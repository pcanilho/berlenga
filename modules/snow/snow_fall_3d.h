/**************************************************************************/
/*  snow_fall_3d.h                                                            */
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


#pragma once

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

class GPUParticles3D;
class GPUParticlesCollisionHeightField3D;
class SnowCover3D;

// Falling and blowing snow (docs/ideas/snow.md, stage 3): GPU particles in a box around the camera,
// traced by the path tracer like any particle system.
//
// Falling: the snowfall rate at the camera comes from the SnowCover3D (weather precipitation times
// the Jennings snow share at that height). Flakes follow the Gunn and Marshall (1958) exponential
// size distribution N(D) = N0 exp(-L D), N0 = 3.8e3 R^-0.87 m-3 mm-1, L = 2.55 R^-0.48 mm-1, counted
// above min_flake_size, so light snow is a few large-looking flakes and heavy snow a dense field.
// They fall at the Locatelli and Hobbs (1974) speed of dendrite aggregates, v = 0.8 D^0.16 m/s,
// carried by the weather map's wind at their height (gusts, shelter) and fluttering on zigzag and
// spiral paths of a few centimetres (Nemes et al. 2017). A heightfield collider following the
// camera stops them under roofs and trees. Sub-pixel flakes keep their energy: they are drawn at
// least a pixel wide with alpha falling as the square of the size ratio.
//
// Blowing: above the Li and Pomeroy (1997) threshold wind for dry snow,
// u_t = 9.43 + 0.18 T + 0.0033 T^2 m/s at 10 m, grains hop in a saltation layer a few centimetres
// deep (Pomeroy and Gray 1990) and finer snow is lifted into a suspension layer, both moving with
// the logarithmic wind profile, only over loose dry snow. The amount follows the saltation flux,
// which grows as u*^3 (1 - u_t^2 / u^2) (Pomeroy 1989).
class WeatherMap3D;

class SnowFall3D : public Node3D {
	GDCLASS(SnowFall3D, Node3D);

	NodePath snow_cover;
	NodePath follow_target;
	bool falling_enabled = true;
	Vector3 fall_extent = Vector3(16, 10, 16);
	int max_flakes = 120000;
	float min_flake_size = 1.5f; // mm
	float flake_size_scale = 1.0f;
	float density_scale = 1.0f;
	float streak_time = 1.0f / 60.0f; // s, the exposure the streaks show
	float snowfall_rate_override = -1.0f; // mm/h water, < 0: from the cover
	bool shelter_enabled = true;
	uint32_t shelter_mask = 0xFFFFFFFF;

	bool blowing_enabled = true;
	float blow_extent = 40.0f;
	int max_blowing = 30000;
	float blowing_strength = 1.0f;
	float threshold_wind_override = -1.0f; // m/s at 10 m, < 0: Li and Pomeroy

	GPUParticles3D *fall = nullptr;
	GPUParticles3D *blow = nullptr;
	GPUParticlesCollisionHeightField3D *shelter = nullptr;
	Ref<ShaderMaterial> fall_process;
	Ref<ShaderMaterial> blow_process;
	Ref<Shader> fall_shader;
	Ref<Shader> blow_shader;
	Ref<StandardMaterial3D> flake_material;
	Ref<StandardMaterial3D> grain_material;

	static constexpr int GROUND_N = 64;
	Ref<Image> ground_image;
	Ref<ImageTexture> ground_texture;
	Vector3 ground_center;
	bool ground_valid = false;
	double ground_accum = 0.0;

	float cur_rate = 0.0f;
	float cur_ratio = 0.0f;
	float cur_blow = 0.0f;
	float cur_threshold = 0.0f;
	float cur_wind10 = 0.0f;
	float cur_blow_sigma = 0.0f; // per m at 1 m above the snow
	float cur_blow_scale = 0.0f; // m, e-folding height of the blowing-snow haze
	float pushed_fall = -1.0f;
	float pushed_blow = -1.0f;
	float pushed_scale = -1.0f;
	Vector3 pushed_base;
	bool haze_enabled = true;
	float turbulence = 1.0f;
	double turb_clock = 0.0;
	void _push_haze(WeatherMap3D *p_weather);
	void _clear_haze();

	SnowCover3D *_cover() const;
	Vector3 _follow_position() const;
	void _ensure_children();
	void _update_ground(Vector3 p_center);
	void _update(double p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_snow_cover(const NodePath &p_path) { snow_cover = p_path; }
	NodePath get_snow_cover() const { return snow_cover; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_falling_enabled(bool p_enabled);
	bool is_falling_enabled() const { return falling_enabled; }
	void set_fall_extent(const Vector3 &p_extent);
	Vector3 get_fall_extent() const { return fall_extent; }
	void set_max_flakes(int p_count);
	int get_max_flakes() const { return max_flakes; }
	void set_min_flake_size(float p_mm) { min_flake_size = CLAMP(p_mm, 0.2f, 20.0f); }
	float get_min_flake_size() const { return min_flake_size; }
	void set_flake_size_scale(float p_scale) { flake_size_scale = MAX(p_scale, 0.01f); }
	float get_flake_size_scale() const { return flake_size_scale; }
	void set_streak_time(float p_seconds) { streak_time = CLAMP(p_seconds, 0.0f, 0.1f); }
	float get_streak_time() const { return streak_time; }
	void set_density_scale(float p_scale) { density_scale = MAX(p_scale, 0.0f); }
	float get_density_scale() const { return density_scale; }
	void set_snowfall_rate_override(float p_rate) { snowfall_rate_override = p_rate; }
	float get_snowfall_rate_override() const { return snowfall_rate_override; }
	void set_shelter_enabled(bool p_enabled);
	bool is_shelter_enabled() const { return shelter_enabled; }
	void set_shelter_mask(uint32_t p_mask);
	uint32_t get_shelter_mask() const { return shelter_mask; }
	void set_blowing_enabled(bool p_enabled);
	bool is_blowing_enabled() const { return blowing_enabled; }
	void set_blow_extent(float p_extent);
	float get_blow_extent() const { return blow_extent; }
	void set_max_blowing(int p_count);
	int get_max_blowing() const { return max_blowing; }
	void set_blowing_strength(float p_strength) { blowing_strength = MAX(p_strength, 0.0f); }
	float get_blowing_strength() const { return blowing_strength; }
	void set_threshold_wind_override(float p_speed) { threshold_wind_override = p_speed; }
	float get_threshold_wind_override() const { return threshold_wind_override; }

	float get_snowfall_rate() const { return cur_rate; } // mm/h water at the camera
	int get_visible_flakes() const { return int(cur_ratio * max_flakes); }
	float get_blowing_amount() const { return cur_blow; } // 0..1 of max_blowing
	float get_threshold_wind() const { return cur_threshold; }
	float get_wind_speed_10m() const { return cur_wind10; }
	float get_blowing_extinction() const { return cur_blow_sigma; }
	float get_blowing_height_scale() const { return cur_blow_scale; }
	void set_haze_enabled(bool p_enabled);
	bool is_haze_enabled() const { return haze_enabled; }
	void set_turbulence(float p_scale) { turbulence = MAX(p_scale, 0.0f); }
	float get_turbulence() const { return turbulence; }
	// Suspended blowing snow (Pomeroy and Male 1992): mass concentration (kg/m3) at height z over the snow
	// for a 10 m wind, and the extinction (per m) of that snow (geometric optics, grains of 40 um radius).
	static float blowing_concentration(float p_wind10, float p_height);
	static float blowing_extinction(float p_concentration);
	// The falling snow beyond the particle box, as fog: extinction per metre and visibility in metres.
	float get_extinction() const { return snowfall_extinction(cur_rate); }
	float get_visibility() const { const float s = get_extinction(); return s > 0.0f ? 3.912f / s : 1e9f; }

	// The published relations, for scripts and checks.
	static float flake_number_density(float p_rate_mm_h, float p_min_size_mm); // flakes per m3 above the size, Gunn and Marshall 1958
	static float flake_size_slope(float p_rate_mm_h); // L, per mm
	static float fall_speed(float p_size_mm);
	static float snowfall_extinction(float p_rate_mm_h); // per m // m/s, Locatelli and Hobbs 1974, aggregates of dendrites
	static float blowing_threshold(float p_celsius); // m/s at 10 m, Li and Pomeroy 1997, dry snow
	static float saltation_flux_ratio(float p_wind10, float p_threshold); // Pomeroy 1989, relative to 1.5 x threshold

	SnowFall3D();
};

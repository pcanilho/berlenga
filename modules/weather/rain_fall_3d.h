/**************************************************************************/
/*  rain_fall_3d.h                                                        */
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
class GPUParticlesCollisionHeightField3D;
class WeatherMap3D;

// Falling rain, its splashes, and the drips off leaves and eaves (docs/ideas/rain.md, steps 2 and 4): the sibling of
// SnowFall3D, GPU particles in a box round the camera, traced by the path tracer like any particle system.
//
// Rain: WeatherMap3D.get_rainfall_at() at the camera, the liquid share of the precipitation at that height (the same
// Jennings split SnowFall3D draws the rest of, so rain, sleet and snow hand over in one place). Drops follow Marshall
// and Palmer (1948), N(D) = 8000 exp(-4.1 R^-0.21 D) per m3 per mm, counted above min_drop_size, and fall at Atlas,
// Srivastava and Sekhon's (1973) terminal speed, 9.65 - 10.3 exp(-0.6 D) m/s, faster in thin air (Foote and du Toit
// 1969). Each drop has inertia: it relaxes toward the air's motion plus its terminal fall over v_t / g, so gusts from
// the weather map's one wind field slant and swing the rain as they move the trees and the snow. Drawn as the
// streaks a camera records over streak_time (Garg and Nayar 2006): a cylinder across the streak, transmissive water
// under the path tracer (IOR 1.33) and the environment's light through it in raster.
//
// Splashes: a drop meets the ground's heightfield (roofs, trees, terrain), the snow's surface, or water. On a wet
// surface it splashes as a crown when K = We Oh^-0.4 is past about 2100 (Yarin and Weiss 1995), so drops above about
// 1 mm throw a crown and droplets a few centimetres; smaller ones stay. On water only the impacts' rings show (drawn
// by the water's shader, rain_ripples()) with a brief Worthington jet for the larger drops.
//
// Drips: the canopy's drainage (WeatherMap3D's Rutter canopy store, which keeps dripping after the rain stops) under
// the canopy, and eaves: edges where the surface hit from above drops by more than a metre, fed by the rain on a strip
// of roof behind them and draining for a few minutes after. Drips are the drops that detach from a leaf tip or an
// edge, about 4.5 mm (Tate's law for a pendant drop), falling from rest.
class RainFall3D : public Node3D {
	GDCLASS(RainFall3D, Node3D);

	NodePath follow_target;
	NodePath snow_cover;
	bool falling_enabled = true;
	Vector3 fall_extent = Vector3(24, 14, 24);
	int max_drops = 200000;
	float min_drop_size = 0.5f; // mm
	float drop_size_scale = 1.0f;
	float density_scale = 1.0f;
	float streak_time = 1.0f / 60.0f; // s
	float rainfall_rate_override = -1.0f; // mm/h, < 0: from the weather
	bool shelter_enabled = true;
	uint32_t shelter_mask = 0xFFFFFFFF;
	bool splashes_enabled = true;
	bool haze_enabled = true;
	float turbulence = 1.0f;

	bool drips_enabled = true;
	int max_drips = 20000;
	float drip_extent = 32.0f; // m, the square searched for eaves and canopy
	float drip_size = 4.5f; // mm
	float eave_drop = 1.0f; // m, an edge drop this tall is an eave
	float roof_depth = 2.0f; // m of roof draining to each metre of eave
	float roof_drain_time = 300.0f; // s, how long a roof keeps draining after the rain
	uint32_t drip_collision_mask = 0xFFFFFFFF;

	GPUParticles3D *fall = nullptr;
	GPUParticles3D *drip = nullptr;
	GPUParticlesCollisionHeightField3D *shelter = nullptr;
	Ref<ShaderMaterial> fall_process;
	Ref<ShaderMaterial> drip_process;
	Ref<ShaderMaterial> drop_material;
	Ref<Shader> process_shader;
	Ref<Shader> draw_shader;

	// The stop height round the camera: the snow's surface and still or running water, whichever is higher (the
	// GPU heightfield collider catches the rest).
	static constexpr int GROUND_N = 64;
	Ref<Image> ground_image;
	Ref<ImageTexture> ground_texture;
	Vector3 ground_center;
	bool ground_valid = false;
	double ground_accum = 0.0;
	LocalVector<ObjectID> found_water;
	double scan_accum = 0.0;
	bool scanned = false;
	void _scan_scene();

	// Drip sources: up to MAX_SOURCES points (x, y, z, cumulative weight), eaves and canopy.
	static constexpr int MAX_SOURCES = 1024;
	Ref<Image> source_image;
	Ref<ImageTexture> source_texture;
	int source_count = 0;
	float drip_rate = 0.0f; // drops per second over all sources
	float roof_rate = 0.0f; // mm/h reaching the eaves (rain, lagged by the roof)
	double source_accum = 1e9;
	Vector3 source_center;

	float cur_rate = 0.0f;
	float published_fallback = -1.0f; // the weather_rain_fallback last set (-1 none)
	void _publish_fallback(float p_rate);
	float cur_share = 1.0f;
	float cur_ratio = 0.0f;
	float cur_weight = 1.0f;
	float pushed_sigma = -1.0f;
	double turb_clock = 0.0;

	WeatherMap3D *_weather() const;
	Node *_snow_cover() const;
	Vector3 _follow_position() const;
	void _ensure_children();
	void _update_ground(Vector3 p_center);
	void _update_sources(Vector3 p_center, double p_delta);
	void _update(double p_delta);
	void _push_haze();
	void _clear_haze();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_snow_cover(const NodePath &p_path) { snow_cover = p_path; }
	NodePath get_snow_cover() const { return snow_cover; }
	void set_falling_enabled(bool p_enabled);
	bool is_falling_enabled() const { return falling_enabled; }
	void set_fall_extent(const Vector3 &p_extent);
	Vector3 get_fall_extent() const { return fall_extent; }
	void set_max_drops(int p_count);
	int get_max_drops() const { return max_drops; }
	void set_min_drop_size(float p_mm) { min_drop_size = CLAMP(p_mm, 0.1f, 5.0f); }
	float get_min_drop_size() const { return min_drop_size; }
	void set_drop_size_scale(float p_scale) { drop_size_scale = MAX(p_scale, 0.01f); }
	float get_drop_size_scale() const { return drop_size_scale; }
	void set_density_scale(float p_scale) { density_scale = MAX(p_scale, 0.0f); }
	float get_density_scale() const { return density_scale; }
	void set_streak_time(float p_seconds) { streak_time = CLAMP(p_seconds, 0.0f, 0.1f); }
	float get_streak_time() const { return streak_time; }
	void set_rainfall_rate_override(float p_rate) { rainfall_rate_override = p_rate; }
	float get_rainfall_rate_override() const { return rainfall_rate_override; }
	void set_shelter_enabled(bool p_enabled);
	bool is_shelter_enabled() const { return shelter_enabled; }
	void set_shelter_mask(uint32_t p_mask);
	uint32_t get_shelter_mask() const { return shelter_mask; }
	void set_splashes_enabled(bool p_enabled) { splashes_enabled = p_enabled; }
	bool is_splashes_enabled() const { return splashes_enabled; }
	void set_haze_enabled(bool p_enabled);
	bool is_haze_enabled() const { return haze_enabled; }
	void set_turbulence(float p_scale) { turbulence = MAX(p_scale, 0.0f); }
	float get_turbulence() const { return turbulence; }
	void set_drips_enabled(bool p_enabled);
	bool is_drips_enabled() const { return drips_enabled; }
	void set_max_drips(int p_count);
	int get_max_drips() const { return max_drips; }
	void set_drip_extent(float p_extent) { drip_extent = CLAMP(p_extent, 4.0f, 256.0f); }
	float get_drip_extent() const { return drip_extent; }
	void set_drip_size(float p_mm) { drip_size = CLAMP(p_mm, 1.0f, 8.0f); }
	float get_drip_size() const { return drip_size; }
	void set_eave_drop(float p_m) { eave_drop = MAX(p_m, 0.1f); }
	float get_eave_drop() const { return eave_drop; }
	void set_roof_depth(float p_m) { roof_depth = MAX(p_m, 0.0f); }
	float get_roof_depth() const { return roof_depth; }
	void set_roof_drain_time(float p_s) { roof_drain_time = MAX(p_s, 1.0f); }
	float get_roof_drain_time() const { return roof_drain_time; }
	void set_drip_collision_mask(uint32_t p_mask) { drip_collision_mask = p_mask; }
	uint32_t get_drip_collision_mask() const { return drip_collision_mask; }

	float get_rainfall_rate() const { return cur_rate; } // mm/h liquid at the camera
	float get_rain_share() const { return cur_share; }
	int get_visible_drops() const { return int(cur_ratio * max_drops); }
	float get_drop_weight() const { return cur_weight; } // drops each drawn streak stands for
	int get_drip_source_count() const { return source_count; }
	float get_drip_rate() const { return drip_rate; } // drops per second near the camera
	float get_extinction() const { return rain_extinction(cur_rate); }
	float get_visibility() const {
		const float s = get_extinction();
		return s > 0.0f ? 3.912f / s : 1e9f;
	}

	// The published relations, for scripts and checks.
	static float drop_size_slope(float p_rate_mm_h); // Marshall-Palmer Lambda, per mm
	static float drop_number_density(float p_rate_mm_h, float p_min_size_mm); // drops per m3 above the size
	static float terminal_velocity(float p_size_mm); // m/s at sea level, Atlas et al. 1973
	static float impact_flux(float p_rate_mm_h, float p_min_size_mm, float p_max_size_mm); // drops per m2 per s
	static float rain_extinction(float p_rate_mm_h); // per m, geometric optics (extinction efficiency 2)
	static float splash_parameter(float p_size_mm, float p_speed); // K = We Oh^-0.4
	static float drips_per_second(float p_rate_mm_h, float p_area_m2, float p_drop_mm); // drops a draining area sheds

	RainFall3D();
};

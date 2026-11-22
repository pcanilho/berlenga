/**************************************************************************/
/*  weather_clouds.h                                                      */
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

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "scene/resources/texture.h"

// The cloud layer of the weather map: one density field, after Guerrilla's Nubis (Schneider 2015,
// 2017) with Frostbite's measured extinction (Hillaire 2016). The same function runs on the CPU
// (cloud shadow bake, get_cloud_density_at) and in WeatherSkyMaterial (WEATHER_CLOUDS_GLSL), so the
// clouds in the sky and their shadows on the ground agree. See docs/ideas/weather.md, step 3.
namespace WeatherClouds {

constexpr int MAX_STORMS = 8;
constexpr int SHAPE_SIZE = 64; // texels across the base shape noise (tiles)
constexpr int DETAIL_SIZE = 32; // texels across the detail noise (tiles)

// Everything the density needs, as WeatherMap3D last eased it.
struct State {
	float base_height = 1500.0f; // m above y = 0
	float thickness = 1500.0f; // m, of cumulus (type 0.7); stratus is a quarter, cumulonimbus five times
	float density = 1.0f; // scale on the measured extinction
	float cover = 0.4f;
	float type = 0.3f;
	float variation = 0.35f;
	float cover_scale = 6000.0f; // m, the cover noise (WeatherMap3D::cloud_scale)
	float shape_scale = 3000.0f; // m, one tile of the shape noise
	Vector2 drift; // m, how far the clouds have moved with the wind aloft
	int storm_count = 0;
	float storms[MAX_STORMS][4] = {}; // x, z, radius of the cloud shield, cover times growth
	float top_max = 0.0f; // highest top any column reaches now (m), set by update_top_max()

	void update_top_max();
};

// Cloud top over the base for a cloud type: stratus a quarter of the thickness, cumulus (0.7) all of
// it, cumulonimbus towers (type above 0.8) up to five times.
float top_height(const State &p_state, float p_type);
// The weather map's cover noise: four octaves of hashed value noise over (p - drift) / scale, 0..1.
float cover_noise(Vector2 p_xz, Vector2 p_drift, float p_scale);
// Cover and type at world x, z: the weather map's drifting noise plus storms.
Vector2 cover_type_at(const State &p_state, Vector2 p_xz);
// Extinction (1/m) at a world point, base shape only (the sky adds detail erosion at the edges).
// Where a world point reads the shape noise (stretched twice vertically).
Vector3 shape_coord(const State &p_state, const Vector3 &p_pos);
float extinction_at(const State &p_state, const Vector3 &p_pos);

// The tiling shape noises (Perlin-Worley base, Worley detail), made once.
const Vector<float> &shape_noise();
const Vector<float> &detail_noise();
Ref<Texture3D> shape_texture();
Ref<Texture3D> detail_texture();
void free_textures();

// GLSL for the sky shader: the same cover, type and density as the functions above.
extern const char *GLSL;

} // namespace WeatherClouds

/**************************************************************************/
/*  vegetation_light.h                                                    */
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

#include "core/math/math_funcs.h"

class TerrainDataMap3D;

// The light a plant grew in (docs/ideas/grass.md, "Sun and shade"): the share of open-sky light
// it gets (1 - canopy shade) times how sunny its slope is against the map's mean (1 on the flat or
// without climate). Packed into instance custom data as 0..1.5 in 1/100 steps.
namespace VegetationLight {

constexpr float LIGHT_MAX = 1.5f;

// The map's mean annual shortwave (TerrainDataMap3D climate), 0 without climate.
float sun_mean(const TerrainDataMap3D *p_map);

// Sun exposure of a point against the map's mean: 1 without climate.
float exposure(const TerrainDataMap3D *p_map, float p_sun_mean, float p_x, float p_z);

// 1..100: 0 is left for "no light recorded" (instances of other systems that only carry a whole
// number there), which the shader reads as open ground.
inline float pack(float p_light) {
	return MAX(1.0f, Math::round(CLAMP(p_light / LIGHT_MAX, 0.0f, 1.0f) * 100.0f));
}

// Sun and shade leaves (Lichtenthaler et al. 2007): leaves grown in shade carry more chlorophyll
// b and fewer carotenoids, so read darker and bluer green; leaves in full sun on a sunny slope carry
// more xanthophylls and dry first, so read paler and yellower. Open flat ground is unchanged.
static constexpr const char *SHADER_CODE = R"(
vec3 vegetation_light_tint(vec3 c, float light) {
	float shade = 1.0 - clamp(light, 0.0, 1.0);
	float sun = smoothstep(1.0, 1.3, light);
	c *= mix(vec3(1.0), vec3(0.72, 0.85, 0.84), shade);
	return mix(c, c * vec3(1.12, 1.05, 0.74), sun);
}
)";

} // namespace VegetationLight

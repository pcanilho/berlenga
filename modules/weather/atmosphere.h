/**************************************************************************/
/*  atmosphere.h                                                          */
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

#include "core/io/image.h"
#include "core/math/vector3.h"

// The atmosphere of Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
// (EGSR 2020), on Bruneton and Neyret's Earth (2008): Rayleigh and Mie scattering with exponential
// profiles, an ozone layer that absorbs (the blue of twilight), multiple scattering of any order folded
// into a small table (the paper's Psi_ms), and the transmittance table indexed as in Bruneton 2017.
// The tables are baked here on the CPU (they change only with the atmosphere's parameters); the sky
// shader marches the view ray against them for any sun angle, so day, golden hour, twilight with the
// Earth's shadow and night all come from the same air. The same transmittance gives the colour and
// strength of the sun and moon light at the ground (SkyClock).
namespace WeatherAtmosphere {

struct Params {
	double bottom_radius = 6360e3; // m
	double top_radius = 6460e3;
	Vector3 rayleigh_scattering = Vector3(5.802e-6, 13.558e-6, 33.1e-6); // 1/m at sea level
	double rayleigh_scale_height = 8000.0;
	double mie_scattering = 3.996e-6;
	double mie_extinction = 4.440e-6;
	double mie_scale_height = 1200.0;
	double mie_g = 0.8;
	Vector3 ozone_absorption = Vector3(0.650e-6, 1.881e-6, 0.085e-6);
	double ozone_center = 25000.0; // m: a tent from 10 to 40 km
	double ozone_half_width = 15000.0;
	Vector3 ground_albedo = Vector3(0.3, 0.3, 0.3);
};

constexpr int TRANSMITTANCE_W = 256;
constexpr int TRANSMITTANCE_H = 64;
constexpr int MULTISCATTER_N = 32;

// Transmittance from radius r (m, from the planet's centre) along a ray at cosine mu from the zenith
// to the top of the atmosphere; 0 where the ray meets the ground.
Vector3 transmittance(const Params &p_params, double p_r, double p_mu);
Ref<Image> bake_transmittance(const Params &p_params);
Ref<Image> bake_multiscatter(const Params &p_params);

// The GLSL that reads the tables (uniforms and functions, for a sky shader's global scope).
extern const char *GLSL;

} // namespace WeatherAtmosphere

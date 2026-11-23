/**************************************************************************/
/*  physx_dust_optics.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the       */
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

#include "physx_flow_3d.h"

#include "modules/modules_enabled.gen.h" // MODULE_WEATHER_ENABLED

#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

// Mineral dust optics for a Flow volume (dust devils, haboob billows): the weather's one dust albedo
// (WeatherMap3D.get_dust_albedo(), the weather_dust_albedo global the sky and haze use), per scatter. Flow weighs every
// scatter by the smoke colour times the scattering albedo, so the albedo takes the brightest channel and the colour the
// rest, times p_tint (white: the weather's dust as it is). The asymmetry is the same value's.
inline void physx_apply_dust_optics(PhysXFlow3D *p_flow, const Color &p_tint) {
	if (!p_flow) {
		return;
	}
#ifdef MODULE_WEATHER_ENABLED
	const Vector4 a = WeatherMap3D::get_dust_albedo();
#else
	const Vector4 a(0.93f, 0.92f, 0.85f, 0.75f);
#endif
	const float m = MAX(MAX(a.x, a.y), MAX(a.z, 1e-3f));
	p_flow->set_scattering_albedo(m);
	p_flow->set_smoke_color(Color(a.x / m * p_tint.r, a.y / m * p_tint.g, a.z / m * p_tint.b));
	p_flow->set_scattering_anisotropy(a.w);
}

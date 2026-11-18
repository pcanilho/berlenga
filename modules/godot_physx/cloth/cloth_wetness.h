/**************************************************************************/
/*  cloth_wetness.h                                                       */
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

#include "core/math/aabb.h"
#include "core/math/vector3.h"

class Node3D;

// Rain soaking fabric, and drying (docs/ideas/physx.md, "Rain soaks cloth"), shared by PhysXCloth3D and
// PhysXSkinnedCloth3D. Rain from the active WeatherMap3D (its liquid share, the canopy, a roof ray above the cloth)
// times the cloth's catch: rain caught per area of fabric over rain per area of ground, from how the fabric faces drops
// falling at about 6.5 m/s (2 mm drops, Gunn and Kinzer 1949) slanted by the wind and scattered by its gusts. Each face
// evaporates as open water does, by Penman's (1948) wind function, 0.26 (1 + 0.54 U) mm a day per hPa of vapour
// pressure deficit, slowing as the last water is held in the fibres (the falling rate period of drying).
class ClothWetness {
public:
	float wetness = 0.0f; // 0 dry, 1 soaked
	float rain_catch = 1.0f; // set by the owner when asked (r_update_catch)
	float rain_shelter = 1.0f; // 0 under a roof
	float rain_timer = 0.0f;

	static constexpr float DROP_SPEED = 6.5f; // m/s

	// Four drop directions around the wind's slant (gust scatter of 0.3 U + 0.5 m/s), and their mean |y|.
	static void rain_directions(const Vector3 &p_wind, Vector3 r_dirs[4], float &r_mean_dy);
	// Advances the water held. p_capacity is kg/m2 when soaked. r_update_catch is set when it rains and the owner should
	// refresh rain_catch (twice a second).
	void step(Node3D *p_node, const AABB &p_world_box, const Vector3 &p_wind, float p_dt, float p_capacity, bool p_weather, bool &r_update_catch);
};

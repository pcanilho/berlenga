/**************************************************************************/
/*  render_raytracing_volumes.h                                           */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/templates/rid.h"

// Simulated gas the path tracer renders (raytracing_volume_inc.glsl): a sparse block atlas in
// NVIDIA Flow's layout, published every frame from the render thread by whatever simulates it
// (modules/godot_physx PhysXFlow3D), before the trace (a pre-opaque compositor callback), and
// valid until the frame's compositor effects end.
struct RTSparseVolume {
	RID density; // 3D texture: r temperature, a smoke
	RID table; // storage buffer: the sparse table
	uint32_t level[40] = {}; // NvFlowSparseLevelParams of the density level
	Vector3 world_min; // simulation units
	Vector3 world_max;
	Vector3 block_size; // simulation units
	int layer = 0;
	float units_per_meter = 1.0;
	float extinction = 4.0; // per meter at smoke density 1
	float albedo = 0.9; // single scattering albedo
	float anisotropy = 0.3; // Henyey-Greenstein g
	float density_majorant = 1.0; // smoke density the extinction is clamped at
	Color smoke_color = Color(1, 1, 1);
	float emission_scale = 0.0; // 0: not emissive
	float temperature_range = 1.0;
	Color heat_ramp[8];
};

class RTVolumes {
	static RTSparseVolume volume;
	static bool present;

public:
	// Render thread. One volume per frame for now (the largest simulation in view).
	static void set_volume(const RTSparseVolume &p_volume);
	static void clear();
	static bool get_volume(RTSparseVolume &r_volume);
};

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
#include "core/math/vector3i.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
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

// A dense grid of density in a box, for media simulated on small grids of their own (waterfall mist,
// PhysXWaterMist3D): up to MAX_BOXES boxes, each a slab of one shared atlas texture stacked along z
// (density in r and a, a zero texel between slabs).
struct RTDenseBox {
	Vector3 origin; // world meters, the box's min corner
	float cell = 1.0; // meters per texel
	Vector3i size; // texels
	int atlas_z = 0; // first slab texel along the atlas's z
};

struct RTDenseVolumes {
	static constexpr int MAX_BOXES = 5;
	RID texture; // RenderingServer texture (Texture3D), resolved to its RD texture by the trace
	LocalVector<RTDenseBox> boxes;
	float extinction = 0.5; // per meter at density 1
	float albedo = 0.99;
	float anisotropy = 0.8;
	float max_density = 1.0; // the densest texel: the majorant's density
	Color color = Color(1, 1, 1);
};

class RTVolumes {
	static RTSparseVolume volume;
	static bool present;
	static RTDenseVolumes dense;
	static Mutex dense_mutex;

public:
	// Render thread. One volume per frame for now (the largest simulation in view).
	static void set_volume(const RTSparseVolume &p_volume);
	static void clear();
	static bool get_volume(RTSparseVolume &r_volume);
	// Any thread; kept until replaced. Traced on frames with no simulated volume published (a frame
	// with one traces that instead).
	static void set_dense(const RTDenseVolumes &p_dense);
	static bool get_dense(RTDenseVolumes &r_dense);
};

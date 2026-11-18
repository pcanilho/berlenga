/**************************************************************************/
/*  flow_shimmer_rd.h                                                     */
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

#include "flow_backend.h"

#include "core/math/projection.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device.h"

// PhysXFlow3D.heat_shimmer in the raster renderers: light bending through the gas's hot air
// (docs/ideas/heat-shimmer.md). Each pixel's view ray is marched through Flow's sparse temperature
// (the volume and table of FlowBackend::ScatterSource), the gradient of the air's refractive index
// across it is integrated (Gladstone-Dale; the eikonal ray equation), and the pixel takes the scene
// colour where the bent ray lands. Path tracing bends the rays themselves (raytracing_volume_inc.glsl,
// rt_volume_bend) from the same field and constants.
class FlowShimmerRD {
public:
	struct Params {
		Projection inv_view_projection; // clip space -> world meters (as the depth buffer has it)
		Vector3 camera_position; // meters
		float kelvin = 1200.0f; // kelvin above the air at a Flow temperature of temperature_range
		float temperature_range = 1.0f;
		float air_kelvin = 288.15f; // the air around the gas
		float turbulence = 0.35f; // rms temperature fluctuation below the grid, as a share of the excess
		float time = 0.0f; // seconds, moves the sub-grid turbulence
	};

private:
	RID shader;
	RID pipeline;
	RID params_buffer;
	RID linear_sampler;
	RID point_sampler;
	RID source_copy; // the frame before the shimmer, sampled at the bent rays' landing points
	Size2i source_size;
	bool failed = false;

	bool _ensure(RenderingDevice *p_rd);

public:
	// The texture the caller copies the frame into before run() (RGBA16F, storage and sampled).
	RID get_source_copy(RenderingDevice *p_rd, const Size2i &p_size);
	// p_color: the frame (rgba16f storage) with the gas composited, after get_source_copy() was filled from
	// it; p_depth: the scene depth. Only the pixels inside the gas's screen rectangle are touched.
	void run(RenderingDevice *p_rd, const FlowBackend::ScatterSource &p_source, const Params &p_params, const RID &p_depth, const RID &p_color, const Size2i &p_size);
	void free(RenderingDevice *p_rd);
};

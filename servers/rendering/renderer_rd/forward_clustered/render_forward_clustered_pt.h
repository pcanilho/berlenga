/**************************************************************************/
/*  render_forward_clustered_pt.h                                         */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/shaders/effects/rt_fog_filter.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/transparency_composite.glsl.gen.h"

namespace RendererSceneRenderImplementation {

// Path-tracing / DLSS variant of the clustered renderer. Inherits all of the
// raster setup and resource management from RenderForwardClustered and overrides
// only the scene render entry points to add the raytraced opaque path, DLSS Ray
// Reconstruction, and the associated debug visualizations.
class RenderForwardClusteredPT : public RenderForwardClustered {
	/* Raytracing */

	RenderRaytracing *raytracing = nullptr;

	bool _setup_rt();
	void _age_out_motion_vectors(const RenderDataRD *p_render_data);

	/* Transparency over the denoised frame */

	// Ray Reconstruction takes the raster-lit transparent surfaces drawn into
	// its input for noise its guides do not explain, and averages small ones,
	// particles, away. Under it they are drawn into a layer of their own and
	// laid over its output.
	struct {
		TransparencyCompositeShaderRD shader;
		RID version;
		RID pipeline;
	} transparency_composite;

	RID _transparency_layer_fb(RenderSceneBuffersRD *p_render_buffers);
	void _composite_transparency(const RenderDataRD *p_render_data, bool p_fog, bool p_layer_in_output);

	// The traced volumetric fog's filter: a blur kept to like depths, then an average over
	// frames into one of two histories, swapped each frame.
	struct {
		RtFogFilterShaderRD shader;
		RID version;
		RID pipeline;
	} fog_filter;
	uint32_t fog_frame = 0;
	RID _filter_fog(const RenderDataRD *p_render_data);

protected:
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) override;

public:
	RenderForwardClusteredPT();
	~RenderForwardClusteredPT();
};

} // namespace RendererSceneRenderImplementation

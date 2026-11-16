/**************************************************************************/
/*  flow_scatter_rd.h                                                     */
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

#include "core/math/basis.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device.h"

// PhysXFlow3D's light scattering: point and spot lights glowing in the gas.
// Flow's own renderer lights the gas with one directional light (its
// self-shadow volume), so this is a second pass of our own, run right after
// Flow composites the gas into the frame: it marches each pixel's view ray
// through Flow's sparse density (the same volume and table Flow's ray marcher
// read, see FlowBackend::ScatterSource), and at each step with smoke adds
// what the nearby lights scatter toward the camera -- each light's color and
// energy with Godot's own distance (and spot cone) falloff, dimmed by the gas
// between it and the point (a short march toward the light) and by the gas
// between the point and the camera. Ambient and sky light (Params::ambient)
// are added the same way, dimmed by the gas above each point. The sum is added
// onto the scene color.
class FlowScatterRD {
public:
	static constexpr int MAX_LIGHTS = 16;

	// std430, 160 bytes.
	struct Light {
		float position_range[4] = {}; // meters; w: range
		float color_decay[4] = {}; // linear rgb * energy; w: attenuation exponent
		float direction_cutoff[4] = {}; // spot: direction it points, w: cos(spot angle); omni: w = -2
		float spot[4] = {}; // x: spot attenuation exponent
		// Its shadow in Godot's shadow atlas, as Godot's own volumetric fog reads
		// it (see LightStorage::update_light_buffers), but from world space:
		// omni: world -> light local; spot: world -> atlas projection.
		float shadow_matrix[16] = {};
		float atlas_rect[4] = {};
		float shadow[4] = {}; // x: opacity (0: no shadow), y: bias, zw: omni dual-paraboloid flip offset
	};

	struct Params {
		Projection inv_view_projection; // clip space -> world meters
		Vector3 camera_position; // meters
		float sigma = 4.0f; // extinction per meter per unit smoke (Flow's attenuation)
		float color[3] = { 0.8f, 0.8f, 0.8f }; // the smoke's albedo
		float strength = 1.0f;
		float anisotropy = 0.4f; // Henyey-Greenstein g
		float step_scale = 1.0f; // view step, in cells
		int shadow_steps = 8; // toward each light; 0: no self-shadow
		float shadow_reach = 4.0f; // meters
		int max_steps = 256;
		bool shadows = true; // sample the lights' shadows (scene objects block their glow)
		int resolution_divisor = 4; // 1: every pixel; 2: half resolution each way; 4: quarter
		// Ambient light in the gas, as Godot's volumetric fog takes it: the ambient color mixed with the
		// sky's radiance by sky_contribution, scaled by the ambient energy. It is dimmed by the gas above
		// each point (a short march up: how much of the sky it sees).
		bool ambient = false;
		float ambient_color[3] = {}; // linear rgb * ambient energy
		float sky_contribution = 0.0f;
		float sky_energy = 0.0f; // the ambient energy, for the sky part
		RID sky_radiance; // the sky's radiance octmap (RID(): none)
		float sky_border[2] = { 0.0f, 1.0f }; // the octmap's uv border (SkyRD::sky_get_uv_border_size)
		Basis sky_inverse; // world direction -> the sky's own orientation
		// The frame's directional lights (Godot's own light buffer, in view space) light the gas here
		// instead of in Flow's colormap, with their shadow cascades (walls shade the smoke).
		int directional_count = 0; // 0: none (Flow's colormap carries the sun)
		bool directional_shadows = true;
		RID directional_buffer; // LightStorage::get_directional_light_buffer()
		RID directional_shadow_atlas; // LightStorage::directional_shadow_get_texture()
		Transform3D world_to_view; // meters
		// Blend each frame's scattered light with the last frames' (reprojected, clamped to this
		// frame's neighbourhood), moving the march's start offset every frame: no step banding.
		bool temporal = false;
	};

	// The gas's volumetric shadow map from the sun (it shadows the scene, and the scattering pass lights the
	// gas from the sun through it): asked for with enabled and
	// to_light; run() fills texture and world_to_shadow for LightStorage::directional_volume_shadow_set.
	struct ShadowMap {
		bool enabled = false;
		Vector3 to_light; // world direction toward the light
		int resolution = 128;
		int steps = 64; // along the light
		int slices = 32; // depth slices of optical depth
		RID texture;
		Transform3D world_to_shadow;
	};

private:
	RID shader;
	RID shadow_shader;
	RID shadow_pipeline;
	// A volumetric shadow map's own resources: the sun's, and the sky's (looking straight down).
	struct MapSlot {
		RID box_buffer;
		RID texture;
		int res = 0;
		int slices = 0;
		float box_data[20] = {}; // the ShadowBox block: origin, axes, then (res, steps, slices)
	};
	MapSlot sun_slot;
	MapSlot sky_slot;
	RID pipeline;
	RID upsample_shader;
	RID upsample_pipeline;
	// The scattered light at the scattering resolution (rgb) + scene distance (a).
	RID scatter_texture;
	Size2i scatter_size;
	// Temporal blending: two history textures at the scattering resolution, used in turn.
	RID temporal_shader;
	RID temporal_pipeline;
	RID history[2];
	int history_index = 0;
	Size2i history_size;
	bool history_valid = false;
	uint64_t temporal_frame = UINT64_MAX;
	Projection prev_view_projection;
	RID linear_sampler;
	RID point_sampler;
	RID shadow_sampler;
	RID sky_sampler;
	RID params_buffer;
	RID lights_buffer;
	RID directional_dummy;
	bool failed = false;
	bool sky_array = false; // the shader reads the sky as a 2D array (Godot's radiance octmap array)

	bool _ensure(RenderingDevice *p_rd);
	bool _fit_shadow(const FlowBackend::ScatterSource &p_source, ShadowMap &r_shadow, MapSlot &r_slot);
	void _run_shadow(RenderingDevice *p_rd, const LocalVector<RD::Uniform> &p_uniforms, ShadowMap &r_shadow, MapSlot &r_slot);

public:
	// After Flow composited into p_color (rgba16f, storage); p_depth is the
	// scene depth it was drawn against.
	// p_shadow_atlas: the frame's shadow atlas (RID() if there is none; lights
	// then glow unshadowed).
	void run(RenderingDevice *p_rd, const FlowBackend::ScatterSource &p_source, const Params &p_params, const Light *p_lights, int p_light_count, const RID &p_depth, const RID &p_color, const RID &p_shadow_atlas, const Size2i &p_size, ShadowMap *r_shadow = nullptr);
	void free(RenderingDevice *p_rd);
};

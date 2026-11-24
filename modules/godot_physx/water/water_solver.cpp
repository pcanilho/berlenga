/**************************************************************************/
/*  water_solver.cpp                                                      */
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

#include "water_solver.h"

#include "water_blit_ocean.glsl.gen.h"
#include "water_blit_ripple.glsl.gen.h"
#include "water_caustics_map.glsl.gen.h"
#include "water_fft.glsl.gen.h"
#include "water_fft_rows.glsl.gen.h"
#include "water_flow_offset.glsl.gen.h"
#include "water_surface_waves.glsl.gen.h"
#include "water_foam.glsl.gen.h"
#include "water_ripple.glsl.gen.h"
#include "water_shore_foam.glsl.gen.h"
#include "water_spectrum_evolve.glsl.gen.h"
#include "water_spectrum_init.glsl.gen.h"
#include "water_swash.glsl.gen.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/memory.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

namespace {
constexpr int PARAMS_BYTES = 80; // 5 * vec4, std140 -- see water_inc.glsl's Params
constexpr int OCEAN_INIT_PARAMS_BYTES = 80; // 5 * vec4 -- see water_spectrum_init.glsl's Params
constexpr int OCEAN_EVOLVE_PARAMS_BYTES = 32; // 2 * vec4 -- see water_spectrum_evolve.glsl's Params
constexpr int FFT_PARAMS_BYTES = 16; // 1 * ivec4 -- see water_fft.glsl's Params
constexpr int FFT_ROWS_MAX_N = 1024; // water_fft_rows.glsl's MAX_N (shared memory); larger grids take the per-stage passes
constexpr int BLIT_PARAMS_BYTES = 16; // 1 * ivec4 -- see water_blit_*.glsl's Params
constexpr int FOAM_PARAMS_BYTES = 32; // vec4 + ivec4 -- see water_foam.glsl's Params
constexpr int SHORE_FOAM_PARAMS_BYTES = 64; // 3 * vec4 + ivec4 -- see water_shore_foam.glsl's Params
constexpr int SWASH_PARAMS_BYTES = 96; // 3 * vec4 + ivec4 + 2 * vec4 -- see water_swash.glsl's Params
constexpr int FLOW_OFFSET_PARAMS_BYTES = 48; // 2 * vec4 + ivec4 -- see water_flow_offset.glsl's Params
constexpr int SURFACE_WAVES_PARAMS_BYTES = 64; // 3 * vec4 + ivec4 -- see water_surface_waves.glsl's Params
constexpr int SURFACE_WAVE_N = 512; // the surface waves' grid (a power of two, <= FFT_ROWS_MAX_N)
constexpr int SURFACE_WAVE_LOG2N = 9;
constexpr int CAUSTICS_PARAMS_BYTES = 112; // 7 * vec4, std140 -- see water_caustics_map.glsl's Params
constexpr uint32_t GROUP = 8; // matches every water_*.glsl's local_size_x/y

uint32_t groups_for(int p_count) {
	return (uint32_t)((p_count + (int)GROUP - 1) / (int)GROUP);
}

int log2i(int p_v) {
	int r = 0;
	while ((1 << r) < p_v) {
		r++;
	}
	return r;
}
} // namespace

/* ===================================================================== */
/*  WaterSolverGPU                                                        */
/* ===================================================================== */

WaterSolverGPU::~WaterSolverGPU() {
#ifdef DEV_ENABLED
	// rt_free() should have run first (via WaterSolver's destructor). If a
	// stale async callback kept us alive past it, everything is already
	// null and there is nothing to do -- same discipline as
	// MPMFluidSolverGPU's own destructor.
	DEV_ASSERT(shader_ripple.is_null() && pipeline_ripple.is_null());
#endif
}

void WaterSolverGPU::rt_compile(Ref<WaterSolverGPU> p_self) {
	if (rd == nullptr) {
		return;
	}
	struct Entry {
		const char *src;
		RID *shader;
		RID *pipeline;
	};
	Entry entries[] = {
		{ water_ripple_shader_glsl, &shader_ripple, &pipeline_ripple },
		{ water_spectrum_init_shader_glsl, &shader_spectrum_init, &pipeline_spectrum_init },
		{ water_spectrum_evolve_shader_glsl, &shader_spectrum_evolve, &pipeline_spectrum_evolve },
		{ water_fft_shader_glsl, &shader_fft, &pipeline_fft },
		{ water_fft_rows_shader_glsl, &shader_fft_rows, &pipeline_fft_rows },
		{ water_blit_ripple_shader_glsl, &shader_blit_ripple, &pipeline_blit_ripple },
		{ water_blit_ocean_shader_glsl, &shader_blit_ocean, &pipeline_blit_ocean },
		{ water_foam_shader_glsl, &shader_foam, &pipeline_foam },
		{ water_shore_foam_shader_glsl, &shader_shore_foam, &pipeline_shore_foam },
		{ water_swash_shader_glsl, &shader_swash, &pipeline_swash },
		{ water_flow_offset_shader_glsl, &shader_flow_offset, &pipeline_flow_offset },
		{ water_surface_waves_shader_glsl, &shader_surface_waves, &pipeline_surface_waves },
	};
	for (Entry &e : entries) {
		Ref<RDShaderFile> sf;
		sf.instantiate();
		if (sf->parse_versions_from_text(e.src) != OK) {
			ERR_PRINT("WaterSolver: a compute shader failed to compile.");
			return;
		}
		*e.shader = rd->shader_create_from_spirv(sf->get_spirv_stages());
		ERR_FAIL_COND(e.shader->is_null());
		*e.pipeline = rd->compute_pipeline_create(*e.shader);
		ERR_FAIL_COND(e.pipeline->is_null());
	}

	// Light-space caustic map: a real RD graphics (vertex+fragment) pipeline,
	// not a compute one -- built here alongside the compute shaders above
	// (compiled unconditionally; the grid/buffers it actually draws into are
	// only built in _rt_build_buffers() if caustics_enabled was requested, so
	// an unused pipeline costs nothing beyond this one-time compile).
	{
		Ref<RDShaderFile> sf;
		sf.instantiate();
		if (sf->parse_versions_from_text(water_caustics_map_shader_glsl) != OK) {
			ERR_PRINT("WaterSolver: the caustics map shader failed to compile.");
			return;
		}
		shader_caustics = rd->shader_create_from_spirv(sf->get_spirv_stages());
		ERR_FAIL_COND(shader_caustics.is_null());

		Vector<RD::VertexAttribute> attrs;
		RD::VertexAttribute attr;
		attr.location = 0;
		attr.format = RD::DATA_FORMAT_R32G32_SFLOAT;
		attr.stride = sizeof(float) * 2;
		attrs.push_back(attr);
		caustics_vertex_format = rd->vertex_format_create(attrs);

		Vector<RD::AttachmentFormat> afs;
		RD::AttachmentFormat af;
		af.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT; // R caustics (irradiance ratio, unbounded), G water mask (4 channels: a 2-channel texture got sampled with a luminance-alpha swizzle)
		af.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		afs.push_back(af);
		RD::FramebufferFormatID fb_format = rd->framebuffer_format_create(afs);

		RD::PipelineColorBlendState blend_state;
		RD::PipelineColorBlendState::Attachment blend_attachment;
		blend_attachment.enable_blend = true;
		blend_attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
		blend_attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE;
		blend_attachment.color_blend_op = RD::BLEND_OP_ADD;
		blend_attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
		blend_attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
		blend_attachment.alpha_blend_op = RD::BLEND_OP_ADD;
		blend_state.attachments.push_back(blend_attachment);
		pipeline_caustics = rd->render_pipeline_create(shader_caustics, fb_format, caustics_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), blend_state, 0);
		ERR_FAIL_COND(pipeline_caustics.is_null());

		RD::SamplerState ss;
		ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sampler_linear = rd->sampler_create(ss);
		ERR_FAIL_COND(sampler_linear.is_null());
		ss.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
		ss.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
		sampler_linear_repeat = rd->sampler_create(ss);
		ERR_FAIL_COND(sampler_linear_repeat.is_null());
	}
	shaders_ok = true;
}

void WaterSolverGPU::_rt_free_buffers() {
	_rt_free_surface_waves();
	RID *usets[] = { &uset_atob, &uset_btoa, &uset_spectrum_init, &uset_spectrum_evolve,
		&uset_fft_spec_to_a, &uset_fft_atob, &uset_fft_btoa, &uset_fft_dz_to_c, &uset_fft_ctod, &uset_fft_dtoc,
		&uset_fft_c_to_e, &uset_fft_etof, &uset_fft_ftoe, &uset_fft_d_to_g, &uset_fft_gtoh, &uset_fft_htog, &uset_fft_rows, &uset_fft_cols,
		&uset_blit_ripple, &uset_blit_ocean, &uset_caustics, &uset_foam, &uset_shore_foam, &uset_shore_foam_copy, &uset_swash, &uset_swash_copy, &uset_flow_offset, &uset_flow_offset_copy };
	for (RID *u : usets) {
		// A set is freed with any texture it uses: the swash's goes with the
		// surface's shore wave texture.
		if (u->is_valid() && rd->uniform_set_is_valid(*u)) {
			rd->free_rid(*u);
			*u = RID();
		}
	}
	RID *framebuffers[] = { &caustics_framebuffer };
	for (RID *framebuffer : framebuffers) {
		if (framebuffer->is_valid()) {
			rd->free_rid(*framebuffer);
			*framebuffer = RID();
		}
	}
	RID *arrays[] = { &caustics_vertex_array, &caustics_index_array };
	for (RID *array : arrays) {
		if (array->is_valid()) {
			rd->free_rid(*array);
			*array = RID();
		}
	}
	RID *vertex_index_buffers[] = { &caustics_vertex_buffer, &caustics_index_buffer };
	for (RID *buffer : vertex_index_buffers) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	RID *bufs[] = { &buf_params, &buf_state_a, &buf_state_b, &buf_height, &buf_spheres, &buf_impulses,
		&buf_ocean_init_params, &buf_ocean_evolve_params, &buf_fft_params, &buf_fft_rows_params, &buf_fft_cols_params, &buf_h0, &buf_ocean_spec, &buf_fft_a, &buf_fft_b,
		&buf_ocean_spec_dz, &buf_fft_c, &buf_fft_d, &buf_ocean_spec_c, &buf_fft_e, &buf_fft_f, &buf_ocean_spec_d, &buf_fft_g, &buf_fft_h,
		&buf_blit_ripple_params, &buf_blit_ocean_params, &buf_caustics_params, &buf_foam_params, &buf_foam_cover, &buf_shore_foam_params, &buf_shore_foam_copy_params, &buf_swash_params, &buf_swash_copy_params, &buf_flow_offset_params, &buf_flow_offset_copy_params };
	for (RID *b : bufs) {
		if (b->is_valid()) {
			rd->free_rid(*b);
			*b = RID();
		}
	}
	RID *texs[] = { &tex_ripple_height, &tex_ocean_height, &tex_ocean_disp, &tex_ocean_deriv, &tex_ocean_foam, &tex_shore_foam, &tex_shore_foam_tmp, &tex_caustics, &tex_cell_depth, &tex_ocean_fade, &tex_shore_depth, &tex_swash, &tex_swash_tmp, &tex_base_flux, &tex_velocity, &tex_flow_offset, &tex_flow_offset_tmp };
	for (RID *t : texs) {
		if (t->is_valid()) {
			rd->free_rid(*t);
			*t = RID();
		}
	}
}

void WaterSolverGPU::_rt_build_buffers() {
	const int n = grid_resolution;
	const int cells = n * n;
	const int on = ocean_grid_resolution;
	const int ocells = on * on;
	ocean_log2n = log2i(on);
	// (The ocean clock survives a rebuild: the spectrum is a pure function of its settings, so a re-centre / resize
	// with the same sea carries on seamlessly instead of every wave jumping back to t = 0.)
	a_is_current = true;

	buf_params = rd->uniform_buffer_create(PARAMS_BYTES);
	// Zero-filled: storage_buffer_create() without data leaves the memory
	// uninitialized, and the wave equation reads its previous state -- any
	// NaN left in recycled GPU memory spreads across the whole surface.
	{
		Vector<uint8_t> zeros;
		zeros.resize(cells * 4 * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		buf_state_a = rd->storage_buffer_create(zeros.size(), zeros);
		buf_state_b = rd->storage_buffer_create(zeros.size(), zeros);
		zeros.resize(cells * sizeof(float));
		buf_height = rd->storage_buffer_create(zeros.size(), zeros);
	}
	buf_spheres = rd->storage_buffer_create(MAX_SPHERES * SPHERE_FLOATS * sizeof(float));
	buf_impulses = rd->storage_buffer_create(MAX_IMPULSES * 4 * sizeof(float));

	// Per-cell still-water depth (R16F) and the ocean chop fade derived from
	// it (R8), uploaded once. No per-cell depth = the constant depth, and full
	// chop, everywhere.
	{
		has_cell_depth = _init_cell_depth.size() == cells;
		Vector<uint8_t> depth_bytes;
		Vector<uint8_t> fade_bytes;
		Vector<uint8_t> shore_bytes;
		_rt_depth_bytes(_init_cell_depth, depth_bytes, fade_bytes, shore_bytes);
		RD::TextureFormat depth_tf;
		depth_tf.format = RD::DATA_FORMAT_R16_SFLOAT;
		depth_tf.width = n;
		depth_tf.height = n;
		depth_tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<Vector<uint8_t>> depth_data;
		depth_data.push_back(depth_bytes);
		tex_cell_depth = rd->texture_create(depth_tf, RD::TextureView(), depth_data);

		RD::TextureFormat fade_tf = depth_tf;
		fade_tf.format = RD::DATA_FORMAT_R8_UNORM;
		Vector<Vector<uint8_t>> fade_data;
		fade_data.push_back(fade_bytes);
		tex_ocean_fade = rd->texture_create(fade_tf, RD::TextureView(), fade_data);

		Vector<Vector<uint8_t>> shore_data;
		shore_data.push_back(shore_bytes);
		tex_shore_depth = rd->texture_create(depth_tf, RD::TextureView(), shore_data);

		// Baked current (face fluxes) for the shallow-water passes, and the live velocity they write.
		has_base_flux = _init_base_flux.size() == cells * 4;
		RD::TextureFormat flux_tf = depth_tf;
		flux_tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		Vector<uint8_t> flux_bytes;
		flux_bytes.resize(cells * 4 * sizeof(float));
		if (has_base_flux) {
			memcpy(flux_bytes.ptrw(), _init_base_flux.ptr(), flux_bytes.size());
		} else {
			memset(flux_bytes.ptrw(), 0, flux_bytes.size());
		}
		Vector<Vector<uint8_t>> flux_data;
		flux_data.push_back(flux_bytes);
		tex_base_flux = rd->texture_create(flux_tf, RD::TextureView(), flux_data);

		RD::TextureFormat velocity_tf = depth_tf;
		velocity_tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		velocity_tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> velocity_bytes;
		velocity_bytes.resize(cells * 4 * sizeof(uint16_t));
		memset(velocity_bytes.ptrw(), 0, velocity_bytes.size());
		Vector<Vector<uint8_t>> velocity_data;
		velocity_data.push_back(velocity_bytes);
		tex_velocity = rd->texture_create(velocity_tf, RD::TextureView(), velocity_data);
	}

	auto make_uset = [&](RID p_state_in, RID p_state_out) {
		const RID by_binding[6] = { buf_params, p_state_in, p_state_out, buf_height, buf_spheres, buf_impulses };
		Vector<RD::Uniform> uniforms;
		for (int bnd = 0; bnd < 6; bnd++) {
			RD::Uniform u;
			u.uniform_type = (bnd == 0) ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = bnd;
			u.append_id(by_binding[bnd]);
			uniforms.push_back(u);
		}
		RD::Uniform depth_uniform;
		depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		depth_uniform.binding = 6;
		depth_uniform.append_id(sampler_linear);
		depth_uniform.append_id(tex_cell_depth);
		uniforms.push_back(depth_uniform);
		RD::Uniform flux_uniform;
		flux_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		flux_uniform.binding = 7;
		flux_uniform.append_id(sampler_linear);
		flux_uniform.append_id(tex_base_flux);
		uniforms.push_back(flux_uniform);
		RD::Uniform velocity_uniform;
		velocity_uniform.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		velocity_uniform.binding = 8;
		velocity_uniform.append_id(tex_velocity);
		uniforms.push_back(velocity_uniform);
		return rd->uniform_set_create(uniforms, shader_ripple, 0);
	};
	uset_atob = make_uset(buf_state_a, buf_state_b);
	uset_btoa = make_uset(buf_state_b, buf_state_a);

	// FFT ocean layer.
	buf_ocean_init_params = rd->uniform_buffer_create(OCEAN_INIT_PARAMS_BYTES);
	buf_ocean_evolve_params = rd->uniform_buffer_create(OCEAN_EVOLVE_PARAMS_BYTES);
	buf_fft_params = rd->uniform_buffer_create(FFT_PARAMS_BYTES);
	buf_h0 = rd->storage_buffer_create(ocells * 4 * sizeof(float));
	buf_ocean_spec = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_a = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_b = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_ocean_spec_dz = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_c = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_d = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_ocean_spec_c = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_e = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_f = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_ocean_spec_d = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_g = rd->storage_buffer_create(ocells * 2 * sizeof(float));
	buf_fft_h = rd->storage_buffer_create(ocells * 2 * sizeof(float));

	{
		Vector<RD::Uniform> uniforms;
		const RID by_binding[2] = { buf_ocean_init_params, buf_h0 };
		for (int bnd = 0; bnd < 2; bnd++) {
			RD::Uniform u;
			u.uniform_type = (bnd == 0) ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = bnd;
			u.append_id(by_binding[bnd]);
			uniforms.push_back(u);
		}
		uset_spectrum_init = rd->uniform_set_create(uniforms, shader_spectrum_init, 0);
	}
	auto make_uset3 = [&](RID p_shader, RID p_params, RID p_in, RID p_out) {
		Vector<RD::Uniform> uniforms;
		const RID by_binding[3] = { p_params, p_in, p_out };
		for (int bnd = 0; bnd < 3; bnd++) {
			RD::Uniform u;
			u.uniform_type = (bnd == 0) ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = bnd;
			u.append_id(by_binding[bnd]);
			uniforms.push_back(u);
		}
		return rd->uniform_set_create(uniforms, p_shader, 0);
	};
	{
		Vector<RD::Uniform> uniforms;
		const RID by_binding[6] = { buf_ocean_evolve_params, buf_h0, buf_ocean_spec, buf_ocean_spec_dz, buf_ocean_spec_c, buf_ocean_spec_d };
		for (int bnd = 0; bnd < 6; bnd++) {
			RD::Uniform u;
			u.uniform_type = (bnd == 0) ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = bnd;
			u.append_id(by_binding[bnd]);
			uniforms.push_back(u);
		}
		uset_spectrum_evolve = rd->uniform_set_create(uniforms, shader_spectrum_evolve, 0);
	}
	uset_fft_spec_to_a = make_uset3(shader_fft, buf_fft_params, buf_ocean_spec, buf_fft_a);
	uset_fft_atob = make_uset3(shader_fft, buf_fft_params, buf_fft_a, buf_fft_b);
	uset_fft_btoa = make_uset3(shader_fft, buf_fft_params, buf_fft_b, buf_fft_a);
	uset_fft_dz_to_c = make_uset3(shader_fft, buf_fft_params, buf_ocean_spec_dz, buf_fft_c);
	uset_fft_ctod = make_uset3(shader_fft, buf_fft_params, buf_fft_c, buf_fft_d);
	uset_fft_dtoc = make_uset3(shader_fft, buf_fft_params, buf_fft_d, buf_fft_c);
	uset_fft_c_to_e = make_uset3(shader_fft, buf_fft_params, buf_ocean_spec_c, buf_fft_e);
	uset_fft_etof = make_uset3(shader_fft, buf_fft_params, buf_fft_e, buf_fft_f);
	uset_fft_ftoe = make_uset3(shader_fft, buf_fft_params, buf_fft_f, buf_fft_e);
	uset_fft_d_to_g = make_uset3(shader_fft, buf_fft_params, buf_ocean_spec_d, buf_fft_g);
	uset_fft_gtoh = make_uset3(shader_fft, buf_fft_params, buf_fft_g, buf_fft_h);
	uset_fft_htog = make_uset3(shader_fft, buf_fft_params, buf_fft_h, buf_fft_g);
	// The shared-memory FFT (water_fft_rows.glsl): all four transforms of a step in two dispatches, rows from the
	// spectra into a/c/e/g, then columns into b/d/f/h -- where the per-stage chain also lands.
	if (on <= FFT_ROWS_MAX_N && bool(GLOBAL_GET("physics/physx_3d/water/shared_memory_fft"))) {
		auto make_rows = [&](RID p_params, const RID *p_in, const RID *p_out) {
			Vector<RD::Uniform> uniforms;
			for (int bnd = 0; bnd < 9; bnd++) {
				RD::Uniform u;
				u.uniform_type = bnd == 0 ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
				u.binding = bnd;
				u.append_id(bnd == 0 ? p_params : (bnd <= 4 ? p_in[bnd - 1] : p_out[bnd - 5]));
				uniforms.push_back(u);
			}
			return rd->uniform_set_create(uniforms, shader_fft_rows, 0);
		};
		const int32_t rows_vals[4] = { on, ocean_log2n, 1, 0 };
		const int32_t cols_vals[4] = { on, ocean_log2n, 0, 0 };
		Vector<uint8_t> pb;
		pb.resize(FFT_PARAMS_BYTES);
		buf_fft_rows_params = rd->uniform_buffer_create(FFT_PARAMS_BYTES);
		buf_fft_cols_params = rd->uniform_buffer_create(FFT_PARAMS_BYTES);
		memcpy(pb.ptrw(), rows_vals, sizeof(rows_vals));
		rd->buffer_update(buf_fft_rows_params, 0, FFT_PARAMS_BYTES, pb.ptr());
		memcpy(pb.ptrw(), cols_vals, sizeof(cols_vals));
		rd->buffer_update(buf_fft_cols_params, 0, FFT_PARAMS_BYTES, pb.ptr());
		const RID spectra[4] = { buf_ocean_spec, buf_ocean_spec_dz, buf_ocean_spec_c, buf_ocean_spec_d };
		const RID mid[4] = { buf_fft_a, buf_fft_c, buf_fft_e, buf_fft_g };
		const RID out[4] = { buf_fft_b, buf_fft_d, buf_fft_f, buf_fft_h };
		uset_fft_rows = make_rows(buf_fft_rows_params, spectra, mid);
		uset_fft_cols = make_rows(buf_fft_cols_params, mid, out);
	}

	// Blit-to-texture: real RD textures, sampled directly by the scene
	// renderer via a caller-side Texture2DRD -- see the header for why.
	RD::TextureFormat ripple_tf;
	ripple_tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	ripple_tf.width = n;
	ripple_tf.height = n;
	ripple_tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	tex_ripple_height = rd->texture_create(ripple_tf, RD::TextureView());

	RD::TextureFormat ocean_tf = ripple_tf;
	ocean_tf.width = on;
	ocean_tf.height = on;
	// Cleared, not left undefined: the materials sample these from the frame they're bound, and a cascade's
	// first step comes a few frames later (undefined derivatives read as folded crests, a white flash).
	{
		Vector<uint8_t> zeros;
		zeros.resize(ocells * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> data;
		data.push_back(zeros);
		tex_ocean_height = rd->texture_create(ocean_tf, RD::TextureView(), data);
	}
	RD::TextureFormat disp_tf = ocean_tf;
	disp_tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	{
		Vector<uint8_t> zeros;
		zeros.resize(ocells * 4 * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> data;
		data.push_back(zeros);
		tex_ocean_disp = rd->texture_create(disp_tf, RD::TextureView(), data);
		tex_ocean_deriv = rd->texture_create(disp_tf, RD::TextureView(), data);
	}
	{
		RD::TextureFormat foam_tf = ocean_tf;
		foam_tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		Vector<uint8_t> zeros;
		zeros.resize(ocells * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> foam_data;
		foam_data.push_back(zeros);
		tex_ocean_foam = rd->texture_create(foam_tf, RD::TextureView(), foam_data);
	}

	buf_blit_ripple_params = rd->uniform_buffer_create(BLIT_PARAMS_BYTES);
	buf_blit_ocean_params = rd->uniform_buffer_create(BLIT_PARAMS_BYTES);
	{
		int32_t v[4] = { n, 0, 0, 0 };
		PackedByteArray b;
		b.resize(BLIT_PARAMS_BYTES);
		memcpy(b.ptrw(), v, sizeof(v));
		rd->buffer_update(buf_blit_ripple_params, 0, BLIT_PARAMS_BYTES, b.ptr());
	}
	{
		int32_t v[4] = { on, 0, 0, 0 };
		PackedByteArray b;
		b.resize(BLIT_PARAMS_BYTES);
		memcpy(b.ptrw(), v, sizeof(v));
		rd->buffer_update(buf_blit_ocean_params, 0, BLIT_PARAMS_BYTES, b.ptr());
	}
	auto make_blit_uset = [&](RID p_shader, RID p_params, RID p_in_buf, RID p_out_tex) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u0.binding = 0;
		u0.append_id(p_params);
		uniforms.push_back(u0);
		RD::Uniform u1;
		u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u1.binding = 1;
		u1.append_id(p_in_buf);
		uniforms.push_back(u1);
		RD::Uniform u2;
		u2.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u2.binding = 2;
		u2.append_id(p_out_tex);
		uniforms.push_back(u2);
		return rd->uniform_set_create(uniforms, p_shader, 0);
	};
	uset_blit_ripple = make_blit_uset(shader_blit_ripple, buf_blit_ripple_params, buf_height, tex_ripple_height);
	{
		Vector<RD::Uniform> uniforms;
		const RID ids[8] = { buf_blit_ocean_params, buf_fft_b, tex_ocean_height, buf_fft_d, tex_ocean_disp, buf_fft_f, buf_fft_h, tex_ocean_deriv };
		const RD::UniformType types[8] = { RD::UNIFORM_TYPE_UNIFORM_BUFFER, RD::UNIFORM_TYPE_STORAGE_BUFFER, RD::UNIFORM_TYPE_IMAGE,
			RD::UNIFORM_TYPE_STORAGE_BUFFER, RD::UNIFORM_TYPE_IMAGE, RD::UNIFORM_TYPE_STORAGE_BUFFER, RD::UNIFORM_TYPE_STORAGE_BUFFER,
			RD::UNIFORM_TYPE_IMAGE };
		for (int bnd = 0; bnd < 8; bnd++) {
			RD::Uniform u;
			u.uniform_type = types[bnd];
			u.binding = bnd;
			u.append_id(ids[bnd]);
			uniforms.push_back(u);
		}
		uset_blit_ocean = rd->uniform_set_create(uniforms, shader_blit_ocean, 0);
	}
	buf_foam_params = rd->uniform_buffer_create(FOAM_PARAMS_BYTES);
	buf_foam_cover = rd->storage_buffer_create(sizeof(uint32_t));
	{
		Vector<RD::Uniform> uniforms;
		const RID ids[5] = { buf_foam_params, tex_ocean_deriv, tex_ocean_disp, tex_ocean_foam, buf_foam_cover };
		for (int bnd = 0; bnd < 5; bnd++) {
			RD::Uniform u;
			u.uniform_type = bnd == 0 ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : (bnd == 4 ? RD::UNIFORM_TYPE_STORAGE_BUFFER : RD::UNIFORM_TYPE_IMAGE);
			u.binding = bnd;
			u.append_id(ids[bnd]);
			uniforms.push_back(u);
		}
		uset_foam = rd->uniform_set_create(uniforms, shader_foam, 0);
	}
	{
		RD::TextureFormat shore_tf = ripple_tf;
		Vector<uint8_t> zeros;
		zeros.resize(cells * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> shore_data;
		shore_data.push_back(zeros);
		tex_shore_foam = rd->texture_create(shore_tf, RD::TextureView(), shore_data);
		tex_shore_foam_tmp = rd->texture_create(shore_tf, RD::TextureView(), shore_data);
	}
	{
		RD::TextureFormat swash_tf = ripple_tf;
		swash_tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		swash_tf.usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT; // (wet sand read back for the weather)
		Vector<uint8_t> zeros;
		zeros.resize(cells * 4 * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> swash_data;
		swash_data.push_back(zeros);
		tex_swash = rd->texture_create(swash_tf, RD::TextureView(), swash_data);
		tex_swash_tmp = rd->texture_create(swash_tf, RD::TextureView(), swash_data);
	}
	buf_shore_foam_params = rd->uniform_buffer_create(SHORE_FOAM_PARAMS_BYTES);
	buf_shore_foam_copy_params = rd->uniform_buffer_create(SHORE_FOAM_PARAMS_BYTES);
	{
		// Copy-mode params never change: just n and the copy flag.
		int32_t copy_res[4] = { n, 1, 1, 0 };
		Vector<uint8_t> cb;
		cb.resize(SHORE_FOAM_PARAMS_BYTES);
		memset(cb.ptrw(), 0, cb.size());
		memcpy(cb.ptrw() + 48, copy_res, sizeof(copy_res));
		rd->buffer_update(buf_shore_foam_copy_params, 0, SHORE_FOAM_PARAMS_BYTES, cb.ptr());
	}
	// Advect + inject reads tex_shore_foam, writes the scratch; the copy
	// pass writes it back so materials always sample tex_shore_foam.
	auto make_shore_uset = [&](RID p_params, RID p_src, RID p_dst) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u0.binding = 0;
		u0.append_id(p_params);
		uniforms.push_back(u0);
		const RID sampled[3] = { tex_cell_depth, tex_ripple_height, tex_ocean_height };
		for (int i = 0; i < 3; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.binding = i + 1;
			u.append_id(sampler_linear);
			u.append_id(sampled[i]);
			uniforms.push_back(u);
		}
		const RID images[2] = { p_src, p_dst };
		for (int i = 0; i < 2; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.binding = 4 + i;
			u.append_id(images[i]);
			uniforms.push_back(u);
		}
		RD::Uniform us;
		us.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		us.binding = 6;
		us.append_id(sampler_linear);
		us.append_id(tex_swash);
		uniforms.push_back(us);
		return rd->uniform_set_create(uniforms, shader_shore_foam, 0);
	};
	uset_shore_foam = make_shore_uset(buf_shore_foam_params, tex_shore_foam, tex_shore_foam_tmp);
	uset_shore_foam_copy = make_shore_uset(buf_shore_foam_copy_params, tex_shore_foam_tmp, tex_shore_foam);

	// Advected ripple coordinates (water_flow_offset.glsl): zero = plain flow mapping.
	{
		RD::TextureFormat offset_tf = ripple_tf;
		offset_tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		offset_tf.usage_bits |= RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT; // (cleared on a re-centre)
		Vector<uint8_t> zeros;
		zeros.resize(cells * 4 * sizeof(float));
		memset(zeros.ptrw(), 0, zeros.size());
		Vector<Vector<uint8_t>> offset_data;
		offset_data.push_back(zeros);
		tex_flow_offset = rd->texture_create(offset_tf, RD::TextureView(), offset_data);
		tex_flow_offset_tmp = rd->texture_create(offset_tf, RD::TextureView(), offset_data);
		buf_flow_offset_params = rd->uniform_buffer_create(FLOW_OFFSET_PARAMS_BYTES);
		buf_flow_offset_copy_params = rd->uniform_buffer_create(FLOW_OFFSET_PARAMS_BYTES);
		int32_t copy_res[4] = { n, 1, 1, 0 };
		Vector<uint8_t> cb;
		cb.resize(FLOW_OFFSET_PARAMS_BYTES);
		memset(cb.ptrw(), 0, cb.size());
		memcpy(cb.ptrw() + 32, copy_res, sizeof(copy_res));
		rd->buffer_update(buf_flow_offset_copy_params, 0, FLOW_OFFSET_PARAMS_BYTES, cb.ptr());
		auto make_offset_uset = [&](RID p_params, RID p_src, RID p_dst) {
			Vector<RD::Uniform> uniforms;
			RD::Uniform u0;
			u0.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u0.binding = 0;
			u0.append_id(p_params);
			uniforms.push_back(u0);
			RD::Uniform uv;
			uv.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			uv.binding = 1;
			uv.append_id(sampler_linear);
			uv.append_id(tex_velocity);
			uniforms.push_back(uv);
			const RID images[2] = { p_src, p_dst };
			for (int i = 0; i < 2; i++) {
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 2 + i;
				u.append_id(images[i]);
				uniforms.push_back(u);
			}
			return rd->uniform_set_create(uniforms, shader_flow_offset, 0);
		};
		uset_flow_offset = make_offset_uset(buf_flow_offset_params, tex_flow_offset, tex_flow_offset_tmp);
		uset_flow_offset_copy = make_offset_uset(buf_flow_offset_copy_params, tex_flow_offset_tmp, tex_flow_offset);
		flow_clock_last = -1.0f;
	}

	buf_swash_params = rd->uniform_buffer_create(SWASH_PARAMS_BYTES);
	buf_swash_copy_params = rd->uniform_buffer_create(SWASH_PARAMS_BYTES);
	{
		int32_t copy_res[4] = { n, 1, 0, 0 };
		Vector<uint8_t> cb;
		cb.resize(SWASH_PARAMS_BYTES);
		memset(cb.ptrw(), 0, cb.size());
		memcpy(cb.ptrw() + 48, copy_res, sizeof(copy_res));
		rd->buffer_update(buf_swash_copy_params, 0, SWASH_PARAMS_BYTES, cb.ptr());
	}
	_make_swash_usets();

	// h0 depends only on static settings (wind/domain/amplitude) -- generate
	// it once here, not every step.
	_rt_spectrum_init();

	if (_init_caustics_enabled) {
		_rt_build_caustics_grid();
	}
}

// (Re)generate h0 from the _init_* sea state into the existing buffer.
void WaterSolverGPU::_rt_spectrum_init() {
	const int on = ocean_grid_resolution;
	{
		Vector<uint8_t> ib;
		ib.resize(OCEAN_INIT_PARAMS_BYTES);
		uint8_t *iw = ib.ptrw();
		auto put_f = [&](int off, float v) { memcpy(iw + off, &v, sizeof(float)); };
		auto put_i = [&](int off, int32_t v) { memcpy(iw + off, &v, sizeof(int32_t)); };
		put_f(0, ocean_domain_size.x);
		put_f(4, ocean_domain_size.y);
		put_f(8, _init_wind_speed);
		put_f(12, _init_wind_direction.x);
		put_f(16, _init_wind_direction.y);
		put_f(20, _init_gravity);
		put_f(24, _init_wave_amplitude * _init_wave_amplitude);
		put_f(28, _init_swell);
		put_i(32, on);
		put_i(36, _init_spreading);
		put_i(40, 0);
		put_i(44, 0);
		put_f(48, _init_fetch);
		put_f(52, _init_depth);
		put_f(56, _init_k_min);
		put_f(60, _init_k_max);
		put_f(64, _init_remote_swell.x);
		put_f(68, _init_remote_swell.y);
		put_f(72, _init_remote_swell.z);
		put_f(76, _init_remote_swell.w);
		rd->buffer_update(buf_ocean_init_params, 0, OCEAN_INIT_PARAMS_BYTES, ib.ptr());

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_spectrum_init);
		rd->compute_list_bind_uniform_set(cl, uset_spectrum_init, 0);
		rd->compute_list_dispatch(cl, groups_for(on), groups_for(on), 1);
		rd->compute_list_end();
		if (local) {
			rd->submit();
			rd->sync();
		}
		// Shared device: this one-time dispatch just gets recorded and rides
		// along with whatever the render thread submits next (the engine's
		// own per-frame flush) -- h0 only needs to be ready before the first
		// rt_step() runs, and rt_build()/rt_step() are always posted to the
		// render thread in that order, so ordering is already guaranteed.
	}
}

void WaterSolverGPU::rt_set_remote_swell(Ref<WaterSolverGPU> p_self, Vector4 p_swell) {
	const Vector2 d = Vector2(p_swell.z, p_swell.w);
	const Vector2 dn = d.length_squared() > 1e-8f ? d.normalized() : Vector2(1.0f, 0.0f);
	_init_remote_swell = Vector4(MAX(p_swell.x, 0.0f), MAX(p_swell.y, 0.0f), dn.x, dn.y);
}

void WaterSolverGPU::rt_set_band(Ref<WaterSolverGPU> p_self, float p_k_min, float p_k_max, int p_spreading, float p_swell) {
	_init_k_min = MAX(p_k_min, 0.0f);
	_init_k_max = MAX(p_k_max, 0.0f);
	_init_spreading = p_spreading;
	_init_swell = CLAMP(p_swell, 0.0f, 1.0f);
}

// A new sea state (wind / direction / height / fetch) WITHOUT a rebuild: only h0 is regenerated, in place. The
// Gaussian draws are seeded by the mode index alone and the ocean clock carries on, so every wave keeps its phase and
// just grows / calms / turns -- ripples, wakes and the whitecap foam survive (a storm can ramp the sea smoothly).
void WaterSolverGPU::rt_reinit_spectrum(Ref<WaterSolverGPU> p_self, float p_wind_speed, Vector2 p_wind_direction, float p_wave_amplitude, float p_fetch) {
	_init_wind_speed = p_wind_speed;
	_init_wind_direction = p_wind_direction;
	_init_wave_amplitude = p_wave_amplitude;
	_init_fetch = p_fetch;
	if (rd == nullptr || !buf_h0.is_valid() || !buf_ocean_init_params.is_valid() || !uset_spectrum_init.is_valid()) {
		return; // not built yet: rt_build() uses these
	}
	_rt_spectrum_init();
}

void WaterSolverGPU::_rt_build_caustics_grid() {
	const int n = MAX(ocean_grid_resolution * 4, CAUSTICS_GRID_MIN_RESOLUTION);
	const int vertex_count = n * n;
	Vector<float> vertices;
	vertices.resize(vertex_count * 2);
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = (z * n + x) * 2;
			vertices.write[i] = ((float)x / (n - 1) - 0.5f) * ocean_domain_size.x * CAUSTICS_TILE_MARGIN;
			vertices.write[i + 1] = ((float)z / (n - 1) - 0.5f) * ocean_domain_size.y * CAUSTICS_TILE_MARGIN;
		}
	}
	caustics_vertex_buffer = rd->vertex_buffer_create(vertices.size() * sizeof(float), vertices.span().reinterpret<uint8_t>());

	caustics_index_count = (n - 1) * (n - 1) * 6;
	Vector<uint32_t> indices;
	indices.resize(caustics_index_count);
	int index = 0;
	for (int z = 0; z < n - 1; z++) {
		for (int x = 0; x < n - 1; x++) {
			const uint32_t a = z * n + x;
			const uint32_t b = a + 1;
			const uint32_t c = a + n;
			const uint32_t d = c + 1;
			indices.write[index++] = a;
			indices.write[index++] = c;
			indices.write[index++] = b;
			indices.write[index++] = b;
			indices.write[index++] = c;
			indices.write[index++] = d;
		}
	}
	caustics_index_buffer = rd->index_buffer_create(caustics_index_count, RD::INDEX_BUFFER_FORMAT_UINT32, indices.span().reinterpret<uint8_t>());
	caustics_index_array = rd->index_array_create(caustics_index_buffer, 0, caustics_index_count);
	Vector<RID> vertex_buffers;
	vertex_buffers.push_back(caustics_vertex_buffer);
	caustics_vertex_array = rd->vertex_array_create(vertex_count, caustics_vertex_format, vertex_buffers);

	RD::TextureFormat texture_format;
	texture_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	texture_format.width = CAUSTICS_MAP_SIZE;
	texture_format.height = CAUSTICS_MAP_SIZE;
	texture_format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	tex_caustics = rd->texture_create(texture_format, RD::TextureView());

	Vector<RD::AttachmentFormat> attachment_formats;
	RD::AttachmentFormat attachment_format;
	attachment_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	attachment_format.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	attachment_formats.push_back(attachment_format);
	const RD::FramebufferFormatID framebuffer_format = rd->framebuffer_format_create(attachment_formats);
	Vector<RID> attachments;
	attachments.push_back(tex_caustics);
	caustics_framebuffer = rd->framebuffer_create(attachments, framebuffer_format);

	buf_caustics_params = rd->uniform_buffer_create(CAUSTICS_PARAMS_BYTES);
	Vector<RD::Uniform> uniforms;
	RD::Uniform params_uniform;
	params_uniform.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	params_uniform.binding = 0;
	params_uniform.append_id(buf_caustics_params);
	uniforms.push_back(params_uniform);
	for (int i = 0; i < 2; i++) {
		RD::Uniform height_uniform;
		height_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		height_uniform.binding = i + 1;
		// The ocean wraps across its edge (the map spans more than one tile):
		// a clamping sampler put a seam of bright caustics there.
		height_uniform.append_id(i == 0 ? sampler_linear : sampler_linear_repeat);
		height_uniform.append_id(i == 0 ? tex_ripple_height : tex_ocean_height);
		uniforms.push_back(height_uniform);
	}
	RD::Uniform depth_uniform;
	depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	depth_uniform.binding = 3;
	depth_uniform.append_id(sampler_linear);
	depth_uniform.append_id(tex_cell_depth);
	uniforms.push_back(depth_uniform);
	// The foam, which shades the bed under it.
	for (int i = 0; i < 2; i++) {
		RD::Uniform foam_uniform;
		foam_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		foam_uniform.binding = 4 + i;
		foam_uniform.append_id(i == 0 ? sampler_linear_repeat : sampler_linear);
		foam_uniform.append_id(i == 0 ? tex_ocean_foam : tex_shore_foam);
		uniforms.push_back(foam_uniform);
	}
	uset_caustics = rd->uniform_set_create(uniforms, shader_caustics, 0);
}

void WaterSolverGPU::_rt_depth_bytes(const PackedFloat32Array &p_cell_depth, Vector<uint8_t> &r_depth, Vector<uint8_t> &r_fade, Vector<uint8_t> &r_shore) const {
	const int cells = grid_resolution * grid_resolution;
	const bool per_cell = p_cell_depth.size() == cells;
	r_depth.resize(cells * sizeof(uint16_t));
	r_fade.resize(cells);
	r_shore.resize(cells * sizeof(uint16_t));
	uint16_t *dw = (uint16_t *)r_depth.ptrw();
	uint8_t *fw = r_fade.ptrw();
	uint16_t *sw = (uint16_t *)r_shore.ptrw();
	for (int i = 0; i < cells; i++) {
		const float d = per_cell ? p_cell_depth[i] : _init_depth;
		dw[i] = Math::make_half_float(d);
		fw[i] = (uint8_t)Math::round(WaterSolver::shallow_fade(d, shallow_fade_depth) * 255.0f);
		sw[i] = Math::make_half_float(d <= WaterSolver::WALL_DEPTH * 0.5f ? 100.0f : d);
	}
}

void WaterSolverGPU::rt_update_depth(Ref<WaterSolverGPU> p_self, PackedFloat32Array p_cell_depth) {
	if (rd == nullptr || !shaders_ok || tex_cell_depth.is_null() || p_cell_depth.size() != grid_resolution * grid_resolution) {
		return;
	}
	_init_cell_depth = p_cell_depth;
	has_cell_depth = true;
	Vector<uint8_t> depth_bytes;
	Vector<uint8_t> fade_bytes;
	Vector<uint8_t> shore_bytes;
	_rt_depth_bytes(p_cell_depth, depth_bytes, fade_bytes, shore_bytes);
	rd->texture_update(tex_cell_depth, 0, depth_bytes);
	rd->texture_update(tex_ocean_fade, 0, fade_bytes);
	rd->texture_update(tex_shore_depth, 0, shore_bytes);
	ripple_quiet.set(0);
}

void WaterSolverGPU::rt_set_base_flux(Ref<WaterSolverGPU> p_self, PackedFloat32Array p_flux, float p_wall_friction) {
	wall_friction = p_wall_friction;
	_init_base_flux = p_flux;
	const int cells = grid_resolution * grid_resolution;
	if (rd == nullptr || !shaders_ok || tex_base_flux.is_null() || (!p_flux.is_empty() && p_flux.size() != cells * 4)) {
		return; // (the next build uploads it: a configure() sends the flow first, for the new grid)
	}
	has_base_flux = !p_flux.is_empty();
	Vector<uint8_t> bytes;
	bytes.resize(cells * 4 * sizeof(float));
	if (has_base_flux) {
		memcpy(bytes.ptrw(), p_flux.ptr(), bytes.size());
	} else {
		memset(bytes.ptrw(), 0, bytes.size());
	}
	rd->texture_update(tex_base_flux, 0, bytes);
	// The live velocities were relative to the old flow: the water starts over on the new one.
	rt_clear_ripples(p_self);
}

void WaterSolverGPU::rt_set_flow_foam(Ref<WaterSolverGPU> p_self, float p_strength, float p_persistence, bool p_impact) {
	flow_foam = MAX(p_strength, 0.0f);
	flow_foam_persistence = MAX(p_persistence, 0.05f);
	flow_foam_impact = p_impact;
}

void WaterSolverGPU::rt_set_advected_ripples(Ref<WaterSolverGPU> p_self, bool p_enabled, float p_period) {
	if (p_enabled != advected_ripples || p_period != advected_period) {
		flow_clock_last = advected_ripples ? 0.0f : -1.0f; // (restart; once more if turning off, to clear)
	}
	advected_ripples = p_enabled;
	advected_period = MAX(p_period, 0.05f);
}

void WaterSolverGPU::rt_set_surface_waves(Ref<WaterSolverGPU> p_self, bool p_enabled, float p_strength) {
	if (p_enabled != surface_waves) {
		surface_wave_clear = true;
	}
	surface_waves = p_enabled;
	surface_wave_strength = MAX(p_strength, 0.0f);
}

void WaterSolverGPU::_rt_free_surface_waves() {
	if (rd == nullptr) {
		return;
	}
	RID *usets[] = { &uset_sw[0], &uset_sw[1], &uset_sw[2], &uset_sw_fwd_rows, &uset_sw_fwd_cols, &uset_sw_inv_rows, &uset_sw_inv_cols };
	for (RID *u : usets) {
		if (u->is_valid() && rd->uniform_set_is_valid(*u)) {
			rd->free_rid(*u);
		}
		*u = RID();
	}
	RID *bufs[] = { &buf_sw_params[0], &buf_sw_params[1], &buf_sw_params[2], &buf_sw_packed, &buf_sw_d[0], &buf_sw_d[1], &buf_sw_d[2], &buf_sw_d[3],
		&buf_sw_m[0], &buf_sw_m[1], &buf_sw_m[2], &buf_sw_m[3], &buf_sw_rows_params, &buf_sw_cols_params, &tex_surface_waves };
	for (RID *b : bufs) {
		if (b->is_valid()) {
			rd->free_rid(*b);
		}
		*b = RID();
	}
	surface_wave_n = 0;
}

void WaterSolverGPU::_rt_build_surface_waves() {
	_rt_free_surface_waves();
	if (!shader_surface_waves.is_valid() || !shader_fft_rows.is_valid() || tex_velocity.is_null() || tex_cell_depth.is_null()) {
		return;
	}
	const int sn = SURFACE_WAVE_N;
	const uint32_t bytes = (uint32_t)(sn * sn) * 2 * sizeof(float);
	Vector<uint8_t> zeros;
	zeros.resize(bytes);
	memset(zeros.ptrw(), 0, bytes);
	buf_sw_packed = rd->storage_buffer_create(bytes, zeros);
	for (int i = 0; i < 4; i++) {
		buf_sw_d[i] = rd->storage_buffer_create(bytes, zeros);
		buf_sw_m[i] = rd->storage_buffer_create(bytes, zeros);
	}
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32G32_SFLOAT;
	tf.width = sn;
	tf.height = sn;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	Vector<Vector<uint8_t>> data;
	data.push_back(zeros);
	tex_surface_waves = rd->texture_create(tf, RD::TextureView(), data);
	for (int m = 0; m < 3; m++) {
		buf_sw_params[m] = rd->uniform_buffer_create(SURFACE_WAVES_PARAMS_BYTES);
		Vector<RD::Uniform> uniforms;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u0.binding = 0;
		u0.append_id(buf_sw_params[m]);
		uniforms.push_back(u0);
		const RID sampled[2] = { tex_velocity, tex_cell_depth };
		for (int i = 0; i < 2; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.binding = 1 + i;
			u.append_id(sampler_linear);
			u.append_id(sampled[i]);
			uniforms.push_back(u);
		}
		RD::Uniform ui;
		ui.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		ui.binding = 3;
		ui.append_id(tex_surface_waves);
		uniforms.push_back(ui);
		const RID storage[5] = { buf_sw_packed, buf_sw_d[0], buf_sw_d[1], buf_sw_d[2], buf_sw_d[3] };
		for (int i = 0; i < 5; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 4 + i;
			u.append_id(storage[i]);
			uniforms.push_back(u);
		}
		uset_sw[m] = rd->uniform_set_create(uniforms, shader_surface_waves, 0);
	}
	// The shared-memory FFT (water_fft_rows.glsl, the +i transform): forward = one field (packed -> m0 -> packed),
	// inverse = the four depth samples (d -> m -> d).
	auto make_rows = [&](RID p_params, const RID *p_in, const RID *p_out) {
		Vector<RD::Uniform> uniforms;
		for (int bnd = 0; bnd < 9; bnd++) {
			RD::Uniform u;
			u.uniform_type = bnd == 0 ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = bnd;
			u.append_id(bnd == 0 ? p_params : (bnd <= 4 ? p_in[bnd - 1] : p_out[bnd - 5]));
			uniforms.push_back(u);
		}
		return rd->uniform_set_create(uniforms, shader_fft_rows, 0);
	};
	const int32_t rows_vals[4] = { sn, SURFACE_WAVE_LOG2N, 1, 0 };
	const int32_t cols_vals[4] = { sn, SURFACE_WAVE_LOG2N, 0, 0 };
	Vector<uint8_t> pb;
	pb.resize(FFT_PARAMS_BYTES);
	memcpy(pb.ptrw(), rows_vals, sizeof(rows_vals));
	buf_sw_rows_params = rd->uniform_buffer_create(FFT_PARAMS_BYTES, pb);
	memcpy(pb.ptrw(), cols_vals, sizeof(cols_vals));
	buf_sw_cols_params = rd->uniform_buffer_create(FFT_PARAMS_BYTES, pb);
	const RID fwd_in[4] = { buf_sw_packed, buf_sw_packed, buf_sw_packed, buf_sw_packed };
	const RID fwd_mid_in[4] = { buf_sw_m[0], buf_sw_m[0], buf_sw_m[0], buf_sw_m[0] };
	const RID fwd_out[4] = { buf_sw_packed, buf_sw_d[1], buf_sw_d[2], buf_sw_d[3] };
	uset_sw_fwd_rows = make_rows(buf_sw_rows_params, fwd_in, buf_sw_m);
	uset_sw_fwd_cols = make_rows(buf_sw_cols_params, fwd_mid_in, fwd_out);
	uset_sw_inv_rows = make_rows(buf_sw_rows_params, buf_sw_d, buf_sw_m);
	uset_sw_inv_cols = make_rows(buf_sw_cols_params, buf_sw_m, buf_sw_d);
	surface_wave_n = sn;
	surface_wave_clear = false;
}

void WaterSolverGPU::rt_set_flow_grid_center(Ref<WaterSolverGPU> p_self, Vector2 p_center) {
	flow_grid_center = p_center;
}

void WaterSolverGPU::rt_clear_ripples(Ref<WaterSolverGPU> p_self) {
	{
		MutexLock lock(cache_mtx);
		ripple_velocity_cache.clear();
	}
	if (rd == nullptr || !shaders_ok || buf_state_a.is_null()) {
		return;
	}
	const uint32_t cells = (uint32_t)(grid_resolution * grid_resolution);
	rd->buffer_clear(buf_state_a, 0, cells * 4 * sizeof(float));
	rd->buffer_clear(buf_state_b, 0, cells * 4 * sizeof(float));
	rd->buffer_clear(buf_height, 0, cells * sizeof(float));
	if (tex_flow_offset.is_valid()) {
		rd->texture_clear(tex_flow_offset, Color(0, 0, 0, 0), 0, 1, 0, 1);
		flow_clock_last = -1.0f;
	}
	surface_wave_clear = true;
	ripple_quiet.set(0);
}

void WaterSolverGPU::rt_build(Ref<WaterSolverGPU> p_self, int p_grid_resolution, Vector2 p_domain_size, int p_ocean_grid_resolution, Vector2 p_ocean_domain_size, float p_wind_speed, Vector2 p_wind_direction, float p_wave_amplitude, float p_gravity, bool p_caustics_enabled, PackedFloat32Array p_cell_depth, float p_depth, float p_shallow_fade_depth, float p_fetch) {
	if (rd == nullptr || !shaders_ok) {
		return;
	}
	built.set_to(false);
	_rt_free_buffers();
	ripple_quiet.set(0);
	grid_resolution = p_grid_resolution;
	domain_size = p_domain_size;
	ocean_grid_resolution = p_ocean_grid_resolution;
	ocean_domain_size = p_ocean_domain_size;
	_init_wind_speed = p_wind_speed;
	_init_wind_direction = p_wind_direction;
	_init_wave_amplitude = p_wave_amplitude;
	_init_gravity = p_gravity;
	_init_caustics_enabled = p_caustics_enabled;
	_init_cell_depth = p_cell_depth;
	_init_depth = p_depth;
	_init_fetch = MAX(p_fetch, 1.0f);
	shallow_fade_depth = p_shallow_fade_depth;
	_rt_build_buffers();
	built.set_to(true);
}

void WaterSolverGPU::rt_step(Ref<WaterSolverGPU> p_self, PackedFloat64Array p_deltas, float p_depth, float p_damping, float p_gravity, float p_water_level, float p_ripple_amplitude, PackedFloat32Array p_spheres, PackedInt32Array p_sphere_counts, PackedFloat32Array p_impulses, PackedInt32Array p_impulse_counts, bool p_rest, int64_t p_serial, bool p_disturbed, float p_rest_threshold) {
	if (rd == nullptr || !shaders_ok || buf_params.is_null() || p_deltas.is_empty()) {
		return;
	}
	const int n = grid_resolution;
	const int on = ocean_grid_resolution;
	ripple_rest_threshold = p_rest_threshold;
	ripple_water_level = p_water_level;
	if (p_disturbed) {
		ripple_disturbed_serial = p_serial;
		ripple_quiet.set(0);
	}

	// GPU profile labels (--gpu-profile, the visual profiler): main device only.
	auto stamp = [&](const char *p_name) {
		if (!local) {
			rd->capture_timestamp(p_name);
		}
	};
	stamp("Water ripples");
	// Ripple layer: one sub-step per tick, each with that tick's dt and bodies (the wave equation's coefficient is
	// clamped per step, so one big dt would not be the same water). The sphere / impulse buffers are re-uploaded only
	// when a tick's data differs from the one before.
	double step_delta = 0.0; // the summed dt of every tick in this call
	const int ticks = p_deltas.size();
	if (!p_rest) {
		int sphere_at = 0;
		int impulse_at = 0;
		const float *prev_spheres = nullptr;
		int prev_sphere_count = -1;
		Vector<uint8_t> params_bytes;
		for (int t = 0; t < ticks; t++) {
			const float dt = (float)p_deltas[t];
			const int sphere_count = t < p_sphere_counts.size() ? p_sphere_counts[t] : 0;
			const int impulse_count = t < p_impulse_counts.size() ? p_impulse_counts[t] : 0;
			const float *spheres = p_spheres.ptr() + sphere_at * SPHERE_FLOATS;
			const float *impulses = p_impulses.ptr() + impulse_at * 4;
			sphere_at += sphere_count;
			impulse_at += impulse_count;
			{
				Vector<uint8_t> b;
				b.resize(PARAMS_BYTES);
				uint8_t *w = b.ptrw();
				auto put_f = [&](int off, float v) { memcpy(w + off, &v, sizeof(float)); };
				auto put_i = [&](int off, int32_t v) { memcpy(w + off, &v, sizeof(int32_t)); };
				put_f(0, domain_size.x * 0.5f);
				put_f(4, domain_size.y * 0.5f);
				put_f(8, p_depth);
				put_f(12, dt);
				put_f(16, p_damping);
				put_f(20, p_gravity);
				put_f(24, domain_size.x / (float)n);
				put_f(28, p_water_level);
				put_i(32, n);
				put_i(36, sphere_count);
				put_i(40, impulse_count);
				put_i(44, 0); // pass (water_ripple.glsl): 0 height, then 1 velocity below
				put_f(48, p_ripple_amplitude);
				put_f(52, wall_friction);
				put_f(56, flow_foam);
				put_f(60, flow_foam_persistence);
				put_f(64, flow_foam_impact ? 1.0f : 0.0f);
				put_f(68, 0.0f);
				put_f(72, 0.0f);
				put_f(76, 0.0f);
				rd->buffer_update(buf_params, 0, PARAMS_BYTES, b.ptr());
				params_bytes = b;
			}
			const bool same_spheres = sphere_count > 0 && prev_sphere_count == sphere_count && memcmp(prev_spheres, spheres, sphere_count * SPHERE_FLOATS * sizeof(float)) == 0;
			if (sphere_count > 0 && !same_spheres) {
				Vector<uint8_t> sb;
				sb.resize(MAX_SPHERES * SPHERE_FLOATS * sizeof(float));
				memset(sb.ptrw(), 0, sb.size());
				memcpy(sb.ptrw(), spheres, MIN((size_t)sphere_count * SPHERE_FLOATS * sizeof(float), (size_t)sb.size()));
				rd->buffer_update(buf_spheres, 0, sb.size(), sb.ptr());
			}
			prev_spheres = spheres;
			prev_sphere_count = sphere_count;
			if (impulse_count > 0) { // (the shader reads only NUM_IMPULSES entries)
				Vector<uint8_t> ib;
				ib.resize(MAX_IMPULSES * 4 * sizeof(float));
				memset(ib.ptrw(), 0, ib.size());
				memcpy(ib.ptrw(), impulses, MIN((size_t)impulse_count * 4 * sizeof(float), (size_t)ib.size()));
				rd->buffer_update(buf_impulses, 0, ib.size(), ib.ptr());
			}
			// Shallow-water step: heights (current -> other buffer), then face velocities from the new heights (back).
			for (int pass = 0; pass < 2; pass++) {
				if (pass == 1) {
					const int32_t one = 1;
					memcpy(params_bytes.ptrw() + 44, &one, sizeof(int32_t));
					rd->buffer_update(buf_params, 0, PARAMS_BYTES, params_bytes.ptr());
				}
				RD::ComputeListID cl = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(cl, pipeline_ripple);
				rd->compute_list_bind_uniform_set(cl, a_is_current ? uset_atob : uset_btoa, 0);
				rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
				rd->compute_list_end();
				a_is_current = !a_is_current;
			}
		}
	}
	for (int t = 0; t < ticks; t++) {
		step_delta += p_deltas[t];
	}

	// Advected ripple coordinates: carried by the velocity the ripple passes just wrote, once per step. A step that
	// skipped the ripple solve (rest) or a clock that jumped (a pause, the materials' hourly wrap) restarts them.
	if (uset_flow_offset.is_valid() && rd->uniform_set_is_valid(uset_flow_offset) && rd->uniform_set_is_valid(uset_flow_offset_copy)) {
		const uint32_t bits = flow_clock_bits.get();
		float clock_now;
		memcpy(&clock_now, &bits, sizeof(float));
		const float period = MAX(advected_period, 0.05f);
		const bool restart = p_rest || flow_clock_last < 0.0f || clock_now < flow_clock_last || clock_now - flow_clock_last > 0.5f * period;
		float op[8] = { period, restart ? clock_now : flow_clock_last, clock_now, (float)step_delta,
			flow_grid_center.x, flow_grid_center.y, domain_size.x, domain_size.y };
		int32_t orr[4] = { n, advected_ripples ? 1 : 0, 0, restart ? 1 : 0 };
		if (advected_ripples || flow_clock_last >= 0.0f) { // (once more after turning off, to clear it)
			Vector<uint8_t> ob;
			ob.resize(FLOW_OFFSET_PARAMS_BYTES);
			memcpy(ob.ptrw(), op, sizeof(op));
			memcpy(ob.ptrw() + sizeof(op), orr, sizeof(orr));
			rd->buffer_update(buf_flow_offset_params, 0, FLOW_OFFSET_PARAMS_BYTES, ob.ptr());
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, pipeline_flow_offset);
			rd->compute_list_bind_uniform_set(cl, uset_flow_offset, 0);
			rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_uniform_set(cl, uset_flow_offset_copy, 0);
			rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
			rd->compute_list_end();
		}
		flow_clock_last = advected_ripples ? clock_now : -1.0f;
	}

	// Dispersive surface waves (water_surface_waves.glsl): pack, forward FFT, exact Airy step at four depths, inverse
	// FFTs, unpack, once a step over the step's whole time.
	if (surface_waves && has_base_flux && step_delta > 0.0) {
		if (surface_wave_n == 0 || surface_wave_clear) {
			_rt_build_surface_waves();
		}
		if (surface_wave_n > 0 && rd->uniform_set_is_valid(uset_sw[0]) && rd->uniform_set_is_valid(uset_sw_inv_cols)) {
			surface_wave_time += (float)step_delta;
			const float dt = MIN((float)step_delta, 0.1f);
			float sp[12] = { dt, domain_size.x / (float)surface_wave_n, _init_gravity, fmodf(surface_wave_time, 3600.0f),
				domain_size.x, domain_size.y, surface_wave_strength, 0.3f,
				0.15f, 0.4f, 1.0f, 2.5f };
			Vector<uint8_t> pb;
			pb.resize(SURFACE_WAVES_PARAMS_BYTES);
			memcpy(pb.ptrw(), sp, sizeof(sp));
			for (int m = 0; m < 3; m++) {
				int32_t rr[4] = { surface_wave_n, m, 1, 0 };
				memcpy(pb.ptrw() + sizeof(sp), rr, sizeof(rr));
				rd->buffer_update(buf_sw_params[m], 0, SURFACE_WAVES_PARAMS_BYTES, pb.ptr());
			}
			const uint32_t g = groups_for(surface_wave_n);
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, pipeline_surface_waves);
			rd->compute_list_bind_uniform_set(cl, uset_sw[0], 0);
			rd->compute_list_dispatch(cl, g, g, 1);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_compute_pipeline(cl, pipeline_fft_rows);
			rd->compute_list_bind_uniform_set(cl, uset_sw_fwd_rows, 0);
			rd->compute_list_dispatch(cl, 1, surface_wave_n, 1);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_uniform_set(cl, uset_sw_fwd_cols, 0);
			rd->compute_list_dispatch(cl, 1, surface_wave_n, 1);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_compute_pipeline(cl, pipeline_surface_waves);
			rd->compute_list_bind_uniform_set(cl, uset_sw[1], 0);
			rd->compute_list_dispatch(cl, g, g, 1);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_compute_pipeline(cl, pipeline_fft_rows);
			rd->compute_list_bind_uniform_set(cl, uset_sw_inv_rows, 0);
			rd->compute_list_dispatch(cl, 1, surface_wave_n, 4);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_uniform_set(cl, uset_sw_inv_cols, 0);
			rd->compute_list_dispatch(cl, 1, surface_wave_n, 4);
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_compute_pipeline(cl, pipeline_surface_waves);
			rd->compute_list_bind_uniform_set(cl, uset_sw[2], 0);
			rd->compute_list_dispatch(cl, g, g, 1);
			rd->compute_list_end();
		}
	}

	stamp("Water FFT ocean");
	// FFT ocean layer: evolve, then the Stockham butterfly passes. The spectrum is evaluated AT ocean_time_accum (no
	// state carried between steps), so one evaluation after N ticks is the same ocean as the last of N; the clock adds
	// each tick's dt in order, exactly as one call per tick did.
	for (int t = 0; t < ticks; t++) {
		ocean_time_accum += p_deltas[t];
	}
	{
		Vector<uint8_t> eb;
		eb.resize(OCEAN_EVOLVE_PARAMS_BYTES);
		uint8_t *w = eb.ptrw();
		auto put_f = [&](int off, float v) { memcpy(w + off, &v, sizeof(float)); };
		auto put_i = [&](int off, int32_t v) { memcpy(w + off, &v, sizeof(int32_t)); };
		put_f(0, ocean_domain_size.x);
		put_f(4, ocean_domain_size.y);
		put_f(8, p_depth);
		put_f(12, (float)ocean_time_accum);
		put_i(16, on);
		put_i(20, 0);
		put_i(24, 0);
		put_i(28, 0);
		rd->buffer_update(buf_ocean_evolve_params, 0, OCEAN_EVOLVE_PARAMS_BYTES, eb.ptr());
	}
	{
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_spectrum_evolve);
		rd->compute_list_bind_uniform_set(cl, uset_spectrum_evolve, 0);
		rd->compute_list_dispatch(cl, groups_for(on), groups_for(on), 1);
		rd->compute_list_end();
	}

	if (uset_fft_rows.is_valid()) {
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_fft_rows);
		rd->compute_list_bind_uniform_set(cl, uset_fft_rows, 0);
		rd->compute_list_dispatch(cl, 1, on, 4);
		rd->compute_list_add_barrier(cl);
		rd->compute_list_bind_uniform_set(cl, uset_fft_cols, 0);
		rd->compute_list_dispatch(cl, 1, on, 4);
		rd->compute_list_end();
	}
	bool first = true;
	bool cur_is_a = false;
	for (int dir = 0; dir < 2 && !uset_fft_rows.is_valid(); dir++) {
		for (int p = 0; p < ocean_log2n; p++) {
			const int ns = 1 << (p + 1);
			Vector<uint8_t> fb;
			fb.resize(FFT_PARAMS_BYTES);
			int32_t vals[4] = { on, ns, dir == 0 ? 1 : 0, 0 };
			memcpy(fb.ptrw(), vals, sizeof(vals));
			rd->buffer_update(buf_fft_params, 0, FFT_PARAMS_BYTES, fb.ptr());

			// Both chains (height + x displacement, z displacement) share each
			// pass's params and ping-pong in lockstep.
			RID usets_pass[4];
			if (first) {
				usets_pass[0] = uset_fft_spec_to_a;
				usets_pass[1] = uset_fft_dz_to_c;
				usets_pass[2] = uset_fft_c_to_e;
				usets_pass[3] = uset_fft_d_to_g;
				first = false;
				cur_is_a = true;
			} else {
				usets_pass[0] = cur_is_a ? uset_fft_atob : uset_fft_btoa;
				usets_pass[1] = cur_is_a ? uset_fft_ctod : uset_fft_dtoc;
				usets_pass[2] = cur_is_a ? uset_fft_etof : uset_fft_ftoe;
				usets_pass[3] = cur_is_a ? uset_fft_gtoh : uset_fft_htog;
				cur_is_a = !cur_is_a;
			}
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, pipeline_fft);
			for (const RID &pass_uset : usets_pass) {
				rd->compute_list_bind_uniform_set(cl, pass_uset, 0);
				rd->compute_list_dispatch(cl, groups_for(on), groups_for(on), 1);
			}
			rd->compute_list_end();
		}
	}
	// Always lands in fft_b (and fft_d, fft_f, fft_h) -- 2*log2(N) passes is
	// always even (see the header's note).

	stamp("Water blits");
	// Blit both layers into their real RD textures for zero-copy rendering (a resting ripple layer's texture already
	// holds its last height).
	if (!p_rest) {
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_blit_ripple);
		rd->compute_list_bind_uniform_set(cl, uset_blit_ripple, 0);
		rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
		rd->compute_list_end();
	}
	{
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_blit_ocean);
		rd->compute_list_bind_uniform_set(cl, uset_blit_ocean, 0);
		rd->compute_list_dispatch(cl, groups_for(on), groups_for(on), 1);
		rd->compute_list_end();
	}
	stamp("Water foam");
	// Persistent foam: inject where the surface folds, fade the rest.
	{
		float fp[4] = { Math::exp(-(float)step_delta / MAX(foam_persistence, 0.01f)), foam_choppiness, foam_threshold, 0.25f };
		int32_t fr[4] = { on, foam_enabled ? 1 : 0, 0, 0 };
		Vector<uint8_t> fb;
		fb.resize(FOAM_PARAMS_BYTES);
		memcpy(fb.ptrw(), fp, sizeof(fp));
		memcpy(fb.ptrw() + sizeof(fp), fr, sizeof(fr));
		rd->buffer_update(buf_foam_params, 0, FOAM_PARAMS_BYTES, fb.ptr());
		rd->buffer_clear(buf_foam_cover, 0, sizeof(uint32_t));
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_foam);
		rd->compute_list_bind_uniform_set(cl, uset_foam, 0);
		rd->compute_list_dispatch(cl, groups_for(on), groups_for(on), 1);
		rd->compute_list_end();
	}
	stamp("Water shore foam");
	// Shore foam over the shallows, on the ripple grid. Lingers a bit longer
	// than the whitecaps.
	{
		float sp[12] = {
			Math::exp(-(float)step_delta / MAX(foam_persistence * 1.5f, 0.01f)), shore_foam_band, 0.2f, p_water_level,
			domain_size.x, domain_size.y, ocean_domain_size.x, ocean_domain_size.y,
			(float)step_delta, domain_size.x / (float)n, shore_undertow, p_gravity
		};
		int32_t sr[4] = { n, (foam_enabled && shore_foam_band > 0.0f) ? 1 : 0, 0, 0 };
		Vector<uint8_t> sb;
		sb.resize(SHORE_FOAM_PARAMS_BYTES);
		memcpy(sb.ptrw(), sp, sizeof(sp));
		memcpy(sb.ptrw() + sizeof(sp), sr, sizeof(sr));
		rd->buffer_update(buf_shore_foam_params, 0, SHORE_FOAM_PARAMS_BYTES, sb.ptr());
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_shore_foam);
		rd->compute_list_bind_uniform_set(cl, uset_shore_foam, 0);
		rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
		rd->compute_list_add_barrier(cl);
		rd->compute_list_bind_uniform_set(cl, uset_shore_foam_copy, 0);
		rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
		rd->compute_list_end();
	}
	stamp("Water swash");
	// Swash run-up and wet sand, on the ripple grid.
	if (rd->uniform_set_is_valid(uset_swash) && rd->uniform_set_is_valid(uset_swash_copy)) {
		const float dt = (float)step_delta;
		float wp[12] = {
			dt, swash_run_up, swash_drain_speed, Math::exp(-dt / MAX(wet_sand_dry_time, 0.05f)),
			domain_size.x, domain_size.y, ocean_domain_size.x, ocean_domain_size.y,
			p_water_level, (float)Math::fmod(ocean_time_accum, 10000.0), 0.25f, 1.0f - Math::exp(-dt / 6.0f)
		};
		int32_t wr[4] = { n, 0, 0, 0 };
		const bool shore_live = shore_on && shore_wave_tex.is_valid() && rd->texture_is_valid(shore_wave_tex);
		float ws[8] = {
			shore_omega[0], shore_omega[1], (float)Math::fmod(shore_time, 3600.0), shore_live ? 1.0f : 0.0f,
			shore_center.x, shore_center.y, shore_h0, shore_l0
		};
		Vector<uint8_t> wb;
		wb.resize(SWASH_PARAMS_BYTES);
		memcpy(wb.ptrw(), wp, sizeof(wp));
		memcpy(wb.ptrw() + sizeof(wp), wr, sizeof(wr));
		memcpy(wb.ptrw() + sizeof(wp) + sizeof(wr), ws, sizeof(ws));
		rd->buffer_update(buf_swash_params, 0, SWASH_PARAMS_BYTES, wb.ptr());
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline_swash);
		rd->compute_list_bind_uniform_set(cl, uset_swash, 0);
		rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
		rd->compute_list_add_barrier(cl);
		rd->compute_list_bind_uniform_set(cl, uset_swash_copy, 0);
		rd->compute_list_dispatch(cl, groups_for(n), groups_for(n), 1);
		rd->compute_list_end();
	}

	stamp(">Water step end");
	// Wet sand for the weather's contact wetness, every 8th step (it changes over seconds).
	bool read_swash = false;
	if (swash_wet_wanted.is_set() && tex_swash.is_valid()) {
		if (--swash_read_countdown <= 0) {
			swash_read_countdown = 8;
			read_swash = true;
		}
	}
	if (local) {
		rd->submit();
		rd->sync();
		MutexLock lock(cache_mtx);
		if (read_swash) {
			const Vector<uint8_t> wraw = rd->texture_get_data(tex_swash, 0);
			_rt_store_swash(wraw.ptr(), wraw.size());
		}
		ocean_cache_time = ocean_time_accum;
		if (!p_rest) {
			ripple_cache_time = ocean_time_accum;
			Vector<uint8_t> rraw = rd->buffer_get_data(buf_height, 0, n * n * sizeof(float));
			ripple_height_cache.resize(n * n);
			if (rraw.size() > 0) {
				memcpy(ripple_height_cache.ptrw(), rraw.ptr(), MIN((size_t)rraw.size(), (size_t)(n * n * sizeof(float))));
			}
			_rt_note_ripple_peak(ripple_height_cache.ptr(), ripple_height_cache.size(), p_serial);
			if (has_base_flux) {
				Vector<uint8_t> sraw = rd->buffer_get_data(a_is_current ? buf_state_a : buf_state_b, 0, n * n * 4 * sizeof(float));
				_rt_store_velocity_cache(sraw.ptr(), sraw.size());
			}
		}
		Vector<uint8_t> oraw = rd->buffer_get_data(buf_fft_b, 0, on * on * 2 * sizeof(float));
		ocean_height_cache.resize(on * on);
		ocean_imag_cache.resize(on * on);
		if (oraw.size() > 0) {
			const float *od = (const float *)oraw.ptr();
			float *hw = ocean_height_cache.ptrw();
			float *iw = ocean_imag_cache.ptrw();
			for (int i = 0; i < on * on; i++) {
				hw[i] = od[i * 2 + 0];
				iw[i] = od[i * 2 + 1];
			}
		}
		Vector<uint8_t> dzraw = rd->buffer_get_data(buf_fft_d, 0, on * on * 2 * sizeof(float));
		ocean_dz_cache.resize(on * on);
		ocean_slope_x_cache.resize(on * on);
		if (dzraw.size() > 0) {
			const float *dd = (const float *)dzraw.ptr();
			float *zw = ocean_dz_cache.ptrw();
			float *sw = ocean_slope_x_cache.ptrw();
			for (int i = 0; i < on * on; i++) {
				zw[i] = dd[i * 2 + 0];
				sw[i] = dd[i * 2 + 1];
			}
		}
		_rt_store_foam_cover(rd->buffer_get_data(buf_foam_cover, 0, sizeof(uint32_t)));
		height_ready.set();
		return;
	}

	// Shared device: async readback, two independent reads (ripple, ocean) --
	// lands a few frames later on the render thread and refreshes the
	// caches; each callback binds a Ref to us so we survive the wait, same
	// convention as every other async readback in this module.
	if (!p_rest) {
		rd->buffer_get_data_async(buf_height, callable_mp(this, &WaterSolverGPU::rt_on_ripple_height).bind(ocean_time_accum, p_self, p_serial), 0, n * n * sizeof(float));
		if (has_base_flux) {
			rd->buffer_get_data_async(a_is_current ? buf_state_a : buf_state_b, callable_mp(this, &WaterSolverGPU::rt_on_ripple_state).bind(p_self), 0, n * n * 4 * sizeof(float));
		}
	}
	rd->buffer_get_data_async(buf_fft_b, callable_mp(this, &WaterSolverGPU::rt_on_ocean_height).bind(ocean_time_accum, p_self), 0, on * on * 2 * sizeof(float));
	rd->buffer_get_data_async(buf_fft_d, callable_mp(this, &WaterSolverGPU::rt_on_ocean_dz).bind(p_self), 0, on * on * 2 * sizeof(float));
	rd->buffer_get_data_async(buf_foam_cover, callable_mp(this, &WaterSolverGPU::rt_on_foam_cover).bind(p_self), 0, sizeof(uint32_t));
	if (read_swash) {
		rd->texture_get_data_async(tex_swash, 0, callable_mp(this, &WaterSolverGPU::rt_on_swash).bind(p_self));
	}
}

void WaterSolverGPU::rt_on_swash(const PackedByteArray &p_data, Ref<WaterSolverGPU> p_self) {
	MutexLock lock(cache_mtx);
	_rt_store_swash(p_data.ptr(), p_data.size());
}

// (cache_mtx held.) Keeps r (run-up film) and g (wetness) of each cell.
void WaterSolverGPU::_rt_store_swash(const uint8_t *p_data, int p_bytes) {
	const int cells = grid_resolution * grid_resolution;
	if (p_data == nullptr || p_bytes < cells * 4 * (int)sizeof(float)) {
		return;
	}
	const float *st = (const float *)p_data;
	swash_wet_cache.resize(cells);
	swash_run_cache.resize(cells);
	float *w = swash_wet_cache.ptrw();
	float *ru = swash_run_cache.ptrw();
	for (int i = 0; i < cells; i++) {
		ru[i] = st[i * 4 + 0];
		w[i] = st[i * 4 + 1];
	}
	swash_wet_serial++;
}

void WaterSolverGPU::rt_on_foam_cover(const PackedByteArray &p_data, Ref<WaterSolverGPU> p_self) {
	_rt_store_foam_cover(p_data);
}

// The foam pass's sum (1/1024 per texel) as the mean foam over the grid.
void WaterSolverGPU::_rt_store_foam_cover(const PackedByteArray &p_data) {
	if (p_data.size() < (int)sizeof(uint32_t) || !foam_enabled) {
		foam_cover_bits.set(0xFFFFFFFFu); // unknown
		return;
	}
	uint32_t sum;
	memcpy(&sum, p_data.ptr(), sizeof(uint32_t));
	const float cover = (float)((double)sum / (1024.0 * (double)ocean_grid_resolution * (double)ocean_grid_resolution));
	uint32_t bits;
	memcpy(&bits, &cover, sizeof(uint32_t));
	foam_cover_bits.set(bits);
}

void WaterSolverGPU::rt_on_ripple_state(const PackedByteArray &p_data, Ref<WaterSolverGPU> p_self) {
	MutexLock lock(cache_mtx);
	_rt_store_velocity_cache(p_data.ptr(), p_data.size());
}

// (cache_mtx held.) Keeps y (u') and w (w') of each cell's state.
void WaterSolverGPU::_rt_store_velocity_cache(const uint8_t *p_state, int p_bytes) {
	const int cells = grid_resolution * grid_resolution;
	if (p_state == nullptr || p_bytes < cells * 4 * (int)sizeof(float)) {
		ripple_velocity_cache.clear();
		return;
	}
	const float *st = (const float *)p_state;
	ripple_velocity_cache.resize(cells * 2);
	float *w = ripple_velocity_cache.ptrw();
	for (int i = 0; i < cells; i++) {
		w[i * 2] = st[i * 4 + 1];
		w[i * 2 + 1] = st[i * 4 + 3];
	}
}

void WaterSolverGPU::rt_on_ocean_dz(const PackedByteArray &p_data, Ref<WaterSolverGPU> p_self) {
	MutexLock lock(cache_mtx);
	const int on = ocean_grid_resolution;
	ocean_dz_cache.resize(on * on);
	ocean_slope_x_cache.resize(on * on);
	if (p_data.size() > 0) {
		const float *dd = (const float *)p_data.ptr();
		const int count = MIN((int)(p_data.size() / (2 * (int)sizeof(float))), on * on);
		float *zw = ocean_dz_cache.ptrw();
		float *sw = ocean_slope_x_cache.ptrw();
		for (int i = 0; i < count; i++) {
			zw[i] = dd[i * 2 + 0];
			sw[i] = dd[i * 2 + 1];
		}
	}
}

void WaterSolverGPU::rt_render_caustics(Ref<WaterSolverGPU> p_self, Vector3 p_sun_direction, Vector3 p_light_right, Vector3 p_light_up, Vector3 p_origin, float p_half_extent, float p_reference_depth, float p_ior, bool p_open_beyond, RID p_scenario) {
	if (rd == nullptr || !shaders_ok || uset_caustics.is_null() || caustics_framebuffer.is_null()) {
		return;
	}
	if (!local && !RSG::viewport->has_3d_viewport(p_scenario)) {
		// No viewport draws the surface's world in 3D this frame, so nothing shows the map: it waits for the first
		// frame that draws it.
		caustics_deferred.set();
		return;
	}

	Vector<uint8_t> params;
	params.resize(CAUSTICS_PARAMS_BYTES);
	memset(params.ptrw(), 0, params.size());
	uint8_t *w = params.ptrw();
	auto put_f = [&](int p_offset, float p_value) { memcpy(w + p_offset, &p_value, sizeof(float)); };
	auto put_v3 = [&](int p_offset, const Vector3 &p_value, float p_w) {
		put_f(p_offset, p_value.x);
		put_f(p_offset + 4, p_value.y);
		put_f(p_offset + 8, p_value.z);
		put_f(p_offset + 12, p_w);
	};
	put_v3(0, p_sun_direction, p_ior);
	put_v3(16, p_light_right, p_half_extent);
	put_v3(32, p_light_up, p_reference_depth);
	put_f(48, domain_size.x);
	put_f(52, domain_size.y);
	put_f(56, (has_cell_depth ? 1.0f : 0.0f) + (p_open_beyond ? 2.0f : 0.0f));
	put_f(60, shallow_fade_depth);
	put_f(64, ocean_domain_size.x);
	put_f(68, ocean_domain_size.y);
	put_f(72, ocean_grid_resolution);
	put_f(76, ocean_grid_resolution);
	put_f(80, CAUSTICS_MAP_SIZE);
	put_f(84, CAUSTICS_MAP_SIZE);
	put_f(88, caustics_foam_shade);
	put_v3(96, p_origin, 0.0f);
	rd->buffer_update(buf_caustics_params, 0, CAUSTICS_PARAMS_BYTES, params.ptr());

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 1));
	if (!local) {
		rd->capture_timestamp("Water caustics");
	}
	RD::DrawListID draw_list = rd->draw_list_begin(caustics_framebuffer, RD::DRAW_CLEAR_COLOR_0, clear_colors);
	rd->draw_list_set_viewport(draw_list, Rect2(0, 0, CAUSTICS_MAP_SIZE, CAUSTICS_MAP_SIZE));
	rd->draw_list_bind_render_pipeline(draw_list, pipeline_caustics);
	rd->draw_list_bind_uniform_set(draw_list, uset_caustics, 0);
	rd->draw_list_bind_vertex_array(draw_list, caustics_vertex_array);
	rd->draw_list_bind_index_array(draw_list, caustics_index_array);
	// Instance 0: the refracted caustics (R); instance 1: the water mask (G).
	rd->draw_list_draw(draw_list, true, 2);
	rd->draw_list_end();
	if (!local) {
		rd->capture_timestamp(">Water caustics end");
	}
}

void WaterSolverGPU::rt_on_ripple_height(const PackedByteArray &p_data, double p_time, Ref<WaterSolverGPU> p_self, int64_t p_serial) {
	MutexLock lock(cache_mtx);
	ripple_cache_time = p_time;
	const int n = grid_resolution;
	ripple_height_cache.resize(n * n);
	if (p_data.size() > 0) {
		memcpy(ripple_height_cache.ptrw(), p_data.ptr(), MIN((size_t)p_data.size(), (size_t)(n * n * sizeof(float))));
	}
	_rt_note_ripple_peak(ripple_height_cache.ptr(), MIN((int)(p_data.size() / sizeof(float)), n * n), p_serial);
	height_ready.set();
}

void WaterSolverGPU::_rt_note_ripple_peak(const float *p_heights, int p_count, int64_t p_serial) {
	if (ripple_rest_threshold <= 0.0f || p_heights == nullptr) {
		return;
	}
	if (p_serial <= ripple_disturbed_serial || p_count <= 0) {
		ripple_quiet.set(0); // taken before / at the last disturbance: says nothing about the water after it
		return;
	}
	float peak = 0.0f;
	for (int i = 0; i < p_count; i++) {
		peak = MAX(peak, Math::abs(p_heights[i] - ripple_water_level));
	}
	if (peak < ripple_rest_threshold) {
		ripple_quiet.increment();
	} else {
		ripple_quiet.set(0);
	}
}

void WaterSolverGPU::rt_on_ocean_height(const PackedByteArray &p_data, double p_time, Ref<WaterSolverGPU> p_self) {
	MutexLock lock(cache_mtx);
	ocean_cache_time = p_time;
	const int on = ocean_grid_resolution;
	ocean_height_cache.resize(on * on);
	ocean_imag_cache.resize(on * on);
	if (p_data.size() > 0) {
		const float *od = (const float *)p_data.ptr();
		const int count = MIN((int)(p_data.size() / (2 * (int)sizeof(float))), on * on);
		float *hw = ocean_height_cache.ptrw();
		float *iw = ocean_imag_cache.ptrw();
		for (int i = 0; i < count; i++) {
			hw[i] = od[i * 2 + 0];
			iw[i] = od[i * 2 + 1];
		}
	}
}

void WaterSolverGPU::rt_set_foam(Ref<WaterSolverGPU> p_self, bool p_enabled, float p_choppiness, float p_threshold, float p_persistence, float p_shore_band, float p_shore_undertow) {
	foam_enabled = p_enabled;
	shore_foam_band = p_shore_band;
	shore_undertow = p_shore_undertow;
	foam_choppiness = p_choppiness;
	foam_threshold = p_threshold;
	foam_persistence = p_persistence;
}

void WaterSolverGPU::_make_swash_usets() {
	auto make_swash_uset = [&](RID p_params, RID p_src, RID p_dst) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u0.binding = 0;
		u0.append_id(p_params);
		uniforms.push_back(u0);
		// Without shore waves the ocean height stands in (shore.w is 0, so
		// the shader never reads it).
		const RID shore = shore_wave_tex.is_valid() && rd->texture_is_valid(shore_wave_tex) ? shore_wave_tex : tex_ocean_height;
		const RID sampled[4] = { tex_cell_depth, tex_ripple_height, tex_ocean_height, shore };
		const int bindings[4] = { 1, 2, 3, 6 };
		for (int i = 0; i < 4; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.binding = bindings[i];
			u.append_id(sampler_linear);
			u.append_id(sampled[i]);
			uniforms.push_back(u);
		}
		const RID images[2] = { p_src, p_dst };
		for (int i = 0; i < 2; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.binding = 4 + i;
			u.append_id(images[i]);
			uniforms.push_back(u);
		}
		return rd->uniform_set_create(uniforms, shader_swash, 0);
	};
	if (uset_swash.is_valid() && rd->uniform_set_is_valid(uset_swash)) {
		rd->free_rid(uset_swash);
	}
	if (uset_swash_copy.is_valid() && rd->uniform_set_is_valid(uset_swash_copy)) {
		rd->free_rid(uset_swash_copy);
	}
	uset_swash = make_swash_uset(buf_swash_params, tex_swash, tex_swash_tmp);
	uset_swash_copy = make_swash_uset(buf_swash_copy_params, tex_swash_tmp, tex_swash);
}

void WaterSolverGPU::rt_set_shore_waves(Ref<WaterSolverGPU> p_self, RID p_texture, bool p_on, Vector2 p_omega, float p_h0, float p_l0) {
	shore_on = p_on && p_texture.is_valid();
	shore_omega[0] = p_omega.x;
	shore_omega[1] = p_omega.y;
	shore_h0 = p_h0;
	shore_l0 = p_l0;
	if (p_texture != shore_wave_tex) {
		shore_wave_tex = p_texture;
		if (rd != nullptr && shader_swash.is_valid() && tex_swash.is_valid()) {
			_make_swash_usets();
		}
	}
}

void WaterSolverGPU::rt_set_shore_clock(Ref<WaterSolverGPU> p_self, double p_time, Vector2 p_center) {
	shore_time = p_time;
	shore_center = p_center;
}

void WaterSolverGPU::rt_set_swash(Ref<WaterSolverGPU> p_self, float p_run_up, float p_drain_speed, float p_dry_time) {
	swash_run_up = p_run_up;
	swash_drain_speed = p_drain_speed;
	wet_sand_dry_time = p_dry_time;
}

void WaterSolverGPU::rt_free(Ref<WaterSolverGPU> p_self) {
	if (rd == nullptr) {
		return;
	}
	if (local) {
		rd->sync();
	}
	_rt_free_buffers();
	RID *pipelines[] = { &pipeline_ripple, &pipeline_spectrum_init, &pipeline_spectrum_evolve, &pipeline_fft, &pipeline_fft_rows, &pipeline_blit_ripple, &pipeline_blit_ocean, &pipeline_foam, &pipeline_shore_foam, &pipeline_swash, &pipeline_flow_offset, &pipeline_surface_waves, &pipeline_caustics };
	for (RID *pipeline : pipelines) {
		if (pipeline->is_valid()) {
			rd->free_rid(*pipeline);
			*pipeline = RID();
		}
	}
	RID *shaders[] = { &shader_ripple, &shader_spectrum_init, &shader_spectrum_evolve, &shader_fft, &shader_fft_rows, &shader_blit_ripple, &shader_blit_ocean, &shader_foam, &shader_shore_foam, &shader_swash, &shader_flow_offset, &shader_surface_waves, &shader_caustics };
	for (RID *s : shaders) {
		if (s->is_valid()) {
			rd->free_rid(*s);
			*s = RID();
		}
	}
	if (sampler_linear.is_valid()) {
		rd->free_rid(sampler_linear);
		sampler_linear = RID();
	}
	if (sampler_linear_repeat.is_valid()) {
		rd->free_rid(sampler_linear_repeat);
		sampler_linear_repeat = RID();
	}
	shaders_ok = false;
}

/* ===================================================================== */
/*  WaterSolver -- CPU-side front. Preps payloads, posts render work.     */
/* ===================================================================== */

WaterSolver::WaterSolver() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	gpu.instantiate();
	gpu->rd = rs->get_rendering_device();
	if (gpu->rd == nullptr) {
		// Headless: no main device. Spin up a private local one and drive it
		// synchronously on this thread -- confirmed empirically this session
		// that this ALSO returns null under --headless, so this path only
		// ever actually engages for a real windowed run without a main
		// device for some other reason; matches MPM's own fallback shape.
		if (!DisplayServer::can_create_rendering_device()) {
			gpu.unref();
			return;
		}
		gpu->rd = rs->create_local_rendering_device();
		if (gpu->rd == nullptr) {
			gpu.unref();
			return;
		}
		gpu->local = true;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_compile).bind(gpu));
}

WaterSolver::~WaterSolver() {
	if (gpu.is_valid() && gpu->rd != nullptr) {
		if (gpu->local) {
			gpu->rt_free(gpu);
			memdelete(gpu->rd); // we own the local device
			gpu->rd = nullptr;
		} else {
			RenderingServer *rs = RenderingServer::get_singleton();
			rs->call_on_render_thread(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_free).bind(gpu));
			rs->sync(); // wait for the render thread to run rt_free before we drop our Ref
		}
	}
	gpu.unref();
}

void WaterSolver::_dispatch(const Callable &p_call) const {
	if (gpu.is_null()) {
		return;
	}
	if (gpu->local) {
		p_call.call();
	} else {
		RenderingServer::get_singleton()->call_on_render_thread(p_call);
	}
}

void WaterSolver::reinit_spectrum(float p_wind_speed, Vector2 p_wind_direction, float p_wave_amplitude, float p_fetch) {
	settings.wind_speed = p_wind_speed;
	settings.wind_direction = p_wind_direction;
	settings.wave_amplitude = p_wave_amplitude;
	settings.fetch = p_fetch;
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_reinit_spectrum).bind(gpu, p_wind_speed, p_wind_direction, p_wave_amplitude, p_fetch));
}

void WaterSolver::set_spreading(int p_spreading, float p_swell) {
	settings.wave_spreading = p_spreading;
	settings.swell = p_swell;
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_band).bind(gpu, settings.ocean_k_min, settings.ocean_k_max, settings.wave_spreading, settings.swell));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_reinit_spectrum).bind(gpu, settings.wind_speed, settings.wind_direction, settings.wave_amplitude, settings.fetch));
}

void WaterSolver::set_remote_swell(const Vector4 &p_swell) {
	settings.remote_swell = p_swell;
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_remote_swell).bind(gpu, settings.remote_swell));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_reinit_spectrum).bind(gpu, settings.wind_speed, settings.wind_direction, settings.wave_amplitude, settings.fetch));
}

void WaterSolver::recenter(Vector2 p_grid_center) {
	flush_ticks(); // ticks recorded so far ran on the old centre (their bodies are relative to it)
	settings.grid_center = p_grid_center;
	for (SphereSlot &s : sphere_slots) {
		s = SphereSlot();
	}
	pending_impulses.clear();
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_flow_grid_center).bind(gpu, p_grid_center));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_clear_ripples).bind(gpu));
}

void WaterSolver::update_seabed(const PackedFloat32Array &p_cell_depth) {
	flush_ticks(); // ticks recorded so far ran on the old seabed
	settings.cell_depth = p_cell_depth;
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_update_depth).bind(gpu, p_cell_depth));
	queued_disturbed = true; // a resting ripple layer has to see the new shore
}

void WaterSolver::set_base_flux(const PackedFloat32Array &p_flux, float p_wall_friction) {
	flush_ticks(); // ticks recorded so far ran on the old flow
	base_flux = p_flux;
	wall_friction = p_wall_friction;
	ripple_resting = false;
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_base_flux).bind(gpu, base_flux, wall_friction));
	queued_disturbed = true;
}

void WaterSolver::set_flow_foam(float p_strength, float p_persistence, bool p_impact) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_flow_foam).bind(gpu, p_strength, p_persistence, p_impact));
}

void WaterSolver::set_advected_ripples(bool p_enabled, float p_period) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_advected_ripples).bind(gpu, p_enabled, p_period));
}

void WaterSolver::set_flow_clock(float p_time) {
	if (gpu.is_null()) {
		return;
	}
	uint32_t bits;
	memcpy(&bits, &p_time, sizeof(float));
	gpu->flow_clock_bits.set(bits);
}

void WaterSolver::set_surface_waves(bool p_enabled, float p_strength) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_surface_waves).bind(gpu, p_enabled, p_strength));
}

RID WaterSolver::get_surface_wave_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_surface_waves;
}

RID WaterSolver::get_flow_offset_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_flow_offset;
}

void WaterSolver::get_velocity_grid(Vector<float> &r_velocity) const {
	r_velocity.clear();
	if (gpu.is_null() || base_flux.is_empty()) {
		return;
	}
	MutexLock lock(gpu->cache_mtx);
	r_velocity = gpu->ripple_velocity_cache;
	if (r_velocity.size() != settings.grid_resolution * settings.grid_resolution * 2) {
		r_velocity.clear();
	}
}

RID WaterSolver::get_velocity_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_velocity;
}

void WaterSolver::configure(const Settings &p_settings) {
	flush_ticks(); // the old surface takes the ticks it already had (the ocean clock survives a rebuild)
	_clear_queue();
	ripple_resting = false;
	settings = p_settings;
	for (SphereSlot &s : sphere_slots) {
		s = SphereSlot();
	}
	pending_impulses.clear();
	if (gpu.is_null()) {
		return; // no device (headless): the callable below would be built on a null object
	}
	if (base_flux.size() != settings.grid_resolution * settings.grid_resolution * 4) {
		base_flux.clear(); // (another grid's flow)
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_flow_grid_center).bind(gpu, settings.grid_center));
	// Before the build, so it uploads the flow (an empty one clears it).
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_base_flux).bind(gpu, base_flux, wall_friction));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_band).bind(gpu, settings.ocean_k_min, settings.ocean_k_max, settings.wave_spreading, settings.swell));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_remote_swell).bind(gpu, settings.remote_swell));
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_build).bind(gpu, settings.grid_resolution, settings.domain_size, settings.ocean_grid_resolution, settings.ocean_domain_size, settings.wind_speed, settings.wind_direction, settings.wave_amplitude, settings.gravity, settings.caustics_enabled, settings.cell_depth, settings.depth, settings.shallow_fade_depth, settings.fetch));
}

PackedFloat32Array WaterSolver::_pack_spheres() const {
	PackedFloat32Array out;
	for (const SphereSlot &s : sphere_slots) {
		if (!s.enabled) {
			continue;
		}
		out.push_back(s.world_pos.x - settings.grid_center.x);
		out.push_back(s.world_pos.z - settings.grid_center.y);
		out.push_back(s.radius);
		out.push_back(s.strength);
		// Second vec4: white water churned in per second (WaterBodyContacts' Froude-gated foam).
		out.push_back(s.foam);
		out.push_back(0.0f);
		out.push_back(0.0f);
		out.push_back(0.0f);
	}
	return out;
}

PackedFloat32Array WaterSolver::_pack_impulses() const {
	PackedFloat32Array out;
	for (const ImpulseSlot &imp : pending_impulses) {
		out.push_back(imp.raw ? imp.world_pos.x : imp.world_pos.x - settings.grid_center.x);
		out.push_back(imp.raw ? imp.world_pos.z : imp.world_pos.z - settings.grid_center.y);
		out.push_back(imp.radius);
		out.push_back(imp.strength);
	}
	return out;
}

void WaterSolver::step(double p_delta) {
	queue_tick(p_delta);
	flush_ticks();
}

void WaterSolver::queue_tick(double p_delta) {
	if (!is_available()) {
		return;
	}
	const PackedFloat32Array spheres = _pack_spheres();
	const PackedFloat32Array impulses = _pack_impulses();
	queued_deltas.push_back(p_delta);
	queued_spheres.append_array(spheres);
	queued_sphere_counts.push_back(spheres.size() / SPHERE_FLOATS);
	queued_impulses.append_array(impulses);
	queued_impulse_counts.push_back(impulses.size() / 4);
	if (!impulses.is_empty()) {
		queued_disturbed = true;
	}
	for (int i = 0; i + 4 < spheres.size(); i += SPHERE_FLOATS) {
		if (spheres[i + 3] != 0.0f || spheres[i + 4] != 0.0f) {
			queued_disturbed = true;
			break;
		}
	}
	// One-shot: consumed by exactly one tick, then cleared. Persistent
	// sphere proxies stay until explicitly cleared/overwritten by the caller.
	pending_impulses.clear();
	if (queued_deltas.size() >= MAX_QUEUED_TICKS) {
		flush_ticks();
	}
}

void WaterSolver::flush_ticks() {
	if (queued_deltas.is_empty()) {
		return;
	}
	if (!is_available()) {
		_clear_queue();
		return;
	}
	step_serial++;
	ripple_resting = rest_threshold > 0.0f && base_flux.is_empty() && !queued_disturbed && gpu->ripple_quiet.get() >= REST_QUIET_READBACKS; // (a current never rests)
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_step).bind(gpu, queued_deltas, settings.depth, settings.damping, settings.gravity, settings.water_level, settings.ripple_amplitude, queued_spheres, queued_sphere_counts, queued_impulses, queued_impulse_counts, ripple_resting, step_serial, queued_disturbed, rest_threshold));
	_clear_queue();
}

void WaterSolver::_clear_queue() {
	queued_deltas.clear();
	queued_spheres.clear();
	queued_sphere_counts.clear();
	queued_impulses.clear();
	queued_impulse_counts.clear();
	queued_disturbed = false;
}

void WaterSolver::submit_sphere(uint64_t p_owner, const Vector3 &p_world_pos, float p_radius, float p_strength, float p_foam) {
	int free_slot = -1;
	for (int i = 0; i < MAX_SPHERES; i++) {
		if (sphere_slots[i].enabled && sphere_slots[i].owner == p_owner) {
			sphere_slots[i].world_pos = p_world_pos;
			sphere_slots[i].radius = p_radius;
			sphere_slots[i].strength = p_strength;
			sphere_slots[i].foam = MAX(p_foam, 0.0f);
			return;
		}
		if (free_slot < 0 && !sphere_slots[i].enabled) {
			free_slot = i;
		}
	}
	if (free_slot < 0) {
		return; // MAX_SPHERES exceeded -- silently drop, matches the module's other MAX_* slot conventions
	}
	sphere_slots[free_slot].owner = p_owner;
	sphere_slots[free_slot].world_pos = p_world_pos;
	sphere_slots[free_slot].radius = p_radius;
	sphere_slots[free_slot].strength = p_strength;
	sphere_slots[free_slot].foam = MAX(p_foam, 0.0f);
	sphere_slots[free_slot].enabled = true;
}

void WaterSolver::set_sphere_foam(uint64_t p_owner, float p_per_second) {
	for (SphereSlot &s : sphere_slots) {
		if (s.enabled && s.owner == p_owner) {
			s.foam = MAX(p_per_second, 0.0f);
			return;
		}
	}
}

void WaterSolver::clear_sphere(uint64_t p_owner) {
	for (SphereSlot &s : sphere_slots) {
		if (s.enabled && s.owner == p_owner) {
			s = SphereSlot();
			return;
		}
	}
}

void WaterSolver::submit_impulse(const Vector3 &p_world_pos, float p_radius, float p_strength) {
	if (pending_impulses.size() >= MAX_IMPULSES) {
		return;
	}
	ImpulseSlot imp;
	imp.world_pos = p_world_pos;
	imp.radius = p_radius;
	imp.strength = p_strength;
	pending_impulses.push_back(imp);
}

bool WaterSolver::submit_wave(const Vector3 &p_world_pos, const Vector2 &p_direction, float p_height, float p_wavelength, float p_celerity, float p_width) {
	if (pending_impulses.size() + 2 > MAX_IMPULSES || p_direction.length_squared() < 1e-8f || p_wavelength <= 0.0f || p_height == 0.0f) {
		return false;
	}
	// Slot one: where, marked as a wave by a negative radius (-1 - wavelength), and the height. Slot two: the
	// direction, the crest's width and the speed, as given.
	const Vector2 d = p_direction.normalized();
	ImpulseSlot a;
	a.world_pos = p_world_pos;
	a.radius = -1.0f - p_wavelength;
	a.strength = p_height;
	ImpulseSlot b;
	b.world_pos = Vector3(d.x, 0.0f, d.y);
	b.radius = MAX(p_width, 0.0f);
	b.strength = MAX(p_celerity, 0.0f);
	b.raw = true;
	pending_impulses.push_back(a);
	pending_impulses.push_back(b);
	return true;
}

void WaterSolver::render_caustics(const Vector3 &p_origin, const Vector3 &p_sun_direction, float p_reference_depth, float p_ior, bool p_open_beyond, RID p_scenario) {
	if (!settings.caustics_enabled || !is_available()) {
		return;
	}
	Vector3 sun_direction = p_sun_direction.normalized();
	if (sun_direction.is_zero_approx()) {
		sun_direction = Vector3(0, -1, 0);
	}
	const Vector3 reference_axis = Math::abs(sun_direction.dot(Vector3(0, 1, 0))) < 0.95f ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
	caustics_light_right = sun_direction.cross(reference_axis).normalized();
	caustics_light_up = caustics_light_right.cross(sun_direction).normalized();
	caustics_origin = p_origin;
	p_reference_depth = MAX(p_reference_depth, 0.1f);
	p_ior = MAX(p_ior, 1.001f);
	caustics_half_extent = MAX(settings.ocean_domain_size.x, settings.ocean_domain_size.y) * 0.5f * WaterSolverGPU::CAUSTICS_TILE_MARGIN + p_reference_depth * 0.75f;
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_render_caustics).bind(gpu, sun_direction, caustics_light_right, caustics_light_up, caustics_origin, caustics_half_extent, p_reference_depth, p_ior, p_open_beyond, p_scenario));
}

RID WaterSolver::get_ripple_height_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ripple_height;
}

RID WaterSolver::get_ocean_height_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ocean_height;
}

RID WaterSolver::get_ocean_foam_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ocean_foam;
}

void WaterSolver::set_caustics_foam_shade(float p_shade) {
	if (gpu.is_valid()) {
		gpu->caustics_foam_shade = CLAMP(p_shade, 0.0f, 1.0f);
	}
}

void WaterSolver::set_foam_settings(bool p_enabled, float p_choppiness, float p_threshold, float p_persistence, float p_shore_band, float p_shore_undertow) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_foam).bind(gpu, p_enabled, p_choppiness, p_threshold, p_persistence, p_shore_band, p_shore_undertow));
}

void WaterSolver::set_swash_settings(float p_run_up, float p_drain_speed, float p_dry_time) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_swash).bind(gpu, p_run_up, p_drain_speed, p_dry_time));
}

void WaterSolver::set_shore_waves(RID p_rd_texture, bool p_on, Vector2 p_omega, float p_h0, float p_l0) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_shore_waves).bind(gpu, p_rd_texture, p_on, p_omega, p_h0, p_l0));
}

void WaterSolver::set_shore_clock(double p_time, Vector2 p_center) {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &WaterSolverGPU::rt_set_shore_clock).bind(gpu, p_time, p_center));
}

RID WaterSolver::get_swash_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_swash;
}

RID WaterSolver::get_shore_foam_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_shore_foam;
}

RID WaterSolver::get_ocean_derivative_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ocean_deriv;
}

void WaterSolver::get_ocean_slope_x_grid(Vector<float> &r_slope_x) const {
	r_slope_x.clear();
	if (gpu.is_null()) {
		return;
	}
	MutexLock lock(gpu->cache_mtx);
	r_slope_x = gpu->ocean_slope_x_cache;
}

RID WaterSolver::get_ocean_displacement_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ocean_disp;
}

void WaterSolver::set_wetness_readback(bool p_enabled) {
	if (gpu.is_valid()) {
		gpu->swash_wet_wanted.set_to(p_enabled);
	}
}

bool WaterSolver::get_wetness_grid(Vector<float> &r_wet, int &r_n, uint64_t &r_serial) const {
	if (gpu.is_null()) {
		return false;
	}
	MutexLock lock(gpu->cache_mtx);
	const int n = settings.grid_resolution;
	if (gpu->swash_wet_cache.size() != n * n) {
		return false;
	}
	r_wet = gpu->swash_wet_cache;
	r_n = n;
	r_serial = gpu->swash_wet_serial;
	return true;
}

bool WaterSolver::get_run_up_grid(Vector<float> &r_run_up, int &r_n) const {
	if (gpu.is_null()) {
		return false;
	}
	MutexLock lock(gpu->cache_mtx);
	const int n = settings.grid_resolution;
	if (gpu->swash_run_cache.size() != n * n) {
		return false;
	}
	r_run_up = gpu->swash_run_cache;
	r_n = n;
	return true;
}

float WaterSolver::get_foam_cover() const {
	if (gpu.is_null()) {
		return -1.0f;
	}
	const uint32_t bits = gpu->foam_cover_bits.get();
	if (bits == 0xFFFFFFFFu) {
		return -1.0f;
	}
	float cover;
	memcpy(&cover, &bits, sizeof(float));
	return cover;
}

void WaterSolver::get_ocean_dz_grid(Vector<float> &r_dz) const {
	r_dz.clear();
	if (gpu.is_null()) {
		return;
	}
	MutexLock lock(gpu->cache_mtx);
	r_dz = gpu->ocean_dz_cache;
}

RID WaterSolver::get_shore_depth_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_shore_depth;
}

RID WaterSolver::get_ocean_fade_texture_rd_rid() const {
	if (gpu.is_null()) {
		return RID();
	}
	return gpu->tex_ocean_fade;
}

float WaterSolver::shallow_fade(float p_depth, float p_fade_depth) {
	if (p_depth <= WALL_DEPTH * 0.5f) {
		return 1.0f;
	}
	// smoothstep(0, fade_depth, depth) -- matches the caustics pass.
	const float t = CLAMP(p_depth / MAX(p_fade_depth, 1e-3f), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

RID WaterSolver::get_caustics_texture_rd_rid() const {
	if (gpu.is_null() || !settings.caustics_enabled) {
		return RID();
	}
	return gpu->tex_caustics;
}

void WaterSolver::get_height_grid(Vector<float> &r_height, int &r_n, Vector2 &r_domain_size, double *r_time) const {
	r_height.clear();
	r_n = settings.grid_resolution;
	r_domain_size = settings.domain_size;
	if (gpu.is_null()) {
		return;
	}
	MutexLock lock(gpu->cache_mtx);
	r_height = gpu->ripple_height_cache;
	if (r_time != nullptr) {
		*r_time = gpu->ripple_cache_time;
	}
	if (r_height.size() != r_n * r_n) {
		// grid_resolution just changed: the cache is still the old size until
		// the rebuilt solver reads back. Report "not ready" rather than let
		// callers index the old grid with the new size.
		r_height.clear();
	}
}

void WaterSolver::get_ocean_height_grid(Vector<float> &r_height, Vector<float> &r_imag, int &r_n, Vector2 &r_domain_size, double *r_time) const {
	r_height.clear();
	r_imag.clear();
	r_n = settings.ocean_grid_resolution;
	r_domain_size = settings.ocean_domain_size;
	if (gpu.is_null()) {
		return;
	}
	MutexLock lock(gpu->cache_mtx);
	r_height = gpu->ocean_height_cache;
	r_imag = gpu->ocean_imag_cache;
	if (r_time != nullptr) {
		*r_time = gpu->ocean_cache_time;
	}
	if (r_height.size() != r_n * r_n || r_imag.size() != r_n * r_n) {
		// ocean_grid_resolution just changed (see get_height_grid()).
		r_height.clear();
		r_imag.clear();
	}
}

void WaterSolver::sync_now() const {
	if (gpu.is_valid() && !gpu->local) {
		RenderingServer::get_singleton()->sync();
	}
}

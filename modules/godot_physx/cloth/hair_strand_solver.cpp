/**************************************************************************/
/*  hair_strand_solver.cpp                                                */
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

#include "hair_strand_solver.h"

#include "../godot_physx_render_thread.h"
#include "hair_strands.glsl.gen.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

namespace {
// std140 Params in hair_strands.glsl: 5 vec4, 3 mat4, MAX_SPHERES vec4.
constexpr int PARAMS_BYTES = 5 * 16 + 3 * 64 + HairStrandSolverGPU::MAX_SPHERES * 16;
constexpr uint32_t GROUP = 64; // local_size_x

uint32_t groups_for(int p_count) {
	return (uint32_t)((MAX(p_count, 1) + (int)GROUP - 1) / (int)GROUP);
}

enum Mode {
	MODE_STEP = 0,
	MODE_RESET = 1,
	MODE_OUTPUT = 2,
};

struct PassConstants {
	int32_t mode;
	float frac;
	int32_t pad0;
	int32_t pad1;
};

Mutex shared_mutex;
Ref<RDShaderFile> shared_spirv;
RID shared_shader, shared_pipeline;

RID storage_from(RenderingDevice *p_rd, const uint8_t *p_data, int p_bytes) {
	Vector<uint8_t> bytes;
	bytes.resize(MAX(p_bytes, 16));
	memset(bytes.ptrw(), 0, bytes.size());
	if (p_data != nullptr && p_bytes > 0) {
		memcpy(bytes.ptrw(), p_data, p_bytes);
	}
	return p_rd->storage_buffer_create(bytes.size(), bytes);
}

// Column-major mat4 of an affine transform.
void put_mat4(float *r_dst, const Transform3D &p_t) {
	for (int c = 0; c < 3; c++) {
		const Vector3 col = p_t.basis.get_column(c);
		r_dst[c * 4 + 0] = col.x;
		r_dst[c * 4 + 1] = col.y;
		r_dst[c * 4 + 2] = col.z;
		r_dst[c * 4 + 3] = 0.0f;
	}
	r_dst[12] = p_t.origin.x;
	r_dst[13] = p_t.origin.y;
	r_dst[14] = p_t.origin.z;
	r_dst[15] = 1.0f;
}
} // namespace

/* ===================================================================== */
/*  HairStrandSolverGPU                                                   */
/* ===================================================================== */

void HairStrandSolverGPU::rt_compile(Ref<HairStrandSolverGPU> p_self) {
	if (rd == nullptr) {
		return;
	}
	MutexLock lock(shared_mutex);
	if (!local && shared_shader.is_valid()) {
		shader = shared_shader;
		pipeline = shared_pipeline;
		return;
	}
	if (shared_spirv.is_null()) {
		Ref<RDShaderFile> sf;
		sf.instantiate();
		if (sf->parse_versions_from_text(hair_strands_shader_glsl) != OK) {
			ERR_PRINT("HairStrandSolver: the compute shader failed to compile.");
			return;
		}
		shared_spirv = sf;
	}
	shader = rd->shader_create_from_spirv(shared_spirv->get_spirv_stages());
	ERR_FAIL_COND(shader.is_null());
	pipeline = rd->compute_pipeline_create(shader);
	if (local) {
		owns_shader = true;
	} else {
		shared_shader = shader;
		shared_pipeline = pipeline;
	}
}

void HairStrandSolverGPU::free_shared() {
	MutexLock lock(shared_mutex);
	shared_spirv.unref();
	if (shared_shader.is_null()) {
		return;
	}
	const RID s = shared_shader;
	const RID p = shared_pipeline;
	shared_shader = RID();
	shared_pipeline = RID();
	godot_physx_run_on_render_thread([s, p]() {
		RenderingDevice *device = RenderingServer::get_singleton() ? RenderingServer::get_singleton()->get_rendering_device() : nullptr;
		if (device == nullptr) {
			return;
		}
		if (p.is_valid()) {
			device->free_rid(p);
		}
		device->free_rid(s);
	});
}

void HairStrandSolverGPU::rt_free_buffers() {
	if (rd == nullptr) {
		return;
	}
	built.clear();
	if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
	RID *rids[] = { &buf_params, &buf_rest, &buf_strands, &buf_pos, &buf_prev, &tex_out };
	for (RID *r : rids) {
		if (r->is_valid()) {
			rd->free_rid(*r);
			*r = RID();
		}
	}
}

void HairStrandSolverGPU::rt_free(Ref<HairStrandSolverGPU> p_self) {
	rt_free_buffers();
	if (rd == nullptr) {
		return;
	}
	if (owns_shader) {
		if (pipeline.is_valid()) {
			rd->free_rid(pipeline);
		}
		if (shader.is_valid()) {
			rd->free_rid(shader);
		}
		owns_shader = false;
	}
	pipeline = RID();
	shader = RID();
}

void HairStrandSolverGPU::rt_build(Ref<HairStrandSolverGPU> p_self, PackedFloat32Array p_rest, PackedInt32Array p_strands) {
	if (rd == nullptr || shader.is_null()) {
		return;
	}
	rt_free_buffers();
	point_count = p_rest.size() / 4;
	strand_count = p_strands.size() / 2;
	if (point_count == 0 || strand_count == 0) {
		return;
	}
	buf_params = rd->uniform_buffer_create(PARAMS_BYTES);
	buf_rest = storage_from(rd, (const uint8_t *)p_rest.ptr(), p_rest.size() * 4);
	buf_strands = storage_from(rd, (const uint8_t *)p_strands.ptr(), p_strands.size() * 4);
	buf_pos = storage_from(rd, nullptr, point_count * 16);
	buf_prev = storage_from(rd, nullptr, point_count * 16);

	tex_width = MIN(point_count, 2048);
	tex_height = (point_count + tex_width - 1) / tex_width;
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	tf.width = tex_width;
	tf.height = tex_height;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	Vector<uint8_t> zeros;
	zeros.resize(tex_width * tex_height * 16);
	memset(zeros.ptrw(), 0, zeros.size());
	Vector<Vector<uint8_t>> data;
	data.push_back(zeros);
	tex_out = rd->texture_create(tf, RD::TextureView(), data);

	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 0;
		u.append_id(buf_params);
		uniforms.push_back(u);
	}
	const RID storages[] = { buf_rest, buf_strands, buf_pos, buf_prev };
	for (int i = 0; i < 4; i++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = i + 1;
		u.append_id(storages[i]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 5;
		u.append_id(tex_out);
		uniforms.push_back(u);
	}
	uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	needs_reset = true;
	built.set();
}

void HairStrandSolverGPU::rt_reset(Ref<HairStrandSolverGPU> p_self) {
	needs_reset = true;
}

void HairStrandSolverGPU::rt_step(Ref<HairStrandSolverGPU> p_self, PackedByteArray p_params, int p_substeps) {
	if (rd == nullptr || !built.is_set() || uniform_set.is_null() || p_params.size() != PARAMS_BYTES) {
		return;
	}
	PackedByteArray params = p_params;
	int32_t *counts = (int32_t *)params.ptrw();
	counts[0] = strand_count;
	counts[1] = point_count;
	counts[2] = tex_width;
	rd->buffer_update(buf_params, 0, PARAMS_BYTES, params.ptr());

	PassConstants pc = { MODE_STEP, 0.0f, 0, 0 };
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	auto run = [&](int p_mode, float p_frac, int p_threads) {
		pc.mode = p_mode;
		pc.frac = p_frac;
		rd->compute_list_set_push_constant(cl, &pc, sizeof(pc));
		rd->compute_list_dispatch(cl, groups_for(p_threads), 1, 1);
		rd->compute_list_add_barrier(cl);
	};
	if (needs_reset) {
		run(MODE_RESET, 1.0f, strand_count);
		needs_reset = false;
	}
	const int substeps = MAX(p_substeps, 1);
	for (int s = 0; s < substeps; s++) {
		run(MODE_STEP, (float)(s + 1) / (float)substeps, strand_count);
	}
	run(MODE_OUTPUT, 1.0f, point_count);
	rd->compute_list_end();
	if (local) {
		rd->submit();
		rd->sync();
	}
}

/* ===================================================================== */
/*  HairStrandSolver                                                      */
/* ===================================================================== */

HairStrandSolver::HairStrandSolver() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	gpu.instantiate();
	gpu->rd = rs->get_rendering_device();
	if (gpu->rd == nullptr) {
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
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_compile).bind(gpu));
}

HairStrandSolver::~HairStrandSolver() {
	if (gpu.is_valid() && gpu->rd != nullptr) {
		if (gpu->local) {
			gpu->rt_free(gpu);
			memdelete(gpu->rd);
			gpu->rd = nullptr;
		} else {
			RenderingServer *rs = RenderingServer::get_singleton();
			rs->call_on_render_thread(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_free).bind(gpu));
			rs->sync();
		}
	}
	gpu.unref();
}

void HairStrandSolver::_dispatch(const Callable &p_call) const {
	if (gpu.is_null()) {
		return;
	}
	if (gpu->local) {
		p_call.call();
	} else {
		RenderingServer::get_singleton()->call_on_render_thread(p_call);
	}
}

void HairStrandSolver::build(const PackedFloat32Array &p_rest, const PackedInt32Array &p_strands) {
	if (gpu.is_null()) {
		return;
	}
	has_last = false;
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_build).bind(gpu, p_rest, p_strands));
	if (!gpu->local) {
		// The texture RID must exist before the caller wraps it.
		RenderingServer::get_singleton()->sync();
	}
}

void HairStrandSolver::step(const Transform3D &p_xform, double p_delta, int p_substeps, const Settings &p_settings) {
	if (gpu.is_null()) {
		return;
	}
	const int substeps = MAX(p_substeps, 1);
	const double delta = MIN(p_delta, 1.0 / 20.0);
	const float dt = (float)(delta / substeps);
	time += delta;
	// Rates given per second or per 1/60 s, turned into per-substep fractions.
	auto per_substep = [&](float p_per_60th) {
		return 1.0f - Math::pow(1.0f - CLAMP(p_per_60th, 0.0f, 0.999f), dt * 60.0f);
	};

	PackedByteArray bytes;
	bytes.resize(PARAMS_BYTES);
	memset(bytes.ptrw(), 0, PARAMS_BYTES);
	int32_t *ints = (int32_t *)bytes.ptrw();
	float *f = (float *)bytes.ptrw();
	ints[3] = MIN(p_settings.spheres.size(), HairStrandSolverGPU::MAX_SPHERES);
	// step
	f[4] = dt;
	f[5] = Math::pow(1.0f - CLAMP(p_settings.damping, 0.0f, 0.999f), dt);
	f[6] = per_substep(p_settings.root_follow);
	f[7] = per_substep(p_settings.tip_follow);
	// accel
	f[8] = p_settings.gravity.x;
	f[9] = p_settings.gravity.y;
	f[10] = p_settings.gravity.z;
	f[11] = per_substep(p_settings.shape_stiffness);
	// wind
	f[12] = p_settings.wind.x;
	f[13] = p_settings.wind.y;
	f[14] = p_settings.wind.z;
	f[15] = CLAMP(p_settings.gust, 0.0f, 1.0f);
	// misc
	f[16] = CLAMP(p_settings.length_damping, 0.0f, 1.0f);
	f[17] = MAX(p_settings.margin, 0.0f);
	f[18] = (float)Math::fmod(time, 3600.0);
	f[19] = CLAMP(p_settings.follow_range, 0.0f, 1.0f);
	// matrices
	put_mat4(f + 20, has_last ? last_xform : p_xform);
	put_mat4(f + 36, p_xform);
	put_mat4(f + 52, p_settings.output_xform);
	// spheres
	for (int i = 0; i < ints[3]; i++) {
		const Vector4 &s = p_settings.spheres[i];
		f[68 + i * 4 + 0] = s.x;
		f[68 + i * 4 + 1] = s.y;
		f[68 + i * 4 + 2] = s.z;
		f[68 + i * 4 + 3] = s.w;
	}
	last_xform = p_xform;
	has_last = true;
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_step).bind(gpu, bytes, substeps));
}

void HairStrandSolver::reset() {
	if (gpu.is_null()) {
		return;
	}
	has_last = false;
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_reset).bind(gpu));
}

RID HairStrandSolver::get_position_texture_rd_rid() const {
	return gpu.is_valid() ? gpu->tex_out : RID();
}

int HairStrandSolver::get_texture_width() const {
	return gpu.is_valid() ? gpu->tex_width : 0;
}

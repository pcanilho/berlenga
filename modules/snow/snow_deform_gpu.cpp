/**************************************************************************/
/*  snow_deform_gpu.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "snow_deform_gpu.h"

#include "snow_deform.glsl.gen.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

static constexpr int WG = 256;
static constexpr int TILE = 64;

enum {
	MODE_STAMPS = 0,
	MODE_SLUMP_GIVE = 1,
	MODE_SLUMP_TAKE = 2,
	MODE_RELAX = 3,
	MODE_BLIT = 4,
	MODE_COLLISION = 5,
	MODE_STORE = 6,
	MODE_SHIFT = 7,
	MODE_PACK = 8,
};

bool SnowDeformGPU::is_available() {
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs && rs->get_rendering_device() != nullptr;
}

SnowDeformGPU::SnowDeformGPU() {
	RenderingServer *rs = RenderingServer::get_singleton();
	rd = rs ? rs->get_rendering_device() : nullptr;
}

SnowDeformGPU::~SnowDeformGPU() {
}

void SnowDeformGPU::shutdown() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !rd) {
		return;
	}
	rs->call_on_render_thread(callable_mp(this, &SnowDeformGPU::rt_free));
	rs->sync();
}

RID SnowDeformGPU::get_texture() const {
	MutexLock lock(mutex);
	return texture_rid;
}

bool SnowDeformGPU::take_collision(PackedFloat32Array &r_heights, Vector2 &r_center) {
	MutexLock lock(mutex);
	if (!collision_ready) {
		return false;
	}
	r_heights = collision_heights;
	r_center = collision_center;
	collision_ready = false;
	return true;
}

bool SnowDeformGPU::take_field(PackedByteArray &r_packed, Vector2i &r_origin) {
	MutexLock lock(mutex);
	if (!field_ready) {
		return false;
	}
	r_packed = field_packed;
	r_origin = field_origin;
	field_ready = false;
	return true;
}

void SnowDeformGPU::step(const Dims &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !rd || failed) {
		return;
	}
	PackedInt32Array d;
	d.push_back(p_dims.n);
	d.push_back(p_dims.gn);
	d.push_back(p_dims.cn);
	d.push_back(p_dims.slots);
	d.push_back(p_dims.coll_w);
	rs->call_on_render_thread(callable_mp(this, &SnowDeformGPU::rt_step).bind(d, p_flags, p_params, p_ground, p_cover, p_stamps, p_stores, p_shift, p_origin, p_collision_center));
	if (p_flags & STEP_SYNC) {
		rs->sync();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Render thread

void SnowDeformGPU::_rt_compile() {
	if (compiled || failed) {
		return;
	}
	Ref<RDShaderFile> sf;
	sf.instantiate();
	if (sf->parse_versions_from_text(snow_deform_shader_glsl) != OK) {
		ERR_PRINT("SnowSurface3D: the print field's compute shader failed to compile; staying on the CPU.");
		failed = true;
		return;
	}
	shader = rd->shader_create_from_spirv(sf->get_spirv_stages());
	if (shader.is_null()) {
		ERR_PRINT("SnowSurface3D: the print field's compute shader failed to build; staying on the CPU.");
		failed = true;
		return;
	}
	pipeline = rd->compute_pipeline_create(shader);
	buf_params = rd->uniform_buffer_create(PARAMS_BYTES);
	compiled = pipeline.is_valid();
	failed = !compiled;
}

RID SnowDeformGPU::_rt_storage(int p_bytes) {
	const int bytes = MAX(p_bytes, 16);
	RID b = rd->storage_buffer_create(bytes);
	rd->buffer_clear(b, 0, bytes);
	return b;
}

void SnowDeformGPU::_rt_free_buffers() {
	for (int i = 0; i < 2; i++) {
		if (uset[i].is_valid() && rd->uniform_set_is_valid(uset[i])) {
			rd->free_rid(uset[i]);
		}
		uset[i] = RID();
	}
	RID *bufs[] = { &buf_state[0], &buf_state[1], &buf_give, &buf_ground, &buf_cover, &buf_stamps, &buf_collision, &buf_cache, &buf_tile_ops, &buf_packed, &tex_out };
	for (RID *b : bufs) {
		if (b->is_valid()) {
			rd->free_rid(*b);
		}
		*b = RID();
	}
	stamps_capacity = 0;
	tile_ops_capacity = 0;
	dims = Dims();
}

void SnowDeformGPU::_rt_make_sets() {
	for (int i = 0; i < 2; i++) {
		if (uset[i].is_valid() && rd->uniform_set_is_valid(uset[i])) {
			rd->free_rid(uset[i]);
		}
		Vector<RD::Uniform> uniforms;
		auto add = [&](int p_binding, RD::UniformType p_type, RID p_id) {
			RD::Uniform u;
			u.uniform_type = p_type;
			u.binding = p_binding;
			u.append_id(p_id);
			uniforms.push_back(u);
		};
		add(0, RD::UNIFORM_TYPE_UNIFORM_BUFFER, buf_params);
		add(1, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_state[i]);
		add(2, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_state[1 - i]);
		add(3, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_give);
		add(4, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_ground);
		add(5, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_cover);
		add(6, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_stamps);
		add(7, RD::UNIFORM_TYPE_IMAGE, tex_out);
		add(8, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_collision);
		add(9, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_cache);
		add(10, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_tile_ops);
		add(11, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_packed);
		uset[i] = rd->uniform_set_create(uniforms, shader, 0);
	}
}

void SnowDeformGPU::_rt_ensure(const Dims &p_dims) {
	bool remake = false;
	if (p_dims.n != dims.n || p_dims.slots != dims.slots) {
		// The field itself: start over.
		_rt_free_buffers();
		const int n = p_dims.n;
		buf_state[0] = _rt_storage(n * n * 16);
		buf_state[1] = _rt_storage(n * n * 16);
		buf_give = _rt_storage(n * n * 8);
		buf_packed = _rt_storage(n * n * 4);
		buf_cache = _rt_storage(MAX(p_dims.slots, 1) * TILE * TILE * 4);
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = n;
		tf.height = n;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		tex_out = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(tex_out, Color(0, 0, 1, 1), 0, 1, 0, 1);
		buf_ground = _rt_storage(MAX(p_dims.gn * p_dims.gn, 1) * 4);
		buf_cover = _rt_storage(MAX(p_dims.cn * p_dims.cn, 1) * 32);
		buf_collision = _rt_storage(MAX(p_dims.coll_w * p_dims.coll_w, 1) * 4);
		stamps_capacity = 16;
		buf_stamps = _rt_storage(stamps_capacity * STAMP_BYTES);
		tile_ops_capacity = 64;
		buf_tile_ops = _rt_storage(tile_ops_capacity * 16);
		cur = 0;
		dims = p_dims;
		remake = true;
		MutexLock lock(mutex);
		texture_rid = tex_out;
		texture_serial.increment();
	}
	auto grow = [&](RID &r_buf, int p_bytes, int p_old_bytes) {
		if (p_bytes != p_old_bytes) {
			if (r_buf.is_valid()) {
				rd->free_rid(r_buf);
			}
			r_buf = _rt_storage(p_bytes);
			remake = true;
		}
	};
	grow(buf_ground, MAX(p_dims.gn * p_dims.gn, 1) * 4, MAX(dims.gn * dims.gn, 1) * 4);
	grow(buf_cover, MAX(p_dims.cn * p_dims.cn, 1) * 32, MAX(dims.cn * dims.cn, 1) * 32);
	grow(buf_collision, MAX(p_dims.coll_w * p_dims.coll_w, 1) * 4, MAX(dims.coll_w * dims.coll_w, 1) * 4);
	dims = p_dims;
	if (remake) {
		_rt_make_sets();
	}
}

void SnowDeformGPU::_dispatch(int64_t p_list, int p_mode, int p_count, int p_groups) {
	if (p_groups <= 0) {
		return;
	}
	const int32_t push[4] = { p_mode, p_count, 0, 0 };
	rd->compute_list_bind_uniform_set(p_list, uset[cur], 0);
	rd->compute_list_set_push_constant(p_list, push, sizeof(push));
	rd->compute_list_dispatch(p_list, p_groups, 1, 1);
	rd->compute_list_add_barrier(p_list);
}

void SnowDeformGPU::rt_step(const PackedInt32Array &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center) {
	_rt_compile();
	if (failed || p_dims.size() < 5 || p_dims[0] <= 0 || p_params.size() != PARAMS_BYTES) {
		return;
	}
	Dims d;
	d.n = p_dims[0];
	d.gn = p_dims[1];
	d.cn = p_dims[2];
	d.slots = p_dims[3];
	d.coll_w = p_dims[4];
	_rt_ensure(d);
	const int n = d.n;
	const int field_groups = (n * n + WG - 1) / WG;
	rd->buffer_update(buf_params, 0, PARAMS_BYTES, p_params.ptr());
	if ((p_flags & STEP_GROUND) && p_ground.size() == d.gn * d.gn * 4) {
		rd->buffer_update(buf_ground, 0, p_ground.size(), p_ground.ptr());
	}
	if ((p_flags & STEP_COVER) && p_cover.size() == d.cn * d.cn * 32) {
		rd->buffer_update(buf_cover, 0, p_cover.size(), p_cover.ptr());
	}
	if (p_flags & STEP_RESET) {
		rd->buffer_clear(buf_state[0], 0, n * n * 16);
		rd->buffer_clear(buf_state[1], 0, n * n * 16);
	}
	const int stamp_count = p_stamps.size() / STAMP_BYTES;
	if ((p_flags & STEP_STAMPS) && stamp_count > 0) {
		if (stamp_count > stamps_capacity) {
			rd->free_rid(buf_stamps);
			stamps_capacity = Math::next_power_of_2((uint32_t)stamp_count);
			buf_stamps = _rt_storage(stamps_capacity * STAMP_BYTES);
			_rt_make_sets();
		}
		rd->buffer_update(buf_stamps, 0, stamp_count * STAMP_BYTES, p_stamps.ptr());
	}
	const int store_count = p_stores.size() / 16;
	const int shift_count = p_shift.size() / 16;
	const int ops = MAX(store_count, shift_count);
	if ((p_flags & STEP_SHIFT) && ops > tile_ops_capacity) {
		rd->free_rid(buf_tile_ops);
		tile_ops_capacity = Math::next_power_of_2((uint32_t)ops);
		buf_tile_ops = _rt_storage(tile_ops_capacity * 16);
		_rt_make_sets();
	}

	if ((p_flags & STEP_SHIFT) && store_count > 0) {
		rd->buffer_update(buf_tile_ops, 0, store_count * 16, p_stores.ptr());
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline);
		_dispatch(cl, MODE_STORE, store_count, (store_count * TILE * TILE + WG - 1) / WG);
		rd->compute_list_end();
	}
	if ((p_flags & STEP_SHIFT) && shift_count > 0) {
		rd->buffer_update(buf_tile_ops, 0, shift_count * 16, p_shift.ptr());
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline);
		_dispatch(cl, MODE_SHIFT, shift_count, field_groups);
		rd->compute_list_end();
		cur = 1 - cur;
	}

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline);
	if (p_flags & STEP_RELAX) {
		_dispatch(cl, MODE_RELAX, 0, field_groups);
	}
	if ((p_flags & STEP_STAMPS) && stamp_count > 0) {
		_dispatch(cl, MODE_STAMPS, stamp_count, 1);
	}
	if (p_flags & STEP_SLUMP) {
		_dispatch(cl, MODE_SLUMP_GIVE, 0, field_groups);
		_dispatch(cl, MODE_SLUMP_TAKE, 0, field_groups);
		cur = 1 - cur;
	}
	if (p_flags & STEP_BLIT) {
		_dispatch(cl, MODE_BLIT, 0, field_groups);
	}
	if ((p_flags & STEP_COLLISION) && d.coll_w > 0) {
		_dispatch(cl, MODE_COLLISION, 0, (d.coll_w * d.coll_w + WG - 1) / WG);
	}
	if (p_flags & (STEP_READBACK | STEP_SYNC)) {
		_dispatch(cl, MODE_PACK, 0, field_groups);
	}
	rd->compute_list_end();

	if (p_flags & STEP_SYNC) {
		const Vector<uint8_t> fdata = rd->buffer_get_data(buf_packed, 0, n * n * 4);
		rt_on_field(fdata, p_origin);
		if (d.coll_w > 0) {
			const Vector<uint8_t> cdata = rd->buffer_get_data(buf_collision, 0, d.coll_w * d.coll_w * 4);
			rt_on_collision(cdata, p_collision_center);
		}
		return;
	}
	if ((p_flags & STEP_COLLISION) && d.coll_w > 0) {
		rd->buffer_get_data_async(buf_collision, callable_mp(this, &SnowDeformGPU::rt_on_collision).bind(p_collision_center), 0, d.coll_w * d.coll_w * 4);
	}
	if (p_flags & STEP_READBACK) {
		rd->buffer_get_data_async(buf_packed, callable_mp(this, &SnowDeformGPU::rt_on_field).bind(p_origin), 0, n * n * 4);
	}
}

void SnowDeformGPU::rt_on_collision(const PackedByteArray &p_data, Vector2 p_center) {
	PackedFloat32Array h;
	h.resize(p_data.size() / 4);
	memcpy(h.ptrw(), p_data.ptr(), h.size() * 4);
	MutexLock lock(mutex);
	collision_heights = h;
	collision_center = p_center;
	collision_ready = true;
}

void SnowDeformGPU::rt_on_field(const PackedByteArray &p_data, Vector2i p_origin) {
	MutexLock lock(mutex);
	field_packed = p_data;
	field_origin = p_origin;
	field_ready = true;
}

void SnowDeformGPU::rt_free() {
	if (!rd) {
		return;
	}
	_rt_free_buffers();
	if (buf_params.is_valid()) {
		rd->free_rid(buf_params);
		buf_params = RID();
	}
	if (shader.is_valid()) {
		rd->free_rid(shader); // frees the pipeline with it
		shader = RID();
		pipeline = RID();
	}
	compiled = false;
	MutexLock lock(mutex);
	texture_rid = RID();
}

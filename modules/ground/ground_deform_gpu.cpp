/**************************************************************************/
/*  ground_deform_gpu.cpp                                                 */
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

#include "ground_deform_gpu.h"

#include "ground_deform.glsl.gen.h"
#include "ground_deform_field.h"

#include "core/object/callable_mp.h"
#include "core/templates/hash_set.h"
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
	MODE_WATER_FLUX = 9,
	MODE_WATER_DEPTH = 10,
	MODE_WATER_COPY = 11,
	MODE_WATER_ERODE = 12,
	MODE_WATER_SED = 13,
	MODE_DRIFT_GIVE = 14,
	MODE_MUD_FLUX = 15,
	MODE_MUD_DEPTH = 16,
};

bool GroundDeformGPU::is_available() {
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs && rs->get_rendering_device() != nullptr;
}

GroundDeformGPU::GroundDeformGPU() {
	RenderingServer *rs = RenderingServer::get_singleton();
	rd = rs ? rs->get_rendering_device() : nullptr;
}

GroundDeformGPU::~GroundDeformGPU() {
}

void GroundDeformGPU::shutdown() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !rd) {
		return;
	}
	rs->call_on_render_thread(callable_mp(this, &GroundDeformGPU::rt_free));
	rs->sync();
}

RID GroundDeformGPU::get_texture() const {
	MutexLock lock(mutex);
	return texture_rid;
}

bool GroundDeformGPU::take_collision(PackedFloat32Array &r_heights, Vector2 &r_center) {
	MutexLock lock(mutex);
	if (!collision_ready) {
		return false;
	}
	r_heights = collision_heights;
	r_center = collision_center;
	collision_ready = false;
	return true;
}

bool GroundDeformGPU::take_field(PackedByteArray &r_packed, Vector2i &r_origin) {
	MutexLock lock(mutex);
	if (!field_ready) {
		return false;
	}
	r_packed = field_packed;
	r_origin = field_origin;
	field_ready = false;
	return true;
}

void GroundDeformGPU::step(const Dims &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center, const PackedByteArray &p_uploads) {
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
	d.push_back(p_dims.stride);
	d.push_back(MAX(p_dims.slump_passes, 1));
	d.push_back(p_dims.cover_vec4s);
	d.push_back(p_dims.water_passes);
	d.push_back(p_dims.morph_passes);
	d.push_back(p_dims.mud_passes);
	rs->call_on_render_thread(callable_mp(this, &GroundDeformGPU::rt_step).bind(d, p_flags, p_params, p_ground, p_cover, p_stamps, p_stores, p_shift, p_origin, p_collision_center, p_uploads));
	if (p_flags & STEP_SYNC) {
		rs->sync();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Render thread

void GroundDeformGPU::_rt_compile() {
	if (compiled || failed) {
		return;
	}
	Ref<RDShaderFile> sf;
	sf.instantiate();
	if (sf->parse_versions_from_text(ground_deform_shader_glsl) != OK) {
		ERR_PRINT("Ground deform field: the print field's compute shader failed to compile; staying on the CPU.");
		failed = true;
		return;
	}
	shader = rd->shader_create_from_spirv(sf->get_spirv_stages());
	if (shader.is_null()) {
		ERR_PRINT("Ground deform field: the print field's compute shader failed to build; staying on the CPU.");
		failed = true;
		return;
	}
	pipeline = rd->compute_pipeline_create(shader);
	buf_params = rd->uniform_buffer_create(PARAMS_BYTES);
	compiled = pipeline.is_valid();
	failed = !compiled;
}

RID GroundDeformGPU::_rt_storage(int p_bytes) {
	const int bytes = MAX(p_bytes, 16);
	RID b = rd->storage_buffer_create(bytes);
	rd->buffer_clear(b, 0, bytes);
	return b;
}

void GroundDeformGPU::_rt_free_buffers() {
	for (int i = 0; i < 2; i++) {
		if (uset[i].is_valid() && rd->uniform_set_is_valid(uset[i])) {
			rd->free_rid(uset[i]);
		}
		uset[i] = RID();
	}
	RID *bufs[] = { &buf_state[0], &buf_state[1], &buf_give, &buf_ground, &buf_cover, &buf_stamps, &buf_collision, &buf_cache, &buf_tile_ops, &buf_packed, &buf_water, &buf_water_tmp, &buf_ledger, &buf_mud, &tex_out };
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

void GroundDeformGPU::_rt_make_sets() {
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
		add(12, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_water);
		add(13, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_water_tmp);
		add(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_ledger);
		add(15, RD::UNIFORM_TYPE_STORAGE_BUFFER, buf_mud);
		uset[i] = rd->uniform_set_create(uniforms, shader, 0);
	}
}

void GroundDeformGPU::_rt_ensure(const Dims &p_dims) {
	bool remake = false;
	if (p_dims.n != dims.n || p_dims.slots != dims.slots || p_dims.stride != dims.stride || p_dims.cover_vec4s != dims.cover_vec4s) {
		// The field itself: start over.
		_rt_free_buffers();
		const int n = p_dims.n;
		buf_state[0] = _rt_storage(n * n * 16);
		buf_state[1] = _rt_storage(n * n * 16);
		buf_give = _rt_storage(n * n * 8);
		// (plus the water ledger's copy after the field: GroundSurface3D::add_water)
		buf_packed = _rt_storage(n * n * 4 * packed_uints(p_dims.stride) + 16);
		buf_ledger = _rt_storage(16);
		// Standing water is the ground's: snow binds a token buffer.
		buf_water = _rt_storage(p_dims.stride == 2 ? n * n * 48 : 16);
		buf_water_tmp = _rt_storage(p_dims.stride == 2 ? n * n * 4 * 6 : 16);
		buf_mud = _rt_storage(p_dims.stride == 2 ? n * n * 16 : 16); // the shifted depth, then the +x and +z faces' discharges, then the shifted clay and regrowth, then the runoff's soil concentration
		buf_cache = _rt_storage(MAX(p_dims.slots, 1) * TILE * TILE * 4 * cache_uints(p_dims.stride));
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = n;
		tf.height = n;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		tex_out = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(tex_out, Color(0, 0, 1, 1), 0, 1, 0, 1);
		buf_ground = _rt_storage(MAX(p_dims.gn * p_dims.gn, 1) * 4);
		buf_cover = _rt_storage(MAX(p_dims.cn * p_dims.cn, 1) * 16 * p_dims.cover_vec4s);
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
	grow(buf_cover, MAX(p_dims.cn * p_dims.cn, 1) * 16 * p_dims.cover_vec4s, MAX(dims.cn * dims.cn, 1) * 16 * dims.cover_vec4s);
	grow(buf_collision, MAX(p_dims.coll_w * p_dims.coll_w, 1) * 4, MAX(dims.coll_w * dims.coll_w, 1) * 4);
	dims = p_dims;
	if (remake) {
		_rt_make_sets();
	}
}

void GroundDeformGPU::_dispatch(int64_t p_list, int p_mode, int p_count, int p_groups) {
	if (p_groups <= 0) {
		return;
	}
	const int32_t push[4] = { p_mode, p_count, 0, 0 };
	rd->compute_list_bind_uniform_set(p_list, uset[cur], 0);
	rd->compute_list_set_push_constant(p_list, push, sizeof(push));
	rd->compute_list_dispatch(p_list, p_groups, 1, 1);
	rd->compute_list_add_barrier(p_list);
}

void GroundDeformGPU::rt_step(const PackedInt32Array &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center, const PackedByteArray &p_uploads) {
	_rt_compile();
	if (failed || p_dims.size() < 11 || p_dims[0] <= 0 || p_dims[5] < 1 || p_dims[5] > 2 || p_params.size() != PARAMS_BYTES) {
		return;
	}
	Dims d;
	d.n = p_dims[0];
	d.gn = p_dims[1];
	d.cn = p_dims[2];
	d.slots = p_dims[3];
	d.coll_w = p_dims[4];
	d.stride = p_dims[5];
	d.slump_passes = p_dims[6];
	d.cover_vec4s = CLAMP(p_dims[7], 2, 4);
	d.water_passes = MAX(p_dims[8], 0);
	d.morph_passes = CLAMP(p_dims[9], 1, 32);
	d.mud_passes = CLAMP(p_dims[10], 0, 16);
	_rt_ensure(d);
	const int n = d.n;
	const int field_groups = (n * n + WG - 1) / WG;
	rd->buffer_update(buf_params, 0, PARAMS_BYTES, p_params.ptr());
	if ((p_flags & STEP_GROUND) && p_ground.size() == d.gn * d.gn * 4) {
		rd->buffer_update(buf_ground, 0, p_ground.size(), p_ground.ptr());
	}
	if ((p_flags & STEP_COVER) && p_cover.size() == d.cn * d.cn * 16 * d.cover_vec4s) {
		rd->buffer_update(buf_cover, 0, p_cover.size(), p_cover.ptr());
	}
	if (p_flags & STEP_RESET) {
		rd->buffer_clear(buf_state[0], 0, n * n * 16);
		rd->buffer_clear(buf_state[1], 0, n * n * 16);
		if (d.stride == 2) {
			rd->buffer_clear(buf_water, 0, n * n * 48);
			rd->buffer_clear(buf_mud, 0, n * n * 16);
		}
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
	{
		const int tile_bytes = TILE * TILE * 4 * cache_uints(d.stride);
		const int rec = 4 + tile_bytes;
		for (int at = 0; at + rec <= p_uploads.size(); at += rec) {
			int32_t slot;
			memcpy(&slot, p_uploads.ptr() + at, 4);
			if (slot >= 0 && slot < d.slots) {
				rd->buffer_update(buf_cache, slot * tile_bytes, tile_bytes, p_uploads.ptr() + at + 4);
			}
		}
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
		if (d.stride == 2) {
			_dispatch(cl, MODE_WATER_COPY, 0, field_groups);
		}
		rd->compute_list_end();
		cur = 1 - cur;
		if (d.stride == 2) {
			// The mud's waves start still in the moved field (they are seconds long).
			rd->buffer_clear(buf_mud, 0, n * n * 16);
		}
	}

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline);
	if (p_flags & STEP_RELAX) {
		_dispatch(cl, MODE_DRIFT_GIVE, 0, field_groups);
		_dispatch(cl, MODE_RELAX, 0, field_groups);
	}
	if ((p_flags & STEP_STAMPS) && stamp_count > 0) {
		_dispatch(cl, MODE_STAMPS, stamp_count, 1);
	}
	if (p_flags & STEP_SLUMP) {
		for (int k = 0; k < MAX(d.slump_passes, 1); k++) {
			_dispatch(cl, MODE_SLUMP_GIVE, 0, field_groups);
			_dispatch(cl, MODE_SLUMP_TAKE, 0, field_groups);
			cur = 1 - cur;
		}
	}
	if ((p_flags & STEP_MUDWAVE) && d.stride == 2) {
		for (int k = 0; k < d.mud_passes; k++) {
			_dispatch(cl, MODE_MUD_FLUX, 0, field_groups);
			_dispatch(cl, MODE_MUD_DEPTH, 0, field_groups);
		}
	}
	if ((p_flags & STEP_WATER) && d.stride == 2) {
		for (int k = 0; k < d.water_passes; k++) {
			_dispatch(cl, MODE_WATER_FLUX, 0, field_groups);
			_dispatch(cl, MODE_WATER_DEPTH, 0, field_groups);
			if (p_flags & STEP_RUNOFF) {
				for (int m = 0; m < d.morph_passes; m++) {
					_dispatch(cl, MODE_WATER_ERODE, 0, field_groups);
					_dispatch(cl, MODE_WATER_SED, 0, field_groups);
				}
			}
		}
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
		const Vector<uint8_t> fdata = rd->buffer_get_data(buf_packed, 0, n * n * 4 * packed_uints(d.stride) + 16);
		rt_on_field(fdata, p_origin);
		if (d.coll_w > 0) {
			const Vector<uint8_t> cdata = rd->buffer_get_data(buf_collision, 0, d.coll_w * d.coll_w * 4);
			rt_on_collision(cdata, p_collision_center);
		}
		return;
	}
	// One read back of each kind in flight at a time. They complete as the device advances frames; with the
	// render loop off (RenderingServer.render_loop_enabled) none does, and queuing one each step overran the
	// device's download staging blocks (a crash). The next step after one lands asks again.
	if ((p_flags & STEP_COLLISION) && d.coll_w > 0 && !collision_in_flight) {
		collision_in_flight = true;
		rd->buffer_get_data_async(buf_collision, callable_mp(this, &GroundDeformGPU::rt_on_collision).bind(p_collision_center), 0, d.coll_w * d.coll_w * 4);
	}
	if ((p_flags & STEP_READBACK) && !field_in_flight) {
		field_in_flight = true;
		rd->buffer_get_data_async(buf_packed, callable_mp(this, &GroundDeformGPU::rt_on_field).bind(p_origin), 0, n * n * 4 * packed_uints(d.stride) + 16);
	}
}

void GroundDeformGPU::rt_on_collision(const PackedByteArray &p_data, Vector2 p_center) {
	collision_in_flight = false;
	PackedFloat32Array h;
	h.resize(p_data.size() / 4);
	memcpy(h.ptrw(), p_data.ptr(), h.size() * 4);
	MutexLock lock(mutex);
	collision_heights = h;
	collision_center = p_center;
	collision_ready = true;
}

void GroundDeformGPU::rt_on_field(const PackedByteArray &p_data, Vector2i p_origin) {
	field_in_flight = false;
	MutexLock lock(mutex);
	field_packed = p_data;
	// The water ledger rides after the field (MODE_PACK copies it there).
	if (field_packed.size() >= 16) {
		memcpy(&water_owed_mm3, field_packed.ptr() + field_packed.size() - 16, 4);
		memcpy(water_moved_mm3, field_packed.ptr() + field_packed.size() - 12, 8);
		field_packed.resize(field_packed.size() - 16);
	}
	field_origin = p_origin;
	field_ready = true;
}

void GroundDeformGPU::rt_free() {
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

// ---------------------------------------------------------------------------------------------------------
// Main thread

void GroundDeformGPUHost::release() {
	if (gpu.is_valid()) {
		gpu->shutdown();
		gpu.unref();
	}
	if (texture.is_valid()) {
		texture->set_texture_rd_rid(RID());
	}
	texture_serial = 0;
	n = 0;
	reset = true;
	clear_cache();
	stamps.clear();
	stores.clear();
	shift.clear();
	uploads.clear();
}

void GroundDeformGPUHost::clear_cache() {
	cache.clear();
	free_slots.clear();
	slots = 0;
}

bool GroundDeformGPUHost::stamp_bounds(const GroundDeformField &p_field, Vector3 p_position, float p_span, int &r_x0, int &r_x1, int &r_z0, int &r_z1) {
	const int fn = p_field.resolution;
	const float t = p_field.texel();
	r_x0 = MAX(int(Math::floor((p_position.x - p_span) / t)) - p_field.origin.x, 0);
	r_x1 = MIN(int(Math::ceil((p_position.x + p_span) / t)) - p_field.origin.x, fn - 1);
	r_z0 = MAX(int(Math::floor((p_position.z - p_span) / t)) - p_field.origin.y, 0);
	r_z1 = MIN(int(Math::ceil((p_position.z + p_span) / t)) - p_field.origin.y, fn - 1);
	return r_x0 <= r_x1 && r_z0 <= r_z1;
}

void GroundDeformGPUHost::add_stamp(GroundDeformField &r_field, const float p_v[12], const float p_d[4], int p_x0, int p_x1, int p_z0, int p_z1) {
	const int32_t e[4] = { p_x0, p_x1, p_z0, p_z1 };
	const int at = stamps.size();
	stamps.resize(at + GroundDeformGPU::STAMP_BYTES);
	uint8_t *w = stamps.ptrw() + at;
	memcpy(w, p_v, 48);
	memcpy(w + 48, p_d, 16);
	memcpy(w + 64, e, 16);
	// The tiles it touches are live (they go to the cache when the field moves off them).
	const int T = GroundDeformField::TILE;
	const int nt = r_field.tiles();
	if (int(tile_stamped.size()) != nt * nt) {
		tile_stamped.resize(nt * nt);
		for (float &s : tile_stamped) {
			s = -1e9f;
		}
	}
	for (int tz = p_z0 / T; tz <= p_z1 / T; tz++) {
		for (int tx = p_x0 / T; tx <= p_x1 / T; tx++) {
			tile_stamped[tz * nt + tx] = time;
			r_field.tile_active[tz * nt + tx] = 1;
		}
	}
}

void GroundDeformGPUHost::shift_to(const GroundDeformField &p_field, Vector2i p_origin, int p_resolution, int p_max_cached_tiles) {
	const int T = GroundDeformField::TILE;
	const int fn = p_resolution;
	const int nt = fn / T;
	const bool keep = p_field.valid && n == fn && !reset;
	const Vector2i old0 = p_field.origin / T;
	const Vector2i new0 = p_origin / T;
	const int want_slots = MAX(p_max_cached_tiles, 1);
	if (slots != want_slots) {
		clear_cache();
		slots = want_slots;
		for (int i = slots - 1; i >= 0; i--) {
			free_slots.push_back(i);
		}
	}
	stores.clear();
	shift.clear();
	shift.resize(nt * nt * 16);
	int32_t *sh = reinterpret_cast<int32_t *>(shift.ptrw());
	// Tiles coming in: from the old field where it overlaps, else from the cache.
	LocalVector<int> loaded;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = new0 + Vector2i(tx, tz);
			const Vector2i o = w - old0;
			int32_t *op = sh + (tz * nt + tx) * 4;
			op[0] = 0;
			op[1] = 0;
			op[2] = 0;
			op[3] = 0;
			if (keep && o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) {
				op[0] = 1;
				op[1] = o.y * nt + o.x;
			} else if (Slot *c = cache.getptr(w)) {
				op[0] = 2;
				op[1] = c->slot;
				loaded.push_back(c->slot);
				cache.erase(w);
			}
		}
	}
	// Tiles leaving with prints in them go to the cache (the least recently used make way), never into a
	// slot being read in this same move.
	if (keep && p_max_cached_tiles > 0) {
		HashSet<int> busy;
		for (int sl : loaded) {
			busy.insert(sl);
		}
		for (int tz = 0; tz < nt; tz++) {
			for (int tx = 0; tx < nt; tx++) {
				const Vector2i w = old0 + Vector2i(tx, tz);
				const Vector2i l = w - new0;
				if (l.x >= 0 && l.y >= 0 && l.x < nt && l.y < nt) {
					continue;
				}
				const int ti = tz * nt + tx;
				const bool stamped = int(tile_stamped.size()) == nt * nt && time - tile_stamped[ti] < 1.5;
				if (!(int(p_field.tile_active.size()) == nt * nt && p_field.tile_active[ti]) && !stamped) {
					continue;
				}
				int slot = -1;
				for (int k = int(free_slots.size()) - 1; k >= 0; k--) {
					if (!busy.has(free_slots[k])) {
						slot = free_slots[k];
						free_slots.remove_at(k);
						break;
					}
				}
				if (slot < 0) {
					// Evict the least recently used.
					Vector2i oldest;
					uint64_t best = UINT64_MAX;
					for (const KeyValue<Vector2i, Slot> &kv : cache) {
						if (kv.value.used < best && !busy.has(kv.value.slot)) {
							best = kv.value.used;
							oldest = kv.key;
						}
					}
					if (best == UINT64_MAX) {
						continue;
					}
					slot = cache[oldest].slot;
					cache.erase(oldest);
				}
				busy.insert(slot);
				cache.insert(w, Slot{ slot, ++cache_clock });
				const int32_t st[4] = { tx, tz, slot, 0 };
				const int at = stores.size();
				stores.resize(at + 16);
				memcpy(stores.ptrw() + at, st, 16);
			}
		}
	}
	for (int sl : loaded) {
		free_slots.push_back(sl);
	}
	// Stamps still waiting were placed in the old field's texels: they go first (the order in the step is
	// stores, shift, stamps), so move them to the new one.
	if (keep && stamps.size() > 0) {
		const Vector2i d = (old0 - new0) * T;
		const int count = stamps.size() / GroundDeformGPU::STAMP_BYTES;
		for (int i = 0; i < count; i++) {
			int32_t *e = reinterpret_cast<int32_t *>(stamps.ptrw() + i * GroundDeformGPU::STAMP_BYTES + 64);
			e[0] = MAX(e[0] + d.x, 0);
			e[1] = MIN(e[1] + d.x, fn - 1);
			e[2] = MAX(e[2] + d.y, 0);
			e[3] = MIN(e[3] + d.y, fn - 1);
		}
	} else {
		stamps.clear();
	}
	if (!keep) {
		reset = true;
		stores.clear();
	}
	if (int(tile_stamped.size()) == nt * nt && keep) {
		LocalVector<float> next;
		next.resize(nt * nt);
		for (int tz = 0; tz < nt; tz++) {
			for (int tx = 0; tx < nt; tx++) {
				const Vector2i o = new0 + Vector2i(tx, tz) - old0;
				next[tz * nt + tx] = (o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) ? tile_stamped[o.y * nt + o.x] : -1e9f;
			}
		}
		tile_stamped = next;
	}
	n = fn;
	changed = true;
}

void GroundDeformGPUHost::seed_tile(Vector2i p_tile, const uint32_t *p_data, int p_stride, int p_max_cached_tiles) {
	const int want_slots = MAX(p_max_cached_tiles, 1);
	if (slots != want_slots) {
		clear_cache();
		slots = want_slots;
		for (int i = slots - 1; i >= 0; i--) {
			free_slots.push_back(i);
		}
	}
	if (cache.has(p_tile)) {
		return;
	}
	int slot = -1;
	if (free_slots.size() > 0) {
		slot = free_slots[free_slots.size() - 1];
		free_slots.remove_at(free_slots.size() - 1);
	} else {
		Vector2i oldest;
		uint64_t best = UINT64_MAX;
		for (const KeyValue<Vector2i, Slot> &kv : cache) {
			if (kv.value.used < best) {
				best = kv.value.used;
				oldest = kv.key;
			}
		}
		if (best == UINT64_MAX) {
			return;
		}
		slot = cache[oldest].slot;
		cache.erase(oldest);
	}
	cache.insert(p_tile, Slot{ slot, ++cache_clock });
	const int T = GroundDeformField::TILE;
	const int bytes = T * T * 4 * p_stride;
	const int at = uploads.size();
	uploads.resize(at + 4 + bytes);
	const int32_t s = slot;
	memcpy(uploads.ptrw() + at, &s, 4);
	memcpy(uploads.ptrw() + at + 4, p_data, bytes);
}

bool GroundDeformGPUHost::take_texture() {
	if (gpu.is_null()) {
		return false;
	}
	if (texture.is_null()) {
		texture.instantiate();
	}
	if (gpu->get_texture_serial() == texture_serial) {
		return false;
	}
	texture_serial = gpu->get_texture_serial();
	texture->set_texture_rd_rid(gpu->get_texture());
	return true;
}

bool GroundDeformGPUHost::take_field(GroundDeformField &r_field, int p_stride, float *r_owed) {
	PackedByteArray packed;
	Vector2i origin;
	if (gpu.is_null() || !gpu->take_field(packed, origin) || origin != r_field.origin || !r_field.valid) {
		return false;
	}
	const int fn = r_field.resolution;
	const bool fresh = p_stride == 2 && int(r_field.fresh.size()) == fn * fn;
	if (packed.size() != fn * fn * 4 * GroundDeformGPU::packed_uints(p_stride) || int(r_field.dent.size()) != fn * fn) {
		return false;
	}
	if (fresh && int(r_field.water.size()) != fn * fn) {
		r_field.water.resize(fn * fn);
	}
	if (fresh && int(r_field.water_cut.size()) != fn * fn) {
		r_field.water_cut.resize(fn * fn);
	}
	if (fresh && int(r_field.water_mud.size()) != fn * fn) {
		r_field.water_mud.resize(fn * fn);
	}
	if (fresh && int(r_field.regrown.size()) != fn * fn) {
		r_field.regrown.resize(fn * fn);
	}
	const int T = GroundDeformField::TILE;
	const uint16_t *h = reinterpret_cast<const uint16_t *>(packed.ptr());
	const int nt = r_field.tiles();
	for (int i = 0; i < nt * nt; i++) {
		r_field.tile_active[i] = 0;
	}
	float owed = 0.0f;
	for (int z = 0; z < fn; z++) {
		for (int x = 0; x < fn; x++) {
			const int i = z * fn + x;
			const float dn = Math::half_to_float(h[i * 2 * p_stride + 0]);
			const float rm = Math::half_to_float(h[i * 2 * p_stride + 1]);
			const float fr = fresh ? Math::half_to_float(h[i * 2 * p_stride + 2]) : 0.0f;
			r_field.dent[i] = dn;
			r_field.rim[i] = rm;
			if (fresh) {
				r_field.fresh[i] = fr;
				owed = MAX(owed, Math::half_to_float(h[i * 4 + 3]));
			}
			if (fresh) {
				r_field.water[i] = Math::half_to_float(h[(fn * fn * 2 + i) * 2]);
				r_field.water_cut[i] = Math::half_to_float(h[(fn * fn * 2 + i) * 2 + 1]);
				r_field.water_mud[i] = Math::half_to_float(h[(fn * fn * 3 + i) * 2]);
				r_field.regrown[i] = Math::half_to_float(h[(fn * fn * 3 + i) * 2 + 1]);
			}
			if (dn > 0.0f || rm > 0.0f || fr > 0.0f) {
				r_field.tile_active[(z / T) * nt + x / T] = 1;
			}
		}
	}
	if (r_owed) {
		*r_owed = owed;
	}
	return true;
}

bool GroundDeformGPUHost::pull_cached(const GroundDeformField &p_field, const LocalVector<Vector2i> &p_tiles) {
	const int T = GroundDeformField::TILE;
	const int nt = p_field.resolution / T;
	if (reset || gpu.is_null() || !p_field.valid || n != p_field.resolution || shift.size() > 0 || stores.size() > 0) {
		return false;
	}
	const Vector2i t0 = p_field.origin / T;
	shift.resize(nt * nt * 16);
	int32_t *sh = reinterpret_cast<int32_t *>(shift.ptrw());
	for (int i = 0; i < nt * nt; i++) {
		sh[i * 4 + 0] = 1; // kept where it is
		sh[i * 4 + 1] = i;
		sh[i * 4 + 2] = 0;
		sh[i * 4 + 3] = 0;
	}
	for (const Vector2i &w : p_tiles) {
		const Vector2i l = w - t0;
		Slot *c = cache.getptr(w);
		if (!c || l.x < 0 || l.y < 0 || l.x >= nt || l.y >= nt) {
			continue;
		}
		int32_t *op = sh + (l.y * nt + l.x) * 4;
		op[0] = 2;
		op[1] = c->slot;
		free_slots.push_back(c->slot); // read before anything is stored into it again (stores come first, next step)
		cache.erase(w);
	}
	changed = true;
	return true;
}

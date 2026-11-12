/**************************************************************************/
/*  hair_strand_solver.cpp                                                */
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
// std140 Params in hair_strands.glsl: 7 vec4, 4 mat4, MAX_SPHERES vec4, 3 vec4 (SDF), 2 vec4 (volume),
// 2 + 4 per map vec4 (deep opacity maps), 1 vec4 (strand-strand collision).
// Then 1 vec4 and 1 mat4 (the cloth).
constexpr int PARAMS_BYTES = 7 * 16 + 4 * 64 + HairStrandSolverGPU::MAX_SPHERES * 16 + (8 + HairStrandSolverGPU::DOM_SLOTS * 4) * 16 + 16 + 64;
constexpr int SDF_PARAMS = (7 * 16 + 4 * 64 + HairStrandSolverGPU::MAX_SPHERES * 16) / 4; // float index
constexpr uint32_t GROUP = 64; // local_size_x

uint32_t groups_for(int p_count) {
	return (uint32_t)((MAX(p_count, 1) + (int)GROUP - 1) / (int)GROUP);
}

enum Mode {
	MODE_STEP = 0,
	MODE_RESET = 1,
	MODE_OUTPUT = 2,
	MODE_FOLLOW = 3,
	MODE_SORT_KEYS = 4,
	MODE_SORT_STEP = 5,
	MODE_SDF_CLEAR = 6,
	MODE_SDF_BUILD = 7,
	MODE_SDF_COLLIDE = 8,
	MODE_SDF_SEED = 9,
	MODE_SDF_JUMP = 10,
	MODE_SDF_FINAL = 11,
	MODE_VOL_CLEAR = 12,
	MODE_VOL_SPLAT = 13,
	MODE_DOM_CLEAR = 14,
	MODE_DOM_FRONT = 15,
	MODE_DOM_OPAQUE = 16,
	MODE_DOM_OPAQUE_TRIS = 17,
	MODE_DOM_ACCUM = 18,
	MODE_DOM_RESOLVE = 19,
	MODE_COLL_CLEAR = 20,
	MODE_COLL_INSERT = 21,
	MODE_COLL_SOLVE = 22,
	MODE_COLL_APPLY = 23,
	MODE_SDF_CLOTH = 24,
};
constexpr int VOL_PARAMS = SDF_PARAMS + 12; // float index
constexpr int VOL_CELLS = 32; // a side
constexpr int DOM_PARAMS = VOL_PARAMS + 8; // float index
constexpr int COLL_PARAMS = DOM_PARAMS + 8 + HairStrandSolverGPU::DOM_SLOTS * 16; // float index
constexpr int COLL_BUCKET = 8; // points a hash bucket holds (hair_strands.glsl)
constexpr int CLOTH_PARAMS = COLL_PARAMS + 4; // float index: cloth_misc, then cloth_xform

struct PassConstants {
	int32_t mode;
	float frac;
	float frac_prev;
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
	RID *rids[] = { &buf_params, &buf_rest, &buf_strands, &buf_pos, &buf_prev, &buf_follow, &buf_order, &tex_out, &tex_out_prev, &buf_last, &buf_dom_depth, &buf_dom_accum, &tex_dom, &buf_tris, &buf_sdf, &buf_vol, &buf_vol_rest, &buf_render_points, &buf_coll_cells, &buf_coll_delta, &buf_point_strand, &tex_cloth_dummy, &buf_cloth_tris };
	history = false;
	tex_cloth = RID();
	cloth_tri_count = 0;
	vol_cells = 0;
	tris_capacity = 0;
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

void HairStrandSolverGPU::rt_build(Ref<HairStrandSolverGPU> p_self, PackedFloat32Array p_rest, PackedInt32Array p_strands, PackedFloat32Array p_follow, int p_slot_points, PackedInt32Array p_render_points, PackedFloat32Array p_vol_rest, int p_vol_cells) {
	if (rd == nullptr || shader.is_null()) {
		return;
	}
	rt_free_buffers();
	point_count = p_rest.size() / 4;
	strand_count = p_strands.size() / 2;
	if (point_count == 0 || strand_count == 0 || p_follow.size() != strand_count * 8) {
		return;
	}
	buf_params = rd->uniform_buffer_create(PARAMS_BYTES);
	buf_rest = storage_from(rd, (const uint8_t *)p_rest.ptr(), p_rest.size() * 4);
	buf_strands = storage_from(rd, (const uint8_t *)p_strands.ptr(), p_strands.size() * 4);
	buf_pos = storage_from(rd, nullptr, point_count * 16);
	buf_prev = storage_from(rd, nullptr, point_count * 16);
	buf_follow = storage_from(rd, (const uint8_t *)p_follow.ptr(), p_follow.size() * 4);
	slot_points = MAX(p_slot_points, 0);
	{
		// The points drawn per strand; every point when none are given.
		PackedInt32Array rp = p_render_points;
		if (rp.size() < strand_count * 2) {
			rp.resize(strand_count * 2 + point_count);
			int *w = rp.ptrw();
			int at = 0;
			for (int s = 0; s < strand_count; s++) {
				const int n = p_strands[s * 2 + 1];
				w[s * 2] = at;
				w[s * 2 + 1] = n;
				for (int k = 0; k < n; k++) {
					w[strand_count * 2 + at + k] = k;
				}
				at += n;
			}
		}
		buf_render_points = storage_from(rd, (const uint8_t *)rp.ptr(), rp.size() * 4);
	}
	{
		// Strand-strand collision: each point's strand, and a spatial hash sized for the guides'
		// points (about two buckets each).
		PackedInt32Array owner;
		owner.resize(point_count);
		int *o = owner.ptrw();
		int guide_points = 0;
		for (int s = 0; s < strand_count; s++) {
			const int first = p_strands[s * 2];
			const int n = p_strands[s * 2 + 1];
			for (int k = 0; k < n && first + k < point_count; k++) {
				o[first + k] = s;
			}
			if (p_follow[s * 8 + 3] < 0.0f) {
				guide_points += n;
			}
		}
		buf_point_strand = storage_from(rd, (const uint8_t *)owner.ptr(), point_count * 4);
		coll_buckets = 1;
		while (coll_buckets < guide_points * 2) {
			coll_buckets <<= 1;
		}
		buf_coll_cells = storage_from(rd, nullptr, coll_buckets * (COLL_BUCKET + 1) * 4);
		buf_coll_delta = storage_from(rd, nullptr, point_count * 16);
	}
	sort_size = 1;
	while (sort_size < strand_count) {
		sort_size <<= 1;
	}
	buf_order = storage_from(rd, nullptr, sort_size * 8);

	const int texels = slot_points > 0 ? strand_count * slot_points : point_count;
	tex_width = MIN(texels, 2048);
	tex_height = (texels + tex_width - 1) / tex_width;
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
	tf.usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	tex_out_prev = rd->texture_create(tf, RD::TextureView(), data);
	buf_last = storage_from(rd, nullptr, point_count * 16);
	history = false;

	const int dom_cells = DOM_CELLS * DOM_CELLS;
	buf_dom_depth = storage_from(rd, nullptr, DOM_SLOTS * dom_cells * 2 * 4);
	buf_dom_accum = storage_from(rd, nullptr, DOM_SLOTS * dom_cells * 3 * 4);
	RD::TextureFormat df;
	df.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	df.width = DOM_CELLS * 2;
	df.height = DOM_CELLS * DOM_SLOTS;
	df.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> far;
	far.resize(df.width * df.height * 16);
	{
		// No map yet: no hair and nothing opaque anywhere.
		float *fp = (float *)far.ptrw();
		for (uint32_t i = 0; i < df.width * df.height; i++) {
			fp[i * 4 + 0] = 1e9f;
			fp[i * 4 + 1] = 0.0f;
			fp[i * 4 + 2] = 0.0f;
			fp[i * 4 + 3] = 0.0f;
		}
	}
	Vector<Vector<uint8_t>> dom_data;
	dom_data.push_back(far);
	tex_dom = rd->texture_create(df, RD::TextureView(), dom_data);

	// No collision body yet: placeholders, grown on the first step that has one.
	buf_tris = storage_from(rd, nullptr, 16);
	buf_sdf = storage_from(rd, nullptr, 16);
	// No cloth: a one-texel stand-in for its positions, set_cloth binds the real one.
	{
		RD::TextureFormat cf;
		cf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		cf.width = 1;
		cf.height = 1;
		cf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		Vector<uint8_t> zero;
		zero.resize(16);
		memset(zero.ptrw(), 0, 16);
		Vector<Vector<uint8_t>> cd;
		cd.push_back(zero);
		tex_cloth_dummy = rd->texture_create(cf, RD::TextureView(), cd);
	}
	tex_cloth = tex_cloth_dummy;
	buf_cloth_tris = storage_from(rd, nullptr, 16);
	cloth_tri_count = 0;
	vol_cells = p_vol_rest.size() == p_vol_cells * p_vol_cells * p_vol_cells ? p_vol_cells : 0;
	const int vol_count = vol_cells * vol_cells * vol_cells;
	buf_vol = storage_from(rd, nullptr, vol_count * 16);
	buf_vol_rest = storage_from(rd, (const uint8_t *)p_vol_rest.ptr(), vol_cells > 0 ? vol_count * 4 : 0);
	rt_make_uniform_set();
	needs_reset = true;
	sorted = false;
	built.set();
}

void HairStrandSolverGPU::rt_make_uniform_set() {
	if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
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
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 6;
		u.append_id(buf_follow);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 7;
		u.append_id(buf_order);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 8;
		u.append_id(buf_tris);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 9;
		u.append_id(buf_sdf);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 10;
		u.append_id(buf_vol);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 11;
		u.append_id(buf_vol_rest);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 17;
		u.append_id(buf_render_points);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 18;
		u.append_id(buf_coll_cells);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 19;
		u.append_id(buf_coll_delta);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 20;
		u.append_id(buf_point_strand);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 12;
		u.append_id(tex_out_prev);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 13;
		u.append_id(buf_last);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 14;
		u.append_id(buf_dom_depth);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 15;
		u.append_id(buf_dom_accum);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 16;
		u.append_id(tex_dom);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 21;
		u.append_id(tex_cloth.is_valid() && rd->texture_is_valid(tex_cloth) ? tex_cloth : tex_cloth_dummy);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 22;
		u.append_id(buf_cloth_tris);
		uniforms.push_back(u);
	}
	uniform_set = rd->uniform_set_create(uniforms, shader, 0);
}

void HairStrandSolverGPU::rt_set_cloth(Ref<HairStrandSolverGPU> p_self, RID p_texture, PackedInt32Array p_tris) {
	if (rd == nullptr || !built.is_set()) {
		return;
	}
	const int n = p_tris.size() / 3;
	const bool valid = n > 0 && p_texture.is_valid() && rd->texture_is_valid(p_texture);
	if (buf_cloth_tris.is_valid()) {
		rd->free_rid(buf_cloth_tris);
	}
	PackedInt32Array packed;
	packed.resize(MAX(valid ? n : 0, 1) * 4);
	int *w = packed.ptrw();
	memset(w, 0, packed.size() * sizeof(int));
	for (int i = 0; valid && i < n; i++) {
		w[i * 4 + 0] = p_tris[i * 3 + 0];
		w[i * 4 + 1] = p_tris[i * 3 + 1];
		w[i * 4 + 2] = p_tris[i * 3 + 2];
	}
	buf_cloth_tris = storage_from(rd, (const uint8_t *)packed.ptr(), packed.size() * 4);
	tex_cloth = valid ? p_texture : tex_cloth_dummy;
	cloth_tri_count = valid ? n : 0;
	rt_make_uniform_set();
}

void HairStrandSolverGPU::rt_reset(Ref<HairStrandSolverGPU> p_self) {
	needs_reset = true;
	history = false;
}

void HairStrandSolverGPU::rt_hold(Ref<HairStrandSolverGPU> p_self) {
	if (rd == nullptr || !built.is_set() || tex_out.is_null() || tex_out_prev.is_null()) {
		return;
	}
	rd->texture_copy(tex_out, tex_out_prev, Vector3(), Vector3(), Vector3(tex_width, tex_height, 1), 0, 0, 0, 0);
	history = false; // on resuming, the first frame starts from where the hair is
	if (local) {
		rd->submit();
		rd->sync();
	}
}

void HairStrandSolverGPU::rt_step(Ref<HairStrandSolverGPU> p_self, PackedByteArray p_params, int p_substeps, PackedFloat32Array p_tris) {
	if (rd == nullptr || !built.is_set() || uniform_set.is_null() || p_params.size() != PARAMS_BYTES) {
		return;
	}
	if (tex_cloth != tex_cloth_dummy && !rd->texture_is_valid(tex_cloth)) {
		// The cloth was rebuilt or freed (its uniform set went with it): none until set again.
		tex_cloth = tex_cloth_dummy;
		cloth_tri_count = 0;
		rt_make_uniform_set();
	}
	const int body_tris = p_tris.size() / 12;
	// The cloth's triangles follow the body's in the same buffer (MODE_SDF_CLOTH writes them).
	const int cloth_count = (tex_cloth != tex_cloth_dummy && rd->texture_is_valid(tex_cloth)) ? MIN(cloth_tri_count, (int)(((const float *)p_params.ptr())[CLOTH_PARAMS] + 0.5f)) : 0;
	const int tri_count = body_tris + cloth_count;
	if (tri_count > 0) {
		if (tri_count > tris_capacity) {
			// Grown (with headroom) only when the body has more triangles than before.
			rd->free_rid(buf_tris);
			rd->free_rid(buf_sdf);
			tris_capacity = tri_count + tri_count / 4;
			buf_tris = storage_from(rd, nullptr, tris_capacity * 48);
			buf_sdf = storage_from(rd, nullptr, MAX_SDF_CELLS * 8);
			rt_make_uniform_set();
		}
		if (body_tris > 0) {
			rd->buffer_update(buf_tris, 0, body_tris * 48, p_tris.ptr());
		}
	}
	PackedByteArray params = p_params;
	int32_t *counts = (int32_t *)params.ptrw();
	counts[0] = strand_count;
	counts[1] = point_count;
	counts[2] = tex_width;
	float *fl = (float *)params.ptrw();
	fl[26] = (float)slot_points;
	fl[27] = (float)sort_size;
	fl[COLL_PARAMS + 1] = (float)coll_buckets;
	fl[CLOTH_PARAMS + 0] = (float)cloth_count;
	fl[CLOTH_PARAMS + 2] = (float)body_tris;
	counts[SDF_PARAMS + 7] = tri_count; // what this frame holds (the cloth may have gone)
	if (vol_cells == 0) {
		fl[VOL_PARAMS + 7] = 0.0f; // no grid: the volume stays off
	}
	rd->buffer_update(buf_params, 0, PARAMS_BYTES, params.ptr());

	PassConstants pc = { MODE_STEP, 0.0f, 0.0f, 0 };
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	auto run = [&](int p_mode, float p_frac, float p_frac_prev, int p_threads, int p_k = 0) {
		pc.mode = p_mode;
		pc.frac = p_frac;
		pc.frac_prev = p_frac_prev;
		pc.pad1 = p_k;
		rd->compute_list_set_push_constant(cl, &pc, sizeof(pc));
		rd->compute_list_dispatch(cl, groups_for(p_threads), 1, 1);
		rd->compute_list_add_barrier(cl);
	};
	if (needs_reset) {
		run(MODE_RESET, 1.0f, 1.0f, strand_count);
		needs_reset = false;
		history = false; // teleported: no motion this frame
	}
	if (fl[VOL_PARAMS + 7] > 0.5f) {
		run(MODE_VOL_CLEAR, 1.0f, 1.0f, vol_cells * vol_cells * vol_cells * 4);
		run(MODE_VOL_SPLAT, 1.0f, 1.0f, strand_count);
	}
	const int substeps = MAX(p_substeps, 1);
	for (int s = 0; s < substeps; s++) {
		run(MODE_STEP, (float)(s + 1) / (float)substeps, (float)s / (float)substeps, strand_count);
	}
	if (fl[COLL_PARAMS] > 0.0f && coll_buckets > 0) {
		// Strand-strand collision between the guides, once a frame, before the followers take
		// their places from them.
		run(MODE_COLL_CLEAR, 1.0f, 1.0f, coll_buckets);
		run(MODE_COLL_INSERT, 1.0f, 1.0f, point_count);
		run(MODE_COLL_SOLVE, 1.0f, 1.0f, point_count);
		run(MODE_COLL_APPLY, 1.0f, 1.0f, point_count);
	}
	run(MODE_FOLLOW, 1.0f, 1.0f, strand_count);
	if (tri_count > 0) {
		if (cloth_count > 0) {
			run(MODE_SDF_CLOTH, 1.0f, 1.0f, cloth_count);
		}
		const int32_t *sd = (const int32_t *)params.ptr() + SDF_PARAMS + 4;
		const int cells = sd[0] * sd[1] * sd[2];
		run(MODE_SDF_CLEAR, 1.0f, 1.0f, cells);
		run(MODE_SDF_BUILD, 1.0f, 1.0f, tri_count);
		run(MODE_SDF_SEED, 1.0f, 1.0f, tri_count);
		int k = 1;
		while (k * 2 < MAX(sd[0], MAX(sd[1], sd[2]))) {
			k *= 2;
		}
		for (; k >= 1; k /= 2) {
			run(MODE_SDF_JUMP, 1.0f, 1.0f, cells, k);
		}
		run(MODE_SDF_JUMP, 1.0f, 1.0f, cells, 1); // JFA+1: fixes most of its misses
		run(MODE_SDF_FINAL, 1.0f, 1.0f, cells);
		run(MODE_SDF_COLLIDE, 1.0f, 1.0f, strand_count);
	}
	const int dom_maps = CLAMP((int)fl[DOM_PARAMS + 3], 0, (int)DOM_SLOTS);
	for (int m = 0; m < dom_maps; m++) {
		// The deep opacity maps, one per light, from the final positions of every strand.
		const int dom_cells = DOM_CELLS * DOM_CELLS;
		run(MODE_DOM_CLEAR, 1.0f, 1.0f, dom_cells, m);
		run(MODE_DOM_OPAQUE, 1.0f, 1.0f, dom_cells, m);
		if (tri_count > 0) {
			run(MODE_DOM_OPAQUE_TRIS, 1.0f, 1.0f, tri_count, m);
		}
		run(MODE_DOM_FRONT, 1.0f, 1.0f, strand_count, m);
		run(MODE_DOM_ACCUM, 1.0f, 1.0f, strand_count, m);
		run(MODE_DOM_RESOLVE, 1.0f, 1.0f, dom_cells, m);
	}
	// Hair cards only: the cards' strands sit in the first slots once sorted, and nothing else
	// is drawn, so the sort is skipped and only their slots are written.
	const float card_flag = fl[SDF_PARAMS + 11];
	const bool cards_only = card_flag < 0.0f;
	const int card_strands = card_flag != 0.0f ? MAX(strand_count - (int)(Math::abs(card_flag) + 0.5f), 0) : 0;
	if (slot_points > 0) {
		if (!cards_only || !sorted) {
			// Draw order, far to near: bitonic sort of the strands by depth.
			run(MODE_SORT_KEYS, 1.0f, 1.0f, sort_size);
			for (int k = 2; k <= sort_size; k <<= 1) {
				for (int j = k >> 1; j > 0; j >>= 1) {
					run(MODE_SORT_STEP, (float)j, 0.0f, sort_size, k);
				}
			}
			sorted = true;
		}
		run(MODE_OUTPUT, 1.0f, 1.0f, (cards_only ? card_strands : strand_count) * slot_points, history ? 1 : 0);
	} else {
		run(MODE_OUTPUT, 1.0f, 1.0f, point_count, history ? 1 : 0);
	}
	history = true;
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

void HairStrandSolver::build(const PackedFloat32Array &p_rest, const PackedInt32Array &p_strands, const PackedFloat32Array &p_follow, int p_slot_points, const PackedInt32Array &p_render_points) {
	if (gpu.is_null()) {
		return;
	}
	has_last = false;

	// The hair volume grid (Petrovic et al. 2005), in groom space: the groom's bounds grown by half
	// its longest strand (room to swing), VOL_CELLS a side, and the groom's own density of guide
	// points in it, which the pressure keeps the hair near.
	vol_cells = 0;
	PackedFloat32Array vol_rest;
	const int strand_total = p_strands.size() / 2;
	if (strand_total > 0 && p_follow.size() == strand_total * 8 && p_rest.size() >= 4) {
		const float *r = p_rest.ptr();
		const int point_total = p_rest.size() / 4;
		AABB box(Vector3(r[0], r[1], r[2]), Vector3());
		float longest = 0.0f;
		for (int s = 0; s < strand_total; s++) {
			const int first = p_strands[s * 2];
			const int n = p_strands[s * 2 + 1];
			float length = 0.0f;
			for (int i = 0; i < n && first + i < point_total; i++) {
				const float *q = r + (first + i) * 4;
				box.expand_to(Vector3(q[0], q[1], q[2]));
				if (i > 0) {
					length += q[3];
				}
			}
			longest = MAX(longest, length);
		}
		const float pad = MAX(0.5f * longest, 0.02f);
		box = box.grow(pad);
		vol_cell = MAX(box.size.x, MAX(box.size.y, box.size.z)) / (float)VOL_CELLS;
		vol_origin = box.get_center() - Vector3(1, 1, 1) * (0.5f * vol_cell * VOL_CELLS);
		vol_rest.resize(VOL_CELLS * VOL_CELLS * VOL_CELLS);
		float *d = vol_rest.ptrw();
		memset(d, 0, vol_rest.size() * sizeof(float));
		// Same splat as MODE_VOL_SPLAT: guide strands, points past the root, trilinear.
		for (int s = 0; s < strand_total; s++) {
			if (p_follow[s * 8 + 3] >= 0.0f) {
				continue;
			}
			const int first = p_strands[s * 2];
			const int n = p_strands[s * 2 + 1];
			for (int i = 1; i < n && first + i < point_total; i++) {
				const float *q = r + (first + i) * 4;
				const Vector3 u = (Vector3(q[0], q[1], q[2]) - vol_origin) / vol_cell - Vector3(0.5f, 0.5f, 0.5f);
				const Vector3i c((int)Math::floor(u.x), (int)Math::floor(u.y), (int)Math::floor(u.z));
				const Vector3 f = u - Vector3(c);
				for (int k = 0; k < 8; k++) {
					const Vector3i o(k & 1, (k >> 1) & 1, k >> 2);
					const Vector3i cc = c + o;
					if (cc.x < 0 || cc.y < 0 || cc.z < 0 || cc.x >= VOL_CELLS || cc.y >= VOL_CELLS || cc.z >= VOL_CELLS) {
						continue;
					}
					const float w = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y) * (o.z ? f.z : 1.0f - f.z);
					d[(cc.z * VOL_CELLS + cc.y) * VOL_CELLS + cc.x] += w;
				}
			}
		}
		double sum = 0.0;
		int occupied = 0;
		for (int i = 0; i < vol_rest.size(); i++) {
			if (d[i] > 0.05f) {
				sum += d[i];
				occupied++;
			}
		}
		vol_mean = occupied > 0 ? (float)(sum / occupied) : 0.0f;
		vol_cells = vol_mean > 0.0f ? VOL_CELLS : 0;
		if (vol_cells == 0) {
			vol_rest.clear();
		}
	}
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_build).bind(gpu, p_rest, p_strands, p_follow, p_slot_points, p_render_points, vol_rest, vol_cells));
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
	// Wet hair (Ward, Galoppo and Lin 2004; Gupta and Magnenat-Thalmann 2007): the fibres take up
	// to 45% of their weight in water, so the same air drag moves them less (the decay toward the
	// air's velocity slows with the mass), and the water softens the keratin, so the groomed shape
	// holds less against gravity (bending x (1 - 0.45 w), Gupta).
	const float wet = CLAMP(p_settings.wet, 0.0f, 1.0f);
	const float wet_mass = 1.0f + 0.45f * wet;
	const float wet_bend = 1.0f - 0.45f * wet;
	f[5] = Math::exp(-MAX(p_settings.damping, 0.0f) * 60.0f * dt / wet_mass); // TressFX's decay, per substep
	f[6] = per_substep(p_settings.global_stiffness * wet_bend);
	f[7] = CLAMP(p_settings.global_range, 0.0f, 1.0f);
	// accel
	f[8] = p_settings.gravity.x;
	f[9] = p_settings.gravity.y;
	f[10] = p_settings.gravity.z;
	// TressFX halves the local stiffness and caps it (1.0 is unstable); given per 1/60 s.
	f[11] = per_substep(0.5f * MIN(MAX(p_settings.local_stiffness * wet_bend, 0.0f), 0.95f));
	// wind
	f[12] = p_settings.wind.x;
	f[13] = p_settings.wind.y;
	f[14] = p_settings.wind.z;
	f[15] = CLAMP(p_settings.gust, 0.0f, 1.0f);
	// misc
	f[16] = CLAMP(p_settings.vsp_coeff, 0.0f, 1.0f);
	f[17] = MAX(p_settings.margin, 0.0f);
	f[18] = (float)Math::fmod(time, 3600.0);
	f[19] = MAX(p_settings.vsp_accel_threshold, 0.0f);
	// air
	f[20] = MAX(p_settings.max_speed, 0.0f);
	f[21] = CLAMP(p_settings.turbulence, 0.0f, 1.0f);
	f[22] = MAX(p_settings.turbulence_size, 0.001f);
	f[23] = CLAMP(p_settings.lod_keep, 0.0f, 1.0f);
	// iterations
	f[24] = (float)CLAMP(p_settings.local_iterations, 0, 16);
	f[25] = (float)CLAMP(p_settings.length_iterations, 0, 16);
	// matrices
	put_mat4(f + 28, has_last ? last_xform : p_xform);
	put_mat4(f + 44, p_xform);
	put_mat4(f + 60, p_settings.output_xform);
	put_mat4(f + 76, p_settings.view_xform);
	// spheres
	for (int i = 0; i < ints[3]; i++) {
		const Vector4 &s = p_settings.spheres[i];
		f[92 + i * 4 + 0] = s.x;
		f[92 + i * 4 + 1] = s.y;
		f[92 + i * 4 + 2] = s.z;
		f[92 + i * 4 + 3] = s.w;
	}
	// The collision body: its triangles, padded to vec4, and a grid around them (TressFX's SDF),
	// cells grown so the grid fits in 64^3.
	PackedFloat32Array tris;
	const int body_count = p_settings.body_triangles.size() / 3;
	const int cloth_count = p_settings.cloth_bounds.has_volume() || p_settings.cloth_bounds.has_surface() ? cloth_tris : 0;
	const int tri_count = body_count + cloth_count;
	if (tri_count > 0) {
		tris.resize(body_count * 12);
		float *t = tris.ptrw();
		AABB box = body_count > 0 ? AABB(p_settings.body_triangles[0], Vector3()) : p_settings.cloth_bounds;
		if (body_count > 0 && cloth_count > 0) {
			box.merge_with(p_settings.cloth_bounds);
		}
		for (int i = 0; i < body_count * 3; i++) {
			const Vector3 &v = p_settings.body_triangles[i];
			box.expand_to(v);
			t[i * 4 + 0] = v.x;
			t[i * 4 + 1] = v.y;
			t[i * 4 + 2] = v.z;
			t[i * 4 + 3] = 1.0f;
		}
		// The grid reaches 5 cm (or 3 cells) past the body, for strands moving in.
		float cell = MAX(p_settings.body_cell_size, 0.001f);
		float band = MAX(0.05f, 3.0f * cell);
		for (int pass = 0; pass < 2; pass++) {
			const Vector3 size = box.size + Vector3(2, 2, 2) * band;
			cell = MAX(cell, MAX(size.x, MAX(size.y, size.z)) / 62.0f);
			band = MAX(0.05f, 3.0f * cell);
		}
		const Vector3 origin = box.position - Vector3(band, band, band);
		const Vector3 size = box.size + Vector3(2, 2, 2) * band;
		f[SDF_PARAMS + 0] = origin.x;
		f[SDF_PARAMS + 1] = origin.y;
		f[SDF_PARAMS + 2] = origin.z;
		f[SDF_PARAMS + 3] = cell;
		ints[SDF_PARAMS + 4] = CLAMP((int)Math::ceil(size.x / cell), 1, 64);
		ints[SDF_PARAMS + 5] = CLAMP((int)Math::ceil(size.y / cell), 1, 64);
		ints[SDF_PARAMS + 6] = CLAMP((int)Math::ceil(size.z / cell), 1, 64);
		ints[SDF_PARAMS + 7] = tri_count;
		f[SDF_PARAMS + 8] = band;
		f[SDF_PARAMS + 9] = MAX(p_settings.margin, 0.0f);
		f[CLOTH_PARAMS + 0] = (float)cloth_count;
		f[CLOTH_PARAMS + 1] = (float)MAX(p_settings.cloth_texture_width, 1);
		f[CLOTH_PARAMS + 3] = MAX(p_settings.cloth_thickness, 0.0f);
		put_mat4(f + CLOTH_PARAMS + 4, p_settings.cloth_xform);
	}
	// The deep opacity maps, one per light.
	{
		const Vector3 layers = deep_shadow_layers();
		float *d = f + DOM_PARAMS;
		int maps = 0;
		for (const Settings::DeepShadowLight &light : p_settings.dom_lights) {
			if (maps >= HairStrandSolverGPU::DOM_SLOTS) {
				break;
			}
			if (deep_shadow_slot(light, d + 8 + maps * 16)) {
				maps++;
			}
		}
		d[0] = (float)HairStrandSolverGPU::DOM_CELLS;
		d[1] = MAX(p_settings.root_width, 0.0f);
		d[2] = MAX(p_settings.tip_width, 0.0f);
		d[3] = (float)maps;
		d[4] = layers.x;
		d[5] = layers.y;
		d[6] = layers.z;
	}
	// Strand-strand collision between guides.
	if (p_settings.collision_radius > 0.0f) {
		f[COLL_PARAMS + 0] = p_settings.collision_radius;
		f[COLL_PARAMS + 2] = CLAMP(p_settings.collision_friction, 0.0f, 1.0f);
		f[COLL_PARAMS + 3] = 1.0f;
	}
	// The hair volume (Petrovic et al. 2005).
	if (p_settings.volume_enabled && vol_cells > 0) {
		f[VOL_PARAMS + 0] = vol_origin.x;
		f[VOL_PARAMS + 1] = vol_origin.y;
		f[VOL_PARAMS + 2] = vol_origin.z;
		f[VOL_PARAMS + 3] = vol_cell;
		f[VOL_PARAMS + 4] = per_substep(MAX(p_settings.volume_friction, 0.0f));
		// Water holds wet strands together (elastocapillary coalescence, Bico et al. 2004) instead
		// of the hair springing apart to its groomed volume.
		f[VOL_PARAMS + 5] = MAX(p_settings.volume_stiffness, 0.0f) * (1.0f - wet);
		f[VOL_PARAMS + 6] = vol_mean;
		f[VOL_PARAMS + 7] = (float)vol_cells;
	}
	f[SDF_PARAMS + 10] = wet * CLAMP(p_settings.wet_clumping, 0.0f, 1.0f); // sdf_misc.z: wet clumping
	if (p_settings.card_first > 0) {
		f[SDF_PARAMS + 11] = p_settings.cards_only ? -(float)p_settings.card_first : (float)p_settings.card_first; // sdf_misc.w
	}
	last_xform = p_xform;
	has_last = true;
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_step).bind(gpu, bytes, substeps, tris));
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

RID HairStrandSolver::get_deep_shadow_texture_rd_rid() const {
	return gpu.is_valid() ? gpu->tex_dom : RID();
}

void HairStrandSolver::deep_shadow_axes(const Vector3 &p_light, Vector3 &r_u, Vector3 &r_v) {
	const Vector3 up = Math::abs(p_light.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	r_u = up.cross(p_light).normalized();
	r_v = p_light.cross(r_u);
}

bool HairStrandSolver::deep_shadow_slot(const Settings::DeepShadowLight &p_light, float r_slot[16]) {
	if (p_light.radius <= 0.0f) {
		return false;
	}
	Vector3 l;
	float distance = 0.0f;
	float tan_half = 0.0f;
	if (p_light.local) {
		// A perspective map from the light that just holds the hair's bounding sphere; a light
		// inside the sphere (or almost) cannot hold it in one view.
		const Vector3 to_light = p_light.light - p_light.center;
		distance = to_light.length();
		if (distance < p_light.radius * 1.05f) {
			return false;
		}
		l = to_light / distance;
		tan_half = p_light.radius / Math::sqrt(distance * distance - p_light.radius * p_light.radius);
	} else {
		if (p_light.light.length_squared() < 1e-6f) {
			return false;
		}
		l = p_light.light.normalized();
	}
	Vector3 u, v;
	deep_shadow_axes(l, u, v);
	const float slot[16] = {
		p_light.center.x, p_light.center.y, p_light.center.z, p_light.radius,
		u.x, u.y, u.z, p_light.local ? 1.0f : 0.0f,
		v.x, v.y, v.z, tan_half,
		l.x, l.y, l.z, distance
	};
	memcpy(r_slot, slot, sizeof(slot));
	return true;
}

RID HairStrandSolver::get_previous_position_texture_rd_rid() const {
	return gpu.is_valid() ? gpu->tex_out_prev : RID();
}

void HairStrandSolverGPU::rt_drop_history(Ref<HairStrandSolverGPU> p_self) {
	history = false;
}

void HairStrandSolver::set_cloth(RID p_texture, const PackedInt32Array &p_tris) {
	if (gpu.is_null()) {
		return;
	}
	cloth_tris = p_texture.is_valid() ? p_tris.size() / 3 : 0;
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_set_cloth).bind(gpu, p_texture, p_tris));
}

void HairStrandSolver::drop_history() {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_drop_history).bind(gpu));
}

void HairStrandSolver::hold() {
	if (gpu.is_null()) {
		return;
	}
	_dispatch(callable_mp(gpu.ptr(), &HairStrandSolverGPU::rt_hold).bind(gpu));
}

int HairStrandSolver::get_texture_width() const {
	return gpu.is_valid() ? gpu->tex_width : 0;
}

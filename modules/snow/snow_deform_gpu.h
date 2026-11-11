/**************************************************************************/
/*  snow_deform_gpu.h                                                     */
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

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"

class RenderingDevice;

// The render-thread side of SnowSurface3D's print field on the GPU (docs/ideas/snow.md, follow-up 13).
// The node builds one frame of work on the main thread (parameters, the stamps of the bodies that
// pressed, tiles moving in and out of the cache) and posts it here with step(); results come back
// asynchronously (the collision heights and a packed copy of the field) and the node takes them with
// take_*().
class SnowDeformGPU : public RefCounted {
	GDSOFTCLASS(SnowDeformGPU, RefCounted);

public:
	enum Flags {
		STEP_RESET = 1 << 0, // clear the field
		STEP_SHIFT = 1 << 1, // store leaving tiles, then move the field by whole tiles
		STEP_STAMPS = 1 << 2,
		STEP_SLUMP = 1 << 3,
		STEP_RELAX = 1 << 4,
		STEP_BLIT = 1 << 5,
		STEP_COLLISION = 1 << 6, // heights for the collision patch, read back
		STEP_READBACK = 1 << 7, // the packed field, read back
		STEP_GROUND = 1 << 8, // new ground heights
		STEP_COVER = 1 << 9, // new cover cells
		STEP_SYNC = 1 << 10, // read back now, blocking (tests)
	};
	static constexpr int PARAMS_BYTES = 192;
	static constexpr int STAMP_BYTES = 80;

	// Sizes: field texels per side, ground lattice per side, cover cells per side, cache slots, collision
	// vertices per side.
	struct Dims {
		int n = 0;
		int gn = 0;
		int cn = 0;
		int slots = 0;
		int coll_w = 0;
	};

	static bool is_available();

	SnowDeformGPU();
	~SnowDeformGPU();
	// Frees the GPU objects (on the render thread, waiting for it). Call before dropping the last reference.
	void shutdown();

	// Main thread. Posts the work to the render thread.
	void step(const Dims &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center);
	// The texture the material samples (RGBA16F: dent, rim, 1, 1), once built; invalid before.
	RID get_texture() const;
	uint64_t get_texture_serial() const { return texture_serial.get(); }
	// The latest collision heights (already divided by the spacing) and the centre they were taken at.
	bool take_collision(PackedFloat32Array &r_heights, Vector2 &r_center);
	// The latest packed field (half floats dent, rim per texel) and the origin it was taken at.
	bool take_field(PackedByteArray &r_packed, Vector2i &r_origin);

private:
	RenderingDevice *rd = nullptr;
	RID shader;
	RID pipeline;
	RID buf_params;
	RID buf_state[2];
	int cur = 0;
	RID buf_give;
	RID buf_ground;
	RID buf_cover;
	RID buf_stamps;
	int stamps_capacity = 0;
	RID buf_collision;
	RID buf_cache;
	RID buf_tile_ops;
	int tile_ops_capacity = 0;
	RID buf_packed;
	RID tex_out;
	RID uset[2]; // state = buf_state[i], out = buf_state[1 - i]
	Dims dims;
	bool compiled = false;
	bool failed = false;

	mutable Mutex mutex;
	RID texture_rid; // read by the main thread
	SafeNumeric<uint64_t> texture_serial;
	bool collision_ready = false;
	PackedFloat32Array collision_heights;
	Vector2 collision_center;
	bool field_ready = false;
	PackedByteArray field_packed;
	Vector2i field_origin;

	void _rt_compile();
	void _rt_ensure(const Dims &p_dims);
	void _rt_free_buffers();
	RID _rt_storage(int p_bytes);
	void _rt_make_sets();
	void _dispatch(int64_t p_list, int p_mode, int p_count, int p_groups);

	void rt_step(const PackedInt32Array &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center);
	void rt_on_collision(const PackedByteArray &p_data, Vector2 p_center);
	void rt_on_field(const PackedByteArray &p_data, Vector2i p_origin);
	void rt_free();
};

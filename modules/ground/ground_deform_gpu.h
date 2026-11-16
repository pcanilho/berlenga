/**************************************************************************/
/*  ground_deform_gpu.h                                                   */
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

#pragma once

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"
#include "scene/resources/texture_rd.h"

class RenderingDevice;

class GroundDeformField;

// The render-thread side of the print field on the GPU, for SnowSurface3D (docs/ideas/snow.md, follow-up 13)
// and GroundSurface3D (docs/ideas/sand.md, stage 0). The node builds one frame of work on the main thread (parameters, the stamps of the bodies that
// pressed, tiles moving in and out of the cache) and posts it here with step(); results come back
// asynchronously (the collision heights and a packed copy of the field) and the node takes them with
// take_*().
class GroundDeformGPU : public RefCounted {
	GDSOFTCLASS(GroundDeformGPU, RefCounted);

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
	static constexpr int PARAMS_BYTES = 224;
	static constexpr int STAMP_BYTES = 80;

	// Sizes: field texels per side, ground lattice per side, cover cells per side, cache slots, collision
	// vertices per side.
	struct Dims {
		int n = 0;
		int gn = 0;
		int cn = 0;
		int slots = 0;
		int coll_w = 0;
		int stride = 1; // uints per texel in the cache and the read back: 1 (dent, rim), 2 (and fresh)
	};

	static bool is_available();

	GroundDeformGPU();
	~GroundDeformGPU();
	// Frees the GPU objects (on the render thread, waiting for it). Call before dropping the last reference.
	void shutdown();

	// Main thread. Posts the work to the render thread.
	void step(const Dims &p_dims, int p_flags, const PackedByteArray &p_params, const PackedByteArray &p_ground, const PackedByteArray &p_cover, const PackedByteArray &p_stamps, const PackedByteArray &p_stores, const PackedByteArray &p_shift, Vector2i p_origin, Vector2 p_collision_center);
	// The texture the material samples (RGBA16F: dent, rim, 1, 1 for snow; dent, rim, albedo and roughness
	// factors for the ground), once built; invalid before.
	RID get_texture() const;
	uint64_t get_texture_serial() const { return texture_serial.get(); }
	// The latest collision heights (already divided by the spacing) and the centre they were taken at.
	bool take_collision(PackedFloat32Array &r_heights, Vector2 &r_center);
	// The latest packed field (half floats dent, rim per texel, then fresh and 0 with a stride of 2) and the
	// origin it was taken at.
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

// The main-thread side, shared by SnowSurface3D and GroundSurface3D: the stamps of a frame, the slots of the
// tile cache on the GPU, the tile moves when the field follows the camera, and the read back into the CPU
// copy of the field.
class GroundDeformGPUHost {
public:
	static constexpr int STAMP_FLOATS = 16;
	struct Slot {
		int slot = 0;
		uint64_t used = 0;
	};
	Ref<GroundDeformGPU> gpu;
	Ref<Texture2DRD> texture;
	uint64_t texture_serial = 0;
	PackedByteArray stamps;
	PackedByteArray stores;
	PackedByteArray shift;
	HashMap<Vector2i, Slot> cache; // world tile -> cache slot
	LocalVector<int> free_slots;
	int slots = 0;
	uint64_t cache_clock = 0;
	LocalVector<float> tile_stamped; // per field tile: time of the last stamp
	double time = 0.0;
	double slump_until = -1.0;
	int n = 0;
	bool reset = true;
	bool changed = true; // since the last read back

	// Drops the GPU objects and everything that refers to them.
	void release();
	// Forgets the cache (the field starts over).
	void clear_cache();
	// Queues a press (GroundDeformField::press's arguments) as a stamp over p_x0..p_x1, p_z0..p_z1 (field
	// texels); p_d is the stamp's last four floats (crack, flags, seed, extra).
	void add_stamp(GroundDeformField &r_field, const float p_v[12], const float p_d[4], int p_x0, int p_x1, int p_z0, int p_z1);
	// The stamp's texel bounds for a footprint reaching p_span around p_position; false when outside.
	static bool stamp_bounds(const GroundDeformField &p_field, Vector3 p_position, float p_span, int &r_x0, int &r_x1, int &r_z0, int &r_z1);
	// The field is about to move to p_origin: tiles leaving with prints go to the cache, tiles coming in are
	// read from the old field or the cache, and stamps not yet sent move with the texels.
	void shift_to(const GroundDeformField &p_field, Vector2i p_origin, int p_resolution, int p_max_cached_tiles);
	// Takes a new texture RID; true when it changed.
	bool take_texture();
	// Copies the latest read back into the field's CPU arrays (and fresh, with p_stride 2; then r_owed gets the
	// most slumping any texel still owes, seconds).
	bool take_field(GroundDeformField &r_field, int p_stride, float *r_owed = nullptr);
};

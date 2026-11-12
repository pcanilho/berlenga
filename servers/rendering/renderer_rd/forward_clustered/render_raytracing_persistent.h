/**************************************************************************/
/*  render_raytracing_persistent.h                                        */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"

namespace RendererSceneRenderImplementation {

// Persistent instance slots (rendering/pathtracing/persistent_instances).
//
// Every traced mesh instance whose TLAS entries follow from its own state alone (no deformation, capture, camera
// billboard or mesh LOD) owns a slot, by rt_index, with fixed ranges of TLAS instances and geometry rows. So does a
// MultiMesh whose surfaces all expand to one TLAS entry per record; its records' changes renew the instance's
// rt_generation (DEPENDENCY_CHANGED_MULTIMESH_DATA). The slot records the instance's rt_generation when written.
// Next frame, an instance whose generation is unchanged and whose slot is still live skips planning and writing:
// its entries stay in place and its rows stay on the GPU. Everything else (merged MultiMeshes, particles,
// procedural and deformed instances, decals) is written each frame after the persistent ranges, as before.
//
// What changes outside the instance invalidates by scope:
// - A changed material rewrites the slots whose rows use it (material_changed, from the material cache).
// - Mesh, readiness, pipeline, camera-layer, LOD, shadow-group and origin changes rewrite every slot (reset).
// - Slots not in this frame's instance list are disabled: their TLAS entries get mask 0, and RenderingDevice leaves
//   them out of the build.
struct RTPersistentInstances {
	enum SlotFlags : uint8_t {
		SLOT_STATIC = 1 << 0, // Owns ranges.
		SLOT_ENABLED = 1 << 1, // Its TLAS entries are live.
		SLOT_REWRITE = 1 << 2, // Written with motion: written again next frame, which may have none.
	};

	struct Range {
		uint32_t begin = 0;
		uint32_t capacity = 0;
		uint32_t count = 0;
	};

	struct Slot {
		Range tlas;
		Range transparent;
		Range rows;
		uint32_t instances = 0; // Render info.
		uint32_t primitives = 0;
		bool emissive = false;
	};

	// By rt_index. Hot fields apart, so the per-instance check and the sweep touch little memory.
	LocalVector<uint64_t> generations;
	LocalVector<uint32_t> stamps; // build of the last frame the slot was in the instance list.
	LocalVector<uint8_t> flags;
	LocalVector<Slot> slots;

	// By position in this frame's instance list: true when the instance kept its slot unchanged.
	LocalVector<uint8_t> clean;
	bool active = false; // This frame's view uses the slots.
	uint32_t build = 0; // Frames opened.
	uint32_t enabled = 0; // Slots with SLOT_ENABLED.
	uint32_t stamped = 0; // Enabled slots stamped this build.
	// Render info of the enabled slots, and of the slots written this frame (counted as they were written).
	uint64_t enabled_instances = 0;
	uint64_t enabled_primitives = 0;
	uint64_t written_instances = 0;
	uint64_t written_primitives = 0;

	// TLAS instances and geometry rows: [0, *_end) persistent ranges, then this frame's per-frame entries.
	LocalVector<RD::AccelerationStructureInstance> tlas;
	LocalVector<uint32_t> tlas_owner; // rt_index of each persistent entry.
	uint32_t tlas_end = 0;
	LocalVector<RD::AccelerationStructureInstance> transparent;
	LocalVector<uint32_t> transparent_owner;
	uint32_t transparent_end = 0;
	// Persistent entries written since the last TLAS build, which writes only these and the per-frame entries.
	LocalVector<uint32_t> changed_tlas;
	LocalVector<uint32_t> changed_transparent;
	bool changed_all = true;
	LocalVector<RT_GeometryData> rows;
	LocalVector<int32_t> motion; // Parallel to rows.
	LocalVector<uint32_t> material_slots; // Parallel to persistent rows.
	uint32_t rows_end = 0;
	LocalVector<uint32_t> dirty_rows; // Persistent rows written since their last upload.
	bool upload_all = true;
	uint32_t garbage = 0; // TLAS entries and rows in ranges given up.
	uint32_t active_transparent = 0; // Persistent transparent entries with a BLAS.

	HashMap<uint32_t, LocalVector<RenderRaytracing::RTEmissiveCandidate>> emissive; // By rt_index; row-relative geometry_idx.
	HashMap<RID, uint32_t> blas_refs; // Live persistent entries per BLAS.
	LocalVector<RID> lost_blas; // Referenced BLASes freed since the last build.

	// Inputs every slot depends on.
	uint64_t global_key = 0;
	LocalVector<uint32_t> caster_masks; // Sorted.
	uint64_t material_version = UINT64_MAX;
	LocalVector<uint16_t> material_counters; // By material cache slot, at the last scan.
	LocalVector<uint8_t> material_changed; // By material cache slot, this frame.
	bool any_material_changed = false;

	// Per frame while writing: where the instance's entries started in the per-frame arrays.
	struct Mark {
		uint32_t tlas = 0;
		uint32_t transparent = 0;
		uint32_t rows = 0;
		uint32_t emissive = 0;
		uint32_t instances = 0;
		uint32_t primitives = 0;
	};

	void ensure_slot(uint32_t p_rt_index) {
		if (p_rt_index < slots.size()) {
			return;
		}
		const uint32_t size = MAX(p_rt_index + 1, slots.size() + slots.size() / 2);
		const uint32_t old = slots.size();
		generations.resize(size);
		stamps.resize(size);
		flags.resize(size);
		slots.resize(size);
		for (uint32_t i = old; i < size; i++) {
			generations[i] = 0;
			stamps[i] = 0;
			flags[i] = 0;
		}
	}

	// Whether the instance at p_rt_index keeps its entries this frame; stamps the slot when it does.
	bool check_clean(uint32_t p_rt_index, uint64_t p_generation) {
		if (!can_keep(p_rt_index, p_generation) || stamps[p_rt_index] == build) {
			return false;
		}
		stamps[p_rt_index] = build;
		stamped++;
		return true;
	}

	// check_clean without the stamp, for the parallel pass: reads only.
	bool can_keep(uint32_t p_rt_index, uint64_t p_generation) const {
		if (p_rt_index >= slots.size() || (flags[p_rt_index] & (SLOT_STATIC | SLOT_ENABLED | SLOT_REWRITE)) != (SLOT_STATIC | SLOT_ENABLED) ||
				generations[p_rt_index] != p_generation) {
			return false;
		}
		if (any_material_changed) {
			const Range &rows_range = slots[p_rt_index].rows;
			for (uint32_t i = rows_range.begin; i < rows_range.begin + rows_range.count; i++) {
				const uint32_t material = material_slots[i];
				if (material < material_changed.size() && material_changed[material]) {
					return false;
				}
			}
		}
		return true;
	}

	void ref_blas(RID p_blas) {
		if (p_blas.is_valid()) {
			uint32_t *count = blas_refs.getptr(p_blas);
			if (count) {
				(*count)++;
			} else {
				blas_refs.insert(p_blas, 1);
			}
		}
	}

	void unref_blas(RID p_blas) {
		if (!p_blas.is_valid()) {
			return;
		}
		uint32_t *count = blas_refs.getptr(p_blas);
		ERR_FAIL_NULL(count);
		if (--(*count) == 0) {
			blas_refs.erase(p_blas);
		}
	}

	// Mask 0 and no BLAS: RenderingDevice leaves the entry out of the build. Live entries never have mask 0.
	void disable_entry(RD::AccelerationStructureInstance &r_entry, bool p_transparent) {
		if (r_entry.mask == 0) {
			return;
		}
		if (p_transparent) {
			active_transparent--;
			changed_transparent.push_back(uint32_t(&r_entry - transparent.ptr()));
		} else {
			changed_tlas.push_back(uint32_t(&r_entry - tlas.ptr()));
		}
		unref_blas(r_entry.blas);
		r_entry.blas = RID();
		r_entry.mask = 0;
	}

	void disable_entries(LocalVector<RD::AccelerationStructureInstance> &r_entries, const Range &p_range, bool p_transparent) {
		for (uint32_t i = p_range.begin; i < p_range.begin + p_range.count; i++) {
			disable_entry(r_entries[i], p_transparent);
		}
	}

	void disable_slot(uint32_t p_rt_index) {
		Slot &slot = slots[p_rt_index];
		disable_entries(tlas, slot.tlas, false);
		disable_entries(transparent, slot.transparent, true);
		if (flags[p_rt_index] & SLOT_ENABLED) {
			enabled--;
			enabled_instances -= slot.instances;
			enabled_primitives -= slot.primitives;
		}
		flags[p_rt_index] &= ~uint8_t(SLOT_ENABLED);
	}

	// Gives the slot's ranges up; their entries are disabled and count as garbage until the next reset.
	void free_slot(uint32_t p_rt_index) {
		if (p_rt_index >= slots.size() || !(flags[p_rt_index] & SLOT_STATIC)) {
			return;
		}
		disable_slot(p_rt_index);
		Slot &slot = slots[p_rt_index];
		garbage += slot.tlas.capacity + slot.transparent.capacity + slot.rows.capacity;
		if (slot.emissive) {
			emissive.erase(p_rt_index);
		}
		slot = Slot();
		flags[p_rt_index] = 0;
	}

	// Forgets every slot; the next frame writes every instance again into fresh ranges.
	void reset() {
		for (uint32_t i = 0; i < slots.size(); i++) {
			slots[i] = Slot();
			flags[i] = 0;
		}
		tlas.clear();
		tlas_owner.clear();
		transparent.clear();
		transparent_owner.clear();
		rows.clear();
		motion.clear();
		material_slots.clear();
		tlas_end = transparent_end = rows_end = 0;
		dirty_rows.clear();
		upload_all = true;
		changed_tlas.clear();
		changed_transparent.clear();
		changed_all = true;
		garbage = 0;
		active_transparent = 0;
		enabled = 0;
		enabled_instances = 0;
		enabled_primitives = 0;
		emissive.clear();
		blas_refs.clear();
		lost_blas.clear();
	}

	// Allocates p_count entries at the end of a persistent region, or keeps r_range when they fit.
	static bool place(Range &r_range, uint32_t p_count, uint32_t &r_end, uint32_t &r_garbage) {
		if (p_count <= r_range.capacity && r_range.capacity > 0) {
			r_range.count = p_count;
			return false;
		}
		r_garbage += r_range.capacity;
		r_range.begin = r_end;
		r_range.capacity = p_count;
		r_range.count = p_count;
		r_end += p_count;
		return true;
	}
};

} // namespace RendererSceneRenderImplementation

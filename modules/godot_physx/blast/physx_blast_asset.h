/**************************************************************************/
/*  physx_blast_asset.h                                                   */
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

#include "core/io/resource.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_server_types.h"

// Render meshes of an asset's chunks, one per chunk, shared by every PhysXDestructible3D drawing that asset: a piece
// is drawn as instances of its chunks' meshes, so a split moves instances instead of rebuilding a mesh. Each mesh
// is built the first time a piece needs it; PhysXBlastAsset.prepare_render() converts every chunk's arrays to GPU
// format ahead of time, from any thread, so the main thread then only uploads.
class PhysXBlastRenderCache : public RefCounted {
	GDSOFTCLASS(PhysXBlastRenderCache, RefCounted);

public:
	struct Surface {
		int slot = 0;
		RenderingServerTypes::SurfaceData data;
	};
	Mutex mutex;
	// prepared[c]: chunk c's surfaces in GPU format (from prepare_render), consumed when its mesh is built.
	LocalVector<LocalVector<Surface>> prepared;
	LocalVector<RID> meshes; // RID() until built
	~PhysXBlastRenderCache();
};

// A fractured Blast asset, saveable/loadable as a normal Godot Resource
// (.tres/.res) -- the real replacement for the two-file (.asset/.chunks)
// convention the standalone throwaway authoring tool and the early runtime-
// bridge MVP (GodotPhysXBlastProbe, PhysXDestructible3D's asset_path/
// chunks_path) used before this Resource type existed. Produced by
// PhysXBlastAuthoring::fracture_mesh(); consumed by PhysXDestructible3D
// (once wired up -- not yet, as of this class's introduction).
class PhysXBlastAsset : public Resource {
	GDCLASS(PhysXBlastAsset, Resource);

protected:
	static void _bind_methods();

public:
	// The raw NvBlastAsset bytes -- a single relocatable memory block per
	// NvBlast's own design, so this is genuinely just "the asset", no
	// framing of our own on top.
	void set_asset_bytes(const PackedByteArray &p_bytes) { asset_bytes = p_bytes; }
	PackedByteArray get_asset_bytes() const { return asset_bytes; }

	// chunk_points[i] is a PackedVector3Array: chunk i's render-mesh
	// triangle-soup positions (object-local space), same data/format
	// GodotPhysXBlastProbe/PhysXDestructible3D already parse from the old
	// .chunks text format -- here it's just real Resource-serialized data,
	// no text parsing needed.
	void set_chunk_points(const Array &p_points) {
		chunk_points = p_points;
		render_cache.unref();
	}
	Array get_chunk_points() const { return chunk_points; }

	int get_chunk_count() const { return chunk_points.size(); }

	// Optional render meshes, one per chunk (entry i: a Mesh arrays Array, indexed or not, with any of normals,
	// tangents, colours, UV and UV2), for chunks authored outside Blast with their own shading data (a procedural
	// rock's pieces). Empty: pieces are drawn from chunk_points with flat normals, as before.
	// An entry may also be a Mesh, or an Array of surfaces (each a Mesh arrays Array, or a Dictionary with "arrays"
	// and "slot"), for chunks with several materials (bark, wood, moss, leaves): surface s draws with materials[slot],
	// its slot being s unless given (a Mesh surface whose material is in materials takes that slot).
	void set_chunk_meshes(const Array &p_meshes) {
		chunk_meshes = p_meshes;
		render_cache.unref();
	}
	Array get_chunk_meshes() const { return chunk_meshes; }

	// Materials by surface slot for chunk_meshes; a PhysXDestructible3D's material_override still wins over them.
	void set_materials(const Array &p_materials) {
		materials = p_materials;
		render_cache.unref();
	}
	Array get_materials() const { return materials; }

	// Optional initial health of each bond, in the asset's bond order, as a multiple of the destructible's health
	// (a twig joint 0.01, a trunk joint 50). Empty: every bond starts at the destructible's health.
	void set_bond_healths(const PackedFloat32Array &p_healths) { bond_healths = p_healths; }
	PackedFloat32Array get_bond_healths() const { return bond_healths; }

	// Optional collision hulls, one PackedVector3Array per chunk (its convex hull's points, few of them), so a
	// destructible cooks small hulls instead of every render triangle's corners. Empty: hulls from chunk_points.
	void set_chunk_hulls(const Array &p_hulls) { chunk_hulls = p_hulls; }
	Array get_chunk_hulls() const { return chunk_hulls; }

	// Converts every chunk's render arrays to GPU format now (the costly part of building them), so a
	// destructible spawning from this asset later uploads them without stalling. Safe to call from a worker
	// thread (WorkerThreadPool) while nothing edits the asset; calling it again does nothing.
	void prepare_render();
	// The shared per-chunk render meshes (see PhysXBlastRenderCache), made on first use.
	Ref<PhysXBlastRenderCache> get_render_cache();

private:
	Ref<PhysXBlastRenderCache> render_cache;
	Mutex render_cache_mutex;
	PackedByteArray asset_bytes;
	Array chunk_points;
	Array chunk_meshes;
	Array chunk_hulls;
	Array materials;
	PackedFloat32Array bond_healths;
};

/**************************************************************************/
/*  physx_blast_asset.cpp                                                 */
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

#include "physx_blast_asset.h"

#include "physx_destructible_3d.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

PhysXBlastRenderCache::~PhysXBlastRenderCache() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	for (const RID &mesh : meshes) {
		if (mesh.is_valid()) {
			rs->free_rid(mesh);
		}
	}
}

Ref<PhysXBlastRenderCache> PhysXBlastAsset::get_render_cache() {
	MutexLock lock(render_cache_mutex);
	if (render_cache.is_null()) {
		render_cache.instantiate();
	}
	return render_cache;
}

void PhysXBlastAsset::prepare_render() {
	Ref<PhysXBlastRenderCache> cache = get_render_cache();
	{
		MutexLock lock(cache->mutex);
		if (!cache->prepared.is_empty()) {
			return;
		}
	}
	LocalVector<PackedVector3Array> points;
	points.resize(chunk_points.size());
	for (int i = 0; i < chunk_points.size(); i++) {
		points[i] = chunk_points[i];
	}
	LocalVector<LocalVector<PhysXDestructible3D::ChunkSurface>> surfaces;
	PhysXDestructible3D::build_chunk_surfaces(points, chunk_meshes, materials, surfaces);
	LocalVector<LocalVector<PhysXBlastRenderCache::Surface>> prepared;
	prepared.resize(surfaces.size());
	RenderingServer *rs = RenderingServer::get_singleton();
	for (uint32_t c = 0; c < surfaces.size(); c++) {
		for (const PhysXDestructible3D::ChunkSurface &cs : surfaces[c]) {
			PhysXBlastRenderCache::Surface s;
			s.slot = cs.slot;
			if (cs.vertices.is_empty() || rs->mesh_create_surface_data_from_arrays(&s.data, RSE::PRIMITIVE_TRIANGLES, PhysXDestructible3D::chunk_surface_arrays(cs)) != OK) {
				continue;
			}
			prepared[c].push_back(s);
		}
	}
	MutexLock lock(cache->mutex);
	if (cache->prepared.is_empty()) {
		cache->prepared = prepared;
	}
}

void PhysXBlastAsset::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_asset_bytes", "bytes"), &PhysXBlastAsset::set_asset_bytes);
	ClassDB::bind_method(D_METHOD("get_asset_bytes"), &PhysXBlastAsset::get_asset_bytes);
	ClassDB::bind_method(D_METHOD("set_chunk_points", "points"), &PhysXBlastAsset::set_chunk_points);
	ClassDB::bind_method(D_METHOD("get_chunk_points"), &PhysXBlastAsset::get_chunk_points);
	ClassDB::bind_method(D_METHOD("get_chunk_count"), &PhysXBlastAsset::get_chunk_count);
	ClassDB::bind_method(D_METHOD("set_chunk_meshes", "meshes"), &PhysXBlastAsset::set_chunk_meshes);
	ClassDB::bind_method(D_METHOD("get_chunk_meshes"), &PhysXBlastAsset::get_chunk_meshes);
	ClassDB::bind_method(D_METHOD("set_chunk_hulls", "hulls"), &PhysXBlastAsset::set_chunk_hulls);
	ClassDB::bind_method(D_METHOD("get_chunk_hulls"), &PhysXBlastAsset::get_chunk_hulls);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &PhysXBlastAsset::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &PhysXBlastAsset::get_materials);
	ClassDB::bind_method(D_METHOD("set_bond_healths", "healths"), &PhysXBlastAsset::set_bond_healths);
	ClassDB::bind_method(D_METHOD("get_bond_healths"), &PhysXBlastAsset::get_bond_healths);
	ClassDB::bind_method(D_METHOD("prepare_render"), &PhysXBlastAsset::prepare_render);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "asset_bytes"), "set_asset_bytes", "get_asset_bytes");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "chunk_points"), "set_chunk_points", "get_chunk_points");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "chunk_meshes"), "set_chunk_meshes", "get_chunk_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "chunk_hulls"), "set_chunk_hulls", "get_chunk_hulls");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, "Material"), "set_materials", "get_materials");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bond_healths"), "set_bond_healths", "get_bond_healths");
}

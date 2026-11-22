/**************************************************************************/
/*  ground_height_query.cpp                                               */
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

#include "ground_height_query.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_deformer_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

GroundHeightQuery GroundHeightQuery::make(const Node3D *p_from, const NodePath &p_source, uint32_t p_mask) {
	GroundHeightQuery q;
	const bool inside = p_from && p_from->is_inside_tree();
	Node *src = p_source.is_empty() || !inside ? nullptr : p_from->get_node_or_null(p_source);
	q.deformer = Object::cast_to<TerrainDeformer3D>(src);
	q.data_map = Object::cast_to<TerrainDataMap3D>(src);
	if (q.data_map && !q.data_map->is_baked()) {
		q.data_map = nullptr;
	}
	q.generator = Object::cast_to<TerrainGenerator3D>(src);
	if (src && !q.deformer && !q.generator && !Object::cast_to<TerrainDataMap3D>(src)) {
		if (src->has_method("get_height")) {
			q.getter = src;
		} else if (src->has_method("get_data")) {
			// Terrain3D, by name: Terrain3D.get_data().get_height(position).
			Object *terrain_data = src->call("get_data");
			if (terrain_data && terrain_data->has_method("get_height")) {
				q.getter = terrain_data;
			}
		}
	}
	if (inside && p_from->get_world_3d().is_valid()) {
		q.space = p_from->get_world_3d()->get_direct_space_state();
	}
	q.mask = p_mask;
	q.y0 = inside ? p_from->get_global_position().y : 0.0f;
	return q;
}

float GroundHeightQuery::height(Vector3 p_position) const {
	const Vector3 p(p_position.x, y0, p_position.z);
	float h = Math::NaN;
	if (deformer) {
		h = deformer->get_height(p);
	} else if (data_map && data_map->get_map_rect().has_point(Vector2(p.x, p.z))) {
		h = data_map->get_height_at(p);
	} else if (generator) {
		h = generator->get_height(p);
	} else if (getter) {
		h = getter->call("get_height", p);
	}
	if (!Math::is_finite(h) && space) {
		PhysicsDirectSpaceState3D::RayParameters rp;
		rp.from = Vector3(p.x, y0 + 10000.0f, p.z);
		rp.to = Vector3(p.x, y0 - 10000.0f, p.z);
		rp.collision_mask = mask;
		PhysicsDirectSpaceState3D::RayResult rr;
		if (space->intersect_ray(rp, rr)) {
			h = rr.position.y;
		}
	}
	return h;
}

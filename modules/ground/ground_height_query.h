/**************************************************************************/
/*  ground_height_query.h                                                 */
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

#include "core/math/vector3.h"
#include "core/string/node_path.h"

class Node;
class Node3D;
class Object;
class PhysicsDirectSpaceState3D;
class TerrainDataMap3D;
class TerrainDeformer3D;
class TerrainGenerator3D;

// The ground under a point, from whatever a scene uses for terrain (docs/ideas/ground-deform.md):
// a TerrainDeformer3D, a baked TerrainDataMap3D, a TerrainGenerator3D, anything with
// get_height(position), Terrain3D by name (get_data().get_height()), or else a downward ray on
// collision_mask. Built once per batch of samples; cheap to copy.
struct GroundHeightQuery {
	TerrainDeformer3D *deformer = nullptr;
	TerrainDataMap3D *data_map = nullptr;
	TerrainGenerator3D *generator = nullptr;
	Object *getter = nullptr;
	PhysicsDirectSpaceState3D *space = nullptr;
	uint32_t mask = 1;
	float y0 = 0.0f;

	bool has_source() const { return deformer || data_map || generator || getter; }
	// The ground height, NaN where nothing answers.
	float height(Vector3 p_position) const;
	// p_from: the node the path is relative to and whose world gives the ray space and y0.
	static GroundHeightQuery make(const Node3D *p_from, const NodePath &p_source, uint32_t p_mask);
};

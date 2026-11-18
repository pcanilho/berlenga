/**************************************************************************/
/*  ground_lump_draw.h                                                    */
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

#include "core/templates/local_vector.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class Node;

// Draws lumps lying near the camera (SnowClumps3D, GroundClods3D; docs/ideas/ground-detail.md) as one MultiMesh per
// shared shape, path traced as instances (RenderingServer.multimesh_set_rt_instanced). Each MultiMesh keeps room for
// more than it shows (visible_instance_count) and grows by doubling, so a lump more or less re-uploads its buffer
// instead of reallocating the MultiMesh and its ray tracing entries.
class GroundLumpDraw {
public:
	static constexpr int MAX_SHAPES = 8;

private:
	int shapes = 0;
	bool colors = false;
	Ref<MultiMesh> multimeshes[MAX_SHAPES];
	MultiMeshInstance3D *instances[MAX_SHAPES] = {};
	int capacity[MAX_SHAPES] = {};

public:
	// One float run per shape: 12 floats a lump (a 3x4 transform, rows), 16 with a colour after it.
	LocalVector<float> buffers[MAX_SHAPES];

	bool is_ready() const { return shapes > 0 && instances[0] != nullptr; }
	void ensure(Node *p_owner, const Ref<ArrayMesh> *p_meshes, int p_shapes, bool p_colors, const Ref<Material> &p_material);
	void set_material(const Ref<Material> &p_material);
	void release();
	// Sends buffers to the MultiMeshes and empties them for the next draw.
	void commit();
};

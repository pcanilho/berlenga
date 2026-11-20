/**************************************************************************/
/*  ground_lump_draw.cpp                                                  */
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


#include "ground_lump_draw.h"

#include "scene/3d/multimesh_instance_3d.h"
#include "servers/rendering/rendering_server.h"

void GroundLumpDraw::ensure(Node *p_owner, const Ref<ArrayMesh> *p_meshes, int p_shapes, bool p_colors, const Ref<Material> &p_material) {
	shapes = MIN(p_shapes, MAX_SHAPES);
	colors = p_colors;
	for (int i = 0; i < shapes; i++) {
		if (multimeshes[i].is_null()) {
			multimeshes[i].instantiate();
			multimeshes[i]->set_transform_format(MultiMesh::TRANSFORM_3D);
			multimeshes[i]->set_use_colors(colors);
			multimeshes[i]->set_mesh(p_meshes[i]);
			capacity[i] = 0;
		}
		if (!instances[i]) {
			instances[i] = memnew(MultiMeshInstance3D);
			instances[i]->set_multimesh(multimeshes[i]);
			instances[i]->set_material_override(p_material);
			instances[i]->set_as_top_level(true);
			instances[i]->set_visible(false);
			p_owner->add_child(instances[i], false, Node::INTERNAL_MODE_FRONT);
			instances[i]->set_global_transform(Transform3D());
		}
	}
}

void GroundLumpDraw::set_material(const Ref<Material> &p_material) {
	for (int i = 0; i < shapes; i++) {
		if (instances[i]) {
			instances[i]->set_material_override(p_material);
		}
	}
}

void GroundLumpDraw::release() {
	for (int i = 0; i < MAX_SHAPES; i++) {
		if (instances[i]) {
			instances[i]->queue_free();
			instances[i] = nullptr;
		}
		multimeshes[i].unref();
		capacity[i] = 0;
		buffers[i].clear();
	}
}

void GroundLumpDraw::commit() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const int stride = colors ? 16 : 12;
	for (int i = 0; i < shapes; i++) {
		if (!instances[i]) {
			continue;
		}
		LocalVector<float> &b = buffers[i];
		const int count = int(b.size()) / stride;
		Ref<MultiMesh> &mm = multimeshes[i];
		// Room by doubling, given back when four times too big.
		if (count > capacity[i] || (capacity[i] > 256 && count < capacity[i] / 4)) {
			int cap = 256;
			while (cap < count) {
				cap *= 2;
			}
			capacity[i] = cap;
			mm->set_instance_count(cap);
			// One TLAS entry per lump over its shape for the path tracer, not a merged copy of every lump.
			rs->multimesh_set_rt_instanced(mm->get_rid(), true);
			// Lumps off screen that look smaller than half a degree leave the path tracer's set (their shadows are below a pixel).
			rs->multimesh_set_rt_offscreen_cull(mm->get_rid(), 0.5f);
		}
		instances[i]->set_visible(count > 0);
		if (count > 0) {
			// The spare room repeats the first lump at zero size, so it keeps the bounds tight and draws nothing.
			PackedFloat32Array buf;
			buf.resize(capacity[i] * stride);
			float *w = buf.ptrw();
			memcpy(w, b.ptr(), b.size() * sizeof(float));
			for (int k = count; k < capacity[i]; k++) {
				float *o = w + k * stride;
				memset(o, 0, stride * sizeof(float));
				o[3] = b[3];
				o[7] = b[7];
				o[11] = b[11];
			}
			rs->multimesh_set_buffer(mm->get_rid(), buf);
		}
		mm->set_visible_instance_count(count);
		b.clear();
	}
}

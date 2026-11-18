/**************************************************************************/
/*  vegetation_detail.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

// Ground detail rules shared by the vegetation (docs/ideas/ground-detail-vegetation.md), the same
// as RockField3D's ground detail (docs/ideas/ground-detail.md): small items are real instanced
// geometry out to where they are still a few pixels tall, and no further.
namespace VegetationDetail {

// The vertical angle one pixel of p_node's viewport subtends through its current camera (radians).
// p_reference_height > 0 stands in for the viewport's height (tests, or a fixed target such as 1080p).
// Outside the tree: 75 degrees over 1080 rows.
inline real_t pixel_angle(const Node *p_node, int p_reference_height = 0) {
	real_t fov = Math::deg_to_rad(real_t(75.0));
	real_t height = 1080.0;
	if (p_node && p_node->is_inside_tree()) {
		Viewport *vp = p_node->get_viewport();
		if (vp) {
			if (Camera3D *cam = vp->get_camera_3d()) {
				fov = Math::deg_to_rad(cam->get_fov());
				if (cam->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH) {
					const Size2 sz = vp->get_visible_rect().size;
					fov = 2.0 * Math::atan(Math::tan(fov * 0.5) * sz.y / MAX(sz.x, real_t(1.0)));
				}
			}
			height = vp->get_visible_rect().size.y;
		}
	}
	if (p_reference_height > 0) {
		height = p_reference_height;
	}
	return fov / MAX(height, real_t(1.0));
}

// How far an item p_size metres tall or long stays p_pixels tall: d = s / (pixels x the angle of one pixel).
inline real_t reach(real_t p_size, real_t p_pixels, real_t p_pixel_angle) {
	return p_size / MAX(p_pixels * p_pixel_angle, real_t(1e-6));
}

// Path tracing: one TLAS entry per item over the shared shape instead of a merged copy of every
// item's triangles. A material whose vertex shader moves the item is captured and merged anyway
// (the path tracer must see the moved shape), so this only changes static items.
inline void trace_instanced(const Ref<MultiMesh> &p_multimesh) {
	if (p_multimesh.is_valid()) {
		RS::get_singleton()->multimesh_set_rt_instanced(p_multimesh->get_rid(), true);
	}
}

} // namespace VegetationDetail

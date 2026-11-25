/**************************************************************************/
/*  terrain_sculpt_layer_3d.h                                             */
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

#include "terrain_brush_3d.h"

#include "scene/resources/texture.h"

// A freehand sculpt layer (docs/ideas/editor-tools.md, E; Unreal Landscape edit layers): a painted map of height
// offsets in metres over a world rect, stacked on the generated ground. Under a TerrainGenerator3D it is a touch-up:
// the generator adds it to its grid (so lakes and rivers re-solve round the sculpted ground) and adds the detail finer
// than its cells when it draws the ground, so strokes keep their shape down to the map's texels. Paint it in the 3D
// view with Paint Mask (slot Height Map; Add raises, Subtract lowers, Replace flattens to Value, Smooth smooths).
// Hiding or disabling the layer restores the ground under it. Under a TerrainDeformer3D it is an offset brush.
class TerrainSculptLayer3D : public TerrainBrush3D {
	GDCLASS(TerrainSculptLayer3D, TerrainBrush3D);

	Ref<Texture2D> height_map;
	Rect2 height_map_rect;
	real_t height_scale = 1.0;
	Ref<Image> image; // height_map's image as RF, read on the main thread when the map is set

	void _read();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_height_map(const Ref<Texture2D> &p_map);
	Ref<Texture2D> get_height_map() const { return height_map; }
	void set_height_map_rect(const Rect2 &p_rect);
	Rect2 get_height_map_rect() const { return height_map_rect; }
	void set_height_scale(real_t p_scale);
	real_t get_height_scale() const { return height_scale; }
	void notify_height_map_changed(const Rect2 &p_area);

	// The offset in metres at a world point (0 outside the map). Any thread, between changes.
	float offset_at(real_t p_x, real_t p_z) const;
	Ref<Image> get_offset_image() const { return image; }

	virtual Rect2 get_world_rect() const override;
	virtual void sample_world(const Transform3D &p_xf, const Transform3D &p_inv, real_t p_x, real_t p_z, float &r_height, float &r_mask) const override;

	TerrainSculptLayer3D();
};

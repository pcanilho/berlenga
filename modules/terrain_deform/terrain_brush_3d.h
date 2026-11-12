/**************************************************************************/
/*  terrain_brush_3d.h                                                    */
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

#include "scene/3d/node_3d.h"

// A simple height brush for TerrainDeformer3D: a circle or rectangle that lowers, raises, flattens or offsets the
// ground to this node's height, with a soft edge. Craters, building plots, road beds, quarry floors.
class TerrainBrush3D : public Node3D {
	GDCLASS(TerrainBrush3D, Node3D);

public:
	enum Shape {
		SHAPE_CIRCLE,
		SHAPE_RECTANGLE,
	};
	enum Mode {
		MODE_LOWER,
		MODE_RAISE,
		MODE_FLATTEN,
		MODE_ADD,
	};
	enum Profile {
		PROFILE_FLAT,
		PROFILE_BOWL,
	};

private:
	Shape shape = SHAPE_CIRCLE;
	Mode mode = MODE_FLATTEN;
	Profile profile = PROFILE_FLAT;
	Vector2 size = Vector2(10, 10);
	real_t depth = 0.0;
	real_t falloff = 2.0;
	real_t cell_size = 0.5;
	bool enabled = true;

	void _changed();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_shape(Shape p_shape);
	Shape get_shape() const { return shape; }
	void set_mode(Mode p_mode);
	Mode get_mode() const { return mode; }
	void set_profile(Profile p_profile);
	Profile get_profile() const { return profile; }
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_depth(real_t p_depth);
	real_t get_depth() const { return depth; }
	void set_falloff(real_t p_falloff);
	real_t get_falloff() const { return falloff; }
	void set_cell_size(real_t p_cell_size);
	real_t get_cell_size() const { return cell_size; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }

	Dictionary get_carve_brush() const;

	TerrainBrush3D();
};

VARIANT_ENUM_CAST(TerrainBrush3D::Shape);
VARIANT_ENUM_CAST(TerrainBrush3D::Mode);
VARIANT_ENUM_CAST(TerrainBrush3D::Profile);

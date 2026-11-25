/**************************************************************************/
/*  world_exclusion_3d.h                                                  */
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
#include "scene/3d/path_3d.h"

// One keep-out area as plain data, copied for worker threads (docs/ideas/editor-tools.md, C).
struct WorldExclusionShape {
	enum Type {
		TYPE_BOX,
		TYPE_AREA, // a closed curve's polygon
		TYPE_TRACK, // an open curve's band of width
	};
	uint32_t kinds = 0;
	Type type = TYPE_BOX;
	Transform2D to_local; // world (x, z) to the box's local frame (box)
	Vector2 half; // box half size, local
	Vector<Vector2> points; // polygon or polyline, world (x, z)
	real_t half_width = 0.0;
	real_t fade = 0.0;
	Rect2 bounds; // world (x, z), fade included

	// 1 inside (past the fade), 0 outside.
	float keep_out(const Vector2 &p_xz) const;
};

// The keep-out areas of a world, as one list for worker threads.
struct WorldExclusions {
	LocalVector<WorldExclusionShape> shapes;
	bool is_empty() const { return shapes.is_empty(); }
	bool any_in(const Rect2 &p_rect) const;
	// The strongest keep-out at p_xz (0 none, 1 fully kept out).
	float keep_out(const Vector2 &p_xz) const;
	// The current areas of p_kind (WorldExclusion3D::Kind bits) from the registry. Main thread.
	void read(uint32_t p_kind);
};

// A keep-out area for the procedural systems (after Unreal PCG Biome Core's exclusion volumes and splines): plants,
// grass and stones are not placed inside it. The curve decides the shape: a closed curve of three or more points is
// an area, an open one a track of [member width]; with fewer than two points it is a box of [member size] round the
// node (its rotation about Y and scale apply). Edges fade over [member fade]. Systems read the areas through
// WorldContracts and place again only the ground an area covered before or after a change.
class WorldExclusion3D : public Path3D {
	GDCLASS(WorldExclusion3D, Path3D);

public:
	enum Kind {
		KIND_PLANTS = 1,
		KIND_GRASS = 2,
		KIND_ROCKS = 4,
	};

private:
	uint32_t kinds = KIND_PLANTS | KIND_GRASS | KIND_ROCKS;
	bool closed = true;
	Vector2 size = Vector2(16, 16);
	real_t width = 4.0;
	real_t fade = 1.0;
	Rect2 published; // the bounds last announced
	bool change_queued = false;

	static LocalVector<WorldExclusion3D *> registry;

	void _changed();
	void _announce();
	void _curve_changed();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static void read_all(uint32_t p_kind, LocalVector<WorldExclusionShape> &r_shapes);

	WorldExclusionShape get_shape() const;
	float get_keep_out_at(const Vector3 &p_position) const;

	void set_kinds(uint32_t p_kinds);
	uint32_t get_kinds() const { return kinds; }
	void set_closed(bool p_closed);
	bool is_closed() const { return closed; }
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_width(real_t p_width);
	real_t get_width() const { return width; }
	void set_fade(real_t p_fade);
	real_t get_fade() const { return fade; }

	PackedStringArray get_configuration_warnings() const override;

	WorldExclusion3D();
};

VARIANT_ENUM_CAST(WorldExclusion3D::Kind);

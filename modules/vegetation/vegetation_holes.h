/**************************************************************************/
/*  vegetation_holes.h                                                    */
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

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"

#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"

// Openings in the ground (TerrainGenerator3D.add_hole(): a cave mouth's passage), where the terrain
// mesh is cut away: no plant or blade stands there. A copy of the generator's boxes, so the worker
// threads that place plants read it without touching the generator. The generator is the data
// map's geology source, else its height source.
struct VegetationHoles {
	struct Box {
		Rect2 rect;
		Transform3D inv; // World to the hole's unit cube [-1, 1]^3.
	};
	LocalVector<Box> boxes;

	static TerrainGenerator3D *source_of(const TerrainDataMap3D *p_map) {
		if (!p_map || !p_map->is_inside_tree()) {
			return nullptr;
		}
		TerrainGenerator3D *gen = p_map->get_geology_source().is_empty() ? nullptr : Object::cast_to<TerrainGenerator3D>(p_map->get_node_or_null(p_map->get_geology_source()));
		if (!gen && !p_map->get_height_source().is_empty()) {
			gen = Object::cast_to<TerrainGenerator3D>(p_map->get_node_or_null(p_map->get_height_source()));
		}
		return gen;
	}

	void read(const TerrainGenerator3D *p_gen) {
		boxes.clear();
		if (!p_gen) {
			return;
		}
		const TypedArray<Transform3D> holes = p_gen->get_holes();
		for (int i = 0; i < holes.size(); i++) {
			const Transform3D box = holes[i];
			Box b;
			b.inv = box.affine_inverse();
			Vector2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
			for (int k = 0; k < 8; k++) {
				const Vector3 c = box.xform(Vector3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1));
				lo = lo.min(Vector2(c.x, c.z));
				hi = hi.max(Vector2(c.x, c.z));
			}
			b.rect = Rect2(lo, hi - lo);
			boxes.push_back(b);
		}
	}

	bool is_empty() const { return boxes.is_empty(); }

	bool any_in(const Rect2 &p_rect) const {
		for (const Box &b : boxes) {
			if (b.rect.intersects(p_rect)) {
				return true;
			}
		}
		return false;
	}

	// Whether a point on the ground (its y the ground's height there) is open, or within p_margin of
	// an opening (four points round it: a stem or a log's end at the edge of the cut).
	bool at(const Vector3 &p_ground, float p_margin = 0.0f) const {
		for (const Box &b : boxes) {
			if (!b.rect.grow(p_margin).has_point(Vector2(p_ground.x, p_ground.z))) {
				continue;
			}
			if (_inside(b, p_ground)) {
				return true;
			}
			if (p_margin > 0.0f) {
				const Vector3 o[4] = { Vector3(p_margin, 0, 0), Vector3(-p_margin, 0, 0), Vector3(0, 0, p_margin), Vector3(0, 0, -p_margin) };
				for (const Vector3 &d : o) {
					if (_inside(b, p_ground + d)) {
						return true;
					}
				}
			}
		}
		return false;
	}

private:
	static bool _inside(const Box &p_b, const Vector3 &p_p) {
		const Vector3 l = p_b.inv.xform(p_p);
		return Math::abs(l.x) <= 1.0f && Math::abs(l.y) <= 1.0f && Math::abs(l.z) <= 1.0f;
	}
};

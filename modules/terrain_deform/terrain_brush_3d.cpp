/**************************************************************************/
/*  terrain_brush_3d.cpp                                                  */
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

#include "terrain_brush_3d.h"

#include "core/object/class_db.h"

void TerrainBrush3D::_changed() {
	if (is_inside_tree()) {
		emit_signal(SNAME("carve_changed"));
	}
}

Dictionary TerrainBrush3D::get_carve_brush() const {
	Dictionary d;
	if (!enabled || !is_inside_tree() || size.x <= 0.0 || size.y <= 0.0) {
		return d;
	}
	const Transform3D xf = get_global_transform();
	const Transform3D inv = xf.affine_inverse();
	const Vector2 half = size * 0.5;
	const real_t fall = MAX(falloff, real_t(0.0));

	// World XZ bounds of the shape plus its falloff, from the rotated local corners.
	Rect2 rect;
	const Vector2 ext = half + Vector2(fall, fall);
	for (int i = 0; i < 4; i++) {
		const Vector3 corner = xf.xform(Vector3((i & 1) ? ext.x : -ext.x, 0.0, (i & 2) ? ext.y : -ext.y));
		const Vector2 c(corner.x, corner.z);
		if (i == 0) {
			rect = Rect2(c, Vector2());
		} else {
			rect.expand_to(c);
		}
	}
	// Cell-centred samples over a rect of n * cs, the convention TerrainDeformer3D and PhysXRiver3D share.
	const real_t cs = MAX(cell_size, real_t(0.01));
	const Vector2i n(MAX(2, int(Math::ceil(rect.size.x / cs))), MAX(2, int(Math::ceil(rect.size.y / cs))));
	rect.size = Vector2(n.x * cs, n.y * cs);

	PackedFloat32Array heights;
	PackedFloat32Array mask;
	heights.resize(n.x * n.y);
	mask.resize(n.x * n.y);
	float *hp = heights.ptrw();
	float *mp = mask.ptrw();
	const real_t level = xf.origin.y;
	for (int z = 0; z < n.y; z++) {
		for (int x = 0; x < n.x; x++) {
			const Vector3 local = inv.xform(Vector3(rect.position.x + (x + 0.5) * cs, level, rect.position.y + (z + 0.5) * cs));
			// r: 0 at the centre, 1 at the shape's edge. outside: distance beyond the edge, in local units.
			real_t r, outside;
			if (shape == SHAPE_CIRCLE) {
				const Vector2 q(local.x / half.x, local.z / half.y);
				r = q.length();
				outside = MAX(real_t(0.0), r - 1.0) * MIN(half.x, half.y);
			} else {
				r = MAX(Math::abs(local.x) / half.x, Math::abs(local.z) / half.y);
				const Vector2 dq(MAX(real_t(0.0), Math::abs(local.x) - half.x), MAX(real_t(0.0), Math::abs(local.z) - half.y));
				outside = dq.length();
			}
			// Soft edge: smoothstep over the falloff band (Unreal Landmass brushes use the same shape).
			real_t m = 1.0;
			if (outside > 0.0) {
				m = fall > 0.0 ? 1.0 - Math::smoothstep(real_t(0.0), fall, outside) : 0.0;
			}
			const real_t shape_depth = profile == PROFILE_BOWL ? depth * MAX(real_t(0.0), real_t(1.0) - MIN(r, real_t(1.0)) * MIN(r, real_t(1.0))) : depth;
			hp[z * n.x + x] = float(mode == MODE_ADD ? -shape_depth : level - shape_depth);
			mp[z * n.x + x] = float(m);
		}
	}
	static const char *mode_names[] = { "lower", "raise", "flatten", "add" };
	d["rect"] = rect;
	d["cell_size"] = cs;
	d["size"] = n;
	d["heights"] = heights;
	d["mask"] = mask;
	d["mode"] = mode_names[mode];
	return d;
}

void TerrainBrush3D::set_shape(Shape p_shape) {
	shape = p_shape;
	_changed();
}

void TerrainBrush3D::set_mode(Mode p_mode) {
	mode = p_mode;
	_changed();
}

void TerrainBrush3D::set_profile(Profile p_profile) {
	profile = p_profile;
	_changed();
}

void TerrainBrush3D::set_size(const Vector2 &p_size) {
	size = p_size.max(Vector2(0.01, 0.01));
	_changed();
	update_gizmos();
}

void TerrainBrush3D::set_depth(real_t p_depth) {
	depth = p_depth;
	_changed();
}

void TerrainBrush3D::set_falloff(real_t p_falloff) {
	falloff = MAX(p_falloff, real_t(0.0));
	_changed();
}

void TerrainBrush3D::set_cell_size(real_t p_cell_size) {
	cell_size = MAX(p_cell_size, real_t(0.01));
	_changed();
}

void TerrainBrush3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	_changed();
}

void TerrainBrush3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_TRANSFORM_CHANGED:
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_changed();
		} break;
	}
}

void TerrainBrush3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &TerrainBrush3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &TerrainBrush3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &TerrainBrush3D::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &TerrainBrush3D::get_mode);
	ClassDB::bind_method(D_METHOD("set_profile", "profile"), &TerrainBrush3D::set_profile);
	ClassDB::bind_method(D_METHOD("get_profile"), &TerrainBrush3D::get_profile);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &TerrainBrush3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &TerrainBrush3D::get_size);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &TerrainBrush3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &TerrainBrush3D::get_depth);
	ClassDB::bind_method(D_METHOD("set_falloff", "falloff"), &TerrainBrush3D::set_falloff);
	ClassDB::bind_method(D_METHOD("get_falloff"), &TerrainBrush3D::get_falloff);
	ClassDB::bind_method(D_METHOD("set_cell_size", "cell_size"), &TerrainBrush3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &TerrainBrush3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &TerrainBrush3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &TerrainBrush3D::is_enabled);
	ClassDB::bind_method(D_METHOD("get_carve_brush"), &TerrainBrush3D::get_carve_brush);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, "Circle,Rectangle"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Lower,Raise,Flatten,Add"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "profile", PROPERTY_HINT_ENUM, "Flat,Bowl"), "set_profile", "get_profile");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "-100,100,0.01,or_less,or_greater,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "falloff", PROPERTY_HINT_RANGE, "0,50,0.01,or_greater,suffix:m"), "set_falloff", "get_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.05,8,0.01,or_greater,suffix:m"), "set_cell_size", "get_cell_size");

	ADD_SIGNAL(MethodInfo("carve_changed"));

	BIND_ENUM_CONSTANT(SHAPE_CIRCLE);
	BIND_ENUM_CONSTANT(SHAPE_RECTANGLE);
	BIND_ENUM_CONSTANT(MODE_LOWER);
	BIND_ENUM_CONSTANT(MODE_RAISE);
	BIND_ENUM_CONSTANT(MODE_FLATTEN);
	BIND_ENUM_CONSTANT(MODE_ADD);
	BIND_ENUM_CONSTANT(PROFILE_FLAT);
	BIND_ENUM_CONSTANT(PROFILE_BOWL);
}

TerrainBrush3D::TerrainBrush3D() {
	set_notify_transform(true);
}

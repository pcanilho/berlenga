/**************************************************************************/
/*  world_burn_scar_3d.cpp                                                */
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


#include "world_burn_scar_3d.h"

#include "core/math/geometry_2d.h"
#include "core/object/class_db.h"

void WorldBurnScar3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			WorldContracts::register_fire(this);
			set_notify_transform(true);
			reported = Rect2();
			_report();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_report();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			WorldContracts::unregister_fire(this);
			if (reported.has_area()) {
				WorldContracts::fire_burn_changed(reported);
			}
			reported = Rect2();
		} break;
	}
}

// Tell the burn readers (regrowth) where the ages changed: the area it covered and the area it covers now.
void WorldBurnScar3D::_report() {
	if (!is_inside_tree()) {
		return;
	}
	const Rect2 area = get_area();
	WorldContracts::fire_burn_changed(reported.has_area() ? reported.merge(area) : area);
	reported = area;
}

Rect2 WorldBurnScar3D::get_area() const {
	const Transform3D t = is_inside_tree() ? get_global_transform() : get_transform();
	PackedVector2Array pts = polygon;
	if (pts.size() < 3) {
		pts = { Vector2(-0.5f, -0.5f) * size, Vector2(0.5f, -0.5f) * size, Vector2(0.5f, 0.5f) * size, Vector2(-0.5f, 0.5f) * size };
	}
	Rect2 r;
	for (int i = 0; i < pts.size(); i++) {
		const Vector3 p = t.xform(Vector3(pts[i].x, 0.0f, pts[i].y));
		if (i == 0) {
			r = Rect2(p.x, p.z, 0.0f, 0.0f);
		} else {
			r.expand_to(Vector2(p.x, p.z));
		}
	}
	return r;
}

void WorldBurnScar3D::set_age_years(float p_years) {
	age_years = MAX(p_years, 0.0f);
	_report();
}

void WorldBurnScar3D::set_size(const Vector2 &p_size) {
	size = Vector2(MAX(p_size.x, 0.0f), MAX(p_size.y, 0.0f));
	_report();
}

void WorldBurnScar3D::set_polygon(const PackedVector2Array &p_polygon) {
	polygon = p_polygon;
	_report();
}

bool WorldBurnScar3D::covers(const Vector3 &p_position) const {
	if (!is_inside_tree()) {
		return false;
	}
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	const Vector2 q(l.x, l.z);
	if (polygon.size() >= 3) {
		return Geometry2D::is_point_in_polygon(q, Vector<Vector2>(polygon));
	}
	return Math::abs(q.x) <= size.x * 0.5f && Math::abs(q.y) <= size.y * 0.5f;
}

void WorldBurnScar3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_age_years", "years"), &WorldBurnScar3D::set_age_years);
	ClassDB::bind_method(D_METHOD("get_age_years"), &WorldBurnScar3D::get_age_years);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &WorldBurnScar3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &WorldBurnScar3D::get_size);
	ClassDB::bind_method(D_METHOD("set_polygon", "polygon"), &WorldBurnScar3D::set_polygon);
	ClassDB::bind_method(D_METHOD("get_polygon"), &WorldBurnScar3D::get_polygon);
	ClassDB::bind_method(D_METHOD("covers", "position"), &WorldBurnScar3D::covers);
	ClassDB::bind_method(D_METHOD("get_area"), &WorldBurnScar3D::get_area);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "age_years", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:years"), "set_age_years", "get_age_years");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "polygon"), "set_polygon", "get_polygon");
}

/**************************************************************************/
/*  world_exclusion_3d.cpp                                                */
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

#include "world_exclusion_3d.h"

#include "world_contracts.h"

#include "core/math/geometry_2d.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/curve.h"

LocalVector<WorldExclusion3D *> WorldExclusion3D::registry;

namespace {
inline float edge_weight(real_t p_inside, real_t p_fade) {
	// p_inside: metres inside the edge (negative outside).
	if (p_inside <= 0.0) {
		return 0.0f;
	}
	if (p_fade <= 0.0 || p_inside >= p_fade) {
		return 1.0f;
	}
	const float t = float(p_inside / p_fade);
	return t * t * (3.0f - 2.0f * t);
}

real_t segment_distance(const Vector2 &p, const Vector2 &a, const Vector2 &b) {
	const Vector2 ab = b - a;
	const real_t l2 = ab.length_squared();
	const real_t t = l2 > 0.0 ? CLAMP((p - a).dot(ab) / l2, real_t(0.0), real_t(1.0)) : real_t(0.0);
	return p.distance_to(a + ab * t);
}
} // namespace

float WorldExclusionShape::keep_out(const Vector2 &p_xz) const {
	if (!bounds.has_point(p_xz)) {
		return 0.0f;
	}
	switch (type) {
		case TYPE_BOX: {
			const Vector2 l = to_local.xform(p_xz);
			// Inside distance in local units scaled back to metres by the frame's scale.
			const real_t sx = 1.0 / MAX(to_local.get_scale().x, real_t(1e-6));
			const real_t sz = 1.0 / MAX(to_local.get_scale().y, real_t(1e-6));
			const real_t in = MIN((half.x - Math::abs(l.x)) * sx, (half.y - Math::abs(l.y)) * sz);
			return edge_weight(in, fade);
		}
		case TYPE_AREA: {
			if (points.size() < 3) {
				return 0.0f;
			}
			real_t d = 1e30;
			const int n = points.size();
			for (int i = 0; i < n; i++) {
				d = MIN(d, segment_distance(p_xz, points[i], points[(i + 1) % n]));
			}
			const bool inside = Geometry2D::is_point_in_polygon(p_xz, points);
			return inside ? edge_weight(d, fade) : 0.0f;
		}
		case TYPE_TRACK: {
			real_t d = 1e30;
			for (int i = 0; i + 1 < points.size(); i++) {
				d = MIN(d, segment_distance(p_xz, points[i], points[i + 1]));
			}
			return edge_weight(half_width - d, fade);
		}
	}
	return 0.0f;
}

bool WorldExclusions::any_in(const Rect2 &p_rect) const {
	for (const WorldExclusionShape &s : shapes) {
		if (s.bounds.intersects(p_rect)) {
			return true;
		}
	}
	return false;
}

float WorldExclusions::keep_out(const Vector2 &p_xz) const {
	float k = 0.0f;
	for (const WorldExclusionShape &s : shapes) {
		k = MAX(k, s.keep_out(p_xz));
		if (k >= 1.0f) {
			break;
		}
	}
	return k;
}

void WorldExclusions::read(uint32_t p_kind) {
	shapes.clear();
	WorldExclusion3D::read_all(p_kind, shapes);
}

void WorldExclusion3D::read_all(uint32_t p_kind, LocalVector<WorldExclusionShape> &r_shapes) {
	for (const WorldExclusion3D *e : registry) {
		if (e->kinds & p_kind) {
			WorldExclusionShape s = e->get_shape();
			if (s.bounds.has_area()) {
				r_shapes.push_back(s);
			}
		}
	}
}

WorldExclusionShape WorldExclusion3D::get_shape() const {
	WorldExclusionShape s;
	s.kinds = kinds;
	s.fade = fade;
	const Transform3D xf = is_inside_tree() ? get_global_transform() : get_transform();
	const Ref<Curve3D> c = get_curve();
	const int count = c.is_valid() ? c->get_point_count() : 0;
	if (count >= 2) {
		// The baked curve, so Bezier handles shape the area as drawn.
		const PackedVector3Array baked = c->get_baked_points();
		const PackedVector3Array &pts = baked.size() >= 2 ? baked : PackedVector3Array();
		for (int i = 0; i < pts.size(); i++) {
			const Vector3 w = xf.xform(pts[i]);
			s.points.push_back(Vector2(w.x, w.z));
		}
		if (s.points.is_empty()) {
			for (int i = 0; i < count; i++) {
				const Vector3 w = xf.xform(c->get_point_position(i));
				s.points.push_back(Vector2(w.x, w.z));
			}
		}
		s.type = closed && count >= 3 ? WorldExclusionShape::TYPE_AREA : WorldExclusionShape::TYPE_TRACK;
		s.half_width = width * 0.5;
		Rect2 b(s.points[0], Vector2());
		for (const Vector2 &p : s.points) {
			b.expand_to(p);
		}
		s.bounds = b.grow(s.type == WorldExclusionShape::TYPE_TRACK ? s.half_width : real_t(0.0));
		return s;
	}
	s.type = WorldExclusionShape::TYPE_BOX;
	s.half = size * 0.5;
	// The node's frame in x, z: x axis, z axis and origin.
	const Vector3 ax = xf.basis.get_column(0);
	const Vector3 az = xf.basis.get_column(2);
	const Transform2D frame(Vector2(ax.x, ax.z), Vector2(az.x, az.z), Vector2(xf.origin.x, xf.origin.z));
	s.to_local = frame.affine_inverse();
	Rect2 b;
	for (int i = 0; i < 4; i++) {
		const Vector2 corner = frame.xform(Vector2(i & 1 ? s.half.x : -s.half.x, i & 2 ? s.half.y : -s.half.y));
		if (i == 0) {
			b = Rect2(corner, Vector2());
		} else {
			b.expand_to(corner);
		}
	}
	s.bounds = b;
	return s;
}

float WorldExclusion3D::get_keep_out_at(const Vector3 &p_position) const {
	return get_shape().keep_out(Vector2(p_position.x, p_position.z));
}

// Something about the area changed: announce the old and new ground once, at the end of the frame.
void WorldExclusion3D::_changed() {
	update_gizmos();
	if (!is_inside_tree() || change_queued) {
		return;
	}
	change_queued = true;
	callable_mp(this, &WorldExclusion3D::_announce).call_deferred();
}

void WorldExclusion3D::_announce() {
	change_queued = false;
	const Rect2 now = is_inside_tree() ? get_shape().bounds : Rect2();
	Rect2 area = published;
	if (now.has_area()) {
		area = area.has_area() ? area.merge(now) : now;
	}
	published = now;
	if (area.has_area() && WorldContracts::get_singleton()) {
		WorldContracts::get_singleton()->emit_signal(SNAME("exclusions_changed"), area);
	}
}

void WorldExclusion3D::_curve_changed() {
	_changed();
}

void WorldExclusion3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			registry.push_back(this);
			set_notify_transform(true);
			const Callable cb = callable_mp(this, &WorldExclusion3D::_curve_changed);
			if (!is_connected(SNAME("curve_changed"), cb)) {
				connect(SNAME("curve_changed"), cb);
			}
			_changed();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			registry.erase(this);
			if (published.has_area() && WorldContracts::get_singleton()) {
				WorldContracts::get_singleton()->emit_signal(SNAME("exclusions_changed"), published);
			}
			published = Rect2();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_changed();
		} break;
	}
}

void WorldExclusion3D::set_kinds(uint32_t p_kinds) {
	if (kinds == p_kinds) {
		return;
	}
	// Both the systems that kept out and those that now keep out place this ground again.
	kinds = p_kinds;
	_changed();
}

void WorldExclusion3D::set_closed(bool p_closed) {
	closed = p_closed;
	_changed();
}

void WorldExclusion3D::set_size(const Vector2 &p_size) {
	size = p_size.max(Vector2());
	_changed();
}

void WorldExclusion3D::set_width(real_t p_width) {
	width = MAX(p_width, real_t(0.0));
	_changed();
}

void WorldExclusion3D::set_fade(real_t p_fade) {
	fade = MAX(p_fade, real_t(0.0));
	_changed();
}

PackedStringArray WorldExclusion3D::get_configuration_warnings() const {
	PackedStringArray w = Path3D::get_configuration_warnings();
	if (kinds == 0) {
		w.push_back(RTR("Keeps nothing out: pick at least one of plants, grass and rocks in Kinds."));
	}
	return w;
}

void WorldExclusion3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_kinds", "kinds"), &WorldExclusion3D::set_kinds);
	ClassDB::bind_method(D_METHOD("get_kinds"), &WorldExclusion3D::get_kinds);
	ClassDB::bind_method(D_METHOD("set_closed", "closed"), &WorldExclusion3D::set_closed);
	ClassDB::bind_method(D_METHOD("is_closed"), &WorldExclusion3D::is_closed);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &WorldExclusion3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &WorldExclusion3D::get_size);
	ClassDB::bind_method(D_METHOD("set_width", "width"), &WorldExclusion3D::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &WorldExclusion3D::get_width);
	ClassDB::bind_method(D_METHOD("set_fade", "fade"), &WorldExclusion3D::set_fade);
	ClassDB::bind_method(D_METHOD("get_fade"), &WorldExclusion3D::get_fade);
	ClassDB::bind_method(D_METHOD("get_keep_out_at", "position"), &WorldExclusion3D::get_keep_out_at);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "kinds", PROPERTY_HINT_FLAGS, "Plants,Grass,Rocks"), "set_kinds", "get_kinds");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "closed"), "set_closed", "is_closed");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:m"), "set_fade", "get_fade");

	BIND_ENUM_CONSTANT(KIND_PLANTS);
	BIND_ENUM_CONSTANT(KIND_GRASS);
	BIND_ENUM_CONSTANT(KIND_ROCKS);
}

WorldExclusion3D::WorldExclusion3D() {
}

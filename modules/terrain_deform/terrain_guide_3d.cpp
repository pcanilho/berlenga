/**************************************************************************/
/*  terrain_guide_3d.cpp                                                  */
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

#include "terrain_guide_3d.h"

#include "terrain_generator_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

// Tell the generator above (if any) so its preview can follow the guide.
void TerrainGuide3D::_changed() {
	if (!is_inside_tree()) {
		return;
	}
	for (Node *p = get_parent(); p; p = p->get_parent()) {
		if (TerrainGenerator3D *g = Object::cast_to<TerrainGenerator3D>(p)) {
			g->guide_changed();
			return;
		}
	}
}

void TerrainGuide3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_notify_transform(true);
			if (!is_connected(SNAME("curve_changed"), callable_mp(this, &TerrainGuide3D::_changed))) {
				connect(SNAME("curve_changed"), callable_mp(this, &TerrainGuide3D::_changed));
			}
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_changed();
		} break;
	}
}

void TerrainGuide3D::set_mode(Mode p_mode) {
	mode = p_mode;
	notify_property_list_changed();
	update_gizmos();
	_changed();
}

void TerrainGuide3D::set_width(real_t p_width) {
	width = MAX(p_width, real_t(1.0));
	update_gizmos();
	_changed();
}

void TerrainGuide3D::set_strength(real_t p_strength) {
	strength = CLAMP(p_strength, real_t(0.0), real_t(4.0));
	_changed();
}

void TerrainGuide3D::set_bar_width(real_t p_width) {
	bar_width = MAX(p_width, real_t(1.0));
	_changed();
}

void TerrainGuide3D::set_apron_slope(real_t p_slope) {
	apron_slope = MAX(p_slope, real_t(0.0));
	_changed();
}

void TerrainGuide3D::set_lip_clear(real_t p_distance) {
	lip_clear = MAX(p_distance, real_t(0.0));
	_changed();
}

void TerrainGuide3D::set_beach_width(real_t p_width) {
	beach_width = MAX(p_width, real_t(-1.0));
	_changed();
}

void TerrainGuide3D::set_roughness(real_t p_roughness) {
	roughness = MAX(p_roughness, real_t(0.0));
	_changed();
}

void TerrainGuide3D::set_invert(bool p_invert) {
	invert = p_invert;
	_changed();
}

bool TerrainGuide3D::is_sea_at(const Vector2 &p_point) const {
	const PackedVector3Array pts = get_global_points();
	if (pts.size() < 2) {
		return false;
	}
	const Ref<Curve3D> c = get_curve();
	const bool closed = c.is_valid() && (c->is_closed() || Vector2(pts[0].x, pts[0].z).distance_to(Vector2(pts[pts.size() - 1].x, pts[pts.size() - 1].z)) < 1.0);
	bool sea = false;
	if (closed) {
		// Inside by the crossing rule.
		for (int i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
			const real_t zi = pts[i].z, zj = pts[j].z;
			if ((zi > p_point.y) != (zj > p_point.y) && p_point.x < (pts[j].x - pts[i].x) * (p_point.y - zi) / (zj - zi) + pts[i].x) {
				sea = !sea;
			}
		}
	} else {
		// Left of the nearest segment, looking along the line (x right, z towards the viewer: left is (dz, -dx)).
		real_t best = 1e30;
		for (int i = 0; i + 1 < pts.size(); i++) {
			const Vector2 a(pts[i].x, pts[i].z), b(pts[i + 1].x, pts[i + 1].z);
			const Vector2 ab = b - a;
			const real_t len2 = ab.length_squared();
			if (len2 <= 0.0) {
				continue;
			}
			const real_t t = CLAMP((p_point - a).dot(ab) / len2, real_t(0.0), real_t(1.0));
			const real_t d = p_point.distance_squared_to(a + ab * t);
			if (d < best - 1e-6f) {
				best = d;
				sea = ab.cross(p_point - a) < 0.0;
			}
		}
	}
	return sea != invert;
}

real_t TerrainGuide3D::distance_to(const Vector2 &p_point) const {
	const PackedVector3Array pts = get_global_points();
	real_t best = 1e30;
	if (pts.size() == 1) {
		return p_point.distance_to(Vector2(pts[0].x, pts[0].z));
	}
	for (int i = 0; i + 1 < pts.size(); i++) {
		const Vector2 a(pts[i].x, pts[i].z), b(pts[i + 1].x, pts[i + 1].z);
		const Vector2 ab = b - a;
		const real_t len2 = ab.length_squared();
		const real_t t = len2 > 0.0 ? CLAMP((p_point - a).dot(ab) / len2, real_t(0.0), real_t(1.0)) : real_t(0.0);
		best = MIN(best, p_point.distance_to(a + ab * t));
	}
	return best;
}

void TerrainGuide3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	_changed();
}

PackedVector3Array TerrainGuide3D::get_global_points() const {
	PackedVector3Array out;
	const Ref<Curve3D> c = get_curve();
	if (c.is_null() || c->get_point_count() == 0) {
		return out;
	}
	const Transform3D xf = is_inside_tree() ? get_global_transform() : get_transform();
	if (c->get_point_count() == 1) {
		out.push_back(xf.xform(c->get_point_position(0)));
		return out;
	}
	const PackedVector3Array baked = c->get_baked_points();
	out.resize(baked.size());
	for (int i = 0; i < baked.size(); i++) {
		out.set(i, xf.xform(baked[i]));
	}
	if (c->is_closed() && out.size() > 1 && out[0].distance_to(out[out.size() - 1]) > 1e-3f) {
		out.push_back(out[0]);
	}
	return out;
}

Vector<TerrainGuide3D::BarPoint> TerrainGuide3D::get_bar_points() const {
	Vector<BarPoint> out;
	const PackedVector3Array pts = get_global_points();
	if (pts.is_empty()) {
		return out;
	}
	// Discs every 32 m along the line (the bar's level is each point's height, interpolated).
	LocalVector<Vector3> along;
	along.push_back(pts[0]);
	real_t carried = 0.0;
	for (int i = 0; i + 1 < pts.size(); i++) {
		const real_t seg = pts[i].distance_to(pts[i + 1]);
		real_t t = 32.0 - carried;
		while (t <= seg) {
			along.push_back(pts[i].lerp(pts[i + 1], seg > 0.0 ? t / seg : 0.0));
			t += 32.0;
		}
		carried = seg - (t - 32.0);
	}
	if (pts.size() > 1 && along[along.size() - 1].distance_to(pts[pts.size() - 1]) > 1.0) {
		along.push_back(pts[pts.size() - 1]);
	}
	int lip = 0;
	for (uint32_t i = 0; i < along.size(); i++) {
		BarPoint b;
		b.position = Vector2(along[i].x, along[i].z);
		b.radius = bar_width;
		b.level = along[i].y;
		out.push_back(b);
		if (along[i].y < along[lip].y) {
			lip = int(i);
		}
	}
	if (apron_slope <= 0.0 || along.size() < 2) {
		return out;
	}
	// The apron: each point's rock falls away straight out to the line's right at apron_slope, so their union is one
	// smooth rock slope that leaves the lip's stretch clear.
	for (uint32_t i = 0; i < along.size(); i++) {
		if (along[i].distance_to(along[lip]) < lip_clear) {
			continue;
		}
		const Vector3 a = along[i > 0 ? i - 1 : 0];
		const Vector3 b = along[i + 1 < along.size() ? i + 1 : i];
		Vector2 dir(b.x - a.x, b.z - a.z);
		if (dir.length_squared() < 1e-6f) {
			continue;
		}
		dir = dir.normalized();
		out.write[i].slope = apron_slope;
		out.write[i].side = Vector2(-dir.y, dir.x); // the right hand looking along the line (x right, z towards the viewer)
	}
	return out;
}

PackedVector3Array TerrainGuide3D::get_bar_points_array() const {
	PackedVector3Array out;
	for (const BarPoint &b : get_bar_points()) {
		out.push_back(Vector3(b.position.x, b.level, b.position.y));
	}
	return out;
}

void TerrainGuide3D::_validate_property(PropertyInfo &p_property) const {
	if ((mode == MODE_BAR || mode == MODE_COAST) && (p_property.name == "width" || p_property.name == "strength")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (mode == MODE_BEACH && p_property.name == "strength") {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (mode != MODE_COAST && mode != MODE_BEACH && p_property.name == "beach_width") {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (mode != MODE_COAST && (p_property.name == "invert" || p_property.name == "roughness")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (mode != MODE_BAR && (p_property.name == "bar_width" || p_property.name == "apron_slope" || p_property.name == "lip_clear")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void TerrainGuide3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &TerrainGuide3D::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &TerrainGuide3D::get_mode);
	ClassDB::bind_method(D_METHOD("set_width", "width"), &TerrainGuide3D::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &TerrainGuide3D::get_width);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &TerrainGuide3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &TerrainGuide3D::get_strength);
	ClassDB::bind_method(D_METHOD("set_bar_width", "width"), &TerrainGuide3D::set_bar_width);
	ClassDB::bind_method(D_METHOD("get_bar_width"), &TerrainGuide3D::get_bar_width);
	ClassDB::bind_method(D_METHOD("set_apron_slope", "slope"), &TerrainGuide3D::set_apron_slope);
	ClassDB::bind_method(D_METHOD("get_apron_slope"), &TerrainGuide3D::get_apron_slope);
	ClassDB::bind_method(D_METHOD("set_lip_clear", "distance"), &TerrainGuide3D::set_lip_clear);
	ClassDB::bind_method(D_METHOD("get_lip_clear"), &TerrainGuide3D::get_lip_clear);
	ClassDB::bind_method(D_METHOD("set_beach_width", "width"), &TerrainGuide3D::set_beach_width);
	ClassDB::bind_method(D_METHOD("get_beach_width"), &TerrainGuide3D::get_beach_width);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &TerrainGuide3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &TerrainGuide3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_invert", "invert"), &TerrainGuide3D::set_invert);
	ClassDB::bind_method(D_METHOD("is_invert"), &TerrainGuide3D::is_invert);
	ClassDB::bind_method(D_METHOD("is_sea_at", "point"), &TerrainGuide3D::is_sea_at);
	ClassDB::bind_method(D_METHOD("distance_to", "point"), &TerrainGuide3D::distance_to);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &TerrainGuide3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &TerrainGuide3D::is_enabled);
	ClassDB::bind_method(D_METHOD("get_global_points"), &TerrainGuide3D::get_global_points);
	ClassDB::bind_method(D_METHOD("get_bar_points"), &TerrainGuide3D::get_bar_points_array);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Raise,Lower,Bar,Coast,Beach"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "1,5000,1,or_greater,suffix:m"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bar_width", PROPERTY_HINT_RANGE, "1,500,1,suffix:m"), "set_bar_width", "get_bar_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "apron_slope", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_apron_slope", "get_apron_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lip_clear", PROPERTY_HINT_RANGE, "0,1000,1,suffix:m"), "set_lip_clear", "get_lip_clear");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "beach_width", PROPERTY_HINT_RANGE, "-1,500,0.1,or_greater,suffix:m"), "set_beach_width", "get_beach_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:m"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "invert"), "set_invert", "is_invert");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");

	BIND_ENUM_CONSTANT(MODE_RAISE);
	BIND_ENUM_CONSTANT(MODE_LOWER);
	BIND_ENUM_CONSTANT(MODE_BAR);
	BIND_ENUM_CONSTANT(MODE_COAST);
	BIND_ENUM_CONSTANT(MODE_BEACH);
}

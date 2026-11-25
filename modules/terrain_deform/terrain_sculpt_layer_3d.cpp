/**************************************************************************/
/*  terrain_sculpt_layer_3d.cpp                                           */
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

#include "terrain_sculpt_layer_3d.h"

#include "core/object/class_db.h"

TerrainSculptLayer3D::TerrainSculptLayer3D() {
	set_mode(MODE_ADD);
	set_falloff(0.0);
}

void TerrainSculptLayer3D::_read() {
	image.unref();
	if (height_map.is_valid()) {
		Ref<Image> im = height_map->get_image();
		if (im.is_valid() && !im->is_empty()) {
			im = im->duplicate();
			if (im->is_compressed()) {
				im->decompress();
			}
			im->clear_mipmaps();
			im->convert(Image::FORMAT_RF);
			image = im;
		}
	}
	// The brush's size is the map's, so the generator and deformer take it as any other touch-up.
	set_size(height_map_rect.has_area() ? height_map_rect.size : Vector2());
	_changed();
}

void TerrainSculptLayer3D::set_height_map(const Ref<Texture2D> &p_map) {
	height_map = p_map;
	_read();
}

void TerrainSculptLayer3D::set_height_map_rect(const Rect2 &p_rect) {
	height_map_rect = p_rect;
	_read();
}

void TerrainSculptLayer3D::set_height_scale(real_t p_scale) {
	height_scale = p_scale;
	_changed();
}

void TerrainSculptLayer3D::notify_height_map_changed(const Rect2 &p_area) {
	_read(); // the generator re-applies its touch-ups a moment later
}

float TerrainSculptLayer3D::offset_at(real_t p_x, real_t p_z) const {
	if (image.is_null() || !height_map_rect.has_point(Vector2(p_x, p_z))) {
		return 0.0f;
	}
	const int w = image->get_width();
	const int h = image->get_height();
	const float fx = float((p_x - height_map_rect.position.x) / height_map_rect.size.x) * w - 0.5f;
	const float fz = float((p_z - height_map_rect.position.y) / height_map_rect.size.y) * h - 0.5f;
	const int x0 = CLAMP(int(Math::floor(fx)), 0, w - 1);
	const int z0 = CLAMP(int(Math::floor(fz)), 0, h - 1);
	const int x1 = MIN(x0 + 1, w - 1);
	const int z1 = MIN(z0 + 1, h - 1);
	const float ax = CLAMP(fx - Math::floor(fx), 0.0f, 1.0f);
	const float az = CLAMP(fz - Math::floor(fz), 0.0f, 1.0f);
	const float *d = reinterpret_cast<const float *>(image->ptr());
	const float a = Math::lerp(d[z0 * w + x0], d[z0 * w + x1], ax);
	const float b = Math::lerp(d[z1 * w + x0], d[z1 * w + x1], ax);
	return Math::lerp(a, b, az) * float(height_scale);
}

Rect2 TerrainSculptLayer3D::get_world_rect() const {
	return is_inside_tree() ? height_map_rect : Rect2();
}

// The layer ignores its own transform: the map's rect is in world x, z, as the other painted maps.
void TerrainSculptLayer3D::sample_world(const Transform3D &p_xf, const Transform3D &p_inv, real_t p_x, real_t p_z, float &r_height, float &r_mask) const {
	const bool in = image.is_valid() && height_map_rect.has_point(Vector2(p_x, p_z));
	r_height = in ? offset_at(p_x, p_z) : 0.0f;
	r_mask = in ? 1.0f : 0.0f;
}

// The brush's shape settings do not apply to a painted layer: hidden from the inspector.
void TerrainSculptLayer3D::_validate_property(PropertyInfo &p_property) const {
	static const char *hidden[] = { "shape", "mode", "profile", "size", "depth", "falloff" };
	for (const char *h : hidden) {
		if (p_property.name == h) {
			p_property.usage = PROPERTY_USAGE_STORAGE;
		}
	}
}

void TerrainSculptLayer3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_height_map", "map"), &TerrainSculptLayer3D::set_height_map);
	ClassDB::bind_method(D_METHOD("get_height_map"), &TerrainSculptLayer3D::get_height_map);
	ClassDB::bind_method(D_METHOD("set_height_map_rect", "rect"), &TerrainSculptLayer3D::set_height_map_rect);
	ClassDB::bind_method(D_METHOD("get_height_map_rect"), &TerrainSculptLayer3D::get_height_map_rect);
	ClassDB::bind_method(D_METHOD("set_height_scale", "scale"), &TerrainSculptLayer3D::set_height_scale);
	ClassDB::bind_method(D_METHOD("get_height_scale"), &TerrainSculptLayer3D::get_height_scale);
	ClassDB::bind_method(D_METHOD("notify_height_map_changed", "area"), &TerrainSculptLayer3D::notify_height_map_changed, DEFVAL(Rect2()));
	ClassDB::bind_method(D_METHOD("get_offset_at", "x", "z"), &TerrainSculptLayer3D::offset_at);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_height_map", "get_height_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "height_map_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_height_map_rect", "get_height_map_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_scale", PROPERTY_HINT_RANGE, "-10,10,0.01,or_greater,or_less"), "set_height_scale", "get_height_scale");
}

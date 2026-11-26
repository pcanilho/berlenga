/**************************************************************************/
/*  world_mask_paint_plugin.cpp                                           */
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

#include "world_mask_paint_plugin.h"

#include "core/input/input_event.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {
// A brush's smooth falloff: 1 at the centre, 0 at the edge (smoothstep of the distance).
inline float falloff(float p_d, float p_radius) {
	const float f = 1.0f - CLAMP(p_d / MAX(p_radius, 1e-4f), 0.0f, 1.0f);
	return f * f * (3.0f - 2.0f * f);
}

const char *CHANNEL_NAMES[4] = { "R", "G", "B", "A" };
} // namespace

Vector<WorldMaskPaintPlugin::Slot> WorldMaskPaintPlugin::find_slots(const Object *p_node) {
	Vector<Slot> out;
	if (!p_node) {
		return out;
	}
	List<PropertyInfo> props;
	p_node->get_property_list(&props);
	HashSet<String> names;
	for (const PropertyInfo &pi : props) {
		names.insert(pi.name);
	}
	const bool world_rect = p_node->has_method(SNAME("get_world_rect"));
	for (const PropertyInfo &pi : props) {
		if (pi.type != Variant::OBJECT || pi.hint != PROPERTY_HINT_RESOURCE_TYPE || !(pi.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}
		const bool is_image = pi.hint_string == "Image";
		if (!is_image && !pi.hint_string.split(",").has("Texture2D")) {
			continue;
		}
		const String name = pi.name;
		String stem;
		if (name.ends_with("_map")) {
			stem = name.trim_suffix("_map");
		} else if (name.ends_with("_mask")) {
			stem = name.trim_suffix("_mask");
		} else {
			continue;
		}
		Slot s;
		s.property = name;
		s.image = is_image;
		if (names.has(name + "_rect")) {
			s.rect_property = name + "_rect";
		} else if (names.has(stem + "_rect")) {
			s.rect_property = stem + "_rect";
		} else if (!world_rect) {
			continue;
		}
		// Density masks start full (every plant kept); the other maps start empty.
		s.fill = name.ends_with("density_mask") ? 1.0f : 0.0f;
		s.height = name.ends_with("height_map");
		out.push_back(s);
	}
	return out;
}

Rect2 WorldMaskPaintPlugin::slot_rect(const Object *p_node, const Slot &p_slot) {
	if (!p_node) {
		return Rect2();
	}
	if (p_slot.rect_property != StringName()) {
		return p_node->get(p_slot.rect_property);
	}
	return const_cast<Object *>(p_node)->call(SNAME("get_world_rect"));
}

Rect2i WorldMaskPaintPlugin::apply_dab(const Ref<Image> &p_image, int p_channel, Operation p_operation, const Vector2 &p_center, float p_radius, float p_strength, float p_value) {
	ERR_FAIL_COND_V(p_image.is_null() || (p_image->get_format() != Image::FORMAT_RGBA8 && p_image->get_format() != Image::FORMAT_RF), Rect2i());
	ERR_FAIL_INDEX_V(p_channel, 4, Rect2i());
	const bool metres = p_image->get_format() == Image::FORMAT_RF;
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	const float r = MAX(p_radius, 0.5f);
	const int x0 = MAX(int(Math::floor(p_center.x - r)), 0);
	const int y0 = MAX(int(Math::floor(p_center.y - r)), 0);
	const int x1 = MIN(int(Math::ceil(p_center.x + r)), w - 1);
	const int y1 = MIN(int(Math::ceil(p_center.y + r)), h - 1);
	if (x1 < x0 || y1 < y0) {
		return Rect2i();
	}
	uint8_t *px = p_image->ptrw();
	float *fp = metres ? reinterpret_cast<float *>(px) : nullptr;
	float avg = 0.0f;
	if (p_operation == OP_SMOOTH) {
		float ws = 0.0f;
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				const float f = falloff(Vector2(x + 0.5f, y + 0.5f).distance_to(p_center), r);
				avg += (metres ? fp[y * w + x] : px[(y * w + x) * 4 + p_channel] / 255.0f) * f;
				ws += f;
			}
		}
		if (ws <= 0.0f) {
			return Rect2i();
		}
		avg /= ws;
	}
	if (metres) {
		// A height map: Add raises by Value metres at the centre, Subtract lowers, Replace eases to Value, Smooth to
		// the brush's mean (Terrain3D's sculpt operations).
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				const float f = falloff(Vector2(x + 0.5f, y + 0.5f).distance_to(p_center), r) * CLAMP(p_strength, 0.0f, 1.0f);
				if (f <= 0.0f) {
					continue;
				}
				float &v = fp[y * w + x];
				switch (p_operation) {
					case OP_ADD:
						v += p_value * f;
						break;
					case OP_SUBTRACT:
						v -= p_value * f;
						break;
					case OP_REPLACE:
						v = Math::lerp(v, p_value, f);
						break;
					case OP_SMOOTH:
						v = Math::lerp(v, avg, f);
						break;
				}
			}
		}
		return Rect2i(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
	}
	const float value = CLAMP(p_value, 0.0f, 1.0f);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const float f = falloff(Vector2(x + 0.5f, y + 0.5f).distance_to(p_center), r) * CLAMP(p_strength, 0.0f, 1.0f);
			if (f <= 0.0f) {
				continue;
			}
			uint8_t &c = px[(y * w + x) * 4 + p_channel];
			const float v = c / 255.0f;
			float n = v;
			switch (p_operation) {
				case OP_ADD:
					n = v + value * f;
					break;
				case OP_SUBTRACT:
					n = v - value * f;
					break;
				case OP_REPLACE:
					n = Math::lerp(v, value, f);
					break;
				case OP_SMOOTH:
					n = Math::lerp(v, avg, f);
					break;
			}
			// Round toward the change, so weak strokes still move a value stored in 8 bits.
			const float t = CLAMP(n, 0.0f, 1.0f) * 255.0f;
			c = uint8_t(n > v ? Math::ceil(t - 1e-3f) : Math::floor(t + 1e-3f));
		}
	}
	return Rect2i(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

WorldMaskPaintPlugin::WorldMaskPaintPlugin() {
	toolbar = memnew(HBoxContainer);
	toolbar->hide();
	toolbar->add_child(memnew(VSeparator));

	paint_button = memnew(Button);
	paint_button->set_text(TTRC("Paint Mask"));
	paint_button->set_toggle_mode(true);
	paint_button->set_theme_type_variation("FlatButton");
	paint_button->set_tooltip_text(TTRC("Paint this node's mask map on the ground.\nLMB: apply the operation. Shift+LMB: subtract. Ctrl+LMB: smooth."));
	paint_button->connect(SceneStringName(toggled), callable_mp(this, &WorldMaskPaintPlugin::_paint_toggled));
	toolbar->add_child(paint_button);

	slot_option = memnew(OptionButton);
	slot_option->set_tooltip_text(TTRC("The mask map to paint."));
	slot_option->connect(SceneStringName(item_selected), callable_mp(this, &WorldMaskPaintPlugin::_slot_selected));
	toolbar->add_child(slot_option);

	channel_option = memnew(OptionButton);
	for (int i = 0; i < 4; i++) {
		channel_option->add_item(CHANNEL_NAMES[i], i);
	}
	channel_option->set_tooltip_text(TTRC("The channel to paint (a plant species reads the channel set in its mask_channel)."));
	channel_option->connect(SceneStringName(item_selected), callable_mp(this, &WorldMaskPaintPlugin::_slot_selected));
	toolbar->add_child(channel_option);

	operation_option = memnew(OptionButton);
	operation_option->add_item(TTR("Add"), OP_ADD);
	operation_option->add_item(TTR("Subtract"), OP_SUBTRACT);
	operation_option->add_item(TTR("Replace"), OP_REPLACE);
	operation_option->add_item(TTR("Smooth"), OP_SMOOTH);
	operation_option->select(OP_REPLACE);
	toolbar->add_child(operation_option);

	auto add_spin = [&](const String &p_label, double p_min, double p_max, double p_step, double p_value, const String &p_suffix) {
		Label *l = memnew(Label);
		l->set_text(p_label);
		toolbar->add_child(l);
		SpinBox *s = memnew(SpinBox);
		s->set_min(p_min);
		s->set_max(p_max);
		s->set_step(p_step);
		s->set_value(p_value);
		s->set_suffix(p_suffix);
		s->set_allow_greater(true);
		s->set_select_all_on_focus(true);
		toolbar->add_child(s);
		return s;
	};
	value_spin = add_spin(TTR("Value"), 0.0, 1.0, 0.01, 1.0, "");
	value_spin->set_allow_greater(false);
	radius_spin = add_spin(TTR("Radius"), 0.5, 500.0, 0.5, 16.0, "m");
	strength_spin = add_spin(TTR("Strength"), 0.0, 1.0, 0.01, 0.5, "");
	strength_spin->set_allow_greater(false);
	texel_spin = add_spin(TTR("New Map Texel"), 0.25, 64.0, 0.25, 1.0, "m");
	texel_spin->set_tooltip_text(TTRC("The texel size of a map this tool creates for an empty slot."));

	Button *fill = memnew(Button);
	fill->set_text(TTRC("Fill"));
	fill->set_tooltip_text(TTRC("Set the channel to Value everywhere."));
	fill->set_theme_type_variation("FlatButton");
	fill->connect(SceneStringName(pressed), callable_mp(this, &WorldMaskPaintPlugin::_fill));
	toolbar->add_child(fill);

	Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);
}

void WorldMaskPaintPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_undo_blit", "node", "property", "part", "at"), &WorldMaskPaintPlugin::_undo_blit);
}

bool WorldMaskPaintPlugin::handles(Object *p_object) const {
	return Object::cast_to<Node3D>(p_object) && !find_slots(p_object).is_empty();
}

void WorldMaskPaintPlugin::edit(Object *p_object) {
	Node3D *next = Object::cast_to<Node3D>(p_object);
	if (next != node) {
		paint_button->set_pressed(false);
		_clear_overlay();
	}
	node = next;
	_update_slots();
}

void WorldMaskPaintPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	if (!p_visible) {
		paint_button->set_pressed(false);
		_clear_overlay();
	}
}

void WorldMaskPaintPlugin::_update_slots() {
	slots = find_slots(node);
	const String keep = slot_option->get_selected() >= 0 ? slot_option->get_item_text(slot_option->get_selected()) : String();
	slot_option->clear();
	for (int i = 0; i < slots.size(); i++) {
		slot_option->add_item(String(slots[i].property).capitalize(), i);
		if (slot_option->get_item_text(i) == keep) {
			slot_option->select(i);
		}
	}
}

void WorldMaskPaintPlugin::_slot_selected(int p_index) {
	if (paint_button->is_pressed()) {
		if (_prepare()) {
			_update_overlay(Rect2i(Point2i(), image->get_size()));
		} else {
			paint_button->set_pressed(false);
		}
	}
}

void WorldMaskPaintPlugin::_paint_toggled(bool p_on) {
	if (p_on) {
		if (!_prepare()) {
			paint_button->set_pressed_no_signal(false);
			return;
		}
		_update_overlay(Rect2i(Point2i(), image->get_size()));
	} else {
		_clear_overlay();
	}
}

// Reads the selected slot's map as RGBA8, or makes one over its rect when the slot is empty.
bool WorldMaskPaintPlugin::_prepare() {
	image.unref();
	const int si = slot_option->get_selected();
	if (!node || !node->is_inside_tree() || si < 0 || si >= slots.size()) {
		return false;
	}
	const Slot &slot = slots[si];
	rect = slot_rect(node, slot);
	ERR_FAIL_COND_V_MSG(!rect.has_area(), false, vformat("Set %s's %s first: the map needs a world rect.", node->get_name(), String(slot.rect_property)));
	const Variant cur = node->get(slot.property);
	Ref<Image> src;
	if (slot.image) {
		src = cur;
	} else if (Ref<Texture2D> tex = cur; tex.is_valid()) {
		src = tex->get_image();
	}
	const Image::Format format = slot.height ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	if (src.is_valid() && !src->is_empty()) {
		image = src->duplicate();
		if (image->is_compressed()) {
			image->decompress();
		}
		image->clear_mipmaps();
		image->convert(format);
	} else {
		const float texel = MAX(float(texel_spin->get_value()), 0.05f);
		const int w = CLAMP(int(Math::ceil(rect.size.x / texel)), 2, 8192);
		const int h = CLAMP(int(Math::ceil(rect.size.y / texel)), 2, 8192);
		image = Image::create_empty(w, h, false, format);
		const float v = slot.height ? slot.fill : Math::round(slot.fill * 255.0f) / 255.0f;
		image->fill(Color(v, v, v, v));
	}
	// Value: metres on a height map, else 0..1.
	value_spin->set_min(slot.height ? -1000.0 : 0.0);
	value_spin->set_max(slot.height ? 1000.0 : 1.0);
	value_spin->set_suffix(slot.height ? "m" : "");
	return true;
}

// The decal over the ground showing the painted channel: transparent at 0, tinted by the value.
void WorldMaskPaintPlugin::_update_overlay(const Rect2i &p_rect) {
	if (image.is_null() || !node) {
		return;
	}
	const int w = image->get_width();
	const int h = image->get_height();
	const bool fresh = overlay_image.is_null() || overlay_image->get_size() != image->get_size();
	if (fresh) {
		overlay_image = Image::create_empty(w, h, false, Image::FORMAT_RGBA8);
	}
	const int ch = channel_option->get_selected();
	const Rect2i r = fresh ? Rect2i(0, 0, w, h) : p_rect.intersection(Rect2i(0, 0, w, h));
	const uint8_t *src = image->ptr();
	uint8_t *dst = overlay_image->ptrw();
	const bool metres = image->get_format() == Image::FORMAT_RF;
	for (int y = r.position.y; y < r.position.y + r.size.y && metres; y++) {
		for (int x = r.position.x; x < r.position.x + r.size.x; x++) {
			// Raised ground warm, lowered ground blue, stronger to 5 m.
			const float v = reinterpret_cast<const float *>(src)[y * w + x];
			const float a = MIN(Math::abs(v) / 5.0f, 1.0f);
			uint8_t *o = dst + (y * w + x) * 4;
			o[0] = v >= 0.0f ? 255 : 40;
			o[1] = v >= 0.0f ? 140 : 110;
			o[2] = v >= 0.0f ? 40 : 255;
			o[3] = uint8_t(a * 150.0f);
		}
	}
	for (int y = r.position.y; y < r.position.y + r.size.y && !metres; y++) {
		for (int x = r.position.x; x < r.position.x + r.size.x; x++) {
			const uint8_t v = src[(y * w + x) * 4 + ch];
			uint8_t *o = dst + (y * w + x) * 4;
			o[0] = 255;
			o[1] = uint8_t(80 + v * 175 / 255);
			o[2] = 40;
			o[3] = uint8_t(v * 150 / 255);
		}
	}
	if (fresh || overlay_texture.is_null()) {
		overlay_texture = ImageTexture::create_from_image(overlay_image);
	} else {
		overlay_texture->update(overlay_image);
	}
	if (!overlay) {
		overlay = memnew(Decal);
		node->add_child(overlay, false, Node::INTERNAL_MODE_BACK); // no owner: never saved
		overlay->set_as_top_level(true);
	}
	const float top = 4000.0f;
	overlay->set_global_transform(Transform3D(Basis(), Vector3(rect.get_center().x, 0.0f, rect.get_center().y)));
	overlay->set_size(Vector3(rect.size.x, top * 2.0f, rect.size.y));
	overlay->set_texture(Decal::TEXTURE_ALBEDO, overlay_texture);
	overlay->set_albedo_mix(1.0f);
	overlay->set_upper_fade(0.0f);
	overlay->set_lower_fade(0.0f);
}

void WorldMaskPaintPlugin::_clear_overlay() {
	if (overlay) {
		overlay->queue_free();
		overlay = nullptr;
	}
	overlay_texture.unref();
	overlay_image.unref();
	image.unref();
	stroking = false;
}

// The ground under p_position: the highest generated terrain in the edited scene (read by name, as the generator is in
// another module), else the first collider, else the painted node's own height.
float WorldMaskPaintPlugin::_ground_height(const Vector3 &p_position) const {
	float best = -Math::INF;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root) {
		LocalVector<Node *> stack;
		stack.push_back(root);
		while (!stack.is_empty()) {
			Node *n = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			if (n->is_class("TerrainGenerator3D") && bool(n->call(SNAME("has_result")))) {
				const float y = n->call(SNAME("get_height"), p_position);
				if (Math::is_finite(y)) {
					best = MAX(best, y);
				}
			}
			for (int i = 0; i < n->get_child_count(); i++) {
				stack.push_back(n->get_child(i));
			}
		}
	}
	if (Math::is_finite(best)) {
		return best;
	}
	Ref<World3D> world = node ? node->get_world_3d() : Ref<World3D>();
	PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
	if (space) {
		PhysicsDirectSpaceState3D::RayParameters ray;
		ray.from = p_position + Vector3(0.0f, 5000.0f, 0.0f);
		ray.to = p_position - Vector3(0.0f, 5000.0f, 0.0f);
		PhysicsDirectSpaceState3D::RayResult hit;
		if (space->intersect_ray(ray, hit)) {
			return hit.position.y;
		}
	}
	return node ? node->get_global_position().y : 0.0f;
}

// Marches the view ray down to the ground (steps shrinking with the height above it), then bisects the crossing.
bool WorldMaskPaintPlugin::_ground_hit(Camera3D *p_camera, const Vector2 &p_screen, Vector3 &r_hit) const {
	const Vector3 from = p_camera->project_ray_origin(p_screen);
	const Vector3 dir = p_camera->project_ray_normal(p_screen);
	float t = 0.0f;
	float prev = 0.0f;
	const float far = 40000.0f;
	while (t < far) {
		const Vector3 p = from + dir * t;
		const float above = p.y - _ground_height(p);
		if (above <= 0.0f) {
			float lo = prev;
			float hi = t;
			for (int i = 0; i < 16; i++) {
				const float mid = (lo + hi) * 0.5f;
				const Vector3 q = from + dir * mid;
				if (q.y - _ground_height(q) > 0.0f) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			r_hit = from + dir * hi;
			return true;
		}
		prev = t;
		t += CLAMP(above * 0.5f, 0.25f, 50.0f);
	}
	return false;
}

void WorldMaskPaintPlugin::_dab(const Vector3 &p_world, Operation p_operation) {
	if (image.is_null() || !rect.has_area()) {
		return;
	}
	const Vector2 px((p_world.x - rect.position.x) / rect.size.x * image->get_width(), (p_world.z - rect.position.y) / rect.size.y * image->get_height());
	const float radius = float(radius_spin->get_value()) / rect.size.x * image->get_width();
	const Rect2i changed = apply_dab(image, channel_option->get_selected(), p_operation, px, radius, float(strength_spin->get_value()), float(value_spin->get_value()));
	if (changed.has_area()) {
		stroke_rect = stroke_rect.has_area() ? stroke_rect.merge(changed) : changed;
		_update_overlay(changed);
	}
}

// Hands the map to the node: a new Image or texture each time, so the node's setter sees a change and re-reads it.
void WorldMaskPaintPlugin::_store(const Ref<Image> &p_image, const Rect2i &p_pixels) {
	const int si = slot_option->get_selected();
	if (!node || si < 0 || si >= slots.size()) {
		return;
	}
	const Slot &slot = slots[si];
	if (slot.image) {
		node->set(slot.property, p_image->duplicate());
		const StringName notify = "notify_" + String(slot.property) + "_changed";
		if (node->has_method(notify)) {
			node->call(notify, Rect2());
		}
		return;
	}
	Rect2 area;
	if (const Rect2 r = slot_rect(node, slot); p_pixels.has_area() && r.has_area()) {
		const Vector2 texel = r.size / Vector2(p_image->get_size());
		area = Rect2(r.position + Vector2(p_pixels.position) * texel, Vector2(p_pixels.size) * texel);
	}
	Ref<ImageTexture> tex = node->get(slot.property);
	if (tex.is_valid() && tex->get_width() == p_image->get_width() && tex->get_height() == p_image->get_height() && tex->get_format() == p_image->get_format()) {
		// In place: every node sharing this map (biome fields each reading a channel) sees the paint.
		tex->update(p_image);
		dirty_textures.insert(tex->get_instance_id());
		_notify_users(tex, area);
		return;
	}
	Ref<ImageTexture> next = ImageTexture::create_from_image(p_image);
	if (tex.is_valid() && !tex->get_path().is_empty() && !tex->get_path().contains("::")) {
		next->set_path(tex->get_path(), true); // the painted map keeps its file
	} else if (Node *owner = node->get_owner() ? node->get_owner() : node; !owner->get_scene_file_path().is_empty()) {
		const String scene = owner->get_scene_file_path();
		next->set_path(scene.get_basename() + "_" + String(node->get_name()).to_snake_case() + "_" + String(slot.property) + ".res", true);
	}
	node->set(slot.property, Ref<Texture2D>());
	node->set(slot.property, next);
	dirty_textures.insert(next->get_instance_id());
	_notify_users(next, area);
}

// Tells each node of the edited scene that reads p_texture in a mask slot, and can refresh part of itself
// (notify_<slot>_changed, as VegetationField3D.notify_density_mask_changed), where it changed.
void WorldMaskPaintPlugin::_notify_users(const Ref<Texture2D> &p_texture, const Rect2 &p_area) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	LocalVector<Node *> stack;
	if (root) {
		stack.push_back(root);
	} else if (node) {
		stack.push_back(node);
	}
	while (!stack.is_empty()) {
		Node *n = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (const Slot &s : find_slots(n)) {
			if (s.image || Ref<Texture2D>(n->get(s.property)) != p_texture) {
				continue;
			}
			const StringName notify = "notify_" + String(s.property) + "_changed";
			if (n->has_method(notify)) {
				n->call(notify, p_area);
			}
		}
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
	}
}

void WorldMaskPaintPlugin::_commit(const String &p_action) {
	if (!stroke_rect.has_area() || stroke_before.is_null()) {
		_store(image, Rect2i());
		return;
	}
	const int si = slot_option->get_selected();
	const StringName prop = slots[si].property;
	const Ref<Image> before = stroke_before->get_region(stroke_rect);
	const Ref<Image> after = image->get_region(stroke_rect);
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
	ur->add_do_method(this, "_undo_blit", node, prop, after, stroke_rect.position);
	ur->add_undo_method(this, "_undo_blit", node, prop, before, stroke_rect.position);
	ur->commit_action(false);
	_store(image, stroke_rect);
	stroke_rect = Rect2i();
	stroke_before.unref();
}

void WorldMaskPaintPlugin::_undo_blit(Object *p_node, const StringName &p_property, const Ref<Image> &p_part, const Vector2i &p_at) {
	Node3D *n = Object::cast_to<Node3D>(p_node);
	ERR_FAIL_NULL(n);
	ERR_FAIL_COND(p_part.is_null());
	Ref<Image> map;
	const Variant cur = n->get(p_property);
	if (Ref<Image> im = cur; im.is_valid()) {
		map = im->duplicate();
	} else if (Ref<Texture2D> tex = cur; tex.is_valid()) {
		map = tex->get_image();
	}
	ERR_FAIL_COND(map.is_null());
	if (map->is_compressed()) {
		map->decompress();
	}
	map->convert(p_part->get_format());
	map->blit_rect(p_part, Rect2i(Point2i(), p_part->get_size()), p_at);
	Node3D *keep = node;
	node = n;
	for (int i = 0; i < slots.size(); i++) {
		if (slots[i].property == p_property) {
			slot_option->select(i);
		}
	}
	_store(map, Rect2i(p_at, p_part->get_size()));
	node = keep;
	if (paint_button->is_pressed() && n == node && _prepare()) {
		_update_overlay(Rect2i(Point2i(), image->get_size()));
	}
}

void WorldMaskPaintPlugin::_fill() {
	if (!node || !_prepare()) {
		return;
	}
	stroke_before = image->duplicate();
	const int ch = channel_option->get_selected();
	const int64_t n = int64_t(image->get_width()) * image->get_height();
	if (image->get_format() == Image::FORMAT_RF) {
		float *fp = reinterpret_cast<float *>(image->ptrw());
		for (int64_t i = 0; i < n; i++) {
			fp[i] = float(value_spin->get_value());
		}
	} else {
		const uint8_t v = uint8_t(CLAMP(float(value_spin->get_value()), 0.0f, 1.0f) * 255.0f);
		uint8_t *px = image->ptrw();
		for (int64_t i = 0; i < n; i++) {
			px[i * 4 + ch] = v;
		}
	}
	stroke_rect = Rect2i(Point2i(), image->get_size());
	_commit(TTR("Fill Mask"));
	if (paint_button->is_pressed()) {
		_update_overlay(Rect2i(Point2i(), image->get_size()));
	}
}

void WorldMaskPaintPlugin::save_external_data() {
	for (const ObjectID &id : dirty_textures) {
		Ref<Texture2D> tex = Object::cast_to<Texture2D>(ObjectDB::get_instance(id));
		if (tex.is_valid() && !tex->get_path().is_empty() && !tex->get_path().contains("::")) {
			ResourceSaver::save(tex, tex->get_path());
		}
	}
	dirty_textures.clear();
}

EditorPlugin::AfterGUIInput WorldMaskPaintPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!node || !paint_button->is_pressed() || image.is_null()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	auto op_for = [&](const Ref<InputEventWithModifiers> &p_mods) {
		if (p_mods->is_ctrl_pressed()) {
			return OP_SMOOTH;
		}
		if (p_mods->is_shift_pressed()) {
			return OP_SUBTRACT;
		}
		return Operation(operation_option->get_selected_id());
	};
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			stroking = true;
			stroke_before = image->duplicate();
			stroke_rect = Rect2i();
			Vector3 hit;
			if (_ground_hit(p_camera, mb->get_position(), hit)) {
				_dab(hit, op_for(mb));
			}
		} else if (stroking) {
			stroking = false;
			_commit(TTR("Paint Mask"));
		}
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && stroking) {
		Vector3 hit;
		if (_ground_hit(p_camera, mm->get_position(), hit)) {
			_dab(hit, op_for(mm));
		}
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

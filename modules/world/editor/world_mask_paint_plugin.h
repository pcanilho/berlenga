/**************************************************************************/
/*  world_mask_paint_plugin.h                                             */
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

#include "editor/plugins/editor_plugin.h"
#include "scene/3d/decal.h"
#include "scene/resources/image_texture.h"

class Button;
class HBoxContainer;
class OptionButton;
class SpinBox;

// Paints the world systems' mask maps in the 3D viewport (docs/ideas/editor-tools.md, B), after Terrain3D's paint
// tools and Unreal's landscape layer painting: select a node with a mask slot (VegetationField3D.density_mask,
// TerrainGenerator3D.uplift_map / sea_map, GroundSurface3D.type_map / mud_map, WeatherMap3D.painted_map and any
// other "<name>_map" or "<name>_mask" with a "<name>_rect", or a node with get_world_rect()), toggle "Paint Mask",
// pick the slot and channel, and paint on the ground. LMB applies the operation, Shift+LMB subtracts, Ctrl+LMB
// smooths. A decal shows the channel while painting. Each stroke is one undo step holding only the pixels it
// touched (Terrain3D's backup_region). A slot with no map gets one over its rect at the chosen texel size; a map of
// a saved scene is saved next to it as a resource when the scene is saved.
class WorldMaskPaintPlugin : public EditorPlugin {
	GDCLASS(WorldMaskPaintPlugin, EditorPlugin);

public:
	enum Operation {
		OP_ADD,
		OP_SUBTRACT,
		OP_REPLACE,
		OP_SMOOTH,
	};

	struct Slot {
		StringName property;
		bool image = false; // an Image property, else a Texture2D
		StringName rect_property; // empty: the node's get_world_rect()
		float fill = 0.0f; // the value a new map starts at
		bool height = false; // a height map in metres (FORMAT_RF, TerrainSculptLayer3D), else 0..1 channels (RGBA8)
	};

	// The mask slots p_node offers, in property order.
	static Vector<Slot> find_slots(const Object *p_node);
	// The world rect of a slot on p_node (x, z); empty when it has none.
	static Rect2 slot_rect(const Object *p_node, const Slot &p_slot);
	// Applies one dab of p_operation to p_image (RGBA8 on p_channel, or RF in metres), centred at pixel p_center with radius p_radius
	// pixels; returns the pixel rect it changed (empty when none). Shared with the tests.
	static Rect2i apply_dab(const Ref<Image> &p_image, int p_channel, Operation p_operation, const Vector2 &p_center, float p_radius, float p_strength, float p_value);

private:
	Node3D *node = nullptr;
	Vector<Slot> slots;
	HBoxContainer *toolbar = nullptr;
	Button *paint_button = nullptr;
	OptionButton *slot_option = nullptr;
	OptionButton *channel_option = nullptr;
	OptionButton *operation_option = nullptr;
	SpinBox *value_spin = nullptr;
	SpinBox *radius_spin = nullptr;
	SpinBox *strength_spin = nullptr;
	SpinBox *texel_spin = nullptr;

	Ref<Image> image; // the slot's map being painted, RGBA8
	Rect2 rect; // its world rect
	Ref<ImageTexture> overlay_texture;
	Ref<Image> overlay_image;
	Decal *overlay = nullptr;
	bool stroking = false;
	Ref<Image> stroke_before; // the map at the stroke's start
	Rect2i stroke_rect; // the pixels the stroke changed
	HashSet<ObjectID> dirty_textures; // painted textures to save with their scenes

	void _paint_toggled(bool p_on);
	void _slot_selected(int p_index);
	void _update_slots();
	bool _prepare();
	void _update_overlay(const Rect2i &p_rect);
	void _clear_overlay();
	void _fill();
	bool _ground_hit(Camera3D *p_camera, const Vector2 &p_screen, Vector3 &r_hit) const;
	float _ground_height(const Vector3 &p_position) const;
	void _dab(const Vector3 &p_world, Operation p_operation);
	void _commit(const String &p_action);
	void _store(const Ref<Image> &p_image, const Rect2i &p_pixels);
	void _notify_users(const Ref<Texture2D> &p_texture, const Rect2 &p_area);

protected:
	static void _bind_methods();
	void _undo_blit(Object *p_node, const StringName &p_property, const Ref<Image> &p_part, const Vector2i &p_at);

public:
	virtual String get_plugin_name() const override { return "WorldMaskPaint"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual void save_external_data() override;

	WorldMaskPaintPlugin();
};

VARIANT_ENUM_CAST(WorldMaskPaintPlugin::Operation);

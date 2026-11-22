/**************************************************************************/
/*  rendering_quality.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/type_info.h"
#include "core/variant/variant.h"

// One quality ladder for the whole engine (docs/ideas/quality-and-budgets.md), after Unreal Engine's
// scalability groups (sg.TextureQuality, sg.ShadowQuality, ...): an overall level sets every group, a group
// may then be moved alone, and each group level is a table of existing project settings. A feature says how
// it scales by registering its rows; a game's options menu maps onto the overall level and the groups.
//
// High is every setting as the project has it (the default level, so a project that never touches the ladder
// renders as before); Ultra goes above that (full meshes at every distance, no texture cap); Medium and Low
// trade detail for speed and memory. Rows only write settings the renderer reads at run time.
class RenderingQuality : public Object {
	GDCLASS(RenderingQuality, Object);

public:
	enum Level {
		LEVEL_LOW,
		LEVEL_MEDIUM,
		LEVEL_HIGH,
		LEVEL_ULTRA,
		LEVEL_MAX,
	};

	enum Group {
		GROUP_TEXTURES,
		GROUP_GEOMETRY,
		GROUP_SHADOWS,
		GROUP_PATH_TRACING,
		GROUP_REFLECTIONS,
		GROUP_VOLUMETRICS,
		GROUP_VEGETATION,
		GROUP_EFFECTS,
		GROUP_POST_PROCESS,
		GROUP_MAX,
	};

private:
	static RenderingQuality *singleton;

	struct Row {
		Group group = GROUP_TEXTURES;
		String setting;
		Variant values[LEVEL_MAX]; // NIL: the project's own value at that level.
	};
	LocalVector<Row> rows;
	HashMap<String, Variant> project_values; // Each row's setting as the project had it, before the ladder wrote it.

	Level level = LEVEL_HIGH;
	int group_levels[GROUP_MAX]; // -1: follows the overall level.
	bool applying = false;
	bool set_by_call = false; // A script set a level before the startup apply: keep it.
	// rendering/quality/* as last read: only a change to these (not the rows the ladder writes, nor a level
	// chosen by a script) re-applies the project's levels.
	int seen_level = -2;
	int seen_group_levels[GROUP_MAX];
	bool _read_project_levels(int &r_level, int *r_groups) const;

	void _register_engine_rows();
	void _apply_group(Group p_group);
	void _push_startup_settings(Group p_group);
	void _on_settings_changed();
	void _startup();

protected:
	static void _bind_methods();

public:
	static RenderingQuality *get_singleton() { return singleton; }

	static const char *get_group_name(Group p_group);

	void set_level(Level p_level);
	Level get_level() const { return level; }

	// p_level -1: the group follows the overall level again.
	void set_group_level(Group p_group, int p_level);
	int get_group_level(Group p_group) const;
	Level get_effective_group_level(Group p_group) const;

	// A setting that scales with p_group: p_values holds one value per level, Low to Ultra; null keeps the
	// project's own value at that level. Re-registering a setting replaces its row.
	void register_setting(Group p_group, const String &p_setting, const Array &p_values);
	void unregister_setting(const String &p_setting);
	Dictionary get_group_settings(Group p_group, Level p_level) const;

	// Reads rendering/quality/level and rendering/quality/groups/* and applies them (startup, and when the
	// project settings change).
	void apply_project_settings();

	RenderingQuality();
	~RenderingQuality();
};

VARIANT_ENUM_CAST(RenderingQuality::Level);
VARIANT_ENUM_CAST(RenderingQuality::Group);

/**************************************************************************/
/*  rendering_quality.cpp                                                 */
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

#include "rendering_quality.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

RenderingQuality *RenderingQuality::singleton = nullptr;

static const char *_group_names[RenderingQuality::GROUP_MAX] = {
	"textures",
	"geometry",
	"shadows",
	"path_tracing",
	"reflections",
	"volumetrics",
	"vegetation",
	"effects",
	"post_process",
};

const char *RenderingQuality::get_group_name(Group p_group) {
	ERR_FAIL_INDEX_V(p_group, GROUP_MAX, "");
	return _group_names[p_group];
}

// The engine's own rows. Low, Medium, High, Ultra; Variant() keeps the project's value (High always does).
void RenderingQuality::_register_engine_rows() {
	const Variant keep;
	auto row = [this](Group p_group, const char *p_setting, const Variant &p_low, const Variant &p_medium, const Variant &p_high, const Variant &p_ultra) {
		Row r;
		r.group = p_group;
		r.setting = p_setting;
		r.values[LEVEL_LOW] = p_low;
		r.values[LEVEL_MEDIUM] = p_medium;
		r.values[LEVEL_HIGH] = p_high;
		r.values[LEVEL_ULTRA] = p_ultra;
		rows.push_back(r);
	};

	// Textures: streamed textures capped at 1K and 2K, with a budget that only frees what nothing samples
	// (rendering/textures/streaming/keep_visible_ms). Ultra: no cap, as High.
	row(GROUP_TEXTURES, "rendering/textures/streaming/max_resolution", 1024, 2048, keep, keep);
	row(GROUP_TEXTURES, "rendering/textures/streaming/memory_budget_enabled", true, true, keep, keep);
	row(GROUP_TEXTURES, "rendering/textures/streaming/memory_budget_mb", 1536, 3072, keep, keep);

	// Geometry: the viewports' mesh LOD threshold scaled (raster and, with rendering/pathtracing/mesh_lod, the
	// path tracer, which picks the same level). Ultra draws every mesh whole at every distance.
	row(GROUP_GEOMETRY, "rendering/mesh_lod/lod_change/quality_scale", 4.0, 2.0, keep, 0.0);
	row(GROUP_GEOMETRY, "rendering/pathtracing/cull_min_size", 0.1, 0.05, keep, keep);

	// Shadows.
	row(GROUP_SHADOWS, "rendering/lights_and_shadows/directional_shadow/far_cascades_every_other_frame", true, keep, keep, keep);

	// Path tracing.
	row(GROUP_PATH_TRACING, "rendering/pathtracing/caustics", false, keep, keep, keep);
	row(GROUP_PATH_TRACING, "rendering/pathtracing/transmission_bounces", 2, 3, keep, keep);
	row(GROUP_PATH_TRACING, "rendering/pathtracing/max_transparency_layers", 2, 3, keep, keep);

	// Reflections: probes set to update always refresh fewer faces a frame.
	row(GROUP_REFLECTIONS, "rendering/reflections/reflection_probes/update_always_faces_per_frame", 1, 2, keep, keep);
}

void RenderingQuality::_apply_group(Group p_group) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return; // The editor keeps the project's settings as written; the ladder applies when the project runs.
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const Level group_level = get_effective_group_level(p_group);
	applying = true;
	for (const Row &r : rows) {
		if (r.group != p_group || !ps->has_setting(r.setting)) {
			continue;
		}
		if (!project_values.has(r.setting)) {
			project_values[r.setting] = ps->get_setting(r.setting);
		}
		const Variant &wanted = r.values[group_level].get_type() == Variant::NIL ? project_values[r.setting] : r.values[group_level];
		if (ps->get_setting(r.setting) != wanted) {
			ps->set_setting(r.setting, wanted);
		}
	}
	applying = false;
	emit_signal(SNAME("group_changed"), p_group, group_level);
}

void RenderingQuality::set_level(Level p_level) {
	ERR_FAIL_INDEX(p_level, LEVEL_MAX);
	set_by_call = true;
	level = p_level;
	for (int g = 0; g < GROUP_MAX; g++) {
		_apply_group(Group(g));
	}
	emit_signal(SNAME("level_changed"), level);
}

void RenderingQuality::set_group_level(Group p_group, int p_level) {
	ERR_FAIL_INDEX(p_group, GROUP_MAX);
	ERR_FAIL_COND(p_level < -1 || p_level >= LEVEL_MAX);
	set_by_call = true;
	group_levels[p_group] = p_level;
	_apply_group(p_group);
}

int RenderingQuality::get_group_level(Group p_group) const {
	ERR_FAIL_INDEX_V(p_group, GROUP_MAX, -1);
	return group_levels[p_group];
}

RenderingQuality::Level RenderingQuality::get_effective_group_level(Group p_group) const {
	ERR_FAIL_INDEX_V(p_group, GROUP_MAX, level);
	return group_levels[p_group] < 0 ? level : Level(group_levels[p_group]);
}

void RenderingQuality::register_setting(Group p_group, const String &p_setting, const Array &p_values) {
	ERR_FAIL_INDEX(p_group, GROUP_MAX);
	ERR_FAIL_COND_MSG(p_values.size() != LEVEL_MAX, "RenderingQuality.register_setting() takes one value per level: Low, Medium, High, Ultra (null keeps the project's value).");
	unregister_setting(p_setting);
	Row r;
	r.group = p_group;
	r.setting = p_setting;
	for (int i = 0; i < LEVEL_MAX; i++) {
		r.values[i] = p_values[i];
	}
	rows.push_back(r);
	_apply_group(p_group);
}

void RenderingQuality::unregister_setting(const String &p_setting) {
	for (uint32_t i = 0; i < rows.size(); i++) {
		if (rows[i].setting == p_setting) {
			if (project_values.has(p_setting) && !Engine::get_singleton()->is_editor_hint()) {
				applying = true;
				ProjectSettings::get_singleton()->set_setting(p_setting, project_values[p_setting]);
				applying = false;
				project_values.erase(p_setting);
			}
			rows.remove_at(i);
			return;
		}
	}
}

Dictionary RenderingQuality::get_group_settings(Group p_group, Level p_level) const {
	Dictionary d;
	ERR_FAIL_INDEX_V(p_group, GROUP_MAX, d);
	ERR_FAIL_INDEX_V(p_level, LEVEL_MAX, d);
	for (const Row &r : rows) {
		if (r.group == p_group) {
			const Variant *project = project_values.getptr(r.setting);
			d[r.setting] = r.values[p_level].get_type() != Variant::NIL ? r.values[p_level] : (project ? *project : ProjectSettings::get_singleton()->get_setting(r.setting));
		}
	}
	return d;
}

bool RenderingQuality::_read_project_levels(int &r_level, int *r_groups) const {
	bool changed = false;
	r_level = CLAMP(int(GLOBAL_GET("rendering/quality/level")), 0, LEVEL_MAX - 1);
	changed = r_level != seen_level;
	for (int g = 0; g < GROUP_MAX; g++) {
		r_groups[g] = CLAMP(int(GLOBAL_GET(vformat("rendering/quality/groups/%s", _group_names[g]))), -1, LEVEL_MAX - 1);
		changed = changed || r_groups[g] != seen_group_levels[g];
	}
	return changed;
}

void RenderingQuality::apply_project_settings() {
	int project_level = 0;
	_read_project_levels(project_level, seen_group_levels);
	seen_level = project_level;
	level = Level(project_level);
	for (int g = 0; g < GROUP_MAX; g++) {
		group_levels[g] = seen_group_levels[g];
	}
	for (int g = 0; g < GROUP_MAX; g++) {
		_apply_group(Group(g));
	}
	emit_signal(SNAME("level_changed"), level);
}

// Once every server and module has defined its settings: the project's levels, unless a script already chose
// some (then those again, now that every row's setting exists).
void RenderingQuality::_startup() {
	if (!set_by_call) {
		apply_project_settings();
		return;
	}
	int groups[GROUP_MAX];
	_read_project_levels(seen_level, groups);
	for (int g = 0; g < GROUP_MAX; g++) {
		seen_group_levels[g] = groups[g];
		_apply_group(Group(g));
	}
}

void RenderingQuality::_on_settings_changed() {
	if (applying || seen_level == -2) {
		return;
	}
	int project_level = 0;
	int groups[GROUP_MAX];
	if (_read_project_levels(project_level, groups)) {
		apply_project_settings();
	}
}

void RenderingQuality::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_level", "level"), &RenderingQuality::set_level);
	ClassDB::bind_method(D_METHOD("get_level"), &RenderingQuality::get_level);
	ClassDB::bind_method(D_METHOD("set_group_level", "group", "level"), &RenderingQuality::set_group_level);
	ClassDB::bind_method(D_METHOD("get_group_level", "group"), &RenderingQuality::get_group_level);
	ClassDB::bind_method(D_METHOD("get_effective_group_level", "group"), &RenderingQuality::get_effective_group_level);
	ClassDB::bind_method(D_METHOD("register_setting", "group", "setting", "values"), &RenderingQuality::register_setting);
	ClassDB::bind_method(D_METHOD("unregister_setting", "setting"), &RenderingQuality::unregister_setting);
	ClassDB::bind_method(D_METHOD("get_group_settings", "group", "level"), &RenderingQuality::get_group_settings);
	ClassDB::bind_method(D_METHOD("apply_project_settings"), &RenderingQuality::apply_project_settings);

	ADD_SIGNAL(MethodInfo("level_changed", PropertyInfo(Variant::INT, "level", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra")));
	ADD_SIGNAL(MethodInfo("group_changed", PropertyInfo(Variant::INT, "group"), PropertyInfo(Variant::INT, "level")));

	BIND_ENUM_CONSTANT(LEVEL_LOW);
	BIND_ENUM_CONSTANT(LEVEL_MEDIUM);
	BIND_ENUM_CONSTANT(LEVEL_HIGH);
	BIND_ENUM_CONSTANT(LEVEL_ULTRA);
	BIND_ENUM_CONSTANT(LEVEL_MAX);

	BIND_ENUM_CONSTANT(GROUP_TEXTURES);
	BIND_ENUM_CONSTANT(GROUP_GEOMETRY);
	BIND_ENUM_CONSTANT(GROUP_SHADOWS);
	BIND_ENUM_CONSTANT(GROUP_PATH_TRACING);
	BIND_ENUM_CONSTANT(GROUP_REFLECTIONS);
	BIND_ENUM_CONSTANT(GROUP_VOLUMETRICS);
	BIND_ENUM_CONSTANT(GROUP_VEGETATION);
	BIND_ENUM_CONSTANT(GROUP_EFFECTS);
	BIND_ENUM_CONSTANT(GROUP_POST_PROCESS);
	BIND_ENUM_CONSTANT(GROUP_MAX);
}

RenderingQuality::RenderingQuality() {
	singleton = this;
	for (int g = 0; g < GROUP_MAX; g++) {
		group_levels[g] = -1;
		seen_group_levels[g] = -1;
	}
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/quality/level", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra"), int(LEVEL_HIGH));
	for (int g = 0; g < GROUP_MAX; g++) {
		GLOBAL_DEF(PropertyInfo(Variant::INT, vformat("rendering/quality/groups/%s", _group_names[g]), PROPERTY_HINT_ENUM, "Follow Level:-1,Low:0,Medium:1,High:2,Ultra:3"), -1);
	}
	_register_engine_rows();
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &RenderingQuality::_on_settings_changed));
	// Once every server and module has defined the settings the rows write.
	callable_mp(this, &RenderingQuality::_startup).call_deferred();
}

RenderingQuality::~RenderingQuality() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

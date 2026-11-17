/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "cave_network_3d.h"
#include "rock_field_3d.h"
#include "rock_generator.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_rocks_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(RockGenerator);
	GDREGISTER_CLASS(RockField3D);
	GDREGISTER_CLASS(CaveNetwork3D);
	// The rock shader's ground blend reads the generator's geology map: declared so it compiles before a
	// TerrainGenerator3D publishes it (rect width 0 = none), whichever module registers first.
	// The contact map the terrain material reads (ground_contact), likewise.
	const char *globals[4][2] = { { "terrain_geology_map", "sampler2D" }, { "terrain_geology_rect", "vec4" }, { "rock_contact_map", "sampler2D" }, { "rock_contact_rect", "vec4" } };
	for (int i = 0; i < 4; i++) {
		const String setting = String("shader_globals/") + globals[i][0];
		if (ProjectSettings::get_singleton()->has_setting(setting)) {
			continue;
		}
		Dictionary d;
		d["type"] = globals[i][1];
		d["value"] = String(globals[i][1]) == "vec4" ? Variant(Vector4()) : Variant("");
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, setting, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), d);
	}
}

void uninitialize_rocks_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	RockGenerator::finish();
}

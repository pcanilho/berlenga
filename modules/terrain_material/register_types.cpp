/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "terrain_material_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_terrain_material_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(TerrainMaterialLayer);
	GDREGISTER_CLASS(TerrainMaterial3D);
	// The generator's geology globals, declared so the material compiles before a TerrainGenerator3D publishes them
	// (rect width 0 = no geology: the material falls back to slope and height rules).
	const struct {
		const char *name;
		const char *type;
	} globals[] = {
		{ "terrain_geology_map", "sampler2D" },
		{ "terrain_geology_rect", "vec4" },
		{ "terrain_detail_keep_map", "sampler2D" },
		// The generator's fall spray, and the plant cover colour the far ground blends to (vegetation publishes it).
		{ "terrain_spray_potential", "sampler2D" },
		{ "terrain_spray_rect", "vec4" },
		{ "terrain_cover_map", "sampler2D" },
		{ "terrain_cover_rect", "vec4" },
		// The last glaciers' flow and wear, for ice-worn rock in the detail normal (TerrainGenerator3D.ice_worn_detail).
		{ "terrain_ice_flow", "sampler2D" },
		{ "terrain_ice_flow_rect", "vec4" },
	};
	for (const auto &g : globals) {
		if (ProjectSettings::get_singleton()->has_setting(String("shader_globals/") + g.name)) {
			continue;
		}
		Dictionary d;
		d["type"] = g.type;
		d["value"] = String(g.type) == "vec4" ? Variant(Vector4()) : Variant("");
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, String("shader_globals/") + g.name, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), d);
	}
}

void uninitialize_terrain_material_module(ModuleInitializationLevel p_level) {
}

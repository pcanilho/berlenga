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
/**************************************************************************/

#include "register_types.h"

#include "climate_recipe.h"
#include "landscape_3d.h"
#include "terrain_sculpt_layer_3d.h"
#include "terrain_brush_3d.h"
#include "terrain_guide_3d.h"
#include "terrain_data_map_3d.h"
#include "terrain_deformer_3d.h"
#include "terrain_generator_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_terrain_deform_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(ClimateRecipe);
	GDREGISTER_CLASS(Landscape3D);
	GDREGISTER_CLASS(TerrainBrush3D);
	GDREGISTER_CLASS(TerrainSculptLayer3D);
	GDREGISTER_CLASS(TerrainGuide3D);
	GDREGISTER_CLASS(TerrainDeformer3D);
	GDREGISTER_CLASS(TerrainDataMap3D);
	GDREGISTER_CLASS(TerrainGenerator3D);
	// The world cache (world_cache.h): stage results kept on disk under a hash of their inputs.
	GLOBAL_DEF("world/cache/enabled", true);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "world/cache/path", PROPERTY_HINT_GLOBAL_DIR), "");
	// Landscape3D's stand map (worn trails, fresh rooting), declared so the terrain material compiles before it is
	// published (rect width 0 = none), whichever module registers first.
	const char *globals[4][2] = { { "terrain_stand_map", "sampler2D" }, { "terrain_stand_rect", "vec4" }, { "terrain_cliff_relief", "vec4" }, { "terrain_bank_relief", "vec4" } };
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

void uninitialize_terrain_deform_module(ModuleInitializationLevel p_level) {
}

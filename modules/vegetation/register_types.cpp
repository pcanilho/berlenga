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

#include "grass_field_3d.h"
#include "plant_species.h"
#include "tree_generator.h"
#include "vegetation_field_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_vegetation_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(PlantSpecies);
	GDREGISTER_CLASS(Biome);
	GDREGISTER_CLASS(VegetationField3D);
	GDREGISTER_CLASS(GrassField3D);
	GDREGISTER_CLASS(TreeGenerator);
	// Declared so terrain shaders using GrassField3D.get_ground_shader_code() compile without a field.
	const struct {
		const char *name;
		const char *type;
	} globals[] = {
		{ "grass_ground_map", "sampler2D" },
		{ "grass_ground_rect", "vec4" },
		{ "grass_ground_color", "vec4" },
	};
	for (const auto &g : globals) {
		Dictionary d;
		d["type"] = g.type;
		d["value"] = String(g.type) == "vec4" ? Variant(Vector4()) : Variant("");
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, String("shader_globals/") + g.name, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), d);
	}
}

void uninitialize_vegetation_module(ModuleInitializationLevel p_level) {
}

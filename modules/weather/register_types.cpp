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

#include "sky_clock.h"
#include "weather_lightning_3d.h"
#include "weather_map_3d.h"
#include "weather_sky_material.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_weather_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(WeatherMap3D);
	GDREGISTER_CLASS(WeatherSkyMaterial);
	GDREGISTER_CLASS(WeatherLightning3D);
	GDREGISTER_CLASS(SkyClock);

	// A shader that names a global uniform only compiles once the global exists, so the ones
	// WeatherMap3D.get_wind_shader_code() reads are declared here with no data, as BaseMaterial3D
	// declares weather_state_map and weather_map_rect: a wind shader compiles before a
	// WeatherMap3D publishes, or with none (rect width 0: no wind). A project's own declaration wins.
	const struct {
		const char *name;
		const char *type;
	} globals[] = {
		{ "weather_wind_map", "sampler2D" },
		{ "weather_gusts", "sampler2D" },
		{ "weather_shelter_map", "sampler2D" },
		{ "weather_wind_info", "vec4" },
		{ "weather_shelter_rect", "vec4" },
		{ "weather_patch_info", "vec4" },
		{ "weather_ground_height", "float" },
	};
	for (const auto &g : globals) {
		Dictionary d;
		d["type"] = g.type;
		if (String(g.type) == "vec4") {
			d["value"] = Vector4();
		} else if (String(g.type) == "float") {
			d["value"] = 0.0;
		} else {
			d["value"] = "";
		}
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, String("shader_globals/") + g.name, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), d);
	}
}

void uninitialize_weather_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	WeatherSkyMaterial::cleanup();
}

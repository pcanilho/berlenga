/**************************************************************************/
/*  weather_sky_material.h                                                */
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

#include "weather_clouds.h"

#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

// Godot's physical sky with the weather map's clouds raymarched through it (Nubis, Frostbite; see
// docs/ideas/weather.md, step 3). The clouds are drawn in the half resolution pass, with fewer steps
// for the radiance map. A WeatherMap3D in the scene pushes its cloud state here; without one the
// sky shows the defaults. Path traced camera rays that miss see this shader, bounce rays its
// radiance map, so the clouds appear under path tracing as well.
class WeatherSkyMaterial : public ShaderMaterial {
	GDCLASS(WeatherSkyMaterial, ShaderMaterial);

public:
	enum AtmosphereModel {
		ATMOSPHERE_PHYSICAL_SKY, // PhysicalSkyMaterial's (Preetham-style), daylight only
		ATMOSPHERE_SCATTERING, // Hillaire 2020: any sun angle, twilight, two lights (atmosphere.h)
	};

private:
	// Reprojection on/off x atmosphere model.
	static Ref<Shader> shared_shaders[4];
	static Ref<ImageTexture> atmo_transmittance_tex;
	static Ref<ImageTexture> atmo_multiscatter_tex;

	bool cloud_reprojection = true;
	AtmosphereModel atmosphere_model = ATMOSPHERE_SCATTERING;
	void _update_shader();
	void _update_atmosphere_textures();

protected:
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();

public:
	// Sets the cloud uniforms straight on the material (not saved with it).
	void set_cloud_state(const WeatherClouds::State &p_state);

	// Clouds drawn a sixteenth of the pixels a frame at full resolution, the rest reprojected
	// (render_mode half_res_reprojection); off: every half resolution pixel every frame.
	void set_cloud_reprojection(bool p_enabled);
	bool is_cloud_reprojection_enabled() const;

	void set_atmosphere_model(AtmosphereModel p_model);
	AtmosphereModel get_atmosphere_model() const;
	// The sun and the moon above the air (directions toward them, irradiance in light energy units), from
	// SkyClock; until set, the first directional light stands for the sun above the air.
	void set_atmosphere_lights(const Vector3 &p_sun_direction, const Vector3 &p_sun_irradiance, const Vector3 &p_moon_direction, const Vector3 &p_moon_irradiance);

	// Lightning glowing in the clouds: up to four flashes, each a world position and its energy now.
	void set_lightning(const Vector4 *p_flashes, int p_count, const Color &p_color);

	// The sky material and environment a node's camera (or its world) draws with.
	static Ref<WeatherSkyMaterial> find_for(Node *p_node);
	static Ref<Environment> find_environment(Node *p_node);

	static void cleanup();

	WeatherSkyMaterial();
};

VARIANT_ENUM_CAST(WeatherSkyMaterial::AtmosphereModel);

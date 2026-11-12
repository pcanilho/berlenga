/**************************************************************************/
/*  water_raster_reflections.cpp                                          */
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

#include "water_raster_reflections.h"

#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

DirectionalLight3D *WaterRasterReflections::find_sun(Node3D *p_node) {
	DirectionalLight3D *s = ObjectDB::get_instance<DirectionalLight3D>(sun);
	if ((s == nullptr || !s->is_inside_tree()) && sun_search-- <= 0) {
		sun_search = 60;
		sun = ObjectID();
		s = nullptr;
		TypedArray<Node> lights = p_node->get_tree()->get_root()->find_children("*", "DirectionalLight3D", true, false);
		for (int i = 0; i < lights.size(); i++) {
			DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(lights[i]);
			if (l != nullptr && l->is_visible_in_tree() && l->get_sky_mode() != DirectionalLight3D::SKY_MODE_SKY_ONLY) {
				sun = l->get_instance_id();
				s = l;
				break;
			}
		}
	}
	return s != nullptr && s->is_inside_tree() ? s : nullptr;
}

void WaterRasterReflections::update(Node3D *p_node, const Ref<ShaderMaterial> &p_material, bool p_enabled) {
	if (p_material.is_null() || p_node == nullptr || !p_node->is_inside_tree()) {
		return;
	}
	p_material->set_shader_parameter("raster_reflections", p_enabled);
	if (!p_enabled) {
		return;
	}
	DirectionalLight3D *s = find_sun(p_node);
	Vector3 travel(0, -1, 0);
	Vector3 irradiance;
	if (s != nullptr && s->is_visible_in_tree()) {
		travel = -s->get_global_transform().basis.get_column(2).normalized();
		// Raster lights are pi x energy in irradiance.
		const Color c = s->get_color().srgb_to_linear() * s->get_param(Light3D::PARAM_ENERGY) * Math::PI;
		irradiance = Vector3(c.r, c.g, c.b);
	}
	p_material->set_shader_parameter("raster_sun_travel", travel);
	p_material->set_shader_parameter("raster_sun_irradiance", irradiance);

	const Viewport *vp = p_node->get_viewport();
	const Camera3D *cam = vp != nullptr ? vp->get_camera_3d() : nullptr;
	Ref<Environment> env = cam != nullptr ? cam->get_environment() : Ref<Environment>();
	if (env.is_null() && p_node->get_world_3d().is_valid()) {
		env = p_node->get_world_3d()->get_environment();
	}
	float energy = 0.0f;
	if (env.is_valid() && env->get_background() == Environment::BG_SKY && env->get_sky().is_valid()) {
		const RID sky_rid = env->get_sky()->get_rid();
		// Again every few seconds (a sky that changes with the time of day), and until the sky has been drawn
		// once (before that the bake comes back black).
		if (sky_rid != sky_baked || --sky_rebake <= 0) {
			sky_baked = sky_rid;
			Ref<Image> pano = RS::get_singleton()->sky_bake_panorama(sky_rid, 1.0, false, Size2i(128, 64));
			bool lit = false;
			if (pano.is_valid() && !pano->is_empty()) {
				lit = pano->get_pixel(pano->get_width() / 2, pano->get_height() / 4).get_luminance() > 0.0f;
				if (sky_texture.is_valid() && sky_texture->get_size() == pano->get_size() && sky_texture->get_format() == pano->get_format()) {
					sky_texture->update(pano);
				} else {
					sky_texture = ImageTexture::create_from_image(pano);
				}
				p_material->set_shader_parameter("raster_sky_panorama", sky_texture);
				// Irradiance on a level surface: the upper half of the panorama (rows from the zenith),
				// each texel's radiance times cos(theta) and its solid angle.
				const int w = pano->get_width();
				const int h = pano->get_height();
				const double d_theta = Math::PI / h;
				const double d_phi = Math::TAU / w;
				Vector3 e;
				for (int j = 0; j < h / 2; j++) {
					const double theta = (j + 0.5) * d_theta;
					const double weight = Math::cos(theta) * Math::sin(theta) * d_theta * d_phi;
					Color row;
					for (int i = 0; i < w; i++) {
						row += pano->get_pixel(i, j);
					}
					e += Vector3(row.r, row.g, row.b) * weight;
				}
				sky_irradiance = e;
			}
			sky_rebake = lit ? 240 : 10;
			sky_energy = lit ? 1.0f : 0.0f;
		}
		energy = sky_energy * env->get_bg_energy_multiplier();
	}
	p_material->set_shader_parameter("raster_sky_energy", energy);
	p_material->set_shader_parameter("raster_sky_irradiance", sky_irradiance);
}

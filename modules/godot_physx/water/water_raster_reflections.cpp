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

#include "core/object/callable_mp.h"
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

void WaterSkyReadback::request(RID p_sky, bool p_bake_irradiance, const Size2i &p_size) {
	pending = true;
	RS::get_singleton()->sky_bake_panorama_async(p_sky, 1.0, p_bake_irradiance, p_size, callable_mp(this, &WaterSkyReadback::_ready));
}

void WaterSkyReadback::_ready(const Ref<Image> &p_image) {
	pending = false;
	landed = true;
	image = p_image;
}

bool WaterSkyReadback::take(Ref<Image> &r_image) {
	if (!landed) {
		return false;
	}
	landed = false;
	r_image = image;
	image.unref();
	return true;
}

// The brightest shown directional light: the sun by day, the moon at night (a sky clock hides the body below the
// horizon, so the first light found, the sun, stayed chosen all night and the water lost the moon's glitter path and
// its light in the water). Searched again every second, and four times as often while the chosen one is hidden.
DirectionalLight3D *WaterRasterReflections::find_sun(Node3D *p_node) {
	DirectionalLight3D *s = ObjectDB::get_instance<DirectionalLight3D>(sun);
	const bool stale = s == nullptr || !s->is_inside_tree() || !s->is_visible_in_tree();
	if (sun_search-- <= 0) {
		sun_search = stale ? 15 : 60;
		sun = ObjectID();
		s = nullptr;
		float best = -1.0f;
		TypedArray<Node> lights = p_node->get_tree()->get_root()->find_children("*", "DirectionalLight3D", true, false);
		for (int i = 0; i < lights.size(); i++) {
			DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(lights[i]);
			if (l == nullptr || !l->is_visible_in_tree() || l->get_sky_mode() == DirectionalLight3D::SKY_MODE_SKY_ONLY) {
				continue;
			}
			const Color c = l->get_color().srgb_to_linear();
			const float e = l->get_param(Light3D::PARAM_ENERGY) * MAX(MAX(c.r, c.g), c.b);
			if (e > best) {
				best = e;
				sun = l->get_instance_id();
				s = l;
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
		p_material->set_shader_parameter("raster_lamp_count", 0);
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
		if (sky_readback.is_null()) {
			sky_readback.instantiate();
		}
		if ((sky_rid != sky_baked || --sky_rebake <= 0) && !sky_readback->pending) {
			sky_baked = sky_rid;
			sky_readback->request(sky_rid, false, Size2i(128, 64));
		}
		Ref<Image> pano;
		if (sky_readback->take(pano)) {
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
	_update_lamps(p_node, cam, p_material);
}

// The omni and spot lights whose light reaches nearest the camera (at most four, looked up again about twice a
// second), for the clear water's lamp beams and bed (water_lamp_beams, water_lamp_bed). Their values every frame,
// so a moving torch or a swinging spot follows at once.
void WaterRasterReflections::_update_lamps(Node3D *p_node, const Camera3D *p_camera, const Ref<ShaderMaterial> &p_material) {
	if (p_camera == nullptr) {
		p_material->set_shader_parameter("raster_lamp_count", 0);
		return;
	}
	const Vector3 cp = p_camera->get_global_position();
	if (lamp_scan-- <= 0) {
		lamp_scan = 30;
		lamps.clear();
		struct Found {
			ObjectID id;
			float dist;
			bool operator<(const Found &p_other) const { return dist < p_other.dist; }
		};
		LocalVector<Found> found;
		for (const char *type : { "OmniLight3D", "SpotLight3D" }) {
			TypedArray<Node> lights = p_node->get_tree()->get_root()->find_children("*", type, true, false);
			for (int i = 0; i < lights.size(); i++) {
				Light3D *l = Object::cast_to<Light3D>(lights[i]);
				if (l == nullptr || !l->is_visible_in_tree() || l->get_param(Light3D::PARAM_ENERGY) <= 0.0f) {
					continue;
				}
				// How near the camera its lit sphere comes; a lamp far beyond the water seen nearby is left out.
				const float reach = l->get_global_position().distance_to(cp) - l->get_param(Light3D::PARAM_RANGE);
				if (reach < 150.0f) {
					found.push_back({ l->get_instance_id(), reach });
				}
			}
		}
		found.sort();
		for (uint32_t i = 0; i < MIN(found.size(), 4u); i++) {
			lamps.push_back(found[i].id);
		}
	}
	PackedVector4Array pos;
	PackedVector4Array dir;
	PackedVector4Array color;
	PackedFloat32Array rim;
	pos.resize(4);
	dir.resize(4);
	color.resize(4);
	rim.resize(4);
	int count = 0;
	for (const ObjectID &id : lamps) {
		Light3D *light = ObjectDB::get_instance<Light3D>(id);
		if (light == nullptr || !light->is_inside_tree() || !light->is_visible_in_tree() || count >= 4) {
			continue;
		}
		const Transform3D xf = light->get_global_transform();
		pos.set(count, Vector4(xf.origin.x, xf.origin.y, xf.origin.z, MAX(light->get_param(Light3D::PARAM_RANGE), 1e-3f)));
		const Vector3 travel = -xf.basis.get_column(2).normalized();
		const bool spot = Object::cast_to<SpotLight3D>(light) != nullptr;
		const float cutoff = spot ? Math::cos(Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE))) : -2.0f;
		dir.set(count, Vector4(travel.x, travel.y, travel.z, cutoff));
		// Raster lights are pi x energy in irradiance at unit distance.
		const Color c = light->get_color().srgb_to_linear() * light->get_param(Light3D::PARAM_ENERGY) * Math::PI;
		color.set(count, Vector4(c.r, c.g, c.b, light->get_param(Light3D::PARAM_ATTENUATION)));
		rim.set(count, spot ? light->get_param(Light3D::PARAM_SPOT_ATTENUATION) : 1.0f);
		count++;
	}
	p_material->set_shader_parameter("raster_lamp_count", count);
	p_material->set_shader_parameter("raster_lamp_pos", pos);
	p_material->set_shader_parameter("raster_lamp_dir", dir);
	p_material->set_shader_parameter("raster_lamp_color", color);
	p_material->set_shader_parameter("raster_lamp_rim", rim);
}

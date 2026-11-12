/**************************************************************************/
/*  weather_haze.cpp                                                      */
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

// Haze from falling and blowing precipitation (docs/ideas/weather.md, "Precipitation haze"). The
// sources' extinction is split the way Unreal splits its fog: a uniform part into the environment's
// exponential fog, coloured by the sky (aerial perspective), for the distance; and each layer near
// the ground into a FogVolume, which the froxel volumetric fog lights with the sun, its shadows and
// the ambient light (Wronski 2014). The path tracer reads both: the exponential fog on every
// segment, the froxels on camera segments.

#include "weather_map_3d.h"

#include "weather_sky_material.h"

#include "core/config/engine.h"
#include "scene/3d/fog_volume.h"
#include "scene/resources/3d/fog_material.h"
#include "scene/resources/environment.h"

float WeatherMap3D::_haze_weight(const HazeSource &p_source, const Vector3 &p_position) const {
	if (p_source.radius <= 0.0f) {
		return 1.0f;
	}
	const float d = Vector2(p_position.x - p_source.centre.x, p_position.z - p_source.centre.z).length();
	return 1.0f - Math::smoothstep(0.7f * p_source.radius, p_source.radius, d);
}

void WeatherMap3D::set_precipitation_extinction(Object *p_source, float p_sigma, float p_height_scale, float p_albedo, Vector3 p_centre, float p_radius) {
	ERR_FAIL_NULL(p_source);
	const ObjectID id = p_source->get_instance_id();
	HazeSource *s = nullptr;
	for (HazeSource &h : haze_sources) {
		if (h.source == id) {
			s = &h;
		}
	}
	if (!s) {
		haze_sources.push_back(HazeSource());
		s = &haze_sources[haze_sources.size() - 1];
		s->source = id;
	}
	s->sigma = MAX(p_sigma, 0.0f);
	s->height_scale = p_height_scale;
	s->albedo = CLAMP(p_albedo, 0.0f, 1.0f);
	s->centre = p_centre;
	s->radius = p_radius;
	applied_haze = -1.0f;
	if (is_inside_tree()) {
		_apply_haze();
	}
}

void WeatherMap3D::clear_precipitation_extinction(Object *p_source) {
	ERR_FAIL_NULL(p_source);
	const ObjectID id = p_source->get_instance_id();
	for (uint32_t i = 0; i < haze_sources.size(); i++) {
		if (haze_sources[i].source == id) {
			if (haze_sources[i].volume) {
				haze_sources[i].volume->queue_free();
			}
			haze_sources.remove_at(i);
			break;
		}
	}
	applied_haze = -1.0f;
	if (is_inside_tree()) {
		_apply_haze();
	}
}

// Where a layer's density starts to fall off: a source with an area (radius > 0) sets it with
// its centre's height (the ground under it, which follows the terrain), otherwise ground_height.
float WeatherMap3D::_haze_base(const HazeSource &p_source) const {
	return p_source.radius > 0.0f ? p_source.centre.y : ground_height;
}

float WeatherMap3D::get_precipitation_extinction_at(Vector3 p_position) const {
	float sigma = 0.0f;
	for (const HazeSource &h : haze_sources) {
		float s = h.sigma * _haze_weight(h, p_position);
		if (h.height_scale > 0.0f) {
			s *= Math::exp(-MAX(p_position.y - _haze_base(h), 0.0f) / h.height_scale);
		}
		sigma += s;
	}
	return sigma;
}

void WeatherMap3D::set_drive_environment_fog(bool p_enabled) {
	if (drive_environment_fog == p_enabled) {
		return;
	}
	drive_environment_fog = p_enabled;
	if (!p_enabled) {
		_restore_fog();
	}
	applied_haze = -1.0f;
}

void WeatherMap3D::_restore_fog() {
	for (HazeSource &h : haze_sources) {
		if (h.volume) {
			h.volume->queue_free();
			h.volume = nullptr;
		}
	}
	if (fog_environment.is_valid() && fog_base.saved) {
		fog_environment->set_fog_enabled(fog_base.fog_enabled);
		fog_environment->set_fog_density(fog_base.fog_density);
		fog_environment->set_fog_aerial_perspective(fog_base.aerial_perspective);
		fog_environment->set_volumetric_fog_enabled(fog_base.volumetric_enabled);
		fog_environment->set_volumetric_fog_density(fog_base.volumetric_density);
		fog_environment->set_volumetric_fog_length(fog_base.volumetric_length);
		fog_environment->set_volumetric_fog_ambient_inject(fog_base.volumetric_ambient_inject);
	}
	fog_base.saved = false;
	fog_environment.unref();
	applied_haze = -1.0f;
}

void WeatherMap3D::_apply_haze() {
	if (!drive_environment_fog || Engine::get_singleton()->is_editor_hint() || active != this) {
		return;
	}
	// Drop sources that were freed without clearing.
	for (int i = int(haze_sources.size()) - 1; i >= 0; i--) {
		if (!ObjectDB::get_instance(haze_sources[i].source)) {
			if (haze_sources[i].volume) {
				haze_sources[i].volume->queue_free();
			}
			haze_sources.remove_at(i);
			applied_haze = -1.0f;
		}
	}
	if (haze_sources.is_empty()) {
		if (fog_base.saved) {
			_restore_fog();
		}
		return;
	}
	Ref<Environment> env = WeatherSkyMaterial::find_environment(this);
	if (env.is_null()) {
		return;
	}
	if (env != fog_environment) {
		_restore_fog();
		fog_environment = env;
	}
	if (!fog_base.saved) {
		fog_base.fog_enabled = env->is_fog_enabled();
		fog_base.fog_density = env->get_fog_density();
		fog_base.aerial_perspective = env->get_fog_aerial_perspective();
		fog_base.volumetric_enabled = env->is_volumetric_fog_enabled();
		fog_base.volumetric_density = env->get_volumetric_fog_density();
		fog_base.volumetric_length = env->get_volumetric_fog_length();
		fog_base.volumetric_ambient_inject = env->get_volumetric_fog_ambient_inject();
		fog_base.saved = true;
	}

	// The uniform part at the camera, for the distance.
	const Vector3 at = _follow_position();
	float uniform = 0.0f;
	bool layers = false;
	for (const HazeSource &h : haze_sources) {
		if (h.height_scale > 0.0f) {
			layers = layers || h.sigma > 0.0f;
		} else {
			uniform += h.sigma * _haze_weight(h, at);
		}
	}
	if (Math::abs(uniform - applied_haze) > 1e-6f) {
		// The environment's own density only counts when its fog was on (Environment's default
		// density, 0.01 /m, is there whether or not fog is).
		const float base = fog_base.fog_enabled ? fog_base.fog_density : 0.0f;
		const float total = base + uniform;
		env->set_fog_enabled(fog_base.fog_enabled || uniform > 0.0f);
		env->set_fog_density(total);
		// Precipitation scatters the sky's light back (albedo near 1): the fog takes the sky's colour
		// for its share of the extinction.
		const float share = total > 0.0f ? uniform / total : 0.0f;
		env->set_fog_aerial_perspective(fog_base.fog_enabled ? Math::lerp(fog_base.aerial_perspective, 1.0f, share) : 1.0f);
		applied_haze = uniform;
	}
	env->set_volumetric_fog_enabled(fog_base.volumetric_enabled || layers);
	// Turned on only for the layers: the environment's own volumetric density (0.05 /m by default)
	// would fill the air everywhere, so it stays at zero.
	// Its 64 m default reach would also cut the layer off a little way from the camera; 300 m covers
	// a raised camera looking down on the ground.
	if (!fog_base.volumetric_enabled) {
		env->set_volumetric_fog_density(0.0f);
		env->set_volumetric_fog_length(MAX(fog_base.volumetric_length, 300.0f));
	}
	// Falling and blowing snow is lit mostly by the sky (single-scatter albedo about 0.97), so the
	// layers take the ambient light in full unless the project set its own share.
	if (fog_base.volumetric_ambient_inject <= 0.0f) {
		env->set_volumetric_fog_ambient_inject(layers ? 1.0f : 0.0f);
	}

	// Each layer near the ground: a FogVolume over its area (or the map), density falling as
	// exp(-(y - ground) / H). FogMaterial falls as exp2(-falloff y), so falloff = 1 / (H ln 2).
	for (HazeSource &h : haze_sources) {
		if (h.height_scale <= 0.0f || h.sigma <= 0.0f) {
			if (h.volume) {
				h.volume->queue_free();
				h.volume = nullptr;
			}
			continue;
		}
		if (!h.volume) {
			h.material.instantiate();
			h.volume = memnew(FogVolume);
			h.volume->set_shape(RSE::FOG_VOLUME_SHAPE_BOX);
			h.volume->set_material(h.material);
			h.volume->set_as_top_level(true);
			add_child(h.volume, false, INTERNAL_MODE_BACK);
		}
		h.material->set_density(h.sigma);
		h.material->set_albedo(Color(h.albedo, h.albedo, h.albedo));
		h.material->set_height_falloff(1.0f / (h.height_scale * Math::LN2));
		h.material->set_edge_fade(0.25f);
		const float width = h.radius > 0.0f ? 2.0f * h.radius : extent;
		// The box is centred on its origin, which sets the fall-off's base (_haze_base()), so it
		// reaches 8 H above the ground and as far below, where nothing is seen.
		const float height = 16.0f * h.height_scale;
		const Vector3 centre = h.radius > 0.0f ? h.centre : at;
		h.volume->set_size(Vector3(width, height, width));
		h.volume->set_global_position(Vector3(centre.x, _haze_base(h), centre.z));
	}
}

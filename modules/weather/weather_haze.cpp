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
#include "scene/resources/image_texture.h"

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

// The aerosol as a traced medium for the path tracer (render_raytracing.cpp reads these globals): extinction at the
// base, albedo, scale height and base height, and how far rays that reach the sky carry it (the froxels' reach,
// where the sky's own aerosol starts). Extinction 0: none.
void WeatherMap3D::_apply_traced_haze(const Vector4 &p_medium, float p_reach) {
	if (p_medium == applied_traced_haze && p_reach == applied_traced_reach) {
		return;
	}
	applied_traced_haze = p_medium;
	applied_traced_reach = p_reach;
	_set_global("weather_haze_medium", p_medium, RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("weather_haze_reach", p_reach, RSE::GLOBAL_VAR_TYPE_FLOAT);
}

void WeatherMap3D::set_traced_haze(bool p_enabled) {
	traced_haze = p_enabled;
	applied_haze = -1.0f;
}

void WeatherMap3D::_restore_fog() {
	if (applied_traced_haze.x > 0.0f) {
		_apply_traced_haze(Vector4(), 0.0f);
	}
	for (HazeSource &h : haze_sources) {
		if (h.volume) {
			h.volume->queue_free();
			h.volume = nullptr;
		}
	}
	_apply_valley_fog(false);
	if (fog_environment.is_valid() && fog_base.saved) {
		fog_environment->set_fog_enabled(fog_base.fog_enabled);
		fog_environment->set_fog_density(fog_base.fog_density);
		fog_environment->set_fog_aerial_perspective(fog_base.aerial_perspective);
		fog_environment->set_volumetric_fog_enabled(fog_base.volumetric_enabled);
		fog_environment->set_volumetric_fog_density(fog_base.volumetric_density);
		fog_environment->set_volumetric_fog_length(fog_base.volumetric_length);
		fog_environment->set_volumetric_fog_ambient_inject(fog_base.volumetric_ambient_inject);
		fog_environment->set_volumetric_fog_anisotropy(fog_base.volumetric_anisotropy);
	}
	fog_base.saved = false;
	fog_environment.unref();
	applied_haze = -1.0f;
	_apply_sky_aerosol(0.0f, 0.0f);
}

void WeatherMap3D::_apply_sky_aerosol(float p_extinction, float p_from) {
	const Vector2 state(p_extinction, p_from);
	if (state == applied_sky_aerosol) {
		return;
	}
	Ref<WeatherSkyMaterial> sky_material = WeatherSkyMaterial::find_for(this);
	if (sky_material.is_null()) {
		return;
	}
	// The fires' smoke share of it: the sky takes the sun through the smoke by colour (SkyClock), not grey.
	sky_material->set_aerosol(p_extinction, 0.9f, haze_height, p_from, p_extinction > 0.0f ? MIN(_smoke_sigma() / p_extinction, 1.0f) : 0.0f);
	applied_sky_aerosol = state;
}

void WeatherMap3D::_apply_haze() {
	if (!drive_environment_fog || Engine::get_singleton()->is_editor_hint() || active != this) {
		return;
	}
	_update_aerosol();
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
	const bool valley_active = valley_fog && valley_fog_max > 1.0f && valley_fog_density > 0.0f && valley_fog_texture.is_valid();
	if (haze_sources.is_empty() && !valley_active) {
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
		fog_base.volumetric_anisotropy = env->get_volumetric_fog_anisotropy();
		fog_base.saved = true;
	}

	// The uniform part at the camera, for the distance.
	const Vector3 at = _follow_position();
	float uniform = 0.0f;
	bool layers = valley_active;
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
	// The aerosol haze is thin (a few 1e-4 /m): its shafts build up over a kilometre or more of air, so the froxels
	// reach 1.5 km while it is on (their depth slices still crowd near the camera).
	if (!fog_base.volumetric_enabled) {
		env->set_volumetric_fog_density(0.0f);
		env->set_volumetric_fog_length(MAX(fog_base.volumetric_length, aerosol_haze ? 1500.0f : 300.0f));
		// Aerosol scatters strongly forward (Henyey-Greenstein g about 0.6 to 0.7 for continental haze,
		// d'Almeida et al. 1991), which is what makes the air glow and the shafts show toward the sun.
		// Falling rain and snow are as forward or more, so the layers share it.
		env->set_volumetric_fog_anisotropy(aerosol_haze ? 0.6f : fog_base.volumetric_anisotropy);
	}
	// The aerosol past the froxels' reach, in the sky's air (its shafts across the sky, its haze at the horizon).
	_apply_sky_aerosol(aerosol_haze && applied_aerosol > 0.0f ? applied_aerosol : 0.0f, env->is_volumetric_fog_enabled() ? env->get_volumetric_fog_length() : 0.0f);
	// Falling and blowing snow is lit mostly by the sky (single-scatter albedo about 0.97), so the
	// layers take the ambient light in full unless the project set its own share.
	if (fog_base.volumetric_ambient_inject <= 0.0f) {
		env->set_volumetric_fog_ambient_inject(layers ? 1.0f : 0.0f);
	}

	// Each layer near the ground: a FogVolume over its area (or the map), density falling as
	// exp(-(y - ground) / H). FogMaterial falls as exp2(-falloff y), so falloff = 1 / (H ln 2).
	// Under path tracing the aerosol (this map's own source) is traced as a medium instead: its volume is hidden
	// and the path tracer draws free flights through it (pixel-sharp shafts, haze on every bounce).
	const bool traced = traced_haze && env->is_pathtracing_enabled();
	Vector4 traced_medium;
	for (HazeSource &h : haze_sources) {
		if (traced && h.source == get_instance_id() && h.height_scale > 0.0f && h.sigma > 0.0f) {
			traced_medium = Vector4(h.sigma, h.albedo, h.height_scale, _haze_base(h));
			if (h.volume) {
				h.volume->queue_free();
				h.volume = nullptr;
			}
			continue;
		}
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
	_apply_traced_haze(traced_medium, traced_medium.x > 0.0f && env->is_volumetric_fog_enabled() ? env->get_volumetric_fog_length() : 0.0f);
	_apply_valley_fog(valley_active);
}

// Valley fog (docs/ideas/world-links.md, "Valley fog"): radiation fog fills a cold pool once its air cools to the dew
// point, from the floor up to where the pooled air is still saturated, and deepens over an hour or so; after sunrise
// or in a wind the pool mixes out and the fog lifts and goes (Whiteman 2000; Gultepe et al. 2007; Price 2011). Seen
// from the ridges it is a flat-topped sea filling the valleys.
void WeatherMap3D::_update_valley_fog(float p_dt_h) {
	const int n = resolution;
	const bool ready = valley_fog && cold_pools && cell_height.size() == n * n && cell_pool.size() == n * n && cell_pool_depth.size() == n * n;
	if (!ready) {
		cell_valley_fog.clear();
		valley_fog_max = 0.0f;
		return;
	}
	if (cell_valley_fog.size() != n * n) {
		cell_valley_fog.resize(n * n);
		cell_valley_fog.fill(0.0f);
	}
	const float dew = get_air_dew_point();
	const float form = 1.0f - Math::exp(-MAX(p_dt_h, 0.0f) / 1.0f);
	const float lift = 1.0f - Math::exp(-MAX(p_dt_h, 0.0f) * (1.0f + cur_wind_speed / 3.0f) / 0.5f);
	float *f = cell_valley_fog.ptrw();
	const float *h = cell_height.ptr();
	const float *pl = cell_pool.ptr();
	const float *pd = cell_pool_depth.ptr();
	float deepest = 0.0f;
	Vector2 span(Math::INF, -Math::INF);
	if (valley_fog_image.is_null() || valley_fog_image->get_width() != n) {
		valley_fog_image = Image::create_empty(n, n, false, Image::FORMAT_RGH);
		valley_fog_texture.unref();
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 4);
	uint16_t *b = reinterpret_cast<uint16_t *>(bytes.ptrw());
	// The saturated layer from each cell's floor up: the pooled air at each height against the dew point there.
	LocalVector<float> sat;
	sat.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		const float g = h[i];
		float top = -Math::INF;
		if (Math::is_finite(g) && pl[i] > 0.05f) {
			const float depth = MAX(pd[i], 10.0f);
			for (int k = 0; k <= 12; k++) {
				const float dz = depth * float(k) / 12.0f;
				const float y = g + dz;
				const float t = cur_air_temperature - 0.0065f * (y - ground_height) - pl[i] * (1.0f - Math::smoothstep(0.0f, depth, dz));
				const float td = dew - 0.0018f * (y - ground_height);
				if (t > td) {
					break;
				}
				top = g + MAX(dz, 5.0f);
			}
		}
		sat[i] = top;
	}
	// The fog fills the pool like water: one level across the basin, set by its lowest fogged floor (the pool's
	// deepest air is the coldest), so the top is flat and the slopes above it stay clear.
	const int r = MAX(1, int(Math::ceil(COLD_POOL_RADIUS / _cell_size())));
	for (int i = 0; i < n * n; i++) {
		const float g = h[i];
		float target = 0.0f;
		if (Math::is_finite(g)) {
			const int x = i % n, z = i / n;
			float low = Math::INF;
			float level = -Math::INF;
			for (int zz = MAX(z - r, 0); zz < MIN(z + r + 1, n); zz++) {
				for (int xx = MAX(x - r, 0); xx < MIN(x + r + 1, n); xx++) {
					const int j = zz * n + xx;
					if (sat[j] > -Math::INF && h[j] < low) {
						low = h[j];
						level = sat[j];
					}
				}
			}
			target = level > g ? level - g : 0.0f;
		}
		f[i] += (target - f[i]) * (target > f[i] ? form : lift);
		if (f[i] < 0.2f && target <= 0.0f) {
			f[i] = 0.0f;
		}
		const float gy = Math::is_finite(g) ? g : ground_height;
		b[i * 2 + 0] = Math::make_half_float(gy + f[i]);
		b[i * 2 + 1] = Math::make_half_float(f[i]);
		if (f[i] > 1.0f) {
			deepest = MAX(deepest, f[i]);
			span.x = MIN(span.x, gy);
			span.y = MAX(span.y, gy + f[i]);
		}
	}
	valley_fog_max = deepest;
	valley_fog_span = span;
	valley_fog_image->set_data(n, n, false, Image::FORMAT_RGH, bytes);
	if (valley_fog_texture.is_null()) {
		valley_fog_texture = ImageTexture::create_from_image(valley_fog_image);
	} else {
		valley_fog_texture->update(valley_fog_image);
	}
}

void WeatherMap3D::_apply_valley_fog(bool p_active) {
	if (!p_active) {
		if (valley_fog_volume) {
			valley_fog_volume->queue_free();
			valley_fog_volume = nullptr;
		}
		return;
	}
	if (!valley_fog_volume) {
		if (valley_fog_material.is_null()) {
			Ref<Shader> sh;
			sh.instantiate();
			// The fog's top from the map (bilinear between cells, so it tapers to the ground at its edges), a few metres
			// soft; white droplets (single-scatter albedo near 1).
			sh->set_code(R"(shader_type fog;
uniform sampler2D fog_map : filter_linear, repeat_disable;
uniform vec4 map_rect;
uniform float density;
uniform vec3 ms_light;
void fog() {
	vec2 uv = (WORLD_POSITION.xz - map_rect.xy) / map_rect.zw;
	vec2 e = min(uv, 1.0 - uv);
	vec2 t = texture(fog_map, uv).rg;
	float top = t.r;
	float d = density * smoothstep(0.5, 4.0, t.g) * (1.0 - smoothstep(top - 6.0, top + 1.0, WORLD_POSITION.y)) * smoothstep(0.0, 0.05, min(e.x, e.y));
	DENSITY = d;
	ALBEDO = vec3(1.0);
	// Light scattered many times inside the fog (optical depths of several): the froxels light single scattering only,
	// so the rest comes in as emission, the sky's and sun's light spread evenly (Wrenninge 2015's octaves, summed).
	// The froxel pass multiplies EMISSION by DENSITY itself.
	EMISSION = ms_light;
}
)");
			valley_fog_material.instantiate();
			valley_fog_material->set_shader(sh);
		}
		valley_fog_volume = memnew(FogVolume);
		valley_fog_volume->set_shape(RSE::FOG_VOLUME_SHAPE_BOX);
		valley_fog_volume->set_material(valley_fog_material);
		valley_fog_volume->set_as_top_level(true);
		add_child(valley_fog_volume, false, INTERNAL_MODE_BACK);
	}
	const Rect2 r = get_map_rect();
	valley_fog_material->set_shader_parameter("fog_map", valley_fog_texture);
	valley_fog_material->set_shader_parameter("map_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
	valley_fog_material->set_shader_parameter("density", valley_fog_density);
	{
		const float sun = sun_known ? MAX(cur_sun_sine, 0.0f) * (1.0f - 0.8f * cur_cloud_cover) : 0.3f;
		const float ms = valley_fog_multiple_scatter * (0.2f + 1.5f * sun);
		valley_fog_material->set_shader_parameter("ms_light", Vector3(ms, ms, ms * 1.04f));
	}
	const float lo = valley_fog_span.x - 5.0f;
	const float hi = valley_fog_span.y + 5.0f;
	valley_fog_volume->set_size(Vector3(r.size.x, MAX(hi - lo, 1.0f), r.size.y));
	valley_fog_volume->set_global_position(Vector3(r.position.x + 0.5f * r.size.x, 0.5f * (lo + hi), r.position.y + 0.5f * r.size.y));
}

void WeatherMap3D::set_valley_fog(bool p_enabled) {
	valley_fog = p_enabled;
	if (!valley_fog) {
		cell_valley_fog.clear();
		valley_fog_max = 0.0f;
	}
}

float WeatherMap3D::get_valley_fog_depth_at(Vector3 p_position) const {
	if (!valley_fog || cell_valley_fog.size() != resolution * resolution) {
		return 0.0f;
	}
	return MAX(_sample(cell_valley_fog, Vector2(p_position.x, p_position.z)), 0.0f);
}

float WeatherMap3D::get_valley_fog_top_at(Vector3 p_position) const {
	const float d = get_valley_fog_depth_at(p_position);
	if (d < 1.0f || cell_height.size() != resolution * resolution) {
		return -Math::INF;
	}
	const float g = _sample(cell_height, Vector2(p_position.x, p_position.z));
	return (Math::is_finite(g) ? g : ground_height) + d;
}

// Background aerosol haze. Koschmieder: a black target fades to the 2% contrast threshold at the
// meteorological visibility V, so the extinction is 3.912 / V. Hygroscopic aerosol swells with the
// relative humidity, its extinction growing as (1 - RH)^-gamma (Hanel 1976; Kasten 1969), gamma about
// 0.35 for continental aerosol, capped at RH 0.98 (beyond it the haze is mist, a different source).
// Its single-scatter albedo is about 0.9 (continental, mid-visible), and it thins with height over the
// boundary layer's scale height. The froxel fog lights it with the sun, the sun's shadow maps and the
// cloud shadow projected on the sun, which is where the shafts come from.
float WeatherMap3D::aerosol_extinction(float p_dry_visibility_km, float p_humidity) {
	const float rh = CLAMP(p_humidity, 0.0f, 0.98f);
	return 3.912f / (MAX(p_dry_visibility_km, 0.05f) * 1000.0f) * Math::pow(1.0f - rh, -0.35f);
}

void WeatherMap3D::_update_aerosol() {
	if (!aerosol_haze) {
		if (applied_aerosol >= 0.0f) {
			// Marked cleared first: clearing re-applies the haze (_apply_haze), which comes back here.
			applied_aerosol = -1.0f;
			clear_precipitation_extinction(this);
		}
		return;
	}
	// Fires' smoke overhead adds its own (weather_smoke.cpp).
	const float sigma = aerosol_extinction(haze_visibility, cur_air_humidity) + _smoke_sigma();
	if (applied_aerosol < 0.0f || Math::abs(sigma - applied_aerosol) > 0.02f * applied_aerosol) {
		applied_aerosol = sigma;
		set_precipitation_extinction(this, sigma, haze_height, 0.9f);
	}
}

void WeatherMap3D::set_aerosol_haze(bool p_enabled) {
	aerosol_haze = p_enabled;
	if (is_inside_tree()) {
		_update_aerosol();
	}
}

void WeatherMap3D::set_haze_visibility(float p_km) {
	haze_visibility = MAX(p_km, 0.05f);
	applied_aerosol = aerosol_haze ? 0.0f : applied_aerosol;
}

void WeatherMap3D::set_haze_height(float p_m) {
	haze_height = MAX(p_m, 10.0f);
	applied_aerosol = aerosol_haze ? 0.0f : applied_aerosol;
	applied_sky_aerosol = Vector2(-1.0f, -1.0f);
}

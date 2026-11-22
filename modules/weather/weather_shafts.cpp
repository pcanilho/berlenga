/**************************************************************************/
/*  weather_shafts.cpp                                                    */
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

// Far rain and snow shafts over the scene (docs/ideas/clouds.md, "Distant shafts in front of terrain"). The sky
// marches the storms' shafts and the lake-effect veils, so they showed only where the sky shows. This pass marches
// the same functions (WeatherSkyMaterial::get_shafts_glsl()) along each scene pixel's ray, from 1.5 km (nearer,
// the falling drops and flakes and their haze draw the precipitation) to the depth buffer, and blends the result
// over the opaque scene: the depth-limited ray march Nubis and Red Dead Redemption 2 composite their volumetrics
// with (Schneider 2015; Bauer 2019). The march returns its light as two weights (the sky's light under the base,
// the sun's through the drops' phase); the material's own lighting supplies the two: the sky's irradiance seen
// sideways (half sky, half ground, as the sky's ambient_bottom) and the sun's colour in light(). Hidden while the
// environment path traces (the tracer has its own media) and while there is nothing to draw.

#include "weather_map_3d.h"
#include "weather_sky_material.h"

#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

static const char *SHAFTS_HEAD = R"(
shader_type spatial;
render_mode blend_premul_alpha, depth_test_disabled, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;

uniform sampler2D shafts_depth : hint_depth_texture, filter_nearest;
uniform vec3 shafts_sun_dir = vec3(0.0, 1.0, 0.0);
uniform float shafts_t_min = 1500.0;
)";

static const char *SHAFTS_BODY = R"(
varying float v_sun;

void vertex() {
	POSITION = vec4(VERTEX.xy, 0.5, 1.0);
}

void fragment() {
	float depth = textureLod(shafts_depth, SCREEN_UV, 0.0).r;
	// Sky pixels (reversed depth: 0 at infinity) are the sky's own: it draws its shafts there.
	if (depth <= 1e-7) {
		discard;
	}
	vec4 vp = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	vec3 wp = (INV_VIEW_MATRIX * vec4(vp.xyz / vp.w, 1.0)).xyz;
	vec3 cam = INV_VIEW_MATRIX[3].xyz;
	vec3 d = wp - cam;
	float t_max = length(d);
	if (t_max <= shafts_t_min) {
		discard;
	}
	vec3 rd = d / t_max;
	// Interleaved gradient noise (Jimenez 2014) over the steps.
	float jitter = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
	// Light weights: ambient_bottom (1, 0, 0) and the sun (0, 1, 0) give the march's two sums in r and g.
	vec4 o = vec4(0.0, 0.0, 0.0, 1.0);
	float dr;
	if (cloud_storm_count > 0) {
		o = march_rain_shafts(cam, rd, shafts_sun_dir, vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), jitter, shafts_t_min, t_max, dr);
	}
	if (cloud_lake.x > 0.0) {
		vec4 l = march_lake_snow(cam, rd, shafts_sun_dir, vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), jitter, shafts_t_min, t_max, dr);
		o = vec4(o.rgb + o.a * l.rgb, o.a * l.a);
	}
	if (o.a >= 0.999) {
		discard;
	}
	// Seen sideways, the sky's irradiance is half sky and half ground, as ambient_bottom under the base.
	vec3 side = normalize(vec3(-rd.x, 0.0, -rd.z) + vec3(0.0, 1e-4, 0.0));
	NORMAL = normalize((VIEW_MATRIX * vec4(side, 0.0)).xyz);
	// The irradiance on a sideways normal takes in the bright horizon and the sunlit ground in full, about twice
	// the sky's ambient_bottom (measured: the same lake-effect veil just above and just below the horizon); halved
	// so a veil crossing the horizon keeps one brightness.
	ALBEDO = vec3(o.r * 0.5);
	ROUGHNESS = 1.0;
	METALLIC = 0.0;
	SPECULAR = 0.0;
	v_sun = o.g;
	ALPHA = 1.0 - o.a;
}

void light() {
	if (LIGHT_IS_DIRECTIONAL) {
		SPECULAR_LIGHT += LIGHT_COLOR * v_sun;
	}
}
)";

void WeatherMap3D::set_distant_shafts(bool p_enabled) {
	distant_shafts = p_enabled;
	if (!distant_shafts) {
		_free_shafts();
	}
}

void WeatherMap3D::_free_shafts() {
	if (shafts_mesh) {
		shafts_mesh->queue_free();
		shafts_mesh = nullptr;
	}
}

void WeatherMap3D::_update_shafts() {
	if (!distant_shafts || active != this || !is_inside_tree()) {
		return;
	}
	Ref<WeatherSkyMaterial> sky = WeatherSkyMaterial::find_for(this);
	Ref<Environment> env = WeatherSkyMaterial::find_environment(this);
	const RID sky_rid = sky.is_valid() ? sky->get_rid() : RID();
	RenderingServer *rs = RS::get_singleton();
	bool show = sky.is_valid() && !(env.is_valid() && env->is_pathtracing_enabled());
	if (show) {
		const int storms = rs->material_get_param(sky_rid, "cloud_storm_count");
		const Vector4 lake = rs->material_get_param(sky_rid, "cloud_lake");
		show = storms > 0 || lake.x > 0.0f;
	}
	if (!show) {
		if (shafts_mesh) {
			shafts_mesh->set_visible(false);
		}
		return;
	}
	if (shafts_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(String(SHAFTS_HEAD) + String(WeatherClouds::GLSL) + String(WeatherSkyMaterial::get_shafts_glsl()) + String(SHAFTS_BODY));
		shafts_material.instantiate();
		shafts_material->set_shader(shader);
	}
	if (!shafts_mesh) {
		Ref<QuadMesh> quad;
		quad.instantiate();
		quad->set_size(Size2(2.0f, 2.0f));
		shafts_mesh = memnew(MeshInstance3D);
		shafts_mesh->set_name("DistantShafts");
		shafts_mesh->set_mesh(quad);
		shafts_mesh->set_material_override(shafts_material);
		shafts_mesh->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		shafts_mesh->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
		shafts_mesh->set_custom_aabb(AABB(Vector3(-1e7f, -1e7f, -1e7f), Vector3(2e7f, 2e7f, 2e7f)));
		shafts_mesh->set_as_top_level(true);
		add_child(shafts_mesh, false, INTERNAL_MODE_BACK);
	}
	shafts_mesh->set_visible(true);
	// The sky's cloud uniforms, as they stand (WeatherMap3D pushes them to the sky; the shafts read the same).
	List<PropertyInfo> params;
	shafts_material->get_shader()->get_shader_uniform_list(&params);
	const RID own = shafts_material->get_rid();
	for (const PropertyInfo &pi : params) {
		if (pi.name.begins_with("shafts_")) {
			continue;
		}
		const Variant v = rs->material_get_param(sky_rid, pi.name);
		if (v.get_type() != Variant::NIL) {
			rs->material_set_param(own, pi.name, v);
		}
	}
	const DirectionalLight3D *sun = sun_light.is_empty() ? nullptr : Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
	if (sun) {
		rs->material_set_param(own, "shafts_sun_dir", sun->get_global_transform().basis.get_column(2).normalized());
	}
}

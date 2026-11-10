/**************************************************************************/
/*  weather_sky_material.cpp                                                */
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

#include "weather_sky_material.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

Ref<Shader> WeatherSkyMaterial::shared_shader;
Ref<Shader> WeatherSkyMaterial::shared_shader_plain;

namespace {

// The atmosphere is PhysicalSkyMaterial's (Preetham-style Rayleigh and Mie), as a function so the
// clouds can take their ambient light and the sun's colour through the air from it.
const char *SKY_HEAD = R"(
// NOTE: Shader of WeatherSkyMaterial (modules/weather).
shader_type sky;
render_mode use_half_res_pass, half_res_reprojection, use_debanding;

uniform float rayleigh : hint_range(0, 64) = 2.0;
uniform vec4 rayleigh_color : source_color = vec4(0.3, 0.405, 0.6, 1.0);
uniform float mie : hint_range(0, 1) = 0.005;
uniform float mie_eccentricity : hint_range(-1, 1) = 0.8;
uniform vec4 mie_color : source_color = vec4(0.69, 0.729, 0.812, 1.0);
uniform float turbidity : hint_range(0, 1000) = 10.0;
uniform float sun_disk_scale : hint_range(0, 360) = 1.0;
uniform vec4 ground_color : source_color = vec4(0.1, 0.07, 0.034, 1.0);
uniform float exposure : hint_range(0, 128) = 1.0;

group_uniforms clouds;
// Raymarch steps for the screen, and for the radiance map (lighting and path traced bounces).
uniform int cloud_steps : hint_range(8, 256) = 64;
uniform int cloud_radiance_steps : hint_range(4, 64) = 16;
// Steps towards the sun from each sample, at growing distance (Frostbite, Nubis).
uniform int cloud_light_steps : hint_range(1, 12) = 6;
// Clouds fade into the atmosphere from half this distance (m).
uniform float cloud_far : hint_range(1000, 200000, 100) = 40000.0;
uniform float cloud_brightness : hint_range(0, 8) = 1.0;
group_uniforms;

group_uniforms weather_map; // Set by WeatherMap3D each update.
)";

const char *SKY_BODY = R"(
group_uniforms;

const vec3 UP = vec3(0.0, 1.0, 0.0);
const float rayleigh_zenith_size = 8.4e3;
const float mie_zenith_size = 1.25e3;
const float PLANET_RADIUS = 6371000.0;

float henyey_greenstein(float cos_theta, float g) {
	const float k = 0.0795774715459;
	return k * (1.0 - g * g) / (pow(1.0 + g * g - 2.0 * g * cos_theta, 1.5));
}

// Air's extinction along dir (towards the sky), PhysicalSkyMaterial's optical mass.
vec3 air_extinction(vec3 dir, float sun_fade) {
	vec3 rayleigh_beta = (rayleigh - (1.0 - sun_fade)) * rayleigh_color.rgb * 0.0001;
	vec3 mie_beta = turbidity * mie * mie_color.rgb * 0.000434;
	float zenith = max(0.0, dot(UP, dir));
	float optical_mass = 1.0 / (zenith + 0.15 * pow(3.885 + 54.5 * zenith, -1.253));
	return exp(-(rayleigh_beta * rayleigh_zenith_size * optical_mass + mie_beta * mie_zenith_size * optical_mass));
}

// PhysicalSkyMaterial's sky for one direction, before exposure; with or without the sun's disk.
vec3 atmosphere(vec3 dir, vec3 sun_dir, float light_energy, vec3 light_color, float light_size, bool disk) {
	float zenith_angle = clamp(dot(UP, sun_dir), -1.0, 1.0);
	float sun_energy = max(0.0, 0.757 * zenith_angle) * light_energy;
	float sun_fade = 1.0 - clamp(1.0 - exp(sun_dir.y), 0.0, 1.0);
	vec3 rayleigh_beta = (rayleigh - (1.0 - sun_fade)) * rayleigh_color.rgb * 0.0001;
	vec3 mie_beta = turbidity * mie * mie_color.rgb * 0.000434;
	vec3 extinction = air_extinction(dir, sun_fade);
	float cos_theta = dot(dir, sun_dir);
	float rayleigh_phase = (3.0 / (16.0 * PI)) * (1.0 + pow(cos_theta * 0.5 + 0.5, 2.0));
	vec3 betaRTheta = rayleigh_beta * rayleigh_phase;
	vec3 betaMTheta = mie_beta * henyey_greenstein(cos_theta, mie_eccentricity);
	vec3 Lin = pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * (1.0 - extinction), vec3(1.5));
	Lin *= mix(vec3(1.0), pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * extinction, vec3(0.5)), clamp(pow(1.0 - zenith_angle, 5.0), 0.0, 1.0));
	Lin *= mix(ground_color.rgb, vec3(1.0), smoothstep(-0.1, 0.1, dot(UP, dir)));
	vec3 color = Lin;
	if (disk) {
		float sundisk = smoothstep(cos(light_size * sun_disk_scale), cos(light_size * sun_disk_scale * 0.5), cos_theta);
		color += (sun_energy * extinction) * sundisk * light_color;
	}
	return pow(color, vec3(1.0 / (1.2 + (1.2 * sun_fade))));
}

// Distances along rd from ro to a sphere about the planet's centre (near, far); negative when missed.
vec2 sphere_hits(vec3 ro, vec3 rd, float r) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - r * r;
	float disc = b * b - c;
	if (disc < 0.0) {
		return vec2(-1.0);
	}
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

// Two Henyey-Greenstein lobes, forward and back (Frostbite); p_c narrows them for the octaves.
float cloud_phase(float cos_theta, float p_c) {
	return mix(henyey_greenstein(cos_theta, 0.8 * p_c), henyey_greenstein(cos_theta, -0.3 * p_c), 0.3);
}

// Clouds along rd from the camera: radiance (rgb) and transmittance (a).
vec4 march_clouds(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, int steps, float jitter) {
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	float r_base = PLANET_RADIUS + cloud_base_height;
	float r_top = PLANET_RADIUS + cloud_top_max;
	vec2 hb = sphere_hits(ro, rd, r_base);
	vec2 ht = sphere_hits(ro, rd, r_top);
	float t0;
	float t1;
	if (cam.y < cloud_base_height) {
		if (sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0); // the ground is in the way
		}
		t0 = hb.y;
		t1 = ht.y;
	} else if (cam.y < cloud_top_max) {
		t0 = 0.0;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	} else {
		if (ht.x < 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = ht.x;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	}
	if (t0 > cloud_far) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	t1 = min(t1, t0 + cloud_far);
	float ds = (t1 - t0) / float(steps);
	float cos_theta = dot(rd, sun_dir);
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	for (int i = 0; i < steps; i++) {
		float t = t0 + (float(i) + jitter) * ds;
		vec3 pl = ro + rd * t;
		vec3 p = vec3(cam.x + rd.x * t, length(pl) - PLANET_RADIUS, cam.z + rd.z * t);
		vec2 ct = wc_cover_type(p.xz);
		float sigma = wc_extinction(p, ct, true, ds);
		if (sigma <= 1e-6) {
			continue;
		}
		// Optical depth towards the sun: steps doubling from 25 m (Frostbite's growing distances).
		float tau = 0.0;
		float d = 0.0;
		float step_l = 25.0;
		for (int j = 0; j < cloud_light_steps; j++) {
			vec3 lp = p + sun_dir * (d + 0.5 * step_l);
			tau += wc_extinction(lp, wc_cover_type(lp.xz), false, step_l) * step_l;
			d += step_l;
			step_l *= 2.0;
		}
		// Multiple scattering by Wrenninge's octaves: scattering, extinction and phase eccentricity
		// each halved per octave.
		vec3 light = vec3(0.0);
		float a = 1.0;
		float b = 1.0;
		float c = 1.0;
		for (int n = 0; n < 3; n++) {
			light += a * sun_radiance * exp(-b * tau) * cloud_phase(cos_theta, c);
			a *= 0.5;
			b *= 0.5;
			c *= 0.5;
		}
		// Light that has scattered many times: the diffuse field of a thick, non-absorbing layer in the
		// two-stream approximation (Bohren 1987). The down stream at optical depth tau carries
		// 1 / (1 + 3/4 (1 - g) tau) of the sun's flux, and the up stream about as much again near the lit
		// side, spread evenly over directions.
		float diffuse = 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau);
		light += sun_radiance * diffuse * (2.0 / (4.0 * PI));
		float hf = clamp((p.y - cloud_base_height) / wc_top_height(ct.y), 0.0, 1.0);
		light += mix(ambient_bottom, ambient_top, hf);
		// Energy-conserving step (Hillaire 2016): albedo 1, so the scattering is the extinction.
		float tr = exp(-sigma * ds);
		radiance += transmittance * light * (1.0 - tr);
		transmittance *= tr;
		if (transmittance < 0.01) {
			break;
		}
	}
	// Far clouds fade into the air.
	float fade = 1.0 - smoothstep(0.5 * cloud_far, cloud_far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

void sky() {
	vec3 sun_dir = LIGHT0_ENABLED ? normalize(LIGHT0_DIRECTION) : UP;
	float light_energy = LIGHT0_ENABLED ? LIGHT0_ENERGY : 0.0;
	if (AT_HALF_RES_PASS) {
		float sun_fade = 1.0 - clamp(1.0 - exp(sun_dir.y), 0.0, 1.0);
		vec3 sun_radiance = LIGHT0_COLOR * light_energy * air_extinction(sun_dir, sun_fade) * cloud_brightness;
		vec3 ambient_top = atmosphere(UP, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, false) * cloud_brightness;
		// From below: the sky's light off the ground (its colour as albedo), lit by the sun where clear.
		vec3 ambient_bottom = ambient_top * 0.5 + ground_color.rgb * sun_radiance * max(sun_dir.y, 0.0) * (1.0 - 0.7 * cloud_cover) / PI;
		float jitter = AT_CUBEMAP_PASS ? 0.5 : fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
#ifdef CLOUD_REPROJECTION
		// Reprojected (half_res_reprojection): a new start each frame, which the resolve averages.
		jitter = AT_CUBEMAP_PASS ? 0.5 : fract(jitter + 0.61803399 * floor(TIME * 60.0));
#endif
		vec4 clouds = march_clouds(POSITION, EYEDIR, sun_dir, sun_radiance, ambient_top, ambient_bottom, AT_CUBEMAP_PASS ? cloud_radiance_steps : cloud_steps, jitter);
		COLOR = clouds.rgb;
		ALPHA = clouds.a;
	} else {
		vec3 air = LIGHT0_ENABLED ? atmosphere(EYEDIR, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, true) : vec3(0.0);
		COLOR = (air * HALF_RES_COLOR.a + HALF_RES_COLOR.rgb) * exposure;
	}
}
)";

} // namespace

void WeatherSkyMaterial::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "shader") {
		// Built in; not saved, so a project always gets the engine's current shader.
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void WeatherSkyMaterial::set_cloud_state(const WeatherClouds::State &p_state) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	rs->material_set_param(m, "cloud_base_height", p_state.base_height);
	rs->material_set_param(m, "cloud_thickness", p_state.thickness);
	rs->material_set_param(m, "cloud_top_max", p_state.top_max);
	rs->material_set_param(m, "cloud_density", p_state.density);
	rs->material_set_param(m, "cloud_cover", p_state.cover);
	rs->material_set_param(m, "cloud_type", p_state.type);
	rs->material_set_param(m, "cloud_variation", p_state.variation);
	rs->material_set_param(m, "cloud_cover_scale", p_state.cover_scale);
	rs->material_set_param(m, "cloud_shape_scale", p_state.shape_scale);
	rs->material_set_param(m, "cloud_drift", p_state.drift);
	rs->material_set_param(m, "cloud_storm_count", p_state.storm_count);
	PackedVector4Array storms;
	storms.resize(WeatherClouds::MAX_STORMS);
	for (int i = 0; i < WeatherClouds::MAX_STORMS; i++) {
		storms.write[i] = Vector4(p_state.storms[i][0], p_state.storms[i][1], MAX(p_state.storms[i][2], 1.0f), p_state.storms[i][3]);
	}
	rs->material_set_param(m, "cloud_storms", storms);
}

void WeatherSkyMaterial::_update_shader() {
	Ref<Shader> &shader = cloud_reprojection ? shared_shader : shared_shader_plain;
	if (shader.is_null()) {
		shader.instantiate();
		String head = String(SKY_HEAD);
		if (cloud_reprojection) {
			head = head.replace("shader_type sky;\n", "shader_type sky;\n#define CLOUD_REPROJECTION\n");
		} else {
			head = head.replace("half_res_reprojection, ", "");
		}
		shader->set_code(head + String(WeatherClouds::GLSL) + String(SKY_BODY));
	}
	set_shader(shader);
}

void WeatherSkyMaterial::set_cloud_reprojection(bool p_enabled) {
	if (cloud_reprojection == p_enabled) {
		return;
	}
	cloud_reprojection = p_enabled;
	_update_shader();
}

bool WeatherSkyMaterial::is_cloud_reprojection_enabled() const {
	return cloud_reprojection;
}

void WeatherSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cloud_reprojection", "enabled"), &WeatherSkyMaterial::set_cloud_reprojection);
	ClassDB::bind_method(D_METHOD("is_cloud_reprojection_enabled"), &WeatherSkyMaterial::is_cloud_reprojection_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cloud_reprojection"), "set_cloud_reprojection", "is_cloud_reprojection_enabled");
}

void WeatherSkyMaterial::cleanup() {
	shared_shader.unref();
	shared_shader_plain.unref();
	WeatherClouds::free_textures();
}

WeatherSkyMaterial::WeatherSkyMaterial() {
	_update_shader();
	RS::get_singleton()->material_set_param(get_rid(), "cloud_shape", WeatherClouds::shape_texture()->get_rid());
	RS::get_singleton()->material_set_param(get_rid(), "cloud_detail", WeatherClouds::detail_texture()->get_rid());
}

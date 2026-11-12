/**************************************************************************/
/*  weather_sky_material.cpp                                              */
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

#include "weather_sky_material.h"

#include "atmosphere.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

Ref<Shader> WeatherSkyMaterial::shared_shaders[4];
Ref<ImageTexture> WeatherSkyMaterial::atmo_transmittance_tex;
Ref<ImageTexture> WeatherSkyMaterial::atmo_multiscatter_tex;

namespace {

// The atmosphere is PhysicalSkyMaterial's (Preetham-style Rayleigh and Mie), as a function so the
// clouds can take their ambient light and the sun's colour through the air from it.
const char *SKY_HEAD = R"(
// NOTE: Shader of WeatherSkyMaterial (modules/weather).
shader_type sky;
// No use_debanding: it adds +-1/255 per channel (green opposite red and blue) in linear HDR before the exposure
// and the tonemapper, which at dusk turned it into a magenta and green checker along the horizon, and the
// reprojected quarter pass froze it into 4x4 blocks. The viewport's own debanding dithers after tonemapping.
render_mode use_half_res_pass, half_res_reprojection;

uniform float rayleigh : hint_range(0, 64) = 2.0;
uniform vec4 rayleigh_color : source_color = vec4(0.3, 0.405, 0.6, 1.0);
uniform float mie : hint_range(0, 1) = 0.005;
uniform float mie_eccentricity : hint_range(-1, 1) = 0.8;
uniform vec4 mie_color : source_color = vec4(0.69, 0.729, 0.812, 1.0);
uniform float turbidity : hint_range(0, 1000) = 10.0;
uniform float sun_disk_scale : hint_range(0, 360) = 1.0;
uniform vec4 ground_color : source_color = vec4(0.1, 0.07, 0.034, 1.0);
uniform float exposure : hint_range(0, 128) = 1.0;

group_uniforms atmosphere; // The scattering atmosphere (atmosphere_model SCATTERING); lights set by SkyClock.
uniform int atmo_steps : hint_range(8, 64) = 32;
uniform int atmo_radiance_steps : hint_range(4, 32) = 12;
uniform int atmo_cloud_air_steps : hint_range(4, 32) = 12; // the air in front of the clouds
uniform bool atmo_driven = false;
uniform vec3 atmo_sun_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 atmo_sun_irradiance = vec3(0.0); // above the air, in light energy units
uniform vec3 atmo_moon_direction = vec3(0.0, -1.0, 0.0);
uniform vec3 atmo_moon_irradiance = vec3(0.0);
uniform float atmo_sun_angular_size = 0.00930; // rad, diameter
group_uniforms;

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

group_uniforms lightning; // Set by WeatherLightning3D while flashes last.
uniform int lightning_count = 0;
uniform vec4 lightning_flashes[4]; // world position, energy this frame
uniform vec3 lightning_color = vec3(0.82, 0.86, 1.0);
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
// p_pixel: the angle (rad) one pixel spans. The noise is read at the pixel's footprint, t * p_pixel,
// not at the march step: at grazing angles a step is hundreds of metres, and the shape noise read
// at that scale is a few texels across, whose trilinear blend thresholded is a box (flat faces,
// square corners). Along the ray the jitter (and the reprojection) average the steps instead.
// r_depth: the distance to the clouds seen, weighted by how much each step hides (for the air in front).
vec4 march_clouds(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, int steps, float jitter, float p_pixel, out float r_depth) {
#ifdef ATMO_SCATTERING
	// The air in front hazes far clouds into the horizon (aerial perspective), so the deck runs out to it.
	const float far = 200000.0;
#else
	float far = cloud_far;
#endif
	r_depth = 0.0;
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
	if (t0 > far) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	t1 = min(t1, t0 + far);
	float depth_sum = 0.0;
	float ds = (t1 - t0) / float(steps);
	float cos_theta = dot(rd, sun_dir);
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	for (int i = 0; i < steps; i++) {
		float t = t0 + (float(i) + jitter) * ds;
		vec3 pl = ro + rd * t;
		vec3 p = vec3(cam.x + rd.x * t, length(pl) - PLANET_RADIUS, cam.z + rd.z * t);
		vec2 ct = wc_cover_type(p.xz);
		float sigma = wc_extinction(p, ct, true, min(ds, t * p_pixel));
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
		depth_sum += t * transmittance * (1.0 - tr);
		transmittance *= tr;
		if (transmittance < 0.01) {
			break;
		}
	}
	// Far clouds fade into the air.
	r_depth = transmittance < 1.0 ? depth_sum / (1.0 - transmittance) : t1;
	float fade = 1.0 - smoothstep(0.5 * far, far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

// Lightning inside the clouds. Each flash is a point source whose light diffuses through the cloud as
// the sun's does in march_clouds (two-stream, Bohren 1987), spread over 4 pi r^2 (r no less than
// 200 m, the size of the lit core), and seen where the view ray passes nearest it, through the
// cloud's opacity along the ray. Drawn at full resolution, so cloud reprojection never smears a flash.
vec3 lightning_glow(vec3 cam, vec3 rd, float opacity) {
	vec3 sum = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		if (i >= lightning_count) {
			break;
		}
		vec3 p = lightning_flashes[i].xyz;
		float t = max(dot(p - cam, rd), 0.0);
		float d = length(cam + rd * t - p);
		float sigma = wc_extinction(p, wc_cover_type(p.xz), false, 200.0);
		float diffuse = 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * sigma * d);
		sum += vec3(lightning_flashes[i].w * diffuse / (4.0 * PI * (d * d + 40000.0)));
	}
	return sum * lightning_color * opacity;
}

#ifdef ATMO_SCATTERING
// The two bodies lighting the air: SkyClock's sun and moon when it drives the sky, else the first
// directional light taken as the sun above the air.
void atmo_lights(out vec3 l0, out vec3 e0, out vec3 l1, out vec3 e1, out float size0) {
	if (atmo_driven) {
		l0 = normalize(atmo_sun_direction);
		e0 = atmo_sun_irradiance;
		l1 = normalize(atmo_moon_direction);
		e1 = atmo_moon_irradiance;
		size0 = atmo_sun_angular_size;
	} else {
		l0 = LIGHT0_ENABLED ? normalize(LIGHT0_DIRECTION) : UP;
		e0 = LIGHT0_ENABLED ? LIGHT0_COLOR * LIGHT0_ENERGY : vec3(0.0);
		l1 = -UP;
		e1 = vec3(0.0);
		size0 = LIGHT0_ENABLED ? LIGHT0_SIZE : atmo_sun_angular_size;
	}
}
#endif

void sky() {
#ifdef ATMO_SCATTERING
	vec3 l0;
	vec3 e0;
	vec3 l1;
	vec3 e1;
	float size0;
	atmo_lights(l0, e0, l1, e1, size0);
	float cam_h = max(POSITION.y, 1.0);
	float r_cam = atmo_radii.x + cam_h;
	if (AT_QUARTER_RES_PASS) {
		// The air alone: smooth, so a quarter of the resolution carries it (Hillaire's sky-view table
		// is coarser still); its transmittance in alpha for what lies past it.
		vec3 t_view;
		vec3 air = atmo_scatter(cam_h, EYEDIR, l0, e0, l1, e1, 1e12, AT_CUBEMAP_PASS ? atmo_radiance_steps : atmo_steps, 0.3, t_view).rgb;
		if (atmo_far_water > 0.0 && atmo_hits_ground(r_cam, EYEDIR.y)) {
			// Open water past the scene: the sky mirrored at the angle the ray meets the curved sea, by
			// Fresnel (Schlick, water), seen through the air in front (Bruneton 2010 lights an ocean so).
			float t_hit = atmo_ray_sphere(r_cam, EYEDIR.y, atmo_radii.x, false);
			vec3 up_hit = normalize(vec3(0.0, r_cam, 0.0) + EYEDIR * t_hit);
			float c = clamp(-dot(EYEDIR, up_hit), 0.0, 1.0);
			vec2 hz = length(EYEDIR.xz) > 1e-5 ? normalize(EYEDIR.xz) : vec2(1.0, 0.0);
			vec3 rm = vec3(hz.x * sqrt(1.0 - c * c), c, hz.y * sqrt(1.0 - c * c));
			vec3 t_m;
			vec3 mirrored = atmo_scatter(1.0, rm, l0, e0, l1, e1, 1e12, 8, 0.5, t_m).rgb;
			float fresnel = 0.02 + 0.98 * pow(1.0 - c, 5.0);
			air += t_view * atmo_far_water * fresnel * mirrored;
		}
		COLOR = air;
		ALPHA = 1.0;
	} else if (AT_HALF_RES_PASS) {
		// The clouds over the air.
		vec4 air = vec4(QUARTER_RES_COLOR.rgb, 1.0);
		float r_c = atmo_radii.x + cloud_base_height;
		vec3 sun_radiance = e0 * atmo_light_transmittance(r_c, l0.y) * cloud_brightness;
		vec3 t_up;
		vec3 ambient_top = atmo_scatter(cloud_base_height, UP, l0, e0, l1, e1, 1e12, 8, 0.5, t_up).rgb * cloud_brightness;
		vec3 ambient_bottom = ambient_top * 0.5 + ground_color.rgb * sun_radiance * max(l0.y, 0.0) * (1.0 - 0.7 * cloud_cover) / PI;
		float jitter = AT_CUBEMAP_PASS ? 0.5 : fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
#ifdef CLOUD_REPROJECTION
		jitter = AT_CUBEMAP_PASS ? 0.5 : fract(jitter + 0.61803399 * floor(TIME * 60.0));
#endif
		vec3 dir_dx = dFdx(EYEDIR);
		vec3 dir_dy = dFdy(EYEDIR);
		float pixel_angle = max(length(dir_dx), length(dir_dy));
		vec4 clouds = vec4(0.0, 0.0, 0.0, 1.0);
		float cloud_depth = 0.0;
		if (dot(e0, vec3(1.0)) > 0.0) {
			clouds = march_clouds(POSITION, EYEDIR, l0, sun_radiance, ambient_top, ambient_bottom, AT_CUBEMAP_PASS ? cloud_radiance_steps : cloud_steps, jitter, pixel_angle, cloud_depth);
		}
		if (clouds.a < 0.999) {
			// Aerial perspective (Hillaire 2020): the clouds seen through the air in front of them, which
			// also scatters light toward the eye; what the clouds let through sees the whole sky behind.
			vec3 t_front;
			vec3 front = atmo_scatter(cam_h, EYEDIR, l0, e0, l1, e1, cloud_depth, atmo_cloud_air_steps, 0.5, t_front).rgb;
			COLOR = (1.0 - clouds.a) * front + t_front * clouds.rgb + air.rgb * clouds.a;
		} else {
			COLOR = air.rgb;
		}
		ALPHA = clouds.a;
	} else {
		// The sun's disc through the air, behind the clouds.
		vec3 disc = vec3(0.0);
		float c0 = dot(EYEDIR, l0);
		float half_size = 0.5 * size0 * sun_disk_scale;
		float sundisk = smoothstep(cos(half_size * 2.0), cos(half_size), c0);
		if (sundisk > 0.0 && !atmo_hits_ground(r_cam, EYEDIR.y)) {
			disc = e0 * atmo_transmittance_to_top(r_cam, EYEDIR.y) * sundisk;
		}
		vec3 glow = vec3(0.0);
		if (lightning_count > 0 && !AT_CUBEMAP_PASS) {
			glow = lightning_glow(POSITION, EYEDIR, 1.0 - HALF_RES_COLOR.a);
		}
		COLOR = (HALF_RES_COLOR.rgb + disc * HALF_RES_COLOR.a + glow) * exposure;
	}
#else
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
		// One pixel's angle, from the eye direction's change to the next pixel (the pass is uniform, so
		// the derivatives are defined here).
		vec3 dir_dx = dFdx(EYEDIR);
		vec3 dir_dy = dFdy(EYEDIR);
		float pixel_angle = max(length(dir_dx), length(dir_dy));
		float cloud_depth;
		vec4 clouds = march_clouds(POSITION, EYEDIR, sun_dir, sun_radiance, ambient_top, ambient_bottom, AT_CUBEMAP_PASS ? cloud_radiance_steps : cloud_steps, jitter, pixel_angle, cloud_depth);
		COLOR = clouds.rgb;
		ALPHA = clouds.a;
	} else {
		vec3 air = LIGHT0_ENABLED ? atmosphere(EYEDIR, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, true) : vec3(0.0);
		vec3 glow = vec3(0.0);
		if (lightning_count > 0 && !AT_CUBEMAP_PASS) {
			// Not in the radiance map: the channel or the flash's light already lights the scene.
			glow = lightning_glow(POSITION, EYEDIR, 1.0 - HALF_RES_COLOR.a);
		}
		COLOR = (air * HALF_RES_COLOR.a + HALF_RES_COLOR.rgb + glow) * exposure;
	}
#endif
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

void WeatherSkyMaterial::set_lightning(const Vector4 *p_flashes, int p_count, const Color &p_color) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	PackedVector4Array flashes;
	flashes.resize(4);
	p_count = CLAMP(p_count, 0, 4);
	for (int i = 0; i < 4; i++) {
		flashes.write[i] = i < p_count ? p_flashes[i] : Vector4();
	}
	rs->material_set_param(m, "lightning_count", p_count);
	rs->material_set_param(m, "lightning_flashes", flashes);
	rs->material_set_param(m, "lightning_color", Vector3(p_color.r, p_color.g, p_color.b));
}

Ref<WeatherSkyMaterial> WeatherSkyMaterial::find_for(Node *p_node) {
	if (!p_node || !p_node->is_inside_tree()) {
		return Ref<WeatherSkyMaterial>();
	}
	Ref<Environment> env = find_environment(p_node);
	if (env.is_null() || env->get_sky().is_null()) {
		return Ref<WeatherSkyMaterial>();
	}
	return env->get_sky()->get_material();
}

Ref<Environment> WeatherSkyMaterial::find_environment(Node *p_node) {
	Ref<Environment> env;
	Viewport *vp = p_node->get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	if (cam && cam->get_environment().is_valid()) {
		env = cam->get_environment();
	}
	Ref<World3D> world = vp ? vp->find_world_3d() : Ref<World3D>();
	if (env.is_null() && world.is_valid()) {
		env = world->get_environment().is_valid() ? world->get_environment() : world->get_fallback_environment();
	}
	return env;
}

void WeatherSkyMaterial::_update_shader() {
	const int key = (cloud_reprojection ? 1 : 0) + (atmosphere_model == ATMOSPHERE_SCATTERING ? 2 : 0);
	Ref<Shader> &shader = shared_shaders[key];
	if (shader.is_null()) {
		shader.instantiate();
		String head = String(SKY_HEAD);
		String defines;
		if (cloud_reprojection) {
			defines += "#define CLOUD_REPROJECTION\n";
		} else {
			head = head.replace("half_res_reprojection, ", "");
		}
		if (atmosphere_model == ATMOSPHERE_SCATTERING) {
			defines += "#define ATMO_SCATTERING\n";
			head = head.replace("use_half_res_pass, ", "use_half_res_pass, use_quarter_res_pass, ");
		}
		head = head.replace("shader_type sky;\n", "shader_type sky;\n" + defines);
		shader->set_code(head + String(WeatherClouds::GLSL) + String(WeatherAtmosphere::GLSL) + String(SKY_BODY));
	}
	set_shader(shader);
	_update_atmosphere_textures();
}

// The atmosphere's tables, baked once (Earth's air; about 0.3 s on one core) and shared by every sky.
void WeatherSkyMaterial::_update_atmosphere_textures() {
	if (atmosphere_model != ATMOSPHERE_SCATTERING) {
		return;
	}
	if (atmo_transmittance_tex.is_null()) {
		const WeatherAtmosphere::Params params;
		atmo_transmittance_tex = ImageTexture::create_from_image(WeatherAtmosphere::bake_transmittance(params));
		atmo_multiscatter_tex = ImageTexture::create_from_image(WeatherAtmosphere::bake_multiscatter(params));
	}
	RS::get_singleton()->material_set_param(get_rid(), "atmo_transmittance", atmo_transmittance_tex->get_rid());
	RS::get_singleton()->material_set_param(get_rid(), "atmo_multiscatter_lut", atmo_multiscatter_tex->get_rid());
}

void WeatherSkyMaterial::set_atmosphere_model(AtmosphereModel p_model) {
	if (atmosphere_model == p_model) {
		return;
	}
	atmosphere_model = p_model;
	_update_shader();
}

WeatherSkyMaterial::AtmosphereModel WeatherSkyMaterial::get_atmosphere_model() const {
	return atmosphere_model;
}

void WeatherSkyMaterial::set_atmosphere_lights(const Vector3 &p_sun_direction, const Vector3 &p_sun_irradiance, const Vector3 &p_moon_direction, const Vector3 &p_moon_irradiance) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	rs->material_set_param(m, "atmo_driven", true);
	rs->material_set_param(m, "atmo_sun_direction", p_sun_direction);
	rs->material_set_param(m, "atmo_sun_irradiance", p_sun_irradiance);
	rs->material_set_param(m, "atmo_moon_direction", p_moon_direction);
	rs->material_set_param(m, "atmo_moon_irradiance", p_moon_irradiance);
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
	ClassDB::bind_method(D_METHOD("set_atmosphere_model", "model"), &WeatherSkyMaterial::set_atmosphere_model);
	ClassDB::bind_method(D_METHOD("get_atmosphere_model"), &WeatherSkyMaterial::get_atmosphere_model);
	ClassDB::bind_method(D_METHOD("set_atmosphere_lights", "sun_direction", "sun_irradiance", "moon_direction", "moon_irradiance"), &WeatherSkyMaterial::set_atmosphere_lights);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "atmosphere_model", PROPERTY_HINT_ENUM, "Physical Sky,Scattering"), "set_atmosphere_model", "get_atmosphere_model");
	BIND_ENUM_CONSTANT(ATMOSPHERE_PHYSICAL_SKY);
	BIND_ENUM_CONSTANT(ATMOSPHERE_SCATTERING);
}

void WeatherSkyMaterial::cleanup() {
	for (Ref<Shader> &sh : shared_shaders) {
		sh.unref();
	}
	atmo_transmittance_tex.unref();
	atmo_multiscatter_tex.unref();
	WeatherClouds::free_textures();
}

WeatherSkyMaterial::WeatherSkyMaterial() {
	_update_shader();
	RS::get_singleton()->material_set_param(get_rid(), "cloud_shape", WeatherClouds::shape_texture()->get_rid());
	RS::get_singleton()->material_set_param(get_rid(), "cloud_detail", WeatherClouds::detail_texture()->get_rid());
}

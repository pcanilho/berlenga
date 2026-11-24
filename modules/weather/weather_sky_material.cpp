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
#include "bright_stars_data.h"

#include "core/config/project_settings.h"
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
Ref<ImageTexture> WeatherSkyMaterial::star_data_tex;
Ref<ImageTexture> WeatherSkyMaterial::star_cells_tex;

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
// Steps through the air under the cloud tops, each reading the clouds' shadow: shafts across the sky
// (0: no cloud shadow in the air); none in the radiance map.
uniform int atmo_cloud_shadow_steps : hint_range(0, 128) = 48;
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
// Optical depth toward the sun past the first 64 m, baked on the CPU over a box round the camera (Nubis Cubed's
// light voxel grid): one read in place of the coarse light march, and shadows reach as far as the box is tall.
// Set by WeatherMap3D; box = corner x, corner z, size, on.
uniform sampler3D cloud_light_grid : filter_linear, repeat_disable;
uniform vec4 cloud_light_grid_box = vec4(0.0);
uniform vec2 cloud_light_grid_y = vec2(0.0, 1.0);
uniform bool cloud_light_grid_enabled = true;
// Scale on the light the sunlit clouds bounce onto their neighbours (from the light grid; 0: none).
uniform float cloud_bounce : hint_range(0, 4) = 1.0;
// Clouds fade into the atmosphere from half this distance (m).
uniform float cloud_far : hint_range(1000, 200000, 100) = 40000.0;
uniform float cloud_brightness : hint_range(0, 8) = 1.0;
// Mean droplet diameter (um) for the cloud phase function: cumulus about 10 to 20.
uniform float cloud_droplet_size : hint_range(5, 50) = 15.0;
// Floor on the phase away from the sun, times the isotropic 1 / 4 pi (Bauer 2019).
uniform float cloud_back_scatter : hint_range(0, 1) = 0.5;
// The share of the cirrus' single scattering in the 22 degree ice halo (the 46 degree ring takes a fifth as much).
uniform float cloud_halo_strength : hint_range(0, 0.5) = 0.08;
// The share of the cirrus' single scattering in sun dogs (plates) and, at 0.6 of it, tangent arcs (columns), within the
// halo patches (oriented_halos()).
uniform float cloud_parhelia_strength : hint_range(0, 0.3) = 0.03;
// How much the billows toward the sun shade the scattered light (times their optical depth).
uniform float cloud_surface_occlusion : hint_range(0, 2) = 1.0;
uniform float cloud_sky_occlusion_reach : hint_range(0, 1000) = 250.0;
// Short steps inside cloud as a share of the long ones (1: no refinement).
uniform float cloud_refine : hint_range(0.1, 1) = 0.33;
// Iridescence on thin, newly formed wave cloud near the sun (diffraction by small uniform droplets): its strength.
uniform float cloud_iridescence : hint_range(0, 1) = 0.8;
group_uniforms;

group_uniforms weather_map; // Set by WeatherMap3D each update.
)";

const char *SKY_BODY = R"(
group_uniforms;

group_uniforms lightning; // Set by WeatherLightning3D while flashes last.
uniform int lightning_count = 0;
uniform vec4 lightning_flashes[4]; // world position, energy this frame
uniform vec3 lightning_color = vec3(0.82, 0.86, 1.0);

// The stars (Yale Bright Star Catalogue, to V 6.5 and a little past: what the eye sees). Each is a point of
// light spread over about a pixel (Schneegans, Kreskowski and Gerndt 2025: illuminance over a small area, so a
// star keeps its brightness at any resolution), looked up through a coarse cube-face grid of the sky
// (WeatherSkyMaterial::_bake_stars()).
uniform bool stars_enabled = true;
uniform float star_brightness : hint_range(0, 16) = 1.0;
uniform float star_glare : hint_range(0, 4) = 1.0;
// Airglow: the upper air's own faint light (oxygen's green 557.7 nm line, OH bands), about 22 V magnitudes a
// square arcsecond at the zenith of a dark site (1.2e-4 cd/m2 of the 2e-4 natural night sky), brightening toward
// the horizon by the van Rhijn factor for a 90 km layer (Jensen et al. 2001; Leinert et al. 1998).
uniform bool airglow_enabled = true;
// The Milky Way: a panorama in galactic coordinates (equirectangular, galactic centre in the middle, longitude
// increasing to the left, as ESO's eso0932a), read blurred to about half a degree so its photographed stars
// smear into the band and do not double the catalogue's; a linear texel of 1 is milky_way_luminance cd/m2 (the
// band's brightest clouds, about 20.5 V magnitudes a square arcsecond, are 7e-4 cd/m2). None by default.
uniform sampler2D milky_way : source_color, filter_linear_mipmap, repeat_enable, hint_default_black;
uniform float milky_way_luminance : hint_range(0, 0.01) = 1e-3;
uniform float milky_way_blur_lod : hint_range(0, 8) = 2.0;
uniform float airglow_luminance : hint_range(0, 0.01) = 1.2e-4; // cd/m2 at the zenith
// The moon's disc (Lommel-Seeliger, lit from the sun's direction: the phase and the terminator follow), earthshine
// on its dark side; its angular diameter from SkyClock (by distance).
uniform bool moon_disc_enabled = true;
uniform float atmo_moon_angular_size = 0.00904; // rad, diameter
uniform float star_energy_per_lux = 0.0; // set by SkyClock (0: no stars)
uniform mat3 star_sky_basis = mat3(1.0);
uniform sampler2D star_data : filter_nearest, repeat_disable;
uniform sampler2D star_cells : filter_nearest, repeat_disable;
group_uniforms;

const vec3 UP = vec3(0.0, 1.0, 0.0);
const float rayleigh_zenith_size = 8.4e3;
const float mie_zenith_size = 1.25e3;
const float PLANET_RADIUS = 6371000.0;
const float GRID_READ = 512.0; // m along the sun path to the light grid's read

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

// Draine's phase function (2003): Henyey-Greenstein times (1 + alpha cos^2) over its norm.
float draine(float cos_theta, float g, float alpha) {
	return henyey_greenstein(cos_theta, g) * (1.0 + alpha * cos_theta * cos_theta) / (1.0 + alpha * (1.0 + 2.0 * g * g) / 3.0);
}

// Cloud droplets' phase: Jendersie and d'Eon, "An Approximate Mie Scattering Function for Fog and Cloud
// Rendering" (SIGGRAPH 2023 Talks): HG for the narrow forward peak (the silver lining) plus Draine for the
// bulk, each fitted to the droplet diameter (um, 5 to 50; eq. 4 to 7). p_c narrows both for the octaves
// (Wrenninge). The fit leaves out the glory and fogbow, so the side away from the sun gets a floor
// (Bauer 2019): a dense cloud seen with the sun behind is lit by light it reflects, not by a dead lobe.
float cloud_phase(float cos_theta, float p_c) {
	float d = cloud_droplet_size;
	float g_hg = exp(-0.0990567 / (d - 1.67154));
	float g_d = exp(-2.20679 / (d + 3.91029) - 0.428934);
	float alpha = exp(3.62489 - 8.29288 / (d + 5.52825));
	float w_d = exp(-0.599085 / (d - 0.641583) - 0.665888);
	float ph = mix(henyey_greenstein(cos_theta, g_hg * p_c), draine(cos_theta, g_d * p_c, alpha), w_d);
	return max(ph, cloud_back_scatter / (4.0 * PI));
}

// The march's start: interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare", SIGGRAPH 2014), its pattern shifted every frame by 5.588238 pixels as his
// temporal variant does. The reprojection draws each pixel every 16th frame; a start shared by the whole
// frame (a per-frame step added to a fixed pattern) gave all the pixels drawn that frame the same bias,
// and with a handful of samples a pixel that showed as a lattice over each 4x4 block. A per-frame step of
// the golden ratio also nearly repeated at the 16-frame revisit (0.888 a revisit).
float march_jitter(vec2 p_pixel, float p_time) {
	vec2 p = p_pixel + 5.588238 * float(int(floor(p_time * 60.0)) % 64);
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// With the reprojection: each pixel is drawn every 16th frame (REPROJECTION_FRAME & 15 picks it), so its own
// starts are what its running mean averages. The same pattern for the same pixel each revisit (shifted by the
// slot, so the 16 pixels of a block differ), stepped on by the golden ratio per revisit: each pixel's starts
// are a low-discrepancy sequence (an R1 sequence; Roberts 2018) and its mean converges about as 1/N instead of
// 1/sqrt(N), so thin hard-edged cloud (a shelf, a far edge) stops sparkling after a few revisits.
float march_jitter_revisit(vec2 p_pixel, uint p_frame) {
	vec2 p = p_pixel + 5.588238 * float(p_frame & 15u);
	float ign = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
	return fract(ign + 0.618034 * float((p_frame >> 4u) & 1023u));
}

uniform float atmo_light_units = 3.14159265; // pi without physical light units, else 1

// Clouds along rd from the camera: radiance (rgb) and transmittance (a).
// p_pixel: the angle (rad) one pixel spans. The noise is read at the pixel's footprint, t * p_pixel,
// not at the march step: at grazing angles a step is hundreds of metres, and the shape noise read
// at that scale is a few texels across, whose trilinear blend thresholded is a box (flat faces,
// square corners). Along the ray the jitter (and the reprojection) average the steps instead.
// r_depth: the distance to the clouds seen, weighted by how much each step hides (for the air in front).
vec4 march_cumulus(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, int steps, float jitter, float p_pixel, out float r_depth) {
#ifdef ATMO_SCATTERING
	// The air in front hazes far clouds into the horizon (aerial perspective), so the deck runs out to it.
	const float far = 200000.0;
#else
	float far = cloud_far;
#endif
	r_depth = 0.0;
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	float r_base = PLANET_RADIUS + cloud_base_min;
	float r_top = PLANET_RADIUS + cloud_top_max;
	vec2 hb = sphere_hits(ro, rd, r_base);
	vec2 ht = sphere_hits(ro, rd, r_top);
	float t0;
	float t1;
	if (cam.y < cloud_base_min) {
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
	// Long grazing paths through the deck get more steps (up to three times, by the root of the length past
	// 5 km), so the far clouds near the horizon are not sampled kilometres apart into dithered, stepped
	// edges (Hillaire 2016 and Schneider's Nubis both scale the step count with the path).
	steps = int(clamp(float(steps) * sqrt((t1 - t0) / 5000.0), float(steps), float(steps) * 3.0));
	float ds = (t1 - t0) / float(steps);
	float cos_theta = dot(rd, sun_dir);
	// A low sun's light crosses the deck for kilometres: two more doubling steps (to 6.4 km) under 15 degrees,
	// so a cloud stands in its neighbours' shadow at sunset rather than every cloud lit whole.
	int light_steps = cloud_light_steps + (sun_dir.y < 0.26 ? 2 : 0);
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	// The view direction is fixed along the ray, so the phase of each octave is too.
	float phase0 = cloud_phase(cos_theta, 1.0);
	float phase1 = cloud_phase(cos_theta, 0.5);
	float phase2 = cloud_phase(cos_theta, 0.25);
	// Toward the sun the light inside a cloud turns isotropic and is absorbed more slowly (Nubis Cubed's inner
	// glow): the multiple scattering field's attenuation relaxes looking toward the sun.
	float toward_sun = smoothstep(0.0, 0.9, cos_theta);
	// Adaptive refinement (Schneider 2015; Hillaire 2016): long steps through clear air; on reaching cloud, one
	// step back and short steps (cloud_refine of a long one) while inside, back to long steps after four empty
	// short ones. Edges and the near shading are sampled metres apart instead of tens, so they come out crisp
	// rather than dithered, and the clear sky between clouds costs no more than before.
	float ds_fine = ds * cloud_refine;
	float t = t0 + jitter * ds;
	bool fine = false;
	int empty = 0;
	int max_iter = steps * 4;
	for (int i = 0; i < max_iter; i++) {
		if (t > t1) {
			break;
		}
		// Inside cloud the short step also stays under 0.4% of the distance (100 m at 25 km): at a grazing view the long
		// steps run to 600 m, a third of that still stepped through a thin hard-edged layer (a shelf, a far deck) in a
		// handful of samples, which no amount of jitter averaging settled into anything smoother than a grain.
		float st = fine ? min(ds_fine, max(40.0, 0.004 * t)) : ds;
		vec3 pl = ro + rd * t;
		vec3 p = vec3(cam.x + rd.x * t, length(pl) - PLANET_RADIUS, cam.z + rd.z * t);
		vec2 ct = wc_cover_type(p.xz);
		float profile;
		float sigma = wc_extinction_p(p, ct, true, min(st, t * p_pixel), profile);
		if (sigma <= 1e-6) {
			if (fine) {
				empty++;
				if (empty >= 4) {
					fine = false;
				}
			}
			t += st;
			continue;
		}
		if (!fine && cloud_refine < 1.0) {
			// The last long step was clear: go back to just after it and walk in short steps.
			fine = true;
			empty = 0;
			t = max(t - ds + ds_fine * (0.5 + 0.5 * jitter), t0);
			continue;
		}
		empty = 0;
		// Optical depth towards the sun. The first two samples (0 to 16 m, 16 to 64 m) read the eroded detail
		// (Nubis Cubed keeps its first two in the view march for this): each billow shades the next, so crests
		// stand lit over darker creases. Past them, steps doubling from 64 m read the shape alone (Frostbite's
		// growing distances). Only while the pixel still shows through: behind 85% cover nothing of it is seen.
		float tau_near = 0.0;
		float d = 0.0;
		int coarse = light_steps;
		float step_l = 25.0;
		if (transmittance > 0.15) {
			vec3 l0p = p + sun_dir * 8.0;
			vec3 l1p = p + sun_dir * 40.0;
			// The detail read at the billows' scale (64 m and up, four pixels' footprint far away): the billows
			// shade each other, while the finest noise shading itself showed as a grain over far cloud faces.
			float fp = max(64.0, 4.0 * min(st, t * p_pixel));
			tau_near = wc_extinction(l0p, ct, true, fp) * 16.0 + wc_extinction(l1p, wc_cover_type(l1p.xz), true, fp) * 48.0;
			d = 64.0;
			step_l = 64.0;
			coarse = light_steps - 1;
		}
		bool near_detailed = coarse < light_steps;
		float tau = tau_near;
		// Past 512 m (GRID_READ) the baked grid stands in for the rest of the coarse march: one read there, of
		// the optical depth from there out of the layer. Nearer, its 256 m texels would blur a small cloud's shading
		// of itself. Above the box the path has left the clouds.
		// Read on a level's centre: between levels the trilinear filter mixed the depths of paths a level apart, which
		// against a low sun start hundreds of metres apart along it, so a clear path took on its neighbours' clouds and
		// sunlit rims went dull. Reading where the path crosses a level's height leaves only the 256 m across.
		float grid_dy = (cloud_light_grid_y.y - cloud_light_grid_y.x) / float(textureSize(cloud_light_grid, 0).z);
		float grid_f = (p.y + sun_dir.y * GRID_READ - cloud_light_grid_y.x) / grid_dy - 0.5;
		float grid_d = GRID_READ + (ceil(grid_f) - grid_f) * grid_dy / max(sun_dir.y, 0.02);
		vec3 gp = p + sun_dir * grid_d;
		vec3 grid_uvw = vec3((gp.x - cloud_light_grid_box.x) / cloud_light_grid_box.z, (gp.z - cloud_light_grid_box.y) / cloud_light_grid_box.z, (gp.y - cloud_light_grid_y.x) / (cloud_light_grid_y.y - cloud_light_grid_y.x));
		bool grid = cloud_light_grid_enabled && cloud_light_grid_box.w > 0.5 && all(greaterThanEqual(grid_uvw, vec3(0.0))) && all(lessThanEqual(grid_uvw.xy, vec2(1.0)));
		if (grid) {
			coarse = 6;
			if (!near_detailed) {
				step_l = 64.0;
			}
		}
		for (int j = 0; j < coarse; j++) {
			if (grid) {
				if (d >= grid_d - 1.0) {
					break;
				}
				step_l = min(step_l, grid_d - d);
			}
			vec3 lp = p + sun_dir * (d + 0.5 * step_l);
			float dt = wc_extinction(lp, wc_cover_type(lp.xz), false, step_l) * step_l;
			tau += dt;
			// Behind the gate the first 75 m of the coarse march stand in for the near samples, so the shading
			// does not jump between neighbouring pixels whose jittered steps fall either side of the gate.
			if (!near_detailed && j < 2) {
				tau_near += dt;
			}
			d += step_l;
			step_l *= 2.0;
		}
		float grid_z = (ceil(grid_f) + 0.5) * grid_dy / (cloud_light_grid_y.y - cloud_light_grid_y.x);
		if (grid) {
			// The read level steps up by one where the read height crosses a level, a jump in the shading that drew a
			// sloping underside (an anvil's) in contour bands. So also read the next level up, a level's climb further
			// along the path (one march sample across the gap), and blend by where the height falls between them: at each
			// crossing both sides then read the same level at the same distance, and the shading is continuous.
			float k_l = grid_dy / max(sun_dir.y, 0.02);
			vec3 mp = p + sun_dir * (grid_d + 0.5 * k_l);
			float tau_b = tau + wc_extinction(mp, wc_cover_type(mp.xz), false, k_l) * k_l;
			vec3 gp_b = gp + sun_dir * k_l;
			vec2 uv_b = vec2((gp_b.x - cloud_light_grid_box.x) / cloud_light_grid_box.z, (gp_b.z - cloud_light_grid_box.y) / cloud_light_grid_box.z);
			float z_b = grid_z + grid_dy / (cloud_light_grid_y.y - cloud_light_grid_y.x);
			if (z_b < 1.0) {
				tau_b += textureLod(cloud_light_grid, vec3(clamp(uv_b, vec2(0.0), vec2(1.0)), z_b), 0.0).r;
			}
			float tau_a = tau + (grid_z < 1.0 ? textureLod(cloud_light_grid, vec3(grid_uvw.xy, grid_z), 0.0).r : 0.0);
			tau = mix(tau_b, tau_a, ceil(grid_f) - grid_f);
		}
		// Multiple scattering by Wrenninge's octaves: scattering, extinction and phase eccentricity
		// each halved per octave.
		vec3 light = sun_radiance * (exp(-tau) * phase0 + 0.5 * exp(-0.5 * tau) * phase1 + 0.25 * exp(-0.25 * tau) * phase2);
		// Light that has scattered many times: the diffuse field of a thick, non-absorbing layer in the
		// two-stream approximation (Bohren 1987). The down stream at optical depth tau carries
		// 1 / (1 + 3/4 (1 - g) tau) of the sun's flux, and the up stream about as much again near the lit
		// side, spread evenly over directions.
		// The light march reads the shape without its eroded edges, at steps doubling to hundreds of metres:
		// it finds about half the optical depth the body has (a cumulus is some 50 per kilometre, Stephens
		// 1978). With the march's own tau a cloud's shaded side kept two thirds of its lit side's light, so every
		// cloud was one flat tone, a cut-out; at twice it the shaded side darkens while an overcast deck (whose
		// diffuse light this also is) keeps most of its brightness (four times darkened decks to a storm).
		// Toward the sun deep inside, the field is absorbed more slowly (the inner glow).
		float glow = toward_sun * profile;
		float diffuse = 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau * mix(2.0, 1.4, glow));
		// Nubis's multiple scattering as a probability field (Schneider 2022, 2023): light has had room to
		// scatter many times only some way into a cloud, so the diffuse field fades toward its thin edge: dark
		// edges round the billows, as photographs of cumulus show. A deck's inside (profile near 1) keeps all of it.
		float ms_probability = mix(0.25, 1.0, smoothstep(0.0, 0.6, profile));
		// The billows between this point and the sun (the detailed near samples) shade the scattered light too, as
		// Nubis Cubed attenuates its scattering field by the summed density toward the sun: crests lit, creases dim.
		light += sun_radiance * diffuse * ms_probability * exp(-cloud_surface_occlusion * tau_near) * (2.0 / (4.0 * PI));
		// Sky light enters a cloud's outer shell only (Nubis's ambient approximation, pow(1 - profile, 0.5)),
		// with a floor for the light the cloud's own body passes on, so a deck's inside stays lit.
		float hf = clamp((p.y - wc_base_at(p.xz)) / wc_top_height(ct.y), 0.0, 1.0);
		// Sky light from above is cut by the cloud over the point (Nubis Cubed scales its ambient by the summed
		// density toward the sky; SunshineClouds2 reads the density one step up): one coarse read 100 m above,
		// so undersides and the lee of towers darken while the tops stay open to the sky.
		float up_tau = transmittance > 0.15 ? wc_extinction(p + vec3(0.0, 100.0, 0.0), ct, false, 100.0) * 100.0 : 2.0 * (1.0 - hf);
		float sky_vis = mix(0.5, 1.0, exp(-up_tau));
		// Sky visibility over a turret's reach (cloud_sky_occlusion_reach, 250 m): the transmittance toward four points
		// of the upper hemisphere (up, and 35 degrees up toward the camera and either side), as ambient occlusion is
		// estimated from a few density probes in production volume rendering (Wrenninge 2012; Nubis Cubed's ambient
		// from the summed density toward the sky). A crest sees open sky and stays bright, a crease between turrets sees
		// cloud on every side and darkens: the turrets stand in relief on a face the sun does not reach, where one read
		// straight up left a storm tower's shaded wall a single flat tone.
		if (cloud_sky_occlusion_reach > 0.0 && transmittance > 0.15) {
			float L = cloud_sky_occlusion_reach;
			vec3 hv = normalize(vec3(-rd.x, 0.0, -rd.z) + vec3(1e-4, 0.0, 0.0));
			vec3 sv = vec3(-hv.z, 0.0, hv.x);
			vec3 d1 = normalize(hv + vec3(0.0, 0.7, 0.0));
			vec3 d2 = normalize(sv + vec3(0.0, 0.7, 0.0));
			vec3 d3 = normalize(-sv + vec3(0.0, 0.7, 0.0));
			float v = 0.4 * exp(-wc_extinction(p + vec3(0.0, L, 0.0), ct, false, L) * L);
			v += 0.2 * exp(-wc_extinction(p + d1 * L, wc_cover_type((p + d1 * L).xz), false, L) * L);
			v += 0.2 * exp(-wc_extinction(p + d2 * L, wc_cover_type((p + d2 * L).xz), false, L) * L);
			v += 0.2 * exp(-wc_extinction(p + d3 * L, wc_cover_type((p + d3 * L).xz), false, L) * L);
			sky_vis = mix(0.15, 1.0, v);
		}
		light += mix(ambient_bottom, ambient_top, mix(0.4, 1.0, hf)) * mix(0.5, 1.0, sqrt(1.0 - profile)) * sky_vis * 1.4;
		// Light bounced off the sunlit clouds round the point (cloud-to-cloud interreflection): the light grid's green
		// is the sunlit share of the cloud within about 600 m (blurred on the CPU, as voxel cone tracing prefilters
		// its voxels, Crassin 2011), reflected at a cumulus' albedo-weighted 0.7 and spread like the diffuse term. It
		// lights the shaded side of a tower that faces a sunlit neighbour; it enters the shell as the sky's light does.
		vec3 bp = p - vec3(cloud_light_grid_box.x, cloud_light_grid_y.x, cloud_light_grid_box.y);
		vec3 b_uvw = vec3(bp.x / cloud_light_grid_box.z, bp.z / cloud_light_grid_box.z, bp.y / (cloud_light_grid_y.y - cloud_light_grid_y.x));
		if (cloud_bounce > 0.0 && cloud_light_grid_enabled && cloud_light_grid_box.w > 0.5 && all(greaterThanEqual(b_uvw, vec3(0.0))) && all(lessThanEqual(b_uvw, vec3(1.0)))) {
			light += sun_radiance * (cloud_bounce * 0.7 * textureLod(cloud_light_grid, b_uvw, 0.0).g * (2.0 / (4.0 * PI))) * mix(0.5, 1.0, sqrt(1.0 - profile));
		}
		// Point lights in the clouds (WeatherMap3D.add_cloud_light; Sunshine Clouds 2 lights its clouds with point
		// lights the same way, Bonkahe 2025): fires, a town's glow, flares lighting the cloud from below. Each within
		// its reach is marched three steps toward it over at most 600 m (lights sit under or in the cloud; the clear
		// air beyond adds nothing), its light falling off as the inverse square times a smooth window to the reach
		// (Karis 2013), forward-scattered through the thin part and diffused through the rest (two-stream, as the
		// sun's).
		for (int li = 0; li < 8; li++) {
			if (li >= cloud_light_count) {
				break;
			}
			vec3 lv = cloud_lights[li].xyz - p;
			float ld2 = dot(lv, lv);
			float reach = cloud_lights[li].w;
			if (ld2 >= reach * reach) {
				continue;
			}
			float ld = sqrt(ld2);
			vec3 to_l = lv / max(ld, 0.001);
			float seg = min(ld, 600.0) / 3.0;
			float tl = 0.0;
			for (int k = 0; k < 3; k++) {
				vec3 q = p + to_l * (seg * (float(k) + 0.5));
				tl += wc_extinction(q, wc_cover_type(q.xz), false, seg) * seg;
			}
			float win = clamp(1.0 - ld2 * ld2 / (reach * reach * reach * reach), 0.0, 1.0);
			vec3 e_l = cloud_light_colors[li] * (cloud_brightness * atmo_light_units) * (win * win / (ld2 + 100.0));
			float through = exp(-tl);
			light += e_l * (through * henyey_greenstein(dot(rd, to_l), 0.6) + max(1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tl) - through, 0.0) * (2.0 / (4.0 * PI)));
		}
		// Energy-conserving step (Hillaire 2016): albedo 1, so the scattering is the extinction.
		float tr = exp(-sigma * st);
		radiance += transmittance * light * (1.0 - tr);
		depth_sum += t * transmittance * (1.0 - tr);
		transmittance *= tr;
		if (transmittance < 0.01) {
			break;
		}
		t += st;
	}
	// Far clouds fade into the air.
	r_depth = transmittance < 1.0 ? depth_sum / (1.0 - transmittance) : t1;
	float fade = 1.0 - smoothstep(0.5 * far, far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

// Ice crystals' phase: Henyey-Greenstein at the asymmetry of bulk ice-cloud models, g about 0.75 (Baum et al.,
// "Bulk Scattering Properties for the Remote Sensing of Ice Clouds", J. Appl. Meteor. 2005), with the same
// floor away from the sun as the cumulus.
// Ice halos (Tape, "Atmospheric Halos", 1994; Greenler, "Rainbows, Halos and Glories"): randomly oriented
// hexagonal ice columns refract through 60 degree prism faces with a minimum deviation of about 22 degrees (21.7
// red to 22.4 blue: the ring's red inner edge), and through 90 degree faces at 46 degrees (a fainter, broader
// ring). No light is refracted inside the minimum, so each ring has a sharp inner edge and a tail outward. The
// colours overlap through the tail: a red inner rim, then white, far paler than a rainbow. A
// lobe each on the phase function, per channel, normalised over the sphere (2 pi sin(theta) over its width),
// taking the share cloud_halo_strength (22 degrees) and a fifth of it (46 degrees) of the single scattering.
float halo_ring(float theta, float peak, float width) {
	float x = theta - peak;
	float f = x < 0.0 ? exp(-x * x / (0.1 * width * width)) : exp(-x / width);
	return f / (2.0 * PI * sin(peak) * width * 1.28);
}

// Halos of oriented crystals (Tape 1994; Greenler; Bravais' effective index): plates fall with their c-axes near
// vertical, columns with theirs near horizontal. Refraction through their 60 degree prism faces then keeps the ray's
// component along the axis, so a ray tilted beta to the plane across the axis sees the prism with the effective index
// n' = sqrt(n^2 - sin^2 beta) / cos beta, and its minimum deviation D' = 2 asin(n' / 2) - 60 degrees in that plane.
// Plates (axis vertical, beta the sun's altitude): the sun dogs (parhelia), at the sun's altitude D' to either side of
// it, 22 degrees at the horizon and further out as the sun climbs, gone above about 61 degrees; red toward the sun,
// a white tail away. Columns (axis horizontal at every bearing): the upper and lower tangent arcs, touching the 22
// degree ring above and below the sun, their wings lifting as it climbs. For a view ray v, the one horizontal axis
// that keeps the component (along it) of v and of the sun equal is perpendicular to their difference, so the arc is
// one evaluation, weighted by how fast that component turns with the bearing (the density of axes there).
// Per channel n 1.307 (red) to 1.317 (blue), as the rings.
float min_dev(float n, float sb) {
	float cb2 = max(1.0 - sb * sb, 1e-4);
	float np = sqrt(max(n * n - sb * sb, 0.0) / cb2);
	float x = 0.5 * np;
	return x >= 1.0 ? -1.0 : 2.0 * asin(x) - PI / 3.0;
}

float oriented_lobe(vec3 v, vec3 s, vec3 a, float n) {
	float sb = dot(s, a);
	float dv = min_dev(n, sb);
	if (dv < 0.0) {
		return 0.0;
	}
	vec3 sp = s - a * sb;
	vec3 vp = v - a * dot(v, a);
	float l = length(sp) * length(vp);
	if (l < 1e-4) {
		return 0.0;
	}
	float ang = acos(clamp(dot(sp, vp) / l, -1.0, 1.0));
	const float W = 0.9 * PI / 180.0;
	float x = ang - dv;
	// A sharp red inner edge, the bright core falling off outward, and a faint white tail (the crystals tilted
	// further, Greenler 1980) that fades into the parhelic circle's start.
	return x < 0.0 ? exp(-x * x / (0.1 * W * W)) : 0.85 * exp(-x / W) + 0.15 * exp(-x / (8.0 * W));
}

vec3 oriented_halos(vec3 v, vec3 s, float p_plates, float p_columns) {
	vec3 o = vec3(0.0);
	const vec3 N = vec3(1.307, 1.311, 1.317);
	const float W = 0.9 * PI / 180.0;
	if (p_plates > 0.0) {
		// Plates wobble a degree or two off horizontal: the dogs' height spread.
		const float WOB = 1.6 * PI / 180.0;
		float db = asin(clamp(v.y, -1.0, 1.0)) - asin(clamp(s.y, -1.0, 1.0));
		float wb = exp(-db * db / (WOB * WOB));
		if (wb > 1e-3) {
			vec3 up = vec3(0.0, 1.0, 0.0);
			o += p_plates * wb * vec3(oriented_lobe(v, s, up, N.r), oriented_lobe(v, s, up, N.g), oriented_lobe(v, s, up, N.b)) / (W * 1.28 * WOB * 1.77);
		}
	}
	if (p_columns > 0.0) {
		vec2 w = v.xz - s.xz;
		float lw = length(w);
		if (lw > 1e-3) {
			vec3 a = vec3(-w.y, 0.0, w.x) / lw;
			// The density of axes giving that match: 1 / (pi |d(component)/d bearing|), capped at the arc's top.
			float dens = 1.0 / (PI * max(lw, 0.03));
			o += p_columns * dens * vec3(oriented_lobe(v, s, a, N.r), oriented_lobe(v, s, a, N.g), oriented_lobe(v, s, a, N.b)) / (W * 1.28 * 2.0 * PI);
		}
	}
	return o;
}

vec3 cirrus_phase(float cos_theta, float p_halo) {
	float base = max(henyey_greenstein(cos_theta, 0.75), cloud_back_scatter / (4.0 * PI));
	if (p_halo <= 0.0) {
		return vec3(base);
	}
	float th = acos(clamp(cos_theta, -1.0, 1.0));
	const float D = PI / 180.0;
	vec3 h22 = vec3(halo_ring(th, 21.75 * D, 1.6 * D), halo_ring(th, 22.0 * D, 1.6 * D), halo_ring(th, 22.2 * D, 1.6 * D));
	vec3 h46 = vec3(halo_ring(th, 45.6 * D, 2.5 * D), halo_ring(th, 46.0 * D, 2.5 * D), halo_ring(th, 46.6 * D, 2.5 * D));
	return vec3(base) * (1.0 - 1.2 * p_halo) + p_halo * (h22 + 0.2 * h46);
}

// The high layer: a thin sheet at cloud_high_height, so one read where the ray crosses it (Bauer 2019 draws
// cirrus the same way, a 2D layer with the clouds' shading). Its cover from the weather map's field, its
// fibres from the detail noise stretched along the wind aloft (cirrus streaks lie along the shear), its optical
// depth along the ray tau / cos. Single scattering with the sun's beam through the sheet plus a diffuse octave,
// and the sky's light, all at albedo 1.
vec4 march_cirrus(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, float p_pixel, out float r_t) {
	r_t = 0.0;
	if (cloud_high_cover <= 0.001 && cloud_storm_count == 0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	float r = PLANET_RADIUS + cloud_high_height;
	vec2 hits = sphere_hits(ro, rd, r);
	float t = cam.y < cloud_high_height ? hits.y : hits.x;
	if (t <= 0.0 || (cam.y < cloud_high_height && sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0)) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 pl = ro + rd * t;
	vec3 up = normalize(pl);
	vec2 xz = vec2(cam.x + rd.x * t, cam.z + rd.z * t);
	float c = wc_high_cover(xz);
	if (c <= 0.001) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// Fibres: the detail noise's wisps, 3.5 times longer along the wind than across it, read at the pixel's
	// footprint so the far sheet does not shimmer.
	vec2 q = xz - cloud_high_drift;
	vec2 dir = cloud_high_dir;
	vec2 aw = vec2(dot(q, dir), dot(q, vec2(-dir.y, dir.x)));
	// Fibres bend and hook (cirrus fibratus and uncinus): the read warped by a 20 km noise, so the streaks do not
	// all run parallel and converge on one vanishing point (which read as motion blur), at 3.5 to 1 rather than 8.
	aw += (vec2(wc_value_noise(q / 20000.0 + vec2(5.3, 1.7)), wc_value_noise(q / 20000.0 + vec2(11.9, 7.1))) - 0.5) * vec2(9000.0, 6000.0);
	vec3 fq = vec3(aw.x / 7000.0, aw.y / 2000.0, 0.37 + cloud_churn.z * 0.25);
	float lod = max(log2(t * p_pixel / (2000.0 / 128.0)), 0.0);
	vec4 dn = textureLod(cloud_detail, fq, lod);
	float fib = smoothstep(0.25, 0.85, mix(dn.r, dn.g, 0.4));
	// Fine fibrils inside each streak (a read 3.3 times finer, along the warped streak), so a streak has the
	// thread-like texture of hair-like ice trails and a crisp edge, not the smooth smear of one soft octave.
	float fine = textureLod(cloud_detail, vec3(fq.x * 3.3 + 0.41, fq.y * 3.3 + 0.13, fq.z + 0.19), lod + log2(3.3)).r;
	fib = clamp(fib * mix(0.45, 1.35, smoothstep(0.2, 0.8, fine)), 0.0, 1.0);
	// Cirrostratus: where the sheet is extensive (a warm front's shield, an anvil) the fibres merge into a veil,
	// still faintly fibrous (WMO Atlas Cs fibratus/nebulosus), and the halos are its own.
	float cs = smoothstep(0.55, 0.9, c);
	fib = mix(fib, 0.7 + 0.3 * fib, cs);
	// Cirrocumulus: under unstable shear (cloud_high_cc, from the Richardson number) patches of the thinner sheet
	// break into grains in ripples across the wind, about 200 m apart (near one degree seen from the ground):
	// the detail noise's billows, 1.8 km over 128 texels along the wind and 2.7 km across it, read half a mip soft.
	if (cloud_high_cc > 0.001) {
		float patch = smoothstep(0.45, 0.65, wc_value_noise((xz - cloud_high_drift) / 14000.0 + vec2(41.3, 7.9)));
		float ccs = cloud_high_cc * patch * (1.0 - cs);
		if (ccs > 0.001) {
			// No lattice (the texture tiles every 1.8 km): two reads at scales 1.37 apart and rotated 23 degrees,
			// blended by a 6 km noise, so the grain size wanders; both warped by the texture's own coarse octave
			// (domain warping) so rows bend, part and merge; and the threshold wanders on 2.5 km, so the ripples
			// break into gaps in places and run together in others (Atlas: Cc in patches, of irregular grains).
			vec2 aq = aw + (textureLod(cloud_detail, vec3(aw / 9000.0, 0.29), 0.0).rg - 0.5) * 700.0;
			vec2 aq2 = mat2(vec2(0.921, 0.391), vec2(-0.391, 0.921)) * aq * 1.37 + vec2(517.0, 1301.0);
			float glod = max(log2(t * p_pixel / (1800.0 / 128.0)) + 0.5, 0.0);
			vec4 ga = textureLod(cloud_detail, vec3(aq.x / 1800.0, aq.y / 2700.0, 0.83 + cloud_churn.z * 0.3), glod);
			vec4 gb = textureLod(cloud_detail, vec3(aq2.x / 1800.0, aq2.y / 2700.0, 0.47 + cloud_churn.z * 0.3), glod + 0.45);
			vec2 nq = (xz - cloud_high_drift) / 6000.0;
			float wsel = smoothstep(0.3, 0.7, wc_value_noise(nq + vec2(3.7, 19.1)));
			float gv = mix(mix(ga.b, ga.a, 0.35) * mix(0.8, 1.2, ga.r), mix(gb.b, gb.a, 0.35) * mix(0.8, 1.2, gb.r), wsel);
			float k = (wc_value_noise(nq * 2.4 + vec2(23.3, 5.1)) - 0.5) * 0.45;
			// Grains with a soft edge: ice grains fray, and a hard threshold drew them as blocks.
			float grain = smoothstep(0.3 + k, 0.85 + k, gv);
			fib = mix(fib, grain, ccs);
		}
	}
	float tau_v = cloud_high_tau * c * fib * 2.0 / max(abs(dot(rd, up)), 0.05);
	float tau_s = cloud_high_tau * c / max(dot(sun_dir, up), 0.05);
	float cos_theta = dot(rd, sun_dir);
	// Halos where the ice is columns falling at random: patches of sheet tens of kilometres across, so a ring shows
	// as arcs across some of the sky and not everywhere.
	float halo = cloud_halo_strength * mix(0.7, 1.4, cs) * smoothstep(0.35, 0.65, wc_value_noise((xz - cloud_high_drift) / 25000.0 + vec2(13.1, 71.7)));
	// Sun dogs and tangent arcs where the halo patches are (plates and columns falling steadily: thin, calm cirrus and
	// cirrostratus), cloud_parhelia_strength of the single scattering for the plates and 3 of it for the columns (fewer, but the arc gathers every axis in its plane).
	vec3 orient = cloud_parhelia_strength > 0.0 && halo > 0.0 ? oriented_halos(rd, sun_dir, cloud_parhelia_strength * halo / max(cloud_halo_strength, 1e-3), 3.0 * cloud_parhelia_strength * halo / max(cloud_halo_strength, 1e-3)) : vec3(0.0);
	vec3 light = sun_radiance * (exp(-0.5 * tau_s) * (cirrus_phase(cos_theta, halo) + orient) + (1.0 - exp(-0.5 * tau_s)) * (2.0 / (4.0 * PI))) + ambient_top;
	float tr = exp(-tau_v);
	r_t = t;
	// Far sheets fade into the air as the low clouds do.
#ifdef ATMO_SCATTERING
	float far = 200000.0;
#else
	float far = cloud_far * 2.0;
#endif
	float fade = 1.0 - smoothstep(0.5 * far, far, t);
	return vec4(light * (1.0 - tr) * fade, mix(1.0, tr, fade));
}

// Contrails (aircraft condensation trails). An engine's exhaust mixes with the cold air along a straight line in
// vapour pressure against temperature; a trail forms where that line crosses saturation over water, the
// Schmidt-Appleman criterion (Schumann 1996, "On conditions for contrail formation from aircraft exhausts", Meteorol.
// Z. 5): below about -40 C at cruise levels, from his fit T_LM = -46.46 + 9.43 ln(G - 0.053) + 0.72 ln^2(G - 0.053)
// (C; G the mixing line's slope, about 1.5 Pa/K at 250 hPa, scaling with the pressure). In air subsaturated over ice
// the trail sublimates within seconds to a minute: a short white line behind the aircraft. In ice-supersaturated air
// (the moist layers where cirrus forms, the high layer's humidity here) it persists for hours, spreading by shear
// and turbulence to kilometres wide while its ice grows, into contrail cirrus (Schumann 2005; Heymsfield et al. 2010:
// optical depth 0.1 to 0.5, widths of 1 to 3 km after half an hour). The flights are procedural: cloud_contrail_traffic
// lanes of straight tracks crossing the sky at 9.5 to 12 km and 240 m/s, each slot flown every 20 minutes, and each
// trail drifts with the wind at its height.
uniform float cloud_contrail_traffic : hint_range(0, 4) = 1.0;
uniform vec2 cloud_contrail_wind = vec2(0.0);
uniform float cloud_clock = 0.0;

float contrail_hash(float a, float b) {
	return fract(sin(a * 127.1 + b * 311.7) * 43758.5453);
}

vec4 march_contrails(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, float p_pixel, out float r_t) {
	r_t = 0.0;
	if (cloud_contrail_traffic <= 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	if (cam.y < 9000.0 && sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// The tracks pass near the camera's 40 km cell, so the sky round it always has its traffic.
	vec2 cell = floor(cam.xz / 40000.0);
	vec2 centre = (cell + 0.5) * 40000.0;
	float tau_sum = 0.0;
	float t_sum = 0.0;
	float cos_theta = dot(rd, sun_dir);
	const float T = 1200.0;
	const float V = 240.0;
	for (int k = 0; k < 12; k++) {
		float fk = float(k) + 17.0 * cell.x + 131.0 * cell.y;
		if (contrail_hash(fk, 3.7) > 0.6 * cloud_contrail_traffic) {
			continue;
		}
		// Three flights of the slot: the current and the two before (their persistent trails still in the sky).
		float phase = contrail_hash(fk, 9.1) * T;
		float jn = floor((cloud_clock + phase) / T);
		for (int m = 0; m < 3; m++) {
			float j = jn - float(m);
			float h = 9500.0 + 2500.0 * contrail_hash(fk + j, 1.3);
			float th = 6.2831853 * contrail_hash(fk + j, 2.9);
			vec2 dir = vec2(cos(th), sin(th));
			vec2 perp = vec2(-dir.y, dir.x);
			vec2 o = centre + perp * ((contrail_hash(fk + j, 4.1) - 0.5) * 60000.0) + dir * ((contrail_hash(fk + j, 5.3) - 0.5) * 40000.0);
			float t_c = j * T - phase + contrail_hash(fk + j, 6.7) * T; // when it passes o
			// The air there (the weather's lapse rate, the tropopause's -56.5 C floor) and its humidity: the dry upper
			// troposphere, moist where the high layer's humidity is (wc_wave_depression's high branch).
			float tc = max(cloud_air.x - 0.0065 * (h - cloud_air.z), -56.5);
			float p_pa = 101325.0 * exp(-h / 8000.0);
			float g = 1.5 * p_pa / 25000.0;
			float lg = log(max(g - 0.053, 0.05));
			float t_lm = -46.46 + 9.43 * lg + 0.72 * lg * lg;
			// Where the ray crosses this flight's level.
			vec2 hits = sphere_hits(ro, rd, PLANET_RADIUS + h);
			float t = cam.y < h ? hits.y : hits.x;
			if (t <= 0.0 || t > 150000.0) {
				continue;
			}
			vec3 pl = ro + rd * t;
			float mu = abs(dot(rd, normalize(pl)));
			vec2 x = vec2(cam.x + rd.x * t, cam.z + rd.z * t);
			// The trail's age at x: the aircraft flew past x - u a seconds ago (u the wind at its height).
			vec2 u = cloud_contrail_wind;
			float along = dot(x - o, dir);
			float a = (V * (cloud_clock - t_c) - along) / max(V - dot(u, dir), 1.0);
			if (a < 1.0) {
				continue;
			}
			// Over ice: e_w / e_i grows about 1 % per degree below 0 C.
			float ice_w = exp(0.0097 * max(-tc, 0.0));
			// The humidity over ice at cruise levels: about 60 to 70 % on average, with ice-supersaturated regions some
			// hundreds of kilometres across over about 15 % of the sky (Gierens et al. 1999, MOZAIC; Spichtinger et al.
			// 2003): a 60 km noise round a mean that the ground's humidity raises, and the high layer's moist patches
			// (wc_wave_depression's high branch) where its cirrus forms.
			float n_r = wc_value_noise((x - cloud_high_drift) / 60000.0 + vec2(2.1, 7.3));
			float rh_i = 0.5 + 0.4 * cloud_air.y + 0.8 * (n_r - 0.5);
			if (cloud_high_cover > 0.001) {
				float n = wc_cover_noise_at(x + vec2(9137.0, 3301.0) - (cloud_high_drift - cloud_drift), cloud_high_scale);
				float rh_l = wc_wave_layer_rh(n, cloud_high_cover, wc_layer_cover_n(n, cloud_high_cover, x, true)) * ice_w;
				float d = (h - cloud_high_height) / 2500.0;
				rh_i = mix(rh_i, max(rh_i, rh_l), exp(-d * d) * min(1.0, 4.0 * cloud_high_cover));
			}
			if (tc > t_lm + 2.0 * min(rh_i / ice_w, 1.0)) {
				continue; // too warm for a trail
			}
			float persist = smoothstep(0.98, 1.05, rh_i);
			float life = mix(clamp(10.0 / max(1.0 - rh_i, 0.03), 10.0, 300.0), 2.0 * T, persist);
			if (a > life) {
				continue;
			}
			float lat = dot(x - o - u * a, perp);
			// Spreading: tens of metres young, about 1 m/s wider in supersaturated air (shear and turbulence).
			float sig = sqrt(25.0 * 25.0 + pow(mix(0.15, 1.0, persist) * a, 2.0));
			// Each pixel's footprint widens the line (a far trail stays a thin line, not a vanishing one).
			float foot = t * p_pixel;
			float sig_v = sqrt(sig * sig + foot * foot);
			float tau_c = 0.3 * (25.0 / sig) * (1.0 + persist * min(a / 900.0, 3.0)) * (1.0 - smoothstep(0.7 * life, life, a));
			// Column optical depth across the line, kept as the footprint blurs it (spread, not lost).
			float tau = tau_c * (sig / sig_v) * exp(-0.5 * lat * lat / (sig_v * sig_v)) / max(mu, 0.08);
			if (tau > 1e-4) {
				tau_sum += tau;
				t_sum += tau * t;
			}
		}
	}
	if (tau_sum <= 1e-4) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	r_t = t_sum / tau_sum;
	// Lit as the cirrus: ice, forward scattering, a diffuse share, the sky's light.
	float tau_s = 0.3 / max(sun_dir.y, 0.05);
	vec3 light = sun_radiance * (exp(-0.5 * tau_s) * cirrus_phase(cos_theta, 0.0) + (1.0 - exp(-0.5 * tau_s)) * (2.0 / (4.0 * PI))) + ambient_top;
	float tr = exp(-tau_sum);
	return vec4(light * (1.0 - tr), tr);
}

// The mid layer's density (1/m) at p in its slab: cloudlets from the detail noise's billows (inverted Worley,
// cells of a few hundred metres) in rows across the wind (altocumulus forms in rolls across the shear, Ludlam's
// cellular convection), thresholded by the cover so a thin cover is scattered cloudlets and a full cover a
// sheet (altostratus); each cloudlet rounded top and bottom. At full cover the slab's vertical optical depth
// is about cloud_mid_tau.
float wc_mid_density(vec3 p, float c, float p_foot, out float r_profile) {
	r_profile = 0.0;
	float h = (p.y - (cloud_mid_height - 0.5 * cloud_mid_thickness)) / cloud_mid_thickness;
	if (h <= 0.0 || h >= 1.0 || c <= 0.001) {
		return 0.0;
	}
	vec2 q = p.xz - cloud_mid_drift;
	vec2 dir = cloud_mid_dir;
	vec2 aw = vec2(dot(q, dir), dot(q, vec2(-dir.y, dir.x)));
	// 128 texels over 2.4 km along the wind, 3.6 km across: the billows' 8-cell octave gives cloudlets about
	// 300 by 450 m, the rolls lying across the wind.
	// The texture tiles every 2.4 by 3.6 km, and over a full sky of cloudlets the repeats lined up into rows and columns
	// (a grid, seen most at a low sun). Broken up as Quilez's texture-repetition fixes do: the read warped by up to
	// 450 m over 5 km, and each region reading its own depth of the 3D texture (a slice chosen by a 7 km noise), so no
	// two tiles show the same cloudlets in the same place.
	vec2 wq = aw / 5200.0;
	vec2 warp = (vec2(wc_value_noise(wq + vec2(13.1, 4.7)), wc_value_noise(wq + vec2(2.9, 17.3))) - 0.5) * 900.0;
	float zs = wc_value_noise(aw / 7000.0 + vec2(31.3, 8.1));
	vec3 fq = vec3((aw.x + warp.x) / 2400.0, (aw.y + warp.y) / 3600.0, 0.61 + 0.17 * h + cloud_churn.z * 0.2 + 0.9 * zs);
	float lod = max(log2(p_foot / (2400.0 / 128.0)), 0.0);
	// Near, the texels (20 to 30 m) are a cloudlet's size or more of it, and a thin cover keeps only the billows' peaks:
	// linear filtering's kinks at the texel borders cut them into boxes. Smoothed between texels (Quilez, "improved
	// texture interpolation"): the same one read, C1 across texels, while the footprint is under a texel.
	vec3 xs = fq * 128.0 + 0.5;
	vec3 xi = floor(xs);
	vec3 xf = xs - xi;
	vec3 fq_s = (xi + xf * xf * (3.0 - 2.0 * xf) - 0.5) / 128.0;
	vec4 dn = textureLod(cloud_detail, mix(fq_s, fq, smoothstep(0.0, 1.0, lod)), lod);
	// Worley cells are evenly spaced and alike, so a field of them still read as a lattice. Real altocumulus come in
	// groups and gaps, large and small together: the cover read through a two-octave noise of 1.8 km and 700 m (the
	// cloudlets thicken into clusters and thin out between them), and the mix of the billows' fine octave varies with
	// it (big rounded cloudlets in one group, small ragged ones in the next).
	float g = 0.65 * wc_value_noise(aw / 1800.0 + vec2(5.1, 23.7)) + 0.35 * wc_value_noise(aw / 700.0 + vec2(17.9, 3.3));
	float g2 = wc_value_noise(aw / 2600.0 + vec2(41.3, 9.9));
	float n = mix(dn.b, dn.a, mix(0.1, 0.6, g2));
	c = clamp(c * mix(0.1, 1.5, smoothstep(0.25, 0.75, g)), 0.0, 1.0);
	float prof = 4.0 * h * (1.0 - h);
	// A thin cover thins the cloudlets and no longer shrinks them to the billows' highest peaks: those peaks are
	// the fine octaves' cells, a few texels (20 to 30 m each) across, and drew blocks shaped like letters.
	float ce = max(c, 0.3);
	float d = smoothstep(0.0, 1.0, clamp((n * mix(0.6, 1.0, prof) - (1.0 - ce)) / ce, 0.0, 1.0)) * smoothstep(0.0, 0.35, prof) * smoothstep(0.0, 0.3, c);
	r_profile = d;
	return d * cloud_mid_tau / (0.45 * cloud_mid_thickness);
}

// The mid layer: a slab cloud_mid_thickness deep, marched in a few steps across it (Microsoft Flight Simulator
// and Unreal's layered volumetric clouds draw such layers as their own thin volumes), lit as the cumulus are:
// Wrenninge's octaves under the droplets' phase, the two-stream diffuse field and the sky's light, with the
// sun's optical depth from two samples toward it through the slab.
vec4 march_altocumulus(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, float jitter, float p_pixel, out float r_t) {
	r_t = 0.0;
	if (cloud_mid_cover <= 0.001 && cloud_storm_count == 0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	float y0 = cloud_mid_height - 0.5 * cloud_mid_thickness;
	float y1 = cloud_mid_height + 0.5 * cloud_mid_thickness;
	vec2 hb = sphere_hits(ro, rd, PLANET_RADIUS + y0);
	vec2 ht = sphere_hits(ro, rd, PLANET_RADIUS + y1);
	float t0;
	float t1;
	if (cam.y < y0) {
		if (sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = hb.y;
		t1 = ht.y;
	} else if (cam.y < y1) {
		t0 = 0.0;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	} else {
		if (ht.x < 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = ht.x;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	}
#ifdef ATMO_SCATTERING
	float far = 200000.0;
#else
	float far = cloud_far * 2.0;
#endif
	if (t0 > far || t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// Grazing paths run kilometres through the slab: at most 8 km of it, which hides all behind anyway.
	t1 = min(t1, t0 + 8000.0);
	const int n = 8;
	float ds = (t1 - t0) / float(n);
	float cos_theta = dot(rd, sun_dir);
	float phase0 = cloud_phase(cos_theta, 1.0);
	float phase1 = cloud_phase(cos_theta, 0.5);
	float phase2 = cloud_phase(cos_theta, 0.25);
	float sun_up = max(sun_dir.y, 0.05);
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	for (int i = 0; i < n; i++) {
		float t = t0 + (float(i) + jitter) * ds;
		vec3 pl = ro + rd * t;
		vec3 p = vec3(cam.x + rd.x * t, length(pl) - PLANET_RADIUS, cam.z + rd.z * t);
		// The cover read through a 300 m warp: its contours run straight across a cloudlet (they bend over kilometres),
		// and a cloudlet the cover's edge crossed was cut along a straight line, as if boxed.
		vec2 cw = (p.xz - cloud_mid_drift) / 300.0;
		float c = wc_mid_cover(p.xz + 400.0 * (vec2(wc_value_noise(cw + vec2(2.3, 9.1)), wc_value_noise(cw + vec2(7.7, 4.4))) - 0.5));
		float foot = max(t * p_pixel, 0.25 * ds);
		float profile;
		float sigma = wc_mid_density(p, c, foot, profile);
		if (sigma <= 1e-6) {
			continue;
		}
		float pr;
		float tau = wc_mid_density(p + sun_dir * 30.0, c, foot, pr) * 60.0 + wc_mid_density(p + sun_dir * 110.0, c, foot, pr) * 100.0;
		// The rest of the way out of the slab toward the sun, past the neighbouring cloudlets. A broken layer is not a
		// uniform medium: the light threads the gaps, so over each cloudlet spacing (about 700 m) it passes with
		// 1 - c + c exp(-tau_cell), raised to the number of spacings (the binary-mixture result for a broken layer;
		// Pomraning 1991, Barker and Davies 1992). At their mean density instead, a low sun's 7 km slant through the slab
		// left every cloudlet in a sunset sky dark grey, where photographs show them lit gold and pink, the nearer
		// side of each brightest.
		float rest = max((y1 - p.y) / sun_up - 160.0, 0.0);
		float tau_cell = 700.0 * cloud_mid_tau / (0.45 * cloud_mid_thickness);
		tau += -(rest / 700.0) * log(max(1.0 - c + c * exp(-tau_cell), 1e-6));
		vec3 light = sun_radiance * (exp(-tau) * phase0 + 0.5 * exp(-0.5 * tau) * phase1 + 0.25 * exp(-0.25 * tau) * phase2);
		float diffuse = 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau * 2.0);
		light += sun_radiance * diffuse * mix(0.25, 1.0, smoothstep(0.0, 0.6, profile)) * exp(-cloud_surface_occlusion * min(tau, 4.0)) * (2.0 / (4.0 * PI));
		float hf = clamp((p.y - y0) / cloud_mid_thickness, 0.0, 1.0);
		light += mix(ambient_bottom, ambient_top, hf) * mix(0.35, 1.0, sqrt(1.0 - profile)) * 1.4;
		float tr = exp(-sigma * ds);
		radiance += transmittance * light * (1.0 - tr);
		depth_sum += transmittance * (1.0 - tr) * t;
		transmittance *= tr;
		if (transmittance < 0.01) {
			break;
		}
	}
	r_t = transmittance < 1.0 ? depth_sum / (1.0 - transmittance) : t0;
	float fade = 1.0 - smoothstep(0.5 * far, far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

// Virga and fallstreaks under the mid layer: ice crystals falling from its thicker cloudlets at about 1 m/s
// (Heymsfield 1975, "Cirrus uncinus generating cells and the evolution of cirriform clouds", J. Atmos. Sci. 32) and
// sublimating in the drier air below. A crystal's radius squared shrinks linearly with the fall in subsaturated air
// (diffusional growth run backwards; Pruppacher and Klett), so the trail's extinction, carried by the radius squared,
// tapers linearly to nothing at the fall length L, which grows as the air below nears saturation. The wind changes
// with height under the layer, so a crystal that fell d metres has drifted (du/dz) d^2 / (2 v) from its cell:
// a parabola, the hooked trail of Marshall (1953, "Precipitation trajectories and patterns", J. Meteor. 10).
// Only glaciated layers shed much (colder than about -10 C at the base); the cell they fell from is found by
// following that parabola back up, so the streaks keep the cloudlets' grouping and the fine streaks follow the hook.
uniform float cloud_virga : hint_range(0, 3) = 1.0;

vec3 virga_geometry() {
	// x: the fall length L (m), y: the shed share (ice), z: the base height.
	float y0 = cloud_mid_height - 0.5 * cloud_mid_thickness;
	float tb = cloud_air.x - 0.0065 * (y0 - cloud_air.z);
	float ice = mix(0.12, 1.0, 1.0 - smoothstep(-14.0, -5.0, tb));
	float zm = 0.5 * y0;
	float rh = clamp(cloud_air.y * (exp(-max(zm - cloud_air.z, 0.0) / 8000.0) - 0.02) / 0.98, 0.05, 0.97);
	// Subsaturated over ice, not water (ice saturates at about 1.1 times lower vapour at -10 C).
	float s_ice = min(1.1 * rh, 0.97);
	float len = clamp(330.0 / (1.0 - s_ice), 250.0, 3000.0);
	return vec3(min(len, y0 - 50.0), ice, y0);
}

vec4 march_virga(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, float jitter, float p_pixel, out float r_t) {
	r_t = 0.0;
	if (cloud_virga <= 0.0 || cloud_mid_cover <= 0.02) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 g = virga_geometry();
	float len = g.x;
	float y0 = g.z;
	float y1 = y0 - len;
	if (len < 100.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	vec2 hb = sphere_hits(ro, rd, PLANET_RADIUS + y1);
	vec2 ht = sphere_hits(ro, rd, PLANET_RADIUS + y0);
	float t0;
	float t1;
	if (cam.y < y1) {
		if (sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = hb.y;
		t1 = ht.y;
	} else if (cam.y < y0) {
		t0 = 0.0;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	} else {
		if (ht.x < 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = ht.x;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	}
#ifdef ATMO_SCATTERING
	float far = 120000.0;
#else
	float far = cloud_far * 1.5;
#endif
	if (t0 > far || t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	t1 = min(t1, t0 + 12000.0);
	const int n = 12;
	float ds = (t1 - t0) / float(n);
	// The wind at the base and below it: the mid layer's way (its speed about 1.6 times the low wind's plus 4 m/s,
	// winds growing with height) against the low wind (the rain's slant over the drops' 7 m/s). Its change per metre
	// over the depth from the base to the ground, and 1 m/s crystals: the drift is a d^2.
	vec2 u_low = cloud_rain_slant * 7.0;
	vec2 u_mid = cloud_mid_dir * (1.6 * length(u_low) + 4.0);
	vec2 hook = (u_low - u_mid) / max(y0, 500.0) * 0.5;
	float sigma_cell = cloud_mid_tau / (0.45 * cloud_mid_thickness);
	float cos_theta = dot(rd, sun_dir);
	float ph = 0.6 * henyey_greenstein(cos_theta, 0.7) + 0.4 / (4.0 * PI);
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	for (int i = 0; i < n; i++) {
		float t = t0 + (float(i) + jitter) * ds;
		vec3 pl = ro + rd * t;
		vec3 p = vec3(cam.x + rd.x * t, length(pl) - PLANET_RADIUS, cam.z + rd.z * t);
		float d = y0 - p.y;
		if (d <= 0.0 || d >= len) {
			continue;
		}
		// The cell these crystals fell from, up the parabola.
		vec2 src = p.xz - hook * d * d;
		// Which cells shed: patches of a few kilometres (fallstreaks come from some cloudlets, not all).
		float shed = smoothstep(0.4, 0.7, wc_value_noise((src - cloud_mid_drift) / 3500.0 + vec2(19.3, 6.1)));
		if (shed <= 0.0) {
			continue;
		}
		float c = wc_mid_cover(src);
		float foot = max(t * p_pixel, 0.25 * ds);
		float prof;
		float sg = wc_mid_density(vec3(src.x, y0 + 0.4 * cloud_mid_thickness, src.y), c, foot, prof);
		if (sg <= 1e-6) {
			continue;
		}
		// Thicker cloudlets shed; the trail tapers as the crystals sublimate (radius squared linear in the fall), with
		// a short onset under the base where the crystals leave the cloud.
		float taper = (1.0 - d / len) * smoothstep(0.0, 60.0, d);
		// Streaks: the cells' own fine structure carried down the hook (a few tens of metres across, the trails of
		// single generating turrets), read at the source so they bend with it.
		vec2 sq = (src - cloud_mid_drift) / 140.0;
		float fib = mix(0.45, 1.55, wc_value_noise(sq + vec2(3.3, 8.8)) * 0.65 + wc_value_noise(sq * 2.7 + vec2(1.9, 4.2)) * 0.35);
		float sigma = cloud_virga * g.y * shed * sigma_cell * smoothstep(0.05, 0.45, prof) * taper * fib;
		if (sigma <= 1e-7) {
			continue;
		}
		// Thin: the sun reaches most of it; the layer above shades the sky light from above a little.
		vec3 light = sun_radiance * ph + mix(ambient_bottom, ambient_top, 0.55) * 1.1;
		float tr = exp(-sigma * ds);
		radiance += transmittance * light * (1.0 - tr);
		depth_sum += transmittance * (1.0 - tr) * t;
		transmittance *= tr;
	}
	r_t = transmittance < 1.0 ? depth_sum / (1.0 - transmittance) : t0;
	float fade = 1.0 - smoothstep(0.5 * far, far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

// Wave clouds (lenticulars): the slab where mountain waves lift moist air past saturation, marched
// (the field is fixed to the ground and smooth: 16 coarse steps over at most 8 km of path, refined in cloud). The lens is laminar: its
// edge is where the lift meets saturation, roughened only by thin striations along the wind (the moist sheets the
// air carries through it). Lit as the mid layer: Wrenninge's octaves, two-stream diffuse, the sky's light.
// Cloud iridescence (Gedzelman and Lock 2003, "Simulating coronas in color", Applied Optics 42; Laven's MiePlot):
// light diffracted round droplets of radius a spreads as the Airy pattern [2 J1(v) / v]^2, v = 2 pi a sin(theta) /
// lambda, so at a given angle from the sun each wavelength sits on a different part of its rings. Where the droplets
// are small and alike (just condensed, at a lens' leading edge: 1 to 6 um) the colours show as pastel bands 10 to
// 30 degrees from the sun, following the lens' outline as the droplets grow inward. Returned as the colour's ratio to
// its own luminance, so it tints the forward-scattered sunlight without brightening it.
float bessel_j1(float x) {
	if (x < 3.0) {
		float x2 = x * x;
		return x * (0.5 - x2 * (1.0 / 16.0 - x2 * (1.0 / 384.0 - x2 / 18432.0)));
	}
	float ph = x - 2.35619449;
	return sqrt(0.63661977 / x) * (cos(ph) - 0.375 / x * sin(ph));
}

vec3 iridescence_airy(float p_sin, float p_radius_um) {
	vec3 v = 6.2831853 * p_radius_um * p_sin / vec3(0.65, 0.55, 0.45);
	vec3 j = vec3(bessel_j1(v.r), bessel_j1(v.g), bessel_j1(v.b));
	return 4.0 * j * j / max(v * v, vec3(1e-4));
}

vec3 iridescence_tint(float p_sin, float p_radius_um) {
	// Over the droplets' spread of sizes (about 8 % at a lens's edge; Gedzelman and Lock 2003 find the colours
	// need a narrow but not single size): three sizes, so the higher orders, whose rings crowd together as the size
	// grows, average to pastel instead of drawing every small change of size as a new hard ring (zebra contours).
	vec3 airy = 0.25 * iridescence_airy(p_sin, 0.92 * p_radius_um) + 0.5 * iridescence_airy(p_sin, p_radius_um) + 0.25 * iridescence_airy(p_sin, 1.08 * p_radius_um);
	float lum = dot(airy, vec3(0.2126, 0.7152, 0.0722));
	// Pastel in photographs, but the lens's depth along the ray mixes droplet sizes and greys them: the
	// difference from white is raised by 2.2 (with the bands spread over the edge's first 15 % of depth, not its
	// last metres, which drew the hard fringe; 1.6 left none visible).
	vec3 tint = airy / max(lum, 1e-6);
	return clamp(vec3(1.0) + 2.2 * (tint - vec3(1.0)), vec3(0.15), vec3(2.2));
}

vec4 march_wave(vec2 slab, vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, float jitter, float p_pixel, out float r_t) {
	r_t = 0.0;
	float y0 = slab.x;
	float y1 = slab.y;
	if (y1 <= y0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 ro = vec3(0.0, PLANET_RADIUS + cam.y, 0.0);
	vec2 hb = sphere_hits(ro, rd, PLANET_RADIUS + y0);
	vec2 ht = sphere_hits(ro, rd, PLANET_RADIUS + y1);
	float t0;
	float t1;
	if (cam.y < y0) {
		if (sphere_hits(ro, rd, PLANET_RADIUS).x > 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = hb.y;
		t1 = ht.y;
	} else if (cam.y < y1) {
		t0 = 0.0;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	} else {
		if (ht.x < 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = ht.x;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	}
	// The field covers its square only (half its size round the camera).
	float far = 0.5 * cloud_wave_rect.z * 1.2;
	if (t0 > far || t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// Up to 16 km through the slab (an 8 km cap hid lenses further along the ray), in a fixed number of coarse steps
	// (a count varying from pixel to pixel stepped the plates' gaps).
	t1 = min(t1, min(t0 + 16000.0, far));
	const int n = 32;
	float ds = (t1 - t0) / float(n);
	float cos_theta = dot(rd, sun_dir);
	float phase0 = cloud_phase(cos_theta, 1.0);
	float phase1 = cloud_phase(cos_theta, 0.5);
	float phase2 = cloud_phase(cos_theta, 0.25);
	vec2 wdir = cloud_mid_dir;
	vec3 radiance = vec3(0.0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	// Coarse steps find where the lift comes within 150 m of saturation (the moisture, the costly read, once a
	// step); there four fine samples resolve the lens, whose edge is thinner than a coarse step at grazing angles.
	for (int i = 0; i < n; i++) {
		float tc = t0 + (float(i) + 0.5) * ds;
		vec3 pc = vec3(cam.x + rd.x * tc, length(ro + rd * tc) - PLANET_RADIUS, cam.z + rd.z * tc);
		if (pc.y < y0 - 0.5 * ds || pc.y > y1 + 0.5 * ds) {
			continue;
		}
		// The largest lift over the whole step (its middle and both ends): from the middle alone a steep plate's end,
		// lifted past saturation at an end of the step but not at its middle, was skipped.
		float ta = t0 + float(i) * ds;
		float tb = ta + ds;
		vec3 pa = vec3(cam.x + rd.x * ta, length(ro + rd * ta) - PLANET_RADIUS, cam.z + rd.z * ta);
		vec3 pb = vec3(cam.x + rd.x * tb, length(ro + rd * tb) - PLANET_RADIUS, cam.z + rd.z * tb);
		float eta_c = max(wc_wave_eta(pc), max(wc_wave_eta(pa), wc_wave_eta(pb)));
		if (eta_c <= -200.0) {
			continue;
		}
		float dep = wc_wave_depression(pc);
		if (eta_c - 125.0 * dep < -200.0) {
			continue;
		}
		bool ambient_done = false;
		vec3 ambient_lens = vec3(0.0);
		// The lens's edge placed between samples (as an analytic coverage): the lift past saturation x is smooth along the
		// ray, so where it changes sign between two samples the cloud starts (or ends) at the linear crossing, and only that
		// share of the interval is cloud. A whole interval either way drew the rim as a dithered gradient one fine step
		// (tens of metres) deep, which under a near lens spread over many pixels.
		bool have_prev = false;
		float x_prev = 0.0;
		float sig_prev = 0.0;
		vec3 l_prev = vec3(0.0);
		float h = 0.125 * ds;
		// Eight fine samples, jittered per revisit (the reprojection's R1 sequence averages them over 12 revisits): four
		// fixed ones stepped a plate's end and a pile's 100 m gaps into stairs and dots.
		for (int k = 0; k < 8; k++) {
			float t = t0 + (float(i) + (float(k) + jitter) * 0.125) * ds;
			vec3 p = vec3(cam.x + rd.x * t, length(ro + rd * t) - PLANET_RADIUS, cam.z + rd.z * t);
			if (p.y < y0 || p.y > y1) {
				have_prev = false;
				continue;
			}
			// Striations: the detail noise stretched along the wind, a few tens of metres on the saturation level.
			vec2 aw = vec2(dot(p.xz, wdir), dot(p.xz, vec2(-wdir.y, wdir.x)));
			float foot = max(t * p_pixel, 0.03125 * ds);
			float lod = max(log2(foot / (2400.0 / 128.0)), 0.0);
			// Read at 4 km by 2.4 km: the rim wanders smoothly by a few metres of lift. Finer or more stretched, the thin
			// ends of the plates were cut into comb teeth along the wind.
			float st = textureLod(cloud_detail, vec3(aw.x / 4000.0, aw.y / 2400.0, p.y / 1200.0), max(lod - 1.5, 0.0)).b;
			float eta = wc_wave_eta(p);
			float x = wc_plate_x(eta - 125.0 * dep, p) + 10.0 * (st - 0.5);
			if (x <= 0.0) {
				if (have_prev && x_prev > 0.0) {
					// Leaving the cloud: the share of the interval before the crossing, lit as the last sample inside.
					float tr_x = exp(-sig_prev * h * x_prev / (x_prev - x));
					radiance += transmittance * l_prev * (1.0 - tr_x);
					depth_sum += transmittance * (1.0 - tr_x) * t;
					transmittance *= tr_x;
				}
				have_prev = true;
				x_prev = x;
				continue;
			}
			float sigma = wc_wave_sigma(p, eta, x);
			if (sigma <= 0.0) {
				have_prev = false;
				continue;
			}
			// Fibres: the water in thin sheets tens of metres deep, drawn out along the wind (the lens's silky, striated
			// look in photographs), as the detail noise stretched 2.5:1 along the wind and flattened 3:1 in height. Read at
			// the pixel's footprint, so far lenses keep their mean and only near ones show the grain; without it a near
			// lens was a soft blur.
			float fib = textureLod(cloud_detail, vec3(aw.x / 3000.0, aw.y / 1200.0, p.y / 170.0), max(log2(foot / (1200.0 / 128.0)), 0.0)).g;
			float profile = clamp(x / 150.0, 0.0, 1.0);
			// Only in the thin rim, and gently: the lens is laminar (smooth, glassy in photographs; Ludlam 1980, Houze
			// 2014), its striations faint where the cloud is a few optical depths thin. A 0.3 to 1.7 grain stretched 6:1
			// through the whole sheet drew it combed, like brushed hair, seen from under a near lens.
			sigma *= mix(1.0, mix(0.75, 1.25, fib), 1.0 - smoothstep(0.1, 0.5, profile));
			float lr;
			float dr;
			// The sun's light at each sample (once a coarse step held a plate's end at one value for 60 m and stepped
			// it): through the whole lens toward the sun (a lens is 200 to 800 m deep: three reads to 700 m), so a belly
			// under a high sun is shaded by the cloud over it and a crown is lit.
			float tau = wc_wave_extinction_d(p + sun_dir * 40.0, 0.0, dep, lr, dr) * 80.0 + wc_wave_extinction_d(p + sun_dir * 170.0, 0.0, dep, lr, dr) * 180.0 + wc_wave_extinction_d(p + sun_dir * 450.0, 0.0, dep, lr, dr) * 380.0;
			// The forward-scattered sunlight carries the diffraction colours (the forward lobes, where single scattering
			// by the droplets rules): droplets from 1 um where the air has just saturated to 6 um 150 m past it, on thin
			// cloud only (many scatterings wash them out), 6 to 35 degrees from the sun. The size is set by the depth in from
			// the edge, so the bands follow the outline; it wanders only a little over a few km (2.5 um over 700 m drew its
			// contours across the edge as zebra bands).
			vec3 irid = vec3(1.0);
			if (cloud_iridescence > 0.0 && cos_theta > 0.82) {
				float s_th = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
				float w_ir = cloud_iridescence * smoothstep(0.76, 0.87, cos_theta) * smoothstep(0.0, 0.15, profile) * (1.0 - smoothstep(0.45, 0.95, profile)); // in from the very edge (the lens's last metres drew a hard fringe), out before the thick core
				irid = mix(vec3(1.0), iridescence_tint(s_th, mix(1.5, 5.0, smoothstep(0.0, 150.0, x)) + 0.6 * wc_value_noise(p.xz / 2500.0 + vec2(5.3, 1.7))), w_ir);
			}
			vec3 light = sun_radiance * ((exp(-tau) * phase0 + 0.5 * exp(-0.5 * tau) * phase1) * irid + 0.25 * exp(-0.25 * tau) * phase2);
			float diffuse = 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau * 2.0);
			light += sun_radiance * diffuse * mix(0.25, 1.0, smoothstep(0.0, 0.6, profile)) * (2.0 / (4.0 * PI));
			if (!ambient_done) {
				// The sky's light, once a coarse step: from above through the cloud over the point, from below off the
				// ground (as the cumulus' hemisphere reads, Nubis's ambient through the density above).
				float tau_up = wc_wave_extinction_d(p + vec3(0.0, 80.0, 0.0), 0.0, dep, lr, dr) * 160.0 + wc_wave_extinction_d(p + vec3(0.0, 300.0, 0.0), 0.0, dep, lr, dr) * 280.0;
				ambient_lens = ambient_top * (1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau_up * 2.0)) + ambient_bottom * 0.5;
				ambient_done = true;
			}
			vec3 l = light + ambient_lens * mix(0.35, 1.0, sqrt(1.0 - profile)) * 1.4;
			float cover = have_prev && x_prev <= 0.0 ? x / (x - x_prev) : 1.0; // entering: from the crossing on
			float tr = exp(-sigma * h * cover);
			have_prev = true;
			x_prev = x;
			sig_prev = sigma;
			l_prev = l;
			radiance += transmittance * l * (1.0 - tr);
			depth_sum += transmittance * (1.0 - tr) * t;
			transmittance *= tr;
		}
		if (transmittance < 0.01) {
			break;
		}
	}
	r_t = transmittance < 1.0 ? depth_sum / (1.0 - transmittance) : t0;
	float fade = 1.0 - smoothstep(0.7 * far, far, t0);
	return vec4(radiance * fade, mix(1.0, transmittance, fade));
}

// Over: a layer b behind a (radiance adds through a's transmittance), and the depth the air in front is taken
// to, weighted by what each hides.
vec4 cloud_over(vec4 a, float da, vec4 b, float db, out float r_depth) {
	vec4 o = vec4(a.rgb + a.a * b.rgb, a.a * b.a);
	r_depth = ((1.0 - a.a) * da + a.a * (1.0 - b.a) * db) / max(1.0 - o.a, 1e-4);
	return o;
}

)";

// The storms' rain and snow shafts and the lake-effect veils: shared by the sky and by the pass that draws them
// over the scene (WeatherMap3D's distant_shafts pass), each marched from p_t_min to no further than p_t_max.
const char *SHAFTS_GLSL = R"(
// Rain shafts under the storms' bases, seen from afar (near the camera the rain's own haze and drops draw it).
// Rain's visible extinction from its rate, sigma = 0.31 R^0.67 per km (Atlas 1953; the visibility in rain), with the
// storm's Gaussian rain core (as the map's rain field) broken into curtains by two octaves of noise, slanted
// downwind as the drops fall (about 7 m/s). Lit by the light under the storm (the sky's light below the clouds,
// darkened under the storm's own shield) and by the sun where it reaches below the base, scattered forward by
// the drops (g 0.6); albedo 1.
vec4 march_rain_shafts(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_bottom, float jitter, float p_t_min, float p_t_max, out float r_t) {
	r_t = 0.0;
	vec3 col = vec3(0.0);
	float tr = 1.0;
	float wsum = 0.0;
	float dsum = 0.0;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		float rain = cloud_storm_rain[i];
		if (rain < 0.5) {
			continue;
		}
		vec4 s = cloud_storms[i];
		float base = (cloud_storm_base[i] > 0.0 ? cloud_storm_base[i] : cloud_base_height);
		// Snow (the weather's rain/snow split under the storm) falls at about 1 m/s against rain's 7 (SnowFall3D's
		// fall speeds; nearer 2 m/s through the stirred air under the base), so its shaft trails 3.5 times further downwind: slanting veils.
		float snow = cloud_storm_snow[i];
		vec2 slant = cloud_rain_slant * mix(1.0, 3.5, snow); // about 2 m/s through the stirred air under the base
		// The chord of the ray through a column 2.2 core radii wide (centred where the drops at mid-height are).
		vec2 c = s.xy - slant * (0.5 * base);
		vec2 o2 = cam.xz - c;
		vec2 d2 = rd.xz;
		float a = dot(d2, d2);
		if (a < 1e-6) {
			continue;
		}
		float rad = 2.2 * s.z + length(slant) * 0.5 * base;
		float b = dot(o2, d2);
		float disc = b * b - a * (dot(o2, o2) - rad * rad);
		if (disc <= 0.0) {
			continue;
		}
		float sq = sqrt(disc);
		float t0 = max((-b - sq) / a, p_t_min);
		float t1 = min((-b + sq) / a, p_t_max);
		if (rd.y > 0.0) {
			t1 = min(t1, (base - cam.y) / rd.y);
		} else if (cam.y > base) {
			t0 = max(t0, (base - cam.y) / rd.y);
		}
		if (t1 <= t0) {
			continue;
		}
		const int n = 24;
		float dt = (t1 - t0) / float(n);
		for (int k = 0; k < n; k++) {
			float t = t0 + (float(k) + jitter) * dt;
			vec3 p = cam + rd * t;
			if (p.y > base || p.y < -50.0) {
				continue;
			}
			// Where the drops at p left the base: upwind by the slant times their fall.
			vec2 q = p.xz - slant * (base - p.y);
			vec2 dq = (q - s.xy) / (0.9 * s.z); // the heavy core about 1.8 radii across, under the tower
			float r2 = dot(dq, dq);
			// A core with an edge (heavy rain falls in shafts a few km across, not a smooth bell).
			float r = rain * exp(-mix(r2 * r2, r2, snow)); // snow a soft-edged veil
			if (r < 0.05) {
				continue;
			}
			vec2 qn = q - cloud_drift;
			// Curtains: three octaves (1.2 km, 400 m, 130 m), thresholded so lighter gaps open between the
			// heavier sheets; the finest stretched across the view so its streaks stand upright.
			float curt = 0.5 * wc_value_noise(qn / 1200.0 + vec2(float(i) * 13.7, 3.1)) + 0.3 * wc_value_noise(qn / 400.0 + vec2(7.3, float(i) * 5.9)) + 0.2 * wc_value_noise(qn / 130.0 + vec2(1.9, 8.3));
			// Snow's veils are softer than rain's curtains (flakes drift and spread as they fall).
			r *= mix(0.0, 2.6, smoothstep(mix(0.45, 0.38, snow), mix(0.62, 0.72, snow), curt)) * smoothstep(0.0, mix(120.0, 500.0, snow), base - p.y);
			// Snow's extinction: SnowFall3D.snowfall_extinction (Gunn and Marshall flakes) gives about 2 km of
			// visibility at 1 mm/h of water and 200 m at 3 mm/h (Rasmussen et al. 1999): fitted as 0.00196 r^2.1,
			// capped at 5 mm/h (a snowstorm's heaviest). Snow hides far more than rain at the same rate.
			float sigma = mix(0.31e-3 * pow(r, 0.67), 0.00196 * pow(min(r, 5.0), 2.1), snow);
			// The light: under the storm's shield the sky's light is cut to about a third; the sun where the cover
			// toward it over the base is thin.
			float shield = exp(-r2 / (1.6 * 1.6)) * s.w;
			vec2 qs = p.xz + sun_dir.xz / max(sun_dir.y, 0.05) * (base - p.y);
			float sun_vis = exp(-6.0 * wc_cover_type(qs).x) * step(0.0, sun_dir.y);
			float g = 0.6;
			float mu = dot(rd, sun_dir);
			float hg = (1.0 - g * g) / (4.0 * PI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
			// Under the shield the sky's light is cut to about two fifths: a heavy shaft stands as a dark grey curtain
			// against the brighter sky behind it.
			// Snow is white (flakes scatter nearly all they intercept, many times over in a dense veil): it keeps about
			// three quarters of the sky's light under the shield and stands as a pale veil, where rain is a dark curtain.
			vec3 light = ambient_bottom * mix(1.0, mix(0.4, 0.75, snow), shield) + sun_radiance * sun_vis * hg;
			float st = exp(-sigma * dt);
			float w = tr * (1.0 - st);
			col += light * w;
			dsum += t * w;
			wsum += w;
			tr *= st;
		}
	}
	r_t = wsum > 0.0 ? dsum / wsum : 0.0;
	return vec4(col, tr);
}

// Lake-effect snow under its bands (wc_lake_effect), seen from afar (out to 60 km): the weather's snowfall there (4 mm/h of water
// at full strength, WeatherMap3D's cells), its visibility as the storms' snow (Rasmussen et al. 1999), in veils
// trailing downwind as the flakes fall at about 1 to 2 m/s. Near the camera (under 1.5 km) the falling snow's own
// particles draw it.
vec4 march_lake_snow(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_bottom, float jitter, float p_t_min, float p_t_max, out float r_t) {
	r_t = 0.0;
	float base = cloud_base_height;
	float t0 = max(1500.0, p_t_min);
	float t1 = min(60000.0, p_t_max); // a band is seen from tens of kilometres aside
	if (rd.y > 0.001) {
		t1 = min(t1, (base - cam.y) / rd.y);
	} else if (rd.y < -0.001) {
		t1 = min(t1, (-50.0 - cam.y) / rd.y);
	}
	if (cam.y > base) {
		if (rd.y >= -0.001) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = max(t0, (base - cam.y) / rd.y);
	}
	if (t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// The weather's snow share (rain_fraction(): Jennings et al. 2018).
	float e = -10.04 + 1.41 * cloud_air.x + 9.0 * cloud_air.y;
	float snow = 1.0 / (1.0 + exp(clamp(e, -60.0, 60.0)));
	vec2 slant = cloud_rain_slant * mix(1.0, 3.5, snow);
	// Steps spread out with distance (the near ones matter most). Over the scene (p_t_min > 0) twice as many: that pass
	// has no temporal resolve to average its jitter, so its noise stood as a fixed grain of stripes.
	int n = p_t_min > 0.0 ? 32 : 16;
	vec3 col = vec3(0.0);
	float tr = 1.0;
	float wsum = 0.0;
	float dsum = 0.0;
	float g = 0.6;
	float mu = dot(rd, sun_dir);
	float hg = (1.0 - g * g) / (4.0 * PI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
	for (int k = 0; k < n; k++) {
		float fa = (float(k) + jitter) / float(n);
		float fb = (float(k) + 1.0 + jitter) / float(n);
		float ta = t0 * pow(t1 / t0, fa);
		float t = t0 * pow(t1 / t0, (float(k) + 0.5 + jitter) / float(n));
		float dt = t0 * pow(t1 / t0, fb) - ta;
		vec3 p = cam + rd * t;
		if (p.y > base || p.y < -50.0) {
			continue;
		}
		vec2 q = p.xz - slant * (base - p.y);
		vec2 qn = q - cloud_drift;
		// The veil's edge wanders a kilometre or so (fall streaks spread and drift); a band's own edge read straight
		// down stood as a sheer wall.
		vec2 wander = 1500.0 * (vec2(wc_value_noise(qn / 4000.0 + vec2(2.1, 9.4)), wc_value_noise(qn / 4000.0 + vec2(7.7, 0.3))) - 0.5);
		float lake = wc_lake_effect(q + wander);
		if (lake < 0.02) {
			continue;
		}
		// Curtains: three octaves (2.5 km, 800 m, 250 m), thresholded so clearer gaps open between the heavier sheets (as
		// the storms' shafts). The density is the same all the way down, so the sheets stand upright by themselves; the
		// finest octave stretched along the view (to stand its streaks up) made the pattern follow the camera, and each
		// pixel column a ray down one long streak: vertical smear columns and blocky stripes along the horizon.
		float curt = 0.5 * wc_value_noise(qn / 2500.0 + vec2(23.1, 4.7)) + 0.3 * wc_value_noise(qn / 800.0 + vec2(3.3, 17.9)) + 0.2 * wc_value_noise(qn / 250.0 + vec2(11.9, 6.2));
		// Under the base the snow starts thin (the flakes growing as they fall out of it): the base shows over the veil.
		float r = 4.0 * lake * mix(0.0, 2.2, smoothstep(0.42, 0.68, curt)) * smoothstep(100.0, 700.0, base - p.y);
		float sigma = mix(0.31e-3 * pow(r, 0.67), 0.00196 * pow(min(r, 5.0), 2.1), snow);
		vec2 qs = p.xz + sun_dir.xz / max(sun_dir.y, 0.05) * (base - p.y);
		float sun_vis = exp(-6.0 * wc_cover_type(qs).x) * step(0.0, sun_dir.y);
		vec3 light = ambient_bottom * mix(1.0, mix(0.4, 0.75, snow), lake) + sun_radiance * sun_vis * hg;
		float st = exp(-sigma * dt);
		float w = tr * (1.0 - st);
		col += light * w;
		dsum += t * w;
		wsum += w;
		tr *= st;
		if (tr < 0.01) {
			break;
		}
	}
	r_t = wsum > 0.0 ? dsum / wsum : 0.0;
	return vec4(col, tr);
}


// Haboob dust walls (docs/ideas/clouds.md, "Haboob dust wall in the cloud march"): the mineral dust layer
// WeatherMap3D publishes (weather_dust_map over weather_map_rect: R the column in g/m2, G the top above the ground
// in m with the density current's raised nose, B the head share 0..1), marched as a participating medium from the
// ground to its top. Its profile is the contract's, densest at the ground: c = col / (Hs (1 - e^-3)) exp(-h / Hs),
// Hs = top / 3, none above the top. At the head (B) the top is a density current's (Simpson 1997, "Gravity
// Currents"): lobes and clefts along the front about a head height or two apart, Kelvin-Helmholtz billows rolling
// on its top, the nose lifted a little off the ground by friction. Mass extinction and albedo from
// weather_dust_optics and weather_dust_albedo (0.6 m2/g; 0.95/0.89/0.75, g 0.75): the march returns grey light weights
// for the green albedo, and dust_tint() turns them brown by the albedo's colour over the scattering orders.
global uniform sampler2D weather_dust_map;
global uniform vec4 weather_dust_near; // where PhysX's Flow billows draw the head near the camera: x, z, radius (0: none), height
global uniform vec4 weather_map_rect;
global uniform vec4 weather_dust_optics;
global uniform vec4 weather_dust_albedo;
global uniform float weather_ground_height;
global uniform sampler2D weather_terrain_map;
global uniform vec4 weather_terrain_rect;
global uniform vec4 weather_terrain_info;
uniform vec4 cloud_dust = vec4(0.0); // the highest top over the map (m above the ground), the largest column (g/m2); 0: none

float dust_ground(vec2 xz) {
	if (weather_terrain_info.z > 0.0 && weather_terrain_rect.z > 0.0) {
		vec2 u = (xz - weather_terrain_rect.xy) / weather_terrain_rect.zw;
		if (u.x >= 0.0 && u.y >= 0.0 && u.x <= 1.0 && u.y <= 1.0) {
			return textureLod(weather_terrain_map, u, 0.0).w + weather_terrain_info.x;
		}
	}
	return weather_ground_height;
}

// The map read as a cubic B-spline (four bilinear reads; Sigg and Hadwiger 2005): its cells are ~200 m, and read
// bilinearly a wall's face and top were runs of straight facets (as the lake band's were).
vec4 dust_map(vec2 xz) {
	if (weather_map_rect.z <= 0.0) {
		return vec4(0.0);
	}
	vec2 uv = (xz - weather_map_rect.xy) / weather_map_rect.zw;
	if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) {
		return vec4(0.0);
	}
	vec2 n = vec2(textureSize(weather_dust_map, 0));
	vec2 st = uv * n - 0.5;
	vec2 i0 = floor(st);
	vec2 f = st - i0;
	vec2 f2 = f * f;
	vec2 f3 = f2 * f;
	vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	vec2 w3 = f3 / 6.0;
	vec2 g0 = w0 + w1;
	vec2 g1 = w2 + w3;
	vec2 c0 = (i0 - 1.0 + w1 / g0 + 0.5) / n;
	vec2 c1 = (i0 + 1.0 + w3 / g1 + 0.5) / n;
	return max(g0.y * (g0.x * textureLod(weather_dust_map, c0, 0.0) + g1.x * textureLod(weather_dust_map, vec2(c1.x, c0.y), 0.0)) + g1.y * (g0.x * textureLod(weather_dust_map, vec2(c0.x, c1.y), 0.0) + g1.x * textureLod(weather_dust_map, c1, 0.0)), vec4(0.0));
}

// g/m3 at p from a map read m (the contract's profile), with the head's shape.
float dust_density(vec3 p, vec4 m, float p_foot) {
	float col = m.r;
	float top = m.g;
	if (col <= 1e-3 || top <= 1.0) {
		return 0.0;
	}
	float h = p.y - dust_ground(p.xz);
	if (h < 0.0) {
		return 0.0;
	}
	// The head share rises over a cell or two to the front; read through the B-spline it peaks under 1, so a third
	// already counts as the head.
	float head = smoothstep(0.0, 0.4, m.b);
	// Lobes and clefts along the front: a head height or two apart (Simpson 1997), the top raised or cut by a quarter.
	float lobe = wc_value_noise(p.xz / max(1.5 * top, 300.0) + vec2(41.7, 13.3));
	float top_eff = top * (1.0 + head * 0.5 * (lobe - 0.5));
	if (h > 1.15 * top_eff) {
		return 0.0;
	}
	// Billows: the detail noise's billow octaves rolling over the head's top and face (the cauliflower in photographs of haboobs),
	// read at the pixel's footprint so a far wall keeps its mean.
	float lod = max(log2(p_foot / (600.0 / 128.0)), 0.0);
	vec4 dn = textureLod(cloud_detail, vec3(p.x / 600.0, p.y / 450.0, p.z / 600.0), lod);
	float billow = dn.b * 0.65 + dn.a * 0.35;
	float hs = top_eff / 3.0;
	float c = col / (hs * (1.0 - exp(-3.0))) * exp(-h / hs);
	// The head is stirred through its depth (a density current's head is turbulent, well mixed, Simpson 1997): its
	// dust nearer uniform to the top, the column kept, so it stands as a wall with a face and a top rather than a haze.
	c = mix(c, col / max(top_eff, 1.0), 0.7 * head);
	// The wall's outline eroded by the billows: the shape (how far inside the dust the point is, across the map and up
	// to the top) less the detail noise, remapped (Schneider 2015, Nubis's cloud erosion). The dust is a few optical
	// depths across, so billows inside it averaged away along the ray; cut into its face and top they stand out as
	// cauliflower lobes and clefts. The inside (shape near 1) keeps its density.
	// Across the map the shape is the column's own optical depth (a couple of g/m2 is about one): read against the
	// largest column, a heavy head eroded the wake behind it and the dust over the ground near the camera away.
	float shape = smoothstep(0.0, 3.0, col) * sqrt(clamp(1.0 - h / (1.1 * top_eff), 0.0, 1.0));
	// Strongest at the edge and fading inward: eroded as deeply inside, the top read as holes punched into the dust
	// rather than lobes standing out of it.
	float e = (0.2 + 0.5 * head) * billow * (1.0 - 0.7 * shape);
	c *= clamp((shape - e) / max(1.0 - e, 1e-3), 0.0, 1.0) / max(shape, 1e-3);
	// The nose lifted off the ground by friction: the foot trails it by a few per cent of the head's height.
	c *= 1.0 - 0.5 * head * (1.0 - smoothstep(0.0, 0.06 * top, h));
	// Near the camera the Flow billows (PhysXHaboobFront3D) are the head up to their height: the wall steps back there,
	// fading over the outer fifth of their cylinder and the top fifth of their height, so the two never stack.
	if (weather_dust_near.z > 0.0) {
		float r = length(p.xz - weather_dust_near.xy);
		float inside = (1.0 - smoothstep(0.8 * weather_dust_near.z, weather_dust_near.z, r)) * (1.0 - smoothstep(0.8 * weather_dust_near.w, weather_dust_near.w, h));
		c *= 1.0 - inside;
	}
	return c;
}

// g/m3 at p: the map read where the wall's lobes have carried it. The face's outline is the map's, round and
// smooth (a ring of 200 m cells); billows inside it average away along a ray grazing the face, so the outline itself
// is warped (domain warping, Quilez 2002): bulges a few hundred metres out, a couple of kilometres apart, changing
// with height so the face leans and bulges as a head's lobes do.
float dust_at(vec3 p, float p_foot) {
	vec2 q = p.xz / 2200.0 + vec2(p.y / 1600.0, -p.y / 2100.0);
	vec2 w = vec2(wc_value_noise(q + vec2(3.1, 7.7)), wc_value_noise(q + vec2(11.4, 2.9))) - 0.5;
	return dust_density(p, dust_map(p.xz + w * 800.0), p_foot);
}

// The dust's colour over its many scatterings: a wall is optically thick (tau 5 to 20 across), and a thick medium's
// diffuse reflectance is R = (1 - s) / (1 + s), s = sqrt((1 - a) / (1 - a g)) (the two-stream result with the
// similarity relations), so blue, absorbed a little more at each scattering, comes out well under red: brown, not the
// single albedo's near-grey. Normalised to the green the march used.
vec3 dust_tint() {
	vec3 a = weather_dust_albedo.rgb;
	vec3 s = sqrt(max(vec3(1.0) - a, vec3(0.0)) / (vec3(1.0) - a * weather_dust_albedo.w));
	vec3 r = (vec3(1.0) - s) / (vec3(1.0) + s);
	return r / max(r.g, 1e-4);
}

// Along rd from p_t_min to p_t_max: radiance (grey weights, see dust_tint()) and transmittance. The dust at the
// camera's own point is left out (sand's haze draws that uniformly round the camera), so a camera inside the wall
// sees only how much denser the dust ahead is.
vec4 march_dust(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, float jitter, float p_pixel, float p_t_min, float p_t_max, out float r_t) {
	r_t = 0.0;
	if (cloud_dust.x <= 0.0 || weather_dust_optics.x <= 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	float y_top = weather_ground_height + 1.3 * cloud_dust.x + 1500.0; // room for ground up to 1.5 km above the reference
	float t0 = max(p_t_min, 1.0);
	float t1 = min(p_t_max, 60000.0);
	if (rd.y > 1e-4) {
		if (cam.y > y_top) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t1 = min(t1, (y_top - cam.y) / rd.y);
	} else if (cam.y > y_top) {
		if (rd.y > -1e-4) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t0 = max(t0, (y_top - cam.y) / rd.y);
	}
	if (t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	float c_cam = dust_at(cam, 1.0);
	float sigma_m = weather_dust_optics.x * dot(weather_dust_optics.yzw, vec3(0.2126, 0.7152, 0.0722));
	float alb = weather_dust_albedo.g;
	float g = weather_dust_albedo.w;
	float mu = dot(rd, sun_dir);
	float ph0 = (1.0 - g * g) / (4.0 * PI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
	// The many scatterings in a thick wall, by the similarity relations (van de Hulst; Joseph, Wiscombe and Weinman
	// 1976): the forward peak counted as unscattered, so the light diffuses through a reduced depth f tau, f = 1 - a g,
	// isotropically. Its strength is set so a deep wall reflects what the two-stream model gives,
	// R = (1 - s) / (1 + s), s = sqrt((1 - a) / (1 - a g)): single scattering of an isotropic source over a reduced
	// depth returns k / (4 (1 + f)) of it, so k = 4 R (1 + f). Wrenninge's octaves kept the forward peak in every
	// order: seen with the sun behind, a wall a few optical depths deep came out at a sixth of that, a dull grey face.
	float f_ms = 1.0 - alb * g;
	float s_ms = sqrt((1.0 - alb) / f_ms);
	float r_ms = (1.0 - s_ms) / (1.0 + s_ms);
	float k_ms = 4.0 * r_ms * (1.0 + f_ms);
	// Where along the ray the map holds dust: a coarse scan of the column (log-spaced, 48 reads of the map only), then
	// the march over that stretch alone in even steps. Spread log over the whole ray, a wall a kilometre deep 5 km out
	// got a step or two and read as a soft haze, its lobes and billows lost between them.
	float t_start = max(t0, 20.0);
	float ratio = t1 / t_start;
	float t_in = -1.0;
	float t_out = -1.0;
	float prev = t_start;
	for (int k = 0; k <= 48; k++) {
		float tk = t_start * pow(ratio, float(k) / 48.0);
		vec3 pk = cam + rd * tk;
		if (dust_map(pk.xz).r > 1e-3) {
			if (t_in < 0.0) {
				t_in = prev;
			}
			t_out = min(t_start * pow(ratio, float(k + 1) / 48.0), t1);
		}
		prev = tk;
	}
	if (t_in < 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// The outline is warped up to 400 m off the map's (dust_at()).
	t_in = max(t_in - 400.0, t0);
	t_out = min(t_out + 400.0, t1);
	// Steps of 30 to 150 m: the wall's face is lit within its first few hundred metres (a few optical depths), and its
	// billows are some 600 m across, so coarser steps blurred them away; a deep stretch past 14 km is cut short.
	const int n = 96;
	float dt = clamp((t_out - t_in) / float(n), 30.0, 150.0);
	t_out = min(t_out, t_in + dt * float(n));
	vec3 col = vec3(0.0);
	float tr = 1.0;
	float wsum = 0.0;
	float dsum = 0.0;
	for (int k = 0; k < n; k++) {
		float t = t_in + (float(k) + jitter) * dt;
		vec3 p = cam + rd * t;
		float c = max(dust_at(p, t * p_pixel) - c_cam, 0.0);
		if (c <= 0.0) {
			continue;
		}
		float sigma = sigma_m * c;
		// The sun through the dust toward it (two reads, to 600 m) and the sky's light from above through the dust over
		// the point.
		float tau_s = sigma_m * (max(dust_at(p + sun_dir * 120.0, 120.0) - c_cam, 0.0) * 200.0 + max(dust_at(p + sun_dir * 480.0, 480.0) - c_cam, 0.0) * 500.0);
		float h = p.y - dust_ground(p.xz);
		float top = max(dust_map(p.xz).g, 1.0);
		float up = clamp(h / top, 0.0, 1.0);
		float tau_up = sigma_m * c * max(top - h, 0.0) * 0.5;
		vec3 light = sun_radiance * step(0.0, sun_dir.y) * (alb * exp(-tau_s) * ph0 + k_ms / (4.0 * PI) * exp(-f_ms * tau_s));
		// The sky's light the same way, isotropic: the same reflectance under the sky (k / 4).
		light += mix(ambient_bottom, ambient_top, 0.5 + 0.5 * up) * 0.25 * k_ms * (0.35 + 0.65 * exp(-0.5 * f_ms * tau_up));
		float st = exp(-sigma * dt);
		col += tr * light * (1.0 - st);
		dsum += tr * (1.0 - st) * t;
		wsum += tr * (1.0 - st);
		tr *= st;
		if (tr < 0.005) {
			break;
		}
	}
	r_t = wsum > 0.0 ? dsum / wsum : t0;
	return vec4(col, tr);
}
)";

const char *SKY_BODY_B = R"(
// The three layers in the order the camera meets them: low cumulus, the mid slab, the high sheet.
vec4 march_cloud_layers(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, int steps, float jitter, float p_pixel, out float r_depth) {
	// The mid sheet over the cumulus dims their sun (as it shades the ground in the shadow bake): read once a
	// pixel, where the view ray crosses the middle of the cumulus layer (at most 30 km out), followed up the sun
	// to the sheet. Two-stream transmission, as the bake: most of the light comes through scattered, not cut.
	vec3 sun_low = sun_radiance;
	// The sun reaching each layer through the air above its own height (Bruneton's transmittance table): the sun
	// handed in is the low base's. At a low sun the mid sheet at 4.5 km sees half the air mass the base does, so at
	// sunset it was lit as dim and red as the base and stood barely brighter than the sky, where photographs show
	// altocumulus lit gold over a darkening sky.
	vec3 sun_mid = sun_radiance;
	vec3 sun_high = sun_radiance;
#ifdef ATMO_SCATTERING
	vec3 t_base = max(atmo_light_transmittance(atmo_radii.x + cloud_base_height, sun_dir.y), vec3(1e-4));
	sun_mid = sun_radiance * min(atmo_light_transmittance(atmo_radii.x + cloud_mid_height, sun_dir.y) / t_base, vec3(20.0));
	sun_high = sun_radiance * min(atmo_light_transmittance(atmo_radii.x + cloud_high_height, sun_dir.y) / t_base, vec3(20.0));
#endif
	if (cloud_mid_cover > 0.001 || cloud_storm_count > 0) {
		float y_mid_low = cloud_base_height + 0.5 * cloud_thickness;
		float t_l = rd.y > 0.01 ? (y_mid_low - cam.y) / rd.y : 30000.0;
		t_l = clamp(t_l, 0.0, 30000.0);
		vec3 pl = cam + rd * t_l;
		float sy = max(sun_dir.y, 0.05);
		vec2 pm = pl.xz + sun_dir.xz * (max(cloud_mid_height - pl.y, 0.0) / sy);
		float tau_m = wc_mid_cover(pm) * cloud_mid_tau / max(sun_dir.y, 0.1);
		sun_low *= 1.0 / (1.0 + 0.75 * 0.15 * tau_m);
	}
	float d_low;
	vec4 low = march_cumulus(cam, rd, sun_dir, sun_low, ambient_top, ambient_bottom, steps, jitter, p_pixel, d_low);
	float d_mid;
	vec4 mid = march_altocumulus(cam, rd, sun_dir, sun_mid, ambient_top, ambient_bottom, jitter, p_pixel, d_mid);
	// The virga hangs under the mid layer: met before it from below, after it from above.
	float d_vir;
	vec4 vir = march_virga(cam, rd, sun_dir, sun_mid, ambient_top, ambient_bottom, jitter, p_pixel, d_vir);
	if (vir.a < 1.0) {
		mid = cam.y < cloud_mid_height ? cloud_over(vir, d_vir, mid, d_mid, d_mid) : cloud_over(mid, d_mid, vir, d_vir, d_mid);
	}
	float d_high;
	vec4 high = march_cirrus(cam, rd, sun_dir, sun_high, ambient_top, p_pixel, d_high);
	// Contrails fly above the cirrus layer (9.5 to 12 km): behind it from below, in front of it from above.
	float d_con;
	vec4 con = march_contrails(cam, rd, sun_dir, sun_high, ambient_top, p_pixel, d_con);
	if (con.a < 1.0) {
		high = cam.y < cloud_high_height ? cloud_over(high, d_high, con, d_con, d_high) : cloud_over(con, d_con, high, d_high, d_high);
	}
	// The two wave slabs, nearer first (the camera is under both but on a mountain top).
	float d_wa;
	vec4 wa = march_wave(cloud_wave_slab.xy, cam, rd, sun_dir, sun_mid, ambient_top, ambient_bottom, jitter, p_pixel, d_wa);
	float d_wb;
	vec4 wb = march_wave(cloud_wave_slab.zw, cam, rd, sun_dir, sun_mid, ambient_top, ambient_bottom, jitter, p_pixel, d_wb);
	float d_wave;
	vec4 wave = cam.y < cloud_wave_slab.z ? cloud_over(wa, d_wa, wb, d_wb, d_wave) : cloud_over(wb, d_wb, wa, d_wa, d_wave);
	float d;
	vec4 o;
	if (cam.y < cloud_mid_height) {
		// Wave cloud below the mid layer is met before it, above it after.
		if (cloud_wave_slab.y <= cloud_wave_slab.x || 0.5 * (cloud_wave_slab.x + cloud_wave_slab.y) < cloud_mid_height) {
			o = cloud_over(low, d_low, wave, d_wave, d);
			o = cloud_over(o, d, mid, d_mid, d);
		} else {
			o = cloud_over(low, d_low, mid, d_mid, d);
			o = cloud_over(o, d, wave, d_wave, d);
		}
		o = cloud_over(o, d, high, d_high, r_depth);
		return o;
	}
	low = cloud_over(low, d_low, wave, d_wave, d_low);
	if (cam.y < cloud_mid_height) {
		o = cloud_over(low, d_low, mid, d_mid, d);
		o = cloud_over(o, d, high, d_high, r_depth);
	} else if (cam.y < cloud_high_height) {
		o = cloud_over(mid, d_mid, low, d_low, d);
		o = cloud_over(o, d, high, d_high, r_depth);
	} else {
		o = cloud_over(high, d_high, mid, d_mid, d);
		o = cloud_over(o, d, low, d_low, r_depth);
	}
	return o;
}

// The cloud layers and the storms' rain shafts below them, nearer first.
vec4 march_clouds(vec3 cam, vec3 rd, vec3 sun_dir, vec3 sun_radiance, vec3 ambient_top, vec3 ambient_bottom, int steps, float jitter, float p_pixel, out float r_depth) {
	float d;
	vec4 o = march_cloud_layers(cam, rd, sun_dir, sun_radiance, ambient_top, ambient_bottom, steps, jitter, p_pixel, d);
	r_depth = d;
	if (cloud_lake.x > 0.0) {
		float d_snow;
		vec4 ls = march_lake_snow(cam, rd, sun_dir, sun_radiance, ambient_bottom, jitter, 0.0, 1e9, d_snow);
		if (ls.a < 0.999) {
			float dd = r_depth;
			o = (d_snow < dd || o.a >= 0.999) ? cloud_over(ls, d_snow, o, dd, r_depth) : cloud_over(o, dd, ls, d_snow, r_depth);
			d = r_depth;
		}
	}
	if (cloud_dust.x > 0.0) {
		float d_dust;
		vec4 du = march_dust(cam, rd, sun_dir, sun_radiance, ambient_top, ambient_bottom, jitter, p_pixel, 0.0, 1e9, d_dust);
		if (du.a < 0.999) {
			du.rgb *= dust_tint();
			float dd = r_depth;
			o = (d_dust < dd || o.a >= 0.999) ? cloud_over(du, d_dust, o, dd, r_depth) : cloud_over(o, dd, du, d_dust, r_depth);
			d = r_depth;
		}
	}
	if (cloud_storm_count == 0) {
		return o;
	}
	float d_rain;
	vec4 rain = march_rain_shafts(cam, rd, sun_dir, sun_radiance, ambient_bottom, jitter, 0.0, 1e9, d_rain);
	if (rain.a >= 0.999) {
		return o;
	}
	return (d_rain < d || o.a >= 0.999) ? cloud_over(rain, d_rain, o, d, r_depth) : cloud_over(o, d, rain, d_rain, r_depth);
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
		// In the surfaces' units (SkyClock's note on pi): the renderer lights them with pi times the energy.
		e0 = LIGHT0_ENABLED ? LIGHT0_COLOR * LIGHT0_ENERGY * atmo_light_units : vec3(0.0);
		l1 = -UP;
		e1 = vec3(0.0);
		size0 = LIGHT0_ENABLED ? LIGHT0_SIZE : atmo_sun_angular_size;
	}
}
#endif
#ifdef ATMO_SCATTERING
// The cube face (0..5) and the face's coordinates in -1..1 of a direction; the C++ bake uses the same.
ivec3 star_cell(vec3 d) {
	vec3 a = abs(d);
	int face;
	vec2 uv;
	if (a.x >= a.y && a.x >= a.z) {
		face = d.x > 0.0 ? 0 : 1;
		uv = d.yz / a.x;
	} else if (a.y >= a.z) {
		face = d.y > 0.0 ? 2 : 3;
		uv = d.xz / a.y;
	} else {
		face = d.z > 0.0 ? 4 : 5;
		uv = d.xy / a.z;
	}
	ivec2 c = clamp(ivec2((uv * 0.5 + 0.5) * 32.0), ivec2(0), ivec2(31));
	return ivec3(face, c);
}

// The moon's radiance toward p_dir (above the air) and how much of the pixel it covers. The disc-integrated
// Lommel-Seeliger phase function divides the shape out, so the disc sums to the clock's moon illuminance
// p_e (its magnitude already carries the phase): L = p_e / disc solid angle x 2 mu0 / (mu0 + mu) / Phi(a).
vec3 moon_radiance(vec3 p_dir, vec3 p_l_sun, vec3 p_l_moon, vec3 p_e, float p_pixel_angle, out float r_cover) {
	r_cover = 0.0;
	float r = 0.5 * atmo_moon_angular_size;
	float c1 = dot(p_dir, p_l_moon);
	float th = acos(clamp(c1, -1.0, 1.0));
	if (th > r + p_pixel_angle) {
		return vec3(0.0);
	}
	r_cover = 1.0 - smoothstep(r - 0.5 * p_pixel_angle, r + 0.5 * p_pixel_angle, th);
	vec3 x = (p_dir - c1 * p_l_moon) / sin(r);
	float x2 = min(dot(x, x), 1.0);
	vec3 n = x - p_l_moon * sqrt(1.0 - x2); // the surface's normal, on the side toward the eye
	float mu = max(dot(n, -p_l_moon), 1e-3);
	float mu0 = dot(n, p_l_sun);
	float ca = clamp(dot(p_l_sun, -p_l_moon), -1.0, 1.0);
	float a = acos(ca);
	float phi = 1.0;
	if (a > 1e-3) {
		phi = 1.0 - sin(0.5 * a) * tan(0.5 * a) * log(1.0 / tan(0.25 * a));
	}
	phi = max(phi, 2e-3);
	vec3 full = p_e / (PI * r * r) / phi;
	// Earthshine: the earth, nearly full from a new moon, lights the dark side about 1/5000 of a full moon.
	vec3 earthshine = full * 2e-4 * 0.5 * (1.0 - ca);
	return (mu0 > 0.0 ? full * 2.0 * mu0 / (mu0 + mu) : vec3(0.0)) + earthshine;
}

// The stars' radiance toward p_dir (world), above the air. Each star's illuminance spread by a normalised
// Gaussian a pixel wide (at least the half arcminute of seeing), so the pixels round it sum to the star.
vec3 star_radiance(vec3 p_dir, float p_pixel_angle) {
	vec3 d = normalize(transpose(star_sky_basis) * p_dir);
	ivec3 cell = star_cell(d);
	vec2 se = texelFetch(star_cells, ivec2(cell.x * 32 + cell.y, cell.z), 0).rg;
	int first = int(se.x);
	int count = min(int(se.y), 64);
	float sigma = max(0.5 * p_pixel_angle, 1.5e-4);
	float inv2s2 = 0.5 / (sigma * sigma);
	float norm = inv2s2 / PI;
	vec3 sum = vec3(0.0);
	for (int i = 0; i < count; i++) {
		int e = first + i;
		ivec2 px = ivec2((e % 128) * 2, e / 128);
		vec4 s = texelFetch(star_data, px, 0);
		float c = dot(d, s.xyz);
		float th2 = 2.0 * max(1.0 - c, 0.0); // the angle squared, for small angles
		float w = th2 * inv2s2;
		if (w < 9.0 * 64.0) {
			// The core, and veiling glare as three wider Gaussians, each twice as wide and half the weight
			// (Schneegans et al. 2025's pyramid for the eye's 1/theta^2 glare); only bright stars show it.
			float k = exp(-w) + star_glare * (0.5 * 0.25 * exp(-0.25 * w) + 0.25 * 0.0625 * exp(-0.0625 * w) + 0.125 * 0.015625 * exp(-0.015625 * w));
			sum += s.w * norm * k * texelFetch(star_data, px + ivec2(1, 0), 0).rgb;
		}
	}
	return sum;
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
	float atmo_pixel_angle = max(length(dFdx(EYEDIR)), length(dFdy(EYEDIR)));
	if (AT_QUARTER_RES_PASS) {
		// The air alone: smooth, so a quarter of the resolution carries it (Hillaire's sky-view table
		// is coarser still); its transmittance in alpha for what lies past it.
		vec3 t_view;
		vec3 air = atmo_scatter(cam_h, EYEDIR, l0, e0, l1, e1, 1e12, AT_CUBEMAP_PASS ? atmo_radiance_steps : atmo_steps, 0.3, AT_CUBEMAP_PASS ? 0 : atmo_cloud_shadow_steps, POSITION.xz, AT_CUBEMAP_PASS ? 0.0 : atmo_aerosol.w, atmo_pixel_angle, t_view).rgb;
		if (atmo_far_water > 0.0 && atmo_hits_ground(r_cam, EYEDIR.y)) {
			// Open water past the scene: the sky mirrored at the angle the ray meets the curved sea, by
			// Fresnel (Schlick, water), seen through the air in front (Bruneton 2010 lights an ocean so).
			float t_hit = atmo_ray_sphere(r_cam, EYEDIR.y, atmo_radii.x, false);
			vec3 up_hit = normalize(vec3(0.0, r_cam, 0.0) + EYEDIR * t_hit);
			float c = clamp(-dot(EYEDIR, up_hit), 0.0, 1.0);
			vec2 hz = length(EYEDIR.xz) > 1e-5 ? normalize(EYEDIR.xz) : vec2(1.0, 0.0);
			vec3 rm = vec3(hz.x * sqrt(1.0 - c * c), c, hz.y * sqrt(1.0 - c * c));
			vec3 t_m;
			vec3 mirrored = atmo_scatter(1.0, rm, l0, e0, l1, e1, 1e12, 8, 0.5, 0, POSITION.xz, 0.0, 0.0, t_m).rgb;
			float fresnel = 0.02 + 0.98 * pow(1.0 - c, 5.0);
			air += t_view * atmo_far_water * fresnel * mirrored;
		}
		if (airglow_enabled && atmo_driven && star_energy_per_lux > 0.0 && !atmo_hits_ground(r_cam, EYEDIR.y)) {
			// cd/m2 is lux per steradian: star_energy_per_lux turns it into radiance. Seen through the air.
			float s = atmo_radii.x / (atmo_radii.x + 90000.0);
			float sin_z2 = 1.0 - EYEDIR.y * EYEDIR.y;
			float van_rhijn = inversesqrt(max(1.0 - s * s * sin_z2, 1e-4));
			vec3 tint = vec3(0.6, 1.15, 0.75); // greenish, unit luminance
			air += t_view * tint * (airglow_luminance * star_energy_per_lux * van_rhijn);
		}
		if (atmo_driven && star_energy_per_lux > 0.0 && milky_way_luminance > 0.0 && !atmo_hits_ground(r_cam, EYEDIR.y)) {
			// J2000 equatorial to galactic (Hipparcos' rotation, ESA 1997), then the panorama's longitude and latitude.
			const mat3 eq_to_gal = mat3(vec3(-0.0548755604, 0.4941094279, -0.8676661490), vec3(-0.8734370902, -0.4448296300, -0.1980763734), vec3(-0.4838350155, 0.7469822445, 0.4559837762));
			vec3 g = eq_to_gal * normalize(transpose(star_sky_basis) * EYEDIR);
			vec2 uv = vec2(0.5 - atan(g.y, g.x) / (2.0 * PI), 0.5 - asin(clamp(g.z, -1.0, 1.0)) / PI);
			vec3 mw = textureLod(milky_way, uv, milky_way_blur_lod).rgb;
			air += t_view * mw * (milky_way_luminance * star_energy_per_lux);
		}
		// Exposed here, ahead of the half-float buffers: a night sky's radiance in light energy units is
		// below half floats' range (Frostbite's pre-exposure; Lagarde and de Rousiers 2014).
		COLOR = air * exposure;
		ALPHA = 1.0;
	} else if (AT_HALF_RES_PASS) {
		// The clouds over the air.
		vec4 air = vec4(QUARTER_RES_COLOR.rgb, 1.0);
		float r_c = atmo_radii.x + cloud_base_height;
		vec3 sun_radiance = e0 * atmo_light_transmittance(r_c, l0.y) * cloud_brightness;
		vec3 t_up;
		// The sky light over a cloud: the mean over the upper hemisphere, not the zenith alone (the zenith is the
		// sky's darkest part; a cloud in another's shadow at a low sun is lit by the bright sky toward the horizon
		// and the sun, and came out darker than the sky behind it). Zenith plus two reads 30 degrees up, on the
		// sun's side and away from it.
		vec2 sun_h = normalize(l0.xz + vec2(1e-4, 0.0));
		sun_h = vec2(0.866 * sun_h.x - 0.5 * sun_h.y, 0.5 * sun_h.x + 0.866 * sun_h.y); // 30 degrees off the sun: its glow, not its aureole
		vec3 d_to = vec3(sun_h.x * 0.866, 0.5, sun_h.y * 0.866);
		vec3 d_away = vec3(-d_to.x, 0.5, -d_to.z);
		vec3 ambient_top = (0.4 * atmo_scatter(cloud_base_height, UP, l0, e0, l1, e1, 1e12, 8, 0.5, 0, POSITION.xz, 0.0, 0.0, t_up).rgb
				+ 0.3 * atmo_scatter(cloud_base_height, d_to, l0, e0, l1, e1, 1e12, 6, 0.5, 0, POSITION.xz, 0.0, 0.0, t_up).rgb
				+ 0.3 * atmo_scatter(cloud_base_height, d_away, l0, e0, l1, e1, 1e12, 6, 0.5, 0, POSITION.xz, 0.0, 0.0, t_up).rgb) * cloud_brightness;
		if (airglow_enabled && atmo_driven) {
			// The airglow over the clouds lights their tops (its mean over the sky, about twice the zenith's).
			ambient_top += vec3(0.6, 1.15, 0.75) * (2.0 * airglow_luminance * star_energy_per_lux * cloud_brightness);
		}
		vec3 ambient_bottom = ambient_top * 0.5 + ground_color.rgb * sun_radiance * max(l0.y, 0.0) * (1.0 - 0.7 * cloud_cover) / PI;
		float jitter = AT_CUBEMAP_PASS ? 0.5 : march_jitter(FRAGCOORD.xy, 0.0);
#ifdef CLOUD_REPROJECTION
		jitter = AT_CUBEMAP_PASS ? 0.5 : march_jitter_revisit(FRAGCOORD.xy, REPROJECTION_FRAME);
#endif
		vec3 dir_dx = dFdx(EYEDIR);
		vec3 dir_dy = dFdy(EYEDIR);
		vec3 rd = EYEDIR;
#ifdef CLOUD_REPROJECTION
		if (!AT_CUBEMAP_PASS) {
			// The pass's pixels are 4 screen pixels apart (each draws one pixel of a 4x4 block): the screen pixel is a
			// quarter of the derivative. Read at four pixels' footprint, the noise lost its detail (smooth, cotton-ball
			// clouds) and edges were the pass's blocks.
			dir_dx *= 0.25;
			dir_dy *= 0.25;
			// Each revisit's ray lands somewhere else inside the pixel (an R2 sequence; Roberts 2018), so the resolve's
			// running mean is the pixel's coverage: an antialiased edge, where a ray through the pixel's centre every
			// time drew a cloud's hard edge (lenses, turrets) as a staircase.
			float k = float((REPROJECTION_FRAME >> 4u) & 1023u);
			vec2 sub = fract(vec2(0.5) + k * vec2(0.7548777, 0.5698403)) - 0.5;
			rd = normalize(EYEDIR + sub.x * dir_dx + sub.y * dir_dy);
		}
#endif
		float pixel_angle = max(length(dir_dx), length(dir_dy));
		vec4 clouds = vec4(0.0, 0.0, 0.0, 1.0);
		float cloud_depth = 0.0;
		if (dot(e0, vec3(1.0)) > 0.0 || cloud_light_count > 0) {
			clouds = march_clouds(POSITION, rd, l0, sun_radiance, ambient_top, ambient_bottom, AT_CUBEMAP_PASS ? cloud_radiance_steps : cloud_steps, jitter, pixel_angle, cloud_depth);
		}
		if (clouds.a < 0.999) {
			// Aerial perspective (Hillaire 2020): the clouds seen through the air in front of them, which
			// also scatters light toward the eye; what the clouds let through sees the whole sky behind.
			vec3 t_front;
			vec3 front = atmo_scatter(cam_h, EYEDIR, l0, e0, l1, e1, cloud_depth, atmo_cloud_air_steps, 0.5, AT_CUBEMAP_PASS ? 0 : atmo_cloud_shadow_steps, POSITION.xz, AT_CUBEMAP_PASS ? 0.0 : atmo_aerosol.w, atmo_pixel_angle, t_front).rgb;
			COLOR = ((1.0 - clouds.a) * front + t_front * clouds.rgb) * exposure + air.rgb * clouds.a;
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
			// The haze grey; the fires' smoke by colour over its whole column, as the sun light (SkyClock). The disc is
			// drawn at the sun's irradiance, not its radiance (that over its solid angle, some 15000 times more), so
			// it clips white as the real one does; through smoke the real disc stays a blinding white until the
			// smoke has taken all but that share of it, and only then dims and turns orange, then red (a slant
			// optical depth of about 8 to 10). So the smoke's share is scaled by the disc's own radiance and
			// clipped at the clean disc's: white under light smoke, a dim red disc under a thick plume.
			float omega_sun = 2.0 * PI * (1.0 - cos(0.5 * size0));
			vec3 smoke_disc = min(atmo_sun_smoke / max(omega_sun, 1e-7), vec3(1.0));
			disc = e0 * atmo_transmittance_to_top(r_cam, EYEDIR.y) * pow(atmo_aerosol_view(r_cam, EYEDIR), 1.0 - atmo_aerosol_smoke) * smoke_disc * sundisk;
		}
		// The moon and the stars through the air, behind the clouds (the stars not in the radiance map: they
		// light nothing).
		if (atmo_driven && !atmo_hits_ground(r_cam, EYEDIR.y)) {
			float pixel_angle = max(max(length(dFdx(EYEDIR)), length(dFdy(EYEDIR))), 1e-5);
			vec3 t_top = atmo_transmittance_to_top(r_cam, EYEDIR.y) * atmo_aerosol_view(r_cam, EYEDIR);
			float cover = 0.0;
			if (moon_disc_enabled && dot(e1, vec3(1.0)) > 0.0) {
				disc += moon_radiance(EYEDIR, l0, l1, e1, pixel_angle, cover) * t_top;
			}
			if (stars_enabled && star_energy_per_lux > 0.0 && !AT_CUBEMAP_PASS) {
				disc += star_radiance(EYEDIR, pixel_angle) * (star_energy_per_lux * star_brightness * (1.0 - cover)) * t_top;
			}
		}
		vec3 glow = vec3(0.0);
		if (lightning_count > 0 && !AT_CUBEMAP_PASS) {
			glow = lightning_glow(POSITION, EYEDIR, 1.0 - HALF_RES_COLOR.a);
		}
		COLOR = HALF_RES_COLOR.rgb + (disc * HALF_RES_COLOR.a + glow) * exposure;
	}
#else
	vec3 sun_dir = LIGHT0_ENABLED ? normalize(LIGHT0_DIRECTION) : UP;
	float light_energy = LIGHT0_ENABLED ? LIGHT0_ENERGY : 0.0;
	if (AT_HALF_RES_PASS) {
		float sun_fade = 1.0 - clamp(1.0 - exp(sun_dir.y), 0.0, 1.0);
		vec3 sun_radiance = LIGHT0_COLOR * light_energy * air_extinction(sun_dir, sun_fade) * cloud_brightness;
		// The mean over the upper hemisphere, as the scattering model's branch: zenith plus 30 degrees up on the sun's
		// side and away from it.
		vec2 sun_h = normalize(sun_dir.xz + vec2(1e-4, 0.0));
		sun_h = vec2(0.866 * sun_h.x - 0.5 * sun_h.y, 0.5 * sun_h.x + 0.866 * sun_h.y); // 30 degrees off the sun: its glow, not its aureole
		vec3 d_to = vec3(sun_h.x * 0.866, 0.5, sun_h.y * 0.866);
		vec3 d_away = vec3(-d_to.x, 0.5, -d_to.z);
		vec3 ambient_top = (0.4 * atmosphere(UP, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, false)
				+ 0.3 * atmosphere(d_to, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, false)
				+ 0.3 * atmosphere(d_away, sun_dir, light_energy, LIGHT0_COLOR, LIGHT0_SIZE, false)) * cloud_brightness;
		// From below: the sky's light off the ground (its colour as albedo), lit by the sun where clear.
		vec3 ambient_bottom = ambient_top * 0.5 + ground_color.rgb * sun_radiance * max(sun_dir.y, 0.0) * (1.0 - 0.7 * cloud_cover) / PI;
		float jitter = AT_CUBEMAP_PASS ? 0.5 : march_jitter(FRAGCOORD.xy, 0.0);
#ifdef CLOUD_REPROJECTION
		// Reprojected (half_res_reprojection): a new start each frame, which the resolve averages.
		jitter = AT_CUBEMAP_PASS ? 0.5 : march_jitter_revisit(FRAGCOORD.xy, REPROJECTION_FRAME);
#endif
		// One pixel's angle, from the eye direction's change to the next pixel (the pass is uniform, so
		// the derivatives are defined here).
		vec3 dir_dx = dFdx(EYEDIR);
		vec3 dir_dy = dFdy(EYEDIR);
		vec3 rd = EYEDIR;
#ifdef CLOUD_REPROJECTION
		if (!AT_CUBEMAP_PASS) {
			// As the scattering sky's branch: a screen pixel is a quarter of the pass's derivative; rays spread in it.
			dir_dx *= 0.25;
			dir_dy *= 0.25;
			float k = float((REPROJECTION_FRAME >> 4u) & 1023u);
			vec2 sub = fract(vec2(0.5) + k * vec2(0.7548777, 0.5698403)) - 0.5;
			rd = normalize(EYEDIR + sub.x * dir_dx + sub.y * dir_dy);
		}
#endif
		float pixel_angle = max(length(dir_dx), length(dir_dy));
		float cloud_depth;
		vec4 clouds = march_clouds(POSITION, rd, sun_dir, sun_radiance, ambient_top, ambient_bottom, AT_CUBEMAP_PASS ? cloud_radiance_steps : cloud_steps, jitter, pixel_angle, cloud_depth);
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
	rs->material_set_param(m, "cloud_shear", p_state.shear);
	rs->material_set_param(m, "cloud_size_mix", p_state.size_mix);
	rs->material_set_param(m, "cloud_evolve", p_state.evolve);
	rs->material_set_param(m, "cloud_churn", Vector3(p_state.shift.x, p_state.shift.y, p_state.rise_detail));
	rs->material_set_param(m, "cloud_storm_count", p_state.storm_count);
	PackedVector4Array storms;
	storms.resize(WeatherClouds::MAX_STORMS);
	for (int i = 0; i < WeatherClouds::MAX_STORMS; i++) {
		storms.write[i] = Vector4(p_state.storms[i][0], p_state.storms[i][1], MAX(p_state.storms[i][2], 1.0f), p_state.storms[i][3]);
	}
	rs->material_set_param(m, "cloud_storms", storms);
	PackedFloat32Array bases;
	bases.resize(WeatherClouds::MAX_STORMS);
	for (int i = 0; i < WeatherClouds::MAX_STORMS; i++) {
		bases.write[i] = i < p_state.storm_count ? p_state.storm_base[i] : 0.0f;
	}
	rs->material_set_param(m, "cloud_storm_base", bases);
	{
		PackedFloat32Array rain;
		PackedFloat32Array outflow;
		PackedFloat32Array snow;
		PackedFloat32Array deep;
		deep.resize(WeatherClouds::MAX_STORMS);
		rain.resize(WeatherClouds::MAX_STORMS);
		outflow.resize(WeatherClouds::MAX_STORMS);
		snow.resize(WeatherClouds::MAX_STORMS);
		for (int i = 0; i < WeatherClouds::MAX_STORMS; i++) {
			rain.write[i] = i < p_state.storm_count ? p_state.storm_rain[i] : 0.0f;
			outflow.write[i] = i < p_state.storm_count ? p_state.storm_outflow[i] : 0.0f;
			snow.write[i] = i < p_state.storm_count ? p_state.storm_snow[i] : 0.0f;
			deep.write[i] = i < p_state.storm_count ? p_state.storm_deep[i] : 0.0f;
		}
		rs->material_set_param(m, "cloud_storm_deep", deep);
		rs->material_set_param(m, "cloud_storm_rain", rain);
		rs->material_set_param(m, "cloud_storm_outflow", outflow);
		rs->material_set_param(m, "cloud_storm_snow", snow);
		rs->material_set_param(m, "cloud_rain_slant", p_state.rain_slant);
		rs->material_set_param(m, "cloud_storm_dir", p_state.storm_dir);
	}
	rs->material_set_param(m, "cloud_base_min", p_state.base_min);
	{
		PackedVector4Array eff;
		eff.resize(WeatherClouds::MAX_CLOUD_EFFECTORS);
		PackedFloat32Array strength;
		strength.resize(WeatherClouds::MAX_CLOUD_EFFECTORS);
		for (int i = 0; i < WeatherClouds::MAX_CLOUD_EFFECTORS; i++) {
			const float *f = p_state.effectors[i];
			const bool on = i < p_state.effector_count;
			eff.write[i] = on ? Vector4(f[0], f[1], f[2], MAX(f[3], 1.0f)) : Vector4(0, 0, 0, 1);
			strength.write[i] = on ? f[4] : 0.0f;
		}
		rs->material_set_param(m, "cloud_effector_count", p_state.effector_count);
		rs->material_set_param(m, "cloud_effectors", eff);
		rs->material_set_param(m, "cloud_effector_strength", strength);
		PackedVector4Array lights;
		lights.resize(WeatherClouds::MAX_CLOUD_LIGHTS);
		PackedVector3Array colors;
		colors.resize(WeatherClouds::MAX_CLOUD_LIGHTS);
		for (int i = 0; i < WeatherClouds::MAX_CLOUD_LIGHTS; i++) {
			const float *l = p_state.lights[i];
			const bool on = i < p_state.light_count;
			lights.write[i] = on ? Vector4(l[0], l[1], l[2], MAX(l[3], 1.0f)) : Vector4(0, 0, 0, 1);
			colors.write[i] = on ? Vector3(l[4], l[5], l[6]) : Vector3();
		}
		rs->material_set_param(m, "cloud_light_count", p_state.light_count);
		rs->material_set_param(m, "cloud_lights", lights);
		rs->material_set_param(m, "cloud_light_colors", colors);
	}
	rs->material_set_param(m, "cloud_sc", Vector4(p_state.sc_dir.x, p_state.sc_dir.y, p_state.sc_roll, p_state.sc_cell));
	rs->material_set_param(m, "cloud_wave_levels", Vector4(p_state.wave_levels[0], p_state.wave_levels[1], p_state.wave_levels[2], p_state.wave_levels[3]));
	rs->material_set_param(m, "cloud_wave_slab", p_state.wave_n > 0 ? Vector4(p_state.wave_y0, p_state.wave_y1, p_state.wave_y2, p_state.wave_y3) : Vector4());
	rs->material_set_param(m, "cloud_air", Vector3(p_state.air_t0, p_state.air_rh0, p_state.air_y0));
	rs->material_set_param(m, "cloud_scud", p_state.scud);
	rs->material_set_param(m, "cloud_lake", p_state.lake_n > 0 ? p_state.lake_params : Vector4());
	rs->material_set_param(m, "cloud_lake_dir", p_state.lake_dir);
	rs->material_set_param(m, "cloud_dust", Vector4(p_state.dust_top, p_state.dust_column, 0.0f, 0.0f));
	rs->material_set_param(m, "cloud_high_cover", p_state.high_cover);
	rs->material_set_param(m, "cloud_high_height", p_state.high_height);
	rs->material_set_param(m, "cloud_high_tau", p_state.high_tau);
	rs->material_set_param(m, "cloud_high_scale", p_state.high_scale);
	rs->material_set_param(m, "cloud_high_drift", p_state.high_drift);
	rs->material_set_param(m, "cloud_contrail_wind", p_state.contrail_wind);
	rs->material_set_param(m, "cloud_clock", p_state.clock);
	rs->material_set_param(m, "cloud_high_dir", p_state.high_dir);
	rs->material_set_param(m, "cloud_high_cc", p_state.high_cc);
	rs->material_set_param(m, "cloud_cell_size", p_state.cell_size);
	rs->material_set_param(m, "cloud_mid_cover", p_state.mid_cover);
	rs->material_set_param(m, "cloud_mid_height", p_state.mid_height);
	rs->material_set_param(m, "cloud_mid_thickness", p_state.mid_thickness);
	rs->material_set_param(m, "cloud_mid_tau", p_state.mid_tau);
	rs->material_set_param(m, "cloud_mid_scale", p_state.mid_scale);
	rs->material_set_param(m, "cloud_mid_drift", p_state.mid_drift);
	rs->material_set_param(m, "cloud_mid_dir", p_state.mid_dir);
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

const char *WeatherSkyMaterial::get_shafts_glsl() {
	return SHAFTS_GLSL;
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
		shader->set_code(head + String(WeatherClouds::GLSL) + String(WeatherAtmosphere::GLSL) + String(SKY_BODY) + String(SHAFTS_GLSL) + String(SKY_BODY_B));
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
	if (star_data_tex.is_null()) {
		_bake_stars();
	}
	RS::get_singleton()->material_set_param(get_rid(), "star_data", star_data_tex->get_rid());
	RS::get_singleton()->material_set_param(get_rid(), "star_cells", star_cells_tex->get_rid());
}

// The cube face and cell of a direction, as star_cell() in the shader.
static int _star_cell(const Vector3 &p_d) {
	const Vector3 a = p_d.abs();
	int face;
	Vector2 uv;
	if (a.x >= a.y && a.x >= a.z) {
		face = p_d.x > 0.0f ? 0 : 1;
		uv = Vector2(p_d.y, p_d.z) / a.x;
	} else if (a.y >= a.z) {
		face = p_d.y > 0.0f ? 2 : 3;
		uv = Vector2(p_d.x, p_d.z) / a.y;
	} else {
		face = p_d.z > 0.0f ? 4 : 5;
		uv = Vector2(p_d.x, p_d.y) / a.z;
	}
	const int cu = CLAMP(int((uv.x * 0.5f + 0.5f) * 32.0f), 0, 31);
	const int cv = CLAMP(int((uv.y * 0.5f + 0.5f) * 32.0f), 0, 31);
	return (face * 32 + cu) + cv * 192;
}

// A star's colour from its B-V index: a black body at Ballesteros' (2012) temperature, through the CIE 1931
// observer (Wyman, Sloan and Shirley 2013's fit) into linear sRGB, scaled to unit luminance.
static Vector3 _star_colour(float p_bv) {
	const double t = 4600.0 * (1.0 / (0.92 * p_bv + 1.7) + 1.0 / (0.92 * p_bv + 0.62));
	double X = 0.0, Y = 0.0, Z = 0.0;
	for (int nm = 380; nm <= 780; nm += 5) {
		const double l = nm;
		const double lm = l * 1e-9;
		const double b = 1.0 / (Math::pow(lm, 5.0) * (Math::exp(1.4387769e-2 / (lm * t)) - 1.0));
		auto g = [](double x, double mu, double s1, double s2) {
			const double s = x < mu ? s1 : s2;
			return Math::exp(-0.5 * (x - mu) * (x - mu) / (s * s));
		};
		const double xb = 1.056 * g(l, 599.8, 37.9, 31.0) + 0.362 * g(l, 442.0, 16.0, 26.7) - 0.065 * g(l, 501.1, 20.4, 26.2);
		const double yb = 0.821 * g(l, 568.8, 46.9, 40.5) + 0.286 * g(l, 530.9, 16.3, 31.1);
		const double zb = 1.217 * g(l, 437.0, 11.8, 36.0) + 0.681 * g(l, 459.0, 26.0, 13.8);
		X += b * xb;
		Y += b * yb;
		Z += b * zb;
	}
	Vector3 rgb(3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z, 0.0557 * X - 0.2040 * Y + 1.0570 * Z);
	rgb = rgb.max(Vector3());
	const float lum = rgb.dot(Vector3(0.2126, 0.7152, 0.0722));
	return lum > 0.0f ? rgb / lum : Vector3(1, 1, 1);
}

// Bins the catalogue by cube-face cell (32 x 32 a face, about 3 deg). A star near a cell's edge goes into
// the cells within its glare's reach too, so a pixel reads one cell's list.
void WeatherSkyMaterial::_bake_stars() {
	const float margin = Math::deg_to_rad(1.0f);
	LocalVector<LocalVector<int>> cells;
	cells.resize(192 * 32);
	LocalVector<Vector3> dirs;
	dirs.resize(BRIGHT_STAR_COUNT);
	for (int i = 0; i < BRIGHT_STAR_COUNT; i++) {
		const float ra = float(uint16_t(BRIGHT_STARS[i * 4 + 0])) / 65536.0f * Math::TAU;
		const float dec = float(BRIGHT_STARS[i * 4 + 1]) / 32767.0f * float(Math::PI) * 0.5f;
		const Vector3 d(Math::cos(dec) * Math::cos(ra), Math::cos(dec) * Math::sin(ra), Math::sin(dec));
		dirs[i] = d;
		const Vector3 t1 = (Math::abs(d.z) < 0.9f ? Vector3(0, 0, 1) : Vector3(1, 0, 0)).cross(d).normalized();
		const Vector3 t2 = d.cross(t1);
		int seen[9];
		int n = 0;
		for (int k = 0; k < 9; k++) {
			const float a = k * Math::TAU / 8.0f;
			const Vector3 p = k == 8 ? d : (d + (t1 * Math::cos(a) + t2 * Math::sin(a)) * margin).normalized();
			const int c = _star_cell(p);
			bool dup = false;
			for (int j = 0; j < n; j++) {
				dup = dup || seen[j] == c;
			}
			if (!dup) {
				seen[n++] = c;
				cells[c].push_back(i);
			}
		}
	}
	int total = 0;
	for (const LocalVector<int> &l : cells) {
		total += l.size();
	}
	const int rows = (total + 127) / 128;
	Vector<float> data;
	data.resize(256 * rows * 4);
	data.fill(0.0f);
	Vector<float> table;
	table.resize(192 * 32 * 2);
	float *dw = data.ptrw();
	float *tw = table.ptrw();
	int e = 0;
	for (uint32_t c = 0; c < cells.size(); c++) {
		tw[c * 2 + 0] = e;
		tw[c * 2 + 1] = cells[c].size();
		for (int s : cells[c]) {
			const float vmag = BRIGHT_STARS[s * 4 + 2] / 100.0f;
			const Vector3 col = _star_colour(BRIGHT_STARS[s * 4 + 3] / 1000.0f);
			float *p = dw + ((e / 128) * 256 + (e % 128) * 2) * 4;
			p[0] = dirs[s].x;
			p[1] = dirs[s].y;
			p[2] = dirs[s].z;
			// Illuminance above the air, in lux (V magnitude 0 gives 2.54e-6 lux).
			p[3] = Math::pow(10.0f, -0.4f * (vmag + 13.99f));
			p[4] = col.x;
			p[5] = col.y;
			p[6] = col.z;
			p[7] = 1.0f;
			e++;
		}
	}
	star_data_tex = ImageTexture::create_from_image(Image::create_from_data(256, rows, false, Image::FORMAT_RGBAF, data.to_byte_array()));
	star_cells_tex = ImageTexture::create_from_image(Image::create_from_data(192, 32, false, Image::FORMAT_RGF, table.to_byte_array()));
}

void WeatherSkyMaterial::set_star_sky(const Basis &p_celestial, float p_energy_per_lux) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	rs->material_set_param(m, "star_sky_basis", p_celestial);
	rs->material_set_param(m, "star_energy_per_lux", p_energy_per_lux);
}

void WeatherSkyMaterial::set_moon_angular_size(float p_radians) {
	RS::get_singleton()->material_set_param(get_rid(), "atmo_moon_angular_size", p_radians);
}

void WeatherSkyMaterial::set_aerosol(float p_extinction, float p_albedo, float p_height, float p_from, float p_smoke_share) {
	const float e = MAX(p_extinction, 0.0f);
	RS::get_singleton()->material_set_param(get_rid(), "atmo_aerosol", Vector4(e * CLAMP(p_albedo, 0.0f, 1.0f), e, MAX(p_height, 10.0f), MAX(p_from, 0.0f)));
	RS::get_singleton()->material_set_param(get_rid(), "atmo_aerosol_smoke", CLAMP(p_smoke_share, 0.0f, 1.0f));
}

void WeatherSkyMaterial::set_sun_smoke(const Vector3 &p_transmittance) {
	RS::get_singleton()->material_set_param(get_rid(), "atmo_sun_smoke", p_transmittance.clamp(Vector3(), Vector3(1, 1, 1)));
}

void WeatherSkyMaterial::set_cloud_shadow_map(const Ref<Texture2D> &p_map, const Vector3 &p_axis_u, const Vector3 &p_axis_v, const Vector2 &p_corner, float p_size) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	const RID map = p_map.is_valid() ? p_map->get_rid() : RID();
	if (map != cloud_shadow_map) {
		rs->material_set_param(m, "atmo_cloud_shadow_map", map.is_valid() ? Variant(map) : Variant());
		cloud_shadow_map = map;
	}
	rs->material_set_param(m, "atmo_cloud_shadow_u", Vector4(p_axis_u.x, p_axis_u.y, p_axis_u.z, p_corner.x));
	rs->material_set_param(m, "atmo_cloud_shadow_v", Vector4(p_axis_v.x, p_axis_v.y, p_axis_v.z, p_corner.y));
	rs->material_set_param(m, "atmo_cloud_shadow_size", p_map.is_valid() ? p_size : 0.0f);
}

void WeatherSkyMaterial::set_cloud_light_grid(const Ref<Texture3D> &p_grid, const Vector2 &p_corner, float p_size, float p_y0, float p_y1) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	const RID grid = p_grid.is_valid() ? p_grid->get_rid() : RID();
	if (grid != cloud_light_grid) {
		rs->material_set_param(m, "cloud_light_grid", grid.is_valid() ? Variant(grid) : Variant());
		cloud_light_grid = grid;
	}
	rs->material_set_param(m, "cloud_light_grid_box", Vector4(p_corner.x, p_corner.y, MAX(p_size, 1.0f), grid.is_valid() ? 1.0f : 0.0f));
	rs->material_set_param(m, "cloud_light_grid_y", Vector2(p_y0, MAX(p_y1, p_y0 + 1.0f)));
}

void WeatherSkyMaterial::set_cloud_wave_field(const Ref<Texture2D> &p_field, const Vector2 &p_corner, float p_size, int p_texels) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	const RID f = p_field.is_valid() ? p_field->get_rid() : RID();
	if (f != cloud_wave_field) {
		rs->material_set_param(m, "cloud_wave_field", f.is_valid() ? Variant(f) : Variant());
		cloud_wave_field = f;
	}
	rs->material_set_param(m, "cloud_wave_rect", Vector4(p_corner.x, p_corner.y, MAX(p_size, 1.0f), float(p_texels)));
}

void WeatherSkyMaterial::set_cloud_lake_field(const Ref<Texture2D> &p_field, const Vector2 &p_corner, float p_size, int p_texels) {
	RenderingServer *rs = RS::get_singleton();
	const RID m = get_rid();
	const RID f = p_field.is_valid() ? p_field->get_rid() : RID();
	if (f != cloud_lake_field) {
		rs->material_set_param(m, "cloud_lake_field", f.is_valid() ? Variant(f) : Variant());
		cloud_lake_field = f;
	}
	rs->material_set_param(m, "cloud_lake_rect", Vector4(p_corner.x, p_corner.y, MAX(p_size, 1.0f), float(p_texels)));
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
	ClassDB::bind_method(D_METHOD("set_star_sky", "celestial", "energy_per_lux"), &WeatherSkyMaterial::set_star_sky);
	ClassDB::bind_method(D_METHOD("set_moon_angular_size", "radians"), &WeatherSkyMaterial::set_moon_angular_size);
	ClassDB::bind_method(D_METHOD("set_aerosol", "extinction", "albedo", "height", "from", "smoke_share"), &WeatherSkyMaterial::set_aerosol, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("set_sun_smoke", "transmittance"), &WeatherSkyMaterial::set_sun_smoke);
	ClassDB::bind_method(D_METHOD("set_cloud_shadow_map", "map", "axis_u", "axis_v", "corner", "size"), &WeatherSkyMaterial::set_cloud_shadow_map);
	ClassDB::bind_method(D_METHOD("set_cloud_light_grid", "grid", "corner", "size", "y0", "y1"), &WeatherSkyMaterial::set_cloud_light_grid);
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
	star_data_tex.unref();
	star_cells_tex.unref();
	WeatherClouds::free_textures();
}

WeatherSkyMaterial::WeatherSkyMaterial() {
	_update_shader();
	RS::get_singleton()->material_set_param(get_rid(), "cloud_shape", WeatherClouds::shape_texture()->get_rid());
	RS::get_singleton()->material_set_param(get_rid(), "atmo_light_units", GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units") ? 1.0f : float(Math::PI));
	RS::get_singleton()->material_set_param(get_rid(), "cloud_detail", WeatherClouds::detail_texture()->get_rid());
}

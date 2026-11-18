/**************************************************************************/
/*  physx_river_3d.cpp                                                    */
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

#include "physx_river_3d.h"

#include "flow_poisson.h"
#include "physx_water_mist_3d.h"
#include "physx_water_surface_3d.h"
#include "water_fall_sound.h"

#include "scene/3d/audio_stream_player_3d.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#include "modules/weather/weather_runoff_3d.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture_rd.h"
#include "servers/physics_3d/physics_server_3d.h"

#ifdef GODOT_PHYSX_FLOW
#include "../flow/physx_flow_emitter_3d.h"
#endif

namespace {

// Seconds each copy of the ripples rides the current (the shader's flow_period; the live window's advected
// coordinates restart on the same phases).
constexpr float RIVER_FLOW_PERIOD = 1.5f;

// The river surface. Per vertex (built by PhysXRiver3D): UV = (arc length,
// distance across) in m, UV2 = flow (local xz, m/s), COLOR.r = water depth
// encoded as (d + 1) / 4, COLOR.g = white water, COLOR.b = fade past the banks.
const char *RIVER_SHADER_SRC_HEAD = R"(
shader_type spatial;
#if defined(RT)
// The path tracer's copy is a medium boundary: it bends light, tints with
// depth and casts caustics (the same as PhysXWaterSurface3D's).
render_mode cull_disabled;
#else
render_mode blend_mix, cull_disabled;
#endif

// Animation clock, set by PhysXRiver3D while the game runs (0 in the editor,
// so the editor doesn't redraw every frame).
uniform float water_time = 0.0;
// Two-phase flow (Vlachos 2010): each copy of the ripples rides the current for
// one flow_period, then restarts while the other one is showing.
uniform float flow_period = 1.5;
uniform vec4 water_color : source_color = vec4(0.10, 0.25, 0.24, 0.72);
uniform vec4 shallow_color : source_color = vec4(0.32, 0.42, 0.34, 0.35);
uniform vec4 foam_color : source_color = vec4(0.92, 0.95, 0.97, 0.95);
// Path tracing only: river water is murkier than the sea (more red and
// green taken out per metre, more fine silt scattering).
uniform vec3 pathtraced_absorption = vec3(0.6, 0.14, 0.1);
uniform vec3 pathtraced_scattering = vec3(0.09, 0.1, 0.09);
uniform float pathtraced_scattering_anisotropy : hint_range(-0.9, 0.95) = 0.85;
uniform float pathtraced_roughness : hint_range(0.0, 1.0) = 0.03;
// Ripples: cells per metre, and their slope in still water and per m/s of current
// (faster water is rougher).
uniform float ripple_scale = 1.0;
uniform float ripple_strength = 0.06;
uniform float ripple_speed_strength = 0.12;
uniform float foam_detail_scale = 1.5;
uniform float foam_breakup : hint_range(0.0, 1.0) = 0.6;
// Crisp white water, as the open sea's (docs/ideas/water.md "Crisp foam on the open sea"): torn into holes and
// strands at three scales with bubble grain (Plateau borders, Weaire and Hutzler 1999), its edge a narrow step of
// the amount against a ragged threshold (Sea of Thieves, Atlas) and once more at the pixel's own scale. 0: the
// older soft value-noise lace.
uniform float foam_crisp : hint_range(0.0, 1.0) = 1.0;
// The foam as a skin about this share of 3 cm thick: its gradient tilts the normal, so rims catch the sun on one
// side (Crest's foam 3D lighting); and its bubbles' domes tilt it by foam_grain_tilt.
uniform float foam_relief : hint_range(0.0, 2.0) = 0.5;
uniform float foam_grain_tilt : hint_range(0.0, 2.0) = 0.15;
// Capillary ripples down to about a centimetre (three more octaves under the ripples), each faded out where it is
// finer than the pixel and its slope put into the surface's roughness instead (Bruneton, Neyret and Holzschuch
// 2010), so the near water is sharp and the far water glitters without shimmering. 0: the three coarse octaves.
uniform float ripple_detail : hint_range(0.0, 2.0) = 1.0;
// The surface fades out over this much water depth (m) at the waterline.
uniform float edge_softness : hint_range(0.0, 0.5, 0.005) = 0.06;
// A living waterline: the edge laps in and out by up to this much water depth
// (m) -- small wavelets and the current's swirls arriving at the bank, so on a
// gentle bank the line wanders by decimetres and is never straight -- with a
// lace of foam caught along it.
uniform float edge_lap_height : hint_range(0.0, 0.2, 0.005) = 0.035;
uniform float edge_lap_scale = 0.8;
uniform float edge_foam : hint_range(0.0, 1.0, 0.01) = 0.35;
// The bank's own small relief the ground mesh is too coarse to carry (pebbles,
// ruts, tufts): up to this much depth (m), fixed to the ground, so the line
// runs in and out of little bays and points instead of following the mesh.
uniform float edge_relief : hint_range(0.0, 0.3, 0.005) = 0.05;
// Debug: colour the water by its Froude number (blue subcritical, green critical, red supercritical, white
// at jump toes).
uniform bool froude_overlay = false;
// Metres between the river's cross-sections (set by PhysXRiver3D): standing waves shorter than a few sections
// are drawn in the slope only, as the mesh would facet them into ridges.
uniform float section_step = 1.0;
// The live window (set by PhysXRiver3D while live_flow runs): a square of
// shallow-water simulation round the camera. Height is the disturbance on the
// river's own surface; flow is the total velocity (rg, world xz, m/s) with the
// white water it made in b.
uniform bool live_enabled = false;
uniform vec2 live_center = vec2(0.0);
uniform vec2 live_size = vec2(48.0);
uniform vec2 live_texel = vec2(0.25);
// The window's mean plane (h0, dh/dx, dh/dz about live_center), taken out.
uniform vec3 live_tilt = vec3(0.0);
uniform sampler2D live_height_tex : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D live_flow_tex : hint_default_black, filter_linear, repeat_disable;
// Advected ripple coordinates (water_flow_offset.glsl): how far each phase's ripples have drifted from where plain
// flow mapping puts them, following the live flow's real paths (round eddies, not straight along today's
// velocity). Phase A in rg, B in ba (m).
uniform bool live_advected = false;
uniform sampler2D live_offset_tex : hint_default_black, filter_linear, repeat_disable;
// Clear water (raster, seen from above; docs/ideas/water.md "Clear water you can see into"): the bed read
// through the surface along the refracted ray and passed through the path tracer's medium, as on the sea
// (PhysXWaterSurface3D). Off: the older tint (water_color, shallow_color).
uniform bool clear_water = true;
// Screen-space reflections with raster_reflections off (a cave stream: no sky to mirror), as PhysXWaterSurface3D's.
uniform bool screen_reflections = true;
// Raster: the water thins out over this much of its own depth (m) against what the screen shows behind it -- the
// bank, a stone standing out of it -- so the shoreline is soft and follows the ground, not the ribbon's
// waterline interpolated between its vertices (depth fade, as UE's Water and Crest do it).
uniform float shore_depth_fade : hint_range(0.0, 0.3, 0.005) = 0.05;
#if !defined(RT)
uniform sampler2D clear_screen : hint_screen_texture, filter_linear, repeat_disable;
uniform sampler2D clear_depth : hint_depth_texture, filter_nearest, repeat_disable;
#endif
// Raster reflections from above (water_raster_reflections.h).
)";
const char *RIVER_SHADER_SRC_TAIL = R"(
varying vec3 v_world;
varying vec2 v_flow;
varying vec4 v_info;
varying vec3 v_normal;
varying vec4 v_wave; // undular train: amplitude, wavenumber, toe arc length, decay length
varying float v_s;

float r_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float r_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(r_hash(i), r_hash(i + vec2(1.0, 0.0)), u.x),
			mix(r_hash(i + vec2(0.0, 1.0)), r_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Value noise's gradient (Quilez): the slope of a ripple octave.
vec2 r_noise_grad(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 du = 6.0 * f * (1.0 - f);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = r_hash(i);
	float b = r_hash(i + vec2(1.0, 0.0));
	float c = r_hash(i + vec2(0.0, 1.0));
	float d = r_hash(i + vec2(1.0, 1.0));
	return du * (vec2(b - a, c - a) + (a - b - c + d) * u.yx);
}

// Slope of the ripples at p (three octaves, each half the height of the last
// per doubling of frequency -- roughly a capillary-gravity spectrum).
vec2 ripple_slope(vec2 p) {
	vec2 s = r_noise_grad(p * 1.7) * 0.5;
	s += r_noise_grad(p * 3.9 + 11.0) * 0.3;
	s += r_noise_grad(p * 8.6 - 7.0) * 0.2;
	return s;
}

// Gust ripples (docs/ideas/wind.md): the shared wind roughens the water where it blows. The short waves' mean
// square slope follows the local wind as Cox and Munk (1954) measured it (0.00512 U10 beyond the calm), and
// vanishes below the capillary onset (about 1 m/s), so lulls leave glassy patches and gusts sweep darker cat's
// paws over the river. The wavelets' crests lie across the wind; they ride the current and creep downwind at
// about the slowest capillary-gravity phase speed (0.23 m/s). Narrow water is fetch-limited: wind_ripple_strength.
uniform bool wind_ripples = true;
uniform float wind_ripple_strength : hint_range(0.0, 2.0, 0.01) = 0.5;
vec2 river_wind_slope(vec2 xz, vec2 flow) {
#ifdef RIVER_WEATHER
	if (!wind_ripples || weather_map_rect.z <= 0.0) {
		return vec2(0.0);
	}
	float z0 = max(weather_wind_info.w, 1e-5);
	vec3 w = weather_wind(vec3(xz.x, 0.0, xz.y), 2.0);
	float u10 = length(w.xz) / max(weather_open_profile(2.0, z0), 1e-3);
	float var = (0.0025 + 0.00512 * u10) * smoothstep(0.7, 2.5, u10);
	if (var <= 1e-6) {
		return vec2(0.0);
	}
	vec2 d = w.xz / max(length(w.xz), 1e-4);
	vec2 c = vec2(-d.y, d.x);
	vec2 q = xz - (flow + d * 0.23) * water_time;
	// Short along the wind (crests across it), longer across it.
	vec2 g = ripple_slope(vec2(dot(q, d) * 9.0, dot(q, c) * 4.0) + vec2(31.7, 3.3));
	// ripple_slope's rms is about 0.35 per axis: scale it to the Cox-Munk rms.
	return (d * g.x + c * g.y * 0.6) * (sqrt(var) * wind_ripple_strength / 0.35);
#else
	return vec2(0.0);
#endif
}

// Capillary octaves under ripple_slope (wavelengths of about 5, 2.3 and 1 cm at ripple_scale 1), each faded by the
// pixel's footprint (m); r_hidden: the mean square slope faded out, for the roughness.
const float RIPPLE_DETAIL_FREQ[3] = float[3](19.0, 41.0, 88.0);
const float RIPPLE_DETAIL_AMP[3] = float[3](0.16, 0.11, 0.07);
vec2 ripple_detail_slope(vec2 p, float footprint, out float r_hidden) {
	vec2 s = vec2(0.0);
	r_hidden = 0.0;
	for (int i = 0; i < 3; i++) {
		float f = RIPPLE_DETAIL_FREQ[i];
		// A noise cell is 1/f m: full while it spans four pixels or more, gone under one.
		float shown = 1.0 - smoothstep(0.25, 1.0, footprint * f * ripple_scale);
		float a = RIPPLE_DETAIL_AMP[i];
		if (shown > 0.0) {
			s += r_noise_grad(p * f + vec2(3.1 * float(i), -7.7 * float(i))) * a * shown;
		}
		r_hidden += 0.5 * a * a * (1.0 - shown); // (value-noise gradients: mean square about half the peak)
	}
	return s;
}

// The white water's structure (the open sea's foam_structure, PhysXWaterSurface3D; kept in step with it): holes
// torn at three scales, strands along their borders, rafts, bubble grain, the crisp outline. p in world metres,
// drawn out along p_streak (the current) by 1 + p_stretch.
float rf_fbm(vec2 p) {
	return 0.5 * r_noise(p) + 0.3 * r_noise(p * 2.3 + 7.1) + 0.2 * r_noise(p * 5.1 - 3.3);
}

vec4 rf_holes(vec2 p, float open, float scale, float bias, float soft, float jit) {
	p += 0.22 * (vec2(rf_fbm(p * 2.3 + 1.3), rf_fbm(p * 2.3 - 4.1)) - 0.5);
	float lobe = rf_fbm(p * 3.7 + 7.3) - 0.5;
	float tear = rf_fbm(p * 9.1 - 2.6) - 0.5;
	vec2 i = floor(p);
	vec2 f = fract(p);
	float d1 = 8.0;
	float d2 = 8.0;
	float keep = 1.0;
	float inside = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec2 g = vec2(float(x), float(y));
			vec2 o = vec2(r_hash(i + g), r_hash(i + g + vec2(17.3, 5.1)));
			vec2 r = g + 0.15 + 0.7 * o - f;
			float d = length(r);
			if (d < d1) {
				d2 = d1;
				d1 = d;
			} else if (d < d2) {
				d2 = d;
			}
			float rad = min(open * max(r_hash(i + g + vec2(41.7, 23.9)) * scale - bias, 0.0), 0.65);
			if (rad > 0.0) {
				float de = d + jit + rad * (0.6 * lobe + 0.25 * tear);
				keep = min(keep, smoothstep(rad - soft, rad + soft, de));
				inside = max(inside, (rad - de) / rad);
			}
		}
	}
	return vec4(keep, d1, d2, inside);
}

float rf_structure(vec2 p, float amount, float footprint, vec2 p_streak, float p_stretch) {
	vec2 q0 = p * foam_detail_scale;
	vec2 q = vec2(dot(q0, p_streak) / (1.0 + p_stretch), dot(q0, vec2(-p_streak.y, p_streak.x)));
	vec2 warp = vec2(r_noise(q * 0.9 + 3.7), r_noise(q * 0.9 - 8.1)) - 0.5;
	float a = clamp(amount, 0.0, 1.0);
	float open = 1.0 - pow(a, 0.6);
	float age_warp = 1.0 + 1.2 * open;
	float rag = rf_fbm(q * 9.0) - 0.5;
	float soft1 = 0.015 + footprint * foam_detail_scale * 1.0;
	float soft2 = 0.015 + footprint * foam_detail_scale * 2.8;
	float soft3 = 0.02 + footprint * foam_detail_scale * 9.2;
	vec4 h1 = rf_holes(q * 0.5 + warp * 0.8 * age_warp, open, 1.5, 0.3, soft1, 0.22 * rag);
	vec4 h2 = rf_holes(q * 1.4 + warp * 1.4 * age_warp + 11.0, open, 1.4, 0.25, soft2, 0.28 * rag);
	vec4 h3 = rf_holes(q * 4.6 + warp * 2.0 * age_warp - 5.0, open, 1.2, 0.15, soft3, 0.32 * rag);
	float cover = h1.x * h2.x * mix(1.0, h3.x, 0.75);
	float brk = smoothstep(0.4, 0.62, rf_fbm(q * 2.7 + 1.7));
	float w = (0.04 + 0.07 * a) * brk;
	float strands = max(1.0 - smoothstep(w - soft1, w + soft1, h1.z - h1.y + 0.2 * rag), 1.0 - smoothstep(w - soft2, w + soft2, h2.z - h2.y + 0.25 * rag));
	cover = max(cover, strands * smoothstep(0.15, 0.45, a));
	float raft = smoothstep(0.62, 0.68, rf_fbm(q * 6.3 + 3.1)) * smoothstep(0.2, 0.5, a) * smoothstep(0.35, 0.5, max(h1.w, h2.w));
	cover = max(cover, raft * (1.0 - clamp(footprint * foam_detail_scale * 20.0 - 0.2, 0.0, 1.0)));
	cover = max(cover, 0.22 * smoothstep(0.05, 0.4, a) * smoothstep(0.35, 0.75, rf_fbm(q * 5.3 - 4.1)));
	float coarse = clamp(footprint * foam_detail_scale * 1.2 - 0.25, 0.0, 1.0);
	cover = mix(cover, clamp(1.0 - 1.4 * open * open, 0.05, 1.0), coarse);
	float grain = 1.0 - clamp((footprint - 0.015) / 0.04, 0.0, 1.0);
	float cluster = 1.0 - clamp((footprint - 0.04) / 0.12, 0.0, 1.0);
	vec2 pw = p + 0.08 * warp;
	float clumps = rf_fbm(pw * 16.0 + 4.7);
	float gaps = smoothstep(0.3, 0.12, rf_fbm(pw * 31.0 - 2.9));
	float domes = smoothstep(0.8, 0.95, rf_fbm(pw * 38.0 + 9.3));
	float bubbles = mix(0.84, (0.62 + 0.45 * clumps) * (1.0 - 0.3 * gaps) + 0.12 * domes, grain);
	bubbles *= mix(0.925, 0.7 + 0.45 * rf_fbm(pw * 3.5 + 2.3), cluster);
	float density = smoothstep(0.02, 0.35, a);
	float rough = 0.6 * rf_fbm(q * 3.1 + 5.3) + 0.4 * rf_fbm(q * 11.0 - 2.1);
	float t_edge = 0.32 + 0.5 * (rough - 0.5);
	float w_edge = clamp(0.02 + footprint * foam_detail_scale * 2.0, 0.02, 0.3);
	float outline = max(smoothstep(t_edge - w_edge, t_edge + w_edge, density) * mix(0.55, 1.0, smoothstep(0.35, 0.7, a)), 0.8 * density);
	density = mix(density, outline, foam_crisp);
	bubbles = mix(1.0, bubbles, 1.0 - 0.55 * foam_crisp);
	return clamp(cover * density * bubbles, 0.0, 1.0);
}

// The bubbles' domes (the clumps' smooth height), for the light to catch the foam's grain.
vec2 rf_bubble_slope(vec2 p, float footprint) {
	float grain = 1.0 - clamp((footprint - 0.015) / 0.04, 0.0, 1.0);
	if (grain <= 0.0) {
		return vec2(0.0);
	}
	const float e = 0.01;
	float h0 = rf_fbm(p * 16.0 + 4.7);
	float hx = rf_fbm((p + vec2(e, 0.0)) * 16.0 + 4.7);
	float hz = rf_fbm((p + vec2(0.0, e)) * 16.0 + 4.7);
	return vec2(hx - h0, hz - h0) / e * 0.03 * grain;
}

// The two phases' offsets and the first one's weight.
float flow_phases(vec2 p, vec2 flow, out vec2 o_a, out vec2 o_b) {
	float t = water_time / max(flow_period, 0.05) + r_noise(p * 0.05);
	float p_a = fract(t);
	float p_b = fract(t + 0.5);
	o_a = flow * (p_a - 0.5) * flow_period;
	o_b = flow * (p_b - 0.5) * flow_period;
	return 1.0 - abs(1.0 - 2.0 * p_a);
}

// How far inside the live window a world point is: 1 inside, easing to 0 over
// its outer edge; and its texture coordinate.
float live_weight(vec2 world_xz, out vec2 cuv) {
	vec2 rel = (world_xz - live_center) / live_size;
	cuv = clamp(rel + 0.5, vec2(0.0), vec2(1.0));
	vec2 a = abs(rel);
	return live_enabled ? 1.0 - smoothstep(0.44, 0.5, max(a.x, a.y)) : 0.0;
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_flow = (MODEL_MATRIX * vec4(UV2.x, 0.0, UV2.y, 0.0)).xz;
	v_info = COLOR;
	v_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_wave = CUSTOM0;
	v_s = UV.x;
	{
		float x = UV.x - CUSTOM0.z;
		if (CUSTOM0.x > 0.0 && x > 0.0) {
			// Height only where the mesh can carry it (four sections or more a wavelength), fading at the banks;
			// the slope is drawn per pixel below.
			float mesh_share = smoothstep(3.0, 6.0, TAU / (CUSTOM0.y * section_step));
			float bank = smoothstep(0.0, 0.2, COLOR.r * 4.0 - 1.0);
			float h = mesh_share * bank * CUSTOM0.x * sin(CUSTOM0.y * x) * exp(-x / CUSTOM0.w) * min(x * CUSTOM0.y / PI, 1.0);
			VERTEX.y += h;
			v_world.y += h;
		}
	}
	vec2 cuv;
	float live = live_weight(v_world.xz, cuv);
	if (live > 0.0) {
		// The live water's rise and fall on the river's surface, onto the banks as far as the live
		// water runs up them (1 m, the solver's run-up band; a dry bank reads below its ground).
		vec2 off = v_world.xz - live_center;
		float h = (textureLod(live_height_tex, cuv, 0.0).r - (live_tilt.x + dot(live_tilt.yz, off))) * live * step(-0.99, v_info.r * 4.0 - 1.0);
		VERTEX.y += h;
		v_world.y += h;
	}
}

void fragment() {
	vec2 flow = v_flow;
	float live_foam = 0.0;
	vec2 live_slope = vec2(0.0);
	vec2 cuv;
	float live = live_weight(v_world.xz, cuv);
	if (live > 0.0) {
		vec4 lf = texture(live_flow_tex, cuv);
		flow = mix(flow, lf.rg, live);
		live_foam = lf.b * live;
		vec2 du = live_texel / live_size;
		float hx = texture(live_height_tex, cuv + vec2(du.x, 0.0)).r - texture(live_height_tex, cuv - vec2(du.x, 0.0)).r;
		float hz = texture(live_height_tex, cuv + vec2(0.0, du.y)).r - texture(live_height_tex, cuv - vec2(0.0, du.y)).r;
		live_slope = (vec2(hx, hz) / (2.0 * live_texel) - live_tilt.yz) * live;
	}
	vec2 p = v_world.xz * ripple_scale;
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(v_world.xz, flow, o_a, o_b);
	if (live_advected && live > 0.0) {
		vec4 d = texture(live_offset_tex, cuv) * live;
		o_a += d.xy;
		o_b += d.zw;
	}
	// Surface-gradient blend of the two phases, renormalised so the crossover
	// doesn't flatten the ripples (phase B is shifted half a tile, so the two are
	// close to independent).
	vec2 slope = (ripple_slope(p - o_a * ripple_scale) * w + ripple_slope(p - o_b * ripple_scale + vec2(13.1, 7.7)) * (1.0 - w))
			/ sqrt(w * w + (1.0 - w) * (1.0 - w));
	// The pixel's footprint on the water (m); the path tracer has no derivatives and draws every octave.
#if defined(RT)
	float footprint = 0.0;
#else
	vec2 fw_xz = fwidth(v_world.xz);
	float footprint = max(fw_xz.x, fw_xz.y);
#endif
	float ripple_hidden = 0.0;
	if (ripple_detail > 0.0) {
		float h_a;
		float h_b;
		vec2 d_a = ripple_detail_slope(p - o_a * ripple_scale, footprint, h_a);
		vec2 d_b = ripple_detail_slope(p - o_b * ripple_scale + vec2(13.1, 7.7), footprint, h_b);
		slope += ripple_detail * (d_a * w + d_b * (1.0 - w)) / sqrt(w * w + (1.0 - w) * (1.0 - w));
		ripple_hidden = ripple_detail * ripple_detail * (h_a * w + h_b * (1.0 - w));
	}
	float speed = length(flow);
	float depth = v_info.r * 4.0 - 1.0;
	// The waterline laps: carried by the current (the same two phases as the
	// ripples) and breathing in and out over a few seconds.
	vec2 e_a = (v_world.xz - o_a) * edge_lap_scale;
	vec2 e_b = (v_world.xz - o_b) * edge_lap_scale + vec2(3.7, 9.1);
	float lap_n = (0.6 * r_noise(e_a) + 0.4 * r_noise(e_a * 2.9 + 4.0)) * w + (0.6 * r_noise(e_b) + 0.4 * r_noise(e_b * 2.9 + 4.0)) * (1.0 - w);
	float breathe = 0.5 + 0.5 * sin(water_time * 1.3 + r_noise(v_world.xz * 0.15) * 6.28318);
	depth += edge_lap_height * ((lap_n - 0.5) * 2.0 + (breathe - 0.5));
	vec2 rq = v_world.xz * 0.35;
	float relief = 0.5 * r_noise(rq) + 0.3 * r_noise(rq * 3.1 + 17.0) + 0.2 * r_noise(rq * 8.3 - 5.0);
	depth += edge_relief * (relief - 0.5) * 2.0;
	// Calm at the waterline: ripples die out over the last 15 cm.
	float ripple_amp = (ripple_strength + ripple_speed_strength * speed) * smoothstep(0.0, 0.15, depth);
	slope *= ripple_amp;
	ripple_hidden *= ripple_amp * ripple_amp;
	slope += river_wind_slope(v_world.xz, flow) * smoothstep(0.0, 0.15, depth);
	vec3 n_mesh = normalize(v_normal);
	if (n_mesh.y < 0.0) {
		n_mesh = -n_mesh;
	}
	slope += live_slope * smoothstep(0.0, 0.15, depth);
	{
		// The undular train's slope along the current (stationary waves: d/ds of a sin(kx) e^(-x/L)).
		float x = v_s - v_wave.z;
		if (v_wave.x > 0.0 && x > 0.0 && speed > 1e-3) {
			float e = exp(-x / v_wave.w) * min(x * v_wave.y / PI, 1.0);
			float dh = v_wave.x * e * (v_wave.y * cos(v_wave.y * x) - sin(v_wave.y * x) / v_wave.w);
			slope += flow / speed * dh * smoothstep(0.0, 0.2, depth);
		}
	}
	// Raindrop rings (rain_ripples(), docs/ideas/rain.md), carried by the current: each ring rides the water it
	// fell on, so the rings are read in the two flow phases the ripples use and blended the same way.
	float river_roughness = pathtraced_roughness;
	if (rain_rings) {
		float rr_rate = weather_rainfall_at(vec3(v_world.x, weather_precip_phase.w, v_world.z));
		if (rr_rate > 0.0) {
#if defined(RT)
			float rr_fp = 0.0;
#else
			vec2 rr_fw = fwidth(v_world.xz);
			float rr_fp = max(max(rr_fw.x, rr_fw.y), 1e-5);
#endif
			vec3 rr = rain_ripples(v_world.xz - o_a, TIME, rr_rate, rr_fp) * w + rain_ripples(v_world.xz - o_b + vec2(13.1, 7.7), TIME, rr_rate, rr_fp) * (1.0 - w);
			slope += rr.xy * smoothstep(0.0, 0.05, depth);
			river_roughness = rain_ripple_roughness(river_roughness, rr.z);
		}
	}
	vec3 world_normal = normalize(n_mesh - vec3(slope.x, 0.0, slope.y));

	// White water, broken into lace that rides the current.
	vec2 q_a = (v_world.xz - o_a) * foam_detail_scale;
	vec2 q_b = (v_world.xz - o_b) * foam_detail_scale + vec2(5.3, 2.1);
	float lace_a = 0.5 * r_noise(q_a) + 0.3 * r_noise(q_a * 2.7 + 17.0) + 0.2 * r_noise(q_a * 7.3 - 5.0);
	float lace_b = 0.5 * r_noise(q_b) + 0.3 * r_noise(q_b * 2.7 + 17.0) + 0.2 * r_noise(q_b * 7.3 - 5.0);
	float lace = lace_a * w + lace_b * (1.0 - w);
	// Foam caught along the waterline, broken into lace.
	float edge_band = edge_foam * (1.0 - smoothstep(0.0, 0.12, depth)) * smoothstep(-0.01, 0.02, depth);
	// Capped below 1 so the lace always opens: at full strength the white water would cover whole sections of
	// the ribbon as solid blocks with straight ends.
	float foam = clamp(min(max(max(v_info.g, live_foam), edge_band), 0.85) * (1.0 + foam_breakup) - lace * foam_breakup, 0.0, 1.0);
	foam = foam * foam * (3.0 - 2.0 * foam);
	if (foam_crisp > 0.0) {
		// The white water's amount drawn as foam (rf_structure), each phase riding the current (the flow mapping
		// draws it out along the current by itself). Not rotated to the local current: world coordinates turned by
		// an angle that varies over the water shear by that angle times their distance from the origin, combing
		// the pattern into hairlines wherever the current turns.
		// The amount the structure tears is the white water as the soft lace drew it (where and how much), so the
		// crisp foam covers what the soft foam did, not every trace of live foam.
		float amount = foam;
		const vec2 sd = vec2(1.0, 0.0);
		const float stretch = 0.0;
		float f_a = rf_structure(v_world.xz - o_a, amount, footprint, sd, stretch);
		float f_b = rf_structure(v_world.xz - o_b + vec2(5.3, 2.1), amount, footprint, sd, stretch);
		float crisp = f_a * w + f_b * (1.0 - w);
		float foam_soft = crisp;
#if !defined(RT)
		// Crisp at every distance: the edge stepped once more at the pixel's own scale (about three pixels wide),
		// up only as far as the foam beside it reaches, keeping 0.8 of the soft value (the open sea's step).
		float fwf = max(fwidth(crisp), 1e-3);
		float stepped = clamp((crisp - 0.3) / (3.0 * fwf) + 0.5, 0.0, 1.0) * min(crisp + 2.0 * fwf, 1.0);
		crisp = max(stepped, 0.8 * crisp);
#endif
		// The milky haze thin white water leaves between the patches stays (a third of the soft foam).
		crisp = max(crisp, 0.35 * foam);
		foam = mix(foam, crisp, foam_crisp);
		if (foam > 0.01) {
			// Foam damps the ripples under it (the film's elasticity, as slicks do), its bubbles' domes catch the
			// light, and its rims stand up as a thin skin.
			world_normal = normalize(mix(world_normal, n_mesh, clamp(foam, 0.0, 1.0)));
			vec2 bs = rf_bubble_slope(v_world.xz - o_a, footprint) * w + rf_bubble_slope(v_world.xz - o_b + vec2(5.3, 2.1), footprint) * (1.0 - w);
			world_normal = normalize(world_normal - vec3(bs.x, 0.0, bs.y) * foam * foam_grain_tilt);
#if !defined(RT)
			if (foam_relief > 0.0) {
				vec2 rdx = dFdx(v_world.xz);
				vec2 rdy = dFdy(v_world.xz);
				float fdx = dFdx(foam_soft);
				float fdy = dFdy(foam_soft);
				float rdet = rdx.x * rdy.y - rdx.y * rdy.x;
				if (abs(rdet) > 1e-12) {
					vec2 fg = vec2(rdy.y * fdx - rdx.y * fdy, rdx.x * fdy - rdy.x * fdx) / rdet;
					fg = clamp(fg * 0.03 * foam_relief, vec2(-1.2), vec2(1.2));
					world_normal = normalize(world_normal - vec3(fg.x, 0.0, fg.y));
				}
			}
#endif
		}
	}
	// The ribbon's own sides (where water spread over flat ground reaches them) and its fade into the water at the
	// mouth, broken by the bank's relief, so neither shows as a ruler-straight edge.
	float bank_fade = smoothstep(0.0, 0.7, v_info.b * 1.6 - 0.45 + (relief - 0.5) * 0.9);

#if defined(RT)
	NORMAL = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	ALBEDO = mix(vec3(1.0), foam_color.rgb, foam);
	ROUGHNESS = mix(river_roughness, 0.8, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	TRANSMISSION = 1.0 - foam;
	IOR = 1.33;
	// Clear over the last few cm to the waterline: there the water is a film, but a ray let through it at a grazing
	// angle can run on under the bank's ground through metres of the medium, tinting a dotted line along the shore.
	float film = smoothstep(0.0, 0.12, depth);
	ABSORPTION = pathtraced_absorption * film;
	SCATTERING = pathtraced_scattering * film;
	SCATTERING_ANISOTROPY = pathtraced_scattering_anisotropy;
	// Gone past the waterline and the banks' fade, as in raster.
	ALPHA = smoothstep(0.0, max(edge_softness, 1e-3), depth) * bank_fade;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#else
	// Open water keeps PhysXWaterSurface3D's raster look: its own colour with a
	// small bounded shading term, plus the sky and the sun's glitter, written to
	// EMISSION (no ALBEDO, no SPECULAR, so the lights add nothing to it). White
	// water is a rough diffuse surface: it goes to ALBEDO and takes the scene's
	// lights and shadows, so a cascade in a shaded valley no longer glows (the
	// path tracer already lit it; docs/ideas/water.md "Step 4f").
	vec3 n = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	float ndotl = clamp(dot(n, vec3(0.3, 0.85, 0.4)), 0.0, 1.0);
	float deep = smoothstep(0.0, 1.2, depth);
	vec3 unlit_rgb = mix(shallow_color.rgb, water_color.rgb, deep) * mix(0.75, 1.15, ndotl);
	vec3 base_rgb = unlit_rgb * water_raster_body_light();
	float base_a = mix(shallow_color.a, water_color.a, deep);
	// Also with raster_reflections off (underground, no sky): the bed shows through all the same, lit by the scene.
	if (clear_water) {
		// The bed through the water, refracted, in place of the tint and its blend; its sun is inside it.
		vec3 sun_dir;
		vec3 sun_irr;
		vec3 sky;
		water_light_in_water(sun_dir, sun_irr, sky);
		base_rgb = water_clear_through(clear_screen, clear_depth, v_world, normalize(v_world - CAMERA_POSITION_WORLD), world_normal, SCREEN_UV, max(v_info.r * 4.0 - 1.0, 0.0), pathtraced_absorption, pathtraced_scattering, pathtraced_scattering_anisotropy, sun_dir, sun_irr, sky, water_jitter(FRAGCOORD.xy, TIME), PROJECTION_MATRIX, VIEW_MATRIX, INV_PROJECTION_MATRIX, INV_VIEW_MATRIX);
		unlit_rgb = vec3(0.0);
		base_a = 1.0;
	}
	float edge = smoothstep(0.0, max(edge_softness, 1e-3), depth) * bank_fade;
	{
		float zd = texture(clear_depth, SCREEN_UV).r;
		if (zd > 0.0 && shore_depth_fade > 0.0) {
			vec4 b = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, zd, 1.0);
			float th = v_world.y - (INV_VIEW_MATRIX * vec4(b.xyz / b.w, 1.0)).y;
			edge *= smoothstep(0.0, shore_depth_fade, th);
		}
	}
	float a0 = mix(base_a, foam_color.a, foam) * edge;
	// The reflection term is linear in the colour, so the water's part and the
	// foam's part split exactly: what was one unshaded colour is now emission
	// plus a lit albedo, the same where the light is the old fixed one.
	// The sun's share (body light and glitter) goes through light(), so the water takes the scene's shadows.
	vec3 sun_body;
	vec4 sun_glint;
	float sun_a2;
	// The capillary ripples finer than the pixel, as roughness (GGX alpha squared plus their slope variance).
	float rough_eff = pow(pow(river_roughness, 4.0) + ripple_hidden, 0.25);
	vec4 refl = water_raster_reflect(base_rgb * (1.0 - foam), a0, world_normal, v_world, CAMERA_POSITION_WORLD, edge * (1.0 - foam), rough_eff, unlit_rgb * (1.0 - foam), sun_body, sun_glint, sun_a2);
	if (!raster_reflections && screen_reflections) {
		refl = water_screen_reflect(clear_screen, clear_depth, refl, n, VERTEX, SCREEN_UV, edge * (1.0 - foam), water_jitter(FRAGCOORD.xy, TIME), PROJECTION_MATRIX, INV_PROJECTION_MATRIX);
	}
	wr_sun_body = sun_body;
	wr_sun_glint = sun_glint;
	wr_sun_a2 = sun_a2;
	float a_new = max(refl.a, 1e-4);
	// Light the surface let through (1 - F), recovered from the alpha it returned.
	float through = a0 < 0.9999 ? clamp((1.0 - refl.a) / (1.0 - a0), 0.0, 1.0) : 1.0;
	float keep = a0 * through / a_new;
	NORMAL = n;
	EMISSION = refl.rgb;
	ALBEDO = foam_color.rgb * foam * keep;
	ROUGHNESS = 0.85;
	SPECULAR = 0.25 * foam;
	METALLIC = 0.0;
	ALPHA = refl.a;
	if (froude_overlay) {
		float fr = v_info.a * 4.0;
		vec3 c = fr < 1.0 ? mix(vec3(0.05, 0.15, 0.9), vec3(0.1, 0.85, 0.2), smoothstep(0.3, 1.0, fr)) : mix(vec3(0.1, 0.85, 0.2), vec3(0.95, 0.1, 0.05), smoothstep(1.0, 2.5, fr));
		c = v_info.a > 0.99 ? vec3(1.0) : c;
		EMISSION = c * 0.8;
		ALBEDO = vec3(0.0);
		ALPHA = max(edge, 0.85 * step(0.01, edge));
	}
#endif
}

#if !defined(RT)
// The lights on the river: the white water (ALBEDO) as a Lambert surface, and the open water's sun share
// (water_raster_reflections.h), all under each light's shadow and cloud projector.
void light() {
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * max(dot(NORMAL, LIGHT), 0.0) / PI;
	SPECULAR_LIGHT += water_raster_sun_light(LIGHT, VIEW, LIGHT_COLOR * ATTENUATION, LIGHT_IS_DIRECTIONAL, INV_VIEW_MATRIX, wr_sun_body, wr_sun_glint, wr_sun_a2);
}
#endif
)";

// The shared wind for the gust ripples (river_wind_slope), when the weather module is in.
static String _river_wind_src() {
#ifdef MODULE_WEATHER_ENABLED
	return "\n#define RIVER_WEATHER\n" + WeatherMap3D::get_wind_shader_code();
#else
	return String();
#endif
}

// The whole shader: head, the shared wind, the raster reflections (water_raster_reflections.h), tail.
static const String &river_shader_src() {
	static const String src = String(RIVER_SHADER_SRC_HEAD) + BaseMaterial3D::get_rain_functions_code(true) + "\nuniform bool rain_rings = true;\n" + _river_wind_src() + WATER_RASTER_REFLECTIONS_GLSL + RIVER_SHADER_SRC_TAIL;
	return src;
}

// A waterfall's falling sheet: a thin slab of water (two faces q/|v| apart, so
// the path tracer refracts through a real sheet). Per vertex (built by
// PhysXRiver3D): UV = (travel time since the lip in s, distance across in m),
// UV2.x = speed (m/s), COLOR.r = fade at the sheet's edges, COLOR.g =
// aeration (white water), COLOR.b = how far past its break-up length the
// sheet has fallen (0 whole, 1 broken into clumps).
const char *FALL_SHADER_SRC = R"(
shader_type spatial;
#if defined(RT)
render_mode cull_back;
#else
render_mode blend_mix, cull_back;
#endif

uniform float water_time = 0.0;
uniform vec4 water_color : source_color = vec4(0.30, 0.42, 0.42, 0.45);
uniform vec4 foam_color : source_color = vec4(0.92, 0.95, 0.97, 0.95);
uniform vec3 pathtraced_absorption = vec3(0.6, 0.14, 0.1);
// The water a ray crosses through the sheet, for its tint under path tracing (m).
uniform float sheet_thickness = 0.1;
uniform float pathtraced_roughness : hint_range(0.0, 1.0) = 0.05;
// Streaks: per metre across, and per second of travel along (the pattern rides
// the water, so it stretches as the sheet speeds up).
uniform float streak_scale = 3.0;
uniform float streak_rate = 5.0;

float f_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float f_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(f_hash(i), f_hash(i + vec2(1.0, 0.0)), u.x),
			mix(f_hash(i + vec2(0.0, 1.0)), f_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void fragment() {
	float edge = COLOR.r;
	float aeration = COLOR.g;
	float broken = COLOR.b;
	vec2 p = vec2(UV.y * streak_scale, (UV.x - water_time) * streak_rate);
	float streak = 0.55 * f_noise(p * vec2(1.0, 0.35)) + 0.3 * f_noise(p * vec2(2.3, 0.9) + 7.0) + 0.15 * f_noise(p * vec2(5.1, 2.4) - 3.0);
	float foam = smoothstep(0.15, 0.85, aeration + (streak - 0.5) * 0.9);
	// Past break-up the sheet is clumps and streaks with air between them; the
	// thin edges go first.
	float holes = broken * 0.8 + (1.0 - edge) * 0.6;
	float cover = smoothstep(-0.04, 0.04, streak - holes + 0.12);
#if defined(RT)
	// A falling sheet is one surface with holes: traced as a thin wall (straight through, tinted by its thickness),
	// not as the boundary of a medium, which a path leaving through a hole never left (see the contact sheet).
	ALBEDO = mix(exp(-pathtraced_absorption * sheet_thickness), foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness, 0.8, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	TRANSMISSION = 1.0 - foam;
	ALPHA = cover;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#else
	// Lit by the scene's lights, shadows and sky like any surface (it was unshaded with a fixed light,
	// so a fall in a shaded valley glowed flat white): white water scatters diffusely, the clear sheet
	// is a smooth dielectric showing the sky in its specular.
	ALBEDO = mix(water_color.rgb, foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness + 0.1, 0.85, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	ALPHA = mix(water_color.a, foam_color.a, foam) * cover;
#endif
}
)";

constexpr float GRAVITY = 9.81f;
constexpr float MAX_SPEED = 12.0f; // m/s cap (a fall's sheet, not a sustained river)
constexpr float CASCADE_MIN_DROP = 0.15f; // m

} // namespace

// The river's surface shader, shared with PhysXLiveWater3D (same per-vertex layout).
const String &physx_river_water_shader_src() {
	return river_shader_src();
}

String PhysXRiver3D::get_fall_shader_code() {
	return String(FALL_SHADER_SRC);
}

namespace {
float cross_value(const PhysXRiver3D::Section &p_sec, const LocalVector<float> &p_values, float p_n, float p_margin, int p_m) {
	const float half = 0.5f * p_sec.width + p_margin;
	if (!Math::is_finite(p_n)) {
		return p_values[0];
	}
	const float u = CLAMP((p_n + half) / (2.0f * half) * p_m, 0.0f, (float)p_m);
	const int j = MIN((int)u, p_m - 1);
	return Math::lerp(p_values[j], p_values[j + 1], u - j);
}
} // namespace

void PhysXRiver3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_widths", "widths"), &PhysXRiver3D::set_widths);
	ClassDB::bind_method(D_METHOD("get_widths"), &PhysXRiver3D::get_widths);
	ClassDB::bind_method(D_METHOD("set_depths", "depths"), &PhysXRiver3D::set_depths);
	ClassDB::bind_method(D_METHOD("get_depths"), &PhysXRiver3D::get_depths);
	ClassDB::bind_method(D_METHOD("set_default_width", "width"), &PhysXRiver3D::set_default_width);
	ClassDB::bind_method(D_METHOD("get_default_width"), &PhysXRiver3D::get_default_width);
	ClassDB::bind_method(D_METHOD("set_discharge", "discharge"), &PhysXRiver3D::set_discharge);
	ClassDB::bind_method(D_METHOD("get_discharge"), &PhysXRiver3D::get_discharge);
	ClassDB::bind_method(D_METHOD("set_rain_response", "enable"), &PhysXRiver3D::set_rain_response);
	ClassDB::bind_method(D_METHOD("get_rain_response"), &PhysXRiver3D::get_rain_response);
	ClassDB::bind_method(D_METHOD("get_rain_discharge"), &PhysXRiver3D::get_rain_discharge);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &PhysXRiver3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &PhysXRiver3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_max_slope", "slope"), &PhysXRiver3D::set_max_slope);
	ClassDB::bind_method(D_METHOD("set_gradually_varied_flow", "enabled"), &PhysXRiver3D::set_gradually_varied_flow);
	ClassDB::bind_method(D_METHOD("get_gradually_varied_flow"), &PhysXRiver3D::get_gradually_varied_flow);
	ClassDB::bind_method(D_METHOD("set_bend_superelevation", "enabled"), &PhysXRiver3D::set_bend_superelevation);
	ClassDB::bind_method(D_METHOD("get_bend_superelevation"), &PhysXRiver3D::get_bend_superelevation);
	ClassDB::bind_method(D_METHOD("set_standing_waves", "enabled"), &PhysXRiver3D::set_standing_waves);
	ClassDB::bind_method(D_METHOD("get_standing_waves"), &PhysXRiver3D::get_standing_waves);
	ClassDB::bind_method(D_METHOD("get_max_slope"), &PhysXRiver3D::get_max_slope);
	ClassDB::bind_method(D_METHOD("set_snap_to_ground", "snap"), &PhysXRiver3D::set_snap_to_ground);
	ClassDB::bind_method(D_METHOD("get_snap_to_ground"), &PhysXRiver3D::get_snap_to_ground);
	ClassDB::bind_method(D_METHOD("set_ground_probe_height", "height"), &PhysXRiver3D::set_ground_probe_height);
	ClassDB::bind_method(D_METHOD("get_ground_probe_height"), &PhysXRiver3D::get_ground_probe_height);
	ClassDB::bind_method(D_METHOD("set_ground_probe_depth", "depth"), &PhysXRiver3D::set_ground_probe_depth);
	ClassDB::bind_method(D_METHOD("get_ground_probe_depth"), &PhysXRiver3D::get_ground_probe_depth);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &PhysXRiver3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &PhysXRiver3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_obstacle_collision_mask", "mask"), &PhysXRiver3D::set_obstacle_collision_mask);
	ClassDB::bind_method(D_METHOD("get_obstacle_collision_mask"), &PhysXRiver3D::get_obstacle_collision_mask);
	ClassDB::bind_method(D_METHOD("set_step_length", "length"), &PhysXRiver3D::set_step_length);
	ClassDB::bind_method(D_METHOD("get_step_length"), &PhysXRiver3D::get_step_length);
	ClassDB::bind_method(D_METHOD("set_cross_segments", "segments"), &PhysXRiver3D::set_cross_segments);
	ClassDB::bind_method(D_METHOD("get_cross_segments"), &PhysXRiver3D::get_cross_segments);
	ClassDB::bind_method(D_METHOD("set_bank_margin", "margin"), &PhysXRiver3D::set_bank_margin);
	ClassDB::bind_method(D_METHOD("get_bank_margin"), &PhysXRiver3D::get_bank_margin);
	ClassDB::bind_method(D_METHOD("set_foam_persistence", "seconds"), &PhysXRiver3D::set_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_foam_persistence"), &PhysXRiver3D::get_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_source_lake", "path"), &PhysXRiver3D::set_source_lake);
	ClassDB::bind_method(D_METHOD("get_source_lake"), &PhysXRiver3D::get_source_lake);
	ClassDB::bind_method(D_METHOD("set_outlet_coefficient", "coefficient"), &PhysXRiver3D::set_outlet_coefficient);
	ClassDB::bind_method(D_METHOD("get_outlet_coefficient"), &PhysXRiver3D::get_outlet_coefficient);
	ClassDB::bind_method(D_METHOD("get_outlet_discharge"), &PhysXRiver3D::get_outlet_discharge);
	ClassDB::bind_method(D_METHOD("get_outlet_head"), &PhysXRiver3D::get_outlet_head);
	ClassDB::bind_method(D_METHOD("set_mouth_target", "path"), &PhysXRiver3D::set_mouth_target);
	ClassDB::bind_method(D_METHOD("get_mouth_target"), &PhysXRiver3D::get_mouth_target);
	ClassDB::bind_method(D_METHOD("set_fall_min_height", "height"), &PhysXRiver3D::set_fall_min_height);
	ClassDB::bind_method(D_METHOD("get_fall_min_height"), &PhysXRiver3D::get_fall_min_height);
	ClassDB::bind_method(D_METHOD("set_fall_exclusions", "points"), &PhysXRiver3D::set_fall_exclusions);
	ClassDB::bind_method(D_METHOD("get_fall_exclusions"), &PhysXRiver3D::get_fall_exclusions);
	ClassDB::bind_method(D_METHOD("set_fall_spray", "enabled"), &PhysXRiver3D::set_fall_spray);
	ClassDB::bind_method(D_METHOD("get_fall_spray"), &PhysXRiver3D::get_fall_spray);
	ClassDB::bind_method(D_METHOD("set_fall_mist", "enabled"), &PhysXRiver3D::set_fall_mist);
	ClassDB::bind_method(D_METHOD("get_fall_mist"), &PhysXRiver3D::get_fall_mist);
	ClassDB::bind_method(D_METHOD("set_fall_sound", "enabled"), &PhysXRiver3D::set_fall_sound);
	ClassDB::bind_method(D_METHOD("get_fall_sound"), &PhysXRiver3D::get_fall_sound);
	ClassDB::bind_method(D_METHOD("set_fall_sound_volume_db", "db"), &PhysXRiver3D::set_fall_sound_volume_db);
	ClassDB::bind_method(D_METHOD("get_fall_sound_volume_db"), &PhysXRiver3D::get_fall_sound_volume_db);
	ClassDB::bind_method(D_METHOD("set_fall_mist_amount", "amount"), &PhysXRiver3D::set_fall_mist_amount);
	ClassDB::bind_method(D_METHOD("get_fall_mist_amount"), &PhysXRiver3D::get_fall_mist_amount);
	ClassDB::bind_method(D_METHOD("set_fall_material", "material"), &PhysXRiver3D::set_fall_material);
	ClassDB::bind_method(D_METHOD("get_fall_material"), &PhysXRiver3D::get_fall_material);
	ClassDB::bind_method(D_METHOD("get_waterfalls"), &PhysXRiver3D::get_waterfalls);
	ClassDB::bind_method(D_METHOD("set_mouth_blend", "blend"), &PhysXRiver3D::set_mouth_blend);
	ClassDB::bind_method(D_METHOD("get_mouth_blend"), &PhysXRiver3D::get_mouth_blend);
	ClassDB::bind_method(D_METHOD("get_discharge_at", "s"), &PhysXRiver3D::get_discharge_at);
	ClassDB::bind_method(D_METHOD("set_raster_reflections", "enabled"), &PhysXRiver3D::set_raster_reflections);
	ClassDB::bind_method(D_METHOD("get_raster_reflections"), &PhysXRiver3D::get_raster_reflections);
	ClassDB::bind_method(D_METHOD("set_live_flow", "enabled"), &PhysXRiver3D::set_live_flow);
	ClassDB::bind_method(D_METHOD("set_contact_wetness", "enabled"), &PhysXRiver3D::set_contact_wetness);
	ClassDB::bind_method(D_METHOD("get_contact_wetness"), &PhysXRiver3D::get_contact_wetness);
	ClassDB::bind_method(D_METHOD("get_live_flow"), &PhysXRiver3D::get_live_flow);
	ClassDB::bind_method(D_METHOD("set_live_domain_size", "size"), &PhysXRiver3D::set_live_domain_size);
	ClassDB::bind_method(D_METHOD("get_live_domain_size"), &PhysXRiver3D::get_live_domain_size);
	ClassDB::bind_method(D_METHOD("set_live_grid_resolution", "resolution"), &PhysXRiver3D::set_live_grid_resolution);
	ClassDB::bind_method(D_METHOD("get_live_grid_resolution"), &PhysXRiver3D::get_live_grid_resolution);
	ClassDB::bind_method(D_METHOD("set_wall_friction", "friction"), &PhysXRiver3D::set_wall_friction);
	ClassDB::bind_method(D_METHOD("get_wall_friction"), &PhysXRiver3D::get_wall_friction);
	ClassDB::bind_method(D_METHOD("set_flow_foam", "strength"), &PhysXRiver3D::set_flow_foam);
	ClassDB::bind_method(D_METHOD("get_flow_foam"), &PhysXRiver3D::get_flow_foam);
	ClassDB::bind_method(D_METHOD("set_flow_foam_persistence", "seconds"), &PhysXRiver3D::set_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_flow_foam_persistence"), &PhysXRiver3D::get_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_flow_foam_impact", "enabled"), &PhysXRiver3D::set_flow_foam_impact);
	ClassDB::bind_method(D_METHOD("get_flow_foam_impact"), &PhysXRiver3D::get_flow_foam_impact);
	ClassDB::bind_method(D_METHOD("set_live_advected_ripples", "enabled"), &PhysXRiver3D::set_live_advected_ripples);
	ClassDB::bind_method(D_METHOD("get_live_advected_ripples"), &PhysXRiver3D::get_live_advected_ripples);
	ClassDB::bind_method(D_METHOD("set_live_damping", "damping"), &PhysXRiver3D::set_live_damping);
	ClassDB::bind_method(D_METHOD("get_live_damping"), &PhysXRiver3D::get_live_damping);
	ClassDB::bind_method(D_METHOD("set_live_follow", "path"), &PhysXRiver3D::set_live_follow);
	ClassDB::bind_method(D_METHOD("get_live_follow"), &PhysXRiver3D::get_live_follow);
	ClassDB::bind_method(D_METHOD("get_live_center"), &PhysXRiver3D::get_live_center);
	ClassDB::bind_method(D_METHOD("is_live_running"), &PhysXRiver3D::is_live_running);
	ClassDB::bind_method(D_METHOD("submit_sphere", "owner", "world_pos", "radius", "strength"), &PhysXRiver3D::submit_sphere, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("clear_sphere", "owner"), &PhysXRiver3D::clear_sphere);
	ClassDB::bind_method(D_METHOD("set_water_material", "material"), &PhysXRiver3D::set_water_material);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXRiver3D::get_water_material);
	ClassDB::bind_method(D_METHOD("set_carve", "carve"), &PhysXRiver3D::set_carve);
	ClassDB::bind_method(D_METHOD("get_carve"), &PhysXRiver3D::get_carve);
	ClassDB::bind_method(D_METHOD("set_carve_depth", "depth"), &PhysXRiver3D::set_carve_depth);
	ClassDB::bind_method(D_METHOD("get_carve_depth"), &PhysXRiver3D::get_carve_depth);
	ClassDB::bind_method(D_METHOD("set_carve_bank_slope", "slope"), &PhysXRiver3D::set_carve_bank_slope);
	ClassDB::bind_method(D_METHOD("get_carve_bank_slope"), &PhysXRiver3D::get_carve_bank_slope);
	ClassDB::bind_method(D_METHOD("set_carve_falloff", "falloff"), &PhysXRiver3D::set_carve_falloff);
	ClassDB::bind_method(D_METHOD("get_carve_falloff"), &PhysXRiver3D::get_carve_falloff);
	ClassDB::bind_method(D_METHOD("set_carve_cell_size", "size"), &PhysXRiver3D::set_carve_cell_size);
	ClassDB::bind_method(D_METHOD("get_carve_cell_size"), &PhysXRiver3D::get_carve_cell_size);
	ClassDB::bind_method(D_METHOD("set_base_height_source", "path"), &PhysXRiver3D::set_base_height_source);
	ClassDB::bind_method(D_METHOD("get_base_height_source"), &PhysXRiver3D::get_base_height_source);
	ClassDB::bind_method(D_METHOD("get_carve_brushes", "tile_size"), &PhysXRiver3D::get_carve_brushes, DEFVAL(64.0f));
	ClassDB::bind_method(D_METHOD("rebuild"), &PhysXRiver3D::rebuild);
	ClassDB::bind_method(D_METHOD("sample_height", "world_pos"), &PhysXRiver3D::sample_height);
	ClassDB::bind_method(D_METHOD("sample_flow", "world_pos"), &PhysXRiver3D::sample_flow);
	ClassDB::bind_method(D_METHOD("sample_depth", "world_pos"), &PhysXRiver3D::sample_depth);
	ClassDB::bind_method(D_METHOD("sample_foam", "world_pos"), &PhysXRiver3D::sample_foam);
	ClassDB::bind_method(D_METHOD("get_length"), &PhysXRiver3D::get_length);
	ClassDB::bind_method(D_METHOD("get_section_count"), &PhysXRiver3D::get_section_count);
	ClassDB::bind_method(D_METHOD("get_section", "index"), &PhysXRiver3D::get_section);

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "widths"), "set_widths", "get_widths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "depths"), "set_depths", "get_depths");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_width", PROPERTY_HINT_RANGE, "0.1,100,0.05,or_greater,suffix:m"), "set_default_width", "get_default_width");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snap_to_ground"), "set_snap_to_ground", "get_snap_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_probe_height", PROPERTY_HINT_RANGE, "0,200,0.05,or_greater,suffix:m"), "set_ground_probe_height", "get_ground_probe_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_probe_depth", PROPERTY_HINT_RANGE, "0.1,200,0.05,or_greater,suffix:m"), "set_ground_probe_depth", "get_ground_probe_depth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "obstacle_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_obstacle_collision_mask", "get_obstacle_collision_mask");
	ADD_GROUP("Hydraulics", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "discharge", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m³/s"), "set_discharge", "get_discharge");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_response"), "set_rain_response", "get_rain_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0.01,0.15,0.001"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope", PROPERTY_HINT_RANGE, "0.005,1,0.005,or_greater"), "set_max_slope", "get_max_slope");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gradually_varied_flow"), "set_gradually_varied_flow", "get_gradually_varied_flow");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bend_superelevation"), "set_bend_superelevation", "get_bend_superelevation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "standing_waves"), "set_standing_waves", "get_standing_waves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foam_persistence", PROPERTY_HINT_RANGE, "0.1,30,0.1,or_greater,suffix:s"), "set_foam_persistence", "get_foam_persistence");
	ADD_GROUP("Outlet", "outlet_");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source_lake", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXWaterSurface3D"), "set_source_lake", "get_source_lake");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outlet_coefficient", PROPERTY_HINT_RANGE, "0.3,1.2,0.01"), "set_outlet_coefficient", "get_outlet_coefficient");
	ADD_GROUP("Mouth", "mouth_");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "mouth_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXWaterSurface3D,PhysXRiver3D"), "set_mouth_target", "get_mouth_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mouth_blend", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:m"), "set_mouth_blend", "get_mouth_blend");
	ADD_GROUP("Waterfalls", "fall_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fall_min_height", PROPERTY_HINT_RANGE, "0,20,0.1,or_greater,suffix:m"), "set_fall_min_height", "get_fall_min_height");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "fall_exclusions"), "set_fall_exclusions", "get_fall_exclusions");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_spray"), "set_fall_spray", "get_fall_spray");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_mist"), "set_fall_mist", "get_fall_mist");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fall_mist_amount", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater"), "set_fall_mist_amount", "get_fall_mist_amount");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_sound"), "set_fall_sound", "get_fall_sound");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fall_sound_volume_db", PROPERTY_HINT_RANGE, "-40,20,0.1,suffix:dB"), "set_fall_sound_volume_db", "get_fall_sound_volume_db");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fall_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_fall_material", "get_fall_material");
	ADD_SIGNAL(MethodInfo("rebuilt"));
	ADD_GROUP("Live flow", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_flow"), "set_live_flow", "get_live_flow");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "contact_wetness"), "set_contact_wetness", "get_contact_wetness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "raster_reflections"), "set_raster_reflections", "get_raster_reflections");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_domain_size", PROPERTY_HINT_RANGE, "8,256,1,or_greater,suffix:m"), "set_live_domain_size", "get_live_domain_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "live_grid_resolution", PROPERTY_HINT_RANGE, "32,512,1"), "set_live_grid_resolution", "get_live_grid_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_friction", PROPERTY_HINT_RANGE, "0,10,0.05,suffix:1/s"), "set_wall_friction", "get_wall_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam", PROPERTY_HINT_RANGE, "0,4,0.05"), "set_flow_foam", "get_flow_foam");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam_persistence", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_flow_foam_persistence", "get_flow_foam_persistence");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flow_foam_impact"), "set_flow_foam_impact", "get_flow_foam_impact");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_advected_ripples"), "set_live_advected_ripples", "get_live_advected_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_damping", PROPERTY_HINT_RANGE, "0,1,0.005"), "set_live_damping", "get_live_damping");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "live_follow", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_live_follow", "get_live_follow");
	ADD_GROUP("Carve", "carve_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "carve"), "set_carve", "get_carve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_depth", PROPERTY_HINT_RANGE, "0,20,0.05,or_greater,suffix:m"), "set_carve_depth", "get_carve_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_bank_slope", PROPERTY_HINT_RANGE, "0.05,5,0.05,or_greater"), "set_carve_bank_slope", "get_carve_bank_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_falloff", PROPERTY_HINT_RANGE, "0,20,0.05,or_greater,suffix:m"), "set_carve_falloff", "get_carve_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_cell_size", PROPERTY_HINT_RANGE, "0.05,4,0.05,or_greater,suffix:m"), "set_carve_cell_size", "get_carve_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "base_height_source"), "set_base_height_source", "get_base_height_source");

	ADD_SIGNAL(MethodInfo("carve_changed"));

	ADD_GROUP("Mesh", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_length", PROPERTY_HINT_RANGE, "0.1,10,0.05,or_greater,suffix:m"), "set_step_length", "get_step_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cross_segments", PROPERTY_HINT_RANGE, "2,64,1"), "set_cross_segments", "get_cross_segments");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_margin", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater,suffix:m"), "set_bank_margin", "get_bank_margin");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "water_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_water_material", "get_water_material");
}

#define RIVER_SETTER(m_name, m_type, m_member, m_expr) \
	void PhysXRiver3D::set_##m_name(m_type p_value) { \
		m_member = (m_expr); \
		_queue_build(); \
	}

RIVER_SETTER(widths, const PackedFloat32Array &, widths, p_value)
RIVER_SETTER(depths, const PackedFloat32Array &, depths, p_value)
RIVER_SETTER(default_width, float, default_width, MAX(p_value, 0.05f))
RIVER_SETTER(discharge, float, discharge, MAX(p_value, 0.001f))
RIVER_SETTER(roughness, float, roughness, CLAMP(p_value, 0.005f, 0.5f))
RIVER_SETTER(max_slope, float, max_slope, MAX(p_value, 0.001f))
RIVER_SETTER(gradually_varied_flow, bool, gradually_varied_flow, p_value)
RIVER_SETTER(ground_probe_height, float, ground_probe_height, MAX(p_value, 0.0f))
RIVER_SETTER(ground_probe_depth, float, ground_probe_depth, MAX(p_value, 0.1f))
RIVER_SETTER(bend_superelevation, bool, bend_superelevation, p_value)
RIVER_SETTER(standing_waves, bool, standing_waves, p_value)
RIVER_SETTER(snap_to_ground, bool, snap_to_ground, p_value)
RIVER_SETTER(ground_collision_mask, uint32_t, ground_collision_mask, p_value)
RIVER_SETTER(obstacle_collision_mask, uint32_t, obstacle_collision_mask, p_value)
RIVER_SETTER(step_length, float, step_length, MAX(p_value, 0.05f))
RIVER_SETTER(cross_segments, int, cross_segments, CLAMP(p_value, 2, 256))
RIVER_SETTER(bank_margin, float, bank_margin, MAX(p_value, 0.0f))
RIVER_SETTER(foam_persistence, float, foam_persistence, MAX(p_value, 0.05f))
RIVER_SETTER(carve, bool, carve, p_value)
RIVER_SETTER(carve_depth, float, carve_depth, MAX(p_value, 0.0f))
RIVER_SETTER(carve_bank_slope, float, carve_bank_slope, MAX(p_value, 0.01f))
RIVER_SETTER(carve_falloff, float, carve_falloff, MAX(p_value, 0.0f))
RIVER_SETTER(carve_cell_size, float, carve_cell_size, MAX(p_value, 0.05f))
RIVER_SETTER(base_height_source, const NodePath &, base_height_source, p_value)

#undef RIVER_SETTER

Node *PhysXRiver3D::_mouth_node() const {
	if (mouth_target.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	Node *n = get_node_or_null(mouth_target);
	return n == this ? nullptr : n;
}

void PhysXRiver3D::set_mouth_target(const NodePath &p_path) {
	_unregister_mouth();
	mouth_target = p_path;
	_queue_build();
}

void PhysXRiver3D::set_mouth_blend(float p_blend) {
	mouth_blend = MAX(p_blend, 0.0f);
	_queue_build();
}

Node *PhysXRiver3D::_source_node() const {
	if (source_lake.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	Node *n = get_node_or_null(source_lake);
	return n == this ? nullptr : n;
}

void PhysXRiver3D::set_source_lake(const NodePath &p_path) {
	_unregister_outlet();
	source_lake = p_path;
	_queue_build();
}

void PhysXRiver3D::set_outlet_coefficient(float p_coefficient) {
	outlet_coefficient = CLAMP(p_coefficient, 0.05f, 2.0f);
	_queue_build();
}

// The lake's head over the outlet's sill gives the river's discharge (a broad-crested weir, Henderson 1966:
// Q = Cd (2/3)^(3/2) b sqrt(g) H^(3/2)). The sill is the highest bed in the river's first 12 m.
void PhysXRiver3D::_measure_outlet(PhysicsDirectSpaceState3D *p_space, const Ref<Curve3D> &p_curve, float p_ds) {
	PhysXWaterSurface3D *lake = Object::cast_to<PhysXWaterSurface3D>(_source_node());
	if (lake == nullptr) {
		_unregister_outlet();
		outlet_discharge = -1.0f;
		outlet_head = 0.0f;
		return;
	}
	const float length = p_curve->get_baked_length();
	const Transform3D xf = get_global_transform();
	float crest = -Math::INF;
	float crest_s = 0.0f;
	for (float s = 0.0f; s <= MIN(12.0f, length) + 1e-4f; s += MAX(p_ds, 0.25f)) {
		const Vector3 c = p_curve->sample_baked(s, true);
		float y = 0.0f;
		if (_base_ground_at(p_space, c, y)) {
			const float wy = xf.xform(Vector3(c.x, y, c.z)).y;
			if (wy > crest) {
				crest = wy;
				crest_s = s;
			}
		}
	}
	const Vector3 at_local = p_curve->sample_baked(crest_s, true);
	if (!Math::is_finite(crest)) {
		crest = xf.xform(at_local).y; // no ground found: the curve's own height is the sill
	}
	outlet_level = lake->get_global_position().y + lake->get_water_level();
	outlet_width = widths.is_empty() ? default_width : _point_value(widths, 0, default_width);
	outlet_crest = crest;
	outlet_crest_s = crest_s;
	outlet_head = MAX(outlet_level - crest, 0.0f);
	outlet_free = PhysXWaterSurface3D::weir_discharge(outlet_coefficient, outlet_width, outlet_head);
	outlet_discharge = outlet_free; // drowned by the channel below, if it is too slow to carry it (in _build)
	Vector3 ahead = p_curve->sample_baked(MIN(crest_s + 1.0f, length), true) - p_curve->sample_baked(MAX(crest_s - 1.0f, 0.0f), true);
	ahead.y = 0.0f;
	outlet_dir = xf.basis.xform(ahead.length_squared() > 1e-8f ? ahead.normalized() : Vector3(1, 0, 0));
	outlet_at = xf.xform(at_local);
	if (source_id.is_valid() && source_id != lake->get_instance_id()) {
		_unregister_outlet();
	}
}

// Registered after the build, with the discharge the channel let through: the lake drains by the same law (its
// coefficient scaled by the share the channel lets through), so its level budget and its approach flow agree with
// the river.
void PhysXRiver3D::_register_outlet() {
	PhysXWaterSurface3D *lake = Object::cast_to<PhysXWaterSurface3D>(_source_node());
	if (lake == nullptr || outlet_discharge < 0.0f) {
		_unregister_outlet();
		return;
	}
	const float share = outlet_free > 1e-6f ? outlet_discharge / outlet_free : 1.0f;
	source_id = lake->get_instance_id();
	lake->set_outflow((int64_t)(uint64_t)get_instance_id(), outlet_at, outlet_dir, outlet_width, outlet_crest, outlet_coefficient * share);
}

void PhysXRiver3D::_unregister_outlet() {
	if (!source_id.is_valid()) {
		return;
	}
	if (PhysXWaterSurface3D *lake = Object::cast_to<PhysXWaterSurface3D>(ObjectDB::get_instance(source_id))) {
		lake->clear_outflow((int64_t)(uint64_t)get_instance_id());
	}
	source_id = ObjectID();
}

float PhysXRiver3D::get_discharge_at(float p_s) const {
	// A lake outlet runs on what the lake passes over its sill (a trickle with no head, so the ribbon stays).
	float q = (outlet_discharge >= 0.0f ? MAX(outlet_discharge, 0.01f) : discharge) + rain_discharge;
	for (const Tributary &t : tributaries) {
		if (p_s >= t.s) {
			q += t.discharge;
		}
	}
	return q;
}

void PhysXRiver3D::set_tributary(ObjectID p_river, const Vector3 &p_world, float p_discharge) {
	for (Tributary &e : tributaries) {
		if (e.river == p_river) {
			if (e.at.distance_to(p_world) < 0.5f && Math::abs(e.discharge - p_discharge) < 1e-3f) {
				return; // unchanged: no rebuild (the two rivers would keep waking each other)
			}
			e.at = p_world;
			e.discharge = p_discharge;
			_queue_build();
			return;
		}
	}
	Tributary trib;
	trib.river = p_river;
	trib.at = p_world;
	trib.discharge = p_discharge;
	tributaries.push_back(trib);
	_queue_build();
}

void PhysXRiver3D::clear_tributary(ObjectID p_river) {
	for (uint32_t k = 0; k < tributaries.size(); k++) {
		if (tributaries[k].river == p_river) {
			tributaries.remove_at(k);
			_queue_build();
			return;
		}
	}
}

// Tell the water this river flows into: a river adds the discharge downstream
// of the join; a lake or the sea gets the outflow as a plume (a turbulent
// jet spreading from the mouth).
void PhysXRiver3D::_register_mouth() {
	Node *target = _mouth_node();
	if (target == nullptr || sections.size() < 2) {
		_unregister_mouth();
		return;
	}
	if (mouth_id.is_valid() && mouth_id != target->get_instance_id()) {
		_unregister_mouth();
	}
	const Transform3D xf = get_global_transform();
	int mouth = -1;
	for (uint32_t i = 1; i < sections.size(); i++) {
		if (sections[i].mouth_fade < 1.0f) {
			mouth = (int)i - 1;
			break;
		}
	}
	if (mouth < 0) {
		mouth = (int)sections.size() - 1;
	}
	const Section &sec = sections[mouth];
	const Vector3 at = xf.xform(sec.centre);
	const Vector3 dir = xf.basis.xform(Vector3(-sec.side.z, 0.0f, sec.side.x)).normalized();
	if (PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(target)) {
		if (!main->is_connected(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt))) {
			main->connect(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt));
		}
		main->set_tributary(get_instance_id(), at, sec.discharge);
	} else if (PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(target)) {
		sea->set_inflow((int64_t)(uint64_t)get_instance_id(), at, dir, sec.width, sec.discharge);
	}
	mouth_id = target->get_instance_id();
}

void PhysXRiver3D::_unregister_mouth() {
	if (!mouth_id.is_valid()) {
		return;
	}
	Object *o = ObjectDB::get_instance(mouth_id);
	if (PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(o)) {
		if (main->is_connected(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt))) {
			main->disconnect(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt));
		}
		main->clear_tributary(get_instance_id());
	} else if (PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(o)) {
		sea->clear_inflow((int64_t)(uint64_t)get_instance_id());
	}
	mouth_id = ObjectID();
}

// The river this one flows into was rebuilt: follow its surface at the join
// if it moved (more than a centimetre, so the two settle instead of waking
// each other for ever).
void PhysXRiver3D::_on_target_rebuilt() {
	PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(_mouth_node());
	if (main == nullptr || sections.size() < 2) {
		return;
	}
	const Transform3D xf = get_global_transform();
	for (uint32_t i = 0; i < sections.size(); i++) {
		const Vector3 w = xf.xform(sections[i].centre);
		if (main->sample_depth(w) > 0.0f) {
			const float level = main->sample_height(w);
			if (!Math::is_finite(mouth_level_seen) || Math::abs(level - mouth_level_seen) > 0.01f) {
				_queue_build();
			}
			return;
		}
	}
}

void PhysXRiver3D::set_live_flow(bool p_enabled) {
	live_flow = p_enabled;
	if (!live_flow) {
		_live_stop();
	}
}

void PhysXRiver3D::set_live_domain_size(float p_size) {
	live_domain_size = MAX(p_size, 4.0f);
	_live_stop();
}

void PhysXRiver3D::set_live_grid_resolution(int p_n) {
	live_grid_resolution = CLAMP(p_n, 16, 1024);
	_live_stop();
}

void PhysXRiver3D::set_live_damping(float p_damping) {
	live_damping = MAX(p_damping, 0.0f);
	_live_stop();
}

void PhysXRiver3D::set_wall_friction(float p_friction) {
	wall_friction = MAX(p_friction, 0.0f);
	live_fields_dirty = true;
}

void PhysXRiver3D::set_flow_foam(float p_strength) {
	flow_foam = MAX(p_strength, 0.0f);
	if (live_configured) {
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence, flow_foam_impact);
	}
}

void PhysXRiver3D::set_flow_foam_persistence(float p_seconds) {
	flow_foam_persistence = MAX(p_seconds, 0.05f);
	if (live_configured) {
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence, flow_foam_impact);
	}
}

void PhysXRiver3D::set_flow_foam_impact(bool p_enabled) {
	flow_foam_impact = p_enabled;
	if (live_configured) {
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence, flow_foam_impact);
	}
}

void PhysXRiver3D::set_live_advected_ripples(bool p_enabled) {
	live_advected_ripples = p_enabled;
	if (live_configured) {
		live_solver.set_advected_ripples(live_advected_ripples, RIVER_FLOW_PERIOD);
	}
	_live_material();
}

void PhysXRiver3D::set_live_follow(const NodePath &p_path) {
	live_follow = p_path;
}

void PhysXRiver3D::submit_sphere(int p_owner, const Vector3 &p_world_pos, float p_radius, float p_strength) {
	if (live_configured) {
		live_solver.submit_sphere((uint64_t)p_owner, p_world_pos, p_radius, p_strength);
	}
}

void PhysXRiver3D::clear_sphere(int p_owner) {
	if (live_configured) {
		live_solver.clear_sphere((uint64_t)p_owner);
	}
}

// Strip coordinates of a world point: section, fraction to the next, distance
// across (local m, + towards the left bank). Unlike _locate(), never refuses a
// point past the banks or the ends (it clamps along the river); returns
// whether the point is on the river.
bool PhysXRiver3D::_strip_at(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const {
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (count < 2 || c.is_null() || !is_inside_tree()) {
		return false;
	}
	if (!p_world.is_finite()) {
		// A body that has left the world (it would index the cross-section with NaN).
		r_section = 0;
		r_t = 0.0f;
		r_n = 0.0f;
		return false;
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	const float ds = sections[1].s - sections[0].s;
	const float s = c->get_closest_offset(local);
	const float x = s / ds;
	const int i = CLAMP((int)Math::floor(x), 0, count - 2);
	const float t = CLAMP(x - i, 0.0f, 1.0f);
	const Section &a = sections[i];
	const Section &b = sections[i + 1];
	const Vector3 centre = a.centre.lerp(b.centre, t);
	const Vector3 side = a.side.lerp(b.side, t).normalized();
	const Vector3 rel = Vector3(local.x - centre.x, 0.0f, local.z - centre.z);
	const Vector3 tangent(-side.z, 0.0f, side.x);
	const float along = rel.dot(tangent);
	r_section = i;
	r_t = t;
	r_n = rel.dot(side);
	const float half = 0.5f * Math::lerp(a.width, b.width, t) + bank_margin;
	const bool past_end = (i == 0 && t <= 0.0f && along < -0.5f * ds) || (i == count - 2 && t >= 1.0f && along > 0.5f * ds);
	return !past_end && Math::abs(r_n) <= half;
}

// The river's stream function at a world point, for the live window's face
// fluxes (the bank's value past the banks), and the water depth there
// (WaterSolver::WALL_DEPTH off the river).
float PhysXRiver3D::_psi_at(const Vector3 &p_world, float &r_depth) const {
	int i;
	float t;
	float n;
	r_depth = WaterSolver::WALL_DEPTH;
	if (sections.size() < 2) {
		return 0.0f;
	}
	const bool on = _strip_at(p_world, i, t, n);
	const int m = cross_segments;
	auto value = [&](const Section &p_sec, const LocalVector<float> &p_values) -> float {
		return cross_value(p_sec, p_values, n, bank_margin, m);
	};
	if (sections[i].psi_at.size() != (uint32_t)(m + 1) || sections[i + 1].psi_at.size() != (uint32_t)(m + 1)) {
		return 0.0f;
	}
	if (on) {
		const float d = Math::lerp(value(sections[i], sections[i].depth), value(sections[i + 1], sections[i + 1].depth), t);
		r_depth = d; // (signed: ground above the water is negative, so the live window's water runs up banks only as far as they are low)
	}
	return Math::lerp(value(sections[i], sections[i].psi_at), value(sections[i + 1], sections[i + 1].psi_at), t);
}

// The live window's still-water depth per cell and baked face fluxes (m^2/s:
// +x, +z, -x, -z per cell), from the river's stream function at the cell
// corners, as PhysXWaterSurface3D does from its own bake. The ocean's
// convention is u = (dpsi/dz, -dpsi/dx) / h; the river's psi grows towards the
// left bank, which is the opposite turn, so it goes in negated.
void PhysXRiver3D::_live_fields(PackedFloat32Array &r_depth, PackedFloat32Array &r_flux) const {
	const int n = live_grid_resolution;
	const float cell = live_domain_size / n;
	const float y = get_global_position().y;
	r_depth.resize(n * n);
	LocalVector<float> psi;
	psi.resize(n * n);
	float *dw = r_depth.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector3 w(live_center.x + (x + 0.5f - 0.5f * n) * cell, y, live_center.y + (z + 0.5f - 0.5f * n) * cell);
			float d = 0.0f;
			psi[z * n + x] = -_psi_at(w, d);
			dw[z * n + x] = d;
		}
	}
	LocalVector<float> corner;
	corner.resize((n + 1) * (n + 1));
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			float sum = 0.0f;
			float dry_psi = 0.0f;
			bool dry = false;
			for (int k = 0; k < 4; k++) {
				const int cx = CLAMP(i - 1 + (k & 1), 0, n - 1);
				const int cz = CLAMP(j - 1 + (k >> 1), 0, n - 1);
				const int c = cz * n + cx;
				if (dw[c] <= 0.0f && !dry) {
					dry = true;
					dry_psi = psi[c];
				}
				sum += psi[c];
			}
			corner[j * (n + 1) + i] = dry ? dry_psi : 0.25f * sum;
		}
	}
	auto at = [&](int i, int j) { return corner[j * (n + 1) + i]; };
	r_flux.resize(n * n * 4);
	float *q = r_flux.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			q[i * 4 + 0] = (at(x + 1, z + 1) - at(x + 1, z)) / cell;
			q[i * 4 + 1] = -(at(x + 1, z + 1) - at(x, z + 1)) / cell;
			q[i * 4 + 2] = (at(x, z + 1) - at(x, z)) / cell;
			q[i * 4 + 3] = -(at(x + 1, z) - at(x, z)) / cell;
		}
	}
	for (int k = 0; k < (int)r_flux.size(); k++) {
		if (!Math::is_finite(q[k])) {
			q[k] = 0.0f;
		}
	}
}

void PhysXRiver3D::_live_stop() {
	if (live_height_tex.is_valid()) {
		live_height_tex->set_texture_rd_rid(RID());
	}
	if (live_flow_tex.is_valid()) {
		live_flow_tex->set_texture_rd_rid(RID());
	}
	if (live_offset_tex.is_valid()) {
		live_offset_tex->set_texture_rd_rid(RID());
	}
	live_bound = false;
	live_configured = false;
	live_height_cache.clear();
	live_velocity_cache.clear();
	live_tilt = Vector3();
	_live_material();
}

void PhysXRiver3D::_live_material() {
	if (water_material.is_null()) {
		return;
	}
	const bool on = live_bound && live_configured;
	water_material->set_shader_parameter(SNAME("live_enabled"), on);
	if (on) {
		water_material->set_shader_parameter(SNAME("live_center"), live_center);
		water_material->set_shader_parameter(SNAME("live_size"), Vector2(live_domain_size, live_domain_size));
		water_material->set_shader_parameter(SNAME("live_texel"), Vector2(live_domain_size, live_domain_size) / live_grid_resolution);
		water_material->set_shader_parameter(SNAME("live_height_tex"), live_height_tex);
		water_material->set_shader_parameter(SNAME("live_flow_tex"), live_flow_tex);
		water_material->set_shader_parameter(SNAME("live_tilt"), live_tilt);
		water_material->set_shader_parameter(SNAME("live_offset_tex"), live_offset_tex);
	}
	water_material->set_shader_parameter(SNAME("live_advected"), on && live_advected_ripples && live_offset_tex.is_valid());
}

// The live window: the shallow-water solver of PhysXWaterSurface3D's live flow
// (Chentanez and Mueller 2010, water_ripple.glsl) on a square round the camera,
// fed the river's depth and baked fluxes. The river's surface slopes, but the
// solver only carries the disturbance on it (gravity and bed friction balance in
// the baked flow), so its heights are added to the river's own. Moving more
// than a quarter of the square re-centres it and restarts it from the baked flow.
void PhysXRiver3D::_live_update(double p_delta) {
	if (!live_flow || Engine::get_singleton()->is_editor_hint() || sections.size() < 2 || build_pending) {
		return;
	}
	Vector3 follow;
	Node3D *target = live_follow.is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(live_follow));
	if (target != nullptr) {
		follow = target->get_global_position();
	} else if (get_viewport() != nullptr && get_viewport()->get_camera_3d() != nullptr) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		// The water the camera looks at: where its view meets the river's height.
		const Vector3 o = cam->get_global_position();
		const Vector3 dir = -cam->get_global_transform().basis.get_column(2);
		const float y = get_global_position().y;
		const float t = dir.y < -0.05f ? (y - o.y) / dir.y : 0.0f;
		follow = o + dir * CLAMP(t, 0.0f, 0.4f * live_domain_size + Math::abs(o.y - y));
	} else {
		return;
	}
	const float cell = live_domain_size / live_grid_resolution;
	const Vector2 want(Math::snapped(follow.x, cell), Math::snapped(follow.z, cell));
	if (!live_configured) {
		live_center = want;
		PackedFloat32Array depth;
		PackedFloat32Array flux;
		_live_fields(depth, flux);
		float mean = 0.0f;
		int wet = 0;
		for (int k = 0; k < depth.size(); k++) {
			if (depth[k] > 0.0f) {
				mean += depth[k];
				wet++;
			}
		}
		WaterSolver::Settings st;
		st.grid_resolution = live_grid_resolution;
		st.domain_size = Vector2(live_domain_size, live_domain_size);
		st.grid_center = live_center;
		st.cell_depth = depth;
		st.depth = wet > 0 ? mean / wet : 1.0f;
		st.damping = live_damping;
		st.gravity = GRAVITY;
		st.water_level = 0.0f; // the solver's heights are the disturbance on the river's surface
		st.ocean_grid_resolution = 16; // no waves on a river: the smallest ocean layer, amplitude 0
		st.ocean_domain_size = Vector2(live_domain_size, live_domain_size);
		st.wave_amplitude = 0.0f;
		st.caustics_enabled = false;
		live_depth = depth;
		live_tilt = Vector3();
		live_solver.configure(st);
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence, flow_foam_impact);
		live_solver.set_advected_ripples(live_advected_ripples, RIVER_FLOW_PERIOD);
		live_solver.set_base_flux(flux, wall_friction);
		live_configured = true;
		live_fields_dirty = false;
		live_bound = false;
	} else if (MAX(Math::abs(want.x - live_center.x), Math::abs(want.y - live_center.y)) > 0.25f * live_domain_size || live_fields_dirty) {
		live_center = want;
		live_solver.recenter(live_center);
		contact_wet_skip = 2;
		PackedFloat32Array depth;
		PackedFloat32Array flux;
		_live_fields(depth, flux);
		live_solver.update_seabed(depth);
		live_depth = depth;
		live_tilt = Vector3();
		live_solver.set_base_flux(flux, wall_friction);
		live_fields_dirty = false;
		live_height_cache.clear();
		live_velocity_cache.clear();
		_live_material();
	}
	live_solver.step(p_delta);
	if (!live_bound && live_solver.is_available()) {
		if (live_height_tex.is_null()) {
			live_height_tex.instantiate();
		}
		if (live_flow_tex.is_null()) {
			live_flow_tex.instantiate();
		}
		live_height_tex->set_texture_rd_rid(live_solver.get_ripple_height_texture_rd_rid());
		live_flow_tex->set_texture_rd_rid(live_solver.get_velocity_texture_rd_rid());
		if (live_offset_tex.is_null()) {
			live_offset_tex.instantiate();
		}
		live_offset_tex->set_texture_rd_rid(live_solver.get_flow_offset_texture_rd_rid());
		live_bound = true;
		_live_material();
	}
	if (++live_refresh >= 2) {
		live_refresh = 0;
		int n = 0;
		Vector2 domain;
		live_solver.get_height_grid(live_height_cache, n, domain);
		live_solver.get_velocity_grid(live_velocity_cache);
		_live_fit_tilt();
	}
	_live_wet_ground();
}

void PhysXRiver3D::set_contact_wetness(bool p_enabled) {
	contact_wetness = p_enabled;
	if (!p_enabled) {
		live_solver.set_wetness_readback(false);
	}
}

// Where the live water reached (the swash pass's wetness on the window, read back every few steps), over the
// banks and water shallower than half a metre, into the weather's contact wetness.
void PhysXRiver3D::_live_wet_ground() {
#ifdef MODULE_WEATHER_ENABLED
	WeatherMap3D *weather = WeatherMap3D::get_active();
	const bool want = contact_wetness && weather != nullptr && weather->is_contact_wetness_enabled();
	live_solver.set_wetness_readback(want);
	if (!want) {
		return;
	}
	Vector<float> wet;
	int n = 0;
	uint64_t serial = 0;
	if (!live_solver.get_wetness_grid(wet, n, serial) || serial == contact_wet_serial || live_depth.size() != n * n) {
		return;
	}
	contact_wet_serial = serial;
	if (contact_wet_skip > 0) {
		contact_wet_skip--;
		return;
	}
	contact_wet_cells.resize(n * n);
	float *cw = contact_wet_cells.ptrw();
	const float *w = wet.ptr();
	const float *d = live_depth.ptr();
	bool any = false;
	for (int i = 0; i < n * n; i++) {
		// Faded over the grid's outer eighth, so the wet band does not end in a straight line at its edge.
		const int x = i % n;
		const int z = i / n;
		const float edge = MIN(MIN(x, n - 1 - x), MIN(z, n - 1 - z)) / MAX(0.125f * n, 1.0f);
		cw[i] = d[i] <= 0.5f ? CLAMP(w[i], 0.0f, 1.0f) * MIN(edge, 1.0f) : 0.0f;
		any = any || cw[i] > 0.0f;
	}
	if (any) {
		const Vector2 size(live_domain_size, live_domain_size);
		weather->add_contact_wetness_cells(cw, n, n, Rect2(live_center - 0.5f * size, size));
	}
#endif
}

// The live water settles into a gentle tilt across the window: the velocity
// head (v^2 / 2g) of a river speeding up or slowing down, which the baked
// surface (Manning's normal depth) leaves out. Kept, it would step the surface
// at the window's edge and move with the window; so the window's mean plane
// over its wet cells is taken out and only local disturbances (eddies, wakes,
// waves round rocks) stay. A least-squares plane, refitted at each readback.
void PhysXRiver3D::_live_fit_tilt() {
	const int n = live_grid_resolution;
	if (live_height_cache.size() != n * n || live_depth.size() != n * n) {
		return;
	}
	const float cell = live_domain_size / n;
	double s1 = 0, sx = 0, sz = 0, sxx = 0, szz = 0, sxz = 0, sh = 0, sxh = 0, szh = 0;
	const float *h = live_height_cache.ptr();
	const float *d = live_depth.ptr();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (!(d[i] > 0.05f) || !Math::is_finite(h[i])) {
				continue;
			}
			const double px = (x + 0.5 - 0.5 * n) * cell;
			const double pz = (z + 0.5 - 0.5 * n) * cell;
			s1 += 1;
			sx += px;
			sz += pz;
			sxx += px * px;
			szz += pz * pz;
			sxz += px * pz;
			sh += h[i];
			sxh += px * h[i];
			szh += pz * h[i];
		}
	}
	if (s1 < 16) {
		return;
	}
	// Normal equations for h = a + b x + c z (3 x 3, Cramer).
	const double m[3][3] = { { s1, sx, sz }, { sx, sxx, sxz }, { sz, sxz, szz } };
	const double r[3] = { sh, sxh, szh };
	auto det3 = [](const double p[3][3]) {
		return p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) + p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
	};
	const double det = det3(m);
	if (Math::abs(det) < 1e-9) {
		return;
	}
	double out[3];
	for (int k = 0; k < 3; k++) {
		double mk[3][3];
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) {
				mk[a][b] = b == k ? r[a] : m[a][b];
			}
		}
		out[k] = det3(mk) / det;
	}
	live_tilt = Vector3((float)out[0], (float)out[1], (float)out[2]);
	if (water_material.is_valid() && live_bound) {
		water_material->set_shader_parameter(SNAME("live_tilt"), live_tilt);
	}
}

// The live window's disturbance height at a world point, 0 outside it (eased
// over its edge, as the shader).
float PhysXRiver3D::_live_height_at(const Vector3 &p_world) const {
	const int n = live_grid_resolution;
	if (!live_configured || live_height_cache.size() != n * n) {
		return 0.0f;
	}
	const Vector2 rel = (Vector2(p_world.x, p_world.z) - live_center) / live_domain_size;
	const float inside = 1.0f - Math::smoothstep(0.44f, 0.5f, MAX(Math::abs(rel.x), Math::abs(rel.y)));
	if (inside <= 0.0f) {
		return 0.0f;
	}
	const float gx = CLAMP((rel.x + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const float gz = CLAMP((rel.y + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const int x0 = MIN((int)gx, n - 2);
	const int z0 = MIN((int)gz, n - 2);
	const float fx = gx - x0;
	const float fz = gz - z0;
	const float *h = live_height_cache.ptr();
	const float a = Math::lerp(h[z0 * n + x0], h[z0 * n + x0 + 1], fx);
	const float b = Math::lerp(h[(z0 + 1) * n + x0], h[(z0 + 1) * n + x0 + 1], fx);
	const Vector2 off = Vector2(p_world.x, p_world.z) - live_center;
	return (Math::lerp(a, b, fz) - (live_tilt.x + live_tilt.y * off.x + live_tilt.z * off.y)) * inside;
}

// The live part of the flow (the solver's read-back face velocities, averaged to
// cell centres), world xz m/s, 0 outside the window.
Vector2 PhysXRiver3D::_live_velocity_at(const Vector3 &p_world) const {
	const int n = live_grid_resolution;
	if (!live_configured || live_velocity_cache.size() != n * n * 2) {
		return Vector2();
	}
	const Vector2 rel = (Vector2(p_world.x, p_world.z) - live_center) / live_domain_size;
	const float inside = 1.0f - Math::smoothstep(0.44f, 0.5f, MAX(Math::abs(rel.x), Math::abs(rel.y)));
	if (inside <= 0.0f) {
		return Vector2();
	}
	const float *v = live_velocity_cache.ptr();
	auto centre = [&](int x, int z) {
		const int i = z * n + x;
		const float u = 0.5f * (v[i * 2] + (x > 0 ? v[(i - 1) * 2] : 0.0f));
		const float w = 0.5f * (v[i * 2 + 1] + (z > 0 ? v[(i - n) * 2 + 1] : 0.0f));
		return Vector2(u, w);
	};
	const float gx = CLAMP((rel.x + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const float gz = CLAMP((rel.y + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const int x0 = MIN((int)gx, n - 2);
	const int z0 = MIN((int)gz, n - 2);
	const float fx = gx - x0;
	const float fz = gz - z0;
	return centre(x0, z0).lerp(centre(x0 + 1, z0), fx).lerp(centre(x0, z0 + 1).lerp(centre(x0 + 1, z0 + 1), fx), fz) * inside;
}

void PhysXRiver3D::set_water_material(const Ref<ShaderMaterial> &p_material) {
	water_material = p_material;
	if (mesh_instance != nullptr) {
		mesh_instance->set_material_override(water_material);
	}
	_live_material();
}

void PhysXRiver3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!is_connected(SNAME("curve_changed"), callable_mp(this, &PhysXRiver3D::_on_curve_changed))) {
				connect(SNAME("curve_changed"), callable_mp(this, &PhysXRiver3D::_on_curve_changed));
			}
			_queue_build();
			set_physics_process_internal(true);
			set_process_internal(!Engine::get_singleton()->is_editor_hint());
			WorldContracts::register_water(this);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			WorldContracts::unregister_water(this);
			set_physics_process_internal(false);
			set_process_internal(false);
			_live_stop();
			_unregister_mouth();
			_unregister_outlet();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			// Ground rays need the ground in the physics space, which it is by the first tick.
			if (build_pending) {
				_build();
			}
			_live_update(get_physics_process_delta_time());
			if (!Engine::get_singleton()->is_editor_hint()) {
				body_contacts.update(
						get_tree(), get_physics_process_delta_time(),
						[this](const Vector3 &p) { return sample_height(p); },
						[this](const Vector3 &p) { return sample_flow(p); },
						[this](int o, const Vector3 &p, float r, float s) { submit_sphere(o, p, r, s); },
						[this](int o) { clear_sphere(o); },
						[this](const Vector3 &p, float r, float s) { submit_sphere(int(uint64_t(get_instance_id()) ^ 0x5a5a), p, r, s); });
			}
			_rain_response_update(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (water_material.is_valid()) {
				water_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
				if (live_configured) {
					live_solver.set_flow_clock((float)Math::fmod(water_time, 3600.0)); // (the clock the ripples' phases run on)
				}
				raster_reflection_state.update(this, water_material, raster_reflections);
			}
			if (fall_material.is_valid() && !falls.is_empty()) {
				fall_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
			}
		} break;
	}
}

// Storm flow: every 10 s, the runoff's peak discharge within two of its cells of the river's upstream end. The
// river is rebuilt when that moves by more than a tenth of its flow (depth and speed follow by Manning, so a
// river in spate is deeper and faster), at most every 10 s.
void PhysXRiver3D::_rain_response_update(double p_delta) {
	rain_check -= p_delta;
	if (rain_check > 0.0 || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	rain_check = 10.0;
	float q = 0.0f;
#ifdef MODULE_WEATHER_ENABLED
	const WeatherRunoff3D *ro = WeatherRunoff3D::get_active();
	const Ref<Curve3D> c = get_curve();
	if (rain_response && ro != nullptr && c.is_valid() && c->get_point_count() > 0) {
		const Vector3 at = get_global_transform().xform(c->get_point_position(0));
		const float reach = 2.0f * ro->get_extent() / float(MAX(ro->get_resolution(), 1));
		q = ro->get_max_discharge_in(Rect2(at.x - reach, at.z - reach, 2.0f * reach, 2.0f * reach));
	}
#endif
	if (Math::abs(q - rain_discharge) > 0.1f * MAX(discharge + rain_discharge, 0.05f) || (q == 0.0f && rain_discharge > 0.0f)) {
		rain_discharge = q;
		_queue_build();
	}
}

void PhysXRiver3D::set_rain_response(bool p_enable) {
	rain_response = p_enable;
	if (!p_enable && rain_discharge > 0.0f) {
		rain_discharge = 0.0f;
		_queue_build();
	}
}

void PhysXRiver3D::_on_curve_changed() {
	_queue_build();
}

void PhysXRiver3D::_queue_build() {
	build_pending = true;
}

void PhysXRiver3D::rebuild() {
	_build();
}

float PhysXRiver3D::_point_value(const PackedFloat32Array &p_values, int p_index, float p_default) const {
	if (p_values.is_empty()) {
		return p_default;
	}
	const float v = p_values[MIN(p_index, p_values.size() - 1)];
	return v > 0.0f ? v : p_default;
}

bool PhysXRiver3D::_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y, uint32_t p_mask) const {
	if (p_space == nullptr) {
		return false;
	}
	const real_t PROBE_ABOVE = ground_probe_height;
	const real_t PROBE_BELOW = ground_probe_depth;
	constexpr int MAX_SKIPS = 8;
	const Transform3D xf = get_global_transform();
	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = xf.xform(p_local + Vector3(0, PROBE_ABOVE, 0));
	ray.to = xf.xform(p_local - Vector3(0, PROBE_BELOW, 0));
	ray.collision_mask = p_mask;
	ray.collide_with_areas = false;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int skip = 0; skip < MAX_SKIPS; skip++) {
		PhysicsDirectSpaceState3D::RayResult result;
		if (!p_space->intersect_ray(ray, result)) {
			return false;
		}
		// Only static ground: anything floating or moving is skipped.
		if (ps->body_get_mode(result.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
			r_y = xf.affine_inverse().xform(result.position).y;
			return true;
		}
		ray.exclude.insert(result.rid);
	}
	return false;
}

// The ground as it was before any carving: from base_height_source's
// base_height_at(world_xz) when set (a terrain deformer keeps the uncarved
// heights; measuring the carved ground would carve deeper on every rebuild),
// else measured.
bool PhysXRiver3D::_base_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y) const {
	Node *source = base_height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(base_height_source);
	if (source != nullptr && source->has_method(SNAME("base_height_at"))) {
		const Transform3D xf = get_global_transform();
		const Vector3 w = xf.xform(p_local);
		const Variant v = source->call(SNAME("base_height_at"), Vector2(w.x, w.z));
		if (v.get_type() != Variant::FLOAT && v.get_type() != Variant::INT) {
			return false;
		}
		const float h = v;
		if (Math::is_nan(h)) {
			return false;
		}
		r_y = xf.affine_inverse().xform(Vector3(w.x, h, w.z)).y;
		return true;
	}
	return _ground_at(p_space, p_local, r_y, ground_collision_mask);
}

// The carved channel's height (local y) at p_n across a section, and how much
// the brush applies there (1 inside the banks, fading to 0 past them). A
// trapezoid: a flat floor 40% of the width, banks sloping up to the
// uncarved ground; at a bend the floor moves towards the outer bank (deep
// outside, a shallow bar inside -- Peytavie et al. 2019's asymmetric sections).
float PhysXRiver3D::_carve_target(const Section &p_sec, float p_n, float &r_mask) const {
	const float half = 0.5f * p_sec.width;
	const float offset = CLAMP(-p_sec.curvature * p_sec.width * p_sec.width * 0.25f, -0.25f * half, 0.25f * half);
	const float floor_half = 0.4f * half;
	const float lo = offset - floor_half;
	const float hi = offset + floor_half;
	const float an = Math::abs(p_n);
	r_mask = 1.0f;
	if (an > half) {
		const float f = carve_falloff > 1e-4f ? CLAMP((an - half) / carve_falloff, 0.0f, 1.0f) : 1.0f;
		r_mask = 1.0f - f * f * (3.0f - 2.0f * f);
		return p_sec.carve_top + (an - half) * carve_bank_slope;
	}
	float shape;
	if (p_n < lo) {
		shape = (p_n + half) / MAX(lo + half, 1e-3f);
	} else if (p_n > hi) {
		shape = (half - p_n) / MAX(half - hi, 1e-3f);
	} else {
		shape = 1.0f;
	}
	shape = CLAMP(shape, 0.0f, 1.0f);
	shape = shape * shape * (3.0f - 2.0f * shape);
	return p_sec.carve_top - (p_sec.carve_top - p_sec.carve_bed) * shape;
}

void PhysXRiver3D::_build() {
	build_pending = false;
	sections.clear();
	falls.clear();
	const Ref<Curve3D> c = get_curve();
	if (!is_inside_tree() || c.is_null() || c->get_point_count() < 2 || c->get_baked_length() < 0.05f) {
		_build_mesh();
		return;
	}
	const float length = c->get_baked_length();
	const int count = MAX(2, (int)Math::ceil(length / step_length) + 1);
	const float ds = length / (count - 1);

	// Arc length of each control point, for interpolating its width and depth.
	const int pc = c->get_point_count();
	LocalVector<float> point_s;
	point_s.resize(pc);
	for (int k = 0; k < pc; k++) {
		point_s[k] = c->get_closest_offset(c->get_point_position(k));
		if (k > 0) {
			point_s[k] = MAX(point_s[k], point_s[k - 1]);
		}
	}
	auto interp = [&](const PackedFloat32Array &p_values, float p_default, float p_s) -> float {
		if (p_s <= point_s[0]) {
			return _point_value(p_values, 0, p_default);
		}
		for (int k = 1; k < pc; k++) {
			if (p_s <= point_s[k]) {
				const float span = point_s[k] - point_s[k - 1];
				const float t = span > 1e-4f ? (p_s - point_s[k - 1]) / span : 1.0f;
				return Math::lerp(_point_value(p_values, k - 1, p_default), _point_value(p_values, k, p_default), t);
			}
		}
		return _point_value(p_values, pc - 1, p_default);
	};

	PhysicsDirectSpaceState3D *space = nullptr;
	if (snap_to_ground && is_inside_world() && get_world_3d().is_valid()) {
		space = get_world_3d()->get_direct_space_state();
	}

	_measure_outlet(space, c, ds);

	// Where each tributary joins: its mouth's place along this river, and which bank.
	{
		const Transform3D inv = get_global_transform().affine_inverse();
		for (Tributary &t : tributaries) {
			const Vector3 local = inv.xform(t.at);
			t.s = c->get_closest_offset(local);
			const Vector3 on = c->sample_baked(t.s, true);
			const Vector3 ahead = c->sample_baked(MIN(t.s + 0.5f, length), true) - c->sample_baked(MAX(t.s - 0.5f, 0.0f), true);
			const Vector3 side(ahead.z, 0.0f, -ahead.x); // towards the left bank
			t.from_right = (local - on).dot(side) < 0.0f;
		}
	}
	// Centre line: position, heading, width, given depth (0 = from Manning), bed.
	sections.resize(count);
	LocalVector<float> given_depth;
	LocalVector<float> bed;
	LocalVector<uint8_t> bed_found;
	given_depth.resize(count);
	bed.resize(count);
	bed_found.resize(count);
	Vector3 last_tangent(1, 0, 0);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.s = i * ds;
		sec.centre = c->sample_baked(sec.s, true);
		Vector3 t = c->sample_baked(MIN(sec.s + 0.5f * ds, length), true) - c->sample_baked(MAX(sec.s - 0.5f * ds, 0.0f), true);
		t.y = 0.0f;
		if (t.length_squared() < 1e-8f) {
			t = last_tangent;
		}
		t.normalize();
		sec.curvature = i > 0 ? last_tangent.cross(t).y / ds : 0.0f;
		last_tangent = t;
		sec.side = Vector3(t.z, 0.0f, -t.x);
		sec.width = interp(widths, default_width, sec.s);
		sec.discharge = get_discharge_at(sec.s);
		sec.psi_right = 0.0f;
		for (const Tributary &t : tributaries) {
			if (t.from_right && sec.s >= t.s) {
				sec.psi_right -= t.discharge;
			}
		}
		sec.mouth_fade = 1.0f;
		given_depth[i] = depths.is_empty() ? 0.0f : interp(depths, 0.0f, sec.s);
		float y = 0.0f;
		bed_found[i] = _base_ground_at(space, sec.centre, y) ? 1 : 0;
		bed[i] = bed_found[i] ? y : sec.centre.y - (given_depth[i] > 0.0f ? given_depth[i] : 0.0f);
	}
	if (space != nullptr) {
		// A floor added with the river (a cave chunk and its stream) can reach the physics space a few ticks later:
		// no ray at all finding it puts the build off, up to 30 ticks, before settling on the given depths.
		bool any = false;
		for (int i = 0; i < count && !any; i++) {
			any = bed_found[i] != 0;
		}
		if (!any && ground_probe_tries < 30) {
			ground_probe_tries++;
			sections.clear();
			build_pending = true;
			return;
		}
		ground_probe_tries = 0;
	}

	// Ground across each section, measured once (independent of the water).
	const int m = cross_segments;
	LocalVector<float> ground;
	LocalVector<uint8_t> ground_found;
	ground.resize(count * (m + 1));
	ground_found.resize(count * (m + 1));
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		for (int j = 0; j <= m; j++) {
			const float n = -half + j * (2.0f * half / m);
			float y = 0.0f;
			ground_found[i * (m + 1) + j] = _base_ground_at(space, sec.centre + sec.side * n, y) ? 1 : 0;
			ground[i * (m + 1) + j] = y;
		}
	}
	{
		// Curvature smoothed over about 6 m (a bend, not the polyline's kinks).
		const int cw = MAX(1, (int)Math::round(3.0f / ds));
		LocalVector<float> raw;
		raw.resize(count);
		for (int i = 0; i < count; i++) {
			raw[i] = sections[i].curvature;
		}
		for (int i = 0; i < count; i++) {
			float sum = 0.0f;
			int used = 0;
			for (int k = MAX(0, i - cw); k <= MIN(count - 1, i + cw); k++) {
				sum += raw[k];
				used++;
			}
			sections[i].curvature = sum / used;
		}
	}
	if (carve) {
		// The channel floor: carve_depth under the uncarved ground, never
		// climbing downstream -- through a rise it cuts (Horizon's river tool).
		for (int i = 0; i < count; i++) {
			Section &sec = sections[i];
			sec.carve_top = bed[i];
			sec.carve_bed = bed[i] - carve_depth;
			if (i > 0) {
				sec.carve_bed = MIN(sec.carve_bed, sections[i - 1].carve_bed);
			}
			bed[i] = sec.carve_bed;
			bed_found[i] = 1;
			// The water sees the ground as the brush leaves it ("lower to").
			const float half = 0.5f * sec.width + bank_margin;
			for (int j = 0; j <= m; j++) {
				const float n = -half + j * (2.0f * half / m);
				float mask = 0.0f;
				const float target = _carve_target(sec, n, mask);
				const int k = i * (m + 1) + j;
				const float base = ground_found[k] ? ground[k] : sec.carve_top;
				ground[k] = Math::lerp(base, MIN(base, target), mask);
				ground_found[k] = 1;
			}
		}
	}
	// The ground without obstacles, for the gradually varied profile (the water goes round rocks: the
	// flow solve's job, not the section's).
	LocalVector<float> bare_ground;
	LocalVector<uint8_t> bare_found;
	bare_ground.resize(ground.size());
	bare_found.resize(ground_found.size());
	for (uint32_t k = 0; k < ground.size(); k++) {
		bare_ground[k] = ground[k];
		bare_found[k] = ground_found[k];
	}
	// Rocks and other obstacles on their own layers stand on the (base or
	// carved) ground: a carved river measures the uncarved terrain, which has
	// no rocks in it.
	PhysicsDirectSpaceState3D *obstacle_space = space;
	if (obstacle_space == nullptr && obstacle_collision_mask != 0 && is_inside_world() && get_world_3d().is_valid()) {
		obstacle_space = get_world_3d()->get_direct_space_state();
	}
	if (obstacle_collision_mask != 0 && obstacle_space != nullptr) {
		for (int i = 0; i < count; i++) {
			const Section &sec = sections[i];
			const float half = 0.5f * sec.width + bank_margin;
			for (int j = 0; j <= m; j++) {
				const float n = -half + j * (2.0f * half / m);
				float y = 0.0f;
				const int k = i * (m + 1) + j;
				if (_ground_at(obstacle_space, sec.centre + sec.side * n, y, obstacle_collision_mask) && (!ground_found[k] || y > ground[k])) {
					ground[k] = y;
					ground_found[k] = 1;
				}
			}
		}
	}
	// Water depth across section i for a surface height: measured ground, or
	// with none, a parabolic bed inside the banks rising past them.
	auto depth_at = [&](int p_i, int p_j, float p_surface, float p_centre_depth) -> float {
		const Section &sec = sections[p_i];
		if (ground_found[p_i * (m + 1) + p_j]) {
			return p_surface - ground[p_i * (m + 1) + p_j];
		}
		const float half = 0.5f * sec.width + bank_margin;
		const float n = -half + p_j * (2.0f * half / m);
		const float u = 2.0f * n / sec.width;
		return Math::abs(u) <= 1.0f ? p_centre_depth * (1.0f - u * u) : -(Math::abs(n) - 0.5f * sec.width);
	};
	// Manning conveyance of section i at a surface height, divided channel:
	// K = integral(d^(5/3) dn) / n, so Q = K S^(1/2).
	auto conveyance_at = [&](int p_i, float p_surface, float p_centre_depth) -> float {
		const Section &sec = sections[p_i];
		const float dn = (sec.width + 2.0f * bank_margin) / m;
		float k = 0.0f;
		for (int j = 0; j <= m; j++) {
			const float d = depth_at(p_i, j, p_surface, p_centre_depth);
			if (d > 0.0f) {
				k += ((j == 0 || j == m) ? 0.5f : 1.0f) * dn * Math::pow(d, 5.0f / 3.0f);
			}
		}
		return k / roughness;
	};

	// Bed slope for Manning's normal depth, averaged over about 10 m, leaving
	// out steps steeper than max_slope: those are cascades the water falls
	// down, not a reach it flows along (counting them would drain the water
	// either side of a fall to a film).
	const int half_window = MAX(1, (int)Math::round(5.0f / ds));
	LocalVector<float> bed_slope;
	bed_slope.resize(count);
	for (int i = 0; i < count; i++) {
		const int a = MAX(0, i - half_window);
		const int b = MIN(count - 1, i + half_window);
		float drop = 0.0f;
		int used = 0;
		for (int k = a; k < b; k++) {
			const float g = (bed[k] - bed[k + 1]) / ds;
			if (g <= max_slope) {
				drop += g;
				used++;
			}
		}
		bed_slope[i] = used > 0 ? MAX(drop / used, 1e-4f) : max_slope;
	}

	// A lake outlet is drowned when the channel below is too slow to carry what a free sill would pass: its
	// normal depth stands above critical on the crest, and the discharge is the one whose energy at the sill
	// (normal depth plus velocity head) the lake's level just reaches (Henderson 1966, ch. 6).
	if (outlet_discharge > 0.0f) {
		const float to_local = get_global_transform().origin.y;
		const float level = outlet_level - to_local;
		const int ic = CLAMP((int)Math::round(outlet_crest_s / ds), 0, count - 1);
		const float dn = (sections[ic].width + 2.0f * bank_margin) / m;
		auto energy = [&](float p_q) -> float {
			const float target = p_q / Math::sqrt(bed_slope[ic]);
			float lo = 0.0f;
			float hi = 20.0f;
			for (int it = 0; it < 30; it++) {
				const float mid = 0.5f * (lo + hi);
				if (conveyance_at(ic, bed[ic] + mid, mid) < target) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			const float d = 0.5f * (lo + hi);
			float area = 0.0f;
			for (int j = 0; j <= m; j++) {
				area += dn * MAX(depth_at(ic, j, bed[ic] + d, d), 0.0f);
			}
			const float u = p_q / MAX(area, 1e-3f);
			return bed[ic] + d + u * u / (2.0f * GRAVITY);
		};
		if (energy(outlet_discharge) > level) {
			float lo = 0.0f;
			float hi = outlet_discharge;
			for (int it = 0; it < 24; it++) {
				const float mid = 0.5f * (lo + hi);
				if (energy(mid) <= level) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			outlet_discharge = lo;
			for (int i = 0; i < count; i++) {
				sections[i].discharge = get_discharge_at(sections[i].s);
			}
		}
	}

	// Surface: the normal depth over the real cross-section where the river
	// follows the ground (or the given depth), the path itself otherwise; then
	// never uphill.
	LocalVector<float> centre_depth;
	centre_depth.resize(count);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		float surface;
		if (given_depth[i] > 0.0f) {
			surface = bed_found[i] ? bed[i] + given_depth[i] : sec.centre.y;
			centre_depth[i] = given_depth[i];
		} else if (bed_found[i] || snap_to_ground) {
			// Bisect for the surface whose conveyance carries the discharge:
			// Q = K(surface) S^(1/2). Without measured ground, the parabola's
			// centre depth is the unknown instead.
			const float target = sec.discharge / Math::sqrt(bed_slope[i]);
			float lo = 0.0f;
			float hi = 20.0f;
			for (int it = 0; it < 30; it++) {
				const float mid = 0.5f * (lo + hi);
				if (conveyance_at(i, bed[i] + mid, mid) < target) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			const float d = MAX(0.5f * (lo + hi), 0.02f);
			surface = bed[i] + d;
			centre_depth[i] = d;
		} else {
			surface = sec.centre.y;
			centre_depth[i] = Math::pow(sec.discharge * roughness / (sec.width * Math::sqrt(bed_slope[i])), 0.6f);
			bed[i] = surface - centre_depth[i];
		}
		sec.surface = surface;
		if (i > 0) {
			sec.surface = MIN(sec.surface, sections[i - 1].surface);
		}
		// A cascade is a real fall: steeper than max_slope and more than a few
		// centimetres (the running minimum leaves small steps where a rock's
		// backwater meets the reach below it; those are not falls).
		const float drop = i > 0 ? sections[i - 1].surface - sec.surface : 0.0f;
		sec.cascade = drop / ds > max_slope && drop > CASCADE_MIN_DROP;
		sec.centre.y = sec.surface;
	}
	// The mouth: the river ends in a lake, the sea or another river. Its surface
	// can't fall below the water it flows into (the level pool: the reach
	// ponds back until its own normal depth rises above that level), and past
	// the shoreline the ribbon fades into that water over mouth_blend.
	{
		Node *target = _mouth_node();
		PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(target);
		PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(target);
		if (sea != nullptr || main != nullptr) {
			const Transform3D xf = get_global_transform();
			const Transform3D inv = xf.affine_inverse();
			int mouth = -1;
			float level_world = 0.0f;
			// The mouth: the first section over that water whose own surface has
			// come down to its level (upstream of it the river may run over
			// water that only floods its bed, still above it).
			for (int i = 0; i < count; i++) {
				const Vector3 w = xf.xform(sections[i].centre);
				if (sea != nullptr ? !sea->is_wet(w) : !(main->sample_depth(w) > 0.0f)) {
					continue;
				}
				const float level = sea != nullptr ? sea->get_global_position().y + sea->get_water_level() : main->sample_height(w);
				if (Math::is_finite(level) && w.y <= level + 0.02f) {
					mouth = i;
					level_world = level;
					break;
				}
			}
			if (main != nullptr) {
				mouth_level_seen = mouth >= 0 && Math::is_finite(level_world) ? level_world : -Math::INF;
			}
			if (mouth >= 0 && Math::is_finite(level_world)) {
				for (int i = 0; i < count; i++) {
					Section &sec = sections[i];
					const float level = inv.xform(Vector3(xf.xform(sec.centre).x, level_world, xf.xform(sec.centre).z)).y;
					if (sec.surface < level) {
						sec.surface = level;
						sec.centre.y = level;
						sec.cascade = false;
					}
					if (i > mouth) {
						const float past = sec.s - sections[mouth].s;
						const float f = mouth_blend > 1e-3f ? CLAMP(past / mouth_blend, 0.0f, 1.0f) : 1.0f;
						sec.mouth_fade = 1.0f - f * f * (3.0f - 2.0f * f);
					}
				}
			}
		}
	}
	// Gradually varied flow (docs/ideas/water.md "River profile from hydraulics"): the standard step
	// method (Chow 1959, ch. 10) on the real cross-sections, replacing normal depth. Subcritical water is
	// stepped up from its downstream control (the mouth's level, or critical depth where the water must
	// pass it), supercritical water down from its upstream one; where both exist the one with the larger
	// specific force holds, so a fast reach ends in a hydraulic jump where the momentum of the slower water
	// below matches it (Belanger's sequent depth). Falls (bed steps steeper than max_slope) break the steps.
	for (int i = 0; i < count; i++) {
		sections[i].froude = 0.0f;
		sections[i].critical = -Math::INF;
		sections[i].jump = 0.0f;
		sections[i].jump_foam = 0.0f;
		sections[i].tilt = 0.0f;
	}
	{
		// Wet area A, top width T, conveyance K and the hydrostatic force term P = integral(d^2 / 2 dn), on
		// the ground without obstacles.
		auto bare_depth = [&](int p_i, int p_j, float p_surface) -> float {
			const int k = p_i * (m + 1) + p_j;
			if (bare_found[k]) {
				return p_surface - bare_ground[k];
			}
			return depth_at(p_i, p_j, p_surface, p_surface - bed[p_i]);
		};
		auto props_one = [&](int p_i, float p_z, float &r_a, float &r_t, float &r_k, float &r_p) {
			const Section &sec = sections[p_i];
			const float dn = (sec.width + 2.0f * bank_margin) / m;
			r_a = r_t = r_k = r_p = 0.0f;
			for (int j = 0; j <= m; j++) {
				const float d = bare_depth(p_i, j, p_z);
				if (d > 0.0f) {
					const float w = ((j == 0 || j == m) ? 0.5f : 1.0f) * dn;
					r_a += w * d;
					r_t += w;
					r_k += w * Math::pow(d, 5.0f / 3.0f);
					r_p += w * 0.5f * d * d;
				}
			}
			r_k /= roughness;
		};
		// The widest of the sections within a metre (at the same surface height): a short narrowing, a
		// boulder in the bed, is passed round rather than choking the river.
		const int reach = MAX(1, (int)Math::round(1.0f / ds));
		auto props = [&](int p_i, float p_z, float &r_a, float &r_t, float &r_k, float &r_p) {
			props_one(p_i, p_z, r_a, r_t, r_k, r_p);
			for (int o = -reach; o <= reach; o++) {
				const int q = p_i + o;
				// Not across a step: below a cascade the same level is a deeper section, not this one.
				if (o == 0 || q < 0 || q >= count || Math::abs(bed[q] - bed[p_i]) > max_slope * Math::abs(o) * ds) {
					continue;
				}
				float a, t, k, p;
				props_one(q, p_z, a, t, k, p);
				if (a > r_a) {
					r_a = a;
					r_t = t;
					r_k = k;
					r_p = p;
				}
			}
		};
		auto energy = [&](int p_i, float p_z, float &r_sf) -> float {
			float a, t, k, p;
			props(p_i, p_z, a, t, k, p);
			const float q = sections[p_i].discharge;
			r_sf = k > 1e-9f ? (q / k) * (q / k) : 1.0f;
			return p_z + (a > 1e-6f ? q * q / (2.0f * GRAVITY * a * a) : 1e3f);
		};
		auto momentum = [&](int p_i, float p_z) -> float {
			float a, t, k, p;
			props(p_i, p_z, a, t, k, p);
			const float q = sections[p_i].discharge;
			return (a > 1e-6f ? q * q / (GRAVITY * a) : 1e6f) + p;
		};
		// Which sections the profile owns: on measured ground (or snapped), no given depth, not past the mouth.
		LocalVector<uint8_t> owned;
		owned.resize(count);
		LocalVector<uint8_t> fall_after; // a bed step steeper than max_slope to the next section
		fall_after.resize(count);
		for (int i = 0; i < count; i++) {
			owned[i] = gradually_varied_flow && given_depth[i] <= 0.0f && (bed_found[i] || snap_to_ground) && sections[i].mouth_fade >= 1.0f;
			fall_after[i] = i + 1 < count && (bed[i] - bed[i + 1]) / ds > max_slope; // a cascade: max_slope's meaning
		}
		// Sections on a cascade keep their surface (the falls' own treatment); the profile breaks there.
		for (int i = 0; i < count; i++) {
			if (fall_after[i] || (i > 0 && fall_after[i - 1])) {
				owned[i] = 0;
			}
		}
		auto brink = [&](int p_i) -> bool { // the last section above a cascade
			return p_i + 1 < count && (fall_after[p_i + 1] || fall_after[p_i]);
		};
		// The lowest point of each section's bare ground (the bed under any rock).
		LocalVector<float> low;
		low.resize(count);
		for (int i = 0; i < count; i++) {
			low[i] = bed[i];
			for (int j = 0; j <= m; j++) {
				const int k = i * (m + 1) + j;
				if (bare_found[k]) {
					low[i] = MIN(low[i], bare_ground[k]);
				}
			}
		}
		// Critical surfaces: Q^2 T / (g A^3) = 1.
		LocalVector<float> zc;
		zc.resize(count);
		for (int i = 0; i < count; i++) {
			const float q = sections[i].discharge;
			float lo = 0.002f;
			float hi = 20.0f;
			for (int it = 0; it < 40; it++) {
				const float mid = 0.5f * (lo + hi);
				float a, t, k, p;
				props(i, low[i] + mid, a, t, k, p);
				if (a > 1e-6f && q * q * t / (GRAVITY * a * a * a) > 1.0f) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			zc[i] = low[i] + 0.5f * (lo + hi);
			sections[i].critical = zc[i];
		}
		if (gradually_varied_flow) {
			// Subcritical, from downstream up. E_i - Sf_i ds / 2 = E_{i+1} + Sf_{i+1} ds / 2 on the deep branch.
			LocalVector<float> z_sub;
			LocalVector<uint8_t> control; // the water passes critical depth here
			z_sub.resize(count);
			control.resize(count);
			// Below a steep reach (normal depth under critical) with nothing holding the water up downstream,
			// there is no slow water to step: only critical stands in (whose specific force any fast water
			// beats) until the slope eases, where the water passes critical (the control).
			bool steep_tail = false;
			for (int i = count - 1; i >= 0; i--) {
				control[i] = 0;
				if (!owned[i]) {
					z_sub[i] = sections[i].surface;
					steep_tail = false;
					continue;
				}
				if (steep_tail) {
					z_sub[i] = zc[i];
					if (sections[i].surface >= zc[i] || brink(i)) {
						control[i] = 1;
						steep_tail = false;
					}
					continue;
				}
				if (brink(i)) {
					// A free overfall: critical depth just upstream of the lip (Rouse 1936; the brink's own
					// drawdown to 0.715 y_c is _find_falls'), unless the pool below drowns the step.
					float tail = -Math::INF;
					for (int j = i + 1; j < count; j++) {
						if (owned[j]) {
							tail = z_sub[j];
							break;
						}
					}
					z_sub[i] = MAX(zc[i], tail);
					control[i] = 1;
					steep_tail = false;
					continue;
				}
				if (i == count - 1 || !owned[i + 1]) {
					// The end, the edge of the owned reach, or a brink: normal depth (or the level the reach
					// below holds), never below critical (a brink's own drawdown is _find_falls').
					z_sub[i] = MAX(i + 1 < count && !owned[i + 1] ? MAX(sections[i].surface, z_sub[i + 1]) : sections[i].surface, zc[i]);
					control[i] = brink(i) || z_sub[i] <= zc[i] + 1e-4f;
					steep_tail = !brink(i) && z_sub[i] <= zc[i] + 1e-4f;
					continue;
				}
				float sf_down;
				const float e_down = energy(i + 1, z_sub[i + 1], sf_down);
				const float target = e_down + 0.5f * sf_down * ds;
				auto residual = [&](float p_z) -> float {
					float sf;
					const float e = energy(i, p_z, sf);
					return e - 0.5f * sf * ds - target;
				};
				if (residual(zc[i]) >= 0.0f) {
					z_sub[i] = zc[i]; // no subcritical water carries this: it passes critical here
					control[i] = 1;
					continue;
				}
				float lo = zc[i];
				float hi = zc[i] + 1.0f;
				while (residual(hi) < 0.0f && hi < zc[i] + 40.0f) {
					hi += 2.0f * (hi - zc[i]);
				}
				for (int it = 0; it < 40; it++) {
					const float mid = 0.5f * (lo + hi);
					if (residual(mid) < 0.0f) {
						lo = mid;
					} else {
						hi = mid;
					}
				}
				z_sub[i] = 0.5f * (lo + hi);
			}
			// Supercritical, from upstream down, chosen where its specific force is the larger.
			bool alive = false;
			float z_prev = 0.0f;
			float fr_prev = 0.0f;
			for (int i = 0; i < count; i++) {
				Section &sec = sections[i];
				if (!owned[i]) {
					alive = false;
					continue;
				}
				float z_sup = -Math::INF;
				if (alive) {
					float sf_up;
					const float e_up = energy(i - 1, z_prev, sf_up);
					const float target = e_up - 0.5f * sf_up * ds;
					auto residual = [&](float p_z) -> float {
						float sf;
						const float e = energy(i, p_z, sf);
						return e + 0.5f * sf * ds - target;
					};
					if (residual(zc[i]) <= 0.0f) {
						float lo = low[i] + 0.002f;
						float hi = zc[i];
						for (int it = 0; it < 40; it++) {
							const float mid = 0.5f * (lo + hi);
							if (residual(mid) > 0.0f) {
								lo = mid;
							} else {
								hi = mid;
							}
						}
						z_sup = 0.5f * (lo + hi);
					} else {
						alive = false; // too little energy to stay fast: it chokes to critical
					}
				} else if (i > 0 && !owned[i - 1]) {
					// The foot of a step: the water arrives fast and shallow. Rand (1955), for a free overfall
					// onto a floor h below the brink: y1 = 0.54 h (y_c / h)^1.275, which keeps the fraction
					// E1 / E0 of the energy above the floor; that fraction is applied to this section's shape.
					int j = i - 1;
					bool dropped = false;
					while (j >= 0 && !owned[j]) {
						dropped = dropped || fall_after[j];
						j--;
					}
					dropped = dropped || (j >= 0 && fall_after[j]);
					const float h = j >= 0 ? low[j] - low[i] : 0.0f;
					const float qw = sections[i].discharge / MAX(sections[i].width, 0.5f);
					const float yc = Math::pow(qw * qw / GRAVITY, 1.0f / 3.0f);
					// A waterfall (a drop of fall_min_height or more) plunges into the pool it has dug instead:
					// natural falls scour one, so its foot stays at the pool's level.
					const bool plunges = fall_min_height > 0.0f && j >= 0 && zc[j] - sections[i].surface >= fall_min_height;
					if (dropped && h > yc && !plunges) {
						const float y1 = MAX(0.54f * h * Math::pow(yc / h, 1.275f), 0.01f);
						const float kept = (y1 + qw * qw / (2.0f * GRAVITY * y1 * y1)) / (h + 1.5f * yc);
						float sf;
						const float target = low[i] + kept * (energy(j, zc[j], sf) - low[i]);
						if (energy(i, zc[i], sf) < target) {
							float lo = low[i] + 0.002f;
							float hi = zc[i];
							for (int it = 0; it < 40; it++) {
								const float mid = 0.5f * (lo + hi);
								if (energy(i, mid, sf) > target) {
									lo = mid;
								} else {
									hi = mid;
								}
							}
							z_sup = 0.5f * (lo + hi);
						}
					} else if (sections[i].surface < zc[i] - 1e-3f) {
						z_sup = sections[i].surface; // a steep reach's own inflow: normal depth
					}
				} else if (i == 0 && sections[i].surface < zc[i] - 1e-3f) {
					z_sup = sections[i].surface;
				}
				float z = z_sub[i];
				if (Math::is_finite(z_sup) && z_sup < z_sub[i] - 1e-4f && momentum(i, z_sup) > momentum(i, z_sub[i])) {
					z = z_sup;
					alive = true;
				} else {
					// The fast water meets slower water whose momentum it cannot push back: a jump.
					if (alive && fr_prev > 1.0f) {
						sec.jump = fr_prev;
					}
					alive = false;
				}
				// Leaving a control (the water passed critical here) into a steep reach: fast water from here.
				if (!alive && control[i] && i + 1 < count && owned[i + 1] && !brink(i) && sections[i + 1].surface < zc[i + 1]) {
					alive = true;
				}
				if (brink(i)) {
					alive = false; // the water drops into the pool below: subcritical from there
				}
				sec.surface = z;
				sec.centre.y = z;
				float a, t, k, p;
				props(i, z, a, t, k, p);
				sec.froude = (a > 1e-6f && t > 1e-6f) ? sec.discharge / (a * Math::sqrt(GRAVITY * a / t)) : 0.0f;
				z_prev = z;
				fr_prev = sec.froude;
			}
			// Through a cascade the surface only falls: a section the passes did not own keeps the original
			// running-min level, which can sit above a brink the profile drew down to critical.
			for (int i = 1; i < count; i++) {
				if (!owned[i] && (fall_after[i] || fall_after[i - 1]) && sections[i].surface > sections[i - 1].surface) {
					sections[i].surface = MAX(sections[i - 1].surface, low[i] + 0.002f);
					sections[i].centre.y = sections[i].surface;
				}
			}
			// Cascades from the profile (as above, but a jump steps the surface up, not down).
			for (int i = 1; i < count; i++) {
				if (owned[i]) {
					const float drop = sections[i - 1].surface - sections[i].surface;
					sections[i].cascade = drop / ds > max_slope && drop > CASCADE_MIN_DROP;
				}
			}
			// A jump's roller: white water over about six sequent depths (Peterka 1958), stronger the higher
			// Fr1 (an undular jump, Fr1 below about 1.7, has none: Chanson 1995).
			for (int i = 0; i < count; i++) {
				const float fr1 = sections[i].jump;
				if (fr1 <= 0.0f) {
					continue;
				}
				const float y2 = MAX(sections[i].surface - bed[i], 0.05f);
				const float roller = 6.0f * y2;
				float f = CLAMP((fr1 - 1.7f) / 2.8f, 0.0f, 1.0f);
				const float strength = 0.35f + 0.65f * f * f * (3.0f - 2.0f * f);
				if (fr1 < 1.7f) {
					continue;
				}
				for (int k = i; k < count && sections[k].s - sections[i].s <= roller; k++) {
					const float u = (sections[k].s - sections[i].s) / MAX(roller, 1e-3f);
					sections[k].jump_foam = MAX(sections[k].jump_foam, strength * (1.0f - u * u));
				}
			}
		}
		// Froude where the profile was not stepped.
		for (int i = 0; i < count; i++) {
			Section &sec = sections[i];
			if (sec.froude <= 0.0f) {
				float a, t, k, p;
				props(i, sec.surface, a, t, k, p);
				sec.froude = (a > 1e-6f && t > 1e-6f) ? sec.discharge / (a * Math::sqrt(GRAVITY * a / t)) : 0.0f;
			}
		}
		// Bend superelevation: the surface banks up to the outside of a bend by about U^2 w / (g r) (Chow 1959,
		// 17-4), here as a tilt U^2 curvature / g across, held to half the mean depth over the half width.
		if (bend_superelevation) {
			for (int i = 0; i < count; i++) {
				Section &sec = sections[i];
				float a, t, k, p;
				props(i, sec.surface, a, t, k, p);
				if (a <= 1e-6f || t <= 1e-6f || sec.mouth_fade < 1.0f) {
					continue;
				}
				const float u = sec.discharge / a;
				const float half = 0.5f * sec.width + bank_margin;
				const float limit = 0.5f * (a / t) / MAX(half, 0.1f);
				sec.tilt = CLAMP(-sec.curvature * u * u / GRAVITY, -limit, limit);
			}
		}
	}
	_find_falls(space, bed, ds);
	for (int i = 0; i < count; i++) {
		const int a = MAX(0, i - half_window);
		const int b = MIN(count - 1, i + half_window);
		sections[i].slope = b > a ? (sections[a].surface - sections[b].surface) / ((b - a) * ds) : 0.0f;
	}

	// Across: depth per sample, then the discharge shared as depth^(5/3).
	float carried_foam = 0.0f;
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		sec.depth.resize(m + 1);
		sec.speed_at.resize(m + 1);
		const float dn = 2.0f * half / m;
		float conveyance = 0.0f; // integral of d^(5/3) dn
		float area = 0.0f;
		float wet_width = 0.0f;
		for (int j = 0; j <= m; j++) {
			const float d = depth_at(i, j, sec.surface + sec.tilt * (-half + j * dn), sec.surface - bed[i]);
			sec.depth[j] = d;
			const float w = (j == 0 || j == m) ? 0.5f : 1.0f;
			if (d > 0.0f) {
				conveyance += w * dn * Math::pow(d, 5.0f / 3.0f);
				area += w * dn * d;
				wet_width += w * dn;
			}
		}
		for (int j = 0; j <= m; j++) {
			const float d = sec.depth[j];
			// v = q / d = Q d^(2/3) / integral(d^(5/3)).
			sec.speed_at[j] = (d > 0.0f && conveyance > 1e-6f) ? MIN(sec.discharge * Math::pow(d, 2.0f / 3.0f) / conveyance, MAX_SPEED) : 0.0f;
		}
		sec.speed = area > 1e-6f ? MIN(sec.discharge / area, MAX_SPEED) : 0.0f;
		// White water: supercritical flow (Froude above ~1) and the foot of every
		// cascade, then carried downstream at the current's speed, fading.
		const float mean_depth = wet_width > 1e-4f ? area / wet_width : 0.0f;
		const float froude = mean_depth > 1e-3f ? sec.speed / Math::sqrt(GRAVITY * mean_depth) : 0.0f;
		float source = CLAMP((froude - 0.8f) / 0.8f, 0.0f, 1.0f);
		// Fast water alone is lace, not a sheet: it whitens only where air gets in (a cascade, a plunge,
		// a jump's roller), so a long supercritical run below a drop keeps its texture.
		source = 0.4f * source * source * (3.0f - 2.0f * source);
		if (sec.cascade) {
			source = 1.0f;
		}
		source = MAX(source, MAX(sec.plunge, sec.jump_foam));
		carried_foam *= Math::exp(-ds / (MAX(sec.speed, 0.1f) * foam_persistence));
		carried_foam = MAX(carried_foam, source);
		sec.foam = carried_foam;
	}
	_solve_flow(ds);
	_solve_standing_waves();
	_build_mesh();
	_build_fall_effects();
	live_fields_dirty = true;
	_register_mouth();
	_register_outlet();
	emit_signal(SNAME("rebuilt"));
	update_configuration_warnings();
	if (carve) {
		emit_signal(SNAME("carve_changed"));
	}
}

// Flow round rocks. Steady river flow is friction dominated (Manning), so the
// unit discharge q follows the surface gradient: q = -K grad(eta) with
// K = d^(5/3) / (n sqrt(S)) (the diffusive-wave model of flood codes such as
// LISFLOOD-FP, Bates and De Roo 2000). Continuity, div q = 0, then makes q the
// curl of a stream function psi with div(grad(psi) / K) = 0. In a straight
// reach that gives exactly the divided channel's d^(5/3) profile, and round a
// rock (dry cells inside the river) the flow parts, speeds up past its sides
// and slows in front and behind. Solved in the river's own strip coordinates
// (s along, n across) with the bend's metric h = 1 - curvature n, psi held at
// 0 on the right bank and the discharge on the left, ends held at the
// section's own profile; an island's psi is chosen so no net flow goes round
// it (as PhysXWaterSurface3D's flow bake). Then white water per sample:
// Froude, cascades, and where the current strikes or grazes a rock, carried
// downstream along the flow.
void PhysXRiver3D::_solve_flow(float p_ds) {
	const int count = (int)sections.size();
	const int m = cross_segments;
	const int row = m + 1;
	const int cells = count * row;
	constexpr float DRY = 0.02f; // m: shallower is a wall
	constexpr float MIN_D = 0.05f; // m: depth floor for weights and speeds
	auto is_wet = [&](int p_i, int p_j) -> bool {
		return sections[p_i].depth[p_j] > DRY;
	};
	auto dn_of = [&](int p_i) -> float {
		return (sections[p_i].width + 2.0f * bank_margin) / m;
	};
	auto n_of = [&](int p_i, int p_j) -> float {
		const float half = 0.5f * sections[p_i].width + bank_margin;
		return -half + p_j * (2.0f * half / m);
	};
	auto metric = [&](int p_i, int p_j) -> float {
		return MAX(0.2f, 1.0f - sections[p_i].curvature * n_of(p_i, p_j));
	};
	LocalVector<float> inv_k; // 1 / d^(5/3), 0 where dry
	inv_k.resize(cells);
	for (int i = 0; i < count; i++) {
		for (int j = 0; j <= m; j++) {
			inv_k[i * row + j] = is_wet(i, j) ? Math::pow(MAX(sections[i].depth[j], MIN_D), -5.0f / 3.0f) : 0.0f;
		}
	}

	// Start from each section's own d^(5/3) profile, integrated from the right bank.
	LocalVector<float> psi;
	psi.resize(cells);
	for (int i = 0; i < count; i++) {
		const float dn = dn_of(i);
		float acc = 0.0f;
		psi[i * row] = 0.0f;
		for (int j = 1; j <= m; j++) {
			const float a = is_wet(i, j - 1) ? Math::pow(sections[i].depth[j - 1], 5.0f / 3.0f) : 0.0f;
			const float b = is_wet(i, j) ? Math::pow(sections[i].depth[j], 5.0f / 3.0f) : 0.0f;
			acc += 0.5f * (a + b) * dn;
			psi[i * row + j] = acc;
		}
		const float scale = acc > 1e-9f ? sections[i].discharge / acc : 0.0f;
		for (int j = 0; j <= m; j++) {
			psi[i * row + j] = sections[i].psi_right + psi[i * row + j] * scale;
		}
	}

	// Dry patches: the banks keep their start values (0 and the discharge);
	// a patch that touches neither bank edge nor an end is an island, solved for.
	LocalVector<int> comp;
	comp.resize(cells);
	for (int c = 0; c < cells; c++) {
		comp[c] = -1;
	}
	LocalVector<uint8_t> comp_island;
	LocalVector<int> stack;
	int comps = 0;
	for (int c = 0; c < cells; c++) {
		if (comp[c] >= 0 || inv_k[c] > 0.0f) {
			continue;
		}
		bool island = true;
		stack.clear();
		stack.push_back(c);
		comp[c] = comps;
		while (!stack.is_empty()) {
			const int k = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			const int i = k / row;
			const int j = k % row;
			if (i == 0 || i == count - 1 || j == 0 || j == m) {
				island = false;
			}
			const int nb[4] = { i > 0 ? k - row : -1, i < count - 1 ? k + row : -1, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
			for (int q = 0; q < 4; q++) {
				if (nb[q] >= 0 && comp[nb[q]] < 0 && inv_k[nb[q]] == 0.0f) {
					comp[nb[q]] = comps;
					stack.push_back(nb[q]);
				}
			}
		}
		comp_island.push_back(island ? 1 : 0);
		comps++;
	}
	bool any_island = false;
	for (int q = 0; q < comps; q++) {
		any_island = any_island || comp_island[q];
	}
	// An island's start value: the mean of its cells' start values.
	LocalVector<float> comp_value;
	comp_value.resize(comps);
	if (any_island) {
		LocalVector<int> comp_cells;
		comp_cells.resize(comps);
		for (int q = 0; q < comps; q++) {
			comp_value[q] = 0.0f;
			comp_cells[q] = 0;
		}
		for (int c = 0; c < cells; c++) {
			if (comp[c] >= 0 && comp_island[comp[c]]) {
				comp_value[comp[c]] += psi[c];
				comp_cells[comp[c]]++;
			}
		}
		for (int q = 0; q < comps; q++) {
			if (comp_island[q]) {
				comp_value[q] /= MAX(comp_cells[q], 1);
			}
		}
		for (int c = 0; c < cells; c++) {
			if (comp[c] >= 0 && comp_island[comp[c]]) {
				psi[c] = comp_value[comp[c]];
			}
		}

		if (bool(GLOBAL_GET("physics/physx_3d/water/multigrid_flow"))) {
			// The same equation in its symmetric finite-volume form (cell area h ds dn): along s a face
			// conducts K_f dn_f / (h_f ds), across K_f h_f ds / dn, with K_f the mean of a wet pair or the wet
			// cell's own onto a wall, and the metric h at the face; then multigrid-preconditioned CG.
			LocalVector<int> kind;
			LocalVector<int> island_of;
			LocalVector<float> fcx;
			LocalVector<float> fcz;
			kind.resize(cells);
			fcx.resize(cells);
			fcz.resize(cells);
			island_of.resize(comps);
			int islands = 0;
			for (int q = 0; q < comps; q++) {
				island_of[q] = comp_island[q] ? islands++ : -1;
			}
			auto k_face = [&](int p_a, int p_b) -> float {
				const float ka = inv_k[p_a];
				const float kb = inv_k[p_b];
				return (ka > 0.0f && kb > 0.0f) ? 0.5f * (ka + kb) : MAX(ka, kb);
			};
			for (int i = 0; i < count; i++) {
				const float dn = dn_of(i);
				for (int j = 0; j <= m; j++) {
					const int k = i * row + j;
					if (i == 0 || i == count - 1) {
						kind[k] = FlowPoisson::FIXED;
					} else if (comp[k] >= 0) {
						kind[k] = island_of[comp[k]] >= 0 ? FlowPoisson::ISLAND + island_of[comp[k]] : FlowPoisson::FIXED;
					} else {
						kind[k] = FlowPoisson::UNKNOWN;
					}
					fcx[k] = j < m ? k_face(k, k + 1) * 0.5f * (metric(i, j) + metric(i, j + 1)) * p_ds / dn : 0.0f;
					fcz[k] = i < count - 1 ? k_face(k, k + row) * 0.5f * (dn + dn_of(i + 1)) / (0.5f * (metric(i, j) + metric(i + 1, j)) * p_ds) : 0.0f;
				}
			}
			FlowPoisson::solve(row, count, fcx, fcz, kind, islands, psi);
		} else {
			// SOR on div(grad(psi) / K) = 0 with the bend metric. Face weights:
			// along s 1 / (h K ds^2), across h / (K dn^2); a face onto a wall
			// takes the wet cell's own K.
			const float ds2 = p_ds * p_ds;
			auto face = [&](int p_a, int p_b, bool p_along, float p_h, float p_dn2) -> float {
				const float ka = inv_k[p_a];
				const float kb = inv_k[p_b];
				const float k = (ka > 0.0f && kb > 0.0f) ? 0.5f * (ka + kb) : MAX(ka, kb);
				return p_along ? k / (p_h * ds2) : k * p_h / p_dn2;
			};
			LocalVector<float> comp_num;
			LocalVector<float> comp_den;
			comp_num.resize(comps);
			comp_den.resize(comps);
			const float omega = 1.85f;
			const float tolerance = 1e-5f * sections[count - 1].discharge;
			for (int iter = 0; iter < 4000; iter++) {
				float max_change = 0.0f;
				for (int i = 1; i < count - 1; i++) {
					const float dn2 = dn_of(i) * dn_of(i);
					for (int j = 0; j <= m; j++) {
						const int k = i * row + j;
						if (inv_k[k] == 0.0f) {
							continue;
						}
						const float h = metric(i, j);
						float num = 0.0f;
						float den = 0.0f;
						const int nb[4] = { k - row, k + row, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
						for (int q = 0; q < 4; q++) {
							if (nb[q] < 0) {
								continue; // the strip's side edge: no flow across it
							}
							const float w = face(k, nb[q], q < 2, h, dn2);
							num += w * psi[nb[q]];
							den += w;
						}
						if (den > 0.0f) {
							const float change = omega * (num / den - psi[k]);
							psi[k] += change;
							max_change = MAX(max_change, Math::abs(change));
						}
					}
				}
				// Islands: psi where the flow round them sums to zero.
				for (int q = 0; q < comps; q++) {
					comp_num[q] = 0.0f;
					comp_den[q] = 0.0f;
				}
				for (int i = 1; i < count - 1; i++) {
					const float dn2 = dn_of(i) * dn_of(i);
					for (int j = 0; j <= m; j++) {
						const int k = i * row + j;
						if (inv_k[k] == 0.0f) {
							continue;
						}
						const int nb[4] = { k - row, k + row, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
						for (int q = 0; q < 4; q++) {
							if (nb[q] < 0 || comp[nb[q]] < 0 || !comp_island[comp[nb[q]]]) {
								continue;
							}
							const float w = face(k, nb[q], q < 2, metric(i, j), dn2);
							comp_num[comp[nb[q]]] += w * psi[k];
							comp_den[comp[nb[q]]] += w;
						}
					}
				}
				for (int c = 0; c < cells; c++) {
					const int q = comp[c];
					if (q >= 0 && comp_island[q] && comp_den[q] > 0.0f) {
						psi[c] = comp_num[q] / comp_den[q];
					}
				}
				if (max_change < tolerance && iter > 4) {
					break;
				}
			}
		}
	}

	// Velocity from psi: along = dpsi/dn / d, across = -dpsi/ds / (h d).
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.across_at.resize(row);
		sec.foam_at.resize(row);
		sec.psi_at.resize(row);
		for (int j = 0; j <= m; j++) {
			sec.psi_at[j] = psi[i * row + j];
		}
		const float dn = dn_of(i);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			if (inv_k[k] == 0.0f) {
				sec.speed_at[j] = 0.0f;
				sec.across_at[j] = 0.0f;
				continue;
			}
			const int jl = MAX(j - 1, 0);
			const int jr = MIN(j + 1, m);
			const int il = MAX(i - 1, 0);
			const int ir = MIN(i + 1, count - 1);
			const float d = MAX(sec.depth[j], MIN_D);
			const float dpsi_dn = (psi[i * row + jr] - psi[i * row + jl]) / ((jr - jl) * dn);
			const float dpsi_ds = ir > il ? (psi[ir * row + j] - psi[il * row + j]) / ((ir - il) * p_ds) : 0.0f;
			sec.speed_at[j] = CLAMP(dpsi_dn / d, -MAX_SPEED, MAX_SPEED);
			sec.across_at[j] = CLAMP(-dpsi_ds / (metric(i, j) * d), -MAX_SPEED, MAX_SPEED);
		}
	}

	// White water per sample. Sources: the local Froude number (so it gathers in
	// the fast core), cascades across the whole river, and the current meeting
	// a rock -- the pillow on its upstream face and the shear lines past its
	// sides (an island's wet neighbours, faster water foaming more). Carried
	// downstream along the local flow, fading at the mean current's pace, so
	// two trails close in behind each rock.
	LocalVector<float> carried;
	carried.resize(row);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		const float decay = Math::exp(-p_ds / (MAX(sec.speed, 0.1f) * foam_persistence));
		const float dn = dn_of(i);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			float src = 0.0f;
			if (inv_k[k] > 0.0f) {
				// Supercritical water and cascades.
				const float d = MAX(sec.depth[j], MIN_D);
				const float speed = Math::sqrt(sec.speed_at[j] * sec.speed_at[j] + sec.across_at[j] * sec.across_at[j]);
				const float fr = speed / Math::sqrt(GRAVITY * d);
				float f = CLAMP((fr - 0.8f) / 0.8f, 0.0f, 1.0f);
				// Lace, not a sheet (as past a rock): fast water whitens through only where air is driven
				// in, at a cascade, a plunge or a jump's roller.
				src = 0.4f * f * f * (3.0f - 2.0f * f);
				if (sec.cascade) {
					src = 1.0f;
				}
				src = MAX(src, MAX(sec.plunge, sec.jump_foam));
				// A rock next to this sample.
				bool by_rock = false;
				for (int di = -1; di <= 1 && !by_rock; di++) {
					for (int dj = -1; dj <= 1; dj++) {
						const int ii = i + di;
						const int jj = j + dj;
						if (ii < 0 || ii >= count || jj < 0 || jj > m) {
							continue;
						}
						const int c = comp[ii * row + jj];
						if (c >= 0 && comp_island[c]) {
							by_rock = true;
							break;
						}
					}
				}
				if (by_rock) {
					// Lace rather than a sheet: up to 0.75, from 0.5 m/s, full at 2.5 m/s.
					float r = CLAMP((speed - 0.5f) / 2.0f, 0.0f, 1.0f);
					src = MAX(src, 0.75f * r * r * (3.0f - 2.0f * r));
				}
			}
			float up = 0.0f;
			if (i > 0 && inv_k[k] > 0.0f) {
				// Where this sample's water was one step upstream.
				const float along = MAX(sec.speed_at[j], 0.05f);
				const float shift = CLAMP(sec.across_at[j] / along * p_ds / dn, -1.0f, 1.0f);
				const float u = CLAMP((float)j - shift, 0.0f, (float)m);
				const int j0 = MIN((int)u, m - 1);
				up = Math::lerp(carried[j0], carried[j0 + 1], u - j0) * decay;
			}
			sec.foam_at[j] = inv_k[k] > 0.0f ? MAX(src, up) : 0.0f;
		}
		for (int j = 0; j <= m; j++) {
			carried[j] = sec.foam_at[j];
		}
	}
}

// Standing waves (docs/ideas/water.md "River standing waves"), baked from the solved profile.
void PhysXRiver3D::_solve_standing_waves() {
	const int count = (int)sections.size();
	const int m = cross_segments;
	LocalVector<float> mean_depth;
	mean_depth.resize(count);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.wave_at.resize(m + 1);
		for (int j = 0; j <= m; j++) {
			sec.wave_at[j] = 0.0f;
		}
		sec.wave_amp = 0.0f;
		sec.wave_k = 0.0f;
		sec.wave_s0 = 0.0f;
		sec.wave_decay = 1.0f;
		float area = 0.0f;
		float wet = 0.0f;
		const float dn = (sec.width + 2.0f * bank_margin) / m;
		for (int j = 0; j <= m; j++) {
			if (sec.depth[j] > 0.0f) {
				area += sec.depth[j] * dn;
				wet += dn;
			}
		}
		mean_depth[i] = wet > 1e-4f ? area / wet : 0.0f;
	}
	if (!standing_waves || !gradually_varied_flow || count < 3) {
		return;
	}
	// Undular jumps: the stationary train below each toe.
	for (int i = 1; i < count; i++) {
		const float fr1 = sections[i].jump;
		if (fr1 <= 1.0f) {
			continue;
		}
		const int down = MIN(i + 2, count - 1);
		const float y1 = mean_depth[i - 1];
		const float y2 = mean_depth[down];
		const float u2 = sections[down].speed;
		if (y2 < 0.02f || y2 <= y1) {
			continue;
		}
		const float fr2 = u2 / Math::sqrt(GRAVITY * y2);
		if (fr2 >= 0.98f) {
			continue;
		}
		// Fr2^2 x = tanh(x), x = k y2 (stationary: the phase speed equals the current).
		const float f2 = fr2 * fr2;
		float lo = 1e-3f;
		float hi = 1.0f / MAX(f2, 1e-3f) + 1.0f;
		for (int it = 0; it < 40; it++) {
			const float x = 0.5f * (lo + hi);
			if (f2 * x < Math::tanh(x)) {
				lo = x;
			} else {
				hi = x;
			}
		}
		const float k = MAX(0.5f * (lo + hi), 0.15f) / y2;
		const float wavelength = (float)Math::TAU / k;
		// Undular below Fr1 1.7 (Chanson and Montes 1995); above it the roller takes most of the energy.
		const float share = fr1 <= 1.7f ? 1.0f : 0.33f;
		const float amp = MIN(0.4f * (y2 - y1), 0.3f * y2) * share;
		const float decay = (fr1 <= 1.7f ? 4.0f : 2.0f) * wavelength;
		const float s0 = sections[i].s;
		for (int q = i; q < count; q++) {
			Section &sec = sections[q];
			const float x = sec.s - s0;
			if (x > 5.0f * decay || (q > i && (sec.jump > 0.0f || sec.cascade || sec.fall_drop))) {
				break;
			}
			if (amp * Math::exp(-x / decay) > sec.wave_amp * Math::exp(-(sec.s - sec.wave_s0) / sec.wave_decay)) {
				sec.wave_amp = amp;
				sec.wave_k = k;
				sec.wave_s0 = s0;
				sec.wave_decay = decay;
			}
		}
	}
	// Cross-waves in supercritical flow (Ippen 1951): each wall's angle into the flow, carried along the
	// characteristics at the Mach angle, directly and after up to three reflections.
	LocalVector<float> theta;
	theta.resize(count);
	for (int i = 0; i < count; i++) {
		const int a = MAX(i - 2, 0);
		const int b = MIN(i + 2, count - 1);
		const float ds = sections[b].s - sections[a].s;
		// Each wall moves in by half the narrowing: positive turns into the flow.
		theta[i] = ds > 1e-4f ? -0.5f * (sections[b].width - sections[a].width) / ds : 0.0f;
		if (sections[i].cascade || sections[i].fall_drop || sections[i].froude < 1.0f) {
			theta[i] = 0.0f;
		}
	}
	auto theta_at = [&](int p_from, float p_s) -> float {
		// Walk upstream from p_from to arc length p_s; the characteristic stops at subcritical water or a step.
		int q = p_from;
		while (q > 0 && sections[q].s > p_s) {
			if (sections[q].froude < 1.0f || sections[q].cascade || sections[q].fall_drop || sections[q].jump > 0.0f) {
				return 0.0f;
			}
			q--;
		}
		if (sections[q].s > p_s) {
			return 0.0f;
		}
		if (q + 1 >= count) {
			return theta[q]; // at the last section (no travel to the wall): its own angle
		}
		const float span = sections[q + 1].s - sections[q].s;
		const float t = span > 1e-5f ? (p_s - sections[q].s) / span : 0.0f;
		return Math::lerp(theta[q], theta[q + 1], CLAMP(t, 0.0f, 1.0f));
	};
	for (int i = 1; i < count; i++) {
		Section &sec = sections[i];
		const float fr = sec.froude;
		if (fr < 1.05f || sec.cascade || sec.fall_drop || mean_depth[i] < 0.02f) {
			continue;
		}
		const float f2 = fr * fr;
		// Linear gain, eased in from Fr 1.05 to 1.25 where linear theory breaks down near critical.
		const float gain = mean_depth[i] * f2 / Math::sqrt(f2 - 1.0f) * CLAMP((fr - 1.05f) / 0.2f, 0.0f, 1.0f);
		const float tan_b = 1.0f / Math::sqrt(f2 - 1.0f); // tan(asin(1/Fr))
		const float w = sec.width;
		const float half = 0.5f * w + bank_margin;
		float sum = 0.0f;
		int wet = 0;
		for (int j = 0; j <= m; j++) {
			const float n = -half + 2.0f * half * j / m;
			if (sec.depth[j] <= 0.0f) {
				continue;
			}
			const float d_left = CLAMP(0.5f * w - n, 0.0f, w);
			const float d_right = w - d_left;
			const float travel[2][4] = { { d_left, 2.0f * w - d_left, 2.0f * w + d_left, 4.0f * w - d_left }, { d_right, 2.0f * w - d_right, 2.0f * w + d_right, 4.0f * w - d_right } };
			float h = 0.0f;
			for (int side = 0; side < 2; side++) {
				float loss = 1.0f;
				for (int r = 0; r < 4; r++) {
					h += loss * theta_at(i, sec.s - travel[side][r] / tan_b);
					loss *= 0.8f;
				}
			}
			h = CLAMP(gain * h, -0.4f * mean_depth[i], 0.4f * mean_depth[i]);
			sec.wave_at[j] = h;
			sum += h;
			wet++;
		}
		// Keep the solved 1D profile: only the pattern across.
		const float mean = wet > 0 ? sum / wet : 0.0f;
		for (int j = 0; j <= m; j++) {
			if (sec.depth[j] > 0.0f) {
				sec.wave_at[j] -= mean;
			}
		}
	}
}

void PhysXRiver3D::_build_mesh() {
	if (mesh_instance == nullptr) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance, false, INTERNAL_MODE_BACK);
	}
	if (water_material.is_null()) {
		Ref<Shader> shader = water_shared_shader(river_shader_src());
		water_material.instantiate();
		water_material->set_shader(shader);
		_live_material();
	}
	mesh_instance->set_material_override(water_material);
	const int count = (int)sections.size();
	if (count < 2) {
		mesh_instance->set_mesh(Ref<Mesh>());
		_build_fall_mesh();
		return;
	}
	const int m = cross_segments;
	const int row = m + 1;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	verts.resize(count * row);
	normals.resize(count * row);
	uvs.resize(count * row);
	uv2s.resize(count * row);
	colors.resize(count * row);
	PackedFloat32Array custom0;
	custom0.resize(count * row * 4);
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		const Vector3 tangent(-sec.side.z, 0.0f, sec.side.x);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			const float n = -half + 2.0f * half * j / m;
			const float wave = j < (int)sec.wave_at.size() ? sec.wave_at[j] : 0.0f;
			verts.set(k, sec.centre + sec.side * n + Vector3(0.0f, sec.tilt * n + wave, 0.0f));
			custom0.set(k * 4 + 0, sec.wave_amp);
			custom0.set(k * 4 + 1, sec.wave_k);
			// The crests bow downstream mid-channel (slower water by the banks holds them back): a quarter
			// wavelength at the banks, as in photographs of undular jumps (Chanson 2009).
			const float r = CLAMP(n / MAX(0.5f * sec.width, 0.1f), -1.0f, 1.0f);
			custom0.set(k * 4 + 2, sec.wave_s0 - (sec.wave_k > 0.0f ? 0.25f * (float)Math::TAU / sec.wave_k * r * r : 0.0f));
			custom0.set(k * 4 + 3, sec.wave_decay);
			uvs.set(k, Vector2(sec.s, n));
			const Vector3 v = tangent * sec.speed_at[j] + sec.side * sec.across_at[j];
			uv2s.set(k, Vector2(v.x, v.z));
			const float foam = sec.foam_at[j] * sec.mouth_fade;
			// Where the water still stands at the ribbon's outer sample (spread over flat ground wider than the
			// channel, as onto a beach), the ribbon would cut it off along a straight line: fade it out over the
			// outer third instead, and the shader breaks that fade up with the bank's relief.
			const bool spills = sec.depth[n < 0.0f ? 0 : m] > 0.0f;
			const float fade_from = spills ? 0.65f * half : 0.5f * sec.width;
			const float width_fade = (1.0f - CLAMP((Math::abs(n) - fade_from) / MAX(half - fade_from, 1e-3f), 0.0f, 1.0f)) * sec.mouth_fade;
			// Alpha: the Froude number over 4 (the Froude overlay), up to 0.98; 1 marks a jump's toe.
			colors.set(k, Color(CLAMP((sec.depth[j] + 1.0f) * 0.25f, 0.0f, 1.0f), foam, width_fade, sec.jump > 0.0f ? 1.0f : CLAMP(sec.froude * 0.25f, 0.0f, 0.98f)));
		}
	}
	for (int i = 0; i < count; i++) {
		for (int j = 0; j <= m; j++) {
			const Vector3 a = verts[i * row + MIN(j + 1, m)] - verts[i * row + MAX(j - 1, 0)];
			const Vector3 b = verts[MIN(i + 1, count - 1) * row + j] - verts[MAX(i - 1, 0) * row + j];
			Vector3 nrm = b.cross(a);
			if (nrm.y < 0.0f) {
				nrm = -nrm;
			}
			normals.set(i * row + j, nrm.length_squared() > 1e-12f ? nrm.normalized() : Vector3(0, 1, 0));
		}
	}
	for (int i = 0; i + 1 < count; i++) {
		for (int j = 0; j < m; j++) {
			const int k00 = i * row + j;
			const int k01 = k00 + 1;
			const int k10 = k00 + row;
			const int k11 = k10 + 1;
			// Skip quads that are dry all round (ground well above the water).
			const float dmax = MAX(MAX(sections[i].depth[j], sections[i].depth[j + 1]), MAX(sections[i + 1].depth[j], sections[i + 1].depth[j + 1]));
			if (dmax < -0.05f || MAX(sections[i].mouth_fade, sections[i + 1].mouth_fade) <= 0.0f || sections[i].fall_drop) {
				continue;
			}
			// Clockwise seen from above: the top is the front face, which the path
			// tracer takes as entering the water (the other way round, every ray
			// from above counted as leaving it and grazing ones were totally reflected).
			indices.push_back(k00);
			indices.push_back(k01);
			indices.push_back(k10);
			indices.push_back(k01);
			indices.push_back(k11);
			indices.push_back(k10);
		}
	}
	_build_fall_mesh();
	if (indices.is_empty()) {
		mesh_instance->set_mesh(Ref<Mesh>());
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_CUSTOM0] = custom0;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT);
	mesh_instance->set_mesh(mesh);
	if (water_material.is_valid()) {
		water_material->set_shader_parameter(SNAME("section_step"), count > 1 ? (sections[count - 1].s - sections[0].s) / (count - 1) : step_length);
	}
}

// Waterfalls (docs/ideas/water.md "Step 4f"). A run of cascade sections that
// drops at least fall_min_height, where a free jet from the lip stays clear
// of the ground, is a free overfall: the surface draws down to the brink depth
// 0.715 y_c (Rouse 1936) over about 4 y_c, the water leaves the lip at
// v0 = q / y_b and falls as a projectile, the ribbon under the jet is hidden
// and the water where it lands boils white.
void PhysXRiver3D::_find_falls(PhysicsDirectSpaceState3D *p_space, const LocalVector<float> &p_bed, float p_ds) {
	falls.clear();
	const int count = (int)sections.size();
	for (Section &sec : sections) {
		sec.fall_drop = false;
		sec.plunge = 0.0f;
	}
	// Faces another node draws (PhysXWaterfalls3D's contact falls, as lip and foot pairs): the ribbon down them is
	// hidden under that sheet, and no fall is made there.
	if (fall_exclusions.size() >= 2) {
		const Transform3D to_local = (is_inside_tree() ? get_global_transform() : get_transform()).affine_inverse();
		for (int e = 0; e + 1 < fall_exclusions.size(); e += 2) {
			const Vector3 lip = to_local.xform(fall_exclusions[e]);
			const Vector3 foot = to_local.xform(fall_exclusions[e + 1]);
			const Vector2 l2(lip.x, lip.z);
			const Vector2 f2(foot.x, foot.z);
			int first = -1;
			for (int k = 0; k + 1 < count; k++) {
				const Vector3 c = sections[k].centre.lerp(sections[k + 1].centre, 0.5f);
				const Vector2 c2(c.x, c.z);
				const Vector2 seg = f2 - l2;
				const float t = seg.length_squared() > 1e-6f ? CLAMP((c2 - l2).dot(seg) / seg.length_squared(), 0.0f, 1.0f) : 0.0f;
				const float reach = MAX(6.0f, sections[k].width);
				if (c2.distance_to(l2 + seg * t) < reach && sections[k].surface < lip.y + 1.0f && sections[k + 1].surface > foot.y - 1.0f) {
					sections[k].fall_drop = true;
					if (first < 0) {
						first = k;
					}
				}
			}
			// The water draws down to the brink depth before the lip (Rouse 1936), as at the river's own falls:
			// standing at its full depth up to the hidden stretch, the ribbon ended in a block of water hanging
			// over the face's first row. Over about 4 y_c, and at least the last section before the lip.
			if (first > 0) {
				const Section &sl = sections[first];
				const float q = sl.discharge / MAX(sl.width, 0.5f);
				const float yc = Math::pow(q * q / GRAVITY, 1.0f / 3.0f);
				const float brink = MAX(lip.y, p_bed[first]) + MAX(0.715f * yc, 0.01f);
				const float span = MAX(4.0f * yc, 1.5f * p_ds);
				for (int k = first - 1; k >= 0; k--) {
					Section &sec = sections[k];
					const float f = 1.0f - (sl.s - sec.s) / span;
					if (f <= 0.0f) {
						break;
					}
					const float y = Math::lerp(sec.surface, MAX(brink, p_bed[k] + 0.02f), f * f);
					sec.surface = MIN(sec.surface, y);
					sec.centre.y = sec.surface;
				}
			}
		}
	}
	if (fall_min_height <= 0.0f || count < 3) {
		return;
	}
	auto bed_at_s = [&](float p_s) -> float {
		const float f = CLAMP(p_s / p_ds, 0.0f, (float)(count - 1));
		const int i = MIN((int)f, count - 2);
		return Math::lerp(p_bed[i], p_bed[i + 1], f - i);
	};
	int i = 1;
	while (i < count) {
		if (!sections[i].cascade) {
			i++;
			continue;
		}
		const int a = i - 1;
		int b = i;
		while (b + 1 < count && sections[b + 1].cascade) {
			b++;
		}
		i = b + 1;
		const Section &sa = sections[a];
		const float height = sa.surface - sections[b].surface;
		if (height < fall_min_height) {
			continue;
		}
		Fall fall;
		fall.brink = a;
		fall.foot = b;
		fall.side = sa.side;
		fall.tangent = Vector3(-sa.side.z, 0.0f, sa.side.x);
		fall.width = sa.width;
		fall.q = sa.discharge / MAX(sa.width, 0.5f);
		const float yc = Math::pow(fall.q * fall.q / GRAVITY, 1.0f / 3.0f);
		const float yb = MAX(0.715f * yc, 0.01f);
		fall.lip_speed = MIN(fall.q / yb, MAX_SPEED);
		// The lip: where the ground between the last high section and the next
		// one drops away (bisected), or halfway with no ground to measure.
		float t_lip = 0.5f;
		if (p_space != nullptr) {
			const Vector3 from = sa.centre;
			const Vector3 to = sections[a + 1].centre;
			const float edge = p_bed[a] - 0.5f * height;
			float lo = 0.0f;
			float hi = 1.0f;
			for (int it = 0; it < 10; it++) {
				const float mid = 0.5f * (lo + hi);
				float y = 0.0f;
				if (_base_ground_at(p_space, from.lerp(to, mid), y) && y > edge) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			t_lip = lo;
		}
		fall.lip_s = sa.s + t_lip * p_ds;
		fall.lip = sa.centre.lerp(sections[a + 1].centre, t_lip);
		fall.lip.y = p_bed[a];
		fall.pool = sections[b].surface;
		// A fall another node draws (a contact fall on its face) is not drawn again here.
		if (sections[a].fall_drop || sections[b].fall_drop) {
			continue;
		}
		// The free jet must stay clear of the ground (the underside of the sheet
		// against the bed under it); a steep chute it would run down stays a cascade.
		const float drop = fall.lip.y + 0.5f * yb - fall.pool;
		if (drop <= 0.0f) {
			continue;
		}
		const float tau_land = Math::sqrt(2.0f * drop / GRAVITY);
		bool clear = true;
		for (int k = 1; k <= 16 && clear; k++) {
			const float tau = tau_land * k / 16.0f;
			const float s = fall.lip_s + fall.lip_speed * tau;
			const float under = fall.lip.y - 0.5f * GRAVITY * tau * tau;
			if (s < sections[count - 1].s && under < bed_at_s(s) - 0.05f && under > fall.pool + 0.1f) {
				clear = false;
			}
		}
		if (!clear) {
			continue;
		}
		fall.height = sa.surface - fall.pool;
		fall.land_s = fall.lip_s + fall.lip_speed * tau_land;
		fall.impact_speed = Math::sqrt(fall.lip_speed * fall.lip_speed + 2.0f * GRAVITY * drop);
		// Break-up length of a nappe, L_b = 6 q^0.32 (Horeni 1956).
		fall.breakup = 6.0f * Math::pow(MAX(fall.q, 1e-3f), 0.32f);
		// Draw-down to the brink over about 4 y_c upstream of the lip.
		const float brink = fall.lip.y + yb;
		for (int k = a; k >= 0; k--) {
			Section &sec = sections[k];
			const float f = 1.0f - (fall.lip_s - sec.s) / MAX(4.0f * yc, 1e-3f);
			if (f <= 0.0f) {
				break;
			}
			const float y = Math::lerp(sec.surface, MAX(brink, p_bed[k] + 0.02f), f * f);
			sec.surface = MIN(sec.surface, y);
			sec.centre.y = sec.surface;
		}
		// Under the jet: hide the ribbon draped down the cliff.
		for (int k = a; k < b; k++) {
			sections[k].fall_drop = true;
		}
		// Where it lands, and the pool round it, boil white: from the foot of the
		// cliff to a few metres past the landing.
		const float pool_end = fall.land_s + MAX(2.0f, 0.3f * fall.height);
		for (int k = b; k < count; k++) {
			Section &sec = sections[k];
			if (sec.s > pool_end + 2.0f) {
				break;
			}
			const float f = CLAMP((pool_end + 2.0f - sec.s) / 2.0f, 0.0f, 1.0f);
			// Boiling, not a solid sheet: the lace still shows through.
			sec.plunge = MAX(sec.plunge, 0.8f * f);
			sec.cascade = false;
		}
		falls.push_back(fall);
	}
}

// The falling sheet of each waterfall: one column per wet cross sample at the
// brink, each following the projectile from its own unit discharge; a slab
// with front and back faces q/|v| apart (aerated water bulks up to three
// times). Rows are spaced evenly in travel time.
void PhysXRiver3D::_build_fall_mesh() {
	if (falls.is_empty() || sections.size() < 2) {
		if (fall_instance != nullptr) {
			fall_instance->set_mesh(Ref<Mesh>());
		}
		return;
	}
	if (fall_instance == nullptr) {
		fall_instance = memnew(MeshInstance3D);
		fall_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(fall_instance, false, INTERNAL_MODE_BACK);
	}
	if (fall_material.is_null()) {
		Ref<Shader> shader = water_shared_shader(FALL_SHADER_SRC);
		fall_material.instantiate();
		fall_material->set_shader(shader);
	}
	fall_instance->set_material_override(fall_material);
	const int m = cross_segments;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	for (const Fall &fall : falls) {
		const Section &sa = sections[fall.brink];
		const float half = 0.5f * sa.width + bank_margin;
		// Wet span at the brink.
		int j0 = -1;
		int j1 = -1;
		for (int j = 0; j <= m; j++) {
			if (sa.depth[j] > 0.02f) {
				j0 = j0 < 0 ? j : j0;
				j1 = j;
			}
		}
		if (j0 < 0 || j1 <= j0) {
			continue;
		}
		const int cols = j1 - j0 + 1;
		const int rows = CLAMP((int)Math::ceil(fall.height * 3.0f), 8, 64);
		const int per_face = cols * (rows + 2);
		for (int face = 0; face < 2; face++) {
			const float sign = face == 0 ? 1.0f : -1.0f;
			const int base = verts.size();
			for (int jj = 0; jj < cols; jj++) {
				const int j = j0 + jj;
				const float n = -half + 2.0f * half * j / m;
				const float q = MAX(sa.speed_at[j] * sa.depth[j], 0.0f);
				const float yc = Math::pow(q * q / GRAVITY, 1.0f / 3.0f);
				const float yb = MAX(0.715f * yc, 0.005f);
				const float v0 = CLAMP(q / yb, 0.3f, MAX_SPEED);
				const float breakup = 6.0f * Math::pow(MAX(q, 1e-3f), 0.32f);
				const float edge = MIN(1.0f, MIN((float)jj, (float)(cols - 1 - jj)) / 2.0f) * CLAMP(q / MAX(fall.q, 1e-3f), 0.0f, 1.0f);
				const Vector3 lip = fall.lip + fall.side * n;
				const float mid_y = fall.lip.y + 0.5f * yb;
				const float tau_land = Math::sqrt(2.0f * MAX(mid_y - fall.pool, 0.01f) / GRAVITY) + 0.05f;
				// Row 0: the brink section's surface, joining the ribbon.
				{
					const Vector3 p = sa.centre + sa.side * n;
					verts.push_back(face == 0 ? p : Vector3(p.x, fall.lip.y - 0.05f, p.z));
					normals.push_back(Vector3(0, sign, 0));
					uvs.push_back(Vector2(-(fall.lip_s - sa.s) / MAX(v0, 0.3f), n));
					uv2s.push_back(Vector2(v0, 0.0f));
					colors.push_back(Color(edge, 0.0f, 0.0f, 1.0f));
				}
				for (int k = 0; k <= rows; k++) {
					const float tau = tau_land * k / rows;
					const float z = 0.5f * GRAVITY * tau * tau; // fallen so far
					const Vector3 vel = fall.tangent * v0 - Vector3(0, GRAVITY * tau, 0);
					const float speed = vel.length();
					const float aeration = CLAMP(0.15f + 0.85f * z / breakup, 0.0f, 1.0f);
					const float broken = CLAMP((z - breakup) / (0.5f * breakup), 0.0f, 1.0f);
					const float thick = q / MAX(speed, 0.1f) * (1.0f + 2.0f * aeration);
					// Outward: away from the cliff and up at the lip.
					Vector3 out = fall.side.cross(vel).normalized();
					if (out.dot(fall.tangent) < -1e-4f || (Math::abs(out.dot(fall.tangent)) <= 1e-4f && out.y < 0.0f)) {
						out = -out;
					}
					// The sheet spreads sideways by a few per cent of the fall (Ervine and Falvey 1987).
					const Vector3 spread = fall.side * (n * 0.04f * z / MAX(half, 0.5f));
					const Vector3 centre = Vector3(lip.x, mid_y, lip.z) + fall.tangent * (v0 * tau) - Vector3(0, z, 0) + spread;
					verts.push_back(centre + out * (0.5f * thick * sign));
					normals.push_back(out * sign);
					uvs.push_back(Vector2(tau, n));
					uv2s.push_back(Vector2(speed, aeration));
					colors.push_back(Color(edge, aeration, broken, 1.0f));
				}
			}
			const int rr = rows + 2;
			for (int jj = 0; jj + 1 < cols; jj++) {
				for (int k = 0; k + 1 < rr; k++) {
					const int k00 = base + jj * rr + k;
					const int k01 = k00 + 1;
					const int k10 = k00 + rr;
					const int k11 = k10 + 1;
					// Front faces wound so the outward side is the front (as the ribbon:
					// the path tracer takes the front as entering the water).
					const Vector3 e1 = verts[k10] - verts[k00];
					const Vector3 e2 = verts[k01] - verts[k00];
					const bool flip = e1.cross(e2).dot(normals[k00]) > 0.0f;
					if (!flip) {
						indices.push_back(k00);
						indices.push_back(k10);
						indices.push_back(k01);
						indices.push_back(k10);
						indices.push_back(k11);
						indices.push_back(k01);
					} else {
						indices.push_back(k00);
						indices.push_back(k01);
						indices.push_back(k10);
						indices.push_back(k10);
						indices.push_back(k01);
						indices.push_back(k11);
					}
				}
			}
			(void)per_face;
		}
	}
	if (indices.is_empty()) {
		fall_instance->set_mesh(Ref<Mesh>());
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	fall_instance->set_mesh(mesh);
}

// Spray and mist where each jet lands, scaled with the fall's power
// rho g Q H. Spray: droplets thrown up from the impact line at a share of the
// impact speed (GPUParticles3D, small spheres, lit, so they show under path
// tracing). Mist: the finest drops, which fall at a few cm/s and drift out on
// the air the fall drags down -- a PhysXWaterMist3D grid per fall, or a cool
// PhysXFlowEmitter3D feeding the scene's PhysXFlow3D when it has one.
void PhysXRiver3D::_build_fall_effects() {
	for (Node3D *n : fall_effects) {
		if (n != nullptr) {
			remove_child(n);
			memdelete(n);
		}
	}
	fall_effects.clear();
	if (Engine::get_singleton()->is_editor_hint() || (!fall_spray && !fall_mist && !fall_sound)) {
		return;
	}
	const Ref<Curve3D> c = get_curve();
	for (const Fall &fall : falls) {
		const float power = 1000.0f * GRAVITY * fall.q * fall.width * fall.height; // W
		Vector3 land = fall.lip + fall.tangent * (fall.land_s - fall.lip_s);
		land.y = fall.pool;
		Transform3D xf;
		xf.basis = Basis(fall.side, Vector3(0, 1, 0), -fall.tangent).orthonormalized();
		xf.origin = land;
		if (fall_spray) {
			GPUParticles3D *spray = memnew(GPUParticles3D);
			spray->set_name("FallSpray");
			spray->set_transform(xf);
			spray->set_amount(CLAMP((int)(power / 200.0f), 64, 4096));
			const float rise = 0.35f * fall.impact_speed;
			spray->set_lifetime(MAX(0.6f, 2.0f * rise / GRAVITY));
			spray->set_pre_process_time(2.0);
			spray->set_visibility_aabb(AABB(Vector3(-fall.width - 4.0f, -1.0f, -6.0f), Vector3(2.0f * fall.width + 8.0f, rise * rise / GRAVITY + 3.0f, 12.0f)));
			Ref<ParticleProcessMaterial> pm;
			pm.instantiate();
			pm->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX);
			pm->set_emission_box_extents(Vector3(0.5f * fall.width, 0.1f, 0.3f));
			pm->set_direction(Vector3(0, 1, 0.35f));
			pm->set_spread(40.0f);
			pm->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.4f * rise);
			pm->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, rise);
			pm->set_gravity(Vector3(0, -GRAVITY, 0));
			pm->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.5f);
			pm->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 1.5f);
			pm->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.04f);
			pm->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 0.12f);
			spray->set_process_material(pm);
			Ref<SphereMesh> drop;
			drop.instantiate();
			drop->set_radius(0.5f);
			drop->set_height(1.0f);
			drop->set_radial_segments(6);
			drop->set_rings(3);
			Ref<StandardMaterial3D> dm;
			dm.instantiate();
			dm->set_albedo(Color(0.95f, 0.97f, 1.0f));
			dm->set_roughness(0.25f);
			drop->set_material(dm);
			spray->set_draw_pass_mesh(0, drop);
			add_child(spray, false, INTERNAL_MODE_BACK);
			fall_effects.push_back(spray);
		}
		if (fall_mist) {
			bool to_flow = false;
#ifdef GODOT_PHYSX_FLOW
			// A scene with a PhysXFlow3D simulates the mist in its own gas grid (a full fluid solve).
			to_flow = is_inside_tree() && !get_tree()->get_root()->find_children("*", "PhysXFlow3D", true, false).is_empty();
			if (to_flow) {
				PhysXFlowEmitter3D *mist = memnew(PhysXFlowEmitter3D);
				mist->set_name("FallMist");
				Transform3D mxf = xf;
				mxf.origin += Vector3(0, 0.6f, 0);
				mist->set_transform(mxf);
				mist->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
				mist->set_size(Vector3(fall.width + 1.0f, 1.2f, 2.0f));
				// The fall's wind: out from the foot at a fraction of the impact speed.
				mist->set_velocity(Vector3(0, 0.4f, -0.15f * fall.impact_speed));
				mist->set_temperature(0.0f);
				mist->set_smoke(CLAMP(power / 100000.0f, 0.2f, 2.0f));
				add_child(mist, false, INTERNAL_MODE_BACK);
				fall_effects.push_back(mist);
			}
#endif
			if (!to_flow) {
				// Otherwise its own mist grid (PhysXWaterMist3D): fed with a share of the fall's water that
				// grows with the square of the impact speed (atomisation by the jet), blown out along the
				// pool by the air the fall drags down (about 0.3 of the impact speed).
				const float reach = CLAMP(4.0f + 0.6f * fall.height, 6.0f, 24.0f);
				const float side = MAX(fall.width + 2.0f * reach, 2.0f * reach);
				const float shift = MAX(reach - 3.0f, 0.0f);
				PhysXWaterMist3D *mist = memnew(PhysXWaterMist3D);
				mist->set_name("FallMist");
				Transform3D mxf = xf;
				mxf.origin += fall.tangent * shift;
				mist->set_transform(mxf);
				mist->set_size(Vector3(side, CLAMP(fall.height + 4.0f, 6.0f, 30.0f), side));
				mist->set_cell_size(0.5f);
				mist->set_source_position(Vector3(0.0f, 0.5f, shift));
				mist->set_source_extents(Vector3(0.5f * fall.width + 0.5f, 0.75f, 1.0f));
				mist->set_jet_direction(Vector3(0.0f, 0.0f, -1.0f));
				mist->set_jet_speed(MIN(0.3f * fall.impact_speed, 12.0f));
				const float q_total = fall.q * fall.width; // m^3/s
				const float v = fall.impact_speed / 10.0f;
				mist->set_source_rate(fall_mist_amount * 1e-5f * v * v * 1000.0f * q_total * 1000.0f); // g/s
				add_child(mist, false, INTERNAL_MODE_BACK);
				fall_effects.push_back(mist);
			}
		}
		if (fall_sound) {
			// The roar is the ringing of the bubbles the plunge drags under (Minnaert resonators): synthesised once per
			// fall as a seamless loop (WaterFallSound), at a level from the fall's acoustic power, about 3e-7 of the
			// jet's kinetic power. A free field over the pool spreads it on a half sphere: the level at unit_size
			// (10 m) is 10 log10(W / (2 pi 100) / 1e-12) dB SPL, mapped so 80 dB SPL plays at 0 dB.
			const float q_total = fall.q * fall.width;
			const uint32_t seed = uint32_t(Math::abs(fall.lip.x * 73.0f + fall.lip.z * 151.0f)) + uint32_t(fall_effects.size());
			AudioStreamPlayer3D *snd = memnew(AudioStreamPlayer3D);
			snd->set_name("FallSound");
			Transform3D sxf = xf;
			sxf.origin += Vector3(0, 0.5f * MIN(fall.height, 4.0f), 0);
			snd->set_transform(sxf);
			snd->set_stream(WaterFallSound::make(fall.impact_speed, q_total, seed));
			const float w = WaterFallSound::sound_power(fall.impact_speed, q_total);
			const float spl = 10.0f * Math::log(MAX(w, 1e-12f) / (Math::TAU * 100.0f) / 1e-12f) / Math::log(10.0f);
			snd->set_unit_size(10.0f);
			snd->set_volume_db(CLAMP(spl - 80.0f + fall_sound_volume_db, -60.0f, 12.0f));
			snd->set_max_db(12.0f);
			snd->set_autoplay(true);
			add_child(snd, false, INTERNAL_MODE_BACK);
			fall_effects.push_back(snd);
		}
	}
	(void)c;
}

void PhysXRiver3D::set_fall_min_height(float p_height) {
	fall_min_height = MAX(p_height, 0.0f);
	_queue_build();
}

void PhysXRiver3D::set_fall_exclusions(const PackedVector3Array &p_points) {
	fall_exclusions = p_points;
	_queue_build();
}

void PhysXRiver3D::set_fall_spray(bool p_enabled) {
	fall_spray = p_enabled;
	_queue_build();
}

void PhysXRiver3D::set_fall_mist(bool p_enabled) {
	fall_mist = p_enabled;
	_queue_build();
}

void PhysXRiver3D::set_fall_sound(bool p_enabled) {
	fall_sound = p_enabled;
	_queue_build();
}

void PhysXRiver3D::set_fall_sound_volume_db(float p_db) {
	fall_sound_volume_db = p_db;
	_queue_build();
}

void PhysXRiver3D::set_fall_mist_amount(float p_amount) {
	fall_mist_amount = MAX(p_amount, 0.0f);
	_queue_build();
}

void PhysXRiver3D::set_fall_material(const Ref<ShaderMaterial> &p_material) {
	fall_material = p_material;
	if (fall_instance != nullptr) {
		fall_instance->set_material_override(fall_material);
	}
}

Array PhysXRiver3D::get_waterfalls() const {
	Array out;
	const Transform3D xf = is_inside_tree() ? get_global_transform() : get_transform();
	for (const Fall &fall : falls) {
		Vector3 land = fall.lip + fall.tangent * (fall.land_s - fall.lip_s);
		land.y = fall.pool;
		Dictionary d;
		d["lip"] = xf.xform(fall.lip);
		d["impact"] = xf.xform(land);
		d["height"] = fall.height;
		d["discharge"] = fall.q * fall.width;
		d["width"] = fall.width;
		d["lip_speed"] = fall.lip_speed;
		d["impact_speed"] = fall.impact_speed;
		d["breakup_length"] = fall.breakup;
		d["throw"] = fall.land_s - fall.lip_s;
		out.push_back(d);
	}
	return out;
}

bool PhysXRiver3D::_locate(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const {
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (count < 2 || c.is_null() || !is_inside_tree() || !p_world.is_finite()) {
		return false;
	}
	Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	const float ds = sections[1].s - sections[0].s;
	// The nearest point on the river's centre line, in plan.
	const Vector3 flat_query(local.x, 0.0f, local.z);
	float s = c->get_closest_offset(local);
	const float x = s / ds;
	int i = CLAMP((int)Math::floor(x), 0, count - 2);
	float t = CLAMP(x - i, 0.0f, 1.0f);
	const Section &a = sections[i];
	const Section &b = sections[i + 1];
	const Vector3 centre = a.centre.lerp(b.centre, t);
	const Vector3 side = a.side.lerp(b.side, t).normalized();
	const Vector3 rel = flat_query - Vector3(centre.x, 0.0f, centre.z);
	const Vector3 tangent(-side.z, 0.0f, side.x);
	const float along = rel.dot(tangent);
	// Past either end of the river.
	if ((i == 0 && t <= 0.0f && along < -0.5f * ds) || (i == count - 2 && t >= 1.0f && along > 0.5f * ds)) {
		return false;
	}
	const float n = rel.dot(side);
	const float half = 0.5f * Math::lerp(a.width, b.width, t) + bank_margin;
	if (Math::abs(n) > half) {
		return false;
	}
	r_section = i;
	r_t = t;
	r_n = n;
	return true;
}

float PhysXRiver3D::sample_depth(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return 0.0f;
	}
	return Math::lerp(cross_value(sections[i], sections[i].depth, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].depth, n, bank_margin, cross_segments), t);
}

float PhysXRiver3D::sample_height(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return -Math::INF;
	}
	const float d = Math::lerp(cross_value(sections[i], sections[i].depth, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].depth, n, bank_margin, cross_segments), t);
	if (d <= 0.0f) {
		return -Math::INF;
	}
	float local_y = Math::lerp(sections[i].surface + sections[i].tilt * n, sections[i + 1].surface + sections[i + 1].tilt * n, t);
	if (!sections[i].wave_at.is_empty() && !sections[i + 1].wave_at.is_empty()) {
		local_y += Math::lerp(cross_value(sections[i], sections[i].wave_at, n, bank_margin, cross_segments), cross_value(sections[i + 1], sections[i + 1].wave_at, n, bank_margin, cross_segments), t);
	}
	{
		// The undular train (the shader draws a short one in the slope only, but bodies ride the real waves).
		const Section &a = sections[i];
		const float s = Math::lerp(a.s, sections[i + 1].s, t);
		const float x = s - a.wave_s0;
		if (a.wave_amp > 0.0f && x > 0.0f) {
			local_y += a.wave_amp * Math::sin(a.wave_k * x) * Math::exp(-x / a.wave_decay) * MIN(x * a.wave_k / (float)Math::PI, 1.0f);
		}
	}
	const Transform3D xf = get_global_transform();
	const Vector3 local = xf.affine_inverse().xform(p_world_pos);
	return xf.xform(Vector3(local.x, local_y, local.z)).y + _live_height_at(p_world_pos);
}

Vector3 PhysXRiver3D::sample_flow(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return Vector3();
	}
	const float v = Math::lerp(cross_value(sections[i], sections[i].speed_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].speed_at, n, bank_margin, cross_segments), t);
	const float a = Math::lerp(cross_value(sections[i], sections[i].across_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].across_at, n, bank_margin, cross_segments), t);
	const Vector3 side = sections[i].side.lerp(sections[i + 1].side, t).normalized();
	const Vector3 tangent(-side.z, 0.0f, side.x);
	const Vector2 live = _live_velocity_at(p_world_pos);
	return get_global_transform().basis.xform(tangent * v + side * a) + Vector3(live.x, 0.0f, live.y);
}

float PhysXRiver3D::sample_foam(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return 0.0f;
	}
	return Math::lerp(cross_value(sections[i], sections[i].foam_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].foam_at, n, bank_margin, cross_segments), t);
}

float PhysXRiver3D::get_length() const {
	return sections.is_empty() ? 0.0f : sections[sections.size() - 1].s;
}

Dictionary PhysXRiver3D::get_section(int p_index) const {
	Dictionary d;
	ERR_FAIL_INDEX_V(p_index, (int)sections.size(), d);
	const Section &sec = sections[p_index];
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	d["centre"] = xf.xform(sec.centre);
	d["s"] = sec.s;
	d["width"] = sec.width;
	d["surface"] = xf.xform(sec.centre).y;
	d["slope"] = sec.slope;
	d["speed"] = sec.speed;
	d["foam"] = sec.foam;
	d["cascade"] = sec.cascade;
	d["froude"] = sec.froude;
	d["critical_height"] = Math::is_finite(sec.critical) ? xf.xform(Vector3(sec.centre.x, sec.critical, sec.centre.z)).y : -Math::INF;
	d["jump"] = sec.jump;
	d["wave_amplitude"] = sec.wave_amp;
	d["wave_length"] = sec.wave_k > 0.0f ? (float)Math::TAU / sec.wave_k : 0.0f;
	d["superelevation"] = sec.tilt * (sec.width + 2.0f * bank_margin);
	d["under_fall"] = sec.fall_drop;
	d["plunge"] = sec.plunge;
	PackedFloat32Array depth;
	PackedFloat32Array speed;
	PackedFloat32Array across;
	PackedFloat32Array foam;
	PackedFloat32Array waves;
	for (uint32_t j = 0; j < sec.depth.size(); j++) {
		waves.push_back(j < sec.wave_at.size() ? sec.wave_at[j] : 0.0f);
		depth.push_back(sec.depth[j]);
		speed.push_back(sec.speed_at[j]);
		across.push_back(sec.across_at[j]);
		foam.push_back(sec.foam_at[j]);
	}
	d["depths"] = depth;
	d["speeds"] = speed;
	d["across"] = across;
	d["foams"] = foam;
	d["cross_waves"] = waves;
	return d;
}

Array PhysXRiver3D::get_carve_brushes(float p_tile_size) const {
	Array brushes;
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (!carve || count < 2 || c.is_null() || !is_inside_tree()) {
		return brushes;
	}
	const float tile = MAX(p_tile_size, 1.0f);
	const float cell = carve_cell_size;
	const Transform3D xf = get_global_transform();
	const Transform3D inv = xf.affine_inverse();
	const float ds = sections[1].s - sections[0].s;
	// Tiles the channel and its fade touch.
	HashSet<Vector2i> tiles;
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float reach = 0.5f * sec.width + carve_falloff;
		for (int e = -2; e <= 2; e++) {
			const Vector3 w = xf.xform(sec.centre + sec.side * (reach * e * 0.5f));
			tiles.insert(Vector2i((int)Math::floor(w.x / tile), (int)Math::floor(w.z / tile)));
		}
	}
	const int res = (int)Math::ceil(tile / cell);
	for (const Vector2i &t : tiles) {
		const Vector2 origin(t.x * tile, t.y * tile);
		PackedFloat32Array heights;
		PackedFloat32Array mask;
		heights.resize(res * res);
		mask.resize(res * res);
		float *hw = heights.ptrw();
		float *mw = mask.ptrw();
		float max_mask = 0.0f;
		for (int z = 0; z < res; z++) {
			for (int x = 0; x < res; x++) {
				const int k = z * res + x;
				hw[k] = 0.0f;
				mw[k] = 0.0f;
				const Vector3 world(origin.x + (x + 0.5f) * cell, xf.origin.y, origin.y + (z + 0.5f) * cell);
				Vector3 local = inv.xform(world);
				const float sq = c->get_closest_offset(local) / ds;
				const int i = CLAMP((int)Math::floor(sq), 0, count - 2);
				const float tt = CLAMP(sq - i, 0.0f, 1.0f);
				const Section &a = sections[i];
				const Section &b = sections[i + 1];
				const Vector3 centre = a.centre.lerp(b.centre, tt);
				const Vector3 side = a.side.lerp(b.side, tt).normalized();
				const Vector3 rel(local.x - centre.x, 0.0f, local.z - centre.z);
				const Vector3 tangent(-side.z, 0.0f, side.x);
				const float along = rel.dot(tangent);
				if ((i == 0 && tt <= 0.0f && along < -0.5f * ds) || (i == count - 2 && tt >= 1.0f && along > 0.5f * ds)) {
					continue; // past either end
				}
				const float n = rel.dot(side);
				float ma;
				float mb;
				const float ya = _carve_target(a, n, ma);
				const float yb = _carve_target(b, n, mb);
				const float m_here = Math::lerp(ma, mb, tt);
				if (m_here <= 0.0f) {
					continue;
				}
				local.y = Math::lerp(ya, yb, tt);
				hw[k] = xf.xform(local).y;
				mw[k] = m_here;
				max_mask = MAX(max_mask, m_here);
			}
		}
		if (max_mask <= 0.0f) {
			continue;
		}
		Dictionary brush;
		brush["rect"] = Rect2(origin, Vector2(res * cell, res * cell));
		brush["cell_size"] = cell;
		brush["size"] = Vector2i(res, res);
		brush["heights"] = heights;
		brush["mask"] = mask;
		brush["mode"] = "lower";
		brushes.push_back(brush);
	}
	return brushes;
}

PackedStringArray PhysXRiver3D::get_configuration_warnings() const {
	PackedStringArray warnings = Path3D::get_configuration_warnings();
	if (get_curve().is_null() || get_curve()->get_point_count() < 2) {
		warnings.push_back(RTR("Draw the river's path: it needs at least two curve points, from source to mouth."));
	}
	return warnings;
}

float PhysXRiver3D::contract_water_height_at(const Vector3 &p_position) const {
	return sample_height(p_position);
}

void PhysXRiver3D::contract_water_disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength) {
	if (Math::is_finite(sample_height(p_position))) {
		submit_sphere(int(p_owner & 0x7fffffff), p_position, p_radius, p_strength);
	}
}

void PhysXRiver3D::contract_water_clear(uint64_t p_owner) {
	clear_sphere(int(p_owner & 0x7fffffff));
}

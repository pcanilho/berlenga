/**************************************************************************/
/*  physx_water_surface_3d.cpp                                            */
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

#include "physx_water_surface_3d.h"

#include "flow_poisson.h"
#include "physx_water_wake_3d.h"
#include "water_raster_reflections.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/face3.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/sky.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture_rd.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

namespace {
// A plain spatial Shader (Godot's own shading language, NOT the RD compute
// shaders the rest of this module uses). Displaces the mesh's own VERTEX.y
// by two independently-sampled height fields (ripple + FFT ocean, summed)
// sampled directly from the solver's shared-RenderingDevice textures via
// Texture2DRD; the separate CPU readback cache is only for sample_height().
//
// Real transparency via Godot's own GPU alpha blending (ALPHA, blend_mix),
// not manual screen-space refraction. An earlier version manually sampled
// hint_screen_texture and composited by hand -- confirmed via direct testing
// that this custom engine build's screen-texture capture reads severely
// overbright (even attenuated to 15% strength it still blew the surface to
// solid white; the engine has no REFRACTION built-in either, confirmed
// directly against servers/rendering/shader_types.cpp, so that wasn't an
// option). Real GPU alpha blending sidesteps the broken capture path
// entirely -- the renderer composites against whatever's already correctly
// drawn underneath, no re-sampling needed. Loses the distortion detail a
// real refraction offset would add; not worth it while the capture itself
// is broken.
//
// Real per-vertex normals from a central-difference of the SAME combined
// height field the vertex is displaced by -- the surface used to move but
// never actually shade like it moved, which was a real, separate
// contributor to an earlier "looks flat" complaint. Feeds a small, bounded,
// self-contained shading term in fragment() (unshaded, not Godot's real
// PBR NORMAL) -- see fragment()'s own comment for why real PBR lighting on
// this material caused a genuine overexposure bug.
const char *WATER_SHADER_SRC = R"(
shader_type spatial;
// The same source builds four materials. Plain: the surface. BREAKER_LIP: the plunging lips' sheet, whose
// vertices are already in place (so the path tracer reads its mesh as it is). UNDERWATER_SURFACE: its next pass while
// the camera is under water, the surface seen from below. UNDERWATER_FOG: a full screen pass that fogs
// what lies under the water and splits the image at the water line (see "The camera under water").
#if defined(UNDERWATER_FOG)
render_mode blend_mix, cull_disabled, unshaded, fog_disabled, depth_test_disabled, depth_draw_never, shadows_disabled, rt_vertex_static;
#elif defined(UNDERWATER_SURFACE)
render_mode blend_mix, cull_disabled, unshaded, fog_disabled, depth_draw_never, shadows_disabled;
#elif defined(BREAKER_LIP) && defined(RT)
render_mode cull_disabled, rt_blas_rebuild;
#elif defined(RT)
// The path tracer's copy (the shader is compiled again with RT defined) is a
// real water surface instead: a medium boundary with IOR and absorption, so it
// bends light, tints with depth and casts caustics, lit by the scene's own
// lights. The raster look below stays as it is.
render_mode cull_disabled;
#else
render_mode blend_mix, cull_disabled, unshaded;
#endif

uniform sampler2D ripple_height_tex : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D ocean_height_tex : hint_default_black, filter_linear, repeat_enable;
// Ocean chop multiplier over the ripple domain: 1 in deep water, fading to 0
// at the waterline (set by PhysXWaterSurface3D when it has a seabed).
uniform sampler2D ocean_fade_tex : hint_default_white, filter_linear, repeat_disable;
// Choppy horizontal displacement (x, z) of the ocean layer; scaled by
// choppiness (both set by PhysXWaterSurface3D).
uniform sampler2D ocean_disp_tex : hint_default_black, filter_linear, repeat_enable;
uniform float choppiness = 1.0;
// The foam pass's Jacobian threshold (PhysXWaterSurface3D::foam_threshold),
// for whitecaps where the cascades together fold the surface (very low: off).
uniform float foam_crest_threshold = -100.0;
// Ocean slopes and displacement derivatives (dh/dx, dh/dz, dDx/dx, dDz/dz),
// straight from the FFT, for exact per-pixel normals and foam.
uniform sampler2D ocean_deriv_tex : hint_default_black, filter_linear, repeat_enable;
// true: normals and foam per pixel from ocean_deriv_tex (smooth crests at any
// mesh density). false: per vertex, from finite differences of the displaced
// mesh (set by PhysXWaterSurface3D::normal_mode).
uniform bool per_pixel_normals = true;
uniform vec2 ripple_domain_size = vec2(20.0, 20.0);
uniform vec2 ocean_domain_size = vec2(40.0, 40.0);
// Wave cascades past the first (PhysXWaterSurface3D::wave_cascades): larger
// tiles carrying the longer waves, summed with ocean_*_tex.
uniform int wave_cascade_count = 1;
uniform vec2 cascade_domain_1 = vec2(240.0);
uniform vec2 cascade_domain_2 = vec2(1440.0);
uniform vec2 cascade_domain_3 = vec2(8640.0);
uniform sampler2D cascade_height_1 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_height_2 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_height_3 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_disp_1 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_disp_2 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_disp_3 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_deriv_1 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_deriv_2 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_deriv_3 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_foam_1 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_foam_2 : hint_default_black, filter_linear, repeat_enable;
uniform sampler2D cascade_foam_3 : hint_default_black, filter_linear, repeat_enable;
// Shore waves (PhysXWaterSurface3D::shore_waves), on the ripple grid: a = (phase,
// height) of trains 1 and 2, b = their phase gradients (k times the direction of
// travel, world xz); omega = each train's angular frequency.
uniform bool shore_waves_enabled = false;
uniform sampler2D shore_wave_tex_a : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D shore_wave_tex_b : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D shore_wave_tex_c : hint_default_black, filter_linear, repeat_disable;
uniform vec2 shore_wave_omega = vec2(0.785, 0.695);
uniform float shore_curl = 1.0;
uniform float shore_groups = 1.0;
const float BREAKER = 0.78;
// World XZ both height fields are centered on (set by PhysXWaterSurface3D).
uniform vec2 grid_center = vec2(0.0);
// Open, deep sea past the simulated square (PhysXWaterSurface3D::open_sea_beyond) instead of the edge's seabed.
uniform bool open_sea_beyond = false;
// World xz the whole open-water mesh is moved by, so its dense core stays
// under the camera; set by the node every frame.
uniform vec2 mesh_offset = vec2(0.0);
// Moving wake grids (PhysXWaterWake3D), up to four: height textures and
// their rects (xy world xz of the corner, z side length -- 0 = unused, w how
// much the wake fades out over the shore ripple grid, 0..1).
uniform sampler2D wake_height_0 : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D wake_height_1 : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D wake_height_2 : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D wake_height_3 : hint_default_black, filter_linear, repeat_disable;
uniform vec4 wake_rect_0 = vec4(0.0);
uniform vec4 wake_rect_1 = vec4(0.0);
uniform vec4 wake_rect_2 = vec4(0.0);
uniform vec4 wake_rect_3 = vec4(0.0);
// Mean surface height (node-relative), what the ripple layer settles back to
// past the edge of its domain when the surface is rendered further out.
uniform float water_level = 0.0;
// Animation clock for the shore noise, set by PhysXWaterSurface3D while the
// game runs -- not the shader's built-in clock: a material that reads it makes
// the editor redraw its 3D view every frame, and with a big scene open that
// ate half of Play Scene's frame rate. In the editor this stays 0 (the water
// is a static preview there anyway).
uniform float water_time = 0.0;
uniform vec4 water_color : source_color = vec4(0.09, 0.32, 0.42, 0.65);
// Path tracing only: light absorbed per metre of water, red, green and blue
// (default: clear coastal water, a little more than pure water's 0.35, 0.06,
// 0.01 from Pope and Fry 1997), and the surface's roughness.
uniform vec3 pathtraced_absorption = vec3(0.45, 0.09, 0.05);
// Path tracing only: light scattered per metre -- what gives deep water its
// blue body colour instead of black (pure water's molecular scattering is
// about 0.001, 0.002, 0.005 from Smith and Baker 1981; fine particles in
// coastal water add more, bluer the finer) -- and how much it keeps going
// forward (Petzold's ocean measurements: ~0.9). Higher = milkier.
uniform vec3 pathtraced_scattering = vec3(0.05, 0.08, 0.12);
uniform float pathtraced_scattering_anisotropy : hint_range(-0.9, 0.95) = 0.85;
uniform float pathtraced_roughness : hint_range(0.0, 1.0) = 0.02;
// Persistent whitecap foam (0..1) on the ocean grid, injected where the
// waves fold and fading over the node's foam_persistence (set by
// PhysXWaterSurface3D).
uniform sampler2D ocean_foam_tex : hint_default_black, filter_linear, repeat_enable;
uniform vec4 foam_color : source_color = vec4(0.92, 0.95, 0.97, 0.95);
// How much fading foam breaks up into lace instead of thinning evenly
// (0 = solid sheet until it's gone).
uniform float foam_breakup : hint_range(0.0, 1.0) = 0.6;
// Size of that lace: noise cells per meter, fixed to the water.
uniform float foam_detail_scale = 1.5;
// Shore foam (0..1) on the ripple grid: the foamy sheet where waves wash in
// over the shallows (set by PhysXWaterSurface3D; needs a seabed).
uniform sampler2D shore_foam_tex : hint_default_black, filter_linear, repeat_disable;
// Shore foam is a thin sheet, not a whitecap: fainter, finer and lacier.
uniform float shore_foam_strength : hint_range(0.0, 1.0) = 0.65;
uniform float shore_foam_breakup : hint_range(0.0, 1.0) = 0.85;
// Churned, milky water over the shallows: tints toward shallow_color as the
// ocean fade drops off near the shore (shallow_tint 0 = off). Its alpha also
// replaces water_color's there.
uniform vec4 shallow_color : source_color = vec4(0.42, 0.66, 0.64, 0.55);
uniform float shallow_tint : hint_range(0.0, 1.0) = 0.6;
// Still-water depth in meters over the ripple domain (dry shore negative,
// pool walls deep), set by PhysXWaterSurface3D: the waterline is measured in
// real depth, independent of shallow_fade_depth.
uniform sampler2D shore_depth_tex : hint_default_white, filter_linear, repeat_disable;
// Soft contact with the shore: the surface fades out over this much water
// depth (m) at the waterline instead of meeting the sand at a hard line.
uniform float shore_edge_softness : hint_range(0.0, 0.5, 0.005) = 0.08;
// Moving waterline: how much water depth (m) the visible edge can pull back
// over. Arriving crests push it toward the sand, troughs pull it back, and
// slow ground-fixed noise keeps it irregular -- so the edge scallops with the
// waves instead of following a depth contour, but only ever goes clear where
// the water really is that thin.
uniform float swash_reach : hint_range(0.0, 1.0, 0.01) = 0.25;
// Swash and wet sand from PhysXWaterSurface3D (swash_run_up, ...): r = how
// high (m) above still water the last waves ran up, g = sand wetness.
uniform sampler2D swash_tex : hint_default_black, filter_linear, repeat_disable;
// Thickness (m) of the swash film over the sand -- just enough to stay above
// the ground mesh.
uniform float swash_film_thickness : hint_range(0.0, 0.2, 0.005) = 0.03;
// Fine ragged fingering of the swash edge: how much water depth (m) drifting
// small-scale noise adds or takes away there (on a 1:14 beach, 0.035 m is
// about half a meter of ragged edge).
uniform float swash_edge_breakup : hint_range(0.0, 0.2, 0.005) = 0.035;
// Size of the shore's small-scale detail -- the lace on the waterline rim,
// the fingering of the swash edge and the ragged wet-sand line, which
// foam_detail_scale doesn't reach. 1 = default; higher = finer.
uniform float shore_detail_scale : hint_range(0.1, 5.0, 0.05) = 1.0;
// Wet sand: rgb blended over the ground where the water has been, alpha the
// strength -- darkens it, fading as it dries.
uniform vec4 wet_sand_color : source_color = vec4(0.16, 0.12, 0.07, 0.45);
// Wave height (m) that pushes the waterline all the way in.
uniform float swash_wave_height = 0.3;
// Thin bright foam rim just behind the moving edge.
uniform float shore_rim_strength : hint_range(0.0, 1.0) = 0.8;

varying vec3 v_world_normal;
varying float v_foam;
varying vec2 v_rest_xz;
// 1 on the breaking lips' sheet (vertex colour alpha 0), whose vertices are already in place.
varying float v_lip;

// The camera under water (raster; the path tracer starts its camera rays in the water instead). Set by
// PhysXWaterSurface3D while the camera is near the water: whether it is under the surface, and the light
// reaching into the water: the sun's direction once refracted, its irradiance past the surface
// (pi x energy x colour x Fresnel transmission), and the sky's radiance from above. The medium is the
// path tracer's (pathtraced_absorption, pathtraced_scattering), so both modes fog alike.
uniform bool underwater_active = false;
uniform bool underwater_camera_below = false;
uniform vec3 underwater_sun_direction = vec3(0.0, -1.0, 0.0);
uniform vec3 underwater_sun_irradiance = vec3(0.0);
uniform vec3 underwater_sky_radiance = vec3(0.0);
// How dark the thin line where the surface crosses the lens is (0 none).
uniform float underwater_meniscus : hint_range(0.0, 1.0) = 0.35;
// Raster reflections from above (water_raster_reflections.h).
)" WATER_RASTER_REFLECTIONS_GLSL R"(
#if (defined(UNDERWATER_FOG) || defined(UNDERWATER_SURFACE)) && !defined(RT)
uniform sampler2D underwater_screen : hint_screen_texture, filter_linear, repeat_disable;
uniform sampler2D underwater_depth : hint_depth_texture, filter_nearest, repeat_disable;
#endif

// Flow (PhysXWaterSurface3D::current): the current in m/s past the simulated
// square, and the flow solved round the ground inside it (flow_tex, RG m/s,
// when flow_baked). The waves and foam are carried along it with two-phase
// flow (Vlachos 2010): two copies, each offset along the flow for one
// flow_period and then restarted, blended so each restarts while unseen.
uniform sampler2D flow_tex : hint_default_black, filter_linear, repeat_disable;
uniform bool flow_baked = false;
uniform vec2 current = vec2(0.0);
uniform float flow_period = 2.0;
uniform bool flow_foam_live = false; // flow_tex is the live velocity, with flow foam in b

// Plumes of rivers flowing in (RG m/s over the simulated square), added on top.
uniform sampler2D plume_tex : hint_default_black, filter_linear, repeat_disable;
uniform bool plume_enabled = false;

vec2 flow_at(vec2 world_xz) {
	vec2 rel = world_xz - grid_center;
	vec2 a = abs(rel / ripple_domain_size);
	vec2 cuv = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float outside = smoothstep(0.46, 0.5, max(a.x, a.y));
	vec2 f = flow_baked ? mix(texture(flow_tex, cuv).rg, current, outside) : current;
	if (plume_enabled) {
		f += texture(plume_tex, cuv).rg * (1.0 - outside);
	}
	return f;
}

float flow_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float flow_value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(flow_hash(i), flow_hash(i + vec2(1.0, 0.0)), u.x),
			mix(flow_hash(i + vec2(0.0, 1.0)), flow_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// The two phases' offsets at world_xz and the first one's weight (1 and no
// offsets in still water). Noise in the phase keeps the restarts from
// pulsing across the whole surface at once.
float flow_phases(vec2 world_xz, out vec2 o_a, out vec2 o_b) {
	vec2 f = flow_at(world_xz);
	o_a = vec2(0.0);
	o_b = vec2(0.0);
	if (dot(f, f) < 1e-8) {
		return 1.0;
	}
	float t = water_time / max(flow_period, 0.05) + flow_value_noise(world_xz * 0.05);
	float p_a = fract(t);
	float p_b = fract(t + 0.5);
	o_a = f * (p_a - 0.5) * flow_period;
	o_b = f * (p_b - 0.5) * flow_period;
	return 1.0 - abs(1.0 - 2.0 * p_a);
}

// An ocean texture at world_xz, carried by the flow.
vec4 ocean_tex(sampler2D tex, vec2 world_xz) {
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(world_xz, o_a, o_b);
	vec2 uv = world_xz / ocean_domain_size + 0.5; // wraps: the FFT ocean is periodic, anchored to the world
	if (w >= 1.0) {
		return texture(tex, uv);
	}
	return texture(tex, uv - o_a / ocean_domain_size) * w + texture(tex, uv - o_b / ocean_domain_size) * (1.0 - w);
}

// The same for a larger cascade's tile.
vec4 cascade_tex(sampler2D tex, vec2 domain, vec2 world_xz) {
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(world_xz, o_a, o_b);
	vec2 uv = world_xz / domain + 0.5;
	if (w >= 1.0) {
		return texture(tex, uv);
	}
	return texture(tex, uv - o_a / domain) * w + texture(tex, uv - o_b / domain) * (1.0 - w);
}

// All cascades summed (heights, displacements and their derivatives add; foam is the most of any).
vec4 ocean_height_all(vec2 world_xz) {
	vec4 v = ocean_tex(ocean_height_tex, world_xz);
	if (wave_cascade_count > 1) {
		v += cascade_tex(cascade_height_1, cascade_domain_1, world_xz);
	}
	if (wave_cascade_count > 2) {
		v += cascade_tex(cascade_height_2, cascade_domain_2, world_xz);
	}
	if (wave_cascade_count > 3) {
		v += cascade_tex(cascade_height_3, cascade_domain_3, world_xz);
	}
	return v;
}

vec4 ocean_disp_all(vec2 world_xz) {
	vec4 v = ocean_tex(ocean_disp_tex, world_xz);
	if (wave_cascade_count > 1) {
		v += cascade_tex(cascade_disp_1, cascade_domain_1, world_xz);
	}
	if (wave_cascade_count > 2) {
		v += cascade_tex(cascade_disp_2, cascade_domain_2, world_xz);
	}
	if (wave_cascade_count > 3) {
		v += cascade_tex(cascade_disp_3, cascade_domain_3, world_xz);
	}
	return v;
}

vec4 ocean_deriv_all(vec2 world_xz) {
	vec4 v = ocean_tex(ocean_deriv_tex, world_xz);
	if (wave_cascade_count > 1) {
		v += cascade_tex(cascade_deriv_1, cascade_domain_1, world_xz);
	}
	if (wave_cascade_count > 2) {
		v += cascade_tex(cascade_deriv_2, cascade_domain_2, world_xz);
	}
	if (wave_cascade_count > 3) {
		v += cascade_tex(cascade_deriv_3, cascade_domain_3, world_xz);
	}
	return v;
}

float ocean_foam_all(vec2 world_xz) {
	float f = ocean_tex(ocean_foam_tex, world_xz).r;
	if (wave_cascade_count > 1) {
		f = max(f, cascade_tex(cascade_foam_1, cascade_domain_1, world_xz).r);
	}
	if (wave_cascade_count > 2) {
		f = max(f, cascade_tex(cascade_foam_2, cascade_domain_2, world_xz).r);
	}
	if (wave_cascade_count > 3) {
		f = max(f, cascade_tex(cascade_foam_3, cascade_domain_3, world_xz).r);
	}
	return f;
}

// Offset of the surface point that rests at world_xz: (dx, height, dz).
// One wake grid's height (x) and foam (y) at world_xz, faded out toward its edges.
vec2 wake_one(sampler2D tex, vec4 rect, vec2 world_xz) {
	if (rect.z <= 0.0) {
		return vec2(0.0);
	}
	vec2 uv = (world_xz - rect.xy) / rect.z;
	float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
	if (edge <= 0.0) {
		return vec2(0.0);
	}
	// Optionally fade out over the shore ripple grid (where a floater's own
	// ripples can take over).
	vec2 from_center = abs((world_xz - grid_center) / ripple_domain_size);
	float outside = smoothstep(0.46, 0.5, max(from_center.x, from_center.y));
	vec4 t = texture(tex, uv); // height, hull foam, propeller foam
	return vec2(t.r, t.g + t.b) * smoothstep(0.0, 0.1, edge) * mix(1.0, outside, rect.w);
}

// 0 inside the simulated square .. 1 past it -- only with open_sea_beyond (else the edge carries on outward).
float beyond_w(vec2 world_xz) {
	vec2 a = abs((world_xz - grid_center) / ripple_domain_size);
	return open_sea_beyond ? smoothstep(0.46, 0.5, max(a.x, a.y)) : 0.0;
}

// The ocean chop multiplier / the still-water depth at world_xz (cuv_r = its clamped ripple UV): open, deep sea past
// the square with open_sea_beyond.
float fade_r(vec2 world_xz, vec2 cuv_r) {
	return mix(texture(ocean_fade_tex, cuv_r).r, 1.0, beyond_w(world_xz));
}

float depth_r(vec2 world_xz, vec2 cuv_r) {
	return mix(texture(shore_depth_tex, cuv_r).r, 1000.0, beyond_w(world_xz));
}

// All wakes at world_xz, everywhere (inside the shore ripple grid too).
vec2 wake_sample(vec2 world_xz) {
	vec2 w = wake_one(wake_height_0, wake_rect_0, world_xz) + wake_one(wake_height_1, wake_rect_1, world_xz)
			+ wake_one(wake_height_2, wake_rect_2, world_xz) + wake_one(wake_height_3, wake_rect_3, world_xz);
	// Not over dry sand: the wake dies out over the last 30 cm of depth.
	vec2 cuv_r = clamp((world_xz - grid_center) / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	return w * smoothstep(0.0, 0.3, depth_r(world_xz, cuv_r));
}

float wake_height(vec2 world_xz) {
	return wake_sample(world_xz).x;
}

// Foam the wakes churned in (bow wave, propeller wash, breaking wake waves).
float wake_foam(vec2 world_xz) {
	return min(wake_sample(world_xz).y, 1.0);
}

// Shore waves at the water resting at world_xz: returns (dx, eta, dz); r_slope
// = (d eta/dx, d eta/dz), r_white = white water of breaking crests. Each train
// is a trochoid riding its refracted phase; where its height reaches the
// breaker limit (0.78 of the depth) the front steepens (the profile is skewed
// shoreward) and the crest and the water behind it turn white. Heights vary
// slowly along the crests, so the waves come in sets with calmer gaps.
// A breaking wave's front: the phase u (crest at the back fraction b of the
// period) remapped so the back takes 2 pi b of phase and the front face the
// rest; b near 1 gives a bore-like face about as wide as the wave is high,
// steep enough for the lip to overhang it. r_dps = d(ps)/du.
float shore_front_warp(float u, float b, out float r_dps) {
	float uw = u - 6.28318530718 * floor((u + 3.14159265359) / 6.28318530718);
	float uc = 3.14159265359 * (2.0 * b - 1.0);
	if (uw < uc) {
		r_dps = 0.5 / b;
		return -3.14159265359 + (uw + 3.14159265359) * r_dps;
	}
	r_dps = 0.5 / (1.0 - b);
	return (uw - uc) * r_dps;
}

// Each wave's own height (docs/ideas/water.md "Each wave breaks at its own place"): the envelope of a
// narrow-band sea, five close frequencies around the train's (Longuet-Higgins 1952, 1984), as a function of the
// wave count th / 2 pi, which travels with the crests. Its mean square is 1, so the individual heights are
// Rayleigh distributed with the train's height as their rms (the energy is unchanged), and they come in
// sets of about five bigger waves every twelve or so. Train i takes its own phases.
float shore_wave_set(float th, float train) {
	float x = th * 0.159154943;
	vec2 s = vec2(0.0);
	s += vec2(cos(-0.446 * x + 0.0 + 1.7 * train), sin(-0.446 * x + 0.0 + 1.7 * train));
	s += vec2(cos(-0.207 * x + 2.1 + 3.1 * train), sin(-0.207 * x + 2.1 + 3.1 * train));
	s += vec2(cos(0.025 * x + 4.4 + 0.7 * train), sin(0.025 * x + 4.4 + 0.7 * train));
	s += vec2(cos(0.295 * x + 1.3 + 2.3 * train), sin(0.295 * x + 1.3 + 2.3 * train));
	s += vec2(cos(0.490 * x + 5.6 + 4.9 * train), sin(0.490 * x + 5.6 + 4.9 * train));
	return mix(1.0, length(s) * 0.4472136, shore_groups);
}

// The baked height and stage are the train's (rms) wave; one of r times its height shoals to r times the
// unbroken height, breaks where that reaches the breaker index (stage r times the baked one) and is then held
// at the breaker height. Dry cells (stage 3) stay dry.
void shore_per_wave(float r, inout float height, inout float stage) {
	float sb = stage;
	if (sb < 2.99) {
		height *= min(r * sb, 1.0) / max(min(sb, 1.0), 1e-3);
		stage = r * sb;
	}
}

vec3 shore_wave(vec2 world_xz, out vec2 r_slope, out float r_white) {
	r_slope = vec2(0.0);
	r_white = 0.0;
	if (!shore_waves_enabled) {
		return vec3(0.0);
	}
	vec2 rel = world_xz - grid_center;
	vec2 cuv_r = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	vec2 a_c = abs(rel / ripple_domain_size);
	float inside = 1.0 - smoothstep(0.46, 0.5, max(a_c.x, a_c.y));
	if (inside <= 0.0) {
		return vec3(0.0);
	}
	vec4 a = texture(shore_wave_tex_a, cuv_r);
	vec4 b = texture(shore_wave_tex_b, cuv_r);
	vec4 c = texture(shore_wave_tex_c, cuv_r);
	float depth = max(depth_r(world_xz, cuv_r), 0.0);
	float vary = 0.55 + 0.45 * flow_value_noise(world_xz * 0.04 + vec2(water_time * 0.03, 0.0));
	// Plunging or spilling, from the surf similarity number (Battjes 1974):
	// spilling below about 0.5, plunging up to about 3.3, surging beyond.
	vec3 off = vec3(0.0);
	for (int i = 0; i < 2; i++) {
		float phi = i == 0 ? a.x : a.z;
		float height = i == 0 ? a.y : a.w;
		vec2 g = i == 0 ? b.xy : b.zw;
		float om = i == 0 ? shore_wave_omega.x : shore_wave_omega.y;
		float k = length(g);
		if (height <= 1e-4 || k <= 1e-5) {
			continue;
		}
		vec2 dir = g / k;
		float th = phi - om * water_time;
		// This wave's height: bigger ones break further out, smaller ones later.
		float r = shore_wave_set(th, float(i));
		float stage_raw = i == 0 ? c.x : c.y;
		shore_per_wave(r, height, stage_raw);
		// A bigger wave is steeper offshore, so it plunges less (xi goes as 1 / sqrt(H0)).
		float xi = c.z * inversesqrt(max(r, 0.05));
		float plunging = smoothstep(0.35, 0.6, xi) * (1.0 - smoothstep(2.6, 3.3, xi)) * shore_curl;
		float brk = smoothstep(0.8, 0.98, height / max(BREAKER * depth, 1e-3));
		float amp = 0.5 * height * vary * inside;
		// The plunging breaker through the surf zone (the stage is how far the
		// wave is past its break point): the face steepens while the lip is
		// thrown (a separate sheet, built on the CPU along the crests), the
		// water ahead drops where it lands, and the wave runs on as a bore.
		float stage = clamp((stage_raw - 0.95) / 1.0, 0.0, 1.0);
		float throw_w = plunging * vary * smoothstep(0.0, 0.45, stage) * (1.0 - smoothstep(0.7, 1.0, stage));
		float drop_w = plunging * vary * smoothstep(0.3, 0.75, stage) * (1.0 - smoothstep(0.8, 1.0, stage));
		float skew = min(0.6 * brk + 0.3 * throw_w, 0.92);
		// Plunging: the face steepens to about 1.5 wave heights wide (half
		// that steep again as a bore), so the lip has room to curl over it.
		float steep = plunging * vary * smoothstep(0.0, 0.3, stage) * (1.0 - 0.5 * smoothstep(0.8, 1.0, stage));
		float front = mix(0.5, clamp(k * 3.0 * amp / 6.28318530718, 0.02, 0.5), clamp(steep, 0.0, 1.0));
		float dps;
		float ps = shore_front_warp(th - skew * cos(th), 1.0 - front, dps);
		// Breaking waves in shallow water have sharp crests and long, flat
		// troughs (as cnoidal waves): ((1 + cos) / 2)^q, kept zero-mean
		// (1 / sqrt(pi (q + 1/4)) is the mean to within half a percent).
		float q = 1.0 + 1.5 * brk;
		float base = max(0.5 + 0.5 * cos(ps), 0.0);
		off.y += amp * 2.0 * (pow(base, q) - inversesqrt(3.14159265359 * (q + 0.25)));
		r_slope += -amp * q * pow(max(base, 1e-4), q - 1.0) * sin(ps) * dps * (1.0 + skew * sin(th)) * g;
		float hor = min(amp, 0.8 / k) * 0.8;
		off.xz += -dir * hor * sin(ps);
		// Phase from the crest, -pi..pi, positive on the shoreward face.
		float pc = ps - 6.28318530718 * floor((ps + 3.14159265359) / 6.28318530718);
		// x * x, not pow(x, 2.0): GLSL leaves pow undefined for a negative base
		// (NaN on some drivers, which the BLAS turns into fans of long triangles).
		float lip_x = (pc - 0.2) / 0.45;
		float lip = exp(-lip_x * lip_x);
		float drop_x = (pc - 0.35) / 0.35;
		off.y -= 1.6 * amp * drop_w * exp(-drop_x * drop_x);
		// White water: the breaking crest, and the bore's foam trailing behind it.
		float behind = fract(-th / 6.28318530718);
		r_white = max(r_white, brk * vary * max(smoothstep(0.35, 0.95, cos(ps)), 0.7 * exp(-behind * 5.0)));
		// The lip turns white as it falls, and the plunge throws up a splash.
		r_white = max(r_white, drop_w * lip);
	}
	return off;
}

vec3 surface_offset(vec2 world_xz) {
	vec2 rel = world_xz - grid_center;
	vec2 cuv_r = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float fade = fade_r(world_xz, cuv_r);
	// Negative: the displacement points away from crests in this FFT's sign
	// convention, so moving against it gathers points into the crests.
	vec2 d = -choppiness * fade * ocean_disp_all(world_xz).rg;
	// The ripple layer isn't periodic: past its domain, ease back to still water.
	vec2 from_center = abs(rel / ripple_domain_size);
	float inside = 1.0 - smoothstep(0.46, 0.5, max(from_center.x, from_center.y));
	float ripple = mix(water_level, texture(ripple_height_tex, cuv_r).r, inside);
	float h = ripple + ocean_height_all(world_xz).r * fade;
	vec2 shore_slope;
	float shore_white;
	vec3 shore = shore_wave(world_xz, shore_slope, shore_white);
	h += shore.y;
	d += shore.xz;
	// The wake grid is laid out where the water is now, not where it rests
	// (its sources sit under the hull, and sample_height() reads it there):
	// read it at this point's displaced position.
	h += wake_height(world_xz + d);
	// Never below the ground: over dry sand (and in the deepest troughs over
	// shallows) the surface lies on the sand as a thin film, which is where
	// the swash shows.
	float ground = water_level - depth_r(world_xz, cuv_r);
	h = max(h, ground + swash_film_thickness);
	return vec3(d.x, h, d.y);
}

// Exact normal of the displaced surface at the point resting at world_xz:
// tangents of P(x0) = x0 + s D(x0) + h(x0) from the FFT's own derivatives. The ripple layer has no FFT, so its slope
// is a small central difference.
// der and disp: ocean_deriv_all() and ocean_disp_all() at world_xz (the
// fragment reads them once for the normal and the foam).
vec3 surface_normal_pixel(vec2 world_xz, vec4 der, vec3 disp) {
	vec2 rel = world_xz - grid_center;
	vec2 cuv_r = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float fade = fade_r(world_xz, cuv_r);
	float s = -choppiness * fade;
	float dxz = disp.b;
	const float e = 0.2;
	vec2 du = vec2(e) / ripple_domain_size;
	float rx = (texture(ripple_height_tex, cuv_r + vec2(du.x, 0.0)).r - texture(ripple_height_tex, cuv_r - vec2(du.x, 0.0)).r) / (2.0 * e);
	float rz = (texture(ripple_height_tex, cuv_r + vec2(0.0, du.y)).r - texture(ripple_height_tex, cuv_r - vec2(0.0, du.y)).r) / (2.0 * e);
	// The ripple layer steps at the waterline (dry cells report their wet
	// neighbours' mean), which the sun picked out as bright streaks along the
	// shore; over the sand the swash film just follows the ground.
	float wet_w = smoothstep(0.0, 0.15, depth_r(world_xz, cuv_r));
	rx *= wet_w;
	rz *= wet_w;
	// Wake slopes (zero over the shore grid), at the displaced position as
	// surface_offset() reads the wake.
	const float ew = 0.12;
	vec2 wake_xz = world_xz + s * disp.rg;
	rx += (wake_height(wake_xz + vec2(ew, 0.0)) - wake_height(wake_xz - vec2(ew, 0.0))) / (2.0 * ew);
	rz += (wake_height(wake_xz + vec2(0.0, ew)) - wake_height(wake_xz - vec2(0.0, ew))) / (2.0 * ew);
	vec3 tx = vec3(1.0 + s * der.z, der.x * fade + rx, s * dxz);
	vec3 tz = vec3(s * dxz, der.y * fade + rz, 1.0 + s * der.w);
	vec4 shore_a = texture(shore_wave_tex_a, cuv_r);
	if (shore_waves_enabled && max(shore_a.y, shore_a.w) > 1e-4) {
		// The shore waves move points sideways as well as up (and fold the
		// surface over in a plunging lip): their tangents by differences.
		vec2 sl;
		float sw;
		const float es = 0.08;
		vec3 sx = (shore_wave(world_xz + vec2(es, 0.0), sl, sw) - shore_wave(world_xz - vec2(es, 0.0), sl, sw)) / (2.0 * es);
		vec3 sz = (shore_wave(world_xz + vec2(0.0, es), sl, sw) - shore_wave(world_xz - vec2(0.0, es), sl, sw)) / (2.0 * es);
		tx += vec3(sx.x, sx.y, sx.z);
		tz += vec3(sz.x, sz.y, sz.z);
	}
	return normalize(cross(tz, tx));
}

float foam_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float foam_value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(foam_hash(i), foam_hash(i + vec2(1.0, 0.0)), u.x),
			mix(foam_hash(i + vec2(0.0, 1.0)), foam_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Visible foam for the water resting at world_xz: the persistent foam amount,
// eroded by fixed-to-the-water noise so it breaks into lace as it fades.
float foam_at_with(vec2 world_xz, vec4 der, vec3 disp) {
	vec2 rel = world_xz - grid_center;
	vec2 cuv_r = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float fade = fade_r(world_xz, cuv_r);
	float amount = ocean_foam_all(world_xz) * fade;
	// Each cascade's foam pass sees only its own waves, but crests break
	// where they add up: the long swell steepens the chop riding on it. The
	// Jacobian of the summed displacement (Tessendorf) catches those crests
	// as they break; the persistent foam stays per cascade.
	if (wave_cascade_count > 1 && foam_crest_threshold > -50.0) {
		float s = -choppiness;
		float dxz = disp.b;
		float jacobian = (1.0 + s * der.z) * (1.0 + s * der.w) - s * s * dxz * dxz;
		amount = max(amount, clamp((foam_crest_threshold - jacobian) / 0.25, 0.0, 1.0) * fade);
	}
	// White water from the live flow (its velocity texture's b), fading out at the square's edge.
	if (flow_foam_live) {
		vec2 a = abs(rel / ripple_domain_size);
		amount = max(amount, texture(flow_tex, cuv_r).b * (1.0 - smoothstep(0.44, 0.5, max(a.x, a.y))));
	}
	{
		vec2 shore_slope;
		float shore_white;
		shore_wave(world_xz, shore_slope, shore_white);
		amount = max(amount, shore_white);
	}
	// Wake foam at the displaced position, as surface_offset() reads the wake.
	amount = max(amount, wake_foam(world_xz - choppiness * fade * disp.rg));
	// The lace is carried with the foam (the same two phases).
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(world_xz, o_a, o_b);
	vec2 p = (world_xz - o_a) * foam_detail_scale;
	float n = 0.5 * foam_value_noise(p) + 0.3 * foam_value_noise(p * 2.7 + 17.0) + 0.2 * foam_value_noise(p * 7.3 - 5.0);
	if (w < 1.0) {
		vec2 q = (world_xz - o_b) * foam_detail_scale;
		n = n * w + (0.5 * foam_value_noise(q) + 0.3 * foam_value_noise(q * 2.7 + 17.0) + 0.2 * foam_value_noise(q * 7.3 - 5.0)) * (1.0 - w);
	}
	// Gentle ramp: fresh foam is solid, thinning foam turns translucent and
	// lacy as the noise eats into it, rather than cutting off at an edge.
	float f = clamp(amount * (1.0 + foam_breakup) - n * foam_breakup, 0.0, 1.0);
	return f * f * (3.0 - 2.0 * f);
}

float foam_at(vec2 world_xz) {
	return foam_at_with(world_xz, ocean_deriv_all(world_xz), ocean_disp_all(world_xz).rgb);
}

// Visible shore foam at world_xz: finer, heavier lace at partial strength.
)"
							   R"(
// Ripple-grid UV for the shore's moving patterns (swash, wet sand, shore
// foam), mirror-repeated past the simulated square: they carry on down the
// beach without a seam instead of stretching the edge row into straight
// lines. (The still-water depth itself stays clamped, so land and sea are
// never mirrored into each other.)
vec2 shore_pattern_uv(vec2 world_xz) {
	vec2 u = (world_xz - grid_center) / ripple_domain_size + 0.5;
	return 1.0 - abs(mod(u, 2.0) - 1.0);
}

float shore_foam_at(vec2 world_xz) {
	vec2 cuv_r = clamp((world_xz - grid_center) / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	vec2 q = world_xz * foam_detail_scale * 2.2 + 31.0;
	float ns = 0.5 * foam_value_noise(q) + 0.5 * foam_value_noise(q * 2.3 - 11.0);
	float shore = texture(shore_foam_tex, shore_pattern_uv(world_xz)).r * (1.0 - beyond_w(world_xz));
	float g = clamp(shore * (1.0 + shore_foam_breakup) - ns * shore_foam_breakup, 0.0, 1.0);
	return g * g * (3.0 - 2.0 * g) * shore_foam_strength;
}

// Water depth (m) above the moving waterline: the still-water depth minus an
// offset in [0, swash_reach] -- small under an arriving crest (the edge
// reaches in), large between crests, jittered by slow noise. <= 0 is past
// the visible edge.
float shore_signal(vec2 world_xz, float time) {
	vec2 rel = world_xz - grid_center;
	vec2 cuv_r = clamp(rel / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float depth = depth_r(world_xz, cuv_r);
	vec2 shore_slope;
	float shore_white;
	float eta = ocean_height_all(world_xz).r + shore_wave(world_xz, shore_slope, shore_white).y; // unfaded: the waves arriving here
	// Lobes a few meters across (like real swash), drifting, with the waves
	// pushing the whole edge in and out over time.
	vec2 p = world_xz * 0.45;
	float n = 0.65 * foam_value_noise(p + vec2(time * 0.12, time * 0.05)) + 0.35 * foam_value_noise(p * 2.4 - vec2(time * 0.07, 0.0));
	float wave = clamp(eta / max(swash_wave_height, 1e-3), -1.0, 1.0);
	float offset = swash_reach * clamp(0.5 + (n - 0.5) * 1.6 - 0.35 * wave, 0.0, 1.0);
	vec2 q = world_xz * shore_detail_scale;
	float eb = 0.5 * foam_value_noise(q * 2.3 + vec2(time * 0.25, 0.0)) + 0.3 * foam_value_noise(q * 6.1 + vec2(3.0, time * 0.4)) + 0.2 * foam_value_noise(q * 15.0 - 7.0);
	float run_up = texture(swash_tex, shore_pattern_uv(world_xz)).r * (1.0 - beyond_w(world_xz));
	float jitter = (eb - 0.5) * 2.0 * swash_edge_breakup;
	// On dry sand the jitter may only pull the edge back, never add water
	// past what the swash covers -- or it left a permanent fringe of rim
	// foam just above the still-water line.
	jitter = depth > 0.0 ? jitter : min(jitter, run_up);
	// The pull-back gives way where a swash is running up, so over the sand
	// the film reaches exactly as far as the run-up (and the wet sand); with
	// no swash it applies right up to the still-water line, or a sliver of
	// water was left showing along it.
	return depth - offset * (1.0 - smoothstep(0.0, 0.08, run_up)) + run_up + jitter;
}

// The camera under water.

// Height of the displaced surface above world_xz (choppy offset inverted, as the RT vertex() does).
float surface_height_world(vec2 world_xz, float base_y) {
	vec2 p = world_xz;
	for (int i = 0; i < 3; i++) {
		p = world_xz - surface_offset(p).xz;
	}
	return base_y + surface_offset(p).y;
}

vec3 uw_sigma_t() {
	return max(pathtraced_absorption, vec3(0.0)) + max(pathtraced_scattering, vec3(0.0));
}

float uw_hg(float cos_theta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4), 1.5));
}

// The integral of exp(-b t) over [0, L], per channel.
vec3 uw_segment(vec3 b, float len) {
	vec3 r;
	for (int i = 0; i < 3; i++) {
		r[i] = abs(b[i] * len) < 1e-4 ? len : (1.0 - exp(-b[i] * len)) / b[i];
	}
	return r;
}

// Single scattering toward the eye over len metres of water along d (unit), from a point z0 metres
// under the surface, in closed form: at depth z the sun is dimmed by exp(-sigma_t z / mu) along its
// refracted slant and the sky by exp(-1.2 sigma_t z) (diffuse light crossing the Snell window
// slantwise); the sun scatters by the water's Henyey-Greenstein phase, the sky isotropically
// (half the sphere lit). The path tracer does the same by sampling.
vec3 uw_inscatter(float z0, vec3 d, float len) {
	vec3 st = uw_sigma_t();
	vec3 ss = max(pathtraced_scattering, vec3(0.0));
	float mu = max(-underwater_sun_direction.y, 0.05);
	vec3 a = st / mu;
	vec3 sun = underwater_sun_irradiance * uw_hg(dot(underwater_sun_direction, -d), pathtraced_scattering_anisotropy) *
			exp(-a * z0) * uw_segment(st - a * d.y, len);
	vec3 k = st * 1.2;
	vec3 sky = underwater_sky_radiance * 0.5 * exp(-k * z0) * uw_segment(st - k * d.y, len);
	return ss * (sun + sky);
}

// The share of the surface's light left at depth z (raster lights ignore the water above a surface).
vec3 uw_light_at_depth(float z) {
	vec3 st = uw_sigma_t();
	float mu = max(-underwater_sun_direction.y, 0.05);
	float w_sun = dot(underwater_sun_irradiance, vec3(1.0 / 3.0)) * mu / PI;
	float w_sky = dot(underwater_sky_radiance, vec3(1.0 / 3.0));
	return (w_sun * exp(-st * z / mu) + w_sky * exp(-st * 1.2 * z)) / max(w_sun + w_sky, 1e-6);
}

// Fresnel reflectance of the water's underside (exact, unpolarised), 1 past the critical angle.
float uw_fresnel_from_below(float cos_i) {
	const float n1 = 1.33;
	float sin_t2 = n1 * n1 * max(1.0 - cos_i * cos_i, 0.0);
	if (sin_t2 >= 1.0) {
		return 1.0;
	}
	float cos_t = sqrt(1.0 - sin_t2);
	float rs = (n1 * cos_i - cos_t) / (n1 * cos_i + cos_t);
	float rp = (cos_i - n1 * cos_t) / (cos_i + n1 * cos_t);
	return 0.5 * (rs * rs + rp * rp);
}

#if defined(UNDERWATER_FOG)
// A full screen pass (Crest's underwater effect, with the path tracer's medium). Its mesh is a 2 mm quad
// at the node, so the path tracer, which never shows it anyway, has nothing in the way.
void vertex() {
	POSITION = vec4(VERTEX.xy * 1000.0, 1.0, 1.0);
}

void fragment() {
#if defined(RT)
	ALPHA = 0.0;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#else
	vec2 ndc = SCREEN_UV * 2.0 - 1.0;
	vec4 near_v = INV_PROJECTION_MATRIX * vec4(ndc, 1.0, 1.0);
	near_v /= near_v.w;
	vec3 p_near = (INV_VIEW_MATRIX * vec4(near_v.xyz, 1.0)).xyz;
	bool orthogonal = PROJECTION_MATRIX[3][3] != 0.0;
	vec3 d = orthogonal ? -INV_VIEW_MATRIX[2].xyz : (INV_VIEW_MATRIX * vec4(normalize(near_v.xyz), 0.0)).xyz;
	d = normalize(d);
	float base_y = MODEL_MATRIX[3].y;
	// Each pixel's point on the near plane against the surface over it: the water line across the lens.
	float above = p_near.y - surface_height_world(p_near.xz, base_y);
	// Meniscus: a dark line a couple of pixels wide where the surface meets the lens.
	float band = max(length(dFdy(p_near)), 1e-5) * 2.0;
	float meniscus = underwater_meniscus * exp(-(above * above) / (band * band));
	if (above > 0.0 && (d.y >= 0.0 || above > band * 8.0)) {
		if (meniscus < 0.004) {
			discard;
		}
		ALBEDO = vec3(0.0);
		ALPHA = meniscus;
	} else if (above > 0.0) {
		// Just over the water line, looking down: the mesh (straight triangles) and these heights
		// (filtered) can disagree by a few millimetres, leaving the floor seen past the near plane with
		// no surface in front of it. Fog the part of the ray under the water.
		float depth = texture(underwater_depth, SCREEN_UV).r;
		float len = 1e4;
		if (depth > 0.0) {
			vec4 o_v = INV_PROJECTION_MATRIX * vec4(ndc, depth, 1.0);
			len = distance((INV_VIEW_MATRIX * vec4(o_v.xyz / o_v.w, 1.0)).xyz, p_near);
		}
		float seg = max(len - above / -d.y, 0.0);
		vec3 scene = texture(underwater_screen, SCREEN_UV).rgb;
		ALBEDO = (scene * exp(-uw_sigma_t() * seg) + uw_inscatter(0.0, d, seg)) * (1.0 - meniscus);
		ALPHA = 1.0;
	} else {
		float z0 = -above;
		float depth = texture(underwater_depth, SCREEN_UV).r;
		float len = 1e4;
		vec3 p_o = p_near + d * len;
		if (depth > 0.0) {
			vec4 o_v = INV_PROJECTION_MATRIX * vec4(ndc, depth, 1.0);
			o_v /= o_v.w;
			p_o = (INV_VIEW_MATRIX * vec4(o_v.xyz, 1.0)).xyz;
			len = distance(p_o, p_near);
		}
		// Only the stretch under the water: past the surface the underside pass draws.
		float to_surface = d.y > 1e-4 ? z0 / d.y : 1e4;
		vec3 scene = texture(underwater_screen, SCREEN_UV).rgb;
		if (len <= to_surface) {
			// A surface under water: the water above it took part of its light on the way down.
			scene *= uw_light_at_depth(max(surface_height_world(p_o.xz, base_y) - p_o.y, 0.0));
		}
		float seg = min(len, to_surface);
		ALBEDO = (scene * exp(-uw_sigma_t() * seg) + uw_inscatter(z0, d, seg)) * (1.0 - meniscus);
		ALPHA = 1.0;
	}
#endif
}
#else

void vertex() {
#if defined(BREAKER_LIP)
	// UV.x is the lip's white water (UV.y unused); the vertex is where it is drawn.
	v_lip = 1.0;
	v_rest_xz = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_foam = UV.x;
#else
	v_lip = 0.0;
	if (COLOR.a < 0.5) {
		// A breaking lip (PhysXWaterSurface3D builds it in place), drawn with a
		// custom copy of this shader: COLOR.r is its white water.
		v_lip = 1.0;
		v_rest_xz = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
		v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
		v_foam = COLOR.r;
	} else {
	// Under path tracing too: a hit re-runs this on the captured rest pose.
	VERTEX += (inverse(MODEL_MATRIX) * vec4(mesh_offset.x, 0.0, mesh_offset.y, 0.0)).xyz;
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_rest_xz = world_pos.xz;
	VERTEX += surface_offset(world_pos.xz);
	v_world_normal = vec3(0.0, 1.0, 0.0);
	v_foam = 0.0;
	// Per pixel, fragment() computes the normal and foam instead.
	if (!per_pixel_normals) {
		// Per-vertex: normal from central differences of the displaced
		// surface -- e is in world meters, small against the grids' cell
		// size.
		const float e = 0.2;
		vec3 px1 = vec3(e, 0.0, 0.0) + surface_offset(world_pos.xz + vec2(e, 0.0));
		vec3 px0 = vec3(-e, 0.0, 0.0) + surface_offset(world_pos.xz - vec2(e, 0.0));
		vec3 pz1 = vec3(0.0, 0.0, e) + surface_offset(world_pos.xz + vec2(0.0, e));
		vec3 pz0 = vec3(0.0, 0.0, -e) + surface_offset(world_pos.xz - vec2(0.0, e));
		vec3 tx = px1 - px0;
		vec3 tz = pz1 - pz0;
		v_world_normal = normalize(cross(tz, tx));
		v_foam = foam_at(world_pos.xz);
	}
	}
#endif
}

void fragment() {
	// Draw only the side of the surface facing the camera's side of the
	// water: the top from above, the underside from below. The surface is
	// transparent and double-sided, and its triangles blend in mesh order,
	// not by distance -- so where it overlaps itself (the back of a crest
	// behind its front, far waves seen through near ones) the far side
	// painted over the near one as jagged translucent teeth, and the
	// pattern changed wherever the mesh order did, drawing a seam.
#if !defined(RT)
	bool camera_above = underwater_active ? !underwater_camera_below : CAMERA_POSITION_WORLD.y > MODEL_MATRIX[3].y + water_level;
#if defined(UNDERWATER_SURFACE)
	if (FRONT_FACING || camera_above) {
		discard;
	}
#else
	// With the camera under water the underside pass draws it. A breaking
	// lip is a closed sheet: both its sides are drawn.
	if ((FRONT_FACING != camera_above && v_lip < 0.5) || (underwater_active && !camera_above)) {
		discard;
	}
#endif
#endif
	// Across a lip's edges the vertex normals of its two sheets point apart and cancel: no direction there
	// (a NaN that the path tracer's filters would spread into squares), so those take the sheet's up.
	vec3 world_normal = v_world_normal;
	if (v_lip > 0.5) {
		float nl = length(v_world_normal);
		world_normal = nl > 1e-3 ? v_world_normal / nl : vec3(0.0, 1.0, 0.0);
	}
	float foam = v_foam;
	if (per_pixel_normals && v_lip < 0.5) {
		vec4 der = ocean_deriv_all(v_rest_xz);
		vec3 disp = ocean_disp_all(v_rest_xz).rgb;
		world_normal = surface_normal_pixel(v_rest_xz, der, disp);
		foam = foam_at_with(v_rest_xz, der, disp);
	}
#if defined(UNDERWATER_SURFACE) && defined(RT)
	ALPHA = 0.0; // the path tracer has the real surface
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#elif defined(UNDERWATER_SURFACE)
	// The surface from below (Crest's ApplyReflectionUnderwater, exact Fresnel): the world above through
	// Snell's window, refracted, and past the critical angle (48.6 degrees) a mirror of the water's own
	// in-scattered light (total internal reflection); then the fog of the water between.
	vec3 p_w = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 cam = CAMERA_POSITION_WORLD;
	vec3 d = normalize(p_w - cam);
	vec3 n_w = -world_normal; // the underside faces down
	if (dot(n_w, d) > 0.0) {
		n_w = -n_w;
	}
	float fresnel = uw_fresnel_from_below(clamp(dot(-d, n_w), 0.0, 1.0));
	float dist = distance(p_w, cam);
	vec3 from_above = underwater_sky_radiance;
	if (fresnel < 1.0) {
		vec3 t = normalize(refract(d, n_w, 1.33));
		// The refracted ray read off the screen where it meets what is behind the surface (its depth
		// straight behind; sky: far); off screen, the sky's radiance.
		float reach = 200.0;
		float depth_bg = texture(underwater_depth, SCREEN_UV).r;
		if (depth_bg > 0.0) {
			vec4 bg_v = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth_bg, 1.0);
			reach = clamp(length(bg_v.xyz / bg_v.w) - length(VERTEX), 0.5, 200.0);
		}
		vec4 clip = PROJECTION_MATRIX * VIEW_MATRIX * vec4(p_w + t * reach, 1.0);
		if (clip.w > 0.0) {
			vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
			vec2 inside2 = smoothstep(vec2(0.0), vec2(0.04), uv) * smoothstep(vec2(0.0), vec2(0.04), 1.0 - uv);
			from_above = mix(underwater_sky_radiance, texture(underwater_screen, clamp(uv, vec2(0.0), vec2(1.0))).rgb, inside2.x * inside2.y);
		}
	}
	// The mirror below: the reflected ray marched across the screen (a short screen-space reflection)
	// to what it meets under the water, seen through the water between; else the water's own light.
	vec3 r = reflect(d, n_w);
	vec3 mirrored = uw_inscatter(0.0, r, 1e4);
	if (fresnel > 0.02) {
		float t_prev = 0.0;
		for (int i = 1; i <= 16; i++) {
			float t = 0.25 * pow(1.3, float(i)); // 0.3 m to 17 m
			vec3 q = p_w + r * t;
			vec4 qc = PROJECTION_MATRIX * VIEW_MATRIX * vec4(q, 1.0);
			if (qc.w <= 0.0) {
				break;
			}
			vec2 quv = qc.xy / qc.w * 0.5 + 0.5;
			if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) {
				break;
			}
			float scene_depth = texture(underwater_depth, quv).r;
			// Reverse Z: the ray point is behind what the screen shows there.
			if (scene_depth > qc.z / qc.w) {
				vec4 sv = INV_PROJECTION_MATRIX * vec4(quv * 2.0 - 1.0, scene_depth, 1.0);
				vec3 hit = (INV_VIEW_MATRIX * vec4(sv.xyz / sv.w, 1.0)).xyz;
				float t_hit = distance(hit, p_w);
				// Thickness test: a hit far behind the step is something else in front.
				if (t_hit < t + (t - t_prev) * 2.0 + 0.5) {
					float z_hit = max(MODEL_MATRIX[3].y + water_level - hit.y, 0.0);
					vec3 seen = texture(underwater_screen, quv).rgb * uw_light_at_depth(z_hit);
					vec2 edge = smoothstep(vec2(0.0), vec2(0.06), quv) * smoothstep(vec2(0.0), vec2(0.06), 1.0 - quv);
					mirrored = mix(mirrored, seen * exp(-uw_sigma_t() * t_hit) + uw_inscatter(0.0, r, t_hit), edge.x * edge.y);
				}
				break;
			}
			t_prev = t;
		}
	}
	vec3 surf = from_above * (1.0 - fresnel) + mirrored * fresnel;
	// Foam from below: a lit, rough layer that lets little through.
	surf = mix(surf, foam_color.rgb * (underwater_sky_radiance + underwater_sun_irradiance / PI) * 0.5, foam * 0.85);
	float z_cam = max(MODEL_MATRIX[3].y + water_level - cam.y, 0.0);
	ALBEDO = surf * exp(-uw_sigma_t() * dist) + uw_inscatter(z_cam, d, dist);
	ALPHA = 1.0;
#else
	// unshaded + a small self-contained, bounded normal-based shading term --
	// NOT Godot's real diffuse/specular PBR response. Real root cause of the
	// "whitewash": this material used render_mode diffuse_burley,
	// specular_schlick_ggx, so it got its own full sun+ambient PBR light
	// response computed as if fully opaque, and THAT result was then alpha-
	// blended on top of the floor's OWN independently-lit PBR response.
	// Alpha blending two independently, fully-lit PBR surfaces in linear HDR
	// sums MORE light than either surface alone before the single tonemap
	// pass sees it -- exceeding any reasonable reference white, regardless
	// of how dark either material's own albedo is on its own (confirmed:
	// the bare floor alone renders correctly; only the blended combination
	// blows out). Going unshaded removes the water from that PBR light
	// budget entirely -- only the floor is now really lit by the scene, and
	// the water contributes a fixed, self-bounded tint on top of it.
	vec3 n = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	// Fixed, hardcoded pseudo-light direction -- not a real scene light, just
	// a bounded shaping term so wave normals still visibly modulate the
	// water's own tint (the whole reason per-vertex normals were added this
	// session in the first place), capped to a small multiplier so it can
	// never itself become a source of overexposure.
	float ndotl = clamp(dot(n, vec3(0.3, 0.85, 0.4)), 0.0, 1.0);
	// A breaking lip stands clear of the shore's film: none of the waterline's foam, rim or cut-out.
	float shore = v_lip > 0.5 ? 1.0 : shore_signal(v_rest_xz, water_time);
	// Shore foam belongs to the moving edge: full at it, thinning over the
	// next ~0.6 m of depth, so the white sheet scallops with the edge instead
	// of filling the band. (Whitecaps are left alone.)
	foam = max(foam, shore_foam_at(v_rest_xz) * (1.0 - smoothstep(0.02, 0.6, shore)));
	// Bright rim right behind the moving edge, broken into lace.
	// Over dry sand the swash film is only as thick as the run-up's excess
	// over the sand -- a few cm -- so it fades in over a fifth of the
	// softness there, or the uprush would barely show.
	vec2 cuv_here = clamp((v_rest_xz - grid_center) / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float soft = max(shore_edge_softness, 1e-3) * mix(0.2, 1.0, smoothstep(-0.05, 0.1, depth_r(v_rest_xz, cuv_here)));
	float rim = smoothstep(0.0, soft * 0.5, shore) * (1.0 - smoothstep(soft * 0.6, soft * 2.2, shore));
	rim *= shore_rim_strength * (0.55 + 0.45 * foam_value_noise(v_rest_xz * 6.0 * shore_detail_scale));
	foam = max(foam, rim * (1.0 - v_lip));
	vec2 cuv_fade = clamp((v_rest_xz - grid_center) / ripple_domain_size + 0.5, vec2(0.0), vec2(1.0));
	float shallow = (1.0 - fade_r(v_rest_xz, cuv_fade)) * shallow_tint;
	vec3 base_rgb = mix(water_color.rgb, shallow_color.rgb, shallow) * mix(0.75, 1.15, ndotl);
	float base_a = mix(water_color.a, shallow_color.a, shallow);
	// Thin water is clear: the swash film and the last few cm at the edge let
	// the sand show through, turning opaque by ~25 cm of depth.
	base_a *= mix(0.4, 1.0, smoothstep(0.0, 0.25, shore));
	ALBEDO = mix(base_rgb, foam_color.rgb, foam);
	float water_a = mix(base_a, foam_color.a, foam) * smoothstep(0.0, soft, shore);
	// Wet sand under and behind the swash: blended in where the water is
	// thin or gone, so it shows through the film and lingers after it.
	// Noise eats into the wet edge, so it dries back ragged rather than
	// along a clean line.
	vec2 wp = v_rest_xz * shore_detail_scale;
	float wn = 0.6 * foam_value_noise(wp * 1.7) + 0.4 * foam_value_noise(wp * 5.3 + 7.0);
	float wet = clamp((texture(swash_tex, shore_pattern_uv(v_rest_xz)).g - 0.35 * wn) / 0.65, 0.0, 1.0);
	// Only where sand is near the surface: under deeper water the "wet" flag
	// is meaningless, and blending it there tinted and roughened all deep
	// water -- which showed as a seam past the simulated square, where the
	// mirrored patterns bring dry beach (not wet) out over the open sea.
	wet *= (1.0 - smoothstep(0.05, 0.35, depth_r(v_rest_xz, cuv_fade))) * (1.0 - v_lip);
	float wet_a = wet * wet * (3.0 - 2.0 * wet) * wet_sand_color.a * (1.0 - water_a);
#if defined(RT)
	// Both faces bound the water: the top enters it, the underside leaves it
	// (the path tracer tracks the medium between them). Foam is an opaque,
	// rough white layer on top; where the swash has left the sand, no surface.
	NORMAL = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	ALBEDO = mix(vec3(1.0), foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness, 0.8, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	TRANSMISSION = 1.0 - foam;
	IOR = 1.33;
	ABSORPTION = pathtraced_absorption;
	SCATTERING = pathtraced_scattering;
	SCATTERING_ANISOTROPY = pathtraced_scattering_anisotropy;
#if !defined(BREAKER_LIP)
	// A lip is water wherever it is (the shore cut-out belongs to the swash film). It writes no alpha at
	// all: an alpha-tested material is also cut by the any-hit pass, which does not see the lip's varyings.
	ALPHA = smoothstep(0.0, soft, shore);
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#endif
#else
	ALPHA = water_a + wet_a;
	ALBEDO = (ALBEDO * water_a + wet_sand_color.rgb * wet_a) / max(ALPHA, 1e-4);
	if (camera_above) {
		// Open water reflects; foam and dry sand are rough.
		vec4 refl = water_raster_reflect(ALBEDO, ALPHA, world_normal, (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz, CAMERA_POSITION_WORLD, smoothstep(0.0, soft, shore) * (1.0 - foam), pathtraced_roughness);
		ALBEDO = refl.rgb;
		ALPHA = refl.a;
	}
#endif
#endif // UNDERWATER_SURFACE
}
#endif // UNDERWATER_FOG
)";

// Sign of the X/Z cross product of an up-facing PlaneMesh triangle -- measured
// from PlaneMesh itself rather than assumed from a winding convention.
real_t plane_mesh_xz_winding() {
	Ref<PlaneMesh> ref_plane;
	ref_plane.instantiate();
	const Vector<Face3> faces = ref_plane->get_faces();
	if (faces.is_empty()) {
		return 1.0;
	}
	const Face3 &f = faces[0];
	const Vector2 a(f.vertex[0].x, f.vertex[0].z), b(f.vertex[1].x, f.vertex[1].z), c(f.vertex[2].x, f.vertex[2].z);
	return (b - a).cross(c - a) < 0 ? -1.0 : 1.0;
}

// Inside or on the edge of triangle abc, either winding.
bool point_in_triangle(const Vector2 &p_p, const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c) {
	const real_t d1 = (p_b - p_a).cross(p_p - p_a);
	const real_t d2 = (p_c - p_b).cross(p_p - p_b);
	const real_t d3 = (p_a - p_c).cross(p_p - p_c);
	const bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
	const bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
	return !(has_neg && has_pos);
}

// Liang-Barsky: does segment p0-p1 touch the box [p_min, p_max]?
bool segment_touches_box(const Vector2 &p_p0, const Vector2 &p_p1, const Vector2 &p_min, const Vector2 &p_max) {
	real_t t0 = 0.0, t1 = 1.0;
	const Vector2 d = p_p1 - p_p0;
	for (int axis = 0; axis < 2; axis++) {
		const real_t q0 = p_min[axis] - p_p0[axis];
		const real_t q1 = p_max[axis] - p_p0[axis];
		if (Math::abs(d[axis]) < (real_t)1e-12) {
			if (q0 > 0 || q1 < 0) {
				return false;
			}
			continue;
		}
		real_t ta = q0 / d[axis];
		real_t tb = q1 / d[axis];
		if (ta > tb) {
			SWAP(ta, tb);
		}
		t0 = MAX(t0, ta);
		t1 = MIN(t1, tb);
		if (t0 > t1) {
			return false;
		}
	}
	return true;
}

// Sutherland-Hodgman: clip a convex polygon to a counter-clockwise triangle.
void clip_to_triangle(LocalVector<Vector2> &r_poly, const Vector2 p_tri[3]) {
	LocalVector<Vector2> out;
	for (int e = 0; e < 3 && r_poly.size() >= 3; e++) {
		const Vector2 a = p_tri[e];
		const Vector2 edge = p_tri[(e + 1) % 3] - a;
		out.clear();
		for (uint32_t i = 0; i < r_poly.size(); i++) {
			const Vector2 cur = r_poly[i];
			const Vector2 prev = r_poly[(i + r_poly.size() - 1) % r_poly.size()];
			const real_t side_cur = edge.cross(cur - a);
			const real_t side_prev = edge.cross(prev - a);
			if (side_cur >= 0) {
				if (side_prev < 0) {
					out.push_back(prev + (cur - prev) * (side_prev / (side_prev - side_cur)));
				}
				out.push_back(cur);
			} else if (side_prev >= 0) {
				out.push_back(prev + (cur - prev) * (side_prev / (side_prev - side_cur)));
			}
		}
		r_poly = out;
	}
}

} // namespace

void PhysXWaterSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_domain_size", "size"), &PhysXWaterSurface3D::set_domain_size);
	ClassDB::bind_method(D_METHOD("get_domain_size"), &PhysXWaterSurface3D::get_domain_size);
	ClassDB::bind_method(D_METHOD("set_grid_resolution", "n"), &PhysXWaterSurface3D::set_grid_resolution);
	ClassDB::bind_method(D_METHOD("get_grid_resolution"), &PhysXWaterSurface3D::get_grid_resolution);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &PhysXWaterSurface3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &PhysXWaterSurface3D::get_depth);
	ClassDB::bind_method(D_METHOD("set_water_level", "level"), &PhysXWaterSurface3D::set_water_level);
	ClassDB::bind_method(D_METHOD("get_water_level"), &PhysXWaterSurface3D::get_water_level);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &PhysXWaterSurface3D::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &PhysXWaterSurface3D::get_damping);
	ClassDB::bind_method(D_METHOD("set_ripple_amplitude", "amplitude"), &PhysXWaterSurface3D::set_ripple_amplitude);
	ClassDB::bind_method(D_METHOD("get_ripple_amplitude"), &PhysXWaterSurface3D::get_ripple_amplitude);

	ClassDB::bind_method(D_METHOD("set_ocean_grid_resolution", "n"), &PhysXWaterSurface3D::set_ocean_grid_resolution);
	ClassDB::bind_method(D_METHOD("get_ocean_grid_resolution"), &PhysXWaterSurface3D::get_ocean_grid_resolution);
	ClassDB::bind_method(D_METHOD("set_ocean_domain_size", "size"), &PhysXWaterSurface3D::set_ocean_domain_size);
	ClassDB::bind_method(D_METHOD("get_ocean_domain_size"), &PhysXWaterSurface3D::get_ocean_domain_size);
	ClassDB::bind_method(D_METHOD("set_wind_speed", "speed"), &PhysXWaterSurface3D::set_wind_speed);
	ClassDB::bind_method(D_METHOD("get_wind_speed"), &PhysXWaterSurface3D::get_wind_speed);
	ClassDB::bind_method(D_METHOD("set_wind_direction", "dir"), &PhysXWaterSurface3D::set_wind_direction);
	ClassDB::bind_method(D_METHOD("get_wind_direction"), &PhysXWaterSurface3D::get_wind_direction);
	ClassDB::bind_method(D_METHOD("set_shore_waves", "enabled"), &PhysXWaterSurface3D::set_shore_waves);
	ClassDB::bind_method(D_METHOD("get_shore_waves"), &PhysXWaterSurface3D::get_shore_waves);
	ClassDB::bind_method(D_METHOD("set_shore_wave_height", "height"), &PhysXWaterSurface3D::set_shore_wave_height);
	ClassDB::bind_method(D_METHOD("get_shore_wave_height"), &PhysXWaterSurface3D::get_shore_wave_height);
	ClassDB::bind_method(D_METHOD("set_mesh_detail", "detail"), &PhysXWaterSurface3D::set_mesh_detail);
	ClassDB::bind_method(D_METHOD("get_mesh_detail"), &PhysXWaterSurface3D::get_mesh_detail);
	ClassDB::bind_method(D_METHOD("set_shore_wave_curl", "curl"), &PhysXWaterSurface3D::set_shore_wave_curl);
	ClassDB::bind_method(D_METHOD("get_shore_wave_curl"), &PhysXWaterSurface3D::get_shore_wave_curl);
	ClassDB::bind_method(D_METHOD("set_shore_wave_groups", "groups"), &PhysXWaterSurface3D::set_shore_wave_groups);
	ClassDB::bind_method(D_METHOD("get_shore_wave_groups"), &PhysXWaterSurface3D::get_shore_wave_groups);
	ClassDB::bind_method(D_METHOD("set_shore_spray", "enabled"), &PhysXWaterSurface3D::set_shore_spray);
	ClassDB::bind_method(D_METHOD("get_shore_spray"), &PhysXWaterSurface3D::get_shore_spray);
	ClassDB::bind_method(D_METHOD("set_shore_spray_amount", "amount"), &PhysXWaterSurface3D::set_shore_spray_amount);
	ClassDB::bind_method(D_METHOD("get_shore_spray_amount"), &PhysXWaterSurface3D::get_shore_spray_amount);
	ClassDB::bind_method(D_METHOD("get_breaker_stats"), &PhysXWaterSurface3D::get_breaker_stats);
	ClassDB::bind_method(D_METHOD("set_shore_wave_period", "period"), &PhysXWaterSurface3D::set_shore_wave_period);
	ClassDB::bind_method(D_METHOD("get_shore_wave_period"), &PhysXWaterSurface3D::get_shore_wave_period);
	ClassDB::bind_method(D_METHOD("set_shore_wave_direction", "direction"), &PhysXWaterSurface3D::set_shore_wave_direction);
	ClassDB::bind_method(D_METHOD("get_shore_wave_direction"), &PhysXWaterSurface3D::get_shore_wave_direction);
	ClassDB::bind_method(D_METHOD("set_wave_cascades", "count"), &PhysXWaterSurface3D::set_wave_cascades);
	ClassDB::bind_method(D_METHOD("get_wave_cascades"), &PhysXWaterSurface3D::get_wave_cascades);
	ClassDB::bind_method(D_METHOD("set_wave_cascade_ratio", "ratio"), &PhysXWaterSurface3D::set_wave_cascade_ratio);
	ClassDB::bind_method(D_METHOD("get_wave_cascade_ratio"), &PhysXWaterSurface3D::get_wave_cascade_ratio);
	ClassDB::bind_method(D_METHOD("get_wave_cascade_size", "index"), &PhysXWaterSurface3D::get_wave_cascade_size);
	ClassDB::bind_method(D_METHOD("set_wave_amplitude", "amplitude"), &PhysXWaterSurface3D::set_wave_amplitude);
	ClassDB::bind_method(D_METHOD("get_wave_amplitude"), &PhysXWaterSurface3D::get_wave_amplitude);
	ClassDB::bind_method(D_METHOD("set_cull_margin", "margin"), &PhysXWaterSurface3D::set_cull_margin);
	ClassDB::bind_method(D_METHOD("get_cull_margin"), &PhysXWaterSurface3D::get_cull_margin);
	ClassDB::bind_method(D_METHOD("set_fetch", "fetch"), &PhysXWaterSurface3D::set_fetch);
	ClassDB::bind_method(D_METHOD("get_fetch"), &PhysXWaterSurface3D::get_fetch);
	ClassDB::bind_method(D_METHOD("get_effective_fetch"), &PhysXWaterSurface3D::get_effective_fetch);
	ClassDB::bind_method(D_METHOD("set_current", "current"), &PhysXWaterSurface3D::set_current);
	ClassDB::bind_method(D_METHOD("get_current"), &PhysXWaterSurface3D::get_current);
	ClassDB::bind_method(D_METHOD("set_flow_period", "period"), &PhysXWaterSurface3D::set_flow_period);
	ClassDB::bind_method(D_METHOD("get_flow_period"), &PhysXWaterSurface3D::get_flow_period);
	ClassDB::bind_method(D_METHOD("sample_flow", "world_position"), &PhysXWaterSurface3D::sample_flow);
	ClassDB::bind_method(D_METHOD("get_flow_texture"), &PhysXWaterSurface3D::get_flow_texture);
	ClassDB::bind_method(D_METHOD("rebake_flow"), &PhysXWaterSurface3D::rebake_flow);
	ClassDB::bind_method(D_METHOD("set_live_flow", "enabled"), &PhysXWaterSurface3D::set_live_flow);
	ClassDB::bind_method(D_METHOD("is_live_flow"), &PhysXWaterSurface3D::is_live_flow);
	ClassDB::bind_method(D_METHOD("set_wall_friction", "friction"), &PhysXWaterSurface3D::set_wall_friction);
	ClassDB::bind_method(D_METHOD("get_wall_friction"), &PhysXWaterSurface3D::get_wall_friction);
	ClassDB::bind_method(D_METHOD("get_live_flow_texture"), &PhysXWaterSurface3D::get_live_flow_texture);
	ClassDB::bind_method(D_METHOD("set_flow_foam", "strength"), &PhysXWaterSurface3D::set_flow_foam);
	ClassDB::bind_method(D_METHOD("get_flow_foam"), &PhysXWaterSurface3D::get_flow_foam);
	ClassDB::bind_method(D_METHOD("set_flow_foam_persistence", "seconds"), &PhysXWaterSurface3D::set_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_flow_foam_persistence"), &PhysXWaterSurface3D::get_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_choppiness", "choppiness"), &PhysXWaterSurface3D::set_choppiness);
	ClassDB::bind_method(D_METHOD("get_choppiness"), &PhysXWaterSurface3D::get_choppiness);
	ClassDB::bind_method(D_METHOD("set_normal_mode", "mode"), &PhysXWaterSurface3D::set_normal_mode);
	ClassDB::bind_method(D_METHOD("get_normal_mode"), &PhysXWaterSurface3D::get_normal_mode);
	ClassDB::bind_method(D_METHOD("set_foam_enabled", "enabled"), &PhysXWaterSurface3D::set_foam_enabled);
	ClassDB::bind_method(D_METHOD("get_foam_enabled"), &PhysXWaterSurface3D::get_foam_enabled);
	ClassDB::bind_method(D_METHOD("set_foam_threshold", "threshold"), &PhysXWaterSurface3D::set_foam_threshold);
	ClassDB::bind_method(D_METHOD("get_foam_threshold"), &PhysXWaterSurface3D::get_foam_threshold);
	ClassDB::bind_method(D_METHOD("set_whitecap_auto", "enabled"), &PhysXWaterSurface3D::set_whitecap_auto);
	ClassDB::bind_method(D_METHOD("get_whitecap_auto"), &PhysXWaterSurface3D::get_whitecap_auto);
	ClassDB::bind_method(D_METHOD("set_whitecap_cover_scale", "scale"), &PhysXWaterSurface3D::set_whitecap_cover_scale);
	ClassDB::bind_method(D_METHOD("get_whitecap_cover_scale"), &PhysXWaterSurface3D::get_whitecap_cover_scale);
	ClassDB::bind_method(D_METHOD("get_whitecap_target_cover"), &PhysXWaterSurface3D::get_whitecap_target_cover);
	ClassDB::bind_method(D_METHOD("get_whitecap_cover"), &PhysXWaterSurface3D::get_whitecap_cover);
	ClassDB::bind_method(D_METHOD("get_effective_foam_threshold"), &PhysXWaterSurface3D::get_effective_foam_threshold);
	ClassDB::bind_method(D_METHOD("set_foam_persistence", "seconds"), &PhysXWaterSurface3D::set_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_foam_persistence"), &PhysXWaterSurface3D::get_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_shore_foam_band", "depth"), &PhysXWaterSurface3D::set_shore_foam_band);
	ClassDB::bind_method(D_METHOD("get_shore_foam_band"), &PhysXWaterSurface3D::get_shore_foam_band);
	ClassDB::bind_method(D_METHOD("set_open_sea_beyond", "open"), &PhysXWaterSurface3D::set_open_sea_beyond);
	ClassDB::bind_method(D_METHOD("set_sea_state", "wind_speed", "wind_direction", "wave_amplitude", "fetch"), &PhysXWaterSurface3D::set_sea_state);
	ClassDB::bind_method(D_METHOD("set_follow_weather", "enabled"), &PhysXWaterSurface3D::set_follow_weather);
	ClassDB::bind_method(D_METHOD("get_follow_weather"), &PhysXWaterSurface3D::get_follow_weather);
	ClassDB::bind_method(D_METHOD("recenter", "world_position"), &PhysXWaterSurface3D::recenter);
	ClassDB::bind_method(D_METHOD("remeasure_seabed"), &PhysXWaterSurface3D::remeasure_seabed);
	ClassDB::bind_method(D_METHOD("get_open_sea_beyond"), &PhysXWaterSurface3D::get_open_sea_beyond);
	ClassDB::bind_method(D_METHOD("set_shore_undertow", "speed"), &PhysXWaterSurface3D::set_shore_undertow);
	ClassDB::bind_method(D_METHOD("get_shore_undertow"), &PhysXWaterSurface3D::get_shore_undertow);
	ClassDB::bind_method(D_METHOD("set_swash_run_up", "height"), &PhysXWaterSurface3D::set_swash_run_up);
	ClassDB::bind_method(D_METHOD("get_swash_run_up"), &PhysXWaterSurface3D::get_swash_run_up);
	ClassDB::bind_method(D_METHOD("set_swash_drain_speed", "speed"), &PhysXWaterSurface3D::set_swash_drain_speed);
	ClassDB::bind_method(D_METHOD("get_swash_drain_speed"), &PhysXWaterSurface3D::get_swash_drain_speed);
	ClassDB::bind_method(D_METHOD("set_wet_sand_dry_time", "seconds"), &PhysXWaterSurface3D::set_wet_sand_dry_time);
	ClassDB::bind_method(D_METHOD("get_wet_sand_dry_time"), &PhysXWaterSurface3D::get_wet_sand_dry_time);

	ClassDB::bind_method(D_METHOD("set_surface_mesh", "mesh"), &PhysXWaterSurface3D::set_surface_mesh);
	ClassDB::bind_method(D_METHOD("get_surface_mesh"), &PhysXWaterSurface3D::get_surface_mesh);
	ClassDB::bind_method(D_METHOD("set_seabed_from_floor", "enabled"), &PhysXWaterSurface3D::set_seabed_from_floor);
	ClassDB::bind_method(D_METHOD("get_seabed_from_floor"), &PhysXWaterSurface3D::get_seabed_from_floor);
	ClassDB::bind_method(D_METHOD("set_seabed_collision_mask", "mask"), &PhysXWaterSurface3D::set_seabed_collision_mask);
	ClassDB::bind_method(D_METHOD("get_seabed_collision_mask"), &PhysXWaterSurface3D::get_seabed_collision_mask);
	ClassDB::bind_method(D_METHOD("set_shallow_fade_depth", "depth"), &PhysXWaterSurface3D::set_shallow_fade_depth);
	ClassDB::bind_method(D_METHOD("get_shallow_fade_depth"), &PhysXWaterSurface3D::get_shallow_fade_depth);
	ClassDB::bind_method(D_METHOD("set_render_extent", "extent"), &PhysXWaterSurface3D::set_render_extent);
	ClassDB::bind_method(D_METHOD("get_render_extent"), &PhysXWaterSurface3D::get_render_extent);
	ClassDB::bind_method(D_METHOD("set_water_material", "material"), &PhysXWaterSurface3D::set_water_material);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXWaterSurface3D::get_water_material);

	ClassDB::bind_method(D_METHOD("set_raster_reflections", "enabled"), &PhysXWaterSurface3D::set_raster_reflections);
	ClassDB::bind_method(D_METHOD("get_raster_reflections"), &PhysXWaterSurface3D::get_raster_reflections);
	ClassDB::bind_method(D_METHOD("set_underwater_enabled", "enabled"), &PhysXWaterSurface3D::set_underwater_enabled);
	ClassDB::bind_method(D_METHOD("get_underwater_enabled"), &PhysXWaterSurface3D::get_underwater_enabled);
	ClassDB::bind_method(D_METHOD("set_underwater_meniscus", "strength"), &PhysXWaterSurface3D::set_underwater_meniscus);
	ClassDB::bind_method(D_METHOD("get_underwater_meniscus"), &PhysXWaterSurface3D::get_underwater_meniscus);
	ClassDB::bind_method(D_METHOD("is_camera_underwater"), &PhysXWaterSurface3D::is_camera_underwater);
	ClassDB::bind_method(D_METHOD("set_caustics_enabled", "enabled"), &PhysXWaterSurface3D::set_caustics_enabled);
	ClassDB::bind_method(D_METHOD("get_caustics_enabled"), &PhysXWaterSurface3D::get_caustics_enabled);
	ClassDB::bind_method(D_METHOD("set_caustics_sun_direction", "direction"), &PhysXWaterSurface3D::set_caustics_sun_direction);
	ClassDB::bind_method(D_METHOD("get_caustics_sun_direction"), &PhysXWaterSurface3D::get_caustics_sun_direction);
	ClassDB::bind_method(D_METHOD("set_caustics_reference_depth", "depth"), &PhysXWaterSurface3D::set_caustics_reference_depth);
	ClassDB::bind_method(D_METHOD("get_caustics_reference_depth"), &PhysXWaterSurface3D::get_caustics_reference_depth);
	ClassDB::bind_method(D_METHOD("get_caustics_texture"), &PhysXWaterSurface3D::get_caustics_texture);
	ClassDB::bind_method(D_METHOD("get_caustics_light_right"), &PhysXWaterSurface3D::get_caustics_light_right);
	ClassDB::bind_method(D_METHOD("get_caustics_light_up"), &PhysXWaterSurface3D::get_caustics_light_up);
	ClassDB::bind_method(D_METHOD("get_caustics_origin"), &PhysXWaterSurface3D::get_caustics_origin);
	ClassDB::bind_method(D_METHOD("get_caustics_half_extent"), &PhysXWaterSurface3D::get_caustics_half_extent);
	ClassDB::bind_method(D_METHOD("get_caustics_tile_size"), &PhysXWaterSurface3D::get_caustics_tile_size);

	ClassDB::bind_method(D_METHOD("sample_height", "world_pos"), &PhysXWaterSurface3D::sample_height);
	ClassDB::bind_method(D_METHOD("sample_height_excluding_wake", "world_pos", "wake"), &PhysXWaterSurface3D::_sample_height_excluding_wake_bind);
	ClassDB::bind_method(D_METHOD("sample_vertical_speed", "world_pos"), &PhysXWaterSurface3D::sample_vertical_speed);
	ClassDB::bind_method(D_METHOD("is_wet", "world_pos"), &PhysXWaterSurface3D::is_wet);
	ClassDB::bind_method(D_METHOD("submit_sphere", "owner", "world_pos", "radius", "strength"), &PhysXWaterSurface3D::submit_sphere, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("clear_sphere", "owner"), &PhysXWaterSurface3D::clear_sphere);
	ClassDB::bind_method(D_METHOD("submit_impulse", "world_pos", "radius", "strength"), &PhysXWaterSurface3D::submit_impulse);
	ClassDB::bind_method(D_METHOD("set_inflow", "owner", "at", "direction", "width", "discharge"), &PhysXWaterSurface3D::set_inflow);
	ClassDB::bind_method(D_METHOD("clear_inflow", "owner"), &PhysXWaterSurface3D::clear_inflow);

	BIND_ENUM_CONSTANT(NORMAL_MODE_PER_PIXEL);
	BIND_ENUM_CONSTANT(NORMAL_MODE_PER_VERTEX);
	BIND_ENUM_CONSTANT(WAVE_SPREADING_COS2);
	BIND_ENUM_CONSTANT(WAVE_SPREADING_DONELAN_BANNER);
	BIND_ENUM_CONSTANT(WAVE_SPREADING_HASSELMANN);
	ClassDB::bind_method(D_METHOD("set_wave_spreading", "spreading"), &PhysXWaterSurface3D::set_wave_spreading);
	ClassDB::bind_method(D_METHOD("get_wave_spreading"), &PhysXWaterSurface3D::get_wave_spreading);
	ClassDB::bind_method(D_METHOD("set_swell", "swell"), &PhysXWaterSurface3D::set_swell);
	ClassDB::bind_method(D_METHOD("get_swell"), &PhysXWaterSurface3D::get_swell);

	ClassDB::bind_method(D_METHOD("set_step_mode", "mode"), &PhysXWaterSurface3D::set_step_mode);
	ClassDB::bind_method(D_METHOD("get_step_mode"), &PhysXWaterSurface3D::get_step_mode);
	ClassDB::bind_method(D_METHOD("set_ripple_rest_threshold", "threshold"), &PhysXWaterSurface3D::set_ripple_rest_threshold);
	ClassDB::bind_method(D_METHOD("get_ripple_rest_threshold"), &PhysXWaterSurface3D::get_ripple_rest_threshold);
	ClassDB::bind_method(D_METHOD("is_ripple_resting"), &PhysXWaterSurface3D::is_ripple_resting);
	BIND_ENUM_CONSTANT(STEP_MODE_PER_TICK);
	BIND_ENUM_CONSTANT(STEP_MODE_PER_FRAME);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "surface_mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_surface_mesh", "get_surface_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "domain_size"), "set_domain_size", "get_domain_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "grid_resolution"), "set_grid_resolution", "get_grid_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_level"), "set_water_level", "get_water_level");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_amplitude", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_ripple_amplitude", "get_ripple_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "step_mode", PROPERTY_HINT_ENUM, "Per Tick,Per Frame"), "set_step_mode", "get_step_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_rest_threshold", PROPERTY_HINT_RANGE, "0,0.05,0.0001,or_greater,suffix:m"), "set_ripple_rest_threshold", "get_ripple_rest_threshold");

	ADD_GROUP("Shore waves", "shore_wave");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shore_waves"), "set_shore_waves", "get_shore_waves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_wave_height", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater,suffix:m"), "set_shore_wave_height", "get_shore_wave_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_wave_period", PROPERTY_HINT_RANGE, "1,30,0.1,suffix:s"), "set_shore_wave_period", "get_shore_wave_period");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_wave_curl", PROPERTY_HINT_RANGE, "0,1.5,0.01"), "set_shore_wave_curl", "get_shore_wave_curl");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_wave_groups", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_shore_wave_groups", "get_shore_wave_groups");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shore_spray"), "set_shore_spray", "get_shore_spray");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shore_spray_amount", PROPERTY_HINT_RANGE, "16,65536,1"), "set_shore_spray_amount", "get_shore_spray_amount");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "shore_wave_direction"), "set_shore_wave_direction", "get_shore_wave_direction");
	ADD_GROUP("Seabed", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "seabed_from_floor"), "set_seabed_from_floor", "get_seabed_from_floor");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seabed_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_seabed_collision_mask", "get_seabed_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shallow_fade_depth", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_shallow_fade_depth", "get_shallow_fade_depth");

	ADD_GROUP("Ocean", "ocean_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ocean_grid_resolution"), "set_ocean_grid_resolution", "get_ocean_grid_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_detail", PROPERTY_HINT_RANGE, "1,4,1"), "set_mesh_detail", "get_mesh_detail");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "ocean_domain_size"), "set_ocean_domain_size", "get_ocean_domain_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_speed"), "set_wind_speed", "get_wind_speed");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "wind_direction"), "set_wind_direction", "get_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_weather"), "set_follow_weather", "get_follow_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_amplitude"), "set_wave_amplitude", "get_wave_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "wave_cascades", PROPERTY_HINT_RANGE, "1,4,1"), "set_wave_cascades", "get_wave_cascades");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_cascade_ratio", PROPERTY_HINT_RANGE, "2,16,0.1"), "set_wave_cascade_ratio", "get_wave_cascade_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fetch", PROPERTY_HINT_RANGE, "0,100000,1,or_greater,suffix:m"), "set_fetch", "get_fetch");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "wave_spreading", PROPERTY_HINT_ENUM, "Cos2,Donelan-Banner,Hasselmann"), "set_wave_spreading", "get_wave_spreading");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "swell", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_swell", "get_swell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "choppiness", PROPERTY_HINT_RANGE, "0,3,0.01,or_greater"), "set_choppiness", "get_choppiness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "foam_enabled"), "set_foam_enabled", "get_foam_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foam_threshold", PROPERTY_HINT_RANGE, "0,1.5,0.01"), "set_foam_threshold", "get_foam_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "whitecap_auto"), "set_whitecap_auto", "get_whitecap_auto");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "whitecap_cover_scale", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_whitecap_cover_scale", "get_whitecap_cover_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_foam_band", PROPERTY_HINT_RANGE, "0,5,0.05,or_greater,suffix:m"), "set_shore_foam_band", "get_shore_foam_band");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "open_sea_beyond"), "set_open_sea_beyond", "get_open_sea_beyond");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_undertow", PROPERTY_HINT_RANGE, "0,1.5,0.01,or_greater,suffix:m/s"), "set_shore_undertow", "get_shore_undertow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foam_persistence", PROPERTY_HINT_RANGE, "0.05,20,0.05,or_greater,suffix:s"), "set_foam_persistence", "get_foam_persistence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "swash_run_up", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:m"), "set_swash_run_up", "get_swash_run_up");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "swash_drain_speed", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater,suffix:m/s"), "set_swash_drain_speed", "get_swash_drain_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_sand_dry_time", PROPERTY_HINT_RANGE, "0.1,60,0.1,or_greater,suffix:s"), "set_wet_sand_dry_time", "get_wet_sand_dry_time");

	ADD_GROUP("Flow", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "current", PROPERTY_HINT_NONE, "suffix:m/s"), "set_current", "get_current");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_period", PROPERTY_HINT_RANGE, "0.25,10,0.05,or_greater,suffix:s"), "set_flow_period", "get_flow_period");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_flow"), "set_live_flow", "is_live_flow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_friction", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater,suffix:1/s"), "set_wall_friction", "get_wall_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam", PROPERTY_HINT_RANGE, "0,4,0.05,or_greater"), "set_flow_foam", "get_flow_foam");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam_persistence", PROPERTY_HINT_RANGE, "0.1,30,0.1,or_greater,suffix:s"), "set_flow_foam_persistence", "get_flow_foam_persistence");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "render_extent", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:m"), "set_render_extent", "get_render_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull_margin", PROPERTY_HINT_RANGE, "0,500,0.1,or_greater,suffix:m"), "set_cull_margin", "get_cull_margin");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "normal_mode", PROPERTY_HINT_ENUM, "Per Pixel,Per Vertex"), "set_normal_mode", "get_normal_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "water_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_water_material", "get_water_material");

	ADD_GROUP("Underwater", "underwater_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "raster_reflections"), "set_raster_reflections", "get_raster_reflections");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "underwater_enabled"), "set_underwater_enabled", "get_underwater_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "underwater_meniscus", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_underwater_meniscus", "get_underwater_meniscus");

	ADD_GROUP("Caustics", "caustic");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "caustics_enabled"), "set_caustics_enabled", "get_caustics_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "caustics_sun_direction"), "set_caustics_sun_direction", "get_caustics_sun_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "caustics_reference_depth", PROPERTY_HINT_RANGE, "0.1,50,0.1,or_greater"), "set_caustics_reference_depth", "get_caustics_reference_depth");
}

void PhysXWaterSurface3D::set_step_mode(StepMode p_mode) {
	if (p_mode == step_mode) {
		return;
	}
	step_mode = p_mode;
	if (step_mode == STEP_MODE_PER_TICK && solver.has_queued_ticks()) {
		_frame_update(); // the ticks recorded so far still run
	}
}

void PhysXWaterSurface3D::set_ripple_rest_threshold(float p_threshold) {
	ripple_rest_threshold = MAX(p_threshold, 0.0f);
	solver.set_rest_threshold(ripple_rest_threshold);
}

void PhysXWaterSurface3D::set_domain_size(Vector2 p_size) {
	domain_size = p_size;
	_rebuild();
}

void PhysXWaterSurface3D::set_grid_resolution(int p_n) {
	grid_resolution = MAX(p_n, 4);
	_rebuild();
}

void PhysXWaterSurface3D::set_depth(float p_depth) {
	depth = MAX(p_depth, 0.1f);
	_rebuild();
}

void PhysXWaterSurface3D::set_water_level(float p_level) {
	water_level = p_level;
	_rebuild();
}

void PhysXWaterSurface3D::set_damping(float p_damping) {
	damping = p_damping;
	_rebuild();
}

void PhysXWaterSurface3D::set_ripple_amplitude(float p_amplitude) {
	ripple_amplitude = MAX(p_amplitude, 0.0f);
	_rebuild();
}

void PhysXWaterSurface3D::set_ocean_grid_resolution(int p_n) {
	// Must stay a power of two -- the Stockham FFT passes require it (see
	// water_solver.h). Round up rather than silently misbehave.
	int n = MAX(p_n, 4);
	int pow2 = 4;
	while (pow2 < n) {
		pow2 <<= 1;
	}
	ocean_grid_resolution = pow2;
	_rebuild();
}

void PhysXWaterSurface3D::set_ocean_domain_size(Vector2 p_size) {
	ocean_domain_size = p_size;
	_rebuild();
}

void PhysXWaterSurface3D::set_wind_speed(float p_speed) {
	wind_speed = MAX(p_speed, 0.01f);
	_reinit_spectrum();
}

void PhysXWaterSurface3D::set_wind_direction(Vector2 p_dir) {
	wind_direction = p_dir;
	_reinit_spectrum();
}

void PhysXWaterSurface3D::set_wave_amplitude(float p_amp) {
	wave_amplitude = MAX(p_amp, 0.0f);
	_reinit_spectrum();
}

void PhysXWaterSurface3D::set_wave_cascades(int p_count) {
	wave_cascades = CLAMP(p_count, 1, MAX_WAVE_CASCADES);
	_rebuild();
}

void PhysXWaterSurface3D::set_wave_cascade_ratio(float p_ratio) {
	wave_cascade_ratio = CLAMP(p_ratio, 2.0f, 16.0f);
	_rebuild();
}

Vector2 PhysXWaterSurface3D::get_wave_cascade_size(int p_index) const {
	return ocean_domain_size * Math::pow(wave_cascade_ratio, (float)CLAMP(p_index, 0, MAX_WAVE_CASCADES - 1));
}

// Cascade i keeps the waves shorter than a quarter of its tile; longer ones go to
// the next, larger tile (whose own Nyquist limit, pi N / (ratio L), stays above
// that for N >= 32 at ratio <= 8), so no tile shows its few longest, tiling waves.
float PhysXWaterSurface3D::_cascade_split(int p_index) const {
	const Vector2 size = get_wave_cascade_size(p_index);
	return 4.0f * Math::TAU / MIN(size.x, size.y);
}

void PhysXWaterSurface3D::_clear_cascades() {
	for (WaveCascade *c : extra_cascades) {
		memdelete(c);
	}
	extra_cascades.clear();
}

// Ocean-only solvers for cascades 1.. : the same sea state on a larger tile and
// its band of the spectrum; their ripple layer is a tiny grid left resting.
void PhysXWaterSurface3D::_configure_cascades(const WaterSolver::Settings &p_base) {
	_clear_cascades();
	for (int i = 1; i < wave_cascades; i++) {
		WaveCascade *c = memnew(WaveCascade);
		WaterSolver::Settings s = p_base;
		s.grid_resolution = 8;
		s.domain_size = Vector2(8, 8);
		s.cell_depth = PackedFloat32Array();
		s.caustics_enabled = false;
		s.ocean_domain_size = get_wave_cascade_size(i);
		s.ocean_k_max = _cascade_split(i - 1);
		s.ocean_k_min = i + 1 < wave_cascades ? _cascade_split(i) : 0.0f;
		c->domain = s.ocean_domain_size;
		c->solver.set_rest_threshold(1e-3f);
		c->solver.configure(s);
		c->solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
		extra_cascades.push_back(c);
	}
}

PhysXWaterSurface3D::~PhysXWaterSurface3D() {
	_clear_cascades();
}

void PhysXWaterSurface3D::set_shore_waves(bool p_enabled) {
	shore_waves = p_enabled;
	_bake_shore_waves();
}

void PhysXWaterSurface3D::set_shore_wave_height(float p_height) {
	shore_wave_height = MAX(p_height, 0.0f);
	_bake_shore_waves();
}

void PhysXWaterSurface3D::set_shore_wave_period(float p_period) {
	shore_wave_period = CLAMP(p_period, 1.0f, 30.0f);
	_bake_shore_waves();
}

void PhysXWaterSurface3D::set_shore_wave_curl(float p_curl) {
	shore_wave_curl = CLAMP(p_curl, 0.0f, 1.5f);
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("shore_curl", shore_wave_curl);
	}
}

void PhysXWaterSurface3D::set_shore_wave_groups(float p_groups) {
	shore_wave_groups = CLAMP(p_groups, 0.0f, 1.0f);
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("shore_groups", shore_wave_groups);
	}
}

void PhysXWaterSurface3D::set_shore_wave_direction(Vector2 p_direction) {
	shore_wave_direction = p_direction;
	_bake_shore_waves();
}

static float _flow_value_noise(float p_x, float p_y);

// Wavenumber for angular frequency w in depth h: w^2 = g k tanh(k h), by
// Fenton and McKee's (1990) explicit approximation (within 1.5%).
static float _shore_k(float p_omega, float p_h) {
	const float k_deep = p_omega * p_omega / 9.81f;
	const float x = k_deep * MAX(p_h, 1e-3f);
	return k_deep / Math::pow(Math::tanh(Math::pow(x, 0.75f)), 2.0f / 3.0f);
}

// Group velocity in depth h.
static float _shore_cg(float p_omega, float p_h) {
	const float k = _shore_k(p_omega, p_h);
	const float kh = MIN(k * MAX(p_h, 1e-3f), 20.0f);
	return 0.5f * (p_omega / k) * (1.0f + 2.0f * kh / Math::sinh(2.0f * kh));
}

// Shore waves over the measured seabed, per ripple cell. The phase obeys the
// eikonal equation of ray theory, |grad phase| = k(h), solved by fast sweeping
// (Zhao 2005) from the edges the waves come in through, so the crests slow and
// turn to follow the depth contours (refraction) and bunch up in the shallows.
// The height follows energy flux (shoaling, H = H0 sqrt(cg0 / cg)), capped at
// the breaker limit 0.78 h; past the point where it reaches it the wave is a
// breaking bore. Where the water is deeper than a quarter wavelength the
// shore waves fade out and the FFT ocean carries the sea alone.
void PhysXWaterSurface3D::_bake_shore_waves() {
	const int n = grid_resolution;
	if (!shore_waves || cell_depth.size() != n * n || n < 4) {
		for (int t = 0; t < SHORE_TRAINS; t++) {
			shore_phase[t].clear();
			shore_height[t].clear();
			shore_grad_x[t].clear();
			shore_grad_z[t].clear();
			shore_stage[t].clear();
		}
		shore_iribarren.clear();
		_update_shore_material();
		return;
	}
	const Vector2 cell = active_domain_size / (real_t)n;
	const float h_cell = 0.5f * (cell.x + cell.y);
	Vector2 dir0 = shore_wave_direction.length_squared() > 1e-8f ? shore_wave_direction.normalized() : wind_direction.normalized();
	if (dir0.length_squared() < 1e-8f) {
		dir0 = Vector2(1, 0);
	}
	const float *dw = cell_depth.ptr();
	for (int t = 0; t < SHORE_TRAINS; t++) {
		// The second train: a little longer, lower and from a few degrees round,
		// so the two beat into sets.
		const float period = shore_wave_period * (t == 0 ? 1.0f : 1.13f);
		const float h0 = shore_wave_height * (t == 0 ? 1.0f : 0.6f);
		const Vector2 dir = t == 0 ? dir0 : dir0.rotated(0.14f);
		const float omega = Math::TAU / period;
		shore_omega[t] = omega;
		const float k_deep = omega * omega / 9.81f;
		const float cg_deep = 9.81f / (2.0f * omega);
		Vector<float> &phase = shore_phase[t];
		phase.resize(n * n);
		float *pw = phase.ptrw();
		LocalVector<float> k_cell;
		k_cell.resize(n * n);
		for (int i = 0; i < n * n; i++) {
			pw[i] = Math::INF;
			k_cell[i] = dw[i] > 0.02f ? _shore_k(omega, dw[i]) : 0.0f;
		}
		// Sources: wet cells on the edges the waves come in through, with the
		// incoming plane wave's phase.
		const Vector2 grid_min = -active_domain_size * 0.5f;
		auto pos = [&](int x, int z) { return grid_min + Vector2(x + 0.5f, z + 0.5f) * cell; };
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				if (k_cell[z * n + x] <= 0.0f) {
					continue;
				}
				const bool in = (x == 0 && dir.x > 0.05f) || (x == n - 1 && dir.x < -0.05f) || (z == 0 && dir.y > 0.05f) || (z == n - 1 && dir.y < -0.05f);
				if (in) {
					pw[z * n + x] = k_deep * pos(x, z).dot(dir);
				}
			}
		}
		// Fast sweeping: four orderings, a few rounds.
		for (int round = 0; round < 3; round++) {
			for (int sweep = 0; sweep < 4; sweep++) {
				const int x0 = (sweep & 1) ? n - 1 : 0;
				const int dx = (sweep & 1) ? -1 : 1;
				const int z0 = (sweep & 2) ? n - 1 : 0;
				const int dz = (sweep & 2) ? -1 : 1;
				for (int zi = 0, z = z0; zi < n; zi++, z += dz) {
					for (int xi = 0, x = x0; xi < n; xi++, x += dx) {
						const int i = z * n + x;
						const float f = k_cell[i] * h_cell;
						if (f <= 0.0f) {
							continue;
						}
						const float a = MIN(x > 0 ? pw[i - 1] : Math::INF, x < n - 1 ? pw[i + 1] : Math::INF);
						const float b = MIN(z > 0 ? pw[i - n] : Math::INF, z < n - 1 ? pw[i + n] : Math::INF);
						if (!Math::is_finite(a) && !Math::is_finite(b)) {
							continue;
						}
						float v;
						if (!Math::is_finite(a) || !Math::is_finite(b) || Math::abs(a - b) >= f) {
							v = MIN(a, b) + f;
						} else {
							v = 0.5f * (a + b + Math::sqrt(2.0f * f * f - (a - b) * (a - b)));
						}
						if (v < pw[i]) {
							pw[i] = v;
						}
					}
				}
			}
		}
		// Dry cells (and any the waves can't reach) take their wet neighbours'
		// phase, so the texture blends smoothly up to the waterline.
		for (int pass = 0; pass < 6; pass++) {
			Vector<float> next = phase;
			float *nw = next.ptrw();
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int i = z * n + x;
					if (Math::is_finite(pw[i])) {
						continue;
					}
					float sum = 0.0f;
					int used = 0;
					const int nb[4] = { x > 0 ? i - 1 : -1, x < n - 1 ? i + 1 : -1, z > 0 ? i - n : -1, z < n - 1 ? i + n : -1 };
					for (int j : nb) {
						if (j >= 0 && Math::is_finite(pw[j])) {
							sum += pw[j];
							used++;
						}
					}
					if (used > 0) {
						nw[i] = sum / used;
					}
				}
			}
			phase = next;
			pw = phase.ptrw();
		}
		for (int i = 0; i < n * n; i++) {
			if (!Math::is_finite(pw[i])) {
				pw[i] = 0.0f;
			}
		}
		// Gradient (k times the direction of travel) and height.
		Vector<float> &gx = shore_grad_x[t];
		Vector<float> &gz = shore_grad_z[t];
		Vector<float> &hh = shore_height[t];
		Vector<float> &st = shore_stage[t];
		gx.resize(n * n);
		gz.resize(n * n);
		hh.resize(n * n);
		st.resize(n * n);
		const float wavelength_deep = Math::TAU / k_deep;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				const int xl = MAX(x - 1, 0), xr = MIN(x + 1, n - 1);
				const int zl = MAX(z - 1, 0), zr = MIN(z + 1, n - 1);
				Vector2 g((pw[z * n + xr] - pw[z * n + xl]) / ((xr - xl) * cell.x), (pw[zr * n + x] - pw[zl * n + x]) / ((zr - zl) * cell.y));
				// Keep |g| = k where wet (finite differences across the waterline aren't).
				const float k = k_cell[i] > 0.0f ? k_cell[i] : k_deep;
				g = g.length_squared() > 1e-10f ? g.normalized() * k : dir * k;
				gx.write[i] = g.x;
				gz.write[i] = g.y;
				const float h = dw[i];
				float height = 0.0f;
				// Past the break point the wave is a bore: stage 3 on dry sand.
				float stage = 3.0f;
				if (h > 0.02f) {
					const float shoal = Math::sqrt(cg_deep / MAX(_shore_cg(omega, h), 1e-3f));
					// Deep water: the FFT ocean's swell, not these.
					const float deep_fade = 1.0f - Math::smoothstep(0.25f * wavelength_deep, 0.5f * wavelength_deep, h);
					const float unbroken = h0 * shoal * deep_fade;
					height = MIN(unbroken, BREAKER_INDEX * h);
					stage = unbroken / (BREAKER_INDEX * h);
				}
				hh.write[i] = height;
				st.write[i] = stage;
			}
		}
	}
	// Surf similarity (Iribarren) number per cell, xi = tan(beta) / sqrt(H0 / L0),
	// from the beach slope over the cell and its neighbours (Battjes 1974).
	{
		const float l0 = 9.81f * shore_wave_period * shore_wave_period / Math::TAU;
		const float steep = Math::sqrt(MAX(shore_wave_height, 0.01f) / MAX(l0, 0.1f));
		LocalVector<float> slope;
		slope.resize(n * n);
		auto dep = [&](int x, int z) {
			const float d = dw[CLAMP(z, 0, n - 1) * n + CLAMP(x, 0, n - 1)];
			return d < WaterSolver::WALL_DEPTH * 0.5f ? 0.0f : CLAMP(d, -1.0f, 50.0f);
		};
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const float sx = (dep(x + 1, z) - dep(x - 1, z)) / (2.0f * cell.x);
				const float sz = (dep(x, z + 1) - dep(x, z - 1)) / (2.0f * cell.y);
				slope[z * n + x] = Math::sqrt(sx * sx + sz * sz);
			}
		}
		shore_iribarren.resize(n * n);
		float *xw = shore_iribarren.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				float sum = 0.0f;
				int used = 0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, zz = z + dz;
						if (xx >= 0 && xx < n && zz >= 0 && zz < n) {
							sum += slope[zz * n + xx];
							used++;
						}
					}
				}
				xw[z * n + x] = (sum / MAX(used, 1)) / steep;
			}
		}
	}
	_update_shore_material();
}

void PhysXWaterSurface3D::_update_shore_material() {
	if (water_material.is_null()) {
		return;
	}
	const int n = grid_resolution;
	const bool on = shore_waves && shore_phase[0].size() == n * n && shore_phase[1].size() == n * n && shore_iribarren.size() == n * n;
	water_material->set_shader_parameter("shore_waves_enabled", on);
	if (!on) {
		solver.set_shore_waves(RID(), false, Vector2(), 0.0f, 0.0f);
		return;
	}
	Vector<uint8_t> a_bytes;
	Vector<uint8_t> b_bytes;
	Vector<uint8_t> c_bytes;
	a_bytes.resize(n * n * 16);
	b_bytes.resize(n * n * 16);
	c_bytes.resize(n * n * 16);
	float *a = (float *)a_bytes.ptrw();
	float *b = (float *)b_bytes.ptrw();
	float *cc = (float *)c_bytes.ptrw();
	for (int i = 0; i < n * n; i++) {
		a[i * 4 + 0] = shore_phase[0][i];
		a[i * 4 + 1] = shore_height[0][i];
		a[i * 4 + 2] = shore_phase[1][i];
		a[i * 4 + 3] = shore_height[1][i];
		b[i * 4 + 0] = shore_grad_x[0][i];
		b[i * 4 + 1] = shore_grad_z[0][i];
		b[i * 4 + 2] = shore_grad_x[1][i];
		b[i * 4 + 3] = shore_grad_z[1][i];
		cc[i * 4 + 0] = shore_stage[0][i];
		cc[i * 4 + 1] = shore_stage[1][i];
		cc[i * 4 + 2] = shore_iribarren[i];
		cc[i * 4 + 3] = 0.0f;
	}
	Ref<Image> ia = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, a_bytes);
	Ref<Image> ib = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, b_bytes);
	Ref<Image> ic = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, c_bytes);
	if (shore_wave_tex_a.is_valid() && shore_wave_tex_a->get_width() == n) {
		shore_wave_tex_a->update(ia);
		shore_wave_tex_b->update(ib);
		shore_wave_tex_c->update(ic);
	} else {
		shore_wave_tex_a = ImageTexture::create_from_image(ia);
		shore_wave_tex_b = ImageTexture::create_from_image(ib);
		shore_wave_tex_c = ImageTexture::create_from_image(ic);
	}
	water_material->set_shader_parameter("shore_wave_tex_a", shore_wave_tex_a);
	water_material->set_shader_parameter("shore_wave_tex_b", shore_wave_tex_b);
	water_material->set_shader_parameter("shore_wave_tex_c", shore_wave_tex_c);
	water_material->set_shader_parameter("shore_curl", shore_wave_curl);
	water_material->set_shader_parameter("shore_groups", shore_wave_groups);
	water_material->set_shader_parameter("shore_wave_omega", Vector2(shore_omega[0], shore_omega[1]));
	// The swash runs the breaking waves up the sand: it reads the same
	// phases and heights, on the render thread.
	const float l0 = 9.81f * shore_wave_period * shore_wave_period / Math::TAU;
	solver.set_shore_waves(RS::get_singleton()->texture_get_rd_texture(shore_wave_tex_a->get_rid()), true,
			Vector2(shore_omega[0], shore_omega[1]), shore_wave_height, l0);
}

// As the shader's shore_wave() (height only, at the resting point: the shore
// waves' horizontal sway is left out here).
// The material's shore_front_warp().
static float _shore_front_warp(float p_u, float p_back, float &r_dps) {
	const float uw = p_u - (float)Math::TAU * Math::floor((p_u + (float)Math::PI) / (float)Math::TAU);
	const float uc = (float)Math::PI * (2.0f * p_back - 1.0f);
	if (uw < uc) {
		r_dps = 0.5f / p_back;
		return -(float)Math::PI + (uw + (float)Math::PI) * r_dps;
	}
	r_dps = 0.5f / (1.0f - p_back);
	return (uw - uc) * r_dps;
}

// The material's shore_wave_set(): this wave's height over the train's (rms) height.
float PhysXWaterSurface3D::_shore_wave_set(float p_theta, int p_train) const {
	static const float freq[5] = { -0.446f, -0.207f, 0.025f, 0.295f, 0.490f };
	static const float phase[5] = { 0.0f, 2.1f, 4.4f, 1.3f, 5.6f };
	static const float seed[5] = { 1.7f, 3.1f, 0.7f, 2.3f, 4.9f };
	const float x = p_theta * 0.159154943f;
	float re = 0.0f, im = 0.0f;
	for (int j = 0; j < 5; j++) {
		const float a = freq[j] * x + phase[j] + seed[j] * p_train;
		re += Math::cos(a);
		im += Math::sin(a);
	}
	return Math::lerp(1.0f, Math::sqrt(re * re + im * im) * 0.4472136f, shore_wave_groups);
}

// The material's shore_per_wave().
static void _shore_per_wave(float p_r, float &r_height, float &r_stage) {
	const float sb = r_stage;
	if (sb < 2.99f) {
		r_height *= MIN(p_r * sb, 1.0f) / MAX(MIN(sb, 1.0f), 1e-3f);
		r_stage = p_r * sb;
	}
}

float PhysXWaterSurface3D::_shore_eta_at(float p_x, float p_z, float *r_rate) const {
	if (r_rate != nullptr) {
		*r_rate = 0.0f;
	}
	const int n = grid_resolution;
	if (!shore_waves || shore_phase[0].size() != n * n || shore_phase[1].size() != n * n) {
		return 0.0f;
	}
	const float inside = 1.0f - Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(p_x / active_domain_size.x), Math::abs(p_z / active_domain_size.y)));
	if (inside <= 0.0f) {
		return 0.0f;
	}
	const float depth = MAX(_bilinear_sample(cell_depth, n, active_domain_size, p_x, p_z), 0.0f);
	const float wx = p_x + grid_center.x, wz = p_z + grid_center.y;
	const float time = (float)Math::fmod(water_time, 3600.0);
	const float vary = 0.55f + 0.45f * _flow_value_noise(wx * 0.04f + time * 0.03f, wz * 0.04f);
	float eta = 0.0f;
	float rate = 0.0f;
	for (int t = 0; t < SHORE_TRAINS; t++) {
		float height = _bilinear_sample(shore_height[t], n, active_domain_size, p_x, p_z);
		if (height <= 1e-4f) {
			continue;
		}
		const float phi = _bilinear_sample(shore_phase[t], n, active_domain_size, p_x, p_z);
		const float th = phi - shore_omega[t] * time;
		const float r = _shore_wave_set(th, t);
		const bool staged = shore_stage[t].size() == n * n && shore_iribarren.size() == n * n;
		float stage_raw = staged ? _bilinear_sample(shore_stage[t], n, active_domain_size, p_x, p_z) : 0.0f;
		_shore_per_wave(r, height, stage_raw);
		const float brk = Math::smoothstep(0.8f, 0.98f, height / MAX(BREAKER_INDEX * depth, 1e-3f));
		const float amp = 0.5f * height * vary * inside;
		float skew = 0.6f * brk;
		float drop_w = 0.0f;
		float front = 0.5f;
		if (staged) {
			const float xi = _bilinear_sample(shore_iribarren, n, active_domain_size, p_x, p_z) / Math::sqrt(MAX(r, 0.05f));
			const float plunging = Math::smoothstep(0.35f, 0.6f, xi) * (1.0f - Math::smoothstep(2.6f, 3.3f, xi)) * shore_wave_curl;
			const float stage = CLAMP((stage_raw - 0.95f) / 1.0f, 0.0f, 1.0f);
			const float throw_w = plunging * vary * Math::smoothstep(0.0f, 0.45f, stage) * (1.0f - Math::smoothstep(0.7f, 1.0f, stage));
			drop_w = plunging * vary * Math::smoothstep(0.3f, 0.75f, stage) * (1.0f - Math::smoothstep(0.8f, 1.0f, stage));
			skew = MIN(skew + 0.3f * throw_w, 0.92f);
			const float steep = plunging * vary * Math::smoothstep(0.0f, 0.3f, stage) * (1.0f - 0.5f * Math::smoothstep(0.8f, 1.0f, stage));
			front = Math::lerp(0.5f, CLAMP(Vector2(_bilinear_sample(shore_grad_x[t], n, active_domain_size, p_x, p_z), _bilinear_sample(shore_grad_z[t], n, active_domain_size, p_x, p_z)).length() * 3.0f * amp / (float)Math::TAU, 0.02f, 0.5f), CLAMP(steep, 0.0f, 1.0f));
		}
		float dps;
		const float ps = _shore_front_warp(th - skew * Math::cos(th), 1.0f - front, dps);
		const float sharp = 1.0f + 1.5f * brk;
		const float base = MAX(0.5f + 0.5f * Math::cos(ps), 0.0f);
		eta += amp * 2.0f * (Math::pow(base, sharp) - 1.0f / Math::sqrt(Math::PI * (sharp + 0.25f)));
		{
			const float pc = ps - Math::TAU * Math::floor((ps + Math::PI) / Math::TAU);
			const float q = (pc - 0.35f) / 0.35f;
			eta -= 1.6f * amp * drop_w * Math::exp(-q * q);
		}
		// d/dt of amp cos(th - skew cos th), th' = -omega.
		rate += amp * sharp * Math::pow(MAX(base, 1e-4f), sharp - 1.0f) * Math::sin(ps) * dps * (1.0f + skew * Math::sin(th)) * shore_omega[t];
	}
	if (r_rate != nullptr) {
		*r_rate = rate;
	}
	return eta;
}

// Plunging lips (docs/ideas/water.md "Step 5e"), after Thurey, Muller-Fischer, Schirm and Gross, Real-time
// Breaking Waves for Shallow Water Simulations (2007): the steep fronts are found as lines, and the lip is a sheet
// of particles launched from each line point, joined into a closed mesh of two offset layers. Here the fronts are
// the crests of the shore waves (where a train's phase, skewed as the material skews it, is a multiple of 2 pi)
// inside the plunging part of the surf zone, found by marching squares over the ripple grid every tick. A lip
// point launched tau seconds ago, seen from the crest, is ballistic: forward at the jet speed u relative to the
// crest, down by g tau^2 / 2, so each crest point carries the sheet's cross-section as a parabola out to where it
// lands on the water ahead. Spray follows Ihmsen, Akinci, Akinci and Teschner, Unified Spray, Foam and Bubbles
// for Particle-Based Fluids (2012): each piece of lip emits droplets in proportion to its kinetic energy times
// its wave-crest potential (the lip's edge, curvature times speed) and trapped-air potential (the impact), each
// mapped to 0..1 by their clamp Phi.

// Ihmsen et al. (2012), eq. 1.
static float _ihmsen_phi(float p_value, float p_min, float p_max) {
	return (MIN(p_value, p_max) - MIN(p_value, p_min)) / (p_max - p_min);
}

// +1 when a triangle (a, b, c) with (b - a) x (c - a) along its normal is front facing, as PlaneMesh's up faces.
static float _front_winding() {
	static float sign = 0.0f;
	if (sign == 0.0f) {
		Ref<PlaneMesh> ref_plane;
		ref_plane.instantiate();
		const Vector<Face3> faces = ref_plane->get_faces();
		sign = 1.0f;
		if (!faces.is_empty()) {
			const Face3 &f = faces[0];
			sign = (f.vertex[1] - f.vertex[0]).cross(f.vertex[2] - f.vertex[0]).y >= 0.0f ? 1.0f : -1.0f;
		}
	}
	return sign;
}

// The lips' material: the built-in water shader compiled with BREAKER_LIP and the surface material's
// parameters, so it shades like the water but never moves its vertices (the path tracer then reads the mesh
// directly instead of capturing it). A custom water_material draws the lips itself (vertex colour alpha 0).
Ref<Material> PhysXWaterSurface3D::_breaker_material() {
	if (water_material.is_null() || water_material->get_shader().is_null() || water_material->get_shader()->get_code() != String(WATER_SHADER_SRC)) {
		return water_material;
	}
	if (breaker_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(String("#define BREAKER_LIP\n") + WATER_SHADER_SRC);
		breaker_material.instantiate();
		breaker_material->set_shader(shader);
		List<PropertyInfo> uniforms;
		water_material->get_shader()->get_shader_uniform_list(&uniforms);
		breaker_params.clear();
		for (const PropertyInfo &pi : uniforms) {
			breaker_params.push_back(pi.name);
		}
	}
	for (const StringName &name : breaker_params) {
		const Variant v = water_material->get_shader_parameter(name);
		if (v.get_type() != Variant::NIL && v != breaker_material->get_shader_parameter(name)) {
			breaker_material->set_shader_parameter(name, v);
		}
	}
	return breaker_material;
}

void PhysXWaterSurface3D::_hide_breakers() {
	if (breaker_instance != nullptr) {
		breaker_instance->set_visible(false);
	}
	if (breaker_spray != nullptr) {
		breaker_spray->set_emitting(false);
	}
	breaker_stat_points = 0;
	breaker_stat_vertices = 0;
	breaker_stat_triangles = 0;
	breaker_stat_spray_rate = 0.0f;
}

// Runs a lambda over a WorkerThreadPool group (the pool calls a member function).
template <typename F>
struct _LipJob {
	F *fn = nullptr;
	void run(uint32_t p_index, void *p_unused) { (*fn)(p_index); }
};

// Appends into a slice of a mesh array that a lip piece owns.
template <typename T>
struct _LipSlice {
	T *ptr = nullptr;
	int count = 0;
	_FORCE_INLINE_ void push_back(const T &p_value) { ptr[count++] = p_value; }
	_FORCE_INLINE_ const T &operator[](int p_index) const { return ptr[p_index]; }
};

void PhysXWaterSurface3D::_update_breakers() {
	const uint64_t start_usec = OS::get_singleton()->get_ticks_usec();
	const int n = grid_resolution;
	bool ready = shore_waves && shore_wave_curl > 0.0f && is_inside_tree() && !Engine::get_singleton()->is_editor_hint() && cell_depth.size() == n * n && shore_iribarren.size() == n * n;
	for (int t = 0; ready && t < SHORE_TRAINS; t++) {
		ready = shore_phase[t].size() == n * n && shore_height[t].size() == n * n && shore_stage[t].size() == n * n && shore_grad_x[t].size() == n * n && shore_grad_z[t].size() == n * n;
	}
	if (!ready) {
		_hide_breakers();
		return;
	}
	const float g = 9.81f;
	const float time = (float)Math::fmod(water_time, 3600.0);
	const Vector2 dom = active_domain_size;
	const Transform3D to_local = get_global_transform().affine_inverse();
	const Vector3 up(0, 1, 0);
	// Cross-section samples from the crest to the lip's edge; the ring runs out along the top and back underneath.
	constexpr int K = 8;
	constexpr int RING = 2 * (K + 1);
	// Crest spacing along the lip (m).
	const float spacing = 0.2f;
	// Droplets per second per metre of lip at full potential (Ihmsen's k_wc and k_ta).
	const float k_wave_crest = 60.0f;
	const float k_trapped_air = 220.0f;

	auto plunge_of = [&](float p_xi) {
		return Math::smoothstep(0.35f, 0.6f, p_xi) * (1.0f - Math::smoothstep(2.6f, 3.3f, p_xi)) * shore_wave_curl;
	};
	auto vary_at = [&](float p_wx, float p_wz) {
		return 0.55f + 0.45f * _flow_value_noise(p_wx * 0.04f + time * 0.03f, p_wz * 0.04f);
	};
	auto sample = [&](const Vector<float> &p_grid, const Vector2 &p_rel) {
		return _bilinear_sample(p_grid, n, dom, p_rel.x, p_rel.y);
	};

	// Each piece of crest builds its own rings on a worker thread (they only read the surface), straight into its
	// own slice of the mesh arrays; the spray is joined in order after, so the mesh and the spray are the same as
	// one thread building them all.
	struct LipOut {
		_LipSlice<Vector3> verts;
		_LipSlice<Vector3> normals;
		_LipSlice<float> tangents;
		_LipSlice<Color> colors;
		_LipSlice<Vector2> uvs;
		_LipSlice<int32_t> indices;
		LocalVector<Vector3> spray_points;
		LocalVector<Vector3> spray_dirs;
		LocalVector<float> spray_weights;
		LocalVector<float> spray_speeds;
		int crest_points = 0;
	};
	struct LipSegment {
		int t = 0;
		Vector2 a;
		Vector2 b;
		int pieces = 1;
		float piece = 0.0f;
		int first_vertex = 0;
		int first_index = 0;
	};
	LocalVector<LipSegment> segments;
	int vert_count = 0;
	int index_count = 0;
	const float winding = _front_winding();
	// Read once here: the node's transform is not read from worker threads.
	const float origin_y = get_global_position().y;

	// This wave's height, stage and surf similarity at grid-relative p for train t (the material's
	// shore_per_wave()): each wave breaks at its own place.
	auto wave_at = [&](int p_t, const Vector2 &p_rel, float &r_height, float &r_stage, float &r_xi) {
		const float r = _shore_wave_set(sample(shore_phase[p_t], p_rel) - shore_omega[p_t] * time, p_t);
		r_height = sample(shore_height[p_t], p_rel);
		r_stage = sample(shore_stage[p_t], p_rel);
		_shore_per_wave(r, r_height, r_stage);
		r_xi = sample(shore_iribarren, p_rel) / Math::sqrt(MAX(r, 0.05f));
	};
	// How much lip there is at grid-relative p for train t (add_ring's own measure), without the surface.
	auto alive_at = [&](int p_t, const Vector2 &p_rel) {
		const float inside = 1.0f - Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(p_rel.x / dom.x), Math::abs(p_rel.y / dom.y)));
		float height, stage, xi_w;
		wave_at(p_t, p_rel, height, stage, xi_w);
		const float s = CLAMP(stage - 0.95f, 0.0f, 1.0f);
		const float w = plunge_of(xi_w) * vary_at(p_rel.x + grid_center.x, p_rel.y + grid_center.y) * inside;
		return w * Math::smoothstep(0.0f, 0.45f, s) * (1.0f - Math::smoothstep(0.75f, 1.0f, s));
	};
	// One cross-section at grid-relative p for train t, appended as a ring of RING vertices. Returns the outer
	// sheet's direction of travel at the crest (for the winding) in r_dir and its tangent there in r_tangent.
	auto add_ring = [&](LipOut &o, int p_t, const Vector2 &p_rel, float p_piece, Vector3 &r_dir, Vector3 &r_tangent) {
		const float wx = p_rel.x + grid_center.x;
		const float wz = p_rel.y + grid_center.y;
		float height, stage, xi_w;
		wave_at(p_t, p_rel, height, stage, xi_w);
		const float inside = 1.0f - Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(p_rel.x / dom.x), Math::abs(p_rel.y / dom.y)));
		const float vary = vary_at(wx, wz);
		const float s = CLAMP(stage - 0.95f, 0.0f, 1.0f);
		const float w = plunge_of(xi_w) * vary * inside;
		const float grow = Math::smoothstep(0.0f, 0.45f, s);
		const float sink = Math::smoothstep(0.5f, 0.85f, s);
		const float alive = w * grow * (1.0f - Math::smoothstep(0.75f, 1.0f, s));
		Vector2 gv(sample(shore_grad_x[p_t], p_rel), sample(shore_grad_z[p_t], p_rel));
		const float k = gv.length();
		const Vector3 dir = k > 1e-5f ? Vector3(gv.x / k, 0, gv.y / k) : Vector3(1, 0, 0);
		r_dir = dir;
		r_tangent = dir;
		const float crest_y = _sample_height_at(Vector3(wx, 0.0f, wz), ObjectID(), origin_y);
		const Vector3 crest(wx, Math::is_finite(crest_y) ? crest_y : origin_y + water_level, wz);
		const Vector3 crest_local = to_local.xform(crest);
		const Vector3 up_local = to_local.basis.xform(up).normalized();
		// Tangents along the crest: the path tracer's hit rebuilds the vertex frame from normal and tangent.
		const Vector3 along_local = to_local.basis.xform(up.cross(dir)).normalized();
		auto push_normal = [&](const Vector3 &p_n) {
			o.normals.push_back(p_n);
			Vector3 t = along_local - p_n * p_n.dot(along_local);
			t = t.length_squared() > 1e-8f ? t.normalized() : p_n.get_any_perpendicular();
			o.tangents.push_back(t.x);
			o.tangents.push_back(t.y);
			o.tangents.push_back(t.z);
			o.tangents.push_back(1.0f);
		};
		const Color white_none(0, 0, 0, 0);
		const float hc = height * vary * inside; // crest to trough
		if (alive < 0.02f || hc < 0.05f || k <= 1e-5f || !Math::is_finite(crest_y)) {
			// Nothing thrown here: the ring shrinks to the crest, so the sheet tapers off.
			for (int r = 0; r < RING; r++) {
				o.verts.push_back(crest_local);
				push_normal(up_local);
				o.colors.push_back(white_none);
				o.uvs.push_back(Vector2());
			}
			return;
		}
		o.crest_points++;
		// The throw: a tube up to about twice as long as it is high at full curl (the vortex shapes surveyed at
		// surf breaks, Mead and Black 2001), and at most half a radian of phase. The lip lands on the water that
		// far ahead: its drop sets the flight time, and the throw the jet speed relative to the crest.
		const float throw_len = w * MIN(0.5f / k, 2.0f * hc);
		const Vector3 land_guess = crest + dir * throw_len;
		float land_y = _sample_height_at(land_guess, ObjectID(), origin_y);
		if (!Math::is_finite(land_y)) {
			land_y = crest.y - hc;
		}
		const float drop = CLAMP(crest.y - land_y, 0.25f * hc, 1.5f * hc);
		const float tau_hit = Math::sqrt(2.0f * drop / g);
		const float u = throw_len / tau_hit; // jet speed relative to the crest
		const float tau_max = tau_hit * grow;
		const bool landed = grow > 0.98f;
		const Vector3 land_point = crest + dir * throw_len - up * drop;
		const float land_tau = tau_hit;
		const float thick = 0.15f * hc * (1.0f - sink) * MIN(2.0f * alive, 1.0f);
		const float c_phase = shore_omega[p_t] / k;
		// The material's face: its front takes 3 amp k / 2 pi of the period (shore_wave), sharpened by q.
		const float face_w = MAX((float)Math::TAU * CLAMP(k * 1.5f * hc / (float)Math::TAU, 0.02f, 0.5f) / k, 0.05f);
		const float face_q = 1.0f + 1.5f * Math::smoothstep(0.8f, 0.98f, height / MAX(BREAKER_INDEX * MAX(sample(cell_depth, p_rel), 0.0f), 1e-3f));
		Vector3 outer[K + 1];
		Vector3 nrm[K + 1];
		float th[K + 1];
		float white[K + 1];
		float face_at[K + 1];
		for (int j = 0; j <= K; j++) {
			const float f = float(j) / K;
			const float tau = tau_max * f;
			// At least 2 cm: thinner, the underside meets the outside and a ray picks either (the coplanar
			// back faces showed as dark quads under path tracing).
			th[j] = MAX(thick * (1.0f - 0.75f * f), 0.02f);
			// Clear green water at the root, aerated white toward the edge and once it has landed.
			white[j] = CLAMP(0.05f + 0.75f * f * f * f * grow + sink, 0.0f, 1.0f);
			nrm[j] = (dir * (g * tau) + up * u).normalized();
			// After landing the sheet sinks into the bore it has become.
			outer[j] = crest + dir * (u * tau) - up * (0.5f * g * tau * tau + sink * 0.8f * hc * f);
			// The sheet never cuts into the face below it: where the face (the warped profile the material
			// draws, crest_y - hc (1 - ((1 + cos(pi x / w)) / 2)^q) over the front width w) is still higher,
			// the lip rides it, a little clear, and peels off where the face falls away. Two crossing water
			// surfaces would show through each other in triangle-sized patches.
			{
				const float x = u * tau;
				const float ph = MIN(x / face_w, 1.0f) * (float)Math::PI;
				const float face_y = crest.y - hc * (1.0f - Math::pow(0.5f + 0.5f * Math::cos(ph), face_q));
				outer[j].y = MAX(outer[j].y, face_y + th[j] + 0.03f);
				face_at[j] = face_y;
			}
		}
		for (int j = 0; j <= K; j++) {
			o.verts.push_back(to_local.xform(outer[j]));
			push_normal(to_local.basis.xform(nrm[j]).normalized());
			o.colors.push_back(Color(white[j], 0, 0, 0));
			o.uvs.push_back(Vector2(white[j], 0.0f));
		}
		for (int j = K; j >= 0; j--) {
			Vector3 inner = outer[j] - nrm[j] * th[j];
			inner.y = MAX(inner.y, face_at[j] + 0.01f); // the underside stays clear of the face too
			o.verts.push_back(to_local.xform(inner));
			push_normal(to_local.basis.xform(-nrm[j]).normalized());
			o.colors.push_back(Color(white[j], 0, 0, 0));
			o.uvs.push_back(Vector2(white[j], 0.0f));
		}
		// Spray (Ihmsen et al. 2012): kinetic energy per unit mass of the lip, times the wave-crest potential at
		// its edge (curvature 1 / thickness times its speed) and the trapped-air potential where it lands.
		if (!shore_spray || sink > 0.6f) {
			return;
		}
		const float v_abs = c_phase + u;
		const float i_k = _ihmsen_phi(0.5f * v_abs * v_abs, 2.0f, 30.0f);
		const float tip_thick = MAX(th[K], 0.02f);
		const float i_wc = _ihmsen_phi(u / tip_thick, 5.0f, 60.0f) * (landed ? 0.0f : 1.0f);
		const float v_impact = landed ? Math::sqrt(u * u + g * land_tau * g * land_tau) : 0.0f;
		const float i_ta = _ihmsen_phi(v_impact, 1.0f, 6.0f);
		const float edge_rate = i_k * k_wave_crest * i_wc * alive * p_piece;
		const float land_rate = i_k * k_trapped_air * i_ta * alive * p_piece;
		if (edge_rate > 1e-3f) {
			// Off the lip's edge, along its jet (absolute: the crest's own speed plus the throw).
			const Vector3 v = dir * v_abs - up * (g * tau_max);
			o.spray_points.push_back(to_local.xform(outer[K]));
			o.spray_dirs.push_back(to_local.basis.xform(v.normalized()));
			o.spray_weights.push_back(edge_rate);
			o.spray_speeds.push_back(edge_rate * 0.35f * v.length());
		}
		if (land_rate > 1e-3f) {
			// Thrown up and forward where it lands.
			const Vector3 v = (dir * 0.6f + up).normalized();
			o.spray_points.push_back(to_local.xform(land_point));
			o.spray_dirs.push_back(to_local.basis.xform(v));
			o.spray_weights.push_back(land_rate);
			o.spray_speeds.push_back(land_rate * 0.6f * v_impact);
		}
	};

	LocalVector<float> level;
	LocalVector<uint8_t> active;
	LocalVector<uint8_t> known;
	LocalVector<float> node_height;
	LocalVector<float> node_stage;
	LocalVector<float> node_xi;
	node_height.resize(n * n);
	node_stage.resize(n * n);
	node_xi.resize(n * n);
	level.resize(n * n);
	active.resize(n * n);
	known.resize(n * n);
	for (int t = 0; t < SHORE_TRAINS; t++) {
		const float om = shore_omega[t];
		if (om <= 0.0f) {
			continue;
		}
		const float *ph = shore_phase[t].ptr();
		const float *hh = shore_height[t].ptr();
		const float *st = shore_stage[t].ptr();
		const float *xi = shore_iribarren.ptr();
		const float *dp = cell_depth.ptr();
		bool any = false;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				// This wave's height and stage at the node (each wave breaks at its own place).
				const float r = hh[i] > 1e-3f ? _shore_wave_set(ph[i] - om * time, t) : 1.0f;
				float h_w = hh[i];
				float st_w = st[i];
				_shore_per_wave(r, h_w, st_w);
				node_height[i] = h_w;
				node_stage[i] = st_w;
				node_xi[i] = xi[i] / Math::sqrt(MAX(r, 0.05f));
				const float s = CLAMP(st_w - 0.95f, 0.0f, 1.0f);
				const float w = plunge_of(node_xi[i]);
				active[i] = (h_w > 1e-3f && s > 0.0f && s < 1.0f && w > 0.01f) ? 1 : 0;
				any = any || active[i];
				known[i] = 0;
			}
		}
		if (!any) {
			continue;
		}
		// The crest as the material places it (th - skew cos(th) at the warp's crest), only at the nodes of
		// cells that hold a breaking crest.
		auto level_at = [&](int i) {
			if (known[i]) {
				return level[i];
			}
			known[i] = 1;
			{
				const int x = i % n;
				const int z = i / n;
				const float s = CLAMP(node_stage[i] - 0.95f, 0.0f, 1.0f);
				const float w = plunge_of(node_xi[i]);
				const float wx = ((x + 0.5f) / n - 0.5f) * dom.x + grid_center.x;
				const float wz = ((z + 0.5f) / n - 0.5f) * dom.y + grid_center.y;
				const float vary = vary_at(wx, wz);
				const float brk = Math::smoothstep(0.8f, 0.98f, node_height[i] / MAX(BREAKER_INDEX * MAX(dp[i], 0.0f), 1e-3f));
				const float throw_w = w * vary * Math::smoothstep(0.0f, 0.45f, s) * (1.0f - Math::smoothstep(0.7f, 1.0f, s));
				const float skew = MIN(0.6f * brk + 0.3f * throw_w, 0.92f);
				const float theta = ph[i] - om * time;
				// The front warp puts the crest at u = pi (2 b - 1), b the back fraction.
				const float steep = w * vary * Math::smoothstep(0.0f, 0.3f, s) * (1.0f - 0.5f * Math::smoothstep(0.8f, 1.0f, s));
				const float kk = Vector2(shore_grad_x[t][i], shore_grad_z[t][i]).length();
				const float amp = 0.5f * node_height[i] * vary;
				const float front = Math::lerp(0.5f, CLAMP(kk * 3.0f * amp / (float)Math::TAU, 0.02f, 0.5f), CLAMP(steep, 0.0f, 1.0f));
				level[i] = theta - skew * Math::cos(theta) - (float)Math::PI * (1.0f - 2.0f * front);
			}
			return level[i];
		};
		for (int z = 0; z + 1 < n; z++) {
			for (int x = 0; x + 1 < n; x++) {
				const int c[4] = { z * n + x, z * n + x + 1, (z + 1) * n + x + 1, (z + 1) * n + x };
				if (!(active[c[0]] || active[c[1]] || active[c[2]] || active[c[3]])) {
					continue;
				}
				if (hh[c[0]] <= 1e-4f || hh[c[1]] <= 1e-4f || hh[c[2]] <= 1e-4f || hh[c[3]] <= 1e-4f) {
					continue; // the waterline: no phase across it
				}
				const Vector2 corner[4] = {
					Vector2(((x + 0.5f) / n - 0.5f) * dom.x, ((z + 0.5f) / n - 0.5f) * dom.y),
					Vector2(((x + 1.5f) / n - 0.5f) * dom.x, ((z + 0.5f) / n - 0.5f) * dom.y),
					Vector2(((x + 1.5f) / n - 0.5f) * dom.x, ((z + 1.5f) / n - 0.5f) * dom.y),
					Vector2(((x + 0.5f) / n - 0.5f) * dom.x, ((z + 1.5f) / n - 0.5f) * dom.y),
				};
				const float lv[4] = { level_at(c[0]), level_at(c[1]), level_at(c[2]), level_at(c[3]) };
				float lo = lv[0], hi = lv[0];
				for (int q = 1; q < 4; q++) {
					lo = MIN(lo, lv[q]);
					hi = MAX(hi, lv[q]);
				}
				const int m0 = (int)Math::ceil(lo / Math::TAU);
				const int m1 = (int)Math::floor(hi / Math::TAU);
				for (int m = m0; m <= m1; m++) {
					const float target = Math::TAU * m;
					Vector2 cross_pts[4];
					int count = 0;
					for (int e = 0; e < 4; e++) {
						const float fa = lv[e] - target;
						const float fb = lv[(e + 1) & 3] - target;
						if ((fa < 0.0f) != (fb < 0.0f)) {
							const float a = fa / (fa - fb);
							cross_pts[count++] = corner[e].lerp(corner[(e + 1) & 3], a);
						}
					}
					for (int pair = 0; pair + 1 < count; pair += 2) {
						// Every piece runs the same way along its crest (crest x travel direction points up),
						// so one winding makes the outside of the sheet face out everywhere.
						Vector2 a = cross_pts[pair];
						Vector2 b = cross_pts[pair + 1];
						{
							const Vector2 mid = (a + b) * 0.5f;
							const Vector2 gm(sample(shore_grad_x[t], mid), sample(shore_grad_z[t], mid));
							if ((b - a).cross(gm) > 0.0f) {
								SWAP(a, b);
							}
						}
						const float len = a.distance_to(b);
						if (len < 1e-4f) {
							continue;
						}
						const int pieces = MAX(1, (int)Math::ceil(len / spacing));
						bool alive_any = false;
						for (int sidx = 0; sidx <= pieces && !alive_any; sidx++) {
							alive_any = alive_at(t, a.lerp(b, float(sidx) / pieces)) >= 0.02f;
						}
						if (!alive_any) {
							continue;
						}
						segments.push_back({ t, a, b, pieces, len / pieces, vert_count, index_count });
						vert_count += (pieces + 1) * RING;
						index_count += pieces * RING * 6;
					}
				}
			}
		}
	}

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedColorArray colors;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	verts.resize(vert_count);
	normals.resize(vert_count);
	tangents.resize(vert_count * 4);
	colors.resize(vert_count);
	uvs.resize(vert_count);
	indices.resize(index_count);
	LocalVector<LipOut> outs;
	outs.resize(segments.size());
	{
		Vector3 *wv = verts.ptrw();
		Vector3 *wn = normals.ptrw();
		float *wt = tangents.ptrw();
		Color *wc = colors.ptrw();
		Vector2 *wu = uvs.ptrw();
		int32_t *wi = indices.ptrw();
		auto build_segment = [&](uint32_t p_index) {
			const LipSegment &seg = segments[p_index];
			LipOut &o = outs[p_index];
			const int vb = seg.first_vertex;
			o.verts.ptr = wv + vb;
			o.normals.ptr = wn + vb;
			o.tangents.ptr = wt + vb * 4;
			o.colors.ptr = wc + vb;
			o.uvs.ptr = wu + vb;
			o.indices.ptr = wi + seg.first_index;
			Vector3 dir, tangent;
			for (int sidx = 0; sidx <= seg.pieces; sidx++) {
				add_ring(o, seg.t, seg.a.lerp(seg.b, float(sidx) / seg.pieces), seg.piece, dir, tangent);
			}
			// Each triangle is wound so its face agrees with the sheet's normals: the outside of the closed sheet
			// faces out (the path tracer enters the water through front faces, and turns a shading normal that
			// disagrees with its face around, which showed as dark quads). Where a sheet folds over the face below, a
			// fixed winding per piece is not enough.
			auto tri = [&](int p_a, int p_b, int p_c) {
				const Vector3 fn = (o.verts[p_b] - o.verts[p_a]).cross(o.verts[p_c] - o.verts[p_a]);
				const Vector3 vn = o.normals[p_a] + o.normals[p_b] + o.normals[p_c];
				const bool flip = (fn.dot(vn) < 0.0f) == (winding > 0.0f);
				o.indices.push_back(vb + p_a);
				o.indices.push_back(vb + (flip ? p_c : p_b));
				o.indices.push_back(vb + (flip ? p_b : p_c));
			};
			for (int sidx = 0; sidx < seg.pieces; sidx++) {
				const int r0 = sidx * RING;
				const int r1 = r0 + RING;
				for (int r = 0; r < RING; r++) {
					const int rn = (r + 1) % RING;
					const int qa = r0 + r, qb = r1 + r, qc = r1 + rn, qd = r0 + rn;
					tri(qa, qb, qc);
					tri(qa, qc, qd);
				}
			}
		};
		if (segments.size() > 8) {
			_LipJob<decltype(build_segment)> job;
			job.fn = &build_segment;
			WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
			const WorkerThreadPool::GroupID gid = pool->add_template_group_task(&job, &_LipJob<decltype(build_segment)>::run, (void *)nullptr, (int)segments.size(), -1, true, SNAME("BreakerLips"));
			pool->wait_for_group_task_completion(gid);
		} else {
			for (uint32_t i = 0; i < segments.size(); i++) {
				build_segment(i);
			}
		}
	}
	// The spray, joined in order.
	LocalVector<Vector3> spray_points;
	LocalVector<Vector3> spray_dirs;
	LocalVector<float> spray_weights;
	float spray_rate = 0.0f;
	float spray_speed_sum = 0.0f;
	int crest_points = 0;
	for (const LipOut &o : outs) {
		crest_points += o.crest_points;
		for (uint32_t i = 0; i < o.spray_points.size(); i++) {
			spray_points.push_back(o.spray_points[i]);
			spray_dirs.push_back(o.spray_dirs[i]);
			spray_weights.push_back(o.spray_weights[i]);
			spray_rate += o.spray_weights[i];
			spray_speed_sum += o.spray_speeds[i];
		}
	}
	if (breaker_instance == nullptr) {
		breaker_instance = memnew(MeshInstance3D);
		breaker_instance->set_name("BreakerLips");
		breaker_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(breaker_instance, false, INTERNAL_MODE_BACK);
		breaker_mesh.instantiate();
		breaker_instance->set_mesh(breaker_mesh);
	}
	breaker_mesh->clear_surfaces();
	if (!indices.is_empty() && crest_points > 0) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TANGENT] = tangents;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;
		breaker_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		breaker_instance->set_material_override(_breaker_material());
		breaker_instance->set_visible(true);
	} else {
		breaker_instance->set_visible(false);
	}
	breaker_stat_points = crest_points;
	breaker_stat_vertices = crest_points > 0 ? verts.size() : 0;
	breaker_stat_triangles = crest_points > 0 ? indices.size() / 3 : 0;
	breaker_stat_spray_rate = spray_rate;
	_update_breaker_spray(spray_points, spray_dirs, spray_weights, spray_rate, spray_rate > 0.0f ? spray_speed_sum / spray_rate : 0.0f);
	breaker_stat_ms = (OS::get_singleton()->get_ticks_usec() - start_usec) / 1000.0f;
}

// The spray: one GPUParticles3D emitting from up to 512 points a tick, drawn from the lip's emitters in
// proportion to their rates (systematic resampling), at a total rate that follows theirs.
void PhysXWaterSurface3D::_update_breaker_spray(const LocalVector<Vector3> &p_points, const LocalVector<Vector3> &p_dirs, const LocalVector<float> &p_weights, float p_rate, float p_speed) {
	if (!shore_spray || p_points.is_empty()) {
		if (breaker_spray != nullptr) {
			breaker_spray->set_emitting(false);
		}
		return;
	}
	constexpr int POINTS = 512;
	const float lifetime = 0.9f;
	if (breaker_spray == nullptr) {
		breaker_spray = memnew(GPUParticles3D);
		breaker_spray->set_name("BreakerSpray");
		breaker_spray->set_use_local_coordinates(false);
		breaker_spray->set_lifetime(lifetime);
		breaker_spray_material.instantiate();
		breaker_spray_material->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_DIRECTED_POINTS);
		breaker_spray_material->set_direction(Vector3(0, 0, 1)); // turned onto each point's direction
		breaker_spray_material->set_spread(18.0f);
		breaker_spray_material->set_gravity(Vector3(0, -9.81f, 0));
		breaker_spray_material->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.2f);
		breaker_spray_material->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 0.8f);
		breaker_spray_material->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.015f);
		breaker_spray_material->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 0.045f);
		breaker_spray->set_process_material(breaker_spray_material);
		Ref<SphereMesh> drop;
		drop.instantiate();
		drop->set_radius(0.5f);
		drop->set_height(1.0f);
		drop->set_radial_segments(6);
		drop->set_rings(3);
		Ref<StandardMaterial3D> dm;
		dm.instantiate();
		dm->set_albedo(Color(0.95f, 0.97f, 1.0f));
		dm->set_roughness(0.5f);
		// Droplets pass most light on (back lighting, also under path tracing), so spray against the sun glows.
		dm->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
		dm->set_backlight(Color(0.8f, 0.85f, 0.85f));
		drop->set_material(dm);
		breaker_spray->set_draw_pass_mesh(0, drop);
		add_child(breaker_spray, false, INTERNAL_MODE_BACK);
	}
	if (breaker_spray->get_amount() != shore_spray_amount) {
		breaker_spray->set_amount(shore_spray_amount);
	}
	Ref<Image> pts = Image::create_empty(POINTS, 1, false, Image::FORMAT_RGBF);
	Ref<Image> dirs = Image::create_empty(POINTS, 1, false, Image::FORMAT_RGBF);
	float total = 0.0f;
	for (float w : p_weights) {
		total += w;
	}
	const float step = total / POINTS;
	float next = step * breaker_rng.randf();
	float acc = 0.0f;
	int used = 0;
	AABB box(p_points[0], Vector3());
	for (uint32_t i = 0; i < p_points.size() && used < POINTS; i++) {
		acc += p_weights[i];
		box.expand_to(p_points[i]);
		while (next < acc && used < POINTS) {
			// Ihmsen et al. sample a cylinder around the emitter; a jitter of a few cm does it here.
			const Vector3 j(breaker_rng.random(-0.08f, 0.08f), breaker_rng.random(0.0f, 0.06f), breaker_rng.random(-0.08f, 0.08f));
			const Vector3 p = p_points[i] + j;
			pts->set_pixel(used, 0, Color(p.x, p.y, p.z));
			dirs->set_pixel(used, 0, Color(p_dirs[i].x, p_dirs[i].y, p_dirs[i].z));
			used++;
			next += step;
		}
	}
	for (uint32_t i = 0; i < p_points.size(); i++) {
		box.expand_to(p_points[i]);
	}
	if (used == 0) {
		breaker_spray->set_emitting(false);
		return;
	}
	if (breaker_spray_points_tex.is_null()) {
		breaker_spray_points_tex = ImageTexture::create_from_image(pts);
		breaker_spray_dirs_tex = ImageTexture::create_from_image(dirs);
		breaker_spray_material->set_emission_point_texture(breaker_spray_points_tex);
		breaker_spray_material->set_emission_normal_texture(breaker_spray_dirs_tex);
	} else {
		breaker_spray_points_tex->update(pts);
		breaker_spray_dirs_tex->update(dirs);
	}
	breaker_spray_material->set_emission_point_count(used);
	breaker_spray_material->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.6f * p_speed);
	breaker_spray_material->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 1.3f * p_speed);
	// At full ratio the node emits amount / lifetime droplets a second.
	breaker_spray->set_amount_ratio(CLAMP(p_rate * lifetime / shore_spray_amount, 0.0f, 1.0f));
	breaker_spray->set_visibility_aabb(box.grow(4.0f));
	breaker_spray->set_emitting(true);
}

void PhysXWaterSurface3D::set_shore_spray(bool p_enabled) {
	shore_spray = p_enabled;
	if (!shore_spray && breaker_spray != nullptr) {
		breaker_spray->set_emitting(false);
	}
}

void PhysXWaterSurface3D::set_shore_spray_amount(int p_amount) {
	shore_spray_amount = CLAMP(p_amount, 16, 65536);
}

Dictionary PhysXWaterSurface3D::get_breaker_stats() const {
	Dictionary d;
	d["crest_points"] = breaker_stat_points;
	d["vertices"] = breaker_stat_vertices;
	d["triangles"] = breaker_stat_triangles;
	d["spray_rate"] = breaker_stat_spray_rate;
	d["cpu_ms"] = breaker_stat_ms;
	return d;
}

void PhysXWaterSurface3D::set_cull_margin(float p_margin) {
	cull_margin = MAX(p_margin, 0.0f);
	_update_cull_box();
}

// follow_weather: the sea takes its wind from the WeatherMap3D. Waves need time to grow: the Shore Protection
// Manual's (CERC 1984) duration for a fetch-limited sea, g t / U = 68.8 (g F / U^2)^(2/3), sets how fast the
// wind the spectrum sees follows the real one (a first-order lag of a third of that time, in weather time).
// The spectrum is updated in place (set_sea_state), so the waves keep their phases while they grow or turn.
void PhysXWaterSurface3D::_follow_weather(double p_delta) {
#ifdef MODULE_WEATHER_ENABLED
	if (!follow_weather || Engine::get_singleton()->is_editor_hint() || !is_inside_tree()) {
		return;
	}
	const WeatherMap3D *weather = WeatherMap3D::get_active();
	if (weather == nullptr) {
		return;
	}
	// A snapped weather (snap_to_settings(), or its first update) snaps the sea too, once the map's cells
	// hold the new state: a sample taken before reads the old wind.
	if (weather->get_snap_serial() != weather_snap_serial) {
		weather_snap_serial = weather->get_snap_serial();
		weather_sea_valid = false;
	}
	if (!weather_sea_valid && !weather->are_cells_fresh_since_snap()) {
		return;
	}
	// Weather time skipped by advance() grows the sea as well.
	const double skipped = weather->get_advanced_time() - weather_advanced_seen;
	weather_advanced_seen = weather->get_advanced_time();
	weather_check_accum += p_delta;
	if (weather_check_accum < 0.25 && weather_sea_valid && skipped <= 0.0) {
		return;
	}
	const double dt = weather_check_accum * weather->get_time_scale() + MAX(skipped, 0.0);
	weather_check_accum = 0.0;
	const Vector2 w = weather->get_mean_wind_at(get_global_position());
	const float target_speed = MAX(w.length(), 0.5f);
	const Vector2 target_dir = w.length_squared() > 1e-6f ? w.normalized() : (wind_direction.length_squared() > 1e-6f ? wind_direction.normalized() : Vector2(1, 0));
	if (!weather_sea_valid) {
		weather_sea_speed = target_speed;
		weather_sea_direction = target_dir;
		weather_sea_valid = true;
	} else {
		const float g = 9.81f;
		const float u = MAX(weather_sea_speed, 0.5f);
		const float f = get_effective_fetch();
		const float t_full = 68.8f * Math::pow(g * f / (u * u), 2.0f / 3.0f) * u / g;
		const float a = 1.0f - Math::exp(-float(dt) / MAX(t_full / 3.0f, 1.0f));
		weather_sea_speed = Math::lerp(weather_sea_speed, target_speed, a);
		weather_sea_direction = weather_sea_direction.rotated(weather_sea_direction.angle_to(target_dir) * a).normalized();
	}
	const Vector2 cur_dir = wind_direction.length_squared() > 1e-6f ? wind_direction.normalized() : Vector2(1, 0);
	if (Math::abs(weather_sea_speed - wind_speed) > 0.02f * wind_speed || Math::abs(cur_dir.angle_to(weather_sea_direction)) > Math::deg_to_rad(2.0f)) {
		set_sea_state(weather_sea_speed, weather_sea_direction, wave_amplitude, fetch);
	}
#endif
}

void PhysXWaterSurface3D::set_follow_weather(bool p_enabled) {
	follow_weather = p_enabled;
	weather_sea_valid = false;
#ifdef MODULE_WEATHER_ENABLED
	if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
		weather_advanced_seen = weather->get_advanced_time();
	}
#endif
}

void PhysXWaterSurface3D::set_sea_state(float p_wind_speed, Vector2 p_wind_direction, float p_wave_amplitude, float p_fetch) {
	wind_speed = MAX(p_wind_speed, 0.01f);
	wind_direction = p_wind_direction;
	wave_amplitude = MAX(p_wave_amplitude, 0.0f);
	fetch = MAX(p_fetch, 0.0f);
	_reinit_spectrum();
}

// Move the ripple square to `p_world_position` (its XZ; the node's height is kept) WITHOUT a rebuild: the mesh, the FFT
// ocean and every texture stay bound (a rebuild left the sea undrawn for a frame -- a flash each time a camera-following
// square re-centred); the ripples are cleared. A footprint / per-cell seabed depends on where the grid is, so those
// still rebuild.
void PhysXWaterSurface3D::recenter(Vector3 p_world_position) {
	Vector3 p = get_global_position();
	p.x = p_world_position.x;
	p.z = p_world_position.z;
	if (!solver.is_available() || Engine::get_singleton()->is_editor_hint() || surface_mesh.is_valid() || seabed_from_floor || !cell_depth.is_empty()) {
		set_global_position(p);
		_rebuild();
		return;
	}
	set_global_position(p);
	grid_center = Vector2(p.x, p.z);
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("grid_center", grid_center);
	}
	solver.recenter(grid_center);
	_bake_plumes();
	cached_ripple_height.clear(); // (the old square's heights are not this one's)
	previous_ripple_height.clear();
	frames_since_refresh = REFRESH_EVERY_FRAMES;
}

// The floor changed: its depths are measured again on the next physics tick and written into the solver's textures in
// place (no rebuild, so ripples, foam and swash carry on). Rays need a physics tick, as in _rebuild().
void PhysXWaterSurface3D::remeasure_seabed() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	ERR_FAIL_COND_MSG(!seabed_from_floor, "PhysXWaterSurface3D: remeasure_seabed() needs seabed_from_floor.");
	seabed_remeasure_pending = true;
}

// The sea state changed: a built solver regenerates its spectrum IN PLACE (no rebuild -- the waves keep their phases,
// ripples / wakes / foam survive: storms can ramp it); not built yet → the usual rebuild.
void PhysXWaterSurface3D::_reinit_spectrum() {
	if (solver.is_available() && !Engine::get_singleton()->is_editor_hint()) {
		solver.reinit_spectrum(wind_speed, wind_direction, wave_amplitude, get_effective_fetch());
		for (WaveCascade *c : extra_cascades) {
			c->solver.reinit_spectrum(wind_speed, wind_direction, wave_amplitude, get_effective_fetch());
		}
		_update_cull_box();
		return;
	}
	_rebuild();
}

void PhysXWaterSurface3D::set_current(Vector2 p_current) {
	if (current == p_current) {
		return;
	}
	current = p_current;
	_bake_flow();
}

void PhysXWaterSurface3D::set_flow_period(float p_period) {
	flow_period = MAX(p_period, 0.05f);
	_update_flow_material();
}

void PhysXWaterSurface3D::rebake_flow() {
	_bake_flow();
}

void PhysXWaterSurface3D::set_live_flow(bool p_enabled) {
	if (live_flow == p_enabled) {
		return;
	}
	live_flow = p_enabled;
	cached_live_velocity.clear();
	_send_flow_to_solver();
}

void PhysXWaterSurface3D::set_flow_foam(float p_strength) {
	flow_foam = MAX(p_strength, 0.0f);
	solver.set_flow_foam(flow_foam, flow_foam_persistence);
	_update_flow_material();
}

void PhysXWaterSurface3D::set_flow_foam_persistence(float p_seconds) {
	flow_foam_persistence = MAX(p_seconds, 0.1f);
	solver.set_flow_foam(flow_foam, flow_foam_persistence);
}

void PhysXWaterSurface3D::set_wall_friction(float p_friction) {
	wall_friction = MAX(p_friction, 0.0f);
	_send_flow_to_solver();
}

Ref<Texture2D> PhysXWaterSurface3D::get_live_flow_texture() const {
	if (!live_flow || !solver.has_base_flux() || live_flow_texture.is_null() || !live_flow_texture->get_texture_rd_rid().is_valid()) {
		return Ref<Texture2D>();
	}
	return live_flow_texture;
}

void PhysXWaterSurface3D::_send_flow_to_solver() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	solver.set_base_flux(live_flow ? flow_faces : PackedFloat32Array(), wall_friction);
	_update_flow_material();
}

// The live part of the flow at grid-relative (x, z): the read-back face velocities averaged to
// cell centres, bilinear between them.
Vector2 PhysXWaterSurface3D::_live_flow_at(float p_x, float p_z) const {
	const int n = grid_resolution;
	if (cached_live_velocity.size() != n * n * 2) {
		return Vector2();
	}
	const float *v = cached_live_velocity.ptr();
	auto centre = [&](int x, int z) {
		const int i = z * n + x;
		const float u = 0.5f * (v[i * 2] + (x > 0 ? v[(i - 1) * 2] : 0.0f));
		const float w = 0.5f * (v[i * 2 + 1] + (z > 0 ? v[(i - n) * 2 + 1] : 0.0f));
		return Vector2(u, w);
	};
	const float gx = (p_x / active_domain_size.x + 0.5f) * n - 0.5f;
	const float gz = (p_z / active_domain_size.y + 0.5f) * n - 0.5f;
	if (gx < -0.5f || gz < -0.5f || gx > n - 0.5f || gz > n - 0.5f) {
		return Vector2();
	}
	const int x0 = CLAMP((int)Math::floor(gx), 0, n - 1);
	const int z0 = CLAMP((int)Math::floor(gz), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1);
	const int z1 = MIN(z0 + 1, n - 1);
	const float fx = CLAMP(gx - x0, 0.0f, 1.0f);
	const float fz = CLAMP(gz - z0, 0.0f, 1.0f);
	return centre(x0, z0).lerp(centre(x1, z0), fx).lerp(centre(x0, z1).lerp(centre(x1, z1), fx), fz);
}

Ref<Texture2D> PhysXWaterSurface3D::get_flow_texture() const {
	return flow_cells.is_empty() ? Ref<Texture2D>() : Ref<Texture2D>(flow_texture);
}

void PhysXWaterSurface3D::_update_flow_material() {
	if (water_material.is_null()) {
		return;
	}
	// The live water's velocity when it runs (the surface's waves and foam then swirl round
	// rocks with it), else the baked flow.
	const bool live = live_flow && textures_bound && solver.has_base_flux() && live_flow_texture.is_valid() && live_flow_texture->get_texture_rd_rid().is_valid();
	const bool baked = !flow_cells.is_empty() && flow_texture.is_valid();
	water_material->set_shader_parameter("current", current);
	water_material->set_shader_parameter("flow_period", flow_period);
	water_material->set_shader_parameter("flow_baked", live || baked);
	water_material->set_shader_parameter("flow_foam_live", live && flow_foam > 0.0f);
	water_material->set_shader_parameter("flow_tex", live ? Variant(live_flow_texture) : (baked ? Variant(flow_texture) : Variant()));
	const bool plumes = !plume_cells.is_empty() && plume_texture.is_valid();
	water_material->set_shader_parameter("plume_enabled", plumes);
	water_material->set_shader_parameter("plume_tex", plumes ? Variant(plume_texture) : Variant());
}

void PhysXWaterSurface3D::set_inflow(int64_t p_owner, const Vector3 &p_at, const Vector3 &p_direction, float p_width, float p_discharge) {
	Inflow f;
	f.owner = p_owner;
	f.at = p_at;
	f.direction = Vector3(p_direction.x, 0.0f, p_direction.z).normalized();
	f.width = MAX(p_width, 0.1f);
	f.discharge = MAX(p_discharge, 0.0f);
	bool found = false;
	for (Inflow &e : inflows) {
		if (e.owner == p_owner) {
			e = f;
			found = true;
			break;
		}
	}
	if (!found) {
		inflows.push_back(f);
	}
	_bake_plumes();
}

void PhysXWaterSurface3D::clear_inflow(int64_t p_owner) {
	for (uint32_t k = 0; k < inflows.size(); k++) {
		if (inflows[k].owner == p_owner) {
			inflows.remove_at(k);
			_bake_plumes();
			return;
		}
	}
}

// A river's outflow as a turbulent plane jet (Rajaratnam 1976, Turbulent
// Jets): a potential core about 3 river widths long at the mouth speed, then a
// centreline speed falling as x^(-1/2) while its half-width grows by 0.1 per
// metre, a Gaussian profile across, and the bed's friction wearing it down
// over 2h / c_f in shallow water (Giger, Dracos and Jirka 1991). The plume is
// carried on top of the flow (the waves and foam ride it, floating bodies
// drift in it); the live water does not simulate it.
void PhysXWaterSurface3D::_bake_plumes() {
	const int n = grid_resolution;
	plume_cells.clear();
	if (inflows.is_empty() || n < 4) {
		_update_flow_material();
		return;
	}
	const float dx = active_domain_size.x / n;
	const float dz = active_domain_size.y / n;
	plume_cells.resize(n * n);
	Vector2 *out = plume_cells.ptrw();
	const bool has_depth = cell_depth.size() == n * n;
	auto depth_at_cell = [&](int x, int z) -> float {
		return has_depth ? cell_depth[z * n + x] : depth;
	};
	for (int i = 0; i < n * n; i++) {
		out[i] = Vector2();
	}
	for (const Inflow &f : inflows) {
		// Depth at the mouth, for its speed.
		const int mx = CLAMP((int)Math::floor(((f.at.x - grid_center.x) / active_domain_size.x + 0.5f) * n), 0, n - 1);
		const int mz = CLAMP((int)Math::floor(((f.at.z - grid_center.y) / active_domain_size.y + 0.5f) * n), 0, n - 1);
		const float h0 = CLAMP(depth_at_cell(mx, mz), 0.3f, 50.0f);
		const float u0 = MIN(f.discharge / (f.width * h0), 6.0f);
		const float core = 3.0f * f.width;
		const Vector2 dir(f.direction.x, f.direction.z);
		const Vector2 perp(-dir.y, dir.x);
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const float h = depth_at_cell(x, z);
				if (!(h > 0.05f)) {
					continue;
				}
				const Vector2 p(grid_center.x + (x + 0.5f - 0.5f * n) * dx - f.at.x, grid_center.y + (z + 0.5f - 0.5f * n) * dz - f.at.z);
				const float along = p.dot(dir);
				if (along < -0.5f * f.width || along > 80.0f * f.width) {
					continue;
				}
				const float across = p.dot(perp);
				const float a = MAX(along, 0.0f);
				const float half = 0.5f * f.width + 0.1f * MAX(a - core, 0.0f);
				const float um = u0 * (a < core ? 1.0f : Math::sqrt(core / a));
				const float cf = 9.81f * 0.03f * 0.03f / Math::pow(MAX(h, 0.1f), 1.0f / 3.0f);
				const float wear = Math::exp(-cf * a / (2.0f * MAX(h, 0.1f)));
				const float r = across / half;
				const float u = um * Math::exp(-0.693f * r * r) * wear * (along < 0.0f ? 1.0f + along / (0.5f * f.width) : 1.0f);
				// Spreading: a little outward with the growing width.
				out[z * n + x] += dir * u + perp * (u * 0.1f * CLAMP(r, -2.0f, 2.0f) * 0.5f);
			}
		}
	}
	PackedByteArray bytes;
	bytes.resize(n * n * 8);
	float *b = (float *)bytes.ptrw();
	for (int i = 0; i < n * n; i++) {
		b[i * 2] = out[i].x;
		b[i * 2 + 1] = out[i].y;
	}
	Ref<Image> img = Image::create_from_data(n, n, false, Image::FORMAT_RGF, bytes);
	if (plume_texture.is_valid() && plume_texture->get_width() == n && plume_texture->get_height() == n) {
		plume_texture->update(img);
	} else {
		plume_texture = ImageTexture::create_from_image(img);
	}
	_update_flow_material();
}

Vector2 PhysXWaterSurface3D::_plume_at(float p_x, float p_z) const {
	const int n = grid_resolution;
	if (plume_cells.size() != n * n) {
		return Vector2();
	}
	const float gx = (p_x / active_domain_size.x + 0.5f) * n - 0.5f;
	const float gz = (p_z / active_domain_size.y + 0.5f) * n - 0.5f;
	if (gx < -0.5f || gz < -0.5f || gx > n - 0.5f || gz > n - 0.5f) {
		return Vector2();
	}
	const int x0 = CLAMP((int)Math::floor(gx), 0, n - 1);
	const int z0 = CLAMP((int)Math::floor(gz), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1);
	const int z1 = MIN(z0 + 1, n - 1);
	const float fx = CLAMP(gx - x0, 0.0f, 1.0f);
	const float fz = CLAMP(gz - z0, 0.0f, 1.0f);
	const Vector2 *f = plume_cells.ptr();
	return f[z0 * n + x0].lerp(f[z0 * n + x1], fx).lerp(f[z1 * n + x0].lerp(f[z1 * n + x1], fx), fz);
}

// The flow over the simulated square, round dry ground and rocks (docs/ideas/water.md, step 3 research):
// a depth-averaged, incompressible, irrotational flow from a stream function psi, with
// u = (dpsi/dz, -dpsi/dx) / depth, so div(depth * u) = 0 (what flows in flows out) and
// div(grad(psi) / depth) = 0 (no eddies of its own). Dry cells are walls: each connected patch of
// them holds one psi value, the banks' fixed by the edge and an island's chosen so no net flow goes
// round it. Along the square's edge psi integrates the current's flux, its inflow and outflow scaled
// to match so the bake conserves the water. Solved once (SOR) per seabed or current change.
void PhysXWaterSurface3D::_bake_flow() {
	_bake_plumes();
	flow_cells.clear();
	flow_faces.clear();
	const int n = grid_resolution;
	if (current.length_squared() < 1e-10f || cell_depth.size() != n * n || n < 4) {
		if (current.length_squared() >= 1e-10f && cell_depth.is_empty() && n >= 4) {
			// No seabed: the same depth everywhere, the current straight through every face.
			flow_faces.resize(n * n * 4);
			float *q = flow_faces.ptrw();
			for (int i = 0; i < n * n; i++) {
				q[i * 4 + 0] = depth * current.x;
				q[i * 4 + 1] = depth * current.y;
				q[i * 4 + 2] = depth * current.x;
				q[i * 4 + 3] = depth * current.y;
			}
		}
		_send_flow_to_solver();
		_update_flow_material();
		return;
	}
	const float dx = active_domain_size.x / n;
	const float dz = active_domain_size.y / n;
	const float DRY = 0.05f; // m: shallower counts as ground
	const float MIN_DEPTH = 0.25f; // m: the depth the flow divides by, at least (no racing films)
	const int cells = n * n;
	LocalVector<float> h;
	h.resize(cells);
	for (int i = 0; i < cells; i++) {
		h[i] = cell_depth[i] > DRY ? CLAMP(cell_depth[i], MIN_DEPTH, 1000.0f) : 0.0f; // (a ray that found no floor reads as deep)
	}
	// Dry patches (4-connected): component id per dry cell, -1 wet.
	LocalVector<int> comp;
	comp.resize(cells);
	for (int i = 0; i < cells; i++) {
		comp[i] = -1;
	}
	int comp_count = 0;
	LocalVector<int> stack;
	for (int i = 0; i < cells; i++) {
		if (h[i] > 0.0f || comp[i] >= 0) {
			continue;
		}
		comp[i] = comp_count;
		stack.push_back(i);
		while (!stack.is_empty()) {
			const int c = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			const int cx = c % n;
			const int cz = c / n;
			const int nb[4] = { cx > 0 ? c - 1 : -1, cx < n - 1 ? c + 1 : -1, cz > 0 ? c - n : -1, cz < n - 1 ? c + n : -1 };
			for (int k = 0; k < 4; k++) {
				if (nb[k] >= 0 && h[nb[k]] <= 0.0f && comp[nb[k]] < 0) {
					comp[nb[k]] = comp_count;
					stack.push_back(nb[k]);
				}
			}
		}
		comp_count++;
	}
	// Edge walk (counter-clockwise in index space), integrating the current's flux.
	LocalVector<int> ring;
	for (int x = 0; x < n; x++) {
		ring.push_back(x);
	}
	for (int z = 1; z < n; z++) {
		ring.push_back(z * n + n - 1);
	}
	for (int x = n - 2; x >= 0; x--) {
		ring.push_back((n - 1) * n + x);
	}
	for (int z = n - 2; z >= 1; z--) {
		ring.push_back(z * n);
	}
	const int rn = (int)ring.size();
	LocalVector<float> step;
	step.resize(rn);
	float pos_sum = 0.0f;
	float neg_sum = 0.0f;
	for (int r = 0; r < rn; r++) {
		const int a = ring[r];
		const int b = ring[(r + 1) % rn];
		const float ddx = (b % n - a % n) * dx;
		const float ddz = (b / n - a / n) * dz;
		const float hm = (h[a] > 0.0f && h[b] > 0.0f) ? 0.5f * (h[a] + h[b]) : 0.0f;
		step[r] = hm * (current.x * ddz - current.y * ddx);
		if (step[r] > 0.0f) {
			pos_sum += step[r];
		} else {
			neg_sum -= step[r];
		}
	}
	if (pos_sum <= 0.0f || neg_sum <= 0.0f) {
		_send_flow_to_solver();
		_update_flow_material(); // nothing flows in or out through wet edge cells
		return;
	}
	const float mean = 0.5f * (pos_sum + neg_sum);
	LocalVector<float> psi;
	psi.resize(cells);
	LocalVector<uint8_t> fixed;
	fixed.resize(cells);
	for (int i = 0; i < cells; i++) {
		psi[i] = 0.0f;
		fixed[i] = 0;
	}
	LocalVector<float> comp_value;
	LocalVector<uint8_t> comp_fixed;
	comp_value.resize(comp_count);
	comp_fixed.resize(comp_count);
	for (int c = 0; c < comp_count; c++) {
		comp_value[c] = 0.0f;
		comp_fixed[c] = 0;
	}
	float acc = 0.0f;
	for (int r = 0; r < rn; r++) {
		const int a = ring[r];
		psi[a] = acc;
		fixed[a] = 1;
		if (comp[a] >= 0 && !comp_fixed[comp[a]]) {
			comp_fixed[comp[a]] = 1; // a bank: the first edge value it touches (constant along a dry run)
			comp_value[comp[a]] = acc;
		}
		acc += step[r] * (step[r] > 0.0f ? mean / pos_sum : mean / neg_sum);
	}
	// Interior start: the uniform current's psi, so the solve only adds the bends.
	float ref_h = 0.0f;
	int ref_count = 0;
	for (int i = 0; i < cells; i++) {
		if (h[i] > 0.0f) {
			ref_h += h[i];
			ref_count++;
		}
	}
	ref_h = ref_count > 0 ? ref_h / ref_count : 1.0f;
	for (int z = 1; z < n - 1; z++) {
		for (int x = 1; x < n - 1; x++) {
			const int i = z * n + x;
			const float px = (x + 0.5f) * dx - 0.5f * active_domain_size.x;
			const float pz = (z + 0.5f) * dz - 0.5f * active_domain_size.y;
			psi[i] = ref_h * (current.x * pz - current.y * px) + psi[0] - ref_h * (current.x * (0.5f * dz - 0.5f * active_domain_size.y) - current.y * (0.5f * dx - 0.5f * active_domain_size.x));
		}
	}
	for (int i = 0; i < cells; i++) {
		if (comp[i] >= 0 && comp_fixed[comp[i]]) {
			psi[i] = comp_value[comp[i]];
		}
	}
	// div(grad(psi) / h) = 0; face conductances 1 / (mean depth) / spacing^2 (a face onto dry ground takes
	// the wet cell's own depth: the wall holds its patch's psi).
	const float wx = 1.0f / (dx * dx);
	const float wz = 1.0f / (dz * dz);
	if (bool(GLOBAL_GET("physics/physx_3d/water/multigrid_flow"))) {
		LocalVector<int> kind;
		LocalVector<int> island_of;
		LocalVector<float> fcx;
		LocalVector<float> fcz;
		kind.resize(cells);
		fcx.resize(cells);
		fcz.resize(cells);
		island_of.resize(comp_count);
		int islands = 0;
		for (int c = 0; c < comp_count; c++) {
			island_of[c] = comp_fixed[c] ? -1 : islands++;
		}
		auto conductance = [&](int p_a, int p_b, float p_w) -> float {
			if (h[p_a] > 0.0f && h[p_b] > 0.0f) {
				return p_w / (0.5f * (h[p_a] + h[p_b]));
			}
			if (h[p_a] > 0.0f || h[p_b] > 0.0f) {
				return p_w / MAX(h[p_a], h[p_b]);
			}
			return 0.0f;
		};
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (fixed[i]) {
					kind[i] = FlowPoisson::FIXED;
				} else if (comp[i] >= 0) {
					kind[i] = island_of[comp[i]] >= 0 ? FlowPoisson::ISLAND + island_of[comp[i]] : FlowPoisson::FIXED;
				} else {
					kind[i] = FlowPoisson::UNKNOWN;
				}
				fcx[i] = x < n - 1 ? conductance(i, i + 1, wx) : 0.0f;
				fcz[i] = z < n - 1 ? conductance(i, i + n, wz) : 0.0f;
			}
		}
		FlowPoisson::solve(n, n, fcx, fcz, kind, islands, psi);
	} else {
		// SOR on div(grad(psi) / h) = 0; face weights 1 / (mean depth) / spacing^2.
		const float omega = 2.0f / (1.0f + Math::sin(Math::PI / n));
		const float tolerance = 1e-5f * mean; // float psi stalls near 1e-6 of its range
		struct IslandLink {
			int cell;
			int comp;
			float w;
		};
		LocalVector<IslandLink> island_links;
		for (int z = 1; z < n - 1; z++) {
			for (int x = 1; x < n - 1; x++) {
				const int i = z * n + x;
				if (fixed[i] || h[i] <= 0.0f) {
					continue;
				}
				const int nb[4] = { i - 1, i + 1, i - n, i + n };
				const float sw[4] = { wx, wx, wz, wz };
				for (int k = 0; k < 4; k++) {
					const int j = nb[k];
					if (h[j] <= 0.0f && comp[j] >= 0 && !comp_fixed[comp[j]]) {
						island_links.push_back({ i, comp[j], sw[k] / h[i] });
					}
				}
			}
		}
		LocalVector<float> comp_num;
		LocalVector<float> comp_den;
		comp_num.resize(comp_count);
		comp_den.resize(comp_count);
		for (int iter = 0; iter < 4000; iter++) {
			float max_change = 0.0f;
			for (int c = 0; c < comp_count; c++) {
				comp_num[c] = 0.0f;
				comp_den[c] = 0.0f;
			}
			for (int z = 1; z < n - 1; z++) {
				for (int x = 1; x < n - 1; x++) {
					const int i = z * n + x;
					if (fixed[i] || h[i] <= 0.0f) {
						continue;
					}
					const int nb[4] = { i - 1, i + 1, i - n, i + n };
					const float sw[4] = { wx, wx, wz, wz };
					float num = 0.0f;
					float den = 0.0f;
					for (int k = 0; k < 4; k++) {
						const int j = nb[k];
						// A wall face: the wet cell's own depth (the wall holds its patch's psi).
						const float hf = h[j] > 0.0f ? 0.5f * (h[i] + h[j]) : h[i];
						const float w = sw[k] / hf;
						num += w * psi[j];
						den += w;
					}
					if (!(den > 0.0f)) {
						continue;
					}
					const float target = num / den;
					const float change = omega * (target - psi[i]);
					psi[i] += change;
					max_change = MAX(max_change, Math::abs(change));
				}
			}
			// Islands: the value that sends no net flow round them (their wet neighbours' weighted mean, after
			// this sweep: Gauss-Seidel on the island's own unknown too, or the two fight and diverge).
			for (const IslandLink &l : island_links) {
				comp_num[l.comp] += l.w * psi[l.cell];
				comp_den[l.comp] += l.w;
			}
			for (int i = 0; i < cells; i++) {
				const int c = comp[i];
				if (c >= 0 && !comp_fixed[c] && comp_den[c] > 0.0f) {
					psi[i] = comp_num[c] / comp_den[c];
				}
			}
			if (max_change < tolerance && iter > 20) {
				break;
			}
		}
	}
	// Velocity per wet cell from central differences.
	const float max_speed = 4.0f * current.length();
	flow_cells.resize(cells);
	Vector2 *fw = flow_cells.ptrw();
	PackedByteArray bytes;
	bytes.resize(cells * 2 * sizeof(float));
	float *bw = (float *)bytes.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			Vector2 u;
			if (h[i] > 0.0f) {
				const int xl = MAX(x - 1, 0), xr = MIN(x + 1, n - 1);
				const int zl = MAX(z - 1, 0), zr = MIN(z + 1, n - 1);
				const float dpsi_dz = (psi[zr * n + x] - psi[zl * n + x]) / ((zr - zl) * dz);
				const float dpsi_dx = (psi[z * n + xr] - psi[z * n + xl]) / ((xr - xl) * dx);
				u = Vector2(dpsi_dz, -dpsi_dx) / h[i];
				if (!u.is_finite()) {
					u = Vector2();
				} else if (u.length() > max_speed) {
					u = u.normalized() * max_speed;
				}
			}
			fw[i] = u;
			bw[i * 2] = u.x;
			bw[i * 2 + 1] = u.y;
		}
	}
	// Face fluxes for the live water (water_ripple.glsl): psi at cell corners, differenced along each
	// face, so every cell's in and out sum to exactly zero. A corner touching dry ground takes that
	// ground's psi, so no water crosses a bank or a rock.
	{
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
					if (h[c] <= 0.0f && !dry) {
						dry = true;
						dry_psi = psi[c];
					}
					sum += psi[c];
				}
				corner[j * (n + 1) + i] = dry ? dry_psi : 0.25f * sum;
			}
		}
		auto at = [&](int i, int j) { return corner[j * (n + 1) + i]; };
		flow_faces.resize(cells * 4);
		float *q = flow_faces.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				q[i * 4 + 0] = (at(x + 1, z + 1) - at(x + 1, z)) / dz; // +x face: dpsi/dz
				q[i * 4 + 1] = -(at(x + 1, z + 1) - at(x, z + 1)) / dx; // +z face: -dpsi/dx
				q[i * 4 + 2] = (at(x, z + 1) - at(x, z)) / dz; // -x face
				q[i * 4 + 3] = -(at(x + 1, z) - at(x, z)) / dx; // -z face
			}
		}
		for (int k = 0; k < (int)flow_faces.size(); k++) {
			if (!Math::is_finite(q[k])) {
				q[k] = 0.0f;
			}
		}
	}
	_send_flow_to_solver();
	flow_image = Image::create_from_data(n, n, false, Image::FORMAT_RGF, bytes);
	if (flow_texture.is_valid() && flow_texture->get_width() == n && flow_texture->get_height() == n) {
		flow_texture->update(flow_image);
	} else {
		flow_texture = ImageTexture::create_from_image(flow_image);
	}
	_update_flow_material();
}

Vector2 PhysXWaterSurface3D::_flow_at(float p_x, float p_z) const {
	const Vector2 live = (live_flow ? _live_flow_at(p_x, p_z) : Vector2()) + _plume_at(p_x, p_z);
	if (flow_cells.is_empty()) {
		return current + live;
	}
	const int n = grid_resolution;
	const float gx = (p_x / active_domain_size.x + 0.5f) * n - 0.5f;
	const float gz = (p_z / active_domain_size.y + 0.5f) * n - 0.5f;
	const int x0 = CLAMP((int)Math::floor(gx), 0, n - 1);
	const int z0 = CLAMP((int)Math::floor(gz), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1);
	const int z1 = MIN(z0 + 1, n - 1);
	const float fx = CLAMP(gx - x0, 0.0f, 1.0f);
	const float fz = CLAMP(gz - z0, 0.0f, 1.0f);
	const Vector2 *f = flow_cells.ptr();
	const Vector2 v = f[z0 * n + x0].lerp(f[z0 * n + x1], fx).lerp(f[z1 * n + x0].lerp(f[z1 * n + x1], fx), fz);
	const float out = Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(p_x / active_domain_size.x), Math::abs(p_z / active_domain_size.y)));
	return (v + live).lerp(current, out);
}

// The shader's flow_hash() / flow_value_noise(), for the same phase noise on the CPU.
static float _flow_hash(float p_x, float p_y) {
	auto fract = [](float v) { return v - Math::floor(v); };
	float x = fract(p_x * 0.1031f), y = fract(p_y * 0.1031f), z = fract(p_x * 0.1031f);
	const float d = x * (y + 33.33f) + y * (z + 33.33f) + z * (x + 33.33f);
	x += d;
	y += d;
	z += d;
	return fract((x + y) * z);
}

static float _flow_value_noise(float p_x, float p_y) {
	const float ix = Math::floor(p_x), iy = Math::floor(p_y);
	const float fx = p_x - ix, fy = p_y - iy;
	const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
	return Math::lerp(Math::lerp(_flow_hash(ix, iy), _flow_hash(ix + 1.0f, iy), ux),
			Math::lerp(_flow_hash(ix, iy + 1.0f), _flow_hash(ix + 1.0f, iy + 1.0f), ux), uy);
}

float PhysXWaterSurface3D::_flow_phases(float p_x, float p_z, Vector2 &r_offset_a, Vector2 &r_offset_b) const {
	const Vector2 f = _flow_at(p_x, p_z);
	r_offset_a = Vector2();
	r_offset_b = Vector2();
	if (f.length_squared() < 1e-8f) {
		return 1.0f;
	}
	const float wx = p_x + grid_center.x, wz = p_z + grid_center.y;
	const float t = (float)Math::fmod(water_time, 3600.0) / MAX(flow_period, 0.05f) + _flow_value_noise(wx * 0.05f, wz * 0.05f);
	const float pa = t - Math::floor(t);
	const float pb = (t + 0.5f) - Math::floor(t + 0.5f);
	r_offset_a = f * ((pa - 0.5f) * flow_period);
	r_offset_b = f * ((pb - 0.5f) * flow_period);
	return 1.0f - Math::abs(1.0f - 2.0f * pa);
}

float PhysXWaterSurface3D::_ocean_flow_sample(const Vector<float> &p_grid, float p_x, float p_z, float p_weight, const Vector2 &p_offset_a, const Vector2 &p_offset_b, int p_n, Vector2 p_domain) const {
	const int n = p_n < 0 ? cached_ocean_n : p_n;
	const Vector2 domain = p_n < 0 ? cached_ocean_domain : p_domain;
	const float wx = p_x + grid_center.x, wz = p_z + grid_center.y;
	const float a = _bilinear_sample(p_grid, n, domain, wx - p_offset_a.x, wz - p_offset_a.y, true);
	if (p_weight >= 1.0f) {
		return a;
	}
	return a * p_weight + _bilinear_sample(p_grid, n, domain, wx - p_offset_b.x, wz - p_offset_b.y, true) * (1.0f - p_weight);
}

Vector3 PhysXWaterSurface3D::sample_flow(Vector3 p_world_pos) const {
	if (!is_wet(p_world_pos)) {
		return Vector3();
	}
	const float x = p_world_pos.x - grid_center.x;
	const float z = p_world_pos.z - grid_center.y;
	const Vector2 f = _flow_at(x, z);
	return Vector3(f.x, 0.0f, f.y);
}

void PhysXWaterSurface3D::set_choppiness(float p_choppiness) {
	choppiness = MAX(p_choppiness, 0.0f);
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("choppiness", choppiness);
	}
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_foam_enabled(bool p_enabled) {
	foam_enabled = p_enabled;
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("foam_crest_threshold", foam_enabled ? _live_foam_threshold() : -100.0f);
	}
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_shore_undertow(float p_speed) {
	shore_undertow = MAX(p_speed, 0.0f);
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_swash_run_up(float p_height) {
	swash_run_up = MAX(p_height, 0.0f);
	solver.set_swash_settings(swash_run_up, swash_drain_speed, wet_sand_dry_time);
}

void PhysXWaterSurface3D::set_swash_drain_speed(float p_speed) {
	swash_drain_speed = MAX(p_speed, 0.01f);
	solver.set_swash_settings(swash_run_up, swash_drain_speed, wet_sand_dry_time);
}

void PhysXWaterSurface3D::set_wet_sand_dry_time(float p_seconds) {
	wet_sand_dry_time = MAX(p_seconds, 0.1f);
	solver.set_swash_settings(swash_run_up, swash_drain_speed, wet_sand_dry_time);
}

void PhysXWaterSurface3D::set_open_sea_beyond(bool p_open) {
	open_sea_beyond = p_open;
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("open_sea_beyond", open_sea_beyond);
	}
}

void PhysXWaterSurface3D::set_shore_foam_band(float p_depth) {
	shore_foam_band = MAX(p_depth, 0.0f);
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_foam_threshold(float p_threshold) {
	foam_threshold = p_threshold;
	whitecap_threshold = p_threshold; // the steering starts again from here
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("foam_crest_threshold", foam_enabled ? _live_foam_threshold() : -100.0f);
	}
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_whitecap_auto(bool p_auto) {
	whitecap_auto = p_auto;
	whitecap_threshold = foam_threshold;
	whitecap_clock = -1.0;
	_push_foam_settings();
}

void PhysXWaterSurface3D::set_whitecap_cover_scale(float p_scale) {
	whitecap_cover_scale = MAX(p_scale, 0.0f);
}

// Monahan and O'Muircheartaigh (1980), the fit to all their open-ocean
// photographs: W = 3.84e-6 U10^3.41, U10 the wind 10 m up in m/s.
float PhysXWaterSurface3D::get_whitecap_target_cover() const {
	return MIN(3.84e-6f * Math::pow(MAX(wind_speed, 0.0f), 3.41f) * whitecap_cover_scale, 0.5f);
}

// The threshold, the material's crest threshold and every cascade's foam
// pass, all from one place.
void PhysXWaterSurface3D::_push_foam_settings() {
	const float th = _live_foam_threshold();
	whitecap_pushed = th;
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("foam_crest_threshold", foam_enabled ? th : -100.0f);
	}
	solver.set_foam_settings(foam_enabled, choppiness, th, foam_persistence, shore_foam_band, shore_undertow);
	for (WaveCascade *c : extra_cascades) {
		c->solver.set_foam_settings(foam_enabled, choppiness, th, foam_persistence, shore_foam_band, shore_undertow);
	}
}

// Steers the foam threshold so the whitecap cover follows the wind. The
// cover is each cascade's mean foam read back from its foam pass; the
// cascades draw their foam on top of each other (max), so for independent
// fields the share left clear is the product of each one's. An integral
// controller in log cover: the cover spans decades with the wind (0.1% to
// 10%), and a threshold step changes it by a factor, not an amount.
void PhysXWaterSurface3D::_steer_whitecaps() {
	const float c0 = foam_enabled ? solver.get_foam_cover() : -1.0f;
	if (c0 < 0.0f) {
		whitecap_measured = -1.0f;
		whitecap_clock = -1.0;
		return;
	}
	double clear = 1.0 - c0;
	for (WaveCascade *c : extra_cascades) {
		const float ci = c->solver.get_foam_cover();
		if (ci >= 0.0f) {
			clear *= 1.0 - ci;
		}
	}
	whitecap_measured = (float)(1.0 - clear);
	if (!whitecap_auto) {
		whitecap_clock = -1.0;
		return;
	}
	const double dt = whitecap_clock < 0.0 ? 0.0 : water_time - whitecap_clock;
	whitecap_clock = water_time;
	if (dt <= 0.0 || dt > 0.5) {
		return;
	}
	constexpr float FLOOR = 1e-4f; // below this both read as clear sea
	constexpr float GAIN = 0.08f; // threshold change per second per e-fold of error
	const float err = CLAMP(Math::log((whitecap_measured + FLOOR) / (get_whitecap_target_cover() + FLOOR)), -3.0f, 3.0f);
	whitecap_threshold = CLAMP(whitecap_threshold - GAIN * err * (float)dt, -1.0f, 1.5f);
	if (Math::abs(whitecap_threshold - whitecap_pushed) > 0.002f) {
		_push_foam_settings();
	}
}

void PhysXWaterSurface3D::set_foam_persistence(float p_seconds) {
	foam_persistence = MAX(p_seconds, 0.05f);
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
}

void PhysXWaterSurface3D::set_normal_mode(NormalMode p_mode) {
	normal_mode = p_mode;
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("per_pixel_normals", normal_mode == NORMAL_MODE_PER_PIXEL);
	}
}

void PhysXWaterSurface3D::set_wave_spreading(WaveSpreading p_spreading) {
	wave_spreading = p_spreading;
	_set_spreading();
}

void PhysXWaterSurface3D::set_swell(float p_swell) {
	swell = CLAMP(p_swell, 0.0f, 1.0f);
	_set_spreading();
}

void PhysXWaterSurface3D::_set_spreading() {
	if (solver.is_available() && !Engine::get_singleton()->is_editor_hint()) {
		solver.set_spreading(int(wave_spreading), swell);
		for (WaveCascade *c : extra_cascades) {
			c->solver.set_spreading(int(wave_spreading), swell);
		}
		return;
	}
	_rebuild();
}

void PhysXWaterSurface3D::set_fetch(float p_fetch) {
	fetch = MAX(p_fetch, 0.0f);
	_reinit_spectrum();
}

float PhysXWaterSurface3D::get_effective_fetch() const {
	if (fetch > 0.0f) {
		return fetch;
	}
	// Enclosed water (a surface_mesh outline) can't have more fetch than its
	// own size; an open square stands for a patch of open sea.
	return footprint_extent > 0.0 ? (float)footprint_extent : 100000.0f;
}

void PhysXWaterSurface3D::set_seabed_from_floor(bool p_enabled) {
	seabed_from_floor = p_enabled;
	_rebuild();
}

void PhysXWaterSurface3D::set_seabed_collision_mask(uint32_t p_mask) {
	seabed_collision_mask = p_mask;
	_rebuild();
}

void PhysXWaterSurface3D::set_shallow_fade_depth(float p_depth) {
	shallow_fade_depth = MAX(p_depth, 0.01f);
	_rebuild();
}

void PhysXWaterSurface3D::set_mesh_detail(int p_detail) {
	mesh_detail = CLAMP(p_detail, 1, 4);
	_rebuild();
}

void PhysXWaterSurface3D::set_render_extent(float p_extent) {
	render_extent = MAX(p_extent, 0.0f);
	_rebuild();
}

void PhysXWaterSurface3D::set_surface_mesh(const Ref<Mesh> &p_mesh) {
	surface_mesh = p_mesh;
	_rebuild();
}

void PhysXWaterSurface3D::set_water_material(const Ref<ShaderMaterial> &p_material) {
	water_material = p_material;
	if (mesh_instance != nullptr && water_material.is_valid()) {
		mesh_instance->set_material_override(water_material);
	}
}

void PhysXWaterSurface3D::set_underwater_enabled(bool p_enabled) {
	underwater_enabled = p_enabled;
	if (!underwater_enabled) {
		_set_underwater_shown(false);
	}
}

void PhysXWaterSurface3D::set_underwater_meniscus(float p_strength) {
	underwater_meniscus = CLAMP(p_strength, 0.0f, 1.0f);
}

void PhysXWaterSurface3D::_set_underwater_shown(bool p_shown) {
	if (p_shown == underwater_shown) {
		return;
	}
	underwater_shown = p_shown;
	if (p_shown && underwater_fog_material.is_null()) {
		// The same source as the surface, built twice more (see WATER_SHADER_SRC's head).
		Ref<Shader> fog_shader;
		fog_shader.instantiate();
		fog_shader->set_code(String("#define UNDERWATER_FOG\n") + WATER_SHADER_SRC);
		underwater_fog_material.instantiate();
		underwater_fog_material->set_shader(fog_shader);
		// Before the surface's underside among the transparent draws, which then covers it.
		underwater_fog_material->set_render_priority(-1);
		Ref<Shader> surface_shader;
		surface_shader.instantiate();
		surface_shader->set_code(String("#define UNDERWATER_SURFACE\n") + WATER_SHADER_SRC);
		underwater_surface_material.instantiate();
		underwater_surface_material->set_shader(surface_shader);
		List<PropertyInfo> uniforms;
		fog_shader->get_shader_uniform_list(&uniforms);
		underwater_shared_params.clear();
		for (const PropertyInfo &pi : uniforms) {
			if (!pi.name.begins_with("underwater_")) {
				underwater_shared_params.push_back(pi.name);
			}
		}
	}
	if (p_shown && underwater_fog_instance == nullptr) {
		underwater_fog_instance = memnew(MeshInstance3D);
		Ref<QuadMesh> quad;
		quad.instantiate();
		// 2 mm across: the vertex shader stretches it over the screen (POSITION); the path tracer sees a speck.
		quad->set_size(Vector2(0.002, 0.002));
		underwater_fog_instance->set_mesh(quad);
		underwater_fog_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		underwater_fog_instance->set_extra_cull_margin(16384.0);
		underwater_fog_instance->set_material_override(underwater_fog_material);
		add_child(underwater_fog_instance, false, INTERNAL_MODE_BACK);
	}
	if (underwater_fog_instance != nullptr) {
		underwater_fog_instance->set_visible(p_shown);
	}
	if (water_material.is_valid()) {
		if (p_shown && water_material->get_next_pass().is_null()) {
			water_material->set_next_pass(underwater_surface_material);
		} else if (!p_shown && water_material->get_next_pass() == underwater_surface_material) {
			water_material->set_next_pass(Ref<Material>());
		}
		water_material->set_shader_parameter("underwater_active", p_shown);
	}
	if (!p_shown) {
		underwater_camera_below = false;
		if (water_material.is_valid()) {
			water_material->set_shader_parameter("underwater_camera_below", false);
		}
	}
}

// Shows the underwater passes while the camera is within reach of the water and feeds them the
// camera's side of the surface and the light entering the water.
void PhysXWaterSurface3D::set_raster_reflections(bool p_enabled) {
	raster_reflections = p_enabled;
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("raster_reflections", raster_reflections);
	}
}

void PhysXWaterSurface3D::_update_raster_reflections() {
	raster_reflection_state.update(this, water_material, raster_reflections);
}

void PhysXWaterSurface3D::_update_underwater() {
	const Viewport *vp = get_viewport();
	const Camera3D *cam = vp != nullptr ? vp->get_camera_3d() : nullptr;
	if (!underwater_enabled || cam == nullptr || water_material.is_null() || !is_visible_in_tree()) {
		_set_underwater_shown(false);
		return;
	}
	const Vector3 cp = cam->get_global_position();
	const float level = get_global_position().y + water_level;
	// The waves reach a few amplitudes up; the near plane a little further.
	const float reach = MAX(wave_amplitude * 4.0f, 2.0f) + (float)cam->get_near() * 2.0f;
	const bool wet_here = is_wet(cp);
	if (!wet_here || cp.y > level + reach) {
		_set_underwater_shown(false);
		return;
	}
	_set_underwater_shown(true);

	float h = sample_height(cp);
	if (!Math::is_finite(h)) {
		h = level;
	}
	underwater_camera_below = cp.y < h;

	DirectionalLight3D *sun = raster_reflection_state.find_sun(this);
	Vector3 sun_dir(0, -1, 0);
	Vector3 sun_irradiance;
	if (sun != nullptr && sun->is_visible_in_tree()) {
		// Light travels along the light's -Z; refracted down into the water (Snell, IOR 1.33) and
		// thinned by the surface's Fresnel reflection. Raster lights are pi x energy in irradiance.
		const Vector3 travel = -sun->get_global_transform().basis.get_column(2).normalized();
		const float cos_i = -travel.y;
		if (cos_i > 0.0f) {
			const float sin_i = Math::sqrt(MAX(1.0f - cos_i * cos_i, 0.0f));
			const float sin_t = sin_i / 1.33f;
			const float cos_t = Math::sqrt(MAX(1.0f - sin_t * sin_t, 0.0f));
			Vector2 hz(travel.x, travel.z);
			hz = hz.length() > 1e-6f ? hz.normalized() : Vector2();
			sun_dir = Vector3(hz.x * sin_t, -cos_t, hz.y * sin_t);
			const float rs = (cos_i - 1.33f * cos_t) / (cos_i + 1.33f * cos_t);
			const float rp = (1.33f * cos_i - cos_t) / (1.33f * cos_i + cos_t);
			const float transmit = 1.0f - 0.5f * (rs * rs + rp * rp);
			const Color c = sun->get_color().srgb_to_linear() * sun->get_param(Light3D::PARAM_ENERGY) * Math::PI * transmit;
			sun_irradiance = Vector3(c.r, c.g, c.b);
		}
	}

	// The sky's radiance from above: the ambient light a white floor facing up would get, from the
	// environment's irradiance (baked once per sky).
	Ref<Environment> env = cam->get_environment();
	if (env.is_null() && get_world_3d().is_valid()) {
		env = get_world_3d()->get_environment();
	}
	Color sky;
	if (env.is_valid()) {
		const Color ambient = env->get_ambient_light_color().srgb_to_linear() * env->get_ambient_light_energy();
		const Environment::AmbientSource source = env->get_ambient_source();
		const bool from_sky = env->get_sky().is_valid() && (source == Environment::AMBIENT_SOURCE_SKY || (source == Environment::AMBIENT_SOURCE_BG && env->get_background() == Environment::BG_SKY));
		if (from_sky) {
			const RID sky_rid = env->get_sky()->get_rid();
			// Again every few seconds (a sky that changes with the time of day), and until the sky has
			// been drawn once (before that the bake comes back black).
			if (sky_rid != underwater_sky_baked || --underwater_sky_rebake <= 0) {
				underwater_sky_baked = sky_rid;
				underwater_sky_radiance = Color(0, 0, 0);
				Ref<Image> pano = RS::get_singleton()->sky_bake_panorama(sky_rid, 1.0, true, Size2i(32, 16));
				if (pano.is_valid() && !pano->is_empty()) {
					Color sum;
					for (int x = 0; x < pano->get_width(); x++) {
						sum += pano->get_pixel(x, 0);
					}
					underwater_sky_radiance = sum / (float)pano->get_width();
				}
				underwater_sky_rebake = underwater_sky_radiance.get_luminance() > 0.0f ? 240 : 10;
			}
			const float sc = env->get_ambient_light_sky_contribution();
			sky = underwater_sky_radiance * env->get_ambient_light_energy() * sc + ambient * (1.0f - sc);
		} else if (source != Environment::AMBIENT_SOURCE_DISABLED) {
			sky = ambient;
		}
	}

	for (Ref<ShaderMaterial> m : { underwater_fog_material, underwater_surface_material }) {
		for (const StringName &name : underwater_shared_params) {
			const Variant v = water_material->get_shader_parameter(name);
			if (v.get_type() != Variant::NIL) {
				m->set_shader_parameter(name, v);
			}
		}
	}
	for (Ref<ShaderMaterial> m : { water_material, underwater_fog_material, underwater_surface_material }) {
		m->set_shader_parameter("underwater_active", true);
		m->set_shader_parameter("underwater_camera_below", underwater_camera_below);
		m->set_shader_parameter("underwater_sun_direction", sun_dir);
		m->set_shader_parameter("underwater_sun_irradiance", sun_irradiance);
		m->set_shader_parameter("underwater_sky_radiance", Vector3(sky.r, sky.g, sky.b));
		m->set_shader_parameter("underwater_meniscus", underwater_meniscus);
	}
}

void PhysXWaterSurface3D::set_caustics_enabled(bool p_enabled) {
	caustics_enabled = p_enabled;
	_rebuild();
}

void PhysXWaterSurface3D::set_caustics_sun_direction(Vector3 p_direction) {
	caustics_sun_direction = p_direction;
}

void PhysXWaterSurface3D::set_caustics_reference_depth(float p_depth) {
	caustics_reference_depth = MAX(p_depth, 0.1f);
}

Ref<Texture2D> PhysXWaterSurface3D::get_caustics_texture() const {
	if (!caustics_enabled || !solver.is_available() || caustics_texture.is_null()) {
		return Ref<Texture2D>();
	}
	return caustics_texture;
}

void PhysXWaterSurface3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_rebuild();
			set_physics_process_internal(true);
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			set_physics_process_internal(false);
			set_process_internal(false);
			_set_underwater_shown(false);
			// The solver's textures go with this node (or are rebuilt on the
			// way back in): let go of them first.
			_detach_textures();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_follow_weather(get_physics_process_delta_time());
			_update(get_physics_process_delta_time());
			_update_breakers();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				if (step_mode == STEP_MODE_PER_FRAME) {
					_frame_update();
				}
				_update_mesh_offset();
				_update_underwater();
				_update_raster_reflections();
			}
		} break;
	}
}

void PhysXWaterSurface3D::_rebuild() {
	if (!is_inside_world()) {
		return;
	}
	if (mesh_instance == nullptr) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance, false, INTERNAL_MODE_BACK);
	}
	if (water_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(WATER_SHADER_SRC);
		Ref<ShaderMaterial> m;
		m.instantiate();
		m->set_shader(shader);
		water_material = m;
	}
	mesh_instance->set_material_override(water_material);

	// Shape: a surface_mesh footprint fits the ripple grid to its bounds and
	// masks everything outside it dry; without one, the plain square domain.
	Vector2 local_center;
	active_domain_size = domain_size;
	footprint_mask.clear();
	cell_depth.clear();
	footprint_extent = 0.0;
	water_mesh.unref();
	if (surface_mesh.is_valid()) {
		const Vector<Vector2> tris = _collect_footprint();
		if (tris.is_empty()) {
			WARN_PRINT("PhysXWaterSurface3D: surface_mesh has no triangles with any X/Z extent; using the square domain instead.");
		} else {
			Rect2 bounds(tris[0], Vector2());
			for (const Vector2 &v : tris) {
				bounds.expand_to(v);
			}
			local_center = bounds.get_center();
			// One dry cell of margin on each side, so the shoreline never sits
			// on the grid edge.
			const real_t side = MAX(bounds.size.x, bounds.size.y);
			footprint_extent = side;
			const real_t padded = side * (real_t)grid_resolution / (real_t)MAX(grid_resolution - 2, 1);
			active_domain_size = Vector2(padded, padded);
			footprint_mask = _rasterize_wet_mask(tris, local_center, active_domain_size, grid_resolution);
			water_mesh = _build_footprint_mesh(tris, bounds, MAX(grid_resolution, ocean_grid_resolution) * mesh_detail);
		}
	}
	if (water_mesh.is_null() && render_extent > MAX(ocean_domain_size.x, ocean_domain_size.y)) {
		water_mesh = _build_extended_plane(ocean_domain_size, ocean_grid_resolution * mesh_detail, render_extent);
	}
	if (water_mesh.is_null()) {
		Ref<PlaneMesh> plane;
		plane.instantiate();
		plane->set_size(ocean_domain_size);
		// Subdivisions, not vertex count -- PlaneMesh's own convention.
		plane->set_subdivide_width(ocean_grid_resolution * mesh_detail);
		plane->set_subdivide_depth(ocean_grid_resolution * mesh_detail);
		water_mesh = plane;
	}
	mesh_instance->set_mesh(water_mesh);
	mesh_offset = Vector2(Math::INF, Math::INF); // re-applied for the new mesh on the next update
	_update_cull_box();

	const Vector3 origin = get_global_position();
	grid_center = Vector2(origin.x, origin.z) + local_center;

	water_material->set_shader_parameter("ripple_domain_size", active_domain_size);
	water_material->set_shader_parameter("ocean_domain_size", ocean_domain_size);
	water_material->set_shader_parameter("grid_center", grid_center);
	water_material->set_shader_parameter("open_sea_beyond", open_sea_beyond);
	water_material->set_shader_parameter("water_level", water_level);
	water_material->set_shader_parameter("choppiness", choppiness);
	water_material->set_shader_parameter("per_pixel_normals", normal_mode == NORMAL_MODE_PER_PIXEL);
	flow_cells.clear(); // solved again for the new square once its seabed is known (_configure_solver())
	cached_live_velocity.clear();
	_update_flow_material();

	// NOTIFICATION_INTERNAL_PHYSICS_PROCESS fires in the editor too, not just
	// Play -- same gotcha PhysXVehicle3D/PhysXDestructible3D already guard
	// against (see PhysXVehicle3D::_build()'s own comment on it). Without
	// this, just having the scene open dispatches real GPU compute work
	// every physics tick, which is exactly what a user reported seeing (the
	// water visibly animating while only hovering nodes in the editor,
	// followed by an editor crash likely from the render-thread dispatch
	// getting into an inconsistent state under the editor's non-realtime
	// update loop). No solver setup at all in the editor -- the node shows
	// a static flat mesh there instead, matching this class's own doc
	// comment ("only runs in an actual running game").
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	if (seabed_from_floor) {
		// Rays need the floor already in the physics space -- defer to the
		// first physics tick (see _update()).
		seabed_pending = true;
		return;
	}
	if (!footprint_mask.is_empty()) {
		const int cells = grid_resolution * grid_resolution;
		cell_depth.resize(cells);
		float *dw = cell_depth.ptrw();
		for (int i = 0; i < cells; i++) {
			dw[i] = footprint_mask[i] ? depth : WaterSolver::WALL_DEPTH;
		}
	}
	_configure_solver();
}

void PhysXWaterSurface3D::_sample_seabed() {
	// One ray per ripple cell center, straight down from well above the water
	// onto static bodies only: anything else it hits (a floater, a player) is
	// excluded and the ray cast again. The seabed can rise above the water --
	// that's dry beach, depth <= 0.
	constexpr real_t PROBE_ABOVE = 100.0;
	constexpr real_t PROBE_BELOW = 1000.0;
	constexpr int MAX_SKIPS = 8;
	const int n = grid_resolution;
	cell_depth.resize(n * n);
	float *dw = cell_depth.ptrw();
	const real_t surface_y = get_global_position().y + water_level;
	const Vector2 cell = active_domain_size / (real_t)n;
	const Vector2 grid_min = grid_center - active_domain_size * 0.5f;
	PhysicsDirectSpaceState3D *space = get_world_3d()->get_direct_space_state();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	int hits = 0;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (!footprint_mask.is_empty() && !footprint_mask[i]) {
				dw[i] = WaterSolver::WALL_DEPTH;
				continue;
			}
			const Vector2 p = grid_min + Vector2(x + 0.5f, z + 0.5f) * cell;
			PhysicsDirectSpaceState3D::RayParameters ray;
			ray.from = Vector3(p.x, surface_y + PROBE_ABOVE, p.y);
			ray.to = Vector3(p.x, surface_y - PROBE_BELOW, p.y);
			ray.collision_mask = seabed_collision_mask;
			ray.collide_with_areas = false;
			float d = depth; // nothing below: open water of the constant depth
			for (int skip = 0; skip < MAX_SKIPS; skip++) {
				PhysicsDirectSpaceState3D::RayResult result;
				if (space == nullptr || !space->intersect_ray(ray, result)) {
					break;
				}
				if (ps->body_get_mode(result.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
					d = surface_y - result.position.y;
					hits++;
					break;
				}
				ray.exclude.insert(result.rid);
			}
			dw[i] = d;
		}
	}
	if (hits == 0) {
		WARN_PRINT("PhysXWaterSurface3D: seabed_from_floor found no static body under the water (check seabed_collision_mask); using the constant depth.");
	}
}

void PhysXWaterSurface3D::_update_cell_fade() {
	cell_fade.resize(cell_depth.size());
	float *fw = cell_fade.ptrw();
	for (int i = 0; i < cell_depth.size(); i++) {
		fw[i] = WaterSolver::shallow_fade(cell_depth[i], shallow_fade_depth);
	}
	_bake_shore_waves();
}

void PhysXWaterSurface3D::_configure_solver() {
	_update_cell_fade();
	WaterSolver::Settings s;
	s.grid_resolution = grid_resolution;
	s.domain_size = active_domain_size;
	s.grid_center = grid_center;
	s.cell_depth = cell_depth;
	s.shallow_fade_depth = shallow_fade_depth;
	s.depth = depth;
	s.damping = damping;
	s.ripple_amplitude = ripple_amplitude;
	s.gravity = 9.81f;
	s.water_level = water_level;
	s.ocean_grid_resolution = ocean_grid_resolution;
	s.ocean_domain_size = ocean_domain_size;
	s.wind_speed = wind_speed;
	s.wind_direction = wind_direction;
	s.wave_amplitude = wave_amplitude;
	s.fetch = get_effective_fetch();
	s.caustics_enabled = caustics_enabled;
	s.ocean_k_min = wave_cascades > 1 ? _cascade_split(0) : 0.0f;
	s.ocean_k_max = 0.0f;
	s.wave_spreading = int(wave_spreading);
	s.swell = swell;
	// configure() frees the solver's textures and makes new ones: let go of
	// the old ones first (a runtime change -- wave_amplitude, say -- drew with
	// freed textures until the new ones were bound).
	_detach_textures();
	solver.configure(s);
	_configure_cascades(s);
	solver.set_foam_settings(foam_enabled, choppiness, _live_foam_threshold(), foam_persistence, shore_foam_band, shore_undertow);
	solver.set_swash_settings(swash_run_up, swash_drain_speed, wet_sand_dry_time);
	solver.set_flow_foam(flow_foam, flow_foam_persistence);
	if (!solver.has_device() && !warned_no_device) {
		warned_no_device = true;
		WARN_PRINT("PhysXWaterSurface3D: the water compute solver could not start (no RenderingDevice / compute support).");
	}

	previous_ripple_height.clear();
	previous_ocean_height.clear();
	frames_since_refresh = REFRESH_EVERY_FRAMES; // force an immediate CPU cache refresh on the next _update()
	_bake_flow();
}

void PhysXWaterSurface3D::_detach_textures() {
	const Ref<Texture2DRD> textures[] = { ripple_height_tex, ocean_height_tex, ocean_fade_tex, shore_depth_tex, ocean_disp_tex, ocean_deriv_tex, ocean_foam_tex, shore_foam_tex, swash_tex, caustics_texture, live_flow_texture };
	for (const Ref<Texture2DRD> &texture : textures) {
		if (texture.is_valid()) {
			texture->set_texture_rd_rid(RID());
		}
	}
	for (WaveCascade *c : extra_cascades) {
		const Ref<Texture2DRD> textures_c[] = { c->height_tex, c->disp_tex, c->deriv_tex, c->foam_tex };
		for (const Ref<Texture2DRD> &texture : textures_c) {
			if (texture.is_valid()) {
				texture->set_texture_rd_rid(RID());
			}
		}
	}
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("wave_cascade_count", 1);
	}
	textures_bound = false; // rebound once the solver's (new) textures exist
	_update_flow_material(); // (the baked flow until then)
}

void PhysXWaterSurface3D::_update_mesh_offset() {
	// Open water past the simulated square: the mesh's vertices spread out
	// with distance from its dense core, and far out they're meters apart --
	// too coarse for the short waves, so what's drawn there missed what
	// sample_height() (and so a boat) sees by tenths of a meter. The core
	// follows the camera instead, smoothly, every frame: near the camera the
	// dense vertices sample the waves finely wherever they sit, and the
	// coarse surface past it reshapes gradually (~1 cm per 5 cm moved, 20-35
	// m out) instead of jumping. (Snapping popped: whole 40 m ocean tiles
	// re-meshed everything past the core at once -- 15 cm jumps -- and
	// core-spacing steps made a constant 6 cm shimmer.)
	if (mesh_instance == nullptr || water_material.is_null() || surface_mesh.is_valid() || render_extent <= MAX(ocean_domain_size.x, ocean_domain_size.y)) {
		return;
	}
	const Viewport *vp = get_viewport();
	const Camera3D *cam = vp != nullptr ? vp->get_camera_3d() : nullptr;
	if (cam == nullptr) {
		return;
	}
	const Vector3 cp = cam->get_global_position();
	const Vector2 rel = Vector2(cp.x, cp.z) - grid_center;
	const Vector2 offset = rel;
	if (offset == mesh_offset) {
		return;
	}
	mesh_offset = offset;
	water_material->set_shader_parameter("mesh_offset", mesh_offset);
	_update_cull_box();
}

// The mesh is flat and the shader displaces it, so culling needs room for the waves (and whatever else a material adds).
void PhysXWaterSurface3D::_update_cull_box() {
	if (mesh_instance == nullptr || water_mesh.is_null()) {
		return;
	}
	AABB box = water_mesh->get_aabb();
	if (mesh_offset.is_finite() && mesh_offset != Vector2()) {
		box.position += mesh_instance->get_global_transform().basis.xform_inv(Vector3(mesh_offset.x, 0, mesh_offset.y));
	}
	mesh_instance->set_custom_aabb(box.grow(MAX(wave_amplitude * 4.0f, 2.0f) * wave_cascades + cull_margin));
}

void PhysXWaterSurface3D::_update(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	water_time += p_delta;
	if (step_mode == STEP_MODE_PER_FRAME) {
		water_time_dirty = true; // the material gets it once, in _frame_update()
	} else if (water_material.is_valid()) {
		// Wrapped so float precision holds up in long sessions.
		water_material->set_shader_parameter("water_time", (float)Math::fmod(water_time, 3600.0));
	}
	if (seabed_pending) {
		seabed_pending = false;
		seabed_remeasure_pending = false; // just measured
		_sample_seabed();
		_configure_solver();
		return;
	}
	if (!solver.has_device()) {
		return;
	}
	// (Waits for a solver still being built, whose depth textures don't exist yet.)
	if (seabed_remeasure_pending && solver.is_available()) {
		seabed_remeasure_pending = false;
		if (seabed_from_floor) {
			_sample_seabed();
			_update_cell_fade();
			solver.update_seabed(cell_depth);
			_bake_flow();
		}
	}
	if (shore_waves) {
		// The swash reads the shore waves on the render thread, at this clock.
		solver.set_shore_clock(water_time, grid_center);
	}
	if (step_mode == STEP_MODE_PER_FRAME) {
		// Bodies / impulses submitted up to this tick are snapshotted with it; the GPU work waits for the frame.
		solver.queue_tick(p_delta); // no-ops until solver.is_available(), like step()
		for (WaveCascade *c : extra_cascades) {
			c->solver.queue_tick(p_delta);
		}
		// Readbacks land at the end of a frame: this tick's sample_height() sees them, as it does per tick.
		_refresh_cpu_cache();
		return;
	}
	solver.step(p_delta); // no-ops internally until solver.is_available() (see water_solver.cpp)
	for (WaveCascade *c : extra_cascades) {
		c->solver.step(p_delta);
	}
	_after_step();
}

void PhysXWaterSurface3D::_frame_update() {
	if (water_time_dirty) {
		water_time_dirty = false;
		if (water_material.is_valid()) {
			water_material->set_shader_parameter("water_time", (float)Math::fmod(water_time, 3600.0));
		}
	}
	if (!solver.has_queued_ticks()) {
		return; // no physics tick since the last frame: nothing moved
	}
	solver.flush_ticks();
	for (WaveCascade *c : extra_cascades) {
		c->solver.flush_ticks();
	}
	_after_step();
}

void PhysXWaterSurface3D::_after_step() {
	_steer_whitecaps();
	if (solver.is_available()) {
		bool cascades_ready = true;
		for (WaveCascade *c : extra_cascades) {
			cascades_ready = cascades_ready && c->solver.is_available();
		}
		if (!textures_bound && cascades_ready) {
			_bind_textures();
		}
	}
	if (solver.is_available() && caustics_enabled) {
		solver.render_caustics(Vector3(grid_center.x, get_global_position().y, grid_center.y), caustics_sun_direction, caustics_reference_depth, 1.333f, _open_beyond());
	}
	frames_since_refresh++;
	if (frames_since_refresh >= REFRESH_EVERY_FRAMES) {
		frames_since_refresh = 0;
		_refresh_cpu_cache();
	}
}

void PhysXWaterSurface3D::_bind_textures() {
	// Zero-copy: wrap the solver's own RD texture RIDs directly. Set once --
	// Texture2DRD holds the RID by reference, so whatever the blit shaders
	// write each step (see water_solver.cpp's rt_step()) is what the
	// material samples automatically; no per-frame reassignment needed.
	if (ripple_height_tex.is_null()) {
		ripple_height_tex.instantiate();
	}
	if (ocean_height_tex.is_null()) {
		ocean_height_tex.instantiate();
	}
	if (ocean_fade_tex.is_null()) {
		ocean_fade_tex.instantiate();
	}
	if (ocean_disp_tex.is_null()) {
		ocean_disp_tex.instantiate();
	}
	ocean_disp_tex->set_texture_rd_rid(solver.get_ocean_displacement_texture_rd_rid());
	if (ocean_deriv_tex.is_null()) {
		ocean_deriv_tex.instantiate();
	}
	ocean_deriv_tex->set_texture_rd_rid(solver.get_ocean_derivative_texture_rd_rid());
	if (ocean_foam_tex.is_null()) {
		ocean_foam_tex.instantiate();
	}
	ocean_foam_tex->set_texture_rd_rid(solver.get_ocean_foam_texture_rd_rid());
	if (live_flow_texture.is_null()) {
		live_flow_texture.instantiate();
	}
	live_flow_texture->set_texture_rd_rid(solver.get_velocity_texture_rd_rid());
	if (shore_foam_tex.is_null()) {
		shore_foam_tex.instantiate();
	}
	shore_foam_tex->set_texture_rd_rid(solver.get_shore_foam_texture_rd_rid());
	if (swash_tex.is_null()) {
		swash_tex.instantiate();
	}
	swash_tex->set_texture_rd_rid(solver.get_swash_texture_rd_rid());
	ripple_height_tex->set_texture_rd_rid(solver.get_ripple_height_texture_rd_rid());
	ocean_height_tex->set_texture_rd_rid(solver.get_ocean_height_texture_rd_rid());
	ocean_fade_tex->set_texture_rd_rid(solver.get_ocean_fade_texture_rd_rid());
	if (shore_depth_tex.is_null()) {
		shore_depth_tex.instantiate();
	}
	shore_depth_tex->set_texture_rd_rid(solver.get_shore_depth_texture_rd_rid());
	if (caustics_enabled) {
		if (caustics_texture.is_null()) {
			caustics_texture.instantiate();
		}
		caustics_texture->set_texture_rd_rid(solver.get_caustics_texture_rd_rid());
	}
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("ripple_height_tex", ripple_height_tex);
		water_material->set_shader_parameter("ocean_height_tex", ocean_height_tex);
		water_material->set_shader_parameter("ocean_fade_tex", ocean_fade_tex);
		water_material->set_shader_parameter("shore_depth_tex", shore_depth_tex);
		water_material->set_shader_parameter("ocean_disp_tex", ocean_disp_tex);
		water_material->set_shader_parameter("ocean_deriv_tex", ocean_deriv_tex);
		water_material->set_shader_parameter("ocean_foam_tex", ocean_foam_tex);
		water_material->set_shader_parameter("shore_foam_tex", shore_foam_tex);
		water_material->set_shader_parameter("swash_tex", swash_tex);
	}
	for (int i = 0; i < (int)extra_cascades.size(); i++) {
		WaveCascade *c = extra_cascades[i];
		const Ref<Texture2DRD> *slots[4] = { &c->height_tex, &c->disp_tex, &c->deriv_tex, &c->foam_tex };
		const RID rids[4] = { c->solver.get_ocean_height_texture_rd_rid(), c->solver.get_ocean_displacement_texture_rd_rid(), c->solver.get_ocean_derivative_texture_rd_rid(), c->solver.get_ocean_foam_texture_rd_rid() };
		const char *names[4] = { "cascade_height_", "cascade_disp_", "cascade_deriv_", "cascade_foam_" };
		for (int k = 0; k < 4; k++) {
			Ref<Texture2DRD> &t = const_cast<Ref<Texture2DRD> &>(*slots[k]);
			if (t.is_null()) {
				t.instantiate();
			}
			t->set_texture_rd_rid(rids[k]);
			if (water_material.is_valid()) {
				water_material->set_shader_parameter(String(names[k]) + itos(i + 1), t);
			}
		}
		if (water_material.is_valid()) {
			water_material->set_shader_parameter("cascade_domain_" + itos(i + 1), c->domain);
		}
	}
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("wave_cascade_count", 1 + (int)extra_cascades.size());
		water_material->set_shader_parameter("foam_crest_threshold", foam_enabled ? _live_foam_threshold() : -100.0f);
	}
	textures_bound = true;
	_update_flow_material(); // the live flow's texture exists now
	_update_shore_material();
}

void PhysXWaterSurface3D::_refresh_cpu_cache() {
	// CPU-side cache for sample_height() only -- rendering doesn't touch
	// this at all (see _bind_textures()). Non-blocking: just whatever the
	// solver's last async readback landed, a few frames stale -- fine for
	// buoyancy, see water_solver.h's own note on why this stays a separate
	// path from the zero-copy texture above.
	Vector<float> ripple;
	double ripple_time = cached_ripple_time;
	solver.get_height_grid(ripple, cached_ripple_n, cached_ripple_domain, &ripple_time);
	if (ripple_time != cached_ripple_time) {
		previous_ripple_height = cached_ripple_height;
		previous_ripple_time = cached_ripple_time;
		cached_ripple_time = ripple_time;
	}
	cached_ripple_height = ripple;
	if (live_flow) {
		solver.get_velocity_grid(cached_live_velocity);
	}
	Vector<float> ocean;
	Vector<float> ocean_dx;
	double ocean_time = cached_ocean_time;
	solver.get_ocean_height_grid(ocean, ocean_dx, cached_ocean_n, cached_ocean_domain, &ocean_time);
	if (ocean_time != cached_ocean_time) {
		previous_ocean_height = cached_ocean_height;
		previous_ocean_dx = cached_ocean_dx;
		previous_ocean_dz = cached_ocean_dz;
		previous_ocean_time = cached_ocean_time;
		cached_ocean_time = ocean_time;
	}
	cached_ocean_height = ocean;
	cached_ocean_dx = ocean_dx;
	solver.get_ocean_dz_grid(cached_ocean_dz);
	for (WaveCascade *c : extra_cascades) {
		Vector<float> h;
		Vector<float> dx;
		double t = c->time;
		c->solver.get_ocean_height_grid(h, dx, c->n, c->cached_domain, &t);
		if (t != c->time) {
			c->previous_height = c->height;
			c->previous_dx = c->dx;
			c->previous_dz = c->dz;
			c->previous_time = c->time;
			c->time = t;
		}
		c->height = h;
		c->dx = dx;
		c->solver.get_ocean_dz_grid(c->dz);
	}
}

float PhysXWaterSurface3D::_bilinear_sample(const Vector<float> &p_grid, int p_n, Vector2 p_domain, float p_world_x, float p_world_z, bool p_repeat) const {
	if (p_grid.is_empty() || p_n <= 0) {
		return 0.0f;
	}
	const float gx = (p_world_x / (p_domain.x * 0.5f) + 1.0f) * 0.5f * p_n - 0.5f;
	const float gz = (p_world_z / (p_domain.y * 0.5f) + 1.0f) * 0.5f * p_n - 0.5f;
	int x0, z0, x1, z1;
	float fx, fz;
	if (p_repeat) {
		const float flx = floorf(gx);
		const float flz = floorf(gz);
		auto wrap = [p_n](int p_i) { return ((p_i % p_n) + p_n) % p_n; };
		x0 = wrap((int)flx);
		z0 = wrap((int)flz);
		x1 = wrap((int)flx + 1);
		z1 = wrap((int)flz + 1);
		fx = gx - flx;
		fz = gz - flz;
	} else {
		x0 = CLAMP((int)floorf(gx), 0, p_n - 1);
		z0 = CLAMP((int)floorf(gz), 0, p_n - 1);
		x1 = CLAMP(x0 + 1, 0, p_n - 1);
		z1 = CLAMP(z0 + 1, 0, p_n - 1);
		fx = CLAMP(gx - x0, 0.0f, 1.0f);
		fz = CLAMP(gz - z0, 0.0f, 1.0f);
	}
	const float h00 = p_grid[z0 * p_n + x0];
	const float h10 = p_grid[z0 * p_n + x1];
	const float h01 = p_grid[z1 * p_n + x0];
	const float h11 = p_grid[z1 * p_n + x1];
	return Math::lerp(Math::lerp(h00, h10, fx), Math::lerp(h01, h11, fx), fz);
}

float PhysXWaterSurface3D::sample_height(Vector3 p_world_pos) const {
	return sample_height_excluding_wake(p_world_pos, ObjectID());
}

// The ripple layer's height at grid-relative (x, z). Past the simulated square the ripples ease back to still water, as
// the shader draws them.
float PhysXWaterSurface3D::_ripple_height_at(const Vector<float> &p_height, float p_x, float p_z) const {
	const float from_center = MAX(Math::abs(p_x / cached_ripple_domain.x), Math::abs(p_z / cached_ripple_domain.y));
	const float inside = 1.0f - Math::smoothstep(0.46f, 0.5f, from_center);
	return Math::lerp(water_level, _bilinear_sample(p_height, cached_ripple_n, cached_ripple_domain, p_x, p_z), inside);
}

float PhysXWaterSurface3D::_ocean_fade_at(float p_x, float p_z) const {
	const float fade = cell_fade.is_empty() ? 1.0f : _bilinear_sample(cell_fade, grid_resolution, active_domain_size, p_x, p_z);
	// Open sea past the square: full chop there, not the edge cell's (the shader's fade_r()).
	const float beyond = (open_sea_beyond && _open_beyond()) ? Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(p_x / active_domain_size.x), Math::abs(p_z / active_domain_size.y))) : 0.0f;
	return Math::lerp(fade, 1.0f, beyond);
}

// With choppy displacement the surface point above (x, z) rested somewhere else: find p0 with p0 + s * D(p0) = (x, z) by
// fixed-point iteration (s matches the shader's -choppiness * fade), then read the ocean height there.
// (The ocean layer is anchored to the world, as the shader reads it: its samples are at world xz, so a re-centre of the
// ripple grid never slides the waves a boat rides.)
float PhysXWaterSurface3D::_ocean_height_at(bool p_previous, float p_x, float p_z, float p_fade) const {
	struct Layer {
		const Vector<float> *height;
		const Vector<float> *dx;
		const Vector<float> *dz;
		int n;
		Vector2 domain;
	};
	Layer layers[MAX_WAVE_CASCADES];
	int count = 0;
	if (p_previous) {
		layers[count++] = { &previous_ocean_height, &previous_ocean_dx, &previous_ocean_dz, cached_ocean_n, cached_ocean_domain };
	} else {
		layers[count++] = { &cached_ocean_height, &cached_ocean_dx, &cached_ocean_dz, cached_ocean_n, cached_ocean_domain };
	}
	for (const WaveCascade *c : extra_cascades) {
		if (count >= MAX_WAVE_CASCADES) {
			break;
		}
		// A cascade without a previous readback yet stands still (adds no speed).
		const bool prev = p_previous && c->previous_height.size() == c->height.size();
		layers[count++] = prev ? Layer{ &c->previous_height, &c->previous_dx, &c->previous_dz, c->n, c->cached_domain } : Layer{ &c->height, &c->dx, &c->dz, c->n, c->cached_domain };
	}
	Vector2 p0(p_x, p_z);
	const float s = -choppiness * p_fade;
	// Carried by the flow as the shader draws it (ocean_tex()); the phases change slowly over p0's few
	// centimetres of iteration, so they are taken at the point itself.
	Vector2 oa, ob;
	const float w = _flow_phases(p_x, p_z, oa, ob);
	if (s != 0.0f) {
		for (int it = 0; it < 4; it++) {
			Vector2 d;
			for (int l = 0; l < count; l++) {
				const Layer &L = layers[l];
				if (L.dx->size() == L.height->size() && L.dz->size() == L.height->size() && !L.height->is_empty()) {
					d += Vector2(_ocean_flow_sample(*L.dx, p0.x, p0.y, w, oa, ob, L.n, L.domain), _ocean_flow_sample(*L.dz, p0.x, p0.y, w, oa, ob, L.n, L.domain));
				}
			}
			p0 = Vector2(p_x, p_z) - d * s;
		}
	}
	float h = 0.0f;
	for (int l = 0; l < count; l++) {
		const Layer &L = layers[l];
		h += _ocean_flow_sample(*L.height, p0.x, p0.y, w, oa, ob, L.n, L.domain);
	}
	return h * p_fade;
}

float PhysXWaterSurface3D::sample_vertical_speed(Vector3 p_world_pos) const {
	if (!is_wet(p_world_pos)) {
		return 0.0f;
	}
	const float x = p_world_pos.x - grid_center.x;
	const float z = p_world_pos.z - grid_center.y;
	float speed = 0.0f;
	const double ripple_dt = cached_ripple_time - previous_ripple_time;
	// A resting ripple layer has no new readbacks: its last two say nothing about now.
	if (!solver.is_ripple_resting() && ripple_dt > 0.0 && !cached_ripple_height.is_empty() && previous_ripple_height.size() == cached_ripple_height.size()) {
		speed += (_ripple_height_at(cached_ripple_height, x, z) - _ripple_height_at(previous_ripple_height, x, z)) / (float)ripple_dt;
	}
	{
		float rate = 0.0f;
		_shore_eta_at(x, z, &rate);
		speed += rate;
	}
	const double ocean_dt = cached_ocean_time - previous_ocean_time;
	if (ocean_dt > 0.0 && !cached_ocean_height.is_empty() && previous_ocean_height.size() == cached_ocean_height.size()) {
		const float fade = _ocean_fade_at(x, z);
		speed += (_ocean_height_at(false, x, z, fade) - _ocean_height_at(true, x, z, fade)) / (float)ocean_dt;
	}
	return speed;
}

int PhysXWaterSurface3D::register_wake(ObjectID p_wake) {
	for (int i = 0; i < MAX_WAKES; i++) {
		if (wakes[i] == p_wake || ObjectDB::get_instance(wakes[i]) == nullptr) {
			wakes[i] = p_wake;
			return i;
		}
	}
	return -1;
}

void PhysXWaterSurface3D::unregister_wake(int p_slot) {
	ERR_FAIL_INDEX(p_slot, MAX_WAKES);
	wakes[p_slot] = ObjectID();
	if (water_material.is_valid()) {
		water_material->set_shader_parameter("wake_height_" + itos(p_slot), Variant());
		water_material->set_shader_parameter("wake_rect_" + itos(p_slot), Vector4());
	}
}

void PhysXWaterSurface3D::set_wake_slot(int p_slot, const Ref<Texture2D> &p_height, const Vector4 &p_rect) {
	ERR_FAIL_INDEX(p_slot, MAX_WAKES);
	if (water_material.is_null()) {
		return;
	}
	const String i = itos(p_slot);
	if (water_material->get_shader_parameter("wake_height_" + i) != Variant(p_height)) {
		water_material->set_shader_parameter("wake_height_" + i, p_height);
	}
	water_material->set_shader_parameter("wake_rect_" + i, p_rect);
}

float PhysXWaterSurface3D::_sample_height_excluding_wake_bind(Vector3 p_world_pos, Object *p_wake) const {
	return sample_height_excluding_wake(p_world_pos, p_wake != nullptr ? p_wake->get_instance_id() : ObjectID());
}

float PhysXWaterSurface3D::sample_height_excluding_wake(Vector3 p_world_pos, ObjectID p_wake) const {
	return _sample_height_at(p_world_pos, p_wake, get_global_position().y);
}

// sample_height_excluding_wake with the node's height given (safe off the main thread).
float PhysXWaterSurface3D::_sample_height_at(Vector3 p_world_pos, ObjectID p_wake, float p_origin_y) const {
	if (!is_wet(p_world_pos)) {
		return -Math::INF;
	}
	const float x = p_world_pos.x - grid_center.x;
	const float z = p_world_pos.z - grid_center.y;
	// The ripple readback already includes water_level (it's the composed
	// height the renderer draws), so only fall back to it before the first
	// readback lands. Both are relative to the node, like the rendered mesh.
	const float ripple = cached_ripple_height.is_empty() ? water_level : _ripple_height_at(cached_ripple_height, x, z);
	const float fade = _ocean_fade_at(x, z);
	const float ocean = _ocean_height_at(false, x, z, fade);
	const float beyond = (open_sea_beyond && _open_beyond()) ? Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(x / active_domain_size.x), Math::abs(z / active_domain_size.y))) : 0.0f;
	// Wakes, everywhere (as the shader draws them).
	float wake = 0.0f;
	for (int i = 0; i < MAX_WAKES; i++) {
		if (wakes[i] == p_wake) {
			continue;
		}
		const PhysXWaterWake3D *w = ObjectDB::get_instance<PhysXWaterWake3D>(wakes[i]);
		if (w != nullptr) {
			const float out = Math::smoothstep(0.46f, 0.5f, MAX(Math::abs(x / active_domain_size.x), Math::abs(z / active_domain_size.y)));
			wake += w->sample_height(p_world_pos) * Math::lerp(1.0f, out, w->get_shore_grid_fade());
		}
	}
	// Not over dry sand (as the shader): fades out over the last 30 cm of depth.
	if (wake != 0.0f && !cell_depth.is_empty()) {
		wake *= Math::smoothstep(0.0f, 0.3f, Math::lerp(_bilinear_sample(cell_depth, grid_resolution, active_domain_size, x, z), 1000.0f, beyond));
	}
	return p_origin_y + ripple + ocean + wake + _shore_eta_at(x, z);
}

bool PhysXWaterSurface3D::_open_beyond() const {
	return surface_mesh.is_null() && render_extent > MAX(domain_size.x, domain_size.y);
}

bool PhysXWaterSurface3D::_inside_render_extent(float p_rel_x, float p_rel_z) const {
	return Math::abs(p_rel_x) <= render_extent * 0.5f && Math::abs(p_rel_z) <= render_extent * 0.5f;
}

bool PhysXWaterSurface3D::is_wet(Vector3 p_world_pos) const {
	const float rel_x = p_world_pos.x - grid_center.x;
	const float rel_z = p_world_pos.z - grid_center.y;
	if (_open_beyond() && !_inside_render_extent(rel_x, rel_z)) {
		return false; // past the drawn surface
	}
	if (cell_depth.is_empty()) {
		return true;
	}
	const int n = grid_resolution;
	if (cell_depth.size() != n * n) {
		return false;
	}
	int gx = (int)Math::floor((rel_x / active_domain_size.x + 0.5f) * n);
	int gz = (int)Math::floor((rel_z / active_domain_size.y + 0.5f) * n);
	if (gx < 0 || gz < 0 || gx >= n || gz >= n) {
		if (!_open_beyond()) {
			return false;
		}
		if (open_sea_beyond) {
			return true; // open, deep sea past the square
		}
		// Open water past the simulated square carries the edge's seabed on
		// outward, as the shader draws it: the sea side stays sea, the beach
		// side stays sand.
		gx = CLAMP(gx, 0, n - 1);
		gz = CLAMP(gz, 0, n - 1);
	}
	return cell_depth[gz * n + gx] > 0.0f;
}

Vector<Vector2> PhysXWaterSurface3D::_collect_footprint() const {
	Vector<Vector2> tris;
	if (surface_mesh.is_null()) {
		return tris;
	}
	// Only up-facing triangles: a closed mesh (a baked CSG disc has a top and
	// a bottom) would otherwise cover its footprint twice. A mesh with no
	// up-facing triangles at all (flipped winding) falls back to every face.
	const real_t up = plane_mesh_xz_winding();
	const Vector<Face3> faces = surface_mesh->get_faces();
	Vector<Vector2> any_facing;
	for (const Face3 &f : faces) {
		const Vector2 a(f.vertex[0].x, f.vertex[0].z);
		const Vector2 b(f.vertex[1].x, f.vertex[1].z);
		const Vector2 c(f.vertex[2].x, f.vertex[2].z);
		const real_t area = (b - a).cross(c - a);
		if (Math::abs(area) < (real_t)1e-8) {
			continue; // no X/Z area (degenerate, or a vertical face)
		}
		Vector<Vector2> &dst = (area * up > 0) ? tris : any_facing;
		dst.push_back(a);
		dst.push_back(b);
		dst.push_back(c);
	}
	if (tris.is_empty()) {
		return any_facing;
	}
	return tris;
}

PackedByteArray PhysXWaterSurface3D::_rasterize_wet_mask(const Vector<Vector2> &p_tris, Vector2 p_center, Vector2 p_domain, int p_n) {
	// A cell is wet when its center lies inside any footprint triangle -- the
	// same cell-centre convention as water_ripple.glsl's xz.
	PackedByteArray mask;
	mask.resize(p_n * p_n);
	memset(mask.ptrw(), 0, mask.size());
	uint8_t *mw = mask.ptrw();
	const Vector2 cell = p_domain / (real_t)p_n;
	const Vector2 grid_min = p_center - p_domain * 0.5f;
	for (int t = 0; t + 2 < p_tris.size(); t += 3) {
		const Vector2 a = p_tris[t], b = p_tris[t + 1], c = p_tris[t + 2];
		const Vector2 lo = a.min(b).min(c);
		const Vector2 hi = a.max(b).max(c);
		const int x0 = CLAMP((int)Math::floor((lo.x - grid_min.x) / cell.x), 0, p_n - 1);
		const int x1 = CLAMP((int)Math::floor((hi.x - grid_min.x) / cell.x), 0, p_n - 1);
		const int z0 = CLAMP((int)Math::floor((lo.y - grid_min.y) / cell.y), 0, p_n - 1);
		const int z1 = CLAMP((int)Math::floor((hi.y - grid_min.y) / cell.y), 0, p_n - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 p = grid_min + Vector2(x + 0.5f, z + 0.5f) * cell;
				if (point_in_triangle(p, a, b, c)) {
					mw[z * p_n + x] = 1;
				}
			}
		}
	}
	return mask;
}

Ref<Mesh> PhysXWaterSurface3D::_build_extended_plane(Vector2 p_inner, int p_inner_cells, real_t p_extent) const {
	// A regular grid of (2K+1)^2 vertices whose square rings are spread out
	// from the center: rings up to K0 are the simulated span at uniform
	// spacing, and each ring past it is 15% further out than the step before
	// -- detail where the waves are simulated, a few hundred rings out to the
	// horizon. Spacing grows with the ring (distance from the center), the
	// same along X and Z. (A per-axis spacing left the simulated span's fine
	// columns/rows running out to the horizon as two strips of fine mesh
	// along the axes, and the same waves looked different either side of
	// each strip's edge: a seam straight out to sea.)
	const int k0 = MAX(p_inner_cells / 2, 1);
	const Vector2 half_inner = p_inner * 0.5f;
	const real_t max_half = MAX(half_inner.x, half_inner.y);
	// radius[c]: ring c's half-size, in units of the inner step (ring c <= k0: c).
	LocalVector<real_t> radius;
	for (int c = 0; c <= k0; c++) {
		radius.push_back((real_t)c);
	}
	{
		const real_t limit = (p_extent * 0.5f) / max_half * (real_t)k0;
		real_t r = (real_t)k0;
		real_t step = 1.0f;
		while (r < limit) {
			step *= 1.15f;
			r = MIN(r + step, limit);
			radius.push_back(r);
		}
	}
	const int kmax = (int)radius.size() - 1;
	const int nx = 2 * kmax + 1;
	const int nz = nx;
	auto position = [&](int p_i, int p_j) {
		const int ai = p_i < 0 ? -p_i : p_i;
		const int aj = p_j < 0 ? -p_j : p_j;
		const int ring = ai > aj ? ai : aj;
		const real_t scale = ring == 0 ? (real_t)1.0 : radius[ring] / (real_t)ring;
		return Vector2((real_t)p_i * scale * half_inner.x / (real_t)k0, (real_t)p_j * scale * half_inner.y / (real_t)k0);
	};

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	verts.resize(nx * nz);
	normals.resize(nx * nz);
	uvs.resize(nx * nz);
	for (int j = 0; j < nz; j++) {
		for (int i = 0; i < nx; i++) {
			const int k = j * nx + i;
			const Vector2 xz = position(i - kmax, j - kmax);
			verts.set(k, Vector3(xz.x, 0.0f, xz.y));
			normals.set(k, Vector3(0, 1, 0));
			uvs.set(k, xz / p_extent + Vector2(0.5f, 0.5f));
		}
	}
	const bool flip = plane_mesh_xz_winding() < 0;
	PackedInt32Array indices;
	indices.resize((nx - 1) * (nz - 1) * 6);
	int w = 0;
	for (int j = 0; j < nz - 1; j++) {
		for (int i = 0; i < nx - 1; i++) {
			const int a = j * nx + i;
			const int b = a + 1;
			const int c = a + nx;
			const int d = c + 1;
			// (a, b, d) has a positive X/Z cross product; flip to PlaneMesh's.
			const int tri[6] = { a, flip ? d : b, flip ? b : d, a, flip ? c : d, flip ? d : c };
			for (int t = 0; t < 6; t++) {
				indices.set(w++, tri[t]);
			}
		}
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

Ref<Mesh> PhysXWaterSurface3D::_build_footprint_mesh(const Vector<Vector2> &p_tris, const Rect2 &p_bounds, int p_cells) const {
	// Resample the footprint into an even grid so the height fields have
	// vertices to displace everywhere (a CSG disc is a fan of long slivers
	// with no interior vertices). Cells no triangle edge passes through are
	// whole quads, kept or dropped by their center; cells an edge touches are
	// clipped against each overlapping triangle. A quad cell's edges are
	// never crossed by a triangle edge, so they carry no extra vertices and
	// meet their clipped neighbors without T-junctions.
	const real_t side = MAX(p_bounds.size.x, p_bounds.size.y);
	const real_t cs = side / (real_t)MAX(p_cells, 1);
	const int nx = MAX((int)Math::ceil(p_bounds.size.x / cs), 1);
	const int nz = MAX((int)Math::ceil(p_bounds.size.y / cs), 1);
	const Vector2 origin = p_bounds.position;

	const int tri_count = p_tris.size() / 3;
	LocalVector<LocalVector<int>> cell_tris;
	cell_tris.resize(nx * nz);
	LocalVector<uint8_t> edge_cell;
	edge_cell.resize(nx * nz);
	memset(edge_cell.ptr(), 0, edge_cell.size());
	const real_t eps = cs * (real_t)1e-4;
	auto cell_range = [&](const Vector2 &p_lo, const Vector2 &p_hi, int &r_x0, int &r_x1, int &r_z0, int &r_z1) {
		r_x0 = CLAMP((int)Math::floor((p_lo.x - eps - origin.x) / cs), 0, nx - 1);
		r_x1 = CLAMP((int)Math::floor((p_hi.x + eps - origin.x) / cs), 0, nx - 1);
		r_z0 = CLAMP((int)Math::floor((p_lo.y - eps - origin.y) / cs), 0, nz - 1);
		r_z1 = CLAMP((int)Math::floor((p_hi.y + eps - origin.y) / cs), 0, nz - 1);
	};
	for (int t = 0; t < tri_count; t++) {
		const Vector2 v[3] = { p_tris[t * 3], p_tris[t * 3 + 1], p_tris[t * 3 + 2] };
		int x0, x1, z0, z1;
		cell_range(v[0].min(v[1]).min(v[2]), v[0].max(v[1]).max(v[2]), x0, x1, z0, z1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				cell_tris[z * nx + x].push_back(t);
			}
		}
		for (int e = 0; e < 3; e++) {
			const Vector2 p0 = v[e], p1 = v[(e + 1) % 3];
			cell_range(p0.min(p1), p0.max(p1), x0, x1, z0, z1);
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 bmin = origin + Vector2(x, z) * cs - Vector2(eps, eps);
					const Vector2 bmax = origin + Vector2(x + 1, z + 1) * cs + Vector2(eps, eps);
					if (segment_touches_box(p0, p1, bmin, bmax)) {
						edge_cell[z * nx + x] = 1;
					}
				}
			}
		}
	}

	const real_t plane_winding = plane_mesh_xz_winding();

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	HashMap<Vector2i, int> vertex_ids;
	auto vertex_id = [&](const Vector2 &p_v) {
		const Vector2i key((int)Math::round(p_v.x * 10000.0f), (int)Math::round(p_v.y * 10000.0f));
		if (const int *found = vertex_ids.getptr(key)) {
			return *found;
		}
		const int id = verts.size();
		verts.push_back(Vector3(p_v.x, 0.0f, p_v.y));
		normals.push_back(Vector3(0, 1, 0));
		uvs.push_back((p_v - origin) / side);
		vertex_ids.insert(key, id);
		return id;
	};
	auto add_triangle = [&](const Vector2 &p_a, Vector2 p_b, Vector2 p_c) {
		const real_t area = (p_b - p_a).cross(p_c - p_a);
		if (Math::abs(area) < cs * cs * (real_t)1e-6) {
			return;
		}
		if ((area < 0) != (plane_winding < 0)) {
			SWAP(p_b, p_c);
		}
		indices.push_back(vertex_id(p_a));
		indices.push_back(vertex_id(p_b));
		indices.push_back(vertex_id(p_c));
	};

	LocalVector<Vector2> poly;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const int ci = z * nx + x;
			const Vector2 c00 = origin + Vector2(x, z) * cs;
			const Vector2 c10 = origin + Vector2(x + 1, z) * cs;
			const Vector2 c11 = origin + Vector2(x + 1, z + 1) * cs;
			const Vector2 c01 = origin + Vector2(x, z + 1) * cs;
			if (!edge_cell[ci]) {
				const Vector2 center = (c00 + c11) * 0.5f;
				bool inside = false;
				for (int t : cell_tris[ci]) {
					if (point_in_triangle(center, p_tris[t * 3], p_tris[t * 3 + 1], p_tris[t * 3 + 2])) {
						inside = true;
						break;
					}
				}
				if (inside) {
					add_triangle(c00, c10, c11);
					add_triangle(c00, c11, c01);
				}
				continue;
			}
			for (int t : cell_tris[ci]) {
				Vector2 tri[3] = { p_tris[t * 3], p_tris[t * 3 + 1], p_tris[t * 3 + 2] };
				if ((tri[1] - tri[0]).cross(tri[2] - tri[0]) < 0) {
					SWAP(tri[1], tri[2]);
				}
				poly.clear();
				poly.push_back(c00);
				poly.push_back(c10);
				poly.push_back(c11);
				poly.push_back(c01);
				clip_to_triangle(poly, tri);
				for (uint32_t i = 1; i + 1 < poly.size(); i++) {
					add_triangle(poly[0], poly[i], poly[i + 1]);
				}
			}
		}
	}

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	if (indices.is_empty()) {
		return mesh;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

void PhysXWaterSurface3D::submit_sphere(int p_owner, Vector3 p_world_pos, float p_radius, float p_strength) {
	solver.submit_sphere((uint64_t)p_owner, p_world_pos, p_radius, p_strength);
}

void PhysXWaterSurface3D::clear_sphere(int p_owner) {
	solver.clear_sphere((uint64_t)p_owner);
}

void PhysXWaterSurface3D::submit_impulse(Vector3 p_world_pos, float p_radius, float p_strength) {
	solver.submit_impulse(p_world_pos, p_radius, p_strength);
}

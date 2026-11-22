/**************************************************************************/
/*  rain_fall_3d.cpp                                                      */
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

#include "rain_fall_3d.h"

#include "weather_map_3d.h"

#include "modules/world/world_contracts.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/gpu_particles_collision_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

// Shared by the particle shader and the drop's material: sizes and the splash's shape.
static const char *RAIN_SHARED = R"(
uint rain_hash(uint x) {
	x ^= x >> 16u;
	x *= 2146121005u;
	x ^= x >> 15u;
	x *= 2221713035u;
	x ^= x >> 16u;
	return x;
}
float rain_rand(uint x) {
	return float(rain_hash(x) >> 8u) * (1.0 / 16777216.0);
}
// Atlas, Srivastava and Sekhon (1973): terminal speed (m/s) of a drop p_d mm across, at sea level.
float rain_terminal(float p_d) {
	return max(9.65 - 10.3 * exp(-0.6 * p_d), 0.05);
}
// A splash from a drop p_d mm across: (life s, width m, height m, launch speed of its droplets m/s). On a wet surface
// the crown's droplets leave at a fraction of the impact speed, about a third (Yarin 2006's review of splashing:
// secondary droplets at 0.2 to 0.5 of it), and land within a few tenths of a second; on deep water a Worthington jet
// rises about a centimetre per millimetre of drop and falls back (Worthington 1908; Castillo-Orozco et al. 2015).
vec4 rain_splash_shape(float p_d, bool p_water) {
	float v = rain_terminal(p_d);
	float u = 0.33 * v;
	float life = 2.0 * u * 0.85 / 9.81 + 0.02;
	float reach = u * u / 9.81;
	float rise = 0.5 * (u * 0.85) * (u * 0.85) / 9.81;
	if (p_water) {
		float vj = 0.45 * sqrt(p_d * 0.001 * v * 9.81 * 20.0);
		rise = max(rise * 0.4, vj * vj / (2.0 * 9.81) + 0.004);
		life = max(life * 0.6, 2.0 * vj / 9.81 + 0.03);
	}
	return vec4(life, 2.0 * reach + 0.01, rise + 0.01, u);
}
)";

// The particles: falling drops (and their splashes), or drips (drip_mode) from the sources.
static const char *RAIN_PROCESS = R"(
uniform vec3 box_center;
uniform vec3 box_size = vec3(24.0, 14.0, 24.0);
uniform vec3 eye;
uniform float pixel_angle = 0.001;
uniform float ratio = 0.0;
uniform float weight = 1.0; // drops each drawn streak stands for
uniform vec3 still_wind;
uniform sampler2D ground_tex : filter_linear, repeat_disable;
uniform vec4 ground_rect;
uniform float lambda = 2.5; // Marshall-Palmer slope, per mm
uniform float d_min = 0.5; // mm
uniform float size_scale = 1.0;
uniform float streak_time = 0.0167;
uniform bool splashes = true;
uniform float turb_sigma = 0.0;
uniform float turb_time = 0.0;
uniform bool drip_mode = false;
uniform sampler2D source_tex : filter_nearest, repeat_disable; // x, y, z, cumulative weight
uniform int source_count = 0;
uniform float drip_d = 4.5; // mm
uniform float drip_idle = 1e6; // s, mean wait between a drip particle's drops

// The stop height (snow or water) and whether it is water there (the alpha channel).
vec2 rain_ground(vec2 p) {
	vec2 uv = (p - ground_rect.xy) / max(ground_rect.zw, vec2(1e-4));
	if (ground_rect.z <= 0.0 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
		return vec2(-1e9, 0.0);
	}
	vec4 t = textureLod(ground_tex, uv, 0.0);
	return vec2(t.r, t.g);
}
// Turbulent eddies over the weather wind, as SnowFall3D (Kraichnan's random Fourier modes, sigma_u about 2.5 u*).
vec3 rain_eddies(vec3 p, vec3 mean) {
	if (turb_sigma <= 0.0) {
		return vec3(0.0);
	}
	vec3 q = p - mean * turb_time;
	vec3 u = vec3(0.0);
	for (int n = 0; n < 6; n++) {
		uint s = rain_hash(uint(n) * 7919u + 101u);
		float ct = 2.0 * rain_rand(s) - 1.0;
		float ph = 6.2831853 * rain_rand(s + 1u);
		vec3 dir = normalize(vec3(sqrt(1.0 - ct * ct) * cos(ph), ct * 0.5, sqrt(1.0 - ct * ct) * sin(ph)));
		float len = mix(2.0, 14.0, float(n) / 5.0);
		vec3 k = dir * (6.2831853 / len);
		vec3 a = normalize(cross(dir, vec3(rain_rand(s + 2u) - 0.5, rain_rand(s + 3u) - 0.5, rain_rand(s + 4u) - 0.5)));
		u += a * cos(dot(k, q) + 6.2831853 * rain_rand(s + 5u) + turb_time * 0.4 * length(k));
	}
	return u * turb_sigma * 0.7071 * sqrt(3.0 / 6.0);
}
vec3 rain_air(vec3 p, float h) {
	vec3 w = weather_map_rect.z > 0.0 ? weather_wind(p, h) : still_wind;
	return w + rain_eddies(p, w) * clamp(h / 0.5, 0.2, 1.0);
}

// A quad facing the eye, stretched along the motion during one exposure (Garg and Nayar 2006's streaks), at least
// 1.5 pixels wide and long with alpha falling by the drawn area's ratio to the drop's own, as SnowFall3D.
mat4 rain_streak(vec3 p, vec3 v, float size, inout float alpha) {
	vec3 view = p - eye;
	float dist = max(length(view), 1e-3);
	view /= dist;
	float px = 1.5 * pixel_angle * dist;
	vec3 st = v * streak_time;
	st -= view * dot(st, view);
	float sl = length(st);
	vec3 along = sl > 1e-5 ? st / sl : normalize(cross(view, abs(view.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 across = normalize(cross(along, view));
	float w = max(size, px);
	float l = max(size + sl, px);
	alpha *= (size * (size + sl)) / (w * l);
	if (alpha < 0.002) {
		alpha = 0.0;
		return mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	}
	return mat4(vec4(across * w, 0.0), vec4(along * l, 0.0), vec4(-view * w, 0.0), vec4(p, 1.0));
}

// A splash's quad: upright, facing the eye about the vertical, its bottom edge on the impact.
mat4 rain_splash_quad(vec3 p, float w, float h) {
	vec3 view = p - eye;
	vec3 vh = vec3(view.x, 0.0, view.z);
	vh = length(vh) > 1e-4 ? normalize(vh) : vec3(0.0, 0.0, 1.0);
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), vh));
	return mat4(vec4(right * w, 0.0), vec4(0.0, h, 0.0, 0.0), vec4(-vh * w, 0.0), vec4(p + vec3(0.0, 0.5 * h, 0.0), 1.0));
}

float rain_draw_size(uint s) {
	// Marshall-Palmer above d_min, truncated at 6 mm (bigger drops break up: Komabayasi et al. 1964).
	float tail = 1.0 - exp(-lambda * max(6.0 - d_min, 0.0));
	return d_min - log(max(1.0 - rain_rand(s) * tail, 1e-6)) / lambda;
}

// A drip's source, by its cumulative weight (a binary search over the sorted row).
vec3 rain_pick_source(float u) {
	int lo = 0;
	int hi = max(source_count - 1, 0);
	for (int i = 0; i < 11; i++) {
		if (lo >= hi) {
			break;
		}
		int mid = (lo + hi) / 2;
		if (texelFetch(source_tex, ivec2(mid, 0), 0).w < u) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return texelFetch(source_tex, ivec2(lo, 0), 0).xyz;
}

void rain_spawn(inout vec4 c, inout vec3 p, inout vec3 v, uint s, bool top) {
	if (drip_mode) {
		// Wait an exponential time (mean drip_idle, read every frame so a change of rate shows at once).
		c.y = -1.0 - (-log(max(rain_rand(s), 1e-6)));
		c.x = drip_d * (0.85 + 0.3 * rain_rand(s + 1u));
		p = vec3(box_center.x, box_center.y - 1e4, box_center.z);
		v = vec3(0.0);
		return;
	}
	vec3 r = vec3(rain_rand(s), rain_rand(s + 1u), rain_rand(s + 2u));
	vec3 rel = (r - 0.5) * box_size;
	if (top) {
		rel.y = 0.5 * box_size.y - 0.5 * rain_rand(s + 3u);
	}
	p = box_center + rel;
	c.x = rain_draw_size(s + 4u);
	c.y = -0.5;
	v = rain_air(p, 10.0) + vec3(0.0, -rain_terminal(c.x), 0.0);
}

void start() {
	uint s = rain_hash(INDEX * 7919u + RANDOM_SEED);
	vec4 c = vec4(0.0, -0.5, rain_rand(s + 9u), rain_rand(rain_hash(INDEX * 2654435761u + 17u)));
	vec3 p;
	vec3 v;
	rain_spawn(c, p, v, s, false);
	CUSTOM = c;
	VELOCITY = v;
	TRANSFORM = mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	COLOR = vec4(1.0, 1.0, 1.0, 0.0);
}

// The step, as a function: a processor function cannot return early.
void rain_step(inout vec4 o_custom, inout mat4 o_xf, inout vec3 o_vel, inout vec4 o_col, bool i_collided, vec3 i_cn, float i_cd, float i_dt, float i_time, uint i_index) {
	vec4 c = o_custom;
	vec3 p = o_xf[3].xyz;
	vec3 v = o_vel;
	uint s = rain_hash(i_index * 9781u + uint(i_time * 61.0) * 13u);
	bool on_water = c.z >= 2.0;
	// Splashing: the crown and its droplets, then back to the sky (or the drips' wait).
	if (c.y >= 0.0) {
		c.y += i_dt;
		vec4 sh = rain_splash_shape(c.x, on_water);
		if (c.y > sh.x) {
			c.z = fract(c.z);
			rain_spawn(c, p, v, s, true);
		} else {
			o_custom = c;
			o_vel = vec3(0.0);
			o_xf = rain_splash_quad(p, sh.y, sh.z);
			// Faded by distance as the streaks: a splash finer than a few pixels is not drawn.
			float px = pixel_angle * distance(p, eye);
			o_col = vec4(1.0, 1.0, 1.0, clamp(sh.z / (4.0 * px), 0.0, 1.0) * step(c.w, ratio));
			return;
		}
	}
	// A drip waiting for its next drop.
	if (drip_mode && c.y < -0.75) {
		c.y += i_dt / max(drip_idle, 1e-3);
		if (c.y >= -1.0 && source_count > 0) {
			vec3 src = rain_pick_source(rain_rand(s + 7u));
			p = src + vec3(rain_rand(s + 8u) - 0.5, -0.02, rain_rand(s + 9u) - 0.5) * vec3(0.5, 1.0, 0.5);
			v = vec3(0.0);
			c.y = -0.5;
		} else {
			o_custom = c;
			o_xf = mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
			o_col = vec4(1.0, 1.0, 1.0, 0.0);
			return;
		}
	}
	// Falling: the drop relaxes toward the air's motion plus its terminal fall over v_t / g (its drag response time),
	// in thinner air faster as (rho0 / rho)^0.4 (Foote and du Toit 1969), the density's scale height 8.5 km.
	vec2 gr = rain_ground(p.xz);
	float d = c.x;
	float vt = rain_terminal(d) * exp(0.4 * clamp(p.y, -500.0, 6000.0) / 8500.0);
	float tau = max(vt / 9.81, 0.02);
	vec3 target = rain_air(p, max(p.y - max(gr.x, box_center.y - 0.5 * box_size.y), 0.2)) + vec3(0.0, -vt, 0.0);
	v = target + (v - target) * exp(-i_dt / tau);
	p += v * i_dt;
	vec3 rel = p - box_center;
	bool landed = i_collided || p.y < gr.x;
	if (landed) {
		if (p.y < gr.x) {
			p.y = gr.x;
			on_water = gr.y > 0.5;
		} else {
			p += i_cn * i_cd;
			on_water = false;
		}
		// Yarin and Weiss (1995): a crown where K = We Oh^-0.4 > 2100; water's sigma 0.072 N/m, mu 1 mPa s.
		float dm = d * 0.001;
		float we = 1000.0 * dot(v, v) * dm / 0.072;
		float oh = 0.001 / sqrt(1000.0 * 0.072 * dm);
		bool crown = we * pow(oh, -0.4) > 2100.0;
		if (splashes && (crown || on_water)) {
			c.y = 0.0;
			c.z = fract(c.z) + (on_water ? 2.0 : 0.0);
			o_custom = c;
			o_vel = vec3(0.0);
			vec4 sh = rain_splash_shape(d, on_water);
			o_xf = rain_splash_quad(p, sh.y, sh.z);
			o_col = vec4(1.0, 1.0, 1.0, 0.0);
			return;
		}
		rain_spawn(c, p, v, s, true);
	} else if (!drip_mode) {
		if (abs(rel.y) > box_size.y || any(greaterThan(abs(rel.xz), box_size.xz))) {
			// The box jumped: scatter through it.
			rain_spawn(c, p, v, s, false);
		} else if (rel.y < -0.5 * box_size.y) {
			rain_spawn(c, p, v, s, true);
		} else {
			rel.xz = mod(rel.xz + 0.5 * box_size.xz, box_size.xz) - 0.5 * box_size.xz;
			p = box_center + rel;
		}
	} else if (p.y < box_center.y - box_size.y) {
		rain_spawn(c, p, v, s, true);
	}
	o_custom = c;
	o_vel = v;
	rel = p - box_center;
	float fade = 1.0;
	if (!drip_mode) {
		vec2 edge = 0.5 * box_size.xz - abs(rel.xz);
		fade = smoothstep(0.0, 2.0, min(edge.x, edge.y)) * smoothstep(0.0, 1.0, 0.5 * box_size.y - abs(rel.y));
	}
	// Drops inside half a metre of the eye are out of focus: faded, as a lens blurs them away.
	float near = smoothstep(0.3, 0.9, distance(p, eye));
	float a = step(c.w, ratio) * fade * near * (drip_mode ? 1.0 : weight);
	o_xf = rain_streak(p, v, d * 0.001 * size_scale, a);
	o_col = vec4(1.0, 1.0, 1.0, min(a, 1.0));
}

void process() {
	vec4 c = CUSTOM;
	mat4 xf = TRANSFORM;
	vec3 v = VELOCITY;
	vec4 col = COLOR;
	rain_step(c, xf, v, col, COLLIDED, COLLISION_NORMAL, COLLISION_DEPTH, DELTA, TIME, INDEX);
	CUSTOM = c;
	TRANSFORM = xf;
	VELOCITY = v;
	COLOR = col;
}
)";

// The drop's material: a streak (a cylinder across it, the motion-blurred sphere's average) or a splash (a crown and
// its droplets, or a Worthington jet on water), drawn from the particle's state in INSTANCE_CUSTOM.
static const char *RAIN_DRAW = R"(
shader_type spatial;
render_mode cull_disabled, depth_draw_never, shadows_disabled;

varying vec4 v_state;

void vertex() {
	v_state = INSTANCE_CUSTOM;
}

// The splash at local (x across, y up, m): coverage and the surface's slope (in the quad's plane) for its normal.
float rain_splash_cover(vec2 q, vec4 st, out vec2 r_n) {
	bool water = st.z >= 2.0;
	float d = st.x * 0.001;
	vec4 sh = rain_splash_shape(st.x, water);
	float t = st.y;
	float cover = 0.0;
	r_n = vec2(0.0);
	uint seed = uint(fract(st.z) * 16777216.0);
	if (water) {
		// The jet: a column rising at vj and falling back, its tip a drop pinched off (Rayleigh-Plateau).
		float vj = sqrt(max(2.0 * 9.81 * (sh.z - 0.004), 0.0));
		float top = max(vj * t - 0.5 * 9.81 * t * t, 0.0);
		float rad = 0.6 * d;
		if (q.y < top && abs(q.x) < rad) {
			cover = 1.0;
			r_n = vec2(q.x / rad, 0.0);
		}
		vec2 tip = q - vec2(0.0, top + rad * 1.5);
		if (length(tip) < rad * 1.2) {
			cover = 1.0;
			r_n = tip / (rad * 1.2);
		}
		return cover;
	}
	// The crown: a sheet rising from a rim spreading as sqrt(t) (Yarin and Weiss 1995), a few millimetres tall, for its
	// first 30 ms.
	float tc = t / 0.03;
	if (tc < 1.0) {
		float rim = 2.0 * d * sqrt(max(tc, 0.0)) + 0.5 * d;
		float height = 2.5 * d * sin(3.14159 * min(tc, 1.0));
		float wall = abs(abs(q.x) - rim * 0.95);
		if (q.y < height && abs(q.x) < rim) {
			float side = smoothstep(0.35 * rim, 0.0, wall);
			cover = max(cover, mix(0.25, 1.0, side) * (1.0 - q.y / max(height, 1e-4) * 0.5));
			r_n = vec2(sign(q.x) * clamp(abs(q.x) / rim, 0.0, 1.0), 0.0);
		}
	}
	// The droplets: eight thrown from the rim, on ballistic arcs, seen projected on the quad.
	for (int k = 0; k < 8; k++) {
		uint h = rain_hash(seed * 31u + uint(k) * 977u);
		float az = 6.2831853 * rain_rand(h);
		float el = mix(0.5, 1.1, rain_rand(h + 1u));
		float u = sh.w * mix(0.5, 1.0, rain_rand(h + 2u));
		float x = cos(az) * cos(el) * u * t;
		float y = sin(el) * u * t - 0.5 * 9.81 * t * t;
		if (y < 0.0) {
			continue;
		}
		float r = d * mix(0.12, 0.3, rain_rand(h + 3u)) + 0.0004;
		vec2 o = q - vec2(x, y);
		float l = length(o);
		if (l < r) {
			cover = 1.0;
			r_n = o / r;
		}
	}
	return cover;
}

void fragment() {
	vec4 st = v_state;
	float alpha = COLOR.a;
	vec2 nq;
	float shape;
	if (st.y >= 0.0) {
		vec4 sh = rain_splash_shape(st.x, st.z >= 2.0);
		vec2 q = vec2((UV.x - 0.5) * sh.y, (1.0 - UV.y) * sh.z);
		shape = rain_splash_cover(q, st, nq);
	} else {
		// The streak: round across, tapered at its ends.
		vec2 a = UV * 2.0 - 1.0;
		shape = (1.0 - smoothstep(0.75, 1.0, abs(a.x))) * (1.0 - smoothstep(0.85, 1.0, abs(a.y)));
		nq = vec2(a.x, 0.0);
	}
	float s2 = min(dot(nq, nq), 0.999);
	NORMAL = normalize(TANGENT * nq.x + BINORMAL * nq.y + NORMAL * sqrt(1.0 - s2));
	ALPHA = alpha * shape;
	METALLIC = 0.0;
#if defined(RT)
	// Water: clear, smooth, bending the light by its index.
	ALBEDO = vec3(1.0);
	ROUGHNESS = 0.0;
	SPECULAR = 0.5;
	TRANSMISSION = 1.0;
	IOR = 1.33;
#else
	// A drop gathers its light from nearly half the sky (Garg and Nayar 2004): its average is close to a white
	// surface lit by the same sky, plus the specular of its face.
	ALBEDO = vec3(0.9);
	ROUGHNESS = 0.15;
	SPECULAR = 0.5;
	ALPHA *= 0.6;
#endif
	if (ALPHA < 0.002) {
		discard;
	}
}
)";

// ---------------------------------------------------------------------------------------------------------
// Relations

float RainFall3D::drop_size_slope(float p_rate_mm_h) {
	return 4.1f * Math::pow(MAX(p_rate_mm_h, 0.01f), -0.21f);
}

float RainFall3D::drop_number_density(float p_rate_mm_h, float p_min_size_mm) {
	if (p_rate_mm_h <= 0.0f) {
		return 0.0f;
	}
	const float l = drop_size_slope(p_rate_mm_h);
	return 8000.0f / l * Math::exp(-l * p_min_size_mm);
}

float RainFall3D::terminal_velocity(float p_size_mm) {
	return MAX(9.65f - 10.3f * Math::exp(-0.6f * p_size_mm), 0.05f);
}

float RainFall3D::impact_flux(float p_rate_mm_h, float p_min_size_mm, float p_max_size_mm) {
	if (p_rate_mm_h <= 0.0f || p_max_size_mm <= p_min_size_mm) {
		return 0.0f;
	}
	const float l = drop_size_slope(p_rate_mm_h);
	const float l2 = l + 0.6f;
	const float f = 9.65f * (Math::exp(-l * p_min_size_mm) - Math::exp(-l * p_max_size_mm)) / l - 10.3f * (Math::exp(-l2 * p_min_size_mm) - Math::exp(-l2 * p_max_size_mm)) / l2;
	return MAX(8000.0f * f, 0.0f);
}

float RainFall3D::rain_extinction(float p_rate_mm_h) {
	// Geometric optics: sigma = integral of N(D) 2 pi D^2 / 4 dD = 8000 * 2 * pi / 4 * 2 / L^3 (mm^2 per m3 per mm
	// becomes 1e-6 m^2): 0.25 per km at 1 mm/h, 1.5 per km at 10, 4 per km at 50, the observed range (Atlas 1953).
	if (p_rate_mm_h <= 0.0f) {
		return 0.0f;
	}
	const float l = drop_size_slope(p_rate_mm_h);
	return 8000.0f * float(Math::PI) * 0.5f * 2.0f / (l * l * l) * 1e-6f;
}

float RainFall3D::splash_parameter(float p_size_mm, float p_speed) {
	const float d = MAX(p_size_mm, 0.01f) * 0.001f;
	const float we = 1000.0f * p_speed * p_speed * d / 0.072f;
	const float oh = 0.001f / Math::sqrt(1000.0f * 0.072f * d);
	return we * Math::pow(oh, -0.4f);
}

float RainFall3D::drips_per_second(float p_rate_mm_h, float p_area_m2, float p_drop_mm) {
	const float d = MAX(p_drop_mm, 0.1f) * 0.001f;
	const float volume = float(Math::PI) / 6.0f * d * d * d; // m3
	return MAX(p_rate_mm_h, 0.0f) / 3.6e6f * MAX(p_area_m2, 0.0f) / volume;
}

// ---------------------------------------------------------------------------------------------------------
// Setup

WeatherMap3D *RainFall3D::_weather() const {
	return WeatherMap3D::get_active();
}

Node *RainFall3D::_snow_cover() const {
	// An explicit SnowCover3D (snow_cover), reached by its methods; otherwise the snow contract (_update_ground).
	if (!snow_cover.is_empty() && is_inside_tree()) {
		return get_node_or_null(snow_cover);
	}
	return nullptr;
}

void RainFall3D::_scan_scene() {
	// The water bodies, found by class name (their module is optional). The snowpack comes through WorldContracts.
	found_water.clear();
	if (!is_inside_tree()) {
		return;
	}
	Node *root = get_tree()->get_root();
	for (const char *type : { "PhysXWaterSurface3D", "PhysXRiver3D", "PhysXLiveWater3D" }) {
		TypedArray<Node> water = root->find_children("*", type, true, false);
		for (int i = 0; i < water.size(); i++) {
			found_water.push_back(Object::cast_to<Node>(water[i])->get_instance_id());
		}
	}
}

Vector3 RainFall3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n && n->is_inside_tree()) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void RainFall3D::_ensure_children() {
	auto make = [&](GPUParticles3D *&r_p, const Ref<ShaderMaterial> &p_process) {
		if (r_p) {
			return;
		}
		r_p = memnew(GPUParticles3D);
		r_p->set_as_top_level(true);
		r_p->set_use_local_coordinates(false);
		r_p->set_explosiveness_ratio(1.0f);
		r_p->set_lifetime(3600.0);
		r_p->set_collision_base_size(0.005f);
		r_p->set_process_material(p_process);
		r_p->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		Ref<QuadMesh> q;
		q.instantiate();
		q->set_size(Size2(1, 1));
		q->set_material(drop_material);
		r_p->set_draw_pass_mesh(0, q);
		add_child(r_p, false, INTERNAL_MODE_FRONT);
	};
	make(fall, fall_process);
	make(drip, drip_process);
	if (shelter_enabled && !shelter) {
		shelter = memnew(GPUParticlesCollisionHeightField3D);
		shelter->set_as_top_level(true);
		shelter->set_follow_camera_enabled(true);
		shelter->set_resolution(GPUParticlesCollisionHeightField3D::RESOLUTION_1024);
		shelter->set_heightfield_mask(shelter_mask);
		add_child(shelter, false, INTERNAL_MODE_FRONT);
	} else if (!shelter_enabled && shelter) {
		shelter->queue_free();
		shelter = nullptr;
	}
	fall->set_amount(MAX(max_drops, 1));
	fall->set_emitting(falling_enabled);
	fall->set_visible(falling_enabled);
	fall->set_visibility_aabb(AABB(-fall_extent * 0.5f - Vector3(2, 2, 2), fall_extent + Vector3(4, 4, 4)));
	drip->set_amount(MAX(max_drips, 1));
	drip->set_emitting(drips_enabled);
	drip->set_visible(drips_enabled);
	drip->set_visibility_aabb(AABB(Vector3(-drip_extent * 0.5f, -60.0f, -drip_extent * 0.5f), Vector3(drip_extent, 120.0f, drip_extent)));
	if (shelter) {
		const float s = MAX(MAX(fall_extent.x, fall_extent.z), drip_extent);
		shelter->set_size(Vector3(s, MAX(fall_extent.y * 2.0f, 40.0f), s));
		shelter->set_heightfield_mask(shelter_mask);
	}
}

void RainFall3D::_update_ground(Vector3 p_center) {
	// The stop height on a coarse grid: the snow's surface, and still or running water (the GPU collider does not
	// see transparent water). Red the height, green 1 on water.
	const float size = MAX(MAX(fall_extent.x, fall_extent.z), drip_extent) + 8.0f;
	const float step = size / (GROUND_N - 1);
	const Vector2 o(Math::snapped(p_center.x, 4.0f) - size * 0.5f, Math::snapped(p_center.z, 4.0f) - size * 0.5f);
	Vector<uint8_t> bytes;
	bytes.resize(GROUND_N * GROUND_N * 8);
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	Node *snow = _snow_cover();
	const bool snow_ok = snow && snow->has_method(SNAME("get_ground_height_at")) && snow->has_method(SNAME("get_depth_at"));
	WorldSnow *snow_contract = snow_ok ? nullptr : WorldContracts::snow();
	if (snow_contract && !snow_contract->has_provider()) {
		snow_contract = nullptr;
	}
	// Water bodies by their methods (they live in the PhysX module): a sea or lake's level over its domain, a river's
	// or live water's surface where it has water.
	LocalVector<Node3D *> seas;
	LocalVector<Node3D *> flows;
	for (const ObjectID &id : found_water) {
		Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(id));
		if (!n || !n->is_inside_tree() || !n->is_visible_in_tree()) {
			continue;
		}
		if (n->is_class("PhysXWaterSurface3D")) {
			seas.push_back(n);
		} else {
			flows.push_back(n);
		}
	}
	for (int z = 0; z < GROUND_N; z++) {
		for (int x = 0; x < GROUND_N; x++) {
			const Vector3 p(o.x + x * step, p_center.y, o.y + z * step);
			float h = -1e9f;
			float water = 0.0f;
			if (snow_ok) {
				const float depth = snow->call(SNAME("get_depth_at"), p);
				if (depth > 0.005f) {
					h = float(snow->call(SNAME("get_ground_height_at"), p)) + depth;
				}
			} else if (snow_contract && snow_contract->depth_at(p) > 0.005f) {
				// The snow's own top, ruts and footprints included (-INF where there is none).
				const float top = snow_contract->surface_at(p);
				if (Math::is_finite(top)) {
					h = top;
				}
			}
			for (Node3D *n : seas) {
				const Vector2 dom = n->call(SNAME("get_domain_size"));
				const bool open = n->get(SNAME("open_sea_beyond"));
				const Vector3 c = n->get_global_position();
				if (!open && (Math::abs(p.x - c.x) > 0.5f * dom.x || Math::abs(p.z - c.z) > 0.5f * dom.y)) {
					continue;
				}
				const float level = c.y + float(n->call(SNAME("get_water_level")));
				if (level > h) {
					h = level;
					water = 1.0f;
				}
			}
			for (Node3D *n : flows) {
				const float level = n->call(SNAME("sample_height"), p);
				if (Math::is_finite(level) && level > h) {
					h = level;
					water = 1.0f;
				}
			}
			w[(z * GROUND_N + x) * 2 + 0] = h;
			w[(z * GROUND_N + x) * 2 + 1] = water;
		}
	}
	if (ground_image.is_null()) {
		ground_image = Image::create_from_data(GROUND_N, GROUND_N, false, Image::FORMAT_RGF, bytes);
		ground_texture = ImageTexture::create_from_image(ground_image);
	} else {
		ground_image->set_data(GROUND_N, GROUND_N, false, Image::FORMAT_RGF, bytes);
		ground_texture->update(ground_image);
	}
	const Vector4 rect(o.x - step * 0.5f, o.y - step * 0.5f, step * GROUND_N, step * GROUND_N);
	for (Ref<ShaderMaterial> m : { fall_process, drip_process }) {
		m->set_shader_parameter("ground_tex", ground_texture);
		m->set_shader_parameter("ground_rect", rect);
	}
	ground_center = p_center;
	ground_valid = true;
}

void RainFall3D::_update_sources(Vector3 p_center, double p_delta) {
	// The roof's water follows the rain: filling in about half a minute, draining over roof_drain_time.
	WeatherMap3D *weather = _weather();
	const float rain = cur_rate;
	const float tau = rain > roof_rate ? 30.0f : roof_drain_time;
	roof_rate += (rain - roof_rate) * float(1.0 - Math::exp(-p_delta / tau));
	source_accum += p_delta;
	if (source_accum < 1.0 && Vector2(p_center.x - source_center.x, p_center.z - source_center.z).length() < 4.0f) {
		return;
	}
	source_accum = 0.0;
	source_center = p_center;
	struct Source {
		Vector3 p;
		float rate; // drops per second
	};
	LocalVector<Source> sources;
	// Eaves: the surface hit from above on a grid; an edge where it falls by eave_drop or more to a neighbour sheds
	// the rain of a roof_depth strip behind it (outside the canopy, which drips by its own store).
	const int n = 48;
	const float cell = drip_extent / n;
	const Vector2 o(Math::snapped(p_center.x, cell) - drip_extent * 0.5f, Math::snapped(p_center.z, cell) - drip_extent * 0.5f);
	PhysicsDirectSpaceState3D *space = is_inside_tree() && get_world_3d().is_valid() ? get_world_3d()->get_direct_space_state() : nullptr;
	if (space && roof_rate > 0.01f) {
		LocalVector<float> top;
		top.resize(n * n);
		const float y0 = p_center.y + 60.0f;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = Vector3(o.x + (x + 0.5f) * cell, y0, o.y + (z + 0.5f) * cell);
				rp.to = rp.from - Vector3(0, 200.0f, 0);
				rp.collision_mask = drip_collision_mask;
				PhysicsDirectSpaceState3D::RayResult rr;
				top[z * n + x] = space->intersect_ray(rp, rr) ? rr.position.y : -1e9f;
			}
		}
		const int dx[4] = { 1, -1, 0, 0 };
		const int dz[4] = { 0, 0, 1, -1 };
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const float h = top[z * n + x];
				if (h < -1e8f) {
					continue;
				}
				const Vector3 at(o.x + (x + 0.5f) * cell, h, o.y + (z + 0.5f) * cell);
				if (weather && weather->get_canopy_closure_at(at) > 0.3f) {
					continue;
				}
				for (int k = 0; k < 4; k++) {
					const int nx = x + dx[k];
					const int nz = z + dz[k];
					if (nx < 0 || nz < 0 || nx >= n || nz >= n || h - top[nz * n + nx] < eave_drop) {
						continue;
					}
					Source s;
					s.p = at + Vector3(dx[k], 0, dz[k]) * (0.5f * cell);
					s.rate = drips_per_second(roof_rate, roof_depth * cell, drip_size);
					sources.push_back(s);
				}
			}
		}
	}
	// The canopy's drainage under the trees: per 2 m cell, at two thirds of the canopy's height.
	if (weather && weather->is_canopy_water_enabled()) {
		const int m = MAX(int(drip_extent / 2.0f), 2);
		const float c2 = drip_extent / m;
		for (int z = 0; z < m; z++) {
			for (int x = 0; x < m; x++) {
				const Vector3 at(o.x + (x + 0.5f) * c2, p_center.y, o.y + (z + 0.5f) * c2);
				const float closure = weather->get_canopy_closure_at(at);
				const float dr = weather->get_canopy_drip_at(at);
				if (closure < 0.05f || dr < 1e-3f) {
					continue;
				}
				float ground = ground_valid ? p_center.y - 2.0f : p_center.y - 2.0f;
				if (space) {
					PhysicsDirectSpaceState3D::RayParameters rp;
					rp.from = at + Vector3(0, 60.0f, 0);
					rp.to = at - Vector3(0, 140.0f, 0);
					rp.collision_mask = drip_collision_mask;
					PhysicsDirectSpaceState3D::RayResult rr;
					if (space->intersect_ray(rp, rr)) {
						ground = rr.position.y;
					}
				}
				const float ch = MAX(weather->get_canopy_height_at(at), 1.0f);
				Source s;
				s.p = Vector3(at.x, ground + 0.6f * ch, at.z);
				s.rate = drips_per_second(dr * closure, c2 * c2, drip_size);
				sources.push_back(s);
			}
		}
	}
	struct ByRate {
		bool operator()(const Source &a, const Source &b) const { return a.rate > b.rate; }
	};
	sources.sort_custom<ByRate>();
	source_count = MIN(int(sources.size()), MAX_SOURCES);
	drip_rate = 0.0f;
	for (int i = 0; i < source_count; i++) {
		drip_rate += sources[i].rate;
	}
	Vector<uint8_t> bytes;
	bytes.resize(MAX_SOURCES * 16);
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	float acc = 0.0f;
	for (int i = 0; i < MAX_SOURCES; i++) {
		if (i < source_count) {
			acc += sources[i].rate;
			w[i * 4 + 0] = sources[i].p.x;
			w[i * 4 + 1] = sources[i].p.y;
			w[i * 4 + 2] = sources[i].p.z;
			w[i * 4 + 3] = drip_rate > 0.0f ? acc / drip_rate : 1.0f;
		} else {
			w[i * 4 + 0] = w[i * 4 + 1] = w[i * 4 + 2] = 0.0f;
			w[i * 4 + 3] = 2.0f;
		}
	}
	if (source_image.is_null()) {
		source_image = Image::create_from_data(MAX_SOURCES, 1, false, Image::FORMAT_RGBAF, bytes);
		source_texture = ImageTexture::create_from_image(source_image);
	} else {
		source_image->set_data(MAX_SOURCES, 1, false, Image::FORMAT_RGBAF, bytes);
		source_texture->update(source_image);
	}
	drip_process->set_shader_parameter("source_tex", source_texture);
	drip_process->set_shader_parameter("source_count", source_count);
}

void RainFall3D::_update(double p_delta) {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_children();
	const Vector3 center = _follow_position();
	Vector3 eye = center;
	float pixel_angle = 0.001f;
	if (get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			eye = cam->get_global_position();
			const float h = MAX(get_viewport()->get_visible_rect().size.y, 1.0f);
			pixel_angle = cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE ? 2.0f * Math::tan(Math::deg_to_rad(cam->get_fov()) * 0.5f) / h : cam->get_size() / h / MAX(eye.distance_to(center), 1.0f);
		}
	}
	scan_accum += p_delta;
	if (scan_accum > 10.0 || !scanned) {
		scan_accum = 0.0;
		scanned = true;
		_scan_scene();
	}
	ground_accum += p_delta;
	if (!ground_valid || ground_accum > 2.0 || Vector2(center.x - ground_center.x, center.z - ground_center.z).length() > 4.0f) {
		ground_accum = 0.0;
		_update_ground(center);
	}
	WeatherMap3D *weather = _weather();
	// The rain at the camera: the liquid share of the weather's precipitation at this height.
	if (rainfall_rate_override >= 0.0f) {
		cur_rate = rainfall_rate_override;
		cur_share = 1.0f;
	} else if (weather) {
		cur_share = weather->get_rain_share_at(center);
		cur_rate = weather->get_rain_at(center) * cur_share;
	} else {
		cur_rate = 0.0f;
		cur_share = 1.0f;
	}
	// With no weather map the rain readers (rings on water and puddles, raster and path tracer) read this rain instead.
	_publish_fallback(cur_rate);
	const float volume = fall_extent.x * fall_extent.y * fall_extent.z;
	const float wanted = drop_number_density(cur_rate, min_drop_size) * density_scale * volume;
	cur_ratio = falling_enabled ? CLAMP(wanted / MAX(max_drops, 1), 0.0f, 1.0f) : 0.0f;
	// Past the budget each streak stands for several drops (energy kept, up to eight).
	cur_weight = CLAMP(wanted / MAX(float(max_drops), 1.0f), 1.0f, 8.0f);
	const float turb = turbulence * 2.5f * 0.41f * (weather ? weather->get_wind_at(center, 10.0f).length() : 0.0f) / Math::log(10.0f / 0.03f);
	turb_clock += p_delta;
	for (Ref<ShaderMaterial> m : { fall_process, drip_process }) {
		m->set_shader_parameter("box_center", center);
		m->set_shader_parameter("box_size", m == drip_process ? Vector3(drip_extent, 60.0f, drip_extent) : fall_extent);
		m->set_shader_parameter("eye", eye);
		m->set_shader_parameter("pixel_angle", pixel_angle);
		m->set_shader_parameter("streak_time", streak_time);
		m->set_shader_parameter("size_scale", drop_size_scale);
		m->set_shader_parameter("splashes", splashes_enabled);
		m->set_shader_parameter("turb_sigma", turb);
		m->set_shader_parameter("turb_time", float(Math::fmod(turb_clock, 3600.0)));
	}
	fall->set_global_transform(Transform3D(Basis(), center));
	drip->set_global_transform(Transform3D(Basis(), center));
	if (shelter) {
		shelter->set_global_position(center);
	}
	fall_process->set_shader_parameter("ratio", cur_ratio);
	fall_process->set_shader_parameter("weight", cur_weight);
	fall_process->set_shader_parameter("lambda", drop_size_slope(MAX(cur_rate, 0.01f)));
	fall_process->set_shader_parameter("d_min", min_drop_size);
	// Drips: every particle runs fall, splash, wait; the wait sets the rate (a particle's cycle is max_drips / rate).
	if (drips_enabled) {
		_update_sources(center, p_delta);
	}
	const float fall_time = 1.5f;
	drip_process->set_shader_parameter("ratio", 1.0f);
	drip_process->set_shader_parameter("drip_d", drip_size);
	drip_process->set_shader_parameter("drip_idle", drip_rate > 1e-4f ? MAX(float(max_drips) / drip_rate - fall_time, 0.05f) : 1e6f);
	_push_haze();
}

void RainFall3D::_push_haze() {
	// The rain beyond the particles, as the weather map's precipitation haze (extinction of the drops, geometric
	// optics; water drops barely absorb).
	WeatherMap3D *w = _weather();
	if (!w || !haze_enabled || !is_inside_tree()) {
		return;
	}
	const float sigma = falling_enabled ? rain_extinction(cur_rate) * density_scale : 0.0f;
	if (Math::abs(sigma - pushed_sigma) > 0.02f * MAX(Math::abs(pushed_sigma), 1e-4f)) {
		if (sigma > 1e-6f) {
			// Falling at about 7 m/s (the shafts' slant): drawn in curtains (WeatherMap3D rain_curtains).
			w->set_precipitation_extinction(this, sigma, 0.0f, 0.99f, Vector3(), 0.0f, 7.0f);
		} else {
			w->clear_precipitation_extinction(this);
		}
		pushed_sigma = sigma;
	}
}

void RainFall3D::_clear_haze() {
	if (WeatherMap3D *w = _weather()) {
		w->clear_precipitation_extinction(this);
	}
	pushed_sigma = -1.0f;
}

// weather_rain_fallback (material.cpp: weather_rainfall_at()): the rain this node falls, for the shaders that read
// rain when no WeatherMap3D publishes its map. The map wins whenever it exists, so there is still one source.
void RainFall3D::_publish_fallback(float p_rate) {
	if (Engine::get_singleton()->is_editor_hint() || Math::abs(p_rate - published_fallback) < 1e-3f) {
		return;
	}
	published_fallback = p_rate;
	RenderingServer::get_singleton()->global_shader_parameter_set(SNAME("weather_rain_fallback"), Vector4(p_rate, 0.0f, 0.0f, 0.0f));
}

void RainFall3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_ensure_children();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			_update(0.0);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear_haze();
			_publish_fallback(0.0f);
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update(get_process_delta_time());
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Settings

void RainFall3D::set_falling_enabled(bool p_enabled) {
	falling_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void RainFall3D::set_fall_extent(const Vector3 &p_extent) {
	fall_extent = p_extent.maxf(1.0f);
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void RainFall3D::set_max_drops(int p_count) {
	max_drops = CLAMP(p_count, 1, 4000000);
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void RainFall3D::set_shelter_enabled(bool p_enabled) {
	shelter_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void RainFall3D::set_shelter_mask(uint32_t p_mask) {
	shelter_mask = p_mask;
	if (shelter) {
		shelter->set_heightfield_mask(p_mask);
	}
}
void RainFall3D::set_haze_enabled(bool p_enabled) {
	haze_enabled = p_enabled;
	if (!p_enabled) {
		_clear_haze();
	}
}
void RainFall3D::set_drips_enabled(bool p_enabled) {
	drips_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void RainFall3D::set_max_drips(int p_count) {
	max_drips = CLAMP(p_count, 1, 1000000);
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void RainFall3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &RainFall3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &RainFall3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &RainFall3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &RainFall3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_falling_enabled", "enabled"), &RainFall3D::set_falling_enabled);
	ClassDB::bind_method(D_METHOD("is_falling_enabled"), &RainFall3D::is_falling_enabled);
	ClassDB::bind_method(D_METHOD("set_fall_extent", "extent"), &RainFall3D::set_fall_extent);
	ClassDB::bind_method(D_METHOD("get_fall_extent"), &RainFall3D::get_fall_extent);
	ClassDB::bind_method(D_METHOD("set_max_drops", "count"), &RainFall3D::set_max_drops);
	ClassDB::bind_method(D_METHOD("get_max_drops"), &RainFall3D::get_max_drops);
	ClassDB::bind_method(D_METHOD("set_min_drop_size", "mm"), &RainFall3D::set_min_drop_size);
	ClassDB::bind_method(D_METHOD("get_min_drop_size"), &RainFall3D::get_min_drop_size);
	ClassDB::bind_method(D_METHOD("set_drop_size_scale", "scale"), &RainFall3D::set_drop_size_scale);
	ClassDB::bind_method(D_METHOD("get_drop_size_scale"), &RainFall3D::get_drop_size_scale);
	ClassDB::bind_method(D_METHOD("set_density_scale", "scale"), &RainFall3D::set_density_scale);
	ClassDB::bind_method(D_METHOD("get_density_scale"), &RainFall3D::get_density_scale);
	ClassDB::bind_method(D_METHOD("set_streak_time", "seconds"), &RainFall3D::set_streak_time);
	ClassDB::bind_method(D_METHOD("get_streak_time"), &RainFall3D::get_streak_time);
	ClassDB::bind_method(D_METHOD("set_rainfall_rate_override", "rate"), &RainFall3D::set_rainfall_rate_override);
	ClassDB::bind_method(D_METHOD("get_rainfall_rate_override"), &RainFall3D::get_rainfall_rate_override);
	ClassDB::bind_method(D_METHOD("set_shelter_enabled", "enabled"), &RainFall3D::set_shelter_enabled);
	ClassDB::bind_method(D_METHOD("is_shelter_enabled"), &RainFall3D::is_shelter_enabled);
	ClassDB::bind_method(D_METHOD("set_shelter_mask", "mask"), &RainFall3D::set_shelter_mask);
	ClassDB::bind_method(D_METHOD("get_shelter_mask"), &RainFall3D::get_shelter_mask);
	ClassDB::bind_method(D_METHOD("set_splashes_enabled", "enabled"), &RainFall3D::set_splashes_enabled);
	ClassDB::bind_method(D_METHOD("is_splashes_enabled"), &RainFall3D::is_splashes_enabled);
	ClassDB::bind_method(D_METHOD("set_haze_enabled", "enabled"), &RainFall3D::set_haze_enabled);
	ClassDB::bind_method(D_METHOD("is_haze_enabled"), &RainFall3D::is_haze_enabled);
	ClassDB::bind_method(D_METHOD("set_turbulence", "scale"), &RainFall3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &RainFall3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_drips_enabled", "enabled"), &RainFall3D::set_drips_enabled);
	ClassDB::bind_method(D_METHOD("is_drips_enabled"), &RainFall3D::is_drips_enabled);
	ClassDB::bind_method(D_METHOD("set_max_drips", "count"), &RainFall3D::set_max_drips);
	ClassDB::bind_method(D_METHOD("get_max_drips"), &RainFall3D::get_max_drips);
	ClassDB::bind_method(D_METHOD("set_drip_extent", "extent"), &RainFall3D::set_drip_extent);
	ClassDB::bind_method(D_METHOD("get_drip_extent"), &RainFall3D::get_drip_extent);
	ClassDB::bind_method(D_METHOD("set_drip_size", "mm"), &RainFall3D::set_drip_size);
	ClassDB::bind_method(D_METHOD("get_drip_size"), &RainFall3D::get_drip_size);
	ClassDB::bind_method(D_METHOD("set_eave_drop", "height"), &RainFall3D::set_eave_drop);
	ClassDB::bind_method(D_METHOD("get_eave_drop"), &RainFall3D::get_eave_drop);
	ClassDB::bind_method(D_METHOD("set_roof_depth", "depth"), &RainFall3D::set_roof_depth);
	ClassDB::bind_method(D_METHOD("get_roof_depth"), &RainFall3D::get_roof_depth);
	ClassDB::bind_method(D_METHOD("set_roof_drain_time", "seconds"), &RainFall3D::set_roof_drain_time);
	ClassDB::bind_method(D_METHOD("get_roof_drain_time"), &RainFall3D::get_roof_drain_time);
	ClassDB::bind_method(D_METHOD("set_drip_collision_mask", "mask"), &RainFall3D::set_drip_collision_mask);
	ClassDB::bind_method(D_METHOD("get_drip_collision_mask"), &RainFall3D::get_drip_collision_mask);
	ClassDB::bind_method(D_METHOD("get_rainfall_rate"), &RainFall3D::get_rainfall_rate);
	ClassDB::bind_method(D_METHOD("get_rain_share"), &RainFall3D::get_rain_share);
	ClassDB::bind_method(D_METHOD("get_visible_drops"), &RainFall3D::get_visible_drops);
	ClassDB::bind_method(D_METHOD("get_drop_weight"), &RainFall3D::get_drop_weight);
	ClassDB::bind_method(D_METHOD("get_drip_source_count"), &RainFall3D::get_drip_source_count);
	ClassDB::bind_method(D_METHOD("get_drip_rate"), &RainFall3D::get_drip_rate);
	ClassDB::bind_method(D_METHOD("get_extinction"), &RainFall3D::get_extinction);
	ClassDB::bind_method(D_METHOD("get_visibility"), &RainFall3D::get_visibility);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("drop_size_slope", "rate"), &RainFall3D::drop_size_slope);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("drop_number_density", "rate", "min_size"), &RainFall3D::drop_number_density);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("terminal_velocity", "size"), &RainFall3D::terminal_velocity);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("impact_flux", "rate", "min_size", "max_size"), &RainFall3D::impact_flux);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("rain_extinction", "rate"), &RainFall3D::rain_extinction);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("splash_parameter", "size", "speed"), &RainFall3D::splash_parameter);
	ClassDB::bind_static_method("RainFall3D", D_METHOD("drips_per_second", "rate", "area", "drop_size"), &RainFall3D::drips_per_second);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover"), "set_snow_cover", "get_snow_cover");
	ADD_GROUP("Falling", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "falling_enabled"), "set_falling_enabled", "is_falling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "fall_extent", PROPERTY_HINT_NONE, "suffix:m"), "set_fall_extent", "get_fall_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_drops", PROPERTY_HINT_RANGE, "1,4000000,1"), "set_max_drops", "get_max_drops");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_drop_size", PROPERTY_HINT_RANGE, "0.1,5,0.01,suffix:mm"), "set_min_drop_size", "get_min_drop_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drop_size_scale", PROPERTY_HINT_RANGE, "0.01,10,0.01"), "set_drop_size_scale", "get_drop_size_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_density_scale", "get_density_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streak_time", PROPERTY_HINT_RANGE, "0,0.1,0.0001,suffix:s"), "set_streak_time", "get_streak_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rainfall_rate_override", PROPERTY_HINT_RANGE, "-1,200,0.01,suffix:mm/h"), "set_rainfall_rate_override", "get_rainfall_rate_override");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shelter_enabled"), "set_shelter_enabled", "is_shelter_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shelter_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_shelter_mask", "get_shelter_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "splashes_enabled"), "set_splashes_enabled", "is_splashes_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "haze_enabled"), "set_haze_enabled", "is_haze_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_turbulence", "get_turbulence");
	ADD_GROUP("Drips", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drips_enabled"), "set_drips_enabled", "is_drips_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_drips", PROPERTY_HINT_RANGE, "1,1000000,1"), "set_max_drips", "get_max_drips");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drip_extent", PROPERTY_HINT_RANGE, "4,256,1,suffix:m"), "set_drip_extent", "get_drip_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drip_size", PROPERTY_HINT_RANGE, "1,8,0.1,suffix:mm"), "set_drip_size", "get_drip_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eave_drop", PROPERTY_HINT_RANGE, "0.1,20,0.1,suffix:m"), "set_eave_drop", "get_eave_drop");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roof_depth", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:m"), "set_roof_depth", "get_roof_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roof_drain_time", PROPERTY_HINT_RANGE, "1,3600,1,suffix:s"), "set_roof_drain_time", "get_roof_drain_time");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "drip_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_drip_collision_mask", "get_drip_collision_mask");
}

RainFall3D::RainFall3D() {
	const String wind = WeatherMap3D::get_wind_shader_code();
	process_shader.instantiate();
	process_shader->set_code(String("shader_type particles;\nrender_mode disable_velocity, disable_force;\n") + wind + RAIN_SHARED + RAIN_PROCESS);
	fall_process.instantiate();
	fall_process->set_shader(process_shader);
	drip_process.instantiate();
	drip_process->set_shader(process_shader);
	drip_process->set_shader_parameter("drip_mode", true);
	draw_shader.instantiate();
	draw_shader->set_code(String(RAIN_DRAW).replace("varying vec4 v_state;", String(RAIN_SHARED) + "\nvarying vec4 v_state;"));
	drop_material.instantiate();
	drop_material->set_shader(draw_shader);
}

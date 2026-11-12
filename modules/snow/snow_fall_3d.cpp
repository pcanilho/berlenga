/**************************************************************************/
/*  snow_fall_3d.cpp                                                      */
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

#include "snow_fall_3d.h"

#include "snow_cover_3d.h"

#include "modules/weather/weather_map_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/gpu_particles_collision_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/gradient_texture.h"

// Shared by both particle shaders: a hash, the ground under the box and the snowpack.
static const char *SNOW_PARTICLE_COMMON = R"(
uniform vec3 box_center;
uniform vec3 box_size = vec3(16.0, 10.0, 16.0);
uniform vec3 eye;
uniform float pixel_angle = 0.001;
uniform float ratio = 0.0;
uniform vec3 still_wind;
uniform sampler2D ground_tex : filter_linear, repeat_disable;
uniform vec4 ground_rect;

uint snow_hash(uint x) {
	x ^= x >> 16u;
	x *= 2146121005u;
	x ^= x >> 15u;
	x *= 2221713035u;
	x ^= x >> 16u;
	return x;
}
float snow_rand(uint x) {
	return float(snow_hash(x) >> 8u) * (1.0 / 16777216.0);
}
float snow_ground(vec2 p) {
	vec2 uv = (p - ground_rect.xy) / max(ground_rect.zw, vec2(1e-4));
	if (ground_rect.z <= 0.0 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
		return -1e9;
	}
	return textureLod(ground_tex, uv, 0.0).r;
}
uniform float turb_sigma = 0.0; // m/s, standard deviation of each component near the ground
uniform float turb_time = 0.0;
// Turbulent eddies on top of the weather wind: Kraichnan's random Fourier modes (Kraichnan 1970), each
// a divergence-free wave (its amplitude across its wavenumber) of 2 to 14 m, frozen in the mean wind
// (Taylor's hypothesis) and slowly turning over. sigma_u is about 2.5 u* near the surface (Panofsky
// and Dutton 1984).
vec3 snow_eddies(vec3 p, vec3 mean) {
	if (turb_sigma <= 0.0) {
		return vec3(0.0);
	}
	vec3 q = p - mean * turb_time;
	vec3 u = vec3(0.0);
	for (int n = 0; n < 6; n++) {
		uint s = snow_hash(uint(n) * 7919u + 101u);
		float ct = 2.0 * snow_rand(s) - 1.0;
		float ph = 6.2831853 * snow_rand(s + 1u);
		vec3 dir = vec3(sqrt(1.0 - ct * ct) * cos(ph), ct * 0.5, sqrt(1.0 - ct * ct) * sin(ph));
		dir = normalize(dir);
		float len = mix(2.0, 14.0, float(n) / 5.0);
		vec3 k = dir * (6.2831853 / len);
		vec3 a = normalize(cross(dir, vec3(snow_rand(s + 2u) - 0.5, snow_rand(s + 3u) - 0.5, snow_rand(s + 4u) - 0.5)));
		u += a * cos(dot(k, q) + 6.2831853 * snow_rand(s + 5u) + turb_time * 0.4 * length(k));
	}
	return u * turb_sigma * 0.7071 * sqrt(3.0 / 6.0);
}
vec3 snow_air(vec3 p, float h) {
	vec3 w = weather_map_rect.z > 0.0 ? weather_wind(p, h) : still_wind;
	return w + snow_eddies(p, w) * clamp(h / 0.5, 0.2, 1.0);
}
uniform float streak_time = 0.0167;

// A quad facing the eye, stretched along the motion seen during one exposure (streak_time), so fast
// flakes and grains read as the streaks a camera records. The light is kept: sub-pixel particles are
// drawn at least 1.5 pixels wide and long, and alpha falls by the ratio of the drawn area to the
// particle's own. Built here (no billboard), so the path tracer traces the same quad.
mat4 snow_streak(vec3 p, vec3 v, float size, inout float alpha) {
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
	alpha *= (size * size) / (w * l);
	if (alpha < 0.003) {
		alpha = 0.0;
		return mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	}
	return mat4(vec4(across * w, 0.0), vec4(along * l, 0.0), vec4(-view * w, 0.0), vec4(p, 1.0));
}
)";

static const char *SNOW_FALL_SHADER = R"(
// Falling snow (SnowFall3D): Gunn and Marshall (1958) sizes, Locatelli and Hobbs (1974) speeds,
// flutter after Nemes et al. (2017), carried by the weather wind.
uniform float lambda = 2.5; // per mm of melted diameter
uniform float dm_min = 1.5; // mm, melted diameter
uniform float size_scale = 1.0;

// Magono and Nakamura (1965): dry flake density 0.02 / D^2 g/cm3 (D in cm), so the flake of a
// melted diameter Dm mm is Dm^3 / 2 mm across (and never denser than ice water).
float snow_flake_size(float dm) {
	return max(dm, 0.5 * dm * dm * dm);
}

void start() {
	uint s = snow_hash(INDEX * 7919u + RANDOM_SEED);
	vec3 r = vec3(snow_rand(s), snow_rand(s + 1u), snow_rand(s + 2u));
	vec3 p = box_center + (r - 0.5) * box_size;
	// Truncated at 3.2 mm melted (16 mm aggregates): the exponential tail beyond is rare and only
	// paints blots across the lens.
	float tail = 1.0 - exp(-lambda * max(3.2 - dm_min, 0.0));
	float dm = dm_min - log(max(1.0 - snow_rand(s + 3u) * tail, 1e-6)) / lambda;
	CUSTOM = vec4(dm, snow_rand(s + 4u) * 6.2831853, 0.7 + 0.6 * snow_rand(s + 5u), snow_rand(snow_hash(INDEX * 2654435761u + 17u)));
	TRANSFORM = mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	COLOR = vec4(1.0, 1.0, 1.0, 0.0);
}

void process() {
	float d = snow_flake_size(CUSTOM.x);
	vec3 p = TRANSFORM[3].xyz;
	float g = snow_ground(p.xz);
	vec3 w = snow_air(p, max(p.y - g, 0.2));
	float vt = 0.8 * pow(d, 0.16);
	float om = 6.2831853 * 1.3 * CUSTOM.z;
	float ph = CUSTOM.y + TIME * om;
	float amp = 0.01 + 0.003 * d;
	vec3 flutter = vec3(cos(ph), 0.25 * sin(2.0 * ph), sin(0.8 * ph)) * amp * om;
	vec3 v = w + vec3(0.0, -vt, 0.0) + flutter;
	p += v * DELTA;
	vec3 rel = p - box_center;
	uint s = snow_hash(INDEX * 9781u + uint(TIME * 61.0) * 13u);
	if (abs(rel.y) > box_size.y || any(greaterThan(abs(rel.xz), box_size.xz))) {
		// The box jumped: scatter through it.
		rel = (vec3(snow_rand(s), snow_rand(s + 1u), snow_rand(s + 2u)) - 0.5) * box_size;
	} else if (COLLIDED || p.y < g || rel.y < -0.5 * box_size.y) {
		// Landed (the snowpack takes it) or sheltered: back in at the top.
		rel.y = 0.5 * box_size.y - 0.5 * snow_rand(s + 3u);
		rel.xz = (vec2(snow_rand(s), snow_rand(s + 1u)) - 0.5) * box_size.xz;
	} else if (rel.y > 0.5 * box_size.y) {
		rel.y -= box_size.y;
	}
	rel.xz = mod(rel.xz + 0.5 * box_size.xz, box_size.xz) - 0.5 * box_size.xz;
	p = box_center + rel;
	VELOCITY = v;
	vec2 edge = 0.5 * box_size.xz - abs(rel.xz);
	float fade = smoothstep(0.0, 2.0, min(edge.x, edge.y)) * smoothstep(0.0, 1.0, 0.5 * box_size.y - abs(rel.y));
	// Flakes inside a metre of the eye are out of focus and fill the view: faded, as a lens would blur
	// them to nothing.
	float near = smoothstep(0.4, 1.2, distance(p, eye));
	float a = step(CUSTOM.w, ratio) * fade * near;
	TRANSFORM = snow_streak(p, v, d * 0.001 * size_scale, a);
	COLOR = vec4(1.0, 1.0, 1.0, a);
}
)";

static const char *SNOW_BLOW_SHADER = R"(
// Blowing snow (SnowFall3D): saltating grains a few centimetres deep and lifted snow above them,
// moving with the wind's log profile, only over loose dry snow.
uniform sampler2D snow_tex : filter_linear, repeat_disable;
uniform vec4 snow_rect;
uniform float salt_height = 0.05; // m, e-folding height of the saltation layer
uniform float lift_height = 0.6; // m, of the suspended snow
uniform float max_density = 0.35; // erodible below this density / 1000

bool snow_erodible(vec2 p) {
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (snow_rect.z <= 0.0 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
		return false;
	}
	vec4 s = textureLod(snow_tex, uv, 0.0);
	// Loose: a light pack, or a fresh surface on a settled one (albedo above 0.78 is snow a couple of days old).
	return s.r > 0.02 && s.b < 0.05 && (s.a < max_density || s.g > 0.78);
}

void snow_spawn(inout vec4 c, inout vec3 p, uint s) {
	bool grain = snow_rand(s) < 0.7;
	vec2 xz = box_center.xz + (vec2(snow_rand(s + 1u), snow_rand(s + 2u)) - 0.5) * box_size.xz;
	float h = (grain ? salt_height : lift_height) * -log(max(snow_rand(s + 3u), 1e-4));
	float g = snow_ground(xz);
	p = vec3(xz.x, (g > -1e8 ? g : box_center.y) + h, xz.y);
	c.x = grain ? 0.0 : 1.0;
	c.y = 0.0;
	c.z = grain ? 0.8 + 1.4 * snow_rand(s + 4u) : 2.0 + 2.5 * snow_rand(s + 4u);
}

void start() {
	uint s = snow_hash(INDEX * 6151u + RANDOM_SEED);
	vec4 c = vec4(0.0, 0.0, 1.0, snow_rand(snow_hash(INDEX * 2654435761u + 31u)));
	vec3 p;
	snow_spawn(c, p, s);
	c.y = snow_rand(s + 9u); // staggered ages
	CUSTOM = c;
	TRANSFORM = mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	COLOR = vec4(1.0, 1.0, 1.0, 0.0);
}

void process() {
	vec4 c = CUSTOM;
	vec3 p = TRANSFORM[3].xyz;
	c.y += DELTA / c.z;
	vec3 rel = p - box_center;
	if (c.y >= 1.0 || any(greaterThan(abs(rel.xz), 0.5 * box_size.xz))) {
		snow_spawn(c, p, snow_hash(INDEX * 3571u + uint(TIME * 61.0) * 7u));
	}
	float g = snow_ground(p.xz);
	bool grain = c.x < 0.5;
	uint k = snow_hash(INDEX * 40503u);
	float h0 = (grain ? salt_height : lift_height) * (0.2 + 1.6 * snow_rand(k));
	// Grains hop: parabolic arcs, three to five a life. Lifted snow rises slowly as it travels.
	float hops = 3.0 + 2.0 * snow_rand(k + 1u);
	float arc = fract(c.y * hops);
	float h = grain ? h0 * 4.0 * arc * (1.0 - arc) + 0.003 : h0 * (1.0 + 0.8 * c.y);
	vec3 w = snow_air(vec3(p.x, g + max(h, 0.02), p.z), max(h, 0.02));
	vec2 move = w.xz * (grain ? 0.6 : 1.0);
	p.xz += move * DELTA;
	p.y = (g > -1e8 ? g : box_center.y) + h;
	VELOCITY = vec3(move.x, 0.0, move.y);
	float life = sin(3.14159 * clamp(c.y, 0.0, 1.0));
	float size = grain ? (0.3 + 0.5 * snow_rand(k + 2u)) * 0.001 : 0.12 + 0.3 * snow_rand(k + 2u);
	float a = step(c.w, ratio) * (snow_erodible(p.xz) ? 1.0 : 0.0) * life * (grain ? 0.9 : 0.1);
	TRANSFORM = snow_streak(p, VELOCITY, size, a);
	CUSTOM = c;
	COLOR = vec4(1.0, 1.0, 1.0, a);
}
)";

// ---------------------------------------------------------------------------------------------------------
// Relations

float SnowFall3D::flake_size_slope(float p_rate_mm_h) {
	return 2.55f * Math::pow(MAX(p_rate_mm_h, 0.01f), -0.48f);
}

static float snow_melted_diameter(float p_flake_mm) {
	// Inverse of the flake size Dm^3 / 2 (Magono and Nakamura 1965), never below the melted size.
	return p_flake_mm >= 1.414f ? Math::pow(2.0f * p_flake_mm, 1.0f / 3.0f) : p_flake_mm;
}

float SnowFall3D::flake_number_density(float p_rate_mm_h, float p_min_size_mm) {
	if (p_rate_mm_h <= 0.0f) {
		return 0.0f;
	}
	const float n0 = 3.8e3f * Math::pow(p_rate_mm_h, -0.87f); // per m3 per mm
	const float lambda = flake_size_slope(p_rate_mm_h);
	return n0 / lambda * Math::exp(-lambda * snow_melted_diameter(p_min_size_mm));
}

float SnowFall3D::snowfall_extinction(float p_rate_mm_h) {
	// Geometric optics (extinction efficiency 2) over the Gunn and Marshall distribution, flakes sized
	// by Magono and Nakamura: sigma = integral of N(Dm) 2 pi D^2 / 4 dDm. Visibility 3.912 / sigma
	// (Koschmieder): about 2 km at 1 mm/h and 200 m at 3 mm/h, as observed (Rasmussen et al. 1999).
	if (p_rate_mm_h <= 0.0f) {
		return 0.0f;
	}
	const float n0 = 3.8e3f * Math::pow(p_rate_mm_h, -0.87f);
	const float lambda = flake_size_slope(p_rate_mm_h);
	double sigma = 0.0;
	const float step = 0.02f;
	for (float dm = step * 0.5f; dm < 8.0f; dm += step) {
		const float d = MAX(dm, 0.5f * dm * dm * dm) * 0.001f;
		sigma += n0 * Math::exp(-lambda * dm) * step * 2.0 * Math::PI * d * d * 0.25;
	}
	return float(sigma);
}

float SnowFall3D::fall_speed(float p_size_mm) {
	return 0.8f * Math::pow(MAX(p_size_mm, 0.01f), 0.16f);
}

float SnowFall3D::blowing_threshold(float p_celsius) {
	const float t = MIN(p_celsius, 0.0f);
	return 9.43f + 0.18f * t + 0.0033f * t * t;
}

float SnowFall3D::blowing_concentration(float p_wind10, float p_height) {
	// Pomeroy and Male (1992), PBSM: eta(z) = eta_r exp(-1.55 (z_r^-0.544 - z^-0.544)), eta_r 0.8 kg/m3 at the
	// top of the saltation layer z_r = 0.05628 u*^1.27, u* from the 10 m wind over snow (z0 0.5 mm).
	const float ustar = 0.41f * MAX(p_wind10, 0.0f) / Math::log(10.0f / 0.0005f);
	if (ustar <= 0.01f) {
		return 0.0f;
	}
	const float zr = 0.05628f * Math::pow(ustar, 1.27f);
	const float z = MAX(p_height, zr);
	return 0.8f * Math::exp(-1.55f * (Math::pow(zr, -0.544f) - Math::pow(z, -0.544f)));
}

float SnowFall3D::blowing_extinction(float p_concentration) {
	// Geometric optics, extinction efficiency 2: sigma = 3 Q c / (4 rho_ice r) (Pomeroy and Male 1988),
	// suspended grains about 40 um in radius at a metre (Schmidt 1982).
	return 1.5f * MAX(p_concentration, 0.0f) / (917.0f * 40e-6f);
}

float SnowFall3D::saltation_flux_ratio(float p_wind10, float p_threshold) {
	if (p_threshold <= 0.0f || p_wind10 <= p_threshold) {
		return 0.0f;
	}
	// Pomeroy (1989): flux grows as u*^3 (1 - u*t^2 / u*^2); u* is proportional to the 10 m wind.
	auto q = [](float u, float ut) { return u * u * u * (1.0f - (ut * ut) / (u * u)); };
	return q(p_wind10, p_threshold) / q(1.5f * p_threshold, p_threshold);
}

// ---------------------------------------------------------------------------------------------------------
// Setup

SnowCover3D *SnowFall3D::_cover() const {
	if (!snow_cover.is_empty() && is_inside_tree()) {
		SnowCover3D *c = Object::cast_to<SnowCover3D>(get_node_or_null(snow_cover));
		if (c) {
			return c;
		}
	}
	return SnowCover3D::get_active();
}

Vector3 SnowFall3D::_follow_position() const {
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

static Ref<StandardMaterial3D> snow_particle_material(float p_softness) {
	// Lit, translucent to the light behind it (ice scatters forward), a soft round edge.
	Ref<StandardMaterial3D> m;
	m.instantiate();
	// No billboard: the particle shaders build each quad facing the eye, stretched along its motion.
	m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	m->set_albedo(Color(0.95, 0.96, 0.98));
	m->set_roughness(1.0f);
	// A flake is a loose clump of ice, lit from all sides, not a flat card: wrapped diffuse plus light
	// passing through from behind.
	m->set_diffuse_mode(BaseMaterial3D::DIFFUSE_LAMBERT_WRAP);
	m->set_specular(0.335f); // ice, F0 0.018
	m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
	m->set_backlight(Color(0.85, 0.87, 0.9));
	Ref<Gradient> g;
	g.instantiate();
	g->set_color(0, Color(1, 1, 1, 1));
	g->set_color(1, Color(1, 1, 1, 0));
	g->set_offset(0, p_softness);
	g->set_offset(1, 1.0f);
	Ref<GradientTexture2D> t;
	t.instantiate();
	t->set_gradient(g);
	t->set_fill(GradientTexture2D::FILL_RADIAL);
	t->set_fill_from(Vector2(0.5, 0.5));
	t->set_fill_to(Vector2(1.0, 0.5));
	t->set_width(64);
	t->set_height(64);
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, t);
	return m;
}

void SnowFall3D::_ensure_children() {
	if (!fall) {
		fall = memnew(GPUParticles3D);
		fall->set_as_top_level(true);
		fall->set_use_local_coordinates(false);
		fall->set_explosiveness_ratio(1.0f);
		fall->set_lifetime(3600.0);
		fall->set_collision_base_size(0.01f);
		fall->set_process_material(fall_process);
		Ref<QuadMesh> q;
		q.instantiate();
		q->set_size(Size2(1, 1));
		q->set_material(flake_material);
		fall->set_draw_pass_mesh(0, q);
		add_child(fall, false, INTERNAL_MODE_FRONT);
	}
	if (!blow) {
		blow = memnew(GPUParticles3D);
		blow->set_as_top_level(true);
		blow->set_use_local_coordinates(false);
		blow->set_explosiveness_ratio(1.0f);
		blow->set_lifetime(3600.0);
		blow->set_process_material(blow_process);
		Ref<QuadMesh> q;
		q.instantiate();
		q->set_size(Size2(1, 1));
		q->set_material(grain_material);
		blow->set_draw_pass_mesh(0, q);
		add_child(blow, false, INTERNAL_MODE_FRONT);
	}
	if (shelter_enabled && !shelter) {
		shelter = memnew(GPUParticlesCollisionHeightField3D);
		shelter->set_as_top_level(true);
		shelter->set_follow_camera_enabled(true);
		shelter->set_resolution(GPUParticlesCollisionHeightField3D::RESOLUTION_512);
		shelter->set_heightfield_mask(shelter_mask);
		add_child(shelter, false, INTERNAL_MODE_FRONT);
	} else if (!shelter_enabled && shelter) {
		shelter->queue_free();
		shelter = nullptr;
	}
	fall->set_amount(MAX(max_flakes, 1));
	fall->set_emitting(falling_enabled);
	fall->set_visible(falling_enabled);
	fall->set_visibility_aabb(AABB(-fall_extent * 0.5f - Vector3(1, 1, 1), fall_extent + Vector3(2, 2, 2)));
	blow->set_amount(MAX(max_blowing, 1));
	blow->set_emitting(blowing_enabled);
	blow->set_visible(blowing_enabled);
	blow->set_visibility_aabb(AABB(Vector3(-blow_extent * 0.5f, -50.0f, -blow_extent * 0.5f), Vector3(blow_extent, 100.0f, blow_extent)));
	if (shelter) {
		shelter->set_size(Vector3(fall_extent.x, fall_extent.y * 2.0f, fall_extent.z));
		shelter->set_heightfield_mask(shelter_mask);
	}
}

void SnowFall3D::_update_ground(Vector3 p_center) {
	// The snow top (ground plus pack) on a coarse grid: blowing snow rides on it, flakes stop at it.
	const float size = MAX(MAX(fall_extent.x, fall_extent.z), blow_extent) + 8.0f;
	const float step = size / (GROUND_N - 1);
	const Vector2 o(Math::snapped(p_center.x, 4.0f) - size * 0.5f, Math::snapped(p_center.z, 4.0f) - size * 0.5f);
	const SnowCover3D *cover = _cover();
	Vector<uint8_t> bytes;
	bytes.resize(GROUND_N * GROUND_N * 4);
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	if (cover) {
		cover->sample_ground_heights(o, step, GROUND_N, GROUND_N, w);
	}
	for (int z = 0; z < GROUND_N; z++) {
		for (int x = 0; x < GROUND_N; x++) {
			const Vector3 p(o.x + x * step, 0, o.y + z * step);
			float h = cover ? w[z * GROUND_N + x] : Math::NaN;
			if (!Math::is_finite(h)) {
				h = cover ? cover->get_ground_height_at(p) : p_center.y - fall_extent.y;
			}
			w[z * GROUND_N + x] = h + (cover ? cover->get_depth_at(p) : 0.0f);
		}
	}
	if (ground_image.is_null()) {
		ground_image = Image::create_from_data(GROUND_N, GROUND_N, false, Image::FORMAT_RF, bytes);
		ground_texture = ImageTexture::create_from_image(ground_image);
	} else {
		ground_image->set_data(GROUND_N, GROUND_N, false, Image::FORMAT_RF, bytes);
		ground_texture->update(ground_image);
	}
	// Texel centres on the samples.
	const Vector4 rect(o.x - step * 0.5f, o.y - step * 0.5f, step * GROUND_N, step * GROUND_N);
	for (Ref<ShaderMaterial> m : { fall_process, blow_process }) {
		m->set_shader_parameter("ground_tex", ground_texture);
		m->set_shader_parameter("ground_rect", rect);
	}
	ground_center = p_center;
	ground_valid = true;
}

void SnowFall3D::_update(double p_delta) {
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
	ground_accum += p_delta;
	if (!ground_valid || ground_accum > 2.0 || Vector2(center.x - ground_center.x, center.z - ground_center.z).length() > 4.0f) {
		ground_accum = 0.0;
		_update_ground(center);
	}
	SnowCover3D *cover = _cover();
	const WeatherMap3D *weather = WeatherMap3D::get_active();

	// Falling.
	cur_rate = snowfall_rate_override >= 0.0f ? snowfall_rate_override : (cover ? cover->get_snowfall_rate_at(center) : 0.0f);
	const float volume = fall_extent.x * fall_extent.y * fall_extent.z;
	const float wanted = flake_number_density(cur_rate, min_flake_size) * density_scale * volume;
	cur_ratio = falling_enabled ? CLAMP(wanted / MAX(max_flakes, 1), 0.0f, 1.0f) : 0.0f;
	fall->set_global_transform(Transform3D(Basis(), center));
	fall_process->set_shader_parameter("box_center", center);
	fall_process->set_shader_parameter("box_size", fall_extent);
	fall_process->set_shader_parameter("eye", eye);
	fall_process->set_shader_parameter("pixel_angle", pixel_angle);
	fall_process->set_shader_parameter("ratio", cur_ratio);
	fall_process->set_shader_parameter("lambda", flake_size_slope(MAX(cur_rate, 0.01f)));
	fall_process->set_shader_parameter("dm_min", snow_melted_diameter(min_flake_size));
	fall_process->set_shader_parameter("size_scale", flake_size_scale);
	fall_process->set_shader_parameter("streak_time", streak_time);
	blow_process->set_shader_parameter("streak_time", streak_time);
	if (shelter) {
		shelter->set_global_position(center);
	}

	// Blowing: the wind at 10 m over the ground under the camera against the threshold of dry snow.
	const float ground_y = ground_valid && cover ? cover->get_ground_height_at(center) : center.y - 2.0f;
	const Vector3 base(center.x, ground_y, center.z);
	const float temp = cover ? cover->get_air_temperature_at(base) : (weather ? weather->get_temperature() : -5.0f);
	cur_threshold = threshold_wind_override >= 0.0f ? threshold_wind_override : blowing_threshold(temp);
	cur_wind10 = weather ? weather->get_wind_at(base, 10.0f).length() : 0.0f;
	cur_blow = blowing_enabled && temp < 0.0f ? CLAMP(saltation_flux_ratio(cur_wind10, cur_threshold) * blowing_strength, 0.0f, 1.0f) : 0.0f;
	blow->set_global_transform(Transform3D(Basis(), base));
	blow_process->set_shader_parameter("box_center", base);
	blow_process->set_shader_parameter("eye", eye);
	blow_process->set_shader_parameter("pixel_angle", pixel_angle);
	blow_process->set_shader_parameter("ratio", cur_blow);
	// Saltation layer depth from the friction velocity (Pomeroy and Gray 1990: h = 0.08436 u*^1.27),
	// u* from the 10 m wind over snow (z0 0.5 mm); lifted snow climbs higher in stronger wind.
	const float ustar = 0.41f * cur_wind10 / Math::log(10.0f / 0.0005f);
	blow_process->set_shader_parameter("salt_height", MAX(0.08436f * Math::pow(MAX(ustar, 0.01f), 1.27f), 0.01f));
	// The suspended snow: PBSM's profile over loose snow, as a haze an e-folding height deep (fitted between
	// 0.2 and 2 m), and the lifted wisps spread through it.
	if (cur_blow > 0.0f) {
		const float c02 = blowing_concentration(cur_wind10, 0.2f);
		const float c2 = blowing_concentration(cur_wind10, 2.0f);
		cur_blow_scale = c2 > 0.0f && c02 > c2 ? CLAMP(1.8f / Math::log(c02 / c2), 0.1f, 20.0f) : 0.5f;
		cur_blow_sigma = blowing_extinction(blowing_concentration(cur_wind10, 1.0f)) * MIN(cur_blow * 2.0f, 1.0f);
	} else {
		cur_blow_scale = 0.0f;
		cur_blow_sigma = 0.0f;
	}
	const float lift = MAX(0.3f + 0.08f * MAX(cur_wind10 - cur_threshold, 0.0f), MIN(cur_blow_scale, 6.0f));
	blow_process->set_shader_parameter("lift_height", lift);
	blow_process->set_shader_parameter("box_size", Vector3(blow_extent, MAX(4.0f, 4.0f * lift), blow_extent));
	// Gusts and eddies: sigma_u = 2.5 u* (Panofsky and Dutton 1984).
	const float turb = turbulence * 2.5f * ustar;
	fall_process->set_shader_parameter("turb_sigma", turb);
	blow_process->set_shader_parameter("turb_sigma", turb);
	turb_clock += p_delta;
	fall_process->set_shader_parameter("turb_time", float(Math::fmod(turb_clock, 3600.0)));
	blow_process->set_shader_parameter("turb_time", float(Math::fmod(turb_clock, 3600.0)));
	_push_haze(WeatherMap3D::get_active());
	if (cover && cover->get_snow_texture().is_valid()) {
		const Rect2 r = cover->get_map_rect();
		blow_process->set_shader_parameter("snow_tex", cover->get_snow_texture());
		blow_process->set_shader_parameter("snow_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
	} else {
		blow_process->set_shader_parameter("snow_rect", Vector4());
	}
}

void SnowFall3D::_push_haze(WeatherMap3D *p_weather) {
	// The snow beyond the particles: the falling snow's extinction everywhere (the weather map's height
	// fog), the blowing snow's in a layer over the ground (a lit fog volume near the camera).
	if (!p_weather || !haze_enabled || !is_inside_tree()) {
		return;
	}
	const float fall_sigma = falling_enabled ? get_extinction() : 0.0f;
	auto changed = [](float a, float b) { return Math::abs(a - b) > 0.02f * MAX(Math::abs(b), 1e-4f); };
	if (changed(fall_sigma, pushed_fall)) {
		if (fall_sigma > 1e-5f) {
			p_weather->set_precipitation_extinction(this, fall_sigma, 0.0f, 0.97f);
		} else {
			p_weather->clear_precipitation_extinction(this);
		}
		pushed_fall = fall_sigma;
	}
	const Vector3 base = blow ? blow->get_global_position() : Vector3();
	const bool moved = blow && base.distance_to(pushed_base) > 20.0f;
	if (blow && (changed(cur_blow_sigma, pushed_blow) || changed(cur_blow_scale, pushed_scale) || (moved && cur_blow_sigma > 1e-5f))) {
		pushed_base = base;
		if (cur_blow_sigma > 1e-5f) {
			// set at the ground: the layer is sigma exp(-z / H); ours is the value at 1 m.
			// Around the camera, on the snow under it (the layer follows the terrain as the camera moves).
			p_weather->set_precipitation_extinction(blow, cur_blow_sigma * Math::exp(1.0f / MAX(cur_blow_scale, 0.1f)), cur_blow_scale, 0.97f, blow->get_global_position(), MAX(blow_extent * 4.0f, 150.0f));
		} else {
			p_weather->clear_precipitation_extinction(blow);
		}
		pushed_blow = cur_blow_sigma;
		pushed_scale = cur_blow_scale;
	}
}

void SnowFall3D::_clear_haze() {
	if (WeatherMap3D *w = WeatherMap3D::get_active()) {
		w->clear_precipitation_extinction(this);
		if (blow) {
			w->clear_precipitation_extinction(blow);
		}
	}
	pushed_fall = -1.0f;
	pushed_blow = -1.0f;
	pushed_scale = -1.0f;
}

void SnowFall3D::set_haze_enabled(bool p_enabled) {
	haze_enabled = p_enabled;
	if (!p_enabled) {
		_clear_haze();
	}
}

void SnowFall3D::_notification(int p_what) {
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
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update(get_process_delta_time());
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Settings

void SnowFall3D::set_falling_enabled(bool p_enabled) {
	falling_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_fall_extent(const Vector3 &p_extent) {
	fall_extent = p_extent.maxf(1.0f);
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_max_flakes(int p_count) {
	max_flakes = CLAMP(p_count, 1, 2000000);
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_shelter_enabled(bool p_enabled) {
	shelter_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_shelter_mask(uint32_t p_mask) {
	shelter_mask = p_mask;
	if (shelter) {
		shelter->set_heightfield_mask(p_mask);
	}
}
void SnowFall3D::set_blowing_enabled(bool p_enabled) {
	blowing_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_blow_extent(float p_extent) {
	blow_extent = CLAMP(p_extent, 2.0f, 1024.0f);
	if (is_inside_tree()) {
		_ensure_children();
	}
}
void SnowFall3D::set_max_blowing(int p_count) {
	max_blowing = CLAMP(p_count, 1, 2000000);
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void SnowFall3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &SnowFall3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &SnowFall3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &SnowFall3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &SnowFall3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_falling_enabled", "enabled"), &SnowFall3D::set_falling_enabled);
	ClassDB::bind_method(D_METHOD("is_falling_enabled"), &SnowFall3D::is_falling_enabled);
	ClassDB::bind_method(D_METHOD("set_fall_extent", "extent"), &SnowFall3D::set_fall_extent);
	ClassDB::bind_method(D_METHOD("get_fall_extent"), &SnowFall3D::get_fall_extent);
	ClassDB::bind_method(D_METHOD("set_max_flakes", "count"), &SnowFall3D::set_max_flakes);
	ClassDB::bind_method(D_METHOD("get_max_flakes"), &SnowFall3D::get_max_flakes);
	ClassDB::bind_method(D_METHOD("set_min_flake_size", "size"), &SnowFall3D::set_min_flake_size);
	ClassDB::bind_method(D_METHOD("get_min_flake_size"), &SnowFall3D::get_min_flake_size);
	ClassDB::bind_method(D_METHOD("set_flake_size_scale", "scale"), &SnowFall3D::set_flake_size_scale);
	ClassDB::bind_method(D_METHOD("get_flake_size_scale"), &SnowFall3D::get_flake_size_scale);
	ClassDB::bind_method(D_METHOD("set_streak_time", "seconds"), &SnowFall3D::set_streak_time);
	ClassDB::bind_method(D_METHOD("get_streak_time"), &SnowFall3D::get_streak_time);
	ClassDB::bind_method(D_METHOD("set_density_scale", "scale"), &SnowFall3D::set_density_scale);
	ClassDB::bind_method(D_METHOD("get_density_scale"), &SnowFall3D::get_density_scale);
	ClassDB::bind_method(D_METHOD("set_snowfall_rate_override", "rate"), &SnowFall3D::set_snowfall_rate_override);
	ClassDB::bind_method(D_METHOD("get_snowfall_rate_override"), &SnowFall3D::get_snowfall_rate_override);
	ClassDB::bind_method(D_METHOD("set_shelter_enabled", "enabled"), &SnowFall3D::set_shelter_enabled);
	ClassDB::bind_method(D_METHOD("is_shelter_enabled"), &SnowFall3D::is_shelter_enabled);
	ClassDB::bind_method(D_METHOD("set_shelter_mask", "mask"), &SnowFall3D::set_shelter_mask);
	ClassDB::bind_method(D_METHOD("get_shelter_mask"), &SnowFall3D::get_shelter_mask);
	ClassDB::bind_method(D_METHOD("set_blowing_enabled", "enabled"), &SnowFall3D::set_blowing_enabled);
	ClassDB::bind_method(D_METHOD("is_blowing_enabled"), &SnowFall3D::is_blowing_enabled);
	ClassDB::bind_method(D_METHOD("set_blow_extent", "extent"), &SnowFall3D::set_blow_extent);
	ClassDB::bind_method(D_METHOD("get_blow_extent"), &SnowFall3D::get_blow_extent);
	ClassDB::bind_method(D_METHOD("set_max_blowing", "count"), &SnowFall3D::set_max_blowing);
	ClassDB::bind_method(D_METHOD("get_max_blowing"), &SnowFall3D::get_max_blowing);
	ClassDB::bind_method(D_METHOD("set_blowing_strength", "strength"), &SnowFall3D::set_blowing_strength);
	ClassDB::bind_method(D_METHOD("get_blowing_strength"), &SnowFall3D::get_blowing_strength);
	ClassDB::bind_method(D_METHOD("set_threshold_wind_override", "speed"), &SnowFall3D::set_threshold_wind_override);
	ClassDB::bind_method(D_METHOD("get_threshold_wind_override"), &SnowFall3D::get_threshold_wind_override);
	ClassDB::bind_method(D_METHOD("get_snowfall_rate"), &SnowFall3D::get_snowfall_rate);
	ClassDB::bind_method(D_METHOD("get_visible_flakes"), &SnowFall3D::get_visible_flakes);
	ClassDB::bind_method(D_METHOD("get_blowing_amount"), &SnowFall3D::get_blowing_amount);
	ClassDB::bind_method(D_METHOD("get_threshold_wind"), &SnowFall3D::get_threshold_wind);
	ClassDB::bind_method(D_METHOD("get_wind_speed_10m"), &SnowFall3D::get_wind_speed_10m);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("flake_number_density", "rate", "min_size"), &SnowFall3D::flake_number_density);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("flake_size_slope", "rate"), &SnowFall3D::flake_size_slope);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("fall_speed", "size"), &SnowFall3D::fall_speed);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("snowfall_extinction", "rate"), &SnowFall3D::snowfall_extinction);
	ClassDB::bind_method(D_METHOD("get_extinction"), &SnowFall3D::get_extinction);
	ClassDB::bind_method(D_METHOD("get_blowing_extinction"), &SnowFall3D::get_blowing_extinction);
	ClassDB::bind_method(D_METHOD("get_blowing_height_scale"), &SnowFall3D::get_blowing_height_scale);
	ClassDB::bind_method(D_METHOD("set_haze_enabled", "enabled"), &SnowFall3D::set_haze_enabled);
	ClassDB::bind_method(D_METHOD("is_haze_enabled"), &SnowFall3D::is_haze_enabled);
	ClassDB::bind_method(D_METHOD("set_turbulence", "scale"), &SnowFall3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &SnowFall3D::get_turbulence);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("blowing_concentration", "wind10", "height"), &SnowFall3D::blowing_concentration);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("blowing_extinction", "concentration"), &SnowFall3D::blowing_extinction);
	ClassDB::bind_method(D_METHOD("get_visibility"), &SnowFall3D::get_visibility);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("blowing_threshold", "celsius"), &SnowFall3D::blowing_threshold);
	ClassDB::bind_static_method("SnowFall3D", D_METHOD("saltation_flux_ratio", "wind10", "threshold"), &SnowFall3D::saltation_flux_ratio);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "SnowCover3D"), "set_snow_cover", "get_snow_cover");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_GROUP("Falling", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "falling_enabled"), "set_falling_enabled", "is_falling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "fall_extent", PROPERTY_HINT_NONE, "suffix:m"), "set_fall_extent", "get_fall_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_flakes", PROPERTY_HINT_RANGE, "1,2000000,1"), "set_max_flakes", "get_max_flakes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_flake_size", PROPERTY_HINT_RANGE, "0.2,20,0.1,suffix:mm"), "set_min_flake_size", "get_min_flake_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flake_size_scale", PROPERTY_HINT_RANGE, "0.01,10,0.01"), "set_flake_size_scale", "get_flake_size_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streak_time", PROPERTY_HINT_RANGE, "0,0.1,0.0001,suffix:s"), "set_streak_time", "get_streak_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_density_scale", "get_density_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snowfall_rate_override", PROPERTY_HINT_RANGE, "-1,50,0.01,suffix:mm/h"), "set_snowfall_rate_override", "get_snowfall_rate_override");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shelter_enabled"), "set_shelter_enabled", "is_shelter_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shelter_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_shelter_mask", "get_shelter_mask");
	ADD_GROUP("Blowing", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blowing_enabled"), "set_blowing_enabled", "is_blowing_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "haze_enabled"), "set_haze_enabled", "is_haze_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blow_extent", PROPERTY_HINT_RANGE, "2,1024,1,suffix:m"), "set_blow_extent", "get_blow_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_blowing", PROPERTY_HINT_RANGE, "1,2000000,1"), "set_max_blowing", "get_max_blowing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blowing_strength", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_blowing_strength", "get_blowing_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "threshold_wind_override", PROPERTY_HINT_RANGE, "-1,40,0.1,suffix:m/s"), "set_threshold_wind_override", "get_threshold_wind_override");
}

SnowFall3D::SnowFall3D() {
	const String wind = WeatherMap3D::get_wind_shader_code();
	fall_shader.instantiate();
	fall_shader->set_code(String("shader_type particles;\nrender_mode disable_velocity, disable_force;\n") + wind + SNOW_PARTICLE_COMMON + SNOW_FALL_SHADER);
	blow_shader.instantiate();
	blow_shader->set_code(String("shader_type particles;\nrender_mode disable_velocity, disable_force;\n") + wind + SNOW_PARTICLE_COMMON + SNOW_BLOW_SHADER);
	fall_process.instantiate();
	fall_process->set_shader(fall_shader);
	blow_process.instantiate();
	blow_process->set_shader(blow_shader);
	flake_material = snow_particle_material(0.35f);
	grain_material = snow_particle_material(0.0f);
}

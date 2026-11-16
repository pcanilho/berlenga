/**************************************************************************/
/*  ground_streamers_3d.cpp                                               */
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

#include "ground_streamers_3d.h"

#include "ground_ripples.h"
#include "ground_surface_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/gradient_texture.h"

static constexpr float MEAN_HOP = 0.04f; // m (Namikas 2003)
static constexpr float SAND_DENSITY = 2650.0f; // kg/m3, quartz

static const char *STREAMER_SHADER = R"(
shader_type particles;
render_mode disable_velocity, disable_force;

uniform vec3 box_center;
uniform vec3 box_size = vec3(10.0, 4.0, 10.0);
uniform vec3 eye;
uniform float pixel_angle = 0.001;
uniform float ratio = 0.0; // share of the particles flying
uniform float per_particle = 1.0; // grains each stands for
uniform float coverage = 1.0; // share of the ground under streamers
uniform vec2 wind_dir = vec2(1.0, 0.0);
uniform float air_speed = 5.0; // m/s, the gusts carrying the streamers
uniform float clock = 0.0;
uniform float grain_size = 0.00025; // m
uniform float hop_k = 0.4; // 0.8 u*t / kappa: the grain speed is hop_k ln(h / z0)
uniform float z0 = 0.0001;
uniform float mean_hop = 0.04;
uniform float streak_time = 0.0167;
uniform sampler2D ground_tex : filter_linear, repeat_disable;
uniform vec4 ground_rect;

uint sh(uint x) {
	x ^= x >> 16u;
	x *= 2146121005u;
	x ^= x >> 15u;
	x *= 2221713035u;
	x ^= x >> 16u;
	return x;
}
float sr(uint x) {
	return float(sh(x) >> 8u) * (1.0 / 16777216.0);
}
// The sand surface (r) and the share of it the wind moves (g).
vec2 ground(vec2 p) {
	vec2 uv = (p - ground_rect.xy) / max(ground_rect.zw, vec2(1e-4));
	if (ground_rect.z <= 0.0 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
		return vec2(-1e9, 0.0);
	}
	return textureLod(ground_tex, uv, 0.0).rg;
}
float vhash(vec3 c) {
	ivec3 i = ivec3(floor(c));
	return sr(uint(i.x) * 73856093u ^ uint(i.y) * 19349663u ^ uint(i.z) * 83492791u);
}
float vnoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = p - i;
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(vhash(i), vhash(i + vec3(1, 0, 0)), f.x), mix(vhash(i + vec3(0, 1, 0)), vhash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(vhash(i + vec3(0, 0, 1)), vhash(i + vec3(1, 0, 1)), f.x), mix(vhash(i + vec3(0, 1, 1)), vhash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// Streamers (Baas and Sherman 2005): ribbons about 20 cm wide and 2 to 3 m long, carried downwind by the
// gusts and turning over in a couple of seconds. True where a grain is inside one.
bool in_streamer(vec2 xz) {
	if (coverage >= 0.99) {
		return true;
	}
	vec2 a = vec2(dot(xz, wind_dir), dot(xz, vec2(-wind_dir.y, wind_dir.x)));
	a.x -= air_speed * clock;
	float n = 0.65 * vnoise(vec3(a.x / 2.5, a.y / 0.2, clock * 0.5)) + 0.35 * vnoise(vec3(a.x / 1.0, a.y / 0.08, clock * 1.1) + 17.0);
	// The sum's spread is about 0.5 +- 0.2: the threshold for the wanted share.
	return n < 0.5 + 0.42 * (coverage - 0.5);
}

void spawn(inout vec4 c, inout vec3 p, uint s) {
	vec2 xz = box_center.xz + (vec2(sr(s), sr(s + 1u)) - 0.5) * box_size.xz;
	c.x = clamp(-mean_hop * log(max(sr(s + 2u), 1e-4)), 0.004, 0.3); // hop height, m
	c.y = 0.0; // distance flown, m
	c.z = 3.0 + 3.0 * sr(s + 3u); // hops before it settles
	p = vec3(xz.x, box_center.y, xz.y);
}

mat4 streak(vec3 p, vec3 v, float size, inout float alpha) {
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
	alpha = min(alpha * (size * size) / (w * l), 1.0);
	if (alpha < 0.002) {
		alpha = 0.0;
		return mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	}
	return mat4(vec4(across * w, 0.0), vec4(along * l, 0.0), vec4(-view * w, 0.0), vec4(p, 1.0));
}

void start() {
	uint s = sh(INDEX * 6151u + RANDOM_SEED);
	vec4 c = vec4(0.0, 0.0, 0.0, sr(sh(INDEX * 2654435761u + 31u)));
	vec3 p;
	spawn(c, p, s);
	c.y = sr(s + 9u) * c.z * 0.3; // staggered
	CUSTOM = c;
	TRANSFORM = mat4(vec4(0.0), vec4(0.0), vec4(0.0), vec4(p, 1.0));
	COLOR = vec4(1.0, 1.0, 1.0, 0.0);
}

void process() {
	vec4 c = CUSTOM;
	vec3 p = TRANSFORM[3].xyz;
	float h = c.x;
	// Speed at the hop's top, then the hop's length from its flight time (ballistic, drag ignored).
	float v = hop_k * log(h / z0);
	float hop = v * 2.0 * sqrt(2.0 * h / 9.81);
	p.xz += wind_dir * v * DELTA;
	c.y += v * DELTA;
	vec3 rel = p - box_center;
	if (c.y >= c.z * hop || any(greaterThan(abs(rel.xz), 0.5 * box_size.xz))) {
		if (any(greaterThan(abs(rel.xz), 0.5 * box_size.xz)) && c.y < c.z * hop) {
			// Blown out of the box (or the box moved): in again on the far side.
			rel.xz = mod(rel.xz + 0.5 * box_size.xz, box_size.xz) - 0.5 * box_size.xz;
			p.xz = box_center.xz + rel.xz;
		} else {
			spawn(c, p, sh(INDEX * 3571u + uint(TIME * 61.0) * 7u));
			h = c.x;
			v = hop_k * log(h / z0);
			hop = v * 2.0 * sqrt(2.0 * h / 9.81);
		}
	}
	vec2 g = ground(p.xz);
	float ph = fract(c.y / hop);
	p.y = (g.x > -1e8 ? g.x : box_center.y) + 4.0 * h * ph * (1.0 - ph) + 0.0005;
	vec3 vel = vec3(wind_dir.x * v, 4.0 * h * (1.0 - 2.0 * ph) * v / hop, wind_dir.y * v);
	VELOCITY = vel;
	rel = p - box_center;
	vec2 edge = 0.5 * box_size.xz - abs(rel.xz);
	float fade = smoothstep(0.0, 1.0, min(edge.x, edge.y)) * smoothstep(0.3, 0.8, distance(p, eye));
	float on = step(c.w, ratio) * (in_streamer(p.xz) ? 1.0 : 0.0);
	// Each particle is per_particle grains: drawn with their summed cross-section.
	float a = on * g.y * fade;
	TRANSFORM = streak(p, vel, grain_size * sqrt(per_particle), a);
	CUSTOM = c;
	COLOR = vec4(1.0, 1.0, 1.0, a);
}
)";

float GroundStreamers3D::grain_speed() {
	// At the mean hop's top.
	return 0.8f * GroundRipples::SAND_USTAR_T / 0.4f * Math::log(MEAN_HOP / GroundRipples::SAND_Z0);
}

float GroundStreamers3D::streamer_coverage(float p_ustar) {
	if (p_ustar <= GroundRipples::SAND_USTAR_T) {
		return 0.0f;
	}
	const float x = (p_ustar / GroundRipples::SAND_USTAR_T - 1.0f);
	return CLAMP(0.15f + 0.85f * x, 0.15f, 1.0f);
}

GroundSurface3D *GroundStreamers3D::_surface() const {
	if (!ground_surface.is_empty() && is_inside_tree()) {
		GroundSurface3D *s = Object::cast_to<GroundSurface3D>(get_node_or_null(ground_surface));
		if (s) {
			return s;
		}
	}
	return GroundSurface3D::get_active();
}

Vector3 GroundStreamers3D::_follow_position() const {
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

void GroundStreamers3D::_ensure_children() {
	if (!particles) {
		particles = memnew(GPUParticles3D);
		particles->set_as_top_level(true);
		particles->set_use_local_coordinates(false);
		particles->set_explosiveness_ratio(1.0f);
		particles->set_lifetime(3600.0);
		particles->set_process_material(process);
		Ref<QuadMesh> q;
		q.instantiate();
		q->set_size(Size2(1, 1));
		q->set_material(grain_material);
		particles->set_draw_pass_mesh(0, q);
		add_child(particles, false, INTERNAL_MODE_FRONT);
	}
	particles->set_amount(MAX(max_grains, 1));
	particles->set_visibility_aabb(AABB(Vector3(-extent * 0.5f, -50.0f, -extent * 0.5f), Vector3(extent, 100.0f, extent)));
}

void GroundStreamers3D::_update_ground(GroundSurface3D *p_surface, Vector3 p_center) {
	const float size = extent + 4.0f;
	const float step = size / (GROUND_N - 1);
	const Vector2 o(Math::snapped(p_center.x, 1.0f) - size * 0.5f, Math::snapped(p_center.z, 1.0f) - size * 0.5f);
	Vector<uint8_t> bytes;
	bytes.resize(GROUND_N * GROUND_N * 8);
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	for (int z = 0; z < GROUND_N; z++) {
		for (int x = 0; x < GROUND_N; x++) {
			const float px = o.x + x * step;
			const float pz = o.y + z * step;
			w[(z * GROUND_N + x) * 2 + 0] = p_surface->get_surface_height_at(Vector3(px, 0.0f, pz));
			w[(z * GROUND_N + x) * 2 + 1] = p_surface->get_blown_at(px, pz);
		}
	}
	if (ground_image.is_null()) {
		ground_image = Image::create_from_data(GROUND_N, GROUND_N, false, Image::FORMAT_RGF, bytes);
		ground_texture = ImageTexture::create_from_image(ground_image);
	} else {
		ground_image->set_data(GROUND_N, GROUND_N, false, Image::FORMAT_RGF, bytes);
		ground_texture->update(ground_image);
	}
	process->set_shader_parameter("ground_tex", ground_texture);
	process->set_shader_parameter("ground_rect", Vector4(o.x - step * 0.5f, o.y - step * 0.5f, step * GROUND_N, step * GROUND_N));
	ground_center = p_center;
	ground_valid = true;
}

void GroundStreamers3D::_update(double p_delta) {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_children();
	GroundSurface3D *surface = _surface();
	const Vector3 center = _follow_position();
	Vector3 eye = center;
	float pixel_angle = 0.001f;
	if (get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			eye = cam->get_global_position();
			const float hp = MAX(get_viewport()->get_visible_rect().size.y, 1.0f);
			pixel_angle = cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE ? 2.0f * Math::tan(Math::deg_to_rad(cam->get_fov()) * 0.5f) / hp : cam->get_size() / hp / MAX(eye.distance_to(center), 1.0f);
		}
	}
	const Vector2 wind = surface ? surface->get_wind_10m() : Vector2();
	cur_wind10 = wind.length();
	cur_ustar = GroundRipples::friction_speed(cur_wind10);
	cur_flux = GroundRipples::saltation_flux(cur_ustar) * strength;
	cur_coverage = streamer_coverage(cur_ustar);
	const bool on = surface && cur_flux > 0.0f;
	if (particles->is_emitting() != on) {
		particles->set_emitting(on);
	}
	particles->set_visible(on);
	if (!on) {
		cur_ratio = 0.0f;
		cur_per_particle = 0.0f;
		return;
	}
	ground_accum += p_delta;
	if (!ground_valid || ground_accum > 1.0 || Vector2(center.x - ground_center.x, center.z - ground_center.z).length() > 2.0f) {
		ground_accum = 0.0;
		_update_ground(surface, center);
	}
	// Grains in the air over the box, put on the share of the particles inside streamers.
	const float d = grain_size * 0.001f;
	const float grain_mass = SAND_DENSITY * Math::PI / 6.0f * d * d * d;
	const float grains = airborne_mass(cur_flux) * extent * extent / grain_mass;
	const float slots = MAX(max_grains, 1) * MAX(cur_coverage, 0.01f);
	cur_ratio = MIN(grains / slots, 1.0f);
	cur_per_particle = MAX(grains / slots, 1.0f);
	clock = Math::fmod(clock + p_delta, 3600.0);
	const Vector2 dir = wind / MAX(cur_wind10, 1e-3f);
	const float ground_y = surface->get_surface_height_at(center);
	const Vector3 base(center.x, ground_y, center.z);
	particles->set_global_transform(Transform3D(Basis(), base));
	process->set_shader_parameter("box_center", base);
	process->set_shader_parameter("box_size", Vector3(extent, 4.0f, extent));
	process->set_shader_parameter("eye", eye);
	process->set_shader_parameter("pixel_angle", pixel_angle);
	process->set_shader_parameter("ratio", cur_ratio);
	process->set_shader_parameter("per_particle", cur_per_particle);
	process->set_shader_parameter("coverage", cur_coverage);
	process->set_shader_parameter("wind_dir", dir);
	// The streamers ride the gusts near the ground: the log-law wind at 20 cm.
	process->set_shader_parameter("air_speed", cur_ustar / 0.4f * Math::log(0.2f / GroundRipples::SAND_Z0));
	process->set_shader_parameter("clock", float(clock));
	process->set_shader_parameter("grain_size", d);
	process->set_shader_parameter("hop_k", 0.8f * GroundRipples::SAND_USTAR_T / 0.4f);
	process->set_shader_parameter("z0", GroundRipples::SAND_Z0);
	process->set_shader_parameter("mean_hop", MEAN_HOP);
	process->set_shader_parameter("streak_time", streak_time);
}

void GroundStreamers3D::_notification(int p_what) {
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
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update(get_process_delta_time());
		} break;
	}
}

void GroundStreamers3D::set_extent(float p_extent) {
	extent = CLAMP(p_extent, 1.0f, 256.0f);
	ground_valid = false;
	if (particles) {
		_ensure_children();
	}
}

void GroundStreamers3D::set_max_grains(int p_count) {
	max_grains = CLAMP(p_count, 1, 2000000);
	if (particles) {
		_ensure_children();
	}
}

void GroundStreamers3D::set_grain_color(const Color &p_color) {
	grain_color = p_color;
	grain_material->set_albedo(p_color);
}

void GroundStreamers3D::_bind_methods() {
	ClassDB::bind_static_method("GroundStreamers3D", D_METHOD("grain_speed"), &GroundStreamers3D::grain_speed);
	ClassDB::bind_static_method("GroundStreamers3D", D_METHOD("airborne_mass", "flux"), &GroundStreamers3D::airborne_mass);
	ClassDB::bind_static_method("GroundStreamers3D", D_METHOD("streamer_coverage", "friction_speed"), &GroundStreamers3D::streamer_coverage);
	ClassDB::bind_method(D_METHOD("set_ground_surface", "path"), &GroundStreamers3D::set_ground_surface);
	ClassDB::bind_method(D_METHOD("get_ground_surface"), &GroundStreamers3D::get_ground_surface);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &GroundStreamers3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &GroundStreamers3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &GroundStreamers3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &GroundStreamers3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_max_grains", "count"), &GroundStreamers3D::set_max_grains);
	ClassDB::bind_method(D_METHOD("get_max_grains"), &GroundStreamers3D::get_max_grains);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &GroundStreamers3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &GroundStreamers3D::get_strength);
	ClassDB::bind_method(D_METHOD("set_grain_size", "mm"), &GroundStreamers3D::set_grain_size);
	ClassDB::bind_method(D_METHOD("get_grain_size"), &GroundStreamers3D::get_grain_size);
	ClassDB::bind_method(D_METHOD("set_streak_time", "seconds"), &GroundStreamers3D::set_streak_time);
	ClassDB::bind_method(D_METHOD("get_streak_time"), &GroundStreamers3D::get_streak_time);
	ClassDB::bind_method(D_METHOD("set_grain_color", "color"), &GroundStreamers3D::set_grain_color);
	ClassDB::bind_method(D_METHOD("get_grain_color"), &GroundStreamers3D::get_grain_color);
	ClassDB::bind_method(D_METHOD("get_wind"), &GroundStreamers3D::get_wind);
	ClassDB::bind_method(D_METHOD("get_flux"), &GroundStreamers3D::get_flux);
	ClassDB::bind_method(D_METHOD("get_coverage"), &GroundStreamers3D::get_coverage);
	ClassDB::bind_method(D_METHOD("get_active_ratio"), &GroundStreamers3D::get_active_ratio);
	ClassDB::bind_method(D_METHOD("get_grains_per_particle"), &GroundStreamers3D::get_grains_per_particle);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GroundSurface3D"), "set_ground_surface", "get_ground_surface");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "1,256,0.5,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_grains", PROPERTY_HINT_RANGE, "1,2000000,1"), "set_max_grains", "get_max_grains");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grain_size", PROPERTY_HINT_RANGE, "0.05,2,0.01,suffix:mm"), "set_grain_size", "get_grain_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streak_time", PROPERTY_HINT_RANGE, "0,0.1,0.001,suffix:s"), "set_streak_time", "get_streak_time");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "grain_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_grain_color", "get_grain_color");
}

GroundStreamers3D::GroundStreamers3D() {
	shader.instantiate();
	shader->set_code(STREAMER_SHADER);
	process.instantiate();
	process->set_shader(shader);
	// Lit, opaque mineral grains with a round edge; alpha carries how much of the pixel they cover.
	grain_material.instantiate();
	grain_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	grain_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	grain_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	grain_material->set_albedo(grain_color);
	grain_material->set_roughness(1.0f);
	grain_material->set_diffuse_mode(BaseMaterial3D::DIFFUSE_LAMBERT_WRAP);
	Ref<Gradient> g;
	g.instantiate();
	g->set_color(0, Color(1, 1, 1, 1));
	g->set_color(1, Color(1, 1, 1, 0));
	g->set_offset(0, 0.0f);
	g->set_offset(1, 1.0f);
	Ref<GradientTexture2D> t;
	t.instantiate();
	t->set_gradient(g);
	t->set_fill(GradientTexture2D::FILL_RADIAL);
	t->set_fill_from(Vector2(0.5, 0.5));
	t->set_fill_to(Vector2(1.0, 0.5));
	t->set_width(32);
	t->set_height(32);
	grain_material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, t);
}

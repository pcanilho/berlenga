/**************************************************************************/
/*  street_water_3d.cpp                                                    */
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


#include "street_water_3d.h"

#include "road_relief_3d.h"
#include "street_water.glsl.gen.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

namespace {
enum Mode {
	MODE_OCCUPY = 0,
	MODE_FLUX = 1,
	MODE_DEPTH = 2,
	MODE_PACK = 3,
	MODE_LOAD = 4,
};
constexpr float WALL_BED = 60.0f; // over the shader's WALL (50): no road
constexpr int PUSH_BYTES = 80;
constexpr int RECORD_BYTES = PUSH_BYTES + 8; // push constant, then the dispatch's groups in x and y

struct Push {
	int32_t a[4] = {};
	int32_t b[4] = {};
	int32_t c[4] = {};
	float d[4] = {};
	float e[4] = {};
};
static_assert(sizeof(Push) == PUSH_BYTES);

int imod(int p_v, int p_n) {
	const int m = p_v % p_n;
	return m < 0 ? m + p_n : m;
}

void add_record(PackedByteArray &r_records, const Push &p_push, int p_gx, int p_gy) {
	const int at = r_records.size();
	r_records.resize(at + RECORD_BYTES);
	uint8_t *w = r_records.ptrw() + at;
	memcpy(w, &p_push, PUSH_BYTES);
	const int32_t g[2] = { p_gx, p_gy };
	memcpy(w + PUSH_BYTES, g, 8);
}
} // namespace

StreetWater3D::StreetWater3D() {
	map.instantiate();
	set_notify_transform(false);
}

StreetWater3D::~StreetWater3D() {
}

void StreetWater3D::set_resolution(int p_v) {
	const int v = CLAMP((p_v + 7) / 8 * 8, 64, 2048);
	if (v != resolution) {
		resolution = v;
		placed = false;
	}
}

void StreetWater3D::set_cell(float p_v) {
	const float v = CLAMP(p_v, 0.005f, 1.0f);
	if (v != cell) {
		cell = v;
		placed = false;
	}
}

void StreetWater3D::set_height_sources(const TypedArray<NodePath> &p_v) {
	height_sources = p_v;
	placed = false;
}

void StreetWater3D::reset() {
	placed = false;
}

void StreetWater3D::submit_sphere(int64_t p_owner, const Vector3 &p_world_position, float p_radius) {
	if (p_radius <= 0.0f) {
		spheres.erase(p_owner);
		return;
	}
	// Kept in water space (frame_path's), so a body's velocity is its speed over the street it runs on.
	const Vector3 at = _frame_xform().affine_inverse().xform(p_world_position);
	Body *b = spheres.getptr(p_owner);
	if (!b) {
		b = &spheres.insert(p_owner, Body())->value;
		b->time = clock;
	} else if (clock > b->time) {
		// The body's velocity from its last place (on the water's own clock), smoothed a little against uneven frames.
		const Vector3 v = (at - b->at) / (clock - b->time);
		b->velocity = b->velocity.lerp(v, 0.5f);
		b->time = clock;
	}
	b->at = at;
	b->radius = p_radius;
}

void StreetWater3D::clear_sphere(int64_t p_owner) {
	spheres.erase(p_owner);
}

void StreetWater3D::add_water(const Vector3 &p_world_position, float p_volume, float p_radius) {
	if (p_volume <= 0.0f || int(pours.size()) >= MAX_POURS) {
		return;
	}
	Pour p;
	p.at = _frame_xform().affine_inverse().xform(p_world_position);
	// At least a cell across, so a drip is never lost between cell centres.
	p.radius = MAX(p_radius, cell * 0.75f);
	p.volume = p_volume;
	pours.push_back(p);
}

Vector4 StreetWater3D::get_water_window() const {
	return Vector4(float(origin.x * real_t(cell)), float(origin.y * real_t(cell)), resolution * cell, cell);
}

Transform3D StreetWater3D::_frame_xform() const {
	if (!frame_path.is_empty() && is_inside_tree()) {
		if (const Node3D *f = Object::cast_to<Node3D>(get_node_or_null(frame_path))) {
			return f->get_global_transform();
		}
	}
	return Transform3D();
}

float StreetWater3D::get_depth_at(const Vector3 &p_world_position) const {
	MutexLock lock(readback_mutex);
	const int n = rb_n;
	if (n <= 0 || rb_data.size() < n * n * 8) {
		return 0.0f;
	}
	const Vector3 l = _frame_xform().affine_inverse().xform(p_world_position);
	const int cx = int(Math::floor(l.x / rb_cell));
	const int cz = int(Math::floor(l.z / rb_cell));
	if (cx - rb_origin.x < 0 || cz - rb_origin.y < 0 || cx - rb_origin.x >= n || cz - rb_origin.y >= n) {
		return 0.0f;
	}
	const int t = imod(cz, n) * n + imod(cx, n);
	uint16_t h;
	memcpy(&h, rb_data.ptr() + t * 8, 2);
	return MAX(Math::half_to_float(h), 0.0f);
}

void StreetWater3D::_rt_on_readback(const PackedByteArray &p_data, int p_n, Vector2i p_origin, float p_cell) {
	MutexLock lock(readback_mutex);
	rb_data = p_data;
	rb_n = p_n;
	rb_origin = p_origin;
	rb_cell = p_cell;
}

Vector3 StreetWater3D::_follow_point() const {
	if (!follow_path.is_empty()) {
		if (const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_path))) {
			return n->get_global_position();
		}
	}
	if (const Camera3D *cam = get_viewport() ? get_viewport()->get_camera_3d() : nullptr) {
		// The window centred a little ahead of the camera, on the ground it looks at (an isometric or high camera
		// sees the street well in front of itself).
		const Vector3 eye = cam->get_global_position();
		const Vector3 fwd = -cam->get_global_transform().basis.get_column(2);
		const real_t to_ground = fwd.y < -0.05f ? MIN((eye.y - get_global_position().y) / -fwd.y, real_t(resolution * cell)) : real_t(0.0);
		return eye + fwd * to_ground * 0.9f;
	}
	return get_global_position();
}

float StreetWater3D::_bed_at(const Vector3 &p_world, const LocalVector<Node *> &p_sources, float &r_pond, bool &r_inlet) const {
	float best = -Math::INF;
	r_pond = 0.0f;
	r_inlet = false;
	for (Node *n : p_sources) {
		float h;
		const RoadRelief3D *road = Object::cast_to<RoadRelief3D>(n);
		if (road) {
			h = road->get_surface_height_at(p_world);
		} else {
			h = n->call(SNAME("get_height_at"), p_world);
		}
		if (Math::is_finite(h) && h > best) {
			best = h;
			if (road) {
				r_pond = road->get_pond_depth_at(p_world);
				r_inlet = road->is_inlet_at(p_world);
			} else {
				r_pond = 0.0f;
				r_inlet = false;
			}
		}
	}
	return best;
}

void StreetWater3D::_frame(double p_delta) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!simulating || !rs || !rs->get_rendering_device() || rt_failed) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int n = resolution;
	const int groups = (n + 7) / 8;

	LocalVector<Node *> sources;
	for (int i = 0; i < height_sources.size(); i++) {
		if (Node *s = get_node_or_null(height_sources[i])) {
			sources.push_back(s);
		}
	}
	// Water space: frame_path's (a street sliding past keeps its water), else the world's. Window cells are whole cells
	// of it; beds and bodies come into it from the world.
	const Transform3D fx = _frame_xform();
	const Transform3D fx_inv = fx.affine_inverse();
	const Vector3 center_world = _follow_point();
	const Vector3 center = fx_inv.xform(center_world);
	const Vector2i want(int(Math::floor(center.x / cell)) - n / 2, int(Math::floor(center.z / cell)) - n / 2);
	struct Rect {
		int x0, z0, w, h;
	};
	LocalVector<Rect> rects;
	if (!placed || Math::abs(want.x - origin.x) >= n || Math::abs(want.y - origin.y) >= n) {
		ref_y = center.y;
		rects.push_back({ 0, 0, n, n });
	} else {
		const int dx = want.x - origin.x;
		const int dz = want.y - origin.y;
		if (dx > 0) {
			rects.push_back({ n - dx, 0, dx, n });
		} else if (dx < 0) {
			rects.push_back({ 0, 0, -dx, n });
		}
		if (dz > 0) {
			rects.push_back({ 0, n - dz, n, dz });
		} else if (dz < 0) {
			rects.push_back({ 0, 0, n, -dz });
		}
	}
	placed = true;
	origin = want;
	const int omx = imod(origin.x, n);
	const int omz = imod(origin.y, n);
	const real_t corner_x = origin.x * real_t(cell);
	const real_t corner_z = origin.y * real_t(cell);

	PackedByteArray records;
	PackedFloat32Array loads;
	for (const Rect &r : rects) {
		const int off = loads.size();
		loads.resize(off + 2 * r.w * r.h);
		float *w = loads.ptrw() + off;
		for (int z = 0; z < r.h; z++) {
			for (int x = 0; x < r.w; x++) {
				const Vector3 l(corner_x + (r.x0 + x + 0.5f) * cell, center.y, corner_z + (r.z0 + z + 0.5f) * cell);
				const Vector3 p = fx.xform(l);
				float pond;
				bool inlet;
				const float h = _bed_at(p, sources, pond, inlet);
				const int k = 2 * (z * r.w + x);
				if (Math::is_finite(h)) {
					w[k] = float(fx_inv.xform(Vector3(p.x, h, p.z)).y - ref_y);
					// (the flood's last tenth of a millimetre over flat ground is no puddle)
					w[k + 1] = inlet ? -1.0f : (pond > 1e-4f ? prefill * pond : 0.0f);
				} else {
					w[k] = WALL_BED;
					w[k + 1] = 0.0f;
				}
			}
		}
		Push p;
		p.a[0] = MODE_LOAD;
		p.a[1] = n;
		p.a[2] = omx;
		p.a[3] = omz;
		p.b[0] = r.x0;
		p.b[1] = r.z0;
		p.b[2] = r.w;
		p.b[3] = r.h;
		p.c[2] = off;
		add_record(records, p, (r.w + 7) / 8, (r.h + 7) / 8);
	}

	// Bodies and pours, from the window's corner and reference height.
	PackedFloat32Array items;
	int nb = 0;
	for (const KeyValue<int64_t, Body> &kv : spheres) {
		if (nb >= MAX_BODIES) {
			break;
		}
		const Body &b = kv.value;
		items.push_back(float(b.at.x - corner_x));
		items.push_back(float(b.at.y - ref_y));
		items.push_back(float(b.at.z - corner_z));
		items.push_back(b.radius);
		items.push_back(b.velocity.x);
		items.push_back(b.velocity.y);
		items.push_back(b.velocity.z);
		items.push_back(0.0f);
		nb++;
	}
	const int np = MIN(int(pours.size()), MAX_POURS);
	for (int i = 0; i < np; i++) {
		items.push_back(float(pours[i].at.x - corner_x));
		items.push_back(float(pours[i].at.z - corner_z));
		items.push_back(pours[i].radius);
		items.push_back(pours[i].volume);
	}

	// Steps at a fixed rate; a long frame does not run the water on (at most max_steps).
	time_left += p_delta;
	clock += p_delta;
	const double dt = 1.0 / step_rate;
	int steps = MIN(int(time_left / dt), max_steps);
	time_left = steps < max_steps ? time_left - steps * dt : 0.0;
	if (np > 0 && steps == 0) {
		steps = 1; // the pours land now
	}
	float rain = rain_rate;
	if (rain_from_weather && is_inside_tree()) {
		if (const WorldWeather *weather = WorldContracts::weather(); weather->has_provider()) {
			rain += weather->rainfall_at(center_world);
		}
	}
	last_rain = rain;
	pours.clear();
	for (int s = 0; s < steps; s++) {
		Push p;
		p.a[1] = n;
		p.a[2] = omx;
		p.a[3] = omz;
		p.c[0] = nb;
		p.c[1] = s == 0 ? np : 0;
		p.d[0] = float(dt);
		p.d[1] = cell;
		p.d[2] = manning;
		p.d[3] = float(rain / 3.6e6 * dt);
		p.e[0] = float(soak_rate / 3.6e6 * dt);
		p.e[1] = float(evaporation_rate / 3.6e6 * dt);
		p.e[2] = float(Math::exp(-dt / drying_time));
		for (int mode : { MODE_OCCUPY, MODE_FLUX, MODE_DEPTH }) {
			p.a[0] = mode;
			add_record(records, p, groups, groups);
		}
	}
	{
		Push p;
		p.a[0] = MODE_PACK;
		p.a[1] = n;
		p.a[2] = omx;
		p.a[3] = omz;
		p.d[1] = cell;
		add_record(records, p, groups, groups);
	}
	// Read the map back now and then for get_depth_at() (footsteps, splashes): a frame or two late, asynchronously.
	const bool readback = readback_rate > 0.0f && clock - last_readback >= 1.0 / readback_rate;
	if (readback) {
		last_readback = clock;
	}
	rs->call_on_render_thread(callable_mp(this, &StreetWater3D::_rt_frame).bind(n, records, items, loads, readback ? 1 : 0, origin, cell));

	if (map->get_texture_rd_rid() != tex_map && tex_map.is_valid()) {
		map->set_texture_rd_rid(tex_map);
	}
	const Vector4 window = get_water_window();
	for (int i = 0; i < materials.size(); i++) {
		Ref<ShaderMaterial> m = materials[i];
		if (m.is_valid()) {
			m->set_shader_parameter(SNAME("street_water_map"), map);
			m->set_shader_parameter(SNAME("street_water_window"), window);
			m->set_shader_parameter(SNAME("street_water_frame"), fx_inv);
		}
	}
	cost_ms = float(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
}

// Render thread.
void StreetWater3D::_rt_frame(int p_n, const PackedByteArray &p_records, const PackedFloat32Array &p_items, const PackedFloat32Array &p_loads, int p_readback, Vector2i p_origin, float p_cell) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (!rd || rt_failed) {
		return;
	}
	if (shader.is_null()) {
		Ref<RDShaderFile> sf;
		sf.instantiate();
		if (sf->parse_versions_from_text(street_water_shader_glsl) != OK) {
			ERR_PRINT("StreetWater3D: the compute shader failed to compile.");
			rt_failed = true;
			return;
		}
		shader = rd->shader_create_from_spirv(sf->get_spirv_stages());
		if (shader.is_null()) {
			ERR_PRINT("StreetWater3D: the compute shader failed to build.");
			rt_failed = true;
			return;
		}
		pipeline = rd->compute_pipeline_create(shader);
		buf_items = rd->storage_buffer_create((2 * MAX_BODIES + MAX_POURS) * 16);
	}
	bool remake = false;
	if (p_n != rt_n) {
		for (RID *r : { &buf_state, &buf_flux, &tex_map }) {
			if (r->is_valid()) {
				rd->free_rid(*r);
				*r = RID();
			}
		}
		buf_state = rd->storage_buffer_create(p_n * p_n * 16);
		buf_flux = rd->storage_buffer_create(p_n * p_n * 16);
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = p_n;
		tf.height = p_n;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		tex_map = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(tex_map, Color(0, 0, 0, 0), 0, 1, 0, 1);
		rt_n = p_n;
		remake = true;
	}
	const int load_bytes = MAX(p_loads.size(), 4) * 4;
	if (load_bytes > rt_load_bytes) {
		if (buf_load.is_valid()) {
			rd->free_rid(buf_load);
		}
		rt_load_bytes = MAX(load_bytes, rt_load_bytes * 2);
		buf_load = rd->storage_buffer_create(rt_load_bytes);
		remake = true;
	}
	if (p_loads.size() > 0) {
		rd->buffer_update(buf_load, 0, p_loads.size() * 4, p_loads.ptr());
	}
	if (p_items.size() > 0) {
		rd->buffer_update(buf_items, 0, MIN(p_items.size(), (2 * MAX_BODIES + MAX_POURS) * 4) * 4, p_items.ptr());
	}
	if (remake || uset.is_null() || !rd->uniform_set_is_valid(uset)) {
		Vector<RD::Uniform> uniforms;
		auto buffer = [&](int p_binding, RID p_rid, RD::UniformType p_type) {
			RD::Uniform u;
			u.uniform_type = p_type;
			u.binding = p_binding;
			u.append_id(p_rid);
			uniforms.push_back(u);
		};
		buffer(0, buf_state, RD::UNIFORM_TYPE_STORAGE_BUFFER);
		buffer(1, buf_flux, RD::UNIFORM_TYPE_STORAGE_BUFFER);
		buffer(2, tex_map, RD::UNIFORM_TYPE_IMAGE);
		buffer(3, buf_items, RD::UNIFORM_TYPE_STORAGE_BUFFER);
		buffer(4, buf_load, RD::UNIFORM_TYPE_STORAGE_BUFFER);
		uset = rd->uniform_set_create(uniforms, shader, 0);
	}
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline);
	rd->compute_list_bind_uniform_set(cl, uset, 0);
	const int count = p_records.size() / RECORD_BYTES;
	for (int i = 0; i < count; i++) {
		const uint8_t *r = p_records.ptr() + i * RECORD_BYTES;
		int32_t g[2];
		memcpy(g, r + PUSH_BYTES, 8);
		rd->compute_list_set_push_constant(cl, r, PUSH_BYTES);
		rd->compute_list_dispatch(cl, g[0], g[1], 1);
		rd->compute_list_add_barrier(cl);
	}
	rd->compute_list_end();
	if (p_readback) {
		rd->texture_get_data_async(tex_map, 0, callable_mp(this, &StreetWater3D::_rt_on_readback).bind(p_n, p_origin, p_cell));
	}
}

void StreetWater3D::_rt_free() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (!rd) {
		return;
	}
	for (RID *r : { &buf_state, &buf_flux, &tex_map, &buf_items, &buf_load }) {
		if (r->is_valid()) {
			rd->free_rid(*r);
			*r = RID();
		}
	}
	if (shader.is_valid()) {
		rd->free_rid(shader); // the pipeline and uniform sets with it
		shader = RID();
		pipeline = RID();
	}
	uset = RID();
	rt_n = 0;
	rt_load_bytes = 0;
}

void StreetWater3D::_release() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (map.is_valid()) {
		map->set_texture_rd_rid(RID());
	}
	if (rs && rs->get_rendering_device()) {
		rs->call_on_render_thread(callable_mp(this, &StreetWater3D::_rt_free));
		rs->sync();
	}
	placed = false;
	MutexLock lock(readback_mutex);
	rb_n = 0;
	rb_data.clear();
}

void StreetWater3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			placed = false;
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_frame(get_process_delta_time());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
			_release();
		} break;
	}
}

String StreetWater3D::get_shader_include() {
	return R"(// StreetWater3D's map (docs/ideas/street-water.md): set street_water_map and street_water_window from
// StreetWater3D.get_map() and get_water_window() (or list the material in its materials).
uniform sampler2D street_water_map : filter_linear, repeat_enable;
uniform vec4 street_water_window = vec4(0.0, 0.0, 0.0, 1.0);
// World to water space (StreetWater3D.frame_path's frame; identity without one).
uniform mat4 street_water_frame = mat4(1.0);

// The water at a world point: x depth (m), y z the water surface's slope along water space's x and z, w damp (0..1, where
// water stood or rain falls). Zero outside the window, faded over its last cells.
vec4 street_water_sample(vec3 world) {
	if (street_water_window.z <= 0.0) {
		return vec4(0.0);
	}
	vec2 wp = (street_water_frame * vec4(world, 1.0)).xz;
	vec2 rel = (wp - street_water_window.xy) / street_water_window.z;
	float edge = min(min(rel.x, rel.y), min(1.0 - rel.x, 1.0 - rel.y));
	if (edge <= 0.0) {
		return vec4(0.0);
	}
	// The window is a ring buffer: world cell c is texel c mod N, so the world position over the window's size is
	// the UV, repeated.
	vec4 w = texture(street_water_map, wp / street_water_window.z);
	return w * smoothstep(0.0, 0.04, edge);
}

// Wet asphalt and standing water as a surface layer (Lagarde, "Water drop 2b/3", 2013): wetness darkens the porous
// albedo and smooths the surface; standing water takes over the normal and the roughness. Call from fragment() with
// the world position, VIEW_MATRIX and the material's own values; it returns the puddle's cover (0..1).
float street_water_apply(vec3 world, mat4 view_matrix, inout vec3 albedo, inout float roughness, inout vec3 normal) {
	vec4 w = street_water_sample(world);
	// Damp: the pores hold water, the surface stays rough (darker, a little smoother). A film over it: a sheen.
	// Standing water: a mirror with the water's own normal.
	float damp = w.w;
	float film = smoothstep(0.0, 0.0003, w.x);
	float puddle = smoothstep(0.0003, 0.0015, w.x);
	float wet = max(damp, film);
	albedo *= mix(1.0, 0.6, wet);
	roughness = mix(roughness, roughness * 0.65, damp);
	roughness = mix(roughness, min(roughness, 0.15), film);
	albedo *= mix(1.0, 0.85, puddle);
	roughness = mix(roughness, 0.03, puddle);
	// The slope is in water space: back to the world (the frame's rotation is the inverse's transpose).
	vec3 wn = normalize(transpose(mat3(street_water_frame)) * vec3(-w.y, 1.0, -w.z));
	normal = normalize(mix(normal, (view_matrix * vec4(wn, 0.0)).xyz, puddle));
	return puddle;
}
)";
}

void StreetWater3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &StreetWater3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &StreetWater3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_cell", "cell"), &StreetWater3D::set_cell);
	ClassDB::bind_method(D_METHOD("get_cell"), &StreetWater3D::get_cell);
	ClassDB::bind_method(D_METHOD("set_frame_path", "path"), &StreetWater3D::set_frame_path);
	ClassDB::bind_method(D_METHOD("get_frame_path"), &StreetWater3D::get_frame_path);
	ClassDB::bind_method(D_METHOD("set_prefill", "fill"), &StreetWater3D::set_prefill);
	ClassDB::bind_method(D_METHOD("get_prefill"), &StreetWater3D::get_prefill);
	ClassDB::bind_method(D_METHOD("set_readback_rate", "hz"), &StreetWater3D::set_readback_rate);
	ClassDB::bind_method(D_METHOD("get_readback_rate"), &StreetWater3D::get_readback_rate);
	ClassDB::bind_method(D_METHOD("get_depth_at", "world_position"), &StreetWater3D::get_depth_at);
	ClassDB::bind_method(D_METHOD("set_follow_path", "path"), &StreetWater3D::set_follow_path);
	ClassDB::bind_method(D_METHOD("get_follow_path"), &StreetWater3D::get_follow_path);
	ClassDB::bind_method(D_METHOD("set_height_sources", "sources"), &StreetWater3D::set_height_sources);
	ClassDB::bind_method(D_METHOD("get_height_sources"), &StreetWater3D::get_height_sources);
	ClassDB::bind_method(D_METHOD("set_rain_rate", "rate"), &StreetWater3D::set_rain_rate);
	ClassDB::bind_method(D_METHOD("get_rain_rate"), &StreetWater3D::get_rain_rate);
	ClassDB::bind_method(D_METHOD("set_rain_from_weather", "enabled"), &StreetWater3D::set_rain_from_weather);
	ClassDB::bind_method(D_METHOD("get_rain_from_weather"), &StreetWater3D::get_rain_from_weather);
	ClassDB::bind_method(D_METHOD("set_soak_rate", "rate"), &StreetWater3D::set_soak_rate);
	ClassDB::bind_method(D_METHOD("get_soak_rate"), &StreetWater3D::get_soak_rate);
	ClassDB::bind_method(D_METHOD("set_evaporation_rate", "rate"), &StreetWater3D::set_evaporation_rate);
	ClassDB::bind_method(D_METHOD("get_evaporation_rate"), &StreetWater3D::get_evaporation_rate);
	ClassDB::bind_method(D_METHOD("set_drying_time", "seconds"), &StreetWater3D::set_drying_time);
	ClassDB::bind_method(D_METHOD("get_drying_time"), &StreetWater3D::get_drying_time);
	ClassDB::bind_method(D_METHOD("set_manning", "n"), &StreetWater3D::set_manning);
	ClassDB::bind_method(D_METHOD("get_manning"), &StreetWater3D::get_manning);
	ClassDB::bind_method(D_METHOD("set_step_rate", "hz"), &StreetWater3D::set_step_rate);
	ClassDB::bind_method(D_METHOD("get_step_rate"), &StreetWater3D::get_step_rate);
	ClassDB::bind_method(D_METHOD("set_max_steps", "steps"), &StreetWater3D::set_max_steps);
	ClassDB::bind_method(D_METHOD("get_max_steps"), &StreetWater3D::get_max_steps);
	ClassDB::bind_method(D_METHOD("set_simulating", "enabled"), &StreetWater3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &StreetWater3D::is_simulating);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &StreetWater3D::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &StreetWater3D::get_materials);
	ClassDB::bind_method(D_METHOD("submit_sphere", "owner", "world_position", "radius"), &StreetWater3D::submit_sphere);
	ClassDB::bind_method(D_METHOD("clear_sphere", "owner"), &StreetWater3D::clear_sphere);
	ClassDB::bind_method(D_METHOD("add_water", "world_position", "volume", "radius"), &StreetWater3D::add_water, DEFVAL(0.05f));
	ClassDB::bind_method(D_METHOD("reset"), &StreetWater3D::reset);
	ClassDB::bind_method(D_METHOD("get_map"), &StreetWater3D::get_map);
	ClassDB::bind_method(D_METHOD("get_water_window"), &StreetWater3D::get_water_window);
	ClassDB::bind_method(D_METHOD("get_cost_ms"), &StreetWater3D::get_cost_ms);
	ClassDB::bind_static_method("StreetWater3D", D_METHOD("get_shader_include"), &StreetWater3D::get_shader_include);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "64,2048,8"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell", PROPERTY_HINT_RANGE, "0.005,1,0.001,suffix:m"), "set_cell", "get_cell");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_path", "get_follow_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "frame_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_frame_path", "get_frame_path");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "height_sources", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_height_sources", "get_height_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prefill", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_prefill", "get_prefill");
	ADD_GROUP("Water", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_rate", PROPERTY_HINT_RANGE, "0,200,0.1,suffix:mm/h"), "set_rain_rate", "get_rain_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_from_weather"), "set_rain_from_weather", "get_rain_from_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soak_rate", PROPERTY_HINT_RANGE, "0,500,0.1,suffix:mm/h"), "set_soak_rate", "get_soak_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "evaporation_rate", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:mm/h"), "set_evaporation_rate", "get_evaporation_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drying_time", PROPERTY_HINT_RANGE, "1,7200,1,suffix:s"), "set_drying_time", "get_drying_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "manning", PROPERTY_HINT_RANGE, "0.005,0.1,0.001"), "set_manning", "get_manning");
	ADD_GROUP("Solver", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_rate", PROPERTY_HINT_RANGE, "30,480,1,suffix:Hz"), "set_step_rate", "get_step_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_steps", PROPERTY_HINT_RANGE, "1,16,1"), "set_max_steps", "get_max_steps");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "readback_rate", PROPERTY_HINT_RANGE, "0,60,0.5,suffix:Hz"), "set_readback_rate", "get_readback_rate");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, "ShaderMaterial"), "set_materials", "get_materials");
}

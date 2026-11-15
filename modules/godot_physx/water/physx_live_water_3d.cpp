/**************************************************************************/
/*  physx_live_water_3d.cpp                                               */
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

#include "physx_live_water_3d.h"

#include "physx_river_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/shader.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/modules_enabled.gen.h" // For weather.

#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

namespace {
constexpr float GRAVITY = 9.81f;
constexpr float WET = 0.002f; // m: thinner than this counts as dry
constexpr float NO_GROUND = -1e9f; // a cell the ray found nothing under: a sink
constexpr int MAX_CELLS = 1024 * 1024;
constexpr int MAX_SUBSTEPS = 16;
} // namespace

void PhysXLiveWater3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PhysXLiveWater3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PhysXLiveWater3D::get_size);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &PhysXLiveWater3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &PhysXLiveWater3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &PhysXLiveWater3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &PhysXLiveWater3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_simulating", "enabled"), &PhysXLiveWater3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &PhysXLiveWater3D::is_simulating);
	ClassDB::bind_method(D_METHOD("set_fill_enabled", "enabled"), &PhysXLiveWater3D::set_fill_enabled);
	ClassDB::bind_method(D_METHOD("get_fill_enabled"), &PhysXLiveWater3D::get_fill_enabled);
	ClassDB::bind_method(D_METHOD("set_fill_height", "height"), &PhysXLiveWater3D::set_fill_height);
	ClassDB::bind_method(D_METHOD("get_fill_height"), &PhysXLiveWater3D::get_fill_height);
	ClassDB::bind_method(D_METHOD("set_fill_rect", "rect"), &PhysXLiveWater3D::set_fill_rect);
	ClassDB::bind_method(D_METHOD("get_fill_rect"), &PhysXLiveWater3D::get_fill_rect);
	ClassDB::bind_method(D_METHOD("set_sources", "sources"), &PhysXLiveWater3D::set_sources);
	ClassDB::bind_method(D_METHOD("get_sources"), &PhysXLiveWater3D::get_sources);
	ClassDB::bind_method(D_METHOD("set_source_radius", "radius"), &PhysXLiveWater3D::set_source_radius);
	ClassDB::bind_method(D_METHOD("get_source_radius"), &PhysXLiveWater3D::get_source_radius);
	ClassDB::bind_method(D_METHOD("set_rain_rate", "rate"), &PhysXLiveWater3D::set_rain_rate);
	ClassDB::bind_method(D_METHOD("get_rain_rate"), &PhysXLiveWater3D::get_rain_rate);
	ClassDB::bind_method(D_METHOD("set_rain_from_weather", "enabled"), &PhysXLiveWater3D::set_rain_from_weather);
	ClassDB::bind_method(D_METHOD("get_rain_from_weather"), &PhysXLiveWater3D::get_rain_from_weather);
	ClassDB::bind_method(D_METHOD("set_infiltration_rate", "rate"), &PhysXLiveWater3D::set_infiltration_rate);
	ClassDB::bind_method(D_METHOD("get_infiltration_rate"), &PhysXLiveWater3D::get_infiltration_rate);
	ClassDB::bind_method(D_METHOD("set_open_edges", "open"), &PhysXLiveWater3D::set_open_edges);
	ClassDB::bind_method(D_METHOD("get_open_edges"), &PhysXLiveWater3D::get_open_edges);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &PhysXLiveWater3D::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &PhysXLiveWater3D::get_friction);
	ClassDB::bind_method(D_METHOD("set_foam_speed", "speed"), &PhysXLiveWater3D::set_foam_speed);
	ClassDB::bind_method(D_METHOD("get_foam_speed"), &PhysXLiveWater3D::get_foam_speed);
	ClassDB::bind_method(D_METHOD("set_raster_reflections", "enabled"), &PhysXLiveWater3D::set_raster_reflections);
	ClassDB::bind_method(D_METHOD("get_raster_reflections"), &PhysXLiveWater3D::get_raster_reflections);
	ClassDB::bind_method(D_METHOD("reset"), &PhysXLiveWater3D::reset);
	ClassDB::bind_method(D_METHOD("update_ground"), &PhysXLiveWater3D::update_ground);
	ClassDB::bind_method(D_METHOD("add_water", "position", "volume", "radius"), &PhysXLiveWater3D::add_water, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("sample_depth", "position"), &PhysXLiveWater3D::sample_depth);
	ClassDB::bind_method(D_METHOD("sample_height", "position"), &PhysXLiveWater3D::sample_height);
	ClassDB::bind_method(D_METHOD("sample_flow", "position"), &PhysXLiveWater3D::sample_flow);
	ClassDB::bind_method(D_METHOD("get_total_volume"), &PhysXLiveWater3D::get_total_volume);
	ClassDB::bind_method(D_METHOD("get_volume_out"), &PhysXLiveWater3D::get_volume_out);
	ClassDB::bind_method(D_METHOD("get_cost_ms"), &PhysXLiveWater3D::get_cost_ms);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXLiveWater3D::get_water_material);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.1,4,0.01,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_GROUP("Fill", "fill_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fill_enabled"), "set_fill_enabled", "get_fill_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fill_height", PROPERTY_HINT_NONE, "suffix:m"), "set_fill_height", "get_fill_height");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "fill_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_fill_rect", "get_fill_rect");
	ADD_GROUP("Water In and Out", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "sources"), "set_sources", "get_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "source_radius", PROPERTY_HINT_RANGE, "0.05,20,0.01,suffix:m"), "set_source_radius", "get_source_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_rate", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:mm/h"), "set_rain_rate", "get_rain_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_from_weather"), "set_rain_from_weather", "get_rain_from_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "infiltration_rate", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:mm/h"), "set_infiltration_rate", "get_infiltration_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "open_edges"), "set_open_edges", "get_open_edges");
	ADD_GROUP("Flow", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:1/s"), "set_friction", "get_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foam_speed", PROPERTY_HINT_RANGE, "0.1,10,0.01,suffix:m/s"), "set_foam_speed", "get_foam_speed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "raster_reflections"), "set_raster_reflections", "get_raster_reflections");
}

PhysXLiveWater3D::PhysXLiveWater3D() {
	set_notify_transform(true);
}

PhysXLiveWater3D::~PhysXLiveWater3D() {
}

void PhysXLiveWater3D::set_size(Vector2 p_size) {
	size = Vector2(MAX(p_size.x, 1.0f), MAX(p_size.y, 1.0f));
	reset();
}

void PhysXLiveWater3D::set_cell_size(float p_size) {
	cell_size = CLAMP(p_size, 0.1f, 4.0f);
	reset();
}

void PhysXLiveWater3D::set_ground_collision_mask(uint32_t p_mask) {
	ground_collision_mask = p_mask;
	reset();
}

void PhysXLiveWater3D::set_fill_enabled(bool p_enabled) {
	fill_enabled = p_enabled;
	reset();
}

void PhysXLiveWater3D::set_fill_height(float p_height) {
	fill_height = p_height;
	if (fill_enabled) {
		reset();
	}
}

void PhysXLiveWater3D::set_fill_rect(const Rect2 &p_rect) {
	fill_rect = p_rect;
	if (fill_enabled) {
		reset();
	}
}

void PhysXLiveWater3D::reset() {
	built = false;
	time_left = 0.0;
	volume_out = 0.0f;
}

void PhysXLiveWater3D::_allocate() {
	nx = MAX(2, (int)Math::ceil(size.x / cell_size));
	nz = MAX(2, (int)Math::ceil(size.y / cell_size));
	if (nx * nz > MAX_CELLS) {
		const float k = Math::sqrt((float)MAX_CELLS / (nx * nz));
		nx = MAX(2, (int)(nx * k));
		nz = MAX(2, (int)(nz * k));
		WARN_PRINT(vformat("PhysXLiveWater3D: %d x %d cells at most; size / cell_size is clamped.", nx, nz));
	}
	const int n = nx * nz;
	ground.resize(n);
	depth.resize(n);
	flux.resize(n * 4);
	vel.resize(n * 2);
	foam.resize(n);
	for (int i = 0; i < n; i++) {
		ground[i] = 0.0f;
		depth[i] = 0.0f;
		foam[i] = 0.0f;
	}
	for (uint32_t i = 0; i < flux.size(); i++) {
		flux[i] = 0.0f;
	}
	for (uint32_t i = 0; i < vel.size(); i++) {
		vel[i] = 0.0f;
	}
}

// Cell centres in local space: x from -size.x / 2, z from -size.y / 2.
void PhysXLiveWater3D::_measure_ground(PhysicsDirectSpaceState3D *p_space, bool p_keep_water) {
	const Transform3D xf = get_global_transform();
	const Transform3D inv = xf.affine_inverse();
	const float sx = nx * cell_size;
	const float sz = nz * cell_size;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector3 local((x + 0.5f) * cell_size - 0.5f * sx, 0.0f, (z + 0.5f) * cell_size - 0.5f * sz);
			PhysicsDirectSpaceState3D::RayParameters ray;
			ray.from = xf.xform(local + Vector3(0, 500.0f, 0));
			ray.to = xf.xform(local - Vector3(0, 500.0f, 0));
			ray.collision_mask = ground_collision_mask;
			ray.collide_with_areas = false;
			float g = NO_GROUND;
			// Only what stands still is ground: floating and moving bodies are skipped.
			for (int skip = 0; skip < 8; skip++) {
				PhysicsDirectSpaceState3D::RayResult hit;
				if (!p_space->intersect_ray(ray, hit)) {
					break;
				}
				if (ps->body_get_mode(hit.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
					g = inv.xform(hit.position).y;
					break;
				}
				ray.exclude.insert(hit.rid);
			}
			const int i = z * nx + x;
			if (p_keep_water) {
				// The water's surface stays where it was; ground that rose takes its place.
				const float top = ground[i] > NO_GROUND ? ground[i] + depth[i] : g;
				ground[i] = g;
				depth[i] = g > NO_GROUND ? MAX(top - g, 0.0f) : 0.0f;
			} else {
				ground[i] = g;
				const bool in_fill = fill_rect.has_area() ? fill_rect.has_point(Vector2(local.x, local.z)) : true;
				depth[i] = (fill_enabled && in_fill && g > NO_GROUND) ? MAX(fill_height - g, 0.0f) : 0.0f;
			}
		}
	}
}

float PhysXLiveWater3D::_rain_mm_per_hour() const {
	float r = rain_rate;
#ifdef MODULE_WEATHER_ENABLED
	if (rain_from_weather) {
		const WeatherMap3D *weather = WeatherMap3D::get_active();
		if (weather != nullptr) {
			r += weather->get_rain_at(get_global_position());
		}
	}
#endif
	return r;
}

// One step of the virtual pipes (Mei, Decaudin and Hu 2007, sec. 3.1). Each cell keeps its outflow through four
// pipes to its neighbours; a pipe's flow is accelerated by the difference in surface height across it (pipe
// cross-section l^2, length l) and slowed by friction, and a cell's outflows are scaled down together when they
// would take more water than it holds this step, so depth never goes negative and volume is kept exactly. Over an
// open edge the neighbour is the cell's own ground with no water on it; a closed edge is a wall.
void PhysXLiveWater3D::_step(float p_dt) {
	const int n = nx * nz;
	const float l = cell_size;
	const float area = l * l;
	const float accel = p_dt * GRAVITY * l; // dt * A * g / l with A = l^2
	const float keep = MAX(0.0f, 1.0f - friction * p_dt);
	const int dx[4] = { 1, -1, 0, 0 };
	const int dz[4] = { 0, 0, 1, -1 };

	// Sources, rain and soaking in.
	const float rain = _rain_mm_per_hour() / 3.6e6f * p_dt; // m this step
	const float soak = infiltration_rate / 3.6e6f * p_dt;
	if (rain > 0.0f || soak > 0.0f) {
		for (int i = 0; i < n; i++) {
			if (ground[i] > NO_GROUND) {
				depth[i] = MAX(depth[i] + rain - soak, 0.0f);
			}
		}
	}
	for (int s = 0; s < sources.size(); s++) {
		const Vector4 src = sources[s];
		add_water(get_global_transform().xform(Vector3(src.x, src.y, src.z)), src.w * p_dt, source_radius);
	}

	// Outflows.
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const int i = z * nx + x;
			float *f = &flux[i * 4];
			if (ground[i] <= NO_GROUND) {
				f[0] = f[1] = f[2] = f[3] = 0.0f;
				continue;
			}
			const float h = ground[i] + depth[i];
			float total = 0.0f;
			for (int k = 0; k < 4; k++) {
				const int x2 = x + dx[k];
				const int z2 = z + dz[k];
				float h2;
				if (x2 < 0 || z2 < 0 || x2 >= nx || z2 >= nz) {
					if (!open_edges) {
						f[k] = 0.0f;
						continue;
					}
					h2 = ground[i];
				} else {
					const int j = z2 * nx + x2;
					// Into a hole in the ground (nothing under it): drains as over an open edge.
					h2 = ground[j] > NO_GROUND ? ground[j] + depth[j] : ground[i];
				}
				f[k] = MAX(0.0f, f[k] * keep + accel * (h - h2));
				total += f[k];
			}
			const float avail = depth[i] * area;
			if (total * p_dt > avail) {
				const float k = total > 0.0f ? avail / (total * p_dt) : 0.0f;
				f[0] *= k;
				f[1] *= k;
				f[2] *= k;
				f[3] *= k;
			}
		}
	}

	// Depths and velocities.
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const int i = z * nx + x;
			if (ground[i] <= NO_GROUND) {
				depth[i] = 0.0f;
				vel[i * 2] = vel[i * 2 + 1] = 0.0f;
				continue;
			}
			const float *f = &flux[i * 4];
			const float in_l = x > 0 ? flux[(i - 1) * 4 + 0] : 0.0f; // from the -x neighbour flowing +x
			const float in_r = x < nx - 1 ? flux[(i + 1) * 4 + 1] : 0.0f;
			const float in_b = z > 0 ? flux[(i - nx) * 4 + 2] : 0.0f;
			const float in_t = z < nz - 1 ? flux[(i + nx) * 4 + 3] : 0.0f;
			const float out = f[0] + f[1] + f[2] + f[3];
			// What leaves over the area's edge (or into a hole) is gone.
			float lost = 0.0f;
			if (x == nx - 1 || (x < nx - 1 && ground[i + 1] <= NO_GROUND)) {
				lost += f[0];
			}
			if (x == 0 || (x > 0 && ground[i - 1] <= NO_GROUND)) {
				lost += f[1];
			}
			if (z == nz - 1 || (z < nz - 1 && ground[i + nx] <= NO_GROUND)) {
				lost += f[2];
			}
			if (z == 0 || (z > 0 && ground[i - nx] <= NO_GROUND)) {
				lost += f[3];
			}
			volume_out += lost * p_dt;
			const float d0 = depth[i];
			const float d1 = MAX(d0 + p_dt * (in_l + in_r + in_b + in_t - out) / area, 0.0f);
			depth[i] = d1;
			// Mean flow through the cell (their eq. 7-8), over the mean depth.
			const float wx = 0.5f * (in_l - f[1] + f[0] - in_r);
			const float wz = 0.5f * (in_b - f[3] + f[2] - in_t);
			const float d_mean = 0.5f * (d0 + d1);
			if (d_mean > WET) {
				vel[i * 2] = wx / (l * d_mean);
				vel[i * 2 + 1] = wz / (l * d_mean);
			} else {
				vel[i * 2] = vel[i * 2 + 1] = 0.0f;
			}
			// White water where it runs fast and shoots (well past Froude 1), relaxing over a second or two. A sheet
			// of a few centimetres sliding over the ground stays clear: it has no depth to trap air in.
			const float speed = Math::sqrt(vel[i * 2] * vel[i * 2] + vel[i * 2 + 1] * vel[i * 2 + 1]);
			const float froude = d1 > WET ? speed / Math::sqrt(GRAVITY * d1) : 0.0f;
			const float fast = CLAMP((speed - foam_speed) / foam_speed, 0.0f, 1.0f) * CLAMP((froude - 1.5f) * 0.5f, 0.0f, 1.0f);
			const float target = 0.8f * fast * Math::smoothstep(0.03f, 0.15f, d1);
			foam[i] = MAX(target, foam[i] - 0.7f * p_dt);
		}
	}
}

bool PhysXLiveWater3D::_cell_of(const Vector3 &p_global, int &r_x, int &r_z, float &r_fx, float &r_fz) const {
	if (!built) {
		return false;
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_global);
	const float gx = (local.x + 0.5f * nx * cell_size) / cell_size - 0.5f;
	const float gz = (local.z + 0.5f * nz * cell_size) / cell_size - 0.5f;
	if (gx < -0.5f || gz < -0.5f || gx > nx - 0.5f || gz > nz - 0.5f) {
		return false;
	}
	r_x = CLAMP((int)Math::floor(gx), 0, nx - 2);
	r_z = CLAMP((int)Math::floor(gz), 0, nz - 2);
	r_fx = CLAMP(gx - r_x, 0.0f, 1.0f);
	r_fz = CLAMP(gz - r_z, 0.0f, 1.0f);
	return true;
}

void PhysXLiveWater3D::add_water(const Vector3 &p_global, float p_volume, float p_radius) {
	if (!built || p_volume <= 0.0f) {
		return;
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_global);
	const float cx = (local.x + 0.5f * nx * cell_size) / cell_size - 0.5f;
	const float cz = (local.z + 0.5f * nz * cell_size) / cell_size - 0.5f;
	const float r = MAX(p_radius / cell_size, 0.5f);
	const int x0 = MAX(0, (int)Math::floor(cx - r));
	const int x1 = MIN(nx - 1, (int)Math::ceil(cx + r));
	const int z0 = MAX(0, (int)Math::floor(cz - r));
	const int z1 = MIN(nz - 1, (int)Math::ceil(cz + r));
	int count = 0;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			if ((x - cx) * (x - cx) + (z - cz) * (z - cz) <= r * r && ground[z * nx + x] > NO_GROUND) {
				count++;
			}
		}
	}
	if (count == 0) {
		return;
	}
	const float add = p_volume / (count * cell_size * cell_size);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			if ((x - cx) * (x - cx) + (z - cz) * (z - cz) <= r * r && ground[z * nx + x] > NO_GROUND) {
				depth[z * nx + x] += add;
			}
		}
	}
}

float PhysXLiveWater3D::sample_depth(const Vector3 &p_global) const {
	int x, z;
	float fx, fz;
	if (!_cell_of(p_global, x, z, fx, fz)) {
		return 0.0f;
	}
	const int i = z * nx + x;
	return Math::lerp(Math::lerp(depth[i], depth[i + 1], fx), Math::lerp(depth[i + nx], depth[i + nx + 1], fx), fz);
}

float PhysXLiveWater3D::sample_height(const Vector3 &p_global) const {
	int x, z;
	float fx, fz;
	if (!_cell_of(p_global, x, z, fx, fz)) {
		return NAN;
	}
	const int idx[4] = { z * nx + x, z * nx + x + 1, (z + 1) * nx + x, (z + 1) * nx + x + 1 };
	float h[4];
	for (int k = 0; k < 4; k++) {
		h[k] = ground[idx[k]] > NO_GROUND ? ground[idx[k]] + depth[idx[k]] : 0.0f;
	}
	const float local_y = Math::lerp(Math::lerp(h[0], h[1], fx), Math::lerp(h[2], h[3], fx), fz);
	return get_global_transform().xform(Vector3(0, local_y, 0)).y;
}

Vector3 PhysXLiveWater3D::sample_flow(const Vector3 &p_global) const {
	int x, z;
	float fx, fz;
	if (!_cell_of(p_global, x, z, fx, fz)) {
		return Vector3();
	}
	const int i = z * nx + x;
	Vector3 v;
	for (int c = 0; c < 2; c++) {
		const float a = Math::lerp(vel[i * 2 + c], vel[(i + 1) * 2 + c], fx);
		const float b = Math::lerp(vel[(i + nx) * 2 + c], vel[(i + nx + 1) * 2 + c], fx);
		v[c == 0 ? 0 : 2] = Math::lerp(a, b, fz);
	}
	return get_global_transform().basis.xform(v);
}

float PhysXLiveWater3D::get_total_volume() const {
	double v = 0.0;
	for (uint32_t i = 0; i < depth.size(); i++) {
		v += depth[i];
	}
	return (float)(v * cell_size * cell_size);
}

// The surface as the river draws it: a vertex per cell centre (the water's top where wet, just under the ground
// where dry, so the waterline falls between them), triangles only where water is. Per vertex: UV2 = flow, COLOR.r
// = (depth + 1) / 4, COLOR.g = white water, COLOR.b = 1.
void PhysXLiveWater3D::_update_mesh() {
	if (mesh_instance == nullptr) {
		mesh.instantiate();
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_mesh(mesh);
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance, false, INTERNAL_MODE_BACK);
	}
	if (water_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(physx_river_water_shader_src());
		water_material.instantiate();
		water_material->set_shader(shader);
	}
	const int n = nx * nz;
	const float sx = nx * cell_size;
	const float sz = nz * cell_size;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedVector2Array uv2;
	verts.resize(n);
	normals.resize(n);
	colors.resize(n);
	uv2.resize(n);
	Vector3 *vw = verts.ptrw();
	Vector3 *nw = normals.ptrw();
	Color *cw = colors.ptrw();
	Vector2 *uw = uv2.ptrw();
	auto top = [&](int x, int z) -> float {
		x = CLAMP(x, 0, nx - 1);
		z = CLAMP(z, 0, nz - 1);
		const int i = z * nx + x;
		return ground[i] > NO_GROUND ? ground[i] + depth[i] : 0.0f;
	};
	float y_min = 1e30f;
	float y_max = -1e30f;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const int i = z * nx + x;
			const bool has_ground = ground[i] > NO_GROUND;
			const float d = has_ground ? depth[i] : 0.0f;
			const float g = has_ground ? ground[i] : 0.0f;
			const float y = d > WET ? g + d : g - 0.03f;
			vw[i] = Vector3((x + 0.5f) * cell_size - 0.5f * sx, y, (z + 0.5f) * cell_size - 0.5f * sz);
			const float hx = top(x + 1, z) - top(x - 1, z);
			const float hz = top(x, z + 1) - top(x, z - 1);
			nw[i] = Vector3(-hx, 2.0f * cell_size, -hz).normalized();
			const float d_enc = d > WET ? d : -0.05f;
			cw[i] = Color(CLAMP((d_enc + 1.0f) * 0.25f, 0.0f, 1.0f), CLAMP(foam[i], 0.0f, 1.0f), 1.0f, 1.0f);
			uw[i] = Vector2(vel[i * 2], vel[i * 2 + 1]);
			y_min = MIN(y_min, y);
			y_max = MAX(y_max, y);
		}
	}
	PackedInt32Array indices;
	for (int z = 0; z < nz - 1; z++) {
		for (int x = 0; x < nx - 1; x++) {
			const int a = z * nx + x;
			const int b = a + 1;
			const int c = a + nx;
			const int e = c + 1;
			if (depth[a] > WET || depth[b] > WET || depth[c] > WET || depth[e] > WET) {
				indices.push_back(a);
				indices.push_back(c);
				indices.push_back(b);
				indices.push_back(b);
				indices.push_back(c);
				indices.push_back(e);
			}
		}
	}
	mesh->clear_surfaces();
	if (indices.is_empty()) {
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, water_material);
	mesh_instance->set_custom_aabb(AABB(Vector3(-0.5f * sx, y_min - 1.0f, -0.5f * sz), Vector3(sx, y_max - y_min + 2.0f, sz)));
}

void PhysXLiveWater3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_physics_process_internal(true);
			set_process_internal(!Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
			set_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (is_inside_tree()) {
				reset();
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!built) {
				Ref<World3D> world = get_world_3d();
				PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
				if (space == nullptr) {
					break;
				}
				_allocate();
				_measure_ground(space, false);
				built = true;
				_update_mesh();
			}
			if (ground_update_pending) {
				Ref<World3D> world = get_world_3d();
				PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
				if (space != nullptr) {
					_measure_ground(space, true);
					ground_update_pending = false;
				}
			}
			if (!simulating || Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
			// Substeps short enough for the fastest gravity wave over the deepest water (CFL), at most 16.
			float d_max = 0.05f;
			for (uint32_t i = 0; i < depth.size(); i++) {
				d_max = MAX(d_max, depth[i]);
			}
			const float dt_frame = (float)get_physics_process_delta_time();
			const float dt_max = 0.25f * cell_size / Math::sqrt(GRAVITY * d_max);
			const int steps = CLAMP((int)Math::ceil(dt_frame / dt_max), 1, MAX_SUBSTEPS);
			for (int s = 0; s < steps; s++) {
				_step(dt_frame / steps);
			}
			_update_mesh();
			cost_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (water_material.is_valid()) {
				water_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
				raster_reflection_state.update(this, water_material, raster_reflections);
			}
		} break;
	}
}

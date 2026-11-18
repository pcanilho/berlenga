/**************************************************************************/
/*  physx_cloth_3d.cpp                                                    */
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

#include "physx_cloth_3d.h"

#include "../godot_physx_server_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/area_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#ifdef GODOT_PHYSX_FLOW
#include "../flow/physx_fire_spread_3d.h"
#include "../flow/physx_flow_3d.h"
#endif

#include "modules/modules_enabled.gen.h" // For weather.
#include "modules/world/world_contracts.h"

// Map a friendly 0..1 stiffness to XPBD compliance (m/N). 1 -> rigid, 0 -> slack.
static float _stiffness_to_compliance(float p_stiffness, float p_max_compliance) {
	const float s = CLAMP(p_stiffness, 0.0f, 1.0f);
	return (1.0f - s) * (1.0f - s) * p_max_compliance;
}

void PhysXCloth3D::_apply_solver_settings() {
	XPBDClothSolver::Settings s;
	s.substeps = MAX(substeps, 1);
	s.stretch_compliance = _stiffness_to_compliance(stiffness, 5.0e-3f);
	s.shear_compliance = _stiffness_to_compliance(shear_stiffness, 2.0e-1f);
	// Water softens the fibres (it plasticises cellulose: wet cotton bends about half as stiffly) and adds its weight.
	s.bend_compliance = _stiffness_to_compliance(bend_stiffness * (1.0f - 0.5f * wet.effective()), 6.0e-1f);
	s.damping = MAX(damping, 0.0f);
	s.drag = MAX(drag, 0.0f);
	s.lift = MAX(lift, 0.0f);
	s.density = MAX(density, 1.0e-3f) * (1.0f + water_capacity * wet.effective());
	s.tear_factor = is_tearing() ? tear_factor : 0.0f;
	solver.settings = s;
}

void PhysXCloth3D::_rebuild() {
	_destroy();
	stepped = false;
	built_xform = is_inside_tree() ? get_global_transform() : Transform3D();

	_apply_solver_settings();

	if (source_mesh.is_valid() && source_mesh->get_surface_count() > 0) {
		const Array arr = source_mesh->surface_get_arrays(0);
		const PackedVector3Array verts = arr[Mesh::ARRAY_VERTEX];
		const PackedVector2Array uv = arr[Mesh::ARRAY_TEX_UV];
		PackedInt32Array idx = arr[Mesh::ARRAY_INDEX];
		if (idx.is_empty()) {
			idx.resize(verts.size());
			for (int i = 0; i < verts.size(); i++) {
				idx.write[i] = i;
			}
		}
		solver.build_mesh(verts, idx, uv);
	} else {
		solver.build_grid(grid_columns, grid_rows, grid_size);
	}
	if (!solver.is_built()) {
		return;
	}

	built = true;
	_resolve_pins();
	solver.reset(get_global_transform());

	mesh = RenderingServer::get_singleton()->mesh_create();
	set_base(mesh);

	// Prefer the PhysX GPU deformable surface; fall back to the solver above.
	const bool want_gpu = !Engine::get_singleton()->is_editor_hint() && is_inside_tree();
	if (want_gpu && _try_build_gpu()) {
		_update_gpu_mesh();
	} else {
		_update_mesh();
	}
	update_gizmos();
}

bool PhysXCloth3D::_gpu_available() {
	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	return server && server->has_gpu();
}

bool PhysXCloth3D::_try_build_gpu() {
	if (simulation_mode == SIM_CPU || (simulation_mode == SIM_AUTO && is_tearing())) {
		return false; // strain tearing splits vertices, which only the CPU solver can do
	}
#ifdef GODOT_PHYSX_FLOW
	if (simulation_mode == SIM_AUTO && burnable && PhysXFireSpread3D::any()) {
		return false; // a fire in the scene: burning holes drop triangles, which only the CPU solver can do
	}
#endif
	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	if (!server || !server->has_gpu() || get_world_3d().is_null()) {
		return false;
	}
	const RID cloth = server->cloth_create();
	if (cloth.is_null()) {
		return false;
	}

	// World-space rest mesh + topology from the solver.
	const LocalVector<Vector3> &wp = solver.get_positions();
	const LocalVector<int> &tri = solver.get_indices();
	Vector<Vector3> positions;
	positions.resize(wp.size());
	for (uint32_t i = 0; i < wp.size(); i++) {
		positions.write[i] = wp[i];
	}
	Vector<int32_t> idx;
	idx.resize(tri.size());
	for (uint32_t i = 0; i < tri.size(); i++) {
		idx.write[i] = tri[i];
	}
	Vector<int32_t> pins;
	for (uint32_t i = 0; i < pinned_index_cache.size(); i++) {
		pins.push_back(pinned_index_cache[i]);
	}
	const RID space = get_world_3d()->get_space();
	const float th = MAX(solver.vertex_radius() * 0.5f, 0.004f);
	const Transform3D xform = get_global_transform();
	bool ready = false;
	// Built where the scene may be touched, and waited for: whether PhysX took the mesh decides the CPU fallback.
	server->run_on_physics_thread([&]() {
		server->cloth_set_space(cloth, space);
		server->cloth_set_params(cloth, th, density, stiffness, bend_stiffness, damping, collision_mask);
		server->cloth_build(cloth, positions, idx, xform);
		ready = server->cloth_is_ready(cloth);
		if (ready) {
			server->cloth_set_pinned(cloth, pins);
		} else {
			server->free_rid(cloth);
		}
	},
			true);
	if (!ready) {
		return false;
	}
	gpu_cloth = cloth;
	gpu_mesh_version = 0;
	// Keep the solver's rest data around for the gizmo and vertex count; it is
	// just not stepped while the GPU owns the simulation.
	return true;
}

void PhysXCloth3D::_destroy() {
	built = false;
	if (gpu_cloth.is_valid()) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		if (server) {
			server->free_rid(gpu_cloth);
		}
		gpu_cloth = RID();
	}
	if (mesh.is_valid()) {
		set_base(RID());
		RenderingServer::get_singleton()->free_rid(mesh);
		mesh = RID();
	}
	if (collision_sphere.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(collision_sphere);
		collision_sphere = RID();
	}
	solver.clear();
#ifdef GODOT_PHYSX_FLOW
	burn.reset();
	if (burn_override.is_valid() && is_inside_tree()) {
		const Ref<Material> mo = get_material_override();
		RenderingServer::get_singleton()->instance_geometry_set_material_override(get_instance(), mo.is_valid() ? mo->get_rid() : RID());
	}
	burn_override = RID();
#endif
	pinned_index_cache.clear();
}

void PhysXCloth3D::_resolve_pins() {
	pinned_index_cache.clear();
	const int n = solver.vertex_count();
	for (int i = 0; i < n; i++) {
		solver.set_pinned(i, false);
	}

	const int c = solver.grid_cols();
	const int r = solver.grid_rows();
	auto pin = [&](int v) {
		if (v >= 0 && v < n && !solver.is_pinned(v)) {
			solver.set_pinned(v, true);
			pinned_index_cache.push_back(v);
		}
	};

	if (c > 0 && r > 0) {
		switch (pin_mode) {
			case PIN_TOP_EDGE:
				for (int x = 0; x < c; x++) {
					pin(x);
				}
				break;
			case PIN_TOP_CORNERS:
				pin(0);
				pin(c - 1);
				break;
			case PIN_LEFT_EDGE:
				for (int y = 0; y < r; y++) {
					pin(y * c);
				}
				break;
			case PIN_RIGHT_EDGE:
				for (int y = 0; y < r; y++) {
					pin(y * c + (c - 1));
				}
				break;
			case PIN_NONE:
			default:
				break;
		}
	}
	for (int i = 0; i < pinned_vertices.size(); i++) {
		pin(pinned_vertices[i]);
	}

	if (gpu_cloth.is_valid()) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		if (server) {
			Vector<int32_t> pins;
			for (uint32_t i = 0; i < pinned_index_cache.size(); i++) {
				pins.push_back(pinned_index_cache[i]);
			}
			server->cloth_set_pinned(gpu_cloth, pins);
		}
	}

	// The anchor is followed incrementally in _step by its frame-to-frame delta;
	// just record where it starts.
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(anchor_path));
	last_anchor_xform = anchor ? anchor->get_global_transform() : Transform3D();
}

// Smooth value noise in -1..1 (quintic fade), for the eddies.
static float _eddy_noise(const Vector3 &p_p, uint32_t p_seed) {
	const Vector3 i = p_p.floor();
	const Vector3 f = p_p - i;
	const Vector3 u = f * f * f * (f * (f * 6.0f - Vector3(15, 15, 15)) + Vector3(10, 10, 10));
	auto h = [p_seed](int x, int y, int z) {
		uint32_t n = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ p_seed * 2654435761u;
		n = (n ^ (n >> 13)) * 1274126177u;
		return float((n ^ (n >> 16)) & 0xffff) / 32767.5f - 1.0f;
	};
	const int x = (int)i.x, y = (int)i.y, z = (int)i.z;
	const float a = Math::lerp(Math::lerp(h(x, y, z), h(x + 1, y, z), u.x), Math::lerp(h(x, y + 1, z), h(x + 1, y + 1, z), u.x), u.y);
	const float b = Math::lerp(Math::lerp(h(x, y, z + 1), h(x + 1, y, z + 1), u.x), Math::lerp(h(x, y + 1, z + 1), h(x + 1, y + 1, z + 1), u.x), u.y);
	return Math::lerp(a, b, u.z);
}

// Turbulence varies over the cloth, not only in time: eddies carried along by the mean wind (Taylor's frozen
// turbulence), at two scales near the size of a sheet, push its parts differently so it ripples and billows instead
// of tilting as a plane. Gusts near the ground are about 0.3 to 0.5 of the mean speed (turbulence intensity), most of
// it along the wind.
void PhysXCloth3D::_eddy_air(const Vector3 &p_wind) {
	const bool has_flow = !flow_path.is_empty();
	const float speed = p_wind.length();
	if (has_flow || wind_turbulence <= 0.0f || speed < 0.05f) {
		if (eddies_on) {
			eddies_on = false;
			if (!has_flow) {
				solver.set_air(LocalVector<Vector3>());
			}
		}
		return;
	}
	eddies_on = true;
	const LocalVector<Vector3> &pos = solver.get_positions();
	LocalVector<Vector3> air;
	air.resize(pos.size());
	const Vector3 drift = p_wind * (float)wind_time;
	const float amp = speed * wind_turbulence * 0.6f;
	for (uint32_t i = 0; i < pos.size(); i++) {
		const Vector3 q = pos[i] - drift;
		const Vector3 q1 = q / 1.2f;
		const Vector3 q2 = q / 0.45f + Vector3(0.0f, 0.0f, (float)wind_time * 0.7f);
		air[i] = Vector3(_eddy_noise(q1, 1u) + 0.5f * _eddy_noise(q2, 4u),
						 0.6f * (_eddy_noise(q1, 2u) + 0.5f * _eddy_noise(q2, 5u)),
						 _eddy_noise(q1, 3u) + 0.5f * _eddy_noise(q2, 6u)) *
				amp;
	}
	solver.set_air(air);
}

Vector3 PhysXCloth3D::_sample_wind(double p_delta) {
	if (!wind_enabled) {
		return Vector3();
	}
	wind_time += p_delta;
	Vector3 w = wind;

	// The PhysX module does not expose areas to point queries, so read the
	// assigned wind Area3D's settings directly (also keeps this backend-agnostic).
	Area3D *area = Object::cast_to<Area3D>(get_node_or_null(wind_area));
	if (area && area->get_wind_force_magnitude() > 0.0f) {
		Node3D *src_node = Object::cast_to<Node3D>(area->get_node_or_null(area->get_wind_source_path()));
		Vector3 dir(-1, 0, 0);
		Vector3 src;
		if (src_node) {
			const Transform3D sx = src_node->get_global_transform();
			dir = -sx.basis.get_column(Vector3::AXIS_Z).normalized();
			src = sx.origin;
		}
		// Cloth centroid, for the downwind attenuation.
		const LocalVector<Vector3> &sp = solver.get_positions();
		Vector3 center = get_global_transform().origin;
		if (!sp.is_empty()) {
			center = Vector3();
			for (uint32_t i = 0; i < sp.size(); i++) {
				center += sp[i];
			}
			center /= (float)sp.size();
		}
		const float att = area->get_wind_attenuation_factor();
		const float proj = MAX((center - src).dot(dir), 1.0f);
		w += dir * (area->get_wind_force_magnitude() * Math::pow(proj, -att));
	}

	if (wind_turbulence > 0.0f && w != Vector3()) {
		const float speed = w.length();
		const Vector3 dir = w / speed;
		Vector3 perp = dir.cross(Vector3(0, 1, 0));
		if (perp.length_squared() < 1.0e-4f) {
			perp = dir.cross(Vector3(1, 0, 0));
		}
		perp.normalize();
		const Vector3 up = dir.cross(perp).normalized();
		const float t = (float)wind_time;
		const float gust = 0.6f * Math::sin(t * 1.7f) + 0.4f * Math::sin(t * 4.3f + 1.3f);
		const float sway = 0.5f * Math::sin(t * 2.9f + 0.7f);
		w += dir * (speed * wind_turbulence * gust);
		w += perp * (speed * wind_turbulence * 0.5f * sway);
		w += up * (speed * wind_turbulence * 0.3f * Math::sin(t * 3.7f));
	}
	// The world's wind at the cloth, with its own gusts (wind_turbulence only shapes the local wind above).
	const WorldWeather *weather = WorldContracts::weather();
	if (weather_wind && weather->has_provider()) {
		const LocalVector<Vector3> &sp = solver.get_positions();
		Vector3 center = get_global_transform().origin;
		if (!sp.is_empty()) {
			center = Vector3();
			for (uint32_t i = 0; i < sp.size(); i++) {
				center += sp[i];
			}
			center /= (float)sp.size();
		}
		w += weather->wind_at(center);
	}
	// A fire's indraft near the ground (WorldContracts' fire): a sheet beside a fire leans into it.
	if (const WorldFire *fire = WorldContracts::fire(); fire->has_provider()) {
		w += fire->wind_at(get_global_transform().xform(get_aabb().get_center()));
	}
	return w;
}

void PhysXCloth3D::_step(double p_delta) {
	if (!built || !simulating || p_delta <= 0.0) {
		return;
	}
	if (!stepped) {
		if (!get_global_transform().is_equal_approx(built_xform)) {
			_rebuild(); // placed by a script after it entered the tree
			if (!built) {
				return;
			}
		}
		stepped = true;
	}

	const Vector3 wind_now = _sample_wind(p_delta);
	_couple_flow(p_delta);
	_update_wetness(p_delta, wind_now);

	// --- GPU path: the deformable surface is stepped by the physics server. ---
	if (gpu_cloth.is_valid()) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		if (!server) {
			return;
		}
		Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(anchor_path));
		if (anchor && !pinned_index_cache.is_empty()) {
			// Feed the pinned vertices' target world positions from the last mesh.
			const Transform3D now = anchor->get_global_transform();
			const Transform3D delta = now * last_anchor_xform.affine_inverse();
			Vector<Vector3> targets;
			targets.resize(render_verts.size());
			const Transform3D gt = get_global_transform();
			for (int i = 0; i < render_verts.size(); i++) {
				targets.write[i] = Vector3(NAN, 0, 0);
			}
			for (uint32_t i = 0; i < pinned_index_cache.size(); i++) {
				const int v = pinned_index_cache[i];
				if (v >= 0 && v < targets.size()) {
					targets.write[v] = delta.xform(gt.xform(render_verts[v]));
				}
			}
			server->cloth_set_pin_targets(gpu_cloth, targets);
			last_anchor_xform = now;
		}
		server->cloth_apply_wind(gpu_cloth, wind_now, drag, lift, p_delta);
		_update_gpu_mesh();
		return;
	}

	// --- CPU path ---
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(anchor_path));
	if (anchor && !pinned_index_cache.is_empty()) {
		const Transform3D now = anchor->get_global_transform();
		const Transform3D delta = now * last_anchor_xform.affine_inverse();
		LocalVector<Vector3> &pos = solver.positions_mut();
		for (uint32_t i = 0; i < pinned_index_cache.size(); i++) {
			const int v = pinned_index_cache[i];
			solver.set_pin_target(v, delta.xform(pos[v]));
		}
		last_anchor_xform = now;
	}

	Vector3 gravity_vec = GLOBAL_GET("physics/3d/default_gravity_vector");
	const float gravity_mag = GLOBAL_GET("physics/3d/default_gravity");
	if (gravity_vec == Vector3()) {
		gravity_vec = Vector3(0, -1, 0);
	}
	const Vector3 gravity = gravity_vec.normalized() * gravity_mag;

	// Under water (docs/ideas/physx.md, "River current drags flags and sheets"): as worn clothing, soaked fabric keeps
	// about 30% of its weight and the current drags it at Morison's 0.5 rho Cd |u| per its soaked areal mass.
	if (wet.water_surface > -1.0e8f) {
		const float areal = MAX(density, 1.0e-3f) * (1.0f + water_capacity);
		solver.settings.water_height = wet.water_surface;
		solver.settings.water_flow = wet.water_flow;
		solver.settings.water_drag = MIN(0.5f * 1000.0f * 1.2f * (wet.water_flow.length() + 0.1f) / MAX(areal, 0.05f), 200.0f);
		solver.settings.water_gravity = 0.3f;
	} else {
		solver.settings.water_height = -1.0e9f;
		solver.settings.water_drag = 0.0f;
	}
	_eddy_air(wind_now);
	if (collision_enabled) {
		pre_step_positions = solver.get_positions();
	}
	const int torn = solver.step((float)p_delta, gravity, wind_now);
	if (torn > 0) {
		const Transform3D inv = get_global_transform().affine_inverse();
		const LocalVector<Vector3> &at = solver.get_last_tears();
		for (uint32_t i = 0; i < at.size(); i++) {
			emit_signal(SNAME("torn"), inv.xform(at[i]));
		}
	}

	if (collision_enabled) {
		_collide();
	}
#ifdef GODOT_PHYSX_FLOW
	_step_burn(p_delta);
#endif
	_update_mesh();
}

// Rain in, evaporation out (ClothWetness); the catch from how the cloth faces the rain.
void PhysXCloth3D::_update_wetness(double p_delta, const Vector3 &p_wind) {
	if (render_verts.size() < 3) {
		return;
	}
	const Transform3D gt = get_global_transform();
	bool update_catch = false;
	wet.step(this, gt.xform(get_aabb()), p_wind, (float)p_delta, density * water_capacity, weather_wetting, update_catch);
	if (update_catch) {
		// |n . d| / |d.y| over the cloth's triangles, both faces: a sheet facing the wind wets almost as fast as a flat
		// one, and one streaming edge on to the rain wets through the gusts.
		Vector3 dirs[4];
		float dy = 1.0f;
		ClothWetness::rain_directions(p_wind, dirs, dy);
		const Basis b = gt.basis;
		float area = 0.0f;
		float caught = 0.0f;
		const Vector3 *v = render_verts.ptr();
		const int *ix = render_indices.ptr();
		const int vn = render_verts.size();
		for (int t = 0; t + 2 < render_indices.size(); t += 3) {
			if (ix[t] >= vn || ix[t + 1] >= vn || ix[t + 2] >= vn) {
				continue;
			}
			const Vector3 cr = b.xform((v[ix[t + 1]] - v[ix[t]]).cross(v[ix[t + 2]] - v[ix[t]]));
			area += cr.length();
			for (const Vector3 &d : dirs) {
				caught += 0.25f * Math::abs(cr.dot(d));
			}
		}
		wet.rain_catch = area > 0.0f ? caught / (area * MAX(dy, 0.05f)) : 0.0f;
	}
	if (Math::abs(wet.effective() - wet_applied) > 0.01f || (wet.effective() == 0.0f && wet_applied != 0.0f)) {
		wet_applied = wet.effective();
		_apply_solver_settings();
		if (built && !gpu_cloth.is_valid()) {
			solver.refresh_masses();
		}
	}
}

void PhysXCloth3D::set_wetness(float p_v) {
	wet.wetness = CLAMP(p_v, 0.0f, 1.0f);
	wet_applied = wet.effective();
	_apply_solver_settings();
	if (built && !gpu_cloth.is_valid()) {
		solver.refresh_masses();
	}
}

void PhysXCloth3D::set_water_capacity(float p_v) {
	water_capacity = MAX(p_v, 0.0f);
	_apply_solver_settings();
	if (built && !gpu_cloth.is_valid()) {
		solver.refresh_masses();
	}
}

#ifdef GODOT_PHYSX_FLOW
void PhysXCloth3D::_step_burn(double p_delta) {
	if (!burnable || p_delta <= 0.0) {
		return;
	}
	PhysXFireSpread3D *fire = fire_path.is_empty() ? PhysXFireSpread3D::find_at(get_global_position()) : Object::cast_to<PhysXFireSpread3D>(get_node_or_null(fire_path));
	PhysXFlow3D *flow = flow_path.is_empty() ? nullptr : Object::cast_to<PhysXFlow3D>(get_node_or_null(flow_path));
	if (!flow && fire) {
		flow = fire->get_flow();
	}
	burn.settings.flame_spread = flame_spread;
	burn.settings.burn_through_time = burn_through_time;
	burn.settings.water = get_water_load();
	burn.step(this, solver, fire, flow, float(p_delta));
}
#endif

void PhysXCloth3D::set_burnable(bool p_v) {
	burnable = p_v;
}

void PhysXCloth3D::set_flame_spread(float p_v) {
	flame_spread = MAX(p_v, 0.0f);
}

void PhysXCloth3D::set_burn_through_time(float p_v) {
	burn_through_time = MAX(p_v, 0.1f);
}

void PhysXCloth3D::ignite(const Vector3 &p_position) {
#ifdef GODOT_PHYSX_FLOW
	ERR_FAIL_COND_MSG(gpu_cloth.is_valid(), "PhysXCloth3D.ignite() needs the CPU solver; set simulation_mode to SIM_CPU.");
	if (built) {
		burn.ignite(solver, p_position);
	}
#endif
}

int PhysXCloth3D::get_burnt_count() const {
#ifdef GODOT_PHYSX_FLOW
	return burn.get_burnt_count();
#else
	return 0;
#endif
}

int PhysXCloth3D::get_burning_count() const {
#ifdef GODOT_PHYSX_FLOW
	return burn.get_burning_count();
#else
	return 0;
#endif
}

// Two-way coupling with a Flow gas, as APEX Turbulence drove APEX Clothing: each vertex takes the gas velocity there
// (read back a few frames late) as extra air for the drag and lift model, and a few of the moving particles enter
// the gas as sphere solids carrying their velocity, so a flapping flag stirs smoke and parts it.
void PhysXCloth3D::_couple_flow(double p_delta) {
#ifdef GODOT_PHYSX_FLOW
	PhysXFlow3D *flow = flow_path.is_empty() ? nullptr : Object::cast_to<PhysXFlow3D>(get_node_or_null(flow_path));
	if (flow == nullptr || p_delta <= 0.0) {
		if (!flow_prev_positions.is_empty()) {
			// Unhooked: no gas air any more.
			flow_prev_positions.clear();
			solver.set_air(LocalVector<Vector3>());
			if (gpu_cloth.is_valid() && GodotPhysXServer3D::get_singleton()) {
				GodotPhysXServer3D::get_singleton()->cloth_set_air(gpu_cloth, Vector<Vector3>());
			}
		}
		return;
	}
	flow->request_velocity();
	LocalVector<Vector3> world;
	if (gpu_cloth.is_valid()) {
		const Transform3D gt = get_global_transform();
		world.resize(render_verts.size());
		for (int i = 0; i < render_verts.size(); i++) {
			world[i] = gt.xform(render_verts[i]);
		}
	} else {
		world = solver.get_positions();
	}
	const uint32_t n = world.size();
	if (n == 0) {
		return;
	}
	LocalVector<Vector3> air_local;
	air_local.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		air_local[i] = flow->sample_velocity(world[i]);
	}
	if (gpu_cloth.is_valid()) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		if (server) {
			Vector<Vector3> air;
			air.resize(n);
			memcpy(air.ptrw(), air_local.ptr(), n * sizeof(Vector3));
			server->cloth_set_air(gpu_cloth, air);
		}
	} else {
		solver.set_air(air_local);
	}
	// The cloth into the gas: up to 32 particles that move, a cell or so across.
	if (flow_prev_positions.size() == n) {
		const uint32_t stride = MAX(1u, n / 32u);
		const float radius = MAX(flow->get_cell_size(), 0.03f);
		const float inv_dt = 1.0f / (float)p_delta;
		for (uint32_t i = 0; i < n; i += stride) {
			const Vector3 v = (world[i] - flow_prev_positions[i]) * inv_dt;
			if (v.length_squared() > 0.09f) { // 0.3 m/s: still cloth is left out
				flow->add_transient_solid(world[i], radius, v);
			}
		}
	}
	flow_prev_positions = world;
#endif
}

void PhysXCloth3D::_update_gpu_mesh() {
	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	if (!server || gpu_cloth.is_null() || mesh.is_null()) {
		return;
	}
	PackedVector3Array wpos;
	PackedInt32Array idx;
	const int tris = server->cloth_get_mesh(gpu_cloth, wpos, idx, gpu_mesh_version);
	if (tris < 0) {
		return; // unchanged
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->mesh_clear(mesh);
	if (tris == 0 || wpos.size() < 3) {
		return;
	}

	const Transform3D inv = get_global_transform().affine_inverse();
	const int n = wpos.size();
	render_verts.resize(n);
	render_normals.resize(n);
	render_indices = idx;
	Vector3 *vw = render_verts.ptrw();
	Vector3 *nw = render_normals.ptrw();
	const Vector3 *sp = wpos.ptr();
	for (int i = 0; i < n; i++) {
		vw[i] = inv.xform(sp[i]);
		nw[i] = Vector3();
	}
	const int *ip = idx.ptr();
	for (int t = 0; t + 2 < idx.size(); t += 3) {
		const int a = ip[t], b = ip[t + 1], c = ip[t + 2];
		if (a >= n || b >= n || c >= n) {
			continue;
		}
		// Godot's front faces wind clockwise, so this is the front side's normal.
		const Vector3 fn = (vw[c] - vw[a]).cross(vw[b] - vw[a]);
		nw[a] += fn;
		nw[b] += fn;
		nw[c] += fn;
	}
	for (int i = 0; i < n; i++) {
		nw[i] = nw[i].normalized();
	}

	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = render_verts;
	arrays[RSE::ARRAY_NORMAL] = render_normals;
	if (_fill_uvs(render_verts.size())) {
		arrays[RSE::ARRAY_TEX_UV] = render_uvs;
	}
	arrays[RSE::ARRAY_INDEX] = render_indices;
	rs->mesh_add_surface_from_arrays(mesh, RSE::PRIMITIVE_TRIANGLES, arrays);

	AABB box(render_verts[0], Vector3());
	for (int i = 1; i < n; i++) {
		box.expand_to(render_verts[i]);
	}
	rs->mesh_set_custom_aabb(mesh, box.grow(0.2));
}

void PhysXCloth3D::_collide() {
	Ref<World3D> world = get_world_3d();
	if (world.is_null()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PhysicsDirectSpaceState3D *ss = world->get_direct_space_state();
	if (!ss) {
		return; // not available yet during world setup
	}

	const float radius = MAX(solver.vertex_radius(), 0.005f);
	// Wet fabric clings: it sticks to what it touches over a film-thin gap, and slides less (water raises the friction
	// of fabric on skin and on most surfaces, Gerhardt et al. 2008).
	const float gap = wet.effective() > 0.05f ? radius * 1.5f * wet.effective() : 0.0f;
	const float wet_friction = friction + (1.0f - friction) * 0.6f * wet.effective();
	if (collision_sphere.is_null()) {
		collision_sphere = ps->sphere_shape_create();
	}
	ps->shape_set_data(collision_sphere, radius + gap);

	PhysicsDirectSpaceState3D::ShapeParameters params;
	params.shape_rid = collision_sphere;
	params.collision_mask = collision_mask;
	params.collide_with_bodies = true;
	params.collide_with_areas = false;

	LocalVector<Vector3> &pos = solver.positions_mut();
	LocalVector<Vector3> &vel = solver.velocities_mut();
	const int n = solver.vertex_count();
	const bool swept = (int)pre_step_positions.size() == n;
	for (int i = 0; i < n; i++) {
		if (solver.is_pinned(i)) {
			continue;
		}
		// Swept: a particle the step carried across a thin collider (a rope, a rail, an edge), which the overlap test
		// below would push out on the far side, is stopped where its path met the surface.
		if (swept) {
			const Vector3 move = pos[i] - pre_step_positions[i];
			const float len = move.length();
			if (len > 0.25f * radius) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = pre_step_positions[i];
				rp.to = pos[i]; // the centre's own path: only a real crossing, not a graze ahead of a sliding particle
				rp.collision_mask = collision_mask;
				rp.hit_from_inside = false;
				PhysicsDirectSpaceState3D::RayResult rr;
				if (ss->intersect_ray(rp, rr) && rr.position.is_finite() && rr.normal.is_finite() && rr.normal.length_squared() > 0.25f && rr.normal.dot(move) < 0.0f && rr.position.distance_squared_to(rp.from) > 1.0e-10f) {
					pos[i] = rr.position + rr.normal * (radius + 0.001f);
					const float vn = vel[i].dot(rr.normal);
					if (vn < 0.0f) {
						vel[i] -= rr.normal * vn;
					}
				}
			}
		}
		params.transform = Transform3D(Basis(), pos[i]);
		PhysicsDirectSpaceState3D::ShapeRestInfo info;
		if (!ss->rest_info(params, &info)) {
			continue;
		}
		// The PhysX backend reports info.normal as the direction to push the query
		// shape out and info.point as (center - normal * penetration), so the
		// penetration depth is just the projection back onto the normal.
		const Vector3 push_dir = info.normal;
		const float depth = (pos[i] - info.point).dot(push_dir) - gap;
		if (depth > 0.0f) {
			pos[i] += push_dir * (depth + 0.001f);
			const float vn = vel[i].dot(push_dir);
			Vector3 v_tangent = vel[i] - push_dir * vn;
			v_tangent *= (1.0f - wet_friction); // Coulomb-style sliding friction
			vel[i] = v_tangent + push_dir * MAX(vn, 0.0f); // no inward velocity
		} else if (gap > 0.0f && depth > -gap) {
			// Within the water film's reach of the surface: capillary adhesion draws the wet fabric onto it and holds it
			// against peeling away.
			pos[i] += push_dir * (depth * 0.5f * wet.effective());
			const float vn = vel[i].dot(push_dir);
			if (vn > 0.0f) {
				vel[i] -= push_dir * vn * 0.8f * wet.effective();
			}
			vel[i] -= (vel[i] - push_dir * vel[i].dot(push_dir)) * (0.5f * wet_friction);
		}
	}
}

// The solver's texture coordinates (a grid's own, or the source mesh's, carried through tears) into
// render_uvs, when it has one per drawn vertex.
bool PhysXCloth3D::_fill_uvs(int p_count) {
	const LocalVector<Vector2> &uv = solver.get_uvs();
	if (p_count <= 0 || (int)uv.size() != p_count) {
		return false;
	}
	render_uvs.resize(p_count);
	Vector2 *uw = render_uvs.ptrw();
	for (int i = 0; i < p_count; i++) {
		uw[i] = uv[i];
	}
	return true;
}

void PhysXCloth3D::_update_mesh() {
	if (mesh.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->mesh_clear(mesh);

	const LocalVector<Vector3> &wpos = solver.get_positions();
	const LocalVector<int> &tri = solver.get_indices();
	const int n = wpos.size();
	if (n < 3 || tri.size() < 3) {
		return;
	}

	const Transform3D inv = get_global_transform().affine_inverse();
	render_verts.resize(n);
	render_normals.resize(n);
	render_indices.resize(tri.size());
	Vector3 *vw = render_verts.ptrw();
	Vector3 *nw = render_normals.ptrw();
	int *iw = render_indices.ptrw();
	for (int i = 0; i < n; i++) {
		vw[i] = inv.xform(wpos[i]);
		nw[i] = Vector3();
	}
	for (int t = 0; t + 2 < (int)tri.size(); t += 3) {
		const int a = tri[t];
		const int b = tri[t + 1];
		const int c = tri[t + 2];
		iw[t] = a;
		iw[t + 1] = b;
		iw[t + 2] = c;
		// Godot's front faces wind clockwise, so this is the front side's normal.
		const Vector3 fn = (vw[c] - vw[a]).cross(vw[b] - vw[a]);
		nw[a] += fn;
		nw[b] += fn;
		nw[c] += fn;
	}
	for (int i = 0; i < n; i++) {
		nw[i] = nw[i].normalized();
	}

	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = render_verts;
	arrays[RSE::ARRAY_NORMAL] = render_normals;
	if (_fill_uvs(render_verts.size())) {
		arrays[RSE::ARRAY_TEX_UV] = render_uvs;
	}
	arrays[RSE::ARRAY_INDEX] = render_indices;
#ifdef GODOT_PHYSX_FLOW
	// The burn state for the burn material: char, glow, scorch and how far each part has burnt through.
	// Also while wet: the same material darkens with the water held.
	if (burn.has_marks() || wet.effective() > 0.0f || burn_override.is_valid()) {
		burn.fill_colors(burn_colors, n);
		arrays[RSE::ARRAY_COLOR] = burn_colors;
		if (burn.settings.wetness != wet.effective()) {
			burn.settings.wetness = wet.effective();
			burn.update_wetness();
		}
		const RID m = burn.get_material(get_material_override());
		if (m != burn_override && m.is_valid()) {
			burn_override = m;
			rs->instance_geometry_set_material_override(get_instance(), m);
		}
	}
#endif
	rs->mesh_add_surface_from_arrays(mesh, RSE::PRIMITIVE_TRIANGLES, arrays);

	AABB box(render_verts[0], Vector3());
	for (int i = 1; i < n; i++) {
		box.expand_to(render_verts[i]);
	}
	rs->mesh_set_custom_aabb(mesh, box.grow(solver.vertex_radius() * 4.0f));
}

// Lets pinned vertices go mid-simulation, without the reset that changing pin_mode or
// pinned_vertices does: the cloth keeps its motion, so a pinned edge tears free of its pole.
// The change lasts until the pins are next resolved (a property change or reset()).
// Rips the cloth along a line (local space, measured on the rest shape's XY plane). Each call
// cuts one segment; calling it with a lengthening segment over a few frames makes the rip run.
// CPU solver only: the GPU deformable surface can't drop constraints.
int PhysXCloth3D::tear(const Vector3 &p_from, const Vector3 &p_to) {
	if (!built) {
		return 0;
	}
	ERR_FAIL_COND_V_MSG(gpu_cloth.is_valid(), 0, "PhysXCloth3D.tear() needs the CPU solver; set simulation_mode to SIM_CPU.");
	const int removed = solver.tear(Vector2(p_from.x, p_from.y), Vector2(p_to.x, p_to.y));
	if (removed > 0) {
		_update_mesh();
	}
	return removed;
}

// Holds pinned vertices at new places (local space), as pegs set closer than the cloth is wide so it hangs in swags
// between them. CPU solver; lasts until the pins are next resolved.
void PhysXCloth3D::move_pins(const PackedInt32Array &p_vertices, const PackedVector3Array &p_positions) {
	ERR_FAIL_COND(p_vertices.size() != p_positions.size());
	ERR_FAIL_COND_MSG(gpu_cloth.is_valid(), "PhysXCloth3D.move_pins() needs the CPU solver; set simulation_mode to SIM_CPU.");
	if (!built) {
		return;
	}
	const Transform3D gt = get_global_transform();
	for (int i = 0; i < p_vertices.size(); i++) {
		const int v = p_vertices[i];
		if (v >= 0 && v < solver.vertex_count()) {
			solver.set_pin_target(v, gt.xform(p_positions[i]));
		}
	}
}

int PhysXCloth3D::release_pins(const PackedInt32Array &p_vertices) {
	if (!built) {
		return 0;
	}
	int released = 0;
	for (int i = 0; i < p_vertices.size(); i++) {
		const int v = p_vertices[i];
		if (v < 0 || v >= solver.vertex_count() || !solver.is_pinned(v)) {
			continue;
		}
		solver.set_pinned(v, false);
		for (uint32_t k = 0; k < pinned_index_cache.size(); k++) {
			if (pinned_index_cache[k] == v) {
				pinned_index_cache.remove_at(k);
				break;
			}
		}
		released++;
	}
	if (released > 0 && gpu_cloth.is_valid()) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		if (server) {
			Vector<int32_t> pins;
			for (uint32_t k = 0; k < pinned_index_cache.size(); k++) {
				pins.push_back(pinned_index_cache[k]);
			}
			server->cloth_set_pinned(gpu_cloth, pins);
		}
	}
	return released;
}

PackedVector3Array PhysXCloth3D::get_pinned_positions() const {
	PackedVector3Array out;
	if (!built) {
		return out;
	}
	const Transform3D inv = get_global_transform().affine_inverse();
	const LocalVector<Vector3> &wpos = solver.get_positions();
	for (uint32_t i = 0; i < pinned_index_cache.size(); i++) {
		const int v = pinned_index_cache[i];
		if (v >= 0 && v < (int)wpos.size()) {
			out.push_back(inv.xform(wpos[v]));
		}
	}
	return out;
}

void PhysXCloth3D::reset() {
	if (!built) {
		return;
	}
	_resolve_pins();
	solver.reset(get_global_transform());
	last_anchor_xform = Transform3D();
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(anchor_path));
	if (anchor) {
		last_anchor_xform = anchor->get_global_transform();
	}
	_update_mesh();
}

void PhysXCloth3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_rebuild();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_destroy();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Only the rest pose lives in local space; a running sim owns world
			// positions, so a mid-sim move should not teleport it (a move before
			// the first step rebuilds it there: see _step).
		} break;
	}
}

AABB PhysXCloth3D::get_aabb() const {
	if (render_verts.size() >= 2) {
		AABB box(render_verts[0], Vector3());
		for (int i = 1; i < render_verts.size(); i++) {
			box.expand_to(render_verts[i]);
		}
		return box.grow(0.1);
	}
	const Vector3 e(grid_size.x * 0.5, grid_size.y * 0.5, MAX(grid_size.x, grid_size.y) * 0.5);
	return AABB(-e, e * 2.0);
}

PackedStringArray PhysXCloth3D::get_configuration_warnings() const {
	PackedStringArray w = GeometryInstance3D::get_configuration_warnings();
	if (pin_mode == PIN_NONE && pinned_vertices.is_empty() && anchor_path.is_empty()) {
		w.push_back(RTR("No pinned vertices: the cloth will fall freely. Set \"Pin Mode\" or \"Pinned Vertices\", or an \"Anchor Path\"."));
	}
	if (source_mesh.is_valid() && source_mesh->get_surface_count() == 0) {
		w.push_back(RTR("\"Source Mesh\" has no surfaces."));
	}
	if (simulation_mode == SIM_GPU && is_tearing()) {
		w.push_back(RTR("\"Tear Factor\" needs the CPU solver: the GPU deformable surface can't split vertices, so this cloth won't tear. Set \"Simulation Mode\" to Auto or CPU."));
	}
	if (simulation_mode == SIM_GPU && !_gpu_available()) {
		w.push_back(RTR("\"Simulation Mode\" is GPU but no CUDA device is available; the cloth runs on the CPU solver."));
	}
	return w;
}

void PhysXCloth3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == StringName("simulation_mode") && !_gpu_available()) {
		// Hide the GPU option (value 1) when there is no CUDA device, keeping the
		// stored value stable across machines.
		p_property.hint_string = "Auto:0,CPU:2";
	}
}

void PhysXCloth3D::set_simulation_mode(SimulationMode p_mode) {
	if (simulation_mode == p_mode) {
		return;
	}
	simulation_mode = p_mode;
	if (is_inside_world()) {
		_rebuild();
	}
	update_configuration_warnings();
}

void PhysXCloth3D::set_source_mesh(const Ref<Mesh> &p_mesh) {
	source_mesh = p_mesh;
	if (is_inside_world()) {
		_rebuild();
	}
	update_configuration_warnings();
}

void PhysXCloth3D::set_grid_columns(int p_v) {
	grid_columns = CLAMP(p_v, 2, 256);
	if (is_inside_world()) {
		_rebuild();
	}
}

void PhysXCloth3D::set_grid_rows(int p_v) {
	grid_rows = CLAMP(p_v, 2, 256);
	if (is_inside_world()) {
		_rebuild();
	}
}

void PhysXCloth3D::set_grid_size(const Vector2 &p_v) {
	grid_size = p_v.maxf(0.01);
	if (is_inside_world()) {
		_rebuild();
	}
}

void PhysXCloth3D::set_substeps(int p_v) {
	substeps = CLAMP(p_v, 1, 40);
	_apply_solver_settings();
}

void PhysXCloth3D::set_stiffness(float p_v) {
	stiffness = CLAMP(p_v, 0.0f, 1.0f);
	_apply_solver_settings();
}

void PhysXCloth3D::set_shear_stiffness(float p_v) {
	shear_stiffness = CLAMP(p_v, 0.0f, 1.0f);
	_apply_solver_settings();
}

void PhysXCloth3D::set_bend_stiffness(float p_v) {
	bend_stiffness = CLAMP(p_v, 0.0f, 1.0f);
	_apply_solver_settings();
}

void PhysXCloth3D::set_damping(float p_v) {
	damping = MAX(p_v, 0.0f);
	_apply_solver_settings();
}

void PhysXCloth3D::set_density(float p_v) {
	density = MAX(p_v, 0.001f);
	if (is_inside_world()) {
		_rebuild();
	}
}

void PhysXCloth3D::set_wind_enabled(bool p_v) {
	wind_enabled = p_v;
}

void PhysXCloth3D::set_wind_area(const NodePath &p_v) {
	wind_area = p_v;
	update_gizmos();
}

void PhysXCloth3D::set_wind(const Vector3 &p_v) {
	wind = p_v;
	update_gizmos();
}

void PhysXCloth3D::set_weather_wind(bool p_v) {
	weather_wind = p_v;
}

void PhysXCloth3D::set_wind_turbulence(float p_v) {
	wind_turbulence = MAX(p_v, 0.0f);
}

void PhysXCloth3D::set_drag(float p_v) {
	drag = MAX(p_v, 0.0f);
	_apply_solver_settings();
}

void PhysXCloth3D::set_lift(float p_v) {
	lift = MAX(p_v, 0.0f);
	_apply_solver_settings();
}

bool PhysXCloth3D::is_tearing() const {
	return tear_factor > 1.0f && (bool)GLOBAL_GET("physics/physx_3d/cloth/tearing");
}

void PhysXCloth3D::set_tear_factor(float p_v) {
	const bool was = is_tearing();
	tear_factor = p_v <= 1.0f ? 0.0f : p_v;
	_apply_solver_settings();
	if (built && simulation_mode == SIM_AUTO && was != is_tearing() && (was ? !gpu_cloth.is_valid() : gpu_cloth.is_valid())) {
		_rebuild(); // tearing needs the CPU solver; without it the GPU one may be had again
	}
	update_configuration_warnings();
}

void PhysXCloth3D::set_pin_mode(PinMode p_v) {
	pin_mode = p_v;
	if (built) {
		_resolve_pins();
		solver.reset(get_global_transform());
		_update_mesh();
	}
	update_configuration_warnings();
	update_gizmos();
}

void PhysXCloth3D::set_pinned_vertices(const PackedInt32Array &p_v) {
	pinned_vertices = p_v;
	if (built) {
		_resolve_pins();
		solver.reset(get_global_transform());
		_update_mesh();
	}
	update_configuration_warnings();
	update_gizmos();
}

void PhysXCloth3D::set_anchor_path(const NodePath &p_v) {
	anchor_path = p_v;
	if (built) {
		reset();
	}
	update_configuration_warnings();
}

void PhysXCloth3D::set_collision_enabled(bool p_v) {
	collision_enabled = p_v;
}

void PhysXCloth3D::set_collision_mask(uint32_t p_v) {
	collision_mask = p_v;
}

void PhysXCloth3D::set_friction(float p_v) {
	friction = CLAMP(p_v, 0.0f, 1.0f);
}

void PhysXCloth3D::set_simulating(bool p_v) {
	simulating = p_v;
}

void PhysXCloth3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("reset"), &PhysXCloth3D::reset);
	ClassDB::bind_method(D_METHOD("get_vertex_count"), &PhysXCloth3D::get_vertex_count);
	ClassDB::bind_method(D_METHOD("get_pinned_positions"), &PhysXCloth3D::get_pinned_positions);
	ClassDB::bind_method(D_METHOD("release_pins", "vertices"), &PhysXCloth3D::release_pins);
	ClassDB::bind_method(D_METHOD("move_pins", "vertices", "positions"), &PhysXCloth3D::move_pins);
	ClassDB::bind_method(D_METHOD("tear", "from", "to"), &PhysXCloth3D::tear);
	ClassDB::bind_method(D_METHOD("set_wetness", "wetness"), &PhysXCloth3D::set_wetness);
	ClassDB::bind_method(D_METHOD("get_wetness"), &PhysXCloth3D::get_wetness);
	ClassDB::bind_method(D_METHOD("set_weather_wetting", "enable"), &PhysXCloth3D::set_weather_wetting);
	ClassDB::bind_method(D_METHOD("is_weather_wetting"), &PhysXCloth3D::is_weather_wetting);
	ClassDB::bind_method(D_METHOD("set_water_capacity", "capacity"), &PhysXCloth3D::set_water_capacity);
	ClassDB::bind_method(D_METHOD("get_water_capacity"), &PhysXCloth3D::get_water_capacity);
	ClassDB::bind_method(D_METHOD("get_water_load"), &PhysXCloth3D::get_water_load);
	ClassDB::bind_method(D_METHOD("set_burnable", "enable"), &PhysXCloth3D::set_burnable);
	ClassDB::bind_method(D_METHOD("is_burnable"), &PhysXCloth3D::is_burnable);
	ClassDB::bind_method(D_METHOD("set_fire_path", "path"), &PhysXCloth3D::set_fire_path);
	ClassDB::bind_method(D_METHOD("get_fire_path"), &PhysXCloth3D::get_fire_path);
	ClassDB::bind_method(D_METHOD("set_flame_spread", "speed"), &PhysXCloth3D::set_flame_spread);
	ClassDB::bind_method(D_METHOD("get_flame_spread"), &PhysXCloth3D::get_flame_spread);
	ClassDB::bind_method(D_METHOD("set_burn_through_time", "seconds"), &PhysXCloth3D::set_burn_through_time);
	ClassDB::bind_method(D_METHOD("get_burn_through_time"), &PhysXCloth3D::get_burn_through_time);
	ClassDB::bind_method(D_METHOD("ignite", "position"), &PhysXCloth3D::ignite);
	ClassDB::bind_method(D_METHOD("get_burnt_count"), &PhysXCloth3D::get_burnt_count);
	ClassDB::bind_method(D_METHOD("get_burning_count"), &PhysXCloth3D::get_burning_count);
	ClassDB::bind_method(D_METHOD("is_gpu_accelerated"), &PhysXCloth3D::is_gpu_accelerated);

	ClassDB::bind_method(D_METHOD("set_simulation_mode", "mode"), &PhysXCloth3D::set_simulation_mode);
	ClassDB::bind_method(D_METHOD("get_simulation_mode"), &PhysXCloth3D::get_simulation_mode);
	ClassDB::bind_method(D_METHOD("set_source_mesh", "mesh"), &PhysXCloth3D::set_source_mesh);
	ClassDB::bind_method(D_METHOD("get_source_mesh"), &PhysXCloth3D::get_source_mesh);
	ClassDB::bind_method(D_METHOD("set_grid_columns", "columns"), &PhysXCloth3D::set_grid_columns);
	ClassDB::bind_method(D_METHOD("get_grid_columns"), &PhysXCloth3D::get_grid_columns);
	ClassDB::bind_method(D_METHOD("set_grid_rows", "rows"), &PhysXCloth3D::set_grid_rows);
	ClassDB::bind_method(D_METHOD("get_grid_rows"), &PhysXCloth3D::get_grid_rows);
	ClassDB::bind_method(D_METHOD("set_grid_size", "size"), &PhysXCloth3D::set_grid_size);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &PhysXCloth3D::get_grid_size);

	ClassDB::bind_method(D_METHOD("set_substeps", "substeps"), &PhysXCloth3D::set_substeps);
	ClassDB::bind_method(D_METHOD("get_substeps"), &PhysXCloth3D::get_substeps);
	ClassDB::bind_method(D_METHOD("set_stiffness", "stiffness"), &PhysXCloth3D::set_stiffness);
	ClassDB::bind_method(D_METHOD("get_stiffness"), &PhysXCloth3D::get_stiffness);
	ClassDB::bind_method(D_METHOD("set_shear_stiffness", "stiffness"), &PhysXCloth3D::set_shear_stiffness);
	ClassDB::bind_method(D_METHOD("get_shear_stiffness"), &PhysXCloth3D::get_shear_stiffness);
	ClassDB::bind_method(D_METHOD("set_bend_stiffness", "stiffness"), &PhysXCloth3D::set_bend_stiffness);
	ClassDB::bind_method(D_METHOD("get_bend_stiffness"), &PhysXCloth3D::get_bend_stiffness);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &PhysXCloth3D::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &PhysXCloth3D::get_damping);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &PhysXCloth3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &PhysXCloth3D::get_density);

	ClassDB::bind_method(D_METHOD("set_wind_enabled", "enabled"), &PhysXCloth3D::set_wind_enabled);
	ClassDB::bind_method(D_METHOD("is_wind_enabled"), &PhysXCloth3D::is_wind_enabled);
	ClassDB::bind_method(D_METHOD("set_wind_area", "path"), &PhysXCloth3D::set_wind_area);
	ClassDB::bind_method(D_METHOD("get_wind_area"), &PhysXCloth3D::get_wind_area);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXCloth3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXCloth3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_flow_path", "path"), &PhysXCloth3D::set_flow_path);
	ClassDB::bind_method(D_METHOD("get_flow_path"), &PhysXCloth3D::get_flow_path);
	ClassDB::bind_method(D_METHOD("set_weather_wind", "enabled"), &PhysXCloth3D::set_weather_wind);
	ClassDB::bind_method(D_METHOD("get_weather_wind"), &PhysXCloth3D::get_weather_wind);
	ClassDB::bind_method(D_METHOD("set_wind_turbulence", "turbulence"), &PhysXCloth3D::set_wind_turbulence);
	ClassDB::bind_method(D_METHOD("get_wind_turbulence"), &PhysXCloth3D::get_wind_turbulence);
	ClassDB::bind_method(D_METHOD("set_drag", "drag"), &PhysXCloth3D::set_drag);
	ClassDB::bind_method(D_METHOD("get_drag"), &PhysXCloth3D::get_drag);
	ClassDB::bind_method(D_METHOD("set_lift", "lift"), &PhysXCloth3D::set_lift);
	ClassDB::bind_method(D_METHOD("set_tear_factor", "factor"), &PhysXCloth3D::set_tear_factor);
	ClassDB::bind_method(D_METHOD("get_tear_factor"), &PhysXCloth3D::get_tear_factor);
	ClassDB::bind_method(D_METHOD("is_tearing"), &PhysXCloth3D::is_tearing);
	ClassDB::bind_method(D_METHOD("get_max_strain"), &PhysXCloth3D::get_max_strain);
	ClassDB::bind_method(D_METHOD("get_lift"), &PhysXCloth3D::get_lift);

	ClassDB::bind_method(D_METHOD("set_pin_mode", "mode"), &PhysXCloth3D::set_pin_mode);
	ClassDB::bind_method(D_METHOD("get_pin_mode"), &PhysXCloth3D::get_pin_mode);
	ClassDB::bind_method(D_METHOD("set_pinned_vertices", "vertices"), &PhysXCloth3D::set_pinned_vertices);
	ClassDB::bind_method(D_METHOD("get_pinned_vertices"), &PhysXCloth3D::get_pinned_vertices);
	ClassDB::bind_method(D_METHOD("set_anchor_path", "path"), &PhysXCloth3D::set_anchor_path);
	ClassDB::bind_method(D_METHOD("get_anchor_path"), &PhysXCloth3D::get_anchor_path);

	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &PhysXCloth3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &PhysXCloth3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PhysXCloth3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PhysXCloth3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &PhysXCloth3D::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &PhysXCloth3D::get_friction);

	ClassDB::bind_method(D_METHOD("set_simulating", "simulating"), &PhysXCloth3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &PhysXCloth3D::is_simulating);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "simulation_mode", PROPERTY_HINT_ENUM, "Auto,GPU,CPU"), "set_simulation_mode", "get_simulation_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "source_mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_source_mesh", "get_source_mesh");
	ADD_GROUP("Grid", "grid_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "grid_columns", PROPERTY_HINT_RANGE, "2,256,1"), "set_grid_columns", "get_grid_columns");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "grid_rows", PROPERTY_HINT_RANGE, "2,256,1"), "set_grid_rows", "get_grid_rows");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "grid_size", PROPERTY_HINT_NONE, "suffix:m"), "set_grid_size", "get_grid_size");
	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "substeps", PROPERTY_HINT_RANGE, "1,40,1"), "set_substeps", "get_substeps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stiffness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_stiffness", "get_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shear_stiffness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shear_stiffness", "get_shear_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bend_stiffness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_bend_stiffness", "get_bend_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.001,5,0.001,or_greater,suffix:kg/m²"), "set_density", "get_density");
	ADD_GROUP("Wind", "wind_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_enabled"), "set_wind_enabled", "is_wind_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "wind_area", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Area3D"), "set_wind_area", "get_wind_area");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_turbulence", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_wind_turbulence", "get_wind_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weather_wind"), "set_weather_wind", "get_weather_wind");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "flow", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_flow_path", "get_flow_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_drag", "get_drag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lift", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_lift", "get_lift");
	ADD_GROUP("Tearing", "tear_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tear_factor", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_tear_factor", "get_tear_factor");
	ADD_GROUP("Wetness", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness", "get_wetness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weather_wetting"), "set_weather_wetting", "is_weather_wetting");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_capacity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_water_capacity", "get_water_capacity");
	ADD_GROUP("Burning", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "burnable"), "set_burnable", "is_burnable");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "fire_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFireSpread3D"), "set_fire_path", "get_fire_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flame_spread", PROPERTY_HINT_RANGE, "0,0.5,0.001,suffix:m/s"), "set_flame_spread", "get_flame_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "burn_through_time", PROPERTY_HINT_RANGE, "0.1,30,0.1,suffix:s"), "set_burn_through_time", "get_burn_through_time");
	ADD_GROUP("Pinning", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pin_mode", PROPERTY_HINT_ENUM, "None,Top Edge,Top Corners,Left Edge,Right Edge"), "set_pin_mode", "get_pin_mode");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "pinned_vertices"), "set_pinned_vertices", "get_pinned_vertices");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "anchor_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_anchor_path", "get_anchor_path");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_friction", "get_friction");

	ADD_SIGNAL(MethodInfo("torn", PropertyInfo(Variant::VECTOR3, "position")));

	BIND_ENUM_CONSTANT(PIN_NONE);
	BIND_ENUM_CONSTANT(PIN_TOP_EDGE);
	BIND_ENUM_CONSTANT(PIN_TOP_CORNERS);
	BIND_ENUM_CONSTANT(PIN_LEFT_EDGE);
	BIND_ENUM_CONSTANT(PIN_RIGHT_EDGE);

	BIND_ENUM_CONSTANT(SIM_AUTO);
	BIND_ENUM_CONSTANT(SIM_GPU);
	BIND_ENUM_CONSTANT(SIM_CPU);
}

PhysXCloth3D::PhysXCloth3D() {
	set_notify_transform(true);
}

PhysXCloth3D::~PhysXCloth3D() {
}

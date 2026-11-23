/**************************************************************************/
/*  snow_bodies_3d.cpp                                                    */
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

#include "snow_bodies_3d.h"

#include "snow_clumps_3d.h"
#include "snow_cover_3d.h"
#include "snow_surface_3d.h"
#include "snow_fall_3d.h"
#include "snow_powder_cloud_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/local_vector.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/animatable_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/physics_material.h"
#include "scene/resources/surface_tool.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/material.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {
const float GRAVITY = 9.81f;
const float WATER_DENSITY = 1000.0f;
const float FUSION = 334000.0f; // J/kg
const float MIN_DEPTH = 0.0005f; // m: less is gone
const double RAY_INTERVAL = 1.0; // s between sky checks
const double GEOMETRY_INTERVAL = 5.0; // s between looks for new meshes under a body
} // namespace

bool SnowBodies3D::is_carrier(const Node *p_node) {
	if (!p_node || p_node->is_in_group(SNAME("snow_ignore"))) {
		return false;
	}
	if (p_node->is_in_group(SNAME("snow_catch")) || p_node->is_in_group(SNAME("snow_roof"))) {
		return true;
	}
	// Things that move by themselves or are moved: rigid bodies (vehicles too), animated bodies, the PhysX vehicles
	// and cloth. Characters only through the group, so a person's materials stay as authored unless asked.
	return Object::cast_to<RigidBody3D>(p_node) || Object::cast_to<AnimatableBody3D>(p_node) ||
			p_node->is_class("PhysXVehicle3D") || p_node->is_class("PhysXMotorcycle3D") || p_node->is_class("PhysXCloth3D");
}

void SnowBodies3D::_consider(Node *p_node) {
	if (auto_track && is_carrier(p_node) && Object::cast_to<Node3D>(p_node)) {
		track(p_node);
	}
}

void SnowBodies3D::_scan(Node *p_node) {
	_consider(p_node);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_scan(p_node->get_child(i));
	}
}

void SnowBodies3D::_on_node_added(Node *p_node) {
	_consider(p_node);
}

void SnowBodies3D::track(Node *p_node) {
	ERR_FAIL_NULL(Object::cast_to<Node3D>(p_node));
	const ObjectID id = p_node->get_instance_id();
	if (bodies.has(id)) {
		return;
	}
	Body b;
	b.id = id;
	if (p_node->is_in_group(SNAME("snow_roof"))) {
		b.roof = true;
		b.roof_phi = roof_friction(p_node->get_meta(SNAME("snow_roof_material"), String()));
		b.roof_heat = MAX(float(p_node->get_meta(SNAME("snow_roof_heat"), 0.0f)), 0.0f);
	}
	bodies.insert(id, b);
}

void SnowBodies3D::untrack(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	HashMap<ObjectID, Body>::Iterator it = bodies.find(p_node->get_instance_id());
	if (!it) {
		return;
	}
	for (const ObjectID &gid : it->value.geometry) {
		if (GeometryInstance3D *g = ObjectDB::get_instance<GeometryInstance3D>(gid)) {
			g->set_instance_shader_parameter(SNAME("snow_body"), Vector4());
		}
	}
	bodies.remove(it);
}

bool SnowBodies3D::is_tracked(Node *p_node) const {
	return p_node && bodies.has(p_node->get_instance_id());
}

TypedArray<Node> SnowBodies3D::get_tracked() const {
	TypedArray<Node> out;
	for (const KeyValue<ObjectID, Body> &kv : bodies) {
		if (Node *n = ObjectDB::get_instance<Node>(kv.key)) {
			out.push_back(n);
		}
	}
	return out;
}

float SnowBodies3D::get_body_depth(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->depth : 0.0f;
}

float SnowBodies3D::get_body_density(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->density : 0.0f;
}

float SnowBodies3D::get_body_wetness(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->wet : 0.0f;
}

void SnowBodies3D::set_body_depth(Node *p_node, float p_depth, float p_density) {
	ERR_FAIL_NULL(p_node);
	track(p_node);
	Body *b = bodies.getptr(p_node->get_instance_id());
	b->depth = CLAMP(p_depth, 0.0f, b->roof ? roof_max_depth : max_depth);
	if (p_density > 0.0f) {
		b->density = p_density;
	}
	b->seen = true;
	if (Node3D *n = Object::cast_to<Node3D>(p_node)) {
		b->last_position = n->get_global_position();
	}
}

void SnowBodies3D::_prepare_geometry(GeometryInstance3D *p_geometry) {
	if (!auto_materials) {
		return;
	}
	// A body's BaseMaterial3D materials learn to show its load. One that had no snow gets snow from the body only
	// (snow_from_weather off), so other instances sharing it, untracked, stay bare as before.
	auto prepare = [](const Ref<Material> &p_material) {
		Ref<BaseMaterial3D> m = p_material;
		if (m.is_null() || m->is_snow_on_body()) {
			return;
		}
		if (!m->get_feature(BaseMaterial3D::FEATURE_SNOW)) {
			m->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
			m->set_snow_from_weather(false);
		}
		m->set_snow_on_body(true);
	};
	prepare(p_geometry->get_material_override());
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_geometry)) {
		for (int s = 0; s < mi->get_surface_override_material_count(); s++) {
			prepare(mi->get_active_material(s));
		}
	} else {
		// Cloth and other generated geometry keep their material in a "material" property.
		bool valid = false;
		const Variant v = p_geometry->get(SNAME("material"), &valid);
		if (valid) {
			prepare(v);
		}
	}
}

void SnowBodies3D::_collect_geometry(Body &r_body, Node *p_node) {
	if (GeometryInstance3D *g = Object::cast_to<GeometryInstance3D>(p_node)) {
		if (!r_body.geometry.has(g->get_instance_id())) {
			r_body.geometry.push_back(g->get_instance_id());
			_prepare_geometry(g);
			r_body.sent = Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		// A carrier inside another keeps its own load.
		if (c != p_node && is_carrier(c) && bodies.has(c->get_instance_id())) {
			continue;
		}
		_collect_geometry(r_body, c);
	}
}

float SnowBodies3D::_exposure(Node3D *p_node) {
	// Is the sky open above the body? A ray up from its top, past itself.
	Ref<World3D> w = get_world_3d();
	if (w.is_null()) {
		return 1.0f;
	}
	PhysicsDirectSpaceState3D *space = PhysicsServer3D::get_singleton()->space_get_direct_state(w->get_space());
	if (!space) {
		return 1.0f;
	}
	Vector3 top = p_node->get_global_position();
	if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node)) {
		const AABB box = p_node->get_global_transform().xform(vi->get_aabb());
		top = Vector3(box.get_center().x, box.position.y + box.size.y, box.get_center().z);
	}
	PhysicsDirectSpaceState3D::RayParameters rp;
	rp.from = top + Vector3(0.0f, 0.05f, 0.0f);
	rp.to = rp.from + Vector3(0.0f, 60.0f, 0.0f);
	if (CollisionObject3D *co = Object::cast_to<CollisionObject3D>(p_node)) {
		rp.exclude.insert(co->get_rid());
	}
	PhysicsDirectSpaceState3D::RayResult rr;
	return space->intersect_ray(rp, rr) ? 0.0f : 1.0f;
}

bool SnowBodies3D::slab_slides(float p_depth, float p_density, float p_wetness, float p_slope, float p_cohesion, float p_friction_deg) {
	// Mohr-Coulomb on the bed (as the cornice's crack, follow-up 7): the load's shear rho g d sin(slope) against the
	// bond c plus friction rho g d cos(slope) tan(phi). Melt water at the bed breaks the bond and lubricates it
	// (wet snow slides off warm roofs at about 25 degrees); phi falls by 10 degrees with wetness (from 25 on a rough
	// surface, from about 10 on sheet metal).
	if (p_depth <= 0.0f || p_slope <= 0.0f) {
		return false;
	}
	const float w = CLAMP(p_wetness, 0.0f, 1.0f);
	const float load = p_density * GRAVITY * p_depth; // Pa
	const float c = p_cohesion * (1.0f - w) * (1.0f - w);
	const float phi = Math::deg_to_rad(MAX(p_friction_deg - 10.0f * w, 3.0f));
	return load * Math::sin(p_slope) > c + load * Math::cos(p_slope) * Math::tan(phi);
}

// Roofs (follow-up 50). Eurocode 1 (EN 1991-1-3) gives the snow a pitched roof holds as a share mu1 of the ground's:
// 0.8 up to 30 degrees, falling linearly to none at 60 (snow does not stay on steeper roofs); Taylor (1985) and
// O'Rourke's roof surveys find slippery roofs shed it at lower pitches, so the bed's friction goes with the roofing:
// about 10 degrees dry on sheet metal, 20 on tiles and 25 on shingles or rough boards.
float SnowBodies3D::roof_shape(float p_slope) {
	const float a = Math::rad_to_deg(MAX(p_slope, 0.0f));
	return a <= 30.0f ? 0.8f : (a >= 60.0f ? 0.0f : 0.8f * (60.0f - a) / 30.0f);
}

float SnowBodies3D::roof_friction(const String &p_material) {
	const String m = p_material.to_lower();
	if (m == "metal" || m == "glass") {
		return 10.0f;
	}
	if (m == "tile" || m == "slate") {
		return 20.0f;
	}
	return 25.0f; // shingle, boards, felt, thatch
}

// What slides off a roof lands below the eave: the slab leaves the eave at the speed it slid down the roof's run,
// v = sqrt(2 g L (sin a - tan phi cos a)), and falls from the eave's height to the ground (a ballistic throw), so a
// steep or slippery roof throws it further out. It lands as a bank along the eave (cones of wind-packed snow into the
// cover, mass kept).
bool SnowBodies3D::_roof_extent(const Body &r_body, const Vector3 &p_down, float p_slope, Vector3 &r_eave, Vector3 &r_across, float &r_half_len, float &r_run) const {
	// The eave: the top's low edge, its length across the slope, and the run up to the ridge.
	AABB box;
	bool any = false;
	for (const ObjectID &gid : r_body.geometry) {
		GeometryInstance3D *g = ObjectDB::get_instance<GeometryInstance3D>(gid);
		if (g && g->is_inside_tree()) {
			const AABB b = g->get_global_transform().xform(g->get_aabb());
			box = any ? box.merge(b) : b;
			any = true;
		}
	}
	if (!any) {
		return false;
	}
	const Vector3 down = p_down.length() > 1e-4f ? p_down.normalized() : Vector3(1.0f, 0.0f, 0.0f);
	r_across = Vector3(-down.z, 0.0f, down.x);
	const Vector3 c = box.get_center();
	const float half_run = 0.5f * Math::abs(box.size.x * down.x) + 0.5f * Math::abs(box.size.z * down.z);
	r_half_len = MAX(0.5f * Math::abs(box.size.x * r_across.x) + 0.5f * Math::abs(box.size.z * r_across.z), 0.5f);
	r_eave = Vector3(c.x, box.position.y, c.z) + down * half_run;
	r_run = 2.0f * half_run / MAX(Math::cos(p_slope), 0.1f); // along the slope
	return true;
}

void SnowBodies3D::_land_pile(const Body &r_body, Node3D *p_node, float p_depth_lost, const Vector3 &p_down, float p_slope) {
	SnowCover3D *cover = SnowCover3D::get_active();
	if (!roof_piles || !cover || p_depth_lost <= 0.0f || r_body.top_area <= 0.0f) {
		return;
	}
	Vector3 eave, across;
	float half_len = 0.5f, run = 1.0f;
	if (!_roof_extent(r_body, p_down, p_slope, eave, across, half_len, run)) {
		return;
	}
	const Vector3 down = p_down.length() > 1e-4f ? p_down.normalized() : Vector3(1.0f, 0.0f, 0.0f);
	const float tphi = Math::tan(Math::deg_to_rad(MAX(r_body.roof_phi - 10.0f * r_body.wet, 3.0f)));
	const float v = Math::sqrt(MAX(2.0f * GRAVITY * run * (Math::sin(p_slope) - tphi * Math::cos(p_slope)), 0.0f));
	if (roof_slab_bodies && is_inside_tree() && get_world_3d().is_valid()) {
		// Follow-up 52: the slab leaves the eave as rigid pieces, row by row as it slides out (each row a chunk's
		// length up the roof leaves that much later), and lands where the pieces come to rest.
		const float mass_all = p_depth_lost * r_body.density * r_body.top_area;
		const float along = CLAMP(slab_chunk_size, 0.4f, 3.0f);
		const int cols = MAX(int(Math::round(2.0f * half_len / along)), 1);
		const int rows = MAX(int(Math::round(run / along)), 1);
		const float each = mass_all / float(cols * rows);
		const Vector3 slope_dir = (down * Math::cos(p_slope) + Vector3(0.0f, -Math::sin(p_slope), 0.0f)).normalized();
		const Vector3 vel = slope_dir * MAX(v, 0.5f);
		const Vector3 up = across.cross(slope_dir).normalized() * (across.cross(slope_dir).y < 0.0f ? -1.0f : 1.0f);
		const float col_w = 2.0f * half_len / cols;
		const float row_l = run / rows;
		uint32_t seed = hash_murmur3_one_32(uint32_t(roof_slides) * 104729u + uint32_t(uint64_t(p_node->get_instance_id())));
		auto rnd = [&seed]() {
			seed = hash_murmur3_one_32(seed);
			return float(seed & 0xffff) / 65535.0f;
		};
		for (int r = 0; r < rows; r++) {
			for (int c = 0; c < cols; c++) {
				PendingChunk pc;
				// The slab parts along its cracks as it tips over the eave: each piece leaves a little apart from
				// its neighbours (smaller than its cell, shifted, turned), with its own speed and spin.
				pc.wait = (r * row_l) / MAX(v, 0.5f) + 0.25f * rnd() + 0.12f * (c & 1);
				const float f = (c + 0.5f) / cols * 2.0f - 1.0f;
				Basis basis(across, up, -slope_dir);
				basis = basis.orthonormalized();
				basis = basis * Basis(Vector3(0.0f, 1.0f, 0.0f), (rnd() - 0.5f) * 0.6f) * Basis(Vector3(0.0f, 0.0f, 1.0f), (rnd() - 0.5f) * 0.3f);
				pc.xform = Transform3D(basis, eave + across * (f * (half_len - 0.5f * col_w) + (rnd() - 0.5f) * 0.15f * col_w) + up * (0.5f * p_depth_lost + 0.03f) - slope_dir * (0.5f * row_l));
				pc.velocity = vel * (0.8f + 0.4f * rnd()) + across * ((rnd() - 0.5f) * 0.8f + 0.25f * f);
				pc.spin = across * ((rnd() - 0.2f) * 2.5f) + up * ((rnd() - 0.5f) * 1.5f);
				pc.size = Vector3(col_w * (0.6f + 0.15f * rnd()), MAX(p_depth_lost, 0.05f) * (0.85f + 0.15f * rnd()), row_l * (0.6f + 0.15f * rnd()));
				pc.mass = each;
				pc.density = MAX(r_body.density, 100.0f);
				pc.dry = r_body.wet < 0.3f && r_body.density < 250.0f;
				pending_chunks.push_back(pc);
			}
		}
		return;
	}
	const float ground = cover->get_ground_height_at(eave);
	const float hgt = MAX(eave.y - ground, 0.0f);
	const float vs = v * Math::sin(p_slope);
	const float t = (-vs + Math::sqrt(vs * vs + 2.0f * GRAVITY * hgt)) / GRAVITY;
	const float throw_d = v * Math::cos(p_slope) * t;
	const Vector3 land = eave + down * (throw_d + 0.5f);
	// A bank of cones along the eave, 1.5 m wide each side, spaced half that so they blend into one bank; each cone's
	// share and its throw vary a little (a slab breaks up unevenly), with the mass kept.
	const float mass = p_depth_lost * r_body.density * r_body.top_area; // kg
	const float r = 1.5f;
	const int k = MAX(int(Math::ceil(4.0f * half_len / r)), 1);
	const float rho = MAX(r_body.density, 100.0f);
	uint32_t seed = hash_murmur3_one_32(uint32_t(roof_slides) * 7919u + uint32_t(uint64_t(p_node->get_instance_id())));
	auto rnd = [&seed]() {
		seed = hash_murmur3_one_32(seed);
		return float(seed & 0xffff) / 65535.0f;
	};
	LocalVector<float> share;
	LocalVector<float> reach;
	share.resize(k);
	reach.resize(k);
	float total = 0.0f;
	for (int j = 0; j < k; j++) {
		share[j] = 0.7f + 0.6f * rnd();
		reach[j] = (rnd() - 0.5f) * 0.6f; // m
		total += share[j];
	}
	for (int j = 0; j < k; j++) {
		const float f = k > 1 ? (float(j) / (k - 1) - 0.5f) * 2.0f * MAX(half_len - 0.5f * r, 0.0f) : 0.0f;
		const float each = mass * share[j] / total / rho; // m3
		const float h = 3.0f * each / (float(Math::PI) * r * r);
		cover->add_drift(land + across * f + down * reach[j], r, h, rho);
	}
	roof_landed += mass;
}

void SnowBodies3D::_measure(Body &r_body, Node3D *p_node) {
	// The body's footprint from above: its meshes' bounds in world space.
	AABB box;
	bool any = false;
	for (const ObjectID &gid : r_body.geometry) {
		GeometryInstance3D *g = ObjectDB::get_instance<GeometryInstance3D>(gid);
		if (!g || !g->is_inside_tree()) {
			continue;
		}
		const AABB b = g->get_global_transform().xform(g->get_aabb());
		box = any ? box.merge(b) : b;
		any = true;
	}
	if (!any) {
		r_body.top_area = 0.0f;
		r_body.top_offset = Vector3();
		return;
	}
	r_body.top_area = box.size.x * box.size.z;
	r_body.top_radius = 0.5f * MAX(box.size.x, box.size.z);
	const Vector3 c = box.get_center();
	r_body.top_offset = Vector3(c.x, box.position.y + box.size.y, c.z) - p_node->get_global_position();
}

void SnowBodies3D::_shed(Body &r_body, Node3D *p_node, float p_depth_lost, const Vector3 &p_toward, float p_temperature) {
	// What falls off lands round the body (or past its low edge, for a slab) as clumps, as much as the snow holds
	// together (SnowClumps3D::cohesion); the rest is powder.
	if (!clumps_enabled || p_depth_lost <= 0.0f) {
		return;
	}
	SnowSurface3D *surface = SnowSurface3D::get_active();
	SnowClumps3D *clumps = surface ? surface->get_clumps_node() : nullptr;
	if (!clumps) {
		return;
	}
	const float share = SnowClumps3D::cohesion(r_body.density, r_body.wet, p_temperature);
	const float volume = p_depth_lost * r_body.top_area * share;
	const Vector3 top = p_node->get_global_position() + r_body.top_offset;
	Vector3 at = top;
	float radius = r_body.top_radius + 0.3f;
	if (p_toward != Vector3()) {
		at = top + p_toward * (r_body.top_radius + 0.25f);
		radius = 0.5f * r_body.top_radius;
	}
	clumps_dropped += clumps->drop(at, radius, volume, r_body.wet);
}

void SnowBodies3D::_apply_weight() {
	// The load's weight at the top of the body, every physics step (forces do not carry over).
	for (const KeyValue<ObjectID, Body> &kv : bodies) {
		const Body &b = kv.value;
		if (b.load_mass <= 0.0f) {
			continue;
		}
		RigidBody3D *rb = ObjectDB::get_instance<RigidBody3D>(b.id);
		if (rb && rb->is_inside_tree() && !rb->is_freeze_enabled()) {
			rb->apply_force(Vector3(0.0f, -b.load_mass * GRAVITY, 0.0f), b.top_offset);
		}
	}
}

float SnowBodies3D::get_body_load_mass(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->load_mass : 0.0f;
}

float SnowBodies3D::get_body_top_area(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->top_area : 0.0f;
}

void SnowBodies3D::_step(double p_dt) {
	if (p_dt <= 0.0) {
		return;
	}
	const WorldWeather *weather = WorldContracts::weather();
	const WorldSnow *snow = WorldContracts::snow();
	const WorldFire *fire = WorldContracts::fire();
	const SnowCover3D *cover = SnowCover3D::get_active();
	const double scale = weather->has_provider() ? weather->time_scale() : 1.0;
	const float dt = float(p_dt);
	const float dt_h = float(p_dt * scale / 3600.0);
	const float ddf = cover ? cover->get_melt_temperature_factor() : 0.05f; // mm water / (h C)

	LocalVector<ObjectID> gone;
	for (KeyValue<ObjectID, Body> &kv : bodies) {
		Body &b = kv.value;
		Node3D *n = ObjectDB::get_instance<Node3D>(b.id);
		if (!n || !n->is_inside_tree()) {
			gone.push_back(kv.key);
			continue;
		}
		const Vector3 p = n->get_global_position();
		b.geometry_accum += p_dt;
		if (b.geometry_accum >= GEOMETRY_INTERVAL) {
			b.geometry_accum = 0.0;
			_collect_geometry(b, n);
		}
		_measure(b, n);
		b.ray_accum += p_dt;
		if (b.ray_accum >= RAY_INTERVAL) {
			b.ray_accum = 0.0;
			b.exposure = _exposure(n);
		}
		const float t = weather->has_provider() ? weather->air_temperature_at(p) : -5.0f;
		if (!b.seen) {
			// First sight: a body standing out in the snow carries what the ground round it holds.
			b.seen = true;
			b.last_position = p;
			b.last_velocity = Vector3();
			if (snow->has_provider() && b.exposure > 0.5f) {
				b.depth = CLAMP(snow->depth_at(p), 0.0f, max_depth);
				if (b.roof) {
					// A roof holds its Eurocode share of the ground's snow.
					const float pitch = Math::acos(CLAMP(n->get_global_transform().basis.get_column(1).normalized().y, -1.0f, 1.0f));
					b.depth *= roof_shape(pitch) / 0.8f;
				}
				const float rho = snow->density_at(p);
				b.density = rho > 0.0f ? rho : SnowCover3D::new_snow_density(t);
				b.wet = CLAMP(snow->wetness_at(p), 0.0f, 1.0f);
			}
		}
		const Vector3 v = (p - b.last_position) / dt;
		const Vector3 a = (v - b.last_velocity) / dt;
		b.last_position = p;
		b.last_velocity = v;

		// The top's tilt (a roof face's pitch).
		const Vector3 up_axis = n->get_global_transform().basis.get_column(1).normalized();
		const float tilt = Math::acos(CLAMP(up_axis.y, -1.0f, 1.0f));
		// Falling snow builds the load where the sky is open; fresh snow is light and bright. A roof keeps its
		// Eurocode share of it (none past 60 degrees: it sloughs off as it falls).
		if (weather->has_provider() && b.exposure > 0.0f) {
			const float swe = weather->snowfall_at(p) * dt_h * b.exposure * (b.roof ? roof_shape(tilt) / 0.8f : 1.0f); // mm of water
			if (swe > 0.0f) {
				const float rho_new = SnowCover3D::new_snow_density(t);
				const float add = swe / 1000.0f * WATER_DENSITY / rho_new;
				b.density = (b.depth * b.density + add * rho_new) / MAX(b.depth + add, 1.0e-6f);
				b.depth += add;
				b.albedo = MIN(b.albedo + add * 30.0f, 0.88f); // a centimetre freshens it
			}
		}
		// Wind over the blowing threshold strips it (Li and Pomeroy 1997); wet and settled snow holds on.
		if (weather->has_provider() && b.depth > 0.0f) {
			const Vector3 air = weather->wind_at(p) - v;
			const float u = Vector2(air.x, air.z).length();
			const float ut = SnowFall3D::blowing_threshold(t) * (1.0f + 4.0f * b.wet) * Math::sqrt(MAX(b.density, 50.0f) / 150.0f);
			if (u > ut) {
				b.depth *= Math::exp(-wind_shed_rate * (u - ut) * dt);
			}
		}
		// Jolts and shakes throw it off: braking, bumps, a door slammed, cloth flapping.
		const float g = a.length() / GRAVITY;
		if (g > shake_threshold && b.depth > 0.0f) {
			const float before = b.depth;
			b.depth *= Math::exp(-shake_shed_rate * (g - shake_threshold) * dt);
			_shed(b, n, before - b.depth, Vector3(), t);
		}
		// A tilted top lets the load go as one slab (not cloth: it has no one top).
		if (slide_enabled && b.depth > 0.0f && !n->is_class("PhysXCloth3D")) {
			const Vector3 up = up_axis;
			const float slope = tilt;
			// An ice dam holds the snow behind it while it stands a quarter of the snow's depth or more (its toe is
			// keyed behind the ridge).
			const bool dammed = b.roof && ice_dam_height(b.dam) >= 0.25f * b.depth;
			if (!dammed && slab_slides(b.depth, b.density, b.wet, slope, slab_cohesion, b.roof ? b.roof_phi : 25.0f)) {
				const Vector3 down = Vector3(up.x, 0.0f, up.z).normalized();
				const float lost = b.depth;
				b.depth = 0.0f;
				slabs_slid++;
				if (b.roof) {
					roof_slides++;
					_land_pile(b, n, lost, down, slope);
				}
				_shed(b, n, lost, down, t);
			}
		}
		// Melt: the cover's degree-day factor in a thaw, and fire's heat; melt water wets what is left, which
		// refreezes in the cold.
		float melt = 0.0f; // m of snow
		if (t > 0.0f) {
			melt += ddf * t * dt_h / 1000.0f * WATER_DENSITY / MAX(b.density, 50.0f);
		}
		if (fire->has_provider()) {
			const float heat = MAX(fire->heat_at(p), 0.0f);
			melt += heat * dt / (FUSION * MAX(b.density, 50.0f));
		}
		// A warm roof: the building's heat loss melts the bed once the snow over it insulates it to 0 C, the heat
		// the snow conducts up to the cold air, k (0 - T) / d, being less than what comes through the roof (the
		// warm-roof melt behind ice dams; Tobiasson 1988). The bed's melt water wets it (lubricating the slide).
		if (b.roof && b.roof_heat > 0.0f && b.depth > 0.0f) {
			const float k = SnowCover3D::snow_conductivity(b.density);
			const float up_flux = k * MAX(-t, 0.0f) / MAX(b.depth, 0.01f);
			const float bed = b.roof_heat - up_flux; // W/m2 melting the bed
			if (bed > 0.0f) {
				melt += bed * float(p_dt * scale) / (FUSION * MAX(b.density, 50.0f));
				b.wet = MAX(b.wet, 0.6f);
				if (ice_dams && t < 0.0f) {
					// The bed's water from the whole run reaches the eave; in the cold it freezes there, all of it
					// from about -4 C (the rest drips: the icicles' water).
					Vector3 eave, across;
					float half_len = 0.5f, run = 1.0f;
					const Vector3 dn = Vector3(up_axis.x, 0.0f, up_axis.z);
					if (dn.length() > 1e-3f && _roof_extent(b, dn, tilt, eave, across, half_len, run)) {
						const float water = bed / FUSION * run * float(p_dt * scale); // kg per m of eave
						b.dam += water * CLAMP(-t / 4.0f, 0.0f, 1.0f);
					}
				}
			}
		}
		if (b.roof && b.dam > 0.0f && t > 0.0f) {
			const float h = MAX(ice_dam_height(b.dam) - 0.0003f * t * dt_h, 0.0f);
			b.dam = 917.0f * h * h;
		}
		if (b.roof && (b.dam > 0.0f || b.dam_mesh.is_valid())) {
			const Vector3 dn = Vector3(up_axis.x, 0.0f, up_axis.z);
			if (dn.length() > 1e-3f) {
				_update_dam_mesh(b, dn.normalized(), tilt);
			}
		}
		const float rain = weather->has_provider() ? weather->rainfall_at(p) * dt_h : 0.0f; // mm
		if (b.depth > 0.0f) {
			const float before = b.depth;
			b.depth = MAX(b.depth - melt, 0.0f);
			const float wet_in = (before - b.depth) / MAX(before, 1.0e-4f) + rain / MAX(b.depth * b.density, 1.0f);
			b.wet = CLAMP(b.wet + wet_in, 0.0f, 1.0f);
			if (t < 0.0f) {
				b.wet = MAX(b.wet - 0.1f * -t * dt_h, 0.0f);
			}
			// Settling (towards about 300 kg/m3 over two days) and ageing (albedo falls, faster when wet).
			b.density += (300.0f - b.density) * MIN(dt_h / 48.0f, 1.0f);
			b.albedo = MAX(b.albedo - dt_h * (0.004f + 0.02f * b.wet), 0.55f);
		}
		b.depth = MIN(b.depth, b.roof ? roof_max_depth : max_depth);
		if (b.depth < MIN_DEPTH) {
			b.depth = 0.0f;
			b.wet = 0.0f;
		}
		b.load_mass = b.depth * b.density * b.top_area;
		// Cloth carries it as areal mass (kg/m2) on its solver, where the cloth offers a snow_load.
		if (n->is_class("PhysXCloth3D")) {
			bool has = false;
			n->get(SNAME("snow_load"), &has);
			if (has) {
				n->set(SNAME("snow_load"), b.depth * b.density);
			}
		}
		// Only real changes go to the GPU.
		const Vector4 out(b.depth, b.albedo, b.wet, 1.0f + b.density / 1000.0f);
		if (Math::abs(out.x - b.sent.x) > 0.001f || Math::abs(out.y - b.sent.y) > 0.01f || Math::abs(out.z - b.sent.z) > 0.02f ||
				Math::abs(out.w - b.sent.w) > 0.01f) {
			b.sent = out;
			for (const ObjectID &gid : b.geometry) {
				if (GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(gid)) {
					gi->set_instance_shader_parameter(SNAME("snow_body"), out);
				}
			}
		}
	}
	for (const ObjectID &id : gone) {
		bodies.erase(id);
	}
}

void SnowBodies3D::step(double p_dt) {
	_step(p_dt);
}

// Falling roof slabs (follow-up 52). The slab that slides off leaves the eave as rigid pieces (Bartelt and Lehning's
// slab fractures into blocks; roof avalanches break on the eave and again where they land), each a PhysX body with
// snow's friction; a piece hitting hard (its speed halved from over 3 m/s) breaks into four, a dry one throws up a
// puff of powder (a SnowPowderCloud3D, when Flow is there), and each comes to rest as snow in the cover where it lies
// (add_drift, its mass kept).
void SnowBodies3D::_spawn_chunk(const PendingChunk &p_chunk, float p_size_along) {
	Node *parent = get_parent() ? get_parent() : get_tree()->get_root();
	if (chunk_material.is_null()) {
		chunk_material.instantiate();
		chunk_material->set_albedo(Color(0.9f, 0.92f, 0.95f));
		chunk_material->set_roughness(0.85f);
	}
	RigidBody3D *rb = memnew(RigidBody3D);
	rb->add_to_group(SNAME("snow_ignore"));
	// Not a presser: the piece's snow joins the cover as a heap where it stops, so it does not also stamp a print
	// with a pushed-up rim under itself (those read as sharp spikes in the bank).
	rb->set_meta(SNAME("world_contact_exclude"), true);
	// Its own collision layer (20): it still lands on the ground and on the other pieces, but the downward rays that
	// sample the ground under the snow (collision layer 1) pass through it, so no piece is taken for the ground.
	rb->set_collision_layer(CHUNK_COLLISION_LAYER);
	rb->set_collision_mask(rb->get_collision_mask() | CHUNK_COLLISION_LAYER);
	rb->set_mass(MAX(p_chunk.mass, 0.5f));
	rb->set_linear_damp(0.3f);
	rb->set_angular_damp(1.0f);
	Ref<PhysicsMaterial> pm;
	pm.instantiate();
	pm->set_friction(p_chunk.dry ? 0.8f : 0.6f); // snow blocks on snow (dry snow's internal friction, about 35-40 degrees)
	pm->set_bounce(0.0f);
	rb->set_physics_material_override(pm);
	CollisionShape3D *cs = memnew(CollisionShape3D);
	Ref<BoxShape3D> shape;
	shape.instantiate();
	shape->set_size(p_chunk.size);
	cs->set_shape(shape);
	rb->add_child(cs);
	// The piece's look: a box broken irregularly, its corners and faces pushed in and out by a hash of where they
	// lie (so faces meet at the seams) and of the piece (no two alike).
	Ref<BoxMesh> bm;
	bm.instantiate();
	bm->set_size(p_chunk.size);
	// A clean-broken slab block keeps flat faces: only its eight corners move (an irregular hexahedron, as a block
	// split along fractures is); a roof piece is knocked about, its faces bumpy and its corners rounded off.
	bm->set_subdivide_width(p_chunk.angular ? 0 : 3);
	bm->set_subdivide_height(p_chunk.angular ? 0 : 1);
	bm->set_subdivide_depth(p_chunk.angular ? 0 : 3);
	Array arrays = bm->surface_get_arrays(0);
	PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
	const uint32_t piece = hash_murmur3_one_32(uint32_t(chunks_spawned) * 2654435761u);
	const float amp = 0.12f * MIN(MIN(p_chunk.size.x, p_chunk.size.z), MAX(p_chunk.size.y, 0.1f) * 2.0f);
	for (int v = 0; v < verts.size(); v++) {
		const Vector3 q = verts[v];
		const uint32_t h = hash_murmur3_one_32(uint32_t(int(Math::round(q.x * 100.0f)) * 73856093) ^ uint32_t(int(Math::round(q.y * 100.0f)) * 19349663) ^ uint32_t(int(Math::round(q.z * 100.0f)) * 83492791), piece);
		const float r = float(h & 0xffff) / 65535.0f * 2.0f - 1.0f;
		const Vector3 out = Vector3(q.x / MAX(p_chunk.size.x, 0.01f), q.y / MAX(p_chunk.size.y, 0.01f), q.z / MAX(p_chunk.size.z, 0.01f));
		// Corners and edges knocked off: the further out along two or three axes, the more a point is pulled in.
		const float corner = 4.0f * Math::abs(out.x * out.z) * (0.5f + Math::abs(out.y));
		if (p_chunk.angular) {
			// Each corner its own way along all three axes (a hash per axis), up to a fifth of the block's size.
			const uint32_t h2 = hash_murmur3_one_32(h);
			const uint32_t h3 = hash_murmur3_one_32(h2);
			const Vector3 j(float(h & 0xffff) / 65535.0f - 0.5f, float(h2 & 0xffff) / 65535.0f - 0.5f, float(h3 & 0xffff) / 65535.0f - 0.5f);
			verts.set(v, q + j * p_chunk.size * 0.4f);
			continue;
		}
		verts.set(v, q * (1.0f - 0.22f * corner) + out.normalized() * amp * r);
	}
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = Variant();
	arrays[Mesh::ARRAY_TANGENT] = Variant();
	Ref<SurfaceTool> st;
	st.instantiate();
	st->create_from_arrays(arrays);
	if (p_chunk.angular) {
		// Each triangle its own vertices and a flat smooth group: flat facets, sharp edges.
		st->deindex();
		const Array flat = st->commit_to_arrays();
		const PackedVector3Array fv = flat[Mesh::ARRAY_VERTEX];
		st.instantiate();
		st->begin(Mesh::PRIMITIVE_TRIANGLES);
		st->set_smooth_group(UINT32_MAX);
		for (int v = 0; v < fv.size(); v++) {
			st->add_vertex(fv[v]);
		}
	}
	st->generate_normals();
	Ref<ArrayMesh> am = st->commit();
	am->surface_set_material(0, chunk_material);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(am);
	rb->add_child(mi);
	parent->add_child(rb);
	rb->set_global_transform(p_chunk.xform);
	rb->set_linear_velocity(p_chunk.velocity);
	rb->set_angular_velocity(p_chunk.spin);
	Chunk c;
	c.id = rb->get_instance_id();
	c.mass = p_chunk.mass;
	c.density = p_chunk.density;
	c.size = p_size_along;
	c.last_velocity = p_chunk.velocity;
	c.dry = p_chunk.dry;
	c.deposit = p_chunk.deposit;
	chunks.push_back(c);
	chunks_spawned++;
}

void SnowBodies3D::throw_block(const Transform3D &p_xform, const Vector3 &p_size, const Vector3 &p_velocity, const Vector3 &p_spin, float p_density, bool p_deposit) {
	PendingChunk pc;
	pc.xform = p_xform;
	pc.size = p_size.max(Vector3(0.03f, 0.03f, 0.03f));
	pc.velocity = p_velocity;
	pc.spin = p_spin;
	pc.density = MAX(p_density, 50.0f);
	pc.mass = pc.size.x * pc.size.y * pc.size.z * pc.density;
	pc.deposit = p_deposit;
	pc.angular = true;
	pc.wait = 0.0f;
	pending_chunks.push_back(pc);
	blocks_thrown++;
}

void SnowBodies3D::_deposit_chunk(const Chunk &p_chunk, const Vector3 &p_at) {
	SnowCover3D *cover = SnowCover3D::get_active();
	if (cover) {
		// Broken blocks settle into a rounded heap a little wider than the piece, kept off the bare strip under
		// the eave (anything over a metre above the piece) and tapered toward it.
		const float r = MAX(1.4f * p_chunk.size, 0.8f);
		const float vol = p_chunk.mass / MAX(p_chunk.density, 100.0f);
		cover->add_mound(p_at, r, vol, MAX(p_chunk.density, 100.0f), p_at.y + 1.0f, 0.8f);
	}
	roof_landed += p_chunk.mass;
}

void SnowBodies3D::_chunks_step(float p_dt) {
	for (int k = int(pending_chunks.size()) - 1; k >= 0; k--) {
		pending_chunks[k].wait -= p_dt;
		if (pending_chunks[k].wait <= 0.0f) {
			const PendingChunk pc = pending_chunks[k];
			pending_chunks.remove_at_unordered(k);
			_spawn_chunk(pc, pc.size.x);
		}
	}
	LocalVector<Chunk> born;
	for (int k = int(chunks.size()) - 1; k >= 0; k--) {
		Chunk &c = chunks[k];
		RigidBody3D *rb = ObjectDB::get_instance<RigidBody3D>(c.id);
		if (!rb || !rb->is_inside_tree()) {
			roof_landed += c.mass; // freed by someone else: its snow is counted, not placed
			chunks.remove_at_unordered(k);
			continue;
		}
		c.age += p_dt;
		const Vector3 v = rb->get_linear_velocity();
		const float before = c.last_velocity.length();
		const Vector3 at = rb->get_global_position();
		if (!c.broken && c.size > 0.5f && before > 3.0f && v.length() < 0.5f * before) {
			// It hit something hard: four pieces, each half its size, thrown on a little.
			const Transform3D xf = rb->get_global_transform();
			Vector3 sz = Vector3(c.size, 0.0f, c.size) * 0.5f;
			const CollisionShape3D *cs = rb->get_child_count() > 0 ? Object::cast_to<CollisionShape3D>(rb->get_child(0)) : nullptr;
			Ref<BoxShape3D> box = cs ? Ref<BoxShape3D>(cs->get_shape()) : Ref<BoxShape3D>();
			if (box.is_valid()) {
				sz = box->get_size() * Vector3(0.5f, 1.0f, 0.5f);
			}
			for (int q = 0; q < 4; q++) {
				const Vector3 off = xf.basis.xform(Vector3((q & 1) ? 0.25f : -0.25f, 0.0f, (q & 2) ? 0.25f : -0.25f) * Vector3(sz.x * 2.0f, 0.0f, sz.z * 2.0f));
				PendingChunk pc;
				pc.xform = Transform3D(xf.basis, at + off * 0.6f + Vector3(0.0f, 0.05f, 0.0f));
				const Vector3 spread = Vector3(off.x, 0.0f, off.z).normalized() * 1.0f;
				pc.velocity = c.last_velocity * 0.3f + spread;
				pc.spin = Vector3(spread.z, 0.0f, -spread.x) * 2.0f;
				pc.size = Vector3(sz.x, MAX(sz.y * 0.8f, 0.05f), sz.z);
				pc.mass = 0.25f * c.mass;
				pc.density = c.density;
				pc.dry = c.dry;
				pc.wait = 0.0f;
				pending_chunks.push_back(pc);
			}
			if (c.dry) {
				Puff pf;
				pf.position = at;
				pf.rate = 0.05f * c.mass; // kg/s for about a second: a few per cent of the piece lifts as powder
				puffs.push_back(pf);
			}
			rb->queue_free();
			chunks.remove_at_unordered(k);
			continue;
		}
		c.rest = v.length() < 0.3f ? c.rest + p_dt : 0.0f;
		c.last_velocity = v;
		if (c.rest > 0.4f || c.age > 8.0f || at.y < -1000.0f) {
			if (c.deposit) {
				_deposit_chunk(c, at);
			}
			// Its snow is in the cover now, as a heap under it; the block itself stays in sight a while, still and
			// no longer solid, sinking into that heap as broken debris settles (BLOCK_SINK_TIME), then goes.
			const CollisionShape3D *cs = rb->get_child_count() > 0 ? Object::cast_to<CollisionShape3D>(rb->get_child(0)) : nullptr;
			Ref<BoxShape3D> box = cs ? Ref<BoxShape3D>(cs->get_shape()) : Ref<BoxShape3D>();
			const float h = box.is_valid() ? MAX(box->get_size().y, 0.05f) : 0.3f;
			if (at.y > -1000.0f && resting.size() < MAX_RESTING_BLOCKS) {
				rb->set_freeze_mode(RigidBody3D::FREEZE_MODE_STATIC);
				rb->set_freeze_enabled(true);
				rb->set_collision_layer(0);
				rb->set_collision_mask(0);
				Resting rs;
				rs.id = rb->get_instance_id();
				rs.left = h * 1.2f;
				rs.rate = rs.left / BLOCK_SINK_TIME;
				resting.push_back(rs);
			} else {
				rb->queue_free();
			}
			chunks.remove_at_unordered(k);
		}
	}
	for (int k = int(resting.size()) - 1; k >= 0; k--) {
		RigidBody3D *rb = ObjectDB::get_instance<RigidBody3D>(resting[k].id);
		if (!rb || !rb->is_inside_tree()) {
			resting.remove_at_unordered(k);
			continue;
		}
		const float d = MIN(resting[k].rate * p_dt, resting[k].left);
		resting[k].left -= d;
		rb->global_translate(Vector3(0.0f, -d, 0.0f));
		if (resting[k].left <= 0.0f) {
			rb->queue_free();
			resting.remove_at_unordered(k);
		}
	}
	// Puffs: fed to a powder cloud of the bodies' own while they last.
	if (!puffs.is_empty()) {
		if (!puff_cloud) {
			SnowPowderCloud3D *pc = memnew(SnowPowderCloud3D);
			pc->set_max_sources(8);
			pc->set_grid(0.4f, 512);
			add_child(pc);
			puff_cloud = pc;
		}
		LocalVector<SnowPowderCloud3D::Source> src;
		for (int k = int(puffs.size()) - 1; k >= 0; k--) {
			puffs[k].age += p_dt;
			if (puffs[k].age > 1.0f) {
				puffs.remove_at_unordered(k);
				continue;
			}
			SnowPowderCloud3D::Source sr;
			sr.position = puffs[k].position;
			sr.velocity = Vector3(0.0f, 1.0f, 0.0f);
			sr.rate = puffs[k].rate;
			sr.radius = 1.0f;
			src.push_back(sr);
		}
		static_cast<SnowPowderCloud3D *>(puff_cloud)->feed(src);
	} else if (puff_cloud) {
		static_cast<SnowPowderCloud3D *>(puff_cloud)->feed(LocalVector<SnowPowderCloud3D::Source>());
	}
}

// Ice dams (follow-up 52). Melt water from a warm roof's bed runs down under the snow to the eave, where the roof is
// over the cold outside and below 0 C, and freezes there into a ridge of ice (Tobiasson 1988, CRREL; Buska and
// Tobiasson 2001): they form when the heat loss melts the bed with the air below about -4 C, hold the snow behind
// them and feed the icicles. The ridge is drawn twice as wide as high; it melts back in a thaw (about 0.3 mm an hour
// per degree, ice's degree-day factor; Hock 2003).
float SnowBodies3D::ice_dam_height(float p_kg_per_m) {
	return Math::sqrt(MAX(p_kg_per_m, 0.0f) / 917.0f);
}

float SnowBodies3D::get_ice_dam_height(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? ice_dam_height(b->dam) : 0.0f;
}

void SnowBodies3D::_update_dam_mesh(Body &r_body, const Vector3 &p_down, float p_slope) {
	const float h = ice_dam_height(r_body.dam);
	MeshInstance3D *mi = ObjectDB::get_instance<MeshInstance3D>(r_body.dam_mesh);
	if (h < 0.005f) {
		if (mi) {
			mi->queue_free();
		}
		r_body.dam_mesh = ObjectID();
		r_body.dam_drawn = 0.0f;
		return;
	}
	if (mi && Math::abs(h - r_body.dam_drawn) < 0.003f) {
		return;
	}
	Vector3 eave, across;
	float half_len = 0.5f, run = 1.0f;
	if (!_roof_extent(r_body, p_down, p_slope, eave, across, half_len, run)) {
		return;
	}
	if (!mi) {
		mi = memnew(MeshInstance3D);
		mi->set_as_top_level(true);
		add_child(mi);
		r_body.dam_mesh = mi->get_instance_id();
		if (dam_material.is_null()) {
			// Hazy roof ice: impurities and bubbles from refrozen melt water (follow-up 13's icicles).
			dam_material.instantiate();
			dam_material->set_albedo(Color(0.8f, 0.86f, 0.92f));
			dam_material->set_roughness(0.15f);
			dam_material->set_specular(0.6f);
		}
	}
	const Vector3 down = p_down.normalized();
	const Vector3 slope_dir = (down * Math::cos(p_slope) + Vector3(0.0f, -Math::sin(p_slope), 0.0f)).normalized();
	Vector3 up = across.cross(slope_dir).normalized();
	if (up.y < 0.0f) {
		up = -up;
	}
	// A ridge along the eave, its crest a little in from the edge; its height wanders along the eave, as the drip
	// points feed it unevenly.
	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);
	const int segs = MAX(int(2.0f * half_len / 0.2f), 2);
	const Vector3 base = eave - slope_dir * (1.0f * h) + up * 0.02f;
	auto profile = [&](int p_i, int p_k) {
		const float u = float(p_i) / segs * 2.0f - 1.0f;
		const float wob = 0.75f + 0.25f * Math::sin(u * 9.1f + 1.3f) * Math::sin(u * 3.7f);
		const float hh = h * wob;
		const Vector3 o = base + across * (u * half_len);
		switch (p_k) {
			case 0:
				return o - slope_dir * hh; // up-roof foot
			case 1:
				return o + up * hh; // crest
			default:
				return o + slope_dir * hh; // eave foot
		}
	};
	for (int i = 0; i < segs; i++) {
		for (int k = 0; k < 2; k++) {
			const Vector3 a = profile(i, k), b2 = profile(i + 1, k), c = profile(i + 1, k + 1), d = profile(i, k + 1);
			st->add_vertex(a);
			st->add_vertex(b2);
			st->add_vertex(c);
			st->add_vertex(a);
			st->add_vertex(c);
			st->add_vertex(d);
		}
	}
	st->generate_normals();
	Ref<ArrayMesh> m = st->commit();
	m->surface_set_material(0, dam_material);
	mi->set_mesh(m);
	r_body.dam_drawn = h;
}

void SnowBodies3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			if (auto_track) {
				_scan(get_tree()->get_root());
			}
			get_tree()->connect(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added));
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (get_tree()->is_connected(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added))) {
				get_tree()->disconnect(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added));
			}
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (weight_enabled) {
				_apply_weight();
			}
			if (!chunks.is_empty() || !pending_chunks.is_empty() || !puffs.is_empty() || !resting.is_empty()) {
				_chunks_step(float(get_physics_process_delta_time()));
			}
			accum += get_physics_process_delta_time();
			if (accum >= 1.0 / MAX(update_rate, 0.1f)) {
				_step(accum);
				accum = 0.0;
			}
		} break;
	}
}

void SnowBodies3D::set_update_rate(float p_rate) {
	update_rate = CLAMP(p_rate, 0.1f, 120.0f);
}

void SnowBodies3D::set_shake_threshold(float p_g) {
	shake_threshold = MAX(p_g, 0.0f);
}

void SnowBodies3D::set_shake_shed_rate(float p_rate) {
	shake_shed_rate = MAX(p_rate, 0.0f);
}

void SnowBodies3D::set_wind_shed_rate(float p_rate) {
	wind_shed_rate = MAX(p_rate, 0.0f);
}

void SnowBodies3D::set_max_depth(float p_depth) {
	max_depth = MAX(p_depth, 0.0f);
}

void SnowBodies3D::_bind_methods() {
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("is_carrier", "node"), &SnowBodies3D::is_carrier);
	ClassDB::bind_method(D_METHOD("track", "node"), &SnowBodies3D::track);
	ClassDB::bind_method(D_METHOD("untrack", "node"), &SnowBodies3D::untrack);
	ClassDB::bind_method(D_METHOD("is_tracked", "node"), &SnowBodies3D::is_tracked);
	ClassDB::bind_method(D_METHOD("get_tracked_count"), &SnowBodies3D::get_tracked_count);
	ClassDB::bind_method(D_METHOD("get_tracked"), &SnowBodies3D::get_tracked);
	ClassDB::bind_method(D_METHOD("get_body_depth", "node"), &SnowBodies3D::get_body_depth);
	ClassDB::bind_method(D_METHOD("get_body_density", "node"), &SnowBodies3D::get_body_density);
	ClassDB::bind_method(D_METHOD("get_body_wetness", "node"), &SnowBodies3D::get_body_wetness);
	ClassDB::bind_method(D_METHOD("set_body_depth", "node", "depth", "density"), &SnowBodies3D::set_body_depth, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("step", "delta"), &SnowBodies3D::step);
	ClassDB::bind_method(D_METHOD("get_body_load_mass", "node"), &SnowBodies3D::get_body_load_mass);
	ClassDB::bind_method(D_METHOD("get_body_top_area", "node"), &SnowBodies3D::get_body_top_area);
	ClassDB::bind_method(D_METHOD("get_clumps_dropped"), &SnowBodies3D::get_clumps_dropped);
	ClassDB::bind_method(D_METHOD("get_slabs_slid"), &SnowBodies3D::get_slabs_slid);
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("slab_slides", "depth", "density", "wetness", "slope", "cohesion", "friction"), &SnowBodies3D::slab_slides, DEFVAL(25.0f));
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("roof_shape", "slope"), &SnowBodies3D::roof_shape);
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("roof_friction", "material"), &SnowBodies3D::roof_friction);
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("ice_dam_height", "kg_per_m"), &SnowBodies3D::ice_dam_height);
	ClassDB::bind_method(D_METHOD("get_ice_dam_height", "node"), &SnowBodies3D::get_ice_dam_height);
	ClassDB::bind_method(D_METHOD("get_falling_chunks"), &SnowBodies3D::get_falling_chunks);
	ClassDB::bind_method(D_METHOD("throw_block", "transform", "size", "velocity", "spin", "density", "deposit"), &SnowBodies3D::throw_block);
	ClassDB::bind_method(D_METHOD("get_blocks_thrown"), &SnowBodies3D::get_blocks_thrown);
	ClassDB::bind_method(D_METHOD("get_chunks_spawned"), &SnowBodies3D::get_chunks_spawned);
	ClassDB::bind_method(D_METHOD("set_roof_slab_bodies", "enable"), &SnowBodies3D::set_roof_slab_bodies);
	ClassDB::bind_method(D_METHOD("is_roof_slab_bodies_enabled"), &SnowBodies3D::is_roof_slab_bodies_enabled);
	ClassDB::bind_method(D_METHOD("set_ice_dams", "enable"), &SnowBodies3D::set_ice_dams);
	ClassDB::bind_method(D_METHOD("is_ice_dams_enabled"), &SnowBodies3D::is_ice_dams_enabled);
	ClassDB::bind_method(D_METHOD("get_roof_slides"), &SnowBodies3D::get_roof_slides);
	ClassDB::bind_method(D_METHOD("get_roof_landed"), &SnowBodies3D::get_roof_landed);
	ClassDB::bind_method(D_METHOD("set_roof_piles", "enable"), &SnowBodies3D::set_roof_piles);
	ClassDB::bind_method(D_METHOD("is_roof_piles_enabled"), &SnowBodies3D::is_roof_piles_enabled);
	ClassDB::bind_method(D_METHOD("set_roof_max_depth", "meters"), &SnowBodies3D::set_roof_max_depth);
	ClassDB::bind_method(D_METHOD("get_roof_max_depth"), &SnowBodies3D::get_roof_max_depth);
	ClassDB::bind_method(D_METHOD("set_weight_enabled", "enable"), &SnowBodies3D::set_weight_enabled);
	ClassDB::bind_method(D_METHOD("is_weight_enabled"), &SnowBodies3D::is_weight_enabled);
	ClassDB::bind_method(D_METHOD("set_slide_enabled", "enable"), &SnowBodies3D::set_slide_enabled);
	ClassDB::bind_method(D_METHOD("is_slide_enabled"), &SnowBodies3D::is_slide_enabled);
	ClassDB::bind_method(D_METHOD("set_clumps_enabled", "enable"), &SnowBodies3D::set_clumps_enabled);
	ClassDB::bind_method(D_METHOD("is_clumps_enabled"), &SnowBodies3D::is_clumps_enabled);
	ClassDB::bind_method(D_METHOD("set_slab_cohesion", "pascals"), &SnowBodies3D::set_slab_cohesion);
	ClassDB::bind_method(D_METHOD("get_slab_cohesion"), &SnowBodies3D::get_slab_cohesion);

	ClassDB::bind_method(D_METHOD("set_update_rate", "rate"), &SnowBodies3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &SnowBodies3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_shake_threshold", "g"), &SnowBodies3D::set_shake_threshold);
	ClassDB::bind_method(D_METHOD("get_shake_threshold"), &SnowBodies3D::get_shake_threshold);
	ClassDB::bind_method(D_METHOD("set_shake_shed_rate", "rate"), &SnowBodies3D::set_shake_shed_rate);
	ClassDB::bind_method(D_METHOD("get_shake_shed_rate"), &SnowBodies3D::get_shake_shed_rate);
	ClassDB::bind_method(D_METHOD("set_wind_shed_rate", "rate"), &SnowBodies3D::set_wind_shed_rate);
	ClassDB::bind_method(D_METHOD("get_wind_shed_rate"), &SnowBodies3D::get_wind_shed_rate);
	ClassDB::bind_method(D_METHOD("set_max_depth", "depth"), &SnowBodies3D::set_max_depth);
	ClassDB::bind_method(D_METHOD("get_max_depth"), &SnowBodies3D::get_max_depth);
	ClassDB::bind_method(D_METHOD("set_auto_materials", "enable"), &SnowBodies3D::set_auto_materials);
	ClassDB::bind_method(D_METHOD("is_auto_materials"), &SnowBodies3D::is_auto_materials);
	ClassDB::bind_method(D_METHOD("set_auto_track", "enable"), &SnowBodies3D::set_auto_track);
	ClassDB::bind_method(D_METHOD("is_auto_track"), &SnowBodies3D::is_auto_track);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.1,120,0.1,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shake_threshold", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:g"), "set_shake_threshold", "get_shake_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shake_shed_rate", PROPERTY_HINT_RANGE, "0,20,0.01"), "set_shake_shed_rate", "get_shake_shed_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_shed_rate", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_wind_shed_rate", "get_wind_shed_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_depth", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_max_depth", "get_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_materials"), "set_auto_materials", "is_auto_materials");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_track"), "set_auto_track", "is_auto_track");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weight_enabled"), "set_weight_enabled", "is_weight_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slide_enabled"), "set_slide_enabled", "is_slide_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "roof_piles"), "set_roof_piles", "is_roof_piles_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "roof_slab_bodies"), "set_roof_slab_bodies", "is_roof_slab_bodies_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ice_dams"), "set_ice_dams", "is_ice_dams_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roof_max_depth", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m"), "set_roof_max_depth", "get_roof_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clumps_enabled"), "set_clumps_enabled", "is_clumps_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slab_cohesion", PROPERTY_HINT_RANGE, "0,2000,1,suffix:Pa"), "set_slab_cohesion", "get_slab_cohesion");
}

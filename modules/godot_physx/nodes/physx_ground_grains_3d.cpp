/**************************************************************************/
/*  physx_ground_grains_3d.cpp                                            */
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


#include "physx_ground_grains_3d.h"

#include "core/config/engine.h"

#include "../particles/mpm_fluid_solver.h"

#include "core/math/random_number_generator.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "modules/ground/ground_surface_3d.h"

PhysXGroundGrains3D::PhysXGroundGrains3D() {
	// Sand: 33 degrees, dry. An 8 x 4 x 10 m box (reaching 3 m further the way sand is thrown) at 6.25 cm cells: a particle stands for 3 cl of sand,
	// drawn as a 3 cm clump.
	set_friction(33.0f);
	set_density(1600.0f);
	set_particle_size(0.03f);
	set_particle_count(20000);
	set_spawn_on_ready(false);
	set_mpm_domain_size(Vector3(8, 4, 10));
	set_mpm_grid_resolution(128);
	mpm_free_flight = true;
	mpm_physics_tick = true;
}

// A clump of sand in flight: thrown sand dilates to a few percent solids and its fine grains make a roost
// all but opaque, which no count of real-size grains can draw. A particle's 3 cl is drawn as a loose cloud
// of 56 specks of 2.5 to 6 mm over ~12 cm, so the spray reads as a sheet of sand rather than as pebbles. Jittered octahedra; the render shader turns and scales each instance by its slot, so no two
// clouds read alike. Grains are deposited back into the field when they land, so the cloud is never seen
// at rest.
Ref<Mesh> PhysXGroundGrains3D::_make_mpm_particle_mesh() {
	static const float octa[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const int tri[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
	const float s = get_particle_size();
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->set_seed(7);
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedInt32Array indices;
	for (int grain = 0; grain < 56; grain++) {
		const Vector3 at = Vector3(rng->randfn(0, 1), rng->randfn(0, 1), rng->randfn(0, 1)) * (1.2f * s);
		const float r = s * rng->randf_range(0.04f, 0.1f);
		const Basis turn = Basis(Vector3(rng->randf_range(-1, 1), rng->randf_range(-1, 1), rng->randf_range(-1, 1)).normalized(), rng->randf_range(0.0f, Math::TAU));
		const int first = verts.size();
		for (int v = 0; v < 6; v++) {
			const Vector3 d = turn.xform(Vector3(octa[v][0], octa[v][1], octa[v][2]));
			verts.push_back(at + d * r * rng->randf_range(0.7f, 1.3f));
			normals.push_back(d);
		}
		for (int t = 0; t < 8; t++) {
			indices.push_back(first + tri[t][0]);
			indices.push_back(first + tri[t][2]);
			indices.push_back(first + tri[t][1]);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<Material> m = grain_material;
	if (m.is_null()) {
		Ref<StandardMaterial3D> sand;
		sand.instantiate();
		sand->set_albedo(Color(0.78f, 0.68f, 0.5f));
		sand->set_roughness(0.95f);
		m = sand;
	}
	mesh->surface_set_material(0, m);
	return mesh;
}

GroundSurface3D *PhysXGroundGrains3D::_ground() const {
	if (!ground_surface.is_empty()) {
		return Object::cast_to<GroundSurface3D>(get_node_or_null(ground_surface));
	}
	return GroundSurface3D::get_active();
}

void PhysXGroundGrains3D::_sync_capacity(int p_capacity) {
	if ((int)alive.size() == p_capacity) {
		return;
	}
	alive.resize(p_capacity);
	last.resize(p_capacity);
	age.resize(p_capacity);
	from_y.resize(p_capacity);
	vel.resize(p_capacity);
	seen_age.resize(p_capacity);
	for (int i = 0; i < p_capacity; i++) {
		alive[i] = 0;
		age[i] = 0.0f;
	}
	live = 0;
}

float PhysXGroundGrains3D::get_airborne_volume() const {
	return (float)(live * particle_volume + carry);
}

PackedVector3Array PhysXGroundGrains3D::get_airborne_positions() const {
	PackedVector3Array out;
	for (uint32_t i = 0; i < alive.size(); i++) {
		if (alive[i]) {
			out.push_back(last[i]);
		}
	}
	return out;
}

void PhysXGroundGrains3D::_emit(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity, double p_volume, float p_stream) {
	MPMFluidSolver *mpm_solver = _mpm_ready_for_emit();
	GroundSurface3D *ground = _ground();
	if (!mpm_solver || !ground) {
		return;
	}
	const int capacity = mpm_solver->get_capacity();
	_sync_capacity(capacity);
	const double pvol = mpm_solver->get_particle_volume();
	particle_volume = pvol;
	carry += p_volume;
	const int n = MIN((int)(carry / pvol), capacity);
	if (n <= 0) {
		return;
	}
	carry -= n * pvol;
	// The slots the ring writes next: a grain still in the air there lands where it was last seen first.
	const int head = mpm_solver->get_write_head();
	LocalVector<MPMFluidSolver::EmittedParticle> batch;
	batch.resize(n);
	const float speed = p_velocity.length();
	const float size = get_particle_size();
	for (int k = 0; k < n; k++) {
		const int slot = (head + k) % capacity;
		if (alive[slot]) {
			const float r = 0.5642f * size * 4.0f;
			const double d = pvol / (Math::PI * r * r);
			ground->deposit(last[slot], r, (float)d);
			deposited_volume += pvol;
			alive[slot] = 0;
			live--;
		}
		const float a = Math::randf() * Math::TAU;
		const float rr = p_radius * Math::sqrt(Math::randf());
		// A stream (a wheel's spray) is spread along the path its sand took over the step, as particle
		// emitters do, so a step's grains do not start stacked on the last step's.
		const Vector3 p = p_position + Vector3(Math::cos(a) * rr, size * (0.5f + Math::randf()), Math::sin(a) * rr) + p_velocity * (p_stream * Math::randf());
		const Vector3 jitter = Vector3(Math::randf() * 2.0f - 1.0f, Math::randf() * 2.0f - 1.0f, Math::randf() * 2.0f - 1.0f) * (speed * spray_spread);
		batch[k].position = p;
		batch[k].velocity = p_velocity + jitter;
		alive[slot] = 1;
		last[slot] = p;
		age[slot] = 0.0f;
		from_y[slot] = p.y;
		vel[slot] = batch[k].velocity;
		seen_age[slot] = 0.0f;
		live++;
	}
	mpm_solver->emit(batch);
}

float PhysXGroundGrains3D::kick(const Vector3 &p_position, float p_radius, float p_depth, const Vector3 &p_velocity) {
	GroundSurface3D *ground = _ground();
	if (!ground || p_radius <= 0.0f || p_depth <= 0.0f || !_mpm_ready_for_emit()) {
		return 0.0f;
	}
	const float v = ground->dig(p_position, p_radius, p_depth);
	if (v <= 0.0f) {
		return 0.0f;
	}
	dug_volume += v;
	Vector3 at = p_position;
	at.y = ground->get_surface_height_at(p_position);
	_emit(at, p_radius, p_velocity, v);
	return v;
}

void PhysXGroundGrains3D::_deposit_pass(double p_delta) {
	MPMFluidSolver *mpm_solver = _mpm_ready_for_emit();
	GroundSurface3D *ground = _ground();
	if (!mpm_solver || !ground || live <= 0) {
		return;
	}
	const int capacity = mpm_solver->get_capacity();
	_sync_capacity(capacity);
	const PackedFloat32Array mm = mpm_solver->get_multimesh_buffer();
	const float *b = mm.ptr();
	const bool have = mm.size() >= capacity * 12;
	const double pvol = mpm_solver->get_particle_volume();
	const float size = get_particle_size();
	const Vector3 center = get_global_position();
	const Vector3 half = get_mpm_domain_size() * 0.5f - Vector3(1, 1, 1) * (4.0f * size);
	// Landed grains, gathered per field texel, then deposited one disc a texel.
	const float tex = 0.0625f;
	HashMap<Vector2i, double> land;
	LocalVector<int> killed;
	for (int i = 0; i < capacity; i++) {
		if (!alive[i]) {
			continue;
		}
		age[i] += (float)p_delta;
		// A slot's read back trails its emit by a few frames (more where the solve runs slower than physics):
		// until the grain is seen leaving its emit point (alive 1, last = that point) it cannot land. A read
		// back farther off than the solver's speed cap allows is the slot's previous grain, not yet rewritten.
		if (have && b[i * 12 + 0] * b[i * 12 + 0] + b[i * 12 + 4] * b[i * 12 + 4] + b[i * 12 + 8] * b[i * 12 + 8] > 0.25f) {
			const Vector3 q(b[i * 12 + 3], b[i * 12 + 7], b[i * 12 + 11]);
			if (alive[i] == 2) {
				if (q != last[i] && age[i] > seen_age[i]) {
					vel[i] = (q - last[i]) / (age[i] - seen_age[i]);
					seen_age[i] = age[i];
				}
				last[i] = q;
			} else {
				const float d = q.distance_to(last[i]);
				if (d > 0.002f && d < 25.0f * age[i] + 0.05f) {
					alive[i] = 2;
					last[i] = q;
					seen_age[i] = age[i];
				}
			}
		}
		const Vector3 p = last[i];
		const Vector3 o = (p - center).abs();
		const bool outside = alive[i] == 2 && (o.x > half.x || o.y > half.y || o.z > half.z);
		const bool flown = p.y > from_y[i] + size || age[i] > 0.25f;
		const bool down = alive[i] == 2 && flown && p.y <= ground->get_surface_height_at(p) + 0.5f * size;
		if (!(outside || down || age[i] > max_flight_time)) {
			continue;
		}
		// One leaving the box flies on as a lone grain (no drag, nothing else to meet) to where it comes down.
		const Vector3 at = outside ? _ballistic_landing(p, vel[i]) : p;
		land[Vector2i((int)Math::floor(at.x / tex), (int)Math::floor(at.z / tex))] += pvol;
		killed.push_back(i);
		alive[i] = 0;
		live--;
	}
	if (killed.is_empty()) {
		return;
	}
	mpm_solver->kill(killed);
	const float r = 0.5642f * tex; // a disc of one texel's area
	for (const KeyValue<Vector2i, double> &kv : land) {
		const Vector3 at((kv.key.x + 0.5f) * tex, 0.0f, (kv.key.y + 0.5f) * tex);
		ground->deposit(at, r, (float)(kv.value / (Math::PI * r * r)));
		deposited_volume += kv.value;
	}
}

Vector3 PhysXGroundGrains3D::_ballistic_landing(const Vector3 &p_from, const Vector3 &p_velocity) const {
	GroundSurface3D *ground = _ground();
	Vector3 x = p_from;
	Vector3 v = p_velocity;
	const float g = 9.8f * get_gravity_scale();
	for (int k = 0; k < 300 && ground; k++) {
		const float h = 0.02f;
		v.y -= g * h;
		const Vector3 nx = x + v * h;
		const float s = ground->get_surface_height_at(nx);
		if (nx.y <= s) {
			// Between the steps, where it crossed.
			const float a = x.y - ground->get_surface_height_at(x);
			const float b = nx.y - s;
			return x.lerp(nx, CLAMP(a / MAX(a - b, 1e-6f), 0.0f, 1.0f));
		}
		x = nx;
	}
	return x;
}

void PhysXGroundGrains3D::_follow() {
	if (follow_target.is_empty()) {
		return;
	}
	Node3D *t = Object::cast_to<Node3D>(get_node_or_null(follow_target));
	GroundSurface3D *ground = _ground();
	if (!t || !ground) {
		return;
	}
	Vector3 p = t->get_global_position();
	// The box reaches the way the sand is thrown: centred 3 m behind a spinning car's wheels' throw.
	const float reach = MAX(0.5f * get_mpm_domain_size().z - 2.0f, 0.0f);
	p += throw_dir * reach;
	// Floor a metre under the ground there.
	p.y = ground->get_surface_height_at(p) - 1.0f + 0.5f * get_mpm_domain_size().y;
	set_global_position(p);
}

void PhysXGroundGrains3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process(true);
			}
		} break;
		case NOTIFICATION_PHYSICS_PROCESS: {
			GroundSurface3D *ground = _ground();
			if (!ground) {
				break;
			}
			const double dt = get_physics_process_delta_time();
			_follow();
			_deposit_pass(dt);
			// Sand thrown by spinning wheels: the tread's layer over the slip, dug and thrown (none dug before
			// the solver can take it, so no volume is lost).
			const Array sprays = ground->take_sprays();
			if (!_mpm_ready_for_emit()) {
				break;
			}
			for (int i = 0; i < sprays.size(); i++) {
				const Dictionary d = sprays[i];
				const float width = d["width"];
				const float slip = d["slip_speed"];
				Vector3 at = d["position"];
				const Vector3 throw_v = d["velocity"];
				const Vector3 flat = Vector3(throw_v.x, 0.0f, throw_v.z);
				if (flat.length() > 0.5f) {
					throw_dir = throw_dir.lerp(flat.normalized(), (float)MIN(dt / 0.5, 1.0));
					throw_dir = throw_dir.limit_length(1.0f);
				}
				const float r = 0.5f * width;
				const Vector3 reach = d.has("reach_center") ? (Vector3)d["reach_center"] : at;
				const float reach_r = MAX(d.has("reach_radius") ? (float)d["reach_radius"] : r, r);
				// Past ~10 m/s of slip the tread is a cutter at its full depth: more speed throws the same sand
				// faster rather than more of it.
				const double volume = width * tread_depth * MIN(slip, 10.0f) * dt;
				// Dug over the sand the tread reaches, the loose rim first (dig takes rim before dent): a spinning
				// wheel throws its own bow wave and berm back rather than heaping them round itself.
				const float depth = (float)(volume / (Math::PI * reach_r * reach_r));
				const float v = ground->dig(reach, reach_r, depth);
				if (v > 0.0f) {
					dug_volume += v;
					// Thrown from the sand's surface there (the rut's floor): below it a grain counts as landed.
					at.y = ground->get_surface_height_at(at);
					_emit(at, r * 0.6f, throw_v, v, (float)dt);
				}
			}
		} break;
	}
}

void PhysXGroundGrains3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ground_surface", "path"), &PhysXGroundGrains3D::set_ground_surface);
	ClassDB::bind_method(D_METHOD("get_ground_surface"), &PhysXGroundGrains3D::get_ground_surface);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXGroundGrains3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXGroundGrains3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_tread_depth", "metres"), &PhysXGroundGrains3D::set_tread_depth);
	ClassDB::bind_method(D_METHOD("get_tread_depth"), &PhysXGroundGrains3D::get_tread_depth);
	ClassDB::bind_method(D_METHOD("set_spray_spread", "share"), &PhysXGroundGrains3D::set_spray_spread);
	ClassDB::bind_method(D_METHOD("get_spray_spread"), &PhysXGroundGrains3D::get_spray_spread);
	ClassDB::bind_method(D_METHOD("set_max_flight_time", "seconds"), &PhysXGroundGrains3D::set_max_flight_time);
	ClassDB::bind_method(D_METHOD("get_max_flight_time"), &PhysXGroundGrains3D::get_max_flight_time);
	ClassDB::bind_method(D_METHOD("kick", "position", "radius", "depth", "velocity"), &PhysXGroundGrains3D::kick);
	ClassDB::bind_method(D_METHOD("get_dug_volume"), &PhysXGroundGrains3D::get_dug_volume);
	ClassDB::bind_method(D_METHOD("get_deposited_volume"), &PhysXGroundGrains3D::get_deposited_volume);
	ClassDB::bind_method(D_METHOD("get_airborne_volume"), &PhysXGroundGrains3D::get_airborne_volume);
	ClassDB::bind_method(D_METHOD("get_airborne_count"), &PhysXGroundGrains3D::get_airborne_count);
	ClassDB::bind_method(D_METHOD("get_airborne_positions"), &PhysXGroundGrains3D::get_airborne_positions);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GroundSurface3D"), "set_ground_surface", "get_ground_surface");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tread_depth", PROPERTY_HINT_RANGE, "0,0.05,0.001,suffix:m"), "set_tread_depth", "get_tread_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spray_spread", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_spray_spread", "get_spray_spread");
	ClassDB::bind_method(D_METHOD("set_grain_material", "material"), &PhysXGroundGrains3D::set_grain_material);
	ClassDB::bind_method(D_METHOD("get_grain_material"), &PhysXGroundGrains3D::get_grain_material);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "grain_material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_grain_material", "get_grain_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_flight_time", PROPERTY_HINT_RANGE, "0.1,30,0.1,suffix:s"), "set_max_flight_time", "get_max_flight_time");
}

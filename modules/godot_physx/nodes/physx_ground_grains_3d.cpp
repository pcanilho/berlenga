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

#include "../particles/mpm_fluid_solver.h"
#include "../vehicle/physx_vehicle_3d.h"

#include "core/config/engine.h"
#include "core/math/random_number_generator.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"

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
	// The ground under the grains is the field, which a rut takes below the static collider's surface: grains
	// meeting the collider would rest on it a few centimetres up and never land (PhysXParticleFluid3D turned
	// its auto colliders on, statics included, in 9131450c).
	set_mpm_auto_colliders(false);
}

// A clump of sand in flight: thrown sand dilates to a few percent solids and its fine grains make a roost
// all but opaque, which no count of real-size grains can draw. A particle's 3 cl is drawn as a loose cloud
// of 56 specks of 2.5 to 6 mm over ~12 cm, so the spray reads as a sheet of sand rather than as pebbles. Jittered octahedra; the render shader turns and scales each instance by its slot, so no two
// clouds read alike. Grains are deposited back into the field when they land, so the cloud is never seen
// at rest.
Ref<Mesh> PhysXGroundGrains3D::_make_mpm_particle_mesh() {
	if (water) {
		return PhysXParticleFluid3D::_make_mpm_particle_mesh();
	}
	if (mud) {
		return _make_clod_mesh();
	}
	static const float octa[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const int tri[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
	const float s = get_particle_size();
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->set_seed(7);
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array centre_xy; // UV: the speck's centre x, y; UV2: its centre z and radius, for the shader's
	PackedVector2Array centre_zr; // minimum on-screen size
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
			centre_xy.push_back(Vector2(at.x, at.y));
			centre_zr.push_back(Vector2(at.z, r));
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
	arrays[Mesh::ARRAY_TEX_UV] = centre_xy;
	arrays[Mesh::ARRAY_TEX_UV2] = centre_zr;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<Material> m = grain_material;
	if (m.is_null()) {
		// Dry sand, each speck kept at least ~1.3 pixels across: real specks of a few mm fall under a pixel past a
		// few metres and the spray vanishes (a minimum screen-size clamp, as for particle LOD); a clump seen from
		// afar draws as a few bigger specks, close up as the real ones.
		Ref<Shader> shader;
		{
			shader.instantiate();
			shader->set_code(R"(shader_type spatial;
render_mode cull_disabled;
uniform vec3 albedo : source_color = vec3(0.78, 0.68, 0.5);
uniform float min_pixels = 1.3;
void vertex() {
	vec3 c = vec3(UV.x, UV.y, UV2.x);
	float scale = length(MODEL_MATRIX[0].xyz);
	vec3 cw = (MODEL_MATRIX * vec4(c, 1.0)).xyz;
	float dist = length(cw - CAMERA_POSITION_WORLD);
	float pixel = 2.0 * dist / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float k = clamp(min_pixels * 0.5 * pixel / max(UV2.y * scale, 1e-6), 1.0, 3.0);
	VERTEX = c + (VERTEX - c) * k;
}
void fragment() {
	ALBEDO = albedo;
	ROUGHNESS = 0.95;
}
)");
		}
		Ref<ShaderMaterial> sand;
		sand.instantiate();
		sand->set_shader(shader);
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
			_land(last[slot], 0.5642f * size * 4.0f, pvol);
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
	// Mud clods meeting a vehicle's body stick to it (mud stage 4b): the grains have no colliders.
	LocalVector<PhysXVehicle3D *> vehicles;
	if (mud) {
		PhysXVehicle3D::get_mud_vehicles(vehicles);
	}
	for (int i = 0; i < capacity; i++) {
		if (!alive[i]) {
			continue;
		}
		age[i] += (float)p_delta;
		// A slot's read back trails its emit by a few frames (more where the solve runs slower than physics):
		// until the grain is seen leaving its emit point (alive 1, last = that point) it cannot land. A read
		// back farther off than the solver's speed cap allows is the slot's previous grain, not yet rewritten.
		// (A live slot's transform has a basis; a dead one is zeroed. The liquid's basis carries the drop's size.)
		if (have && b[i * 12 + 0] * b[i * 12 + 0] + b[i * 12 + 4] * b[i * 12 + 4] + b[i * 12 + 8] * b[i * 12 + 8] > (water ? 1e-10f : 0.25f)) {
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
		if (alive[i] == 2 && age[i] > 0.05f && !vehicles.is_empty()) {
			PhysXVehicle3D *hit = nullptr;
			for (PhysXVehicle3D *veh : vehicles) {
				if (veh->mud_box_contains(p)) {
					hit = veh;
					break;
				}
			}
			if (hit) {
				hit->add_mud_splat(p, vel[i], (float)pvol);
				stuck_volume += pvol;
				deposited_volume += pvol;
				killed.push_back(i);
				alive[i] = 0;
				live--;
				continue;
			}
		}
		const Vector3 o = (p - center).abs();
		const bool outside = alive[i] == 2 && (o.x > half.x || o.y > half.y || o.z > half.z);
		const bool flown = p.y > from_y[i] + size || age[i] > 0.25f;
		const bool down = alive[i] == 2 && flown && p.y <= ground->get_surface_height_at(p) + 0.5f * size;
		if (!(outside || down || age[i] > max_flight_time)) {
			continue;
		}
		// One leaving the box flies on as a lone grain (no drag, nothing else to meet) to where it comes down.
		Vector3 at = outside ? _ballistic_landing(p, vel[i]) : p;
		if (!outside && down && vel[i].y < -0.1f) {
			// Seen only after it passed through the surface (read backs come every few frames): back along its
			// path to where it crossed.
			const float under = ground->get_surface_height_at(p) - p.y;
			if (under > 0.0f) {
				at = p - vel[i] * MIN(under / -vel[i].y, 0.2f);
			}
		}
		land[Vector2i((int)Math::floor(at.x / tex), (int)Math::floor(at.z / tex))] += pvol;
		killed.push_back(i);
		alive[i] = 0;
		live--;
	}
	if (killed.is_empty()) {
		return;
	}
	mpm_solver->kill(killed);
	// A disc of one texel's area; a clod of clay splats wider (it does not roll and slump as sand does, and stiff
	// mud holds a heap's walls).
	const float r = mud || water ? 0.12f : 0.5642f * tex;
	for (const KeyValue<Vector2i, double> &kv : land) {
		_land(Vector3((kv.key.x + 0.5f) * tex, 0.0f, (kv.key.y + 0.5f) * tex), r, kv.value);
	}
}

void PhysXGroundGrains3D::_land(const Vector3 &p_at, float p_radius, double p_volume) {
	GroundSurface3D *ground = _ground();
	if (!ground || p_volume <= 0.0) {
		return;
	}
	if (water) {
		ground->add_water(p_at, p_radius, (float)p_volume, water_mud);
	} else {
		ground->deposit(p_at, p_radius, (float)(p_volume / (Math::PI * p_radius * p_radius)));
	}
	deposited_volume += p_volume;
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

void PhysXGroundGrains3D::set_follow_target(const NodePath &p_path) {
	follow_target = p_path;
	// The mud and water children follow the same node (set after they were made, as a script does after add_child).
	PhysXGroundGrains3D *children[2] = { mud_child, water_child };
	for (PhysXGroundGrains3D *c : children) {
		if (!c || !is_inside_tree() || !c->is_inside_tree()) {
			continue;
		}
		Node *t = p_path.is_empty() ? nullptr : get_node_or_null(p_path);
		c->set_follow_target(t ? c->get_path_to(t) : NodePath());
	}
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
				if (mud_spray && !mud && !water) {
					_make_mud_child();
				}
				if (water_spray && !mud && !water) {
					_make_water_child();
				}
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
			if (mud || water) {
				// The mud and water children: their sprays come from the node that made them.
				break;
			}
			// Sand thrown by spinning wheels: the tread's layer over the slip, dug and thrown (none dug before
			// the solver can take it, so no volume is lost).
			const Array sprays = ground->take_sprays();
			if (!_mpm_ready_for_emit()) {
				break;
			}
			for (int i = 0; i < sprays.size(); i++) {
				const Dictionary d = sprays[i];
				// Standing water goes to the water child (mud stage 4c), sticky clay to the mud child (mud stage 4);
				// the rest is sand.
				if (d.has("water") && (float)d["water"] > 0.0f) {
					if (water_child) {
						water_child->_take_spray(d, dt);
					}
				} else if (mud_child && d.has("stickiness") && (float)d["stickiness"] >= 0.3f) {
					mud_child->_take_spray(d, dt);
				} else if (!d.has("volume") || (float)d["volume"] <= 0.0f) {
					_take_spray(d, dt);
				}
			}
		} break;
	}
}

void PhysXGroundGrains3D::set_as_mud() {
	// Wet clay (docs/ideas/mud.md, stage 4): about 1700 kg/m3, a low friction angle and some cohesion (the
	// Drucker-Prager cohesion term), so a clod lands and slumps rather than piling like sand; 3.5 cm particles
	// drawn as clods.
	mud = true;
	mud_spray = false;
	set_friction(20.0f);
	set_density(1700.0f);
	set_grain_cohesion(0.02f);
	set_particle_size(0.035f);
	set_particle_count(8000);
	spray_spread = 0.12f;
	// Sticky clay mostly packs into the tread (stage 3a) rather than flying off it: a slipping tread throws a quarter
	// of the depth it throws of sand.
	tread_depth = 0.002f;
}

void PhysXGroundGrains3D::set_as_water() {
	// Muddy water thrown by tyres (docs/ideas/mud.md, stage 4c): MPM liquid (the solver's fluid, not Drucker-Prager),
	// 2 cm particles (a sheet a few drops thick), drawn as the solver's clear water with its spray and foam.
	water = true;
	mud_spray = false;
	water_spray = false;
	set_density(1000.0f);
	set_particle_size(0.02f);
	set_particle_count(12000);
	spray_spread = 0.15f;
	// Drawn as liquid (the granular base draws particles, no foam).
	set_surface_mesh(true);
	set_foam_enabled(true);
}

// Muddy water's look (round 3): clear water's material (PhysXParticleFluid3D), the light through it absorbed and
// scattered by its clay (Beer-Lambert in the concentration, as the standing water's: a 1 cm e-folding at 3 kg/m3,
// under a millimetre at 40), and past some tens of kg/m3 it reads as the slurry it is, milky brown with the clay's
// colour rather than see-through.
void PhysXGroundGrains3D::_muddy_look() {
	water_mud_drawn = water_mud;
	Ref<StandardMaterial3D> m = get_liquid_material();
	if (m.is_null()) {
		return;
	}
	const float c = water_mud;
	if (c < 0.5f) {
		m->set_albedo(Color(1, 1, 1));
		m->set_transmission(1.0f);
		m->set_transmission_attenuation_color(Color(0.70f, 0.94f, 0.99f));
		m->set_transmission_attenuation_distance(1.0f);
		return;
	}
	const float slurry = c / (c + 30.0f);
	const Color clay = water_mud_color.linear_to_srgb();
	m->set_albedo(Color(1, 1, 1).lerp(clay, slurry));
	m->set_transmission(1.0f - 0.8f * slurry);
	m->set_transmission_attenuation_color(clay);
	m->set_transmission_attenuation_distance(0.03f / (3.0f + c));
}

void PhysXGroundGrains3D::_make_mud_child() {
	if (mud_child) {
		return;
	}
	mud_child = memnew(PhysXGroundGrains3D);
	mud_child->set_as_mud();
	mud_child->set_name("MudSpray");
	_adopt_child(mud_child);
}

void PhysXGroundGrains3D::_make_water_child() {
	if (water_child) {
		return;
	}
	water_child = memnew(PhysXGroundGrains3D);
	water_child->set_as_water();
	water_child->set_name("WaterSpray");
	_adopt_child(water_child);
}

void PhysXGroundGrains3D::_adopt_child(PhysXGroundGrains3D *p_child) {
	p_child->set_mpm_domain_size(get_mpm_domain_size());
	p_child->set_mpm_grid_resolution(get_mpm_grid_resolution());
	p_child->set_max_flight_time(max_flight_time);
	add_child(p_child, false, INTERNAL_MODE_FRONT);
	p_child->set_as_top_level(true);
	if (GroundSurface3D *g = _ground()) {
		p_child->set_ground_surface(p_child->get_path_to(g));
	}
	if (!follow_target.is_empty()) {
		if (Node *t = get_node_or_null(follow_target)) {
			p_child->set_follow_target(p_child->get_path_to(t));
		}
	}
	p_child->set_global_position(get_global_position());
}

// A clod of wet clay: two or three lumpy blobs stuck together, about the particle's size. Real geometry (path tracing
// keeps it), dark and glossy with water.
Ref<Mesh> PhysXGroundGrains3D::_make_clod_mesh() {
	const float s = get_particle_size();
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->set_seed(11);
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedInt32Array indices;
	const int RINGS = 5;
	const int SEGS = 8;
	for (int blob = 0; blob < 3; blob++) {
		const Vector3 at = blob == 0 ? Vector3() : Vector3(rng->randfn(0, 1), rng->randfn(0, 1), rng->randfn(0, 1)).normalized() * (0.45f * s);
		const float r = s * (blob == 0 ? 0.55f : rng->randf_range(0.3f, 0.42f));
		const Vector3 squash(rng->randf_range(0.8f, 1.2f), rng->randf_range(0.7f, 1.0f), rng->randf_range(0.8f, 1.2f));
		const int first = verts.size();
		for (int j = 0; j <= RINGS; j++) {
			const float th = Math::PI * float(j) / RINGS;
			for (int i = 0; i < SEGS; i++) {
				const float ph = Math::TAU * float(i) / SEGS;
				const Vector3 d(Math::sin(th) * Math::cos(ph), Math::cos(th), Math::sin(th) * Math::sin(ph));
				const float lump = (j == 0 || j == RINGS) ? 1.0f : rng->randf_range(0.82f, 1.12f);
				verts.push_back(at + d * squash * (r * lump));
				normals.push_back((d / squash).normalized());
			}
		}
		for (int j = 0; j < RINGS; j++) {
			for (int i = 0; i < SEGS; i++) {
				const int a = first + j * SEGS + i;
				const int b = first + j * SEGS + (i + 1) % SEGS;
				const int c = a + SEGS;
				const int d = b + SEGS;
				indices.push_back(a);
				indices.push_back(c);
				indices.push_back(b);
				indices.push_back(b);
				indices.push_back(c);
				indices.push_back(d);
			}
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
		Ref<StandardMaterial3D> clay;
		clay.instantiate();
		// The clay of the body mud (BaseMaterial3D Mud), wet.
		clay->set_albedo(Color(0.42f, 0.34f, 0.26f) * 0.75f);
		clay->set_roughness(0.3f);
		m = clay;
	}
	mesh->surface_set_material(0, m);
	return mesh;
}

void PhysXGroundGrains3D::_take_spray(const Dictionary &d, double dt) {
	GroundSurface3D *ground = _ground();
	if (!ground || !_mpm_ready_for_emit()) {
		return;
	}
	if (water) {
		// Standing water a tyre throws (mud stage 4c): taken from the field over the reach disc and thrown.
		const double wv = (float)d["water"];
		const Vector3 reach = d.has("reach_center") ? (Vector3)d["reach_center"] : (Vector3)d["position"];
		const float reach_r = MAX(d.has("reach_radius") ? (float)d["reach_radius"] : 0.1f, 0.02f);
		ground->add_water(reach, reach_r, -(float)wv);
		// Muddy water (round 3): the clay it carries mixes into what is already in the air.
		const float mud_c = d.has("water_mud") ? MAX((float)d["water_mud"], 0.0f) : 0.0f;
		const double air = get_airborne_volume();
		water_mud = float((double(water_mud) * air + double(mud_c) * wv) / MAX(air + wv, 1e-12));
		if (mud_c > 0.5f) {
			const Color clay = ground->get_mud_color_at(reach).srgb_to_linear();
			water_mud_color = water_mud_color.lerp(clay, 0.2f);
		}
		if (Math::abs(water_mud - water_mud_drawn) > 0.15f * MAX(water_mud_drawn, 3.0f)) {
			_muddy_look();
		}
		dug_volume += wv;
		_emit(d["position"], 0.5f * (float)d["width"], d["velocity"], wv, (float)dt);
		return;
	}
	if (d.has("volume") && (float)d["volume"] > 0.0f) {
		// Clay flung off a tyre (mud stage 4): thrown as it is. It came out of the field into the tread, so it
		// counts as dug.
		const double fv = (float)d["volume"];
		dug_volume += fv;
		_emit(d["position"], 0.25f * (float)d["width"], d["velocity"], fv, (float)dt);
		return;
	}
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
	// A sideways slide's tread throws less (the lugs drag across) and its sidewall shoves a wedge out.
	const float side = d.has("side_share") ? (float)d["side_share"] : 0.0f;
	const float side_rate = d.has("side_volume_rate") ? (float)d["side_volume_rate"] : 0.0f;
	const double volume = (width * tread_depth * MIN(slip, 10.0f) * (1.0f - side) + side_rate) * dt;
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
	ClassDB::bind_method(D_METHOD("set_mud_spray", "enabled"), &PhysXGroundGrains3D::set_mud_spray);
	ClassDB::bind_method(D_METHOD("is_mud_spray"), &PhysXGroundGrains3D::is_mud_spray);
	ClassDB::bind_method(D_METHOD("get_mud_grains"), &PhysXGroundGrains3D::get_mud_grains);
	ClassDB::bind_method(D_METHOD("set_water_spray", "enabled"), &PhysXGroundGrains3D::set_water_spray);
	ClassDB::bind_method(D_METHOD("is_water_spray"), &PhysXGroundGrains3D::is_water_spray);
	ClassDB::bind_method(D_METHOD("get_water_grains"), &PhysXGroundGrains3D::get_water_grains);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "water_spray"), "set_water_spray", "is_water_spray");
	ClassDB::bind_method(D_METHOD("get_stuck_volume"), &PhysXGroundGrains3D::get_stuck_volume);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mud_spray"), "set_mud_spray", "is_mud_spray");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_flight_time", PROPERTY_HINT_RANGE, "0.1,30,0.1,suffix:s"), "set_max_flight_time", "get_max_flight_time");
}

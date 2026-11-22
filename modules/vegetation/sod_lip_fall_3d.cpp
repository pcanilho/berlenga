/**************************************************************************/
/*  sod_lip_fall_3d.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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


#include "sod_lip_fall_3d.h"

#include "plant_generator.h"
#include "plant_species.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/mesh.h"

namespace {
uint64_t lip_key(const Vector3 &p) {
	return hash_murmur3_one_64(uint64_t(int64_t(Math::round(p.x * 20.0f))), hash_murmur3_one_64(uint64_t(int64_t(Math::round(p.z * 20.0f)))));
}
// A deterministic draw per lip and check, so a run repeats.
float lip_draw(uint64_t p_key, int p_tick) {
	return float(hash_murmur3_one_64(uint64_t(p_tick), p_key) & 0xFFFFFF) / float(0x1000000);
}
} // namespace

Vector3 SodLipFall3D::_camera_position() const {
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		return get_viewport()->get_camera_3d()->get_global_position();
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SodLipFall3D::_break(Node *p_field, int p_species, const Transform3D &p_xf, const Ref<Mesh> &p_mesh, const String &p_cause) {
	VegetationField3D *vf = Object::cast_to<VegetationField3D>(p_field);
	if (!vf) {
		return;
	}
	gone.insert(lip_key(p_xf.origin), true);
	vf->add_plant_removal(p_xf.origin, 0.6f, p_species);
	falls++;
	if (p_mesh.is_null()) {
		return;
	}
	// The block: the sod slab (the mesh's top 20 cm across its whole width; roots and blades ride along).
	const AABB bb = p_mesh->get_aabb();
	const float top = bb.position.y + bb.size.y;
	const Vector3 scale = p_xf.basis.get_scale();
	const Vector3 half(MAX(bb.size.x * 0.5f, 0.05f), 0.1f, MAX(bb.size.z * 0.5f, 0.1f));
	const Vector3 centre(bb.position.x + half.x, MIN(top, 0.02f) - half.y, bb.position.z + half.z);
	RigidBody3D *body = memnew(RigidBody3D);
	body->set_name(vformat("SodBlock%d", falls));
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(p_mesh);
	mi->set_position(-centre);
	body->add_child(mi);
	CollisionShape3D *cs = memnew(CollisionShape3D);
	Ref<BoxShape3D> box;
	box.instantiate();
	box->set_size(half * 2.0f);
	cs->set_shape(box);
	body->add_child(cs);
	body->set_mass(MAX(density * 8.0f * half.x * half.y * half.z * scale.x * scale.y * scale.z, 1.0f));
	add_child(body);
	// The body carries the instance's scale on its children (a rigid body's own basis stays orthonormal).
	mi->set_scale(scale);
	mi->set_position(-centre * scale);
	cs->set_scale(scale);
	const Basis rot = p_xf.basis.orthonormalized();
	body->set_global_transform(Transform3D(rot, p_xf.xform(centre)));
	// A cantilever fails at its root and tips out over the drop: turning about the edge (mesh Z), its outer side down,
	// with a little push out.
	const Vector3 out = rot.get_column(0).normalized();
	const Vector3 along = rot.get_column(2).normalized();
	body->set_angular_velocity(along * 1.2f);
	body->set_linear_velocity(out * 0.4f);
	body->set_meta("sod_cause", p_cause);
	Fallen f;
	f.body = body;
	fallen.push_back(f);
	while (int(fallen.size()) > max_blocks) {
		// The oldest settle where they lie: frozen, still drawn.
		if (fallen[0].body) {
			fallen[0].body->set_freeze_enabled(true);
		}
		fallen.remove_at(0);
	}
}

void SodLipFall3D::check(double p_delta) {
	if (!enabled || !is_inside_tree()) {
		return;
	}
	const Vector3 eye = _camera_position();
	const WorldWeather *wm = WorldContracts::weather();
	const WorldWater *water = WorldContracts::water();
	const int tick = int(Engine::get_singleton()->get_physics_frames());
	// Loads: the bodies of the load group.
	LocalVector<Vector3> loads;
	if (load_group != StringName()) {
		const Vector<Node *> nodes = get_tree()->get_nodes_in_group(load_group);
		for (Node *n : nodes) {
			const Node3D *n3 = Object::cast_to<Node3D>(n);
			if (n3 && n3->is_inside_tree() && n3->get_global_position().distance_to(eye) < range + 2.0f) {
				loads.push_back(n3->get_global_position());
			}
		}
	}
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
	for (int f = 0; f < found.size(); f++) {
		VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
		if (!vf || vf->get_biome().is_null()) {
			continue;
		}
		const TypedArray<PlantSpecies> species = vf->get_biome()->get_species();
		for (int s = 0; s < species.size(); s++) {
			const Ref<PlantSpecies> sp = species[s];
			if (sp.is_null() || int(sp->get_meta("plant_preset", -1)) != PlantGenerator::PRESET_TURF_LIP) {
				continue;
			}
			const Array lips = vf->get_instances_near(s, eye, range);
			for (int i = 0; i < lips.size(); i++) {
				const Dictionary d = lips[i];
				const Transform3D xf = d["transform"];
				const Ref<Mesh> mesh = d["mesh"];
				const uint64_t key = lip_key(xf.origin);
				if (gone.has(key)) {
					continue;
				}
				const Transform3D inv = xf.affine_inverse();
				const AABB bb = mesh.is_valid() ? mesh->get_aabb() : AABB(Vector3(-0.1, -0.2, -0.5), Vector3(0.4, 0.4, 1.0));
				// A body standing on the overhang (over the drop, within the lip's length, up to 2 m over it).
				String cause;
				for (const Vector3 &l : loads) {
					const Vector3 q = inv.xform(l);
					if (q.x > -0.02f && q.x < bb.position.x + bb.size.x + 0.05f && q.z > bb.position.z && q.z < bb.position.z + bb.size.z && q.y > -0.3f && q.y < 2.0f) {
						cause = "load";
						break;
					}
				}
				if (cause.is_empty()) {
					// The water at its foot (0.6 m out over the drop) cutting under it, or the sod soaked in heavy rain:
					// a hazard rate, a deterministic draw per lip and look.
					float hazard = 0.0f;
					const Vector3 out = xf.basis.get_column(0).normalized();
					const Vector3 foot = xf.origin + out * 0.6f;
					if (water) {
						const float wy = water->height_at(foot);
						if (Math::is_finite(wy) && wy > xf.origin.y - 0.7f && wy < xf.origin.y + 0.05f) {
							hazard += 1.0f / undercut_time;
						}
					}
					if (wm && wm->surface_wetness_at(xf.origin) > 0.8f && wm->rainfall_at(xf.origin) > 5.0f) {
						hazard += 1.0f / soaked_time;
					}
					if (hazard > 0.0f && lip_draw(key, tick) < 1.0f - Math::exp(-hazard * float(p_delta))) {
						cause = hazard >= 1.0f / undercut_time ? "undercut" : "soaked";
					}
				}
				if (!cause.is_empty()) {
					_break(vf, s, xf, mesh, cause);
				}
			}
		}
	}
}

bool SodLipFall3D::break_at(const Vector3 &p_position) {
	if (!is_inside_tree()) {
		return false;
	}
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
	float best = 1.0f;
	VegetationField3D *bf = nullptr;
	int bs = -1;
	Transform3D bx;
	Ref<Mesh> bm;
	for (int f = 0; f < found.size(); f++) {
		VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
		if (!vf || vf->get_biome().is_null()) {
			continue;
		}
		const TypedArray<PlantSpecies> species = vf->get_biome()->get_species();
		for (int s = 0; s < species.size(); s++) {
			const Ref<PlantSpecies> sp = species[s];
			if (sp.is_null() || int(sp->get_meta("plant_preset", -1)) != PlantGenerator::PRESET_TURF_LIP) {
				continue;
			}
			const Array lips = vf->get_instances_near(s, p_position, 1.0f);
			for (int i = 0; i < lips.size(); i++) {
				const Dictionary d = lips[i];
				const Transform3D xf = d["transform"];
				const float dd = Vector2(xf.origin.x - p_position.x, xf.origin.z - p_position.z).length();
				if (dd < best && !gone.has(lip_key(xf.origin))) {
					best = dd;
					bf = vf;
					bs = s;
					bx = xf;
					bm = d["mesh"];
				}
			}
		}
	}
	if (!bf) {
		return false;
	}
	_break(bf, bs, bx, bm, "asked");
	return true;
}

Array SodLipFall3D::get_blocks() const {
	Array out;
	for (const Fallen &f : fallen) {
		if (f.body) {
			out.push_back(f.body);
		}
	}
	return out;
}

void SodLipFall3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			check_left -= get_physics_process_delta_time();
			if (check_left <= 0.0) {
				check_left = 0.5;
				check(0.5);
			}
		} break;
	}
}

void SodLipFall3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("check", "delta"), &SodLipFall3D::check);
	ClassDB::bind_method(D_METHOD("break_at", "position"), &SodLipFall3D::break_at);
	ClassDB::bind_method(D_METHOD("get_fall_count"), &SodLipFall3D::get_fall_count);
	ClassDB::bind_method(D_METHOD("get_blocks"), &SodLipFall3D::get_blocks);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SodLipFall3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SodLipFall3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &SodLipFall3D::set_range);
	ClassDB::bind_method(D_METHOD("get_range"), &SodLipFall3D::get_range);
	ClassDB::bind_method(D_METHOD("set_load_group", "group"), &SodLipFall3D::set_load_group);
	ClassDB::bind_method(D_METHOD("get_load_group"), &SodLipFall3D::get_load_group);
	ClassDB::bind_method(D_METHOD("set_undercut_time", "seconds"), &SodLipFall3D::set_undercut_time);
	ClassDB::bind_method(D_METHOD("get_undercut_time"), &SodLipFall3D::get_undercut_time);
	ClassDB::bind_method(D_METHOD("set_soaked_time", "seconds"), &SodLipFall3D::set_soaked_time);
	ClassDB::bind_method(D_METHOD("get_soaked_time"), &SodLipFall3D::get_soaked_time);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &SodLipFall3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &SodLipFall3D::get_density);
	ClassDB::bind_method(D_METHOD("set_max_blocks", "count"), &SodLipFall3D::set_max_blocks);
	ClassDB::bind_method(D_METHOD("get_max_blocks"), &SodLipFall3D::get_max_blocks);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "1,500,1,suffix:m"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "load_group"), "set_load_group", "get_load_group");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "undercut_time", PROPERTY_HINT_RANGE, "1,100000,1,suffix:s"), "set_undercut_time", "get_undercut_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soaked_time", PROPERTY_HINT_RANGE, "1,100000,1,suffix:s"), "set_soaked_time", "get_soaked_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "100,3000,10,suffix:kg/m³"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_blocks", PROPERTY_HINT_RANGE, "1,256,1"), "set_max_blocks", "get_max_blocks");
}

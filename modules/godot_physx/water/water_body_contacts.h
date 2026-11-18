/**************************************************************************/
/*  water_body_contacts.h                                                 */
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


#pragma once

#include "modules/world/world_contracts.h"
#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"

// Bodies the world feels disturb the water (docs/ideas/world-links.md, contact 4): every Node3D in world_contact
// (characters, vehicle wheels, props) pushes the ripple simulation as a PhysXBuoyancy3D does (a sphere, stronger the
// faster it moves through the water, a dimple at rest), and one falling in through the surface throws an impulse.
// Bodies with a PhysXBuoyancy3D of their own are left to it. Size: the trample_radius / touch_radius metas, else
// the first collision shape, as the grass and ground read them.
class WaterBodyContacts {
	struct Seen {
		Vector3 position;
		bool submitted = false;
	};
	HashMap<ObjectID, Seen> seen;

	static Vector2 _extent(const Node3D *p_node) {
		Vector2 e = WorldContracts::body_extent(p_node, 0.3f);
		if (p_node->has_meta(SNAME("trample_radius")) && p_node->has_meta(SNAME("deform_wheel_radius"))) {
			e.y = p_node->get_meta(SNAME("deform_wheel_radius")); // a wheel reaches down its radius
		}
		return e;
	}

	static bool _floats_itself(const Node *p_node) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			if (p_node->get_child(i)->is_class("PhysXBuoyancy3D")) {
				return true;
			}
		}
		return false;
	}

public:
	static constexpr float REFERENCE_SPEED = 2.0f; // m/s through the water for a full-strength wake
	static constexpr float REST_STRENGTH = 0.1f;

	// p_height(pos) -> the surface height (NaN where there is none); p_flow(pos) -> the water's velocity;
	// p_submit(owner, pos, radius, strength), p_clear(owner), p_impulse(pos, radius, strength).
	template <typename H, typename F, typename S, typename C, typename I>
	void update(SceneTree *p_tree, double p_delta, const H &p_height, const F &p_flow, const S &p_submit, const C &p_clear, const I &p_impulse) {
		if (!p_tree || p_delta <= 0.0) {
			return;
		}
		HashMap<ObjectID, Seen> next;
		const Vector<Node *> nodes = p_tree->get_nodes_in_group(SNAME("world_contact"));
		for (Node *n : nodes) {
			const Node3D *n3 = Object::cast_to<Node3D>(n);
			if (!n3 || !n3->is_visible_in_tree() || next.has(n3->get_instance_id()) || _floats_itself(n3)) {
				continue;
			}
			const ObjectID id = n3->get_instance_id();
			const int owner = int(uint64_t(id));
			const Vector3 pos = n3->get_global_position();
			const Seen *last = seen.getptr(id);
			Vector3 vel = last ? (pos - last->position) / float(p_delta) : Vector3();
			if (vel.length_squared() > 30.0f * 30.0f) {
				vel = Vector3(); // teleported
			}
			Seen s;
			s.position = pos;
			const float surface = p_height(pos);
			const Vector2 ext = _extent(n3);
			if (Math::is_finite(surface)) {
				// In contact while the surface lies across the body; fading over its radius beyond.
				const float below = pos.y - ext.y - surface; // its underside over the water
				const float above = surface - (pos.y + ext.y); // the water over its top
				const float gap = MAX(below, above);
				const float proximity = CLAMP(1.0f - gap / MAX(ext.x, 0.05f), 0.0f, 1.0f);
				if (proximity > 0.0f) {
					const Vector3 rel = vel - p_flow(pos);
					const float strength = CLAMP(REST_STRENGTH + (1.0f - REST_STRENGTH) * rel.length() / REFERENCE_SPEED, REST_STRENGTH, 1.0f) * proximity;
					p_submit(owner, Vector3(pos.x, surface, pos.z), ext.x, strength);
					s.submitted = true;
					// Falling in: its underside crossed the surface going down this step.
					if (last && vel.y < -1.0f && last->position.y - ext.y > surface && below <= 0.0f) {
						p_impulse(Vector3(pos.x, surface, pos.z), ext.x, CLAMP(-vel.y / 5.0f, 0.0f, 1.0f));
					}
				}
			}
			if (!s.submitted && last && last->submitted) {
				p_clear(owner);
			}
			next.insert(id, s);
		}
		// Gone from the group or the tree.
		for (const KeyValue<ObjectID, Seen> &kv : seen) {
			if (kv.value.submitted && !next.has(kv.key)) {
				p_clear(int(uint64_t(kv.key)));
			}
		}
		seen = next;
	}
};

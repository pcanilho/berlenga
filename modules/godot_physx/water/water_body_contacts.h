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
		bool foamed = false;
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

	// White water churned in where a body breaks the surface (docs/ideas/water.md "White water round bodies in the
	// water"): air is entrained once the water runs past the body fast for its width, the Froude number
	// Fr = U / sqrt(g D) past FOAM_FR_ONSET (bow wave spilling, lee cavity ventilating; Ageorges, Peixinho and Perret
	// 2019 see the regime change at Fr about 1.2), full by FOAM_FR_FULL. Slow wading makes none.
	static constexpr float FOAM_FR_ONSET = 0.8f;
	static constexpr float FOAM_FR_FULL = 2.0f;
	float foam_rate = 1.0f; // foam per second at full churn (1 = fully white in a second); 0 = none

	// The foam rate (per second) and patch radius (m) for a body of radius p_radius with the water passing it at
	// p_speed (m/s): zero below the onset.
	static Vector2 foam_for(float p_speed, float p_radius, float p_gravity, float p_rate) {
		const float fr = p_speed / Math::sqrt(MAX(p_gravity * 2.0f * MAX(p_radius, 0.02f), 1e-4f));
		const float churn = Math::smoothstep(FOAM_FR_ONSET, FOAM_FR_FULL, fr);
		// The run-up and lee cavity grow as Fr^2 (Chaplin and Teigen 2003): the patch from the body's radius to twice it.
		return Vector2(p_rate * churn, p_radius * (1.0f + churn));
	}

	// p_height(pos) -> the surface height (NaN where there is none); p_flow(pos) -> the water's velocity;
	// p_rise(pos) -> the surface's vertical speed (m/s);
	// p_submit(owner, pos, radius, strength), p_clear(owner), p_impulse(pos, radius, strength);
	// p_foam(owner, pos, radius, per_second): white water this step (per_second 0 = none now).
	template <typename H, typename F, typename R, typename S, typename C, typename I, typename W>
	void update(SceneTree *p_tree, double p_delta, const H &p_height, const F &p_flow, const R &p_rise, const S &p_submit, const C &p_clear, const I &p_impulse, const W &p_foam) {
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
					const bool entering = last && vel.y < -1.0f && last->position.y - ext.y > surface && below <= 0.0f;
					if (entering) {
						p_impulse(Vector3(pos.x, surface, pos.z), ext.x, CLAMP(-vel.y / 5.0f, 0.0f, 1.0f));
					}
					// White water: only while the surface lies across the body (it breaks against it), on the water's
					// speed past it, horizontally against the flow and vertically against the surface's rise.
					float foam_per_second = 0.0f;
					float foam_radius = ext.x;
					if (foam_rate > 0.0f && below <= 0.0f && above <= 0.0f) {
						const float rise = p_rise(pos);
						const float u = Math::sqrt(rel.x * rel.x + rel.z * rel.z + (vel.y - rise) * (vel.y - rise));
						const Vector2 f = foam_for(u, ext.x, 9.81f, foam_rate);
						foam_per_second = f.x;
						foam_radius = f.y;
					}
					// Falling in: the impact crown and cavity entrain air once, on the entry speed (over this step).
					if (foam_rate > 0.0f && entering) {
						const Vector2 f = foam_for(-vel.y, ext.x, 9.81f, foam_rate);
						foam_per_second = MAX(foam_per_second, f.x / MAX(float(p_delta), 1e-3f) * 0.5f);
						foam_radius = MAX(foam_radius, f.y);
					}
					if (foam_per_second > 0.0f) {
						p_foam(owner, Vector3(pos.x, surface, pos.z), foam_radius, foam_per_second);
						s.foamed = true;
					}
				}
			}
			if (!s.submitted && last && last->submitted) {
				p_clear(owner);
			}
			if (!s.foamed && last && last->foamed) {
				p_foam(owner, pos, 0.0f, 0.0f);
			}
			next.insert(id, s);
		}
		// Gone from the group or the tree.
		for (const KeyValue<ObjectID, Seen> &kv : seen) {
			if (kv.value.submitted && !next.has(kv.key)) {
				p_clear(int(uint64_t(kv.key)));
			}
			if (kv.value.foamed && !next.has(kv.key)) {
				p_foam(int(uint64_t(kv.key)), kv.value.position, 0.0f, 0.0f);
			}
		}
		seen = next;
	}
};

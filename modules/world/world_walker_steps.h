/**************************************************************************/
/*  world_walker_steps.h                                                  */
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

#pragma once

// Footfalls for a walker (docs/ideas/world-contracts.md, Contact bodies). A character's body shape is a capsule, not
// two feet: the ground, snow, sand and mud press its soles one step at a time instead, left and right of its path,
// unless its feet print themselves (meta world_contact_feet). A rigged character whose FootPlacementModifier3D plants
// its real feet publishes each footfall to WorldFootfalls, and the walker's steps are then those footfalls, where the
// animation put the feet; with meta world_contact_feet and no published footfalls (feet that are contact bodies of
// their own) the walker presses nothing. Each deformer keeps one of these and asks it for the steps since its last
// update.

#include "world_contracts.h"

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

struct WorldWalkerStep {
	Vector3 position; // the sole's middle, at the walker's underside (the deformer seats it on its own surface)
	float yaw = 0.0f; // the sole's heading, atan2(-dir.z, dir.x) as the other prints
	float length = 0.27f; // m, the sole
	float width = 0.1f; // m
	int side = 0; // 0 left, 1 right
	// The share of the body's weight on this sole: a walk spends most of each stride on one foot (single support), and
	// the heel strike peaks near 1.2 body weights (Winter 2009), so a deformer presses the full weight on the sole.
	float load_share = 1.0f;
};

// Footfalls published by animated feet (FootPlacementModifier3D), per walker, for every deformer to read once.
class WorldFootfalls {
	struct Log {
		LocalVector<WorldWalkerStep> steps;
		LocalVector<uint64_t> serials;
		uint64_t next = 1;
	};
	static inline Mutex mutex;
	static inline HashMap<ObjectID, Log> logs;
	static constexpr uint32_t KEEP = 16;

public:
	static void publish(ObjectID p_walker, const WorldWalkerStep &p_step) {
		MutexLock lock(mutex);
		Log &log = logs[p_walker];
		log.steps.push_back(p_step);
		log.serials.push_back(log.next++);
		if (log.steps.size() > KEEP) {
			log.steps.remove_at(0);
			log.serials.remove_at(0);
		}
	}

	// The serial of the latest footfall (0: none published), so a new reader starts after what is already there.
	static uint64_t latest(ObjectID p_walker) {
		MutexLock lock(mutex);
		const Log *log = logs.getptr(p_walker);
		return log ? log->next - 1 : 0;
	}

	static bool has(ObjectID p_walker) {
		MutexLock lock(mutex);
		return logs.has(p_walker);
	}

	// The footfalls after r_serial, appended to r_steps; r_serial moves to the latest.
	static void read_since(ObjectID p_walker, uint64_t &r_serial, LocalVector<WorldWalkerStep> &r_steps) {
		MutexLock lock(mutex);
		const Log *log = logs.getptr(p_walker);
		if (!log) {
			return;
		}
		for (uint32_t i = 0; i < log->steps.size(); i++) {
			if (log->serials[i] > r_serial) {
				r_steps.push_back(log->steps[i]);
				r_serial = log->serials[i];
			}
		}
	}

	static void remove(ObjectID p_walker) {
		MutexLock lock(mutex);
		logs.erase(p_walker);
	}
};

class WorldWalkerSteps {
	struct State {
		Vector3 last;
		uint64_t footfall = 0; // the last published footfall read (walkers whose feet print themselves)
		Vector2 dir = Vector2(0, -1);
		float travel = 0.0f;
		int side = 0;
		bool seen = false;
	};
	HashMap<ObjectID, State> states;

public:
	// Step length about 0.41 of stature for a walk (Grieve and Gear 1966; Dean 1965), the feet about 0.18 m apart.
	static constexpr float STEP_PER_STATURE = 0.41f;
	static constexpr float FOOT_SPACING = 0.18f;

	void begin() {
		for (KeyValue<ObjectID, State> &kv : states) {
			kv.value.seen = false;
		}
	}

	// The footfalls of p_node (described as p_body) since the last call; nothing for a body that is not a walker or
	// whose feet print themselves. A jump of more than 10 m (a teleport) starts over.
	void step(const Node3D *p_node, const WorldContactBody &p_body, LocalVector<WorldWalkerStep> &r_steps) {
		if (!p_node || !p_body.walker) {
			return;
		}
		if (p_body.feet) {
			// Feet that print themselves: their published footfalls, each once.
			const ObjectID id = p_node->get_instance_id();
			State *f = states.getptr(id);
			if (!f) {
				f = &states.insert(id, State())->value;
				f->footfall = WorldFootfalls::latest(id);
			}
			f->seen = true;
			WorldFootfalls::read_since(id, f->footfall, r_steps);
			return;
		}
		const Vector3 under(p_body.center.x, p_body.center.y - p_body.half_height, p_body.center.z);
		State *s = states.getptr(p_node->get_instance_id());
		if (!s) {
			s = &states.insert(p_node->get_instance_id(), State())->value;
			s->last = under;
		}
		s->seen = true;
		const Vector2 d(under.x - s->last.x, under.z - s->last.z);
		const float len = d.length();
		if (len > 10.0f) {
			s->last = under;
			s->travel = 0.0f;
			return;
		}
		const float step_len = CLAMP(STEP_PER_STATURE * 2.0f * p_body.half_height, 0.3f, 1.0f);
		if (len < 1e-4f) {
			return;
		}
		s->dir = d / len;
		// Each step lands where the walker's travel crosses a step length, interpolated along this update's path.
		float t = step_len - s->travel;
		while (t <= len) {
			const Vector2 p = Vector2(s->last.x, s->last.z) + s->dir * t;
			const Vector2 side_v = Vector2(-s->dir.y, s->dir.x) * (s->side == 0 ? 0.5f : -0.5f) * FOOT_SPACING;
			WorldWalkerStep w;
			w.position = Vector3(p.x + side_v.x, Math::lerp(s->last.y, under.y, (real_t)(t / len)), p.y + side_v.y);
			w.yaw = Math::atan2(-s->dir.y, s->dir.x);
			w.side = s->side;
			r_steps.push_back(w);
			s->side ^= 1;
			t += step_len;
		}
		s->travel = len - (t - step_len);
		s->last = under;
	}

	void end() {
		LocalVector<ObjectID> gone;
		for (const KeyValue<ObjectID, State> &kv : states) {
			if (!kv.value.seen) {
				gone.push_back(kv.key);
			}
		}
		for (const ObjectID &id : gone) {
			states.erase(id);
		}
	}
};

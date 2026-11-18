/**************************************************************************/
/*  physx_ash.cpp                                                         */
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

#include "physx_ash.h"

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/templates/sort_array.h"
#include "scene/3d/node_3d.h"

void PhysXAsh::_build(Node3D *p_owner) {
	if (flow || !p_owner || !p_owner->is_inside_tree()) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Ash");
	flow->set_cell_size(0.2f);
	flow->set_max_blocks(512);
	flow->set_combustion(false);
	flow->set_emission_strength(0.0f);
	flow->set_temperature_range(1.0f);
	flow->set_buoyancy(0.0f); // cold
	flow->set_cooling_rate(1.0f);
	flow->set_vorticity(1.0f); // eddies over rough burnt ground
	// Grey-white mineral ash over darker char (ash albedo about 0.3-0.5, Roy et al. 2010).
	flow->set_smoke_color(Color(0.56f, 0.54f, 0.51f));
	flow->set_smoke_density(1.5f);
	flow->set_smoke_buoyancy(-0.4f); // a dust a little heavier than air: it drifts low and settles
	flow->set_smoke_fade(0.12f); // fine ash stays up a while
	flow->set_fire_light(false);
	flow->set_embers(false);
	flow->set_terrain_floor(true);
	p_owner->add_child(flow, false, Node::INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	for (int k = 0; k < MAX_EMITTERS; k++) {
		PhysXFlowEmitter3D *em = memnew(PhysXFlowEmitter3D);
		em->set_name(vformat("Ash%d", k));
		em->set_enabled(false);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		em->set_fuel(0.0f);
		em->set_burn(0.0f);
		em->set_temperature(0.0f);
		flow->add_child(em);
		em->set_flow_path(em->get_path_to(flow));
		emitters.push_back(em);
	}
}

void PhysXAsh::step(Node3D *p_owner, const LocalVector<Source> &p_sources, const Vector3 &p_wind, float p_delta) {
	float sum = 0.0f;
	for (const Source &s : p_sources) {
		sum += s.rate;
	}
	total += sum * p_delta;
	if (sum <= 0.0f) {
		if (flow) {
			for (PhysXFlowEmitter3D *em : emitters) {
				em->set_enabled(false);
			}
			// The dust drifts off and settles; then the gas goes.
			idle += p_delta;
			if (idle > 20.0f) {
				clear();
			}
		}
		return;
	}
	idle = 0.0f;
	_build(p_owner);
	if (!flow) {
		return;
	}
	struct ByRate {
		const LocalVector<Source> *s;
		bool operator()(uint32_t a, uint32_t b) const { return (*s)[a].rate > (*s)[b].rate; }
	};
	LocalVector<uint32_t> order;
	for (uint32_t k = 0; k < p_sources.size(); k++) {
		if (p_sources[k].rate > 0.0f) {
			order.push_back(k);
		}
	}
	SortArray<uint32_t, ByRate> sorter;
	sorter.compare.s = &p_sources;
	sorter.sort(order.ptr(), order.size());
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *em = emitters[k];
		if (k >= order.size()) {
			em->set_enabled(false);
			continue;
		}
		const Source &s = p_sources[order[k]];
		const float flux = s.rate / MAX(s.size * s.size, 0.01f); // kg/m2/s
		// About 1 unit of dust for 6e-5 kg/m2/s: a strong gust over fresh ash (1e-4) reads as a grey sheet, the
		// threshold's flicker as a faint haze.
		em->set_enabled(true);
		em->set_size(Vector3(s.size, 0.3f, s.size));
		em->set_global_position(s.position + Vector3(0.0f, 0.15f, 0.0f));
		em->set_smoke(CLAMP(flux / 6.0e-5f, 0.05f, 2.0f));
		// Lifted off the surface into the wind: carried at most of its speed, a little upward from the eddies.
		em->set_velocity(Vector3(p_wind.x, 0.0f, p_wind.z) * 0.8f + Vector3(0.0f, 0.2f, 0.0f));
		em->set_couple_rate(4.0f);
	}
}

void PhysXAsh::clear() {
	emitters.clear();
	if (flow) {
		flow->queue_free(); // its emitters with it
		flow = nullptr;
	}
	idle = 0.0f;
}

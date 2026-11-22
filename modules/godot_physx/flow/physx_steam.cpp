/**************************************************************************/
/*  physx_steam.cpp                                                       */
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

#include "physx_steam.h"

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/templates/sort_array.h"
#include "scene/3d/node_3d.h"

#include "modules/world/world_contracts.h"

void PhysXSteam::add_heat(const Vector3 &p_position, float p_heat, float p_radius, float p_tau) {
	if (p_heat <= 0.0f) {
		return;
	}
	const float tau = MAX(p_tau, 0.25f);
	const float kg = p_heat / BOIL_HEAT;
	total += kg;
	// Joins a puff within its radius (a hose sweeping a patch steams as one cloud).
	for (Puff &q : puffs) {
		const float dist = q.position.distance_to(p_position);
		if (dist < 0.75f) {
			const float w = q.rate / MAX(q.rate + kg / tau, 1.0e-9f);
			q.position = q.position.lerp(p_position, 1.0f - w);
			q.rate += kg / tau;
			// Steam leaves the wet ground's surface: the source stays a low patch, never a dome.
			q.radius = MIN(MAX(q.radius, 0.5f * (dist + p_radius)), 0.8f);
			q.tau = Math::lerp(tau, q.tau, w);
			return;
		}
	}
	Puff q;
	q.position = p_position;
	q.rate = kg / tau;
	q.radius = CLAMP(p_radius, 0.2f, 0.8f);
	q.tau = tau;
	puffs.push_back(q);
}

void PhysXSteam::_build(Node3D *p_owner) {
	if (flow || !p_owner || !p_owner->is_inside_tree()) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Steam");
	flow->set_cell_size(0.1f);
	flow->set_max_blocks(512);
	flow->set_combustion(false);
	flow->set_emission_strength(0.0f); // vapour near 100 C does not glow
	flow->set_temperature_range(1.0f);
	// Steam rises: water vapour is lighter than air (18 against 29 g/mol) and starts near 100 C.
	flow->set_buoyancy(3.0f);
	flow->set_cooling_rate(1.5f);
	flow->set_vorticity(1.5f);
	flow->set_smoke_color(Color(0.93f, 0.94f, 0.96f));
	flow->set_smoke_density(3.0f);
	flow->set_smoke_buoyancy(6.0f); // the vapour itself is light, whatever its heat
	flow->set_fire_light(false);
	flow->set_embers(false);
	flow->set_terrain_floor(true);
	p_owner->add_child(flow, false, Node::INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	for (int k = 0; k < MAX_EMITTERS; k++) {
		PhysXFlowEmitter3D *em = memnew(PhysXFlowEmitter3D);
		em->set_regional_smoke(false); // its smoke is reported by its owner, or is not smoke
		em->set_name(vformat("Puff%d", k));
		em->set_enabled(false);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		em->set_fuel(0.0f);
		em->set_burn(0.0f);
		flow->add_child(em);
		em->set_flow_path(em->get_path_to(flow));
		emitters.push_back(em);
	}
}

void PhysXSteam::step(Node3D *p_owner, const Vector3 &p_wind, float p_delta) {
	for (int64_t k = int64_t(puffs.size()) - 1; k >= 0; k--) {
		Puff &q = puffs[k];
		q.rate *= Math::exp(-p_delta / q.tau);
		if (q.rate < 1.0e-4f) {
			puffs.remove_at_unordered(k);
		}
	}
	if (puffs.is_empty()) {
		if (flow) {
			for (PhysXFlowEmitter3D *em : emitters) {
				em->set_enabled(false);
			}
			// The cloud clears within seconds; then the gas goes.
			idle += p_delta;
			if (idle > 12.0f) {
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
	// The cloud thins as its droplets evaporate into the air: in a few seconds in dry air, lingering when it is
	// near saturation (the white plume of a kettle on a cold damp day).
	float humidity = 0.6f;
	if (const WorldWeather *w = WorldContracts::weather(); w->has_provider()) {
		humidity = CLAMP(w->air_humidity_at(puffs[0].position), 0.0f, 1.0f);
	}
	flow->set_smoke_fade(Math::lerp(1.2f, 0.25f, humidity * humidity));
	// The strongest puffs get the emitters.
	struct ByRate {
		const LocalVector<Puff> *p;
		bool operator()(uint32_t a, uint32_t b) const { return (*p)[a].rate > (*p)[b].rate; }
	};
	LocalVector<uint32_t> order;
	for (uint32_t k = 0; k < puffs.size(); k++) {
		order.push_back(k);
	}
	SortArray<uint32_t, ByRate> sorter;
	sorter.compare.p = &puffs;
	sorter.sort(order.ptr(), order.size());
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *em = emitters[k];
		if (k >= order.size()) {
			em->set_enabled(false);
			continue;
		}
		const Puff &q = puffs[order[k]];
		// About 1 unit of smoke for 0.02 kg/s of steam per square metre of source: a bucket on a hot bed
		// (a few tenths of a kilogram a second) reads as a dense white burst, a damp edge as a wisp.
		const float area = Math::PI * q.radius * q.radius;
		em->set_enabled(true);
		em->set_size(Vector3(2.0f * q.radius, 0.4f, 2.0f * q.radius));
		em->set_global_position(q.position + Vector3(0.0f, 0.2f, 0.0f));
		em->set_smoke(CLAMP(q.rate / area / 0.02f, 0.1f, 3.0f));
		em->set_temperature(CLAMP(q.rate / area / 0.05f, 0.3f, 0.7f));
		em->set_velocity(p_wind * 0.4f + Vector3(0.0f, CLAMP(1.5f + 10.0f * q.rate / area, 1.5f, 4.0f), 0.0f));
		// Boiling water swells some 1,700 times: the source pushes outwards as well as up.
		em->set_divergence(CLAMP(q.rate / area / 0.02f, 0.2f, 2.0f));
		em->set_couple_rate(8.0f);
	}
}

void PhysXSteam::clear() {
	puffs.clear();
	emitters.clear();
	if (flow) {
		flow->queue_free(); // its emitters with it
		flow = nullptr;
	}
	idle = 0.0f;
}

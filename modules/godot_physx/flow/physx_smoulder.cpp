/**************************************************************************/
/*  physx_smoulder.cpp                                                    */
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

#include "physx_smoulder.h"

#include "physx_fire_spread_3d.h"
#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/templates/hash_map.h"
#include "core/templates/sort_array.h"

#include "modules/world/world_contracts.h"

void PhysXSmoulder::reset(uint32_t p_cells) {
	duff.resize(p_cells);
	duff0.resize(p_cells);
	age.resize(p_cells);
	reach.resize(p_cells);
	soak.resize(p_cells);
	tried.resize(p_cells);
	heat.resize(p_cells);
	for (uint32_t i = 0; i < p_cells; i++) {
		duff[i] = 0.0f;
		duff0[i] = -1.0f; // read from the fuel bed when first needed
		age[i] = -1.0f;
		reach[i] = 0.0f;
		soak[i] = 0.0f;
		tried[i] = 0;
		heat[i] = 0.0f;
	}
	active.clear();
	moisture = -1.0f;
	burnt_kg = 0.0f;
	rekindled = 0;
}

float PhysXSmoulder::_rand() {
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return float(seed & 0xffffffu) / float(0x1000000);
}

// The duff under a cell: deeper where the fuel bed is heavier (its patches), kg/m2.
void PhysXSmoulder::_init(const PhysXFireSpread3D *p_fire, uint32_t p_i) {
	if (duff0[p_i] >= 0.0f) {
		return;
	}
	const float bed = p_fire->_model(p_fire->fuel_type).load;
	const float d = BULK_DENSITY * p_fire->_duff_depth() * CLAMP(p_fire->load[p_i] / MAX(bed, 1.0e-3f), 0.3f, 1.5f);
	duff0[p_i] = d;
	duff[p_i] = d;
}

float PhysXSmoulder::_cell_moisture(const PhysXFireSpread3D *p_fire, uint32_t p_i) const {
	// The fine fuel's moisture patches (wetter hollows, drier rises) hold for the duff under them too.
	const float patch = CLAMP(p_fire->wet[p_i], 0.5f, 3.0f);
	return moisture * patch + soak[p_i] / MAX(duff0[p_i], 0.5f);
}

// Moisture with a 50% chance of lighting: about 120% of dry weight in organic soil with little mineral matter
// (Frandsen 1997; 110% in milled peat, Frandsen 1987), lower as the mineral share rises (Frandsen 1987, Hungerford et
// al. 1995). The chance across it as a logistic in the moisture.
float PhysXSmoulder::_m50(const PhysXFireSpread3D *p_fire) {
	return MAX(1.2f - 1.5f * MAX(p_fire->duff_mineral - 0.1f, 0.0f), 0.15f);
}

float PhysXSmoulder::_ignition_chance(const PhysXFireSpread3D *p_fire, float p_moisture) const {
	return 1.0f / (1.0f + Math::exp((p_moisture - _m50(p_fire)) / 0.12f));
}

// Lateral spread, m/s of game time: 1-5 cm/h from dry to soaked peat (Prat-Guitart et al. 2016), a little faster
// in a breeze that feeds it air (Ohlemiller 2002).
static float _creep(float p_moisture, float p_wind) {
	return 0.05f / 3600.0f * Math::exp(-0.8f * p_moisture) * (1.0f + 0.15f * MIN(p_wind, 4.0f));
}

void PhysXSmoulder::_start(uint32_t p_i) {
	if (age[p_i] >= 0.0f) {
		return;
	}
	age[p_i] = 0.0f;
	reach[p_i] = 0.0f;
	active.push_back(p_i);
}

void PhysXSmoulder::step(PhysXFireSpread3D *p_fire, float p_delta) {
	PhysXFireSpread3D *f = p_fire;
	const uint32_t n = f->nx * f->nz;
	if (duff.size() != n) {
		reset(n);
	}
	if (f->_duff_depth() <= 0.0f) {
		return;
	}
	const WorldWeather *ww = WorldContracts::weather();
	const bool weather = ww->has_provider();
	const float dtg = p_delta * float(weather ? ww->time_scale() : 1.0);
	const Vector3 at = f->get_global_position();
	// The duff's moisture follows the weather slowly (the Duff Moisture Code's time lag is about 12 days: Van Wagner
	// 1987), from duff_moisture on a day when the fine fuel is at fuel_moisture; rain soaks into it, about half of it
	// getting through the litter.
	float eq = f->duff_moisture;
	if (f->use_weather_moisture && f->fuel_moisture > 1.0e-3f) {
		eq *= CLAMP(f->base_moisture / f->fuel_moisture, 0.5f, 4.0f);
	}
	if (moisture < 0.0f) {
		moisture = eq;
	}
	moisture += (eq - moisture) * (1.0f - Math::exp(-dtg / (12.0f * 86400.0f)));
	if (weather) {
		const float rain = MAX(ww->precipitation_at(at), 0.0f); // mm/h = kg/m2/h
		moisture = MIN(moisture + rain * dtg / 3600.0f * 0.5f / (BULK_DENSITY * f->_duff_depth()), 4.0f);
	}
	// Water poured on: soaks the duff under it and boils off a smouldering cell's heat as steam.
	const float cell_area = f->cell_size * f->cell_size;
	const float dry = Math::exp(-dtg / (2.0f * 86400.0f));
	for (float &s : soak) {
		s *= dry;
	}
	for (const KeyValue<uint32_t, float> &e : f->water_cells) {
		if (e.key >= n) {
			continue;
		}
		_init(f, e.key);
		soak[e.key] = MIN(soak[e.key] + e.value / cell_area, 20.0f);
		if (age[e.key] >= 0.0f) {
			f->steam.add_heat(f->_cell_world(int(e.key % f->nx), int(e.key / f->nx)), MIN(e.value * PhysXSteam::BOIL_HEAT, duff[e.key] * cell_area * 1500.0f * 400.0f), 0.3f);
		}
	}
	const float m50 = _m50(f);
	// The flaming front lights the duff it passes, once, where it is dry enough; a thin grass bed's flames heat it
	// less than a deep litter bed's.
	const PhysXFireSpread3D::FuelModel &m = f->_model(f->fuel_type);
	if (f->band_x1 >= f->band_x0) {
		for (int z = f->band_z0; z <= f->band_z1; z++) {
			for (int x = f->band_x0; x <= f->band_x1; x++) {
				const uint32_t i = z * f->nx + x;
				if (tried[i] || f->t_ignite[i] < 0.0f || f->time - f->t_ignite[i] < 0.5f * m.burn_time) {
					continue;
				}
				tried[i] = 1;
				_init(f, i);
				if (duff[i] <= 0.0f) {
					continue;
				}
				// A hash steady per cell, so the same ground smoulders on a rerun.
				uint32_t hs = (i + 1u) * 2654435761u;
				hs = (hs ^ (hs >> 15)) * 0x2c1b3c6du;
				const float u = float((hs ^ (hs >> 12)) & 0xffffu) / 65535.0f;
				if (u < _ignition_chance(f, _cell_moisture(f, i)) * MIN(f->load[i] / 0.3f, 1.0f)) {
					_start(i);
				}
			}
		}
	}
	if (active.is_empty()) {
		rate_kg_s = 0.0f;
		return;
	}
	const float burnt_before = burnt_kg;
	const float wind = f->_wind().length();
	const float h = f->cell_size;
	const int nx = f->nx, nz = f->nz;
	const uint32_t was = active.size();
	for (int64_t k = int64_t(was) - 1; k >= 0; k--) {
		const uint32_t i = active[k];
		const float mi = _cell_moisture(f, i);
		// Out when wetter than well past the lighting limit (its own heat dries the duff ahead of it), or burnt
		// through.
		if (mi > 1.6f * m50 || duff[i] <= 0.02f * duff0[i]) {
			age[i] = -1.0f;
			heat[i] = 0.0f;
			active.remove_at_unordered(k);
			continue;
		}
		age[i] += dtg;
		const float v = _creep(mi, wind);
		// Burning down at about a quarter of the sideways rate (Rein 2009).
		const float take = MIN(BULK_DENSITY * 0.25f * v * dtg, duff[i]);
		duff[i] -= take;
		burnt_kg += take * cell_area;
		heat[i] = BULK_DENSITY * 0.25f * v * HEAT;
		const int x = int(i % nx), z = int(i / nx);
		const int nbr[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
		for (int d = 0; d < 4; d++) {
			const int xx = x + nbr[d][0], zz = z + nbr[d][1];
			if (xx < 0 || zz < 0 || xx >= nx || zz >= nz) {
				continue;
			}
			const uint32_t j = zz * nx + xx;
			if (age[j] >= 0.0f) {
				continue;
			}
			_init(f, j);
			if (duff[j] <= 0.05f * duff0[j] || duff[j] <= 0.0f) {
				continue;
			}
			reach[j] += v * dtg / h;
			if (reach[j] >= 1.0f) {
				reach[j] = 0.0f;
				if (_rand() < _ignition_chance(f, _cell_moisture(f, j))) {
					_start(j);
				}
			}
		}
		// A holdover: smouldering under fine fuel the front never burnt, it flames again once a breeze over about
		// 1 m/s feeds it air and the fuel above is dry enough to carry flame (Santoso et al. 2019), within an hour or
		// two of game time at 4 m/s.
		if (f->t_ignite[i] < 0.0f && f->load[i] > 1.0e-4f && wind > 1.0f) {
			const float mf = f->_moisture(i);
			if (mf < m.moisture_extinction) {
				const float lambda = CLAMP((wind - 1.0f) / 3.0f, 0.0f, 1.0f) * (1.0f - mf / m.moisture_extinction) / 3600.0f;
				if (_rand() < 1.0f - Math::exp(-lambda * dtg)) {
					f->ignite(f->_cell_world(x, z), 0.6f * h);
					rekindled++;
				}
			}
		}
	}
	rate_kg_s = (burnt_kg - burnt_before) / MAX(p_delta, 1.0e-4f);
	Vector3 c;
	for (uint32_t i : active) {
		c += f->_cell_world(int(i % nx), int(i / nx));
	}
	centre = active.is_empty() ? Vector3() : c / float(active.size());
}

float PhysXSmoulder::heat_at(uint32_t p_i) const {
	return p_i < age.size() && age[p_i] >= 0.0f ? heat[p_i] : 0.0f;
}

float PhysXSmoulder::consumed(uint32_t p_i) const {
	return p_i < duff0.size() && duff0[p_i] > 0.0f ? 1.0f - duff[p_i] / duff0[p_i] : 0.0f;
}

void PhysXSmoulder::extinguish() {
	for (uint32_t i : active) {
		age[i] = -1.0f;
		heat[i] = 0.0f;
	}
	active.clear();
}

void PhysXSmoulder::_build(PhysXFireSpread3D *p_fire) {
	if (flow || !p_fire || !p_fire->is_inside_tree()) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("SmoulderSmoke");
	flow->set_cell_size(0.2f);
	flow->set_max_blocks(1024);
	flow->set_combustion(false);
	flow->set_emission_strength(0.0f); // a few hundred degrees under the ash: no flame to see
	flow->set_temperature_range(1.0f);
	// Smouldering smoke is pale (mostly condensed tar and water droplets, little soot: Rein 2009) and cool, so it
	// rises slowly and hangs low, more of it for each kilogram burnt than flaming gives (Urbanski 2014).
	flow->set_buoyancy(0.8f);
	flow->set_cooling_rate(1.0f);
	flow->set_vorticity(1.2f);
	flow->set_smoke_color(Color(0.74f, 0.73f, 0.7f));
	flow->set_smoke_density(2.0f);
	flow->set_smoke_buoyancy(0.3f);
	flow->set_smoke_fade(0.12f);
	flow->set_fire_light(false);
	flow->set_embers(false);
	flow->set_terrain_floor(true);
	p_fire->add_child(flow, false, Node::INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	for (int k = 0; k < MAX_EMITTERS; k++) {
		PhysXFlowEmitter3D *em = memnew(PhysXFlowEmitter3D);
		em->set_name(vformat("Smoulder%d", k));
		em->set_enabled(false);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		em->set_fuel(0.0f);
		em->set_burn(0.0f);
		flow->add_child(em);
		em->set_flow_path(em->get_path_to(flow));
		emitters.push_back(em);
	}
}

void PhysXSmoulder::step_smoke(PhysXFireSpread3D *p_fire, const Vector3 &p_wind, float p_delta) {
	if (active.is_empty()) {
		if (flow) {
			for (PhysXFlowEmitter3D *em : emitters) {
				em->set_enabled(false);
			}
			idle += p_delta;
			if (idle > 20.0f) {
				clear();
			}
		}
		return;
	}
	idle = 0.0f;
	_build(p_fire);
	if (!flow) {
		return;
	}
	// The smouldering cells gathered on 2 m tiles; the busiest get the emitters.
	const PhysXFireSpread3D *f = p_fire;
	const int tile = MAX(1, (int)Math::round(2.0f / f->cell_size));
	struct Tile {
		int cells = 0;
		Vector3 centre;
	};
	HashMap<uint32_t, Tile> tiles;
	for (uint32_t i : active) {
		const int x = int(i % f->nx), z = int(i / f->nx);
		const uint32_t key = uint32_t(z / tile) * 65536u + uint32_t(x / tile);
		Tile *t = tiles.getptr(key);
		if (!t) {
			t = &tiles.insert(key, Tile())->value;
		}
		t->cells++;
		t->centre += f->_cell_world(x, z);
	}
	LocalVector<Tile> list;
	for (const KeyValue<uint32_t, Tile> &e : tiles) {
		list.push_back(e.value);
	}
	struct Busier {
		bool operator()(const Tile &a, const Tile &b) const { return a.cells > b.cells; }
	};
	SortArray<Tile, Busier> sorter;
	sorter.sort(list.ptr(), list.size());
	const float tile_w = tile * f->cell_size;
	const float full = float(tile * tile);
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *em = emitters[k];
		if (k >= list.size()) {
			em->set_enabled(false);
			continue;
		}
		const Tile &t = list[k];
		const float share = CLAMP(t.cells / full, 0.0f, 1.0f);
		em->set_enabled(true);
		em->set_size(Vector3(tile_w * Math::sqrt(MAX(share, 0.1f)), 0.25f, tile_w * Math::sqrt(MAX(share, 0.1f))));
		em->set_global_position(t.centre / float(t.cells) + Vector3(0.0f, 0.1f, 0.0f));
		em->set_smoke(CLAMP(0.3f + 0.9f * share, 0.3f, 1.2f));
		em->set_temperature(0.15f);
		em->set_velocity(p_wind * 0.5f + Vector3(0.0f, 0.3f, 0.0f));
		em->set_couple_rate(4.0f);
	}
}

void PhysXSmoulder::clear() {
	emitters.clear();
	if (flow) {
		flow->queue_free();
		flow = nullptr;
	}
	idle = 0.0f;
}

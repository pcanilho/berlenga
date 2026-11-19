/**************************************************************************/
/*  physx_smoulder.h                                                      */
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

#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

class PhysXFireSpread3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Smouldering ground fire (docs/ideas/physx.md, "Smouldering ground fire"): the duff and organic soil under the fine
// fuel, which a passing front can light and which then burns for hours with no flame (Rein 2009). It lights where it
// is dry enough, with a 50% chance near 110-120% moisture (dry basis) in low-mineral organic soil, drier as the mineral
// share rises (Frandsen 1987, 1997); creeps sideways at 1-5 cm/h, slower the wetter it is (Prat-Guitart et al. 2016),
// and burns down about a quarter as fast; heavy rain or water puts it out, light rain does not. Where it creeps under
// unburnt fine fuel and the wind picks up, it can flame again (a holdover fire: Santoso et al. 2019). It runs on the
// weather's clock (WorldWeather.time_scale) and gives off a pale, cool, low-lying smoke from a gas of its own.
class PhysXSmoulder {
	LocalVector<float> duff; // kg/m2 left
	LocalVector<float> duff0; // kg/m2 at the start
	LocalVector<float> age; // game s smouldering, -1 not smouldering
	LocalVector<float> reach; // 0..1, the smoulder front's progress into the cell from a smouldering neighbour
	LocalVector<float> soak; // kg/m2 of liquid water poured on (fluids), drying with the duff
	LocalVector<uint8_t> tried; // the flaming front's chance to light it has been taken
	LocalVector<float> heat; // W/m2 released now
	LocalVector<uint32_t> active;
	float moisture = -1.0f; // the duff's moisture now (fraction of dry weight), before per-cell patches
	float burnt_kg = 0.0f;
	int rekindled = 0;
	uint32_t seed = 0x9e3779b9u;
	// The gas.
	PhysXFlow3D *flow = nullptr;
	LocalVector<PhysXFlowEmitter3D *> emitters;
	float idle = 0.0f;

	float _rand();
	float _cell_moisture(const PhysXFireSpread3D *p_fire, uint32_t p_i) const;
	static float _m50(const PhysXFireSpread3D *p_fire);
	float _ignition_chance(const PhysXFireSpread3D *p_fire, float p_moisture) const;
	void _init(const PhysXFireSpread3D *p_fire, uint32_t p_i);
	void _start(uint32_t p_i);
	void _build(PhysXFireSpread3D *p_fire);

public:
	static constexpr int MAX_EMITTERS = 8;
	static constexpr float BULK_DENSITY = 100.0f; // kg/m3, forest duff and peat 50-150 (Prat-Guitart et al. 2016)
	static constexpr float HEAT = 8.0e6f; // J/kg released smouldering (char oxidation, below flaming's heat content)

	void reset(uint32_t p_cells);
	// Every p_delta real seconds: lights, spreads, burns and wets the duff; rekindles flame.
	void step(PhysXFireSpread3D *p_fire, float p_delta);
	// Every frame: the smoke.
	void step_smoke(PhysXFireSpread3D *p_fire, const Vector3 &p_wind, float p_delta);
	void clear(); // frees the gas
	void extinguish();

	bool is_active() const { return !active.is_empty(); }
	bool has_gas() const { return flow != nullptr; }
	bool is_smouldering(uint32_t p_i) const { return p_i < age.size() && age[p_i] >= 0.0f; }
	float heat_at(uint32_t p_i) const; // W/m2
	float consumed(uint32_t p_i) const; // share of the cell's duff burnt away
	int get_cells() const { return active.size(); }
	float get_burnt_kg() const { return burnt_kg; }
	int get_rekindled() const { return rekindled; }
	float get_moisture() const { return moisture; }
	PhysXFlow3D *get_flow() const { return flow; }
};

/**************************************************************************/
/*  physx_steam.h                                                         */
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

class Node3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Steam where water lands on fire (docs/ideas/physx.md, "Water on fire makes steam"). Water on burning or glowing
// fuel takes the bed's heat to boil (2.26 MJ/kg, 2.6 with heating to 100 C; FDS's water spray sink), and the vapour
// condenses into a white cloud as it meets cool air, then clears as its droplets evaporate again, slower in humid
// air. Steam is white and the fire's smoke is grey, so it gets a gas of its own: a PhysXFlow3D made only while
// something steams and freed after, combustion off, no glow, white, buoyant, fading by the air's humidity. Puffs
// (a place, kilograms of steam a second, a time constant) drive a small pool of its emitters.
class PhysXSteam {
	struct Puff {
		Vector3 position;
		float rate = 0.0f; // kg/s of steam now
		float radius = 0.5f; // m
		float tau = 3.0f; // s, the rate falls as exp(-t / tau)
	};
	LocalVector<Puff> puffs;
	PhysXFlow3D *flow = nullptr;
	LocalVector<PhysXFlowEmitter3D *> emitters;
	float idle = 0.0f;
	float total = 0.0f;

	void _build(Node3D *p_owner);

public:
	static constexpr int MAX_EMITTERS = 8;
	static constexpr float BOIL_HEAT = 2.6e6f; // J/kg, heating to 100 C and boiling

	// Steam from water taking p_heat joules out of hot fuel at p_position, given off over about p_tau seconds.
	void add_heat(const Vector3 &p_position, float p_heat, float p_radius, float p_tau = 3.0f);
	void step(Node3D *p_owner, const Vector3 &p_wind, float p_delta);
	void clear(); // frees the gas (the owner leaving the tree)
	float get_total_kg() const { return total; } // steam made so far
	bool is_steaming() const { return !puffs.is_empty(); }
};

/**************************************************************************/
/*  physx_ash.h                                                           */
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

// Ash blown off burnt ground (docs/ideas/physx.md, "Wind lifts ash after a fire"). Fine mineral ash left by a fire
// is loose and light; wind over it lifts a grey dust that drifts low downwind. Its gas is its own PhysXFlow3D (the
// fire's may be gone, and the fire's smoke rises where ash dust hugs the ground), made only while ash blows and
// freed after: no combustion, no glow, grey, a little heavier than air. Patches of blowing ash (a place, a size,
// kilograms a second) drive a small pool of box emitters on the ground.
class PhysXAsh {
public:
	struct Source {
		Vector3 position; // on the ground
		float size = 2.0f; // m, the patch's side
		float rate = 0.0f; // kg/s of ash lifted
	};

private:
	PhysXFlow3D *flow = nullptr;
	LocalVector<PhysXFlowEmitter3D *> emitters;
	float idle = 0.0f;
	float total = 0.0f;
	float settle_timer = 0.0f;
	LocalVector<Source> settling; // lifted since the last hand-over to the snow (rate holds kg)
	void _build(Node3D *p_owner);
	void _settle_on_snow(const Vector3 &p_wind);

public:
	static constexpr int MAX_EMITTERS = 8;

	// The patches blowing now (the strongest MAX_EMITTERS are drawn), stepped over p_delta.
	void step(Node3D *p_owner, const LocalVector<Source> &p_sources, const Vector3 &p_wind, float p_delta);
	void clear(); // frees the gas
	float get_total_kg() const { return total; } // ash lifted so far
	bool is_active() const { return flow != nullptr; }
};

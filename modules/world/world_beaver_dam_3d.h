/**************************************************************************/
/*  world_beaver_dam_3d.h                                                 */
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

#include "world_contracts.h"

#include "scene/3d/node_3d.h"

// A beaver dam and its pond (docs/ideas/world-links.md, beaver dam fails -> beaver meadow). While the dam holds, the
// pond stands; when it fails (breach(), or authored as breached) the pond drains and the flat it leaves is a beaver
// meadow: the dam reports a DISTURBANCE_BEAVER disc of pond_radius round itself to WorldStand, years_drained old, so
// vegetation grows the meadow's sedges and grasses and trees leave the open ground to succession (Naiman et al. 1988;
// Johnston and Naiman 1990: abandoned ponds drain within a few years and stay sedge meadow for decades).
class WorldBeaverDam3D : public Node3D {
	GDCLASS(WorldBeaverDam3D, Node3D);

public:
	enum DamState {
		DAM_ACTIVE = 0, // the pond stands
		DAM_BREACHED = 1, // drained: a beaver meadow
	};

private:
	DamState state = DAM_ACTIVE;
	float pond_radius = 40.0f; // m
	float years_drained = 0.0f;
	int event_id = 0; // the stand's disturbance while breached

	void _sync();
	void _drop();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_state(DamState p_state);
	DamState get_state() const { return state; }
	void set_pond_radius(float p_radius);
	float get_pond_radius() const { return pond_radius; }
	void set_years_drained(float p_years);
	float get_years_drained() const { return years_drained; }
	// The dam fails now (a flood over it, a debris flow, the beavers gone): the pond drains, the meadow starts at 0 years.
	void breach();
	int get_disturbance_id() const { return event_id; }
};

VARIANT_ENUM_CAST(WorldBeaverDam3D::DamState);

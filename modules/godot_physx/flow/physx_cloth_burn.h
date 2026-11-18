/**************************************************************************/
/*  physx_cloth_burn.h                                                    */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/object/object_id.h"
#include "core/templates/local_vector.h"
#include "scene/resources/material.h"

class XPBDClothSolver;
class Node;
class PhysXFlow3D;
class PhysXFireSpread3D;

// Burning fabric for PhysXCloth3D (docs/ideas/physx.md, "Cloth burns and tears"). Each particle takes in heat from a
// ground fire's flames, hot Flow gas and flaming burnables; thin fabric ignites once the heat it has taken in raises it
// to its ignition temperature (the thermally thin ignition of fire science: areal heat capacity times the temperature
// rise). Flame then spreads over the cloth's own mesh, fast upward and slowly down and across (as on a hanging
// curtain); each part chars, glows at the burning edge, and burns through after a few seconds, when its triangles
// leave the mesh so the cloth opens, sags and falls apart. Flames rise from the burning edge through Flow emitters.
class PhysXClothBurn {
public:
	struct Settings {
		float flame_spread = 0.02f; // m/s across and down the fabric; upward about eight times faster
		float burn_through_time = 3.0f; // s from ignition to a hole
		float glow_energy = 4.0f;
		float water = 0.0f; // kg/m2 of water the fabric holds: heat must boil it off before the fabric can light
		float wetness = 0.0f; // 0..1 for the look: water in the weave darkens it
	};

private:
	LocalVector<float> heat; // J/m2 taken in
	LocalVector<float> dried; // kJ/m2 spent boiling off the water held
	LocalVector<float> ignited; // time lit, -1 before
	LocalVector<float> progress; // 0..1 of the flame's way from a burning neighbour
	LocalVector<float> jitter; // per particle, steady
	LocalVector<uint8_t> gone;
	LocalVector<LocalVector<int>> neighbours;
	uint32_t built_vertices = 0;
	uint32_t built_indices = 0;
	float time = 0.0f;
	float sample_timer = 0.0f;
	bool any_lit = false;
	bool any_heat = false;
	LocalVector<ObjectID> emitters;
	Ref<Material> source_material;
	Ref<Material> burn_material;

	void _rebuild(const XPBDClothSolver &p_solver);
	void _sample(const XPBDClothSolver &p_solver, PhysXFireSpread3D *p_fire, PhysXFlow3D *p_flow, float p_delta);
	void _update_emitters(Node *p_owner, const XPBDClothSolver &p_solver, PhysXFlow3D *p_flow);

public:
	Settings settings;

	// Advances the burn; returns true when triangles burnt away (the mesh changed shape).
	bool step(Node *p_owner, XPBDClothSolver &p_solver, PhysXFireSpread3D *p_fire, PhysXFlow3D *p_flow, float p_delta);
	// Lights the particle nearest a world position.
	void ignite(const XPBDClothSolver &p_solver, const Vector3 &p_world);
	void reset();
	void clear_emitters();
	// Per particle: red char, green glow, blue scorch, alpha 1. Empty while nothing has heated.
	bool has_marks() const { return any_heat || any_lit; }
	void fill_colors(PackedColorArray &r_colors, int p_count) const;
	// The material with the char and glow read from vertex colours (base: the cloth's material, or null).
	RID get_material(const Ref<Material> &p_base);
	// Updates the material's wetness from settings.wetness.
	void update_wetness();
	int get_burning_count() const;
	int get_burnt_count() const;
};

/**************************************************************************/
/*  physx_firebrands.h                                                    */
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
#include "scene/resources/multimesh.h"

class Node3D;
class MultiMeshInstance3D;
class PhysXFlow3D;

// Firebrands (docs/ideas/physx.md, "Embers start new fires"): glowing bits of bark, twig and grass thrown up by a fire,
// lofted by its plume (Flow's gas in the flames, Byram's convection column over the front above them, leaning
// with the wind), carried by the wind at their height and falling at their terminal velocity
// (quadratic drag; Tarifa et al. 1965, Tohidi et al. 2015), which drops as they burn away. Where one lands on the
// ground of any PhysXFireSpread3D it may start a spot fire, with the probability of ignition of the fine fuel for its
// moisture and the air temperature (Schroeder 1969, as BehavePlus uses it), less as the brand has burnt out. Drawn as
// short glowing streaks along their flight.
class PhysXFirebrands {
public:
	struct Release {
		Vector3 position;
		float weight = 1.0f; // share of the brands from here
	};
	// A stretch of the burning front and the convection column over it.
	struct Plume {
		Vector3 base;
		float w0 = 0.0f; // m/s, the column's rise speed near the ground
		float width = 0.5f; // m, the stretch of front
	};

private:
	struct Brand {
		Vector3 p;
		Vector3 v;
		float vt = 4.0f; // terminal velocity when fresh, m/s
		float age = 0.0f;
		float life = 20.0f; // s of glowing combustion
	};
	LocalVector<Brand> brands;
	float carry = 0.0f;
	uint32_t seed = 0x9e3779b9u;
	MultiMeshInstance3D *mmi = nullptr;
	Ref<MultiMesh> mm;
	int spot_fires = 0;
	int landed = 0;

	LocalVector<Plume> plumes;
	float plume_length = 1.0f; // m, the front's length: the column keeps its speed to about this height

	float _rand();
	float _plume_lift(const Vector3 &p_pos, const Vector3 &p_wind) const;
	void _draw(Node3D *p_owner, float p_glow_energy);

public:
	static constexpr int MAX_BRANDS = 1000; // a torching tree throws hundreds a second
	// Schroeder's (1969) probability that a firebrand ignites fine dead fuel at this moisture (fraction of dry weight)
	// and air temperature (C).
	static float ignition_probability(float p_moisture, float p_air_temperature);

	// Byram's convection column over a line of fire of this intensity (kW/m) in air at this temperature (C): its rise
	// speed, m/s.
	static float plume_speed(float p_intensity, float p_air_temperature);
	// The columns over the front this step; p_length is the front's length.
	void set_plumes(const LocalVector<Plume> &p_plumes, float p_length);
	// Throws p_count more brands (fractions carry over) from the release points, weighted.
	void release(float p_count, const LocalVector<Release> &p_points, PhysXFlow3D *p_flow);
	// Moves them; returns the spot fires started this step.
	int step(Node3D *p_owner, PhysXFlow3D *p_flow, const Vector3 &p_wind, float p_delta, float p_glow_energy);
	void clear();
	int get_count() const { return brands.size(); }
	int get_spot_fires() const { return spot_fires; }
	int get_landed() const { return landed; }
};

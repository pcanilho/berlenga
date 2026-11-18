/**************************************************************************/
/*  physx_ground_grains_3d.h                                              */
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


#pragma once

#include "physx_granular_3d.h"

class GroundSurface3D;

// Sand that leaves the ground and lands again (docs/ideas/sand.md, stage 2): the hybrid of a height field
// and particles of Holz, Beer and Kuhlen 2009 ("Soil deformation models for real-time simulation: a hybrid
// approach"). The GroundSurface3D's field is the sand at rest; where a spinning wheel throws its tread's sand
// (GroundSurface3D sprays) or a script kicks some (kick()), that volume is dug out of the field and becomes
// MPM grains (this node's PhysXGranular3D solver, Drucker-Prager); a grain that comes down onto the field's
// surface, or leaves the domain, is deposited back into its column and the field slumps it. Volume is kept
// end to end: dug = in the air + deposited.
class PhysXGroundGrains3D : public PhysXGranular3D {
	GDCLASS(PhysXGroundGrains3D, PhysXGranular3D);

	NodePath ground_surface; // empty: the active GroundSurface3D
	NodePath follow_target; // the domain follows it (a vehicle), its floor a metre under the ground
	float tread_depth = 0.008f; // m of sand a slipping tread throws, per metre of slip (lug grooves and the sand they drag)
	float spray_spread = 0.2f; // velocity jitter, as a share of the throw speed
	float max_flight_time = 8.0f;
	Ref<Material> grain_material; // empty: dry sand
	// Mud (docs/ideas/mud.md, stage 4): sticky sprays go to a child grains node of wet clay this node makes.
	bool mud_spray = true;
	bool mud = false; // this node is that child: clay clods, not sand
	PhysXGroundGrains3D *mud_child = nullptr;
	void _make_mud_child();
	// Water (mud stage 4c): sprays carrying standing water go to a child of MPM liquid this node makes.
	bool water_spray = true;
	bool water = false; // this node is that child: water, not grains
	PhysXGroundGrains3D *water_child = nullptr;
	void _make_water_child();
	// The clay the thrown water carries (muddy water, round 3), kg/m3: the mean of what is in the air, landed with it
	// and drawn (a milky brown liquid past a few kg/m3), and the colour of that clay.
	float water_mud = 0.0f;
	float water_mud_drawn = 0.0f;
	Color water_mud_color = Color(0.42f, 0.32f, 0.22f);
	void _muddy_look();
	void _adopt_child(PhysXGroundGrains3D *p_child);
	// A grain or drop come down at p_at: sand and clay back into the field, water into its standing water.
	void _land(const Vector3 &p_at, float p_radius, double p_volume);
	void _take_spray(const Dictionary &p_spray, double p_delta);
	Ref<Mesh> _make_clod_mesh();

	LocalVector<uint8_t> alive;
	LocalVector<Vector3> last;
	LocalVector<float> age;
	LocalVector<Vector3> vel; // from the read backs, for a grain leaving the box
	LocalVector<float> seen_age; // its age at the last read back that moved it
	Vector3 throw_dir; // the sprays' mean heading, the box reaching that way
	LocalVector<float> from_y; // the height a grain left at: it lands only once it has risen or flown a while
	double particle_volume = 0.0;
	double carry = 0.0; // dug volume not yet whole particles
	double dug_volume = 0.0;
	double deposited_volume = 0.0;
	double stuck_volume = 0.0; // of that, stuck to vehicles' bodies (mud)
	int live = 0;

	GroundSurface3D *_ground() const;
	void _sync_capacity(int p_capacity);
	void _emit(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity, double p_volume, float p_stream = 0.0f);
	void _deposit_pass(double p_delta);
	void _follow();
	Vector3 _ballistic_landing(const Vector3 &p_from, const Vector3 &p_velocity) const;

protected:
	static void _bind_methods();
	Ref<Mesh> _make_mpm_particle_mesh() override;
	bool _is_granular() const override { return !water; }
	void _notification(int p_what);

public:
	void set_ground_surface(const NodePath &p_path) { ground_surface = p_path; }
	NodePath get_ground_surface() const { return ground_surface; }
	void set_follow_target(const NodePath &p_path);
	NodePath get_follow_target() const { return follow_target; }
	void set_tread_depth(float p_m) { tread_depth = MAX(p_m, 0.0f); }
	float get_tread_depth() const { return tread_depth; }
	void set_spray_spread(float p_v) { spray_spread = CLAMP(p_v, 0.0f, 1.0f); }
	float get_spray_spread() const { return spray_spread; }
	void set_max_flight_time(float p_s) { max_flight_time = MAX(p_s, 0.1f); }
	float get_max_flight_time() const { return max_flight_time; }
	void set_grain_material(const Ref<Material> &p_material) { grain_material = p_material; }
	Ref<Material> get_grain_material() const { return grain_material; }
	void set_mud_spray(bool p_enabled) { mud_spray = p_enabled; }
	bool is_mud_spray() const { return mud_spray; }
	PhysXGroundGrains3D *get_mud_grains() const { return mud_child; }
	void set_water_spray(bool p_enabled) { water_spray = p_enabled; }
	bool is_water_spray() const { return water_spray; }
	PhysXGroundGrains3D *get_water_grains() const { return water_child; }
	// Makes this node the water one (before it enters the tree): MPM liquid drawn as clear water.
	void set_as_water();
	// Makes this node the clay one (before it enters the tree): heavier, cohesive, drawn as wet clods.
	void set_as_mud();

	// Digs p_radius by p_depth out of the ground at p_position and throws it at p_velocity. Returns the volume.
	float kick(const Vector3 &p_position, float p_radius, float p_depth, const Vector3 &p_velocity);
	float get_dug_volume() const { return (float)dug_volume; }
	float get_deposited_volume() const { return (float)deposited_volume; }
	float get_stuck_volume() const { return (float)stuck_volume; }
	float get_water_mud() const { return water_mud; }
	float get_airborne_volume() const;
	int get_airborne_count() const { return live; }
	PackedVector3Array get_airborne_positions() const;

	PhysXGroundGrains3D();
};

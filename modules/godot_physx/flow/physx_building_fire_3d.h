/**************************************************************************/
/*  physx_building_fire_3d.h                                              */
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

#include "physx_firebrands.h"

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

#include "modules/world/world_contracts.h"

class OmniLight3D;
class PhysXFireSpread3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// A wooden building on fire, room by room (docs/ideas/physx.md, "Burning wooden buildings"): each room is a fire
// compartment that grows as t-squared, takes air through its openings (Kawagoe's ventilation limit), heats as
// McCaffrey, Quintiere and Harkleroad's hot layer, breaks its windows, flashes over at Thomas's heat release, burns
// ventilation-controlled with the excess burning outside as Eurocode's external flames, decays as its fuel goes,
// chars its timber and burns through its walls, floors and roof, and collapses. It catches from outside (a ground
// fire at its walls, radiation from burning things, firebrands on its roof and at its walls) and throws firebrands
// and smoke, radiates to its neighbours and reports its heat, plumes and smoke to WorldFire.
class PhysXBuildingFire3D : public Node3D, public WorldFireProvider {
	GDCLASS(PhysXBuildingFire3D, Node3D);

public:
	enum RoomState {
		ROOM_COLD,
		ROOM_GROWING,
		ROOM_FLASHED,
		ROOM_DECAY,
		ROOM_OUT,
	};
	enum Face {
		FACE_NEG_X,
		FACE_POS_X,
		FACE_NEG_Z,
		FACE_POS_Z,
		FACE_TOP,
		FACE_COUNT,
	};

private:
	struct Opening {
		int face = FACE_POS_X;
		Rect2 rect; // m on the face: x along it from the room's lower corner, y up from the floor (the top: x, z)
		bool glazed = true;
		bool broken = false;
		int to_room = -1; // another room on the far side: an inside door or hatch
		float flame_contact = 0.0f; // s of another window's flames on it
		float ground_dose = 0.0f; // (W/m2)^2 s on the ground under its flames
		float break_at = 450.0f; // C of the hot layer that drops its glass
		ObjectID emitter;
	};
	struct Room {
		AABB box; // node-local
		float fuel_load = 780.0f; // MJ/m2 of floor
		LocalVector<Opening> openings;
		RoomState state = ROOM_COLD;
		float t = 0.0f; // s since it lit
		float q = 0.0f; // kW inside
		float q_ext = 0.0f; // kW burning outside its openings
		float temperature = 20.0f; // C, the hot layer
		float fuel_total = 0.0f; // MJ
		float fuel_left = 0.0f;
		float q_decay = 0.0f; // kW when decay began
		float decay_t0 = 0.0f;
		float decay_time = 1.0f; // s
		float char_depth = 0.0f; // m into its timber
		float weakened = 0.0f;
		float ember = 0.0f; // glow left after it goes out, 0..1
		float flash_time = -1.0f; // s from its ignition to flashover
		bool roof_open = false;
		bool floor_open = false; // the floor burnt through or fell into the room below
		bool collapsed = false;
		bool exterior[FACE_COUNT] = { true, true, true, true, true };
		Vector3 origin; // node-local, where it lit
		// Its outside cladding (walls and roof covering) on each face.
		float clad_dose[FACE_COUNT] = {};
		float clad_r[FACE_COUNT] = {}; // m, the burning patch; 0: not burning
		float clad_char[FACE_COUNT] = {};
		Vector2 clad_origin[FACE_COUNT];
		float brand_glow[FACE_COUNT] = {};
		Vector2 brand_at[FACE_COUNT];
		float neighbour_dose = 0.0f;
		ObjectID light;
		ObjectID inner_emitter;
		ObjectID roof_emitter;
		ObjectID clad_emitter[FACE_COUNT];
	};
	LocalVector<Room> rooms;
	Array room_data;
	bool rooms_dirty = true;

	NodePath target = NodePath("..");
	NodePath fire_path;
	NodePath flow_path;
	float growth_time = 300.0f; // s to reach 1 MW (EN 1991-1-2: medium growth for dwellings)
	float heat_release_rate = 250.0f; // kW/m2 of floor, fuel-controlled (EN 1991-1-2, dwellings)
	float wall_thickness = 0.1f; // m of timber to burn through
	float floor_thickness = 0.04f;
	float roof_thickness = 0.03f;
	float member_size = 0.05f; // m, the smallest side of its framing
	float collapse_share = 0.6f; // share of a member's section charred at which it fails
	float glass_break_temperature = 450.0f; // C
	float smoke_yield = 0.03f; // kg of particulate per kg of fuel burnt
	float light_energy = 4.0f;
	bool firebrands = true;
	float firebrand_rate = 40.0f; // a second per MW
	float time_scale = 1.0f;
	float sample_timer = 0.0f;
	float flicker = 0.0f;
	uint32_t rng = 1u;

	PhysXFireSpread3D *fire = nullptr;
	PhysXFlow3D *flow = nullptr;
	ObjectID destructible_id;
	bool look_done = false;
	PhysXFirebrands brands;
	Vector3 wind;
	float t_air = 20.0f;

	static LocalVector<PhysXBuildingFire3D *> buildings;

	float _rand();
	Node3D *_target() const;
	void _resolve();
	void _build_rooms();
	static Vector3 _face_normal(int p_face);
	Vector3 _face_point(const Room &p_room, int p_face, const Vector2 &p_uv) const;
	Vector2 _face_size(const Room &p_room, int p_face) const;
	Vector2 _face_uv(const Room &p_room, int p_face, const Vector3 &p_local) const;
	float _vent(const Room &p_room) const; // sum of A sqrt(H) of its open openings, m^(5/2)
	float _lining_area(const Room &p_room) const;
	float _exposure(int p_room, int p_face, Vector3 &r_hot) const; // kW/m2 on its outside
	void _ignite_room(int p_room, const Vector3 &p_local);
	void _sample(float p_delta);
	void _step_room(int p_room, float p_delta);
	void _spread(float p_delta);
	void _weaken(int p_room);
	void _update_visuals(float p_delta);
	void _update_brands(float p_delta);
	void _update_look();
	PhysXFlowEmitter3D *_emitter(ObjectID &r_id, const String &p_name);
	static void _disable(ObjectID p_id);
	float _external_flame(const Room &p_room, const Opening &p_opening) const; // m above its top
	static float _radiant_flux_bind(const Vector3 &p_position) { return radiant_flux_at(p_position); }

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Radiant heat (kW/m2) at a point from every burning building: its flashed rooms' openings (blackbody panels at
	// the hot layer's temperature), its flames outside and its open roofs. p_skip_room of p_skip is left out.
	static float radiant_flux_at(const Vector3 &p_position, const PhysXBuildingFire3D *p_skip = nullptr, int p_skip_room = -1);
	// The building whose target is (or holds, or is held by) this collider, or null.
	static PhysXBuildingFire3D *find_by_collider(Object *p_collider);
	// A firebrand came to rest on it (world point and normal); p_glow is how much of it is left (0..1). One landing
	// in an open opening lands on the room's contents.
	void add_brand(const Vector3 &p_position, const Vector3 &p_normal, float p_glow);

	void ignite(const Vector3 &p_position);
	void extinguish();

	void set_rooms(const Array &p_rooms);
	Array get_rooms() const { return room_data; }
	int get_room_count() const { return rooms.size(); }
	RoomState get_room_state(int p_room) const;
	float get_room_heat_release(int p_room) const; // kW, inside and out
	float get_room_temperature(int p_room) const; // C
	float get_room_flashover_time(int p_room) const; // s from ignition, -1 before
	float get_room_char_depth(int p_room) const; // m
	float get_room_fuel_left(int p_room) const; // share
	bool is_room_collapsed(int p_room) const;
	int get_broken_windows() const;
	float get_heat_release() const; // kW, the whole building
	float get_smoke_rate() const; // kg/s of particulate
	int get_firebrand_count() const { return brands.get_count(); }
	int get_spot_fire_count() const { return brands.get_spot_fires(); }
	bool is_burning() const;

	void set_target(const NodePath &p_path);
	NodePath get_target() const { return target; }
	void set_fire(const NodePath &p_path);
	NodePath get_fire() const { return fire_path; }
	void set_flow(const NodePath &p_path);
	NodePath get_flow() const { return flow_path; }
	void set_growth_time(float p_v) { growth_time = MAX(p_v, 10.0f); }
	float get_growth_time() const { return growth_time; }
	void set_heat_release_rate(float p_v) { heat_release_rate = MAX(p_v, 10.0f); }
	float get_heat_release_rate() const { return heat_release_rate; }
	void set_wall_thickness(float p_v) { wall_thickness = MAX(p_v, 0.005f); }
	float get_wall_thickness() const { return wall_thickness; }
	void set_floor_thickness(float p_v) { floor_thickness = MAX(p_v, 0.005f); }
	float get_floor_thickness() const { return floor_thickness; }
	void set_roof_thickness(float p_v) { roof_thickness = MAX(p_v, 0.005f); }
	float get_roof_thickness() const { return roof_thickness; }
	void set_member_size(float p_v) { member_size = MAX(p_v, 0.01f); }
	float get_member_size() const { return member_size; }
	void set_collapse_share(float p_v) { collapse_share = CLAMP(p_v, 0.05f, 1.0f); }
	float get_collapse_share() const { return collapse_share; }
	void set_glass_break_temperature(float p_v) { glass_break_temperature = MAX(p_v, 50.0f); }
	float get_glass_break_temperature() const { return glass_break_temperature; }
	void set_smoke_yield(float p_v) { smoke_yield = MAX(p_v, 0.0f); }
	float get_smoke_yield() const { return smoke_yield; }
	void set_light_energy(float p_v) { light_energy = MAX(p_v, 0.0f); }
	float get_light_energy() const { return light_energy; }
	void set_firebrands(bool p_v) { firebrands = p_v; }
	bool has_firebrands() const { return firebrands; }
	void set_firebrand_rate(float p_v) { firebrand_rate = MAX(p_v, 0.0f); }
	float get_firebrand_rate() const { return firebrand_rate; }
	void set_time_scale(float p_v) { time_scale = MAX(p_v, 0.0f); }
	float get_time_scale() const { return time_scale; }

	// WorldFire.
	float contract_fire_heat_at(const Vector3 &p_position) const override;
	float contract_fire_burnt_at(const Vector3 &p_position) const override;
	void contract_fire_plumes(PackedVector4Array &r_plumes) const override;
	void contract_fire_smoke(PackedVector4Array &r_sources) const override;

	PhysXBuildingFire3D();
	~PhysXBuildingFire3D();
};

VARIANT_ENUM_CAST(PhysXBuildingFire3D::RoomState);
VARIANT_ENUM_CAST(PhysXBuildingFire3D::Face);

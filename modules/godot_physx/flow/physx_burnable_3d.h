/**************************************************************************/
/*  physx_burnable_3d.h                                                   */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "physx_firebrands.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class MeshInstance3D;
class MultiMesh;
class MultiMeshInstance3D;
class PhysXFireSpread3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Wood and debris that catch fire, burn with Flow flames, char and throw embers (docs/ideas/physx.md, "Burning wood
// and debris, embers spot new fires").
class PhysXBurnable3D : public Node3D {
	GDCLASS(PhysXBurnable3D, Node3D);

public:
	enum State {
		STATE_UNBURNT,
		STATE_FLAMING,
		STATE_SMOULDERING,
		STATE_BURNT_OUT,
	};

private:
	NodePath target = NodePath("..");
	NodePath fire_path;
	NodePath flow_path;
	float fuel_mass = 0.0f; // kg; 0: from the bounds (wood at 500 kg/m3, a third of the box)
	float heat_release = 150.0f; // kW/m2 of burning surface
	float flame_spread = 0.04f; // m/s over the surface (upward spread is fast, lateral slow)
	float ignition_temperature = 350.0f; // C
	float flame_strength = 1.0f;
	bool embers = true;
	float ember_rate = 30.0f; // embers a second per MW
	float glow_energy = 6.0f;

	State state = STATE_UNBURNT;
	float dose = 0.0f; // integral of q^2 dt, (W/m2)^2 s
	float fuel_initial = 0.0f;
	float fuel_left = 0.0f;
	float burn_radius = 0.0f; // m, from the ignition point
	float hrr = 0.0f; // W
	float smoulder = 0.0f; // 0..1
	float char_amount = 0.0f; // 0..1, the share of the surface charred
	float rain_water = 0.0f; // mm held
	float sample_timer = 0.0f;
	float heat_in = 0.0f; // kW/m2, last sampled
	Vector3 origin_local; // ignition point, target-local
	AABB bounds; // target-local, of the meshes
	bool bounds_ready = false;
	uint32_t rng = 1u;

	struct MeshRef {
		ObjectID id;
		Transform3D to_mesh; // target-local to mesh-local
	};
	LocalVector<MeshRef> meshes;
	bool materials_done = false;

	PhysXFireSpread3D *fire = nullptr;
	PhysXFlow3D *flow = nullptr;
	ObjectID emitters[5]; // one per burning face: four sides and the top

	struct Ember {
		Vector3 p;
		Vector3 v;
		float age = 0.0f;
		float life = 1.0f;
	};
	LocalVector<Ember> ember_list; // sparks: short-lived, for the look
	// Firebrands: bark and char flakes lofted by the fire's own column that can start spot fires where they land on a
	// PhysXFireSpread3D's ground (the same flight as the ground fire's).
	PhysXFirebrands brands;
	// Brands resting on it (ember attack, Manzello et al. 2008, 2020): they pile up where they settle (top faces,
	// corners, against an edge), glow, and heat the wood under the pile; a pile big enough lights it.
	struct BrandSpot {
		Vector3 local; // target-local
		float glow = 0.0f; // glowing brands in the pile, each weighed by how much of it is left
	};
	LocalVector<BrandSpot> brand_spots;
	float _brand_flux(Vector3 &r_hot) const; // kW/m2 under the hottest pile
	Vector3 brand_wind;
	float ember_carry = 0.0f;
	MultiMeshInstance3D *ember_mmi = nullptr;
	Ref<MultiMesh> ember_mm;

	static LocalVector<PhysXBurnable3D *> burnables;
	static HashMap<RID, Ref<Material>> material_cache;

	Node3D *_target() const;
	void _gather();
	void _resolve();
	float _rand();
	Transform3D _target_xform() const;
	void _wet_from_fluids(const Transform3D &p_xf);
	void _sample_heat(float p_delta);
	void _burn(float p_delta);
	void _update_emitter();
	void _update_embers(float p_delta);
	void _update_look();
	float _surface_area() const;
	static Ref<Material> _burn_material(const Ref<Material> &p_base);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Radiant heat (kW/m2) at a point from every flaming burnable, as a point source each.
	static float radiant_flux_at(const Vector3 &p_position);
	// The burnable whose target is (or holds, or is held by) this collider, or null.
	static PhysXBurnable3D *find_by_collider(Object *p_collider);
	// A firebrand came to rest on it at a world point with this surface normal; p_glow is how much of the brand is
	// left (0..1).
	void add_brand(const Vector3 &p_position, const Vector3 &p_normal, float p_glow);
	int get_brand_pile_count() const;
	static void free_materials();
	void ignite(const Vector3 &p_position);
	void extinguish();
	State get_state() const { return state; }
	bool is_burning() const { return state == STATE_FLAMING; }
	float get_heat_release() const { return hrr / 1000.0f; } // kW
	float get_heat_flux() const { return heat_in; } // kW/m2 received
	float get_fuel_left() const { return fuel_left; }
	float get_char_amount() const { return char_amount; }
	int get_ember_count() const { return (int)ember_list.size(); }
	int get_firebrand_count() const { return brands.get_count(); }
	int get_spot_fire_count() const { return brands.get_spot_fires(); }

	void set_target(const NodePath &p_path);
	NodePath get_target() const { return target; }
	void set_fire(const NodePath &p_path);
	NodePath get_fire() const { return fire_path; }
	void set_flow(const NodePath &p_path);
	NodePath get_flow() const { return flow_path; }
	void set_fuel_mass(float p_v) { fuel_mass = MAX(p_v, 0.0f); }
	float get_fuel_mass() const { return fuel_mass; }
	void set_heat_release(float p_v) { heat_release = MAX(p_v, 1.0f); }
	float get_heat_release_rate() const { return heat_release; }
	void set_flame_spread(float p_v) { flame_spread = MAX(p_v, 0.0f); }
	float get_flame_spread() const { return flame_spread; }
	void set_ignition_temperature(float p_v) { ignition_temperature = MAX(p_v, 30.0f); }
	float get_ignition_temperature() const { return ignition_temperature; }
	void set_flame_strength(float p_v) { flame_strength = MAX(p_v, 0.0f); }
	float get_flame_strength() const { return flame_strength; }
	void set_embers(bool p_v) { embers = p_v; }
	bool has_embers() const { return embers; }
	void set_ember_rate(float p_v) { ember_rate = MAX(p_v, 0.0f); }
	float get_ember_rate() const { return ember_rate; }
	void set_glow_energy(float p_v) { glow_energy = MAX(p_v, 0.0f); }
	float get_glow_energy() const { return glow_energy; }

	PhysXBurnable3D();
	~PhysXBurnable3D();
};

VARIANT_ENUM_CAST(PhysXBurnable3D::State);

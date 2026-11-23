/**************************************************************************/
/*  sod_lip_fall_3d.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"

class RigidBody3D;

// Sod lips breaking off (docs/ideas/vegetation.md, round 8): the turf lips VegetationField3D stands along cut banks
// (PlantGenerator.PRESET_TURF_LIP) fail as cantilevers do (Thorne and Tovey 1981): under a load on the overhang, as
// the water at their foot cuts further under them, and when soaked. Twice a second this looks at the lips within
// range of the camera; one that fails is taken out of its field (add_plant_removal) and falls as a rigid block of sod
// tipping out over the drop, to lie at the bank's foot.
// Round 9: a block that lands hard breaks (a PhysXDestructible3D of the lip cut across its length along root-torn
// cracks, the soil showing on the breaks); blocks and pieces in water are buoyed and dragged by WorldWater.flow_at,
// so a fast river carries the small pieces off while slower water leaves the heavy blocks at the toe (Henshaw,
// SERC "Composite river bank erosion in mid-Wales": some blocks are entrained, some break up on impact, others remain).
class SodLipFall3D : public Node3D {
	GDCLASS(SodLipFall3D, Node3D);

	bool enabled = true;
	float range = 40.0f;
	StringName load_group = "grass_trample"; // Bodies whose weight on an overhang breaks it (players, animals).
	float undercut_time = 600.0f; // s: mean time to fail with water at the foot (estimate; game time scale)
	float soaked_time = 3600.0f; // s: mean time to fail soaked in heavy rain (estimate)
	float density = 1500.0f; // kg/m3, wet topsoil bound by roots (estimate)
	int max_blocks = 24; // Blocks still simulated; older ones are frozen where they lie.
	float break_speed = 2.5f; // m/s: a landing that stops the block this fast breaks it (estimate; about a 0.3 m drop)
	float drag_coefficient = 1.05f; // A cube's face-on (the usual textbook value)

	double check_left = 0.0;
	struct Fallen {
		RigidBody3D *body = nullptr;
		ObjectID pieces; // The PhysXDestructible3D it broke into on landing.
		double age = 0.0;
		Vector3 last_velocity;
		bool landed = false;
		Ref<Mesh> mesh;
		Vector3 scale = Vector3(1, 1, 1);
		Vector3 centre; // The body's origin in mesh space.
		float volume = 0.0f; // m3, scaled
		float height = 0.2f; // m, scaled
	};
	LocalVector<Fallen> fallen;
	int falls = 0;
	int breaks = 0;
	// Blast assets of the lip meshes cut into pieces, built on a worker when a lip first falls.
	struct PieceJob {
		Array surfaces;
		Array materials;
		int centre_surface = 0;
		uint64_t seed = 0;
		float volume = 0.0f;
		Ref<Resource> asset;
		uint64_t task = 0;
	};
	HashMap<ObjectID, Ref<Resource>> piece_assets;
	HashMap<ObjectID, AABB> soil_boxes; // The lip meshes' soil bounds (their blocks' collision).
	HashMap<ObjectID, PieceJob *> piece_jobs;
	static void _piece_job(PieceJob *p_job);
	void _piece_job_member(PieceJob *p_job) { _piece_job(p_job); }
	void _request_pieces(const Ref<Mesh> &p_mesh);
	Ref<Resource> _pieces_for(const Ref<Mesh> &p_mesh);
	void _land(Fallen &r_f, float p_speed_change);
	void _carry(double p_delta);
	void _process_fallen(double p_delta);
	HashMap<uint64_t, bool> gone; // Lips already broken (by position key) while their tile is placed again.

	Vector3 _camera_position() const;
	void _break(Node *p_field, int p_species, const Transform3D &p_xf, const Ref<class Mesh> &p_mesh, const String &p_cause);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// One look over the lips now (process does it every 0.5 s, p_delta the time since the last look).
	void check(double p_delta);
	// Breaks the lip of the scene's fields nearest p_position within 1 m; false if there is none.
	bool break_at(const Vector3 &p_position);
	int get_fall_count() const { return falls; }
	// Blocks that broke on landing so far.
	int get_break_count() const { return breaks; }
	// The falling or lying blocks (RigidBody3D) and the broken ones (PhysXDestructible3D).
	Array get_blocks() const;
	~SodLipFall3D();

	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_range(float p_v) { range = MAX(p_v, 1.0f); }
	float get_range() const { return range; }
	void set_load_group(const StringName &p_v) { load_group = p_v; }
	StringName get_load_group() const { return load_group; }
	void set_undercut_time(float p_v) { undercut_time = MAX(p_v, 1.0f); }
	float get_undercut_time() const { return undercut_time; }
	void set_soaked_time(float p_v) { soaked_time = MAX(p_v, 1.0f); }
	float get_soaked_time() const { return soaked_time; }
	void set_density(float p_v) { density = MAX(p_v, 100.0f); }
	float get_density() const { return density; }
	void set_max_blocks(int p_v) { max_blocks = CLAMP(p_v, 1, 256); }
	int get_max_blocks() const { return max_blocks; }
	void set_break_speed(float p_v) { break_speed = MAX(p_v, 0.1f); }
	float get_break_speed() const { return break_speed; }
	void set_drag_coefficient(float p_v) { drag_coefficient = MAX(p_v, 0.0f); }
	float get_drag_coefficient() const { return drag_coefficient; }
};

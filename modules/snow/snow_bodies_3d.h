/**************************************************************************/
/*  snow_bodies_3d.h                                                      */
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

#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class GeometryInstance3D;
class SnowCover3D;

// Snow that moving things carry (docs/ideas/snow.md, follow-up 18). Every rigid body, vehicle and cloth (and any
// node in the snow_catch group; snow_ignore opts out) keeps its own snow load: falling snow builds it where the sky
// is open above, the cover's depth seeds it on first sight, wind over the blowing threshold (Li and Pomeroy 1997,
// raised for wet and settled snow) strips it, hard shakes and jolts throw it off, warm air, rain and fire melt it.
// The load reaches the body's materials through BaseMaterial3D snow_on_body (instance uniform slot 12), the same
// in raster and path tracing. SnowCover3D adds one by itself while bodies_enabled.
class SnowBodies3D : public Node3D {
	GDCLASS(SnowBodies3D, Node3D);

	struct Body {
		ObjectID id;
		float depth = 0.0f; // m
		float density = 100.0f; // kg/m3
		float wet = 0.0f; // 0..1
		float albedo = 0.85f;
		float exposure = 1.0f; // 1 where the sky is open above
		Vector3 last_position;
		Vector3 last_velocity;
		bool seen = false;
		double ray_accum = 1.0e9;
		double geometry_accum = 1.0e9;
		Vector<ObjectID> geometry;
		float top_area = 0.0f; // m2, the body's footprint seen from above
		Vector3 top_offset; // from the body's origin to the middle of its top, world axes
		float top_radius = 0.5f; // m
		float load_mass = 0.0f; // kg
		float cloth_base = -1.0f; // a cloth's own snow_load before ours (none: -1)
		Vector4 sent = Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
		// A roof face (the snow_roof group, follow-up 50): the bed's dry friction angle (from its
		// snow_roof_material meta) and the heat the building loses through it (snow_roof_heat meta, W/m2).
		bool roof = false;
		float roof_phi = 25.0f; // degrees
		float roof_heat = 0.0f; // W/m2
		// Round 8 (follow-up 52): the ice dam at its eave (kg of ice per metre of eave) and the mesh that shows it.
		float dam = 0.0f;
		float dam_drawn = 0.0f; // m, the height the mesh was built for
		ObjectID dam_mesh;
	};
	// A piece of a roof slab falling as a rigid body (follow-up 52), until it comes to rest in the cover.
	struct Chunk {
		ObjectID id;
		float mass = 0.0f; // kg
		float density = 300.0f;
		float size = 1.0f; // m, its edge along the eave
		float age = 0.0f; // s
		float rest = 0.0f; // s it has been still
		Vector3 last_velocity;
		bool broken = false;
		bool dry = true;
	};
	struct PendingChunk { // a row of the slab still on the roof, leaving the eave at its time
		float wait = 0.0f; // s
		Transform3D xform;
		Vector3 velocity;
		Vector3 size;
		float mass = 0.0f;
		float density = 300.0f;
		bool dry = true;
	};
	struct Puff {
		Vector3 position;
		float rate = 0.0f; // kg/s
		float age = 0.0f;
	};
	LocalVector<Chunk> chunks;
	LocalVector<PendingChunk> pending_chunks;
	LocalVector<Puff> puffs;
	Node3D *puff_cloud = nullptr; // a SnowPowderCloud3D made on the first dry impact
	Ref<StandardMaterial3D> chunk_material;
	Ref<StandardMaterial3D> dam_material;
	bool roof_slab_bodies = true;
	float slab_chunk_size = 1.2f; // m, a falling piece's edge
	bool ice_dams = true;
	int chunks_spawned = 0;
	bool _roof_extent(const Body &r_body, const Vector3 &p_down, float p_slope, Vector3 &r_eave, Vector3 &r_across, float &r_half_len, float &r_run) const;
	void _spawn_chunk(const PendingChunk &p_chunk, float p_size_along);
	void _chunks_step(float p_dt);
	void _deposit_chunk(const Chunk &p_chunk, const Vector3 &p_at);
	void _update_dam_mesh(Body &r_body, const Vector3 &p_down, float p_slope);

	HashMap<ObjectID, Body> bodies;
	float update_rate = 10.0f; // Hz
	float shake_threshold = 0.5f; // g
	float shake_shed_rate = 3.0f; // per second per g over the threshold
	float wind_shed_rate = 0.08f; // per second per m/s over the threshold
	float max_depth = 0.4f; // m
	float roof_max_depth = 1.5f; // m on a roof face
	bool roof_piles = true; // what slides off a roof lands in the snow cover below the eave
	int roof_slides = 0;
	float roof_landed = 0.0f; // kg landed in the cover from roofs
	void _land_pile(const Body &r_body, Node3D *p_node, float p_depth_lost, const Vector3 &p_down, float p_slope);
	bool auto_materials = true;
	bool auto_track = true;
	bool weight_enabled = true;
	bool slide_enabled = true;
	bool clumps_enabled = true;
	float slab_cohesion = 250.0f; // Pa, dry snow's bond to the surface under it
	double accum = 0.0;
	int clumps_dropped = 0;
	int slabs_slid = 0;

	void _consider(Node *p_node);
	void _scan(Node *p_node);
	void _on_node_added(Node *p_node);
	void _collect_geometry(Body &r_body, Node *p_node);
	void _prepare_geometry(GeometryInstance3D *p_geometry);
	void _step(double p_dt);
	void _measure(Body &r_body, Node3D *p_node);
	void _shed(Body &r_body, Node3D *p_node, float p_depth_lost, const Vector3 &p_toward, float p_temperature);
	void _apply_weight();
	float _exposure(Node3D *p_node);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static bool is_carrier(const Node *p_node);

	void track(Node *p_node);
	void untrack(Node *p_node);
	bool is_tracked(Node *p_node) const;
	int get_tracked_count() const { return bodies.size(); }
	TypedArray<Node> get_tracked() const;
	float get_body_depth(Node *p_node) const; // m
	float get_body_density(Node *p_node) const; // kg/m3
	float get_body_wetness(Node *p_node) const; // 0..1
	void set_body_depth(Node *p_node, float p_depth, float p_density = -1.0f);
	void step(double p_dt); // one update now, for tests
	float get_body_load_mass(Node *p_node) const; // kg
	float get_body_top_area(Node *p_node) const; // m2
	int get_clumps_dropped() const { return clumps_dropped; }
	int get_slabs_slid() const { return slabs_slid; }
	int get_roof_slides() const { return roof_slides; }
	float get_roof_landed() const { return roof_landed; }
	void set_roof_piles(bool p_enable) { roof_piles = p_enable; }
	bool is_roof_piles_enabled() const { return roof_piles; }
	void set_roof_max_depth(float p_m) { roof_max_depth = MAX(p_m, 0.0f); }
	float get_roof_max_depth() const { return roof_max_depth; }
	static bool slab_slides(float p_depth, float p_density, float p_wetness, float p_slope, float p_cohesion, float p_friction_deg = 25.0f);
	static float roof_shape(float p_slope); // Eurocode 1 (EN 1991-1-3) mu1: the share of the ground load a roof holds
	static float roof_friction(const String &p_material); // degrees, dry
	static float ice_dam_height(float p_kg_per_m); // m, a ridge twice as wide as high
	float get_ice_dam_height(Node *p_node) const;
	int get_falling_chunks() const { return int(chunks.size() + pending_chunks.size()); }
	int get_chunks_spawned() const { return chunks_spawned; }
	void set_roof_slab_bodies(bool p_enable) { roof_slab_bodies = p_enable; }
	bool is_roof_slab_bodies_enabled() const { return roof_slab_bodies; }
	void set_ice_dams(bool p_enable) { ice_dams = p_enable; }
	bool is_ice_dams_enabled() const { return ice_dams; }

	void set_update_rate(float p_rate);
	float get_update_rate() const { return update_rate; }
	void set_shake_threshold(float p_g);
	float get_shake_threshold() const { return shake_threshold; }
	void set_shake_shed_rate(float p_rate);
	float get_shake_shed_rate() const { return shake_shed_rate; }
	void set_wind_shed_rate(float p_rate);
	float get_wind_shed_rate() const { return wind_shed_rate; }
	void set_max_depth(float p_depth);
	float get_max_depth() const { return max_depth; }
	void set_auto_materials(bool p_enable) { auto_materials = p_enable; }
	bool is_auto_materials() const { return auto_materials; }
	void set_auto_track(bool p_enable) { auto_track = p_enable; }
	bool is_auto_track() const { return auto_track; }
	void set_weight_enabled(bool p_enable) { weight_enabled = p_enable; }
	bool is_weight_enabled() const { return weight_enabled; }
	void set_slide_enabled(bool p_enable) { slide_enabled = p_enable; }
	bool is_slide_enabled() const { return slide_enabled; }
	void set_clumps_enabled(bool p_enable) { clumps_enabled = p_enable; }
	bool is_clumps_enabled() const { return clumps_enabled; }
	void set_slab_cohesion(float p_pa) { slab_cohesion = MAX(p_pa, 0.0f); }
	float get_slab_cohesion() const { return slab_cohesion; }
};

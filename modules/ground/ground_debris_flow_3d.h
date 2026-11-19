/**************************************************************************/
/*  ground_debris_flow_3d.h                                               */
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

#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

// A post-fire debris flow (docs/ideas/mud.md, after round 12). WeatherRunoff3D fires the event (WorldGround's
// debris_flow signal: an outlet, a volume from Gartner et al. 2014, the channel path downhill with its bankfull widths
// and burn severity); this node runs the moving body and lays its deposit:
// - a depth-averaged flow on a corridor of cells along the path (local inertial shallow water, Bates et al. 2010, as
//   the ground's runoff), against a Voellmy basal resistance, tau = mu rho g h + rho g u^2 / xi (RAMMS: Christen,
//   Kowalski and Bartelt 2010), plus the slurry's yield stress (O'Brien, Julien and Fullerton 1993), so it stops by
//   itself where the ground flattens, as a fan;
// - fed at the outlet by a triangular hydrograph peaking at Q_p = 0.1 V^(5/6) (Rickenmann 1999);
// - bulking along the channel by entrainment at McDougall and Hungr's (2005) growth rate E (dV/ds = E V), per area
//   E h |u|, scaled by the path's burn severity and held to the bed's loose depth;
// - what stops is the deposit: wet slurry for the ground (GroundSurface3D.add_mud_deposit, so WorldGround's mud reads
//   it), raised ground where a TerrainDeformer3D is given (TerrainBrush3D add brushes), and sediment for a river or
//   lake it runs into (WorldWater.add_sediment);
// - small events whose outlets join a running body's path feed that body (steep burnt valleys fire dozens at once).
// The front (the furthest cell the flow covers) is what PhysXDebrisSurge3D follows: get_debris_front(id).
class GroundDebrisFlow3D : public Node3D {
	GDCLASS(GroundDebrisFlow3D, Node3D);

public:
	struct Body {
		int64_t id = -1;
		LocalVector<int64_t> pooled; // events fed into this body
		int ns = 0; // cells along the path
		int nw = 0; // cells across
		float cell = 1.5f;
		LocalVector<Vector3> center; // the path resampled, world, per row
		LocalVector<Vector3> normal; // across the path, horizontal, per row
		LocalVector<float> path_index; // the source path's (fractional) index per row
		LocalVector<float> half_channel; // m, half the bankfull width per row
		LocalVector<float> severity; // per row
		LocalVector<float> bed; // m, per cell (the ground, lowered by scour)
		LocalVector<float> scoured; // m, per cell
		LocalVector<float> loose; // m, the bed's loose depth, per cell
		LocalVector<float> h; // m, flow depth per cell
		LocalVector<float> qs; // m2/s, along-path flux on the faces (ns + 1) x nw
		LocalVector<float> qn; // m2/s, across flux on the faces ns x (nw + 1)
		struct Feed {
			int row = 0;
			float volume = 0.0f; // m3
			float start = 0.0f; // s after the body started
		};
		LocalVector<Feed> feeds;
		float time = 0.0f; // s
		float still = 0.0f; // s with nothing moving
		float volume_in = 0.0f; // m3 fed so far
		float volume_entrained = 0.0f; // m3
		float volume_out = 0.0f; // m3 run off the corridor's end
		bool done = false;
		bool deposited = false;
		// The front, updated each step.
		Vector3 front;
		float front_index = 0.0f;
		float front_depth = 0.0f;
		float front_speed = 0.0f;
		float front_width = 0.0f;
		MeshInstance3D *mesh = nullptr;
		float mesh_accum = 0.0f;
	};

private:
	bool listen = true;
	float cell_size = 1.5f;
	float corridor_width = 60.0f; // m across
	float voellmy_mu = 0.1f;
	float voellmy_xi = 400.0f; // m/s2
	float yield_stress = 1500.0f; // Pa
	float density = 1900.0f; // kg/m3
	float entrainment = 0.002f; // 1/m at full burn severity
	float max_scour = 0.6f; // m
	float pool_distance = 30.0f; // m
	float time_scale = 1.0f;
	float sediment_share = 0.55f; // the solids' volume share of the slurry (debris flows: 0.4 to 0.8)
	NodePath terrain_deformer;
	bool show_flow = true;
	Ref<StandardMaterial3D> flow_material;

	LocalVector<Body *> bodies;
	bool connected = false;

	void _connect();
	void _disconnect();
	void _on_debris_flow(const Dictionary &p_event);
	void _step(Body &p_body, float p_dt);
	void _update_front(Body &p_body);
	void _deposit(Body &p_body);
	void _update_mesh(Body &p_body);
	float _ground_height(const Vector3 &p_position) const;
	Vector3 _cell_position(const Body &p_body, int p_row, int p_col) const;
	Body *_find(int64_t p_id) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Starts (or pools) a body from an event Dictionary (the keys of WeatherRunoff3D's debris_flow).
	void start(const Dictionary &p_event);
	void clear();
	// Runs every body p_seconds at once (tests).
	void advance(float p_seconds, float p_step = 0.05f);
	Dictionary get_debris_front(int64_t p_event_id) const;
	int get_body_count() const { return (int)bodies.size(); }
	int get_active_bodies() const;
	float get_volume(int64_t p_event_id) const; // m3 in the body now
	// The body's volume budget, m3: fed in, entrained, run off the corridor's end, and in it now.
	Dictionary get_budget(int64_t p_event_id) const;
	float get_deposit_depth_at(const Vector3 &p_position) const; // m, from finished bodies
	float get_flow_depth_at(const Vector3 &p_position) const; // m, moving now

	void set_listen(bool p_v);
	bool is_listening() const { return listen; }
	void set_cell_size(float p_v) { cell_size = CLAMP(p_v, 0.5f, 5.0f); }
	float get_cell_size() const { return cell_size; }
	void set_corridor_width(float p_v) { corridor_width = CLAMP(p_v, 4.0f, 200.0f); }
	float get_corridor_width() const { return corridor_width; }
	void set_voellmy_mu(float p_v) { voellmy_mu = CLAMP(p_v, 0.0f, 1.0f); }
	float get_voellmy_mu() const { return voellmy_mu; }
	void set_voellmy_xi(float p_v) { voellmy_xi = MAX(p_v, 10.0f); }
	float get_voellmy_xi() const { return voellmy_xi; }
	void set_yield_stress(float p_v) { yield_stress = MAX(p_v, 0.0f); }
	float get_yield_stress() const { return yield_stress; }
	void set_density(float p_v) { density = MAX(p_v, 1000.0f); }
	float get_density() const { return density; }
	void set_entrainment(float p_v) { entrainment = MAX(p_v, 0.0f); }
	float get_entrainment() const { return entrainment; }
	void set_max_scour(float p_v) { max_scour = MAX(p_v, 0.0f); }
	float get_max_scour() const { return max_scour; }
	void set_pool_distance(float p_v) { pool_distance = MAX(p_v, 0.0f); }
	float get_pool_distance() const { return pool_distance; }
	void set_time_scale(float p_v) { time_scale = MAX(p_v, 0.0f); }
	float get_time_scale() const { return time_scale; }
	void set_terrain_deformer(const NodePath &p_path) { terrain_deformer = p_path; }
	NodePath get_terrain_deformer() const { return terrain_deformer; }
	void set_show_flow(bool p_v) { show_flow = p_v; }
	bool is_show_flow() const { return show_flow; }

	GroundDebrisFlow3D();
	~GroundDebrisFlow3D();
};

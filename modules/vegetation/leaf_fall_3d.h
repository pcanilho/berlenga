/**************************************************************************/
/*  leaf_fall_3d.h                                                        */
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
/**************************************************************************/

#pragma once

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;

// Falling and blown leaves (docs/ideas/wind.md, "Leaves"): leaves detach from the crowns near the camera as
// the season sheds them and the wind tears them off, fall as flat plates through the shared wind
// (WeatherMap3D) with the quasi-steady fluttering and tumbling model of Andersen, Pesavento and Wang (2005),
// settle on the ground and lift again when the near-ground wind passes the threshold for loose litter.
// Drawn as one MultiMesh per leaf mesh, so the path tracer sees real leaves.
class LeafFall3D : public Node3D {
	GDCLASS(LeafFall3D, Node3D);

public:
	struct Crown {
		Vector3 center; // world, the crown's middle
		Vector3 radii = Vector3(3, 3, 3); // ellipsoid half sizes
		int kind = 0; // index into leaf kinds
		float leaves = 20000.0f; // leaves on the crown
		float shed_daily = -1.0f; // share of its leaves the season sheds a day; < 0 uses shed_rate
		bool automatic = false; // made from a VegetationField3D's crowns
	};
	struct Kind {
		Ref<Mesh> mesh;
		float length = 0.08f; // m, chord
		float area = 0.003f; // m2
		float mass = 0.0003f; // kg
		float flutter = 1.0f; // 0: falls straight (needles), 1: a broad leaf's full lift and tumbling
		int group_first = -1; // kinds made together (a species' variants): a crown picks among them
		int group_count = 1;
	};

private:
	struct Leaf {
		Vector3 p;
		Vector3 v;
		Basis b; // columns: chord, normal (y), span
		Vector3 w; // angular velocity, world
		float age = 0.0f;
		float lying = -1.0f; // seconds on the ground; < 0 aloft
		float scale = 1.0f;
		uint16_t kind = 0;
		bool alive = false;
	};

	bool enabled = true;
	int max_leaves = 3000;
	float range = 60.0f; // m from the camera that leaves are kept and crowns shed
	float shed_rate = 0.002f; // share of a crown's leaves a day that the season alone sheds (autumn peak)
	float wind_shed = 1.0f; // extra shedding at 10 m/s, as a multiple of the season's (grows with the wind squared)
	float lift_speed = 4.0f; // m/s at 10 cm over the ground that lifts loose dry leaves (litter threshold)
	float litter_density = 0.0f; // loose leaves per m2 on the ground near the camera that the wind can take
	float lie_time = 180.0f; // s a leaf lies before it is recycled (it joins the litter)
	float air_density = 1.2f;
	NodePath weather_map;
	bool auto_crowns = true; // take the crowns near the camera from the scene's VegetationField3D nodes
	NodePath vegetation_field; // one field only; empty: every VegetationField3D in the scene
	float leaf_area_index = 4.0f; // one-sided leaf area per crown ground area, for the leaves a crown holds
	float auto_timer = 0.0f;
	int auto_day = -1;
	Dictionary auto_season, auto_season_next;
	HashMap<int, int> preset_kind; // tree preset -> first kind, made on the day in preset_kind_day
	HashMap<int, int> preset_kind_day;
	HashMap<int, float> preset_shed; // share a day, today

	LocalVector<Kind> kinds;
	LocalVector<Crown> crowns;
	LocalVector<Leaf> leaves;
	LocalVector<uint32_t> free_slots;
	LocalVector<float> shed_accum; // per crown
	float litter_accum = 0.0f;
	RandomPCG rng;
	LocalVector<MultiMeshInstance3D *> draws; // per kind
	LocalVector<Ref<MultiMesh>> multimeshes;
	int alive_count = 0;
	int lifted_count = 0;
	float step_ms = 0.0f;

	Vector3 _camera_position() const;
	float _ground_at(const Vector3 &p_p) const;
	void _spawn(const Vector3 &p_p, const Vector3 &p_v, int p_kind, bool p_lying);
	void _step(float p_delta);
	void _step_leaf(Leaf &p_leaf, float p_delta, const class WeatherMap3D *p_weather);
	void _rebuild_draws();
	void _upload();
	void _refresh_auto_crowns(const class WeatherMap3D *p_weather, const Vector3 &p_camera);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_max_leaves(int p_v);
	int get_max_leaves() const { return max_leaves; }
	void set_range(float p_v) { range = MAX(p_v, 1.0f); }
	float get_range() const { return range; }
	void set_shed_rate(float p_v) { shed_rate = MAX(p_v, 0.0f); }
	float get_shed_rate() const { return shed_rate; }
	void set_wind_shed(float p_v) { wind_shed = MAX(p_v, 0.0f); }
	float get_wind_shed() const { return wind_shed; }
	void set_lift_speed(float p_v) { lift_speed = MAX(p_v, 0.1f); }
	float get_lift_speed() const { return lift_speed; }
	void set_litter_density(float p_v) { litter_density = MAX(p_v, 0.0f); }
	float get_litter_density() const { return litter_density; }
	void set_lie_time(float p_v) { lie_time = MAX(p_v, 0.0f); }
	float get_lie_time() const { return lie_time; }
	void set_weather_map(const NodePath &p_v) { weather_map = p_v; }
	NodePath get_weather_map() const { return weather_map; }
	void set_auto_crowns(bool p_v);
	bool is_auto_crowns() const { return auto_crowns; }
	void set_vegetation_field(const NodePath &p_v) { vegetation_field = p_v; auto_timer = 0.0f; }
	NodePath get_vegetation_field() const { return vegetation_field; }
	void set_leaf_area_index(float p_v) { leaf_area_index = MAX(p_v, 0.0f); }
	float get_leaf_area_index() const { return leaf_area_index; }
	// The share of a tree preset's leaves the season sheds a day, as the automatic crowns use it.
	float get_preset_shed_daily(int p_preset) const;
	// Take the automatic crowns round a point now (they refresh every 2 s round the camera on their own).
	void refresh_crowns_near(const Vector3 &p_position);

	// A leaf kind: its mesh at true size (chord along x, blade normal along y) and its flat-plate numbers.
	int add_kind(const Ref<Mesh> &p_mesh, float p_length, float p_area, float p_mass, float p_flutter);
	// A tree species' falling leaves from TreeGenerator (make_leaf_mesh, get_leaf_physics): p_variants kinds
	// (curled, dead brown), dry leaves' mass by default. Returns the first kind; crowns given it pick among all.
	int add_tree_kinds(int p_preset, const Dictionary &p_season, int p_variants = 4, bool p_dry = true);
	void clear_kinds();
	int get_kind_count() const { return kinds.size(); }
	// A crown that sheds: its middle, its half sizes, its leaf kind and how many leaves it holds.
	int add_crown(const Vector3 &p_center, const Vector3 &p_radii, int p_kind, float p_leaves);
	void clear_crowns();
	int get_crown_count() const { return crowns.size(); }
	// Drop a leaf now (tests, scripted gusts).
	void drop_leaf(const Vector3 &p_position, int p_kind);

	int get_leaf_count() const { return alive_count; }
	int get_lifted_count() const { return lifted_count; }
	float get_step_time() const { return step_ms; }
	// Per leaf, for checks: position, velocity, normal; lying flag in w of the position.
	PackedVector4Array get_leaf_states() const;
	PackedVector3Array get_leaf_velocities() const;
	PackedVector3Array get_leaf_normals() const;

	// The quasi-steady plate model alone (one leaf, still air or a given wind), for the bench: steps p_seconds
	// and returns the path as (x, y, z, angle of the normal from up in degrees).
	static PackedVector4Array simulate_plate(float p_length, float p_area, float p_mass, float p_flutter, const Vector3 &p_wind, float p_tilt_degrees, float p_seconds, float p_air_density = 1.2f);
};

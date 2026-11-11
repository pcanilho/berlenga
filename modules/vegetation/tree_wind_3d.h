/**************************************************************************/
/*  tree_wind_3d.h                                                        */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class WeatherMap3D;

// Wind for rigged trees (TreeGenerator.rigged), docs/ideas/trees.md section 3a. Every bone is a
// damped cantilever spring; its motion is the turbulence filtered by that oscillator, in closed
// form (Davenport 1961; Shinya and Fournier 1992; Stam 1997): the weather's gust waves through
// the bone's quasi-static gain (background) plus a narrow-band sway at its own frequency
// (resonance). Stateless, so any instance can be solved anywhere and the forest stays coherent.
// Poses go into MultiMesh instance bones, so raster and the path tracer skin the same tree.
class TreeWind3D : public Node3D {
	GDCLASS(TreeWind3D, Node3D);

public:
	struct Bone {
		int parent = 0;
		Vector3 pivot;
		Vector3 centroid; // Relative to the pivot.
		Vector3 dir;
		float area = 0.0f;
		float stiffness = 0.0f;
		float omega = 0.0f; // rad/s at scale 1.
		float zeta = 0.1f;
		float radius = 0.0f; // Wood radius (m).
		float mass = 0.0f; // Carried mass (kg).
		float reach = 0.3f; // Foliage reach around the bone's axis (m), for touch.
		float length = 0.0f;
	};
	struct Rig {
		LocalVector<Bone> bones;
		LocalVector<int> groups; // Per bone: first bone of its stem (shared resonant phase).
		float vogel = -0.7f;
		float extent = 0.0f; // Furthest bone tip from the base (m).
		PackedFloat32Array colliders; // TreeGenerator.get_collision_capsules().
		Ref<Mesh> mesh;
	};
	// Touch on one instance: each bone's extra turn (tree space, axis * angle) and its rate, and
	// the bones that snapped off.
	struct Touch {
		LocalVector<Vector3> angle;
		LocalVector<Vector3> rate;
		LocalVector<Vector3> target;
		LocalVector<uint8_t> broken;
		bool active = false;
	};
	struct Contact {
		Vector3 position;
		Vector3 velocity;
		float radius = 0.4f;
	};
	// One solved pose input: the wind a bone sees, gathered on the main thread's data.
	struct WindSample {
		Vector3 mean;
		Vector2 along;
		float sigma = 0.0f;
	};

private:
	struct Target {
		ObjectID node;
		Ref<MultiMesh> multimesh;
		const Rig *rig = nullptr;
		int bones = 0;
		LocalVector<Transform3D> placements;
		LocalVector<Color> customs;
		Vector<float> buffer;
		float *write = nullptr; // buffer.ptrw() for the frame's workers.
		LocalVector<uint8_t> moving; // Per instance: solved last frame (else at rest).
		HashMap<int, Touch> touch;
		struct Body {
			RID body;
			LocalVector<RID> shapes;
		};
		HashMap<int, Body> bodies; // Static colliders of instances near the camera.
	};

	NodePath target_path = NodePath("..");
	NodePath weather_map_path;
	float wind_range = 200.0f;
	float drag_scale = 1.0f;
	float stiffness_scale = 1.0f;
	float max_bend = 0.5f;
	Vector2 fallback_wind = Vector2(4.0f, 0.0f);
	float fallback_turbulence = 0.2f;
	bool active = true;
	bool collision_enabled = true;
	float collision_range = 60.0f;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 0;
	bool touch_enabled = true;
	StringName touch_group = "tree_touch";
	float touch_radius = 0.4f;
	float touch_stiffness = 4000.0f; // N m per radian a body bends a branch against.
	float break_speed = 3.0f; // m/s; a faster body snaps thin branches it bends far.
	float break_radius = 0.04f;
	int max_pieces = 32;
	HashMap<ObjectID, Vector3> contact_last;
	LocalVector<Contact> contacts;
	LocalVector<ObjectID> pieces;
	HashMap<uint64_t, Ref<ArrayMesh>> piece_meshes;

	double time = 0.0;
	float rescan_timer = 0.0f;
	void _on_target_populated();
	HashMap<uint64_t, Rig> rigs; // By mesh instance id.
	LocalVector<Target> targets;
	LocalVector<Vector4> waves; // Gust waves (k along, k across, amplitude, phase).
	Vector2 base_direction = Vector2(1, 0);
	float base_speed = 0.0f;
	ObjectID weather_id;
	uint64_t solve_usec = 0;
	int solved_instances = 0;

	// Per frame, for the worker threads.
	struct Job {
		Target *target = nullptr;
		int instance = 0;
		Transform3D world; // MultiMesh space to world.
	};
	LocalVector<Job> jobs;
	const WeatherMap3D *frame_weather = nullptr;

	void _scan();
	void _scan_node(Node *p_node);
	const Rig *_rig_for(const Ref<Mesh> &p_mesh);
	void _gather_waves();
	void _solve_job(uint32_t p_index, int p_unused);
	void _process(double p_delta);
	void _gather_contacts(double p_delta);
	void _update_colliders(Target &r_target, const Transform3D &p_world, const Vector3 &p_eye);
	void _free_colliders(Target &r_target);
	bool _touch_instance(Target &r_target, int p_instance, const Transform3D &p_tree, double p_delta);
	void _snap(Target &r_target, int p_instance, const Transform3D &p_tree, int p_bone, const Vector3 &p_velocity);
	Ref<ArrayMesh> _piece_mesh(const Rig &p_rig, int p_bone);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Rig parse_rig(const PackedFloat32Array &p_data, float p_vogel);
	// The wind a point sees: mean, gust frame and sigma (weather map or fallback).
	WindSample sample_wind(const Vector3 &p_position, float p_height) const;
	// The skinning transforms (in the tree's own space) of one instance at the current time.
	void solve_pose(const Rig &p_rig, const Transform3D &p_world, uint32_t p_seed, Transform3D *r_bones, const Touch *p_touch = nullptr) const;

	// A MultiMesh of rigged trees at rest: bones_per_instance records per placement.
	static Ref<MultiMesh> make_multimesh(const Ref<Mesh> &p_mesh, const TypedArray<Transform3D> &p_transforms, const PackedColorArray &p_custom_data = PackedColorArray());
	static void fill_rest(const Ref<MultiMesh> &p_multimesh, const Ref<Mesh> &p_mesh, const LocalVector<Transform3D> &p_transforms, const LocalVector<Color> &p_custom_data);
	static int get_mesh_bone_count(const Ref<Mesh> &p_mesh);

	void set_target(const NodePath &p_path);
	NodePath get_target() const { return target_path; }
	void set_weather_map(const NodePath &p_path);
	NodePath get_weather_map() const { return weather_map_path; }
	void set_wind_range(float p_range) { wind_range = MAX(p_range, 0.0f); }
	float get_wind_range() const { return wind_range; }
	void set_drag_scale(float p_scale) { drag_scale = MAX(p_scale, 0.0f); }
	float get_drag_scale() const { return drag_scale; }
	void set_stiffness_scale(float p_scale) { stiffness_scale = MAX(p_scale, 0.01f); }
	float get_stiffness_scale() const { return stiffness_scale; }
	void set_max_bend(float p_angle) { max_bend = CLAMP(p_angle, 0.01f, 1.5f); }
	float get_max_bend() const { return max_bend; }
	void set_fallback_wind(const Vector2 &p_wind) { fallback_wind = p_wind; }
	Vector2 get_fallback_wind() const { return fallback_wind; }
	void set_fallback_turbulence(float p_intensity) { fallback_turbulence = MAX(p_intensity, 0.0f); }
	float get_fallback_turbulence() const { return fallback_turbulence; }
	void set_active(bool p_active) { active = p_active; }
	bool is_active() const { return active; }
	void set_time(double p_time) { time = p_time; }
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_range(float p_range) { collision_range = MAX(p_range, 0.0f); }
	float get_collision_range() const { return collision_range; }
	void set_collision_layer(uint32_t p_layer) { collision_layer = p_layer; }
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask) { collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_touch_enabled(bool p_enabled) { touch_enabled = p_enabled; }
	bool is_touch_enabled() const { return touch_enabled; }
	void set_touch_group(const StringName &p_group) { touch_group = p_group; }
	StringName get_touch_group() const { return touch_group; }
	void set_touch_radius(float p_radius) { touch_radius = MAX(p_radius, 0.01f); }
	float get_touch_radius() const { return touch_radius; }
	void set_touch_stiffness(float p_stiffness) { touch_stiffness = MAX(p_stiffness, 1.0f); }
	float get_touch_stiffness() const { return touch_stiffness; }
	void set_break_speed(float p_speed) { break_speed = MAX(p_speed, 0.0f); }
	float get_break_speed() const { return break_speed; }
	void set_break_radius(float p_radius) { break_radius = MAX(p_radius, 0.0f); }
	float get_break_radius() const { return break_radius; }
	int get_collider_count() const;
	int get_piece_count() const { return pieces.size(); }
	// For tests: the touch turn (axis * angle) of each bone of one instance of a target.
	PackedVector3Array get_touch_bends(int p_target, int p_instance) const;
	double get_time() const { return time; }

	void rescan();
	void update(double p_delta);
	int get_target_count() const { return targets.size(); }
	int get_solved_instances() const { return solved_instances; }
	float get_solve_msec() const { return solve_usec / 1000.0f; }
	// For tests: each bone's sway frequency (Hz) at scale 1, and the bone rotations (axis * angle,
	// tree space) of a rig at a placement and the current time.
	static PackedFloat32Array get_rig_frequencies(const Ref<Mesh> &p_mesh);
	PackedVector3Array get_bone_bends(const Ref<Mesh> &p_mesh, const Transform3D &p_placement, int p_seed);

	TreeWind3D();
};

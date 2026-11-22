/**************************************************************************/
/*  tree_wind_3d.h                                                        */
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

#include "core/io/image.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class RigidBody3D;
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
		float snow_stiffness = 0.0f; // The wood's own stiffness under a slow load (no floor for the wind), N m/rad.
		float omega = 0.0f; // rad/s at scale 1.
		float zeta = 0.1f;
		float radius = 0.0f; // Wood radius (m).
		float mass = 0.0f; // Carried mass (kg).
		float reach = 0.3f; // Foliage reach around the bone's axis (m), for touch.
		float length = 0.0f;
		float strength = 0.0f; // Bending moment that breaks it (N m at scale 1): MOR pi r^3 / 4, less knots.
	};
	struct Rig {
		LocalVector<Bone> bones;
		LocalVector<int> groups; // Per bone: first bone of its stem (shared resonant phase).
		float vogel = -0.7f;
		float extent = 0.0f; // Furthest bone tip from the base (m).
		float leaf_area = 0.0f; // Carried area of the stems at the root: shares the crown's snow by bone.
		float crown_radius = 0.5f; // Horizontal reach of the crown (m, tree space).
		float snow_catch = 1.0f; // Share of the canopy's snow this crown holds: bare broadleaves catch on twigs only.
		Vector2 glaze_surface; // (sum of shoot lengths m, sum of length x radius m2) at scale 1 (mesh meta tree_glaze_surface).
		Ref<Mesh> snow_caps[3]; // Light, full and heavy cap pads (mesh meta tree_snow_caps), skinned to this rig.
		PackedFloat32Array cap_sites; // bone, x, y, z per cap (mesh meta tree_snow_cap_sites).
		PackedFloat32Array colliders; // TreeGenerator.get_collision_capsules().
		Ref<Mesh> mesh;
		// Storms (docs/ideas/trees.md section 5t), from TreeGenerator's storm kit.
		bool storm = false;
		float anchorage = 0.0f; // Root anchorage (N m) at scale 1: C_reg times the stem's mass.
		float plate_radius = 1.0f;
		float plate_thick = 0.3f;
		PackedFloat32Array storm_data; // TreeGenerator's "tree_storm" meta.
		Array cap_kit;
		Array static_materials;
	};
	// The worst storm load on one instance this frame: the bone nearest to breaking and the roots.
	struct StormLoad {
		int bone = -1;
		float ratio = 0.0f; // Bending moment over the bone's strength.
		float root_ratio = 0.0f; // Overturning moment over the anchorage.
		Vector3 wind; // At the failing part, world space.
		// A fire's char at the stem's foot (m, bark and wood; WorldFire.stem_char_at), in. Out: the charred foot gives
		// way before the roots (root_ratio is then the foot's), so it snaps there and falls without its root plate.
		float base_char = 0.0f;
		bool base_snap = false;
		// A tree the fire killed (a snag), in: its age dead (years, -1 alive). Its roots and wood rot over the years,
		// and it fails at the rotten butt (no root plate).
		float snag_age = -1.0f;
		bool snow = false; // Out: the failing part broke under its snow more than the wind.
	};
	// Touch on one instance: each bone's extra turn (tree space, axis * angle) and its rate, and
	// the bones that snapped off.
	struct Touch {
		LocalVector<Vector3> angle;
		LocalVector<Vector3> rate;
		LocalVector<Vector3> target;
		LocalVector<uint8_t> broken;
		LocalVector<uint8_t> flat; // Stem segments snapped by a storm: drawn flattened onto their joint.
		LocalVector<ObjectID> parts; // Bodies broken off this instance.
		bool uprooted = false; // Thrown by a storm: the whole instance is a body now.
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
		LocalVector<uint8_t> slot; // Per instance: its frame within an update period (staggers the load).
		LocalVector<float> base_char; // Per instance: char at the stem's foot from a fire (m), refreshed now and then.
		LocalVector<float> snag_age; // Per instance: years since the fire killed it, -1 alive.
		LocalVector<uint8_t> avalanche_hit; // Per instance: 1 an avalanche took its crown, 2 the whole tree.
		LocalVector<float> snow_kg; // Per instance: snow held in the crown (kg, WorldSnow.canopy_load_at), as the wood feels it.
		LocalVector<float> snow_creep; // Per instance: the crept part of that (kg; the wood's slow bend under a held load).
		LocalVector<uint8_t> cap_level; // Per instance: snow caps drawn, 0 none, 1-3 light to heavy.
		LocalVector<float> snow_seen; // Per instance: the load (kg, no creep) clumps were last dropped against.
		LocalVector<float> glaze_kg; // Per instance: clear ice on the crown (kg, WorldWeather.glaze_at), as the wood feels it.
		LocalVector<float> glaze_shed; // Per instance: the share of the glaze a break shook off (until the glaze melts).
		bool caps_dirty = false; // cap_level changed since the caps' last upload.
		ObjectID cap_nodes[3]; // MultiMeshInstance3D children drawing the caps of each level.
		Vector<float> cap_buffer;
		HashMap<int, Touch> touch;
		struct Body {
			RID body;
			LocalVector<RID> shapes;
		};
		HashMap<int, Body> bodies; // Static colliders of instances near the camera.
	};

	NodePath target_path = NodePath("..");
	NodePath weather_map_path;
	float wind_range = 1000.0f;
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
	bool storm_enabled = true;
	float storm_strength = 1.0f;
	float storm_anchorage = 1.0f;
	float storm_min_radius = 0.03f;
	float storm_min_wind = 12.0f;
	int storm_breaks = 0;
	int storm_throws = 0;
	int storm_snaps = 0; // Of the throws: fire-charred stems that snapped at the foot.
	int storm_snags = 0; // Trees a fire killed, standing dead.
	float char_timer = 0.0f;
	// Avalanches (WorldSnow.avalanche_peak_pressure_at, docs/ideas/trees.md 5ak).
	bool avalanche_enabled = true;
	// Windthrow patches to the stand-age map (WorldStand.add_disturbance, trees.md 5aq): a 30 m cell where a storm
	// threw most stems is a stand-replacing disturbance; single throws are gap dynamics and are not reported.
	bool report_windthrow = true;
	static constexpr float WINDTHROW_CELL = 30.0f; // m
	static constexpr float WINDTHROW_SHARE = 0.6f; // of a cell's stems thrown
	struct ThrowCell {
		int thrown = 0;
		Vector2 sum; // of the thrown stems' xz
		int disturbance = 0; // WorldStand event id, 0 none yet
	};
	HashMap<Vector2i, ThrowCell> throw_cells;
	bool throw_cells_dirty = false;
	int windthrow_patches = 0;
	void _report_windthrow();
	// Snow-laden crowns (docs/ideas/trees.md section 5al): the crown's load from WorldSnow bends the branches and
	// can break them; a tree that breaks or falls shakes its snow onto the pack (WorldSnow.shake_canopy).
	bool snow_load_enabled = true;
	float snow_creep_hours = 3.0f;
	bool snow_frame = false; // Loaded trees get the storm check this frame (once a second).
	int snow_breaks = 0;
	float snow_shaken_kg = 0.0f;
	void _snow_check(float p_dt);
	// Ice storms (trees.md 5be): the glaze WorldWeather.glaze_at reports on the crown's wood, loading it as the snow does.
	bool glaze_load_enabled = true;
	int glaze_breaks = 0;
	void _glaze_check();
	float snow_cap_range = 150.0f; // m: past it the crown whitening alone shows the snow.
	Vector3 last_eye;
	int caps_drawn = 0;
	void _update_caps(Target &r_target, bool p_moved);
	// Falling clumps: snow leaving a crown (unloading, a gust, a break) drops from its caps.
	struct Clump {
		Vector3 position;
		Vector3 velocity;
		Basis rotation;
		Vector3 spin;
		Vector3 size;
		float ground = 0.0f;
	};
	LocalVector<Clump> clumps;
	bool snow_clumps_enabled = true;
	float clump_range = 80.0f;
	int clumps_dropped = 0;
	int clump_capacity = 0;
	ObjectID clump_node;
	Ref<Mesh> clump_mesh;
	void _drop_clumps(Target &r_target, int p_instance, const Transform3D &p_world, float p_kg);
	void _step_clumps(double p_delta);
	void _free_caps(Target &r_target);
	int avalanche_breaks = 0; // Crowns snapped.
	int avalanche_throws = 0; // Whole trees broken at the foot or thrown.
	struct AvalancheFail {
		int target = -1;
		int instance = -1;
		StormLoad load;
	};
	LocalVector<AvalancheFail> avalanche_pending;
	// Canopy a felled tree or crown took out of the weather map's canopy_map, applied once a second.
	struct CanopyHole {
		Vector2 xz;
		float radius = 0.0f;
		float keep = 0.0f; // Leaf area left inside the crown's core.
	};
	LocalVector<CanopyHole> canopy_holes;
	Ref<Image> canopy_edit;
	ObjectID canopy_texture;
	int canopy_holes_cut = 0;
	void _avalanche_check();
	void _avalanche_fail(LocalVector<Target *> &r_changed);
	void _cut_canopy();
	static float _crown_radius(const Rig &p_rig);
	bool making_snap = false; // _make_part: a charred stem snapped at its foot (no root plate)
	float snap_cut = 0.0f; // _make_part: its cut over the stump (tree space)
	// A part a storm broke off or threw, while the wind still pushes it.
	struct StormPart {
		ObjectID body;
		float area = 0.0f; // Frontal area at scale (m2), with its Vogel exponent.
		float vogel = -0.7f;
		Vector3 load; // Load point in the body's own space.
		float age = 0.0f;
		RID joint; // The hinge it turns on (torn fibres or the root plate's edge), until it lets go.
		float release = 0.0f; // Angle (rad) past which the hinge lets go; 0 never.
		Basis start;
		// For its Blast swap at the first hard impact.
		Ref<Mesh> mesh;
		int bone = 0;
		float scale = 1.0f;
		uint64_t key = 0;
		LocalVector<uint8_t> exclude; // Bones broken off before (not part of it).
		Transform3D butt; // Its fracture cap, tree space (none for a thrown tree).
		Vector3 fall; // Downwind, tree space: the way it fell.
		float snap_cut = 0.0f; // Burnt through at its foot, cut this high (tree space) over its stump: no root plate.
		bool swapped = false;
		// A hard hit that came before its Blast asset was ready: swapped when it is.
		bool hit = false;
		Vector3 hit_local; // Contact, in the body's space.
		float hit_impulse = 0.0f;
	};
	LocalVector<StormPart> storm_parts;
	// A fracture cap on a stub left standing, following the bone below the break.
	struct Cap {
		ObjectID node;
		ObjectID target;
		int instance = 0;
		int bone = 0;
		Transform3D local; // In the bone's tree space.
	};
	LocalVector<Cap> caps;
	HashMap<uint64_t, Ref<ArrayMesh>> kit_meshes;
	HashMap<uint64_t, Ref<Resource>> blast_assets;
	HashMap<uint64_t, Array> blast_materials;
	// A Blast asset being built on a worker thread, from a copy of the rig and the mesh's arrays.
	struct BlastJob {
		Rig rig;
		Array surface_arrays;
		int bone = 0;
		LocalVector<uint8_t> exclude;
		Transform3D butt;
		Vector3 fall;
		float snap_cut = 0.0f;
		Ref<Resource> asset;
		Array materials;
		uint64_t task = 0;
	};
	HashMap<uint64_t, BlastJob *> blast_jobs;
	// A part that fell as a Blast destructible, while it settles (its chunks by authored index).
	struct StormFall {
		ObjectID node;
		float scale = 1.0f;
		float age = 0.0f;
		float next = 0.0f;
		int butt = -1; // The chunk at its foot or break.
		LocalVector<float> mass; // kg, at scale.
		LocalVector<Vector3> center; // Tree space.
		LocalVector<Vector3> low_a, low_b; // Its first bone's ends, tree space.
		LocalVector<int> parent; // The chunk each grows from (-1 for the butt's).
		LocalVector<float> strength; // Of its bond to that chunk (kN m at scale 1).
		HashMap<int, Transform3D> last; // Pieces' poses at the last check, by their first chunk.
	};
	LocalVector<StormFall> storm_settling;
	HashMap<uint64_t, Array> mesh_arrays; // Surface arrays by mesh instance id, read on the main thread.
	int storm_falls = 0;
	bool lod_enabled = true;
	float lod_pixel_error = 0.5f; // Largest pose error the LOD may leave, in pixels.
	float budget_msec = 0.0f; // 0: none; else the LOD coarsens while the solve runs over it.
	float lod_bias = 1.0f;
	uint64_t frame_index = 0;
	int skipped_instances = 0;
	HashMap<ObjectID, Vector3> contact_last;
	LocalVector<Contact> contacts;
	LocalVector<ObjectID> pieces;
	HashMap<uint64_t, Ref<ArrayMesh>> piece_meshes;

	double time = 0.0;
	float rescan_timer = 0.0f;
	void _on_target_populated();
	void _on_target_child(Node *p_child);
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
		float min_length = 0.0f; // Bones shorter than this (tree space) ride their parent.
		bool storm = false; // Test the storm load this frame.
		float snow_kg = 0.0f; // Snow on the crown (kg).
		StormLoad load;
	};
	LocalVector<Job> jobs;
	const WeatherMap3D *frame_weather = nullptr;

	void _scan();
	void _scan_node(Node *p_node);
	const Rig *_rig_for(const Ref<Mesh> &p_mesh);
	void _gather_waves();
	void _solve_job(uint32_t p_index, int p_unused);
	void _process(double p_delta);
	void _update_snag(Target &r_target, uint32_t p_instance, const Transform3D &p_tree, const Vector3 &p_at, const class WorldFire *p_fire);
	void _gather_contacts(double p_delta);
	void _update_colliders(Target &r_target, const Transform3D &p_world, const Vector3 &p_eye);
	void _free_colliders(Target &r_target);
	bool _touch_instance(Target &r_target, int p_instance, const Transform3D &p_tree, double p_delta);
	void _snap(Target &r_target, int p_instance, const Transform3D &p_tree, int p_bone, const Vector3 &p_velocity);
	// p_cut_side: 0 whole; 1 the stem (bones 0 and 1) cut away below height p_cut (tree space); -1 only the stem's
	// bark below it (a stump).
	Ref<ArrayMesh> _piece_mesh(const Rig &p_rig, int p_bone, const LocalVector<uint8_t> *p_exclude = nullptr, uint64_t p_key = 0, float p_cut = 0.0f, int p_cut_side = 0);
	Ref<ArrayMesh> _kit_mesh(const Rig &p_rig, bool p_plate);
	RigidBody3D *_make_part(Target &r_target, int p_instance, const Transform3D &p_tree, int p_bone, const Transform3D &p_at, const LocalVector<uint8_t> &p_exclude, uint64_t p_key);
	static Ref<Resource> _blast_asset(const Rig &p_rig, const Array &p_surface_arrays, int p_bone, const LocalVector<uint8_t> &p_exclude, const Transform3D &p_butt, const Vector3 &p_fall, float p_snap_cut, Array &r_materials);
	static void _blast_chunks(const Rig &p_rig, int p_bone, const LocalVector<uint8_t> &p_exclude, LocalVector<int> &r_chunk_of, LocalVector<int> &r_chunk_bone);
	void _storm_settle(double p_delta);
	void _blast_request(const StormPart &p_part);
	void _blast_job(BlastJob *p_job);
	bool _blast_ready(uint64_t p_key);
	void _blast_wait_all();
	void _blast_swap(StormPart &r_part, RigidBody3D *p_body, const Vector3 &p_contact, float p_impulse);
	void _storm_impacts();
	void _storm_fail(Target &r_target, int p_instance, const Transform3D &p_world, const StormLoad &p_load);
	void _storm_parts(double p_delta);
	void _collapse(Target &r_target, int p_instance, const Transform3D &p_world);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Rig parse_rig(const PackedFloat32Array &p_data, float p_vogel);
	// The wind a point sees: mean, gust frame and sigma (weather map or fallback).
	WindSample sample_wind(const Vector3 &p_position, float p_height) const;
	// The skinning transforms (in the tree's own space) of one instance at the current time.
	void solve_pose(const Rig &p_rig, const Transform3D &p_world, uint32_t p_seed, Transform3D *r_bones, const Touch *p_touch = nullptr, float p_min_length = 0.0f, StormLoad *r_storm = nullptr, float p_snow_kg = 0.0f) const;

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
	void set_storm_enabled(bool p_enabled) { storm_enabled = p_enabled; }
	void set_avalanche_enabled(bool p_enabled) { avalanche_enabled = p_enabled; }
	void set_report_windthrow(bool p_enabled) { report_windthrow = p_enabled; }
	bool is_report_windthrow() const { return report_windthrow; }
	int get_windthrow_patch_count() const { return windthrow_patches; }
	bool is_avalanche_enabled() const { return avalanche_enabled; }
	int get_avalanche_break_count() const { return avalanche_breaks; }
	void set_snow_load_enabled(bool p_enabled) { snow_load_enabled = p_enabled; }
	bool is_snow_load_enabled() const { return snow_load_enabled; }
	void set_snow_creep_hours(float p_hours) { snow_creep_hours = MAX(p_hours, 0.0f); }
	float get_snow_creep_hours() const { return snow_creep_hours; }
	int get_snow_break_count() const { return snow_breaks; }
	void set_glaze_load_enabled(bool p_enabled) { glaze_load_enabled = p_enabled; }
	bool is_glaze_load_enabled() const { return glaze_load_enabled; }
	int get_glaze_break_count() const { return glaze_breaks; }
	float get_instance_glaze_kg(Node *p_target, int p_instance) const;
	// Clear ice (kg) a tree's wood carries under a radial glaze of p_glaze_mm, for its glaze surface at scale p_scale.
	static float glaze_kg(const Vector2 &p_surface, float p_glaze_mm, float p_scale);
	float get_snow_shaken_kg() const { return snow_shaken_kg; }
	float get_instance_snow_kg(Node *p_target, int p_instance) const;
	int get_instance_cap_level(Node *p_target, int p_instance) const;
	void set_snow_cap_range(float p_range) { snow_cap_range = MAX(p_range, 0.0f); }
	float get_snow_cap_range() const { return snow_cap_range; }
	int get_snow_caps_drawn() const { return caps_drawn; }
	void set_snow_clumps_enabled(bool p_enabled) { snow_clumps_enabled = p_enabled; }
	bool is_snow_clumps_enabled() const { return snow_clumps_enabled; }
	int get_snow_clumps_dropped() const { return clumps_dropped; }
	int get_snow_clumps_falling() const { return clumps.size(); }
	int get_avalanche_throw_count() const { return avalanche_throws; }
	int get_canopy_holes_cut() const { return canopy_holes_cut; }
	static float avalanche_strength(float p_stem_diameter, bool p_crown);
	bool is_storm_enabled() const { return storm_enabled; }
	void set_storm_strength(float p_scale) { storm_strength = MAX(p_scale, 0.01f); }
	float get_storm_strength() const { return storm_strength; }
	void set_storm_anchorage(float p_scale) { storm_anchorage = MAX(p_scale, 0.01f); }
	float get_storm_anchorage() const { return storm_anchorage; }
	void set_storm_min_radius(float p_radius) { storm_min_radius = MAX(p_radius, 0.0f); }
	float get_storm_min_radius() const { return storm_min_radius; }
	void set_storm_min_wind(float p_speed) { storm_min_wind = MAX(p_speed, 0.0f); }
	float get_storm_min_wind() const { return storm_min_wind; }
	int get_storm_break_count() const { return storm_breaks; }
	int get_storm_throw_count() const { return storm_throws; }
	int get_storm_fall_count() const { return storm_falls; }
	int get_storm_snap_count() const { return storm_snaps; }
	int get_snag_count() const { return storm_snags; }
	// For tests: the storm load on a rigged mesh at a placement and the current time.
	Dictionary get_storm_load(const Ref<Mesh> &p_mesh, const Transform3D &p_placement, int p_seed);
	int get_collider_count() const;
	int get_piece_count() const { return pieces.size(); }
	// For tests: the touch turn (axis * angle) of each bone of one instance of a target.
	PackedVector3Array get_touch_bends(int p_target, int p_instance) const;
	double get_time() const { return time; }

	void rescan();
	void update(double p_delta);
	int get_target_count() const { return targets.size(); }
	int get_solved_instances() const { return solved_instances; }
	int get_skipped_instances() const { return skipped_instances; }
	void set_lod_enabled(bool p_enabled) { lod_enabled = p_enabled; }
	bool is_lod_enabled() const { return lod_enabled; }
	void set_lod_pixel_error(float p_px) { lod_pixel_error = MAX(p_px, 0.01f); }
	float get_lod_pixel_error() const { return lod_pixel_error; }
	void set_budget_msec(float p_msec) { budget_msec = MAX(p_msec, 0.0f); }
	float get_budget_msec() const { return budget_msec; }
	float get_lod_bias() const { return lod_bias; }
	float get_solve_msec() const { return solve_usec / 1000.0f; }
	// For tests: each bone's sway frequency (Hz) at scale 1, and the bone rotations (axis * angle,
	// tree space) of a rig at a placement and the current time.
	static PackedFloat32Array get_rig_frequencies(const Ref<Mesh> &p_mesh);
	PackedVector3Array get_bone_bends(const Ref<Mesh> &p_mesh, const Transform3D &p_placement, int p_seed);

	TreeWind3D();
	~TreeWind3D();
};

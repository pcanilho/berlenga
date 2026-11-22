/**************************************************************************/
/*  seed_down_3d.h                                                        */
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

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class WorldWeather;

// Seed down in the wind (docs/ideas/vegetation.md, "Seed down in the wind"): plumed seeds (dandelion clocks,
// thistledown, willowherb fluff) loosed from the heads in seed near the camera when dry wind tugs them, carried by
// the shared wind (WorldContracts.weather()) with a falling speed and a random vertical gust, caught in the grass or
// floating on water where they land, lifted again by a stronger gust. Each one a small mesh (seed, beak, pappus),
// one MultiMesh per kind, so the path tracer sees them.
class SeedDown3D : public Node3D {
	GDCLASS(SeedDown3D, Node3D);

public:
	enum Kind {
		KIND_DANDELION, // Taraxacum: a 14 mm parachute of about 100 bristles on a long beak (Cummins et al. 2018).
		KIND_THISTLE, // Cirsium, Carduus: a larger, feathery pappus that often drops its seed.
		KIND_WILLOWHERB, // Chamerion, Epilobium: a tuft of silky hairs on a tiny seed.
		KIND_MAX,
	};

private:
	struct Seed {
		Vector3 p;
		Vector3 v;
		float w_turb = 0.0f; // m/s, the vertical gust the seed rides
		float lying = -1.0f; // s on the ground or water; < 0 aloft
		float spin = 0.0f;
		float scale = 1.0f;
		uint8_t kind = 0;
		bool on_water = false;
		bool alive = false;
	};
	struct Source {
		Vector3 p; // the head, world
		uint8_t kind = 0;
		int preset = -1;
	};

	bool enabled = true;
	bool still_shot = false; // Stills: hold the seeds where they are (drawn, not stepped).
	int max_seeds = 1500;
	float range = 40.0f; // m round the camera that heads loose seeds and seeds are kept
	float release_rate = 0.15f; // seeds a second from one head in seed, in a strong dry wind
	float lie_time = 90.0f; // s a seed lies before it is recycled
	float lift_speed = 3.0f; // m/s at 10 cm that lifts a lying seed again
	LocalVector<Seed> seeds;
	int next_slot = 0;
	LocalVector<Source> sources;
	HashMap<int, float> seed_share; // preset -> share of its heads in seed today
	int share_day = -1;
	float source_timer = 0.0f;
	int released = 0;
	RandomPCG rng;
	Ref<Mesh> meshes[KIND_MAX];
	Ref<MultiMesh> multimeshes[KIND_MAX];
	MultiMeshInstance3D *draws[KIND_MAX] = {};

	static float _fall_speed(int p_kind); // m/s in still air, dry pappus open
	static int _kind_of(int p_preset); // -1: not a plumed seed
	Vector3 _camera_position() const;
	float _ground_at(const Vector3 &p_p) const;
	void _refresh_sources(const Vector3 &p_camera);
	void _release(const Vector3 &p_camera, float p_delta, const WorldWeather *p_weather);
	void _step_seed(Seed &p_seed, float p_delta, const WorldWeather *p_weather);
	void _ensure_draws();
	void _upload();
	Ref<Mesh> _make_mesh(int p_kind) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_still_shot(bool p_v) { still_shot = p_v; }
	bool is_still_shot() const { return still_shot; }
	void set_max_seeds(int p_v);
	int get_max_seeds() const { return max_seeds; }
	void set_range(float p_v) { range = MAX(p_v, 1.0f); }
	float get_range() const { return range; }
	void set_release_rate(float p_v) { release_rate = MAX(p_v, 0.0f); }
	float get_release_rate() const { return release_rate; }
	void set_lie_time(float p_v) { lie_time = MAX(p_v, 1.0f); }
	float get_lie_time() const { return lie_time; }
	void set_lift_speed(float p_v) { lift_speed = MAX(p_v, 0.1f); }
	float get_lift_speed() const { return lift_speed; }

	// Loose p_count seeds of p_kind at p_position now (tests, scripted gusts).
	void release_at(const Vector3 &p_position, int p_kind, int p_count);
	// Advance p_seconds in steps of 1/60 s without the scene (tests); with p_release, heads loose seeds too.
	void advance(float p_seconds, bool p_release = false);
	void refresh_sources();
	int get_source_count() const { return sources.size(); }
	int get_released_count() const { return released; }
	int get_aloft_count() const;
	int get_lying_count() const;
	int get_floating_count() const;
	// x, y, z position, w: seconds lying (< 0 aloft), one per live seed.
	PackedVector4Array get_seed_states() const;
	void clear_seeds();
	static float get_fall_speed(int p_kind) { return _fall_speed(p_kind); }
	static int get_kind_for_preset(int p_preset) { return _kind_of(p_preset); }

	SeedDown3D();
};

VARIANT_ENUM_CAST(SeedDown3D::Kind);

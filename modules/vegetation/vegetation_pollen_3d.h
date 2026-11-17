/**************************************************************************/
/*  vegetation_pollen_3d.h                                                */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// Pollen puffs (docs/ideas/vegetation.md, "Ground mist and pollen with PhysX Flow"): bodies brushing
// wind-pollinated plants in flower shake pollen out of the anthers, and Flow carries the puff. Uses
// PhysXFlow3D by class name: without Flow in the build it stays inert.
class VegetationPollen3D : public Node3D {
	GDCLASS(VegetationPollen3D, Node3D);

	bool enabled = true;
	NodePath vegetation_field;
	NodePath weather_map;
	StringName trample_group = "grass_trample";
	Dictionary season;
	float min_speed = 0.4f;
	float strength = 1.0f;
	float recharge_time = 20.0f;
	int max_puffs = 12;
	Color pollen_color = Color(0.93f, 0.84f, 0.5f);

	struct Plant {
		Vector3 position;
		float top = 0.3f; // Flower height over the base.
		float reach = 0.2f;
		float pollen = 0.0f; // Amount at full flower.
		int preset = -1;
		float last = -1e9f; // When it last shed, seconds.
	};
	LocalVector<Plant> plants;
	HashMap<Vector2i, LocalVector<uint32_t>> grid;
	HashMap<int, float> release; // Per preset, for the season.
	int indexed_count = -1;
	uint64_t indexed_field = 0;
	float season_timer = 0.0f;

	struct Mover {
		ObjectID id;
		Vector3 last;
		bool seen = false;
	};
	LocalVector<Mover> movers;

	struct Puff {
		Node3D *emitter = nullptr;
		float left = 0.0f;
	};
	LocalVector<Puff> puffs;
	uint32_t next_puff = 0;
	Node3D *flow = nullptr;
	float time = 0.0f;
	int fired = 0;

	Node *_field() const;
	void _index();
	void _refresh_release();
	void _fire(const Plant &p_plant, const Vector3 &p_velocity, const Vector3 &p_wind, float p_amount);
	void _build();
	void _resize_pool();
	void _step(float p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_vegetation_field(const NodePath &p_v);
	NodePath get_vegetation_field() const { return vegetation_field; }
	void set_weather_map(const NodePath &p_v) { weather_map = p_v; }
	NodePath get_weather_map() const { return weather_map; }
	void set_trample_group(const StringName &p_v) { trample_group = p_v; }
	StringName get_trample_group() const { return trample_group; }
	void set_season(const Dictionary &p_v);
	Dictionary get_season() const { return season; }
	void set_min_speed(float p_v) { min_speed = MAX(p_v, 0.0f); }
	float get_min_speed() const { return min_speed; }
	void set_strength(float p_v) { strength = MAX(p_v, 0.0f); }
	float get_strength() const { return strength; }
	void set_recharge_time(float p_v) { recharge_time = MAX(p_v, 0.1f); }
	float get_recharge_time() const { return recharge_time; }
	void set_max_puffs(int p_v);
	int get_max_puffs() const { return max_puffs; }
	void set_pollen_color(const Color &p_c);
	Color get_pollen_color() const { return pollen_color; }

	Node3D *get_flow() const { return flow; }
	bool is_flow_available() const;
	int get_pollen_plant_count() const { return plants.size(); }
	int get_puffs_fired() const { return fired; }
	int get_active_puffs() const;
	void reindex();
};

/**************************************************************************/
/*  vegetation_mist_3d.h                                                  */
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

// Ground mist (docs/ideas/vegetation.md, "Ground mist and pollen with PhysX Flow"): radiation fog in
// the near field, carried by NVIDIA Flow so it parts round bodies, drifts with the ground wind and
// drains into hollows. Uses PhysXFlow3D by class name: without Flow in the build it stays inert.
class VegetationMist3D : public Node3D {
	GDCLASS(VegetationMist3D, Node3D);

	bool enabled = true;
	NodePath follow_target;
	NodePath terrain;
	NodePath weather_map;
	NodePath sun_light;
	float mist_amount = -1.0f; // Below 0: from the weather.
	float mist_range = 24.0f;
	float mist_cell = 6.0f;
	float mist_depth = 1.2f;
	float extinction = 0.06f;
	float heaviness = 0.15f;
	float flow_cell_size = 0.25f;
	Color mist_color = Color(0.92f, 0.93f, 0.95f);

	Node3D *flow = nullptr;
	struct Cell {
		Vector2i key;
		Node3D *emitter = nullptr;
	};
	LocalVector<Cell> cells;
	HashMap<Vector2i, Vector2> ground_cache; // x: height, y: propensity.
	float level = 0.0f;
	float level_timer = 0.0f;
	int active_cells = 0;

	Node3D *_target() const;
	float _ground_height(const Vector3 &p_at) const;
	Vector2 _ground_cell(const Vector2i &p_key);
	void _build();
	void _apply_flow_settings();
	void _resize_pool();
	void _step(float p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_enabled(bool p_v);
	bool is_enabled() const { return enabled; }
	void set_follow_target(const NodePath &p_v) { follow_target = p_v; }
	NodePath get_follow_target() const { return follow_target; }
	void set_terrain(const NodePath &p_v);
	NodePath get_terrain() const { return terrain; }
	void set_weather_map(const NodePath &p_v) { weather_map = p_v; }
	NodePath get_weather_map() const { return weather_map; }
	void set_sun_light(const NodePath &p_v) { sun_light = p_v; }
	NodePath get_sun_light() const { return sun_light; }
	void set_mist_amount(float p_v) { mist_amount = p_v; }
	float get_mist_amount() const { return mist_amount; }
	void set_mist_range(float p_v);
	float get_mist_range() const { return mist_range; }
	void set_mist_cell(float p_v);
	float get_mist_cell() const { return mist_cell; }
	void set_mist_depth(float p_v) { mist_depth = MAX(p_v, 0.1f); }
	float get_mist_depth() const { return mist_depth; }
	void set_extinction(float p_v);
	float get_extinction() const { return extinction; }
	void set_heaviness(float p_v);
	float get_heaviness() const { return heaviness; }
	void set_flow_cell_size(float p_v);
	float get_flow_cell_size() const { return flow_cell_size; }
	void set_mist_color(const Color &p_c);
	Color get_mist_color() const { return mist_color; }

	Node3D *get_flow() const { return flow; }
	bool is_flow_available() const;
	float get_mist_level() const { return level; }
	float weather_mist_level() const;
	float get_propensity_at(const Vector3 &p_position);
	int get_active_cells() const { return active_cells; }
};

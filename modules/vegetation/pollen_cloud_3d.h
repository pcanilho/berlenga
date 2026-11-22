/**************************************************************************/
/*  pollen_cloud_3d.h                                                     */
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

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// Pollen clouds (docs/ideas/vegetation.md, round 7): in early summer, dry wind shakes clouds of pollen from conifer
// crowns and flowering grass. Wind-dispersed pollen settles slowly (about 2.5 cm/s for pine, 3.2 for spruce, in still
// air), so the turbulence carries it and it shows as a drifting yellow haze. This picks the sources near the camera
// each quarter second (conifer crowns from the scene's VegetationField3Ds, sward from its GrassField3Ds), works out
// how much each sheds from the weather (WorldWeather: air humidity, wetness, rain, wind at the crown or the sward;
// the conifers' flowering window, the grasses' anthesis from the season), and draws the pollen with NVIDIA Flow as a faint, slightly heavy gas
// through a pool of sphere emitters in a PhysXFlow3D of its own that drifts with the weather's wind. It talks to the
// PhysX module by class and property names only: without Flow the release is still worked out, nothing is drawn.
// VegetationPollen3D draws the close puffs a body brushing flowering plants shakes out (4 cm cells); this is the
// landscape scale (0.5 m cells), shed by the wind alone.
class PollenCloud3D : public Node3D {
	GDCLASS(PollenCloud3D, Node3D);

public:
	struct Source {
		Vector3 position;
		Vector3 velocity;
		float radius = 2.0f;
		float rate = 0.0f; // 0..1 of a full release
		bool conifer = false;
	};

private:
	bool enabled = true;
	float range = 60.0f; // m round the camera
	int max_conifer_sources = 10;
	int max_grass_sources = 6;
	Vector2 conifer_window = Vector2(130, 170); // day of year: flowering (northern temperate pines and spruces; estimate)
	Vector2 grass_window = Vector2(145, 205); // day of year: grass anthesis when the weather gives no season (estimate)
	float smoke_per_source = 1.2f; // Flow emitter smoke at a full release
	Color color = Color(0.95, 0.84, 0.38); // sulphur yellow (pine pollen)
	float cell_size = 0.5f;
	int max_blocks = 2048;
	NodePath terrain_path;

	Node3D *flow = nullptr;
	LocalVector<Node3D *> emitters;
	bool tried = false;
	LocalVector<Source> sources;
	float release = 0.0f; // the weather's share of a full release at the camera, last update
	double update_left = 0.0;
	int conifer_count = 0;
	int grass_count = 0;

	void _build();
	void _feed();
	Vector3 _camera_position() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The weather's share of a full release (0..1): dry air (relative humidity 55% and under, none over 85%), dry
	// leaves, no rain, and a wind past 3 m/s rising to full at 8 m/s.
	static float release_factor(float p_humidity, float p_wetness, float p_rain_mmh, float p_wind);
	// The share in flower on a day for a window of days (ramps of 7 days either side).
	static float window_share(int p_day, const Vector2 &p_window);
	// Picks the sources and feeds the emitters now (process does it every 0.25 s).
	void update_sources();

	bool has_flow() const { return flow != nullptr; }
	Node3D *get_flow() const { return flow; }
	float get_release() const { return release; }
	int get_conifer_sources() const { return conifer_count; }
	int get_grass_sources() const { return grass_count; }
	Array get_sources() const;

	void set_enabled(bool p_enable);
	bool is_enabled() const { return enabled; }
	void set_range(float p_range) { range = MAX(p_range, 1.0f); }
	float get_range() const { return range; }
	void set_max_conifer_sources(int p_count) { max_conifer_sources = CLAMP(p_count, 0, 24); }
	int get_max_conifer_sources() const { return max_conifer_sources; }
	void set_max_grass_sources(int p_count) { max_grass_sources = CLAMP(p_count, 0, 24); }
	int get_max_grass_sources() const { return max_grass_sources; }
	void set_conifer_window(const Vector2 &p_days) { conifer_window = p_days; }
	Vector2 get_conifer_window() const { return conifer_window; }
	void set_grass_window(const Vector2 &p_days) { grass_window = p_days; }
	Vector2 get_grass_window() const { return grass_window; }
	void set_smoke_per_source(float p_value) { smoke_per_source = MAX(p_value, 0.0f); }
	float get_smoke_per_source() const { return smoke_per_source; }
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
	void set_terrain_path(const NodePath &p_path);
	NodePath get_terrain_path() const { return terrain_path; }
};

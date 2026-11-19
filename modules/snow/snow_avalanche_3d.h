/**************************************************************************/
/*  snow_avalanche_3d.h                                                   */
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

#include "scene/3d/node_3d.h"

class SnowCover3D;

// Avalanches (docs/ideas/snow.md, follow-up 20). Slab release where the stability index (shear strength of the weak
// layer under the slab against the slab's shear stress, Foehn 1987; Jamieson and Johnston 2001 for strength by
// density) drops below 1 on 28-55 degree slopes; the slab is the new snow since it last settled, or the snow over a
// buried crust. The released snow runs as a depth-averaged Voellmy flow (Voellmy 1955; Salm 1993; Christen et al.
// 2010, RAMMS) on the cover's own grid: gravity down the snow surface, dry friction mu g cos(theta) and turbulent
// friction g u^2 / (xi h), donor-cell transport of mass and momentum, entrainment of the new snow it runs over at a
// rate with its speed, deposition where it can no longer overcome its own friction. The moving snow lives in the
// cover's cells while it runs, so the snow map, prints, plants and every reader of WorldSnow see it move; bodies in
// its path take its dynamic pressure, 1/2 rho u^2. SnowCover3D adds one by itself while avalanches_enabled.
class SnowAvalanche3D : public Node3D {
	GDCLASS(SnowAvalanche3D, Node3D);

	// Settings.
	float mu = 0.2f; // Voellmy dry friction (RAMMS: 0.155-0.4)
	float xi = 1500.0f; // m/s2, Voellmy turbulent friction (RAMMS: 1000-3000)
	float min_slope = 28.0f; // degrees
	float max_slope = 55.0f;
	float min_slab = 0.2f; // m
	float settle_hours = 72.0f; // e-folding of the new snow layer's weakness
	float entrainment = 0.005f; // m of snow taken per m/s per s
	float flow_density = 300.0f; // kg/m3
	float max_time = 180.0f; // s
	bool natural_release = true;
	float debris_share = 0.02f; // of the deposit, broken into clumps on its surface
	float debris_radius = 60.0f; // m round the camera

	// Recent snow per cover cell (m), and its age in hours, in the cover's cell coordinates.
	Vector<float> recent;
	Vector<float> recent_age;
	Vector2i recent_origin;
	int recent_res = 0;
	uint64_t seen_update = UINT64_MAX;

	// The running flow, on the grid of the cover when it released.
	bool running = false;
	Vector2i flow_origin;
	int res = 0;
	float cell = 1.0f;
	Vector<float> h; // m
	Vector<float> hu; // m2/s
	Vector<float> hv;
	Vector<float> written; // kg/m2 of flow already written into the cover's cells
	Vector<float> deposit; // kg/m2 the flow left in each cell
	double flow_time = 0.0;
	double flush_accum = 0.0;
	float released_volume = 0.0f; // m3
	float entrained_volume = 0.0f;
	float deposited_volume = 0.0f;
	float max_speed = 0.0f;
	float run_out = 0.0f; // m, the farthest the front got from the release's centre
	Vector3 release_center;
	int release_count = 0;

	SnowCover3D *_cover() const;
	void _track_recent(SnowCover3D *p_cover);
	float _slope_at(const SnowCover3D *p_cover, int p_x, int p_z) const;
	bool _check_release(SnowCover3D *p_cover);
	void _release(SnowCover3D *p_cover, const Vector<int> &p_cells, const Vector<float> &p_slab);
	void _flow_step(SnowCover3D *p_cover, float p_dt);
	void _flush(SnowCover3D *p_cover);
	void _debris(const Vector3 &p_at, float p_volume);
	void _hit_bodies(SnowCover3D *p_cover);
	void _finish(SnowCover3D *p_cover);
	int _cover_index(const SnowCover3D *p_cover, int p_i) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Stability index of a slab: the weak layer's shear strength (Pa) over the slab's shear stress.
	static float weak_layer_strength(float p_density, float p_age_hours); // Pa
	static float stability_index(float p_slab, float p_slab_density, float p_weak_density, float p_age_hours, float p_slope);

	bool trigger(const Vector3 &p_at, float p_radius); // release the slab round a point now (a skier, a cornice, a test)
	void step(double p_delta); // run the flow now, for tests
	bool is_running() const { return running; }
	int get_release_count() const { return release_count; }
	float get_released_volume() const { return released_volume; }
	float get_entrained_volume() const { return entrained_volume; }
	float get_deposited_volume() const { return deposited_volume; }
	float get_flowing_volume() const;
	float get_max_speed() const { return max_speed; }
	float get_run_out() const { return run_out; }
	float get_flow_height_at(const Vector3 &p_position) const;
	Vector3 get_flow_velocity_at(const Vector3 &p_position) const;
	float get_recent_snow_at(const Vector3 &p_position) const;

	void set_mu(float p_mu) { mu = MAX(p_mu, 0.0f); }
	float get_mu() const { return mu; }
	void set_xi(float p_xi) { xi = MAX(p_xi, 1.0f); }
	float get_xi() const { return xi; }
	void set_min_slope(float p_deg) { min_slope = CLAMP(p_deg, 0.0f, 90.0f); }
	float get_min_slope() const { return min_slope; }
	void set_max_slope(float p_deg) { max_slope = CLAMP(p_deg, 0.0f, 90.0f); }
	float get_max_slope() const { return max_slope; }
	void set_min_slab(float p_m) { min_slab = MAX(p_m, 0.0f); }
	float get_min_slab() const { return min_slab; }
	void set_entrainment(float p_rate) { entrainment = MAX(p_rate, 0.0f); }
	float get_entrainment() const { return entrainment; }
	void set_natural_release(bool p_enable) { natural_release = p_enable; }
	bool is_natural_release() const { return natural_release; }
};

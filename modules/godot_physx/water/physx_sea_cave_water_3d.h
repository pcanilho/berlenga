/**************************************************************************/
/*  physx_water_mist_3d.h                                                 */
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
#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"

class MeshInstance3D;
class ShaderMaterial;

// The sea inside sea caves (docs/ideas/water.md, "Sea-cave water"): CaveNetwork3D carries the sea into each sea cave
// as a long wave along the passage (its surge, the water contract's answer there); this draws that water with the
// river's surface (clear water, sky and sun reflections, white water, the path tracer's refracting medium). A surface
// per sea cave along the passage's line: each station's level, half width and current from
// CaveNetwork3D.get_sea_cave_water(), rebuilt every frame, from the arch in (the sea's own surface hands over there).
// On it the short waves that come in at the mouth and reflect off the walls and the back of the cave, as Water
// Surface Wavelets (Jeschke et al. 2018): an amplitude per place and direction, carried at the group speed by the
// current too, mirrored at the walls, drawn with a shared wave profile.
class PhysXSeaCaveWater3D : public Node3D {
	GDCLASS(PhysXSeaCaveWater3D, Node3D);

	NodePath cave_network;
	int lanes = 17; // vertices across the passage
	float station_step = 0.25f; // m between rows of vertices along the passage
	float wall_margin = 0.25f; // m past the passage's half width, into the walls, so no gap shows at the rock
	float white_water_speed = 1.5f; // m/s: the current where white water starts (it is full at twice that)
	float fade_length = 4.0f; // m in from the arch over which the sea's surface hands over to this one
	bool wavelets = true;
	float wave_inflow = -1.0f; // rms height (m) of the short waves coming in at the mouth; below 0, from the sea there
	float wave_inflow_share = 0.5f; // of the sea's rms at the mouth (the rest is the surge's long wave)
	float wave_decay_time = 12.0f; // s for the waves to lose 1/e of their height (friction, breaking on the rock)
	float wall_reflection = 0.85f; // of the height a wave keeps off a wall (rough rock scatters the rest)

	static constexpr int WAVELET_DIRS = 16;
	static constexpr int WAVELET_ACROSS = 12;
	static constexpr float WAVELET_CELL = 0.5f; // m along
	static constexpr float WAVELET_VERTEX_SPACING = 0.25f; // m across at most while the wavelets are drawn
	static constexpr int PROFILE_SIZE = 1024;
	static constexpr float PROFILE_PERIOD = 51.2f; // m

	struct Ribbon {
		int cave = -1;
		MeshInstance3D *instance = nullptr;
		Ref<ArrayMesh> mesh;
		int cells = 0; // along; WAVELET_ACROSS across, WAVELET_DIRS directions each
		int vertex_count = -1, index_count = -1; // the surface's, while its layout holds (updated in place)
		LocalVector<float> amp, amp_next;
	};
	LocalVector<Ribbon> ribbons;
	Ref<ShaderMaterial> material;
	double water_time = 0.0;
	LocalVector<float> profile; // the wave profile (unit variance) at water_time

	void _update(double p_delta, bool p_mesh = true);
	void _clear();
	void _update_profile();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_cave_network(const NodePath &p_path);
	NodePath get_cave_network() const { return cave_network; }
	void set_lanes(int p_lanes);
	int get_lanes() const { return lanes; }
	void set_station_step(float p_step) { station_step = CLAMP(p_step, 0.05f, 5.0f); }
	float get_station_step() const { return station_step; }
	void set_wall_margin(float p_margin) { wall_margin = MAX(p_margin, 0.0f); }
	float get_wall_margin() const { return wall_margin; }
	void set_white_water_speed(float p_speed) { white_water_speed = MAX(p_speed, 0.01f); }
	float get_white_water_speed() const { return white_water_speed; }
	void set_fade_length(float p_length) { fade_length = MAX(p_length, 0.01f); }
	float get_fade_length() const { return fade_length; }
	void set_wavelets(bool p_on) { wavelets = p_on; }
	bool get_wavelets() const { return wavelets; }
	void set_wave_inflow(float p_rms) { wave_inflow = p_rms; }
	float get_wave_inflow() const { return wave_inflow; }
	void set_wave_inflow_share(float p_share) { wave_inflow_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_wave_inflow_share() const { return wave_inflow_share; }
	void set_wave_decay_time(float p_time) { wave_decay_time = MAX(p_time, 0.1f); }
	float get_wave_decay_time() const { return wave_decay_time; }
	void set_wall_reflection(float p_r) { wall_reflection = CLAMP(p_r, 0.0f, 1.0f); }
	float get_wall_reflection() const { return wall_reflection; }

	int get_ribbon_count() const { return int(ribbons.size()); }
	Ref<ShaderMaterial> get_water_material() const { return material; }
	// The short waves' rms height (m) over a sea cave's surface (0 without wavelets), for tests.
	float get_wave_rms(int p_cave) const;
	// Rebuilds the surfaces from the network now, stepping the waves p_delta seconds (it does so every frame).
	void update_now(double p_delta = 0.0) { _update(p_delta); }
	// Steps the waves p_seconds without drawing between (stills: the waves take about a minute to fill a cave), then
	// builds the surfaces once.
	void warm_up(double p_seconds);
};

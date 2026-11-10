/**************************************************************************/
/*  weather_lightning_3d.h                                                */
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

#include "core/math/random_pcg.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class AudioStreamPlayer3D;
class AudioStreamWAV;
class MeshInstance3D;
class OmniLight3D;
class WeatherMap3D;

// Lightning from the active WeatherMap3D's storms, and its thunder (docs/ideas/weather.md,
// "Lightning and thunder"). Each storm flashes at the Price and Rind (1992) rate for the height of
// the cloud the sky draws over it; the cold cloud depth sets the share that reach the ground
// (Price and Rind 1993). A ground flash is a channel of tubes grown as a Reed and Wyvill (1994)
// random walk, with an emissive material the path tracer samples as a light; the rasterizer gets
// an OmniLight3D at the channel instead, which is off under path tracing. Flashes glow in a
// WeatherSkyMaterial's clouds. Thunder is made from the channel (Few 1969, Ribner and Roy 1982):
// an N-wave per piece, late by its distance at the speed of sound, filtered by the air.
class WeatherLightning3D : public Node3D {
	GDCLASS(WeatherLightning3D, Node3D);

public:
	static constexpr int MAX_FLASHES = 4; // at once; the sky material holds as many

	// The thunder of one flash, made on a worker thread. Public for the worker's template.
	struct Thunder {
		// In: channel pieces (from, to, loudness), the strokes that re-light the main channel.
		LocalVector<Vector3> from;
		LocalVector<Vector3> to;
		LocalVector<float> gain;
		LocalVector<uint8_t> main; // 1: on the main channel, sounds again with each stroke
		LocalVector<float> stroke_time; // s after the flash starts
		LocalVector<float> stroke_gain;
		Vector3 listener;
		float speed_of_sound = 340.0f;
		float energy_per_metre = 1.0e5f; // J/m, sets the N-wave period (Few's relaxation radius)
		uint32_t seed = 1;
		int mix_rate = 22050;
		// Out.
		Vector<uint8_t> pcm; // 16-bit mono
		float delay = 0.0f; // s from the flash to the first sound
		float length = 0.0f; // s
		float peak = 0.0f; // before the soft clip
		float build_ms = 0.0f;
		void build();
	};

private:
	struct Flash {
		uint64_t id = 0;
		double start = 0.0; // node clock, s
		bool cloud_to_ground = true;
		Vector3 ground; // where it struck (ground flashes) or the centre of the glow (cloud flashes)
		Vector3 glow; // where the cloud glows
		float leader = 0.02f; // s, the stepped leader coming down
		LocalVector<float> stroke_time;
		LocalVector<float> stroke_peak;
		float continuing_start = 0.0f;
		float continuing_length = 0.0f;
		float end = 0.5f; // s, when its light is gone
		float width = 1.0f; // m, of the main channel
		float length = 0.0f; // m, of the main channel
		LocalVector<Vector3> main_channel; // cloud to ground
		LocalVector<LocalVector<Vector3>> branches;
		LocalVector<LocalVector<Vector3>> in_cloud; // heard, not drawn
		MeshInstance3D *mesh = nullptr;
		OmniLight3D *light = nullptr;
		Ref<StandardMaterial3D> main_material;
		Ref<StandardMaterial3D> branch_material;
		float brightness = 0.0f; // main channel, this frame, frame averaged
		float branch_brightness = 0.0f;
	};

	struct PendingThunder {
		Thunder *job = nullptr;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		double start = 0.0; // node clock of the flash
		Vector3 position; // the nearest piece of channel: where the sound comes from
		AudioStreamPlayer3D *player = nullptr;
	};

	bool enabled = true;
	float rate_scale = 1.0f;
	bool marine = false;
	float strike_range = 20000.0f;
	float channel_width = 0.5f;
	float min_pixel_width = 1.5f;
	float channel_energy = 6000.0f;
	Color channel_color = Color(0.82f, 0.86f, 1.0f);
	float sky_flash_energy = 2.0e7f;
	float raster_light_scale = 1.0f;
	bool raster_light_shadows = true;
	bool thunder_enabled = true;
	float thunder_volume_db = 0.0f;
	StringName thunder_bus = SNAME("Master");
	uint32_t seed = 1;

	RandomPCG rng;
	double clock = 0.0;
	uint64_t next_id = 1;
	LocalVector<Flash> flashes;
	LocalVector<PendingThunder> thunders;
	int strike_count = 0;
	int ground_count = 0;
	float last_thunder_delay = 0.0f;
	float last_thunder_length = 0.0f;
	float last_thunder_ms = 0.0f;
	bool sky_had_flashes = false;

	void _process_frame(double p_delta);
	void _storm_strikes(WeatherMap3D *p_map, double p_delta, const Vector3 &p_listener);
	int _start_flash(const Vector3 &p_position, bool p_cloud_to_ground, WeatherMap3D *p_map, float p_storm_radius);
	void _grow_channel(Flash &r_flash, const Vector3 &p_top, const Vector3 &p_ground);
	void _grow_in_cloud(Flash &r_flash, const Vector3 &p_centre, float p_spread, float p_height);
	void _plan_timing(Flash &r_flash);
	void _build_mesh(Flash &r_flash, const Vector3 &p_camera, float p_pixel_angle, bool p_orthogonal, float p_pixel_size);
	void _start_thunder(const Flash &p_flash, const Vector3 &p_listener, float p_temperature);
	void _free_flash(Flash &r_flash);
	float _frame_brightness(const Flash &p_flash, float p_a, float p_b, bool p_branches) const;
	void _push_sky();
	bool _path_traced() const;
	Vector3 _listener() const;
	float _ground_at(const Vector3 &p_from, float p_fallback) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_rate_scale(float p_scale);
	float get_rate_scale() const { return rate_scale; }
	void set_marine(bool p_marine);
	bool is_marine() const { return marine; }
	void set_strike_range(float p_range);
	float get_strike_range() const { return strike_range; }
	void set_channel_width(float p_width);
	float get_channel_width() const { return channel_width; }
	void set_min_pixel_width(float p_pixels);
	float get_min_pixel_width() const { return min_pixel_width; }
	void set_channel_energy(float p_energy);
	float get_channel_energy() const { return channel_energy; }
	void set_channel_color(const Color &p_color);
	Color get_channel_color() const { return channel_color; }
	void set_sky_flash_energy(float p_energy);
	float get_sky_flash_energy() const { return sky_flash_energy; }
	void set_raster_light_scale(float p_scale);
	float get_raster_light_scale() const { return raster_light_scale; }
	void set_raster_light_shadows(bool p_enabled);
	bool get_raster_light_shadows() const { return raster_light_shadows; }
	void set_thunder_enabled(bool p_enabled);
	bool is_thunder_enabled() const { return thunder_enabled; }
	void set_thunder_volume_db(float p_db);
	float get_thunder_volume_db() const { return thunder_volume_db; }
	void set_thunder_bus(const StringName &p_bus);
	StringName get_thunder_bus() const { return thunder_bus; }
	void set_seed(int p_seed);
	int get_seed() const { return int(seed); }

	// Flashes per minute of one storm cell with this cloud top (m above the ground), Price and Rind.
	float get_flash_rate(float p_cloud_top) const;
	// The share of flashes that reach the ground for a cloud top and freezing level (m), Price and Rind 1993.
	static float ground_flash_fraction(float p_cloud_top, float p_freezing_level);

	// A flash now, at world x, z (y is found by a ray down); returns its id.
	int strike(const Vector3 &p_position, bool p_cloud_to_ground = true);
	int get_active_flash_count() const { return flashes.size(); }
	int get_strike_count() const { return strike_count; }
	int get_ground_strike_count() const { return ground_count; }
	// The last flash's main channel, cloud to ground, and its brightness this frame (0..).
	PackedVector3Array get_last_channel() const;
	float get_flash_brightness() const;
	// The last thunder: seconds from its flash to the first sound, its length, and how long it took to make.
	float get_last_thunder_delay() const { return last_thunder_delay; }
	float get_last_thunder_length() const { return last_thunder_length; }
	float get_last_thunder_build_time() const { return last_thunder_ms; }
	// Makes thunder for a channel heard from p_listener, without playing it (for tests and tools).
	Ref<AudioStreamWAV> make_thunder(const PackedVector3Array &p_channel, const Vector3 &p_listener, float p_temperature = 15.0f);

	WeatherLightning3D();
	~WeatherLightning3D();
};

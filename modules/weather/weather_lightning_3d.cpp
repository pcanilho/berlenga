/**************************************************************************/
/*  weather_lightning_3d.cpp                                              */
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

#include "weather_lightning_3d.h"

#include "weather_clouds.h"
#include "weather_map_3d.h"
#include "weather_sky_material.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/audio_stream_wav.h"
#include "scene/resources/mesh.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {

constexpr float AIR_PRESSURE = 101325.0f; // Pa
constexpr float LEADER_SPEED = 2.0e5f; // m/s, a stepped leader's mean speed (Rakov and Uman)
constexpr float COARSE_STEP = 30.0f; // m, the random walk's step before the fine kinks
constexpr int TUBE_SIDES = 5;

// p_dir turned by p_angle about a random axis across it.
Vector3 turn(const Vector3 &p_dir, float p_angle, RandomPCG &p_rng) {
	Vector3 axis = p_dir.cross(Vector3(p_rng.randf() * 2.0f - 1.0f, p_rng.randf() * 2.0f - 1.0f, p_rng.randf() * 2.0f - 1.0f));
	if (axis.length_squared() < 1e-8f) {
		axis = p_dir.get_any_perpendicular();
	}
	return p_dir.rotated(axis.normalized(), p_angle).normalized();
}

// Reed and Wyvill's turn: about 16 degrees from the parent, normally distributed (the spread is ours).
float walk_angle(RandomPCG &p_rng) {
	return Math::deg_to_rad(MAX(p_rng.randfn(16.0f, 6.0f), 0.0f));
}

// The fine kinks: each piece's midpoint moved across it, twice (midpoint displacement), so the
// channel is tortuous at every scale down to a few metres, as Few describes.
void refine(LocalVector<Vector3> &r_points, int p_levels, float p_jitter, RandomPCG &p_rng) {
	for (int level = 0; level < p_levels; level++) {
		if (r_points.size() < 2) {
			return;
		}
		LocalVector<Vector3> out;
		out.reserve(r_points.size() * 2);
		for (uint32_t i = 0; i + 1 < r_points.size(); i++) {
			const Vector3 a = r_points[i];
			const Vector3 b = r_points[i + 1];
			const Vector3 d = b - a;
			const float len = d.length();
			out.push_back(a);
			if (len > 1e-3f) {
				const Vector3 across = turn(d / len, Math::PI * 0.5f, p_rng);
				out.push_back((a + b) * 0.5f + across * (p_rng.randfn(0.0f, 1.0f) * len * p_jitter));
			}
		}
		out.push_back(r_points[r_points.size() - 1]);
		r_points = out;
	}
}

// A tube along p_points: TUBE_SIDES around, frames carried along by parallel transport.
void add_tube(const LocalVector<Vector3> &p_points, float p_radius, const Vector3 &p_origin, PackedVector3Array &r_vertices, PackedVector3Array &r_normals, PackedInt32Array &r_indices) {
	const int n = p_points.size();
	if (n < 2) {
		return;
	}
	const int base = r_vertices.size();
	Vector3 normal;
	for (int i = 0; i < n; i++) {
		const Vector3 t = (p_points[MIN(i + 1, n - 1)] - p_points[MAX(i - 1, 0)]).normalized();
		if (i == 0) {
			normal = t.get_any_perpendicular().normalized();
		} else {
			normal = (normal - t * normal.dot(t));
			normal = normal.length_squared() > 1e-8f ? normal.normalized() : t.get_any_perpendicular().normalized();
		}
		const Vector3 binormal = t.cross(normal);
		// The tip narrows to a point at the ends of branches.
		const float r = p_radius * (i == n - 1 ? 0.3f : 1.0f);
		for (int s = 0; s < TUBE_SIDES; s++) {
			const float a = Math::TAU * s / TUBE_SIDES;
			const Vector3 dir = normal * Math::cos(a) + binormal * Math::sin(a);
			r_vertices.push_back(p_points[i] - p_origin + dir * r);
			r_normals.push_back(dir);
		}
	}
	for (int i = 0; i + 1 < n; i++) {
		for (int s = 0; s < TUBE_SIDES; s++) {
			const int a = base + i * TUBE_SIDES + s;
			const int b = base + i * TUBE_SIDES + (s + 1) % TUBE_SIDES;
			const int c = a + TUBE_SIDES;
			const int d = b + TUBE_SIDES;
			r_indices.push_back(a);
			r_indices.push_back(c);
			r_indices.push_back(b);
			r_indices.push_back(b);
			r_indices.push_back(c);
			r_indices.push_back(d);
		}
	}
}

// Light a return stroke gives off up to x seconds after it starts, for a stroke of peak 1. A fast
// part (the stroke itself, decaying in about 2 ms) and a slow one (the channel cooling, about
// 30 ms): Guo and Krider's light rises in microseconds, so only the integral over a frame matters.
// Scaled so the first frame of a stroke averages about 1 at 60 frames a second.
float stroke_light(float p_x) {
	if (p_x <= 0.0f) {
		return 0.0f;
	}
	constexpr float FAST = 0.7f / 60.0f;
	constexpr float SLOW = 0.3f / 60.0f / 0.43f;
	return FAST * (1.0f - Math::exp(-p_x / 0.002f)) + SLOW * (1.0f - Math::exp(-p_x / 0.03f));
}

void thunder_task(void *p_userdata) {
	static_cast<WeatherLightning3D::Thunder *>(p_userdata)->build();
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Thunder (Few 1969; Ribner and Roy 1982; Blanco et al. 2009).

void WeatherLightning3D::Thunder::build() {
	const uint64_t t_start = OS::get_singleton()->get_ticks_usec();
	RandomPCG rng(seed);
	const int count = from.size();
	pcm.clear();
	if (count == 0) {
		return;
	}
	const float c = speed_of_sound;
	// Few's relaxation radius and the N-wave it sends: R0 = (E / (pi p0))^1/2, loudest at 0.63 c / R0.
	const float r0 = Math::sqrt(energy_per_metre / (Math::PI * AIR_PRESSURE));

	float last_stroke = 0.0f;
	for (float s : stroke_time) {
		last_stroke = MAX(last_stroke, s);
	}
	const float first_stroke = stroke_time.is_empty() ? 0.0f : stroke_time[0];

	// When the first and last sound arrive.
	float t_first = 1e30f;
	float t_last = 0.0f;
	for (int i = 0; i < count; i++) {
		const float r = MAX(MIN(from[i].distance_to(listener), to[i].distance_to(listener)), 1.0f);
		const float len = from[i].distance_to(to[i]);
		const float t = r / c + first_stroke;
		t_first = MIN(t_first, t);
		t_last = MAX(t_last, t + len / c + (main[i] ? last_stroke - first_stroke : 0.0f));
	}
	delay = t_first;
	const float tail = 0.6f;
	length = t_last - t_first + tail;
	const int samples = MAX(int(length * mix_rate), 1);

	// Air takes the high frequencies (ISO 9613-1, about 20 C and 70% humidity): roughly
	// 5 dB/km at 1 kHz rising as f^1.3. Pieces are grouped by distance in octave bands, and each band
	// is low-passed where its absorption reaches 3 dB.
	constexpr int BANDS = 8;
	LocalVector<float> main_band[BANDS];
	LocalVector<float> other_band[BANDS];
	bool used_main[BANDS] = {};
	bool used_other[BANDS] = {};

	for (int i = 0; i < count; i++) {
		const Vector3 d = to[i] - from[i];
		const float len = d.length();
		if (len < 1e-3f) {
			continue;
		}
		const Vector3 mid = (from[i] + to[i]) * 0.5f;
		const Vector3 to_listener = listener - mid;
		const float r = MAX(to_listener.length(), 1.0f);
		const int band = CLAMP(int(Math::floor(Math::log2(MAX(r, 250.0f) / 250.0f))), 0, BANDS - 1);
		LocalVector<float> &buf = main[i] ? main_band[band] : other_band[band];
		if (buf.is_empty()) {
			buf.resize(samples);
			memset(buf.ptr(), 0, sizeof(float) * samples);
		}
		(main[i] ? used_main : used_other)[band] = true;

		// Each piece's N-wave: its own relaxation radius (the energy varies along the channel).
		const float radius = r0 * Math::exp(rng.randfn(0.0f, 0.35f));
		const float period = radius / (0.63f * c);
		const int wave = MAX(int(period * mix_rate), 2);
		// A piece seen end-on spreads its sound over its own length (the rumble); seen broadside it
		// arrives at once (a clap). The piece is cut into parts no more than half a period apart.
		const float cos_t = Math::abs(d.dot(to_listener)) / (len * r);
		const float spread = len * cos_t / c;
		const int parts = CLAMP(int(Math::ceil(spread / (0.5f * period))), 1, 64);
		// Pressure falls as 1/r from each part; louder for more channel per part.
		const float amp = gain[i] * (len / parts) / (r * c * period);
		// The near end of the piece first.
		const float t_near = MIN(from[i].distance_to(listener), to[i].distance_to(listener)) / c;
		const float t0 = t_near + first_stroke - delay;
		for (int p = 0; p < parts; p++) {
			const int start = int((t0 + spread * (p + 0.5f) / parts) * mix_rate);
			for (int k = 0; k < wave; k++) {
				const int at = start + k;
				if (at < 0 || at >= samples) {
					continue;
				}
				buf[at] += amp * (1.0f - 2.0f * float(k) / float(wave - 1));
			}
		}
	}

	LocalVector<float> mix;
	mix.resize(samples);
	memset(mix.ptr(), 0, sizeof(float) * samples);
	for (int b = 0; b < BANDS; b++) {
		const float r_km = 0.25f * Math::pow(2.0f, b + 0.5f);
		const float cutoff = CLAMP(1000.0f * Math::pow(3.0f / (5.0f * r_km), 1.0f / 1.3f), 30.0f, 4000.0f);
		const float a = 1.0f - Math::exp(-Math::TAU * cutoff / mix_rate);
		for (int pass = 0; pass < 2; pass++) {
			LocalVector<float> &buf = pass == 0 ? main_band[b] : other_band[b];
			if (!(pass == 0 ? used_main : used_other)[b]) {
				continue;
			}
			// Two one-pole low-passes.
			float y1 = 0.0f;
			float y2 = 0.0f;
			for (int n = 0; n < samples; n++) {
				y1 += a * (buf[n] - y1);
				y2 += a * (y1 - y2);
				buf[n] = y2;
			}
			if (pass == 1) {
				for (int n = 0; n < samples; n++) {
					mix[n] += buf[n];
				}
				continue;
			}
			// The main channel sounds again with each return stroke, as loud as the root of its peak.
			for (uint32_t k = 0; k < stroke_time.size(); k++) {
				const int shift = int((stroke_time[k] - first_stroke) * mix_rate);
				const float g = k < stroke_gain.size() ? stroke_gain[k] : 1.0f;
				for (int n = 0; n + shift < samples; n++) {
					mix[n + shift] += g * buf[n];
				}
			}
		}
	}

	// Remove what is below hearing (one-pole high-pass at 15 Hz), then a soft clip.
	const float hp = Math::exp(-Math::TAU * 15.0f / mix_rate);
	float x_prev = 0.0f;
	float y_prev = 0.0f;
	constexpr float GAIN = 24.0f; // a flash 340 m off peaks near -4 dBFS before the soft clip
	peak = 0.0f;
	pcm.resize(samples * 2);
	uint8_t *w = pcm.ptrw();
	for (int n = 0; n < samples; n++) {
		const float x = mix[n] * GAIN;
		const float y = hp * (y_prev + x - x_prev);
		x_prev = x;
		y_prev = y;
		peak = MAX(peak, Math::abs(y));
		// Fade the tail out.
		const float fade = MIN(1.0f, float(samples - n) / (tail * 0.5f * mix_rate));
		const int16_t v = int16_t(CLAMP(Math::tanh(y) * fade, -1.0f, 1.0f) * 32767.0f);
		w[n * 2] = uint8_t(v & 0xff);
		w[n * 2 + 1] = uint8_t((v >> 8) & 0xff);
	}
	build_ms = (OS::get_singleton()->get_ticks_usec() - t_start) / 1000.0f;
}

// ---------------------------------------------------------------------------------------------------
// Rates

float WeatherLightning3D::get_flash_rate(float p_cloud_top) const {
	const float h = MAX(p_cloud_top, 0.0f) / 1000.0f;
	return (marine ? 6.57e-6f : 3.44e-5f) * Math::pow(h, 4.9f);
}

float WeatherLightning3D::ground_flash_fraction(float p_cloud_top, float p_freezing_level) {
	// Price and Rind (1993): the ratio of cloud to ground flashes from the cold cloud depth (km),
	// fitted over 5.5 to 14 km.
	const float d = CLAMP((p_cloud_top - p_freezing_level) / 1000.0f, 5.5f, 14.0f);
	const float z = 0.021f * d * d * d * d - 0.648f * d * d * d + 7.493f * d * d - 36.54f * d + 63.09f;
	return 1.0f / (1.0f + MAX(z, 0.0f));
}

// ---------------------------------------------------------------------------------------------------
// Flashes

Vector3 WeatherLightning3D::_listener() const {
	Viewport *vp = get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : get_global_position();
}

bool WeatherLightning3D::_path_traced() const {
	Viewport *vp = get_viewport();
	if (!vp || !vp->is_using_pathtracing()) {
		return false;
	}
	Ref<Environment> env = WeatherSkyMaterial::find_environment(const_cast<WeatherLightning3D *>(this));
	return env.is_valid() && env->is_pathtracing_enabled();
}

float WeatherLightning3D::_ground_at(const Vector3 &p_from, float p_fallback) const {
	Ref<World3D> world = get_world_3d();
	PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
	if (space) {
		PhysicsDirectSpaceState3D::RayParameters ray;
		ray.from = p_from;
		ray.to = Vector3(p_from.x, MIN(p_fallback, p_from.y) - 2000.0f, p_from.z);
		PhysicsDirectSpaceState3D::RayResult hit;
		if (space->intersect_ray(ray, hit)) {
			return hit.position.y;
		}
	}
	return p_fallback;
}

void WeatherLightning3D::_grow_channel(Flash &r_flash, const Vector3 &p_top, const Vector3 &p_ground) {
	struct Seed {
		Vector3 at;
		Vector3 dir;
		float length = 0.0f;
		int depth = 0;
	};
	LocalVector<Seed> seeds;

	// The main channel: a walk steered to the strike point, harder as it nears the ground.
	LocalVector<Vector3> walk;
	walk.push_back(p_top);
	Vector3 p = p_top;
	Vector3 dir = (p_ground - p_top).normalized();
	const float total = MAX(p_top.distance_to(p_ground), 1.0f);
	for (int guard = 0; guard < 4000 && p.distance_to(p_ground) > COARSE_STEP * 1.5f; guard++) {
		const float left = p.distance_to(p_ground) / total;
		const Vector3 to_goal = (p_ground - p).normalized();
		Vector3 nd = turn(dir, walk_angle(rng), rng);
		nd = (nd + to_goal * Math::lerp(0.3f, 1.5f, (1.0f - left) * (1.0f - left))).normalized();
		p += nd * (COARSE_STEP * (0.5f + rng.randf()));
		walk.push_back(p);
		dir = nd;
		// Branches split off along the upper channel and die out before the ground.
		if (left > 0.15f && rng.randf() < 0.07f) {
			Seed s;
			s.at = p;
			s.dir = turn(nd, Math::deg_to_rad(rng.random(25.0f, 55.0f)), rng);
			s.dir.y = MIN(s.dir.y, -0.2f);
			s.dir.normalize();
			s.length = total * left * rng.random(0.15f, 0.45f);
			seeds.push_back(s);
		}
	}
	walk.push_back(p_ground);
	refine(walk, 2, 0.12f, rng);
	r_flash.main_channel = walk;
	r_flash.length = 0.0f;
	for (uint32_t i = 0; i + 1 < walk.size(); i++) {
		r_flash.length += walk[i].distance_to(walk[i + 1]);
	}

	// Branches: walks that lean down and away, a second generation off the first.
	const float floor_y = p_ground.y + 5.0f;
	for (uint32_t si = 0; si < seeds.size() && r_flash.branches.size() < 24; si++) {
		const Seed s = seeds[si];
		LocalVector<Vector3> b;
		b.push_back(s.at);
		Vector3 q = s.at;
		Vector3 bd = s.dir;
		float done = 0.0f;
		while (done < s.length && q.y > floor_y) {
			Vector3 nd = turn(bd, walk_angle(rng), rng);
			nd = (nd + Vector3(0, -0.2f, 0)).normalized();
			const float step = COARSE_STEP * 0.7f * (0.5f + rng.randf());
			q += nd * step;
			done += step;
			b.push_back(q);
			bd = nd;
			if (s.depth == 0 && rng.randf() < 0.05f) {
				Seed sub;
				sub.at = q;
				sub.dir = turn(nd, Math::deg_to_rad(rng.random(25.0f, 55.0f)), rng);
				sub.dir.y = MIN(sub.dir.y, -0.2f);
				sub.dir.normalize();
				sub.length = (s.length - done) * rng.random(0.2f, 0.5f);
				sub.depth = 1;
				seeds.push_back(sub);
			}
		}
		if (b.size() >= 2) {
			refine(b, 2, 0.12f, rng);
			r_flash.branches.push_back(std::move(b));
		}
	}
}

void WeatherLightning3D::_grow_in_cloud(Flash &r_flash, const Vector3 &p_centre, float p_spread, float p_height) {
	// Channels that run sideways through the charge layers: heard as the long rumble, not drawn.
	const int arms = 2 + int(rng.randf() * 3.0f);
	for (int a = 0; a < arms; a++) {
		const float angle = rng.randf() * Math::TAU;
		Vector3 dir(Math::cos(angle), 0.0f, Math::sin(angle));
		LocalVector<Vector3> arm;
		Vector3 p = p_centre;
		arm.push_back(p);
		const float length = p_spread * rng.random(0.5f, 1.0f);
		for (float done = 0.0f; done < length;) {
			Vector3 nd = turn(dir, walk_angle(rng), rng);
			nd.y *= 0.3f;
			// Stay within the layer.
			nd.y -= (p.y - p_centre.y) / MAX(p_height, 1.0f) * 0.3f;
			nd.normalize();
			const float step = COARSE_STEP * 2.0f * (0.5f + rng.randf());
			p += nd * step;
			done += step;
			arm.push_back(p);
			dir = nd;
		}
		refine(arm, 1, 0.12f, rng);
		r_flash.in_cloud.push_back(std::move(arm));
	}
}

void WeatherLightning3D::_plan_timing(Flash &r_flash) {
	r_flash.stroke_time.clear();
	r_flash.stroke_peak.clear();
	if (r_flash.cloud_to_ground) {
		// The stepped leader comes down in the dark at about 2e5 m/s, then 1 + Poisson(2.5) return
		// strokes, a geometric mean of 60 ms apart (Rakov and Uman 2003). Later strokes carry a third to
		// two thirds of the first one's current.
		r_flash.leader = MAX(r_flash.length / LEADER_SPEED, 0.005f);
		int strokes = 1;
		const float l = Math::exp(-2.5f);
		float p = rng.randf();
		while (p > l && strokes < 8) {
			p *= rng.randf();
			strokes++;
		}
		float t = r_flash.leader;
		for (int k = 0; k < strokes; k++) {
			r_flash.stroke_time.push_back(t);
			r_flash.stroke_peak.push_back(k == 0 ? 1.0f : rng.random(0.3f, 0.7f));
			t += Math::exp(Math::log(0.06f) + rng.randfn(0.0f, 0.6f));
		}
		// About a third carry a continuing current after one of the strokes: a dim steady glow.
		r_flash.continuing_length = 0.0f;
		if (rng.randf() < 0.3f) {
			const int k = strokes > 1 ? 1 + int(rng.randf() * (strokes - 1)) : 0;
			r_flash.continuing_start = r_flash.stroke_time[MIN(k, strokes - 1)];
			r_flash.continuing_length = rng.random(0.04f, 0.3f);
		}
		r_flash.end = MAX(r_flash.stroke_time[strokes - 1], r_flash.continuing_start + r_flash.continuing_length) + 0.25f;
	} else {
		// In-cloud flashes: several weaker pulses over a few hundred milliseconds.
		r_flash.leader = 0.0f;
		const int pulses = 3 + int(rng.randf() * 6.0f);
		const float span = rng.random(0.2f, 0.5f);
		for (int k = 0; k < pulses; k++) {
			r_flash.stroke_time.push_back(span * k / pulses + rng.randf() * 0.02f);
			r_flash.stroke_peak.push_back(rng.random(0.2f, 0.6f));
		}
		r_flash.end = span + 0.25f;
	}
}

float WeatherLightning3D::_frame_brightness(const Flash &p_flash, float p_a, float p_b, bool p_branches) const {
	const float dt = MAX(p_b - p_a, 1.0f / 240.0f);
	float sum = 0.0f;
	for (uint32_t k = 0; k < p_flash.stroke_time.size(); k++) {
		// Branches carry only the first stroke; later ones follow the dart leader down the main channel.
		if (p_branches && k > 0) {
			break;
		}
		const float t = p_flash.stroke_time[k];
		sum += p_flash.stroke_peak[k] * (stroke_light(p_b - t) - stroke_light(p_a - t));
	}
	if (!p_branches && p_flash.continuing_length > 0.0f) {
		const float a = MAX(p_a, p_flash.continuing_start);
		const float b = MIN(p_b, p_flash.continuing_start + p_flash.continuing_length);
		sum += MAX(b - a, 0.0f) * 0.08f;
	}
	return sum / dt;
}

void WeatherLightning3D::_build_mesh(Flash &r_flash, const Vector3 &p_camera, float p_pixel_angle, bool p_orthogonal, float p_pixel_size) {
	// One width for the whole channel, held to at least min_pixel_width pixels at its mean distance;
	// the radiance falls as the width grows, so a flash gives the same light at any distance.
	Vector3 mean;
	for (const Vector3 &v : r_flash.main_channel) {
		mean += v;
	}
	mean /= MAX(int(r_flash.main_channel.size()), 1);
	const float pixel = p_orthogonal ? p_pixel_size : p_pixel_angle * mean.distance_to(p_camera);
	r_flash.width = MAX(channel_width, min_pixel_width * pixel);

	const Vector3 origin = r_flash.ground;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for (int surface = 0; surface < 2; surface++) {
		PackedVector3Array vertices;
		PackedVector3Array normals;
		PackedInt32Array indices;
		if (surface == 0) {
			add_tube(r_flash.main_channel, r_flash.width * 0.5f, origin, vertices, normals, indices);
		} else {
			for (const LocalVector<Vector3> &b : r_flash.branches) {
				add_tube(b, r_flash.width * 0.3f, origin, vertices, normals, indices);
			}
		}
		if (indices.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_INDEX] = indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		// Opaque and lit, so the path tracer takes its emission as a light (emissive next event estimation).
		m->set_albedo(Color(0, 0, 0));
		m->set_roughness(1.0f);
		m->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
		m->set_emission(channel_color);
		m->set_emission_energy_multiplier(0.0f);
		mesh->surface_set_material(mesh->get_surface_count() - 1, m);
		if (surface == 0) {
			r_flash.main_material = m;
		} else {
			r_flash.branch_material = m;
		}
	}
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(mesh);
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mi->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	mi->set_as_top_level(true);
	mi->set_visible(false);
	add_child(mi, false, INTERNAL_MODE_BACK);
	mi->set_global_transform(Transform3D(Basis(), origin));
	r_flash.mesh = mi;
}

int WeatherLightning3D::_start_flash(const Vector3 &p_position, bool p_cloud_to_ground, WeatherMap3D *p_map, float p_storm_radius) {
	if (!is_inside_tree()) {
		return 0;
	}
	if (flashes.size() >= MAX_FLASHES) {
		_free_flash(flashes[0]);
		flashes.remove_at(0);
	}
	WeatherClouds::State state;
	float ground_height = 0.0f;
	float temperature = 15.0f;
	if (p_map) {
		state = p_map->get_cloud_state();
		ground_height = p_map->get_ground_height();
		temperature = p_map->get_air_temperature();
	}
	const Vector2 ct = WeatherClouds::cover_type_at(state, Vector2(p_position.x, p_position.z));
	const float top_height = WeatherClouds::top_height(state, MAX(ct.y, 0.9f));
	const float base = state.base_height;

	Flash f;
	f.id = next_id++;
	f.start = clock;
	f.cloud_to_ground = p_cloud_to_ground;
	const Vector3 listener = _listener();
	if (p_cloud_to_ground) {
		const float y = _ground_at(Vector3(p_position.x, base, p_position.z), ground_height);
		f.ground = Vector3(p_position.x, y, p_position.z);
		// The channel leaves the cloud base up to a few hundred metres to one side.
		const Vector3 top(p_position.x + rng.randfn(0.0f, 300.0f), base + 100.0f, p_position.z + rng.randfn(0.0f, 300.0f));
		f.glow = top + Vector3(0.0f, 0.15f * top_height, 0.0f);
		_grow_channel(f, top, f.ground);
		_grow_in_cloud(f, top, MIN(p_storm_radius, 4000.0f), 0.3f * top_height);
	} else {
		f.ground = Vector3(p_position.x, base + 0.3f * top_height, p_position.z);
		f.glow = f.ground;
		_grow_in_cloud(f, f.glow, MIN(p_storm_radius, 4000.0f), 0.3f * top_height);
	}
	_plan_timing(f);

	if (p_cloud_to_ground) {
		Viewport *vp = get_viewport();
		Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
		const float height = vp ? MAX(vp->get_visible_rect().size.y, 1.0f) : 1080.0f;
		const bool ortho = cam && cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
		const float fov = cam ? Math::deg_to_rad(cam->get_fov()) : Math::deg_to_rad(75.0f);
		_build_mesh(f, listener, 2.0f * Math::tan(fov * 0.5f) / height, ortho, cam ? cam->get_size() / height : 0.0f);
	}

	// The light for the rasterizer: at the channel point nearest the camera (cloud flashes: in the cloud).
	Vector3 light_at = f.glow;
	if (p_cloud_to_ground) {
		float best = 1e30f;
		for (const Vector3 &v : f.main_channel) {
			const float d = v.distance_squared_to(listener);
			if (d < best) {
				best = d;
				light_at = v;
			}
		}
	}
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_as_top_level(true);
	light->set_color(channel_color);
	light->set_param(Light3D::PARAM_ENERGY, 0.0f);
	light->set_param(Light3D::PARAM_ATTENUATION, 2.0f); // inverse square, as the energy below assumes
	const float d_cam = MAX(light_at.distance_to(listener), 50.0f);
	light->set_param(Light3D::PARAM_RANGE, CLAMP(d_cam * 4.0f, 500.0f, 60000.0f));
	light->set_shadow(raster_light_shadows);
	light->set_visible(false);
	add_child(light, false, INTERNAL_MODE_BACK);
	light->set_global_position(light_at);
	f.light = light;

	flashes.push_back(f);
	strike_count++;
	if (p_cloud_to_ground) {
		ground_count++;
	}
	if (thunder_enabled) {
		_start_thunder(flashes[flashes.size() - 1], listener, temperature);
	}
	emit_signal(SNAME("struck"), f.ground, p_cloud_to_ground, f.ground.distance_to(listener));
	return int(f.id);
}

void WeatherLightning3D::_start_thunder(const Flash &p_flash, const Vector3 &p_listener, float p_temperature) {
	Thunder *job = memnew(Thunder);
	auto add = [&](const LocalVector<Vector3> &p_line, float p_gain, bool p_main) {
		for (uint32_t i = 0; i + 1 < p_line.size(); i++) {
			job->from.push_back(p_line[i]);
			job->to.push_back(p_line[i + 1]);
			job->gain.push_back(p_gain);
			job->main.push_back(p_main ? 1 : 0);
		}
	};
	add(p_flash.main_channel, 1.0f, true);
	for (const LocalVector<Vector3> &b : p_flash.branches) {
		add(b, 0.6f, false);
	}
	for (const LocalVector<Vector3> &a : p_flash.in_cloud) {
		add(a, 0.7f, false);
	}
	for (uint32_t k = 0; k < p_flash.stroke_time.size(); k++) {
		job->stroke_time.push_back(p_flash.stroke_time[k]);
		// Pressure goes as the root of the energy.
		job->stroke_gain.push_back(Math::sqrt(p_flash.stroke_peak[k]));
	}
	job->listener = p_listener;
	job->speed_of_sound = 331.3f + 0.606f * p_temperature;
	job->energy_per_metre = 1.0e6f;
	job->seed = rng.rand();

	// Where the sound comes from: the nearest piece of channel.
	Vector3 nearest = p_flash.glow;
	float best = 1e30f;
	for (uint32_t i = 0; i < job->from.size(); i++) {
		const float d = job->from[i].distance_squared_to(p_listener);
		if (d < best) {
			best = d;
			nearest = job->from[i];
		}
	}

	PendingThunder t;
	t.job = job;
	t.start = p_flash.start;
	t.position = nearest;
	t.task = WorkerThreadPool::get_singleton()->add_native_task(&thunder_task, job, false, "Thunder");
	thunders.push_back(t);
}

void WeatherLightning3D::_free_flash(Flash &r_flash) {
	if (r_flash.mesh) {
		r_flash.mesh->queue_free();
		r_flash.mesh = nullptr;
	}
	if (r_flash.light) {
		r_flash.light->queue_free();
		r_flash.light = nullptr;
	}
}

void WeatherLightning3D::_storm_strikes(WeatherMap3D *p_map, double p_delta, const Vector3 &p_listener) {
	const WeatherClouds::State state = p_map->get_cloud_state();
	const float ground = p_map->get_ground_height();
	// The freezing level from the air temperature at the standard lapse rate (6.5 K/km).
	const float freezing = MAX(p_map->get_air_temperature(), 0.0f) / 6.5f * 1000.0f;
	for (const WeatherMap3D::Storm &s : p_map->get_storms()) {
		const float strength = p_map->get_storm_strength(s);
		if (strength <= 0.0f || s.position.distance_to(Vector2(p_listener.x, p_listener.z)) > strike_range + s.radius) {
			continue;
		}
		// The cloud the sky draws over the storm's centre: its top above the ground.
		const Vector2 ct = WeatherClouds::cover_type_at(state, s.position);
		const float top = state.base_height + WeatherClouds::top_height(state, ct.y) - ground;
		const float per_minute = get_flash_rate(top) * strength * rate_scale;
		const float p = 1.0f - Math::exp(-per_minute * float(p_delta) / 60.0f);
		if (rng.randf() >= p) {
			continue;
		}
		// Near the rain core.
		const Vector2 at = s.position + Vector2(rng.randfn(0.0f, 0.5f * s.radius), rng.randfn(0.0f, 0.5f * s.radius));
		const bool cg = rng.randf() < ground_flash_fraction(top, freezing);
		_start_flash(Vector3(at.x, 0.0f, at.y), cg, p_map, s.radius);
	}
}

void WeatherLightning3D::_push_sky() {
	Ref<WeatherSkyMaterial> sky = WeatherSkyMaterial::find_for(this);
	if (sky.is_null()) {
		return;
	}
	Vector4 lit[MAX_FLASHES];
	int count = 0;
	for (const Flash &f : flashes) {
		if (f.brightness > 1e-4f && count < MAX_FLASHES) {
			lit[count++] = Vector4(f.glow.x, f.glow.y, f.glow.z, sky_flash_energy * f.brightness);
		}
	}
	if (count == 0 && !sky_had_flashes) {
		return;
	}
	sky->set_lightning(lit, count, channel_color);
	sky_had_flashes = count > 0;
}

void WeatherLightning3D::_process_frame(double p_delta) {
	clock += p_delta;
	const Vector3 listener = _listener();
	WeatherMap3D *map = WeatherMap3D::get_active();
	if (enabled && map && p_delta > 0.0) {
		_storm_strikes(map, p_delta, listener);
	}

	const bool pt = _path_traced();
	for (int i = flashes.size() - 1; i >= 0; i--) {
		Flash &f = flashes[i];
		const float a = float(clock - p_delta - f.start);
		const float b = float(clock - f.start);
		if (a > f.end) {
			_free_flash(f);
			flashes.remove_at(i);
			continue;
		}
		f.brightness = _frame_brightness(f, a, b, false);
		f.branch_brightness = f.cloud_to_ground ? _frame_brightness(f, a, b, true) : 0.0f;
		const bool lit = f.brightness > 1e-4f || f.branch_brightness > 1e-4f;
		if (f.mesh) {
			f.mesh->set_visible(lit);
			if (f.main_material.is_valid()) {
				f.main_material->set_emission_energy_multiplier(channel_energy / f.width * f.brightness);
			}
			if (f.branch_material.is_valid()) {
				f.branch_material->set_emission_energy_multiplier(0.5f * channel_energy / (0.6f * f.width) * f.branch_brightness);
			}
		}
		if (f.light) {
			// A channel's irradiance at distance d is 2 L w / d; an omni light gives pi E / d^2, so
			// E = 2 L w d / pi matches it at the camera. Cloud flashes: a source of power W behind a
			// cloud that lets about 0.3 of it out below, E = 0.3 W / (4 pi^2).
			const float d = MAX(f.light->get_global_position().distance_to(listener), 50.0f);
			float energy;
			if (f.cloud_to_ground) {
				energy = 2.0f * channel_energy * d / Math::PI * MAX(f.brightness, f.branch_brightness);
			} else {
				energy = 0.3f * sky_flash_energy * f.brightness / (4.0f * Math::PI * Math::PI);
			}
			energy *= raster_light_scale;
			f.light->set_param(Light3D::PARAM_ENERGY, energy);
			// Under path tracing the channel itself is the light; the cloud flash keeps its light,
			// whose source is the glowing cloud.
			f.light->set_visible(energy > 1e-4f && !(pt && f.cloud_to_ground));
		}
	}
	_push_sky();

	for (int i = thunders.size() - 1; i >= 0; i--) {
		PendingThunder &t = thunders[i];
		if (t.task != WorkerThreadPool::INVALID_TASK_ID) {
			if (!WorkerThreadPool::get_singleton()->is_task_completed(t.task)) {
				continue;
			}
			WorkerThreadPool::get_singleton()->wait_for_task_completion(t.task);
			t.task = WorkerThreadPool::INVALID_TASK_ID;
		}
		if (t.player) {
			if (!t.player->is_playing()) {
				t.player->queue_free();
				memdelete(t.job);
				thunders.remove_at(i);
			}
			continue;
		}
		const double due = t.start + t.job->delay;
		if (!thunder_enabled || t.job->pcm.is_empty()) {
			memdelete(t.job);
			thunders.remove_at(i);
			continue;
		}
		if (clock < due) {
			continue;
		}
		Ref<AudioStreamWAV> wav;
		wav.instantiate();
		wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
		wav->set_mix_rate(t.job->mix_rate);
		wav->set_stereo(false);
		wav->set_data(t.job->pcm);
		AudioStreamPlayer3D *player = memnew(AudioStreamPlayer3D);
		player->set_stream(wav);
		player->set_attenuation_model(AudioStreamPlayer3D::ATTENUATION_DISABLED); // the distance is in the samples
		player->set_doppler_tracking(AudioStreamPlayer3D::DOPPLER_TRACKING_DISABLED);
		player->set_volume_db(thunder_volume_db);
		player->set_bus(thunder_bus);
		player->set_as_top_level(true);
		add_child(player, false, INTERNAL_MODE_BACK);
		player->set_global_position(t.position);
		player->play(MAX(float(clock - due), 0.0f));
		t.player = player;
		last_thunder_delay = t.job->delay;
		last_thunder_length = t.job->length;
		last_thunder_ms = t.job->build_ms;
		// The samples are no longer needed once the stream holds them.
		t.job->pcm.clear();
	}
}

void WeatherLightning3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_process_frame(get_process_delta_time());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			for (Flash &f : flashes) {
				_free_flash(f);
			}
			flashes.clear();
			_push_sky();
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------
// API

int WeatherLightning3D::strike(const Vector3 &p_position, bool p_cloud_to_ground) {
	return _start_flash(p_position, p_cloud_to_ground, WeatherMap3D::get_active(), 3000.0f);
}

PackedVector3Array WeatherLightning3D::get_last_channel() const {
	PackedVector3Array out;
	if (!flashes.is_empty()) {
		for (const Vector3 &v : flashes[flashes.size() - 1].main_channel) {
			out.push_back(v);
		}
	}
	return out;
}

float WeatherLightning3D::get_flash_brightness() const {
	float b = 0.0f;
	for (const Flash &f : flashes) {
		b = MAX(b, f.brightness);
	}
	return b;
}

Ref<AudioStreamWAV> WeatherLightning3D::make_thunder(const PackedVector3Array &p_channel, const Vector3 &p_listener, float p_temperature) {
	Thunder job;
	for (int i = 0; i + 1 < p_channel.size(); i++) {
		job.from.push_back(p_channel[i]);
		job.to.push_back(p_channel[i + 1]);
		job.gain.push_back(1.0f);
		job.main.push_back(1);
	}
	job.stroke_time.push_back(0.0f);
	job.stroke_gain.push_back(1.0f);
	job.listener = p_listener;
	job.speed_of_sound = 331.3f + 0.606f * p_temperature;
	job.energy_per_metre = 1.0e6f;
	job.seed = seed;
	job.build();
	last_thunder_delay = job.delay;
	last_thunder_length = job.length;
	last_thunder_ms = job.build_ms;
	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_mix_rate(job.mix_rate);
	wav->set_data(job.pcm);
	return wav;
}

void WeatherLightning3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
}

void WeatherLightning3D::set_rate_scale(float p_scale) {
	rate_scale = MAX(p_scale, 0.0f);
}

void WeatherLightning3D::set_marine(bool p_marine) {
	marine = p_marine;
}

void WeatherLightning3D::set_strike_range(float p_range) {
	strike_range = MAX(p_range, 0.0f);
}

void WeatherLightning3D::set_channel_width(float p_width) {
	channel_width = MAX(p_width, 0.01f);
}

void WeatherLightning3D::set_min_pixel_width(float p_pixels) {
	min_pixel_width = MAX(p_pixels, 0.0f);
}

void WeatherLightning3D::set_channel_energy(float p_energy) {
	channel_energy = MAX(p_energy, 0.0f);
}

void WeatherLightning3D::set_channel_color(const Color &p_color) {
	channel_color = p_color;
}

void WeatherLightning3D::set_sky_flash_energy(float p_energy) {
	sky_flash_energy = MAX(p_energy, 0.0f);
}

void WeatherLightning3D::set_raster_light_scale(float p_scale) {
	raster_light_scale = MAX(p_scale, 0.0f);
}

void WeatherLightning3D::set_raster_light_shadows(bool p_enabled) {
	raster_light_shadows = p_enabled;
}

void WeatherLightning3D::set_thunder_enabled(bool p_enabled) {
	thunder_enabled = p_enabled;
}

void WeatherLightning3D::set_thunder_volume_db(float p_db) {
	thunder_volume_db = p_db;
}

void WeatherLightning3D::set_thunder_bus(const StringName &p_bus) {
	thunder_bus = p_bus;
}

void WeatherLightning3D::set_seed(int p_seed) {
	seed = uint32_t(p_seed);
	rng.seed(seed);
}

void WeatherLightning3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &WeatherLightning3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &WeatherLightning3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_rate_scale", "scale"), &WeatherLightning3D::set_rate_scale);
	ClassDB::bind_method(D_METHOD("get_rate_scale"), &WeatherLightning3D::get_rate_scale);
	ClassDB::bind_method(D_METHOD("set_marine", "marine"), &WeatherLightning3D::set_marine);
	ClassDB::bind_method(D_METHOD("is_marine"), &WeatherLightning3D::is_marine);
	ClassDB::bind_method(D_METHOD("set_strike_range", "range"), &WeatherLightning3D::set_strike_range);
	ClassDB::bind_method(D_METHOD("get_strike_range"), &WeatherLightning3D::get_strike_range);
	ClassDB::bind_method(D_METHOD("set_channel_width", "width"), &WeatherLightning3D::set_channel_width);
	ClassDB::bind_method(D_METHOD("get_channel_width"), &WeatherLightning3D::get_channel_width);
	ClassDB::bind_method(D_METHOD("set_min_pixel_width", "pixels"), &WeatherLightning3D::set_min_pixel_width);
	ClassDB::bind_method(D_METHOD("get_min_pixel_width"), &WeatherLightning3D::get_min_pixel_width);
	ClassDB::bind_method(D_METHOD("set_channel_energy", "energy"), &WeatherLightning3D::set_channel_energy);
	ClassDB::bind_method(D_METHOD("get_channel_energy"), &WeatherLightning3D::get_channel_energy);
	ClassDB::bind_method(D_METHOD("set_channel_color", "color"), &WeatherLightning3D::set_channel_color);
	ClassDB::bind_method(D_METHOD("get_channel_color"), &WeatherLightning3D::get_channel_color);
	ClassDB::bind_method(D_METHOD("set_sky_flash_energy", "energy"), &WeatherLightning3D::set_sky_flash_energy);
	ClassDB::bind_method(D_METHOD("get_sky_flash_energy"), &WeatherLightning3D::get_sky_flash_energy);
	ClassDB::bind_method(D_METHOD("set_raster_light_scale", "scale"), &WeatherLightning3D::set_raster_light_scale);
	ClassDB::bind_method(D_METHOD("get_raster_light_scale"), &WeatherLightning3D::get_raster_light_scale);
	ClassDB::bind_method(D_METHOD("set_raster_light_shadows", "enabled"), &WeatherLightning3D::set_raster_light_shadows);
	ClassDB::bind_method(D_METHOD("get_raster_light_shadows"), &WeatherLightning3D::get_raster_light_shadows);
	ClassDB::bind_method(D_METHOD("set_thunder_enabled", "enabled"), &WeatherLightning3D::set_thunder_enabled);
	ClassDB::bind_method(D_METHOD("is_thunder_enabled"), &WeatherLightning3D::is_thunder_enabled);
	ClassDB::bind_method(D_METHOD("set_thunder_volume_db", "db"), &WeatherLightning3D::set_thunder_volume_db);
	ClassDB::bind_method(D_METHOD("get_thunder_volume_db"), &WeatherLightning3D::get_thunder_volume_db);
	ClassDB::bind_method(D_METHOD("set_thunder_bus", "bus"), &WeatherLightning3D::set_thunder_bus);
	ClassDB::bind_method(D_METHOD("get_thunder_bus"), &WeatherLightning3D::get_thunder_bus);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &WeatherLightning3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &WeatherLightning3D::get_seed);

	ClassDB::bind_method(D_METHOD("get_flash_rate", "cloud_top"), &WeatherLightning3D::get_flash_rate);
	ClassDB::bind_static_method("WeatherLightning3D", D_METHOD("ground_flash_fraction", "cloud_top", "freezing_level"), &WeatherLightning3D::ground_flash_fraction);
	ClassDB::bind_method(D_METHOD("strike", "position", "cloud_to_ground"), &WeatherLightning3D::strike, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_active_flash_count"), &WeatherLightning3D::get_active_flash_count);
	ClassDB::bind_method(D_METHOD("get_strike_count"), &WeatherLightning3D::get_strike_count);
	ClassDB::bind_method(D_METHOD("get_ground_strike_count"), &WeatherLightning3D::get_ground_strike_count);
	ClassDB::bind_method(D_METHOD("get_last_channel"), &WeatherLightning3D::get_last_channel);
	ClassDB::bind_method(D_METHOD("get_flash_brightness"), &WeatherLightning3D::get_flash_brightness);
	ClassDB::bind_method(D_METHOD("get_last_thunder_delay"), &WeatherLightning3D::get_last_thunder_delay);
	ClassDB::bind_method(D_METHOD("get_last_thunder_length"), &WeatherLightning3D::get_last_thunder_length);
	ClassDB::bind_method(D_METHOD("get_last_thunder_build_time"), &WeatherLightning3D::get_last_thunder_build_time);
	ClassDB::bind_method(D_METHOD("make_thunder", "channel", "listener", "temperature"), &WeatherLightning3D::make_thunder, DEFVAL(15.0f));

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rate_scale", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater"), "set_rate_scale", "get_rate_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "marine"), "set_marine", "is_marine");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strike_range", PROPERTY_HINT_RANGE, "0,60000,10,or_greater,suffix:m"), "set_strike_range", "get_strike_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_GROUP("Channel", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "channel_width", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:m"), "set_channel_width", "get_channel_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_pixel_width", PROPERTY_HINT_RANGE, "0,8,0.1"), "set_min_pixel_width", "get_min_pixel_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "channel_energy", PROPERTY_HINT_RANGE, "0,100000,1,or_greater"), "set_channel_energy", "get_channel_energy");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "channel_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_channel_color", "get_channel_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_flash_energy", PROPERTY_HINT_RANGE, "0,1e10,1,or_greater"), "set_sky_flash_energy", "get_sky_flash_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "raster_light_scale", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_raster_light_scale", "get_raster_light_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "raster_light_shadows"), "set_raster_light_shadows", "get_raster_light_shadows");
	ADD_GROUP("Thunder", "thunder_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "thunder_enabled"), "set_thunder_enabled", "is_thunder_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "thunder_volume_db", PROPERTY_HINT_RANGE, "-80,24,0.1,suffix:dB"), "set_thunder_volume_db", "get_thunder_volume_db");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "thunder_bus"), "set_thunder_bus", "get_thunder_bus");

	ADD_SIGNAL(MethodInfo("struck", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::BOOL, "cloud_to_ground"), PropertyInfo(Variant::FLOAT, "distance")));
}

WeatherLightning3D::WeatherLightning3D() {
	rng.seed(seed);
}

WeatherLightning3D::~WeatherLightning3D() {
	for (PendingThunder &t : thunders) {
		if (t.task != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(t.task);
		}
		memdelete(t.job);
	}
	thunders.clear();
}

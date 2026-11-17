/**************************************************************************/
/*  water_fall_sound.cpp                                                  */
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

#include "water_fall_sound.h"

#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"

namespace WaterFallSound {

float sound_power(float p_impact_speed, float p_flow) {
	return 3e-7f * 0.5f * 1000.0f * MAX(p_flow, 0.0f) * p_impact_speed * p_impact_speed;
}

// A bubble's radius (m) from the plume's size spectrum: N(r) ~ r^-1.5 below the Hinze scale (1 mm), r^-3.3 above
// (Deane and Stokes 2002), from 0.25 mm to r_max; inverse transform sampling per branch.
static float sample_radius(RandomPCG &p_rng, float p_r_max) {
	const float r0 = 0.00025f;
	const float rh = 0.001f;
	auto integral = [](float a, float b, float k) { return (Math::pow(b, 1.0f - k) - Math::pow(a, 1.0f - k)) / (1.0f - k); };
	// The two branches meet at rh: the upper one is scaled to match there.
	const float w_low = integral(r0, rh, 1.5f) * Math::pow(rh, 1.5f);
	const float w_high = p_r_max > rh ? integral(rh, p_r_max, 3.3f) * Math::pow(rh, 3.3f) : 0.0f;
	const float u = p_rng.randf();
	const float v = p_rng.randf();
	auto invert = [](float a, float b, float k, float t) {
		const float pa = Math::pow(a, 1.0f - k);
		const float pb = Math::pow(b, 1.0f - k);
		return Math::pow(pa + t * (pb - pa), 1.0f / (1.0f - k));
	};
	if (u * (w_low + w_high) < w_low) {
		return invert(r0, rh, 1.5f, v);
	}
	return invert(rh, p_r_max, 3.3f, v);
}

Ref<AudioStreamWAV> make(float p_impact_speed, float p_flow, uint32_t p_seed, float p_seconds, int p_mix_rate) {
	const int n = MAX(int(p_seconds * p_mix_rate), 1024);
	Vector<float> buf;
	buf.resize(n);
	buf.fill(0.0f);
	float *s = buf.ptrw();
	RandomPCG rng(p_seed + 1);
	// Larger, faster jets carry larger cavities down: the biggest bubbles grow with the impact speed (a 16 m/s
	// plunge rings down to about 130 Hz, a 3 m/s cascade stays above 1 kHz).
	const float r_max = CLAMP(0.0015f * p_impact_speed, 0.003f, 0.03f);
	// Entrainment: air drawn in at about a tenth of the water's flow for a plunging jet (Bin 1993), as bubbles of
	// about 1 mm, far more than can be rung one by one. Up to 20000 a second are: past that the rings overlap into
	// the same steady roar of the same spectrum, and the level is set by the player from the sound power.
	const float real_rate = 0.1f * MAX(p_flow, 1e-3f) / (4.0f / 3.0f * Math::PI * 1e-9f);
	const float rate = MIN(real_rate, 20000.0f);
	const int count = MAX(int(rate * p_seconds), 1);
	const float inv_rate = 1.0f / float(p_mix_rate);
	for (int b = 0; b < count; b++) {
		const float r = sample_radius(rng, r_max);
		const float f0 = 3.26f / r;
		if (f0 > 0.45f * p_mix_rate) {
			continue;
		}
		// Damping (1/s): viscous, thermal and radiation losses (van den Doel 2005).
		const float d = 0.13f / r + 0.0072f * Math::pow(r, -1.5f);
		// Amplitude: a pulsating bubble radiates p ~ rho r^3 omega^2 eps / R, and with omega ~ 1/r and the same
		// starting excursion eps for every size, p ~ r (van den Doel's a ~ r^alpha with alpha 1). With the plume's
		// sizes this gives a spectrum falling about 6 dB an octave above its peak, as surf noise does (Knudsen).
		const float a = r / 0.001f;
		const int start = int(rng.randf() * n);
		const int len = MIN(int(5.0f / d * p_mix_rate), n);
		float phase = 0.0f;
		for (int i = 0; i < len; i++) {
			const float t = i * inv_rate;
			// The pitch rises as the bubble nears the surface (xi 0.1).
			const float f = f0 * (1.0f + 0.1f * d * t);
			phase += Math::TAU * f * inv_rate;
			s[(start + i) % n] += a * Math::exp(-d * t) * Math::sin(phase);
		}
	}
	// High-pass at 20 Hz against any offset; normalise to -3 dBFS peak (the level is the player's).
	// The filter runs over the loop once to settle, so its state at the start is the one the end leaves: no seam.
	float peak = 0.0f;
	float y = 0.0f;
	float x_prev = s[n - 1];
	const float k = Math::exp(-Math::TAU * 20.0f / p_mix_rate);
	for (int i = 0; i < n; i++) {
		y = k * (y + s[i] - x_prev);
		x_prev = s[i];
	}
	for (int i = 0; i < n; i++) {
		const float x = s[i];
		y = k * (y + x - x_prev);
		x_prev = x;
		s[i] = y;
		peak = MAX(peak, Math::abs(y));
	}
	const float g = peak > 0.0f ? 0.708f / peak : 0.0f;
	PackedByteArray data;
	data.resize(n * 2);
	uint8_t *w = data.ptrw();
	for (int i = 0; i < n; i++) {
		const int16_t v = int16_t(CLAMP(int(s[i] * g * 32767.0f), -32768, 32767));
		w[i * 2 + 0] = uint8_t(v & 0xff);
		w[i * 2 + 1] = uint8_t((v >> 8) & 0xff);
	}
	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_mix_rate(p_mix_rate);
	wav->set_stereo(false);
	wav->set_data(data);
	wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
	wav->set_loop_begin(0);
	wav->set_loop_end(n);
	return wav;
}

} // namespace WaterFallSound

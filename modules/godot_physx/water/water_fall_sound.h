/**************************************************************************/
/*  water_fall_sound.h                                                    */
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

#include "scene/resources/audio_stream_wav.h"

// The roar of a waterfall's plunge, synthesised from the bubbles it entrains (docs/ideas/water.md,
// "Waterfall stage 3b: sound"). Each bubble rings at Minnaert's frequency, f r = 3.26 m/s, damped and
// rising as it reaches the surface (van den Doel 2005, "Physically based models for liquid sounds"), with
// radii by Deane and Stokes' (2002) breaking-wave plume spectrum. A seamless loop.
namespace WaterFallSound {
// p_impact_speed: m/s where the jet meets the pool; p_flow: m^3/s; p_seconds: the loop's length.
Ref<AudioStreamWAV> make(float p_impact_speed, float p_flow, uint32_t p_seed, float p_seconds = 6.0f, int p_mix_rate = 44100);
// The sound power (W) of a plunge: a fixed share (3e-7) of the falling water's kinetic power, rho Q v^2 / 2,
// which puts a 6 m fall of 1 m^3/s near 75 dB at 10 m (Galbrun and Ali's water-feature measurements).
float sound_power(float p_impact_speed, float p_flow);
} // namespace WaterFallSound

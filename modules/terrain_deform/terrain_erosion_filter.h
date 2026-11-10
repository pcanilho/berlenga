/**************************************************************************/
/*  terrain_erosion_filter.h                                              */
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

// Erosion filter: gullies and ridges carved along the downhill direction of a height function, evaluated at each
// point on its own (no simulation), so tiles generated apart join exactly. The technique of Clay John ("Eroded
// Terrain Noise", 2018) and Fewes ("Terrain Erosion Noise", 2023), with the refinements described by
// R. S. Johansen, "Fast and Gorgeous Erosion Filter" (2026): the fade approach (fade gullies towards a target that
// runs from -1 in valleys to +1 on peaks where the slope vanishes), stacked fading (each octave's ridges and creases
// mask the next), partially normalized gullies, straight gullies (the slope that steers the next octave uses the
// sign of the gully slope), and a gully weight for pointed peaks. Written here from that description, not from
// the published (MPL) code.
//
// Per octave: a grid of cells one wavelength wide, each with a hashed pivot; stripes through each pivot run
// downhill (their phase varies across the slope), cos for height and sin for slope; the neighbouring cells are
// blended with a smooth distance weight (the 5x5 cells around the point), and the blended (cos, sin) pair is renormalized when longer than 1/2.

#include <cmath>
#include <cstdint>

class TerrainErosionFilter {
public:
	struct Params {
		uint32_t seed = 0;
		double wavelength = 64.0; // metres, first (largest) octave
		int octaves = 5;
		double lacunarity = 2.0; // wavelength divisor per octave
		double depth = 0.1; // gully depth as a fraction of its wavelength
		double gully_weight = 0.5; // < 1 keeps peaks pointed (depth is divided by it)
		double detail = 1.5; // how far smaller octaves reach onto gentle ground (higher = more)
		double assumed_slope = 0.6; // slope the input is treated as having, for steering the first octave
		double slope_fade = 0.5; // input slopes below this fade the effect (flat peaks, valley floors)
		double valley_height = 0.0; // heights mapped to the fade target -1 ...
		double peak_height = 1000.0; // ... and +1
		double crest_keep = 0.4; // share of the gullies kept where coherence is 0 (crests, valley floors)
	};

	// Returns the height offset to add at world (x, z), given the input height and its gradient there. coherence
	// (0..1) says how well the gradient keeps one direction around the point (1 on an even slope, 0 where opposite
	// slopes meet on a crest or a valley floor); the effect fades with it to crest_keep, so gullies only lightly
	// notch a crest instead of cutting it into a regular comb.
	static double offset(const Params &p, double x, double z, double h, double gx, double gz, double coherence = 1.0) {
		const double slope = std::sqrt(gx * gx + gz * gz);
		if (slope < 1e-9 || p.octaves <= 0) {
			return 0.0;
		}
		// Steering slope: the input's direction at an assumed constant steepness.
		double sx = gx / slope * p.assumed_slope;
		double sz = gz / slope * p.assumed_slope;
		double fade = _clamp((h - p.valley_height) / (p.peak_height - p.valley_height + 1e-9), 0.0, 1.0) * 2.0 - 1.0;
		double mask = _ease_out(slope / p.slope_fade) * (p.crest_keep + (1.0 - p.crest_keep) * _smooth(coherence));
		double total = 0.0;
		double lambda = p.wavelength;
		const double gw = p.gully_weight > 1e-3 ? p.gully_weight : 1e-3;
		for (int o = 0; o < p.octaves; o++) {
			const double sl = std::sqrt(sx * sx + sz * sz);
			if (sl < 1e-9) {
				break;
			}
			// Across-slope unit vector: stripes vary along it and run down the slope.
			const double tx = -sz / sl;
			const double tz = sx / sl;
			double c = 0.0;
			double s = 0.0;
			double wsum = 0.0;
			const double fx = x / lambda;
			const double fz = z / lambda;
			const int ix = (int)std::floor(fx);
			const int iz = (int)std::floor(fz);
			// Pivots within 1.5 cells: cells -2..2 around the point hold every one of them (a pivot two cells out
			// is at least 2 cells away), so no pivot drops in or out across a cell border.
			for (int j = -2; j <= 2; j++) {
				for (int i = -2; i <= 2; i++) {
					const int cx = ix + i;
					const int cz = iz + j;
					const uint32_t hsh = _hash(uint32_t(cx), uint32_t(cz), p.seed + uint32_t(o) * 0x9E3779B9u);
					const double px = cx + (hsh & 0xffff) / 65536.0;
					const double pz = cz + (hsh >> 16) / 65536.0;
					const double dx = fx - px;
					const double dz = fz - pz;
					const double d2 = dx * dx + dz * dz;
					// Smooth compact weight, zero beyond 1.5 cells.
					double w = 1.0 - d2 / 2.25;
					if (w <= 0.0) {
						continue;
					}
					w = w * w * w;
					const double phase = (dx * tx + dz * tz) * 6.283185307179586;
					c += w * std::cos(phase);
					s += w * std::sin(phase);
					wsum += w;
				}
			}
			if (wsum <= 0.0) {
				break;
			}
			c /= wsum;
			s /= wsum;
			// Partial normalization: lengths above 1/2 become 1, shorter ones are doubled.
			const double len = std::sqrt(c * c + s * s);
			const double k = len > 0.5 ? 1.0 / len : 2.0;
			c *= k;
			s *= k;

			const double raw = c * gw; // gully height, -gw..gw (ridge at +)
			const double faded = fade + (raw - fade) * mask; // fade towards the target where the mask is 0
			total += faded * p.depth / gw * lambda;

			// Stack: this octave's result is the next fade target; its ridges and creases (slope ~ 0) mask the next.
			fade = faded;
			mask = (1.0 - std::pow(1.0 - _clamp(mask, 0.0, 1.0), p.detail)) * _ease_out(std::fabs(s));
			// Straight gullies: steer the next octave by the sign of this octave's slope, at constant steepness.
			const double gs = (s >= 0.0 ? 1.0 : -1.0) * p.depth * 6.283185307179586;
			sx += -gs * tx;
			sz += -gs * tz;
			lambda /= p.lacunarity;
		}
		return total;
	}

	// Ridged gradient-noise fBm (Musgrave's ridged multifractal family, Ebert et al. 2003): sum of (1/2 - |n|) over
	// octaves, so it has sharp maxima (pinnacles) and rounded minima (cols), -1/2..1/2 per octave weight. Used on
	// crests, where the gullies fade, so a crest keeps an irregular, jagged line instead of a straight one.
	static double crest_noise(uint32_t p_seed, double x, double z, double p_wavelength, int p_octaves) {
		double sum = 0.0;
		double amp = 1.0;
		double norm = 0.0;
		double f = 1.0 / p_wavelength;
		for (int o = 0; o < p_octaves; o++) {
			sum += amp * (0.5 - std::fabs(_gradient_noise(p_seed + uint32_t(o) * 0x632BE5ABu, x * f, z * f)));
			norm += amp;
			amp *= 0.5;
			f *= 2.0;
		}
		return norm > 0.0 ? sum / norm : 0.0;
	}

private:
	// Perlin gradient noise, about -0.7..0.7.
	static double _gradient_noise(uint32_t p_seed, double x, double z) {
		const double fx = std::floor(x);
		const double fz = std::floor(z);
		const int ix = int(fx);
		const int iz = int(fz);
		const double tx = x - fx;
		const double tz = z - fz;
		auto grad = [&](int cx, int cz, double dx, double dz) -> double {
			const uint32_t h = _hash(uint32_t(cx), uint32_t(cz), p_seed);
			const double a = (h & 0xffff) * (6.283185307179586 / 65536.0);
			return std::cos(a) * dx + std::sin(a) * dz;
		};
		const double ux = tx * tx * tx * (tx * (tx * 6.0 - 15.0) + 10.0);
		const double uz = tz * tz * tz * (tz * (tz * 6.0 - 15.0) + 10.0);
		const double n00 = grad(ix, iz, tx, tz);
		const double n10 = grad(ix + 1, iz, tx - 1.0, tz);
		const double n01 = grad(ix, iz + 1, tx, tz - 1.0);
		const double n11 = grad(ix + 1, iz + 1, tx - 1.0, tz - 1.0);
		const double a = n00 + (n10 - n00) * ux;
		const double b = n01 + (n11 - n01) * ux;
		return a + (b - a) * uz;
	}
	static double _clamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
	static double _ease_out(double t) {
		const double v = 1.0 - _clamp(t, 0.0, 1.0);
		return 1.0 - v * v;
	}
	static double _smooth(double t) {
		const double v = _clamp(t, 0.0, 1.0);
		return v * v * (3.0 - 2.0 * v);
	}
	static uint32_t _hash(uint32_t x, uint32_t z, uint32_t seed) {
		uint32_t h = x * 0x8DA6B343u ^ z * 0xD8163841u ^ seed * 0xCB1AB31Fu;
		h ^= h >> 16;
		h *= 0x7FEB352Du;
		h ^= h >> 15;
		h *= 0x846CA68Bu;
		h ^= h >> 16;
		return h;
	}
};

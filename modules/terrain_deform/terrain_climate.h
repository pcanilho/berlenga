/**************************************************************************/
/*  terrain_climate.h                                                     */
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

// Climate maps from a height grid (docs/ideas/terrain-generation.md, "Climate maps"). Plain C++ on
// row-major square grids, x east (+X), z south (+Z), so it runs on any height source.
//
// - Temperature: a seasonal harmonic between January and July means at a reference height, minus a
//   lapse rate that itself moves between a winter and a summer value (Rolland 2003).
// - Precipitation: the linear theory of orographic precipitation (Smith and Barstad 2004), summed over
//   a wind rose of wet-day winds.
// - Snow fraction: daily temperatures spread normally around each month's mean, snow below the
//   rain-snow threshold (Jennings et al. 2018).
// - Aridity index (UNEP 1997): precipitation over Thornthwaite (1948) potential evapotranspiration,
//   Willmott et al. (1985) above 26.5 C.
// - Shortwave: sun path (Cooper 1969), horizons in 16 directions and the sky view factor (Dozier and
//   Frew 1990), Kasten and Young (1989) air mass, Meinel clear-sky beam, clouds and the diffuse share
//   by Kasten and Czeplak (1980), ground reflection.
// - Winstral Sx (Winstral, Elder and Davis 2002) for 16 wind directions.

#include "core/math/math_funcs.h"
#include "core/object/worker_thread_pool.h"
#include "core/string/string_name.h"

#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace TerrainClimate {

constexpr int DIRECTIONS = 16; // every 22.5 degrees, clockwise from north (-Z)
constexpr float DIRECTION_STEP = 22.5f;

enum Field {
	TEMPERATURE_MEAN,
	TEMPERATURE_JANUARY,
	TEMPERATURE_JULY,
	PRECIPITATION,
	SNOW_FRACTION,
	ARIDITY,
	SHORTWAVE,
	SHORTWAVE_WINTER,
	SKY_VIEW,
	FIELD_MAX,
};

struct Params {
	float latitude = 46.0f; // degrees
	float reference_height = 0.0f; // m, where the temperatures below hold
	float temperature_january = 3.0f; // C
	float temperature_july = 22.0f; // C
	float lapse_rate_january = 4.5f; // K/km (Rolland 2003, Alps winter)
	float lapse_rate_july = 6.5f; // K/km (summer)
	float precipitation = 900.0f; // mm a year on flat ground
	float wet_fraction = 0.12f; // share of the year's hours with precipitation
	float wind_speed = 10.0f; // m/s, wet-day wind
	float wind_from = 270.0f; // degrees clockwise from north
	float wind_spread = 45.0f; // degrees, standard deviation of wet-day wind directions
	float moist_stability = 0.005f; // 1/s, moist buoyancy frequency
	float conversion_time = 1000.0f; // s
	float fallout_time = 1000.0f; // s
	float precipitation_floor = 0.4f; // share of the background left in the deepest rain shadow
	float rain_snow_threshold = 1.0f; // C
	float daily_temperature_spread = 3.0f; // K
	float cloud_cover = 0.5f; // 0 to 1
	float ground_albedo = 0.2f;
	float horizon_distance = 5000.0f; // m
	float sx_distance = 100.0f; // m
	float precipitation_seasonality = 0.0f; // 0 even through the year; 1 none at the driest month
	float precipitation_peak_month = 0.0f; // month of the most precipitation, 0 January .. 11 December
	float melt_factor = 1.8f; // mm/day/K, snowmelt per positive degree day without sun (Hock 1999)
	float radiation_melt_factor = 0.015f; // mm/day/K per W/m2 of shortwave (Hock 1999)
	float soil_capacity = 150.0f; // mm of plant-available water (McCabe and Markstrom 2007)
	float direct_runoff = 0.05f; // share of rain running off at once
	float groundwater_release = 0.5f; // share of stored surplus leaving each month (linear reservoir)
};

inline void parallel_for(int p_count, const std::function<void(int)> &p_fn) {
	if (p_count <= 0) {
		return;
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (!pool || p_count < 4) {
		for (int i = 0; i < p_count; i++) {
			p_fn(i);
		}
		return;
	}
	struct Job {
		const std::function<void(int)> *fn;
		static void run(void *p_ud, uint32_t p_i) { (*static_cast<Job *>(p_ud)->fn)(int(p_i)); }
	} job{ &p_fn };
	WorkerThreadPool::GroupID g = pool->add_native_group_task(&Job::run, &job, p_count, -1, true, SNAME("TerrainClimate"));
	pool->wait_for_group_task_completion(g);
}

// Month m (0 January to 11 December) on a harmonic through January and July.
inline float month_value(float p_january, float p_july, int p_month) {
	return 0.5f * (p_january + p_july) - 0.5f * (p_july - p_january) * Math::cos(float(Math::TAU) * p_month / 12.0f);
}

// Share of the year's precipitation falling in month m: a harmonic peaking at precipitation_peak_month.
inline float precipitation_share(const Params &p, int p_month) {
	const float a = CLAMP(p.precipitation_seasonality, 0.0f, 1.0f);
	return (1.0f + a * Math::cos(float(Math::TAU) * (p_month - p.precipitation_peak_month) / 12.0f)) / 12.0f;
}

inline float temperature(const Params &p, float p_height, int p_month) {
	const float t = month_value(p.temperature_january, p.temperature_july, p_month);
	const float lapse = month_value(p.lapse_rate_january, p.lapse_rate_july, p_month);
	return t - lapse * (p_height - p.reference_height) * 0.001f;
}

// Solar declination, radians (Cooper 1969).
inline double declination(double p_day) {
	return Math::deg_to_rad(23.45) * Math::sin(Math::TAU * (284.0 + p_day) / 365.0);
}

// Day length in hours at a latitude (radians) and declination.
inline double day_length(double p_lat, double p_decl) {
	const double c = CLAMP(-Math::tan(p_lat) * Math::tan(p_decl), -1.0, 1.0);
	return 24.0 * Math::acos(c) / Math::PI;
}

/* Thornthwaite (1948) potential evapotranspiration, mm a year, from twelve monthly means; each month's into
r_monthly when given. */
inline float thornthwaite_pet(const float *p_t, float p_latitude, float *r_monthly = nullptr) {
	static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	static const int MID[12] = { 15, 46, 74, 105, 135, 166, 196, 227, 258, 288, 319, 349 };
	if (r_monthly) {
		for (int m = 0; m < 12; m++) {
			r_monthly[m] = 0.0f;
		}
	}
	double heat = 0.0;
	for (int m = 0; m < 12; m++) {
		if (p_t[m] > 0.0f) {
			heat += Math::pow(double(p_t[m]) / 5.0, 1.514);
		}
	}
	if (heat <= 0.0) {
		return 0.0f;
	}
	const double a = 6.75e-7 * heat * heat * heat - 7.71e-5 * heat * heat + 1.792e-2 * heat + 0.49239;
	const double lat = Math::deg_to_rad(double(p_latitude));
	double pet = 0.0;
	for (int m = 0; m < 12; m++) {
		const double t = p_t[m];
		if (t <= 0.0) {
			continue;
		}
		const double scale = (day_length(lat, declination(MID[m])) / 12.0) * (DAYS[m] / 30.0);
		double e;
		if (t < 26.5) {
			e = 16.0 * Math::pow(10.0 * t / heat, a);
		} else {
			e = -415.85 + 32.24 * t - 0.43 * t * t; // Willmott, Rowe and Mintz 1985
		}
		pet += MAX(e, 0.0) * scale;
		if (r_monthly) {
			r_monthly[m] = float(MAX(e, 0.0) * scale);
		}
	}
	return float(pet);
}

/* FFT: iterative radix-2, in place, size a power of two. */
inline void fft(std::complex<double> *p_a, int p_n, int p_stride, bool p_inverse) {
	for (int i = 1, j = 0; i < p_n; i++) {
		int bit = p_n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(p_a[i * p_stride], p_a[j * p_stride]);
		}
	}
	for (int len = 2; len <= p_n; len <<= 1) {
		const double ang = Math::TAU / len * (p_inverse ? 1.0 : -1.0);
		const std::complex<double> wl(Math::cos(ang), Math::sin(ang));
		for (int i = 0; i < p_n; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (int k = 0; k < len / 2; k++) {
				const std::complex<double> u = p_a[(i + k) * p_stride];
				const std::complex<double> v = p_a[(i + k + len / 2) * p_stride] * w;
				p_a[(i + k) * p_stride] = u + v;
				p_a[(i + k + len / 2) * p_stride] = u - v;
				w *= wl;
			}
		}
	}
}

inline void fft2(std::vector<std::complex<double>> &r_a, int p_n, bool p_inverse) {
	parallel_for(p_n, [&](int z) { fft(r_a.data() + size_t(z) * p_n, p_n, 1, p_inverse); });
	parallel_for(p_n, [&](int x) { fft(r_a.data() + x, p_n, p_n, p_inverse); });
	if (p_inverse) {
		const double s = 1.0 / (double(p_n) * p_n);
		for (std::complex<double> &c : r_a) {
			c *= s;
		}
	}
}

// Wet-day wind rose: weight of each of the 16 directions, Gaussian around wind_from.
inline void wind_rose(const Params &p, float *r_w) {
	float sum = 0.0f;
	for (int d = 0; d < DIRECTIONS; d++) {
		float delta = Math::fmod(d * DIRECTION_STEP - p.wind_from + 540.0f, 360.0f) - 180.0f;
		const float s = MAX(p.wind_spread, 1.0f);
		r_w[d] = p.wind_spread >= 360.0f ? 1.0f : Math::exp(-0.5f * delta * delta / (s * s));
		sum += r_w[d];
	}
	for (int d = 0; d < DIRECTIONS; d++) {
		r_w[d] /= sum;
	}
}

/* Smith and Barstad (2004) precipitation, mm a year, on an n x n grid of surface heights with cell p_cell (m).
   The terrain is padded to twice its size: past each edge it eases to the lowest height over half the pad
   (cosine), so the map edge is no cliff and the wrap-around is far away. */
inline void precipitation(const Params &p, const float *p_h, int p_n, float p_cell, std::vector<float> &r_p) {
	const int n = p_n;
	int N = 1;
	while (N < 2 * n) {
		N <<= 1;
	}
	float base = p_h[0];
	for (int i = 0; i < n * n; i++) {
		base = MIN(base, p_h[i]);
	}
	const int half_pad = MAX((N - n) / 2, 1);
	auto outside = [&](int i, int &r_src) -> float {
		if (i < n) {
			r_src = i;
			return 0.0f;
		}
		const int right = i - (n - 1);
		const int left = N - i;
		r_src = right <= left ? n - 1 : 0;
		return float(MIN(right, left));
	};
	auto taper = [&](float d) -> float {
		return d >= half_pad ? 0.0f : 0.5f * (1.0f + Math::cos(float(Math::PI) * d / half_pad));
	};
	std::vector<std::complex<double>> hh(size_t(N) * N);
	for (int z = 0; z < N; z++) {
		int sz;
		const float wz = taper(outside(z, sz));
		for (int x = 0; x < N; x++) {
			int sx;
			const float wx = taper(outside(x, sx));
			hh[size_t(z) * N + x] = double((p_h[sz * n + sx] - base) * wx * wz);
		}
	}
	fft2(hh, N, false);

	// Thermodynamics at the reference temperature (annual mean at the reference height).
	const double t_c = 0.5 * (p.temperature_january + p.temperature_july);
	const double t_k = t_c + 273.15;
	const double gamma = MAX(0.5 * (p.lapse_rate_january + p.lapse_rate_july), 1.0) * 1.0e-3; // K/m
	const double es = 611.2 * Math::exp(17.67 * t_c / (t_c + 243.5)); // Pa (Bolton 1980)
	const double rv = 461.5, rd = 287.04, lv = 2.5e6, cp = 1004.0, g = 9.81, eps = 0.622;
	const double rho_s = es / (rv * t_k); // saturated vapour density at the surface, kg/m3
	const double rs = eps * es / (101325.0 - es);
	const double gamma_m = g * (1.0 + lv * rs / (rd * t_k)) / (cp + lv * lv * rs * eps / (rd * t_k * t_k)); // moist adiabat, K/m
	const double cw = rho_s * gamma_m / gamma; // uplift sensitivity, kg/m3
	const double hw = rv * t_k * t_k / (lv * gamma); // water vapour scale height, m
	const double f = 2.0 * 7.2921e-5 * Math::sin(Math::deg_to_rad(double(p.latitude)));
	const double nm2 = double(p.moist_stability) * p.moist_stability;
	const double tau_c = p.conversion_time, tau_f = p.fallout_time;

	const double wet_hours = 8760.0 * CLAMP(double(p.wet_fraction), 0.001, 1.0);
	const double background = p.precipitation / wet_hours; // mm/h on wet hours
	// The linear model only knows stratiform rain from flow over the terrain; showers and fronts still reach
	// the lee, so no event drops below this share of the background (the precip_min of fastscape's version).
	const double floor_rate = background * CLAMP(double(p.precipitation_floor), 0.0, 1.0);
	float rose[DIRECTIONS];
	wind_rose(p, rose);

	r_p.assign(size_t(n) * n, 0.0f);
	std::vector<std::complex<double>> pp(size_t(N) * N);
	const double dk = Math::TAU / (double(N) * p_cell);
	for (int d = 0; d < DIRECTIONS; d++) {
		if (rose[d] < 0.005f) {
			continue;
		}
		const double from = Math::deg_to_rad(d * double(DIRECTION_STEP));
		// Coming from bearing b it blows toward b + 180: x east, z south.
		const double u = -Math::sin(from) * p.wind_speed;
		const double v = Math::cos(from) * p.wind_speed;
		parallel_for(N, [&](int j) {
			const double kz = (j < N / 2 ? j : j - N) * dk;
			for (int i = 0; i < N; i++) {
				const double kx = (i < N / 2 ? i : i - N) * dk;
				const double sigma = u * kx + v * kz;
				const size_t idx = size_t(j) * N + i;
				if (Math::abs(sigma) < 1e-12) {
					pp[idx] = 0.0;
					continue;
				}
				double den = sigma * sigma - f * f;
				if (Math::abs(den) < 1e-18) {
					den = den < 0.0 ? -1e-18 : 1e-18;
				}
				const double ratio = (nm2 - sigma * sigma) / den * (kx * kx + kz * kz);
				// Vertical wavenumber: propagating (real, sign of sigma) or evanescent (decaying, imaginary).
				const std::complex<double> m = ratio >= 0.0 ? std::complex<double>((sigma > 0.0 ? 1.0 : -1.0) * Math::sqrt(ratio), 0.0) : std::complex<double>(0.0, Math::sqrt(-ratio));
				const std::complex<double> I(0.0, 1.0);
				pp[idx] = cw * I * sigma * hh[idx] / ((1.0 - I * m * hw) * (1.0 + I * sigma * tau_c) * (1.0 + I * sigma * tau_f));
			}
		});
		fft2(pp, N, true);
		const double w = rose[d] * wet_hours;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const double rate = pp[size_t(z) * N + x].real() * 3600.0 + background; // mm/h
				r_p[z * n + x] += float(MAX(rate, floor_rate) * w);
			}
		}
	}
}

/* Precipitation on an n x n grid of surface heights with cell p_cell: solved on a coarser grid of about
   p_precipitation_cell (box means), then bilinear back to the fine one, into r_p (n * n). */
inline void precipitation_on(const Params &p, const float *p_surf, int n, float cell, float p_precipitation_cell, float *r_p) {
	const int f = MAX(1, int(Math::round(p_precipitation_cell / cell)));
	const int nc = (n + f - 1) / f;
	std::vector<float> coarse(nc * nc, 0.0f);
	for (int cz = 0; cz < nc; cz++) {
		for (int cx = 0; cx < nc; cx++) {
			double sum = 0.0;
			int count = 0;
			for (int z = cz * f; z < MIN((cz + 1) * f, n); z++) {
				for (int x = cx * f; x < MIN((cx + 1) * f, n); x++) {
					sum += p_surf[z * n + x];
					count++;
				}
			}
			coarse[cz * nc + cx] = float(sum / MAX(count, 1));
		}
	}
	std::vector<float> coarse_p;
	precipitation(p, coarse.data(), nc, cell * f, coarse_p);
	parallel_for(n, [&](int z) {
		const float gz = CLAMP((z + 0.5f) / f - 0.5f, 0.0f, float(nc - 1));
		const int z0 = MIN(int(gz), nc - 1), z1 = MIN(z0 + 1, nc - 1);
		for (int x = 0; x < n; x++) {
			const float gx = CLAMP((x + 0.5f) / f - 0.5f, 0.0f, float(nc - 1));
			const int x0 = MIN(int(gx), nc - 1), x1 = MIN(x0 + 1, nc - 1);
			const float fx = gx - x0, fz = gz - z0;
			r_p[z * n + x] = Math::lerp(Math::lerp(coarse_p[z0 * nc + x0], coarse_p[z0 * nc + x1], fx), Math::lerp(coarse_p[z1 * nc + x0], coarse_p[z1 * nc + x1], fx), fz);
		}
	});
}

/* Horizons: elevation angle (radians) of the terrain horizon in each of 16 directions, marched out to
   horizon_distance with steps growing 8% each; past the map edge the ground counts as flat. */
inline void horizons(const Params &p, const float *p_h, int p_n, float p_cell, std::vector<float> &r_hz) {
	const int n = p_n;
	r_hz.assign(size_t(n) * n * DIRECTIONS, 0.0f);
	float dx[DIRECTIONS], dz[DIRECTIONS];
	for (int d = 0; d < DIRECTIONS; d++) {
		const float a = Math::deg_to_rad(d * DIRECTION_STEP);
		dx[d] = Math::sin(a);
		dz[d] = -Math::cos(a);
	}
	const float max_r = p.horizon_distance / p_cell;
	parallel_for(n, [&](int z) {
		for (int x = 0; x < n; x++) {
			const float h0 = p_h[z * n + x];
			for (int d = 0; d < DIRECTIONS; d++) {
				float best = 0.0f; // flat beyond
				for (float r = 1.0f; r <= max_r; r += MAX(1.0f, r * 0.08f)) {
					const float gx = x + dx[d] * r, gz = z + dz[d] * r;
					if (gx < 0.0f || gz < 0.0f || gx > n - 1 || gz > n - 1) {
						break;
					}
					const int x0 = MIN(int(gx), n - 2), z0 = MIN(int(gz), n - 2);
					const float fx = gx - x0, fz = gz - z0;
					const float *row0 = p_h + z0 * n, *row1 = row0 + n;
					const float h = Math::lerp(Math::lerp(row0[x0], row0[x0 + 1], fx), Math::lerp(row1[x0], row1[x0 + 1], fx), fz);
					best = MAX(best, (h - h0) / (r * p_cell));
				}
				r_hz[(size_t(z) * n + x) * DIRECTIONS + d] = Math::atan(best);
			}
		}
	});
}

/* Shortwave (mean W/m2 over the year and over the three winter months) and the sky view factor. */
inline void shortwave(const Params &p, const float *p_h, int p_n, float p_cell, const std::vector<float> &p_hz, float *r_year, float *r_winter, float *r_sky) {
	const int n = p_n;
	struct Sun {
		float sx, sy, sz; // unit vector to the sun
		float az; // radians clockwise from north
		float beam; // W/m2 normal to the beam
		float diffuse; // W/m2 on the horizontal
		float global; // W/m2 on the horizontal
		bool winter;
	};
	std::vector<Sun> suns;
	const double lat = Math::deg_to_rad(double(p.latitude));
	const int DAYS = 24;
	const int STEPS = 48;
	const float cloud = CLAMP(p.cloud_cover, 0.0f, 1.0f);
	const float cloud_factor = 1.0f - 0.75f * Math::pow(cloud, 3.4f);
	const float diffuse_share = 0.3f + 0.7f * cloud * cloud;
	int winter_samples = 0;
	for (int k = 0; k < DAYS; k++) {
		const double day = 365.0 * (k + 0.5) / DAYS;
		const bool winter = p.latitude >= 0.0f ? (day >= 335.0 || day < 59.0) : (day >= 152.0 && day < 243.0);
		winter_samples += winter ? STEPS : 0;
		const double decl = declination(day);
		const double i0 = 1361.0 * (1.0 + 0.033 * Math::cos(Math::TAU * day / 365.0));
		for (int s = 0; s < STEPS; s++) {
			const double hour = (s + 0.5) * 24.0 / STEPS;
			const double omega = Math::deg_to_rad(15.0 * (hour - 12.0));
			const double sin_alt = Math::sin(lat) * Math::sin(decl) + Math::cos(lat) * Math::cos(decl) * Math::cos(omega);
			if (sin_alt <= 0.0) {
				continue;
			}
			const double alt = Math::asin(sin_alt);
			const double cos_alt = Math::cos(alt);
			const double az = Math::atan2(-Math::cos(decl) * Math::sin(omega), Math::sin(decl) * Math::cos(lat) - Math::cos(decl) * Math::cos(omega) * Math::sin(lat));
			const double air_mass = 1.0 / (sin_alt + 0.50572 * Math::pow(Math::rad_to_deg(alt) + 6.07995, -1.6364)); // Kasten and Young 1989
			const double clear_beam = i0 * Math::pow(0.7, Math::pow(air_mass, 0.678)); // Meinel
			// Clear-sky global: beam plus a clear-sky diffuse share; clouds by Kasten and Czeplak (1980).
			const double clear_global = clear_beam * sin_alt + i0 * sin_alt * (0.271 - 0.294 * clear_beam / i0);
			const double global = clear_global * cloud_factor;
			const double diffuse = global * MAX(double(diffuse_share), (clear_global - clear_beam * sin_alt) / clear_global);
			Sun sun;
			sun.sx = float(Math::sin(az) * cos_alt);
			sun.sy = float(sin_alt);
			sun.sz = float(-Math::cos(az) * cos_alt);
			sun.az = float(az < 0.0 ? az + Math::TAU : az);
			sun.beam = float((global - diffuse) / sin_alt);
			sun.diffuse = float(diffuse);
			sun.global = float(global);
			sun.winter = winter;
			suns.push_back(sun);
		}
	}
	const float total_samples = float(DAYS * STEPS);
	const float rho = p.ground_albedo;
	const float dphi = float(Math::TAU) / DIRECTIONS;
	parallel_for(n, [&](int z) {
		for (int x = 0; x < n; x++) {
			auto H = [&](int ix, int iz) { return p_h[CLAMP(iz, 0, n - 1) * n + CLAMP(ix, 0, n - 1)]; };
			// Horn (1981) gradient.
			const float a = H(x - 1, z - 1), b = H(x, z - 1), c = H(x + 1, z - 1);
			const float d = H(x - 1, z), f = H(x + 1, z);
			const float g = H(x - 1, z + 1), hh = H(x, z + 1), k = H(x + 1, z + 1);
			const float dhdx = ((c + 2.0f * f + k) - (a + 2.0f * d + g)) / (8.0f * p_cell);
			const float dhdz = ((g + 2.0f * hh + k) - (a + 2.0f * b + c)) / (8.0f * p_cell);
			const float inv = 1.0f / Math::sqrt(dhdx * dhdx + dhdz * dhdz + 1.0f);
			const float nx = -dhdx * inv, ny = inv, nz = -dhdz * inv;
			const float slope = Math::acos(ny);
			const float down_az = Math::atan2(-dhdx, dhdz); // downhill, clockwise from north (-Z)
			const float *hz = p_hz.data() + (size_t(z) * n + x) * DIRECTIONS;
			// Effective horizon: the terrain or the slope's own plane, whichever is higher.
			float eff[DIRECTIONS];
			float view = 0.0f;
			for (int j = 0; j < DIRECTIONS; j++) {
				const float phi = j * dphi;
				const float tx = Math::sin(phi), tz = -Math::cos(phi);
				const float plane = Math::atan(-(nx * tx + nz * tz) / ny);
				eff[j] = MAX(hz[j], plane);
				const float hzen = float(Math::PI) * 0.5f - eff[j]; // horizon zenith angle
				const float sh = Math::sin(hzen), ch = Math::cos(hzen);
				view += Math::cos(slope) * sh * sh + Math::sin(slope) * Math::cos(phi - down_az) * (hzen - sh * ch);
			}
			view = CLAMP(view / DIRECTIONS, 0.0f, 1.0f);
			double year = 0.0, winter = 0.0;
			for (const Sun &s : suns) {
				float e = 0.0f;
				const float cos_i = nx * s.sx + ny * s.sy + nz * s.sz;
				if (cos_i > 0.0f) {
					const float fj = s.az / dphi;
					const int j0 = int(fj) % DIRECTIONS;
					const int j1 = (j0 + 1) % DIRECTIONS;
					const float t = fj - Math::floor(fj);
					const float horizon = Math::lerp(hz[j0], hz[j1], t);
					if (Math::asin(s.sy) > horizon) {
						e += s.beam * cos_i;
					}
				}
				e += s.diffuse * view + rho * s.global * (1.0f - view);
				year += e;
				if (s.winter) {
					winter += e;
				}
			}
			const int i = z * n + x;
			r_year[i] = float(year / total_samples);
			r_winter[i] = winter_samples > 0 ? float(winter / winter_samples) : 0.0f;
			r_sky[i] = view;
		}
	});
}

/* Winstral Sx, degrees, for 16 wind directions: the mean over a 30 degree window (every 5 degrees) of the
   largest upward angle to the terrain within sx_distance upwind. Positive sheltered, negative exposed. */
inline void winstral_sx(const Params &p, const float *p_h, int p_n, float p_cell, std::vector<float> &r_sx) {
	const int n = p_n;
	r_sx.assign(size_t(n) * n * DIRECTIONS, 0.0f);
	const int steps = MAX(1, int(Math::round(p.sx_distance / p_cell)));
	const int RAYS = 7;
	float rx[DIRECTIONS][RAYS], rz[DIRECTIONS][RAYS];
	for (int d = 0; d < DIRECTIONS; d++) {
		for (int r = 0; r < RAYS; r++) {
			// Wind from bearing b: upwind is toward b.
			const float a = Math::deg_to_rad(d * DIRECTION_STEP + 5.0f * (r - RAYS / 2));
			rx[d][r] = Math::sin(a);
			rz[d][r] = -Math::cos(a);
		}
	}
	parallel_for(n, [&](int z) {
		for (int x = 0; x < n; x++) {
			const float h0 = p_h[z * n + x];
			for (int d = 0; d < DIRECTIONS; d++) {
				float sum = 0.0f;
				for (int r = 0; r < RAYS; r++) {
					float best = -float(Math::PI) * 0.5f;
					bool any = false;
					for (int s = 1; s <= steps; s++) {
						const int ix = x + int(Math::round(rx[d][r] * s));
						const int iz = z + int(Math::round(rz[d][r] * s));
						if (ix < 0 || iz < 0 || ix >= n || iz >= n) {
							break;
						}
						const float dist = Math::sqrt(float((ix - x) * (ix - x) + (iz - z) * (iz - z))) * p_cell;
						best = MAX(best, Math::atan((p_h[iz * n + ix] - h0) / dist));
						any = true;
					}
					sum += any ? best : 0.0f;
				}
				r_sx[(size_t(z) * n + x) * DIRECTIONS + d] = Math::rad_to_deg(sum / RAYS);
			}
		}
	});
}

/* Temperature, snow fraction and aridity per cell from the surface height and the year's precipitation. */
inline void temperature_fields(const Params &p, float p_height, float p_precip, float *r_mean, float *r_jan, float *r_jul, float *r_snow, float *r_aridity) {
	float t[12];
	float mean = 0.0f, snow = 0.0f;
	const float spread = MAX(p.daily_temperature_spread, 0.1f);
	for (int m = 0; m < 12; m++) {
		t[m] = temperature(p, p_height, m);
		mean += t[m] / 12.0f;
		// Share of days below the threshold, daily means normal around the month's mean.
		snow += 0.5f * (1.0f + std::erf((p.rain_snow_threshold - t[m]) / (spread * float(Math::SQRT2)))) / 12.0f;
	}
	*r_mean = mean;
	*r_jan = t[0];
	*r_jul = t[6];
	*r_snow = snow;
	const float pet = thornthwaite_pet(t, p.latitude);
	*r_aridity = pet > 1.0f ? MIN(p_precip / pet, 10.0f) : 10.0f;
}

} // namespace TerrainClimate

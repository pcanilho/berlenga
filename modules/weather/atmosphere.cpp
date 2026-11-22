/**************************************************************************/
/*  atmosphere.cpp                                                        */
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

#include "atmosphere.h"

#include "core/math/math_funcs.h"

namespace WeatherAtmosphere {

namespace {

struct Media {
	Vector3 scattering; // Rayleigh + Mie
	Vector3 extinction;
	Vector3 rayleigh;
	double mie = 0.0;
};

Media media_at(const Params &p, double p_height) {
	const double h = MAX(p_height, 0.0);
	const double dr = Math::exp(-h / p.rayleigh_scale_height);
	const double dm = Math::exp(-h / p.mie_scale_height);
	const double dozone = MAX(0.0, 1.0 - Math::abs(h - p.ozone_center) / p.ozone_half_width);
	Media m;
	m.rayleigh = p.rayleigh_scattering * dr;
	m.mie = p.mie_scattering * dm;
	m.scattering = m.rayleigh + Vector3(m.mie, m.mie, m.mie);
	const double mie_ext = p.mie_extinction * dm;
	m.extinction = m.rayleigh + Vector3(mie_ext, mie_ext, mie_ext) + p.ozone_absorption * dozone;
	return m;
}

// Distance along (r, mu) to a sphere of radius p_radius; -1 if it is missed.
double ray_sphere(double p_r, double p_mu, double p_radius, bool p_far) {
	const double b = p_r * p_mu;
	const double disc = b * b - (p_r * p_r - p_radius * p_radius);
	if (disc < 0.0) {
		return -1.0;
	}
	const double s = Math::sqrt(disc);
	const double t = p_far ? -b + s : -b - s;
	return t;
}

bool hits_ground(const Params &p, double p_r, double p_mu) {
	return p_mu < 0.0 && p_r * p_r * (p_mu * p_mu - 1.0) + p.bottom_radius * p.bottom_radius >= 0.0;
}

Vector3 vexp(const Vector3 &v) {
	return Vector3(Math::exp(-v.x), Math::exp(-v.y), Math::exp(-v.z));
}

} // namespace

Vector3 transmittance(const Params &p, double p_r, double p_mu) {
	if (hits_ground(p, p_r, p_mu)) {
		return Vector3();
	}
	const double t_max = MAX(ray_sphere(p_r, p_mu, p.top_radius, true), 0.0);
	const int steps = 40;
	Vector3 depth;
	const double dt = t_max / steps;
	for (int i = 0; i < steps; i++) {
		const double t = (i + 0.5) * dt;
		const double r = Math::sqrt(p_r * p_r + t * t + 2.0 * p_r * p_mu * t);
		depth += media_at(p, r - p.bottom_radius).extinction * dt;
	}
	return vexp(depth);
}

Ref<Image> bake_transmittance(const Params &p) {
	const double h = Math::sqrt(p.top_radius * p.top_radius - p.bottom_radius * p.bottom_radius);
	Vector<uint8_t> data;
	data.resize(TRANSMITTANCE_W * TRANSMITTANCE_H * 4 * sizeof(float));
	float *w = (float *)data.ptrw();
	for (int y = 0; y < TRANSMITTANCE_H; y++) {
		const double x_r = (y + 0.5) / TRANSMITTANCE_H;
		const double rho = h * x_r;
		const double r = Math::sqrt(rho * rho + p.bottom_radius * p.bottom_radius);
		for (int x = 0; x < TRANSMITTANCE_W; x++) {
			const double x_mu = (x + 0.5) / TRANSMITTANCE_W;
			const double d_min = p.top_radius - r;
			const double d_max = rho + h;
			const double d = d_min + x_mu * (d_max - d_min);
			const double mu = d == 0.0 ? 1.0 : CLAMP((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
			const Vector3 t = transmittance(p, r, mu);
			float *o = w + (y * TRANSMITTANCE_W + x) * 4;
			o[0] = t.x;
			o[1] = t.y;
			o[2] = t.z;
			o[3] = 1.0f;
		}
	}
	return Image::create_from_data(TRANSMITTANCE_W, TRANSMITTANCE_H, false, Image::FORMAT_RGBAF, data);
}

// Hillaire 2020, section 5.5: at height h with the sun at cosine mu_s, the second order light L_2nd and
// the transfer f_ms gathered over the sphere with an isotropic phase function; every higher order is a
// geometric series, so Psi_ms = L_2nd / (1 - f_ms) (equation 9), per unit sun illuminance.
Ref<Image> bake_multiscatter(const Params &p) {
	const int n = MULTISCATTER_N;
	const int dirs_sqrt = 8;
	const int steps = 20;
	Vector<uint8_t> data;
	data.resize(n * n * 4 * sizeof(float));
	float *w = (float *)data.ptrw();
	const double iso = 1.0 / (4.0 * Math::PI);
	for (int y = 0; y < n; y++) {
		const double r = p.bottom_radius + (y + 0.5) / n * (p.top_radius - p.bottom_radius);
		for (int x = 0; x < n; x++) {
			const double mu_s = (x + 0.5) / n * 2.0 - 1.0;
			const Vector3 sun(Math::sqrt(MAX(1.0 - mu_s * mu_s, 0.0)), mu_s, 0.0);
			Vector3 l2;
			Vector3 fms;
			for (int i = 0; i < dirs_sqrt; i++) {
				for (int j = 0; j < dirs_sqrt; j++) {
					// Uniform on the sphere (equal-area in cos theta and phi).
					const double cos_t = 1.0 - 2.0 * (i + 0.5) / dirs_sqrt;
					const double sin_t = Math::sqrt(MAX(1.0 - cos_t * cos_t, 0.0));
					const double phi = Math::TAU * (j + 0.5) / dirs_sqrt;
					const Vector3 dir(sin_t * Math::cos(phi), cos_t, sin_t * Math::sin(phi));
					const double mu = dir.y;
					const bool ground = hits_ground(p, r, mu);
					const double t_max = ground ? ray_sphere(r, mu, p.bottom_radius, false) : ray_sphere(r, mu, p.top_radius, true);
					const double dt = MAX(t_max, 0.0) / steps;
					Vector3 throughput(1, 1, 1);
					Vector3 lum;
					Vector3 f;
					for (int k = 0; k < steps; k++) {
						const double t = (k + 0.5) * dt;
						const Vector3 pos = Vector3(0, r, 0) + dir * t;
						const double pr = pos.length();
						const Media m = media_at(p, pr - p.bottom_radius);
						const double sun_mu = pos.dot(sun) / pr;
						const Vector3 t_sun = transmittance(p, pr, sun_mu);
						const Vector3 step_t = vexp(m.extinction * dt);
						const Vector3 s = t_sun * m.scattering * iso;
						for (int c = 0; c < 3; c++) {
							const double ext = MAX(m.extinction[c], 1e-12);
							const double integ = (1.0 - step_t[c]) / ext;
							lum[c] += throughput[c] * s[c] * integ;
							f[c] += throughput[c] * m.scattering[c] * integ;
						}
						throughput = throughput * step_t;
					}
					if (ground) {
						const Vector3 gp = Vector3(0, r, 0) + dir * t_max;
						const Vector3 up = gp.normalized();
						const double cos_sun = up.dot(sun);
						const Vector3 t_sun = transmittance(p, p.bottom_radius + 1.0, cos_sun);
						lum += throughput * t_sun * p.ground_albedo * (MAX(cos_sun, 0.0) / Math::PI);
					}
					l2 += lum / double(dirs_sqrt * dirs_sqrt);
					fms += f / double(dirs_sqrt * dirs_sqrt);
				}
			}
			float *o = w + (y * n + x) * 4;
			for (int c = 0; c < 3; c++) {
				o[c] = float(l2[c] / MAX(1.0 - fms[c], 1e-3));
			}
			o[3] = 1.0f;
		}
	}
	return Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, data);
}

const char *GLSL = R"(
// Hillaire 2020 atmosphere (modules/weather/atmosphere.cpp), lengths in m.
uniform sampler2D atmo_transmittance : filter_linear, repeat_disable;
uniform sampler2D atmo_multiscatter_lut : filter_linear, repeat_disable;
uniform vec2 atmo_radii = vec2(6360e3, 6460e3);
uniform vec3 atmo_rayleigh = vec3(5.802e-6, 13.558e-6, 33.1e-6);
uniform vec4 atmo_mie = vec4(3.996e-6, 4.440e-6, 1200.0, 0.8); // scattering, extinction, scale height, g
uniform vec3 atmo_ozone = vec3(0.650e-6, 1.881e-6, 0.085e-6);
uniform float atmo_rayleigh_height = 8000.0;
uniform vec3 atmo_ground_albedo = vec3(0.3);
uniform float atmo_far_water : hint_range(0, 1) = 0.0; // how much of the ground past the scene is open water

float atmo_ray_sphere(float r, float mu, float radius, bool far_hit) {
	float b = r * mu;
	float disc = b * b - (r * r - radius * radius);
	if (disc < 0.0) {
		return -1.0;
	}
	float s = sqrt(disc);
	return far_hit ? -b + s : -b - s;
}

bool atmo_hits_ground(float r, float mu) {
	return mu < 0.0 && r * r * (mu * mu - 1.0) + atmo_radii.x * atmo_radii.x >= 0.0;
}

vec3 atmo_transmittance_to_top(float r, float mu) {
	float h = sqrt(atmo_radii.y * atmo_radii.y - atmo_radii.x * atmo_radii.x);
	float rho = sqrt(max(r * r - atmo_radii.x * atmo_radii.x, 0.0));
	float disc = r * r * (mu * mu - 1.0) + atmo_radii.y * atmo_radii.y;
	float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
	float d_min = atmo_radii.y - r;
	float d_max = rho + h;
	vec2 uv = vec2((d - d_min) / max(d_max - d_min, 1.0), rho / h);
	uv = (uv * vec2(255.0, 63.0) + 0.5) / vec2(256.0, 64.0);
	return textureLod(atmo_transmittance, uv, 0.0).rgb;
}

// Light from a body at cosine mu_s seen from radius r: 0 behind the planet, softened over the body's own
// size so the Earth's shadow edge is not a hard line.
vec3 atmo_light_transmittance(float r, float mu_s) {
	float horizon = -sqrt(max(1.0 - (atmo_radii.x / r) * (atmo_radii.x / r), 0.0));
	return atmo_transmittance_to_top(r, mu_s) * smoothstep(horizon - 0.0047, horizon + 0.0047, mu_s);
}

vec3 atmo_multiscatter(float r, float mu_s) {
	vec2 uv = vec2(mu_s * 0.5 + 0.5, (r - atmo_radii.x) / (atmo_radii.y - atmo_radii.x));
	uv = clamp(uv, vec2(0.0), vec2(1.0));
	uv = (uv * 31.0 + 0.5) / 32.0;
	return textureLod(atmo_multiscatter_lut, uv, 0.0).rgb;
}

void atmo_media(float h, out vec3 rayleigh_s, out float mie_s, out vec3 extinction) {
	h = max(h, 0.0);
	rayleigh_s = atmo_rayleigh * exp(-h / atmo_rayleigh_height);
	float dm = exp(-h / atmo_mie.z);
	mie_s = atmo_mie.x * dm;
	float ozone = max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
	extinction = rayleigh_s + vec3(atmo_mie.y * dm) + atmo_ozone * ozone;
}

float atmo_phase_rayleigh(float c) {
	return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

// Cornette-Shanks, as Hillaire and Bruneton use for Mie.
float atmo_phase_mie(float c, float g) {
	float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
	return k * (1.0 + c * c) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// Light scattered toward the eye along a ray from height cam_h (m above the ground) in direction rd,
// out to max_dist (or the top of the air / the ground), from up to two bodies of top-of-air irradiance
// e0, e1 in directions l0, l1. rgb = in-scattered radiance, a = mean transmittance along the ray (for
// what lies behind, e.g. the ground or the stars).
vec4 atmo_scatter(float cam_h, vec3 rd, vec3 l0, vec3 e0, vec3 l1, vec3 e1, float max_dist, int steps, float jitter, out vec3 transmittance_out) {
	float r0 = atmo_radii.x + max(cam_h, 1.0);
	float mu = rd.y;
	bool ground = atmo_hits_ground(r0, mu);
	float t_max = ground ? atmo_ray_sphere(r0, mu, atmo_radii.x, false) : atmo_ray_sphere(r0, mu, atmo_radii.y, true);
	t_max = max(min(t_max, max_dist), 0.0);
	vec3 origin = vec3(0.0, r0, 0.0);
	float c0 = dot(rd, l0);
	float c1 = dot(rd, l1);
	float pr0 = atmo_phase_rayleigh(c0);
	float pm0 = atmo_phase_mie(c0, atmo_mie.w);
	float pr1 = atmo_phase_rayleigh(c1);
	float pm1 = atmo_phase_mie(c1, atmo_mie.w);
	vec3 lum = vec3(0.0);
	vec3 thr = vec3(1.0);
	float t_prev = 0.0;
	for (int i = 0; i < steps; i++) {
		// Denser near the eye, where the air is thickest along the ray.
		float f = (float(i) + jitter) / float(steps);
		float t = t_max * f * f;
		float f1 = (float(i) + 1.0) / float(steps);
		float t_next = t_max * f1 * f1;
		float dt = t_next - t_prev;
		t_prev = t_next;
		vec3 p = origin + rd * t;
		float r = length(p);
		vec3 up = p / r;
		vec3 rs;
		float ms;
		vec3 ext;
		atmo_media(r - atmo_radii.x, rs, ms, ext);
		vec3 s = vec3(0.0);
		float mu0 = dot(up, l0);
		float mu1 = dot(up, l1);
		s += e0 * (atmo_light_transmittance(r, mu0) * (rs * pr0 + ms * pm0) + atmo_multiscatter(r, mu0) * (rs + ms));
		s += e1 * (atmo_light_transmittance(r, mu1) * (rs * pr1 + ms * pm1) + atmo_multiscatter(r, mu1) * (rs + ms));
		vec3 step_t = exp(-ext * dt);
		lum += thr * s * (1.0 - step_t) / max(ext, vec3(1e-12));
		thr *= step_t;
	}
	if (ground && t_max < max_dist) {
		// The ground beyond, lit by the bodies through the air and by the sky (multiple scattering).
		vec3 gp = origin + rd * t_max;
		vec3 up = normalize(gp);
		float g0 = dot(up, l0);
		float g1 = dot(up, l1);
		vec3 lit = e0 * atmo_light_transmittance(atmo_radii.x + 1.0, g0) * max(g0, 0.0) + e1 * atmo_light_transmittance(atmo_radii.x + 1.0, g1) * max(g1, 0.0);
		lum += thr * mix(atmo_ground_albedo, vec3(0.02), atmo_far_water) / PI * lit;
	}
	transmittance_out = thr;
	return vec4(lum, (thr.r + thr.g + thr.b) / 3.0);
}
)";

} // namespace WeatherAtmosphere

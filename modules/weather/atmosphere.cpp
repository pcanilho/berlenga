/**************************************************************************/
/*  atmosphere.cpp                                                        */
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
// The weather's aerosol haze (WeatherMap3D aerosol_haze), past the froxel fog that draws its nearer part: its
// scattering and extinction at the ground (1/m), its scale height (m), and the distance along the eye's rays
// where it starts (the froxels' reach). Not in the tables: its own multiple scattering is left out.
// Until a WeatherMap3D sets it: the map's own default haze (40 km dry visibility, Koschmieder 3.912 / V, albedo
// 0.9), so a sky alone hazes its far clouds into the horizon as the world's does. Without it the deck ran to the
// horizon as a crisp strip of small puffs in rows; real cumulus there merge into the boundary layer's haze.
uniform vec4 atmo_aerosol = vec4(8.802e-5, 9.78e-5, 1200.0, 0.0);
// The clouds' shadow map (WeatherMap3D bakes it in the sun's plane; size 0: none, the shader marches the
// clouds itself): red the beam's transmittance, green the diffuse. Axes in xyz, the square's corner in w.
uniform sampler2D atmo_cloud_shadow_map : filter_linear, repeat_disable, hint_default_white;
uniform vec4 atmo_cloud_shadow_u = vec4(1.0, 0.0, 0.0, 0.0);
uniform vec4 atmo_cloud_shadow_v = vec4(0.0, 0.0, 1.0, 0.0);
uniform float atmo_cloud_shadow_size = 0.0;

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

// The aerosol's optical depth from radius r toward cosine mu: its column above, by the air mass of an
// exponential layer, 1 / sqrt(mu^2 + 2 H / (pi R)), which is 1 / mu overhead and the grazing column
// sqrt(pi R / (2 H)) at the horizon (Chapman's function for a thin layer).
float atmo_aerosol_tau(float r, float mu) {
	if (atmo_aerosol.y <= 0.0) {
		return 0.0;
	}
	float hs = atmo_aerosol.z;
	float m = inversesqrt(max(mu, 0.0) * max(mu, 0.0) + 2.0 * hs / (PI * atmo_radii.x));
	return atmo_aerosol.y * hs * exp(-max(r - atmo_radii.x, 0.0) / hs) * m;
}

// What the aerosol past the froxels lets through to the eye from beyond the air (the sun's disc, the stars).
float atmo_aerosol_view(float r_cam, vec3 rd) {
	if (atmo_aerosol.y <= 0.0) {
		return 1.0;
	}
	vec3 pw = vec3(0.0, r_cam, 0.0) + rd * atmo_aerosol.w;
	float r_w = length(pw);
	return exp(-atmo_aerosol_tau(r_w, dot(rd, pw) / r_w));
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

// The sun's light through the cloud layer to the point q (planet-centred, the camera's xz at the origin,
// cam_xz in the world): the clouds' shadow in the air below them, which draws shafts across the sky
// (Hillaire 2020 and Unreal's "cloud shadow on atmosphere"). The same base shape the cloud shadow bake
// uses (wc_extinction without the detail erosion, from WeatherClouds' GLSL ahead of this), read at the
// steps' scale. x: the direct beam, exp(-tau); y: the light the deck lets through diffusely (two-stream,
// Bohren 1987, as march_clouds' diffuse term), which the multiply scattered light under it follows.
// Far off (p_dist past 20 km) the steps along the view are kilometres apart and single clouds' shadows
// would alias into bands at the horizon: there it turns to the shadow's mean over the cover (the share of
// the ground under cloud, a typical cloud letting a tenth of the beam through).
vec2 atmo_cloud_transmittance(vec3 q, vec3 l, vec2 cam_xz, float p_step, float p_dist) {
	float r = length(q);
	float mu_s = dot(q, l) / r;
	float rb = atmo_radii.x + cloud_base_min;
	float rt = atmo_radii.x + cloud_top_max;
	if (r >= rt) {
		return vec2(1.0);
	}
	float s0 = r < rb ? atmo_ray_sphere(r, mu_s, rb, true) : 0.0;
	float s1 = atmo_ray_sphere(r, mu_s, rt, true);
	if (s0 < 0.0 || s1 <= s0) {
		return vec2(1.0);
	}
	if (atmo_cloud_shadow_size > 0.0) {
		// The map: one read, sharp to its texels (160 m), out to its edge, then the mean.
		vec3 w = vec3(cam_xz.x + q.x, r - atmo_radii.x, cam_xz.y + q.z);
		vec2 uv = (vec2(dot(w, atmo_cloud_shadow_u.xyz), dot(w, atmo_cloud_shadow_v.xyz)) - vec2(atmo_cloud_shadow_u.w, atmo_cloud_shadow_v.w)) / atmo_cloud_shadow_size;
		vec2 e = abs(uv - 0.5) * 2.0;
		float edge = smoothstep(0.8, 0.98, max(e.x, e.y));
		vec2 c = edge < 1.0 ? textureLod(atmo_cloud_shadow_map, uv, 0.0).rg : vec2(1.0);
		if (edge > 0.0) {
			float cover = wc_cover_type(w.xz).x;
			c = mix(c, vec2(1.0 - 0.9 * cover, 1.0 - 0.5 * cover), edge);
		}
		return c;
	}
	float far_mean = smoothstep(20000.0, 60000.0, p_dist);
	vec2 mean = vec2(1.0);
	if (far_mean > 0.0) {
		vec3 mq = q + l * (0.5 * (s0 + s1));
		float cover = wc_cover_type(vec2(cam_xz.x + mq.x, cam_xz.y + mq.z)).x;
		mean = vec2(1.0 - 0.9 * cover, 1.0 - 0.5 * cover);
		if (far_mean >= 1.0) {
			return mean;
		}
	}
	// The shape read near its own scale (a cloud's edge, 50 to 200 m), not at the steps' length: read at
	// a kilometre the noise's mips are a uniform grey and the shadow a uniform dimming, with no shafts.
	const int n = 5;
	float ds = (s1 - s0) / float(n);
	float lod_step = clamp(p_step, 50.0, 200.0);
	float tau = 0.0;
	for (int i = 0; i < n; i++) {
		vec3 lq = q + l * (s0 + (float(i) + 0.5) * ds);
		vec3 w = vec3(cam_xz.x + lq.x, length(lq) - atmo_radii.x, cam_xz.y + lq.z);
		tau += wc_extinction(w, wc_cover_type(w.xz), false, lod_step) * ds;
	}
	return mix(vec2(exp(-tau), 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau)), mean, far_mean);
}

// One stretch [t_a, t_b] of a view ray, its steps denser near t_a (where the air is thickest along the
// ray), added to lum and thr. Under the cloud deck (shadow) each step reads the clouds' shadow.
// The aerosol haze scatters and dims from aerosol_from on (the sun's light through it everywhere).
void atmo_march(vec3 origin, vec3 rd, float t_a, float t_b, int steps, float jitter, bool shadow, vec2 cam_xz, vec3 l0, vec3 e0, vec3 l1, vec3 e1, vec4 phases, float aerosol_from, inout vec3 lum, inout vec3 thr) {
	float span = t_b - t_a;
	if (span <= 0.0 || steps <= 0) {
		return;
	}
	// Continental aerosol's forward scattering (g 0.6, as the froxels light it).
	float pa0 = atmo_phase_mie(dot(rd, l0), 0.6);
	float pa1 = atmo_phase_mie(dot(rd, l1), 0.6);
	float t_prev = t_a;
	for (int i = 0; i < steps; i++) {
		float f = (float(i) + jitter) / float(steps);
		float t = t_a + span * f * f;
		float f1 = (float(i) + 1.0) / float(steps);
		float t_next = t_a + span * f1 * f1;
		float dt = t_next - t_prev;
		t_prev = t_next;
		vec3 p = origin + rd * t;
		float r = length(p);
		vec3 up = p / r;
		vec3 rs;
		float ms;
		vec3 ext;
		atmo_media(r - atmo_radii.x, rs, ms, ext);
		float mu0 = dot(up, l0);
		float mu1 = dot(up, l1);
		vec2 c0 = vec2(1.0);
		vec2 c1 = vec2(1.0);
		if (shadow) {
			if (mu0 > 0.0) {
				c0 = atmo_cloud_transmittance(p, l0, cam_xz, dt, t);
			}
			if (mu1 > 0.0 && dot(e1, vec3(1.0)) > 0.0) {
				c1 = atmo_cloud_transmittance(p, l1, cam_xz, dt, t);
			}
		}
		float as = 0.0;
		float a0 = 1.0;
		float a1 = 1.0;
		if (atmo_aerosol.y > 0.0) {
			a0 = exp(-atmo_aerosol_tau(r, mu0));
			a1 = exp(-atmo_aerosol_tau(r, mu1));
			if (t >= aerosol_from) {
				float da = exp(-max(r - atmo_radii.x, 0.0) / atmo_aerosol.z);
				as = atmo_aerosol.x * da;
				ext += vec3(atmo_aerosol.y * da);
			}
		}
		vec3 tl0 = atmo_light_transmittance(r, mu0) * a0;
		vec3 tl1 = atmo_light_transmittance(r, mu1) * a1;
		vec3 s = vec3(0.0);
		s += e0 * (tl0 * c0.x * (rs * phases.x + ms * phases.y + as * pa0) + atmo_multiscatter(r, mu0) * a0 * c0.y * (rs + ms + as));
		s += e1 * (tl1 * c1.x * (rs * phases.z + ms * phases.w + as * pa1) + atmo_multiscatter(r, mu1) * a1 * c1.y * (rs + ms + as));
		vec3 step_t = exp(-ext * dt);
		lum += thr * s * (1.0 - step_t) / max(ext, vec3(1e-12));
		thr *= step_t;
	}
}

// Light scattered toward the eye along a ray from height cam_h (m above the ground) in direction rd,
// out to max_dist (or the top of the air / the ground), from up to two bodies of top-of-air irradiance
// e0, e1 in directions l0, l1. rgb = in-scattered radiance, a = mean transmittance along the ray (for
// what lies behind, e.g. the ground or the stars).
// shadow_steps: more steps for the stretch under the cloud tops, each reading the clouds' shadow (0: no
// shadow, the whole ray in steps); cam_xz: the camera's place in the world, where the clouds are.
// aerosol_from: where along the ray the aerosol haze starts (the froxels' reach from the camera; 0 for rays
// the froxels do not draw).
vec4 atmo_scatter(float cam_h, vec3 rd, vec3 l0, vec3 e0, vec3 l1, vec3 e1, float max_dist, int steps, float jitter, int shadow_steps, vec2 cam_xz, float aerosol_from, out vec3 transmittance_out) {
	float r0 = atmo_radii.x + max(cam_h, 1.0);
	float mu = rd.y;
	bool ground = atmo_hits_ground(r0, mu);
	float t_max = ground ? atmo_ray_sphere(r0, mu, atmo_radii.x, false) : atmo_ray_sphere(r0, mu, atmo_radii.y, true);
	t_max = max(min(t_max, max_dist), 0.0);
	vec3 origin = vec3(0.0, r0, 0.0);
	float c0 = dot(rd, l0);
	float c1 = dot(rd, l1);
	vec4 phases = vec4(atmo_phase_rayleigh(c0), atmo_phase_mie(c0, atmo_mie.w), atmo_phase_rayleigh(c1), atmo_phase_mie(c1, atmo_mie.w));
	// Under the cloud tops: out to where the ray leaves them (the deck's own far fade, 200 km, at most).
	float t_low = 0.0;
	float r_top = atmo_radii.x + cloud_top_max;
	if (shadow_steps > 0 && r0 < r_top) {
		t_low = min(min(ground ? t_max : atmo_ray_sphere(r0, mu, r_top, true), t_max), 200000.0);
	}
	vec3 lum = vec3(0.0);
	vec3 thr = vec3(1.0);
	// The shadow's shafts read within some tens of kilometres: most of the steps there, a few for the rest.
	float t_near = min(t_low, 60000.0);
	atmo_march(origin, rd, 0.0, t_near, shadow_steps, jitter, true, cam_xz, l0, e0, l1, e1, phases, aerosol_from, lum, thr);
	atmo_march(origin, rd, t_near, t_low, shadow_steps > 0 ? 8 : 0, jitter, true, cam_xz, l0, e0, l1, e1, phases, aerosol_from, lum, thr);
	atmo_march(origin, rd, t_low, t_max, steps, jitter, false, cam_xz, l0, e0, l1, e1, phases, aerosol_from, lum, thr);
	if (ground && t_max < max_dist) {
		// The ground beyond, lit by the bodies through the air and by the sky (multiple scattering).
		vec3 gp = origin + rd * t_max;
		vec3 up = normalize(gp);
		float g0 = dot(up, l0);
		float g1 = dot(up, l1);
		vec3 lit = e0 * atmo_light_transmittance(atmo_radii.x + 1.0, g0) * max(g0, 0.0) + e1 * atmo_light_transmittance(atmo_radii.x + 1.0, g1) * max(g1, 0.0);
		float ga0 = exp(-atmo_aerosol_tau(atmo_radii.x + 1.0, g0));
		lit = e0 * atmo_light_transmittance(atmo_radii.x + 1.0, g0) * ga0 * max(g0, 0.0) + e1 * atmo_light_transmittance(atmo_radii.x + 1.0, g1) * exp(-atmo_aerosol_tau(atmo_radii.x + 1.0, g1)) * max(g1, 0.0);
		if (shadow_steps > 0 && t_max < 200000.0 && g0 > 0.0) {
			// Under the clouds' shadow too: the sun through them, the sky's light through them diffusely.
			vec2 c = atmo_cloud_transmittance(gp, l0, cam_xz, t_max / float(shadow_steps), t_max);
			lit = e0 * atmo_light_transmittance(atmo_radii.x + 1.0, g0) * ga0 * max(g0, 0.0) * mix(c.y, c.x, 0.8) + e1 * atmo_light_transmittance(atmo_radii.x + 1.0, g1) * exp(-atmo_aerosol_tau(atmo_radii.x + 1.0, g1)) * max(g1, 0.0);
		}
		lum += thr * mix(atmo_ground_albedo, vec3(0.02), atmo_far_water) / PI * lit;
	}
	transmittance_out = thr;
	return vec4(lum, (thr.r + thr.g + thr.b) / 3.0);
}
)";

} // namespace WeatherAtmosphere

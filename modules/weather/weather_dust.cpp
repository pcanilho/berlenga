/**************************************************************************/
/*  weather_dust.cpp                                                      */
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

// Mineral dust (docs/ideas/sand.md, "Haboob dust walls"). Strong dry wind over loose ground raises dust: the
// horizontal saltation flux Q from the friction speed over its threshold (White 1979, Q = C rho/g u*^3 (1 + r)
// (1 - r^2), r = u*t/u*), and the dust the saltating grains knock loose into the air, F = alpha Q with
// alpha = 10^(0.134 clay% - 6) per metre (Marticorena and Bergametti 1995). The map keeps it as its own layer beside
// the fires' smoke, grams per square metre over each cell: carried by the cell's own wind near the ground (the dust
// rides the outflow that raised it, not the wind aloft), settling at about 1.5 cm/s through its mixed depth (10 to
// 20 um grains, Stokes) and washed out as a coarse aerosol. A storm's downburst outflow is the haboob: its wind raises
// the dust and carries it out to where the outflow slows, piling it at the gust front, whose leading edge the map
// marks (the head share): there the density current's nose stands higher than the layer behind it (Simpson 1997;
// Benjamin 1968), so the wall's top is raised up to 1.75 times. Optics for the readers: mass extinction 0.6 m2/g at
// 550 nm, Angstrom exponent about 0.2, single scattering albedo 0.92 falling to 0.85 in the blue (iron oxides), so
// the sun dims orange-brown (Dubovik et al. 2002; Miller et al. 2008).

#include "weather_map_3d.h"

#include "modules/world/world_contracts.h"

static constexpr float DUST_MASS_EXTINCTION = 0.6f; // m2/g at 550 nm
static constexpr float DUST_SETTLING = 0.015f; // m/s
static constexpr float DUST_DIFFUSION = 100.0f; // m2/s
static constexpr float DUST_USTAR_DRY = 0.35f; // m/s, natural desert surfaces' threshold (Gillette 1980: 0.2 for loose
// fine sand, more with crusts and gravel; about an 8.5 m/s wind at 10 m)
static constexpr float DUST_Z0 = 5e-4f; // m, the erodible surface's own roughness (Marticorena et al. 1997), not the map's
static constexpr float DOWNBURST_PEAK_D = 0.6382f; // weather_map_3d.cpp's DOWNBURST_PEAK

// Smooth value noise for the gust front's lobes (hash of the lattice, smoothstep between).
static float _dust_hash(int p_x, int p_y) {
	uint32_t h = uint32_t(p_x) * 374761393u + uint32_t(p_y) * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h ^ (h >> 16)) / 4294967295.0f;
}

static float _dust_noise(Vector2 p_p) {
	const int x = int(Math::floor(p_p.x)), y = int(Math::floor(p_p.y));
	const float fx = p_p.x - x, fy = p_p.y - y;
	const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
	return Math::lerp(Math::lerp(_dust_hash(x, y), _dust_hash(x + 1, y), sx), Math::lerp(_dust_hash(x, y + 1), _dust_hash(x + 1, y + 1), sx), sy);
}
static const Vector3 DUST_SPECTRUM(0.97f, 1.0f, 1.04f); // (lambda / 550)^-0.2 at 610, 550 and 465 nm

float WeatherMap3D::dust_flux(float p_ustar, float p_ustar_threshold, float p_clay_percent) {
	if (p_ustar <= p_ustar_threshold || p_ustar <= 0.0f) {
		return 0.0f;
	}
	const float r = p_ustar_threshold / p_ustar;
	const float q = 2.6f * 1.225f / 9.81f * p_ustar * p_ustar * p_ustar * (1.0f + r) * (1.0f - r * r); // kg/m/s
	const float alpha = Math::pow(10.0f, 0.134f * CLAMP(p_clay_percent, 0.0f, 20.0f) - 6.0f); // 1/m
	return alpha * q;
}

// g/m2/s for a 10 m wind over ground with p_loose of its top loose and dry, p_wetness of its surface wet (the film
// raises the threshold steeply: Fecan et al. 1999's moisture factor, here 1 + 4 wetness, none past half wet).
float WeatherMap3D::get_dust_flux_for(float p_wind10, float p_loose, float p_wetness) const {
	if (p_loose <= 0.0f || p_wetness >= 0.5f) {
		return 0.0f;
	}
	const float ustar = 0.41f * MAX(p_wind10, 0.0f) / Math::log(10.0f / DUST_Z0);
	const float ut = DUST_USTAR_DRY * (1.0f + 4.0f * CLAMP(p_wetness, 0.0f, 1.0f));
	return 1000.0f * dust_flux(ustar, ut, dust_clay) * CLAMP(p_loose, 0.0f, 1.0f) * dust_emission_scale;
}

float WeatherMap3D::_dust_loose_at(const Vector3 &p_position) const {
	const WorldGround *g = WorldContracts::ground();
	if (g && (g->has_provider() || g->get_script_instance())) {
		return CLAMP(g->blown_sand_at(p_position), 0.0f, 1.0f);
	}
	return dust_source_share;
}

void WeatherMap3D::_dust_step(double p_weather_dt) {
	const int n = resolution;
	if (cell_dust.size() != n * n) {
		cell_dust.resize(n * n);
		cell_dust.fill(0.0f);
		cell_dust_head.resize(n * n);
		cell_dust_head.fill(0.0f);
	}
	if (p_weather_dt <= 0.0 || !origin_valid || !dust_storms || cell_wind.size() != n * n * 2) {
		return;
	}
	const float cs = _cell_size();
	const float dt = float(p_weather_dt);
	const float depth = dust_layer_depth;
	const float storage = MAX(surface_storage, 1e-3f);
	const bool have_rain = cell_rain.size() == n * n;
	const bool have_film = cell_film.size() == n * n && cell_puddle.size() == n * n;
	const bool have_h = cell_height.size() == n * n;

	// The dust's own wind: each cell's 10 m wind, less the storms' outflow beyond their gust fronts. The outflow is a
	// density current of cold air with a front (as weather_clouds' shelf cloud: 1.45 core radii plus 0.02 per m/s of
	// outflow, lobed 0.25 radii in and out along it); the downburst profile's 1/r tail beyond it is the warm air
	// ahead, which neither raises the cold pool's dust nor carries it on. So the dust piles at the front: the wall.
	Vector<float> dust_wind;
	dust_wind.resize(n * n * 2);
	{
		float *dw = dust_wind.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				Vector2 w(cell_wind[i * 2 + 0], cell_wind[i * 2 + 1]);
				const Vector2 p((origin_cell.x + x + 0.5f) * cs, (origin_cell.y + z + 0.5f) * cs);
				for (const Storm &st : storms) {
					const float env = _storm_weight(st);
					if (env <= 0.0f || st.outflow <= 0.0f || st.fire) {
						continue;
					}
					const Vector2 d = p - st.position;
					const float rr = d.length();
					if (rr < 1e-3f) {
						continue;
					}
					const float front = st.radius * (1.45f + 0.02f * MIN(st.outflow, 15.0f) + 0.5f * (_dust_noise(d / 1500.0f + Vector2(41.7f, 13.9f)) - 0.5f));
					if (rr <= front) {
						continue;
					}
					const float x2 = (rr / st.radius) * (rr / st.radius);
					const float vr = st.outflow * (st.radius / rr) * (1.0f - Math::exp(-x2)) / DOWNBURST_PEAK_D;
					w -= d / rr * vr * env * Math::smoothstep(front, front + 0.6f * cs, rr);
				}
				dw[i * 2 + 0] = w.x;
				dw[i * 2 + 1] = w.y;
			}
		}
	}
	const float *dwind = dust_wind.ptr();

	// Raised: each cell's own 10 m wind over its loose ground.
	bool any = false;
	{
		float *d = cell_dust.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				const float speed = Vector2(dwind[i * 2 + 0], dwind[i * 2 + 1]).length();
				if (speed > 6.0f && !(have_rain && cell_rain[i] > 0.1f)) {
					const float wet = have_film ? (cell_puddle[i] > 0.0f ? 1.0f : MIN(cell_film[i] / storage, 1.0f)) : 0.0f;
					const float gy = have_h && Math::is_finite(cell_height[i]) ? cell_height[i] : ground_height;
					const Vector3 p((origin_cell.x + x + 0.5f) * cs, gy, (origin_cell.y + z + 0.5f) * cs);
					const float f = get_dust_flux_for(speed, _dust_loose_at(p), wet);
					d[i] += f * dt;
				}
				any = any || d[i] > 1e-6f;
			}
		}
	}
	if (!any) {
		cell_dust_head.fill(0.0f);
		return;
	}

	// Carried by its own wind, forward: each cell's load moves to where its wind takes it and is shared out over the
	// four cells round the landing point (a mass-conserving forward splat; a backward trace cannot pile the load up where
	// the wind converges, at the front), settling and washed out on the way.
	{
		const float keep = Math::exp(-DUST_SETTLING * dt / depth);
		const float snow_share = have_rain ? 1.0f - get_rain_share_at(_follow_position()) : 0.0f;
		Vector<float> old = cell_dust;
		const float *o = old.ptr();
		cell_dust.fill(0.0f);
		float *w = cell_dust.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (o[i] <= 0.0f) {
					continue;
				}
				float k = keep;
				if (have_rain && cell_rain[i] > 0.0f) {
					const float r = cell_rain[i];
					k *= Math::exp(-dt * WorldWeather::washout_rate(r * (1.0f - snow_share), r * snow_share, true));
				}
				const float m = o[i] * k;
				const float fx = x + dwind[i * 2 + 0] * dt / cs, fz = z + dwind[i * 2 + 1] * dt / cs;
				const int x0 = int(Math::floor(fx)), z0 = int(Math::floor(fz));
				const float tx = fx - x0, tz = fz - z0;
				auto put = [&](int xx, int zz, float a) {
					if (xx >= 0 && zz >= 0 && xx < n && zz < n) {
						w[zz * n + xx] += a; // what leaves the map is gone
					}
				};
				put(x0, z0, m * (1.0f - tx) * (1.0f - tz));
				put(x0 + 1, z0, m * tx * (1.0f - tz));
				put(x0, z0 + 1, m * (1.0f - tx) * tz);
				put(x0 + 1, z0 + 1, m * tx * tz);
			}
		}
		const float alpha = DUST_DIFFUSION * dt / (cs * cs);
		const int iters = MIN(int(Math::ceil(alpha / 0.2f)), 32);
		const float a = MIN(alpha / MAX(iters, 1), 0.2f);
		for (int it = 0; it < iters; it++) {
			old = cell_dust;
			o = old.ptr();
			w = cell_dust.ptrw();
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int i = z * n + x;
					const float l = x > 0 ? o[i - 1] : o[i], r = x < n - 1 ? o[i + 1] : o[i];
					const float d = z > 0 ? o[i - n] : o[i], up = z < n - 1 ? o[i + n] : o[i];
					w[i] = o[i] + a * (l + r + d + up - 4.0f * o[i]);
				}
			}
		}
	}

	// The leading edge: where the column falls off within two cells downwind along the cell's wind, on a column
	// thick enough to stand as a wall (over about 1 g/m2: an optical depth of 0.6).
	{
		const float *o = cell_dust.ptr();
		float *hd = cell_dust_head.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				const Vector2 wv(cell_wind[i * 2 + 0], cell_wind[i * 2 + 1]); // the cold air's push: toward the front
				const float sp = wv.length();
				if (o[i] < 0.2f || sp < 1.0f) {
					hd[i] = 0.0f;
					continue;
				}
				const Vector2 u = wv / sp * 2.0f;
				const int xd = CLAMP(int(Math::round(x + u.x)), 0, n - 1), zd = CLAMP(int(Math::round(z + u.y)), 0, n - 1);
				const float drop = (o[i] - o[zd * n + xd]) / o[i];
				hd[i] = Math::smoothstep(0.3f, 0.8f, drop) * Math::smoothstep(0.3f, 1.5f, o[i]);
			}
		}
	}
}

void WeatherMap3D::_dust_shift(Vector2i p_delta) {
	const int n = resolution;
	Vector<float> *fields[2] = { &cell_dust, &cell_dust_head };
	for (Vector<float> *f : fields) {
		if (f->size() != n * n) {
			continue;
		}
		Vector<float> old = *f;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int sx = x + p_delta.x, sz = z + p_delta.y;
				f->write[z * n + x] = (sx >= 0 && sz >= 0 && sx < n && sz < n) ? old[sz * n + sx] : 0.0f;
			}
		}
	}
}

// The shaders' dust (weather_dust_map over weather_map_rect): R column g/m2, G the top in m above the ground,
// B the head share; weather_dust_optics and weather_dust_albedo set once.
void WeatherMap3D::_dust_upload() {
	const int n = resolution;
	if (!publish_shader_globals || active != this || cell_dust.size() != n * n) {
		return;
	}
	Vector<uint8_t> data;
	data.resize(n * n * 8);
	uint16_t *p = reinterpret_cast<uint16_t *>(data.ptrw());
	for (int i = 0; i < n * n; i++) {
		const float h = cell_dust_head.size() == n * n ? cell_dust_head[i] : 0.0f;
		p[i * 4 + 0] = Math::make_half_float(cell_dust[i]);
		p[i * 4 + 1] = Math::make_half_float(cell_dust[i] > 1e-4f ? dust_layer_depth * (1.0f + 0.75f * h) : 0.0f);
		p[i * 4 + 2] = Math::make_half_float(h);
		p[i * 4 + 3] = 0;
	}
	if (dust_image.is_null() || dust_image->get_width() != n) {
		dust_image = Image::create_from_data(n, n, false, Image::FORMAT_RGBAH, data);
		dust_texture = ImageTexture::create_from_image(dust_image);
		dust_global_added = false;
	} else {
		dust_image->set_data(n, n, false, Image::FORMAT_RGBAH, data);
		dust_texture->update(dust_image);
	}
	if (!dust_global_added) {
		_set_global("weather_dust_map", dust_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		_set_global("weather_dust_optics", Vector4(DUST_MASS_EXTINCTION, DUST_SPECTRUM.x, DUST_SPECTRUM.y, DUST_SPECTRUM.z), RSE::GLOBAL_VAR_TYPE_VEC4);
		_set_global("weather_dust_albedo", Vector4(0.93f, 0.92f, 0.85f, 0.75f), RSE::GLOBAL_VAR_TYPE_VEC4);
		dust_global_added = true;
	}
}

void WeatherMap3D::add_dust(Vector3 p_position, float p_kg) {
	const int n = resolution;
	if (p_kg <= 0.0f || !origin_valid) {
		return;
	}
	if (cell_dust.size() != n * n) {
		cell_dust.resize(n * n);
		cell_dust.fill(0.0f);
		cell_dust_head.resize(n * n);
		cell_dust_head.fill(0.0f);
	}
	const float cs = _cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
	if (x < 0 || z < 0 || x >= n || z >= n) {
		return;
	}
	cell_dust.write[z * n + x] += p_kg * 1000.0f / (cs * cs);
}

void WeatherMap3D::clear_dust() {
	cell_dust.fill(0.0f);
	cell_dust_head.fill(0.0f);
}

void WeatherMap3D::set_dust_storms(bool p_v) {
	dust_storms = p_v;
	if (!p_v) {
		clear_dust();
	}
}

float WeatherMap3D::get_dust_at(Vector3 p_position) const {
	const int n = resolution;
	if (cell_dust.size() != n * n || !origin_valid || !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	return MAX(_sample(cell_dust, Vector2(p_position.x, p_position.z)), 0.0f);
}

float WeatherMap3D::get_dust_head_at(Vector3 p_position) const {
	const int n = resolution;
	if (cell_dust_head.size() != n * n || !origin_valid || !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	return CLAMP(_sample(cell_dust_head, Vector2(p_position.x, p_position.z)), 0.0f, 1.0f);
}

float WeatherMap3D::get_dust_top_at(Vector3 p_position) const {
	return get_dust_at(p_position) > 1e-4f ? dust_layer_depth * (1.0f + 0.75f * get_dust_head_at(p_position)) : 0.0f;
}

// Densest at the ground, falling off with height on a scale of a third of the top (dust raised at the surface and
// mixed up through the outflow's turbulent head; Chen and Fryrear 2002's profiles), none above the top.
float WeatherMap3D::get_dust_concentration_at(Vector3 p_position) const {
	const float col = get_dust_at(p_position);
	if (col <= 0.0f) {
		return 0.0f;
	}
	const float top = get_dust_top_at(p_position);
	const Vector2 xz(p_position.x, p_position.z);
	const float g = cell_height.size() == resolution * resolution ? _sample(cell_height, xz) : NAN;
	const float above = MAX(p_position.y - (Math::is_finite(g) ? g : ground_height), 0.0f);
	if (above > top) {
		return 0.0f;
	}
	const float hs = top / 3.0f;
	return col / (hs * (1.0f - Math::exp(-3.0f))) * Math::exp(-above / hs);
}

float WeatherMap3D::get_dust_optical_depth_at(Vector3 p_position) const {
	return DUST_MASS_EXTINCTION * get_dust_at(p_position);
}

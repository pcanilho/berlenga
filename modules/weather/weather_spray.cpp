/**************************************************************************/
/*  weather_spray.cpp                                                     */
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

// Sea spray on the shore (docs/ideas/world-links.md, "Sea spray wets and salts the shore"). Breaking waves tear
// drops off the crests and burst bubbles in the foam; the wind carries them inland (Andreas 1998; de Leeuw et al.
// 2000 for the surf zone). Two sizes, each carried downwind and settling at its own rate, steady along the wind
// (dA/ds = P - A / L, deposition A / L):
// - coarse spray (drops of 0.1 to 1 mm, settling about 1 m/s from a lift of about one breaker height), which wets
//   the shore for tens of metres: L = u (1 + H) / 1 m/s. Rising ground catches it (a cliff face takes the spray,
//   its top much less);
// - fine salt aerosol (film and jet drops, settling by dry deposition), salting surfaces for hundreds of metres:
//   L = 50 s x u; chloride deposition falls by about an order of magnitude over the first kilometre inland
//   (Gustafsson and Franzen 1996).
// Production goes with the breaking (WorldWater.breaking_at: the broken waves' height in the surf zone, whitecap
// cover times wave height on open water) and with the wind's square past about 2 m/s. Calibration: 1 m breakers in
// a 10 m/s onshore wind settle about 0.3 mm/h of spray at the waterline and some 0.3 g/m2 of salt a day as fine
// aerosol a few hundred metres inland (ISO 9223 class S2 to S3). Sea water carries 35 g of salt per kg.
// The spray's water lies as a film on surfaces (to the map's surface storage) and dries at Penman's rate; its
// salt stays, lost to rain (half to about 3.5 mm) and slowly otherwise (about two weeks).

#include "weather_map_3d.h"

#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "servers/rendering/rendering_server.h"

static constexpr float SPRAY_COARSE_PER_M = 1.0f; // mm/h of coarse spray made per m of breaking, at 10 m/s
static constexpr float SPRAY_FINE_PER_M = 0.25f; // g/m2/h of fine salt made per m of breaking, at 10 m/s
static constexpr float SPRAY_SETTLING = 1.0f; // m/s, coarse drops
static constexpr float SPRAY_FINE_TIME = 50.0f; // s: the fine aerosol's reach is the wind's run over it
static constexpr float SEA_SALT = 35.0f; // g of salt per kg (mm over a square metre) of sea water
static constexpr float SALT_RAIN_WASH = 5.0f; // mm of rain to wash off all but 1/e of the salt
static constexpr float SALT_LIFETIME = 14.0f * 86400.0f; // s

static float spray_wind_factor(float p_u10) {
	const float u = MAX(p_u10 - 2.0f, 0.0f) / 8.0f;
	return MIN(u * u, 6.0f);
}

void WeatherMap3D::_spray_follow(Vector3 p_center) {
	const int n = MAX(sea_spray_resolution, 8);
	const float cell = MAX(sea_spray_extent, 16.0f) / n;
	if (spray.n != n || spray.cell != cell) {
		spray.n = n;
		spray.cell = cell;
		spray.valid = false;
		for (Vector<float> *f : { &spray.breaking, &spray.ground, &spray.rate, &spray.salt_rate, &spray.film, &spray.salt }) {
			f->resize(n * n);
			f->fill(0.0f);
		}
		spray.image.unref();
		spray.texture.unref();
		spray.global_added = false;
	}
	const Vector2i o(int(Math::floor(p_center.x / cell)) - n / 2, int(Math::floor(p_center.z / cell)) - n / 2);
	if (spray.valid && o == spray.origin) {
		return;
	}
	if (spray.valid) {
		// Keep what overlaps (the film and salt stay where they lie); the sources outside are new.
		const Vector2i d = o - spray.origin;
		if (Math::abs(d.x) >= n || Math::abs(d.y) >= n) {
			spray.film.fill(0.0f);
			spray.salt.fill(0.0f);
		} else {
			for (Vector<float> *f : { &spray.film, &spray.salt, &spray.breaking, &spray.ground }) {
				Vector<float> old = *f;
				float *w = f->ptrw();
				for (int z = 0; z < n; z++) {
					for (int x = 0; x < n; x++) {
						const int sx = x + d.x, sz = z + d.y;
						w[z * n + x] = (sx >= 0 && sz >= 0 && sx < n && sz < n) ? old[sz * n + sx] : 0.0f;
					}
				}
			}
		}
	}
	spray.origin = o;
	spray.valid = true;
	spray.sources_valid = false;
	spray.source_row = 0;
}

void WeatherMap3D::_spray_sources(int p_rows) {
	const int n = spray.n;
	const WorldWater *ww = WorldContracts::water();
	const WorldGround *wg = WorldContracts::ground();
	const bool water = ww->has_provider();
	if (!water) {
		// No water anywhere: nothing breaks, and the ground need not be read.
		spray.any_breaking = false;
		spray.sources_valid = false;
		spray.source_row = 0;
		return;
	}
	const bool ground_known = wg->has_provider() || wg->get_script_instance();
	float *b = spray.breaking.ptrw();
	float *g = spray.ground.ptrw();
	for (int r = 0; r < p_rows; r++) {
		const int z = spray.source_row;
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float wx = (spray.origin.x + x + 0.5f) * spray.cell;
			const float wz = (spray.origin.y + z + 0.5f) * spray.cell;
			float gh = ground_known ? wg->height_at(Vector3(wx, ground_height + 5000.0f, wz)) : -Math::INF;
			if (!Math::is_finite(gh)) {
				gh = ground_height;
			}
			g[i] = gh;
			float br = -1.0f; // land
			if (water) {
				const float wh = ww->height_at(Vector3(wx, gh + 0.01f, wz));
				if (Math::is_finite(wh) && wh > gh + 0.02f) {
					br = MAX(ww->breaking_at(Vector3(wx, wh, wz)), 0.0f);
				}
			}
			b[i] = br;
		}
		spray.source_row++;
		if (spray.source_row >= n) {
			spray.source_row = 0;
			spray.sources_valid = true;
		}
	}
	bool any = false;
	for (int i = 0; i < n * n && !any; i++) {
		any = b[i] > 0.0f;
	}
	spray.any_breaking = any;
}

void WeatherMap3D::_spray_step(double p_weather_dt) {
	if (!sea_spray) {
		return;
	}
	_spray_follow(_follow_position());
	const int n = spray.n;
	// Sources: all at once the first time, then a band of rows per update so the water's changes come through.
	_spray_sources(spray.sources_valid ? MAX(n / 16, 1) : n);
	const float dt = float(p_weather_dt);
	const float dt_h = dt / 3600.0f;
	float *rate = spray.rate.ptrw();
	float *srate = spray.salt_rate.ptrw();
	const float *b = spray.breaking.ptr();
	const float *g = spray.ground.ptr();
	const float u10 = cur_wind_speed;
	const float wf = spray_wind_factor(u10);
	const Vector2 dir = cur_wind_direction.length() > 1e-4f ? cur_wind_direction.normalized() : Vector2(1, 0);
	if (!spray.any_breaking || wf <= 0.0f) {
		memset(rate, 0, sizeof(float) * n * n);
		memset(srate, 0, sizeof(float) * n * n);
	} else {
		// Steady transport along the wind, swept along its main axis: each cell takes the column (or row) upwind of
		// it, read a fraction of a cell across, one step s = cell / |main component| back.
		const bool along_x = Math::abs(dir.x) >= Math::abs(dir.y);
		const float major = along_x ? dir.x : dir.y;
		const float minor = along_x ? dir.y : dir.x;
		const int sgn = major >= 0.0f ? 1 : -1;
		const float s = spray.cell / MAX(Math::abs(major), 1e-3f);
		const float shift = minor / MAX(Math::abs(major), 1e-3f); // cells across per step
		// Coarse lift: about one breaker height over the crest; the largest breaking in the square sets it.
		float hmax = 0.0f;
		for (int i = 0; i < n * n; i++) {
			hmax = MAX(hmax, b[i]);
		}
		const float lift = 1.0f + hmax;
		const float lc = MAX(u10 * lift / SPRAY_SETTLING, 1.0f);
		const float lf = MAX(u10 * SPRAY_FINE_TIME, 1.0f);
		const float kc = Math::exp(-s / lc), kf = Math::exp(-s / lf);
		Vector<float> prev_c, prev_f, cur_c, cur_f;
		prev_c.resize(n);
		prev_f.resize(n);
		cur_c.resize(n);
		cur_f.resize(n);
		auto idx = [&](int p_major, int p_minor) { return along_x ? p_minor * n + p_major : p_major * n + p_minor; };
		for (int step = 0; step < n; step++) {
			const int m = sgn > 0 ? step : n - 1 - step;
			for (int k = 0; k < n; k++) {
				const int i = idx(m, k);
				const bool sea = b[i] >= 0.0f;
				const float pc = sea ? SPRAY_COARSE_PER_M * b[i] * wf : 0.0f; // mm/h
				const float pf = sea ? SPRAY_FINE_PER_M * b[i] * wf : 0.0f; // g/m2/h
				float ac, af; // airborne load, as the deposition it would give (mm/h, g/m2/h) times L
				float catch_c = 0.0f;
				if (step == 0) {
					// The square's upwind edge: over the sea, what a long fetch like this cell would carry.
					ac = pc * lc;
					af = pf * lf;
				} else {
					const float fk = k - shift;
					const int k0 = int(Math::floor(fk));
					const float t = fk - k0;
					auto rd = [&](const Vector<float> &p_v, int p_k) { return p_v[CLAMP(p_k, 0, n - 1)]; };
					ac = Math::lerp(rd(prev_c, k0), rd(prev_c, k0 + 1), t) * kc + pc * lc * (1.0f - kc);
					af = Math::lerp(rd(prev_f, k0), rd(prev_f, k0 + 1), t) * kf + pf * lf * (1.0f - kf);
					// Rising ground takes the coarse spray out of the air: the share below the rise, over a lift
					// of about one breaker height.
					const int mu = m - sgn;
					const float gu = Math::lerp(g[idx(mu, CLAMP(k0, 0, n - 1))], g[idx(mu, CLAMP(k0 + 1, 0, n - 1))], t);
					const float rise = g[i] - gu;
					if (rise > 0.0f && !sea) {
						const float taken = ac * (1.0f - Math::exp(-rise / lift));
						ac -= taken;
						catch_c = taken / s; // settles on the face
					}
				}
				cur_c.write[k] = ac;
				cur_f.write[k] = af;
				rate[i] = sea ? 0.0f : ac / lc + catch_c;
				srate[i] = sea ? 0.0f : af / lf + SEA_SALT * rate[i];
			}
			SWAP(prev_c, cur_c);
			SWAP(prev_f, cur_f);
		}
	}
	// The map's own film takes the spray too (its cells' mean), so wetness readers far from the square's detail and
	// the path tracer's state map see the wet shore.
	{
		const int mn = resolution;
		if (cell_spray.size() != mn * mn) {
			cell_spray.resize(mn * mn);
		}
		cell_spray.fill(0.0f);
		if (origin_valid && spray.any_breaking) {
			const float mcs = _cell_size();
			Vector<int> count;
			count.resize(mn * mn);
			count.fill(0);
			float *cs = cell_spray.ptrw();
			int *cnt = count.ptrw();
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int mx = int(Math::floor((spray.origin.x + x + 0.5f) * spray.cell / mcs)) - origin_cell.x;
					const int mz = int(Math::floor((spray.origin.y + z + 0.5f) * spray.cell / mcs)) - origin_cell.y;
					if (mx >= 0 && mz >= 0 && mx < mn && mz < mn) {
						cs[mz * mn + mx] += rate[z * n + x];
						cnt[mz * mn + mx]++;
					}
				}
			}
			for (int i = 0; i < mn * mn; i++) {
				cs[i] = cnt[i] > 0 ? cs[i] / cnt[i] : 0.0f;
			}
		}
	}
	// What settles: the film (dries at the shade's Penman rate) and the salt (rain washes it off).
	if (dt <= 0.0f) {
		return;
	}
	const float z0 = MAX(roughness_length, 1e-5f);
	const float u2 = cur_wind_speed * Math::log(2.0f / z0) / Math::log(10.0f / z0);
	const float evap = penman_evaporation(0.0f, u2, cur_air_temperature, MIN(cur_air_humidity, 0.98f)); // mm/h
	const float keep = Math::exp(-dt / SALT_LIFETIME);
	float *film = spray.film.ptrw();
	float *salt = spray.salt.ptrw();
	const bool raining = cell_rain.size() == resolution * resolution;
	for (int z = 0; z < n; z++) {
		float rain_row = 0.0f;
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (raining && (x & 7) == 0) {
				rain_row = get_rainfall_at(Vector3((spray.origin.x + x + 0.5f) * spray.cell, 0.0f, (spray.origin.y + z + 0.5f) * spray.cell));
			}
			film[i] = CLAMP(film[i] + (rate[i] - evap) * dt_h, 0.0f, MAX(surface_storage, 1e-3f));
			float sl = salt[i] * keep + srate[i] * dt_h;
			if (rain_row > 0.0f) {
				sl *= Math::exp(-rain_row * dt_h / SALT_RAIN_WASH);
			}
			salt[i] = sl;
		}
	}
	// Shaders that read where water has been see the spray's wetness near the camera too.
	bool wet_any = false;
	for (int i = 0; i < n * n && !wet_any; i++) {
		wet_any = film[i] > 1e-4f;
	}
	if (contact_wetness_enabled && wet_any) {
		Vector<float> wet;
		wet.resize(n * n);
		const float storage = MAX(surface_storage, 1e-3f);
		for (int i = 0; i < n * n; i++) {
			wet.write[i] = MIN(film[i] / storage, 1.0f);
		}
		add_contact_wetness_cells(wet.ptr(), n, n, get_sea_spray_rect());
	}
	_spray_upload();
}

void WeatherMap3D::_spray_upload() {
	const int n = spray.n;
	if (!publish_shader_globals || active != this || n <= 0) {
		return;
	}
	// R: the spray's wetness, G: the salt as 1 - exp(-salt / 10 g/m2) (salt burn and bloom read it).
	Vector<uint8_t> data;
	data.resize(n * n * 2);
	uint8_t *d = data.ptrw();
	const float storage = MAX(surface_storage, 1e-3f);
	for (int i = 0; i < n * n; i++) {
		d[i * 2 + 0] = uint8_t(CLAMP(spray.film[i] / storage, 0.0f, 1.0f) * 255.0f + 0.5f);
		d[i * 2 + 1] = uint8_t(CLAMP(1.0f - Math::exp(-spray.salt[i] / 10.0f), 0.0f, 1.0f) * 255.0f + 0.5f);
	}
	if (spray.image.is_null() || spray.image->get_width() != n) {
		spray.image = Image::create_from_data(n, n, false, Image::FORMAT_RG8, data);
		spray.texture = ImageTexture::create_from_image(spray.image);
		spray.global_added = false;
	} else {
		spray.image->set_data(n, n, false, Image::FORMAT_RG8, data);
		spray.texture->update(spray.image);
	}
	if (!spray.global_added) {
		_set_global("weather_spray_map", spray.texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		spray.global_added = true;
	}
	const Rect2 r = get_sea_spray_rect();
	_set_global("weather_spray_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
}

Rect2 WeatherMap3D::get_sea_spray_rect() const {
	if (!spray.valid) {
		return Rect2();
	}
	return Rect2(spray.origin.x * spray.cell, spray.origin.y * spray.cell, spray.n * spray.cell, spray.n * spray.cell);
}

float WeatherMap3D::_spray_sample(const Vector<float> &p_field, Vector3 p_position) const {
	const int n = spray.n;
	if (!sea_spray || !spray.valid || p_field.size() != n * n) {
		return 0.0f;
	}
	const float fx = p_position.x / spray.cell - spray.origin.x - 0.5f;
	const float fz = p_position.z / spray.cell - spray.origin.y - 0.5f;
	if (fx < -0.5f || fz < -0.5f || fx > n - 0.5f || fz > n - 0.5f) {
		return 0.0f;
	}
	const int x0 = CLAMP(int(Math::floor(fx)), 0, n - 1), z0 = CLAMP(int(Math::floor(fz)), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1), z1 = MIN(z0 + 1, n - 1);
	const float tx = CLAMP(fx - x0, 0.0f, 1.0f), tz = CLAMP(fz - z0, 0.0f, 1.0f);
	const float *f = p_field.ptr();
	return Math::lerp(Math::lerp(f[z0 * n + x0], f[z0 * n + x1], tx), Math::lerp(f[z1 * n + x0], f[z1 * n + x1], tx), tz);
}

float WeatherMap3D::get_sea_spray_at(Vector3 p_position) const {
	return MAX(_spray_sample(spray.rate, p_position), 0.0f);
}

float WeatherMap3D::get_spray_wetness_at(Vector3 p_position) const {
	return CLAMP(_spray_sample(spray.film, p_position) / MAX(surface_storage, 1e-3f), 0.0f, 1.0f);
}

float WeatherMap3D::get_salt_at(Vector3 p_position) const {
	return MAX(_spray_sample(spray.salt, p_position), 0.0f);
}

float WeatherMap3D::get_salt_deposition_at(Vector3 p_position) const {
	return MAX(_spray_sample(spray.salt_rate, p_position), 0.0f);
}

void WeatherMap3D::set_sea_spray(bool p_enabled) {
	sea_spray = p_enabled;
}

void WeatherMap3D::set_sea_spray_extent(float p_extent) {
	sea_spray_extent = MAX(p_extent, 16.0f);
}

void WeatherMap3D::set_sea_spray_resolution(int p_resolution) {
	sea_spray_resolution = CLAMP(p_resolution, 8, 1024);
}

void WeatherMap3D::clear_salt() {
	spray.salt.fill(0.0f);
	spray.film.fill(0.0f);
}

void WeatherMap3D::_bind_spray_methods() {
	ClassDB::bind_method(D_METHOD("get_sea_spray_at", "position"), &WeatherMap3D::get_sea_spray_at);
	ClassDB::bind_method(D_METHOD("get_spray_wetness_at", "position"), &WeatherMap3D::get_spray_wetness_at);
	ClassDB::bind_method(D_METHOD("get_salt_at", "position"), &WeatherMap3D::get_salt_at);
	ClassDB::bind_method(D_METHOD("get_salt_deposition_at", "position"), &WeatherMap3D::get_salt_deposition_at);
	ClassDB::bind_method(D_METHOD("get_sea_spray_rect"), &WeatherMap3D::get_sea_spray_rect);
	ClassDB::bind_method(D_METHOD("clear_salt"), &WeatherMap3D::clear_salt);
	ClassDB::bind_method(D_METHOD("set_sea_spray", "enabled"), &WeatherMap3D::set_sea_spray);
	ClassDB::bind_method(D_METHOD("is_sea_spray_enabled"), &WeatherMap3D::is_sea_spray_enabled);
	ClassDB::bind_method(D_METHOD("set_sea_spray_extent", "extent"), &WeatherMap3D::set_sea_spray_extent);
	ClassDB::bind_method(D_METHOD("get_sea_spray_extent"), &WeatherMap3D::get_sea_spray_extent);
	ClassDB::bind_method(D_METHOD("set_sea_spray_resolution", "resolution"), &WeatherMap3D::set_sea_spray_resolution);
	ClassDB::bind_method(D_METHOD("get_sea_spray_resolution"), &WeatherMap3D::get_sea_spray_resolution);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sea_spray"), "set_sea_spray", "is_sea_spray_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sea_spray_extent", PROPERTY_HINT_RANGE, "16,4096,1,suffix:m"), "set_sea_spray_extent", "get_sea_spray_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sea_spray_resolution", PROPERTY_HINT_RANGE, "8,1024,1"), "set_sea_spray_resolution", "get_sea_spray_resolution");
}

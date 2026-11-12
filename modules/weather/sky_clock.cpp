/**************************************************************************/
/*  sky_clock.cpp                                                         */
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

#include "sky_clock.h"

#include "atmosphere.h"
#include "weather_sky_material.h"

#include "core/config/engine.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"

namespace {

constexpr double DEG = Math::PI / 180.0;
constexpr double AU_KM = 149597870.7;
// Solar illuminance at the top of the atmosphere at 1 AU (Darula, Kittler and Gueymard 2005).
constexpr double SOLAR_LUX = 133800.0;

double norm_deg(double p_deg) {
	double d = Math::fmod(p_deg, 360.0);
	return d < 0.0 ? d + 360.0 : d;
}

// Meeus table 47.A: the main periodic terms of the moon's longitude (1e-6 deg) and distance
// (1e-3 km), by multiples of D, M, M', F.
struct LrTerm {
	int8_t d, m, mp, f;
	int32_t l, r;
};
const LrTerm MOON_LR[] = {
	{ 0, 0, 1, 0, 6288774, -20905355 },
	{ 2, 0, -1, 0, 1274027, -3699111 },
	{ 2, 0, 0, 0, 658314, -2955968 },
	{ 0, 0, 2, 0, 213618, -569925 },
	{ 0, 1, 0, 0, -185116, 48888 },
	{ 0, 0, 0, 2, -114332, -3149 },
	{ 2, 0, -2, 0, 58793, 246158 },
	{ 2, -1, -1, 0, 57066, -152138 },
	{ 2, 0, 1, 0, 53322, -170733 },
	{ 2, -1, 0, 0, 45758, -204586 },
	{ 0, 1, -1, 0, -40923, -129620 },
	{ 1, 0, 0, 0, -34720, 108743 },
	{ 0, 1, 1, 0, -30383, 104755 },
	{ 2, 0, 0, -2, 15327, 10321 },
	{ 0, 0, 1, 2, -12528, 0 },
	{ 0, 0, 1, -2, 10980, 79661 },
	{ 4, 0, -1, 0, 10675, -34782 },
	{ 0, 0, 3, 0, 10034, -23210 },
	{ 4, 0, -2, 0, 8548, -21636 },
	{ 2, 1, -1, 0, -7888, 24208 },
	{ 2, 1, 0, 0, -6766, 30824 },
	{ 1, 0, -1, 0, -5163, -8379 },
	{ 1, 1, 0, 0, 4987, -16675 },
	{ 2, -1, 1, 0, 4036, -12831 },
	{ 2, 0, 2, 0, 3994, -10445 },
	{ 4, 0, 0, 0, 3861, -11650 },
	{ 2, 0, -3, 0, 3665, 14403 },
	{ 0, 1, -2, 0, -2689, -7003 },
	{ 2, 0, -1, 2, -2602, 0 },
	{ 2, -1, -2, 0, 2390, 10056 },
	{ 1, 0, 1, 0, -2348, 6322 },
	{ 2, -2, 0, 0, 2236, -9884 },
	{ 0, 1, 2, 0, -2120, 5751 },
	{ 0, 2, 0, 0, -2069, 0 },
	{ 2, -2, -1, 0, 2048, -4950 },
	{ 2, 0, 1, -2, -1773, 4130 },
	{ 2, 0, 0, 2, -1595, 0 },
	{ 4, -1, -1, 0, 1215, -3958 },
	{ 0, 0, 2, 2, -1110, 0 },
	{ 3, 0, -1, 0, -892, 3258 },
	{ 2, 1, 1, 0, -810, 2616 },
	{ 4, -1, -2, 0, 759, -1897 },
	{ 0, 2, -1, 0, -713, -2117 },
	{ 2, 2, -1, 0, -700, 2354 },
	{ 2, 1, -2, 0, 691, 0 },
	{ 2, -1, 0, -2, 596, 0 },
	{ 4, 0, 1, 0, 549, -1423 },
	{ 0, 0, 4, 0, 537, -1117 },
	{ 4, -1, 0, 0, 520, -1571 },
	{ 1, 0, -2, 0, -487, -1739 },
};

// Meeus table 47.B: the main terms of the moon's latitude (1e-6 deg).
struct BTerm {
	int8_t d, m, mp, f;
	int32_t b;
};
const BTerm MOON_B[] = {
	{ 0, 0, 0, 1, 5128122 },
	{ 0, 0, 1, 1, 280602 },
	{ 0, 0, 1, -1, 277693 },
	{ 2, 0, 0, -1, 173237 },
	{ 2, 0, -1, 1, 55413 },
	{ 2, 0, -1, -1, 46271 },
	{ 2, 0, 0, 1, 32573 },
	{ 0, 0, 2, 1, 17198 },
	{ 2, 0, 1, -1, 9266 },
	{ 0, 0, 2, -1, 8822 },
	{ 2, -1, 0, -1, 8216 },
	{ 2, 0, -2, -1, 4324 },
	{ 2, 0, 1, 1, 4200 },
	{ 2, 1, 0, -1, -3359 },
	{ 2, -1, -1, 1, 2463 },
	{ 2, -1, 0, 1, 2211 },
	{ 2, -1, -1, -1, 2065 },
	{ 0, 1, -1, -1, -1870 },
	{ 4, 0, -1, -1, 1828 },
	{ 0, 1, 0, 1, -1794 },
	{ 0, 0, 0, 3, -1749 },
	{ 0, 1, -1, 1, -1565 },
	{ 1, 0, 0, 1, -1491 },
	{ 0, 1, 1, 1, -1475 },
	{ 0, 1, 1, -1, -1410 },
	{ 0, 1, 0, -1, -1344 },
	{ 1, 0, 0, -1, -1335 },
	{ 0, 0, 3, 1, 1107 },
	{ 4, 0, 0, -1, 1021 },
	{ 4, 0, -1, 1, 833 },
};

void equatorial(double p_lon, double p_lat, double p_eps, double &r_ra, double &r_dec) {
	r_ra = Math::atan2(Math::sin(p_lon) * Math::cos(p_eps) - Math::tan(p_lat) * Math::sin(p_eps), Math::cos(p_lon));
	if (r_ra < 0.0) {
		r_ra += Math::TAU;
	}
	r_dec = Math::asin(Math::sin(p_lat) * Math::cos(p_eps) + Math::cos(p_lat) * Math::sin(p_eps) * Math::sin(p_lon));
}

// Apparent minus true altitude, rad (Saemundsson 1986, as Meeus 16.4), for a true altitude in rad.
double refraction_for_true(double p_alt) {
	const double h = p_alt / DEG;
	if (h < -1.9) {
		return 0.0; // well below the horizon: nothing to see, so nothing to bend
	}
	const double r_arcmin = 1.02 / Math::tan((h + 10.3 / (h + 5.11)) * DEG);
	return MAX(r_arcmin, 0.0) / 60.0 * DEG;
}

void alt_az(double p_ra, double p_dec, double p_lst, double p_lat, double &r_alt, double &r_az, double &r_hour_angle) {
	const double h = p_lst - p_ra;
	r_hour_angle = h;
	const double up = Math::sin(p_lat) * Math::sin(p_dec) + Math::cos(p_lat) * Math::cos(p_dec) * Math::cos(h);
	const double north = Math::cos(p_lat) * Math::sin(p_dec) - Math::sin(p_lat) * Math::cos(p_dec) * Math::cos(h);
	const double east = -Math::cos(p_dec) * Math::sin(h);
	r_alt = Math::asin(CLAMP(up, -1.0, 1.0));
	r_az = Math::atan2(east, north);
	if (r_az < 0.0) {
		r_az += Math::TAU;
	}
}

} // namespace

double SkyClock::julian_day(int p_year, int p_month, double p_day) {
	int y = p_year;
	int m = p_month;
	if (m <= 2) {
		y -= 1;
		m += 12;
	}
	const double a = Math::floor(y / 100.0);
	const double b = 2.0 - a + Math::floor(a / 4.0);
	return Math::floor(365.25 * (y + 4716)) + Math::floor(30.6001 * (m + 1)) + p_day + b - 1524.5;
}

void SkyClock::calendar_from_jd(double p_jd, int &r_year, int &r_month, double &r_day) {
	const double z = Math::floor(p_jd + 0.5);
	const double f = p_jd + 0.5 - z;
	double a = z;
	if (z >= 2299161.0) {
		const double alpha = Math::floor((z - 1867216.25) / 36524.25);
		a = z + 1.0 + alpha - Math::floor(alpha / 4.0);
	}
	const double b = a + 1524.0;
	const double c = Math::floor((b - 122.1) / 365.25);
	const double d = Math::floor(365.25 * c);
	const double e = Math::floor((b - d) / 30.6001);
	r_day = b - d - Math::floor(30.6001 * e) + f;
	r_month = (int)(e < 14.0 ? e - 1.0 : e - 13.0);
	r_year = (int)(r_month > 2 ? c - 4716.0 : c - 4715.0);
}

double SkyClock::delta_t(double p_year) {
	const double t = p_year - 2000.0;
	if (p_year >= 1986.0 && p_year < 2005.0) {
		return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t + 0.000651814 * t * t * t * t + 0.00002373599 * t * t * t * t * t;
	}
	if (p_year >= 2005.0 && p_year < 2050.0) {
		return 62.92 + 0.32217 * t + 0.005589 * t * t;
	}
	const double u = (p_year - 1820.0) / 100.0;
	if (p_year >= 2050.0 && p_year < 2150.0) {
		return -20.0 + 32.0 * u * u - 0.5628 * (2150.0 - p_year);
	}
	return -20.0 + 32.0 * u * u;
}

void SkyClock::ephemeris(double p_jd_tt, Body &r_sun, Body &r_moon, double &r_obliquity, double &r_nutation_lon) {
	const double t = (p_jd_tt - 2451545.0) / 36525.0;
	const double t2 = t * t;
	const double t3 = t2 * t;
	const double t4 = t3 * t;

	// Nutation and obliquity (Meeus ch. 22, the 0.5" series).
	const double omega = norm_deg(125.04452 - 1934.136261 * t + 0.0020708 * t2 + t3 / 450000.0) * DEG;
	const double l_sun = norm_deg(280.4665 + 36000.7698 * t) * DEG;
	const double l_moon = norm_deg(218.3165 + 481267.8813 * t) * DEG;
	const double dpsi = (-17.20 * Math::sin(omega) - 1.32 * Math::sin(2.0 * l_sun) - 0.23 * Math::sin(2.0 * l_moon) + 0.21 * Math::sin(2.0 * omega)) / 3600.0 * DEG;
	const double deps = (9.20 * Math::cos(omega) + 0.57 * Math::cos(2.0 * l_sun) + 0.10 * Math::cos(2.0 * l_moon) - 0.09 * Math::cos(2.0 * omega)) / 3600.0 * DEG;
	const double eps0 = (23.4392911 - (46.8150 * t + 0.00059 * t2 - 0.001813 * t3) / 3600.0) * DEG;
	const double eps = eps0 + deps;
	r_obliquity = eps;
	r_nutation_lon = dpsi;

	// The sun (Meeus ch. 25).
	{
		const double l0 = norm_deg(280.46646 + 36000.76983 * t + 0.0003032 * t2);
		const double m = norm_deg(357.52911 + 35999.05029 * t - 0.0001537 * t2) * DEG;
		const double e = 0.016708634 - 0.000042037 * t - 0.0000001267 * t2;
		const double c = (1.914602 - 0.004817 * t - 0.000014 * t2) * Math::sin(m) + (0.019993 - 0.000101 * t) * Math::sin(2.0 * m) + 0.000289 * Math::sin(3.0 * m);
		const double true_lon = l0 + c;
		const double nu = m + c * DEG;
		const double r_au = 1.000001018 * (1.0 - e * e) / (1.0 + e * Math::cos(nu));
		// Apparent: nutation and aberration (20.4898" / R).
		const double lon = true_lon * DEG + dpsi - (20.4898 / 3600.0) * DEG / r_au;
		r_sun.lon = Math::fposmod(lon, Math::TAU);
		r_sun.lat = 0.0;
		r_sun.dist_km = r_au * AU_KM;
		equatorial(r_sun.lon, 0.0, eps, r_sun.ra, r_sun.dec);
	}

	// The moon (Meeus ch. 47).
	{
		const double lp = norm_deg(218.3164477 + 481267.88123421 * t - 0.0015786 * t2 + t3 / 538841.0 - t4 / 65194000.0) * DEG;
		const double d = norm_deg(297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868.0 - t4 / 113065000.0) * DEG;
		const double m = norm_deg(357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000.0) * DEG;
		const double mp = norm_deg(134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699.0 - t4 / 14712000.0) * DEG;
		const double f = norm_deg(93.2720950 + 483202.0175233 * t - 0.0036539 * t2 - t3 / 3526000.0 + t4 / 863310000.0) * DEG;
		const double a1 = norm_deg(119.75 + 131.849 * t) * DEG;
		const double a2 = norm_deg(53.09 + 479264.290 * t) * DEG;
		const double a3 = norm_deg(313.45 + 481266.484 * t) * DEG;
		const double ecc = 1.0 - 0.002516 * t - 0.0000074 * t2;
		double sl = 0.0;
		double sr = 0.0;
		for (const LrTerm &k : MOON_LR) {
			const double arg = k.d * d + k.m * m + k.mp * mp + k.f * f;
			const double w = k.m == 0 ? 1.0 : (Math::abs(k.m) == 1 ? ecc : ecc * ecc);
			sl += w * k.l * Math::sin(arg);
			sr += w * k.r * Math::cos(arg);
		}
		double sb = 0.0;
		for (const BTerm &k : MOON_B) {
			const double arg = k.d * d + k.m * m + k.mp * mp + k.f * f;
			const double w = k.m == 0 ? 1.0 : (Math::abs(k.m) == 1 ? ecc : ecc * ecc);
			sb += w * k.b * Math::sin(arg);
		}
		sl += 3958.0 * Math::sin(a1) + 1962.0 * Math::sin(lp - f) + 318.0 * Math::sin(a2);
		sb += -2235.0 * Math::sin(lp) + 382.0 * Math::sin(a3) + 175.0 * Math::sin(a1 - f) + 175.0 * Math::sin(a1 + f) + 127.0 * Math::sin(lp - mp) - 115.0 * Math::sin(lp + mp);
		r_moon.lon = Math::fposmod(lp + sl * 1e-6 * DEG + dpsi, Math::TAU);
		r_moon.lat = sb * 1e-6 * DEG;
		r_moon.dist_km = 385000.56 + sr * 1e-3;
		equatorial(r_moon.lon, r_moon.lat, eps, r_moon.ra, r_moon.dec);
	}
}

SkyClock::Sky SkyClock::compute(double p_jd_ut, double p_latitude_deg, double p_longitude_deg, bool p_refraction) {
	Sky s;
	s.jd_ut = p_jd_ut;
	int y = 0;
	int mo = 0;
	double dd = 0.0;
	calendar_from_jd(p_jd_ut, y, mo, dd);
	s.jd_tt = p_jd_ut + delta_t(y + (mo - 0.5) / 12.0) / 86400.0;
	double dpsi = 0.0;
	ephemeris(s.jd_tt, s.sun, s.moon, s.obliquity, dpsi);

	// Apparent sidereal time (Meeus 12.4 plus the equation of the equinoxes).
	const double tu = (p_jd_ut - 2451545.0) / 36525.0;
	const double gmst = norm_deg(280.46061837 + 360.98564736629 * (p_jd_ut - 2451545.0) + 0.000387933 * tu * tu - tu * tu * tu / 38710000.0);
	s.lst = Math::fposmod(gmst * DEG + dpsi * Math::cos(s.obliquity) + p_longitude_deg * DEG, Math::TAU);

	const double lat = p_latitude_deg * DEG;
	double h_sun = 0.0;
	alt_az(s.sun.ra, s.sun.dec, s.lst, lat, s.sun_alt, s.sun_az, h_sun);
	// The sun's parallax (8.8") is left out.
	double h_moon = 0.0;
	alt_az(s.moon.ra, s.moon.dec, s.lst, lat, s.moon_alt, s.moon_az, h_moon);
	// The moon's parallax: seen from the ground it sits lower by up to its horizontal parallax.
	const double parallax = Math::asin(6378.14 / s.moon.dist_km);
	s.moon_alt -= Math::asin(Math::sin(parallax) * Math::cos(s.moon_alt));
	if (p_refraction) {
		s.sun_alt += refraction_for_true(s.sun_alt);
		s.moon_alt += refraction_for_true(s.moon_alt);
	}

	// The moon's phase angle (Meeus 48.2, 48.3).
	const double cos_psi = Math::cos(s.moon.lat) * Math::cos(s.moon.lon - s.sun.lon);
	const double psi = Math::acos(CLAMP(cos_psi, -1.0, 1.0));
	s.moon_phase_angle = Math::atan2(s.sun.dist_km * Math::sin(psi), s.moon.dist_km - s.sun.dist_km * Math::cos(psi));
	// Position angle of the bright limb (Meeus 48.5) and the parallactic angle (Meeus 14.1).
	const double da = s.sun.ra - s.moon.ra;
	s.moon_bright_limb = Math::atan2(Math::cos(s.sun.dec) * Math::sin(da), Math::sin(s.sun.dec) * Math::cos(s.moon.dec) - Math::cos(s.sun.dec) * Math::sin(s.moon.dec) * Math::cos(da));
	s.moon_parallactic = Math::atan2(Math::sin(h_moon), Math::tan(lat) * Math::cos(s.moon.dec) - Math::sin(s.moon.dec) * Math::cos(h_moon));
	return s;
}

Vector3 SkyClock::_alt_az_to_world(double p_alt, double p_az) const {
	const double off = north_offset * DEG;
	const Vector3 n_w(Math::sin(off), 0.0, -Math::cos(off));
	const Vector3 e_w(Math::cos(off), 0.0, Math::sin(off));
	const double ch = Math::cos(p_alt);
	return (n_w * (ch * Math::cos(p_az)) + e_w * (ch * Math::sin(p_az)) + Vector3(0.0, Math::sin(p_alt), 0.0)).normalized();
}

void SkyClock::_update() {
	if (!dirty) {
		return;
	}
	dirty = false;
	sky = compute(get_julian_day(), latitude, longitude, refraction);
}

double SkyClock::get_julian_day() const {
	return julian_day(year, month, day) + (time_of_day - utc_offset) / 24.0;
}

void SkyClock::set_julian_day(double p_jd_ut) {
	int y = 0;
	int m = 0;
	double d = 0.0;
	calendar_from_jd(p_jd_ut + utc_offset / 24.0, y, m, d);
	year = y;
	month = m;
	day = (int)Math::floor(d);
	time_of_day = (d - day) * 24.0;
	dirty = true;
	_apply_lights();
}

void SkyClock::advance(double p_seconds) {
	if (p_seconds == 0.0) {
		return;
	}
	set_julian_day(get_julian_day() + p_seconds / 86400.0);
}

void SkyClock::_apply_lights() {
	if (!is_inside_tree()) {
		return;
	}
	_update();
	const struct {
		const NodePath *path;
		double alt;
		double az;
		double radius;
	} bodies[2] = {
		{ &sun_light, sky.sun_alt, sky.sun_az, 0.267 * DEG },
		{ &moon_light, sky.moon_alt, sky.moon_az, 0.259 * DEG },
	};
	for (const auto &b : bodies) {
		if (b.path->is_empty()) {
			continue;
		}
		DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(*b.path));
		if (light == nullptr) {
			continue;
		}
		const Vector3 dir = _alt_az_to_world(b.alt, b.az);
		const Vector3 up = Math::abs(dir.y) > 0.999 ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
		Transform3D xf = light->get_global_transform();
		// A DirectionalLight3D shines along its -Z: from the body toward the scene.
		xf.basis = Basis::looking_at(-dir, up);
		light->set_global_transform(xf);
		if (hide_below_horizon) {
			light->set_visible(b.alt > -b.radius);
		}
		if (drive_light_energy) {
			// The light at the ground: the body above the air through the air's transmittance (the
			// atmosphere's own tables, so the light and the sky agree), coloured by what is left.
			const double top = b.path == &sun_light ? get_sun_illuminance_top() : get_moon_illuminance_top();
			const Vector3 t = air_transmittance(b.alt / DEG, light->get_global_position().y);
			const double peak = MAX(MAX(t.x, t.y), t.z);
			if (peak > 1e-6) {
				light->set_color(Color(t.x / peak, t.y / peak, t.z / peak).linear_to_srgb());
			}
			light->set_param(Light3D::PARAM_ENERGY, float(top * peak * energy_per_lux));
		}
	}
	// The sky's scattering atmosphere takes both bodies above the air.
	Ref<WeatherSkyMaterial> sky_mat = WeatherSkyMaterial::find_for(this);
	sky_material_id = sky_mat.is_valid() ? sky_mat->get_instance_id() : ObjectID();
	if (sky_mat.is_valid() && drive_light_energy) {
		const double e_sun = get_sun_illuminance_top() * energy_per_lux;
		const double e_moon = get_moon_illuminance_top() * energy_per_lux;
		sky_mat->set_atmosphere_lights(get_sun_direction(), Vector3(e_sun, e_sun, e_sun), get_moon_direction(), Vector3(e_moon, e_moon, e_moon));
	}
}

Vector3 SkyClock::air_transmittance(double p_altitude_deg, double p_height) {
	static const WeatherAtmosphere::Params params;
	const double r = params.bottom_radius + MAX(p_height, 1.0);
	return WeatherAtmosphere::transmittance(params, r, Math::sin(p_altitude_deg * DEG));
}

void SkyClock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Every frame: the sky may be set up after the clock (or swapped), and then takes the lights.
			set_process_internal(true);
			_apply_lights();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (time_scale != 0.0 && !Engine::get_singleton()->is_editor_hint()) {
				advance(get_process_delta_time() * time_scale);
			} else if (drive_light_energy) {
				Ref<WeatherSkyMaterial> sky_mat = WeatherSkyMaterial::find_for(this);
				if (sky_mat.is_valid() && sky_mat->get_instance_id() != sky_material_id) {
					_apply_lights();
				}
			}
		} break;
	}
}

#define SKY_CLOCK_SETTER(m_name, m_type, m_member) \
	void SkyClock::set_##m_name(m_type p_value) { \
		m_member = p_value; \
		dirty = true; \
		_apply_lights(); \
	}

SKY_CLOCK_SETTER(utc_offset, double, utc_offset)
SKY_CLOCK_SETTER(refraction, bool, refraction)
SKY_CLOCK_SETTER(hide_below_horizon, bool, hide_below_horizon)
SKY_CLOCK_SETTER(drive_light_energy, bool, drive_light_energy)
SKY_CLOCK_SETTER(energy_per_lux, double, energy_per_lux)

void SkyClock::set_year(int p_year) {
	year = p_year;
	dirty = true;
	_apply_lights();
}
void SkyClock::set_month(int p_month) {
	month = CLAMP(p_month, 1, 12);
	dirty = true;
	_apply_lights();
}
void SkyClock::set_day(int p_day) {
	day = CLAMP(p_day, 1, 31);
	dirty = true;
	_apply_lights();
}
void SkyClock::set_time_of_day(double p_hours) {
	time_of_day = CLAMP(p_hours, 0.0, 24.0);
	dirty = true;
	_apply_lights();
}
void SkyClock::set_latitude(double p_deg) {
	latitude = CLAMP(p_deg, -90.0, 90.0);
	dirty = true;
	_apply_lights();
}
void SkyClock::set_longitude(double p_deg) {
	longitude = CLAMP(p_deg, -180.0, 180.0);
	dirty = true;
	_apply_lights();
}
void SkyClock::set_north_offset(double p_deg) {
	north_offset = p_deg;
	_apply_lights();
}
void SkyClock::set_time_scale(double p_scale) {
	time_scale = p_scale;
}
void SkyClock::set_sun_light(const NodePath &p_path) {
	sun_light = p_path;
	_apply_lights();
}
void SkyClock::set_moon_light(const NodePath &p_path) {
	moon_light = p_path;
	_apply_lights();
}

Vector3 SkyClock::get_sun_direction() {
	_update();
	return _alt_az_to_world(sky.sun_alt, sky.sun_az);
}
Vector3 SkyClock::get_moon_direction() {
	_update();
	return _alt_az_to_world(sky.moon_alt, sky.moon_az);
}
double SkyClock::get_sun_altitude() {
	_update();
	return sky.sun_alt / DEG;
}
double SkyClock::get_sun_azimuth() {
	_update();
	return sky.sun_az / DEG;
}
double SkyClock::get_moon_altitude() {
	_update();
	return sky.moon_alt / DEG;
}
double SkyClock::get_moon_azimuth() {
	_update();
	return sky.moon_az / DEG;
}
double SkyClock::get_moon_phase_angle() {
	_update();
	return sky.moon_phase_angle / DEG;
}
double SkyClock::get_moon_illuminated_fraction() {
	_update();
	return 0.5 * (1.0 + Math::cos(sky.moon_phase_angle));
}
double SkyClock::get_moon_bright_limb_angle() {
	_update();
	return Math::fposmod(sky.moon_bright_limb - sky.moon_parallactic, Math::TAU) / DEG;
}
double SkyClock::get_moon_distance() {
	_update();
	return sky.moon.dist_km;
}
double SkyClock::get_sun_distance() {
	_update();
	return sky.sun.dist_km;
}
double SkyClock::get_local_sidereal_time() {
	_update();
	return sky.lst / DEG / 15.0;
}

Basis SkyClock::get_celestial_basis() {
	_update();
	const double off = north_offset * DEG;
	const Vector3 n_w(Math::sin(off), 0.0, -Math::cos(off));
	const Vector3 e_w(Math::cos(off), 0.0, Math::sin(off));
	const Vector3 up(0.0, 1.0, 0.0);
	const double sl = Math::sin(sky.lst);
	const double cl = Math::cos(sky.lst);
	const double sp = Math::sin(latitude * DEG);
	const double cp = Math::cos(latitude * DEG);
	// Columns: where the equator's x (RA 0h), y (RA 6h) and the north celestial pole land.
	const Vector3 cx = e_w * -sl + up * (cp * cl) + n_w * (-sp * cl);
	const Vector3 cy = e_w * cl + up * (cp * sl) + n_w * (-sp * sl);
	const Vector3 cz = up * sp + n_w * cp;
	Basis b;
	b.set_columns(cx, cy, cz);
	return b;
}

double SkyClock::get_moon_magnitude() {
	_update();
	// Allen's Astrophysical Quantities: V = -12.73 + 0.026 |a| + 4e-9 a^4 at the mean distance.
	const double a = Math::abs(sky.moon_phase_angle / DEG);
	const double m = -12.73 + 0.026 * a + 4e-9 * a * a * a * a;
	return m + 5.0 * Math::log(sky.moon.dist_km / 385000.56) / Math::log(10.0);
}
double SkyClock::get_sun_illuminance_top() {
	_update();
	const double r = sky.sun.dist_km / AU_KM;
	return SOLAR_LUX / (r * r);
}
double SkyClock::get_moon_illuminance_top() {
	// Visual magnitude to illuminance: E = 10^(-0.4 (m + 14.18)) lux (full moon about 0.26 lux).
	return Math::pow(10.0, -0.4 * (get_moon_magnitude() + 14.18));
}

Dictionary SkyClock::get_ephemeris() {
	_update();
	Dictionary d;
	d["jd_ut"] = sky.jd_ut;
	d["jd_tt"] = sky.jd_tt;
	d["sun_lon"] = sky.sun.lon / DEG;
	d["sun_ra"] = sky.sun.ra / DEG;
	d["sun_dec"] = sky.sun.dec / DEG;
	d["sun_distance_km"] = sky.sun.dist_km;
	d["moon_lon"] = sky.moon.lon / DEG;
	d["moon_lat"] = sky.moon.lat / DEG;
	d["moon_ra"] = sky.moon.ra / DEG;
	d["moon_dec"] = sky.moon.dec / DEG;
	d["moon_distance_km"] = sky.moon.dist_km;
	d["obliquity"] = sky.obliquity / DEG;
	d["lst_hours"] = sky.lst / DEG / 15.0;
	d["sun_alt"] = sky.sun_alt / DEG;
	d["sun_az"] = sky.sun_az / DEG;
	d["moon_alt"] = sky.moon_alt / DEG;
	d["moon_az"] = sky.moon_az / DEG;
	d["moon_phase_angle"] = sky.moon_phase_angle / DEG;
	return d;
}

void SkyClock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_year", "year"), &SkyClock::set_year);
	ClassDB::bind_method(D_METHOD("get_year"), &SkyClock::get_year);
	ClassDB::bind_method(D_METHOD("set_month", "month"), &SkyClock::set_month);
	ClassDB::bind_method(D_METHOD("get_month"), &SkyClock::get_month);
	ClassDB::bind_method(D_METHOD("set_day", "day"), &SkyClock::set_day);
	ClassDB::bind_method(D_METHOD("get_day"), &SkyClock::get_day);
	ClassDB::bind_method(D_METHOD("set_time_of_day", "hours"), &SkyClock::set_time_of_day);
	ClassDB::bind_method(D_METHOD("get_time_of_day"), &SkyClock::get_time_of_day);
	ClassDB::bind_method(D_METHOD("set_utc_offset", "hours"), &SkyClock::set_utc_offset);
	ClassDB::bind_method(D_METHOD("get_utc_offset"), &SkyClock::get_utc_offset);
	ClassDB::bind_method(D_METHOD("set_latitude", "degrees"), &SkyClock::set_latitude);
	ClassDB::bind_method(D_METHOD("get_latitude"), &SkyClock::get_latitude);
	ClassDB::bind_method(D_METHOD("set_longitude", "degrees"), &SkyClock::set_longitude);
	ClassDB::bind_method(D_METHOD("get_longitude"), &SkyClock::get_longitude);
	ClassDB::bind_method(D_METHOD("set_north_offset", "degrees"), &SkyClock::set_north_offset);
	ClassDB::bind_method(D_METHOD("get_north_offset"), &SkyClock::get_north_offset);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &SkyClock::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &SkyClock::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_refraction", "enable"), &SkyClock::set_refraction);
	ClassDB::bind_method(D_METHOD("get_refraction"), &SkyClock::get_refraction);
	ClassDB::bind_method(D_METHOD("set_sun_light", "path"), &SkyClock::set_sun_light);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &SkyClock::get_sun_light);
	ClassDB::bind_method(D_METHOD("set_moon_light", "path"), &SkyClock::set_moon_light);
	ClassDB::bind_method(D_METHOD("get_moon_light"), &SkyClock::get_moon_light);
	ClassDB::bind_method(D_METHOD("set_hide_below_horizon", "hide"), &SkyClock::set_hide_below_horizon);
	ClassDB::bind_method(D_METHOD("get_hide_below_horizon"), &SkyClock::get_hide_below_horizon);

	ClassDB::bind_static_method("SkyClock", D_METHOD("julian_day", "year", "month", "day"), &SkyClock::julian_day);
	ClassDB::bind_method(D_METHOD("advance", "seconds"), &SkyClock::advance);
	ClassDB::bind_method(D_METHOD("get_julian_day"), &SkyClock::get_julian_day);
	ClassDB::bind_method(D_METHOD("set_julian_day", "jd_ut"), &SkyClock::set_julian_day);
	ClassDB::bind_method(D_METHOD("get_sun_direction"), &SkyClock::get_sun_direction);
	ClassDB::bind_method(D_METHOD("get_moon_direction"), &SkyClock::get_moon_direction);
	ClassDB::bind_method(D_METHOD("get_sun_altitude"), &SkyClock::get_sun_altitude);
	ClassDB::bind_method(D_METHOD("get_sun_azimuth"), &SkyClock::get_sun_azimuth);
	ClassDB::bind_method(D_METHOD("get_moon_altitude"), &SkyClock::get_moon_altitude);
	ClassDB::bind_method(D_METHOD("get_moon_azimuth"), &SkyClock::get_moon_azimuth);
	ClassDB::bind_method(D_METHOD("get_moon_phase_angle"), &SkyClock::get_moon_phase_angle);
	ClassDB::bind_method(D_METHOD("get_moon_illuminated_fraction"), &SkyClock::get_moon_illuminated_fraction);
	ClassDB::bind_method(D_METHOD("get_moon_bright_limb_angle"), &SkyClock::get_moon_bright_limb_angle);
	ClassDB::bind_method(D_METHOD("get_moon_distance"), &SkyClock::get_moon_distance);
	ClassDB::bind_method(D_METHOD("get_sun_distance"), &SkyClock::get_sun_distance);
	ClassDB::bind_method(D_METHOD("get_local_sidereal_time"), &SkyClock::get_local_sidereal_time);
	ClassDB::bind_method(D_METHOD("get_celestial_basis"), &SkyClock::get_celestial_basis);
	ClassDB::bind_method(D_METHOD("get_moon_magnitude"), &SkyClock::get_moon_magnitude);
	ClassDB::bind_method(D_METHOD("get_sun_illuminance_top"), &SkyClock::get_sun_illuminance_top);
	ClassDB::bind_method(D_METHOD("get_moon_illuminance_top"), &SkyClock::get_moon_illuminance_top);
	ClassDB::bind_method(D_METHOD("get_ephemeris"), &SkyClock::get_ephemeris);

	ADD_GROUP("Date and Time", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "year", PROPERTY_HINT_RANGE, "1900,2150,1"), "set_year", "get_year");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "month", PROPERTY_HINT_RANGE, "1,12,1"), "set_month", "get_month");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "day", PROPERTY_HINT_RANGE, "1,31,1"), "set_day", "get_day");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_of_day", PROPERTY_HINT_RANGE, "0,24,0.001,suffix:h"), "set_time_of_day", "get_time_of_day");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "utc_offset", PROPERTY_HINT_RANGE, "-12,14,0.25,suffix:h"), "set_utc_offset", "get_utc_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "-3600,3600,0.1,or_greater,or_less"), "set_time_scale", "get_time_scale");
	ADD_GROUP("Place", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "latitude", PROPERTY_HINT_RANGE, "-90,90,0.0001,suffix:deg"), "set_latitude", "get_latitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "longitude", PROPERTY_HINT_RANGE, "-180,180,0.0001,suffix:deg"), "set_longitude", "get_longitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "north_offset", PROPERTY_HINT_RANGE, "-180,180,0.1,suffix:deg"), "set_north_offset", "get_north_offset");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "refraction"), "set_refraction", "get_refraction");
	ADD_GROUP("Lights", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "moon_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_moon_light", "get_moon_light");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hide_below_horizon"), "set_hide_below_horizon", "get_hide_below_horizon");
	ClassDB::bind_method(D_METHOD("set_drive_light_energy", "drive"), &SkyClock::set_drive_light_energy);
	ClassDB::bind_method(D_METHOD("get_drive_light_energy"), &SkyClock::get_drive_light_energy);
	ClassDB::bind_method(D_METHOD("set_energy_per_lux", "scale"), &SkyClock::set_energy_per_lux);
	ClassDB::bind_method(D_METHOD("get_energy_per_lux"), &SkyClock::get_energy_per_lux);
	ClassDB::bind_static_method("SkyClock", D_METHOD("air_transmittance", "altitude_degrees", "height"), &SkyClock::air_transmittance);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drive_light_energy"), "set_drive_light_energy", "get_drive_light_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_per_lux", PROPERTY_HINT_RANGE, "0,0.001,0.0000001,or_greater"), "set_energy_per_lux", "get_energy_per_lux");
}

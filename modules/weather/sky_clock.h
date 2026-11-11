/**************************************************************************/
/*  sky_clock.h                                                           */
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

#include "core/math/basis.h"
#include "core/math/vector3.h"
#include "scene/main/node.h"

// The sky's clock (docs/ideas/day-night.md): a date, a time and a place on Earth give the
// directions of the sun and the moon, the moon's phase and which way its lit side faces, and
// the rotation of the stars. Positions follow Meeus, Astronomical Algorithms (2nd ed.): the sun
// from chapter 25 (about 0.01 deg), the moon from chapter 47's main periodic terms (well under
// 0.05 deg, the moon's disc is 0.5 deg), sidereal time from chapter 12, the moon's phase from
// chapter 48, its bright limb from chapter 48 and the parallactic angle from chapter 14.
// Topocentric: the moon's parallax (up to 1 deg) and atmospheric refraction (Bennett 1982) are
// applied. World axes: +Y up, -Z north (turned by north_offset), +X east.
class SkyClock : public Node {
	GDCLASS(SkyClock, Node);

public:
	struct Body {
		double ra = 0.0; // right ascension, rad (apparent, of date)
		double dec = 0.0; // declination, rad
		double lon = 0.0; // ecliptic longitude, rad
		double lat = 0.0; // ecliptic latitude, rad
		double dist_km = 0.0;
	};
	struct Sky {
		double jd_ut = 0.0;
		double jd_tt = 0.0;
		Body sun;
		Body moon;
		double lst = 0.0; // local apparent sidereal time, rad
		double obliquity = 0.0; // true obliquity, rad
		double sun_alt = 0.0; // topocentric with refraction, rad
		double sun_az = 0.0; // from north, through east, rad
		double moon_alt = 0.0;
		double moon_az = 0.0;
		double moon_phase_angle = 0.0; // rad, 0 = full
		double moon_bright_limb = 0.0; // position angle of the bright limb, from celestial north through east, rad
		double moon_parallactic = 0.0; // rad
	};

private:
	int year = 2026;
	int month = 6;
	int day = 21;
	double time_of_day = 12.0; // local clock hours
	double utc_offset = 0.0; // hours
	double latitude = 39.4; // deg, north positive
	double longitude = -9.5; // deg, east positive
	double north_offset = 0.0; // deg: world -Z turned this much toward +X is north
	double time_scale = 0.0; // simulated seconds per real second (0 = stopped)
	bool refraction = true;
	NodePath sun_light;
	NodePath moon_light;
	bool hide_below_horizon = true;

	Sky sky;
	bool dirty = true;

	void _update();
	void _apply_lights();
	Vector3 _alt_az_to_world(double p_alt, double p_az) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Julian day (UT) of a civil date and time (Meeus ch. 7; Gregorian).
	static double julian_day(int p_year, int p_month, double p_day);
	static void calendar_from_jd(double p_jd, int &r_year, int &r_month, double &r_day);
	// TT - UT in seconds (Espenak and Meeus 2006 polynomials, 2005-2150).
	static double delta_t(double p_year);
	// Geocentric apparent sun and moon, true obliquity (TT).
	static void ephemeris(double p_jd_tt, Body &r_sun, Body &r_moon, double &r_obliquity, double &r_nutation_lon);
	static Sky compute(double p_jd_ut, double p_latitude_deg, double p_longitude_deg, bool p_refraction);

	void set_year(int p_year);
	int get_year() const { return year; }
	void set_month(int p_month);
	int get_month() const { return month; }
	void set_day(int p_day);
	int get_day() const { return day; }
	void set_time_of_day(double p_hours);
	double get_time_of_day() const { return time_of_day; }
	void set_utc_offset(double p_hours);
	double get_utc_offset() const { return utc_offset; }
	void set_latitude(double p_deg);
	double get_latitude() const { return latitude; }
	void set_longitude(double p_deg);
	double get_longitude() const { return longitude; }
	void set_north_offset(double p_deg);
	double get_north_offset() const { return north_offset; }
	void set_time_scale(double p_scale);
	double get_time_scale() const { return time_scale; }
	void set_refraction(bool p_enable);
	bool get_refraction() const { return refraction; }
	void set_sun_light(const NodePath &p_path);
	NodePath get_sun_light() const { return sun_light; }
	void set_moon_light(const NodePath &p_path);
	NodePath get_moon_light() const { return moon_light; }
	void set_hide_below_horizon(bool p_hide);
	bool get_hide_below_horizon() const { return hide_below_horizon; }

	void advance(double p_seconds);
	double get_julian_day() const;
	void set_julian_day(double p_jd_ut);

	Vector3 get_sun_direction();
	Vector3 get_moon_direction();
	double get_sun_altitude();
	double get_sun_azimuth();
	double get_moon_altitude();
	double get_moon_azimuth();
	double get_moon_phase_angle();
	double get_moon_illuminated_fraction();
	double get_moon_bright_limb_angle();
	double get_moon_distance();
	double get_sun_distance();
	double get_local_sidereal_time();
	Basis get_celestial_basis();
	double get_moon_magnitude();
	double get_sun_illuminance_top();
	double get_moon_illuminance_top();
	Dictionary get_ephemeris();
};

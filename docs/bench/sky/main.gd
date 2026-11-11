extends Node3D
# Sky bench (docs/ideas/day-night.md).
#   --test   check SkyClock against Meeus' worked examples and known sky facts, print PASS/FAIL, quit

var fails := 0


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	if args.has("--test"):
		_test()
		get_tree().quit(1 if fails > 0 else 0)


func check(ok: bool, what: String) -> void:
	print(("[sky] PASS  " if ok else "[sky] FAIL  ") + what)
	if not ok:
		fails += 1


func ang(a: float, b: float) -> float:
	return absf(wrapf(a - b, -180.0, 180.0))


# The clock at a Julian day in TT (the worked examples are in dynamical time).
func at_tt(c: SkyClock, jd_tt: float) -> Dictionary:
	c.set_julian_day(jd_tt)
	var e: Dictionary = c.get_ephemeris()
	c.set_julian_day(jd_tt - (e["jd_tt"] - e["jd_ut"]))
	return c.get_ephemeris()


func _test() -> void:
	var c := SkyClock.new()
	add_child(c)
	c.refraction = false
	# Calendar (Meeus 7.a, 7.c).
	check(absf(SkyClock.julian_day(1957, 10, 4.81) - 2436116.31) < 1e-6, "Julian day of 1957 October 4.81 is 2436116.31")
	check(absf(SkyClock.julian_day(2000, 1, 1.5) - 2451545.0) < 1e-6, "J2000.0 is JD 2451545.0")
	# The sun, Meeus example 25.a: 1992 October 13.0 TD.
	var e := at_tt(c, 2448908.5)
	print("[sky] sun 1992-10-13: lon %.5f ra %.5f dec %.5f R %.5f AU" % [e["sun_lon"], e["sun_ra"], e["sun_dec"], e["sun_distance_km"] / 149597870.7])
	check(ang(e["sun_lon"], 199.90895) < 0.002, "sun's apparent longitude within 0.002 deg of Meeus 25.a (199.90895)")
	check(ang(e["sun_ra"], 198.38083) < 0.003 and absf(e["sun_dec"] + 7.78507) < 0.003, "sun's right ascension and declination within 0.003 deg of Meeus 25.a")
	# The moon, Meeus example 47.a: 1992 April 12.0 TD.
	e = at_tt(c, 2448724.5)
	print("[sky] moon 1992-04-12: lon %.6f lat %.6f dist %.1f km ra %.6f dec %.6f phase angle %.4f" % [e["moon_lon"], e["moon_lat"], e["moon_distance_km"], e["moon_ra"], e["moon_dec"], e["moon_phase_angle"]])
	check(ang(e["moon_lon"], 133.167265) < 0.01, "moon's apparent longitude within 0.01 deg of Meeus 47.a (133.167265)")
	check(absf(e["moon_lat"] + 3.229126) < 0.01, "moon's latitude within 0.01 deg of Meeus 47.a (-3.229126)")
	check(absf(e["moon_distance_km"] - 368409.7) < 20.0, "moon's distance within 20 km of Meeus 47.a (368409.7 km)")
	check(ang(e["moon_ra"], 134.688470) < 0.01 and absf(e["moon_dec"] - 13.768368) < 0.01, "moon's right ascension and declination within 0.01 deg of Meeus 47.a")
	# Moon phase, Meeus example 48.a (same instant): i = 69.0756 deg, k = 0.6786.
	check(absf(e["moon_phase_angle"] - 69.0756) < 0.05, "moon's phase angle within 0.05 deg of Meeus 48.a (69.0756)")
	check(absf(c.get_moon_illuminated_fraction() - 0.6786) < 0.001, "moon's illuminated fraction %.4f against 0.6786 (Meeus 48.a)" % c.get_moon_illuminated_fraction())
	# Sidereal time, Meeus example 12.a: 1987 April 10, 0h UT, Greenwich: 13h 10m 46.1351s apparent.
	c.longitude = 0.0
	c.utc_offset = 0.0
	c.set_julian_day(2446895.5)
	var lst: float = c.get_local_sidereal_time()
	var want := 13.0 + 10.0 / 60.0 + 46.1351 / 3600.0
	check(absf(lst - want) * 3600.0 < 0.1, "apparent sidereal time %.6f h against 13h10m46.1351s (Meeus 12.a), off %.3f s" % [lst, (lst - want) * 3600.0])
	# The sun's highest altitude at the June solstice: 90 - latitude + 23.44.
	c.latitude = 38.72
	c.longitude = -9.14
	c.utc_offset = 1.0
	c.year = 2026
	c.month = 6
	c.day = 21
	var best := -90.0
	var best_t := 0.0
	for k in 24 * 60:
		c.time_of_day = k / 60.0
		var a: float = c.get_sun_altitude()
		if a > best:
			best = a
			best_t = c.time_of_day
	print("[sky] Lisbon 2026-06-21: sun highest %.3f deg at %.3f h" % [best, best_t])
	check(absf(best - (90.0 - 38.72 + 23.437)) < 0.03, "Lisbon at the June solstice: sun's highest altitude %.3f deg (90 - 38.72 + 23.437 = 74.717)" % best)
	check(absf(best_t - 13.6) < 0.1, "Lisbon at the June solstice: local noon at %.2f h (solar noon about 13:36 in summer time)" % best_t)
	# The celestial basis puts the sun where the alt/az path does (no refraction).
	c.time_of_day = 9.5
	e = c.get_ephemeris()
	var ra := deg_to_rad(float(e["sun_ra"]))
	var dec := deg_to_rad(float(e["sun_dec"]))
	var eq := Vector3(cos(dec) * cos(ra), cos(dec) * sin(ra), sin(dec))
	var via_basis: Vector3 = c.get_celestial_basis() * eq
	check(rad_to_deg(via_basis.angle_to(c.get_sun_direction())) < 0.01, "celestial basis and sun direction agree (%.4f deg)" % rad_to_deg(via_basis.angle_to(c.get_sun_direction())))
	var sd := c.get_sun_direction()
	print("[sky] sun at 09:30: alt %.2f az %.2f dir %s" % [c.get_sun_altitude(), c.get_sun_azimuth(), sd])
	check(sd.x > 0.0, "morning sun is east of the meridian (+X)")
	# The lights: a DirectionalLight3D shines along its -Z, from the body toward the scene.
	var sun_l := DirectionalLight3D.new()
	sun_l.name = "Sun"
	add_child(sun_l)
	c.sun_light = c.get_path_to(sun_l)
	var shine := -sun_l.global_basis.z
	check(rad_to_deg(shine.angle_to(-c.get_sun_direction())) < 0.01 and sun_l.visible, "sun light shines from the sun (%.4f deg off)" % rad_to_deg(shine.angle_to(-c.get_sun_direction())))
	c.time_of_day = 2.0
	check(not sun_l.visible, "sun light is off at 02:00")
	# Full moon brightness: about 0.25 lux at the top of the atmosphere.
	var full_lux := 0.0
	var start: float = SkyClock.julian_day(2026, 11, 1.0)
	for k in 30 * 24:
		c.set_julian_day(start + k / 24.0)
		full_lux = maxf(full_lux, c.get_moon_illuminance_top())
	check(full_lux > 0.2 and full_lux < 0.35, "full moon gives %.3f lux above the air (Jensen 2001: about 0.25)" % full_lux)
	# Sunrise and sunset in Lisbon on 21 June 2026 (summer time): 06:12 and 21:05 (Observatorio
	# Astronomico de Lisboa almanac), when the sun's upper limb, lifted by refraction, meets the horizon.
	c.refraction = true
	c.year = 2026
	c.month = 6
	c.day = 21
	var rise := -1.0
	var set_t := -1.0
	var prev := -90.0
	for k in 24 * 60 * 6:
		c.time_of_day = k / 360.0
		var a: float = c.get_sun_altitude() + 0.267
		if prev < 0.0 and a >= 0.0:
			rise = c.time_of_day
		if prev >= 0.0 and a < 0.0:
			set_t = c.time_of_day
		prev = a
	print("[sky] Lisbon 2026-06-21: sunrise %02d:%04.1f sunset %02d:%04.1f" % [int(rise), fmod(rise, 1.0) * 60.0, int(set_t), fmod(set_t, 1.0) * 60.0])
	check(absf(rise - (6.0 + 12.0 / 60.0)) * 60.0 < 1.5 and absf(set_t - (21.0 + 5.0 / 60.0)) * 60.0 < 1.5, "Lisbon sunrise and sunset within 1.5 min of the almanac (06:12, 21:05)")
	c.time_of_day = rise
	var with_r: float = c.get_sun_altitude()
	c.refraction = false
	var without_r: float = c.get_sun_altitude()
	print("[sky] refraction at sunrise: %.3f deg" % (with_r - without_r))
	check(with_r - without_r > 0.45 and with_r - without_r < 0.65, "refraction at the horizon lifts the sun by about 0.57 deg")
	# Speed: the whole ephemeris.
	var t0 := Time.get_ticks_usec()
	for k in 1000:
		c.set_julian_day(start + k * 0.01)
		c.get_sun_direction()
	var us := (Time.get_ticks_usec() - t0) / 1000.0
	print("[sky] one update: %.1f us" % us)
	check(us < 50.0, "one clock update costs %.1f us" % us)
	print("[sky] %d failed" % fails)

extends Node3D
# World links bench (docs/ideas/world-links.md): checks, in one scene, that the world's systems read each other
# through their shared sources. Each check prints PASS or FAIL with its numbers; a link whose system is not in the
# build prints SKIP. Exits 1 on any FAIL.
#
#   godot --path docs/bench/links --headless -- --test
#
# Links checked:
#   air       no clock and no climate: the air is the settings, exactly
#             the day's cycle: dawn coldest, afternoon warmest, the day's mean kept, humidity up at night
#             the season: a winter day colder than a summer one, from the climate
#             clouds narrow the day's range
#   water     rain fills the surface film and puddles; the cold turns rain to snow
#             the sun dries puddles faster than the shade of a canopy and than the night
#   lightning a tall mast takes the strike meant for the ground beside it; striking distance on IEC 62305
#   fire      a long continuing current lights dry fuel, not wet (PhysX)
#   contact   a body in world_contact flattens grass without being in grass_trample, and herbs where there is no grass

var fails := 0
var passes := 0
var skips := 0


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		passes += 1
		print("PASS %s  %s" % [name, detail])
	else:
		fails += 1
		print("FAIL %s  %s" % [name, detail])


func _skip(name: String, why: String) -> void:
	skips += 1
	print("SKIP %s  %s" % [name, why])


func _ready() -> void:
	if not ClassDB.class_exists("WeatherMap3D"):
		push_error("This build has no weather module.")
		get_tree().quit(1)
		return
	var cam := Camera3D.new()
	cam.position = Vector3(0, 3, 6)
	add_child(cam)
	cam.make_current()
	await _air_tests()
	await _water_tests()
	await _lightning_tests()
	_fire_tests()
	await _contact_tests()
	print("links: %d passed, %d failed, %d skipped" % [passes, fails, skips])
	get_tree().quit(1 if fails > 0 else 0)


func _new_map() -> Node3D:
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 1024.0
	m.resolution = 64
	m.wind_speed = 2.0
	m.cloud_cover = 0.0
	m.cloud_variation = 0.0
	m.transition_time = 0.0
	add_child(m)
	return m


func _new_clock(month: int, hour: float) -> Node:
	var c: Node = ClassDB.instantiate("SkyClock")
	c.latitude = 46.0
	c.longitude = 0.0
	c.utc_offset = 0.0
	c.month = month
	c.day = 15
	c.time_of_day = hour
	c.time_scale = 0.0
	add_child(c)
	return c


func _air_at(m: Node3D, c: Node, hour: float) -> Vector2:
	c.time_of_day = hour
	m.snap_to_settings()
	return Vector2(m.get_air_temperature(), m.get_air_humidity())


func _air_tests() -> void:
	# No clock, no climate: exactly the settings, as before.
	var m := _new_map()
	m.temperature = 12.5
	m.humidity = 0.63
	m.snap_to_settings()
	await get_tree().physics_frame
	_check("air: settings without clock or climate", m.get_air_temperature() == 12.5 and is_equal_approx(m.get_air_humidity(), 0.63), "%.3f C, %.3f" % [m.get_air_temperature(), m.get_air_humidity()])

	# The day's cycle.
	var c := _new_clock(7, 12.0)
	m.sky_clock = m.get_path_to(c)
	m.humidity = 0.6
	await get_tree().physics_frame
	var sum := 0.0
	var lo := Vector2(1e9, 0)
	var hi := Vector2(-1e9, 0)
	var lo_h := 0.0
	var hi_h := 0.0
	for i in 48:
		var h := (i + 0.5) * 0.5
		var a := _air_at(m, c, h)
		sum += a.x
		if a.x < lo.x:
			lo = a
			lo_h = h
		if a.x > hi.x:
			hi = a
			hi_h = h
	var mean := sum / 48.0
	_check("air: coldest near dawn", lo_h >= 3.0 and lo_h <= 6.5, "min %.1f C at %.1f h" % [lo.x, lo_h])
	_check("air: warmest in the afternoon", hi_h >= 13.0 and hi_h <= 16.5, "max %.1f C at %.1f h" % [hi.x, hi_h])
	_check("air: range about daily_temperature_range", absf((hi.x - lo.x) - m.daily_temperature_range) < 1.5, "%.1f K against %.1f" % [hi.x - lo.x, m.daily_temperature_range])
	_check("air: the day's mean kept", absf(mean - 12.5) < 0.2, "%.2f C against 12.5" % mean)
	_check("air: humidity up at night", lo.y > hi.y + 0.15, "%.2f at dawn, %.2f in the afternoon" % [lo.y, hi.y])
	var dew := m.get_air_dew_point()
	_check("air: dew point held through the day", absf(dew - (_dew(12.5, 0.6))) < 0.3, "%.2f C" % dew)
	m.cloud_cover = 1.0
	var overcast := _air_at(m, c, 15.0).x - _air_at(m, c, 5.0).x
	_check("air: overcast narrows the range", overcast < 0.7 * (hi.x - lo.x), "%.1f K overcast" % overcast)
	m.cloud_cover = 0.0

	# The season, from the climate.
	var clim := Resource.new()
	clim.set_script(load("res://climate.gd"))
	m.climate = clim
	c.month = 1
	var winter := _air_at(m, c, 12.0).x
	c.month = 7
	var summer := _air_at(m, c, 12.0).x
	_check("air: winter colder than summer from the climate", summer - winter > 15.0, "%.1f C in January, %.1f C in July" % [winter, summer])
	m.climate = null
	m.queue_free()
	c.queue_free()
	await get_tree().physics_frame


func _dew(t: float, rh: float) -> float:
	var g := log(rh) + 17.62 * t / (243.12 + t)
	return 243.12 * g / (17.62 - g)


func _water_tests() -> void:
	var m := _new_map()
	m.temperature = 15.0
	m.humidity = 0.7
	m.rain_rate = 5.0
	m.cloud_cover = 1.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(3600.0)
	var p := Vector3(10, 0, 10)
	_check("water: rain fills the film and puddles", m.get_wetness_at(p) > 0.9 and m.get_puddle_at(p) > 0.5, "wetness %.2f, puddles %.2f mm" % [m.get_wetness_at(p), m.get_puddle_at(p)])
	_check("water: rain falls as snow in the cold", WeatherMap3D.rain_fraction(-5.0, 0.8) < 0.1 and WeatherMap3D.rain_fraction(8.0, 0.8) > 0.9, "")

	# Sun against shade against night: the same puddles under a summer noon sun, under a dense canopy, at night.
	m.rain_rate = 0.0
	m.cloud_cover = 0.0
	var c := _new_clock(7, 12.0)
	m.sky_clock = m.get_path_to(c)
	m.air_daily_cycle = false
	m.add_canopy(Vector2(-200, -200), 60.0, 20.0, 4.0)
	m.snap_to_settings()
	var sun_p := Vector3(200, 0, 200)
	var shade_p := Vector3(-200, 0, -200)
	var times := {}
	for case_name in ["noon", "night"]:
		c.time_of_day = 12.0 if case_name == "noon" else 0.0
		m.rain_rate = 5.0
		m.snap_to_settings()
		m.advance(3600.0)
		m.rain_rate = 0.0
		m.snap_to_settings()
		var t_sun := -1.0
		var t_shade := -1.0
		var hours := 0.0
		while hours < 72.0 and (t_sun < 0.0 or t_shade < 0.0):
			m.advance(600.0)
			hours += 600.0 / 3600.0
			if t_sun < 0.0 and m.get_puddle_at(sun_p) <= 0.0:
				t_sun = hours
			if t_shade < 0.0 and m.get_puddle_at(shade_p) <= 0.0:
				t_shade = hours
		times[case_name] = Vector2(t_sun, t_shade)
	var noon: Vector2 = times["noon"]
	var night: Vector2 = times["night"]
	print("  puddles gone: noon sun %.1f h, noon shade %.1f h, night %.1f h" % [noon.x, noon.y, night.x])
	if m.sun_drying:
		_check("water: sun dries puddles before the shade", noon.x > 0.0 and (noon.y < 0.0 or noon.x < 0.8 * noon.y), "%.1f h against %.1f h" % [noon.x, noon.y])
		_check("water: the noon sun dries before the night", noon.x > 0.0 and (night.x < 0.0 or noon.x < 0.8 * night.x), "%.1f h against %.1f h" % [noon.x, night.x])
	m.queue_free()
	c.queue_free()
	await get_tree().physics_frame


func _lightning_tests() -> void:
	if not ClassDB.class_exists("WeatherLightning3D"):
		_skip("lightning", "no WeatherLightning3D")
		return
	_check("lightning: striking distance 10 I^0.65", absf(WeatherLightning3D.striking_distance(30.0) - 91.2) < 1.0, "%.1f m at 30 kA" % WeatherLightning3D.striking_distance(30.0))
	var m := _new_map()
	m.snap_to_settings()
	# Flat ground and a 40 m mast 30 m from where the flash comes down.
	var ground := StaticBody3D.new()
	var gs := CollisionShape3D.new()
	var gb := BoxShape3D.new()
	gb.size = Vector3(2000, 1, 2000)
	gs.shape = gb
	ground.add_child(gs)
	ground.position = Vector3(0, -0.5, 0)
	add_child(ground)
	var mast := StaticBody3D.new()
	var ms := CollisionShape3D.new()
	var mb := BoxShape3D.new()
	mb.size = Vector3(1, 40, 1)
	ms.shape = mb
	mast.add_child(ms)
	mast.position = Vector3(30, 20, 0)
	add_child(mast)
	var l: Node3D = ClassDB.instantiate("WeatherLightning3D")
	add_child(l)
	await get_tree().physics_frame
	await get_tree().physics_frame
	var hit: Vector3 = l.find_strike_point(Vector3(0, 0, 0), 30.0)
	_check("lightning: the mast takes the strike", absf(hit.x - 30.0) < 8.0 and hit.y > 35.0, "landed at %s" % hit)
	var far: Vector3 = l.find_strike_point(Vector3(-300, 0, 0), 30.0)
	_check("lightning: open ground far from the mast", far.y < 2.0 and far.x < -150.0, "landed at %s" % far)
	l.queue_free()
	mast.queue_free()
	ground.queue_free()
	m.queue_free()
	await get_tree().physics_frame


func _fire_tests() -> void:
	if not ClassDB.class_exists("PhysXFireSpread3D"):
		_skip("fire: lightning ignition", "no PhysX in this build")
		return
	var dry: float = PhysXFireSpread3D.lightning_ignition_chance(0.15, 0.05, 0.25)
	var wet: float = PhysXFireSpread3D.lightning_ignition_chance(0.15, 0.3, 0.25)
	var short: float = PhysXFireSpread3D.lightning_ignition_chance(0.01, 0.05, 0.25)
	_check("fire: a continuing current lights dry fuel, not wet or brief", dry > 0.3 and wet == 0.0 and short == 0.0, "dry %.2f, wet %.2f, no continuing current %.2f" % [dry, wet, short])


func _contact_tests() -> void:
	if not ClassDB.class_exists("GrassField3D"):
		_skip("contact", "no GrassField3D")
		return
	var g: Node3D = ClassDB.instantiate("GrassField3D")
	add_child(g)
	var body := Node3D.new()
	body.add_to_group("world_contact")
	body.set_meta("trample_radius", 0.5)
	add_child(body)
	for i in 30:
		body.position = Vector3(0.1 * i, 0.2, 0)
		await get_tree().process_frame
	var t: Vector2 = g.get_trample_at(Vector3(1.5, 0, 0))
	_check("contact: a world_contact body flattens grass", t.length() > 0.1, "bend %.2f" % t.length())
	g.queue_free()
	await get_tree().process_frame
	# No grass: a VegetationField3D keeps the field the other plants read.
	if ClassDB.class_exists("VegetationField3D"):
		var v: Node3D = ClassDB.instantiate("VegetationField3D")
		v.populate_on_ready = false
		add_child(v)
		for i in 30:
			body.position = Vector3(0.1 * i, 0.2, 0)
			await get_tree().process_frame
		var tv: Vector2 = v.get_contact_trample_at(Vector3(1.5, 0, 0))
		_check("contact: plants bend under bodies where there is no grass", tv.length() > 0.1, "bend %.2f" % tv.length())
		v.queue_free()
	body.queue_free()

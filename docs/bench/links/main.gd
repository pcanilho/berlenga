extends Node3D
# World links bench (docs/ideas/world-links.md): checks, in one scene, that the world's systems read each other
# through their shared sources. Each check prints PASS or FAIL with its numbers; a link whose system is not in the
# build prints SKIP. Exits 1 on any FAIL.
#
#   godot --path docs/bench/links --headless -- --test
#   godot --path docs/bench/links -- --frost=out.png [--frost-off]   grass, a fern, a rock and TerrainMaterial3D ground
#   godot --path docs/bench/links -- --snowpress=out.png [--snow-off]   grass, a crop strip and ferns under a settled
#             snowpack (pressed flat and buried), or the same day with no snow
#   godot --path docs/bench/links -- --current=out.png [--current-off]   grass and rushes in a shallow stream running
#             left to right, laid over by its current, or the same stream still
#   godot --path docs/bench/links -- --shimmer=out.png [--shimmer-off] [--pt] [--clear] [--frames=N]   a fire in front
#             of a striped wall, its heat shimmer on or off (--clear: the gas drawn invisible, only the bending);
#             --clip=dir saves every frame after the warm-up; --turbulence=x overrides heat_shimmer_turbulence
#             after a clear night at -2 C (hoarfrost), or the same night with dew_and_frost off
#
# Links checked:
#   air       no clock and no climate: the air is the settings, exactly
#             the day's cycle: dawn coldest, afternoon warmest, the day's mean kept, humidity up at night
#             the season: a winter day colder than a summer one, from the climate
#             clouds narrow the day's range
#   water     rain fills the surface film and puddles; the cold turns rain to snow
#             the sun dries puddles faster than the shade of a canopy and than the night
#   dew       a clear calm night wets open ground with dew, and frosts it below freezing; clouds and a canopy keep it
#             dry; the morning sun melts the frost
#   lightning a tall mast takes the strike meant for the ground beside it; striking distance on IEC 62305
#   fire      a long continuing current lights dry fuel, not wet (PhysX)
#   contact   a body in world_contact flattens grass without being in grass_trample, and herbs where there is no grass
#   contracts the water contract picks the right water where several overlap (a pool in a hollow below the sea's
#             level, a body above the water); with no producer it answers -INF; the weather's defaults

# Stub producers for the water contract: a sea at 0 m everywhere, and a pool at -5 m over x < 0.
# A stream 30 cm deep over x < 0, running +z at 0.4 m/s.
class StreamStub extends WorldWater:
	func _height_at(p: Vector3) -> float:
		return 0.3 if p.x < 0.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(0, 0, 0.4) if p.x < 0.0 else Vector3.ZERO


class SeaStub extends WorldWater:
	func _height_at(_p: Vector3) -> float:
		return 0.0


# A pond 1 m deep over x < 0 holding what flows into it: 1000 m3, so 1 kg/s for 1 s is 1 g/L.
class SiltPondStub extends WorldWater:
	var load := Vector2()

	func _height_at(p: Vector3) -> float:
		return 1.0 if p.x < 0.0 else -INF

	func _add_sediment(_p: Vector3, mineral: float, ash: float) -> void:
		load += Vector2(mineral, ash) / 1000.0

	func _sediment_at(_p: Vector3) -> Vector2:
		return load


# A ground stub taking runoff loads.
class RunoffStub extends WorldGround:
	var ash := 0.0

	func _add_runoff_load(_p: Vector3, _mineral: float, a: float) -> void:
		ash += a


class PoolStub extends WorldWater:
	func _height_at(p: Vector3) -> float:
		return -5.0 if p.x < 0.0 else -INF

	func _flow_at(_p: Vector3) -> Vector3:
		return Vector3(0.5, 0, 0)


# Packed snow 10 cm deep over x > 0.
class SnowStub extends WorldSnow:
	func _depth_at(p: Vector3) -> float:
		return 0.1 if p.x > 0.0 else 0.0

	func _density_at(_p: Vector3) -> float:
		return 350.0


# A pond 0.5 m deep over 10 < x < 30, its surface water running +x at 0.5 m/s; snow of a set depth over
# -30 < x < -10.
class LeafPondStub extends WorldWater:
	func _height_at(p: Vector3) -> float:
		return 0.5 if p.x > 10.0 and p.x < 30.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(0.5, 0, 0) if p.x > 10.0 and p.x < 30.0 else Vector3.ZERO


class LeafSnowStub extends WorldSnow:
	var depth := 0.2

	func _depth_at(p: Vector3) -> float:
		return depth if p.x > -30.0 and p.x < -10.0 else 0.0


# A fire stub: 50 kW/m2 burning over x > 0.
class FireStub extends WorldFire:
	func _heat_at(p: Vector3) -> float:
		return 50000.0 if p.x > 0.0 else 0.0


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
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--frost="):
			await _frost_view(a.substr(8), OS.get_cmdline_user_args().has("--frost-off"))
			get_tree().quit()
			return
		if a.begins_with("--snowpress="):
			await _snow_press_view(a.substr(12), OS.get_cmdline_user_args().has("--snow-off"))
			get_tree().quit()
			return
		if a.begins_with("--current="):
			await _current_view(a.substr(10), OS.get_cmdline_user_args().has("--current-off"))
			get_tree().quit()
			return
		if a.begins_with("--shimmer="):
			await _shimmer_view(a.substr(10))
			get_tree().quit()
			return
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
	await _dew_tests()
	await _lightning_tests()
	_fire_tests()
	await _contact_tests()
	await _snow_fire_tests()
	await _leaf_kick_tests()
	await _leaf_surface_tests()
	await _washout_tests()
	_tyre_snow_tests()
	await _contract_tests()
	await _shimmer_tests()
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
	var dew: float = m.get_air_dew_point()
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
	# Open water lags the season: on an autumn dawn it is warmer than the air (steam fog over lakes).
	c.month = 10
	var dawn := _air_at(m, c, 5.0).x
	var water: float = m.get_water_temperature()
	_check("air: autumn water warmer than the dawn air", water - dawn > 3.0, "water %.1f C, dawn air %.1f C" % [water, dawn])
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

	# Sun against shade against night: the same wet film under a summer noon sun, under a dense canopy, at night.
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
		m.cloud_cover = 1.0
		m.snap_to_settings()
		m.advance(3600.0)
		m.rain_rate = 0.0
		m.cloud_cover = 0.0
		m.snap_to_settings()
		var t_sun := -1.0
		var t_shade := -1.0
		var hours := 0.0
		while hours < 72.0 and (t_sun < 0.0 or t_shade < 0.0):
			m.advance(600.0)
			hours += 600.0 / 3600.0
			if t_sun < 0.0 and m.get_wetness_at(sun_p) <= 0.05:
				t_sun = hours
			if t_shade < 0.0 and m.get_wetness_at(shade_p) <= 0.05:
				t_shade = hours
		times[case_name] = Vector2(t_sun, t_shade)
	var noon: Vector2 = times["noon"]
	var night: Vector2 = times["night"]
	print("  surface dry: noon sun %.1f h, noon shade %.1f h, night %.1f h" % [noon.x, noon.y, night.x])
	if m.sun_drying:
		_check("water: sun dries the surface before the shade", noon.x > 0.0 and (noon.y < 0.0 or noon.x < 0.8 * noon.y), "%.1f h against %.1f h" % [noon.x, noon.y])
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
	var dry: float = ClassDB.class_call_static("PhysXFireSpread3D", "lightning_ignition_chance", 0.15, 0.05, 0.25)
	var wet: float = ClassDB.class_call_static("PhysXFireSpread3D", "lightning_ignition_chance", 0.15, 0.3, 0.25)
	var short: float = ClassDB.class_call_static("PhysXFireSpread3D", "lightning_ignition_chance", 0.01, 0.05, 0.25)
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
	# A stream's current lays the sward downstream where it runs, nothing on the dry bank.
	WorldContracts.clear_overrides()
	WorldContracts.add_water_stub(StreamStub.new())
	# The current is resampled every 0.25 s: wait in time, not frames (headless frames are far shorter).
	await get_tree().create_timer(0.6).timeout
	await get_tree().process_frame
	var wet: Vector2 = g.get_trample_at(Vector3(-4, 0, 3))
	var dry: Vector2 = g.get_trample_at(Vector3(4, 0, 3))
	_check("current: a stream lays grass downstream, not on the bank", wet.y > 0.5 and absf(wet.x) < 0.05 and dry.length() < 0.01, "bend (%.2f, %.2f) in the stream, %.2f on the bank" % [wet.x, wet.y, dry.length()])
	WorldContracts.clear_overrides()
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


# Bodies kick up fallen leaves: a body walking through lying leaves throws them; one standing still does not.
func _leaf_kick_tests() -> void:
	if not ClassDB.class_exists("LeafFall3D"):
		_skip("leaves: bodies kick them up", "no LeafFall3D")
		return
	WorldContracts.clear_overrides()
	var lf: Node3D = ClassDB.instantiate("LeafFall3D")
	lf.auto_crowns = false
	add_child(lf)
	var q := QuadMesh.new()
	q.size = Vector2(0.06, 0.05)
	lf.add_kind(q, 0.06, 0.003, 0.0003, 0.5)
	for i in 12:
		lf.drop_leaf(Vector3(0.15 * i, 0.05, 0.0), 0)
	for i in 120:
		await get_tree().physics_frame
	var lying := 0
	for s in lf.get_leaf_states():
		if s.w > 0.5:
			lying += 1
	var walker := Node3D.new()
	walker.add_to_group("world_contact")
	walker.set_meta("trample_radius", 0.3)
	add_child(walker)
	walker.position = Vector3(-2, 0.4, 0)
	for i in 30:
		await get_tree().physics_frame
	var still: int = lf.get_kicked_count()
	for i in 180:
		walker.position = Vector3(-2.0 + 6.0 * i / 180.0, 0.4, 0)
		await get_tree().physics_frame
	var kicked: int = lf.get_kicked_count()
	_check("leaves: a walking body kicks up the leaves it passes, a still one none", lying >= 10 and still == 0 and kicked >= 8, "%d lying; %d kicked standing, %d walking through" % [lying, still, kicked])
	walker.queue_free()
	lf.queue_free()
	await get_tree().physics_frame


# Leaves land on water and snow (WorldWater, WorldSnow): on a pond they float and drift with its current; on
# snow they lie on its surface, are buried when more falls and set down on the ground when it melts.
func _leaf_surface_tests() -> void:
	if not ClassDB.class_exists("LeafFall3D"):
		_skip("leaves: water and snow", "no LeafFall3D")
		return
	WorldContracts.clear_overrides()
	WorldContracts.add_water_stub(LeafPondStub.new())
	var snow := LeafSnowStub.new()
	WorldContracts.set_snow(snow)
	var lf: Node3D = ClassDB.instantiate("LeafFall3D")
	lf.auto_crowns = false
	lf.lie_time = 3600.0
	add_child(lf)
	var q := QuadMesh.new()
	q.size = Vector2(0.06, 0.05)
	lf.add_kind(q, 0.06, 0.003, 0.0003, 0.5)
	for i in 6:
		lf.drop_leaf(Vector3(15.0 + 0.5 * i, 1.5, 0.0), 0)
		lf.drop_leaf(Vector3(-20.0 + 0.5 * i, 1.0, 0.0), 0)
	for i in 240:
		await get_tree().physics_frame
	var floating: int = lf.get_floating_count()
	var on_water_x := 0.0
	var on_snow_y := 0.0
	var nw := 0
	var ns := 0
	for st in lf.get_leaf_states():
		if st.x > 10.0:
			on_water_x += st.x
			nw += 1
		elif st.x < -10.0 and st.w > 0.5:
			on_snow_y += st.y
			ns += 1
	on_water_x /= maxf(nw, 1)
	on_snow_y /= maxf(ns, 1)
	for i in 120:
		await get_tree().physics_frame
	var later_x := 0.0
	for st in lf.get_leaf_states():
		if st.x > 10.0:
			later_x += st.x / maxf(nw, 1)
	var drift := later_x - on_water_x
	_check("leaves: on a pond they float and drift with the current", floating == 6 and drift > 0.6 and drift < 1.4, "%d floating, drifted %.2f m in 2 s on a 0.5 m/s current" % [floating, drift])
	snow.depth = 0.35
	for i in 10:
		await get_tree().physics_frame
	var buried: int = lf.get_buried_count()
	snow.depth = 0.0
	for i in 10:
		await get_tree().physics_frame
	var melted_y := 0.0
	for st in lf.get_leaf_states():
		if st.x < -10.0:
			melted_y = maxf(melted_y, st.y)
	_check("leaves: on snow they lie on top, more snow buries them, the thaw sets them on the ground", ns == 6 and absf(on_snow_y - 0.2) < 0.02 and buried == 6 and lf.get_buried_count() == 0 and melted_y < 0.02, "%d on snow at %.3f m, %d buried by 15 cm more, highest after the thaw %.3f m" % [ns, on_snow_y, buried, melted_y])
	lf.queue_free()
	WorldContracts.clear_overrides()
	await get_tree().physics_frame


# Tyres on snow (WorldSnow): grip falls to packed snow's, and chains win much of it back.
func _tyre_snow_tests() -> void:
	if not ClassDB.class_exists("PhysXVehicle3D"):
		_skip("tyres: snow grip", "no PhysX in this build")
		return
	WorldContracts.clear_overrides()
	WorldContracts.set_snow(SnowStub.new())
	var road: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", Vector3(-5, 0, 0), 10.0, 0)
	var on_snow: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", Vector3(5, 0, 0), 10.0, 0)
	var chains: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", Vector3(5, 0, 0), 10.0, 3)
	_check("tyres: packed snow takes a road tyre's grip down, chains bring much of it back", on_snow < 0.4 * road and chains > 1.8 * on_snow, "road %.2f, snow %.2f, snow with chains %.2f" % [road, on_snow, chains])
	WorldContracts.clear_overrides()


# Rain washes smoke and dust out of the air (below-cloud scavenging): the weather contract's rate, coarse dust far
# faster than fine smoke and snow faster than rain; the weather map's smoke haze thins under hours of rain.
class WashStub extends WorldWeather:
	func _rainfall_at(p: Vector3) -> float:
		return 10.0 if p.x < 0.0 else 0.0
	func _snowfall_at(p: Vector3) -> float:
		return 10.0 if p.x > 100.0 else 0.0


func _washout_tests() -> void:
	WorldContracts.clear_overrides()
	WorldContracts.set_weather(WashStub.new())
	var w: WorldWeather = WorldContracts.get_weather()
	var smoke_rain := w.washout_at(Vector3(-5, 0, 0), false)
	var dust_rain := w.washout_at(Vector3(-5, 0, 0), true)
	var dry := w.washout_at(Vector3(5, 0, 0), true)
	var smoke_snow := w.washout_at(Vector3(150, 0, 0), false)
	_check("washout: dust washes out of rain far faster than smoke, snow faster than rain, none when dry", dry == 0.0 and dust_rain > 20.0 * smoke_rain and smoke_snow > 3.0 * smoke_rain and smoke_rain > 1e-5 and smoke_rain < 2e-4, "per hour: smoke %.3f, dust %.2f, dry %.2f, smoke in snow %.3f" % [smoke_rain * 3600.0, dust_rain * 3600.0, dry * 3600.0, smoke_snow * 3600.0])
	WorldContracts.clear_overrides()
	# The haze: the same smoke over a wet and a dry map, an hour of heavy rain apart.
	var amounts := []
	var rains := []
	for rain in [0.0, 30.0]:
		var m := _new_map()
		m.extent = 40000.0
		m.wind_speed = 0.0
		m.temperature = 12.0
		m.humidity = 0.95
		m.cloud_cover = 1.0
		m.rain_rate = rain
		m.snap_to_settings()
		await get_tree().physics_frame
		m.advance(1.0)
		m.add_smoke(Vector3(0, 0, 0), 2.0e6)
		var before: float = m.get_smoke_at(Vector3())
		m.advance(3600.0)
		amounts.append(m.get_smoke_at(Vector3()) / maxf(before, 1e-9))
		rains.append(WorldContracts.get_weather().rainfall_at(Vector3()) if rain > 0.0 else 0.0)
		m.queue_free()
		await get_tree().physics_frame
	var ratio: float = amounts[1] / maxf(amounts[0], 1e-9)
	_check("washout: an hour of heavy rain thins the smoke haze, the contract reports the map's rain", ratio < 0.85 and ratio > 0.2 and rains[1] > 1.0, "dry keeps %.3f, wet %.3f of it (%.2f); contract rainfall %.1f mm/h" % [amounts[0], amounts[1], ratio, rains[1]])


# Fire on lying snow: the fire contract's heat melts the pack under it, whatever the air.
func _snow_fire_tests() -> void:
	if not ClassDB.class_exists("SnowCover3D"):
		_skip("snow: fire melts the pack", "no SnowCover3D")
		return
	WorldContracts.clear_overrides()
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "FireGround"
	add_child(ground_node)
	var m := _new_map()
	m.temperature = -4.0
	m.humidity = 0.9
	m.rain_rate = 3.0
	m.cloud_cover = 1.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(1.0)
	var snow: Node3D = ClassDB.instantiate("SnowCover3D")
	snow.extent = 64.0
	snow.resolution = 64
	snow.update_rate = 0.0001
	add_child(snow)
	snow.height_source = snow.get_path_to(ground_node)
	snow.follow_target = snow.get_path_to(ground_node)
	snow.refresh_heights()
	snow.advance(10.0 * 3600.0)
	m.rain_rate = 0.0
	m.snap_to_settings()
	m.advance(1.0)
	var before: float = snow.get_depth_at(Vector3(10, 0, 0))
	WorldContracts.set_fire(FireStub.new())
	snow.advance(120.0)
	var burning: float = snow.get_depth_at(Vector3(10, 0, 0))
	var beside: float = snow.get_depth_at(Vector3(-10, 0, 0))
	_check("snow: a fire melts the pack under it, not beside it, at -4 C", before > 0.05 and burning < 0.6 * beside and beside > 0.9 * before, "%.3f m before, %.3f m under the fire, %.3f m beside it" % [before, burning, beside])
	WorldContracts.clear_overrides()
	snow.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


func _contract_tests() -> void:
	WorldContracts.clear_overrides()
	var w: WorldWater = WorldContracts.get_water()
	if not w.has_provider():
		_check("contracts: no water producer gives -INF", is_inf(w.height_at(Vector3(0, -1, 0))) and w.height_at(Vector3.ZERO) < 0.0 and w.depth_at(Vector3.ZERO) == 0.0, "%f" % w.height_at(Vector3.ZERO))
	WorldContracts.add_water_stub(SeaStub.new())
	WorldContracts.add_water_stub(PoolStub.new())
	var out := w.height_at(Vector3(5, -2, 0))
	var in_pool := w.height_at(Vector3(-5, -7, 0))
	var between := w.height_at(Vector3(-5, -3, 0))
	var above := w.height_at(Vector3(-5, 3, 0))
	_check("contracts: a pool below the sea's level reads its own water", out == 0.0 and in_pool == -5.0 and between == 0.0 and above == 0.0, "sea side %.1f, in the pool %.1f, over the pool under the sea %.1f, above both %.1f" % [out, in_pool, between, above])
	_check("contracts: the flow comes from the water that answered", w.flow_at(Vector3(-5, -7, 0)).x == 0.5 and w.flow_at(Vector3(5, -2, 0)) == Vector3.ZERO, "")
	_check("contracts: depth under the surface", absf(w.depth_at(Vector3(-5, -7, 0)) - 2.0) < 1e-4, "%.2f m" % w.depth_at(Vector3(-5, -7, 0)))
	# A fire dries the surface water under it, through the fire contract.
	var m := _new_map()
	m.rain_rate = 5.0
	m.cloud_cover = 1.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(3600.0)
	m.rain_rate = 0.0
	m.snap_to_settings()
	WorldContracts.set_fire(FireStub.new())
	m.advance(3600.0)
	var burning: float = m.get_wetness_at(Vector3(200, 0, 0))
	var beside: float = m.get_wetness_at(Vector3(-200, 0, 0))
	_check("contracts: a fire dries the ground under it", burning < 0.05 and beside > 0.5, "wetness %.2f under the fire, %.2f beside it" % [burning, beside])
	# Burn age: an authored scar answers over its area; the most recent of overlapping burns wins; -1 elsewhere.
	WorldContracts.clear_overrides()
	var old_scar := WorldBurnScar3D.new()
	old_scar.age_years = 12.0
	old_scar.size = Vector2(100, 100)
	add_child(old_scar)
	var new_scar := WorldBurnScar3D.new()
	new_scar.age_years = 2.0
	new_scar.polygon = PackedVector2Array([Vector2(0, 0), Vector2(30, 0), Vector2(0, 30)])
	add_child(new_scar)
	var fa: WorldFire = WorldContracts.get_fire()
	# The change log: a scar moved reports its old and new area; a reader older than the log reads everything again.
	var gen := fa.get_burn_generation()
	new_scar.position = Vector3(100, 0, 0)
	await get_tree().process_frame # transform changes reach the node at the frame's flush
	var ch: Dictionary = fa.get_burn_changes_since(gen)
	var moved_ok: bool = fa.get_burn_generation() > gen and ch["complete"] and (ch["area"] as Rect2).has_point(Vector2(10, 10)) and (ch["area"] as Rect2).has_point(Vector2(110, 10))
	var none: Dictionary = fa.get_burn_changes_since(fa.get_burn_generation())
	for i in 70:
		fa.burn_changed(Rect2(0, 0, 1, 1))
	var lost: Dictionary = fa.get_burn_changes_since(gen)
	_check("contracts: burn change log covers a moved scar's old and new area; too old means read all", moved_ok and none["complete"] and not (none["area"] as Rect2).has_area() and not lost["complete"], "%s, %s" % [ch, lost])
	new_scar.position = Vector3.ZERO
	_check("contracts: no fire gives no plumes or smoke", fa.plumes().is_empty() and fa.smoke_sources().is_empty(), "%d, %d" % [fa.plumes().size(), fa.smoke_sources().size()])
	var a_old := fa.burn_age_at(Vector3(-20, 0, -20))
	var a_new := fa.burn_age_at(Vector3(5, 0, 5))
	var a_none := fa.burn_age_at(Vector3(200, 0, 0))
	_check("contracts: burn age from authored scars, the latest burn wins", a_old == 12.0 and a_new == 2.0 and a_none == -1.0, "%.0f, %.0f, %.0f years" % [a_old, a_new, a_none])
	old_scar.queue_free()
	new_scar.queue_free()
	m.queue_free()
	WorldContracts.clear_overrides()
	await get_tree().physics_frame
	# A waterfall answers inside its falling water, falling fast, and nowhere beside it.
	if ClassDB.class_exists("PhysXWaterfalls3D"):
		WorldContracts.clear_overrides()
		var cliff := Node3D.new()
		cliff.set_script(load("res://cliff_source.gd"))
		cliff.name = "Cliff"
		add_child(cliff)
		var falls: Node3D = ClassDB.instantiate("PhysXWaterfalls3D")
		add_child(falls)
		falls.source = falls.get_path_to(cliff)
		falls.rebuild()
		var wf: WorldWater = WorldContracts.get_water()
		var fastest := 0.0
		for xi in 40:
			for yi in 6:
				var q := Vector3(0.25 * xi, 5.0 + 4.0 * yi, 0.0)
				if is_finite(wf.height_at(q)):
					fastest = minf(fastest, wf.flow_at(q).y)
		var off_fall := wf.height_at(Vector3(5, 15, 30))
		_check("contracts: a waterfall's falling water answers the water contract, nothing beside it", fastest < -5.0 and is_inf(off_fall), "fastest %.1f m/s down; beside %f" % [fastest, off_fall])
		falls.queue_free()
		cliff.queue_free()
		await get_tree().process_frame
	# Runoff load: dropped quietly with no runoff producer, taken by a stub.
	var gr: WorldGround = WorldContracts.get_ground()
	var had_runoff := gr.has_runoff()
	gr.add_runoff_load(Vector3(5, 0, 0), 1.0, 1.0)
	var rs := RunoffStub.new()
	WorldContracts.set_ground(rs)
	WorldContracts.get_ground().add_runoff_load(Vector3(5, 0, 0), 0.0, 0.5)
	WorldContracts.get_ground().add_runoff_load(Vector3(6, 0, 0), 0.0, 0.25)
	_check("contracts: runoff loads reach the runoff producer (a stub here), none without one", rs.ash == 0.75 and rs.has_runoff() and not had_runoff, "%.2f kg ash" % rs.ash)
	WorldContracts.clear_overrides()
	# Sediment: a load goes to the water that answers where it enters, and that water reports it.
	WorldContracts.clear_overrides()
	var pond := SiltPondStub.new()
	WorldContracts.add_water_stub(pond)
	var ws: WorldWater = WorldContracts.get_water()
	ws.add_sediment(Vector3(-5, 0, 0), 0.0, 2.0)
	ws.add_sediment(Vector3(5, 0, 0), 9.0, 9.0) # no water there: dropped
	var sed := ws.sediment_at(Vector3(-5, 0, 0))
	_check("contracts: ash washed into a pond clouds it, none lands where there is no water", is_equal_approx(sed.y, 0.002) and sed.x == 0.0 and ws.sediment_at(Vector3(5, 0, 0)) == Vector2.ZERO, "%.4f kg/m3 ash, %.4f mineral" % [sed.y, sed.x])
	WorldContracts.clear_overrides()
	var wx: WorldWeather = WorldContracts.get_weather()
	if not wx.has_provider():
		_check("contracts: weather defaults with no producer", wx.air_temperature_at(Vector3.ZERO) == 15.0 and wx.wind_at(Vector3.ZERO) == Vector3.ZERO and wx.rain_share_at(Vector3.ZERO) == 1.0, "")


func _dew_tests() -> void:
	var m := _new_map()
	var c := _new_clock(10, 0.0)
	m.sky_clock = m.get_path_to(c)
	m.air_daily_cycle = false
	m.wind_speed = 1.0
	m.humidity = 0.9
	m.add_canopy(Vector2(-200, -200), 60.0, 20.0, 4.0)
	var open_p := Vector3(200, 0, 200)
	var shade_p := Vector3(-200, 0, -200)
	# A mild clear night: dew.
	m.temperature = 8.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(4.0 * 3600.0)
	var dew_open: float = m.get_wetness_at(open_p)
	var dew_shade: float = m.get_wetness_at(shade_p)
	var cool: float = m.get_surface_temperature_at(open_p) - m.get_air_temperature_at(open_p)
	_check("dew: a clear calm night wets open ground, not under a canopy", dew_open > 0.3 and dew_shade < 0.1 and m.get_frost_at(open_p) == 0.0, "film %.2f open, %.2f under the canopy; surface %.1f K under the air" % [dew_open, dew_shade, -cool])
	# A cold clear night: hoarfrost; an overcast one: none.
	m.temperature = 1.0
	m.snap_to_settings()
	m.advance(6.0 * 3600.0)
	var frost_open: float = m.get_frost_at(open_p)
	var frost_shade: float = m.get_frost_at(shade_p)
	_check("dew: hoarfrost below freezing at the surface, none under the canopy", frost_open > 0.05 and frost_shade < 0.01, "%.3f mm open, %.3f mm under the canopy" % [frost_open, frost_shade])
	var m2 := _new_map()
	m2.sky_clock = m2.get_path_to(c)
	m2.air_daily_cycle = false
	m2.wind_speed = 1.0
	m2.humidity = 0.9
	m2.temperature = 1.0
	m2.cloud_cover = 1.0
	m2.snap_to_settings()
	await get_tree().physics_frame
	m2.advance(6.0 * 3600.0)
	_check("dew: an overcast night leaves no frost", m2.get_frost_at(open_p) < 0.01, "%.3f mm" % m2.get_frost_at(open_p))
	# Morning sun melts it.
	c.time_of_day = 11.0
	m.temperature = 3.0
	m.snap_to_settings()
	m.advance(2.0 * 3600.0)
	_check("dew: the morning sun melts the frost", m.get_frost_at(open_p) < 0.01, "%.3f mm left after 2 h" % m.get_frost_at(open_p))
	# Radiation mist: wet ground and a surface cooled below the dew point bring it on sooner.
	if ClassDB.class_exists("VegetationMist3D"):
		var m3 := _new_map()
		m3.sky_clock = m3.get_path_to(c)
		m3.air_daily_cycle = false
		m3.wind_speed = 0.5
		m3.humidity = 0.85
		m3.temperature = 8.0
		m3.snap_to_settings()
		await get_tree().physics_frame
		c.time_of_day = 2.0
		var mist: Node3D = ClassDB.instantiate("VegetationMist3D")
		add_child(mist)
		mist.weather_map = mist.get_path_to(m3)
		m3.dew_and_frost = false
		m3.advance(600.0)
		var dry_level: float = mist.weather_mist_level()
		m3.rain_rate = 5.0
		m3.cloud_cover = 1.0
		m3.snap_to_settings()
		m3.advance(3600.0)
		m3.rain_rate = 0.0
		m3.cloud_cover = 0.0
		m3.dew_and_frost = true
		m3.snap_to_settings()
		m3.advance(600.0)
		var wet_level: float = mist.weather_mist_level()
		_check("mist: a clear night over wet ground mists sooner than over dry", wet_level > dry_level + 0.1, "level %.2f wet, %.2f dry" % [wet_level, dry_level])
		mist.queue_free()
		m3.queue_free()
	else:
		_skip("mist", "no VegetationMist3D")
	m.queue_free()
	m2.queue_free()
	c.queue_free()
	await get_tree().physics_frame



func _frost_view(path: String, off: bool) -> void:
	var m := _new_map()
	var c := _new_clock(1, 2.0)
	m.sky_clock = m.get_path_to(c)
	m.air_daily_cycle = false
	m.air_follows_climate = false
	m.wind_speed = 1.0
	m.humidity = 0.9
	m.temperature = -2.0
	m.dew_and_frost = not off
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(6.0 * 3600.0)
	print("frost %.3f mm" % m.get_frost_at(Vector3.ZERO))
	# Seen in a low morning light (the frost stays: the clock is not moved).
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-25, 150, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.55, 0.65, 0.8)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.5, 0.58, 0.7)
	env.environment.ambient_light_energy = 0.6
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	add_child(env)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(40, 40)
	ground.mesh = pm
	ground.material_override = TerrainMaterial3D.new()
	ground.position.x = 8.0
	add_child(ground)
	var grass := GrassField3D.new()
	grass.radius = 12.0
	add_child(grass)
	var gen := PlantGenerator.new()
	gen.preset = 0
	gen.apply_preset_colors()
	var fern := MeshInstance3D.new()
	fern.mesh = gen.generate()
	fern.position = Vector3(1.2, 0, 2.0)
	add_child(fern)
	var rg := RockGenerator.new()
	rg.seed = 3
	var rock := MeshInstance3D.new()
	rock.mesh = rg.generate()
	rock.position = Vector3(-1.5, 0, 1.5)
	add_child(rock)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.6, 6.5)
	cam.rotation_degrees = Vector3(-14, 0, 0)
	add_child(cam)
	cam.make_current()
	get_viewport().size = Vector2i(1280, 720)
	for i in 30:
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


# A fire in front of a striped wall (heat shimmer). p_clear: the gas draws nothing (no smoke, no glow), so the
# frame differs from the empty scene only by the bending.
func _shimmer_scene(p_clear: bool, p_pt: bool) -> Node3D:
	var root := Node3D.new()
	add_child(root)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.5, 0.6, 0.75)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.6, 0.6, 0.65)
	env.environment.ambient_light_energy = 0.5
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	if p_pt:
		env.environment.pathtracing_samples_per_pixel = 4 if not p_clear else 32
		env.environment.pathtracing_max_bounces = 2
	root.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 30, 0)
	root.add_child(sun)
	var wall := MeshInstance3D.new()
	var qm := QuadMesh.new()
	qm.size = Vector2(8, 4)
	wall.mesh = qm
	wall.position = Vector3(0, 2, -5)
	var sm := ShaderMaterial.new()
	sm.shader = Shader.new()
	sm.shader.code = """shader_type spatial;
void fragment() {
	vec2 q = UV * vec2(48.0, 24.0);
	float s = step(0.5, fract(q.x)) * 0.7 + step(0.5, fract(q.y * 0.5)) * 0.2;
	ALBEDO = mix(vec3(0.08), vec3(0.85, 0.8, 0.7), s);
}"""
	wall.material_override = sm
	root.add_child(wall)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(12, 12)
	ground.mesh = pm
	root.add_child(ground)
	var flow = ClassDB.instantiate("PhysXFlow3D")
	flow.cell_size = 0.03
	flow.combustion = true
	flow.cooling_rate = 3.0
	flow.vorticity = 4.0
	flow.buoyancy = 4.0
	flow.temperature_range = 1.0
	flow.emission_strength = 0.0 if p_clear else 8.0
	flow.smoke_density = 0.0 if p_clear else 0.7
	if p_clear:
		flow.embers = false
		flow.fire_light = false
	root.add_child(flow)
	var em = ClassDB.instantiate("PhysXFlowEmitter3D")
	em.position = Vector3(0, 0.05, 0)
	em.shape = 1
	em.size = Vector3(0.4, 0.06, 0.4)
	em.fuel = 1.0
	em.temperature = 1.0
	em.smoke = 0.0 if p_clear else 0.02
	em.divergence = 0.3
	em.velocity = Vector3(0, 1.0, 0)
	flow.add_child(em)
	em.flow = em.get_path_to(flow)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0.9, 3.0)
	cam.rotation_degrees = Vector3(-4, 0, 0)
	cam.fov = 40.0
	root.add_child(cam)
	cam.make_current()
	root.set_meta("flow", flow)
	return root


func _shimmer_view(path: String) -> void:
	var args := OS.get_cmdline_user_args()
	var frames := 90
	var clip := ""
	for a in args:
		if a.begins_with("--frames="):
			frames = int(a.substr(9))
		elif a.begins_with("--clip="):
			clip = a.substr(7)
	if not ClassDB.class_exists("PhysXFlow3D"):
		push_error("This build has no Flow.")
		return
	get_viewport().size = Vector2i(640, 360) if args.has("--pt") else Vector2i(1280, 720)
	var root := _shimmer_scene(args.has("--clear"), args.has("--pt"))
	(root.get_child(0) as WorldEnvironment).environment.pathtracing_enabled = false
	root.get_meta("flow").heat_shimmer = not args.has("--shimmer-off")
	for a in args:
		if a.begins_with("--turbulence="):
			root.get_meta("flow").heat_shimmer_turbulence = float(a.substr(13))
	var pt := args.has("--pt")
	for i in frames:
		await RenderingServer.frame_post_draw
	if pt:
		# The path tracer accumulates one frozen instant (the gas and its shimmer stop), as a raster frame shows one.
		Engine.time_scale = 0.0
		var env: Environment = root.get_child(0).environment
		env.pathtracing_enabled = true
		for i in 30:
			await RenderingServer.frame_post_draw
	if clip != "":
		DirAccess.make_dir_recursive_absolute(clip)
		for i in 48:
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png("%s/%03d.png" % [clip, i])
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


# The bending alone (the gas drawn invisible): with heat_shimmer on the frame moves above the fire and nowhere
# else; off, it matches the empty scene.
func _shimmer_tests() -> void:
	if not ClassDB.class_exists("PhysXFlow3D") or DisplayServer.get_name() == "headless":
		_skip("shimmer", "needs Flow and a rendering device")
		return
	var root := _shimmer_scene(true, false)
	var flow = root.get_meta("flow")
	for i in 60:
		await RenderingServer.frame_post_draw
	var on := get_viewport().get_texture().get_image()
	flow.heat_shimmer = false
	for i in 3:
		await RenderingServer.frame_post_draw
	var off := get_viewport().get_texture().get_image()
	var w := on.get_width()
	var h := on.get_height()
	var above := 0.0
	var side := 0.0
	var n_above := 0
	var n_side := 0
	for y in range(int(h * 0.15), int(h * 0.75), 2):
		for x in range(0, w, 2):
			var d := absf(on.get_pixel(x, y).get_luminance() - off.get_pixel(x, y).get_luminance())
			if absf(x - w * 0.5) < w * 0.06:
				above += d
				n_above += 1
			elif absf(x - w * 0.5) > w * 0.3:
				side += d
				n_side += 1
	above /= max(n_above, 1)
	side /= max(n_side, 1)
	_check("shimmer: the hot air bends the wall above the fire, nothing beside it", above > 0.004 and side < 0.0005, "mean change %.4f above the fire, %.5f beside it" % [above, side])
	root.queue_free()
	await get_tree().process_frame


# Snow on plants: a day of snowfall at -4 C, settled for a day, over grass, wheat and ferns.
func _snow_press_view(path: String, off: bool) -> void:
	get_viewport().size = Vector2i(1280, 720)
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "Ground"
	add_child(ground_node)
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-30, 140, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var m := _new_map()
	m.temperature = -4.0
	m.humidity = 0.9
	m.set_physics_process_internal(false)
	m.snap_to_settings()
	m.advance(1.0)
	if not off:
		var snow: Node3D = ClassDB.instantiate("SnowCover3D")
		snow.name = "SnowCover3D"
		snow.extent = 64.0
		snow.resolution = 128
		snow.height_source = NodePath("../Ground")
		snow.sun_light = NodePath("../Sun")
		snow.follow_target = NodePath("../Ground")
		snow.update_rate = 0.0001
		add_child(snow)
		snow.refresh_heights()
		m.rain_rate = 3.0
		m.cloud_cover = 1.0
		m.snap_to_settings()
		m.advance(1.0)
		snow.advance(20.0 * 3600.0)
		m.rain_rate = 0.0
		m.cloud_cover = 0.0
		m.snap_to_settings()
		m.advance(1.0)
		snow.advance(24.0 * 3600.0)
		print("snow depth %.2f m" % snow.get_depth_at(Vector3.ZERO))
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.55, 0.65, 0.8)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.55, 0.6, 0.7)
	env.environment.ambient_light_energy = 0.6
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	add_child(env)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(60, 60)
	ground.mesh = pm
	ground.material_override = TerrainMaterial3D.new()
	add_child(ground)
	var grass := GrassField3D.new()
	grass.radius = 14.0
	add_child(grass)
	var gen := PlantGenerator.new()
	gen.preset = 0
	gen.apply_preset_colors()
	var mesh := gen.generate()
	for k in 5:
		var fern := MeshInstance3D.new()
		fern.mesh = mesh
		fern.position = Vector3(-2.0 + k * 1.1, 0, 1.5 + 0.4 * (k % 2))
		add_child(fern)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.4, 5.5)
	cam.rotation_degrees = Vector3(-14, 0, 0)
	add_child(cam)
	cam.make_current()
	for i in 40:
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


# A stream 25 cm deep over z > 0 (towards the camera), running +x at 0.5 m/s, or still.
class ViewStreamStub extends WorldWater:
	var speed := 0.5

	func _height_at(p: Vector3) -> float:
		return 0.25 if p.z > 0.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(speed, 0, 0) if p.z > 0.0 else Vector3.ZERO


func _current_view(path: String, still: bool) -> void:
	get_viewport().size = Vector2i(1280, 720)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 150, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.55, 0.65, 0.8)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.55, 0.6, 0.7)
	env.environment.ambient_light_energy = 0.6
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	add_child(env)
	WorldContracts.clear_overrides()
	var stub := ViewStreamStub.new()
	stub.speed = 0.0 if still else 0.5
	WorldContracts.add_water_stub(stub)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(60, 60)
	ground.mesh = pm
	ground.material_override = TerrainMaterial3D.new()
	add_child(ground)
	var water := MeshInstance3D.new()
	var wm := PlaneMesh.new()
	wm.size = Vector2(30, 8)
	water.mesh = wm
	water.position = Vector3(0, 0.25, 4.0)
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = Color(0.18, 0.3, 0.32, 0.3)
	mat.roughness = 0.05
	mat.metallic_specular = 0.6
	water.material_override = mat
	add_child(water)
	var grass := GrassField3D.new()
	grass.radius = 14.0
	add_child(grass)
	var gen := PlantGenerator.new()
	gen.preset = PlantGenerator.PRESET_RUSH
	gen.apply_preset_colors()
	var mesh := gen.generate()
	for k in 7:
		var rush := MeshInstance3D.new()
		rush.mesh = mesh
		rush.position = Vector3(-3.0 + k * 1.0, 0, 1.2 + 0.5 * (k % 2))
		add_child(rush)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.3, 5.5)
	cam.rotation_degrees = Vector3(-16, 0, 0)
	add_child(cam)
	cam.make_current()
	for i in 40:
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)
	WorldContracts.clear_overrides()

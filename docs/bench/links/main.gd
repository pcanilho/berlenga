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
#   glaze     a warm nose aloft over -3 C air: the snow melts aloft and lands as freezing rain (rain_share_at
#             through the contract too), glazing twigs as Jones's (1998) model says; a deep cold layer refreezes it
#             to pellets (Bourgouin 2000); a thaw takes the ice off; the ice-storm knob holds a glaze
#   lightning a tall mast takes the strike meant for the ground beside it; striking distance on IEC 62305
#   fire      a long continuing current lights dry fuel, not wet (PhysX)
#   contact   a body in world_contact flattens grass without being in grass_trample, and herbs where there is no grass
#   lee       a wall and a moving prop (get_wind_obstacles) shelter the wind behind them; snow drifts into a wall's
#             lee and blows off its top, the ground past the drift swept; a moved prop takes its lee along
#   fog       a forest on ground standing in the cloud combs the fog out: it drips, wets the ground under it,
#             feeds the runoff; open ground gets little; none under the cloud base
#   spray     an onshore gale off a surf wets the first tens of metres of the shore and salts it for hundreds;
#             none offshore or calm; rain washes the salt off; a cliff face takes the spray, its top stays drier
#   tracks    a body wading out of a stream leaves wet prints fading over a dozen strides, drips where it
#             stands, and the prints dry with the weather
#   mud       a body out of mud prints its clay on firm ground, less each step
#   stand     stand age: youngest stand-replacing disturbance (fire, wind, beetle), change hook
#   walker    every CharacterBody3D is a walker: deformers press its soles, not its capsule
#   salt      a storm's sea salt melts a thin snowpack at -2 C, the unsalted snow beside stays
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

	func _add_sediment_mass(_p: Vector3, mineral: float, ash: float) -> void:
		load += Vector2(mineral, ash) / 1000.0

	func _sediment_at(_p: Vector3) -> Vector2:
		return load


# A sea at 0 m over x < 0: a surf zone of 1 m breakers over its last 40 m, whitecaps (cover times height) beyond.
class SurfStub extends WorldWater:
	func _height_at(p: Vector3) -> float:
		return 0.0 if p.x < 0.0 else -INF

	func _breaking_at(p: Vector3) -> float:
		if p.x >= 0.0:
			return 0.0
		return 1.0 if p.x > -40.0 else 0.03


# The shore under it: a 5 m deep seabed, then a beach rising 2 in 100; with cliff, a 25 m cliff 60 m inland.
class ShoreGround extends WorldGround:
	var cliff := false

	func _height_at(p: Vector3) -> float:
		if p.x < 0.0:
			return -5.0
		return p.x * 0.02 + (25.0 if cliff and p.x > 60.0 else 0.0)


# A weather stub laying sea salt over x < 0 (g/m2), nothing beyond; the rest of the weather from the map.
class SaltWeather extends WorldWeather:
	var salt := 60.0

	func _salt_at(p: Vector3) -> float:
		return salt if p.x < 0.0 else 0.0


# Mud over x < 0, firm ground beyond, recording the mud prints pressed onto it.
class MudGround extends WorldGround:
	var prints := []

	func _height_at(_p: Vector3) -> float:
		return 0.0

	func _mud_depth_at(p: Vector3) -> float:
		return 0.2 if p.x < 0.0 else 0.0

	func _track_mud(p: Vector3, radius: float, depth: float, wet: float, print: Vector4) -> float:
		prints.append(Vector4(p.x, p.z, depth, print.x))
		return PI * radius * radius * depth

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
# Red ground everywhere, for the gas's ground colour.
class RedGround extends WorldGround:
	func _albedo_at(_p: Vector3) -> Color:
		return Color(0.4, 0.1, 0.05, 1.0)


# Soaked ground everywhere.
class SoakedStub extends WorldWeather:
	func _surface_wetness_at(_p: Vector3) -> float:
		return 1.0


# Half a metre of aged snow (albedo 0.7) over x > 0.
class DeepSnowStub extends WorldSnow:
	func _depth_at(p: Vector3) -> float:
		return 0.5 if p.x > 0.0 else 0.0

	func _density_at(_p: Vector3) -> float:
		return 300.0

	func _albedo_at(_p: Vector3) -> float:
		return 0.7


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


# A flood 0.3 m deep over x < 0 carrying 5 kg/m3 of silt and 1 of ash: still over -6 < x < 0, a 1 m/s current
# beyond.
class FloodStub extends WorldWater:
	func _height_at(p: Vector3) -> float:
		return 0.3 if p.x < 0.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(0, 0, 1.0) if p.x < -6.0 else Vector3.ZERO

	func _sediment_at(p: Vector3) -> Vector2:
		return Vector2(5.0, 1.0) if p.x < 0.0 else Vector2.ZERO


# Ground burnt a year ago over x < -10 (no fire burning now).
class BurntStub extends WorldFire:
	func _burn_age_at(p: Vector3) -> float:
		return 1.0 if p.x < -10.0 else -1.0


# A fire stub: 50 kW/m2 burning over x > 0.
# Stand age: an old crown fire for x < 0, a surface fire (low scorch) for x > 20.
class StandFireStub extends WorldFire:
	func _burn_age_at(p: Vector3) -> float:
		return 30.0 if p.x < 0.0 or p.x > 20.0 else -1.0
	func _crown_scorch_at(p: Vector3) -> float:
		return 2.0 if p.x > 20.0 else 0.0


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
		if a.begins_with("--salt="):
			await _salt_view(a.substr(7), not OS.get_cmdline_user_args().has("--salt-off"), OS.get_cmdline_user_args().has("--pt"))
			get_tree().quit()
			return
		if a.begins_with("--valley-fog="):
			await _valley_fog_view(a.substr(13), OS.get_cmdline_user_args().has("--fog-off"), OS.get_cmdline_user_args().has("--pt"))
			get_tree().quit()
			return
		if a.begins_with("--rime="):
			await _rime_view(a.substr(7), OS.get_cmdline_user_args().has("--rime-off"), OS.get_cmdline_user_args().has("--pt"))
			get_tree().quit()
			return
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
	await _glaze_tests()
	await _lightning_tests()
	_fire_tests()
	await _contact_tests()
	await _snow_fire_tests()
	await _dirty_snow_tests()
	await _flood_ground_tests()
	await _lee_drift_tests()
	await _fog_drip_tests()
	await _sea_spray_tests()
	await _wet_track_tests()
	await _mud_track_tests()
	_walker_tests()
	_stand_tests()
	await _salt_snow_tests()
	await _leaf_kick_tests()
	await _leaf_surface_tests()
	await _washout_tests()
	_tyre_snow_tests()
	await _contract_tests()
	await _shimmer_tests()
	await _ground_color_tests()
	await _salt_bloom_tests()
	await _spray_sand_tests()
	await _orographic_tests()
	await _cold_pool_tests()
	await _rime_tests()
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
	# A strike into a forest crown is kept by the stand for the struck tree's variant; one on open ground is not.
	var stn: WorldStand = WorldContracts.get_stand()
	stn.clear_strikes()
	m.add_canopy(Vector2(200, 200), 20.0, 20.0, 4.0)
	await get_tree().physics_frame
	m.rebake_shelter()
	l.thunder_enabled = false
	l.strike(Vector3(200, 0, 200), true)
	l.strike(Vector3(-300, 0, -300), true)
	var sa0 := stn.strike_age_at(Vector3(201.5, 0, 200))
	var sa_near := stn.strike_age_at(Vector3(205, 0, 200))
	var sa_open := stn.strike_age_at(Vector3(-300, 0, -300))
	var sg := stn.get_stand_generation()
	stn.advance_years(6.0)
	var sa6 := stn.strike_age_at(Vector3(201.5, 0, 200))
	var sch: Dictionary = stn.get_stand_changes_since(sg)
	stn.clear_strikes()
	_check("lightning: a crown strike is kept as the struck tree's age (trunks within 3 m), not the neighbours' or open ground's; it ages with the years", sa0 == 0.0 and sa_near < 0.0 and sa_open < 0.0 and sa6 == 6.0 and Rect2(sch["area"]).has_point(Vector2(200, 200)), "at the tree %.0f y, 5 m off %.0f, open ground %.0f, 6 y on %.0f; change %s" % [sa0, sa_near, sa_open, sa6, str(sch)])
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
# Low cloud on a hill: base 100 m, overcast, dry air and a fresh wind (no radiation fog).
class HillCloudStub extends WorldWeather:
	func _cloud_base_at(_p: Vector3) -> float:
		return 100.0
	func _cloud_cover_at(_p: Vector3) -> float:
		return 1.0
	func _air_humidity_at(_p: Vector3) -> float:
		return 0.7
	func _mean_wind_at(_p: Vector3) -> Vector3:
		return Vector3(6, 0, 0)
	func _air_temperature_at(_p: Vector3) -> float:
		return 8.0


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
	# Tyre grip from the snow's own friction (WorldSnow.grip_at): loose snow, a packed road, the road polished to ice.
	if ClassDB.class_exists("PhysXVehicle3D"):
		var p_loose := Vector3(-24, 0, -20)
		var p_packed := Vector3(-24, 0, 0)
		var p_polished := Vector3(-24, 0, 20)
		snow.add_traffic(p_packed, 3.0, 1.0, 0.0)
		snow.add_traffic(p_polished, 3.0, 1.0, 2.0)
		var g_loose: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", p_loose, 10.0, 0)
		var g_packed: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", p_packed, 10.0, 0)
		var g_polished: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", p_polished, 10.0, 0)
		var g_chains: float = ClassDB.class_call_static("PhysXVehicle3D", "get_firm_grip_at", p_polished, 10.0, 3)
		var c_polished: float = WorldContracts.get_snow().grip_at(p_polished)
		_check("tyres: grip from the snow's own friction, loose over packed, dropping on snow polished to ice; chains bite the ice", is_equal_approx(c_polished, snow.get_grip_at(p_polished)) and g_loose > g_packed and g_polished < 0.6 * g_packed and g_polished <= 0.15 and g_chains > 2.5 * g_polished, "loose %.3f, packed %.3f, polished %.3f (contract %.3f), polished with chains %.3f" % [g_loose, g_packed, g_polished, c_polished, g_chains])
	else:
		_skip("tyres: snow's own grip", "no PhysX in this build")
	snow.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# Dirty snow: particles on the snow lower its albedo and speed the melt in sun; fresh snow buries them and the melt
# brings them back; burnt forest sheds its char onto the snow under it.
func _dirty_snow_tests() -> void:
	if not ClassDB.class_exists("SnowCover3D"):
		_skip("snow: dirty snow", "no SnowCover3D")
		return
	WorldContracts.clear_overrides()
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "DirtyGround"
	add_child(ground_node)
	var m := _new_map()
	m.temperature = -4.0
	m.humidity = 0.9
	m.rain_rate = 3.0
	m.cloud_cover = 1.0
	m.air_follows_climate = false
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(1.0)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 0, 0)
	add_child(sun)
	var snow: Node3D = ClassDB.instantiate("SnowCover3D")
	snow.extent = 64.0
	snow.resolution = 64
	snow.update_rate = 0.0001
	add_child(snow)
	snow.height_source = snow.get_path_to(ground_node)
	snow.follow_target = snow.get_path_to(ground_node)
	snow.sun_light = snow.get_path_to(sun)
	snow.refresh_heights()
	snow.advance(10.0 * 3600.0)
	var dirty := Vector3(10, 0, 0)
	var clean := Vector3(-5, 0, 0)
	snow.add_impurity(dirty, 5.0, 30.0)
	snow.advance(60.0)
	var a_dirty: float = snow.get_albedo_at(dirty)
	var a_clean: float = snow.get_albedo_at(clean)
	var shown: float = snow.get_impurity_at(dirty)
	# Fresh snow over it.
	snow.advance(3.0 * 3600.0)
	var buried: float = snow.get_impurity_at(dirty)
	_check("snow: dust darkens the snow it lands on; fresh snow buries it", absf(shown - 30.0) < 3.0 and a_clean - a_dirty > 0.2 and buried < 0.2 * shown and snow.get_impurity_at(clean) == 0.0, "albedo %.2f clean, %.2f dusty (%.0f mg/m2); %.1f mg/m2 showing after 3 h of snowfall" % [a_clean, a_dirty, shown, buried])
	# A sunny thaw: the dust melts out and the dusty snow goes faster.
	m.rain_rate = 0.0
	m.cloud_cover = 0.0
	m.temperature = 2.0
	m.snap_to_settings()
	m.advance(1.0)
	var d0: float = snow.get_depth_at(dirty)
	var c0: float = snow.get_depth_at(clean)
	snow.advance(8.0 * 3600.0)
	var dirty_melt: float = d0 - snow.get_depth_at(dirty)
	var clean_melt: float = c0 - snow.get_depth_at(clean)
	var out: float = snow.get_impurity_at(dirty)
	_check("snow: the thaw brings the dust back up and the dusty snow melts faster", out > 15.0 and dirty_melt > 1.15 * clean_melt and clean_melt > 0.0, "%.1f mg/m2 showing again; melt %.3f m dusty, %.3f m clean over 8 h" % [out, dirty_melt, clean_melt])
	# Burnt forest a year ago: its char falls on the snow over ten cold, dry days.
	m.temperature = -10.0
	m.snap_to_settings()
	m.advance(1.0)
	WorldContracts.set_fire(BurntStub.new())
	snow.advance(10.0 * 86400.0)
	var burnt: float = snow.get_impurity_at(Vector3(-20, 0, 0))
	var unburnt: float = snow.get_impurity_at(clean)
	_check("snow: burnt forest sheds char onto the snow under it", burnt > 1.0 and unburnt < 0.05, "%.2f mg/m2 under the burn after 10 days, %.2f beside it" % [burnt, unburnt])
	WorldContracts.clear_overrides()
	snow.queue_free()
	sun.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# A prop that gives its own wind obstacles (as RockField3D and PhysXVehicle3D do): one box, 1.5 m high.
class CrateObstacle extends Node3D:
	func get_wind_obstacles(_rect: Rect2) -> Array:
		return [AABB(global_position - Vector3(1, 0, 1), Vector3(2, 1.5, 2))]


# Lee drifts (docs/ideas/world-links.md): obstacles in the weather_shelter group slow the wind behind them
# (WorldWeather.obstacle_lee_at), and stand in the snow's Sx (obstacle_height_at), so snow fills their lee.
func _lee_drift_tests() -> void:
	if not ClassDB.class_exists("SnowCover3D"):
		_skip("lee: snow drifts", "no SnowCover3D")
		return
	WorldContracts.clear_overrides()
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "LeeGround"
	add_child(ground_node)
	# A wall 2 m high and 40 m long across a 14 m/s wind blowing to +x.
	var wall := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(1, 2, 40)
	wall.mesh = bm
	wall.position = Vector3(0, 1, 0)
	wall.add_to_group("weather_shelter")
	add_child(wall)
	var crate := CrateObstacle.new()
	crate.position = Vector3(-200, 0, 100)
	crate.add_to_group("weather_shelter")
	add_child(crate)
	var m := _new_map()
	m.wind_speed = 14.0
	m.wind_direction = Vector2(1, 0)
	m.temperature = -6.0
	m.humidity = 0.9
	m.rain_rate = 2.0
	m.cloud_cover = 1.0
	m.air_follows_climate = false
	m.snap_to_settings()
	await get_tree().physics_frame
	m.rebake_shelter()
	m.advance(1.0)
	var w: WorldWeather = WorldContracts.get_weather()
	var on_wall: float = w.obstacle_height_at(Vector3(0, 0, 0))
	var cavity: float = w.obstacle_lee_at(Vector3(3, 0, 0))
	var ten_h: float = w.obstacle_lee_at(Vector3(20, 0, 0))
	var upwind: float = w.obstacle_lee_at(Vector3(-6, 0, 0))
	_check("lee: a wall in the weather_shelter group shelters the wind behind it, from the terrain under it", absf(on_wall - 2.0) < 0.05 and cavity < 0.3 and ten_h > cavity and ten_h < 0.95 and upwind == 1.0, "wall %.2f m high; near-ground wind %.2f at 1.5 H behind, %.2f at 10 H, %.2f upwind" % [on_wall, cavity, ten_h, upwind])
	var crate_lee: float = w.obstacle_lee_at(Vector3(-196, 0, 100))
	crate.position = Vector3(-200, 0, 60)
	for i in 150:
		await get_tree().physics_frame
	var left: float = w.obstacle_lee_at(Vector3(-196, 0, 100))
	var moved: float = w.obstacle_lee_at(Vector3(-196, 0, 60))
	_check("lee: a prop's own obstacles (get_wind_obstacles) shelter, and a moved one takes its lee along", crate_lee < 0.5 and left == 1.0 and moved < 0.5 and absf(w.obstacle_height_at(Vector3(-200, 0, 60)) - 1.5) < 0.05, "%.2f behind it, %.2f there after it moved, %.2f behind it at its new place" % [crate_lee, left, moved])
	# Snow over a day of the gale: the wall's lee fills, its top is stripped.
	var snow: Node3D = ClassDB.instantiate("SnowCover3D")
	snow.extent = 64.0
	snow.resolution = 64
	snow.update_rate = 0.0001
	add_child(snow)
	snow.height_source = snow.get_path_to(ground_node)
	snow.follow_target = snow.get_path_to(ground_node)
	snow.refresh_heights()
	snow.advance(6.0 * 3600.0)
	var lee_d: float = snow.get_depth_at(Vector3(4, 0, 5))
	var far_d: float = snow.get_depth_at(Vector3(20, 0, 5))
	var open_d: float = snow.get_depth_at(Vector3(-15, 0, 5))
	var top_d: float = snow.get_depth_at(Vector3(0, 0, 5))
	var foot_d: float = snow.get_depth_at(Vector3(2.5, 0, 5))
	# The wall traps the blowing snow, so the ground past its drift is swept bare until the drift fills (what a snow
	# fence does for a road; Tabler 1991).
	_check("lee: snow drifts deep in a wall's lee, blows off its top, and the ground past the drift is swept", foot_d > 2.0 * open_d and lee_d > 1.2 * open_d and far_d < 0.5 * open_d and top_d < open_d and open_d > 0.05, "%.1f cm at its foot, %.1f cm at 2 H behind, %.1f cm at 10 H, %.1f cm in the open upwind, %.1f cm on the wall" % [foot_d * 100.0, lee_d * 100.0, far_d * 100.0, open_d * 100.0, top_d * 100.0])
	snow.drift_obstacles = false
	snow.advance(6.0 * 3600.0)
	var lee_off: float = snow.get_depth_at(Vector3(4, 0, 5)) - lee_d
	var foot_off: float = snow.get_depth_at(Vector3(2.5, 0, 5)) - foot_d
	var open_off: float = snow.get_depth_at(Vector3(-15, 0, 5)) - open_d
	# The wind sees the drift's own relief (SnowCover3D drift_relief): unsheltered, its crest is taken back while snow
	# still falls in the open.
	_check("lee: drift_obstacles off, the drift is no longer fed and the wind takes its crest back", foot_off < 0.0 and lee_off < 0.5 * open_off and open_off > 0.05, "%.1f cm at its foot, %.1f cm 2 H behind it, %.1f cm in the open over 6 more hours" % [foot_off * 100.0, lee_off * 100.0, open_off * 100.0])
	WorldContracts.clear_overrides()
	snow.queue_free()
	wall.queue_free()
	crate.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# Fog drip (docs/ideas/world-links.md): ground standing in the sky's cloud deck, a forest on half of it.
func _fog_drip_tests() -> void:
	WorldContracts.clear_overrides()
	var m := _new_map()
	m.ground_height = 400.0
	m.cloud_base_height = 300.0
	m.cloud_cover = 1.0
	m.wind_speed = 6.0
	m.temperature = 8.0
	m.humidity = 0.95
	m.air_follows_climate = false
	m.snap_to_settings()
	m.add_canopy(Vector2(-150, 0), 120.0, 20.0, 2.5)
	await get_tree().physics_frame
	m.rebake_shelter()
	# A day in the cloud: the canopy fills its storage (about 1 mm) over the first hours, then drips (Rutter).
	m.advance(24.0 * 3600.0)
	var w: WorldWeather = WorldContracts.get_weather()
	var forest := Vector3(-150, 400, 0)
	var open := Vector3(200, 400, 0)
	var lwc: float = w.fog_water_at(open)
	var drip_f: float = w.fog_drip_at(forest)
	var drip_o: float = w.fog_drip_at(open)
	print("  fog: %.2f g/m3 at the ground; drip %.3f mm/h under the forest, %.3f in the open; wetness %.2f / %.2f; ground input %.3f mm/h" % [lwc, drip_f, drip_o, m.get_wetness_at(forest), m.get_wetness_at(open), w.ground_water_input_at(forest)])
	_check("fog: a forest standing in the cloud combs it out and, its leaves full, drips onto the ground (0.015 to 0.5 mm/h)", lwc > 0.05 and drip_f > 0.015 and drip_f < 0.5 and drip_f > 3.0 * drip_o and m.get_wetness_at(forest) > m.get_wetness_at(open) and w.ground_water_input_at(forest) >= drip_f * 0.99, "%.2f g/m3 of cloud water; %.3f mm/h under the forest, %.3f in the open; wetness %.2f under the trees, %.2f in the open" % [lwc, drip_f, drip_o, m.get_wetness_at(forest), m.get_wetness_at(open)])
	m.cloud_base_height = 1500.0
	m.snap_to_settings()
	m.advance(6.0 * 3600.0)
	_check("fog: none once the cloud base lifts over the ground", w.fog_water_at(forest) == 0.0 and w.fog_drip_at(forest) < 0.2 * drip_f, "%.2f g/m3, %.3f mm/h still draining" % [w.fog_water_at(forest), w.fog_drip_at(forest)])
	m.queue_free()
	await get_tree().physics_frame


# Sea spray (WorldWater.breaking_at into WeatherMap3D): an onshore gale off a surf wets the first tens of metres of the
# shore and salts it for hundreds; an offshore or calm wind brings none; rain washes the salt off; a cliff face takes
# the spray and leaves its top drier.
func _sea_spray_tests() -> void:
	WorldContracts.clear_overrides()
	var ground := ShoreGround.new()
	WorldContracts.set_ground(ground)
	WorldContracts.add_water_stub(SurfStub.new())
	var m := _new_map()
	m.ground_height = 0.0
	m.wind_speed = 12.0
	m.wind_direction = Vector2(1, 0)
	m.temperature = 12.0
	m.humidity = 0.8
	m.air_follows_climate = false
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(6.0 * 3600.0)
	var w: WorldWeather = WorldContracts.get_weather()
	var near := Vector3(6, 0, 0)
	var mid := Vector3(60, 0, 0)
	var far := Vector3(220, 0, 0)
	var s_near: float = w.sea_spray_at(near)
	var s_mid: float = w.sea_spray_at(mid)
	var s_far: float = w.sea_spray_at(far)
	var salt_near: float = w.salt_at(near)
	var salt_mid: float = w.salt_at(mid)
	var salt_far: float = w.salt_at(far)
	print("  spray (12 m/s onshore, 1 m surf): %.3f mm/h at 6 m, %.4f at 60 m, %.5f at 220 m; salt %.2f / %.3f / %.3f g/m2 after 6 h; wetness %.2f / %.2f" % [s_near, s_mid, s_far, salt_near, salt_mid, salt_far, w.surface_wetness_at(near), w.surface_wetness_at(far)])
	_check("spray: an onshore gale off the surf wets the first tens of metres of the shore (0.05 to 3 mm/h at the waterline)", s_near > 0.05 and s_near < 3.0 and s_mid < 0.3 * s_near and s_far < 0.01 * s_near and w.surface_wetness_at(near) > 0.5 and w.surface_wetness_at(near) > w.surface_wetness_at(far) + 0.3 and w.ground_water_input_at(near) >= s_near * 0.99, "%.3f / %.4f / %.5f mm/h; wetness %.2f near, %.2f far" % [s_near, s_mid, s_far, w.surface_wetness_at(near), w.surface_wetness_at(far)])
	_check("spray: its salt lies thickest at the shore and, as fine aerosol, still reaches hundreds of metres inland", salt_near > salt_mid and salt_mid > salt_far and salt_far > 0.01 and salt_far < 0.2 * salt_near, "%.2f / %.3f / %.3f g/m2" % [salt_near, salt_mid, salt_far])
	# Offshore and calm: none comes ashore.
	m.wind_direction = Vector2(-1, 0)
	m.snap_to_settings()
	m.advance(60.0)
	var off: float = w.sea_spray_at(near)
	m.wind_direction = Vector2(1, 0)
	m.wind_speed = 1.5
	m.snap_to_settings()
	m.advance(60.0)
	var calm: float = w.sea_spray_at(near)
	_check("spray: none comes ashore in an offshore wind or a calm", off < 0.01 * s_near and calm == 0.0, "offshore %.4f, calm %.4f mm/h" % [off, calm])
	# Rain washes the salt off (about 5 mm takes all but a third).
	m.cloud_cover = 1.0
	m.rain_rate = 5.0
	m.snap_to_settings()
	m.advance(2.0 * 3600.0)
	var washed: float = w.salt_at(near)
	var fell: float = w.rainfall_at(near)
	m.rain_rate = 0.0
	m.cloud_cover = 0.0
	m.snap_to_settings()
	_check("spray: rain washes the salt off the shore", washed < 0.2 * salt_near and fell > 1.0, "%.2f -> %.2f g/m2 after two hours of %.1f mm/h" % [salt_near, washed, fell])
	# A cliff 60 m inland: its face catches the spray, its top stays drier than open ground there.
	m.clear_salt()
	m.wind_speed = 15.0
	m.snap_to_settings()
	m.advance(3600.0)
	var flat_top: float = w.sea_spray_at(Vector3(70, 0, 0))
	ground.cliff = true
	m.advance(3600.0)
	var face: float = w.sea_spray_at(Vector3(62, 0, 0))
	var top: float = w.sea_spray_at(Vector3(70, 25, 0))
	_check("spray: a cliff's face catches the spray, its top gets less than open ground", face > flat_top and top < 0.5 * flat_top, "face %.4f, top %.4f, open ground %.4f mm/h" % [face, top, flat_top])
	m.queue_free()
	WorldContracts.clear_overrides()
	await get_tree().physics_frame


# Wet tracks: a body wading out of a stream leaves wet prints that fade over a dozen or so strides and drips where it
# stands; dry ground beside stays dry; the prints dry with the weather.
func _track_wet(m: Node3D, x0: float, x1: float) -> float:
	var best := 0.0
	var x := x0
	while x <= x1:
		var z := -0.4
		while z <= 0.4:
			best = maxf(best, m.get_contact_wetness_at(Vector3(x, 0, z)))
			z += 0.05
		x += 0.05
	return best


func _wet_track_tests() -> void:
	WorldContracts.clear_overrides()
	WorldContracts.add_water_stub(StreamStub.new())
	var m := _new_map()
	m.temperature = 15.0
	m.humidity = 0.6
	m.air_follows_climate = false
	m.snap_to_settings()
	var anchor := Node3D.new()
	add_child(anchor)
	m.follow_target = m.get_path_to(anchor)
	var body := Node3D.new()
	body.set_meta("trample_radius", 0.3)
	body.add_to_group("world_contact")
	add_child(body)
	body.position = Vector3(-3, 0.3, 0)
	for i in 30:
		await get_tree().physics_frame
	var soaked: Vector2 = m.get_body_water(body)
	# Walk out at 1.4 m/s to x = 12.
	while body.position.x < 12.0:
		body.position.x += 1.4 / 60.0
		await get_tree().physics_frame
	var near := _track_wet(m, 0.5, 1.5)
	var far := _track_wet(m, 9.0, 10.0)
	var beside: float = m.get_contact_wetness_at(Vector3(5, 0, 2.0))
	print("  tracks: body water %.2f / %.2f in the stream; prints %.2f at 1 m, %.2f at 9.5 m; beside %.2f" % [soaked.x, soaked.y, near, far, beside])
	_check("tracks: a body out of a stream leaves wet prints that fade along the way, dry ground beside", soaked.x > 0.99 and near > 0.5 and far < 0.6 * near and far > 0.02 and beside == 0.0, "prints %.2f at 1 m, %.2f at 9.5 m, beside %.2f" % [near, far, beside])
	for i in 120:
		await get_tree().physics_frame
	var under := _track_wet(m, 11.8, 12.6)
	_check("tracks: standing still, it drips round itself", under > 0.2, "%.2f under it after 2 s (body drip %.2f)" % [under, m.get_body_water(body).y])
	body.queue_free()
	m.time_scale = 2000.0
	for i in 60:
		await get_tree().physics_frame
	var dried := _track_wet(m, 0.5, 1.5)
	_check("tracks: the prints dry with the weather", dried < 0.3 * near, "%.2f -> %.2f after about %.0f s of weather (drying time %.0f s)" % [near, dried, 2000.0, m.get_contact_drying_time()])
	m.queue_free()
	anchor.queue_free()
	WorldContracts.clear_overrides()
	await get_tree().physics_frame


# Muddy prints: a body walking out of mud presses the clay it carries onto the firm ground, print by print, less
# each time; none while still in the mud.
# Characters print with their feet: every CharacterBody3D is a walker (deformers press its soles step by step, not
# its capsule), a meta turns that off or says its feet are contact bodies of their own.
# Stand age: the youngest stand-replacing disturbance (a crown fire from WorldFire, wind and beetle events), a surface
# fire leaving the stand, events ageing with the years, and the change hook readers re-place by.
func _stand_tests() -> void:
	WorldContracts.set_fire(StandFireStub.new())
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	var g0: int = st.get_stand_generation()
	var a_fire := st.stand_age_at(Vector3(-5, 0, 0))
	var k_fire := st.disturbance_at(Vector3(-5, 0, 0))
	var a_surface := st.stand_age_at(Vector3(25, 0, 0))
	var a_old := st.stand_age_at(Vector3(10, 0, 0))
	var wind := st.add_disturbance(Vector3(-5, 0, 0), 4.0, WorldStand.DISTURBANCE_WIND, 3.0)
	var a_wind := st.stand_age_at(Vector3(-5, 0, 1))
	var k_wind := st.disturbance_at(Vector3(-5, 0, 1))
	var ch: Dictionary = st.get_stand_changes_since(g0)
	st.advance_years(40.0)
	var a_aged := st.stand_age_at(Vector3(-5, 0, 1))
	var k_aged := st.disturbance_at(Vector3(-5, 0, 1))
	st.remove_disturbance(wind)
	# Events across the index's cells: a beetle patch over a cell corner, and a 2 km storm track (the wide list).
	var beetle := st.add_disturbance(Vector3(128, 0, 128), 30.0, WorldStand.DISTURBANCE_BEETLE, 2.0)
	var storm := st.add_disturbance(Vector3(5000, 0, 0), 1500.0, WorldStand.DISTURBANCE_WIND, 7.0)
	var grid_ok := st.stand_age_at(Vector3(110, 0, 140)) == 2.0 and st.stand_age_at(Vector3(150, 0, 110)) == 2.0 and st.stand_age_at(Vector3(170, 0, 170)) < 0.0 and st.stand_age_at(Vector3(4000, 0, 900)) == 7.0 and st.disturbance_at(Vector3(4000, 0, 900)) == WorldStand.DISTURBANCE_WIND
	var beaver := st.add_disturbance(Vector3(-300, 0, 0), 40.0, WorldStand.DISTURBANCE_BEAVER, 6.0)
	grid_ok = grid_ok and st.disturbance_at(Vector3(-300, 0, 10)) == WorldStand.DISTURBANCE_BEAVER and st.stand_age_at(Vector3(-300, 0, 10)) == 6.0
	st.remove_disturbance(beaver)
	# A beaver dam: no meadow while it holds; breached, the drained pond is a beaver meadow aging with the years; gone with the dam.
	var dam: Node3D = ClassDB.instantiate("WorldBeaverDam3D")
	dam.pond_radius = 30.0
	dam.position = Vector3(2000, 0, 2000)
	add_child(dam)
	var k_held := st.disturbance_at(Vector3(2000, 0, 2005))
	dam.breach()
	var k_breach := st.disturbance_at(Vector3(2000, 0, 2005))
	var a_breach := st.stand_age_at(Vector3(2000, 0, 2005))
	dam.years_drained = 12.0
	var a_set := st.stand_age_at(Vector3(2000, 0, 2005))
	var k_off := st.disturbance_at(Vector3(2000, 0, 2040))
	remove_child(dam)
	var k_gone := st.disturbance_at(Vector3(2000, 0, 2005))
	dam.free()
	_check("stand: a beaver dam that fails drains its pond into a beaver meadow (DISTURBANCE_BEAVER), aged by years_drained, only over the pond, gone with the dam", k_held == WorldStand.DISTURBANCE_NONE and k_breach == WorldStand.DISTURBANCE_BEAVER and a_breach == 0.0 and a_set == 12.0 and k_off == WorldStand.DISTURBANCE_NONE and k_gone == WorldStand.DISTURBANCE_NONE, "held %d, breached %d at %.0f y, set %.0f y, off the pond %d, dam removed %d" % [k_held, k_breach, a_breach, a_set, k_off, k_gone])
	# Boar rooting turns the field layer, not the stand: stand age and kind stay; rooting_age_at reads it.
	var wood := st.add_disturbance(Vector3(3000, 0, 3000), 60.0, WorldStand.DISTURBANCE_WIND, 40.0)
	var rooted := st.add_disturbance(Vector3(3000, 0, 3000), 8.0, WorldStand.DISTURBANCE_ROOTING, 0.25)
	var root_ok := is_equal_approx(st.rooting_age_at(Vector3(3002, 0, 3000)), 0.25) and st.rooting_age_at(Vector3(3020, 0, 3000)) < 0.0 and st.stand_age_at(Vector3(3002, 0, 3000)) == 40.0 and st.disturbance_at(Vector3(3002, 0, 3000)) == WorldStand.DISTURBANCE_WIND
	st.remove_disturbance(rooted)
	st.remove_disturbance(wood)
	_check("stand: boar rooting (DISTURBANCE_ROOTING) is read by rooting_age_at and leaves the stand's age and kind alone", root_ok, str(root_ok))
	# Game trails: published by their maker, read along their tread.
	var tg := st.get_stand_generation()
	st.set_trails([{"points": PackedVector3Array([Vector3(4000, 0, 4000), Vector3(4100, 0, 4000), Vector3(4100, 0, 4100)]), "width": 0.6, "passes": 900.0, "kind": "game_trail"}, {"points": PackedVector2Array([Vector2(4050, 3950), Vector2(4050, 4050)]), "width": 1.0, "passes_per_year": 200.0, "kind": "game_trail"}])
	var t_mid: Vector2 = st.trail_at(Vector3(4050, 0, 4000))
	var t_edge: Vector2 = st.trail_at(Vector3(4070, 0, 4000.25))
	var t_side: Vector2 = st.trail_at(Vector3(4050, 0, 4030))
	var t_off: Vector2 = st.trail_at(Vector3(4070, 0, 4005))
	var tch: Dictionary = st.get_stand_changes_since(tg)
	var t_count := st.get_trails().size()
	st.set_trails([])
	var t_gone: Vector2 = st.trail_at(Vector3(4050, 0, 4000))
	_check("stand: game trails published once, read by every system: the busiest trail's passes and tread share, eased at the edge, none off them; changes reported", t_count == 2 and t_mid == Vector2(900, 1) and t_edge.x == 900.0 and t_edge.y > 0.0 and t_edge.y < 1.0 and t_side == Vector2(200, 1) and t_off == Vector2.ZERO and t_gone == Vector2.ZERO and Rect2(tch["area"]).has_point(Vector2(4050, 4000)), "crossing %s, edge %s, side trail %s, off %s, cleared %s" % [t_mid, t_edge, t_side, t_off, t_gone])
	# Browsing: a red deer wood and a roe patch inside it; soft rim; pressure changes reported.
	var red := st.add_browse(Vector3(1000, 0, 1000), 50.0, 0.6)
	var roe := st.add_browse(Vector3(1010, 0, 1000), 10.0, 0.9, 1.1)
	var bg := st.get_stand_generation()
	var b_red := st.browse_at(Vector3(970, 0, 1000))
	var b_roe := st.browse_at(Vector3(1010, 0, 1000))
	var r_red := st.browse_reach_at(Vector3(970, 0, 1000))
	var r_roe := st.browse_reach_at(Vector3(1010, 0, 1000))
	var b_rim := st.browse_at(Vector3(1047, 0, 1000))
	var b_out := st.browse_at(Vector3(1060, 0, 1000))
	st.set_browse(red, 0.2)
	var b_set := st.browse_at(Vector3(970, 0, 1000))
	var bch: Dictionary = st.get_stand_changes_since(bg)
	st.remove_browse(roe)
	var b_gone := st.browse_at(Vector3(1010, 0, 1000))
	st.clear_browse()
	var b_clear := st.browse_at(Vector3(970, 0, 1000))
	_check("stand: browse pressure is the strongest disc's, with its reach (red deer 1.8 m, roe 1.1 m), soft over the rim, 0 outside; changes reported", is_equal_approx(b_red, 0.6) and is_equal_approx(b_roe, 0.9) and is_equal_approx(r_red, 1.8) and is_equal_approx(r_roe, 1.1) and b_rim > 0.0 and b_rim < 0.6 and b_out == 0.0 and is_equal_approx(b_set, 0.2) and is_equal_approx(b_gone, 0.2) and b_clear == 0.0 and Rect2(bch["area"]).has_point(Vector2(1000, 1000)), "red %.2f reach %.1f, roe %.2f reach %.1f, rim %.2f, outside %.2f, set %.2f, roe removed %.2f, cleared %.2f" % [b_red, r_red, b_roe, r_roe, b_rim, b_out, b_set, b_gone, b_clear])
	st.remove_disturbance(beetle)
	st.remove_disturbance(storm)
	grid_ok = grid_ok and st.stand_age_at(Vector3(128, 0, 128)) < 0.0
	_check("stand: disturbances found across grid cells and wide storm tracks (a drained beaver pond too), gone once removed", grid_ok, str(grid_ok))
	WorldContracts.set_fire(null)
	print("  stand: crown fire %.0f y (kind %d), surface fire %.0f, untouched %.0f; windthrow %.0f y (kind %d), 40 y on %.0f (kind %d); change %s" % [a_fire, k_fire, a_surface, a_old, a_wind, k_wind, a_aged, k_aged, str(ch)])
	_check("stand: the youngest stand-replacing disturbance gives the age (crown fire 30 y, windthrow 3 y over it, then the fire again once the windthrow is older); a surface fire and untouched ground stay old growth (-1)", a_fire == 30.0 and k_fire == WorldStand.DISTURBANCE_FIRE and a_surface < 0.0 and a_old < 0.0 and a_wind == 3.0 and k_wind == WorldStand.DISTURBANCE_WIND and a_aged == 30.0 and k_aged == WorldStand.DISTURBANCE_FIRE, "fire %.0f, surface %.0f, wind %.0f, aged %.0f" % [a_fire, a_surface, a_wind, a_aged])
	_check("stand: a new disturbance reports its area since the reader's generation", ch["complete"] and Rect2(ch["area"]).has_point(Vector2(-5, 0)), str(ch))


func _walker_tests() -> void:
	var wc: Object = Engine.get_singleton("WorldContracts")
	var ch := CharacterBody3D.new()
	var cs := CollisionShape3D.new()
	var cap := CapsuleShape3D.new()
	cap.height = 1.8
	cs.shape = cap
	ch.add_child(cs)
	add_child(ch)
	var rb := RigidBody3D.new()
	add_child(rb)
	var d1: Dictionary = wc.describe_body(ch)
	var d2: Dictionary = wc.describe_body(rb)
	ch.set_meta("world_contact_feet", true)
	var d3: Dictionary = wc.describe_body(ch)
	ch.set_meta("world_contact_walker", false)
	var d4: Dictionary = wc.describe_body(ch)
	_check("contact: a character is a walker (soles, not its capsule), a rigid body is not; metas say feet print or opt out", d1["walker"] and not d1["feet"] and not d2["walker"] and d3["feet"] and not d4["walker"], "character %s, rigid %s, with feet %s, opted out %s" % [d1["walker"], d2["walker"], d3["feet"], d4["walker"]])
	ch.queue_free()
	rb.queue_free()


func _mud_track_tests() -> void:
	WorldContracts.clear_overrides()
	var g := MudGround.new()
	WorldContracts.set_ground(g)
	var m := _new_map()
	var anchor := Node3D.new()
	add_child(anchor)
	m.follow_target = m.get_path_to(anchor)
	var body := Node3D.new()
	body.set_meta("trample_radius", 0.3)
	body.add_to_group("world_contact")
	add_child(body)
	body.position = Vector3(-4, 0.3, 0)
	for i in 10:
		await get_tree().physics_frame
	while body.position.x < 12.0:
		body.position.x += 1.4 / 60.0
		await get_tree().physics_frame
	var in_mud := 0
	var firm := []
	for pr in g.prints:
		if pr.x < 0.0:
			in_mud += 1
		else:
			firm.append(pr)
	var first: float = firm[0].z if firm.size() > 0 else 0.0
	var last: float = firm[firm.size() - 1].z if firm.size() > 0 else 0.0
	print("  mud prints: %d on firm ground, first %.2f mm, last %.3f mm, %d in the mud; carried %.4f mm at the end" % [firm.size(), first * 1000.0, last * 1000.0, in_mud, m.get_body_mud(body) * 1000.0])
	_check("mud: a body out of mud prints its clay on the firm ground, less each step, as soles (none in the mud)", firm.size() >= 8 and in_mud == 0 and first >= 0.0009 and last < 0.4 * first and firm[0].w == 2.0, "%d prints, %.2f -> %.3f mm, %d in the mud" % [firm.size(), first * 1000.0, last * 1000.0, in_mud])
	body.queue_free()
	m.queue_free()
	anchor.queue_free()
	WorldContracts.clear_overrides()
	await get_tree().physics_frame


# Salt on snow: a storm's sea salt (60 g/m2, near the waterline) on a thin pack melts it at -2 C (its brine freezes
# lower), the unsalted snow beside stays.
func _salt_snow_tests() -> void:
	if not ClassDB.class_exists("SnowCover3D"):
		_skip("snow: salt melts it", "no SnowCover3D")
		return
	WorldContracts.clear_overrides()
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "SaltGround"
	add_child(ground_node)
	var m := _new_map()
	m.temperature = -2.0
	m.humidity = 0.9
	m.air_follows_climate = false
	m.rain_rate = 2.0
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
	snow.advance(3.0 * 3600.0)
	m.rain_rate = 0.0
	m.cloud_cover = 0.0
	m.snap_to_settings()
	m.advance(1.0)
	var salted_at := Vector3(-10, 0, 0)
	var plain_at := Vector3(10, 0, 0)
	var before: float = snow.get_depth_at(salted_at)
	var sw := SaltWeather.new()
	WorldContracts.set_weather(sw)
	snow.advance(6.0 * 3600.0)
	var dep: float = snow.get_salt_depression_at(salted_at)
	var salted: float = snow.get_depth_at(salted_at)
	var plain: float = snow.get_depth_at(plain_at)
	print("  salt snow: %.3f m before; after 6 h at -2 C, %.3f m salted (brine %.1f K lower now), %.3f m unsalted" % [before, salted, dep, plain])
	# 60 g/m2 of salt at -2 C can only hold about 1.8 kg/m2 of brine, so the
	# salted pack loses that much and no more; the plain pack only settles.
	_check("snow: sea salt melts what its brine can hold at -2 C, the unsalted snow beside only settles", before > 0.005 and before - salted > 2.5 * (before - plain) and before - salted > 0.005 and plain > 0.8 * before, "%.3f m salted, %.3f m unsalted (from %.3f m), brine %.1f K" % [salted, plain, before, dep])
	WorldContracts.clear_overrides()
	snow.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# Floods on the ground (WorldWater into GroundSurface3D): any producer's water soaks the ground it covers, and still
# water lays its load as fresh mud; a fast current keeps it up; dry ground beside gets neither.
func _flood_ground_tests() -> void:
	if not ClassDB.class_exists("GroundSurface3D"):
		_skip("ground: floods", "no GroundSurface3D")
		return
	WorldContracts.clear_overrides()
	var ground_node := Node3D.new()
	ground_node.set_script(load("res://flat_ground.gd"))
	ground_node.name = "FloodGround"
	add_child(ground_node)
	var g: Node3D = ClassDB.instantiate("GroundSurface3D")
	g.name = "FloodSurface"
	g.gpu_simulation = false
	g.near_field = false
	g.runoff_channels = false
	g.standing_water = false
	g.wind_ripples = false
	g.world_puddles = false
	add_child(g)
	g.height_source = g.get_path_to(ground_node)
	g.follow_target = g.get_path_to(ground_node)
	await get_tree().physics_frame
	WorldContracts.add_water_stub(FloodStub.new())
	for i in 480:
		await get_tree().physics_frame
	var wet_still: float = g.get_saturation_at(Vector3(-3, 0, 0))
	var wet_fast: float = g.get_saturation_at(Vector3(-9, 0, 0))
	var dry: float = g.get_saturation_at(Vector3(3, 0, 0))
	var silt_still: float = g.get_mud_deposit_at(Vector3(-3, 0, 0))
	var silt_fast: float = g.get_mud_deposit_at(Vector3(-9, 0, 0))
	var silt_dry: float = g.get_mud_deposit_at(Vector3(3, 0, 0)) # Green-Ampt fills the top 2 cm in about 10 s: over half in 8
	_check("ground: floodwater from any producer soaks the ground it covers, not beside it", wet_still > 0.5 and wet_fast > 0.5 and dry < 0.1, "saturation %.2f under still water, %.2f under the current, %.2f on dry ground" % [wet_still, wet_fast, dry])
	_check("ground: still floodwater lays its silt as fresh mud, a current keeps it up", silt_still > 1e-6 and silt_fast == 0.0 and silt_dry == 0.0, "%.1f um of silt under still water, %.1f um under the current, %.1f um beside" % [silt_still * 1e6, silt_fast * 1e6, silt_dry * 1e6])
	WorldContracts.clear_overrides()
	g.queue_free()
	ground_node.queue_free()
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
	# A debris flow's fines dropped in at once (kg, not kg/s), and none where there is no water.
	ws.add_sediment_mass(Vector3(-5, 0, 0), 3000.0, 0.0)
	ws.add_sediment_mass(Vector3(5, 0, 0), 9000.0, 0.0)
	sed = ws.sediment_at(Vector3(-5, 0, 0))
	_check("contracts: a debris flow's fines dumped into a pond mix through it", is_equal_approx(sed.x, 3.0) and ws.sediment_at(Vector3(5, 0, 0)) == Vector2.ZERO, "%.3f kg/m3 mineral" % sed.x)
	WorldContracts.clear_overrides()
	var wx: WorldWeather = WorldContracts.get_weather()
	if not wx.has_provider():
		_check("contracts: weather defaults with no producer", wx.air_temperature_at(Vector3.ZERO) == 15.0 and wx.wind_at(Vector3.ZERO) == Vector3.ZERO and wx.rain_share_at(Vector3.ZERO) == 1.0, "")


func _glaze_tests() -> void:
	var m := _new_map()
	m.air_daily_cycle = false
	m.ground_height = 0.0
	m.temperature = -3.0
	m.humidity = 0.95
	m.wind_speed = 5.0
	m.rain_rate = 3.0
	m.cloud_cover = 1.0
	var p := Vector3(10, 0, 10)
	# No warm layer: at -3 C nearly all of it is snow, no glaze.
	m.warm_nose_temperature = 0.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(3.0 * 3600.0)
	var cold_glaze: float = m.get_glaze_at(p)
	var cold_share: float = m.get_rain_share_at(p)
	# A warm nose peaking at +4 C, its base 1 km up: the snow melts aloft, the drops reach the -3 C ground liquid.
	m.warm_nose_temperature = 4.0
	m.warm_nose_base = 1000.0
	m.snap_to_settings()
	var share: float = m.get_rain_share_at(p)
	var freezing: float = m.get_freezing_share_at(p)
	var wx: WorldWeather = WorldContracts.get_weather()
	_check("glaze: a warm layer aloft turns -3 C snow into freezing rain, through the contract too", cold_share < 0.1 and share > 0.95 and freezing > 0.95 and m.get_snowfall_at(p) < 0.1 * m.get_rain_at(p) and wx.rain_share_at(p) > 0.95, "share %.2f without, %.2f with (freezing %.2f, contract %.2f), snow %.2f mm/h" % [cold_share, share, freezing, wx.rain_share_at(p), m.get_snowfall_at(p)])
	var rate: float = m.get_rain_at(p) * freezing
	m.advance(6.0 * 3600.0)
	var glaze: float = m.get_glaze_at(p)
	var expect: float = WeatherMap3D.glaze_accretion_rate(rate, m.get_wind_at(p, 10.0).length()) * 6.0
	_check("glaze: six hours of freezing rain glaze twigs as Jones (1998), little without the warm layer", absf(glaze - expect) < 0.15 * expect and cold_glaze < 0.1 * glaze, "%.2f mm (model %.2f at %.2f mm/h), %.2f mm without the warm layer" % [glaze, expect, rate, cold_glaze])
	_check("glaze: WorldWeather.glaze_at reads the weather's glaze", wx.has_provider() and is_equal_approx(wx.glaze_at(p), glaze), "%.2f mm" % wx.glaze_at(p))
	# A deep, cold layer under the warm nose refreezes the drops to ice pellets: no glaze from them.
	m.warm_nose_base = 1500.0
	m.temperature = -10.0
	m.snap_to_settings()
	var pellets_freezing: float = m.get_freezing_share_at(p)
	_check("glaze: a deep cold layer refreezes the drops to pellets", pellets_freezing < 0.05, "freezing share %.3f" % pellets_freezing)
	# The thaw: +4 C and a fresh wind take it off.
	m.temperature = 4.0
	m.rain_rate = 0.0
	m.cloud_cover = 0.0
	m.wind_speed = 8.0
	m.snap_to_settings()
	m.advance(3.0 * 3600.0)
	var thawed: float = m.get_glaze_at(p)
	m.advance(6.0 * 3600.0)
	var gone: float = m.get_glaze_at(p)
	_check("glaze: a thaw takes the ice off (0.3 mm/h per K, Hock 2003)", absf(glaze - thawed - 3.6) < 0.4 and gone == 0.0, "%.2f mm left of %.2f after 3 h at 4 C, %.2f after 9 h" % [thawed, glaze, gone])
	# The ice-storm knob holds a glaze for tests and set pieces.
	m.ice_storm_glaze = 12.0
	_check("glaze: the ice-storm knob holds its glaze", m.get_glaze_at(p) >= 12.0 and wx.glaze_at(p) >= 12.0, "%.2f mm" % m.get_glaze_at(p))
	m.ice_storm_glaze = 0.0
	m.queue_free()
	await get_tree().physics_frame


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
		# Hill fog: ground at the cloud base stands in the cloud, in wind and under overcast; the valley below does not.
		WorldContracts.set_weather(HillCloudStub.new())
		var walker := Node3D.new()
		add_child(walker)
		mist.follow_target = mist.get_path_to(walker)
		walker.global_position = Vector3(0, 110, 0)
		var hill_level: float = mist.weather_mist_level()
		walker.global_position = Vector3(0, 20, 0)
		var valley_level: float = mist.weather_mist_level()
		WorldContracts.set_weather(null)
		walker.queue_free()
		_check("mist: ground at the cloud base (cloud_base_at) stands in hill fog, wind or not; the valley below stays clear", hill_level > 0.9 and valley_level < 0.05, "level %.2f at 110 m under a 100 m base, %.2f at 20 m" % [hill_level, valley_level])
		mist.queue_free()
		m3.queue_free()
	else:
		_skip("mist", "no VegetationMist3D")
	m.queue_free()
	m2.queue_free()
	c.queue_free()
	await get_tree().physics_frame



func _valley_fog_view(path: String, off: bool, pt: bool) -> void:
	# A valley 200 m deep after a clear calm night, seen from above its shoulder at sunrise.
	var vs := GDScript.new()
	vs.source_code = "extends Node3D\nfunc get_height(p: Vector3) -> float:\n\treturn 200.0 * (1.0 - exp(-p.x * p.x / (2.0 * 400.0 * 400.0))) + 30.0 * sin(p.z * 0.0021) * cos(p.x * 0.0013)\n"
	vs.reload()
	var valley := Node3D.new()
	valley.set_script(vs)
	add_child(valley)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step := 20.0
	var half := 1600.0
	var cells := int(2.0 * half / step)
	for zi in cells:
		for xi in cells:
			var quad := [Vector2(xi, zi), Vector2(xi + 1, zi), Vector2(xi + 1, zi + 1), Vector2(xi, zi), Vector2(xi + 1, zi + 1), Vector2(xi, zi + 1)]
			for q in quad:
				var px: float = -half + q.x * step
				var pz: float = -half + q.y * step
				st.add_vertex(Vector3(px, valley.get_height(Vector3(px, 0, pz)), pz))
	st.generate_normals()
	var ground := MeshInstance3D.new()
	ground.mesh = st.commit()
	ground.material_override = TerrainMaterial3D.new()
	add_child(ground)
	var c := _new_clock(10, 1.0)
	var m := _new_map()
	m.extent = 4096.0
	m.terrain_source = m.get_path_to(valley)
	m.terrain_wind_enabled = false
	m.sky_clock = m.get_path_to(c)
	m.air_daily_cycle = false
	m.air_follows_climate = false
	m.wind_speed = 1.0
	m.humidity = 0.95
	m.temperature = 6.0
	m.valley_fog = not off
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(5.0 * 3600.0)
	print("valley fog %.0f m deep, top %.0f m" % [m.get_valley_fog_depth_at(Vector3.ZERO), m.get_valley_fog_top_at(Vector3.ZERO)])
	c.time_of_day = 8.0 # just after sunrise: the fog lasts an hour or two into the morning
	get_viewport().size = Vector2i(640, 360) if pt else Vector2i(1280, 720)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-18, 45, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.62, 0.7, 0.82)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.6, 0.66, 0.75)
	env.environment.ambient_light_energy = 0.5
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	if pt:
		env.environment.pathtracing_samples_per_pixel = 16
		env.environment.pathtracing_max_bounces = 2
	add_child(env)
	var cam := Camera3D.new()
	cam.position = Vector3(260, 110, 260)
	cam.far = 6000.0
	add_child(cam)
	cam.look_at(Vector3(-60, 15, -150))
	cam.make_current()
	for i in 40:
		await RenderingServer.frame_post_draw
	if pt:
		env.environment.pathtracing_enabled = true
		for i in 40:
			await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


func _rime_view(path: String, off: bool, pt: bool) -> void:
	# A ridge in supercooled cloud, the wind from the left (-x): rime on the faces turned that way.
	var m := _new_map()
	m.ground_height = 0.0
	m.cloud_base_height = -200.0
	m.cloud_cover = 1.0
	m.wind_speed = 10.0
	m.wind_direction = Vector2(1, 0)
	m.temperature = -6.0
	m.humidity = 0.98
	m.air_follows_climate = false
	m.rime = not off
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(3.0 * 3600.0)
	print("rime %.1f mm" % m.get_rime_at(Vector3.ZERO).length())
	get_viewport().size = Vector2i(640, 360) if pt else Vector2i(1280, 720)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-30, -30, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.62, 0.66, 0.72)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.6, 0.64, 0.7)
	env.environment.ambient_light_energy = 0.6
	env.environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	if pt:
		env.environment.pathtracing_samples_per_pixel = 16
		env.environment.pathtracing_max_bounces = 2
	add_child(env)
	var wall := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(1.5, 3, 1.5)
	wall.mesh = bm
	wall.rotation_degrees = Vector3(0, 35, 0)
	wall.position = Vector3(2.2, 1.5, -1.5)
	wall.material_override = TerrainMaterial3D.new()
	add_child(wall)
	var gen := PlantGenerator.new()
	gen.preset = 0
	gen.apply_preset_colors()
	var fern := MeshInstance3D.new()
	fern.mesh = gen.generate()
	fern.position = Vector3(0.3, 0, 1.0)
	add_child(fern)
	var rg := RockGenerator.new()
	rg.seed = 3
	var rock := MeshInstance3D.new()
	rock.mesh = rg.generate()
	rock.position = Vector3(-1.6, 0, 0.5)
	add_child(rock)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.4, 5.5)
	cam.rotation_degrees = Vector3(-10, 0, 0)
	add_child(cam)
	cam.make_current()
	for i in 30:
		await RenderingServer.frame_post_draw
	if pt:
		env.environment.pathtracing_enabled = true
		for i in 40:
			await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


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


# Spray-wet sand holds sharper prints: the spray's film counts in the weather's surface wetness, which GroundSurface3D
# takes as its sand's moisture, so beach sand under an onshore gale gains capillary cohesion (steeper print walls)
# where the same sand beyond the spray stays loose.
func _spray_sand_tests() -> void:
	if not ClassDB.class_exists("GroundSurface3D"):
		_skip("ground: spray-wet sand", "no GroundSurface3D")
		return
	WorldContracts.clear_overrides()
	WorldContracts.add_water_stub(SurfStub.new())
	var m := _new_map()
	m.ground_height = 0.0
	m.wind_speed = 12.0
	m.wind_direction = Vector2(1, 0)
	m.temperature = 12.0
	m.humidity = 0.8
	m.air_follows_climate = false
	m.snap_to_settings()
	# The shore: a 5 m deep seabed, then a beach rising 2 in 100 (ShoreGround's), as the ground's height source.
	var shore := GDScript.new()
	shore.source_code = "extends Node3D\nfunc get_height(p: Vector3) -> float:\n\treturn -5.0 if p.x < 0.0 else p.x * 0.02\n"
	shore.reload()
	var ground_node := Node3D.new()
	ground_node.set_script(shore)
	add_child(ground_node)
	var g: Node3D = ClassDB.instantiate("GroundSurface3D")
	g.ground_type = 0 # sand
	g.gpu_simulation = false
	g.near_field = false
	g.runoff_channels = false
	g.standing_water = false
	g.wind_ripples = false
	g.world_puddles = false
	add_child(g)
	g.height_source = g.get_path_to(ground_node)
	g.follow_target = g.get_path_to(ground_node)
	await get_tree().physics_frame
	m.advance(3.0 * 3600.0)
	for i in 120:
		await get_tree().physics_frame
	var w: WorldWeather = WorldContracts.get_weather()
	var near := Vector3(4, 0.08, 0)
	var far := Vector3(220, 4.4, 0)
	var c_near: float = WorldContracts.get_ground().soil_at(near)["cohesion"]
	var c_far: float = WorldContracts.get_ground().soil_at(far)["cohesion"]
	_check("ground: spray-wet beach sand gains capillary cohesion (sharper prints), sand beyond the spray stays loose", w.surface_wetness_at(near) > 0.3 and c_near > c_far + 0.1, "wetness %.2f / %.2f, cohesion %.2f / %.2f kPa at the waterline / inland" % [w.surface_wetness_at(near), w.surface_wetness_at(far), c_near, c_far])
	WorldContracts.clear_overrides()
	g.queue_free()
	ground_node.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# Snow line and upslope rain: a 600 m ridge across a westerly; the terrain climate (TerrainDataMap3D, Smith and Barstad
# 2004) gives more on its windward side than in its lee, the weather's cells take that factor, and each cell's rain
# or snow goes by the air at its own height; the snowpack divides the weather's factor out of its own.
func _orographic_tests() -> void:
	if not ClassDB.class_exists("TerrainDataMap3D") or not ClassDB.class_exists("ClimateRecipe"):
		_skip("orographic rain", "no terrain modules")
		return
	WorldContracts.clear_overrides()
	var ridge_script := GDScript.new()
	ridge_script.source_code = "extends Node3D\nfunc get_height(p: Vector3) -> float:\n\treturn 600.0 * exp(-p.x * p.x / (2.0 * 400.0 * 400.0))\n"
	ridge_script.reload()
	var ridge := Node3D.new()
	ridge.set_script(ridge_script)
	add_child(ridge)
	var r := ClimateRecipe.new()
	r.latitude = 46.0
	r.reference_height = 0.0
	r.temperature_january = 2.0
	r.temperature_july = 19.0
	r.precipitation = 1200.0
	r.wind_from = 270.0
	r.wind_speed = 10.0
	var dm := TerrainDataMap3D.new()
	dm.size = 4096.0
	dm.resolution = 256
	add_child(dm)
	dm.height_source = dm.get_path_to(ridge)
	dm.climate_recipe = r
	await get_tree().physics_frame
	dm.bake()
	dm.bake_climate()
	var m := _new_map()
	m.extent = 4096.0
	m.terrain_source = m.get_path_to(ridge)
	m.terrain_wind_enabled = false
	m.ground_height = 0.0
	m.temperature = 3.0
	m.humidity = 0.8
	m.cloud_cover = 1.0
	m.rain_rate = 2.0
	m.wind_speed = 10.0
	m.wind_direction = Vector2(1, 0)
	m.air_follows_climate = false
	m.fog_drip = false # the summit stands in the cloud base (hill fog would wet it too)
	m.snap_to_settings()
	await get_tree().physics_frame
	# The first two minutes of rain on the dry ground: the foot's film fills, the summit's (sleet, a tenth of it liquid)
	# barely. Half an hour wets both through (the film holds a few tenths of a millimetre).
	m.advance(120.0)
	var w: WorldWeather = WorldContracts.get_weather()
	var wet_foot: float = w.surface_wetness_at(Vector3(-1600, 0, 0))
	var wet_top: float = w.surface_wetness_at(Vector3(0, 600, 0))
	m.advance(1680.0)
	w = WorldContracts.get_weather()
	var wind_side := Vector3(-450, 0, 0)
	var lee := Vector3(900, 0, 0)
	var foot := Vector3(-1600, 0, 0)
	var top := Vector3(0, 600, 0)
	var f_wind: float = WorldContracts.climate_precipitation_factor_at(wind_side)
	var f_lee: float = WorldContracts.climate_precipitation_factor_at(lee)
	var r_wind: float = w.precipitation_at(wind_side)
	var r_lee: float = w.precipitation_at(lee)
	var wf: float = w.orographic_factor_at(wind_side)
	# A ridge 1 km across in a 10 m/s wind: the linear model's condensation and fall delays (about 1000 s each) carry
	# its precipitation over the crest into the lee (spillover), so the lee gets the most here.
	_check("orographic: the weather's rain carries the terrain climate's orographic pattern over the ridge", absf(f_wind - f_lee) > 0.1 and absf(r_wind / r_lee - f_wind / f_lee) < 0.05 * f_wind / f_lee and absf(wf - f_wind) < 0.05 * f_wind, "climate factor %.2f windward, %.2f lee; rain %.2f / %.2f mm/h; weather factor %.2f" % [f_wind, f_lee, r_wind, r_lee, wf])
	_check("orographic: rain wets the foot at +3 C, the 600 m summit (-0.9 C) gets snow (the snow line by each cell's height), its surface wets slower", w.rain_share_at(Vector3(foot.x, 0, 0)) > 0.6 and w.rain_share_at(top) < 0.4 and wet_foot > 0.05 and wet_foot > 4.0 * wet_top, "rain share %.2f at the foot, %.2f on top; wetness after 2 min %.2f / %.2f" % [w.rain_share_at(Vector3(foot.x, 0, 0)), w.rain_share_at(top), wet_foot, wet_top])
	if ClassDB.class_exists("SnowCover3D"):
		var snow: Node3D = ClassDB.instantiate("SnowCover3D")
		snow.extent = 64.0
		snow.resolution = 16
		add_child(snow)
		snow.climate_map = snow.get_path_to(dm)
		var s_rate: float = snow.get_snowfall_rate_at(top)
		var expect: float = w.precipitation_at(top) * (1.0 - w.rain_share_at(top))
		_check("orographic: the snowpack does not count the climate's factor twice (its snowfall matches the weather's on the summit)", absf(s_rate - expect) < 0.2 * expect + 0.01, "snow %.3f mm/h, weather's snowfall %.3f" % [s_rate, expect])
		snow.queue_free()
	m.orographic_rain = false
	m.advance(60.0)
	var off_ratio: float = w.precipitation_at(wind_side) / maxf(w.precipitation_at(lee), 1e-4)
	_check("orographic: off, the rain is even again", absf(off_ratio - 1.0) < 0.05, "windward over lee %.2f" % off_ratio)
	WorldContracts.clear_overrides()
	m.queue_free()
	dm.queue_free()
	ridge.queue_free()
	await get_tree().physics_frame


# Cold air pools: a valley 200 m deep on a clear calm night fills with cold air; the shoulders above stay milder.
func _cold_pool_tests() -> void:
	var vs := GDScript.new()
	vs.source_code = "extends Node3D\nfunc get_height(p: Vector3) -> float:\n\treturn 200.0 * (1.0 - exp(-p.x * p.x / (2.0 * 400.0 * 400.0)))\n"
	vs.reload()
	var valley := Node3D.new()
	valley.set_script(vs)
	add_child(valley)
	var c := _new_clock(10, 1.0)
	var m := _new_map()
	m.extent = 4096.0
	m.terrain_source = m.get_path_to(valley)
	m.terrain_wind_enabled = false
	m.sky_clock = m.get_path_to(c)
	m.air_daily_cycle = false
	m.air_follows_climate = false
	m.wind_speed = 1.0
	m.humidity = 0.8
	m.temperature = 6.0
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(4.0 * 3600.0)
	var floor_p := Vector3(0, 0, 0)
	var slope_p := Vector3(500, 108, 0)
	var shoulder := Vector3(1500, 200, 0)
	var t_floor: float = m.get_air_temperature_at(floor_p)
	var t_slope: float = m.get_air_temperature_at(slope_p)
	var t_shoulder: float = m.get_air_temperature_at(shoulder)
	var pool: float = m.get_cold_pool_at(floor_p)
	_check("cold pool: a clear calm night pools cold air on the valley floor, warmer up the slope (an inversion)", pool > 4.0 and t_floor < t_slope - 2.0 and m.get_cold_pool_at(shoulder) < 0.2, "pool %.1f C; air %.1f C on the floor, %.1f C 108 m up the slope, %.1f C on the shoulder" % [pool, t_floor, t_slope, t_shoulder])
	var w: WorldWeather = WorldContracts.get_weather()
	var rh_floor: float = w.air_humidity_at(floor_p)
	var rh_shoulder: float = w.air_humidity_at(shoulder)
	var fr_floor: float = m.get_frost_at(floor_p)
	var fr_shoulder: float = m.get_frost_at(shoulder)
	_check("cold pool: the floor's air saturates (valley fog) and frosts first, the contract reads it", rh_floor > 0.98 and rh_shoulder < 0.92 and fr_floor > fr_shoulder + 0.02 and absf(w.air_temperature_at(floor_p) - t_floor) < 0.01, "humidity %.2f on the floor, %.2f on the shoulder; frost %.3f / %.3f mm" % [rh_floor, rh_shoulder, fr_floor, fr_shoulder])
	# The saturated pool fills with radiation fog, flat-topped below the shoulders (a sea of fog seen from above).
	var fog_top: float = w.valley_fog_top_at(floor_p)
	var fog_depth: float = m.get_valley_fog_depth_at(floor_p)
	var fog_side: float = w.valley_fog_top_at(Vector3(150, 0, 0))
	var fog_up: float = w.valley_fog_top_at(Vector3(300, 0, 0))
	_check("valley fog: the saturated pool fills with fog, its top level across the valley; the slopes above it and the shoulders clear", fog_depth > 20.0 and fog_top < 200.0 and absf(fog_side - fog_top) < 3.0 and fog_up == -INF and w.valley_fog_top_at(shoulder) == -INF, "%.0f m deep on the floor, top at %.0f m there and %.0f m 150 m across (ground 14 m); clear at 300 m (ground 49 m) and on the shoulder" % [fog_depth, fog_top, fog_side])
	m.wind_speed = 8.0
	m.snap_to_settings()
	m.advance(2.0 * 3600.0)
	_check("valley fog: a wind lifts it with the pool", m.get_valley_fog_depth_at(floor_p) < 1.0, "%.1f m left" % m.get_valley_fog_depth_at(floor_p))
	var windy: float = m.get_cold_pool_at(floor_p)
	_check("cold pool: a wind mixes the pool out", windy < 0.5, "pool %.2f C after 2 hours at 8 m/s" % windy)
	m.wind_speed = 1.0
	m.cold_pools = false
	m.snap_to_settings()
	m.advance(600.0)
	_check("cold pool: off, the floor has the free air's temperature", m.get_cold_pool_at(floor_p) == 0.0 and absf(m.get_air_temperature_at(floor_p) - 6.0) < 0.05, "%.2f C" % m.get_air_temperature_at(floor_p))
	m.queue_free()
	c.queue_free()
	valley.queue_free()
	await get_tree().physics_frame


# Rime: a summit in supercooled cloud and a westerly wind; the deposit grows into the wind and goes in a thaw.
func _rime_tests() -> void:
	WorldContracts.clear_overrides()
	var m := _new_map()
	m.ground_height = 1200.0
	m.cloud_base_height = 1000.0
	m.cloud_cover = 1.0
	m.wind_speed = 10.0
	m.wind_direction = Vector2(1, 0)
	m.temperature = -6.0
	m.humidity = 0.98
	m.air_follows_climate = false
	m.snap_to_settings()
	await get_tree().physics_frame
	m.advance(6.0 * 3600.0)
	var w: WorldWeather = WorldContracts.get_weather()
	var at := Vector3(0, 1200, 0)
	var lwc: float = w.fog_water_at(at)
	var r: Vector3 = w.rime_at(at)
	var rate: float = WeatherMap3D.rime_accretion_rate(lwc, 10.0, -6.0)
	_check("rime: a summit in supercooled cloud grows rime into the wind (Makkonen 2000), about the rate's worth", r.length() > 5.0 and r.x < -0.95 * r.length() and absf(r.length() - 6.0 * rate) < 0.2 * 6.0 * rate + 1.0, "%.1f mm toward (%.2f, %.2f) in 6 h of %.2f g/m3 cloud at 10 m/s (%.1f mm/h)" % [r.length(), r.x / maxf(r.length(), 1e-4), r.z / maxf(r.length(), 1e-4), lwc, rate])
	var warm: float = WeatherMap3D.rime_accretion_rate(lwc, 10.0, 0.5)
	m.cloud_base_height = 2500.0
	m.temperature = 4.0
	m.snap_to_settings()
	m.advance(6.0 * 3600.0)
	var left: float = w.rime_at(at).length()
	_check("rime: none grows above freezing (wet growth is glaze), and a thaw out of the cloud sheds it", warm == 0.0 and left < 0.5, "%.2f mm/h at +0.5 C; %.2f mm left after 6 h at +4 C" % [warm, left])
	m.rime = false
	_check("rime: off, none", w.rime_at(at) == Vector3(), "")
	m.queue_free()
	await get_tree().physics_frame


# Salt bloom on the terrain's sea cliffs: a rock wall (TerrainMaterial3D, steep faces are rock) under the spray's salt
# (weather_spray_map G) and without it.
func _salt_scene(p_pt: bool) -> Node3D:
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
		env.environment.pathtracing_samples_per_pixel = 16
		env.environment.pathtracing_max_bounces = 2
	root.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 30, 0)
	root.add_child(sun)
	var wall := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(8, 4, 0.5)
	wall.mesh = bm
	wall.position = Vector3(0, 2, -3)
	wall.material_override = TerrainMaterial3D.new()
	root.add_child(wall)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 2, 2)
	root.add_child(cam)
	cam.make_current()
	var img := Image.create(4, 4, false, Image.FORMAT_RG8)
	img.fill(Color(0, 0.6, 0)) # about 9 g/m2 of salt
	RenderingServer.global_shader_parameter_set("weather_spray_map", ImageTexture.create_from_image(img))
	RenderingServer.global_shader_parameter_set("weather_spray_rect", Vector4(-50, -50, 100, 100))
	return root


func _salt_view(path: String, salt: bool, pt: bool) -> void:
	get_viewport().size = Vector2i(640, 360) if pt else Vector2i(1280, 720)
	var root := _salt_scene(pt)
	if not salt:
		RenderingServer.global_shader_parameter_set("weather_spray_rect", Vector4(0, 0, 0, 0))
	for i in 20:
		await RenderingServer.frame_post_draw
	if pt:
		(root.get_child(0) as WorldEnvironment).environment.pathtracing_enabled = true
		for i in 40:
			await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("saved ", path)


func _salt_bloom_tests() -> void:
	if not ClassDB.class_exists("TerrainMaterial3D") or DisplayServer.get_name() == "headless":
		_skip("salt bloom", "needs a rendering device")
		return
	var root := _salt_scene(false)
	for i in 20:
		await RenderingServer.frame_post_draw
	var salted := _centre_luminance(get_viewport().get_texture().get_image())
	RenderingServer.global_shader_parameter_set("weather_spray_rect", Vector4(0, 0, 0, 0))
	for i in 5:
		await RenderingServer.frame_post_draw
	var bare := _centre_luminance(get_viewport().get_texture().get_image())
	root.queue_free()
	await get_tree().process_frame
	_check("salt bloom: the spray's salt crusts the sea cliff's dry rock white (terrain material), none without salt", salted > 1.2 * bare, "centre luminance %.3f salted, %.3f bare" % [salted, bare])


func _centre_luminance(img: Image) -> float:
	var w := img.get_width()
	var h := img.get_height()
	var sum := 0.0
	var n := 0
	for y in range(int(h * 0.3), int(h * 0.7), 4):
		for x in range(int(w * 0.3), int(w * 0.7), 4):
			sum += img.get_pixel(x, y).get_luminance()
			n += 1
	return sum / maxf(n, 1)


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
	# The gas reads the ground's colour under it from the world (ground_from_world).
	WorldContracts.set_ground(RedGround.new())
	for i in 40:
		await RenderingServer.frame_post_draw
	var wg: Color = flow.get_world_ground_albedo()
	WorldContracts.set_ground(null)
	_check("ground colour: the gas reads the ground under it from the world", wg.a == 1.0 and is_equal_approx(wg.r, 0.4) and is_equal_approx(wg.b, 0.05), str(wg))

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


# The ground's colour (WorldGround.albedo_at) for the light it sends up into gas: the terrain material's layers as its
# shader weighs them, darkened when wet by porosity, under the snow's cover; PhysXFlow3D reads it under its gas.
func _ground_color_tests() -> void:
	if not ClassDB.class_exists("TerrainMaterial3D"):
		_skip("ground colour", "needs the terrain modules")
		return
	var gen := TerrainGenerator3D.new()
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.tile_erosion_enabled = false
	gen.strata_enabled = false
	gen.guide_preview = false
	gen.dunes_enabled = false
	gen.deposition = 0.0
	gen.moraine_age = 0.0
	gen.scree_age = 0.0
	gen.meander_age = 0.0
	gen.outwash_age = 0.0
	gen.size = Vector2(512, 512)
	gen.cell_size = 8.0
	gen.seed = 4
	gen.uplift = 3.0
	add_child(gen)
	gen.generate()
	var g := WorldContracts.get_ground()
	var none: Color = g.albedo_at(Vector3(100, 0, 100))
	var tm := TerrainMaterial3D.new()
	var colors := [Color(0.3, 0.35, 0.6), Color(0.45, 0.45, 0.45), Color(0.5, 0.25, 0.15), Color(0.35, 0.3, 0.2), Color(0.8, 0.7, 0.5)]
	for i in 5:
		var l := TerrainMaterialLayer.new()
		l.color = colors[i]
		l.porosity = 0.5
		tm.set_layer(i, l)
	tm.sea_level = -1000.0
	tm.use_generator(gen)
	# The steepest and flattest points on a 16 m grid.
	var steep_p := Vector3.ZERO
	var flat_p := Vector3.ZERO
	var best_steep := -1.0
	var best_flat := 1e9
	for z in range(-200, 201, 16):
		for x in range(-200, 201, 16):
			var p := Vector3(x, 0, z)
			var s := absf(gen.get_height(p + Vector3(2, 0, 0)) - gen.get_height(p - Vector3(2, 0, 0))) + absf(gen.get_height(p + Vector3(0, 0, 2)) - gen.get_height(p - Vector3(0, 0, 2)))
			if s > best_steep:
				best_steep = s
				steep_p = p
			if s < best_flat:
				best_flat = s
				flat_p = p
	var a_steep: Color = g.albedo_at(steep_p)
	var a_flat: Color = g.albedo_at(flat_p)
	var a_out: Color = g.albedo_at(Vector3(5000, 0, 5000))
	var rock: Color = (colors[0] as Color).srgb_to_linear()
	var blue_steep := a_steep.b / maxf(a_steep.r, 1e-4)
	var blue_flat := a_flat.b / maxf(a_flat.r, 1e-4)
	_check("ground colour: none without a terrain material; with one, its layers' colours as laid (rock on the steepest ground, bluer here, than the flattest); none past the terrain", none.a == 0.0 and a_flat.a == 1.0 and a_steep.a == 1.0 and blue_steep > blue_flat and a_steep.b <= rock.b + 1e-4 and a_out.a == 0.0, "no material %s; steep %s (B/R %.2f), flat %s (B/R %.2f); outside a %.1f" % [none, a_steep, blue_steep, a_flat, blue_flat, a_out.a])
	WorldContracts.set_weather(SoakedStub.new())
	var a_wet: Color = g.albedo_at(flat_p)
	WorldContracts.set_weather(null)
	WorldContracts.set_snow(DeepSnowStub.new())
	var p_snow := Vector3(absf(flat_p.x) + 1.0, 0, flat_p.z)
	var a_snow: Color = g.albedo_at(p_snow)
	var a_snow_far: Color = g.albedo_at(Vector3(5000, 0, 5000))
	var a_bare: Color = g.albedo_at(Vector3(-absf(flat_p.x) - 1.0, 0, flat_p.z))
	WorldContracts.set_snow(null)
	# Porosity 0.5: soaked keeps mix(1, 0.2, 0.5) = 0.6 of the dry albedo.
	_check("ground colour: rain darkens it by porosity (0.6 of dry soaked at porosity 0.5), snow covers it with its albedo, snow beyond the terrain answers alone", is_equal_approx(a_wet.r, a_flat.r * 0.6) and is_equal_approx(a_snow.r, 0.7) and is_equal_approx(a_snow.b, 0.7) and a_snow.a == 1.0 and is_equal_approx(a_snow_far.r, 0.7) and a_snow_far.a == 1.0 and a_bare.a == 1.0 and a_bare.r < 0.6, "wet %s (dry %s), snow %s, snow past the terrain %s, bare %s" % [a_wet, a_flat, a_snow, a_snow_far, a_bare])
	tm = null
	gen.queue_free()
	await _ground_color_meadow_tests()


# The plants' colour over the ground (WorldPlantColorProvider, GrassField3D's sward cover): gas over a meadow takes its
# green, gas over a worn path through it the bare tread.
func _ground_color_meadow_tests() -> void:
	if not ClassDB.class_exists("GrassField3D"):
		_skip("ground colour meadow", "needs the vegetation module")
		return
	var root := Node3D.new()
	add_child(root)
	var w := 65
	var data := PackedFloat32Array()
	data.resize(w * w)
	var shape := HeightMapShape3D.new()
	shape.map_width = w
	shape.map_depth = w
	shape.map_data = data
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	body.add_child(cs)
	root.add_child(body)
	var dm := TerrainDataMap3D.new()
	dm.size = 64.0
	dm.resolution = 64
	root.add_child(dm)
	await get_tree().physics_frame
	dm.bake()
	var grass := GrassField3D.new()
	grass.radius = 8.0
	# A worn path along x = 0 (1500 walkers a year, bare soil in its tread).
	grass.paths = [{"points": PackedVector3Array([Vector3(0, 0, -30), Vector3(0, 0, 30)]), "width": 1.5, "passes": 1500.0}]
	root.add_child(grass)
	grass.data_map = grass.get_path_to(dm)
	for i in 10:
		grass.update_at(Vector3(10, 0, 0))
		await get_tree().process_frame
	var meadow: Color = WorldContracts.get_ground().albedo_at(Vector3(12, 0, 5))
	var path: Color = WorldContracts.get_ground().albedo_at(Vector3(0, 0, 5))
	var far: Color = grass.get_far_color()
	root.queue_free()
	await get_tree().process_frame
	_check("ground colour: gas over a meadow takes the sward's green (its far colour), over a worn path through it little of it", meadow.a > 0.8 and meadow.g > meadow.r and absf(meadow.g - far.g) < 0.02 and path.a < 0.5 * meadow.a, "meadow %s (far %s), path %s" % [meadow, far, path])

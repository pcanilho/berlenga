extends Node3D
# SnowCover3D bench: checks the snowpack against the published relations, and renders snow on a mountain
# or on props, raster or path traced.
#
#   --test            print the checks and quit (--test=climate: stage 4's only):
#                       snowfall share against Jennings et al. (2018) and its 50% temperature by humidity
#                       new snow density against Hedstrom and Pomeroy (1998)
#                       a snowfall: water kept, depth, settling toward the CLASS maximum
#                       melt by temperature (Pellicciotti et al. 2005), water kept as liquid, runoff
#                       melt by sunlight on a south face against a north face
#                       the snow line on a mountain against the lapse rate
#                       SnowSlide on a 60 degree face: what it holds, mass kept
#                       drift over a ridge: lee against windward, mass kept
#                       the cost of one update
#                       stage 4: the winter generator (WGEN statistics, the sun's path), a season baked over a
#                       mountain (snow line, north against south faces, lee against windward), cornices on a
#                       ridge in the right wind only, the near square seeded from the world tier, a cornice
#                       mesh you can stand on
#   --view=mountain   a 700 m mountain after a day of snow and two days of sun (snow line, south faces melted)
#   --view=trail      deep snow as geometry: a walker, rolling balls and a wheel leave tracks (SnowSurface3D)
#   --view=snowfall   falling snow over deep snow and a shed (flakes stop under the roof); --wind=M m/s
#                     at 10 m (default 4; 15 or more blows the snow off the ground)
#   --view=climate   a winter baked over the mountain (SnowClimate): snow line, shaded north faces, loaded lee
#   --view=cornice   a corniced ridge from below its lee face
#   --view=sparkle    dry new snow at the feet against a low sun ahead: glinting grains; --melt=H hours of thaw
#                     first wets it (the glints go); prints the share of glinting pixels
#   --view=shelter    a shed and young spruces after a snowfall: bare ground under the roof, snow in the
#                     crowns and less under them (--test=shelter for the checks)
#   --view=props      close: bumpy ground, boxes, a sphere, a 45 degree board and a 70 degree wall after snow
#   --snow=H          hours of 2 mm/h precipitation (default 24 mountain, 8 props)
#   --sun=H           then hours of sun (mountain; default 48)
#   --steps=N         save N frames as the snow builds (time-lapse) to --out with _NNN before .png
#   --probe           print the snowpack along the axes (mountain view)
#   --raster          raster only (default path traced)
#   --out=path.png    save the last frame and quit
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)

const GroundScript := preload("res://ground.gd")

var view := "mountain"
var raster := false
var test := false
var out := ""
var spp := 16
var bounces := 3
var snow_hours := -1.0
var sun_hours := -1.0
var steps := 0
var n := 0
var snow: SnowCover3D
var weather: WeatherMap3D
var ground: Node3D
var sun: DirectionalLight3D
var fails := 0
var surface: SnowSurface3D
var walker: CharacterBody3D
var walk_t := 0.0
var walking := false
var wind := 4.0
var only := ""
var melt_hours := 0.0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a.begins_with("--test"):
			test = true
			only = a.substr(7)
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--snow="): snow_hours = float(a.substr(7))
		elif a.begins_with("--sun="): sun_hours = float(a.substr(6))
		elif a.begins_with("--steps="): steps = int(a.substr(8))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--melt="): melt_hours = float(a.substr(7))
	if test:
		if only == "climate":
			await _run_climate_tests()
		elif only == "shelter":
			await _run_shelter_tests()

		else:
			await _run_tests()
		print("SNOW CHECKS: ", "PASS" if fails == 0 else "%d FAILED" % fails)
		get_tree().quit()
		return
	_build_view()


func _check(label: String, ok: bool, detail: String) -> void:
	print(("  ok   " if ok else "  FAIL ") + label + ": " + detail)
	if not ok:
		fails += 1


func _setup(kind: String, extent: float, resolution: int, temperature: float) -> void:
	for c in get_children():
		c.queue_free()
	await get_tree().process_frame
	ground = GroundScript.new()
	ground.kind = kind
	ground.name = "Ground"
	add_child(ground)
	sun = DirectionalLight3D.new()
	sun.name = "Sun"
	add_child(sun)
	weather = WeatherMap3D.new()
	weather.extent = 8192.0
	weather.resolution = 32
	weather.temperature = temperature
	weather.humidity = 0.9
	weather.cloud_cover = 0.0
	weather.cloud_variation = 0.0
	weather.wind_speed = 3.0
	weather.rain_rate = 0.0
	weather.set_physics_process_internal(false)
	add_child(weather)
	weather.snap_to_settings()
	weather.advance(1.0)
	snow = SnowCover3D.new()
	snow.name = "SnowCover3D"
	snow.extent = extent
	snow.resolution = resolution
	snow.height_source = NodePath("../Ground")
	snow.sun_light = NodePath("../Sun")
	snow.follow_target = NodePath("../Ground")
	snow.update_rate = 0.0001
	add_child(snow)
	snow.refresh_heights()
	sun.visible = false


func _precipitate(hours: float, rate: float) -> void:
	# Overcast while it falls (the weather map spreads precipitation where the cloud is), clear after.
	weather.rain_rate = rate
	weather.cloud_cover = 1.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(hours * 3600.0)
	weather.rain_rate = 0.0
	weather.cloud_cover = 0.0
	weather.snap_to_settings()
	weather.advance(1.0)


func _run_tests() -> void:
	print("Rain or snow (Jennings et al. 2018): share of snow at")
	for t: float in [-2.0, 0.0, 1.0, 2.0, 3.0, 4.0]:
		var row := "  %+.0f C:" % t
		for rh: float in [0.5, 0.7, 0.9, 1.0]:
			row += "  RH %d%% %.2f" % [int(rh * 100), SnowCover3D.snowfall_fraction(t, rh)]
		print(row)
	for rh: float in [0.5, 0.7, 0.9]:
		var t50: float = (10.04 - 9.0 * rh) / 1.41
		var lo := -10.0
		var hi := 10.0
		for i in 40:
			var mid := (lo + hi) * 0.5
			if SnowCover3D.snowfall_fraction(mid, rh) > 0.5: lo = mid
			else: hi = mid
		_check("50%% snow at RH %d%%" % int(rh * 100), absf(lo - t50) < 0.01, "%.2f C (formula %.2f C)" % [lo, t50])

	print("New snow density (Hedstrom and Pomeroy 1998):")
	for t: float in [-15.0, -5.0, -2.0, 0.0]:
		var want: float = 67.92 + 51.25 * exp(t / 2.59)
		_check("%+.0f C" % t, absf(SnowCover3D.new_snow_density(t) - want) < 0.1, "%.1f kg/m3 (formula %.1f)" % [SnowCover3D.new_snow_density(t), want])

	# A snowfall on flat ground: 10 hours of 2 mm/h at -5 C.
	await _setup("flat", 256.0, 64, -5.0)
	_precipitate(10.0, 2.0)
	var p := Vector3(0, 0, 0)
	var water := snow.get_total_water()
	var area := 256.0 * 256.0
	_check("snowfall keeps the water", absf(water / area - 20.0) < 0.05, "%.2f mm of 20 mm" % (water / area))
	var rho0 := SnowCover3D.new_snow_density(-5.0)
	print("  after the fall: %.1f cm deep, %.0f kg/m3 (fresh %.0f), albedo %.2f" % [snow.get_depth_at(p) * 100.0, snow.get_density_at(p), rho0, snow.get_albedo_at(p)])
	var rho_before := snow.get_density_at(p)
	snow.advance(48.0 * 3600.0)
	var d := snow.get_depth_at(p)
	var rho_max := 450.0 - 204.7 / d * (1.0 - exp(-d / 0.673))
	print("  48 h later: %.1f cm deep, %.0f kg/m3 (CLASS maximum %.0f), albedo %.2f" % [d * 100.0, snow.get_density_at(p), rho_max, snow.get_albedo_at(p)])
	_check("settling", snow.get_density_at(p) > rho_before and snow.get_density_at(p) < rho_max + 1.0, "denser, below the maximum")
	_check("settling keeps the water", absf(snow.get_total_water() / area - 20.0) < 0.05, "%.2f mm" % (snow.get_total_water() / area))

	# Melt by temperature: 100 mm at +5 C, no sun, a day.
	await _setup("flat", 256.0, 64, 5.0)
	snow.add_snow(100.0, 300.0)
	snow.advance(24.0 * 3600.0)
	var left := snow.get_total_water() / area
	var melt := 0.05 * 5.0 * 24.0
	var kept := 0.05 * (100.0 - melt)
	_check("melt by temperature", absf((100.0 - left) - (melt - kept)) < 0.1, "%.2f mm left the pack: TF T = %.1f mm melted, %.1f mm stays as liquid (5%% of the ice)" % [100.0 - left, melt, kept])
	_check("runoff", snow.get_runoff_at(p) > 0.2 and snow.get_runoff_at(p) < 0.3, "%.3f mm/h (0.25 melts)" % snow.get_runoff_at(p))
	print("  wetness %.2f, albedo %.2f" % [snow.get_wetness_at(p), snow.get_albedo_at(p)])

	# Sun on a ridge running north-south, flanks facing east and west; sun low in the east.
	await _setup("ridge", 512.0, 128, 3.0)
	snow.add_snow(200.0, 300.0)
	sun.visible = true
	sun.rotation_degrees = Vector3(-30.0, 90.0, 0.0) # from the east (+x), 30 degrees up
	snow.advance(12.0 * 3600.0)
	var east := 200.0 - snow.get_swe_at(Vector3(150.0, 0, 0))
	var west := 200.0 - snow.get_swe_at(Vector3(-150.0, 0, 0))
	var flat := 200.0 - snow.get_swe_at(Vector3(240.0, 0, 0))
	print("  12 h, 3 C, low sun from the east: melted %.1f mm on the east flank, %.1f mm on flat ground, %.1f mm on the west flank" % [east, flat, west])
	_check("sun-facing slopes melt first", east > flat and flat > west, "east > flat > west")

	# Snow line: 2 km mountain, 4 C at the foot, humid air; a day of 2 mm/h.
	await _setup("mountain", 2048.0, 128, 4.0)
	_precipitate(24.0, 2.0)
	var line := _snow_line()
	var prof := "  radial (ground m / SWE mm / liquid mm):"
	for r in range(400, 800, 25):
		var q := Vector3(r, 0, 0)
		prof += "  %.0f/%.1f" % [snow.get_ground_height_at(q), snow.get_swe_at(q)]
	print(prof)
	for hh: float in [300.0, 400.0, 450.0, 500.0, 600.0]:
		print("  T at %.0f m: %.2f C, snow share %.2f" % [hh, 4.0 - 6.5 * hh / 1000.0, SnowCover3D.snowfall_fraction(4.0 - 6.5 * hh / 1000.0, 0.9)])
	var t50_90 := (10.04 - 8.1) / 1.41
	var want_line := (4.0 - t50_90) / 6.5 * 1000.0
	_check("snow line", absf(line - want_line) < 60.0, "%.0f m where half the precipitation is snow (lapse rate: %.0f m)" % [line, want_line])

	# SnowSlide on a 60 degree face.
	await _setup("wall", 256.0, 128, -5.0)
	snow.drift_enabled = false
	var before := snow.get_total_water()
	snow.add_snow(400.0, 200.0)
	before = snow.get_total_water()
	snow.advance(600.0)
	var row := "  profile (x m: ground m / SWE mm):"
	for x in range(-6, 46, 4):
		row += "  %d: %.0f/%.0f" % [x, snow.get_ground_height_at(Vector3(x + 1, 0, 1)), snow.get_swe_at(Vector3(x + 1, 0, 1))]
	print(row)
	var face := snow.get_swe_at(Vector3(12.0, 0, 0))
	var foot := snow.get_swe_at(Vector3(-2.0, 0, 0))
	var hold60 := SnowCover3D.holding_depth(60.0, 50.0, 0.074)
	_check("slide: the face holds", face <= hold60 + 1.0, "%.0f mm on the face (SnowSlide holds %.0f mm at 60 degrees)" % [face, hold60])
	_check("slide: the foot gains", foot > 400.0, "%.0f mm at the foot" % foot)
	_check("slide keeps the water", absf(snow.get_total_water() - before) / before < 0.001, "%.4f%% change" % (100.0 * (snow.get_total_water() - before) / before))

	# Drift over a ridge with 14 m/s from the west (toward +x).
	await _setup("ridge", 512.0, 128, -8.0)
	weather.wind_speed = 14.0
	weather.wind_direction = Vector2(1, 0)
	weather.snap_to_settings()
	weather.advance(1.0)
	_precipitate(10.0, 2.0)
	var lee := snow.get_swe_at(Vector3(60.0, 0, 0))
	var windward := snow.get_swe_at(Vector3(-60.0, 0, 0))
	var mean := snow.get_total_water() / (512.0 * 512.0)
	print("  lee flank %.1f mm, windward flank %.1f mm, map mean %.1f mm" % [lee, windward, mean])
	_check("drift: lee gains", lee > windward * 1.2, "lee / windward %.2f" % (lee / maxf(windward, 0.01)))
	_check("drift keeps the water", absf(mean - 20.0) < 0.5, "%.2f mm of 20 mm" % mean)

	await _run_surface_tests()
	await _run_fall_tests()
	await _run_shelter_tests()
	await _run_climate_tests()

	# Cost: a 512 m map at 2 m (256 x 256), snowing with drift.
	await _setup("mountain", 512.0, 256, -2.0)
	weather.rain_rate = 2.0
	weather.wind_speed = 12.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.add_snow(300.0, 200.0)
	snow.advance(60.0) # the first update bakes Sx for this wind direction
	print("  first update, blocking here (bakes Sx for the wind; on worker threads in a running game): %.2f ms" % snow.get_update_time_msec())
	for mode in ["drift and slide", "no slide", "no drift, no slide"]:
		snow.slide_enabled = mode == "drift and slide"
		snow.drift_enabled = mode != "no drift, no slide"
		var total := 0.0
		for i in 10:
			snow.advance(60.0)
			total += snow.get_update_time_msec()
		print("  one update, 256 x 256 cells, %s: %.2f ms" % [mode, total / 10.0])


func _surface_setup(swe: float, density: float) -> void:
	await _setup("flat", 64.0, 64, -4.0)
	var floor_body := StaticBody3D.new()
	var floor_shape := CollisionShape3D.new()
	floor_shape.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(floor_shape)
	add_child(floor_body)
	snow.add_snow(swe, density)
	var probe := Marker3D.new()
	probe.name = "Probe"
	add_child(probe)
	surface = SnowSurface3D.new()
	surface.name = "Surface"
	surface.snow_cover = NodePath("../SnowCover3D") if has_node("SnowCover3D") else NodePath("")
	surface.follow_target = NodePath("../Probe")
	add_child(surface)
	await get_tree().physics_frame
	await get_tree().physics_frame


func _run_surface_tests() -> void:
	print("Deep snow (SnowSurface3D):")
	# Bekker for a boot: 80 kg on 0.035 m2, 0.11 m wide, Wong's snow values.
	var z_boot := SnowSurface3D.bekker_sinkage(80.0 * 9.81 / 1000.0 / 0.035, sqrt(0.035 / 3.0), 1.6, 4.37, 196.72)
	_check("Bekker sinkage of a boot in fresh snow", z_boot > 0.15 and z_boot < 0.4, "%.2f m at %.1f kPa" % [z_boot, 80.0 * 9.81 / 1000.0 / 0.035])

	# 45 mm of water at 110 kg/m3: 0.41 m of fresh snow.
	await _surface_setup(45.0, 110.0)
	var d0 := snow.get_depth_at(Vector3.ZERO)
	var p := Vector3(1.03, 0, 0.51)
	_check("surface stands on the pack", absf(surface.get_surface_height_at(p) - d0) < 0.01, "%.3f m surface, %.3f m pack" % [surface.get_surface_height_at(p), d0])
	# A flat foot pressed all the way: the column compacts to 450 kg/m3 with 30% pushed into the rim.
	var rho := snow.get_density_at(p)
	var want := d0 * (1.0 - rho / 450.0) / (1.0 - 0.3 * rho / 450.0)
	var moved := surface.press(Vector3(p.x, 0.0, p.z), 0.3, -1.0, 0.0, false)
	var dent := surface.get_dent_at(p)
	_check("pressed column stops at the compacted density", absf(dent - want) < 0.005, "dent %.3f m, expected %.3f m" % [dent, want])
	var left := d0 - dent
	var mass_left := d0 * rho - 0.3 * dent * rho
	_check("compacted column density", absf(mass_left / left - 450.0) < 5.0, "%.0f kg/m3" % (mass_left / left))
	var vol := surface.get_moved_volume()
	_check("rim holds the pushed share (mass conserved)", absf(vol.y - 0.3 * vol.x) < 0.01 * vol.x, "dent %.4f m3, rim %.4f m3 (%.1f%%), moved %.4f m3" % [vol.x, vol.y, 100.0 * vol.y / vol.x, moved])
	var rim_at := surface.get_rim_at(Vector3(p.x + 0.36, 0, p.z))
	_check("a raised rim beside the print", rim_at > 0.01, "%.3f m at 6 cm outside" % rim_at)

	# A boot print turned 90 degrees: long along x, narrow along z.
	var bp := Vector3(6.0, 0, -6.0)
	surface.press_box(bp, Vector2(0.1, 0.28), PI * 0.5, -1.0, 0.0)
	var along := surface.get_dent_at(bp + Vector3(0.08, 0, 0))
	var across := surface.get_dent_at(bp + Vector3(0, 0, 0.12))
	_check("boot prints follow the heading", along > 0.3 and across < 0.05, "%.3f m along the boot, %.3f m beside it" % [along, across])

	# Collision follows the deformed surface.
	await get_tree().create_timer(0.2).timeout
	var space := get_world_3d().direct_space_state
	var q := PhysicsRayQueryParameters3D.create(Vector3(p.x, 5, p.z), Vector3(p.x, -5, p.z))
	var hit := space.intersect_ray(q)
	var hy: float = hit.position.y if hit else -99.0
	_check("collision sits in the print", absf(hy - (d0 - dent)) < 0.03, "ray hits %.3f m, surface %.3f m" % [hy, d0 - dent])

	# A 25 kg ball (radius 0.25 m) dropped on the snow sinks by Bekker and rests in its own print.
	var ball := RigidBody3D.new()
	ball.mass = 25.0
	var bs := CollisionShape3D.new()
	bs.shape = SphereShape3D.new()
	(bs.shape as SphereShape3D).radius = 0.25
	ball.add_child(bs)
	ball.add_to_group("snow_deform")
	ball.position = Vector3(-4.0, d0 + 0.6, -3.0)
	add_child(ball)
	for i in 240:
		await get_tree().physics_frame
	var sink := d0 - (ball.global_position.y - 0.25)
	var bd := surface.get_dent_at(ball.global_position)
	_check("a ball sinks into fresh snow and rests there", sink > 0.03 and sink < want + 0.03 and absf(sink - bd) < 0.04, "sank %.3f m, print %.3f m, floor %.3f m" % [sink, bd, want])
	ball.queue_free()

	# A walker leaves a line of prints and the trail is kept when the field moves away and back.
	var wk := CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.15
	(ws.shape as CapsuleShape3D).height = 1.8
	wk.add_child(ws)
	wk.add_to_group("grass_trample") # the grass's movers press the snow too
	add_child(wk)
	for i in 60:
		wk.global_position = Vector3(-3.0 + i * 0.1, d0 + 0.9, 3.0)
		await get_tree().physics_frame
	var trail := surface.get_dent_at(Vector3(0.0, 0, 3.0))
	_check("a character walking leaves a trail", trail > 0.1, "%.3f m deep" % trail)
	wk.queue_free()
	var probe: Node3D = get_node("Probe")
	probe.global_position = Vector3(300.0, 0, 0)
	for i in 4:
		await get_tree().physics_frame
	var cached := surface.get_cached_tile_count()
	probe.global_position = Vector3.ZERO
	for i in 4:
		await get_tree().physics_frame
	var back := surface.get_dent_at(Vector3(0.0, 0, 3.0))
	_check("trails survive leaving and coming back", cached > 0 and absf(back - trail) < 0.002, "%d tiles cached, %.3f m after, %.3f m before" % [cached, back, trail])

	# New snow settles into the prints; the pack settling shrinks the step; melt takes it all.
	var before := surface.get_dent_at(p)
	snow.add_snow(10.0, 100.0) # 0.1 m of new snow
	await get_tree().physics_frame
	var after := surface.get_dent_at(p)
	_check("new snow fills prints (refill_share 0.5)", absf((before - after) - 0.05) < 0.005, "%.3f m to %.3f m" % [before, after])
	snow.advance(14.0 * 24.0 * 3600.0)
	await get_tree().physics_frame
	var rho2 := snow.get_density_at(p)
	var d2 := snow.get_depth_at(p)
	var cap := d2 * (1.0 - rho2 / 450.0) / (1.0 - 0.3 * rho2 / 450.0)
	_check("settled pack: the print is no deeper than the compaction allows", surface.get_dent_at(p) <= cap + 0.002, "%.3f m, limit %.3f m (pack %.0f kg/m3, %.2f m)" % [surface.get_dent_at(p), cap, rho2, d2])
	weather.temperature = 10.0
	weather.snap_to_settings()
	weather.advance(1.0)
	sun.visible = true
	sun.rotation_degrees = Vector3(-60, 0, 0)
	snow.advance(30.0 * 24.0 * 3600.0)
	await get_tree().physics_frame
	_check("melted out: no prints, surface on the ground", surface.get_dent_at(p) == 0.0 and absf(surface.get_surface_height_at(p)) < 0.001, "dent %.3f, surface %.3f, pack %.3f m" % [surface.get_dent_at(p), surface.get_surface_height_at(p), snow.get_depth_at(p)])

	# Cost of a frame with four movers in the field.
	await _surface_setup(45.0, 110.0)
	for k in 4:
		var b := RigidBody3D.new()
		b.mass = 30.0
		var c := CollisionShape3D.new()
		c.shape = SphereShape3D.new()
		b.add_child(c)
		b.add_to_group("snow_deform")
		b.position = Vector3(-6.0 + k * 4.0, 1.0, 0.0)
		b.linear_velocity = Vector3(0, 0, 3.0)
		add_child(b)
	var tot := 0.0
	for i in 60:
		await get_tree().physics_frame
		tot += surface.get_step_time_msec()
	print("  SnowSurface3D step with 4 rolling bodies (press, relax, collision at 15 Hz, upload): %.2f ms" % (tot / 60.0))


func _run_fall_tests() -> void:
	print("Falling and blowing snow (SnowFall3D):")
	var n1 := SnowFall3D.flake_number_density(1.0, 2.0)
	var lam := 2.55
	var want := 3.8e3 / lam * exp(-lam * pow(4.0, 1.0 / 3.0))
	_check("Gunn-Marshall flakes over 2 mm at 1 mm/h", absf(n1 - want) < 0.01 * want, "%.1f per m3 (formula %.1f)" % [n1, want])
	var n02 := SnowFall3D.flake_number_density(0.2, 2.0)
	var n5 := SnowFall3D.flake_number_density(5.0, 2.0)
	_check("more and bigger flakes in heavier snow", n02 < n1 and n1 < n5, "%.2f, %.1f, %.0f per m3 at 0.2, 1, 5 mm/h" % [n02, n1, n5])
	_check("Locatelli-Hobbs fall speed", absf(SnowFall3D.fall_speed(1.0) - 0.8) < 1e-4 and absf(SnowFall3D.fall_speed(10.0) - 0.8 * pow(10.0, 0.16)) < 1e-4, "%.2f m/s at 1 mm, %.2f m/s at 10 mm" % [SnowFall3D.fall_speed(1.0), SnowFall3D.fall_speed(10.0)])
	var vis1 := 3.912 / SnowFall3D.snowfall_extinction(1.0)
	var vis3 := 3.912 / SnowFall3D.snowfall_extinction(3.0)
	_check("visibility in snowfall (Rasmussen et al. 1999: km at 1 mm/h, a few hundred m in heavy snow)", vis1 > 1000.0 and vis1 < 4000.0 and vis3 > 100.0 and vis3 < 500.0, "%.0f m at 1 mm/h, %.0f m at 3 mm/h" % [vis1, vis3])
	_check("Li-Pomeroy threshold wind", absf(SnowFall3D.blowing_threshold(-10.0) - (9.43 - 1.8 + 0.33)) < 1e-3 and absf(SnowFall3D.blowing_threshold(0.0) - 9.43) < 1e-3, "%.2f m/s at -10 C, %.2f at 0 C" % [SnowFall3D.blowing_threshold(-10.0), SnowFall3D.blowing_threshold(0.0)])
	var ut := SnowFall3D.blowing_threshold(-6.0)
	_check("saltation flux", SnowFall3D.saltation_flux_ratio(ut * 0.9, ut) == 0.0 and absf(SnowFall3D.saltation_flux_ratio(ut * 1.5, ut) - 1.0) < 1e-4 and SnowFall3D.saltation_flux_ratio(ut * 2.0, ut) > 2.0, "0 below the threshold, 1 at 1.5x, %.2f at 2x" % SnowFall3D.saltation_flux_ratio(ut * 2.0, ut))
	# In a scene: snowfall from the weather at -6 C, then rain at +6 C; calm, then a gale.
	await _setup("flat", 64.0, 64, -6.0)
	snow.add_snow(30.0, 120.0)
	weather.rain_rate = 2.0
	weather.cloud_cover = 1.0
	weather.wind_speed = 3.0
	weather.snap_to_settings()
	weather.advance(1.0)
	var fall := SnowFall3D.new()
	fall.follow_target = NodePath("../Ground")
	add_child(fall)
	await get_tree().process_frame
	await get_tree().process_frame
	_check("snowfall at -6 C fills the box", fall.get_snowfall_rate() > 1.5 and fall.get_visible_flakes() > 1000, "%.2f mm/h of snow, %d flakes shown" % [fall.get_snowfall_rate(), fall.get_visible_flakes()])
	_check("no blowing in a light wind", fall.get_blowing_amount() == 0.0, "%.1f m/s against %.1f m/s" % [fall.get_wind_speed_10m(), fall.get_threshold_wind()])
	weather.wind_speed = 15.0
	weather.snap_to_settings()
	weather.advance(1.0)
	await get_tree().process_frame
	_check("a gale lifts the snow", fall.get_blowing_amount() > 0.5, "%.0f%% of the grains at %.1f m/s" % [fall.get_blowing_amount() * 100.0, fall.get_wind_speed_10m()])
	weather.temperature = 6.0
	weather.snap_to_settings()
	weather.advance(1.0)
	await get_tree().process_frame
	_check("no flakes and no blowing at +6 C", fall.get_visible_flakes() == 0 and fall.get_blowing_amount() == 0.0, "%.3f mm/h of snow, %d flakes" % [fall.get_snowfall_rate(), fall.get_visible_flakes()])
	fall.queue_free()


func _shed(pos: Vector3, size: Vector3, height: float) -> StaticBody3D:
	# A roof on four posts: its collider catches the snow over the ground below.
	var body := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = size
	shape.shape = box
	body.add_child(shape)
	body.position = pos + Vector3(0, height, 0)
	add_child(body)
	return body


func _run_shelter_tests() -> void:
	print("Snow under roofs and trees")
	# A roof 3 m up over flat ground; snow cast down finds the roof where it covers the cell.
	await _setup("flat", 32.0, 64, -6.0)
	snow.height_source = NodePath("")
	snow.drift_enabled = false
	var floor_body := StaticBody3D.new()
	var floor_shape := CollisionShape3D.new()
	floor_shape.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(floor_shape)
	add_child(floor_body)
	_shed(Vector3(0, 0, 0), Vector3(6, 0.2, 4), 3.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	snow.refresh_heights()
	_precipitate(6.0, 2.0)
	var roof_h := snow.get_ground_height_at(Vector3(0, 0, 0))
	var open_h := snow.get_ground_height_at(Vector3(8, 0, 0))
	_check("a roof catches the snow over the ground below", roof_h > 2.9 and absf(open_h) < 0.05 and snow.get_swe_at(Vector3(0, 0, 0)) > 5.0, "catch %.2f m under the roof, %.2f m in the open, %.1f mm on the roof" % [roof_h, open_h, snow.get_swe_at(Vector3(0, 0, 0))])
	# With a height source the roof is only found on shelter_collision_mask.
	ground.set("kind", "flat")
	snow.height_source = NodePath("../Ground")
	snow.shelter_collision_mask = 0
	snow.refresh_heights()
	var h_off := snow.get_ground_height_at(Vector3(0, 0, 0))
	snow.shelter_collision_mask = 1
	snow.refresh_heights()
	var h_on := snow.get_ground_height_at(Vector3(0, 0, 0))
	_check("height source plus shelter_collision_mask finds the roof", absf(h_off) < 0.05 and h_on > 2.9, "%.2f m without the mask, %.2f m with it" % [h_off, h_on])

	# Trees (Hedstrom and Pomeroy 1998): a stand of closure about 0.85 takes much of a 20 mm fall, then
	# unloads it onto the snow below in a mild wind as it warms toward thaw.
	await _setup("flat", 64.0, 64, -6.0)
	snow.drift_enabled = false
	weather.add_canopy(Vector2(0, 0), 14.0, 15.0, 4.0)
	weather.rebake_shelter()
	snow.refresh_heights()
	var closure := weather.get_canopy_closure_at(Vector3(0, 0, 0))
	var w0 := snow.get_total_water()
	_precipitate(10.0, 2.0)
	var held := snow.get_canopy_load_at(Vector3(0, 0, 0))
	var cap := snow.get_canopy_capacity_at(Vector3(0, 0, 0))
	var under := snow.get_swe_at(Vector3(0, 0, 0))
	var open := snow.get_swe_at(Vector3(26, 0, 26))
	var w1 := snow.get_total_water()
	print("  closure %.2f: canopy holds %.1f mm of %.1f mm capacity; %.1f mm under it, %.1f mm in the open" % [closure, held, cap, under, open])
	_check("a canopy holds snow and shades the ground", closure > 0.5 and held > 3.0 and held <= cap + 0.01 and under < open * 0.8 and absf(under + held - open) < open * 0.1, "%.1f + %.1f mm against %.1f in the open" % [under, held, open])
	_check("interception keeps the water", absf(w1 - w0 - (open * 64.0 * 64.0)) < open * 64.0 * 64.0 * 0.05, "%.0f kg on the map against %.0f fallen" % [w1 - w0, open * 64.0 * 64.0])
	weather.temperature = -1.0
	weather.wind_speed = 6.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(24.0 * 3600.0)
	var held2 := snow.get_canopy_load_at(Vector3(0, 0, 0))
	var under2 := snow.get_swe_at(Vector3(0, 0, 0))
	_check("it unloads onto the snow below", held2 < held * 0.5 and under2 > under, "%.1f mm left in the canopy after a day at -1 C in 6 m/s wind; %.1f mm below (was %.1f)" % [held2, under2, under])


func _climate(wind_speed: float) -> SnowClimate:
	var c := SnowClimate.new()
	c.latitude = 46.0
	c.base_height = 0.0
	c.january_temperature = -1.0
	c.july_temperature = 17.0
	c.wind_speed = wind_speed
	c.prevailing_wind_from = 270.0
	c.seed = 3
	return c


func _alpine(wind_speed: float) -> SnowClimate:
	# A high, cold ridge: -8 C in January at its foot.
	var c := _climate(wind_speed)
	c.january_temperature = -8.0
	c.july_temperature = 10.0
	return c


func _run_climate_tests() -> void:
	print("Stage 4: the climate bake")
	# The winter generator against its own recipe over ten years (WGEN, Richardson 1981).
	var c := _climate(5.0)
	c.start_day = 1
	var days := c.sample_days(3650)
	var jan := 0.0
	var jan_n := 0
	var wet := 0
	var wet_total := 0.0
	for d in 3650:
		if d % 365 < 31:
			jan += days[d * 4]
			jan_n += 1
		if days[d * 4 + 3] > 0.5:
			wet += 1
			wet_total += days[d * 4 + 1]
	var want_wet := c.wet_after_dry / (1.0 + c.wet_after_dry - c.wet_after_wet)
	_check("generator: January mean", absf(jan / jan_n - (-1.0)) < 0.6, "%.2f C (recipe -1.0, seasonal curve %.2f)" % [jan / jan_n, c.seasonal_temperature(16.0)])
	_check("generator: wet days (Markov chain)", absf(float(wet) / 3650.0 - want_wet) < 0.03, "%.3f (stationary %.3f)" % [float(wet) / 3650.0, want_wet])
	_check("generator: water on a wet day", absf(wet_total / wet - c.wet_day_precipitation) < 1.0, "%.1f mm (recipe %.1f)" % [wet_total / wet, c.wet_day_precipitation])
	var noon_winter := c.sun_direction(355.0, 12.0)
	var noon_summer := c.sun_direction(172.0, 12.0)
	_check("the sun's path at 46 N", absf(rad_to_deg(asin(noon_winter.y)) - 20.56) < 0.5 and absf(rad_to_deg(asin(noon_summer.y)) - 67.44) < 0.5 and noon_winter.z > 0.9 * noon_winter.length() * cos(asin(noon_winter.y)), "noon elevation %.1f deg at midwinter, %.1f at midsummer (90 - 46 -/+ 23.44), from the south" % [rad_to_deg(asin(noon_winter.y)), rad_to_deg(asin(noon_summer.y))])

	# A winter, 1 October to 1 April, over the 700 m mountain: 4 km at 16 m.
	await _setup("mountain", 512.0, 128, -2.0)
	# Mild at the foot (0.5 C in January), so by April the snow line sits on the mountain.
	var mc := _climate(5.0)
	mc.january_temperature = 0.5
	mc.july_temperature = 18.5
	snow.climate = mc
	var t0 := Time.get_ticks_msec()
	snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 256)
	var bake_s := (Time.get_ticks_msec() - t0) / 1000.0
	print("  bake: 182 days in 3 h steps on 256 x 256 cells: %.1f s" % bake_s)
	var bins := {}
	var north := 0.0
	var south := 0.0
	var north_n := 0
	var south_n := 0
	var east := 0.0
	var west := 0.0
	var east_n := 0
	var west_n := 0
	for i in 20000:
		var p := Vector3(randf_range(-1400, 1400), 0, randf_range(-1400, 1400))
		var h: float = ground.get_height(p)
		var swe := snow.get_world_swe_at(p)
		var b := int(h / 100.0)
		if not bins.has(b):
			bins[b] = [0, 0]
		bins[b][0] += 1
		if swe > 10.0:
			bins[b][1] += 1
		# Aspect from the ground's slope.
		var gx: float = ground.get_height(p + Vector3(8, 0, 0)) - ground.get_height(p - Vector3(8, 0, 0))
		var gz: float = ground.get_height(p + Vector3(0, 0, 8)) - ground.get_height(p - Vector3(0, 0, 8))
		var slope := Vector2(gx, gz) / 16.0
		if h > 250.0 and h < 500.0 and slope.length() > 0.2:
			# The ground falls toward -slope: facing north (-z) when gz > 0.
			if slope.y > 0.6 * slope.length():
				north += swe
				north_n += 1
			elif slope.y < -0.6 * slope.length():
				south += swe
				south_n += 1
		if h > 300.0 and h < 600.0 and slope.length() > 0.2:
			if slope.x < -0.6 * slope.length():
				east += swe
				east_n += 1
			elif slope.x > 0.6 * slope.length():
				west += swe
				west_n += 1
	var keys := bins.keys()
	keys.sort()
	var row := "  covered share by height:"
	var shares := []
	for k in keys:
		var share: float = float(bins[k][1]) / bins[k][0]
		shares.append(share)
		row += "  %d m %.2f" % [k * 100, share]
	print(row)
	var low: float = shares[0]
	var high: float = shares[shares.size() - 1]
	var line_m := -1.0
	for k in keys:
		if float(bins[k][1]) / bins[k][0] >= 0.5:
			line_m = k * 100.0
			break
	_check("a snow line: bare foot, white top", low < 0.2 and high > 0.9 and line_m > 0.0, "%.2f covered at the foot, %.2f at the top, half covered from %.0f m" % [low, high, line_m])
	north /= maxf(north_n, 1)
	south /= maxf(south_n, 1)
	_check("north faces keep more than south faces", north > south * 1.15, "%.0f mm against %.0f mm at 250-500 m (%d, %d samples)" % [north, south, north_n, south_n])
	east /= maxf(east_n, 1)
	west /= maxf(west_n, 1)
	_check("the lee (east) gains against the windward (west)", east > west * 1.1, "%.0f mm against %.0f mm at 300-600 m" % [east, west])
	# The near square starts from the world tier where it arrives.
	snow.advance(1.0)
	var ok_seed := true
	var worst := 0.0
	for q in [Vector3(100, 0, 100), Vector3(-150, 0, 60), Vector3(40, 0, -200)]:
		var a := snow.get_swe_at(q)
		var w := snow.get_world_swe_at(q)
		worst = maxf(worst, absf(a - w) / maxf(w, 1.0))
		if absf(a - w) > 0.1 * maxf(w, 10.0):
			ok_seed = false
	_check("the near square starts from the world tier", ok_seed, "largest difference %.1f%%" % (worst * 100.0))
	_check("the world tier is up for materials", snow.has_world() and snow.get_world_texture() != null and snow.get_world_rect().size.x >= 4096.0, "world rect %s" % str(snow.get_world_rect()))
	t0 = Time.get_ticks_usec()
	snow.advance_world(3600.0)
	print("  world tier, 256 x 256 cells, one update: %.1f ms" % ((Time.get_ticks_usec() - t0) / 1000.0))

	# Cornices on a ridge: a winter of westerlies in the band where they grow (Yu et al. 2023), then too
	# little wind, then gales.
	var lengths := []
	for ws: float in [5.0, 1.0, 22.0]:
		await _setup("cornice", 256.0, 64, -2.0)
		snow.climate = _alpine(ws)
		snow.bake_climate(Rect2(-512, -512, 1024, 1024), 128)
		var crest := Vector2.ZERO
		var windward := 0.0
		for z in range(-300, 300, 8):
			for x in range(-16, 8, 4):
				var cv := snow.get_cornice_at(Vector3(x, 0, z))
				if cv.length() > crest.length():
					crest = cv
			for x in range(-400, -60, 16):
				windward = maxf(windward, snow.get_cornice_at(Vector3(x, 0, z)).length())
		lengths.append([crest, windward])
		print("  wind %.0f m/s mean: longest cornice %.2f m toward %s, windward %.2f m" % [ws, crest.length(), str(crest.normalized()), windward])
	var good: Vector2 = lengths[0][0]
	_check("cornices grow on the lee crest, out with the wind", good.length() > 1.0 and good.length() < 12.5 and good.normalized().x > 0.9 and lengths[0][1] < 0.01, "%.2f m toward +x, none windward" % good.length())
	# A milder ridge on a coarse world tier (16 m cells): -4.5 C in January at the crest, 5 m/s mean. The
	# crest's speed-up (Taylor and Lee 1984) carries the snow; thaws waste it back slowly.
	await _setup("cornice", 256.0, 64, -2.0)
	var mild := _climate(5.0)
	mild.january_temperature = 1.0
	mild.base_height = -700.0
	snow.climate = mild
	snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 256)
	var mild_len := 0.0
	for z in range(-300, 300, 8):
		for x in range(-40, 24, 4):
			mild_len = maxf(mild_len, snow.get_cornice_at(Vector3(x, 0, z)).length())
	_check("cornices on a milder ridge at 16 m cells", mild_len > 1.0, "%.2f m" % mild_len)
	_check("no cornices in light wind", lengths[1][0].length() < 0.3, "%.2f m" % lengths[1][0].length())
	_check("gales keep them short", lengths[2][0].length() < good.length() * 0.5, "%.2f m against %.2f m" % [lengths[2][0].length(), good.length()])
	# The mesh, with collision.
	await _setup("cornice", 256.0, 64, -2.0)
	snow.climate = _alpine(5.0)
	snow.bake_climate(Rect2(-512, -512, 1024, 1024), 128)
	var cor := SnowCornice3D.new()
	cor.follow_target = NodePath("../Ground")
	add_child(cor)
	cor.rebuild()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var space := get_world_3d().direct_space_state
	var hits := 0
	var tries := 0
	for z in range(-200, 200, 20):
		var cv := snow.get_cornice_at(Vector3(-4, 0, z))
		if cv.length() < 2.0:
			continue
		tries += 1
		var top: float = ground.get_height(Vector3(-4, 0, z)) + 50.0
		var rq := PhysicsRayQueryParameters3D.create(Vector3(cv.length() * 0.5, top, z), Vector3(cv.length() * 0.5, -10.0, z))
		var hit := space.intersect_ray(rq)
		var gy: float = ground.get_height(Vector3(cv.length() * 0.5, 0, z))
		if hit and hit.position.y > gy + 1.0:
			hits += 1
	_check("cornice mesh: overhangs with collision", cor.get_segment_count() > 10 and tries > 0 and hits >= tries * 0.8, "%d segments; a ray down halfway out stops on the cornice above the lee face in %d of %d places" % [cor.get_segment_count(), hits, tries])
	cor.queue_free()


func _snow_line() -> float:
	# The lowest ground where at least half the day's precipitation (48 mm) stayed as snow.
	var lowest := INF
	var rect := snow.get_map_rect()
	for i in 4000:
		var p := Vector3(rect.position.x + randf() * rect.size.x, 0, rect.position.y + randf() * rect.size.y)
		if snow.get_swe_at(p) >= 24.0:
			lowest = minf(lowest, snow.get_ground_height_at(p))
	return lowest


# ---------------------------------------------------------------------------------------------------------
# Views

func _build_view() -> void:
	if view == "mountain":
		await _setup("mountain", 2048.0, 256, 4.0)
	elif view == "trail":
		await _setup("meadow", 64.0, 64, -4.0)
	elif view == "snowfall":
		await _setup("meadow", 64.0, 64, -6.0)
	elif view == "climate":
		await _setup("mountain", 512.0, 256, -2.0)
	elif view == "cornice":
		await _setup("cornice", 512.0, 256, -6.0)
	elif view == "sparkle":
		await _setup("flat", 32.0, 128, -6.0)
	elif view == "shelter":
		await _setup("flat", 32.0, 128, -6.0)
	else:
		await _setup("flat", 32.0, 128, -4.0)
	var env := Environment.new()
	# A sky, so path traced shadows are lit by it as raster's are by the ambient.
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var cam := Camera3D.new()
	add_child(cam)
	if view == "mountain":
		sun.visible = true
		sun.shadow_enabled = true
		# Low winter sun from the south (+z): south faces melt, north faces keep their snow.
		sun.rotation_degrees = Vector3(-25.0, 0.0, 0.0)
		_add_terrain(2048.0, 256, Color(0.33, 0.31, 0.27), 64.0)
		cam.position = Vector3(2100.0, 1150.0, 250.0)
		cam.far = 8000.0
		cam.look_at(Vector3(0, 250, 0))
		cam.fov = 45.0
	elif view == "trail":
		await _build_trail(cam)
		return
	elif view == "snowfall":
		await _build_snowfall(cam)
		return
	elif view == "climate" or view == "cornice":
		await _build_climate(cam)
		return
	elif view == "sparkle":
		await _build_sparkle(cam)
		return
	elif view == "shelter":
		await _build_shelter(cam)
		return
	else:
		snow.drift_enabled = false
		sun.visible = true
		sun.shadow_enabled = true
		sun.rotation_degrees = Vector3(-40.0, -35.0, 0.0)
		_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
		_add_props()
		# The snowpack finds the ground and the prop tops by casting down.
		snow.height_source = NodePath("")
		var floor_body := StaticBody3D.new()
		var floor_shape := CollisionShape3D.new()
		floor_shape.shape = WorldBoundaryShape3D.new()
		floor_body.add_child(floor_shape)
		add_child(floor_body)
		cam.position = Vector3(0.0, 4.2, 6.8)
		cam.look_at(Vector3(0.0, 0.6, 0.0))
		cam.fov = 50.0
	await get_tree().physics_frame
	await get_tree().physics_frame
	snow.refresh_heights()
	var hours := snow_hours if snow_hours >= 0.0 else (24.0 if view == "mountain" else 8.0)
	if steps > 0:
		for s in steps:
			_precipitate(hours / steps, 2.0)
			await _save_frame("_%03d" % s)
	else:
		_precipitate(hours, 2.0)
	if view == "mountain":
		var sun_h := sun_hours if sun_hours >= 0.0 else 48.0
		if steps > 0:
			for s in steps:
				snow.advance(sun_h / steps * 3600.0)
				await _save_frame("_%03d" % (steps + s))
		else:
			snow.advance(sun_h * 3600.0)
	if OS.get_cmdline_user_args().has("--probe"):
		for r in [0.0, 100.0, 200.0, 300.0, 400.0, 500.0, 600.0, 800.0]:
			for q in [Vector3(r, 0, 0), Vector3(0, 0, r), Vector3(0, 0, -r)]:
				print("probe %s ground %.0f swe %.1f depth %.2f rho %.0f alb %.2f" % [q, snow.get_ground_height_at(q), snow.get_swe_at(q), snow.get_depth_at(q), snow.get_density_at(q), snow.get_albedo_at(q)])
	await _save_frame("")
	get_tree().quit()


func _build_trail(cam: Camera3D) -> void:
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	# Low afternoon sun across the tracks so their walls cast shadows.
	# Low sun from behind the tracks, toward the camera and from the left: the print walls throw
	# shadows forward and the rims catch the light.
	sun.rotation_degrees = Vector3(-16.0, 140.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	_add_terrain(64.0, 256, Color(0.36, 0.33, 0.28), 6.0)
	cam.position = Vector3(2.6, 1.35, 5.4)
	cam.look_at(Vector3(-0.6, 0.15, 0.6))
	cam.fov = 55.0
	var target := Marker3D.new()
	target.name = "Probe"
	add_child(target)
	await get_tree().physics_frame
	snow.refresh_heights()
	var hours := snow_hours if snow_hours >= 0.0 else 0.0
	if hours > 0.0:
		_precipitate(hours, 2.0)
	else:
		snow.add_snow(45.0, 110.0) # 0.41 m of fresh snow
	surface = SnowSurface3D.new()
	surface.snow_cover = NodePath("../SnowCover3D")
	surface.follow_target = NodePath("../Probe")
	add_child(surface)
	# Movers, driven on set paths so the shot is the same each run: a walker whose two boots print the
	# snow (boot boxes in the snow_deform group, 80 kg on 0.035 m2: Bekker), a cart wheel and two balls
	# rolling through it at a set depth.
	var dark := StandardMaterial3D.new()
	dark.albedo_color = Color(0.15, 0.17, 0.22)
	walker = CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.14
	(ws.shape as CapsuleShape3D).height = 1.6
	walker.add_child(ws)
	var wm := MeshInstance3D.new()
	var cm := CapsuleMesh.new()
	cm.radius = 0.14
	cm.height = 1.6
	wm.mesh = cm
	wm.material_override = dark
	walker.add_child(wm)
	walker.position = Vector3(-7.0, 1.5, 1.3)
	add_child(walker)
	for k in 2:
		var foot := Node3D.new()
		foot.name = "Foot%d" % k
		var fs := CollisionShape3D.new()
		fs.shape = BoxShape3D.new()
		(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
		foot.add_child(fs)
		var fm := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(0.11, 0.06, 0.29)
		fm.mesh = bm
		fm.material_override = dark
		foot.add_child(fm)
		foot.set_meta("snow_mass", 80.0)
		foot.set_meta("snow_contact_area", 0.035)
		foot.add_to_group("snow_deform")
		foot.visible = false
		add_child(foot)
	var ball_mat := StandardMaterial3D.new()
	ball_mat.albedo_color = Color(0.6, 0.12, 0.08)
	for k in 2:
		var b := Node3D.new()
		b.name = "Ball%d" % k
		var c := CollisionShape3D.new()
		c.shape = SphereShape3D.new()
		(c.shape as SphereShape3D).radius = 0.22 + 0.1 * k
		b.add_child(c)
		var bmi := MeshInstance3D.new()
		var sm := SphereMesh.new()
		sm.radius = 0.22 + 0.1 * k
		sm.height = 2.0 * sm.radius
		bmi.mesh = sm
		bmi.material_override = ball_mat
		b.add_child(bmi)
		b.add_to_group("snow_deform")
		add_child(b)
	var wheel := Node3D.new()
	wheel.name = "Wheel"
	var wc := CollisionShape3D.new()
	wc.shape = CylinderShape3D.new()
	(wc.shape as CylinderShape3D).radius = 0.4
	(wc.shape as CylinderShape3D).height = 0.12
	wc.rotation_degrees = Vector3(90, 0, 0)
	wheel.add_child(wc)
	var wmi := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.4
	cyl.bottom_radius = 0.4
	cyl.height = 0.12
	wmi.mesh = cyl
	wmi.rotation_degrees = Vector3(90, 0, 0)
	wmi.material_override = dark
	wheel.add_child(wmi)
	wheel.add_to_group("snow_deform")
	add_child(wheel)
	walking = true
	# Path tracing is slow on a CPU device: simulate under raster, trace only the saved frames.
	var we: WorldEnvironment = null
	for c in get_children():
		if c is WorldEnvironment:
			we = c
	if we and not raster:
		we.environment.pathtracing_enabled = false
	var frames := steps if steps > 0 else 1
	var secs := 6.5
	for s in frames:
		for i in int(secs * 60.0 / frames):
			await get_tree().physics_frame
		if steps > 0:
			walking = false # hold the movers while the frame is saved
			await _save_frame("_%03d" % s)
			walking = true
	walking = false
	print("surface step %.2f ms" % surface.get_step_time_msec())
	if we and not raster:
		we.environment.pathtracing_enabled = true
	await _save_frame("")
	get_tree().quit()


func _build_climate(cam: Camera3D) -> void:
	# A winter baked over the terrain (1 October to 1 April), drawn as it stands at the end.
	sun.visible = true
	sun.shadow_enabled = true
	if view == "climate":
		var mc := _climate(5.0)
		mc.january_temperature = 0.5
		mc.july_temperature = 18.5
		snow.climate = mc
		snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 256)
		_add_terrain(4096.0, 256, Color(0.33, 0.31, 0.27), 128.0)
		# Afternoon sun from the south-west; seen from the east-north-east, so the shaded north faces, the
		# sunny south faces and the loaded lee (east) all show.
		sun.rotation_degrees = Vector3(-28.0, -140.0, 0.0)
		cam.position = Vector3(2500.0, 1250.0, -900.0)
		cam.far = 9000.0
		cam.look_at(Vector3(0, 200, 0))
		cam.fov = 42.0
	else:
		snow.climate = _alpine(5.0)
		snow.bake_climate(Rect2(-512, -512, 1024, 1024), 128)
		_add_terrain(1024.0, 512, Color(0.36, 0.34, 0.31), 64.0)
		var cor := SnowCornice3D.new()
		cor.name = "Cornices"
		cor.follow_target = NodePath("../Ground")
		add_child(cor)
		cor.rebuild()
		var row := "cornice lengths along the crest:"
		for z in range(-80, 81, 16):
			row += " %d: %.1f" % [z, snow.get_cornice_at(Vector3(-4, 0, z)).length()]
		print(row, ", %d segments" % cor.get_segment_count())
		# From the lee side, along the crest, so the overhang shows in profile; morning sun from the south-east.
		sun.rotation_degrees = Vector3(-30.0, 50.0, 0.0)
		cam.position = Vector3(26.0, 146.0, 40.0)
		cam.far = 3000.0
		cam.look_at(Vector3(-2.0, 150.0, -10.0))
		cam.fov = 50.0
	var we: WorldEnvironment = null
	for c in get_children():
		if c is WorldEnvironment:
			we = c
	if we and not raster:
		we.environment.pathtracing_enabled = true
	await get_tree().physics_frame
	await get_tree().process_frame
	await _save_frame("")
	get_tree().quit()


func _build_shelter(cam: Camera3D) -> void:
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-35.0, -30.0, 0.0)
	_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
	snow.height_source = NodePath("")
	var floor_body := StaticBody3D.new()
	var floor_shape := CollisionShape3D.new()
	floor_shape.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(floor_shape)
	add_child(floor_body)
	# A lean-to shed: a sloping roof on posts, open at the front.
	var wood := _ground_material(Color(0.42, 0.28, 0.16), 1.0)
	_prop(BoxMesh.new(), Vector3(5.0, 0.12, 3.4), Vector3(-2.5, 2.6, -1.5), Vector3(10, 0, 0), wood)
	for p in [Vector3(-4.8, 1.25, -0.1), Vector3(-0.2, 1.25, -0.1), Vector3(-4.8, 1.25, -2.9), Vector3(-0.2, 1.25, -2.9)]:
		var post := MeshInstance3D.new()
		post.mesh = BoxMesh.new()
		post.scale = Vector3(0.15, 2.5, 0.15)
		post.position = p
		post.material_override = wood
		add_child(post)
	# Young spruces: no colliders (the snow below is the ground's); their crowns stand in the weather map's
	# canopy and their needles take what the canopy holds.
	var needles := StandardMaterial3D.new()
	needles.albedo_color = Color(0.09, 0.17, 0.1)
	needles.roughness = 0.8
	needles.snow_enabled = true
	needles.snow_max_slope = 75.0
	var bark := StandardMaterial3D.new()
	bark.albedo_color = Color(0.25, 0.18, 0.12)
	for t in [Vector3(3.0, 0, -2.5), Vector3(5.2, 0, -0.5), Vector3(2.4, 0, 0.8), Vector3(6.0, 0, -3.6)]:
		weather.add_canopy(Vector2(t.x, t.z), 1.6, 7.0, 4.0)
		var trunk := MeshInstance3D.new()
		trunk.mesh = CylinderMesh.new()
		trunk.scale = Vector3(0.25, 1.0, 0.25)
		trunk.position = t + Vector3(0, 1.0, 0)
		trunk.material_override = bark
		add_child(trunk)
		for k in 4:
			var cone := MeshInstance3D.new()
			var cm := CylinderMesh.new()
			cm.top_radius = 0.0
			cm.bottom_radius = 1.7 - 0.35 * k
			cm.height = 2.2
			cm.radial_segments = 24
			cone.mesh = cm
			cone.position = t + Vector3(0, 2.4 + 1.25 * k, 0)
			cone.material_override = needles
			add_child(cone)
	weather.rebake_shelter()
	cam.position = Vector3(0.5, 5.2, 10.0)
	cam.look_at(Vector3(0.6, 1.4, -1.5))
	cam.fov = 55.0
	await get_tree().physics_frame
	await get_tree().physics_frame
	snow.refresh_heights()
	_precipitate(snow_hours if snow_hours >= 0.0 else 10.0, 2.0)
	print("shelter: %.1f mm under the roof, %.1f mm open, canopy %.1f of %.1f mm, %.1f mm under a crown" % [snow.get_swe_at(Vector3(-2.5, 0, -1.5)), snow.get_swe_at(Vector3(0, 0, 4)), snow.get_canopy_load_at(Vector3(3, 0, -2.5)), snow.get_canopy_capacity_at(Vector3(3, 0, -2.5)), snow.get_swe_at(Vector3(3, 0, -2.5))])
	await _save_frame("")
	get_tree().quit()


func _build_sparkle(cam: Camera3D) -> void:
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	# A low sun ahead, shining toward the camera: the grains tilted to mirror it flash.
	sun.rotation_degrees = Vector3(-12.0, 172.0, 0.0)
	_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
	cam.position = Vector3(0.0, 1.5, 6.0)
	cam.look_at(Vector3(0.0, 0.0, -3.0))
	cam.fov = 50.0
	await get_tree().physics_frame
	snow.refresh_heights()
	_precipitate(8.0, 2.0)
	if melt_hours > 0.0:
		weather.temperature = 3.0
		weather.snap_to_settings()
		weather.advance(1.0)
		snow.advance(melt_hours * 3600.0)
	var p := Vector3(0, 0, 0)
	print("sparkle: depth %.2f m, density %.0f, albedo %.2f" % [snow.get_depth_at(p), snow.get_density_at(p), snow.get_albedo_at(p)])
	await _save_frame("")
	# The share of glinting pixels: over the snow below the horizon, those a fifth brighter than the ring of
	# pixels around them (a wide highlight is not a glint).
	var img := get_viewport().get_texture().get_image()
	var w := img.get_width()
	var h := img.get_height()
	var lum := PackedFloat32Array()
	lum.resize(w * h)
	for y in h:
		for x in w:
			var c := img.get_pixel(x, y)
			lum[y * w + x] = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
	var count := 0
	var total := 0
	for y in range(h / 2, h - 2):
		for x in range(2, w - 2):
			var ring := 0.0
			for d in [Vector2i(-2, 0), Vector2i(2, 0), Vector2i(0, -2), Vector2i(0, 2), Vector2i(-2, -2), Vector2i(2, 2), Vector2i(-2, 2), Vector2i(2, -2)]:
				ring += lum[(y + d.y) * w + x + d.x]
			total += 1
			if lum[y * w + x] > ring / 8.0 * 1.2:
				count += 1
	print("sparkle: glinting %.3f%% of %d pixels" % [100.0 * count / total, total])
	get_tree().quit()


func _undisturbed(p: Vector3) -> float:
	return surface.get_surface_height_at(p) + surface.get_dent_at(p) - surface.get_rim_at(p)


func _build_snowfall(cam: Camera3D) -> void:
	sun.visible = true
	sun.shadow_enabled = true
	sun.light_energy = 0.6
	sun.rotation_degrees = Vector3(-30.0, 150.0, 0.0)
	_add_terrain(64.0, 256, Color(0.36, 0.33, 0.28), 6.0)
	var target := Marker3D.new()
	target.name = "Probe"
	add_child(target)
	await get_tree().physics_frame
	snow.refresh_heights()
	snow.add_snow(30.0, 120.0)
	weather.rain_rate = 3.0
	weather.cloud_cover = 1.0
	weather.wind_speed = wind
	weather.snap_to_settings()
	weather.advance(1.0)
	var deep := SnowSurface3D.new()
	deep.snow_cover = NodePath("../SnowCover3D")
	deep.follow_target = NodePath("../Probe")
	add_child(deep)
	# A dark timber shed: the roof keeps the snow off the ground under it.
	var wood := _ground_material(Color(0.2, 0.13, 0.08), 1.0)
	wood.snow_enabled = false
	for c in [Vector2(-1.5, -1.5), Vector2(1.5, -1.5), Vector2(-1.5, 1.5), Vector2(1.5, 1.5)]:
		var post := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(0.15, 2.6, 0.15)
		post.mesh = bm
		post.material_override = wood
		post.position = Vector3(c.x - 1.0, 1.1, c.y - 3.0)
		add_child(post)
	var roof := MeshInstance3D.new()
	var rm := BoxMesh.new()
	rm.size = Vector3(3.6, 0.12, 3.6)
	roof.mesh = rm
	roof.material_override = wood
	roof.position = Vector3(-1.0, 2.45, -3.0)
	add_child(roof)
	var back := MeshInstance3D.new()
	var wm := BoxMesh.new()
	wm.size = Vector3(3.3, 2.4, 0.1)
	back.mesh = wm
	back.material_override = wood
	back.position = Vector3(-1.0, 1.1, -4.6)
	add_child(back)
	var fall := SnowFall3D.new()
	fall.snow_cover = NodePath("../SnowCover3D")
	fall.follow_target = NodePath("../Probe")
	add_child(fall)
	cam.position = Vector3(1.6, 1.4, 3.2)
	cam.look_at(Vector3(-1.0, 1.0, -3.0))
	cam.fov = 55.0
	var we: WorldEnvironment = null
	for c in get_children():
		if c is WorldEnvironment:
			we = c
	if we and not raster:
		we.environment.pathtracing_enabled = false
	# The snowfall past the particle box as fog, from its extinction.
	await get_tree().process_frame
	if we:
		we.environment.fog_enabled = true
		we.environment.fog_light_color = Color(0.78, 0.8, 0.84)
		we.environment.fog_density = fall.get_extinction()
		we.environment.fog_sky_affect = 0.3
	var frames := steps if steps > 0 else 1
	for s in frames:
		for i in 150 / frames + 1:
			await get_tree().process_frame
		if steps > 0:
			await _save_frame("_%03d" % s)
	print("visibility %.0f m" % fall.get_visibility())
	print("snowfall %.2f mm/h, %d flakes, blowing %.0f%% at %.1f m/s (threshold %.1f)" % [fall.get_snowfall_rate(), fall.get_visible_flakes(), fall.get_blowing_amount() * 100.0, fall.get_wind_speed_10m(), fall.get_threshold_wind()])
	if we and not raster:
		we.environment.pathtracing_enabled = true
	await _save_frame("")
	get_tree().quit()


func _physics_process(delta: float) -> void:
	if not walking or walker == null:
		return
	walk_t += delta
	# The walker: 1.4 m/s on a gentle S, a 0.7 m stride, boots planted for one step each.
	var x := -7.0 + 1.4 * walk_t
	var z := 1.3 + 0.5 * sin(walk_t * 0.9)
	walker.velocity = (Vector3(x, walker.global_position.y, z) - walker.global_position) / delta
	walker.velocity.y = -2.0
	walker.move_and_slide()
	var step := int(walk_t * 1.4 / 0.7)
	for k in 2:
		var foot: Node3D = get_node("Foot%d" % k)
		var planted := step % 2 == k
		foot.visible = planted
		if planted:
			var sx := -7.0 + step * 0.7 + 0.35
			var st := (sx + 7.0) / 1.4
			var sz := 1.3 + 0.5 * sin(st * 0.9)
			var side := 0.12 if k == 0 else -0.12
			var hd := atan2(-0.45 * cos(st * 0.9), 1.4)
			var p := Vector3(sx, 0, sz) + Vector3(sin(hd), 0, cos(hd)) * side
			if foot.get_meta("step", -1) != step:
				foot.set_meta("step", step)
				foot.rotation = Vector3(0, hd + PI * 0.5, 0)
				foot.global_position = Vector3(p.x, _undisturbed(p) + 0.03, p.z)
	# Balls and the wheel roll through at a set depth below the undisturbed snow (plough, no Bekker).
	for k in 2:
		var b: Node3D = get_node("Ball%d" % k)
		var r := 0.22 + 0.1 * k
		var p := Vector3(-6.0 + (1.1 + 0.3 * k) * walk_t, 0, -0.5 - 1.8 * k)
		b.global_position = Vector3(p.x, _undisturbed(p) - 0.12 - 0.06 * k + r, p.z)
		b.rotation.z = -p.x / r
	var w: Node3D = get_node("Wheel")
	var wp := Vector3(-8.0 + 1.7 * walk_t, 0, -4.0)
	w.global_position = Vector3(wp.x, _undisturbed(wp) - 0.2 + 0.4, wp.z)
	w.rotation.z = -wp.x / 0.4


func _save_frame(suffix: String) -> void:
	for i in 12:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		var path := out.replace(".png", suffix + ".png")
		get_viewport().get_texture().get_image().save_png(path)
		print("saved ", path)


func _ground_material(color: Color, tile: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.roughness = 0.9
	var nt := NoiseTexture2D.new()
	nt.seamless = true
	nt.as_normal_map = true
	nt.bump_strength = 6.0
	nt.width = 512
	nt.height = 512
	var nz := FastNoiseLite.new()
	nz.frequency = 0.02
	nz.fractal_octaves = 5
	nt.noise = nz
	m.normal_enabled = true
	m.normal_texture = nt
	m.uv1_scale = Vector3(tile, tile, tile)
	m.snow_enabled = true
	m.wetness_enabled = true
	return m


func _add_terrain(extent: float, cells: int, color: Color, tile: float) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step := extent / cells
	var h0 := -extent * 0.5
	for z in cells + 1:
		for x in cells + 1:
			var p := Vector3(h0 + x * step, 0, h0 + z * step)
			p.y = ground.get_height(p)
			st.set_uv(Vector2(float(x) / cells, float(z) / cells))
			st.add_vertex(p)
	for z in cells:
		for x in cells:
			var i := z * (cells + 1) + x
			st.add_index(i)
			st.add_index(i + 1)
			st.add_index(i + cells + 1)
			st.add_index(i + 1)
			st.add_index(i + cells + 2)
			st.add_index(i + cells + 1)
	st.generate_normals()
	st.generate_tangents()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	mi.material_override = _ground_material(color, tile)
	add_child(mi)


func _add_props() -> void:
	var stone := _ground_material(Color(0.5, 0.48, 0.45), 1.0)
	var wood := _ground_material(Color(0.42, 0.28, 0.16), 1.0)
	var metal := StandardMaterial3D.new()
	metal.albedo_color = Color(0.7, 0.1, 0.08)
	metal.metallic = 0.6
	metal.roughness = 0.35
	metal.snow_enabled = true
	_prop(BoxMesh.new(), Vector3(1.2, 0.8, 1.2), Vector3(-2.6, 0.4, -0.5), Vector3.ZERO, wood)
	_prop(BoxMesh.new(), Vector3(1.6, 1.2, 0.9), Vector3(2.4, 0.6, -1.0), Vector3.ZERO, metal)
	var sm := SphereMesh.new()
	_prop(sm, Vector3(1.4, 1.4, 1.4), Vector3(0.0, 0.7, -0.6), Vector3.ZERO, stone)
	_prop(BoxMesh.new(), Vector3(2.0, 0.08, 1.0), Vector3(-0.6, 0.75, 1.6), Vector3(0, 0, 45), wood)
	_prop(BoxMesh.new(), Vector3(2.4, 0.1, 1.6), Vector3(2.6, 0.8, 1.4), Vector3(0, 0, 70), stone)


func _prop(mesh: PrimitiveMesh, size: Vector3, pos: Vector3, rot: Vector3, mat: Material) -> void:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.scale = size
	mi.position = pos
	mi.rotation_degrees = rot
	mi.material_override = mat
	add_child(mi)
	# Collision so the snowpack finds the tops (SnowCover3D casts down where no height_source answers).
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = BoxShape3D.new() if mesh is BoxMesh else SphereShape3D.new()
	if mesh is BoxMesh:
		(cs.shape as BoxShape3D).size = Vector3.ONE
	else:
		(cs.shape as SphereShape3D).radius = 0.5
	body.add_child(cs)
	mi.add_child(body)

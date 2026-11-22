extends Node3D

# A fire on the ground east of x = 2 m: 928 W/m2, enough to melt 10 mm of water an hour.
class FireStub extends WorldFire:
	func _heat_at(p: Vector3) -> float:
		return 928.0 if p.x > 2.0 else 0.0
# Records the impulse waves handed to the water (WorldWater.wave), for the avalanche-into-water checks.
class WaveStub extends WorldWater:
	var waves: Array = []
	func _wave(position: Vector3, direction: Vector3, height: float, period: float, width: float) -> void:
		waves.append([position, direction, height, period, width])
# Weather for the body checks: one value everywhere, an hour of weather per second.
class BodyWeatherStub extends WorldWeather:
	var t := -5.0
	var wind := Vector3()
	var snowfall := 0.0
	var rain := 0.0
	func _air_temperature_at(_p: Vector3) -> float:
		return t
	func _air_humidity_at(_p: Vector3) -> float:
		return 0.9
	func _wind_at(_p: Vector3, _h: float) -> Vector3:
		return wind
	func _snowfall_at(_p: Vector3) -> float:
		return snowfall
	func _rainfall_at(_p: Vector3) -> float:
		return rain
	func _precipitation_at(_p: Vector3) -> float:
		return snowfall + rain
	func _time_scale() -> float:
		return 3600.0
# Ground snow for the body checks: 20 cm of 150 kg/m3 everywhere.
class BodySnowStub extends WorldSnow:
	func _depth_at(_p: Vector3) -> float:
		return 0.2
	func _density_at(_p: Vector3) -> float:
		return 150.0
	func _wetness_at(_p: Vector3) -> float:
		return 0.0
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
#   --view=snowfall   falling snow over deep snow and a shed (flakes stop under the roof); --wind=M m/s;
#                     --air=C for wet snow and sleet near and above 0 C
#                     at 10 m (default 4; 15 or more blows the snow off the ground)
#   --view=climate   a winter baked over the mountain (SnowClimate): snow line, shaded north faces, loaded lee
#   --view=cornice   a corniced ridge from below its lee face
#   --view=avalanche  a metre of new snow on a 38 degree face releases and piles at the toe (--before: the
#                     loaded face before it goes); --test=avalanche for the checks
#                     --powder [--at=S]: the run in real time, shot S s after release (default 9) with its
#                     powder cloud (PhysX Flow; traced, it also saves the raster frame of the same cloud as
#                     <out>_raster.png; --no-gas: without the cloud, quick); --crown: the crown wall from
#                     14 m down the bed (--crown-far: from 40 m); --wet: a thaw first, then a wet slide (--triggered: the
#                     same release, dry); --test=triggers, --test=powder, --test=forest, --test=wet for
#                     triggers, the cloud, forests (holding, braking, pressure on trees) and wet snow;
#                     --test=waves: avalanches into a lake (impulse waves, lake ice)
#   --view=bodies     three crates after a snowfall: one left out, one just driven in, one from a shed (SnowBodies3D)
#   --test=bodies     snow on moving things (follow-up 18): seeded, built by snowfall, sheltered, blown and shaken
#                     off, melted, burnt
#   --view=lake       a frozen lake from its shore after two weeks of frost and a light snow (--snow=H)
#   --view=sparkle    dry new snow at the feet against a low sun ahead: glinting grains; --melt=H hours of thaw
#                     first wets it (the glints go); --glints=exact (default) / grid: every grain a facet under the path tracer, or the raster's grid
#                     (sparkle) prints the share of glinting pixels and the mean luminance; --hoar=MM: surface hoar
#                     of MM mm over the field (its upright plates flash in the low sun)
#   --view=rime       a fence and a bare shrub after a day in freezing fog (8 m/s, -6 C, 0.25 g/m3): rime feathers into
#                     the wind [--close: the rail and the wire]; --test=rime checks the accretion
#   --view=shelter    a shed and young spruces after a snowfall: bare ground under the roof, snow in the
#                     crowns and less under them (--test=shelter for the checks)
#   --test=background updates on worker threads (follow-up 14): the same pack as updates run in the frame,
#                     the getters steady while one runs, the main thread's share of each update
#   --test=water      snow and water (follow-up 15): snow on a lake melts into it, lake ice grows by Stefan's
#                     law under the frost (against Ashton's observed coefficients), holds the snow, floods
#                     into snow ice under a heavy load and melts in a thaw
#   --test=crust      melt-freeze crusts: formed by a thaw and a frost, harder each cycle, a boot breaks a
#                     first-cycle crust and stands on a second-cycle one, new snow over it
#   --view=spindrift  the cornice ridge in a 20 m/s westerly after new snow: plumes off the crest (PhysX Flow),
#                     --at=S seconds of plume (default 12), --wind=M m/s; traced, it also saves _raster from the same run
#   --view=windblown  a ridge after new snow and a day of 15 m/s wind (--snow=H hours of wind, default 24;
#                     --transport=0 without the wind moving lying snow)
#   --test=transport  the wind moving lying snow (SnowTran-3D): the flux against Tabler (1991), a ridge
#                     scoured on its windward side and crest and loaded below it in the lee, mass kept, a crust
#                     and warm snow holding
#   --test=failure    cornice failure: the tension crack's opening rate against Gruvefjellet (Vogel et al.
#                     2012), the length a cold cornice falls at, a baked cornice holding a cold month and
#                     falling in a thaw with its debris at the foot of the face, none with failure off
#   --test=icicles    icicles: tip and wall rates (Makkonen 1988), the ratio of the two against Maeno et al.
#                     (1994), an eave in a day of thaw over frost, water kept, thickening once the wall
#                     freezes all of it, melting back; the shape: plumb in still air, leaning in wind, tips dripping
#   --view=cornice --crack=D  D days of thaw after the bake (the tension crack opening); --fall: on until the
#                     first cornice falls, a moment later (the blocks tumbling)
#   --view=icicles    a snowy roof's eave after sunny days at -4 C, backlit: clear ice with a hazy core [--close for 100%]
#                     [--wind=S: a steady wind along the eave by day, the tips lean; --dense: a week of melt with
#                     sites 5 cm apart, rippled icicles bridged into curtains] [--no-core: clear ice only]
#                     [--hero (implies --dense): 1.7 m out from the eave, a little above, a fused pair centred, root to
#                     tip (--under: from under the roof, raster only for now; --wall: head on against a boarded front); pass --resolution 1920x1080]
#                     [--black-core: a black core, to see where raster and the trace see it]
#   --mirror          (sparkle view) an overhead mirror showing the snow ahead: glints in reflections (--close: the mirror fills the frame)
#   --settled         (trail view) 0.4 m of settled snow (300 kg/m3) instead of fresh: prints break clumps
#   --no-clumps       (trail view) no snow clumps beside the prints
#   --no-plates       (trail view) a broken crust without its plates (the field's ramps alone)
#   --prints-close    (trail view) 1.5 m over two boot prints (with --crust=1: the broken crust's pieces)
#   --clumps-close    (trail view) the camera low beside the small ball's furrow
#   --away            (trail view) the field leaves for 300 m and comes back: are the clumps kept?
#   --sled            (trail view) a sled pulled at 4 m/s for 2 s and lifted away: its bow wave and berm
#   --deform=R,E[,S]  (trail view) deformation texels R over E m, triangles every S m
#   --crust=N         (trail view) a 2 cm crust of N melt-freeze cycles on the snow: 1 breaks into plates
#                     under the boots, balls and wheel; 2 holds the boots (--close: down on the boot prints)
#   --view=props      close: bumpy ground, boxes, a sphere, a 45 degree board and a 70 degree wall after snow
#   --snow=H          hours of 2 mm/h precipitation (default 24 mountain, 8 props)
#   --sun=H           then hours of sun (mountain; default 48)
#   --steps=N         save N frames as the snow builds (time-lapse) to --out with _NNN before .png
#   --probe           print the snowpack along the axes (mountain view)
#   --raster          raster only (default path traced)
#   --taa             raster with TAA (stills set against path traced ones: no per-pixel shadow or dither dots)
#   --out=path.png    save the last frame and quit
#   --suncups=M       trail view: suncups of M m relief over the field (the deep-snow surface draws them)
#   --sastrugi=M[,deg] trail view: sastrugi of M m relief carved by a wind toward deg (0 = +x, the default)
#   --slush=MM        trail view: 15 cm of old snow over frozen ground, then MM mm of rain at +8 C: slush in its base
#   --ice-drop=M      icicles view: the edge M m under the roof (the roots must still join it)
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)

const GroundScript := preload("res://ground.gd")

var view := "mountain"
var crack_days := 0
var fall_at := Vector3.ZERO
var fall := false
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
var crust_cycles := 0.0
var transport := true
var hoar_mm := 0.0 # --hoar=MM: the sparkle view's surface hoar
var suncups_m := 0.0 # --suncups=M: the sparkle view's suncup relief
var sastrugi_arg := "" # --sastrugi=M[,deg]
var slush_mm := 0.0 # --slush=MM: rain on a thin pack over frozen ground (trail view)
var air := INF # --air=C: the snowfall view's air temperature (sleet near and above 0 C)


func _ready() -> void:
	# 4x MSAA for raster stills: thin geometry far off (a crown wall a pixel high) is otherwise a row of dots.
	get_viewport().msaa_3d = Viewport.MSAA_4X
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		if a == "--glints=exact" or a == "--glints=grid":
			# Every grain a facet under the path tracer (follow-up 5, the default), or stage 5's footprint grid.
			RenderingServer.global_shader_parameter_set("snow_exact_glints", a == "--glints=exact")
		elif a == "--raster": raster = true
		elif a == "--taa":
			# Raster stills set against path traced ones: TAA resolves the per-pixel shadow and dither dots
			# (the bench holds still a few frames before saving, so it converges).
			get_viewport().use_taa = true
		elif a.begins_with("--test"):
			test = true
			only = a.substr(7)
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--snow="): snow_hours = float(a.substr(7))
		elif a.begins_with("--hoar="): hoar_mm = float(a.substr(7))
		elif a.begins_with("--suncups="): suncups_m = float(a.substr(10))
		elif a.begins_with("--sastrugi="): sastrugi_arg = a.substr(11)
		elif a.begins_with("--slush="): slush_mm = float(a.substr(8))
		elif a.begins_with("--sun="): sun_hours = float(a.substr(6))
		elif a.begins_with("--steps="): steps = int(a.substr(8))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--melt="): melt_hours = float(a.substr(7))
		elif a.begins_with("--crust="): crust_cycles = float(a.substr(8))
		elif a.begins_with("--transport="): transport = a.substr(12) != "0"
		elif a.begins_with("--crack="): crack_days = int(a.substr(8))
		elif a == "--fall": fall = true
		elif a.begins_with("--air="): air = float(a.substr(6))
	if test:
		if only == "climate":
			await _run_climate_tests()
		elif only == "shelter":
			await _run_shelter_tests()
		elif only == "crust":
			await _run_crust_tests()
			await _run_rotten_tests()
			await _run_slush_tests()
		elif only == "hoar":
			await _run_hoar_tests()
		elif only == "suncups":
			_run_suncup_tests()
		elif only == "spindrift":
			await _run_spindrift_tests()
		elif only == "road":
			await _run_road_tests()
		elif only == "windslab":
			await _run_wind_slab_tests()
		elif only == "sastrugi":
			await _run_sastrugi_tests()
		elif only == "rime":
			await _run_rime_tests()
		elif only == "transport":
			await _run_transport_tests()
		elif only == "failure":
			await _run_failure_tests()
		elif only == "icicles":
			_run_icicle_tests()
			await _run_icicle_attach_tests()
		elif only == "background":
			await _run_background_tests()
		elif only == "water":
			await _run_water_tests()
		elif only == "fire":
			await _run_fire_tests()
		elif only == "sleet":
			_run_sleet_tests()
		elif only == "avalanche":
			await _run_avalanche_tests()
		elif only == "triggers":
			await _run_trigger_tests()
		elif only == "powder":
			await _run_powder_tests()
		elif only == "forest":
			await _run_forest_tests()
		elif only == "wet":
			await _run_wet_tests()
		elif only == "waves":
			await _run_impulse_wave_tests()
		elif only == "bodies":
			await _run_body_tests()
			await _run_body_slab_tests()

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
	weather.cloud_cover = 0.0
	weather.snap_to_settings()
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
	# With no world tier the square is all the pack there is: outside it there is no snow (the materials read the
	# same), not the edge's snow stretched outward.
	var outside := Vector3(200.0, 0, 0)
	_check("no snow known outside the square without a world tier", not snow.has_world() and snow.get_depth_at(outside) == 0.0 and snow.get_depth_at(Vector3(120.0, 0, 0)) > 0.0, "%.3f m at 200 m, %.3f m at 120 m" % [snow.get_depth_at(outside), snow.get_depth_at(Vector3(120.0, 0, 0))])
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
	snow.wind_transport_enabled = false # the falling snow's drift alone (--test=transport moves lying snow)
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

	await _run_transport_tests()
	await _run_surface_tests()
	await _run_crust_tests()
	await _run_rotten_tests()
	await _run_slush_tests()
	await _run_spindrift_tests()
	await _run_road_tests()
	await _run_wind_slab_tests()
	await _run_sastrugi_tests()
	await _run_hoar_tests()
	_run_suncup_tests()
	await _run_rime_tests()
	await _run_fall_tests()
	await _run_shelter_tests()
	await _run_climate_tests()
	await _run_failure_tests()
	_run_icicle_tests()
	await _run_icicle_attach_tests()
	await _run_background_tests()
	await _run_water_tests()
	await _run_fire_tests()
	_run_sleet_tests()
	await _run_body_tests()
	await _run_body_slab_tests()
	await _run_avalanche_tests()
	await _run_trigger_tests()
	await _run_powder_tests()
	await _run_forest_tests()
	await _run_wet_tests()
	await _run_impulse_wave_tests()

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


func _weather_to(t: float, hours: float) -> void:
	weather.temperature = t
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(hours * 3600.0)
	await get_tree().physics_frame


func _walk(z: float, from_x: float = -3.0, to_x: float = 3.0) -> void:
	var wk := CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.15
	(ws.shape as CapsuleShape3D).height = 1.8
	wk.add_child(ws)
	wk.add_to_group("snow_deform")
	# A boot dragged along the line (the crust's answer to a load), not a walk: pressed as its body shape.
	wk.set_meta("world_contact_walker", false)
	add_child(wk)
	var steps_n := int((to_x - from_x) / 0.1)
	for i in steps_n + 1:
		var x := from_x + i * 0.1
		surface.sync_deformation() # the walker is set on the surface it left: the same on the GPU as on the CPU
		var top := surface.get_surface_height_at(Vector3(x, 0, z))
		wk.global_position = Vector3(x, top + 0.9, z)
		await get_tree().physics_frame
	wk.queue_free()
	await get_tree().physics_frame


func _trail_stats(z: float) -> Dictionary:
	surface.sync_deformation()
	# Mean dent along the trail's middle, and the rim beside it: mean and coefficient of variation.
	# Beside it (0.25 m from the middle, past the boot and its rim ring): how far the surface is down.
	var dsum := 0.0
	var edges: Array[float] = []
	for i in 40:
		var x := -2.0 + i * 0.1
		dsum += surface.get_dent_at(Vector3(x, 0, z))
		for side in [-1.0, 1.0]:
			var q := Vector3(x, 0, z + side * 0.25)
			edges.append(surface.get_dent_at(q) - surface.get_rim_at(q))
	var esum := 0.0
	for e in edges:
		esum += e
	var emean := esum / edges.size()
	var var_sum := 0.0
	for e in edges:
		var_sum += (e - emean) * (e - emean)
	return {"dent": dsum / 40.0, "edge": emean, "cv": sqrt(var_sum / edges.size()) / maxf(absf(emean), 1e-6)}


func _ridge_band(x0: float, x1: float) -> float:
	var sum := 0.0
	var k := 0
	var x := x0
	while x <= x1:
		for z in [-40.0, 0.0, 40.0]:
			sum += snow.get_swe_at(Vector3(x, 0, z))
			k += 1
		x += 4.0
	return sum / k


func _run_transport_tests() -> void:
	print("Wind moving lying snow:")
	# Tabler (1991): transport in the lowest 5 m, Q = U10^3.8 / 233847 kg/(m s), from Wyoming measurements.
	var ok := true
	var row := ""
	for u in [10.0, 15.0, 20.0]:
		var q := SnowCover3D.transport_flux(u, -10.0, 100.0)
		var tab := pow(u, 3.8) / 233847.0
		row += " %.0f m/s %.3f (Tabler %.3f)" % [u, q, tab]
		if u >= 15.0:
			ok = ok and q > tab / 3.0 and q < tab * 3.0
	_check("flux against Tabler (1991), within a factor of 3 in a gale (closer to the threshold it falls below his fit, which has none)", ok, row)
	var ut := SnowCover3D.transport_threshold(-10.0, 100.0)
	_check("no transport at the threshold, none in warm or packed snow", SnowCover3D.transport_flux(ut - 0.01, -10.0, 100.0) == 0.0 and SnowCover3D.transport_flux(15.0, 0.5, 100.0) == 0.0 and SnowCover3D.transport_flux(9.0, -10.0, 450.0) == 0.0, "threshold %.1f m/s at -10 C (Li and Pomeroy), %.1f m/s at 450 kg/m3 (Liston et al. 2007)" % [ut, SnowCover3D.transport_threshold(-10.0, 450.0)])
	# A 100 m ridge across a 15 m/s wind from the west for six hours, 40 mm of new snow on it.
	await _setup("ridge", 512.0, 128, -8.0)
	snow.drift_enabled = false
	snow.add_snow(40.0, 100.0)
	weather.wind_speed = 15.0
	weather.wind_direction = Vector2(1, 0)
	weather.snap_to_settings()
	weather.advance(1.0)
	var total0 := snow.get_total_water()
	var wind0 := _ridge_band(-24.0, -4.0)
	var lee0 := _ridge_band(0.0, 30.0)
	var flank0 := _ridge_band(-120.0, -40.0)
	snow.advance(6.0 * 3600.0)
	var wind1 := _ridge_band(-24.0, -4.0)
	var lee1 := _ridge_band(0.0, 30.0)
	var flank1 := _ridge_band(-120.0, -40.0)
	var prof := "  profile (x: mm):"
	for x in [-240, -160, -120, -80, -40, -12, 0, 12, 24, 40, 80, 160, 240]:
		prof += " %d: %.0f" % [x, snow.get_swe_at(Vector3(x, 0, 0))]
	print(prof)
	print("  largest flux %.3f kg/(m s), %.1f mm per cell moved in the last step; one update of 128 x 128 cells with transport %.2f ms" % [snow.get_blown_flux_max(), snow.get_blown_mean(), snow.get_update_time_msec()])
	_check("the windward shoulder is scoured, the lee below the crest loaded, the even slope between passes it on", wind1 < wind0 * 0.5 and lee1 > lee0 * 1.5 and absf(flank1 - flank0) < 0.05 * flank0, "shoulder %.1f to %.1f mm, lee %.1f to %.1f mm, mid-flank %.1f to %.1f mm" % [wind0, wind1, lee0, lee1, flank0, flank1])
	var lee_rho := snow.get_density_at(Vector3(12.0, 0, 0))
	_check("wind-laid snow is wind-packed", lee_rho > 150.0, "%.0f kg/m3 in the lee (laid at 300, the new snow was 100)" % lee_rho)
	var total1 := snow.get_total_water()
	_check("transport keeps the water", absf(total1 - total0) / total0 < 0.01, "%.3f%% change" % (100.0 * (total1 - total0) / total0))
	# The same wind over a frozen crust, and over snow at +1 C: nothing moves.
	await _setup("ridge", 512.0, 128, -8.0)
	snow.drift_enabled = false
	snow.add_snow(40.0, 100.0)
	snow.add_crust(0.02, 2.0)
	weather.wind_speed = 15.0
	weather.wind_direction = Vector2(1, 0)
	weather.snap_to_settings()
	weather.advance(1.0)
	var c0 := _ridge_band(-120.0, -10.0)
	snow.advance(6.0 * 3600.0)
	var c1 := _ridge_band(-120.0, -10.0)
	_check("a frozen crust holds the snow", absf(c1 - c0) < 0.1, "%.2f to %.2f mm on the windward side" % [c0, c1])


func _run_rotten_tests() -> void:
	print("Rotten snow (follow-up 41):")
	_check("a wet pack keeps a share of its bearing (Yamanoi and Endo 2002)", SnowSurface3D.rotten_share(0.0) == 1.0 and absf(SnowSurface3D.rotten_share(1.0) - 0.15) < 1e-4, "%.2f dry, %.2f soaked" % [SnowSurface3D.rotten_share(0.0), SnowSurface3D.rotten_share(1.0)])
	# 0.4 m of old spring snow at 400 kg/m3: firm in the cold, rotten in a warm afternoon.
	await _surface_setup(160.0, 400.0)
	await _walk(-1.0)
	var dry := _trail_stats(-1.0)
	await _weather_to(8.0, 10.0)
	var wet := snow.get_wetness_at(Vector3(0.0, 0, 1.0))
	await _walk(1.0)
	var rotten := _trail_stats(1.0)
	var q := Vector3(0.0, 0, 1.0)
	_check("boots posthole in a wet spring pack", wet > 0.5 and rotten.dent > 2.0 * maxf(dry.dent, 0.01), "%.3f m in the cold, %.3f m when wet (wetness %.2f; %.2f m at %.0f kg/m3)" % [dry.dent, rotten.dent, wet, snow.get_depth_at(q), snow.get_density_at(q)])


func _run_slush_tests() -> void:
	print("Slush over frozen ground (follow-up 41):")
	var seal_frozen := SnowCover3D.ground_seal(Vector3(0.0, 0.3, 0.0))
	var seal_thawed := SnowCover3D.ground_seal(Vector3(0.1, 0.3, 0.0))
	_check("concrete frost seals the base, a thawed top or unfrozen ground does not", seal_frozen > 0.99 and seal_thawed == 0.0 and SnowCover3D.ground_seal(Vector3()) == 0.0, "%.2f frozen, %.2f under 10 cm thawed" % [seal_frozen, seal_thawed])
	var flat := SnowCover3D.slush_drain_rate(500.0, 0.0, 50.0)
	var steep := SnowCover3D.slush_drain_rate(500.0, sin(deg_to_rad(20.0)), 50.0)
	_check("slush ponds for days on the flat and drains in hours off a slope (Darcy, K about 8 mm/s)", 1.0 / flat > 72.0 and 1.0 / steep < 12.0, "e-folds in %.0f h flat, %.1f h at 20 degrees (50 m slope)" % [1.0 / flat, 1.0 / steep])
	# 0.4 m of spring snow, two warm days: over unfrozen ground the melt soaks away; over frozen ground it ponds.
	var q := Vector3(0.0, 0, 1.0)
	await _surface_setup(160.0, 400.0)
	await _weather_to(10.0, 48.0)
	var open_slush := snow.get_slush_at(q)
	var open_depth := snow.get_depth_at(q)
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _frost_at(_p: Vector3) -> Vector3:\n\treturn Vector3(0.0, 0.3, 0.0)\n"
	stub.reload()
	WorldContracts.set_ground(stub.new())
	await _surface_setup(160.0, 400.0)
	await _weather_to(10.0, 48.0)
	var slush := snow.get_slush_at(q)
	var swe := snow.get_swe_at(q)
	var wet := snow.get_wetness_at(q)
	var depth_frozen := snow.get_depth_at(q)
	# Then a hard frost: the slush freezes into the pack's base (denser, not deeper).
	await _weather_to(-8.0, 24.0)
	var frozen_slush := snow.get_slush_at(q)
	var swe_frozen := snow.get_swe_at(q)
	var depth_after := snow.get_depth_at(q)
	WorldContracts.clear_overrides()
	_check("melt water ponds as slush over frozen ground, drains away over thawed", open_slush == 0.0 and slush > 0.02 and wet == 1.0, "%.3f m of slush, none over thawed ground" % slush)
	_check("the slush is pore water: inside the pack, no deeper than the drained pack", slush <= depth_frozen + 1e-4 and absf(depth_frozen - open_depth) < 0.02, "%.3f of %.3f m (%.3f m over thawed ground)" % [slush, depth_frozen, open_depth])
	_check("a hard frost freezes it into the base: denser, not deeper", frozen_slush < 0.5 * slush and swe_frozen > swe + 5.0 and depth_after <= depth_frozen + 1e-3, "%.3f m left, %.0f to %.0f mm SWE, %.3f to %.3f m" % [frozen_slush, swe, swe_frozen, depth_frozen, depth_after])


func _spindrift_at(wind: float, temp: float) -> Array:
	weather.temperature = temp
	weather.wind_speed = wind
	weather.wind_direction = Vector2(1, 0)
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(600.0)
	await get_tree().physics_frame
	return [snow.get_spindrift_rate(), snow.get_spindrift_sources()]


func _run_spindrift_tests() -> void:
	print("Spindrift off ridges (follow-up 42):")
	# The cornice ridge (crest along z at x = 0, a 50 degree lee face to the east), 0.4 m of new snow, westerlies.
	await _setup("cornice", 256.0, 64, -10.0)
	snow.add_snow(40.0, 100.0)
	var calm: Array = await _spindrift_at(4.0, -10.0)
	var gale: Array = await _spindrift_at(18.0, -10.0)
	var storm: Array = await _spindrift_at(26.0, -10.0)
	var srcs: Array = gale[1]
	var on_crest := srcs.size() > 0
	for s: Dictionary in srcs:
		var p: Vector3 = s["position"]
		var v: Vector3 = s["velocity"]
		if absf(p.x) > 8.0 or v.x <= 0.0:
			on_crest = false
	_check("no plume under the threshold wind (Li and Pomeroy 1997)", calm[0] == 0.0 and (calm[1] as Array).is_empty(), "%.2f kg/s at 4 m/s (6.4 m/s at most over the crest)" % calm[0])
	_check("a gale streams snow off the lee crest, downwind", gale[0] > 0.0 and on_crest, "%.1f kg/s from %d sources at 18 m/s, all on the crest blowing east" % [gale[0], srcs.size()])
	_check("stronger wind, far more snow (flux about u^4)", storm[0] > 2.5 * gale[0], "%.1f kg/s at 26 m/s against %.1f at 18" % [storm[0], gale[0]])
	var cloud: SnowPowderCloud3D = snow.get_spindrift()
	_check("the plume is a Flow gas fed from the crest", cloud != null and (not ClassDB.class_exists("PhysXFlow3D") or (cloud.has_flow() and cloud.get_active_sources() == (storm[1] as Array).size())), "%s, %d emitters" % ["flow" if cloud and cloud.has_flow() else "no flow", cloud.get_active_sources() if cloud else 0])
	_check("drier air thins it sooner (sublimation)", SnowCover3D.spindrift_fade(0.4) > 2.0 * SnowCover3D.spindrift_fade(0.95), "%.3f /s at 40%%, %.3f at 95%%" % [SnowCover3D.spindrift_fade(0.4), SnowCover3D.spindrift_fade(0.95)])
	var wet: Array = await _spindrift_at(26.0, 2.0)
	_check("no plume from wet snow", wet[0] == 0.0, "%.2f kg/s at +2 C" % wet[0])


# Wind slab (follow-up 45): snow falling in wind lands denser (Crocus, Vionnet et al. 2012), and where the surface
# drifts (Crocus's driftability SI > 0) its top packs toward 350 kg/m3 as wind slab (Sommer et al. 2018).
func _wind_case(wind: float, hours: int, fall: float) -> Array:
	await _setup("flat", 128.0, 32, -8.0)
	snow.add_snow(30.0, 100.0)
	weather.temperature = -8.0
	weather.wind_speed = wind
	weather.wind_direction = Vector2(1, 0)
	weather.rain_rate = fall
	weather.cloud_cover = 1.0 if fall > 0.0 else 0.0
	weather.snap_to_settings()
	weather.advance(1.0)
	var p := Vector3(0, 0, 0)
	var swe0 := snow.get_swe_at(p)
	var d0 := snow.get_depth_at(p)
	for h in hours:
		weather.advance(60.0)
		snow.advance(3600.0)
	var added := snow.get_swe_at(p) - swe0
	var added_depth := snow.get_depth_at(p) - d0
	return [snow.get_density_at(p), snow.get_wind_slab_at(p), added, added_depth]


func _run_wind_slab_tests() -> void:
	print("Wind slab (follow-up 45):")
	var calm_rho := SnowCover3D.wind_snow_density(-8.0, 0.0)
	var wind_rho := SnowCover3D.wind_snow_density(-8.0, 12.0)
	_check("snow falling in wind lands denser (Crocus)", absf(calm_rho - SnowCover3D.new_snow_density(-8.0)) < 1.0 and absf(wind_rho - (109.0 - 48.0 + 26.0 * sqrt(12.0))) < 1.0, "%.0f kg/m3 calm, %.0f at 12 m/s" % [calm_rho, wind_rho])
	_check("fresh light snow drifts in a breeze, old dense snow only in a gale (SI)", SnowCover3D.drift_index(5.0, 100.0, 1.0) > 0.0 and SnowCover3D.drift_index(2.0, 100.0, 1.0) < 0.0 and SnowCover3D.drift_index(8.0, 300.0, 0.0) < 0.0 and SnowCover3D.drift_index(15.0, 300.0, 0.0) > 0.0, "SI %.2f at 5 m/s and %.2f at 2 (fresh, 100 kg/m3); %.2f at 8 and %.2f at 15 (old, 300)" % [SnowCover3D.drift_index(5.0, 100.0, 1.0), SnowCover3D.drift_index(2.0, 100.0, 1.0), SnowCover3D.drift_index(8.0, 300.0, 0.0), SnowCover3D.drift_index(15.0, 300.0, 0.0)])
	var p24 := SnowCover3D.wind_packing_step(100.0, 10.0, 1.0, 24.0)
	var p96 := SnowCover3D.wind_packing_step(100.0, 10.0, 1.0, 96.0)
	_check("wind packing toward 350 kg/m3 over days (48 h / SI)", p24 > 140.0 and p24 < p96 and p96 < 350.0 and SnowCover3D.wind_packing_step(100.0, 2.0, 1.0, 24.0) == 100.0, "%.0f kg/m3 after a day at 10 m/s, %.0f after four; none at 2 m/s" % [p24, p96])
	var calm: Array = await _wind_case(2.0, 12, 0.0)
	var gale: Array = await _wind_case(14.0, 12, 0.0)
	print("  12 h on 30 cm of new snow: calm %.0f kg/m3, slab %.3f m; 14 m/s %.0f kg/m3, slab %.3f m" % [calm[0], calm[1], gale[0], gale[1]])
	_check("a gale packs the surface into wind slab; calm does not", calm[1] == 0.0 and gale[1] > 0.05 and gale[0] > calm[0] + 10.0, "slab %.3f m and %.0f kg/m3 at 14 m/s, %.3f m and %.0f kg/m3 calm" % [gale[1], gale[0], calm[1], calm[0]])
	var fall_calm: Array = await _wind_case(1.0, 3, 2.0)
	var fall_wind: Array = await _wind_case(12.0, 3, 2.0)
	var rc: float = fall_calm[2] / maxf(fall_calm[3], 1e-6)
	var rw: float = fall_wind[2] / maxf(fall_wind[3], 1e-6)
	_check("a windy snowfall lays down a denser, thinner layer, and it counts as slab", rw > rc * 1.3 and fall_wind[1] > fall_calm[1], "new snow %.0f kg/m3 in 12 m/s, %.0f calm; slab %.3f m and %.3f m" % [rw, rc, fall_wind[1], fall_calm[1]])
	await _setup("flat", 128.0, 32, -8.0)
	snow.wind_packing_enabled = false
	snow.add_snow(30.0, 100.0)
	weather.wind_speed = 14.0
	weather.snap_to_settings()
	weather.advance(1.0)
	for h in 12:
		snow.advance(3600.0)
	_check("off: no slab", snow.get_wind_slab_at(Vector3.ZERO) == 0.0, "%.3f m" % snow.get_wind_slab_at(Vector3.ZERO))
	weather.rain_rate = 0.0
	weather.cloud_cover = 0.0


# Sastrugi (follow-up 46): wind eroding hard (wind-packed or dense) snow carves ridges along it (Filhol and Sturm
# 2015); new snow, drift and melt fill them.
func _run_sastrugi_tests() -> void:
	print("Sastrugi (follow-up 46):")
	var v := SnowCover3D.sastrugi_step(Vector2.ZERO, Vector2(1, 0), 0.05, 0.0, 1.0, true)
	var soft := SnowCover3D.sastrugi_step(Vector2.ZERO, Vector2(1, 0), 0.05, 0.0, 1.0, false)
	var filled := SnowCover3D.sastrugi_step(Vector2(0.1, 0), Vector2.ZERO, 0.0, 0.04, 1.0, false)
	var cap := SnowCover3D.sastrugi_step(Vector2(0.3, 0), Vector2(1, 0), 0.3, 0.0, 0.2, true)
	var turn := SnowCover3D.sastrugi_step(Vector2(0.1, 0), Vector2(0, 1), 0.1, 0.0, 1.0, true)
	_check("hard snow carved by what the wind takes, soft snow not", absf(v.x - 0.05) < 1e-4 and soft == Vector2.ZERO, "%s from 5 cm off hard snow, %s off soft" % [str(v), str(soft)])
	_check("new snow fills them; at most half the pack and 0.4 m; they turn toward a new wind", absf(filled.x - 0.06) < 1e-4 and absf(cap.length() - 0.1) < 1e-4 and turn.y > 0.0 and turn.x > 0.0, "%.3f m after 4 cm fill; %.2f m on a 0.2 m pack; %s after a south wind" % [filled.x, cap.length(), str(turn)])
	# The cornice ridge's windward slope in a westerly gale on 0.4 m of new snow: the wind packs it into slab and
	# erodes the slab; without wind packing the snow stays soft and is only blown away.
	var res := []
	for packing: bool in [true, false]:
		await _setup("cornice", 256.0, 64, -12.0)
		snow.wind_packing_enabled = packing
		snow.add_snow(60.0, 150.0)
		weather.temperature = -12.0
		weather.wind_speed = 24.0
		weather.wind_direction = Vector2(1, 0)
		weather.snap_to_settings()
		weather.advance(1.0)
		for h in 6:
			snow.advance(3600.0)
		var best := Vector2.ZERO
		var lee := 0.0
		var drift_swe := 0.0
		for z in range(-60, 61, 8):
			for x in range(-60, -8, 4):
				var sv := snow.get_sastrugi_at(Vector3(x, 0, z))
				if sv.length() > best.length():
					best = sv
			# The lee drift: the deepest snow below the crest.
			for x in range(0, 40, 4):
				var dq := Vector3(x, 0, z)
				if snow.get_swe_at(dq) > drift_swe:
					drift_swe = snow.get_swe_at(dq)
					lee = snow.get_sastrugi_at(dq).length()
		res.append([best, lee])
		print("  packing %s: windward sastrugi %.3f m toward %s, lee drift (%.0f mm) %.3f m" % [str(packing), best.length(), str(best.normalized()), drift_swe, lee])
	var hard_best: Vector2 = res[0][0]
	var soft_best: Vector2 = res[1][0]
	_check("a gale packs and carves the windward snow into sastrugi along the wind", hard_best.length() > 0.02 and hard_best.normalized().x > 0.95, "%.3f m toward %s" % [hard_best.length(), str(hard_best.normalized())])
	_check("none in the fresh lee deposit, none in soft unpacked snow", float(res[0][1]) < 0.005 and soft_best.length() < 0.005, "lee %.3f m; soft %.3f m" % [res[0][1], soft_best.length()])


func _road_wheel(spin: float, roll: float) -> RigidBody3D:
	# A 0.66 m car tyre, 0.2 m tread, 300 kg on it (3 kN), held in place resting on the snow at x = 0, z = 1.
	var w := RigidBody3D.new()
	w.mass = 300.0
	w.freeze = true
	w.freeze_mode = RigidBody3D.FREEZE_MODE_KINEMATIC
	var cs := CollisionShape3D.new()
	var cy := CylinderShape3D.new()
	cy.radius = 0.33
	cy.height = 0.2
	cs.shape = cy
	cs.rotation_degrees = Vector3(90, 0, 0) # its axle along z: it rolls along x
	w.add_child(cs)
	add_child(w)
	w.add_to_group("world_contact")
	w.global_position = Vector3(0.3, snow.get_ground_height_at(Vector3(0.3, 0, 1)) + snow.get_depth_at(Vector3(0.3, 0, 1)) + 0.3, 1.0)
	w.linear_velocity = Vector3(roll, 0, 0)
	w.angular_velocity = Vector3(0, 0, -(roll + spin) / 0.33)
	return w


func _run_road_tests() -> void:
	print("Packed-snow roads (follow-up 43):")
	var g0 := SnowCover3D.ice_grip(-0.5, 0.0)
	var g20 := SnowCover3D.ice_grip(-20.0, 0.0)
	var gw := SnowCover3D.ice_grip(-5.0, 1.0)
	_check("ice grips least near melting and when wet, better in the cold (Wallman and Astrom 2001)", g0 < 0.07 and g20 > 0.13 and g20 < 0.16 and gw < 0.06, "%.2f at -0.5 C, %.2f at -20 C, %.2f wet" % [g0, g20, gw])
	await _surface_setup(60.0, 250.0)
	weather.temperature = -6.0
	weather.snap_to_settings()
	weather.advance(1.0)
	var q := Vector3(0.3, 0, 1.0)
	snow.advance(3600.0) # past the new-snow layer: an hour on, nothing fresh on top
	await get_tree().physics_frame
	var loose := snow.get_grip_at(q)
	# A wheel rolling without slipping: it packs the snow, it does not polish it.
	var w := _road_wheel(0.0, 5.0)
	for i in 60:
		await get_tree().physics_frame
	var packed := snow.get_packed_at(q)
	var rolled_glaze := snow.get_glaze_at(q)
	var packed_grip := snow.get_grip_at(q)
	w.queue_free()
	# Spinning in place (stuck at a junction): its friction melts the surface and the melt freezes as a glaze.
	w = _road_wheel(10.0, 0.0)
	for i in 600:
		await get_tree().physics_frame
	w.queue_free()
	var glaze := snow.get_glaze_at(q)
	var polished := snow.get_grip_at(q)
	_check("a rolling wheel packs the snow and leaves no glaze", packed > 0.5 and rolled_glaze < 0.005 and packed_grip < loose and packed_grip > 0.2, "packed %.2f, glaze %.3f mm; grip %.2f loose, %.2f packed" % [packed, rolled_glaze, loose, packed_grip])
	_check("a spinning wheel polishes a glaze: the grip falls toward ice", glaze > 0.05 and polished < packed_grip - 0.03, "%.2f mm of glaze after 10 s at 10 m/s slip; grip %.2f" % [glaze, polished])
	# Settled: the cells carry it through an update, then a warm spell melts the glaze and new snow covers it.
	snow.advance(60.0)
	await get_tree().physics_frame
	var kept := snow.get_glaze_at(q)
	await _weather_to(4.0, 3.0)
	var melted := snow.get_glaze_at(q)
	weather.temperature = -6.0
	weather.snap_to_settings()
	snow.add_snow(5.0, 100.0)
	_check("the glaze lasts through updates, melts in a thaw", kept > 0.8 * glaze and melted < 0.5 * kept, "%.2f mm after an update, %.2f after 3 h at +4 C" % [kept, melted])
	_check("new snow on a packed road: the tyre runs in loose snow again", absf(snow.get_grip_at(q) - 0.3) < 0.02, "grip %.2f" % snow.get_grip_at(q))


func _run_crust_tests() -> void:
	print("Crusts:")
	var s1 := SnowCover3D.crust_strength(1.0, 0.02)
	var s2 := SnowCover3D.crust_strength(2.0, 0.02)
	var s3 := SnowCover3D.crust_strength(3.0, 0.02)
	_check("hand hardness to bearing pressure (Fierz et al. 2009)", absf(s1 - 19.0) < 0.1 and absf(s3 - 120.0) < 0.5 and s2 > s1 and s2 < s3 and absf(SnowCover3D.crust_strength(1.0, 0.01) - 9.5) < 0.1, "1F %.0f kPa (one cycle), %.0f (two), P %.0f (three); half as thick bears half" % [s1, s2, s3])
	# 0.4 m of settled snow, a cold night, then thaws and frosts.
	await _surface_setup(80.0, 200.0)
	var p := Vector3(0.5, 0, 0.5)
	_check("new snow has no crust", snow.get_crust_cycles_at(p) == 0.0 and snow.get_crust_strength_at(p) == 0.0, "%.0f cycles" % snow.get_crust_cycles_at(p))
	await _weather_to(8.0, 10.0)
	var wet_strength := snow.get_crust_strength_at(p)
	var wetness := snow.get_wetness_at(p)
	await _weather_to(-8.0, 14.0)
	var c1 := snow.get_crust_cycles_at(p)
	var t1 := snow.get_crust_thickness_at(p)
	var st1 := snow.get_crust_strength_at(p)
	_check("a thaw and a frost make a melt-freeze crust", wet_strength == 0.0 and wetness > 0.5 and c1 == 1.0 and t1 > 0.005 and t1 <= 0.05, "wet %.2f in the thaw (bears %.0f kPa), then %.0f cycle, %.1f cm, %.1f kPa" % [wetness, wet_strength, c1, t1 * 100.0, st1])
	var boot := 75.0 * 9.81 / 1000.0 / 0.035
	surface.clear_deformation()
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	var v0 := surface.get_moved_volume()
	await _walk(3.0)
	var broken := _trail_stats(3.0)
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	var vol := surface.get_moved_volume() - v0
	_check("a boot (%.0f kPa) breaks a first-cycle crust" % boot, st1 < boot and broken.dent > 0.05, "%.3f m deep" % broken.dent)
	_check("the crust cracks around the hole and the pieces tip in", broken.edge > 0.01 and broken.cv > 0.3 and vol.y <= 0.3 * vol.x + 1e-4, "%.3f m down 25 cm from the middle, varying %.0f%% along it (jagged); rim %.4f of %.4f m3 pressed" % [broken.edge, broken.cv * 100.0, vol.y, vol.x])
	for i in 30:
		await get_tree().physics_frame
	var plates := surface.get_crust_plate_count()
	_check("a broken crust lies in plates (follow-up 38; the dragged boot makes one hole)", plates >= 1, "%d holes with plates" % plates)
	await _weather_to(8.0, 10.0)
	await _weather_to(-8.0, 14.0)
	var c2 := snow.get_crust_cycles_at(p)
	var st2 := snow.get_crust_strength_at(p)
	await _walk(-3.0)
	var held := _trail_stats(-3.0)
	_check("a second frost hardens it: the boot stands on it", c2 == 2.0 and st2 > boot and held.dent < 0.01, "%.0f cycles, %.1f cm, %.0f kPa; %.3f m deep" % [c2, snow.get_crust_thickness_at(p) * 100.0, st2, held.dent])
	# 10 cm of new snow over it: the boot compacts that and stops on the crust.
	snow.add_snow(10.0, 100.0)
	await get_tree().physics_frame
	var over := snow.get_crust_depth_at(p)
	await _walk(-6.0)
	var buried := _trail_stats(-6.0)
	var rho := snow.get_density_at(p)
	var cap := over * (1.0 - rho / 450.0) / (1.0 - 0.3 * rho / 450.0)
	_check("new snow over a crust: the boot compacts it and stops", absf(over - 0.1) < 0.02 and buried.dent > 0.02 and buried.dent <= cap + 0.01, "%.2f m over the crust, %.3f m deep (to the crust %.3f)" % [over, buried.dent, cap])
	# The same pack without crusts: the boot goes in.
	snow.crust_enabled = false
	snow.advance(60.0)
	await get_tree().physics_frame
	await _walk(-9.0)
	var none := _trail_stats(-9.0)
	print("  beside an unbroken trail (crusts off): %.3f m" % none.edge)
	_check("crust_enabled off: the boot sinks as before", snow.get_crust_strength_at(p) == 0.0 and none.dent > buried.dent + 0.02, "%.3f m deep" % none.dent)


func _run_rime_tests() -> void:
	print("Rime (round 5):")
	var e_twig := SnowRime3D.collision_efficiency(8.0, 20.0, 0.005)
	var e_rail := SnowRime3D.collision_efficiency(8.0, 20.0, 0.05)
	var e_post := SnowRime3D.collision_efficiency(8.0, 20.0, 0.3)
	_check("thin collectors catch more of the fog than thick ones (Finstad et al. 1988)", e_twig > 0.5 and e_rail < e_twig and e_post < e_rail, "5 mm %.2f, 5 cm %.2f, 30 cm %.2f at 8 m/s" % [e_twig, e_rail, e_post])
	var soft := SnowRime3D.rime_density(20.0, 3.0, -15.0)
	var hard := SnowRime3D.rime_density(20.0, 15.0, -2.0)
	_check("rime is soft in a light cold wind, hard in a strong mild one (Macklin 1962)", soft < 250.0 and hard > 500.0, "%.0f and %.0f kg/m3" % [soft, hard])
	var rate := SnowRime3D.accretion_rate(8.0, 0.25, -6.0, 20.0, 0.01)
	_check("a 1 cm twig in dense fog gains millimetres an hour", rate > 0.001 and rate < 0.02, "%.1f mm/h" % (rate * 1000.0))
	_check("none above freezing or without fog", SnowRime3D.accretion_rate(8.0, 0.25, 1.0, 20.0, 0.01) == 0.0 and SnowRime3D.accretion_rate(8.0, 0.0, -6.0, 20.0, 0.01) == 0.0, "")
	var holder := Node3D.new()
	add_child(holder)
	var pole := MeshInstance3D.new()
	var cm := CylinderMesh.new()
	cm.top_radius = 0.01
	cm.bottom_radius = 0.01
	cm.height = 1.0
	cm.rings = 8
	pole.mesh = cm
	pole.position = Vector3(40.0, 1.0, 40.0)
	holder.add_child(pole)
	var rime := SnowRime3D.new()
	rime.target_group = ""
	add_child(rime)
	rime.target_paths = [rime.get_path_to(holder)]
	rime.refresh_targets()
	for h in 12:
		rime.grow(1.0, -6.0, Vector3(8.0, 0.0, 0.0), 0.25)
	var th := rime.get_target_thickness(0)
	var g := rime.get_growth_direction(0)
	var upwind := rime.get_thickness_at(Vector3(39.99, 1.0, 40.0))
	var downwind := rime.get_thickness_at(Vector3(40.01, 1.0, 40.0))
	_check("rime builds on the windward side, growing into the wind", th > 0.01 and g.x < -0.9 and upwind > 0.3 * th and downwind < 0.05 * th, "%.1f cm in 12 h (windward middle %.1f cm, lee %.1f mm), toward (%.2f, %.2f, %.2f)" % [th * 100.0, upwind * 100.0, downwind * 1000.0, g.x, g.y, g.z])
	_check("drawn as a crust with feathers", rime.get_feather_count() > 20, "%d feathers" % rime.get_feather_count())
	for h in 24:
		rime.grow(1.0, 4.0, Vector3(2.0, 0.0, 0.0), 0.0)
	_check("a thaw takes it off", rime.get_target_thickness(0) < 0.001, "%.1f mm left" % (rime.get_target_thickness(0) * 1000.0))
	# A twig at 45 degrees: its collector is its own width, not its world box's.
	pole.rotation_degrees = Vector3(45.0, 30.0, 0.0)
	rime.refresh_targets()
	_check("a slanted twig's collector is its width", rime.get_target_diameter(0) < 0.025, "%.1f mm for a 20 mm pole at 45 degrees" % (rime.get_target_diameter(0) * 1000.0))
	rime.queue_free()
	holder.queue_free()
	await get_tree().process_frame


func _run_suncup_tests() -> void:
	print("Suncups (follow-up 41):")
	# Five sunny spring days: 25 mm of melt a day from the direct sun, 5 from the air, in 1 m of old snow.
	var a := 0.0
	for day in 5:
		for h in 6:
			a = SnowCover3D.suncup_step(a, 25.0 / 6.0, 5.0 / 6.0, 0.0, 0.0, 0.0, 1.0)
	_check("five sunny days grow suncups (full grown in about 5 days; Mitchell and Tiedje 2010)", a > 0.15 and a <= 0.3, "%.2f m ridge to hollow" % a)
	var b := a
	for day in 3:
		b = SnowCover3D.suncup_step(b, 2.0, 30.0, 0.0, 0.0, 0.0, 1.0)
	_check("warm cloudy days melt them flat (air heats the surface evenly)", b < 0.5 * a, "%.2f to %.2f m in 3 days of 30 mm air melt" % [a, b])
	var filled := SnowCover3D.suncup_step(a, 0.0, 0.0, 0.0, 0.12, 0.0, 1.0)
	_check("new snow fills the hollows", absf(filled - maxf(a - 0.12, 0.0)) < 0.005, "%.2f m under 12 cm of new snow" % filled)
	var dirty := 0.0
	var thick := 0.0
	for day in 5:
		for h in 6:
			dirty = SnowCover3D.suncup_step(dirty, 25.0 / 6.0, 5.0 / 6.0, 0.0, 0.0, 5.0e4, 1.0)
			thick = SnowCover3D.suncup_step(thick, 25.0 / 6.0, 5.0 / 6.0, 0.0, 0.0, 5.0e6, 1.0)
	_check("dust on the ridges deepens them, a thick layer stops them (Rhodes et al. 1987)", dirty > a and thick < 0.01, "%.2f m dusty, %.3f m under a thick layer" % [dirty, thick])
	var shallow := 0.0
	for day in 5:
		for h in 6:
			shallow = SnowCover3D.suncup_step(shallow, 25.0 / 6.0, 0.0, 0.0, 0.0, 0.0, 0.2)
	_check("no deeper than half the pack", shallow <= 0.1 + 1e-4, "%.2f m in 20 cm of snow" % shallow)


func _run_hoar_tests() -> void:
	print("Surface hoar (round 5):")
	var r := [0.0]
	var e_clear := SnowCover3D.hoar_deposition(-10.0, 0.9, 1.5, 0.0, 0.0)
	var e_cloud := SnowCover3D.hoar_deposition(-10.0, 0.9, 1.5, 1.0, 0.0)
	var e_dry := SnowCover3D.hoar_deposition(-10.0, 0.3, 1.5, 0.0, 0.0)
	var e_sun := SnowCover3D.hoar_deposition(-10.0, 0.9, 1.5, 0.0, 250.0)
	_check("a clear humid night deposits frost; cloud, dry air or sun stop it (bulk transfer)", e_clear > 0.003 and e_clear < 0.02 and e_cloud < 0.3 * e_clear and e_dry < 0.0 and e_sun < e_clear * 0.3, "clear %.4f, overcast %.4f, 30%% humidity %.4f, sunny %.4f kg/m2/h" % [e_clear, e_cloud, e_dry, e_sun])
	var s5 := SnowCover3D.buried_hoar_strength(5.0, 0.0)
	var s15 := SnowCover3D.buried_hoar_strength(15.0, 0.0)
	var s5_old := SnowCover3D.buried_hoar_strength(5.0, 24.0 * 21.0)
	_check("buried hoar is weak, weaker with larger crystals, gains slowly (Jamieson and Schweizer 2000)", s5 > 300.0 and s5 < 800.0 and s15 < s5 and s5_old > s5 * 1.5 and s5_old < 3.0 * s5, "5 mm %.0f Pa, 15 mm %.0f Pa, 5 mm after three weeks %.0f Pa" % [s5, s15, s5_old])
	await _surface_setup(80.0, 200.0)
	var p := Vector3(0.5, 0, 0.5)
	weather.cloud_cover = 0.0
	weather.humidity = 0.9
	weather.wind_speed = 1.5
	await _weather_to(-10.0, 12.0)
	var night := snow.get_hoar_at(p)
	_check("a clear calm night at -10 C grows surface hoar", night > 2.0 and night < 15.0, "%.1f mm" % night)
	await _weather_to(-10.0, 24.0)
	var nights := snow.get_hoar_at(p)
	_check("more clear calm nights grow it on", nights > night * 1.5, "%.1f mm after three nights" % nights)
	weather.wind_speed = 6.0
	await _weather_to(-10.0, 2.0)
	var blown := snow.get_hoar_at(p)
	_check("a 6 m/s wind breaks it", blown < 0.1 * nights, "%.2f mm" % blown)
	weather.wind_speed = 1.5
	weather.cloud_cover = 1.0
	await _weather_to(-10.0, 12.0)
	var overcast := snow.get_hoar_at(p)
	weather.cloud_cover = 0.0
	await _weather_to(-10.0, 12.0)
	var regrown := snow.get_hoar_at(p)
	_check("none under overcast, it grows again under a clear sky", overcast < 1.0 and regrown > 2.0, "%.2f mm overcast, %.1f mm clear" % [overcast, regrown])
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	var layer := snow.get_buried_hoar_at(p)
	var depth := snow.get_buried_hoar_depth_at(p)
	_check("new snow buries it as a weak layer", absf(layer - regrown) < 0.01 and absf(depth - 0.3) < 0.03 and snow.get_hoar_at(p) == 0.0, "%.1f mm crystals under %.2f m" % [layer, depth])
	var slope := deg_to_rad(38.0)
	var tau := 100.0 * 9.81 * depth * sin(slope) * cos(slope)
	var stab := SnowCover3D.buried_hoar_strength(layer, 0.0) / tau
	var stab_new := SnowAvalanche3D.stability_index(depth, 100.0, 100.0, 48.0, slope)
	# A skier (Foehn 1987's line load, 1 kN/m) on it.
	var ski := tau + SnowAvalanche3D.added_shear(1000.0, depth * cos(slope), slope)
	var sk := SnowCover3D.buried_hoar_strength(layer, 0.0) / ski
	var sk_new := SnowAvalanche3D.weak_layer_strength(100.0, 48.0) / ski
	_check("on a 38 degree slope it is weaker than two-day-old new snow, and a skier fails it", stab < stab_new and sk < 1.0 and sk_new > sk * 1.5, "S %.2f over buried hoar (%.2f in the new snow), with a skier %.2f (%.2f)" % [stab, stab_new, sk, sk_new])
	await _weather_to(-10.0, 48.0)
	_check("it stays buried and ages", snow.get_buried_hoar_at(p) > 0.0 and snow.get_buried_hoar_age_at(p) > 1.9, "%.1f mm, %.1f days" % [snow.get_buried_hoar_at(p), snow.get_buried_hoar_age_at(p)])
	await _weather_to(4.0, 1.0)
	_check("a thaw wets the surface crystals away", snow.get_hoar_at(p) == 0.0, "%.2f mm" % snow.get_hoar_at(p))
	snow.hoar_enabled = false
	await _weather_to(-10.0, 12.0)
	_check("hoar_enabled off: none grows", snow.get_hoar_at(p) == 0.0, "%.2f mm" % snow.get_hoar_at(p))
	snow.hoar_enabled = true


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
	surface.gpu_simulation = not OS.get_cmdline_user_args().has("--gpu=0")
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

	# A bow wave: the same flat foot moving at 5 m/s piles its rim ahead (mass kept).
	var bw := Vector3(-6.0, 0, 6.0)
	var vol0 := surface.get_moved_volume()
	surface.press(bw, 0.3, -1.0, 0.0, false, Vector3(5.0, 0, 0))
	var vol1 := surface.get_moved_volume()
	var ahead := surface.get_rim_at(bw + Vector3(0.36, 0, 0))
	var behind := surface.get_rim_at(bw - Vector3(0.36, 0, 0))
	var dvol := vol1 - vol0
	_check("a fast body piles its rim ahead (bow wave), mass kept", ahead > 2.0 * behind and absf(dvol.y - 0.3 * dvol.x) < 0.01 * dvol.x, "%.3f m ahead, %.3f m behind at 6 cm; rim %.1f%% of the dent" % [ahead, behind, 100.0 * dvol.y / dvol.x])
	# Driven on along x in 5 cm steps, the bow rides in front.
	var sp := bw
	for k in 30:
		sp = bw + Vector3(0.05 * (k + 1), 0, 0)
		surface.press(sp, 0.3, -1.0, 0.0, false, Vector3(5.0, 0, 0))
	var front := 0.0
	for k in 13:
		front = maxf(front, surface.get_rim_at(sp + Vector3(0.3 + 0.05 * k, 0, 0)))
	var flank := surface.get_rim_at(sp + Vector3(-0.6, 0, 0.4))
	_check("the bow rides ahead and spills to the sides", front > flank and front > ahead, "highest %.3f m in front after 1.5 m (%.3f m after one press), %.3f m beside the track behind" % [front, ahead, flank])

	# A boot print turned 90 degrees: long along x, narrow along z.
	var bp := Vector3(6.0, 0, -6.0)
	surface.press_box(bp, Vector2(0.1, 0.28), PI * 0.5, -1.0, 0.0)
	var along := surface.get_dent_at(bp + Vector3(0.08, 0, 0))
	var across := surface.get_dent_at(bp + Vector3(0, 0, 0.12))
	_check("boot prints follow the heading", along > 0.3 and across < 0.05, "%.3f m along the boot, %.3f m beside it" % [along, across])

	# Collision follows the deformed surface.
	await get_tree().create_timer(0.2).timeout
	surface.sync_deformation() # on the GPU the heights come back a few rendered frames late
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
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
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
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	# Its feet print (WorldWalkerSteps), 9 cm either side of its line: the deepest print over 2 m.
	var trail := 0.0
	var trail_at := Vector3(0.0, 0, 3.0)
	for k in 201:
		for zf in [2.91, 3.09]:
			var fq := Vector3(-1.0 + 0.01 * k, 0, zf)
			if surface.get_dent_at(fq) > trail:
				trail = surface.get_dent_at(fq)
				trail_at = fq
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
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	var back := surface.get_dent_at(trail_at)
	_check("trails survive leaving and coming back", cached > 0 and absf(back - trail) < 0.002, "%d tiles cached, %.3f m after, %.3f m before" % [cached, back, trail])

	# New snow settles into the prints; the pack settling shrinks the step; melt takes it all.
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	var before := surface.get_dent_at(p)
	snow.add_snow(10.0, 100.0) # 0.1 m of new snow
	await get_tree().physics_frame
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	var after := surface.get_dent_at(p)
	_check("new snow fills prints (refill_share 0.5)", absf((before - after) - 0.05) < 0.005, "%.3f m to %.3f m" % [before, after])
	snow.advance(14.0 * 24.0 * 3600.0)
	await get_tree().physics_frame
	var rho2 := snow.get_density_at(p)
	var d2 := snow.get_depth_at(p)
	var cap := d2 * (1.0 - rho2 / 450.0) / (1.0 - 0.3 * rho2 / 450.0)
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
	_check("settled pack: the print is no deeper than the compaction allows", surface.get_dent_at(p) <= cap + 0.002, "%.3f m, limit %.3f m (pack %.0f kg/m3, %.2f m)" % [surface.get_dent_at(p), cap, rho2, d2])
	weather.temperature = 10.0
	weather.snap_to_settings()
	weather.advance(1.0)
	sun.visible = true
	sun.rotation_degrees = Vector3(-60, 0, 0)
	snow.advance(30.0 * 24.0 * 3600.0)
	await get_tree().physics_frame
	surface.sync_deformation() # the GPU field is read back a frame or two late; wait for it
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

	# Snowstorms: the suspended snow of Pomeroy and Male (1992) as extinction. The NWS blizzard is visibility
	# under 400 m in winds of 15.6 m/s or more; just past the threshold the snow drifts below eye level.
	print("Blowing snow visibility at 1 m (Koschmieder, 3.912 / sigma):")
	var vis := {}
	for u: float in [10.0, 15.0, 20.0, 25.0]:
		var c := SnowFall3D.blowing_concentration(u, 1.0)
		vis[u] = 3.912 / maxf(SnowFall3D.blowing_extinction(c), 1e-9)
		print("  %2.0f m/s: %.6f kg/m3 -> %.0f m" % [u, c, vis[u]])
	_check("blowing snow thickens with the wind to a blizzard", vis[10.0] > vis[15.0] and vis[15.0] > vis[20.0] and vis[20.0] < 400.0 and vis[10.0] > 1000.0, "%.0f m at 10 m/s (drifting below eye level), %.0f m at 20 m/s" % [vis[10.0], vis[20.0]])
	var c02 := SnowFall3D.blowing_concentration(20.0, 0.2)
	var c2 := SnowFall3D.blowing_concentration(20.0, 2.0)
	_check("the drifting layer thins upward", c02 > 3.0 * c2, "%.5f kg/m3 at 0.2 m against %.5f at 2 m" % [c02, c2])
	# The haze reaches the weather map: snowfall everywhere, blowing snow in a layer over the snow.
	await _setup("meadow", 64.0, 64, -10.0)
	weather.rain_rate = 2.0
	weather.cloud_cover = 1.0
	weather.wind_speed = 18.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(6.0 * 3600.0)
	var storm := SnowFall3D.new()
	storm.snow_cover = NodePath("../SnowCover3D")
	add_child(storm)
	for i in 4:
		await get_tree().process_frame
	var g := snow.get_ground_height_at(Vector3.ZERO) + snow.get_depth_at(Vector3.ZERO)
	var at1 := weather.get_precipitation_extinction_at(Vector3(0, g + 1.0, 0))
	var at20 := weather.get_precipitation_extinction_at(Vector3(0, g + 20.0, 0))
	_check("a storm hazes the air through the weather map", at1 > storm.get_extinction() + 0.5 * storm.get_blowing_extinction() and storm.get_blowing_extinction() > 0.0 and absf(at20 - storm.get_extinction()) < 0.2 * at1, "%.3f /m at 1 m (snowfall %.3f, blowing %.3f), %.3f /m at 20 m" % [at1, storm.get_extinction(), storm.get_blowing_extinction(), at20])
	storm.queue_free()
	await get_tree().process_frame


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
	# A crown jolted from outside the weather (WorldSnow.shake_canopy; trees, docs/ideas/trees.md 5al): half the held
	# snow within 3 m drops onto the pack below at the next update, the water kept.
	var moved: float = Engine.get_singleton("WorldContracts").get_snow().shake_canopy(Vector3(0, 0, 0), 3.0, 0.5)
	snow.advance(1.0)
	var held_s := snow.get_canopy_load_at(Vector3(0, 0, 0))
	var under_s := snow.get_swe_at(Vector3(0, 0, 0))
	_check("a shaken crown drops its snow onto the pack (shake_canopy)", moved > 0.0 and absf(held_s - held * 0.5) < held * 0.1 and absf(under_s - under - (held - held_s)) < held * 0.1 and absf(snow.get_total_water() - w1) < w1 * 0.01, "%.0f kg moved; %.1f mm left of %.1f, %.1f mm below (was %.1f)" % [moved, held_s, held, under_s, under])
	held = held_s
	under = under_s
	weather.temperature = -1.0
	weather.wind_speed = 6.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(24.0 * 3600.0)
	var held2 := snow.get_canopy_load_at(Vector3(0, 0, 0))
	var under2 := snow.get_swe_at(Vector3(0, 0, 0))
	_check("it unloads onto the snow below", held2 < held * 0.5 and under2 > under, "%.1f mm left in the canopy after a day at -1 C in 6 m/s wind; %.1f mm below (was %.1f)" % [held2, under2, under])


# Follow-up 14: the cover's updates on worker threads, swapped in when done. A snowy, windy, sunny mountain with a
# world tier is run through the same physics frames twice, once updating in the frame and once in the
# background: the packs must come out the same, and the getters must not change while an update runs.
func _background_run(background: bool, updates: int) -> Dictionary:
	await _setup("mountain", 512.0, 256, 0.5)
	weather.rain_rate = 2.0
	weather.cloud_cover = 0.6
	weather.wind_speed = 12.0
	weather.snap_to_settings()
	weather.advance(1.0)
	sun.visible = true
	sun.rotation_degrees = Vector3(-35.0, 150.0, 0.0)
	snow.add_snow(150.0, 200.0)
	snow.climate = _climate(9.0)
	snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 64)
	snow.world_update_every = 3
	# Warm up in the frame: Sx for this wind baked on both tiers, so no bake lands mid-run.
	snow.advance(60.0)
	snow.advance_world(3600.0)
	snow.background_updates = background
	# The fresh load can avalanche on these slopes; the flow's timing would differ between the two runs.
	snow.avalanches_enabled = false
	snow.update_rate = 2.0
	var start: int = snow.get_update_count()
	var main_ms: Array[float] = []
	var work_ms := 0.0
	var steady := true
	var seen: int = start
	var water := snow.get_total_water()
	var probe := Vector3(37.0, 0, -61.0)
	var probe_swe := snow.get_swe_at(probe)
	var busy_frames := 0
	while snow.get_update_count() < start + updates:
		await get_tree().physics_frame
		if snow.get_update_count() != seen:
			seen = snow.get_update_count()
			main_ms.append(snow.get_main_thread_msec())
			work_ms += snow.get_update_time_msec()
			water = snow.get_total_water()
			probe_swe = snow.get_swe_at(probe)
		if snow.is_updating():
			busy_frames += 1
			if snow.get_total_water() != water or snow.get_swe_at(probe) != probe_swe:
				steady = false
	snow.update_rate = 0.0001
	snow.sync_update()
	var r := {}
	var grid := PackedFloat32Array()
	for z in range(-240, 241, 24):
		for x in range(-240, 241, 24):
			var q := Vector3(x, 0, z)
			grid.append(snow.get_swe_at(q))
			grid.append(snow.get_density_at(q))
			grid.append(snow.get_crust_thickness_at(q))
	for z in range(-1900, 1901, 200):
		for x in range(-1900, 1901, 200):
			grid.append(snow.get_world_swe_at(Vector3(x, 0, z)))
	r.grid = grid
	r.water = snow.get_total_water()
	r.main_ms = main_ms
	r.work_ms = work_ms / maxf(main_ms.size(), 1)
	r.steady = steady
	r.busy_frames = busy_frames
	r.failures = snow.get_cornice_failure_count()
	return r


func _run_background_tests() -> void:
	print("Follow-up 14: cover updates in the background")
	var n := 12
	var a: Dictionary = await _background_run(false, n)
	var b: Dictionary = await _background_run(true, n)
	var ga: PackedFloat32Array = a.grid
	var gb: PackedFloat32Array = b.grid
	var worst := 0.0
	for i in ga.size():
		worst = maxf(worst, absf(ga[i] - gb[i]))
	_check("the same pack as updating in the frame", worst == 0.0 and a.water == b.water, "%d samples over both tiers, largest difference %.6f; water %.1f against %.1f kg" % [ga.size(), worst, a.water, b.water])
	_check("getters steady while an update runs", b.steady and b.busy_frames > 0, "%d physics frames with an update in flight" % b.busy_frames)
	var ma: Array[float] = a.main_ms
	var mb: Array[float] = b.main_ms
	var fmt := func(m: Array[float]) -> String:
		var s := 0.0
		var top := 0.0
		for v in m:
			s += v
			top = maxf(top, v)
		return "mean %.2f ms, worst %.2f ms" % [s / maxf(m.size(), 1), top]
	print("  main thread per update, 256 x 256 cells and a 64 x 64 world tier every 3rd update: in the frame %s; in the background %s (cells, drift, slides and cornices %.2f ms on a worker)" % [fmt.call(ma), fmt.call(mb), b.work_ms])
	# Medians, not the worst update: on a loaded machine (a render alongside) the scheduler can stall any one
	# update in either mode, but the typical frame cost only drops when the work leaves the main thread.
	var med := func(m: Array[float]) -> float:
		var c := m.duplicate()
		c.sort()
		return c[c.size() / 2] if c.size() > 0 else 0.0
	var med_a: float = med.call(ma)
	var med_b: float = med.call(mb)
	_check("the frame no longer waits for the update", med_b < 0.5 * med_a, "median %.2f ms against %.2f ms" % [med_b, med_a])


# A lake around the mountain's foot: a water surface the terrain data map takes as a water source (any node
# with get_water_level(), as PhysXWaterSurface3D has).
func _lake(level: float) -> void:
	var ws := GDScript.new()
	ws.source_code = "extends Node3D\nvar level := 0.0\nfunc get_water_level() -> float:\n\treturn level\n"
	ws.reload()
	var lake := Node3D.new()
	lake.set_script(ws)
	lake.set("level", level)
	lake.name = "Lake"
	add_child(lake)
	var dm := TerrainDataMap3D.new()
	dm.name = "WaterMap"
	dm.size = 1024.0
	dm.resolution = 256
	add_child(dm)
	dm.height_source = dm.get_path_to(ground)
	dm.water_sources = [dm.get_path_to(lake)]
	dm.bake()
	snow.climate_map = snow.get_path_to(dm)
	snow.refresh_heights()


func _run_water_tests() -> void:
	print("Follow-up 15: snow and water")
	# Stefan's law against the field: Ashton (1989) gives h = a sqrt(FDD) (cm, degree-days) with a about 2.7
	# for a windy lake without snow and 1.4 to 2.4 for lakes with snow; Stefan's ideal is 3.6.
	var bare := SnowCover3D.lake_ice_growth(0.0, -10.0, 0.0, 300.0, 24.0 * 30.0)
	var snowy := SnowCover3D.lake_ice_growth(0.0, -10.0, 0.2, 300.0, 24.0 * 30.0)
	var a_bare := bare * 100.0 / sqrt(300.0)
	var a_snow := snowy * 100.0 / sqrt(300.0)
	_check("lake ice by Stefan's law, bare", a_bare > 2.4 and a_bare < 3.6, "%.1f cm after 30 days at -10 C, a = %.2f (windy lake 2.7, Stefan's ideal 3.6)" % [bare * 100.0, a_bare])
	# Snow conducts heat 18 times less than ice (Sturm et al. 1997: 0.13 W/(m K) at 300 kg/m3 against 2.24), so
	# 20 cm of it insulates like 3.5 m of ice: lakes reach Ashton's 1.4 to 2.4 with snow only because the snow
	# comes later and floods into snow ice.
	_check("snow insulates the ice", snowy < bare * 0.25 and absf(SnowCover3D.snow_conductivity(300.0) - 0.126) < 0.002, "%.1f cm under 20 cm of snow from the start (k = %.3f W/(m K))" % [snowy * 100.0, SnowCover3D.snow_conductivity(300.0)])
	var split := SnowCover3D.lake_ice_growth(SnowCover3D.lake_ice_growth(0.0, -10.0, 0.0, 300.0, 100.0), -10.0, 0.0, 300.0, 100.0)
	_check("ice growth adds up over steps", absf(split - SnowCover3D.lake_ice_growth(0.0, -10.0, 0.0, 300.0, 200.0)) < 1e-5, "%.4f m in two steps of 100 h" % split)

	await _setup("mountain", 512.0, 128, -5.0)
	var shore := Vector3(200.0, 0, 0)
	var level: float = ground.get_height(shore)
	_lake(level)
	var wet := Vector3(240.0, 0, 0)
	var dry := Vector3(120.0, 0, 0)
	_check("the lake is found", snow.get_water_level_at(wet) > ground.get_height(wet) and snow.get_water_level_at(dry) == -INF, "water at %.1f m over a bed at %.1f m; none at %.0f m from the summit" % [snow.get_water_level_at(wet), ground.get_height(wet), dry.x])
	snow.lake_ice_enabled = false
	_precipitate(10.0, 2.0)
	_check("snow on open water melts into it", snow.get_swe_at(wet) == 0.0 and snow.get_swe_at(dry) > 15.0 and snow.get_runoff_at(wet) > 0.0, "%.1f mm on the lake, %.1f mm on land; %.2f mm/h into the lake" % [snow.get_swe_at(wet), snow.get_swe_at(dry), snow.get_runoff_at(wet)])
	_check("the pack over the lake lies at its surface", absf(snow.get_ground_height_at(wet) - level) < 0.01, "%.2f m (water %.2f m)" % [snow.get_ground_height_at(wet), level])

	# Frost: ten days at -10 C on clear water grow ice as the relation says.
	await _setup("mountain", 512.0, 128, -10.0)
	_lake(level)
	snow.advance(240.0 * 3600.0)
	var air := snow.get_air_temperature_at(Vector3(wet.x, level, wet.z)) # the lapse rate at the lake's height
	var want := SnowCover3D.lake_ice_growth(0.0, air, 0.0, 300.0, 240.0)
	var got := snow.get_ice_thickness_at(wet)
	_check("ice grows in the frost", absf(got - want) < 0.005 and snow.get_ice_thickness_at(dry) == 0.0, "%.1f cm after 10 days at %.1f C (relation %.1f cm); none on land" % [got * 100.0, air, want * 100.0])
	_precipitate(10.0, 2.0)
	_check("the ice holds the snow", snow.get_swe_at(wet) > 15.0, "%.1f mm on %.1f cm of ice" % [snow.get_swe_at(wet), snow.get_ice_thickness_at(wet) * 100.0])
	# A heavy load on the ice floods it: past the buoyancy (1000 - 917) h it turns to snow ice.
	var ice0 := snow.get_ice_thickness_at(wet)
	snow.add_snow(150.0, 250.0)
	snow.advance(600.0)
	snow.advance(6.0 * 3600.0)
	var cap := 83.0 * snow.get_ice_thickness_at(wet)
	_check("a heavy load floods into snow ice", snow.get_swe_at(wet) <= cap + 1.0 and snow.get_ice_thickness_at(wet) > ice0 + 0.05, "ice %.1f -> %.1f cm, %.0f mm of snow left on it (the ice floats %.0f mm)" % [ice0 * 100.0, snow.get_ice_thickness_at(wet) * 100.0, snow.get_swe_at(wet), cap])
	_check("the flooded layer is white snow ice", snow.get_white_ice_thickness_at(wet) > 0.03 and snow.get_white_ice_thickness_at(wet) <= snow.get_ice_thickness_at(wet), "%.1f cm of the %.1f cm" % [snow.get_white_ice_thickness_at(wet) * 100.0, snow.get_ice_thickness_at(wet) * 100.0])
	# The drawn ice: a sheet over the lake at its level plus the freeboard, solid where it bears a walker.
	var li: SnowLakeIce3D = snow.get_lake_ice()
	li.collision_radius = 1000.0
	li.rebuild()
	var top := snow.get_water_level_at(wet) + SnowLakeIce3D.freeboard(snow.get_ice_thickness_at(wet))
	await get_tree().physics_frame
	await get_tree().physics_frame
	var hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(wet + Vector3(0, top + 20.0, 0), wet + Vector3(0, top - 20.0, 0)))
	_check("the ice is drawn and walkable", li != null and li.get_quad_count() > 100 and li.get_walkable_count() > 100 and not hit.is_empty() and absf(hit.position.y - top) < 0.05, "%d cells drawn, %d walkable; a ray stops at %.2f m (ice top %.2f m)" % [li.get_quad_count() if li else 0, li.get_walkable_count() if li else 0, hit.position.y if not hit.is_empty() else -INF, top])
	# Load on the ice (world links): a walker stands on it, a heavy vehicle goes through (Gold 1971, P = A h^2).
	var spot := wet + Vector3(0, 0, 24)
	var spot_ice := snow.get_ice_thickness_at(spot)
	var spot_top := snow.get_water_level_at(spot) + SnowLakeIce3D.freeboard(spot_ice)
	var walker := Node3D.new()
	walker.add_to_group("world_contact")
	walker.set_meta("ground_mass", 80.0)
	walker.set_meta("trample_radius", 0.3)
	add_child(walker)
	walker.global_position = Vector3(spot.x, spot_top + 0.3, spot.z)
	await get_tree().physics_frame
	await get_tree().physics_frame
	var walker_ok := snow.get_ice_break_count() == 0 and snow.get_ice_thickness_at(spot) == spot_ice
	var load_cap := SnowCover3D.ice_breaking_load(spot_ice, snow.get_white_ice_thickness_at(spot) / maxf(spot_ice, 1e-4), snow.get_air_temperature_at(spot))
	walker.set_meta("ground_mass", 1.5 * load_cap) # half again what this ice bears
	walker.set_meta("trample_radius", 1.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	var through := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(spot + Vector3(0, spot_top + 5.0, 0), spot + Vector3(0, spot_top - 5.0, 0)))
	_check("a walker stands on lake ice, a load past its bearing breaks through", spot_ice > 0.05 and walker_ok and snow.get_ice_break_count() >= 1 and snow.get_ice_thickness_at(spot) == 0.0 and through.is_empty() and snow.get_ice_thickness_at(wet) > 0.05, "%.1f cm of ice bears %.0f kg; 80 kg stood, half again broke through (%d breaks, %.1f cm left, ray %s)" % [spot_ice * 100.0, load_cap, snow.get_ice_break_count(), snow.get_ice_thickness_at(spot) * 100.0, "through" if through.is_empty() else "stopped"])
	walker.queue_free()
	snow.advance(3600.0)
	_check("the hole stays broken through the next update and freezes over thin", snow.get_ice_thickness_at(spot) < 0.3 * spot_ice, "%.1f cm an hour later at -10 C (was %.1f cm)" % [snow.get_ice_thickness_at(spot) * 100.0, spot_ice * 100.0])
	var white0 := snow.get_white_ice_thickness_at(wet)
	# A thaw: the snow goes first, then the ice.
	await _weather_to(12.0, 0.0)
	var ice1 := snow.get_ice_thickness_at(wet)
	var swe1 := snow.get_swe_at(wet)
	snow.advance(24.0 * 3600.0 * 10.0)
	_check("a thaw melts the snow, then the ice", snow.get_swe_at(wet) == 0.0 and snow.get_ice_thickness_at(wet) < ice1 - 0.02, "%.0f mm of snow and %.1f cm of ice -> %.1f mm and %.1f cm after 10 days at %.1f C" % [swe1, ice1 * 100.0, snow.get_swe_at(wet), snow.get_ice_thickness_at(wet) * 100.0, snow.get_air_temperature_at(Vector3(wet.x, level, wet.z))])
	var lost := ice1 - snow.get_ice_thickness_at(wet)
	_check("the thaw takes the white ice first", white0 - snow.get_white_ice_thickness_at(wet) > 0.99 * minf(lost, white0), "%.1f cm of ice lost, %.1f -> %.1f cm of white ice" % [lost * 100.0, white0 * 100.0, snow.get_white_ice_thickness_at(wet) * 100.0])
	snow.advance(24.0 * 3600.0 * 30.0)
	li.rebuild()
	_check("open water again, nothing drawn", snow.get_ice_thickness_at(wet) == 0.0 and li.get_quad_count() == 0, "%.1f cm, %d cells" % [snow.get_ice_thickness_at(wet) * 100.0, li.get_quad_count()])


func _run_sleet_tests() -> void:
	print("Sleet (follow-up 17)")
	var a_cold := SnowFall3D.aggregate_factor(-10.0)
	var a_warm := SnowFall3D.aggregate_factor(-0.5)
	_check("flakes aggregate near 0 C (Hobbs, Chang and Locatelli 1974: 2 to 3 times)", is_equal_approx(a_cold, 1.0) and a_warm > 2.0 and a_warm < 3.0, "x%.2f at -10 C, x%.2f at -0.5 C" % [a_cold, a_warm])
	var m0 := SnowFall3D.melted_share(-1.0, 0.85)
	var m1 := SnowFall3D.melted_share(1.0, 0.85)
	var m3 := SnowFall3D.melted_share(3.0, 0.85)
	_check("falling snow melts in air above 0 C, as the rain-or-snow split", m0 == 0.0 and m1 > 0.0 and m3 > m1 and absf(m1 - (1.0 - SnowCover3D.snowfall_fraction(1.0, 0.85))) < 1e-5, "%.2f at -1 C, %.2f at 1 C, %.2f at 3 C" % [m0, m1, m3])
	var v_dry := SnowFall3D.wet_fall_speed(2.0, 0.0)
	var v_half := SnowFall3D.wet_fall_speed(2.0, 0.5)
	var v_wet := SnowFall3D.wet_fall_speed(2.0, 1.0)
	_check("a melting flake keeps its slow fall until mostly melted, then falls as a drop (Mitra et al. 1990)", v_dry > 0.8 and v_dry < 1.5 and v_half < 1.5 * v_dry and absf(v_wet - 6.55) < 0.1, "%.2f m/s dry, %.2f half melted, %.2f melted (2 mm drop)" % [v_dry, v_half, v_wet])


func _body_box(at: Vector3) -> RigidBody3D:
	var b := RigidBody3D.new()
	b.freeze = true
	var cs := CollisionShape3D.new()
	var sh := BoxShape3D.new()
	sh.size = Vector3(1, 1, 1)
	cs.shape = sh
	b.add_child(cs)
	var mi := MeshInstance3D.new()
	mi.name = "Mesh"
	var bm := BoxMesh.new()
	bm.material = StandardMaterial3D.new()
	mi.mesh = bm
	b.add_child(mi)
	add_child(b)
	b.global_position = at
	return b


func _run_body_tests() -> void:
	print("Snow on moving things (follow-up 18)")
	for c in get_children():
		c.queue_free()
	await get_tree().process_frame
	var w := BodyWeatherStub.new()
	WorldContracts.set_weather(w)
	WorldContracts.set_snow(BodySnowStub.new())
	var sb := SnowBodies3D.new()
	sb.update_rate = 0.1 # the checks step it by hand
	add_child(sb)
	var a := _body_box(Vector3(0, 1, 0))
	var roofed := _body_box(Vector3(10, 1, 0))
	var roof := StaticBody3D.new()
	var rcs := CollisionShape3D.new()
	var rsh := BoxShape3D.new()
	rsh.size = Vector3(4, 0.2, 4)
	rcs.shape = rsh
	roof.add_child(rcs)
	add_child(roof)
	roof.global_position = Vector3(10, 4, 0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	_check("rigid bodies are found and tracked by themselves", sb.is_tracked(a) and sb.is_tracked(roofed) and not sb.is_tracked(roof), "%d tracked" % sb.get_tracked_count())
	sb.step(0.1)
	var mat := (a.get_node("Mesh") as MeshInstance3D).get_active_material(0) as StandardMaterial3D
	_check("a body in the open starts with the ground's snow; one under a roof with none", absf(sb.get_body_depth(a) - 0.2) < 1e-3 and sb.get_body_depth(roofed) == 0.0, "%.3f m open, %.3f m roofed" % [sb.get_body_depth(a), sb.get_body_depth(roofed)])
	var p: Variant = (a.get_node("Mesh") as MeshInstance3D).get_instance_shader_parameter("snow_body")
	_check("its materials show the load (snow_on_body, from the body only) and the load reaches them", mat.snow_on_body and mat.snow_enabled and not mat.snow_from_weather and p is Vector4 and absf(p.x - 0.2) < 2e-3, "%s" % [p])
	# Snowfall: 2 mm/h of water for an hour at -5 C.
	w.snowfall = 2.0
	sb.set_body_depth(a, 0.0)
	sb.set_body_depth(roofed, 0.0)
	sb.step(1.0)
	var expect := 2.0 / SnowCover3D.new_snow_density(-5.0)
	_check("falling snow builds the load at the new snow density (Hedstrom and Pomeroy 1998), not under a roof", absf(sb.get_body_depth(a) - expect) < 1e-3 and sb.get_body_depth(roofed) < 1e-4, "%.4f m in an hour (expected %.4f), %.4f m roofed" % [sb.get_body_depth(a), expect, sb.get_body_depth(roofed)])
	w.snowfall = 0.0
	# Wind: calm keeps it, a gale over the blowing threshold strips it.
	sb.set_body_depth(a, 0.1)
	w.wind = Vector3(3, 0, 0)
	for i in 10:
		sb.step(0.1)
	var calm := sb.get_body_depth(a)
	w.wind = Vector3(20, 0, 0)
	for i in 10:
		sb.step(0.1)
	var gale := sb.get_body_depth(a)
	w.wind = Vector3()
	_check("wind over the blowing threshold (Li and Pomeroy 1997) strips it; a breeze does not", absf(calm - 0.1) < 1e-3 and gale < 0.05, "%.3f m after 1 s at 3 m/s, %.3f m after 1 s more at 20 m/s (threshold %.1f m/s at -5 C)" % [calm, gale, SnowFall3D.blowing_threshold(-5.0)])
	# Driving into still air is the same wind.
	a.global_position.x += 2.5 # up to speed first (the start is a jolt of its own)
	sb.step(0.1)
	a.global_position.x += 2.5
	sb.step(0.1)
	sb.set_body_depth(a, 0.1)
	for i in 10:
		a.global_position.x += 2.5 # 25 m/s
		sb.step(0.1)
	var driven := sb.get_body_depth(a)
	_check("driving fast blows it off too (the air past the body is what counts)", driven < 0.06, "%.3f m after 1 s at 25 m/s" % driven)
	# Shaking: a jolt of several g throws it off; a steady crawl does not.
	a.global_position = Vector3(0, 1, 0)
	sb.step(0.1)
	sb.step(0.1)
	sb.set_body_depth(a, 0.1)
	for i in 10:
		a.global_position.z += 0.05
		sb.step(0.1)
	var crawl := sb.get_body_depth(a)
	for i in 10:
		a.global_position.y = 1.0 + (0.1 if i % 2 == 0 else 0.0) # 10 cm up and down every tenth of a second
		sb.step(0.1)
	var shaken := sb.get_body_depth(a)
	_check("jolts over half a g shake it off; a slow crawl keeps it", absf(crawl - 0.1) < 1e-3 and shaken < 0.03, "%.3f m after a crawl, %.3f m after 1 s of shaking at about 20 g" % [crawl, shaken])
	# Thaw: +5 C for two hours melts and wets it.
	sb.set_body_depth(a, 0.1)
	w.t = 5.0
	sb.step(1.0)
	sb.step(1.0)
	var thawed := sb.get_body_depth(a)
	var wet := sb.get_body_wetness(a)
	w.t = -5.0
	_check("a thaw melts it and wets what is left", thawed < 0.1 and thawed > 0.0 and wet > 0.0, "%.4f m of 0.1 after 2 h at 5 C, wetness %.2f" % [thawed, wet])
	# Fire: a body standing in the FireStub's heat.
	WorldContracts.set_fire(FireStub.new())
	sb.set_body_depth(a, 0.05)
	a.global_position = Vector3(5, 1, 0)
	sb.step(0.1)
	sb.set_body_depth(a, 0.05)
	sb.step(10.0)
	var burnt := sb.get_body_depth(a)
	_check("fire's heat melts it at 0.334 MJ/kg", burnt < 0.05, "%.4f m of 0.05 after 10 s in 928 W/m2" % burnt)
	sb.untrack(a)
	var cleared: Variant = (a.get_node("Mesh") as MeshInstance3D).get_instance_shader_parameter("snow_body")
	_check("an untracked body drops back to its material's own snow", cleared == Vector4(), "%s" % [cleared])
	# Weight (follow-up 19): 10 cm of 100 kg/m3 on a 1 m2 top is 10 kg pressing a free 10 kg body down at g.
	var free := _body_box(Vector3(20, 5, 0))
	free.freeze = false
	free.gravity_scale = 0.0
	free.mass = 10.0
	free.linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
	free.linear_damp = 0.0
	await get_tree().physics_frame
	sb.set_body_depth(free, 0.1, 100.0)
	sb.step(0.1)
	var load_mass := sb.get_body_load_mass(free)
	var t0 := Time.get_ticks_usec()
	for i in 30:
		await get_tree().physics_frame
	var vy := free.linear_velocity.y
	var t_s := 30.0 / Engine.physics_ticks_per_second
	var expect_v := -load_mass * 9.81 / free.mass * t_s
	_check("the load weighs on the body (mass = depth x density x top area)", absf(load_mass - 10.0) < 0.5 and absf(vy - expect_v) < 0.15 * absf(expect_v), "%.1f kg of snow on %.2f m2; %.2f m/s down after %.2f s (expected %.2f)" % [load_mass, sb.get_body_top_area(free), -vy, t_s, -expect_v])
	free.queue_free()
	# Slabs (Mohr-Coulomb on the bed): dry snow holds on a 40 degree roof, wet snow goes, deep dry snow goes when steep.
	var dry40 := SnowBodies3D.slab_slides(0.15, 150.0, 0.0, deg_to_rad(40.0), 250.0)
	var wet40 := SnowBodies3D.slab_slides(0.15, 150.0, 0.6, deg_to_rad(40.0), 250.0)
	var wet20 := SnowBodies3D.slab_slides(0.15, 150.0, 0.6, deg_to_rad(20.0), 250.0)
	var deep80 := SnowBodies3D.slab_slides(0.4, 150.0, 0.0, deg_to_rad(80.0), 250.0)
	_check("a slab slides by Mohr-Coulomb: wet snow off a 40 degree roof, not a 20 degree one; dry holds unless deep and steep", not dry40 and wet40 and not wet20 and deep80, "dry 40: %s, wet 40: %s, wet 20: %s, deep dry 80: %s" % [dry40, wet40, wet20, deep80])
	WorldContracts.clear_overrides()
	for c in get_children():
		c.queue_free()
	await get_tree().process_frame


func _run_body_slab_tests() -> void:
	# A crate tipped over in deep snow: its 30 cm of settled snow slides off its side as clumps on the snow.
	await _surface_setup(60.0, 250.0)
	var sb: SnowBodies3D = snow.get_bodies()
	sb.update_rate = 0.1
	var tipped := _body_box(Vector3(0, 1.0, 0))
	tipped.rotation_degrees = Vector3(0, 0, 75.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	sb.step(0.1)
	var clumps: SnowClumps3D = surface.get_clumps_node()
	var before := clumps.get_spawned_total() if clumps else 0
	sb.set_body_depth(tipped, 0.3, 280.0)
	sb.step(0.1)
	var made := (clumps.get_spawned_total() if clumps else 0) - before
	_check("a tipped body's load slides off as a slab and lands as clumps on the snow", sb.get_body_depth(tipped) == 0.0 and sb.get_slabs_slid() >= 1 and made > 0, "%.3f m left, %d slabs, %d clumps" % [sb.get_body_depth(tipped), sb.get_slabs_slid(), made])


func _run_avalanche_tests() -> void:
	print("Avalanches (follow-up 20)")
	# The stability index (Foehn 1987) with Jamieson and Johnston's (2001) strength: half a metre of new snow
	# holds on a 38 degree slope, a metre does not; two days of sintering holds the metre.
	var s50 := SnowAvalanche3D.stability_index(0.5, 100.0, 100.0, 0.0, deg_to_rad(38.0))
	var s100 := SnowAvalanche3D.stability_index(1.0, 100.0, 100.0, 0.0, deg_to_rad(38.0))
	var s100_old := SnowAvalanche3D.stability_index(1.0, 100.0, 100.0, 48.0, deg_to_rad(38.0))
	_check("the stability index: 50 cm of new snow holds on 38 degrees, a metre fails, two days later it holds", s50 > 1.0 and s100 < 1.0 and s100_old > 1.0, "S = %.2f, %.2f, %.2f" % [s50, s100, s100_old])
	await _setup("avalanche", 256.0, 128, -8.0)
	snow.drift_enabled = false
	snow.slide_enabled = false
	var av: SnowAvalanche3D = snow.get_avalanches()
	# 30 cm first: no release.
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	var calm := av.get_release_count()
	# A storm: another metre of light snow.
	snow.add_snow(90.0, 90.0)
	var water0 := snow.get_total_water()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var released := av.get_release_count() - calm
	var t := 0.0
	while av.is_running() and t < 200.0:
		av.step(0.25)
		t += 0.25
	var water1 := snow.get_total_water()
	var toe := snow.get_depth_at(Vector3(40.0, 0, 0))
	var face := snow.get_depth_at(Vector3(-40.0, 0, 0))
	_check("30 cm of new snow stays; a metre more releases on the 38 degree face", calm == 0 and released >= 1, "%d releases after 30 cm, %d after the storm" % [calm, released])
	_check("it runs out past the toe and stops (Voellmy mu 0.2, xi 1500)", not av.is_running() and av.get_run_out() > 60.0 and av.get_max_speed() > 10.0 and av.get_max_speed() < 70.0, "%.0f m run-out, %.1f m/s at most, %.0f s" % [av.get_run_out(), av.get_max_speed(), t])
	_check("it runs at Voellmy's steady speed for its depth, within 20% (second-order HLL transport)", av.get_max_speed() > 0.8 * av.get_max_speed_terminal() and av.get_max_speed() < 1.2 * av.get_max_speed_terminal(), "%.1f m/s against %.1f m/s" % [av.get_max_speed(), av.get_max_speed_terminal()])
	_check("the snow moves, none is made or lost", av.get_released_volume() > 1000.0 and absf(water1 - water0) < 0.005 * water0, "%.0f m3 released, %.0f m3 entrained, water %.0f -> %.0f t" % [av.get_released_volume(), av.get_entrained_volume(), water0 / 1000.0, water1 / 1000.0])
	_check("debris piles at the toe; the face keeps its old snow", toe > 1.3 and face < 1.0, "%.2f m at the toe (40 m out), %.2f m on the face" % [toe, face])
	# A trigger (a skier, a cornice drop) releases what natural release left.
	av.natural_release = false
	snow.add_snow(40.0, 100.0)
	await get_tree().physics_frame
	var ok := av.trigger(Vector3(-50.0, 0, 30.0), 12.0)
	_check("a trigger releases the slab round it", ok and av.is_running(), "running: %s" % av.is_running())
	while av.is_running():
		av.step(0.5)
	av.natural_release = true


func _trigger_body(at: Vector3, mass: float, size: Vector3) -> RigidBody3D:
	# A frozen body standing on the snow: a person (size.y over 1 m: a capsule 0.3 m round) or a sled (a box).
	var b := RigidBody3D.new()
	b.mass = mass
	b.freeze = true
	var cs := CollisionShape3D.new()
	if size.y > 1.0:
		var c := CapsuleShape3D.new()
		c.radius = 0.3
		c.height = size.y
		cs.shape = c
	else:
		var bx := BoxShape3D.new()
		bx.size = size
		cs.shape = bx
	b.add_child(cs)
	b.add_to_group("snow_deform")
	add_child(b)
	b.global_position = Vector3(at.x, snow.get_ground_height_at(at) + snow.get_depth_at(at) + size.y * 0.5, at.z)
	return b


func _trigger_setup() -> SnowAvalanche3D:
	await _setup("avalanche", 256.0, 128, -8.0)
	snow.drift_enabled = false
	snow.slide_enabled = false
	var av: SnowAvalanche3D = snow.get_avalanches()
	av.natural_release = false
	return av


func _run_trigger_tests() -> void:
	print("Avalanche triggers (follow-up 21)")
	var flat := SnowAvalanche3D.added_shear(1000.0, 1.0, 0.0)
	_check("a line load's shear at depth on the flat is Flamant's, 0.207 P / D (Foehn 1987)", absf(flat - 207.0) < 2.0, "%.1f Pa under 1 kN/m at 1 m" % flat)
	var face := Vector3(-40.0, 0.0, 6.0)
	var person_load := 80.0 * 9.81
	# Fresh: 60 cm of new snow holds on the 38 degree face by itself, a person standing on it releases it.
	var av := await _trigger_setup()
	snow.add_snow(60.0, 100.0)
	for i in 3:
		await get_tree().physics_frame
	var s_alone := av.get_trigger_index_at(face, 0.0)
	var s_person := av.get_trigger_index_at(face, person_load)
	var quiet := av.get_release_count()
	var person := _trigger_body(face, 80.0, Vector3(0.6, 1.8, 0.6))
	for i in 3:
		await get_tree().physics_frame
	var vol := av.get_released_volume()
	_check("60 cm of new snow holds alone; a person standing on it releases the slope", quiet == 0 and s_alone > 1.0 and av.get_trigger_count() == 1 and vol > 1000.0, "index %.2f alone, %.2f under a person; %d triggered, %.0f m3" % [s_alone, s_person, av.get_trigger_count(), vol])
	while av.is_running():
		av.step(0.5)
	person.queue_free()
	# A metre, five cold days on: it has settled and sintered, a person holds, a sled (400 kg, 2.5 m) releases it.
	av = await _trigger_setup()
	snow.add_snow(100.0, 100.0)
	await get_tree().physics_frame
	snow.advance(120.0 * 3600.0)
	for i in 3:
		await get_tree().physics_frame
	var sled_load := 400.0 * 9.81 / 2.5
	s_person = av.get_trigger_index_at(face, person_load)
	var s_sled := av.get_trigger_index_at(face, sled_load)
	person = _trigger_body(face, 80.0, Vector3(0.6, 1.8, 0.6))
	for i in 3:
		await get_tree().physics_frame
	var after_person := av.get_trigger_count()
	person.queue_free()
	await get_tree().physics_frame
	var sled := _trigger_body(face + Vector3(0, 0, 4.0), 400.0, Vector3(2.5, 0.5, 1.0))
	for i in 3:
		await get_tree().physics_frame
	_check("a metre five days on: a person holds, a sled releases it", after_person == 0 and av.get_trigger_count() == 1, "index %.2f under a person, %.2f under a sled; %d then %d triggered (%.2f m, %.0f kg/m3)" % [s_person, s_sled, after_person, av.get_trigger_count(), snow.get_depth_at(face), snow.get_density_at(face)])
	while av.is_running():
		av.step(0.5)
	sled.queue_free()
	# A buried crust: a week-old slab over facets on a crust. A heavy load on the flat collapses it (whumpf) and the
	# collapse runs back along the crust to the face, which releases from 20 m away.
	av = await _trigger_setup()
	snow.add_snow(30.0, 100.0)
	snow.add_crust(0.02, 2.0)
	snow.add_snow(100.0, 100.0)
	await get_tree().physics_frame
	snow.advance(170.0 * 3600.0)
	for i in 3:
		await get_tree().physics_frame
	var flat_at := Vector3(15.0, 0.0, 0.0)
	var i_light := av.get_trigger_index_at(flat_at, person_load)
	var i_heavy := av.get_trigger_index_at(flat_at, 3000.0)
	var light := av.apply_load(flat_at, person_load)
	var heavy := av.apply_load(flat_at, 3000.0)
	_check("a buried crust: a person on the flat holds, a heavy load collapses it and the face releases from afar", light == 0 and heavy == 1 and av.get_collapse_count() == 1 and av.is_running() and av.get_released_volume() > 500.0, "results %d, %d; %d collapse; %.2f m over the crust; index on the flat %.2f under a person, %.2f under 3 kN/m; %.0f m3 released" % [light, heavy, av.get_collapse_count(), snow.get_depth_at(flat_at), i_light, i_heavy, av.get_released_volume()])
	while av.is_running():
		av.step(0.5)
	# Snow from the weather map (not add_snow) builds the slab too, update by update.
	av = await _trigger_setup()
	_precipitate(6.0, 2.0)
	for i in 3:
		await get_tree().physics_frame
	var fell := snow.get_depth_at(face)
	_check("snowfall from the weather map builds the slab", fell > 0.02 and absf(av.get_recent_snow_at(face) - fell) < 0.1 * fell, "%.3f m fell, %.3f m of it new slab" % [fell, av.get_recent_snow_at(face)])
	# Nothing triggers with triggers off.
	av = await _trigger_setup()
	av.triggers_enabled = false
	snow.add_snow(60.0, 100.0)
	await get_tree().physics_frame
	person = _trigger_body(face, 80.0, Vector3(0.6, 1.8, 0.6))
	for i in 3:
		await get_tree().physics_frame
	_check("triggers_enabled off: the person stands on it", av.get_trigger_count() == 0 and not av.is_running(), "%d triggered" % av.get_trigger_count())
	person.queue_free()


func _run_powder_tests() -> void:
	print("Powder cloud (follow-up 23)")
	var av := await _trigger_setup()
	av.natural_release = true
	var far := Vector3(90.0, 0.0, 0.0)
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	var far0 := snow.get_depth_at(far)
	snow.add_snow(90.0, 90.0)
	var water0 := snow.get_total_water()
	# A grid over the run-out, 8 m apart: past each column's debris toe the cloud's layer is all that changed.
	var grid0: Array[float] = []
	for kz in 25:
		for kx in 31:
			grid0.append(snow.get_depth_at(Vector3(8.0 * kx, 0.0, -96.0 + 8.0 * kz)))
	await get_tree().physics_frame
	await get_tree().physics_frame
	var t := 0.0
	var peak_air := 0.0
	var peak_rho := 0.0
	while (av.is_running() or av.get_suspended_mass() > 0.0) and t < 300.0:
		av.step(0.25)
		t += 0.25
		peak_air = maxf(peak_air, av.get_suspended_mass())
	var water1 := snow.get_total_water()
	var layer := 0.0
	for kz in 25:
		for kx in range(30, -1, -1):
			var at := Vector3(8.0 * kx, 0.0, -96.0 + 8.0 * kz)
			var d := snow.get_depth_at(at) - grid0[kz * 31 + kx]
			if d >= 0.15:
				break # this column's debris toe
			if d > layer:
				layer = d
				far = at
	far0 = snow.get_depth_at(far) - layer
	_check("a fast dry avalanche throws snow into the air, and all of it settles", av.get_powder_taken() > 1000.0 and absf(av.get_powder_settled() - av.get_powder_taken()) < 0.01 * av.get_powder_taken() and av.get_suspended_mass() == 0.0, "%.1f t up (%.1f t in the air at once), %.1f t down, %.0f s" % [av.get_powder_taken() / 1000.0, peak_air / 1000.0, av.get_powder_settled() / 1000.0, t])
	_check("the cloud outruns the dense flow", av.get_powder_reach() > av.get_run_out() + 10.0, "powder settles %.0f m from the release, the dense flow ran %.0f m" % [av.get_powder_reach(), av.get_run_out()])
	_check("it leaves a thin layer of powder past the debris", layer > 0.0005 and layer < 0.15, "%.1f cm at (%.0f, %.0f) (%.2f m of snow there before)" % [layer * 100.0, far.x, far.z, far0])
	_check("no snow is made or lost", absf(water1 - water0) < 0.005 * water0, "water %.0f -> %.0f t" % [water0 / 1000.0, water1 / 1000.0])
	# A wet slide: the same load after a thaw makes no cloud.
	av = await _trigger_setup()
	snow.add_snow(30.0, 100.0)
	snow.add_snow(90.0, 90.0)
	weather.temperature = 4.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(3.0 * 3600.0)
	await get_tree().physics_frame
	var wet := snow.get_wetness_at(Vector3(-40.0, 0, 0))
	av.trigger(Vector3(-50.0, 0, 0), 30.0)
	t = 0.0
	while (av.is_running() or av.get_suspended_mass() > 0.0) and t < 300.0:
		av.step(0.25)
		t += 0.25
	_check("a wet slide makes no powder cloud", av.get_release_count() >= 1 and av.get_powder_taken() == 0.0, "wetness %.2f, %d released at %.1f m/s, %.0f kg up" % [wet, av.get_release_count(), av.get_max_speed(), av.get_powder_taken()])


func _avalanche_run(av: SnowAvalanche3D) -> float:
	var t := 0.0
	while (av.is_running() or av.get_suspended_mass() > 0.0) and t < 300.0:
		av.step(0.25)
		t += 0.25
	return t


func _debris_reach() -> float:
	# The farthest point along the run-out (x, three lines across) with 10 cm of debris over the 1.3 m storm cover.
	var far := 0.0
	for z in [-20.0, 0.0, 20.0]:
		for x in range(0, 128, 2):
			if snow.get_depth_at(Vector3(x, 0, z)) > 1.4:
				far = maxf(far, float(x))
	return far


func _run_forest_tests() -> void:
	print("Forests and avalanches (follow-up 24)")
	# Anchoring: the storm that releases the bare face holds under a closed forest (Teich et al. 2012).
	var av := await _trigger_setup()
	av.natural_release = true
	for z in range(-128, 129, 8):
		for x in range(-128, 4, 8):
			weather.add_canopy(Vector2(x, z), 7.0, 18.0, 4.0)
	weather.rebake_shelter()
	for i in 16:
		snow.advance(1.0) # the cover reads the canopy a sixteenth of its rows an update
	var face := Vector3(-40.0, 0.0, 6.0)
	var closure := weather.get_canopy_closure_at(face)
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	snow.add_snow(90.0, 90.0)
	for i in 3:
		await get_tree().physics_frame
	var s_forest := av.get_trigger_index_at(face, 0.0)
	var s_person := av.get_trigger_index_at(face, 80.0 * 9.81)
	_check("a closed forest holds the storm slab that releases on the bare face", av.get_release_count() == 0 and s_person > 1.0, "closure %.2f, %.2f m under the trees; index %.1f alone, %.1f under a person; %d released" % [closure, snow.get_depth_at(face), s_forest, s_person, av.get_release_count()])
	# Braking: the same avalanche runs out over the flat, then through a forest strip on the run-out.
	av = await _trigger_setup()
	av.natural_release = true
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	snow.add_snow(90.0, 90.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	await _avalanche_run(av)
	var open_run := _debris_reach()
	var toe_p := av.get_peak_pressure_at(Vector3(10.0, 0, 0))
	var contract_p := (WorldContracts.get_snow() as WorldSnow).avalanche_peak_pressure_at(Vector3(10.0, 0, 0))
	# The track's stand starts again where the flow broke trunks (WorldStand DISTURBANCE_AVALANCHE), not beside it.
	var stand: WorldStand = WorldContracts.get_stand()
	var broke := 0
	var broke_p := 0.0
	for x in range(-60, 12, 4):
		var at := Vector3(x, 0, 6)
		if stand.disturbance_at(at) == WorldStand.DISTURBANCE_AVALANCHE and stand.stand_age_at(at) == 0.0:
			broke += 1
			broke_p = maxf(broke_p, av.get_peak_pressure_at(at))
	# Past the run-out (debris to about 60 m) the stand stands.
	var past := stand.disturbance_at(Vector3(110, 0, 6))
	_check("the dense flow resets the stand on its track (avalanche paths stay young), not past its run-out", broke >= 3 and past != WorldStand.DISTURBANCE_AVALANCHE, "%d of 18 points on the track reset (peak %.0f kPa), kind %d past the run-out (%.1f kPa there)" % [broke, broke_p / 1000.0, past, av.get_peak_pressure_at(Vector3(110, 0, 6)) / 1000.0])
	av = await _trigger_setup()
	av.natural_release = true
	for z in range(-128, 129, 8):
		for x in range(12, 128, 8):
			weather.add_canopy(Vector2(x, z), 7.0, 18.0, 4.0)
	weather.rebake_shelter()
	for i in 16:
		snow.advance(1.0) # the cover reads the canopy a sixteenth of its rows an update
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	snow.add_snow(90.0, 90.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	await _avalanche_run(av)
	var forest_run := _debris_reach()
	_check("a forest on the run-out brakes it: its debris stops sooner, some of it among the trees", forest_run < open_run - 10.0 and av.get_detrained_volume() > 100.0, "debris (10 cm and more) reaches x = %.0f m into the forest against %.0f m in the open; %.0f m3 stopped by the stems" % [forest_run, open_run, av.get_detrained_volume()])
	_check("trees in its path take its dynamic pressure, 1/2 rho u^2 (and read it through the snow contract)", toe_p > 5000.0 and toe_p < 200000.0 and absf(contract_p - toe_p) < 1.0, "%.0f kPa at the toe, %.0f kPa through WorldSnow (Bartelt and Stoeckli 2001: trunks break from about 10-20 kPa)" % [toe_p / 1000.0, contract_p / 1000.0])


func _thaw(t: float, hours: float, rain: float = 0.0) -> void:
	# Warm air (and rain) on the pack, an hour an update so the avalanche node sees each one.
	weather.temperature = t
	weather.rain_rate = rain
	weather.cloud_cover = 1.0 if rain > 0.0 else 0.0
	weather.snap_to_settings()
	weather.advance(1.0)
	for i in int(hours):
		snow.advance(3600.0)
		await get_tree().physics_frame


func _deposit_peak() -> float:
	# The deepest snow on the run-out (x 0 to 128 m), over the gully and beside it.
	var top := 0.0
	for z in range(-30, 31, 2):
		for x in range(0, 128, 2):
			top = maxf(top, snow.get_depth_at(Vector3(x, 0, z)))
	return top


func _run_wet_tests() -> void:
	print("Wet spring avalanches (follow-up 25)")
	var face := Vector3(-40.0, 0.0, 6.0)
	# Meltwater: 60 cm of new snow that holds in frost releases by itself once a thaw wets it.
	var av := await _trigger_setup()
	av.natural_release = true
	snow.add_snow(60.0, 100.0)
	for i in 3:
		await get_tree().physics_frame
	var s_dry := av.get_trigger_index_at(face, 0.0)
	var quiet := av.get_release_count()
	await _thaw(4.0, 6.0)
	var released := av.get_release_count()
	var wet_flow := av.get_flow_wetness()
	_check("meltwater weakens the slab: what holds in frost releases in a thaw", quiet == 0 and s_dry > 1.0 and released >= 1 and wet_flow > 0.05, "index %.2f in frost, 0 released; after 6 h at +4 C %d released, the flow %.0f%% wet" % [s_dry, released, wet_flow * 100.0])
	# Rheology: the same slab triggered dry and wet. The wet one runs slower and shorter and piles up deeper.
	av = await _trigger_setup()
	snow.add_snow(60.0, 100.0)
	await get_tree().physics_frame
	av.trigger(Vector3(-50.0, 0, 0), 30.0)
	await _avalanche_run(av)
	var dry_speed := av.get_max_speed()
	var dry_out := av.get_run_out()
	var dry_pile := _deposit_peak()
	av = await _trigger_setup()
	snow.add_snow(60.0, 100.0)
	await get_tree().physics_frame
	await _thaw(4.0, 6.0)
	av.trigger(Vector3(-50.0, 0, 0), 30.0)
	var wetness := av.get_flow_wetness()
	await _avalanche_run(av)
	_check("a wet flow runs slower and stops shorter, in a deeper pile", wetness > 0.3 and av.get_max_speed() < 0.85 * dry_speed and av.get_run_out() < dry_out - 5.0 and _deposit_peak() > dry_pile, "dry %.1f m/s, %.0f m, piles %.2f m; wet (%.0f%%) %.1f m/s, %.0f m, piles %.2f m" % [dry_speed, dry_out, dry_pile, wetness * 100.0, av.get_max_speed(), av.get_run_out(), _deposit_peak()])
	# Glide: an old, bonded pack on the face; rain on it drains through to the ground. Cracks open, then it goes
	# full depth. None on the flat.
	av = await _trigger_setup()
	snow.add_snow(300.0, 150.0)
	snow.advance(10.0 * 24.0 * 3600.0) # ten cold days: the storm has bonded
	await get_tree().physics_frame
	av.natural_release = true
	var depth0 := snow.get_depth_at(face)
	var cracked_at := -1
	var hours := 0
	while hours < 96 and av.get_glide_count() == 0:
		await _thaw(3.0, 1.0, 2.0)
		hours += 1
		if cracked_at < 0 and av.get_glide_crack_count() > 0:
			cracked_at = hours
	var flat_glide := av.get_glide_at(Vector3(60.0, 0, 0))
	_check("rain on an old pack opens glide cracks, then it releases full depth", cracked_at > 0 and av.get_glide_count() >= 1 and av.get_released_volume() > 500.0 and flat_glide == 0.0, "%.2f m of old snow; cracks after %d h of rain, a glide avalanche after %d h, %.0f m3 (%.0f%% wet); no glide on the flat" % [depth0, cracked_at, hours, av.get_released_volume(), av.get_flow_wetness() * 100.0])


func _run_impulse_wave_tests() -> void:
	print("Avalanches into water (follow-up 26)")
	# Heller and Hager (2010): F 2, S 0.5, M 1 at 45 degrees gives P 1.25 and H_M 0.66 h.
	var p := SnowAvalanche3D.impulse_product(2.0 * sqrt(9.81 * 10.0), 5.0, 1000.0 * 10.0 * 100.0, 10.0, 10.0, deg_to_rad(45.0))
	var hm := SnowAvalanche3D.impulse_wave_height(p, 10.0, 0.0, 0.0)
	var far := SnowAvalanche3D.impulse_wave_height(p, 10.0, 300.0, 0.0)
	var side := SnowAvalanche3D.impulse_wave_height(p, 10.0, 300.0, deg_to_rad(90.0))
	_check("the impulse product and the wave it makes (Heller and Hager 2010; Heller et al. 2009)", absf(p - 1.251) < 0.01 and absf(hm - 6.64) < 0.05 and far < hm and side < far, "P %.3f, H_M %.2f m in 10 m of water; %.2f m 300 m out ahead, %.2f m to the side; leading period %.1f s there" % [p, hm, far, side, SnowAvalanche3D.impulse_wave_period(1.0, 10.0, 300.0)])
	# The storm avalanche runs into an open lake at the foot of the face.
	var stub := WaveStub.new()
	WorldContracts.set_water(stub)
	var av := await _lake_avalanche(0.0)
	WorldContracts.clear_overrides()
	var entered := av.get_water_entry_mass()
	var wave := av.get_wave_height_at(Vector3(60.0, 0, 0))
	var biggest: Array = []
	for w in stub.waves:
		if biggest.is_empty() or w[2] > biggest[2]:
			biggest = w
	_check("the water is handed the wave in metres and seconds (WorldWater.wave)", stub.waves.size() == av.get_wave_count() and not biggest.is_empty() and absf(biggest[2] - av.get_wave_height_at(biggest[0])) < 0.01 * biggest[2] + 1e-3 and biggest[1].x > 0.5 and absf(biggest[1].y) < 1e-4 and biggest[3] > 1.0 and biggest[3] < 60.0 and biggest[4] >= 2.0, "%d sent; biggest %.2f m, %.1f s, %.0f m wide, heading (%.2f, %.2f)" % [stub.waves.size(), biggest[2] if not biggest.is_empty() else 0.0, biggest[3] if not biggest.is_empty() else 0.0, biggest[4] if not biggest.is_empty() else 0.0, biggest[1].x if not biggest.is_empty() else 0.0, biggest[1].z if not biggest.is_empty() else 0.0])
	_check("an avalanche into open water plunges in and pushes a wave", entered > 1.0e5 and av.get_wave_count() >= 1 and wave > 0.05 and wave < 12.0, "%.0f t into the lake, %d waves; %.2f m high 60 m out (%.0f m deep there)" % [entered / 1000.0, av.get_wave_count(), wave, 15.0])
	# Thick ice carries it (no wave); thin ice breaks under it.
	av = await _lake_avalanche(30.0)
	var thick := snow.get_ice_thickness_at(Vector3(40.0, 0, 0))
	_check("thick lake ice carries the avalanche: no wave", av.get_wave_count() == 0 and av.get_ice_broken() == 0 and thick > 0.3, "%.2f m of ice; %d waves, %d ice cells broken, run-out %.0f m" % [thick, av.get_wave_count(), av.get_ice_broken(), av.get_run_out()])
	av = await _lake_avalanche(0.5)
	var thin := snow.get_ice_thickness_at(Vector3(80.0, 0, 0))
	_check("thin lake ice breaks under it, and the snow goes in", av.get_ice_broken() > 0 and av.get_water_entry_mass() > 0.0, "%.2f m of ice far out; %d cells broken, %.0f t into the lake" % [thin, av.get_ice_broken(), av.get_water_entry_mass() / 1000.0])


func _lake_avalanche(freeze_days: float) -> SnowAvalanche3D:
	# The storm (as in the avalanche tests) on the face over a lake, after freeze_days of hard frost on the lake.
	await _setup("avalanche_lake", 256.0, 128, -15.0)
	snow.drift_enabled = false
	snow.slide_enabled = false
	_lake(0.0)
	var av: SnowAvalanche3D = snow.get_avalanches()
	av.natural_release = false
	if freeze_days > 0.0:
		snow.advance(freeze_days * 24.0 * 3600.0)
	await get_tree().physics_frame
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	snow.add_snow(90.0, 90.0)
	await get_tree().physics_frame
	av.trigger(Vector3(-45.0, 0, 0), 40.0)
	await _avalanche_run(av)
	return av


func _run_fire_tests() -> void:
	print("Fire melts snow (WorldFire)")
	await _setup("flat", 32.0, 32, -8.0)
	snow.drift_enabled = false
	_precipitate(10.0, 2.0)
	var hot := Vector3(8.0, 0, 0)
	var cold := Vector3(-8.0, 0, 0)
	var h0 := snow.get_swe_at(hot)
	WorldContracts.set_fire(FireStub.new())
	snow.advance(2.0 * 3600.0)
	WorldContracts.clear_overrides()
	var lost := h0 - snow.get_swe_at(hot)
	_check("fire heat melts the pack at 0.334 MJ/kg, in frost", absf(lost - 20.0) < 1.0 and absf(snow.get_swe_at(cold) - h0) < 0.5, "%.1f mm melted in 2 h under 928 W/m2 at -8 C (energy gives 20.0); %.1f mm beside it of %.1f" % [lost, snow.get_swe_at(cold), h0])
	# The snow contract other systems read (vehicle grip, plants): the cover answers it.
	var ws: WorldSnow = WorldContracts.get_snow()
	var surf := snow.get_ground_height_at(cold) + snow.get_depth_at(cold)
	_check("the snow contract reads the cover", ws.has_provider() and absf(ws.depth_at(cold) - snow.get_depth_at(cold)) < 1e-4 and absf(ws.density_at(cold) - snow.get_density_at(cold)) < 1e-3 and absf(ws.surface_at(cold) - surf) < 1e-3 and ws.lake_ice_at(cold) == Vector2(), "depth %.3f m, density %.0f kg/m3, surface %.3f m" % [ws.depth_at(cold), ws.density_at(cold), ws.surface_at(cold)])


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


func _recipe() -> ClimateRecipe:
	var r := ClimateRecipe.new()
	r.latitude = 52.0
	r.reference_height = 0.0
	r.temperature_january = 2.0
	r.temperature_july = 19.0
	r.precipitation = 1200.0
	r.precipitation_seasonality = 0.4
	r.precipitation_peak_month = 11.0 # December
	r.wind_from = 270.0
	r.wind_speed = 9.0
	return r


func _run_recipe_tests() -> void:
	print("One climate with the terrain (SnowClimate.recipe, SnowCover3D.climate_map)")
	var r := _recipe()
	var c := _climate(5.0)
	c.recipe = r
	c.start_day = 1
	var days := c.sample_days(3650)
	var jan := 0.0
	var jan_n := 0
	var year := 0.0
	var dec := 0.0
	var jun := 0.0
	for d in 3650:
		var doy := d % 365
		year += days[d * 4 + 1] / 10.0
		if doy < 31:
			jan += days[d * 4]
			jan_n += 1
		elif doy >= 334:
			dec += days[d * 4 + 1] / 10.0
		elif doy >= 151 and doy < 181:
			jun += days[d * 4 + 1] / 10.0
	_check("recipe: its latitude, January mean and wet-day wind", is_equal_approx(c.latitude, 52.0) and absf(jan / jan_n - 2.0) < 0.6 and is_equal_approx(c.wind_speed * c.storm_wind_factor, 9.0), "latitude %.0f, January %.2f C (recipe 2.0), wet-day wind %.1f m/s" % [c.latitude, jan / jan_n, c.wind_speed * c.storm_wind_factor])
	_check("recipe: the year's precipitation", absf(year - 1200.0) < 0.08 * 1200.0, "%.0f mm a year (recipe 1200)" % year)
	_check("recipe: its seasonality (wettest in December)", dec > 1.6 * jun, "December %.0f mm, June %.0f mm (harmonic 0.4: about 2.3 to 1)" % [dec, jun])
	var l_jan := c.seasonal_lapse_rate(20.0, 6.5)
	var l_jul := c.seasonal_lapse_rate(202.0, 6.5)
	_check("recipe: winter and summer lapse rates", absf(l_jan - r.lapse_rate_january) < 0.05 and absf(l_jul - r.lapse_rate_july) < 0.05, "%.2f and %.2f C/km" % [l_jan, l_jul])

	# The terrain's climate map on the mountain: more on the windward (west) side and summit, less in the lee.
	await _setup("mountain", 512.0, 128, -2.0)
	var dm := TerrainDataMap3D.new()
	dm.name = "DataMap"
	dm.size = 4096.0
	dm.resolution = 256
	add_child(dm)
	dm.height_source = dm.get_path_to(ground)
	dm.climate_recipe = r
	var t0 := Time.get_ticks_msec()
	dm.bake_climate()
	print("  data map climate on 4 km: %d ms" % (Time.get_ticks_msec() - t0))
	snow.climate_map = snow.get_path_to(dm)
	var flat := snow.get_orographic_factor_at(Vector3(-1900, 0, 1900), 0.0)
	var top := Vector3.ZERO
	var top_h := -1e9
	for i in 4000:
		var q := Vector3(randf_range(-800, 800), 0, randf_range(-800, 800))
		var hq: float = ground.get_height(q)
		if hq > top_h:
			top_h = hq
			top = q
	var windward := snow.get_orographic_factor_at(top + Vector3(-500, 0, 0), 0.0)
	var lee := snow.get_orographic_factor_at(top + Vector3(900, 0, 0), 0.0)
	var probe := ""
	for dx in [-1500, -1000, -500, 0, 500, 1000, 1500, 2000]:
		probe += " %d m x%.2f" % [dx, snow.get_orographic_factor_at(top + Vector3(dx, 0, 0), 0.0)]
	print("  precipitation factor along the wind (summit at 0):" + probe)
	_check("climate map: the mountain gets more than flat ground", windward > 1.1 and lee > 1.1 and absf(flat - 1.0) < 0.15, "x%.2f windward, x%.2f in the lee, x%.2f on far flat ground" % [windward, lee, flat])
	# The same wind from the east mirrors it.
	var r2 := _recipe()
	r2.wind_from = 90.0
	dm.climate_recipe = r2
	dm.bake_climate()
	var windward2 := snow.get_orographic_factor_at(top + Vector3(500, 0, 0), 0.0)
	var lee2 := snow.get_orographic_factor_at(top + Vector3(-900, 0, 0), 0.0)
	_check("climate map: an east wind mirrors it", absf(windward2 - windward) < 0.1 * windward and absf(lee2 - lee) < 0.1 * lee, "x%.2f windward, x%.2f in the lee" % [windward2, lee2])
	dm.climate_recipe = r
	dm.bake_climate()
	# A winter from the same recipe, with the map and without: the map moves snow water to the windward side
	# (drift still loads the lee in both).
	var ratios := []
	for use_map in [false, true]:
		snow.climate_map = snow.get_path_to(dm) if use_map else NodePath()
		var mc := _climate(5.0)
		mc.recipe = r
		snow.climate = mc
		t0 = Time.get_ticks_msec()
		snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 256)
		print("  bake with the recipe%s: %.1f s" % [" and climate map" if use_map else "", (Time.get_ticks_msec() - t0) / 1000.0])
		var w_sum := 0.0
		var l_sum := 0.0
		seed(7)
		for i in 20000:
			var q := Vector3(randf_range(-1400, 1400), 0, randf_range(-1400, 1400))
			var hq: float = ground.get_height(q)
			if hq < 350.0 or hq > 550.0:
				continue
			if q.x < top.x - 150.0:
				w_sum += snow.get_world_swe_at(q)
			elif q.x > top.x + 150.0:
				l_sum += snow.get_world_swe_at(q)
		ratios.append(w_sum / maxf(l_sum, 1.0))
	# Where the map puts more precipitation, the bake puts more snow.
	var map_ratio := snow.get_orographic_factor_at(top + Vector3(-500, 0, 0), 0.0) / snow.get_orographic_factor_at(top + Vector3(900, 0, 0), 0.0)
	_check("climate map: the bake's snow follows its precipitation", (ratios[1] - ratios[0]) * (map_ratio - 1.0) > 0.0 and absf(ratios[1] / ratios[0] - 1.0) > 0.1, "windward over lee snow water at 350-550 m: %.2f with the map, %.2f without (map's precipitation windward over lee %.2f)" % [ratios[1], ratios[0], map_ratio])
	dm.queue_free()


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
	# Two-way: what the square does goes back to the world tier under it (fresh snow here), and not beyond.
	var fq := snow.global_position + Vector3(37.0, 0, -51.0)
	var far_q := snow.global_position + Vector3(-700.0, 0, 300.0)
	var w_before := snow.get_world_swe_at(fq)
	var far_before := snow.get_world_swe_at(far_q)
	snow.add_snow(30.0, 100.0, false) # the square alone: what feeds back is the square's
	var near_now := 0.0
	for k in 16:
		near_now += snow.get_swe_at(fq + Vector3((k % 4) * 4.0 - 6.0, 0, (k / 4) * 4.0 - 6.0)) / 16.0
	t0 = Time.get_ticks_usec()
	snow.advance_world(3600.0)
	print("  world tier, 256 x 256 cells, one update: %.1f ms" % ((Time.get_ticks_usec() - t0) / 1000.0))
	var w_after := snow.get_world_swe_at(fq)
	var far_after := snow.get_world_swe_at(far_q)
	_check("the square feeds the world tier under it (two-way nesting)", w_after > w_before + 10.0 and absf(w_after - near_now) < 0.05 * maxf(near_now, 10.0) and absf(far_after - far_before) < 5.0, "world %.0f mm before, %.0f after, square %.0f mm there; far away %.0f to %.0f mm" % [w_before, w_after, near_now, far_before, far_after])
	# A storm (add_snow's default) falls on the world tier too: slopes outside the square carry it.
	var storm_before := snow.get_world_swe_at(far_q)
	snow.add_snow(20.0, 100.0)
	_check("add_snow falls on the world tier outside the square", absf(snow.get_world_swe_at(far_q) - storm_before - 20.0) < 1.0, "%.0f to %.0f mm at 760 m" % [storm_before, snow.get_world_swe_at(far_q)])
	await _run_melt_runoff_tests()

	await _run_recipe_tests()

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
	var mild_lens := []
	for end_days in [151, 182]:
		# To 1 March, and on to 1 April through its first spring thaws (follow-up 6: cornices fall).
		mild.days = end_days
		snow.climate = mild
		snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), 256)
		var mild_len := 0.0
		for z in range(-300, 300, 8):
			for x in range(-40, 24, 4):
				mild_len = maxf(mild_len, snow.get_cornice_at(Vector3(x, 0, z)).length())
		mild_lens.append(mild_len)
	_check("cornices on a milder ridge at 16 m cells, falling in its March thaws", mild_lens[0] > 1.0 and mild_lens[1] < mild_lens[0], "%.2f m on 1 March, %.2f m on 1 April" % [mild_lens[0], mild_lens[1]])
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


# Melt water downhill (round 6, docs/ideas/snow.md): a warm clear spell melts the world tier's snow outside the
# square, and that melt is the weather's ground water there (what WeatherRunoff3D routes into streams); inside
# the square the square's own runoff stands for it, counted once.
func _run_melt_runoff_tests() -> void:
	weather.temperature = 14.0
	weather.snap_to_settings()
	for h in 4:
		snow.advance(3600.0)
		snow.advance_world(3600.0)
	weather.advance(60.0)
	var cs := weather.extent / weather.resolution
	var best := Vector3.ZERO
	var melt := 0.0
	for i in range(-7, 7):
		for j in range(-7, 7):
			var q := Vector3((i + 0.5) * cs, 0, (j + 0.5) * cs)
			if absf(q.x - snow.global_position.x) < snow.extent and absf(q.z - snow.global_position.z) < snow.extent:
				continue
			var r := snow.get_runoff_at(q)
			if r > melt:
				melt = r
				best = q
	var got := weather.get_ground_water_input_at(best)
	print("  melt: %.2f mm/h leaves the world tier's pack at %s; the weather's ground water there %.2f mm/h (rain %.2f)" % [melt, str(best), got, weather.get_rainfall_at(best)])
	_check("melt outside the square reaches the weather's ground water", melt > 0.1 and absf(got - melt) < 0.05 * melt + 0.01, "%.2f mm/h melt, %.2f mm/h ground water at %s" % [melt, got, str(best)])
	# Inside the square: the weather cell's centre reads the square's runoff (averaged to the world's 16 m
	# cells), not the square's and the world's added. Rain on the square cleared bare, the world's pack under it.
	snow.clear()
	weather.rain_rate = 4.0
	weather.cloud_cover = 1.0
	weather.snap_to_settings()
	weather.advance(60.0)
	snow.advance(3600.0)
	weather.advance(60.0)
	var c := snow.global_position + Vector3(0.5 * cs - fposmod(snow.global_position.x, cs), 0, 0.5 * cs - fposmod(snow.global_position.z, cs))
	var near_avg := 0.0
	for k in 64:
		near_avg += snow.get_runoff_at(c + Vector3((k % 8) * 4.0 - 14.0, 0, (k / 8) * 4.0 - 14.0)) / 64.0
	var got_in := weather.get_ground_water_input_at(c)
	print("  square: %.1f mm snow, rain %.2f mm/h at %s" % [snow.get_swe_at(c), weather.get_rainfall_at(c), str(c)])
	_check("the square's runoff is counted once", near_avg > 1.0 and absf(got_in - near_avg) < 0.25 * near_avg + 0.02, "%.2f mm/h ground water at %s, the square's %.2f mm/h (bare), the world tier's pack %.0f mm" % [got_in, str(c), near_avg, snow.get_world_swe_at(c)])
	weather.rain_rate = 0.0
	weather.cloud_cover = 0.0
	weather.snap_to_settings()


func _longest_cornice() -> Vector3:
	# The crest cell with the longest cornice on the cornice ridge (x, z, length).
	var best := Vector3.ZERO
	for z in range(-300, 300, 8):
		for x in range(-16, 8, 4):
			var l := snow.get_cornice_at(Vector3(x, 0, z)).length()
			if l > best.z:
				best = Vector3(x, z, l)
	return best


func _run_failure_tests() -> void:
	print("Cornice failure (follow-up 6):")
	var cold := SnowCover3D.cornice_crack_rate(4.0, 0.0, -5.0, 0.0) * 100.0
	var zero := SnowCover3D.cornice_crack_rate(4.0, 0.0, 0.0, 0.0) * 100.0
	var wet := SnowCover3D.cornice_crack_rate(4.0, 0.0, 0.0, 0.08) * 100.0
	var late := SnowCover3D.cornice_crack_rate(4.0, 0.3, 0.0, 0.08) * 100.0
	_check("crack opening rate against Gruvefjellet (0.4 to 2.8 cm a day, 8.6 before failing; Vogel et al. 2012)", cold > 0.3 and cold < 2.8 and zero > cold and zero < 2.8 and wet > zero and late > wet and late < 20.0, "4 m slab: %.2f cm/day at -5 C, %.2f at 0 C, %.2f wet, %.2f wet with the crack 30 cm open" % [cold, zero, wet, late])
	var fall_len := 0.0
	for i in 400:
		var l := 0.05 * i
		if SnowCover3D.cornice_root_stress(l, 0.0, 0.0) > SnowCover3D.cornice_strength(0.0):
			fall_len = l
			break
	_check("a cold, uncracked cornice falls near the longest field cornices", fall_len > 7.0 and fall_len < 13.0, "at %.1f m (root strength %.1f kPa)" % [fall_len, SnowCover3D.cornice_strength(0.0)])
	# A baked ridge (1 April), left in calm cold, in a calm thaw, and with failure off: the day the longest
	# cornice falls, its crack after a week, the debris below.
	var days := {}
	for mode in ["cold", "thaw", "off"]:
		await _setup("cornice", 256.0, 64, -10.0)
		snow.cornice_failure_enabled = mode != "off"
		snow.climate = _alpine(5.0)
		snow.bake_climate(Rect2(-512, -512, 1024, 1024), 128)
		var failures := []
		snow.cornice_failed.connect(func(pos, dir, length, mass): failures.append([pos, dir, length, mass]))
		weather.wind_speed = 1.0
		weather.temperature = 1.5 if mode == "thaw" else -10.0
		weather.snap_to_settings()
		weather.advance(1.0)
		var c0 := _longest_cornice()
		var at0 := Vector3(c0.x, 0, c0.y)
		var lee0 := []
		for k in 24:
			lee0.append(snow.get_world_swe_at(Vector3(c0.x + 8.0 * (k + 1), 0, c0.y)))
		var day := -1
		var week_crack := 0.0
		for d in 60:
			snow.advance_world(86400.0)
			if d == 6:
				week_crack = snow.get_cornice_crack_at(at0)
			if day < 0 and snow.get_cornice_at(at0).length() < 0.25 * c0.z:
				day = d + 1
				if mode != "off":
					break
		days[mode] = day
		if mode == "off":
			_check("failure off: none fall", failures.size() == 0 and snow.get_cornice_failure_count() == 0, "%d" % failures.size())
			continue
		var mass := 0.0
		for f in failures:
			mass += f[3]
		var gain := -INF
		var out := 0.0
		for k in 24:
			var g: float = snow.get_world_swe_at(Vector3(c0.x + 8.0 * (k + 1), 0, c0.y)) - lee0[k]
			if g > gain:
				gain = g
				out = 8.0 * (k + 1)
		print("  %s: the longest cornice %.1f m, its crack %.1f cm after a week, gone on day %d; %d failures on the ridge (%.0f t), %.0f mm more snow %.0f m out from the crest" % [mode, c0.z, week_crack * 100.0, day, failures.size(), mass / 1000.0, gain, out])
		if mode == "cold":
			_check("calm cold: the crack opens about a centimetre a day, the cornice falls after weeks, not days (4 to 5 weeks from crack to failure at Gruvefjellet; Vogel et al. 2012)", week_crack > 0.03 and week_crack < 0.2 and day > 14, "%.1f cm after a week, gone on day %d" % [week_crack * 100.0, day])
			if failures.size() > 0:
				var f: Array = failures[0]
				_check("a failed slab falls out over the lee, its debris lands below", f[1].x > 0.8 and f[2] > 0.5 and gain > 0.0, "direction %s, %.1f m long; %.0f mm of debris" % [str(f[1]), f[2], gain])
		else:
			_check("a thaw brings them down sooner", day > 0 and day < days["cold"], "day %d against %d" % [day, days["cold"]])


func _run_icicle_attach_tests() -> void:
	# Roots joined to what they hang from: an eave slab 0.2 m above the edge (a placement that leaves the
	# roots under it, as at the hut), and a gap past the reach.
	var holder := Node3D.new()
	add_child(holder)
	var slab := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(2.4, 0.1, 0.6)
	slab.mesh = bm
	slab.position = Vector3(0, 0.25, 0)
	holder.add_child(slab)
	var ice := SnowIcicles3D.new()
	holder.add_child(ice)
	ice.edge = PackedVector3Array([Vector3(-1, 0, 0), Vector3(1, 0, 0)])
	for day in 3:
		ice.grow(6.0, -4.0, 0.5, 0.9)
		ice.grow(18.0, -8.0, 0.5, 0.0)
	await get_tree().process_frame
	var lo := INF
	var hi := -INF
	var n := 0
	for k in ice.get_icicle_count():
		if ice.get_icicle_length(k) < 0.01:
			continue
		lo = minf(lo, ice.get_icicle_attach(k))
		hi = maxf(hi, ice.get_icicle_attach(k))
		n += 1
	_check("roots join the eave above them (0.2 m up)", n > 0 and lo > 0.195 and hi < 0.205, "%d icicles, %.3f to %.3f m" % [n, lo, hi])
	ice.attach_reach = 0.15
	var far := 0.0
	for k in ice.get_icicle_count():
		far = maxf(far, ice.get_icicle_attach(k))
	_check("nothing within reach: roots left as placed", far == 0.0, "%.3f m" % far)
	holder.queue_free()


func _run_icicle_tests() -> void:
	print("Icicles (follow-up 6):")
	var tip_still := SnowIcicles3D.tip_growth_rate(-10.0, 0.0) * 100.0
	var tip_wind := SnowIcicles3D.tip_growth_rate(-10.0, 2.0) * 100.0
	var wall_still := SnowIcicles3D.wall_growth_rate(-10.0, 0.0, 0.01, 0.2) * 2000.0
	var wall_wind := SnowIcicles3D.wall_growth_rate(-10.0, 2.0, 0.01, 0.2) * 2000.0
	print("  at -10 C: tip %.2f cm/h still, %.2f at 2 m/s; diameter of a 20 cm icicle %.2f mm/h still, %.2f at 2 m/s" % [tip_still, tip_wind, wall_still, wall_wind])
	var ratio := tip_still * 10.0 / wall_still
	_check("length against diameter growth in still air (8 to 32 measured; Maeno et al. 1994)", ratio > 4.0 and ratio < 64.0, "%.1f" % ratio)
	_check("colder and windier grow faster, none above 0 C", SnowIcicles3D.tip_growth_rate(-20.0, 0.0) > SnowIcicles3D.tip_growth_rate(-5.0, 0.0) and tip_wind > tip_still and SnowIcicles3D.tip_growth_rate(1.0, 2.0) == 0.0, "tip at -5 C %.2f, -20 C %.2f cm/h" % [SnowIcicles3D.tip_growth_rate(-5.0, 0.0) * 100.0, SnowIcicles3D.tip_growth_rate(-20.0, 0.0) * 100.0])
	var ice := SnowIcicles3D.new()
	ice.edge = PackedVector3Array([Vector3(-1, 0, 0), Vector3(1, 0, 0)])
	# Melt off a sunny roof: 0.3 mm/h over a 3 m catchment, 6 hours a day, at -4 C and still air.
	var supplied := 0.0
	for day in 3:
		ice.grow(6.0, -4.0, 0.5, 0.9)
		supplied += 6.0 * 0.9 * 2.0
		ice.grow(18.0, -8.0, 0.5, 0.0)
	var longest := ice.get_longest()
	_check("three sunny days at -4 C grow icicles", longest > 0.1 and longest < 1.5, "longest %.2f m of %d sites, %.3f kg of ice" % [longest, ice.get_icicle_count(), ice.get_ice_mass()])
	_check("ice is no more than the water that came", ice.get_ice_mass() <= supplied * 1.001, "%.3f kg of %.3f kg supplied (%.3f kg/h dripped on the last day)" % [ice.get_ice_mass(), supplied, ice.get_last_dripped()])
	var l0 := ice.get_longest()
	var m0 := ice.get_ice_mass()
	ice.grow(24.0, -8.0, 0.5, 0.004)
	_check("a trickle the wall freezes: thicker, no longer (Szilder and Lozowski 1994)", ice.get_longest() < l0 + 0.002 and ice.get_ice_mass() > m0, "longest %.3f to %.3f m, %.3f to %.3f kg" % [l0, ice.get_longest(), m0, ice.get_ice_mass()])
	# Shape (follow-up 37): the film's own lean stays gentle; a steady wind leans the tips downwind.
	var lean_still := 0.0
	var k_long := 0
	for k in ice.get_icicle_count():
		if ice.get_icicle_length(k) > ice.get_icicle_length(k_long):
			k_long = k
		var len_k := ice.get_icicle_length(k)
		if len_k > 0.05:
			lean_still = maxf(lean_still, ice.get_icicle_offset(k, len_k).length() / len_k)
	_check("in still air an icicle hangs near plumb (its film's side bends it a little)", lean_still < 0.1, "steepest lean %.3f" % lean_still)
	var windy := SnowIcicles3D.new()
	windy.edge = PackedVector3Array([Vector3(-1, 0, 0), Vector3(1, 0, 0)])
	windy.wind_direction = Vector3(1, 0, 0)
	for day in 3:
		windy.grow(6.0, -4.0, 5.0, 0.9)
		windy.grow(18.0, -8.0, 0.5, 0.0)
	var lean_x := 0.0
	var n_long := 0
	for k in windy.get_icicle_count():
		var len_k := windy.get_icicle_length(k)
		if len_k > 0.05:
			lean_x += windy.get_icicle_offset(k, len_k).x / len_k
			n_long += 1
	lean_x /= maxf(n_long, 1)
	_check("a 5 m/s wind leans the tips downwind (the drop's drag; Maeno et al. 1994)", lean_x > 0.3 and lean_x < 0.8, "mean lean %.2f (%.0f degrees) over %d icicles" % [lean_x, rad_to_deg(atan(lean_x)), n_long])
	windy.grow(0.5, -4.0, 0.5, 0.9)
	var drips := 0
	for k in windy.get_icicle_count():
		if windy.is_icicle_dripping(k):
			drips += 1
	_check("water left at the tips hangs as drops while they grow", drips > 0, "%d of %d tips dripping" % [drips, windy.get_icicle_count()])
	windy.free()
	ice.grow(12.0, 4.0, 2.0, 0.0)
	_check("a thaw melts them back", ice.get_longest() < l0 * 0.8, "%.3f m after 12 h at +4 C" % ice.get_longest())
	ice.free()


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
	elif view == "rime":
		await _setup("flat", 32.0, 128, -6.0)
	elif view == "lake":
		await _setup("mountain", 512.0, 128, -12.0)
	elif view == "avalanche":
		if OS.get_cmdline_user_args().has("--powder"):
			# Wider, at the same 2 m cells: the deep-snow geometry the shot sees (320 m) stays over the cover, whose
			# edge otherwise showed as a dotted seam across the far slope.
			await _setup("avalanche_bowl", 320.0, 160, -8.0)
		else:
			await _setup("avalanche", 256.0, 128, -8.0)
	elif view == "windblown":
		await _setup("ridge", 512.0, 256, -8.0)
	elif view == "spindrift":
		await _setup("cornice", 512.0, 256, -12.0)
	elif view == "icicles" and OS.get_cmdline_user_args().has("--hero") and not OS.get_cmdline_user_args().has("--wall"):
		# --hero looks out from under the roof: snow out to 64 m, past where the view meets the horizon haze.
		await _setup("flat", 128.0, 256, -6.0)
	elif view == "shelter" or view == "icicles":
		await _setup("flat", 32.0, 128, -6.0)
	else:
		await _setup("flat", 32.0, 128, -4.0)
	var env := Environment.new()
	# A sky, so path traced shadows are lit by it as raster's are by the ambient.
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	if view == "icicles":
		# A snowy land below the horizon, not the default brown: clear ice shows what lies behind and below it.
		sky_material.ground_horizon_color = Color(0.72, 0.75, 0.8)
		sky_material.ground_bottom_color = Color(0.55, 0.58, 0.62)
	elif view == "avalanche":
		# Snow below the horizon, as bright as the snow there is under this sky (albedo 0.84 under the sky's own
		# colours), so raster's ambient lights walls as the path tracer's bounce off the real snow does.
		sky_material.ground_horizon_color = Color(0.53, 0.54, 0.56)
		sky_material.ground_bottom_color = Color(0.33, 0.38, 0.46)
	sky.sky_material = sky_material
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--pt-debug="):
			env.pathtracing_debug_mode = int(a.substr(11)) # Environment.PathtracingDebugMode
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
	elif view == "rime":
		await _build_rime(cam)
		return
	elif view == "lake":
		await _build_lake(cam)
		return
	elif view == "windblown":
		await _build_windblown(cam)
		return
	elif view == "spindrift":
		await _build_spindrift(cam)
		return
	elif view == "shelter":
		await _build_shelter(cam)
		return
	elif view == "icicles":
		await _build_icicles(cam)
		return
	elif view == "bodies":
		await _build_bodies(cam)
		return
	elif view == "avalanche":
		await _build_avalanche(cam)
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
	if OS.get_cmdline_user_args().has("--close"):
		# Down on the boot prints, the sun behind them.
		cam.position = Vector3(-1.2, 1.1, 2.6)
		cam.look_at(Vector3(-2.9, 0.3, 1.0))
		cam.fov = 50.0
		if OS.get_cmdline_user_args().has("--sled"):
			# Down on the end of the sled's track and the berm its bow wave left.
			cam.position = Vector3(0.2, 1.0, 5.6)
			cam.look_at(Vector3(-2.2, 0.2, 3.4))
	if OS.get_cmdline_user_args().has("--prints-close"):
		# From 1.5 m over two boot prints, the sun high enough to light their floors: the pieces of a broken crust.
		cam.position = Vector3(-1.4, 1.05, 2.35)
		cam.look_at(Vector3(-2.6, 0.3, 1.35))
		cam.fov = 45.0
		sun.rotation_degrees = Vector3(-38.0, 100.0, 0.0) # higher and from the side, into the prints
	if OS.get_cmdline_user_args().has("--clumps-close"):
		# Low beside the small ball's furrow, looking back along it: the clumps it broke out of the rim.
		cam.position = Vector3(-1.2, 0.8, 0.2)
		cam.look_at(Vector3(-4.2, 0.4, -0.9))
		cam.fov = 45.0
	var target := Marker3D.new()
	target.name = "Probe"
	add_child(target)
	await get_tree().physics_frame
	snow.refresh_heights()
	var hours := snow_hours if snow_hours >= 0.0 else 0.0
	if hours > 0.0:
		_precipitate(hours, 2.0)
	elif OS.get_cmdline_user_args().has("--settled"):
		snow.add_snow(120.0, 300.0) # 0.4 m of settled snow: cohesive, prints break lumps out of it
	elif slush_mm > 0.0:
		# --slush=MM: 15 cm of old spring snow on concrete frost (a stand-in ground answering frost_at), then rain
		# at +8 C for ten hours: what the pack cannot hold stays in its base.
		var frozen := GDScript.new()
		frozen.source_code = "extends WorldGround\nfunc _frost_at(_p: Vector3) -> Vector3:\n\treturn Vector3(0.0, 0.3, 0.0)\n"
		frozen.reload()
		WorldContracts.set_ground(frozen.new())
		snow.add_snow(50.0, 330.0)
		print("slush: before the rain %.3f m, %.0f mm" % [snow.get_depth_at(Vector3(0, 0, 1)), snow.get_swe_at(Vector3(0, 0, 1))])
		weather.temperature = 8.0
		_precipitate(10.0, slush_mm / 10.0)
		var sp := Vector3(0.0, 0, 1.0)
		print("slush: %.3f m of %.3f m, wetness %.2f, %.0f mm SWE" % [snow.get_slush_at(sp), snow.get_depth_at(sp), snow.get_wetness_at(sp), snow.get_swe_at(sp)])
	else:
		snow.add_snow(45.0, 110.0) # 0.41 m of fresh snow
	if crust_cycles > 0.0:
		# A 2 cm melt-freeze crust on top: one cycle breaks under a boot, two hold it.
		snow.add_crust(0.02, crust_cycles)
	if suncups_m > 0.0:
		snow.add_suncups(suncups_m) # --suncups=M: a sunny spring week's hollows (the deep-snow surface draws them)
	if sastrugi_arg != "":
		var sa := sastrugi_arg.split(",")
		var deg := float(sa[1]) if sa.size() > 1 else 0.0
		snow.add_sastrugi(float(sa[0]), Vector2(cos(deg_to_rad(deg)), sin(deg_to_rad(deg)))) # a gale's carving
	surface = SnowSurface3D.new()
	surface.crust_plates = not OS.get_cmdline_user_args().has("--no-plates") # --no-plates: the field's ramps alone
	surface.snow_cover = NodePath("../SnowCover3D")
	surface.follow_target = NodePath("../Probe")
	if OS.get_cmdline_user_args().has("--close") or OS.get_cmdline_user_args().has("--prints-close"):
		# Triangles as fine as the deformation texels (6 cm), for the pieces of a broken crust.
		surface.detail_spacing = 0.0625
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--deform="):
			# The deformation field's texels and extent, and the triangles' spacing: --deform=768,24[,0.03125].
			var v := a.substr(9).split(",")
			surface.deform_resolution = int(v[0])
			surface.deform_extent = float(v[1])
			if v.size() > 2:
				surface.detail_spacing = float(v[2])
		if a == "--gpu=0":
			surface.gpu_simulation = false # the print field on the CPU (default: compute shaders where there is a GPU)
		if a == "--no-clumps":
			surface.clumps = false # no snow clumps beside the prints (ground detail)
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
	walker.set_meta("world_contact_exclude", true) # only its boots print; the capsule is a stand-in
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
	if OS.get_cmdline_user_args().has("--sled"):
		# A sled pulled fast (4 m/s) through the snow, its underside 15 cm down: a bow wave rides ahead and
		# is left as a berm where it stops.
		var sled := Node3D.new()
		sled.name = "Sled"
		var sc := CollisionShape3D.new()
		sc.shape = BoxShape3D.new()
		(sc.shape as BoxShape3D).size = Vector3(1.2, 0.2, 0.5)
		sled.add_child(sc)
		var smi := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(1.2, 0.2, 0.5)
		smi.mesh = bm
		smi.material_override = dark
		sled.add_child(smi)
		sled.add_to_group("snow_deform")
		add_child(sled)
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
	surface.sync_deformation()
	var mv := surface.get_moved_volume()
	print("print field on the %s: dent %.4f m3, rim %.4f m3" % ["GPU" if surface.is_gpu_active() else "CPU", mv.x, mv.y])
	var clumps: SnowClumps3D = surface.get_clumps_node()
	if clumps:
		clumps.update_now()
		print("snow clumps: %d lying, %d broken off; update %.2f ms mean, %.2f ms worst (%d updates)" % [clumps.get_clump_count(), clumps.get_spawned_total(), clumps.get_update_msec_mean(), clumps.get_update_msec_worst(), clumps.get_update_runs()])
		if OS.get_cmdline_user_args().has("--away"):
			# The field leaves for 300 m and comes back: the clumps are kept meanwhile and lie where they were.
			var lying := clumps.get_clump_count()
			var probe: Node3D = get_node("Probe")
			probe.global_position = Vector3(300.0, 0, 0)
			for i in 6:
				await get_tree().physics_frame
			clumps.update_now()
			var away := clumps.get_parked_count()
			probe.global_position = Vector3.ZERO
			for i in 30:
				await get_tree().physics_frame
			surface.sync_deformation()
			for i in 10:
				clumps.update_now()
			print("snow clumps away and back: %d lying, %d kept while away, %d lying again, %d still kept" % [lying, away, clumps.get_clump_count(), clumps.get_parked_count()])
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
		_add_terrain(1024.0, 512, Color(0.36, 0.34, 0.31), 64.0, fall)
		var cor := SnowCornice3D.new()
		cor.name = "Cornices"
		cor.follow_target = NodePath("../Ground")
		add_child(cor)
		cor.rebuild()
		var row := "cornice lengths along the crest:"
		for z in range(-80, 81, 16):
			row += " %d: %.1f" % [z, snow.get_cornice_at(Vector3(-4, 0, z)).length()]
		print(row, ", %d segments" % cor.get_segment_count())
		if crack_days > 0 or fall:
			# A calm thaw after the bake: the tension cracks open (follow-up 6); with --fall, on until the
			# first cornice in view falls, then a moment for its blocks to tumble.
			weather.wind_speed = 1.0
			weather.temperature = 1.0
			weather.snap_to_settings()
			weather.advance(1.0)
			var n0 := snow.get_cornice_failure_count()
			var fell := []
			snow.cornice_failed.connect(func(pos, dir, length, mass): fell.append(pos))
			for d in (crack_days if crack_days > 0 else 60):
				snow.advance_world(86400.0)
				if fall and fell.size() > 0:
					print("a cornice fell on day %d (%d along the ridge), %d blocks, at %s" % [d + 1, snow.get_cornice_failure_count() - n0, cor.get_debris_count(), str(fell[0])])
					fall_at = fell[0]
					break
			if not fall:
				cor.rebuild()
			var crow := "cracks along the crest (cm):"
			for z in range(-80, 81, 16):
				crow += " %d: %.0f" % [z, snow.get_cornice_crack_at(Vector3(-4, 0, z)) * 100.0]
			print(crow)
		# From the lee side, along the crest, so the overhang shows in profile; morning sun from the south-east.
		sun.rotation_degrees = Vector3(-30.0, 50.0, 0.0)
		cam.position = Vector3(26.0, 146.0, 40.0)
		cam.far = 3000.0
		cam.look_at(Vector3(-2.0, 150.0, -10.0))
		cam.fov = 50.0
		if fall_at != Vector3.ZERO:
			# From the lee, across the face below where it broke off, the blocks a moment into their fall.
			cam.position = fall_at + Vector3(28.0, -4.0, 20.0)
			cam.look_at(fall_at + Vector3(3.0, -6.0, 0.0))
			for f in 40:
				await get_tree().physics_frame
		elif crack_days > 0:
			# Close over a cornice's roof, from the windward side, to see its crack.
			cam.position = Vector3(-14.0, 157.0, 10.0)
			cam.look_at(Vector3(2.0, 146.0, -2.0))
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


func _build_windblown(cam: Camera3D) -> void:
	# A 100 m ridge after 20 mm of new snow and a day of 15 m/s wind from the west (left): the windward
	# shoulder scoured, the snow laid in the lee below the crest. --transport=0 for the same day without it.
	snow.drift_enabled = false
	snow.wind_transport_enabled = transport
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-24.0, -150.0, 0.0)
	_add_terrain(512.0, 256, Color(0.33, 0.31, 0.27), 32.0)
	await get_tree().physics_frame
	snow.refresh_heights()
	snow.add_snow(20.0, 100.0)
	weather.wind_speed = 15.0
	weather.wind_direction = Vector2(1, 0)
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance((snow_hours if snow_hours >= 0.0 else 24.0) * 3600.0)
	var prof := "profile (x: mm):"
	for x in [-60, -30, -16, -8, 0, 8, 16, 30, 60]:
		prof += " %d: %.0f" % [x, snow.get_swe_at(Vector3(x, 0, 0))]
	print(prof)
	# From the south along the crest, a little to the lee: both flanks, the wind blowing left to right.
	cam.position = Vector3(40.0, 175.0, 260.0)
	cam.far = 2000.0
	cam.look_at(Vector3(0.0, 60.0, 0.0))
	cam.fov = 45.0
	for i in 4:
		await get_tree().physics_frame
	await _save_frame("")
	get_tree().quit()


func _build_spindrift(cam: Camera3D) -> void:
	# The cornice ridge (crest along z, 150 m up, a 50 degree lee face to the east) after 0.3 m of new snow, in a
	# westerly gale: snow streams off the crest and out over the lee in plumes (follow-up 42).
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 800.0
	sun.rotation_degrees = Vector3(-22.0, 150.0, 0.0) # low, from the south-east: the plumes lit from the side
	_add_terrain(512.0, 256, Color(0.33, 0.31, 0.27), 32.0)
	await get_tree().physics_frame
	snow.refresh_heights()
	snow.add_snow(30.0, 100.0)
	var wind := 20.0
	var at_s := 12.0
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--wind="):
			wind = float(a.substr(7))
		if a.begins_with("--at="):
			at_s = float(a.substr(5))
	weather.wind_speed = wind
	weather.wind_direction = Vector2(1, 0)
	weather.humidity = 0.7
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(600.0)
	print("spindrift: %.0f kg/s off the crest from %d sources at %.0f m/s" % [snow.get_spindrift_rate(), snow.get_spindrift_source_count(), wind])
	print("  world wind at the crest %s" % str(WorldContracts.get_weather().wind_at(Vector3(0, 160, 0))))
	for sd: Dictionary in snow.get_spindrift_sources().slice(0, 3):
		print("  source at %s, %s m/s, %.0f kg/s" % [str(sd["position"]), str(sd["velocity"]), sd["rate"]])
	# From the south along the ridge, a little above the crest: the ridge in profile, the wind blowing left to right
	# and the plumes streaming off the crest over the lee against the sky.
	cam.position = Vector3(70.0, 185.0, 470.0)
	cam.far = 3000.0
	cam.look_at(Vector3(70.0, 150.0, 0.0))
	cam.fov = 50.0
	var wenv: Environment = null
	for c in get_children():
		if c is WorldEnvironment:
			wenv = c.environment
	var traced := wenv != null and wenv.pathtracing_enabled
	if traced:
		wenv.pathtracing_enabled = false
	# One physics tick a frame so Flow steps with the frames (as the powder view).
	Engine.max_physics_steps_per_frame = 1
	for i in int(at_s * Engine.physics_ticks_per_second):
		await get_tree().physics_frame
	var cloud: SnowPowderCloud3D = snow.get_spindrift()
	print("spindrift: flow %s, %d emitters" % ["yes" if cloud and cloud.has_flow() else "no", cloud.get_active_sources() if cloud else 0])
	if traced:
		await _save_frame("_raster")
		process_mode = Node.PROCESS_MODE_ALWAYS
		get_tree().paused = true
		wenv.pathtracing_enabled = true
	await _save_frame("")
	get_tree().quit()


func _build_icicles(cam: Camera3D) -> void:
	# A lean-to shed after a snowfall and three sunny days at -4 C: melt off its roof freezes at the eave.
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	# A low afternoon sun ahead of the camera, in front of the eave so the roof does not shade the ice:
	# backlit, the light comes through it.
	sun.look_at_from_position(Vector3.ZERO, Vector3(1.0, -0.16, -0.45))
	_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
	snow.height_source = NodePath("")
	var floor_body := StaticBody3D.new()
	var floor_shape := CollisionShape3D.new()
	floor_shape.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(floor_shape)
	add_child(floor_body)
	var wood := _ground_material(Color(0.42, 0.28, 0.16), 1.0)
	var shed := not OS.get_cmdline_user_args().has("--no-shed") # --no-shed: the icicles alone in the air
	if shed:
		_prop(BoxMesh.new(), Vector3(5.0, 0.12, 3.4), Vector3(-2.5, 2.6, -1.5), Vector3(10, 0, 0), wood)
	for p in ([Vector3(-4.8, 1.25, -0.1), Vector3(-0.2, 1.25, -0.1), Vector3(-4.8, 1.25, -2.9), Vector3(-0.2, 1.25, -2.9)] if shed else []):
		var post := MeshInstance3D.new()
		post.mesh = BoxMesh.new()
		post.scale = Vector3(0.15, 2.5, 0.15)
		post.position = p
		post.material_override = wood
		add_child(post)
	var hero := OS.get_cmdline_user_args().has("--hero")
	if hero and OS.get_cmdline_user_args().has("--wall"):
		# --hero --wall: the shed's front wall, set back under the eave as a house wall is: vertical boards 14 cm wide
		# with 1 cm gaps, each its own weathered tone, so the ice has a lit, lined ground to refract.
		var backing := MeshInstance3D.new()
		backing.mesh = BoxMesh.new()
		backing.scale = Vector3(4.6, 2.37, 0.02)
		backing.position = Vector3(-2.5, 1.185, -0.56)
		backing.material_override = _ground_material(Color(0.12, 0.09, 0.07), 1.0)
		add_child(backing)
		var tones: Array[StandardMaterial3D] = []
		for k in 5:
			var v := 0.85 + 0.07 * k
			tones.append(_ground_material(Color(0.5 * v, 0.41 * v, 0.31 * v), 1.0))
		var bx := -4.75
		var nb := 0
		while bx < -0.25:
			var board := MeshInstance3D.new()
			board.mesh = BoxMesh.new()
			board.scale = Vector3(0.14, 2.37, 0.022)
			board.position = Vector3(bx + 0.07, 1.185, -0.5)
			board.material_override = tones[(nb * 7 + nb / 3) % tones.size()]
			add_child(board)
			bx += 0.15
			nb += 1
	weather.rebake_shelter()
	await get_tree().physics_frame
	await get_tree().physics_frame
	snow.refresh_heights()
	_precipitate(10.0, 2.0)
	# The roof's front edge: 1.7 m out from its middle, dropped by its 10 degree slope, under its thickness.
	# Rooted a little inside the roof.
	var eave_y := 2.6 - 1.7 * sin(deg_to_rad(10.0)) - 0.03
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--ice-drop="): # --ice-drop=M: the edge M m below the roof (roots must still join it)
			eave_y -= float(a.substr(11))
	var ice := SnowIcicles3D.new()
	ice.name = "Icicles"
	ice.spacing = 0.12
	# --hero implies --dense: a week of melt at 5 cm sites, so there are long icicles and fused pairs to frame.
	var dense: bool = OS.get_cmdline_user_args().has("--dense") or OS.get_cmdline_user_args().has("--hero")
	if dense:
		ice.spacing = 0.05 # --dense: a dripping stretch, sites close enough that neighbours bridge into curtains
	if OS.get_cmdline_user_args().has("--no-surface") and surface != null:
		surface.visible = false # --no-surface: the deep-snow surface hidden (what the ice shows without it)
	if OS.get_cmdline_user_args().has("--plain-ice"):
		# --plain-ice: the ice's settings without its ripple normal map (what the map does to the trace)
		var pim := StandardMaterial3D.new()
		pim.albedo_color = Color(0.97, 0.99, 1.0)
		pim.roughness = 0.06
		pim.metallic_specular = 0.335
		pim.transmission_enabled = true
		pim.transmission_amount = 1.0
		pim.transmission_ior = 1.31
		for a in OS.get_cmdline_user_args():
			if a.begins_with("--ice-ior="):
				pim.transmission_ior = float(a.substr(10))
		pim.transmission_thickness = 0.04
		ice.material = pim
	if OS.get_cmdline_user_args().has("--hidden-ice"):
		# --hidden-ice: the clear ice fully transparent, the core alone (is the core there for the trace?)
		var hm := StandardMaterial3D.new()
		hm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		hm.alpha_scissor_threshold = 0.5
		hm.albedo_color = Color(1, 1, 1, 0)
		ice.material = hm
	if OS.get_cmdline_user_args().has("--no-curtains"):
		ice.curtains = false # --no-curtains: no webs between fused neighbours
	if OS.get_cmdline_user_args().has("--no-core"):
		ice.hazy_core = false # --no-core: clear ice only (checks what the core adds)
	if OS.get_cmdline_user_args().has("--black-core"):
		var black := StandardMaterial3D.new() # --black-core: shows where the trace and raster see the core
		black.albedo_color = Color(0, 0, 0)
		ice.core_material = black
	ice.edge = PackedVector3Array([Vector3(-4.95, eave_y, 0.12), Vector3(-0.05, eave_y, 0.12)])
	add_child(ice)
	# Three days: six hours of melt off the sunny roof (0.3 mm/h over its 3.4 m) at -4 C, then a night at -8 C.
	# --wind=S: a steady wind along the eave by day (it leans the tips; follow-up 37).
	var day_wind := 0.5
	if Array(OS.get_cmdline_user_args()).any(func(a): return a.begins_with("--wind=")):
		day_wind = wind
		ice.wind_direction = Vector3(-1, 0, 0)
	# --dense: a week of it (long, thick icicles, rippled).
	for day in (7 if dense else 3):
		ice.grow(6.0, -4.0, day_wind, 0.3 * 3.4)
		ice.grow(18.0, -8.0, 0.5, 0.0)
	# The last afternoon's melt still running: the tips drip.
	ice.grow(0.25, -4.0, day_wind, 0.3 * 3.4)
	var rmax := 0.0
	for k in ice.get_icicle_count():
		rmax = maxf(rmax, ice.get_icicle_radius(k))
	print("icicles: %d sites, longest %.2f m, thickest %.1f cm across, %.2f kg of ice" % [ice.get_icicle_count(), ice.get_longest(), rmax * 200.0, ice.get_ice_mass()])
	var n_long := 0
	var n_stub := 0
	var n_bare := 0
	for k in ice.get_icicle_count():
		var l := ice.get_icicle_length(k)
		if l >= 0.25:
			n_long += 1
		elif l <= 0.0:
			n_bare += 1
		elif l < 0.08:
			n_stub += 1
	print("icicles: %d long (25 cm or more), %d stubs (under 8 cm), %d bare" % [n_long, n_stub, n_bare])
	# At the icicles' height along the eave, the sky and the field behind them.
	cam.position = Vector3(-0.2, 2.05, 1.1)
	cam.look_at(Vector3(-2.2, 2.1, 0.17))
	cam.fov = 50.0
	if hero:
		# From under the roof, 1.7 m back from the eave and a little above the icicles' middle, looking out
		# at the snow: a run of them root to tip, backlit by the low sun, the longest fused pair (neighbours
		# bridged at the root) in the middle. Shoot with --resolution 1920x1080. (--wall: from outside, head on,
		# against the shed's boarded front.)
		var best := -1
		var best_len := 0.0
		for k in ice.get_icicle_count() - 1:
			var a := ice.get_icicle_position(k)
			var b := ice.get_icicle_position(k + 1)
			var ra := ice.get_icicle_radius(k)
			var rb := ice.get_icicle_radius(k + 1)
			var l := minf(ice.get_icicle_length(k), ice.get_icicle_length(k + 1))
			var gap := Vector2(a.x - b.x, a.z - b.z).length() - ra - rb
			if gap > 0.0 and gap < minf(ra, rb) and a.x > -3.6 and a.x < -1.4 and l > best_len:
				best = k
				best_len = l
		var mid := Vector3(-2.5, eave_y - 0.25, 0.12)
		if best >= 0:
			mid = (ice.get_icicle_position(best) + ice.get_icicle_position(best + 1)) * 0.5
			mid.y -= 0.5 * maxf(ice.get_icicle_length(best), ice.get_icicle_length(best + 1))
			print("hero pair %d/%d at x %.2f, %.2f m" % [best, best + 1, mid.x, best_len])
		if OS.get_cmdline_user_args().has("--wall"):
			cam.position = mid + Vector3(0.45, -0.08, 1.62)
		else:
			# From outside the eave by default; --under: from under the roof (the path tracer misreads a camera
			# under a thin roof as inside a medium until rt_camera_medium is fixed, so raster only for now).
			cam.position = mid + Vector3(-0.4, 0.4, 1.55)
			if OS.get_cmdline_user_args().has("--under"):
				cam.position = mid + Vector3(-0.4, 0.4, -1.55) # under the roof, 2.45 m up
			for a in OS.get_cmdline_user_args():
				if a.begins_with("--cam-offset="): # --cam-offset=x,y,z: the camera from the hero pair (tests)
					var c := a.substr(13).split_floats(",")
					cam.position = mid + Vector3(c[0], c[1], c[2])
		cam.look_at(mid)
		cam.fov = 26.0
	elif OS.get_cmdline_user_args().has("--close"):
		# The longest icicles at 100%: the ripples, the clear sheath and the hazy core.
		cam.position = Vector3(-0.1, 2.1, 0.75)
		cam.look_at(Vector3(-0.6, 2.12, 0.17))
		cam.fov = 30.0
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


# A frozen lake at the mountain's foot from its shore: two weeks of frost, then a light snow on the ice.
func _build_lake(cam: Camera3D) -> void:
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-18.0, 120.0, 0.0)
	_add_terrain(512.0, 256, Color(0.33, 0.31, 0.27), 64.0)
	var level: float = ground.get_height(Vector3(200.0, 0, 0))
	_lake(level)
	# The open water under the ice (dark, smooth), for where the ice is thin or gone.
	var water := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(512.0, 512.0)
	water.mesh = pm
	var wm := StandardMaterial3D.new()
	wm.albedo_color = Color(0.02, 0.04, 0.05)
	wm.roughness = 0.04
	water.material_override = wm
	water.position = Vector3(0, level, 0)
	add_child(water)
	await get_tree().physics_frame
	snow.refresh_heights()
	snow.advance(14.0 * 24.0 * 3600.0)
	_precipitate(snow_hours if snow_hours >= 0.0 else 1.5, 1.0)
	snow.get_lake_ice().rebuild()
	# Stand on the shore looking out along it.
	var x := 150.0
	while x < 250.0 and ground.get_height(Vector3(x, 0, 0)) > level + 0.8:
		x += 1.0
	var at := Vector3(x - 6.0, 0, 0)
	cam.position = at + Vector3(0, ground.get_height(at) + 1.7, 0)
	cam.look_at(Vector3(x + 40.0, level, -45.0))
	cam.fov = 55.0
	cam.far = 2000.0
	var p := Vector3(x + 30.0, 0, 0)
	print("lake: level %.2f m, ice %.1f cm (%.1f cm white), snow %.1f mm, %d cells drawn" % [level, snow.get_ice_thickness_at(p) * 100.0, snow.get_white_ice_thickness_at(p) * 100.0, snow.get_swe_at(p), snow.get_lake_ice().get_quad_count()])
	await _save_frame("")


func _build_avalanche(cam: Camera3D) -> void:
	# A metre of new snow on a 38 degree face: it releases, runs out over the flat and piles at the toe. --before
	# shoots the loaded face before it goes.
	snow.drift_enabled = false
	snow.slide_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-30.0, -60.0, 0.0)
	_add_terrain(snow.extent, int(snow.extent / 2.0), Color(0.33, 0.31, 0.27), 16.0)
	# The deposit near the camera as geometry (the deep-snow surface), its blocks as clumps.
	var probe := Marker3D.new()
	probe.name = "Probe"
	probe.position = Vector3(15.0, 0.0, 0.0)
	add_child(probe)
	surface = SnowSurface3D.new()
	surface.snow_cover = NodePath("../SnowCover3D")
	surface.follow_target = NodePath("../Probe")
	surface.extent = 200.0 # the release and the whole run-out as deep-snow geometry
	surface.deform_extent = 96.0 # and the debris granules over the whole deposit (they live in the print field)
	add_child(surface)
	await get_tree().physics_frame
	var av: SnowAvalanche3D = snow.get_avalanches()
	var wet := OS.get_cmdline_user_args().has("--wet")
	var triggered := wet or OS.get_cmdline_user_args().has("--triggered")
	var powder := OS.get_cmdline_user_args().has("--powder")
	if OS.get_cmdline_user_args().has("--before") or triggered or powder:
		av.natural_release = false
	if triggered or powder:
		# An old, bonded pack under the storm (60 cm a week and more old): the slab breaks away from it, leaving a
		# crown step and a white bed, as slabs do.
		snow.add_snow(90.0, 150.0)
		snow.advance(8.0 * 24.0 * 3600.0)
		await get_tree().physics_frame
	snow.add_snow(30.0, 100.0)
	await get_tree().physics_frame
	snow.add_snow(90.0, 90.0)
	await get_tree().physics_frame
	await get_tree().physics_frame
	if wet:
		# --wet: a spring thaw (12 h at +5 C) soaks the storm snow, then the slab goes as a wet slide.
		await _thaw(5.0, 12.0)
	if triggered:
		# --triggered (dry) and --wet release the same face, so the two compare.
		av.trigger(Vector3(-40.0, 0, 0), 30.0) # inside the face: the crown breaks across the slope, below the ridge
		print("triggered slide: %.0f%% wet" % (av.get_flow_wetness() * 100.0))
	if powder:
		# A wide dry slab across the face in front of the camera (left to itself the face lets go wherever it is
		# weakest, here off to both sides of the frame, and its cloud with it).
		# One physics tick a frame: Flow steps once a frame, so on a slow renderer the gas keeps pace with the slide
		# (with several ticks a frame it falls behind and the cloud is only the emitters' trail).
		Engine.max_physics_steps_per_frame = 1
		if OS.get_cmdline_user_args().has("--no-gas"):
			# --no-gas: the snow alone (the slide, its bed and crown), quick: no Flow to keep pace with.
			av.get_powder_cloud().enabled = false
			Engine.max_physics_steps_per_frame = 32
		var pflow: Node = av.get_powder_cloud().get_flow()
		if pflow:
			pflow.set("scattering_resolution", 0) # full: quarter averages away the billows at still sizes
			for a in OS.get_cmdline_user_args():
				if a.begins_with("--flow-set="):
					# --flow-set=prop=value[;prop=value]: try PhysXFlow3D settings on the powder cloud.
					for kv in a.substr(11).split(";"):
						var pv := kv.split("=")
						pflow.set(pv[0], str_to_var(pv[1]))
		av.trigger(Vector3(-40.0, 0, 0), 60.0)
		surface.extent = 320.0 # the deep-snow geometry over the whole face the camera sees, out to the camera
		# Mid-run in real time, so PhysX Flow steps the cloud with it: --at=S seconds after release (default 9).
		var at_s := 9.0
		for a in OS.get_cmdline_user_args():
			if a.begins_with("--at="):
				at_s = float(a.substr(5))
		# Stepped under raster (path tracing every frame would take hours here), then frozen for the path-traced still.
		var wenv: Environment = get_viewport().find_world_3d().environment
		if wenv == null:
			for c in get_children():
				if c is WorldEnvironment:
					wenv = c.environment
		var traced := wenv != null and wenv.pathtracing_enabled
		if traced:
			wenv.pathtracing_enabled = false
		var frames := 0
		while av.is_running() and frames < int(at_s * Engine.physics_ticks_per_second):
			await get_tree().physics_frame
			frames += 1
		var cloud := av.get_powder_cloud()
		print("powder: %.1f t up, %.1f t in the air, Flow %s, %d sources, front %.1f m/s" % [av.get_powder_taken() / 1000.0, av.get_suspended_mass() / 1000.0, cloud.has_flow(), cloud.get_active_sources(), av.get_max_speed()])
		# Back and up far enough to hold the whole front (--head-on: from straight down the track, flank to flank).
		if OS.get_cmdline_user_args().has("--head-on"):
			cam.position = Vector3(92.0, snow.get_ground_height_at(Vector3(92, 0, 12)) + 15.0, 12.0)
			cam.look_at(Vector3(-15.0, 12.0, 0.0))
		else:
			# Three-quarters from the side, so the head's height and the cloud trailing back up the track both show.
			cam.position = Vector3(100.0, snow.get_ground_height_at(Vector3(100, 0, 115)) + 28.0, 115.0)
			cam.look_at(Vector3(-25.0, 16.0, -10.0))
		cam.fov = 66.0
		cam.far = 1000.0
		sun.directional_shadow_max_distance = 400.0
		# Shot with the sun over the camera's right shoulder (the slide ran under (-30, -60), behind the face): from
		# behind the face, the bowl's rim past the edge of the bench terrain shades the run-out in one renderer and
		# not the other. Light on the camera's side keeps every shadow inside the scene.
		sun.rotation_degrees = Vector3(-40.0, 40.0, 0.0)
		if OS.get_cmdline_user_args().has("--crown"):
			_crown_camera(cam)
		if traced:
			# The raster frame of the same cloud first (_raster.png beside --out): Flow is not bit-for-bit the
			# same from run to run, so a raster run and a traced run do not show one cloud.
			await _save_frame("_raster")
			process_mode = Node.PROCESS_MODE_ALWAYS
			get_tree().paused = true
			wenv.pathtracing_enabled = true
		await _save_frame("")
		return
	var t := 0.0
	while av.is_running() and t < 200.0:
		av.step(0.25)
		t += 0.25
	print("avalanche: %d released, %.0f m3, run-out %.0f m, %.1f m/s, toe %.2f m" % [av.get_release_count(), av.get_released_volume(), av.get_run_out(), av.get_max_speed(), snow.get_depth_at(Vector3(40, 0, 0))])
	# Shot with the sun 55 degrees up from behind the ridge (the slide ran under 30 degrees, which melts the same
	# but left the whole 38 degree face in shade): it rakes the face, the crown wall stands in its own shade and
	# throws a band of shadow down the bed.
	sun.rotation_degrees = Vector3(-55.0, -60.0, 0.0)
	cam.position = Vector3(42.0, snow.get_ground_height_at(Vector3(42, 0, 9)) + snow.get_depth_at(Vector3(42, 0, 9)) + 2.2, 9.0)
	cam.look_at(Vector3(-25.0, 18.0, 0.0))
	cam.fov = 55.0
	if OS.get_cmdline_user_args().has("--close"):
		# --close: the debris at the toe from 20 m.
		cam.position = Vector3(22.0, snow.get_ground_height_at(Vector3(22, 0, 14)) + snow.get_depth_at(Vector3(22, 0, 14)) + 3.0, 14.0)
		cam.look_at(Vector3(2.0, 1.0, 0.0))
	if OS.get_cmdline_user_args().has("--crown"):
		_crown_camera(cam)
	if OS.get_cmdline_user_args().has("--toe"):
		# --toe: the deposit's tongue from above and in front, to see its outline.
		cam.position = Vector3(45.0, 22.0, 30.0)
		cam.look_at(Vector3(5.0, 0.0, 0.0))
		cam.fov = 50.0
	cam.far = 1000.0
	await _save_frame("")


func _crown_camera(cam: Camera3D) -> void:
	# --crown: the crown wall from 14 m down the bed (found along z = 0 as the biggest drop in depth).
	var best := 0.0
	var cx := -60.0
	for i in 120:
		var x := -100.0 + i * 0.5
		var drop := snow.get_depth_at(Vector3(x, 0, 0)) - snow.get_depth_at(Vector3(x + 0.5, 0, 0))
		if drop > best:
			best = drop
			cx = x
	# --crown-far: from 40 m off, three-quarters from the side and above, as the wide shots see it.
	var far := OS.get_cmdline_user_args().has("--crown-far")
	var gx := cx + (30.0 if far else 14.0)
	var gz := 28.0 if far else 6.0
	cam.position = Vector3(gx, snow.get_ground_height_at(Vector3(gx, 0, gz)) + snow.get_depth_at(Vector3(gx, 0, gz)) + (10.0 if far else 3.0), gz)
	cam.look_at(Vector3(cx, snow.get_ground_height_at(Vector3(cx, 0, 0)) + 0.5, -2.0))
	print("crown at x %.1f, %.2f m step" % [cx, best])


func _build_bodies(cam: Camera3D) -> void:
	# Three crates after six hours of snow: one left out (it carries the ground's load and the fall on it), one
	# just driven in on a trailer (the wind of its speed and the bumps took most of it), one from a shed (bare).
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-35.0, -40.0, 0.0)
	_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
	_precipitate(6.0, 2.0)
	var crates: Array[RigidBody3D] = []
	for i in 3:
		var b := _body_box(Vector3(-1.6 + 1.6 * i, ground.get_height(Vector3(-1.6 + 1.6 * i, 0, 0)) + 0.5, 0))
		var m := (b.get_node("Mesh") as MeshInstance3D).mesh.material as StandardMaterial3D
		m.albedo_color = Color(0.42, 0.3, 0.18)
		m.roughness = 0.8
		crates.append(b)
	await get_tree().physics_frame
	await get_tree().physics_frame
	var sb := snow.get_bodies()
	sb.update_rate = 0.1
	sb.step(0.1)
	sb.set_body_depth(crates[1], 0.006)
	sb.set_body_depth(crates[2], 0.0)
	sb.step(0.1)
	print("bodies: %.3f m, %.3f m, %.3f m on the crates (ground %.3f m)" % [sb.get_body_depth(crates[0]), sb.get_body_depth(crates[1]), sb.get_body_depth(crates[2]), snow.get_depth_at(Vector3())])
	cam.position = Vector3(0.6, ground.get_height(Vector3(0, 0, 3.4)) + 3.4, 3.4)
	cam.look_at(Vector3(0, ground.get_height(Vector3()) + 0.6, 0))
	cam.fov = 50.0
	await _save_frame("")


func _build_rime(cam: Camera3D) -> void:
	# A fence on a hill in freezing fog: posts, rails, a wire and a bare shrub after a day of 8 m/s wind at -6 C
	# through 0.25 g/m3 of cloud water. Rime feathers point into the wind, thickest on the wire, the twigs and the
	# rails' windward edges.
	snow.drift_enabled = false
	sun.visible = true
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-25.0, 140.0, 0.0)
	_add_terrain(32.0, 128, Color(0.36, 0.33, 0.28), 2.0)
	var wood := _ground_material(Color(0.36, 0.27, 0.19), 1.0)
	var metal := StandardMaterial3D.new()
	metal.albedo_color = Color(0.3, 0.3, 0.32)
	metal.metallic = 0.8
	metal.roughness = 0.5
	var fence := Node3D.new()
	fence.name = "Fence"
	fence.add_to_group("rime")
	add_child(fence)
	for k in 3:
		var post := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(0.08, 1.3, 0.08)
		bm.subdivide_height = 12
		post.mesh = bm
		post.position = Vector3(0.0, 0.65, -1.6 + 1.6 * k)
		post.material_override = wood
		fence.add_child(post)
	for y in [0.55, 1.05]:
		var rail := MeshInstance3D.new()
		var rm := BoxMesh.new()
		rm.size = Vector3(0.03, 0.08, 3.4)
		rm.subdivide_depth = 40
		rail.mesh = rm
		rail.position = Vector3(-0.055, y, 0.0)
		rail.material_override = wood
		fence.add_child(rail)
	var wire := MeshInstance3D.new()
	var wm := CylinderMesh.new()
	wm.top_radius = 0.0025
	wm.bottom_radius = 0.0025
	wm.height = 3.4
	wm.rings = 60
	wm.radial_segments = 8
	wire.mesh = wm
	wire.rotation_degrees = Vector3(90.0, 0.0, 0.0)
	wire.position = Vector3(-0.045, 1.25, 0.0)
	wire.material_override = metal
	fence.add_child(wire)
	# A bare shrub: twigs fanning up from a root, 4 to 8 mm thick.
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for k in 14:
		var twig := MeshInstance3D.new()
		var tm := CylinderMesh.new()
		var r := rng.randf_range(0.002, 0.004)
		tm.top_radius = r * 0.5
		tm.bottom_radius = r
		tm.height = rng.randf_range(0.5, 0.9)
		tm.rings = 10
		tm.radial_segments = 6
		twig.mesh = tm
		var tilt := rng.randf_range(10.0, 45.0)
		var turn := rng.randf_range(0.0, 360.0)
		twig.rotation_degrees = Vector3(tilt, turn, 0.0)
		twig.position = Vector3(0.9, 0.0, 0.8) + twig.transform.basis.y * tm.height * 0.5
		twig.material_override = wood
		fence.add_child(twig)
	var rime := SnowRime3D.new()
	rime.name = "Rime"
	add_child(rime)
	await get_tree().physics_frame
	rime.refresh_targets()
	var hours := 24
	for h in hours:
		rime.grow(1.0, -6.0, Vector3(8.0, 0.0, 0.0), 0.25)
	print("rime: %d parts, %d feathers, density %.0f kg/m3, wire %.1f cm, post %.1f cm, %.1f mm/h at the most" % [rime.get_target_count(), rime.get_feather_count(), rime.get_last_density(), rime.get_thickness_at(Vector3(-0.05, 1.25, 0.4)) * 100.0, rime.get_thickness_at(Vector3(-0.04, 0.3, 0.0)) * 100.0, rime.get_last_rate() * 1000.0])
	# From upwind and to the side: the feathers stand out toward the camera, into the wind.
	cam.position = Vector3(-1.7, 1.35, 1.5)
	cam.look_at(Vector3(0.1, 0.85, 0.3))
	cam.fov = 40.0
	if OS.get_cmdline_user_args().has("--close"):
		cam.position = Vector3(-0.55, 1.2, 0.6)
		cam.look_at(Vector3(-0.06, 1.12, 0.05))
		cam.fov = 30.0
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
	var mirror_rect := Rect2i()
	if OS.get_cmdline_user_args().has("--mirror"):
		# An overhead mirror tilted down and forward: it shows the snow ahead from above, toward the low
		# sun, so its reflection should glint like the snow seen directly (path traced: reflected rays).
		cam.look_at(Vector3(0.0, 0.9, -3.0))
		var mirror := MeshInstance3D.new()
		var q := QuadMesh.new()
		q.size = Vector2(3.0, 1.6)
		mirror.mesh = q
		var mm := StandardMaterial3D.new()
		mm.metallic = 1.0
		mm.roughness = 0.0
		mm.albedo_color = Color(0.95, 0.95, 0.95)
		mirror.material_override = mm
		add_child(mirror)
		var n := Vector3(0.0, -0.97, 0.24).normalized()
		mirror.look_at_from_position(Vector3(0.0, 2.6, 0.0), Vector3(0.0, 2.6, 0.0) - n)
		# Its rectangle on screen, for counting glints inside it.
		var pts := []
		for c in [Vector2(-1.5, -0.8), Vector2(1.5, -0.8), Vector2(-1.5, 0.8), Vector2(1.5, 0.8)]:
			pts.append(cam.unproject_position(mirror.global_transform * Vector3(c.x, c.y, 0.0)))
		var lo := Vector2(1e9, 1e9)
		var hi := Vector2(-1e9, -1e9)
		for pt: Vector2 in pts:
			lo = lo.min(pt)
			hi = hi.max(pt)
		mirror_rect = Rect2i(Vector2i(lo) + Vector2i(8, 8), Vector2i(hi - lo) - Vector2i(16, 16))
		if OS.get_cmdline_user_args().has("--close"):
			# Up at the mirror, the reflected snow filling the frame.
			q.size = Vector2(5.0, 3.0)
			cam.position = Vector3(0.0, 1.4, 2.6)
			cam.look_at(Vector3(0.0, 2.6, 0.0))
			cam.fov = 45.0
			mirror_rect = Rect2i(0, 0, int(get_viewport().get_visible_rect().size.x), int(get_viewport().get_visible_rect().size.y)).grow(-8)
	await get_tree().physics_frame
	snow.refresh_heights()
	_precipitate(8.0, 2.0)
	if melt_hours > 0.0:
		weather.temperature = 3.0
		weather.snap_to_settings()
		weather.advance(1.0)
		for a in OS.get_cmdline_user_args():
			if a.begins_with("--sun-pitch="): # the melt under this sun (suncups need a high one)
				sun.rotation_degrees.x = -float(a.substr(12))
		snow.advance(melt_hours * 3600.0)
	if hoar_mm > 0.0:
		# A clear calm night's surface hoar over the field (SnowCover3D.hoar_enabled grows it; set here so the
		# look is tested alone): the crystals' upright plates flash in the low sun.
		snow.add_hoar(hoar_mm)
		snow.advance(1.0)
	var p := Vector3(0, 0, 0)
	print("sparkle: hoar %.1f mm, suncups %.3f m" % [snow.get_hoar_at(p), snow.get_suncup_relief_at(p)])
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
	var mean_lum := 0.0
	for y in range(h / 2, h - 2):
		for x in range(2, w - 2):
			var ring := 0.0
			for d in [Vector2i(-2, 0), Vector2i(2, 0), Vector2i(0, -2), Vector2i(0, 2), Vector2i(-2, -2), Vector2i(2, 2), Vector2i(-2, 2), Vector2i(2, -2)]:
				ring += lum[(y + d.y) * w + x + d.x]
			total += 1
			mean_lum += lum[y * w + x]
			if lum[y * w + x] > ring / 8.0 * 1.2:
				count += 1
	print("sparkle: glinting %.3f%% of %d pixels, mean luminance %.4f" % [100.0 * count / total, total, mean_lum / total])
	if mirror_rect.size.x > 0:
		var mc := 0
		var mt := 0
		for y in range(maxi(mirror_rect.position.y, 2), mini(mirror_rect.end.y, h - 2)):
			for x in range(maxi(mirror_rect.position.x, 2), mini(mirror_rect.end.x, w - 2)):
				var ring := 0.0
				for d in [Vector2i(-2, 0), Vector2i(2, 0), Vector2i(0, -2), Vector2i(0, 2), Vector2i(-2, -2), Vector2i(2, 2), Vector2i(-2, 2), Vector2i(2, -2)]:
					ring += lum[(y + d.y) * w + x + d.x]
				mt += 1
				if lum[y * w + x] > ring / 8.0 * 1.2:
					mc += 1
		print("sparkle: in the mirror %s, glinting %.3f%% of %d pixels" % [str(mirror_rect), 100.0 * mc / maxi(mt, 1), mt])
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
	if air < INF:
		weather.temperature = air
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
	# The snow past the particle box is haze: SnowFall3D pushes its falling and blowing extinction to the
	# weather map, which drives the environment's fog and a blowing-snow fog volume.
	await get_tree().process_frame
	var frames := steps if steps > 0 else 1
	for s in frames:
		for i in 150 / frames + 1:
			await get_tree().process_frame
		if steps > 0:
			await _save_frame("_%03d" % s)
	print("visibility %.0f m in the falling snow; blowing snow %.4f /m at 1 m (%.0f m), layer %.2f m deep; haze at 1 m %.4f /m" % [fall.get_visibility(), fall.get_blowing_extinction(), 3.912 / maxf(fall.get_blowing_extinction(), 1e-6), fall.get_blowing_height_scale(), weather.get_precipitation_extinction_at(Vector3(0, weather.get_ground_height() + 1.0, 0))])
	print("sleet: air %.1f C, melted share %.2f, aggregates x%.2f" % [snow.get_air_temperature_at(Vector3(0, 1, 0)), fall.get_melted_share(), fall.get_aggregate()])
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
	var sled: Node3D = get_node_or_null("Sled")
	if sled:
		# Pulled for 2 s, then lifted away so the berm it pushed is seen.
		if walk_t > 2.0:
			sled.remove_from_group("snow_deform")
			sled.visible = false
		var sp := Vector3(-10.0 + 4.0 * minf(walk_t, 2.0), 0, 3.4)
		sled.global_position = Vector3(sp.x, _undisturbed(sp) - 0.15 + 0.1, sp.z)


func _save_frame(suffix: String) -> void:
	# Light probes for raster and path traced comparisons: --no-sun-shadow, --no-sun (the sky alone), --no-sun-specular,
	# --sun-pitch=D (the sun's elevation), --plain-ground [--white-lambert], --sun-only.
	if sun != null and OS.get_cmdline_user_args().has("--no-sun-shadow"):
		sun.shadow_enabled = false
	if sun != null and OS.get_cmdline_user_args().has("--no-sun"):
		sun.visible = false
	if sun != null and OS.get_cmdline_user_args().has("--no-sun-specular"):
		sun.light_specular = 0.0
	if OS.get_cmdline_user_args().has("--sun-only"):
		# The sun alone, untonemapped: a black sky with no ambient (linear values to compare).
		var wenv: Environment = get_viewport().find_world_3d().environment
		wenv.background_mode = Environment.BG_COLOR
		wenv.background_color = Color(0, 0, 0)
		wenv.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
		wenv.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
		wenv.tonemap_mode = Environment.TONE_MAPPER_LINEAR
		wenv.set("pathtracing_sky_shader_enabled", false)
		wenv.sky = null
	if OS.get_cmdline_user_args().has("--plain-ground"):
		# A plain StandardMaterial3D plane with the snow's albedo, roughness and specular over the view: is a gap
		# the snow material's or the renderer's?
		var plane := MeshInstance3D.new()
		var pm := PlaneMesh.new()
		pm.size = Vector2(400.0, 400.0)
		plane.mesh = pm
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(0.83, 0.83, 0.83)
		mat.roughness = 0.7
		mat.metallic_specular = 0.335
		if OS.get_cmdline_user_args().has("--white-lambert"):
			mat.albedo_color = Color(1, 1, 1)
			mat.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
			mat.metallic_specular = 0.0
		plane.material_override = mat
		plane.position.y = 0.5
		add_child(plane)
		for i in 4:
			await get_tree().process_frame
	for a in OS.get_cmdline_user_args():
		if sun != null and a.begins_with("--sun-pitch="):
			sun.rotation_degrees.x = -float(a.substr(12)) # the sun's elevation in degrees
	for i in (40 if get_viewport().use_taa else 12): # TAA converges over some frames
		await get_tree().process_frame
	# Path traced: wait until every displaced surface has been captured for the trace (a big deep-snow mesh
	# takes several frames), or the still shows it flat.
	var waited := 0
	while not raster and waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		await get_tree().process_frame
		waited += 1
	if waited > 0:
		print("waited %d frames for the trace's surfaces" % waited)
	# --time=N: the viewport's mean GPU time over N frames (timestamp queries), for cost comparisons.
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--time="):
			var vp := get_viewport().get_viewport_rid()
			RenderingServer.viewport_set_measure_render_time(vp, true)
			for i in 3:
				await RenderingServer.frame_post_draw
			var gpu := 0.0
			var n := int(a.substr(7))
			for i in n:
				await RenderingServer.frame_post_draw
				gpu += RenderingServer.viewport_get_measured_render_time_gpu(vp)
			print("gpu time: %.2f ms a frame over %d frames" % [gpu / maxf(n, 1), n])
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


func _add_terrain(extent: float, cells: int, color: Color, tile: float, collide := false) -> void:
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
	if collide:
		# For bodies that fall on it (the blocks of a failed cornice).
		var body := StaticBody3D.new()
		var shape := CollisionShape3D.new()
		shape.shape = mi.mesh.create_trimesh_shape()
		body.add_child(shape)
		add_child(body)


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

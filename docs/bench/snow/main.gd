extends Node3D
# SnowCover3D bench: checks the snowpack against the published relations, and renders snow on a mountain
# or on props, raster or path traced.
#
#   --test            print the checks and quit:
#                       snowfall share against Jennings et al. (2018) and its 50% temperature by humidity
#                       new snow density against Hedstrom and Pomeroy (1998)
#                       a snowfall: water kept, depth, settling toward the CLASS maximum
#                       melt by temperature (Pellicciotti et al. 2005), water kept as liquid, runoff
#                       melt by sunlight on a south face against a north face
#                       the snow line on a mountain against the lapse rate
#                       SnowSlide on a 60 degree face: what it holds, mass kept
#                       drift over a ridge: lee against windward, mass kept
#                       the cost of one update
#   --view=mountain   a 700 m mountain after a day of snow and two days of sun (snow line, south faces melted)
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


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a.begins_with("--test"): test = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--snow="): snow_hours = float(a.substr(7))
		elif a.begins_with("--sun="): sun_hours = float(a.substr(6))
		elif a.begins_with("--steps="): steps = int(a.substr(8))
	if test:
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

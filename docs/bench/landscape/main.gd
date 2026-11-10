extends Node3D
# Landscape integration bench: one generated world from mountain to sea, every system on it at once, driven by one
# weather state. TerrainGenerator3D makes the mountains, rivers, coast, seabed and lakes and builds their water
# (PhysXWaterSurface3D sea with shore waves, one surface per lake, PhysXRiver3D rivers ending on them); a waterfall
# ledge is cut by hand on a tributary. WeatherMap3D storm with WeatherSkyMaterial clouds, wetness, SnowCover3D and
# SnowFall3D on the peaks, VegetationField3D forest (stand-in meshes) on slope, water and geology rules, GrassField3D
# near the camera. Each link between systems is checked: PASS, FAIL, or GAP for a missing or hand-made join.
#
# godot --path docs/bench/landscape --rendering-driver vulkan -- [options]
#   --test            run the link checks, print PASS/FAIL/GAP, quit
#   --out=path.png    render a view (path traced unless --raster)
#   --raster          raster only
#   --spp=N           path tracing samples (default 16)
#   --view=wide|lake|fall|beach|peak   camera (default wide)
#   --strike[=1] [--strike-x= --strike-z=]   force a lightning strike just before the frame is saved
#   --probe           print coast profiles, lakes and rivers from the generator
#   --weather=storm|clear         weather state (default storm)

const STEP := 4.0 # metres between ground vertices
const X0 := -2048.0
const Z0 := -2048.0
const NX := 1025 # x from -2048 to 2048
const NZ := 1281 # z from -2048 to 3072 (the last 1 km is sea)
const GEN_SIZE := 4096.0

var args := {}
var gen: TerrainGenerator3D
var heights := PackedFloat32Array()
var ground_body: StaticBody3D
var ground_mesh: MeshInstance3D
var ground_mat: StandardMaterial3D
var ocean: PhysXWaterSurface3D
var lake: PhysXWaterSurface3D
var lakes: Array = []
var water_nodes: Array = []
var lake_center := Vector3.ZERO
var lake_axes := Vector2(140, 90)
var lake_dir := Vector2(0, 1)
var lake_level := 0.0
var fall_point := Vector3.ZERO
var fall_dir := Vector2(0, 1)
var rivers: Array = []
var weather: WeatherMap3D
var sky_mat: WeatherSkyMaterial
var snow: SnowCover3D
var snowfall: SnowFall3D
var cornices: SnowCornice3D
var lightning: WeatherLightning3D
var probe: Node3D
var data_map: TerrainDataMap3D
var veg: VegetationField3D
var grass: GrassField3D
var sun: DirectionalLight3D
var cam: Camera3D
var env: Environment
var results: Array = []
var t_start := 0


class Ground extends Node3D:
	# Height source for the data map and the snowpack: bilinear on the bench's grid.
	var bench
	func get_height(p: Vector3) -> float:
		return bench.height_at(p.x, p.z)
	func base_height_at(p: Vector2) -> float:
		return bench.height_at(p.x, p.y)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	t_start = Time.get_ticks_msec()
	_generate()
	_lake_and_fall()
	_build_ground()
	await get_tree().physics_frame
	await get_tree().physics_frame
	_build_weather()
	_weather_settings()
	_build_water()
	await get_tree().physics_frame
	await get_tree().physics_frame
	for r in rivers:
		r.rebuild()
	_build_vegetation()
	_build_camera()
	_run_weather()
	_log("setup done in %d ms" % (Time.get_ticks_msec() - t_start))
	for i in 30:
		await get_tree().process_frame
	await _checks()
	if args.has("test"):
		get_tree().quit(0)
		return
	var frames := 6 if args.has("raster") else int(args.get("frames", "40"))
	for i in frames:
		await get_tree().process_frame
	if env.pathtracing_enabled:
		var w := 0
		var can_ask := RenderingServer.has_method("is_pathtracing_ready")
		while can_ask and w < 400 and not RenderingServer.call("is_pathtracing_ready"):
			await get_tree().process_frame
			w += 1
		for i in int(args.get("spp-frames", "4")):
			await get_tree().process_frame
	if args.has("strike"):
		lightning.strike(Vector3(float(args.get("strike-x", "-300")), 0, float(args.get("strike-z", "-400"))), true)
		for i in int(args.get("strike-frames", "2")):
			await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var out: String = args.get("out", "")
	if out != "":
		get_viewport().get_texture().get_image().save_png(out)
		_log("saved " + out)
	get_tree().quit(0)


func _log(s: String) -> void:
	print("[landscape] ", s)


func check(ok: bool, what: String, gap := false) -> void:
	var tag := "PASS" if ok else ("GAP " if gap else "FAIL")
	results.append([tag, what])
	print("[landscape] ", tag, "  ", what)


# --- Terrain -----------------------------------------------------------------------------------------------------

func _generate() -> void:
	# A range across the north with a spur, uplift fading out towards a coastal plain in the south; water leaves only
	# by the south edge, so every river runs to the sea.
	var path := Path3D.new()
	path.name = "Range"
	var curve := Curve3D.new()
	curve.add_point(Vector3(-2000, 0, -900), Vector3.ZERO, Vector3(600, 0, -300))
	curve.add_point(Vector3(-200, 0, -1300), Vector3(-500, 0, 0), Vector3(500, 0, 0))
	curve.add_point(Vector3(2000, 0, -1000), Vector3(-600, 0, -200))
	path.curve = curve
	add_child(path)
	var spur := Path3D.new()
	spur.name = "Spur"
	var c2 := Curve3D.new()
	c2.add_point(Vector3(700, 0, -1200))
	c2.add_point(Vector3(1100, 0, 200))
	spur.curve = c2
	spur.set_meta("width", 450.0)
	spur.set_meta("uplift", 0.6)
	add_child(spur)
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 1.0 / 1500.0
	noise.fractal_octaves = 3
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	for y in 64:
		var z := Z0 + (y + 0.5) / 64.0 * GEN_SIZE
		var s := 0.15 + 0.85 * (1.0 - smoothstep(-400.0, 1100.0, z))
		for x in 64:
			up.set_pixel(x, y, Color(s, 0, 0))
	gen = TerrainGenerator3D.new()
	gen.name = "Generator"
	gen.size = Vector2(GEN_SIZE, GEN_SIZE)
	gen.cell_size = float(args.get("cell", "16"))
	gen.seed = 3
	gen.uplift = float(args.get("uplift", "3.0"))
	gen.base_uplift = 0.1
	gen.uplift_map = ImageTexture.create_from_image(up)
	gen.uplift_noise = noise
	# The sea along the south edge is the only outlet; the generator shapes the coast, beach and seabed.
	gen.outlet_edges = 0
	gen.ocean_edges = TerrainGenerator3D.OUTLET_POSITIVE_Z
	gen.coast_distance = 600.0
	# Lakes: automatic dammed valley lakes.
	gen.lake_count = int(args.get("lakes", "3"))
	gen.tile_erosion_enabled = args.has("tiles")
	add_child(gen)
	gen.ranges = [gen.get_path_to(path), gen.get_path_to(spur)]
	var t := Time.get_ticks_msec()
	gen.generate()
	_log("generate %d ms, grid %s, last change %.3f m" % [Time.get_ticks_msec() - t, gen.get_grid_size(), gen.get_last_change()])
	t = Time.get_ticks_msec()
	var img := gen.sample_heights(Vector2(X0, Z0), STEP, Vector2i(NX, NZ))
	heights = img.get_data().to_float32_array()
	var hmax := -INF
	for i in heights.size():
		if is_nan(heights[i]):
			heights[i] = -gen.max_sea_depth # past the generated rectangle: open sea floor
		hmax = maxf(hmax, heights[i])
	_log("water %d ms: sea %s, %d lakes" % [gen.get_last_water_msec(), gen.has_sea_result(), gen.get_lake_result_count()])
	for l in gen.get_lake_results():
		_log("  lake level %.1f m, depth %.1f m, area %.1f ha, outlet %s" % [l["level"], l["depth"], l["area"] / 10000.0, l.has("outlet")])
	_log("sample_heights %d ms, highest %.0f m" % [Time.get_ticks_msec() - t, hmax])


func height_at(x: float, z: float) -> float:
	var fx := clampf((x - X0) / STEP, 0.0, NX - 1.001)
	var fz := clampf((z - Z0) / STEP, 0.0, NZ - 1.001)
	var ix := int(fx)
	var iz := int(fz)
	var tx := fx - ix
	var tz := fz - iz
	var i := iz * NX + ix
	return lerpf(lerpf(heights[i], heights[i + 1], tx), lerpf(heights[i + NX], heights[i + NX + 1], tx), tz)


func _lake_and_fall() -> void:
	# The lake for the camera and the checks: the biggest one the generator made.
	var lr: Array = gen.get_lake_results()
	if lr.size() > 0:
		var big: Dictionary = lr[0]
		for l in lr:
			if l["area"] > big["area"]:
				big = l
		lake_center = big["center"]
		lake_level = big["level"]
		var rect: Rect2 = big["rect"]
		lake_axes = Vector2(maxf(rect.size.x, rect.size.y), minf(rect.size.x, rect.size.y)) * 0.5
		lake_dir = Vector2(1, 0) if rect.size.x >= rect.size.y else Vector2(0, 1)
	# Waterfall: a ledge across the valley on a tributary, where it runs steep in the mountains.
	# STAND-IN: the generator grades every river, so it makes no knickpoints; the ledge is cut here by hand.
	var raw: Array = gen.get_rivers(float(args.get("min-area", "1500000")))
	_log("rivers from the generator: %d" % raw.size())
	if raw.size() < 2:
		return
	var fall_drop := 14.0
	var trib: Dictionary = raw[1]
	var fp: PackedVector3Array = trib["points"]
	var kf := int(fp.size() * 0.35)
	fall_point = fp[kf]
	var fd := fp[mini(kf + 4, fp.size() - 1)] - fp[maxi(kf - 4, 0)]
	fall_dir = Vector2(fd.x, fd.z).normalized()
	var fside := Vector2(-fall_dir.y, fall_dir.x)
	var reach := 260.0
	for zi in NZ:
		var z := Z0 + zi * STEP
		if absf(z - fall_point.z) > reach + 60.0:
			continue
		for xi in NX:
			var x := X0 + xi * STEP
			if absf(x - fall_point.x) > reach + 60.0:
				continue
			var rel := Vector2(x - fall_point.x, z - fall_point.z)
			var along := rel.dot(fall_dir)
			var across := absf(rel.dot(fside))
			if along < -2.0 or along > reach or across > 60.0:
				continue
			var step := smoothstep(-2.0, 2.0, along) * (1.0 - smoothstep(reach * 0.4, reach, along))
			var lateral := 1.0 - smoothstep(25.0, 60.0, across)
			heights[zi * NX + xi] -= fall_drop * step * lateral
	fall_point.y = height_at(fall_point.x, fall_point.z)
	_log("lake at %s level %.1f m, waterfall at %s drop %.0f m" % [lake_center, lake_level, fall_point, fall_drop])




func _build_ground() -> void:
	var t := Time.get_ticks_msec()
	var shape := HeightMapShape3D.new()
	shape.map_width = NX
	shape.map_depth = NZ
	shape.map_data = heights
	ground_body = StaticBody3D.new()
	ground_body.name = "Ground"
	var cs := CollisionShape3D.new()
	cs.shape = shape
	cs.scale = Vector3(STEP, 1.0, STEP)
	ground_body.add_child(cs)
	ground_body.position = Vector3(X0 + (NX - 1) * STEP * 0.5, 0.0, Z0 + (NZ - 1) * STEP * 0.5)
	add_child(ground_body)
	var gh := Ground.new()
	gh.name = "GroundHeight"
	gh.bench = self
	add_child(gh)
	# Display mesh, coloured by the generator's geology: bare rock, scree, soil, alluvium; sand at the coast.
	# STAND-IN material: a StandardMaterial3D with vertex colours, because the engine's snow and wetness live on
	# BaseMaterial3D only (under path tracing too), while the generator's geology and detail-normal code is shader
	# code for a ShaderMaterial.
	var verts := PackedVector3Array()
	verts.resize(NX * NZ)
	var norms := PackedVector3Array()
	norms.resize(NX * NZ)
	var cols := PackedColorArray()
	cols.resize(NX * NZ)
	var rock := Color(0.36, 0.35, 0.33)
	var scree := Color(0.48, 0.46, 0.42)
	var soil := Color(0.2, 0.22, 0.1)
	var alluvium := Color(0.28, 0.27, 0.16)
	var sand := Color(0.7, 0.62, 0.46)
	for zi in NZ:
		var zu := maxi(zi - 1, 0) * NX
		var zd := mini(zi + 1, NZ - 1) * NX
		for xi in NX:
			var i := zi * NX + xi
			var x := X0 + xi * STEP
			var z := Z0 + zi * STEP
			var h := heights[i]
			verts[i] = Vector3(x, h, z)
			var hx := heights[zi * NX + mini(xi + 1, NX - 1)] - heights[zi * NX + maxi(xi - 1, 0)]
			var hz := heights[zd + xi] - heights[zu + xi]
			norms[i] = Vector3(-hx, 2.0 * STEP, -hz).normalized()
			var g := gen.get_geology_at(Vector3(x, h, z)) if z < Z0 + GEN_SIZE else Color(0, 0, 0, 0)
			var c := soil
			if is_nan(g.r):
				c = sand
			else:
				c = soil.lerp(alluvium, clampf((g.a - 5.5) / 1.5, 0.0, 1.0))
				c = c.lerp(rock, clampf(g.g, 0.0, 1.0))
				c = c.lerp(scree, clampf(g.b, 0.0, 1.0))
			c = c.lerp(sand, 1.0 - smoothstep(1.2, 4.0, h))
			cols[i] = c
	var idx := PackedInt32Array()
	idx.resize((NX - 1) * (NZ - 1) * 6)
	var k := 0
	for zi in NZ - 1:
		for xi in NX - 1:
			var i := zi * NX + xi
			idx[k] = i
			idx[k + 1] = i + 1
			idx[k + 2] = i + NX
			idx[k + 3] = i + 1
			idx[k + 4] = i + NX + 1
			idx[k + 5] = i + NX
			k += 6
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_INDEX] = idx
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	ground_mat = StandardMaterial3D.new()
	ground_mat.vertex_color_use_as_albedo = true
	ground_mat.roughness = 0.9
	ground_mat.set_feature(BaseMaterial3D.FEATURE_WETNESS, true)
	ground_mat.wetness_porosity = 0.8
	ground_mat.set_feature(BaseMaterial3D.FEATURE_SNOW, true)
	ground_mesh = MeshInstance3D.new()
	ground_mesh.name = "GroundMesh"
	ground_mesh.mesh = mesh
	ground_mesh.material_override = ground_mat
	add_child(ground_mesh)
	_log("ground %d ms" % (Time.get_ticks_msec() - t))


# --- Water -------------------------------------------------------------------------------------------------------

func _build_water() -> void:
	# The generator's own water: the sea (shore waves, seabed from the ground) and one surface per lake, then the
	# rivers, each ending on the sea, a lake or the river it joins, outflows starting at the lake outlets.
	var t := Time.get_ticks_msec()
	water_nodes = gen.build_water(self)
	for w in water_nodes:
		if String(w.name).begins_with("Sea"):
			ocean = w
		else:
			lakes.append(w)
	if lakes.size() > 0:
		lake = lakes[0]
		for l in lakes:
			if absf(l.global_position.y + l.water_level - lake_level) < absf(lake.global_position.y + lake.water_level - lake_level):
				lake = l
	rivers = gen.build_river_paths(self, float(args.get("min-area", "1500000")))
	_log("water + %d rivers built in %d ms (%d lakes)" % [rivers.size(), Time.get_ticks_msec() - t, lakes.size()])



func _river(nm: String, pts: PackedVector3Array, ws: PackedFloat32Array, q: float, target: Node) -> PhysXRiver3D:
	var r := PhysXRiver3D.new()
	r.name = nm
	var c := Curve3D.new()
	for p in pts:
		c.add_point(p)
	r.curve = c
	var w2 := PackedFloat32Array()
	for w in ws:
		w2.append(clampf(w, 3.0, 40.0))
	r.widths = w2
	r.discharge = maxf(q, 0.5)
	r.step_length = 2.0
	add_child(r)
	if target:
		r.mouth_target = r.get_path_to(target)
	return r


func _ellipse_mesh(axes: Vector2, dir: Vector2) -> ArrayMesh:
	var side := Vector2(-dir.y, dir.x)
	var verts := PackedVector3Array()
	var n := 96
	for k in n:
		var a0 := TAU * k / n
		var a1 := TAU * (k + 1) / n
		var p0 := dir * cos(a0) * axes.x + side * sin(a0) * axes.y
		var p1 := dir * cos(a1) * axes.x + side * sin(a1) * axes.y
		verts.append(Vector3.ZERO)
		verts.append(Vector3(p1.x, 0, p1.y))
		verts.append(Vector3(p0.x, 0, p0.y))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return m


# --- Weather, sky, snow ------------------------------------------------------------------------------------------

func _build_weather() -> void:
	sun = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 1.6
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 6000.0
	add_child(sun)
	sun.look_at_from_position(Vector3(-2500, 1500, 2500), Vector3.ZERO)
	weather = WeatherMap3D.new()
	weather.name = "Weather"
	weather.extent = 8192.0
	weather.resolution = 128
	weather.ground_height = 0.0
	# The weather and the snowpack stay on the world, not the camera, so the peaks are covered from any view.
	var anchor := Node3D.new()
	anchor.name = "WorldAnchor"
	add_child(anchor)
	add_child(weather)
	weather.follow_target = weather.get_path_to(anchor)
	weather.sun_light = weather.get_path_to(sun)
	env = Environment.new()
	sky_mat = WeatherSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
	sky.process_mode = Sky.PROCESS_MODE_INCREMENTAL
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.pathtracing_sky_shader_enabled = true
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not args.has("raster")
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	# Snow: a winter baked over the whole world (SnowClimate: snow line, shaded faces, loaded lee slopes, cornices),
	# carried on by the weather; a finer near square follows the camera.
	snow = SnowCover3D.new()
	snow.name = "Snow"
	snow.extent = float(args.get("snow-extent", "1024"))
	snow.resolution = 256
	add_child(snow)
	snow.height_source = snow.get_path_to(get_node("GroundHeight"))
	snow.sun_light = snow.get_path_to(sun)
	if args.get("snow", "climate") == "climate":
		var climate := SnowClimate.new()
		climate.base_height = 0.0
		climate.january_temperature = 1.0 # mild coast, cold peaks
		climate.prevailing_wind_from = 190.0 # from the sea
		climate.wind_speed = float(args.get("climate-wind", "5"))
		snow.climate = climate
		var t := Time.get_ticks_msec()
		snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), int(args.get("climate-res", "256")))
		_log("snow climate baked in %d ms" % (Time.get_ticks_msec() - t))
		var sw: PackedFloat32Array = climate.swe
		var mx := 0.0
		var nz := 0
		for v in sw:
			mx = maxf(mx, v)
			if v > 1.0: nz += 1
		var cx := 0.0
		for v in climate.cornice_x: cx = maxf(cx, absf(v))
		_log("climate swe max %.1f mm, %d of %d cells with snow, cornice max %.2f m, has_world %s" % [mx, nz, sw.size(), cx, snow.has_world()])
	else:
		snow.extent = 4096.0
		snow.follow_target = snow.get_path_to(get_node("WorldAnchor"))
	cornices = SnowCornice3D.new()
	cornices.name = "Cornices"
	add_child(cornices)
	# Falling and blowing snow around the camera, from the same snowpack and weather.
	snowfall = SnowFall3D.new()
	snowfall.name = "Snowfall"
	add_child(snowfall)
	snowfall.snow_cover = snowfall.get_path_to(snow)
	snow.global_position = Vector3(0, 0, 0)


func _weather_settings() -> void:
	# One weather state drives all of it. Storm: cold wet air, a storm cell over the hills, strong wind from the sea.
	var storm: bool = args.get("weather", "storm") == "storm"
	weather.wind_direction = Vector2(-0.2, -1.0).normalized() # onshore, from the sea up the valleys
	weather.wind_speed = 13.0 if storm else 4.0
	weather.cloud_cover = 0.85 if storm else 0.3
	weather.cloud_type = 0.85 if storm else 0.4
	weather.rain_rate = 8.0 if storm else 0.0
	weather.temperature = 5.0 if storm else 12.0
	weather.humidity = 0.95 if storm else 0.6
	weather.snap_to_settings()
	if storm:
		weather.cloud_thickness = 2500.0 # a deep storm cell: about 1.6 flashes a minute
		weather.add_storm(Vector2(-300, -400), 1500.0, 20.0, 12.0, 0.0)
	# Lightning and thunder from the storm cells.
	lightning = WeatherLightning3D.new()
	lightning.name = "Lightning"
	add_child(lightning)


func _run_weather() -> void:
	var t := Time.get_ticks_msec()
	# A night and a day of weather: snow builds on the peaks, rain wets the lowlands.
	var hours := float(args.get("hours", "12"))
	for k in int(hours):
		weather.advance(3600.0)
		snow.advance(3600.0)
	_log("weather + snow %.0f h in %d ms" % [hours, Time.get_ticks_msec() - t])
	# Snowfall beyond the particle box is fog (SnowFall3D.get_extinction), wired by hand here.
	set_process(true)


func _process(_dt: float) -> void:
	if snowfall and env and args.get("view", "wide") == "peak":
		var ext := snowfall.get_extinction()
		env.fog_enabled = ext > 0.0
		env.fog_density = ext


# --- Vegetation --------------------------------------------------------------------------------------------------

func _build_vegetation() -> void:
	var t := Time.get_ticks_msec()
	data_map = TerrainDataMap3D.new()
	data_map.name = "DataMap"
	data_map.size = 4096.0
	data_map.resolution = int(args.get("map-res", "1024"))
	data_map.bake_on_ready = false
	add_child(data_map)
	data_map.position = Vector3(0, 0, 0)
	data_map.height_source = data_map.get_path_to(get_node("GroundHeight"))
	var ws: Array[NodePath] = []
	for r in rivers:
		ws.append(data_map.get_path_to(r))
	for w in water_nodes:
		ws.append(data_map.get_path_to(w))
	data_map.water_sources = ws
	data_map.geology_source = data_map.get_path_to(gen)
	data_map.bake()
	_log("data map bake %d ms" % (Time.get_ticks_msec() - t))
	t = Time.get_ticks_msec()
	# STAND-IN trees: tree generation is not built yet, so cones and balls mark where the forest grows.
	var conifer := PlantSpecies.new()
	conifer.meshes = [_tree_mesh(true, 14.0), _tree_mesh(true, 10.0)]
	conifer.spacing = 9.0
	conifer.density = 0.8
	conifer.footprint = 3.0
	conifer.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(120, 650))
	conifer.set_rule_fade(PlantSpecies.RULE_HEIGHT, 80.0)
	conifer.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 32))
	conifer.set_rule_fade(PlantSpecies.RULE_SLOPE, 6.0)
	conifer.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(10, 1000000))
	conifer.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 5.0)
	conifer.set_rule_range(PlantSpecies.RULE_SOIL_DEPTH, Vector2(0.5, 100000))
	conifer.set_rule_fade(PlantSpecies.RULE_SOIL_DEPTH, 0.1)
	conifer.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
	conifer.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	conifer.set_rule_range(PlantSpecies.RULE_TALUS, Vector2(0, 0.2))
	var broad := PlantSpecies.new()
	broad.meshes = [_tree_mesh(false, 11.0), _tree_mesh(false, 8.0)]
	broad.spacing = 10.0
	broad.density = 0.7
	broad.footprint = 3.5
	broad.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(6, 220))
	broad.set_rule_fade(PlantSpecies.RULE_HEIGHT, 40.0)
	broad.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 25))
	broad.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
	broad.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(12, 1000000))
	broad.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 6.0)
	broad.set_rule_range(PlantSpecies.RULE_SOIL_DEPTH, Vector2(0.5, 100000))
	broad.set_rule_fade(PlantSpecies.RULE_SOIL_DEPTH, 0.1)
	broad.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
	broad.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	broad.set_rule_range(PlantSpecies.RULE_TALUS, Vector2(0, 0.2))
	broad.patch_scale = 300.0
	broad.patch_cover = 0.65
	var biome := Biome.new()
	biome.species = [conifer, broad]
	veg = VegetationField3D.new()
	veg.name = "Forest"
	veg.biome = biome
	veg.tile_size = 128.0
	veg.populate_on_ready = false
	add_child(veg)
	veg.data_map = veg.get_path_to(data_map)
	veg.populate()
	_log("forest %d plants in %d ms" % [veg.get_plant_count(), Time.get_ticks_msec() - t])
	# Grass near the camera, on the same maps, moved by the weather's wind.
	var meadow := PlantSpecies.new()
	meadow.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 28))
	meadow.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(3, 1000000))
	meadow.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(3, 700))
	grass = GrassField3D.new()
	grass.name = "Grass"
	grass.rules = meadow
	grass.radius = float(args.get("grass-radius", "40"))
	add_child(grass)
	grass.data_map = grass.get_path_to(data_map)
	# The forest as shelter for the wind: stands become canopies in the weather map.
	weather.canopy_map = _canopy_map()
	weather.canopy_map_rect = Rect2(-2048, -2048, 4096, 4096)


func _canopy_map() -> ImageTexture:
	var n := 256
	var img := Image.create(n, n, false, Image.FORMAT_RF)
	for s in 2:
		for p in veg.get_placements(s):
			var x := int((p.x + 2048.0) / 4096.0 * n)
			var z := int((p.z + 2048.0) / 4096.0 * n)
			if x >= 0 and z >= 0 and x < n and z < n:
				img.set_pixel(x, z, Color(12.0 if s == 0 else 10.0, 0, 0))
	return ImageTexture.create_from_image(img)


func _tree_mesh(conifer: bool, h: float) -> Mesh:
	var st := SurfaceTool.new()
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.07, 0.16, 0.08) if conifer else Color(0.14, 0.22, 0.07)
	mat.roughness = 0.85
	mat.set_feature(BaseMaterial3D.FEATURE_WETNESS, true)
	mat.wetness_porosity = 0.3
	mat.set_feature(BaseMaterial3D.FEATURE_SNOW, true)
	var m: Mesh
	if conifer:
		var cone := CylinderMesh.new()
		cone.top_radius = 0.0
		cone.bottom_radius = h * 0.22
		cone.height = h * 0.85
		cone.radial_segments = 8
		cone.rings = 1
		m = cone
		st.append_from(cone, 0, Transform3D(Basis(), Vector3(0, h * 0.15 + h * 0.425, 0)))
	else:
		var ball := SphereMesh.new()
		ball.radius = h * 0.3
		ball.height = h * 0.55
		ball.radial_segments = 10
		ball.rings = 5
		st.append_from(ball, 0, Transform3D(Basis(), Vector3(0, h * 0.62, 0)))
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.15
	trunk.bottom_radius = 0.25
	trunk.height = h * 0.4
	trunk.radial_segments = 6
	trunk.rings = 1
	st.append_from(trunk, 0, Transform3D(Basis(), Vector3(0, h * 0.2, 0)))
	var out := st.commit()
	out.surface_set_material(0, mat)
	return out


# --- Camera ------------------------------------------------------------------------------------------------------

func _build_camera() -> void:
	cam = Camera3D.new()
	cam.far = 20000.0
	cam.near = 0.5
	cam.fov = 55.0
	add_child(cam)
	var view: String = args.get("view", "wide")
	var coast_z := _coast_z(0.0)
	match view:
		"lake":
			var back := Vector3(lake_dir.x, 0, lake_dir.y) * -1.0
			var side := Vector3(-lake_dir.y, 0, lake_dir.x)
			cam.position = lake_center + (Vector3(lake_dir.x, 0, lake_dir.y) * 260.0 + side * 120.0) + Vector3(0, 70, 0)
			cam.look_at(lake_center + back * 80.0)
		"fall":
			var fd3 := Vector3(fall_dir.x, 0, fall_dir.y)
			var side := Vector3(-fall_dir.y, 0, fall_dir.x)
			cam.position = fall_point + fd3 * 90.0 + side * 30.0 + Vector3(0, 2.0, 0)
			cam.position.y = height_at(cam.position.x, cam.position.z) + 8.0
			cam.look_at(fall_point + Vector3(0, -6, 0))
		"beach":
			cam.fov = 60.0
			cam.position = Vector3(40.0, 0.0, coast_z - 60.0)
			cam.position.y = height_at(cam.position.x, cam.position.z) + 2.0
			cam.look_at(Vector3(-60.0, 0.0, coast_z + 140.0))
		"peak":
			var top := _highest_point()
			var off := Vector3(-120, 0, 160)
			cam.position = top + off
			cam.position.y = height_at(cam.position.x, cam.position.z) + 3.0
			cam.look_at(top + Vector3(0, -40, 0))
		_:
			cam.position = Vector3(300, 520, coast_z + 900.0)
			cam.look_at(Vector3(-100, 250, -700))
	grass.update_at(cam.global_position)


# --- Links -------------------------------------------------------------------------------------------------------

func _checks() -> void:
	var peak := _highest_point()
	# Terrain -> rivers, lake, sea
	check(rivers.size() >= 3, "terrain generator gives a river network (%d rivers placed)" % rivers.size())
	var lip := 0
	for r in rivers:
		lip += r.get_waterfalls().size()
	check(lip > 0, "waterfall forms where the river falls off the ledge (%d)" % lip)
	check(false, "the generator makes waterfalls (knickpoints, hard rock bands): the ledge is cut by hand here", true)
	if args.has("probe"):
		for z in range(1000, 2049, 40):
			var pp := Vector3(0, 0, z)
			print("  z %d  mine %.2f  gen %.2f  water %.2f  depth %.2f" % [z, height_at(0, z), gen.get_height(pp), gen.get_water_level(pp), gen.get_water_depth(pp)])
		for w in lakes:
			var aabb: AABB = w.surface_mesh.get_aabb() if w.surface_mesh else AABB()
			print("  lake node %s pos %s level %.2f mesh aabb %s wet(center) %s" % [w.name, w.global_position, w.water_level, aabb, w.is_wet(w.global_position + aabb.get_center())])
		for l in gen.get_lake_results():
			print("  result ", l)
		for d in gen.get_rivers(float(args.get("min-area", "1500000"))):
			var pts: PackedVector3Array = d["points"]
			print("  river from_lake %s ends_in %s lake %s joins %s area %.2f km2 head %s mouth %s" % [d.get("from_lake"), d.get("ends_in"), d.get("lake"), d.get("joins"), d["areas"][d["areas"].size() - 1] / 1e6, pts[0], pts[pts.size() - 1]])
		for x in [-1800.0, -1000.0, 0.0, 1000.0, 1800.0]:
			var z := 0.0
			while z < 2040.0 and is_nan(gen.get_water_level(Vector3(x, 0, z))):
				z += 4.0
			print("  coast x %d at z %d: inland 10/20/40/80/120 m: %.2f %.2f %.2f %.2f %.2f" % [x, z, height_at(x, z - 10), height_at(x, z - 20), height_at(x, z - 40), height_at(x, z - 80), height_at(x, z - 120)])
	# Coast: a sea with a beach and a seabed deep enough for the open-sea waves.
	check(ocean != null and gen.has_sea_result(), "the generator makes the sea and build_water() places it")
	# Along the coast: share of it with a low beach (under 3 m, 20 m inland), and the depth 300 m out.
	var low := 0
	var n_c := 0
	var depth_sum := 0.0
	for k in 37:
		var x := -1800.0 + k * 100.0
		var cz := _coast_z(x)
		if cz >= 2040.0:
			continue
		n_c += 1
		var b := height_at(x, cz - 40.0)
		if b > 0.3 and b < 4.0:
			low += 1
		depth_sum += -height_at(x, minf(cz + 300.0, 2040.0))
	var beach_share := float(low) / maxf(n_c, 1)
	check(beach_share > 0.3 and depth_sum / maxf(n_c, 1) > 2.0, "beaches rise from the sea (%d%% of %d coast points 0.3 to 4 m high 40 m inland; seabed %.1f m deep 300 m out on average)" % [int(beach_share * 100.0), n_c, depth_sum / maxf(n_c, 1)])
	check(ocean != null and ocean.shore_waves and ocean.seabed_from_floor, "the sea has shore waves on the measured seabed")
	check(ocean != null and ocean.follow_weather and ocean.open_sea_beyond and ocean.render_extent >= 8000.0 and ocean.wave_cascades >= 3, "the generated sea follows the weather and runs past the world with no hand settings (extent %.0f m, %d cascades)" % [ocean.render_extent if ocean else 0.0, ocean.wave_cascades if ocean else 0])
	var lakes_follow := 0
	for l in lakes:
		if l.follow_weather:
			lakes_follow += 1
	check(lakes_follow == lakes.size(), "lake surfaces follow the weather wind (%d of %d)" % [lakes_follow, lakes.size()])
	# Lakes: surfaces at their levels, rivers in and out.
	var lr: Array = gen.get_lake_results()
	check(lakes.size() == lr.size() and lr.size() > 0, "the generator makes lakes and build_water() places a surface on each (%d)" % lakes.size())
	var lake_ok := 0
	for l in lr:
		var y: float = -INF
		for w in lakes:
			var h: float = w.sample_height(l["center"])
			if is_finite(h):
				y = h
		if is_finite(y) and absf(y - float(l["level"])) < 0.5:
			lake_ok += 1
		else:
			_log("  lake at %s level %.2f: water there %.2f, generator says %.2f" % [l["center"], l["level"], y, gen.get_water_level(l["center"])])
	check(lake_ok == lr.size(), "each lake's water stands at its level over its centre (%d of %d)" % [lake_ok, lr.size()])
	var to_sea := 0
	var to_lake := 0
	var from_lake := 0
	var dangling := 0
	for r in rivers:
		var tgt: Node = r.get_node_or_null(r.mouth_target)
		if tgt == ocean:
			to_sea += 1
		elif tgt in lakes:
			to_lake += 1
		elif tgt == null:
			dangling += 1
	for d in gen.get_rivers(float(args.get("min-area", "1500000"))):
		if d.get("from_lake", -1) >= 0:
			from_lake += 1
	check(to_sea > 0 and dangling == 0, "every river ends somewhere: %d in the sea, %d in lakes, %d joins, %d nowhere" % [to_sea, to_lake, rivers.size() - to_sea - to_lake - dangling, dangling])
	var outlets := 0
	for l in lr:
		if l.has("outlet"):
			outlets += 1
	_log("lakes with an outlet: %d" % outlets)
	check(to_lake > 0 or lr.is_empty(), "rivers flow into the lakes (%d)" % to_lake)
	check(from_lake >= outlets, "an outflow river leaves each lake with an outlet (%d rivers for %d outlets)" % [from_lake, outlets])
	# Weather -> everything
	var wet := weather.get_wetness_at(Vector3(0, 0, 900))
	check(wet > 0.05, "rain wets the lowlands (wetness %.2f)" % wet)
	var rain := weather.get_rain_at(Vector3(-300, 0, -400))
	check(rain > 1.0, "storm rains over the hills (%.1f mm/h)" % rain)
	var snow_peak := _snow_depth(peak)
	var snow_coast := _snow_depth(Vector3(0, 0, 1200))
	check(snow_peak > 0.02 and snow_coast < 0.005, "snow lies on the peaks (%.2f m at %.0f m) and not on the coast (%.3f m)" % [snow_peak, peak.y, snow_coast])
	# Snow line from the baked winter: mean depth by height band over the world tier.
	if snow.has_world():
		var bands := [0.0, 0.0, 0.0, 0.0]
		var counts := [0, 0, 0, 0]
		for k in 4000:
			var x := -2000.0 + (k % 80) * 50.0
			var z := -2000.0 + (k / 80) * 70.0
			var h := height_at(x, z)
			if h < 1.0:
				continue
			var b := clampi(int(h / 250.0), 0, 3)
			bands[b] += _snow_depth(Vector3(x, h, z))
			counts[b] += 1
		var txt := ""
		for b in 4:
			txt += "%d-%d m: %.2f m  " % [b * 250, b * 250 + 250, bands[b] / maxf(counts[b], 1)]
		_log("snow by height: " + txt)
		check(bands[3] / maxf(counts[3], 1) > bands[0] / maxf(counts[0], 1) + 0.1, "the baked winter gives a snow line: deeper with height (" + txt.strip_edges() + ")")
	else:
		check(false, "SnowCover3D world tier from bake_climate", false)
	# Cornices on the lee crests near the peak.
	var cprobe := Node3D.new()
	add_child(cprobe)
	cprobe.global_position = peak
	cornices.follow_target = cornices.get_path_to(cprobe)
	cornices.rebuild()
	check(cornices.get_segment_count() > 0, "cornices grow on the lee crests around the peak (%d pieces)" % cornices.get_segment_count())
	cornices.follow_target = NodePath("")
	check(ocean.follow_weather and absf(ocean.wind_speed - weather.get_mean_wind_at(ocean.global_position).length()) < 3.0, "sea state follows the weather wind (%.1f m/s vs %.1f)" % [ocean.wind_speed, weather.get_mean_wind_at(ocean.global_position).length()])
	# Grass on a meadow by the lake (the wide camera hangs over the sea, where none grows).
	var meadow_pt := lake_center + Vector3(-lake_dir.y, 0, lake_dir.x) * (lake_axes.y + 80.0)
	meadow_pt.y = height_at(meadow_pt.x, meadow_pt.z)
	grass.update_at(meadow_pt)
	var g := grass.get_blade_count()
	check(g > 0, "grass grows from the data map's rules on a meadow (%d blades); it bends with weather_wind" % g)
	grass.update_at(cam.global_position)
	check(false, "trees and shrubs bend with the wind (stand-in forest is static; tree generation and bone wind not built)", true)
	# A dry point below the lake's level, far from it and from the rivers (on the coastal plain).
	var plain := Vector3.ZERO
	for k in 400:
		var c := Vector3(-1900.0 + (k % 20) * 200.0, 0.0, -1900.0 + (k / 20) * 200.0)
		c.y = height_at(c.x, c.z)
		if c.y > 3.0 and c.y < lake_level - 5.0 and Vector2(c.x - lake_center.x, c.z - lake_center.z).length() > 1000.0 and is_nan(gen.get_water_level(c)):
			plain = c
			break
	var wd := data_map.get_water_depth_at(plain)
	check(plain.y < lake_level and wd <= 0.0, "data map: land away from the lake stays dry (%.1f m deep at %.0f m, lake at %.0f m)" % [wd, plain.y, lake_level])
	check(veg.get_plant_count() > 1000, "forest placed by slope, height and water rules (%d plants)" % veg.get_plant_count())
	# Geology rules: no tree stands on bare rock or scree, every tree on soil.
	var on_rock := 0
	var total := 0
	for sp in 2:
		for p in veg.get_placements(sp):
			total += 1
			if data_map.get_rock_at(p) > 0.4 or data_map.get_talus_at(p) > 0.3 or data_map.get_soil_depth_at(p) < 0.4:
				on_rock += 1
	check(total > 0 and on_rock == 0, "trees follow the generator's geology: %d of %d on bare rock, scree or thin soil" % [on_rock, total])
	check(false, "rain raises river discharge and lake level (rivers carry a fixed discharge)", true)
	check(false, "snowmelt runoff reaches rivers (it feeds the surface water film only)", true)
	# Lightning: a ground flash in the storm cell, its channel lighting the scene, thunder after it.
	var strikes_before := lightning.get_ground_strike_count()
	lightning.strike(Vector3(-300, 0, -400), true)
	var bright := 0.0
	var t0 := Time.get_ticks_msec()
	for i in 120:
		await get_tree().process_frame
		bright = maxf(bright, lightning.get_flash_brightness())
	_log("lightning after %d ms: strikes %d, ground %d (before %d), channel %d points" % [Time.get_ticks_msec() - t0, lightning.get_strike_count(), lightning.get_ground_strike_count(), strikes_before, lightning.get_last_channel().size()])
	var ch: PackedVector3Array = lightning.get_last_channel()
	var ground_end := ch[ch.size() - 1] if ch.size() > 0 else Vector3(INF, INF, INF)
	var on_ground := ch.size() > 0 and absf(ground_end.y - height_at(ground_end.x, ground_end.z)) < 5.0
	check(lightning.get_ground_strike_count() > strikes_before and on_ground and bright > 0.0, "lightning strikes the ground in the storm (channel %d points, ends %.1f m from the ground, brightness %.2f)" % [ch.size(), absf(ground_end.y - height_at(ground_end.x, ground_end.z)) if ch.size() > 0 else -1.0, bright])
	var dist := cam.global_position.distance_to(ground_end) if ch.size() > 0 else 0.0
	var delay := lightning.get_last_thunder_delay()
	check(delay > 0.0 and absf(delay - dist / 343.0) < 0.35 * dist / 343.0 + 0.5, "thunder follows the flash by its distance (%.1f s for about %.0f m; %.1f s long)" % [delay, dist, lightning.get_last_thunder_length()])
	_log("flash rate (Price and Rind) for a cloud top at %.0f m: %.2f a minute" % [weather.cloud_base_height + weather.cloud_thickness, lightning.get_flash_rate(weather.cloud_base_height + weather.cloud_thickness)])
	check(false, "the terrain's geology/detail-normal ShaderMaterial gets the engine's snow and wetness (BaseMaterial3D only)", true)
	# Falling snow: on the peak in the storm, none at the coast (rain there).
	var probe_node := Node3D.new()
	add_child(probe_node)
	snowfall.follow_target = snowfall.get_path_to(probe_node)
	probe_node.global_position = peak + Vector3(0, 2, 0)
	for i in 20:
		await get_tree().process_frame
	var rate_peak := snowfall.get_snowfall_rate()
	var flakes_peak := snowfall.get_visible_flakes()
	var vis_peak := snowfall.get_visibility()
	probe_node.global_position = Vector3(0, height_at(0, 1200) + 2.0, 1200)
	for i in 20:
		await get_tree().process_frame
	var rate_coast := snowfall.get_snowfall_rate()
	check(rate_peak > 0.1 and flakes_peak > 0 and rate_coast < 0.05, "snow falls on the peak in the storm (%.1f mm/h, %d flakes, visibility %.0f m) and not at the coast (%.2f mm/h)" % [rate_peak, flakes_peak, vis_peak, rate_coast])
	snowfall.follow_target = NodePath("")
	check(false, "snowfall extinction feeds the fog by itself (wired by a script here)", true)
	check(false, "falling rain particles from the weather (none in the engine)", true)
	var passes := 0
	var gaps := 0
	var fails := 0
	for r in results:
		if r[0] == "PASS": passes += 1
		elif r[0] == "FAIL": fails += 1
		else: gaps += 1
	print("[landscape] links: %d pass, %d fail, %d gaps" % [passes, fails, gaps])


func _snow_depth(p: Vector3) -> float:
	# The near square where it covers the point, else the world tier the climate bake left.
	var r: Rect2 = snow.get_map_rect()
	if r.has_point(Vector2(p.x, p.z)) or not snow.has_world():
		return snow.get_depth_at(p)
	return snow.get_world_depth_at(p)


func _coast_z(x: float) -> float:
	# Where the generator's coast is along x: the first point south with sea over it.
	var z := 0.0
	# (sea level, so lakes on the way do not count)
	while z < 2040.0 and not (gen.get_water_level(Vector3(x, 0, z)) <= gen.sea_level + 0.5):
		z += 4.0
	return z


func _highest_point() -> Vector3:
	var best := 0
	for i in heights.size():
		if heights[i] > heights[best]:
			best = i
	return Vector3(X0 + (best % NX) * STEP, heights[best], Z0 + (best / NX) * STEP)

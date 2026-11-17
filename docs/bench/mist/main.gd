extends Node3D
# Ground mist and pollen puffs (docs/ideas/vegetation.md, "Ground mist and pollen with PhysX
# Flow"): VegetationMist3D and VegetationPollen3D on a 64 m meadow with a hollow (south) and a
# crest (north). Needs an engine built with flow_sdk= and the Flow libraries beside it.
#
# godot --path docs/bench/mist -- [options]
#   --test           run the checks, print PASS/FAIL, quit (MIST_RESULT)
#   --mist[=off]     dawn view across the hollow, a walker crossing the mist
#   --pollen         close view of a walker brushing flowering cocksfoot and timothy
#   --out=path.png   save the view after --frames drawn frames (default 480; Flow steps once a frame)
#   --raster         raster (default); --spp=N path traced instead

const SIZE := 64.0
var test := false
var view := ""
var mist_off := false
var out := ""
var raster := true
var spp := 8
var frames := 480
var fails := 0
var dm: TerrainDataMap3D
var field: VegetationField3D
var mist: VegetationMist3D
var pollen: VegetationPollen3D
var walker: AnimatableBody3D
var walker_from := Vector3.ZERO
var walker_to := Vector3.ZERO
var walker_speed := 1.4
var walker_moving := false
var cam: Camera3D
var sun: DirectionalLight3D

const HOLLOW := Vector2(0, -12)
const CREST := Vector2(0, 14)

static func height(x: float, z: float) -> float:
	var hollow := -2.2 * exp(-((x - HOLLOW.x) * (x - HOLLOW.x) + (z - HOLLOW.y) * (z - HOLLOW.y)) / 70.0)
	var crest := 1.8 * exp(-((x - CREST.x) * (x - CREST.x) + (z - CREST.y) * (z - CREST.y)) / 60.0)
	return hollow + crest + 0.15 * sin(x * 0.21) * sin(z * 0.17)

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--mist": view = "mist"
		elif a == "--mist=off":
			view = "mist"
			mist_off = true
		elif a == "--pollen": view = "pollen"
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="):
			spp = int(a.substr(6))
			raster = false
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--frames="): frames = int(a.substr(9))
	_terrain()
	_walker()
	_env()
	if test:
		_checks.call_deferred()
	elif not view.is_empty():
		_view.call_deferred()

func _terrain() -> void:
	var w := int(SIZE) + 1
	var data := PackedFloat32Array()
	data.resize(w * w)
	for z in w:
		for x in w:
			data[z * w + x] = height(x - SIZE * 0.5, z - SIZE * 0.5)
	var shape := HeightMapShape3D.new()
	shape.map_width = w
	shape.map_depth = w
	shape.map_data = data
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	body.add_child(cs)
	add_child(body)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var res := 128
	var step := SIZE / res
	for z in res:
		for x in res:
			var x0 := -SIZE * 0.5 + x * step
			var z0 := -SIZE * 0.5 + z * step
			var q := [Vector3(x0, height(x0, z0), z0), Vector3(x0 + step, height(x0 + step, z0), z0),
					Vector3(x0 + step, height(x0 + step, z0 + step), z0 + step), Vector3(x0, height(x0, z0 + step), z0 + step)]
			for i in [0, 1, 2, 0, 2, 3]:
				st.add_vertex(q[i])
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.12, 0.16, 0.06) # Meadow ground under the sward.
	gm.roughness = 0.95
	mi.material_override = gm
	add_child(mi)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = 128
	dm.bake_on_ready = false
	add_child(dm)

func _species(preset: int, spacing: float, density: float, textures: bool) -> PlantSpecies:
	var gen := PlantGenerator.new()
	gen.textures = textures
	gen.preset = preset
	gen.apply_preset_colors()
	gen.seed = 700 + preset
	gen.season = {"forcing": 900.0, "day_of_year": 170}
	var sp := PlantSpecies.new()
	var ms: Array[Mesh] = []
	for m in gen.generate_variants(2):
		ms.append(m)
	sp.meshes = ms
	sp.spacing = spacing
	sp.footprint = 0.0
	sp.density = density
	sp.align_to_ground = 0.3
	sp.scale_range = Vector2(0.85, 1.2)
	sp.visibility_range = 30.0 # Lavapipe draws a few frames a second; the view needs only the near meadow.
	sp.set_meta("plant_preset", preset)
	var amount := PlantGenerator.get_pollen_amount(preset)
	if amount > 0.0:
		sp.set_meta("pollen", amount)
	return sp

func _meadow(textures: bool) -> void:
	var b := Biome.new()
	var list: Array[PlantSpecies] = [
		_species(PlantGenerator.PRESET_COCKSFOOT, 0.35, 0.8, textures),
		_species(PlantGenerator.PRESET_TIMOTHY, 0.35, 0.7, textures),
		_species(PlantGenerator.PRESET_SORREL, 0.6, 0.4, textures),
		_species(PlantGenerator.PRESET_YARROW, 0.6, 0.4, textures),
	]
	b.species = list
	field = VegetationField3D.new()
	field.name = "Field"
	field.far_volume = false # Not under test here.
	field.populate_on_ready = false
	field.biome = b
	add_child(field)
	field.data_map = field.get_path_to(dm)
	dm.bake()
	field.populate()

func _walker() -> void:
	walker = AnimatableBody3D.new()
	walker.name = "Walker"
	walker.sync_to_physics = false
	var cs := CollisionShape3D.new()
	var cap := CapsuleShape3D.new()
	cap.radius = 0.3
	cap.height = 1.75
	cs.shape = cap
	cs.position.y = 0.875
	walker.add_child(cs)
	var mi := MeshInstance3D.new()
	var cm := CapsuleMesh.new()
	cm.radius = 0.3
	cm.height = 1.75
	mi.mesh = cm
	mi.position.y = 0.875
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.2, 0.18, 0.16)
	mi.material_override = m
	walker.add_child(mi)
	walker.collision_layer = 2 # Not ground: the map's and the mist's height rays must not hit it.
	walker.set_meta("trample_radius", 0.3)
	walker.add_to_group("grass_trample")
	walker.position = Vector3(0, -50, 0)
	add_child(walker)

func _walk(from: Vector2, to: Vector2, speed: float) -> void:
	walker_from = Vector3(from.x, height(from.x, from.y), from.y)
	walker_to = Vector3(to.x, height(to.x, to.y), to.y)
	walker_speed = speed
	walker.global_position = walker_from
	walker_moving = true

func _physics_process(dt: float) -> void:
	if not walker_moving:
		return
	var p := walker.global_position
	var d := walker_to - p
	d.y = 0.0
	if d.length() < walker_speed * dt:
		walker_moving = false
		return
	p += d.normalized() * walker_speed * dt
	p.y = height(p.x, p.z)
	walker.global_position = p

func _env() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.72, 0.68, 0.62) # Dawn haze.
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.62, 0.66, 0.74) # Dawn sky light: the mist in shade is lit by it.
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.name = "Env"
	we.environment = env
	add_child(we)
	sun = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_color = Color(1.0, 0.82, 0.62)
	sun.shadow_enabled = true
	_sun_at(6.0, 200.0)
	add_child(sun)
	cam = Camera3D.new()
	cam.name = "Camera"
	cam.fov = 55.0
	add_child(cam)

# Sun elevation and azimuth (degrees; azimuth from +z toward +x is where the light comes from).
func _sun_at(elev: float, az: float) -> void:
	var dir := Vector3(sin(deg_to_rad(az)) * cos(deg_to_rad(elev)), sin(deg_to_rad(elev)), cos(deg_to_rad(az)) * cos(deg_to_rad(elev)))
	sun.global_transform = Transform3D(Basis.looking_at(-dir, Vector3.UP if absf(dir.y) < 0.99 else Vector3.FORWARD), Vector3.ZERO)

func _check(label: String, ok: bool, value) -> void:
	print(("PASS " if ok else "FAIL ") + label + ": " + str(value))
	if not ok:
		fails += 1

func _wait(seconds: float) -> void:
	var t := 0.0
	while t < seconds:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()

# Flow steps once per drawn frame, so the gas's time runs by frames, not by the clock (lavapipe
# draws a few frames a second).
func _frames(count: int) -> void:
	for i in count:
		await RenderingServer.frame_post_draw
		if not test and i % 50 == 49:
			print("frame ", i + 1, " of ", count)

func _mist_node(amount: float) -> VegetationMist3D:
	var m := VegetationMist3D.new()
	m.name = "Mist"
	m.mist_amount = amount
	m.mist_range = 18.0
	m.mist_cell = 6.0
	m.sun_light = NodePath("../Sun")
	m.follow_target = NodePath("../Camera")
	add_child(m)
	return m

func _checks() -> void:
	# Pollen data: wind-pollinated presets only, shedding in their months.
	var summer := {"forcing": 900.0, "day_of_year": 170}
	var winter := {"forcing": 0.0, "day_of_year": 20}
	_check("pollen: grasses shed, daisy does not", PlantGenerator.get_pollen_amount(PlantGenerator.PRESET_COCKSFOOT) > 0.5 and PlantGenerator.get_pollen_amount(PlantGenerator.PRESET_DAISY) == 0.0, [PlantGenerator.get_pollen_amount(PlantGenerator.PRESET_COCKSFOOT), PlantGenerator.get_pollen_amount(PlantGenerator.PRESET_DAISY)])
	var rs := PlantGenerator.get_pollen_release(PlantGenerator.PRESET_COCKSFOOT, summer)
	var rw := PlantGenerator.get_pollen_release(PlantGenerator.PRESET_COCKSFOOT, winter)
	_check("pollen: cocksfoot sheds in June, not in January", rs > 0.8 and rw == 0.0, [rs, rw])
	var tagged := 0
	var with_pollen := 0
	var floor_set := PlantGenerator.make_floor(PlantGenerator.FLOOR_RIPARIAN, 1, false)
	for sp in floor_set:
		tagged += 1 if sp.has_meta("plant_preset") else 0
		with_pollen += 1 if sp.has_meta("pollen") else 0
	_check("pollen: make_floor species carry their preset, wind-pollinated ones their pollen", tagged == floor_set.size() and with_pollen > 0, [tagged, floor_set.size(), with_pollen])

	cam.global_position = Vector3(0, height(0, -20) + 1.6, -20)
	cam.look_at(Vector3(0, 0, 0))
	mist = _mist_node(1.0)
	var flow_ok := mist.is_flow_available()
	_check("Flow is in this build", flow_ok, flow_ok)
	var ph := mist.get_propensity_at(Vector3(HOLLOW.x, 0, HOLLOW.y))
	var pc := mist.get_propensity_at(Vector3(CREST.x, 0, CREST.y))
	_check("mist: the hollow gathers more than the crest", ph > 1.2 and pc < 0.8, [ph, pc])

	# The weather: humid, calm, clear, before the sun is up.
	var wm := WeatherMap3D.new()
	wm.name = "Weather"
	wm.transition_time = 0.0
	wm.humidity = 0.98
	wm.wind_speed = 0.5
	wm.cloud_cover = 0.05
	wm.temperature = 8.0
	add_child(wm)
	wm.snap_to_settings()
	mist.weather_map = NodePath("../Weather")
	_sun_at(3.0, 200.0)
	var dawn := mist.weather_mist_level()
	_sun_at(30.0, 200.0)
	var day := mist.weather_mist_level()
	_sun_at(3.0, 200.0)
	wm.wind_speed = 6.0
	wm.snap_to_settings()
	var windy := mist.weather_mist_level()
	wm.wind_speed = 0.5
	wm.humidity = 0.7
	wm.snap_to_settings()
	var dry := mist.weather_mist_level()
	_check("mist: forms at dawn in humid calm air, gone with the sun up, wind or dry air", dawn > 0.6 and day < 0.05 and windy < 0.05 and dry < 0.05, [dawn, day, windy, dry])
	mist.weather_map = NodePath("")
	wm.queue_free()

	if not flow_ok:
		_check("without Flow the nodes stay inert", mist.get_flow() == null, mist.get_flow())
		_finish()
		return
	var flow: Node = mist.get_flow()
	flow.set("cpu_readback", true)
	await _frames(300)
	var st: Dictionary = flow.get_stats()
	_check("mist: Flow runs with blocks over the meadow", flow.is_running() and int(st.get("active_blocks", 0)) > 0, [flow.is_running(), st.get("active_blocks", 0), st.get("error", "")])
	# 0.6 m up: the terrain floor's tiles take the lowest few cells.
	var at_h := Vector3(HOLLOW.x, height(HOLLOW.x, HOLLOW.y) + 0.6, HOLLOW.y)
	var at_c := Vector3(CREST.x, height(CREST.x, CREST.y) + 0.6, CREST.y)
	var sh: float = flow.sample_smoke(at_h)
	var sc: float = flow.sample_smoke(at_c)
	print("MIST_STATS ", st.get("bounds"), " step_ms ", st.get("step_ms"), " hollow ", at_h, " crest ", at_c)
	_check("mist: thicker in the hollow than on the crest", sh > 0.5 and sh > sc * 1.3, [sh, sc])
	var above: float = flow.sample_smoke(at_h + Vector3(0, 3.0, 0))
	_check("mist: a ground layer, thin 3 m up", above < sh * 0.5, [above, sh])
	mist.mist_amount = 0.0
	await _frames(300)
	var cleared: float = flow.sample_smoke(at_h)
	_check("mist: clears when the level drops", cleared < sh * 0.5, [cleared, sh])
	mist.queue_free()
	await _wait(0.5)

	# Pollen: the walker through flowering cocksfoot and timothy. The camera looks at the walker from
	# above, close, so lavapipe draws few plants.
	cam.global_position = Vector3(0, height(0, 0) + 6.0, 0.5)
	cam.look_at(Vector3(0, height(0, 0), 0))
	cam.far = 12.0
	_meadow(false)
	pollen = VegetationPollen3D.new()
	pollen.name = "Pollen"
	pollen.season = summer
	add_child(pollen)
	pollen.vegetation_field = pollen.get_path_to(field)
	await _wait(0.2)
	_check("pollen: the grasses' placements indexed", pollen.get_pollen_plant_count() > 500, pollen.get_pollen_plant_count())
	var pf: Node = pollen.get_flow()
	pf.set("cpu_readback", true)
	walker.global_position = Vector3(-6, height(-6, 0), 0)
	await _wait(1.0)
	_check("pollen: none from a body standing still", pollen.get_puffs_fired() == 0, pollen.get_puffs_fired())
	_walk(Vector2(-6, 0), Vector2(6, 0), 1.4)
	await _wait(2.0)
	var fired := pollen.get_puffs_fired()
	_check("pollen: a walker brushing grasses in flower shakes out puffs", fired > 3, fired)
	await _frames(30)
	var smoke: float = pf.sample_smoke(walker.global_position + Vector3(-1.0, 0.8, 0))
	var st2: Dictionary = pf.get_stats()
	_check("pollen: Flow carries the puffs", int(st2.get("active_blocks", 0)) > 0, [st2.get("active_blocks", 0), smoke])
	walker_moving = false
	pollen.season = winter
	var before := pollen.get_puffs_fired()
	_walk(Vector2(6, 3), Vector2(-6, 3), 1.4)
	await _wait(2.0)
	_check("pollen: none in January", pollen.get_puffs_fired() == before, pollen.get_puffs_fired() - before)
	_finish()

func _finish() -> void:
	print("MIST_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
	get_tree().quit(1 if fails > 0 else 0)

func _view() -> void:
	# One physics step a drawn frame, so the walker moves in step with the gas (Flow steps once a frame).
	Engine.max_physics_steps_per_frame = 1
	_meadow(true)
	if view == "mist":
		# Looking down into the hollow towards a low sun: the mist glows forward.
		_sun_at(7.0, 180.0)
		cam.global_position = Vector3(3, height(3, 6) + 1.7, 6)
		cam.look_at(Vector3(0, height(0, -14) + 0.4, -14))
		# The wide haze past the near patch is the environment's, as in a scene: a light depth fog
		# (a height fog here draws a flat white disc over the hollow).
		var env: Environment = (get_node("Env") as WorldEnvironment).environment
		env.fog_enabled = true
		env.fog_light_color = Color(0.75, 0.72, 0.68)
		env.fog_density = 0.003
		if not mist_off:
			mist = _mist_node(1.0)
			mist.mist_range = 30.0
		await _frames(maxi(frames - 150, 10))
		_walk(Vector2(-8, -10), Vector2(8, -13), 1.3)
	else:
		_sun_at(25.0, 230.0)
		pollen = VegetationPollen3D.new()
		pollen.name = "Pollen"
		pollen.season = {"forcing": 900.0, "day_of_year": 170}
		add_child(pollen)
		pollen.vegetation_field = pollen.get_path_to(field)
		await _frames(20)
		_walk(Vector2(-4, 2), Vector2(4, 2), 1.4)
		cam.global_position = Vector3(1.0, height(1, 5) + 1.1, 5.0)
		cam.look_at(Vector3(0, height(0, 2) + 0.6, 2.0))
	await _frames(150 if view == "mist" else frames)
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

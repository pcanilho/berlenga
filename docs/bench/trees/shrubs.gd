extends Node3D
# Heath and scrub shrubs bench (TreeGenerator.make_shrub_species, docs/ideas/trees.md 5u, 5v).
#
# godot --path docs/bench/trees -- --shrubs [options]
#   --test             checks (PASS/FAIL), quit
#   --out=path.png     render the forms of each species in rows (raster unless --pt), then quit
#   --presets=a,b      species (default 31,32: gorse, broom; 33-35 gum rockrose, kermes oak, lentisk; 36 dwarf fan palm)
#   --forms=a,b        TreeGenerator.ShrubForm values (default all: young, mature, old, thicket, cushion)
#   --day=N            day of year (default 100)
#   --pt --spp=N       path traced
#   CAM="x,y,z,lx,ly,lz,fov" camera

var fails := 0

func _check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		fails += 1

func _aabb(m: Mesh) -> AABB:
	return m.get_aabb()

func run(args: Dictionary) -> void:
	var day := int(args.get("day", "100"))
	var season: Dictionary = load("res://main.gd").weather_season(day)
	var presets: Array = [31, 32]
	if args.has("presets"):
		presets = Array(String(args.presets).split(",")).map(func(v): return int(v))
	var forms: Array = [0, 1, 2, 3, 4]
	if args.has("forms"):
		forms = Array(String(args.forms).split(",")).map(func(v): return int(v))
	if args.has("test"):
		_test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	var z := 0.0
	var width := 0.0
	for p in presets:
		var x := 0.0
		for f in forms:
			var t0 := Time.get_ticks_msec()
			var sp: PlantSpecies = TreeGenerator.make_shrub_species(p, f, 1, season)
			var m: Mesh = sp.meshes[0]
			var bb := m.get_aabb()
			var tris := 0
			for s in m.get_surface_count():
				tris += m.surface_get_array_index_len(s) / 3
			print("%s: %.2f m tall, %.2f x %.2f m, canopy %.2f m, %d tris, far %s, %d ms" % [sp.resource_name if sp.resource_name != "" else sp.get_name(), bb.end.y, bb.size.x, bb.size.z, sp.canopy_radius, tris, sp.far_color, Time.get_ticks_msec() - t0])
			var mi := MeshInstance3D.new()
			mi.mesh = m
			mi.position = Vector3(x - bb.position.x, 0, z)
			add_child(mi)
			x += bb.size.x + 0.8
		width = maxf(width, x)
		z -= 3.5
	_world(args)
	var cam := Camera3D.new()
	cam.fov = 40.0
	add_child(cam)
	if OS.has_environment("CAM"):
		var cv := OS.get_environment("CAM").split_floats(",")
		cam.position = Vector3(cv[0], cv[1], cv[2])
		cam.look_at(Vector3(cv[3], cv[4], cv[5]))
		cam.fov = cv[6]
	else:
		cam.position = Vector3(width * 0.5, 2.2, width * 0.75 + 2.0)
		cam.look_at(Vector3(width * 0.5, 0.6, z * 0.5 + 1.75))
	for i in 4:
		await RenderingServer.frame_post_draw
	if args.has("pt"):
		for k in int(args.get("spp", "16")) / 4 + 2:
			await RenderingServer.frame_post_draw
	var out := String(args.get("out", "/tmp/shrubs.png"))
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(0)

func _world(args: Dictionary) -> void:
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(200, 200)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)
	var sk := Sky.new()
	sk.sky_material = ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sk
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	if args.has("pt"):
		env.pathtracing_enabled = true
		env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
		env.pathtracing_max_bounces = 3
	get_viewport().msaa_3d = Viewport.MSAA_8X
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 35, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)

func _test() -> void:
	var main := load("res://main.gd")
	var april: Dictionary = main.weather_season(100)
	var may: Dictionary = main.weather_season(150)
	var aug: Dictionary = main.weather_season(225)
	# Flowering seasons: gorse's flush Feb-May with a few all year; broom May-June.
	_check(TreeGenerator.get_bloom(31, 100) > 0.9 and TreeGenerator.get_bloom(31, 250) > 0.0 and TreeGenerator.get_bloom(31, 250) < 0.1, "gorse in flower in April, a few in September (%.2f, %.2f)" % [TreeGenerator.get_bloom(31, 100), TreeGenerator.get_bloom(31, 250)])
	_check(TreeGenerator.get_bloom(32, 150) > 0.9 and TreeGenerator.get_bloom(32, 100) == 0.0 and TreeGenerator.get_bloom(32, 225) == 0.0, "broom in flower in late May only")
	_check(float(TreeGenerator.get_leaf_season(31, april).get("flowering", 0.0)) > 0.9, "get_leaf_season gives gorse's flowering")
	var heights := {}
	for p in [31, 32, 33, 34, 35, 36]:
		for f in [0, 1, 2, 3, 4]:
			var sp: PlantSpecies = TreeGenerator.make_shrub_species(p, f, 1, may)
			var m: Mesh = sp.meshes[0]
			var bb := m.get_aabb()
			heights[[p, f]] = bb.end.y
			_check(sp.footprint > 0.0 and sp.canopy_radius > 0.0, "%s form %d: footprint %.2f m, canopy %.2f m" % [sp.get_name(), f, sp.footprint, sp.canopy_radius])
			# No climate or terrain rules: vegetation sets them.
			_check(sp.get_rule_range(PlantSpecies.RULE_TEMPERATURE) == PlantSpecies.new().get_rule_range(PlantSpecies.RULE_TEMPERATURE), "%s form %d: no climate rules" % [sp.get_name(), f])
	_check(heights[[31, 4]] < 0.6, "gorse cushion under 60 cm (%.2f m)" % heights[[31, 4]])
	_check(heights[[31, 0]] < heights[[31, 1]] and heights[[32, 0]] < heights[[32, 1]], "young shrubs smaller than mature")
	_check(heights[[31, 1]] > 0.9 and heights[[31, 1]] < 2.6 and heights[[32, 1]] > 1.0 and heights[[32, 1]] < 2.6, "mature gorse %.2f m and broom %.2f m in 1-2.5 m" % [heights[[31, 1]], heights[[32, 1]]])
	_check(heights[[34, 4]] < 0.6 and heights[[35, 4]] < 0.6, "kermes oak and lentisk cushions under 60 cm (%.2f, %.2f m)" % [heights[[34, 4]], heights[[35, 4]]])
	_check(heights[[33, 0]] < heights[[33, 1]] and heights[[34, 0]] < heights[[34, 1]] and heights[[35, 0]] < heights[[35, 1]], "young scrub smaller than mature")
	_check(heights[[33, 1]] > 0.9 and heights[[33, 1]] < 2.6, "mature gum rockrose %.2f m in 1-2.5 m" % heights[[33, 1]])
	_check(heights[[34, 1]] > 0.5 and heights[[34, 1]] < 2.3, "mature kermes oak %.2f m in 0.5-2.2 m" % heights[[34, 1]])
	_check(heights[[35, 1]] > 0.9 and heights[[35, 1]] < 3.1, "mature lentisk %.2f m in 1-3 m" % heights[[35, 1]])
	_check(heights[[36, 4]] < 0.6 and heights[[36, 0]] < heights[[36, 1]] and heights[[36, 1]] < heights[[36, 2]], "dwarf fan palm: cushion %.2f m under 60 cm, young %.2f < mature %.2f < old %.2f m" % [heights[[36, 4]], heights[[36, 0]], heights[[36, 1]], heights[[36, 2]]])
	_check(heights[[36, 1]] > 0.8 and heights[[36, 1]] < 2.6, "mature dwarf fan palm %.2f m in 0.8-2.5 m" % heights[[36, 1]])
	_check(TreeGenerator.get_bloom(33, 125) > 0.9 and TreeGenerator.get_bloom(33, 225) == 0.0 and TreeGenerator.get_bloom(33, 40) == 0.0, "gum rockrose in flower in May only")
	var jara_may: PlantSpecies = TreeGenerator.make_shrub_species(33, 1, 1, main.weather_season(125))
	var jara_aug: PlantSpecies = TreeGenerator.make_shrub_species(33, 1, 1, aug)
	_check(jara_may.far_color.b > jara_aug.far_color.b + 0.05, "gum rockrose far colour whitened in flower (%s vs %s)" % [jara_may.far_color, jara_aug.far_color])
	var flowering: PlantSpecies = TreeGenerator.make_shrub_species(31, 1, 1, april)
	var summer: PlantSpecies = TreeGenerator.make_shrub_species(31, 1, 1, aug)
	_check(flowering.far_color.r > summer.far_color.r + 0.1, "gorse far colour gold in flower (%s vs %s)" % [flowering.far_color, summer.far_color])

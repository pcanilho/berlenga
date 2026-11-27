extends Node3D
# Heath and scrub shrubs bench (TreeGenerator.make_shrub_species, docs/ideas/trees.md 5u, 5v).
#
# godot --path docs/bench/trees -- --shrubs [options]
#   --test             checks (PASS/FAIL), quit
#   --out=path.png     render the forms of each species in rows (raster unless --pt), then quit
#   --presets=a,b      species (default 31,32: gorse, broom; 33-35 gum rockrose, kermes oak, lentisk; 36 dwarf fan palm)
#   --forms=a,b        TreeGenerator.ShrubForm values (default all: young, mature, old, thicket, cushion; 5 hedge, 6 laid hedge)
#   --day=N            day of year (default 100)
#   --variants=N       N variants of each form side by side (default 1)
#   --row=M            metres between species rows (default 3.5)
#   --sand             sand-coloured ground (desert shrubs)
#   --pt --spp=N       path traced
#   --near            the near mesh (full detail) in place of the far one
#   --lod-report      triangles of each surface and of its LOD levels
#   --lod_threshold=T the mesh LOD threshold in pixels (0: full detail)
#   CAM="x,y,z,lx,ly,lz,fov" camera

var fails := 0

func _check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		fails += 1

func _low_foliage(m: Mesh, p_frac: float) -> float:
	var b := m.get_aabb()
	var cut := b.position.y + b.size.y * p_frac
	var low := 0
	var all := 0
	for s in m.get_surface_count():
		if not (m.surface_get_name(s) in ["leaves", "needles"]):
			continue
		for v in m.surface_get_arrays(s)[Mesh.ARRAY_VERTEX]:
			all += 1
			if v.y < cut:
				low += 1
	return float(low) / maxf(all, 1.0)

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
			var nv := int(args.get("variants", "1"))
			var sp: PlantSpecies = TreeGenerator.make_shrub_species(p, f, nv, season)
			# Living variants only (the fire band variants follow them in the list).
			for vi in TreeGenerator.get_living_meshes(sp).size():
				x = await _place(sp, vi, x, z, args, t0)
		width = maxf(width, x)
		z -= float(args.get("row", "3.5"))
	await _after_place(args, width, z)

func _place(sp: PlantSpecies, vi: int, x: float, z: float, args: Dictionary, t0: int) -> float:
			var m: Mesh = sp.meshes[vi]
			if args.has("near"):
				# The near mesh the field swaps in within near_range (built on demand).
				var src: Object = sp.near_mesh_source
				var nm: Mesh = null
				for k in 1200:
					if src == null:
						break
					nm = src.request_near_mesh(sp.near_mesh_key, 0)
					if nm != null:
						break
					await get_tree().create_timer(0.1).timeout
				if nm != null:
					m = nm
				else:
					print("no near mesh for ", sp.get_name())
			var bb := m.get_aabb()
			var tris := 0
			for s in m.get_surface_count():
				tris += m.surface_get_array_index_len(s) / 3
			print("%s: %.2f m tall, %.2f x %.2f m, canopy %.2f m, %d tris, far %s, %d ms" % [sp.resource_name if sp.resource_name != "" else sp.get_name(), bb.end.y, bb.size.x, bb.size.z, sp.canopy_radius, tris, sp.far_color, Time.get_ticks_msec() - t0])
			if args.has("lod-report"):
				# --lod-report: each surface's vertices, triangles and the triangles of its LOD levels.
				for s in m.get_surface_count():
					var sd: Dictionary = RenderingServer.mesh_get_surface(m.get_rid(), s)
					var ib := 4 if sd.vertex_count > 65535 else 2
					var ls: Array = []
					for l in sd.get("lods", []):
						ls.append(l.index_data.size() / ib / 3)
					print("  %s: %d verts, %d tris, lods %s" % [m.surface_get_name(s), sd.vertex_count, m.surface_get_array_index_len(s) / 3, str(ls)])
			var mi := MeshInstance3D.new()
			mi.mesh = m
			mi.position = Vector3(x - bb.position.x, 0, z)
			add_child(mi)
			x += bb.size.x + float(args.get("gap", "0.8"))
			return x

func _after_place(args: Dictionary, width: float, z: float) -> void:
	_world(args)
	if args.has("lod_threshold"):
		# 0 draws every mesh at LOD 0: the reference for a LOD comparison.
		get_viewport().mesh_lod_threshold = float(args.lod_threshold)
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
	print("saved ", out, " (", RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME), " primitives in the frame)")
	get_tree().quit(0)

func _world(args: Dictionary) -> void:
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(200, 200)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	if args.has("sand"):
		gm.albedo_color = Color(0.62, 0.5, 0.36) # Desert sand (a sand sea's quartz, iron-stained).
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
	# Gorse and broom are many-stemmed from the base and leafy to the ground (Stace 2019; Clements et al. 2001,
	# Ulex europaeus biology; Peterson and Prasad 1998, Cytisus scoparius): not a crown on a bare stem. Share of the
	# foliage vertices in the lower 40% of the height (the lollipops had 0-6%).
	for p in [31, 32]:
		for f in [1, 2]:
			var low := _low_foliage(TreeGenerator.make_shrub_species(p, f, 1, aug).meshes[0], 0.4)
			# A dome leafy to the ground has some 40% of its shell there; erect broom's old stems are bare green
			# switches low down, its leaves on the year's shoots, so less.
			_check(low > (0.3 if p == 31 else 0.1), "%s form %d: %.0f%% of the foliage in the lower 40%% of the height" % [["gorse", "broom"][p - 31], f, low * 100.0])
	_check(heights[[31, 4]] < 0.6, "gorse cushion under 60 cm (%.2f m)" % heights[[31, 4]])
	_check(heights[[31, 0]] < heights[[31, 1]] and heights[[32, 0]] < heights[[32, 1]], "young shrubs smaller than mature")
	_check(heights[[31, 1]] > 0.9 and heights[[31, 1]] < 2.6 and heights[[32, 1]] > 1.0 and heights[[32, 1]] < 2.6, "mature gorse %.2f m and broom %.2f m in 1-2.5 m" % [heights[[31, 1]], heights[[32, 1]]])
	_check(heights[[34, 4]] < 0.6 and heights[[35, 4]] < 0.6, "kermes oak and lentisk cushions under 60 cm (%.2f, %.2f m)" % [heights[[34, 4]], heights[[35, 4]]])
	_check(heights[[33, 0]] < heights[[33, 1]] and heights[[34, 0]] < heights[[34, 1]] and heights[[35, 0]] < heights[[35, 1]], "young scrub smaller than mature")
	_check(heights[[33, 1]] > 0.9 and heights[[33, 1]] < 2.6, "mature gum rockrose %.2f m in 1-2.5 m" % heights[[33, 1]])
	_check(heights[[34, 1]] > 0.5 and heights[[34, 1]] < 2.3, "mature kermes oak %.2f m in 0.5-2.2 m" % heights[[34, 1]])
	_check(heights[[35, 1]] > 0.9 and heights[[35, 1]] < 3.1, "mature lentisk %.2f m in 1-3 m" % heights[[35, 1]])
	_check(heights[[36, 4]] < 0.6 and heights[[36, 0]] < heights[[36, 1]] and heights[[36, 1]] < heights[[36, 2]], "dwarf fan palm: cushion %.2f m under 60 cm, young %.2f < mature %.2f < old %.2f m" % [heights[[36, 4]], heights[[36, 0]], heights[[36, 1]], heights[[36, 2]]])
	_check(TreeGenerator.get_bloom(36, 100) > 0.9 and TreeGenerator.get_bloom(36, 225) == 0.0, "dwarf fan palm in flower in April, not in August")
	_check(heights[[36, 1]] > 0.8 and heights[[36, 1]] < 2.6, "mature dwarf fan palm %.2f m in 0.8-2.5 m" % heights[[36, 1]])
	_check(TreeGenerator.get_bloom(33, 125) > 0.9 and TreeGenerator.get_bloom(33, 225) == 0.0 and TreeGenerator.get_bloom(33, 40) == 0.0, "gum rockrose in flower in May only")
	var jara_may: PlantSpecies = TreeGenerator.make_shrub_species(33, 1, 1, main.weather_season(125))
	var jara_aug: PlantSpecies = TreeGenerator.make_shrub_species(33, 1, 1, aug)
	_check(jara_may.far_color.b > jara_aug.far_color.b + 0.05, "gum rockrose far colour whitened in flower (%s vs %s)" % [jara_may.far_color, jara_aug.far_color])
	var flowering: PlantSpecies = TreeGenerator.make_shrub_species(31, 1, 1, april)
	var summer: PlantSpecies = TreeGenerator.make_shrub_species(31, 1, 1, aug)
	_check(flowering.far_color.r > summer.far_color.r + 0.1, "gorse far colour gold in flower (%s vs %s)" % [flowering.far_color, summer.far_color])
	# Gorse's flowers and pods thin with distance (trees.md 5x.2): the coarsest level keeps a
	# few per cent of them; drawn whole they were 1.7 million triangles at every distance.
	var gm: ArrayMesh = flowering.meshes[0]
	for si in gm.get_surface_count():
		if gm.surface_get_name(si) != "crown_parts":
			continue
		var sd: Dictionary = RenderingServer.mesh_get_surface(gm.get_rid(), si)
		var full := gm.surface_get_array_index_len(si)
		var lods: Array = sd.get("lods", [])
		var last := full
		if not lods.is_empty():
			last = lods[lods.size() - 1].index_data.size() / (4 if sd.vertex_count > 65535 else 2)
		_check(lods.size() >= 2 and last * 10 < full, "gorse flowers and pods thin with distance (%d of %d indices at the coarsest of %d levels)" % [last, full, lods.size()])
	# Hedges (trees.md 5ad): a two-metre segment along x, 1.6-2.7 m tall, no random yaw.
	for p in [6, 43, 5, 44, 45]:
		var hs: PlantSpecies = TreeGenerator.make_shrub_species(p, TreeGenerator.SHRUB_HEDGE, 1, aug)
		var hb := (hs.meshes[0] as Mesh).get_aabb()
		_check(hb.end.y > 1.6 and hb.end.y < 2.7 and hb.size.x > 1.8 and hb.size.x < 3.2 and float(hs.get_meta("hedge_segment_length", 0.0)) == 2.0 and not hs.random_yaw, "%s: %.2f m tall, %.2f m along the line, segment meta, no yaw" % [hs.get_name(), hb.end.y, hb.size.x])
	# Laid hedges (trees.md, laid hedges): pleached stems lie along the line, stakes and binders on top.
	for p in [6, 5]:
		var ls: PlantSpecies = TreeGenerator.make_shrub_species(p, TreeGenerator.SHRUB_LAID_HEDGE, 1, aug)
		var lb := (ls.meshes[0] as Mesh).get_aabb()
		_check(lb.end.y > 1.0 and lb.end.y < 2.6 and lb.size.x > 1.6 and lb.size.x < 3.0 and lb.size.z < 1.8 and float(ls.get_meta("hedge_segment_length", 0.0)) == 2.0 and not ls.random_yaw, "%s: %.2f m tall, %.2f m along, %.2f m deep, segment meta, no yaw" % [ls.get_name(), lb.end.y, lb.size.x, lb.size.z])
	_check(TreeGenerator.get_bloom(43, 95) > 0.9 and TreeGenerator.get_bloom(6, 135) > 0.9 and TreeGenerator.get_bloom(45, 170) > 0.9 and TreeGenerator.get_bloom(6, 200) == 0.0, "blackthorn in flower in April, hawthorn in May, dog rose in June")
	# Desert shrubs (trees.md 5ae): calligonum, white broom and saltbush, 1-2.5 m mature.
	var winter: Dictionary = main.weather_season(50)
	for p in [46, 47, 48]:
		var hs := {}
		for f in [0, 1, 3]:
			var ds: PlantSpecies = TreeGenerator.make_shrub_species(p, f, 1, april)
			_check(ds != null and ds.meshes.size() > 0 and ds.canopy_radius > 0.0, "%s form %d made" % [ds.get_name() if ds else str(p), f])
			hs[f] = (ds.meshes[0] as Mesh).get_aabb().end.y
		_check(hs[0] < hs[1] and hs[1] > 0.9 and hs[1] < 2.6 and hs[3] < 2.6, "desert shrub %d: young %.2f < mature %.2f m in 1-2.5 m, thicket %.2f m" % [p, hs[0], hs[1], hs[3]])
	_check(TreeGenerator.get_bloom(47, 50) > 0.9 and TreeGenerator.get_bloom(47, 150) == 0.0, "white broom in flower in February, not in June")
	var rf: PlantSpecies = TreeGenerator.make_shrub_species(47, 1, 1, winter)
	var rs: PlantSpecies = TreeGenerator.make_shrub_species(47, 1, 1, aug)
	_check(rf.far_color.b > rs.far_color.b + 0.15, "white broom far colour white in flower (%s vs %s)" % [rf.far_color, rs.far_color])
	# Calligonum's bristly fruit in April: crown parts, red.
	var cm: Mesh = TreeGenerator.make_shrub_species(46, 1, 1, april).meshes[0]
	var red := false
	for si in cm.get_surface_count():
		if cm.surface_get_name(si) == "crown_parts":
			var cols: PackedColorArray = cm.surface_get_arrays(si)[Mesh.ARRAY_COLOR]
			var rsum := 0.0
			var gsum := 0.0
			for col in cols:
				rsum += col.r
				gsum += col.g
			red = cols.size() > 1000 and rsum > gsum * 2.0
	_check(red, "calligonum carries red bristly fruit in April")

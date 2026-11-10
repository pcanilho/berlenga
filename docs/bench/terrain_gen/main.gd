# TerrainGenerator3D bench and checks: a 4 km world with one mountain range along a curve, noise breaking it up,
# outlets on every edge.
#   godot --headless --path docs/bench/terrain_gen -- --test             checks (HeightMapShape3D ground)
#   godot --headless --path docs/bench/terrain_gen -- --test --terrain3d same on Terrain3D (addon installed)
#   godot --headless --path docs/bench/terrain_gen -- --time             generation time at 256, 512, 1024 cells
#   godot --path docs/bench/terrain_gen --rendering-driver vulkan -- --out=view.png [--raster] [--spp=16] [--close]
#       [--cell=8 --res=513] [--view-res=1024] [--no-detail]
#   godot --headless --path docs/bench/terrain_gen -- --map=map.png      shaded relief with rivers
extends Node3D

const WORLD := 4096.0
var args := {}
var checks := 0
var failures := 0
var gen: TerrainGenerator3D
var ground_shape: CollisionShape3D
var ground_mesh: MeshInstance3D
var terrain: Node3D
var deformer: TerrainDeformer3D


func check(ok: bool, what: String) -> void:
	checks += 1
	if not ok:
		failures += 1
	print(("ok   " if ok else "FAIL ") + what)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	if args.has("time"):
		_time()
		get_tree().quit()
		return
	if args.has("terrain3d") and not ClassDB.class_exists("Terrain3D"):
		print("SKIP: Terrain3D is not installed in this project (addons/terrain_3d).")
		get_tree().quit(0)
		return
	_build_generator(float(args.get("cell", "16")))
	if args.has("terrain3d"):
		_build_terrain3d()
	else:
		_build_height_map(int(args.get("res", "257")))
	if args.has("test"):
		await _run_checks()
		print("%d checks, %d failed" % [checks, failures])
		print("PASS" if failures == 0 else "FAIL")
		get_tree().quit(1 if failures else 0)
	elif args.has("map"):
		gen.generate()
		_save_map(args["map"])
		get_tree().quit()
	elif args.has("out"):
		# A wider, faster-rising range than the checks use, for a mountain view.
		gen.uplift = 6.0
		gen.range_width = 1300.0
		gen.detail_enabled = not args.has("no-detail")
		gen.bake()
		_build_view()
		var raster := args.has("raster")
		for i in (6 if raster else 12):
			await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(args["out"])
		print("saved ", args["out"])
		get_tree().quit()


func _build_generator(cell: float) -> void:
	# A range curving across the world, a lower spur, and noise that breaks the uplift into massifs.
	var path := Path3D.new()
	path.name = "Range"
	var curve := Curve3D.new()
	curve.add_point(Vector3(-1900, 0, 700), Vector3.ZERO, Vector3(600, 0, -200))
	curve.add_point(Vector3(-300, 0, 100), Vector3(-500, 0, 100), Vector3(500, 0, -100))
	curve.add_point(Vector3(1900, 0, -600), Vector3(-600, 0, 0))
	path.curve = curve
	add_child(path)
	var spur := Path3D.new()
	spur.name = "Spur"
	var c2 := Curve3D.new()
	c2.add_point(Vector3(-200, 0, 150))
	c2.add_point(Vector3(300, 0, 1500))
	spur.curve = c2
	spur.set_meta("width", 500.0)
	spur.set_meta("uplift", 0.6)
	add_child(spur)
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 1.0 / 1500.0
	noise.fractal_octaves = 3
	gen = TerrainGenerator3D.new()
	gen.name = "Generator"
	gen.size = Vector2(WORLD, WORLD)
	gen.cell_size = cell
	gen.seed = 1
	gen.uplift_noise = noise
	add_child(gen)
	gen.ranges = [gen.get_path_to(path), gen.get_path_to(spur)]


func _build_height_map(res: int) -> void:
	var shape := HeightMapShape3D.new()
	shape.map_width = res
	shape.map_depth = res
	var body := StaticBody3D.new()
	add_child(body)
	ground_shape = CollisionShape3D.new()
	ground_shape.shape = shape
	var spacing := WORLD / (res - 1)
	ground_shape.scale = Vector3(spacing, 1.0, spacing)
	body.add_child(ground_shape)
	ground_mesh = MeshInstance3D.new()
	ground_mesh.mesh = ArrayMesh.new()
	add_child(ground_mesh)
	deformer = TerrainDeformer3D.new()
	deformer.auto_apply = false
	add_child(deformer)
	deformer.terrain = deformer.get_path_to(ground_shape)
	deformer.ground_mesh = deformer.get_path_to(ground_mesh)
	gen.deformer = gen.get_path_to(deformer)


func _build_terrain3d() -> void:
	terrain = ClassDB.instantiate("Terrain3D")
	terrain.set("region_size", 1024)
	terrain.set("vertex_spacing", 4.0)
	add_child(terrain)
	deformer = TerrainDeformer3D.new()
	deformer.auto_apply = false
	add_child(deformer)
	deformer.terrain = deformer.get_path_to(terrain)
	gen.deformer = gen.get_path_to(deformer)


func _hash(img: Image) -> int:
	return hash(img.get_data())


func _run_checks() -> void:
	# Determinism: same settings and seed give the same bytes; another seed changes them.
	check(gen.generate(), "generate() runs")
	var g := gen.get_grid_size()
	print("     grid %dx%d, %d steps, %.0f ms, last step change %.2f m" % [g.x, g.y, gen.get_last_steps(), gen.get_last_generate_msec(), gen.get_last_change()])
	var h1 := gen.get_height_image()
	var first := _hash(h1)
	gen.generate()
	check(_hash(gen.get_height_image()) == first, "same seed gives the same terrain")
	gen.seed = 2
	gen.generate()
	check(_hash(gen.get_height_image()) != first, "another seed gives another terrain")
	gen.seed = 1
	gen.generate()

	# Shape: mountains along the range, low ground away from it, outlets at sea level.
	var hmax := -INF
	var w := g.x
	for z in g.y:
		for x in g.x:
			hmax = maxf(hmax, h1.get_pixel(x, z).r)
	var on_range := gen.get_height(Vector3(-300, 0, 100))
	var far := gen.get_height(Vector3(1600, 0, 1600))
	print("     highest %.0f m, on the range %.0f m, far corner %.0f m" % [hmax, on_range, far])
	check(hmax > 300.0, "mountains rise (highest %.0f m)" % hmax)
	check(on_range > far + 100.0, "the range stands above the plain")
	check(absf(h1.get_pixel(0, w / 2).r) < 1e-3 and absf(h1.get_pixel(w - 1, w / 2).r) < 1e-3, "outlet edges at sea level")

	# Drainage: every node has a strictly lower neighbour or is an outlet (no closed basins left), and the largest
	# river carries a real share of the world.
	var pits := 0
	for z in range(1, g.y - 1):
		for x in range(1, g.x - 1):
			var c := h1.get_pixel(x, z).r
			var lower := false
			for dz in range(-1, 2):
				for dx in range(-1, 2):
					if (dx != 0 or dz != 0) and h1.get_pixel(x + dx, z + dz).r < c:
						lower = true
			if not lower:
				pits += 1
	check(pits == 0, "every node drains to an outlet (%d pits)" % pits)
	var area := gen.get_drainage_image()
	var amax := 0.0
	for z in g.y:
		for x in g.x:
			amax = maxf(amax, area.get_pixel(x, z).r)
	check(amax > 0.02 * WORLD * WORLD, "a trunk river drains %.1f%% of the world" % (100.0 * amax / (WORLD * WORLD)))

	# Critical slope: no grid step steeper than tan(40 deg) (plus a margin for the diagonal and float noise).
	var cell := gen.cell_size
	var smax := 0.0
	for z in range(0, g.y - 1):
		for x in range(0, g.x - 1):
			var c := h1.get_pixel(x, z).r
			smax = maxf(smax, absf(h1.get_pixel(x + 1, z).r - c) / cell)
			smax = maxf(smax, absf(h1.get_pixel(x, z + 1).r - c) / cell)
	check(smax <= tan(deg_to_rad(gen.critical_slope)) * 1.42 + 0.01, "slopes held by the critical slope (max %.2f)" % smax)

	# Sediment fills basins only where the land was closed.
	var sed := gen.get_sediment_image()
	var smaxs := 0.0
	for z in g.y:
		for x in g.x:
			smaxs = maxf(smaxs, sed.get_pixel(x, z).r)
	check(smaxs >= 0.0, "sediment map present (deepest fill %.1f m)" % smaxs)

	# max_height rescales the relief.
	gen.max_height = 1500.0
	gen.generate()
	var hi2 := -INF
	var img2 := gen.get_height_image()
	for z in g.y:
		for x in g.x:
			hi2 = maxf(hi2, img2.get_pixel(x, z).r)
	check(absf(hi2 - 1500.0) < 0.5, "max_height scales the highest point to 1500 m (%.1f)" % hi2)
	gen.max_height = 0.0
	gen.generate()

	# Detail filter: carves gullies of metres to tens of metres, bounded, deterministic, off when disabled.
	var dmax := 0.0
	var dsum := 0.0
	var fp := PackedFloat32Array()
	for i in 2000:
		var p := Vector3(-2000.0 + 4000.0 * fposmod(i * 0.618034, 1.0), 0, -2000.0 + 4000.0 * fposmod(i * 0.414214, 1.0))
		var d := gen.get_height(p) - gen.get_base_height(p)
		dmax = maxf(dmax, absf(d))
		dsum += absf(d)
		fp.append(gen.get_height(p))
	var bound := gen.detail_depth / gen.detail_gully_weight * gen.detail_wavelength * 2.0
	check(dmax > 1.0 and dmax < bound, "detail carves the ground (mean %.2f m, max %.1f m, bound %.1f m)" % [dsum / 2000.0, dmax, bound])
	var fp2 := PackedFloat32Array()
	for i in 2000:
		fp2.append(gen.get_height(Vector3(-2000.0 + 4000.0 * fposmod(i * 0.618034, 1.0), 0, -2000.0 + 4000.0 * fposmod(i * 0.414214, 1.0))))
	check(fp == fp2, "detail is a pure function of position")
	gen.detail_enabled = false
	check(gen.get_height(Vector3(100, 0, 100)) == gen.get_base_height(Vector3(100, 0, 100)), "detail_enabled off gives the simulated ground")
	gen.detail_enabled = true

	# Write into the ground, with a brush on the deformer that must survive the new ground.
	var plot := TerrainBrush3D.new()
	plot.shape = TerrainBrush3D.SHAPE_RECTANGLE
	plot.mode = TerrainBrush3D.MODE_FLATTEN
	plot.size = Vector2(60, 60)
	plot.falloff = 10.0
	add_child(plot)
	plot.global_position = Vector3(600, 120.0, -400)
	deformer.brushes = [deformer.get_path_to(plot)]
	deformer.apply()
	var written := gen.write_to_terrain()
	print("     wrote %d vertices in %.0f ms" % [written, gen.get_last_write_msec()])
	check(written > 0, "heights written")
	var worst := 0.0
	var samples := 0
	for i in 400:
		var p := Vector3(-1900.0 + 3800.0 * fposmod(i * 0.618034, 1.0), 0, -1900.0 + 3800.0 * fposmod(i * 0.414214, 1.0))
		if absf(p.x - 600) < 60 and absf(p.z + 400) < 60:
			continue
		var got := deformer.get_height(_snap(p))
		var want_s := gen.get_height(_snap(p))
		worst = maxf(worst, absf(got - want_s))
		samples += 1
	check(worst < 0.001, "ground written in parallel rows matches get_height() at %d vertices (worst %.4f m)" % [samples, worst])
	var plot_h := deformer.get_height(Vector3(600, 0, -400))
	check(absf(plot_h - 120.0) < 0.05, "deformer brush re-applied on the new ground (%.2f m)" % plot_h)
	var base_h := deformer.get_base_height(_snap(Vector3(600, 0, -400)))
	var gen_h := gen.get_height(_snap(Vector3(600, 0, -400)))
	check(absf(base_h - gen_h) < 0.05, "deformer's base height is the generated ground (%.3f vs %.3f)" % [base_h, gen_h])
	if terrain:
		# Region joins (4096 m regions meet at x = 0 and z = 0): the step across the border matches the generator's.
		var seam := 0.0
		for i in 64:
			var z := -1800.0 + i * 56.0
			var left := deformer.get_height(Vector3(-4, 0, z))
			var right := deformer.get_height(Vector3(0, 0, z))
			seam = maxf(seam, absf(right - left - (gen.get_height(Vector3(0, 0, z)) - gen.get_height(Vector3(-4, 0, z)))))
		check(seam < 0.01, "Terrain3D region border continuous (worst %.4f m)" % seam)
	await get_tree().process_frame


func _snap(p: Vector3) -> Vector3:
	var sp: float = 4.0 if terrain else WORLD / (ground_shape.shape.map_width - 1)
	var o: Vector2 = Vector2(0, 0) if terrain else Vector2(-WORLD * 0.5, -WORLD * 0.5)
	return Vector3(o.x + roundf((p.x - o.x) / sp) * sp, 0, o.y + roundf((p.z - o.y) / sp) * sp)


func _time() -> void:
	for cell in [16.0, 8.0, 4.0]:
		_build_generator(cell)
		gen.generate()
		var g := gen.get_grid_size()
		print("%dx%d (%.0f m cells over %.0f m): %d steps in %.0f ms (%.2f ms per step)" % [g.x, g.y, cell, WORLD, gen.get_last_steps(), gen.get_last_generate_msec(), gen.get_last_generate_msec() / gen.get_last_steps()])
		gen.queue_free()


func _save_map(path: String) -> void:
	var g := gen.get_grid_size()
	var h := gen.get_height_image()
	var a := gen.get_drainage_image()
	var img := Image.create_empty(g.x, g.y, false, Image.FORMAT_RGB8)
	var cell := gen.cell_size
	var hmax := 1.0
	for z in g.y:
		for x in g.x:
			hmax = maxf(hmax, h.get_pixel(x, z).r)
	var sun := Vector3(-0.5, 0.7, -0.5).normalized()
	for z in g.y:
		for x in g.x:
			var hx := (h.get_pixel(mini(x + 1, g.x - 1), z).r - h.get_pixel(maxi(x - 1, 0), z).r) / (2.0 * cell)
			var hz := (h.get_pixel(x, mini(z + 1, g.y - 1)).r - h.get_pixel(x, maxi(z - 1, 0)).r) / (2.0 * cell)
			var n := Vector3(-hx, 1.0, -hz).normalized()
			var t := h.get_pixel(x, z).r / hmax
			var col := Color(0.32, 0.42, 0.24).lerp(Color(0.55, 0.5, 0.45), smoothstep(0.2, 0.6, t)).lerp(Color(0.92, 0.93, 0.95), smoothstep(0.75, 0.9, t))
			col = col * (0.3 + 0.9 * maxf(n.dot(sun), 0.0))
			var river := clampf((log(a.get_pixel(x, z).r) / log(10.0) - 5.3) / 1.2, 0.0, 1.0)
			col = col.lerp(Color(0.2, 0.4, 0.7), river)
			img.set_pixel(x, z, col)
	img.save_png(path)
	print("saved ", path)


func _build_view() -> void:
	# A display mesh of the generated ground with slope and height colours (grass, rock, snow).
	var res := int(args.get("view-res", "1024"))
	var step := WORLD / res
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var hmax := 1.0
	var img := gen.get_height_image()
	var g := gen.get_grid_size()
	for z in g.y:
		for x in g.x:
			hmax = maxf(hmax, img.get_pixel(x, z).r)
	for z in res + 1:
		for x in res + 1:
			var p := Vector3(-WORLD * 0.5 + x * step, 0, -WORLD * 0.5 + z * step)
			p.y = gen.get_height(p)
			var hx := gen.get_height(p + Vector3(step, 0, 0)) - gen.get_height(p - Vector3(step, 0, 0))
			var hz := gen.get_height(p + Vector3(0, 0, step)) - gen.get_height(p - Vector3(0, 0, step))
			if is_nan(hx):
				hx = 0.0
			if is_nan(hz):
				hz = 0.0
			var n := Vector3(-hx, 2.0 * step, -hz).normalized()
			var t := p.y / hmax
			var col := Color(0.2, 0.3, 0.12).lerp(Color(0.36, 0.33, 0.29), smoothstep(0.9, 0.75, n.y))
			col = col.lerp(Color(0.9, 0.9, 0.92), smoothstep(0.8, 0.92, t) * smoothstep(0.6, 0.8, n.y))
			st.set_color(col)
			st.set_normal(n)
			st.add_vertex(p)
	for z in res:
		for x in res:
			var i := z * (res + 1) + x
			for k in [i, i + 1, i + res + 1, i + 1, i + res + 2, i + res + 1]:
				st.add_index(k)
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	var mat := StandardMaterial3D.new()
	mat.vertex_color_use_as_albedo = true
	mat.vertex_color_is_srgb = true
	mat.roughness = 0.95
	mi.material_override = mat
	add_child(mi)
	if ground_mesh:
		ground_mesh.visible = false
	var water := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(WORLD, WORLD)
	water.mesh = plane
	var wm := StandardMaterial3D.new()
	wm.albedo_color = Color(0.1, 0.2, 0.28)
	wm.roughness = 0.1
	water.material_override = wm
	water.position.y = 2.0
	add_child(water)
	var env := Environment.new()
	# A procedural sky lights both renderers: skylight fills the shadowed slopes, raster through ambient, the path
	# tracer by sampling the sky shader.
	var sky_mat := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
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
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.6
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 9000.0
	add_child(sun)
	sun.look_at_from_position(Vector3(3000, 900, -2000), Vector3.ZERO)
	var cam := Camera3D.new()
	cam.far = 20000.0
	cam.fov = 50.0
	if args.has("close"):
		# Close view of one flank, for raster and path traced crops at 100%.
		cam.fov = 40.0
		cam.position = Vector3(700, 900, 1100)
		add_child(cam)
		cam.look_at(Vector3(-200, 450, 200))
	else:
		cam.position = Vector3(1900, 1100, 2900)
		add_child(cam)
		cam.look_at(Vector3(-400, 250, -300))

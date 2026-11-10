# TerrainGenerator3D bench and checks: a 4 km world with one mountain range along a curve, noise breaking it up,
# outlets on every edge.
#   godot --headless --path docs/bench/terrain_gen -- --test             checks (HeightMapShape3D ground)
#   godot --headless --path docs/bench/terrain_gen -- --test --terrain3d same on Terrain3D (addon installed)
#   godot --headless --path docs/bench/terrain_gen -- --time             generation time at 256, 512, 1024 cells
#   godot --path docs/bench/terrain_gen --rendering-driver vulkan -- --out=view.png [--raster] [--spp=16] [--close]
#       [--cell=8 --res=513] [--view-res=1024] [--no-detail] [--crest=1] [--tiles[=2]] [--mdetail=4]
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
	elif args.has("geology"):
		if args.has("view"):
			gen.uplift = 6.0
			gen.range_width = 1300.0
		gen.generate()
		_save_geology(args["geology"])
		if args.has("dump"):
			var fd := FileAccess.open(args["dump"], FileAccess.WRITE)
			fd.store_buffer(gen.get_height_image().get_data())
			fd.close()
		get_tree().quit()
	elif args.has("map"):
		gen.generate()
		_save_map(args["map"])
		get_tree().quit()
	elif args.has("out"):
		# A wider, faster-rising range than the checks use, for a mountain view.
		gen.uplift = 6.0
		gen.range_width = 1300.0
		gen.detail_enabled = not args.has("no-detail")
		if args.has("crest"):
			gen.detail_crest = float(args["crest"])
		if args.has("tiles"):
			gen.tile_erosion_enabled = true
			gen.tile_erosion_spacing = float(args["tiles"]) if args["tiles"] != "1" else 2.0
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
	var bound := gen.detail_depth / gen.detail_gully_weight * gen.detail_wavelength * (2.0 + gen.detail_crest)
	check(dmax > 1.0 and dmax < bound, "detail carves the ground (mean %.2f m, max %.1f m, bound %.1f m)" % [dsum / 2000.0, dmax, bound])
	var fp2 := PackedFloat32Array()
	for i in 2000:
		fp2.append(gen.get_height(Vector3(-2000.0 + 4000.0 * fposmod(i * 0.618034, 1.0), 0, -2000.0 + 4000.0 * fposmod(i * 0.414214, 1.0))))
	check(fp == fp2, "detail is a pure function of position")
	# Crests: the gullies fade to a share where opposite slopes meet (slope coherence), so they do not notch a crest
	# into a regular comb, and a ridged noise gives the crest irregular pinnacles and cols instead. The comb is
	# measured with the crest noise off: the second difference of the offset over 2 m steps along crests running
	# north to south, against the same across the gullies of slopes facing east or west (without the fade about 7
	# times as much). Then the crest noise must move the crests.
	var crest_amount := gen.detail_crest
	gen.detail_crest = 0.0
	var hgrid := gen.get_height_image()
	var gs := gen.get_grid_size()
	var cs := gen.cell_size
	var o := func(q: Vector3) -> float: return gen.get_height(q) - gen.get_base_height(q)
	var crest_bend := 0.0
	var crest_n := 0
	var slope_bend := 0.0
	var slope_n := 0
	for z in range(4, gs.y - 4):
		for x in range(4, gs.x - 4):
			var h0 := hgrid.get_pixel(x, z).r
			var dl := h0 - hgrid.get_pixel(x - 1, z).r
			var dr := h0 - hgrid.get_pixel(x + 1, z).r
			var dz := (hgrid.get_pixel(x, z + 1).r - hgrid.get_pixel(x, z - 1).r) * 0.5
			var pc := Vector3(-WORLD * 0.5 + x * cs, 0, -WORLD * 0.5 + z * cs)
			var bend := func() -> float: return absf(o.call(pc + Vector3(0, 0, 2)) - 2.0 * o.call(pc) + o.call(pc - Vector3(0, 0, 2)))
			if dl > 0.1 * cs and dr > 0.1 * cs and absf(dz) < 0.1 * cs:
				crest_bend += bend.call()
				crest_n += 1
			elif x % 3 == 0 and z % 3 == 0 and absf(dl + dr) < 0.02 * cs and absf(dl) > 0.15 * cs and absf(dz) < 0.05 * cs:
				slope_bend += bend.call()
				slope_n += 1
	crest_bend /= maxi(crest_n, 1)
	slope_bend /= maxi(slope_n, 1)
	check(crest_n >= 10 and slope_n > 100 and crest_bend < 4.0 * slope_bend, "gullies fade on crests, no comb (bend %.2f m along %d crest points, %.2f m across %d slope points)" % [crest_bend, crest_n, slope_bend, slope_n])
	var crest_pts: Array[Vector3] = []
	for z in range(4, gs.y - 4):
		for x in range(4, gs.x - 4):
			var h0 := hgrid.get_pixel(x, z).r
			if h0 - hgrid.get_pixel(x - 1, z).r > 0.1 * cs and h0 - hgrid.get_pixel(x + 1, z).r > 0.1 * cs:
				crest_pts.append(Vector3(-WORLD * 0.5 + x * cs, 0, -WORLD * 0.5 + z * cs))
	var before := PackedFloat32Array()
	for q in crest_pts:
		before.append(gen.get_height(q))
	gen.detail_crest = crest_amount
	var moved := 0.0
	for i in crest_pts.size():
		moved += absf(gen.get_height(crest_pts[i]) - before[i])
	moved /= maxi(crest_pts.size(), 1)
	check(moved > 1.0, "crest noise shapes the crests (mean %.1f m at %d crest points)" % [moved, crest_pts.size()])
	# sample_heights gives the same ground as get_height.
	var tile := gen.sample_heights(Vector2(-300, 200), 2.0, Vector2i(16, 8))
	var tile_worst := 0.0
	for z in 8:
		for x in 16:
			tile_worst = maxf(tile_worst, absf(tile.get_pixel(x, z).r - gen.get_height(Vector3(-300 + 2.0 * x, 0, 200 + 2.0 * z))))
	check(tile_worst < 1e-4, "sample_heights matches get_height (worst %.6f m)" % tile_worst)
	gen.detail_enabled = false
	check(gen.get_height(Vector3(100, 0, 100)) == gen.get_base_height(Vector3(100, 0, 100)), "detail_enabled off gives the simulated ground")
	gen.detail_enabled = true

	# Geology: rock bare on steep ground, soil on gentle ground, scree under cliffs.
	var geo := gen.get_geology_image()
	check(geo != null and geo.get_width() == gs.x and geo.get_format() == Image.FORMAT_RGBAF, "geology map present")
	var rock_steep := 0.0
	var n_steep := 0
	var rock_flat := 0.0
	var soil_flat := 0.0
	var soil_steep := 0.0
	var n_flat := 0
	var talus_n := 0
	var talus_slope := 0.0
	var bad := 0
	var repose := tan(deg_to_rad(gen.talus_angle))
	for z in range(1, gs.y - 1):
		for x in range(1, gs.x - 1):
			var c := geo.get_pixel(x, z)
			if c.r < 0.0 or c.g < 0.0 or c.g > 1.0 or c.b < 0.0 or c.b > 1.0:
				bad += 1
			var h0 := hgrid.get_pixel(x, z).r
			var sl := maxf(maxf(absf(h0 - hgrid.get_pixel(x - 1, z).r), absf(h0 - hgrid.get_pixel(x + 1, z).r)), maxf(absf(h0 - hgrid.get_pixel(x, z - 1).r), absf(h0 - hgrid.get_pixel(x, z + 1).r))) / cs
			if sl > 0.6:
				rock_steep += c.g
				soil_steep += c.r
				n_steep += 1
			elif sl < 0.1:
				rock_flat += c.g
				soil_flat += c.r
				n_flat += 1
			if c.b > 0.3:
				talus_n += 1
				talus_slope += sl
	check(bad == 0, "geology values in range")
	rock_steep /= maxi(n_steep, 1)
	rock_flat /= maxi(n_flat, 1)
	soil_steep /= maxi(n_steep, 1)
	soil_flat /= maxi(n_flat, 1)
	check(rock_steep > 0.5 and rock_flat < 0.2, "rock bare on steep ground (%.2f), covered on gentle ground (%.2f)" % [rock_steep, rock_flat])
	check(soil_flat > 4.0 * soil_steep, "soil deeper on gentle ground (%.2f m) than steep (%.2f m)" % [soil_flat, soil_steep])
	talus_slope /= maxi(talus_n, 1)
	check(talus_n > 50 and talus_slope < repose, "scree lies below repose (%d cells, mean slope %.2f, repose %.2f)" % [talus_n, talus_slope, repose])
	var gc := gen.get_geology_at(Vector3(100, 0, 100))
	check(absf(gc.r - gen.get_soil_depth(Vector3(100, 0, 100))) < 1e-5 and absf(gc.g - gen.get_rock_exposure(Vector3(100, 0, 100))) < 1e-5, "get_geology_at agrees with the single queries")

	# Rivers: trunk first, downhill, widening downstream, every tributary ending on the river it joins.
	var rivers := gen.get_rivers(5.0e5)
	check(rivers.size() > 3, "river network extracted (%d rivers over 0.5 km2)" % rivers.size())
	var trunk: Dictionary = rivers[0]
	var most := 0.0
	for r in rivers:
		most = maxf(most, r["discharge"])
	check(trunk["joins"] == -1 and is_equal_approx(trunk["discharge"], most), "the first river is the largest (%.3f m3/s)" % trunk["discharge"])
	var downhill := true
	var widening := true
	var joined := true
	var own_sum := 0.0
	for ri in rivers.size():
		var r: Dictionary = rivers[ri]
		var pts: PackedVector3Array = r["points"]
		var wd: PackedFloat32Array = r["widths"]
		if pts[pts.size() - 1].y > pts[0].y + 1.0:
			downhill = false
		for k in range(1, wd.size()):
			if wd[k] < wd[k - 1] - 1e-4:
				widening = false
		if r["joins"] >= 0:
			var parent: PackedVector3Array = rivers[r["joins"]]["points"]
			var end := pts[pts.size() - 1]
			var best := INF
			for q in parent:
				best = minf(best, Vector2(q.x - end.x, q.z - end.z).length())
			if best > cs:
				joined = false
		own_sum += r["own_discharge"]
	check(downhill, "every river ends lower than it starts")
	check(widening, "widths never shrink downstream")
	check(joined, "every tributary ends on the river it joins")
	var outlet_sum := 0.0
	for r in rivers:
		if r["joins"] == -1:
			outlet_sum += r["discharge"]
	check(absf(own_sum - outlet_sum) < 1e-3 * outlet_sum, "own discharges add up to the outlets' (%.3f vs %.3f m3/s)" % [own_sum, outlet_sum])
	var holder := Node3D.new()
	add_child(holder)
	var paths := gen.build_river_paths(holder, 2.0e6)
	var n_big := gen.get_rivers(2.0e6).size()
	check(paths.size() == n_big and holder.get_child_count() == n_big and (paths[0] as Path3D).curve.point_count > 2, "build_river_paths makes one %s per river (%d)" % [(paths[0] as Node).get_class(), n_big])
	holder.queue_free()

	# Tile erosion (stage 4): droplet hydraulic and thermal erosion in overlapping tiles, cross-faded.
	gen.tile_erosion_enabled = true
	gen.tile_erosion_spacing = 4.0
	gen.tile_erosion_tile_size = 1024.0
	gen.tile_erosion_apron = 128.0
	gen.generate()
	print("     tile erosion %.0f ms" % gen.get_last_tile_erosion_msec())
	var t1 := gen.sample_heights(Vector2(-1500, -1500), 7.0, Vector2i(400, 400))
	gen.generate()
	var t2 := gen.sample_heights(Vector2(-1500, -1500), 7.0, Vector2i(400, 400))
	check(t1.get_data() == t2.get_data(), "tile erosion is deterministic")
	var tmax := 0.0
	var tsum := 0.0
	var tcount := 0
	for z in 80:
		for x in 80:
			var q := Vector3(-1900 + x * 47.5, 0, -1900 + z * 47.5)
			var d := gen.get_height(q)
			gen.tile_erosion_enabled = false
			d -= gen.get_height(q)
			gen.tile_erosion_enabled = true
			tmax = maxf(tmax, absf(d))
			tsum += absf(d)
			tcount += 1
	check(tmax > 0.5 and tmax <= gen.tile_erosion_max_depth * 1.5, "tile erosion changes the ground, bounded (mean %.2f m, max %.1f m)" % [tsum / tcount, tmax])
	# Tile borders (x = -1024, 0, 1024 from the corner at -2048): steps across them no larger than elsewhere.
	var at_border := 0.0
	var off_border := 0.0
	for i in 200:
		var z := -1900.0 + i * 19.0
		for bxw in [-1024.0, 0.0, 1024.0]:
			at_border = maxf(at_border, absf(gen.get_height(Vector3(bxw + 1.0, 0, z)) - gen.get_height(Vector3(bxw - 1.0, 0, z))))
			off_border = maxf(off_border, absf(gen.get_height(Vector3(bxw + 513.0, 0, z)) - gen.get_height(Vector3(bxw + 511.0, 0, z))))
	check(at_border <= off_border * 1.5, "no seam at tile borders (largest 2 m step %.2f m across borders, %.2f m inside tiles)" % [at_border, off_border])
	var wet_valley := 0.0
	var wet_ridge := 0.0
	var nv := 0
	var nr := 0
	for z in range(4, gs.y - 4, 2):
		for x in range(4, gs.x - 4, 2):
			var q := Vector3(-WORLD * 0.5 + x * cs, 0, -WORLD * 0.5 + z * cs)
			var a := gen.get_drainage_area(q)
			if a > 2.0e5:
				wet_valley += gen.get_tile_flow(q)
				nv += 1
			elif a < 2.0 * cs * cs:
				wet_ridge += gen.get_tile_flow(q)
				nr += 1
	wet_valley /= maxi(nv, 1)
	wet_ridge /= maxi(nr, 1)
	check(wet_valley > 3.0 * wet_ridge, "droplet flow gathers in valleys (%.2f against %.2f on ridges)" % [wet_valley, wet_ridge])
	gen.tile_erosion_enabled = false
	gen.generate()

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


const VIEW_SHADER := """
shader_type spatial;
// Grass on soil, bare rock, scree and snow, per pixel from the generator's geology map (terrain_geology(), published
// as shader globals), the world normal and height.
uniform float hmax = 1000.0;
uniform int detail_octaves = 0;
%s
%s
void fragment() {
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (detail_octaves > 0) {
		// Erosion detail below the vertex spacing; octaves under two pixels fade out.
		float footprint = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (PROJECTION_MATRIX[1][1] * VIEWPORT_SIZE.y);
		wn = terrain_detail_normal(wp, wn, detail_octaves, footprint);
		NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);
	}
	vec4 geo = terrain_geology(wp);
	vec3 col = mix(vec3(0.2, 0.3, 0.12), vec3(0.36, 0.33, 0.29), max(geo.y, smoothstep(0.85, 0.7, wn.y)));
	col = mix(col, vec3(0.5, 0.45, 0.38), geo.z);
	col = mix(col, vec3(0.9, 0.9, 0.92), smoothstep(0.8, 0.92, wp.y / hmax) * smoothstep(0.6, 0.8, wn.y));
	ALBEDO = pow(col, vec3(2.2));
	ROUGHNESS = 0.95;
}
"""


func _save_geology(path: String) -> void:
	# Geology map: soil (green, darker where deeper), bare rock (grey), scree (ochre), shaded by the relief; rivers
	# over 0.5 km2 drawn from get_rivers() at their widths times 20.
	var h := gen.get_height_image()
	var geo := gen.get_geology_image()
	var g := gen.get_grid_size()
	var cell := gen.cell_size
	var img := Image.create(g.x, g.y, false, Image.FORMAT_RGB8)
	var sun := Vector3(-1, 1.2, -1).normalized()
	for z in g.y:
		for x in g.x:
			var hx := (h.get_pixel(mini(x + 1, g.x - 1), z).r - h.get_pixel(maxi(x - 1, 0), z).r) / (2.0 * cell)
			var hz := (h.get_pixel(x, mini(z + 1, g.y - 1)).r - h.get_pixel(x, maxi(z - 1, 0)).r) / (2.0 * cell)
			var n := Vector3(-hx, 1.0, -hz).normalized()
			var c := geo.get_pixel(x, z)
			var col := Color(0.55, 0.62, 0.35).lerp(Color(0.3, 0.38, 0.18), clampf(c.r / 4.0, 0.0, 1.0))
			col = col.lerp(Color(0.62, 0.6, 0.58), c.g)
			col = col.lerp(Color(0.78, 0.6, 0.32), c.b)
			img.set_pixel(x, z, col * (0.35 + 0.8 * maxf(n.dot(sun), 0.0)))
	var half := Vector2(gen.size.x, gen.size.y) * 0.5
	for r in gen.get_rivers(5.0e5):
		var pts: PackedVector3Array = r["points"]
		var wd: PackedFloat32Array = r["widths"]
		for k in range(1, pts.size()):
			var a := (Vector2(pts[k - 1].x, pts[k - 1].z) + half) / cell
			var b := (Vector2(pts[k].x, pts[k].z) + half) / cell
			var rad := maxf(wd[k] * 20.0 / cell * 0.5, 0.5)
			for t in 8:
				var q := a.lerp(b, t / 8.0)
				for oz in range(-int(rad), int(rad) + 1):
					for ox in range(-int(rad), int(rad) + 1):
						var px := int(q.x) + ox
						var pz := int(q.y) + oz
						if px >= 0 and pz >= 0 and px < g.x and pz < g.y and ox * ox + oz * oz <= rad * rad + 0.5:
							img.set_pixel(px, pz, Color(0.15, 0.35, 0.75))
	img.resize(g.x * 2, g.y * 2, Image.INTERPOLATE_NEAREST)
	img.save_png(path)
	print("saved ", path)


func _build_view() -> void:
	# A display mesh of the generated ground; the close view uses a finer one. Heights come from sample_heights
	# (rows in parallel), normals from the neighbouring vertices.
	var res := int(args.get("view-res", "2048" if args.has("close") else "1024"))
	var step := WORLD / res
	var n := res + 1
	var heights := gen.sample_heights(Vector2(-WORLD * 0.5, -WORLD * 0.5), step, Vector2i(n, n)).get_data().to_float32_array()
	var hmax := 1.0
	for i in heights.size():
		if is_nan(heights[i]):
			heights[i] = 0.0
		hmax = maxf(hmax, heights[i])
	var verts := PackedVector3Array()
	verts.resize(n * n)
	var norms := PackedVector3Array()
	norms.resize(n * n)
	for z in n:
		var zu := maxi(z - 1, 0) * n
		var zd := mini(z + 1, n - 1) * n
		for x in n:
			var i := z * n + x
			var hx := heights[z * n + mini(x + 1, n - 1)] - heights[z * n + maxi(x - 1, 0)]
			var hz := heights[zd + x] - heights[zu + x]
			verts[i] = Vector3(-WORLD * 0.5 + x * step, heights[i], -WORLD * 0.5 + z * step)
			norms[i] = Vector3(-hx, 2.0 * step, -hz).normalized()
	var idx := PackedInt32Array()
	idx.resize(res * res * 6)
	var k := 0
	for z in res:
		for x in res:
			var i := z * n + x
			idx[k] = i
			idx[k + 1] = i + 1
			idx[k + 2] = i + n
			idx[k + 3] = i + 1
			idx[k + 4] = i + n + 1
			idx[k + 5] = i + n
			k += 6
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_INDEX] = idx
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	var shader := Shader.new()
	shader.code = VIEW_SHADER % [TerrainGenerator3D.get_geology_shader_code(), gen.get_detail_shader_code()]
	var mat := ShaderMaterial.new()
	mat.shader = shader
	mat.set_shader_parameter("hmax", hmax)
	mat.set_shader_parameter("detail_octaves", int(args.get("mdetail", "0")))
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

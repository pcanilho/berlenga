# TerrainGenerator3D bench and checks: a 4 km world with one mountain range along a curve, noise breaking it up,
# outlets on every edge.
#   godot --headless --path docs/bench/terrain_gen -- --test             checks (HeightMapShape3D ground)
#   godot --headless --path docs/bench/terrain_gen -- --test --terrain3d same on Terrain3D (addon installed)
#   godot --headless --path docs/bench/terrain_gen -- --time             generation time at 256, 512, 1024 cells
#   godot --path docs/bench/terrain_gen --rendering-driver vulkan -- --out=view.png [--raster] [--spp=16] [--close]
#       [--cell=8 --res=513] [--view-res=1024] [--no-detail] [--crest=1] [--tiles[=2]] [--mdetail=4]
#       [--coast [--focus=lake|coast] [--dist=520]]   an island with lakes, its sea, lakes and rivers built
#   godot --headless --path docs/bench/terrain_gen -- --coast --geology=map.png  island map, lakes and rivers listed
#   godot --headless --path docs/bench/terrain_gen -- --map=map.png      shaded relief with rivers
#   godot --headless --path docs/bench/terrain_gen -- --soil [--uplift=6]  soil and rock by slope above the valley floors
#   godot --headless --path docs/bench/terrain_gen -- [--coast] --runoff[=out.csv] [--jan=-4 --jul=15 --precip=1100
#       --season=0 --peak=0]  rivers fed by rain and snowmelt: monthly discharge, snow by height, cost
#   godot --headless --path docs/bench/terrain_gen -- --strata --geology=map.png [--profiles=rivers.csv]  layered rock:
#       waterfalls listed, river long profiles (x, z, height, hardness, log area); view: --strata --focus=fall --close
#   godot --headless --path docs/bench/terrain_gen -- --compare=prefix [--cell=8] [--refine=40]  coarse-to-fine
#       against single-level: time, height percentiles, slopes, drainage density; relief maps prefix_{one,multi}.png
#   godot --headless --path docs/bench/terrain_gen -- --landscape-test [--spacing=16 --quads=16]  Landscape3D checks only
#       (watertight edges at every level, neighbours one level apart, no rebuilds while turning); view: --landscape[=1]
#       draws the ground as a Landscape3D at that spacing instead of one 4 m (2 m close) mesh; --focus=ground
#       [--x --z --eye=3] stands on a flank
#   godot --headless --path docs/bench/terrain_gen -- [--coast] --climate=map.png  climate maps (temperature,
#       precipitation, winter sun, snow share) of the generated world [--wind=270] [--view: taller range]
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
var landscape: Landscape3D


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
	if args.has("coast"):
		_coast_setup()
	if args.has("terrain3d"):
		_build_terrain3d()
	else:
		_build_height_map(int(args.get("res", "257")))
	if args.has("test"):
		await _run_checks()
		print("%d checks, %d failed" % [checks, failures])
		print("PASS" if failures == 0 else "FAIL")
		get_tree().quit(1 if failures else 0)
	elif args.has("landscape-test"):
		gen.generate()
		_landscape_checks()
		print("%d checks, %d failed" % [checks, failures])
		print("PASS" if failures == 0 else "FAIL")
		get_tree().quit(1 if failures else 0)
	elif args.has("valleys"):
		if args.has("view"):
			gen.uplift = 6.0
			gen.range_width = 1300.0
		gen.generate()
		_valley_report()
		if args["valleys"] != "1":
			_save_map(args["valleys"])
		get_tree().quit()
	elif args.has("runoff"):
		_runoff_report()
		get_tree().quit()
	elif args.has("geology"):
		if args.has("view"):
			gen.uplift = 6.0
			gen.range_width = 1300.0
		gen.generate()
		_save_geology(args["geology"])
		_print_water()
		if gen.strata_enabled or args.has("profiles"):
			_print_waterfalls()
		if args.has("dump"):
			var fd := FileAccess.open(args["dump"], FileAccess.WRITE)
			fd.store_buffer(gen.get_height_image().get_data())
			fd.close()
		get_tree().quit()
	elif args.has("soil"):
		if args.has("uplift"):
			gen.uplift = float(args["uplift"])
		gen.generate()
		_soil_histogram()
		get_tree().quit()
	elif args.has("compare"):
		await _compare(args["compare"])
		get_tree().quit()
	elif args.has("climate"):
		if args.has("view"):
			gen.uplift = 6.0
			gen.range_width = 1300.0
		gen.generate()
		await _save_climate(args["climate"])
		get_tree().quit()
	elif args.has("map"):
		gen.generate()
		_save_map(args["map"])
		get_tree().quit()
	elif args.has("out"):
		# A wider, faster-rising range than the checks use, for a mountain view (the island keeps the checks' gentler
		# land, where valleys hold larger lakes).
		if not args.has("coast"):
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
		if landscape:
			print("landscape ", landscape.get_stats())
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
	gen.seed = int(args.get("seed", "1"))
	gen.uplift_noise = noise
	if args.has("levels"):
		gen.coarse_levels = int(args["levels"])
	if args.has("refine"):
		gen.refine_steps = int(args["refine"])
	if args.has("climate-rivers"):
		gen.climate_recipe = _runoff_recipe()
		gen.river_month = int(args.get("month", "-1"))
	if args.has("strata"):
		gen.strata_enabled = true
	if args.has("deposition"):
		gen.deposition = float(args["deposition"])
	for a in args:
		if a.begins_with("strata_"):
			gen.set(a, str_to_var(args[a]))
	add_child(gen)
	gen.ranges = [gen.get_path_to(path), gen.get_path_to(spur)]


func _coast_setup() -> void:
	# An island: sea on every edge, the land draining only to it, and three lakes behind natural dams.
	gen.ocean_edges = 15
	gen.outlet_edges = 0
	gen.lake_count = int(args.get("lakes", "3"))
	gen.lake_depth = 25.0
	for a in args:
		if a.begins_with("gen."):
			gen.set(a.substr(4), str_to_var(args[a]))


func _print_water() -> void:
	print("generate %.0f ms (coast, lakes and routing %.1f ms), sea %s, %d lakes" % [gen.get_last_generate_msec(), gen.get_last_water_msec(), gen.has_sea_result(), gen.get_lake_result_count()])
	for l in gen.get_lake_results():
		print("  lake level %.1f depth %.1f area %.0f m2 catchment %.2f km2 %s" % [l["level"], l["depth"], l["area"], l["catchment"] / 1.0e6, "endorheic" if l["endorheic"] else "outflow"])
	var ends := {}
	for r in gen.get_rivers(5.0e5):
		ends[r["ends_in"]] = ends.get(r["ends_in"], 0) + 1
		if r["from_lake"] >= 0 or r["lake"] >= 0:
			print("  river from lake %d to %s %d, %.2f m3/s, %d points" % [r["from_lake"], r["ends_in"], r["lake"], r["discharge"], r["points"].size()])
	print("  rivers end in: ", ends)
	if args.has("probe-x"):
		var z := float(args["probe-z"])
		for k in 60:
			var x := float(args["probe-x"]) + k * 8.0
			var q := Vector3(x, 0, z)
			print("  x %.0f base %.2f ground %.2f water %s" % [x, gen.get_base_height(q), gen.get_height(q), str(gen.get_water_level(q))])


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
	gen.seed = int(args.get("seed", "1"))
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
	# Steep ground (over 31 degrees) is mostly soil-mantled up to the critical slope (forested threshold hillslopes);
	# rock shows on sharp crests, cliffs and near the limit, so it is several times as common there as on gentle ground.
	check(rock_steep > 0.15 and rock_steep > 3.0 * rock_flat and rock_flat < 0.2, "rock commoner on steep ground (%.2f) than on gentle ground (%.2f)" % [rock_steep, rock_flat])
	var hist := _soil_histogram()
	var mid := 0
	var mid_soil := 0
	for b in range(4, 7):
		mid += int(hist["bins"][b][0])
		mid_soil += int(hist["bins"][b][3])
	check(mid > 100 and mid_soil > 0.5 * mid, "hillslopes of 20 to 35 degrees carry 0.2 m of soil or more on most cells (%.0f%% of %d)" % [100.0 * mid_soil / maxi(mid, 1), mid])
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

	await _coast_checks()
	await _climate_checks()
	_strata_checks()
	_runoff_checks()
	_deposition_checks()
	_landscape_checks()

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


func _coast_checks() -> void:
	# Coast and lakes: an island (sea on every edge, no other outlet) with lakes behind natural dams.
	gen.ocean_edges = 15
	gen.outlet_edges = 0
	gen.lake_count = 4
	gen.lake_depth = 25.0
	gen.generate()
	print("     coast and lakes: generate %.0f ms (coast, lakes and routing %.1f ms), %d lakes" % [gen.get_last_generate_msec(), gen.get_last_water_msec(), gen.get_lake_result_count()])
	var sea := gen.sea_level + gen.height_offset
	check(gen.has_sea_result() and absf(gen.get_water_level(Vector3(-2040, 0, -2040)) - sea) < 0.001 and is_nan(gen.get_water_level(Vector3.ZERO)), "sea along the edges, at sea level; the middle is dry land")
	# Offshore the seabed deepens (Dean profile plus shelf): along rays in from each edge, the depth falls towards the
	# coast, and the first wet cell is shallow.
	var monotone := 0
	var rays := 0
	var first_wet := 0.0
	for e in 4:
		for t in [-0.25, 0.25]:
			var dir := [Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)][e] as Vector3
			var start: Vector3 = -dir * 2040.0 + Vector3(dir.z, 0, dir.x) * WORLD * float(t)
			var depths := []
			for k in 200:
				var q: Vector3 = start + dir * (k * 8.0)
				if is_nan(gen.get_water_level(q)):
					break
				depths.append(gen.get_water_depth(q))
			if depths.size() < 10:
				continue
			rays += 1
			first_wet = maxf(first_wet, depths[depths.size() - 1])
			var n := depths.size()
			if depths[0] > depths[n / 2] and depths[n / 2] > depths[n - 1]:
				monotone += 1
	check(rays >= 6 and monotone == rays, "seabed deepens offshore on %d of %d rays" % [monotone, rays])
	check(first_wet < 2.5, "water at the coastline is shallow (deepest %.2f m)" % first_wet)
	# Inland from the waterline, along the same rays: the land is never at sea level and rises as a beach.
	var lowest_near := 1.0e9
	var lowest_back := 1.0e9
	for e in 4:
		for t in [-0.3, -0.1, 0.1, 0.3]:
			var dir := [Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)][e] as Vector3
			var q: Vector3 = -dir * 2040.0 + Vector3(dir.z, 0, dir.x) * WORLD * float(t)
			for k in 400:
				if is_nan(gen.get_water_level(q)):
					break
				q += dir * 4.0
			for k in range(1, 31):
				var h := gen.get_height(q + dir * (k * 4.0)) - sea
				if k * 4.0 >= 20.0 and k * 4.0 <= 40.0:
					lowest_near = minf(lowest_near, h)
				elif k * 4.0 >= 100.0:
					if h < 1.0 and args.has("verbose"):
						print("     low beach ", q + dir * (k * 4.0), " ", h, " wet ", gen.get_water_level(q + dir * (k * 4.0)))
					lowest_back = minf(lowest_back, h)
	check(lowest_near > 0.1 and lowest_back > 0.5, "land above the sea at the waterline (lowest %.2f m from 20 to 40 m) and rising inland (lowest %.2f m at 100 to 120 m)" % [lowest_near, lowest_back])
	var bed_detail := 0.0
	var dry_detail := 0.0
	for i in 300:
		var q := Vector3(-2000.0 + 4000.0 * fposmod(i * 0.618034, 1.0), 0, -2000.0 + 4000.0 * fposmod(i * 0.414214, 1.0))
		var diff := absf(gen.get_height(q) - gen.get_base_height(q))
		if gen.get_water_depth(q) > 5.0:
			bed_detail = maxf(bed_detail, diff)
		elif is_nan(gen.get_water_level(q)):
			dry_detail = maxf(dry_detail, diff)
	check(bed_detail < 0.01 and dry_detail > 1.0, "erosion detail stays off the seabed (%.3f m under water, %.1f m on land)" % [bed_detail, dry_detail])
	var rivers := gen.get_rivers(2.0e5)
	var ends := {}
	for r in rivers:
		ends[r["ends_in"]] = ends.get(r["ends_in"], 0) + 1
	check(not ends.has("edge") and ends.get("sea", 0) >= 3, "every river reaches the sea, a lake or another river %s" % str(ends))
	var lakes := gen.get_lake_results()
	check(lakes.size() >= 1, "lakes form behind natural dams (%d)" % lakes.size())
	var held := true
	var outflows := 0
	for k in lakes.size():
		var l: Dictionary = lakes[k]
		var dam: Vector3 = l["dam"]
		held = held and float(l["depth"]) > 2.0 and float(l["area"]) >= gen.lake_min_area and gen.get_height(dam) >= float(l["level"]) - 0.01
		if l.has("outlet"):
			# At a small threshold and at half the lake's catchment (where a stream joining at the outlet may have more
			# area than any other donor).
			var o: Vector3 = l["outlet"]
			var found := 0
			for min_area in [1.0e5, float(l["catchment"]) * 0.5]:
				for r in gen.get_rivers(min_area):
					if int(r["from_lake"]) == k and (r["points"][0] as Vector3).distance_to(Vector3(o.x, (r["points"][0] as Vector3).y, o.z)) < gen.cell_size * 0.5:
						found += 1
						break
			if found == 2:
				outflows += 1
	if not held or args.has("verbose"):
		for l in lakes:
			print("     lake depth %.1f area %.0f level %.2f dam %.2f center depth %.2f" % [l["depth"], l["area"], l["level"], gen.get_height(l["dam"]), gen.get_water_depth(l["center"])])
	check(held, "each lake is deeper than 2 m, at least lake_min_area, and its dam stands at its level")
	var centred := true
	for l in lakes:
		centred = centred and absf(gen.get_water_level(l["center"]) - float(l["level"])) < 0.001 and gen.get_water_depth(l["center"]) > 1.0
	check(centred, "each lake's center is on its water, over at least 1 m of it")
	var with_outlet := 0
	for l in lakes:
		if l.has("outlet"):
			with_outlet += 1
	check(outflows == with_outlet, "an outflow river starts at each lake's outlet (%d of %d)" % [outflows, with_outlet])
	var mouths_on_water := true
	var into_lakes := 0
	for r in gen.get_rivers(1.0e5):
		if r["ends_in"] == "lake":
			into_lakes += 1
			var pts: PackedVector3Array = r["points"]
			var off := absf(pts[pts.size() - 1].y - float(lakes[int(r["lake"])]["level"]))
			if off >= 0.01:
				print("     river into lake %d ends %.2f m off its level at %s (water depth there %.2f, level %.2f)" % [int(r["lake"]), off, pts[pts.size() - 1], gen.get_water_depth(pts[pts.size() - 1]), gen.get_water_level(pts[pts.size() - 1])])
			mouths_on_water = mouths_on_water and off < 0.01
	check(mouths_on_water, "rivers into lakes end on the water (%d)" % into_lakes)
	# Water balance: the first lake again, from a marker, with evaporation three times what its catchment can make up.
	if lakes.size() > 0:
		var l0: Dictionary = lakes[0]
		var marker := Marker3D.new()
		add_child(marker)
		marker.global_position = l0["dam"]
		gen.lake_count = 0
		gen.lakes = [gen.get_path_to(marker)]
		gen.generate()
		var again := gen.get_lake_results()
		check(again.size() == 1 and absf(float(again[0]["level"]) - float(l0["level"])) < 0.01, "a lake marker builds the same lake")
		gen.lake_evaporation = 3.0 * gen.river_runoff * float(l0["catchment"]) / float(l0["area"])
		gen.generate()
		again = gen.get_lake_results()
		check(again.size() == 0 or (again[0]["endorheic"] and not again[0].has("outlet") and float(again[0]["level"]) < float(l0["level"]) - 0.1), "a lake whose catchment cannot make up its evaporation stands lower with no outflow")
		gen.lake_evaporation = 1000.0
		gen.lakes = []
		marker.queue_free()
		gen.lake_count = 4
		gen.generate()
	var img_a := gen.get_water_image()
	gen.generate()
	check(_hash(img_a) == _hash(gen.get_water_image()), "coast and lakes deterministic")
	# The water nodes and the rivers into them.
	var holder := Node3D.new()
	add_child(holder)
	var nodes := gen.build_water(holder)
	var ok_nodes: bool = nodes.size() == 1 + lakes.size() and absf((nodes[0] as Node3D).global_position.y - sea) < 0.001
	if ClassDB.class_exists("PhysXWaterSurface3D"):
		ok_nodes = ok_nodes and (nodes[0] as Node).get_class() == "PhysXWaterSurface3D"
	for k in lakes.size():
		var lake_node := nodes[1 + k] as Node3D
		ok_nodes = ok_nodes and absf(lake_node.global_position.y - float(lakes[k]["level"])) < 0.001
		if ClassDB.class_exists("PhysXWaterSurface3D"):
			var outline: Mesh = lake_node.get("surface_mesh")
			ok_nodes = ok_nodes and outline != null and outline.get_surface_count() == 1
	check(ok_nodes, "build_water(): the sea at sea level and one surface per lake at its level, cut to its outline")
	var paths := gen.build_river_paths(holder, 2.0e5)
	var targeted := 0
	var to_water := 0
	var info := gen.get_rivers(2.0e5)
	for r in paths.size():
		if info[r]["ends_in"] == "sea" or info[r]["ends_in"] == "lake":
			to_water += 1
			if ClassDB.class_exists("PhysXRiver3D") and not (paths[r].get("mouth_target") as NodePath).is_empty():
				targeted += 1
	check(not ClassDB.class_exists("PhysXRiver3D") or targeted == to_water, "rivers ending in the sea or a lake get it as mouth_target (%d of %d)" % [targeted, to_water])
	holder.queue_free()
	gen.ocean_edges = 0
	gen.outlet_edges = 15
	gen.lake_count = 0
	gen.lake_depth = 20.0
	gen.generate()
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
uniform float sea_level = -100000.0;
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
	// Sand on beaches, darker and greener sediment on the seabed.
	float shore = smoothstep(sea_level + 3.0, sea_level + 1.0, wp.y);
	col = mix(col, mix(vec3(0.72, 0.65, 0.5), vec3(0.2, 0.22, 0.17), smoothstep(sea_level - 0.5, sea_level - 4.0, wp.y)), shore * smoothstep(0.7, 0.9, wn.y));
	ALBEDO = pow(col, vec3(2.2));
	ROUGHNESS = 0.95;
}
"""


func _save_geology(path: String) -> void:
	# Geology map: soil (green, darker where deeper), bare rock (grey), scree (ochre), shaded by the relief; rivers
	# over 0.5 km2 drawn from get_rivers() at their widths times 20.
	var h := gen.get_height_image()
	var geo := gen.get_geology_image()
	var water := gen.get_water_image()
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
			col = col * (0.35 + 0.8 * maxf(n.dot(sun), 0.0))
			var wd := water.get_pixel(x, z).r if water else 0.0
			if wd > 0.0:
				col = Color(0.55, 0.75, 0.85).lerp(Color(0.05, 0.15, 0.35), clampf(wd / 30.0, 0.0, 1.0))
			img.set_pixel(x, z, col)
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
	var shader := Shader.new()
	shader.code = VIEW_SHADER % [TerrainGenerator3D.get_geology_shader_code(), gen.get_detail_shader_code()]
	var mat := ShaderMaterial.new()
	mat.shader = shader
	mat.set_shader_parameter("hmax", hmax)
	mat.set_shader_parameter("detail_octaves", int(args.get("mdetail", "0")))
	if gen.has_sea_result():
		mat.set_shader_parameter("sea_level", gen.sea_level + gen.height_offset)
	if args.has("landscape"):
		# The ground as a Landscape3D: chunk meshes with LODs at the given spacing (1 m by default).
		var ls := Landscape3D.new()
		ls.spacing = float(args["landscape"]) if args["landscape"] != "1" else 1.0
		ls.lod_error = float(args.get("lod-error", "1.0"))
		ls.material = mat
		add_child(ls)
		ls.source = ls.get_path_to(gen)
		ls.rebuild()
		landscape = ls
	else:
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.material_override = mat
		add_child(mi)
	if ground_mesh:
		ground_mesh.visible = false
	if args.has("coast"):
		# The generator's own water: the sea and the lakes as water surfaces, rivers ending in them.
		var nodes := gen.build_water(self)
		var rivers := gen.build_river_paths(self, float(args.get("river-area", "3e5")))
		print("water nodes %d, rivers %d" % [nodes.size(), rivers.size()])
	else:
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
	cam.far = 60000.0 if args.has("coast") else 20000.0
	cam.fov = 50.0
	if args.get("focus", "") == "lake" and gen.get_lake_result_count() > 0:
		# A lake, its dam and outflow, from above the valley below it.
		var l: Dictionary = gen.get_lake_results()[int(args.get("lake", "0"))]
		var c: Vector3 = l["center"]
		var dam: Vector3 = l["dam"]
		var away := Vector3(dam.x - c.x, 0.0, dam.z - c.z).normalized()
		cam.fov = 45.0
		cam.position = c + away * float(args.get("dist", "520")) + Vector3(0, float(args.get("dist", "520")) * 0.5, 0)
		add_child(cam)
		cam.look_at(c)
	elif args.get("focus", "") == "fall":
		# The tallest waterfall the layered rock made, from downstream, its river drawn.
		var falls := gen.get_waterfalls(float(args.get("river-area", "1e5")), 8.0)
		var best: Dictionary = {}
		for f in falls:
			if best.is_empty() or f["height"] * sqrt(f["discharge"]) > best["height"] * sqrt(best["discharge"]):
				best = f
		gen.build_river_paths(self, float(args.get("river-area", "1e5")))
		var top: Vector3 = best["top"]
		var foot: Vector3 = best["bottom"]
		var down := Vector3(foot.x - top.x, 0.0, foot.z - top.z).normalized()
		var d := float(args.get("dist", "160"))
		print("fall %.0f m at %s, %.2f m3/s" % [best["height"], top, best["discharge"]])
		cam.fov = 45.0
		cam.position = foot + down * d + Vector3(0, d * float(args.get("rise", "0.35")), 0)
		add_child(cam)
		cam.look_at((top + foot) * 0.5)
		# Low sun from behind the camera's left shoulder, so the face is lit.
		var back := (cam.position - (top + foot) * 0.5).normalized().rotated(Vector3.UP, 0.6)
		sun.look_at_from_position(foot + Vector3(back.x, 0.0, back.z).normalized() * 3000.0 + Vector3(0, 1600, 0), foot)
	elif args.get("focus", "") == "coast":
		# Low over the sea, looking along the coast at a beach and the shore waves.
		cam.fov = 45.0
		cam.position = Vector3(400, 120, 2350)
		add_child(cam)
		cam.look_at(Vector3(-300, 10, 1500))
	elif args.get("focus", "") == "ground":
		# Standing on a flank of the range, looking across the slope: where vertex spacing shows.
		var at := Vector3(float(args.get("x", "-300")), 0.0, float(args.get("z", "100")))
		at.y = gen.get_height(at) + float(args.get("eye", "3"))
		var to := at + Vector3(float(args.get("dx", "80")), 0.0, float(args.get("dz", "60")))
		to.y = gen.get_height(to)
		cam.fov = 60.0
		cam.near = 0.1
		cam.position = at
		add_child(cam)
		cam.look_at(to)
		# Low raking sun across the view, so facets show.
		var side := Vector3(to.x - at.x, 0.0, to.z - at.z).normalized().rotated(Vector3.UP, float(args.get("sun", "1.4")))
		sun.look_at_from_position(to + side * 3000.0 + Vector3(0, 700, 0), to)
	elif args.has("close"):
		# Close view of one flank, for raster and path traced crops at 100%.
		cam.fov = 40.0
		cam.position = Vector3(700, 900, 1100)
		add_child(cam)
		cam.look_at(Vector3(-200, 450, 200))
	else:
		cam.position = Vector3(1900, 1100, 2900)
		add_child(cam)
		cam.look_at(Vector3(-400, 250, -300))


func _hill_source(height: float, sigma: float) -> Node3D:
	# A round Gaussian hill as a plain height source (any node with get_height(Vector3)).
	var sc := GDScript.new()
	sc.source_code = "extends Node3D\nvar height := 0.0\nvar sigma := 1.0\nfunc get_height(p: Vector3) -> float:\n\treturn height * exp(-(p.x * p.x + p.z * p.z) / (2.0 * sigma * sigma))\n"
	sc.reload()
	var n := Node3D.new()
	n.set_script(sc)
	n.set("height", height)
	n.set("sigma", sigma)
	return n


func _edge_key(a: Vector3, b: Vector3) -> String:
	var ka := "%d,%d" % [roundi(a.x * 8.0), roundi(a.z * 8.0)]
	var kb := "%d,%d" % [roundi(b.x * 8.0), roundi(b.z * 8.0)]
	return ka + ";" + kb if ka < kb else kb + ";" + ka


func _landscape_checks() -> void:
	# Landscape3D: chunk meshes with an LOD chain over the generated ground, coarse here so the checks stay fast.
	var ls := Landscape3D.new()
	ls.spacing = float(args.get("spacing", "16"))
	ls.chunk_quads = int(args.get("quads", "16"))
	ls.lod_levels = 4
	ls.lod_error = float(args.get("lod-error", "8"))
	add_child(ls)
	ls.source = ls.get_path_to(gen)
	check(ls.rebuild(), "Landscape3D builds from the generator")
	var grid := ls.get_chunk_grid()
	var st: Dictionary = ls.get_stats()
	print("     landscape %dx%d chunks, %d levels, %.1f ms" % [grid.x, grid.y, ls.get_lod_count(), st["build_msec"]])
	var mono := true
	var coarse_err := 0.0
	for cz in grid.y:
		for cx in grid.x:
			var c := Vector2i(cx, cz)
			mono = mono and ls.get_chunk_lod_error(c, 0) == 0.0
			for l in range(1, ls.get_lod_count()):
				mono = mono and ls.get_chunk_lod_error(c, l) >= ls.get_chunk_lod_error(c, l - 1)
			coarse_err = maxf(coarse_err, ls.get_chunk_lod_error(c, ls.get_lod_count() - 1))
	check(mono, "each coarser level departs at least as far from the ground (worst %.1f m)" % coarse_err)
	var probe := Vector3(-300.0, 0.0, 100.0)
	check(absf(ls.get_height(Vector3(-2048.0 + 64 * ls.spacing, 0, -2048.0 + 64 * ls.spacing)) - gen.get_height(Vector3(-2048.0 + 64 * ls.spacing, 0, -2048.0 + 64 * ls.spacing))) < 0.01, "its heights are the generator's at the grid points")
	# A camera low over the range, looking along it.
	var proj := Projection.create_perspective(50.0, 16.0 / 9.0, 0.1, 20000.0)
	var eye := Transform3D(Basis(), probe + Vector3(0, gen.get_height(probe) + 30.0, 0)).looking_at(Vector3(1500, 0, 1500))
	ls.update_lods(eye, proj, 540.0)
	var lods := {}
	var apart := true
	var masks_ok := true
	for cz in grid.y:
		for cx in grid.x:
			var l := ls.get_chunk_lod(Vector2i(cx, cz))
			lods[l] = lods.get(l, 0) + 1
			var m := ls.get_chunk_edge_mask(Vector2i(cx, cz))
			var nbs := [Vector2i(cx, cz - 1), Vector2i(cx + 1, cz), Vector2i(cx, cz + 1), Vector2i(cx - 1, cz)]
			for s in 4:
				var nb: Vector2i = nbs[s]
				if nb.x < 0 or nb.y < 0 or nb.x >= grid.x or nb.y >= grid.y:
					masks_ok = masks_ok and (m & (1 << s)) == 0
					continue
				var nl := ls.get_chunk_lod(nb)
				apart = apart and absi(nl - l) <= 1
				masks_ok = masks_ok and (((m >> s) & 1) == 1) == (nl > l)
	print("     chunks per level ", lods)
	check(lods.size() >= 3, "near chunks fine, far ones coarse (%d levels in use)" % lods.size())
	check(apart, "neighbouring chunks are at most one level apart")
	check(masks_ok, "edges are stitched exactly where the neighbour is coarser")
	# Watertight: every triangle edge inside the ground is shared by exactly two triangles, at every level and mask.
	var edges := {}
	var tris := 0
	for cz in grid.y:
		for cx in grid.x:
			var t := ls.get_chunk_triangles(Vector2i(cx, cz))
			tris += t.size() / 3
			for i in range(0, t.size(), 3):
				for e in 3:
					var k := _edge_key(t[i + e], t[i + (e + 1) % 3])
					edges[k] = edges.get(k, 0) + 1
	var bad := 0
	var lo := -2048.0
	var hi := -2048.0 + grid.x * ls.chunk_quads * ls.spacing
	for k in edges:
		if edges[k] == 2:
			continue
		var pts: PackedStringArray = k.split(";")
		var a := pts[0].split(",")
		var b := pts[1].split(",")
		var ax := float(a[0]) / 8.0
		var az := float(a[1]) / 8.0
		var bx := float(b[0]) / 8.0
		var bz := float(b[1]) / 8.0
		var outer := (ax == bx and (ax == lo or ax == hi)) or (az == bz and (az == lo or az == hi))
		if not (outer and edges[k] == 1):
			bad += 1
	check(bad == 0, "no cracks: every inner edge is shared by two triangles (%d triangles, %d bad edges)" % [tris, bad])
	st = ls.get_stats()
	var full: int = grid.x * grid.y * ls.chunk_quads * ls.chunk_quads * 2
	check(int(st["triangles"]) < full * 3 / 4, "the LODs draw fewer triangles (%d against %d at full detail)" % [st["triangles"], full])
	# Turning on the spot changes nothing; flying away goes coarse; coming back reuses the kept meshes.
	var builds: int = st["mesh_builds"]
	var switches: int = st["lod_switches"]
	for a in 8:
		ls.update_lods(Transform3D(eye.basis.rotated(Vector3.UP, a * TAU / 8.0), eye.origin), proj, 540.0)
	st = ls.get_stats()
	check(int(st["mesh_builds"]) == builds and int(st["lod_switches"]) == switches, "turning the camera builds and switches nothing (behind keeps its level)")
	ls.update_lods(Transform3D(eye.basis, eye.origin + Vector3(0, 6000, 0)), proj, 540.0)
	var far_tris: int = ls.get_stats()["triangles"]
	ls.update_lods(eye, proj, 540.0)
	st = ls.get_stats()
	check(far_tris < int(st["triangles"]), "from high up it draws fewer triangles (%d against %d)" % [far_tris, st["triangles"]])
	var builds_back: int = st["mesh_builds"]
	ls.update_lods(Transform3D(eye.basis, eye.origin + Vector3(0, 6000, 0)), proj, 540.0)
	ls.update_lods(eye, proj, 540.0)
	check(int(ls.get_stats()["mesh_builds"]) == builds_back, "flying back and forth reuses the kept meshes (%d built in all)" % builds_back)
	# Hysteresis: a small step back and forth at a level boundary does not flip chunks.
	var sw0: int = ls.get_stats()["lod_switches"]
	for i in 6:
		ls.update_lods(Transform3D(eye.basis, eye.origin + Vector3(0, 0, 1.0 if i % 2 == 0 else 0.0)), proj, 540.0)
	check(int(ls.get_stats()["lod_switches"]) - sw0 <= 2, "a 1 m jiggle does not flip levels (%d switches)" % (int(ls.get_stats()["lod_switches"]) - sw0))
	# Full detail at 1 m over a 1 km square: build time and update cost.
	var big := Landscape3D.new()
	big.spacing = 1.0
	big.height_image = gen.sample_heights(Vector2(-500, -400), 1.0, Vector2i(1025, 1025))
	big.origin = Vector2(-500, -400)
	add_child(big)
	big.rebuild()
	big.update_lods(eye, proj, 1080.0)
	var first_update: float = big.get_stats()["update_usec"]
	big.update_lods(Transform3D(eye.basis.rotated(Vector3.UP, 0.5), eye.origin), proj, 1080.0)
	var bs: Dictionary = big.get_stats()
	print("     1 km at 1 m: first update with mesh builds %.0f ms" % (first_update / 1000.0))
	var med := []
	for l in big.get_lod_count():
		var e := PackedFloat32Array()
		for cz in big.get_chunk_grid().y:
			for cx in big.get_chunk_grid().x:
				e.append(big.get_chunk_lod_error(Vector2i(cx, cz), l))
		e.sort()
		med.append("%.2f" % e[e.size() / 2])
	print("     1 km at 1 m: median error per level (m) ", med)
	print("     1 km at 1 m: build %.0f ms, update %.0f us, %d triangles drawn, per level %s" % [bs["build_msec"], bs["update_usec"], bs["triangles"], bs["chunks_per_level"]])
	check(int(bs["triangles"]) > 0 and float(bs["update_usec"]) < 5000.0, "1 km at 1 m picks its levels (no builds) in under 5 ms (%.0f us)" % bs["update_usec"])
	big.queue_free()
	ls.queue_free()


func _deposition_checks() -> void:
	# Deposition (Yuan et al. 2019): off changes nothing. On, with the ranges rising out of ground that does not rise
	# (base_uplift 0), the rivers leaving the mountains drop sediment and build alluvial plains; where everything
	# rises, it steepens the rivers instead, so the range stands higher (relief grows with 1 + G).
	var before := _hash(gen.get_height_image())
	var r0 := _valley_report()
	gen.deposition = 0.0
	gen.generate()
	check(_hash(gen.get_height_image()) == before, "deposition 0 leaves the terrain byte for byte")
	gen.deposition = 0.5
	gen.generate()
	var r1 := _valley_report()
	check(r1["highest"] > r0["highest"], "where all the ground rises, deposition steepens the rivers (highest %.0f m against %.0f m)" % [r1["highest"], r0["highest"]])
	check(r1["msec"] < r0["msec"] * 1.6, "deposition costs under 60%% more time (%.0f against %.0f ms)" % [r1["msec"], r0["msec"]])
	gen.base_uplift = 0.0
	gen.deposition = 0.0
	gen.generate()
	var b0 := _valley_report()
	gen.deposition = 0.5
	gen.generate()
	var b1 := _valley_report()
	check(b1["alluvium_share"] > b0["alluvium_share"] + 0.05, "below ranges on still ground, deposition spreads alluvium (%.0f%% of the land on over 1 m, against %.0f%%)" % [100.0 * b1["alluvium_share"], 100.0 * b0["alluvium_share"]])
	gen.base_uplift = 0.1
	gen.deposition = 0.0
	gen.generate()


# Valley floors: how much of the land below the mountains is flat, how deep its alluvium, by drainage area.
func _valley_report() -> Dictionary:
	var g := gen.get_grid_size()
	var cell := gen.cell_size
	var hi := gen.get_height_image().get_data().to_float32_array()
	var ar := gen.get_drainage_image().get_data().to_float32_array()
	var sd := gen.get_sediment_image().get_data().to_float32_array()
	var land := 0
	var flat := 0
	var floor_cells := 0
	var floor_flat := 0
	var floor_sed := 0.0
	var hmax := 0.0
	for i in hi.size():
		hmax = maxf(hmax, hi[i])
	for z in range(1, g.y - 1):
		for x in range(1, g.x - 1):
			var i := z * g.x + x
			if hi[i] <= gen.sea_level + 0.5:
				continue
			land += 1
			var sx := (hi[i + 1] - hi[i - 1]) / (2.0 * cell)
			var sz := (hi[i + g.x] - hi[i - g.x]) / (2.0 * cell)
			var slope := sqrt(sx * sx + sz * sz)
			if slope < 0.03:
				flat += 1
			if ar[i] > 2.0e6:
				floor_cells += 1
				floor_sed += sd[i]
				if slope < 0.03:
					floor_flat += 1
	var deep := 0
	var smax := 0.0
	for i in sd.size():
		smax = maxf(smax, sd[i])
		if sd[i] > 1.0 and hi[i] > gen.sea_level + 0.5:
			deep += 1
	var r := {
		"alluvium_share": float(deep) / maxi(land, 1),
		"alluvium_max": smax,
		"msec": gen.get_last_generate_msec(),
		"highest": hmax,
		"flat_land": float(flat) / maxi(land, 1),
		"valley_flat": float(floor_flat) / maxi(floor_cells, 1),
		"valley_alluvium": floor_sed / maxi(floor_cells, 1),
	}
	print("deposition %.2f: %.0f ms, highest %.0f m, land under 3%% slope %.1f%%, valley cells (>2 km2) under 3%% %.1f%%, mean alluvium on them %.1f m, land on over 1 m of sediment %.1f%% (deepest %.0f m)" % [gen.deposition, r["msec"], hmax, 100.0 * r["flat_land"], 100.0 * r["valley_flat"], r["valley_alluvium"], 100.0 * r["alluvium_share"], smax])
	return r


func _strata_checks() -> void:
	# Layered rock: rivers step down over hard bands as waterfalls; off, the strata settings change nothing.
	gen.generate()
	var base := gen.get_height_image().get_data()
	gen.strata_hardness = 50.0
	gen.generate()
	check(gen.get_height_image().get_data() == base and gen.get_waterfalls().is_empty(), "strata off: its settings leave the ground as it was, no waterfalls")
	gen.strata_hardness = 100.0
	gen.strata_enabled = true
	gen.generate()
	var t := gen.get_last_generate_msec()
	var falls := gen.get_waterfalls(1.0e5, 8.0)
	var tall := 0
	var in_band := 0
	for f in falls:
		if f["height"] >= 15.0:
			tall += 1
		# The face cuts a hard band: hard rock within the face or just above the lip (a lip that has just broken
		# off stands on the soft rock under it for a while).
		var hard := 0.0
		for k in 13:
			hard = maxf(hard, gen.get_rock_hardness(f["top"].lerp(f["bottom"], -2.0 + k / 4.0)))
		if hard > 0.5:
			in_band += 1
	check(tall >= 3, "strata: rivers step down over waterfalls (%d of 8 m or more, %d of 15 m or more)" % [falls.size(), tall])
	check(falls.size() > 0 and in_band == falls.size(), "strata: every waterfall cuts a hard band (%d of %d)" % [in_band, falls.size()])
	# No hard pinnacles: ground steeper than 60 degrees stays rare (the river faces), as without the bands.
	var cs := 16.0
	var steep := 0
	var total := 0
	for z in range(4, 252, 2):
		for x in range(4, 252, 2):
			var q := Vector3(-WORLD * 0.5 + x * cs, 0, -WORLD * 0.5 + z * cs)
			var h0 := gen.get_base_height(q)
			var gx := (gen.get_base_height(q + Vector3(cs, 0, 0)) - gen.get_base_height(q - Vector3(cs, 0, 0))) / (2.0 * cs)
			var gz := (gen.get_base_height(q + Vector3(0, 0, cs)) - gen.get_base_height(q - Vector3(0, 0, cs))) / (2.0 * cs)
			if h0 > 5.0:
				total += 1
				if sqrt(gx * gx + gz * gz) > tan(deg_to_rad(60.0)):
					steep += 1
	check(steep < total / 200, "strata: no rock pinnacles (%.2f%% of the ground over 60 degrees)" % [100.0 * steep / maxi(total, 1)])
	var first := gen.get_height_image().get_data()
	gen.generate()
	check(gen.get_height_image().get_data() == first and gen.get_waterfalls(1.0e5, 8.0).size() == falls.size(), "strata: deterministic")
	print("     strata generate %.0f ms" % t)
	gen.strata_enabled = false
	gen.generate()
	check(gen.get_height_image().get_data() == base and gen.get_rock_hardness(Vector3.ZERO) == 0.0, "strata off again: the same ground, no hardness map")


func _peak_month(m: PackedFloat32Array) -> int:
	var best := 0
	for k in 12:
		if m[k] > m[best]:
			best = k
	return best


func _runoff_checks() -> void:
	# Rivers fed by rain and snowmelt from the climate (McCabe and Markstrom 2007 balance, Hock 1999 melt).
	gen.climate_recipe = null
	gen.generate()
	check(not gen.has_runoff() and is_equal_approx(gen.get_runoff_at(Vector3.ZERO), gen.river_runoff), "runoff: without a climate, rivers keep river_runoff")
	var cold := ClimateRecipe.new()
	cold.temperature_january = -4.0
	cold.temperature_july = 15.0
	cold.precipitation = 1100.0
	gen.climate_recipe = cold
	gen.generate()
	var rivers := gen.get_rivers(1.0e5)
	var trunk: Dictionary = rivers[0]
	var m: PackedFloat32Array = trunk["monthly_discharge"]
	var mean := 0.0
	var days := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	for k in 12:
		mean += m[k] * days[k] / 365.0
	check(gen.has_runoff() and absf(mean - trunk["discharge"]) < 0.01 * trunk["discharge"], "runoff: annual discharge is the mean of the months (%.3f, %.3f m3/s)" % [trunk["discharge"], mean])
	var peak := _peak_month(m)
	check(peak >= 2 and peak <= 5 and m[peak] > 2.0 * m[7], "runoff: cold climate, the snowmelt flood in spring (peak month %d, %.3f against %.3f in August)" % [peak + 1, m[peak], m[7]])
	# Snow lies longer and deeper up the range.
	var low := Vector3.ZERO
	var high := Vector3.ZERO
	for z in range(-1900, 1900, 64):
		for x in range(-1900, 1900, 64):
			var q := Vector3(x, 0, z)
			q.y = gen.get_base_height(q)
			if low == Vector3.ZERO or q.y < low.y:
				low = q
			if q.y > high.y:
				high = q
	var months_low := 0
	var months_high := 0
	for k in 12:
		months_low += 1 if gen.get_snow_water_at(low, k) > 1.0 else 0
		months_high += 1 if gen.get_snow_water_at(high, k) > 1.0 else 0
	check(gen.get_snow_water_at(high, 2) > gen.get_snow_water_at(low, 2) and months_high >= months_low, "runoff: snow lies deeper and longer up the range (March %.0f mm at %.0f m, %.0f mm at %.0f m)" % [gen.get_snow_water_at(high, 2), high.y, gen.get_snow_water_at(low, 2), low.y])
	check(gen.get_mean_runoff() > 0.2 * cold.precipitation and gen.get_mean_runoff() < 1.5 * cold.precipitation, "runoff: mean runoff %.0f mm a year of %.0f mm of precipitation (more on the range)" % [gen.get_mean_runoff(), cold.precipitation])
	check(gen.get_last_runoff_msec() < 0.2 * gen.get_last_generate_msec(), "runoff: %.0f ms of %.0f ms generation" % [gen.get_last_runoff_msec(), gen.get_last_generate_msec()])
	gen.river_month = 3
	var april: Dictionary = gen.get_rivers(1.0e5)[0]
	check(is_equal_approx(april["discharge"], m[3]) and april["widths"][-2] > trunk["widths"][-2], "runoff: river_month picks April's flood for the rivers (%.3f m3/s, wider)" % april["discharge"])
	gen.river_month = -1
	# A mild winter with even precipitation: the high flows come in winter, when little evaporates.
	var mild := ClimateRecipe.new()
	mild.temperature_january = 8.0
	mild.temperature_july = 24.0
	gen.climate_recipe = mild
	gen.generate()
	var wm: PackedFloat32Array = gen.get_rivers(1.0e5)[0]["monthly_discharge"]
	var wp := _peak_month(wm)
	check(wp >= 10 or wp <= 2, "runoff: mild climate, high water in winter (peak month %d)" % (wp + 1))
	# Summer rain (a monsoon): precipitation_seasonality moves the high water to summer.
	mild.precipitation_seasonality = 0.9
	mild.precipitation_peak_month = 6
	gen.generate()
	var sp := _peak_month(gen.get_rivers(1.0e5)[0]["monthly_discharge"])
	check(sp >= 5 and sp <= 9, "runoff: summer rain brings the high water to summer (peak month %d)" % (sp + 1))
	gen.climate_recipe = null
	gen.generate()


func _climate_checks() -> void:
	# Climate maps on a 24 km square around a 1500 m Gaussian hill (sigma 3 km), westerly wet winds.
	var hill := _hill_source(1500.0, 3000.0)
	hill.name = "Hill"
	add_child(hill)
	var dm := TerrainDataMap3D.new()
	dm.bake_on_ready = false
	dm.size = 24000.0
	dm.resolution = 64
	add_child(dm)
	dm.height_source = dm.get_path_to(hill)
	var r := ClimateRecipe.new()
	r.sun_cell = 64.0
	r.precipitation_cell = 256.0
	dm.climate_recipe = r
	check(dm.bake_climate() == OK and dm.has_climate(), "bake_climate() runs on a plain height source (%.0f ms at %d cells)" % [dm.get_climate_bake_time_msec(), int(24000.0 / 64.0)])
	var top := Vector3(0, 0, 0)
	var t_jan := dm.get_temperature_at(top, 15.0)
	var t_jul := dm.get_climate_at(top)["temperature_july"] as float
	check(absf(t_jan - (3.0 - 4.5 * 1.5)) < 0.3 and absf(t_jul - (22.0 - 6.5 * 1.5)) < 0.3, "temperature at the top by the seasonal lapse rate (January %.2f C, July %.2f C)" % [t_jan, t_jul])
	var south := dm.get_winter_shortwave_at(Vector3(0, 0, 3000))
	var north := dm.get_winter_shortwave_at(Vector3(0, 0, -3000))
	var flat := dm.get_shortwave_at(Vector3(-11000, 0, -11000))
	print("     shortwave W/m2: flat year %.0f, winter south face %.0f, north face %.0f" % [flat, south, north])
	check(south > 1.5 * north, "south face gets more winter sun than the north face")
	check(flat > 100.0 and flat < 200.0, "annual mean shortwave on flat ground at 46 N with half cloud (%.0f W/m2)" % flat)
	check(dm.get_sky_view_at(Vector3(-11000, 0, -11000)) > 0.97, "open sky on flat ground (%.3f)" % dm.get_sky_view_at(Vector3(-11000, 0, -11000)))
	var sx_lee := dm.get_sx_at(Vector3(3000, 0, 0), 270.0)
	var sx_wind := dm.get_sx_at(Vector3(-3000, 0, 0), 270.0)
	check(sx_lee > 0.0 and sx_wind < 0.0, "Sx sheltered in the lee, exposed windward (%.1f, %.1f degrees)" % [sx_lee, sx_wind])
	check(dm.get_snow_fraction_at(top) > dm.get_snow_fraction_at(Vector3(-11000, 0, 0)) + 0.1, "more of the year's precipitation falls as snow on top (%.2f against %.2f)" % [dm.get_snow_fraction_at(top), dm.get_snow_fraction_at(Vector3(-11000, 0, 0))])
	var before := dm.get_precipitation_at(wind_probe())
	dm.bake()
	check(dm.has_climate() and dm.get_precipitation_at(wind_probe()) == before, "bake() keeps the climate")

	# Precipitation needs a range at its own scale: 2000 m, sigma 15 km, on 150 km (linear theory wants
	# N h / U near 1 or below; a 3 km hill only shifts its rain over the crest).
	hill.set("height", 2000.0)
	hill.set("sigma", 15000.0)
	dm.size = 150000.0
	r.sun_cell = 500.0
	r.precipitation_cell = 1000.0
	r.horizon_distance = 0.0
	dm.bake_climate()
	var far := dm.get_precipitation_at(Vector3(-60000, 0, 0))
	var foot := dm.get_precipitation_at(Vector3(-40000, 0, 0))
	var wind := dm.get_precipitation_at(Vector3(-15000, 0, 0))
	var lee := dm.get_precipitation_at(Vector3(15000, 0, 0))
	print("     range: precipitation mm/yr far upwind %.0f, foot %.0f, windward %.0f, top %.0f, lee %.0f" % [far, foot, wind, dm.get_precipitation_at(top), lee])
	check(wind > 2.0 * foot and lee < 0.6 * far, "windward slope wetter, rain shadow in the lee")
	check(absf(far - 900.0) < 0.35 * 900.0, "near the background far upwind (%.0f mm)" % far)
	check(lee >= 0.4 * 900.0 - 1.0, "rain shadow keeps the floor share of the background (%.0f mm)" % lee)
	var best_x := 0.0
	var best := -INF
	for i in 41:
		var x := -40000.0 + i * 2000.0
		var p := dm.get_precipitation_at(Vector3(x, 0, 0))
		if p > best:
			best = p
			best_x = x
	check(best_x < 0.0, "wettest point upwind of the crest (x %.0f m)" % best_x)
	check(dm.get_aridity_at(Vector3(15000, 0, 0)) < dm.get_aridity_at(Vector3(-15000, 0, 0)), "drier in the rain shadow (aridity index %.2f against %.2f)" % [dm.get_aridity_at(Vector3(15000, 0, 0)), dm.get_aridity_at(Vector3(-15000, 0, 0))])

	# The generated world (4 km at 16 m), its own sea and lakes as the air's floor.
	var dm2 := TerrainDataMap3D.new()
	dm2.bake_on_ready = false
	dm2.size = WORLD
	dm2.resolution = 64
	add_child(dm2)
	dm2.height_source = dm2.get_path_to(gen)
	dm2.geology_source = dm2.get_path_to(gen)
	dm2.climate_recipe = ClimateRecipe.new()
	check(dm2.bake_climate() == OK, "bake_climate() on the generated world: %.0f ms at 256 x 256 cells" % dm2.get_climate_bake_time_msec())
	dm.queue_free()
	dm2.queue_free()
	hill.queue_free()
	await get_tree().process_frame


func wind_probe() -> Vector3:
	return Vector3(-3000, 0, 0)



func _save_climate(path: String) -> void:
	var dm := TerrainDataMap3D.new()
	dm.bake_on_ready = false
	dm.size = WORLD
	add_child(dm)
	dm.height_source = dm.get_path_to(gen)
	dm.geology_source = dm.get_path_to(gen)
	var r := ClimateRecipe.new()
	r.wind_from = float(args.get("wind", "270"))
	dm.climate_recipe = r
	dm.bake_climate()
	print("climate %.0f ms" % dm.get_climate_bake_time_msec())
	var res := 384
	var img := Image.create(res * 2 + 8, res * 2 + 8, false, Image.FORMAT_RGB8)
	img.fill(Color.WHITE)
	var rect := dm.get_climate_rect()
	var ranges := [[INF, -INF], [INF, -INF], [INF, -INF], [INF, -INF]]
	var vals := []
	for z in res:
		for x in res:
			var p := Vector3(rect.position.x + (x + 0.5) * rect.size.x / res, 0, rect.position.y + (z + 0.5) * rect.size.y / res)
			var v := [dm.get_temperature_at(p), dm.get_precipitation_at(p), dm.get_winter_shortwave_at(p), dm.get_snow_fraction_at(p)]
			vals.append(v)
			for k in 4:
				ranges[k][0] = minf(ranges[k][0], v[k])
				ranges[k][1] = maxf(ranges[k][1], v[k])
	var lo := [Color(0.1, 0.2, 0.7), Color(0.75, 0.6, 0.35), Color(0.05, 0.05, 0.15), Color(0.25, 0.3, 0.2)]
	var hi := [Color(0.9, 0.25, 0.1), Color(0.1, 0.3, 0.85), Color(1.0, 0.85, 0.3), Color(1, 1, 1)]
	for z in res:
		for x in res:
			var v: Array = vals[z * res + x]
			var wet := gen.get_water_depth(Vector3(rect.position.x + (x + 0.5) * rect.size.x / res, 0, rect.position.y + (z + 0.5) * rect.size.y / res)) > 0.0
			for k in 4:
				var t: float = (v[k] - ranges[k][0]) / maxf(ranges[k][1] - ranges[k][0], 1e-6)
				var c: Color = (lo[k] as Color).lerp(hi[k], t)
				if wet:
					c = c.lerp(Color(0.15, 0.2, 0.3), 0.6)
				img.set_pixel((k % 2) * (res + 8) + x, (k / 2) * (res + 8) + z, c)
	img.save_png(path)
	var names := ["temperature C", "precipitation mm/yr", "winter shortwave W/m2", "snow fraction"]
	for k in 4:
		print("%s: %.2f to %.2f" % [names[k], ranges[k][0], ranges[k][1]])
	print("saved ", path)


func _stats() -> Dictionary:
	var g := gen.get_grid_size()
	var h := gen.get_height_image()
	var a := gen.get_drainage_image()
	var cell := gen.cell_size
	var hs: Array[float] = []
	var slope_sum := 0.0
	var steep := 0
	var channels := 0
	var crests := 0
	for z in range(1, g.y - 1):
		for x in range(1, g.x - 1):
			var v := h.get_pixel(x, z).r
			hs.append(v)
			var hx := (h.get_pixel(x + 1, z).r - h.get_pixel(x - 1, z).r) / (2.0 * cell)
			var hz := (h.get_pixel(x, z + 1).r - h.get_pixel(x, z - 1).r) / (2.0 * cell)
			var sl := rad_to_deg(atan(sqrt(hx * hx + hz * hz)))
			slope_sum += sl
			steep += 1 if sl > 35.0 else 0
			channels += 1 if a.get_pixel(x, z).r >= 1.0e5 else 0
			var dl := v - h.get_pixel(x - 1, z).r
			var dr := v - h.get_pixel(x + 1, z).r
			var du := v - h.get_pixel(x, z - 1).r
			var dd := v - h.get_pixel(x, z + 1).r
			crests += 1 if (dl > 0.1 * cell and dr > 0.1 * cell) or (du > 0.1 * cell and dd > 0.1 * cell) else 0
	hs.sort()
	var n := hs.size()
	return {
		"p10": hs[n / 10], "p50": hs[n / 2], "p90": hs[n * 9 / 10], "p99": hs[n * 99 / 100], "max": hs[n - 1],
		"slope": slope_sum / n, "steep": float(steep) / n, "channels": float(channels) / n, "crests": float(crests) / n,
		"img": h, "msec": gen.get_last_generate_msec(),
	}


func _blurred(img: Image, r: int) -> PackedFloat32Array:
	var w := img.get_width()
	var out := PackedFloat32Array()
	out.resize(w * w)
	for z in w:
		for x in w:
			var s := 0.0
			var c := 0
			for dz in range(-r, r + 1, maxi(r / 2, 1)):
				for dx in range(-r, r + 1, maxi(r / 2, 1)):
					s += img.get_pixel(clampi(x + dx, 0, w - 1), clampi(z + dz, 0, w - 1)).r
					c += 1
			out[z * w + x] = s / c
	return out


func _compare(prefix: String) -> void:
	gen.refine_steps = int(args.get("refine", "60"))
	gen.coarse_levels = 0
	gen.generate()
	var one := _stats()
	_save_map(prefix + "_one.png")
	gen.coarse_levels = int(args.get("levels", "-1"))
	gen.generate()
	var multi := _stats()
	_save_map(prefix + "_multi.png")
	print("levels %d, refine %d" % [gen.get_last_coarse_levels(), gen.refine_steps])
	for k in ["msec", "p10", "p50", "p90", "p99", "max", "slope", "steep", "channels", "crests"]:
		print("%-9s one %10.3f  multi %10.3f  ratio %.3f" % [k, one[k], multi[k], float(multi[k]) / maxf(absf(float(one[k])), 1e-9)])
	# Large-scale agreement: heights blurred over ~250 m.
	var r := int(round(128.0 / gen.cell_size))
	var b1 := _blurred(one["img"], r)
	var b2 := _blurred(multi["img"], r)
	var m1 := 0.0
	var m2 := 0.0
	for i in b1.size():
		m1 += b1[i]
		m2 += b2[i]
	m1 /= b1.size()
	m2 /= b2.size()
	var sxy := 0.0
	var sxx := 0.0
	var syy := 0.0
	var mad := 0.0
	for i in b1.size():
		sxy += (b1[i] - m1) * (b2[i] - m2)
		sxx += (b1[i] - m1) * (b1[i] - m1)
		syy += (b2[i] - m2) * (b2[i] - m2)
		mad += absf(b1[i] - b2[i])
	print("large-scale correlation %.4f, mean abs difference %.2f m" % [sxy / sqrt(sxx * syy), mad / b1.size()])


func _soil_histogram() -> Dictionary:
	# Hillslopes only (drainage under 1e5 m2, more than 40 m above the lowest ground): per 5 degree slope band, the
	# share of cells, mean soil depth and mean rock exposure.
	var g := gen.get_grid_size()
	var h := gen.get_height_image()
	var a := gen.get_drainage_image()
	var cell := gen.cell_size
	var r := gen.get_world_rect()
	var bins := []
	for b in 14:
		bins.append([0, 0.0, 0.0, 0])
	var total := 0
	var lo := INF
	for z in g.y:
		for x in g.x:
			lo = minf(lo, h.get_pixel(x, z).r)
	for z in range(1, g.y - 1):
		for x in range(1, g.x - 1):
			if a.get_pixel(x, z).r >= 1.0e5 or h.get_pixel(x, z).r < lo + 40.0:
				continue
			var hx := (h.get_pixel(x + 1, z).r - h.get_pixel(x - 1, z).r) / (2.0 * cell)
			var hz := (h.get_pixel(x, z + 1).r - h.get_pixel(x, z - 1).r) / (2.0 * cell)
			var sl := rad_to_deg(atan(sqrt(hx * hx + hz * hz)))
			var p := Vector3(r.position.x + x * cell, 0, r.position.y + z * cell)
			var b := mini(int(sl / 5.0), 13)
			bins[b][0] += 1
			bins[b][1] += gen.get_soil_depth(p)
			bins[b][2] += gen.get_rock_exposure(p)
			bins[b][3] += 1 if gen.get_soil_depth(p) >= 0.2 else 0
			total += 1
	print("slope band   share   soil m   rock   cells with >= 0.2 m soil")
	var out := {"total": total, "bins": bins}
	for b in 14:
		var c: int = bins[b][0]
		if c == 0:
			continue
		print("%2d-%2d deg   %5.1f%%   %5.2f   %5.2f   %5.1f%%" % [b * 5, b * 5 + 5, 100.0 * c / total, bins[b][1] / c, bins[b][2] / c, 100.0 * bins[b][3] / c])
	return out


func _print_waterfalls() -> void:
	var falls := gen.get_waterfalls(float(args.get("river-area", "1e5")), 5.0)
	print("waterfalls: %d" % falls.size())
	for f in falls:
		print("  %.0f m high at %s, %.1f m wide, %.2f m3/s, hardness %.2f" % [f["height"], f["top"], f["width"], f["discharge"], f["hardness"]])
	if args.has("profiles"):
		# Long profiles of every river: x, z, height, hardness, drainage area per point (CSV).
		var fd := FileAccess.open(args["profiles"], FileAccess.WRITE)
		var rivers := gen.get_rivers(float(args.get("river-area", "1e5")))
		for r in rivers.size():
			for p0 in rivers[r]["points"]:
				# On the grid node the river cell is at (the points are smoothed between them).
				var c := gen.cell_size
				var p := Vector3(snappedf(p0.x + WORLD * 0.5, c) - WORLD * 0.5, 0.0, snappedf(p0.z + WORLD * 0.5, c) - WORLD * 0.5)
				var g: Color = gen.get_geology_at(p)
				fd.store_line("%d,%.1f,%.1f,%.2f,%.2f,%.2f" % [r, p.x, p.z, gen.get_base_height(p), gen.get_rock_hardness(p), g.a])
		print("profiles ", args["profiles"])


func _runoff_recipe() -> ClimateRecipe:
	# A cool mountain climate: snow on the range in winter, melt in spring.
	var r := ClimateRecipe.new()
	r.temperature_january = float(args.get("jan", "-4"))
	r.temperature_july = float(args.get("jul", "15"))
	r.precipitation = float(args.get("precip", "1100"))
	r.precipitation_seasonality = float(args.get("season", "0"))
	r.precipitation_peak_month = float(args.get("peak", "0"))
	return r


func _runoff_report() -> void:
	gen.generate()
	var plain := gen.get_rivers(1.0e5)
	var t_plain := gen.get_last_generate_msec()
	gen.climate_recipe = _runoff_recipe()
	gen.generate()
	print("generate %.0f ms without climate, %.0f ms with (runoff %.0f ms); mean runoff %.0f mm a year" % [t_plain, gen.get_last_generate_msec(), gen.get_last_runoff_msec(), gen.get_mean_runoff()])
	var rivers := gen.get_rivers(1.0e5)
	var out: FileAccess = FileAccess.open(args["runoff"], FileAccess.WRITE) if args["runoff"] != "1" else null
	const MONTHS := ["J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"]
	for r in mini(rivers.size(), 6):
		var d: Dictionary = rivers[r]
		var pts: PackedVector3Array = d["points"]
		var m: PackedFloat32Array = d["monthly_discharge"]
		var line := ""
		for k in 12:
			line += "%s %.3f  " % [MONTHS[k], m[k]]
		print("river %d (head %.0f m): %.3f m3/s (was %.3f)  %s" % [r, pts[0].y, d["discharge"], plain[r]["discharge"] if r < plain.size() else 0.0, line])
		if out:
			out.store_line("river,%d,%s" % [r, ",".join(Array(m).map(func(v): return "%.4f" % v))])
	for h in [0.0, 150.0, 300.0, 450.0]:
		var best := Vector3.ZERO
		var bd := 1e9
		for z in range(-1900, 1900, 64):
			for x in range(-1900, 1900, 64):
				var q := Vector3(x, 0, z)
				var dh := absf(gen.get_base_height(q) - h)
				if dh < bd:
					bd = dh
					best = Vector3(x, gen.get_base_height(q), z)
		var sw := ""
		for k in 12:
			sw += "%.0f " % gen.get_snow_water_at(best, k)
		print("at %4.0f m: runoff %.0f mm a year; snow water by month end: %s" % [best.y, gen.get_runoff_at(best), sw])
		if out:
			var snow := []
			for k in 12:
				snow.append("%.1f" % gen.get_snow_water_at(best, k))
			out.store_line("snow,%.0f,%s" % [best.y, ",".join(snow)])

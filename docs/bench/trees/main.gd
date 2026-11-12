extends Node3D
# TreeGenerator bench: grows each species preset open-grown and in a closed stand, prints the
# measurements and generation cost, checks them, and renders lineups or single trees.
#
# godot --path docs/bench/trees --rendering-driver vulkan -- [options]
#   --test            grow every preset, print the table and PASS/FAIL, quit
#   --out=path.png    render (path traced unless --raster) and quit
#   --raster          raster only
#   --spp=N           path tracing samples (default 16)
#   --preset=N        one species (index into NAMES below); default: all of them in a row
#   --presets=a,b,c   several species
#   --stand=open|forest|both   growth setting (default both: open in front, forest-grown behind)
#   --seed=N          (default 1)
#   --view=side|crop|trunk  side: whole trees; crop: a 100% crop into the crown of the first tree;
#                     trunk: the first tree's bark at arm's length
#   --set=name:value  override a growth property on every preset, e.g. --set=shed_ratio:0.2
#   --view=fork       where the first limbs leave the first tree's stem (junctions)
#   --view=leaf       the crop view zoomed in to single leaves
#   --view=base|log  the first tree's foot (moss), or the last plant (the log with --decays)
#   --day=N           day of year: leaves by season, from a stand-in climate (see season_for)
#   --decays          deadwood: each preset living, then decay classes 1-5, then a log (class 3)
#   --stands ...      stands and placement instead: see stands.gd
#   --wind ...        wind on rigged trees: see wind.gd

const NAMES := ["oak", "beech", "birch", "Scots pine", "Norway spruce", "hazel", "hawthorn", "lime", "sycamore maple",
		"ash", "horse chestnut", "black alder", "white willow", "European larch", "silver fir", "rowan", "elder", "holly", "yew", "box",
		"sugar maple", "red maple", "red oak", "white oak", "white pine", "hemlock", "American beech", "paper birch",
		"yellow birch", "witch-hazel", "mountain laurel"]
const SHRUBS := [5, 6, 16, 17, 18, 19, 29, 30] # Shrubs and evergreens that stay leafy low in a stand.
# Stem spacing in a mature stand (m), as in TreeGenerator's habitat table.
const STAND_SPACING := [9.0, 7.0, 5.0, 5.5, 4.5, 3.0, 3.0, 7.0, 7.0, 7.0, 9.0, 5.0, 7.0, 6.0, 5.0, 4.0, 3.0, 4.0, 5.0, 2.5,
		7.0, 6.0, 8.0, 9.0, 6.0, 5.0, 7.0, 4.5, 6.0, 3.0, 2.5]
# Plausible heights (m) for the default ages: mature trees, shrubs at full size.
const HEIGHT_RANGE := [Vector2(12, 30), Vector2(14, 34), Vector2(10, 28), Vector2(12, 32), Vector2(14, 38), Vector2(2.5, 8), Vector2(3, 10),
		Vector2(12, 38), Vector2(12, 30), Vector2(14, 40), Vector2(12, 28), Vector2(10, 26), Vector2(10, 25), Vector2(15, 38),
		Vector2(15, 40), Vector2(5, 15), Vector2(2.5, 8), Vector2(4, 15), Vector2(5, 20), Vector2(1.5, 6),
		Vector2(15, 37), Vector2(12, 30), Vector2(15, 35), Vector2(15, 32), Vector2(18, 46), Vector2(15, 40), Vector2(15, 35),
		Vector2(12, 30), Vector2(15, 32), Vector2(3, 9), Vector2(1.5, 6)]

var junction_side := 1 # --junction-side=-1: the junction view from the limb's other side.
var test := false
var raster := false
var spp := 16
var out := ""
var preset := -1
var pick: Array = []
var stand := "both"
var seed := 1
var view := "side"
var overrides := {}
var decays := false
var day := -1
var fails := 0

func _ready() -> void:
	if "--wind" in OS.get_cmdline_user_args():
		# Wind on rigged trees: wind.gd.
		var wargs := {}
		for a in OS.get_cmdline_user_args():
			var kv := a.trim_prefix("--").split("=", true, 1)
			wargs[kv[0]] = kv[1] if kv.size() > 1 else ""
		var wb: Node3D = load("res://wind.gd").new()
		add_child(wb)
		wb.run(wargs)
		return
	if "--stands" in OS.get_cmdline_user_args():
		# Stands and placement: stands.gd.
		var args := {}
		for a in OS.get_cmdline_user_args():
			var kv := a.trim_prefix("--").split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else ""
		var bench: Node3D = load("res://stands.gd").new()
		add_child(bench)
		bench.run(args)
		return
	for a in OS.get_cmdline_user_args():
		if a == "--test":
			test = true
		elif a == "--raster":
			raster = true
		elif a.begins_with("--spp="):
			spp = int(a.substr(6))
		elif a.begins_with("--out="):
			out = a.substr(6)
		elif a.begins_with("--preset="):
			preset = int(a.substr(9))
		elif a.begins_with("--presets="):
			for v in a.substr(10).split(","):
				pick.append(int(v))
		elif a.begins_with("--stand="):
			stand = a.substr(8)
		elif a.begins_with("--seed="):
			seed = int(a.substr(7))
		elif a.begins_with("--view="):
			view = a.substr(7)
		elif a == "--decays":
			decays = true
		elif a.begins_with("--day="):
			day = int(a.substr(6))
		elif a.begins_with("--junction-side="):
			junction_side = int(a.substr(16))
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":")
			# "true"/"false" as 1/0: float("true") is 0, which silently turned a switch off.
			overrides[kv[0]] = 1.0 if kv[1] == "true" else (0.0 if kv[1] == "false" else float(kv[1]))
	if test:
		_run_test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	_build_scene()
	_render_later()

func _gen(p: int, forest: bool, s: int) -> Dictionary:
	var g := TreeGenerator.new()
	g.preset = p
	g.seed = s
	for k in overrides:
		g.set(k, bool(overrides[k]) if typeof(g.get(k)) == TYPE_BOOL else overrides[k])
	g.forest_shade = 0.6 if forest else 0.0
	if day >= 0:
		g.season = weather_season(day)
	# The spacing make_species() grows the stand form at: the stand spacing times 1.3.
	g.forest_spacing = STAND_SPACING[p] * 1.3
	var mesh := g.generate()
	var tris := 0
	for i in mesh.get_surface_count():
		tris += mesh.surface_get_array_index_len(i) / 3
	return {"mesh": mesh, "height": g.get_height(), "crown_radius": g.get_crown_radius(), "crown_base": g.get_crown_base(),
		"internodes": g.get_internode_count(), "leaves": g.get_leaf_count(), "dead": g.get_dead_branch_count(),
		"shed": g.get_shed_count(), "tris": tris, "ms": g.get_generation_usec() / 1000.0,
		"capsules": g.get_collision_capsules(), "features": g.get_veteran_features()}

# The shared season state (WeatherMap3D.get_season(), section 2c) for a 0 C January / 18 C July
# climate at the weather's own latitude (46 N without a SkyClock) on a given day; the stand-in
# below if the weather module gives none.
static func weather_season(d: int) -> Dictionary:
	var w := WeatherMap3D.new()
	var c := ClimateRecipe.new()
	c.temperature_january = 0.0
	c.temperature_july = 18.0
	w.climate = c
	w.season_day = d
	var s := w.get_season()
	w.free()
	return s if not s.is_empty() else season_for(d, 46.0)

# A stand-in for WeatherMap3D.get_season() (docs/ideas/trees.md 2c): a temperate climate at
# 50 N, daily mean 0 C mid-January to 18 C mid-July, integrated from 1 Jan (forcing, degree days
# above 5 C), from 1 Nov (chilling, days of 0-5 C) and, once days shorten below 13.2 h after
# midsummer, Delpierre et al. 2009's senescence sum.
static func season_for(d: int, lat := 50.0) -> Dictionary:
	var temp := func(x: float) -> float: return 9.0 + 9.0 * cos(TAU * (x - 200.0) / 365.0)
	var photo := func(x: float) -> float:
		# Forsythe et al. 1995 (CBM model), day length with civil twilight 0.8333 deg.
		var th := 0.2163108 + 2.0 * atan(0.9671396 * tan(0.0086 * (x - 186.0)))
		var ph := asin(0.39795 * cos(th))
		var c := (sin(deg_to_rad(0.8333)) + sin(deg_to_rad(lat)) * sin(ph)) / (cos(deg_to_rad(lat)) * cos(ph))
		return 24.0 - 24.0 / PI * acos(clampf(c, -1.0, 1.0))
	var forcing := 0.0
	var senescence := 0.0
	var chilling := 0.0
	var started := false
	for x in range(1, d + 1):
		var t: float = temp.call(float(x))
		forcing += maxf(t - 5.0, 0.0)
		var p: float = photo.call(float(x))
		if x > 172 and p < 13.2:
			started = true
		if started:
			senescence += maxf(20.0 - t, 0.0) * pow(p / 13.2, 2.0)
	for x in (range(305, d + 1) if d >= 305 else range(305, 366) + range(1, d + 1)):
		var t: float = temp.call(float(x))
		if t >= 0.0 and t <= 5.0:
			chilling += 1.0
	return {"day_of_year": float(d), "latitude_deg": lat, "photoperiod_h": photo.call(float(d)), "forcing": forcing,
		"chilling": chilling, "senescence": senescence, "snow_depth": 0.0, "soil_frost": 0.0}

func _check(ok: bool, what: String) -> void:
	if not ok:
		fails += 1
	print(("PASS " if ok else "FAIL ") + what)

func _run_test() -> void:
	print("species        stand   height  crown_r  crown_base  internodes  leaves   dead  shed  tris      ms")
	for p in (pick if not pick.is_empty() else ([preset] if preset >= 0 else range(NAMES.size()))):
		var o := _gen(p, false, seed)
		var f := _gen(p, true, seed)
		for pair in [["open", o], ["forest", f]]:
			var d: Dictionary = pair[1]
			print("%-14s %-7s %6.1f  %7.1f  %10.1f  %10d  %6d  %5d  %4d  %8d  %6.0f" % [NAMES[p], pair[0], d.height, d.crown_radius, d.crown_base, d.internodes, d.leaves, d.dead, d.shed, d.tris, d.ms])
		var r: Vector2 = HEIGHT_RANGE[p]
		_check(o.height >= r.x and o.height <= r.y, "%s open-grown height %.1f m in %.0f-%.0f m" % [NAMES[p], o.height, r.x, r.y])
		if not p in SHRUBS and not p in [14, 25]: # Shrubs and shade-tolerant fir and hemlock keep low foliage in a stand.
			_check(f.crown_base > o.crown_base, "%s forest-grown crown base %.1f m above open-grown %.1f m" % [NAMES[p], f.crown_base, o.crown_base])
		_check(f.crown_radius < o.crown_radius, "%s forest-grown crown narrower (%.1f m vs %.1f m)" % [NAMES[p], f.crown_radius, o.crown_radius])
		var again := _gen(p, false, seed)
		_check(again.internodes == o.internodes and is_equal_approx(again.height, o.height), "%s deterministic for one seed" % NAMES[p])
	_season_test()
	var other := _gen(0, false, seed + 1)
	var first := _gen(0, false, seed)
	_check(other.internodes != first.internodes, "oak seeds give different trees")

# Phenology (trees.md 2c): order of leaf-out and colour, bare winter crowns, evergreens unchanged.
func _season_test() -> void:
	var ls := func(p: int, d: int) -> Dictionary: return TreeGenerator.get_leaf_season(p, season_for(d))
	var summer: Dictionary = ls.call(0, 182)
	_check(summer.leaf_out == 1.0 and summer.autumn == 0.0 and summer.fallen_fraction == 0.0, "oak in full leaf on 1 July")
	var winter: Dictionary = ls.call(0, 20)
	_check(winter.leaf_out == 0.0 and winter.fallen_fraction == 1.0, "oak bare in January")
	var first := -1
	var order := []
	for p in [16, 5, 2, 10, 1, 0, 9]: # Elder, hazel, birch, horse chestnut, beech, oak, ash.
		for d in range(60, 200):
			if ls.call(p, d).leaf_out > 0.0:
				order.append(d)
				break
	var sorted := order.duplicate()
	sorted.sort()
	_check(order.size() == 7 and order == sorted and order[0] < order[6] - 10, "leaf-out order elder..ash, days %s" % str(order))
	_check(ls.call(21, 285).autumn > ls.call(0, 285).autumn, "red maple turns before oak (12 Oct: %.2f vs %.2f)" % [ls.call(21, 285).autumn, ls.call(0, 285).autumn])
	var late: Dictionary = ls.call(2, 330)
	_check(late.fallen_fraction > 0.9, "birch bare by late November (%.2f fallen)" % late.fallen_fraction)
	var real := weather_season(290)
	var mine := season_for(290, real.get("latitude_deg", 50.0))
	_check(absf(real.forcing - mine.forcing) < 0.1 * mine.forcing and absf(real.senescence - mine.senescence) < 0.15 * mine.senescence + 10.0,
			"WeatherMap3D season on 17 Oct matches the stand-in (forcing %.0f vs %.0f, senescence %.0f vs %.0f)" % [real.forcing, mine.forcing, real.senescence, mine.senescence])
	var pine: Dictionary = ls.call(3, 20)
	_check(pine.leaf_out == 1.0 and pine.fallen_fraction == 0.0, "Scots pine keeps its needles in January")
	var g := TreeGenerator.new()
	g.preset = 2
	g.generate()
	var full := g.get_leaf_count()
	g.season = season_for(20)
	var bare_mesh := g.generate()
	var bare := g.get_leaf_count()
	var bare_ok := bare_mesh != null and bare_mesh.get_surface_count() > 0
	g.season = season_for(300)
	g.generate()
	var thin := g.get_leaf_count()
	_check(bare == 0 and bare_ok and thin > 0 and thin < full, "birch leaves: summer %d, 27 Oct %d, January %d (bark still built)" % [full, thin, bare])

func _place(mesh: Mesh, pos: Vector3) -> void:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = pos
	add_child(mi)

func _build_scene() -> void:
	var presets: Array = pick if not pick.is_empty() else ([preset] if preset >= 0 else range(NAMES.size()))
	var rows: Array = []
	if stand == "open" or stand == "both":
		rows.append(false)
	if stand == "forest" or stand == "both":
		rows.append(true)
	var x := 0.0
	var tallest := 0.0
	var first_info := {}
	var xs: Array[float] = []
	if decays:
		rows = [0, 1, 2, 3, 4, 5, 6]
	for p in presets:
		var w := 0.0
		var infos := []
		for forest in rows:
			if decays:
				overrides["decay_class"] = mini(int(forest), 5) if int(forest) < 6 else 3
				overrides["fallen"] = int(forest) == 6
			var d := _gen(p, (stand == "forest") if decays else forest, seed)
			infos.append(d)
			w = max(w, d.crown_radius)
			tallest = max(tallest, d.height)
			if first_info.is_empty():
				first_info = d
		for i in infos.size():
			var d: Dictionary = infos[i]
			x += d.crown_radius
			xs.append(x)
			_place(d.mesh, Vector3(x, 0, 0))
			x += d.crown_radius + 1.0
			print("%s %s: %.1f m tall, %d internodes, %d tris, %.0f ms" % [NAMES[p], ("class %s" % rows[i]) if decays else ("forest" if rows[i] else "open"), d.height, d.internodes, d.tris, d.ms])
		x += 2.0
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(600, 600)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)
	var sky := ProceduralSkyMaterial.new()
	var sk := Sky.new()
	sk.sky_material = sky
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sk
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	# Needles are a few millimetres wide, far under a pixel at lineup distance: without
	# multisampling most miss every sample point and conifers read as bare sticks. Games run TAA
	# or DLSS, which integrate that coverage; 8x MSAA stands in for it here.
	get_viewport().msaa_3d = Viewport.MSAA_8X
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 35, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 300.0
	if view in ["bole", "junction", "fork", "leaf"]:
		# Close-ups: the cascades over 300 m blur the leaves' shadow flecks into blocky speckle.
		sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var vet_az := 0.0
	if view == "veteran":
		# The lower 4 m of the first tree's bole, facing its basal hollow (else its largest
		# feature), lit side-on by a low sun from the camera's side so hollows and burls model.
		var best_k := 99
		for f in first_info.features:
			if int(f.kind) != 2 and int(f.kind) < best_k:
				best_k = int(f.kind)
				vet_az = float(f.azimuth)
		if OS.has_environment("VAZ"):
			vet_az = float(OS.get_environment("VAZ"))
		print("veteran view azimuth %.4f" % vet_az)
		for f in first_info.features:
			print("feature ", f)
		var la := vet_az + deg_to_rad(55.0)
		var target := Vector3(xs[0], 1.4, 0)
		var ldir := Vector3(cos(la) * cos(deg_to_rad(18.0)), sin(deg_to_rad(18.0)), sin(la) * cos(deg_to_rad(18.0)))
		sun.look_at_from_position(target + ldir * 10.0, target)
		sun.directional_shadow_max_distance = 40.0
	var cam := Camera3D.new()
	cam.fov = 40.0
	add_child(cam)
	var cx := x * 0.5
	if view == "trunk":
		# Bark at arm's length: 2.2 m from the first tree's trunk, at chest height.
		cam.position = Vector3(xs[0] + 0.3, 1.4, 2.2)
		cam.look_at(Vector3(xs[0], 1.2, 0))
		cam.fov = 40.0
	elif view == "base":
		# The foot of the first tree, where moss gathers: 3 m away, looking down at the flare.
		cam.position = Vector3(xs[0] + 0.6, 1.1, 3.0)
		cam.look_at(Vector3(xs[0], 0.45, 0))
		cam.fov = 45.0
		if OS.has_environment("CAM"): # As on the bole view: "dx,y,z,lx,ly,lz,fov" from the tree.
			var cv := OS.get_environment("CAM").split_floats(",")
			cam.position = Vector3(xs[0] + cv[0], cv[1], cv[2])
			cam.look_at(Vector3(xs[0] + cv[3], cv[4], cv[5]))
			cam.fov = cv[6]
	elif view == "veteran":
		var out := Vector3(cos(vet_az), 0, sin(vet_az))
		var cy := float(OS.get_environment("VCAMY")) if OS.has_environment("VCAMY") else 1.5
		cam.position = Vector3(xs[0], cy, 0) + out * 4.6
		cam.look_at(Vector3(xs[0], 1.6 if cy > 1.0 else 0.4, 0))
		cam.fov = 50.0
	elif view == "log":
		# The last plant in the row (the log with --decays) from 3.5 m, slightly above.
		cam.position = Vector3(xs[xs.size() - 1] + 1.0, 1.6, 3.5)
		cam.look_at(Vector3(xs[xs.size() - 1], 0.3, 0))
		cam.fov = 50.0
	elif view == "fork":
		# Where the first limbs leave the stem: the junctions (trees.md 2d).
		var cb: float = first_info.crown_base
		cam.position = Vector3(xs[0] + 2.2, cb + 0.6, 2.6)
		cam.look_at(Vector3(xs[0], cb + 0.4, 0))
		cam.fov = 40.0
	elif view == "bole":
		# Standing 4.5 m from the first tree, eye at 1.3 m, looking at where the low limbs leave.
		cam.position = Vector3(xs[0] + 0.8, 1.3, 4.5)
		cam.look_at(Vector3(xs[0], 1.9, 0))
		cam.fov = 60.0
		if OS.has_environment("CAM"):
			var cv := OS.get_environment("CAM").split_floats(",")
			cam.position = Vector3(xs[0] + cv[0], cv[1], cv[2])
			cam.look_at(Vector3(xs[0] + cv[3], cv[4], cv[5]))
			cam.fov = cv[6]
	elif view == "junction":
		# Close on where a low limb leaves the first tree's trunk (trees.md 2d): the lowest limb
		# of 4 cm or more, seen from its side at 2 m, a little below.
		var caps: PackedFloat32Array = first_info.capsules
		var best := -1
		for c in range(0, caps.size(), 8):
			var a := Vector3(caps[c], caps[c + 1], caps[c + 2])
			var b := Vector3(caps[c + 3], caps[c + 4], caps[c + 5])
			var dir := (b - a).normalized()
			# A limb's first capsule starts on its parent's axis, off the trunk's foot. It must be
			# a true limb (under half the trunk's girth: thicker is a fork, which gets no weld),
			# spreading rather than steep or hanging.
			if caps[c + 6] < 0.04 or dir.y > 0.7 or dir.y < 0.0 or a.y < 0.8:
				continue
			var trunk_r := 0.0
			for t in range(0, caps.size(), 8):
				var ta := Vector3(caps[t], caps[t + 1], caps[t + 2])
				var tb := Vector3(caps[t + 3], caps[t + 4], caps[t + 5])
				if Vector2(ta.x, ta.z).length() < 0.4 and ta.y <= a.y and tb.y >= a.y and (tb - ta).normalized().y > 0.85:
					trunk_r = max(trunk_r, caps[t + 6])
			if trunk_r <= 0.0 or caps[c + 6] > 0.5 * trunk_r or Vector2(a.x, a.z).length() > trunk_r:
				continue
			if best < 0 or a.y < caps[best + 1]:
				best = c
		if best >= 0:
			var a := Vector3(caps[best], caps[best + 1], caps[best + 2])
			var b := Vector3(caps[best + 3], caps[best + 4], caps[best + 5])
			var hd := Vector3(b.x - a.x, 0, b.z - a.z).normalized()
			var side := hd.cross(Vector3.UP)
			if junction_side < 0:
				side = -side
			var target := Vector3(xs[0], 0, 0) + a + hd * caps[best + 6] * 2.0
			cam.position = target + side * 2.0 - hd * 0.6 + Vector3(0, -0.3, 0)
			cam.look_at(target)
		cam.fov = 40.0
	elif view == "crop" or view == "leaf":
		var h: float = first_info.height
		cam.position = Vector3(xs[0] + first_info.crown_radius * 0.9, h * 0.62, 4.0)
		cam.look_at(Vector3(xs[0], h * 0.62, 0))
		cam.fov = 30.0 if view == "crop" else 7.0
	else:
		var span: float = max(x, tallest * 1.6)
		var dist := span * 0.5 / tan(deg_to_rad(cam.fov * 0.5)) * 0.62 + 6.0
		cam.position = Vector3(cx, tallest * 0.45, dist)
		cam.look_at(Vector3(cx, tallest * 0.42, -6))

func _render_later() -> void:
	if OS.has_environment("WIRE"):
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_WIREFRAME
	for i in (8 if raster else 12):
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(1 if fails > 0 else 0)

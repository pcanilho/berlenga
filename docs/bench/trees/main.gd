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
#   --view=side|crop  side: whole trees; crop: a 100% crop into the crown of the first tree
#   --set=name:value  override a growth property on every preset, e.g. --set=shed_ratio:0.2
#   --stands ...      stands and placement instead: see stands.gd

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
var fails := 0

func _ready() -> void:
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
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":")
			overrides[kv[0]] = float(kv[1])
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
		g.set(k, overrides[k])
	g.forest_shade = 0.6 if forest else 0.0
	# The spacing make_species() grows the stand form at: the stand spacing times 1.3.
	g.forest_spacing = STAND_SPACING[p] * 1.3
	var mesh := g.generate()
	var tris := 0
	for i in mesh.get_surface_count():
		tris += mesh.surface_get_array_index_len(i) / 3
	return {"mesh": mesh, "height": g.get_height(), "crown_radius": g.get_crown_radius(), "crown_base": g.get_crown_base(),
		"internodes": g.get_internode_count(), "leaves": g.get_leaf_count(), "dead": g.get_dead_branch_count(),
		"shed": g.get_shed_count(), "tris": tris, "ms": g.get_generation_usec() / 1000.0}

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
	var other := _gen(0, false, seed + 1)
	var first := _gen(0, false, seed)
	_check(other.internodes != first.internodes, "oak seeds give different trees")

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
	for p in presets:
		var w := 0.0
		var infos := []
		for forest in rows:
			var d := _gen(p, forest, seed)
			infos.append(d)
			w = max(w, d.crown_radius)
			tallest = max(tallest, d.height)
			if first_info.is_empty():
				first_info = d
		for i in infos.size():
			var d: Dictionary = infos[i]
			x += d.crown_radius
			if xs.is_empty():
				xs.append(x)
			_place(d.mesh, Vector3(x, 0, 0))
			x += d.crown_radius + 1.0
			print("%s %s: %.1f m tall, %d internodes, %d tris, %.0f ms" % [NAMES[p], "forest" if rows[i] else "open", d.height, d.internodes, d.tris, d.ms])
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
	add_child(sun)
	var cam := Camera3D.new()
	cam.fov = 40.0
	add_child(cam)
	var cx := x * 0.5
	if view == "crop":
		var h: float = first_info.height
		cam.position = Vector3(xs[0] + first_info.crown_radius * 0.9, h * 0.62, 4.0)
		cam.look_at(Vector3(xs[0], h * 0.62, 0))
		cam.fov = 30.0
	else:
		var span: float = max(x, tallest * 1.6)
		var dist := span * 0.5 / tan(deg_to_rad(cam.fov * 0.5)) * 0.62 + 6.0
		cam.position = Vector3(cx, tallest * 0.45, dist)
		cam.look_at(Vector3(cx, tallest * 0.42, -6))

func _render_later() -> void:
	for i in (8 if raster else 12):
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(1 if fails > 0 else 0)

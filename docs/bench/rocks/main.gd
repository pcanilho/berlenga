extends Node3D
# Rocks bench: RockGenerator shapes and RockField3D placement (docs/ideas/rocks.md).
#
# godot --path docs/bench/rocks --rendering-driver vulkan -- [options]
#   --test                checks: every rock type and family builds (size, axes, timing), transport rounds and beach
#                         wear flattens, then the field's six layers on a generated world; prints PASS/FAIL, quits
#   --gallery             every family (columns) of every rock type (rows), each scaled to a 1 m b axis
#   --rock=type,family    one rock close up (--seed=N, --distance=m transport, --weathering=0..1; --stand: a fin,
#                         hoodoo or stack stood on end, side on)
#   --world               a generated 2 km world (range, strata, glaciers, scree, rivers, a lake, the sea) with a
#                         RockField3D over it; --view=rockfall|scree|glacial|river|shore|cliff|tor|coast|landform puts the camera at the
#                         densest place of that layer (default scree)
#   --type=granite|limestone|sandstone|basalt|slate   the world's rock (default granite; soft beds limestone)
#   --stand-ins           with --world: the old grey spheres at the rockfall blocks instead of the field (before shots)
#   --no-rocks            with --world: no rocks at all
#   --form=stack|arch     with --view=coast: frame the biggest stack or arch; --form=pavement|fin|hoodoo with --view=landform
#   --seaward [--side=f --cam-height=m --look-height=m] with --view=coast: from the land side, looking out to sea
#   --no-ground-blend     the stones' feet not blended into the ground (for a before frame)
#   --wind=m/s            with --water: the sea's wind speed (and some swell)
#   --shadow-filter=N     the directional soft-shadow filter quality (0 hard .. 5 ultra)
#   --water [--sea-res=N] with --world: the generator's water bodies; --sea-res sets the sea's simulated grid (cells)
#   --settle=N            N more frames before the capture (near plant cells are built lazily)
#   --out=path.png        render and quit (path traced unless --raster); --spp=N (16); --size=WxH
#   --cam=x,y,z --look=x,y,z   a fixed camera; --sun=elevation,yaw (degrees)
#   --plants[=off]        with --world: the crevice plants of the view's stones (PlantGenerator.make_stones: scree,
#                         river or shore; VegetationField3D.obstacles from the field); =off places them hidden (before
#                         shots, same camera); --dist=m sets the camera's distance from the spot
#   --only-layer          with --world: only the view's layer is placed
#   --stone-density=k     with --world: the view's layer density times k (RockField3D.set_layer_density), e.g. a dense block field

const TYPES := ["granite", "limestone", "sandstone", "basalt", "slate"]
const FAMILIES := ["rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block", "corestone", "sea_stack", "sea_arch", "sea_platform", "hoodoo", "pavement", "fin"]
const LAYERS := ["rockfall", "scree", "glacial", "river", "shore", "cliff", "tor", "coast", "landform"]
const WORLD := 2048.0

var args := {}
var raster := false
var spp := 16
var out := ""
var plants_ready := false
var fails := 0
var gen: TerrainGenerator3D
var rocks: RockField3D
var cam: Camera3D


var breaking := false

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.substr(2).split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	raster = args.has("raster")
	spp = int(args.get("spp", "16"))
	out = args.get("out", "")
	if args.has("no-lod"):
		get_viewport().mesh_lod_threshold = 0.0
	if args.has("size"):
		var s: PackedStringArray = args["size"].split("x")
		get_window().size = Vector2i(int(s[0]), int(s[1]))
	cam = Camera3D.new()
	cam.fov = 50.0
	cam.far = 4000.0
	add_child(cam)
	_environment()
	if args.has("test"):
		_test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("world"):
		_world()
	elif args.has("rock"):
		_one_rock()
	elif args.has("families"):
		_families()
	else:
		_gallery()
	if args.has("cam"):
		var c: PackedStringArray = args["cam"].split(",")
		var l: PackedStringArray = args.get("look", "0,0,0").split(",")
		cam.position = Vector3(float(c[0]), float(c[1]), float(c[2]))
		cam.look_at(Vector3(float(l[0]), float(l[1]), float(l[2])))
	if args.has("sun"):
		# --sun=elevation,yaw in degrees (a low grazing sun shows relief)
		var sv: PackedStringArray = args["sun"].split(",")
		for n in get_children():
			if n is DirectionalLight3D:
				(n as DirectionalLight3D).rotation_degrees = Vector3(-float(sv[0]), float(sv[1]) if sv.size() > 1 else -50.0, 0)
	if out != "":
		# Single captures have no TAA to smooth the soft-low shadow kernel's per-pixel rotation (a dot
		# grid in penumbras): the high filter instead.
		# --shadow-filter=N overrides it (0 hard .. 5 soft ultra).
		var q := int(args.get("shadow-filter", str(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)))
		RenderingServer.directional_soft_shadow_filter_set_quality(q)
		RenderingServer.positional_soft_shadow_filter_set_quality(q)
		_render_later()


func check(ok: bool, what: String) -> void:
	print("%s  %s" % ["PASS" if ok else "FAIL", what])
	if not ok:
		fails += 1


func _environment() -> void:
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
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-32, -50, 0)
	sun.light_energy = 2.2
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 120.0
	add_child(sun)


func _rock(type: int, family: int, seed: int) -> ArrayMesh:
	var g := RockGenerator.new()
	g.rock_type = type
	g.family = family
	g.seed = seed
	if args.has("distance"):
		g.transport_distance = float(args["distance"])
	if args.has("weathering"):
		g.weathering = float(args["weathering"])
	g.simplify = not args.has("no-simplify")
	g.close_up = args.has("close-up")
	return g.generate()


func _plain_ground(size: float) -> void:
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(size, size)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.22, 0.2, 0.16)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)


# A rock as a lone MeshInstance3D at rest on y = 0, sunk a little, b axis `b` metres long.
func _place_mesh(mesh: ArrayMesh, b: float, pos: Vector3, yaw: float) -> void:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	var s := b * 0.5
	mi.transform = Transform3D(Basis(Vector3.UP, yaw).scaled(Vector3(s, s, s)), pos + Vector3(0, -float(mesh.get_meta("rock_bottom", -0.5)) * s - 0.04 * b, 0))
	add_child(mi)


func _gallery() -> void:
	_plain_ground(60.0)
	var t0 := Time.get_ticks_msec()
	for ti in TYPES.size():
		for fi in FAMILIES.size():
			var m := _rock(ti, fi, int(args.get("seed", "3")) + fi)
			_place_mesh(m, 1.0, Vector3((fi - 2.5) * 1.6, 0, (ti - 2.0) * 1.6), 0.4 * fi)
	print("gallery: 30 rocks in %d ms" % (Time.get_ticks_msec() - t0))
	cam.position = Vector3(0, 7.5, 9.5)
	cam.look_at(Vector3(0, 0, 0.6))


# The six families on one ground at their real relative sizes (b axis): the big three behind, the small three in
# front where the camera can read them, sun low from the left so edges and facets show.
func _families() -> void:
	_plain_ground(60.0)
	for n in get_children():
		if n is DirectionalLight3D:
			(n as DirectionalLight3D).rotation_degrees = Vector3(-24, -72, 0)
			(n as DirectionalLight3D).directional_shadow_max_distance = 20.0
	var sd := int(args.get("seed", "3"))
	var rows := [
		["granite", "rockfall", 1.3, Vector3(-2.5, 0, 0), 0.5],
		["granite", "glacial", 1.7, Vector3(0.1, 0, -0.2), 2.4],
		["sandstone", "cliff_block", 1.3, Vector3(2.8, 0, 0), 0.25],
		# The small three close to the camera, so a 7 cm pebble still reads.
		["basalt", "scree", 0.32, Vector3(-0.6, 0, 3.7), 0.9],
		["limestone", "cobble", 0.22, Vector3(0.0, 0, 3.85), 2.0],
		["slate", "pebble", 0.07, Vector3(0.62, 0, 4.6), 0.4],
	]
	for r in rows:
		var m := _rock(TYPES.find(r[0]), FAMILIES.find(r[1]), sd)
		_place_mesh(m, r[2], r[3], r[4])
	cam.position = Vector3(0.05, 0.95, 6.0)
	cam.look_at(Vector3(0.05, 0.3, 2.0))


func _one_rock() -> void:
	_plain_ground(20.0)
	var tf: PackedStringArray = args["rock"].split(",")
	var m := _rock(TYPES.find(tf[0]), FAMILIES.find(tf[1]) if tf.size() > 1 else 0, int(args.get("seed", "3")))
	print("rock: %d triangles, axes %s" % [m.surface_get_array_index_len(0) / 3, m.get_meta("rock_axes")])
	var fam := FAMILIES.find(tf[1]) if tf.size() > 1 else 0
	if args.has("stand") and fam in [RockGenerator.FAMILY_FIN, RockGenerator.FAMILY_HOODOO, RockGenerator.FAMILY_SEA_STACK]:
		# --stand: on end as the field stands it (a fin's z up, a column's x up), seen side on from 6 b-axes away.
		var mi := MeshInstance3D.new()
		mi.mesh = m
		var bs := Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0)) if fam == RockGenerator.FAMILY_FIN else Basis(Vector3(0, 1, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1))
		var box := m.get_aabb()
		var foot := box.position.z if fam == RockGenerator.FAMILY_FIN else box.position.x
		mi.transform = Transform3D(Basis(Vector3.UP, 0.35) * bs, Vector3(0, -foot - 0.05, 0))
		add_child(mi)
		var top := box.size.z if fam == RockGenerator.FAMILY_FIN else box.size.x
		cam.position = Vector3(1.2, top * 0.45, top * 2.6 + 3.0)
		cam.look_at(Vector3(0, top * 0.4, 0))
		return
	_place_mesh(m, 1.0, Vector3.ZERO, 0.6)
	cam.position = Vector3(0.9, 0.75, 1.5)
	cam.look_at(Vector3(0, 0.2, 0))


func _world() -> void:
	var t0 := Time.get_ticks_msec()
	gen = TerrainGenerator3D.new()
	gen.name = "Generator"
	gen.size = Vector2(WORLD, WORLD)
	gen.cell_size = 8.0
	gen.seed = int(args.get("seed", "5"))
	gen.uplift = 3.0
	gen.base_uplift = 0.1
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	for y in 64:
		var z := -WORLD * 0.5 + (y + 0.5) / 64.0 * WORLD
		for x in 64:
			up.set_pixel(x, y, Color(0.12 + 0.88 * (1.0 - smoothstep(-500.0, 500.0, z)), 0, 0))
	gen.uplift_map = ImageTexture.create_from_image(up)
	gen.outlet_edges = 0
	gen.ocean_edges = TerrainGenerator3D.OUTLET_POSITIVE_Z
	gen.coast_distance = 300.0
	gen.lake_count = 1
	gen.strata_enabled = true
	gen.cliff_ledge_spacing = 16.0
	gen.scree_age = float(args.get("scree-age", "6000"))
	gen.moraine_age = float(args.get("moraine-age", "8000"))
	gen.detail_octaves = 5
	gen.glacial_enabled = true
	gen.glacial_ela = float(args.get("ela", "450"))
	add_child(gen)
	gen.generate()
	print("world generated in %d ms" % (Time.get_ticks_msec() - t0))
	if args.has("probe"):
		var hist := [0, 0, 0, 0, 0]
		var till := 0
		var hmax := -1e9
		for zz in range(-1000, 1000, 16):
			for xx in range(-1000, 1000, 16):
				var p := Vector3(xx, 0, zz)
				var t := gen.get_talus(p)
				hist[mini(int(t * 5.0), 4)] += 1
				if gen.get_geology_at(p).b < 0.0:
					till += 1
				hmax = maxf(hmax, gen.get_height(p))
		print("talus histogram (0-0.2..0.8-1): %s, till cells %d, highest %.0f m, ice %.2f km2" % [hist, till, hmax, gen.get_last_glacial_ice_area() / 1e6])
	var tm := TerrainMaterial3D.new()
	tm.use_generator(gen)
	tm.update_shader()
	var land := Landscape3D.new()
	land.name = "Landscape"
	add_child(land)
	land.material = tm
	land.source = land.get_path_to(gen)
	land.collision_enabled = args.has("water") # the sea's seabed_from_floor measures the bed through it
	land.camera = land.get_path_to(cam)
	land.rebuild()
	land.load_all_heights()
	if args.has("water"):
		var waters: Array = gen.build_water(self)
		if args.has("sea-res"):
			# The sea's shallow-water grid is 16 m cells on a 2 km world: too coarse for water round a stack.
			for w in waters:
				if w is Node and (w as Node).name == "Sea":
					w.set("grid_resolution", int(args["sea-res"]))
		if args.has("wind"):
			# --wind=m/s: a rougher sea with some swell, so the foot of a stack has waves and foam on it.
			for w in waters:
				if w is Node and (w as Node).name == "Sea":
					w.set("follow_weather", false)
					w.set("wind_speed", float(args["wind"]))
					w.set("swell", 0.5)
					w.set("shore_wave_height", 1.4)
	var focus := Node3D.new()
	focus.name = "Focus"
	add_child(focus)
	if args.has("stand-ins"):
		_stand_ins()
	elif not args.has("no-rocks"):
		rocks = RockField3D.new()
		rocks.name = "Rocks"
		rocks.rock_type = maxi(TYPES.find(args.get("type", "granite")), 0)
		rocks.soft_rock_type = maxi(TYPES.find(args.get("soft-type", "limestone")), 0)
		rocks.streaming = false
		add_child(rocks)
		rocks.generator = rocks.get_path_to(gen)
		rocks.follow_target = rocks.get_path_to(focus)
		rocks.ground_material = tm # the stones' feet blend into these layers
		# Off by default in the engine until their GPU cost is timed; on in the bench.
		rocks.ground_blend = not args.has("no-ground-blend")
		rocks.mineral_glints = not args.has("no-glints")
		if args.has("water"):
			rocks.collision_enabled = false # the sea's seabed probe would take the rocks for the bed
	var view: String = args.get("view", "scree")
	var spot := _densest(LAYERS.find(view)) if rocks else Vector3(0, 0, 0)
	spot.y = gen.get_height(spot)
	focus.global_position = spot
	var dist := {"rockfall": 14.0, "scree": 7.0, "glacial": 12.0, "river": 14.0, "shore": 12.0, "cliff": 30.0, "tor": 22.0, "coast": 60.0, "landform": 30.0}.get(view, 10.0) as float
	if args.has("dist"):
		dist = float(args["dist"])
	cam.position = _view_from(spot, dist)
	if view == "river" or view == "shore":
		# Low ground under steep sides: look down on the bed from higher, over the walls round it.
		cam.position.y = maxf(cam.position.y, spot.y + dist * 0.9)
	if args.has("top"):
		cam.position = spot + Vector3(0, float(args["top"]) if args["top"] != "1" else 30.0, 0.5)
	var look := spot + Vector3(0, 0.3, 0)
	if view == "coast" and args.has("seaward"):
		# --seaward: from the land side looking out, the form against sea and sky with the cliff behind the camera.
		var land_dir := Vector3.ZERO
		var top := -INF
		for i in 16:
			var d := Vector3(cos(TAU * i / 16.0), 0, sin(TAU * i / 16.0))
			var hgt := gen.get_height(spot + d * 50.0)
			if hgt > top:
				top = hgt
				land_dir = d
		var side := land_dir.cross(Vector3.UP) * float(args.get("side", "0.35"))
		cam.position = spot + (land_dir + side).normalized() * dist
		var sea_y := gen.get_sea_level() if gen.has_sea_result() else spot.y
		cam.position.y = maxf(gen.get_height(cam.position) + 2.0, sea_y + float(args.get("cam-height", "6")))
		look = Vector3(spot.x, sea_y + float(args.get("look-height", "8")), spot.z)
	cam.look_at(look)
	print("spot %s, camera %s" % [spot, cam.position])
	print("ground at the spot: generator %.2f, landscape %.2f; camera %s" % [gen.get_height(spot), land.get_height(spot), cam.position])
	if rocks:
		if args.has("only-layer"):
			for i in LAYERS.size():
				rocks.set_layer_enabled(i, LAYERS[i] == view)
		if args.has("stone-density"):
			rocks.set_layer_density(LAYERS.find(view), float(args["stone-density"]))
		var tb := Time.get_ticks_msec()
		var built := rocks.build_area(Rect2(spot.x - 160, spot.z - 160, 320, 320))
		print("rocks: %d cells, %d stones in %d ms (shape libraries %.0f ms, %d library triangles)" % [built, rocks.get_stone_count(), Time.get_ticks_msec() - tb, rocks.get_last_library_msec(), rocks.get_library_triangle_count()])
		if args.has("plants"):
			_crevice_plants(spot, view, land)
		if args.has("break"):
			_break_near(spot, land)


# The pieces against the stone: their total volume and the box round them as drawn (each chunk through its
# piece's pose and the node's scale), against the intact chunk at the stone's own transform.
func _piece_stats(node: Node, stone_xf: Transform3D) -> Dictionary:
	var asset: Resource = node.get("blast_asset")
	var pts: Array = asset.get("chunk_points")
	var s := (node as Node3D).global_transform.basis.get_scale()
	var p0: PackedVector3Array = pts[0]
	var whole := 0.0
	var src := AABB(stone_xf * p0[0], Vector3.ZERO)
	for t in range(0, p0.size(), 3):
		whole += p0[t].dot(p0[t + 1].cross(p0[t + 2]))
		src = src.expand(stone_xf * p0[t]).expand(stone_xf * p0[t + 1]).expand(stone_xf * p0[t + 2])
	whole = absf(whole) / 6.0 * absf(stone_xf.basis.determinant())
	var vol := 0.0
	var box := AABB()
	var first := true
	for i in node.get_piece_count():
		var pose: Transform3D = node.get_piece_transform(i)
		var xf := Transform3D(pose.basis.orthonormalized().scaled_local(s), pose.origin)
		var cp: PackedVector3Array = pts[node.get_piece_chunk(i)]
		var v := 0.0
		for t in range(0, cp.size(), 3):
			v += cp[t].dot(cp[t + 1].cross(cp[t + 2]))
			for k in 3:
				var q := xf * cp[t + k]
				if first:
					box = AABB(q, Vector3.ZERO)
					first = false
				box = box.expand(q)
		vol += absf(v) / 6.0 * absf(xf.basis.determinant())
	return {"volume": vol, "stone_volume": whole, "box": box, "stone_box": src}


func _report_pieces(node: Node, stone_xf: Transform3D) -> void:
	if node == null:
		return
	var st := _piece_stats(node, stone_xf)
	var ys := []
	for i in node.get_piece_count():
		var o: Vector3 = node.get_piece_transform(i).origin
		ys.append("%.1f" % (o.y - gen.get_height(o)))
	print("piece heights above ground: ", ys)
	print("pieces: %d, volume %.3f m3 against the stone's %.3f m3, spread %s against the stone's %s" % [node.get_piece_count(), st["volume"], st["stone_volume"], (st["box"] as AABB).size, (st["stone_box"] as AABB).size])


# --break[=frames]: break the biggest stone near the spot (RockField3D.break_stone) and let the pieces fall for that
# many physics ticks (default 120) before the shot; --break-cam=dist frames the pieces from that far.
func _break_near(spot: Vector3, land: Landscape3D) -> void:
	land.collision_enabled = true
	land.rebuild()
	land.load_all_heights()
	rocks.collision_enabled = true
	rocks.rebuild()
	rocks.build_area(Rect2(spot.x - 40, spot.z - 40, 80, 80))
	var near: Array = rocks.get_stones(LAYERS.find(args.get("view", "scree")), Rect2(spot.x - 14, spot.z - 14, 28, 28))
	# A block of about 1.1 to 1.6 m on flat ground; the stones touching it are taken away (broken and freed before
	# anything is drawn, in both shots), so the before reads as one rock and the after as its pieces.
	var big: Dictionary = {}
	for st in near:
		var sc0: float = st["scale"]
		var p: Vector3 = st["position"]
		var flat := absf(gen.get_height(p + Vector3(1.5, 0, 0)) - gen.get_height(p - Vector3(1.5, 0, 0))) < 0.3 and absf(gen.get_height(p + Vector3(0, 0, 1.5)) - gen.get_height(p - Vector3(0, 0, 1.5))) < 0.3
		if flat and sc0 >= 0.55 and sc0 <= 0.8 and (big.is_empty() or sc0 > float(big["scale"])):
			big = st
	if not big.is_empty():
		for o in near:
			if o != big and (o["position"] as Vector3).distance_to(big["position"]) < (float(o["scale"]) + float(big["scale"])) * 1.4 + 0.3:
				var gone: Node = rocks.break_stone(o["position"], 0.0, 0.05)
				if gone:
					gone.free()
	if big.is_empty():
		print("no stone to break near the spot")
		return
	var at: Vector3 = big["position"]
	var sc: float = big["scale"]
	var d := float(args.get("break-cam", "0"))
	if d > 0.0:
		cam.position = _view_from(at, d)
		cam.look_at(at)
	var ticks := int(args["break"]) if args["break"] != "1" else 120
	if ticks <= 0:
		print("intact %.2f m stone at %s (before)" % [2.0 * sc, at])
		return
	breaking = true
	var hit := at + Vector3(0, sc * 0.6, 0)
	var node: Node = rocks.break_stone(hit, float(args.get("damage", "100")), 3.0 * sc)
	print("broke a %.2f m stone at %s into %d pieces" % [2.0 * sc, at, node.get_piece_count() if node else 0])
	RenderingServer.render_loop_enabled = false
	for i in ticks:
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	_report_pieces(node, Transform3D(big["basis"], at))
	breaking = false


# --plants: the stand's crevice plants between the field's stones round the spot (docs/ideas/vegetation.md,
# "Plants between stones"), on a 64 m data map over the landscape.
func _crevice_plants(spot: Vector3, view: String, land: Landscape3D) -> void:
	var t0 := Time.get_ticks_msec()
	var dm := TerrainDataMap3D.new()
	dm.name = "PlantMap"
	dm.size = 64.0
	dm.resolution = 256
	dm.bake_on_ready = false
	add_child(dm)
	dm.global_position = Vector3(spot.x, 0.0, spot.z)
	dm.height_source = dm.get_path_to(land)
	dm.geology_source = dm.get_path_to(gen)
	dm.bake()
	var stand: int = {"river": PlantGenerator.STONES_RIVER, "shore": PlantGenerator.STONES_SHORE}.get(view, PlantGenerator.STONES_SCREE)
	var species: Array[PlantSpecies] = []
	for sp in PlantGenerator.make_stones(stand, 3, true, {"forcing": 900.0, "day_of_year": 175}):
		species.append(sp as PlantSpecies)
	var biome := Biome.new()
	biome.species = species
	var veg := VegetationField3D.new()
	veg.name = "CrevicePlants"
	veg.populate_on_ready = false
	veg.biome = biome
	veg.obstacles = rocks.get_obstacles(Rect2(spot.x - 32.0, spot.z - 32.0, 64.0, 64.0), 0.0)
	# Moss cushions on the stones' tops: each stone's flat cap from its shape (RockField3D.get_stone_caps).
	var tops := PackedVector4Array()
	var normals := PackedVector3Array()
	for layer in LAYERS.size():
		if LAYERS[layer] == "cliff":
			continue
		for cap in rocks.get_stone_caps(layer, Rect2(spot.x - 32.0, spot.z - 32.0, 64.0, 64.0), float(args.get("cap-tol", "0.03"))):
			var n: Vector3 = cap["cap_normal"]
			var r := float(cap["cap_radius"])
			if r >= 0.05 and n.y >= 0.6:
				var cp: Vector3 = cap["cap_point"]
				tops.append(Vector4(cp.x, cp.y, cp.z, r * 0.85))
				normals.append(n)
	veg.stone_tops = tops
	veg.stone_top_normals = normals
	add_child(veg)
	veg.data_map = veg.get_path_to(dm)
	veg.populate()
	var counts := []
	for i in species.size():
		counts.append(veg.get_placements(i).size())
	veg.visible = args["plants"] != "off"
	print("crevice plants (%s): %d obstacles, %d stone caps, per species %s, %d ms" % [view, veg.obstacles.size(), veg.stone_tops.size(), str(counts), Time.get_ticks_msec() - t0])
	plants_ready = true


func _sunlit(p: Vector3) -> bool:
	var to_sun := Basis.from_euler(Vector3(deg_to_rad(-32), deg_to_rad(-50), 0)) * Vector3(0, 0, 1)
	var t := 4.0
	while t < 400.0:
		var q := p + to_sun * t
		if gen.get_height(q) > q.y:
			return false
		t *= 1.35
	return true


# A camera spot round the target with a clear line of sight, on the sunny side so the stones are lit, not
# looking into a shaded wall: sixteen directions, scored by the lowest clearance over the terrain along the ray.
func _view_from(spot: Vector3, dist: float) -> Vector3:
	var sun_dir := Basis.from_euler(Vector3(deg_to_rad(-32), deg_to_rad(-50), 0)) * Vector3(0, 0, 1)
	var sun_h := Vector2(sun_dir.x, sun_dir.z).normalized()
	var best := spot + Vector3(dist * 0.7, dist * 0.45, dist * 0.7)
	var best_score := -INF
	for i in 16:
		var a := TAU * i / 16.0
		var d := Vector2(cos(a), sin(a))
		var p := spot + Vector3(d.x * dist, 0, d.y * dist)
		p.y = maxf(spot.y + dist * 0.45, gen.get_height(p) + 3.0)
		var clear := INF
		for k in range(1, 12):
			var q := spot.lerp(p, k / 12.0) + Vector3(0, 0.3 * (1.0 - k / 12.0), 0)
			clear = minf(clear, q.y - gen.get_height(q))
		var walls := 0.0
		for j in 8:
			var w := p + Vector3(cos(TAU * j / 8.0), 0, sin(TAU * j / 8.0)) * dist * 0.5
			walls = maxf(walls, gen.get_height(w) - p.y)
		# From the open, lower side (the water for a shore, the valley for a slope), not from up against the hill.
		var score := minf(clear, 3.0) - maxf(walls, 0.0) * 0.5 - (gen.get_height(p) - spot.y) * 0.3 + 2.0 * d.dot(sun_h) - maxf(p.y - spot.y - dist, 0.0) * 0.2
		if score > best_score:
			best_score = score
			best = p
	return best


# The middle of the 40 m square holding most stones of a layer, near the middle of the map.
func _densest(layer: int) -> Vector3:
	if layer < 0:
		return Vector3.ZERO
	var t0 := Time.get_ticks_msec()
	if layer == RockField3D.LAYER_SCREE:
		# Scree covers too much ground to place it all: the fullest talus near the middle, on gentle enough ground.
		var bt := -1.0
		var bp := Vector3.ZERO
		for zz in range(-800, 800, 8):
			for xx in range(-800, 800, 8):
				var p := Vector3(xx, 0, zz)
				var t := gen.get_talus(p) - absf(gen.get_height(p + Vector3(4, 0, 0)) - gen.get_height(p - Vector3(4, 0, 0))) * 0.02
				if t > bt:
					bt = t
					bp = p
		print("scree: fullest talus %.2f at %s (%d ms)" % [bt, bp, Time.get_ticks_msec() - t0])
		return bp
	var stones: Array = rocks.get_stones(layer, Rect2(-800, -800, 1600, 1600))
	if layer == RockField3D.LAYER_COAST:
		# The coast: frame the stack or arch itself (the biggest form), its foot at the water.
		var best := {}
		for s in stones:
			var fams := [RockGenerator.FAMILY_SEA_STACK, RockGenerator.FAMILY_SEA_ARCH]
			if args.has("form"):
				fams = [RockGenerator.FAMILY_SEA_ARCH if args["form"] == "arch" else RockGenerator.FAMILY_SEA_STACK]
			if int(s["family"]) in fams and (best.is_empty() or float(s["scale"]) > float(best["scale"])):
				best = s
		if not best.is_empty():
			print("coast form: family %d at %s" % [best["family"], best["position"]])
			return best["position"]
	if layer == RockField3D.LAYER_LANDFORM and FAMILIES.has(args.get("form", "")):
		# --form=pavement|fin|hoodoo: the biggest of that family in the sun.
		var best := {}
		for s in rocks.get_stones(layer, Rect2(-1000, -1000, 2000, 2000)):
			# Sunlit first, then the bigger.
			if int(s["family"]) != FAMILIES.find(args["form"]):
				continue
			var score := float(s["scale"]) + (100.0 if _sunlit(s["position"] + Vector3(0, 1, 0)) else 0.0)
			if best.is_empty() or score > float(best["score"]):
				best = s.duplicate()
				best["score"] = score
		if not best.is_empty():
			print("%s at %s" % [args["form"], best["position"]])
			return best["position"]
	var cells := {}
	var sums := {}
	for s in stones:
		var p: Vector3 = s["position"]
		# Rockfall: the runout below the cliff, not the stones still on the steep foot (the view would face a wall).
		if layer == RockField3D.LAYER_ROCKFALL and absf(gen.get_height(p + Vector3(3, 0, 0)) - gen.get_height(p - Vector3(3, 0, 0))) + absf(gen.get_height(p + Vector3(0, 0, 3)) - gen.get_height(p - Vector3(0, 0, 3))) > 3.0:
			continue
		# In the sun: march toward it over the terrain (the bench sky gives little fill, so shade reads black).
		if layer != RockField3D.LAYER_CLIFF and not _sunlit(Vector3(p.x, gen.get_height(p) + 0.5, p.z)):
			continue
		# Rivers: an open valley floor, not a slot gorge in its own shade.
		if layer == RockField3D.LAYER_RIVER:
			var slot := 0.0
			for j in 8:
				slot = maxf(slot, gen.get_height(p + Vector3(cos(TAU * j / 8.0), 0, sin(TAU * j / 8.0)) * 25.0) - p.y)
			if slot > 8.0:
				continue
		var key := Vector2i(floori(p.x / 40.0), floori(p.z / 40.0))
		cells[key] = int(cells.get(key, 0)) + 1
		sums[key] = (sums.get(key, Vector3.ZERO) as Vector3) + p
	var best := Vector2i.ZERO
	var best_n := -1
	for k in cells:
		if cells[k] > best_n:
			best_n = cells[k]
			best = k
	print("%s: %d stones in the middle 1.6 km, densest 40 m square %d (%d ms)" % [LAYERS[layer], stones.size(), best_n, Time.get_ticks_msec() - t0])
	if best_n <= 0:
		return Vector3.ZERO
	# The middle of the stones in it, not of the square: a shore or river band can run along one edge.
	var mid: Vector3 = (sums[best] as Vector3) / float(best_n)
	return Vector3(mid.x, 0, mid.z)


func _stand_ins() -> void:
	var bl: PackedVector4Array = gen.get_boulders(Rect2(-1024, -1024, 2048, 2048), 1.0)
	var sm := SphereMesh.new()
	sm.radial_segments = 7
	sm.rings = 4
	var rm := StandardMaterial3D.new()
	rm.albedo_color = Color(0.42, 0.40, 0.37)
	rm.roughness = 0.9
	sm.material = rm
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = sm
	mm.instance_count = bl.size()
	for i in bl.size():
		var b := bl[i]
		mm.set_instance_transform(i, Transform3D(Basis().scaled(Vector3(b.w * 2.0, b.w * 1.2, b.w * 2.0)), Vector3(b.x, b.y - b.w * 0.2, b.z)))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)


func _volume(m: ArrayMesh) -> float:
	var arr := m.surface_get_arrays(0)
	var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
	var vol := 0.0
	for i in range(0, idx.size(), 3):
		vol += v[idx[i]].dot(v[idx[i + 1]].cross(v[idx[i + 2]]))
	return absf(vol) / 6.0


func _test() -> void:
	# 1. Every rock type and family.
	var worst := 0
	for ti in TYPES.size():
		for fi in FAMILIES.size():
			var g := RockGenerator.new()
			g.rock_type = ti
			g.family = fi
			g.seed = 11
			var m := g.generate()
			if m == null:
				check(false, "%s %s builds" % [TYPES[ti], FAMILIES[fi]])
				continue
			var ax: Vector3 = m.get_meta("rock_axes")
			var tris := m.surface_get_array_index_len(0) / 3
			worst = maxi(worst, int(g.get_last_build_msec()))
			var ok := absf(ax.z - 1.0) < 0.02 and ax.y <= ax.z + 0.02 and ax.x >= ax.z - 0.05 and tris > 500
			check(ok, "%s %s: axes %.2f %.2f %.2f (b 1), %d triangles, %.0f ms" % [TYPES[ti], FAMILIES[fi], ax.x, ax.y, ax.z, tris, g.get_last_build_msec()])
	check(worst < 3000, "slowest rock %d ms (under 3 s)" % worst)
	# 2. Transport rounds: the stone fills less of its box as corners wear (a box fills all of it, an ellipsoid
	# 52%), and beach wear flattens.
	var fills := []
	for d in [0.0, 30000.0, 150000.0]:
		var g := RockGenerator.new()
		g.rock_type = RockGenerator.ROCK_GRANITE
		g.family = RockGenerator.FAMILY_COBBLE
		g.transport_distance = d
		g.seed = 4
		var m := g.generate()
		var ax: Vector3 = m.get_meta("rock_axes")
		fills.append(_volume(m) / (8.0 * ax.x * ax.y * ax.z))
	check(fills[0] > fills[1] and fills[2] <= fills[1] + 0.02 and fills[2] < 0.7, "river transport rounds granite: box fill %.2f, %.2f, %.2f at 0, 30, 150 km" % fills)
	var flat := []
	for f in [RockGenerator.FAMILY_COBBLE, RockGenerator.FAMILY_PEBBLE]:
		var sum := 0.0
		for s in 4:
			var g := RockGenerator.new()
			g.rock_type = RockGenerator.ROCK_LIMESTONE
			g.family = f
			g.seed = s
			var ax: Vector3 = g.generate().get_meta("rock_axes")
			sum += ax.y / ax.z
		flat.append(sum / 4.0)
	check(flat[1] < flat[0], "beach pebbles flatter than river cobbles: c/b %.2f against %.2f" % [flat[1], flat[0]])
	# 2b. Breaking: the solid split into rough pieces (fracture_pieces) that fill it, bonded to their neighbours.
	for ti in [RockGenerator.ROCK_GRANITE, RockGenerator.ROCK_SLATE]:
		var g := RockGenerator.new()
		g.rock_type = ti
		g.family = RockGenerator.FAMILY_ROCKFALL
		g.seed = 5
		g.lods = false
		g.fracture_pieces = 10
		var tf := Time.get_ticks_msec()
		var m := g.generate()
		var chunks: Array = m.get_meta("rock_chunks", [])
		var bonds: Array = m.get_meta("rock_bonds", [])
		var vol: float = m.get_meta("rock_volume", 0.0)
		var sum := 0.0
		var good := 0
		var tris := 0
		for c in chunks:
			if (c as Array).size() == Mesh.ARRAY_MAX:
				good += 1
				var am := ArrayMesh.new()
				am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, c)
				sum += _volume(am)
				tris += (c[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
		var whole := _volume(m)
		check(good >= 6 and bonds.size() >= good - 1 and absf(sum / whole - 1.0) < 0.12, "%s fracture: %d pieces, %d bonds, pieces fill %.2f of the stone (field volume %.3f, mesh %.3f), %d triangles, %d ms" % [TYPES[ti], good, bonds.size(), sum / whole, vol, whole, tris, Time.get_ticks_msec() - tf])
	# 3. The field on a generated world.
	_world()
	var wr := Rect2(-1000, -1000, 2000, 2000)
	var scree_at := _densest(RockField3D.LAYER_SCREE)
	for l in LAYERS.size():
		var t0 := Time.get_ticks_msec()
		var st: Array = rocks.get_stones(l, wr if l != RockField3D.LAYER_SCREE else Rect2(scree_at.x - 100, scree_at.z - 100, 200, 200))
		var floating := 0
		var buried := 0
		for s in st:
			var p: Vector3 = s["position"]
			var sc: float = s["scale"]
			# Tor and coast blocks stand in stacks: their upper tiers are meant to be off the ground.
			if l < RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) > sc * 3.0:
				floating += 1
			if l < RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) < -sc * 1.2:
				buried += 1
			# A cliff block's centre lies inside the wall: under the terrain at its own plan position.
			if l == RockField3D.LAYER_CLIFF and p.y > gen.get_height(p) + 0.1:
				floating += 1
		var want_some := true
		check(not want_some or st.size() > 0, "%s: %d stones over the world in %d ms" % [LAYERS[l], st.size(), Time.get_ticks_msec() - t0])
		check(floating == 0 and buried == 0, "%s: none floating (%d) or buried (%d)" % [LAYERS[l], floating, buried])
	# Landforms: hoodoo rows on the steep soft beds, pavement on flat bare limestone; each kind somewhere.
	var kinds := {}
	for s in rocks.get_stones(RockField3D.LAYER_LANDFORM, wr):
		kinds[s["family"]] = int(kinds.get(s["family"], 0)) + 1
	# Pavement wants flat bare limestone benches, which this steep test world nearly lacks (0 to 2 of them): it
	# is counted, not required.
	check(kinds.has(RockGenerator.FAMILY_HOODOO) and kinds.has(RockGenerator.FAMILY_FIN), "landforms: %d hoodoos, %d fins, %d pavement patches" % [kinds.get(RockGenerator.FAMILY_HOODOO, 0), kinds.get(RockGenerator.FAMILY_FIN, 0), kinds.get(RockGenerator.FAMILY_PAVEMENT, 0)])
	# Scree packs without overlaps: stones in one cell, as spheres of their mean radius.
	var scree: Array = rocks.get_stones(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 16, scree_at.z - 16, 32, 32))
	var overlaps := 0
	for i in scree.size():
		for j in range(i + 1, mini(scree.size(), i + 200)):
			var a: Dictionary = scree[i]
			var b: Dictionary = scree[j]
			var rr: float = (float(a["scale"]) + float(b["scale"])) * 0.55
			if (a["position"] as Vector3).distance_to(b["position"]) < rr * 0.6:
				overlaps += 1
	check(overlaps <= scree.size() / 50, "scree packs: %d deep overlaps in %d stones" % [overlaps, scree.size()])
	# Stone caps (where moss and snow settle) from the shapes themselves: on the stone, at its top, facing up.
	var tc := Time.get_ticks_msec()
	var caps: Array = rocks.get_stone_caps(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 16, scree_at.z - 16, 32, 32))
	var bad_caps := 0
	var usable := 0
	var radii := 0.0
	for c in caps:
		var cp: Vector3 = c["cap_point"]
		var cn: Vector3 = c["cap_normal"]
		var sc: float = c["scale"]
		if cp.y > float(c["top"]) + 0.01 or cp.y < float(c["ground"]) - 0.2 * sc or cn.y < 0.65 or float(c["cap_radius"]) > 2.5 * sc:
			bad_caps += 1
		radii += float(c["cap_radius"]) / sc
		if float(c["cap_radius"]) >= 0.06:
			usable += 1
	check(caps.size() > 0 and bad_caps <= caps.size() / 100 and usable > caps.size() / 2, "stone caps: %d stones, %d off their tops or tilted past 50 degrees, %d with a flat radius of 6 cm or more, mean flat radius %.2f of the half size (%d ms)" % [caps.size(), bad_caps, usable, radii / maxf(caps.size(), 1), Time.get_ticks_msec() - tc])
	var t1 := Time.get_ticks_msec()
	var obs: PackedVector4Array = rocks.get_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256), 0.25)
	check(obs.size() > 0, "obstacles for the plant fields: %d stones of 25 cm and more in 256 m round the scree (%d ms)" % [obs.size(), Time.get_ticks_msec() - t1])
	var t2 := Time.get_ticks_msec()
	var obs2: PackedVector4Array = rocks.get_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256), 0.25)
	check(obs2 == obs and Time.get_ticks_msec() - t2 < 100, "obstacles again from the cache: same %d stones in %d ms" % [obs2.size(), Time.get_ticks_msec() - t2])
	var rivers: Array = rocks.get_stones(RockField3D.LAYER_RIVER, wr)
	var rounder := 0
	for s in rivers:
		rounder += 1 if int(s["bin"]) > 0 else 0
	print("river stones past the first roundness step: %d of %d" % [rounder, rivers.size()])
	# 4. Cost: build 320 m round the scree with collision, then the memory the shared shapes and the instances hold.
	rocks.collision_enabled = true
	(get_node("Focus") as Node3D).global_position = scree_at
	var tb := Time.get_ticks_msec()
	var cells := rocks.build_area(Rect2(scree_at.x - 160, scree_at.z - 160, 320, 320))
	var build_ms := Time.get_ticks_msec() - tb
	var meshes := {}
	var close_meshes := {}
	var instances := 0
	var bodies := 0
	var shapes := 0
	for n in rocks.get_children(true):
		if n is MultiMeshInstance3D and n.has_meta("rock_close_up"):
			var cm: Mesh = (n as MultiMeshInstance3D).multimesh.mesh
			close_meshes[cm.get_rid()] = cm
		elif n is MultiMeshInstance3D:
			var mm: MultiMesh = (n as MultiMeshInstance3D).multimesh
			instances += mm.instance_count
			meshes[mm.mesh.get_rid()] = mm.mesh
		elif n is StaticBody3D:
			bodies += 1
			shapes += n.get_child_count()
	var mesh_bytes := _mesh_bytes(meshes)
	var close_bytes := _mesh_bytes(close_meshes)
	var inst_bytes := instances * (48 + 16)
	print("cost: %d cells, %d stones, %d shapes, %.1f MB shapes (LOD0 %d triangles), %.1f MB instances, %d collision bodies / %d hulls, built in %d ms" % [cells, instances, meshes.size(), mesh_bytes / 1048576.0, rocks.get_library_triangle_count(), inst_bytes / 1048576.0, bodies, shapes, build_ms])
	check(shapes > 0, "collision hulls near the follow target: %d" % shapes)
	# Close-up shapes for the stones within close_up_radius of the follow target.
	check(rocks.get_close_up_shape_count() > 0, "close-up shapes near the follow target: %d shapes, %d triangles, %.1f MB" % [rocks.get_close_up_shape_count(), rocks.get_close_up_triangle_count(), close_bytes / 1048576.0])
	# Close-up shapes by family and size octave (what drives their count and memory).
	var hist := {}
	for l in [RockField3D.LAYER_ROCKFALL, RockField3D.LAYER_SCREE, RockField3D.LAYER_GLACIAL]:
		for st in rocks.get_stones(l, Rect2(scree_at.x - 12, scree_at.z - 12, 24, 24)):
			if (st["position"] as Vector3).distance_to(scree_at) < 8.0 + float(st["scale"]) * 1.5:
				var fam: int = st["family"]
				var b := roundi(log(float(st["scale"]) / [0.65, 0.16, 0.85, 0.11, 0.035, 0.65, 1.0][fam]) / log(2.0))
				var k := "%s %+d" % [FAMILIES[fam], b]
				hist[k] = hist.get(k, 0) + 1
	print("near stones by family and octave: ", hist)
	# Breaking a stone near the target: it is swapped for its pieces (PhysX Blast) and stays broken.
	check(rocks.is_breaking_supported(), "breaking supported (PhysX Blast in this build)")
	if rocks.is_breaking_supported():
		var near: Array = rocks.get_stones(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 6, scree_at.z - 6, 12, 12))
		var big: Dictionary = {}
		for st in near:
			if big.is_empty() or float(st["scale"]) > float(big["scale"]):
				big = st
		check(not big.is_empty(), "a stone to break near the target: %d stones" % near.size())
		if not big.is_empty():
			var bt := Time.get_ticks_msec()
			var node: Node = rocks.break_stone(big["position"], 100.0, 3.0 * float(big["scale"]))
			var pieces: int = node.get_piece_count() if node else 0
			check(node != null and pieces > 3 and rocks.get_broken_count() == 1, "break_stone: a %.2f m stone into %d pieces in %d ms (shape fractured on demand)" % [2.0 * float(big["scale"]), pieces, Time.get_ticks_msec() - bt])
			if node:
				var ps := _piece_stats(node, Transform3D(big["basis"], big["position"]))
				var grown: AABB = (ps["stone_box"] as AABB).grow(0.02 * (ps["stone_box"] as AABB).size.length())
				check(absf(ps["volume"] / ps["stone_volume"] - 1.0) < 0.05 and grown.encloses(ps["box"]), "pieces are the stone: volume %.3f of %.3f m3, all inside its box (%s in %s)" % [ps["volume"], ps["stone_volume"], (ps["box"] as AABB).size, (ps["stone_box"] as AABB).size])
			# A hit on a stone whose shape is already fractured (what break_prepare_radius does ahead of the player on a
			# worker): only the swap for the pieces, no fracture on the main thread.
			rocks.restore_broken()
			var bp: Vector3 = big["position"]
			rocks.build_area(Rect2(bp.x - 40, bp.z - 40, 80, 80)) # restore_broken drops the cells; build them again
			var pt := Time.get_ticks_usec()
			var again: Node = rocks.break_stone(big["position"], 100.0, 3.0 * float(big["scale"]))
			var ms := (Time.get_ticks_usec() - pt) / 1000.0
			check(again != null and ms < 16.0, "break_stone on a prepared shape: %.1f ms (under a 60 Hz frame)" % ms)


# What the renderer holds for these meshes: vertex, attribute and index buffers and every LOD's indices.
func _mesh_bytes(meshes: Dictionary) -> int:
	var bytes := 0
	for k in meshes:
		var m: Mesh = meshes[k]
		for si in m.get_surface_count():
			var d: Dictionary = RenderingServer.mesh_get_surface(m.get_rid(), si)
			for key in ["vertex_data", "attribute_data", "skin_data", "index_data"]:
				if d.has(key):
					bytes += (d[key] as PackedByteArray).size()
			for lod in d.get("lods", []):
				bytes += (lod["index_data"] as PackedByteArray).size()
	return bytes


func _render_later() -> void:
	await get_tree().process_frame
	while breaking:
		await get_tree().process_frame
	while args.has("plants") and rocks and not plants_ready:
		await get_tree().process_frame
	var frames := (8 if raster else 12) + int(args.get("settle", "0")) # --settle=N: N more frames (lazily built near plants)
	var t0 := 0
	for i in frames:
		if i == frames - 4:
			t0 = Time.get_ticks_usec()
		await RenderingServer.frame_post_draw
	print("frame %.1f ms" % ((Time.get_ticks_usec() - t0) / 4000.0))
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(0)

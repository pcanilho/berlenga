extends Node3D
# Rocks bench: RockGenerator shapes and RockField3D placement (docs/ideas/rocks.md).
#
# godot --path docs/bench/rocks --rendering-driver vulkan -- [options]
#   --test                checks: every rock type and family builds (size, axes, timing), transport rounds and beach
#                         wear flattens, then the field's six layers on a generated world; prints PASS/FAIL, quits
#   --gallery             every family (columns) of every rock type (rows), each scaled to a 1 m b axis
#   --rock=type,family    one rock close up (--seed=N, --distance=m transport, --weathering=0..1)
#   --world               a generated 2 km world (range, strata, glaciers, scree, rivers, a lake, the sea) with a
#                         RockField3D over it; --view=rockfall|scree|glacial|river|shore|cliff puts the camera at the
#                         densest place of that layer (default scree)
#   --type=granite|limestone|sandstone|basalt|slate   the world's rock (default granite; soft beds limestone)
#   --stand-ins           with --world: the old grey spheres at the rockfall blocks instead of the field (before shots)
#   --no-rocks            with --world: no rocks at all
#   --out=path.png        render and quit (path traced unless --raster); --spp=N (16); --size=WxH
#   --cam=x,y,z --look=x,y,z   a fixed camera

const TYPES := ["granite", "limestone", "sandstone", "basalt", "slate"]
const FAMILIES := ["rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block"]
const LAYERS := ["rockfall", "scree", "glacial", "river", "shore", "cliff"]
const WORLD := 2048.0

var args := {}
var raster := false
var spp := 16
var out := ""
var fails := 0
var gen: TerrainGenerator3D
var rocks: RockField3D
var cam: Camera3D


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.substr(2).split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	raster = args.has("raster")
	spp = int(args.get("spp", "16"))
	out = args.get("out", "")
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
	else:
		_gallery()
	if args.has("cam"):
		var c: PackedStringArray = args["cam"].split(",")
		var l: PackedStringArray = args.get("look", "0,0,0").split(",")
		cam.position = Vector3(float(c[0]), float(c[1]), float(c[2]))
		cam.look_at(Vector3(float(l[0]), float(l[1]), float(l[2])))
	if out != "":
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


func _one_rock() -> void:
	_plain_ground(20.0)
	var tf: PackedStringArray = args["rock"].split(",")
	var m := _rock(TYPES.find(tf[0]), FAMILIES.find(tf[1]) if tf.size() > 1 else 0, int(args.get("seed", "3")))
	print("rock: %d triangles, axes %s" % [m.surface_get_array_index_len(0) / 3, m.get_meta("rock_axes")])
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
	land.collision_enabled = false
	land.camera = land.get_path_to(cam)
	land.rebuild()
	land.load_all_heights()
	if args.has("water"):
		gen.build_water(self)
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
	var view: String = args.get("view", "scree")
	var spot := _densest(LAYERS.find(view)) if rocks else Vector3(0, 0, 0)
	spot.y = gen.get_height(spot)
	focus.global_position = spot
	var dist := {"rockfall": 14.0, "scree": 7.0, "glacial": 12.0, "river": 14.0, "shore": 12.0, "cliff": 30.0}.get(view, 10.0) as float
	cam.position = _view_from(spot, dist)
	if view == "river" or view == "shore":
		# Low ground under steep sides: look down on the bed from higher, over the walls round it.
		cam.position.y = maxf(cam.position.y, spot.y + dist * 0.9)
	if args.has("top"):
		cam.position = spot + Vector3(0, float(args["top"]) if args["top"] != "1" else 30.0, 0.5)
	cam.look_at(spot + Vector3(0, 0.3, 0))
	print("spot %s, camera %s" % [spot, cam.position])
	print("ground at the spot: generator %.2f, landscape %.2f; camera %s" % [gen.get_height(spot), land.get_height(spot), cam.position])
	if rocks:
		var tb := Time.get_ticks_msec()
		var built := rocks.build_area(Rect2(spot.x - 160, spot.z - 160, 320, 320))
		print("rocks: %d cells, %d stones in %d ms (shape libraries %.0f ms, %d library triangles)" % [built, rocks.get_stone_count(), Time.get_ticks_msec() - tb, rocks.get_last_library_msec(), rocks.get_library_triangle_count()])


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
			if l != RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) > sc * 3.0:
				floating += 1
			if l != RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) < -sc * 1.2:
				buried += 1
		var want_some := true
		check(not want_some or st.size() > 0, "%s: %d stones over the world in %d ms" % [LAYERS[l], st.size(), Time.get_ticks_msec() - t0])
		check(floating == 0 and buried == 0, "%s: none floating (%d) or buried (%d)" % [LAYERS[l], floating, buried])
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
	var t1 := Time.get_ticks_msec()
	var obs: PackedVector4Array = rocks.get_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256), 0.25)
	check(obs.size() > 0, "obstacles for the plant fields: %d stones of 25 cm and more in 256 m round the scree (%d ms)" % [obs.size(), Time.get_ticks_msec() - t1])
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
	var instances := 0
	var bodies := 0
	var shapes := 0
	for n in rocks.get_children(true):
		if n is MultiMeshInstance3D:
			var mm: MultiMesh = (n as MultiMeshInstance3D).multimesh
			instances += mm.instance_count
			meshes[mm.mesh.get_rid()] = mm.mesh
		elif n is StaticBody3D:
			bodies += 1
			shapes += n.get_child_count()
	var mesh_bytes := 0
	for k in meshes:
		var m: Mesh = meshes[k]
		for si in m.get_surface_count():
			# What the renderer holds: vertex, attribute and index buffers and every LOD's indices.
			var d: Dictionary = RenderingServer.mesh_get_surface(m.get_rid(), si)
			for key in ["vertex_data", "attribute_data", "skin_data", "index_data"]:
				if d.has(key):
					mesh_bytes += (d[key] as PackedByteArray).size()
			for lod in d.get("lods", []):
				mesh_bytes += (lod["index_data"] as PackedByteArray).size()
	var inst_bytes := instances * (48 + 16)
	print("cost: %d cells, %d stones, %d shapes, %.1f MB shapes (LOD0 %d triangles), %.1f MB instances, %d collision bodies / %d hulls, built in %d ms" % [cells, instances, meshes.size(), mesh_bytes / 1048576.0, rocks.get_library_triangle_count(), inst_bytes / 1048576.0, bodies, shapes, build_ms])
	check(shapes > 0, "collision hulls near the follow target: %d" % shapes)


func _render_later() -> void:
	var frames := 8 if raster else 12
	var t0 := 0
	for i in frames:
		if i == frames - 4:
			t0 = Time.get_ticks_usec()
		await RenderingServer.frame_post_draw
	print("frame %.1f ms" % ((Time.get_ticks_usec() - t0) / 4000.0))
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(0)

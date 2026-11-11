extends Node3D
# Stand bench: every TreeGenerator stand (and make_woodland()) placed by VegetationField3D on a
# small valley with a stream, checked, and drawn as a placement map (crowns from above, one colour
# per species), or rendered.
#
# godot --path docs/bench/trees --rendering-driver vulkan -- --stands [options]
#   --test            place every stand, print the counts and PASS/FAIL, quit
#   --map=path.png    the placement maps of every stand in one image
#   --plot=N          render stand N (TreeGenerator.Stand) on the valley (--out=path.png, --raster, --spp=N)
#   --wind=U          with --plot: rigged trees posed by a U m/s wind (TreeWind3D) at --time=T s
#   --inside          with --plot: the view from inside the stand, 70 m deep
#   --variants=N      meshes per species (default 1)
#   --region=path.png make_woodland() over a 6 km massif with its climate (placement only, no
#                     meshes): the stands as belts on a hillshade
#   --generated       with --region: a TerrainGenerator3D range (4 km, geology and soil) instead
#   --na              with --region: the North American woodland instead of the European one

const SIZE := 160.0
const STREAM_HALF := 3.0 # Stream bed half width, metres.

class Valley extends Node3D:
	func get_height(p: Vector3) -> float:
		# A stream along z at x = 0 in a shallow valley, with low swells.
		var ax: float = abs(p.x)
		var bank: float = -0.8 + 0.1 * max(ax - STREAM_HALF, 0.0) if ax > STREAM_HALF else -0.8
		return bank + 0.8 * min(ax / 20.0, 1.0) * sin(p.z * 0.05) * cos(p.x * 0.04)

class Stream extends Node3D:
	func get_water_level() -> float:
		return -0.3

const COLORS := [Color("6b8e23"), Color("2e8b57"), Color("d8d8c0"), Color("8b5a2b"), Color("1f4d2b"), Color("c9a227"),
		Color("b03a2e"), Color("7fbf7f"), Color("e07b39"), Color("9aa0a6"), Color("8e44ad"), Color("3b6e8f"), Color("a3c4a8"),
		Color("d4b483"), Color("335c4c"), Color("e74c3c"), Color("f5e6a8"), Color("0b3d1e"), Color("4a2c1a"), Color("5c7a2e"),
		Color("d35400"), Color("c0392b"), Color("a04000"), Color("b7950b"), Color("4d6b5a"), Color("17332a"), Color("a6acaf"),
		Color("f4f1ea"), Color("caa472"), Color("e5c07b"), Color("6c8f3a")]
const STAND_NAMES := ["beech", "oak mixed", "montane conifer", "subalpine", "pine heath", "riparian", "hedgerow",
		"northern hardwood", "eastern oak", "pine-hemlock", "woodland", "NA woodland"]

var gen: TreeGenerator
var dm: TerrainDataMap3D
var fails := 0
var variants := 1
var use_generator := false
var north_america := false

func run(args: Dictionary) -> void:
	variants = int(args.get("variants", "1"))
	gen = TreeGenerator.new()
	gen.seed = int(args.get("seed", "1"))
	if args.has("region"):
		use_generator = args.has("generated")
		north_america = args.has("na")
		await _region(args["region"])
		get_tree().quit(1 if fails > 0 else 0)
		return
	_build_ground()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var err := dm.bake()
	_check(err == OK, "valley data map bakes")
	if args.has("test"):
		_habitat_checks()
		_stand_checks()
		_mask_check()
		await _wind_check()
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("map"):
		_draw_maps(args["map"])
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("plot"):
		await _render_plot(int(args["plot"]), args)

func _check(ok: bool, what: String) -> void:
	if not ok:
		fails += 1
	print(("PASS " if ok else "FAIL ") + what)

func _build_ground() -> void:
	var valley := Valley.new()
	valley.name = "Valley"
	add_child(valley)
	var stream := Stream.new()
	stream.name = "Stream"
	add_child(stream)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = 160
	dm.bake_on_ready = false
	add_child(dm)
	dm.height_source = dm.get_path_to(valley)
	dm.water_sources = [dm.get_path_to(stream)]

func _field(biome: Biome) -> VegetationField3D:
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 40.0
	f.biome = biome
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	return f

func _biome(i: int) -> Biome:
	if i == TreeGenerator.STAND_MAX:
		return gen.make_woodland(variants)
	if i == TreeGenerator.STAND_MAX + 1:
		return gen.make_woodland(variants, TreeGenerator.REGION_NORTH_AMERICA)
	return gen.make_stand(i, variants)

func _habitat_checks() -> void:
	var sp := PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_BEECH
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_PRECIPITATION, 450.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_PRECIPITATION, 1000.0) == 1.0, "beech needs rain (none at 450 mm, full at 1000 mm)")
	_check(sp.rule_weight(PlantSpecies.RULE_JANUARY_TEMPERATURE, -15.0) == 0.0, "beech absent where January averages -15 C")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 13.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 3.0) == 1.0, "spruce in cold belts (none at 13 C annual, full at 3 C)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_BLACK_ALDER
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 100.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 10.0) == 1.0, "alder by water (none 100 m away, full at 10 m)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_SCOTS_PINE
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_ARIDITY, 0.5) == 1.0, "pine takes dry ground (aridity 0.5)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_PAPER_BIRCH
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 14.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_JANUARY_TEMPERATURE, -20.0) == 1.0, "paper birch is boreal (none at 14 C annual, full at -20 C January)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_WHITE_OAK
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 18.0) == 1.0 and sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 3.0) == 0.0, "white oak in the warm south (full at 18 C, none at 3 C)")

func _stand_checks() -> void:
	print("stand              species                     placed  per_ha")
	var area_ha := SIZE * SIZE / 10000.0
	for i in TreeGenerator.STAND_MAX + 2:
		var t := Time.get_ticks_msec()
		var biome := _biome(i)
		var f := _field(biome)
		var sps: Array = biome.species
		var stand_count := 0
		var open_count := 0
		var canopy_total := 0
		var regen := {"sapling": 0, "snag": 0, "log": 0, "fallen branch": 0, "sticks": 0}
		for s in sps.size():
			var n: int = f.get_placements(s).size()
			var nm: String = sps[s].resource_name
			print("%-18s %-27s %6d  %6.0f" % [STAND_NAMES[i], nm, n, n / area_ha])
			if nm.ends_with("(stand)"):
				stand_count += n
				canopy_total += n
			elif nm.ends_with("(open)"):
				open_count += n
			for k in regen:
				if nm.ends_with("(%s)" % k):
					regen[k] += n
		print("  %d plants in %d ms (meshes and placement)" % [f.get_plant_count(), Time.get_ticks_msec() - t])
		if i == TreeGenerator.STAND_HEDGEROW:
			_check(f.get_plant_count() > 0 and open_count == f.get_plant_count(), "hedgerow: shrubs and open-grown standards only (%d)" % open_count)
		else:
			_check(stand_count > 0, "%s: stand-grown trees form the canopy (%d stand-grown, %d gap and shrub)" % [STAND_NAMES[i], stand_count, open_count])
			_check(regen.sapling > 0 and regen.snag > 0 and regen.log > 0 and regen.snag + regen.log < stand_count and regen["fallen branch"] > 0 and regen.sticks > regen["fallen branch"], "%s: %d saplings, %d snags, %d logs, %d fallen branches, %d sticks" % [STAND_NAMES[i], regen.sapling, regen.snag, regen.log, regen["fallen branch"], regen.sticks])
		# Climbers ride on variants 1-3 of each host (one variant in four), so need --variants=2+.
		if variants >= 2 and (i == TreeGenerator.STAND_OAK_MIXED or i == TreeGenerator.STAND_RIPARIAN):
			var climbing := 0
			for sp in sps:
				for m in sp.meshes:
					if m.has_meta("tree_climber_leaves"):
						climbing += 1
			_check(climbing > 0, "%s: %d tree variants carry climbers" % [STAND_NAMES[i], climbing])
		if i == TreeGenerator.STAND_RIPARIAN:
			var far := 0
			for p in f.get_placements(0):
				if dm.get_water_distance_at(p) > 40.0 + 16.0:
					far += 1
			_check(far == 0, "riparian: no alder beyond its 40 m (+16 m fade) of water (%d)" % far)
		if i >= TreeGenerator.STAND_MAX:
			# All stands at once on a map without climate: the gap rule must stop them stacking.
			var per_ha := canopy_total / area_ha
			_check(per_ha < 700.0, "woodland: canopy trees do not stack across stands (%.0f per ha)" % per_ha)
		f.queue_free()

func _mask_check() -> void:
	# A clearing painted into the field's density mask (red 0 inside a 40 m disc) keeps every tree
	# and woody shrub out once TreeGenerator.mask_channel points at red.
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	img.fill(Color(1, 1, 1, 1))
	for y in 64:
		for x in 64:
			if Vector2(x - 31.5, y - 31.5).length() < 16.0:
				img.set_pixel(x, y, Color(0, 1, 1, 1))
	gen.mask_channel = 0
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	gen.mask_channel = -1
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 40.0
	f.biome = biome
	f.density_mask = ImageTexture.create_from_image(img)
	f.density_mask_rect = Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var inside := 0
	var total := 0
	for s in biome.species.size():
		for p in f.get_placements(s):
			total += 1
			if Vector2(p.x, p.z).length() < 16.0 / 64.0 * SIZE - 3.0:
				inside += 1
	_check(total > 0 and inside == 0, "clearing mask: no trees or shrubs in the painted clearing (%d of %d)" % [inside, total])
	f.queue_free()

func _rigged_instances(f: VegetationField3D) -> Array:
	var out := []
	for c in f.get_children(true):
		var mmi := c as MultiMeshInstance3D
		if mmi and mmi.multimesh and TreeWind3D.get_mesh_bone_count(mmi.multimesh.mesh) > 0:
			out.append(mmi)
	return out

func _wind_check() -> void:
	# Rigged stand meshes placed by the field get rest bones (TreeWind3D.fill_rest), and one
	# TreeWind3D aimed at the field poses every cell MultiMesh, near and far, from the wind.
	gen.rigged = true
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	gen.rigged = false
	var f := _field(biome)
	var mmis := _rigged_instances(f)
	_check(mmis.size() > 0, "rigged beech stand: %d cell MultiMeshes carry bones" % mmis.size())
	var wind := TreeWind3D.new()
	wind.fallback_wind = Vector2(12, 0)
	wind.collision_enabled = false
	add_child(wind)
	wind.target = wind.get_path_to(f)
	await get_tree().process_frame
	wind.set_time(1.0)
	wind.update(0.0)
	var before := []
	for mmi in mmis:
		before.append(RenderingServer.multimesh_get_buffer(mmi.multimesh.get_rid()))
	wind.set_time(2.3)
	wind.update(0.0)
	var moved := 0
	for i in mmis.size():
		if RenderingServer.multimesh_get_buffer(mmis[i].multimesh.get_rid()) != before[i]:
			moved += 1
	if wind.get_target_count() == 0:
		# A VegetationField3D older than its rigged-mesh support draws the stand unposed.
		print("SKIP rigged field: this VegetationField3D gives rigged meshes no rest bones")
		wind.queue_free()
		f.queue_free()
		return
	_check(wind.get_target_count() == mmis.size(), "wind finds every rigged cell (%d of %d)" % [wind.get_target_count(), mmis.size()])
	_check(moved == mmis.size(), "wind moves every rigged cell (%d of %d), %d instances solved in %.2f ms" % [moved, mmis.size(), wind.get_solved_instances(), wind.get_solve_msec()])
	wind.queue_free()
	f.queue_free()

func _draw_maps(path: String) -> void:
	var px := 320
	var cols := 4
	var rows := 3
	var img := Image.create(px * cols, px * rows, false, Image.FORMAT_RGB8)
	img.fill(Color(0.93, 0.92, 0.88))
	for i in TreeGenerator.STAND_MAX + 2:
		var biome := _biome(i)
		var f := _field(biome)
		var ox := (i % cols) * px
		var oy := (i / cols) * px
		var k := (px - 8) / SIZE
		# Water.
		for y in px - 8:
			for x in px - 8:
				var wx := (x / k) - SIZE * 0.5
				if abs(wx) < STREAM_HALF:
					img.set_pixel(ox + 4 + x, oy + 4 + y, Color(0.55, 0.7, 0.85))
		var sps: Array = biome.species
		for s in sps.size():
			var sp: PlantSpecies = sps[s]
			var c: Color = COLORS[_preset_of(sp.resource_name)]
			if sp.resource_name.ends_with("(open)"):
				c = c.lightened(0.25)
			var r: float = max(sp.canopy_radius * k, 1.5)
			for p in f.get_placements(s):
				_disc(img, ox + 4 + (p.x + SIZE * 0.5) * k, oy + 4 + (p.z + SIZE * 0.5) * k, r, c, ox, oy, px)
		f.queue_free()
	img.save_png(path)
	print("saved ", path)

const PRESET_NAMES := ["Oak", "Beech", "Birch", "Scots Pine", "Norway Spruce", "Hazel", "Hawthorn", "Lime", "Sycamore Maple", "Ash",
		"Horse Chestnut", "Black Alder", "White Willow", "European Larch", "Silver Fir", "Rowan", "Elder", "Holly", "Yew", "Box",
		"Sugar Maple", "Red Maple", "Northern Red Oak", "White Oak", "Eastern White Pine", "Eastern Hemlock", "American Beech",
		"Paper Birch", "Yellow Birch", "Witch-hazel", "Mountain Laurel"]

func _preset_of(nm: String) -> int:
	var base := nm.substr(0, nm.find(" ("))
	return max(PRESET_NAMES.find(base), 0)

func _disc(img: Image, cx: float, cy: float, r: float, c: Color, ox: int, oy: int, px: int) -> void:
	for y in range(int(cy - r), int(cy + r) + 1):
		for x in range(int(cx - r), int(cx + r) + 1):
			if x < ox + 2 or y < oy + 2 or x >= ox + px - 2 or y >= oy + px - 2:
				continue
			var d := Vector2(x - cx, y - cy).length()
			if d <= r:
				var under := img.get_pixel(x, y)
				img.set_pixel(x, y, under.lerp(c, 0.75 if d < r - 1.0 else 0.4))

func _render_plot(i: int, args: Dictionary) -> void:
	var raster: bool = args.has("raster")
	gen.rigged = args.has("wind")
	var biome := _biome(i)
	gen.rigged = false
	var f := _field(biome)
	if args.has("wind"):
		# Posed by the wind at --time seconds (rigged meshes, one TreeWind3D on the field).
		var wind := TreeWind3D.new()
		wind.fallback_wind = Vector2(float(args.get("wind", "10")), 0)
		wind.active = false
		add_child(wind)
		wind.target = wind.get_path_to(f)
		await get_tree().process_frame
		wind.set_time(float(args.get("time", "2")))
		wind.update(0.0)
	print("%s: %d plants" % [STAND_NAMES[i], f.get_plant_count()])
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(SIZE, SIZE)
	pm.subdivide_width = 159
	pm.subdivide_depth = 159
	var st := SurfaceTool.new()
	st.create_from(pm, 0)
	var arr := st.commit_to_arrays()
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var valley: Valley = get_node("Valley")
	for v in verts.size():
		verts[v].y = valley.get_height(verts[v])
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_NORMAL] = null
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	st = SurfaceTool.new()
	st.create_from(am, 0)
	st.generate_normals()
	ground.mesh = st.commit()
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)
	var water := MeshInstance3D.new()
	var wp := PlaneMesh.new()
	wp.size = Vector2(STREAM_HALF * 2.6, SIZE)
	water.mesh = wp
	water.position.y = -0.3
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.1, 0.15, 0.18)
	wmat.roughness = 0.05
	water.material_override = wmat
	add_child(water)
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-38, 30, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 400.0
	add_child(sun)
	var cam := Camera3D.new()
	cam.fov = 45.0
	cam.far = 1000.0
	add_child(cam)
	cam.position = Vector3(-30, 45, SIZE * 0.62)
	cam.look_at(Vector3(5, 4, 0))
	if args.has("inside"):
		# Standing in the stand: a short view (and shadow) distance keeps a CPU render bounded.
		cam.far = 70.0
		sun.directional_shadow_max_distance = 70.0
		var eye := Vector3(0, 0, 45) # on the stream bed, the one gap in the stand
		eye.y = valley.get_height(eye) + 1.7
		cam.position = eye
		cam.look_at(eye + Vector3(-0.35, 0.18, -1))
	for k in (8 if raster else 12):
		await RenderingServer.frame_post_draw
	var out: String = args.get("out", "/tmp/plot.png")
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit()


# --- Region: stands as belts over a generated range ------------------------------------------------

const STAND_COLORS := [Color("3f8f4f"), Color("b5a642"), Color("1f4d3b"), Color("7a6aa8"), Color("c0703a"), Color("3b6e8f"), Color("888888"),
		Color("c86f2e"), Color("8a5a2b"), Color("234d45")]

class Mountain extends Node3D:
	# A massif with forest-able slopes (at most about 33 degrees) and a valley floor: the climate
	# belts without the generator's bare rock walls.
	func get_height(p: Vector3) -> float:
		var r := Vector2(p.x - 400.0, p.z + 300.0).length()
		var ridge := 2500.0 * exp(-pow(r / 2100.0, 2.0))
		return ridge + 60.0 * sin(p.x * 0.004) * cos(p.z * 0.003) + 40.0

func _region(path: String) -> void:
	var WORLD := 4096.0 if use_generator else 6000.0
	var tg: Node3D
	var t := Time.get_ticks_msec()
	if use_generator:
		# The terrain bench's range (docs/bench/terrain_gen), mountain view settings.
		var range_path := Path3D.new()
		var curve := Curve3D.new()
		curve.add_point(Vector3(-1900, 0, 700), Vector3.ZERO, Vector3(600, 0, -200))
		curve.add_point(Vector3(-300, 0, 100), Vector3(-500, 0, 100), Vector3(500, 0, -100))
		curve.add_point(Vector3(1900, 0, -600), Vector3(-600, 0, 0))
		range_path.curve = curve
		add_child(range_path)
		var noise := FastNoiseLite.new()
		noise.seed = 7
		noise.frequency = 1.0 / 1500.0
		noise.fractal_octaves = 3
		var g3 := TerrainGenerator3D.new()
		g3.size = Vector2(WORLD, WORLD)
		g3.cell_size = 16.0
		g3.seed = 1
		g3.uplift_noise = noise
		g3.uplift = 6.0
		g3.range_width = 1300.0
		add_child(g3)
		g3.ranges = [g3.get_path_to(range_path)]
		g3.generate()
		tg = g3
	else:
		tg = Mountain.new()
		add_child(tg)
	var map := TerrainDataMap3D.new()
	map.bake_on_ready = false
	map.size = WORLD
	map.resolution = 600
	add_child(map)
	map.height_source = map.get_path_to(tg)
	if use_generator:
		map.geology_source = map.get_path_to(tg)
	# A cool temperate climate (Central European uplands to Alps): the belts from beech and oak
	# in the valleys through spruce and fir to larch at the tree line.
	var recipe := ClimateRecipe.new()
	recipe.temperature_january = 0.0
	recipe.temperature_july = 19.0
	recipe.reference_height = 0.0
	recipe.precipitation = 700.0
	map.climate_recipe = recipe
	_check(map.bake() == OK, "region data map bakes")
	_check(map.bake_climate() == OK, "region climate bakes")
	t = Time.get_ticks_msec()
	var biome := gen.make_woodland(0, TreeGenerator.REGION_NORTH_AMERICA if north_america else TreeGenerator.REGION_EUROPE)
	print("woodland species (no meshes) %d ms" % (Time.get_ticks_msec() - t))
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 128.0
	f.biome = biome
	add_child(f)
	f.data_map = f.get_path_to(map)
	t = Time.get_ticks_msec()
	f.populate()
	print("woodland: %d plants placed in %d ms" % [f.get_plant_count(), Time.get_ticks_msec() - t])
	# Which stand each species came from, by name (make_woodland keeps make_stand's species objects).
	var stand_of := {}
	for st in TreeGenerator.STAND_MAX:
		if st == TreeGenerator.STAND_HEDGEROW:
			continue
		for sp in gen.make_stand(st, 0).species:
			stand_of[sp.resource_name + str(st)] = st
	# Hillshade with the stands over it.
	var res := 1024
	var img := Image.create(res, res, false, Image.FORMAT_RGB8)
	var k := res / WORLD
	var hmin := INF
	var hmax := -INF
	for z in res:
		for x in res:
			var p := Vector3((x + 0.5) / k - WORLD * 0.5, 0, (z + 0.5) / k - WORLD * 0.5)
			var h := map.get_height_at(p)
			hmin = min(hmin, h)
			hmax = max(hmax, h)
			var hx := map.get_height_at(p + Vector3(4, 0, 0)) - h
			var hz := map.get_height_at(p + Vector3(0, 0, 4)) - h
			var n := Vector3(-hx, 4.0, -hz).normalized()
			var shade: float = clamp(n.dot(Vector3(-0.5, 0.7, -0.5).normalized()), 0.0, 1.0)
			var base := Color(0.86, 0.84, 0.78)
			img.set_pixel(x, z, base * (0.8 + 0.2 * shade))
	print("height %.0f to %.0f m" % [hmin, hmax])
	for hh in ([] if use_generator else [100.0, 600.0, 1100.0, 1600.0, 2100.0, 2450.0]):
		var q := Vector3(400.0 + 2100.0 * sqrt(-log((hh - 40.0) / 2500.0)), 0, -300.0)
		print("  at %.0f m: T %.1f C, January %.1f C, P %.0f mm" % [map.get_height_at(q), map.get_temperature_at(q), map.get_temperature_at(q, 15.0), map.get_precipitation_at(q)])
	var sps: Array = biome.species
	var per_stand := []
	per_stand.resize(TreeGenerator.STAND_MAX)
	per_stand.fill(0)
	for s in sps.size():
		var sp: PlantSpecies = sps[s]
		var st := _stand_of_species(sp)
		var c: Color = STAND_COLORS[st]
		if sp.resource_name.ends_with("(open)"):
			c = c.lightened(0.35)
		var pts := f.get_placements(s)
		per_stand[st] += pts.size()
		for p in pts:
			var x := int((p.x + WORLD * 0.5) * k)
			var z := int((p.z + WORLD * 0.5) * k)
			for dz in 2:
				for dx in 2:
					if x + dx >= 0 and z + dz >= 0 and x + dx < res and z + dz < res:
						img.set_pixel(x + dx, z + dz, img.get_pixel(x + dx, z + dz).lerp(c, 0.7))
	for st in TreeGenerator.STAND_MAX:
		print("  %-16s %7d plants" % [STAND_NAMES[st], per_stand[st]])
	# Belts in order of height: the mean height of each stand's plants.
	var mean_h := {}
	for s in sps.size():
		var st := _stand_of_species(sps[s])
		for p in f.get_placements(s):
			mean_h[st] = mean_h.get(st, Vector2.ZERO) + Vector2(map.get_height_at(p), 1)
	for st in mean_h:
		print("  %-16s mean height %.0f m" % [STAND_NAMES[st], mean_h[st].x / mean_h[st].y])
	if mean_h.has(TreeGenerator.STAND_SUBALPINE) and mean_h.has(TreeGenerator.STAND_MONTANE_CONIFER):
		var sa: Vector2 = mean_h[TreeGenerator.STAND_SUBALPINE]
		var mo: Vector2 = mean_h[TreeGenerator.STAND_MONTANE_CONIFER]
		_check(sa.x / sa.y > mo.x / mo.y, "subalpine belt above the montane belt")
	var bare := 0
	for s in sps.size():
		for p in f.get_placements(s):
			if map.get_temperature_at(p) < -3.5:
				bare += 1
	_check(bare == 0, "no trees above the tree line (%d)" % bare)
	img.save_png(path)
	print("saved ", path)

# Species names carry no stand: find it from the species order make_woodland() uses.
var _species_stand := {}
func _stand_of_species(sp: PlantSpecies) -> int:
	if _species_stand.has(sp):
		return _species_stand[sp]
	var best := 0
	# make_woodland() builds stands in this order; match by the patch seed each stand sets, or by
	# the stand's distinctive rules.
	var order := [TreeGenerator.STAND_RIPARIAN, TreeGenerator.STAND_SUBALPINE, TreeGenerator.STAND_MONTANE_CONIFER, TreeGenerator.STAND_BEECH, TreeGenerator.STAND_OAK_MIXED, TreeGenerator.STAND_PINE_HEATH]
	best = sp.get_meta("stand", 0)
	_species_stand[sp] = best
	return best

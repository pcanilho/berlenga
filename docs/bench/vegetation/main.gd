extends Node3D
# VegetationField3D placement checks and cost, on the terrain-data valley (river down the
# floor, lake at the low end). Three species built in code, in priority order: trees on dry
# gentle ground away from water, bushes between them, reeds in shallow water and on wet banks.
#
# godot --path docs/bench/vegetation --rendering-driver vulkan -- [options]
#   --test           run the checks, print PASS/FAIL, quit
#   --out=path.png   render a view (path traced unless --raster)
#   --raster         raster only
#   --spp=N          path tracing samples (default 16)
#   --stream-cost    a 1 km map of trees over the oak wood's floor (PlantGenerator.make_floor),
#                    streamed 200 m around a point running across it at 6 m/s: prints the
#                    main thread's slowest frame and map publish each second
#   --floor[=off]    the oak wood's floor (make_floor) under crowns, from standing height, with the small
#                    species' pixel reach on (off: their hand-set ranges): walks VEG_FLOOR_WALK m (30) and
#                    prints the main thread's streaming time a frame, memory, primitives, draw calls and
#                    the frame time; with --out also saves the view (path traced unless --raster)
#   --desert[=off]   a transverse sand sea (TerrainGenerator3D.sand_sea_rect) with make_desert's flanks and
#                    interdune sets streamed round a camera 1.7 m up (off: no plants): checks the plants keep
#                    off the crests and slip faces by the sand's mobility, prints the same costs as --floor
#                    and with --out saves the view. VEG_DESERT_HIGH=1 looks from 12 m up across the dunes.

const SIZE := 256.0
var size := SIZE
var stream_cost := false
var runner: Node3D
var stream_t := 0.0
var test := false
var raster := false
var spp := 16
var out := ""
var deadwood_view := false
var stone_view := "" # --stones=scree|river|shore|stream: plants between stones (PlantGenerator.make_stones).
var stone_plants := true # --stones-off: the same stones, no plants.
var far_volume_view := 0 # --far-volume: 1 with the far plants as volume, 2 without (--far-volume=off).
var far_match := 0 # --far-match: the far-volume view without volume, 4 m up, on a ground drawing the far colour, waiting for the far shade match; 2 = match off (=off), 3 = shade forced to 0.3 (=dark).
var floor_view := 0 # --floor: 1 with the small species' pixel reach, 2 without (--floor=off).
var desert_view := 0 # --desert: 1 with make_desert's plants on a sand sea, 2 the same view without (--desert=off).
var mushroom_view := 0 # --mushrooms: 1 with the mushrooms, 2 the same view without them (--mushrooms=off).
var n := 0
var dm: TerrainDataMap3D
var field: VegetationField3D
var fails := 0

class Lake extends Node3D:
	func get_water_level() -> float:
		return 0.5

class River extends Node3D:
	func sample_height(p: Vector3) -> float:
		if absf(p.x) > 8.0 or p.z < -100.0 or p.z > 20.0:
			return -INF
		return 0.03 * (p.z + 100.0) + 0.3

# Geology for the soil and rock rules: bare rock east of x = 60 m (a cliff band), thin soil
# between 40 and 60 m, deep soil elsewhere.
class Geology extends Node3D:
	func get_geology_at(p: Vector3) -> Color:
		if p.x > 60.0:
			return Color(0.0, 1.0, 0.0, 0.0)
		if p.x > 40.0:
			return Color(0.15, 0.4, 0.0, 0.0)
		return Color(1.2, 0.0, 0.0, 0.0)
	# A waterfall's spray zone: soaked within 15 m of (-40, -40), drying out to 25 m.
	func get_spray_potential(p: Vector3) -> float:
		return 1.0 - smoothstep(15.0, 25.0, Vector2(p.x, p.z).distance_to(Vector2(-40, -40)))

static func height(x: float, z: float) -> float:
	var pit := -0.8 * exp(-(x * x + (z - 40.0) * (z - 40.0)) / 30.0)
	return 0.03 * (z + 100.0) + 0.12 * sqrt(x * x + 25.0) - 0.6 + 0.1 * sin(x * 0.31) * sin(z * 0.27) + pit

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--stones="):
			stone_view = a.substr(9)
			size = 48.0
		elif a.begins_with("--far-match"):
			far_match = 2 if a == "--far-match=off" else (3 if a == "--far-match=dark" else 1)
			far_volume_view = 2
		elif a.begins_with("--far-volume"):
			far_volume_view = 2 if a == "--far-volume=off" else 1
		elif a == "--stones-off":
			stone_plants = false
		elif a.begins_with("--floor"):
			floor_view = 2 if a == "--floor=off" else 1
			size = 128.0
		elif a.begins_with("--desert"):
			desert_view = 2 if a == "--desert=off" else 1
		elif a.begins_with("--mushrooms"):
			mushroom_view = 2 if a == "--mushrooms=off" else 1
			deadwood_view = true
			size = 48.0
		elif a == "--deadwood":
			deadwood_view = true
			size = 48.0 # A patch of the stand: lavapipe renders the whole map's trees too slowly.
			if OS.get_environment("VEG_DW_SIZE") != "":
				size = float(OS.get_environment("VEG_DW_SIZE")) # A bigger patch, for more logs to pick from.
		elif a == "--stream-cost":
			stream_cost = true
			size = 1024.0
	var w := int(size) + 1
	var data := PackedFloat32Array()
	data.resize(w * w)
	for z in w:
		for x in w:
			data[z * w + x] = height(x - size * 0.5, z - size * 0.5)
	var shape := HeightMapShape3D.new()
	shape.map_width = w
	shape.map_depth = w
	shape.map_data = data
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	body.add_child(cs)
	add_child(body)
	var lake := Lake.new()
	lake.name = "Lake"
	add_child(lake)
	var river := River.new()
	river.name = "River"
	add_child(river)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = size
	dm.resolution = 512 if stream_cost else 256
	dm.bake_on_ready = false
	dm.water_sources = [NodePath("../Lake"), NodePath("../River")]
	add_child(dm)
	field = VegetationField3D.new()
	field.far_volume = false # Not under test here.
	field.name = "Field"
	field.populate_on_ready = false
	field.biome = _biome()
	add_child(field)
	field.data_map = field.get_path_to(dm)
	if stream_cost:
		_setup_stream_cost()
	if not out.is_empty() and desert_view == 0:
		_build_view()

func _mesh(m: PrimitiveMesh, lift: float, col: Color) -> Mesh:
	var st := SurfaceTool.new()
	st.append_from(m, 0, Transform3D(Basis(), Vector3(0, lift, 0)))
	var mat := StandardMaterial3D.new()
	mat.albedo_color = col
	mat.roughness = 0.8
	st.set_material(mat)
	return st.commit()

func _biome() -> Biome:
	var cone := CylinderMesh.new()
	cone.top_radius = 0.0
	cone.bottom_radius = 1.3
	cone.height = 5.0
	var tree := PlantSpecies.new()
	tree.meshes = [_mesh(cone, 2.5, Color(0.12, 0.3, 0.12))]
	tree.spacing = 5.0
	tree.footprint = 2.0
	tree.density = 0.6
	tree.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 20))
	tree.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
	tree.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6, 1000000))
	tree.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	var ball := SphereMesh.new()
	ball.radius = 0.6
	ball.height = 1.0
	var bush := PlantSpecies.new()
	bush.meshes = [_mesh(ball, 0.4, Color(0.25, 0.35, 0.12))]
	bush.spacing = 1.5
	bush.footprint = 0.6
	bush.density = 0.5
	bush.align_to_ground = 0.5
	bush.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 35))
	bush.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
	bush.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(2, 1000000))
	bush.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.0)
	var stalk := CylinderMesh.new()
	stalk.top_radius = 0.01
	stalk.bottom_radius = 0.03
	stalk.height = 1.6
	stalk.radial_segments = 5
	var reed := PlantSpecies.new()
	reed.meshes = [_mesh(stalk, 0.8, Color(0.45, 0.5, 0.25)), _mesh(stalk, 0.6, Color(0.5, 0.48, 0.28))]
	reed.spacing = 0.4
	reed.footprint = 0.0
	reed.density = 0.9
	reed.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0, 0.15))
	reed.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.05)
	reed.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(-6, 3))
	reed.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.0)
	reed.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 15))
	var b := Biome.new()
	b.species = [tree, bush, reed]
	return b

func _setup_stream_cost() -> void:
	var tree: PlantSpecies = field.biome.species[0]
	tree.canopy_radius = 3.0
	tree.canopy_opacity = 0.9
	var species: Array = [tree]
	species.append_array(Array(PlantGenerator.make_floor(PlantGenerator.FLOOR_OAK_MIXED, 3, false)))
	field.biome = Biome.new()
	field.biome.species = species
	field.tile_size = 16.0
	runner = Node3D.new()
	add_child(runner)
	runner.position = Vector3(-300, 0, -300)
	field.stream_target = field.get_path_to(runner)
	field.stream_radius = 200.0
	field.cover_map_enabled = true
	var cam := Camera3D.new()
	cam.position = Vector3(0, 400, 0)
	cam.rotation_degrees = Vector3(90, 0, 0) # At the sky: the cost of streaming, not of drawing.
	add_child(cam)

func _process(dt: float) -> void:
	if not stream_cost or n < 3:
		return
	if not field.streaming:
		print("STREAM_COST species=%d" % field.biome.species.size())
		var tc := Time.get_ticks_usec()
		field.streaming = true
		await get_tree().process_frame
		await get_tree().process_frame
		if field.get_cover_texture():
			field.get_cover_texture().get_image().save_png("user://cover_map.png")
			print("STREAM_COST cover map %s saved, first frames %.0f ms" % [str(field.get_cover_texture().get_size()), (Time.get_ticks_usec() - tc) / 1000.0])
	stream_t += dt
	# Fixed steps, as if at 60 fps, so the run covers the same ground however slow the frame.
	runner.position += Vector3(1, 0, 1).normalized() * 6.0 / 60.0
	if Engine.get_process_frames() % 60 == 0:
		var st := field.get_stream_stats()
		print("STREAM_COST t=%.0fs pos=%s tiles=%d jobs=%d plants=%d frame_max=%.2fms publish=%.1fms idle=%s" % [stream_t, str(Vector2(runner.position.x, runner.position.z).round()), st.tiles, st.jobs, field.get_plant_count(), st.frame_max_msec, st.publish_msec, str(field.is_stream_idle())])
	if runner.position.x > 300.0:
		get_tree().quit()

func _physics_process(_d: float) -> void:
	n += 1
	if n != 3:
		return
	dm.bake()
	if stream_cost:
		return
	if deadwood_view:
		await _deadwood_view()
		return
	if floor_view > 0:
		await _floor_view()
		return
	if desert_view > 0:
		await _desert_view()
		return
	if stone_view != "":
		_stones_view()
		_render_later()
		return
	if far_volume_view > 0:
		_far_volume_setup()
	var err := field.populate()
	if far_volume_view > 0:
		await _far_volume_stats()
	var counts := []
	for s in mini(3, field.biome.species.size()):
		counts.append(field.get_placements(s).size())
	print("VEG_POPULATE err=%d plants=%d per_species=%s ms=%.1f" % [err, field.get_plant_count(), str(counts), field.get_populate_time_msec()])
	if test:
		_checks()
		await _stream_checks()
		await _lazy_near_checks()
		_deadwood_checks()
		await _place_form_checks()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
	if out.is_empty():
		get_tree().quit(1 if fails > 0 else 0)
	else:
		_render_later()

# --floor[=off]: the cost of the small species' pixel reach (VegetationField3D.detail_reach) on a streamed oak
# wood floor under simple crowns, seen from standing height.
func _floor_view() -> void:
	field.queue_free()
	var cone := CylinderMesh.new()
	cone.top_radius = 0.0
	cone.bottom_radius = 1.3
	cone.height = 5.0
	var tree := PlantSpecies.new()
	tree.meshes = [_mesh(cone, 2.5, Color(0.12, 0.3, 0.12))]
	tree.spacing = 6.0
	tree.footprint = 2.0
	tree.density = 0.7
	tree.canopy_radius = 3.0
	tree.canopy_opacity = 0.9
	tree.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6, 1000000))
	tree.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	var wood := VegetationField3D.new()
	wood.far_volume = false
	wood.populate_on_ready = false
	var wb := Biome.new()
	wb.species = [tree]
	wood.biome = wb
	add_child(wood)
	wood.data_map = wood.get_path_to(dm)
	wood.populate()
	var at := Vector3(30, 0, -30)
	at.y = height(at.x, at.z)
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = at + Vector3(0, 1.7, 0)
	cam.look_at(Vector3(at.x, height(at.x, at.z + 20.0), at.z + 20.0))
	cam.current = true
	var close := OS.get_environment("VEG_FLOOR_CLOSE") != "" # Looking down 3 m ahead: the near litter.
	var ground := VegetationField3D.new()
	ground.far_volume = false
	ground.name = "Floor"
	ground.populate_on_ready = false
	ground.detail_reach = floor_view == 1
	ground.detail_reference_height = 1080 # The reach of a 1080p screen, not of the bench's small window.
	var gb := Biome.new()
	var floor_set: Array[PlantSpecies] = []
	floor_set.assign(PlantGenerator.make_floor(PlantGenerator.FLOOR_OAK_MIXED, 3, false))
	if OS.get_environment("VEG_FLOOR_NEAR") == "0":
		# The litter without its leaf-on-leaf near cards (PlantGenerator.litter_deposit).
		for sp in floor_set:
			if sp.near_range == 7.0 and sp.near_meshes.size() > 0:
				sp.near_meshes = []
				sp.near_range = 0.0
	gb.species = floor_set
	ground.biome = gb
	ground.tile_size = 16.0
	ground.stream_radius = 60.0
	add_child(ground)
	ground.stream_target = ground.get_path_to(cam)
	ground.data_map = ground.get_path_to(dm)
	ground.canopy_field = ground.get_path_to(wood)
	var ranges := []
	for i in gb.species.size():
		var sp: PlantSpecies = gb.species[i]
		if sp.detail_size > 0.0:
			ranges.append("%d %.0f->%.0f m" % [sp.get_meta("plant_preset", -1), sp.visibility_range, ground.get_species_range(i)])
	print("VEG_FLOOR reach %s: %s" % ["on" if floor_view == 1 else "off", ", ".join(ranges)])
	var mem0 := OS.get_static_memory_usage()
	var vid0 := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)
	ground.streaming = true
	# Streaming is main-thread work, not drawing: the software renderer would take seconds a frame.
	get_viewport().disable_3d = true
	var warm := 0
	while warm < 1200 and (warm < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		warm += 1
	# Walking: the main thread's streaming time a frame, at a fixed 0.1 m a frame (6 m/s at 60 fps).
	var walk := float(OS.get_environment("VEG_FLOOR_WALK")) if OS.get_environment("VEG_FLOOR_WALK") != "" else 30.0
	var ms: Array[float] = []
	var walked := 0.0
	ground.get_stream_stats()
	while walked < walk:
		cam.position += Vector3(0, 0, 0.1)
		cam.position.y = height(cam.position.x, cam.position.z) + 1.7
		walked += 0.1
		await get_tree().process_frame
		ms.append(float(ground.get_stream_stats().frame_max_msec))
	var settle := 0
	while settle < 1200 and not ground.is_stream_idle():
		await get_tree().process_frame
		settle += 1
	ms.sort()
	var total := 0.0
	for v in ms:
		total += v
	get_viewport().disable_3d = false
	if close:
		cam.look_at(Vector3(cam.position.x, height(cam.position.x, cam.position.z + 3.0), cam.position.z + 3.0))
		for i in 30:
			await get_tree().process_frame
	var nodes := 0
	for ch in ground.get_children(true):
		if ch is MultiMeshInstance3D:
			nodes += 1
	# Frame time: the mean of 12 frames once the view has settled (and the path tracer has built every surface).
	for i in 6:
		await RenderingServer.frame_post_draw
	var waited := 0
	while not raster and waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		await RenderingServer.frame_post_draw
		waited += 1
	var t0 := Time.get_ticks_usec()
	for i in 12:
		await RenderingServer.frame_post_draw
	var frame_ms := (Time.get_ticks_usec() - t0) / 12000.0
	print("VEG_FLOOR walk %.0f m in %d frames: streaming mean %.2f ms, p95 %.2f ms, worst %.2f ms" % [walk, ms.size(), total / maxi(ms.size(), 1), ms[int(ms.size() * 0.95)], ms[-1]])
	print("VEG_FLOOR plants %d, cells %d, memory +%.1f MB, video +%.1f MB, primitives %d, draw calls %d, objects %d, %s frame %.1f ms" % [
		ground.get_plant_count(), nodes, (OS.get_static_memory_usage() - mem0) / 1048576.0,
		(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) - vid0) / 1048576.0,
		Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME), Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME), "raster" if raster else "path traced", frame_ms])
	if out.is_empty():
		get_tree().quit(0)
	else:
		_render_later()

# --desert[=off]: make_desert's two stands on a transverse sand sea, streamed round a walker; checks the plants by
# the sand's mobility (the field's RULE_SAND_MOBILITY, worked out again here from the generator's sand depth).
func _desert_view() -> void:
	field.queue_free()
	var gen := TerrainGenerator3D.new()
	gen.name = "DesertGen"
	# Heavy generator options are on by default; this bench keeps its own fast choices.
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.tile_erosion_enabled = false
	gen.strata_enabled = false
	gen.guide_preview = false
	gen.dunes_enabled = false
	gen.deposition = 0.0
	gen.moraine_age = 0.0
	gen.scree_age = 0.0
	gen.meander_age = 0.0
	gen.outwash_age = 0.0
	gen.base_uplift = 0.0
	gen.size = Vector2(1024, 1024)
	gen.cell_size = 8.0
	gen.seed = 3
	gen.sand_sea_rect = Rect2(-400, -400, 800, 800)
	gen.sand_sea_scale = 4.0
	gen.sand_sea_depth = 8.0
	gen.sand_sea_steps = 200
	gen.sand_sea_edge = 150.0
	add_child(gen)
	var t := Time.get_ticks_msec()
	gen.generate()
	print("VEG_DESERT sand sea %.0f ms, dunes up to %.1f m (generate %d ms)" % [gen.get_last_sand_sea_msec(), gen.get_last_sand_sea_max(), Time.get_ticks_msec() - t])
	var map := TerrainDataMap3D.new()
	map.name = "DesertMap"
	map.size = 256.0
	map.resolution = 256
	map.bake_on_ready = false
	add_child(map)
	map.height_source = map.get_path_to(gen)
	map.geology_source = map.get_path_to(gen)
	map.bake()
	var mob := _desert_mobility(gen)
	var high := OS.get_environment("VEG_DESERT_HIGH") != ""
	var at := Vector3(-60, 0, -20)
	at.y = gen.get_height(at)
	var cam := Camera3D.new()
	add_child(cam)
	cam.far = 600.0
	cam.position = at + Vector3(0, 12.0 if high else 1.7, 0)
	var look := Vector3(at.x + 40.0, 0, at.z + 30.0)
	look.y = gen.get_height(look) + (0.0 if high else 1.0)
	cam.look_at(look)
	cam.current = true
	var ground := VegetationField3D.new()
	ground.far_volume = false
	ground.name = "Desert"
	ground.populate_on_ready = false
	ground.detail_reference_height = 1080
	var gb := Biome.new()
	var desert_set: Array[PlantSpecies] = []
	desert_set.assign(Array(PlantGenerator.make_desert(PlantGenerator.DESERT_FLANKS, 3, true)) + Array(PlantGenerator.make_desert(PlantGenerator.DESERT_INTERDUNE, 3, true)))
	gb.species = desert_set if desert_view == 1 else ([] as Array[PlantSpecies])
	ground.biome = gb
	ground.tile_size = 16.0
	ground.stream_radius = 60.0
	add_child(ground)
	ground.stream_target = ground.get_path_to(cam)
	ground.data_map = ground.get_path_to(map)
	var mem0 := OS.get_static_memory_usage()
	var vid0 := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)
	ground.streaming = true
	get_viewport().disable_3d = true
	var warm := 0
	while warm < 1200 and (warm < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		warm += 1
	if desert_view == 1:
		_desert_checks(ground, desert_set, gen, map, mob)
	# Walking 30 m (VEG_FLOOR_WALK) along the wind, across the dunes, at 0.1 m a frame.
	var walk := float(OS.get_environment("VEG_FLOOR_WALK")) if OS.get_environment("VEG_FLOOR_WALK") != "" else 30.0
	var ms: Array[float] = []
	var walked := 0.0
	var start := cam.position
	ground.get_stream_stats()
	while walked < walk:
		cam.position += Vector3(0.1, 0, 0)
		cam.position.y = gen.get_height(cam.position) + 1.7
		walked += 0.1
		await get_tree().process_frame
		ms.append(float(ground.get_stream_stats().frame_max_msec))
	cam.position = start
	var settle := 0
	while settle < 1200 and (settle < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		settle += 1
	ms.sort()
	var total := 0.0
	for v in ms:
		total += v
	get_viewport().disable_3d = false
	_desert_scene(gen, cam)
	for i in 30:
		await get_tree().process_frame
	for i in 6:
		await RenderingServer.frame_post_draw
	var waited := 0
	while not raster and waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		await RenderingServer.frame_post_draw
		waited += 1
	var t0 := Time.get_ticks_usec()
	for i in 12:
		await RenderingServer.frame_post_draw
	var frame_ms := (Time.get_ticks_usec() - t0) / 12000.0
	print("VEG_DESERT walk %.0f m in %d frames: streaming mean %.2f ms, p95 %.2f ms, worst %.2f ms" % [walk, ms.size(), total / maxi(ms.size(), 1), ms[int(ms.size() * 0.95)], ms[-1]])
	print("VEG_DESERT plants %d, memory +%.1f MB, video +%.1f MB, primitives %d, draw calls %d, objects %d, %s frame %.1f ms" % [
		ground.get_plant_count(), (OS.get_static_memory_usage() - mem0) / 1048576.0,
		(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) - vid0) / 1048576.0,
		Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME), Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME), "raster" if raster else "path traced", frame_ms])
	if desert_view == 1:
		print("VEG_DESERT_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
	if out.is_empty():
		get_tree().quit(1 if fails > 0 else 0)
	else:
		_render_later()

# The sand's mobility as VegetationField3D works it out (_read_sand_mobility): the depth's place between its low and
# high within 12 cells, 0 under thin sand, -1 off the sand. Returns [grid, nx, ny, rect].
func _desert_mobility(gen: TerrainGenerator3D) -> Array:
	var rect := gen.sand_sea_rect
	var c := gen.sand_sea_scale
	var nx := int(ceil(rect.size.x / c)) + 1
	var ny := int(ceil(rect.size.y / c)) + 1
	var d := PackedFloat32Array()
	d.resize(nx * ny)
	for z in ny:
		for x in nx:
			d[z * nx + x] = gen.get_sand_sea_height(Vector3(rect.position.x + rect.size.x * x / (nx - 1), 0, rect.position.y + rect.size.y * z / (ny - 1)))
	var r := 12
	var lox := PackedFloat32Array()
	var hix := PackedFloat32Array()
	lox.resize(nx * ny)
	hix.resize(nx * ny)
	for z in ny:
		for x in nx:
			var a := 1e9
			var b := -1e9
			for k in range(maxi(x - r, 0), mini(x + r, nx - 1) + 1):
				a = minf(a, d[z * nx + k])
				b = maxf(b, d[z * nx + k])
			lox[z * nx + x] = a
			hix[z * nx + x] = b
	var m := PackedFloat32Array()
	m.resize(nx * ny)
	for z in ny:
		for x in nx:
			var a := 1e9
			var b := -1e9
			for k in range(maxi(z - r, 0), mini(z + r, ny - 1) + 1):
				a = minf(a, lox[k * nx + x])
				b = maxf(b, hix[k * nx + x])
			var v := d[z * nx + x]
			if v < 0.05:
				m[z * nx + x] = -1.0
			else:
				m[z * nx + x] = smoothstep(0.3, 0.85, (v - a) / (b - a + 0.3)) * smoothstep(0.2, 0.6, v)
	return [m, nx, ny, rect]

func _desert_mob_at(mob: Array, p: Vector3) -> float:
	var rect: Rect2 = mob[3]
	var x := clampi(roundi((p.x - rect.position.x) / rect.size.x * (mob[1] - 1)), 0, mob[1] - 1)
	var z := clampi(roundi((p.z - rect.position.y) / rect.size.y * (mob[2] - 1)), 0, mob[2] - 1)
	return mob[0][z * mob[1] + x]

func _desert_checks(ground: VegetationField3D, dset: Array[PlantSpecies], gen: TerrainGenerator3D, map: TerrainDataMap3D, mob: Array) -> void:
	# Per preset: plants, on a crest (mobility over 0.85 here, the near cell), on a slip face (over 30 deg),
	# share on the flanks (0.1 to 0.8), on stable sand (under 0.2), mean slope.
	# Keyed by preset, and drinn by stand (the flanks' set comes first, 89; the interdune drinn as -89).
	var stats := {}
	var seen_drinn := false
	for s in dset.size():
		var key: int = dset[s].get_meta("plant_preset", -1)
		if key == PlantGenerator.PRESET_DRINN:
			key = -key if seen_drinn else key
			seen_drinn = true
		if not stats.has(key):
			stats[key] = [0, 0, 0, 0, 0]
		var st: Array = stats[key]
		for p in ground.get_placements(s):
			var m := _desert_mob_at(mob, p)
			var slope := rad_to_deg(map.get_slope_at(p))
			st[0] += 1
			if maxf(m, smoothstep(20.0, 30.0, slope)) > 0.85:
				st[1] += 1
			if slope > 30.0:
				st[2] += 1
			if m > 0.07 and m < 0.8: # the flanks set: 0.15 to 0.7, its fade 0.08
				st[3] += 1
			if m > -0.5 and m < 0.25:
				st[4] += 1
	print("VEG_DESERT per preset [plants, crest, slip face, flanks, stable]: ", stats)
	var drinn: Array = stats.get(PlantGenerator.PRESET_DRINN, [0, 0, 0, 0, 0])
	var crust: Array = stats.get(PlantGenerator.PRESET_SOIL_CRUST, [0, 0, 0, 0, 0])
	var annual: Array = stats.get(PlantGenerator.PRESET_DESERT_ANNUALS, [0, 0, 0, 0, 0])
	var crest := 0
	var slip := 0
	for k in stats:
		crest += stats[k][1]
		slip += stats[k][2]
	# Mobility over the sea: crests and corridors both there.
	var hi := 0
	var lo := 0
	for v in mob[0]:
		if v > 0.85:
			hi += 1
		elif v > -0.5 and v < 0.12:
			lo += 1
	_check("desert: the sand sea has mobile crests and stable corridors", hi > 200 and lo > 200, [hi, lo, mob[0].size()])
	_check("desert: no plant on a crest or a slip face", crest == 0 and slip == 0 and drinn[0] > 50, [crest, slip])
	_check("desert: the flanks' drinn on the flanks", drinn[0] > 50 and drinn[3] > drinn[0] * 0.7, [drinn[0], drinn[3]])
	# Mobility here is the nearest cell, the field's bilinear: a fade's width (0.08) of slack.
	_check("desert: crust only on stable sand", crust[0] > 500 and crust[4] == crust[0], [crust[0], crust[4]])
	_check("desert: annuals between the dunes, not up them", annual[0] > 500 and annual[3] < annual[0] * 0.5, [annual[0], annual[3]])

# The desert's ground, sky and sun for the view.
func _desert_scene(gen: TerrainGenerator3D, cam: Camera3D) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var res := 240
	var step := 1.0
	var o := Vector3(cam.position.x - 60.0, 0, cam.position.z - 60.0)
	var hs := PackedFloat32Array()
	hs.resize((res + 1) * (res + 1))
	for z in res + 1:
		for x in res + 1:
			hs[z * (res + 1) + x] = gen.get_height(o + Vector3(x * step, 0, z * step))
	for z in res:
		for x in res:
			var q := [Vector3(x, hs[z * (res + 1) + x], z), Vector3(x + 1, hs[z * (res + 1) + x + 1], z),
					Vector3(x + 1, hs[(z + 1) * (res + 1) + x + 1], z + 1), Vector3(x, hs[(z + 1) * (res + 1) + x], z + 1)]
			for i in [0, 1, 2, 0, 2, 3]:
				st.add_vertex(o + Vector3(q[i].x * step, q[i].y, q[i].z * step))
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.72, 0.55, 0.36) # Quartz sand with an iron-oxide coat.
	gm.roughness = 0.95
	mi.material_override = gm
	add_child(mi)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.62, 0.72, 0.86)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.55, 0.6, 0.7)
	env.ambient_light_energy = 0.6
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-28, 120, 0)
	sun.light_energy = 1.3
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 80.0
	add_child(sun)

# Mean and standard deviation of a cover map's alpha, inside its border.
func _alpha_stats(img: Image) -> Array:
	var sum := 0.0
	var sq := 0.0
	var n := 0
	for y in range(2, img.get_height() - 2):
		for x in range(2, img.get_width() - 2):
			var a := img.get_pixel(x, y).a
			sum += a
			sq += a * a
			n += 1
	var m := sum / maxf(n, 1)
	return [m, sqrt(maxf(sq / maxf(n, 1) - m * m, 0.0))]

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

func _min_spacing(pts: PackedVector3Array, r: float) -> float:
	var grid := {}
	for i in pts.size():
		var k := Vector2i(floori(pts[i].x / r), floori(pts[i].z / r))
		if not grid.has(k): grid[k] = []
		grid[k].append(i)
	var best := INF
	for i in pts.size():
		var k := Vector2i(floori(pts[i].x / r), floori(pts[i].z / r))
		for dz in range(-1, 2):
			for dx in range(-1, 2):
				for j in grid.get(k + Vector2i(dx, dz), []):
					if j != i:
						best = minf(best, Vector2(pts[i].x, pts[i].z).distance_to(Vector2(pts[j].x, pts[j].z)))
	return best

# Trees shaped by their place (trees module, 627a91de): wind-flagged species turned with mesh +X
# downwind; bank forms (mesh meta tree_ground_slope) picked by the ground's slope, +Z downhill.
func _place_form_checks() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(0.4, 1.0, 0.2)
	var flag := PlantSpecies.new()
	flag.meshes = [box]
	flag.spacing = 6.0
	flag.footprint = 0.0
	flag.set_meta("wind_flag", true)
	var f := VegetationField3D.new()
	f.far_volume = false # Not under test here.
	f.populate_on_ready = false
	f.litter_wind = Vector2(0.3, 0.4)
	var b := Biome.new()
	b.species = [flag]
	f.biome = b
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var wind := Vector2(0.6, 0.8)
	var n := 0
	var off := 0
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D:
			for i in ch.multimesh.instance_count:
				var x: Vector3 = ch.multimesh.get_instance_transform(i).basis.x
				n += 1
				if Vector2(x.x, x.z).normalized().dot(wind) < cos(0.2):
					off += 1
	_check("wind-flagged trees: mesh +X down the prevailing wind", n > 20 and off == 0, [n, off])
	f.queue_free()
	# Bank forms grown on 0, 0.15 and 0.3 rad of slope.
	var meshes: Array[Mesh] = []
	for k in 3:
		var m := BoxMesh.new()
		m.size = Vector3(0.3, 1.0, 0.3)
		var am := ArrayMesh.new()
		am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, m.get_mesh_arrays())
		if k > 0: # The flat form carries no slope (as the trees module's).
			am.set_meta("tree_ground_slope", Vector2(0.15 * k, 0.2))
		meshes.append(am)
	var bank := PlantSpecies.new()
	bank.meshes = meshes
	bank.spacing = 6.0
	bank.footprint = 0.0
	var fb := VegetationField3D.new()
	fb.far_volume = false # Not under test here.
	fb.populate_on_ready = false
	var bb := Biome.new()
	bb.species = [bank]
	fb.biome = bb
	add_child(fb)
	fb.data_map = fb.get_path_to(dm)
	fb.populate()
	var total := 0
	var wrong_variant := 0
	var wrong_turn := 0
	for ch in fb.get_children(true):
		if not ch is MultiMeshInstance3D:
			continue
		var vm: Mesh = ch.multimesh.mesh
		var vs: float = Vector2(vm.get_meta("tree_ground_slope", Vector2())).x
		for i in ch.multimesh.instance_count:
			var t: Transform3D = ch.multimesh.get_instance_transform(i)
			var p := fb.global_transform * t.origin
			var sl := dm.get_slope_at(p)
			total += 1
			var best := 1e9
			for k in 3:
				best = minf(best, absf(0.15 * k - sl))
			if absf(vs - sl) > best + 1e-3:
				wrong_variant += 1
			var asp := dm.get_aspect_at(p)
			if vs > 0.0 and sl > 0.02:
				var z := t.basis.z
				if Vector2(z.x, z.z).normalized().dot(Vector2(sin(asp), cos(asp))) < 0.98:
					wrong_turn += 1
	_check("bank trees: the variant grown on the nearest slope, its ground falling (+Z) downhill", total > 20 and wrong_variant == 0 and wrong_turn == 0, [total, wrong_variant, wrong_turn])
	fb.queue_free()
	# Roots over a stone (tree_root_stone: centre, half extents): floor plants keep off it.
	var tm := ArrayMesh.new()
	tm.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, box.get_mesh_arrays())
	tm.set_meta("tree_root_stone", PackedFloat32Array([0.6, 0.1, 0.2, 0.35, 0.25, 0.3]))
	var st := PlantSpecies.new()
	st.meshes = [tm]
	st.spacing = 6.0
	st.footprint = 0.0
	var floor_sp := PlantSpecies.new()
	floor_sp.meshes = [box]
	floor_sp.spacing = 0.15
	floor_sp.footprint = 0.0
	var fs := VegetationField3D.new()
	fs.far_volume = false # Not under test here.
	fs.populate_on_ready = false
	var sb := Biome.new()
	sb.species = [st, floor_sp]
	fs.biome = sb
	add_child(fs)
	fs.data_map = fs.get_path_to(dm)
	fs.populate()
	var stones := []
	for ch in fs.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh.mesh == tm:
			for i in ch.multimesh.instance_count:
				stones.append(fs.global_transform * (ch.multimesh.get_instance_transform(i) * Vector3(0.6, 0.1, 0.2)))
	var on_stone := 0
	var near := 0
	for q in fs.get_placements(1):
		for c in stones:
			var d := Vector2(q.x - c.x, q.z - c.z).length()
			if d < 0.3 * 0.8:
				on_stone += 1
			elif d < 0.6:
				near += 1
	_check("root stones: no floor plant on the stone a tree's roots grow over, plants right round it", stones.size() > 20 and on_stone == 0 and near > stones.size(), [stones.size(), on_stone, near])
	fs.queue_free()
	await get_tree().process_frame

func _checks() -> void:
	# Litter only where crowns drop it: every floor set's litter rule is 0 on open ground (litter 0).
	var open_litter := []
	for fs in [PlantGenerator.FLOOR_OAK_MIXED, PlantGenerator.FLOOR_BEECH]:
		for sp in PlantGenerator.make_floor(fs, 1, false):
			var lr: Vector2 = sp.get_rule_range(PlantSpecies.RULE_LITTER)
			if lr.x > 0.0 and not (lr.x - sp.get_rule_fade(PlantSpecies.RULE_LITTER) > 0.0):
				open_litter.append(sp.resource_name)
	_check("litter: no fallen leaves or twigs on open ground away from crowns", open_litter.is_empty(), open_litter)
	# Dunes on a generator's dunes (RULE_DUNE): embryo and mobile sets on moving sand, fixed on held
	# sand, none off the dune sand (-1).
	var dune_bad := []
	for stand in [PlantGenerator.DUNES_EMBRYO, PlantGenerator.DUNES_MOBILE, PlantGenerator.DUNES_FIXED]:
		for sp in PlantGenerator.make_dunes(stand, 1, false):
			var off: float = sp.rule_weight(PlantSpecies.RULE_DUNE, -1.0)
			var dr: Vector2 = sp.get_rule_range(PlantSpecies.RULE_DUNE)
			var moving: bool = stand != PlantGenerator.DUNES_FIXED
			if dr.x - sp.get_rule_fade(PlantSpecies.RULE_DUNE) <= -1.0 or off > 0.0 or (moving and dr.y > 0.85) or (not moving and dr.x < 0.5):
				dune_bad.append([stand, dr])
	_check("dunes: dune sets follow the generator's dune stability, none off the dune sand", dune_bad.is_empty(), dune_bad)
	var trees := field.get_placements(0)
	var bushes := field.get_placements(1)
	var reeds := field.get_placements(2)
	_check("every species placed", trees.size() > 50 and bushes.size() > 200 and reeds.size() > 500, [trees.size(), bushes.size(), reeds.size()])
	var bad := 0
	for p in reeds:
		var d := dm.get_water_distance_at(p)
		if d < -7.0 or d > 4.0 or dm.get_water_depth_at(p) > 0.2:
			bad += 1
	_check("reeds only in shallow water and on banks", bad == 0, bad)
	bad = 0
	var near := 0
	for p in trees:
		if dm.get_water_distance_at(p) < 3.0 or rad_to_deg(dm.get_slope_at(p)) > 25.0:
			bad += 1
		if dm.get_water_distance_at(p) < 6.0:
			near += 1
	_check("no tree in water, on steep ground or within 3 m of water", bad == 0, bad)
	_check("trees thin out inside the 6 m fade", near < trees.size() / 10, near)
	bad = 0
	for b in bushes:
		for t in trees:
			if Vector2(b.x, b.z).distance_to(Vector2(t.x, t.z)) < 2.6 - 0.001:
				bad += 1
	_check("no bush inside a tree footprint", bad == 0, bad)
	_check("tree spacing >= 5 m", _min_spacing(trees, 5.0) >= 4.999, _min_spacing(trees, 5.0))
	_check("bush spacing >= 1.5 m", _min_spacing(bushes, 1.5) >= 1.499, _min_spacing(bushes, 1.5))
	_check("reed spacing >= 0.4 m", _min_spacing(reeds, 0.4) >= 0.399, _min_spacing(reeds, 0.4))
	# Plants stand on the ground.
	var off := 0.0
	for p in trees:
		off = maxf(off, absf(p.y - height(p.x, p.z)))
	_check("trees on the ground (5 cm)", off < 0.05, off)
	# Same seed, same field; another seed, another field.
	var first := trees.duplicate()
	field.populate()
	_check("deterministic", field.get_placements(0) == first, field.get_placements(0).size())
	field.seed = 7
	field.populate()
	_check("seed changes the field", field.get_placements(0) != first, field.get_placements(0).size())
	field.seed = 0
	field.populate()
	# Geology: trees need 0.5 m of soil and little bare rock; nothing else changes without it.
	var geo := Geology.new()
	geo.name = "Geology"
	add_child(geo)
	dm.geology_source = dm.get_path_to(geo)
	dm.bake()
	var tree_sp: PlantSpecies = field.biome.species[0]
	tree_sp.set_rule_range(PlantSpecies.RULE_SOIL_DEPTH, Vector2(0.5, 100000))
	tree_sp.set_rule_fade(PlantSpecies.RULE_SOIL_DEPTH, 0.1)
	tree_sp.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0.0, 0.3))
	tree_sp.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	field.populate()
	var on_rock := 0
	var west := 0
	for p in field.get_placements(0):
		if p.x > 40.0:
			on_rock += 1
		elif p.x > 0.0:
			west += 1
	_check("geology: map has it", dm.has_geology() and is_equal_approx(dm.get_rock_at(Vector3(100, 0, 0)), 1.0), dm.get_rock_at(Vector3(100, 0, 0)))
	_check("geology: no tree on thin soil or bare rock", on_rock == 0 and west > 10, [on_rock, west])
	var wet := 0
	for s in 2:
		for p in field.get_placements(s):
			if dm.get_spray_at(p) > 0.65:
				wet += 1
	_check("spray: no tree or bush in a fall's wet zone", wet == 0 and dm.get_spray_at(Vector3(-40, 0, -40)) > 0.95 and field.get_placements(0).size() > 50, [wet, dm.get_spray_at(Vector3(-40, 0, -40)), field.get_placements(0).size()])
	dm.geology_source = NodePath()
	dm.bake()
	field.populate()
	_check("geology: rules ignored without a source", field.get_placements(0) == first, field.get_placements(0).size())
	# Holes (a cave mouth's opening, TerrainGenerator3D.add_hole()): no plant in one. The generator
	# is found as the map's geology source; it needs no terrain of its own for its boxes.
	var hgen := TerrainGenerator3D.new()
	# Heavy generator options are on by default; this bench keeps its own fast choices.
	hgen.glacial_enabled = false
	hgen.glacial_converged = false
	hgen.tile_erosion_enabled = false
	hgen.strata_enabled = false
	hgen.guide_preview = false
	hgen.dunes_enabled = false
	hgen.deposition = 0.0
	hgen.moraine_age = 0.0
	hgen.scree_age = 0.0
	hgen.meander_age = 0.0
	hgen.outwash_age = 0.0
	hgen.name = "HoleGen"
	add_child(hgen)
	dm.geology_source = dm.get_path_to(hgen)
	var hole := Transform3D(Basis.from_scale(Vector3(10, 30, 10)), Vector3(0, height(0, 0), 0))
	var in_hole := func() -> int:
		var n := 0
		for s in field.biome.species.size():
			for p in field.get_placements(s):
				if absf(p.x) <= 10.0 and absf(p.z) <= 10.0:
					n += 1
		return n
	var before_hole: int = in_hole.call()
	hgen.add_hole(hole)
	field.populate()
	var after_hole: int = in_hole.call()
	hgen.clear_holes()
	field.populate()
	_check("holes: no plant in a cave mouth's opening, back when it closes", before_hole > 0 and after_hole == 0 and in_hole.call() == before_hole, [before_hole, after_hole, in_hole.call()])
	dm.geology_source = NodePath()
	hgen.queue_free()
	# Canopy shade: trees cast it, bushes that want shade grow only under crowns.
	var bush_sp: PlantSpecies = field.biome.species[1]
	var bushes_open := field.get_placements(1).size()
	tree_sp.canopy_radius = 3.0
	tree_sp.canopy_opacity = 0.9
	bush_sp.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.5, 1.0))
	field.populate()
	var t0: Vector3 = field.get_placements(0)[0]
	var sunny := 0
	for p in field.get_placements(1):
		if field.get_shade_at(p) < 0.5:
			sunny += 1
	var under_t: float = field.get_shade_at(t0)
	var open_t: float = field.get_shade_at(Vector3(t0.x, 0, t0.z) + Vector3(40, 0, 40))
	_check("shade: core of a crown", under_t >= 0.89, under_t)
	_check("shade: bushes only under crowns", sunny == 0 and field.get_placements(1).size() > 20 and field.get_placements(1).size() < bushes_open, [sunny, field.get_placements(1).size(), bushes_open])
	_check("shade: trees unchanged", field.get_placements(0) == first, field.get_placements(0).size())
	# Crowns near a point (falling leaves): the tree at t0 is listed with its crown.
	var near_c: Array = field.get_crowns_near(t0, 1.0)
	var found := false
	for d in near_c:
		if Vector2(d.position.x, d.position.z).distance_to(Vector2(t0.x, t0.z)) < 0.01 and d.crown_radius > 0.0 and d.height > 0.0 and d.crown_base < d.height:
			found = true
	_check("crowns near: the tree over a point, with its crown", found and field.get_crowns_near(t0 + Vector3(500, 0, 500), 1.0).is_empty(), [near_c.size()])
	# Falling leaves take their crowns from the field (LeafFall3D.auto_crowns).
	if ClassDB.class_exists("LeafFall3D"):
		tree_sp.set_meta("preset", TreeGenerator.PRESET_BEECH)
		var lf: Node3D = ClassDB.instantiate("LeafFall3D")
		add_child(lf)
		lf.vegetation_field = lf.get_path_to(field)
		lf.refresh_crowns_near(t0)
		var lf_crowns: int = lf.get_crown_count()
		var lf_share: float = lf.get_preset_shed_daily(TreeGenerator.PRESET_BEECH)
		_check("leaf fall: crowns from the field, with the beech's daily share", lf_crowns > 0 and lf.get_kind_count() >= 1 and lf_share >= 0.0, [lf_crowns, lf.get_kind_count(), lf_share])
		lf.queue_free()
		tree_sp.remove_meta("preset")
	# Light: each ranged plant carries the light it grew in (VegetationLight), which tints it.
	bush_sp.visibility_range = 30.0
	field.populate()
	var lit_n := 0
	var lit_bad := 0
	for ch in field.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh.mesh == bush_sp.meshes[0]:
			for i in ch.multimesh.instance_count:
				var y: float = ch.multimesh.get_instance_custom_data(i).g
				var light := (y - floorf(y)) * 101.0 / 100.0 * 1.5
				var shade := field.get_shade_at(ch.global_transform * ch.multimesh.get_instance_transform(i).origin)
				lit_n += 1
				if floorf(y) != 30.0 or absf(light - (1.0 - shade)) > 0.03:
					lit_bad += 1
	_check("light: shade plants carry the light under the crowns", lit_n > 20 and lit_bad == 0, [lit_n, lit_bad])
	bush_sp.visibility_range = 0.0
	# canopy_field: a second field without trees (a camera-local herb layer) reads the first's crowns.
	var herbs := VegetationField3D.new()
	herbs.far_volume = false # Not under test here.
	herbs.name = "Herbs"
	herbs.populate_on_ready = false
	var hb := Biome.new()
	hb.species = [bush_sp]
	herbs.biome = hb
	add_child(herbs)
	herbs.data_map = herbs.get_path_to(dm)
	herbs.populate()
	var open_n := herbs.get_placements(0).size()
	herbs.canopy_field = herbs.get_path_to(field)
	herbs.populate()
	var herb_sunny := 0
	for p in herbs.get_placements(0):
		if field.get_shade_at(p) < 0.5:
			herb_sunny += 1
	_check("canopy_field: a field without trees grows its shade plants under another's crowns", open_n == 0 and herb_sunny == 0 and herbs.get_placements(0).size() > 20, [open_n, herb_sunny, herbs.get_placements(0).size()])
	herbs.queue_free()
	# The same canopy reaches WeatherMap3D (canopy_map), so snow and wind see the same forest.
	var wm := WeatherMap3D.new()
	wm.name = "Weather"
	add_child(wm)
	field.weather_map = field.get_path_to(wm)
	var follow := Node3D.new()
	add_child(follow)
	follow.global_position = t0
	wm.follow_target = wm.get_path_to(follow)
	wm.shelter_extent = 128.0 # 0.5 m cells: the comparison is then limited by the 1 m canopy map.
	field.populate()
	wm.rebake_shelter()
	var worst := 0.0
	for q in [t0, t0 + Vector3(2, 0, 0), t0 + Vector3(0, 0, -3), t0 + Vector3(-4, 0, 4)]:
		worst = max(worst, abs(wm.get_canopy_closure_at(q) - field.get_shade_at(q)))
	_check("canopy map: weather closure matches shade (1 m cells)", wm.get_canopy_map() != null and worst < 0.12, [worst, wm.get_canopy_closure_at(t0), field.get_shade_at(t0), wm.get_canopy_height_at(t0)])
	field.weather_map = NodePath()
	wm.queue_free()
	follow.queue_free()
	tree_sp.canopy_radius = 0.0
	bush_sp.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 1.0))
	field.populate()
	# Climate (TerrainDataMap3D.bake_climate): a warmth rule keeps trees to the warmer half.
	dm.climate_recipe = ClimateRecipe.new()
	dm.bake_climate()
	var temps: Array = []
	for p in first:
		temps.append(dm.get_temperature_at(p))
	temps.sort()
	var t_mid: float = temps[temps.size() / 2]
	tree_sp.set_rule_range(PlantSpecies.RULE_TEMPERATURE, Vector2(t_mid, 100.0))
	field.populate()
	var cold := 0
	for p in field.get_placements(0):
		if dm.get_temperature_at(p) < t_mid - 1e-4:
			cold += 1
	_check("climate: trees only where warm enough", dm.has_climate() and cold == 0 and field.get_placements(0).size() < first.size() and temps[-1] > temps[0], [cold, field.get_placements(0).size(), temps[0], t_mid, temps[-1]])
	tree_sp.set_rule_range(PlantSpecies.RULE_TEMPERATURE, Vector2(-100.0, 100.0))
	field.populate()
	_check("shade: no canopy, no shade", field.get_placements(1).size() == bushes_open and field.get_shade_at(t0) == 0.0, field.get_placements(1).size())
	# Density mask: a clearing painted into the red channel keeps the trees out of it; the bushes,
	# reading the inverted channel, grow only there.
	var all_trees := field.get_placements(0).size()
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	img.fill(Color(1, 1, 1, 1))
	var rect: Rect2 = dm.get_map_rect()
	var centre := rect.get_center()
	var radius := rect.size.x * 0.25
	for y in 64:
		for x in 64:
			var w := rect.position + Vector2((x + 0.5) / 64.0 * rect.size.x, (y + 0.5) / 64.0 * rect.size.y)
			if w.distance_to(centre) < radius:
				img.set_pixel(x, y, Color(0, 1, 1, 1))
	field.density_mask = ImageTexture.create_from_image(img)
	field.density_mask_rect = rect
	tree_sp.mask_channel = 0
	var mask_bush: PlantSpecies = field.biome.species[1]
	mask_bush.mask_channel = 0
	mask_bush.mask_invert = true
	field.populate()
	var in_clearing := 0
	for t in field.get_placements(0):
		if Vector2(t.x, t.z).distance_to(centre) < radius - rect.size.x / 64.0:
			in_clearing += 1
	var bush_out := 0
	for b in field.get_placements(1):
		if Vector2(b.x, b.z).distance_to(centre) > radius + rect.size.x / 64.0:
			bush_out += 1
	_check("density mask: no tree in the clearing, bushes only inside it", in_clearing == 0 and bush_out == 0 and field.get_placements(0).size() < all_trees and field.get_placements(1).size() > 0, [in_clearing, bush_out, field.get_placements(0).size(), all_trees])
	tree_sp.mask_channel = -1
	mask_bush.mask_channel = -1
	field.density_mask = null
	# Near swap: a heavier mesh within near_range, the same plants in the same places.
	var near_box := BoxMesh.new()
	tree_sp.near_meshes = [near_box]
	tree_sp.near_range = 30.0
	field.populate()
	var near_n := 0
	var far_n := 0
	var bad_range := 0
	var near_pos := {}
	var far_pos := {}
	for ch in field.get_children(true):
		if not ch is MultiMeshInstance3D or ch.multimesh.mesh == null:
			continue
		var mm: MultiMesh = ch.multimesh
		var is_near: bool = mm.mesh == near_box
		var is_tree: bool = is_near or tree_sp.meshes.has(mm.mesh)
		if not is_tree:
			continue
		if is_near:
			near_n += mm.instance_count
			if not is_equal_approx(ch.visibility_range_end, 30.0) or ch.visibility_range_begin != 0.0:
				bad_range += 1
		else:
			far_n += mm.instance_count
			if not is_equal_approx(ch.visibility_range_begin, 30.0):
				bad_range += 1
		for i in mm.instance_count:
			var key := str(mm.get_instance_transform(i).origin.snapped(Vector3.ONE * 0.01))
			if is_near:
				near_pos[key] = true
			else:
				far_pos[key] = true
	var unmatched := 0
	for k in near_pos:
		if not far_pos.has(k):
			unmatched += 1
	_check("near swap: every tree drawn near and far, split by range", near_n == far_n and near_n == field.get_placements(0).size() and unmatched == 0 and bad_range == 0 and field.get_placements(0) == first, [near_n, far_n, field.get_placements(0).size(), unmatched, bad_range])
	tree_sp.near_meshes = []
	tree_sp.near_range = 0.0
	# Cover map for distant terrain: the expected crown cover from the rules matches the placed one.
	tree_sp.canopy_radius = 3.0
	field.cover_map_enabled = true
	field.populate()
	var cimg: Image = field.get_cover_texture().get_image()
	var expect := 0.0
	var placed := 0.0
	var cn := 0
	for cz in range(2, cimg.get_height() - 2):
		for cx in range(2, cimg.get_width() - 2):
			var cw := Vector3(-SIZE * 0.5 + (cx + 0.5) * 4.0, 0, -SIZE * 0.5 + (cz + 0.5) * 4.0)
			expect += cimg.get_pixel(cx, cz).a
			var sh := 0.0
			for o in [Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(-1, 0, 1), Vector3(1, 0, 1)]:
				sh += field.get_shade_at(cw + o) * 0.25
			placed += sh
			cn += 1
	var water_a := 0.0
	for cz in cimg.get_height():
		for cx in cimg.get_width():
			if dm.get_water_depth_at(Vector3(-SIZE * 0.5 + (cx + 0.5) * 4.0, 0, -SIZE * 0.5 + (cz + 0.5) * 4.0)) > 0.3:
				water_a = maxf(water_a, cimg.get_pixel(cx, cz).a)
	_check("cover map: expected crown cover matches the placed crowns (20%), none on the lake", cn > 50 and absf(expect - placed) < 0.2 * placed and water_a < 0.05, [expect / cn, placed / cn, water_a, cimg.get_size()])
	# Grass layers: a tall sward in patches reads part bare from afar unless the undercoat that fills
	# its gaps is a layer too (cover_grass_layers).
	tree_sp.canopy_radius = 0.0
	var tall_g := GrassField3D.new()
	var tall_r := PlantSpecies.new()
	tall_r.patch_scale = 9.0
	tall_r.patch_cover = 0.55
	tall_g.rules = tall_r
	var under_g := GrassField3D.new()
	under_g.rules = PlantSpecies.new()
	add_child(tall_g)
	add_child(under_g)
	field.cover_grass = field.get_path_to(tall_g)
	field.populate()
	var one: Array = _alpha_stats(field.get_cover_texture().get_image())
	field.cover_grass_layers = [field.get_path_to(under_g)]
	field.populate()
	var two: Array = _alpha_stats(field.get_cover_texture().get_image())
	_check("cover map grass layers: the undercoat fills the tall grass's gaps (more cover, less patchy)", two[0] > one[0] + 0.1 and two[1] < one[1] * 0.6, [one, two])
	field.cover_grass = NodePath()
	field.cover_grass_layers = []
	tall_g.queue_free()
	under_g.queue_free()
	field.cover_map_enabled = false
	# Impostors: the species' meshes near, their baked impostors (two triangles each) past the range.
	var real_meshes: Array = tree_sp.meshes.duplicate()
	PlantGenerator.add_impostors(tree_sp, 30.0, 4, 16)
	field.populate()
	var imp_n := 0
	var mesh_n := 0
	var bad_imp := 0
	for ch in field.get_children(true):
		if not ch is MultiMeshInstance3D or ch.multimesh.mesh == null:
			continue
		var m: Mesh = ch.multimesh.mesh
		if m.has_meta("impostor_of"):
			imp_n += ch.multimesh.instance_count
			if not real_meshes.has(m.get_meta("impostor_of")) or m.get_faces().size() != 6 or not is_equal_approx(ch.visibility_range_begin, 30.0):
				bad_imp += 1
		elif real_meshes.has(m):
			mesh_n += ch.multimesh.instance_count
	_check("impostors: each tree drawn as its mesh near and its impostor far", imp_n == mesh_n and imp_n == field.get_placements(0).size() and bad_imp == 0, [imp_n, mesh_n, field.get_placements(0).size(), bad_imp])
	tree_sp.meshes = real_meshes
	tree_sp.near_meshes = []
	tree_sp.near_range = 0.0

# Streaming: tiles placed on workers around a moving centre come out exactly as populate() placed
# them (shade bushes under the trees' crowns, so each tile's halo matters), and go past the radius.
func _stream_wait(f: VegetationField3D) -> int:
	for i in 900:
		await get_tree().process_frame
		if f.is_stream_idle():
			return i
	return -1

func _streamed_set(full: PackedVector3Array, centre: Vector2, radius: float, ts: float) -> Dictionary:
	var want := {}
	for p in full:
		var lo := Vector2(floorf(p.x / ts), floorf(p.z / ts)) * ts
		var d := Vector2(maxf(maxf(lo.x - centre.x, centre.x - lo.x - ts), 0.0), maxf(maxf(lo.y - centre.y, centre.y - lo.y - ts), 0.0)).length()
		if d <= radius:
			want[p] = true
	return want

# Tiles stay a tile and a half past the radius before they go (hysteresis): only compare within it.
func _same(got: PackedVector3Array, want: Dictionary, centre: Vector2, radius: float, ts: float) -> bool:
	var inside := _streamed_set(got, centre, radius, ts)
	if inside.size() != want.size():
		return false
	for p in inside:
		if not want.has(p):
			return false
	return true

# Near meshes on demand (PlantSpecies.near_mesh_source): a source that is still "building" for its
# first calls, then hands out one mesh; near cells appear only round the target and go when it leaves.
class NearSource extends RefCounted:
	var mesh: Mesh
	var calls := 0
	var keys := {}
	func request_near_mesh(key: int, variant: int) -> Mesh:
		calls += 1
		keys[key] = true
		return mesh if calls > 3 else null

# Plants and fallen wood (docs/ideas/forest-floor.md, 6.6): logs as the trees module marks them
# (species meta "deadwood", mesh meta tree_deadwood_axis), in a canopy field; a ground cover that
# must keep off them, moss on them and ferns along them in a second field.
func _deadwood_checks() -> void:
	var r := 0.3
	var log_mesh := CylinderMesh.new()
	log_mesh.top_radius = r
	log_mesh.bottom_radius = r
	log_mesh.height = 6.0
	var m := _mesh(log_mesh, 0.0, Color(0.3, 0.22, 0.15))
	var st := SurfaceTool.new()
	st.append_from(m, 0, Transform3D(Basis(Vector3(0, 0, 1), PI / 2), Vector3(0, r, 0)))
	var lm := st.commit()
	# The trees module's layout: a, b, widest reach, then N reaches along the stem to the outside
	# of its moss (2.2 cm over the bark, VegetationField3D's DEADWOOD_MOSS_LIFT).
	var lr := r + 0.022
	lm.set_meta("tree_deadwood_axis", PackedFloat32Array([-3.0, r, 0.0, 3.0, r, 0.0, lr, 4, lr, lr, lr, lr]))
	# An uprooted tree's root pit past the foot end (tree_root_pit: centre, radius, depth in mesh space).
	lm.set_meta("tree_root_pit", PackedFloat32Array([-4.0, 0.0, 0.0, 0.9, 0.5]))
	var logs := PlantSpecies.new()
	logs.meshes = [lm]
	logs.spacing = 12.0
	logs.footprint = 0.0
	logs.scale_range = Vector2(1, 1)
	logs.set_meta("deadwood", true)
	# Stand-in trunks (a 25 cm stem from just under the soil): the ground plants keep out of their feet.
	var trunk_mesh := CylinderMesh.new()
	trunk_mesh.top_radius = 0.25
	trunk_mesh.bottom_radius = 0.25
	trunk_mesh.height = 3.0
	var trunks := PlantSpecies.new()
	trunks.meshes = [_mesh(trunk_mesh, 1.4, Color(0.3, 0.25, 0.2))]
	trunks.spacing = 6.0
	trunks.footprint = 1.0
	trunks.scale_range = Vector2(1, 1)
	trunks.canopy_radius = 3.0
	trunks.canopy_opacity = 0.5
	var wood := VegetationField3D.new()
	wood.far_volume = false # Not under test here.
	wood.name = "Wood"
	wood.populate_on_ready = false
	var wb := Biome.new()
	wb.species = [logs, trunks]
	wood.biome = wb
	add_child(wood)
	wood.data_map = wood.get_path_to(dm)
	wood.populate()
	var small := BoxMesh.new()
	small.size = Vector3(0.05, 0.05, 0.05)
	var sm := _mesh(small, 0.025, Color(0.2, 0.4, 0.1))
	var cover := PlantSpecies.new()
	cover.meshes = [sm]
	cover.spacing = 0.25
	cover.footprint = 0.0
	var moss := PlantSpecies.new()
	moss.meshes = [sm]
	moss.spacing = 0.2
	moss.footprint = 0.0
	moss.deadwood_rule = PlantSpecies.DEADWOOD_ON
	moss.deadwood_min_radius = 0.1
	moss.align_to_ground = 1.0
	var fern := PlantSpecies.new()
	fern.meshes = [sm]
	fern.spacing = 0.3
	fern.footprint = 0.0
	fern.deadwood_rule = PlantSpecies.DEADWOOD_BESIDE
	fern.deadwood_band = 0.6
	var ground := VegetationField3D.new()
	ground.far_volume = false # Not under test here.
	ground.name = "DeadwoodGround"
	ground.populate_on_ready = false
	var gb := Biome.new()
	# Honey fungus as make_floor() sets it up (beside logs at least 12 cm thick), on the test's mesh.
	var honey: PlantSpecies = null
	for sp in PlantGenerator.make_floor(PlantGenerator.FLOOR_BEECH, 1, false):
		if sp.has_meta("mushroom") and sp.deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
			honey = sp.duplicate()
	honey.meshes = [sm]
	honey.density = 0.8
	gb.species = [cover, moss, fern, honey]
	ground.biome = gb
	add_child(ground)
	ground.data_map = ground.get_path_to(dm)
	ground.canopy_field = ground.get_path_to(wood)
	ground.populate()
	var nlogs: int = wood.get_deadwood_count()
	var under := 0
	for p in ground.get_placements(0):
		var h: Dictionary = wood.get_deadwood_at(p)
		if not h.is_empty() and h["side"] < -0.01:
			under += 1
	var on_ok := 0
	var on_bad := 0
	for p in ground.get_placements(1):
		var h: Dictionary = wood.get_deadwood_at(p)
		if not h.is_empty() and p.y - h["top"] > 0.01 and p.y - h["top"] < 0.045 and p.y > dm.get_height_at(p) + 0.05: # Above the ground (a level log on a slope sinks at one end).
			on_ok += 1
		else:
			on_bad += 1
	var in_stem := 0
	var near_stem := 0
	var stems: Array = wood.get_placements(1)
	for p in ground.get_placements(0):
		for t in stems:
			var d := Vector2(p.x - t.x, p.z - t.z).length()
			if d < 0.25:
				in_stem += 1
			elif d < 0.6:
				near_stem += 1
	var honey_by := 0
	for p in ground.get_placements(3):
		# Out of the wood's side, its foot 1.5 cm into the bark (PlantSpecies.deadwood_from_wood).
		var h: Dictionary = wood.get_deadwood_at(p, 0.85, 0.12)
		# Up the side to a crack about 20 cm over the litter (or the log's widest girth).
		if not h.is_empty() and h["side"] >= -0.12 and h["side"] < 0.01 and p.y > height(p.x, p.z) + 0.05:
			honey_by += 1
	_check("deadwood: honey fungus clumps come up from the logs' side, above the litter", ground.get_placements(3).size() > 0 and honey_by == ground.get_placements(3).size(), [ground.get_placements(3).size(), honey_by, nlogs])
	# Root pits stay bare soil: no ground cover inside one, and it grows round it.
	var pits := []
	for mmi in wood.find_children("*", "MultiMeshInstance3D", true, false):
		var mm: MultiMesh = mmi.multimesh
		if mm == null or mm.mesh == null or not mm.mesh.has_meta("tree_root_pit"):
			continue
		var pm: PackedFloat32Array = mm.mesh.get_meta("tree_root_pit")
		for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
			var xf: Transform3D = mmi.global_transform * mm.get_instance_transform(i)
			pits.append(xf * Vector3(pm[0], pm[1], pm[2]))
	var in_pit := 0
	var by_pit := 0
	for p in ground.get_placements(0):
		for c in pits:
			var d := Vector2(p.x - c.x, p.z - c.z).length()
			if d < 0.88:
				in_pit += 1
			elif d < 1.5:
				by_pit += 1
	_check("deadwood: root pits of uprooted trees stay bare, cover grows round them", pits.size() > 0 and in_pit == 0 and by_pit > 5, [pits.size(), in_pit, by_pit])
	_check("deadwood: ground plants keep out of the canopy field's trunks, and grow up to them", stems.size() > 3 and in_stem == 0 and near_stem > 0, [stems.size(), in_stem, near_stem])
	# Rocks (VegetationField3D.obstacles): boulders where cover grew; none of it inside them after,
	# and it grows up to their foot.
	var covers: Array = ground.get_placements(0)
	var rocks := PackedVector4Array()
	for k in mini(6, covers.size()):
		var q: Vector3 = covers[k * covers.size() / 6]
		rocks.append(Vector4(q.x, q.y, q.z, 0.4 + 0.3 * k))
	ground.obstacles = rocks
	ground.populate()
	var in_rock := 0
	var by_rock := 0
	for si in 3:
		for p in ground.get_placements(si):
			for rk in rocks:
				var d := Vector2(p.x - rk.x, p.z - rk.z).length() - rk.w
				if d < 0.029: # VegetationField3D's margin is 3 cm.
					in_rock += 1
				elif d < 0.5:
					by_rock += 1
	_check("rocks: no ground plant inside a boulder, and they grow up to it", rocks.size() == 6 and in_rock == 0 and by_rock > 5, [rocks.size(), in_rock, by_rock])
	# Leaves do not reach in over a rock past 8 cm (VegetationField3D.ROCK_OVERHANG): plants at a
	# foot are made smaller and lean away instead. The ferns as wide plants (60 cm reach) for it.
	var wide := BoxMesh.new()
	wide.size = Vector3(1.2, 0.3, 1.2)
	fern.meshes = [_mesh(wide, 0.15, Color(0.2, 0.4, 0.1))]
	ground.populate()
	var reach_in := 0
	var fitted := 0
	for ch in ground.get_children(true):
		if not (ch is MultiMeshInstance3D) or ch.multimesh == null or ch.multimesh.mesh == null:
			continue
		var bb: AABB = ch.multimesh.mesh.get_aabb()
		var spread := maxf(maxf(absf(bb.position.x), absf(bb.end.x)), maxf(absf(bb.position.z), absf(bb.end.z)))
		for ii in ch.multimesh.instance_count:
			var t: Transform3D = ch.global_transform * ch.multimesh.get_instance_transform(ii)
			var c := 1e9
			for rk in rocks:
				c = minf(c, Vector2(t.origin.x - rk.x, t.origin.z - rk.z).length() - rk.w)
			var reach := spread * t.basis.x.length()
			if c < reach:
				fitted += 1
			if c + 0.08 < reach * 0.98:
				reach_in += 1
	_check("rocks: leaves reach no farther than 8 cm in over a rock", reach_in == 0 and fitted > 0, [reach_in, fitted])
	fern.meshes = [sm]
	# Rock foot (PlantSpecies.rock_foot): a thin cover gathers at the rocks' feet.
	cover.density = 0.2
	cover.rock_foot = 3.0
	ground.populate()
	var foot_n := 0
	var ring_n := 0
	var foot_a := 0.0
	var ring_a := 0.0
	for rk in rocks:
		foot_a += PI * (pow(rk.w + 0.4, 2.0) - pow(rk.w + 0.03, 2.0))
		ring_a += PI * (pow(rk.w + 3.0, 2.0) - pow(rk.w + 2.0, 2.0))
	for p in ground.get_placements(0):
		var best := 1e9
		for rk in rocks:
			best = minf(best, Vector2(p.x - rk.x, p.z - rk.z).length() - rk.w)
		if best < 0.4:
			foot_n += 1
		elif best >= 2.0 and best < 3.0:
			ring_n += 1
	var foot_d := foot_n / maxf(foot_a, 0.01)
	var ring_d := ring_n / maxf(ring_a, 0.01)
	_check("rock foot: a thin cover gathers at the rocks' feet (rock_foot 3)", ring_n > 10 and foot_d > ring_d * 2.0, "%.1f /m2 at the foot, %.1f /m2 2-3 m out" % [foot_d, ring_d])
	cover.density = 1.0
	cover.rock_foot = 0.0
	# Worn paths (VegetationField3D.paths, PlantSpecies.trample_resistance): a busy path across the
	# cover leaves its trodden line nearly bare and the band beside it thinned; a resistant
	# species keeps more of its cover there than a fragile one.
	ground.obstacles = PackedVector4Array()
	ground.populate()
	var all_cov: Array = ground.get_placements(0)
	var mid := Vector3()
	for q in all_cov:
		mid += q
	mid /= maxf(all_cov.size(), 1)
	var path_line := PackedVector3Array([mid - Vector3(30, 0, 0), mid + Vector3(30, 0, 0)])
	ground.paths = [{"points": path_line, "width": 2.0, "passes": 2000.0}]
	var path_bands := func() -> Array:
		var on := 0
		var off := 0
		for q in ground.get_placements(0):
			var dz := absf(q.z - mid.z)
			if absf(q.x - mid.x) < 25.0:
				if dz < 0.3:
					on += 1
				elif dz > 3.0 and dz < 3.6:
					off += 1
		return [on, off]
	cover.trample_resistance = 100.0
	ground.populate()
	var fragile: Array = path_bands.call()
	cover.trample_resistance = 3000.0
	ground.populate()
	var tough: Array = path_bands.call()
	cover.trample_resistance = 0.0
	ground.populate()
	var untrodden: Array = path_bands.call()
	ground.paths = []
	_check("worn path: the trodden line nearly bare for a fragile species, a resistant one keeps more, none without resistance", fragile[0] * 10 < fragile[1] and tough[0] > fragile[0] * 3 and untrodden[0] * 2 > untrodden[1], [fragile, tough, untrodden])
	# Plants between stones (PlantSpecies.stone_gap): a 2 m patch of packed scree laid by random
	# sequential addition; with stone_gap 1 the cover grows only in its gaps, none on open ground,
	# and none at all without stones.
	var c0: Vector3 = covers[0]
	var scree := PackedVector4Array()
	var srng := RandomNumberGenerator.new()
	srng.seed = 77
	for attempt in 4000:
		var rr := srng.randf_range(0.08, 0.22)
		var pos := Vector2(c0.x, c0.z) + Vector2(srng.randf_range(-1.0, 1.0), srng.randf_range(-1.0, 1.0))
		var free := true
		for sv in scree:
			if Vector2(pos.x - sv.x, pos.y - sv.z).length() < rr + sv.w + 0.03:
				free = false
				break
		if free:
			scree.append(Vector4(pos.x, c0.y, pos.y, rr))
	ground.obstacles = scree
	cover.stone_gap = 1.0
	ground.populate()
	var in_gap := 0
	var open_ground := 0
	for p in ground.get_placements(0):
		var ds := []
		for sv in scree:
			ds.append(Vector2(p.x - sv.x, p.z - sv.z).length() - sv.w)
		ds.sort()
		if ds.size() > 1 and ds[1] < 0.3:
			in_gap += 1
		elif ds.size() > 1 and ds[1] > 0.6:
			open_ground += 1
	ground.obstacles = PackedVector4Array()
	ground.populate()
	var bare := ground.get_placements(0).size()
	_check("stone gap: crevice plants only between stones, none on open ground or without stones", scree.size() > 30 and in_gap >= 5 and open_ground == 0 and bare == 0, [scree.size(), in_gap, open_ground, bare])
	cover.stone_gap = 0.0
	# Stone tops (PlantSpecies.stone_top): only on the caps, set on them above the ground.
	var caps := PackedVector4Array()
	for sv in scree:
		caps.append(Vector4(sv.x, sv.y + 0.15, sv.z, sv.w * 0.8))
	ground.obstacles = scree
	ground.stone_tops = caps
	cover.stone_top = true
	ground.populate()
	var on_cap := 0
	var off_cap := 0
	for p in ground.get_placements(0):
		var ok_cap := false
		for cp in caps:
			if Vector2(p.x - cp.x, p.z - cp.z).length() < cp.w and p.y > cp.y - cp.w and p.y < cp.y:
				ok_cap = true
		if ok_cap:
			on_cap += 1
		else:
			off_cap += 1
	_check("stone tops: plants only on the stones' caps, set down on them", on_cap > 3 and off_cap == 0, [on_cap, off_cap])
	# Stone tops by their height over the water (PlantSpecies.stone_top_water): a row of stones
	# across the river's edge; aquatic moss only on those under or just over the surface.
	var wcaps := PackedVector4Array()
	for i in 24:
		var wx := -2.0 + i * 0.5
		var wz := -5.0
		wcaps.append(Vector4(wx, dm.get_height_at(Vector3(wx, 0, wz)) + 0.12, wz, 0.15))
	ground.obstacles = PackedVector4Array()
	ground.stone_tops = wcaps
	cover.stone_top_water = Vector2(-0.6, 0.1)
	cover.water_depth_range = Vector2(0, 3) # The bed under the stone is under water.
	ground.populate()
	var wet_on := 0
	var dry_on := 0
	var wet_caps := 0
	for cp in wcaps:
		var q := Vector3(cp.x, 0, cp.z)
		var dep := dm.get_water_depth_at(q)
		var over: float = cp.y - (dm.get_height_at(q) + dep) if dep > 0.0 else cp.y - dm.get_height_at(q) + 0.25 * maxf(dm.get_water_distance_at(q), 0.0)
		var k := 0
		for p in ground.get_placements(0):
			if Vector2(p.x - cp.x, p.z - cp.z).length() < cp.w + 0.02:
				k += 1
		if over < 0.1:
			wet_caps += 1
			wet_on += k
		elif over > 0.14:
			dry_on += k
	_check("stone tops by water: aquatic moss on stones under or just over the river, none up the bank", wet_caps > 3 and wet_on > 3 and dry_on == 0, [wet_caps, wet_on, dry_on])
	cover.stone_top_water = Vector2()
	cover.water_depth_range = Vector2()
	cover.stone_top = false
	ground.stone_tops = PackedVector4Array()
	ground.obstacles = PackedVector4Array()
	ground.populate()
	# Stand margins (PlantSpecies.margin_*): the patch depth is positive inside a patch, negative
	# outside, and about zero where the patch weight crosses one half.
	var pm := PlantSpecies.new()
	pm.patch_scale = 9.0
	pm.patch_cover = 0.3
	pm.patch_seed = 5
	var d_in := 0
	var d_out := 0
	var d_bad := 0
	var d_edge := 0.0
	var n_edge := 0
	seed(4)
	for i in 3000:
		var q := Vector3(randf_range(-60.0, 60.0), 0, randf_range(-60.0, 60.0))
		var w := pm.get_patch_weight(q)
		var dd := pm.get_patch_depth(q)
		if w > 0.99:
			d_in += 1
			if dd <= 0.0: d_bad += 1
		elif w < 0.01:
			d_out += 1
			if dd >= 0.0: d_bad += 1
		elif absf(w - 0.5) < 0.1:
			d_edge += absf(dd)
			n_edge += 1
	d_edge /= maxi(n_edge, 1)
	_check("stand margins: patch depth inside > 0, outside < 0, about 0 at the edge", d_in > 100 and d_out > 100 and d_bad == 0 and n_edge > 10 and d_edge < 0.3, [d_in, d_out, d_bad, n_edge, d_edge])
	var beside_ok := 0
	var beside_bad := 0
	for p in ground.get_placements(2):
		var h: Dictionary = wood.get_deadwood_at(p, 0.6)
		if not h.is_empty() and h["side"] >= -0.001 and h["side"] <= 0.6:
			beside_ok += 1
		else:
			beside_bad += 1
	print("VEG_DEADWOOD logs=%d cover_under_logs=%d on=%d/%d beside=%d/%d" % [nlogs, under, on_ok, on_bad, beside_ok, beside_bad])
	_check("deadwood: logs placed with their axes", nlogs > 10, nlogs)
	_check("deadwood: ground cover keeps off the logs", under == 0 and ground.get_placements(0).size() > 1000, under)
	# On the log's moss, 2.2 cm proud of the bark (VegetationField3D.DEADWOOD_MOSS_LIFT, more where it slopes).
	_check("deadwood: moss only on the logs' upper surface", on_ok > 100 and on_bad == 0, [on_ok, on_bad])
	_check("deadwood: ferns only along the logs", beside_ok > 20 and beside_bad == 0, [beside_ok, beside_bad])
	ground.queue_free()
	wood.queue_free()

func _lazy_near_count(f: VegetationField3D, m: Mesh) -> Array:
	var near := 0
	var cells := 0
	var far_begin := 0
	for ch in f.get_children(true):
		if not ch is MultiMeshInstance3D or ch.multimesh.mesh == null:
			continue
		if ch.multimesh.mesh == m:
			near += ch.multimesh.instance_count
			cells += 1
		elif ch.visibility_range_begin > 0.0:
			far_begin += 1
	return [near, cells, far_begin]

func _lazy_near_checks() -> void:
	var tree_sp: PlantSpecies = field.biome.species[0]
	var src := NearSource.new()
	src.mesh = BoxMesh.new()
	tree_sp.near_meshes = []
	tree_sp.near_range = 30.0
	tree_sp.near_mesh_source = src
	tree_sp.near_mesh_key = 7
	var target := Node3D.new()
	add_child(target)
	target.global_position = Vector3(5000, 0, 5000)
	field.stream_target = field.get_path_to(target)
	field.populate()
	for i in 3:
		await get_tree().process_frame
	var away: Array = _lazy_near_count(field, src.mesh)
	var t: Vector3 = field.get_placements(0)[0]
	target.global_position = t
	for i in 8:
		await get_tree().process_frame
	var near: Array = _lazy_near_count(field, src.mesh)
	var want := 0
	for p in field.get_placements(0):
		if Vector2(p.x, p.z).distance_to(Vector2(t.x, t.z)) < 20.0:
			want += 1
	# Path traced, the crossfade margins go (the tracer would see both cells inside them), and come back.
	var margins := func() -> Array:
		var on := 0
		var off := 0
		for ch in field.get_children(true):
			if ch is MultiMeshInstance3D and ch.visibility_range_fade_mode == GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF:
				var m: float = ch.visibility_range_begin_margin + ch.visibility_range_end_margin
				if m > 0.0:
					on += 1
				else:
					off += 1
		return [on, off]
	var raster_m: Array = margins.call()
	var env := Environment.new()
	env.pathtracing_enabled = true
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	get_viewport().use_pathtracing = true
	for i in 3:
		await get_tree().create_timer(0.3).timeout
	var traced_m: Array = margins.call()
	get_viewport().use_pathtracing = false
	we.queue_free()
	for i in 3:
		await get_tree().create_timer(0.3).timeout
	var back_m: Array = margins.call()
	_check("near swap under path tracing: no crossfade margins (no plant traced twice), back after", raster_m[0] > 0 and traced_m[0] == 0 and traced_m[1] == raster_m[0] + raster_m[1] and back_m[0] == raster_m[0], [raster_m, traced_m, back_m])
	target.global_position = Vector3(5000, 0, 5000)
	for i in 3:
		await get_tree().process_frame
	var gone: Array = _lazy_near_count(field, src.mesh)
	_check("near meshes on demand: none far away, built round the target once ready, freed after", away[0] == 0 and src.calls > 3 and near[0] >= want and near[0] > 0 and near[1] == near[2] and near[0] < field.get_placements(0).size() and gone[0] == 0 and gone[2] == 0 and src.keys.has(7), [away, near, want, gone, src.calls, field.get_placements(0).size()])
	tree_sp.near_mesh_source = null
	tree_sp.near_range = 0.0
	field.stream_target = NodePath()
	field.populate()
	target.queue_free()

# The plants as the sand's wind shelter (GroundSurface3D.wind_shelters, docs/ideas/sand.md "Dune
# grass"): a dry sand surface under a 12 m/s wind carries less sand among the plants once the
# field shelters it.
func _sand_shelter_check(spots: Array) -> void:
	var at := Vector3.ZERO
	for p in spots:
		if field.sample_lateral_cover(p) > 0.2:
			at = p
			break
	var hs := GDScript.new()
	hs.source_code = "extends Node3D\nvar map: Node\nfunc get_height(p: Vector3) -> float:\n\treturn map.get_height_at(p)\n"
	hs.reload()
	var height := Node3D.new()
	height.set_script(hs)
	height.set("map", dm)
	add_child(height)
	var probe := Marker3D.new()
	add_child(probe)
	probe.global_position = at
	var sand := GroundSurface3D.new()
	sand.ground_type = GroundSurface3D.GROUND_SAND
	sand.moisture = 0.0
	sand.keep_ruts = false # Each run starts from fresh sand.
	add_child(sand)
	sand.height_source = sand.get_path_to(height)
	sand.follow_target = sand.get_path_to(probe)
	sand.wind_override = Vector2(12.0, 0.0)
	for i in 70:
		await get_tree().physics_frame
	var open := sand.get_sand_flux_at(at).length()
	sand.wind_shelters = [sand.get_path_to(field)]
	for i in 70:
		await get_tree().physics_frame
	var sheltered := sand.get_sand_flux_at(at).length()
	_check("sand shelter: a ground surface under a gale carries less sand among the plants once they shelter it", open > 0.0 and sheltered < 0.5 * open, [snappedf(field.sample_lateral_cover(at), 0.01), open, sheltered])
	sand.queue_free()
	height.queue_free()
	probe.queue_free()

func _stream_checks() -> void:
	field.weather_map = NodePath()
	field.populate()
	var full := []
	for s in 3:
		full.append(field.get_placements(s))
	await _sand_shelter_check(full[0])
	var target := Node3D.new()
	add_child(target)
	target.global_position = Vector3(-60, 0, -50)
	var sf := VegetationField3D.new()
	sf.far_volume = false # Not under test here.
	sf.name = "Streamed"
	sf.populate_on_ready = false
	sf.biome = field.biome
	add_child(sf)
	sf.data_map = sf.get_path_to(dm)
	sf.stream_target = sf.get_path_to(target)
	sf.stream_radius = 60.0
	sf.streaming = true
	var frames := await _stream_wait(sf)
	var ok := frames >= 0
	var counts := []
	for s in 3:
		var want := _streamed_set(full[s], Vector2(-60, -50), 60.0, sf.tile_size)
		counts.append([sf.get_placements(s).size(), want.size()])
		ok = ok and _same(sf.get_placements(s), want, Vector2(-60, -50), 60.0, sf.tile_size)
	var drawn := 0
	for ch in sf.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh.mesh == field.biome.species[0].meshes[0]:
			drawn += ch.multimesh.instance_count
	_check("streaming: tiles around the centre match populate() exactly", ok and drawn == sf.get_placements(0).size(), [frames, sf.get_stream_tile_count(), counts, drawn])
	# Sand shelter (GroundSurface3D.wind_shelters): the plants' frontal area index, the same from
	# streamed tiles as from populate(), none where no plant stands.
	var lam := []
	var same := true
	var pos := 0
	for p in full[0]:
		if Vector2(p.x, p.z).distance_to(Vector2(-60, -50)) < 25.0 and lam.size() < 40:
			var a: float = field.sample_lateral_cover(p)
			var b: float = sf.sample_lateral_cover(p)
			lam.append(a)
			same = same and absf(a - b) < 1e-4
			if a > 0.0:
				pos += 1
	var mx := 0.0
	for v in lam:
		mx = maxf(mx, v)
	_check("sand shelter: plants' frontal area index at their feet, 0 to 0.5, streamed same as populated, 0 far off", lam.size() > 10 and pos == lam.size() and mx <= 0.5 and same and field.sample_lateral_cover(Vector3(5000, 0, 5000)) == 0.0, [lam.size(), pos, snappedf(mx, 0.001), same])
	target.global_position = Vector3(80, 0, 70)
	frames = await _stream_wait(sf)
	ok = frames >= 0
	var old_left := 0
	for s in 3:
		old_left += sf.get_placements(s).size() - _streamed_set(sf.get_placements(s), Vector2(80, 70), 60.0 + 1.5 * sf.tile_size, sf.tile_size).size()
	counts = []
	for s in 3:
		var want := _streamed_set(full[s], Vector2(80, 70), 60.0, sf.tile_size)
		counts.append([sf.get_placements(s).size(), want.size()])
		ok = ok and _same(sf.get_placements(s), want, Vector2(80, 70), 60.0, sf.tile_size)
	_check("streaming: moved centre, tiles past the radius freed, new ones match", ok and old_left == 0, [frames, sf.get_stream_tile_count(), counts, old_left])
	# Where the placed plants end, for the terrain's far blend (vegetation_near_radius).
	var near_on := VegetationField3D.get_near_radius()
	# A second streaming field out to 150 m (a forest): the nearest end still counts.
	var sf2 := VegetationField3D.new()
	sf2.far_volume = false # Not under test here.
	sf2.name = "StreamedWide"
	sf2.populate_on_ready = false
	sf2.biome = field.biome
	add_child(sf2)
	sf2.data_map = sf2.get_path_to(dm)
	sf2.stream_target = sf2.get_path_to(target)
	sf2.stream_radius = 150.0
	sf2.streaming = true
	await _stream_wait(sf2)
	var near_two := VegetationField3D.get_near_radius()
	# The cover map switched on after streaming started: built at once, the field keeps streaming.
	var had_map := sf2.get_cover_texture() != null
	sf2.cover_map_enabled = true
	var map_now := sf2.get_cover_texture() != null
	target.global_position += Vector3(30, 0, 0)
	var moved := await _stream_wait(sf2)
	_check("streaming: cover map switched on later is built at once, streaming goes on", not had_map and map_now and moved >= 0 and sf2.get_stream_tile_count() > 0 and VegetationField3D.get_near_radius() == 60.0, [had_map, map_now, moved, sf2.get_stream_tile_count()])
	sf.streaming = false
	var near_wide := VegetationField3D.get_near_radius()
	sf2.streaming = false
	sf2.queue_free()
	var near_off := VegetationField3D.get_near_radius()
	_check("streaming: publishes vegetation_near_radius (nearest of several fields), clears it when they stop", near_on == 60.0 and near_two == 60.0 and near_wide == 150.0 and near_off == 0.0, [near_on, near_two, near_wide, near_off])
	sf.queue_free()
	target.queue_free()

func _build_view() -> void:
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.17, 0.12, 0.08) # Woodland humus, dark brown.
	gm.roughness = 0.95
	var mi := MeshInstance3D.new()
	mi.mesh = _ground_mesh()
	mi.material_override = gm
	if far_match > 0:
		# A ground that draws the plants' far colour, as a terrain does (TerrainMaterial3D).
		var sh := Shader.new()
		sh.code = "shader_type spatial;\nrender_mode diffuse_burley;\n" + VegetationField3D.get_floor_shader_code() + """
void fragment() {
	vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	ALBEDO = vegetation_far(wp, vec3(0.025, 0.013, 0.007), CAMERA_POSITION_WORLD);
	ROUGHNESS = 0.95;
}
"""
		var smat := ShaderMaterial.new()
		smat.shader = sh
		mi.material_override = smat
	add_child(mi)
	var water := MeshInstance3D.new()
	var wp := PlaneMesh.new()
	wp.size = Vector2(size, size)
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.1, 0.2, 0.25)
	wmat.roughness = 0.05
	wmat.metallic_specular = 0.5
	wmat.cull_mode = BaseMaterial3D.CULL_DISABLED
	wp.material = wmat
	water.mesh = wp
	water.position.y = 0.5
	add_child(water)
	# The river's surface: a strip down the valley floor.
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for i in 60:
		var z0 := -100.0 + i * 2.0
		var z1 := z0 + 2.0
		var y0 := 0.03 * (z0 + 100.0) + 0.3
		var y1 := 0.03 * (z1 + 100.0) + 0.3
		var q := [Vector3(-8, y0, z0), Vector3(8, y0, z0), Vector3(8, y1, z1), Vector3(-8, y1, z1)]
		for k in [0, 2, 1, 0, 3, 2]:
			st.set_normal(Vector3.UP)
			st.add_vertex(q[k])
	var rv := MeshInstance3D.new()
	rv.name = "RiverWater"
	rv.mesh = st.commit()
	rv.material_override = wmat
	add_child(rv)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.6, 0.7, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.5, 0.55, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 50, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(30, 16, -20)
	add_child(cam)
	cam.look_at(Vector3(0, 1, -60))

func _ground_mesh() -> Mesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var res := 128
	var step := size / res
	for z in res:
		for x in res:
			var x0 := -size * 0.5 + x * step
			var z0 := -size * 0.5 + z * step
			var q := [Vector3(x0, height(x0, z0), z0), Vector3(x0 + step, height(x0 + step, z0), z0),
					Vector3(x0 + step, height(x0 + step, z0 + step), z0 + step), Vector3(x0, height(x0, z0 + step), z0 + step)]
			for i in [0, 1, 2, 0, 2, 3]:
				st.add_vertex(q[i])
	st.generate_normals()
	return st.commit()

# --stones=scree|river|shore --out=path.png: a 3 m patch of packed stones (random sequential
# addition, sunk a third into the ground) with the stand's plants between them, from 1.5 m.
# Stand-in stones until the rock generator's are on main.
func _stones_view() -> void:
	field.queue_free()
	var at := Vector3(20, height(20, 0), 0) # Dry ground on the valley side, clear of the river.
	var stream := stone_view == "stream"
	if stream:
		# --stones=stream: the same patch across the river's east edge (about 5.6 m from its middle),
		# stones under the water, at its edge and up the bank, with the stream's bank plants.
		at = Vector3(4.0, height(4.0, -5.0), -5.0)
	var rng := RandomNumberGenerator.new()
	rng.seed = 11
	var stones := []
	for attempt in 6000:
		var r := rng.randf_range(0.05, 0.26)
		var q := Vector2(at.x, at.z) + Vector2(rng.randf_range(-1.6, 1.6), rng.randf_range(-1.6, 1.6))
		var free := true
		for o in stones:
			if Vector2(q.x - o.x, q.y - o.z).length() < r + o.w + rng.randf_range(0.02, 0.12):
				free = false
				break
		if free:
			stones.append(Vector4(q.x, height(q.x, q.y), q.y, r))
	var noise := FastNoiseLite.new()
	noise.frequency = 2.2
	var meshes := []
	if stream:
		# Real stream cobbles from the rock generator, a little flattened (cobbles lie flat).
		for v in 4:
			var rg := RockGenerator.new()
			rg.family = RockGenerator.FAMILY_COBBLE
			rg.seed = 30 + v
			meshes.append(rg.generate())
		# Clear water, so the stones' wet tops and their moss show through it.
		var river_water := get_node_or_null("RiverWater") as MeshInstance3D
		if river_water:
			var clear := StandardMaterial3D.new()
			clear.albedo_color = Color(0.25, 0.32, 0.3, 0.35)
			clear.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			clear.roughness = 0.05
			clear.metallic_specular = 0.5
			clear.cull_mode = BaseMaterial3D.CULL_DISABLED
			river_water.material_override = clear
	for v in (0 if stream else 4):
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		sm.radial_segments = 20
		sm.rings = 10
		var st := SurfaceTool.new()
		st.create_from(sm, 0)
		var arr := st.commit_to_arrays()
		var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		for i in verts.size():
			var d := verts[i]
			var k := 1.0 + 0.22 * noise.get_noise_3dv(d * 1.3 + Vector3(v * 7.0, 0, 0)) + 0.08 * noise.get_noise_3dv(d * 4.0 + Vector3(0, v * 5.0, 0))
			verts[i] = Vector3(d.x * k, d.y * k * 0.62, d.z * k)
		arr[Mesh.ARRAY_VERTEX] = verts
		arr[Mesh.ARRAY_NORMAL] = null
		var am := ArrayMesh.new()
		am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
		var st2 := SurfaceTool.new()
		st2.create_from(am, 0)
		st2.generate_normals()
		meshes.append(st2.commit())
	var obstacles := PackedVector4Array()
	var tops := PackedVector4Array()
	for i in stones.size():
		var o: Vector4 = stones[i]
		var mi := MeshInstance3D.new()
		mi.mesh = meshes[i % 4]
		var g := rng.randf_range(0.36, 0.5)
		if stream:
			# A cobble's mesh at its own size: scaled so its widest is the stone's diameter, flattened
			# to lie on its broad face, sunk a third into the bed; its top from its bounds.
			var bb: AABB = mi.mesh.get_aabb()
			var k := 2.0 * o.w / maxf(bb.size.x, bb.size.z)
			var ky := k * 0.75
			mi.transform = Transform3D(Basis(Vector3.UP, rng.randf() * TAU).scaled(Vector3(k, ky, k)), Vector3(o.x, o.y - bb.position.y * ky - bb.size.y * ky * 0.33, o.z))
			add_child(mi)
			obstacles.append(Vector4(o.x, o.y, o.z, o.w * 0.95))
			tops.append(Vector4(o.x, o.y + bb.size.y * ky * 0.67 - 0.005, o.z, o.w * 0.5))
			continue
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(g, g * 0.98, g * 0.93)
		mat.roughness = 0.85
		mi.material_override = mat
		mi.transform = Transform3D(Basis(Vector3.UP, rng.randf() * TAU).scaled(Vector3(o.w, o.w, o.w)), Vector3(o.x, o.y + o.w * 0.62 * 0.3, o.z))
		add_child(mi)
		obstacles.append(Vector4(o.x, o.y, o.z, o.w * 0.95))
		tops.append(Vector4(o.x, o.y + o.w * 0.62 * 1.25, o.z, o.w * 0.5))
	var stand: int = {"scree": PlantGenerator.STONES_SCREE, "river": PlantGenerator.STONES_RIVER, "stream": PlantGenerator.STONES_RIVER, "shore": PlantGenerator.STONES_SHORE}.get(stone_view, PlantGenerator.STONES_SCREE)
	var veg := VegetationField3D.new()
	veg.far_volume = false # Not under test here.
	veg.name = "Stones"
	veg.populate_on_ready = false
	var b := Biome.new()
	var stone_set: Array[PlantSpecies] = []
	var stone_season := {"forcing": 900.0, "day_of_year": 175}
	if OS.get_environment("VEG_STONES_FORCING") != "":
		stone_season["forcing"] = float(OS.get_environment("VEG_STONES_FORCING"))
		stone_season["day_of_year"] = int(OS.get_environment("VEG_STONES_DAY"))
	for sp in PlantGenerator.make_stones(stand, 3, true, stone_season):
		stone_set.append(sp as PlantSpecies)
	if stream:
		for sp in PlantGenerator.make_banks(PlantGenerator.BANKS_STREAM, 3, true, stone_season):
			stone_set.append(sp as PlantSpecies)
	b.species = stone_set
	veg.biome = b
	veg.obstacles = obstacles
	veg.stone_tops = tops
	add_child(veg)
	veg.data_map = veg.get_path_to(dm)
	veg.populate()
	var counts := []
	for i in b.species.size():
		counts.append(veg.get_placements(i).size())
	print("VEG_STONES stand=%s stones=%d plants=%s" % [stone_view, stones.size(), str(counts)])
	if stream:
		var wet := 0
		for cp in tops:
			var q := Vector3(cp.x, 0, cp.z)
			var dep := dm.get_water_depth_at(q)
			if dep > 0.0 and cp.y - (dm.get_height_at(q) + dep) < 0.1:
				wet += 1
		print("VEG_STONES wet tops %d of %d" % [wet, tops.size()])
	veg.visible = stone_plants
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = at + Vector3(1.2, 1.1, 1.5)
	if stream:
		cam.position = at + Vector3(1.1, 0.55, 0.8) # From the bank, looking down over the edge.
	cam.fov = 50.0
	cam.near = 0.05
	cam.look_at(at + Vector3(0, 0.05, 0))
	if stream and OS.get_environment("VEG_STREAM_BANK") != "":
		# Along the bank: marsh marigold at the water's edge, butterbur and meadowsweet behind.
		cam.position = at + Vector3(4.2, 1.5, 7.0)
		cam.look_at(at + Vector3(2.2, 0.4, 0.0))
	cam.current = true

# --deadwood --out=path.png: a beech stand from TreeGenerator (trees, logs, fallen branches,
# sticks) over the beech floor set, looking at a log: moss and wood sorrel on it, ferns and
# banked leaves along it.
func _deadwood_view() -> void:
	field.queue_free()
	var t0 := Time.get_ticks_msec()
	var tg := TreeGenerator.new()
	var wood := VegetationField3D.new()
	wood.far_volume = false # Not under test here.
	wood.name = "Wood"
	wood.populate_on_ready = false
	if OS.get_environment("VEG_DW_FUNGI") != "":
		# The logs' and snags' fungi (TreeGenerator.fungi_cover, trees.md 5g): 0 for the before shot.
		tg.fungi_cover = float(OS.get_environment("VEG_DW_FUNGI"))
	if OS.get_environment("VEG_DW_SEED") != "":
		tg.seed = int(OS.get_environment("VEG_DW_SEED"))
	# VEG_DW_VARIANTS=3: logs and snags of decay classes 2-4 (one variant each), not only class 2.
	var nvar := int(OS.get_environment("VEG_DW_VARIANTS")) if OS.get_environment("VEG_DW_VARIANTS") != "" else 1
	wood.biome = tg.make_stand(TreeGenerator.STAND_BEECH, nvar)
	print("VEG_DEADWOOD_VIEW stand made in %d ms" % (Time.get_ticks_msec() - t0))
	add_child(wood)
	wood.data_map = wood.get_path_to(dm)
	wood.populate()
	print("VEG_DEADWOOD_VIEW stand placed in %d ms" % (Time.get_ticks_msec() - t0))
	var log_i := -1
	for i in wood.biome.species.size():
		var sp: PlantSpecies = wood.biome.species[i]
		if sp.has_meta("deadwood") and sp.resource_name.contains("(log)"):
			log_i = i
			print("VEG_LOGS %s placed %d" % [sp.resource_name, wood.get_placements(i).size()])
	var at := Vector3(0, height(0, 0), 0)
	if log_i >= 0 and wood.get_placements(log_i).size() > 0:
		var best := 1e9
		for p in wood.get_placements(log_i):
			var d := Vector2(p.x, p.z).length()
			if d < best:
				best = d
				at = p
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = at + Vector3(1.0, 2.0, 1.4)
	cam.fov = 60.0
	cam.look_at(at + Vector3(0, 0.2, 0))
	cam.current = true
	var ground := VegetationField3D.new()
	ground.far_volume = false # Not under test here.
	ground.name = "Floor"
	ground.populate_on_ready = false
	var gb := Biome.new()
	var floor_set: Array[PlantSpecies] = []
	var floor_season := {}
	if mushroom_view > 0:
		# Mid October in the weather's default climate: the beech litter's autumn colours, the
		# mushrooms fruiting.
		var wm := WeatherMap3D.new()
		wm.season_day = 288
		add_child(wm)
		floor_season = wm.get_season()
		wm.queue_free()
	floor_set.assign(PlantGenerator.make_floor(PlantGenerator.FLOOR_BEECH, 3, true, 15.0, floor_season))
	if OS.get_environment("VEG_DW_OFF") != "":
		# The before shot: the floor blind to the logs (nothing on or beside them, plants through them).
		for sp in floor_set:
			if sp.deadwood_rule == PlantSpecies.DEADWOOD_ON or sp.deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
				sp.density = 0.0
			else:
				sp.deadwood_rule = PlantSpecies.DEADWOOD_IGNORE
	if OS.get_environment("VEG_HONEY_DEBUG") != "":
		for sp in floor_set:
			if sp.has_meta("mushroom") and sp.deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
				sp.footprint = 0.0
				sp.density = 1.0
				print("VEG_HONEY footprint 0, density 1, min radius %.2f band %.2f shade %s" % [sp.deadwood_min_radius, sp.deadwood_band, str(sp.get_rule_range(PlantSpecies.RULE_SHADE))])
	gb.species = floor_set
	print("VEG_DEADWOOD_VIEW floor made in %d ms" % (Time.get_ticks_msec() - t0))
	ground.biome = gb
	ground.tile_size = 8.0
	add_child(ground)
	ground.data_map = ground.get_path_to(dm)
	ground.canopy_field = ground.get_path_to(wood)
	# The mushroom view places the whole 48 m floor at once, so its tufts are known before framing.
	ground.streaming = mushroom_view == 0
	ground.stream_target = ground.get_path_to(cam)
	ground.stream_radius = 30.0
	ground.populate()
	print("VEG_DEADWOOD_VIEW logs=%d log_species=%d at=%s" % [wood.get_deadwood_count(), log_i, str(at)])
	if OS.get_environment("VEG_DW_FUNGI_VIEW") != "" or OS.get_environment("VEG_DW_CAM") != "":
		for i in 3:
			await get_tree().process_frame
		_frame_log_fungi(wood, cam)
	if OS.get_environment("VEG_DW_MOSS") != "":
		# Close on a moss cushion on the log (the first of make_floor's log species).
		for i in 3:
			await get_tree().process_frame
		var mi := floor_set.size() - (5 if floor_set.size() > 0 else 0)
		var best := 1e9
		for p in ground.get_placements(mi):
			var d := p.distance_to(at)
			if d < best:
				best = d
				cam.position = p + Vector3(0.5, 0.45, 0.5)
				cam.look_at(p)
		print("VEG_DEADWOOD_VIEW moss species %d (%s) count %d nearest %.2f" % [mi, floor_set[mi].resource_name, ground.get_placements(mi).size(), best])
	if mushroom_view > 0:
		var logs := PackedVector3Array()
		for i in wood.biome.species.size():
			var sp: PlantSpecies = wood.biome.species[i]
			if sp.has_meta("deadwood") and sp.resource_name.contains("(log)"):
				logs.append_array(wood.get_placements(i))
		await _frame_mushrooms(ground, floor_set, cam, at, logs)
	for i in 15:
		await get_tree().process_frame
	if mushroom_view == 2:
		_hide_mushrooms(ground, floor_set)
	_render_later()

# --mushrooms[=off]: a honey fungus clump coming up from a log's side (make_floor's), from 40 cm out
# and 15 cm up under a low side sun. --mushrooms=off hides every mushroom and keeps the rest,
# placements and camera the same.
func _frame_mushrooms(ground: VegetationField3D, floor_set: Array[PlantSpecies], cam: Camera3D, at: Vector3, logs: PackedVector3Array) -> void:
	var honey := -1
	for i in floor_set.size():
		if floor_set[i].has_meta("mushroom"):
			print("VEG_MUSHROOMS species %d density %.3f band %.2f placed %d" % [i, floor_set[i].density, floor_set[i].deadwood_band, ground.get_placements(i).size()])
	if OS.get_environment("VEG_MUSH_COUNT") != "":
		get_tree().quit()
		return
	for i in floor_set.size():
		if floor_set[i].has_meta("mushroom") and floor_set[i].deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
			honey = i
	var tuft := at
	var best := 1e9
	# The tuft closest to a log's side.
	var wood := ground.get_node(ground.canopy_field) as VegetationField3D
	var hit := {}
	if honey >= 0:
		for p in ground.get_placements(honey):
			var h: Dictionary = wood.get_deadwood_at(p, 1.0, 0.12)
			# Clumps come up from the log's side: the one on a trunk nearest 30 cm thick (not a
			# root plate's girth).
			if not h.is_empty() and h["radius"] >= 0.12 and absf(float(h["radius"]) - 0.3) < best:
				best = absf(float(h["radius"]) - 0.3)
				tuft = p
				hit = h
	print("VEG_MUSHROOMS honey tufts %d, %.2f m off a 30 cm trunk at %s" % [ground.get_placements(honey).size() if honey >= 0 else 0, best, str(tuft)])
	# Out of the log: from its axis to the clump's foot, on wood as thick as the clumps take.
	var away := Vector3(1, 0, 0)
	var h0: Dictionary = wood.get_deadwood_at(tuft, 1.2, 0.12)
	if not h0.is_empty() and (h0["across"] as Vector3).length() > 0.5:
		away = h0["across"]
	away = away.normalized()
	print("VEG_MUSHROOMS away from the log %s" % str(away))
	var side := Vector3(-away.z, 0, away.x)
	# About 40 cm out from the clump and a little to one side, low (15 cm up), looking at it with the
	# log's bark behind: the clump fills about a third of the frame.
	var dir := (-away * 0.85 + side * 0.5).normalized()
	var eye := tuft - dir * 0.4
	# The clump roots up the log's side (tuft.y): from 13 cm above its foot, looking a little down
	# onto the caps and the stalks coming out of the bark (docs' framing: no sky in the frame).
	cam.position = Vector3(eye.x, tuft.y + 0.13, eye.z)
	cam.fov = 40.0
	cam.near = 0.05
	cam.look_at(tuft)
	print("VEG_MUSHROOMS camera eye %s look %s fov 40" % [str(cam.position), str(tuft)])
	# Soft shadows without the per-pixel rotated kernel's dot pattern (no TAA in one still).
	RenderingServer.directional_soft_shadow_filter_set_quality(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)
	# Low side light: the sun 22 degrees up, across the view from the open side, so the caps' scales
	# and the stalks' curves throw shadows.
	for ch in get_children():
		if ch is DirectionalLight3D:
			var ld := (side * 0.9 - away * 0.4).normalized() * cos(deg_to_rad(22.0)) + Vector3(0, -sin(deg_to_rad(22.0)), 0)
			(ch as DirectionalLight3D).global_transform = Transform3D(Basis.looking_at(ld, Vector3.UP), Vector3())
			(ch as DirectionalLight3D).directional_shadow_max_distance = 20.0
			(ch as DirectionalLight3D).light_angular_distance = 0.5
			(ch as DirectionalLight3D).light_color = Color(1.0, 0.88, 0.72) # Late afternoon in a wood: warm low sun.
		elif ch is WorldEnvironment:
			var env := (ch as WorldEnvironment).environment
			env.tonemap_exposure = 2.4 # Most of a stop over the floor views: the shade under a log.
			# Under the crowns the sky light reaches the floor filtered and reflected by leaves and
			# litter: warmer and weaker than the open sky (the bench's sky reads blue-grey on bark).
			env.ambient_light_color = Color(0.5, 0.44, 0.34)
			env.ambient_light_sky_contribution = 0.45

# --mushrooms=off: hide the floor's mushroom MultiMeshes once the tiles round the camera are in.
func _hide_mushrooms(ground: VegetationField3D, floor_set: Array[PlantSpecies]) -> void:
	var meshes := {}
	for sp in floor_set:
		if sp.has_meta("mushroom"):
			for m in sp.meshes:
				meshes[m] = true
			for m in sp.near_meshes:
				meshes[m] = true
	var hidden := 0
	for ch in ground.find_children("*", "MultiMeshInstance3D", true, false):
		var mmi := ch as MultiMeshInstance3D
		if mmi.multimesh and meshes.has(mmi.multimesh.mesh):
			mmi.visible = false
			hidden += 1
	print("VEG_MUSHROOMS off: %d mushroom MultiMeshes hidden" % hidden)

# VEG_DW_FUNGI_VIEW=1: from about a metre at knee to hip height, looking along the log nearest
# the middle that carries a bracket shelf (kinds 0-4) with a tuft of mushrooms (6, 7) on the same
# side within 2.5 m of it (VEG_DW_VARIANTS=3 VEG_DW_SEED=24: a class 3 beech log with a Fomes and a sulphur tuft),
# the sun behind the camera on the shelf's side. Prints VEG_DW_CAM=eye,look,sun so the same camera
# can be given back (VEG_DW_CAM) for the shot without fungi.
func _frame_log_fungi(p_wood: Node, p_cam: Camera3D) -> void:
	var sun: DirectionalLight3D
	for ch in get_children():
		if ch is DirectionalLight3D:
			sun = ch
	if OS.get_environment("VEG_DW_CAM") != "":
		var v := OS.get_environment("VEG_DW_CAM").split_floats(",")
		p_cam.position = Vector3(v[0], v[1], v[2])
		p_cam.look_at(Vector3(v[3], v[4], v[5]))
		sun.look_at_from_position(Vector3(v[6], v[7], v[8]) * 10.0, Vector3.ZERO)
		p_cam.fov = 55.0
		return
	var best := 1e9
	var pick := []
	for mmi in p_wood.find_children("*", "MultiMeshInstance3D", true, false):
		var mm: MultiMesh = mmi.multimesh
		if mm == null or mm.mesh == null or not mm.mesh.has_meta("tree_deadwood_axis"):
			continue
		var fp: PackedFloat32Array = mm.mesh.get_meta("tree_fungi", PackedFloat32Array())
		if fp.size() < 16:
			continue
		for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
			var xf: Transform3D = mmi.global_transform * mm.get_instance_transform(i)
			var d0 := Vector2(xf.origin.x, xf.origin.z).length()
			if d0 >= best:
				continue
			for a in fp.size() / 8:
				if int(fp[a * 8 + 7]) > 4:
					continue
				var pa := xf * Vector3(fp[a * 8], fp[a * 8 + 1], fp[a * 8 + 2])
				var na := (xf.basis * Vector3(fp[a * 8 + 3], fp[a * 8 + 4], fp[a * 8 + 5])).normalized()
				for b in fp.size() / 8:
					if int(fp[b * 8 + 7]) < 6:
						continue
					var pb := xf * Vector3(fp[b * 8], fp[b * 8 + 1], fp[b * 8 + 2])
					var nb := (xf.basis * Vector3(fp[b * 8 + 3], fp[b * 8 + 4], fp[b * 8 + 5])).normalized()
					# On the same side of the log, so one camera sees both.
					if Vector2(na.x, na.z).normalized().dot(Vector2(nb.x, nb.z).normalized()) < 0.2:
						continue
					# Well inside the ground patch (its edge shows past a log near it).
					if maxf(absf(pa.x), absf(pa.z)) > size * 0.5 - 8.0:
						continue
					if pa.distance_to(pb) < 2.5 and pa.distance_to(pb) > 0.3 and d0 < best:
						var ax: PackedFloat32Array = mm.mesh.get_meta("tree_deadwood_axis")
						best = d0
						pick = [pa, na, pb, (xf.basis * Vector3(ax[3] - ax[0], ax[4] - ax[1], ax[5] - ax[2])).normalized(), int(fp[a * 8 + 7]), int(fp[b * 8 + 7])]
	if pick.is_empty():
		print("VEG_DW_FUNGI_VIEW no log with a shelf and a tuft")
		return
	var pa: Vector3 = pick[0]
	var pb: Vector3 = pick[2]
	var ax: Vector3 = pick[3]
	var nh := Vector3(pick[1].x, 0, pick[1].z).normalized()
	var to_b := ax if (pb - pa).dot(ax) > 0.0 else -ax
	to_b = Vector3(to_b.x, 0, to_b.z).normalized()
	# A metre out and a little back along the log, the eye a hand above the shelf (knee to hip
	# height over the floor for a log on the ground), so the tuft on the log's shoulder shows.
	var eye := pa + nh * 0.85 - to_b * 0.55
	eye.y = maxf(height(eye.x, eye.z) + 0.5, maxf(pa.y, pb.y) + 0.22)
	var look := pa.lerp(pb, 0.4) - Vector3(0, 0.05, 0)
	p_cam.position = eye
	p_cam.look_at(look)
	p_cam.fov = 55.0
	var sdir := (nh * 0.8 - to_b * 0.4 + Vector3(0, 0.9, 0)).normalized()
	sun.look_at_from_position(sdir * 10.0, Vector3.ZERO)
	print("VEG_DW_FUNGI_VIEW shelf kind %d at %s, tuft kind %d at %s" % [pick[4], str(pa), pick[5], str(pb)])
	print("VEG_DW_CAM=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f" % [eye.x, eye.y, eye.z, look.x, look.y, look.z, sdir.x, sdir.y, sdir.z])

# --far-volume[=off] --out=path.png: bracken and heather down the valley with a 25 m range, from
# the valley's upper end at eye height: past 25 m the plants as a voxel shell out to 160 m
# (VegetationField3D.far_volume), or (=off) as nothing but bare ground.
func _far_volume_setup() -> void:
	var b := Biome.new()
	var list: Array[PlantSpecies] = []
	for spec in [[PlantGenerator.PRESET_BRACKEN, 1.1, 0.75, Color(0.16, 0.26, 0.07, 0.85), 25.0], [PlantGenerator.PRESET_HEATHER, 0.45, 0.9, Color(0.2, 0.17, 0.12, 0.7), 18.0]]:
		var g := PlantGenerator.new()
		g.preset = spec[0]
		g.apply_preset_colors()
		var sp := PlantSpecies.new()
		var ms: Array[Mesh] = []
		for m in g.generate_variants(3):
			ms.append(m)
		sp.meshes = ms
		sp.spacing = spec[1]
		sp.density = spec[2]
		sp.far_color = spec[3]
		sp.visibility_range = spec[4]
		sp.scale_range = Vector2(0.8, 1.2)
		sp.patch_scale = 12.0
		sp.patch_cover = 0.75 if far_match > 0 else 0.55 # The match wants covered and bare ground on both sides of the range.
		sp.patch_seed = 3 + list.size()
		sp.patch_invert = list.size() == 1
		list.append(sp)
	b.species = list
	field.biome = b
	field.far_volume = far_volume_view == 1
	field.far_match_view = far_match == 1
	field.far_volume_range = 160.0
	var cam_at := Vector3(0, height(0, 85) + (4.0 if far_match > 0 else 1.7), 85) # The match needs the ground past the range in view.
	var cam := get_viewport().get_camera_3d()
	cam.position = cam_at
	cam.look_at(Vector3(0, height(0, -40) + 1.0, -40))
	cam.far = 400.0

func _far_volume_stats() -> void:
	await get_tree().process_frame
	var tris := 0
	var cells := 0
	for ch in field.get_children(true):
		if ch is MultiMeshInstance3D and String(ch.name).begins_with("FarVolume"):
			cells += 1
			tris += (ch.multimesh.mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	print("VEG_FAR_VOLUME cells=%d triangles=%d" % [cells, tris])

func _render_later() -> void:
	for i in (6 if raster else 10):
		await RenderingServer.frame_post_draw
	if far_match == 3:
		RenderingServer.global_shader_parameter_set("vegetation_far_shade", 0.3)
	if far_match > 0:
		for i in 240:
			await RenderingServer.frame_post_draw
		print("VEG_FAR_MATCH ", field.get_far_match_stats())
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(1 if fails > 0 else 0)

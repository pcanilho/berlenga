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
#                    and with --out saves the view. VEG_DESERT_HIGH=1 looks from 12 m up across the dunes;
#                    VEG_DESERT_DAY=N sets the day of the year; VEG_DESERT_SHRUBS=0 leaves the woody shrubs out.
#   --burn[=off]     dry heath (make_heath) with four burn scars (WorldBurnScar3D, 0.05, 0.6, 2.5 and 5 years old)
#                    and make_burn's heath set, streamed round a camera 2.5 m up looking along the scars (off: no scars, no burn set):
#                    checks each burn plant grows only at its ages and the heather thins on the burns, prints the
#                    same costs as --floor and with --out saves the view.
#   --burn=live      quick check: a streamed field of one species recovering over a year; a scar placed after the
#                    tiles stand, aged, then removed (WorldFire.burn_changed, no rebuild): gone, half back, whole.
#   --rooting        quick check: boar rooting (RULE_ROOTING, make_rooting, make_floor's regrowth rules)
#   --trails         quick check: the game trail network (VegetationField3D.make_game_trails) over the valley, printed as VEG_TRAILS
#   --stand=live     quick check: a streamed field of succession stages (meta succession_age / succession_kind); a
#                    windthrow added after the tiles stand, aged a year (no re-place), then past the pioneer stage.

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
var stand_live := false # --stand=live
var trails_only := false # --trails
var rooting_only := false # --rooting
var burn_view := 0 # --burn: 1 with burn scars and make_burn's heath set, 2 the same heath unburnt (--burn=off).
const BURN_SCARS := [[0.05, -60.0], [0.6, -25.0], [2.5, 10.0], [5.0, 45.0]] # Years since, and the scar's centre z (x 60, 60 x 30 m).
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
		elif a == "--test=blowing":
			test = true
			only_blowing = true
		elif a == "--test=sod":
			test = true
			only_sod = true
		elif a.begins_with("--grass-walk"):
			grass_walk = float(a.substr(13)) if a.length() > 13 else 0.05
		elif a == "--test=rocks":
			test = true
			only_rocks = true
		elif a == "--test=bare":
			test = true
			only_bare = true
		elif a == "--test=grazing":
			test = true
			only_graze = true
		elif a == "--test=deer":
			test = true
			only_deer = true
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
		elif a == "--trails":
			trails_only = true
		elif a == "--rooting":
			rooting_only = true
		elif a == "--stand=live":
			stand_live = true
		elif a.begins_with("--burn"):
			burn_view = 2 if a == "--burn=off" else (3 if a == "--burn=live" else 1)
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
	if not out.is_empty() and desert_view == 0 and burn_view == 0:
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
	if burn_view == 3:
		await _burn_live()
		return
	if stand_live:
		await _stand_live()
		return
	if trails_only or rooting_only:
		if trails_only:
			_trails_check()
		else:
			_rooting_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if burn_view > 0:
		await _burn_view()
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
	if test and only_sod:
		await _sod_fall_check()
		await _sod_land_check()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if grass_walk > 0.0:
		await _grass_walk()
		get_tree().quit(0)
		return
	if test and only_deer:
		_deer_bed_check()
		_terracette_check()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test and only_graze:
		await _grazing_stream_check()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test and only_bare:
		await _climate_bare_check()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test and only_rocks:
		await _rock_field_check()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test and only_blowing:
		await _blowing_checks()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test:
		_checks()
		await _stream_checks()
		await _rock_field_check()
		await _climate_bare_check()
		_deer_bed_check()
		_terracette_check()
		await _lazy_near_checks()
		_deadwood_checks()
		await _place_form_checks()
		_trunk_checks()
		await _blowing_checks()
		await _wrack_live_check()
		await _sod_fall_check()
		await _sod_land_check()
		await _hay_bale_check()
		await _grazing_stream_check()
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
	var stand_env := OS.get_environment("VEG_FLOOR_STAND")
	floor_set.assign(PlantGenerator.make_floor(PlantGenerator.FLOOR_MEDITERRANEAN if stand_env == "mediterranean" else (PlantGenerator.FLOOR_BOREAL_TAIGA if stand_env == "taiga" else PlantGenerator.FLOOR_OAK_MIXED), 3, false)) # VEG_FLOOR_STAND=mediterranean: the montado set; =taiga: the boreal set
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
	ground.far_shell_clumps = OS.get_environment("VEG_FAR_CLUMPS") != "0" # VEG_FAR_CLUMPS=0: the far shell as an even mass
	if OS.get_environment("VEG_FLOOR_STAND") == "mediterranean" and OS.get_environment("VEG_FLOOR_SWARD") != "0":
		# The montado's summer sward as real blades (GrassField3D): near tiles, far rings to 250 m, blade columns to
		# 500 m (VEG_FLOOR_SWARD=0: the floor set alone).
		var sward := GrassField3D.new()
		sward.name = "Sward"
		sward.radius = 40.0
		sward.density = 90.0
		sward.blade_height = Vector2(0.2, 0.45)
		sward.clump_size = 0.6
		sward.dryness = 0.85
		sward.base_color = Color(0.32, 0.27, 0.13)
		sward.tip_color = Color(0.78, 0.66, 0.38)
		sward.ground_tint = Color(0.2, 0.16, 0.08, 1.0) # The thinned far blades' ground takes the sward's colour (as Ghost of Tsushima's terrain does).
		add_child(sward)
		sward.data_map = sward.get_path_to(dm)
		sward.canopy_field = sward.get_path_to(wood)
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
	gen.generate_on_ready = false # this bench generates itself, with its own settings
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
	# 400 m: the oasis's spring (about 150 m from the middle with this seed) falls inside.
	map.size = 400.0
	map.resolution = 400
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
	var shrubs := OS.get_environment("VEG_DESERT_SHRUBS") != "0" # 0: without the woody shrubs (their cost).
	# VEG_DESERT_DAY=N: the plants on day N of the year (saltbush panicles 190 to 20, calligonum fruit 85 to 215).
	var dseason := {} if OS.get_environment("VEG_DESERT_DAY") == "" else {"day_of_year": int(OS.get_environment("VEG_DESERT_DAY"))}
	desert_set.assign(Array(PlantGenerator.make_desert(PlantGenerator.DESERT_FLANKS, 3, true, dseason, shrubs)) + Array(PlantGenerator.make_desert(PlantGenerator.DESERT_INTERDUNE, 3, true, dseason, shrubs)))
	# Oases at desert springs (make_desert DESERT_OASIS, RULE_SPRING); VEG_DESERT_OASIS=0 leaves them out, =view frames one.
	var oasis_env := OS.get_environment("VEG_DESERT_OASIS")
	if oasis_env != "0" and oasis_env != "before":
		desert_set.append_array(Array(PlantGenerator.make_desert(PlantGenerator.DESERT_OASIS, 3, true, dseason, shrubs)))
	if desert_view == 1:
		# A plain species with no sand rule: the field keeps it off the sand sea's sand by itself.
		var plain := PlantSpecies.new()
		var pm := BoxMesh.new()
		pm.size = Vector3(0.2, 0.3, 0.2)
		plain.meshes = [pm]
		plain.spacing = 2.0
		plain.density = 0.6
		plain.set_meta("plain", true)
		desert_set.append(plain)
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
	var warm_t := Time.get_ticks_msec()
	while warm < 6000 and (warm < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		warm += 1
	print("VEG_DESERT streamed in %d frames, %d ms" % [warm, Time.get_ticks_msec() - warm_t])
	if desert_view == 1:
		_desert_checks(ground, desert_set, gen, map, mob)
		if oasis_env != "0":
			await _oasis_checks(ground, desert_set, cam, gen, oasis_env == "view" or oasis_env == "before", map)
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
	# VEG_DESERT_SAND=hours: a GroundSurface3D of dry sand on the dunes, sheltered by the plants (wind_shelters), under
	# a gale along the dunes' wind for that many hours (time scaled), seen from 2.5 m up across the wind: the sand
	# tails in the plants' lee (plant_wake). VEG_DESERT_TAA=1: TAA on.
	var sand_hours := float(OS.get_environment("VEG_DESERT_SAND")) if OS.get_environment("VEG_DESERT_SAND") != "" else 0.0
	var sand: GroundSurface3D = null
	if sand_hours > 0.0:
		sand = await _desert_sand(gen, cam, ground, sand_hours)
	get_viewport().use_taa = OS.get_environment("VEG_DESERT_TAA") == "1"
	get_viewport().disable_3d = false
	_desert_scene(gen, cam)
	if sand:
		# The dunes beyond the patch in the patch's own material, so its edge is no seam.
		for c in get_children():
			if c is MeshInstance3D and c.get_parent() == self and c.material_override is ShaderMaterial:
				c.material_override = sand.material
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

func _desert_sand(gen: TerrainGenerator3D, cam: Camera3D, plants: VegetationField3D, hours: float) -> GroundSurface3D:
	var wd := Vector2.from_angle(deg_to_rad(gen.sand_sea_wind_angle))
	var across := Vector3(-wd.y, 0.0, wd.x)
	var probe := Marker3D.new()
	probe.name = "SandProbe"
	add_child(probe)
	probe.position = cam.position + across * 12.0
	cam.position.y = gen.get_height(cam.position) + 2.5
	var look := cam.position + across * 25.0
	look.y = gen.get_height(look)
	cam.look_at(look)
	var s := GroundSurface3D.new()
	s.name = "DesertSand"
	s.ground_type = GroundSurface3D.GROUND_SAND
	s.moisture = 0.0
	s.wind_ripples = true
	# A fresh sand each run: kept ruts would bring back the last run's sand (and any fault it had).
	s.keep_ruts = false
	# VEG_DESERT_SAND_PROPS=name:value,...: the sand ground's properties, for an A/B.
	for kv in OS.get_environment("VEG_DESERT_SAND_PROPS").split(",", false):
		var pkv := kv.split(":", true, 1)
		s.set(pkv[0], str_to_var(pkv[1]))
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.72, 0.55, 0.36)
	m.roughness = 0.95
	s.material = m
	add_child(s)
	s.height_source = s.get_path_to(gen)
	s.follow_target = s.get_path_to(probe)
	s.wind_shelters = [s.get_path_to(plants)]
	s.wind_override = wd * 13.0
	for i in 120:
		await get_tree().physics_frame
	var scale := 3600.0
	s.ripple_time_scale = scale
	var frames := int(ceil(hours * 3600.0 / scale * 60.0))
	var t0 := Time.get_ticks_msec()
	for i in frames:
		await get_tree().physics_frame
		if i % 300 == 0:
			print("VEG_DESERT sand frame %d of %d, %.0f s" % [i, frames, (Time.get_ticks_msec() - t0) / 1000.0])
	s.ripple_time_scale = 1.0
	s.sync_deformation()
	for i in 60:
		await get_tree().physics_frame
	print("VEG_DESERT sand %.0f h of a 13 m/s wind: wind step %.2f ms" % [hours, s.get_wind_step_usec() / 1000.0])
	return s

# The sand's mobility as VegetationField3D works it out (_read_sand_mobility): the depth's place between its low and
# high within 12 cells, 0 under thin sand, -1 off the sand. Returns [grid, nx, ny, rect].
# The meadow sward (GrassField3D) keeps off the sand sea like the plants: no blades on deep sand, some off it.
func _desert_grass_check(gen: TerrainGenerator3D, map: TerrainDataMap3D) -> void:
	var g := GrassField3D.new()
	g.radius = 12.0
	g.far_rings = false
	g.far_shell = false
	g.rules = PlantSpecies.new()
	add_child(g)
	g.data_map = g.get_path_to(map)
	var deep := Vector3()
	var rng := RandomNumberGenerator.new()
	rng.seed = 11
	for k in 2000:
		var q := Vector3(rng.randf_range(-350, 350), 0, rng.randf_range(-350, 350))
		# Deep sand all round the 10 m the count looks at (the sward fades in by 0.3 m of sand).
		var ok := gen.get_sand_sea_height(q) > 1.5
		for a in 12:
			ok = ok and gen.get_sand_sea_height(q + Vector3(11.0, 0, 0).rotated(Vector3.UP, a * TAU / 12.0)) > 0.35
		if ok:
			deep = q
			break
	var counts := []
	for q in [deep, Vector3(460, 0, 460)]:
		for i in 4:
			g.update_at(q)
		var n := 0
		for ch in g.get_children(true):
			if ch is MultiMeshInstance3D:
				var buf := RenderingServer.multimesh_get_buffer(ch.multimesh.get_rid())
				for b in range(0, buf.size(), 16):
					if not (buf[b] == 0.0 and buf[b + 2] == 0.0) and Vector2(buf[b + 3] - q.x, buf[b + 11] - q.z).length() < 10.0:
						n += 1
		counts.append(n)
	g.queue_free()
	_check("desert: the meadow grass keeps off the sand sea (blades on deep sand, off the sea)", counts[0] == 0 and counts[1] > 100, [deep, counts])

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
	# As the field reads it (PlaceSetup::mobility_at): bilinear, nearest where a corner is off the sand (-1).
	var rect: Rect2 = mob[3]
	var nx: int = mob[1]
	var nz: int = mob[2]
	var gx := (p.x - rect.position.x) / rect.size.x * (nx - 1)
	var gz := (p.z - rect.position.y) / rect.size.y * (nz - 1)
	if gx < 0.0 or gz < 0.0 or gx > nx - 1 or gz > nz - 1:
		return -1.0
	var ix := mini(int(gx), nx - 2)
	var iz := mini(int(gz), nz - 2)
	var fx := gx - ix
	var fz := gz - iz
	var g: PackedFloat32Array = mob[0]
	var a: float = g[iz * nx + ix]
	var b: float = g[iz * nx + ix + 1]
	var c: float = g[(iz + 1) * nx + ix]
	var d: float = g[(iz + 1) * nx + ix + 1]
	if minf(minf(a, b), minf(c, d)) < 0.0:
		return (a if fz < 0.5 else c) if fx < 0.5 else (b if fz < 0.5 else d)
	return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fz)

func _desert_checks(ground: VegetationField3D, dset: Array[PlantSpecies], gen: TerrainGenerator3D, map: TerrainDataMap3D, mob: Array) -> void:
	_desert_grass_check(gen, map)
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
	# Mobility here is the field's own (bilinear): a fade's width (0.08) of slack.
	_check("desert: crust only on stable sand", crust[0] > 500 and crust[4] == crust[0], [crust[0], crust[4]])
	var shrubs := 0
	for s in dset.size():
		if dset[s].get_meta("woody_shrub", false):
			shrubs += ground.get_placements(s).size()
	var plain_deep := 0
	var plain_n := 0
	for s in dset.size():
		if dset[s].get_meta("plain", false):
			for p in ground.get_placements(s):
				plain_n += 1
				if gen.get_sand_sea_height(p) > 0.5: # The field fades it out by 0.3 m on a 4 m grid: slack for the grid.
					plain_deep += 1
	_check("desert: a species without a sand rule keeps off the sand sea's sand", plain_deep == 0, [plain_n, plain_deep])
	_check("desert: woody shrubs (calligonum, white broom, saltbush) placed", shrubs > 20 or OS.get_environment("VEG_DESERT_SHRUBS") == "0", [shrubs])
	# Nebkhas (TerrainGenerator3D.get_nebkhas): one mound shrub at each mound top in the streamed ground, the other
	# desert shrubs off the mounds.
	var mounds := gen.get_nebkhas(Rect2(-400, -400, 800, 800))
	var neb_n := 0
	var neb_off := 0
	var shrub_on := 0
	for s in dset.size():
		var is_neb: bool = dset[s].get_meta("nebkha", false)
		var is_shrub: bool = dset[s].get_meta("woody_shrub", false) and not dset[s].get_meta("oasis", false)
		if not is_neb and not is_shrub:
			continue
		for p in ground.get_placements(s):
			var best := 1e9
			var best_r := 0.0
			for m in mounds:
				var d := Vector2(m.x - p.x, m.z - p.z).length()
				if d < best:
					best = d
					best_r = 2.7 * m.w
			if is_neb:
				neb_n += 1
				if best > 0.6:
					neb_off += 1
			elif best < best_r + 1.0:
				shrub_on += 1
	print("VEG_NEBKHA mounds %d in the sea, mound shrubs %d (%d off a top), other shrubs on a mound %d" % [mounds.size(), neb_n, neb_off, shrub_on])
	_check("desert: a shrub crowns each nebkha, the others keep off them", mounds.is_empty() or (neb_n > 0 and neb_off == 0 and shrub_on == 0), [mounds.size(), neb_n, neb_off, shrub_on])
	_check("desert: annuals between the dunes, not up them", annual[0] > 500 and annual[3] < annual[0] * 0.5, [annual[0], annual[3]])

# Oases (docs/ideas/vegetation.md, "Oases at desert springs"): springs found in the sand sea's hollows; near one, the
# palms on its wet ground, the shrub ring where it is drier, no crust on the wettest. keep: leave the camera there,
# looking at the spring, for the frame.
func _oasis_checks(ground: VegetationField3D, dset: Array[PlantSpecies], cam: Camera3D, gen: TerrainGenerator3D, keep: bool, map: TerrainDataMap3D) -> void:
	var with_oasis := false
	for sp0 in dset:
		with_oasis = with_oasis or sp0.get_meta("oasis", false)
	var springs := ground.get_springs()
	print("VEG_OASIS springs ", springs)
	_check("oasis: a spring found in the sand sea's hollows", springs.size() >= 1, [springs.size()])
	if springs.is_empty():
		return
	var sp4: Vector4 = springs[0]
	var sp := Vector3(sp4.x, sp4.y, sp4.z)
	_check("oasis: the vegetation's springs are the terrain's (one list)", springs == gen.get_springs(), [springs, gen.get_springs()])
	# The wet ground round it, on a 4 m grid: area wet enough for palms, the shrub ring's, and the sand under the palms.
	var a_palm := 0
	var a_ring := 0
	var deep := 0.0
	for gx in range(-250, 251, 4):
		for gz in range(-250, 251, 4):
			var q := Vector3(sp.x + gx, 0, sp.z + gz)
			var wv := ground.get_spring_wetness_at(q)
			if wv >= 0.55:
				a_palm += 16
				deep = maxf(deep, gen.get_sand_sea_height(q))
			elif wv > 0.08 and wv < 0.5:
				a_ring += 16
	print("VEG_OASIS wet ground: palms %d m2, shrub ring %d m2, deepest sand under the palms %.1f m" % [a_palm, a_ring, deep])
	var start := cam.global_transform
	var at := Vector3(sp.x - 45.0, 0, sp.z - 25.0)
	at.y = gen.get_height(at) + 1.7
	if OS.get_environment("VEG_DESERT_HIGH") != "":
		# Above the corridor, looking down into it over the dune in front.
		at = Vector3(sp.x - 24.0, 0, sp.z - 14.0)
		at.y = maxf(gen.get_height(at), sp.y) + 7.0
	cam.global_position = at
	cam.look_at(Vector3(sp.x, sp.y + 2.0, sp.z))
	var radius := ground.stream_radius
	ground.stream_radius = 220.0 # The whole oasis for the counts.
	var w := 0
	var wt := Time.get_ticks_msec()
	while w < 20000 and (w < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		w += 1
	print("VEG_OASIS streamed in %d frames, %d ms" % [w, Time.get_ticks_msec() - wt])
	# Per kind: plants within the spring's reach, the lowest and highest wetness under them.
	var kinds := {"palm": [0, 9.0, -9.0], "olive": [0, 9.0, -9.0], "shrub": [0, 9.0, -9.0], "crust": [0, 9.0, -9.0]}
	for s in dset.size():
		var kind := ""
		var preset: int = dset[s].get_meta("preset", -1)
		if dset[s].get_meta("oasis", false):
			kind = "palm" if preset == TreeGenerator.PRESET_DATE_PALM else ("olive" if preset == TreeGenerator.PRESET_WILD_OLIVE else ("shrub" if dset[s].get_meta("woody_shrub", false) else ""))
		elif dset[s].get_meta("plant_preset", -1) == PlantGenerator.PRESET_SOIL_CRUST:
			kind = "crust"
		if kind == "":
			continue
		var k: Array = kinds[kind]
		for p in ground.get_placements(s):
			if Vector2(p.x - sp.x, p.z - sp.z).length() > 250.0:
				continue
			var wet := ground.get_spring_wetness_at(p)
			k[0] += 1
			k[1] = minf(k[1], wet)
			k[2] = maxf(k[2], wet)
	print("VEG_OASIS near the spring [plants, wetness min, max]: ", kinds)
	for s in dset.size():
		if dset[s].get_meta("oasis", false):
			var mn := 9.0
			for p in ground.get_placements(s):
				mn = minf(mn, ground.get_spring_wetness_at(p))
			print("VEG_OASIS species %s: %d placed, wetness min %.2f, spring_range %s" % [dset[s].resource_name if dset[s].resource_name != "" else dset[s].get_name(), ground.get_placements(s).size(), mn, dset[s].spring_range])
	if not with_oasis:
		# VEG_DESERT_OASIS=before: the same view without the oasis (the springs come from the crust's rule).
		ground.stream_radius = radius
		return
	if keep:
		# The oasis sward for the frame: short green grass round the spring (GrassField3D rules with a spring range).
		var sward := GrassField3D.new()
		sward.name = "OasisSward"
		sward.rules = PlantSpecies.new()
		sward.rules.spring_range = Vector2(0.5, 1.0)
		sward.rules.spring_fade = 0.1
		sward.blade_height = Vector2(0.15, 0.35)
		add_child(sward)
		sward.data_map = sward.get_path_to(map)
	_check("oasis: date palms on the wet ground (wetness over 0.47)", kinds["palm"][0] > 10 and kinds["palm"][1] > 0.46, kinds["palm"])
	_check("oasis: the shrub ring on the drier ground (under 0.73)", kinds["shrub"][0] > 2 and kinds["shrub"][2] < 0.74, kinds["shrub"])
	_check("oasis: no crust on the wettest ground (under 0.5)", kinds["crust"][2] < 0.51, kinds["crust"])
	if ClassDB.class_exists("OasisSprings3D"):
		# The water system's spring pools (OasisSprings3D, from the same springs): reeds in the shallows and on the
		# shore, rushes behind, following each pool's outline.
		var pools: Node3D = ClassDB.instantiate("OasisSprings3D")
		pools.name = "OasisPools"
		add_child(pools)
		# Terrain's springs and carving (TerrainGenerator3D.oasis_springs): the pools from the generator's springs,
		# their levels refitted, the hollows carved to the outlines; then the data map again.
		pools.spring_source = pools.get_path_to(gen)
		gen.oasis_springs = gen.get_path_to(pools)
		var carved: bool = gen.carve_oases()
		if not carved:
			pools.springs = springs
			pools.rebuild()
		map.bake()
		print("VEG_OASIS carved %s, pools %d, first vents %s, generator springs %s" % [str(carved), pools.get_pool_count(), str(pools.get_vents(0)) if pools.get_pool_count() > 0 else "-", str(gen.get_springs())])
		ground.spring_pools = ground.get_path_to(pools)
		w = 0
		while w < 20000 and (w < 20 or not ground.is_stream_idle()):
			await get_tree().process_frame
			w += 1
		var outline: PackedVector2Array = pools.get_outline(0) if pools.get_pool_count() > 0 else PackedVector2Array()
		var by := {}
		for s in dset.size():
			if not dset[s].get_meta("oasis_pool", false):
				continue
			var name: String = "reed" if dset[s].get_meta("plant_preset", -1) == PlantGenerator.PRESET_REED else "rush"
			var r := [0, 1e9, -1e9]
			for p in ground.get_placements(s):
				var d := _outline_distance(outline, Vector2(p.x, p.z))
				r[0] += 1
				r[1] = minf(r[1], d)
				r[2] = maxf(r[2], d)
			by[name] = r
		print("VEG_OASIS pools %d, area %.0f m2; round the first [plants, shore distance min, max]: %s" % [pools.get_pool_count(), pools.get_pool_area(0) if pools.get_pool_count() > 0 else 0.0, str(by)])
		_check("oasis: reeds round the spring pool, in the shallows and on the shore (-2.6 to 1.8 m)", by.has("reed") and by["reed"][0] > 10 and by["reed"][1] > -2.7 and by["reed"][2] < 1.9, by.get("reed"))
		_check("oasis: rushes on the damp sand behind (0 to 5.7 m)", by.has("rush") and by["rush"][0] > 5 and by["rush"][1] > -0.05 and by["rush"][2] < 5.8, by.get("rush"))
	ground.stream_radius = radius
	if not keep:
		cam.global_transform = start

# Signed distance from a closed outline (negative inside); far for an empty one.
func _outline_distance(o: PackedVector2Array, q: Vector2) -> float:
	if o.size() < 3:
		return 1e9
	var d := 1e9
	var inside := false
	var j := o.size() - 1
	for i in o.size():
		var a := o[j]
		var b := o[i]
		d = minf(d, q.distance_to(Geometry2D.get_closest_point_to_segment(q, a, b)))
		if (a.y > q.y) != (b.y > q.y) and q.x < a.x + (q.y - a.y) * (b.x - a.x) / (b.y - a.y):
			inside = not inside
		j = i
	return -d if inside else d

# Trees grown for a fire (mesh meta "tree_fire_years"): on a burn the variant nearest its age, unburnt ones off it.
func _fire_variant_check() -> void:
	var plain := BoxMesh.new()
	var young := BoxMesh.new()
	young.set_meta("tree_fire_years", 0.5)
	young.set_meta("tree_fire_band", Vector2(0, 1))
	var old := BoxMesh.new()
	old.set_meta("tree_fire_years", 8.0)
	old.set_meta("tree_fire_band", Vector2(6, 10))
	var sp := PlantSpecies.new()
	sp.meshes = [plain, young, old]
	sp.spacing = 1.5
	var scar := WorldBurnScar3D.new()
	scar.age_years = 0.4
	scar.size = Vector2(20, 20)
	scar.position = Vector3(-30, 0, -30)
	add_child(scar)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species = [sp]
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var on := {plain: 0, young: 0, old: 0}
	var off := {plain: 0, young: 0, old: 0}
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh and on.has(ch.multimesh.mesh):
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var p: Vector3 = ch.global_transform * mm.get_instance_transform(i).origin
				var d := maxf(absf(p.x + 30.0), absf(p.z + 30.0))
				if d < 8.0:
					on[mm.mesh] += 1
				elif d > 12.0:
					off[mm.mesh] += 1
	scar.free()
	f.queue_free()
	var r := [on[plain], on[young], on[old], off[plain], off[young], off[old]]
	_check("fire variants: trees on a burn take the variant nearest its age, unburnt ones off it", r[1] > 10 and r[0] == 0 and r[2] == 0 and r[3] > 10 and r[4] == 0 and r[5] == 0, r)

# Succession stages (TreeGenerator.make_succession meta, WorldStand): a pioneer stage (0-15 y, any kind), a windthrow
# legacy (0-15 y, wind only), a fire-only stage and the old stand (50 y on, old growth too).
func _stand_species() -> Array:
	var out := []
	for st in [[Vector2(0, 15), -1], [Vector2(0, 15), 2], [Vector2(0, 15), 1], [Vector2(50, 10000), -1]]:
		var sp := PlantSpecies.new()
		sp.meshes = [BoxMesh.new()]
		sp.spacing = 1.5
		sp.set_meta("succession_age", st[0])
		sp.set_meta("succession_kind", st[1])
		out.append(sp)
	return out

func _stand_count(f: VegetationField3D, sps: Array, inside: bool, c := Vector2.ZERO) -> Array:
	var n := [0, 0, 0, 0]
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var mm: MultiMesh = ch.multimesh
			var k := -1
			for j in sps.size():
				if sps[j].meshes[0] == mm.mesh:
					k = j
			if k < 0:
				continue
			for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
				var p: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
				var d := Vector2(p.x, p.z).distance_to(c)
				if (d < 7.0) if inside else (d > 13.0 and d < 30.0):
					n[k] += 1
	return n

func _stand_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	var c := Vector2(30, 30)
	st.add_disturbance(Vector3(c.x, 0, c.y), 10.0, WorldStand.DISTURBANCE_WIND, 5.0)
	var sps := _stand_species()
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species = sps
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var on := _stand_count(f, sps, true, c)
	var off := _stand_count(f, sps, false, c)
	st.clear_disturbances()
	f.queue_free()
	# A 5-year windthrow: pioneers and the wind legacy, no fire stage, no old stand; round it the old stand alone.
	_check("stand age: a 5-year windthrow holds the pioneers and its own legacy, the old stand round it", on[0] > 10 and on[1] > 2 and on[2] == 0 and on[3] == 0 and off[3] > 10 and off[0] == 0 and off[1] == 0 and off[2] == 0, [on, off])

# Beaver meadows (make_beaver): a drained pond bed reported as WorldStand kind 4, 6 years old, holds the meadow;
# the ground round it and a windthrow of the same age hold none of it. The pond set zones by water: lilies float
# only in deep water, rush only on the shore.
# Avalanche tracks (make_avalanche_track): ground reported as WorldStand kind 6 holds the track's flora by its age (torn
# turf and debris the first years, fireweed and tall forbs after), none round it or on a windthrow of the same age.
func _avalanche_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	var c := Vector2(30, 30)
	var track := PlantGenerator.make_avalanche_track(1, false)
	var counts := []
	for kind in [PlantGenerator.DISTURBANCE_AVALANCHE_KIND, WorldStand.DISTURBANCE_WIND]:
		st.clear_disturbances()
		st.add_disturbance(Vector3(c.x, 0, c.y), 10.0, kind, 5.0)
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign(track)
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var on := 0
		var off := 0
		for ch in f.get_children(true):
			if ch is MultiMeshInstance3D and ch.multimesh:
				var mm: MultiMesh = ch.multimesh
				for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
					var q: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
					var d := Vector2(q.x, q.z).distance_to(c)
					if d < 7.0:
						on += 1
					elif d > 13.0 and d < 30.0:
						off += 1
		counts.append([on, off])
		f.free()
	st.clear_disturbances()
	var fresh := 0
	var grown := 0
	for sp in track:
		if sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 0.5) > 0.5: fresh += 1
		if sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 15.0) > 0.5: grown += 1
	_check("avalanche track: a 5-year track (kind 6) holds its flora, none round it or on a windthrow; debris first, tall forbs later",
			counts[0][0] > 30 and counts[0][1] == 0 and counts[1][0] == 0 and counts[1][1] == 0 and fresh >= 3 and grown >= 4, [counts, fresh, grown])

# Flood wrack lines (make_flood_wrack): strand lines along the last flood's mark (WorldWater.high_water_at, a script
# stub here), the main line within 8 cm under the crest and thinner lines lower; none above it, none without a flood
# or once the flood is old.
class FloodMarkStub extends WorldWater:
	var peak := 0.0
	var days := 12.0
	func _height_at(_p: Vector3) -> float:
		return -INF
	func _high_water_at(_p: Vector3) -> Vector4:
		return Vector4(peak, days, 1.0, 0.0)

func _wrack_check() -> void:
	var sps := PlantGenerator.make_flood_wrack(1, false)
	var hs := []
	for z in range(-40, 41, 4):
		for x in range(-40, 41, 4):
			hs.append(dm.get_height_at(Vector3(x, 0, z)))
	hs.sort()
	var stub := FloodMarkStub.new()
	stub.peak = hs[hs.size() / 2]
	var res := []
	for case in ["fresh", "old", "none"]:
		if case != "none":
			stub.days = 12.0 if case == "fresh" else 900.0
			WorldContracts.add_water_stub(stub)
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign(sps)
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var main_on := 0
		var low_on := 0
		var off := 0
		for k in 2:
			for q in f.get_placements(k):
				var d: float = dm.get_height_at(q) - stub.peak
				if k == 0 and d > -0.12 and d < 0.03:
					main_on += 1
				elif k == 1 and d > -1.0 and d < -0.25:
					low_on += 1
				else:
					off += 1
		res.append([main_on, low_on, off])
		f.free()
		WorldContracts.clear_overrides()
	_check("flood wrack: main line under the crest, lower lines below, none above or without a recent flood", res[0][0] > 10 and res[0][1] > 0 and res[0][2] == 0 and res[1] == [0, 0, 0] and res[2] == [0, 0, 0], res)

# Live flood wrack: a streamed field follows the flood marks. The mark rises 0.4 m (a new flood); within a few
# seconds the tiles under it are placed again and the strand line lies along the new mark, none on the old one.
func _wrack_live_check() -> void:
	var vc := get_viewport().get_camera_3d()
	var eye := vc.global_position if vc else Vector3.ZERO
	var hs := []
	for z in range(-30, 31, 4):
		for x in range(-30, 31, 4):
			hs.append(dm.get_height_at(Vector3(eye.x + x, 0, eye.z + z)))
	hs.sort()
	var stub := FloodMarkStub.new()
	stub.peak = hs[hs.size() / 3]
	WorldContracts.add_water_stub(stub)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign([PlantGenerator.make_flood_wrack(1, false)[0]])
	add_child(f)
	f.data_map = f.get_path_to(dm)
	get_viewport().disable_3d = true
	f.streaming = true
	var wait := 0
	while wait < 1200 and (wait < 20 or not f.is_stream_idle()):
		await get_tree().process_frame
		wait += 1
	var before := _wrack_offsets(f, stub.peak)
	var old_peak := stub.peak
	stub.peak += 0.4
	var t0 := Time.get_ticks_msec()
	wait = 0
	while wait < 2400 and (Time.get_ticks_msec() - t0 < 2500 or f.get_flood_replaced_tiles() == 0 or not f.is_stream_idle()):
		await get_tree().process_frame
		wait += 1
	var after := _wrack_offsets(f, stub.peak)
	var on_old := _wrack_offsets(f, old_peak)
	get_viewport().disable_3d = false
	_check("live flood wrack: a new flood's mark re-places the strand line along it within seconds, none left on the old one",
			before.y > 10 and after.y > 10 and after.x > 0.8 and on_old.x < 0.2 and f.get_flood_replaced_tiles() > 0,
			"before: %s, after: %s (on the old line %.2f), tiles placed again %d, %.1f s" % [before, after, on_old.x, f.get_flood_replaced_tiles(), (Time.get_ticks_msec() - t0) / 1000.0])
	f.free()
	WorldContracts.clear_overrides()

# The share of a field's drawn wrack within the main line's band (-0.12..0.03 m) of a peak, and the count.
func _wrack_offsets(f: VegetationField3D, peak: float) -> Vector2:
	var on := 0
	var all := 0
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh and ch.visible:
			for i in ch.multimesh.visible_instance_count if ch.multimesh.visible_instance_count >= 0 else ch.multimesh.instance_count:
				var p: Vector3 = ch.global_transform * ch.multimesh.get_instance_transform(i).origin
				var d: float = dm.get_height_at(p) - peak
				all += 1
				if d > -0.12 and d < 0.03:
					on += 1
	return Vector2(float(on) / maxf(all, 1), all)

# Sod lips breaking (SodLipFall3D): a body stepping onto an overhang breaks that lip alone, which falls as a block and
# is not placed again; water at the scarp's foot undercuts the rest within the hazard's time; a dry bank keeps them.
class SodWaterStub extends WorldWater:
	var level := -INF
	var flow := Vector3.ZERO
	func _height_at(_p: Vector3) -> float:
		return level
	func _flow_at(_p: Vector3) -> Vector3:
		return flow
	var silt := 0.0 # kg/s handed in, summed over the calls
	func _add_sediment(_p: Vector3, mineral: float, _ash: float) -> void:
		silt += mineral

func _sod_fall_check() -> void:
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _relief_at(p: Vector3) -> Vector2:\n\tvar t := clampf((p.x + 0.3) / 0.6, 0.0, 1.0)\n\treturn Vector2(1.2 * (1.0 - t * t * (3.0 - 2.0 * t)), 1.0)\n"
	stub.reload()
	WorldContracts.set_ground(stub.new())
	var ws := SodWaterStub.new()
	WorldContracts.add_water_stub(ws)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign(PlantGenerator.make_bank_turf(1, false))
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var sod := SodLipFall3D.new()
	sod.range = 200.0
	sod.process_mode = Node.PROCESS_MODE_DISABLED # Driven by check() here.
	add_child(sod)
	var lips: Array = f.get_instances_near(0, Vector3.ZERO, 200.0)
	var n0 := lips.size()
	sod.check(0.5)
	var dry := sod.get_fall_count()
	# A walker on the first lip's overhang, 15 cm out over the drop.
	var xf: Transform3D = lips[0].transform if n0 > 0 else Transform3D()
	var walker := Node3D.new()
	walker.add_to_group("grass_trample")
	add_child(walker)
	walker.global_position = xf.origin + xf.basis.x.normalized() * 0.15 + Vector3(0, 0.9, 0)
	sod.check(0.5)
	var stepped := sod.get_fall_count()
	walker.queue_free()
	for i in 3:
		await get_tree().process_frame
	var after: Array = f.get_instances_near(0, Vector3.ZERO, 200.0)
	var regrown := 0
	for d in after:
		if (d.transform as Transform3D).origin.distance_to(xf.origin) < 0.5:
			regrown += 1
	# The river rises to 0.3 m under the lips: undercut, all fail within a long while.
	ws.level = xf.origin.y - 0.3
	var in_band := 0
	for d in after:
		var y: float = (d.transform as Transform3D).origin.y
		if ws.level > y - 0.7 and ws.level < y + 0.05:
			in_band += 1
	sod.check(20000.0)
	var undercut := sod.get_fall_count() - stepped
	var blocks: Array = sod.get_blocks()
	var falling := blocks.size() > 0 and (blocks[0] as RigidBody3D).mass > 5.0
	_check("sod lips: a step on the overhang breaks that lip into a falling block, not placed again; the river's undercut breaks the rest; a dry bank keeps them",
			n0 > 20 and dry == 0 and stepped == 1 and after.size() == n0 - 1 and regrown == 0 and in_band > 0 and undercut == in_band and falling,
			[n0, dry, stepped, after.size(), regrown, undercut, in_band, blocks.size()])
	sod.free()
	f.free()
	WorldContracts.clear_overrides()

# Sod blocks breaking on landing (SodLipFall3D, round 9): a lip broken off the 1.2 m scarp lands hard on the floor
# below and breaks into Blast pieces; in still water at the foot they stay; a 2 m/s flood flow along the bank rolls
# them off downstream.
func _sod_land_check() -> void:
	if not ClassDB.class_exists("PhysXDestructible3D"):
		_check("sod landing: no Blast in this build", true, [])
		return
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _relief_at(p: Vector3) -> Vector2:\n\tvar t := clampf((p.x + 0.3) / 0.6, 0.0, 1.0)\n\treturn Vector2(1.2 * (1.0 - t * t * (3.0 - 2.0 * t)), 1.0)\n"
	stub.reload()
	WorldContracts.set_ground(stub.new())
	var ws := SodWaterStub.new()
	WorldContracts.add_water_stub(ws)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign(PlantGenerator.make_bank_turf(1, false))
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var lips: Array = f.get_instances_near(0, Vector3.ZERO, 200.0)
	if lips.is_empty():
		_check("sod landing: lips placed", false, [])
		f.free()
		WorldContracts.clear_overrides()
		return
	var xf: Transform3D = lips[0].transform
	# The scarp's foot: a floor 1.2 m under the lip, out over the drop.
	var floor_y := xf.origin.y - 1.2
	var floor := StaticBody3D.new()
	# A wet silt bed (friction an estimate; the game's beds take theirs from the ground).
	floor.physics_material_override = PhysicsMaterial.new()
	floor.physics_material_override.friction = 0.4
	var cs := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(60, 1, 60)
	cs.shape = box
	floor.add_child(cs)
	add_child(floor)
	floor.global_position = Vector3(xf.origin.x, floor_y - 0.5, xf.origin.z)
	var sod := SodLipFall3D.new()
	sod.range = 200.0
	sod.undercut_time = 100000.0
	sod.soaked_time = 100000.0
	add_child(sod)
	sod.break_at(xf.origin)
	for i in 150:
		await get_tree().physics_frame
	var blocks: Array = sod.get_blocks()
	var node: Node = blocks[0] if blocks.size() > 0 else null
	var broke := node != null and node.is_class("PhysXDestructible3D")
	var n_pieces: int = node.get_piece_count() if broke else 0
	var centres := func() -> Array:
		var out := []
		if broke and is_instance_valid(node):
			for k in node.get_piece_count():
				out.append(node.get_piece_center(k))
		return out
	# Still water at the foot, then the flow.
	ws.level = floor_y + 0.45
	var c0: Array = centres.call()
	for i in 60:
		await get_tree().physics_frame
	var c1: Array = centres.call()
	ws.flow = Vector3(0, 0, 2.0)
	for i in 180:
		await get_tree().physics_frame
	var c2: Array = centres.call()
	var still := 0.0
	var carried := 0.0
	var moved := 0
	for k in mini(c0.size(), mini(c1.size(), c2.size())):
		still = maxf(still, Vector2(c1[k].x - c0[k].x, c1[k].z - c0[k].z).length())
		var dz: float = c2[k].z - c1[k].z
		carried += dz
		if dz > 0.3:
			moved += 1
	carried /= maxf(c2.size(), 1)
	print("VEG_SOD_LAND pieces %d still %.2f carried %.2f moved %d breaks %d" % [n_pieces, still, carried, moved, sod.get_break_count()])
	_check("sod landing: a lip falling 1.2 m breaks into Blast pieces on the floor; still water leaves them; a 2 m/s flood flow carries them downstream",
			broke and n_pieces >= 2 and sod.get_break_count() == 1 and still < 0.3 and carried > 0.3 and moved >= 1,
			[n_pieces, still, carried, moved])
	# Under water they slake (silt into the water), crumble and the crumbs wash out (times shortened for the check).
	sod.slake_time = 2.0
	sod.wash_time = 1.0
	var p_before: int = node.get_piece_count() if broke and is_instance_valid(node) else 0
	# Still water first: they slake but stay.
	var flow_was: Vector3 = ws.flow
	ws.flow = Vector3.ZERO
	for i in 180:
		await get_tree().physics_frame
	var p_mid: int = node.get_piece_count() if broke and is_instance_valid(node) else 0
	ws.flow = flow_was
	sod.crumb_volume = 0.05 # this small lip's pieces wear away within the second
	for i in 300:
		await get_tree().physics_frame
	var p_after: int = node.get_piece_count() if broke and is_instance_valid(node) else 0
	print("VEG_SOD_SLAKE silt %.3f kg, water got %.3f, pieces %d -> %d -> %d" % [sod.get_silt_released(), ws.silt, p_before, p_mid, p_after])
	_check("sod crumbling: pieces in the water slake silt into it; still water keeps them, a current wears them away",
			broke and sod.get_silt_released() > 0.0 and ws.silt > 0.0 and p_mid == p_before and p_after < p_before,
			[sod.get_silt_released(), ws.silt, p_before, p_after])
	sod.free()
	floor.free()
	f.free()
	WorldContracts.clear_overrides()

# Roots on bare scarps (make_bank_roots): the sward's fine roots along the scarp's top edge, each on the variant grown
# nearest the face's angle (the stub's face falls 1.2 m over about 0.6 m, near 63 degrees); none without the scarp;
# the woody roots none with no canopy near.
func _bank_roots_check() -> void:
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _relief_at(p: Vector3) -> Vector2:\n\tvar t := clampf((p.x + 0.3) / 0.6, 0.0, 1.0)\n\treturn Vector2(1.2 * (1.0 - t * t * (3.0 - 2.0 * t)), 1.0)\n"
	stub.reload()
	var res := []
	var faces := []
	for scarp in [true, false]:
		if scarp:
			WorldContracts.set_ground(stub.new())
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign(PlantGenerator.make_bank_roots(6, false))
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var on := 0
		var off := 0
		for q in f.get_placements(0):
			if q.x > -0.45 and q.x < 0.0:
				on += 1
			else:
				off += 1
		if scarp:
			for d in f.get_instances_near(0, Vector3.ZERO, 200.0):
				var m: Mesh = d.mesh
				if m and m.has_meta("bank_face_slope"):
					faces.append(roundi(rad_to_deg(float(m.get_meta("bank_face_slope")))))
		res.append([on, off, f.get_placements(1).size()])
		f.free()
		WorldContracts.clear_overrides()
	var face_ok := faces.size() > 0
	for a in faces:
		face_ok = face_ok and a >= 55 and a <= 73
	print("VEG_BANK_ROOTS faces ", faces.slice(0, 12))
	_check("bank roots: fine roots along a scarp's top edge on the variant nearest the face, none without the scarp, no woody roots with no canopy",
			res[0][0] > 20 and res[0][1] == 0 and res[0][2] == 0 and res[1] == [0, 0, 0] and face_ok, res)

# Hay meadows (GrassField3D.hay_meadows, GrassField3D.mown_days_at): a tall herb stands as cut stalks (its mesh cut at
# the stubble) inside a meadow mown 2 days ago, whole outside it and by 40 days; a low herb under the stubble is kept.
func _hay_check() -> void:
	var grass := GrassField3D.new()
	grass.radius = 0.0
	add_child(grass)
	var meadow := PackedVector2Array([Vector2(-20, -20), Vector2(0, -20), Vector2(0, 20), Vector2(-20, 20)])
	var res := []
	for days in [2.0, 40.0]:
		grass.hay_meadows = [{"polygon": meadow, "row_angle": 0.0, "days": days}]
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		var gen := PlantGenerator.new()
		var list := []
		for pr in [PlantGenerator.PRESET_FOXGLOVE, PlantGenerator.PRESET_MOSS]:
			gen.preset = pr
			gen.apply_preset_colors()
			var sp := PlantSpecies.new()
			sp.meshes = [gen.generate()]
			sp.spacing = 2.0
			sp.set_meta("plant_preset", pr)
			list.append(sp)
		f.biome.species.assign(list)
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var tall_in := 0
		var cut_in := 0
		var cut_out := 0
		for d in f.get_instances_near(0, Vector3(-10, 0, 0), 60.0):
			var q: Vector3 = (d.transform as Transform3D).origin
			var top: float = (d.mesh as Mesh).get_aabb().end.y
			var inside := q.x > -20.0 and q.x < 0.0 and q.z > -20.0 and q.z < 20.0
			if inside:
				tall_in += 1
				cut_in += 1 if top < 0.1 else 0
			elif top < 0.1:
				cut_out += 1
		var low_in := 0
		for q in f.get_placements(1):
			if q.x > -20.0 and q.x < 0.0 and q.z > -20.0 and q.z < 20.0:
				low_in += 1
		res.append([tall_in, cut_in, cut_out, low_in])
		f.free()
	grass.free()
	_check("hay meadows: the tall herb stands as cut stalks in a meadow mown 2 days ago, whole outside it and by 40 days; the low herb kept",
			res[0][0] > 20 and res[0][1] == res[0][0] and res[0][2] == 0 and res[1][0] > 20 and res[1][1] == 0 and res[0][3] > 20, res)

# Hay bales (GrassField3D, PlantGenerator.make_hay_bale): none while the windrows lie (day 3), dropped along the rows
# once baled (day 4), on the ground; carted by day 6.
func _hay_bale_check() -> void:
	var grass := GrassField3D.new()
	grass.radius = 0.0
	add_child(grass)
	grass.data_map = grass.get_path_to(dm)
	var meadow := PackedVector2Array([Vector2(-100, -100), Vector2(100, -100), Vector2(100, 100), Vector2(-100, 100)])
	var counts := []
	for days in [3.0, 4.0, 6.0]:
		grass.hay_meadows = [{"polygon": meadow, "row_angle": 0.4, "days": days}]
		for i in 3:
			await get_tree().process_frame
		counts.append(grass.get_hay_bale_count())
	grass.free()
	_check("hay bales: none on the windrows' day, along the rows once baled, carted after", counts[0] == 0 and counts[1] > 20 and counts[2] == 0, counts)

# Turf overhangs (make_bank_turf, RULE_BANK_EDGE): a stub's relief raises the ground 1.2 m west of a scarp at x = 0;
# sod lips stand along its top edge, none on the open ground either side or without the scarp.
func _turf_lip_check() -> void:
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _relief_at(p: Vector3) -> Vector2:\n\tvar t := clampf((p.x + 0.3) / 0.6, 0.0, 1.0)\n\treturn Vector2(1.2 * (1.0 - t * t * (3.0 - 2.0 * t)), 1.0)\n"
	stub.reload()
	var res := []
	for scarp in [true, false]:
		if scarp:
			WorldContracts.set_ground(stub.new())
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign(PlantGenerator.make_bank_turf(1, false))
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var on := 0
		var off := 0
		for q in f.get_placements(0):
			if q.x > -0.45 and q.x < 0.0:
				on += 1
			else:
				off += 1
				if scarp and off <= 4:
					print("VEG_TURF off at %s, ground %.2f" % [q, dm.get_height_at(q)])
		res.append([on, off])
		f.free()
		WorldContracts.clear_overrides()
	_check("turf overhangs: sod lips along a scarp's top edge, none on the open ground or without the scarp",
			res[0][0] > 20 and res[0][1] == 0 and res[1] == [0, 0], res)

# Pollen clouds (PollenCloud3D): dry wind shakes pollen from conifer crowns in flower; damp air, rain, calm or the
# wrong season sheds none; the nearest crowns shed, each by the wind at its crown.
class PollenWeatherStub extends WorldWeather:
	var wind := Vector3(9, 0, 0)
	var humidity := 0.4
	var day := 150
	func _wind_at(_p: Vector3, _h: float) -> Vector3:
		return wind
	func _air_humidity_at(_p: Vector3) -> float:
		return humidity
	func _surface_wetness_at(_p: Vector3) -> float:
		return 0.0
	func _rainfall_at(_p: Vector3) -> float:
		return 0.0
	func _day_of_year() -> int:
		return day
	func _season_at(_p: Vector3, _d: int) -> Dictionary:
		return {}

func _pollen_cloud_check() -> void:
	var rf := [PollenCloud3D.release_factor(0.4, 0.0, 0.0, 9.0), PollenCloud3D.release_factor(0.4, 0.0, 0.0, 1.0),
			PollenCloud3D.release_factor(0.92, 0.0, 0.0, 9.0), PollenCloud3D.release_factor(0.4, 0.8, 0.0, 9.0),
			PollenCloud3D.release_factor(0.4, 0.0, 2.0, 9.0), PollenCloud3D.window_share(150, Vector2(130, 170)),
			PollenCloud3D.window_share(100, Vector2(130, 170))]
	var w := PollenWeatherStub.new()
	WorldContracts.set_weather(w)
	var sp: PlantSpecies = field.biome.species[0].duplicate()
	sp.canopy_conifer = true
	if sp.canopy_radius <= 0.0:
		sp.canopy_radius = 2.0
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign([sp])
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var pc := PollenCloud3D.new()
	pc.max_grass_sources = 0
	add_child(pc)
	var res := []
	for case in ["windy", "humid", "calm", "winter"]:
		w.wind = Vector3(1, 0, 0) if case == "calm" else Vector3(9, 0, 0)
		w.humidity = 0.92 if case == "humid" else 0.4
		w.day = 20 if case == "winter" else 150
		pc.update_sources()
		res.append([case, pc.get_conifer_sources(), snappedf(pc.get_release(), 0.01)])
	var srcs: Array = pc.get_sources()
	pc.update_sources()
	var near_first := true
	var eye := get_viewport().get_camera_3d().global_position if get_viewport().get_camera_3d() else Vector3.ZERO
	var last := -1.0
	for d in pc.get_sources():
		var dd: float = (d.position as Vector3).distance_to(eye)
		near_first = near_first and dd >= last - 0.01
		last = dd
	_check("pollen clouds: dry windy air in flower sheds from the nearest conifer crowns; damp, calm or out of season none",
			rf[0] > 0.99 and rf[1] == 0.0 and rf[2] == 0.0 and rf[3] < 0.05 and rf[4] == 0.0 and rf[5] > 0.99 and rf[6] == 0.0
			and res[0][1] > 0 and res[1][1] == 0 and res[2][1] == 0 and res[3][1] == 0 and near_first and srcs.is_empty(),
			[rf, res, near_first])
	pc.free()
	f.free()
	WorldContracts.clear_overrides()

# Bank relief (WorldGround.relief_at, Landscape3D's bank and cliff relief): plants stand on the drawn ground, the
# heightfield plus the relief, not on the heightfield under it. A script stub raises the ground 0.3 m east of x = 0.
func _relief_check() -> void:
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _relief_at(p: Vector3) -> Vector2:\n\treturn Vector2(0.3, 1.0) if p.x > 0.0 else Vector2()\n"
	stub.reload()
	var res := []
	for raised in [false, true]:
		if raised:
			WorldContracts.set_ground(stub.new())
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign([field.biome.species[1]])
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var east := Vector2(0, 0)
		var west := Vector2(0, 0)
		for q in f.get_placements(0):
			var d: float = q.y - dm.get_height_at(q)
			if q.x > 0.5:
				east += Vector2(d, 1)
			elif q.x < -0.5:
				west += Vector2(d, 1)
		res.append([east.x / maxf(east.y, 1.0), west.x / maxf(west.y, 1.0), int(east.y)])
		f.free()
		WorldContracts.clear_overrides()
	_check("bank relief: plants stand on the drawn ground (heightfield plus WorldGround.relief_at), not under it",
			res[0][2] > 5 and absf(res[0][0]) < 0.02 and absf(res[1][0] - 0.3) < 0.02 and absf(res[1][1]) < 0.02, res)

# Seed down (SeedDown3D): a dandelion seed falls at its measured 0.39 m/s in still air (Cummins et al. 2018), two to
# three times faster damp; the wind carries seeds downwind and its gusts loft some; heads in seed loose them in a
# strong dry wind, none in calm air.
class SeedWeatherStub extends WorldWeather:
	var wind := Vector3.ZERO
	var wet := 0.0
	var season := {}
	func _wind_at(_p: Vector3, _h: float) -> Vector3:
		return wind
	func _surface_wetness_at(_p: Vector3) -> float:
		return wet
	func _rainfall_at(_p: Vector3) -> float:
		return 0.0
	func _ground_height_at(_p: Vector3) -> float:
		return 0.0
	func _season_at(_p: Vector3, _d: int) -> Dictionary:
		return season

func _seed_mean(sd: SeedDown3D) -> Vector4:
	var m := Vector4.ZERO
	var st := sd.get_seed_states()
	for q in st:
		m += q
	return m / maxf(st.size(), 1.0)

func _seed_down_check() -> void:
	var w := SeedWeatherStub.new()
	WorldContracts.set_weather(w)
	var sd := SeedDown3D.new()
	add_child(sd)
	sd.set_physics_process_internal(false)
	# Still air: the measured falling speed, and faster with the pappus closed.
	sd.release_at(Vector3(0, 10, 0), SeedDown3D.KIND_DANDELION, 100)
	sd.advance(5.0)
	var still := (10.0 - _seed_mean(sd).y) / 5.0
	sd.clear_seeds()
	w.wet = 1.0
	sd.release_at(Vector3(0, 10, 0), SeedDown3D.KIND_DANDELION, 100)
	sd.advance(5.0)
	var damp := (10.0 - _seed_mean(sd).y) / 5.0
	sd.clear_seeds()
	w.wet = 0.0
	# A 6 m/s wind: downwind, some lofted by the gusts, some landed. Over dry ground (the bench pond is water).
	var dry := Vector3(0, 0, 0)
	var water: WorldWater = WorldContracts.get_water()
	for k in 400:
		var c := Vector3(-60.0 + (k % 20) * 6.0, 0.0, -60.0 + (k / 20) * 6.0)
		var ok := true
		for dx in range(0, 25, 3):
			if is_finite(water.height_at(c + Vector3(dx, 0, 0))):
				ok = false
				break
		if ok:
			dry = c
			break
	w.wind = Vector3(6, 0, 0)
	sd.release_at(dry + Vector3(0, 0.6, 0), SeedDown3D.KIND_DANDELION, 200)
	var top := 0.0
	for i in 20:
		sd.advance(0.5)
		for q in sd.get_seed_states():
			top = maxf(top, q.y)
	var windy := _seed_mean(sd) - Vector4(dry.x, 0, dry.z, 0)
	var landed := sd.get_lying_count()
	sd.clear_seeds()
	# Heads in seed: a stand of dandelion clocks loose seeds in a strong dry wind, none in calm air.
	var forcing := -1.0
	for f in range(90, 700, 10):
		if PlantGenerator.get_bloom_shares(PlantGenerator.PRESET_DANDELION, {"forcing": float(f), "day_of_year": 120}).z > 0.3:
			forcing = f
			break
	w.season = {"forcing": forcing, "day_of_year": 120}
	var gen := PlantGenerator.new()
	gen.preset = PlantGenerator.PRESET_DANDELION
	var sp := PlantSpecies.new()
	sp.meshes = [gen.generate()]
	sp.spacing = 0.6
	sp.set_meta("plant_preset", PlantGenerator.PRESET_DANDELION)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign([sp])
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	sd.position = Vector3(0, 0, 0)
	sd.refresh_sources()
	var heads := sd.get_source_count()
	w.wind = Vector3.ZERO
	sd.advance(10.0, true)
	var calm := sd.get_released_count()
	w.wind = Vector3(9, 0, 0)
	sd.advance(10.0, true)
	var blown := sd.get_released_count() - calm
	f.free()
	sd.free()
	WorldContracts.set_weather(null)
	_check("seed down: falls at 0.39 m/s still, 2-3x damp; drifts downwind, gusts loft some; heads loose seeds in wind, none in calm",
			absf(still - 0.391) < 0.03 and damp > still * 2.0 and windy.x > 3.0 and top > 0.9 and landed > 10 and heads > 50 and calm == 0 and blown > 20,
			[snappedf(still, 0.001), snappedf(damp, 0.001), windy, snappedf(top, 0.01), landed, heads, forcing, calm, blown])

func _beaver_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	var c := Vector2(30, 30)
	var meadow := PlantGenerator.make_beaver(PlantGenerator.BEAVER_MEADOW, 1, false)
	var counts := []
	for kind in [PlantGenerator.DISTURBANCE_BEAVER_KIND, WorldStand.DISTURBANCE_WIND]:
		st.clear_disturbances()
		st.add_disturbance(Vector3(c.x, 0, c.y), 10.0, kind, 6.0)
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = Biome.new()
		f.biome.species.assign(meadow)
		add_child(f)
		f.data_map = f.get_path_to(dm)
		f.populate()
		var on := 0
		var off := 0
		for ch in f.get_children(true):
			if ch is MultiMeshInstance3D and ch.multimesh:
				var mm: MultiMesh = ch.multimesh
				for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
					var q: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
					var d := Vector2(q.x, q.z).distance_to(c)
					if d < 7.0:
						on += 1
					elif d > 13.0 and d < 30.0:
						off += 1
		counts.append([on, off])
		f.free()
	st.clear_disturbances()
	var stages := []
	for sp in meadow:
		stages.append([sp.get_meta("plant_preset", -1), snappedf(sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 1.0), 0.01), snappedf(sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 20.0), 0.01)])
	var young := 0
	var old := 0
	for s in stages:
		if s[1] > 0.5: young += 1
		if s[2] > 0.5: old += 1
	_check("beaver meadow: a 6-year drained pond bed (kind 4) holds the meadow, none round it or on a windthrow", counts[0][0] > 30 and counts[0][1] == 0 and counts[1][0] == 0 and counts[1][1] == 0 and young >= 2 and old >= 4, [counts, stages])
	var pond_bad := []
	for sp in PlantGenerator.make_beaver(PlantGenerator.BEAVER_POND, 1, false):
		var p: int = sp.get_meta("plant_preset", -1)
		var land: float = sp.rule_weight(PlantSpecies.RULE_WATER_DEPTH, 0.0)
		var deep: float = sp.rule_weight(PlantSpecies.RULE_WATER_DEPTH, 1.0)
		if p == PlantGenerator.PRESET_WATER_LILY and (not sp.floating or land > 0.0 or deep < 0.9):
			pond_bad.append(["lily", land, deep])
		if p == PlantGenerator.PRESET_RUSH and (deep > 0.0 or sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 3.0) < 0.9 or sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 12.0) > 0.0):
			pond_bad.append(["rush", deep])
	_check("beaver pond: lilies float in deep water only, rush on the shore only", pond_bad.is_empty(), pond_bad)

# Deer browsing (RULE_BROWSE, WorldStand.browse_at): a 0.9 browse disc swaps the full bramble for the bitten one and
# brings the deer lawn; round it the bramble is whole and no lawn grows. make_floor carries the browse response.
# Game trails (VegetationField3D.make_game_trails, docs/ideas/vegetation.md "Game trails"): goals on both sides of the
# river and below the cliff band, linked over the map. No trail through deep water or up ground over 40 degrees; the
# links merge (the network shorter than its links laid end to end); shared stretches carry the summed passes; the same
# seed gives the same network.
func _trails_check() -> void:
	var goals := PackedVector3Array([Vector3(-40, 0, -30), Vector3(-35, 0, 35), Vector3(30, 0, -35), Vector3(35, 0, 30), Vector3(-10, 0, 70), Vector3(20, 0, 5)])
	var area := Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE)
	var t0 := Time.get_ticks_usec()
	var trails: Array = VegetationField3D.make_game_trails(dm, area, goals, {"seed": 3})
	var ms := (Time.get_ticks_usec() - t0) / 1000.0
	var bad := 0
	var length := 0.0
	var top := 0.0
	for t in trails:
		var pts: PackedVector3Array = t["points"]
		top = maxf(top, t["passes"])
		for k in pts.size():
			if dm.get_water_depth_at(pts[k]) > 0.45 or dm.get_slope_at(pts[k]) > deg_to_rad(42.0):
				bad += 1
			if k > 0:
				length += Vector2(pts[k].x - pts[k - 1].x, pts[k].z - pts[k - 1].z).length()
	var links := 0.0
	for i in goals.size():
		for j in range(i + 1, goals.size()):
			var d := Vector2(goals[i].x - goals[j].x, goals[i].z - goals[j].z).length()
			if d <= 250.0:
				links += d
	var again: Array = VegetationField3D.make_game_trails(dm, area, goals, {"seed": 3})
	var same := again.size() == trails.size()
	for i in mini(again.size(), trails.size()):
		same = same and again[i]["points"] == trails[i]["points"]
	_check("game trails: none through deep water or up steep ground, merged (network %.0f m vs links %.0f m), shared stretches busier (%.0f passes), repeatable (%.0f ms)" % [length, links, top, ms],
			trails.size() > 3 and bad == 0 and length < 0.6 * links and top >= 1000.0 and same, [trails.size(), bad, length, links, top, same])

# Boar rooting (WorldStand DISTURBANCE_ROOTING discs, PlantSpecies.RULE_ROOTING; make_rooting, make_floor): a fresh
# patch holds the sods and none of the field layer, the ground round it the reverse; aged two years the field layer is
# back and the sods gone; the patch's arrival after the tiles stand is followed.
# Anthills and molehills (PlantGenerator.make_mounds): every species tells the grass its radius, ant hills keep their
# east crest, molehills slump with age, wood ant nests stand in the wood near its edge only.
# Leaf drifts (docs/ideas/vegetation.md, "Leaf drifts"): the wind gathers fallen leaves in hollows and at the feet of
# obstacles, deepest downwind, and sweeps crests; broadleaf floors end with leaf banks placed by that drift.
func _drift_check() -> void:
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	var sp := PlantSpecies.new()
	sp.meshes = [BoxMesh.new()]
	sp.spacing = 4.0
	f.biome = Biome.new()
	f.biome.species.assign([sp])
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.litter_wind = Vector2(0.5, 0.0)
	var ring := func(p: Vector3) -> float:
		return 0.6 * ((dm.get_height_at(p + Vector3(2.5, 0, 0)) + dm.get_height_at(p - Vector3(2.5, 0, 0)) + dm.get_height_at(p + Vector3(0, 0, 2.5)) + dm.get_height_at(p - Vector3(0, 0, 2.5))) * 0.25 - dm.get_height_at(p))
	var hollow := Vector3.ZERO
	var crest := Vector3.ZERO
	var flat := Vector3.ZERO
	var hi := -1e9
	var lo := 1e9
	var fl := 1e9
	for z in range(-60, 61, 5):
		for x in range(-60, 61, 5):
			var p := Vector3(x, 0, z)
			var r: float = ring.call(p)
			if r > hi:
				hi = r
				hollow = p
			if r < lo:
				lo = r
				crest = p
			if absf(r) < fl and dm.get_slope_at(p) < 0.15:
				fl = absf(r)
				flat = p
	f.obstacles = PackedVector4Array([Vector4(flat.x, dm.get_height_at(flat), flat.z, 0.8)])
	f.populate()
	var d_hollow := f.get_litter_drift_at(hollow)
	var d_crest := f.get_litter_drift_at(crest)
	var lee := f.get_litter_drift_at(flat + Vector3(1.0, 0, 0))
	var windward := f.get_litter_drift_at(flat - Vector3(1.0, 0, 0))
	var away := f.get_litter_drift_at(flat + Vector3(4.0, 0, 0))
	f.free()
	_check("leaf drift: more in the deepest hollow than on the highest crest, most at a stone's downwind foot, more there than at its windward foot or 3 m off",
			d_hollow > d_crest + 0.1 and lee > away + 0.5 and lee > windward and windward > away, "hollow %.2f (%.2f m) crest %.2f (%.2f m); stone lee %.2f windward %.2f away %.2f" % [d_hollow, hi / 0.6, d_crest, lo / 0.6, lee, windward, away])
	var floor_set := PlantGenerator.make_floor(PlantGenerator.FLOOR_BEECH, 1, false)
	var last: PlantSpecies = floor_set[floor_set.size() - 1]
	var bank_ok: bool = last.get_meta("plant_preset", -1) == PlantGenerator.PRESET_LEAF_BANK and last.get_rule_range(PlantSpecies.RULE_LITTER_DRIFT).x >= 1.3 and last.rule_weight(PlantSpecies.RULE_LITTER_DRIFT, 1.0) == 0.0
	var bh: float = (last.meshes[0] as Mesh).get_aabb().end.y if bank_ok else 0.0
	# Only where broadleaf crowns dropped litter (needle crowns add none to get_litter_at).
	var on_litter: bool = bank_ok and last.rule_weight(PlantSpecies.RULE_LITTER, 0.2) == 0.0
	_check("leaf drift: broadleaf floors end with leaf banks 8 to 25 cm high where the drift passes 1.35, only on broadleaf litter", bank_ok and on_litter and bh > 0.08 and bh < 0.25, [bank_ok, on_litter, bh])

# Grazing roughs and dung (docs/ideas/vegetation.md, "Grazing roughs and dung"): the sets' make-up, and deer pellets
# only on the heavily browsed lawn.
func _grazing_check() -> void:
	var cattle := PlantGenerator.make_grazing(PlantGenerator.GRAZE_CATTLE, 1, false)
	var horse := PlantGenerator.make_grazing(PlantGenerator.GRAZE_HORSE, 1, false)
	var deer := PlantGenerator.make_grazing(PlantGenerator.GRAZE_DEER, 1, false)
	var pat_h := {}
	var rough_h := 0.0
	var rough_tag := false
	for sp in cattle:
		var pre: int = sp.get_meta("plant_preset", -1)
		var top: float = (sp.meshes[0] as Mesh).get_aabb().end.y
		if pre == PlantGenerator.PRESET_COW_PAT:
			pat_h[float(sp.get_meta("dung_age"))] = top
		elif pre == PlantGenerator.PRESET_DUNG_ROUGH:
			rough_h = top
			rough_tag = sp.has_meta("sward_mound")
	var ages := pat_h.keys()
	ages.sort()
	var pats_age: bool = ages.size() == 3 and pat_h[ages[0]] < 0.06 and pat_h[ages[0]] > 0.015
	var seeds := {}
	for sp in horse:
		if sp.get_patch_scale() > 0.0:
			seeds[sp.get_patch_seed()] = true
	_check("grazing: cattle pats of three ages 2-6 cm high, a rough 25 cm+ that keeps the sward out; horse latrines share one patch layout",
			pats_age and rough_h > 0.25 and rough_tag and seeds.size() == 1, [pat_h, rough_h, rough_tag, seeds.keys()])
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	st.clear_browse()
	var c := Vector2(-40, -40)
	st.add_browse(Vector3(c.x, 0, c.y), 10.0, 0.9, 1.5)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	var heads: Array = PlantGenerator.make_flowering_grasses(1, false, {"day_of_year": 170})
	f.biome.species.assign(deer + cattle + heads)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var on := 0
	var off := 0
	var c_on := 0
	var c_off := 0
	for s in deer.size() + cattle.size():
		for q in f.get_placements(s):
			var d := Vector2(q.x, q.z).distance_to(c)
			if d < 8.0:
				if s < deer.size(): on += 1
				else: c_on += 1
			elif d > 14.0:
				if s < deer.size(): off += 1
				else: c_off += 1
	# The flowering grasses: bitten to the stubble on the pasture (mesh meta hay_cut_of), standing off it.
	var bit_on := [0, 0]
	var bit_off := [0, 0]
	for s in heads.size():
		var si: int = deer.size() + cattle.size() + s
		for d in f.get_instances_near(si, Vector3(c.x, 0, c.y), 8.0):
			bit_on[0] += 1
			if (d["mesh"] as Mesh).has_meta("hay_cut_of"): bit_on[1] += 1
		for d in f.get_instances_near(si, Vector3(c.x + 40.0, 0, c.y + 40.0), 8.0):
			bit_off[0] += 1
			if (d["mesh"] as Mesh).has_meta("hay_cut_of"): bit_off[1] += 1
	f.free()
	st.clear_browse()
	_check("grazing: deer pellet groups on the browsed lawn only", on > 3 and off == 0, [on, off])
	_check("grazing: flowering grasses bitten to the stubble on the pasture, standing off it", bit_on[0] > 5 and bit_on[1] > bit_on[0] * 0.8 and bit_off[0] > 5 and bit_off[1] == 0, [bit_on, bit_off])
	# The pasture is one grazing disc (WorldStand.add_browse): it crops the sward to a lawn and holds the cattle's
	# pats and roughs; none off it.
	_check("grazing: cattle pats and roughs on the grazed pasture only", c_on > 2 and c_off == 0, [c_on, c_off])

# Meadow fairy rings (docs/ideas/vegetation.md, "Meadow fairy rings"): the champignons come up on the rings the grass
# shows (GrassField3D.get_fairy_ring_at), nowhere else.
func _fairy_check() -> void:
	var sps := PlantGenerator.make_fairy_rings(1, false)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign(sps)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var on := 0
	var off := 0
	for q in f.get_placements(0):
		var b: Vector3 = GrassField3D.get_fairy_ring_at(q)
		if b.x + b.y > 0.05:
			on += 1
		else:
			off += 1
	f.free()
	var dark := 0
	var dead := 0
	for z in range(-100, 101):
		for x in range(-100, 101):
			var b: Vector3 = GrassField3D.get_fairy_ring_at(Vector3(x + 0.5, 0, z + 0.5))
			if b.x > 0.5:
				dark += 1
			if b.y > 0.5:
				dead += 1
	_check("fairy rings: dark and dead bands in a 200 m square; champignons only on the rings", dark > 20 and dead > 20 and on > 20 and off == 0, [dark, dead, on, off])

# Blowing leaves (LeafFall3D.creep_speed): lying leaves stay put in light air, slide downwind once the wind 10 cm up
# passes the creep speed (2.5 m/s here, from 12 m/s at 10 m over short grass), and only a few lift below the lift speed.
var only_blowing := false # --test=blowing: the blowing-leaves check alone
var only_sod := false # --test=sod: the sod lip checks alone
var only_rocks := false # --test=rocks: the rock_field check alone
var only_bare := false # --test=bare: the climate_bare check alone
var only_graze := false # --test=grazing: the streamed pasture check alone
var only_deer := false # --test=deer: the deer bed check alone
var grass_walk := 0.0 # --grass-walk[=M]: main-thread cost of a GrassField3D (defaults) walked M metres a frame

# --grass-walk[=M] (0.05 m a frame by default: a walk at 174 frames a second is 0.008): the main-thread time of
# GrassField3D.update_at each frame (its near tiles and far ring tiles are built there), over 600 frames.
# --grass-cpu: the CPU fill path; --grass-norings: no far rings.
func _grass_walk() -> void:
	var user := OS.get_cmdline_user_args()
	var g := GrassField3D.new()
	g.rules = PlantSpecies.new()
	g.gpu_fill = not user.has("--grass-cpu")
	g.far_rings = not user.has("--grass-norings")
	add_child(g)
	g.data_map = g.get_path_to(dm)
	var times := []
	for i in 600:
		var q := Vector3(-100.0 + i * grass_walk, 0, -20)
		var t := Time.get_ticks_usec()
		g.update_at(q)
		times.append((Time.get_ticks_usec() - t) / 1000.0)
		await get_tree().process_frame
	var tt: Array = times.slice(1)
	tt.sort()
	var sum := 0.0
	for x in tt:
		sum += x
	var n := tt.size()
	print("GRASS_WALK %.3f m/frame: first %.1f ms, then mean %.2f, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms; over 2 ms %d frames, over 5.7 ms %d; tiles %d, ring tiles %d" % [grass_walk, times[0], sum / n, tt[n / 2], tt[n * 95 / 100], tt[n * 99 / 100], tt[n - 1], tt.filter(func(x): return x > 2.0).size(), tt.filter(func(x): return x > 5.7).size(), g.get_tile_count(), g.get_ring_tile_count()])

# A stand-in for RockField3D.get_obstacles(): the stones whose centre is in the rect (half-open, as Rect2.has_point),
# of the size or more. Counts the calls and the widest rect asked for.
class MockRocks extends Node3D:
	var stones := PackedVector4Array()
	var sizes := PackedFloat32Array()
	var calls := 0
	var widest := 0.0
	func get_obstacles(rect: Rect2, min_size: float = 0.25) -> PackedVector4Array:
		calls += 1
		widest = maxf(widest, maxf(rect.size.x, rect.size.y))
		var got := PackedVector4Array()
		for i in stones.size():
			var o := stones[i]
			if sizes[i] >= min_size and rect.has_point(Vector2(o.x, o.z)):
				got.append(o)
		return got

func _sorted(a: PackedVector3Array) -> Array:
	var l := Array(a)
	l.sort_custom(func(p: Vector3, q: Vector3) -> bool: return p.x < q.x or (p.x == q.x and (p.z < q.z or (p.z == q.z and p.y < q.y))))
	return l

func _grass_blades(g: GrassField3D) -> Array:
	var l := []
	for ch in g.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var buf := RenderingServer.multimesh_get_buffer(ch.multimesh.get_rid())
			var stride := 12 + (4 if ch.multimesh.use_colors else 0) + (4 if ch.multimesh.use_custom_data else 0)
			for b in range(0, buf.size(), stride):
				if not (buf[b] == 0.0 and buf[b + 2] == 0.0):
					l.append(Vector3(buf[b + 3], buf[b + 7], buf[b + 11]))
	l.sort_custom(func(p: Vector3, q: Vector3) -> bool: return p.x < q.x or (p.x == q.x and (p.z < q.z or (p.z == q.z and p.y < q.y))))
	return l

# rock_field (VegetationField3D, GrassField3D): the stones fetched round each tile as it is placed give exactly the
# placements of the world-wide obstacles array: populated, streamed, and the sward's near and far ring blades.
# Deer beds (GrassField3D.deer_beds, deer_beds.h): roe-sized (about 1 m) and red-sized (about 1.5 m) ovals, a few
# square metres of flattened grass a hectare; a fresh bed is standing up again three days on.
func _deer_bed_check() -> void:
	var area := 0.0
	var lengths := {}
	var fresh := 0
	var lower := 0
	var step := 0.5
	for k in 2:
		var day := 190.0 + 3.0 * k
		for zi in 512:
			for xi in 512:
				var p := Vector3(xi * step - 128.0, 0.0, zi * step - 128.0)
				var b: Vector4 = GrassField3D.get_deer_bed_at(p, day)
				if b.x <= 0.0:
					continue
				if k == 0:
					area += step * step * b.x
					lengths[snappedf(b.w, 0.01)] = true
				if b.x > 0.9:
					fresh += 1
					if GrassField3D.get_deer_bed_at(p, day + 3.0).x < b.x - 0.1:
						lower += 1
	var per_ha := area / 6.5536
	var ls: Array = lengths.keys()
	ls.sort()
	_check("deer beds: a few square metres of flattened grass a hectare", per_ha > 0.5 and per_ha < 15.0, per_ha)
	_check("deer beds: 0.9 to 1.7 m long (roe and red deer)", not ls.is_empty() and ls[0] >= 0.89 and ls[-1] <= 1.71, ls)
	_check("deer beds: a fresh bed stands up again over the next days (unless another bed overlaps it)", fresh > 0 and lower >= fresh * 0.9, [fresh, lower])

# Terracettes (WorldContracts::terracette_at through GrassField3D.get_terracette_at): none on a 15 degree slope; on 35
# degrees, treads about 3.4 m apart along the slope (counted down a fall line), each a fifth of the step or so.
func _terracette_check() -> void:
	var gentle := 0
	for i in 400:
		var z := i * 0.1
		if GrassField3D.get_terracette_at(Vector3(3.0, z * tan(deg_to_rad(15.0)), z), Vector2(0, tan(deg_to_rad(15.0)))).x > 0.0:
			gentle += 1
	var g := Vector2(0, tan(deg_to_rad(35.0)))
	var spacings := []
	var tread_len := 0.0
	var steps := 0
	for xi in 6:
		var x := 30.0 * xi + 7.0
		var last := -1e9
		var inside := false
		for i in 4000:
			var s := i * 0.02 # along the slope, m
			var p := Vector3(x, s * sin(deg_to_rad(35.0)), s * cos(deg_to_rad(35.0)))
			var t: Vector3 = GrassField3D.get_terracette_at(p, g)
			if t.y < 0.1 and t.z > 0.0 and not inside:
				inside = true
				if last > -1e8:
					spacings.append(s - last)
				last = s
				steps += 1
			elif t.y > 0.5:
				inside = false
			if t.x > 0.5:
				tread_len += 0.02
	spacings.sort()
	var med: float = spacings[spacings.size() / 2] if not spacings.is_empty() else 0.0
	_check("terracettes: none on a 15 degree slope", gentle == 0, gentle)
	_check("terracettes: steps about 3.4 m apart along a 35 degree slope", med > 3.0 and med < 3.8, [med, spacings.size()])
	_check("terracettes: treads a fifth of the step or so (where they show)", steps > 0 and tread_len / steps > 0.2 and tread_len / steps < 0.9, tread_len / maxf(steps, 1))

# Drylands (VegetationField3D.climate_bare, WorldContracts::bare_patch): under a 300 mm climate the cover plants keep to
# the clumps, about the share the rain leaves covered (0.9 * (1 - smoothstep(200, 700, 300)) = 0.81 bare); trees stay;
# a 900 mm climate leaves the placements as they were; climate_bare off, as without a climate.
# A streamed cover field with the cattle set, as the landscape's pasture: the grazing disc (WorldStand.add_browse)
# comes after the field has streamed, and the pats and roughs must appear inside it once the tiles settle.
func _grazing_stream_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_browse()
	var cattle: Array = PlantGenerator.make_grazing(PlantGenerator.GRAZE_CATTLE, 1, false)
	var b := Biome.new()
	b.species.assign(cattle)
	var anchor := Node3D.new()
	add_child(anchor)
	anchor.global_position = Vector3(-40, 0, -40)
	var f := VegetationField3D.new()
	f.far_volume = false
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = b
	f.tile_size = 8.0
	f.stream_radius = 80.0
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.stream_target = f.get_path_to(anchor)
	f.streaming = true
	var waited := 0
	while waited < 600 and (waited < 10 or not f.is_stream_idle()):
		await get_tree().process_frame
		waited += 1
	var before := 0
	for s in cattle.size():
		before += f.get_instances_near(s, anchor.global_position, 20.0).size()
	st.add_browse(anchor.global_position, 30.0, 0.9, 1.5)
	waited = 0
	var t0 := Time.get_ticks_msec()
	while waited < 900 and (Time.get_ticks_msec() - t0 < 1500 or f.get_stream_pending() > 0 or not f.is_stream_idle()): # the stand is looked at twice a second
		await get_tree().process_frame
		waited += 1
	var kinds := {}
	var after := 0
	for s in cattle.size():
		var n: int = f.get_instances_near(s, anchor.global_position, 20.0).size()
		after += n
		kinds[int((cattle[s] as PlantSpecies).get_meta("plant_preset", -1))] = kinds.get(int((cattle[s] as PlantSpecies).get_meta("plant_preset", -1)), 0) + n
	f.queue_free()
	anchor.queue_free()
	st.clear_browse()
	await get_tree().process_frame
	# About one standing pat per 10 m2 (MacDiarmid and Watkin 1972): 60 to 250 in the 1257 m2 counted.
	var pats: int = kinds.get(PlantGenerator.PRESET_COW_PAT, 0)
	_check("grazing: a pasture disc added after a streamed field has settled brings its pats (about one per 10 m2) and roughs (none before)", before == 0 and pats >= 60 and pats <= 250 and kinds.get(PlantGenerator.PRESET_DUNG_ROUGH, 0) > 10, [before, after, kinds, waited])

func _climate_bare_check() -> void:
	var counts := {}
	var b := Biome.new()
	var tree: PlantSpecies = (field.biome.species[0] as PlantSpecies).duplicate()
	tree.canopy_radius = 3.0
	b.species.assign([tree] + Array(PlantGenerator.make_flowering_grasses(1, false)))
	for mm in [300.0, 900.0]:
		var wm := WeatherMap3D.new()
		var rc := ClimateRecipe.new()
		rc.precipitation = mm
		wm.climate = rc
		add_child(wm)
		await get_tree().process_frame
		for on in [true, false]:
			var f := VegetationField3D.new()
			f.far_volume = false
			f.populate_on_ready = false
			f.bark_detail = false
			f.biome = b
			f.climate_bare = on
			add_child(f)
			f.data_map = f.get_path_to(dm)
			f.populate()
			var cover := 0
			var trees := 0
			for s in f.biome.species.size():
				var n: int = f.get_placements(s).size()
				if (f.biome.species[s] as PlantSpecies).canopy_radius > 0.0:
					trees += n
				else:
					cover += n
			counts["%d_%s" % [int(mm), "on" if on else "off"]] = [cover, trees]
			f.free()
		wm.free()
		await get_tree().process_frame
	var dry_on: Array = counts["300_on"]
	var dry_off: Array = counts["300_off"]
	var kept := float(dry_on[0]) / maxf(float(dry_off[0]), 1.0)
	_check("climate_bare: a 300 mm climate keeps the cover plants to the clumps (about 0.19 of them), trees stay", kept > 0.08 and kept < 0.4 and dry_on[1] == dry_off[1], [kept, counts])
	_check("climate_bare: a 900 mm climate leaves them as they were", counts["900_on"] == counts["900_off"], counts)

func _rock_field_check() -> void:
	var rocks := MockRocks.new()
	rocks.name = "MockRocks"
	add_child(rocks)
	var rng := RandomNumberGenerator.new()
	rng.seed = 41
	var half := size * 0.5
	for i in 2500:
		var x := rng.randf_range(-half, half)
		var z := rng.randf_range(-half, half)
		var big := rng.randf() < 0.02
		var r := rng.randf_range(3.0, 9.0) if big else rng.randf_range(0.1, 1.4)
		rocks.stones.append(Vector4(x, dm.get_height_at(Vector3(x, 0, z)), z, r))
		rocks.sizes.append(r * 1.6)
	var everything: PackedVector4Array = rocks.get_obstacles(Rect2(-4096, -4096, 8192, 8192), 0.25)
	var b := Biome.new()
	var list: Array = field.biome.species.duplicate()
	list.append_array(PlantGenerator.make_stones(PlantGenerator.STONES_SCREE, 2, false))
	b.species.assign(list)
	var res := []
	var ok := true
	# Populated: one field reads the array, the other the rock field.
	var fs := []
	for mode in 2:
		var f := VegetationField3D.new()
		f.far_volume = false
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = b
		add_child(f)
		f.data_map = f.get_path_to(dm)
		if mode == 0:
			f.obstacles = everything
		else:
			f.rock_field = f.get_path_to(rocks)
		f.populate()
		fs.append(f)
	var placed := 0
	var bare := VegetationField3D.new() # No rocks: the rocks must change the placements, or the check proves nothing.
	bare.far_volume = false
	bare.populate_on_ready = false
	bare.bark_detail = false
	bare.biome = b
	add_child(bare)
	bare.data_map = bare.get_path_to(dm)
	bare.populate()
	var differs := false
	for s in b.species.size():
		var pa: Array = _sorted(fs[0].get_placements(s))
		var pb: Array = _sorted(fs[1].get_placements(s))
		placed += pa.size()
		ok = ok and pa == pb
		differs = differs or pa != _sorted(bare.get_placements(s))
	bare.free()
	ok = ok and differs
	res.append(["populate", placed, ok, differs])
	for f in fs:
		f.free()
	# Streamed round a point: the tiles fetch only the rocks round them.
	var target := Node3D.new()
	add_child(target)
	target.global_position = Vector3(30, 0, -20)
	fs = []
	rocks.calls = 0
	rocks.widest = 0.0
	for mode in 2:
		var f := VegetationField3D.new()
		f.far_volume = false
		f.populate_on_ready = false
		f.bark_detail = false
		f.biome = b
		add_child(f)
		f.data_map = f.get_path_to(dm)
		if mode == 0:
			f.obstacles = everything
		else:
			f.rock_field = f.get_path_to(rocks)
		f.stream_target = f.get_path_to(target)
		f.stream_radius = 40.0
		f.streaming = true
		fs.append(f)
	var frames := [await _stream_wait(fs[0]), await _stream_wait(fs[1])]
	var same_stream: bool = frames[0] >= 0 and frames[1] >= 0
	placed = 0
	for s in b.species.size():
		var pa: Array = _sorted(fs[0].get_placements(s))
		var pb: Array = _sorted(fs[1].get_placements(s))
		placed += pa.size()
		same_stream = same_stream and pa == pb
	var drift_same := true
	for i in 30:
		var q := Vector3(30 + rng.randf_range(-20, 20), 0, -20 + rng.randf_range(-20, 20))
		drift_same = drift_same and absf(fs[0].get_litter_drift_at(q) - fs[1].get_litter_drift_at(q)) < 1e-6
	res.append(["stream", placed, same_stream, drift_same, rocks.calls, rocks.widest, fs[1].get_rock_chunk_count()])
	ok = ok and same_stream and drift_same and rocks.widest < size
	for f in fs:
		f.free()
	target.free()
	# The sward: near tiles and far ring tiles.
	var gs := []
	rocks.calls = 0
	for mode in 2:
		var g := GrassField3D.new()
		g.radius = 16.0
		g.far_shell = false
		g.rules = PlantSpecies.new()
		add_child(g)
		g.data_map = g.get_path_to(dm)
		if mode == 0:
			g.obstacles = everything
		else:
			g.rock_field = g.get_path_to(rocks)
		for i in 6:
			g.update_at(Vector3(-20, 0, 25))
			await get_tree().process_frame
		gs.append(g)
	var ga: Array = _grass_blades(gs[0])
	var gb: Array = _grass_blades(gs[1])
	var grass_same: bool = ga.size() > 1000 and ga == gb and gs[0].get_ring_blade_count() == gs[1].get_ring_blade_count()
	res.append(["grass", ga.size(), gs[0].get_ring_blade_count(), grass_same, rocks.calls])
	ok = ok and grass_same
	for g in gs:
		g.free()
	rocks.free()
	_check("rock_field: rocks fetched round each tile place exactly as the world-wide obstacles array (populated, streamed, litter drift, sward near and far rings)", ok, res)

func _blowing_checks() -> void:
	if not ClassDB.class_exists("LeafFall3D"):
		return
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 1024.0
	m.resolution = 64
	m.wind_direction = Vector2(1, 0)
	m.roughness_length = 0.03
	m.transition_time = 0.0
	m.gust_intensity = 0.0
	m.gust_patch_strength = 0.0
	m.wind_speed = 2.0
	add_child(m)
	# On dry ground: the bench's pond would float them (LeafFall3D's water branch).
	var lf: Node3D = null
	var quad := QuadMesh.new()
	quad.size = Vector2(0.08, 0.05)
	quad.orientation = PlaneMesh.FACE_Y
	for c in [Vector2(40, 40), Vector2(-40, 40), Vector2(40, -40), Vector2(-40, -40), Vector2(80, 0), Vector2(0, 80)]:
		lf = ClassDB.instantiate("LeafFall3D")
		lf.auto_crowns = false
		lf.field_litter = 0.0
		add_child(lf)
		lf.add_kind(quad, 0.08, 0.003, 0.00025, 1.0)
		await get_tree().physics_frame
		for i in 100:
			lf.drop_leaf(Vector3(c.x + randf_range(-3, 3), -1000.0, c.y + randf_range(-3, 3)), 0) # under the ground: set on it at once
		for i in 120:
			await get_tree().physics_frame
		if lf.get_floating_count() == 0:
			break
		lf.queue_free()
		lf = null
	if lf == null:
		_check("blowing leaves: a dry spot for the check", false, [])
		m.queue_free()
		return
	var mean_x := func() -> float:
		var sx := 0.0
		var st: PackedVector4Array = lf.get_leaf_states()
		for v in st:
			sx += v.x
		return sx / maxf(st.size(), 1)
	var calm_slide: int = lf.get_sliding_count()
	var x0: float = mean_x.call()
	var lifted0: int = lf.get_lifted_count()
	m.wind_speed = 12.0
	m.snap_to_settings()
	var most := 0
	for i in 180:
		await get_tree().physics_frame
		most = maxi(most, lf.get_sliding_count())
	var moved: float = mean_x.call() - x0
	var lifted: int = lf.get_lifted_count() - lifted0
	_check("blowing leaves: still in light air, sliding downwind past the creep speed, few lifted under the lift speed",
			calm_slide == 0 and most > 30 and moved > 0.25 and lifted < 10, [calm_slide, most, moved, lifted, lf.get_crept_count()])
	lf.queue_free()
	# The field's own litter: under and round its crowns the wind sets leaves sliding off it.
	var wood := VegetationField3D.new()
	wood.populate_on_ready = false
	wood.bark_detail = false
	var tsp := PlantSpecies.new()
	tsp.meshes = [BoxMesh.new()]
	tsp.spacing = 6.0
	tsp.canopy_radius = 3.0
	tsp.canopy_opacity = 0.9
	wood.biome = Biome.new()
	wood.biome.species.assign([tsp])
	add_child(wood)
	wood.data_map = wood.get_path_to(dm)
	wood.populate()
	var t0: Vector3 = wood.get_placements(0)[0] if wood.get_placements(0).size() > 0 else Vector3()
	var lit: float = wood.get_litter_at(t0)
	var lf2: Node3D = ClassDB.instantiate("LeafFall3D")
	lf2.auto_crowns = false
	lf2.field_litter = 40.0
	add_child(lf2)
	lf2.vegetation_field = lf2.get_path_to(wood)
	lf2.add_kind(quad, 0.08, 0.003, 0.00025, 1.0)
	lf2.global_position = Vector3(t0.x, 0, t0.z)
	var cam := Camera3D.new()
	add_child(cam)
	cam.global_position = Vector3(t0.x, t0.y + 1.7, t0.z)
	cam.current = true
	for i in 240:
		await get_tree().physics_frame
	var from_litter: int = lf2.get_crept_count() + lf2.get_lifted_count()
	_check("blowing leaves: the wind takes leaves off the field's litter round its crowns", lit > 0.3 and from_litter > 5, [lit, lf2.get_crept_count(), lf2.get_lifted_count()])
	lf2.queue_free()
	cam.queue_free()
	wood.queue_free()
	m.queue_free()
	await get_tree().physics_frame

func _webs_check() -> void:
	var autumn := PlantGenerator.make_dew_webs(1, false, {"day_of_year": 270})
	var spring := PlantGenerator.make_dew_webs(1, false, {"day_of_year": 120})
	var winter := PlantGenerator.make_dew_webs(1, false, {"day_of_year": 15})
	var silk := true
	var orb_r := 0.0
	var sheet_h := 0.0
	var orb_h := 0.0
	for sp in autumn:
		var ps := sp as PlantSpecies
		var pre: int = ps.get_meta("plant_preset", -1)
		for m in ps.meshes:
			var found := false
			for s in m.get_surface_count():
				var mat := m.surface_get_material(s) as ShaderMaterial
				if mat and mat.shader and mat.shader.code.contains("#define PLANT_SILK"):
					found = true
					if pre == PlantGenerator.PRESET_ORB_WEB:
						orb_r = max(orb_r, (mat.get_shader_parameter("silk_orb") as Vector4).x)
			silk = silk and found
			if pre == PlantGenerator.PRESET_SHEET_WEB:
				sheet_h = max(sheet_h, m.get_aabb().end.y)
			else:
				orb_h = max(orb_h, m.get_aabb().end.y)
	var d := func(set: Array, i: int) -> float: return (set[i] as PlantSpecies).get_density()
	_check("dew webs: silk shader on every web, sheets low in the sward, orbs on stems; orbs only late summer and autumn, none in winter",
			silk and orb_r > 0.05 and sheet_h < 0.4 and orb_h > 0.4 and d.call(autumn, 0) > 0.5 and d.call(autumn, 1) > 0.3 and d.call(spring, 1) == 0.0 and d.call(spring, 0) > 0.0 and d.call(winter, 0) == 0.0,
			[silk, orb_r, sheet_h, orb_h, d.call(autumn, 0), d.call(autumn, 1), d.call(spring, 0), d.call(spring, 1), d.call(winter, 0)])

func _mounds_check() -> void:
	var meadow := PlantGenerator.make_mounds(PlantGenerator.MOUNDS_MEADOW, 1, false)
	var wood := PlantGenerator.make_mounds(PlantGenerator.MOUNDS_WOOD, 1, false)
	var tagged := true
	var heights := {}
	var ant_turns := true
	for sp in meadow + wood:
		var ps := sp as PlantSpecies
		tagged = tagged and ps.has_meta("sward_mound") and float(ps.get_meta("sward_mound")) > 0.1
		var m: Mesh = ps.meshes[0]
		var pre: int = ps.get_meta("plant_preset", -1)
		if pre == PlantGenerator.PRESET_ANTHILL:
			ant_turns = ps.random_yaw
		if pre == PlantGenerator.PRESET_MOLEHILL and ps.has_meta("mound_age"):
			heights[float(ps.get_meta("mound_age"))] = m.get_aabb().end.y
	var ages := heights.keys()
	ages.sort()
	var slumps: bool = ages.size() >= 3 and heights[ages[0]] > heights[ages[ages.size() - 1]] * 1.15
	var nest: PlantSpecies = wood[0]
	var edge := nest.get_rule_range(PlantSpecies.RULE_FOREST_EDGE)
	_check("mounds: every species has its sward_mound radius, ant hills keep their crest east, molehills slump with age, wood ant nests at the wood's edge",
			tagged and not ant_turns and slumps and edge.x < 0.0 and edge.y <= 60.0 and nest.get_rule_range(PlantSpecies.RULE_CONIFER).x >= 0.3, [tagged, ant_turns, heights, edge])

func _rooting_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	st.clear_browse()
	var c := Vector2(-30, 30)
	var herb := PlantSpecies.new()
	herb.meshes = [BoxMesh.new()]
	herb.spacing = 1.0
	herb.set_rule_range(PlantSpecies.RULE_ROOTING, Vector2(0.8, 10000.0))
	herb.set_rule_fade(PlantSpecies.RULE_ROOTING, 0.6)
	var sod := PlantSpecies.new()
	sod.meshes = [BoxMesh.new()]
	sod.spacing = 0.8
	sod.set_rule_range(PlantSpecies.RULE_ROOTING, Vector2(0.0, 0.6))
	sod.set_rule_fade(PlantSpecies.RULE_ROOTING, 0.1)
	var sps := [herb, sod]
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign(sps)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	var count := func() -> Array:
		var on := [0, 0]
		var off := [0, 0]
		for ch in f.get_children(true):
			if ch is MultiMeshInstance3D and ch.multimesh:
				var mm: MultiMesh = ch.multimesh
				var k := -1
				for j in sps.size():
					if sps[j].meshes[0] == mm.mesh:
						k = j
				if k < 0:
					continue
				for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
					var q: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
					var d := Vector2(q.x, q.z).distance_to(c)
					if d < 6.0:
						on[k] += 1
					elif d > 12.0 and d < 25.0:
						off[k] += 1
		return [on, off]
	var id := st.add_disturbance(Vector3(c.x, 0, c.y), 9.0, 5, 0.05)
	f.populate()
	var fresh: Array = count.call()
	st.clear_disturbances()
	st.add_disturbance(Vector3(c.x, 0, c.y), 9.0, 5, 2.0)
	f.populate()
	var old: Array = count.call()
	f.free()
	st.clear_disturbances()
	_check("boar rooting: sods and no field layer on a fresh patch, the reverse round it; two years on the field layer is back and the sods gone",
			fresh[0][0] == 0 and fresh[0][1] > 10 and fresh[1][0] > 10 and fresh[1][1] == 0 and old[0][0] > 10 and old[0][1] == 0, [fresh, old, id])
	var fl := PlantGenerator.make_floor(PlantGenerator.FLOOR_OAK_MIXED, 1, false)
	var gone := 0
	var back := 0
	var herbs := 0
	for sp in fl:
		if (sp as PlantSpecies).rooting_range == Vector2(-1, 10000):
			continue
		herbs += 1
		var r: Vector2 = sp.rooting_range
		if r.x > 0.2:
			gone += 1
		if r.x + sp.rooting_fade < 3.0:
			back += 1
	var rs := PlantGenerator.make_rooting(PlantGenerator.ROOTING_WOOD, 1, false)
	_check("boar rooting: the floor's field layer leaves fresh rooting and comes back within three years; the wood set has sods and ruderals",
			herbs > 5 and gone == herbs and back == herbs and rs.size() >= 5, [herbs, gone, back, rs.size()])

func _browse_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	st.clear_browse()
	var c := Vector2(30, 30)
	st.add_browse(Vector3(c.x, 0, c.y), 12.0, 0.9, 1.8)
	var full := PlantSpecies.new()
	full.meshes = [BoxMesh.new()]
	full.spacing = 1.2
	full.set_rule_range(PlantSpecies.RULE_BROWSE, Vector2(0.0, 0.35))
	full.set_rule_fade(PlantSpecies.RULE_BROWSE, 0.15)
	var bitten := PlantSpecies.new()
	bitten.meshes = [BoxMesh.new()]
	bitten.spacing = 1.2
	bitten.set_rule_range(PlantSpecies.RULE_BROWSE, Vector2(0.35, 1.0))
	bitten.set_rule_fade(PlantSpecies.RULE_BROWSE, 0.15)
	var lawn := PlantSpecies.new()
	lawn.meshes = [BoxMesh.new()]
	lawn.spacing = 0.8
	lawn.set_rule_range(PlantSpecies.RULE_BROWSE, Vector2(0.45, 1.0))
	lawn.set_rule_fade(PlantSpecies.RULE_BROWSE, 0.2)
	var sps := [full, bitten, lawn]
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species.assign(sps)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var on := [0, 0, 0]
	var off := [0, 0, 0]
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var mm: MultiMesh = ch.multimesh
			var k := -1
			for j in sps.size():
				if sps[j].meshes[0] == mm.mesh:
					k = j
			if k < 0:
				continue
			for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
				var q: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
				var d := Vector2(q.x, q.z).distance_to(c)
				if d < 8.0:
					on[k] += 1
				elif d > 15.0 and d < 30.0:
					off[k] += 1
	f.free()
	st.clear_browse()
	_check("deer browsing: bitten bramble and the deer lawn inside a heavy browse disc, whole bramble and no lawn round it", on[0] == 0 and on[1] > 10 and on[2] > 3 and off[0] > 10 and off[1] == 0 and off[2] == 0, [on, off])
	var fl := PlantGenerator.make_floor(PlantGenerator.FLOOR_OAK_MIXED, 1, false)
	var found := {}
	var bad := []
	for sp in fl:
		var p: int = sp.get_meta("plant_preset", -1)
		var w0: float = sp.rule_weight(PlantSpecies.RULE_BROWSE, 0.0)
		var w9: float = sp.rule_weight(PlantSpecies.RULE_BROWSE, 0.9)
		if p == PlantGenerator.PRESET_GRAZED_LAWN:
			found["lawn"] = w0 == 0.0 and w9 > 0.9
		if p == PlantGenerator.PRESET_BRAMBLE:
			if sp.has_meta("browsed"):
				found["bitten"] = w0 == 0.0 and w9 > 0.9
			elif w9 > 0.0:
				bad.append(["whole bramble at 0.9", w9])
		if p == PlantGenerator.PRESET_WOOD_ANEMONE and not (w0 == 1.0 and w9 < 0.5):
			bad.append(["anemone", w0, w9])
	_check("deer browsing: the oak floor swaps bramble for bitten bramble, adds the lawn, thins anemone", found.get("lawn", false) and found.get("bitten", false) and bad.is_empty(), [found, bad])

# Forest edges (RULE_FOREST_EDGE, trees' make_edge meta): a forest of an old stand round a 25 m windthrow clearing.
# The mantle (meta edge_distance -8 to 3 m) rings the clearing's edge; edge trees (0 to 10 m in, mesh meta
# tree_open_side +X) stand just inside the forest with +X towards the clearing.
func _edge_check() -> void:
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	var c := Vector2(40, 40)
	st.add_disturbance(Vector3(c.x, 0, c.y), 25.0, WorldStand.DISTURBANCE_WIND, 5.0)
	var forest := PlantSpecies.new()
	forest.meshes = [BoxMesh.new()]
	forest.spacing = 4.0
	forest.canopy_radius = 2.0
	forest.set_meta("succession_age", Vector2(50, 10000))
	var mantle := PlantSpecies.new()
	mantle.meshes = [BoxMesh.new()]
	mantle.spacing = 1.5
	mantle.set_meta("edge_distance", Vector2(-8, 3))
	var edge_mesh := BoxMesh.new()
	edge_mesh.set_meta("tree_open_side", Vector3(1, 0, 0))
	var edge := PlantSpecies.new()
	edge.meshes = [edge_mesh]
	edge.spacing = 3.0
	edge.set_meta("edge_distance", Vector2(0, 10))
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species = [forest, mantle, edge]
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var m_in := 0
	var m_out := 0
	var e_in := 0
	var e_out := 0
	var e_facing := 0
	var forest_in := 0
	for ch in f.get_children(true):
		if not (ch is MultiMeshInstance3D and ch.multimesh):
			continue
		var mm: MultiMesh = ch.multimesh
		for i in mm.instance_count:
			var t: Transform3D = (ch as Node3D).global_transform * mm.get_instance_transform(i)
			var r := Vector2(t.origin.x, t.origin.z).distance_to(c)
			if r > 36.0:
				continue # Past it the bench's ground has gaps of its own (more edges).
			if mm.mesh == mantle.meshes[0]:
				if r > 15.0 and r < 31.0:
					m_in += 1
				else:
					m_out += 1
			elif mm.mesh == edge_mesh:
				if r > 23.0:
					e_in += 1
					var to_c := (c - Vector2(t.origin.x, t.origin.z)).normalized()
					if Vector2(t.basis.x.x, t.basis.x.z).normalized().dot(to_c) > 0.85:
						e_facing += 1
				else:
					e_out += 1
			elif mm.mesh == forest.meshes[0] and r < 24.0:
				forest_in += 1
	# The floor under it as its own field (canopy_field): the edge grass of make_floor reads the forest's edge.
	var fl := VegetationField3D.new()
	fl.populate_on_ready = false
	fl.bark_detail = false
	fl.biome = Biome.new()
	var fsp := []
	for sp in PlantGenerator.make_floor(PlantGenerator.FLOOR_OAK_MIXED, 1, false):
		if sp.get_rule_range(PlantSpecies.RULE_FOREST_EDGE).y < 10000.0:
			fsp.append(sp)
	fl.biome.species = fsp
	add_child(fl)
	fl.data_map = fl.get_path_to(dm)
	fl.canopy_field = fl.get_path_to(f)
	fl.populate()
	var g_band := 0
	var g_off := 0
	for ch in fl.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var p: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
				var rr := Vector2(p.x, p.z).distance_to(c)
				if rr > 36.0:
					continue
				if rr > 6.0:
					g_band += 1
				else:
					g_off += 1
	# The sward under them (GrassField3D.undergrowth_field reads sward_at): thinned under the edge brambles, not out in
	# the clearing where they do not grow.
	var sw_edge := 0.0
	var sw_clear := 0.0
	for k in 72:
		var a := k * TAU / 72.0
		sw_edge = maxf(sw_edge, fl.get_sward_shade_at(Vector3(c.x + cos(a) * 25.0, 0, c.y + sin(a) * 25.0)))
		sw_clear = maxf(sw_clear, fl.get_sward_shade_at(Vector3(c.x + cos(a) * 5.0, 0, c.y + sin(a) * 5.0)))
	_check("forest edge: the sward thins under edge brambles, not in the open clearing", sw_edge > 0.2 and sw_clear == 0.0, [snappedf(sw_edge, 0.01), sw_clear])
	fl.queue_free()
	_check("forest edge: a floor field's edge plants follow its canopy field's edge (none deep in the clearing, past their fade)", fsp.size() >= 3 and g_band > 50 and g_off == 0, [fsp.size(), g_band, g_off])
	st.clear_disturbances()
	f.queue_free()
	var r := [m_in, m_out, e_in, e_out, e_facing, forest_in]
	_check("forest edge: the mantle rings the clearing's edge, edge trees stand just inside facing the open", m_in > 20 and m_out == 0 and e_in > 10 and e_out == 0 and e_facing >= e_in * 0.9 and forest_in == 0, r)

# Stilt-rooted trees (mesh meta tree_base_lift, trees' alder carr): the origin raised that far above the ground.
func _base_lift_check() -> void:
	var sps := []
	for lift in [0.0, 0.6]:
		var m := BoxMesh.new()
		if lift > 0.0:
			m.set_meta("tree_base_lift", lift)
		var sp := PlantSpecies.new()
		sp.meshes = [m]
		sp.spacing = 3.0
		sp.align_to_ground = 0.0
		sp.scale_range = Vector2(1, 1)
		sps.append(sp)
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.bark_detail = false
	f.biome = Biome.new()
	f.biome.species = sps
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var dy := [[], []]
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var mm: MultiMesh = ch.multimesh
			var k := 0 if mm.mesh == sps[0].meshes[0] else (1 if mm.mesh == sps[1].meshes[0] else -1)
			if k < 0:
				continue
			for i in mini(mm.instance_count, 200):
				var p: Vector3 = (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
				dy[k].append(p.y - dm.get_height_at(p))
	f.queue_free()
	var mean := [0.0, 0.0]
	for k in 2:
		for v in dy[k]:
			mean[k] += v / maxf(dy[k].size(), 1)
	_check("stilt roots: tree_base_lift raises the trunk's foot that far over the ground", dy[0].size() > 20 and dy[1].size() > 20 and absf(mean[1] - mean[0] - 0.6) < 0.03, [dy[0].size(), dy[1].size(), snappedf(mean[0], 0.001), snappedf(mean[1], 0.001)])

# --stand=live: a disturbance reported after the field streamed in (WorldStand.stand_changed) re-places the tiles over
# it; a year passing inside every stage changes nothing; past the pioneer stage they go.
func _stand_live() -> void:
	field.queue_free()
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	var sps := _stand_species()
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.biome = Biome.new()
	f.biome.species = sps
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.stream_radius = 40.0
	f.stream_target = f.get_path_to(self)
	f.streaming = true
	var counts := []
	await _burn_live_wait(3.0)
	counts.append(_stand_count(f, sps, true))
	st.add_disturbance(Vector3.ZERO, 10.0, WorldStand.DISTURBANCE_WIND, 2.0)
	await _burn_live_wait(3.0)
	counts.append(_stand_count(f, sps, true))
	var mmis := []
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D:
			mmis.append(ch)
	st.advance_years(1.0)
	await _burn_live_wait(3.0)
	var same := true
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D and not mmis.has(ch):
			same = false
	counts.append(_stand_count(f, sps, true))
	st.advance_years(60.0)
	await _burn_live_wait(3.0)
	counts.append(_stand_count(f, sps, true))
	st.clear_disturbances()
	var c0: Array = counts[0]
	var c1: Array = counts[1]
	var c3: Array = counts[3]
	_check("stand: a live windthrow re-places the streamed tiles over it (old stand out, pioneers and legacy in)", c0[3] > 3 and c0[0] == 0 and c1[3] == 0 and c1[0] > 3 and c1[1] > 1 and c1[2] == 0, counts)
	_check("stand: a year passing inside every stage places nothing again", same and counts[2] == counts[1], [same, counts[2]])
	_check("stand: 63 years on the old stand is back, the pioneers gone", c3[3] > 3 and c3[0] == 0 and c3[1] == 0, c3)
	print("VEG_STAND_LIVE_RESULT ", "PASS" if fails == 0 else "FAIL")
	get_tree().quit(1 if fails > 0 else 0)

# Close-up bark on by default: a field whose species have trunks (tree_colliders) makes and links a BarkDetail3D
# as an internal child; none for a field without trunks, none with bark_detail off, gone when switched off.
func _bark_detail_check() -> void:
	var made := []
	for k in 3:
		var m := BoxMesh.new()
		if k != 1:
			m.set_meta("tree_colliders", PackedFloat32Array())
		var sp := PlantSpecies.new()
		sp.meshes = [m]
		var f := VegetationField3D.new()
		f.populate_on_ready = false
		f.bark_detail = k != 2
		f.biome = Biome.new()
		f.biome.species = [sp]
		add_child(f)
		made.append(f)
	var bd = made[0].get_bark_detail_node()
	var r := [bd != null and bd.get_node(bd.vegetation) == made[0], made[1].get_bark_detail_node() == null, made[2].get_bark_detail_node() == null, made[0].get_child_count() == 0]
	made[0].bark_detail = false
	r.append(made[0].get_bark_detail_node() == null or made[0].get_bark_detail_node().is_queued_for_deletion())
	for f in made:
		f.queue_free()
	_check("bark detail: made and linked for a field with trunks, not without, switchable", not r.has(false), r)

# --burn=live: a burn reported after the field streamed in (WorldFire.burn_changed) re-places the tiles over it.
func _burn_live() -> void:
	field.queue_free()
	var sp := PlantSpecies.new()
	sp.meshes = [BoxMesh.new()]
	sp.spacing = 0.6
	sp.density = 0.8
	sp.burn_recovery = 1.0
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.biome = Biome.new()
	f.biome.species = [sp]
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.stream_radius = 30.0
	f.stream_target = f.get_path_to(self)
	f.streaming = true
	var counts := []
	await _burn_live_wait(3.0)
	burn_live_field = f
	burn_live_ring0 = _burn_live_count(f, true)
	burn_live_ring_min = burn_live_ring0
	counts.append(_burn_live_count(f))
	var scar := WorldBurnScar3D.new()
	scar.age_years = 0.02
	scar.size = Vector2(16, 16)
	add_child(scar)
	await _burn_live_wait(3.0)
	counts.append(_burn_live_count(f))
	scar.age_years = 0.5
	await _burn_live_wait(3.0)
	counts.append(_burn_live_count(f))
	scar.free()
	await _burn_live_wait(3.0)
	counts.append(_burn_live_count(f))
	var share := float(counts[2]) / maxf(counts[0], 1)
	_check("burn: a live burn re-places the streamed tiles over it (gone, half back at half the recovery, whole once it goes)", counts[0] > 15 and counts[1] == 0 and share > 0.3 and share < 0.7 and counts[3] == counts[0], counts)
	_check("burn: the unburnt plants beside it never leave while their tiles are replaced (fewest in any frame)", burn_live_ring0 > 15 and burn_live_ring_min == burn_live_ring0, [burn_live_ring0, burn_live_ring_min])
	print("VEG_BURN_LIVE_RESULT ", "PASS" if fails == 0 else "FAIL")
	get_tree().quit(1 if fails > 0 else 0)

var burn_live_field: VegetationField3D
var burn_live_ring0 := 0
var burn_live_ring_min := 0

func _burn_live_wait(sec: float) -> void:
	var t := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t < sec * 1000.0:
		await get_tree().process_frame
		if burn_live_field:
			burn_live_ring_min = mini(burn_live_ring_min, _burn_live_count(burn_live_field, true))

# Plants on the scar's middle 12 m, or (ring) on the unburnt ground 10 to 16 m out, on tiles the scar touches.
func _burn_live_count(f: VegetationField3D, ring := false) -> int:
	var n := 0
	for ch in f.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			for i in mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count:
				var p := (ch as Node3D).global_transform * mm.get_instance_transform(i).origin
				var m := maxf(absf(p.x), absf(p.z))
				if (m >= 10.0 and m < 16.0) if ring else (absf(p.x) < 6 and absf(p.z) < 6):
					n += 1
	return n

# --burn[=off]: dry heath with burn scars of four ages (WorldBurnScar3D, the fire contract's burn_age_at) and
# make_burn's heath set, streamed round a camera looking across the scars.
func _burn_view() -> void:
	field.queue_free()
	if burn_view == 1:
		for s in BURN_SCARS:
			var scar := WorldBurnScar3D.new()
			scar.age_years = s[0]
			scar.size = Vector2(60, 30)
			scar.position = Vector3(60, 0, s[1])
			add_child(scar)
	# From the unburnt heath at the scars' near end, 2.5 m up, looking along them: the fresh burn ahead, then a
	# six-month one, then two and a half and five years.
	var at := Vector3(60, 0, -92)
	at.y = height(at.x, at.z)
	var cam := Camera3D.new()
	add_child(cam)
	cam.far = 300.0
	cam.position = at + Vector3(0, 2.5, 0)
	cam.look_at(Vector3(60, height(60, -40), -40))
	cam.current = true
	var ground := VegetationField3D.new()
	ground.far_volume = false
	ground.name = "Heath"
	ground.populate_on_ready = false
	ground.detail_reference_height = 1080
	var gb := Biome.new()
	var heath_set: Array[PlantSpecies] = []
	heath_set.assign(PlantGenerator.make_heath(PlantGenerator.HEATH_DRY, 3, true, {}, false))
	var burn_set: Array[PlantSpecies] = []
	if burn_view == 1:
		burn_set.assign(PlantGenerator.make_burn(PlantGenerator.BURN_HEATH, 3, true))
	var all: Array[PlantSpecies] = []
	# The heath first: a field places its species in order, so the recovering heather claims its ground and the burn
	# plants fill what it leaves (listed first, fireweed and charred stems kept it off five-year-old burns).
	all.assign(Array(heath_set) + Array(burn_set))
	gb.species = all
	ground.biome = gb
	ground.tile_size = 16.0
	ground.stream_radius = 90.0
	add_child(ground)
	# The checks stream round the scars' middle, so all four and the unburnt heath beyond are in.
	var mid := Node3D.new()
	add_child(mid)
	mid.position = Vector3(60, height(60, 0), 0)
	ground.stream_target = ground.get_path_to(mid)
	ground.data_map = ground.get_path_to(dm)
	var mem0 := OS.get_static_memory_usage()
	var vid0 := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)
	ground.streaming = true
	get_viewport().disable_3d = true
	var warm := 0
	while warm < 1500 and (warm < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		warm += 1
	if burn_view == 1:
		_burn_checks(ground, all)
	# The costs and the view on 60 m round the camera: software rendering of the whole 80 m is very slow.
	ground.stream_radius = 60.0
	ground.stream_target = ground.get_path_to(cam)
	warm = 0
	while warm < 1500 and (warm < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		warm += 1
	var walk := float(OS.get_environment("VEG_FLOOR_WALK")) if OS.get_environment("VEG_FLOOR_WALK") != "" else 30.0
	var ms: Array[float] = []
	var walked := 0.0
	var start := cam.position
	ground.get_stream_stats()
	while walked < walk:
		cam.position += Vector3(0, 0, 0.1)
		cam.position.y = height(cam.position.x, cam.position.z) + 2.5
		walked += 0.1
		await get_tree().process_frame
		ms.append(float(ground.get_stream_stats().frame_max_msec))
	cam.position = start
	var settle := 0
	while settle < 1500 and (settle < 20 or not ground.is_stream_idle()):
		await get_tree().process_frame
		settle += 1
	ms.sort()
	var total := 0.0
	for v in ms:
		total += v
	get_viewport().disable_3d = false
	_burn_scene(cam)
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
	print("VEG_BURN walk %.0f m in %d frames: streaming mean %.2f ms, p95 %.2f ms, worst %.2f ms" % [walk, ms.size(), total / maxi(ms.size(), 1), ms[int(ms.size() * 0.95)], ms[-1]])
	print("VEG_BURN plants %d, memory +%.1f MB, video +%.1f MB, primitives %d, draw calls %d, objects %d, %s frame %.1f ms" % [
		ground.get_plant_count(), (OS.get_static_memory_usage() - mem0) / 1048576.0,
		(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) - vid0) / 1048576.0,
		Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME), Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME), "raster" if raster else "path traced", frame_ms])
	if burn_view == 1:
		print("VEG_BURN_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
	if out.is_empty():
		get_tree().quit(1 if fails > 0 else 0)
	else:
		_render_later()

# Years since the burn at p from the bench's scars; -1 unburnt.
func _burn_age_at(p: Vector3) -> float:
	for s in BURN_SCARS:
		if absf(p.x - 60.0) <= 30.0 and absf(p.z - s[1]) <= 15.0:
			return s[0]
	return -1.0

func _burn_checks(ground: VegetationField3D, all: Array[PlantSpecies]) -> void:
	_check("burn: the scars answer the fire contract", is_equal_approx(WorldContracts.get_fire().burn_age_at(Vector3(60, 0, 10)), 2.5) and WorldContracts.get_fire().burn_age_at(Vector3(-60, 0, 10)) < 0.0, [WorldContracts.get_fire().burn_age_at(Vector3(60, 0, 10))])
	# Burn plants: each only at its own ages (its range and fade), none on unburnt ground; the scars' 1 m rims and the
	# field's 2 m age grid (nearest cell) give a little slack at the edges.
	var outside := 0
	var unburnt := 0
	var burn_plants := 0
	var by_preset := {}
	# Heather per scar (x 35 to 85) against the unburnt heath beside it (x 92 to 117, the same z), away from the edges.
	var heather := {}
	for s in all.size():
		var sp := all[s]
		var key: int = sp.get_meta("plant_preset", -1)
		var is_burn: bool = sp.get_meta("burn", false)
		var r := sp.get_rule_range(PlantSpecies.RULE_BURN_AGE)
		var f := sp.get_rule_fade(PlantSpecies.RULE_BURN_AGE)
		for p in ground.get_placements(s):
			var edge := absf(absf(p.x - 60.0) - 30.0) < 2.5 or (absf(p.x - 60.0) < 32.5 and _near_scar_edge(p.z))
			var age := _burn_age_at(p)
			if is_burn:
				burn_plants += 1
				by_preset[key] = by_preset.get(key, 0) + 1
				if edge:
					continue
				if age < 0.0:
					unburnt += 1
				elif age < r.x - f or age > r.y + f:
					outside += 1
			elif key == PlantGenerator.PRESET_HEATHER:
				for sc in BURN_SCARS:
					if absf(p.z - sc[1]) > 12.5:
						continue
					var k := str(sc[0])
					var hv: Array = heather.get(k, [0, 0])
					if absf(p.x - 60.0) < 25.0:
						hv[0] += 1
					elif p.x > 92.0 and p.x < 117.0:
						hv[1] += 1
					heather[k] = hv
	print("VEG_BURN burn plants by preset: ", by_preset, " heather by burn age (-1 unburnt): ", heather)
	_check("burn: every burn preset placed", by_preset.size() == 4 and burn_plants > 2000, [by_preset])
	_check("burn: burn plants only on burnt ground", unburnt == 0, [unburnt])
	_check("burn: each burn plant at its own ages", outside == 0, [outside])
	# Density on each scar against beside it (the scar's 50 x 25 m against 25 x 25 m): none on the fresh burn, about
	# smoothstep(0, 8, 5) = 0.65 of it at five years (heather's burn_recovery, 8 years).
	var share := {}
	for k in heather:
		var hv: Array = heather[k]
		share[k] = (hv[0] / 50.0) / maxf(hv[1] / 25.0, 1e-3)
	var fresh: float = share.get(str(0.05), 1.0)
	var old: float = share.get(str(5.0), 0.0)
	var beside: int = heather.get(str(5.0), [0, 0])[1]
	_check("burn: heather gone on the fresh burn, coming back on the old", beside > 500 and fresh < 0.05 and old > 0.4 and old < 0.9, [share])

func _near_scar_edge(z: float) -> bool:
	for s in BURN_SCARS:
		if absf(absf(z - s[1]) - 15.0) < 2.5:
			return true
	return false

# The burn view's ground (scorched under the fresh scars, ash and soil under the older ones, as the terrain's own
# burn colour would show it), sky and sun.
func _burn_scene(cam: Camera3D) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var res := 200
	var step := 0.75
	var o := Vector3(cam.position.x - 75.0, 0, cam.position.z - 10.0)
	for z in res:
		for x in res:
			var q := [Vector3(x, 0, z), Vector3(x + 1, 0, z), Vector3(x + 1, 0, z + 1), Vector3(x, 0, z + 1)]
			for i in [0, 1, 2, 0, 2, 3]:
				var v: Vector3 = o + q[i] * step
				v.y = height(v.x, v.z) - 0.02
				var age := _burn_age_at(v) if burn_view == 1 else -1.0
				var col := Color(0.16, 0.13, 0.08) # Peaty heath soil under the litter.
				if age >= 0.0:
					col = Color(0.03, 0.028, 0.026).lerp(col, smoothstep(0.0, 3.0, age))
				st.set_color(col)
				st.add_vertex(v)
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	var gm := StandardMaterial3D.new()
	gm.vertex_color_use_as_albedo = true
	gm.roughness = 0.95
	mi.material_override = gm
	add_child(mi)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.55, 0.65, 0.8)
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
	sun.rotation_degrees = Vector3(-35, 150, 0)
	sun.light_energy = 1.2
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 100.0
	add_child(sun)

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
	# Quartz sand with an iron-oxide coat (sRGB 0.72, 0.55, 0.36), tinted towards a sward where one grows (an oasis's,
	# GrassField3D.get_ground_shader_code()), as the terrain material does.
	var gm := ShaderMaterial.new()
	var gs := Shader.new()
	# Only with a sward in the scene: the meadow check's freed field leaves its ground globals behind.
	var tint := has_node("OasisSward")
	gs.code = "shader_type spatial;\n" + (GrassField3D.get_ground_shader_code() if tint else "") + """
varying vec3 wp;
void vertex() { wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	ALBEDO = __SAND__;
	ROUGHNESS = 0.95;
	SPECULAR = __SPEC__;
}
""".replace("__SAND__", "grass_ground(wp, vec3(0.477, 0.263, 0.106))" if tint else "vec3(0.477, 0.263, 0.106)").replace("__SPEC__", "grass_ground_specular(wp, 0.5)" if tint else "0.5")
	gm.shader = gs
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

# get_trunks_near (for close-up bark): a stand-in tree mesh carrying TreeGenerator's tree_colliders (a trunk of two
# capsules, then a branch elsewhere); each placed tree near the point comes back once, with its trunk in world space.
func _trunk_checks() -> void:
	var cone := CylinderMesh.new()
	cone.top_radius = 0.0
	cone.bottom_radius = 1.0
	cone.height = 6.0
	var m := _mesh(cone, 3.0, Color(0.3, 0.25, 0.2))
	m.set_meta("tree_colliders", PackedFloat32Array([0, 0, 0, 0, 2, 0, 0.3, 0, 0, 2, 0, 0, 4, 0, 0.25, 0, 0.5, 3, 0, 2, 4, 0, 0.08, 0]))
	var sp := PlantSpecies.new()
	sp.meshes = [m]
	sp.spacing = 6.0
	sp.density = 0.6
	sp.set_meta("preset", 7)
	var f := VegetationField3D.new()
	f.far_volume = false
	f.populate_on_ready = false
	f.biome = Biome.new()
	f.biome.species = [sp]
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var at := Vector3(-20, height(-20, -20), -20)
	var trunks := f.get_trunks_near(at, 15.0)
	var want := 0
	for p in f.get_placements(0):
		if p.distance_to(at) <= 15.0:
			want += 1
	var ok := trunks.size() == want and want > 0
	for t in trunks:
		var base: Vector3 = t["base"]
		var s := (t["radius_base"] as float) / 0.3
		ok = ok and base.distance_to(at) <= 15.001 and absf(base.y - height(base.x, base.z)) < 0.05 and (t["segments"] as Array).size() == 2 and absf(t["height"] - 4.0 * s) < 0.01 and t["preset"] == 7 and (t["up"] as Vector3).y > 0.99
	_check("trunks near a point: each tree once, its trunk's two capsules to the branch, in world space", ok, [trunks.size(), want])
	f.queue_free()

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
	_bark_detail_check()
	_fire_variant_check()
	_stand_check()
	_edge_check()
	_beaver_check()
	_browse_check()
	_trails_check()
	_rooting_check()
	_mounds_check()
	_drift_check()
	_grazing_check()
	_fairy_check()
	_webs_check()
	_avalanche_check()
	_wrack_check()
	_relief_check()
	_pollen_cloud_check()
	_turf_lip_check()
	_bank_roots_check()
	_hay_check()
	_seed_down_check()
	_base_lift_check()
	# Litter only where crowns drop it: every floor set's litter rule is 0 on open ground (litter 0).
	var open_litter := []
	for fs in [PlantGenerator.FLOOR_OAK_MIXED, PlantGenerator.FLOOR_BEECH, PlantGenerator.FLOOR_MEDITERRANEAN, PlantGenerator.FLOOR_BOREAL_TAIGA]:
		for sp in PlantGenerator.make_floor(fs, 1, false):
			var lr: Vector2 = sp.get_rule_range(PlantSpecies.RULE_LITTER)
			if lr.x > 0.0 and not (lr.x - sp.get_rule_fade(PlantSpecies.RULE_LITTER) > 0.0):
				open_litter.append(sp.resource_name)
	_check("litter: no fallen leaves or twigs on open ground away from crowns", open_litter.is_empty(), open_litter)
	# Nor in full light inside a ring of crowns (a clearing sums every edge's drift): the sward takes it there.
	var lit_open := []
	for sp in PlantGenerator.make_floor(PlantGenerator.FLOOR_BEECH, 1, false):
		if sp.get_rule_range(PlantSpecies.RULE_LITTER).x > 0.0:
			var sr: Vector2 = sp.get_rule_range(PlantSpecies.RULE_SHADE)
			if not (sr.x - sp.get_rule_fade(PlantSpecies.RULE_SHADE) > 0.0):
				lit_open.append(sp.resource_name)
	_check("litter: none in a clearing's full light (shade 0), as forest_floor() draws the ground", lit_open.is_empty(), lit_open)
	# Montado and dehesa (FLOOR_MEDITERRANEAN): the dry grass layer runs through the open and under the crowns.
	var med := PlantGenerator.make_floor(PlantGenerator.FLOOR_MEDITERRANEAN, 1, false)
	var med_grass: PlantSpecies = null
	for sp in med:
		if sp.get_meta("plant_preset", -1) == PlantGenerator.PRESET_DRY_GRASS:
			med_grass = sp
	_check("montado: dry grass in the open and under the crowns", med_grass != null and med_grass.get_density() > 0.5 and med_grass.rule_weight(PlantSpecies.RULE_SHADE, 0.0) > 0.9 and med_grass.rule_weight(PlantSpecies.RULE_SHADE, 0.7) > 0.9, [med.size(), med_grass.get_density() if med_grass else 0.0])
	# Clearings (stand age, wind and beetle kinds): every forest floor carries fireweed, raspberry and clearing grass
	# for the first years to two decades after a windthrow, none in old growth (-1); forest mosses thin after opening.
	var clear_bad := []
	for fs in [PlantGenerator.FLOOR_OAK_MIXED, PlantGenerator.FLOOR_BEECH, PlantGenerator.FLOOR_MONTANE_CONIFER, PlantGenerator.FLOOR_BOREAL_TAIGA]:
		var found := {}
		for sp in PlantGenerator.make_floor(fs, 1, false):
			var p: int = sp.get_meta("plant_preset", -1)
			var ar: Vector2 = sp.get_rule_range(PlantSpecies.RULE_STAND_AGE)
			if p in [PlantGenerator.PRESET_FIREWEED, PlantGenerator.PRESET_RASPBERRY, PlantGenerator.PRESET_COCKSFOOT] and ar.x >= 0.0:
				var w5: float = sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 5.0) * sp.rule_weight(PlantSpecies.RULE_DISTURBANCE, 2.0)
				var w_fire: float = sp.rule_weight(PlantSpecies.RULE_DISTURBANCE, 1.0)
				var w60: float = sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 60.0)
				if w5 > 0.5 and w_fire == 0.0 and w60 == 0.0:
					found[p] = true
				else:
					clear_bad.append([fs, p, w5, w_fire, w60])
			if p == PlantGenerator.PRESET_FEATHER_MOSS or p == PlantGenerator.PRESET_MOSS:
				if ar.x > 0.0 and sp.rule_weight(PlantSpecies.RULE_STAND_AGE, 1.0) > 0.5:
					clear_bad.append([fs, p, "moss not thinned at 1 y"])
		if found.size() < 3:
			clear_bad.append([fs, found.keys()])
	_check("clearings: fireweed, raspberry and clearing grass after windthrow and beetles only, gone in older stands", clear_bad.is_empty(), clear_bad)
	# Boreal taiga (FLOOR_BOREAL_TAIGA): reindeer lichen, feather moss and bilberry carpet the floor.
	var taiga_presets := []
	for sp in PlantGenerator.make_floor(PlantGenerator.FLOOR_BOREAL_TAIGA, 1, false):
		taiga_presets.append(sp.get_meta("plant_preset", -1))
	_check("taiga: reindeer lichen, feather moss and bilberry on the floor", PlantGenerator.PRESET_REINDEER_LICHEN in taiga_presets and PlantGenerator.PRESET_FEATHER_MOSS in taiga_presets and PlantGenerator.PRESET_BILBERRY in taiga_presets, taiga_presets)
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
	hgen.generate_on_ready = false # this bench generates itself, with its own settings
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
	elif floor_view > 0 and OS.get_environment("VEG_FLOOR_STAND") == "mediterranean" and OS.get_environment("VEG_FLOOR_SWARD") != "0":
		# The montado's sward tints its ground (GrassField3D.get_ground_shader_code()), as the terrain material does:
		# where its far blades thin, the ground between them takes their colour.
		var gsh := Shader.new()
		gsh.code = "shader_type spatial;\n" + GrassField3D.get_ground_shader_code() + """
varying vec3 wp;
void vertex() { wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	ALBEDO = grass_ground(wp, vec3(0.025, 0.013, 0.007));
	ROUGHNESS = 0.95;
	SPECULAR = grass_ground_specular(wp, 0.5);
}
"""
		var gmat := ShaderMaterial.new()
		gmat.shader = gsh
		mi.material_override = gmat
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
	tg.fire_variants = false # No burn in this view: skip the five fire-band meshes (halves the stand build).
	tg.lightning_variants = false # Nor strikes: skip the two struck meshes.
	tg.browse_variants = false # Nor deer: skip the browsed meshes.
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

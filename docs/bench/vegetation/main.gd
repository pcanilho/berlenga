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

const SIZE := 256.0
var size := SIZE
var stream_cost := false
var runner: Node3D
var stream_t := 0.0
var test := false
var raster := false
var spp := 16
var out := ""
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

static func height(x: float, z: float) -> float:
	var pit := -0.8 * exp(-(x * x + (z - 40.0) * (z - 40.0)) / 30.0)
	return 0.03 * (z + 100.0) + 0.12 * sqrt(x * x + 25.0) - 0.6 + 0.1 * sin(x * 0.31) * sin(z * 0.27) + pit

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
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
	field.name = "Field"
	field.populate_on_ready = false
	field.biome = _biome()
	add_child(field)
	field.data_map = field.get_path_to(dm)
	if stream_cost:
		_setup_stream_cost()
	if not out.is_empty():
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
	var cam := Camera3D.new()
	cam.position = Vector3(0, 400, 0)
	cam.rotation_degrees = Vector3(90, 0, 0) # At the sky: the cost of streaming, not of drawing.
	add_child(cam)

func _process(dt: float) -> void:
	if not stream_cost or n < 3:
		return
	if not field.streaming:
		print("STREAM_COST species=%d" % field.biome.species.size())
		field.streaming = true
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
	var err := field.populate()
	var counts := []
	for s in 3:
		counts.append(field.get_placements(s).size())
	print("VEG_POPULATE err=%d plants=%d per_species=%s ms=%.1f" % [err, field.get_plant_count(), str(counts), field.get_populate_time_msec()])
	if test:
		_checks()
		await _stream_checks()
		print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
	if out.is_empty():
		get_tree().quit(1 if fails > 0 else 0)
	else:
		_render_later()

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

func _checks() -> void:
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
	dm.geology_source = NodePath()
	dm.bake()
	field.populate()
	_check("geology: rules ignored without a source", field.get_placements(0) == first, field.get_placements(0).size())
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
	# canopy_field: a second field without trees (a camera-local herb layer) reads the first's crowns.
	var herbs := VegetationField3D.new()
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

func _stream_checks() -> void:
	field.weather_map = NodePath()
	field.populate()
	var full := []
	for s in 3:
		full.append(field.get_placements(s))
	var target := Node3D.new()
	add_child(target)
	target.global_position = Vector3(-60, 0, -50)
	var sf := VegetationField3D.new()
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
	sf.queue_free()
	target.queue_free()

func _build_view() -> void:
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.36, 0.32, 0.22)
	gm.roughness = 0.95
	var mi := MeshInstance3D.new()
	mi.mesh = _ground_mesh()
	mi.material_override = gm
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

func _render_later() -> void:
	for i in (6 if raster else 10):
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(1 if fails > 0 else 0)

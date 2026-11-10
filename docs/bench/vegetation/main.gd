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

const SIZE := 256.0
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
	var w := int(SIZE) + 1
	var data := PackedFloat32Array()
	data.resize(w * w)
	for z in w:
		for x in w:
			data[z * w + x] = height(x - SIZE * 0.5, z - SIZE * 0.5)
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
	dm.size = SIZE
	dm.resolution = 256
	dm.bake_on_ready = false
	dm.water_sources = [NodePath("../Lake"), NodePath("../River")]
	add_child(dm)
	field = VegetationField3D.new()
	field.name = "Field"
	field.populate_on_ready = false
	field.biome = _biome()
	add_child(field)
	field.data_map = field.get_path_to(dm)
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

func _physics_process(_d: float) -> void:
	n += 1
	if n != 3:
		return
	dm.bake()
	var err := field.populate()
	var counts := []
	for s in 3:
		counts.append(field.get_placements(s).size())
	print("VEG_POPULATE err=%d plants=%d per_species=%s ms=%.1f" % [err, field.get_plant_count(), str(counts), field.get_populate_time_msec()])
	if test:
		_checks()
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
	tree_sp.canopy_radius = 0.0
	bush_sp.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 1.0))
	field.populate()
	_check("shade: no canopy, no shade", field.get_placements(1).size() == bushes_open and field.get_shade_at(t0) == 0.0, field.get_placements(1).size())
	print("VEG_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)

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
	wp.size = Vector2(SIZE, SIZE)
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
	var step := SIZE / res
	for z in res:
		for x in res:
			var x0 := -SIZE * 0.5 + x * step
			var z0 := -SIZE * 0.5 + z * step
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

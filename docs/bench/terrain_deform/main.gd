# TerrainDeformer3D bench and checks.
#   godot --headless --path docs/bench/terrain_deform -- --test            HeightMapShape3D backend
#   godot --headless --path docs/bench/terrain_deform -- --test --terrain3d Terrain3D backend (addon installed)
#   godot --path docs/bench/terrain_deform -- --out=frame.png              render the carved ground
extends Node3D

const SIZE := 129 # samples a side, 1 m apart

var args := {}
var failures := 0
var checks := 0
var ground_shape: CollisionShape3D
var ground_mesh: MeshInstance3D
var terrain: Node3D
var deformer: TerrainDeformer3D
var crater: TerrainBrush3D
var plot: TerrainBrush3D
var river: Node3D


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	if args.has("terrain3d"):
		if not ClassDB.class_exists("Terrain3D"):
			print("SKIP: Terrain3D is not installed in this project (addons/terrain_3d).")
			get_tree().quit(0)
			return
		_build_terrain3d()
	else:
		_build_height_map()
	_build_brushes()
	if args.has("test"):
		await _run_checks()
		print("%d checks, %d failed" % [checks, failures])
		print("PASS" if failures == 0 else "FAIL")
		get_tree().quit(1 if failures else 0)
	elif args.has("out"):
		_build_view()
		for i in 8:
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(args["out"])
		get_tree().quit()


# Rolling ground with a slope, so carving has something to cut through.
func ground_height(x: float, z: float) -> float:
	return 4.0 + 0.04 * x + 1.5 * sin(x * 0.07) * cos(z * 0.05) + 0.6 * sin(z * 0.13 + 1.0)


func _build_height_map() -> void:
	var shape := HeightMapShape3D.new()
	shape.map_width = SIZE
	shape.map_depth = SIZE
	var data := PackedFloat32Array()
	data.resize(SIZE * SIZE)
	var half := (SIZE - 1) * 0.5
	for z in SIZE:
		for x in SIZE:
			data[z * SIZE + x] = ground_height(x - half, z - half)
	shape.map_data = data
	var body := StaticBody3D.new()
	add_child(body)
	ground_shape = CollisionShape3D.new()
	ground_shape.shape = shape
	body.add_child(ground_shape)
	ground_mesh = MeshInstance3D.new()
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.42, 0.38, 0.3)
	mat.roughness = 0.9
	var mesh := ArrayMesh.new()
	ground_mesh.mesh = mesh
	add_child(ground_mesh)
	deformer = TerrainDeformer3D.new()
	add_child(deformer)
	deformer.terrain = deformer.get_path_to(ground_shape)
	deformer.ground_mesh = deformer.get_path_to(ground_mesh)
	ground_mesh.material_override = mat


func _build_terrain3d() -> void:
	terrain = ClassDB.instantiate("Terrain3D")
	terrain.set("region_size", 256)
	add_child(terrain)
	var data: Object = terrain.get("data")
	var img := Image.create_empty(256, 256, false, Image.FORMAT_RF)
	for z in 256:
		for x in 256:
			img.set_pixel(x, z, Color(ground_height(x - 128.0, z - 128.0), 0, 0, 1))
	# One 256 m region centred on the origin.
	data.call("import_images", [img, null, null], Vector3(-128, 0, -128), 0.0, 1.0)
	deformer = TerrainDeformer3D.new()
	add_child(deformer)
	deformer.terrain = deformer.get_path_to(terrain)


func _build_brushes() -> void:
	crater = TerrainBrush3D.new()
	crater.name = "Crater"
	crater.profile = TerrainBrush3D.PROFILE_BOWL
	crater.mode = TerrainBrush3D.MODE_LOWER
	crater.size = Vector2(12, 12)
	crater.depth = 3.0
	crater.falloff = 2.0
	add_child(crater)
	crater.global_position = Vector3(-30, ground_height(-30, -20), -20)

	plot = TerrainBrush3D.new()
	plot.name = "Plot"
	plot.shape = TerrainBrush3D.SHAPE_RECTANGLE
	plot.mode = TerrainBrush3D.MODE_FLATTEN
	plot.size = Vector2(16, 10)
	plot.falloff = 3.0
	add_child(plot)
	plot.global_position = Vector3(25, 5.0, 25)
	plot.rotation.y = deg_to_rad(30)

	river = Node3D.new()
	river.set_script(load("res://river_strip_brush.gd"))
	river.name = "River"
	add_child(river)
	river.global_position = Vector3(0, 0, 10)
	river.deformer = deformer

	deformer.auto_apply = false
	deformer.brushes = [deformer.get_path_to(crater), deformer.get_path_to(plot), deformer.get_path_to(river)]


func _build_view() -> void:
	var cam := Camera3D.new()
	add_child(cam)
	cam.global_position = Vector3(-55, 40, 55)
	cam.look_at(Vector3(0, 4, 0))
	var sun := DirectionalLight3D.new()
	add_child(sun)
	sun.rotation_degrees = Vector3(-35, -60, 0)
	sun.shadow_enabled = true
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_SKY
	env.environment.sky = Sky.new()
	env.environment.sky.sky_material = ProceduralSkyMaterial.new()
	add_child(env)
	deformer.apply()


func check(cond: bool, what: String) -> void:
	checks += 1
	if not cond:
		failures += 1
		print("FAIL: ", what)
	else:
		print("ok: ", what)


func near(a: float, b: float, eps := 0.02) -> bool:
	return absf(a - b) <= eps


func _run_checks() -> void:
	var backend_name := "Terrain3D" if terrain else "HeightMapShape3D"
	print("backend: ", backend_name, " (", deformer.get_backend(), ")")
	check(deformer.get_backend() == (TerrainDeformer3D.BACKEND_TERRAIN3D if terrain else TerrainDeformer3D.BACKEND_HEIGHT_MAP), "backend detected")

	var c := crater.global_position
	var river_pt := Vector3(3, 0, 10)
	var far_pt := Vector3(50, 0, -50)
	var before_crater := deformer.get_height(c)
	var before_river := deformer.get_height(river_pt)
	var before_far := deformer.get_height(far_pt)
	check(near(before_crater, ground_height(c.x, c.z), 0.05), "ground reads back (%.3f vs %.3f)" % [before_crater, ground_height(c.x, c.z)])

	var written := deformer.apply()
	check(written > 0, "apply writes vertices (%d)" % written)
	check(near(deformer.get_height(c), c.y - 3.0, 0.05), "bowl crater bottom at brush height - depth (%.3f vs %.3f)" % [deformer.get_height(c), c.y - 3.0])
	check(near(deformer.get_base_height(c), before_crater), "base height under the crater unchanged")
	check(near(deformer.get_height(plot.global_position), 5.0), "plot flattened to its node height (%.3f)" % deformer.get_height(plot.global_position))
	check(near(deformer.get_height(river_pt), before_river - 2.0, 0.05), "river channel cut 2 m below the uncarved ground (%.3f vs %.3f)" % [deformer.get_height(river_pt), before_river - 2.0])
	check(near(deformer.get_height(far_pt), before_far, 0.0001), "ground outside every brush untouched")

	check(deformer.apply() == 0, "second apply writes nothing (idempotent)")
	river.changed()
	check(deformer.apply() == 0, "river re-carve from base heights does not dig deeper")

	if terrain:
		var t3d_data: Object = terrain.get("data")
		var t3d_h: float = t3d_data.call("get_height", river_pt)
		check(near(t3d_h, deformer.get_height(river_pt), 0.05), "Terrain3D's own get_height sees the channel (%.3f)" % t3d_h)
		var region: Object = t3d_data.call("get_regionp", river_pt)
		check(region.get("modified") and not region.get("edited"), "carved region marked modified (saves), edited flag cleared")

	# Collision follows the carve.
	if not terrain:
		await get_tree().physics_frame
		await get_tree().physics_frame
		var q := PhysicsRayQueryParameters3D.create(Vector3(river_pt.x, 50, river_pt.z), Vector3(river_pt.x, -50, river_pt.z))
		var hit := get_world_3d().direct_space_state.intersect_ray(q)
		check(not hit.is_empty() and near(hit.position.y, deformer.get_height(river_pt), 0.05), "ray hits the carved collision (%s)" % [hit.get("position", "no hit")])
		check(ground_mesh.mesh.get_surface_count() == 1 and ground_mesh.mesh.get_faces().size() > 0, "ground mesh rebuilt")

	# Move the crater: the old place comes back exactly, the new one is carved.
	var old_c := c
	crater.global_position = Vector3(-30, ground_height(-30, 40), 40)
	deformer.apply()
	check(near(deformer.get_height(old_c), before_crater, 0.0001), "moved crater restores the old ground exactly")
	check(deformer.get_height(crater.global_position) < ground_height(-30, 40) - 2.0, "moved crater carves its new place")

	# Stack order: a later raise brush wins over the crater.
	var mound := TerrainBrush3D.new()
	mound.mode = TerrainBrush3D.MODE_RAISE
	mound.size = Vector2(4, 4)
	mound.falloff = 0.0
	add_child(mound)
	mound.global_position = crater.global_position + Vector3(0, 1.0, 0)
	deformer.add_brush(mound)
	deformer.apply()
	check(near(deformer.get_height(crater.global_position), crater.global_position.y + 1.0), "later raise brush wins in stack order")
	deformer.remove_brush(mound)
	deformer.apply()
	check(deformer.get_height(crater.global_position) < ground_height(-30, 40) - 2.0, "removing the raise brush brings the crater back")

	# Persistence: base heights saved with the scene let a fresh deformer restore a carved ground.
	var keys: PackedInt64Array = deformer.get("base_keys")
	var values: PackedFloat32Array = deformer.get("base_values")
	check(keys.size() == values.size() and keys.size() == deformer.get_carved_vertex_count(), "base heights serialise (%d)" % keys.size())
	var packed := PackedScene.new()
	deformer.owner = self
	packed.pack(deformer)
	var restored: TerrainDeformer3D = packed.instantiate()
	restored.auto_apply = false
	restored.brushes = []
	deformer.auto_apply = false
	add_child(restored)
	restored.terrain = deformer.terrain
	check(restored.get_carved_vertex_count() == keys.size(), "packed scene keeps base heights")
	restored.clear()
	check(near(deformer.get_height(crater.global_position), ground_height(-30, 40), 0.05) and near(deformer.get_height(river_pt), before_river, 0.001), "clear() from the saved base heights restores the original ground")

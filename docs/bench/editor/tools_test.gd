# Checks the level-design tools' effects without the editor (docs/ideas/editor-tools.md B to E):
# keep-out areas, hand-placed plants, the biome map and the sculpt layer.
# godot --headless --path docs/bench/editor --script tools_test.gd
extends SceneTree

var fails := 0
var world: Node3D
var gen: TerrainGenerator3D
var dm: TerrainDataMap3D


func check(ok: bool, what: String) -> void:
	print("%s: %s" % ["ok" if ok else "FAIL", what])
	if not ok:
		fails += 1


func _species(mesh: Mesh, spacing: float, footprint: float) -> PlantSpecies:
	var sp := PlantSpecies.new()
	sp.meshes = [mesh]
	sp.spacing = spacing
	sp.footprint = footprint
	sp.density = 1.0
	sp.slope_range = Vector2(0, 90)
	sp.height_range = Vector2(-10000, 10000)
	return sp


func _field(name: String) -> VegetationField3D:
	var b := Biome.new()
	b.species = [_species(BoxMesh.new(), 2.0, 0.5)]
	var v := VegetationField3D.new()
	v.name = name
	v.biome = b
	v.tile_size = 32.0
	v.populate_on_ready = false
	world.add_child(v)
	v.data_map = v.get_path_to(dm)
	return v


# Placements, not the drawn instances: a headless run's dummy renderer keeps no MultiMesh transforms.
func _count(v: VegetationField3D, at: Vector3, r: float) -> int:
	var n := 0
	for p in v.get_placements(0):
		if Vector2(p.x - at.x, p.z - at.z).length() <= r:
			n += 1
	return n


func _initialize() -> void:
	world = Node3D.new()
	get_root().add_child(world)
	await process_frame # the root enters the tree after _initialize starts
	gen = TerrainGenerator3D.new()
	gen.generate_on_ready = false
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.tile_erosion_enabled = false
	gen.strata_enabled = false
	gen.dunes_enabled = false
	gen.deposition = 0.0
	gen.moraine_age = 0.0
	gen.scree_age = 0.0
	gen.meander_age = 0.0
	gen.outwash_age = 0.0
	gen.size = Vector2(512, 512)
	gen.cell_size = 16.0
	gen.seed = 3
	world.add_child(gen)
	check(gen.generate(), "the generator runs")
	dm = TerrainDataMap3D.new()
	dm.size = 512.0
	dm.resolution = 128
	dm.bake_on_ready = false
	world.add_child(dm)
	dm.height_source = dm.get_path_to(gen)
	check(dm.bake() == OK, "the data map bakes")

	# C: a keep-out box clears plants inside it and keeps them outside.
	var v := _field("Plants")
	v.populate()
	var c := Vector3(40, 0, 40)
	var before := _count(v, c, 10.0)
	check(before > 0, "plants grow round the test point (%d)" % before)
	var ex := WorldExclusion3D.new()
	ex.size = Vector2(30, 30)
	ex.fade = 0.0
	world.add_child(ex)
	ex.global_position = c
	check(is_equal_approx(ex.get_keep_out_at(c), 1.0) and ex.get_keep_out_at(c + Vector3(40, 0, 0)) == 0.0, "the box keeps out inside, not outside")
	v.populate()
	check(_count(v, c, 10.0) == 0, "no plant inside a keep-out box")
	check(_count(v, c + Vector3(60, 0, 0), 10.0) > 0, "plants still grow outside it")
	# A track: an open curve of width 6.
	var tr := WorldExclusion3D.new()
	var cv := Curve3D.new()
	cv.add_point(Vector3(-100, 0, -60))
	cv.add_point(Vector3(100, 0, -60))
	tr.curve = cv
	tr.closed = false
	tr.width = 6.0
	tr.fade = 0.0
	world.add_child(tr)
	check(tr.get_keep_out_at(Vector3(0, 0, -60)) == 1.0 and tr.get_keep_out_at(Vector3(0, 0, -50)) == 0.0, "an open curve keeps out a track of its width")
	ex.kinds = WorldExclusion3D.KIND_GRASS # plants may come back
	v.populate()
	check(_count(v, c, 10.0) > 0, "a grass-only area leaves plants")
	ex.queue_free()
	tr.queue_free()

	# C: a hand-placed plant draws its species and clears its footprint.
	var pp := PlacedPlant3D.new()
	pp.species = _species(BoxMesh.new(), 2.0, 3.0)
	world.add_child(pp)
	pp.global_position = Vector3(-40, 0, 40)
	await process_frame
	v.populate()
	check(_count(v, pp.global_position, 2.5) == 0, "procedural plants keep out of a placed plant's footprint")
	check(pp.get_effective_clearance() == 3.0, "the clearance is the species' footprint")
	pp.queue_free()
	await process_frame

	# D: two fields share one biome map, a channel each.
	var img := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			img.set_pixel(x, y, Color(1, 0, 0, 0) if x < 32 else Color(0, 1, 0, 0))
	var tex := ImageTexture.create_from_image(img)
	var west := _field("West")
	var east := _field("East")
	for f in [west, east]:
		f.biome_map = tex
		f.biome_map_rect = Rect2(-256, -256, 512, 512)
	west.biome_channel = 0
	east.biome_channel = 1
	check(is_equal_approx(west.get_biome_share_at(Vector3(-100, 0, 0)), 1.0) and west.get_biome_share_at(Vector3(100, 0, 0)) == 0.0, "a field's share is its channel")
	west.populate()
	east.populate()
	check(_count(west, Vector3(-120, 0, 0), 15.0) > 0 and _count(west, Vector3(120, 0, 0), 15.0) == 0, "the red field grows only where red is painted")
	check(_count(east, Vector3(120, 0, 0), 15.0) > 0 and _count(east, Vector3(-120, 0, 0), 15.0) == 0, "the green field grows only where green is painted")
	# Overlapping paint splits the ground.
	img.fill(Color(1, 1, 0, 0))
	tex.update(img)
	check(is_equal_approx(west.get_biome_share_at(Vector3(0, 0, 0)), 0.5), "overlapping biomes share the ground (%.2f)" % west.get_biome_share_at(Vector3(0, 0, 0)))

	# E: a sculpt layer raises the ground, finer than the generator's cells, and goes when removed.
	var p := Vector3(24.0, 0, -8.0) # between grid nodes (16 m cells)
	var h0 := gen.get_height(p)
	var hq0 := gen.get_height(p + Vector3(0.5, 0, 0.5))
	var far := Vector3(-200, 0, -200)
	var hfar0 := gen.get_height(far)
	var layer := TerrainSculptLayer3D.new()
	var hm := Image.create_empty(512, 512, false, Image.FORMAT_RF)
	hm.fill(Color(0, 0, 0))
	# A 3 m bump, 4 m wide: much finer than a 16 m cell.
	for y in range(-2, 3):
		for x in range(-2, 3):
			hm.set_pixel(256 + int(p.x) + x, 256 + int(p.z) + y, Color(3.0, 0, 0))
	layer.height_map_rect = Rect2(-256, -256, 512, 512)
	layer.height_map = ImageTexture.create_from_image(hm)
	gen.add_child(layer)
	check(gen.apply_touch_ups(), "the touch-ups apply the sculpt layer")
	var h1 := gen.get_height(p + Vector3(0.5, 0, 0.5))
	check(absf(h1 - hq0 - 3.0) < 0.6, "a 4 m stroke raises the ground by its 3 m between grid nodes (%.2f m)" % (h1 - hq0))
	check(absf(gen.get_height(far) - hfar0) < 0.01, "the ground is unchanged far from the stroke")
	layer.enabled = false
	gen.apply_touch_ups()
	check(absf(gen.get_height(p) - h0) < 0.05 and absf(gen.get_height(p + Vector3(0.5, 0, 0.5)) - hq0) < 0.05, "turning the layer off restores the ground")

	print("CHECKS %s (%d failed)" % ["PASSED" if fails == 0 else "FAILED", fails])
	quit(1 if fails else 0)

# Writes world.tscn: a level made of world-system nodes only, no script, as a level designer would save it.
# godot --headless --path docs/bench/editor --script make_world.gd
extends SceneTree


func _species(mesh: Mesh, spacing: float, footprint: float, density: float) -> PlantSpecies:
	var sp := PlantSpecies.new()
	sp.meshes = [mesh]
	sp.spacing = spacing
	sp.footprint = footprint
	sp.density = density
	return sp


func _initialize() -> void:
	var root := Node3D.new()
	root.name = "World"

	var gen := TerrainGenerator3D.new()
	gen.name = "Generator"
	# Fast settings for the bench: a 1 km world at 16 m cells, the heavy stages off.
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
	gen.size = Vector2(1024, 1024)
	gen.cell_size = 16.0
	gen.ocean_edges = 1
	gen.seed = 7
	root.add_child(gen)

	var land := Landscape3D.new()
	land.name = "Landscape"
	land.material = TerrainMaterial3D.new()
	root.add_child(land)
	land.source = land.get_path_to(gen)

	var dm := TerrainDataMap3D.new()
	dm.name = "DataMap"
	dm.size = 1024.0
	dm.resolution = 256
	root.add_child(dm)
	dm.height_source = dm.get_path_to(gen)

	var biome := Biome.new()
	var tree_mesh := CylinderMesh.new()
	tree_mesh.top_radius = 0.0
	tree_mesh.bottom_radius = 2.0
	tree_mesh.height = 8.0
	var bush := SphereMesh.new()
	bush.radius = 0.6
	bush.height = 1.0
	biome.species = [_species(tree_mesh, 9.0, 3.0, 0.6), _species(bush, 3.0, 0.8, 0.5)]
	var veg := VegetationField3D.new()
	veg.name = "Vegetation"
	veg.biome = biome
	veg.tile_size = 64.0
	root.add_child(veg)
	veg.data_map = veg.get_path_to(dm)

	var grass := GrassField3D.new()
	grass.name = "Grass"
	grass.radius = 30.0
	root.add_child(grass)
	grass.data_map = grass.get_path_to(dm)

	var rocks := RockField3D.new()
	rocks.name = "Rocks"
	root.add_child(rocks)
	rocks.generator = rocks.get_path_to(gen)

	# A level has a player camera; grass and the streaming fields follow it in the game.
	var cam := Camera3D.new()
	cam.name = "Camera"
	cam.position = Vector3(0, 40, 0)
	cam.rotation_degrees = Vector3(-35, 0, 0)
	root.add_child(cam)

	for n in root.get_children():
		n.owner = root
	var ps := PackedScene.new()
	var err := ps.pack(root)
	if err == OK:
		err = ResourceSaver.save(ps, "res://world.tscn")
	print("make_world: %s, %d nodes" % [error_string(err), root.get_child_count() + 1])
	root.free()
	quit(0 if err == OK else 1)

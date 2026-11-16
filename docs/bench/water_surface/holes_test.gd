extends SceneTree
# Terrain holes under the sea (PhysXWaterSurface3D.terrain_source) and the shore queries sample_depth / sample_swash.
# A static floor 3 m down covers x < 0 only; x > 0 is "a hole onto nothing". A stand-in terrain says x in (0, 40) is a
# hole whose ground stands 2 m above the water. Run (needs a GPU device; lavapipe works):
#   xvfb-run -a godot --path docs/bench/water_surface --rendering-driver vulkan -s holes_test.gd
# Prints PASS/FAIL lines and quits.

class StandIn extends Node3D:
	signal holes_changed(rect: Rect2)
	var open := true
	func is_hole(p: Vector3) -> bool:
		return open and p.x > 0.0 and p.x < 40.0
	func has_holes_in(_r: Rect2) -> bool:
		return open
	func get_height(_p: Vector3) -> float:
		return 2.0

var water: Node3D
var terrain: StandIn
var frame := 0
var fails := 0

func check(label: String, ok: bool, value) -> void:
	print(("PASS " if ok else "FAIL ") + label + " = " + str(value))
	if not ok:
		fails += 1

func _initialize() -> void:
	if not ClassDB.class_exists("PhysXWaterSurface3D"):
		print("FAIL no PhysX module")
		quit(1)
		return
	var floor_body := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(200, 1, 400)
	shape.shape = box
	floor_body.add_child(shape)
	floor_body.position = Vector3(-100, -3.5, 0)
	root.add_child(floor_body)
	terrain = StandIn.new()
	terrain.name = "Terrain"
	root.add_child(terrain)
	water = ClassDB.instantiate("PhysXWaterSurface3D")
	water.name = "Sea"
	water.set("depth", 20.0)
	water.set("seabed_from_floor", true)
	water.set("terrain_source", NodePath("../Terrain"))
	root.add_child(water)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 30, 30)
	root.add_child(cam)
	cam.look_at(Vector3.ZERO)

func _physics_process(_delta: float) -> bool:
	frame += 1
	if frame == 20:
		check("floor side depth ~3", absf(water.sample_depth(Vector3(-10, 0, 0)) - 3.0) < 0.6, water.sample_depth(Vector3(-10, 0, 0)))
		check("hole closed by terrain (2 m above: depth -2)", absf(water.sample_depth(Vector3(10, 0, 0)) + 2.0) < 0.6, water.sample_depth(Vector3(10, 0, 0)))
		var sw: Vector2 = water.sample_swash(Vector3(-10, 0, 0))
		check("sample_swash in range", sw.x >= 0.0 and sw.y >= 0.0 and sw.y <= 1.0, sw)
		terrain.open = false
		terrain.holes_changed.emit(Rect2(0, -20, 40, 40))
	if frame == 40:
		check("hole closed again: remeasured to open water", water.sample_depth(Vector3(10, 0, 0)) > 15.0, water.sample_depth(Vector3(10, 0, 0)))
		var sw: Vector2 = water.sample_swash(Vector3(-10, 0, 0))
		check("sample_swash after readback", sw.y > 0.0, sw)
		print("DONE fails=", fails)
		quit(0 if fails == 0 else 1)
	return false

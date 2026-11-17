extends SceneTree
# The features that ship on by default must survive being switched off and on again in a running scene (aerosol
# haze once recursed forever when turned off). Run (needs a GPU device; lavapipe works):
#   xvfb-run -a godot --path docs/bench/water_surface --rendering-driver vulkan -s defaults_toggle_test.gd
# Prints PASS/FAIL lines and quits.

var water: Node3D
var weather: Node3D
var frame := 0
var fails := 0

func check(label: String, ok: bool, value) -> void:
	print(("PASS " if ok else "FAIL ") + label + " = " + str(value))
	if not ok:
		fails += 1

func _initialize() -> void:
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	root.add_child(env)
	var sun := DirectionalLight3D.new()
	root.add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 3, 10)
	root.add_child(cam)
	if ClassDB.class_exists("WeatherMap3D"):
		weather = ClassDB.instantiate("WeatherMap3D")
		root.add_child(weather)
	else:
		print("SKIP no weather module")
	if ClassDB.class_exists("PhysXWaterSurface3D"):
		water = ClassDB.instantiate("PhysXWaterSurface3D")
		root.add_child(water)
	else:
		print("SKIP no PhysX module")

func _process(_delta: float) -> bool:
	frame += 1
	if frame == 10:
		if weather:
			check("aerosol_haze on by default", weather.aerosol_haze, weather.aerosol_haze)
			check("cloud_evolution on by default", weather.cloud_evolution, weather.cloud_evolution)
		if water:
			for p in ["crest_sheets", "caustics_enabled", "shore_waves"]:
				check(p + " on by default", water.get(p), water.get(p))
	elif frame == 20:
		if weather:
			weather.aerosol_haze = false
			weather.cloud_evolution = false
		if water:
			for p in ["crest_sheets", "caustics_enabled", "shore_waves"]:
				water.set(p, false)
	elif frame == 40:
		if weather:
			check("haze off, still running", not weather.aerosol_haze, weather.aerosol_haze)
			weather.aerosol_haze = true
			weather.cloud_evolution = true
		if water:
			for p in ["crest_sheets", "caustics_enabled", "shore_waves"]:
				water.set(p, true)
	elif frame == 60:
		if weather:
			weather.aerosol_haze = false
			weather.aerosol_haze = false
			weather.aerosol_haze = true
			check("haze toggled twice in a frame", weather.aerosol_haze, weather.aerosol_haze)
		if water:
			check("water toggled off and on", water.crest_sheets and water.caustics_enabled and water.shore_waves, true)
	elif frame == 80:
		print("DONE fails=%d" % fails)
		return true
	return false

extends SceneTree
# Impulse waves (WorldWater.wave -> PhysXWaterSurface3D.submit_wave): a 0.5 m, 4 s wave set off along +x in 4 m of
# still water must run one way at the long-wave speed sqrt(g h) = 6.26 m/s, holding its height. Run (needs a GPU
# device; lavapipe works):
#   xvfb-run -a godot --path docs/bench/water_surface --rendering-driver vulkan -s wave_test.gd
# Prints PASS/FAIL lines and quits.

const DEPTH := 4.0
const START := Vector3(-20.0, 0.0, 0.0)
const HEIGHT := 0.5
const PERIOD := 4.0
const WAIT := 120 # physics frames (2 s at 60 Hz)

var water: Node3D
var frame := 0
var fails := 0
var sent := -1

func check(label: String, ok: bool, value) -> void:
	print(("PASS " if ok else "FAIL ") + label + " = " + str(value))
	if not ok:
		fails += 1

func _initialize() -> void:
	if not ClassDB.class_exists("PhysXWaterSurface3D"):
		print("FAIL no PhysX module")
		quit(1)
		return
	water = ClassDB.instantiate("PhysXWaterSurface3D")
	water.set("domain_size", Vector2(96, 96))
	water.set("grid_resolution", 192)
	water.set("depth", DEPTH)
	water.set("seabed_from_floor", false)
	water.set("follow_weather", false)
	water.set("wave_amplitude", 0.0)
	water.set("shore_waves", false)
	root.add_child(water)

func _physics_process(_delta: float) -> bool:
	frame += 1
	if sent < 0 and frame >= 30 and water.call("sample_depth", START) > 0.0:
		WorldContracts.get_water().wave(START, Vector3(1, 0, 0), HEIGHT, PERIOD, 30.0)
		sent = frame
	if sent > 0 and frame == sent + WAIT:
		var best_x := 0.0
		var best := -1.0
		var back := 0.0
		for i in 321:
			var x := -40.0 + i * 0.25
			var h: float = water.call("sample_height", Vector3(x, 0.0, 0.0))
			if h > best:
				best = h
				best_x = x
			if x < START.x - 8.0:
				back = maxf(back, absf(h))
		var c := sqrt(9.81 * DEPTH)
		var want := START.x + c * PERIOD * 0.25 + c * WAIT / 60.0
		check("crest travels at sqrt(g h)", absf(best_x - want) < 3.5, "%.1f m (want %.1f)" % [best_x, want])
		check("crest holds about half the height", best > 0.5 * HEIGHT * 0.6 and best < 0.5 * HEIGHT * 1.3, "%.3f m" % best)
		check("it runs one way", back < 0.25 * best, "%.3f m behind" % back)
		print("wave: %d failed" % fails)
		quit(1 if fails > 0 else 0)
	if frame > 600:
		check("wave sent", false, sent)
		quit(1)
	return false

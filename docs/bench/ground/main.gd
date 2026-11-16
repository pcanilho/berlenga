extends Node3D
# GroundSurface3D bench (docs/ideas/ground-deform.md): footprints in sand, soil, mud and leaf litter.
#
#   --test              print the checks and quit:
#                         boot sinkage by material against Bekker with Wong's values
#                         volume kept through a press and through slumping
#                         dry sand slumps to its repose angle, damp sand keeps steep walls
#                         mud creeps back slowly; an undisturbed 50 degree slope does not move
#                         litter adds to the print; bare rock does not print; turned ground dries
#   --test=soil         soft soil under wheels and feet (needs the PhysX build):
#                         a wheel's sinkage, traction and resistance against Bekker, Wong and Janosi-Hanamoto
#                         a 4x4 on dry sand, damp sand and firm ground; an aired-down tyre; a bogged car
#                         walker footing: effort by print depth, sliding on a slope past the repose
#   --view=trail        a walker, a rolling ball and a wheel cross the ground; --ground=KIND picks it:
#                       dry_sand, damp_sand, soil, mud, litter (default soil)
#   --view=ruts         a 4x4 drives a curve through the ground and leaves (needs the PhysX build); on dry sand
#                       its tyres are aired down to 60 kPa; --ground=KIND as above (default damp_sand)
#   --raster            raster only (default path traced)
#   --out=path.png      save the frame and quit
#   --spp=N --bounces=N path tracing samples and bounces (default 16, 3)
#   --gpu=0             the field on the CPU (default: compute shaders where there is a GPU; --headless
#                       always runs it on the CPU, so --test under a window and headless compares both)

const HeightScript := preload("res://height.gd")

var view := "trail"
var kind := "soil"
var raster := false
var test := false
var soil_test := false
var out := ""
var spp := 16
var gpu := true
var bounces := 3
var fails := 0
var height: Node3D
var surface: GroundSurface3D
var walker: CharacterBody3D
var walk_t := 0.0
var walking := false


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a.begins_with("--ground="): kind = a.substr(9)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a == "--test=soil": soil_test = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a == "--gpu=0": gpu = false
	if soil_test:
		await _run_soil_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test:
		await _run_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if view == "ruts":
		await _build_ruts()
		return
	await _build_trail()


func _check(label: String, ok: bool, detail: String) -> void:
	print(("PASS " if ok else "FAIL ") + label + ": " + detail)
	if not ok:
		fails += 1


func _setup(height_kind: String, ground_type: int, moisture: float, litter: float) -> void:
	for c in get_children():
		c.queue_free()
	await get_tree().physics_frame
	height = HeightScript.new()
	height.name = "Height"
	height.kind = height_kind
	add_child(height)
	var probe := Marker3D.new()
	probe.name = "Probe"
	add_child(probe)
	surface = GroundSurface3D.new()
	surface.height_source = NodePath("../Height")
	surface.follow_target = NodePath("../Probe")
	surface.ground_type = ground_type
	surface.moisture = moisture
	surface.litter_cover = litter
	surface.gpu_simulation = gpu
	add_child(surface)
	await get_tree().physics_frame


func _apply(kind_name: String) -> void:
	# dry_sand, damp_sand, soil, mud, litter
	match kind_name:
		"dry_sand":
			surface.ground_type = GroundSurface3D.GROUND_SAND
			surface.moisture = 0.0
		"damp_sand":
			surface.ground_type = GroundSurface3D.GROUND_SAND
			surface.moisture = 0.4
		"mud":
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 1.0
		"litter":
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 0.2
			surface.litter_cover = 1.0
		_:
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 0.2


# Steepest surface slope (degrees) between neighbouring texels in a square around p.
func _max_slope(p: Vector3, half: float) -> float:
	var t := surface.deform_extent / surface.deform_resolution
	var worst := 0.0
	var x := p.x - half
	while x <= p.x + half:
		var z := p.z - half
		while z <= p.z + half:
			var a := Vector3(x, 0, z)
			var h := surface.get_surface_height_at(a)
			for d in [Vector3(t, 0, 0), Vector3(0, 0, t)]:
				var g := surface.get_surface_height_at(a + d)
				worst = maxf(worst, rad_to_deg(atan(absf(g - h) / t)))
			z += t
		x += t
	return worst


func _run_tests() -> void:
	# Sinkage under a boot: 75 kg on 0.035 m2, 0.108 m wide.
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var o := Vector3(0, 0, 0)
	var dry_sand := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("boot in dry sand", dry_sand > 0.015 and dry_sand < 0.03, "%.1f cm (1.5 to 3)" % (dry_sand * 100.0))
	surface.moisture = 0.4
	var damp := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("damp sand is firmer", damp < dry_sand * 0.6, "%.1f cm" % (damp * 100.0))
	surface.ground_type = GroundSurface3D.GROUND_SOIL
	surface.moisture = 0.0
	var soil := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	surface.moisture = 1.0
	var mud := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("boot in mud", mud > 0.05 and mud < 0.1, "%.1f cm (5 to 10); dry soil %.1f cm" % [mud * 100.0, soil * 100.0])
	surface.moisture = 0.5
	var moist := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("moist soil between", moist > soil and moist < mud, "%.1f cm" % (moist * 100.0))
	surface.moisture = 0.2
	var bare := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	surface.litter_cover = 1.0
	var litter := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("litter adds to the print", litter > bare + 0.02, "%.1f cm on 4 cm of litter against %.1f cm" % [litter * 100.0, bare * 100.0])
	surface.litter_cover = 0.0

	# A boot pressed into dry sand: volume kept, then slumping keeps it too.
	surface.ground_type = GroundSurface3D.GROUND_SAND
	surface.moisture = 0.0
	surface.clear_deformation()
	var moved := surface.press_box(o, Vector2(0.11, 0.29), 0.3, 0.0, dry_sand)
	var v := surface.get_moved_volume()
	var rim_share: float = surface.get_material_at(o)["rim_share"]
	var compacted := v.x - v.y
	_check("press keeps volume", absf(v.y - moved * rim_share) < 0.02 * moved and absf(compacted - moved * (1.0 - rim_share)) < 0.02 * moved,
			"moved %.2f l, rim %.2f l (%.0f%%), compacted %.2f l" % [moved * 1000.0, v.y * 1000.0, 100.0 * v.y / maxf(moved, 1e-9), compacted * 1000.0])
	# A deep round print (a dropped ball) in dry sand: walls collapse to the repose angle.
	surface.clear_deformation()
	surface.press(o, 0.15, -0.12, 0.0, true)
	var before := _max_slope(o, 0.3)
	var v0 := surface.get_moved_volume()
	surface.settle(3.0)
	var after := _max_slope(o, 0.3)
	var v1 := surface.get_moved_volume()
	_check("dry sand slumps to its repose angle", before > 45.0 and after < 36.0, "steepest wall %.0f before, %.0f degrees after 3 s (33 + a texel's rounding)" % [before, after])
	_check("slumping keeps volume", absf((v1.x - v1.y) - (v0.x - v0.y)) < 1e-4, "net hollow %.3f l before, %.3f l after" % [(v0.x - v0.y) * 1000.0, (v1.x - v1.y) * 1000.0])
	# Damp sand keeps steep walls (Hornbaker et al. 1997).
	surface.clear_deformation()
	surface.moisture = 0.4
	surface.press(o, 0.15, -0.12, 0.0, true)
	surface.settle(3.0)
	var damp_wall := _max_slope(o, 0.3)
	_check("damp sand keeps steep walls", damp_wall > 60.0, "steepest wall %.0f degrees after 3 s" % damp_wall)
	# Mud creeps back slowly.
	surface.clear_deformation()
	surface.ground_type = GroundSurface3D.GROUND_SOIL
	surface.moisture = 1.0
	surface.press(o, 0.15, -0.12, 0.0, true)
	var mud0 := _max_slope(o, 0.3)
	var fresh0 := surface.get_fresh_at(o)
	surface.settle(10.0, 0.1)
	var mud10 := _max_slope(o, 0.3)
	surface.settle(900.0, 0.5)
	var mud900 := _max_slope(o, 0.3)
	_check("mud creeps back slowly", mud10 > 45.0 and mud900 < mud10 - 10.0, "steepest wall %.0f, %.0f after 10 s, %.0f degrees after 15 min" % [mud0, mud10, mud900])
	# Turned ground dries.
	surface.settle(surface.drying_time * 2.0, 5.0)
	var fresh1 := surface.get_fresh_at(o)
	_check("turned ground dries", fresh0 > 0.5 and fresh1 < 0.1, "fresh %.2f, %.2f after two drying times" % [fresh0, fresh1])

	# A 50 degree slope of dry sand: only the print relaxes, the slope stays.
	await _setup("slope", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var sp := Vector3(1.0, height.get_height(Vector3(1.0, 0, 0)), 0.0)
	surface.press_box(sp, Vector2(0.11, 0.29), 0.0, sp.y, 0.02)
	surface.settle(3.0)
	var far := Vector3(4.0, 0, 3.0)
	var drift := absf(surface.get_surface_height_at(far) - height.get_height(far))
	var vv := surface.get_moved_volume()
	_check("an undisturbed slope steeper than the repose stays", drift < 1e-4 and vv.x > 0.0, "%.2f mm off 3 m away, print %.2f l" % [drift * 1000.0, vv.x * 1000.0])

	# Bare rock (type map, green) does not print.
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	var img := Image.create(4, 4, false, Image.FORMAT_RGB8)
	img.fill(Color(0, 1, 0))
	surface.type_map = img
	surface.type_map_rect = Rect2(-2, -2, 4, 4)
	var rock := surface.press_box(o, Vector2(0.11, 0.29), 0.0, 0.0, 0.05)
	var soil_print := surface.press_box(Vector3(5, 0, 0), Vector2(0.11, 0.29), 0.0, 0.0, 0.05)
	_check("bare rock does not print", rock == 0.0 and soil_print > 0.0, "rock %.2f l, soil beside it %.2f l" % [rock * 1000.0, soil_print * 1000.0])

	# Bodies in the group press by themselves: a 75 kg character on dry sand.
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var foot := Node3D.new()
	var fs := CollisionShape3D.new()
	fs.shape = BoxShape3D.new()
	(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
	foot.add_child(fs)
	foot.set_meta("ground_mass", 75.0)
	foot.set_meta("ground_contact_area", 0.035)
	foot.add_to_group("ground_deform")
	add_child(foot)
	foot.global_position = Vector3(1.0, 0.03, 1.0)
	for i in 3:
		await get_tree().physics_frame
	surface.sync_deformation()
	var auto_dent := surface.get_dent_at(Vector3(1.0, 0, 1.0)) - surface.get_rim_at(Vector3(1.0, 0, 1.0))
	_check("a body in ground_deform prints", auto_dent > 0.01, "%.1f cm under the boot" % (auto_dent * 100.0))
	print("step %.2f ms, field on the %s" % [surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU"])


# Bekker's rigid wheel, written out here apart from the engine's: z0 = (3 W / ((3 - n) b k sqrt(D)))^(2 / (2n + 1)).
func _bekker_sinkage(load: float, radius: float, width: float, n: float, kc: float, kphi: float) -> float:
	var k := (kc / width + kphi) * 1000.0
	return pow(3.0 * load / ((3.0 - n) * width * k * sqrt(2.0 * radius)), 2.0 / (2.0 * n + 1.0))


func _make_car(mass: float, torque: float, at: Vector3) -> PhysXVehicle3D:
	var car := PhysXVehicle3D.new()
	car.mass = mass
	car.max_engine_torque = torque
	car.can_sleep = false
	var cs := CollisionShape3D.new()
	cs.shape = BoxShape3D.new()
	(cs.shape as BoxShape3D).size = Vector3(1.8, 0.6, 4.0)
	cs.position = Vector3(0, 0.45, 0)
	car.add_child(cs)
	for k in 4:
		var w := PhysXVehicleWheel3D.new()
		w.position = Vector3(0.8 if k % 2 == 0 else -0.8, 0.0, 1.3 if k < 2 else -1.3)
		w.radius = 0.35
		w.half_width = 0.13
		w.use_as_steering = k < 2
		w.use_as_traction = true
		car.add_child(w)
	car.position = at
	add_child(car)
	return car


# Drives a car straight at a steady throttle for p_seconds; returns [distance, mean sinkage, mean |slip|].
func _drive(car: PhysXVehicle3D, p_seconds: float, p_throttle := 1.0) -> Array:
	for i in 30:
		await get_tree().physics_frame
	var start := car.global_position
	car.throttle = p_throttle
	var sink := 0.0
	var slip := 0.0
	var n := 0
	for i in int(p_seconds * 60.0):
		await get_tree().physics_frame
		for w in 4:
			sink += car.get_wheel_sinkage(w)
			slip += absf(car.get_wheel_slip(w))
			n += 1
	car.throttle = 0.0
	var d := car.global_position - start
	return [Vector2(d.x, d.z).length(), sink / maxi(n, 1), slip / maxi(n, 1)]


func _soil_scene(kind_name: String) -> void:
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply(kind_name)
	var floor_body := StaticBody3D.new()
	var fc := CollisionShape3D.new()
	fc.shape = BoxShape3D.new()
	(fc.shape as BoxShape3D).size = Vector3(400, 2, 400)
	fc.position = Vector3(0, -1, 0)
	floor_body.add_child(fc)
	add_child(floor_body)


func _run_soil_tests() -> void:
	await _soil_scene("dry_sand")
	var o := Vector3.ZERO
	# One wheel: 3.5 kN on a 0.35 m radius, 0.26 m wide, rigid (inflation 0).
	var w: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	var ref := _bekker_sinkage(3500.0, 0.35, 0.26, 1.1, 0.99, 1528.43)
	_check("rigid wheel sinkage, dry sand (Wong table 2.2)", absf(w["sinkage"] - ref) < 0.002 and ref > 0.04, "%.1f cm, Bekker %.1f cm" % [w["sinkage"] * 100.0, ref * 100.0])
	var rc: float = w["compaction"]
	var rc_ref := 0.26 * (0.99 / 0.26 + 1528.43) * 1000.0 * pow(ref, 2.1) / 2.1
	_check("compaction resistance", absf(rc - rc_ref) < 0.05 * rc_ref, "%.0f N (Bekker %.0f N), bulldozing %.0f N, %.0f%% of the load in all" % [rc, rc_ref, w["bulldozing"], 100.0 * w["resistance"] / 3500.0])
	var aired: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 50.0, 0.0, 0.0)
	var hard: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 240.0, 0.0, 0.0)
	_check("airing down sinks less", aired["sinkage"] < hard["sinkage"] * 0.8 and aired["resistance"] < hard["resistance"], "50 kPa %.1f cm, %.0f N; 240 kPa %.1f cm, %.0f N" % [aired["sinkage"] * 100.0, aired["resistance"], hard["sinkage"] * 100.0, hard["resistance"]])
	var s50: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.5, 0.0)
	_check("slip sinkage (Lyasko 2010)", absf(s50["sinkage"] / w["sinkage"] - 2.0) < 0.02, "%.1f cm at 50%% slip, %.2f times" % [s50["sinkage"] * 100.0, s50["sinkage"] / w["sinkage"]])
	# Janosi-Hanamoto: traction grows with slip toward the shear limit.
	var curve := []
	var rising := true
	var last := -1.0
	for sl in [0.02, 0.05, 0.1, 0.2, 0.4, 0.8]:
		var t: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, sl, 0.0)
		var share: float = t["traction"] / t["shear_limit"]
		rising = rising and share > last
		last = share
		curve.append("%.0f%%: %.2f" % [sl * 100.0, share])
	_check("traction rises with slip (Janosi-Hanamoto)", rising and last > 0.9, ", ".join(curve))
	var lim: float = w["shear_limit"]
	_check("dry sand shear limit near tan(phi) W", lim > 3500.0 * 0.5 and lim < 3500.0 * 0.75, "%.0f N for 3500 N" % lim)
	surface.moisture = 0.4
	var damp: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	_check("damp sand: firmer and grippier", damp["sinkage"] < w["sinkage"] * 0.6 and damp["shear_limit"] > lim, "%.1f cm, %.0f N resistance, shear limit %.0f N" % [damp["sinkage"] * 100.0, damp["resistance"], damp["shear_limit"]])
	# Multi-pass: in the rut already pressed, little work is left.
	surface.moisture = 0.0
	surface.clear_deformation()
	surface.press_wheel(o, Vector2(0.26, 0.6), 0.0, w["sinkage"])
	surface.sync_deformation()
	var again: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	_check("second pass in the rut", again["resistance"] < 0.3 * w["resistance"], "%.0f N against %.0f N on fresh sand" % [again["resistance"], w["resistance"]])

	# Walker footing.
	surface.clear_deformation()
	var f_dry: Dictionary = surface.get_footing_at(o)
	surface.moisture = 0.4
	var f_damp: Dictionary = surface.get_footing_at(o)
	surface.moisture = 0.0
	_check("walking effort (Lejeune 1998: 1.6 to 2.5 on dry sand)", f_dry["effort"] > 1.6 and f_dry["effort"] < 2.5 and f_damp["effort"] < 1.5, "dry %.2f, damp %.2f" % [f_dry["effort"], f_damp["effort"]])
	_check("no slide on the flat", (f_dry["slide"] as Vector3).length() < 1e-4, "%.3f m/s2" % (f_dry["slide"] as Vector3).length())
	await _setup("slope", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var sp := Vector3(1.0, height.get_height(Vector3(1.0, 0, 0)), 0.0)
	var f_slope: Dictionary = surface.get_footing_at(sp)
	var slide: Vector3 = f_slope["slide"]
	_check("a footing on a 50 degree sand face slides down it", slide.length() > 2.0 and slide.x < 0.0 and slide.y < 0.0, "%.1f m/s2, %s" % [slide.length(), slide])

	# A 300 kg stone block (0.4 m) shoved across dry sand at 3 m/s on a 0.3 friction floor: the sand it sinks
	# into and ploughs (1.7 cm, Bekker) stops it sooner.
	var slides := []
	for plough in [false, true]:
		await _soil_scene("dry_sand")
		surface.ploughing = plough
		var pm := PhysicsMaterial.new()
		pm.friction = 0.3
		for c in get_children():
			if c is StaticBody3D:
				c.physics_material_override = pm
		var crate := RigidBody3D.new()
		crate.mass = 300.0
		crate.physics_material_override = pm
		var cc := CollisionShape3D.new()
		cc.shape = BoxShape3D.new()
		(cc.shape as BoxShape3D).size = Vector3(0.4, 0.4, 0.4)
		crate.add_child(cc)
		crate.add_to_group("ground_deform")
		crate.position = Vector3(0, 0.201, 0)
		add_child(crate)
		surface.follow_target = surface.get_path_to(crate)
		for i in 10:
			await get_tree().physics_frame
		crate.linear_velocity = Vector3(3.0, 0, 0)
		for i in 180:
			await get_tree().physics_frame
		slides.append(crate.global_position.x)
	_check("a stone block shoved over dry sand ploughs to a stop sooner", slides[1] < 0.9 * slides[0], "%.2f m against %.2f m on the floor's friction alone" % [slides[1], slides[0]])

	# A 1500 kg 4x4, 700 N m a wheel, for 4 s at half throttle (easing in, as one drives on sand): firm ground,
	# damp sand, dry sand at road pressure (200 kPa) and aired down to 60 kPa; then floored on dry sand.
	var results := {}
	for k in ["firm", "damp_sand", "dry_sand", "dry_sand_aired", "dry_sand_floored"]:
		await _soil_scene("damp_sand" if k == "damp_sand" else "dry_sand")
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		if k == "firm":
			car.soft_soil = false
		if k.begins_with("dry_sand_"):
			for c in car.get_children():
				if c is PhysXVehicleWheel3D:
					c.tire_pressure = 60.0
		surface.follow_target = surface.get_path_to(car)
		var start := car.global_position
		results[k] = await _drive(car, 4.0, 1.0 if k == "dry_sand_floored" else 0.5)
		surface.sync_deformation()
		# The deepest rut along the way.
		var rut := 0.0
		var fwd := car.get_forward()
		var side := fwd.cross(Vector3.UP).normalized()
		var steps := int(results[k][0] / 0.1)
		for j in range(-16, steps + 17):
			var q0 := start + fwd * (j * 0.1)
			for x in 41:
				var q := q0 + side * (x - 20) * 0.05
				rut = maxf(rut, surface.get_dent_at(q) - surface.get_rim_at(q))
		results[k].append(rut)
		print("  %s: %.1f m in 4 s, sinkage %.1f cm, slip %.2f, rut %.1f cm" % [k, results[k][0], results[k][1] * 100.0, results[k][2], rut * 100.0])
	_check("damp sand drives, slower than firm ground (motion resistance 0.15 to 0.3 of the load on sand, Wong table 1.1)",
			results["damp_sand"][0] > 3.0 and results["damp_sand"][0] < 0.5 * results["firm"][0], "%.1f m against %.1f m" % [results["damp_sand"][0], results["firm"][0]])
	_check("dry sand at road pressure: the car does not get going", results["dry_sand"][0] < 1.0 and results["dry_sand"][1] > 0.08,
			"%.1f m, sinkage %.1f cm" % [results["dry_sand"][0], results["dry_sand"][1] * 100.0])
	_check("aired down to 60 kPa it sinks less and gets further", results["dry_sand_aired"][0] > 3.0 * results["dry_sand"][0] and results["dry_sand_aired"][1] < 0.7 * results["dry_sand"][1],
			"%.1f m, sinkage %.1f cm" % [results["dry_sand_aired"][0], results["dry_sand_aired"][1] * 100.0])
	_check("floored, the wheels spin and dig in (slip sinkage)", results["dry_sand_floored"][2] > 0.5 and results["dry_sand_floored"][1] > 1.5 * results["dry_sand_aired"][1],
			"slip %.2f, sinkage %.1f cm" % [results["dry_sand_floored"][2], results["dry_sand_floored"][1] * 100.0])
	_check("wheels print ruts", results["damp_sand"][3] > 0.01 and results["dry_sand_aired"][3] > 0.03, "damp %.1f cm, dry aired down %.1f cm" % [results["damp_sand"][3] * 100.0, results["dry_sand_aired"][3] * 100.0])
	_check("firm ground: no sinkage", results["firm"][1] == 0.0, "%.2f cm" % (results["firm"][1] * 100.0))
	print("step %.2f ms, field on the %s" % [surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU"])


func _ground_material(kind_name: String) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	match kind_name:
		"dry_sand":
			m.albedo_color = Color(0.78, 0.68, 0.5)
		"damp_sand":
			m.albedo_color = Color(0.6, 0.5, 0.36)
		"mud":
			m.albedo_color = Color(0.3, 0.24, 0.18)
			m.roughness = 0.6
		"litter":
			m.albedo_color = Color(0.45, 0.3, 0.16)
		_:
			m.albedo_color = Color(0.42, 0.34, 0.25)
	if kind_name != "mud":
		m.roughness = 0.95
	var nt := NoiseTexture2D.new()
	nt.seamless = true
	nt.as_normal_map = true
	nt.bump_strength = 4.0 if kind_name != "litter" else 12.0
	nt.width = 512
	nt.height = 512
	var nz := FastNoiseLite.new()
	nz.frequency = 0.03 if kind_name != "litter" else 0.08
	nz.fractal_octaves = 5
	nt.noise = nz
	m.normal_enabled = true
	m.normal_texture = nt
	# World-mapped, as a terrain material is: the patch and the terrain line up.
	m.uv1_triplanar = true
	m.uv1_world_triplanar = true
	m.uv1_scale = Vector3(0.5, 0.5, 0.5)
	m.ground_deform_enabled = true
	return m


func _add_terrain(extent: float, cells: int, mat: Material) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step := extent / cells
	var h0 := -extent * 0.5
	for z in cells + 1:
		for x in cells + 1:
			var p := Vector3(h0 + x * step, 0, h0 + z * step)
			p.y = height.get_height(p)
			st.add_vertex(p)
	for z in cells:
		for x in cells:
			var i := z * (cells + 1) + x
			st.add_index(i)
			st.add_index(i + 1)
			st.add_index(i + cells + 1)
			st.add_index(i + 1)
			st.add_index(i + cells + 2)
			st.add_index(i + cells + 1)
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	mi.material_override = mat
	add_child(mi)
	# Bodies stand on the undisturbed ground, as on a terrain's own collider.
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = mi.mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)


func _undisturbed(p: Vector3) -> float:
	return height.get_height(p)


func _build_trail() -> void:
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = false
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-18.0, 140.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(1.2, 1.05, 3.4)
	cam.look_at(Vector3(-1.2, 0.0, 0.7))
	cam.fov = 55.0
	# The walker: a capsule on the ground's collider, its two boots (boxes in ground_deform, 80 kg on
	# 0.035 m2) printing by Bekker sinkage; a ball and a cart wheel roll through at set depths.
	var dark := StandardMaterial3D.new()
	dark.albedo_color = Color(0.15, 0.17, 0.22)
	walker = CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.14
	(ws.shape as CapsuleShape3D).height = 1.6
	walker.add_child(ws)
	walker.position = Vector3(-7.0, 1.0, 1.3)
	add_child(walker)
	for k in 2:
		var foot := Node3D.new()
		foot.name = "Foot%d" % k
		var fs := CollisionShape3D.new()
		fs.shape = BoxShape3D.new()
		(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
		foot.add_child(fs)
		foot.set_meta("ground_mass", 80.0)
		foot.set_meta("ground_contact_area", 0.035)
		foot.add_to_group("ground_deform")
		foot.position = Vector3(0, 100, 0)
		add_child(foot)
	var ball := Node3D.new()
	ball.name = "Ball"
	var bc := CollisionShape3D.new()
	bc.shape = SphereShape3D.new()
	(bc.shape as SphereShape3D).radius = 0.25
	ball.add_child(bc)
	var bmi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.25
	sm.height = 0.5
	bmi.mesh = sm
	var ball_mat := StandardMaterial3D.new()
	ball_mat.albedo_color = Color(0.6, 0.12, 0.08)
	bmi.material_override = ball_mat
	ball.add_child(bmi)
	ball.add_to_group("ground_deform")
	ball.position = Vector3(0, 100, 0)
	add_child(ball)
	var wheel := Node3D.new()
	wheel.name = "Wheel"
	var wc := CollisionShape3D.new()
	wc.shape = CylinderShape3D.new()
	(wc.shape as CylinderShape3D).radius = 0.4
	(wc.shape as CylinderShape3D).height = 0.12
	wc.rotation_degrees = Vector3(90, 0, 0)
	wheel.add_child(wc)
	var wmi := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.4
	cyl.bottom_radius = 0.4
	cyl.height = 0.12
	wmi.mesh = cyl
	wmi.rotation_degrees = Vector3(90, 0, 0)
	wmi.material_override = dark
	wheel.add_child(wmi)
	wheel.set_meta("ground_mass", 150.0)
	wheel.add_to_group("ground_deform")
	wheel.position = Vector3(0, 100, 0)
	add_child(wheel)
	walking = true
	for i in int(6.5 * 60.0):
		await get_tree().physics_frame
	walking = false
	# Let the walls settle as they would in the second after.
	for i in 60:
		await get_tree().physics_frame
	surface.sync_deformation()
	print("ground %s: step %.2f ms on the %s, moved %s m3" % [kind, surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU", surface.get_moved_volume()])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


func _build_ruts() -> void:
	if not "--ground=" in " ".join(OS.get_cmdline_user_args()):
		kind = "damp_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = false
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-16.0, 150.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var car := _make_car(1500.0, 700.0, Vector3(-1.0, _undisturbed(Vector3(-1.0, 0, -9.0)) + 0.9, -9.0))
	if kind == "dry_sand":
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				c.tire_pressure = 60.0
	surface.follow_target = surface.get_path_to(car)
	# Nothing to see while it drives: draw only the final frame.
	RenderingServer.render_loop_enabled = false
	for i in 30:
		await get_tree().physics_frame
	# A gentle S through the frame, then on out of it.
	car.throttle = 0.5
	var t := 0.0
	while t < 9.0 and car.global_position.z < 14.0:
		car.steer = 0.25 * sin(t * 0.8)
		await get_tree().physics_frame
		t += 1.0 / 60.0
	car.throttle = 0.0
	car.brake = 1.0
	# The camera on the ruts, low, the car gone past; the field follows the camera now.
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(1.6, _undisturbed(Vector3(1.6, 0, -3.5)) + 0.9, -3.5)
	cam.look_at(Vector3(-0.6, _undisturbed(Vector3(-0.6, 0, 1.5)), 1.5))
	cam.fov = 55.0
	car.queue_free()
	surface.follow_target = surface.get_path_to(cam)
	for i in 60:
		await get_tree().physics_frame
	surface.sync_deformation()
	RenderingServer.render_loop_enabled = true
	print("ruts in %s: car at %s, moved %s m3, field on the %s" % [kind, car.global_position, surface.get_moved_volume(), "GPU" if surface.is_gpu_active() else "CPU"])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


func _physics_process(delta: float) -> void:
	if not walking or walker == null:
		return
	walk_t += delta
	var x := -7.0 + 1.4 * walk_t
	var z := 1.3 + 0.5 * sin(walk_t * 0.9)
	walker.velocity = (Vector3(x, walker.global_position.y, z) - walker.global_position) / delta
	walker.velocity.y = -2.0
	walker.move_and_slide()
	var step := int(walk_t * 1.4 / 0.7)
	for k in 2:
		var foot: Node3D = get_node("Foot%d" % k)
		var planted := step % 2 == k
		if planted:
			var sx := -7.0 + step * 0.7 + 0.35
			var st := (sx + 7.0) / 1.4
			var sz := 1.3 + 0.5 * sin(st * 0.9)
			var side := 0.12 if k == 0 else -0.12
			var hd := atan2(-0.45 * cos(st * 0.9), 1.4)
			var p := Vector3(sx, 0, sz) + Vector3(sin(hd), 0, cos(hd)) * side
			if foot.get_meta("step", -1) != step:
				foot.set_meta("step", step)
				foot.rotation = Vector3(0, hd + PI * 0.5, 0)
				foot.global_position = Vector3(p.x, _undisturbed(p) + 0.03, p.z)
		else:
			foot.global_position = Vector3(0, 100, 0)
	var b: Node3D = get_node("Ball")
	var bp := Vector3(-6.0 + 1.2 * walk_t, 0, -0.6)
	b.global_position = Vector3(bp.x, _undisturbed(bp) - 0.06 + 0.25, bp.z)
	b.rotation.z = -bp.x / 0.25
	var w: Node3D = get_node("Wheel")
	var wp := Vector3(-8.0 + 1.7 * walk_t, 0, -2.4)
	w.global_position = Vector3(wp.x, _undisturbed(wp) - 0.03 + 0.4, wp.z)
	w.rotation.z = -wp.x / 0.4


func _save_frame() -> void:
	for i in 12:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)

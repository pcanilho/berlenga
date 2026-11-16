extends Node3D
# Buoyancy bench: bodies of different shapes and densities dropped on calm water (PhysXBuoyancy3D in
# volume_mode Shapes with mass_from_densities), raster. Each body's submerged share should settle at its
# density over the water's (Archimedes), a steel block should sink and a steel shell round air should float.
#
#   --out=path.png   save a frame after --frames and quit (default 300 frames at 30 fps: 10 s)
#   --frames=N
#   --waves          a 6 m/s wind instead of calm water
#   --report         print each body's submerged share against the expected one every 2 s
#   --still          the bodies don't disturb the water (their own dimple lowers them a little)
#
# Run with --fixed-fps 30.

var out := ""
var frames := 300
var report := false
var waves := false
var water: Node3D
var bodies := []
var n := 0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a == "--report": report = true
		elif a == "--waves": waves = true
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = ProceduralSkyMaterial.new()
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 30, 0)
	sun.shadow_enabled = true
	add_child(sun)
	water = ClassDB.instantiate("PhysXWaterSurface3D")
	water.set("domain_size", Vector2(24, 24))
	water.set("grid_resolution", 96)
	water.set("depth", 6.0)
	water.set("wind_speed", 6.0 if waves else 0.0)
	water.set("wave_amplitude", 0.3 if waves else 0.0)
	add_child(water)
	# name, shapes [[kind, size, offset, density]], expected submerged share (-1: sinks)
	_add("wood box", [["box", Vector3(1.0, 0.5, 0.6), Vector3(), 500.0]], 0.5, Vector3(-6, 1, 0))
	_add("ice ball", [["sphere", 0.5, Vector3(), 917.0]], 0.917, Vector3(-3, 1, 0))
	_add("log", [["cylinder", Vector2(0.25, 2.0), Vector3(), 600.0]], 0.6, Vector3(0, 1, 0), Vector3(0, 0, 90))
	_add("cork capsule", [["capsule", Vector2(0.3, 1.2), Vector3(), 240.0]], 0.24, Vector3(3, 1, 0), Vector3(90, 0, 0))
	_add("steel block", [["box", Vector3(0.5, 0.5, 0.5), Vector3(), 7850.0]], -1.0, Vector3(6, 1, 0))
	# A steel hull: a 4 x 1 x 1.6 m air box under 2 cm steel plates (bottom and sides).
	var hull := [["box", Vector3(4.0, 1.0, 1.6), Vector3(0, 0.0, 0), 0.0],
		["box", Vector3(4.0, 0.02, 1.6), Vector3(0, -0.51, 0), 7850.0],
		["box", Vector3(4.0, 1.0, 0.02), Vector3(0, 0.0, 0.81), 7850.0],
		["box", Vector3(4.0, 1.0, 0.02), Vector3(0, 0.0, -0.81), 7850.0],
		["box", Vector3(0.02, 1.0, 1.6), Vector3(2.01, 0.0, 0), 7850.0],
		["box", Vector3(0.02, 1.0, 1.6), Vector3(-2.01, 0.0, 0), 7850.0]]
	_add("steel hull", hull, -2.0, Vector3(0, 1, 5))
	var cam := Camera3D.new()
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 4.5, 13), Vector3(0, -0.3, 1.5))


func _add(p_name: String, p_shapes: Array, p_expect: float, p_at: Vector3, p_rot := Vector3()) -> void:
	var body := RigidBody3D.new()
	body.name = p_name.replace(" ", "_")
	body.position = p_at
	body.rotation_degrees = p_rot
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color.from_hsv(randf(), 0.5, 0.8)
	for s in p_shapes:
		var cs := CollisionShape3D.new()
		var mi := MeshInstance3D.new()
		match s[0]:
			"box":
				var b := BoxShape3D.new(); b.size = s[1]; cs.shape = b
				var m := BoxMesh.new(); m.size = s[1]; mi.mesh = m
			"sphere":
				var b := SphereShape3D.new(); b.radius = s[1]; cs.shape = b
				var m := SphereMesh.new(); m.radius = s[1]; m.height = 2.0 * s[1]; mi.mesh = m
			"cylinder":
				var b := CylinderShape3D.new(); b.radius = s[1].x; b.height = s[1].y; cs.shape = b
				var m := CylinderMesh.new(); m.top_radius = s[1].x; m.bottom_radius = s[1].x; m.height = s[1].y; mi.mesh = m
			"capsule":
				var b := CapsuleShape3D.new(); b.radius = s[1].x; b.height = s[1].y; cs.shape = b
				var m := CapsuleMesh.new(); m.radius = s[1].x; m.height = s[1].y; mi.mesh = m
		cs.position = s[2]
		mi.position = s[2]
		cs.set_meta("buoyancy_density", s[3])
		mi.material_override = mat
		body.add_child(cs)
		if s[3] > 0.0:
			body.add_child(mi)
	var by: Node3D = ClassDB.instantiate("PhysXBuoyancy3D")
	by.set("volume_mode", 1)
	by.set("mass_from_densities", true)
	if OS.get_cmdline_user_args().has("--still"): by.set("disturb_water", false)
	body.add_child(by)
	add_child(body)
	by.set("water_surface_path", by.get_path_to(water))
	bodies.append([p_name, body, by, p_expect])


func _physics_process(_delta: float) -> void:
	n += 1
	if report and n % 60 == 0:
		for e in bodies:
			var by: Node = e[2]
			var b: RigidBody3D = e[1]
			print("t=%4.1f %-12s mass %8.1f kg  volume %6.3f m3  submerged %.3f (expect %s)  y %.2f" % [n / 30.0, e[0], b.mass, by.call("get_total_volume"), by.call("get_submerged_fraction"), ("sinks" if e[3] == -1.0 else ("%.3f" % e[3] if e[3] >= 0.0 else "mass / water")), b.global_position.y])
	if n >= frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
			print("saved ", out)
		get_tree().quit()

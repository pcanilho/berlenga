extends Node3D
# Water surface bench: the PhysX module's PhysXWaterSurface3D (FFT ocean, ripples, shore) over a
# sloping sand floor, with a few floating boxes, raster or path traced. Needs a build with the
# PhysX module (modules/godot_physx/README.md).
#
#   --view=beach    the shore from above: shallows, swash and the sea beyond (default)
#   --view=under    looking down into the shallows, where the floor shows through
#   --raster        raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --frames=N      frames rendered before --out is saved (default 12)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/refraction:0
#   --out=path.png  save the last frame and quit
#   --debug=N       path tracing debug view (Environment.pathtracing_debug_mode)
#   --water=param:value  a water shader parameter, e.g. --water=pathtraced_scattering:Vector3(0,0,0)
#   --slope=DEG     the sand's slope (default 5); steeper reaches deep water sooner
#
# Run with --fixed-fps 30 so raster and path traced frames show the same waves.

var view := "beach"
var raster := false
var spp := 16
var bounces := 3
var frames := 12
var out := ""
var debug_mode := 0
var slope_deg := 5.0
var water_params := {}
var n := 0
var water: Node3D


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--debug="): debug_mode = int(a.substr(8))
		elif a.begins_with("--slope="): slope_deg = float(a.substr(8))
		elif a.begins_with("--water="):
			var wkv := a.substr(8).split(":", true, 1)
			water_params[wkv[0]] = str_to_var(wkv[1])
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":", true, 1)
			ProjectSettings.set_setting(kv[0], str_to_var(kv[1]))

	var sky_mat := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_debug_mode = debug_mode
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -140, 0)
	sun.shadow_enabled = true
	add_child(sun)

	# Sand: a 200 m slab sloping down away from the camera, from dry beach out to about 9 m of water, with a few rocks.
	var sand_mat := StandardMaterial3D.new()
	sand_mat.albedo_texture = _checker(Color(0.78, 0.7, 0.55), Color(0.62, 0.55, 0.42))
	sand_mat.uv1_scale = Vector3(100, 100, 1)
	sand_mat.roughness = 0.95
	var slope := deg_to_rad(slope_deg)
	_slab(Vector3(200, 1, 200), Vector3(0, -1.2, 0), Vector3(-slope, 0, 0), sand_mat)
	var rock_mat := StandardMaterial3D.new()
	rock_mat.albedo_color = Color(0.3, 0.3, 0.32)
	rock_mat.roughness = 0.8
	for p in [Vector3(-2.5, -1.4, -4.0), Vector3(1.5, -1.9, -7.0), Vector3(3.0, -0.9, -1.5)]:
		_slab(Vector3(1.2, 1.2, 1.2), p, Vector3(0.3, 0.7, 0.2), rock_mat)

	if not ClassDB.class_exists("PhysXWaterSurface3D"):
		push_error("This build has no PhysX module.")
		get_tree().quit(1)
		return
	water = ClassDB.instantiate("PhysXWaterSurface3D")
	water.set("domain_size", Vector2(40, 40))
	water.set("grid_resolution", 160)
	water.set("seabed_from_floor", true)
	water.set("ocean_grid_resolution", 128)
	water.set("wind_speed", 9.0)
	water.set("wind_direction", Vector2(-0.3, 1.0))
	water.set("choppiness", 1.0)
	water.set("render_extent", 600.0)
	add_child(water)
	var wm: ShaderMaterial = water.call("get_water_material")
	for k in water_params:
		if wm: wm.set_shader_parameter(k, water_params[k])

	# Floating boxes.
	for i in 3:
		var body := RigidBody3D.new()
		body.mass = 40.0
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(0.8, 0.4, 0.8)
		cs.shape = box
		body.add_child(cs)
		var mi := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = box.size
		mi.mesh = bm
		var m := StandardMaterial3D.new()
		m.albedo_color = [Color(0.8, 0.3, 0.2), Color(0.85, 0.75, 0.3), Color(0.3, 0.5, 0.8)][i]
		mi.material_override = m
		body.add_child(mi)
		var b: Node3D = ClassDB.instantiate("PhysXBuoyancy3D")
		body.add_child(b)
		body.position = Vector3(-2.0 + 2.0 * i, 0.3, -3.0 - 2.0 * i)
		add_child(body)

	var cam := Camera3D.new()
	cam.fov = 45
	add_child(cam)
	match view:
		"under":
			cam.look_at_from_position(Vector3(0, 3.5, 5.0), Vector3(0, -1.0, -1.5))
		_:
			cam.look_at_from_position(Vector3(0, 4.5, 12.0), Vector3(0, -0.5, -6.0))


func _checker(a: Color, b: Color) -> ImageTexture:
	var img := Image.create(8, 8, true, Image.FORMAT_RGB8)
	for y in 8:
		for x in 8:
			img.set_pixel(x, y, a if (x / 4 + y / 4) % 2 == 0 else b)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _slab(p_size: Vector3, p_pos: Vector3, p_rot: Vector3, p_mat: Material) -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = p_size
	cs.shape = shape
	body.add_child(cs)
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = p_size
	mi.mesh = bm
	mi.material_override = p_mat
	body.add_child(mi)
	body.position = p_pos
	body.rotation = p_rot
	add_child(body)


func _process(_delta: float) -> void:
	n += 1
	if out != "" and n >= frames:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

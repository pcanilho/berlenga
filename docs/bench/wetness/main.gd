extends Node3D
# BaseMaterial3D Wetness, raster against path traced. Spheres with porosity 0 to 1 (left to
# right) on a porous ground. The weather wetness globals are set by hand, so the frames are
# deterministic; --weather uses a raining WeatherMap3D instead.
#
# godot --path docs/bench/wetness --rendering-driver vulkan -- [options]
#   --mode=off      wetness feature off (the frame before this change)
#   --mode=dry      feature on, weather map dry (must equal off)
#   --mode=wet      feature on, weather map fully wet
#   --mode=half     wet where x < 0 only (the map drives it)
#   --mode=manual   wetness_from_weather off, wetness_amount 1
#   --weather       a WeatherMap3D raining instead of hand-set globals (checks they reach materials)
#   --raster        raster only (default path traced)
#   --spp=N         samples per pixel (default 32)
#   --out=path.png

var mode := "wet"
var raster := false
var weather := false
var spp := 32
var out := ""
var n := 0
var mats: Array[StandardMaterial3D] = []
var state_tex: ImageTexture

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--mode="): mode = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--weather": weather = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.5, 0.6, 0.75)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.45, 0.55)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, -30, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.6, 6.0)
	cam.rotation_degrees = Vector3(-12, 0, 0)
	add_child(cam)

	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(16, 16)
	var gm := _material(Color(0.42, 0.36, 0.3), 0.85, 0.9)
	pm.material = gm
	ground.mesh = pm
	add_child(ground)
	for i in 5:
		var mi := MeshInstance3D.new()
		var sm := SphereMesh.new()
		sm.radius = 0.6
		sm.height = 1.2
		sm.material = _material(Color(0.55, 0.5, 0.45), 0.6, i / 4.0)
		mi.mesh = sm
		mi.position = Vector3((i - 2) * 1.5, 0.6, 0)
		add_child(mi)

	if weather:
		var wm := WeatherMap3D.new()
		add_child(wm)
		wm.rain_rate = 8.0
		wm.set("time_scale", 3600.0)
	else:
		_set_globals()

func _material(c: Color, roughness: float, porosity: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = roughness
	if mode != "off":
		m.wetness_enabled = true
		m.wetness_porosity = porosity
		m.wetness_from_weather = mode != "manual"
	mats.append(m)
	return m

func _set_globals() -> void:
	var size := 64
	var img := Image.create(size, size, false, Image.FORMAT_RGBAH)
	for y in size:
		for x in size:
			var wet := 0.0
			match mode:
				"wet": wet = 1.0
				"half": wet = 1.0 if x < size / 2 else 0.0
			img.set_pixel(x, y, Color(0.0, wet, 0.0, 0.0))
	state_tex = ImageTexture.create_from_image(img)
	RenderingServer.global_shader_parameter_set("weather_state_map", state_tex.get_rid())
	RenderingServer.global_shader_parameter_set("weather_map_rect", Vector4(-8, -8, 16, 16))

func _process(_d: float) -> void:
	n += 1
	if n < (40 if weather else 12):
		return
	set_process(false)
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

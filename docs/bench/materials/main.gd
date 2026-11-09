extends Node3D
# Material test scene for the path tracer: rows of spheres under a sun and a sky, to compare
# the path tracer with the rasterizer material by material.
#
# godot --path docs/bench/materials -- [options]
#   --scene=NAME      clearcoat (default): red paint, coat strength 0 to 1 left to right,
#                     coat roughness 0 (back row) and 1 (front row)
#                     backlight: leaf-green cards against a low sun, back light 0 to 1 left to right
#                     transmission: glass panes (Transmission) before coloured spheres, roughness 0 to 1
#                     left to right, green tint in the back row
#                     textures: layer textures and custom shader outputs, sun behind. Left to right:
#                     coat stripes (clearcoat_texture), a card back lit through a checker (backlight_texture),
#                     a pane let through by a checker (transmission_texture), custom TRANSMISSION and
#                     custom BACKLIGHT
#   --raster          raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 64, 3)
#   --frames=N        frames rendered before --out is saved (default 8)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/clearcoat:0
#   --out=path.png    save the last frame and quit
#
# Software Vulkan (lavapipe) needs shader execution reordering off, read at startup, so put
# it in an override.cfg next to project.godot (not --set):
#   [rendering]
#   pathtracing/use_shader_execution_reordering=false
var scene := "clearcoat"
var raster := false
var spp := 64
var bounces := 3
var frames := 8
var out := ""
var n := 0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--scene="): scene = a.substr(8)
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":", true, 1)
			ProjectSettings.set_setting(kv[0], str_to_var(kv[1]))

	var sky_mat := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, -150, 0)
	sun.shadow_enabled = true
	add_child(sun)

	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	ground.mesh = plane
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.35, 0.35, 0.35)
	ground_mat.roughness = 0.9
	ground.material_override = ground_mat
	add_child(ground)

	var cam := Camera3D.new()
	cam.fov = 40
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 3.2, 8.5), Vector3(0, 0.6, 0))

	match scene:
		"clearcoat":
			_clearcoat_rows()
		"transmission":
			_transmission_panes()
		"backlight":
			sun.rotation_degrees = Vector3(-12, 180, 0)
			_backlight_cards()
		"textures":
			sun.rotation_degrees = Vector3(-25, 165, 0)
			_layer_textures()
		_:
			push_error("Unknown scene: " + scene)


func _sphere(p_pos: Vector3, p_mat: Material) -> void:
	var mi := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.5
	sphere.height = 1.0
	sphere.radial_segments = 96
	sphere.rings = 48
	mi.mesh = sphere
	mi.material_override = p_mat
	mi.position = p_pos
	add_child(mi)


func _clearcoat_rows() -> void:
	for row in 2:
		for i in 5:
			var m := StandardMaterial3D.new()
			m.albedo_color = Color(0.55, 0.03, 0.03)
			m.roughness = 0.6
			m.clearcoat_enabled = true
			m.clearcoat = i / 4.0
			m.clearcoat_roughness = 0.0 if row == 0 else 1.0
			_sphere(Vector3((i - 2) * 1.25, 0.5, -1.0 + row * 1.6), m)


func _backlight_cards() -> void:
	for i in 5:
		var m := StandardMaterial3D.new()
		m.albedo_color = Color(0.25, 0.5, 0.12)
		m.roughness = 0.7
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.backlight_enabled = true
		m.backlight = Color(1, 1, 1) * (i / 4.0)
		var mi := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(1.0, 1.4)
		mi.mesh = quad
		mi.material_override = m
		mi.position = Vector3((i - 2) * 1.25, 0.8, 0.0)
		add_child(mi)
	# A curtain the sun shines through, for its coloured shadow.
	var c := StandardMaterial3D.new()
	c.albedo_color = Color(0.8, 0.25, 0.2)
	c.cull_mode = BaseMaterial3D.CULL_DISABLED
	c.backlight_enabled = true
	c.backlight = Color(0.6, 0.6, 0.6)
	var curtain := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 1.2)
	curtain.mesh = quad
	curtain.material_override = c
	curtain.position = Vector3(0.0, 1.0, -3.0)
	add_child(curtain)


func _transmission_panes() -> void:
	var colors := [Color(0.8, 0.1, 0.1), Color(0.9, 0.7, 0.1), Color(0.1, 0.6, 0.2), Color(0.1, 0.3, 0.8), Color(0.7, 0.7, 0.7)]
	for i in 5:
		var s := StandardMaterial3D.new()
		s.albedo_color = colors[i]
		_sphere(Vector3((i - 2) * 1.25, 0.5, -2.0), s)
	for row in 2:
		for i in 5:
			var m := StandardMaterial3D.new()
			m.albedo_color = Color(1, 1, 1) if row == 0 else Color(0.55, 0.85, 0.6)
			m.roughness = i / 4.0 * 0.6
			m.transmission_enabled = true
			m.transmission_amount = 1.0
			m.cull_mode = BaseMaterial3D.CULL_DISABLED
			var mi := MeshInstance3D.new()
			var quad := QuadMesh.new()
			quad.size = Vector2(1.1, 0.5)
			mi.mesh = quad
			mi.material_override = m
			mi.position = Vector3((i - 2) * 1.25, 0.25 + row * 0.55, 0.0)
			add_child(mi)


# A 64 px checker of 8 px cells, p_on in one cell colour and p_off in the other.
func _checker(p_on: Color, p_off: Color) -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			img.set_pixel(x, y, p_on if ((x / 8) + (y / 8)) % 2 == 0 else p_off)
	return ImageTexture.create_from_image(img)


func _quad(p_size: Vector2, p_pos: Vector3, p_mat: Material) -> void:
	var mi := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = p_size
	mi.mesh = quad
	mi.material_override = p_mat
	mi.position = p_pos
	add_child(mi)


func _layer_textures() -> void:
	# Coat in stripes: red channel 1 or 0, green (roughness scale) 0.
	var coat := StandardMaterial3D.new()
	coat.albedo_color = Color(0.55, 0.03, 0.03)
	coat.roughness = 0.6
	coat.clearcoat_enabled = true
	coat.clearcoat = 1.0
	coat.clearcoat_roughness = 1.0
	var stripes := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			stripes.set_pixel(x, y, Color(1, 0, 0) if (x / 8) % 2 == 0 else Color(0, 0, 0))
	coat.clearcoat_texture = ImageTexture.create_from_image(stripes)
	coat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_sphere(Vector3(-3.0, 0.5, 0.0), coat)

	# Back light only where the checker is white.
	var card := StandardMaterial3D.new()
	card.albedo_color = Color(0.25, 0.5, 0.12)
	card.cull_mode = BaseMaterial3D.CULL_DISABLED
	card.backlight_enabled = true
	card.backlight = Color(0, 0, 0)
	card.backlight_texture = _checker(Color(1, 1, 1), Color(0, 0, 0))
	card.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_quad(Vector2(1.2, 1.2), Vector3(-1.5, 0.8, 0.0), card)

	# Glass only where the checker is red.
	var pane := StandardMaterial3D.new()
	pane.albedo_color = Color(0.55, 0.85, 0.6)
	pane.roughness = 0.0
	pane.transmission_enabled = true
	pane.transmission_amount = 1.0
	pane.transmission_texture = _checker(Color(1, 0, 0), Color(0, 0, 0))
	pane.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	pane.cull_mode = BaseMaterial3D.CULL_DISABLED
	_quad(Vector2(1.2, 1.2), Vector3(0.0, 0.8, 0.0), pane)

	# Custom shaders writing TRANSMISSION and BACKLIGHT.
	var glass := Shader.new()
	glass.code = "shader_type spatial;\nrender_mode cull_disabled;\nvoid fragment() {\n\tALBEDO = vec3(0.3, 0.5, 0.9);\n\tROUGHNESS = 0.0;\n\tTRANSMISSION = 1.0;\n}\n"
	var glass_mat := ShaderMaterial.new()
	glass_mat.shader = glass
	_quad(Vector2(1.2, 1.2), Vector3(1.5, 0.8, 0.0), glass_mat)
	var leaf := Shader.new()
	leaf.code = "shader_type spatial;\nrender_mode cull_disabled;\nvoid fragment() {\n\tALBEDO = vec3(0.8, 0.25, 0.2);\n\tROUGHNESS = 0.8;\n\tBACKLIGHT = vec3(0.6);\n}\n"
	var leaf_mat := ShaderMaterial.new()
	leaf_mat.shader = leaf
	_quad(Vector2(1.2, 1.2), Vector3(3.0, 0.8, 0.0), leaf_mat)

	var colors := [Color(0.8, 0.1, 0.1), Color(0.9, 0.7, 0.1), Color(0.1, 0.3, 0.8)]
	for i in 3:
		var b := StandardMaterial3D.new()
		b.albedo_color = colors[i]
		_sphere(Vector3(-1.5 + i * 1.5, 0.5, -1.6), b)


func _process(_delta: float) -> void:
	n += 1
	if n == frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

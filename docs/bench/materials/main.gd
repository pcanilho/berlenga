extends Node3D
# Material test scene for the path tracer: rows of spheres under a sun and a sky, to compare
# the path tracer with the rasterizer material by material.
#
# godot --path docs/bench/materials -- [options]
#   --scene=NAME      clearcoat (default): red paint, coat strength 0 to 1 left to right,
#                     coat roughness 0 (back row) and 1 (front row)
#   --raster          raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 64, 3)
#   --frames=N        frames rendered before --out is saved (default 8)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/clearcoat:0
#   --out=path.png    save the last frame and quit
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


func _process(_delta: float) -> void:
	n += 1
	if n == frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

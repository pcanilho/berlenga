extends Node3D
# Neutral showcase scenes for the README. Args after --:
# --shot=city|lights|textures|glass --spp=N --out=path.png --frames=N --far=N --raster
var shot := "lights"
var spp := 16
var out := "out.png"
var frames := 3
var far := 0
var raster := false
var n := 0
var env := Environment.new()

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--shot="): shot = a.substr(7)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--far="): far = int(a.substr(6))
		elif a == "--raster": raster = true
	seed(11)
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0, 0, 0)
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	env.pathtracing_denoiser = 0
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	call("shot_" + shot)

func mat(c: Color, r := 0.6) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = r
	return m

func box(size: Vector3, pos: Vector3, m: Material) -> MeshInstance3D:
	var b := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	bm.material = m
	b.mesh = bm
	b.position = pos
	add_child(b)
	return b

func plane(size: Vector2, m: Material) -> MeshInstance3D:
	var g := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = size
	pm.material = m
	g.mesh = pm
	add_child(g)
	return g

func camera(pos: Vector3, look: Vector3, fov := 55.0) -> void:
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = pos
	cam.look_at(look)
	cam.fov = fov
	cam.far = 12000.0
	cam.current = true

func emissive(c: Color, e: float) -> StandardMaterial3D:
	var m := mat(c)
	m.emission_enabled = true
	m.emission = c
	m.emission_energy_multiplier = e
	var img := Image.create(4, 4, false, Image.FORMAT_RGB8)
	img.fill(Color.WHITE)
	m.emission_texture = ImageTexture.create_from_image(img)
	return m

# A long night street: two rows of lamp posts, 4 m apart for 600 m (300 lamps), plus --far lamps over 10 km.
func shot_lights() -> void:
	env.tonemap_exposure = 1.6
	plane(Vector2(40, 2600), mat(Color(0.35, 0.35, 0.36), 0.35)).position = Vector3(0, 0, -600)
	var bulb := emissive(Color(1.0, 0.8, 0.55), 8.0)
	var pole := mat(Color(0.2, 0.2, 0.2), 0.5)
	for side in [-1, 1]:
		box(Vector3(2, 6, 2500), Vector3(side * 16, 3, -1200), mat(Color(0.45, 0.43, 0.4)))
		for i in 150:
			var z := 10.0 - i * 8.0 - (4.0 if side > 0 else 0.0)
			var x: float = side * 6.0
			box(Vector3(0.12, 4.0, 0.12), Vector3(x, 2.0, z), pole)
			box(Vector3(0.3, 0.12, 0.3), Vector3(x, 4.06, z), bulb)
			var l := OmniLight3D.new()
			l.position = Vector3(x, 3.85, z)
			l.omni_range = 10.0
			l.light_energy = 3.0
			l.light_color = Color(1.0, 0.78, 0.5)
			l.light_size = 0.1
			l.shadow_enabled = true
			add_child(l)
	for i in far:
		var fl := OmniLight3D.new()
		fl.position = Vector3(randf_range(-5000, 5000), 3.0, randf_range(-5000, 5000))
		fl.omni_range = 9.0
		fl.light_energy = 2.0
		add_child(fl)
	camera(Vector3(0, 4.0, 18), Vector3(0, 2.0, -200), 50)

# Blocks of a city at night: 1024 street lamps on a 32 x 32 grid, 14 m apart, seen from above.
func shot_city() -> void:
	env.tonemap_exposure = 1.6
	plane(Vector2(600, 600), mat(Color(0.35, 0.35, 0.36), 0.4))
	var bulb := emissive(Color(1.0, 0.8, 0.55), 8.0)
	var bm := mat(Color(0.5, 0.48, 0.45), 0.8)
	for bx in 16:
		for bz in 16:
			var h := randf_range(3.0, 14.0)
			box(Vector3(20, h, 20), Vector3((bx - 7.5) * 28.0, h * 0.5, (bz - 7.5) * 28.0), bm)
	for x in 32:
		for z in 32:
			var p := Vector3((x - 15.5) * 14.0, 4.0, (z - 15.5) * 14.0)
			# keep lamps in the streets between blocks
			p.x = (floor(x / 2.0) - 7.5) * 28.0 + 14.0
			p.z = (z - 15.5) * 14.0
			if z % 2 == 0:
				p = Vector3((x - 15.5) * 14.0, 4.0, (floor(z / 2.0) - 7.5) * 28.0 + 14.0)
			box(Vector3(0.4, 0.15, 0.4), p + Vector3(0, 0.1, 0), bulb)
			var l := OmniLight3D.new()
			l.position = p
			l.omni_range = 12.0
			l.light_energy = 3.0
			l.light_color = Color.from_hsv(randf_range(0.05, 0.12), randf_range(0.2, 0.6), 1.0)
			l.light_size = 0.1
			l.shadow_enabled = true
			add_child(l)
	camera(Vector3(0, 90, 170), Vector3(0, 0, -20), 55)

# A big floor with a fine checker and line grid, seen at a grazing angle.
func shot_textures() -> void:
	env.background_color = Color(0.55, 0.6, 0.7)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.45, 0.5)
	var s := 1024
	var img := Image.create(s, s, false, Image.FORMAT_RGB8)
	for y in s:
		for x in s:
			var c := 0.82 if ((x / 32) + (y / 32)) % 2 == 0 else 0.25
			if x % 128 < 3 or y % 128 < 3: c = 0.05
			if (x + y) % 64 == 0: c = 0.95
			img.set_pixel(x, y, Color(c, c * 0.95, c * 0.9))
	img.generate_mipmaps()
	var m := mat(Color.WHITE, 0.8)
	m.albedo_texture = ImageTexture.create_from_image(img)
	m.uv1_scale = Vector3(40, 40, 1)
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	plane(Vector2(400, 400), m).position = Vector3(0, 0, -150)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.light_energy = 1.5
	add_child(sun)
	camera(Vector3(0, 2.0, 40), Vector3(0, 0, -60), 60)

# A clear glass pane in sunlight over a white floor.
func shot_glass() -> void:
	env.background_color = Color(0.5, 0.55, 0.65)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.3, 0.32, 0.35)
	plane(Vector2(30, 30), mat(Color(0.85, 0.85, 0.85), 0.7))
	var g := mat(Color(0.75, 0.9, 0.85, 0.25), 0.05)
	g.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	box(Vector3(4, 3, 0.05), Vector3(0, 1.6, 0), g)
	var frame := mat(Color(0.15, 0.15, 0.15), 0.5)
	box(Vector3(4.2, 0.1, 0.1), Vector3(0, 3.15, 0), frame)
	box(Vector3(4.2, 0.1, 0.1), Vector3(0, 0.05, 0), frame)
	box(Vector3(0.1, 3.2, 0.1), Vector3(-2.05, 1.6, 0), frame)
	box(Vector3(0.1, 3.2, 0.1), Vector3(2.05, 1.6, 0), frame)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45, 20, 0)
	sun.light_energy = 2.0
	sun.shadow_enabled = true
	add_child(sun)
	camera(Vector3(5, 4.5, -8), Vector3(0, 0.5, 2), 55)

func _process(_d: float) -> void:
	n += 1
	if n == frames:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

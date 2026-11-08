extends Node3D
# Light selection test rigs. Args after --:
# --scene=street|city --bounces=N --spp=N --out=path.png --frames=N --spacing=M --far=N
# street: the open street of lamps from the light-tree rig (little occlusion).
# city: blocks of closed buildings with lamps inside them (hidden from the street but in
#   range), street lamps, a moon, and an arcade roof that hides the moon from part of the street.
# Prints the mean GPU render time of the last half of the frames.
var scene := "street"
var bounces := 2
var interior := true
var exposure := 1.0
var spp := 4
var spacing := 4.0
var far := 0
var gpu_ms := 0.0
var gpu_n := 0
var out := "out.png"
var frames := 3
var n := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--spacing="): spacing = float(a.substr(10))
		elif a.begins_with("--far="): far = int(a.substr(6))
		elif a.begins_with("--scene="): scene = a.substr(8)
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a == "--no-interior": interior = false
		elif a.begins_with("--exposure="): exposure = float(a.substr(11))
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	seed(7)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.pathtracing_enabled = true
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_denoiser = 0
	env.tonemap_exposure = exposure
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	if scene == "city":
		_city()
	elif scene == "signs":
		_signs()
	else:
		_street()

func _box(pos: Vector3, size: Vector3, col := Color(0.5, 0.5, 0.5)) -> void:
	var b := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	var m := StandardMaterial3D.new()
	m.albedo_color = col
	m.roughness = 0.6
	bm.material = m
	b.mesh = bm
	b.position = pos
	add_child(b)

func _omni(pos: Vector3, rng: float, energy: float, col: Color) -> void:
	var l := OmniLight3D.new()
	l.position = pos
	l.omni_range = rng
	l.light_energy = energy
	l.light_color = col
	l.light_size = 0.05
	l.shadow_enabled = true
	add_child(l)

# signs: a dark street lit only by emissive surfaces (shop signs, a lit window, a screen
# with a texture), over a glossy wet floor and a rough one, plus one small street lamp.
func _signs() -> void:
	var floor_m := StandardMaterial3D.new()
	floor_m.albedo_color = Color(0.25, 0.25, 0.27)
	floor_m.roughness = 0.15
	var f := MeshInstance3D.new()
	var fm := BoxMesh.new()
	fm.size = Vector3(40, 0.1, 60)
	fm.material = floor_m
	f.mesh = fm
	f.position = Vector3(0, -0.05, 0)
	add_child(f)
	var walk := StandardMaterial3D.new()
	walk.albedo_color = Color(0.5, 0.48, 0.45)
	walk.roughness = 0.8
	for side in [-1, 1]:
		_box(Vector3(side * 7.0, 0.1, 0), Vector3(4, 0.2, 60), Color(0.5, 0.48, 0.45))
		_box(Vector3(side * 10.0, 6, 0), Vector3(2, 12, 60), Color(0.45, 0.42, 0.4))
	var tex := Image.create(64, 16, false, Image.FORMAT_RGBA8)
	for x in 64:
		for y in 16:
			var on := (x / 4 + y / 4) % 3 != 0 and y > 1 and y < 14
			tex.set_pixel(x, y, Color(1, 1, 1) if on else Color(0, 0, 0))
	var etex := ImageTexture.create_from_image(tex)
	for i in 10:
		var side := -1 if i % 2 == 0 else 1
		var z := -24.0 + i * 5.0 + randf_range(-1, 1)
		var q := MeshInstance3D.new()
		var qm := QuadMesh.new()
		qm.size = Vector2(randf_range(1.0, 3.0), randf_range(0.4, 1.0))
		var m := StandardMaterial3D.new()
		m.albedo_color = Color(0.05, 0.05, 0.05)
		m.emission_enabled = true
		m.emission = Color.from_hsv(randf(), 0.8, 1.0)
		m.emission_energy_multiplier = randf_range(2.0, 8.0)
		if i % 3 == 0:
			m.emission_texture = etex
		qm.material = m
		q.mesh = qm
		q.position = Vector3(side * 8.9, randf_range(2.5, 4.5), z)
		q.rotation_degrees = Vector3(0, -90 * side, 0)
		add_child(q)
	# a lit shop window (box, all sides emit) and a big screen high up
	var win := MeshInstance3D.new()
	var wm := BoxMesh.new()
	wm.size = Vector3(0.2, 2.0, 4.0)
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.1, 0.1, 0.1)
	wmat.emission_enabled = true
	wmat.emission = Color(1.0, 0.8, 0.55)
	wmat.emission_energy_multiplier = 3.0
	wm.material = wmat
	win.mesh = wm
	win.position = Vector3(-8.9, 1.4, 6)
	add_child(win)
	_omni(Vector3(6, 4, -10), 9.0, 1.5, Color(1.0, 0.85, 0.6))
	var cam := Camera3D.new()
	cam.position = Vector3(0, 2.2, 22)
	cam.rotation_degrees = Vector3(-6, 0, 0)
	cam.fov = 65
	add_child(cam)
	cam.current = true

func _city() -> void:
	_box(Vector3(0, -0.05, 0), Vector3(200, 0.1, 200), Color(0.35, 0.35, 0.35))
	var block := 14.0
	var street := 10.0
	var pitch := block + street
	for bx in range(-3, 4):
		for bz in range(-3, 4):
			var c := Vector3(bx * pitch, 0, bz * pitch)
			var h := randf_range(8, 24)
			_box(c + Vector3(0, h * 0.5, 0), Vector3(block, h, block), Color.from_hsv(randf_range(0.0, 0.15), 0.2, randf_range(0.4, 0.7)))
			# lights inside the closed building, in range of the street outside
			for i in 6:
				var p := c + Vector3(randf_range(-5, 5), randf_range(2, h - 1), randf_range(-5, 5))
				var e := randf_range(2, 6)
				if interior:
					_omni(p, 14.0, e, Color(1.0, 0.85, 0.6))
	# street lamps along the streets between blocks, both directions
	for k in range(-3, 3):
		var s := (k + 0.5) * pitch
		for t in range(-80, 81, 8):
			_omni(Vector3(s - 4.0, 4.0, t), 9.0, randf_range(1.0, 3.0), Color.from_hsv(randf_range(0.05, 0.6), 0.5, 1.0))
			_omni(Vector3(t, 4.0, s + 4.0), 9.0, randf_range(1.0, 3.0), Color.from_hsv(randf_range(0.05, 0.6), 0.5, 1.0))
	# an arcade roof over part of the street the camera looks down: the moon is hidden under it
	_box(Vector3(12, 5.0, 10), Vector3(10, 0.3, 30), Color(0.4, 0.4, 0.45))
	var moon := DirectionalLight3D.new()
	moon.rotation_degrees = Vector3(-55, 30, 0)
	moon.light_energy = 0.4
	moon.light_color = Color(0.7, 0.8, 1.0)
	moon.shadow_enabled = true
	add_child(moon)
	var cam := Camera3D.new()
	cam.position = Vector3(12, 7, 34)
	cam.rotation_degrees = Vector3(-12, 0, 0)
	cam.fov = 70
	add_child(cam)
	cam.current = true

func _street() -> void:
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(80, 80)
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.5, 0.5, 0.5)
	gm.roughness = 0.35
	pm.material = gm
	ground.mesh = pm
	add_child(ground)
	for i in 30:
		var b := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(randf_range(1, 4), randf_range(1, 6), randf_range(1, 4))
		b.mesh = bm
		b.position = Vector3(randf_range(-30, 30), bm.size.y * 0.5, randf_range(-30, 30))
		add_child(b)
	for x in 20:
		for z in 20:
			var l := OmniLight3D.new()
			l.position = Vector3((x - 9.5) * spacing, 3.0, (z - 9.5) * spacing)
			l.omni_range = 7.0
			l.light_energy = randf_range(0.5, 3.0)
			l.light_color = Color.from_hsv(randf(), 0.6, 1.0)
			l.light_size = 0.05
			l.shadow_enabled = true
			add_child(l)
	for i in far:
		var fl := OmniLight3D.new()
		fl.position = Vector3(randf_range(-5000, 5000), 3.0, randf_range(-5000, 5000))
		fl.omni_range = 7.0
		fl.light_energy = 2.0
		add_child(fl)
	for i in 60:
		var s := SpotLight3D.new()
		s.position = Vector3(randf_range(-35, 35), 4.0, randf_range(-35, 35))
		s.rotation_degrees = Vector3(-90 + randf_range(-30, 30), randf_range(0, 360), 0)
		s.spot_range = 10.0
		s.spot_angle = randf_range(15, 45)
		s.light_energy = 4.0
		s.shadow_enabled = true
		add_child(s)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 18, 26)
	cam.rotation_degrees = Vector3(-40, 0, 0)
	cam.fov = 60
	add_child(cam)
	cam.current = true

func _process(_d: float) -> void:
	n += 1
	if n > frames / 2:
		gpu_ms += RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid())
		gpu_n += 1
	if n == frames:
		print("RIG gpu_ms=%.3f frames=%d spp=%d scene=%s" % [gpu_ms / max(gpu_n, 1), gpu_n, spp, scene])
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

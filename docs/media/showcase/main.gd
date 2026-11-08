extends Node3D
# Neutral showcase scenes for the README. Args after --:
# --shot=city|lights|textures|glass|physx --spp=N --rr --out=path.png --frames=N --far=N --raster
var shot := "lights"
var spp := 16
var out := "out.png"
var frames := 3
var far := 0
var raster := false
var rr := false
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
		elif a == "--rr": rr = true
	seed(11)
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0, 0, 0)
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	env.pathtracing_denoiser = 0
	if rr:
		# DLSS Ray Reconstruction: needs DLSS scaling on the viewport (RTX GPU, Windows).
		env.pathtracing_denoiser = RenderingServer.PT_DENOISER_DLSS_RAY_RECONSTRUCTION
		get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
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

# PhysX module: a flag in gusty wind, a Blast wall a ball breaks, a crate stack, a smoke plume.
# Needs a build with the PhysX module; record a clip with --write-movie and --frames=N.
func shot_physx() -> void:
	if not ClassDB.class_exists("PhysXCloth3D"):
		push_error("This build has no PhysX module.")
		get_tree().quit(1)
		return
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.12, 0.12, 0.14)
	camera(Vector3(0.3, 2.3, 6.6), Vector3(0, 1.2, -0.4), 50)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 160, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	add_child(sun)
	var lamp := OmniLight3D.new()
	lamp.position = Vector3(2.4, 1.6, 1.2)
	lamp.light_color = Color(1, 0.6, 0.3)
	lamp.light_energy = 5
	lamp.omni_range = 7
	lamp.shadow_enabled = true
	add_child(lamp)
	var floor_mesh := plane(Vector2(40, 40), mat(Color(0.5, 0.5, 0.52), 0.8))
	var ground := StaticBody3D.new()
	var gs := CollisionShape3D.new()
	gs.shape = WorldBoundaryShape3D.new()
	ground.add_child(gs)
	add_child(ground)
	floor_mesh.name = "Floor"

	# Flag on a pole: a 24 x 16 cloth grid pinned along its pole edge.
	box(Vector3(0.08, 3.2, 0.08), Vector3(-3.6, 1.6, -0.5), mat(Color(0.3, 0.3, 0.32), 0.4))
	var flag = ClassDB.instantiate("PhysXCloth3D")
	flag.grid_size = Vector2(1.8, 1.1)
	flag.grid_columns = 24
	flag.grid_rows = 16
	flag.pin_mode = 3 # left edge
	flag.wind = Vector3(12.0, 0.5, 2.0)
	flag.wind_turbulence = 0.6
	flag.position = Vector3(-2.66, 2.6, -0.5)
	var flag_mat := mat(Color(0.85, 0.22, 0.16), 0.7)
	flag_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	flag.material_override = flag_mat
	add_child(flag)

	# Blast wall: a box fractured into 40 Voronoi cells, grounded, broken by a ball.
	var wall_mesh := BoxMesh.new()
	wall_mesh.size = Vector3(2.0, 1.6, 0.25)
	var authoring = ClassDB.instantiate("PhysXBlastAuthoring")
	var asset = authoring.fracture_mesh(wall_mesh, 40, 7)
	var wall = ClassDB.instantiate("PhysXDestructible3D")
	wall.blast_asset = asset
	wall.anchor = 1 # grounded
	wall.shatter_speed = 2.5
	wall.piece_lifetime = 0.0
	wall.position = Vector3(0.2, 0.8, 0)
	wall.material_override = mat(Color(0.72, 0.68, 0.62), 0.9)
	add_child(wall)
	var ball := RigidBody3D.new()
	var bc := CollisionShape3D.new()
	var bs := SphereShape3D.new()
	bs.radius = 0.25
	bc.shape = bs
	ball.add_child(bc)
	var bm := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.25
	sm.height = 0.5
	sm.material = mat(Color(0.15, 0.15, 0.17), 0.3)
	bm.mesh = sm
	ball.add_child(bm)
	ball.mass = 40.0
	ball.position = Vector3(0.2, 1.0, 6.0)
	ball.freeze = true
	add_child(ball)
	ball.contact_monitor = true
	ball.max_contacts_reported = 4
	ball.body_entered.connect(func(_b: Node) -> void:
		if not wall.is_fractured() and ball.position.z < 1.0:
			wall.apply_radial_damage(ball.position + Vector3(0, 0, -0.2), 2.5, 0.35, 0.75))
	get_tree().create_timer(1.0, true, true).timeout.connect(func() -> void:
		ball.freeze = false
		ball.linear_velocity = Vector3(0, 1.0, -14.0))

	# Crate stack the debris runs into.
	var crate_mat := mat(Color(0.55, 0.42, 0.28), 0.8)
	for i in 6:
		var c := RigidBody3D.new()
		var cs := CollisionShape3D.new()
		var cb := BoxShape3D.new()
		cb.size = Vector3(0.5, 0.5, 0.5)
		cs.shape = cb
		c.add_child(cs)
		var cm := MeshInstance3D.new()
		var cbm := BoxMesh.new()
		cbm.size = cb.size
		cbm.material = crate_mat
		cm.mesh = cbm
		c.add_child(cm)
		var level := 0 if i < 3 else (1 if i < 5 else 2)
		var slot := i - (0 if level == 0 else (3 if level == 1 else 5))
		c.position = Vector3(-0.32 + slot * 0.52 + level * 0.26, 0.25 + level * 0.5, -1.6)
		add_child(c)

	# Smoke plume beside the lamp.
	var flow = ClassDB.instantiate("PhysXFlow3D")
	flow.smoke_density = 4.0
	flow.position = Vector3(3.2, 0, -2.0)
	add_child(flow)
	var em = ClassDB.instantiate("PhysXFlowEmitter3D")
	em.position = Vector3(0, 0.3, 0)
	em.radius = 0.5
	em.velocity = Vector3(0.6, 2.2, 0)
	em.smoke = 1.0
	em.temperature = 0.3
	flow.add_child(em)
	em.flow = em.get_path_to(flow)

func _process(_d: float) -> void:
	n += 1
	if n == frames:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

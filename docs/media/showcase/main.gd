extends Node3D
# Neutral showcase scenes for the README. Args after --:
# --shot=city|lights|plaza|room|textures|glass|far|night|physx|tearing --spp=N --rr --out=path.png --frames=N --far=N --raster
# --set=setting:value (project setting, e.g. --set=rendering/camera/auto_exposure/metering:1)
# --bounces=N (path tracing bounces after the shot sets its own; 0: direct light only)
var shot := "lights"
var spp := 16
var out := "out.png"
var frames := 3
var far := 0
var raster := false
var rr := false
var bounces := -1
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
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":")
			ProjectSettings.set_setting(kv[0], float(kv[1]))
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
	if bounces >= 0:
		env.pathtracing_max_bounces = bounces

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

# A closed room lit by the sun through one window: a bright patch on the floor, and everything else
# (walls, ceiling, the far corner, a table and a cabinet) lit only by light bounced off it.
func shot_room() -> void:
	env.tonemap_exposure = 2.0
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 110, 0)
	sun.light_energy = 3.0
	sun.shadow_enabled = true
	add_child(sun)
	var wall := mat(Color(0.75, 0.72, 0.66), 0.9)
	var floor_m := mat(Color(0.55, 0.42, 0.3), 0.7)
	var t := 0.3
	# floor, ceiling, back (-z), left (-x), right (+x) walls of an 8 x 3 x 6 m room
	box(Vector3(8 + 2 * t, t, 6 + 2 * t), Vector3(0, -t / 2, 0), floor_m)
	box(Vector3(8 + 2 * t, t, 6 + 2 * t), Vector3(0, 3 + t / 2, 0), wall)
	box(Vector3(8 + 2 * t, 3, t), Vector3(0, 1.5, -3 - t / 2), wall)
	box(Vector3(t, 3, 6), Vector3(-4 - t / 2, 1.5, 0), wall)
	# the right wall has a 1.6 x 1.4 m window, its sill 0.9 m up
	box(Vector3(t, 3, 2.2), Vector3(4 + t / 2, 1.5, -1.9), wall)
	box(Vector3(t, 3, 2.2), Vector3(4 + t / 2, 1.5, 1.9), wall)
	box(Vector3(t, 0.9, 1.6), Vector3(4 + t / 2, 0.45, 0), wall)
	box(Vector3(t, 0.7, 1.6), Vector3(4 + t / 2, 2.65, 0), wall)
	# front wall (+z) behind the camera, with the camera inside the room
	box(Vector3(8 + 2 * t, 3, t), Vector3(0, 1.5, 3 + t / 2), wall)
	var wood := mat(Color(0.35, 0.22, 0.14), 0.6)
	box(Vector3(1.6, 0.06, 0.9), Vector3(-1.5, 0.75, -1.2), wood)
	for x in [-2.2, -0.8]:
		for z in [-1.55, -0.85]:
			box(Vector3(0.06, 0.72, 0.06), Vector3(x, 0.36, z), wood)
	box(Vector3(1.2, 1.8, 0.5), Vector3(-3.3, 0.9, -2.5), mat(Color(0.3, 0.4, 0.5), 0.8))
	box(Vector3(0.5, 0.5, 0.5), Vector3(1.5, 0.25, 1.0), mat(Color(0.7, 0.2, 0.15), 0.5))
	camera(Vector3(2.6, 1.6, 2.7), Vector3(-2.0, 0.9, -2.0), 70)

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

# A square under festoon lights: 400 bulbs on strings 4 m up, each reaching 30 m, so every point
# sees a hundred or more lights at once, with columns and kiosks casting shadows among them.
func shot_plaza() -> void:
	env.tonemap_exposure = 1.2
	plane(Vector2(120, 120), mat(Color(0.4, 0.38, 0.36), 0.5))
	var stone := mat(Color(0.55, 0.52, 0.48), 0.8)
	for i in 6:
		for j in 6:
			if (i + j) % 2 == 0:
				box(Vector3(0.8, 5.0, 0.8), Vector3((i - 2.5) * 7.0, 2.5, (j - 2.5) * 7.0), stone)
			elif i % 3 == 1:
				box(Vector3(2.5, 2.2, 1.6), Vector3((i - 2.5) * 7.0, 1.1, (j - 2.5) * 7.0), stone)
	for k in 400:
		var row := k / 20
		var col := k % 20
		var p := Vector3((col - 9.5) * 2.2, 4.2 - 0.4 * sin(PI * (col + 0.5) / 20.0), (row - 9.5) * 2.2)
		var l := OmniLight3D.new()
		l.position = p
		l.omni_range = 30.0
		l.light_energy = 0.25
		l.light_color = Color.from_hsv(randf(), 0.5, 1.0)
		l.light_size = 0.05
		l.shadow_enabled = true
		add_child(l)
	camera(Vector3(0, 1.8, 26), Vector3(0, 1.5, 0), 60)

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
	# One wind for the scene: the flag and the smoke both get it.
	var wind := Vector3(4.5, 0.2, 1.0)
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
	lamp.position = Vector3(3.4, 1.4, 1.6)
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
	flag.wind = wind
	flag.simulation_mode = 2 # CPU solver: tear() needs it
	flag.wind_turbulence = 0.6
	flag.position = Vector3(-2.66, 2.6, -0.5)
	var flag_mat := mat(Color(0.85, 0.22, 0.16), 0.7)
	flag_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	flag.material_override = flag_mat
	add_child(flag)
	# A gust rips the flag through the middle, from the free end toward the pole (tear() cuts
	# a lengthening line over half a second), leaving two tails.
	get_tree().create_timer(3.2, true, true).timeout.connect(func() -> void:
		flag.wind = wind * 2.5
		for k in 12:
			flag.tear(Vector3(1.0, 0.0, 0.0), Vector3(1.0 - 0.13 * (k + 1), 0.0, 0.0))
			await get_tree().create_timer(0.04, true, true).timeout)

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
	flow.smoke_density = 8.0
	flow.position = Vector3(2.5, 0, 0.6)
	add_child(flow)
	var em = ClassDB.instantiate("PhysXFlowEmitter3D")
	em.position = Vector3(0, 0.3, 0)
	em.radius = 0.8
	em.velocity = Vector3(0, 2.0, 0)
	em.smoke = 1.0
	em.temperature = 0.3
	flow.add_child(em)
	em.flow = em.get_path_to(flow)
	# The same wind for the gas: a box emitter that only pushes air.
	var air = ClassDB.instantiate("PhysXFlowEmitter3D")
	air.shape = 1 # box
	air.size = Vector3(8, 6, 8)
	air.position = Vector3(0, 3, 0)
	air.smoke = 0.0
	air.temperature = 0.0
	air.velocity = wind
	air.couple_rate = 0.5
	flow.add_child(air)
	air.flow = air.get_path_to(flow)
	# A static wall downwind: physics bodies are solids for the gas, so the smoke piles up
	# against it and spills round its sides and over its top.
	var screen := StaticBody3D.new()
	var ss := CollisionShape3D.new()
	var sb := BoxShape3D.new()
	sb.size = Vector3(0.15, 1.1, 2.4)
	ss.shape = sb
	screen.add_child(ss)
	var scm := MeshInstance3D.new()
	var sbm := BoxMesh.new()
	sbm.size = sb.size
	sbm.material = mat(Color(0.62, 0.64, 0.68), 0.6)
	scm.mesh = sbm
	screen.add_child(scm)
	screen.position = flow.position + Vector3(1.15, 0.55, 0.25)
	screen.rotation.y = -atan2(wind.z, wind.x)
	add_child(screen)

# A small lit scene --far metres from the world origin on x and z (try 65536), for
# rendering/pathtracing/relative_origin_grid on and off.
func shot_far() -> void:
	env.background_color = Color(0.25, 0.3, 0.4)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.pathtracing_max_bounces = 2
	plane(Vector2(40, 40), mat(Color(0.6, 0.6, 0.6), 0.9))
	var red := mat(Color(0.7, 0.15, 0.1), 0.5)
	box(Vector3(1, 1, 1), Vector3(-1, 0.5, 0), red)
	box(Vector3(2, 0.02, 0.5), Vector3(1, 0.01, 1), red)
	var shiny := mat(Color(0.8, 0.8, 0.8), 0.1)
	shiny.metallic = 1.0
	var s := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.5
	sm.height = 1.0
	sm.material = shiny
	s.mesh = sm
	s.position = Vector3(1, 0.5, -1)
	add_child(s)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 40, 0)
	sun.light_energy = 1.5
	sun.shadow_enabled = true
	add_child(sun)
	var o := OmniLight3D.new()
	o.position = Vector3(0, 0.4, 1.5)
	o.omni_range = 6
	o.light_energy = 2.0
	o.light_color = Color(1, 0.7, 0.4)
	o.shadow_enabled = true
	add_child(o)
	camera(Vector3(0, 2.5, 5), Vector3(0, 0.3, 0), 50)
	position = Vector3(far, 0, far)

# A night street with bright lamp bulbs in view, under auto exposure.
func shot_night() -> void:
	env.pathtracing_max_bounces = 2
	var ca := CameraAttributesPractical.new()
	ca.auto_exposure_enabled = true
	ca.auto_exposure_speed = 50.0
	ca.auto_exposure_scale = 0.4
	get_child(0).camera_attributes = ca
	plane(Vector2(200, 200), mat(Color(0.4, 0.4, 0.4), 0.8))
	var wall := mat(Color(0.55, 0.5, 0.45), 0.9)
	for i in 6:
		for side in [-1, 1]:
			var h := 8.0 + (i * 3 % 5)
			box(Vector3(6, h, 7), Vector3(side * 9.0, h / 2, -6.0 - i * 9.0), wall)
	var bulb := emissive(Color(1.0, 0.85, 0.6), 60.0)
	bulb.albedo_color = Color.BLACK
	var pole := mat(Color(0.15, 0.15, 0.15))
	for i in 4:
		for side in [-1, 1]:
			var z := -4.0 - i * 12.0
			var p := MeshInstance3D.new()
			var cm := CylinderMesh.new()
			cm.top_radius = 0.08
			cm.bottom_radius = 0.1
			cm.height = 4.5
			cm.material = pole
			p.mesh = cm
			p.position = Vector3(side * 5.0, 2.25, z)
			add_child(p)
			var s := MeshInstance3D.new()
			var sm := SphereMesh.new()
			sm.radius = 0.25
			sm.height = 0.5
			sm.material = bulb
			s.mesh = sm
			s.position = Vector3(side * 5.0, 4.6, z)
			s.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			add_child(s)
			var l := OmniLight3D.new()
			l.position = Vector3(side * 5.0, 4.3, z)
			l.light_color = Color(1.0, 0.85, 0.6)
			l.light_energy = 1.5
			l.omni_range = 14.0
			l.shadow_enabled = true
			add_child(l)
	camera(Vector3(0, 1.7, 6), Vector3(0, 2.5, -30), 60)

func _process(_d: float) -> void:
	n += 1
	if n == frames:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()


# Strain tearing (PhysXCloth3D.tear_factor): nothing is cut by script. A striped ribbon tied between
# two posts and a checked flag on a pole take a wind that rises to a gale; each tears where it is
# stretched past its tear factor, the ribbon off a post, the flag from its pole edge.
func shot_tearing() -> void:
	if not ClassDB.class_exists("PhysXCloth3D"):
		push_error("This build has no PhysX module.")
		get_tree().quit(1)
		return
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.25, 0.25, 0.28)
	camera(Vector3(0.4, 3.1, 5.0), Vector3(0.0, 0.9, -0.2), 52)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 160, 0)
	sun.light_energy = 2.2
	sun.shadow_enabled = true
	add_child(sun)
	plane(Vector2(40, 40), mat(Color(0.5, 0.5, 0.52), 0.8))
	var ground := StaticBody3D.new()
	var gs := CollisionShape3D.new()
	gs.shape = WorldBoundaryShape3D.new()
	ground.add_child(gs)
	add_child(ground)
	var post_mat := mat(Color(0.3, 0.3, 0.32), 0.4)

	# One wind for both, across the ribbon's line and along the flag's.
	var wind_dir := Vector3(1.0, 0.0, -1.0).normalized()
	# Ribbon: 4 m of 8 cm wide film, 3 vertices across and 15 cm rows, tied at both ends, its line square
	# to the wind.
	var line := Vector3(1.0, 0.0, 1.0).normalized()
	var mid := Vector3(0.4, 0.0, 0.4)
	box(Vector3(0.06, 1.1, 0.06), mid - line * 2.0 + Vector3(0, 0.55, 0), post_mat)
	box(Vector3(0.06, 1.1, 0.06), mid + line * 2.0 + Vector3(0, 0.55, 0), post_mat)
	var stripes := Image.create(64, 8, false, Image.FORMAT_RGB8)
	for x in 64:
		for y in 8:
			stripes.set_pixel(x, y, Color(0.95, 0.78, 0.1) if ((x + y) / 8) % 2 == 0 else Color(0.08, 0.08, 0.08))
	var rib_mat := mat(Color(1, 1, 1), 0.5)
	rib_mat.albedo_texture = ImageTexture.create_from_image(stripes)
	rib_mat.uv1_scale = Vector3(12, 1, 1)
	rib_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	# its rest shape hangs 12 cm slack between the ties (a taut line under its own weight would need
	# infinite tension), drawn in its local XY plane: +Y along it, +X up across its width
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	var idx := PackedInt32Array()
	var rows := 28
	for r in rows:
		var t := float(r) / (rows - 1)
		for j in 3:
			verts.append(Vector3(-0.12 * 4.0 * t * (1.0 - t) + 0.08 * (j / 2.0 - 0.5), 2.0 - 4.0 * t, 0.0))
			uvs.append(Vector2(t, j / 2.0))
	for r in rows - 1:
		for j in 2:
			var q := r * 3 + j
			idx.append_array([q, q + 1, q + 3, q + 1, q + 4, q + 3])
	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_TEX_UV] = uvs
	arr[Mesh.ARRAY_INDEX] = idx
	var rest := ArrayMesh.new()
	rest.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	var ribbon = ClassDB.instantiate("PhysXCloth3D")
	ribbon.simulation_mode = 2
	ribbon.source_mesh = rest
	ribbon.pin_mode = 0
	ribbon.pinned_vertices = PackedInt32Array([1, rows * 3 - 2])   # tied: held at its middle, free to turn
	ribbon.stiffness = 0.8
	ribbon.bend_stiffness = 0.05
	ribbon.density = 0.15
	ribbon.drag = 0.72
	ribbon.lift = 0.3
	ribbon.wind_turbulence = 0.2
	ribbon.substeps = 40                 # a long thin strip needs them to carry its load without solver stretch
	ribbon.tear_factor = 1.07             # holds in the breeze, parts in about 11 m/s (get_max_strain)
	# local +Y along its line (row 0 at the first post), +X up across its width, +Z its face
	ribbon.basis = Basis(Vector3.UP, -line, Vector3.UP.cross(-line))
	ribbon.position = mid + Vector3(0, 1.06, 0)
	ribbon.material_override = rib_mat
	add_child(ribbon)

	# Flag: a 24 x 16 grid on its pole, checked so its texture shows through the rip.
	box(Vector3(0.08, 3.2, 0.08), Vector3(-3.2, 1.6, -2.6), post_mat)
	var check := Image.create(8, 8, false, Image.FORMAT_RGB8)
	for x in 8:
		for y in 8:
			check.set_pixel(x, y, Color(0.85, 0.22, 0.16) if (x + y) % 2 == 0 else Color(0.92, 0.9, 0.86))
	var flag_mat := mat(Color(1, 1, 1), 0.7)
	flag_mat.albedo_texture = ImageTexture.create_from_image(check)
	flag_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	flag_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	var flag = ClassDB.instantiate("PhysXCloth3D")
	flag.simulation_mode = 2
	flag.grid_size = Vector2(1.8, 1.1)
	flag.grid_columns = 24
	flag.grid_rows = 16
	flag.pin_mode = 3 # left edge
	flag.wind_turbulence = 0.6
	flag.tear_factor = 1.6
	flag.position = Vector3(-2.26, 2.6, -2.6)
	flag.material_override = flag_mat
	add_child(flag)

	# The wind: a breeze, then a gale from 2.5 s.
	var clock := [0.0]   # simulated seconds (movie capture runs off the wall clock)
	var blow := func() -> void:
		clock[0] += 1.0 / Engine.physics_ticks_per_second
		var tt: float = clock[0]
		var u := lerpf(3.0, 22.0, clampf((tt - 2.5) / 2.0, 0.0, 1.0))
		ribbon.wind = wind_dir * u
		flag.wind = wind_dir * u
	get_tree().physics_frame.connect(blow)

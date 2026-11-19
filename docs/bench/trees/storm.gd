extends Node3D
# Storm bench for rigged trees (TreeWind3D storm breaks and windthrow, docs/ideas/trees.md 5t).
#
# godot --path docs/bench/trees --fixed-fps 30 -- --storm [options]
#   --test             checks (PASS/FAIL), quit
#   --out=dir          render frames into dir, then quit
#   --seconds=S        storm length (default 12)
#   --every=N          save every Nth frame (default 15); --at=T one frame at T s
#   --speed=U          mean wind at 10 m, m/s (default 30); --turbulence=I (default 0.25)
#   --anchorage=A      soil factor on root anchorage (default 1; wet soil 0.6)
#   --presets=a,b      species (default 4,1: Norway spruce, beech); --seed=S
#   --char=C           a past fire charred every stem foot C metres deep (a WorldFire stub), so they snap there
#   --snag=Y           a past fire killed every tree Y years ago (standing snags, rotting)
#   --spp=N with --pt  path traced; default raster
#   CAM="x,y,z,lx,ly,lz,fov" camera

var fails := 0
var wind: TreeWind3D
var events := []
var target_speed := 30.0
var ramp := 3.0 # Seconds the wind takes to rise from 12 m/s to the storm's speed.

func _process(_delta: float) -> void:
	if wind and ramp > 0.0:
		var t := wind.get_time()
		wind.fallback_wind = Vector2(lerpf(12.0, target_speed, clampf(t / ramp, 0.0, 1.0)), 0.0)

func _check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		fails += 1

func _on_damage(pos: Vector3, radius: float, uprooted: bool, pit: Dictionary) -> void:
	events.append({"pos": pos, "radius": radius, "uprooted": uprooted, "pit": pit, "t": wind.get_time()})
	print("storm: %s r=%.3f at (%.1f, %.1f, %.1f) t=%.2f %s" % ["thrown" if uprooted else "break", radius, pos.x, pos.y, pos.z, wind.get_time(), str(pit) if uprooted else ""])

func _tree(p: int, seed: int, at: Vector3) -> Dictionary:
	var g := TreeGenerator.new()
	g.preset = p
	g.seed = seed
	g.rigged = true
	g.needle_sprays = true
	var m := g.generate()
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = TreeWind3D.make_multimesh(m, [Transform3D(Basis(), at)])
	add_child(mmi)
	return {"mesh": m, "height": g.get_height(), "crown": g.get_crown_radius()}

var pt_env: Environment

func _world(pt: bool, spp: int) -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(600, 600)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)
	var sk := Sky.new()
	sk.sky_material = ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sk
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	# Path tracing only for the saved frames: the fall itself steps in raster (the cloud rebuilds
	# its acceleration structures every frame while trees move).
	env.pathtracing_enabled = false
	pt_env = env
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	get_viewport().msaa_3d = Viewport.MSAA_8X
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 35, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 200.0
	add_child(sun)

func _setup(args: Dictionary) -> Array:
	wind = TreeWind3D.new()
	wind.fallback_wind = Vector2(float(args.get("speed", "30")), 0.0)
	wind.fallback_turbulence = float(args.get("turbulence", "0.25"))
	wind.storm_anchorage = float(args.get("anchorage", "1"))
	wind.lod_enabled = false
	add_child(wind)
	wind.storm_damage.connect(_on_damage)
	if args.has("char") or args.has("snag"):
		var stub: WorldFire = load("res://fire_stub.gd").new()
		stub.char_m = float(args.get("char", "0"))
		stub.snag_years = float(args.get("snag", "-1"))
		Engine.get_singleton("WorldContracts").set_fire(stub)
	var picks: Array = [4, 1]
	if args.has("presets"):
		picks = []
		for v in String(args.presets).split(","):
			picks.append(int(v))
	# In a row across the view, far enough apart downwind (+X) that a thrown tree falls clear.
	var trees := []
	var x := 0.0
	for p in picks:
		var t := _tree(p, int(args.get("seed", "1")), Vector3(x, 0, 0))
		t["x"] = x
		trees.append(t)
		x += t.height * 1.3 + t.crown
	target_speed = float(args.get("speed", "30"))
	wind.target = wind.get_path_to(self)
	wind.rescan()
	return trees

func run(args: Dictionary) -> void:
	if args.has("test"):
		await _test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	_world(args.has("pt"), int(args.get("spp", "16")))
	var trees := _setup(args)
	var tallest := 0.0
	var span := 0.0
	for t in trees:
		tallest = maxf(tallest, t.height)
		span = t.x + t.height
	var cam := Camera3D.new()
	cam.fov = 45.0
	add_child(cam)
	var c := OS.get_environment("CAM")
	if c != "":
		var v := c.split(",")
		cam.position = Vector3(float(v[0]), float(v[1]), float(v[2]))
		cam.look_at(Vector3(float(v[3]), float(v[4]), float(v[5])))
		cam.fov = float(v[6])
	else:
		# From the side: the wind blows +X, left to right.
		cam.position = Vector3(span * 0.45, tallest * 0.35, span * 0.95 + 10.0)
		cam.look_at(Vector3(span * 0.45, tallest * 0.3, 0.0))
	var dir := String(args.get("out", "/tmp/storm"))
	DirAccess.make_dir_recursive_absolute(dir)
	var seconds := float(args.get("seconds", "12"))
	var every := int(args.get("every", "15"))
	var at := float(args.get("at", "-1"))
	var frames := int(seconds * 30.0)
	# Only the saved frames are drawn (the cloud's software renderer takes seconds a frame); the
	# physics and the wind step every frame (--fixed-fps 30).
	for i in 4:
		await RenderingServer.frame_post_draw
	RenderingServer.render_loop_enabled = false
	for f in frames:
		await get_tree().process_frame
		var t := f / 30.0
		if f % 15 == 0:
			print("t=%.1f wall %d ms parts %d falls %d" % [t, Time.get_ticks_msec(), _parts(), wind.get_storm_fall_count()])
			if OS.get_environment("STORM_DEBUG") == "1":
				for d in wind.get_children(true):
					if d.get_class() == "PhysXDestructible3D":
						var o: Array = []
						for k in mini(d.get_piece_count(), 3):
							var x: Transform3D = d.get_piece_transform(k)
							o.append("%s up %s" % [x.origin.snapped(Vector3.ONE * 0.1), x.basis.y.normalized().snapped(Vector3.ONE * 0.01)])
						print("  fall %s pieces %d %s" % [d.name, d.get_piece_count(), o])
		var save := (at < 0.0 and f % every == 0) or (at >= 0.0 and absf(t - at) < 0.5 / 30.0)
		if save:
			if args.has("pt"):
				pt_env.pathtracing_enabled = true
			for k in (int(args.get("spp", "16")) / 4 + 2 if args.has("pt") else 2):
				RenderingServer.force_draw(false)
			get_viewport().get_texture().get_image().save_png(dir.path_join("storm_%04d.png" % f))
			print("frame %d t=%.2f parts %d" % [f, t, _parts()])
	for st in wind.get_children(true):
		if String(st.name).begins_with("TreeStump"):
			for m in st.get_children():
				if m is MeshInstance3D:
					print("stump mesh %s scale %.2f aabb %s" % [m.mesh.get_class(), m.scale.x, m.mesh.get_aabb()])
	print("storm: %d breaks, %d thrown (%d snapped at the foot), %d Blast falls, %d snags" % [wind.get_storm_break_count(), wind.get_storm_throw_count(), wind.get_storm_snap_count(), wind.get_storm_fall_count(), wind.get_snag_count()])
	get_tree().quit(1 if fails > 0 else 0)

func _parts() -> int:
	var n := 0
	for c in wind.get_children(true):
		if c is RigidBody3D or c.get_class() == "PhysXDestructible3D":
			n += 1
	return n

func _test() -> void:
	# A breeze breaks nothing; a storm on wet soil breaks and throws, and what falls comes to rest
	# on the ground, not through it.
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)
	ramp = 0.0
	var trees := _setup({"speed": "10", "presets": "4,1,2"})
	for f in 300:
		await get_tree().physics_frame
	_check(wind.get_storm_break_count() == 0 and wind.get_storm_throw_count() == 0, "10 m/s for 10 s: nothing breaks (%d breaks, %d thrown)" % [wind.get_storm_break_count(), wind.get_storm_throw_count()])
	wind.fallback_wind = Vector2(34, 0)
	wind.storm_anchorage = 0.5
	for f in 600:
		await get_tree().physics_frame
	_check(wind.get_storm_break_count() + wind.get_storm_throw_count() > 0, "34 m/s on wet soil: %d breaks, %d thrown" % [wind.get_storm_break_count(), wind.get_storm_throw_count()])
	var low := 1e9
	var bodies := 0
	# Falling parts are bodies; landed ones become (internal) Blast destructibles.
	for c in wind.get_children(true):
		if c is RigidBody3D:
			bodies += 1
			low = minf(low, (c.global_transform * c.center_of_mass).y)
		elif c.has_method("get_piece_count"):
			for i in c.get_piece_count():
				bodies += 1
				low = minf(low, c.get_piece_center(i).y)
	_check(bodies > 0, "%d parts are bodies or Blast pieces" % bodies)
	_check(low > -2.0, "no part fell through the ground (lowest centre of mass %.2f m)" % low)
	for e in events:
		if e.uprooted:
			_check(e.pit.has("radius") and e.pit.radius > 0.4, "a thrown tree leaves a pit (radius %.2f m)" % e.pit.get("radius", 0.0))
			break

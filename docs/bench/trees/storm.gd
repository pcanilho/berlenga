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
#   --presets=a,b      species (default 4,1: Norway spruce, beech); --seed=S; --spacing=M between them (default clear)
#   --char=C           a past fire charred every stem foot C metres deep (a WorldFire stub), so they snap there
#   --snag=Y           a past fire killed every tree Y years ago (standing snags, rotting)
#   --avalanche=P      an avalanche puts P Pa on every tree from 1 s (WorldSnow stub), on a slope down +Z
#   --avalanche-test   avalanche checks (WorldSnow and WorldGround stubs): pressure breaks crowns, then stems,
#                      downslope, and the felled crowns leave the weather map's canopy
#   --snow=L           every crown holds L kg/m2 of snow from the start (WorldSnow stub), in a calm
#   --glaze-test       ice storm checks (WorldWeather stub's glaze_at): the ice's weight on each crown, limbs bowing
#                      under 6 mm, breaking under 25 mm (the spruce's cone sheds it best), the ice gone with the melt
#   --snow-test        snow-laden crown checks: the branches bow with the load and creep, heavy wet snow
#                      breaks them and the break shakes the snow off, and they spring back when it goes
#   --spp=N with --pt  path traced; default raster
#   CAM="x,y,z,lx,ly,lz,fov" camera

var fails := 0
var wind: TreeWind3D
var events := []
var target_speed := 30.0
var winter := false # Trees in their January state (bare broadleaves), for the snow checks.
var ramp := 3.0 # Seconds the wind takes to rise from 12 m/s to the storm's speed.

func _process(_delta: float) -> void:
	if wind and ramp > 0.0:
		var t := wind.get_time()
		wind.fallback_wind = Vector2(lerpf(12.0, target_speed, clampf(t / ramp, 0.0, 1.0)), 0.0)

# Script stubs held by the contracts are dropped before quitting (a script instance freed after the engine's
# script languages aborts the exit).
func _release_stubs() -> void:
	var wc = Engine.get_singleton("WorldContracts")
	wc.set_snow(null)
	wc.set_weather(null)
	wc.set_ground(null)
	wc.set_fire(null)

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
	if winter:
		g.season = preload("res://main.gd").weather_season(20)
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
		x += float(args["spacing"]) if args.has("spacing") else t.height * 1.3 + t.crown
	target_speed = float(args.get("speed", "30"))
	wind.target = wind.get_path_to(self)
	wind.rescan()
	return trees

func run(args: Dictionary) -> void:
	if args.has("glaze-test"):
		await _glaze_test()
		_release_stubs()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("snow-test"):
		await _snow_test()
		_release_stubs()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("avalanche-test"):
		await _avalanche_test()
		_release_stubs()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("test"):
		await _test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	_world(args.has("pt"), int(args.get("spp", "16")))
	var snow: WorldSnow = null
	if args.has("snow"):
		winter = true
		var ss: WorldSnow = load("res://snow_load_stub.gd").new()
		ss.load_kg_m2 = float(args.snow)
		Engine.get_singleton("WorldContracts").set_snow(ss)
	if args.has("avalanche"):
		snow = load("res://avalanche_stub.gd").new()
		Engine.get_singleton("WorldContracts").set_snow(snow)
		Engine.get_singleton("WorldContracts").set_ground(load("res://slope_stub.gd").new())
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
		if snow and t >= 1.0:
			snow.pressure = float(args.avalanche)
		if f % 15 == 0:
			print("t=%.1f wall %d ms parts %d falls %d snow caps %d clumps %d dropped %d" % [t, Time.get_ticks_msec(), _parts(), wind.get_storm_fall_count(), wind.get_snow_caps_drawn(), wind.get_snow_clumps_falling(), wind.get_snow_clumps_dropped()])
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
				# The path tracer's surfaces built first (skinned trees take dozens of frames).
				for k in 200:
					RenderingServer.force_draw(false)
					if get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) == 0:
						break
			for k in (int(args.get("spp", "16")) / 4 + 2 if args.has("pt") else 2):
				RenderingServer.force_draw(false)
			get_viewport().get_texture().get_image().save_png(dir.path_join("storm_%04d.png" % f))
			print("frame %d t=%.2f parts %d" % [f, t, _parts()])
	for st in wind.get_children(true):
		if String(st.name).begins_with("TreeStump"):
			for m in st.get_children():
				if m is MeshInstance3D:
					print("stump mesh %s scale %.2f aabb %s" % [m.mesh.get_class(), m.scale.x, m.mesh.get_aabb()])
	print("storm: %d breaks, %d thrown (%d snapped at the foot), %d Blast falls, %d snags, %d stand-replacing patches (stand-age map kind %d at the first tree)" % [wind.get_storm_break_count(), wind.get_storm_throw_count(), wind.get_storm_snap_count(), wind.get_storm_fall_count(), wind.get_snag_count(), wind.get_windthrow_patch_count(), WorldContracts.get_stand().disturbance_at(Vector3.ZERO)])
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
	# Stand-replacing windthrow reaches the stand-age map (WorldStand, trees.md 5aq): a 30 m cell with 60% of its
	# stems thrown (at least three) is reported once as a wind disturbance. These trees stand a tree height or more
	# apart, one to a cell, so single throws are gaps and none is reported.
	var patches: int = wind.get_windthrow_patch_count()
	_check(patches == 0, "windthrow to the stand-age map: %d thrown of %d trees, one to a cell: %d stand-replacing patches" % [wind.get_storm_throw_count(), trees.size(), patches])
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

# Avalanche impact (TreeWind3D, trees.md 5ak): a calm day, so the wind breaks nothing; a WorldSnow stub
# sets the peak pressure on every tree and a WorldGround stub tilts the ground toward +Z.
# Mean drop of the side branches' tips (m) and their mean bow (degrees: drop over length), posed against rest, for
# the tree at index k (the end segments of laterals: not a stem, level within 45 degrees, over 0.5 m).
func _lateral_bow(k: int) -> Vector2:
	var mmis := []
	for c in get_children():
		if c is MultiMeshInstance3D:
			mmis.append(c)
	var mm: MultiMesh = mmis[k].multimesh
	var rig: PackedFloat32Array = mm.mesh.get_meta("tree_rig")
	var st: int = TreeGenerator.RIG_STRIDE
	var nb := rig.size() / st
	var buf := RenderingServer.multimesh_get_buffer(mm.get_rid())
	var drop := 0.0
	var bow := 0.0
	var n := 0
	var parent_of := {}
	for b in range(1, nb):
		parent_of[int(rig[b * st + TreeGenerator.RIG_PARENT])] = true
	for b in range(1, nb):
		if parent_of.has(b):
			continue # The branch ends only: where the droop shows.
		var o := b * st
		var dir := Vector3(rig[o + TreeGenerator.RIG_DIR_X], rig[o + TreeGenerator.RIG_DIR_Y], rig[o + TreeGenerator.RIG_DIR_Z])
		var length := rig[o + TreeGenerator.RIG_LENGTH]
		if int(rig[o + TreeGenerator.RIG_PARENT]) == 0 or absf(dir.normalized().y) > 0.7 or length < 0.5:
			continue
		var tip := Vector3(rig[o + TreeGenerator.RIG_PIVOT_X], rig[o + TreeGenerator.RIG_PIVOT_Y], rig[o + TreeGenerator.RIG_PIVOT_Z]) + dir.normalized() * length
		var f := b * 20
		var xf := Transform3D(Basis(Vector3(buf[f], buf[f + 4], buf[f + 8]), Vector3(buf[f + 1], buf[f + 5], buf[f + 9]), Vector3(buf[f + 2], buf[f + 6], buf[f + 10])), Vector3(buf[f + 3], buf[f + 7], buf[f + 11]))
		var posed := xf * tip
		var rest: Vector3 = mm.get_instance_transform(0).origin + tip
		drop += rest.y - posed.y
		bow += rad_to_deg(asin(clampf((rest.y - posed.y) / length, -1.0, 1.0)))
		n += 1
	return Vector2(drop / maxf(n, 1), bow / maxf(n, 1))

# Ice storms (trees.md 5be; Hauer, Dawson and Werner 2006, Trees and Ice Storms; Jones 1998): glaze from freezing rain
# weighs on every twig. 6-12 mm breaks small branches and weak limbs, 12-25 mm and more larger ones; birch, beech and
# other spreading broadleaves fail first, conical conifers shed it best.
func _glaze_test() -> void:
	# The weather's freezing rain (WeatherMap3D warm_nose_temperature): Jones' (1998) rate, accretion under -3 C air
	# below a +4 C warm layer, and the thaw.
	var jr: float = WeatherMap3D.glaze_accretion_rate(2.5, 5.0)
	_check(absf(jr - 1.28) < 0.02, "Jones 1998: 2.5 mm/h of freezing rain in 5 m/s wind adds %.2f mm/h of radial ice (1.28)" % jr)
	var gwm := WeatherMap3D.new()
	add_child(gwm)
	gwm.set_temperature(-3.0)
	gwm.set_rain_rate(2.5)
	gwm.advance(3600.0)
	_check(gwm.get_glaze_at(Vector3()) == 0.0, "no warm layer aloft: no glaze (%.2f mm)" % gwm.get_glaze_at(Vector3()))
	gwm.warm_nose_temperature = 4.0
	for h in 4:
		gwm.advance(3600.0)
	var g4: float = gwm.get_glaze_at(Vector3())
	_check(g4 > 3.0 and g4 < 12.0 and gwm.get_rain_share_at(Vector3()) > 0.99, "a +4 C warm layer over -3 C air: %.1f mm of glaze in 4 h, all of it falling liquid" % g4)
	gwm.set_rain_rate(0.0)
	gwm.set_temperature(3.0)
	for h in 4:
		gwm.advance(3600.0)
	_check(gwm.get_glaze_at(Vector3()) < g4 - 2.0, "4 h at +3 C melt it to %.1f mm" % gwm.get_glaze_at(Vector3()))
	gwm.queue_free()
	# The ice as geometry (TreeGenerator glaze_mm): an "ice" surface round the wood, skinned on a rigged tree.
	var ig := TreeGenerator.new()
	ig.preset = 2
	ig.rigged = true
	ig.glaze_mm = 7.0
	var im: ArrayMesh = ig.generate()
	var ice := -1
	for si in im.get_surface_count():
		if im.surface_get_name(si) == "ice":
			ice = si
	var iv := 0
	var skinned := false
	if ice >= 0:
		var ia := im.surface_get_arrays(ice)
		iv = (ia[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
		skinned = ia[Mesh.ARRAY_BONES] != null and (ia[Mesh.ARRAY_BONES] as PackedInt32Array).size() == iv * 4
	_check(ice >= 0 and iv > 1000 and skinned and im.get_meta("tree_glaze_mm", 0.0) == 7.0, "glaze_mm 7: a skinned ice surface round the birch's wood (%d vertices)" % iv)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)
	ramp = 0.0
	winter = true
	var glaze: WorldWeather = load("res://glaze_stub.gd").new()
	Engine.get_singleton("WorldContracts").set_weather(glaze)
	# Birch, beech, Norway spruce, oak, all bare but the spruce.
	var trees := _setup({"speed": "0.3", "turbulence": "0.05", "presets": "2,1,4,0"})
	wind.snow_creep_hours = 0.0
	for f in 45:
		await get_tree().physics_frame
	var mm_nodes := []
	for c in get_children():
		if c is MultiMeshInstance3D:
			mm_nodes.append(c)
	var base := []
	for k in trees.size():
		base.append(_lateral_bow(k))
	var kg := func() -> Array:
		var out := []
		for m in mm_nodes:
			out.append(snappedf(wind.get_instance_glaze_kg(m, 0), 1.0))
		return out
	_check(kg.call().max() == 0.0, "no glaze: no ice on the crowns %s" % str(kg.call()))
	glaze.glaze_mm = 6.0
	for f in 60:
		await get_tree().physics_frame
	var kg6: Array = kg.call()
	var bows := []
	for k in trees.size():
		bows.append(snappedf((_lateral_bow(k) - base[k]).y, 0.1))
	print("6 mm: ice kg %s, laterals bow %s deg, %d breaks" % [str(kg6), str(bows), wind.get_glaze_break_count()])
	# A tree's twigs run to kilometres: ice on them is tonnes on a broad crown, capped by the rain its plan catches (glaze
	# multiplies a branch's weight 10-100 times; Hauer et al. 2008; Jones 1998).
	_check(kg6.min() > 30.0 and kg6.max() < 15000.0, "6 mm: tens of kilograms on a young birch, tonnes on a broad crown (no more than the rain it catches) %s" % str(kg6))
	_check(bows[0] > 1.0 and bows[1] > 1.0, "6 mm: the broadleaves' laterals bow under it (%s deg)" % str(bows))
	var b6: int = wind.get_glaze_break_count()
	# A thaw after a light glaze: the ice goes and the laterals stand up again.
	glaze.glaze_mm = 0.0
	for f in 45:
		await get_tree().physics_frame
	var back := []
	for k in trees.size():
		back.append(snappedf((_lateral_bow(k) - base[k]).y, 0.1))
	_check(kg.call().max() == 0.0 and absf(back[2]) < 2.0, "the melt: the ice goes and the spruce's laterals stand up again (%s deg)" % str(back))
	glaze.glaze_mm = 25.0
	for f in 150:
		await get_tree().physics_frame
	var kg25: Array = kg.call()
	print("25 mm: ice kg %s, %d glaze breaks" % [str(kg25), wind.get_glaze_break_count()])
	_check(wind.get_glaze_break_count() > b6, "25 mm: limbs and tops break under the ice (%d breaks, %d at 6 mm)" % [wind.get_glaze_break_count(), b6])
	var per := [0, 0, 0, 0]
	for e in events:
		var best := 0
		for k in trees.size():
			if absf(e.pos.x - trees[k].x) < absf(e.pos.x - trees[best].x):
				best = k
		per[best] += 1
	_check(per[2] <= mini(per[0], per[1]) and per[2] <= per[3], "the spruce's cone sheds the ice best: breaks birch %d, beech %d, spruce %d, oak %d" % per)

func _snow_test() -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)
	ramp = 0.0
	winter = true
	var snow: WorldSnow = load("res://snow_load_stub.gd").new()
	Engine.get_singleton("WorldContracts").set_snow(snow)
	# Norway spruce, silver fir, Scots pine, beech (bare: snow on its limbs only).
	var trees := _setup({"speed": "0.3", "turbulence": "0.05", "presets": "4,14,3,1"})
	wind.snow_creep_hours = 0.0
	for f in 45:
		await get_tree().physics_frame
	var base := []
	for k in trees.size():
		base.append(_lateral_bow(k))
	var line := func(tag: String) -> String:
		var parts := []
		for k in trees.size():
			var b: Vector2 = _lateral_bow(k) - base[k]
			parts.append("%.0f m: %.1f deg (%.0f cm)" % [trees[k].height, b.y, b.x * 100.0])
		return tag + " " + ", ".join(parts)
	# A fresh fall the canopy holds (spruce capacity about 15-30 kg/m2 of ground).
	snow.load_kg_m2 = 4.0
	for f in 45:
		await get_tree().physics_frame
	print(line.call("4 kg/m2:"))
	var mm_nodes := []
	for c in get_children():
		if c is MultiMeshInstance3D:
			mm_nodes.append(c)
	var levels := func() -> Array:
		var out := []
		for m in mm_nodes:
			out.append(wind.get_instance_cap_level(m, 0))
		return out
	var lv_light: Array = levels.call()
	var msg := "4 kg/m2 (13 percent of capacity): conifers wear light caps, the bare beech none (levels %s, %d drawn)" % [str(lv_light), wind.get_snow_caps_drawn()]
	_check(lv_light[0] == 1 and lv_light[1] == 1 and lv_light[3] == 0 and wind.get_snow_caps_drawn() >= 3, msg)
	var light := []
	for k in trees.size():
		light.append((_lateral_bow(k) - base[k]).y)
	_check(light[0] > 1.0 and light[0] < 15.0 and wind.get_snow_break_count() == 0, "4 kg/m2: spruce laterals bow %.1f deg, nothing breaks (%d)" % [light[0], wind.get_snow_break_count()])
	snow.load_kg_m2 = 15.0
	for f in 45:
		await get_tree().physics_frame
	print(line.call("15 kg/m2:"))
	var lv_full: Array = levels.call()
	_check(lv_full[0] == 2 and lv_full[1] == 2, "15 kg/m2 (half the capacity): full caps (levels %s)" % str(lv_full))
	var full: float = (_lateral_bow(0) - base[0]).y
	# Scots pine is the snow-breakage species (damage from about 20 kg/m2 of crown, Peltola et al. 1999): its
	# thinnest limbs may go under a full canopy; no top or stem does.
	var thick := 0
	for e in events:
		if not e.uprooted and e.radius >= 0.05:
			thick += 1
	_check(full > light[0] * 1.5 and full >= 8.0 and full <= 35.0 and thick == 0 and wind.get_storm_throw_count() == 0, "15 kg/m2 (a full canopy): spruce laterals bow %.1f deg (10-30 measured); no top or stem breaks (%d thin limbs)" % [full, wind.get_snow_break_count()])
	# The wood creeps on under a held load (Ray and Bret-Harte 2019: softwoods about a third more over hours).
	wind.snow_creep_hours = 0.002
	for f in 300:
		await get_tree().physics_frame
	var crept: float = (_lateral_bow(0) - base[0]).y
	print(line.call("15 kg/m2 crept:"))
	_check(crept > full * 1.12 and crept < full * 1.6, "creep: the held load bows them on to %.1f deg (%.0f%% more)" % [crept, (crept / full - 1.0) * 100.0])
	# Heavy wet snow (Peltola et al. 1999; critical crown loads of some 20-50 kg/m2 for pines).
	snow.load_kg_m2 = 60.0
	for f in 120:
		await get_tree().physics_frame
	print(line.call("60 kg/m2:"))
	_check(wind.get_snow_break_count() >= 1, "60 kg/m2 of wet snow breaks tops and limbs (%d breaks, %d stems thrown)" % [wind.get_snow_break_count(), wind.get_storm_throw_count()])
	_check(snow.shakes >= 1 and snow.shaken_kg > 0.0 and wind.get_snow_shaken_kg() > 0.0, "the breaks shake the snow onto the pack (%d shakes, %.0f kg)" % [snow.shakes, snow.shaken_kg])
	var lv_heavy: Array = levels.call()
	_check(lv_heavy[0] == 3 or lv_heavy[1] == 3, "60 kg/m2: heavy caps (levels %s)" % str(lv_heavy))
	var dropped_before: int = wind.get_snow_clumps_dropped()
	print("before unload: kg %.0f, dropped %d, falling %d" % [wind.get_instance_snow_kg(mm_nodes[0], 0), dropped_before, wind.get_snow_clumps_falling()])
	snow.load_kg_m2 = 0.0
	wind.snow_creep_hours = 0.0
	for f in 75:
		await get_tree().physics_frame
	_check(wind.get_snow_clumps_dropped() > dropped_before and levels.call()[0] == 0, "the snow unloads: %d clumps fall from the crowns near the camera, the caps go" % (wind.get_snow_clumps_dropped() - dropped_before))
	print("after unload: kg %.0f, dropped %d, falling %d" % [wind.get_instance_snow_kg(mm_nodes[0], 0), wind.get_snow_clumps_dropped(), wind.get_snow_clumps_falling()])
	for f in 600:
		await get_tree().physics_frame
	_check(wind.get_snow_clumps_falling() == 0, "the clumps land within 10 s (%d still falling)" % wind.get_snow_clumps_falling())
	var back: float = absf((_lateral_bow(3) - base[3]).y)
	_check(back < 1.0, "the snow gone, the beech's limbs spring back (%.2f deg left)" % back)

func _avalanche_test() -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)
	ramp = 0.0
	var snow: WorldSnow = load("res://avalanche_stub.gd").new()
	Engine.get_singleton("WorldContracts").set_snow(snow)
	Engine.get_singleton("WorldContracts").set_ground(load("res://slope_stub.gd").new())
	# A closed canopy over the row (LAI 4, closure 0.86), as VegetationField3D paints it.
	var wm := WeatherMap3D.new()
	add_child(wm)
	var img := Image.create_empty(160, 40, false, Image.FORMAT_RGF)
	img.fill(Color(20.0, 1.0, 0.0))
	wm.canopy_map_rect = Rect2(-20, -20, 160, 40)
	wm.canopy_map = ImageTexture.create_from_image(img)
	var trees := _setup({"speed": "5", "presets": "4,1,2"})
	wind.weather_map = wind.get_path_to(wm)
	# The trees as a VegetationField3D's: it keeps the felled crowns (add_canopy_loss) for its repaints.
	var vf := VegetationField3D.new()
	add_child(vf)
	for c in get_children():
		if c is MultiMeshInstance3D:
			c.reparent(vf)
	wind.target = wind.get_path_to(vf)
	wind.rescan()
	var sizes := []
	for t in trees:
		sizes.append("%.0f m" % t.height)
	snow.pressure = 500.0
	for f in 90:
		await get_tree().physics_frame
	_check(wind.get_avalanche_break_count() + wind.get_avalanche_throw_count() == 0 and wind.get_storm_break_count() == 0, "0.5 kPa: no tree breaks (%d crowns, %d stems)" % [wind.get_avalanche_break_count(), wind.get_avalanche_throw_count()])
	snow.pressure = 4500.0
	for f in 90:
		await get_tree().physics_frame
	_check(wind.get_avalanche_break_count() >= 2 and wind.get_avalanche_throw_count() == 0, "4.5 kPa (powder cloud): crowns snap, stems hold (%d crowns of trees %s, %d stems)" % [wind.get_avalanche_break_count(), ", ".join(sizes), wind.get_avalanche_throw_count()])
	snow.pressure = 20000.0
	for f in 60:
		await get_tree().physics_frame
	var mid: int = wind.get_avalanche_throw_count()
	_check(mid >= 1 and mid < trees.size(), "20 kPa: the thinner stems go, the old spruce stands (%d of %d)" % [mid, trees.size()])
	snow.pressure = 100000.0
	for f in 300:
		await get_tree().physics_frame
	_check(wind.get_avalanche_throw_count() == trees.size(), "100 kPa (a large dense flow): every stem goes (%d of %d)" % [wind.get_avalanche_throw_count(), trees.size()])
	# What fell lies downslope (+Z) of where it stood.
	var zs := 0.0
	var n := 0
	for c in wind.get_children(true):
		if c is RigidBody3D:
			zs += (c.global_transform * c.center_of_mass).z
			n += 1
		elif c.has_method("get_piece_count"):
			for i in c.get_piece_count():
				zs += c.get_piece_center(i).z
				n += 1
	_check(n > 0 and zs / n > 1.0, "the trees fall downslope (mean part z %.1f m over %d parts)" % [zs / maxf(n, 1), n])
	var canopy: Image = wm.canopy_map.get_image()
	var worst := 0.0
	for t in trees:
		var px := int(t.x + 20.0)
		worst = maxf(worst, canopy.get_pixel(px, 20).g)
	var kept := canopy.get_pixel(int(trees[0].x + 20.0), 2).g # 18 m upslope of the first stem.
	_check(worst < 0.05 and kept > 0.95, "felled crowns leave the canopy map (leaf area left %.2f at the stems, %.2f away)" % [worst * 4.0, kept * 4.0])
	_check(vf.get_canopy_losses().size() == trees.size() * 2, "the field keeps every felled crown and snapped top (%d losses)" % vf.get_canopy_losses().size())
	_check(wm.get_canopy_closure_at(Vector3(trees[0].x, 0, 0)) < 0.1, "the weather map sees the gap (closure %.2f at the first stem)" % wm.get_canopy_closure_at(Vector3(trees[0].x, 0, 0)))

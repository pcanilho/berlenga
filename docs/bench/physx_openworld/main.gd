extends Node3D
# Open-world physics bench for the PhysX server: streaming regions of static bodies in and out, scene query
# cost, debris settling, and resting jitter far from the origin. Headless is fine. Prints one line per check.
#
# godot --headless --fixed-fps 60 --path docs/bench/physx_openworld -- [options]
#   --regions=N       regions streamed in one after another (default 4)
#   --per-region=N    static bodies per region (default 3000): boxes, convexes and trimeshes
#   --region-size=M   region side in meters (default 500)
#   --rays=N          raycasts per query check (default 20000)
#   --debris=N        dynamic boxes dropped for the debris check (default 2000)
#   --far=M,M,...     offsets for the precision check (default 0,1000,10000,50000)
#   --only=a,b        run only these checks: stream, debris, precision
var regions := 4
var per_region := 3000
var region_size := 500.0
var rays := 20000
var debris := 2000
var far := [0.0, 1000.0, 10000.0, 50000.0]
var only := []
var rng := RandomNumberGenerator.new()
var shapes := []

func _arg(a: String, k: String) -> String:
	return a.substr(k.length()) if a.begins_with(k) else ""

func _ready():
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--regions="): regions = int(_arg(a, "--regions="))
		if a.begins_with("--per-region="): per_region = int(_arg(a, "--per-region="))
		if a.begins_with("--region-size="): region_size = float(_arg(a, "--region-size="))
		if a.begins_with("--rays="): rays = int(_arg(a, "--rays="))
		if a.begins_with("--debris="): debris = int(_arg(a, "--debris="))
		if a.begins_with("--far="): far = Array(_arg(a, "--far=").split_floats(","))
		if a.begins_with("--only="): only = Array(_arg(a, "--only=").split(","))
	rng.seed = 7
	var box := BoxShape3D.new(); box.size = Vector3(2, 3, 2)
	var conv := ConvexPolygonShape3D.new()
	var pts := PackedVector3Array()
	for i in 24:
		pts.append(Vector3(rng.randf_range(-1.5, 1.5), rng.randf_range(0, 2), rng.randf_range(-1.5, 1.5)))
	conv.points = pts
	var tri := ConcavePolygonShape3D.new()
	tri.set_faces(SphereMesh.new().get_faces())
	shapes = [box, conv, tri]
	print("PhysX open-world bench: engine=", ProjectSettings.get_setting("physics/3d/physics_engine"), " regions=", regions, " per_region=", per_region)
	if only.is_empty() or "stream" in only:
		await _stream()
	if only.is_empty() or "debris" in only:
		await _debris()
	if only.is_empty() or "precision" in only:
		await _precision()
	get_tree().quit()

func _physics_ms(frames: int) -> Array:
	# Mean and max wall time of the next frames. Run with --fixed-fps 60: one physics step a frame and
	# nothing else drawn, so a frame's time is the step plus a little overhead.
	var total := 0.0
	var worst := 0.0
	var last := Time.get_ticks_usec()
	for i in frames:
		await get_tree().process_frame
		var now := Time.get_ticks_usec()
		var ms := (now - last) / 1000.0
		last = now
		total += ms
		worst = max(worst, ms)
	return [total / frames, worst]

func _ray_ms(center: Vector3) -> float:
	var space := get_world_3d().direct_space_state
	var q := PhysicsRayQueryParameters3D.new()
	var t0 := Time.get_ticks_usec()
	var hits := 0
	for i in rays:
		var p := center + Vector3(rng.randf_range(-0.5, 0.5) * region_size, 50.0, rng.randf_range(-0.5, 0.5) * region_size)
		q.from = p
		q.to = p - Vector3(0, 100, 0)
		if not space.intersect_ray(q).is_empty():
			hits += 1
	return (Time.get_ticks_usec() - t0) / 1000.0

func _region(center: Vector3) -> Node3D:
	var root := Node3D.new()
	for i in per_region:
		var b := StaticBody3D.new()
		var cs := CollisionShape3D.new()
		cs.shape = shapes[i % 3]
		b.add_child(cs)
		b.position = center + Vector3(rng.randf_range(-0.5, 0.5) * region_size, 0, rng.randf_range(-0.5, 0.5) * region_size)
		b.rotation.y = rng.randf() * TAU
		root.add_child(b)
	return root

func _stream():
	var loaded := []
	for r in regions:
		var center := Vector3((r % 4) * region_size, 0, (r / 4) * region_size)
		var root := _region(center)
		await get_tree().process_frame
		var t0 := Time.get_ticks_usec()
		add_child(root)
		var add_ms := (Time.get_ticks_usec() - t0) / 1000.0
		# The hitch: the worst of the first frames (a region's statics may enter over a few steps).
		var first := await _physics_ms(10)
		var rays_now := _ray_ms(center)
		await get_tree().process_frame
		var steady := await _physics_ms(120)
		var rays_later := _ray_ms(center)
		await _physics_ms(180)
		var settled := await _physics_ms(60)
		loaded.append(root)
		print("stream region=%d statics=%d add_ms=%.1f first_frames_max_ms=%.2f first_frames_sum_ms=%.2f frames_mean_ms=%.3f frames_max_ms=%.2f settled_mean_ms=%.3f rays_first_ms=%.1f rays_settled_ms=%.1f" % [r, (r + 1) * per_region, add_ms, first[1], first[0] * 10.0, steady[0], steady[1], settled[0], rays_now, rays_later])
	for root in loaded:
		var t0 := Time.get_ticks_usec()
		root.queue_free()
		await get_tree().process_frame
		var free_ms := (Time.get_ticks_usec() - t0) / 1000.0
		var after := await _physics_ms(10)
		print("unload free_ms=%.1f frames_mean_ms=%.3f" % [free_ms, after[0]])

func _debris():
	var ground := StaticBody3D.new()
	var gs := CollisionShape3D.new(); var gb := BoxShape3D.new(); gb.size = Vector3(200, 1, 200); gs.shape = gb; gs.position.y = -0.5
	ground.add_child(gs); add_child(ground)
	var bodies := []
	var bs := BoxShape3D.new(); bs.size = Vector3(0.4, 0.3, 0.5)
	var side := int(ceil(sqrt(debris)))
	for i in debris:
		var rb := RigidBody3D.new()
		var cs := CollisionShape3D.new(); cs.shape = bs; rb.add_child(cs)
		rb.position = Vector3((i % side) * 0.9 - side * 0.45, 2.0 + rng.randf() * 4.0, (i / side) * 0.9 - side * 0.45)
		rb.rotation = Vector3(rng.randf(), rng.randf(), rng.randf()) * TAU
		add_child(rb)
		bodies.append(rb)
	for sec in 8:
		var s := await _physics_ms(60)
		var asleep := 0
		for rb in bodies:
			if rb.sleeping: asleep += 1
		print("debris t=%ds bodies=%d asleep=%d frames_mean_ms=%.3f frames_max_ms=%.2f" % [sec + 1, debris, asleep, s[0], s[1]])
	for rb in bodies: rb.queue_free()
	ground.queue_free()
	await get_tree().process_frame

func _precision():
	for off in far:
		var o := Vector3(off, 0, off)
		var ground := StaticBody3D.new()
		var gs := CollisionShape3D.new(); var gb := BoxShape3D.new(); gb.size = Vector3(20, 1, 20); gs.shape = gb; gs.position.y = -0.5
		ground.add_child(gs); ground.position = o; add_child(ground)
		var rb := RigidBody3D.new()
		rb.can_sleep = false
		var cs := CollisionShape3D.new(); var bs := BoxShape3D.new(); bs.size = Vector3(0.5, 0.5, 0.5); cs.shape = bs; rb.add_child(cs)
		rb.position = o + Vector3(0, 0.3, 0)
		add_child(rb)
		await _physics_ms(120)
		var p0: Vector3 = rb.global_position
		var drift := 0.0
		var jitter := 0.0
		var last := p0
		for i in 300:
			await get_tree().physics_frame
			var p: Vector3 = rb.global_position
			drift = max(drift, (p - p0).length())
			jitter = max(jitter, (p - last).length())
			last = p
		print("precision offset_m=%.0f rest_y=%.4f drift_mm=%.3f step_jitter_mm=%.3f" % [off, p0.y - o.y, drift * 1000.0, jitter * 1000.0])
		rb.queue_free(); ground.queue_free()
		await get_tree().process_frame

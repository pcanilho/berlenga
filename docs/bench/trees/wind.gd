extends Node3D
# Wind bench for rigged trees (TreeWind3D, docs/ideas/trees.md section 3a).
#
# godot --path docs/bench/trees --rendering-driver vulkan -- --wind [options]
#   --test             rig and solver checks per species, PASS/FAIL, quit
#   --out=dir          render --frames frames into dir (frame_0000.png, ...), then quit
#   --frames=N         (default 90)
#   --fps=F            frames per second of the clip (default 30)
#   --speed=U          mean wind at 10 m, m/s (default 8)
#   --turbulence=I     turbulence intensity of the fallback wind (default 0.2)
#   --raster           raster only (default); --pt path traces with --spp samples
#   --presets=a,b,c    species (indices as in main.gd; default oak, birch, spruce)
#   --still=T          one frame at time T seconds instead of a clip

const S := 18
const NAMES := ["oak", "beech", "birch", "Scots pine", "Norway spruce", "hazel", "hawthorn", "lime", "sycamore maple",
		"ash", "horse chestnut", "black alder", "white willow", "European larch", "silver fir", "rowan", "elder", "holly", "yew", "box",
		"sugar maple", "red maple", "red oak", "white oak", "white pine", "hemlock", "American beech", "paper birch",
		"yellow birch", "witch-hazel", "mountain laurel"]

var fails := 0
var fps := 30.0
var wind: TreeWind3D

func _check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		fails += 1

func _rig_tree(p: int, seed: int) -> ArrayMesh:
	var g := TreeGenerator.new()
	g.preset = p
	g.seed = seed
	g.rigged = true
	g.needle_sprays = true
	return g.generate()

func run(args: Dictionary) -> void:
	fps = float(args.get("fps", "30"))
	wind = TreeWind3D.new()
	wind.active = false
	wind.fallback_wind = Vector2(float(args.get("speed", "8")), 0.0)
	wind.fallback_turbulence = float(args.get("turbulence", "0.2"))
	add_child(wind)
	if args.has("test"):
		_test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	var picks: Array = [0, 2, 4]
	if args.has("presets"):
		picks = []
		for v in String(args.presets).split(","):
			picks.append(int(v))
	_scene(picks, args.has("pt"), int(args.get("spp", "8")))
	await _render(String(args.get("out", "/tmp/wind")), int(args.get("frames", "90")), float(args.get("still", "-1")), args.has("pt"))
	get_tree().quit(1 if fails > 0 else 0)

# Pose of every bone (tree space) from the solver's relative bends.
func _pose(rig: PackedFloat32Array, bends: PackedVector3Array) -> Array:
	var n := rig.size() / S
	var pose := []
	pose.resize(n)
	pose[0] = Transform3D()
	for b in range(1, n):
		var piv := Vector3(rig[b * S + 1], rig[b * S + 2], rig[b * S + 3])
		var a: Vector3 = bends[b]
		var rot := Basis() if a.length() < 1e-7 else Basis(a.normalized(), a.length())
		pose[b] = pose[int(rig[b * S])] * Transform3D(Basis(), piv) * Transform3D(rot, Vector3()) * Transform3D(Basis(), -piv)
	return pose

func _top_move(rig: PackedFloat32Array, bends: PackedVector3Array) -> Vector3:
	var n := rig.size() / S
	var pose := _pose(rig, bends)
	var top := 1
	var top_y := -1.0
	for b in range(1, n):
		var tip := Vector3(rig[b * S + 1], rig[b * S + 2], rig[b * S + 3]) + Vector3(rig[b * S + 13], rig[b * S + 14], rig[b * S + 15]) * rig[b * S + 12]
		if tip.y > top_y:
			top_y = tip.y
			top = b
	var t := Vector3(rig[top * S + 1], rig[top * S + 2], rig[top * S + 3]) + Vector3(rig[top * S + 13], rig[top * S + 14], rig[top * S + 15]) * rig[top * S + 12]
	return (pose[top] as Transform3D) * t - t

func _test() -> void:
	var t0 := Time.get_ticks_msec()
	for p in [0, 1, 2, 3, 4, 5, 13, 22, 24]:
		var m := _rig_tree(p, 1)
		var rig: PackedFloat32Array = m.get_meta("tree_rig")
		var n := rig.size() / S
		var f := TreeWind3D.get_rig_frequencies(m)
		var name: String = NAMES[p]
		_check(n >= 2 and n <= TreeGenerator.MAX_BONES, "%s: %d bones" % [name, n])
		# Every vertex skinned with weights summing to 1, to bones that exist.
		var ok := true
		for s in m.get_surface_count():
			var arr := m.surface_get_arrays(s)
			var bones: PackedInt32Array = arr[Mesh.ARRAY_BONES]
			var weights: PackedFloat32Array = arr[Mesh.ARRAY_WEIGHTS]
			var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
			if bones.size() != verts.size() * 4 or weights.size() != verts.size() * 4:
				ok = false
				break
			for v in range(0, verts.size(), 97):
				var sum := weights[v * 4] + weights[v * 4 + 1] + weights[v * 4 + 2] + weights[v * 4 + 3]
				if absf(sum - 1.0) > 1e-3 or bones[v * 4] >= n or bones[v * 4 + 1] >= n:
					ok = false
		_check(ok, "%s: skin weights" % name)
		# Sway frequencies (Moore and Maguire 2004: 0.15-0.8 Hz for mature trunks; shrubs higher).
		var shrub: bool = p == 5
		_check(f[1] >= (0.15 if not shrub else 0.3) and f[1] <= (0.9 if not shrub else 6.0), "%s: stem sways at %.2f Hz" % [name, f[1]])
		var fmax := 0.0
		for b in range(1, n):
			fmax = maxf(fmax, f[b])
		_check(fmax <= 8.01, "%s: fastest bone %.2f Hz" % [name, fmax])
		# Steady wind, no turbulence: the top moves downwind, more with more wind, and stays small
		# in a breeze (under 3% of the height at 5 m/s).
		wind.fallback_turbulence = 0.0
		var h := 0.0
		for b in range(1, n):
			h = maxf(h, rig[b * S + 2] + rig[b * S + 14] * rig[b * S + 12])
		var moves := []
		for u in [5.0, 10.0, 20.0]:
			wind.fallback_wind = Vector2(u, 0)
			moves.append(_top_move(rig, wind.get_bone_bends(m, Transform3D(), 1)))
		_check(moves[0].x > 0.0 and moves[1].x > moves[0].x and moves[2].x > moves[1].x, "%s: top moves downwind %.2f / %.2f / %.2f m at 5 / 10 / 20 m/s" % [name, moves[0].x, moves[1].x, moves[2].x])
		_check(moves[0].length() < 0.03 * h, "%s: breeze bend %.1f%% of %.1f m" % [name, 100.0 * moves[0].length() / h, h])
		# Turbulent wind: poses change smoothly (no step over 0.03 rad between 60 Hz frames),
		# the same time gives the same pose, and the tree really moves.
		wind.fallback_wind = Vector2(15, 0)
		wind.fallback_turbulence = 0.25
		var worst := 0.0
		var span := 0.0
		wind.set_time(0.0)
		var prev := wind.get_bone_bends(m, Transform3D(), 3)
		var first := prev
		for k in 240:
			wind.set_time((k + 1) / 60.0)
			var cur := wind.get_bone_bends(m, Transform3D(), 3)
			for b in range(1, n):
				worst = maxf(worst, (cur[b] - prev[b]).length())
				span = maxf(span, (cur[b] - first[b]).length())
			prev = cur
		wind.set_time(2.0)
		var again := wind.get_bone_bends(m, Transform3D(), 3)
		wind.set_time(4.0 - 2.0)
		var same := true
		var again2 := wind.get_bone_bends(m, Transform3D(), 3)
		for b in range(n):
			same = same and again[b].is_equal_approx(again2[b])
		_check(worst < 0.03, "%s: largest bone step per frame %.4f rad" % [name, worst])
		_check(span > 0.005, "%s: sway over 4 s up to %.3f rad" % [name, span])
		_check(same, "%s: deterministic in time" % name)
	_test_touch()
	_test_deadwood()
	print("wind test: %.1f s" % ((Time.get_ticks_msec() - t0) / 1000.0))

# Colliders, touch bending, spring back and snapping, on a hazel and an oak.
func _test_touch() -> void:
	var holder := Node3D.new()
	add_child(holder)
	var hazel := _rig_tree(5, 1)
	var oak := _rig_tree(0, 1)
	var caps: PackedFloat32Array = oak.get_meta("tree_colliders")
	_check(caps.size() >= 8, "oak: %d collision capsules" % (caps.size() / 8))
	var mm_h := MultiMeshInstance3D.new()
	mm_h.multimesh = TreeWind3D.make_multimesh(hazel, [Transform3D()])
	holder.add_child(mm_h)
	var mm_o := MultiMeshInstance3D.new()
	mm_o.multimesh = TreeWind3D.make_multimesh(oak, [Transform3D(Basis(), Vector3(40, 0, 0))])
	holder.add_child(mm_o)
	wind.target = wind.get_path_to(holder)
	wind.fallback_turbulence = 0.0
	wind.fallback_wind = Vector2()
	wind.collision_range = 100.0
	wind.rescan()
	wind.update(1.0 / 60.0)
	_check(wind.get_target_count() == 2, "touch scene: %d targets" % wind.get_target_count())
	_check(wind.get_collider_count() == 1 or wind.get_collider_count() == 2, "colliders near the camera: %d" % wind.get_collider_count())
	# A slow body walks into the hazel at chest height and stays: its branches bend aside.
	var body := Node3D.new()
	body.add_to_group("tree_touch")
	body.set_meta("touch_radius", 0.5)
	add_child(body)
	body.position = Vector3(-3, 1.4, 0)
	wind.update(1.0 / 60.0)
	for k in 150:
		body.position = Vector3(-3 + 3.0 * minf(k / 120.0, 1.0), 1.4, 0)
		wind.update(1.0 / 60.0)
	_check(wind.get_piece_count() == 0, "walking pace (1.5 m/s) snaps nothing")
	var bent := wind.get_touch_bends(0, 0)
	var most := 0.0
	for a in bent:
		most = maxf(most, (a as Vector3).length())
	_check(most > 0.05, "hazel bends aside around a body: %.2f rad" % most)
	# It walks away: the branches spring back.
	body.position = Vector3(-10, 1.4, 0)
	for k in 300:
		wind.update(1.0 / 60.0)
	var left := 0.0
	for a in wind.get_touch_bends(0, 0):
		left = maxf(left, (a as Vector3).length())
	_check(left < 0.01, "hazel springs back after the body leaves (%.4f rad left, %d pieces)" % [left, wind.get_piece_count()])
	# A fast body through the hazel snaps thin branches off as rigid bodies.
	wind.break_radius = 0.04
	for k in 40:
		body.position = Vector3(-3 + 0.15 * k, 1.6, 0.2)
		wind.update(1.0 / 60.0)
	_check(wind.get_piece_count() > 0, "a body at 9 m/s snaps %d branches" % wind.get_piece_count())
	# The oak's trunk does not yield to a body.
	body.position = Vector3(40, 1.0, 0)
	for k in 30:
		wind.update(1.0 / 60.0)
	var trunk := wind.get_touch_bends(1, 0)
	_check(trunk.is_empty() or (trunk[1] as Vector3).length() < 0.002, "oak trunk does not yield to a body")
	body.queue_free()
	holder.queue_free()

func _bounds(m: Mesh) -> AABB:
	return m.get_aabb()

func _test_deadwood() -> void:
	for p in [0, 4]:
		var g := TreeGenerator.new()
		g.preset = p
		g.seed = 2
		var alive := g.generate()
		var h0 := g.get_height()
		var bark0: Color = g.bark_color
		var tris := []
		var heights := []
		var leaves := []
		for c in range(1, 6):
			g.decay_class = c
			var m := g.generate()
			var t := 0
			for s in m.get_surface_count():
				t += m.surface_get_array_index_len(s) / 3
			tris.append(t)
			heights.append(g.get_height())
			leaves.append(m.get_surface_count())
		var name: String = NAMES[p]
		if g.get_param(TreeGenerator.PARAM_NEEDLES) > 0.5:
			_check(leaves[0] > 1, "%s class 1: keeps its needles (red stage)" % name)
		else:
			_check(leaves[0] == 1, "%s class 1: bark only, no foliage" % name)
		_check(g.bark_color == bark0 and leaves[1] == 1, "%s class 2: needles gone, living colours restored after generate" % name)
		_check(tris[1] < tris[0], "%s class 2: twigs gone (%d of %d triangles)" % [name, tris[1], tris[0]])
		_check(heights[2] < h0 * 0.9 and heights[2] > h0 * 0.45, "%s class 3: top broken at %.1f of %.1f m" % [name, heights[2], h0])
		_check(heights[3] < heights[2] + 0.5 and tris[3] < tris[2], "%s class 4: stubs on a lower stem (%.1f m)" % [name, heights[3]])
		_check(heights[4] <= 1.6, "%s class 5: a stump of %.1f m" % [name, heights[4]])
		g.decay_class = 3
		g.fallen = true
		var log := g.generate()
		var b := log.get_aabb()
		_check(b.size.y < 4.0 and maxf(b.size.x, b.size.z) > h0 * 0.4, "%s log: lies %.1f m long, %.1f m high" % [name, maxf(b.size.x, b.size.z), b.size.y])
		g.fallen = false
		g.decay_class = 0
	# A clipped hawthorn stays inside its hedge envelope (leaves may poke out by a leaf).
	var hg := TreeGenerator.new()
	hg.preset = 6
	hg.envelope = AABB(Vector3(-0.7, -0.5, -1.3), Vector3(1.4, 2.3, 2.6))
	var hm := hg.generate()
	var hb := hm.get_aabb()
	_check(hb.size.x < 1.4 + 0.6 and hb.size.y < 1.8 + 0.6 and hb.size.z < 2.6 + 0.6, "clipped hawthorn %.1f x %.1f x %.1f m in a 1.4 x 1.8 x 2.6 m envelope" % [hb.size.x, hb.size.y, hb.size.z])
	_check(hg.get_leaf_count() > 500, "clipped hawthorn keeps %d leaves" % hg.get_leaf_count())

func _scene(picks: Array, pt: bool, spp: int) -> void:
	var x := 0.0
	var tallest := 0.0
	var gen := TreeGenerator.new()
	for p in picks:
		var g := TreeGenerator.new()
		g.preset = p
		g.seed = 1
		g.rigged = true
		g.needle_sprays = true
		var m := g.generate()
		x += g.get_crown_radius()
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = TreeWind3D.make_multimesh(m, [Transform3D(Basis(), Vector3(x, 0, 0))])
		add_child(mmi)
		x += g.get_crown_radius() + 2.0
		tallest = maxf(tallest, g.get_height())
		print("%s: %.1f m, %d bones, stem %.2f Hz" % [NAMES[p], g.get_height(), g.get_bone_count(), TreeWind3D.get_rig_frequencies(m)[1]])
	wind.target = wind.get_path_to(self)
	wind.rescan()
	print("wind targets: ", wind.get_target_count())
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
	env.pathtracing_enabled = pt
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
	sun.directional_shadow_max_distance = 300.0
	add_child(sun)
	var cam := Camera3D.new()
	cam.fov = 40.0
	add_child(cam)
	var span: float = max(x, tallest * 1.6)
	var dist := span * 0.5 / tan(deg_to_rad(cam.fov * 0.5)) * 0.62 + 6.0
	cam.position = Vector3(x * 0.5, tallest * 0.45, dist)
	cam.look_at(Vector3(x * 0.5, tallest * 0.42, -6))
	wind.position = cam.position

func _render(dir: String, frames: int, still: float, pt: bool) -> void:
	DirAccess.make_dir_recursive_absolute(dir)
	if still >= 0.0:
		wind.set_time(still)
		wind.update(0.0)
		for i in (12 if pt else 8):
			await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(dir.path_join("still.png"))
		print("saved still, solve %.2f ms" % wind.get_solve_msec())
		return
	var ms := 0.0
	for i in 6:
		await RenderingServer.frame_post_draw
	for f in frames:
		wind.update(1.0 / fps)
		ms += wind.get_solve_msec()
		await RenderingServer.frame_post_draw
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(dir.path_join("frame_%04d.png" % f))
	print("saved %d frames, solve %.3f ms per frame" % [frames, ms / frames])

extends Node3D
# Close-up bark bench (docs/ideas/ground-detail.md, "Close-up bark"): trees grown by TreeGenerator in a row, their
# trunks handed to a BarkDetail3D (set_trunks, the same dictionaries VegetationField3D.get_trunks_near returns).
#   --presets=a,b,c   TreeGenerator presets in a row, 4 m apart (default 3,27,41,39: Scots pine, paper birch,
#                     maritime pine, holm oak)
#   --look=i          the camera on the i-th tree's trunk (default 0); --dist=m from it (1.0), --at=m up it (1.3)
#   --no-bark         BarkDetail3D off (before frames)
#   --raster          raster (default path traced, --spp=N, 16)
#   --sun=EL,AZ       sun elevation and azimuth (35,35)
#   --out=path.png    render and quit
#   --readback        leave trees without a tree_bark_profile meta (BarkDetail3D reads the bark surface back itself)
#   --test            checks: pieces placed for each family, none for smooth bark, the update cost; quit

var args := {}
var bark: BarkDetail3D
var fails := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else ""
	var raster := args.has("raster") or args.has("test")
	var env := Environment.new()
	var sk := Sky.new()
	sk.sky_material = ProceduralSkyMaterial.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sk
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	var sv: PackedFloat64Array = [35.0, 35.0]
	if args.has("sun"):
		sv = args["sun"].split_floats(",")
	sun.rotation_degrees = Vector3(-sv[0], sv[1], 0)
	sun.light_energy = 1.6
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(60, 60)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.25, 0.22, 0.17)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)

	var presets: PackedInt64Array = [3, 27, 41, 39]
	if args.has("presets"):
		presets = PackedInt64Array(Array(args["presets"].split(",")).map(func(x): return int(x)))
	var trunks := []
	for i in presets.size():
		var g := TreeGenerator.new()
		g.preset = presets[i]
		g.seed = 1
		var mesh := g.generate()
		if not args.has("readback") and not mesh.has_meta("tree_bark_profile"):
			# What TreeGenerator stores when it builds the mesh (until it does, made here, outside the timed updates).
			var si := 0
			for s in mesh.get_surface_count():
				if mesh.surface_get_name(s) == "bark":
					si = s
			var arr := mesh.surface_get_arrays(si)
			mesh.set_meta("tree_bark_profile", BarkDetail3D.make_bark_profile(arr[Mesh.ARRAY_VERTEX], arr[Mesh.ARRAY_COLOR], mesh.get_meta("tree_colliders", PackedFloat32Array())))
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.position = Vector3(i * 4.0, 0, 0)
		add_child(mi)
		trunks.append(_trunk(mesh, mi.global_transform, presets[i], i + 1))
	bark = BarkDetail3D.new()
	bark.reference_height = 1080
	bark.enabled = not args.has("no-bark")
	add_child(bark)
	bark.set_trunks(trunks)

	var cam := Camera3D.new()
	add_child(cam)
	var look := int(args.get("look", "0"))
	var tr: Dictionary = trunks[min(look, trunks.size() - 1)]
	var base: Vector3 = tr["base"]
	var at := float(args.get("at", "1.3"))
	var dist := float(args.get("dist", "1.0")) + float(tr["radius_base"])
	cam.position = base + Vector3(0.35, at + 0.05, dist)
	cam.look_at(base + Vector3(0, at, 0))
	cam.fov = 40.0
	cam.current = true
	if args.has("test"):
		await _test(trunks, presets)
		get_tree().quit(1 if fails > 0 else 0)
		return
	_render_later(raster)

# The same dictionary VegetationField3D.get_trunks_near gives: the stem's capsule chain (tree_colliders meta) up to 4 m.
func _trunk(mesh: Mesh, xf: Transform3D, preset: int, seed: int) -> Dictionary:
	var col: PackedFloat32Array = mesh.get_meta("tree_colliders", PackedFloat32Array())
	var segs := []
	var prev := Vector3(col[0], col[1], col[2])
	var a0 := prev
	var r_top := col[6]
	for k in range(0, col.size() - 7, 8):
		var a := Vector3(col[k], col[k + 1], col[k + 2])
		var b := Vector3(col[k + 3], col[k + 4], col[k + 5])
		if k > 0 and a.distance_to(prev) > 1e-3:
			break
		segs.append([xf * a, xf * b, col[k + 6]])
		r_top = col[k + 6]
		prev = b
		if b.y - a0.y >= 4.0:
			break
	return {"base": xf * a0, "up": Vector3.UP, "height": 4.0, "radius_base": col[6], "radius_top": r_top,
		"segments": segs, "preset": preset, "species": -1, "variant": -1, "basis": xf.basis, "seed": seed, "mesh": mesh}

func check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		fails += 1

func _test(trunks: Array, presets: PackedInt64Array) -> void:
	get_viewport().disable_3d = true
	await get_tree().process_frame
	await _settle(bark)
	check(bark.get_trunk_count() == trunks.size(), "every trunk taken: %d of %d" % [bark.get_trunk_count(), trunks.size()])
	check(bark.get_piece_count() > 0, "pieces on the near trunks: %d" % bark.get_piece_count())
	for p in presets:
		print("  preset %d: reach %.1f m" % [p, bark.get_reach(BarkDetail3D.get_bark_kind(p))])
	var smooth := BarkDetail3D.new()
	smooth.reference_height = 1080
	add_child(smooth)
	var g2 := TreeGenerator.new()
	g2.preset = 1 # beech: smooth bark
	var m2 := g2.generate()
	smooth.set_trunks([_trunk(m2, Transform3D(), 1, 9)])
	await _settle(smooth)
	check(smooth.get_piece_count() == 0, "no pieces on smooth bark (beech): %d" % smooth.get_piece_count())
	for i in 20:
		bark.update_now()
	var cost := "update on the main thread: %.3f ms mean, %.3f ms worst" % [bark.get_update_msec_mean(), bark.get_update_msec_worst()]
	if args.has("readback"):
		print("      " + cost + " (bark read back from the GPU)")
	else:
		check(bark.get_update_msec_mean() < 2.0, cost)

# Updates until the worker has placed every trunk's pieces.
func _settle(b: BarkDetail3D) -> void:
	b.update_now()
	var n := 0
	while (b.is_placing() or n < 2) and n < 2000:
		await get_tree().process_frame
		b.update_now()
		n += 1

func _render_later(raster: bool) -> void:
	for i in 3:
		await get_tree().process_frame
	await _settle(bark)
	print("bark: %d trunks, %d pieces, update %.3f ms" % [bark.get_trunk_count(), bark.get_piece_count(), bark.get_update_msec_mean()])
	if not raster:
		var waited := 0
		while waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
			await RenderingServer.frame_post_draw
			waited += 1
	for i in (8 if raster else 12):
		await RenderingServer.frame_post_draw
	if args.has("out"):
		get_viewport().get_texture().get_image().save_png(args["out"])
		print("saved " + args["out"])
	get_tree().quit()

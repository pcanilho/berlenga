extends Node3D
# PlantGenerator: every preset at three seeds in a row, on a ground plane.
#
# godot --path docs/bench/plants -- [options]
#   --test           checks, print PASS/FAIL, quit
#   --raster         raster only (default path traced)
#   --spp=N          path tracing samples (default 16)
#   --close=P        close view of preset P (0..6)
#   --macro          with --close, one plant from a hand span away
#   --notex          without the generated leaf textures
#   --dump=dir       save the baked textures of the first seed as PNGs
#   --out=path.png   save the frame

const NAMES := ["dandelion", "plantain", "clover", "fern", "sedge", "daisy", "buttercup", "nettle", "thistle", "yarrow", "dock", "cocksfoot", "timothy", "dry_grass", "leaf_litter", "moss", "bilberry"]
const SEEDS := 3
const STEP := 0.55
var test := false
var raster := false
var spp := 16
var close := -1
var out := ""
var macro := false
var notex := false
var dump := ""
var n := 0
var fails := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--close="): close = int(a.substr(8))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--macro": macro = true
		elif a == "--notex": notex = true
		elif a.begins_with("--dump="): dump = a.substr(7)
	if test:
		_checks()
		get_tree().quit(1 if fails > 0 else 0)
		return
	var gen := PlantGenerator.new()
	gen.textures = not notex
	for p in NAMES.size():
		if close >= 0 and p != close:
			continue
		gen.preset = p
		gen.apply_preset_colors()
		for s in SEEDS:
			gen.seed = s
			var mi := MeshInstance3D.new()
			mi.mesh = gen.generate()
			if dump != "" and s == 0:
				for si in mi.mesh.get_surface_count():
					var mat := mi.mesh.surface_get_material(si) as BaseMaterial3D
					if mat and mat.albedo_texture:
						mat.albedo_texture.get_image().save_png("%s/%s_%d_albedo.png" % [dump, NAMES[p], si])
						mat.normal_texture.get_image().save_png("%s/%s_%d_normal.png" % [dump, NAMES[p], si])
						mat.backlight_texture.get_image().save_png("%s/%s_%d_back.png" % [dump, NAMES[p], si])
			if macro and s != 0:
				mi.free()
				continue
			mi.position = Vector3((p % 9) * STEP, 0, (s + (p / 9) * 3.4) * STEP) if close < 0 else Vector3((s - 1) * 0.36, 0, 0)
			add_child(mi)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(14, 12)
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.3, 0.25, 0.18)
	gm.roughness = 0.95
	pm.material = gm
	ground.mesh = pm
	ground.position = Vector3(2.2, 0, 1.9)
	add_child(ground)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	env.pathtracing_sky_shader_enabled = true
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 150, 0)
	sun.shadow_enabled = true
	sun.light_energy = 2.0
	add_child(sun)
	var cam := Camera3D.new()
	cam.fov = 40
	add_child(cam)
	if close >= 0 and macro:
		var bb: AABB = (get_child(0) as MeshInstance3D).get_aabb()
		var c: Vector3 = (get_child(0) as Node3D).position + bb.get_center()
		c.y = min(c.y, 0.06)
		cam.fov = 30
		cam.position = c + Vector3(0, max(0.12, bb.size.y * 0.5), max(0.22, max(bb.size.x, bb.size.z) * 0.9))
		cam.look_at(c)
	elif close >= 0:
		cam.position = Vector3(0, 0.42, 0.85)
		cam.look_at(Vector3(0, 0.1, 0))
	else:
		cam.position = Vector3(2.2, 2.3, 5.9)
		cam.look_at(Vector3(2.2, 0.1, 1.8))

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

func _tris(m: Mesh) -> int:
	var t := 0
	for s in m.get_surface_count():
		t += m.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	return t

func _checks() -> void:
	var gen := PlantGenerator.new()
	for p in NAMES.size():
		gen.preset = p
		gen.seed = 1
		var t0 := Time.get_ticks_usec()
		var m := gen.generate()
		var ms := (Time.get_ticks_usec() - t0) / 1000.0
		var aabb := m.get_aabb()
		var again := gen.generate()
		gen.seed = 2
		var other := gen.generate()
		print("PLANT %s tris=%d height=%.2f width=%.2f ms=%.2f surfaces=%d" % [NAMES[p], _tris(m), aabb.end.y, max(aabb.size.x, aabb.size.z), ms, m.get_surface_count()])
		_check(NAMES[p] + " has geometry", _tris(m) > 50, _tris(m))
		_check(NAMES[p] + " plausible size", aabb.end.y > 0.005 and aabb.end.y < 1.2 and aabb.size.x < 1.2, aabb)
		_check(NAMES[p] + " grows from the ground", aabb.position.y > -0.08, aabb.position.y)
		_check(NAMES[p] + " same seed, same plant", _tris(again) == _tris(m) and again.get_aabb() == aabb, _tris(again))
		_check(NAMES[p] + " other seed, other plant", other.get_aabb() != aabb, other.get_aabb())
	print("PLANTS_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)

func _process(_d: float) -> void:
	n += 1
	if n < 8:
		return
	set_process(false)
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

extends Node3D
# Oasis spring pools (OasisSprings3D): a spring in a hollow of sand, its pool sized by the water balance
# (A = Q / (E + I)) with a noise outline, and the sand boils at its vents.
#   --view=pool     the whole pool from 8 m up on the shore (default)
#   --view=vent     2 m above the water, steeply down onto the vents through the clear water
#   --view=top      straight down from 40 m (the outline)
#   --raster        raster instead of path tracing
#   --spp=N         path traced samples per pixel (64)
#   --frames=N      frames before saving (180; path tracing waits for its surfaces too)
#   --discharge=Q   the spring's discharge, L/s (0.05)
#   --seed=N        OasisSprings3D.seed
#   --wind=W        wind over the pool, m/s (2)
#   --no-water      hide the water: the bed and the boils alone
#   --out=PATH      save the frame and quit
#   --test          checks only (no image)

var view := "pool"
var raster := false
var spp := 64
var frames := 180
var discharge := 0.05
var seed := 0
var wind := 2.0
var out := ""
var test := false
var oasis: OasisSprings3D
var n := 0
var fails := 0
var cam: Camera3D

const LEVEL := 0.0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--discharge="): discharge = float(a.substr(12))
		elif a.begins_with("--seed="): seed = int(a.substr(7))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--out="): out = a.substr(6)

	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.2, 0.42, 0.78)
	sky_mat.sky_horizon_color = Color(0.72, 0.76, 0.8)
	sky_mat.ground_horizon_color = Color(0.72, 0.76, 0.8)
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and not test
	env.pathtracing_samples_per_pixel = spp
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55, -150, 0)
	sun.shadow_enabled = true
	sun.light_energy = 1.4
	add_child(sun)

	oasis = OasisSprings3D.new()
	oasis.seed = seed
	oasis.springs = PackedVector4Array([Vector4(0.0, LEVEL, 0.0, discharge)])
	add_child(oasis)
	_ground(oasis.get_outline(0))
	# A light air (no weather here): palms round a spring shelter it.
	var pool: PhysXWaterSurface3D = oasis.get_pool(0)
	pool.follow_weather = false
	pool.wind_speed = wind
	if "--no-water" in OS.get_cmdline_user_args():
		pool.visible = false # the bed and the boils alone

	cam = Camera3D.new()
	cam.fov = 55
	add_child(cam)
	var r := oasis.get_pool_radius(0) if oasis.get_pool_count() > 0 else 12.0
	match view:
		"vent":
			cam.look_at_from_position(Vector3(-1.2, LEVEL + 2.0, 1.6), Vector3(0.0, LEVEL - 1.8, 0.0))
		"top":
			cam.look_at_from_position(Vector3(0.0, 40.0, 0.01), Vector3.ZERO)
		_:
			cam.look_at_from_position(Vector3(-r * 1.05, LEVEL + 8.0, r * 1.25), Vector3(0.0, LEVEL - 1.0, -r * 0.1))
	cam.current = true


# A hollow in dune sand carved to the pool's outline (as terrain would: the shore just inside it), 2.4 m deep
# in the middle, with low dunes past its rim. Static collision for the pool's seabed and the vents.
func _ground(outline: PackedVector2Array) -> void:
	var c := Vector2.ZERO
	for p in outline:
		c += p
	c /= outline.size()
	var size := 70.0
	var cells := 280
	var step := size / cells
	var noise := FastNoiseLite.new()
	noise.frequency = 0.035
	noise.seed = 7
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var h := func(x: float, z: float) -> float:
		var q := Vector2(x, z) - c
		var t := fposmod(q.angle(), TAU) / TAU * outline.size()
		var i := int(t) % outline.size()
		var j := (i + 1) % outline.size()
		var rad := lerpf((outline[i] - c).length(), (outline[j] - c).length(), t - floor(t))
		var d := q.length() / (rad * 1.05)
		var bowl := -2.7 * pow(maxf(1.0 - d * d, 0.0), 1.2)
		return 0.25 + bowl + 0.6 * noise.get_noise_2d(x, z) * clampf(d - 1.1, 0.0, 1.0)
	for zi in cells:
		for xi in cells:
			var x0 := -size * 0.5 + xi * step
			var z0 := -size * 0.5 + zi * step
			var p := [Vector3(x0, h.call(x0, z0), z0), Vector3(x0 + step, h.call(x0 + step, z0), z0),
					Vector3(x0, h.call(x0, z0 + step), z0 + step), Vector3(x0 + step, h.call(x0 + step, z0 + step), z0 + step)]
			for i in [0, 1, 2, 1, 3, 2]:
				st.set_uv(Vector2(p[i].x, p[i].z) * 0.25)
				st.add_vertex(p[i])
	st.generate_normals()
	var mesh := st.commit()
	var mat := StandardMaterial3D.new()
	var tex := NoiseTexture2D.new()
	tex.seamless = true
	tex.width = 512
	tex.height = 512
	var tn := FastNoiseLite.new()
	tn.frequency = 0.08
	tex.noise = tn
	var grad := Gradient.new()
	grad.set_color(0, Color(0.68, 0.6, 0.45))
	grad.set_color(1, Color(0.86, 0.77, 0.6))
	tex.color_ramp = grad
	mat.albedo_texture = tex
	mat.roughness = 0.95
	mesh.surface_set_material(0, mat)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	add_child(mi)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)


func _check(what: String, ok: bool, detail: String) -> void:
	print(("PASS " if ok else "FAIL ") + what + "  (" + detail + ")")
	if not ok:
		fails += 1


func _physics_process(_delta: float) -> void:
	n += 1
	if test and n == 30:
		var a := OasisSprings3D.pool_area(0.05, 2.5, 1.0)
		_check("water balance: 0.05 L/s against 3.5 m a year", absf(a - 450.8) < 0.5, "%.1f m2" % a)
		_check("no weather: evaporation 2.5 m a year", absf(oasis.evaporation_at(0.0) - 2.5) < 1e-4, "%.2f" % oasis.evaporation_at(0.0))
		_check("one pool per spring", oasis.get_pool_count() == 1, str(oasis.get_pool_count()))
		var pool: PhysXWaterSurface3D = oasis.get_pool(0)
		_check("pool radius from its area", absf(oasis.get_pool_radius(0) - sqrt(a / PI)) < 0.01, "%.2f m" % oasis.get_pool_radius(0))
		var vents := oasis.get_vents(0)
		_check("one to three vents", vents.size() >= 1 and vents.size() <= 3, str(vents.size()))
		var wet := 0
		for v in vents:
			if pool.sample_depth(Vector3(v.x, LEVEL, v.z)) > 0.5:
				wet += 1
		_check("every vent under water, on the bed", wet == vents.size(), "%d of %d, bed %.2f m" % [wet, vents.size(), vents[0].y])
		var boils := 0
		for c in oasis.get_children(true):
			if c is MeshInstance3D:
				boils += 1
		_check("a sand boil at each vent", boils == vents.size(), str(boils))
		# Two springs at different places never share an outline (the no-repetition rule).
		oasis.springs = PackedVector4Array([Vector4(0, LEVEL, 0, 0.05), Vector4(200, LEVEL, 0, 0.05)])
		var m0: Mesh = oasis.get_pool(0).surface_mesh
		var m1: Mesh = oasis.get_pool(1).surface_mesh
		var v0: PackedVector3Array = m0.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var v1: PackedVector3Array = m1.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var diff := 0.0
		for i in v0.size():
			diff = maxf(diff, (v0[i] - v1[i]).length())
		_check("two springs, two outlines", diff > 0.5, "%.2f m apart at most" % diff)
		var area := 0.0
		for i in range(0, v0.size(), 3):
			area += absf((v0[i + 1] - v0[i]).cross(v0[i + 2] - v0[i]).y) * 0.5
		_check("outline holds the balance's area", absf(area - a) < 0.01 * a, "%.1f m2" % area)
		_check("outline for the ground to carve to", oasis.get_outline(0).size() == 128, str(oasis.get_outline(0).size()))
		print("oasis: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if out != "" and n >= frames:
		if not raster and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0 and n < frames + 600:
			return
		set_physics_process(false)
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

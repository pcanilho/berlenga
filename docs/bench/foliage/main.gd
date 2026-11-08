extends Node3D
# Generic foliage bench for the path tracer: alpha-scissor leaf cards, the case opacity micromaps are for.
# Trees (a trunk and a crown of crossed leaf cards, two surfaces of one mesh) as scene nodes, and a grass
# MultiMesh of cards, all with one procedural leaf texture. Prints frame timings; run with --gpu-profile
# for per-pass GPU times. Opacity micromaps are a restart setting, so turn them on with an override.cfg
# next to project.godot holding:
#   [rendering]
#   pathtracing/opacity_micromaps=true
#
# godot --path docs/bench/foliage --gpu-profile -- [options]
#   --trees=N         trees (default 3000)
#   --cards=N         leaf cards per tree crown (default 160)
#   --grass=N         grass cards in the MultiMesh (default 40000; above the merge cap, so one TLAS entry per card)
#   --radius=M        forest radius in meters (default 120)
#   --frames=N        measured frames (default 300), after --warmup=N frames (default 120)
#   --spp=N --bounces=N  path tracing samples and bounces (default 1, 2)
#   --raster          raster only, for comparison
#   --out=path.png    save the last frame
var trees := 3000
var cards := 160
var grass := 40000
var radius := 120.0
var frames := 300
var warmup := 120
var spp := 1
var bounces := 2
var raster := false
var out := ""
var n := 0
var frame_ms: Array[float] = []
var gpu_ms: Array[float] = []
var last_t := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--trees="): trees = int(a.substr(8))
		elif a.begins_with("--cards="): cards = int(a.substr(8))
		elif a.begins_with("--grass="): grass = int(a.substr(8))
		elif a.begins_with("--radius="): radius = float(a.substr(9))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--warmup="): warmup = int(a.substr(9))
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a == "--raster": raster = true
		elif a.begins_with("--out="): out = a.substr(6)
	seed(7)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.55, 0.68, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.5, 0.6, 0.75)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	add_child(sun)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(radius * 2.5, radius * 2.5)
	pm.material = _mat(Color(0.3, 0.27, 0.2), null)
	ground.mesh = pm
	add_child(ground)

	var leaves := _leaf_texture()
	var leaf_mat := _mat(Color(1, 1, 1), leaves)
	var tree := _tree_mesh(leaf_mat)
	for i in trees:
		var r := radius * sqrt(randf())
		var a := randf() * TAU
		var t := MeshInstance3D.new()
		t.mesh = tree
		t.position = Vector3(cos(a) * r, 0.0, sin(a) * r)
		t.rotation.y = randf() * TAU
		t.scale = Vector3.ONE * randf_range(0.7, 1.3)
		add_child(t)
	_grass(leaf_mat)

	var cam := Camera3D.new()
	cam.fov = 65
	cam.far = radius * 3.0
	add_child(cam)
	cam.position = Vector3(-radius * 0.15, 1.7, -radius * 0.1)
	cam.look_at(Vector3(radius * 0.3, 4.0, radius * 0.25))
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	print("BENCH trees=%d cards=%d grass=%d spp=%d bounces=%d raster=%s opacity_micromaps=%s" % [trees, cards, grass, spp, bounces, raster,
			ProjectSettings.get_setting("rendering/pathtracing/opacity_micromaps", false)])

func _mat(c: Color, tex: Texture2D) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 0.85
	if tex:
		m.albedo_texture = tex
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		m.alpha_scissor_threshold = 0.5
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
	return m

# A 512 x 512 atlas cell of leaves: pointed ellipses on transparent texels, green with darker veins.
func _leaf_texture() -> ImageTexture:
	var size := 512
	var img := Image.create(size, size, false, Image.FORMAT_RGBA8)
	img.fill(Color(0.18, 0.3, 0.12, 0.0))
	var rng := RandomNumberGenerator.new()
	rng.seed = 11
	for k in 80:
		var c := Vector2(rng.randf_range(0.12, 0.88), rng.randf_range(0.12, 0.88)) * size
		var ang := rng.randf() * TAU
		var length := rng.randf_range(35.0, 75.0)
		var width := length * rng.randf_range(0.28, 0.4)
		var dir := Vector2(cos(ang), sin(ang))
		var tint := Color(rng.randf_range(0.15, 0.3), rng.randf_range(0.32, 0.5), rng.randf_range(0.08, 0.16), 1.0)
		var ext := int(length) + 2
		for y in range(max(int(c.y) - ext, 0), min(int(c.y) + ext, size)):
			for x in range(max(int(c.x) - ext, 0), min(int(c.x) + ext, size)):
				var d := Vector2(x, y) - c
				var u := d.dot(dir) / length
				var v := d.dot(Vector2(-dir.y, dir.x)) / width
				# Pointed at both ends: the half-width shrinks with 1 - u^2.
				var edge := 1.0 - u * u
				if edge <= 0.0:
					continue
				var inside := edge - absf(v)
				if inside > 0.0:
					var vein := 1.0 - 0.35 * exp(-v * v * 400.0)
					img.set_pixel(x, y, Color(tint.r * vein, tint.g * vein, tint.b * vein, 1.0))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)

# A trunk surface and a crown of crossed leaf cards (one surface), so the tree is a two-surface mesh.
func _tree_mesh(leaf_mat: Material) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.12
	trunk.bottom_radius = 0.22
	trunk.height = 6.0
	trunk.radial_segments = 10
	var ta := trunk.get_mesh_arrays()
	var tv: PackedVector3Array = ta[Mesh.ARRAY_VERTEX]
	for k in tv.size():
		tv[k] += Vector3(0, 3.0, 0)
	ta[Mesh.ARRAY_VERTEX] = tv
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, ta)
	mesh.surface_set_material(0, _mat(Color(0.32, 0.24, 0.17), null))

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var rng := RandomNumberGenerator.new()
	rng.seed = 3
	for i in cards:
		var centre := Vector3(rng.randfn(0.0, 1.1), 6.5 + rng.randfn(0.0, 1.0), rng.randfn(0.0, 1.1))
		var basis := Basis.from_euler(Vector3(rng.randf_range(-0.6, 0.6), rng.randf() * TAU, rng.randf_range(-0.6, 0.6)))
		var half := rng.randf_range(0.6, 1.0)
		var right := basis.x * half
		var up := basis.y * half
		var nrm := basis.z
		var corners := [centre - right - up, centre + right - up, centre + right + up, centre - right + up]
		var uvs := [Vector2(0, 1), Vector2(1, 1), Vector2(1, 0), Vector2(0, 0)]
		for idx in [0, 1, 2, 0, 2, 3]:
			st.set_normal(nrm)
			st.set_uv(uvs[idx])
			st.add_vertex(corners[idx])
	st.generate_tangents()
	st.commit(mesh)
	mesh.surface_set_material(1, leaf_mat)
	return mesh

func _grass(leaf_mat: Material) -> void:
	if grass <= 0:
		return
	var card := QuadMesh.new()
	card.size = Vector2(0.5, 0.5)
	card.center_offset = Vector3(0, 0.25, 0)
	card.material = leaf_mat
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = card
	mm.instance_count = grass
	for i in grass:
		var r := radius * sqrt(randf())
		var a := randf() * TAU
		var xf := Transform3D(Basis.from_euler(Vector3(0, randf() * TAU, 0)).scaled(Vector3.ONE * randf_range(0.6, 1.4)), Vector3(cos(a) * r, 0.0, sin(a) * r))
		mm.set_instance_transform(i, xf)
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)

func _process(_delta: float) -> void:
	n += 1
	var now := Time.get_ticks_usec()
	if n > warmup:
		frame_ms.push_back((now - last_t) / 1000.0)
		gpu_ms.push_back(RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
	last_t = now
	if n == warmup + frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
		frame_ms.sort()
		gpu_ms.sort()
		print("RESULT frame_ms_median=%.2f frame_ms_p95=%.2f gpu_ms_median=%.2f gpu_ms_p95=%.2f" % [
			frame_ms[frame_ms.size() / 2], frame_ms[int(frame_ms.size() * 0.95)],
			gpu_ms[gpu_ms.size() / 2], gpu_ms[int(gpu_ms.size() * 0.95)]])
		get_tree().quit()

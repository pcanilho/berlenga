extends Node3D
# MultiMesh instance bones against Godot's own skinning. Fifteen bent stems, each a column with
# three bones, drawn either as one MultiMesh with bones_per_instance = 3 and a particle_trails
# material, or as fifteen MeshInstance3D nodes skinned by a Skeleton3D each, in the same pose.
# The frames should match in raster and under path tracing.
#
# godot --path docs/bench/instance_bones --rendering-driver vulkan -- [options]
#   --mode=bones      one MultiMesh with instance bones (default)
#   --mode=skeleton   one Skeleton3D and MeshInstance3D per stem (reference)
#   --mode=rest       instance bones, all in rest pose (shows the bend is real)
#   --raster          raster only
#   --taa             TAA on, so the MultiMesh keeps two motion-vector halves
#   --spp=N           path tracing samples per pixel (default 16)
#   --frames=N        frames before the capture (default 40)
#   --move            change the pose at frame 20 (the capture and BLAS must follow)
#   --out=path.png    save the last frame
#   --grid=N          N x N stems instead of 5 x 3 (cost runs)
#   --animate         sway every stem every frame (bones rewritten / bone poses set each frame)
#   --mode=plain      unskinned MultiMesh of the same stems (cost baseline)
# Prints the mean CPU and GPU render time over the last half of the frames.

const HEIGHTS := [0.0, 0.8, 1.6] # Bone pivots along the 2.4 m column.
var COLS := 5
var ROWS := 3
var animate := false
var cpu_ms := 0.0
var gpu_ms := 0.0
var process_ms := 0.0
var samples := 0
var mode := "bones"
var raster := false
var taa := false
var move := false
var bend := 1.0
var skeletons: Array[Skeleton3D] = []
var spp := 16
var frames := 40
var out := ""
var n := 0
var mm: MultiMesh

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--mode="): mode = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--taa": taa = true
		elif a == "--move": move = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--grid="):
			COLS = int(a.substr(7))
			ROWS = COLS
		elif a == "--animate": animate = true
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	get_viewport().use_taa = taa
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.55, 0.68, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.45, 0.5, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55, 35, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(30, 30)
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.35, 0.33, 0.3)
	pm.material = gm
	ground.mesh = pm
	add_child(ground)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 2.2, 5.2)
	cam.rotation_degrees = Vector3(-16, 0, 0)
	add_child(cam)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.3, 0.6, 0.25)
	mat.roughness = 0.7
	var mesh := _stem_mesh()
	if mode == "skeleton":
		for i in COLS * ROWS:
			_add_skeleton_stem(mesh, mat, i)
	elif mode == "plain":
		var pmm := MultiMesh.new()
		pmm.transform_format = MultiMesh.TRANSFORM_3D
		pmm.mesh = mesh
		pmm.instance_count = COLS * ROWS
		for i in COLS * ROWS:
			pmm.set_instance_transform(i, _placement(i))
		mesh.surface_set_material(0, mat)
		var pmi := MultiMeshInstance3D.new()
		pmi.multimesh = pmm
		add_child(pmi)
	else:
		mat.use_particle_trails = true
		mm = MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_colors = true
		mm.use_custom_data = true
		mm.mesh = mesh
		mm.bones_per_instance = HEIGHTS.size()
		mm.instance_count = COLS * ROWS * HEIGHTS.size()
		mesh.surface_set_material(0, mat)
		var mi := MultiMeshInstance3D.new()
		mi.multimesh = mm
		add_child(mi)

# A 0.25 m square column, 2.4 m tall, in 24 rings, each vertex blended between the two nearest bones.
func _stem_mesh() -> ArrayMesh:
	var st := SurfaceTool.new()
	st.set_skin_weight_count(SurfaceTool.SKIN_4_WEIGHTS)
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var rings := 24
	var h := 2.4
	var r := 0.125
	var corners := [Vector2(-r, -r), Vector2(r, -r), Vector2(r, r), Vector2(-r, r)]
	for k in rings:
		for s in 4:
			var c0: Vector2 = corners[s]
			var c1: Vector2 = corners[(s + 1) % 4]
			var y0 := h * k / rings
			var y1 := h * (k + 1) / rings
			var quad := [Vector3(c0.x, y0, c0.y), Vector3(c1.x, y0, c1.y), Vector3(c1.x, y1, c1.y), Vector3(c0.x, y1, c0.y)]
			var nrm := Vector3((c0.x + c1.x) * 0.5, 0, (c0.y + c1.y) * 0.5).normalized()
			for idx in [0, 2, 1, 0, 3, 2]:
				var v: Vector3 = quad[idx]
				st.set_normal(nrm)
				_set_weights(st, v.y)
				st.add_vertex(v)
	return st.commit()

func _set_weights(st: SurfaceTool, y: float) -> void:
	var b := 0
	while b + 1 < HEIGHTS.size() and y >= HEIGHTS[b + 1]:
		b += 1
	var w := 1.0
	if b + 1 < HEIGHTS.size():
		w = 1.0 - clampf((y - HEIGHTS[b]) / (HEIGHTS[b + 1] - HEIGHTS[b]), 0.0, 1.0)
	var nb := mini(b + 1, HEIGHTS.size() - 1)
	st.set_bones(PackedInt32Array([b, nb, 0, 0]))
	st.set_weights(PackedFloat32Array([w, 1.0 - w, 0.0, 0.0]))

func _placement(i: int) -> Transform3D:
	var x := (float(i % COLS) - (COLS - 1) * 0.5) * 1.6
	var z := -float(i / COLS) * 1.6
	return Transform3D(Basis(Vector3.UP, 0.4 * i), Vector3(x, 0, z))

func _angles(i: int) -> Array[float]:
	if mode == "rest":
		return [0.0, 0.0, 0.0]
	return [0.08 * (i % 3) * bend, (0.25 + 0.03 * i) * bend, (0.35 - 0.02 * i) * bend]

# Skin matrix of each bone: the product of every pivot rotation up to it (bind pose is the rest pose).
func _skin(i: int) -> Array[Transform3D]:
	var a := _angles(i)
	var res: Array[Transform3D] = []
	var acc := Transform3D()
	for b in HEIGHTS.size():
		var pivot := Vector3(0, HEIGHTS[b], 0)
		acc = acc * Transform3D(Basis(), pivot) * Transform3D(Basis(Vector3.RIGHT, a[b]), Vector3()) * Transform3D(Basis(), -pivot)
		res.append(acc)
	return res

func _add_skeleton_stem(mesh: Mesh, mat: Material, i: int) -> void:
	var sk := Skeleton3D.new()
	sk.transform = _placement(i)
	add_child(sk)
	skeletons.append(sk)
	var a := _angles(i)
	for b in HEIGHTS.size():
		sk.add_bone("b%d" % b)
		var rest_y: float = HEIGHTS[b] - (HEIGHTS[b - 1] if b > 0 else 0.0)
		if b > 0:
			sk.set_bone_parent(b, b - 1)
		sk.set_bone_rest(b, Transform3D(Basis(), Vector3(0, rest_y, 0)))
		sk.set_bone_pose_position(b, Vector3(0, rest_y, 0))
		sk.set_bone_pose_rotation(b, Quaternion(Vector3.RIGHT, a[b]))
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = mat
	sk.add_child(mi)
	mi.skeleton = NodePath("..")

func _write_bones() -> void:
	var d := PackedFloat32Array()
	for i in COLS * ROWS:
		var p := _placement(i)
		for skin in _skin(i):
			var t := p * skin
			d.append_array([t.basis.x.x, t.basis.y.x, t.basis.z.x, t.origin.x,
					t.basis.x.y, t.basis.y.y, t.basis.z.y, t.origin.y,
					t.basis.x.z, t.basis.y.z, t.basis.z.z, t.origin.z,
					1.0, 1.0, 1.0, 1.0,
					0.0, 0.0, 0.0, 0.0])
	mm.buffer = d

func _process(_delta: float) -> void:
	n += 1
	if n == 3 and mm:
		_write_bones()
	if animate:
		var t0 := Time.get_ticks_usec()
		bend = 1.0 + 0.5 * sin(n * 0.2)
		if mm:
			_write_bones()
		for i in skeletons.size():
			var a := _angles(i)
			for b in HEIGHTS.size():
				skeletons[i].set_bone_pose_rotation(b, Quaternion(Vector3.RIGHT, a[b]))
		if n > frames / 2:
			process_ms += (Time.get_ticks_usec() - t0) / 1000.0
	if n > frames / 2:
		var vp := get_viewport().get_viewport_rid()
		cpu_ms += RenderingServer.viewport_get_measured_render_time_cpu(vp) + RenderingServer.get_frame_setup_time_cpu()
		gpu_ms += RenderingServer.viewport_get_measured_render_time_gpu(vp)
		samples += 1
	if move and n == 20:
		bend = -0.6
		if mm:
			_write_bones()
		for i in skeletons.size():
			var a := _angles(i)
			for b in HEIGHTS.size():
				skeletons[i].set_bone_pose_rotation(b, Quaternion(Vector3.RIGHT, a[b]))
	if n < frames:
		return
	set_process(false)
	if samples > 0:
		print("IB_COST mode=%s stems=%d cpu_ms=%.3f gpu_ms=%.3f script_ms=%.3f" % [mode, COLS * ROWS, cpu_ms / samples, gpu_ms / samples, process_ms / samples])
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

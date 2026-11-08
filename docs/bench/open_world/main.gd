extends Node3D
# Generic open-world bench for the path tracer: rocks, trees and blocks scattered over a disk
# around a slowly moving camera. Prints frame timings and memory; run with --gpu-profile for
# per-pass GPU times (Acceleration Structure Build, TLAS Build, Pathtracer). cpu_ms is the viewport's
# render time on the CPU (scene cull, TLAS records, command recording), setup_ms the frame setup
# (dirty instances), process_ms the scripts.
#
# godot --path docs/bench/open_world --gpu-profile -- [options]
#   --count=N         instances (default 20000)
#   --radius=M        world radius in meters (default 2000)
#   --unique=N        distinct rock meshes (default 16); more means more BLAS memory
#   --frames=N        measured frames (default 300), after 60 warm-up frames
#   --spp=N --bounces=N  path tracing samples and bounces (default 1, 2)
#   --raster          raster only, for comparison
#   --still           camera does not move
#   --movers=N        N extra boxes near the camera that move every frame
#   --churn=N         free N objects and add N new ones every frame
#   --recolor         change the blocks' material every frame
#   --fly=S           camera flies along X at S m/s instead of circling (crosses origin cells)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/mesh_lod:1
#   --out=path.png    save the last frame
var count := 20000
var radius := 2000.0
var unique := 16
var frames := 300
var warmup := 60
var spp := 1
var bounces := 2
var raster := false
var still := false
var out := ""
var movers: Array[Node3D] = []
var mover_count := 0
var churn := 0
var recolor := false
var fly := 0.0
var box_mat: StandardMaterial3D
var churn_mesh: BoxMesh
var n := 0
var t0 := 0
var cam: Camera3D
var frame_ms: Array[float] = []
var gpu_ms: Array[float] = []
var cpu_ms: Array[float] = []
var setup_ms: Array[float] = []
var process_ms: Array[float] = []
var last_t := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--count="): count = int(a.substr(8))
		elif a.begins_with("--radius="): radius = float(a.substr(9))
		elif a.begins_with("--unique="): unique = int(a.substr(9))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a == "--raster": raster = true
		elif a == "--still": still = true
		elif a.begins_with("--movers="): mover_count = int(a.substr(9))
		elif a.begins_with("--churn="): churn = int(a.substr(8))
		elif a == "--recolor": recolor = true
		elif a.begins_with("--fly="): fly = float(a.substr(6))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":")
			ProjectSettings.set_setting(kv[0], float(kv[1]))
	seed(5)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.5, 0.62, 0.8)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.5, 0.6, 0.8)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 35, 0)
	sun.light_energy = 1.3
	sun.shadow_enabled = true
	add_child(sun)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(radius * 2.2, radius * 2.2)
	pm.material = mat(Color(0.35, 0.4, 0.28), 0.95)
	ground.mesh = pm
	add_child(ground)
	_scatter()
	_add_movers()
	cam = Camera3D.new()
	cam.fov = 60
	cam.far = radius * 1.5
	add_child(cam)
	_place_camera(0.0)
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	print("BENCH count=%d radius=%d unique=%d spp=%d bounces=%d raster=%s" % [count, radius, unique, spp, bounces, raster])

func mat(c: Color, r := 0.8) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = r
	return m

func _rock(i: int) -> ArrayMesh:
	var sm := SphereMesh.new()
	sm.radial_segments = 96
	sm.rings = 48
	var arr := sm.get_mesh_arrays()
	var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var rng := RandomNumberGenerator.new()
	rng.seed = i + 1
	var f := Vector3(rng.randf_range(2, 5), rng.randf_range(2, 5), rng.randf_range(2, 5))
	for k in v.size():
		var p := v[k]
		var d := 1.0 + 0.15 * sin(p.x * f.x * 3.0) * cos(p.y * f.y * 3.0) + 0.08 * sin(p.z * f.z * 7.0 + p.x * 5.0)
		v[k] = p * d * Vector3(1.4, 0.8, 1.1)
	arr[Mesh.ARRAY_VERTEX] = v
	arr[Mesh.ARRAY_NORMAL] = null
	var st := SurfaceTool.new()
	st.create_from_arrays(arr)
	st.generate_normals()
	var im := ImporterMesh.new()
	im.add_surface(Mesh.PRIMITIVE_TRIANGLES, st.commit_to_arrays())
	im.generate_lods(25, 60, [])
	return im.get_mesh()

func _tree() -> Array[Mesh]:
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.15
	trunk.bottom_radius = 0.25
	trunk.height = 4.0
	trunk.radial_segments = 12
	trunk.material = mat(Color(0.3, 0.22, 0.15))
	var crown := SphereMesh.new()
	crown.radius = 1.8
	crown.height = 4.0
	crown.radial_segments = 24
	crown.rings = 12
	crown.material = mat(Color(0.2, 0.35, 0.15))
	return [trunk, crown]

func _scatter() -> void:
	var rocks: Array[Mesh] = []
	for i in unique:
		rocks.push_back(_rock(i))
	var rock_mat := mat(Color(0.5, 0.47, 0.43))
	var tree := _tree()
	var box := BoxMesh.new()
	box_mat = mat(Color(0.6, 0.58, 0.55))
	box.material = box_mat
	# MultiMesh-free on purpose: one instance per object, as a streamed world of scene nodes.
	for i in count:
		# Uniform over the disk, but denser near the centre so near detail exists.
		var r := radius * pow(randf(), 0.75)
		var a := randf() * TAU
		var pos := Vector3(cos(a) * r, 0.0, sin(a) * r)
		var kind := i % 10
		if kind < 5:
			var mi := MeshInstance3D.new()
			mi.mesh = rocks[i % unique]
			mi.material_override = rock_mat
			mi.position = pos + Vector3(0, 0.4, 0)
			mi.rotation.y = randf() * TAU
			mi.scale = Vector3.ONE * randf_range(0.5, 3.0)
			add_child(mi)
		elif kind < 9:
			var s := randf_range(0.7, 1.5)
			var t := MeshInstance3D.new()
			t.mesh = tree[0]
			t.position = pos + Vector3(0, 2.0 * s, 0)
			t.scale = Vector3.ONE * s
			add_child(t)
			var c := MeshInstance3D.new()
			c.mesh = tree[1]
			c.position = pos + Vector3(0, 5.0 * s, 0)
			c.scale = Vector3.ONE * s
			add_child(c)
		else:
			var b := MeshInstance3D.new()
			b.mesh = box
			var sz := Vector3(randf_range(4, 14), randf_range(3, 25), randf_range(4, 14))
			b.scale = sz
			b.position = pos + Vector3(0, sz.y / 2, 0)
			b.rotation.y = randf() * TAU
			add_child(b)

func _add_movers() -> void:
	var box := BoxMesh.new()
	box.material = mat(Color(0.7, 0.3, 0.25))
	for i in mover_count:
		var m := MeshInstance3D.new()
		m.mesh = box
		add_child(m)
		movers.push_back(m)
	_move_movers(0.0)

# Movers circle the camera's path, each on its own radius and phase.
func _move_movers(t: float) -> void:
	for i in movers.size():
		var r := 20.0 + float(i % 97) * 1.7
		var a := t * 0.4 + float(i) * 0.61
		movers[i].position = Vector3(cos(a) * r, 1.0 + float(i % 7) * 0.5, sin(a) * r)
		movers[i].rotation.y = a

# Frees the oldest scattered objects and adds as many new ones, so instance slots are reused.
func _churn() -> void:
	var removed := 0
	for c in get_children():
		if removed >= churn:
			break
		if c is MeshInstance3D and c.mesh is BoxMesh and not movers.has(c):
			c.queue_free()
			removed += 1
	if churn_mesh == null:
		churn_mesh = BoxMesh.new()
		churn_mesh.material = box_mat
	for i in removed:
		var b := MeshInstance3D.new()
		b.mesh = churn_mesh
		var a := randf() * TAU
		var r := randf_range(10.0, 300.0)
		b.scale = Vector3(randf_range(2, 6), randf_range(2, 10), randf_range(2, 6))
		b.position = cam.position + Vector3(cos(a) * r, b.scale.y / 2 - cam.position.y, sin(a) * r)
		add_child(b)

func _place_camera(t: float) -> void:
	if fly > 0.0:
		cam.position = Vector3(t * fly - 1500.0, 2.5, 5.0)
		cam.look_at(cam.position + Vector3(100.0, 0.0, 30.0))
		return
	var a := t * 0.05
	cam.position = Vector3(cos(a) * 30.0, 2.5, sin(a) * 30.0)
	cam.look_at(Vector3(cos(a + 0.6) * 200.0, 3.0, sin(a + 0.6) * 200.0))

func _process(delta: float) -> void:
	n += 1
	if not still:
		_place_camera(float(n) / 60.0)
	if not movers.is_empty():
		_move_movers(float(n) / 60.0)
	if churn > 0:
		_churn()
	if recolor:
		box_mat.albedo_color = Color.from_hsv(fmod(float(n) * 0.01, 1.0), 0.4, 0.7)
	var now := Time.get_ticks_usec()
	if n > warmup:
		frame_ms.push_back((now - last_t) / 1000.0)
		gpu_ms.push_back(RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
		cpu_ms.push_back(RenderingServer.viewport_get_measured_render_time_cpu(get_viewport().get_viewport_rid()))
		setup_ms.push_back(RenderingServer.get_frame_setup_time_cpu())
		process_ms.push_back(Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0)
	last_t = now
	if n == warmup + frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
		frame_ms.sort()
		gpu_ms.sort()
		cpu_ms.sort()
		setup_ms.sort()
		process_ms.sort()
		var mem := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED) / 1048576.0
		var tex := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED) / 1048576.0
		print("RESULT frame_ms_median=%.2f frame_ms_p95=%.2f gpu_ms_median=%.2f gpu_ms_p95=%.2f buffer_mb=%.1f texture_mb=%.1f objects=%d cpu_ms_median=%.2f setup_ms_median=%.2f process_ms_median=%.2f" % [
			frame_ms[frame_ms.size() / 2], frame_ms[int(frame_ms.size() * 0.95)],
			gpu_ms[gpu_ms.size() / 2], gpu_ms[int(gpu_ms.size() * 0.95)], mem, tex, get_child_count(),
			cpu_ms[cpu_ms.size() / 2], setup_ms[setup_ms.size() / 2], process_ms[process_ms.size() / 2]])
		get_tree().quit()

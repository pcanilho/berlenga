# Ray tracing memory bench: static meshes of several kinds, path traced, to measure acceleration structure
# memory and to compare frames between engine builds (docs/ideas/optimisation.md).
#
# godot --path docs/bench/rt_memory --rendering-driver vulkan [--verbose] -- [options]
#   --out=path.png    save the frame (path traced unless --raster)
#   --raster          raster only
#   --spp=N           path tracing samples (default 4)
#   --no-elision      rendering/pathtracing/scene_feature_elision off (the frame must not change)
#   --frames=N        frames drawn after nothing is pending, before the saved one (default 30)
#   --copies=N        copies of each mesh in a row (default 6)
#   --persistent      rendering/pathtracing/persistent_instances on (keyed TLAS builds)
#   --medium          add a slab of water (Transmission, not thin-walled) over the front row: caustics and media run
#   --no-caustics     rendering/pathtracing/caustics off
#   --no-radiance-cache  rendering/pathtracing/radiance_cache off (the cache is not bit-repeatable between runs)
#   --evict           hide the meshes for --evict-frames frames (default 150) once settled, then show them again:
#                     the path tracer frees their unused BLASes and rebuilds them (the frame must not change)
# Prints the video memory in use at the saved frame. With --verbose the engine also logs BLAS compaction
# ("BLAS compaction: ..."); env RD_BLAS_COMPACTION_MOVE_ALL=1 moves every BLAS even where the driver reports no
# saving (lavapipe), so the copies run and the frames can be compared in the cloud.
extends Node3D

var out := ""
var raster := false
var spp := 4
var frames := 30
var copies := 6
var medium := false
var evict_frames := 0
var rows: Array[Node3D] = []


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--out="):
			out = a.substr(6)
		elif a == "--raster":
			raster = true
		elif a.begins_with("--spp="):
			spp = int(a.substr(6))
		elif a.begins_with("--frames="):
			frames = int(a.substr(9))
		elif a.begins_with("--copies="):
			copies = int(a.substr(9))
		elif a == "--evict":
			evict_frames = 150
		elif a.begins_with("--evict-frames="):
			evict_frames = int(a.substr(15))
		elif a == "--medium":
			medium = true
		elif a == "--no-caustics":
			ProjectSettings.set_setting("rendering/pathtracing/caustics", false)
		elif a == "--no-radiance-cache":
			ProjectSettings.set_setting("rendering/pathtracing/radiance_cache", false)
		elif a == "--no-elision":
			ProjectSettings.set_setting("rendering/pathtracing/scene_feature_elision", false)
		elif a == "--persistent":
			ProjectSettings.set_setting("rendering/pathtracing/persistent_instances", true)
	_build()
	_run()


func _mem() -> float:
	return RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED) / 1.0e6


func _material(c: Color, rough: float, metal := 0.0) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = rough
	m.metallic = metal
	return m


# A heightfield with fine relief: many small triangles, the case compaction cares about.
func _relief(n: int, size: float) -> ArrayMesh:
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 0.08
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for z in n + 1:
		for x in n + 1:
			var px := (float(x) / n - 0.5) * size
			var pz := (float(z) / n - 0.5) * size
			st.add_vertex(Vector3(px, noise.get_noise_2d(px * 10.0, pz * 10.0) * 0.25, pz))
	for z in n:
		for x in n:
			var i := z * (n + 1) + x
			st.add_index(i)
			st.add_index(i + 1)
			st.add_index(i + n + 1)
			st.add_index(i + 1)
			st.add_index(i + n + 2)
			st.add_index(i + n + 1)
	st.generate_normals()
	return st.commit()


func _build() -> void:
	var sphere := SphereMesh.new()
	sphere.radial_segments = 192
	sphere.rings = 96
	var torus := TorusMesh.new()
	torus.rings = 160
	torus.ring_segments = 64
	torus.inner_radius = 0.25
	torus.outer_radius = 0.5
	var box := BoxMesh.new()
	box.subdivide_width = 32
	box.subdivide_height = 32
	box.subdivide_depth = 32
	var relief := _relief(160, 1.6)
	var meshes: Array[Mesh] = [sphere, torus, box, relief]
	var mats := [_material(Color(0.8, 0.3, 0.2), 0.4), _material(Color(0.9, 0.9, 0.9), 0.2, 1.0),
			_material(Color(0.2, 0.5, 0.8), 0.6), _material(Color(0.5, 0.45, 0.35), 0.9)]
	for row in meshes.size():
		for i in copies:
			var mi := MeshInstance3D.new()
			mi.mesh = meshes[row]
			mi.material_override = mats[row]
			mi.position = Vector3((i - (copies - 1) * 0.5) * 1.3, 0.6, -row * 1.4)
			mi.rotation = Vector3(0.3 * i, 0.7 * i + row, 0.0)
			add_child(mi)
			rows.append(mi)
	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	ground.mesh = plane
	ground.material_override = _material(Color(0.6, 0.6, 0.6), 0.8)
	add_child(ground)
	if medium:
		var slab := MeshInstance3D.new()
		var slab_box := BoxMesh.new()
		slab_box.size = Vector3(copies * 1.3, 0.6, 1.2)
		slab_box.subdivide_width = 64
		slab_box.subdivide_depth = 16
		slab.mesh = slab_box
		var water := _material(Color(0.9, 0.95, 1.0), 0.05)
		water.transmission_enabled = true
		water.transmission_amount = 1.0
		water.transmission_thin_walled = false
		water.transmission_ior = 1.33
		water.transmission_attenuation_color = Color(0.6, 0.85, 0.9)
		water.transmission_attenuation_distance = 3.0
		slab.material_override = water
		slab.position = Vector3(0.0, 0.3, 0.0)
		add_child(slab)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 35, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = ProceduralSkyMaterial.new()
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var cam := Camera3D.new()
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 3.2, 4.5), Vector3(0, 0.4, -2.0))


func _as_mb() -> float:
	return RenderingServer.get_rendering_device().get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) / 1.0e6


func _settle() -> void:
	for i in 600:
		await RenderingServer.frame_post_draw
		if get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) == 0:
			break


func _run() -> void:
	await _settle()
	if evict_frames > 0:
		print("acceleration structures before hiding: %.1f MB" % _as_mb())
		for mi in rows:
			mi.visible = false
		for i in evict_frames:
			await RenderingServer.frame_post_draw
		print("acceleration structures after %d hidden frames: %.1f MB" % [evict_frames, _as_mb()])
		for mi in rows:
			mi.visible = true
		await _settle()
	for i in frames:
		await RenderingServer.frame_post_draw
	print("video memory at the saved frame: %.1f MB (acceleration structures %.1f MB)" % [_mem(), _as_mb()])
	if out != "":
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

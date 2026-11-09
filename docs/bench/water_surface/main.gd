extends Node3D
# Water surface bench: the PhysX module's PhysXWaterSurface3D (FFT ocean, ripples, shore) over a
# sloping sand floor, with a few floating boxes, raster or path traced. Needs a build with the
# PhysX module (modules/godot_physx/README.md).
#
#   --view=beach    the shore from above: shallows, swash and the sea beyond (default)
#   --view=under    looking down into the shallows, where the floor shows through
#   --view=submerged  the camera 1.4 m under the water, looking along the floor to the rock (--slope=12)
#   --view=waterline  the camera at the surface: the lens split between above and below
#   --view=up       from 2.5 m down, looking up at the surface: Snell's window and the mirror past it
#   --view=river    from above, the rock standing out of the water (with --current)
#   --view=sea      out to sea from 6 m up (with --node=wave_cascades:3 for swell under the chop)
#   --view=wake     straight down on the rock and the water downstream of it
#   --raster        raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --frames=N      frames rendered before --out is saved (default 12)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/refraction:0
#   --out=path.png  save the last frame and quit
#   --debug=N       path tracing debug view (Environment.pathtracing_debug_mode)
#   --water=param:value  a water shader parameter, e.g. --water=pathtraced_scattering:Vector3(0,0,0)
#   --node=prop:value  a PhysXWaterSurface3D property (e.g. live_flow:false)
#   --current=X,Z   a current in m/s (flow round the rocks, boxes drift)
#   --flow-out=path.png  save the solved flow (speed as brightness, direction as hue) and quit
#   --pt-from=N     rasterize until frame N, path trace from then on (a long settle at raster speed)
#   --tracers=N     N white flecks floating on the surface, carried by sample_flow() (shows the flow)
#   --seq=N         also save the N frames after --frames, as path_0000.png ... (for a GIF)
#   --dive=M        over the --seq frames, lower the camera by M metres (through the surface at --view=waterline)
#   --cam-dy=M      raise (or lower) the camera by M metres
#   --slope=DEG     the sand's slope (default 5); steeper reaches deep water sooner
#
# Run with --fixed-fps 30 so raster and path traced frames show the same waves.

var view := "beach"
var raster := false
var spp := 16
var bounces := 3
var frames := 12
var out := ""
var debug_mode := 0
var slope_deg := -1.0
var cam_dy := 0.0
var dive := 0.0
var bench_cam: Camera3D
var water_params := {}
var node_params := {}
var current := Vector2.ZERO
var flow_out := ""
var seq := 0
var tracers := 0
var pt_from := 0
var bench_env: Environment
var tracer_mm: MultiMesh
var tracer_pos := PackedVector3Array()
var tracer_rng := RandomNumberGenerator.new()
var n := 0
var water: Node3D
var sea_series := PackedFloat32Array()


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--debug="): debug_mode = int(a.substr(8))
		elif a.begins_with("--current="):
			var c := a.substr(10).split(",")
			current = Vector2(float(c[0]), float(c[1]))
		elif a.begins_with("--flow-out="): flow_out = a.substr(11)
		elif a.begins_with("--seq="): seq = int(a.substr(6))
		elif a.begins_with("--tracers="): tracers = int(a.substr(10))
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--slope="): slope_deg = float(a.substr(8))
		elif a.begins_with("--cam-dy="): cam_dy = float(a.substr(9))
		elif a.begins_with("--dive="): dive = float(a.substr(7))
		elif a.begins_with("--water="):
			var wkv := a.substr(8).split(":", true, 1)
			water_params[wkv[0]] = str_to_var(wkv[1])
		elif a.begins_with("--node="):
			var nkv := a.substr(7).split(":", true, 1)
			node_params[nkv[0]] = str_to_var(nkv[1])
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":", true, 1)
			ProjectSettings.set_setting(kv[0], str_to_var(kv[1]))

	var sky_mat := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and pt_from <= 0
	bench_env = env
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_debug_mode = debug_mode
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -140, 0)
	sun.shadow_enabled = true
	add_child(sun)

	# Sand: a 200 m slab sloping down away from the camera, from dry beach out to about 9 m of water, with a few rocks.
	var sand_mat := StandardMaterial3D.new()
	sand_mat.albedo_texture = _checker(Color(0.78, 0.7, 0.55), Color(0.62, 0.55, 0.42))
	sand_mat.uv1_scale = Vector3(100, 100, 1)
	sand_mat.roughness = 0.95
	if slope_deg < 0.0:
		# Under water the floor needs to fall away faster, for depth to look through.
		slope_deg = 12.0 if view in ["submerged", "waterline", "up"] else 5.0
	var slope := deg_to_rad(slope_deg)
	_slab(Vector3(200, 1, 200), Vector3(0, -1.2, 0), Vector3(-slope, 0, 0), sand_mat)
	var rock_mat := StandardMaterial3D.new()
	rock_mat.albedo_color = Color(0.3, 0.3, 0.32)
	rock_mat.roughness = 0.8
	for p in [Vector3(-2.5, -1.4, -4.0), Vector3(1.5, -1.9, -7.0), Vector3(3.0, -0.9, -1.5)]:
		_slab(Vector3(1.2, 1.2, 1.2), p, Vector3(0.3, 0.7, 0.2), rock_mat)
	# A rock standing out of the water, for a current to go round.
	_slab(Vector3(2.0, 4.0, 3.0), Vector3(-6.0, -1.0, -9.0), Vector3(0.0, 0.4, 0.0), rock_mat)

	if not ClassDB.class_exists("PhysXWaterSurface3D"):
		push_error("This build has no PhysX module.")
		get_tree().quit(1)
		return
	water = ClassDB.instantiate("PhysXWaterSurface3D")
	water.set("domain_size", Vector2(40, 40))
	water.set("grid_resolution", 160)
	water.set("seabed_from_floor", true)
	water.set("ocean_grid_resolution", 128)
	water.set("wind_speed", 9.0)
	water.set("wind_direction", Vector2(-0.3, 1.0))
	water.set("choppiness", 1.0)
	water.set("render_extent", 600.0)
	water.set("current", current)
	if view == "waterline":
		water.set("wave_amplitude", 0.3) # small waves, so the surface stays across the lens
	for k in node_params:
		water.set(k, node_params[k])
	add_child(water)
	var wm: ShaderMaterial = water.call("get_water_material")
	for k in water_params:
		if wm: wm.set_shader_parameter(k, water_params[k])

	# Floating boxes.
	for i in 3:
		var body := RigidBody3D.new()
		body.mass = 40.0
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(0.8, 0.4, 0.8)
		cs.shape = box
		body.add_child(cs)
		var mi := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = box.size
		mi.mesh = bm
		var m := StandardMaterial3D.new()
		m.albedo_color = [Color(0.8, 0.3, 0.2), Color(0.85, 0.75, 0.3), Color(0.3, 0.5, 0.8)][i]
		mi.material_override = m
		body.add_child(mi)
		var b: Node3D = ClassDB.instantiate("PhysXBuoyancy3D")
		body.add_child(b)
		body.position = Vector3(-2.0 + 2.0 * i, 0.3, -3.0 - 2.0 * i)
		add_child(body)
		b.set("water_surface_path", b.get_path_to(water))

	var cam := Camera3D.new()
	cam.fov = 45
	add_child(cam)
	match view:
		"under":
			cam.look_at_from_position(Vector3(0, 3.5, 5.0), Vector3(0, -1.0, -1.5))
		"submerged":
			cam.look_at_from_position(Vector3(2.0, -1.4, -15.0), Vector3(-6.0, -1.0, -9.0))
		"waterline":
			cam.look_at_from_position(Vector3(3.0, 0.02, -14.0), Vector3(-6.0, -0.4, -9.0))
		"up":
			cam.look_at_from_position(Vector3(0.0, -2.5, -14.0), Vector3(-1.0, 1.5, -20.0))
		"river":
			cam.look_at_from_position(Vector3(-1.0, 9.0, 1.0), Vector3(-4.0, -1.0, -7.0))
		"wake":
			cam.look_at_from_position(Vector3(1.0, 16.0, -9.0), Vector3(1.0, 0.0, -9.01))
		"sea":
			cam.look_at_from_position(Vector3(0, 4.0, -40.0), Vector3(0, -1.0, -110.0))
		_:
			cam.look_at_from_position(Vector3(0, 4.5, 12.0), Vector3(0, -0.5, -6.0))


	cam.position.y += cam_dy
	bench_cam = cam


func _checker(a: Color, b: Color) -> ImageTexture:
	var img := Image.create(8, 8, true, Image.FORMAT_RGB8)
	for y in 8:
		for x in 8:
			img.set_pixel(x, y, a if (x / 4 + y / 4) % 2 == 0 else b)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _slab(p_size: Vector3, p_pos: Vector3, p_rot: Vector3, p_mat: Material) -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = p_size
	cs.shape = shape
	body.add_child(cs)
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = p_size
	mi.mesh = bm
	mi.material_override = p_mat
	body.add_child(mi)
	body.position = p_pos
	body.rotation = p_rot
	add_child(body)


func _tracer_spawn(p_anywhere: bool) -> Vector3:
	# Streaklines: released along lanes 0.5 m apart, as dye from a row of nozzles upstream.
	var x := tracer_rng.randf_range(-16.0, 8.0) if p_anywhere else tracer_rng.randf_range(-17.0, -16.5)
	var lane := tracer_rng.randi_range(0, 20)
	return Vector3(x, 0.0, -14.0 + 0.5 * lane + tracer_rng.randf_range(-0.04, 0.04))


func _tracers_step(p_dt: float) -> void:
	if tracers <= 0:
		return
	if tracer_mm == null:
		tracer_rng.seed = 7
		var quad := QuadMesh.new()
		quad.size = Vector2(0.12, 0.12)
		quad.orientation = PlaneMesh.FACE_Y
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.albedo_color = Color(1.0, 0.95, 0.6)
		quad.material = mat
		tracer_mm = MultiMesh.new()
		tracer_mm.transform_format = MultiMesh.TRANSFORM_3D
		tracer_mm.mesh = quad
		tracer_mm.instance_count = tracers
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = tracer_mm
		mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(mmi)
		for i in tracers:
			tracer_pos.push_back(_tracer_spawn(true))
	for i in tracers:
		var p := tracer_pos[i]
		var f: Vector3 = water.call("sample_flow", p)
		p += f * p_dt
		if p.x > 9.0 or p.z < -16.0 or p.z > -2.0 or f.length() < 0.02:
			p = _tracer_spawn(false)
		p.y = float(water.call("sample_height", p)) + 0.03
		tracer_pos[i] = p
		tracer_mm.set_instance_transform(i, Transform3D(Basis(), p))


func _process(_delta: float) -> void:
	n += 1
	if pt_from > 0 and n == pt_from:
		bench_env.pathtracing_enabled = true
	_tracers_step(1.0 / 30.0)
	if view == "sea" and n > 30:
		# Significant wave height, 4 sigma of the height at one point over time (a line through the square is
		# shorter than the swell).
		var hv := float(water.call("sample_height", Vector3(3.0, 0.0, -15.0)))
		if is_finite(hv):
			sea_series.push_back(hv)
		if n == frames - 1:
			var mean := 0.0
			for h in sea_series: mean += h
			mean /= maxi(sea_series.size(), 1)
			var v := 0.0
			for h in sea_series: v += (h - mean) * (h - mean)
			print("sea Hs=%.2f m over %d frames (cascades %d)" % [4.0 * sqrt(v / maxi(sea_series.size(), 1)), sea_series.size(), water.get("wave_cascades")])
			if out == "":
				get_tree().quit()
	if flow_out != "" and n >= frames:
		# The flow as the surface samples it, over the simulated square (40 m), 10 cm a pixel.
		var size := 400
		var vis := Image.create(size, size, false, Image.FORMAT_RGB8)
		var top := maxf(current.length() * 2.0, 0.01)
		for y in size:
			for x in size:
				var p := Vector3(-20.0 + 40.0 * (x + 0.5) / size, 0.0, -20.0 + 40.0 * (y + 0.5) / size)
				var f: Vector3 = water.call("sample_flow", p)
				var v := Vector2(f.x, f.z)
				vis.set_pixel(x, y, Color.from_hsv(fposmod(v.angle() / TAU, 1.0), 0.8, clampf(v.length() / top, 0.0, 1.0)))
		vis.save_png(flow_out)
		# The flow at every cell centre of the square (160 a side), as float pairs, for a closer look.
		var cells := PackedFloat32Array()
		for z in 160:
			for x in 160:
				var f: Vector3 = water.call("sample_flow", Vector3(-20.0 + 0.25 * (x + 0.5), 0.0, -20.0 + 0.25 * (z + 0.5)))
				cells.push_back(f.x)
				cells.push_back(f.z)
		var fa := FileAccess.open(flow_out.get_basename() + "_cells.bin", FileAccess.WRITE)
		fa.store_buffer(cells.to_byte_array())
		get_tree().quit()
		return
	if dive != 0.0 and seq > 0 and n >= frames and n < frames + seq:
		bench_cam.position.y -= dive / seq
	if out != "" and seq > 0 and n >= frames and n < frames + seq:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%04d.png" % (n - frames))
		return
	if out != "" and n >= frames + seq:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		for b in get_children():
			if b is RigidBody3D:
				print("box at ", b.global_position, " flow there ", water.call("sample_flow", b.global_position))
		get_tree().quit()

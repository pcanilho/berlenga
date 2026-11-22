extends Node3D
# PhysXDustDevils3D bench (docs/ideas/sand.md, "Dust devils").
#   --test            the weather gate and the life of a devil, headless: favour on a hot, calm, sunny noon over
#                     dry ground; none in a strong wind, in rain, or at night; a spawned devil drifts downwind and
#                     dies within half a minute once rain sets in
#   (default)         a view: a devil over a flat of dry sand at noon, after --secs of growth, saved to --out
#   --raster          raster only (default path traced); --pair saves <out>_raster.png first, then the PT frame
#   --spp=N           path tracing samples (default 16)
#   --secs=S          seconds of the devil's life before the shot (default 20)
#   --radius=R --swirl=V  the devil's ring radius (m) and swirl speed on it (m/s); default 3, 12
#   --cam=x,h,z,tx,ty,tz  the camera; default 0,3,-32 toward 0,9,0
#   --sun=alt,yaw     degrees (default 55, 150)
#   --wind=W          the 10 m wind (m/s, along +x; default 3)
#   --flow=prop:value  a property of the devils' PhysXFlow3D (tuning), repeatable
#   --cell=C          Flow cell size (m; default 0.25)
# Render: VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a godot --path . --rendering-driver vulkan --resolution 960x540 -- ...

var out := ""
var raster := false
var spp := 16
var secs := 20.0
var radius := 3.0
var swirl := 12.0
var cam_arg: Array = [0.0, 3.0, -32.0, 0.0, 9.0, 0.0]
var sun_arg: Array = [55.0, 150.0]
var wind := 3.0
var cell := 0.25
var fails := 0


func _ready() -> void:
	var test := false
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--secs="): secs = float(a.substr(7))
		elif a.begins_with("--radius="): radius = float(a.substr(9))
		elif a.begins_with("--swirl="): swirl = float(a.substr(8))
		elif a.begins_with("--cam="): cam_arg = Array(a.substr(6).split_floats(","))
		elif a.begins_with("--sun="): sun_arg = Array(a.substr(6).split_floats(","))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--cell="): cell = float(a.substr(7))
	if not ClassDB.class_exists("PhysXDustDevils3D"):
		print("PhysXDustDevils3D missing (built without flow_sdk)")
		get_tree().quit(1)
		return
	if test:
		await _run_tests()
	else:
		await _view()
	get_tree().quit(1 if fails > 0 else 0)


func _check(what: String, ok: bool, detail: String) -> void:
	print("%s %s (%s)" % ["PASS" if ok else "FAIL", what, detail])
	if not ok: fails += 1


func _weather(sun: DirectionalLight3D, w: float) -> Node3D:
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 2048.0
	m.resolution = 64
	m.wind_speed = w
	m.wind_direction = Vector2(1, 0)
	m.cloud_cover = 0.0
	m.transition_time = 0.0
	m.temperature = 32.0
	m.humidity = 0.15
	add_child(m)
	m.sun_light = m.get_path_to(sun)
	return m


func _sun(alt: float, yaw: float) -> DirectionalLight3D:
	var s := DirectionalLight3D.new()
	s.rotation_degrees = Vector3(-alt, yaw, 0.0)
	s.shadow_enabled = true
	add_child(s)
	return s


func _settle(n: int = 30) -> void:
	for i in n:
		await get_tree().physics_frame


func _run_tests() -> void:
	var sun := _sun(60.0, 150.0)
	var m := _weather(sun, 2.0)
	var dd: Node3D = ClassDB.instantiate("PhysXDustDevils3D")
	dd.max_devils = 2
	dd.frequency = 0.0 # only the devils the test spawns
	add_child(dd)
	await _settle()
	var at := Vector3.ZERO
	var heat: float = m.get_surface_temperature_at(at) - m.get_air_temperature_at(at)
	var f0: float = dd.get_favour_at(at)
	_check("a hot, calm, sunny noon favours devils", f0 > 0.3, "favour %.2f, surface %.1f K over the air" % [f0, heat])
	m.wind_speed = 12.0
	await _settle()
	_check("none in a 12 m/s wind", dd.get_favour_at(at) == 0.0, "favour %.2f" % dd.get_favour_at(at))
	m.wind_speed = 2.0
	sun.rotation_degrees = Vector3(20.0, 150.0, 0.0) # below the horizon
	await _settle()
	_check("none at night", dd.get_favour_at(at) == 0.0, "favour %.2f, heat %.1f K" % [dd.get_favour_at(at), m.get_surface_temperature_at(at) - m.get_air_temperature_at(at)])
	sun.rotation_degrees = Vector3(-60.0, 150.0, 0.0)
	m.wind_speed = 4.0
	await _settle()
	var i: int = dd.spawn(Vector3(0, 0, 0), 3.0, 10.0)
	_check("spawn places a devil", i >= 0 and dd.get_devil_count() == 1, "index %d, count %d" % [i, dd.get_devil_count()])
	var t := 0.0
	while t < 20.0:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
	var d: Vector4 = dd.get_devil(0)
	_check("it drifts downwind (+x) at about 0.7 of the wind", d.x > 20.0 and d.x < 90.0, "x %.1f m, z %.1f m after 20 s" % [d.x, d.y])
	_check("it spins up over its first 15 s", d.w > 0.9 * 10.0, "swirl now %.1f m/s" % d.w)
	# The shared wind (WeatherMap3D add_vortex): the swirl on the ring, the updraft on the axis, as every reader sees it.
	var ring := Vector3(d.x + d.z, 0.0, d.y)
	var vw: Vector3 = m.get_vortex_wind_at(ring, 2.0)
	var ax: Vector3 = m.get_vortex_wind_at(Vector3(d.x, 0.0, d.y), 6.0)
	var far: Vector3 = m.get_vortex_wind_at(Vector3(d.x + 8.0 * d.z, 0.0, d.y), 2.0)
	_check("the devil is in the shared wind: its swirl on the ring", m.get_vortex_count() == 1 and abs(vw.z) > 0.8 * d.w and abs(vw.z) < 1.05 * d.w, "%d vortices, %.1f m/s across at R" % [m.get_vortex_count(), vw.z])
	_check("an updraft on its axis, none 8 radii out", ax.y > 0.25 * d.w and far.length() < 0.01, "up %.1f m/s, %.2f m/s at 8 R" % [ax.y, far.length()])
	_check("its own swirl does not count as wind against it", dd.get_favour_at(ring) > 0.9, "favour %.2f on its ring" % dd.get_favour_at(ring))
	var total: Vector3 = m.get_wind_at(ring, 2.0)
	_check("get_wind_at carries it", abs(total.z) > 0.6 * d.w, "%s" % total)
	# Spawning from the weather: a high rate over the disc, the cap holds and they stand apart.
	dd.frequency = 1e6
	t = 0.0
	while t < 5.0:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
	var n: int = dd.get_devil_count()
	var apart := true
	if n == 2:
		var a: Vector4 = dd.get_devil(0)
		var b: Vector4 = dd.get_devil(1)
		apart = Vector2(a.x - b.x, a.y - b.y).length() > 25.0
	_check("the weather spawns them up to max_devils, apart", n >= 1 and n <= 2 and apart, "%d devils, %d spawned, favour %.2f" % [n, dd.get_spawned(), dd.get_favour()])
	dd.frequency = 0.0
	m.cloud_cover = 0.9
	m.rain_rate = 5.0
	var wait := 0
	while m.get_rain_at(at) <= 0.0 and wait < 600:
		await get_tree().physics_frame
		wait += 1
	_check("rain stops new ones", dd.get_favour_at(at) == 0.0, "favour %.2f, rain %.2f mm/h" % [dd.get_favour_at(at), m.get_rain_at(at) if m.has_method("get_rain_at") else -1.0])
	t = 0.0
	while t < 30.0 and dd.get_devil_count() > 0:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
	_check("rain kills it within 25 s", dd.get_devil_count() == 0, "gone after %.1f s" % t)
	_check("its vortex leaves the shared wind with it", m.get_vortex_count() == 0, "%d vortices" % m.get_vortex_count())
	print("%d failed" % fails)


func _view() -> void:
	var sun := _sun(sun_arg[0], sun_arg[1])
	sun.directional_shadow_max_distance = 80.0
	var m := _weather(sun, wind)
	var env := Environment.new()
	var sky := Sky.new()
	var psky := ProceduralSkyMaterial.new()
	sky.sky_material = psky
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.set("pathtracing_sky_shader_enabled", true)
	env.pathtracing_enabled = false
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	# A flat of dry sand, 400 m.
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(400, 400)
	pm.subdivide_width = 8
	pm.subdivide_depth = 8
	ground.mesh = pm
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.78, 0.64, 0.46)
	mat.roughness = 0.95
	var nt := NoiseTexture2D.new()
	nt.noise = FastNoiseLite.new()
	nt.noise.frequency = 0.02
	nt.as_normal_map = true
	nt.bump_strength = 2.0
	nt.seamless = true
	mat.normal_enabled = true
	mat.normal_texture = nt
	mat.uv1_scale = Vector3(60, 60, 1)
	ground.material_override = mat
	if "--ground-no-shadow" in OS.get_cmdline_user_args():
		ground.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(ground)
	var cam := Camera3D.new()
	cam.fov = 55.0
	add_child(cam)
	cam.current = true
	cam.look_at_from_position(Vector3(cam_arg[0], cam_arg[1], cam_arg[2]), Vector3(cam_arg[3], cam_arg[4], cam_arg[5]))
	var dd: Node3D = ClassDB.instantiate("PhysXDustDevils3D")
	dd.frequency = 0.0
	dd.flow_cell_size = cell
	add_child(dd)
	dd.follow_target = dd.get_path_to(cam)
	# --flow=prop:value sets a property of the devils' PhysXFlow3D (tuning, A/B); repeatable
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--flow="):
			var kv := a.substr(7).split(":")
			dd.get_flow().set(kv[0], str_to_var(kv[1]))
	# The weather map's first update (the surface heating) before the spawn, or the devil starts in no favour.
	await _settle(60)
	# Upwind of the centre so the shot holds it after its drift.
	dd.spawn(Vector3(-0.7 * wind * secs, 0.0, 0.0), radius, swirl)
	var t := 0.0
	var k := 0
	while t < secs:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
		k += 1
		if k % 120 == 0:
			var d: Vector4 = dd.get_devil(0)
			var fl = dd.get_flow()
			if fl and "--probe" in OS.get_cmdline_user_args():
				fl.cpu_readback = true
				var col := []
				for hh in [0.5, 2.0, 5.0, 10.0, 15.0]:
					col.append(snappedf(fl.sample_smoke(Vector3(d.x, hh, d.y)), 0.001))
				var vel := []
				for hh in [0.5, 5.0, 12.0]:
					vel.append(fl.call("sample_velocity", Vector3(d.x, hh, d.y)) if fl.has_method("sample_velocity") else null)
				print("  smoke up the axis ", col, " vel ", vel)
			print("t %.1f devil %s favour %.2f stats %s" % [t, d, dd.get_favour_at(Vector3.ZERO), fl.call("get_stats") if fl else "none"])
	if "--pair" in OS.get_cmdline_user_args() and not out.is_empty():
		var pt_out := out
		out = pt_out.get_basename() + "_raster.png"
		await _save_frame(env)
		get_tree().paused = true
		out = pt_out
		env.pathtracing_enabled = true
		await _save_frame(env)
		return
	env.pathtracing_enabled = not raster
	await _save_frame(env)


func _save_frame(env: Environment) -> void:
	for i in 12:
		await get_tree().process_frame
	var waited := 0
	while env.pathtracing_enabled and waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		await get_tree().process_frame
		waited += 1
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)

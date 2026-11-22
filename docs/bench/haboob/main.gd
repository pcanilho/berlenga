extends Node3D
# Haboob / dust storm bench (docs/ideas/sand.md, "Haboob dust walls"): WeatherMap3D's mineral dust layer.
#   --test   headless: the source law (threshold, wetness, clay), a storm's downburst outflow over a dusty map raising
#            a ring of dust piled at its gust front (head share, raised top), the profile, the sun dimmed through it,
#            the contract, and rain washing it out
# Prints a map of the column (g/m2) round the storm with --map.
# Without --test, a view of the gust front's billows (PhysXHaboobFront3D) from the ground ahead of it:
#   --out=PATH        save the frame;  --raster  raster only (default path traced);  --pair  raster then PT, one run
#   --spp=N           path tracing samples (default 16);  --secs=S  Flow time before the shot (default 20)
#   --ahead=M         camera distance ahead of the head (default 250);  --cam-height=M (default 2)
#   --emitters=N      front emitters (default 10);  --cell=M  Flow cell (default 2);  --sun=alt,yaw (default 35,200)
#   --flow=prop:value sets a property of the billows' PhysXFlow3D (repeatable)

var fails := 0
var out := ""
var raster := false
var spp := 16
var secs := 20.0
var ahead := 250.0
var cam_height := 2.0
var emitters := 10
var cell := 2.0
var sun_arg: Array = [35.0, 200.0]


func _ready() -> void:
	var test := false
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--secs="): secs = float(a.substr(7))
		elif a.begins_with("--ahead="): ahead = float(a.substr(8))
		elif a.begins_with("--cam-height="): cam_height = float(a.substr(13))
		elif a.begins_with("--emitters="): emitters = int(a.substr(11))
		elif a.begins_with("--cell="): cell = float(a.substr(7))
		elif a.begins_with("--sun="): sun_arg = Array(a.substr(6).split_floats(","))
	if test:
		await _run_tests()
	else:
		await _view()
	get_tree().quit(1 if fails > 0 else 0)


func _storm_map() -> Node3D:
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 24000.0
	m.resolution = 128
	m.wind_speed = 0.5
	m.wind_direction = Vector2(1, 0)
	m.cloud_cover = 0.0
	m.transition_time = 0.0
	m.temperature = 35.0
	m.humidity = 0.15
	m.dust_source_share = 0.8
	add_child(m)
	await get_tree().physics_frame
	m.add_storm(Vector2(0, 0), 1500.0, 0.0, 25.0, 0.0)
	for i in 30:
		m.advance(60.0)
		await get_tree().physics_frame
	return m


func _view() -> void:
	if not ClassDB.class_exists("PhysXHaboobFront3D"):
		print("PhysXHaboobFront3D missing (built without flow_sdk)")
		fails += 1
		return
	var m := await _storm_map()
	var head_at := 0.0
	var head := 0.0
	for r in range(500, 11000, 100):
		var h: float = m.get_dust_head_at(Vector3(r, 0, 0))
		if h > head:
			head = h
			head_at = r
	print("head %.2f at %.0f m" % [head, head_at])
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-sun_arg[0], sun_arg[1], 0.0)
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 600.0
	add_child(sun)
	m.sun_light = m.get_path_to(sun)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
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
	# Dry desert flat, 2 km, centred between the camera and the front.
	var x0 := head_at + ahead
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(2000, 2000)
	pm.subdivide_width = 16
	pm.subdivide_depth = 16
	ground.mesh = pm
	ground.position = Vector3(head_at, 0, 0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.74, 0.6, 0.44)
	mat.roughness = 0.95
	var nt := NoiseTexture2D.new()
	nt.noise = FastNoiseLite.new()
	nt.noise.frequency = 0.02
	nt.as_normal_map = true
	nt.bump_strength = 2.0
	nt.seamless = true
	mat.normal_enabled = true
	mat.normal_texture = nt
	mat.uv1_scale = Vector3(300, 300, 1)
	ground.material_override = mat
	add_child(ground)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.far = 8000.0
	add_child(cam)
	cam.current = true
	cam.look_at_from_position(Vector3(x0, cam_height, 0), Vector3(head_at, 60.0, 0))
	var hf: Node3D = ClassDB.instantiate("PhysXHaboobFront3D")
	hf.emitter_count = emitters
	hf.flow_cell_size = cell
	hf.near_range = maxf(300.0, ahead + 50.0)
	add_child(hf)
	hf.follow_target = hf.get_path_to(cam)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--flow="):
			var kv := a.substr(7).split(":")
			hf.get_flow().set(kv[0], str_to_var(kv[1]))
	hf.scan_now()
	print("front points %d, first %s at %s m/s" % [hf.get_front_point_count(), hf.get_front_point(0), hf.get_front_velocity(0)])
	var t := 0.0
	var k := 0
	while t < secs:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
		k += 1
		if k % 120 == 0:
			var fl = hf.get_flow()
			print("t %.1f points %d first %s stats %s" % [t, hf.get_front_point_count(), hf.get_front_point(0), fl.call("get_stats") if fl else "none"])
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


func _check(what: String, ok: bool, detail: String) -> void:
	print("%s %s (%s)" % ["PASS" if ok else "FAIL", what, detail])
	if not ok: fails += 1


func _run_tests() -> void:
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 24000.0
	m.resolution = 128
	m.wind_speed = 0.5 # a slow storm: it stays over the scan
	m.wind_direction = Vector2(1, 0)
	m.cloud_cover = 0.0
	m.transition_time = 0.0
	m.temperature = 35.0
	m.humidity = 0.15
	m.dust_source_share = 0.8
	add_child(m)
	await get_tree().physics_frame

	# The source law.
	var calm: float = m.get_dust_flux_for(5.0, 1.0, 0.0)
	var gale: float = m.get_dust_flux_for(20.0, 1.0, 0.0)
	var storm: float = m.get_dust_flux_for(30.0, 1.0, 0.0)
	_check("no dust under the threshold, dust in a gale, much more in a storm", calm == 0.0 and gale > 0.0 and storm > 2.0 * gale, "5 m/s %.5f, 20 m/s %.5f, 30 m/s %.5f g/m2/s" % [calm, gale, storm])
	_check("a wet surface holds it", m.get_dust_flux_for(20.0, 1.0, 0.6) == 0.0 and m.get_dust_flux_for(20.0, 1.0, 0.2) < gale, "%.5f damp, %.5f wet" % [m.get_dust_flux_for(20.0, 1.0, 0.2), m.get_dust_flux_for(20.0, 1.0, 0.6)])
	m.dust_clay = 10.0
	var clayey: float = m.get_dust_flux_for(20.0, 1.0, 0.0)
	m.dust_clay = 8.0
	_check("more clay, more dust per grain (Marticorena and Bergametti)", clayey > 1.5 * gale, "clay 10 pct %.5f, 8 pct %.5f" % [clayey, gale])

	# A storm's outflow over the dusty map: no rain, so its dust stays.
	var id: int = m.add_storm(Vector2(0, 0), 1500.0, 0.0, 25.0, 0.0)
	var t := 0.0
	for i in 30:
		var t0 := Time.get_ticks_msec()
		m.advance(60.0)
		if i % 10 == 0:
			print("  minute %d: %d ms, peak 3 km %.2f" % [i + 1, Time.get_ticks_msec() - t0, m.get_dust_at(Vector3(3000, 0, 0))])
		await get_tree().physics_frame
	var ring := 0.0
	var ring_at := 0.0
	for r in range(500, 9000, 250):
		var c: float = m.get_dust_at(Vector3(r, 0, 0))
		if c > ring:
			ring = c
			ring_at = r
	var far: float = m.get_dust_at(Vector3(11500, 0, 0)) if m.get_dust_at(Vector3(11500, 0, 0)) else 0.0
	var core: float = m.get_dust_at(Vector3(0, 0, 0))
	_check("the outflow raises a dust ring round the storm", ring > 1.0 and ring_at > 1500.0, "peak %.2f g/m2 at %.0f m, %.2f under the core" % [ring, ring_at, core])
	if "--map" in OS.get_cmdline_user_args():
		for z in range(-8000, 8001, 1000):
			var row := ""
			for x in range(-8000, 8001, 1000):
				row += "%5.1f" % m.get_dust_at(Vector3(x, 0, z))
			print(row)
	var head := 0.0
	var head_at := 0.0
	for r in range(500, 11000, 250):
		var h: float = m.get_dust_head_at(Vector3(r, 0, 0))
		if h > head:
			head = h
			head_at = r
	_check("its leading edge is marked, outside the peak", head > 0.5 and head_at >= ring_at, "head %.2f at %.0f m" % [head, head_at])
	var top_head: float = m.get_dust_top_at(Vector3(head_at, 0, 0))
	_check("the nose stands higher than the layer", top_head > 1.3 * m.dust_layer_depth, "top %.0f m at the head, depth %.0f m" % [top_head, m.dust_layer_depth])
	var p := Vector3(ring_at, 0, 0)
	var c100: float = m.get_dust_concentration_at(p + Vector3(0, 100, 0))
	var ctop: float = m.get_dust_concentration_at(p + Vector3(0, m.get_dust_top_at(p) + 10.0, 0))
	var c1000: float = m.get_dust_concentration_at(p + Vector3(0, 1000, 0))
	_check("densest at the ground, thinning up, none above the top", c100 > 0.0 and c1000 < 0.5 * c100 and ctop == 0.0, "%.2f mg/m3 at 100 m, %.3f above the top; visibility %.0f m" % [c100 * 1000.0, ctop, 3.9 / maxf(0.6 * c100, 1e-9)])
	var tr: Vector3 = m.get_smoke_transmittance(p, Vector3(0.3, 0.6, 0.0))
	_check("the sun dims through it, a little more in the blue", tr.y < 0.7 and tr.z < tr.y and tr.x > tr.y, "transmittance %s" % tr)
	# Valence's review: a wall coming in under a low sun dims it before it is overhead; dust_storms gates it, not smoke_haze.
	var clear_ahead := Vector3(ring_at + 1500.0, 0, 0)
	var low_toward: Vector3 = m.get_smoke_transmittance(clear_ahead, Vector3(-1.0, 0.5, 0.0))
	var low_away: Vector3 = m.get_smoke_transmittance(clear_ahead, Vector3(1.0, 0.5, 0.0))
	_check("a low sun behind the coming wall dims first", low_toward.y < 0.8 * low_away.y, "toward the wall %.3f, away %.3f, dust overhead %.2f g/m2" % [low_toward.y, low_away.y, m.get_dust_at(clear_ahead)])
	m.smoke_haze = false
	var no_smoke: Vector3 = m.get_smoke_transmittance(p, Vector3(0.3, 0.6, 0.0))
	m.smoke_haze = true
	_check("smoke_haze off leaves the dust dimming", absf(no_smoke.y - tr.y) < 1e-4, "%.4f vs %.4f" % [no_smoke.y, tr.y])
	var ww = WorldContracts.get_weather()
	_check("the contract carries it", absf(ww.dust_at(p) - m.get_dust_at(p)) < 1e-4 and ww.dust_top_at(p) > 0.0 and ww.dust_concentration_at(p + Vector3(0, 100, 0)) > 0.0, "dust_at %.2f, top %.0f" % [ww.dust_at(p), ww.dust_top_at(p)])
	# The near field: Flow billows ride the head where it passes the camera.
	if ClassDB.class_exists("PhysXHaboobFront3D"):
		var cam := Node3D.new()
		add_child(cam)
		cam.global_position = Vector3(head_at - 150.0, 0, 40.0)
		var hf = ClassDB.instantiate("PhysXHaboobFront3D")
		add_child(hf)
		hf.follow_target = hf.get_path_to(cam)
		hf.scan_now()
		var n: int = hf.get_front_point_count()
		var on_head := 0
		var outward := 0
		for i in n:
			var fp: Vector3 = hf.get_front_point(i)
			if m.get_dust_head_at(fp) > 0.3:
				on_head += 1
			var v: Vector3 = hf.get_front_velocity(i)
			if v.dot(Vector3(fp.x, 0, fp.z).normalized()) > 0.5 * v.length():
				outward += 1
		_check("billows sit on the head and travel outward", n > 0 and on_head >= n * 0.8 and outward >= n * 0.8, "%d points, %d on the head, %d outward, first %s at %s m/s" % [n, on_head, outward, hf.get_front_point(0) if n > 0 else Vector3(), hf.get_front_velocity(0) if n > 0 else Vector3()])
		hf.queue_free()
		cam.queue_free()
	# Rain over it: washed out as a coarse aerosol.
	m.remove_storm(id)
	var before: float = m.get_dust_at(p)
	m.rain_rate = 10.0
	m.cloud_cover = 1.0
	for i in 20:
		m.advance(60.0)
		if i % 10 == 0: print("  rain minute %d" % i)
		await get_tree().physics_frame
	var after: float = m.get_dust_at(p)
	_check("rain washes it out", after < 0.5 * before, "%.2f before, %.2f after 20 min of 10 mm/h" % [before, after])
	m.dust_storms = false
	_check("dust_storms off clears it", m.get_dust_at(p) == 0.0, "%.2f" % m.get_dust_at(p))
	print("%d failed" % fails)

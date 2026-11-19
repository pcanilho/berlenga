extends Node3D
# Rain bench (docs/ideas/rain.md): the rain's systems working together, raster or path traced.
#
#   --test            print the checks and quit:
#                       rain and snow split with height (lapse rate, Jennings et al. 2018)
#                       Rutter canopy store: fills in rain, drips, stays wet after the rain, dries
#                       world runoff on a valley: water reaches the outlet, the channel gathers the area
#                       runoff load (WorldGround.add_runoff_load): carried down the valley into a lake stub, mass kept
#                       falling rain: drops in the box, splashes, haze from the drop sizes
#   --view=pond       a pool in 10 mm/h rain under a wet tree and a shed: rings, streaks, splashes, drips,
#                     beaded leaves (--close: down on the water and leaves for a 100% crop)
#   --rate=R          rain rate mm/h (default 10)
#   --drops=N         most falling drops drawn (default RainFall3D's 200000; fewer for a quick path traced still)
#   --raster          raster only (default path traced)
#   --out=path.png    save the frame and quit
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)

const Valley := preload("res://valley.gd")
const LakeStub := preload("res://lake_stub.gd")

var view := "pond"
var raster := false
var test := false
var close := false
var out := ""
var spp := 16
var bounces := 3
var rate := 10.0
var drops := 0
var fails := 0
var weather: WeatherMap3D


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a == "--close": close = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--rate="): rate = float(a.substr(7))
		elif a.begins_with("--drops="): drops = int(a.substr(8))
	if test:
		await _run_tests()
		print("RAIN CHECKS: ", "PASS" if fails == 0 else "%d FAILED" % fails)
		get_tree().quit()
		return
	await _build_pond()


func _check(label: String, ok: bool, detail: String) -> void:
	print(("  ok   " if ok else "  FAIL ") + label + ": " + detail)
	if not ok:
		fails += 1


func _weather(temperature: float, r: float) -> WeatherMap3D:
	var w := WeatherMap3D.new()
	w.extent = 4096.0
	w.resolution = 32
	w.temperature = temperature
	w.humidity = 0.9
	w.cloud_cover = 1.0 if r > 0.0 else 0.0
	w.cloud_variation = 0.0
	w.wind_speed = 3.0
	w.rain_rate = r
	w.set_physics_process_internal(false)
	add_child(w)
	w.snap_to_settings()
	w.advance(1.0)
	return w


func _set_rain(w: WeatherMap3D, r: float) -> void:
	w.rain_rate = r
	w.cloud_cover = 1.0 if r > 0.0 else 0.0
	w.snap_to_settings()


func _run_tests() -> void:
	var p := Vector3.ZERO
	# Phase with height: 2 C at the ground, 6.5 K/km.
	var w := _weather(2.0, 5.0)
	var low := w.get_rain_share_at(Vector3(0, w.get_ground_height(), 0))
	var high := w.get_rain_share_at(Vector3(0, w.get_ground_height() + 600.0, 0))
	_check("rain share falls with height", low > high + 0.1, "%.2f at the ground, %.2f 600 m up (%.1f C)" % [low, high, w.get_air_temperature_at(Vector3(0, w.get_ground_height() + 600.0, 0))])
	_check("rainfall + snowfall = precipitation", absf(w.get_rainfall_at(p) + w.get_snowfall_at(p) - w.get_rain_at(p)) < 0.01 * maxf(w.get_rain_at(p), 1.0), "%.2f + %.2f of %.2f mm/h" % [w.get_rainfall_at(p), w.get_snowfall_at(p), w.get_rain_at(p)])
	w.queue_free()

	# Rutter canopy: an hour of 5 mm/h rain, then dry weather.
	w = _weather(12.0, 5.0)
	w.advance(3600.0)
	var held := w.get_canopy_water_at(p)
	var wet := w.get_canopy_wetness_at(p)
	var drip := w.get_canopy_drip_at(p)
	_check("canopy fills in rain", wet > 0.9 and held > 0.8, "%.2f mm held, wetness %.2f, drip %.2f mm/h" % [held, wet, drip])
	_check("canopy drips near the rain's rate when full", drip > 0.5 * 5.0 * 0.75 and drip < 5.0, "%.2f mm/h" % drip)
	_set_rain(w, 0.0)
	w.advance(600.0)
	var wet10 := w.get_canopy_wetness_at(p)
	var drip10 := w.get_canopy_drip_at(p)
	_check("leaves still wet 10 min after the rain", wet10 > 0.5, "wetness %.2f, drip %.3f mm/h" % [wet10, drip10])
	w.advance(6.0 * 3600.0)
	var wet6 := w.get_canopy_wetness_at(p)
	_check("leaves dry over hours", wet6 < wet10, "wetness %.2f after 6 h" % wet6)
	w.queue_free()

	# Runoff on a 4 km valley: 6 h of 20 mm/h.
	w = _weather(12.0, 20.0)
	var ground := Valley.new()
	ground.name = "Valley"
	add_child(ground)
	var ro := WeatherRunoff3D.new()
	ro.extent = 4096.0
	ro.resolution = 128
	ro.bake_on_ready = false
	add_child(ro)
	ro.height_source = ro.get_path_to(ground)
	ro.bake()
	_check("runoff baked", ro.is_baked(), "%d ms" % ro.get_bake_time_msec())
	for h in 6:
		w.advance(3600.0)
		ro.advance(3600.0)
	var outlet := Vector3(0, 0, -1900.0)
	var side := Vector3(800.0, 0, -1900.0)
	var q_out := ro.get_max_discharge_in(Rect2(-80, -2048, 160, 300))
	var a_out := ro.get_upstream_area_at(outlet)
	_check("the valley floor gathers the area", a_out > 20.0 * ro.get_upstream_area_at(side), "%.2f km2 at the outlet, %.4f km2 on the side" % [a_out * 1e-6, ro.get_upstream_area_at(side) * 1e-6])
	_check("water reaches the outlet", q_out > 1.0, "%.1f m3/s, %.0f m3 held" % [q_out, ro.get_total_storage()])
	var depth := ro.get_depth_at(outlet)
	_check("runoff wets the valley floor", depth > 0.0, "%.2f mm over the outlet cell" % depth)
	var q_peak := q_out
	_set_rain(w, 0.0)
	for h in 12:
		w.advance(3600.0)
		ro.advance(3600.0)
	q_out = ro.get_max_discharge_in(Rect2(-80, -2048, 160, 300))
	_check("the flood recedes after the rain", q_out < q_peak, "%.1f m3/s 12 h later" % q_out)
	# Load on the runoff (the contract call PhysX and the ground make): 100 kg of ash and 400 kg of silt on a burnt
	# slope high in the valley, carried down the drainage in a storm into a lake at the outlet.
	var lake: WorldWater = LakeStub.new()
	WorldContracts.set_water(lake)
	_check("runoff answers the ground's load call", WorldContracts.get_ground().has_runoff(), "")
	WorldContracts.get_ground().add_runoff_load(Vector3(300.0, 0.0, 1500.0), 400.0, 100.0)
	var on_map: Vector2 = ro.get_total_load()
	_check("the load lands on the runoff", on_map.is_equal_approx(Vector2(400.0, 100.0)), "%s kg" % on_map)
	_set_rain(w, 20.0)
	for h in 12:
		w.advance(3600.0)
		ro.advance(3600.0)
	var got: Vector2 = ro.get_delivered_load()
	var left: Vector2 = ro.get_total_load()
	_check("the load reaches the lake", got.x > 200.0 and got.y > 50.0 and lake.calls > 0, "%s kg delivered, %s kg still on the land" % [got, left])
	_check("no load is made or lost on the way", (got + left).is_equal_approx(Vector2(400.0, 100.0)) or (got + left - Vector2(400.0, 100.0)).length() < 0.5, "%s kg" % (got + left))
	WorldContracts.clear_overrides()
	ro.queue_free()
	ground.queue_free()
	w.queue_free()

	# Falling rain: the particle box and the haze.
	w = _weather(12.0, 10.0)
	weather = w
	var fall := RainFall3D.new()
	add_child(fall)
	for i in 30:
		await get_tree().process_frame
	_check("rain falls at the weather's rate", absf(fall.get_rainfall_rate() - w.get_rainfall_at(fall.global_position)) < 0.5, "%.2f mm/h, %d drops, weight %.1f" % [fall.get_rainfall_rate(), fall.get_visible_drops(), fall.get_drop_weight()])
	_check("haze from the drops (Atlas 1953: about 1.5 per km at 10 mm/h)", fall.get_extinction() > 0.0008 and fall.get_extinction() < 0.003, "%.2f per km, visibility %.0f m" % [fall.get_extinction() * 1000.0, fall.get_visibility()])
	fall.queue_free()
	w.queue_free()


func _mat(c: Color, rough: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = rough
	m.set_feature(BaseMaterial3D.FEATURE_WETNESS, true)
	return m


func _box(size: Vector3, pos: Vector3, m: Material, collide := true) -> void:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	mi.mesh = bm
	mi.material_override = m
	mi.position = pos
	add_child(mi)
	if collide:
		var body := StaticBody3D.new()
		var cs := CollisionShape3D.new()
		var sh := BoxShape3D.new()
		sh.size = size
		cs.shape = sh
		body.add_child(cs)
		body.position = pos
		add_child(body)


func _build_pond() -> void:
	weather = _weather(12.0, rate)
	weather.advance(1800.0) # half an hour of rain: the canopy full, the ground wet
	var env := Environment.new()
	var sky := Sky.new()
	var sm := ProceduralSkyMaterial.new()
	sm.sky_top_color = Color(0.42, 0.45, 0.5)
	sm.sky_horizon_color = Color(0.6, 0.62, 0.65)
	sky.sky_material = sm
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, 30, 0)
	sun.light_energy = 0.4 # overcast
	sun.shadow_enabled = true
	add_child(sun)

	# Ground with a sunken pool, a shed and a tree.
	var soil := _mat(Color(0.32, 0.27, 0.2), 0.9)
	soil.wetness_porosity = 0.7
	_box(Vector3(40, 1, 14), Vector3(0, -0.5, -13), soil)
	_box(Vector3(40, 1, 14), Vector3(0, -0.5, 13), soil)
	_box(Vector3(14, 1, 12), Vector3(-13, -0.5, 0), soil)
	_box(Vector3(14, 1, 12), Vector3(13, -0.5, 0), soil)
	_box(Vector3(12, 0.2, 12), Vector3(0, -1.1, 0), _mat(Color(0.2, 0.18, 0.15), 0.9))
	var wood := _mat(Color(0.35, 0.25, 0.17), 0.8)
	_box(Vector3(3.6, 0.12, 3.0), Vector3(-7.5, 2.45, -4.5), wood)
	_box(Vector3(3.3, 2.4, 0.1), Vector3(-7.5, 1.2, -5.9), wood)
	if ClassDB.class_exists("PhysXWaterSurface3D"):
		var water: Node3D = ClassDB.instantiate("PhysXWaterSurface3D")
		water.set("domain_size", Vector2(12, 12))
		water.set("grid_resolution", 96)
		water.set("depth", 0.9)
		water.set("wind_speed", 1.0)
		water.set("render_extent", 12.0)
		water.position = Vector3(0, -0.15, 0)
		add_child(water)
	if ClassDB.class_exists("TreeGenerator"):
		var g = ClassDB.instantiate("TreeGenerator")
		g.set("preset", 2)
		g.set("seed", 3)
		var mesh: Mesh = g.call("generate")
		var tree := MeshInstance3D.new()
		tree.mesh = mesh
		tree.position = Vector3(5.0, 0.0, -3.5)
		add_child(tree)
	var probe := Node3D.new()
	probe.name = "Probe"
	add_child(probe)
	var fall := RainFall3D.new()
	fall.follow_target = NodePath("../Probe")
	if drops > 0:
		fall.max_drops = drops
	add_child(fall)
	var cam := Camera3D.new()
	add_child(cam)
	if close:
		cam.position = Vector3(1.5, 0.9, 3.0)
		cam.look_at(Vector3(2.5, -0.1, -1.5))
		cam.fov = 35.0
	else:
		cam.position = Vector3(-1.0, 2.2, 9.0)
		cam.look_at(Vector3(0.5, 0.6, -3.0))
		cam.fov = 60.0
	probe.global_position = cam.global_position
	if not raster:
		env.pathtracing_enabled = false
	for i in 120:
		await get_tree().process_frame
	print("rain %.2f mm/h, %d drops, %d drip sources (%.2f drips/s), canopy wetness %.2f, haze %.2f per km" % [fall.get_rainfall_rate(), fall.get_visible_drops(), fall.get_drip_source_count(), fall.get_drip_rate(), weather.get_canopy_wetness_at(Vector3.ZERO), fall.get_extinction() * 1000.0])
	if not raster:
		env.pathtracing_enabled = true
		# Wait for every path traced surface (animated vegetation needs about 50 frames).
		for i in 600:
			await get_tree().process_frame
			if i > 60 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) == 0:
				break
	for i in 12:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

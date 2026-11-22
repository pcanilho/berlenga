extends Node3D
# Rain bench (docs/ideas/rain.md): the rain's systems working together, raster or path traced.
#
#   --test            print the checks and quit:
#                       rain and snow split with height (lapse rate, Jennings et al. 2018)
#                       Rutter canopy store: fills in rain, drips, stays wet after the rain, dries
#                       world runoff on a valley: water reaches the outlet, the channel gathers the area
#                       runoff load (WorldGround.add_runoff_load): carried down the valley into a lake stub, mass kept
#                       sediment in a river (r / Q downstream of a source, carried at the river's speed) and a pool
#                       (well mixed through its volume; mineral settles faster than ash)
#                       post-fire debris flows: a burnt steep valley in a downpour fires one event per basin
#                       (USGS M1 likelihood, Gartner volume) with a path; none unburnt, none again within a
#                       day, none on ground burnt four years ago
#                       falling rain: drops in the box, splashes, haze from the drop sizes
#                       high-water marks: a river's annual flood stage (depth ~ Q^0.40), a lake's by its weir, the contract,
#                       the sea's strandline (Stockdon 2006 run-up)
#   --view=pond       a pool in 10 mm/h rain under a wet tree and a shed: rings, streaks, splashes, drips,
#                     beaded leaves (--close: down on the water and leaves for a 100% crop)
#   --rate=R          rain rate mm/h (default 10)
#   --temp=C          the pond's air (default 12); --warm-nose=C a warm layer aloft (freezing rain under 0 C air:
#                     the rings stay, snow would stop them)
#   --drops=N         most falling drops drawn (default RainFall3D's 200000; fewer for a quick path traced still)
#   --raster          raster only (default path traced)
#   --out=path.png    save the frame and quit
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --pt-wait=N       most frames to wait for the path traced surfaces (default 600; fewer for a quick cloud still)

const Valley := preload("res://valley.gd")
const LakeStub := preload("res://lake_stub.gd")

var view := "pond"
var raster := false
var test := false
var close := false
var out := ""
var spp := 16
var pt_wait := 600
var bounces := 3
var rate := 10.0
var drops := 0
var pond_temp := 12.0
var warm_nose := 0.0
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
		elif a.begins_with("--pt-wait="): pt_wait = int(a.substr(10))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--rate="): rate = float(a.substr(7))
		elif a.begins_with("--drops="): drops = int(a.substr(8))
		elif a.begins_with("--temp="): pond_temp = float(a.substr(7))
		elif a.begins_with("--warm-nose="): warm_nose = float(a.substr(12))
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

	await _sediment_checks()
	_debris_checks()

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
	weather = _weather(pond_temp, rate)
	weather.warm_nose_temperature = warm_nose
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
		for i in pt_wait:
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


# Sediment in rivers and pools (WorldWater.add_sediment, water_sediment.h).
class FastWeather extends WorldWeather:
	func _time_scale() -> float:
		return 86400.0


func _sediment_checks() -> void:
	# A straight river 120 m long on flat ground, 5 m3/s; 2 kg/s of silt enters 10 m from its head.
	var ground := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(400, 2, 400)
	shape.shape = box
	shape.position = Vector3(0, -1, 0)
	ground.add_child(shape)
	add_child(ground)
	var r := PhysXRiver3D.new()
	var c := Curve3D.new()
	for k in 7:
		c.add_point(Vector3(0, 0.0 - 0.1 * k, -60.0 + 20.0 * k))
	r.curve = c
	r.discharge = 5.0
	r.step_length = 2.0
	r.rain_response = false
	add_child(r)
	for i in 5:
		await get_tree().physics_frame
	r.rebuild()
	var head := r.to_global(c.sample_baked(10.0))
	var tail := r.to_global(c.sample_baked(100.0))
	_check("river answers the water contract at its head", r.sample_depth(head) > 0.0, "%.2f m deep" % r.sample_depth(head))
	await _high_water_checks(r, head)
	var speed: float = r.sample_flow(tail).length()
	# Long enough for the load to run 90 m at the river's speed (one and a half passes), the clock run 8 times fast.
	Engine.time_scale = 8.0
	var frames := int(clampf(1.5 * 90.0 / maxf(speed, 0.05) * 60.0 / 8.0, 60.0, 12000.0))
	for i in frames:
		r.add_sediment(head, 2.0, 0.0)
		await get_tree().physics_frame
	var sed: Vector2 = r.get_sediment_at(tail)
	var want := 2.0 / 5.0
	_check("silt into a river: r / Q downstream", absf(sed.x - want) < 0.15 * want and sed.y == 0.0, "%.3f kg/m3 at 90 m (want %.3f), river %.2f m/s, %d frames" % [sed.x, want, speed, frames])
	var up: Vector2 = r.get_sediment_at(r.to_global(c.sample_baked(2.0)))
	_check("none upstream of the source", up.x < 0.01 * want, "%.4f kg/m3 above it" % up.x)
	# The source stops: the plume runs off the river's end.
	for i in frames:
		await get_tree().physics_frame
	sed = r.get_sediment_at(tail)
	_check("the plume passes when the source stops", sed.x < 0.05 * want, "%.4f kg/m3 left" % sed.x)
	Engine.time_scale = 1.0
	r.queue_free()

	# A pool 20 m square, 2 m deep (800 m3): 4 kg/s for one second of weather time is 5 g/m3.
	var pool := PhysXWaterSurface3D.new()
	pool.domain_size = Vector2(20, 20)
	pool.depth = 2.0
	pool.seabed_from_floor = false
	pool.position = Vector3(0, 0.5, 0)
	add_child(pool)
	await get_tree().physics_frame
	var vol: float = pool.get_water_volume()
	_check("pool volume from its depths", absf(vol - 800.0) < 1.0, "%.1f m3" % vol)
	for i in 60:
		pool.add_sediment(Vector3(0, 0, 0), 4.0, 4.0)
		await get_tree().physics_frame
	await get_tree().physics_frame
	var ps: Vector2 = pool.get_sediment()
	_check("load into a pool mixes through its volume", absf(ps.x - 4.0 / 800.0) < 0.1 * 4.0 / 800.0, "%.5f kg/m3 mineral, %.5f ash (want %.5f)" % [ps.x, ps.y, 4.0 / 800.0])
	var ww := WorldContracts.get_water()
	var through := ww.sediment_at(Vector3(0, 0, 0))
	_check("the water contract reads the pool's load", through.is_equal_approx(ps), "%s" % through)
	# A day a second: mineral settles in about half a day, ash in about two (the source's hold over first).
	for i in 150:
		await get_tree().physics_frame
	# A day a second: mineral settles in about half a day, ash in about two.
	pool.set_sediment(Vector2(0.1, 0.1))
	WorldContracts.set_weather(FastWeather.new())
	for i in 60:
		await get_tree().physics_frame
	WorldContracts.clear_overrides()
	ps = pool.get_sediment()
	_check("a still pool clears, silt faster than ash", ps.x < 0.1 * exp(-1.6) and ps.y > 0.1 * exp(-0.65) and ps.y < 0.1 * exp(-0.4), "after a day: %.4f mineral, %.4f ash kg/m3" % [ps.x, ps.y])
	pool.queue_free()
	ground.queue_free()



# Post-fire debris flows (WeatherRunoff3D, docs/ideas/rain.md "Mud after fire").
class SteepValley extends Node3D:
	# A V valley with 35% sides falling 8% toward -z.
	func get_height_at(p: Vector3) -> float:
		return 0.35 * absf(p.x) + 0.08 * p.z


class FireStub extends WorldFire:
	# Burnt at 0.8 above z = 0 (the valley's upper half), age set by the test.
	var age := 0.5

	func _burnt_at(p: Vector3) -> float:
		return 0.8 if p.z > 0.0 else 0.0

	func _burn_age_at(p: Vector3) -> float:
		return age if p.z > 0.0 else -1.0


var df_events: Array = []
var df_ground_events := 0


# High-water marks (water.md "High-water marks"): the median annual flood's stage by at-a-station hydraulic geometry
# (depth ~ Q^0.40, Leopold and Maddock 1953) on the river, by the weir law on a lake; the contract hands them on.
func _high_water_checks(r: PhysXRiver3D, head: Vector3) -> void:
	var hw: Vector4 = r.get_high_water_at(head)
	var surf: float = r.sample_height(head)
	var d: float = r.sample_depth(head)
	var want: float = d * (pow(r.flood_peak_ratio, 0.4) - 1.0)
	var doy: int = WorldContracts.get_weather().day_of_year()
	var age: int = posmod(doy - r.flood_peak_day, 365)
	_check("river flood mark: annual flood stage by Q^0.40", absf(hw.x - surf - want) < 0.15 * want + 0.02 and is_equal_approx(hw.y, float(age)),
			"%.2f m over the %.2f m deep water (want %.2f), %d days since its peak (want %d)" % [hw.x - surf, d, want, int(hw.y), age])
	_check("river flood mark: flow direction downstream", Vector2(hw.z, hw.w).distance_to(Vector2(0, 1)) < 0.05, "(%.2f, %.2f)" % [hw.z, hw.w])
	var side := head + Vector3(150.0, 0.0, 0.0)
	_check("river flood mark: the contract hands it on, none off the floodplain",
			WorldContracts.get_water().high_water_at(head).is_equal_approx(hw) and is_inf(r.get_high_water_at(side).x),
			"%s, off: %s" % [WorldContracts.get_water().high_water_at(head), r.get_high_water_at(side)])
	# A lake 40 m across draining over a 4 m sill at y = 0, fed 2 m3/s.
	var lake := PhysXWaterSurface3D.new()
	lake.domain_size = Vector2(40, 40)
	lake.position = Vector3(150.0, 0.0, 0.0)
	add_child(lake)
	lake.set_outflow(1234, Vector3(170.0, 0.0, 0.0), Vector3(1, 0, 0), 4.0, 0.0, 1.0)
	lake.catchment_inflow = 2.0
	lake.level_from_flows = true
	await get_tree().physics_frame
	var lh: Vector4 = lake.get_high_water_at(Vector3(150.0, 0.0, 5.0))
	var h_now: float = lake.global_position.y + lake.water_level
	var ratio: float = lh.x / maxf(h_now, 1e-4)
	_check("lake flood mark: the weir passes the annual flood (head x ratio^(2/3))", absf(ratio - pow(lake.flood_peak_ratio, 2.0 / 3.0)) < 0.05 and is_inf(lake.get_high_water_at(Vector3(150.0, 0.0, 40.0)).x),
			"head %.3f m now, %.3f m at the mark (x%.2f, want x%.2f)" % [h_now, lh.x, ratio, pow(lake.flood_peak_ratio, 2.0 / 3.0)])
	lake.queue_free()
	# The sea's strandline: Stockdon et al. (2006) run-up, R2 = 1.63 m for 2 m, 10 s waves on a 1 in 10 beach.
	var r2: float = PhysXWaterSurface3D.wave_runup(2.0, 10.0, 0.1)
	_check("sea strandline: Stockdon 2006 run-up", absf(r2 - 1.634) < 0.01 and PhysXWaterSurface3D.wave_runup(2.0, 10.0, 0.01) < r2, "%.3f m (want 1.634), %.3f m on a 1 in 100 beach" % [r2, PhysXWaterSurface3D.wave_runup(2.0, 10.0, 0.01)])
	var sea := PhysXWaterSurface3D.new()
	sea.domain_size = Vector2(40, 40)
	sea.position = Vector3(-150.0, 0.0, 0.0)
	add_child(sea)
	for i in 3:
		await get_tree().physics_frame
	var sh: Vector4 = sea.get_high_water_at(Vector3(-150.0, 0.0, 10.0))
	var top: float = PhysXWaterSurface3D.wave_runup(sea.shore_wave_height * sea.storm_wave_ratio, sea.shore_wave_period * sqrt(sea.storm_wave_ratio), 0.1)
	var sea_age: int = posmod(WorldContracts.get_weather().day_of_year() - sea.storm_day, 365)
	var rise: float = sh.x - (sea.global_position.y + sea.water_level)
	_check("sea strandline: the year's storm run-up over the still level", rise > 0.05 and rise <= top + 0.01 and is_equal_approx(sh.y, float(sea_age)),
			"%.2f m over the sea (at most %.2f), %d days since the storm (want %d)" % [rise, top, int(sh.y), sea_age])
	sea.queue_free()


func _on_debris_flow(ev: Dictionary) -> void:
	df_events.append(ev)


func _on_ground_debris_flow(_ev: Dictionary) -> void:
	df_ground_events += 1


func _storm(w: WeatherMap3D, ro: WeatherRunoff3D, rate_mm_h: float, hours: int) -> void:
	_set_rain(w, rate_mm_h)
	for h in hours * 4:
		w.advance(900.0)
		ro.advance(900.0)


func _debris_checks() -> void:
	var w := _weather(12.0, 0.0)
	var ground := SteepValley.new()
	add_child(ground)
	var ro := WeatherRunoff3D.new()
	ro.extent = 2048.0
	ro.resolution = 64
	ro.bake_on_ready = false
	add_child(ro)
	ro.height_source = ro.get_path_to(ground)
	ro.bake()
	ro.debris_flow.connect(_on_debris_flow)
	WorldContracts.get_ground().debris_flow.connect(_on_ground_debris_flow)
	_check("debris flow likelihood (Staley et al. 2017 M1)", absf(WeatherRunoff3D.debris_flow_likelihood(10.0, 0.5, 0.5, 0.3) - 1.0 / (1.0 + exp(3.63 - 10.0 * (0.205 + 0.335 + 0.21)))) < 1e-4, "%.3f at 10 mm in 15 min" % WeatherRunoff3D.debris_flow_likelihood(10.0, 0.5, 0.5, 0.3))
	# Unburnt: a downpour makes floods, not debris flows.
	_storm(w, ro, 40.0, 2)
	_check("no debris flows off unburnt ground", df_events.is_empty(), "%d events" % df_events.size())
	var fire := FireStub.new()
	WorldContracts.set_fire(fire)
	_storm(w, ro, 2.0, 2)
	_check("no debris flows in light rain on a fresh burn", df_events.is_empty(), "%d events at 2 mm/h" % df_events.size())
	_storm(w, ro, 40.0, 1)
	var n_first := df_events.size()
	var big: Dictionary = {}
	var ids_rise := true
	var keys_ok := true
	for k in df_events.size():
		var ev: Dictionary = df_events[k]
		if k > 0 and int(ev["id"]) <= int(df_events[k - 1]["id"]):
			ids_rise = false
		var path: PackedVector3Array = ev["path"]
		keys_ok = keys_ok and path.size() > 0 and (ev["widths"] as PackedFloat32Array).size() == path.size() and (ev["severity"] as PackedFloat32Array).size() == path.size() and float(ev["volume"]) > 0.0 and float(ev["probability"]) >= ro.debris_flow_threshold
		if big.is_empty() or path.size() > (big["path"] as PackedVector3Array).size():
			big = ev
	_check("a downpour on a fresh burn sets off debris flows", n_first > 0 and df_ground_events == n_first, "%d events, %d heard on the ground contract" % [n_first, df_ground_events])
	_check("each event carries its id, path, widths and burn", ids_rise and keys_ok, "")
	if not big.is_empty():
		var path: PackedVector3Array = big["path"]
		var widths: PackedFloat32Array = big["widths"]
		var sev: PackedFloat32Array = big["severity"]
		_check("the longest runs down the valley and widens", path[path.size() - 1].y < path[0].y and widths[widths.size() - 1] >= widths[0], "%.2f km2 basin, %.0f m3, p %.2f, %d points from %.0f m to %.0f m, %.1f to %.1f m wide, burn %.1f to %.1f" % [big["basin_area"], big["volume"], big["probability"], path.size(), path[0].y, path[path.size() - 1].y, widths[0], widths[widths.size() - 1], sev[0], sev[sev.size() - 1]])
		_check("a debris flow's volume (Gartner et al. 2014)", float(big["volume"]) > 100.0 and float(big["volume"]) < 1e7, "%.0f m3 off %.0f m of relief" % [big["volume"], big["relief"]])
	var since: Array = ro.get_debris_flows_since(int(df_events[n_first - 3]["id"]) if n_first > 2 else -1)
	_check("polling returns the events after an id", since.size() == 2 and int(since[1]["id"]) == int(df_events[n_first - 1]["id"]), "%d after the third last" % since.size())
	# The same storm again within the day: the channels are emptied, nothing more.
	_storm(w, ro, 40.0, 1)
	_check("one event per basin per storm", df_events.size() == n_first, "%d more" % (df_events.size() - n_first))
	# Four years on the ground has healed (Cannon et al. 2008).
	_storm(w, ro, 0.0, 24)
	fire.age = 4.0
	fire.burn_changed(Rect2(-1024, -1024, 2048, 2048))
	_storm(w, ro, 40.0, 1)
	_check("none on ground burnt four years ago", df_events.size() == n_first, "%d more" % (df_events.size() - n_first))
	WorldContracts.get_ground().debris_flow.disconnect(_on_ground_debris_flow)
	WorldContracts.clear_overrides()
	ro.queue_free()
	ground.queue_free()
	w.queue_free()

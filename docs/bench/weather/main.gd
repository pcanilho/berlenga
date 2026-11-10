extends Node3D
# WeatherMap3D bench: checks the numbers, and renders a wet street with a flag, raster or path traced.
#
#   --test            print the checks and quit:
#                       mean wind on the log law at 2, 10 and 40 m against u10 ln(z / z0) / ln(10 / z0)
#                       gust spread at 10 m over a minute against the IEC turbulence intensity 0.96 / ln(10 / z0)
#                       a storm's downburst outflow against distance
#                       an hour of 5 mm/h rain, then the hours it takes to dry, against Penman
#                       a sea that follows the weather: its wind over time after the weather turns
#                       the cost of one map update
#                       clouds: the ground share in cloud shadow against cover, the optical depth of the
#                       thickest column, the baked shadow against a direct march, the bake's cost, and the
#                       shadow following the clouds as they drift
#   --view=map        an 8 km map with broken cloud, light rain and a storm drifting through, after an hour;
#                     saved with --map-out
#   --view=street     a street that the rain has wetted, with puddles, and a flag in the weather's wind
#   --view=clouds     broken cumulus over fields from a rise, WeatherSkyMaterial sky, cloud shadows on the sun
#   --view=clouds_low the same from 220 m, looking down on the cloud shadows
#   --view=projector  a checker projected by the sun (DirectionalLight3D projector), raster against path traced
#   --rain=H          hours of 5 mm/h rain before the shot (street view; 0 = dry)
#   --dry=H           then this many hours without rain
#   --raster          raster only (default path traced)
#   --pt-from=N       rasterize until frame N, then path trace
#   --frames=N        frames before --out is saved (default 12)
#   --out=path.png    save the last frame and quit
#   --map-out=path.png  save the map (wind speed red, cloud cover green, wetness blue) and quit
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#
# Run with --fixed-fps 30 so raster and path traced frames match.

var view := "street"
var raster := false
var pt_from := 0
var frames := 12
var out := ""
var map_out := ""
var spp := 16
var bounces := 3
var rain_hours := 1.0
var dry_hours := 0.0
var test := false
var n := 0
var env: Environment
var weather: Node3D


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--map-out="): map_out = a.substr(10)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--rain="): rain_hours = float(a.substr(7))
		elif a.begins_with("--dry="): dry_hours = float(a.substr(6))

	if not ClassDB.class_exists("WeatherMap3D"):
		push_error("This build has no weather module.")
		get_tree().quit(1)
		return
	if test:
		await _run_tests()
		get_tree().quit()
		return
	if view == "map":
		await _map_view()
		get_tree().quit()
		return
	if view == "clouds" or view == "clouds_low":
		await _build_clouds()
		return
	if view == "projector":
		_build_projector()
		return
	_build_street()


# ---------------------------------------------------------------------------------------------------
# Checks

func _new_map() -> Node3D:
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 4096.0
	m.resolution = 128
	m.wind_speed = 8.0
	m.wind_direction = Vector2(1, 0)
	m.roughness_length = 0.03
	m.cloud_cover = 0.5
	m.transition_time = 0.0
	add_child(m)
	return m


func _run_tests() -> void:
	var m := _new_map()
	await get_tree().physics_frame
	var z0: float = m.roughness_length
	print("log law (u10 = 8 m/s, z0 = %.2f m), mean wind at height:" % z0)
	for h in [2.0, 10.0, 40.0]:
		var want := 8.0 * log(h / z0) / log(10.0 / z0)
		# The mean: average the gusts away over many places along the wind.
		var acc := Vector3.ZERO
		for i in 400:
			acc += m.get_wind_at(Vector3(i * 37.0, 0.0, i * 11.0), h)
		acc /= 400.0
		print("  %4.0f m: %.2f m/s (log law %.2f)" % [h, Vector2(acc.x, acc.z).length(), want])

	# Gusts at one point over a minute of real time.
	var series := PackedFloat32Array()
	var lat := PackedFloat32Array()
	var vert := PackedFloat32Array()
	for i in 1800:
		await get_tree().physics_frame
		var w: Vector3 = m.get_wind_at(Vector3(0, 10, 0))
		series.append(w.x)
		lat.append(w.z)
		vert.append(w.y)
	var su := _std(series)
	print("gusts at 10 m over 60 s: mean %.2f m/s, sigma_u %.2f (I = %.3f, IEC %.3f), sigma_v/sigma_u %.2f (0.8), sigma_w/sigma_u %.2f (0.5), peak %.1f m/s" % [
			_mean(series), su, su / 8.0, 0.96 / log(10.0 / z0), _std(lat) / su, _std(vert) / su, _max(series)])

	# A storm: outflow against distance (it peaks just outside the rain core).
	m.add_storm(Vector2(0, 0), 1000.0, 40.0, 15.0, 0.0)
	m.wind_speed = 0.0
	m.advance(900.0)
	var line := "storm outflow (R 1000 m, peak 15 m/s), radial wind at r ="
	for r in [0.0, 500.0, 1000.0, 1121.0, 1500.0, 2000.0]:
		var c: Vector2 = m.get_mean_wind_at(Vector3(r, 0, 0))
		line += " %.0f m: %.1f," % [r, c.x]
	print(line)
	print("  rain at the core %.1f mm/h, at 1000 m %.1f mm/h" % [m.get_rain_at(Vector3(0, 0, 0)), m.get_rain_at(Vector3(1000, 0, 0))])
	m.clear_storms()
	m.queue_free()
	await get_tree().physics_frame

	# Wetting and drying.
	var d := _new_map()
	d.wind_speed = 3.0
	d.cloud_cover = 1.0
	d.cloud_variation = 0.0
	d.temperature = 15.0
	d.humidity = 0.7
	d.rain_rate = 5.0
	d.snap_to_settings()
	await get_tree().physics_frame
	d.advance(3600.0)
	var p := Vector3(10, 0, 10)
	print("after 1 h of 5 mm/h: wetness %.2f, puddles %.2f mm (storage %.1f mm, drain %.1f mm/h)" % [d.get_wetness_at(p), d.get_puddle_at(p), d.surface_storage, d.puddle_drain_rate])
	d.rain_rate = 0.0
	d.snap_to_settings()
	var hours := 0.0
	var puddle_gone := -1.0
	while hours < 48.0 and d.get_wetness_at(p) > 0.05:
		d.advance(600.0)
		hours += 600.0 / 3600.0
		if puddle_gone < 0.0 and d.get_puddle_at(p) <= 0.0:
			puddle_gone = hours
	# Penman at 3 m/s at 10 m: 2 m wind on the log law.
	var u2 := 3.0 * log(2.0 / z0) / log(10.0 / z0)
	var es := 6.112 * exp(17.62 * 15.0 / (243.12 + 15.0))
	var e_mm_h := 0.26 * (1.0 + 0.54 * u2) * es * 0.3 / 24.0
	print("drying (15 C, 70%%, 3 m/s): puddles gone after %.1f h, surface dry after %.1f h (Penman %.3f mm/h)" % [puddle_gone, hours, e_mm_h])
	d.queue_free()
	await get_tree().physics_frame

	# Cost of one update of the cells (128 x 128, a storm).
	var c2 := _new_map()
	c2.add_storm(Vector2(300, 0), 800.0, 20.0, 10.0, 0.0)
	await get_tree().physics_frame
	var t0 := Time.get_ticks_usec()
	for i in 20:
		c2.advance(1.0)
	print("one map update (128 x 128 cells, 1 storm, upload included): %.2f ms" % ((Time.get_ticks_usec() - t0) / 20000.0))
	c2.queue_free()
	await get_tree().physics_frame

	# The sea follows the weather.
	if ClassDB.class_exists("PhysXWaterSurface3D"):
		var s := _new_map()
		s.wind_speed = 4.0
		s.time_scale = 600.0 # ten weather minutes a second
		s.transition_time = 60.0
		s.snap_to_settings()
		var sea: Node3D = ClassDB.instantiate("PhysXWaterSurface3D")
		sea.fetch = 20000.0
		sea.wind_speed = 4.0
		sea.follow_weather = true
		add_child(sea)
		await get_tree().physics_frame
		s.wind_speed = 15.0
		s.wind_direction = Vector2(0, 1)
		var l2 := "sea wind after the weather turns to 15 m/s (20 km fetch, SPM growth over %.1f h):" % (
				68.8 * pow(9.81 * 20000.0 / (15.0 * 15.0), 2.0 / 3.0) * 15.0 / 9.81 / 3600.0)
		for i in 360:
			await get_tree().physics_frame
			if i % 60 == 59:
				l2 += " %.0f min %.1f m/s," % [(i + 1) / 30.0 * 10.0, sea.wind_speed]
		print(l2)
		sea.queue_free()
		s.queue_free()
		await get_tree().physics_frame

	await _cloud_tests()


func _cloud_tests() -> void:
	# Sun straight down: the share of the ground in cloud shadow against the cover setting.
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-90, 0, 0)
	add_child(sun)
	var c := _new_map()
	c.cloud_variation = 0.0
	c.cloud_type = 0.6
	c.sun_light = c.get_path_to(sun)
	c.cloud_shadow_extent = 8192.0
	c.cloud_shadow_resolution = 256
	await get_tree().physics_frame
	var line := "cloud shadow share of the ground (type 0.6, sun overhead), cover:"
	for cover in [0.2, 0.4, 0.6, 0.8, 1.0]:
		c.cloud_cover = cover
		c.snap_to_settings()
		c.update_cloud_shadow()
		var shaded := 0
		var mean_t := 0.0
		for i in 64:
			for j in 64:
				var v: float = c.get_sun_visibility_at(Vector3(-3000 + i * 95.0, 0, -3000 + j * 95.0))
				mean_t += v
				if v < 0.5:
					shaded += 1
		line += " %.1f: %.0f%% (mean sun %.2f)," % [cover, shaded / 40.96, mean_t / 4096.0]
	print(line)
	# Optical depth through the thickest column the bake saw, against Frostbite's range.
	c.cloud_cover = 0.6
	c.snap_to_settings()
	var best := 0.0
	var at := Vector3.ZERO
	for i in 40:
		for j in 40:
			var p := Vector3(-2000 + i * 100.0, 0, -2000 + j * 100.0)
			var tau := 0.0
			var y := 1500.0
			while y < 1500.0 + 1500.0:
				tau += c.get_cloud_density_at(Vector3(p.x, y + 5.0, p.z)) * 10.0
				y += 10.0
			if tau > best:
				best = tau
				at = p
	var sigma_max := 0.0
	var y2 := 1500.0
	while y2 < 3000.0:
		sigma_max = max(sigma_max, c.get_cloud_density_at(Vector3(at.x, y2, at.z)))
		y2 += 10.0
	print("thickest column (cover 0.6, type 0.6): optical depth %.0f, peak extinction %.3f /m (Frostbite cumulus 0.05 to 0.12)" % [best, sigma_max])
	# The baked shadow against a direct march for the same points.
	c.update_cloud_shadow()
	var err := 0.0
	for k in 200:
		var q := Vector3(randf_range(-3000, 3000), 0, randf_range(-3000, 3000))
		var tau2 := 0.0
		var y3 := 1500.0
		while y3 < 3000.0:
			tau2 += c.get_cloud_density_at(Vector3(q.x, y3 + 5.0, q.z)) * 10.0
			y3 += 10.0
		err += abs(1.0 / (1.0 + 0.75 * 0.15 * tau2) - c.get_sun_visibility_at(q))
	print("baked shadow vs a 10 m march (200 points, two-stream transmission): mean |difference| %.3f" % (err / 200.0))
	var t0 := Time.get_ticks_usec()
	for k in 5:
		c.update_cloud_shadow()
	print("one cloud shadow bake (256 x 256, 12 steps, worker threads): %.1f ms" % ((Time.get_ticks_usec() - t0) / 5000.0))
	# The shadow slides with the clouds between bakes: a point at a shadow's edge, followed downwind.
	c.cloud_cover = 0.4
	c.cloud_variation = 0.3
	c.snap_to_settings()
	c.update_cloud_shadow()
	var probe := Vector3.ZERO
	for k in 400:
		probe = Vector3(-1500 + (k % 20) * 150.0, 0, -1500 + (k / 20) * 150.0)
		var v0: float = c.get_sun_visibility_at(probe)
		if v0 > 0.3 and v0 < 0.7:
			break
	var before: float = c.get_sun_visibility_at(probe)
	var drift_s := 60.0
	c.advance(drift_s)
	var z0: float = c.roughness_length
	var steer: float = 8.0 * log(1000.0 / z0) / log(10.0 / z0)
	var moved: float = c.get_sun_visibility_at(probe + Vector3(steer * drift_s, 0, 0))
	print("shadow after %.0f s of drift, followed downwind %.0f m: %.3f (was %.3f)" % [drift_s, steer * drift_s, moved, before])
	c.queue_free()
	sun.queue_free()


func _mean(a: PackedFloat32Array) -> float:
	var s := 0.0
	for v in a: s += v
	return s / a.size()


func _std(a: PackedFloat32Array) -> float:
	var m := _mean(a)
	var s := 0.0
	for v in a: s += (v - m) * (v - m)
	return sqrt(s / a.size())


func _max(a: PackedFloat32Array) -> float:
	var r := -1e9
	for v in a: r = max(r, v)
	return r


# ---------------------------------------------------------------------------------------------------
# Street

const WET_SHADER := """
shader_type spatial;
// Wet ground from the weather map, after Lagarde, Water drop 3b: physically based wet surfaces (2013):
// a wet porous surface darkens (water fills the pores and light is trapped by total internal reflection,
// Lekner and Dorf 1988) and its film turns it glossy; in puddles the water is a smooth dielectric over a
// darker, drowned ground. Puddles collect in the low spots of a height map, filled to the map's puddle depth.
global uniform sampler2D weather_state_map : filter_linear, repeat_disable;
global uniform vec4 weather_map_rect;
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D height_tex : filter_linear_mipmap, repeat_enable;
uniform float uv_scale = 6.0;
uniform float porosity = 0.7;
uniform float dry_roughness = 0.85;
uniform float relief_mm = 12.0; // height of the ground's bumps; puddles fill them from the bottom
varying vec3 wpos;

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 muv = (wpos.xz - weather_map_rect.xy) / weather_map_rect.zw;
	vec4 st = texture(weather_state_map, muv);
	float wet = clamp(st.g, 0.0, 1.0);
	float puddle_mm = max(st.b, 0.0);
	vec2 uv = UV * uv_scale;
	vec3 a = texture(albedo_tex, uv).rgb;
	float h = texture(height_tex, uv * 0.25).r * relief_mm; // mm above the lowest point
	float in_puddle = smoothstep(-0.6, 0.6, puddle_mm - h);
	float dark = mix(1.0, 0.2, porosity * wet);
	ALBEDO = a * mix(dark, 0.25, in_puddle);
	ROUGHNESS = mix(mix(dry_roughness, 0.25, wet), 0.02, in_puddle);
	SPECULAR = 0.5;
}
"""


func _build_street() -> void:
	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.32, 0.36, 0.42)
	sky_mat.sky_horizon_color = Color(0.6, 0.62, 0.66)
	var sky := Sky.new()
	sky.sky_material = sky_mat
	env = Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and pt_from <= 0
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-30, 35, 0)
	sun.light_energy = 0.9
	sun.shadow_enabled = true
	add_child(sun)

	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.7, 6.0)
	cam.look_at_from_position(cam.position, Vector3(0, 0.4, -4), Vector3.UP)
	cam.fov = 55
	add_child(cam)
	cam.make_current()

	weather = ClassDB.instantiate("WeatherMap3D")
	weather.extent = 512.0
	weather.resolution = 64
	weather.wind_speed = 7.0
	weather.wind_direction = Vector2(1, -0.3)
	weather.roughness_length = 0.5 # buildings
	weather.cloud_cover = 0.9
	weather.cloud_variation = 0.0
	weather.rain_rate = 5.0 if rain_hours > 0.0 else 0.0
	weather.transition_time = 0.0
	add_child(weather)

	# Ground: cobbles from noise, so the puddles have low spots to fill.
	var noise := FastNoiseLite.new()
	noise.noise_type = FastNoiseLite.TYPE_CELLULAR
	noise.frequency = 0.06
	var height := NoiseTexture2D.new()
	height.noise = noise
	height.width = 512
	height.height = 512
	height.seamless = true
	var cnoise := FastNoiseLite.new()
	cnoise.frequency = 0.02
	var albedo := NoiseTexture2D.new()
	albedo.noise = cnoise
	albedo.width = 512
	albedo.height = 512
	albedo.seamless = true
	albedo.color_ramp = Gradient.new()
	albedo.color_ramp.set_color(0, Color(0.42, 0.38, 0.33))
	albedo.color_ramp.set_color(1, Color(0.62, 0.58, 0.52))
	await height.changed
	var sh := Shader.new()
	sh.code = WET_SHADER
	var gm := ShaderMaterial.new()
	gm.shader = sh
	gm.set_shader_parameter("albedo_tex", albedo)
	gm.set_shader_parameter("height_tex", height)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(40, 40)
	ground.mesh = pm
	ground.material_override = gm
	add_child(ground)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = WorldBoundaryShape3D.new()
	body.add_child(cs)
	add_child(body)

	# A wall and a pole, for something to compare against.
	var wall := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(30, 6, 0.4)
	wall.mesh = bm
	wall.position = Vector3(0, 3, -10)
	var wm := StandardMaterial3D.new()
	wm.albedo_color = Color(0.55, 0.5, 0.46)
	wm.roughness = 0.9
	wall.material_override = wm
	add_child(wall)
	var pole := MeshInstance3D.new()
	var cm := CylinderMesh.new()
	cm.top_radius = 0.04
	cm.bottom_radius = 0.04
	cm.height = 3.2
	pole.mesh = cm
	pole.position = Vector3(-1.5, 1.6, -3)
	var pmat := StandardMaterial3D.new()
	pmat.albedo_color = Color(0.2, 0.2, 0.22)
	pmat.metallic = 0.8
	pmat.roughness = 0.4
	pole.material_override = pmat
	add_child(pole)

	if ClassDB.class_exists("PhysXCloth3D"):
		var flag = ClassDB.instantiate("PhysXCloth3D")
		flag.grid_size = Vector2(1.4, 0.9)
		flag.grid_columns = 20
		flag.grid_rows = 13
		flag.pin_mode = 3 # left edge
		flag.wind_turbulence = 0.0 # the weather's gusts only
		flag.position = Vector3(-1.5 + 0.7, 2.7, -3)
		var fm := StandardMaterial3D.new()
		fm.albedo_color = Color(0.8, 0.2, 0.15)
		fm.roughness = 0.75
		fm.cull_mode = BaseMaterial3D.CULL_DISABLED
		flag.material_override = fm
		add_child(flag)

	await get_tree().physics_frame
	weather.snap_to_settings()
	if rain_hours > 0.0:
		weather.advance(rain_hours * 3600.0)
	if dry_hours > 0.0:
		weather.rain_rate = 0.0
		weather.snap_to_settings()
		weather.advance(dry_hours * 3600.0)
	print("street: wetness %.2f, puddles %.1f mm" % [weather.get_wetness_at(Vector3.ZERO), weather.get_puddle_at(Vector3.ZERO)])
	if map_out != "":
		_save_map(map_out)
		get_tree().quit()


# The cloud layer over open ground: a WeatherSkyMaterial sky, cloud shadows on the sun.
func _build_clouds() -> void:
	var sky_mat = ClassDB.instantiate("WeatherSkyMaterial")
	var sky := Sky.new()
	sky.sky_material = sky_mat
	sky.process_mode = Sky.PROCESS_MODE_REALTIME
	env = Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and pt_from <= 0
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_sky_shader_enabled = true # camera rays that miss run the sky shader, as raster does
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-35, 40, 0)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 400.0
	add_child(sun)

	var cam := Camera3D.new()
	cam.far = 60000.0
	cam.fov = 60
	if view == "clouds_low":
		# Looking down on the fields: the shadows.
		cam.position = Vector3(0, 220, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 0, -600), Vector3.UP)
	else:
		# From a rise: sky and shadowed land.
		cam.position = Vector3(0, 60, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 160, -1000), Vector3.UP)
	add_child(cam)
	cam.make_current()

	weather = ClassDB.instantiate("WeatherMap3D")
	weather.extent = 4096.0
	weather.resolution = 64
	weather.wind_speed = 6.0
	weather.wind_direction = Vector2(0.6, -0.8)
	weather.cloud_cover = 0.45
	weather.cloud_type = 0.6
	weather.cloud_variation = 0.3
	weather.transition_time = 0.0
	weather.cloud_shadow_extent = 8192.0
	weather.cloud_shadow_resolution = 256
	add_child(weather)
	weather.sun_light = weather.get_path_to(sun)

	# Fields: a large plane with a little colour noise, and some blocks for scale.
	var cnoise := FastNoiseLite.new()
	cnoise.frequency = 0.004
	var albedo := NoiseTexture2D.new()
	albedo.noise = cnoise
	albedo.width = 512
	albedo.height = 512
	albedo.seamless = true
	albedo.color_ramp = Gradient.new()
	albedo.color_ramp.set_color(0, Color(0.22, 0.3, 0.12))
	albedo.color_ramp.set_color(1, Color(0.42, 0.42, 0.22))
	await albedo.changed
	var gm := StandardMaterial3D.new()
	gm.albedo_texture = albedo
	gm.uv1_scale = Vector3(30, 30, 1)
	gm.roughness = 0.95
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(12000, 12000)
	ground.mesh = pm
	ground.material_override = gm
	add_child(ground)
	var bm := StandardMaterial3D.new()
	bm.albedo_color = Color(0.6, 0.57, 0.52)
	bm.roughness = 0.8
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for i in 60:
		var b := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(rng.randf_range(8, 20), rng.randf_range(6, 30), rng.randf_range(8, 20))
		b.mesh = box
		b.material_override = bm
		b.position = Vector3(rng.randf_range(-700, 700), box.size.y * 0.5, rng.randf_range(-1600, -150))
		add_child(b)

	await get_tree().physics_frame
	weather.snap_to_settings()
	weather.update_cloud_shadow()
	print("clouds: sun visibility under the camera %.2f" % weather.get_sun_visibility_at(cam.position))


# A checker projected by the sun, to check that the rasterizer and the path tracer place it alike.
func _build_projector() -> void:
	env = Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.2, 0.22, 0.25)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.15, 0.15, 0.17)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and pt_from <= 0
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var img := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	for y in 8:
		for x in 8:
			var on := (x + y) % 2 == 0
			img.set_pixel(x, y, Color(1, 1, 1) if on else Color(0.1, 0.1, 0.1))
	img.set_pixel(0, 0, Color(1, 0.1, 0.1)) # the corner, to see the orientation
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-60, 25, 0)
	sun.light_energy = 1.2
	sun.shadow_enabled = true
	sun.light_projector = ImageTexture.create_from_image(img)
	sun.projector_size = Vector2(4, 4)
	sun.projector_offset = Vector2(-2, -2)
	add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 4.5, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0, 0, 0), Vector3.UP)
	add_child(cam)
	cam.make_current()
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.7, 0.7, 0.7)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(12, 12)
	ground.mesh = pm
	ground.material_override = gm
	add_child(ground)
	var box := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(0.8, 1.2, 0.8)
	box.mesh = bm
	box.material_override = gm
	box.position = Vector3(1.2, 0.6, 0.5)
	add_child(box)


func _map_view() -> void:
	weather = _new_map()
	weather.extent = 8192.0
	weather.resolution = 256
	weather.wind_speed = 9.0
	weather.wind_direction = Vector2(1, 0.4)
	weather.cloud_cover = 0.45
	weather.rain_rate = 1.5
	weather.snap_to_settings()
	await get_tree().physics_frame
	# Start the storm upwind, so that it is near the middle after 20 minutes of drift with the wind aloft.
	var z0: float = weather.roughness_length
	var steer: Vector2 = weather.wind_direction.normalized() * 9.0 * log(1000.0 / z0) / log(10.0 / z0)
	weather.add_storm(Vector2(800, 300) - steer * 1200.0, 900.0, 35.0, 14.0, 0.0)
	weather.advance(1200.0)
	if map_out != "":
		_save_map(map_out)


# Four panels: cloud cover, rain rate (to 40 mm/h), wind speed at 10 m (to 25 m/s), wetness.
func _save_map(path: String) -> void:
	var wi: Image = weather.get_wind_texture().get_image()
	var si: Image = weather.get_state_texture().get_image()
	var w := wi.get_width()
	var h := wi.get_height()
	var img := Image.create(w * 4, h, false, Image.FORMAT_RGB8)
	for y in h:
		for x in w:
			var a := wi.get_pixel(x, y)
			var s := si.get_pixel(x, y)
			img.set_pixel(x, y, Color(a.b, a.b, a.b))
			var r: float = clamp(s.r / 40.0, 0.0, 1.0)
			img.set_pixel(w + x, y, Color(r, r, r))
			var sp: float = clamp(Vector2(a.r, a.g).length() / 25.0, 0.0, 1.0)
			img.set_pixel(2 * w + x, y, Color(sp, sp, sp))
			img.set_pixel(3 * w + x, y, Color(s.g, s.g, s.g))
	img.save_png(path)


func _process(_delta: float) -> void:
	if test or env == null:
		return
	n += 1
	if pt_from > 0 and n == pt_from:
		env.pathtracing_enabled = not raster
	if out != "" and n >= frames:
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

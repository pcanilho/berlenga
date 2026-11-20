extends Node3D
# Sky bench (docs/ideas/day-night.md).
#   --test   check SkyClock against Meeus' worked examples and known sky facts, print PASS/FAIL, quit
#   --sky [--model=scattering|physical] [--time=H] [--out=path.png] [--frames=N]
#            the sky alone (WeatherSkyMaterial, clouds from the defaults, sun from SkyClock), looking south-west
#            and a little up; prints the mean frame time, saves the last frame
#            [--chart]  colour patches on grey ground in front of the camera, lit by the clock's sun and moon
#                       (CameraAttributes.night_vision; --no-night-vision for the A/B, --lamp=E a warm lamp, --pt)
#            [--exposure=k] [--pitch=deg --yaw=deg --fov=deg] [--no-stars] [--no-airglow] [--milky-way=path] [--month=M --day=D] [--look-moon]
#            [--smoke=tau]  fires' smoke of that optical depth overhead (WeatherMap3D smoke haze): the sun reddens
#            [--pt]  path traced (camera rays that miss run the sky shader), for raster/PT pairs of the clouds

var fails := 0
var smoke_map: Node = null


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	if args.has("--test"):
		_test()
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("--sky"):
		var kv := {}
		for a in args:
			var p: PackedStringArray = a.trim_prefix("--").split("=", true, 1)
			kv[p[0]] = p[1] if p.size() > 1 else "1"
		await _sky(kv)
		get_tree().quit()


func _sky(kv: Dictionary) -> void:
	var sun := DirectionalLight3D.new()
	add_child(sun)
	var mat := WeatherSkyMaterial.new()
	mat.atmosphere_model = WeatherSkyMaterial.ATMOSPHERE_PHYSICAL_SKY if kv.get("model", "scattering") == "physical" else WeatherSkyMaterial.ATMOSPHERE_SCATTERING
	mat.cloud_reprojection = kv.has("reproject")
	if kv.has("no-stars"):
		mat.set_shader_parameter("stars_enabled", false)
	if kv.has("milky-way"):
		# --milky-way=path: the galactic panorama (art-src sky/milky_way/eso0932a_milky_way_panorama.jpg, Gitea LFS).
		var img := Image.load_from_file(kv["milky-way"])
		img.generate_mipmaps()
		mat.set_shader_parameter("milky_way", ImageTexture.create_from_image(img))
	if kv.has("no-airglow"):
		mat.set_shader_parameter("airglow_enabled", false)
	if kv.has("aerosol"):
		# --aerosol=km: the weather's aerosol haze for that visibility (Koschmieder, 3.912 / V), as WeatherMap3D gives it.
		mat.set_aerosol(3.912 / (float(kv["aerosol"]) * 1000.0), 0.9, 1200.0, 0.0)
	if kv.has("params"):
		# --params=name:value,...: any of the sky's shader parameters (e.g. cloud_cover:0.5,atmo_cloud_shadow_steps:0, vec2s as cloud_shear:900:0).
		for pair in String(kv["params"]).split(","):
			# name:x:y sets a vec2 (cloud_shear:900:0).
			var nv := pair.split(":")
			mat.set_shader_parameter(nv[0], Vector2(float(nv[1]), float(nv[2])) if nv.size() == 3 else float(nv[1]))
	var sky := Sky.new()
	sky.sky_material = mat
	sky.process_mode = Sky.PROCESS_MODE_REALTIME
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	# --exposure=k: the sky's own exposure, applied before the half-float buffers (pre-exposure, as Frostbite's):
	# a night sky needs about 1e5-1e6 (a faint star is 1e-8 lux; auto exposure is step 4).
	mat.set_shader_parameter("exposure", float(kv.get("exposure", "1")))
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 2, 0)
	cam.fov = float(kv.get("fov", "90"))
	add_child(cam)
	cam.rotation_degrees = Vector3(float(kv.get("pitch", "12")), float(kv.get("yaw", "135")), 0) # south-west
	cam.make_current()
	if kv.has("pre-exposure"):
		# --pre-exposure=k: the camera's exposure multiplier (Godot scales lights and the sky by it before the
		# buffers), instead of the sky's own exposure.
		var ca := CameraAttributesPractical.new()
		ca.exposure_multiplier = float(kv["pre-exposure"])
		cam.attributes = ca
	var clock := SkyClock.new()
	add_child(clock)
	clock.latitude = 45.0
	clock.longitude = 0.0
	clock.month = int(kv.get("month", "5"))
	clock.day = int(kv.get("day", "31"))
	clock.time_of_day = float(kv.get("time", "19.0"))
	clock.sun_light = clock.get_path_to(sun)
	if kv.has("smoke"):
		# --smoke=tau: fires' smoke over the bench, that optical depth overhead at 550 nm (WeatherMap3D smoke haze):
		# the sun's disc, its aureole and its light dimmed and reddened through it.
		var map := WeatherMap3D.new()
		smoke_map = map
		map.follow_target = NodePath("")
		add_child(map)
		map.time_scale = 1.0
		map.snap_to_settings()
		await get_tree().physics_frame
		map.advance(1.0)
		var want := float(kv["smoke"])
		# Grams per square metre over the map (4.5 m2/g, Reid et al. 2005), laid in every cell of the map (a trial
		# load, then scaled to the wanted depth), so a low sun's ray crosses the same smoke.
		var cs: float = map.extent / float(map.resolution)
		var cells := int(map.extent * 0.5 / cs)
		for pass_i in 2:
			var tau1: float = map.get_smoke_optical_depth_at(Vector3())
			var kg := 0.1 if pass_i == 0 else 0.1 * (want / maxf(tau1, 1e-6) - 1.0)
			if kg <= 0.0:
				break
			for gz in range(-cells, cells):
				for gx in range(-cells, cells):
					map.add_smoke(Vector3((gx + 0.5) * cs, 0.0, (gz + 0.5) * cs), kg)
		print("[sky] smoke optical depth overhead %.2f" % map.get_smoke_optical_depth_at(Vector3()))
	if kv.has("chart"):
		# --chart: a colour chart on grey ground under the clock's sun and moon (night vision checks).
		var moon := DirectionalLight3D.new()
		moon.shadow_enabled = true
		add_child(moon)
		clock.moon_light = clock.get_path_to(moon)
		var ground := MeshInstance3D.new()
		var pm := PlaneMesh.new()
		pm.size = Vector2(200, 200)
		ground.mesh = pm
		var gm := StandardMaterial3D.new()
		gm.albedo_color = Color(0.35, 0.35, 0.35)
		ground.material_override = gm
		add_child(ground)
		var cols := [Color(0.7, 0.1, 0.1), Color(0.85, 0.55, 0.1), Color(0.85, 0.8, 0.15), Color(0.15, 0.55, 0.15), Color(0.15, 0.25, 0.75), Color(0.45, 0.2, 0.6), Color(0.9, 0.9, 0.9), Color(0.05, 0.05, 0.05)]
		for i in cols.size():
			var b := MeshInstance3D.new()
			var bm := BoxMesh.new()
			bm.size = Vector3(0.9, 0.9, 0.9)
			b.mesh = bm
			var m := StandardMaterial3D.new()
			m.albedo_color = cols[i]
			b.material_override = m
			b.position = Vector3(-3.5 + i, 0.45, -5.0)
			add_child(b)
		# A lamp-lit patch: a warm omni light near the white box keeps its colour (photopic) at night.
		var lamp := OmniLight3D.new()
		lamp.light_color = Color(1.0, 0.75, 0.45)
		lamp.position = Vector3(2.5, 1.6, -4.0)
		lamp.omni_range = 4.0
		lamp.light_energy = float(kv.get("lamp", "0"))
		add_child(lamp)
		cam.position = Vector3(0, 1.6, 0)
		cam.rotation_degrees = Vector3(-12, 0, 0)
		cam.fov = 60.0
		if kv.has("pt"):
			env.pathtracing_enabled = true
	elif kv.has("pt"):
		env.pathtracing_enabled = true
		env.pathtracing_sky_shader_enabled = true
	# The clock drives the camera's exposure (an adapting eye) unless the bench sets one.
	clock.drive_exposure = not (kv.has("exposure") or kv.has("pre-exposure"))
	if kv.has("no-night-vision"):
		await get_tree().process_frame
		if cam.attributes:
			cam.attributes.night_vision = false
	if kv.has("look-sun"):
		# --look-sun: the camera turned to the sun's azimuth, keeping --pitch.
		await get_tree().process_frame
		var d := sun.global_transform.basis.z # toward the sun
		cam.rotation_degrees.y = rad_to_deg(atan2(-d.x, -d.z))
	if kv.has("look-moon"):
		# --look-moon: the camera on the moon (with a narrow --fov to see its phase).
		cam.look_at(cam.global_position + clock.get_moon_direction())
		print("[sky] moon alt %.2f deg, phase angle %.1f deg" % [rad_to_deg(asin(clock.get_moon_direction().y)), clock.get_moon_phase_angle()])
	for i in 20:
		await get_tree().process_frame
	var n := int(kv.get("frames", "30"))
	var t0 := Time.get_ticks_usec()
	for i in n:
		await RenderingServer.frame_post_draw
	var ms := (Time.get_ticks_usec() - t0) / 1000.0 / n
	print("[sky] %s at %.2f h (sun %.2f deg): %.2f ms a frame; ground %s lux, exposure x%s" % [kv.get("model", "scattering"), clock.time_of_day, clock.get_sun_altitude(), ms, str(clock.get_ground_illuminance()), str(clock.get_exposure_multiplier())])
	if kv.has("smoke"):
		print("[sky] smoke on the sun %s, sun light %s" % [str(smoke_map.get_smoke_transmittance(Vector3(), clock.get_sun_direction())), str(sun.light_color)])
	if kv.has("out"):
		get_viewport().get_texture().get_image().save_png(kv["out"])


func check(ok: bool, what: String) -> void:
	print(("[sky] PASS  " if ok else "[sky] FAIL  ") + what)
	if not ok:
		fails += 1


func _lin(col: Color) -> Vector3:
	var l := col.srgb_to_linear()
	return Vector3(l.r, l.g, l.b)


func ang(a: float, b: float) -> float:
	return absf(wrapf(a - b, -180.0, 180.0))


# The clock at a Julian day in TT (the worked examples are in dynamical time).
func at_tt(c: SkyClock, jd_tt: float) -> Dictionary:
	c.set_julian_day(jd_tt)
	var e: Dictionary = c.get_ephemeris()
	c.set_julian_day(jd_tt - (e["jd_tt"] - e["jd_ut"]))
	return c.get_ephemeris()


func _test() -> void:
	var c := SkyClock.new()
	add_child(c)
	c.refraction = false
	# Calendar (Meeus 7.a, 7.c).
	check(absf(SkyClock.julian_day(1957, 10, 4.81) - 2436116.31) < 1e-6, "Julian day of 1957 October 4.81 is 2436116.31")
	check(absf(SkyClock.julian_day(2000, 1, 1.5) - 2451545.0) < 1e-6, "J2000.0 is JD 2451545.0")
	# The sun, Meeus example 25.a: 1992 October 13.0 TD.
	var e := at_tt(c, 2448908.5)
	print("[sky] sun 1992-10-13: lon %.5f ra %.5f dec %.5f R %.5f AU" % [e["sun_lon"], e["sun_ra"], e["sun_dec"], e["sun_distance_km"] / 149597870.7])
	check(ang(e["sun_lon"], 199.90895) < 0.002, "sun's apparent longitude within 0.002 deg of Meeus 25.a (199.90895)")
	check(ang(e["sun_ra"], 198.38083) < 0.003 and absf(e["sun_dec"] + 7.78507) < 0.003, "sun's right ascension and declination within 0.003 deg of Meeus 25.a")
	# The moon, Meeus example 47.a: 1992 April 12.0 TD.
	e = at_tt(c, 2448724.5)
	print("[sky] moon 1992-04-12: lon %.6f lat %.6f dist %.1f km ra %.6f dec %.6f phase angle %.4f" % [e["moon_lon"], e["moon_lat"], e["moon_distance_km"], e["moon_ra"], e["moon_dec"], e["moon_phase_angle"]])
	check(ang(e["moon_lon"], 133.167265) < 0.01, "moon's apparent longitude within 0.01 deg of Meeus 47.a (133.167265)")
	check(absf(e["moon_lat"] + 3.229126) < 0.01, "moon's latitude within 0.01 deg of Meeus 47.a (-3.229126)")
	check(absf(e["moon_distance_km"] - 368409.7) < 20.0, "moon's distance within 20 km of Meeus 47.a (368409.7 km)")
	check(ang(e["moon_ra"], 134.688470) < 0.01 and absf(e["moon_dec"] - 13.768368) < 0.01, "moon's right ascension and declination within 0.01 deg of Meeus 47.a")
	# Moon phase, Meeus example 48.a (same instant): i = 69.0756 deg, k = 0.6786.
	check(absf(e["moon_phase_angle"] - 69.0756) < 0.05, "moon's phase angle within 0.05 deg of Meeus 48.a (69.0756)")
	check(absf(c.get_moon_illuminated_fraction() - 0.6786) < 0.001, "moon's illuminated fraction %.4f against 0.6786 (Meeus 48.a)" % c.get_moon_illuminated_fraction())
	# Sidereal time, Meeus example 12.a: 1987 April 10, 0h UT, Greenwich: 13h 10m 46.1351s apparent.
	c.longitude = 0.0
	c.utc_offset = 0.0
	c.set_julian_day(2446895.5)
	var lst: float = c.get_local_sidereal_time()
	var want := 13.0 + 10.0 / 60.0 + 46.1351 / 3600.0
	check(absf(lst - want) * 3600.0 < 0.1, "apparent sidereal time %.6f h against 13h10m46.1351s (Meeus 12.a), off %.3f s" % [lst, (lst - want) * 3600.0])
	# The sun's highest altitude at the June solstice: 90 - latitude + 23.44.
	c.latitude = 38.72
	c.longitude = -9.14
	c.utc_offset = 1.0
	c.year = 2026
	c.month = 6
	c.day = 21
	var best := -90.0
	var best_t := 0.0
	for k in 24 * 60:
		c.time_of_day = k / 60.0
		var a: float = c.get_sun_altitude()
		if a > best:
			best = a
			best_t = c.time_of_day
	print("[sky] Lisbon 2026-06-21: sun highest %.3f deg at %.3f h" % [best, best_t])
	check(absf(best - (90.0 - 38.72 + 23.437)) < 0.03, "Lisbon at the June solstice: sun's highest altitude %.3f deg (90 - 38.72 + 23.437 = 74.717)" % best)
	check(absf(best_t - 13.6) < 0.1, "Lisbon at the June solstice: local noon at %.2f h (solar noon about 13:36 in summer time)" % best_t)
	# The celestial basis puts the sun where the alt/az path does (no refraction).
	c.time_of_day = 9.5
	e = c.get_ephemeris()
	var ra := deg_to_rad(float(e["sun_ra"]))
	var dec := deg_to_rad(float(e["sun_dec"]))
	var eq := Vector3(cos(dec) * cos(ra), cos(dec) * sin(ra), sin(dec))
	var via_basis: Vector3 = c.get_celestial_basis() * eq
	check(rad_to_deg(via_basis.angle_to(c.get_sun_direction())) < 0.01, "celestial basis and sun direction agree (%.4f deg)" % rad_to_deg(via_basis.angle_to(c.get_sun_direction())))
	var sd := c.get_sun_direction()
	print("[sky] sun at 09:30: alt %.2f az %.2f dir %s" % [c.get_sun_altitude(), c.get_sun_azimuth(), sd])
	check(sd.x > 0.0, "morning sun is east of the meridian (+X)")
	# The lights: a DirectionalLight3D shines along its -Z, from the body toward the scene.
	var sun_l := DirectionalLight3D.new()
	sun_l.name = "Sun"
	add_child(sun_l)
	c.sun_light = c.get_path_to(sun_l)
	var shine := -sun_l.global_basis.z
	check(rad_to_deg(shine.angle_to(-c.get_sun_direction())) < 0.01 and sun_l.visible, "sun light shines from the sun (%.4f deg off)" % rad_to_deg(shine.angle_to(-c.get_sun_direction())))
	c.time_of_day = 2.0
	check(not sun_l.visible, "sun light is off at 02:00")
	# Full moon brightness: about 0.25 lux at the top of the atmosphere.
	var full_lux := 0.0
	var start: float = SkyClock.julian_day(2026, 11, 1.0)
	for k in 30 * 24:
		c.set_julian_day(start + k / 24.0)
		full_lux = maxf(full_lux, c.get_moon_illuminance_top())
	check(full_lux > 0.2 and full_lux < 0.35, "full moon gives %.3f lux above the air (Jensen 2001: about 0.25)" % full_lux)
	# Sunrise and sunset in Lisbon on 21 June 2026 (summer time): 06:12 and 21:05 (Observatorio
	# Astronomico de Lisboa almanac), when the sun's upper limb, lifted by refraction, meets the horizon.
	c.refraction = true
	c.year = 2026
	c.month = 6
	c.day = 21
	var rise := -1.0
	var set_t := -1.0
	var prev := -90.0
	for k in 24 * 60 * 6:
		c.time_of_day = k / 360.0
		var a: float = c.get_sun_altitude() + 0.267
		if prev < 0.0 and a >= 0.0:
			rise = c.time_of_day
		if prev >= 0.0 and a < 0.0:
			set_t = c.time_of_day
		prev = a
	print("[sky] Lisbon 2026-06-21: sunrise %02d:%04.1f sunset %02d:%04.1f" % [int(rise), fmod(rise, 1.0) * 60.0, int(set_t), fmod(set_t, 1.0) * 60.0])
	check(absf(rise - (6.0 + 12.0 / 60.0)) * 60.0 < 1.5 and absf(set_t - (21.0 + 5.0 / 60.0)) * 60.0 < 1.5, "Lisbon sunrise and sunset within 1.5 min of the almanac (06:12, 21:05)")
	c.time_of_day = rise
	var with_r: float = c.get_sun_altitude()
	c.refraction = false
	var without_r: float = c.get_sun_altitude()
	print("[sky] refraction at sunrise: %.3f deg" % (with_r - without_r))
	check(with_r - without_r > 0.45 and with_r - without_r < 0.65, "refraction at the horizon lifts the sun by about 0.57 deg")
	# The air's transmittance toward the sun (the atmosphere's own tables, Hillaire 2020 / Bruneton 2008 Earth air).
	var tz := SkyClock.air_transmittance(90.0, 0.0)
	var t5 := SkyClock.air_transmittance(5.0, 0.0)
	var t0h := SkyClock.air_transmittance(0.0, 0.0)
	var lum := func(v: Vector3) -> float: return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z
	print("[sky] air transmittance from sea level: zenith %s, 5 deg %s, horizon %s" % [tz, t5, t0h])
	check(tz.x > tz.y and tz.y > tz.z, "the air lets through more red than green than blue")
	check(lum.call(tz) > 0.78 and lum.call(tz) < 0.9, "sun at the zenith: %.0f lux of 133,800 reach sea level (clear air: 100,000 to 120,000)" % (133800.0 * lum.call(tz)))
	check(t5.z / t5.x < 0.4, "sun 5 deg up is orange (blue/red %.2f)" % (t5.z / t5.x))
	check(lum.call(SkyClock.air_transmittance(5.0, 3000.0)) > lum.call(t5), "the air is thinner from a mountain (3 km)")
	# The lights take that colour and strength.
	c.year = 2026
	c.month = 6
	c.day = 21
	c.time_of_day = 13.6
	var noon_e := sun_l.light_energy
	var noon_lux: float = noon_e * lum.call(_lin(sun_l.light_color)) / 1e-5
	print("[sky] noon sun light energy %.3f colour %s" % [noon_e, sun_l.light_color])
	check(noon_e > 0.95 and noon_e < 1.25 and sun_l.light_color.b > 0.85, "noon sun: light_energy %.2f (about 1.1 for 110,000 lux), near white" % noon_e)
	for k in 600:
		c.time_of_day = 20.0 + k / 600.0
		if c.get_sun_altitude() < 3.0:
			break
	var low_lux: float = sun_l.light_energy * lum.call(_lin(sun_l.light_color)) / 1e-5
	print("[sky] sun %.1f deg up: energy %.3f colour %s, %.0f lux against %.0f at noon" % [c.get_sun_altitude(), sun_l.light_energy, sun_l.light_color, low_lux, noon_lux])
	check(low_lux < 0.4 * noon_lux and sun_l.light_color.b < 0.5 * sun_l.light_color.r, "low sun: dimmer and orange")
	# Speed: the whole ephemeris.
	var t0 := Time.get_ticks_usec()
	for k in 1000:
		c.set_julian_day(start + k * 0.01)
		c.get_sun_direction()
	var us := (Time.get_ticks_usec() - t0) / 1000.0
	print("[sky] one update: %.1f us" % us)
	check(us < 50.0, "one clock update costs %.1f us" % us)
	print("[sky] %d failed" % fails)

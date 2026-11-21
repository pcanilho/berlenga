extends Node3D
# WeatherMap3D bench: checks the numbers, and renders a wet street with a flag, raster or path traced.
#
#   --test            print the checks and quit (--test=shelter: only the wind near the ground):
#                       mean wind on the log law at 2, 10 and 40 m against u10 ln(z / z0) / ln(10 / z0)
#                       gust spread at 10 m over a minute against the IEC turbulence intensity 0.96 / ln(10 / z0)
#                       a storm's downburst outflow against distance
#                       an hour of 5 mm/h rain, then the hours it takes to dry, against Penman
#                       a sea that follows the weather: its wind over time after the weather turns
#                       the cost of one map update
#                       clouds: the ground share in cloud shadow against cover, the optical depth of the
#                       thickest column, the baked shadow against a direct march, the bake's cost, and the
#                       shadow following the clouds as they drift, and clouds forming and clearing over time
#                       (share of sky held, how much of the ground changes after 1, 5 and 15 minutes)
#                       wind near the ground: a forest's profile against Cionco's, the lee of a wall against
#                       distance, gust patches (spread and size), the shader code against get_wind_at(), and
#                       the shelter bake's cost
#   --test=basics     the checks above down to the cost of one map update (no clouds, no shelter)
#   --test=clouds     only the cloud checks; --test=basics, --test=clouds and --test=shelter together are --test.
#                     The shelter and terrain checks draw the wind shader in a SubViewport, which never draws under
#                     --headless: run them under xvfb with a Vulkan driver (lavapipe on a machine with no GPU), about
#                     2 min for all of --test
#   --test=terrain    wind over terrain (docs/ideas/wind.md): a 60 m hill and a 40 m ridge, speed-up on the crest,
#                     slowing in the lee (Sx), turning on the flanks, the updraught on the windward slope, the
#                     fade with height, the shader code against get_wind_at(), the bake's cost, and particles
#                     (CPUParticles3D) catching the wind
#   --test=leaves     falling leaves (LeafFall3D, docs/ideas/wind.md): a broad leaf flutters down at about 1 m/s and
#                     drifts sideways, a needle drops near straight, leaves carried by the wind settle, and the wind
#                     lifts them again past the litter threshold
#   --view=map        an 8 km map with broken cloud, light rain and a storm drifting through, after an hour;
#                     saved with --map-out
#   --view=street     a street that the rain has wetted, with puddles, and a flag in the weather's wind
#   --view=cloud_map  the clouds from straight above over 24 km (column optical depth), saved with --out
#   --view=clouds     broken cumulus over fields from a rise, WeatherSkyMaterial sky, cloud shadows on the sun
#   --view=lenticular wind across a ridge 1.5 km high, a moist mid layer: wave clouds (lenticulars) in its lee
#   --view=clouds_low the same from 220 m, looking down on the cloud shadows
#   --view=clouds_wide fair-weather cumulus (cover 0.3, type 0.4) from 520 m, 10 degrees down, low sun behind:
#                     the landscape bench's clear wide shot (distant clouds at grazing angles)
#   --view=shafts     broken cumulus from a rise toward a sun 20 degrees up, aerosol haze: shafts under the clouds
#                     and across the sky (docs/ideas/weather.md, "Godrays", stages 1 and 2)
#   --view=towers     towering cumulus (cover 0.4, type 0.8) from a rise, a sun 17 degrees up off to the right: the
#                     towers' shadows on the clouds behind them (the sun shadow grid, docs/ideas/clouds.md)
#   --sun-up=D        shafts view: the sun D degrees up (default 20)
#   --sun-yaw=D       shafts view: the sun D degrees right of the view (default -13)
#   --mid-cover=C     clouds views: the mid (altocumulus) layer's base cover (WeatherMap3D.mid_cloud_cover)
#   --cloud-light=x,y,z,E,R[,r,g,b]  clouds views: a point light in the clouds (WeatherMap3D.add_cloud_light;
#                     orange by default, a fire's); repeatable
#   --cloud-effector=x,y,z,R,S  clouds views: a density effector (S > 0 adds cloud, < 0 clears); repeatable
#   --type=T          clouds views: cloud_type (0.05 stratus, 0.2 stratocumulus, 0.6 cumulus)
#   --wind=W          clouds views: wind_speed at 10 m (m/s)
#   --temp=C          clouds views: the air temperature (below about 1 C a storm's precipitation falls as snow)
#   --humidity=H      clouds views: the air's relative humidity at the ground (0 to 1; above 0.82 scud hangs under bases)
#   --fov=D           the camera's vertical field of view (degrees), to frame a whole storm
#   --storm=x,z,R,mm,out  clouds views: a storm (WeatherMap3D.add_storm, held), grown 15 min: its rain shafts under the
#                     base and the shelf cloud on its gust front ahead of it (it moves with the wind, about 9 km in the 15 min); repeatable
#   --wind-dir=x,z    clouds views: wind_direction (default 0.6,-0.8; 0,1 blows toward the camera)
#   --shear-aloft=S   clouds views: wind_shear_aloft (m/s per km); 5 and up gives cirrocumulus (get_cirrocumulus_share)
#   --no-wave-clouds  lenticular view: WeatherMap3D.wave_clouds off (cost A/B)
#   --sun-energy=E    clouds views: the sun's light_energy (0: night, for cloud lights)
#   --high-cover=C    clouds views: the cirrus layer's base cover (WeatherMap3D.high_cloud_cover); with the shafts
#                     view and --cover=0.05, the 22 and 46 degree ice halos round the sun
#   --cover=C --haze-km=V  shafts view: cloud cover (default 0.45) and the aerosol's dry visibility (default 40)
#   --froxel-haze     path traced: the aerosol stays a froxel layer (WeatherMap3D.traced_haze off)
#   --no-sky-shadow   the sky's air without the clouds' shadow (WeatherSkyMaterial atmo_cloud_shadow_steps 0)
#   --view=lightning  a storm at night over fields; a ground flash 1.4 km off on the last frame
#                     (--strike-frames=K: K frames after the flash)
#   --haze=uniform|layer|both  clouds view: precipitation haze pushed into the map (uniform 2 km
#                     visibility; a blowing layer 200 m visibility at the ground, 10 m height scale)
#   --view=projector  a checker projected by the sun (DirectionalLight3D projector), raster against path traced
#   --rain=H          hours of 5 mm/h rain before the shot (street view; 0 = dry)
#   --dry=H           then this many hours without rain
#   --raster          raster only (default path traced)
#   --pt-from=N       rasterize until frame N, then path trace
#   --frames=N        frames before --out is saved (default 12)
#   --out=path.png    save the last frame and quit
#   --map-out=path.png  save the map (wind speed red, cloud cover green, wetness blue) and quit
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --fire=W          clouds views: a fire plume of W watts convective heat 5 km ahead (a WorldFire stub) in dry air
#                     (humidity 0.3), the weather advanced 30 minutes: its fire cloud at its own condensation level
#   --no-reprojection  clouds view: every half resolution pixel every frame (WeatherSkyMaterial.cloud_reprojection off)
#   --pan=D           clouds view: the camera turns D degrees a second (reprojection under motion)
#   --cloud-time=S    clouds views: the clouds have evolved S seconds (the drift stays), for time-lapse strips
#   --frozen          clouds views: cloud_evolution off (shapes only slide, as before step 3e)
#
# Run with --fixed-fps 30 so raster and path traced frames match.

var view := "street"
var raster := false
var pt_from := 0
var frames := 12
var mid_cover := -1.0
var high_cover := -1.0
var cloud_lights_arg: Array = []
var storms_arg: Array = []
var wind_dir_arg := Vector2.ZERO
var cloud_effectors_arg: Array = []
var sun_energy := -1.0
var type_arg := -1.0
var wind_arg := -1.0
var temp_arg := -999.0
var fov_arg := 0.0
var humidity_arg := -1.0
var shear_arg := -1.0
var wave_clouds_off := false
var half_usec := 0
var out := ""
var map_out := ""
var spp := 16
var bounces := 3
var rain_hours := 1.0
var dry_hours := 0.0
var test := false
var only_shelter := false
var only_terrain := false
var only_leaves := false
var only_lightning := false
var only_haze := false
var only_basics := false
var only_clouds := false
var haze := ""
var no_sky_shadow := false
var froxel_haze := false
var sun_up := 20.0
var sun_yaw := -13.0
var cover := -1.0
var haze_km := -1.0
var strike_frames := 1
var light_scale := 1.0
var thunder_out := ""
var reprojection := true
var pan := 0.0
var cloud_time := -1.0
var frozen := false
var fire_w := 0.0


# A fire's convection column 5 km ahead of the clouds views' camera (--fire=W).
class PlumeStub extends WorldFire:
	var heat := 0.0
	func _plumes() -> PackedVector4Array:
		return PackedVector4Array([Vector4(0, 0, -5000, heat)])

var pan_cam: Camera3D
var n := 0
var env: Environment
var weather: Node3D
var lightning: Node3D
var strike_at := Vector3(150, 0, -1400)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--no-reprojection": reprojection = false
		elif a.begins_with("--pan="): pan = float(a.substr(6))
		elif a.begins_with("--cloud-time="): cloud_time = float(a.substr(13))
		elif a == "--frozen": frozen = true
		elif a.begins_with("--fire="): fire_w = float(a.substr(7))
		elif a == "--no-sky-shadow": no_sky_shadow = true
		elif a == "--froxel-haze": froxel_haze = true
		elif a.begins_with("--sun-up="): sun_up = float(a.substr(9))
		elif a.begins_with("--sun-yaw="): sun_yaw = float(a.substr(10))
		elif a.begins_with("--cover="): cover = float(a.substr(8))
		elif a.begins_with("--mid-cover="): mid_cover = float(a.substr(12))
		elif a.begins_with("--high-cover="): high_cover = float(a.substr(13))
		elif a.begins_with("--cloud-light="): cloud_lights_arg.append(a.substr(14).split_floats(","))
		elif a.begins_with("--wind-dir="):
			var wd := a.substr(11).split_floats(",")
			wind_dir_arg = Vector2(wd[0], wd[1])
		elif a.begins_with("--storm="): storms_arg.append(a.substr(8).split_floats(","))
		elif a.begins_with("--cloud-effector="): cloud_effectors_arg.append(a.substr(17).split_floats(","))
		elif a.begins_with("--sun-energy="): sun_energy = float(a.substr(13))
		elif a == "--no-wave-clouds": wave_clouds_off = true
		elif a.begins_with("--type="): type_arg = float(a.substr(7))
		elif a.begins_with("--wind="): wind_arg = float(a.substr(7))
		elif a.begins_with("--temp="): temp_arg = float(a.substr(7))
		elif a.begins_with("--fov="): fov_arg = float(a.substr(6))
		elif a.begins_with("--humidity="): humidity_arg = float(a.substr(11))
		elif a.begins_with("--shear-aloft="): shear_arg = float(a.substr(14))
		elif a.begins_with("--haze-km="): haze_km = float(a.substr(10))
		elif a == "--test": test = true
		elif a == "--test=shelter": test = true; only_shelter = true
		elif a == "--test=terrain": test = true; only_terrain = true
		elif a == "--test=leaves": test = true; only_leaves = true
		elif a == "--test=lightning": test = true; only_lightning = true
		elif a == "--test=haze": test = true; only_haze = true
		elif a == "--test=basics": test = true; only_basics = true
		elif a == "--test=clouds": test = true; only_clouds = true
		elif a.begins_with("--haze="): haze = a.substr(7)
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--map-out="): map_out = a.substr(10)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--rain="): rain_hours = float(a.substr(7))
		elif a.begins_with("--dry="): dry_hours = float(a.substr(6))
		elif a.begins_with("--strike-frames="): strike_frames = int(a.substr(16))
		elif a.begins_with("--light-scale="): light_scale = float(a.substr(14))
		elif a.begins_with("--thunder-out="): thunder_out = a.substr(14)

	if not ClassDB.class_exists("WeatherMap3D"):
		push_error("This build has no weather module.")
		get_tree().quit(1)
		return
	if test:
		if only_shelter:
			await _shelter_tests()
		elif only_terrain:
			await _terrain_tests()
		elif only_leaves:
			await _leaf_tests()
		elif only_lightning:
			await _lightning_tests()
		elif only_haze:
			await _haze_tests()
		elif only_clouds:
			await _cloud_tests()
			await _cloud_evolution_tests()
		else:
			await _run_tests()
		get_tree().quit()
		return
	if view == "map":
		await _map_view()
		get_tree().quit()
		return
	if view == "cloud_map":
		await _cloud_map()
		get_tree().quit()
		return
	if view in ["clouds", "clouds_low", "clouds_up", "clouds_wide", "shafts", "towers", "lenticular"]:
		await _build_clouds()
		return
	if view == "projector":
		_build_projector()
		return
	if view == "lightning":
		await _build_lightning()
		return
	_build_street()


# The clouds from straight above over 24 km: each pixel the optical depth of its column (get_cloud_density_at
# at 16 heights), white where thick. Shows whether the clouds repeat in a grid.
func _cloud_map() -> void:
	var m := _new_map()
	m.cloud_cover = 0.4
	await get_tree().physics_frame
	var px := 256
	var span := 24000.0
	var img := Image.create(px, px, false, Image.FORMAT_L8)
	var base: float = m.cloud_base_height
	var top: float = base + m.cloud_thickness
	var dh := (top - base) / 16.0
	for y in px:
		for x in px:
			var tau := 0.0
			for k in 16:
				tau += m.get_cloud_density_at(Vector3((x + 0.5) / px * span - span * 0.5, base + (k + 0.5) * dh, (y + 0.5) / px * span - span * 0.5)) * dh
			img.set_pixel(x, y, Color.from_hsv(0, 0, 1.0 - exp(-tau * 0.1)))
	img.save_png(out if out != "" else "user://cloud_map.png")
	print("cloud map: %d px over %.0f m saved" % [px, span])


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

	if only_basics:
		return
	await _cloud_tests()
	await _cloud_evolution_tests()
	await _shelter_tests()


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



# Clouds that form and clear (step 3e): the share of the ground in cloud shadow must hold over time (no pulsing),
# while the pattern itself changes: the share of points whose shadow state differs after a lag.
func _cloud_evolution_tests() -> void:
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-90, 0, 0)
	add_child(sun)
	var c := _new_map()
	c.cloud_cover = 0.4
	c.cloud_type = 0.6
	c.cloud_variation = 0.35
	c.cloud_evolution = true
	c.sun_light = c.get_path_to(sun)
	c.cloud_shadow_extent = 8192.0
	c.cloud_shadow_resolution = 256
	c.snap_to_settings()
	await get_tree().physics_frame
	var states := {}
	var line := "evolving clouds (cover 0.4, variation 0.35, lifetime %.0f s, churn %.1f m/s): shadow share at minute" % [c.cloud_lifetime, c.cloud_churn]
	var times := [0.0, 60.0, 300.0, 450.0, 900.0, 1350.0, 1800.0, 2700.0, 3600.0]
	var lo := 1.0
	var hi := 0.0
	for t in times:
		c.set_cloud_time(t)
		c.update_cloud_shadow()
		var st := PackedByteArray()
		st.resize(4096)
		var shaded := 0
		for i in 64:
			for j in 64:
				var v: float = c.get_sun_visibility_at(Vector3(-3000 + i * 95.0, 0, -3000 + j * 95.0))
				st[i * 64 + j] = 1 if v < 0.5 else 0
				shaded += st[i * 64 + j]
		states[t] = st
		lo = minf(lo, shaded / 4096.0)
		hi = maxf(hi, shaded / 4096.0)
		line += " %.0f: %.0f%%," % [t / 60.0, shaded / 40.96]
	print(line)
	print("  share range over the hour: %.0f%% to %.0f%%" % [lo * 100.0, hi * 100.0])
	var l2 := "  ground whose shadow changed since minute 0, after:"
	for t in [60.0, 300.0, 900.0, 3600.0]:
		var diff := 0
		for k in 4096:
			if states[t][k] != states[0.0][k]:
				diff += 1
		l2 += " %.0f min %.0f%%," % [t / 60.0, diff / 40.96]
	print(l2)
	# The whole sky: the cover's share over 64 km (ten cover-noise cells across), so a patch forming or clearing in
	# the window above cannot hide a pulse of the whole field.
	for cfg in [[900.0, 1.5], [900.0, 0.0], [0.0, 1.5]]:
		c.cloud_lifetime = cfg[0]
		c.cloud_churn = cfg[1]
		var l3 := "  cloud at 1950 m over 64 km (share of points), lifetime %.0f s, churn %.1f m/s, at minute:" % [cfg[0], cfg[1]]
		var shares := PackedFloat32Array()
		for t in [0.0, 225.0, 450.0, 675.0, 900.0, 1800.0, 2700.0, 3600.0]:
			c.set_cloud_time(t)
			var above := 0
			for i in 96:
				for j in 96:
					if c.get_cloud_density_at(Vector3(-32000 + i * 667.0, 1950.0, -32000 + j * 667.0)) > 0.0:
						above += 1
			shares.append(above / 9216.0)
			l3 += " %.0f: %.1f%%," % [t / 60.0, above / 92.16]
		print(l3)
	c.cloud_lifetime = 900.0
	c.cloud_churn = 1.5
	# Control: frozen clouds, the same window moved to eight other places (how much a 64 km window's share
	# varies by itself, from place to place).
	c.cloud_evolution = false
	var l4 := "  control, frozen, the window elsewhere:"
	for k in 8:
		var above2 := 0
		for i in 96:
			for j in 96:
				if c.get_cloud_density_at(Vector3(-32000 + i * 667.0 + 70000.0 * (k + 1), 1950.0, -32000 + j * 667.0)) > 0.0:
					above2 += 1
		l4 += " %.1f%%," % (above2 / 92.16)
	print(l4)
	c.cloud_evolution = true
	# Off: nothing changes with cloud time.
	c.cloud_evolution = false
	c.set_cloud_time(0.0)
	var f0 := []
	for i in 32:
		f0.append(c.get_cloud_density_at(Vector3(-3000 + i * 190.0, 2200.0, 500.0)))
	c.set_cloud_time(3600.0)
	var same := true
	for i in 32:
		if absf(c.get_cloud_density_at(Vector3(-3000 + i * 190.0, 2200.0, 500.0)) - f0[i]) > 1e-6:
			same = false
	print("  cloud_evolution off: density unchanged after an hour of cloud time: %s" % same)
	# Density effectors: a hole where the cloud is thickest, a cloud where the air is clear.
	var thick := Vector3.ZERO
	var clear := Vector3.ZERO
	var d_thick := 0.0
	var d_clear := 1.0
	for i in 32:
		var q := Vector3(-3000 + i * 190.0, 2200.0, 500.0)
		var d: float = c.get_cloud_density_at(q)
		if d > d_thick:
			d_thick = d
			thick = q
		if d < d_clear:
			d_clear = d
			clear = q
	var hole: int = c.add_cloud_effector(thick, 400.0, -1.0)
	var grow: int = c.add_cloud_effector(clear, 400.0, 1.0)
	print("  effectors: density in the thickest cloud %.4f -> %.4f with a clearing effector (want 0); clear air %.4f -> %.4f with an adding one (want > 0); contract cloud_effector_at %.2f, %.2f (want -1, 1)" % [d_thick, c.get_cloud_density_at(thick), d_clear, c.get_cloud_density_at(clear), c.get_cloud_effector_at(thick), c.get_cloud_effector_at(clear)])
	c.remove_cloud_effector(hole)
	c.remove_cloud_effector(grow)
	print("  effectors removed: density back %.4f, %.4f (want %.4f, %.4f)" % [c.get_cloud_density_at(thick), c.get_cloud_density_at(clear), d_thick, d_clear])
	var ids := []
	for i in 9:
		ids.append(c.add_cloud_light(Vector3(i * 100.0, 500.0, 0.0), Color(1, 0.6, 0.3), 1000.0, 5000.0))
	print("  cloud lights: ids %s (the ninth -1), listed %d" % [str(ids), c.get_cloud_lights().size()])
	for id in ids:
		c.remove_cloud_light(id)
	c.queue_free()
	sun.queue_free()
	await get_tree().physics_frame
	await _regime_tests()

# Round 6: the low cloud's type and base from the weather (docs/ideas/clouds.md).
func _regime_tests() -> void:
	var m := _new_map()
	var clock: Node = ClassDB.instantiate("SkyClock")
	add_child(clock)
	clock.latitude = 45.0
	clock.longitude = 0.0
	clock.utc_offset = 0.0
	clock.time_scale = 0.0
	m.sky_clock = m.get_path_to(clock)
	m.air_daily_cycle = true
	m.cloud_type = 0.6
	m.cloud_cover = 0.3
	m.temperature = 15.0
	for setup in [[2.0, 0.92, 0.0, "calm, humid"], [8.0, 0.92, 0.0, "windy, humid"], [2.0, 0.6, 0.0, "calm, dry"], [2.0, 0.92, 4.0, "calm, humid, rain 4 mm/h"]]:
		m.wind_speed = setup[0]
		m.humidity = setup[1]
		m.rain_rate = setup[2]
		var line := "  cloud regime (%s, afternoon type 0.6), type / heating / base at hour:" % setup[3]
		for h in [4.0, 8.0, 11.0, 14.0, 19.0]:
			clock.time_of_day = h
			m.snap_to_settings()
			line += " %.0fh %.2f / %.2f / %.0f m," % [h, m.get_cloud_type_at(Vector3.ZERO), m.get_convective_heating(), m.get_cloud_base_now()]
		print(line)
	m.cloud_regime_from_weather = false
	m.snap_to_settings()
	print("  cloud regime off: type %.2f (want 0.60), base %.0f m (want %.0f)" % [m.get_cloud_type_at(Vector3.ZERO), m.get_cloud_base_now(), m.cloud_base_height])
	m.queue_free()
	clock.queue_free()
	await get_tree().physics_frame

func _shelter_tests() -> void:
	var m := _new_map()
	m.gust_intensity = 0.0
	m.gust_patch_strength = 0.0
	await get_tree().physics_frame
	var z0: float = m.roughness_length
	# A forest 20 m tall (Cionco a = 2.5) round the origin.
	m.add_canopy(Vector2(0, 0), 100.0, 20.0, 2.5)
	m.rebake_shelter()
	var line := "forest (h 20 m, a 2.5), wind against open ground at height:"
	for h in [2.0, 5.0, 10.0, 15.0, 20.0, 30.0, 50.0, 80.0]:
		var w: Vector3 = m.get_wind_at(Vector3(0, 0, 0), h)
		line += " %.0f m %.2f (open %.2f)," % [h, Vector2(w.x, w.z).length(), 8.0 * log(h / z0) / log(10.0 / z0)]
	print(line)
	# Inside the canopy the profile should be exponential: u(z) / u(h) = exp(a (z / h - 1)).
	var top: float = m.get_shelter_at(Vector3(0, 0, 0), 20.0) * log(20.0 / z0)
	var l2 := "  inside, u / u(top) against exp(2.5 (z / 20 - 1)):"
	for h in [2.0, 10.0, 15.0]:
		var r: float = m.get_shelter_at(Vector3(0, 0, 0), h) * log(h / z0) / top
		l2 += " %.0f m %.3f (%.3f)," % [h, r, exp(2.5 * (h / 20.0 - 1.0))]
	print(l2)
	m.clear_shelter()

	# A wall 10 m high across the wind at x = 0: the wind at 2 m behind it.
	m.add_shelter_box(AABB(Vector3(-2, 0, -60), Vector3(4, 10, 120)))
	m.rebake_shelter()
	var l3 := "lee of a 10 m wall, wind at 2 m over open ground at x / H ="
	for xh in [-2.0, 1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0]:
		l3 += " %.0f: %.2f," % [xh, m.get_shelter_at(Vector3(xh * 10.0 + 2.0, 0, 0), 2.0)]
	print(l3)
	print("  bake of the 256 x 256 shelter square (2 m cells) with one wall: %.1f ms" % m.get_shelter_bake_time())
	m.clear_shelter()
	m.rebake_shelter()

	# Gust patches: the speed's spread at 2 m and how fast it decorrelates along a line.
	m.gust_patch_strength = 0.35
	var speeds := PackedFloat32Array()
	for i in 20000:
		var w: Vector3 = m.get_wind_at(Vector3(i * 0.5, 0, 1000.0 + float(i % 7) * 31.0), 2.0)
		speeds.append(Vector2(w.x, w.z).length())
	var mean := _mean(speeds)
	var l4 := "gust patches (size %.0f m, strength 0.35): speed at 2 m mean %.2f, spread %.2f of the mean; correlation at" % [m.gust_patch_size, mean, _std(speeds) / mean]
	for lag in [1, 4, 8, 12, 24]:
		var a := PackedFloat32Array()
		var b := PackedFloat32Array()
		for i in 5000:
			var p := Vector3(i * 3.1, 0, 2000.0)
			var w1: Vector3 = m.get_wind_at(p, 2.0)
			var w2: Vector3 = m.get_wind_at(p + Vector3(lag, 0, 0), 2.0)
			a.append(Vector2(w1.x, w1.z).length())
			b.append(Vector2(w2.x, w2.z).length())
		l4 += " %d m %.2f," % [lag, _corr(a, b)]
	print(l4)

	# The shader code against get_wind_at(), with gusts, patches and a wall.
	m.gust_intensity = 1.0
	m.add_shelter_box(AABB(Vector3(-2, 0, -60), Vector3(4, 10, 120)))
	m.rebake_shelter()
	await get_tree().physics_frame
	var vp := SubViewport.new()
	vp.size = Vector2i(32, 32)
	vp.use_hdr_2d = true
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	vp.process_mode = Node.PROCESS_MODE_ALWAYS
	add_child(vp)
	var rect := ColorRect.new()
	rect.size = Vector2(32, 32)
	var sm := ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = "shader_type canvas_item;\n" + m.get_wind_shader_code() + """
void fragment() {
	vec3 wp = vec3(mix(-40.0, 280.0, UV.x), 0.0, mix(-40.0, 40.0, UV.y));
	COLOR = vec4(weather_wind(wp, 2.0) * 0.05 + 0.5, 1.0);
}
"""
	sm.shader = sh
	rect.material = sm
	vp.add_child(rect)
	get_tree().paused = true
	for i in 3:
		await RenderingServer.frame_post_draw
	var img := vp.get_texture().get_image()
	var worst := 0.0
	var mean_err := 0.0
	var cnt := 0
	for y in range(2, 32, 6):
		for x in range(2, 32, 6):
			var u := (x + 0.5) / 32.0
			var v := (y + 0.5) / 32.0
			var wp := Vector3(lerp(-40.0, 280.0, u), 0.0, lerp(-40.0, 40.0, v))
			var cpu: Vector3 = m.get_wind_at(wp, 2.0)
			var c := img.get_pixel(x, y)
			var e := ((Vector3(c.r, c.g, c.b) - Vector3(0.5, 0.5, 0.5)) / 0.05).distance_to(cpu)
			worst = max(worst, e)
			mean_err += e
			cnt += 1
	get_tree().paused = false
	print("shader code against get_wind_at() at %d points (2 m, gusts, patches, a wall): mean difference %.3f m/s, worst %.3f m/s" % [cnt, mean_err / cnt, worst])
	vp.queue_free()
	m.queue_free()
	await get_tree().physics_frame


# Precipitation haze: sources add up, a layer falls off with height, and the environment's fog
# comes back as it was when the sources clear.
func _haze_tests() -> void:
	var we := WorldEnvironment.new()
	var e := Environment.new()
	e.fog_density = 0.0005
	we.environment = e
	add_child(we)
	var cam := Camera3D.new()
	add_child(cam)
	cam.make_current()
	var m := _new_map()
	await get_tree().physics_frame
	var a := Node.new()
	var b := Node.new()
	add_child(a)
	add_child(b)
	m.set_precipitation_extinction(a, 0.002, 0.0)
	m.set_precipitation_extinction(b, 0.02, 10.0, 0.95)
	await get_tree().physics_frame
	print("haze: extinction at 0, 10, 30 m: %.4f %.4f %.4f /m (want 0.0220 %.4f %.4f)" % [m.get_precipitation_extinction_at(Vector3(0, 0, 0)), m.get_precipitation_extinction_at(Vector3(0, 10, 0)), m.get_precipitation_extinction_at(Vector3(0, 30, 0)), 0.002 + 0.02 * exp(-1.0), 0.002 + 0.02 * exp(-3.0)])
	var vols := 0
	for c in m.get_children(true):
		if c is FogVolume:
			vols += 1
	print("haze: fog %s density %.4f (want 0.0020: the base 0.0005 is off), aerial %.2f, volumetric %s, %d fog volume, ambient inject %.2f (want 1)" % [e.fog_enabled, e.fog_density, e.fog_aerial_perspective, e.volumetric_fog_enabled, vols, e.volumetric_fog_ambient_inject])
	# A layer over an area takes its ground from the centre's height (snow on a mountain).
	m.set_precipitation_extinction(b, 0.02, 10.0, 0.95, Vector3(0, 1200, 0), 150.0)
	await get_tree().physics_frame
	var base_y := 0.0
	for c in m.get_children(true):
		if c is FogVolume:
			base_y = c.global_position.y
	print("haze: raised layer: volume base %.0f m (want 1200), extinction at 1200, 1210 m: %.4f %.4f (want 0.0220 %.4f)" % [base_y, m.get_precipitation_extinction_at(Vector3(0, 1200, 0)), m.get_precipitation_extinction_at(Vector3(0, 1210, 0)), 0.002 + 0.02 * exp(-1.0)])
	m.set_precipitation_extinction(b, 0.0, 10.0)
	m.clear_precipitation_extinction(a)
	m.clear_precipitation_extinction(b)
	await get_tree().physics_frame
	await get_tree().process_frame
	vols = 0
	for c in m.get_children(true):
		if c is FogVolume and not c.is_queued_for_deletion():
			vols += 1
	print("haze: cleared: fog %s density %.4f aerial %.2f volumetric %s, %d fog volumes, ambient inject %.2f (want 0)" % [e.fog_enabled, e.fog_density, e.fog_aerial_perspective, e.volumetric_fog_enabled, vols, e.volumetric_fog_ambient_inject])


# Lightning: Price and Rind's rate and ground share, a storm's flashes against that rate, a ground
# flash's channel and timing, and the thunder's delay, length and pitch with distance.
func _lightning_tests() -> void:
	if not ClassDB.class_exists("WeatherLightning3D"):
		print("lightning: no WeatherLightning3D in this build")
		return
	var m := _new_map()
	m.cloud_type = 0.3
	m.temperature = 15.0
	var lt: Node3D = ClassDB.instantiate("WeatherLightning3D")
	lt.thunder_enabled = false
	add_child(lt)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 2, 0)
	add_child(cam)
	cam.make_current()
	# Ground, and a 60 m mast where the scripted strike lands.
	var ground := StaticBody3D.new()
	var gs := CollisionShape3D.new()
	gs.shape = WorldBoundaryShape3D.new()
	ground.add_child(gs)
	add_child(ground)
	var mast := StaticBody3D.new()
	var ms := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(4, 60, 4)
	ms.shape = box
	mast.add_child(ms)
	mast.position = Vector3(800, 30, -1200)
	add_child(mast)
	await get_tree().physics_frame

	print("lightning: Price and Rind (1992) flashes per minute by cloud top (land, sea):")
	for h in [6000.0, 9000.0, 12000.0]:
		lt.marine = false
		var land: float = lt.get_flash_rate(h)
		lt.marine = true
		print("  %5.0f m: %.2f, %.2f" % [h, land, lt.get_flash_rate(h)])
	lt.marine = false
	print("lightning: ground share by cloud top over a 2.3 km freezing level (Price and Rind 1993):")
	for h in [8000.0, 10000.0, 12000.0, 15000.0]:
		print("  %5.0f m: %.2f" % [h, ClassDB.class_call_static("WeatherLightning3D", "ground_flash_fraction", h, 2300.0)])

	# A storm 3 km off, grown in (no lifetime: it rises over 600 s), many times the natural rate.
	m.add_storm(Vector2(3000, 0), 2000.0, 40.0, 10.0, 0.0)
	m.advance(1200.0)
	await get_tree().physics_frame
	lt.rate_scale = 100.0
	var seconds := 0.0
	var frames := 0
	var start: int = lt.get_strike_count()
	while seconds < 20.0:
		await get_tree().process_frame
		seconds += get_process_delta_time()
		frames += 1
	var st = m.get_cloud_state() if m.has_method("get_cloud_state") else null
	var got: int = lt.get_strike_count() - start
	var ground_n: int = lt.get_ground_strike_count()
	print("lightning: storm made %d flashes in %.1f s (%d frames), %d to the ground" % [got, seconds, frames, ground_n])

	# A scripted ground flash over the mast.
	lt.rate_scale = 0.0
	lt.strike(Vector3(800, 0, -1200), true)
	var ch: PackedVector3Array = lt.get_last_channel()
	var length := 0.0
	var max_turn := 0.0
	for i in ch.size() - 1:
		length += ch[i].distance_to(ch[i + 1])
	print("lightning: channel %d points, %.0f m from y %.0f to y %.1f (mast top 60), %.2f times the straight drop" % [ch.size(), length, ch[0].y, ch[ch.size() - 1].y, length / max(ch[0].distance_to(ch[ch.size() - 1]), 1.0)])
	var bright := PackedFloat32Array()
	for i in 40:
		await get_tree().process_frame
		bright.append(lt.get_flash_brightness())
	var line := ""
	for b in bright:
		line += "%.2f " % b
	print("lightning: brightness by frame: " + line)
	print("lightning: after the flash faded (%d active): last channel still %d points" % [lt.get_active_flash_count(), lt.get_last_channel().size()])
	# The cloud top a storm's flash rate uses: a calm map so the storm stays put while it grows.
	m.clear_storms()
	m.wind_speed = 0.0
	m.cloud_thickness = 2500.0
	m.snap_to_settings()
	m.add_storm(Vector2(-5000, 0), 2000.0, 40.0, 10.0, 0.0)
	m.advance(900.0)
	var top: float = m.get_cloud_top_at(Vector3(-5000, 0, 0))
	print("lightning: grown storm's cloud top %.0f m (base %.0f, thickness %.0f; base + 3 x thickness is %.0f), %.2f flashes a minute" % [top, m.cloud_base_height, m.cloud_thickness, m.cloud_base_height + 3.0 * m.cloud_thickness, lt.get_flash_rate(top)])

	# Flashes 340 m and 2 km off with their thunder played: the sound starts at the distance over c.
	# --thunder-out=dir saves each flash's thunder there.
	var heard := [0]
	lt.struck.connect(func(_p, _cg, _d): heard[0] += 1)
	lt.thunder_enabled = true
	for d in [340.0, 2000.0]:
		lt.strike(Vector3(d, 0, 0), true)
		var waited := 0.0
		var played: AudioStreamPlayer3D = null
		while waited < 10.0 and played == null:
			await get_tree().process_frame
			waited += get_process_delta_time()
			for c in lt.get_children(true):
				if c is AudioStreamPlayer3D and c.playing:
					played = c
		print("lightning: %4.0f m flash, thunder playing after %.2f s (first sound due %.2f s), %.1f s long" % [d, waited, lt.get_last_thunder_delay(), lt.get_last_thunder_length()])
		if played and thunder_out != "":
			played.stream.save_to_wav(thunder_out + "/thunder_%d.wav" % int(d))
		if played:
			played.stop()
	print("lightning: struck signal %d times" % heard[0])
	lt.thunder_enabled = false

	# Thunder from a straight 1500 m channel at 1, 3 and 8 km.
	var straight := PackedVector3Array()
	for i in 151:
		straight.append(Vector3(0, 1500.0 - i * 10.0, 0))
	print("lightning: thunder from a straight 1500 m channel (delay, length, peak dBFS, zero crossings per s, build ms):")
	for d in [1000.0, 3000.0, 8000.0]:
		var wav: AudioStreamWAV = lt.make_thunder(straight, Vector3(d, 2, 0), 15.0)
		var data: PackedByteArray = wav.data
		var peak := 0.0
		var crossings := 0
		var prev := 0
		for i in range(0, data.size(), 2):
			var v := data.decode_s16(i)
			peak = max(peak, abs(v) / 32767.0)
			if (v >= 0) != (prev >= 0):
				crossings += 1
			prev = v
		var secs := data.size() / 2.0 / wav.mix_rate
		print("  %4.0f m: %.2f s (sound %.2f), %.2f s, %.1f dB, %.0f/s, %.1f ms" % [d, lt.get_last_thunder_delay(), d / (331.3 + 0.606 * 15.0), lt.get_last_thunder_length(), 20.0 * log(max(peak, 1e-6)) / log(10.0), crossings / secs / 2.0, lt.get_last_thunder_build_time()])


func _corr(a: PackedFloat32Array, b: PackedFloat32Array) -> float:
	var ma := _mean(a)
	var mb := _mean(b)
	var sab := 0.0
	var saa := 0.0
	var sbb := 0.0
	for i in a.size():
		sab += (a[i] - ma) * (b[i] - mb)
		saa += (a[i] - ma) * (a[i] - ma)
		sbb += (b[i] - mb) * (b[i] - mb)
	return sab / sqrt(max(saa * sbb, 1e-12))


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
# The wave field once it is in: the slab where wave cloud can form, the lift and the cloud along a line across the ridge.
func _wave_report() -> void:
	for i in 6:
		await get_tree().process_frame
	var slab: Vector4 = weather.get_wave_cloud_slab()
	print("wave cloud slabs %.0f to %.0f m, %.0f to %.0f m, wind aloft at 4500 m %s m/s" % [slab.x, slab.y, slab.z, slab.w, str(weather.get_wind_aloft(Vector3(), 4500.0))])
	# Top-down round the camera: wave cloud optical depth over 48 km, as characters (the ridge's crest "|").
	var best := Vector3()
	var best_tau := 0.0
	for j in 12:
		var row := "  "
		for i in 24:
			var q := Vector3(pan_cam.position.x - 24000.0 + i * 2000.0, 0.0, pan_cam.position.z - 24000.0 + j * 4000.0)
			var tau := 0.0
			var ysum := 0.0
			for y in range(3500, 9000, 500):
				var d: float = weather.get_wave_cloud_density_at(Vector3(q.x, y, q.z)) * 500.0
				tau += d
				ysum += d * y
			if tau > best_tau and q.distance_to(Vector3(pan_cam.position.x, 0, pan_cam.position.z)) > 12000.0:
				best_tau = tau
				best = Vector3(q.x, ysum / tau, q.z)
			row += "#" if tau > 3.0 else ("+" if tau > 0.3 else ("|" if absf(q.x + 6000.0) < 1000.0 else " "))
		print(row)
	print("  thickest wave cloud over 12 km from the camera: %s, optical depth %.1f" % [str(best), best_tau])
	# Connected: the vertical wind of the waves aloft, none near the ground; dry air or no wind, no wave cloud.
	var wl := "  wave vertical wind at 4 km, x -12 to +8 km (m/s):"
	var gl := "  get_wind_at().y there, at 4 km / at 50 m above ground:"
	for k in 11:
		var wx := Vector3(-12000.0 + k * 2000.0, 0.0, 0.0)
		wl += " %.2f(%.0f)" % [weather.get_mountain_wave_velocity_at(wx + Vector3(0, 4000, 0)), weather.get_mountain_wave_at(wx + Vector3(0, 4000, 0))]
		gl += " %.2f/%.2f" % [weather.get_wind_at(wx + Vector3(0, 4000, 0), 4000.0).y, weather.get_wind_at(wx + Vector3(0, 50, 0), 50.0).y]
	print(wl)
	print(gl)
	if weather.has_method("get_wave_cloud_density_at"):
		var h0: float = weather.humidity
		weather.humidity = 0.3
		weather.snap_to_settings()
		for i in 2:
			await get_tree().create_timer(0.6).timeout
		var dry: Vector4 = weather.get_wave_cloud_slab()
		print("  humidity 0.3: slabs %.0f to %.0f, %.0f to %.0f m (want empty or narrower)" % [dry.x, dry.y, dry.z, dry.w])
		weather.humidity = h0
		weather.snap_to_settings()
		for i in 2:
			await get_tree().create_timer(0.6).timeout

func _build_clouds() -> void:
	var sky_mat = ClassDB.instantiate("WeatherSkyMaterial")
	sky_mat.cloud_reprojection = reprojection
	if no_sky_shadow:
		sky_mat.set_shader_parameter("atmo_cloud_shadow_steps", 0)
	for a in OS.get_cmdline_user_args():
		# --sky-params=name:value,...: the sky's shader parameters, for A/B checks of the cloud lighting.
		if a.begins_with("--sky-params="):
			for pair in a.substr(13).split(","):
				var nv := pair.split(":")
				sky_mat.set_shader_parameter(nv[0], float(nv[1]))
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
	if view == "clouds_wide":
		sun.look_at_from_position(Vector3(-2500, 1500, 2500), Vector3.ZERO) # low, behind the camera
	elif view == "shafts":
		var sd := 2668.0 # ahead
		var yr := deg_to_rad(sun_yaw)
		sun.look_at_from_position(Vector3(sin(yr) * sd, sd * tan(deg_to_rad(sun_up)), -cos(yr) * sd), Vector3.ZERO)
	elif view == "towers":
		var yt := deg_to_rad(75.0)
		sun.look_at_from_position(Vector3(sin(yt) * 2668.0, 2668.0 * tan(deg_to_rad(17.0)), -cos(yt) * 2668.0), Vector3.ZERO)

	var cam := Camera3D.new()
	cam.far = 60000.0
	cam.fov = 60
	if view == "clouds_wide":
		cam.fov = 55
		cam.position = Vector3(0, 520, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 0, -3000), Vector3.UP) # 10 degrees down
	elif view == "shafts":
		cam.position = Vector3(0, 60, 0)
		cam.look_at_from_position(cam.position, Vector3(-200, 230, -1000), Vector3.UP)
	elif view == "towers":
		cam.position = Vector3(0, 60, 0)
		cam.look_at_from_position(cam.position, Vector3(-150, 330, -1000), Vector3.UP)
	elif view == "clouds_up":
		# Looking up 50 degrees: the high and mid layers' texture.
		cam.position = Vector3(0, 60, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 60 + 1192, -1000), Vector3.UP)
	elif view == "clouds_low":
		# Looking down on the fields: the shadows.
		cam.position = Vector3(0, 220, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 0, -600), Vector3.UP)
	else:
		# From a rise: sky and shadowed land.
		cam.position = Vector3(0, 60, 0)
		cam.look_at_from_position(cam.position, Vector3(0, 160, -1000), Vector3.UP)
	if fov_arg > 0.0:
		cam.fov = fov_arg
	add_child(cam)
	pan_cam = cam
	cam.make_current()

	weather = ClassDB.instantiate("WeatherMap3D")
	weather.extent = 4096.0
	weather.resolution = 64
	weather.wind_speed = 6.0
	weather.wind_direction = Vector2(0.6, -0.8)
	weather.cloud_cover = 0.3 if view == "clouds_wide" else 0.45
	if cover >= 0.0:
		weather.cloud_cover = cover
	if haze_km > 0.0:
		weather.haze_visibility = haze_km
		weather.traced_haze = not froxel_haze
	weather.cloud_type = 0.4 if view == "clouds_wide" else 0.6
	if view == "towers":
		weather.cloud_cover = 0.4 if cover < 0.0 else cover
		weather.cloud_type = 0.8
	weather.cloud_variation = 0.3
	if type_arg >= 0.0:
		weather.cloud_type = type_arg
	if temp_arg > -999.0:
		weather.temperature = temp_arg
	if humidity_arg >= 0.0:
		weather.humidity = humidity_arg
	if wind_arg >= 0.0:
		weather.wind_speed = wind_arg
	if shear_arg >= 0.0:
		weather.wind_shear_aloft = shear_arg
		print("cirrocumulus share %.2f" % weather.get_cirrocumulus_share())
	if mid_cover >= 0.0:
		weather.mid_cloud_cover = mid_cover
	if high_cover >= 0.0:
		weather.high_cloud_cover = high_cover
	if wind_dir_arg != Vector2.ZERO:
		weather.wind_direction = wind_dir_arg
	for st in storms_arg: # x, z, radius, rain mm/h, outflow m/s: grown (15 min), then held
		weather.add_storm(Vector2(st[0], st[1]), st[2], st[3], st[4], 0.0)
	if not storms_arg.is_empty():
		weather.advance(900.0)
		var st0: Array = storms_arg[0]
		# Where the 15 minutes of steering carried it (about 9.6 km along the wind in the bench).
		var moved: Vector2 = 9600.0 * wind_dir_arg.normalized() if wind_dir_arg != Vector2.ZERO else Vector2.ZERO
		var line := "storm: cloud base toward the camera, from its first core (m out, base m):"
		for k in 12:
			var r := k * 500.0
			line += " %.0f %.0f," % [r, weather.get_cloud_base_at(Vector3(st0[0] + moved.x, 0.0, st0[1] + moved.y + r))]
		print(line)
		var dl := "storm: density up the core (height m, 1/m):"
		for hgt in [1600.0, 2500.0, 4000.0, 6000.0, 8000.0, 8800.0]:
			dl += " %.0f %.4f," % [hgt, weather.get_cloud_density_at(Vector3(st0[0] + moved.x, hgt, st0[1] + moved.y))]
		print(dl)
	for l in cloud_lights_arg: # x, y, z, energy, reach[, r, g, b]
		var col := Color(1.0, 0.62, 0.32) if l.size() < 8 else Color(l[5], l[6], l[7])
		print("cloud light id %d" % weather.add_cloud_light(Vector3(l[0], l[1], l[2]), col, l[3], l[4]))
	for e in cloud_effectors_arg: # x, y, z, radius, strength
		print("cloud effector id %d" % weather.add_cloud_effector(Vector3(e[0], e[1], e[2]), e[3], e[4]))
	if sun_energy >= 0.0:
		sun.light_energy = sun_energy
	weather.transition_time = 0.0
	weather.cloud_shadow_extent = 8192.0
	weather.cloud_shadow_resolution = 256
	add_child(weather)
	weather.sun_light = weather.get_path_to(sun)
	weather.cloud_evolution = not frozen
	if cloud_time >= 0.0:
		weather.set_cloud_time(cloud_time)
	if view == "lenticular":
		# Wind across a ridge (toward +x), a moist mid layer: wave clouds over its lee.
		weather.wind_direction = Vector2(1.0, 0.0)
		weather.wind_speed = 7.0
		weather.cloud_cover = 0.02 if cover < 0.0 else cover
		if mid_cover < 0.0:
			weather.mid_cloud_cover = 0.34
		weather.humidity = 0.8
		var ridge := Node3D.new()
		ridge.set_script(load("res://ridge.gd"))
		ridge.name = "Ridge"
		add_child(ridge)
		var rm := StandardMaterial3D.new()
		rm.albedo_color = Color(0.36, 0.34, 0.3)
		rm.roughness = 0.9
		ridge.build_mesh(rm)
		weather.terrain_source = weather.get_path_to(ridge)
		weather.wave_clouds = not wave_clouds_off
		cam.position = Vector3(14000, 300, 24000)
		cam.look_at_from_position(cam.position, Vector3(2000, 3800, 12000), Vector3.UP)
		weather.rebake_terrain_wind()
		_wave_report()
	if view == "clouds_wide":
		# The landscape bench's clear weather.
		weather.extent = 8192.0
		weather.resolution = 128
		weather.wind_speed = 4.0
		weather.wind_direction = Vector2(-0.2, -1.0).normalized()
		weather.temperature = 12.0
		weather.humidity = 0.6
		weather.snap_to_settings()
		sun.light_energy = 1.6

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
	gm.uv1_scale = Vector3(150, 150, 1)
	gm.roughness = 0.95
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(60000, 60000) # to the horizon from the wide view's 520 m (a 12 km plane ended in a grey band)
	ground.mesh = pm
	ground.material_override = gm
	add_child(ground)
	var bm := StandardMaterial3D.new()
	bm.albedo_color = Color(0.6, 0.57, 0.52)
	bm.roughness = 0.8
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for i in (0 if view == "towers" else 60): # the towers view: open fields only (a docs frame)
		var b := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(rng.randf_range(8, 20), rng.randf_range(6, 30), rng.randf_range(8, 20))
		b.mesh = box
		b.material_override = bm
		b.position = Vector3(rng.randf_range(-700, 700), box.size.y * 0.5, rng.randf_range(-1600, -150))
		add_child(b)

	await get_tree().physics_frame
	weather.snap_to_settings()
	if fire_w > 0.0:
		var stub := PlumeStub.new()
		stub.heat = fire_w
		WorldContracts.set_fire(stub)
		weather.humidity = 0.3
		weather.snap_to_settings()
		for i in 30:
			weather.advance(60.0)
		var fc: Vector3 = weather.get_fire_cloud_for(fire_w)
		print("fire: rise %.0f m, condensation level %.0f m, strength %.2f, fire clouds %d, map cloud base %.0f m" % [fc.x, fc.y, fc.z, weather.get_fire_cloud_count(), weather.cloud_base_height])
	weather.update_cloud_shadow()
	if haze == "uniform" or haze == "both":
		weather.set_precipitation_extinction(sun, 3.912 / 2000.0, 0.0)
	if haze == "layer" or haze == "both":
		weather.set_precipitation_extinction(cam, 3.912 / 200.0, 10.0, 0.95)
	print("clouds: sun visibility under the camera %.2f" % weather.get_sun_visibility_at(cam.position))


# A storm at night over fields and a village, a ground flash 1.4 km off.
func _build_lightning() -> void:
	var sky_mat = ClassDB.instantiate("WeatherSkyMaterial")
	sky_mat.cloud_reprojection = reprojection
	var sky := Sky.new()
	sky.sky_material = sky_mat
	sky.process_mode = Sky.PROCESS_MODE_REALTIME
	env = Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.glow_enabled = true
	env.pathtracing_enabled = not raster and pt_from <= 0
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_sky_shader_enabled = true
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	# Night: a dim moon behind the storm.
	var moon := DirectionalLight3D.new()
	moon.rotation_degrees = Vector3(-30, 150, 0)
	moon.light_energy = 0.03
	moon.light_color = Color(0.7, 0.75, 1.0)
	add_child(moon)

	var cam := Camera3D.new()
	cam.far = 60000.0
	cam.fov = 60
	cam.position = Vector3(0, 3, 0)
	add_child(cam)
	cam.look_at_from_position(cam.position, Vector3(0, 380, -1500), Vector3.UP)
	cam.make_current()

	weather = ClassDB.instantiate("WeatherMap3D")
	weather.extent = 8192.0
	weather.resolution = 64
	weather.wind_speed = 9.0
	weather.cloud_cover = 0.85
	weather.cloud_type = 0.8
	weather.cloud_thickness = 2500.0
	weather.transition_time = 0.0
	add_child(weather)
	weather.add_storm(Vector2(0, -2500), 3000.0, 40.0, 12.0, 0.0)
	weather.advance(1200.0)

	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.25, 0.3, 0.18)
	gm.roughness = 0.9
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(20000, 20000)
	ground.mesh = pm
	ground.material_override = gm
	add_child(ground)
	var body := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	shape.shape = WorldBoundaryShape3D.new()
	body.add_child(shape)
	add_child(body)
	var bm := StandardMaterial3D.new()
	bm.albedo_color = Color(0.7, 0.66, 0.6)
	bm.roughness = 0.8
	var rng := RandomNumberGenerator.new()
	rng.seed = 11
	for i in 40:
		var b := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(rng.randf_range(6, 14), rng.randf_range(5, 12), rng.randf_range(6, 14))
		b.mesh = box
		b.material_override = bm
		b.position = Vector3(rng.randf_range(-120, 120), box.size.y * 0.5, rng.randf_range(-260, -40))
		add_child(b)

	lightning = ClassDB.instantiate("WeatherLightning3D")
	lightning.rate_scale = 0.0
	lightning.seed = 5
	lightning.raster_light_scale = light_scale
	add_child(lightning)
	await get_tree().physics_frame
	weather.snap_to_settings()


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
	if pan != 0.0 and pan_cam:
		pan_cam.rotate_y(deg_to_rad(pan) * _delta)
	if pt_from > 0 and n == pt_from:
		env.pathtracing_enabled = not raster
	if lightning and n == frames - strike_frames - 1:
		lightning.strike(strike_at, true)
	if n == frames / 2:
		half_usec = Time.get_ticks_usec()
	if out != "" and n >= frames:
		# The second half's frames, for cost checks (--raster, with --frames well past the warm-up).
		print("frames %d to %d: %.1f ms a frame" % [frames / 2, frames, (Time.get_ticks_usec() - half_usec) / 1000.0 / (frames - frames / 2)])
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()


func _terrain_tests() -> void:
	var hill: Node3D = load("res://hill.gd").new()
	hill.name = "Hill"
	add_child(hill)
	var m := _new_map()
	m.gust_intensity = 0.0
	m.gust_patch_strength = 0.0
	m.terrain_source = m.get_path_to(hill)
	await get_tree().physics_frame
	m.rebake_terrain_wind()
	var tally := [0, 0]
	var check := func(cond: bool, what: String) -> void:
		print(("PASS " if cond else "FAIL ") + what)
		tally[0 if cond else 1] += 1
	check.call(m.is_terrain_wind_ready(), "terrain wind baked from the hill node (derive %.1f ms, 256 x 256, 8 m cells)" % m.get_terrain_wind_derive_time())
	var flat: Vector3 = m.get_wind_at(Vector3(-700, 0, 600), 10.0)
	var crest: Vector3 = m.get_wind_at(Vector3(0, 60, 0), 10.0)
	var lee: Vector3 = m.get_wind_at(Vector3(990, 0, 0), 10.0)
	var windward: Vector3 = m.get_wind_at(Vector3(-120, 0, 0), 10.0)
	var flank: Vector3 = m.get_wind_at(Vector3(-80, 0, 120), 10.0)
	var high: Vector3 = m.get_wind_at(Vector3(0, 60, 0), 300.0)
	var high_flat: Vector3 = m.get_wind_at(Vector3(-700, 0, 600), 300.0)
	print("  10 m wind: flat %.2f, hill crest %.2f, windward slope %.2f (w %.2f), lee of the ridge %.2f (Sx %.1f deg), flank turned %.1f deg" % [
			Vector2(flat.x, flat.z).length(), Vector2(crest.x, crest.z).length(), Vector2(windward.x, windward.z).length(), windward.y,
			Vector2(lee.x, lee.z).length(), rad_to_deg(m.get_terrain_lee_at(Vector3(990, 0, 0))), rad_to_deg(Vector2(flank.x, flank.z).angle())])
	check.call(absf(Vector2(flat.x, flat.z).length() - 8.0) < 0.05, "flat ground keeps the open wind at 10 m")
	check.call(Vector2(crest.x, crest.z).length() > 8.0 * 1.08, "the crest speeds the wind up (MicroMet curvature)")
	check.call(Vector2(lee.x, lee.z).length() < 8.0 * 0.85, "the ridge's lee slows it (Sx)")
	check.call(windward.y > 0.3, "the air climbs the windward slope")
	check.call(m.get_wind_at(Vector3(120, 0, 0), 10.0).y < -0.3, "and sinks down the lee slope")
	check.call(absf(Vector2(flank.x, flank.z).angle()) > deg_to_rad(2.0), "the wind turns along the contours on the flank")
	check.call(absf(Vector2(high.x, high.z).length() / Vector2(high_flat.x, high_flat.z).length() - 1.0) < 0.05, "300 m up, the hill no longer shows")
	var g: float = m.get_terrain_ground_at(Vector3(0, 0, 0))
	check.call(absf(g - 60.0) < 1.0, "the ground under the crest is the hill's top (%.1f m)" % g)
	var a: Vector3 = m.get_wind_at(Vector3(0, 70, 0))
	check.call(a.distance_to(crest) < 0.01, "no height given: measured from the terrain (10 m over the crest)")
	var mean_t: Vector2 = m.get_mean_wind_at(Vector3(0, 0, 0))
	var mean_f: Vector2 = m.get_mean_wind_at(Vector3(0, 0, 0), false)
	check.call(mean_t.length() > mean_f.length() * 1.05 and absf(mean_f.length() - 8.0) < 0.05, "get_mean_wind_at has the terrain; with terrain = false, the flat wind")

	# The wind aloft for the clouds: log law to the boundary layer, veering to the right (north), then sheared.
	var w10: Vector2 = m.get_mean_wind_at(Vector3(-700, 0, 600), false)
	var w1k: Vector2 = m.get_wind_aloft(Vector3(-700, 0, 600), 1000.0)
	var w3k: Vector2 = m.get_wind_aloft(Vector3(-700, 0, 600), 3000.0)
	var veer := rad_to_deg(w10.angle_to(w1k))
	print("  aloft: 10 m %.1f m/s, 1 km %.1f m/s veered %.1f deg, 3 km %.1f m/s" % [w10.length(), w1k.length(), veer, w3k.length()])
	check.call(veer > 20.0 and veer < 35.0 and w1k.length() > w10.length() * 1.5, "the wind aloft veers clockwise through the boundary layer and grows on the log law")
	check.call(absf(w3k.length() - w1k.length() - 3.0) < 0.05, "above it, the thermal-wind shear (1.5 m/s per km)")

	# The shader code against get_wind_at() over the hill and the ridge, at 5 m above the ground.
	var vp := SubViewport.new()
	vp.size = Vector2i(32, 32)
	vp.use_hdr_2d = true
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	vp.process_mode = Node.PROCESS_MODE_ALWAYS
	add_child(vp)
	var rect := ColorRect.new()
	rect.size = Vector2(32, 32)
	var sm := ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = "shader_type canvas_item;\n" + m.get_wind_shader_code() + """
void fragment() {
	vec3 wp = vec3(mix(-300.0, 1100.0, UV.x), 0.0, mix(-300.0, 300.0, UV.y));
	wp.y = weather_terrain(wp.xz).w + 5.0;
	COLOR = vec4(weather_wind(wp, weather_height_above_ground(wp)) * 0.05 + 0.5, 1.0);
}
"""
	sm.shader = sh
	rect.material = sm
	vp.add_child(rect)
	get_tree().paused = true
	for i in 3:
		await RenderingServer.frame_post_draw
	var img := vp.get_texture().get_image()
	var worst := 0.0
	var mean_err := 0.0
	var cnt := 0
	for y in range(2, 32, 6):
		for x in range(2, 32, 6):
			var u := (x + 0.5) / 32.0
			var v := (y + 0.5) / 32.0
			var wp := Vector3(lerp(-300.0, 1100.0, u), 0.0, lerp(-300.0, 300.0, v))
			wp.y = m.get_terrain_ground_at(wp) + 5.0
			var cpu: Vector3 = m.get_wind_at(wp)
			var c := img.get_pixel(x, y)
			var e := ((Vector3(c.r, c.g, c.b) - Vector3(0.5, 0.5, 0.5)) / 0.05).distance_to(cpu)
			worst = max(worst, e)
			mean_err += e
			cnt += 1
	get_tree().paused = false
	vp.queue_free()
	check.call(worst < 0.25, "shader code against get_wind_at() at %d points over the hill and ridge: mean difference %.3f m/s, worst %.3f m/s" % [cnt, mean_err / cnt, worst])

	# Particles: one slow puff from a CPUParticles3D on flat ground catches the wind across gravity.
	var p := CPUParticles3D.new()
	p.amount = 1
	p.lifetime = 30.0
	p.one_shot = true
	p.explosiveness = 1.0
	p.direction = Vector3(0, 1, 0)
	p.spread = 0.0
	p.initial_velocity_min = 0.0
	p.initial_velocity_max = 0.0
	p.gravity = Vector3(0, -0.5, 0)
	p.position = Vector3(-700, 20, 600)
	p.local_coords = true
	p.mesh = QuadMesh.new() # CPUParticles3D only runs with a mesh
	add_child(p)
	p.emitting = true
	for i in 240:
		await get_tree().physics_frame
	# The particle positions are not exposed; read where the whole system has gone from its AABB instead.
	var aabb: AABB = p.capture_aabb()
	var drift := aabb.get_center().x
	var w20: Vector3 = m.get_wind_at(Vector3(-700, 20, 600))
	# From rest with drag k: x = U (t - (1 - exp(-k t)) / k).
	check.call(absf(drift - w20.x * (4.0 - (1.0 - exp(-4.0)))) < 2.0, "a particle drifts downwind as linear drag predicts (%.1f m in 4 s, the 20 m wind %.1f m/s, wind_drag 1/s)" % [drift, w20.x])
	p.queue_free()
	var p2 := CPUParticles3D.new()
	p2.wind_drag = 0.0
	check.call(p2.wind_drag == 0.0 and ParticleProcessMaterial.new().wind_drag == 1.0, "wind_drag 1/s by default, 0 switches it off")
	p2.free()
	print("terrain: %d passed, %d failed" % [tally[0], tally[1]])
	m.queue_free()
	hill.queue_free()
	await get_tree().physics_frame


func _leaf_tests() -> void:
	if not ClassDB.class_exists("LeafFall3D"):
		print("FAIL no LeafFall3D in this build")
		return
	var tally := [0, 0]
	var check := func(cond: bool, what: String) -> void:
		print(("PASS " if cond else "FAIL ") + what)
		tally[0 if cond else 1] += 1
	# A beech leaf: 8 cm chord, 30 cm2, 0.25 g dry. A pine needle pair: 8 cm long, 1 mm wide, 0.03 g.
	var broad: PackedVector4Array = ClassDB.class_call_static("LeafFall3D", "simulate_plate", 0.08, 0.003, 0.00025, 1.0, Vector3.ZERO, 15.0, 10.0)
	var needle: PackedVector4Array = ClassDB.class_call_static("LeafFall3D", "simulate_plate", 0.08, 0.00008, 0.00003, 0.0, Vector3.ZERO, 15.0, 10.0)
	var lat_b := 0.0
	var lat_n := 0.0
	var swings := 0
	var prev_x := 0.0
	var prev_dx := 0.0
	for i in broad.size():
		lat_b = maxf(lat_b, Vector2(broad[i].x, broad[i].z).length())
		if i > 0:
			var dx := broad[i].x - prev_x
			if dx * prev_dx < 0.0:
				swings += 1
			prev_dx = dx
		prev_x = broad[i].x
	for v in needle:
		lat_n = maxf(lat_n, Vector2(v.x, v.z).length())
	var t_b := 10.0
	var vy_b := -broad[broad.size() - 1].y / t_b
	var vy_n := -needle[needle.size() - 1].y / 10.0
	# Falling broadside it would reach sqrt(2 m g / (rho S C_D)), C_D 2.4: fluttering, it falls slower than that.
	var broadside := sqrt(2.0 * 0.00025 * 9.81 / (1.2 * 0.003 * 2.4))
	print("  broad leaf, still air, 10 s: fell %.1f m (%.2f m/s, broadside terminal %.2f), widest sideways %.2f m, %d swings; needle: %.2f m/s, sideways %.3f m" % [-broad[broad.size() - 1].y, vy_b, broadside, lat_b, swings, vy_n, lat_n])
	check.call(vy_b > 0.5 * broadside and vy_b < 1.05 * broadside, "a broad leaf flutters down a little slower than it would fall flat")
	check.call(lat_b > 0.08 and swings >= 4, "and swings from side to side as it falls")
	check.call(lat_n < lat_b * 0.3, "a needle drops near straight")
	var windy: PackedVector4Array = ClassDB.class_call_static("LeafFall3D", "simulate_plate", 0.08, 0.003, 0.00025, 1.0, Vector3(5, 0, 0), 15.0, 6.0)
	var dx := windy[windy.size() - 1].x
	check.call(dx > 5.0 * 6.0 * 0.7, "in a 5 m/s wind it travels with the air (%.1f m in 6 s)" % dx)

	var m := _new_map()
	m.wind_speed = 3.0
	m.gust_intensity = 0.0
	m.gust_patch_strength = 0.0
	await get_tree().physics_frame
	var lf: Node3D = ClassDB.instantiate("LeafFall3D")
	add_child(lf)
	var quad := QuadMesh.new()
	quad.size = Vector2(0.08, 0.05)
	quad.orientation = PlaneMesh.FACE_Y
	lf.add_kind(quad, 0.08, 0.003, 0.00025, 1.0)
	lf.lift_speed = 4.0
	for i in 200:
		lf.drop_leaf(Vector3(randf_range(-5, 5), 8.0 + randf() * 4.0, randf_range(-5, 5)), 0)
	for i in 60 * 30:
		await get_tree().physics_frame
	var lying := 0
	var st: PackedVector4Array = lf.get_leaf_states()
	var mean_x := 0.0
	for v in st:
		if v.w > 0.5:
			lying += 1
		mean_x += v.x
	mean_x /= maxf(st.size(), 1)
	print("  200 leaves from 8 to 12 m in a 3 m/s wind: %d on the ground after 30 s, moved %.1f m downwind on average, step %.2f ms" % [lying, mean_x, lf.get_step_time()])
	check.call(lying >= 190, "the leaves settle on the ground")
	check.call(mean_x > 5.0, "carried downwind on the way")
	var lifted0: int = lf.get_lifted_count()
	m.wind_speed = 25.0
	m.snap_to_settings()
	for i in 60 * 5:
		await get_tree().physics_frame
	var lifted: int = lf.get_lifted_count() - lifted0
	check.call(lifted > 20, "a gale lifts lying leaves again (%d lifts in 5 s)" % lifted)
	print("leaves: %d passed, %d failed" % [tally[0], tally[1]])
	lf.queue_free()
	m.queue_free()
	await get_tree().physics_frame

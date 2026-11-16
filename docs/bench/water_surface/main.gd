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
#   --view=surf     low along the beach, the shore waves rolling in and breaking (--node=shore_waves:true)
#   --view=lip      low in the shallows, looking back at the plunging lips (--node=shore_waves:true)
#   --view=sea      out to sea from 6 m up (with --node=wave_cascades:3 for swell under the chop)
#   --view=storm    a gale over deep open sea (22 m/s, 4 cascades, depth 80 m, no floor) from 7 m up; storm_sun looks
#                   toward the sun from 9 m up, through the crests; storm_low looks downwind from 3.5 m at 20 m/s
#                   (the windrows run away from the viewer)
#   --view=sea_level  eye 1 m above a moderate sea (5 m/s), into a low sun: near crests in silhouette
#   --view=storm_roller 1.75 m up, across the wind at the front face of spilling crests, low side sun (20 degrees)
#   --view=storm_plume 4 m up, steeply down on a fresh whitecap and its turquoise bubble plume (as storm_foam)
#   --view=storm_foam  13 m/s deep sea from 8 m up, 25 degrees down, sun to the side (--sun-pitch=20): whitecaps ageing to lace
#   --view=storm_glitter  8 m/s deep sea from 2 m up into the sun's glitter path (--sun-pitch=20)
#   --view=sea_top  straight down on the open sea from 90 m (crest lengths and crossing seas)
#   --view=wake     straight down on the rock and the water downstream of it
#   --raster        raster only, for comparison
#   --contact       a WeatherMap3D (dry weather) whose contact wetness the sea writes; the sand reads it (wetness on),
#                   and the wetness up the beach is printed every 60 frames
#   --wader         two legs (capsules) walking along the shallows at 1.5 m/s with a PhysXWaterPusher3D
#                   (--wader-nopush: without it); --view=wader looks at them
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --frames=N      frames rendered before --out is saved (default 12)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/refraction:0
#   --out=path.png  save the last frame and quit
#   --height-out=path.png  the surface height over 160 m of open sea (sample_height), and the slope spread along x and
#                   z, at the last frame
#   --whitecaps=U1,U2,...  step the wind through these speeds (m/s), --whitecap-hold frames each, and print the
#                   whitecap cover the wind asks for (Monahan) against the cover on the foam grids, and the threshold
#   --whitecap-hold=N  frames at each wind speed (default 600)
#   --boat=U        a hull (8 m x 2.4 m) crossing open sea along +X at U m/s, with a PhysXWaterWake3D; the camera
#                   follows it (--view=boat from above, --view=boat_low from behind and to the side, --view=boat_sun
#                   from ahead looking back toward the sun); no beach
#   --wake=prop:value  a PhysXWaterWake3D property (e.g. dispersive:false)
#   --wake-out=path.png  at the last frame, the wake's height around the boat (grey, +-3 sigma) and the Kelvin
#                   measures: the wedge half-angle (where the waves are highest, each distance behind the bow)
#                   and the transverse wavelength along the track (theory 19.47 degrees, 2 pi U^2 / g)
#   --debug=N       path tracing debug view (Environment.pathtracing_debug_mode)
#   --water=param:value  a water shader parameter, e.g. --water=pathtraced_scattering:Vector3(0,0,0)
#   --node=prop:value  a PhysXWaterSurface3D property (e.g. live_flow:false)
#   --current=X,Z   a current in m/s (flow round the rocks, boxes drift)
#   --flow-out=path.png  save the solved flow (speed as brightness, direction as hue) and quit
#   --bake-time=N   with --flow-out, first time N flow bakes (rebake_flow) with the multigrid solver and with the
#                   older SOR sweeps (physics/physx_3d/water/multigrid_flow), and print the mean of each
#   --pt-from=N     rasterize until frame N, path trace from then on (a long settle at raster speed)
#   --tracers=N     N white flecks floating on the surface, carried by sample_flow() (shows the flow)
#   --seq=N         also save the N frames after --frames, as path_0000.png ... (for a GIF)
#   --seq-step=K    with --seq, save every Kth frame (a time-lapse)
#   --view=top      straight down over the surf zone
#   --dive=M        over the --seq frames, lower the camera by M metres (through the surface at --view=waterline)
#   --cam-dy=M      raise (or lower) the camera by M metres
#   --sun-energy=E  the sun's energy (default 1; 0 = in shade, lit by the sky only)
#   --dump-shader=path  write the water material's shader code to a file
#   --shader-file=path  replace the water material's shader with this file (try a shader change without a rebuild)
#   --perf          print the mean process and physics time per frame (after frame 60) and the cost of sample_height()
#   --lip-debug     draw the breaking lips opaque red (to see their shape)
#   --breakers      print the breaking lips' size and cost each frame (crest points, vertices, spray rate, CPU ms)
#   --splash=Z[,S]  drop a mound of water at (-4..4, Z) every 2 s, to watch it run up the sand (--view=runup --splash=6)
#   --profile       print the surface height along x = 0, out to sea and up the beach, at the last frame
#   --plain-sand    noise-toned sand instead of the checker (for shots)
#   --no-props      no slabs, standing rock or floating boxes (for shots)
#   --view=shallows lower and closer over the shallow sand (caustics shots)
#   --slope=DEG     the sand's slope (default 5); steeper reaches deep water sooner
#   --sun-pitch=DEG the sun's height above the horizon (default 50)
#   --shade=X,Z     a slab 12 m up whose shadow (a 5 m band along x) falls on the water at X,Z (an overhang's shade)
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
var plain_sand := false
var no_props := false
var cam_dy := 0.0
var sun_energy := 1.0
var dive := 0.0
var shade := Vector2(INF, INF)
var sun_pitch := 50.0
var bench_cam: Camera3D
var water_params := {}
var node_params := {}
var current := Vector2.ZERO
var flow_out := ""
var bake_time := 0
var seq := 0
var cam_set := PackedFloat64Array() # --cam=x,y,z,tx,ty,tz: an explicit camera, over the view's
var seq_step := 1
var tracers := 0
var pt_from := 0
var bench_env: Environment
var tracer_mm: MultiMesh
var tracer_pos := PackedVector3Array()
var tracer_rng := RandomNumberGenerator.new()
var n := 0
var water: Node3D
var sea_series := PackedFloat32Array()
var height_out := ""
var whitecap_winds := PackedFloat32Array()
var whitecap_hold := 600
var whitecap_sum := Vector3.ZERO
var whitecap_count := 0
var profile := false
var perf := false
var breakers := false
var lip_debug := false
var hide_lips := false
var breaker_ms := 0.0
var breaker_peak := {}
var perf_proc := 0.0
var perf_phys := 0.0
var perf_n := 0
var boat_speed := 0.0
var splash_strength := 1.0
var splash_z := 1e9 # --splash=Z: a mound of water dropped at (0, Z) every 2 s (run-up onto the sand)
var debug_storm := OS.get_environment("STORM_DEBUG") != ""
var boat: RigidBody3D
var wader: AnimatableBody3D
var contact := false
var weather: Node3D
var wader_on := false
var wader_push := true
var wake: Node3D
var wake_params := {}
var wake_out := ""


# The surface height from sample_height() on a 256 x 256 grid over 160 m of open sea, grey from -2 to +2 sigma,
# and the mean square slope along x and z: their ratio shows the directional spread (Cox and Munk 1954 measured
# upwind / crosswind 1.48 at 14 m/s).
func _save_heights() -> void:
	var px := 256
	var span := 160.0
	var h := PackedFloat32Array()
	h.resize(px * px)
	var mean := 0.0
	for y in px:
		for x in px:
			var v := float(water.call("sample_height", Vector3((x + 0.5) / px * span - span * 0.5, 0.0, -100.0 + (y + 0.5) / px * span - span * 0.5)))
			h[y * px + x] = v
			mean += v
	mean /= px * px
	var var_h := 0.0
	var sx := 0.0
	var sz := 0.0
	var dx := span / px
	for y in px - 1:
		for x in px - 1:
			var v := h[y * px + x] - mean
			var_h += v * v
			sx += pow((h[y * px + x + 1] - h[y * px + x]) / dx, 2)
			sz += pow((h[(y + 1) * px + x] - h[y * px + x]) / dx, 2)
	var sigma := sqrt(var_h / ((px - 1) * (px - 1)))
	var img := Image.create(px, px, false, Image.FORMAT_L8)
	for y in px:
		for x in px:
			img.set_pixel(x, y, Color.from_hsv(0, 0, clampf(0.5 + (h[y * px + x] - mean) / (4.0 * sigma), 0.0, 1.0)))
	img.save_png(height_out)
	print("heights: Hs %.2f m, mean square slope along x %.4f, along z %.4f, z/x %.2f (wind direction %s)" % [4.0 * sigma, sx / ((px - 1) * (px - 1)), sz / ((px - 1) * (px - 1)), sz / maxf(sx, 1e-9), water.get("wind_direction")])

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--taa": get_viewport().use_taa = true # resolves the jittered ray marches (wave shadows), as in a game
		elif a == "--wader": wader_on = true
		elif a == "--contact": contact = true
		elif a == "--wader-nopush": wader_on = true; wader_push = false
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--height-out="): height_out = a.substr(13)
		elif a.begins_with("--whitecaps="):
			for w in a.substr(12).split(","): whitecap_winds.push_back(float(w))
		elif a.begins_with("--whitecap-hold="): whitecap_hold = int(a.substr(16))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--debug="): debug_mode = int(a.substr(8))
		elif a.begins_with("--current="):
			var c := a.substr(10).split(",")
			current = Vector2(float(c[0]), float(c[1]))
		elif a.begins_with("--flow-out="): flow_out = a.substr(11)
		elif a.begins_with("--bake-time="): bake_time = int(a.substr(12))
		elif a.begins_with("--seq="): seq = int(a.substr(6))
		elif a.begins_with("--seq-step="): seq_step = maxi(int(a.substr(11)), 1)
		elif a.begins_with("--tracers="): tracers = int(a.substr(10))
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--slope="): slope_deg = float(a.substr(8))
		elif a == "--plain-sand": plain_sand = true
		elif a == "--no-props": no_props = true
		elif a.begins_with("--boat="): boat_speed = float(a.substr(7))
		elif a.begins_with("--splash="):
			var sp := a.substr(9).split(",")
			splash_z = float(sp[0])
			if sp.size() > 1:
				splash_strength = float(sp[1])
		elif a.begins_with("--wake-out="): wake_out = a.substr(11)
		elif a.begins_with("--wake="):
			var wkv2 := a.substr(7).split(":", true, 1)
			wake_params[wkv2[0]] = str_to_var(wkv2[1])
		elif a == "--profile": profile = true
		elif a == "--perf": perf = true
		elif a == "--breakers": breakers = true
		elif a == "--lip-debug": lip_debug = true
		elif a == "--hide-lips": hide_lips = true
		elif a.begins_with("--cam-dy="): cam_dy = float(a.substr(9))
		elif a.begins_with("--cam="): cam_set = a.substr(6).split_floats(",")
		elif a.begins_with("--sun-energy="): sun_energy = float(a.substr(13))
		elif a.begins_with("--dive="): dive = float(a.substr(7))
		elif a.begins_with("--sun-pitch="): sun_pitch = float(a.substr(12))
		elif a.begins_with("--shade="):
			var v := a.substr(8).split_floats(",")
			shade = Vector2(v[0], v[1])
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
	sun.rotation_degrees = Vector3(-sun_pitch, -140, 0)
	sun.shadow_enabled = true
	sun.light_energy = sun_energy
	if view == "boat_sun":
		# Low, ahead of the camera: the glitter path runs down the wake and lights up its waves.
		sun.look_at_from_position(Vector3.ZERO, Vector3(36.0, -10.0, 28.0))
	elif view == "storm_roller":
		# 20 degrees up, from downwind: the light falls on the crests' front faces, side on to the camera.
		var wdr := Vector2(-0.3, 1.0).normalized()
		sun.look_at_from_position(Vector3.ZERO, Vector3(-wdr.x, -tan(deg_to_rad(20.0)), -wdr.y))
	add_child(sun)

	# Sand: a 200 m slab sloping down away from the camera, from dry beach out to about 9 m of water, with a few rocks.
	var sand_mat := StandardMaterial3D.new()
	sand_mat.albedo_texture = _checker(Color(0.78, 0.7, 0.55), Color(0.62, 0.55, 0.42))
	sand_mat.uv1_scale = Vector3(500, 500, 1)
	if plain_sand:
		# Sand without the checker (for shots): soft noise between two sand tones.
		var nt := NoiseTexture2D.new()
		nt.noise = FastNoiseLite.new()
		nt.noise.frequency = 0.05
		nt.seamless = true
		var ramp := Gradient.new()
		ramp.set_color(0, Color(0.66, 0.59, 0.46))
		ramp.set_color(1, Color(0.8, 0.73, 0.58))
		nt.color_ramp = ramp
		sand_mat.albedo_texture = nt
		sand_mat.uv1_scale = Vector3(200, 200, 1)
	sand_mat.roughness = 0.95
	if contact and ClassDB.class_exists("WeatherMap3D"):
		sand_mat.set("wetness_enabled", true)
		sand_mat.set("wetness_porosity", 0.9)
		weather = ClassDB.instantiate("WeatherMap3D")
		weather.set("rain_rate", 0.0)
		weather.set("drive_environment_fog", false)
		add_child(weather)
	if slope_deg < 0.0:
		# Under water the floor needs to fall away faster, for depth to look through.
		slope_deg = 12.0 if view in ["submerged", "waterline", "up"] else 5.0
	var slope := deg_to_rad(slope_deg)
	if boat_speed > 0.0 or view.begins_with("storm"):
		pass # open sea: no floor
	else:
		# Wide enough that its far edge (a straight colour line through clear water) stays out of every view.
		_slab(Vector3(1000, 1, 1000), Vector3(0, -1.2, 0), Vector3(-slope, 0, 0), sand_mat)
	var rock_mat := StandardMaterial3D.new()
	rock_mat.albedo_color = Color(0.3, 0.3, 0.32)
	rock_mat.roughness = 0.8
	var storm := view.begins_with("storm") or no_props
	for p in [] if storm else [Vector3(-2.5, -1.4, -4.0), Vector3(1.5, -1.9, -7.0), Vector3(3.0, -0.9, -1.5)]:
		_slab(Vector3(1.2, 1.2, 1.2), p, Vector3(0.3, 0.7, 0.2), rock_mat)
	if shade.x != INF:
		# Under the sun at the shade point, 12 m up the light's ray.
		var d := -sun.global_transform.basis.z
		var top := Vector3(shade.x, 0.0, shade.y) - d * (12.0 / -d.y)
		_slab(Vector3(60, 0.4, 5), top, Vector3.ZERO, rock_mat)
	# A rock standing out of the water, for a current to go round.
	if not storm:
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
	if view.begins_with("storm"):
		# Deep open sea in a gale (Beaufort 9): the spectrum's depth must be real, or TMA holds the swell down.
		water.set("depth", 80.0)
		water.set("wind_speed", 22.0)
		water.set("wave_cascades", 4)
		water.set("fetch", 500000.0)
		water.set("swell", 0.4)
		water.set("render_extent", 3000.0)
		if view == "storm_low":
			water.set("wind_speed", 20.0)
		elif view == "storm_foam" or view == "storm_plume" or view == "storm_roller":
			water.set("wind_speed", 13.0) # Beaufort 6: whitecaps everywhere, no gale streaks yet
		elif view == "storm_glitter":
			water.set("wind_speed", 8.0)
			water.set("wave_cascades", 3)
	if view == "sea_level":
		water.set("render_extent", 3000.0)
		water.set("depth", 80.0)
		water.set("wind_speed", 5.0)
		water.set("swell", 0.0)
		water.set("wave_cascades", 3)
		water.set("fetch", 500000.0)
	if view == "waterline":
		water.set("wave_amplitude", 0.3) # small waves, so the surface stays across the lens
	for k in node_params:
		water.set(k, node_params[k])
	add_child(water)
	var wm: ShaderMaterial = water.call("get_water_material")
	for k in water_params:
		if wm: wm.set_shader_parameter(k, water_params[k])
	for a in OS.get_cmdline_user_args():
		if wm and a.begins_with("--dump-shader="):
			var f := FileAccess.open(a.substr(14), FileAccess.WRITE)
			f.store_string(wm.shader.code)
			f.close()
		elif wm and a.begins_with("--shader-file="):
			var sh := Shader.new()
			sh.code = FileAccess.get_file_as_string(a.substr(14))
			wm.shader = sh

	# Floating boxes.
	for i in 0 if storm else 3:
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

	if boat_speed > 0.0:
		_add_boat()
	if wader_on:
		_add_wader()

	var cam := Camera3D.new()
	cam.fov = 45
	add_child(cam)
	match view:
		"shallows":
			cam.look_at_from_position(Vector3(0, 3.0, 8.0), Vector3(0.0, -1.0, -4.0))
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
		"surf":
			cam.look_at_from_position(Vector3(10.0, 2.2, 9.0), Vector3(-2.0, -0.6, -6.0))
		"lip":
			# From the shallows, low, looking back at the breakers as they curl toward the camera.
			cam.look_at_from_position(Vector3(-15.0, 0.6, 5.0), Vector3(-19.0, 0.0, 0.5))
		"curl":
			# Side on to the breakers, along their crests, where they plunge.
			cam.look_at_from_position(Vector3(-10.0, 1.8, -1.0), Vector3(2.0, -0.2, -2.0))
		"top":
			cam.look_at_from_position(Vector3(0.0, 40.0, -6.0), Vector3(0.0, 0.0, -6.01))
		"runup":
			# The waterline from the dry sand (it lies near z = 8 at the default slope), for --splash=6.
			cam.look_at_from_position(Vector3(7.0, 2.5, 15.0), Vector3(0.0, -0.3, 6.5))
		"wader":
			cam.look_at_from_position(Vector3(4.0, 1.4, 5.5), Vector3(1.5, -0.2, 2.0))
		"sea":
			cam.look_at_from_position(Vector3(0, 4.0, -40.0), Vector3(0, -1.0, -110.0))
		"boat", "boat_low", "boat_sun":
			pass # follows the boat (_process)
		"storm":
			cam.look_at_from_position(Vector3(0, 7.0, 0.0), Vector3(-15.0, 2.0, -80.0))
		"storm_low":
			# Low, looking downwind: the streaks run off toward the horizon, the crests' spume blows away from us.
			var wd := Vector2(-0.3, 1.0).normalized()
			cam.look_at_from_position(Vector3(0, 3.5, 0.0), Vector3(wd.x * 80.0, 1.0, wd.y * 80.0))
		"storm_sun":
			# Into the light, low: the thin crests between camera and sun.
			var to_sun := sun.transform.basis.z
			cam.look_at_from_position(Vector3(0, 9.0, 0.0), Vector3(to_sun.x, 0.0, to_sun.z).normalized() * 80.0 + Vector3(0, 3.0, 0))
		"storm_foam":
			# 8 m up, 25 degrees down, the sun to the side: whitecaps and the lace they leave.
			var to_sun_f := sun.transform.basis.z
			var side := Vector3(to_sun_f.z, 0.0, -to_sun_f.x).normalized()
			cam.look_at_from_position(Vector3(0, 8.0, 0.0), side * 17.0)
		"storm_plume":
			# 4 m up, steeply down on the water in front of the storm_foam view: a fresh whitecap and its plume.
			var to_sun_p := sun.transform.basis.z
			var side_p := Vector3(to_sun_p.z, 0.0, -to_sun_p.x).normalized()
			var right_p := side_p.cross(Vector3.UP)
			cam.look_at_from_position(side_p * 4.0 - right_p * 0.5 + Vector3(0, 4.0, 0), side_p * 8.5 + right_p * 0.5)
		"storm_roller":
			# 1.75 m above the mean sea, looking across the wind: spilling crests stand in profile, their rollers
			# against the darker wave faces behind.
			var wdc := Vector2(-0.3, 1.0).normalized()
			var across := Vector3(wdc.y, 0.0, -wdc.x)
			cam.look_at_from_position(Vector3(0, 1.75, 0.0), across * 18.0 + Vector3(0, 0.2, 0))
		"sea_level":
			# Eye 1 m above the mean sea, looking across a moderate sea at the low sun: the near crests stand in
			# silhouette against the sky and the glitter.
			var to_sun_l := sun.transform.basis.z
			var fwd := Vector3(to_sun_l.x, 0.0, to_sun_l.z).normalized()
			cam.look_at_from_position(Vector3(0, 1.0, 0.0), fwd * 30.0 + Vector3(0, 0.8, 0))
		"storm_glitter":
			# 2 m up, into the sun's glitter path.
			var to_sun_g := sun.transform.basis.z
			cam.look_at_from_position(Vector3(0, 2.0, 0.0), Vector3(to_sun_g.x, 0.0, to_sun_g.z).normalized() * 40.0)
		"sea_top":
			cam.look_at_from_position(Vector3(0, 90.0, -110.0), Vector3(0, 0.0, -110.01))
		_:
			cam.look_at_from_position(Vector3(0, 4.5, 12.0), Vector3(0, -0.5, -6.0))


	cam.position.y += cam_dy
	if cam_set.size() == 6:
		cam.look_at_from_position(Vector3(cam_set[0], cam_set[1], cam_set[2]), Vector3(cam_set[3], cam_set[4], cam_set[5]))
	bench_cam = cam


# A hull crossing open sea along +X, held at the waterline at a steady speed (no buoyancy: the wake alone).
func _add_boat() -> void:
	boat = RigidBody3D.new()
	boat.mass = 2000.0
	boat.gravity_scale = 0.0
	boat.axis_lock_angular_x = true
	boat.axis_lock_angular_y = true
	boat.axis_lock_angular_z = true
	boat.axis_lock_linear_y = true
	var cs := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(2.4, 1.0, 8.0)
	cs.shape = box
	boat.add_child(cs)
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(2.4, 1.2, 8.0)
	mi.mesh = bm
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.75, 0.2, 0.15)
	mi.material_override = m
	mi.position.y = 0.3
	boat.add_child(mi)
	# Local +Z is the bow: face +X.
	boat.rotation.y = PI * 0.5
	boat.position = Vector3(-70.0, 0.0, -110.0)
	add_child(boat)
	wake = ClassDB.instantiate("PhysXWaterWake3D")
	wake.set("water_surface_path", NodePath("../../" + str(water.name)))
	wake.set("hull_length", 8.0)
	wake.set("hull_beam", 2.4)
	wake.set("hull_profile", PackedFloat32Array([0.15, 0.6, 0.9, 1.0, 1.0, 1.0, 0.95, 0.85]))
	wake.set("source_count", 9)
	wake.set("reference_speed", boat_speed)
	for k in wake_params:
		wake.set(k, wake_params[k])
	boat.add_child(wake)


func _add_wader() -> void:
	wader = AnimatableBody3D.new()
	wader.sync_to_physics = false
	var lm := StandardMaterial3D.new()
	lm.albedo_color = Color(0.25, 0.22, 0.2)
	for sx in [-0.15, 0.15]:
		var lcs := CollisionShape3D.new()
		var cap := CapsuleShape3D.new()
		cap.radius = 0.12
		cap.height = 1.4
		lcs.shape = cap
		lcs.position = Vector3(0.0, 0.7, sx)
		wader.add_child(lcs)
		var lmi := MeshInstance3D.new()
		var cm := CapsuleMesh.new()
		cm.radius = 0.12
		cm.height = 1.4
		lmi.mesh = cm
		lmi.material_override = lm
		lmi.position = lcs.position
		wader.add_child(lmi)
	# On the sand at z = 2 (its top is at -0.7 at z = 0 and rises to the waterline near z = 8): half a metre deep.
	wader.position = Vector3(-6.0, -0.7 + 2.0 * tan(deg_to_rad(5.0)), 2.0)
	add_child(wader)
	if wader_push:
		wader.add_child(ClassDB.instantiate("PhysXWaterPusher3D"))


func _physics_process(delta: float) -> void:
	if wader != null:
		wader.position.x = minf(wader.position.x + 1.5 * delta, 4.0)
	if boat != null:
		boat.linear_velocity = Vector3(boat_speed, 0.0, 0.0)
		boat.angular_velocity = Vector3.ZERO


# --wake-out: the wake around the boat from sample_height(), and the Kelvin measures.
func _save_wake() -> void:
	var dx := 0.25
	var behind := 56.0 # m behind the bow
	var side := 24.0
	var w := int(behind / dx)
	var hgt := int(2.0 * side / dx)
	var bow := boat.global_position.x + 4.0
	var z0 := boat.global_position.z
	var h := PackedFloat32Array()
	h.resize(w * hgt)
	var s2 := 0.0
	for j in hgt:
		for i in w:
			var v := float(wake.call("sample_height", Vector3(bow + 4.0 - i * dx, 0.0, z0 - side + (j + 0.5) * dx)))
			h[j * w + i] = v
			s2 += v * v
	var sigma := sqrt(s2 / (w * hgt))
	var img := Image.create(w, hgt, false, Image.FORMAT_L8)
	for j in hgt:
		for i in w:
			img.set_pixel(i, j, Color.from_hsv(0, 0, clampf(0.5 + h[j * w + i] / (6.0 * sigma), 0.0, 1.0)))
	img.save_png(wake_out)
	# Wedge: at each distance behind the bow, the outermost offset each side where the wave envelope (|h| max
	# over 1 m along x) still reaches a quarter of its peak across that side: the cusp line bounding the waves
	# (outside it they die away). Half-angle = atan(offset / distance), median over 16..48 m.
	var angles := PackedFloat32Array()
	for xb in range(16, 49, 2):
		for sgn in [-1, 1]:
			var envs := PackedFloat32Array()
			var top := 0.0
			for jj in int(side / dx):
				var j := int((side + sgn * (jj + 0.5) * dx) / dx)
				var env := 0.0
				for di in 4:
					var i := int((xb + 4.0) / dx) + di
					env = maxf(env, absf(h[j * w + i]))
				envs.push_back(env)
				top = maxf(top, env)
			var edge_y := 0.0
			for jj in envs.size():
				if envs[jj] >= 0.25 * top:
					edge_y = (jj + 0.5) * dx
			angles.push_back(rad_to_deg(atan2(edge_y, float(xb))))
	angles.sort()
	# Transverse wavelength: zero up-crossings of h along the track (offset 0), 8..50 m behind the bow.
	var ups := PackedFloat32Array()
	var jc := int(side / dx)
	for i in range(int(12.0 / dx), int(54.0 / dx)):
		var a := h[jc * w + i]
		var b := h[jc * w + i + 1]
		if a < 0.0 and b >= 0.0:
			ups.push_back(i * dx)
	var lam := 0.0
	if ups.size() >= 2:
		lam = (ups[ups.size() - 1] - ups[0]) / (ups.size() - 1)
	print("WAKE U=%.1f m/s dispersive %s: wedge half-angle (outer edge) median %.1f deg (quartiles %.1f..%.1f; Kelvin 19.47), transverse wavelength %.1f m (theory %.1f), rms %.3f m, peak %.3f m" % [
		boat_speed, str(wake.get("dispersive")), angles[angles.size() / 2], angles[angles.size() / 4], angles[3 * angles.size() / 4],
		lam, TAU * boat_speed * boat_speed / 9.81, sigma, _peak(h)])


func _peak(h: PackedFloat32Array) -> float:
	var m := 0.0
	for v in h:
		m = maxf(m, absf(v))
	return m


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
	if water != null and view.begins_with("storm") and n % 10 == 0 and water.has_method("get_plunging_lip_count"):
		print("frame %d plunging lips %d, crest spray %.0f/s" % [n, water.call("get_plunging_lip_count"), water.call("get_crest_spray_rate")])
		for l in water.call("get_plunging_lip_list"):
			print("  lip at %s dir %s height %.2f half %.2f age %.2f/%.2f" % [l.position, l.direction, l.height, l.half_length, l.age, l.life])
	if weather != null and n % 60 == 0:
		var line := ""
		for z in [4.0, 6.0, 7.0, 8.0, 8.5, 9.0, 9.5, 10.0, 11.0, 13.0]:
			line += " z%.1f=%.2f" % [z, weather.call("get_contact_wetness_at", Vector3(0.0, 0.0, z))]
		print("frame %d contact wetness up the beach:%s" % [n, line])
	if boat != null:
		var bp := boat.global_position
		if view == "boat":
			bench_cam.look_at_from_position(Vector3(bp.x - 20.0, 70.0, bp.z), Vector3(bp.x - 20.0, 0.0, bp.z + 0.01))
		elif view == "boat_sun":
			# Ahead and to the side, looking back along the wake toward the sun: the waves catch its glint.
			bench_cam.look_at_from_position(Vector3(bp.x + 14.0, 8.0, bp.z + 22.0), Vector3(bp.x - 22.0, 0.0, bp.z - 6.0))
		elif view == "boat_low":
			bench_cam.look_at_from_position(Vector3(bp.x - 34.0, 9.0, bp.z + 16.0), Vector3(bp.x - 8.0, 0.0, bp.z))
		if wake_out != "" and n == frames - 1:
			_save_wake()
			if out == "":
				get_tree().quit()
	if pt_from > 0 and n == pt_from:
		bench_env.pathtracing_enabled = true
	_tracers_step(1.0 / 30.0)
	if hide_lips:
		for c in water.get_children(true):
			if c.name == "BreakerLips" or c.name == "OceanLips":
				c.visible = false
	if lip_debug:
		for c in water.get_children(true):
			if c.name == "BreakerLips" or c.name == "OceanLips":
				var dm := StandardMaterial3D.new()
				dm.albedo_color = Color(0.9, 0.15, 0.1)
				dm.cull_mode = BaseMaterial3D.CULL_DISABLED
				c.material_override = dm
				if n == frames and c.mesh and c.mesh.get_surface_count() > 0:
					var arr: Array = c.mesh.surface_get_arrays(0)
					var vs: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
					var cols: PackedColorArray = arr[Mesh.ARRAY_COLOR]
					# The ring (18 vertices) with the longest lip: outer sheet, then each point against the surface under it.
					var best := 0
					var best_len := 0.0
					for r in range(0, vs.size() / 18):
						var l := vs[r * 18].distance_to(vs[r * 18 + 8])
						if l > best_len:
							best_len = l
							best = r
					for j in 18:
						var v: Vector3 = water.to_global(vs[best * 18 + j])
						print("RING %d x=%.2f y=%.3f z=%.2f surf=%.3f white=%.2f" % [j, v.x, v.y, v.z, float(water.call("sample_height", v)), cols[best * 18 + j].r])
					var v0: Vector3 = water.to_global(vs[best * 18])
					var v8: Vector3 = water.to_global(vs[best * 18 + 8])
					var d := Vector3(v8.x - v0.x, 0, v8.z - v0.z).normalized()
					var line := "PROFILE"
					for i in 25:
						var q := v0 + d * (-1.2 + 0.1 * i)
						line += " %.1f:%.3f" % [-1.2 + 0.1 * i, float(water.call("sample_height", q))]
					print(line)
	if breakers and water.has_method("get_breaker_stats"):
		var bs: Dictionary = water.call("get_breaker_stats")
		breaker_ms += float(bs["cpu_ms"])
		if breaker_peak.is_empty() or int(bs["vertices"]) > int(breaker_peak["vertices"]):
			breaker_peak = bs
		if n == frames - 1:
			print("BREAKERS mean cpu %.3f ms per tick; peak %s" % [breaker_ms / maxi(n, 1), str(breaker_peak)])
	if perf and n > 60 and n < frames - 1:
		perf_proc += Performance.get_monitor(Performance.TIME_PROCESS)
		perf_phys += Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)
		perf_n += 1
	if perf and n == frames - 1:
		var t0 := Time.get_ticks_usec()
		for i in 2000:
			water.call("sample_height", Vector3(-10.0 + 0.01 * i, 0.0, -5.0 + 0.005 * i))
		var per_call := float(Time.get_ticks_usec() - t0) / 2000.0
		print("PERF process %.3f ms, physics %.3f ms per frame, sample_height %.2f us per call" % [1000.0 * perf_proc / maxi(perf_n, 1), 1000.0 * perf_phys / maxi(perf_n, 1), per_call])
	if splash_z < 1e8 and n % 60 == 10:
		for sx in [-4.0, 0.0, 4.0]:
			water.call("submit_impulse", Vector3(sx, 0.0, splash_z), 2.0, splash_strength)
	if profile and n == frames - 1:
		# Depth and surface along x = 0 toward the beach (sample_height includes the shore waves).
		var line := ""
		for i in 41:
			var z := -20.0 + i
			var h := float(water.call("sample_height", Vector3(0.0, 0.0, z)))
			line += "z=%d h=%.2f  " % [z, h]
		print(line)
	if height_out != "" and n == frames - 1:
		_save_heights()
	if not whitecap_winds.is_empty():
		_whitecap_step()
	if view.begins_with("storm") and (n == frames - 1 or (debug_storm and n % 5 == 0)):
		print("storm: crest spray %.0f droplets/s, whitecap cover %.3f (target %.3f)" % [water.call("get_crest_spray_rate"), water.call("get_whitecap_cover"), water.call("get_whitecap_target_cover")])
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
		if bake_time > 0:
			var keep: bool = ProjectSettings.get_setting("physics/physx_3d/water/multigrid_flow")
			for mg in [true, false]:
				ProjectSettings.set_setting("physics/physx_3d/water/multigrid_flow", mg)
				water.call("rebake_flow")
				var t0 := Time.get_ticks_usec()
				for i in bake_time:
					water.call("rebake_flow")
				print("flow bake (%s, grid %d): %.2f ms" % ["multigrid" if mg else "SOR", water.get("grid_resolution"), (Time.get_ticks_usec() - t0) / 1000.0 / bake_time])
			ProjectSettings.set_setting("physics/physx_3d/water/multigrid_flow", keep)
			water.call("rebake_flow")
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
	if out != "" and seq > 0 and n >= frames and n < frames + seq * seq_step:
		if (n - frames) % seq_step == 0:
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%04d.png" % ((n - frames) / seq_step))
		return
	if out != "" and n >= frames + seq * seq_step:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		for b in get_children():
			if b is RigidBody3D:
				print("box at ", b.global_position, " flow there ", water.call("sample_flow", b.global_position))
		get_tree().quit()


# --whitecaps: hold each wind speed, average the second half of the hold.
func _whitecap_step() -> void:
	var i := n / whitecap_hold
	var k := n % whitecap_hold
	if i >= whitecap_winds.size():
		get_tree().quit()
		return
	if k < 5:
		water.call("set_wind_speed", whitecap_winds[i]) # again over a few frames: the first ones come before the solver is up
	if k == 0:
		whitecap_sum = Vector3.ZERO
		whitecap_count = 0
	var cover := float(water.call("get_whitecap_cover"))
	if k >= whitecap_hold / 2 and cover >= 0.0:
		whitecap_sum += Vector3(float(water.call("get_whitecap_target_cover")), cover, float(water.call("get_effective_foam_threshold")))
		whitecap_count += 1
	if k == whitecap_hold - 1:
		var m := whitecap_sum / maxi(whitecap_count, 1)
		print("WHITECAPS U=%.1f m/s target %.3f%% cover %.3f%% threshold %.3f (auto %s)" % [whitecap_winds[i], 100.0 * m.x, 100.0 * m.y, m.z, str(water.get("whitecap_auto"))])

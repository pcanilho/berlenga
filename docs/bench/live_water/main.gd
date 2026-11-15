extends Node3D

# PhysXLiveWater3D bench: a hillside with a hollow and a gully, water poured in by a spring or held by a dam
# that breaks. Raster or path traced. Run with --fixed-fps 30.
#
#   --scene=spring  a spring high on the slope runs down the gully into the hollow, fills it and spills over (default)
#   --scene=dam     a reservoir behind a wall across the gully; the wall goes at --break=FRAME (default 30)
#   --scene=stream  240 m of hillside, a 48 m streamed area following a marker: the spring runs for 20 s, stops,
#                   the marker goes 100 m away and comes back; prints the water awake, asleep and lost
#   --scene=rain    heavy rain over the whole slope (--rain=MM_PER_HOUR, default 400)
#   --view=overview high, looking down the slope (default); --view=low from the hollow's edge, looking up the gully
#   --cell=M        cell size (default 0.5)
#   --frames=N      frames before --out is saved (default 300)
#   --out=path.png  save the last frame and quit
#   --raster        raster only
#   --spp=N         path tracing samples (default 8)
#   --wet           a WeatherMap3D (dry weather) and wetness on the ground: where the water ran stays dark
#                   after it has gone, then dries (--dry=S, contact_wetness_dry_time, default 60)
#   --stop=FRAME    the spring stops and the water soaks away fast from this frame (to watch the ground dry)
#   --pt-from=N     rasterize until frame N, path trace from then on
#   --seq=N --seq-step=K  also save N frames after --frames, every Kth, as <out>_0000.png ...
# Prints volume, wet cells and the solver's cost every 30 frames.

var SIZE := 80.0
var RES := 161 # height samples per side, 0.5 m apart
var tracker: Node3D

var scene_name := "spring"
var view := "overview"
var cell := 0.5
var frames := 300
var out := ""
var raster := false
var spp := 8
var rain := 400.0
var break_frame := 30
var seq := 0
var wet := false
var dry_time := 60.0
var stop_frame := -1
var pt_from := -1
var env: Environment
var seq_step := 1
var n := 0
var water: Node3D
var dam: StaticBody3D

func ground_h(x: float, z: float) -> float:
	# A hillside falling toward -z, a gully winding down it and a hollow at its foot.
	var h := 0.18 * (z + 40.0)
	var gully_x := 6.0 * sin(z * 0.09)
	h -= 2.2 * exp(-pow((x - gully_x) / 3.5, 2.0))
	h -= 4.0 * exp(-(pow(x - 2.0, 2.0) + pow(z + 22.0, 2.0)) / 90.0)
	# Small uneven ground, so the water finds its own way and pools.
	h += 0.12 * sin(x * 0.41 + 1.3) * sin(z * 0.37) + 0.08 * sin(x * 1.13 - z * 0.77) * cos(z * 0.9 + x * 0.3)
	return h

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--scene=stream":
			SIZE = 240.0
			RES = 241
		if a.begins_with("--scene="): scene_name = a.substr(8)
		elif a.begins_with("--view="): view = a.substr(7)
		elif a.begins_with("--cell="): cell = float(a.substr(7))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--rain="): rain = float(a.substr(7))
		elif a.begins_with("--break="): break_frame = int(a.substr(8))
		elif a.begins_with("--seq="): seq = int(a.substr(6))
		elif a == "--wet": wet = true
		elif a.begins_with("--dry="): dry_time = float(a.substr(6))
		elif a.begins_with("--stop="): stop_frame = int(a.substr(7))
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--seq-step="): seq_step = maxi(int(a.substr(11)), 1)

	env = Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and pt_from < 0
	env.pathtracing_samples_per_pixel = spp
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 150.0
	sun.look_at_from_position(Vector3.ZERO, Vector3(-0.5, -0.75, -0.6))
	add_child(sun)

	# The ground: a height map (physics) and the same grid drawn.
	var data := PackedFloat32Array()
	data.resize(RES * RES)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step := SIZE / (RES - 1)
	for j in RES:
		for i in RES:
			var x := -SIZE * 0.5 + i * step
			var z := -SIZE * 0.5 + j * step
			data[j * RES + i] = ground_h(x, z)
	for j in RES - 1:
		for i in RES - 1:
			var p := []
			for c in [Vector2i(0, 0), Vector2i(1, 0), Vector2i(0, 1), Vector2i(1, 1)]:
				var ii: int = i + c.x
				var jj: int = j + c.y
				p.append(Vector3(-SIZE * 0.5 + ii * step, data[jj * RES + ii], -SIZE * 0.5 + jj * step))
			for t in [[0, 1, 2], [1, 3, 2]]:
				for k in t:
					st.set_uv(Vector2(p[k].x, p[k].z) * 0.25)
					st.add_vertex(p[k])
	st.generate_normals()
	var gm := MeshInstance3D.new()
	gm.mesh = st.commit()
	var mat := StandardMaterial3D.new()
	var nt := NoiseTexture2D.new()
	nt.noise = FastNoiseLite.new()
	nt.noise.frequency = 0.05
	nt.seamless = true
	var ramp := Gradient.new()
	ramp.set_color(0, Color(0.36, 0.33, 0.26))
	ramp.set_color(1, Color(0.55, 0.5, 0.38))
	nt.color_ramp = ramp
	mat.albedo_texture = nt
	mat.roughness = 0.95
	if wet:
		mat.set("wetness_enabled", true)
		mat.set("wetness_porosity", 0.9)
		if ClassDB.class_exists("WeatherMap3D"):
			var wm3: Node3D = ClassDB.instantiate("WeatherMap3D")
			wm3.set("rain_rate", 0.0)
			wm3.set("drive_environment_fog", false)
			wm3.set("contact_wetness_dry_time", dry_time)
			wm3.set("contact_wetness_size", 96.0) # (the whole slope, round its centre below)
			wm3.set("contact_wetness_resolution", 768)
			add_child(wm3)
			(func(): wm3.set("follow_target", wm3.get_path_to(gm))).call_deferred() # (gm joins the tree below)
	gm.material_override = mat
	add_child(gm)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	var hm := HeightMapShape3D.new()
	hm.map_width = RES
	hm.map_depth = RES
	hm.map_data = data
	cs.shape = hm
	cs.scale = Vector3(step, 1.0, step)
	body.add_child(cs)
	add_child(body)

	if scene_name == "dam":
		# A wall across the gully, 3.5 m high.
		dam = StaticBody3D.new()
		var dcs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(30.0, 6.0, 1.0)
		dcs.shape = box
		dam.add_child(dcs)
		var dmi := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = box.size
		dmi.mesh = bm
		var wm := StandardMaterial3D.new()
		wm.albedo_color = Color(0.45, 0.43, 0.4)
		dmi.material_override = wm
		dam.add_child(dmi)
		dam.position = Vector3(0.0, ground_h(0.0, 5.0) + 0.5, 5.0)
		add_child(dam)

	if not ClassDB.class_exists("PhysXLiveWater3D"):
		push_error("This build has no PhysXLiveWater3D.")
		get_tree().quit(1)
		return
	water = ClassDB.instantiate("PhysXLiveWater3D")
	water.set("size", Vector2(SIZE - 1.0, SIZE - 1.0))
	water.set("cell_size", cell)
	water.set("rain_from_weather", false)
	if scene_name == "stream":
		tracker = Node3D.new()
		add_child(tracker)
		water.set("size", Vector2(48.0, 48.0))
		water.set("streaming", true)
	match scene_name:
		"stream":
			water.set("sources", PackedVector4Array([Vector4(6.0 * sin(20.0 * 0.09), ground_h(6.0 * sin(1.8), 20.0), 20.0, 1.5)]))
		"spring":
			water.set("sources", PackedVector4Array([Vector4(6.0 * sin(30.0 * 0.09), ground_h(6.0 * sin(2.7), 30.0), 30.0, 1.5)]))
		"dam":
			water.set("fill_enabled", true)
			water.set("fill_height", ground_h(0.0, 5.0) + 3.0)
			# Only behind the wall (uphill of z = 5): everything below it starts dry.
			water.set("fill_rect", Rect2(-40.0, 5.5, 80.0, 35.0))
		"rain":
			water.set("rain_rate", rain)
	add_child(water)
	if tracker != null:
		water.set("stream_follow", water.get_path_to(tracker))

	var cam := Camera3D.new()
	cam.fov = 55
	add_child(cam)
	match view:
		"low":
			cam.look_at_from_position(Vector3(10.0, ground_h(10.0, -26.0) + 2.5, -26.0), Vector3(0.0, ground_h(0.0, 0.0), 0.0))
		_:
			if scene_name == "stream":
				cam.look_at_from_position(Vector3(70.0, 70.0, -90.0), Vector3(40.0, 0.0, 0.0))
			else:
				cam.look_at_from_position(Vector3(30.0, 22.0, -46.0), Vector3(0.0, 0.0, -5.0))

var stream_total0 := -1.0

func _physics_process(_delta: float) -> void:
	if tracker != null:
		# 0-600 at the spring's valley (the spring stops at 600), 600-900 out 100 m along +x, back by 1500.
		var t := float(n)
		var x := 0.0
		if t > 600.0 and t <= 900.0:
			x = (t - 600.0) / 300.0 * 100.0
		elif t > 900.0 and t <= 1200.0:
			x = 100.0
		elif t > 1200.0 and t <= 1500.0:
			x = 100.0 - (t - 1200.0) / 300.0 * 100.0
		tracker.position = Vector3(x, 0.0, 0.0)
		if n == 600:
			water.set("sources", PackedVector4Array())
	if n == stop_frame:
		water.set("sources", PackedVector4Array())
		water.set("infiltration_rate", 100000.0) # (about 3 cm a second)
	if scene_name == "dam" and n == break_frame and dam != null:
		dam.queue_free()
		dam = null
		water.call_deferred("update_ground")

func _process(_delta: float) -> void:
	n += 1
	if pt_from >= 0 and n == pt_from:
		env.pathtracing_enabled = true
	if n % 30 == 0:
		if scene_name == "stream":
			var awake: float = water.call("get_total_volume")
			var asleep: float = water.call("get_sleeping_volume")
			var lost: float = water.call("get_volume_out")
			if n == 630:
				stream_total0 = awake + asleep + lost
			print("frame %d: awake %.1f m3, asleep %.1f m3 in %d tiles, lost %.1f m3, total %.2f (after the spring %.2f), %.2f ms a tick" % [n, awake, asleep, water.call("get_sleeping_tile_count"), lost, awake + asleep + lost, stream_total0, water.call("get_cost_ms")])
		else:
			print("frame %d: volume %.1f m3, lost over the edges %.1f m3, %.2f ms a tick" % [n, water.call("get_total_volume"), water.call("get_volume_out"), water.call("get_cost_ms")])
	if out != "":
		if n == frames:
			get_viewport().get_texture().get_image().save_png(out)
			if seq == 0:
				get_tree().quit()
		elif seq > 0 and n > frames and (n - frames) % seq_step == 0:
			var k: int = (n - frames) / seq_step - 1
			get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%04d.png" % k)
			if k >= seq - 1:
				get_tree().quit()

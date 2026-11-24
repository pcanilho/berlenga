extends Node3D
# PhysXFlow3D downwind corridor bench: smoke sources spread far apart in a wind, one Flow grid. Each source cluster
# gets its own corridor swept downwind (PhysXFlow3D._cluster_sources); before, one corridor was swept from the box
# round all of them, so two chimneys 200 m apart reserved every block between them.
#   --sources=N       sources in a row across the wind (default 2)
#   --spacing=M       metres between them (default 200)
#   --wind=W          10 m wind, m/s along +x (default 4)
#   --cell=C          Flow cell size (m; default 0.25, a chimney's smoke at a distance)
#   --blocks=N        the grid's max_blocks (default 16384, so the count is not capped)
#   --secs=S          seconds simulated (default 8)
#   --out=PATH        save a still of the first source at the end (needs a window); --pt path traced
# Prints the grid's active blocks and corridors each second, and the peak.
# Run with --fixed-fps 30 (the bench counts frames as 1/30 s).
# Render: VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a godot --path . --rendering-driver vulkan --resolution 960x540 -- ...

var sources := 2
var spacing := 200.0
var wind := 4.0
var secs := 8.0
var out := ""
var pt := false
var cell := 0.25
var blocks := 16384


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sources="): sources = int(a.substr(10))
		elif a.begins_with("--spacing="): spacing = float(a.substr(10))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--secs="): secs = float(a.substr(7))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--pt": pt = true
		elif a.begins_with("--cell="): cell = float(a.substr(7))
		elif a.begins_with("--blocks="): blocks = int(a.substr(9))
	if not ClassDB.class_exists("PhysXFlow3D"):
		print("PhysXFlow3D missing (built without flow_sdk)")
		get_tree().quit(1)
		return
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 150, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var m: Node3D = ClassDB.instantiate("WeatherMap3D")
	m.extent = 2048.0
	m.resolution = 64
	m.wind_speed = wind
	m.wind_direction = Vector2(1, 0)
	m.cloud_cover = 0.0
	m.transition_time = 0.0
	add_child(m)
	m.sun_light = m.get_path_to(sun)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.5, 0.6, 0.75)
	env.environment.ambient_light_color = Color(0.5, 0.55, 0.6)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.pathtracing_samples_per_pixel = 16
	add_child(env)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(2000, 2000)
	ground.mesh = pm
	add_child(ground)
	var flow: Node3D = ClassDB.instantiate("PhysXFlow3D")
	flow.name = "Smoke"
	flow.cell_size = cell
	flow.max_blocks = blocks
	add_child(flow)
	for i in sources:
		var e: Node3D = ClassDB.instantiate("PhysXFlowEmitter3D")
		e.radius = 0.6
		e.velocity = Vector3(0, 2, 0)
		e.temperature = 1.0
		e.smoke = 1.0
		add_child(e)
		e.global_position = Vector3(0, 0.5, (i - (sources - 1) * 0.5) * spacing)
	var cam := Camera3D.new()
	add_child(cam)
	var first := Vector3(0, 0.5, -(sources - 1) * 0.5 * spacing)
	cam.look_at_from_position(first + Vector3(-6, 6, -22), first + Vector3(8, 5, 0))
	cam.current = true
	var peak := 0
	var t := 0.0
	var next := 1.0
	while t < secs:
		await get_tree().process_frame
		t += 1.0 / 30.0
		if t >= next:
			next += 1.0
			var s: Dictionary = flow.get_stats()
			var b: int = s.get("active_blocks", -1)
			peak = maxi(peak, b)
			print("t=%.0f s  active_blocks=%d  corridors=%s" % [t, b, str(s.get("corridors", "n/a"))])
	print("RESULT sources=%d spacing=%.0f m wind=%.1f m/s peak_active_blocks=%d" % [sources, spacing, wind, peak])
	if out != "":
		if pt: # path traced for the still only (the run itself raster, so it stays quick without a GPU)
			env.environment.pathtracing_enabled = true
			for i in 4:
				await RenderingServer.frame_post_draw
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

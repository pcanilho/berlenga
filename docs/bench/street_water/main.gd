extends Node3D
# Street water bench (StreetWater3D over RoadRelief3D, docs/ideas/street-water.md): a 30 m street, a 1 mm shower that
# runs off into the ruts and gutters, then a tyre (a sphere pressed 2 cm into the road) through the rut puddle at 3 m/s.
# Saves <out>_a_before, _b_during, _c_after, _d_settled (.png, and _map.png: a top view, blue = depth) and prints the
# water's volume (kept to within evaporation), wet share and deepest water.
#
#   --out=prefix     where the shots go (default user://street)
#   --pt             path traced: the water holds still from the tyre's mid-pass while it accumulates; one shot
#   --hidetyre       the tyre's mesh hidden (the water's tyre still runs), for a raster pair with --pt
#   --nowheel        no tyre (a reference run)
#   --bench          GPU cost: no shots; after the shower, the mean frame time over 600 frames with the water running,
#                    then 600 with it held (simulating off), and their difference. Run without --fixed-fps, vsync off.
#   --resolution=N   the water's cells a side (default 512 at 2.5 cm)
#
# Raster shots: --fixed-fps 30.
var t := 0
var road
var water
var cam: Camera3D
var out := ""
var wheel_x := -12.0
const WHEEL_Z := 0.85
const WHEEL_R := 0.32
var mat: ShaderMaterial
var tyre: MeshInstance3D
var pt := false
var bench := false
var resolution := 512
var bench_t0 := 0
var bench_on := 0.0
func _ready():
	var args := OS.get_cmdline_user_args()
	out = "user://street"
	for a in args:
		if a.begins_with("--out="): out = a.substr(6)
		if a.begins_with("--resolution="): resolution = int(a.substr(13))
	bench = "--bench" in args
	var e := Environment.new()
	var sky := Sky.new(); sky.sky_material = ProceduralSkyMaterial.new(); e.sky = sky; e.background_mode = Environment.BG_SKY
	e.ambient_light_source = Environment.AMBIENT_SOURCE_SKY; e.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	e.ssr_enabled = true; e.ssr_max_steps = 128
	pt = "--pt" in args
	if pt:
		e.pathtracing_enabled = true; e.pathtracing_samples_per_pixel = 8; e.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new(); we.environment = e; add_child(we)
	var sun := DirectionalLight3D.new(); sun.rotation_degrees = Vector3(-14, 160, 0); sun.light_energy = 2.5; sun.shadow_enabled = true; add_child(sun)
	road = ClassDB.instantiate("RoadRelief3D")
	road.length = 30.0; road.width = 7.0; road.traffic = 0.8
	road.position = Vector3(-15, 0, 0)
	road.name = "Road"
	mat = ShaderMaterial.new()
	var sh := Shader.new()
	var inc: String = ClassDB.class_call_static("StreetWater3D", "get_shader_include")
	if "--oldinc" in args:
		var a := inc.find("	float damp = w.w;"); var b := inc.find("	vec3 wn =")
		inc = inc.substr(0, a) + """	float puddle = smoothstep(0.0003, 0.0015, w.x);
	float wet = max(w.w * 0.85, smoothstep(0.0, 0.0003, w.x));
	albedo *= mix(1.0, 0.55, wet);
	roughness = mix(roughness, min(roughness, 0.25), wet);
	albedo *= mix(1.0, 0.85, puddle);
	roughness = mix(roughness, 0.03, puddle);
""" + inc.substr(b)
	sh.code = "shader_type spatial;\n" + inc + """
varying vec3 wpos;
void vertex() { wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	vec3 alb = vec3(0.32, 0.31, 0.30);
	float r = 0.8;
	vec3 n = NORMAL;
	street_water_apply(wpos, VIEW_MATRIX, alb, r, n);
	ALBEDO = alb; ROUGHNESS = r; NORMAL = n;
}
"""
	mat.shader = sh
	road.material_override = mat
	add_child(road)
	road.rebuild()
	var focus := Marker3D.new(); focus.name = "Focus"; focus.position = Vector3(-6, 0, 0.5); add_child(focus)
	water = ClassDB.instantiate("StreetWater3D")
	water.name = "Water"
	add_child(water)
	water.height_sources = [water.get_path_to(road)]
	water.follow_path = water.get_path_to(focus)
	water.rain_from_weather = false
	water.max_steps = 8
	water.resolution = resolution
	water.materials = [mat]
	tyre = MeshInstance3D.new(); var cy := CylinderMesh.new(); cy.top_radius = WHEEL_R; cy.bottom_radius = WHEEL_R; cy.height = 0.2
	tyre.mesh = cy; tyre.rotation_degrees = Vector3(90, 0, 0); tyre.visible = false
	var tm := StandardMaterial3D.new(); tm.albedo_color = Color(0.04, 0.04, 0.04); tm.roughness = 0.9; tyre.material_override = tm; add_child(tyre)
	# Things for the puddles to mirror: a row of blocks beyond the far kerb, posts along it.
	var rng := RandomNumberGenerator.new(); rng.seed = 7
	for k in 8:
		var b := MeshInstance3D.new(); var bm := BoxMesh.new(); var hgt := rng.randf_range(4.0, 12.0)
		bm.size = Vector3(rng.randf_range(3.0, 6.0), hgt, 4.0); b.mesh = bm
		b.position = Vector3(-16.0 + k * 5.0, hgt * 0.5 - 0.1, 7.0)
		var bmat := StandardMaterial3D.new(); bmat.albedo_color = Color.from_hsv(rng.randf_range(0.0, 0.12), 0.35, rng.randf_range(0.25, 0.55)); b.material_override = bmat
		add_child(b)
	for k in 6:
		var p := MeshInstance3D.new(); var pm := CylinderMesh.new(); pm.top_radius = 0.06; pm.bottom_radius = 0.08; pm.height = 4.0; p.mesh = pm
		p.position = Vector3(-13.0 + k * 3.0, 2.0, 3.9); add_child(p)
	cam = Camera3D.new(); add_child(cam)
	cam.position = Vector3(-11.0, 1.3, -1.6); cam.look_at(Vector3(-8.0, 0, 0.9)); cam.fov = 55
	Engine.max_fps = 0
	if bench:
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if pt:
		tyre.queue_free() # the cloud's software ray tracer crashes on a moving instance; the water's tyre still runs
		tyre = MeshInstance3D.new()

func _shot(tag: String):
	get_viewport().get_texture().get_image().save_png(out + "_" + tag + ".png")
	var m: Image = water.get_map().get_image()
	if m:
		# depth as blue over damp as grey, top view of the window
		var n := m.get_width()
		var img := Image.create(n, n, false, Image.FORMAT_RGB8)
		var w: Vector4 = water.get_water_window()
		var vol := 0.0; var wet := 0; var maxd := 0.0
		for j in n:
			for i in n:
				# texel of window-relative cell (i, j)
				var cx := int(round(w.x / w.w)) + i; var cz := int(round(w.y / w.w)) + j
				var c := m.get_pixel(posmod(cx, n), posmod(cz, n))
				var d := c.r
				vol += d * w.w * w.w; maxd = maxf(maxd, d)
				if d > 0.0003: wet += 1
				var g := 0.15 + 0.25 * c.a
				var b := clampf(d / 0.015, 0.0, 1.0)
				img.set_pixel(i, j, Color(g * (1.0 - b), g * (1.0 - b) + 0.3 * b, g * (1.0 - b) + b))
		img.save_png(out + "_" + tag + "_map.png")
		print(tag, ": volume l ", vol * 1000.0, " wet% ", 100.0 * wet / float(n * n), " max mm ", maxd * 1000.0, " cost ms ", water.get_cost_ms())

func _bench():
	if t == 2:
		water.rain_rate = 2500.0
	if t == 46:
		water.rain_rate = 0.0
	if t == 120:
		bench_t0 = Time.get_ticks_usec()
	if t == 720:
		bench_on = (Time.get_ticks_usec() - bench_t0) / 600000.0
		water.simulating = false
		bench_t0 = Time.get_ticks_usec()
	if t == 1320:
		var off := (Time.get_ticks_usec() - bench_t0) / 600000.0
		print("[street-water] resolution %d: frame %.3f ms running, %.3f ms held, water %.3f ms (main thread %.3f ms)" % [resolution, bench_on, off, bench_on - off, water.get_cost_ms()])
		get_tree().quit()

func _process(_d):
	t += 1
	if bench:
		_bench()
		return
	if t == 2:
		water.rain_rate = 2500.0
	if t == 46:
		water.rain_rate = 0.0
	if t == 230 and not pt:
		_shot("a_before")
	if pt and t == 262:
		water.simulating = false # hold the water still while the path tracer accumulates
	if pt and t == 330:
		_shot("b_during_pt")
	if "--hidetyre" in OS.get_cmdline_user_args() and t == 262:
		_shot("b_during_notyre")
		get_tree().quit()
	if pt and t >= 262:
		return
	if t > 230 and t <= 290 and not ("--nowheel" in OS.get_cmdline_user_args()):
		wheel_x += 3.0 / 30.0
		var hub := Vector3(wheel_x, 0, WHEEL_Z)
		hub.y = road.get_height_at(hub) + WHEEL_R - 0.02
		water.submit_sphere(1, hub, WHEEL_R)
		tyre.visible = not pt and not ("--hidetyre" in OS.get_cmdline_user_args())
		tyre.position = hub
	if t == 262:
		_shot("b_during")
	if t == 291:
		water.clear_sphere(1)
		tyre.visible = false
	if t == 296:
		_shot("c_after")
	if t == 330:
		_shot("d_settled")
		get_tree().quit()

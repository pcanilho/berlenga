extends Node3D
# TerrainDataMap3D checks and cost. A V-shaped valley 256 m across with bumps and a pit that
# Priority-Flood must fill, a river down its floor and a lake at the low end. The ground is a
# HeightMapShape3D (ray path); a tilted plane checks the get_height path and the slope.
#
# godot --path docs/bench/terrain_data --rendering-driver vulkan -- [options]
#   --test            run the checks, print PASS/FAIL, quit
#   --res=N           map resolution (default 256)
#   --maps=dir        save false-colour maps (slope, curvature, flow, wetness, water distance)
#   --out=path.png    render the valley coloured by the maps through the shader globals

const SIZE := 256.0
const LAKE_LEVEL := 0.5
var res := 256
var test := false
var maps_dir := ""
var out := ""
var n := 0
var dm: TerrainDataMap3D
var fails := 0

class PlaneSource extends Node3D:
	func get_height(p: Vector3) -> float:
		return 0.2 * p.x + 3.0

class Lake extends Node3D:
	func get_water_level() -> float:
		return 0.5

class River extends Node3D:
	func sample_height(p: Vector3) -> float:
		if absf(p.x) > 8.0 or p.z < -100.0 or p.z > 20.0:
			return -INF
		return 0.03 * (p.z + 100.0) + 0.3

static func valley_floor(z: float) -> float:
	return 0.03 * (z + 100.0)

static func height(x: float, z: float) -> float:
	# V-shaped valley (sides 0.12, floor 0.03 towards -Z), gentle bumps, and a 0.8 m pit on the
	# floor at z = 40 that Priority-Flood must fill so the flow carries on past it.
	var pit := -0.8 * exp(-(x * x + (z - 40.0) * (z - 40.0)) / 30.0)
	return valley_floor(z) + 0.12 * sqrt(x * x + 25.0) - 0.6 + 0.1 * sin(x * 0.31) * sin(z * 0.27) + pit

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a.begins_with("--res="): res = int(a.substr(6))
		elif a.begins_with("--maps="): maps_dir = a.substr(7)
		elif a.begins_with("--out="): out = a.substr(6)
	# Ground collider.
	var w := int(SIZE) + 1
	var data := PackedFloat32Array()
	data.resize(w * w)
	for z in w:
		for x in w:
			data[z * w + x] = height(x - SIZE * 0.5, z - SIZE * 0.5)
	var shape := HeightMapShape3D.new()
	shape.map_width = w
	shape.map_depth = w
	shape.map_data = data
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	body.add_child(cs)
	add_child(body)
	var lake := Lake.new()
	lake.name = "Lake"
	add_child(lake)
	var river := River.new()
	river.name = "River"
	add_child(river)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = res
	dm.bake_on_ready = false
	dm.water_sources = [NodePath("../Lake"), NodePath("../River")]
	add_child(dm)
	if not out.is_empty():
		_build_view()

func _physics_process(_d: float) -> void:
	n += 1
	if n != 3:
		return
	var err := dm.bake()
	print("TD_BAKE res=%d ms=%.1f err=%d" % [res, dm.get_bake_time_msec(), err])
	if test:
		_checks()
	if not maps_dir.is_empty():
		_save_maps()
	if out.is_empty():
		get_tree().quit(1 if fails > 0 else 0)
	else:
		_render_later()

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

func _checks() -> void:
	var cell := SIZE / res
	# Ray-sampled heights match the analytic ground.
	var hp := dm.get_height_at(Vector3(17.0, 0, 33.0))
	_check("height", absf(hp - height(17.0, 33.0)) < 0.05, hp)
	# Upstream of the river the valley floor collects the whole valley above it. The bumps make
	# the channel wander, so compare the largest value across a band, not one point.
	var f_floor := _band_max(TerrainDataMap3D.CHANNEL_FLOW, -15.0, 15.0, 60.0)
	var f_hill := _band_max(TerrainDataMap3D.CHANNEL_FLOW, 55.0, 70.0, 60.0)
	_check("flow floor > hillside x30", f_floor - f_hill > 1.5, [f_floor, f_hill])
	var f_up := _band_max(TerrainDataMap3D.CHANNEL_FLOW, -15.0, 15.0, 110.0)
	_check("flow grows downstream", f_floor > f_up + 0.3, [f_up, f_floor])
	var w_floor := _band_max(TerrainDataMap3D.CHANNEL_WETNESS, -15.0, 15.0, 60.0)
	var w_hill := _band_max(TerrainDataMap3D.CHANNEL_WETNESS, 55.0, 70.0, 60.0)
	_check("wetness floor > hillside", w_floor > w_hill + 2.0, [w_floor, w_hill])
	# Beside the river the land drains sideways into it: no valley-sized channel there.
	var f_bank := _band_max(TerrainDataMap3D.CHANNEL_FLOW, 10.0, 25.0, -50.0)
	_check("river captures the floor flow", f_bank < f_floor - 1.0, [f_bank, f_floor])
	# Lake: depth at the low end, shoreline where the floor reaches the lake level.
	var d_lake := dm.get_water_depth_at(Vector3(0.0, 0, -120.0))
	var expect := LAKE_LEVEL - height(0.0, -120.0)
	_check("lake depth", absf(d_lake - expect) < 0.1, [d_lake, expect])
	_check("dry land depth 0", dm.get_water_depth_at(Vector3(60.0, 0, 60.0)) == 0.0, 0)
	# Signed distance: negative in water, positive on land, grows with distance.
	var dist_in := dm.get_water_distance_at(Vector3(0.0, 0, -120.0))
	var dist_far := dm.get_water_distance_at(Vector3(100.0, 0, 100.0))
	var dist_near := dm.get_water_distance_at(Vector3(20.0, 0, -40.0))
	_check("distance signs", dist_in < 0.0 and dist_far > dist_near and dist_near > 0.0, [dist_in, dist_near, dist_far])
	# The river edge: find the first dry cell east of x = 0 at z = -40 and check distance there.
	var x := 0.0
	while x < 40.0 and dm.get_water_depth_at(Vector3(x, 0, -40.0)) > 0.0:
		x += 0.25
	var shore := dm.get_water_distance_at(Vector3(x, 0, -40.0))
	_check("distance ~0 at the river bank", absf(shore) <= cell * 1.5, [x, shore])
	# Curvature: valley floor is a hollow (positive), the ridge line is not.
	_check("curvature hollow > 0", dm.get_curvature_at(Vector3(0.0, 0, 50.0)) > 0.0, dm.get_curvature_at(Vector3(0.0, 0, 50.0)))
	# Slope and aspect on an analytic plane through the get_height path.
	var ps := PlaneSource.new()
	ps.name = "Plane"
	add_child(ps)
	var pm := TerrainDataMap3D.new()
	pm.size = 64.0
	pm.resolution = 64
	pm.bake_on_ready = false
	pm.publish_shader_globals = false
	add_child(pm)
	pm.height_source = pm.get_path_to(ps)
	# Water level 3 m: water where x < 0 on the plane, so distance = x exactly.
	var pl := Lake.new()
	pl.position.y = 2.5
	add_child(pl)
	pm.water_sources = [pm.get_path_to(pl)]
	pm.bake()
	var s := rad_to_deg(pm.get_slope_at(Vector3(5, 0, 5)))
	_check("plane slope 11.31 deg", absf(s - rad_to_deg(atan(0.2))) < 0.1, s)
	var asp := rad_to_deg(pm.get_aspect_at(Vector3(5, 0, 5)))
	_check("plane faces -X (aspect 270)", absf(asp - 270.0) < 0.5, asp)
	var d10 := pm.get_water_distance_at(Vector3(10.0, 0, 5.0))
	_check("distance 10 m from a straight shore", absf(d10 - 10.0) <= 0.5, d10)
	var dm5 := pm.get_water_distance_at(Vector3(-5.0, 0, 5.0))
	_check("distance -5 m under water", absf(dm5 + 5.0) <= 0.5, dm5)
	print("TD_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)

func _band_max(ch: int, x0: float, x1: float, z: float) -> float:
	var best := -INF
	var x := x0
	while x <= x1:
		var v: float
		match ch:
			TerrainDataMap3D.CHANNEL_FLOW: v = dm.get_flow_at(Vector3(x, 0, z))
			_: v = dm.get_wetness_at(Vector3(x, 0, z))
		best = maxf(best, v)
		x += 0.5
	return best

func _save_maps() -> void:
	DirAccess.make_dir_recursive_absolute(maps_dir)
	var specs := {
		"slope": [TerrainDataMap3D.CHANNEL_SLOPE, 0.0, 0.6],
		"curvature": [TerrainDataMap3D.CHANNEL_CURVATURE, -0.05, 0.05],
		"flow": [TerrainDataMap3D.CHANNEL_FLOW, 0.0, 4.0],
		"wetness": [TerrainDataMap3D.CHANNEL_WETNESS, 2.0, 14.0],
		"water_distance": [TerrainDataMap3D.CHANNEL_WATER_DISTANCE, -10.0, 60.0],
	}
	for key in specs:
		var sp: Array = specs[key]
		var src := dm.get_channel_image(sp[0])
		var img := Image.create(src.get_width(), src.get_height(), false, Image.FORMAT_RGB8)
		for y in src.get_height():
			for x in src.get_width():
				var v := clampf((src.get_pixel(x, y).r - sp[1]) / (sp[2] - sp[1]), 0.0, 1.0)
				img.set_pixel(x, y, Color(v, v, v))
		img.save_png(maps_dir.path_join(key + ".png"))
	print("maps saved to ", maps_dir)

# The valley as a mesh, displaced and coloured in the shader from the published globals.
func _build_view() -> void:
	var pm := PlaneMesh.new()
	pm.size = Vector2(SIZE, SIZE)
	pm.subdivide_width = 255
	pm.subdivide_depth = 255
	var sh := Shader.new()
	sh.code = "shader_type spatial;\n" + TerrainDataMap3D.get_shader_code() + """
varying vec4 td;
varying vec4 tw;
void vertex() {
	vec3 wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	td = terrain_data(wp);
	tw = terrain_water(wp);
	VERTEX.y = td.x;
}
void fragment() {
	// Dry grass on slopes, green where wet, mud near water, blue under water.
	vec3 dry = vec3(0.55, 0.5, 0.3);
	vec3 lush = vec3(0.12, 0.35, 0.1);
	vec3 col = mix(dry, lush, smoothstep(6.0, 11.0, tw.y));
	col = mix(col, vec3(0.3, 0.24, 0.16), smoothstep(3.0, 0.0, tw.w));
	col = mix(col, vec3(0.08, 0.2, 0.35), step(0.001, tw.z));
	col = mix(col, vec3(0.45, 0.43, 0.4), smoothstep(0.35, 0.55, td.y));
	ALBEDO = col;
	ROUGHNESS = 0.9;
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = sh
	var mi := MeshInstance3D.new()
	mi.mesh = pm
	mi.material_override = mat
	mi.custom_aabb = AABB(Vector3(-SIZE, -10, -SIZE), Vector3(SIZE * 2, 40, SIZE * 2))
	add_child(mi)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.6, 0.7, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.5, 0.55, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 60, 0)
	add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(95, 55, 120)
	add_child(cam)
	cam.look_at(Vector3(0, 0, -20))

func _render_later() -> void:
	for i in 4:
		await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(1 if fails > 0 else 0)

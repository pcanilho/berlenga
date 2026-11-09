extends Node3D
# River bench: a PhysXRiver3D drawn down a valley with a bend, a rock and a 1.5 m step, a few
# boxes floating down it, raster or path traced. Needs a build with the PhysX module.
#
#   --view=overview  the valley from above and downstream (default)
#   --view=cascade   closer, on the step and the white water below it
#   --view=top       straight down on the cascade, for the foam
#   --view=bend      looking up a bend (with --carve: deep on the outside)
#   --view=rocks     straight down on the rocks: the current parting round them, foam trails behind
#   --raster         raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 16, 3)
#   --frames=N       frames rendered before --out is saved (default 12)
#   --pt-from=N      rasterize until frame N, path trace from then on
#   --out=path.png   save the last frame and quit
#   --seq=N          also save the N frames after --frames (for a GIF)
#   --river=prop:value  a PhysXRiver3D property, e.g. --river=discharge:12.0
#   --water=param:value a river shader parameter
#   --set=setting:value any project setting
#   --debug=N        path tracing debug view (8 = albedo)
#   --carve          no channel in the ground: the river carves its own (the bench applies its
#                    brushes to the ground mesh, as a terrain deformer would)
#   --joins          a lake at the bottom of the valley (a PhysXWaterSurface3D) that the river
#                    flows into, and a tributary joining from the side below the step
#   --view=joins     from above: the tributary's confluence and the lake
#   --view=mouth     the river's mouth and its plume into the lake
#   --live           the live window: shallow-water simulation round the rocks (eddies, live foam)
#   --report         print the cross-sections and the boxes' positions
#
# Run with --fixed-fps 30.

var view := "overview"
var raster := false
var spp := 16
var bounces := 3
var frames := 12
var pt_from := 0
var out := ""
var seq := 0
var report := false
var carve := false
var live := false
var joins := false
var lake: Node3D
var tributary: Node3D
var focus: Marker3D
var ground_body: StaticBody3D
var ground_heights := PackedFloat32Array()
var debug_mode := 0
var river_params := {}
var water_params := {}
var bench_env: Environment
var river: Node3D
var boxes: Array[RigidBody3D] = []
var n := 0


func _valley_x(z: float) -> float:
	return 6.0 * sin(z / 15.0)


# Ground height: a valley falling 0.5% downstream (+z), a 1.5 m step at z = 0, a parabolic channel 6 m wide.
# With --joins: a lake basin from z = 30 down, and a side valley for the tributary.
func _ground(x: float, z: float) -> float:
	var base := -0.005 * z - 1.5 * smoothstep(-0.75, 0.75, z)
	var d := absf(x - _valley_x(z))
	var channel := 1.2 * (1.0 - pow(d / 3.0, 2.0)) if d < 3.0 and not carve else 0.0
	var wall := 3.0
	if joins:
		base -= 3.5 * smoothstep(30.0, 40.0, z)
		wall += 9.0 * smoothstep(28.0, 42.0, z)
	var h := base - channel + minf(maxf(d - wall, 0.0) * 0.4, 3.0)
	if joins:
		h = minf(h, _tributary_ground(x, z))
	return h


const TRIB_A := Vector2(15.0, -26.0)
const TRIB_B := Vector2(3.05, 8.0)


# The tributary's side valley: a straight channel from TRIB_A down to the main river at TRIB_B,
# its bed falling from -0.9 to -2.3 m, 3 m wide, in a V valley.
func _tributary_ground(x: float, z: float) -> float:
	var ab := TRIB_B - TRIB_A
	var t := clampf((Vector2(x, z) - TRIB_A).dot(ab) / ab.length_squared(), 0.0, 1.15)
	var d := (Vector2(x, z) - (TRIB_A + ab * t)).length()
	var bed := lerpf(-0.9, -2.3, t)
	return bed + (0.6 * pow(d / 1.5, 2.0) if d < 1.5 else 0.6 + (d - 1.5) * 0.5)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--report": report = true
		elif a == "--carve": carve = true
		elif a == "--live": live = true
		elif a == "--joins": joins = true
		elif a.begins_with("--debug="): debug_mode = int(a.substr(8))
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--pt-from="): pt_from = int(a.substr(10))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--seq="): seq = int(a.substr(6))
		elif a.begins_with("--river="):
			var rkv := a.substr(8).split(":", true, 1)
			river_params[rkv[0]] = str_to_var(rkv[1])
		elif a.begins_with("--water="):
			var wkv := a.substr(8).split(":", true, 1)
			water_params[wkv[0]] = str_to_var(wkv[1])
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
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_debug_mode = debug_mode
	bench_env = env
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -140, 0)
	sun.shadow_enabled = true
	add_child(sun)

	_build_ground()
	var rock_mat := StandardMaterial3D.new()
	rock_mat.albedo_color = Color(0.3, 0.3, 0.32)
	rock_mat.roughness = 0.8
	# Rocks in the channel above the step, standing out of the water, on their own physics layer (2)
	# so a carving river, which measures the uncarved terrain, still finds them.
	for r in [[-14.0, 0.8, Vector3(1.0, 2.0, 1.4), 0.5], [-24.0, -0.9, Vector3(1.2, 2.0, 1.0), -0.4], [-31.0, 0.3, Vector3(0.8, 2.0, 0.9), 1.1]]:
		var rz: float = r[0]
		var rx: float = _valley_x(rz) + r[1]
		var size: Vector3 = r[2]
		_slab(size, Vector3(rx, _ground(rx, rz) - (1.2 if carve else 0.0) + 0.6, rz), Vector3(0.2, r[3], 0.1), rock_mat, 3)

	if not ClassDB.class_exists("PhysXRiver3D"):
		push_error("This build has no PhysXRiver3D.")
		get_tree().quit(1)
		return
	river = ClassDB.instantiate("PhysXRiver3D")
	var curve := Curve3D.new()
	var z := -48.0
	while z <= 48.0:
		curve.add_point(Vector3(_valley_x(z), _ground(_valley_x(z), z), z))
		z += 4.0
	river.set("curve", curve)
	river.set("default_width", 5.0)
	river.set("discharge", 6.0)
	river.set("step_length", 0.5)
	river.set("cross_segments", 24)
	river.set("obstacle_collision_mask", 2)
	if live:
		focus = Marker3D.new()
		focus.position = Vector3(_valley_x(-22.0), 0.0, -22.0)
		add_child(focus)
		river.set("live_flow", true)
		river.set("live_domain_size", 32.0)
		river.set("live_grid_resolution", 160)
	if carve:
		river.set("carve", true)
		river.set("carve_depth", 1.2)
		river.set("default_width", 6.0)
	for k in river_params:
		river.set(k, river_params[k])
	if joins:
		lake = ClassDB.instantiate("PhysXWaterSurface3D")
		lake.position = Vector3(_valley_x(42.0), 0.0, 42.0)
		lake.set("domain_size", Vector2(32, 16))
		lake.set("grid_resolution", 128)
		lake.set("seabed_from_floor", true)
		lake.set("water_level", -2.35)
		lake.set("ocean_grid_resolution", 64)
		lake.set("wind_speed", 3.0)
		lake.set("wave_amplitude", 0.15)
		lake.set("render_extent", 0.0)
		add_child(lake)
	add_child(river)
	if joins:
		river.set("mouth_target", river.get_path_to(lake))
		tributary = ClassDB.instantiate("PhysXRiver3D")
		var tc := Curve3D.new()
		for k in 13:
			var p2 := TRIB_A.lerp(TRIB_B, k / 12.0)
			tc.add_point(Vector3(p2.x, _ground(p2.x, p2.y), p2.y))
		tributary.set("curve", tc)
		tributary.set("default_width", 3.0)
		tributary.set("discharge", 2.0)
		tributary.set("roughness", 0.065) # a stony creek
		tributary.set("step_length", 0.5)
		tributary.set("cross_segments", 16)
		add_child(tributary)
		tributary.set("mouth_target", tributary.get_path_to(river))
	if live:
		river.set("live_follow", river.get_path_to(focus))
	if carve:
		river.set("base_height_source", river.get_path_to(self))
	await get_tree().physics_frame
	await get_tree().physics_frame
	if carve:
		_apply_brushes(river.call("get_carve_brushes", 64.0))
		await get_tree().physics_frame
		river.call("rebuild")
	var wm: ShaderMaterial = river.get("water_material")
	for k in water_params:
		if wm: wm.set_shader_parameter(k, water_params[k])
	if report:
		for i in range(0, river.call("get_section_count"), 8):
			var s: Dictionary = river.call("get_section", i)
			print("s=%5.1f surface=%6.2f slope=%.3f speed=%.2f foam=%.2f cascade=%s q=%.1f" % [s.s, s.surface, s.slope, s.speed, s.foam, s.cascade, river.call("get_discharge_at", s.s)])
		_print_cross()

	# Floating boxes, dropped upstream.
	for i in 3:
		var body := RigidBody3D.new()
		body.mass = 30.0
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(0.6, 0.3, 0.6)
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
		var bz := -30.0 + 4.0 * i
		var bx := _valley_x(bz) + (i - 1) * 0.6
		body.position = Vector3(bx, river.call("sample_height", Vector3(bx, 0, bz)) + 0.2, bz)
		add_child(body)
		b.set("water_surface_path", b.get_path_to(river))
		boxes.append(body)

	var cam := Camera3D.new()
	cam.fov = 45
	add_child(cam)
	match view:
		"cascade":
			cam.look_at_from_position(Vector3(9.0, 5.0, 10.0), Vector3(_valley_x(0.0), -2.0, 0.0))
		"bend":
			cam.look_at_from_position(Vector3(_valley_x(-23.6) + 2.0, 6.0, -14.0), Vector3(_valley_x(-23.6), -1.5, -23.6))
		"joins":
			cam.look_at_from_position(Vector3(6.0, 62.0, 44.0), Vector3(5.0, -2.0, 18.0))
		"mouth":
			cam.look_at_from_position(Vector3(_valley_x(30.0) - 10.0, 8.0, 20.0), Vector3(_valley_x(38.0), -2.35, 40.0))
		"rocks":
			cam.look_at_from_position(Vector3(_valley_x(-22.0) + 0.5, 16.0, -20.0), Vector3(_valley_x(-22.0), -1.0, -21.99))
		"top":
			cam.look_at_from_position(Vector3(_valley_x(2.0), 14.0, 2.0), Vector3(_valley_x(2.0), -2.0, 2.01))
		_:
			cam.look_at_from_position(Vector3(16.0, 12.0, 30.0), Vector3(0.0, -1.0, -6.0))


# The uncarved ground, for the river (a terrain deformer's base heights).
func base_height_at(p_xz: Vector2) -> float:
	return _ground(p_xz.x, p_xz.y)


# What a terrain deformer does with the river's brushes: lower the ground to them ("lower to"), blended by the mask.
func _apply_brushes(p_brushes: Array) -> void:
	var step := 0.5
	var nx := 160
	for b in p_brushes:
		var rect: Rect2 = b.rect
		var cell: float = b.cell_size
		var size: Vector2i = b.size
		var heights: PackedFloat32Array = b.heights
		var mask: PackedFloat32Array = b.mask
		for j in ground_heights.size() / (nx + 1):
			for i in nx + 1:
				var x := -40.0 + i * step
				var z := -50.0 + j * step
				var cx := int(floor((x - rect.position.x) / cell))
				var cz := int(floor((z - rect.position.y) / cell))
				if cx < 0 or cz < 0 or cx >= size.x or cz >= size.y:
					continue
				var m := mask[cz * size.x + cx]
				if m <= 0.0:
					continue
				var k := j * (nx + 1) + i
				ground_heights[k] = lerpf(ground_heights[k], minf(ground_heights[k], heights[cz * size.x + cx]), m)
	print("carved with %d brushes" % p_brushes.size())
	ground_body.queue_free()
	_build_ground(false)


func _build_ground(p_fresh := true) -> void:
	var step := 0.5
	var nx := 160
	var nz := 200
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	if p_fresh:
		ground_heights.resize((nx + 1) * (nz + 1))
	for j in nz + 1:
		for i in nx + 1:
			var x := -40.0 + i * step
			var z := -50.0 + j * step
			if p_fresh:
				ground_heights[j * (nx + 1) + i] = _ground(x, z)
			verts.push_back(Vector3(x, ground_heights[j * (nx + 1) + i], z))
			uvs.push_back(Vector2(x, z) * 0.25)
	var idx := PackedInt32Array()
	for j in nz:
		for i in nx:
			var k := j * (nx + 1) + i
			idx.append_array([k, k + 1, k + nx + 1, k + 1, k + nx + 2, k + nx + 1])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = idx
	var st := SurfaceTool.new()
	st.create_from_arrays(arrays)
	st.generate_normals()
	var mesh := st.commit()
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = _checker(Color(0.52, 0.48, 0.36), Color(0.42, 0.39, 0.3))
	mat.roughness = 0.95
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	var body := StaticBody3D.new()
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = mat
	body.add_child(mi)
	var cs := CollisionShape3D.new()
	cs.shape = mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)
	ground_body = body


func _checker(a: Color, b: Color) -> ImageTexture:
	var img := Image.create(8, 8, true, Image.FORMAT_RGB8)
	for y in 8:
		for x in 8:
			img.set_pixel(x, y, a if (x / 4 + y / 4) % 2 == 0 else b)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


# Across the river beside the rocks: speed, x velocity, white water, depth, height.
func _print_cross() -> void:
	for zq in [-34.0, -31.0, -28.0, -26.0, -24.0, -22.0, -20.0, -17.0, -14.0, -10.0]:
		var line := "z=%5.1f" % zq
		for k in range(-6, 7):
			var p := Vector3(_valley_x(zq) + 0.5 * k, 0.0, zq)
			var f: Vector3 = river.call("sample_flow", p)
			line += " %4.1f/%+4.1f/%.2f/%+.2f" % [f.length(), f.x, river.call("sample_foam", p), river.call("sample_height", p)]
		print(line)


# The joins: discharge along the main river, the tributary's mouth, the plume in the lake.
func _print_joins() -> void:
	for i in range(0, river.call("get_section_count"), 16):
		var sec: Dictionary = river.call("get_section", i)
		print("main s=%5.1f z=%5.1f surface=%6.2f speed=%.2f q=%.1f" % [sec.s, sec.centre.z, sec.surface, sec.speed, river.call("get_discharge_at", sec.s)])
	var tn: int = tributary.call("get_section_count")
	for i in range(0, tn, 8):
		var sec: Dictionary = tributary.call("get_section", i)
		print("trib s=%5.1f surface=%6.2f speed=%.2f cascade=%s foam=%.2f depths=%s" % [sec.s, sec.surface, sec.speed, sec.cascade, sec.foam, sec.depths])
	for zq in [34.0, 38.0, 42.0, 46.0, 52.0, 58.0]:
		var line := "lake z=%.0f" % zq
		for dx in [-4.0, -2.0, 0.0, 2.0, 4.0]:
			var f: Vector3 = lake.call("sample_flow", Vector3(_valley_x(34.0) + dx, -2.35, zq))
			line += " %+.2f,%+.2f%s" % [f.x, f.z, "" if lake.call("is_wet", Vector3(_valley_x(34.0) + dx, -2.35, zq)) else "d"]
		print(line)


func _slab(p_size: Vector3, p_pos: Vector3, p_rot: Vector3, p_mat: Material, p_layers := 1) -> void:
	var body := StaticBody3D.new()
	body.collision_layer = p_layers
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


func _process(_delta: float) -> void:
	n += 1
	if pt_from > 0 and n == pt_from:
		bench_env.pathtracing_enabled = true
	if report and n % 60 == 0:
		for b in boxes:
			print("frame %d box at %s flow %s" % [n, b.global_position, river.call("sample_flow", b.global_position)])
	if out != "" and seq > 0 and n >= frames and n < frames + seq:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%04d.png" % (n - frames))
		return
	if report and joins and n == frames - 1:
		_print_joins()
	if report and live and n == frames - 1:
		print("live window, frame %d:" % n)
		_print_cross()
	if out != "" and n >= frames + seq:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()
	elif out == "" and n >= frames:
		get_tree().quit()

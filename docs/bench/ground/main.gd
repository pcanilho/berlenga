extends Node3D
# GroundSurface3D bench (docs/ideas/ground-deform.md): footprints in sand, soil, mud and leaf litter.
#
#   --test              print the checks and quit:
#                         boot sinkage by material against Bekker with Wong's values
#                         volume kept through a press and through slumping
#                         dry sand slumps to its repose angle, damp sand keeps steep walls
#                         mud creeps back slowly; an undisturbed 50 degree slope does not move
#                         litter adds to the print; bare rock does not print; turned ground dries
#   --view=trail        a walker, a rolling ball and a wheel cross the ground; --ground=KIND picks it:
#                       dry_sand, damp_sand, soil, mud, litter (default soil)
#   --raster            raster only (default path traced)
#   --out=path.png      save the frame and quit
#   --spp=N --bounces=N path tracing samples and bounces (default 16, 3)

const HeightScript := preload("res://height.gd")

var view := "trail"
var kind := "soil"
var raster := false
var test := false
var out := ""
var spp := 16
var bounces := 3
var fails := 0
var height: Node3D
var surface: GroundSurface3D
var walker: CharacterBody3D
var walk_t := 0.0
var walking := false


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a.begins_with("--ground="): kind = a.substr(9)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
	if test:
		await _run_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	await _build_trail()


func _check(label: String, ok: bool, detail: String) -> void:
	print(("PASS " if ok else "FAIL ") + label + ": " + detail)
	if not ok:
		fails += 1


func _setup(height_kind: String, ground_type: int, moisture: float, litter: float) -> void:
	for c in get_children():
		c.queue_free()
	await get_tree().physics_frame
	height = HeightScript.new()
	height.name = "Height"
	height.kind = height_kind
	add_child(height)
	var probe := Marker3D.new()
	probe.name = "Probe"
	add_child(probe)
	surface = GroundSurface3D.new()
	surface.height_source = NodePath("../Height")
	surface.follow_target = NodePath("../Probe")
	surface.ground_type = ground_type
	surface.moisture = moisture
	surface.litter_cover = litter
	add_child(surface)
	await get_tree().physics_frame


func _apply(kind_name: String) -> void:
	# dry_sand, damp_sand, soil, mud, litter
	match kind_name:
		"dry_sand":
			surface.ground_type = GroundSurface3D.GROUND_SAND
			surface.moisture = 0.0
		"damp_sand":
			surface.ground_type = GroundSurface3D.GROUND_SAND
			surface.moisture = 0.4
		"mud":
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 1.0
		"litter":
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 0.2
			surface.litter_cover = 1.0
		_:
			surface.ground_type = GroundSurface3D.GROUND_SOIL
			surface.moisture = 0.2


# Steepest surface slope (degrees) between neighbouring texels in a square around p.
func _max_slope(p: Vector3, half: float) -> float:
	var t := surface.deform_extent / surface.deform_resolution
	var worst := 0.0
	var x := p.x - half
	while x <= p.x + half:
		var z := p.z - half
		while z <= p.z + half:
			var a := Vector3(x, 0, z)
			var h := surface.get_surface_height_at(a)
			for d in [Vector3(t, 0, 0), Vector3(0, 0, t)]:
				var g := surface.get_surface_height_at(a + d)
				worst = maxf(worst, rad_to_deg(atan(absf(g - h) / t)))
			z += t
		x += t
	return worst


func _run_tests() -> void:
	# Sinkage under a boot: 75 kg on 0.035 m2, 0.108 m wide.
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var o := Vector3(0, 0, 0)
	var dry_sand := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("boot in dry sand", dry_sand > 0.015 and dry_sand < 0.03, "%.1f cm (1.5 to 3)" % (dry_sand * 100.0))
	surface.moisture = 0.4
	var damp := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("damp sand is firmer", damp < dry_sand * 0.6, "%.1f cm" % (damp * 100.0))
	surface.ground_type = GroundSurface3D.GROUND_SOIL
	surface.moisture = 0.0
	var soil := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	surface.moisture = 1.0
	var mud := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("boot in mud", mud > 0.05 and mud < 0.1, "%.1f cm (5 to 10); dry soil %.1f cm" % [mud * 100.0, soil * 100.0])
	surface.moisture = 0.5
	var moist := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("moist soil between", moist > soil and moist < mud, "%.1f cm" % (moist * 100.0))
	surface.moisture = 0.2
	var bare := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	surface.litter_cover = 1.0
	var litter := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("litter adds to the print", litter > bare + 0.02, "%.1f cm on 4 cm of litter against %.1f cm" % [litter * 100.0, bare * 100.0])
	surface.litter_cover = 0.0

	# A boot pressed into dry sand: volume kept, then slumping keeps it too.
	surface.ground_type = GroundSurface3D.GROUND_SAND
	surface.moisture = 0.0
	surface.clear_deformation()
	var moved := surface.press_box(o, Vector2(0.11, 0.29), 0.3, 0.0, dry_sand)
	var v := surface.get_moved_volume()
	var rim_share: float = surface.get_material_at(o)["rim_share"]
	var compacted := v.x - v.y
	_check("press keeps volume", absf(v.y - moved * rim_share) < 0.02 * moved and absf(compacted - moved * (1.0 - rim_share)) < 0.02 * moved,
			"moved %.2f l, rim %.2f l (%.0f%%), compacted %.2f l" % [moved * 1000.0, v.y * 1000.0, 100.0 * v.y / maxf(moved, 1e-9), compacted * 1000.0])
	# A deep round print (a dropped ball) in dry sand: walls collapse to the repose angle.
	surface.clear_deformation()
	surface.press(o, 0.15, -0.12, 0.0, true)
	var before := _max_slope(o, 0.3)
	var v0 := surface.get_moved_volume()
	surface.settle(3.0)
	var after := _max_slope(o, 0.3)
	var v1 := surface.get_moved_volume()
	_check("dry sand slumps to its repose angle", before > 45.0 and after < 36.0, "steepest wall %.0f before, %.0f degrees after 3 s (33 + a texel's rounding)" % [before, after])
	_check("slumping keeps volume", absf((v1.x - v1.y) - (v0.x - v0.y)) < 1e-4, "net hollow %.3f l before, %.3f l after" % [(v0.x - v0.y) * 1000.0, (v1.x - v1.y) * 1000.0])
	# Damp sand keeps steep walls (Hornbaker et al. 1997).
	surface.clear_deformation()
	surface.moisture = 0.4
	surface.press(o, 0.15, -0.12, 0.0, true)
	surface.settle(3.0)
	var damp_wall := _max_slope(o, 0.3)
	_check("damp sand keeps steep walls", damp_wall > 60.0, "steepest wall %.0f degrees after 3 s" % damp_wall)
	# Mud creeps back slowly.
	surface.clear_deformation()
	surface.ground_type = GroundSurface3D.GROUND_SOIL
	surface.moisture = 1.0
	surface.press(o, 0.15, -0.12, 0.0, true)
	var mud0 := _max_slope(o, 0.3)
	var fresh0 := surface.get_fresh_at(o)
	surface.settle(10.0, 0.1)
	var mud10 := _max_slope(o, 0.3)
	surface.settle(900.0, 0.5)
	var mud900 := _max_slope(o, 0.3)
	_check("mud creeps back slowly", mud10 > 45.0 and mud900 < mud10 - 10.0, "steepest wall %.0f, %.0f after 10 s, %.0f degrees after 15 min" % [mud0, mud10, mud900])
	# Turned ground dries.
	surface.settle(surface.drying_time * 2.0, 5.0)
	var fresh1 := surface.get_fresh_at(o)
	_check("turned ground dries", fresh0 > 0.5 and fresh1 < 0.1, "fresh %.2f, %.2f after two drying times" % [fresh0, fresh1])

	# A 50 degree slope of dry sand: only the print relaxes, the slope stays.
	await _setup("slope", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var sp := Vector3(1.0, height.get_height(Vector3(1.0, 0, 0)), 0.0)
	surface.press_box(sp, Vector2(0.11, 0.29), 0.0, sp.y, 0.02)
	surface.settle(3.0)
	var far := Vector3(4.0, 0, 3.0)
	var drift := absf(surface.get_surface_height_at(far) - height.get_height(far))
	var vv := surface.get_moved_volume()
	_check("an undisturbed slope steeper than the repose stays", drift < 1e-4 and vv.x > 0.0, "%.2f mm off 3 m away, print %.2f l" % [drift * 1000.0, vv.x * 1000.0])

	# Bare rock (type map, green) does not print.
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	var img := Image.create(4, 4, false, Image.FORMAT_RGB8)
	img.fill(Color(0, 1, 0))
	surface.type_map = img
	surface.type_map_rect = Rect2(-2, -2, 4, 4)
	var rock := surface.press_box(o, Vector2(0.11, 0.29), 0.0, 0.0, 0.05)
	var soil_print := surface.press_box(Vector3(5, 0, 0), Vector2(0.11, 0.29), 0.0, 0.0, 0.05)
	_check("bare rock does not print", rock == 0.0 and soil_print > 0.0, "rock %.2f l, soil beside it %.2f l" % [rock * 1000.0, soil_print * 1000.0])

	# Bodies in the group press by themselves: a 75 kg character on dry sand.
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var foot := Node3D.new()
	var fs := CollisionShape3D.new()
	fs.shape = BoxShape3D.new()
	(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
	foot.add_child(fs)
	foot.set_meta("ground_mass", 75.0)
	foot.set_meta("ground_contact_area", 0.035)
	foot.add_to_group("ground_deform")
	add_child(foot)
	foot.global_position = Vector3(1.0, 0.03, 1.0)
	for i in 3:
		await get_tree().physics_frame
	var auto_dent := surface.get_dent_at(Vector3(1.0, 0, 1.0)) - surface.get_rim_at(Vector3(1.0, 0, 1.0))
	_check("a body in ground_deform prints", auto_dent > 0.01, "%.1f cm under the boot" % (auto_dent * 100.0))
	print("step %.2f ms" % surface.get_step_time_msec())


func _ground_material(kind_name: String) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	match kind_name:
		"dry_sand":
			m.albedo_color = Color(0.78, 0.68, 0.5)
		"damp_sand":
			m.albedo_color = Color(0.6, 0.5, 0.36)
		"mud":
			m.albedo_color = Color(0.3, 0.24, 0.18)
			m.roughness = 0.6
		"litter":
			m.albedo_color = Color(0.45, 0.3, 0.16)
		_:
			m.albedo_color = Color(0.42, 0.34, 0.25)
	if kind_name != "mud":
		m.roughness = 0.95
	var nt := NoiseTexture2D.new()
	nt.seamless = true
	nt.as_normal_map = true
	nt.bump_strength = 4.0 if kind_name != "litter" else 12.0
	nt.width = 512
	nt.height = 512
	var nz := FastNoiseLite.new()
	nz.frequency = 0.03 if kind_name != "litter" else 0.08
	nz.fractal_octaves = 5
	nt.noise = nz
	m.normal_enabled = true
	m.normal_texture = nt
	# World-mapped, as a terrain material is: the patch and the terrain line up.
	m.uv1_triplanar = true
	m.uv1_world_triplanar = true
	m.uv1_scale = Vector3(0.5, 0.5, 0.5)
	m.ground_deform_enabled = true
	return m


func _add_terrain(extent: float, cells: int, mat: Material) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step := extent / cells
	var h0 := -extent * 0.5
	for z in cells + 1:
		for x in cells + 1:
			var p := Vector3(h0 + x * step, 0, h0 + z * step)
			p.y = height.get_height(p)
			st.add_vertex(p)
	for z in cells:
		for x in cells:
			var i := z * (cells + 1) + x
			st.add_index(i)
			st.add_index(i + 1)
			st.add_index(i + cells + 1)
			st.add_index(i + 1)
			st.add_index(i + cells + 2)
			st.add_index(i + cells + 1)
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	mi.material_override = mat
	add_child(mi)
	# Bodies stand on the undisturbed ground, as on a terrain's own collider.
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = mi.mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)


func _undisturbed(p: Vector3) -> float:
	return height.get_height(p)


func _build_trail() -> void:
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.set("pathtracing_sky_shader_enabled", true)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = false
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-18.0, 140.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(1.2, 1.05, 3.4)
	cam.look_at(Vector3(-1.2, 0.0, 0.7))
	cam.fov = 55.0
	# The walker: a capsule on the ground's collider, its two boots (boxes in ground_deform, 80 kg on
	# 0.035 m2) printing by Bekker sinkage; a ball and a cart wheel roll through at set depths.
	var dark := StandardMaterial3D.new()
	dark.albedo_color = Color(0.15, 0.17, 0.22)
	walker = CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.14
	(ws.shape as CapsuleShape3D).height = 1.6
	walker.add_child(ws)
	walker.position = Vector3(-7.0, 1.0, 1.3)
	add_child(walker)
	for k in 2:
		var foot := Node3D.new()
		foot.name = "Foot%d" % k
		var fs := CollisionShape3D.new()
		fs.shape = BoxShape3D.new()
		(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
		foot.add_child(fs)
		foot.set_meta("ground_mass", 80.0)
		foot.set_meta("ground_contact_area", 0.035)
		foot.add_to_group("ground_deform")
		foot.position = Vector3(0, 100, 0)
		add_child(foot)
	var ball := Node3D.new()
	ball.name = "Ball"
	var bc := CollisionShape3D.new()
	bc.shape = SphereShape3D.new()
	(bc.shape as SphereShape3D).radius = 0.25
	ball.add_child(bc)
	var bmi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.25
	sm.height = 0.5
	bmi.mesh = sm
	var ball_mat := StandardMaterial3D.new()
	ball_mat.albedo_color = Color(0.6, 0.12, 0.08)
	bmi.material_override = ball_mat
	ball.add_child(bmi)
	ball.add_to_group("ground_deform")
	ball.position = Vector3(0, 100, 0)
	add_child(ball)
	var wheel := Node3D.new()
	wheel.name = "Wheel"
	var wc := CollisionShape3D.new()
	wc.shape = CylinderShape3D.new()
	(wc.shape as CylinderShape3D).radius = 0.4
	(wc.shape as CylinderShape3D).height = 0.12
	wc.rotation_degrees = Vector3(90, 0, 0)
	wheel.add_child(wc)
	var wmi := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.4
	cyl.bottom_radius = 0.4
	cyl.height = 0.12
	wmi.mesh = cyl
	wmi.rotation_degrees = Vector3(90, 0, 0)
	wmi.material_override = dark
	wheel.add_child(wmi)
	wheel.set_meta("ground_mass", 150.0)
	wheel.add_to_group("ground_deform")
	wheel.position = Vector3(0, 100, 0)
	add_child(wheel)
	walking = true
	for i in int(6.5 * 60.0):
		await get_tree().physics_frame
	walking = false
	# Let the walls settle as they would in the second after.
	for i in 60:
		await get_tree().physics_frame
	print("ground %s: step %.2f ms, moved %s m3" % [kind, surface.get_step_time_msec(), surface.get_moved_volume()])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


func _physics_process(delta: float) -> void:
	if not walking or walker == null:
		return
	walk_t += delta
	var x := -7.0 + 1.4 * walk_t
	var z := 1.3 + 0.5 * sin(walk_t * 0.9)
	walker.velocity = (Vector3(x, walker.global_position.y, z) - walker.global_position) / delta
	walker.velocity.y = -2.0
	walker.move_and_slide()
	var step := int(walk_t * 1.4 / 0.7)
	for k in 2:
		var foot: Node3D = get_node("Foot%d" % k)
		var planted := step % 2 == k
		if planted:
			var sx := -7.0 + step * 0.7 + 0.35
			var st := (sx + 7.0) / 1.4
			var sz := 1.3 + 0.5 * sin(st * 0.9)
			var side := 0.12 if k == 0 else -0.12
			var hd := atan2(-0.45 * cos(st * 0.9), 1.4)
			var p := Vector3(sx, 0, sz) + Vector3(sin(hd), 0, cos(hd)) * side
			if foot.get_meta("step", -1) != step:
				foot.set_meta("step", step)
				foot.rotation = Vector3(0, hd + PI * 0.5, 0)
				foot.global_position = Vector3(p.x, _undisturbed(p) + 0.03, p.z)
		else:
			foot.global_position = Vector3(0, 100, 0)
	var b: Node3D = get_node("Ball")
	var bp := Vector3(-6.0 + 1.2 * walk_t, 0, -0.6)
	b.global_position = Vector3(bp.x, _undisturbed(bp) - 0.06 + 0.25, bp.z)
	b.rotation.z = -bp.x / 0.25
	var w: Node3D = get_node("Wheel")
	var wp := Vector3(-8.0 + 1.7 * walk_t, 0, -2.4)
	w.global_position = Vector3(wp.x, _undisturbed(wp) - 0.03 + 0.4, wp.z)
	w.rotation.z = -wp.x / 0.4


func _save_frame() -> void:
	for i in 12:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)

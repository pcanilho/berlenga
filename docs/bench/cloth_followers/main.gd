extends Node3D
# Cloth followers (PhysXSkinnedCloth3D.followers / auto_followers): meshes carried by a simulated cloth, each vertex
# bound to its nearest simulated triangle. A sheet pinned along its top swings in a gusting wind; a trim strip along its
# hem, two flaps and a row of buttons are separate skinned meshes marked to follow it (meta "follow" = the sheet's
# node name, as glTF extras would). They must stay on the fabric however it swings.
#
# godot --path docs/bench/cloth_followers --rendering-driver vulkan --resolution 640x480 -- [options]
#   --pt            path traced
#   --nofollow      the followers left as plain skinned meshes (they hang where the rest pose put them)
#   --out=prefix    saves prefix_<frame>.png at a few frames and quits

var out := ""
var pt := false
var n := 0
var cloth: Node

func _arg(p: String) -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with(p + "="):
			return a.substr(p.length() + 1)
	return ""

func _ready() -> void:
	out = _arg("--out")
	pt = "--pt" in OS.get_cmdline_user_args()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.55, 0.6, 0.68)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.55, 0.55, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = pt
	env.pathtracing_samples_per_pixel = 8
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, -25, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(1.5, 0.9, 1.6)
	add_child(cam)
	cam.look_at(Vector3(0, 0.75, 0))

	var skel := Skeleton3D.new()
	skel.add_bone("root")
	skel.position = Vector3(0, 1.5, 0)
	add_child(skel)
	var sk := Skin.new()
	sk.add_named_bind("root", Transform3D())

	# The sheet: 1.2 by 1.4 m, hanging from y 0 down to -1.4 (skeleton space).
	var pm := PlaneMesh.new()
	pm.size = Vector2(1.2, 1.4)
	pm.subdivide_width = 20
	pm.subdivide_depth = 24
	pm.orientation = PlaneMesh.FACE_Z
	var sheet := _skinned(pm, Vector3(0, -0.7, 0), Color(0.18, 0.2, 0.28))
	sheet.name = "Coat"
	sheet.skin = sk
	skel.add_child(sheet)
	sheet.skeleton = sheet.get_path_to(skel)

	# The followers: a trim strip along the hem, two pocket flaps, five buttons; each a little proud of the sheet.
	var trim := BoxMesh.new()
	trim.size = Vector3(1.16, 0.06, 0.012)
	trim.subdivide_width = 24
	var flap := BoxMesh.new()
	flap.size = Vector3(0.24, 0.12, 0.01)
	flap.subdivide_width = 6
	flap.subdivide_height = 3
	var button := SphereMesh.new()
	button.radius = 0.018
	button.height = 0.012
	button.radial_segments = 12
	button.rings = 4
	var parts := [[trim, Vector3(0, -1.36, 0.012), Color(0.55, 0.45, 0.25)],
			[flap, Vector3(-0.3, -1.0, 0.01), Color(0.12, 0.13, 0.18)],
			[flap, Vector3(0.3, -1.0, 0.01), Color(0.12, 0.13, 0.18)]]
	for k in 5:
		parts.append([button, Vector3(0.0, -0.3 - 0.2 * k, 0.012), Color(0.7, 0.6, 0.3)])
	for k in parts.size():
		var f := _skinned(parts[k][0], parts[k][1], parts[k][2])
		f.name = "Trim%d" % k
		f.skin = sk
		skel.add_child(f)
		f.skeleton = f.get_path_to(skel)
		if not "--nofollow" in OS.get_cmdline_user_args():
			f.set_meta("follow", "Coat")

	cloth = ClassDB.instantiate("PhysXSkinnedCloth3D")
	skel.add_child(cloth)
	cloth.pin_height = -0.03
	cloth.max_distance = 0.6
	cloth.use_world_gravity = true
	cloth.mesh_instance_path = cloth.get_path_to(sheet)

func _skinned(src: Mesh, at: Vector3, col: Color) -> MeshInstance3D:
	var am := ArrayMesh.new()
	for s in src.get_surface_count():
		var arr: Array = src.surface_get_arrays(s)
		var vv: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var bones := PackedInt32Array()
		var weights := PackedFloat32Array()
		for i in vv.size():
			vv[i] += at
			bones.append_array([0, 0, 0, 0])
			weights.append_array([1.0, 0.0, 0.0, 0.0])
		arr[Mesh.ARRAY_VERTEX] = vv
		arr[Mesh.ARRAY_BONES] = bones
		arr[Mesh.ARRAY_WEIGHTS] = weights
		am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	var mi := MeshInstance3D.new()
	mi.mesh = am
	var m := StandardMaterial3D.new()
	m.albedo_color = col
	m.roughness = 0.8
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.material_override = m
	return mi

func _process(_d: float) -> void:
	n += 1
	if cloth:
		# A gusting wind across and into the sheet.
		var t := n / 60.0
		cloth.extra_acceleration = Vector3(8.0 * sin(t * 1.7), 0.0, 9.0 + 7.0 * sin(t * 2.9))
	if out != "" and n in [90, 150, 210]:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png("%s_%d.png" % [out, n])
		if n == 210:
			print("cloth_followers: %d followers carried" % (cloth.get_follower_instances() as Array).size())
			get_tree().quit()

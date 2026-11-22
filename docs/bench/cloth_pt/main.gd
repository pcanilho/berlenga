extends Node3D
# A skinned character's clothing under path tracing while its shaders compile. One skinned mesh: a body box
# (surface 0) and a cloth sheet (surface 1) driven by PhysXSkinnedCloth3D, whose render copy replaces it. Both
# draw with shaders made new on every run, so no shader cache already holds them: path tracing is warmed on an
# empty scene first, then the character is added and its custom hit groups compile in the background
# (rendering/pathtracing/async_shader_compilation, on here as in a game). While they compile, the body and the
# cloth must be traced with the default material, not left out (the character used to vanish on a cold cache).
#
# godot --path docs/bench/cloth_pt --rendering-driver vulkan --resolution 320x240 -- [options]
#   --test         check the frames: exit code 1 on a failure
#   --warm=S       seconds to let path tracing come up on the empty scene first (default 150; lavapipe is slow)
#   --out=dir      where the frames go (default user://): compiling.png, ready.png

var env: Environment
var cam: Camera3D
var out_dir := "user://"
var failures := 0

func _arg(p_name: String, p_default: String) -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with(p_name + "="):
			return a.substr(p_name.length() + 1)
	return p_default

func _ready() -> void:
	out_dir = _arg("--out", "user://")
	var we := WorldEnvironment.new()
	env = Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.5, 0.6, 0.7)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.5, 0.55, 0.6)
	env.pathtracing_samples_per_pixel = 4
	env.pathtracing_max_bounces = 2
	env.pathtracing_enabled = true
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, -30, 0)
	add_child(sun)
	cam = Camera3D.new()
	cam.position = Vector3(0, 1, 4)
	add_child(cam)
	cam.look_at(Vector3(0, 1, 0))
	var floor_mi := MeshInstance3D.new()
	var fp := PlaneMesh.new()
	fp.size = Vector2(10, 10)
	floor_mi.mesh = fp
	add_child(floor_mi)

	var warm_ms := float(_arg("--warm", "150")) * 1000.0
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < warm_ms:
		await get_tree().process_frame
	_add_character()

	var compiling_checked := false
	for i in 100000:
		await get_tree().process_frame
		var pending := get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES)
		if i == 6:
			var img := get_viewport().get_texture().get_image()
			img.save_png(out_dir.path_join("compiling.png"))
			if pending == 0:
				print("cloth_pt: shaders already compiled at frame 6; nothing to check while compiling")
			else:
				compiling_checked = true
				_check(img, "while compiling (pending %d)" % pending)
		if i > 6 and pending == 0:
			for k in 10:
				await get_tree().process_frame
			var img := get_viewport().get_texture().get_image()
			img.save_png(out_dir.path_join("ready.png"))
			_check(img, "compiled")
			break
	print("cloth_pt: %s (%s)" % ["PASS" if failures == 0 else "FAIL", "checked while compiling" if compiling_checked else "compile not caught"])
	get_tree().quit(1 if failures > 0 and "--test" in OS.get_cmdline_user_args() else 0)

# The body and the sheet both cover their projected centres: neither may show the background there.
func _check(p_img: Image, p_when: String) -> void:
	var bg := p_img.get_pixel(4, 4)
	for probe in [["sheet", Vector3(0, 1.0, 0.05)], ["body", Vector3(0, 0.95, -0.3)]]:
		var px := cam.unproject_position(probe[1])
		# A 5 x 5 mean: path traced frames are noisy.
		var c := Color(0, 0, 0)
		for dy in range(-2, 3):
			for dx in range(-2, 3):
				c += p_img.get_pixel(clampi(int(px.x) + dx, 0, p_img.get_width() - 1), clampi(int(px.y) + dy, 0, p_img.get_height() - 1))
		c /= 25.0
		var d: float = abs(c.r - bg.r) + abs(c.g - bg.g) + abs(c.b - bg.b)
		if d < 0.15:
			failures += 1
			print("cloth_pt: FAIL %s: the %s is missing (pixel %s, background %s)" % [p_when, probe[0], c, bg])

func _unique_shader(p_code: String) -> Shader:
	var s := Shader.new()
	# A constant no cache has seen: the hit group compiles now, as a shader new to the cache does.
	s.code = p_code.replace("UNIQUE", "%d.0" % (Time.get_unix_time_from_system() * 1000.0 + randi() % 1000))
	return s

func _add_character() -> void:
	var pm := PlaneMesh.new()
	pm.size = Vector2(1.5, 1.5)
	pm.subdivide_width = 24
	pm.subdivide_depth = 24
	pm.orientation = PlaneMesh.FACE_Z
	var am := ArrayMesh.new()
	# The body: a box, surface 0; the sheet hangs in front of it, surface 1. All on one bone.
	var bm := BoxMesh.new()
	bm.size = Vector3(0.6, 1.0, 0.3)
	for src in [bm, pm]:
		var arr: Array = src.get_mesh_arrays()
		var n: int = (arr[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
		var bones := PackedInt32Array()
		var weights := PackedFloat32Array()
		for i in n:
			bones.append_array([0, 0, 0, 0])
			weights.append_array([1.0, 0.0, 0.0, 0.0])
		arr[Mesh.ARRAY_BONES] = bones
		arr[Mesh.ARRAY_WEIGHTS] = weights
		if src == bm:
			var vv: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
			for i in vv.size():
				vv[i] += Vector3(0, -0.25, -0.3)
			arr[Mesh.ARRAY_VERTEX] = vv
		am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	var skel := Skeleton3D.new()
	skel.add_bone("root")
	skel.position = Vector3(0, 1.2, 0)
	add_child(skel)
	var mi := MeshInstance3D.new()
	mi.mesh = am
	var body_mat := ShaderMaterial.new()
	body_mat.shader = _unique_shader("shader_type spatial;\nvoid fragment() {\n\tALBEDO = vec3(0.2, 0.45, 0.9) * (UNIQUE > 0.0 ? 1.0 : 0.5);\n}\n")
	mi.set_surface_override_material(0, body_mat)
	var sheet_mat := StandardMaterial3D.new()
	sheet_mat.albedo_color = Color(0.8, 0.2, 0.2)
	sheet_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.set_surface_override_material(1, sheet_mat)
	var sk := Skin.new()
	sk.add_named_bind("root", Transform3D())
	mi.skin = sk
	skel.add_child(mi)
	mi.skeleton = mi.get_path_to(skel)
	var cloth = ClassDB.instantiate("PhysXSkinnedCloth3D")
	skel.add_child(cloth)
	cloth.surface = 1
	# The render copy's look, as a game's own cloth material: drawn from the solver's images (CUSTOM0.x is the particle).
	var cloth_mat := ShaderMaterial.new()
	cloth_mat.shader = _unique_shader("""shader_type spatial;
render_mode cull_disabled, rt_vertex_dynamic;
uniform sampler2D cloth_positions : filter_nearest, repeat_disable;
uniform sampler2D cloth_normals : filter_nearest, repeat_disable;
uniform int texture_width = 1;
uniform float normal_sign = 1.0;
void vertex() {
	int i = int(CUSTOM0.x + 0.5);
	ivec2 px = ivec2(i % texture_width, i / texture_width);
	VERTEX = texelFetch(cloth_positions, px, 0).xyz;
	NORMAL = normalize(texelFetch(cloth_normals, px, 0).xyz * normal_sign);
}
void fragment() {
	ALBEDO = vec3(0.8, 0.2, 0.2) * (UNIQUE > 0.0 ? 1.0 : 0.5);
}
""")
	cloth.render_material = cloth_mat
	cloth.mesh_instance_path = cloth.get_path_to(mi)

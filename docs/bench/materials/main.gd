extends Node3D
# Material test scene for the path tracer: rows of spheres under a sun and a sky, to compare
# the path tracer with the rasterizer material by material.
#
# godot --path docs/bench/materials -- [options]
#   --scene=NAME      clearcoat (default): red paint, coat strength 0 to 1 left to right,
#                     coat roughness 0 (back row) and 1 (front row)
#                     backlight: leaf-green cards against a low sun, back light 0 to 1 left to right
#                     transmission: glass panes (Transmission) before coloured spheres, roughness 0 to 1
#                     left to right, green tint in the back row
#                     textures: layer textures and custom shader outputs, sun behind. Left to right:
#                     coat stripes (clearcoat_texture), a card back lit through a checker (backlight_texture),
#                     a pane let through by a checker (transmission_texture), custom TRANSMISSION and
#                     custom BACKLIGHT
#                     water: a pool sloping from 0.1 m (left) to 4 m (right) deep, checker floor, posts
#                     through the surface; pure water (Pope and Fry 1997), IOR 1.33, Transmission not thin-walled
#                     --water=murky: the same pool with lake water that scatters (Transmission scatter,
#                     glTF KHR_materials_scatter); --water=shader: clear water from a ShaderMaterial writing
#                     TRANSMISSION, IOR and ABSORPTION
#                     caustics: the same pool's shallow half from above (use with --ripples);
#                     caustics_far: the same from 70 m through a long lens
#                     --ripples: a rippled surface (normal map, or NORMAL_MAP in the shader), which
#                     gathers the sun into caustics on the floor (rendering/pathtracing/caustics)
#                     subsurface: subsurface scattering under a low side sun. Front row: skin (Skin Mode),
#                     strength 0 to 1 left to right. Back row: wax candles (strength 1, plain mode), a skin
#                     sphere with a fine checker albedo (texture detail), a marble block
#                     subsurface_back: the same skin lit from behind: slabs 2, 5, 10 and 20 cm thick
#                     (light through thin parts), and an open card (traced as plain diffuse)
#                     Both set rendering/pathtracing/subsurface_scattering_scale to 0.05 m, so the
#                     1 m spheres scatter as a 10 cm object at the default 0.01
#                     water_depths: unshaded white tiles under 0.25, 0.5, 1, 2 and 4 m of water, seen straight
#                     down in an orthogonal view with linear tone mapping, to read Beer-Lambert off the pixels
#                     hair: hair shells (BaseMaterial3D Hair, strands from the crown) on spheres, melanin 0.1
#                     to 0.9 left to right; back row the same colours without Hair, for comparison
#                     --hair=back: the sun low behind them (light through the fibres);
#                     --hair=mask: front row hair on the upper half only (hair_mask_texture);
#                     --hair=cards: hair cards (hair_cards, strands along the bitangent) against a low sun
#                     strands: three heads with strand hair (HairGroom grown on the scalp, HairMesh, Hair
#                     direction Strands): straight light brown, wavy brown, curly black;
#                     --hair=back: the sun low behind them; --hair=thin: one strip per segment, not crossed
#                     strands_sim: one head of long strand hair simulated on the GPU (PhysXHair3D) that
#                     shakes side to side for two and a half seconds, then turns and stops; a breeze from the
#                     left. Record it with --fixed-fps 30 --write-movie (frame time is fixed to 1/30 s).
#                     --hair=wind: the head held still in a 6 m/s gusty breeze; add "wavy" (--hair=wind_wavy,
#                     --hair=wavy) for wavy hair; "blonde" for light hair, "back" for the sun low behind
#                     the head, "plain" for Godot's standard lighting instead of hair_shading, "far" for the
#                     camera 8 m away, "nolod" to draw every strand at any distance, "novolume" for
#                     strands that pass through each other (volume_enabled off), "taa" for temporal
#                     antialiasing, "mvview" to see the motion vectors, "nodeep" for the shadow map
#                     alone (deep_shadows off); "box" for broad
#                     rounded shoulders (spheres only on the skull), "body" to make the shoulders (with
#                     or without "box") the collision body, held still and seen from the side,
#                     "skinned" to skin them to a tilted bone (the head stays still), "hidebox" to see the drape;
#                     --hair=compare: held still, beside the same groom on a HairMesh with BaseMaterial3D
#                     Hair; --hair=compare_shader: that HairMesh through a shader's HAIR outputs instead
#   --debug=N         path tracing debug view (Environment.pathtracing_debug_mode), e.g. 4 normal map
#   --raster          raster only, for comparison
#   --aa=MODE         raster anti-aliasing: taa, msaa (4x), both or none (default)
#   --spp=N --bounces=N  path tracing samples and bounces (default 64, 3)
#   --frames=N        frames rendered before --out is saved (default 8)
#   --set=setting:value  any project setting, e.g. --set=rendering/pathtracing/clearcoat:0
#   --out=path.png    save the last frame and quit
#
# Software Vulkan (lavapipe) needs shader execution reordering off, read at startup, so put
# it in an override.cfg next to project.godot (not --set):
#   [rendering]
#   pathtracing/use_shader_execution_reordering=false
var scene := "clearcoat"
var raster := false
var spp := 64
var bounces := 3
var frames := 8
var out := ""
var water := "clear"
var hair := ""
var ripples := false
var debug_mode := 0
var n := 0
var sim_head: Node3D = null


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--scene=subsurface"):
			ProjectSettings.set_setting("rendering/pathtracing/subsurface_scattering_scale", 0.05)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--scene="): scene = a.substr(8)
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--water="): water = a.substr(8)
		elif a.begins_with("--hair="): hair = a.substr(7)
		elif a == "--ripples": ripples = true
		elif a.begins_with("--debug="): debug_mode = int(a.substr(8))
		elif a.begins_with("--aa="):
			# Raster anti-aliasing: taa, msaa (4x), both, or none (the default).
			var aa := a.substr(5)
			get_viewport().use_taa = aa == "taa" or aa == "both"
			get_viewport().msaa_3d = Viewport.MSAA_4X if aa == "msaa" or aa == "both" else Viewport.MSAA_DISABLED
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":", true, 1)
			ProjectSettings.set_setting(kv[0], str_to_var(kv[1]))

	var sky_mat := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	env.pathtracing_debug_mode = debug_mode
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35, -150, 0)
	sun.shadow_enabled = true
	add_child(sun)

	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	ground.mesh = plane
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.35, 0.35, 0.35)
	ground_mat.roughness = 0.9
	ground.material_override = ground_mat
	add_child(ground)

	var cam := Camera3D.new()
	cam.fov = 40
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 3.2, 8.5), Vector3(0, 0.6, 0))

	match scene:
		"clearcoat":
			_clearcoat_rows()
		"transmission":
			_transmission_panes()
		"backlight":
			sun.rotation_degrees = Vector3(-12, 180, 0)
			_backlight_cards()
		"textures":
			sun.rotation_degrees = Vector3(-25, 165, 0)
			_layer_textures()
		"water":
			ground.visible = false
			sun.rotation_degrees = Vector3(-55, -150, 0)
			cam.look_at_from_position(Vector3(0, 5.5, 7.5), Vector3(0, -1.2, 0))
			_water_pool()
		"caustics":
			# The pool's shallow half from above, with --ripples.
			ground.visible = false
			sun.rotation_degrees = Vector3(-60, -150, 0)
			cam.look_at_from_position(Vector3(-1.0, 4.0, 2.2), Vector3(-1.0, -1.0, 0.0))
			_water_pool()
		"caustics_far":
			# The same pool from 70 m away through a long lens, as an isometric game camera sees it.
			ground.visible = false
			sun.rotation_degrees = Vector3(-60, -150, 0)
			cam.fov = 6
			cam.look_at_from_position(Vector3(-1.0, 50.0, 48.0), Vector3(-1.0, -1.0, 0.0))
			_water_pool()
		"subsurface":
			sun.rotation_degrees = Vector3(-20, 95, 0)
			_subsurface_rows()
		"subsurface_back":
			sun.rotation_degrees = Vector3(-15, 180, 0)
			_subsurface_back()
		"water_depths":
			ground.visible = false
			env.background_mode = Environment.BG_COLOR
			env.background_color = Color(0, 0, 0)
			env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
			sun.visible = false
			cam.projection = Camera3D.PROJECTION_ORTHOGONAL
			cam.size = 6.0
			cam.look_at_from_position(Vector3(0, 6, 0), Vector3(0, 0, 0), Vector3(0, 0, -1))
			_water_depths()
		"hair":
			sun.rotation_degrees = Vector3(-12, 180, 0) if hair in ["back", "cards"] else Vector3(-40, -120, 0)
			cam.look_at_from_position(Vector3(0, 2.4, 6.5), Vector3(0, 0.7, 0))
			_hair()
		"strands":
			sun.rotation_degrees = Vector3(-15, 180, 0) if hair == "back" else Vector3(-30, -120, 0)
			cam.fov = 30
			# Light finds its way through a head of hair over many fibres.
			env.pathtracing_max_bounces = maxi(bounces, 12)
			cam.look_at_from_position(Vector3(0, 0.45, 1.75), Vector3(0, 0.25, 0))
			_strands()
		"strands_sim":
			sun.rotation_degrees = Vector3(-30, -120, 0)
			cam.fov = 30
			env.pathtracing_max_bounces = maxi(bounces, 12)
			if hair.begins_with("compare"):
				cam.look_at_from_position(Vector3(0.15, 0.4, 1.7), Vector3(0.15, 0.22, 0))
			elif hair.contains("box") or hair.contains("body"):
				# From the side, the hair against the shoulders' top and back.
				cam.look_at_from_position(Vector3(1.5, 0.3, 0.2), Vector3(0, 0.12, -0.05))
			elif hair.contains("far"):
				# 8 m away: the distance LOD draws about half the strands.
				cam.look_at_from_position(Vector3(3.0, 2.0, 7.2), Vector3(0, 0.2, 0))
			else:
				cam.look_at_from_position(Vector3(0.55, 0.45, 1.45), Vector3(0, 0.2, 0))
			_strands_sim()
		_:
			push_error("Unknown scene: " + scene)


func _sphere(p_pos: Vector3, p_mat: Material) -> void:
	var mi := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.5
	sphere.height = 1.0
	sphere.radial_segments = 96
	sphere.rings = 48
	mi.mesh = sphere
	mi.material_override = p_mat
	mi.position = p_pos
	add_child(mi)


func _clearcoat_rows() -> void:
	for row in 2:
		for i in 5:
			var m := StandardMaterial3D.new()
			m.albedo_color = Color(0.55, 0.03, 0.03)
			m.roughness = 0.6
			m.clearcoat_enabled = true
			m.clearcoat = i / 4.0
			m.clearcoat_roughness = 0.0 if row == 0 else 1.0
			_sphere(Vector3((i - 2) * 1.25, 0.5, -1.0 + row * 1.6), m)


func _backlight_cards() -> void:
	for i in 5:
		var m := StandardMaterial3D.new()
		m.albedo_color = Color(0.25, 0.5, 0.12)
		m.roughness = 0.7
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.backlight_enabled = true
		m.backlight = Color(1, 1, 1) * (i / 4.0)
		var mi := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(1.0, 1.4)
		mi.mesh = quad
		mi.material_override = m
		mi.position = Vector3((i - 2) * 1.25, 0.8, 0.0)
		add_child(mi)
	# A curtain the sun shines through, for its coloured shadow.
	var c := StandardMaterial3D.new()
	c.albedo_color = Color(0.8, 0.25, 0.2)
	c.cull_mode = BaseMaterial3D.CULL_DISABLED
	c.backlight_enabled = true
	c.backlight = Color(0.6, 0.6, 0.6)
	var curtain := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 1.2)
	curtain.mesh = quad
	curtain.material_override = c
	curtain.position = Vector3(0.0, 1.0, -3.0)
	add_child(curtain)


func _transmission_panes() -> void:
	var colors := [Color(0.8, 0.1, 0.1), Color(0.9, 0.7, 0.1), Color(0.1, 0.6, 0.2), Color(0.1, 0.3, 0.8), Color(0.7, 0.7, 0.7)]
	for i in 5:
		var s := StandardMaterial3D.new()
		s.albedo_color = colors[i]
		_sphere(Vector3((i - 2) * 1.25, 0.5, -2.0), s)
	for row in 2:
		for i in 5:
			var m := StandardMaterial3D.new()
			m.albedo_color = Color(1, 1, 1) if row == 0 else Color(0.55, 0.85, 0.6)
			m.roughness = i / 4.0 * 0.6
			m.transmission_enabled = true
			m.transmission_amount = 1.0
			m.cull_mode = BaseMaterial3D.CULL_DISABLED
			var mi := MeshInstance3D.new()
			var quad := QuadMesh.new()
			quad.size = Vector2(1.1, 0.5)
			mi.mesh = quad
			mi.material_override = m
			mi.position = Vector3((i - 2) * 1.25, 0.25 + row * 0.55, 0.0)
			add_child(mi)


# A 64 px checker of 8 px cells, p_on in one cell colour and p_off in the other.
func _checker(p_on: Color, p_off: Color) -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			img.set_pixel(x, y, p_on if ((x / 8) + (y / 8)) % 2 == 0 else p_off)
	return ImageTexture.create_from_image(img)


func _quad(p_size: Vector2, p_pos: Vector3, p_mat: Material) -> void:
	var mi := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = p_size
	mi.mesh = quad
	mi.material_override = p_mat
	mi.position = p_pos
	add_child(mi)


func _layer_textures() -> void:
	# Coat in stripes: red channel 1 or 0, green (roughness scale) 0.
	var coat := StandardMaterial3D.new()
	coat.albedo_color = Color(0.55, 0.03, 0.03)
	coat.roughness = 0.6
	coat.clearcoat_enabled = true
	coat.clearcoat = 1.0
	coat.clearcoat_roughness = 1.0
	var stripes := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			stripes.set_pixel(x, y, Color(1, 0, 0) if (x / 8) % 2 == 0 else Color(0, 0, 0))
	coat.clearcoat_texture = ImageTexture.create_from_image(stripes)
	coat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_sphere(Vector3(-3.0, 0.5, 0.0), coat)

	# Back light only where the checker is white.
	var card := StandardMaterial3D.new()
	card.albedo_color = Color(0.25, 0.5, 0.12)
	card.cull_mode = BaseMaterial3D.CULL_DISABLED
	card.backlight_enabled = true
	card.backlight = Color(0, 0, 0)
	card.backlight_texture = _checker(Color(1, 1, 1), Color(0, 0, 0))
	card.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_quad(Vector2(1.2, 1.2), Vector3(-1.5, 0.8, 0.0), card)

	# Glass only where the checker is red.
	var pane := StandardMaterial3D.new()
	pane.albedo_color = Color(0.55, 0.85, 0.6)
	pane.roughness = 0.0
	pane.transmission_enabled = true
	pane.transmission_amount = 1.0
	pane.transmission_texture = _checker(Color(1, 0, 0), Color(0, 0, 0))
	pane.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	pane.cull_mode = BaseMaterial3D.CULL_DISABLED
	_quad(Vector2(1.2, 1.2), Vector3(0.0, 0.8, 0.0), pane)

	# Custom shaders writing TRANSMISSION and BACKLIGHT.
	var glass := Shader.new()
	glass.code = "shader_type spatial;\nrender_mode cull_disabled;\nvoid fragment() {\n\tALBEDO = vec3(0.3, 0.5, 0.9);\n\tROUGHNESS = 0.0;\n\tTRANSMISSION = 1.0;\n}\n"
	var glass_mat := ShaderMaterial.new()
	glass_mat.shader = glass
	_quad(Vector2(1.2, 1.2), Vector3(1.5, 0.8, 0.0), glass_mat)
	var leaf := Shader.new()
	leaf.code = "shader_type spatial;\nrender_mode cull_disabled;\nvoid fragment() {\n\tALBEDO = vec3(0.8, 0.25, 0.2);\n\tROUGHNESS = 0.8;\n\tBACKLIGHT = vec3(0.6);\n}\n"
	var leaf_mat := ShaderMaterial.new()
	leaf_mat.shader = leaf
	_quad(Vector2(1.2, 1.2), Vector3(3.0, 0.8, 0.0), leaf_mat)

	var colors := [Color(0.8, 0.1, 0.1), Color(0.9, 0.7, 0.1), Color(0.1, 0.3, 0.8)]
	for i in 3:
		var b := StandardMaterial3D.new()
		b.albedo_color = colors[i]
		_sphere(Vector3(-1.5 + i * 1.5, 0.5, -1.6), b)


# Pure water: absorption 0.35, 0.06 and 0.01 per metre in red, green and blue (Pope and Fry 1997),
# as the colour left after 1 m.
func _water() -> Material:
	if water == "shader":
		var sm := ShaderMaterial.new()
		var sh := Shader.new()
		sh.code = """shader_type spatial;
void fragment() {
	ALBEDO = vec3(1.0);
	ROUGHNESS = 0.05;
	SPECULAR = 0.354;
	TRANSMISSION = 1.0;
	IOR = 1.33;
	ABSORPTION = vec3(0.35, 0.06, 0.01);
	if (RIPPLES) {
		// Three crossing sine swells, about 0.5 to 0.9 m long.
		vec2 p = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xz;
		vec2 g = vec2(0.0);
		g += 0.18 * vec2(1.0, 0.3) * cos(dot(p, vec2(1.0, 0.3)) * 7.0);
		g += 0.14 * vec2(-0.4, 1.0) * cos(dot(p, vec2(-0.4, 1.0)) * 9.0 + 1.3);
		g += 0.1 * vec2(0.7, -0.7) * cos(dot(p, vec2(0.7, -0.7)) * 12.0 + 2.1);
		NORMAL_MAP = normalize(vec3(-g.x, -g.y, 1.0)) * 0.5 + 0.5;
	}
}
"""
		sh.code = sh.code.replace("RIPPLES", "true" if ripples else "false")
		sm.shader = sh
		return sm
	var w := StandardMaterial3D.new()
	w.roughness = 0.05
	w.specular = 0.354 # F0 0.02, ((1.33 - 1) / (1.33 + 1))^2.
	w.transmission_enabled = true
	w.transmission_thin_walled = false
	w.transmission_ior = 1.33
	w.transmission_attenuation_color = Color(exp(-0.35), exp(-0.06), exp(-0.01)).linear_to_srgb()
	w.transmission_attenuation_distance = 1.0
	if ripples:
		w.normal_enabled = true
		w.normal_texture = _ripple_normal_map()
		w.uv1_scale = Vector3(4.0, 4.0 / 1.5, 1.0) # 1.5 m tiles on the 6 x 4 m pool.
	if water == "murky":
		# Green lake water: light fades within a few metres and much of it scatters back.
		w.transmission_attenuation_color = Color(0.55, 0.72, 0.6)
		w.transmission_attenuation_distance = 1.0
		w.transmission_scatter_strength = 1.0
		w.transmission_scatter_color = Color(0.45, 0.62, 0.5)
		w.transmission_scatter_anisotropy = 0.6
	return w


# A 1.5 m tile of three crossing swells (periodic, so it repeats): slopes up to about 0.3.
func _ripple_normal_map() -> ImageTexture:
	var n_px := 256
	var waves := [Vector3(2, 1, 0.26), Vector3(-1, 2, 0.22), Vector3(3, -2, 0.14)] # tile cycles x, y; slope
	var data := PackedByteArray()
	data.resize(n_px * n_px * 3)
	var i := 0
	for y in n_px:
		for x in n_px:
			var gx := 0.0
			var gy := 0.0
			for wv: Vector3 in waves:
				var c: float = wv.z * cos(TAU * (wv.x * x + wv.y * y) / n_px + wv.x) / Vector2(wv.x, wv.y).length()
				gx += wv.x * c
				gy += wv.y * c
			var n := Vector3(-gx, -gy, 1.0).normalized()
			data[i] = int((n.x * 0.5 + 0.5) * 255.0)
			data[i + 1] = int((n.y * 0.5 + 0.5) * 255.0)
			data[i + 2] = int((n.z * 0.5 + 0.5) * 255.0)
			i += 3
	var img := Image.create_from_data(n_px, n_px, false, Image.FORMAT_RGB8, data)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _box(p_size: Vector3, p_pos: Vector3, p_mat: Material, p_rot := Vector3.ZERO) -> void:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = p_size
	mi.mesh = box
	mi.material_override = p_mat
	mi.position = p_pos
	mi.rotation_degrees = p_rot
	add_child(mi)


func _skin(p_strength: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.85, 0.6, 0.5)
	m.roughness = 0.5
	m.subsurf_scatter_enabled = true
	m.subsurf_scatter_skin_mode = true
	m.subsurf_scatter_strength = p_strength
	return m


func _subsurface_rows() -> void:
	for i in 5:
		_sphere(Vector3((i - 2) * 1.25, 0.5, 0.6), _skin(i / 4.0))
	var wax := StandardMaterial3D.new()
	wax.albedo_color = Color(0.92, 0.86, 0.7)
	wax.roughness = 0.35
	wax.subsurf_scatter_enabled = true
	wax.subsurf_scatter_strength = 1.0
	for i in 2:
		var mi := MeshInstance3D.new()
		var cyl := CylinderMesh.new()
		cyl.top_radius = 0.18
		cyl.bottom_radius = 0.18
		cyl.height = 1.2 - i * 0.4
		cyl.radial_segments = 64
		mi.mesh = cyl
		mi.material_override = wax
		mi.position = Vector3(-2.4 + i * 0.5, cyl.height * 0.5, -1.2)
		add_child(mi)
	var textured := _skin(1.0)
	textured.albedo_texture = _checker(Color(1, 1, 1), Color(0.55, 0.3, 0.3))
	textured.uv1_scale = Vector3(4, 4, 1)
	_sphere(Vector3(0.0, 0.5, -1.2), textured)
	var marble := StandardMaterial3D.new()
	marble.albedo_color = Color(0.95, 0.95, 0.93)
	marble.roughness = 0.2
	marble.subsurf_scatter_enabled = true
	marble.subsurf_scatter_strength = 0.6
	_box(Vector3(1.0, 0.8, 0.8), Vector3(2.2, 0.4, -1.2), marble, Vector3(0, 25, 0))


func _subsurface_back() -> void:
	var thickness := [0.02, 0.05, 0.1, 0.2]
	for i in 4:
		_box(Vector3(0.8, 1.2, thickness[i]), Vector3((i - 2) * 1.1, 0.6, 0.0), _skin(1.0))
	var card := _skin(1.0)
	card.cull_mode = BaseMaterial3D.CULL_DISABLED
	_quad(Vector2(0.8, 1.2), Vector3(2.2, 0.6, 0.0), card)


func _water_pool() -> void:
	# Pool 6 x 4 m; the deck's top is at 0, the water's surface just under it.
	var deck := StandardMaterial3D.new()
	deck.albedo_color = Color(0.6, 0.58, 0.52)
	deck.roughness = 0.8
	_box(Vector3(40, 0.5, 18), Vector3(0, -0.25, -11), deck)
	_box(Vector3(40, 0.5, 18), Vector3(0, -0.25, 11), deck)
	_box(Vector3(17, 0.5, 4), Vector3(-11.5, -0.25, 0), deck)
	_box(Vector3(17, 0.5, 4), Vector3(11.5, -0.25, 0), deck)
	var tiles := StandardMaterial3D.new()
	tiles.albedo_texture = _checker(Color(0.85, 0.85, 0.85), Color(0.25, 0.25, 0.25))
	tiles.uv1_triplanar = true
	tiles.uv1_world_triplanar = true
	tiles.uv1_scale = Vector3(0.25, 0.25, 0.25) # 0.5 m squares.
	tiles.roughness = 0.7
	_box(Vector3(6, 4.5, 0.2), Vector3(0, -2.25, -2.1), tiles)
	_box(Vector3(6, 4.5, 0.2), Vector3(0, -2.25, 2.1), tiles)
	_box(Vector3(0.2, 4.5, 4.4), Vector3(-3.1, -2.25, 0), tiles)
	_box(Vector3(0.2, 4.5, 4.4), Vector3(3.1, -2.25, 0), tiles)
	# Floor from 0.1 m deep at the left wall to 4 m at the right, its ends tucked into the walls.
	var slope := atan2(3.9, 6.0)
	var floor_mid := Vector3(0, -2.05, 0) + Vector3(sin(slope), -cos(slope), 0) * 0.1
	_box(Vector3(6.0 / cos(slope) + 0.2, 0.2, 4.0), floor_mid, tiles, Vector3(0, 0, -rad_to_deg(slope)))
	# Posts standing out of the water.
	var colors := [Color(0.9, 0.9, 0.9), Color(0.8, 0.1, 0.08), Color(0.95, 0.75, 0.1), Color(0.9, 0.9, 0.9)]
	for i in 4:
		var post := StandardMaterial3D.new()
		post.albedo_color = colors[i]
		post.roughness = 0.5
		var mi := MeshInstance3D.new()
		var cyl := CylinderMesh.new()
		cyl.top_radius = 0.1
		cyl.bottom_radius = 0.1
		cyl.height = 5.3
		mi.mesh = cyl
		mi.material_override = post
		mi.position = Vector3(-2.0 + i * 1.5, -1.85, 0.4) # 0.8 m above the water.
		add_child(mi)
	var water := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(6, 4)
	water.mesh = plane
	water.material_override = _water()
	water.position = Vector3(0, -0.02, 0)
	add_child(water)


func _water_depths() -> void:
	var tile := StandardMaterial3D.new()
	tile.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tile.albedo_color = Color(1, 1, 1)
	var depths := [0.0, 0.25, 0.5, 1.0, 2.0, 4.0]
	for i in depths.size():
		_box(Vector3(0.8, 0.1, 0.8), Vector3(-2.5 + i * 1.0, -depths[i] - 0.05, 0), tile)
	var water := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(5.2, 1.6)
	water.mesh = plane
	water.material_override = _water()
	water.position = Vector3(0.5, 0.0, 0)
	add_child(water)


# The colour a mass of fibres with this melanin shows (BaseMaterial3D's raster hair colour).
func _melanin_color(p_melanin: float) -> Color:
	var m := -log(maxf(1.0 - p_melanin, 1e-4))
	var d := 5.969 - 0.215 * 0.3 + 2.532 * 0.09 - 10.73 * 0.027 + 5.574 * 0.0081 + 0.245 * 0.00243
	var sigma := Vector3(0.506, 0.841, 1.653) * m
	return Color(exp(-sqrt(sigma.x) * d), exp(-sqrt(sigma.y) * d), exp(-sqrt(sigma.z) * d)).linear_to_srgb()


func _hair() -> void:
	if hair == "cards":
		for i in 5:
			var m := StandardMaterial3D.new()
			m.albedo_color = Color(0.85, 0.7, 0.45)
			m.cull_mode = BaseMaterial3D.CULL_DISABLED
			m.hair_enabled = true
			m.hair_cards = true
			m.hair_direction = BaseMaterial3D.HAIR_DIRECTION_BITANGENT
			m.hair_roughness = 0.15 + 0.1 * i
			_quad(Vector2(0.9, 1.6), Vector3((i - 2) * 1.2, 0.9, 0.0), m)
		return
	var mask: ImageTexture = null
	if hair == "mask":
		var img := Image.create(4, 64, false, Image.FORMAT_L8)
		for y in 64:
			for x in 4:
				img.set_pixel(x, y, Color.WHITE if y < 26 else Color.BLACK)
		mask = ImageTexture.create_from_image(img)
	for row in 2:
		for i in 5:
			var melanin := 0.1 + 0.2 * i
			var m := StandardMaterial3D.new()
			m.roughness = 0.3
			if row == 1:
				m.hair_enabled = true
				m.hair_color_mode = BaseMaterial3D.HAIR_COLOR_MELANIN
				m.hair_melanin = melanin
				m.hair_crown = Vector3(0, 0.5, 0)
				if mask:
					m.albedo_color = Color(0.8, 0.6, 0.5)
					m.hair_mask_texture = mask
			else:
				m.albedo_color = _melanin_color(melanin)
			_sphere(Vector3((i - 2) * 1.25, 0.5, -1.0 + row * 1.6), m)


func _strands() -> void:
	var scalp := Image.create(4, 64, false, Image.FORMAT_L8)
	for y in 64:
		for x in 4:
			scalp.set_pixel(x, y, Color.WHITE if y < 27 else Color.BLACK)
	var looks := [
		# melanin, length, droop, curl radius, curls per metre, frizz
		[0.25, 0.30, 0.8, 0.0, 0.0, 0.02],
		[0.55, 0.26, 0.6, 0.008, 12.0, 0.04],
		[0.9, 0.12, 0.15, 0.006, 45.0, 0.08],
	]
	for i in 3:
		var head := SphereMesh.new()
		head.radius = 0.1
		head.height = 0.22
		head.radial_segments = 64
		head.rings = 32
		var skin := StandardMaterial3D.new()
		skin.albedo_color = Color(0.8, 0.6, 0.5)
		skin.roughness = 0.5
		var hi := MeshInstance3D.new()
		hi.mesh = head
		hi.material_override = skin
		hi.position = Vector3((i - 1) * 0.45, 0.27, 0.0)
		add_child(hi)

		var l: Array = looks[i]
		var groom := HairGroom.new()
		groom.grow_on_mesh(head, 0, 6000, l[1], 24, scalp, Vector3(0, 0.11, -0.03), 0.8, l[2], l[3], l[4], l[5], 7 + i)
		var hair_mesh := HairMesh.new()
		hair_mesh.groom = groom
		hair_mesh.root_width = 0.0005
		hair_mesh.tip_width = 0.0002
		hair_mesh.crossed = hair != "thin"
		var m := StandardMaterial3D.new()
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.hair_enabled = true
		m.hair_color_mode = BaseMaterial3D.HAIR_COLOR_MELANIN
		m.hair_melanin = l[0]
		m.hair_direction = BaseMaterial3D.HAIR_DIRECTION_STRANDS
		var mi := MeshInstance3D.new()
		mi.mesh = hair_mesh
		mi.material_override = m
		mi.position = hi.position
		add_child(mi)


func _strands_sim() -> void:
	var scalp := Image.create(4, 64, false, Image.FORMAT_L8)
	for y in 64:
		for x in 4:
			scalp.set_pixel(x, y, Color.WHITE if y < 27 else Color.BLACK)
	sim_head = Node3D.new()
	sim_head.position = Vector3(0, 0.3, 0)
	add_child(sim_head)
	var head := SphereMesh.new()
	head.radius = 0.1
	head.height = 0.22
	head.radial_segments = 64
	head.rings = 32
	var skin := StandardMaterial3D.new()
	skin.albedo_color = Color(0.8, 0.6, 0.5)
	skin.roughness = 0.5
	var hi := MeshInstance3D.new()
	hi.mesh = head
	hi.material_override = skin
	sim_head.add_child(hi)
	var groom := HairGroom.new()
	var wavy := hair.contains("wavy")
	groom.grow_on_mesh(head, 0, 30000, 0.32, 24 if wavy else 20, scalp, Vector3(0, 0.11, -0.03), 0.8, 0.8, 0.01 if wavy else 0.0, 9.0 if wavy else 0.0, 0.02, 11)
	var h := PhysXHair3D.new()
	h.groom = groom
	h.root_width = 0.0003
	h.tip_width = 0.0001
	h.melanin = 0.12 if hair.contains("blonde") else 0.3
	# "plain": Godot's standard lighting on the strands instead of the hair shading.
	h.hair_shading = not hair.contains("plain")
	h.lod_enabled = not hair.contains("nolod")
	h.volume_enabled = not hair.contains("novolume")
	h.deep_shadows = not hair.contains("nodeep")
	if hair.contains("taa"):
		# Temporal antialiasing, which follows the strands by their motion vectors.
		get_viewport().use_taa = true
	if hair.contains("mvview"):
		# The motion vectors themselves (Godot's debug view).
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_MOTION_VECTORS
	if hair.contains("back"):
		# The sun low behind the head, toward the camera: light through the hair.
		for c in get_children():
			if c is DirectionalLight3D:
				c.rotation_degrees = Vector3(-15, 160, 0)
	if hair.contains("all"):
		h.guide_ratio = 1.0
	h.wind = Vector3(1.5, 0, 0)
	if hair.contains("wind"):
		# Head held still in a moderate breeze (6 m/s, Beaufort 4) with gusts.
		h.wind = Vector3(6.0, 0.5, 1.5)
		h.gust = 0.5
	# The skull and the shoulders, so the strands drape instead of falling through.
	h.collision_spheres = PackedVector4Array([Vector4(0, 0, 0, 0.1), Vector4(0, -0.27, 0, 0.16)])
	sim_head.add_child(h)
	var body := MeshInstance3D.new()
	var torso := SphereMesh.new()
	torso.radius = 0.15
	torso.height = 0.3
	body.mesh = torso
	body.material_override = skin
	body.position = Vector3(0, -0.27, 0)
	sim_head.add_child(body)
	if hair.contains("body") and not hair.contains("box"):
		# The rounded shoulders as the collision body (a distance field) instead of a sphere.
		h.collision_spheres = PackedVector4Array([Vector4(0, 0, 0, 0.1)])
		h.collision_body = h.get_path_to(body)
	if hair.contains("box"):
		# Broad rounded shoulders, which the spheres fit badly: "body" makes them the collision
		# body (a distance field), "skinned" skins them to a bone tilted 25 degrees.
		var shoulders := SphereMesh.new()
		shoulders.radius = 0.21
		shoulders.height = 0.16
		body.mesh = shoulders
		body.position = Vector3(0, -0.22, 0)
		h.collision_spheres = PackedVector4Array([Vector4(0, 0, 0, 0.1)])
		if hair.contains("skinned"):
			body.mesh = _skinned_box(shoulders)
			body.position = Vector3.ZERO
			var skel := Skeleton3D.new()
			skel.add_bone("spine")
			skel.set_bone_rest(0, Transform3D(Basis(), Vector3(0, -0.22, 0)))
			skel.set_bone_pose_position(0, Vector3(0, -0.22, 0))
			skel.set_bone_pose_rotation(0, Quaternion(Vector3(0, 0, 1), deg_to_rad(25.0)))
			sim_head.add_child(skel)
			var sk := Skin.new()
			sk.add_named_bind("spine", Transform3D(Basis(), Vector3(0, 0.22, 0)))
			body.skin = sk
			body.skeleton = body.get_path_to(skel)
		if hair.contains("body"):
			h.collision_body = h.get_path_to(body)
		if hair.contains("hidebox"):
			body.visible = false
	if hair.begins_with("compare"):
		# The same groom drawn still by HairMesh with BaseMaterial3D Hair, beside it.
		h.simulating = false
		var hm := HairMesh.new()
		hm.groom = groom
		hm.root_width = 0.0005
		hm.tip_width = 0.0002
		var m := StandardMaterial3D.new()
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.hair_enabled = true
		m.hair_color_mode = BaseMaterial3D.HAIR_COLOR_MELANIN
		m.hair_melanin = 0.3
		m.hair_direction = BaseMaterial3D.HAIR_DIRECTION_STRANDS
		var mi := MeshInstance3D.new()
		mi.mesh = hm
		mi.material_override = m
		if hair == "compare_shader":
			# The same through a custom shader's HAIR outputs, to tell the shader path from the node.
			var sh := Shader.new()
			sh.code = "shader_type spatial;\nrender_mode cull_disabled, hair_strands;\nvoid fragment() {\n\tALBEDO = vec3(0.3, 0.2, 0.1);\n\tHAIR = 1.0;\n\tHAIR_ABSORPTION = vec3(0.18, 0.30, 0.59);\n\tHAIR_ROUGHNESS = vec2(0.3);\n}\n"
			var sm := ShaderMaterial.new()
			sm.shader = sh
			mi.material_override = sm
		mi.position = Vector3(0.3, 0.3, 0)
		add_child(mi)
		var hi2 := MeshInstance3D.new()
		hi2.mesh = head
		hi2.material_override = skin
		hi2.position = mi.position
		add_child(hi2)


# The box's surface skinned wholly to bone 0, in the bone's bind space at rest.
func _skinned_box(p_box: PrimitiveMesh) -> ArrayMesh:
	var a := p_box.get_mesh_arrays()
	var verts: PackedVector3Array = a[Mesh.ARRAY_VERTEX]
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	for i in verts.size():
		verts[i] += Vector3(0, -0.22, 0)
		bones.append_array([0, 0, 0, 0])
		weights.append_array([1.0, 0.0, 0.0, 0.0])
	a[Mesh.ARRAY_VERTEX] = verts
	a[Mesh.ARRAY_BONES] = bones
	a[Mesh.ARRAY_WEIGHTS] = weights
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, a)
	return m


func _process(_delta: float) -> void:
	n += 1
	if sim_head and not hair.contains("wind") and not hair.contains("box") and not hair.contains("body"):
		var t := n / 30.0
		# Shake for three full periods (ends back at x = 0), then turn.
		if t < 2.5:
			sim_head.position.x = 0.08 * sin(TAU * 1.2 * t)
		else:
			sim_head.position.x = 0.0
			sim_head.rotation.y = lerpf(0.0, 1.2, smoothstep(2.5, 3.1, t))
	if n == frames:
		if out != "":
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

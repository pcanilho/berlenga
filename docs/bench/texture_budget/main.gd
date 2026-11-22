# Texture budget bench (docs/ideas/quality-and-budgets.md): texture streaming and the quality ladder must
# not lose detail, in raster or path traced. Run gen.gd and --import first (see gen.gd).
#
# godot --path docs/bench/texture_budget --rendering-driver vulkan -- [options]
#   --out=path.png   save the frame
#   --raster         raster only (default path traced)
#   --streamed       draw the Texture2D Streamed copies (default: plain Texture2D, every mip resident)
#   --level=N        RenderingQuality level 0-3 (Low, Medium, High, Ultra); default the project's (High)
#   --budget=MB      streaming budget on, at MB
#   --spp=N          path tracing samples (default 4)
#   --frames=N       frames drawn after streaming settled (default 30)
#   --pin            streamed textures stay whole (streaming max_lod 0): the same files, every mip resident
#   --max-lod=N      the coarsest mip a streamed texture drops to (streaming max_lod; default 3, 512 px of 4K).
#                    6 lets textures nothing samples fall to 64 px, so a poster seen only in the mirror shows
#                    whether the path tracer's own requests keep its mips
# The camera looks at a floor and a wall close by and a block far away, and at a mirror that shows a poster
# behind the camera (only the path tracer sees it), beside alpha-tested leaf cards (any-hit reads the base
# level). Prints video memory, the streamer's resident bytes, and each texture's resident size. With
# streaming working, a --streamed frame matches the plain one (raster byte for byte; path traced within
# noise, with the radiance cache off), while far and unseen textures hold fewer mips.
extends Node3D

const NAMES := ["floor", "wall", "poster", "far", "leaves"]
var out := ""
var raster := false
var streamed := false
var level := -1
var budget := 0
var spp := 4
var frames := 30
var textures := {}


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--out="):
			out = a.substr(6)
		elif a == "--raster":
			raster = true
		elif a == "--streamed":
			streamed = true
		elif a.begins_with("--level="):
			level = int(a.substr(8))
		elif a.begins_with("--budget="):
			budget = int(a.substr(9))
		elif a.begins_with("--spp="):
			spp = int(a.substr(6))
		elif a.begins_with("--frames="):
			frames = int(a.substr(9))
		elif a == "--pin":
			ProjectSettings.set_setting("rendering/textures/streaming/max_lod", 0)
		elif a.begins_with("--max-lod="):
			ProjectSettings.set_setting("rendering/textures/streaming/max_lod", int(a.substr(10)))
	# Bit-repeatable path traced frames for comparing (docs/ideas/optimisation.md).
	ProjectSettings.set_setting("rendering/pathtracing/radiance_cache", false)
	if budget > 0:
		ProjectSettings.set_setting("rendering/textures/streaming/memory_budget_enabled", true)
		ProjectSettings.set_setting("rendering/textures/streaming/memory_budget_mb", budget)
	if level >= 0:
		RenderingQuality.set_level(level)
	_build()
	_run()


func _tex(name: String) -> Texture2D:
	var t: Texture2D = load("res://tex/%s%s.png" % [name, "_s" if streamed else ""])
	textures[name] = t
	return t


func _mat(name: String, uv: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_texture = _tex(name)
	m.uv1_scale = Vector3(uv, uv, 1.0)
	m.roughness = 0.7
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	return m


func _quad(size: Vector2, mat: Material, pos: Vector3, rot: Vector3) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = size
	mi.mesh = q
	mi.material_override = mat
	mi.position = pos
	mi.rotation_degrees = rot
	add_child(mi)
	return mi


func _build() -> void:
	_quad(Vector2(30, 30), _mat("floor", 6.0), Vector3(0, 0, 0), Vector3(-90, 0, 0))
	_quad(Vector2(4, 2.5), _mat("wall", 1.0), Vector3(-1.6, 1.25, -2.5), Vector3(0, 25, 0))
	_quad(Vector2(3, 3), _mat("far", 1.0), Vector3(4, 1.5, -40), Vector3(0, 0, 0))
	# The poster hangs behind the camera, facing it from behind; only the mirror shows it.
	_quad(Vector2(2, 2), _mat("poster", 1.0), Vector3(1.2, 1.4, 6.5), Vector3(0, 180, 0))
	var mirror := StandardMaterial3D.new()
	mirror.metallic = 1.0
	mirror.roughness = 0.0
	mirror.albedo_color = Color(0.95, 0.95, 0.95)
	_quad(Vector2(2.2, 2.2), mirror, Vector3(1.4, 1.3, -2.0), Vector3(0, -8, 0))
	var leaves := _mat("leaves", 1.0)
	leaves.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	leaves.alpha_scissor_threshold = 0.5
	leaves.cull_mode = BaseMaterial3D.CULL_DISABLED
	_quad(Vector2(1.5, 1.5), leaves, Vector3(-0.2, 2.6, -0.5), Vector3(-60, 20, 0))

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = ProceduralSkyMaterial.new()
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var cam := Camera3D.new()
	cam.fov = 60
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 1.6, 2.0), Vector3(0.2, 1.0, -3.0))


func _streaming() -> Object:
	return Engine.get_singleton("TextureStreaming") if Engine.has_singleton("TextureStreaming") else null


func _run() -> void:
	for i in 600:
		await RenderingServer.frame_post_draw
		if get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) == 0:
			break
	var ts := _streaming()
	if streamed and ts:
		# Let feedback arrive (every 30 ms), then load everything to its target now.
		for i in 120:
			await RenderingServer.frame_post_draw
		for round in 3:
			ts.call("flush_texture_streaming")
			await ts.flush_completed
			for i in 20:
				await RenderingServer.frame_post_draw
	for i in frames:
		await RenderingServer.frame_post_draw
	print("video memory: %.1f MB" % (RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED) / 1.0e6))
	print("quality level %d; geometry lod scale %s, texture cap %s" % [RenderingQuality.get_level(), ProjectSettings.get_setting("rendering/mesh_lod/lod_change/quality_scale"), ProjectSettings.get_setting("rendering/textures/streaming/max_resolution")])
	if ts:
		print("streamed resident: %.1f MB" % (int(ts.call("get_memory_budget_bytes_used")) / 1.0e6))
	for n in NAMES:
		var t: Texture2D = textures[n]
		var rd := RenderingServer.texture_get_rd_texture(t.get_rid())
		var dev := RenderingServer.get_rendering_device()
		var line := "%s: %dx%d" % [n, t.get_width(), t.get_height()]
		if rd.is_valid() and dev:
			var fmt := dev.texture_get_format(rd)
			line += " resident %dx%d, %d mips" % [fmt.width, fmt.height, fmt.mipmaps]
		print("  ", line)
	if out != "":
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

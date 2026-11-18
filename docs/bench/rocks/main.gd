extends Node3D
# Rocks bench: RockGenerator shapes and RockField3D placement (docs/ideas/rocks.md).
#
# godot --path docs/bench/rocks --rendering-driver vulkan -- [options]
#   --test                checks: every rock type and family builds (size, axes, timing), transport rounds and beach
#                         wear flattens, then the field's six layers on a generated world; prints PASS/FAIL, quits
#   --gallery             every family (columns) of every rock type (rows), each scaled to a 1 m b axis
#   --rock=type,family    one rock close up (--seed=N, --distance=m transport, --weathering=0..1; --stand: a fin,
#                         hoodoo or stack stood on end, side on)
#   --clouds[=C]          the weather sky (clouds of cover C, atmosphere) instead of the plain gradient
#   --shader-scree        the ground shader's parallax scree blocks under the real stones too (before frames)
#   --talus-slope         with --view=scree: the fullest talus on a 25-38 degree slope above the beaches
#   --world               a generated 2 km world (range, strata, glaciers, scree, rivers, a lake, the sea) with a
#                         RockField3D over it; --view=rockfall|scree|glacial|river|shore|cliff|tor|coast|landform puts the camera at the
#                         densest place of that layer (default scree)
#   --type=granite|limestone|sandstone|basalt|slate   the world's rock (default granite; soft beds limestone)
#   --stand-ins           with --world: the old grey spheres at the rockfall blocks instead of the field (before shots)
#   --no-rocks            with --world: no rocks at all
#   --cell=m              with --world: the generator's cell size (8; 4 for close shore and cliff shots)
#   --form=stack|arch     with --view=coast: frame the biggest stack or arch; --form=pavement|fin|hoodoo|pothole|goblin|cave_mouth with --view=landform
#   --seaward [--side=f --cam-height=m --look-height=m] with --view=coast: from the land side, looking out to sea
#   --no-ground-blend     the stones' feet not blended into the ground (for a before frame)
#   --no-ground-contact   no damp ring or banked fines on the ground round the stones (for a before frame)
#   --fall-dust           with --fall: keep drawing while it falls, so the Flow rockfall dust is in the shot (run
#                         godot with --fixed-fps 60: Flow steps once a drawn frame); --no-dust
#                         turns the field's rockfall_dust off
#   --cave-view=stream    with --caves --cave-mesh: down the channel of the cave stream nearest the entrance
#                         (--dist m upstream, 2.5); --no-streams turns cave_streams off
#   --cave-view=rimstone  with --caves --cave-mesh: the busiest rimstone cascade from about --dist m (1.6), a lamp;
#                         --pool-water=n live water on the n nearest pools (12), --no-pool-water none
#   --no-sea-mist         with --sea-caves --cave-mesh: no mist breathed out of the mouths; --mist-period=s its swell
#                         period (run with --fixed-fps 60 and --settle=N for N frames of mist)
#   --no-moss             no moss shells on the close-up stones (the shader's moss tint only; a before frame)
#   --rain=mm/h           with --rock: rain on the stone for --rain-hours=h (0.5), then --dry-hours=h of drying;
#                         --no-rivulets for a before frame; --film=f the water film of a stream bed's stones
#   --moss=k              the field's moss_cover (0..1)
#   --cave-mouths         the retired cave-mouth portal blocks (and their terrain holes) in --world frames
#   --wind=m/s            with --water: the sea's wind speed (and some swell)
#   --shadow-filter=N     the directional soft-shadow filter quality (0 hard .. 5 ultra)
#   --water [--sea-res=N] with --world: the generator's water bodies; --sea-res sets the sea's simulated grid (cells)
#   --settle=N            N more frames before the capture (near plant cells are built lazily)
#   --out=path.png        render and quit (path traced unless --raster); --spp=N (16); --size=WxH
#   --cam=x,y,z --look=x,y,z   a fixed camera; --sun=elevation,yaw (degrees)
#   --plants[=off]        with --world: the crevice plants of the view's stones (PlantGenerator.make_stones: scree,
#                         river or shore; VegetationField3D.obstacles from the field); =off places them hidden (before
#                         shots, same camera); --dist=m sets the camera's distance from the spot
#   --only-layer          with --world: only the view's layer is placed
#   --stone-density=k     with --world: the view's layer density times k (RockField3D.set_layer_density), e.g. a dense block field
#   --no-ground-detail    with --world: RockField3D.ground_detail off (on by default: stones of 1 to 8 cm round the
#                         camera as geometry; off for --test); --detail-ref=px image height the reach is set for (1080),
#                         --detail-pixels=n (3);
#                         --detail-sync places the detail cells on the main thread (ground_detail_threads off)
#   --walk=m              with --world: stream while the camera walks m metres forward (--walk-step=m a frame, 0.1),
#                         then print the main thread's streaming time per frame (mean, 95th percentile, worst)

const TYPES := ["granite", "limestone", "sandstone", "basalt", "slate"]
const FAMILIES := ["rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block", "corestone", "sea_stack", "sea_arch", "sea_platform", "hoodoo", "pavement", "fin", "pothole", "goblin", "cave_mouth"]
const LAYERS := ["rockfall", "scree", "glacial", "river", "shore", "cliff", "tor", "coast", "landform"]
const WORLD := 2048.0

var args := {}
var raster := false
var spp := 16
var out := ""
var plants_ready := false
var fails := 0
var gen: TerrainGenerator3D
var rocks: RockField3D
var cam: Camera3D


var breaking := false
var form_stone := {} # the stone --form framed

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.substr(2).split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	raster = args.has("raster")
	spp = int(args.get("spp", "16"))
	out = args.get("out", "")
	if args.has("no-lod"):
		get_viewport().mesh_lod_threshold = 0.0
	if args.has("size"):
		var s: PackedStringArray = args["size"].split("x")
		get_window().size = Vector2i(int(s[0]), int(s[1]))
	cam = Camera3D.new()
	cam.fov = 50.0
	cam.far = 4000.0
	add_child(cam)
	_environment()
	if args.has("test"):
		await _test()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("world"):
		_world()
	elif args.has("rock"):
		_one_rock()
	elif args.has("families"):
		_families()
	else:
		_gallery()
	if args.has("cam"):
		var c: PackedStringArray = args["cam"].split(",")
		var l: PackedStringArray = args.get("look", "0,0,0").split(",")
		cam.position = Vector3(float(c[0]), float(c[1]), float(c[2]))
		cam.look_at(Vector3(float(l[0]), float(l[1]), float(l[2])))
	if args.has("sun"):
		# --sun=elevation,yaw in degrees (a low grazing sun shows relief)
		var sv: PackedStringArray = args["sun"].split(",")
		for n in get_children():
			if n is DirectionalLight3D:
				(n as DirectionalLight3D).rotation_degrees = Vector3(-float(sv[0]), float(sv[1]) if sv.size() > 1 else -50.0, 0)
	if out != "":
		# Single captures have no TAA to smooth the soft-low shadow kernel's per-pixel rotation (a dot
		# grid in penumbras): the high filter instead.
		# --shadow-filter=N overrides it (0 hard .. 5 soft ultra).
		var q := int(args.get("shadow-filter", str(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)))
		RenderingServer.directional_soft_shadow_filter_set_quality(q)
		RenderingServer.positional_soft_shadow_filter_set_quality(q)
		_render_later()


func check(ok: bool, what: String) -> void:
	print("%s  %s" % ["PASS" if ok else "FAIL", what])
	if not ok:
		fails += 1


func _environment() -> void:
	var sky: Material = ProceduralSkyMaterial.new()
	if args.has("clouds"):
		# --clouds: the engine's weather sky (clouds, physically based atmosphere) instead of the plain gradient, so
		# water has a sky worth reflecting. Clouds drawn whole each frame (no reprojection), so a still needs no
		# settling frames.
		var ws := WeatherSkyMaterial.new()
		ws.cloud_reprojection = false
		ws.set_shader_parameter("atmo_far_water", 1.0)
		# --clouds=C: the cover (0..1); a still gets three times the march's steps, as it has no frames to average.
		if args["clouds"] != "1": # (a bare --clouds keeps the sky's own cover)
			ws.set_shader_parameter("cloud_cover", float(args["clouds"]))
		ws.set_shader_parameter("cloud_steps", 192)
		sky = ws
	var sk := Sky.new()
	sk.sky_material = sky
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sk
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	if args.get("cave-view", "outside") in ["inside", "spel", "chamber", "rimstone", "stream"]:
		# Underground: no sky light reaches here, so none in raster either (the path tracer already has none).
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0, 0, 0)
		env.ambient_light_energy = 0.0
		env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_sky_shader_enabled = args.has("clouds")
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-32, -50, 0)
	sun.light_energy = 2.2
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 120.0
	add_child(sun)


func _rock(type: int, family: int, seed: int) -> ArrayMesh:
	var g := RockGenerator.new()
	g.rock_type = type
	g.family = family
	g.seed = seed
	if args.has("distance"):
		g.transport_distance = float(args["distance"])
	if args.has("weathering"):
		g.weathering = float(args["weathering"])
	g.simplify = not args.has("no-simplify")
	g.close_up = args.has("close-up")
	return g.generate()


func _plain_ground(size: float) -> void:
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(size, size)
	ground.mesh = pm
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.22, 0.2, 0.16)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)


# A rock as a lone MeshInstance3D at rest on y = 0, sunk a little, b axis `b` metres long.
func _place_mesh(mesh: ArrayMesh, b: float, pos: Vector3, yaw: float) -> void:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	var s := b * 0.5
	mi.transform = Transform3D(Basis(Vector3.UP, yaw).scaled(Vector3(s, s, s)), pos + Vector3(0, -float(mesh.get_meta("rock_bottom", -0.5)) * s - 0.04 * b, 0))
	add_child(mi)


func _gallery() -> void:
	_plain_ground(60.0)
	var t0 := Time.get_ticks_msec()
	for ti in TYPES.size():
		for fi in FAMILIES.size():
			var m := _rock(ti, fi, int(args.get("seed", "3")) + fi)
			_place_mesh(m, 1.0, Vector3((fi - 2.5) * 1.6, 0, (ti - 2.0) * 1.6), 0.4 * fi)
	print("gallery: 30 rocks in %d ms" % (Time.get_ticks_msec() - t0))
	cam.position = Vector3(0, 7.5, 9.5)
	cam.look_at(Vector3(0, 0, 0.6))


# --rain=mm/h [--rain-hours=h --dry-hours=h]: a WeatherMap3D over the bench raining for --rain-hours (0.5), then
# drying for --dry-hours (0), so the stone's wetness, its drying pattern and its rivulets show (docs/ideas/rocks.md,
# "Rain-wet rock"). --no-rivulets turns the rock material's rain_rivulets off (a before frame).
func _rain_weather() -> void:
	if not args.has("rain"):
		return
	var w = ClassDB.instantiate("WeatherMap3D")
	w.extent = 256.0
	w.resolution = 32
	w.wind_speed = 3.0
	w.cloud_cover = 0.8
	w.cloud_variation = 0.0
	w.transition_time = 0.0
	w.rain_rate = float(args["rain"])
	add_child(w)
	w.advance(float(args.get("rain-hours", "0.5")) * 3600.0)
	var dry := float(args.get("dry-hours", "0"))
	if dry > 0.0:
		w.rain_rate = 0.0
		w.advance(dry * 3600.0)


# The six families on one ground at their real relative sizes (b axis): the big three behind, the small three in
# front where the camera can read them, sun low from the left so edges and facets show.
func _families() -> void:
	_plain_ground(60.0)
	for n in get_children():
		if n is DirectionalLight3D:
			(n as DirectionalLight3D).rotation_degrees = Vector3(-24, -72, 0)
			(n as DirectionalLight3D).directional_shadow_max_distance = 20.0
	var sd := int(args.get("seed", "3"))
	var rows := [
		["granite", "rockfall", 1.3, Vector3(-2.5, 0, 0), 0.5],
		["granite", "glacial", 1.7, Vector3(0.1, 0, -0.2), 2.4],
		["sandstone", "cliff_block", 1.3, Vector3(2.8, 0, 0), 0.25],
		# The small three close to the camera, so a 7 cm pebble still reads.
		["basalt", "scree", 0.32, Vector3(-0.6, 0, 3.7), 0.9],
		["limestone", "cobble", 0.22, Vector3(0.0, 0, 3.85), 2.0],
		["slate", "pebble", 0.07, Vector3(0.62, 0, 4.6), 0.4],
	]
	for r in rows:
		var m := _rock(TYPES.find(r[0]), FAMILIES.find(r[1]), sd)
		_place_mesh(m, r[2], r[3], r[4])
	cam.position = Vector3(0.05, 0.95, 6.0)
	cam.look_at(Vector3(0.05, 0.3, 2.0))


func _one_rock() -> void:
	_plain_ground(20.0)
	_rain_weather()
	var tf: PackedStringArray = args["rock"].split(",")
	var m := _rock(TYPES.find(tf[0]), FAMILIES.find(tf[1]) if tf.size() > 1 else 0, int(args.get("seed", "3")))
	print("rock: %d triangles, axes %s" % [m.surface_get_array_index_len(0) / 3, m.get_meta("rock_axes")])
	if args.has("no-rivulets") and m.surface_get_material(0) is ShaderMaterial:
		(m.surface_get_material(0) as ShaderMaterial).set_shader_parameter("rain_rivulets", false)
	if args.has("film") and m.surface_get_material(0) is ShaderMaterial:
		# --film=f: the water film a stream bed gives its stones (INSTANCE_CUSTOM.y), contact band off.
		(m.surface_get_material(0) as ShaderMaterial).set_shader_parameter("rock_instance", Vector4(-1.0e4, float(args["film"]), 0.0, 0.5))
	var fam := FAMILIES.find(tf[1]) if tf.size() > 1 else 0
	if args.has("stand") and fam in [RockGenerator.FAMILY_FIN, RockGenerator.FAMILY_HOODOO, RockGenerator.FAMILY_SEA_STACK, RockGenerator.FAMILY_GOBLIN, RockGenerator.FAMILY_CAVE_MOUTH]:
		# --stand: on end as the field stands it (a fin's z up, a column's x up), seen side on from 6 b-axes away.
		var mi := MeshInstance3D.new()
		mi.mesh = m
		var z_up := fam == RockGenerator.FAMILY_FIN or fam == RockGenerator.FAMILY_CAVE_MOUTH
		var bs := Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0)) if z_up else Basis(Vector3(0, 1, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1))
		if fam == RockGenerator.FAMILY_CAVE_MOUTH:
			bs = Basis(Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 1, 0)) # the opening (+y) toward the camera
		var box := m.get_aabb()
		var foot := box.position.z if z_up else box.position.x
		mi.transform = Transform3D(Basis(Vector3.UP, 0.35) * bs, Vector3(0, -foot - 0.05, 0))
		add_child(mi)
		var top := box.size.z if z_up else box.size.x
		cam.position = Vector3(1.2, top * 0.45, top * 2.6 + 3.0)
		cam.look_at(Vector3(0, top * 0.4, 0))
		return
	_place_mesh(m, 1.0, Vector3.ZERO, 0.6)
	cam.position = Vector3(0.9, 0.75, 1.5)
	cam.look_at(Vector3(0, 0.2, 0))


func _world() -> void:
	var t0 := Time.get_ticks_msec()
	gen = TerrainGenerator3D.new()
	gen.name = "Generator"
	# Heavy generator options are on by default; this bench keeps its own fast choices.
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.tile_erosion_enabled = false
	gen.strata_enabled = false
	gen.guide_preview = false
	gen.dunes_enabled = false
	gen.deposition = 0.0
	gen.moraine_age = 0.0
	gen.scree_age = 0.0
	gen.meander_age = 0.0
	gen.outwash_age = 0.0
	gen.size = Vector2(WORLD, WORLD)
	gen.cell_size = float(args.get("cell", "8.0")) # --cell=4 for close shore shots (finer cliff facets)
	gen.seed = int(args.get("seed", "5"))
	gen.uplift = 3.0
	gen.base_uplift = 0.1
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	for y in 64:
		var z := -WORLD * 0.5 + (y + 0.5) / 64.0 * WORLD
		for x in 64:
			up.set_pixel(x, y, Color(0.12 + 0.88 * (1.0 - smoothstep(-500.0, 500.0, z)), 0, 0))
	gen.uplift_map = ImageTexture.create_from_image(up)
	gen.outlet_edges = 0
	gen.ocean_edges = TerrainGenerator3D.OUTLET_POSITIVE_Z
	gen.coast_distance = 300.0
	gen.lake_count = 1
	gen.strata_enabled = true
	gen.cliff_ledge_spacing = 16.0
	gen.scree_age = float(args.get("scree-age", "6000"))
	gen.moraine_age = float(args.get("moraine-age", "8000"))
	gen.detail_octaves = 5
	gen.glacial_enabled = true
	gen.glacial_ela = float(args.get("ela", "450"))
	add_child(gen)
	gen.generate()
	print("world generated in %d ms" % (Time.get_ticks_msec() - t0))
	if args.has("probe"):
		var hist := [0, 0, 0, 0, 0]
		var till := 0
		var hmax := -1e9
		for zz in range(-1000, 1000, 16):
			for xx in range(-1000, 1000, 16):
				var p := Vector3(xx, 0, zz)
				var t := gen.get_talus(p)
				hist[mini(int(t * 5.0), 4)] += 1
				if gen.get_geology_at(p).b < 0.0:
					till += 1
				hmax = maxf(hmax, gen.get_height(p))
		print("talus histogram (0-0.2..0.8-1): %s, till cells %d, highest %.0f m, ice %.2f km2" % [hist, till, hmax, gen.get_last_glacial_ice_area() / 1e6])
	var tm := TerrainMaterial3D.new()
	tm.use_generator(gen)
	tm.rock_detail_enabled = args.get("rock-detail", "1") != "0" # --rock-detail=0: the ground without its joints and stones
	tm.update_shader()
	var land := Landscape3D.new()
	land.cliff_relief = 0.0 # on by default; this bench measures without it
	land.name = "Landscape"
	add_child(land)
	land.material = tm
	land.source = land.get_path_to(gen)
	land.collision_enabled = args.has("water") # the sea's seabed_from_floor measures the bed through it
	land.camera = land.get_path_to(cam)
	land.rebuild()
	land.load_all_heights()
	if args.has("water"):
		var waters: Array = gen.build_water(self)
		for w in waters:
			# Seabed rays through terrain holes (cave mouths) land on the cave floors.
			if w is Node and (w as Node).name == "Sea" and "terrain_source" in w:
				w.set("terrain_source", (w as Node).get_path_to(gen))
		if args.has("sea-res"):
			# The sea's shallow-water grid is 16 m cells on a 2 km world: too coarse for water round a stack.
			for w in waters:
				if w is Node and (w as Node).name == "Sea":
					w.set("grid_resolution", int(args["sea-res"]))
		if args.has("wind"):
			# --wind=m/s: a rougher sea with some swell, so the foot of a stack has waves and foam on it.
			for w in waters:
				if w is Node and (w as Node).name == "Sea":
					w.set("follow_weather", false)
					w.set("wind_speed", float(args["wind"]))
					w.set("swell", 0.5)
					w.set("shore_wave_height", 1.4)
	var focus := Node3D.new()
	focus.name = "Focus"
	add_child(focus)
	if args.has("stand-ins"):
		_stand_ins()
	elif not args.has("no-rocks"):
		rocks = RockField3D.new()
		rocks.name = "Rocks"
		rocks.rock_type = maxi(TYPES.find(args.get("type", "granite")), 0)
		rocks.soft_rock_type = maxi(TYPES.find(args.get("soft-type", "limestone")), 0)
		rocks.streaming = false
		add_child(rocks)
		rocks.generator = rocks.get_path_to(gen)
		rocks.follow_target = rocks.get_path_to(focus)
		rocks.ground_material = tm # the stones' feet blend into these layers
		# Real scree stones out to the layer's radius: the ground shader keeps only their bed there (--shader-scree
		# for the old parallax blocks under them, a before frame).
		if not args.has("shader-scree"):
			tm.scree_blocks_distance = rocks.get_layer_radius(RockField3D.LAYER_SCREE)
		# On by default; --no-ground-blend for an A/B.
		rocks.ground_blend = not args.has("no-ground-blend")
		rocks.ground_contact = not args.has("no-ground-contact")
		rocks.moss_cushions = not args.has("no-moss")
		rocks.rockfall_dust = not args.has("no-dust")
		if args.has("moss"):
			rocks.moss_cover = float(args["moss"])
		# Live falls serve only the rockfall runs (--live, the test turns them on itself): off here so frames stay still.
		rocks.live_rockfall = false
		rocks.mineral_glints = not args.has("no-glints")
		# The portal block is retired (off by default; CaveNetwork3D's entrance patch replaced it): frames leave it out,
		# the test keeps covering its holes. --cave-mouths shows it.
		rocks.cave_mouths = args.has("cave-mouths") or args.has("test")
		if args.has("water"):
			# The rocks on their own physics layer: the sea's contact foam measures them, its seabed probe keeps to
			# the land (it would take the rocks for the bed).
			rocks.collision_layer = 2
			for w in get_children():
				if w is Node and (w as Node).name == "Sea" and "seabed_collision_mask" in w:
					w.set("seabed_collision_mask", 1)
					# --no-contact-foam: the sea without its rock contact foam, for a before/after pair (strength 0, so both
					# frames wait for the map alike and show the same waves).
					if args.has("no-contact-foam"):
						w.set("contact_foam_strength", 0.0)
					# --sea-prop=name:value (repeatable): one of the sea's properties.
					for pa in OS.get_cmdline_user_args():
						if pa.begins_with("--sea-prop="):
							var pkv := pa.substr(11).split(":", true, 1)
							w.set(pkv[0], str_to_var(pkv[1]))
					# --sea-uniform=name:value (repeatable): one of the sea's shader uniforms, for A/B checks.
					var wm: ShaderMaterial = w.call("get_water_material")
					for ua in OS.get_cmdline_user_args():
						if ua.begins_with("--sea-uniform=") and wm:
							var kv := ua.substr(14).split(":", true, 1)
							wm.set_shader_parameter(kv[0], str_to_var(kv[1]))
	var view: String = args.get("view", "scree")
	var spot := _densest(LAYERS.find(view)) if rocks else Vector3(0, 0, 0)
	spot.y = gen.get_height(spot)
	focus.global_position = spot
	var dist := {"rockfall": 14.0, "scree": 7.0, "glacial": 12.0, "river": 14.0, "shore": 12.0, "cliff": 30.0, "tor": 22.0, "coast": 60.0, "landform": 30.0}.get(view, 10.0) as float
	if args.has("dist"):
		dist = float(args["dist"])
	cam.position = _view_from(spot, dist)
	if view == "river" or view == "shore":
		# Low ground under steep sides: look down on the bed from higher, over the walls round it.
		cam.position.y = maxf(cam.position.y, spot.y + dist * 0.9)
	if args.has("top"):
		cam.position = spot + Vector3(0, float(args["top"]) if args["top"] != "1" else 30.0, 0.5)
	var look := spot + Vector3(0, 0.3, 0)
	if view == "coast" and args.has("seaward"):
		# --seaward: from the land side looking out, the form against sea and sky with the cliff behind the camera.
		var land_dir := Vector3.ZERO
		var top := -INF
		for i in 16:
			var d := Vector3(cos(TAU * i / 16.0), 0, sin(TAU * i / 16.0))
			var hgt := gen.get_height(spot + d * 50.0)
			if hgt > top:
				top = hgt
				land_dir = d
		var side := land_dir.cross(Vector3.UP) * float(args.get("side", "0.35"))
		cam.position = spot + (land_dir + side).normalized() * dist
		var sea_y := gen.get_sea_level() if gen.has_sea_result() else spot.y
		cam.position.y = maxf(gen.get_height(cam.position) + 2.0, sea_y + float(args.get("cam-height", "6")))
		look = Vector3(spot.x, sea_y + float(args.get("look-height", "8")), spot.z)
	if form_stone.has("basis") and args.get("form", "") == "cave_mouth" and not args.has("cam"):
		# A cave mouth from in front, at eye height a little above its floor: its basis y points out of the face.
		var ob: Basis = form_stone["basis"]
		var o := Vector3(ob.y.x, 0, ob.y.z).normalized()
		var side := Vector3(-o.z, 0, o.x) * float(args.get("side", "0.3"))
		var mouth: Vector3 = form_stone["position"]
		# The passage's mouth from its terrain hole (the hole box's front, floor middle).
		var best_d := 1e9
		for h in gen.get_holes():
			var hb: Transform3D = h
			if hb.origin.distance_to(form_stone["position"]) < best_d:
				best_d = hb.origin.distance_to(form_stone["position"])
				mouth = hb * Vector3(0, 1, -1)
		var d := float(args.get("dist", "14"))
		# Out from the face, turned up to 70 degrees either way toward the lowest ground (a mouth in a gully looks
		# out along it, not into the far bank).
		var best_dir := (o + side).normalized()
		var best_g := 1e9
		for k in range(-7, 8):
			var dir := (o + side).normalized().rotated(Vector3.UP, deg_to_rad(10.0 * k))
			var g := gen.get_height(mouth + dir * d) + 0.02 * absf(k)
			if g < best_g:
				best_g = g
				best_dir = dir
		cam.position = mouth + best_dir * d
		cam.position.y = maxf(gen.get_height(cam.position) + 1.7, mouth.y + float(args.get("cam-up", "1.6")))
		look = mouth + Vector3(0, float(args.get("look-height", "3")), 0)
	cam.look_at(look)
	print("spot %s, camera %s" % [spot, cam.position])
	print("ground at the spot: generator %.2f, landscape %.2f; camera %s" % [gen.get_height(spot), land.get_height(spot), cam.position])
	if rocks:
		if args.has("only-layer"):
			for i in LAYERS.size():
				rocks.set_layer_enabled(i, LAYERS[i] == view)
		if args.has("stone-density"):
			rocks.set_layer_density(LAYERS.find(view), float(args["stone-density"]))
		# Ground detail: the stones under min_size near the camera as real geometry (docs/ideas/ground-detail.md);
		# --detail-ref=px the image height the reach is set for (0: the window's), --detail-pixels=n the smallest drawn.
		rocks.ground_detail = not (args.has("no-ground-detail") or args.has("test"))
		if rocks.ground_detail:
			rocks.ground_detail_reference_height = int(args.get("detail-ref", "1080"))
			if args.has("detail-pixels"):
				rocks.ground_detail_pixels = float(args["detail-pixels"])
			rocks.ground_detail_threads = not args.has("detail-sync")
		var tb := Time.get_ticks_msec()
		var built := rocks.build_area(Rect2(spot.x - 160, spot.z - 160, 320, 320))
		print("rocks: %d cells, %d stones in %d ms (shape libraries %.0f ms, %d library triangles)" % [built, rocks.get_stone_count(), Time.get_ticks_msec() - tb, rocks.get_last_library_msec(), rocks.get_library_triangle_count()])
		if rocks.ground_detail:
			print("ground detail: pebbles %d within %.1f m, gravel %d within %.1f m, fine gravel %d within %.1f m" % [
				rocks.get_ground_detail_count(RockField3D.LAYER_DETAIL_PEBBLE), rocks.get_layer_radius(RockField3D.LAYER_DETAIL_PEBBLE),
				rocks.get_ground_detail_count(RockField3D.LAYER_DETAIL_GRAVEL), rocks.get_layer_radius(RockField3D.LAYER_DETAIL_GRAVEL),
				rocks.get_ground_detail_count(RockField3D.LAYER_DETAIL_FINE), rocks.get_layer_radius(RockField3D.LAYER_DETAIL_FINE)])
		if gen.get_hole_count() > 0:
			print("holes: %d in the generator, %d landscape chunks, %d quads" % [gen.get_hole_count(), land.get_hole_chunk_count(), land.get_hole_quad_count()])
			if args.has("hole-dump"):
				for h in gen.get_holes():
					var hb: Transform3D = h
					if hb.origin.distance_to(spot) < float(args.get("hole-radius", "60")):
						print("  hole at %s, half extents %.1f %.1f %.1f" % [hb.origin, hb.basis.x.length(), hb.basis.y.length(), hb.basis.z.length()])
				for st in rocks.get_stones(RockField3D.LAYER_LANDFORM, Rect2(spot.x - 60, spot.z - 60, 120, 120)):
					if int(st["family"]) == RockGenerator.FAMILY_CAVE_MOUTH:
						print("  cave block at %s, scale %.2f, out %s" % [st["position"], st["scale"], (st["basis"] as Basis).y.normalized()])
		if args.has("plants"):
			_crevice_plants(spot, view, land)
		if args.has("break"):
			_break_near(spot, land)
		if args.has("fall"):
			_fall_near(spot, land)
		if args.has("live"):
			_live_falls(spot, land)
	if args.has("caves"):
		_caves(cam)


# Cave networks (CaveNetwork3D, docs/ideas/rocks.md "Real caves"): grown from the first entrances found in the
# world, the skeleton drawn through the ground (colour by passage type) from above and in front.
func _caves(cam: Camera3D) -> void:
	var net := CaveNetwork3D.new()
	net.name = "Caves"
	add_child(net)
	net.generator = net.get_path_to(gen)
	if rocks:
		net.rock_field = net.get_path_to(rocks)
	if args.has("cave-length"):
		net.cave_length = float(args["cave-length"])
	if args.has("gamma"):
		net.gamma = float(args["gamma"])
	var r := gen.get_world_rect()
	var t0 := Time.get_ticks_msec()
	var sea_caves := args.has("sea-caves")
	var found: Array = net.find_sea_entrances(r, int(args.get("caves", "1")) if str(args["caves"]).is_valid_int() else 1) if sea_caves else net.find_entrances(r, int(args.get("caves", "1")) if str(args["caves"]).is_valid_int() else 1)
	print("caves: %d entrances found in %d ms" % [found.size(), Time.get_ticks_msec() - t0])
	var colours := [Color(1, 1, 1), Color(0.2, 0.5, 1), Color(0.1, 0.9, 0.9), Color(1, 0.8, 0.1), Color(1, 0.3, 0.1), Color(0.9, 0.2, 0.9), Color(0.2, 0.9, 0.3)]
	var names := ["entrance", "tube", "bed", "keyhole", "canyon", "shaft", "sea"]
	for e in found:
		var id := net.add_sea_cave(e["position"], e["out"]) if sea_caves else net.add_cave(e["position"], e["out"])
		if id < 0:
			print("  entrance at %s: no network" % [e["position"]])
			continue
		var segs := net.get_segments(id)
		var counts := {}
		var length := 0.0
		var lo := 1e9
		var hi := -1e9
		for sg in segs:
			var nm: String = names[int(sg["type"])]
			var l: float = (sg["a"] as Vector3).distance_to(sg["b"])
			counts[nm] = counts.get(nm, 0.0) + l
			length += l
			lo = minf(lo, minf(sg["a"].y, sg["b"].y))
			hi = maxf(hi, maxf(sg["a"].y, sg["b"].y))
		var per := []
		for k in counts:
			per.append("%s %.0f m" % [k, counts[k]])
		print("  cave at %s out %s: %d segments, %.0f m of passage (%s), %d chambers, %d key points, %.0f m deep to %.0f m up; %d samples, %.0f ms" % [e["position"], e["out"], segs.size(), length, ", ".join(per), net.get_chambers(id).size(), net.get_key_points(id).size(), e["position"].y - lo, hi - e["position"].y, net.get_last_sample_count(), net.get_last_build_msec()])
		# Every passage must stay under the ground with some rock over it.
		var out_of_rock := 0
		for sg in segs:
			if int(sg["type"]) == CaveNetwork3D.PASSAGE_ENTRANCE:
				continue
			var m: Vector3 = (sg["a"] + sg["b"]) * 0.5
			if m.y > gen.get_height(m) - 1.0:
				out_of_rock += 1
		print("  %d segments with under 1 m of rock over them" % out_of_rock)
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_colors = true
		var cyl := CylinderMesh.new()
		cyl.top_radius = 1.0
		cyl.bottom_radius = 1.0
		cyl.height = 1.0
		cyl.radial_segments = 6
		cyl.rings = 1
		mm.mesh = cyl
		mm.instance_count = segs.size()
		for i in segs.size():
			var a: Vector3 = segs[i]["a"]
			var b: Vector3 = segs[i]["b"]
			var y := (b - a)
			var l := y.length()
			var yn := y / maxf(l, 1e-4)
			var x := yn.cross(Vector3.UP if absf(yn.y) < 0.9 else Vector3.RIGHT).normalized()
			var z := x.cross(yn)
			var rr := 0.5 * (float(segs[i]["radius_a"]) + float(segs[i]["radius_b"]))
			mm.set_instance_transform(i, Transform3D(Basis(x * rr, y, z * rr), (a + b) * 0.5))
			mm.set_instance_color(i, colours[int(segs[i]["type"])])
		var mi := MultiMeshInstance3D.new()
		mi.multimesh = mm
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.vertex_color_use_as_albedo = true
		mat.no_depth_test = true
		mat.render_priority = 1
		mi.material_override = mat
		add_child(mi)
		for ch in net.get_chambers(id):
			var sm := SphereMesh.new()
			sm.radius = 1.0
			sm.height = 2.0
			var ci := MeshInstance3D.new()
			ci.mesh = sm
			ci.scale = ch["radii"]
			ci.position = ch["center"]
			var cm := mat.duplicate()
			cm.vertex_color_use_as_albedo = false
			cm.albedo_color = Color(0.2, 1.0, 0.3)
			ci.material_override = cm
			add_child(ci)
		if args.has("cave-mesh"):
			mi.visible = false
		if id == 0 and args.has("cave-mesh"):
			var land_m: Landscape3D = get_node("Landscape")
			net.ground_material = land_m.material
			if args.has("cell"):
				net.mesh_cell = float(args["cell"])
			var mr := float(args.get("mesh-radius", "50"))
			var pos: Vector3 = e["position"]
			net.rimstone_water = not args.has("no-pool-water")
			net.sea_mist = not args.has("no-sea-mist")
			net.cave_streams = not args.has("no-streams")
			if args.has("mist-period"):
				# The mist node is made on the network's first frames: set its swell period once it is there.
				var mp := float(args["mist-period"])
				var set_period := func():
					var mn: Node = net.get_sea_mist_node()
					if mn:
						mn.set("swell_period", mp)
						print("  sea mist: swell period %.1f s" % mp)
				get_tree().create_timer(0.05).timeout.connect(set_period)
			if args.has("pool-water"):
				net.rimstone_water_max = int(args["pool-water"])
			var made := net.build_meshes(AABB(pos - Vector3(mr, mr, mr), Vector3(2 * mr, 2 * mr, 2 * mr)))
			print("  meshes: %d chunks, %d triangles in %.0f ms (cell %.2f m)" % [made, net.get_mesh_triangle_count(), net.get_last_mesh_msec(), net.mesh_cell])
			land_m.load_all_heights()
			var tri := {"walls": 0, "ground": 0}
			for ch in net.get_children():
				if ch is MeshInstance3D:
					var m: ArrayMesh = ch.mesh
					for si in m.get_surface_count():
						tri[m.surface_get_name(si)] += m.surface_get_array_index_len(si) / 3
			print("  surfaces: %d wall triangles, %d ground triangles; holes %d, landscape quads %d" % [tri["walls"], tri["ground"], gen.get_hole_count(), land_m.get_hole_quad_count()])
			var kinds := {}
			for ch in net.get_children():
				for mmi in ch.get_children():
					if mmi is MultiMeshInstance3D:
						var kn: String = String(mmi.name).split(" ")[0]
						kinds[kn] = kinds.get(kn, 0) + (mmi.multimesh as MultiMesh).instance_count
			print("  speleothems: %d %s" % [net.get_speleothem_count(), kinds])
			var pools: Array = net.get_rimstone_pools()
			var rim_tris := 0
			for ch in net.get_children():
				for rc in ch.get_children():
					if rc is MeshInstance3D and String(rc.name) == "Rimstone":
						rim_tris += (rc.mesh as ArrayMesh).surface_get_array_index_len(0) / 3
			print("  rimstone: %d pools, %d dam triangles" % [pools.size(), rim_tris])
			for pl in pools:
				var rr := 0.0
				for v in (pl["outline"] as PackedVector2Array):
					rr += v.length()
				print("    pool at %s depth %.3f m, mean radius %.2f m" % [pl["center"], pl["depth"], rr / maxf((pl["outline"] as PackedVector2Array).size(), 1)])
			for c in get_children():
				if c is MeshInstance3D and c.material_override is StandardMaterial3D and (c.material_override as StandardMaterial3D).no_depth_test:
					c.visible = false
			# Debug: --hide=land|ground|walls hides one of the three surfaces; --tint colours ground red, walls blue.
			if args.get("hide", "") == "land":
				land_m.visible = false
			for ch in net.get_children():
				if ch is MeshInstance3D:
					var m2: ArrayMesh = ch.mesh
					for si in m2.get_surface_count():
						var nm := m2.surface_get_name(si)
						if args.get("hide", "") == nm:
							var inv := StandardMaterial3D.new()
							inv.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
							inv.albedo_color = Color(0, 0, 0, 0)
							ch.set_surface_override_material(si, inv)
						elif args.has("tint"):
							var tm := StandardMaterial3D.new()
							tm.albedo_color = Color(1, 0.2, 0.2) if nm == "ground" else Color(0.3, 0.5, 1)
							ch.set_surface_override_material(si, tm)
			var o: Vector3 = e["out"]
			var f := -o
			if args.get("cave-view", "outside") == "inside":
				# From the first metres of passage, looking in along it, a lamp at the camera (a caver's).
				var path: Array = []
				for sg in segs:
					path.append(sg["a"])
				var eye: Vector3 = pos + f * float(args.get("in", "4")) + Vector3(0, 1.6, 0)
				var tgt: Vector3 = eye + f * 10.0
				var best := 1e9
				for p in path:
					var dd: float = (p as Vector3).distance_to(eye + f * 12.0)
					if dd < best:
						best = dd
						tgt = p
				if float(args.get("in", "4")) > 10.0:
					# Deep in: walk the passage graph from the entrance along its longest branch and stand on it.
					var adj := {}
					for sg in segs:
						var ka: Vector3 = (sg["a"] as Vector3).snapped(Vector3(0.01, 0.01, 0.01))
						var kb: Vector3 = (sg["b"] as Vector3).snapped(Vector3(0.01, 0.01, 0.01))
						if not adj.has(ka): adj[ka] = []
						if not adj.has(kb): adj[kb] = []
						adj[ka].append(kb)
						adj[kb].append(ka)
					var root: Vector3 = (segs[0]["a"] as Vector3).snapped(Vector3(0.01, 0.01, 0.01))
					var dist := {root: 0.0}
					var parent := {}
					var queue := [root]
					while not queue.is_empty():
						var cur: Vector3 = queue.pop_front()
						for nb in adj.get(cur, []):
							if not dist.has(nb):
								dist[nb] = dist[cur] + cur.distance_to(nb)
								parent[nb] = cur
								queue.append(nb)
					var far: Vector3 = root
					for k in dist:
						if dist[k] > dist[far]:
							far = k
					var chain := [far]
					while parent.has(chain[-1]):
						chain.append(parent[chain[-1]])
					chain.reverse()
					var want_d := float(args["in"])
					for i in chain.size():
						if dist[chain[i]] >= want_d:
							eye = chain[i] + Vector3(0, float(args.get("eye-up", "0.3")), 0)
							for j in range(i, chain.size()):
								if dist[chain[j]] >= want_d + 7.0:
									tgt = chain[j]
									break
							break
				cam.position = eye
				cam.look_at(tgt + Vector3(0, 0.5, 0))
				if args.has("look-out"):
					# From inside back out through the mouth (a sea cave onto the lit sea).
					cam.look_at(pos + Vector3(0, float(args.get("look-height", "1.0")), 0) + o * 6.0)
				if args.has("lamp"):
					var lamp := SpotLight3D.new()
					lamp.spot_range = 40.0
					lamp.spot_angle = 35.0
					lamp.light_energy = 6.0
					lamp.shadow_enabled = true
					cam.add_child(lamp)
			elif args.get("cave-view", "outside") in ["spel", "chamber"]:
				# Where the speleothems are thickest: from open air a few metres off their middle, a lamp at the camera.
				var best_n := 0
				var centre := pos
				for ch in net.get_children():
					var n_here := 0
					var sum := Vector3()
					for mmi in ch.get_children():
						if mmi is MultiMeshInstance3D:
							var smm: MultiMesh = mmi.multimesh
							for ii in smm.instance_count:
								sum += smm.get_instance_transform(ii).origin
								n_here += 1
					if n_here > best_n:
						best_n = n_here
						centre = sum / n_here
				if args.get("cave-view", "") == "chamber" and not net.get_chambers(0).is_empty():
					# The first chamber: its centre, a metre over its floor.
					var chm: Dictionary = net.get_chambers(0)[0]
					centre = (chm["center"] as Vector3) - Vector3(0, 0.55 * (chm["radii"] as Vector3).y - 1.0, 0)
				var eye_s := centre
				var best_air := -1.0
				seed(7)
				for ii in 600:
					var cand := centre + Vector3(randf_range(-6, 6), randf_range(-1.5, 2.5), randf_range(-6, 6))
					var dd := cand.distance_to(centre)
					if dd < 4.0 or dd > 6.5:
						continue
					var air := -net.get_rock_distance(cand)
					var score := minf(air, 1.5)
					if air > 0.8 and score > best_air:
						best_air = score
						eye_s = cand
				cam.position = eye_s
				cam.look_at(centre + Vector3(0, 0.5, 0))
				cam.fov = 65.0
				# One lamp off to the side (a caver standing beside the camera), so the forms model in side light.
				var side_dir := (centre - eye_s).cross(Vector3.UP).normalized()
				var lamp_s := OmniLight3D.new()
				lamp_s.omni_range = 18.0
				lamp_s.light_energy = float(args.get("lamp-energy", "3.0"))
				lamp_s.shadow_enabled = true
				add_child(lamp_s)
				var lp := eye_s + side_dir * 2.5 + Vector3(0, 0.6, 0)
				for k in 8:
					if net.get_rock_distance(lp) < -0.4:
						break
					lp = lp.lerp(eye_s, 0.3)
				lamp_s.global_position = lp
				print("  speleothem view: %d in the chunk round %s, camera %s" % [best_n, centre, eye_s])
			elif args.get("cave-view", "outside") == "rimstone":
				# The biggest pool (depth times reach) with the most pools round it: from about --dist m off and a metre up,
				# a lamp beside the camera.
				var pools_v: Array = net.get_rimstone_pools()
				var pc := pos
				var best_k := -1
				var best_c := 0
				var best_score := -1.0
				for k in pools_v.size():
					var n_near := 0
					for k2 in pools_v.size():
						if (pools_v[k2]["center"] as Vector3).distance_to(pools_v[k]["center"]) < 3.0:
							n_near += 1
					var rr := 0.0
					for v in (pools_v[k]["outline"] as PackedVector2Array):
						rr += v.length() / 32.0
					var score: float = float(pools_v[k]["depth"]) * rr * (1.0 + 0.3 * n_near)
					if score > best_score:
						best_score = score
						best_c = n_near
						best_k = k
				if best_k >= 0:
					pc = pools_v[best_k]["center"]
				var eye_r := pc + Vector3(0.9, 1.3, 0.6)
				var best_air_r := -1.0
				seed(11)
				for ii in 400:
					var cand := pc + Vector3(randf_range(-3, 3), randf_range(0.25, 0.45) if args.has("low") else randf_range(0.8, 1.8), randf_range(-3, 3))
					var dd := Vector2(cand.x - pc.x, cand.z - pc.z).length()
					if dd < float(args.get("dist", "1.6")) * 0.8 or dd > float(args.get("dist", "1.6")) * 1.3:
						continue
					var air := -net.get_rock_distance(cand)
					if air > 0.25 and minf(air, 1.0) > best_air_r:
						best_air_r = minf(air, 1.0)
						eye_r = cand
				cam.position = eye_r
				# --low: a grazing view across the pool, so it mirrors the roof and its stalactites beyond.
				cam.look_at(pc + (Vector3(pc.x - eye_r.x, 0, pc.z - eye_r.z).normalized() * 1.5 + Vector3(0, 0.15, 0) if args.has("low") else Vector3.ZERO))
				cam.fov = 60.0
				var lamp_r := OmniLight3D.new()
				lamp_r.omni_range = 14.0
				lamp_r.light_energy = float(args.get("lamp-energy", "2.0"))
				lamp_r.shadow_enabled = true
				add_child(lamp_r)
				var side_r := (pc - eye_r).cross(Vector3.UP).normalized()
				var lp_r := eye_r + side_r * 1.2 + Vector3(0, 0.4, 0)
				for k in 8:
					if net.get_rock_distance(lp_r) < -0.3:
						break
					lp_r = lp_r.lerp(eye_r, 0.3)
				lamp_r.global_position = lp_r
				print("  rimstone view: pool %d of %d (%d within 3 m) at %s, camera %s" % [best_k, pools_v.size(), best_c, pc, eye_r])
				get_tree().create_timer(1.0).timeout.connect(func():
					print("  rimstone water: %d pools with live water" % net.get_rimstone_water_count())
					_screen_reflections(net)
					# The drawn floor under each pool's middle against its level (the floor must lie under the water).
					var space := get_world_3d().direct_space_state
					for pl in pools_v:
						var c: Vector3 = pl["center"]
						var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(c + Vector3(0, 0.5, 0), c - Vector3(0, 1.0, 0)))
						print("    pool floor at middle: %s m against the level (depth %.3f)" % [("%.3f" % (hit["position"].y - c.y)) if hit else "none", pl["depth"]])
					# The focal pool: the share of its outline's inside where the drawn floor stands over the water.
					var fc: Vector3 = pools_v[best_k]["center"]
					var ol: PackedVector2Array = pools_v[best_k]["outline"]
					var above := 0
					var total := 0
					var worst := -1.0
					for ri in ol.size():
						for fr in [0.2, 0.4, 0.6, 0.8, 0.95]:
							var q: Vector3 = fc + Vector3(ol[ri].x, 0, ol[ri].y) * fr
							var h2 := space.intersect_ray(PhysicsRayQueryParameters3D.create(q + Vector3(0, 0.5, 0), q - Vector3(0, 1.0, 0)))
							if h2:
								total += 1
								var dy: float = h2["position"].y - fc.y
								worst = maxf(worst, dy)
								if dy > 0.0:
									above += 1
					print("    focal pool: floor over the water at %d of %d samples, highest %.3f m" % [above, total, worst])
					# Hanging edges: rays whose floor at the outline lies well under the level (the sheet's edge in the air).
					for pi2 in pools_v.size():
						var pc2: Vector3 = pools_v[pi2]["center"]
						if pc2.distance_to(fc) > 5.0:
							continue
						var hang := 0
						var lowest := 0.0
						for v2 in (pools_v[pi2]["outline"] as PackedVector2Array):
							var q2: Vector3 = pc2 + Vector3(v2.x, 0, v2.y) * 0.97
							var h3 := space.intersect_ray(PhysicsRayQueryParameters3D.create(q2 + Vector3(0, 0.5, 0), q2 - Vector3(0, 1.0, 0)))
							var q4: Vector3 = pc2 + Vector3(v2.x, 0, v2.y) * 1.08
							var h4 := space.intersect_ray(PhysicsRayQueryParameters3D.create(q4 + Vector3(0, 0.5, 0), q4 - Vector3(0, 1.0, 0)))
							if OS.get_environment("POOL_DEBUG") == "1":
								print("      ray r=%.2f in %.3f out %s" % [v2.length(), (h3["position"].y - pc2.y) if h3 else 9.0, ("%.3f" % (h4["position"].y - pc2.y)) if h4 else "none"])
							if h3 and h3["position"].y - pc2.y < -0.03:
								hang += 1
								lowest = minf(lowest, h3["position"].y - pc2.y)
						print("    pool %d at %s: %d of 32 edge rays hang, floor down to %.3f m" % [pi2, pc2, hang, lowest])
					for nd in net.find_children("Pool *", "", true, false):
						var wmi: MeshInstance3D = null
						for ch in nd.find_children("*", "MeshInstance3D", true, false):
							wmi = ch
							break
						if wmi and wmi.mesh and (nd as Node3D).global_position.distance_to(fc) < 0.01:
							print("    focal water node %s at %s, mesh aabb %s, mesh node at %s" % [nd.get_class(), (nd as Node3D).global_position, wmi.mesh.get_aabb(), wmi.global_position]))
			elif args.get("cave-view", "outside") == "stream":
				# A cave stream: the stream segment nearest the entrance, from 2.5 m upstream at the passage line,
				# looking down the channel; a lamp beside the camera.
				var st_segs: Array = net.get_segments(0)
				var best_s := -1
				var best_d := 1e9
				# The nearest long, gently sloping stream segment (a short or steep one leaves no channel to look down).
				for k in st_segs.size():
					if st_segs[k]["stream"]:
						var ln := (st_segs[k]["b"] as Vector3) - (st_segs[k]["a"] as Vector3)
						# Also a wide one: a narrow trench puts the walls in the lens.
						var dk: float = (st_segs[k]["a"] as Vector3).distance_to(pos) - 4.0 * minf(ln.length(), 8.0) + (20.0 if absf(ln.normalized().y) > 0.25 else 0.0) - 12.0 * float(st_segs[k]["radius_a"])
						if dk < best_d:
							best_d = dk
							best_s = k
				var sa: Vector3 = st_segs[best_s]["a"] if best_s >= 0 else pos
				var sb: Vector3 = st_segs[best_s]["b"] if best_s >= 0 else pos + f
				var rr_s: float = float(st_segs[best_s]["radius_a"]) if best_s >= 0 else 1.0
				var mid := (sa + sb) * 0.5
				# The stream may lie past the meshed ball round the entrance: mesh round it too.
				net.build_meshes(AABB(mid - Vector3(24, 24, 24), Vector3(48, 48, 48)))
				var dir := (sb - sa).normalized()
				if sb.y > sa.y:
					dir = -dir # look downstream
				var eye_s := mid - dir * float(args.get("dist", "2.5")) + Vector3(0, -0.2 * rr_s, 0)
				# Low over the water, as a caver crouched in the stream: 0.4 m over the bed under the eye.
				var fl_hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(eye_s + Vector3(0, 1.0, 0), eye_s - Vector3(0, 3.0 * rr_s, 0)))
				if fl_hit:
					eye_s.y = fl_hit["position"].y + float(args.get("eye-height", "0.4"))
				print("  stream view: bed under the eye %s" % ("found" if fl_hit else "not found (collision not in the space yet)"))
				# Inside the air, with room round the lens.
				for k in 10:
					if net.get_rock_distance(eye_s) < -0.35:
						break
					eye_s = eye_s.lerp(mid, 0.25)
				cam.position = eye_s
				# Down the channel, a little below level; a narrower lens so the walls enter the frame only past a metre.
				cam.look_at(Vector3(mid.x, eye_s.y, mid.z) + dir * 3.0 - Vector3(0, 0.35, 0))
				cam.fov = 50.0
				var lamp_st := OmniLight3D.new()
				lamp_st.omni_range = 14.0
				lamp_st.light_energy = float(args.get("lamp-energy", "1.2"))
				lamp_st.shadow_enabled = true
				add_child(lamp_st)
				# Overhead and a little behind the camera, as a helmet lamp: no wall close enough to burn out.
				lamp_st.global_position = eye_s - dir * 0.2 + Vector3(0, 0.15, 0)
				if args.has("graze"):
					# --graze: a second lamp downstream, low over the water (another caver's, out of shot): its light grazes the
					# surface toward the lens, so the water shows as a glinting sheet, as it does to a caver.
					var lamp_g := OmniLight3D.new()
					lamp_g.omni_range = 10.0
					lamp_g.light_energy = float(args.get("graze-energy", "1.5"))
					lamp_g.shadow_enabled = true
					add_child(lamp_g)
					# Kept on this segment's own water, short of its end: past it the stream may drop down a cascade, and a
					# lamp down in the plunge would light the whole passage through the fall's water.
					var ahead := minf(float(args.get("graze-dist", "5.0")), float(args.get("dist", "2.5")) + 0.5 * (sb - sa).length() - 0.4)
					var g_at := eye_s + dir * maxf(ahead, 1.5)
					var bed_eye := eye_s.y - float(args.get("eye-height", "0.4"))
					var g_hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(g_at.x, bed_eye + 0.8, g_at.z), Vector3(g_at.x, bed_eye - 0.6, g_at.z)))
					g_at.y = (g_hit["position"].y if g_hit else bed_eye) + 0.2
					for k in 8:
						if net.get_rock_distance(g_at) < -0.15:
							break
						g_at = g_at.lerp(Vector3(mid.x, g_at.y, mid.z), 0.3)
					lamp_g.global_position = g_at
					print("  stream view: grazing lamp %.1f m ahead, %.2f m over the eye's bed" % [ahead, g_at.y - bed_eye])
				var n_stream := 0
				for sgx in st_segs:
					n_stream += 1 if sgx["stream"] else 0
				print("  stream view: segment %d (%d of %d segments carry streams), radius %.2f, camera %s" % [best_s, n_stream, st_segs.size(), rr_s, eye_s])
				get_tree().create_timer(0.5).timeout.connect(func():
					print("  cave streams: %d" % net.get_stream_count())
					if OS.get_environment("POOL_DEBUG") == "1":
						for wn2 in get_tree().root.find_children("*", "Node3D", true, false):
							if wn2.get_class().contains("Water") or wn2.get_class().contains("River") or wn2.get_class().contains("Ocean") or wn2.get_class().contains("Lake"):
								for wm2 in wn2.find_children("*", "MeshInstance3D", true, false):
									var gb2: AABB = (wm2 as MeshInstance3D).global_transform * (wm2 as MeshInstance3D).get_aabb()
									if gb2.grow(10.0).has_point(cam.global_position):
										print("    water node %s %s mesh aabb %s" % [wn2.get_class(), wn2.name, gb2])
						for wm in net.find_children("*", "MeshInstance3D", true, false):
							var gb: AABB = (wm as MeshInstance3D).global_transform * (wm as MeshInstance3D).get_aabb()
							print("    water mesh %s aabb %s contains eye %s" % [wm.get_parent().name, gb, gb.has_point(cam.global_position)])
					_screen_reflections(net))
			elif args.get("cave-view", "outside") == "top":
				cam.position = pos + o * 10.0 + Vector3(0, float(args.get("dist", "70")), 0)
				cam.look_at(pos, f)
			else:
				var d := float(args.get("dist", "18"))
				cam.position = pos + o * d + Vector3(0, 1.7, 0)
				cam.position.y = maxf(cam.position.y, gen.get_height(cam.position) + 1.7)
				cam.look_at(pos + Vector3(0, float(args.get("look-height", "3")), 0))
			land_m.update_lods_from_camera()
		elif id == 0:
			var o: Vector3 = e["out"]
			var c: Vector3 = e["position"] - o * net.cave_length * 0.45
			cam.position = c + o * net.cave_length * 0.9 + Vector3(0, net.cave_length * 0.7, 0)
			cam.look_at(c)


# --no-screen-reflections: the cave water without its screen-space reflections (the water shader's uniform), for an
# off/on pair from one camera.
func _screen_reflections(root: Node) -> void:
	var on := not args.has("no-screen-reflections")
	var n := 0
	for w in root.find_children("*", "", true, false):
		if w.get_class() in ["PhysXWaterSurface3D", "PhysXRiver3D"]:
			for mi in w.find_children("*", "MeshInstance3D", true, false):
				var m := (mi as MeshInstance3D).material_override as ShaderMaterial
				if m:
					m.set_shader_parameter("screen_reflections", on)
					n += 1
	print("  screen reflections %s on %d water meshes" % ["on" if on else "off", n])


# The pieces against the stone: their total volume and the box round them as drawn (each chunk through its
# piece's pose and the node's scale), against the intact chunk at the stone's own transform.
func _piece_stats(node: Node, stone_xf: Transform3D) -> Dictionary:
	var asset: Resource = node.get("blast_asset")
	var pts: Array = asset.get("chunk_points")
	var s := (node as Node3D).global_transform.basis.get_scale()
	var p0: PackedVector3Array = pts[0]
	var whole := 0.0
	var src := AABB(stone_xf * p0[0], Vector3.ZERO)
	for t in range(0, p0.size(), 3):
		whole += p0[t].dot(p0[t + 1].cross(p0[t + 2]))
		src = src.expand(stone_xf * p0[t]).expand(stone_xf * p0[t + 1]).expand(stone_xf * p0[t + 2])
	whole = absf(whole) / 6.0 * absf(stone_xf.basis.determinant())
	var vol := 0.0
	var box := AABB()
	var first := true
	for i in node.get_piece_count():
		var pose: Transform3D = node.get_piece_transform(i)
		var xf := Transform3D(pose.basis.orthonormalized().scaled_local(s), pose.origin)
		var cp: PackedVector3Array = pts[node.get_piece_chunk(i)]
		var v := 0.0
		for t in range(0, cp.size(), 3):
			v += cp[t].dot(cp[t + 1].cross(cp[t + 2]))
			for k in 3:
				var q := xf * cp[t + k]
				if first:
					box = AABB(q, Vector3.ZERO)
					first = false
				box = box.expand(q)
		vol += absf(v) / 6.0 * absf(xf.basis.determinant())
	return {"volume": vol, "stone_volume": whole, "box": box, "stone_box": src}


func _report_pieces(node: Node, stone_xf: Transform3D) -> void:
	if node == null:
		return
	var st := _piece_stats(node, stone_xf)
	var ys := []
	for i in node.get_piece_count():
		var o: Vector3 = node.get_piece_transform(i).origin
		ys.append("%.1f" % (o.y - gen.get_height(o)))
	print("piece heights above ground: ", ys)
	print("pieces: %d, volume %.3f m3 against the stone's %.3f m3, spread %s against the stone's %s" % [node.get_piece_count(), st["volume"], st["stone_volume"], (st["box"] as AABB).size, (st["stone_box"] as AABB).size])


# --break[=frames]: break the biggest stone near the spot (RockField3D.break_stone) and let the pieces fall for that
# many physics ticks (default 120) before the shot; --break-cam=dist frames the pieces from that far.
func _break_near(spot: Vector3, land: Landscape3D) -> void:
	land.collision_enabled = true
	land.rebuild()
	land.load_all_heights()
	rocks.collision_enabled = true
	rocks.rebuild()
	rocks.build_area(Rect2(spot.x - 40, spot.z - 40, 80, 80))
	var near: Array = rocks.get_stones(LAYERS.find(args.get("view", "scree")), Rect2(spot.x - 14, spot.z - 14, 28, 28))
	# A block of about 1.1 to 1.6 m on flat ground; the stones touching it are taken away (broken and freed before
	# anything is drawn, in both shots), so the before reads as one rock and the after as its pieces.
	var big: Dictionary = {}
	for st in near:
		var sc0: float = st["scale"]
		var p: Vector3 = st["position"]
		var flat := absf(gen.get_height(p + Vector3(1.5, 0, 0)) - gen.get_height(p - Vector3(1.5, 0, 0))) < 0.3 and absf(gen.get_height(p + Vector3(0, 0, 1.5)) - gen.get_height(p - Vector3(0, 0, 1.5))) < 0.3
		if flat and sc0 >= 0.55 and sc0 <= 0.8 and (big.is_empty() or sc0 > float(big["scale"])):
			big = st
	if not big.is_empty():
		for o in near:
			if o != big and (o["position"] as Vector3).distance_to(big["position"]) < (float(o["scale"]) + float(big["scale"])) * 1.4 + 0.3:
				var gone: Node = rocks.break_stone(o["position"], 0.0, 0.05)
				if gone:
					gone.free()
	if big.is_empty():
		print("no stone to break near the spot")
		return
	var at: Vector3 = big["position"]
	var sc: float = big["scale"]
	var d := float(args.get("break-cam", "0"))
	if d > 0.0:
		cam.position = _view_from(at, d)
		cam.look_at(at)
	var ticks := int(args["break"]) if args["break"] != "1" else 120
	if ticks <= 0:
		print("intact %.2f m stone at %s (before)" % [2.0 * sc, at])
		return
	breaking = true
	var hit := at + Vector3(0, sc * 0.6, 0)
	var node: Node = rocks.break_stone(hit, float(args.get("damage", "100")), 3.0 * sc)
	print("broke a %.2f m stone at %s into %d pieces" % [2.0 * sc, at, node.get_piece_count() if node else 0])
	RenderingServer.render_loop_enabled = false
	for i in ticks:
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	_report_pieces(node, Transform3D(big["basis"], at))
	breaking = false


# --fall[=ticks]: loose a cliff block near the spot (RockField3D.trigger_rockfall) and let it fall for that many
# physics ticks (default 300) before the shot; prints its path. --fall-cam=dist frames where the pieces came to rest.
func _fall_near(spot: Vector3, land: Landscape3D) -> void:
	land.collision_enabled = true
	land.rebuild()
	land.load_all_heights()
	rocks.collision_enabled = true
	rocks.rebuild()
	rocks.build_area(Rect2(spot.x - 80, spot.z - 80, 160, 160))
	breaking = true
	var r := await _rockfall_run(spot, float(args.get("fall-radius", "40")), int(args["fall"]) if args["fall"] != "1" else 300, true)
	if not r.is_empty():
		var d := float(args.get("fall-cam", "0"))
		if d > 0.0:
			var at: Vector3 = r["end"]
			cam.position = _view_from(at, d)
			cam.look_at(at)
	breaking = false


# --live[=frames]: live_rockfall at a very high rate round the spot for that many frames (default 240), printing each
# fall and the slowest frame.
func _live_falls(spot: Vector3, land: Landscape3D) -> void:
	land.collision_enabled = true
	land.rebuild()
	land.load_all_heights()
	rocks.collision_enabled = true
	rocks.rebuild()
	rocks.build_area(Rect2(spot.x - 120, spot.z - 120, 240, 240))
	breaking = true
	var n := [0]
	var cb := func(p: Vector3, m: float) -> void:
		n[0] += 1
		print("  fall %d: %.0f kg at %s" % [n[0], m, p])
	rocks.rockfall.connect(cb)
	rocks.live_rockfall_rate = float(args.get("rate", "36000"))
	rocks.live_rockfall = true
	var frames := int(args["live"]) if args["live"] != "1" else 240
	var slowest := 0.0
	RenderingServer.render_loop_enabled = false
	for i in frames:
		var ft := Time.get_ticks_usec()
		await get_tree().process_frame
		var ms := (Time.get_ticks_usec() - ft) / 1000.0
		slowest = maxf(slowest, ms)
		if i % 30 == 29:
			print("  frame %d: %d falls, slowest %.0f ms, last %.0f ms" % [i + 1, n[0], slowest, ms])
	RenderingServer.render_loop_enabled = true
	rocks.live_rockfall = false
	rocks.rockfall.disconnect(cb)
	print("live: %d falls in %d frames, slowest frame %.0f ms" % [n[0], frames, slowest])
	breaking = false


# One fall: loose a block within p_radius of p_at, run p_ticks physics ticks, and report where its pieces went.
func _rockfall_run(p_at: Vector3, p_radius: float, p_ticks: int, p_log: bool) -> Dictionary:
	var fired := []
	var cb := func(pos: Vector3, mass: float) -> void: fired.append([pos, mass])
	rocks.rockfall.connect(cb)
	var t0 := Time.get_ticks_usec()
	var node: Node3D = rocks.trigger_rockfall(p_at, p_radius)
	var ms := (Time.get_ticks_usec() - t0) / 1000.0
	rocks.rockfall.disconnect(cb)
	if node == null:
		print("no cliff block within %.0f m of %s" % [p_radius, p_at])
		return {}
	var start := node.global_position
	var top := start.y
	if p_log:
		print("loosed a %.0f kg block at %s in %.1f ms" % [fired[0][1] if fired.size() else 0.0, start, ms])
	var max_speed := 0.0
	var last := start
	var broke_at := -1
	var dust0: float = rocks.get_rockfall_dust_total()
	var hits0: int = rocks.get_rockfall_dust_impacts()
	# --fall-dust keeps drawing while it falls: the Flow dust steps with the frames, drawn small (a full-size frame
	# of the world takes half a minute in the cloud) and full size again for the shot.
	RenderingServer.render_loop_enabled = args.has("fall-dust")
	var full_size := get_window().size
	if args.has("fall-dust"):
		get_window().size = Vector2i(maxi(full_size.x / 4, 64), maxi(full_size.y / 4, 36))
	for i in p_ticks:
		await get_tree().physics_frame
		var c := _pieces_centre(node)
		max_speed = maxf(max_speed, c.distance_to(last) * Engine.physics_ticks_per_second)
		last = c
		if broke_at < 0 and node.is_fractured():
			broke_at = i
			if p_log:
				print("  broke at tick %d, %.1f m below the start, into %d pieces" % [i, top - c.y, node.get_piece_count()])
		if p_log and i % 60 == 59:
			print("  tick %d: %d pieces, centre %s, %.1f m below the start, ground there %.1f" % [i + 1, node.get_piece_count(), c, top - c.y, gen.get_height(c)])
	RenderingServer.render_loop_enabled = true
	get_window().size = full_size
	var below := 0
	for i in node.get_piece_count():
		var o: Vector3 = node.get_piece_transform(i).origin
		if o.y < gen.get_height(o) - 1.0:
			below += 1
	var r := {"node": node, "start": start, "end": last, "drop": top - last.y, "travel": Vector2(last.x - start.x, last.z - start.z).length(), "pieces": node.get_piece_count(), "fractured": node.is_fractured(), "fired": fired.size(), "ms": ms, "max_speed": max_speed, "below": below}
	r["dust"] = rocks.get_rockfall_dust_total() - dust0
	r["hits"] = rocks.get_rockfall_dust_impacts() - hits0
	if p_log:
		print("fell %.1f m and %.1f m out, top speed %.1f m/s, %d pieces (%s), %d under the ground" % [r["drop"], r["travel"], max_speed, r["pieces"], "broke" if r["fractured"] else "whole", below])
		var dn: Node = rocks.get_rockfall_dust_node()
		print("dust: %.1f kg of fines from %d impacts; Flow node %s, %.1f kg taken, %d bursts running" % [r["dust"], r["hits"], "yes" if dn else "none (engine without Flow)", dn.get_total_dust() if dn else 0.0, dn.get_active_sources() if dn else 0])
	return r


func _pieces_centre(node: Node) -> Vector3:
	var c := Vector3.ZERO
	var n: int = node.get_piece_count()
	for i in n:
		c += node.get_piece_transform(i).origin
	return c / maxf(n, 1)


# --plants: the stand's crevice plants between the field's stones round the spot (docs/ideas/vegetation.md,
# "Plants between stones"), on a 64 m data map over the landscape.
func _crevice_plants(spot: Vector3, view: String, land: Landscape3D) -> void:
	var t0 := Time.get_ticks_msec()
	var dm := TerrainDataMap3D.new()
	dm.name = "PlantMap"
	dm.size = 64.0
	dm.resolution = 256
	dm.bake_on_ready = false
	add_child(dm)
	dm.global_position = Vector3(spot.x, 0.0, spot.z)
	dm.height_source = dm.get_path_to(land)
	dm.geology_source = dm.get_path_to(gen)
	dm.bake()
	var stand: int = {"river": PlantGenerator.STONES_RIVER, "shore": PlantGenerator.STONES_SHORE}.get(view, PlantGenerator.STONES_SCREE)
	var species: Array[PlantSpecies] = []
	for sp in PlantGenerator.make_stones(stand, 3, true, {"forcing": 900.0, "day_of_year": 175}):
		species.append(sp as PlantSpecies)
	var biome := Biome.new()
	biome.species = species
	var veg := VegetationField3D.new()
	veg.name = "CrevicePlants"
	veg.populate_on_ready = false
	veg.biome = biome
	veg.obstacles = rocks.get_obstacles(Rect2(spot.x - 32.0, spot.z - 32.0, 64.0, 64.0), 0.0)
	# Moss cushions on the stones' tops: each stone's flat cap from its shape (RockField3D.get_stone_caps).
	var tops := PackedVector4Array()
	var normals := PackedVector3Array()
	for layer in LAYERS.size():
		if LAYERS[layer] == "cliff":
			continue
		for cap in rocks.get_stone_caps(layer, Rect2(spot.x - 32.0, spot.z - 32.0, 64.0, 64.0), float(args.get("cap-tol", "0.03"))):
			var n: Vector3 = cap["cap_normal"]
			var r := float(cap["cap_radius"])
			if r >= 0.05 and n.y >= 0.6:
				var cp: Vector3 = cap["cap_point"]
				tops.append(Vector4(cp.x, cp.y, cp.z, r * 0.85))
				normals.append(n)
	veg.stone_tops = tops
	veg.stone_top_normals = normals
	add_child(veg)
	veg.data_map = veg.get_path_to(dm)
	veg.populate()
	var counts := []
	for i in species.size():
		counts.append(veg.get_placements(i).size())
	veg.visible = args["plants"] != "off"
	print("crevice plants (%s): %d obstacles, %d stone caps, per species %s, %d ms" % [view, veg.obstacles.size(), veg.stone_tops.size(), str(counts), Time.get_ticks_msec() - t0])
	plants_ready = true


func _sunlit(p: Vector3) -> bool:
	var to_sun := Basis.from_euler(Vector3(deg_to_rad(-32), deg_to_rad(-50), 0)) * Vector3(0, 0, 1)
	var t := 4.0
	while t < 400.0:
		var q := p + to_sun * t
		if gen.get_height(q) > q.y:
			return false
		t *= 1.35
	return true


# A camera spot round the target with a clear line of sight, on the sunny side so the stones are lit, not
# looking into a shaded wall: sixteen directions, scored by the lowest clearance over the terrain along the ray.
func _view_from(spot: Vector3, dist: float) -> Vector3:
	var sun_dir := Basis.from_euler(Vector3(deg_to_rad(-32), deg_to_rad(-50), 0)) * Vector3(0, 0, 1)
	var sun_h := Vector2(sun_dir.x, sun_dir.z).normalized()
	var best := spot + Vector3(dist * 0.7, dist * 0.45, dist * 0.7)
	var best_score := -INF
	for i in 16:
		var a := TAU * i / 16.0
		var d := Vector2(cos(a), sin(a))
		var p := spot + Vector3(d.x * dist, 0, d.y * dist)
		p.y = maxf(spot.y + dist * 0.45, gen.get_height(p) + 3.0)
		var clear := INF
		for k in range(1, 12):
			var q := spot.lerp(p, k / 12.0) + Vector3(0, 0.3 * (1.0 - k / 12.0), 0)
			clear = minf(clear, q.y - gen.get_height(q))
		var walls := 0.0
		for j in 8:
			var w := p + Vector3(cos(TAU * j / 8.0), 0, sin(TAU * j / 8.0)) * dist * 0.5
			walls = maxf(walls, gen.get_height(w) - p.y)
		# From the open, lower side (the water for a shore, the valley for a slope), not from up against the hill.
		var score := minf(clear, 3.0) - maxf(walls, 0.0) * 0.5 - (gen.get_height(p) - spot.y) * 0.3 + 2.0 * d.dot(sun_h) - maxf(p.y - spot.y - dist, 0.0) * 0.2
		if score > best_score:
			best_score = score
			best = p
	return best


# The middle of the 40 m square holding most stones of a layer, near the middle of the map.
func _densest(layer: int) -> Vector3:
	if layer < 0:
		return Vector3.ZERO
	var t0 := Time.get_ticks_msec()
	if layer == RockField3D.LAYER_SCREE:
		# Scree covers too much ground to place it all: the fullest talus near the middle, on gentle enough ground.
		var bt := -1.0
		var bp := Vector3.ZERO
		for zz in range(-800, 800, 8):
			for xx in range(-800, 800, 8):
				var p := Vector3(xx, 0, zz)
				var t := gen.get_talus(p) - absf(gen.get_height(p + Vector3(4, 0, 0)) - gen.get_height(p - Vector3(4, 0, 0))) * 0.02
				if args.has("talus-slope"):
					# --talus-slope: a mid-slope patch of a real talus apron instead, 25 to 38 degrees and well above
					# the beaches (the fullest talus can be a flat at the cliff foot by the sea).
					var gx := (gen.get_height(p + Vector3(4, 0, 0)) - gen.get_height(p - Vector3(4, 0, 0))) / 8.0
					var gz := (gen.get_height(p + Vector3(0, 0, 4)) - gen.get_height(p - Vector3(0, 0, 4))) / 8.0
					var sl := rad_to_deg(atan(sqrt(gx * gx + gz * gz)))
					t = gen.get_talus(p) if sl > 25.0 and sl < 38.0 and gen.get_height(p) > gen.sea_level + 30.0 else -1.0
				if t > bt:
					bt = t
					bp = p
		print("scree: fullest talus %.2f at %s (%d ms)" % [bt, bp, Time.get_ticks_msec() - t0])
		return bp
	var stones: Array = rocks.get_stones(layer, Rect2(-800, -800, 1600, 1600))
	if layer == RockField3D.LAYER_COAST:
		# The coast: frame the stack or arch itself (the biggest form), its foot at the water.
		var best := {}
		for s in stones:
			var fams := [RockGenerator.FAMILY_SEA_STACK, RockGenerator.FAMILY_SEA_ARCH]
			if args.has("form"):
				fams = [RockGenerator.FAMILY_SEA_ARCH if args["form"] == "arch" else RockGenerator.FAMILY_SEA_STACK]
			if int(s["family"]) in fams and (best.is_empty() or float(s["scale"]) > float(best["scale"])):
				best = s
		if not best.is_empty():
			print("coast form: family %d at %s" % [best["family"], best["position"]])
			return best["position"]
	if layer == RockField3D.LAYER_LANDFORM and FAMILIES.has(args.get("form", "")):
		# --form=pavement|fin|hoodoo: the biggest of that family in the sun.
		var best := {}
		for s in rocks.get_stones(layer, Rect2(-1000, -1000, 2000, 2000)):
			# Sunlit first, then the bigger.
			if int(s["family"]) != FAMILIES.find(args["form"]):
				continue
			var score := float(s["scale"]) + (100.0 if _sunlit(s["position"] + Vector3(0, 1, 0)) else 0.0)
			if best.is_empty() or score > float(best["score"]):
				best = s.duplicate()
				best["score"] = score
		if not best.is_empty():
			print("%s at %s" % [args["form"], best["position"]])
			form_stone = best
			return best["position"]
	var cells := {}
	var sums := {}
	for s in stones:
		var p: Vector3 = s["position"]
		# Rockfall: the runout below the cliff, not the stones still on the steep foot (the view would face a wall).
		if layer == RockField3D.LAYER_ROCKFALL and absf(gen.get_height(p + Vector3(3, 0, 0)) - gen.get_height(p - Vector3(3, 0, 0))) + absf(gen.get_height(p + Vector3(0, 0, 3)) - gen.get_height(p - Vector3(0, 0, 3))) > 3.0:
			continue
		# In the sun: march toward it over the terrain (the bench sky gives little fill, so shade reads black).
		if layer != RockField3D.LAYER_CLIFF and not _sunlit(Vector3(p.x, gen.get_height(p) + 0.5, p.z)):
			continue
		# Rivers: an open valley floor, not a slot gorge in its own shade.
		if layer == RockField3D.LAYER_RIVER:
			var slot := 0.0
			for j in 8:
				slot = maxf(slot, gen.get_height(p + Vector3(cos(TAU * j / 8.0), 0, sin(TAU * j / 8.0)) * 25.0) - p.y)
			if slot > 8.0:
				continue
		var key := Vector2i(floori(p.x / 40.0), floori(p.z / 40.0))
		cells[key] = int(cells.get(key, 0)) + 1
		sums[key] = (sums.get(key, Vector3.ZERO) as Vector3) + p
	var best := Vector2i.ZERO
	var best_n := -1
	for k in cells:
		if cells[k] > best_n:
			best_n = cells[k]
			best = k
	print("%s: %d stones in the middle 1.6 km, densest 40 m square %d (%d ms)" % [LAYERS[layer], stones.size(), best_n, Time.get_ticks_msec() - t0])
	if best_n <= 0:
		return Vector3.ZERO
	# The middle of the stones in it, not of the square: a shore or river band can run along one edge.
	var mid: Vector3 = (sums[best] as Vector3) / float(best_n)
	return Vector3(mid.x, 0, mid.z)


func _stand_ins() -> void:
	var bl: PackedVector4Array = gen.get_boulders(Rect2(-1024, -1024, 2048, 2048), 1.0)
	var sm := SphereMesh.new()
	sm.radial_segments = 7
	sm.rings = 4
	var rm := StandardMaterial3D.new()
	rm.albedo_color = Color(0.42, 0.40, 0.37)
	rm.roughness = 0.9
	sm.material = rm
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = sm
	mm.instance_count = bl.size()
	for i in bl.size():
		var b := bl[i]
		mm.set_instance_transform(i, Transform3D(Basis().scaled(Vector3(b.w * 2.0, b.w * 1.2, b.w * 2.0)), Vector3(b.x, b.y - b.w * 0.2, b.z)))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)


func _volume(m: ArrayMesh) -> float:
	var arr := m.surface_get_arrays(0)
	var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
	var vol := 0.0
	for i in range(0, idx.size(), 3):
		vol += v[idx[i]].dot(v[idx[i + 1]].cross(v[idx[i + 2]]))
	return absf(vol) / 6.0


func _test() -> void:
	# 1. Every rock type and family.
	var worst := 0
	for ti in TYPES.size():
		for fi in FAMILIES.size():
			var g := RockGenerator.new()
			g.rock_type = ti
			g.family = fi
			g.seed = 11
			var m := g.generate()
			if m == null:
				check(false, "%s %s builds" % [TYPES[ti], FAMILIES[fi]])
				continue
			var ax: Vector3 = m.get_meta("rock_axes")
			var tris := m.surface_get_array_index_len(0) / 3
			worst = maxi(worst, int(g.get_last_build_msec()))
			var ok := absf(ax.z - 1.0) < 0.02 and ax.y <= ax.z + 0.02 and ax.x >= ax.z - 0.05 and tris > 500
			check(ok, "%s %s: axes %.2f %.2f %.2f (b 1), %d triangles, %.0f ms" % [TYPES[ti], FAMILIES[fi], ax.x, ax.y, ax.z, tris, g.get_last_build_msec()])
	check(worst < 3000, "slowest rock %d ms (under 3 s)" % worst)
	# 2. Transport rounds: the stone fills less of its box as corners wear (a box fills all of it, an ellipsoid
	# 52%), and beach wear flattens.
	var fills := []
	for d in [0.0, 30000.0, 150000.0]:
		var g := RockGenerator.new()
		g.rock_type = RockGenerator.ROCK_GRANITE
		g.family = RockGenerator.FAMILY_COBBLE
		g.transport_distance = d
		g.seed = 4
		var m := g.generate()
		var ax: Vector3 = m.get_meta("rock_axes")
		fills.append(_volume(m) / (8.0 * ax.x * ax.y * ax.z))
	check(fills[0] > fills[1] and fills[2] <= fills[1] + 0.02 and fills[2] < 0.7, "river transport rounds granite: box fill %.2f, %.2f, %.2f at 0, 30, 150 km" % fills)
	var flat := []
	for f in [RockGenerator.FAMILY_COBBLE, RockGenerator.FAMILY_PEBBLE]:
		var sum := 0.0
		for s in 4:
			var g := RockGenerator.new()
			g.rock_type = RockGenerator.ROCK_LIMESTONE
			g.family = f
			g.seed = s
			var ax: Vector3 = g.generate().get_meta("rock_axes")
			sum += ax.y / ax.z
		flat.append(sum / 4.0)
	check(flat[1] < flat[0], "beach pebbles flatter than river cobbles: c/b %.2f against %.2f" % [flat[1], flat[0]])
	# 2b. Breaking: the solid split into rough pieces (fracture_pieces) that fill it, bonded to their neighbours.
	for ti in [RockGenerator.ROCK_GRANITE, RockGenerator.ROCK_SLATE]:
		var g := RockGenerator.new()
		g.rock_type = ti
		g.family = RockGenerator.FAMILY_ROCKFALL
		g.seed = 5
		g.lods = false
		g.fracture_pieces = 10
		var tf := Time.get_ticks_msec()
		var m := g.generate()
		var chunks: Array = m.get_meta("rock_chunks", [])
		var bonds: Array = m.get_meta("rock_bonds", [])
		var vol: float = m.get_meta("rock_volume", 0.0)
		var sum := 0.0
		var good := 0
		var tris := 0
		for c in chunks:
			if (c as Array).size() == Mesh.ARRAY_MAX:
				good += 1
				var am := ArrayMesh.new()
				am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, c)
				sum += _volume(am)
				tris += (c[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
		var whole := _volume(m)
		check(good >= 6 and bonds.size() >= good - 1 and absf(sum / whole - 1.0) < 0.12, "%s fracture: %d pieces, %d bonds, pieces fill %.2f of the stone (field volume %.3f, mesh %.3f), %d triangles, %d ms" % [TYPES[ti], good, bonds.size(), sum / whole, vol, whole, tris, Time.get_ticks_msec() - tf])
	# 3. The field on a generated world.
	_world()
	var wr := Rect2(-1000, -1000, 2000, 2000)
	var scree_at := _densest(RockField3D.LAYER_SCREE)
	for l in LAYERS.size():
		var t0 := Time.get_ticks_msec()
		var st: Array = rocks.get_stones(l, wr if l != RockField3D.LAYER_SCREE else Rect2(scree_at.x - 100, scree_at.z - 100, 200, 200))
		var floating := 0
		var buried := 0
		for s in st:
			var p: Vector3 = s["position"]
			var sc: float = s["scale"]
			# Tor and coast blocks stand in stacks: their upper tiers are meant to be off the ground.
			if l < RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) > sc * 3.0:
				floating += 1
			if l < RockField3D.LAYER_CLIFF and p.y - float(s["ground"]) < -sc * 1.2:
				buried += 1
			# A cliff block's centre lies inside the wall: under the terrain at its own plan position.
			if l == RockField3D.LAYER_CLIFF and p.y > gen.get_height(p) + 0.1:
				floating += 1
		var want_some := true
		check(not want_some or st.size() > 0, "%s: %d stones over the world in %d ms" % [LAYERS[l], st.size(), Time.get_ticks_msec() - t0])
		check(floating == 0 and buried == 0, "%s: none floating (%d) or buried (%d)" % [LAYERS[l], floating, buried])
	# Ground contact: the damp ring and banked fines round the stones, drawn into the map the terrain reads.
	rocks.ground_contact = true
	var ct0 := Time.get_ticks_msec()
	rocks.set_ground_contact(true)
	var ctex: ImageTexture = rocks.get_contact_texture()
	var damp_n := 0
	if ctex:
		var ci: Image = ctex.get_image()
		for y in range(0, ci.get_height(), 4):
			for x in range(0, ci.get_width(), 4):
				if ci.get_pixel(x, y).r > 0.1:
					damp_n += 1
	check(ctex != null and damp_n > 0 and rocks.get_last_contact_usec() > 0, "ground contact: map drawn in %d ms (%.1f ms on the worker), %d damp samples" % [Time.get_ticks_msec() - ct0, rocks.get_last_contact_usec() / 1000.0, damp_n])
	# Landforms: hoodoo rows on the steep soft beds, pavement on flat bare limestone; each kind somewhere.
	var kinds := {}
	for s in rocks.get_stones(RockField3D.LAYER_LANDFORM, wr):
		kinds[s["family"]] = int(kinds.get(s["family"], 0)) + 1
	# Pavement wants flat bare limestone benches and potholes level bedrock stream beds, which this steep, coarse
	# test world nearly lacks: they are counted, not required.
	check(kinds.has(RockGenerator.FAMILY_HOODOO) and kinds.has(RockGenerator.FAMILY_FIN), "landforms: %d hoodoos, %d fins, %d pavement patches, %d pothole slabs, %d goblins, %d cave mouths" % [kinds.get(RockGenerator.FAMILY_HOODOO, 0), kinds.get(RockGenerator.FAMILY_FIN, 0), kinds.get(RockGenerator.FAMILY_PAVEMENT, 0), kinds.get(RockGenerator.FAMILY_POTHOLE, 0), kinds.get(RockGenerator.FAMILY_GOBLIN, 0), kinds.get(RockGenerator.FAMILY_CAVE_MOUTH, 0)])
	# Cave holes: each cave mouth opens the ground inside its passage (TerrainGenerator3D.add_hole), Landscape3D leaves
	# those quads out, and a Terrain3D (here a stand-in with the addon's calls) gets them in its control map.
	var caves := int(kinds.get(RockGenerator.FAMILY_CAVE_MOUTH, 0))
	var land_h: Landscape3D = get_node("Landscape")
	check(caves == 0 or (gen.get_hole_count() >= caves and land_h.get_hole_quad_count() > 0), "cave holes: %d cave mouths, %d holes, %d landscape quads left out in %d chunks" % [caves, gen.get_hole_count(), land_h.get_hole_quad_count(), land_h.get_hole_chunk_count()])
	if gen.get_hole_count() > 0:
		var t3 := Node3D.new()
		t3.set_script(load("res://terrain3d_stand_in.gd"))
		t3.name = "Terrain3DStandIn"
		add_child(t3)
		rocks.terrain3d = rocks.get_path_to(t3)
		var written := rocks.write_terrain3d_holes()
		check(t3.holes > 0 and t3.map_updates.has(1) and t3.collision_updates > 0, "Terrain3D holes: %d holes, %d control-map vertices, map updates %s, %d collision updates" % [written, t3.holes, t3.map_updates.slice(0, 3), t3.collision_updates])
		rocks.terrain3d = NodePath()
		t3.queue_free()
	# Cave networks (CaveNetwork3D): an entrance found at a limestone cliff foot grows a network that stays in the rock
	# and joins its key points; the same seed gives the same cave.
	var net := CaveNetwork3D.new()
	net.rimstone_water = false # the pools' live water costs frames and checks nothing here
	add_child(net)
	net.generator = net.get_path_to(gen)
	net.rock_field = net.get_path_to(rocks)
	var ents: Array = net.find_entrances(gen.get_world_rect(), 2)
	if ents.is_empty():
		check(true, "caves: no limestone cliff foot in this world (counted, not required)")
	else:
		var e: Dictionary = ents[0]
		var id := net.add_cave(e["position"], e["out"])
		var segs: Array = net.get_segments(id) if id >= 0 else []
		var length := 0.0
		var shallow := 0
		for sg in segs:
			length += (sg["a"] as Vector3).distance_to(sg["b"])
			var m: Vector3 = (sg["a"] + sg["b"]) * 0.5
			if int(sg["type"]) != CaveNetwork3D.PASSAGE_ENTRANCE and m.y > gen.get_height(m) - 1.0:
				shallow += 1
		check(id >= 0 and segs.size() > 10 and shallow == 0, "caves: %d entrances; a network of %d segments, %.0f m, %d chambers, %d key points in %.0f ms, %d under 1 m of rock" % [ents.size(), segs.size(), length, net.get_chambers(maxi(id, 0)).size(), net.get_key_points(maxi(id, 0)).size(), net.get_last_build_msec(), shallow])
		var id2 := net.add_cave(e["position"], e["out"])
		check(id2 >= 0 and net.get_segments(id2).size() == segs.size(), "caves: the same entrance grows the same network (%d and %d segments)" % [segs.size(), net.get_segments(maxi(id2, 0)).size()])
		# Meshes near the entrance: walls and the entrance patch's ground, the landscape opened over the patch, the
		# entrance open (air) and the rock round it solid.
		net.clear_caves()
		net.add_cave(e["position"], e["out"])
		var holes_before := gen.get_hole_count()
		var pos: Vector3 = e["position"]
		var made := net.build_meshes(AABB(pos - Vector3(20, 20, 20), Vector3(40, 40, 40)))
		var tri := {"walls": 0, "ground": 0}
		for ch in net.get_children():
			if ch is MeshInstance3D:
				var m: ArrayMesh = ch.mesh
				for si in m.get_surface_count():
					tri[m.surface_get_name(si)] += m.surface_get_array_index_len(si) / 3
		var in_air := net.get_rock_distance(pos + Vector3(0, 1.2, 0) - (e["out"] as Vector3) * 3.0)
		var in_rock := net.get_rock_distance(pos + Vector3(0, -6.0, 0) + (e["out"] as Vector3) * 8.0)
		print("  speleothems near the entrance: %d" % net.get_speleothem_count())
		# Rimstone pools: each pool's water stands in air over a floor below its level (none is required this near the
		# entrance, where no formations grow).
		var pools_t: Array = net.get_rimstone_pools()
		var wet_ok := 0
		for pl in pools_t:
			var pc: Vector3 = pl["center"]
			if net.get_rock_distance(pc + Vector3(0, 0.08, 0)) < 0.0 and net.get_rock_distance(pc - Vector3(0, float(pl["depth"]) + 0.08, 0)) > -0.05:
				wet_ok += 1
		check(wet_ok == pools_t.size(), "rimstone: %d pools near the entrance, %d standing in air over their floor" % [pools_t.size(), wet_ok])
		# Every stalactite hangs from rock: rock 0.25 m over its base, air 0.3 m under it.
		var hung := 0
		var loose := 0
		for ch in net.get_children():
			for mmi in ch.get_children():
				if mmi is MultiMeshInstance3D and mmi.name == "Stalactites":
					for ii in (mmi.multimesh as MultiMesh).instance_count:
						var o: Vector3 = (mmi.multimesh as MultiMesh).get_instance_transform(ii).origin
						if net.get_rock_distance(o + Vector3(0, 0.25, 0)) > 0.0 and net.get_rock_distance(o - Vector3(0, 0.3, 0)) < 0.0:
							hung += 1
						else:
							loose += 1
		print("  stalactites hung from the roof: %d, loose %d" % [hung, loose])
		check(made > 0 and tri["walls"] > 0 and tri["ground"] > 0 and gen.get_hole_count() > holes_before and in_air < 0.0 and in_rock > 0.0, "cave meshes: %d chunks, %d wall and %d ground triangles in %.0f ms; the landscape opened (%d holes); %.2f m into the air in the entrance, %.2f m into the rock under the foot" % [made, tri["walls"], tri["ground"], net.get_last_mesh_msec(), gen.get_hole_count() - holes_before, -in_air, in_rock])
		# Streaming: a target 30 m into the cave gets the chunks round it on workers; moved 400 m away it keeps only
		# the entrance patch (pinned while the landscape is open there).
		net.clear_meshes()
		var walker := Node3D.new()
		add_child(walker)
		walker.global_position = pos + Vector3(0, 1.5, 0) - (e["out"] as Vector3) * 30.0
		net.stream_radius = 32.0
		net.follow_target = net.get_path_to(walker)
		var t_stream := Time.get_ticks_msec()
		for i in 3000:
			await get_tree().process_frame
			if i > 2 and not net.is_streaming():
				break
		var near_chunks := net.get_mesh_chunk_count()
		var stream_ms := Time.get_ticks_msec() - t_stream
		walker.global_position += Vector3(400, 0, 0)
		for i in 3000:
			await get_tree().process_frame
			if i > 2 and not net.is_streaming():
				break
		var far_chunks := net.get_mesh_chunk_count()
		check(near_chunks > far_chunks and far_chunks > 0, "cave streaming: %d chunks round a target 30 m in (%d ms with frames running), %d left (the entrance patch) with it 400 m away" % [near_chunks, stream_ms, far_chunks])
		net.follow_target = NodePath()
		walker.queue_free()
	# Sea caves: mouths at cliffs over the sea, the floor at the mouth under the sea, rock round the passage.
	var sea_ents: Array = net.find_sea_entrances(gen.get_world_rect(), 4)
	if sea_ents.is_empty():
		check(true, "sea caves: no sea cliff in this world (counted, not required)")
	else:
		net.clear_caves()
		var sid := -1
		for se in sea_ents:
			sid = net.add_sea_cave(se["position"], se["out"])
			if sid >= 0:
				break
		var ssegs: Array = net.get_segments(sid) if sid >= 0 else []
		var sea_y := gen.get_sea_level()
		var mouth_floor := INF
		var thin := 0
		for sg in ssegs:
			if int(sg["type"]) == CaveNetwork3D.PASSAGE_SEA:
				var m: Vector3 = (sg["a"] + sg["b"]) * 0.5
				mouth_floor = minf(mouth_floor, m.y - 1.25 * float(sg["radius_a"]))
				if gen.get_height(m) < m.y + 0.6 * float(sg["radius_a"]) + 2.0:
					thin += 1
		check(sid >= 0 and net.is_sea_cave(sid) and ssegs.size() >= 3 and mouth_floor < maxf(sea_y, gen.get_height((sea_ents[0]["position"] as Vector3) + (sea_ents[0]["out"] as Vector3) * 4.0)) + 0.1 and thin <= 3, "sea caves: %d mouths; %d segments, floor %.1f m under the sea at the mouth (at most level with a beach), %d blowholes, %d segments under thin rock" % [sea_ents.size(), ssegs.size(), sea_y - mouth_floor, net.get_blowholes(maxi(sid, 0)).size(), thin])
		# The network breathes mist out of its sea-cave mouths (PhysXCaveMist3D, made on its first frames).
		if ClassDB.class_exists("PhysXCaveMist3D") and sid >= 0:
			await get_tree().process_frame
			await get_tree().process_frame
			var mist: Node = net.get_sea_mist_node()
			check(mist != null and mist.get("cave_network") != NodePath(), "sea mist: %s" % ("a mist node on the network" if mist else "no mist node"))
			net.sea_mist = false # it checks nothing more here
	net.queue_free()
	# Scree packs without overlaps: stones in one cell, as spheres of their mean radius.
	var scree: Array = rocks.get_stones(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 16, scree_at.z - 16, 32, 32))
	var overlaps := 0
	for i in scree.size():
		for j in range(i + 1, mini(scree.size(), i + 200)):
			var a: Dictionary = scree[i]
			var b: Dictionary = scree[j]
			var rr: float = (float(a["scale"]) + float(b["scale"])) * 0.55
			if (a["position"] as Vector3).distance_to(b["position"]) < rr * 0.6:
				overlaps += 1
	check(overlaps <= scree.size() / 50, "scree packs: %d deep overlaps in %d stones" % [overlaps, scree.size()])
	# Stone caps (where moss and snow settle) from the shapes themselves: on the stone, at its top, facing up.
	var tc := Time.get_ticks_msec()
	var caps: Array = rocks.get_stone_caps(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 16, scree_at.z - 16, 32, 32))
	var bad_caps := 0
	var usable := 0
	var radii := 0.0
	for c in caps:
		var cp: Vector3 = c["cap_point"]
		var cn: Vector3 = c["cap_normal"]
		var sc: float = c["scale"]
		if cp.y > float(c["top"]) + 0.01 or cp.y < float(c["ground"]) - 0.2 * sc or cn.y < 0.65 or float(c["cap_radius"]) > 2.5 * sc:
			bad_caps += 1
		radii += float(c["cap_radius"]) / sc
		if float(c["cap_radius"]) >= 0.06:
			usable += 1
	check(caps.size() > 0 and bad_caps <= caps.size() / 100 and usable > caps.size() / 2, "stone caps: %d stones, %d off their tops or tilted past 50 degrees, %d with a flat radius of 6 cm or more, mean flat radius %.2f of the half size (%d ms)" % [caps.size(), bad_caps, usable, radii / maxf(caps.size(), 1), Time.get_ticks_msec() - tc])
	var t1 := Time.get_ticks_msec()
	var obs: PackedVector4Array = rocks.get_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256), 0.25)
	check(obs.size() > 0, "obstacles for the plant fields: %d stones of 25 cm and more in 256 m round the scree (%d ms)" % [obs.size(), Time.get_ticks_msec() - t1])
	var t2 := Time.get_ticks_msec()
	var obs2: PackedVector4Array = rocks.get_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256), 0.25)
	check(obs2 == obs and Time.get_ticks_msec() - t2 < 100, "obstacles again from the cache: same %d stones in %d ms" % [obs2.size(), Time.get_ticks_msec() - t2])
	var rivers: Array = rocks.get_stones(RockField3D.LAYER_RIVER, wr)
	var rounder := 0
	for s in rivers:
		rounder += 1 if int(s["bin"]) > 0 else 0
	print("river stones past the first roundness step: %d of %d" % [rounder, rivers.size()])
	# 4. Cost: build 320 m round the scree with collision, then the memory the shared shapes and the instances hold.
	rocks.collision_enabled = true
	(get_node("Focus") as Node3D).global_position = scree_at
	var tb := Time.get_ticks_msec()
	var cells := rocks.build_area(Rect2(scree_at.x - 160, scree_at.z - 160, 320, 320))
	var build_ms := Time.get_ticks_msec() - tb
	var meshes := {}
	var close_meshes := {}
	var instances := 0
	var bodies := 0
	var shapes := 0
	for n in rocks.get_children(true):
		if n is MultiMeshInstance3D and n.has_meta("rock_close_up"):
			var cm: Mesh = (n as MultiMeshInstance3D).multimesh.mesh
			close_meshes[cm.get_rid()] = cm
		elif n is MultiMeshInstance3D:
			var mm: MultiMesh = (n as MultiMeshInstance3D).multimesh
			instances += mm.instance_count
			meshes[mm.mesh.get_rid()] = mm.mesh
		elif n is StaticBody3D:
			bodies += 1
			shapes += n.get_child_count()
	var mesh_bytes := _mesh_bytes(meshes)
	var close_bytes := _mesh_bytes(close_meshes)
	var inst_bytes := instances * (48 + 16)
	print("cost: %d cells, %d stones, %d shapes, %.1f MB shapes (LOD0 %d triangles), %.1f MB instances, %d collision bodies / %d hulls, built in %d ms" % [cells, instances, meshes.size(), mesh_bytes / 1048576.0, rocks.get_library_triangle_count(), inst_bytes / 1048576.0, bodies, shapes, build_ms])
	check(shapes > 0, "collision hulls near the follow target: %d" % shapes)
	# Close-up shapes for the stones within close_up_radius of the follow target.
	check(rocks.get_close_up_shape_count() > 0, "close-up shapes near the follow target: %d shapes, %d triangles, %.1f MB" % [rocks.get_close_up_shape_count(), rocks.get_close_up_triangle_count(), close_bytes / 1048576.0])
	# Moss shells on the close-up stones where the shader paints moss (real geometry, rock_moss.cpp).
	var moss_bytes := 0
	for n in rocks.get_children(true):
		if n is MeshInstance3D and n.has_meta("rock_moss"):
			moss_bytes += _mesh_bytes({0: (n as MeshInstance3D).mesh})
	check(rocks.get_moss_shell_count() > 0 and rocks.get_moss_pending() == 0, "moss shells: %d on the close-up stones, %d triangles, %.1f MB" % [rocks.get_moss_shell_count(), rocks.get_moss_triangle_count(), moss_bytes / 1048576.0])
	# Close-up shapes by family and size octave (what drives their count and memory).
	var hist := {}
	for l in [RockField3D.LAYER_ROCKFALL, RockField3D.LAYER_SCREE, RockField3D.LAYER_GLACIAL]:
		for st in rocks.get_stones(l, Rect2(scree_at.x - 12, scree_at.z - 12, 24, 24)):
			if (st["position"] as Vector3).distance_to(scree_at) < 8.0 + float(st["scale"]) * 1.5:
				var fam: int = st["family"]
				var b := roundi(log(float(st["scale"]) / [0.65, 0.16, 0.85, 0.11, 0.035, 0.65, 1.0][fam]) / log(2.0))
				var k := "%s %+d" % [FAMILIES[fam], b]
				hist[k] = hist.get(k, 0) + 1
	print("near stones by family and octave: ", hist)
	# Breaking a stone near the target: it is swapped for its pieces (PhysX Blast) and stays broken.
	check(rocks.is_breaking_supported(), "breaking supported (PhysX Blast in this build)")
	if rocks.is_breaking_supported():
		var near: Array = rocks.get_stones(RockField3D.LAYER_SCREE, Rect2(scree_at.x - 6, scree_at.z - 6, 12, 12))
		var big: Dictionary = {}
		for st in near:
			if big.is_empty() or float(st["scale"]) > float(big["scale"]):
				big = st
		check(not big.is_empty(), "a stone to break near the target: %d stones" % near.size())
		if not big.is_empty():
			var bt := Time.get_ticks_msec()
			var node: Node = rocks.break_stone(big["position"], 100.0, 3.0 * float(big["scale"]))
			var pieces: int = node.get_piece_count() if node else 0
			check(node != null and pieces > 3 and rocks.get_broken_count() == 1, "break_stone: a %.2f m stone into %d pieces in %d ms (shape fractured on demand)" % [2.0 * float(big["scale"]), pieces, Time.get_ticks_msec() - bt])
			if node:
				var ps := _piece_stats(node, Transform3D(big["basis"], big["position"]))
				var grown: AABB = (ps["stone_box"] as AABB).grow(0.02 * (ps["stone_box"] as AABB).size.length())
				check(absf(ps["volume"] / ps["stone_volume"] - 1.0) < 0.05 and grown.encloses(ps["box"]), "pieces are the stone: volume %.3f of %.3f m3, all inside its box (%s in %s)" % [ps["volume"], ps["stone_volume"], (ps["box"] as AABB).size, (ps["stone_box"] as AABB).size])
			# A hit on a stone whose shape is already fractured (what break_prepare_radius does ahead of the player on a
			# worker): only the swap for the pieces, no fracture on the main thread.
			rocks.restore_broken()
			var bp: Vector3 = big["position"]
			rocks.build_area(Rect2(bp.x - 40, bp.z - 40, 80, 80)) # restore_broken drops the cells; build them again
			var pt := Time.get_ticks_usec()
			var again: Node = rocks.break_stone(big["position"], 100.0, 3.0 * float(big["scale"]))
			var ms := (Time.get_ticks_usec() - pt) / 1000.0
			check(again != null and ms < 16.0, "break_stone on a prepared shape: %.1f ms (under a 60 Hz frame)" % ms)
		# Live rockfall: a cliff block comes loose, out of the wall, falls under gravity and comes to rest on the ground.
		check(is_equal_approx(rocks.get_rockfall_trigger(), 1.0), "rockfall trigger without weather: %.2f (base rate)" % rocks.get_rockfall_trigger())
		var cliff: Array = rocks.get_stones(RockField3D.LAYER_CLIFF, Rect2(-800, -800, 1600, 1600))
		var block: Dictionary = {}
		for st in cliff:
			if block.is_empty() or float(st["scale"]) > float(block["scale"]):
				block = st
		check(not block.is_empty(), "cliff blocks to loose: %d" % cliff.size())
		if not block.is_empty():
			var bp: Vector3 = block["position"]
			var land: Landscape3D = get_node("Landscape")
			cam.position = bp + Vector3(0, 20, 30) # the landscape's collision follows the camera
			land.collision_enabled = true
			land.rebuild()
			land.load_all_heights()
			rocks.collision_enabled = true
			rocks.rebuild()
			rocks.build_area(Rect2(bp.x - 60, bp.z - 60, 120, 120))
			var before := rocks.get_broken_count()
			var r := await _rockfall_run(bp, 1.0, 120, true)
			check(not r.is_empty() and r["fired"] == 1 and rocks.get_broken_count() == before + 1, "trigger_rockfall: a block loosed (signal %d, broken %d), %.1f ms" % [r.get("fired", 0), rocks.get_broken_count() - before, r.get("ms", 0.0)])
			if not r.is_empty():
				check(r["drop"] > 0.5 and r["below"] == 0, "the fall: %.1f m down, %.1f m out, top speed %.1f m/s, %d pieces, none under the ground" % [r["drop"], r["travel"], r["max_speed"], r["pieces"]])
				check(r["hits"] > 0 and r["dust"] > 0.1, "rockfall dust: %.1f kg of fines from %d impacts" % [r["dust"], r["hits"]])
			# Live: with the rate turned right up, blocks come loose by themselves (fracture on a worker first).
			var live := [0]
			var cb := func(_p: Vector3, _m: float) -> void: live[0] += 1
			rocks.rockfall.connect(cb)
			rocks.live_rockfall_rate = 36000.0
			rocks.live_rockfall_radius = 2000.0
			rocks.live_rockfall = true
			# Software Vulkan draws a frame of this world in tens of seconds, so a frame here is many game seconds
			# and a few frames are enough (frame cost goes to the PC queue).
			for i in 6:
				await get_tree().process_frame
			rocks.live_rockfall = false
			rocks.rockfall.disconnect(cb)
			check(live[0] > 0, "live_rockfall: %d falls by themselves in 6 frames" % live[0])
	# 6. Ground detail (on by default; off above for speed): every size class on the shingle beach, placed on workers
	# while streaming as on the main thread.
	var shore_at := _densest(RockField3D.LAYER_SHORE)
	cam.position = shore_at + Vector3(0, 1.7, 3)
	cam.look_at(shore_at)
	rocks.collision_enabled = false
	rocks.ground_detail_reference_height = 1080
	rocks.ground_detail = true
	rocks.ground_detail_threads = false
	rocks.build_area(Rect2(shore_at.x - 40, shore_at.z - 40, 80, 80))
	var sync_counts := []
	for l in [RockField3D.LAYER_DETAIL_PEBBLE, RockField3D.LAYER_DETAIL_GRAVEL, RockField3D.LAYER_DETAIL_FINE]:
		sync_counts.append(rocks.get_ground_detail_count(l))
	check(sync_counts.min() > 0, "ground detail on the beach: pebbles %d, gravel %d, fine gravel %d" % sync_counts)
	rocks.rebuild()
	rocks.ground_detail_threads = true
	rocks.build_budget_msec = 1000.0 # the coarse layers' cells in a few frames
	get_viewport().disable_3d = true # frames without drawing the world (software Vulkan takes seconds a frame)
	rocks.streaming = true
	var frames := 0
	var still := 0
	var last := -1
	while frames < 400 and still < 10:
		await get_tree().process_frame
		frames += 1
		var n := rocks.get_ground_detail_count()
		still = still + 1 if n == last and n > 0 and rocks.get_ground_detail_pending() == 0 else 0
		last = n
	rocks.streaming = false
	get_viewport().disable_3d = false
	rocks.build_budget_msec = 4.0
	var thr_counts := []
	for l in [RockField3D.LAYER_DETAIL_PEBBLE, RockField3D.LAYER_DETAIL_GRAVEL, RockField3D.LAYER_DETAIL_FINE]:
		thr_counts.append(rocks.get_ground_detail_count(l))
	check(thr_counts.min() > 0, "ground detail streamed on workers in %d frames: pebbles %d, gravel %d, fine gravel %d" % ([frames] + thr_counts))
	rocks.ground_detail = false


# What the renderer holds for these meshes: vertex, attribute and index buffers and every LOD's indices.
func _mesh_bytes(meshes: Dictionary) -> int:
	var bytes := 0
	for k in meshes:
		var m: Mesh = meshes[k]
		for si in m.get_surface_count():
			var d: Dictionary = RenderingServer.mesh_get_surface(m.get_rid(), si)
			for key in ["vertex_data", "attribute_data", "skin_data", "index_data"]:
				if d.has(key):
					bytes += (d[key] as PackedByteArray).size()
			for lod in d.get("lods", []):
				bytes += (lod["index_data"] as PackedByteArray).size()
	return bytes


# --walk=m: the camera walks forward with the field streaming; the main thread's streaming time per frame.
func _walk() -> void:
	var cam := get_viewport().get_camera_3d()
	var dist := float(args["walk"])
	var step := float(args.get("walk-step", "0.1"))
	var dir := -cam.global_transform.basis.z
	dir.y = 0.0
	dir = dir.normalized()
	rocks.streaming = true
	# The first updates fill the coarse layers' full streaming radius (build_area covered less): not walking cost.
	var warm := 0
	while warm < 300 and (warm < 10 or rocks.get_last_build_msec() > 2.0 * rocks.build_budget_msec):
		await get_tree().process_frame
		warm += 1
	var ms: Array[float] = []
	var dms: Array[float] = []
	var walked := 0.0
	while walked < dist:
		cam.global_position += dir * step
		walked += step
		await get_tree().process_frame
		ms.append(rocks.get_last_build_msec())
		dms.append(rocks.get_last_detail_msec())
	# Let the last cells land.
	var settle := 0
	while rocks.get_ground_detail_pending() > 0 and settle < 600:
		await get_tree().process_frame
		ms.append(rocks.get_last_build_msec())
		dms.append(rocks.get_last_detail_msec())
		settle += 1
	rocks.streaming = false
	var stats := func(a: Array[float]) -> String:
		var s := a.duplicate()
		s.sort()
		var total := 0.0
		for v in a:
			total += v
		return "mean %.2f ms, p95 %.2f ms, worst %.2f ms" % [total / a.size(), s[int(s.size() * 0.95)], s[-1]]
	print("walk: %.1f m in %d frames after %d warm-up (%s): streaming %s; ground detail %s; %d stones" % [
		dist, ms.size(), warm, "detail on the main thread" if not rocks.ground_detail_threads else "detail on workers",
		stats.call(ms), stats.call(dms), rocks.get_stone_count()])


func _render_later() -> void:
	await get_tree().process_frame
	if args.has("walk") and rocks:
		await _walk()
	while breaking:
		await get_tree().process_frame
	while args.has("plants") and rocks and not plants_ready:
		await get_tree().process_frame
	# The sea's rock contact foam is measured over the first ticks (and once more for bodies built late): a path-traced
	# still must not start accumulating before it.
	var sea_node := get_node_or_null("Sea")
	while sea_node and sea_node.get("contact_foam") and not sea_node.is_contact_foam_ready():
		await get_tree().process_frame
	# A path-traced still waits for every surface's ray-tracing geometry (the pools' water among them), up to 600 frames.
	if not raster:
		var waited := 0
		while waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
			await RenderingServer.frame_post_draw
			waited += 1
		print("rt pending surfaces cleared after %d frames" % waited)
	var frames := (8 if raster else 12) + int(args.get("settle", "0")) # --settle=N: N more frames (lazily built near plants)
	var t0 := 0
	var tf := Time.get_ticks_msec()
	for i in frames:
		if i == frames - 4:
			t0 = Time.get_ticks_usec()
		await RenderingServer.frame_post_draw
		if OS.is_stdout_verbose() or args.has("frame-log"):
			print("frame %d at %d ms" % [i, Time.get_ticks_msec() - tf])
	print("frame %.1f ms" % ((Time.get_ticks_usec() - t0) / 4000.0))
	if rocks and rocks.moss_cushions:
		print("moss: %d shells, %d triangles, %d building" % [rocks.get_moss_shell_count(), rocks.get_moss_triangle_count(), rocks.get_moss_pending()])
	if rocks and rocks.ground_contact and rocks.get_contact_texture():
		var ci: Image = rocks.get_contact_texture().get_image()
		var lit := 0
		for y in range(0, ci.get_height(), 8):
			for x in range(0, ci.get_width(), 8):
				if ci.get_pixel(x, y).r > 0.1:
					lit += 1
		print("ground contact: map drawn in %.1f ms, %.1f%% of the samples damp" % [rocks.get_last_contact_usec() / 1000.0, 100.0 * lit / float((ci.get_height() / 8) * (ci.get_width() / 8))])
	var sea := get_node_or_null("Sea")
	if sea and sea.has_method("get_contact_foam_cells"):
		print("contact foam: ready %s, %d ring cells, %d rays" % [sea.is_contact_foam_ready(), sea.get_contact_foam_cells(), sea.get_contact_foam_rays()])
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(0)

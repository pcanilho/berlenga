extends Node3D
# Rocks bench: RockGenerator shapes and RockField3D placement (docs/ideas/rocks.md).
#
# godot --path docs/bench/rocks --rendering-driver vulkan -- [options]
#   --test                checks: every rock type and family builds (size, axes, timing), transport rounds and beach
#                         wear flattens, then the field's six layers on a generated world; prints PASS/FAIL, quits
#                         (sea caves: the sea surges in under the cliff, blowholes blow in a storm swell; boulders as
#                         wind obstacles)
#   --world --height-check  the generator's heights at fixed points and a hash of its height image against a reference
#                         (seed 5, 2048 m world): the same on every platform; prints PASS/FAIL, quits
#   --gallery             every family (columns) of every rock type (rows), each scaled to a 1 m b axis
#   --rock=type,family    one rock close up (--seed=N, --distance=m transport, --weathering=0..1; --stand: a fin,
#                         hoodoo or stack stood on end, side on)
#   --clouds[=C]          the weather sky (clouds of cover C, atmosphere) instead of the plain gradient
#   --shader-scree        the ground shader's parallax scree blocks under the real stones too (before frames)
#   --terracette-spot     with --world: a 31-40 degree soil slope along its contour; --terracettes=k Landscape3D.terracette_relief
#   --talus-slope         with --view=scree: the fullest talus on a 25-38 degree slope above the beaches
#   --exposure=x          the tonemapper's exposure (1): more for a dark cave seen from inside
#   --gen-prop=name:value[,...]  any TerrainGenerator3D property before it generates (A/B one stage)
#   --mat-prop=name:value[;...]  any TerrainMaterial3D property or tm_ shader uniform (tm_sand_color:Color(1,0,1) marks a layer)
#   --hide=Node[,Node...]  hide those children of the bench (A/B which object draws something)
#   --lod-error=px        Landscape3D.lod_error
#   --no-cliff-kit        RockField3D.cliff_kit off (the fluted walls bare of their pillars: a before frame)
#   --recess-probe=cx,cz,half,step  print the share of the 45 degree and steeper face the cliff kit recesses there
#   --pick=x0,y0,x1,y1    print the cliff layer's stones whose centre the saved frame shows in that pixel rectangle
#   --no-cliff-recess     RockField3D.cliff_recess off (the pillars stand proud of the ridges; no recess published)
#   --recess-stub=x,y,z,nx,nz,w,h,d  a WorldGround stub answering recess_at alone: a w by h box (m, up and across the
#                         face) centred at x,y,z whose face (horizontal outward normal nx,nz) steps back d m, fading
#                         over its outline's last 0.5 m (the landscape's recess tested without a stone field: --no-rocks)
#   --slope-map=cx,cz,half,step  print the terrain slope round that point (tens of degrees, '#' past 80)
#   --face-spot=north|south  a rock wall facing that way, from its foot with the sky over it (--view=cliff);
#                         --face-side=k moves the camera along the wall (k times --dist)
#   --taa                 temporal antialiasing as in play (24 frames settle before a sea-cave still); on by default
#                         for raster stills, --no-taa for the single-sample frame
#   --world-size=m        with --world: its side (2048); smaller for path traced stills in less memory
#   --world               a generated 2 km world (range, strata, glaciers, scree, rivers, a lake, the sea) with a
#                         RockField3D over it; --view=rockfall|scree|glacial|river|shore|cliff|tor|coast|landform puts the camera at the
#                         densest place of that layer (default scree)
#   --type=granite|limestone|sandstone|basalt|slate   the world's rock (default granite; soft beds limestone)
#   --stand-ins           with --world: the old grey spheres at the rockfall blocks instead of the field (before shots)
#   --no-rocks            with --world: no rocks at all
#   --burn=years[,r]      with --world: a burn scar that old, r m round (6), 2 m beyond the spot (the terrain's char overlay)
#   --cell=m              with --world: the generator's cell size (8; 4 for close shore and cliff shots)
#   --form=stack|arch     with --view=coast: frame the biggest stack or arch; --form=ledge with --view=cliff: the ledge slabs; --form=ledge-run: their longest run along a lip; --form=pavement|fin|hoodoo|pothole|goblin|cave_mouth with --view=landform
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
#                         --downstream from below a dam, looking up at its downstream face; --low a grazing view
#   --sea-cave-pick=first with --sea-caves: the finder's best site, not the one facing the sun most (frames only)
#   --sea-cave-near=x,z with --sea-caves: the site nearest that point (a frame keeps its cave when the sun turns)
#   --no-refine           with --cave-mesh and --cam: the chunks round the camera stay on the coarse grid
#   --no-sea-water        with --sea-caves --cave-mesh: the sea not drawn inside the caves (a before frame);
#                         --cave-waves=m the short waves' rms coming in at the mouth (default: from the sea),
#                         --no-cave-waves the cave's water without them (the surge's level alone)
#   --no-sea-mist         with --sea-caves --cave-mesh: no mist breathed out of the mouths; --mist-period=s its swell
#                         period (run with --fixed-fps 60 and --settle=N for N frames of mist)
#   --no-moss             no moss shells on the close-up stones (the shader's moss tint only; a before frame)
#   --rt-instanced        RockField3D.rt_instanced_cells on: cell stones path traced as instances (memory A/B)
#   --mem-report          video memory at the saved frame (with the engine flag --extra-gpu-memory-tracking: by Vulkan object type)
#   --no-close-up         the near stones keep their base shape (RockField3D.close_up_enabled off; an A/B)
#   --prim-report         with --out: the rock MultiMeshes ranked by the triangles they draw at their LODs (perf hunts)
#   --rain=mm/h           with --rock: rain on the stone for --rain-hours=h (0.5), then --dry-hours=h of drying;
#                         --no-rivulets for a before frame; --film=f the water film of a stream bed's stones
#   --salt=k              with --rock: a stand-in sea salt index (0..1) for the salt bloom; --no-salt-bloom: off
#   --moss=k              the field's moss_cover (0..1)
#   --cave-mouths         the retired cave-mouth portal blocks (and their terrain holes) in --world frames
#   --wind=m/s            with --water: the sea's wind speed (and some swell)
#   --shadow-filter=N     the directional soft-shadow filter quality (0 hard .. 5 ultra)
#   --sea-set=prop:value[,...]  with --water: set properties of the sea (e.g. wave_tile_breakup:false)
#   --water [--sea-res=N] with --world: the generator's water bodies; --sea-res sets the sea's simulated grid (cells)
#   --settle=N            N more frames before the capture (near plant cells are built lazily)
#   --out=path.png        render and quit (path traced unless --raster); --spp=N (16); --size=WxH
#   --eye=h [--beyond=m --turn=deg]  with --world: standing --dist m off the spot, h m up, looking along the
#                         ground's contour past it (turned by --turn)
#   --cam=x,y,z --look=x,y,z   a fixed camera; --sun=elevation,yaw (degrees); --fov=deg vertical (27: a 50 mm lens)
#   --plants[=off]        with --world: the crevice plants of the view's stones (PlantGenerator.make_stones: scree,
#                         river or shore; VegetationField3D.obstacles from the field); =off places them hidden (before
#                         shots, same camera); --dist=m sets the camera's distance from the spot
#   --only-layer          with --world: only the view's layer is placed
#   --no-far-thinning     with --world: RockField3D.far_thinning off (each layer whole to its radius, then a cut)
#   --save-far-cover=p    with --world: save the far cover map (32 m texels to 4 km) and count its stony texels
#   --frost[=C]           with --world: a cold, wet climate (mean annual air C, -4) so the ground sorts into stone
#                         circles and stripes (RockField3D.frost_sorting); --no-frost for the before frame;
#                         --frost-spot (flat soil: circles) or --frost-spot=stripes (4 to 9 degrees); --no-frost-ground
#                         keeps the stones but not the material's silt centres and gravel lag (a before frame)
#   --trail[=passes]      with --world: a game trail across the spot, 1.5 m before it (WorldStand.set_trails, 0.8 m wide,
#                         400 passes a year); --rooting[=years] a boar rooting 2 m behind it, that old; --no-wear ignores both
#   --repetition          with --world: per layer, how often a stone stands next to the same shape, and next to a twin
#   --streaming           with --world: RockField3D streams after the build, so far thinning runs as in play
#   --focus=camera        with --world: RockField3D follows the camera instead of the spot (far thinning as a player sees it)
#   --no-far-widen        with --world: thinned cells draw their survivors at their own size (RockField3D.far_widen off)
#   --no-far              with --world: RockField3D.far_radius 0 (no big stones or landmarks past each layer's radius)
#   --stone-density=k     with --world: the view's layer density times k (RockField3D.set_layer_density), e.g. a dense block field
#   --no-ground-detail    with --world: RockField3D.ground_detail off (on by default: stones of 1 to 8 cm round the
#                         camera as geometry; off for --test); --detail-ref=px image height the reach is set for (1080),
#                         --detail-pixels=n (3);
#                         --detail-sync places the detail cells on the main thread (ground_detail_threads off),
#                         --cell-sync the coarse layers' cells (cell_threads off)
#   --tracks              with --world: a walker and a two-wheeled cart cross the spot on bends before the still
#   --roll[=m/s]          with --world: a ball rolled across the spot before the still (stones_react; --no-react off)
#   --no-river-detail     with --world: RockField3D.ground_detail_rivers off (river beds keep only the river layer's stones)
#   --no-tiles            with --world: TerrainGenerator3D.tile_erosion_enabled off (no droplet rills and gullies);
#                         --gully-incision=x the gullies' extra cut (1; 0 for none, a before frame); --gully-spot
#                         puts the view's spot on the nearest strong gully on soil
#   --face-weathering=x   with --world: the ground material's rock-face lichen, moss and seep streaks (1; 0 before)
#   --bank-relief=x       with --world: Landscape3D.bank_relief (0.8 m; 0 for a before frame); --bank-spot puts the
#                         spot on a 42 to 52 degree soil bank; --spot=x,z puts the spot at a world point
#   --rain-climate[=P]    with --world: a mild climate of P mm a year (350) for RockField3D.rain_pedestals (stones on rain
#                         splash pedestals on bare soil); --pedestal-spot the tallest near the centre; --no-rain-pedestals off
#   --snow[=CM]           with --world: an even settled pack of CM cm (8) round the spot with its deep snow surface, for
#                         RockField3D.snow_burial (stones under the pack left out); --no-snow-burial off;
#                         --no-snow-surface leaves the deep snow surface out (the ground material's snow only)
#   --no-cliff-relief     with --world: Landscape3D.cliff_relief off (the bare heightfield faces; a before frame)
#   --no-patch-relief     with --world: the beach GroundSurface3D patch without the landscape's cliff relief (a before
#                         frame; the landscape keeps it)
#   --detail-octaves=N    with --world: the material's erosion detail octaves (4; 0 for none, a before frame);
#                         --sun-shadow=0 drops the raster sun shadow map (A/B)
#   --stand-test          with --world: a game trail (600 passes a year, 1.2 m) across the spot and a boar rooting disc
#                         beside it on WorldStand (TerrainMaterial3D's worn soil and turned soil); --rooting-age=y (0.3),
#                         --stand-at=x,z puts them there instead
#   --gravel-reach[=d]    with --view=river: a gravel-bed reach whose median is nearest d m (0.03)
#   --no-ground-cover     with --world: RockField3D.ground_detail_cover off (no grain aggregate on the ground past
#                         each class's reach; a before frame); --save-cover writes the near cover map, --cover-check
#                         compares it with the stones placed
#   --walk=m              with --world: stream while the camera walks m metres forward (--walk-step=m a frame, 0.1),
#                         then print the main thread's streaming time per frame (mean, 95th percentile, worst)


# A swell at the sea cave's mouth for the sea surge check: still sea plus 1 m amplitude, 12 s period, open water only
# (it answers outside the cliff, as the ocean does).
# The landscape's recess on its own (WorldGround.recess_at, RockField3D's contract): one box on a cliff face whose
# drawn face steps back d m, share 1 inside and fading to 0 over its outline's last 0.5 m.
class RecessBoxStub extends WorldGround:
	var centre := Vector3.ZERO
	var normal := Vector3(0, 0, 1) # horizontal, outward
	var half := Vector2(2, 3) # across, up
	var depth := 1.0

	func _recess_at(p: Vector3) -> Vector2:
		var along := Vector3.UP.cross(normal).normalized()
		var d := p - centre
		var v := absf(d.dot(along))
		var u := absf(d.y)
		var t := d.dot(normal)
		if v > half.x or u > half.y or t < -depth - 6.0 or t > 4.0:
			return Vector2.ZERO
		var share := clampf((half.x - v) / 0.5, 0.0, 1.0) * clampf((half.y - u) / 0.5, 0.0, 1.0)
		return Vector2(depth, share)

	func _recess_vector_at(p: Vector3) -> Vector4:
		var r := _recess_at(p)
		var off := -normal * r.x * r.y
		return Vector4(off.x, off.y, off.z, r.y)


class SwellStub extends WorldWater:
	var sea := 0.0
	var t := 0.0
	var gen: Node
	var amplitude := 1.0
	func _height_at(p: Vector3) -> float:
		if gen.get_height(p) > sea:
			return -INF
		return sea + amplitude * sin(TAU * t / 12.0)


const TYPES := ["granite", "limestone", "sandstone", "basalt", "slate"]
const FAMILIES := ["rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block", "corestone", "sea_stack", "sea_arch", "sea_platform", "hoodoo", "pavement", "fin", "pothole", "goblin", "cave_mouth", "ledge"]
const LAYERS := ["rockfall", "scree", "glacial", "river", "shore", "cliff", "tor", "coast", "landform"]
var WORLD := 2048.0 # --world-size=m (a smaller world for path traced stills that would not fit the memory)

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
var spot_pos := Vector3.ZERO # the view's spot (--roll)
var bank_side := Vector3.ZERO # --bank-spot: along the bank's contour, for a raking view down its length

func _exit_tree() -> void:
	# A script stub held by the engine singleton outlives the script language at shutdown (abort): drop it first.
	if args.has("recess-stub"):
		WorldContracts.set_ground(null)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.substr(2).split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	raster = args.has("raster")
	if raster and args.has("out") and not args.has("test") and not args.has("no-taa"):
		# Raster stills take TAA by default, as play does: in one unjittered frame a stone's sub-pixel crystals,
		# biotite flakes and facets point-sample into salt-and-pepper speckle (a fringe on the shaded side) that TAA
		# resolves into the stone's real texture. --no-taa: the single-sample frame.
		args["taa"] = "1"
	if args.has("recess-stub"):
		var rs: PackedStringArray = str(args["recess-stub"]).split(",")
		var stub := RecessBoxStub.new()
		stub.centre = Vector3(float(rs[0]), float(rs[1]), float(rs[2]))
		stub.normal = Vector3(float(rs[3]), 0, float(rs[4])).normalized()
		stub.half = Vector2(float(rs[5]), float(rs[6])) * 0.5
		stub.depth = float(rs[7])
		WorldContracts.set_ground(stub) # before the landscape builds: its drawn face reads the recess
		print("recess stub: %s" % [rs])
	WORLD = float(args.get("world-size", "2048"))
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
	if args.has("sun-bias"):
		# --sun-bias=normal_bias[,depth_bias[,blur]]: the sun's shadow biases and blur (shadow acne A/B).
		var bv: PackedStringArray = args["sun-bias"].split(",")
		for n in get_children():
			if n is DirectionalLight3D:
				(n as DirectionalLight3D).shadow_normal_bias = float(bv[0])
				if bv.size() > 1:
					(n as DirectionalLight3D).shadow_bias = float(bv[1])
				if bv.size() > 2:
					(n as DirectionalLight3D).shadow_blur = float(bv[2])
	if out != "":
		# Single captures have no TAA to smooth the soft-low shadow kernel's per-pixel rotation (a dot
		# grid in penumbras): the high filter instead.
		# --shadow-filter=N overrides it (0 hard .. 5 soft ultra).
		var q := int(args.get("shadow-filter", str(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)))
		RenderingServer.directional_soft_shadow_filter_set_quality(q)
		RenderingServer.positional_soft_shadow_filter_set_quality(q)
		# --taa: temporal antialiasing, as in play (the water's jittered screen reflections resolve over frames);
		# the still waits 24 frames for it to settle.
		if args.has("taa"):
			get_viewport().use_taa = true
		_render_later()


# Real ground scans (Poly Haven CC0, art repository terrain/layers, fetched by docs/bench/landscape/fetch_art.sh into
# docs/bench/landscape/art/terrain_layers). Each layer keeps the geology's colour (detail_only: the scan adds detail
# only, to colour and roughness) and tiles at the scan's real size. Loaded from the files directly (the folder is .gdignore'd, no import step).
# The scan set's folder (the stones' and caves' sections in it too: RockField3D and CaveNetwork3D load_scan_set).
func _rock_scan_dir() -> String:
	return ProjectSettings.globalize_path("res://").path_join("../landscape/art/terrain_layers")

func _apply_ground_scans(tm: TerrainMaterial3D, dir: String, only := "1") -> int:
	# The whole scan set (layers and overlay scans) from the art folder's scans.cfg (TerrainMaterial3D.load_scan_set).
	# --ground-scans=rock,scree: scans on those slots only; --overlay-scans=0: the five layers' scans only (A/B);
	# --scan-maps=an: only the albedo and normal maps.
	var slots := PackedStringArray()
	if only != "1":
		slots = only.split(",")
	elif args.get("overlay-scans", "1") == "0":
		slots = PackedStringArray(["rock", "scree", "soil", "alluvium", "sand"])
	return tm.load_scan_set(dir, slots, args.get("scan-maps", ""))

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
		if not args.has("look-out"):
			# (Looking out through a mouth the sky is in the frame: the path tracer draws its misses from it.)
			env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	# --exposure=x: the tonemapper's exposure (1), for a dark scene such as a cave seen from inside.
	if args.has("exposure"):
		env.tonemap_exposure = float(args["exposure"])
	env.pathtracing_sky_shader_enabled = args.has("clouds")
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-32, -50, 0)
	sun.light_energy = 2.2
	sun.shadow_enabled = args.get("sun-shadow", "1") != "0" # --sun-shadow=0: no raster sun shadow map (A/B)
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


# --salt=0..1: a stand-in for WeatherMap3D sea_spray's salt index over the bench (the weather_spray_map global, G),
# so the rock material's salt bloom shows without a sea; --no-salt-bloom turns it off (a before frame).
func _salt_stand_in() -> void:
	if not args.has("salt"):
		return
	var img := Image.create(4, 4, false, Image.FORMAT_RGBAF)
	img.fill(Color(0.0, float(args["salt"]), 0.0, 1.0))
	RenderingServer.global_shader_parameter_set("weather_spray_map", ImageTexture.create_from_image(img))
	RenderingServer.global_shader_parameter_set("weather_spray_rect", Vector4(-100.0, -100.0, 200.0, 200.0))


func _one_rock() -> void:
	_plain_ground(20.0)
	_rain_weather()
	_salt_stand_in()
	var tf: PackedStringArray = args["rock"].split(",")
	var m := _rock(TYPES.find(tf[0]), FAMILIES.find(tf[1]) if tf.size() > 1 else 0, int(args.get("seed", "3")))
	print("rock: %d triangles, axes %s" % [m.surface_get_array_index_len(0) / 3, m.get_meta("rock_axes")])
	if args.has("no-rivulets") and m.surface_get_material(0) is ShaderMaterial:
		(m.surface_get_material(0) as ShaderMaterial).set_shader_parameter("rain_rivulets", false)
	if args.has("no-salt-bloom") and m.surface_get_material(0) is ShaderMaterial:
		(m.surface_get_material(0) as ShaderMaterial).set_shader_parameter("salt_bloom", false)
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
	gen.generate_on_ready = false # this bench generates itself, with its own settings
	gen.name = "Generator"
	# Heavy generator options are on by default; this bench keeps its own fast choices.
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.tile_erosion_enabled = not args.has("no-tiles") # droplet erosion's rills and gullies (--no-tiles: a before frame)
	gen.gully_incision = float(args.get("gully-incision", "1")) # --gully-incision=0: the droplets' channels without the gullies' extra cut
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
	gen.uplift = float(args.get("uplift", "3.0")) # --uplift=k: lower for a gentler world (frost-sorted flats)
	gen.base_uplift = 0.1
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	for y in 64:
		var z := -WORLD * 0.5 + (y + 0.5) / 64.0 * WORLD
		for x in 64:
			up.set_pixel(x, y, Color(0.12 + 0.88 * (1.0 - smoothstep(-500.0 * WORLD / 2048.0, 500.0 * WORLD / 2048.0, z)), 0, 0))
	gen.uplift_map = ImageTexture.create_from_image(up)
	gen.outlet_edges = 0
	gen.ocean_edges = TerrainGenerator3D.OUTLET_POSITIVE_Z
	gen.coast_distance = 300.0
	gen.lake_count = 1
	gen.strata_enabled = true
	gen.cliff_ledge_spacing = float(args.get("ledge-spacing", "16"))
	gen.scree_age = float(args.get("scree-age", "6000"))
	gen.moraine_age = float(args.get("moraine-age", "8000"))
	gen.detail_octaves = 5
	gen.glacial_enabled = true
	gen.glacial_ela = float(args.get("ela", "450"))
	# --gen-prop=name:value[,name:value...]: any of the generator's properties (A/B one stage).
	for kv in str(args.get("gen-prop", "")).split(",", false):
		var p: PackedStringArray = kv.split(":")
		gen.set(p[0], str_to_var(p[1]))
	add_child(gen)
	gen.generate()
	print("world generated in %d ms" % (Time.get_ticks_msec() - t0))
	if args.has("geo-at"): # --geo-at=x,z: the generator's geology (beach sand, rock, talus, drainage) on a 2 m grid round x,z, then quit
		var c: PackedStringArray = str(args["geo-at"]).split(",")
		for dz in range(-10, 12, 4):
			var row := ""
			for dx in range(-10, 12, 4):
				var p := Vector3(float(c[0]) + dx, 0, float(c[1]) + dz)
				var g: Color = gen.get_geology_at(p)
				row += "(%5.2f %4.2f %4.2f h%5.1f) " % [g.r, g.g, g.b, gen.get_height(p)]
			print(row)
		get_tree().quit()
		return
	if args.has("height-check"):
		_height_check()
		return
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
	tm.cliff_self_shadow = args.get("ledge-shadow", "1") != "0" # --ledge-shadow=0: cliff relief ledges without their per-pixel shadows
	# --mat-prop=name:value[;...]: any TerrainMaterial3D property (A/B one feature, or a layer's colour as a marker).
	for kv in str(args.get("mat-prop", "")).split(";", false):
		var mp: PackedStringArray = kv.split(":")
		if not mp[0].begins_with("tm_"):
			tm.set(mp[0], str_to_var(mp[1]))
	if args.has("detail-octaves"):
		tm.detail_octaves = int(args["detail-octaves"]) # --detail-octaves=N: erosion detail octaves in the normal (0: none, A/B)
	if args.get("far-shadows", "1") == "0":
		tm.far_shadow_start = 0.0 # --far-shadows=0: no far sun shadows from the ground's horizon
	tm.update_shader()
	if args.get("ground-scans", "1") != "0" and not args.has("test"): # --ground-scans=0: plain ground layers
		var nscan := _apply_ground_scans(tm, ProjectSettings.globalize_path("res://").path_join("../landscape/art/terrain_layers"), args.get("ground-scans", "1"))
		print("ground scans: %d slots" % nscan)
		if nscan > 0:
			tm.update_shader()
		if args.has("scan-check"):
			var code := tm.get_shader_code()
			print("scan-check: rock tex %s cliff scan %s, code has tm_ov_cliff %s, rock_albedo_tex %s" % [tm.rock_layer != null and tm.rock_layer.albedo_texture != null, tm.cliff_scan != null, code.contains("tm_ov_cliff("), code.contains("tm_rock_albedo_tex")])
			get_tree().quit()
			return
	if args.has("hide"):
		get_tree().process_frame.connect(func():
			for nm in str(args["hide"]).split(","):
				var nd = get_node_or_null(nm)
				if nd:
					nd.visible = false)
	# --layer-tint: each layer one flat colour (rock red, scree green, soil yellow, alluvium magenta, sand blue), diagnostics.
	if args.has("layer-tint"):
		var tints := {"rock": Color(1, 0, 0), "scree": Color(0, 1, 0), "soil": Color(1, 1, 0), "alluvium": Color(1, 0, 1), "sand": Color(0, 0, 1)}
		for ln in tints:
			var l: TerrainMaterialLayer = tm.get(ln + "_layer")
			if l == null:
				l = TerrainMaterialLayer.new()
				tm.set(ln + "_layer", l)
			l.color = tints[ln]
		tm.update_shader()
	if args.has("face-weathering"):
		tm.set_shader_parameter("tm_face_weathering", float(args["face-weathering"]))
	var land := Landscape3D.new()
	# Cliff relief is on by default (frames show the ground as the game draws it: without it a backlit cliff reads as
	# smooth white spur flanks); the checks measure the bare heightfield, and --no-cliff-relief gives a before frame.
	if args.has("bank-relief"):
		land.bank_relief = float(args["bank-relief"])
	if args.has("terracettes"):
		land.terracette_relief = float(args["terracettes"]) # --terracettes=0: no terracette steps (a before frame)
	if args.has("test"):
		land.bank_relief = 0.0
	if args.has("test") or args.has("no-cliff-relief"):
		land.cliff_relief = 0.0
	land.name = "Landscape"
	if args.has("lod-error"):
		land.lod_error = float(args["lod-error"])
	add_child(land)
	land.material = tm
	land.source = land.get_path_to(gen)
	land.collision_enabled = args.has("water") or args.has("roll") or args.has("tracks") # the sea's seabed_from_floor measures the bed through it; --roll's stones land on it
	land.camera = land.get_path_to(cam)
	land.rebuild()
	land.load_all_heights()
	if gen.has_sea_result() and not args.has("no-ground-surface"):
		_beach_surface(tm)
		if args.has("no-patch-relief"):
			get_tree().process_frame.connect(func(): RenderingServer.global_shader_parameter_set("terrain_cliff_relief", Vector4()))
	if args.has("no-frost-ground"):
		# --no-frost-ground: the frost stones without the terrain material's silt centres and gravel lag (a before frame).
		get_tree().process_frame.connect(func(): RenderingServer.global_shader_parameter_set("terrain_frost_rect", Vector4()))
	if args.has("water"):
		var waters: Array = gen.build_water(self)
		if args.has("sea-set"):
			# --sea-set=property:value[,property:value]: properties of the sea (PhysXWaterSurface3D), for off/on pairs.
			for kv in String(args["sea-set"]).split(","):
				var pv := kv.split(":", true, 1)
				for w in waters:
					if w is Node and (w as Node).name == "Sea":
						w.set(pv[0], str_to_var(pv[1]))
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
		if args.has("no-cliff-kit"):
			rocks.cliff_kit = false # --no-cliff-kit: the fluted walls without their kit pieces (the before frame)
		if args.has("no-cliff-recess"):
			rocks.cliff_recess = false # --no-cliff-recess: the pillars proud of the ridges, the face drawn whole
		if args.has("cliff-max"):
			rocks.cliff_block_max_slope = float(args["cliff-max"]) # --cliff-max=76: the wall's courses stop there (the before frame)
		if args.get("stone-scans", "1") != "0" and not args.has("test"): # --stone-scans=0: no scan detail on stones and caves (A/B)
			print("stone scans: %d" % rocks.load_scan_set(_rock_scan_dir()))
		# Real scree stones out to the layer's radius: the ground shader keeps only their bed there (--shader-scree
		# for the old parallax blocks under them, a before frame).
		if not args.has("shader-scree"):
			tm.scree_blocks_distance = rocks.get_layer_radius(RockField3D.LAYER_SCREE)
		# On by default; --no-ground-blend for an A/B.
		rocks.ground_blend = not args.has("no-ground-blend")
		rocks.ground_contact = not args.has("no-ground-contact")
		rocks.moss_cushions = not args.has("no-moss")
		rocks.close_up_enabled = not args.has("no-close-up")
		rocks.rt_instanced_cells = args.has("rt-instanced") # cell stones path traced as instances (memory A/B)
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
	if args.has("bare-spot"):
		# --bare-spot: the nearest open soil to the spot (no rock or scree near, no alluvium, a gentle 6 to 20 degree
		# slope well above the sea), for close frames of the ground layers alone.
		var best := INF
		var found := spot
		for zz in range(-600, 601, 8):
			for xx in range(-600, 601, 8):
				var p := Vector3(xx, 0, zz)
				var g := gen.get_geology_at(p)
				if g.r > 0.1 or gen.get_talus(p) > 0.05:
					continue
				var h := gen.get_height(p)
				if h < gen.sea_level + 15.0:
					continue
				var sl := rad_to_deg(atan(Vector2(gen.get_height(p + Vector3(2, 0, 0)) - gen.get_height(p - Vector3(2, 0, 0)), gen.get_height(p + Vector3(0, 0, 2)) - gen.get_height(p - Vector3(0, 0, 2))).length() / 4.0))
				if sl < 3.0 or sl > 12.0 or h > gen.sea_level + 150.0:
					continue
				var dd := Vector2(xx, zz).length()
				if dd < best:
					best = dd
					found = p
		spot = found
		print("bare spot ", spot, " geology ", gen.get_geology_at(spot))
	if args.has("gully-spot"):
		# --gully-spot: a strong gully on an open slope (TerrainGenerator3D.get_gully_at; no rock exposure or fill,
		# 15 to 35 degrees over 16 m, banks under 4 m high 10 m either side so not a ravine), for close frames.
		var best := -INF
		var found := spot
		var best_ravine := -INF
		var ravine := spot
		for zz in range(-1000, 1001, 6):
			for xx in range(-1000, 1001, 6):
				var p := Vector3(xx, 0, zz)
				var g := gen.get_geology_at(p)
				var h := gen.get_height(p)
				if g.g > 0.05 or h < gen.sea_level + 10.0:
					continue
				var gx := gen.get_height(p + Vector3(8, 0, 0)) - gen.get_height(p - Vector3(8, 0, 0))
				var gz := gen.get_height(p + Vector3(0, 0, 8)) - gen.get_height(p - Vector3(0, 0, 8))
				var ny := Vector3(-gx, 16.0, -gz).normalized().y
				if ny < cos(deg_to_rad(35.0)) or ny > cos(deg_to_rad(15.0)):
					continue
				var gl := gen.get_gully_at(p, ny)
				if gl.x < 0.5 or gl.y > 0.3:
					continue
				var t := Vector3(-gz, 0, gx).normalized() * 10.0
				var bank := maxf(gen.get_height(p + t), gen.get_height(p - t)) - h
				var score := gl.x - bank * 0.1 - absf(h - gen.sea_level - 80.0) / 400.0
				if bank > 4.0:
					# A ravine: kept only for a world whose strong gullies are all incised deeper than that.
					if score > best_ravine:
						best_ravine = score
						ravine = p
					continue
				if score > best:
					best = score
					found = p
		if best == -INF and best_ravine > -INF:
			found = ravine
		spot = found
		print("gully spot ", spot, " channel ", gen.get_gully_at(spot, 0.85))
	if args.has("spot"):
		# --spot=x,z: the view's spot at that world point (the ground streams and detail builds round it), for a
		# fixed --cam/--look shot of a place another search found.
		var sp: PackedStringArray = args["spot"].split(",")
		spot = Vector3(float(sp[0]), 0, float(sp[1]))
	# --frost[=C]: a cold, wet climate (mean annual air C at sea level, -4 by default; 700 mm a year) on a
	# WeatherMap3D, so the ground sorts into stone circles and stripes (RockField3D.frost_sorting; --no-frost off).
	if args.has("frost"):
		var maat := -4.0 if args["frost"] == "1" else float(args["frost"])
		var cr = ClassDB.instantiate("ClimateRecipe")
		cr.temperature_january = maat - 14.0
		cr.temperature_july = maat + 14.0
		cr.precipitation = 700.0
		var fw = ClassDB.instantiate("WeatherMap3D")
		fw.extent = 256.0
		fw.resolution = 32
		fw.climate = cr
		fw.season_day = int(args.get("frost-day", "230")) # late summer: the active layer thawed, the snow gone
		add_child(fw)
	if args.has("frost-spot"):
		# --frost-spot[=stripes]: the nearest ground the frost sorts fully (with --frost), flat for circles or 5 to 9
		# degrees for stripes.
		var stripes: bool = args["frost-spot"] == "stripes"
		var best_f := INF
		var found_f := spot
		# Where RockField3D's own frost gate is fully open there and 8 m round (open, even ground), the nearest; the
		# hillside's slope (over 4 m) picks circles under 2 degrees or stripes from 5.
		var tried := 0
		for zz in range(-800, 801, 8):
			for xx in range(-800, 801, 8):
				var p := Vector3(xx, 0, zz)
				var f: Vector2 = rocks.get_frost_at(p)
				if f.x < 0.95:
					continue
				tried += 1
				var h := gen.get_height(p)
				var sl := rad_to_deg(atan(Vector2(gen.get_height(p + Vector3(4, 0, 0)) - gen.get_height(p - Vector3(4, 0, 0)), gen.get_height(p + Vector3(0, 0, 4)) - gen.get_height(p - Vector3(0, 0, 4))).length() / 8.0))
				if (sl < 5.0 or sl > 9.0) if stripes else sl > 2.0:
					continue
				var open := true
				for k in 8:
					var q := p + Vector3(cos(k * PI / 4.0), 0, sin(k * PI / 4.0)) * 8.0
					if rocks.get_frost_at(q).x < 0.95 or gen.get_height(q) - h > 4.0:
						open = false
						break
				if not open:
					continue
				var dd := Vector2(xx, zz).length()
				if dd < best_f:
					best_f = dd
					found_f = p
		spot = found_f
		print("frost spot ", spot, " (", tried, " points fully sorting)")
	# --rain-climate=P: a mild climate of P mm a year (350 by default: dry, half-bare soil) on a WeatherMap3D, so bare soil
	# carries stones on rain splash pedestals (RockField3D.rain_pedestals; --no-rain-pedestals off).
	if args.has("rain-climate") and not args.has("frost"):
		var rc = ClassDB.instantiate("ClimateRecipe")
		rc.temperature_january = 6.0
		rc.temperature_july = 24.0
		rc.precipitation = 350.0 if args["rain-climate"] == "1" else float(args["rain-climate"])
		var rw = ClassDB.instantiate("WeatherMap3D")
		rw.extent = 256.0
		rw.resolution = 32
		rw.climate = rc
		rw.season_day = 200
		add_child(rw)
	if args.has("pedestal-spot"):
		# --pedestal-spot: open, stony bare soil of 6 to 20 degrees where the pedestals stand tallest (with
		# --rain-climate).
		var best_p := -INF
		var found_p := spot
		for zz in range(-800, 801, 8):
			for xx in range(-800, 801, 8):
				var p := Vector3(xx, 0, zz)
				var pa: Vector2 = rocks.get_pedestal_at(p)
				if pa.x < 0.3:
					continue
				var sl := rad_to_deg(atan(Vector2(gen.get_height(p + Vector3(4, 0, 0)) - gen.get_height(p - Vector3(4, 0, 0)), gen.get_height(p + Vector3(0, 0, 4)) - gen.get_height(p - Vector3(0, 0, 4))).length() / 8.0))
				if sl < 6.0 or sl > 20.0:
					continue
				# Open ground (the less bank or crag over it 10 m round, the better), so the light reaches it.
				var hp := gen.get_height(p)
				var rise := 0.0
				for k in 8:
					var q := p + Vector3(cos(k * PI / 4.0), 0, sin(k * PI / 4.0)) * 10.0
					rise = maxf(rise, gen.get_height(q) - hp)
				var score := pa.x + pa.y * 20.0 - maxf(rise - 2.0, 0.0) * 0.15 - Vector2(xx, zz).length() / 4000.0
				if score > best_p:
					best_p = score
					found_p = p
		spot = found_p
		print("pedestal spot ", spot, " ", rocks.get_pedestal_at(spot))
		var ps := rocks.get_pedestal_spots(Rect2(spot.x - 40, spot.z - 40, 80, 80), 1.0, 2000)
		print("pedestal spots (bare, deepest first) within 40 m: %d, first %s" % [ps.size(), ps[0] if ps.size() > 0 else Vector4()])
	if args.has("bank-spot"):
		# --bank-spot: a soil bank of 42 to 52 degrees (no rock exposure, not talus) 20 m or more long, for Landscape3D's
		# bank relief; scored by how much of the bank 8 m either side along the contour is as steep.
		var best_b := -INF
		var found_b := spot
		for zz in range(-800, 801, 6):
			for xx in range(-800, 801, 6):
				var p := Vector3(xx, 0, zz)
				var h := gen.get_height(p)
				if h < gen.sea_level + 8.0:
					continue
				var gx := gen.get_height(p + Vector3(2, 0, 0)) - gen.get_height(p - Vector3(2, 0, 0))
				var gz := gen.get_height(p + Vector3(0, 0, 2)) - gen.get_height(p - Vector3(0, 0, 2))
				var sl := rad_to_deg(atan(Vector2(gx, gz).length() / 4.0))
				if sl < 42.0 or sl > 52.0:
					continue
				var g := gen.get_geology_at(p)
				if g.r > 0.2 or gen.get_talus(p) > 0.1:
					continue
				var t := Vector3(-gz, 0, gx).normalized() * 8.0
				var score := 0.0
				for q in [p + t, p - t]:
					var qx := gen.get_height(q + Vector3(2, 0, 0)) - gen.get_height(q - Vector3(2, 0, 0))
					var qz := gen.get_height(q + Vector3(0, 0, 2)) - gen.get_height(q - Vector3(0, 0, 2))
					score += 1.0 - absf(rad_to_deg(atan(Vector2(qx, qz).length() / 4.0)) - 47.0) / 20.0
				score -= Vector2(xx, zz).length() / 4000.0
				if score > best_b:
					best_b = score
					found_b = p
		spot = found_b
		var bx := gen.get_height(spot + Vector3(2, 0, 0)) - gen.get_height(spot - Vector3(2, 0, 0))
		var bz := gen.get_height(spot + Vector3(0, 0, 2)) - gen.get_height(spot - Vector3(0, 0, 2))
		bank_side = Vector3(-bz, 0, bx).normalized()
		print("bank spot ", spot, " geology ", gen.get_geology_at(spot))
	if args.has("terracette-spot"):
		# --terracette-spot: a soil slope of 33 to 42 degrees (no rock exposure, not talus), where the grazing terracettes
		# form most (WorldContracts::terracette_at; Landscape3D.terracette_relief), seen along its contour as --bank-spot.
		var best_t := -INF
		var found_t := spot
		for zz in range(-800, 801, 6):
			for xx in range(-800, 801, 6):
				var p := Vector3(xx, 0, zz)
				if gen.get_height(p) < gen.sea_level + 8.0:
					continue
				# The slope over 3 m and over 1 m (the near ground's own, which the steps go by) both in the band.
				var gx := gen.get_height(p + Vector3(3, 0, 0)) - gen.get_height(p - Vector3(3, 0, 0))
				var gz := gen.get_height(p + Vector3(0, 0, 3)) - gen.get_height(p - Vector3(0, 0, 3))
				var sl := rad_to_deg(atan(Vector2(gx, gz).length() / 6.0))
				if sl < 31.0 or sl > 40.0:
					continue
				var fx := gen.get_height(p + Vector3(1, 0, 0)) - gen.get_height(p - Vector3(1, 0, 0))
				var fz := gen.get_height(p + Vector3(0, 0, 1)) - gen.get_height(p - Vector3(0, 0, 1))
				var fsl := rad_to_deg(atan(Vector2(fx, fz).length() / 2.0))
				if fsl < 31.0 or fsl > 40.0:
					continue
				var g := gen.get_material_geology_at(p)
				if g.r < 0.25 or g.g > 0.0 or absf(g.b) > 0.0:
					continue
				# Facing the sun's side (south, +z; north is -z) so the treads catch the light.
				var down := Vector2(-gx, -gz).normalized()
				if down.y < 0.5:
					continue
				# Where the layout's bands are strong (its patch fade): the most tread over one step's height.
				var grad := Vector2(fx, fz) / 2.0
				var hp := gen.get_height(p)
				var band := 0.0
				for k in 10:
					band = maxf(band, GrassField3D.get_terracette_at(Vector3(xx, hp - float(k) * 0.2, zz), grad).x)
				if band < 0.8:
					continue
				var score := -absf(fsl - 35.0) / 10.0 + down.y * 0.5 - Vector2(xx, zz).length() / 4000.0
				if score > best_t:
					best_t = score
					found_t = p
		spot = found_t
		var tx := gen.get_height(spot + Vector3(2, 0, 0)) - gen.get_height(spot - Vector3(2, 0, 0))
		var tz := gen.get_height(spot + Vector3(0, 0, 2)) - gen.get_height(spot - Vector3(0, 0, 2))
		bank_side = Vector3(-tz, 0, tx).normalized()
		print("terracette spot ", spot, " geology ", gen.get_material_geology_at(spot))
	var face_out := Vector3.ZERO
	if args.has("slope-map"):
		# --slope-map=cx,cz,half,step: a map of slope (tens of degrees, '#' past 80) round that point, x across, z down.
		var sm: PackedStringArray = str(args["slope-map"]).split(",")
		var smc := Vector3(float(sm[0]), 0, float(sm[1]))
		var smh := float(sm[2])
		var sms := float(sm[3])
		var zz2 := -smh
		while zz2 <= smh:
			var row := "%7.1f " % (smc.z + zz2)
			var xx2 := -smh
			while xx2 <= smh:
				var q := smc + Vector3(xx2, 0, zz2)
				var sx := gen.get_height(q + Vector3(1, 0, 0)) - gen.get_height(q - Vector3(1, 0, 0))
				var sz := gen.get_height(q + Vector3(0, 0, 1)) - gen.get_height(q - Vector3(0, 0, 1))
				var dg := rad_to_deg(atan(Vector2(sx, sz).length() / 2.0))
				row += "#" if dg >= 80.0 else str(int(dg / 10.0))
				xx2 += sms
			print(row)
			zz2 += sms
	if args.has("face-spot"):
		# --face-spot=north|south: a rock wall of 60 degrees or more facing that way (outward within 30 degrees,
		# north is -z) and 15 m high or more, the nearest; seen from its foot looking up with the sky over its top.
		var want := Vector3(0, 0, -1) if args["face-spot"] == "north" else Vector3(0, 0, 1)
		var best_w := INF
		var found_w := spot
		for zz in range(-900, 901, 6):
			for xx in range(-900, 901, 6):
				var p := Vector3(xx, 0, zz)
				var h := gen.get_height(p)
				if not is_finite(h) or h < gen.sea_level + 6.0:
					continue
				var gx := gen.get_height(p + Vector3(2, 0, 0)) - gen.get_height(p - Vector3(2, 0, 0))
				var gz := gen.get_height(p + Vector3(0, 0, 2)) - gen.get_height(p - Vector3(0, 0, 2))
				# Past the world's edge the heights are not numbers (the search runs over a 1800 m square).
				if not is_finite(gx) or not is_finite(gz):
					continue
				if rad_to_deg(atan(Vector2(gx, gz).length() / 4.0)) < 60.0:
					continue
				var o := Vector3(-gx, 0, -gz).normalized()
				if o.dot(want) < 0.87:
					continue
				if gen.get_height(p - o * 10.0) - gen.get_height(p + o * 10.0) < 25.0:
					continue
				# Open ground in front, lower than the wall's foot, for the camera.
				var fd := float(args.get("dist", "60"))
				if absf(xx) > 500 or absf(zz) > 500:
					continue
				# A wide wall: as steep 12 m either side along it.
				var wide := true
				for sd in [-12.0, 12.0]:
					var q0: Vector3 = p + o.cross(Vector3.UP) * sd
					var sx := gen.get_height(q0 + Vector3(2, 0, 0)) - gen.get_height(q0 - Vector3(2, 0, 0))
					var sz := gen.get_height(q0 + Vector3(0, 0, 2)) - gen.get_height(q0 - Vector3(0, 0, 2))
					if rad_to_deg(atan(Vector2(sx, sz).length() / 4.0)) < 52.0:
						wide = false
				if not wide:
					continue
				var open := true
				for k in range(1, 11):
					var q := p + (o + o.cross(Vector3.UP) * 0.25).normalized() * fd * k / 10.0
					if gen.get_height(q) > h - 3.0 - 6.0 * k / 10.0:
						open = false
						break
				if not open:
					continue
				var dd := Vector2(xx, zz).length()
				if dd < best_w:
					best_w = dd
					found_w = p
		spot = found_w
		var fx := gen.get_height(spot + Vector3(2, 0, 0)) - gen.get_height(spot - Vector3(2, 0, 0))
		var fz := gen.get_height(spot + Vector3(0, 0, 2)) - gen.get_height(spot - Vector3(0, 0, 2))
		face_out = Vector3(-fx, 0, -fz).normalized()
		print("face spot ", spot, " facing ", face_out)
	spot.y = gen.get_height(spot)
	focus.global_position = spot
	spot_pos = spot
	if args.has("burn"):
		# --burn=years[,r]: a burn scar that old (WorldBurnScar3D, r m round, 6) centred 2 m beyond the spot: the
		# terrain's burn overlay (Landscape3D's stand map, TerrainMaterial3D's char) over the soil.
		var bp: PackedStringArray = str(args["burn"]).split(",")
		var scar := WorldBurnScar3D.new()
		scar.age_years = float(bp[0])
		var br := float(bp[1]) if bp.size() > 1 else 6.0
		scar.size = Vector2(br, br) * 2.0
		add_child(scar)
		scar.global_position = spot + Vector3(0, 0, 2)
		print("burn scar %.1f years, %.0f m across, burn age at the spot %.2f" % [scar.age_years, br * 2.0, WorldContracts.get_fire().burn_age_at(spot)])
	if args.has("stand-test"):
		var stand: WorldStand = WorldContracts.get_stand()
		var at := spot
		if args.has("stand-at"):
			var xz: PackedStringArray = str(args["stand-at"]).split(",")
			at = Vector3(float(xz[0]), 0, float(xz[1]))
			at.y = gen.get_height(at)
		var pts := PackedVector3Array()
		for i in range(-12, 13):
			var t := float(i) * 2.0
			pts.append(at + Vector3(t, 0, 1.2 * sin(t * 0.18) - 0.6))
		stand.set_trails([{"points": pts, "width": 1.2, "passes": 600.0, "kind": "game_trail"}])
		stand.add_disturbance(at + Vector3(-3.0, 0, 2.6), 2.2, WorldStand.DISTURBANCE_ROOTING, float(args.get("rooting-age", "0.3")))
		print("stand test: trail through ", at, ", rooting at ", at + Vector3(-3.0, 0, 2.6))
	var dist := {"rockfall": 14.0, "scree": 7.0, "glacial": 12.0, "river": 14.0, "shore": 12.0, "cliff": 30.0, "tor": 22.0, "coast": 60.0, "landform": 30.0}.get(view, 10.0) as float
	if args.has("dist"):
		dist = float(args["dist"])
	cam.position = _view_from(spot, dist)
	if bank_side != Vector3.ZERO:
		# Down the bank's length from beside it, a little out from the face and above: the relief reads in silhouette.
		var bout := Vector3(gen.get_height(spot - Vector3(2, 0, 0)) - gen.get_height(spot + Vector3(2, 0, 0)), 0, gen.get_height(spot - Vector3(0, 0, 2)) - gen.get_height(spot + Vector3(0, 0, 2))).normalized()
		cam.position = spot + bank_side * dist + bout * dist * 0.35
		cam.position.y = maxf(gen.get_height(cam.position) + 1.7, spot.y + 2.0)
	if face_out != Vector3.ZERO:
		# Out from the wall's foot, a little to one side, looking up the face.
		var fside := face_out.cross(Vector3.UP) * float(args.get("face-side", "0.25"))
		cam.position = spot + (face_out + fside).normalized() * dist
		cam.position.y = maxf(gen.get_height(cam.position) + 2.0, spot.y - dist * 0.2)
	if view == "river" or view == "shore":
		# Low ground under steep sides: look down on the bed from higher, over the walls round it.
		cam.position.y = maxf(cam.position.y, spot.y + dist * 0.9)
	if args.has("top"):
		cam.position = spot + Vector3(0, float(args["top"]) if args["top"] != "1" else 30.0, 0.5)
	var look := spot + Vector3(0, 0.3, 0)
	if face_out != Vector3.ZERO:
		look = spot + Vector3(0, dist * 0.05, 0)
	if args.has("eye"):
		# --eye=h: standing --dist m off, h m above the ground, looking across it past the spot (where each ground
		# detail class stops drawing shows as a line across the frame).
		# Along the ground's contour (across the highest ground's direction), so a shore or scree band runs away.
		var land_dir := Vector3.ZERO
		var top := -INF
		for i in 16:
			var d := Vector3(cos(TAU * i / 16.0), 0, sin(TAU * i / 16.0))
			var hgt := gen.get_height(spot + d * 30.0)
			if hgt > top:
				top = hgt
				land_dir = d
		var flat := land_dir.cross(Vector3.UP).rotated(Vector3.UP, deg_to_rad(float(args.get("turn", "0"))))
		cam.position = spot + flat * dist
		cam.position.y = gen.get_height(cam.position) + float(args["eye"])
		look = spot - flat * float(args.get("beyond", "20"))
		look.y = gen.get_height(look) + 0.2
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
	if form_stone.has("run_centre") and not args.has("cam"):
		# A run of ledge slabs raked along its lip from out in front: the slabs' fronts and the joints between them.
		var c: Vector3 = form_stone["run_centre"]
		var o: Vector3 = form_stone["run_out"]
		var a: Vector3 = form_stone["run_along"]
		var d := float(args.get("dist", str(maxf(10.0, float(form_stone["run_length"]) * 0.8))))
		cam.position = c + (o + a * float(args.get("side", "0.7"))).normalized() * d
		cam.position.y = maxf(gen.get_height(cam.position) + 1.7, c.y + float(args.get("cam-up", "1.0")))
		look = c
	if form_stone.has("basis") and args.get("form", "") == "ledge" and not args.has("cam"):
		# A ledge slab from out over its lip and to one side, a little under its top: the overhang and its soffit.
		var lb: Basis = form_stone["basis"]
		var o := Vector3(lb.z.x, 0, lb.z.z).normalized()
		var sd := Vector3(-o.z, 0, o.x)
		var lip: Vector3 = form_stone["position"] + lb.z * 0.8
		var d := float(args.get("dist", "9"))
		cam.position = lip + (o + sd * float(args.get("side", "0.9"))).normalized() * d
		cam.position.y = maxf(gen.get_height(cam.position) + 1.7, lip.y + float(args.get("cam-up", "0.5")))
		look = lip
	cam.look_at(look)
	if args.has("cam"):
		# --cam/--look win over the view's camera (a fixed shot of a spot in the world).
		var c: PackedStringArray = args["cam"].split(",")
		var l: PackedStringArray = args.get("look", "0,0,0").split(",")
		cam.position = Vector3(float(c[0]), float(c[1]), float(c[2]))
		cam.look_at(Vector3(float(l[0]), float(l[1]), float(l[2])))
	# --snow=CM: an even, settled pack of CM centimetres (8) over the ground round the spot (a cold WeatherMap3D, a
	# SnowCover3D and its deep snow surface round the camera), for RockField3D.snow_burial (--no-snow-burial off).
	if args.has("snow"):
		var cm := 8.0 if args["snow"] == "1" else float(args["snow"])
		var sw = get_node_or_null("SnowWeather")
		if sw == null:
			sw = WeatherMap3D.new()
			sw.name = "SnowWeather"
			sw.extent = 512.0
			sw.resolution = 32
			sw.temperature = -4.0
			sw.humidity = 0.8
			sw.cloud_cover = 0.0
			sw.cloud_variation = 0.0
			sw.wind_speed = 2.0
			sw.rain_rate = 0.0
			add_child(sw)
			sw.snap_to_settings()
		var sc := SnowCover3D.new()
		sc.name = "SnowCover3D"
		sc.extent = 256.0
		sc.resolution = 256
		add_child(sc)
		sc.global_position = Vector3(cam.position.x, 0, cam.position.z)
		sc.height_source = sc.get_path_to(gen)
		sc.sun_light = sc.get_path_to(get_node("Sun"))
		sc.update_rate = 0.0001
		sc.refresh_heights()
		sc.add_snow(cm * 0.01 * 250.0, 250.0) # settled: 250 kg/m3
		sc.advance(1.0) # (publishes the pack: snow_map for the materials, the cells for the deep snow surface)
		print("snow: %.3f m under the camera" % sc.get_depth_at(cam.position))
	if has_node("SnowCover3D") and not args.has("no-snow-surface"):
		var ss := SnowSurface3D.new()
		ss.name = "SnowSurface"
		ss.snow_cover = NodePath("../SnowCover3D")
		add_child(ss)
		ss.follow_target = ss.get_path_to(cam)
	if args.get("focus", "") == "camera":
		focus.global_position = cam.position # --focus=camera: the field streams and thins round the camera, not the spot
	print("spot %s, camera %s" % [spot, cam.position])
	if args.has("rain-climate"):
		# The season the cured cover reads (WorldContracts::cured_share_for_season): soil moisture and temperature.
		var ss: Dictionary = WorldContracts.get_weather().season_at(spot)
		print("season at the spot: soil_moisture %s, temperature %s, cured %.2f" % [ss.get("soil_moisture"), ss.get("temperature"), 1.0 - clampf(smoothstep(0.1, 0.4, float(ss.get("soil_moisture", 0.0))) * smoothstep(-8.0, 2.0, float(ss.get("temperature", 0.0))), 0.0, 1.0)])
	# --mat-prop's shader uniforms (tm_sand_color:Color(1,0,1), a layer marked), set last so the material's own setup
	# does not overwrite them.
	for kv in str(args.get("mat-prop", "")).split(";", false):
		var mu: PackedStringArray = kv.split(":")
		if mu[0].begins_with("tm_"):
			land.material.set_shader_parameter(mu[0], str_to_var(mu[1]))
	print("ground at the spot: generator %.2f, landscape %.2f; camera %s" % [gen.get_height(spot), land.get_height(spot), cam.position])
	if args.has("terracette-spot"):
		# The steps' relief down the fall line from the spot (WorldGround.relief_at, x the offset) every 0.5 m.
		var fall := Vector3(gen.get_height(spot - Vector3(2, 0, 0)) - gen.get_height(spot + Vector3(2, 0, 0)), 0, gen.get_height(spot - Vector3(0, 0, 2)) - gen.get_height(spot + Vector3(0, 0, 2))).normalized()
		var steps := PackedStringArray()
		for i in range(0, 24):
			var q := spot + fall * (float(i) * 0.5)
			steps.append("%.2f" % WorldContracts.get_ground().relief_at(q).x)
		print("terracette relief down the fall line: ", ", ".join(steps))
	if rocks:
		if args.has("only-layer"):
			for i in LAYERS.size():
				rocks.set_layer_enabled(i, LAYERS[i] == view)
		if args.has("skip-layers"):
			# --skip-layers=a,b: those layers off (to find which layer a stone in a frame comes from)
			for nm in str(args["skip-layers"]).split(","):
				if LAYERS.has(nm):
					rocks.set_layer_enabled(LAYERS.find(nm), false)
		if args.has("stone-density"):
			rocks.set_layer_density(LAYERS.find(view), float(args["stone-density"]))
		# Ground detail: the stones under min_size near the camera as real geometry (docs/ideas/ground-detail.md);
		# --detail-ref=px the image height the reach is set for (0: the window's), --detail-pixels=n the smallest drawn.
		rocks.ground_detail = not (args.has("no-ground-detail") or args.has("test"))
		rocks.far_thinning = not args.has("no-far-thinning") # --no-far-thinning: every layer drawn whole to its radius (a before frame)
		rocks.far_widen = not args.has("no-far-widen") # --no-far-widen: thinning without widened survivors
		# --no-far: no far tier (big stones and landmarks past each layer's radius, to 4 km); off in --test (time).
		if args.has("no-far") or args.has("test"):
			rocks.far_radius = 0.0
		rocks.ground_detail_cover = not args.has("no-ground-cover")
		rocks.ground_detail_rivers = not args.has("no-river-detail")
		# --tiles / --no-tiles: tiles of piles on or off (else the default), the A/B of the tiles.
		rocks.ground_detail_tiles = args.has("tiles") or (rocks.ground_detail_tiles and not args.has("no-tiles"))
		rocks.stones_react = not args.has("no-react")
		if rocks.ground_detail:
			rocks.ground_detail_reference_height = int(args.get("detail-ref", "1080"))
			if args.has("detail-pixels"):
				rocks.ground_detail_pixels = float(args["detail-pixels"])
			rocks.ground_detail_threads = not args.has("detail-sync")
		rocks.cell_threads = not args.has("cell-sync")
		var tb := Time.get_ticks_msec()
		# --trail[=passes]: a game trail (0.8 m, 400 passes a year) across the spot along x, 1.5 m before it;
		# --rooting[=years]: a boar rooting 1.5 m across, 2 m behind the spot, that age (0: fresh). WorldStand.
		rocks.frost_sorting = not args.has("no-frost")
		rocks.runoff_gravel = not args.has("no-runoff-gravel") # --no-runoff-gravel: the rills and gullies without their lag (a before frame)
		rocks.rain_pedestals = not args.has("no-rain-pedestals") # --no-rain-pedestals: bare soil without its stones and pedestals (a before frame)
		rocks.snow_burial = not args.has("no-snow-burial") # --no-snow-burial: every stone drawn, under the snow too (a before frame)
		if args.has("frost"):
			var fr := ""
			for dz in [-6, -3, 0, 3, 6]:
				for dx in [-6, -3, 0, 3, 6]:
					var f: Vector2 = rocks.get_frost_at(spot + Vector3(dx, 0, dz))
					fr += " %.2f/%.2f" % [f.x, f.y]
			print("frost gate/share round the spot:", fr)
		if args.has("trail"):
			var passes := 400.0 if args["trail"] == "1" else float(args["trail"])
			WorldContracts.get_stand().set_trails([{"points": PackedVector2Array([Vector2(spot.x - 40, spot.z - 1.5), Vector2(spot.x + 40, spot.z - 1.5)]), "width": 0.8, "passes": passes, "kind": "game_trail"}])
		if args.has("rooting"):
			var age := 0.0 if args["rooting"] == "1" else float(args["rooting"])
			WorldContracts.get_stand().add_disturbance(spot + Vector3(0, 0, 2), 1.5, WorldStand.DISTURBANCE_ROOTING, age)
		rocks.ground_wear = not args.has("no-wear")
		if args.has("trail") or args.has("rooting"):
			var stw: WorldStand = WorldContracts.get_stand()
			print("worn ground: trail %s at the trail's centre, rooting age %s at the rooting's" % [stw.trail_at(Vector3(spot.x, 0, spot.z - 1.5)), stw.rooting_age_at(spot + Vector3(0, 0, 2))])
		var built := rocks.build_area(Rect2(spot.x - 160, spot.z - 160, 320, 320))
		if args.has("streaming"):
			rocks.streaming = true # --streaming: the field streams and thins every frame as in play (stills draw every cell whole)
		print("rocks: %d cells, %d stones in %d ms (shape libraries %.0f ms, %d library triangles)" % [built, rocks.get_stone_count(), Time.get_ticks_msec() - tb, rocks.get_last_library_msec(), rocks.get_library_triangle_count()])
		if args.has("recess-probe"):
			# --recess-probe=cx,cz,half,step: over that square, how much of the steep face the cliff kit recesses (WorldGround.recess_at at the unrecessed drawn face).
			var rp: PackedStringArray = str(args["recess-probe"]).split(",")
			var wg2: WorldGround = WorldContracts.get_ground()
			var n_face := 0
			var n_cov := 0
			var dsum := 0.0
			var hist := [0, 0, 0, 0, 0]
			var zz3 := -float(rp[2])
			while zz3 <= float(rp[2]):
				var xx3 := -float(rp[2])
				while xx3 <= float(rp[2]):
					var q := Vector3(float(rp[0]) + xx3, 0, float(rp[1]) + zz3)
					var sx := gen.get_height(q + Vector3(1, 0, 0)) - gen.get_height(q - Vector3(1, 0, 0))
					var sz := gen.get_height(q + Vector3(0, 0, 1)) - gen.get_height(q - Vector3(0, 0, 1))
					if rad_to_deg(atan(Vector2(sx, sz).length() / 2.0)) >= 45.0:
						var h := gen.get_height(q)
						q.y = h + wg2.relief_at(Vector3(q.x, h, q.z), true, false).x
						n_face += 1
						var r := wg2.recess_at(q)
						if r.y > 0.0:
							n_cov += 1
							dsum += r.x * r.y
							hist[mini(int(r.x * r.y), 4)] += 1
					xx3 += float(rp[3])
				zz3 += float(rp[3])
			print("recess probe: %d face points, %d covered (%.0f%%), mean step %.2f m, by metre %s" % [n_face, n_cov, 100.0 * n_cov / maxi(n_face, 1), dsum / maxi(n_cov, 1), str(hist)])
		if args.has("repetition"):
			_repetition_audit(Rect2(spot_pos.x - 120, spot_pos.z - 120, 240, 240))
		if rocks.far_radius > 0.0:
			# The far tier: big stones and landmarks past each layer's radius, to far_radius (streaming is off here).
			var tfar := Time.get_ticks_msec()
			rocks.build_far()
			print("far tier: %d stones to %.0f m in %d ms" % [rocks.get_far_stone_count(), rocks.far_radius, Time.get_ticks_msec() - tfar])
			if args.has("save-far-cover"):
				# --save-far-cover=path.png: the far cover map (rgb stone colour, a cover) as the terrain reads it
				var fct: Texture2D = rocks.get_far_cover_texture()
				var fimg: Image = fct.get_image() if fct else null
				if fimg:
					fimg.save_png(str(args["save-far-cover"]))
					var nz := 0
					for fy in fimg.get_height():
						for fx in fimg.get_width():
							nz += 1 if fimg.get_pixel(fx, fy).a > 0.0 else 0
					print("far cover: %d of %d texels with stone" % [nz, fimg.get_width() * fimg.get_height()])
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
		if args.has("ledge-fall"):
			await _ledge_fall(rocks.get_stones(RockField3D.LAYER_CLIFF, Rect2(-800, -800, 1600, 1600)))
		if args.has("live"):
			_live_falls(spot, land)
	if args.has("caves"):
		_caves(cam)


# Cave networks (CaveNetwork3D, docs/ideas/rocks.md "Real caves"): grown from the first entrances found in the
# world, the skeleton drawn through the ground (colour by passage type) from above and in front.
func _caves(cam: Camera3D) -> void:
	var net := CaveNetwork3D.new()
	net.name = "Caves"
	if args.get("stone-scans", "1") != "0":
		print("cave scans: %d" % net.load_scan_set(_rock_scan_dir()))
	if args.has("scallops"):
		net.scallops = float(args["scallops"]) # --scallops=k: the walls' dish hollows (0 off, a before frame)
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
	if sea_caves and not args.has("test") and found.size() == 1 and args.get("sea-cave-pick", "lit") == "lit":
		# One sea cave for a frame: of the best dozen sites, the one whose mouth faces the sun most (its mouth and
		# the sea in front lit, not a cleft in its own cliff's shadow); --sea-cave-pick=first keeps the finder's own.
		var to_sun := Vector3(0, 0, 1)
		for n in get_children():
			if n is DirectionalLight3D:
				to_sun = (n as DirectionalLight3D).global_transform.basis.z
		var flat_sun := Vector3(to_sun.x, 0, to_sun.z).normalized()
		var many: Array = net.find_sea_entrances(r, 12)
		var best := -2.0
		for e in many:
			var o: Vector3 = e["out"]
			var lit := Vector3(o.x, 0, o.z).normalized().dot(flat_sun)
			if lit > best:
				best = lit
				found = [e]
		print("  sea cave picked by facing: %d sites, mouth at %s facing the sun %.2f" % [many.size(), found[0]["position"], best])
	if sea_caves and not args.has("test") and found.size() == 1 and args.has("sea-cave-near"):
		# --sea-cave-near=x,z: the site nearest that point, so a frame keeps its cave when the sun turns.
		var near: PackedStringArray = str(args["sea-cave-near"]).split(",")
		var at := Vector2(float(near[0]), float(near[1]))
		var closest := INF
		for e in net.find_sea_entrances(r, 12):
			var ep: Vector3 = e["position"]
			if Vector2(ep.x, ep.z).distance_to(at) < closest:
				closest = Vector2(ep.x, ep.z).distance_to(at)
				found = [e]
		print("  sea cave picked near %s: mouth at %s" % [at, found[0]["position"]])
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
			# The bench already has no ambient underground (the environment's is zeroed for cave views), so the
			# network's interior ambient probes would only add light the path tracer does not have.
			net.cave_ambient_probes = false
			if args.has("cave-cell"):
				net.mesh_cell = float(args["cave-cell"]) # --cave-cell=m: the cave mesh's grid (0.3 by default)
			var mr := float(args.get("mesh-radius", "50"))
			var pos: Vector3 = e["position"]
			net.rimstone_water = not args.has("no-pool-water")
			net.sea_mist = not args.has("no-sea-mist")
			net.sea_water = not args.has("no-sea-water")
			if args.has("no-sea-clip"):
				# The sea not cut along the passages (a diagnostic: the cave's surface over the sea's own).
				net.ready.connect(func(): net.remove_from_group("water_clippers"))
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
			if args.has("cam") and not args.has("no-refine"):
				# A fixed --cam shot: the chunks round the lens on the near grid, as round a player in play (the coarse
				# grid alone showed walls smoother than anyone sees them); --no-refine keeps the coarse one.
				var n_cam := net.refine_meshes(cam.position)
				print("  refined %d chunks round the camera (cell %.2f m)" % [n_cam, net.near_mesh_cell])
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
				if not args.has("no-refine"):
					# The chunks between the lens and the mouth on the near grid, as round a player (the thin rock at a
					# mouth tears on the coarse grid); --no-refine: all on the coarse one.
					var n_in := net.refine_meshes(eye.lerp(pos, 0.8))
					print("  refined %d chunks between the camera and the mouth (cell %.2f m)" % [n_in, net.near_mesh_cell])
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
				var order: Array = []
				var near_n: Array = []
				for k in pools_v.size():
					var n_near := 0
					for k2 in pools_v.size():
						if (pools_v[k2]["center"] as Vector3).distance_to(pools_v[k]["center"]) < 3.0:
							n_near += 1
					near_n.append(n_near)
					var rr := 0.0
					for v in (pools_v[k]["outline"] as PackedVector2Array):
						rr += v.length() / 32.0
					order.append([float(pools_v[k]["depth"]) * rr * (1.0 + 0.3 * n_near), k])
				order.sort_custom(func(x, y): return x[0] > y[0])
				# Best pool first; a pool in a passage too low to stand the camera in (no air round it) is passed over, or the
				# camera and lamp end up inside the rock and the frame is black.
				var eye_r := pc + Vector3(0.9, 1.3, 0.6)
				var dist_r := float(args.get("dist", "1.6"))
				for ord_k in order:
					var k: int = ord_k[1]
					var pk: Vector3 = pools_v[k]["center"]
					var best_air_r := -1.0
					var eye_k := pk + Vector3(0.9, 1.3, 0.6)
					# --downstream: from below the pool's dam, looking back up at its downstream face (toward the nearest lower pool).
					var down_k := Vector2.ZERO
					if args.has("downstream"):
						var dn_best := 1e9
						for k3 in pools_v.size():
							var p3: Vector3 = pools_v[k3]["center"]
							var d3 := Vector2(p3.x - pk.x, p3.z - pk.z).length()
							if k3 != k and p3.y < pk.y - 0.01 and d3 < 4.0 and d3 < dn_best:
								dn_best = d3
								down_k = Vector2(p3.x - pk.x, p3.z - pk.z).normalized()
						if down_k == Vector2.ZERO:
							continue
					seed(11)
					for ii in 400:
						var cand := pk + Vector3(randf_range(-3, 3), randf_range(0.25, 0.45) if args.has("low") else randf_range(0.8, 1.8), randf_range(-3, 3))
						var dd := Vector2(cand.x - pk.x, cand.z - pk.z).length()
						if dd < dist_r * 0.8 or dd > dist_r * 1.3:
							continue
						if down_k != Vector2.ZERO and Vector2(cand.x - pk.x, cand.z - pk.z).dot(down_k) < 0.7 * dd:
							continue
						var air := -net.get_rock_distance(cand)
						if air > 0.25 and minf(air, 1.0) > best_air_r:
							best_air_r = minf(air, 1.0)
							eye_k = cand
					if best_k < 0 or best_air_r > 0.0:
						best_k = k
						best_c = near_n[k]
						pc = pk
						eye_r = eye_k
					if best_air_r > 0.0:
						break
				cam.position = eye_r
				# --low: a grazing view across the pool, so it mirrors the roof and its stalactites beyond.
				cam.look_at(pc + (Vector3(pc.x - eye_r.x, 0, pc.z - eye_r.z).normalized() * 1.5 + Vector3(0, 0.15, 0) if args.has("low") and not args.has("downstream") else Vector3.ZERO))
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
				if not args.has("no-refine"):
					# The chunks round the lens on the near grid (--no-refine: all on the coarse one).
					var t_ref := Time.get_ticks_msec()
					var n_ref := net.refine_meshes(eye_s)
					print("  refined %d chunks round the camera in %d ms (cell %.2f m)" % [n_ref, Time.get_ticks_msec() - t_ref, net.near_mesh_cell])
				# Down the channel, a little below level; a narrower lens so the walls enter the frame only past a metre.
				cam.look_at(Vector3(mid.x, eye_s.y, mid.z) + dir * 3.0 - Vector3(0, float(args.get("look-down", "0.35")), 0)) # --look-down=m: aim lower, onto the water
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
					# Low, dim and short-reaching: its light falls mainly on the water round it, the walls stay dark.
					lamp_g.omni_range = 4.0
					lamp_g.omni_attenuation = 2.0
					lamp_g.light_energy = float(args.get("graze-energy", "0.8"))
					lamp_g.shadow_enabled = true
					add_child(lamp_g)
					# Kept on this segment's own water, short of its end: past it the stream may drop down a cascade, and a
					# lamp down in the plunge would light the whole passage through the fall's water.
					var ahead := minf(float(args.get("graze-dist", "5.0")), float(args.get("dist", "2.5")) + 0.5 * (sb - sa).length() - 0.4)
					var g_at := eye_s + dir * maxf(ahead, 1.5)
					var bed_eye := eye_s.y - float(args.get("eye-height", "0.4"))
					var g_hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(g_at.x, bed_eye + 0.8, g_at.z), Vector3(g_at.x, bed_eye - 0.6, g_at.z)))
					g_at.y = (g_hit["position"].y if g_hit else bed_eye) + 0.12
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
					if OS.get_environment("ROCK_NEAR_DEBUG") == "1" and rocks:
						var cp := cam.global_position
						for ob in rocks.get_obstacles(Rect2(cp.x - 8.0, cp.z - 8.0, 16.0, 16.0), 0.1):
							print("    field stone at (%.2f, %.2f, %.2f) r %.2f, camera y %.2f" % [ob.x, ob.y, ob.z, ob.w, cp.y])
						for mmi in rocks.find_children("*", "MultiMeshInstance3D", true, false):
							var ab: AABB = (mmi as MultiMeshInstance3D).global_transform * (mmi as MultiMeshInstance3D).get_aabb()
							if ab.grow(1.0).has_point(cp) or ab.intersects(AABB(cp - Vector3(6, 3, 6), Vector3(12, 6, 12))):
								print("    field group %s aabb %s" % [mmi.name, ab])
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
	rocks.build_area(Rect2(spot_pos.x - 120, spot_pos.z - 120, 240, 240))
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
func _rockfall_run(p_at: Vector3, p_radius: float, p_ticks: int, p_log: bool, p_dust_ticks: int = 0) -> Dictionary:
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
	# Undrawn, the Flow plume is not stepped either (its grid on a software GPU in the cloud takes seconds a tick); the
	# field still counts the fines and the impacts.
	var dust_node: Node = rocks.get_rockfall_dust_node()
	var dust_mode := dust_node.process_mode if dust_node else Node.PROCESS_MODE_INHERIT
	if dust_node and not args.has("fall-dust"):
		dust_node.process_mode = Node.PROCESS_MODE_DISABLED
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
	var below := 0
	for i in node.get_piece_count():
		var o: Vector3 = node.get_piece_transform(i).origin
		if o.y < gen.get_height(o) - 1.0:
			below += 1
	# p_dust_ticks more ticks for the dust alone (a block that slides down the face lands after the fall is measured).
	for i in p_dust_ticks:
		if rocks.get_rockfall_dust_impacts() > hits0:
			break
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	get_window().size = full_size
	if dust_node:
		dust_node.process_mode = dust_mode
	var r := {"node": node, "start": start, "end": last, "drop": top - last.y, "travel": Vector2(last.x - start.x, last.z - start.z).length(), "pieces": node.get_piece_count(), "fractured": node.is_fractured(), "fired": fired.size(), "ms": ms, "max_speed": max_speed, "below": below}
	r["dust"] = rocks.get_rockfall_dust_total() - dust0
	r["hits"] = rocks.get_rockfall_dust_impacts() - hits0
	if p_log:
		print("fell %.1f m and %.1f m out, top speed %.1f m/s, %d pieces (%s), %d under the ground" % [r["drop"], r["travel"], max_speed, r["pieces"], "broke" if r["fractured"] else "whole", below])
		var dn: Node = rocks.get_rockfall_dust_node()
		print("dust: %.1f kg of fines from %d impacts; Flow node %s, %.1f kg taken, %d bursts running" % [r["dust"], r["hits"], "yes" if dn else "none (engine without Flow)", dn.get_total_dust() if dn else 0.0, dn.get_active_sources() if dn else 0])
	return r



# A ledge slab fails as a cantilever (the test, and --ledge-fall with --world): the biggest slab that can fall.
func _ledge_fall(cliff: Array) -> void:
	# A ledge slab fails as a cantilever: cut at its root over the bench's edge, its back stays under the bench and
	# the part out over the lip falls.
	# (The biggest slabs first: one whose free part would start in the ground is refused, and the next is tried.)
	var slabs := []
	for st in cliff:
		if int(st["family"]) == RockGenerator.FAMILY_LEDGE:
			slabs.append(st)
	slabs.sort_custom(func(a, b): return float(a["scale"]) > float(b["scale"]))
	var slab: Dictionary = {}
	var refused := 0
	for st in slabs.slice(0, 40):
		var p: Vector3 = st["position"]
		var land0: Landscape3D = get_node("Landscape")
		cam.position = p + Vector3(0, 20, 30)
		land0.collision_enabled = true
		land0.rebuild()
		land0.load_all_heights()
		rocks.collision_enabled = true
		rocks.rebuild()
		rocks.build_area(Rect2(p.x - 60, p.z - 60, 120, 120))
		if rocks.trigger_rockfall(p, 0.3) != null:
			rocks.restore_broken()
			slab = st
			break
		refused += 1
	print("ledge fall: %d of the biggest slabs refused (their free part would start in the ground)" % refused)
	if not slab.is_empty():
		var sp: Vector3 = slab["position"]
		var land: Landscape3D = get_node("Landscape")
		cam.position = sp + Vector3(0, 20, 30)
		land.collision_enabled = true
		land.rebuild()
		land.load_all_heights()
		rocks.collision_enabled = true
		rocks.rebuild()
		rocks.build_area(Rect2(sp.x - 60, sp.z - 60, 120, 120))
		await get_tree().physics_frame
		var sb: Basis = slab["basis"]
		var fz := Vector3(sb.z.x, 0, sb.z.z).normalized()
		for k in 4:
			var q := sp + fz * (0.5 * k)
			var hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(q + Vector3(0, 40, 0), q - Vector3(0, 60, 0)))
			var land_h: float = (get_node("Landscape") as Landscape3D).get_height(q)
			print("  ledge ground %.1f m out: collision %s, heightfield %.2f, landscape %.2f, slab centre y %.2f" % [0.5 * k, ("%.2f" % hit["position"].y) if hit else "none", gen.get_height(q), land_h, sp.y])
		var r := await _rockfall_run(sp, 0.3, 180, true, 180)
		check(not r.is_empty(), "ledge fall: the slab at %s loosed" % sp)
		if not r.is_empty():
			var node: Node3D = r["node"]
			var held := 0
			var fell := 0
			var under := 0
			var drop := 0.0
			for i in node.get_piece_count():
				var c: Vector3 = node.get_piece_center(i)
				var d := sp.y - c.y
				if node.get_piece_linear_velocity(i).length() < 0.01 and absf(d) < 2.0 * float(slab["scale"]):
					held += 1
					continue
				fell += 1
				drop = maxf(drop, d)
				# Under the ground: below the lowest ground 3 m round (beside a cliff the height at the very spot
				# can be the face's top).
				var low := gen.get_height(c)
				for k in 8:
					low = minf(low, gen.get_height(c + Vector3(cos(k * PI / 4.0), 0, sin(k * PI / 4.0)) * 3.0))
				if c.y < low - 1.0:
					under += 1
				print("  fallen piece at %s, ground there %.1f, lowest 3 m round %.1f" % [c, gen.get_height(c), low])
			print("ledge fall: %d pieces held under the bench, %d fell, deepest %.1f m down, %d under the ground" % [held, fell, drop, under])
			# (Known, so live rockfall leaves ledges out: the falling part drops through the collision ground.)
			check(held > 0 and fell > 0, "ledge fall: cracked at the root, %d held, %d fell" % [held, fell])
			print("KNOWN ledge fall: %.1f m down, %d under the ground (should be 0)" % [drop, under])

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
	if layer == RockField3D.LAYER_RIVER and args.has("gravel-reach"):
		# --gravel-reach[=d]: a gravel-bed reach (the bed's median under the river layer's smallest stones, so only
		# ground detail shows it), the widest whose median is nearest d (m, default 0.03); as the river layer's sizes.
		var want := float(args["gravel-reach"]) if args["gravel-reach"] != "1" else 0.03
		var best_score := INF
		var best_p := Vector3.ZERO
		var rvs: Array = gen.get_rivers(rocks.river_min_area)
		var wmax := 0.0
		for rv in rvs:
			for wv in (rv["widths"] as PackedFloat32Array):
				wmax = maxf(wmax, wv)
		print("river: %d rivers, widest %.1f m" % [rvs.size(), wmax])
		for rv in rvs:
			var pts: PackedVector3Array = rv["points"]
			var wid: PackedFloat32Array = rv["widths"]
			var areas: PackedFloat32Array = rv["areas"]
			var dist := 0.0
			for i in range(1, pts.size() - 1):
				dist += Vector2(pts[i].x - pts[i - 1].x, pts[i].z - pts[i - 1].z).length()
				if absf(pts[i].x) > 700 or absf(pts[i].z) > 700 or wid[i] < minf(3.0, 0.5 * wmax) or pts[i].y < gen.sea_level + 8.0:
					continue
				var q: float = gen.get_discharge(areas[i])
				var depth := 0.27 * pow(maxf(q, 1e-3), 0.4)
				var run := Vector2(pts[i + 1].x - pts[i].x, pts[i + 1].z - pts[i].z).length()
				var slope := clampf((pts[i].y - pts[i + 1].y) / maxf(run, 1e-3), 1e-4, 0.3)
				var d50 := clampf(minf(depth * slope / (1.65 * 0.045), 0.5 * exp(-dist / 20000.0)), 0.005, 1.2)
				# In the open (no gorge walls within 15 m either side), so the sun reaches the bed.
				var fd := Vector2(pts[i + 1].x - pts[i].x, pts[i + 1].z - pts[i].z).normalized()
				var sd := Vector3(-fd.y, 0, fd.x)
				var walls := maxf(gen.get_height(pts[i] + sd * 15.0), gen.get_height(pts[i] - sd * 15.0)) - pts[i].y
				var score := absf(log(d50 / want)) - 0.02 * wid[i] + maxf(walls - 3.0, 0.0) * 0.3
				if score < best_score:
					best_score = score
					best_p = pts[i]
		print("river: gravel reach at %s (score %.2f)" % [best_p, best_score])
		return best_p
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
	if layer == RockField3D.LAYER_CLIFF and args.get("form", "") == "ledge-run":
		# --view=cliff --form=ledge-run: the longest run of slabs along one lip (get_ledge_slabs), sunlit first.
		var runs := {}
		for s in rocks.get_ledge_slabs(Rect2(-800, -800, 1600, 1600)):
			var r := int(s["run"])
			if not runs.has(r):
				runs[r] = []
			runs[r].append(s)
		var hist := [0, 0, 0, 0]
		var n_sl := 0
		for r in runs.values():
			hist[mini(r.size(), 4) - 1] += 1
			n_sl += r.size()
		print("ledge slabs: %d in %d runs (1/2/3/4+ slabs: %s)" % [n_sl, runs.size(), hist])
		if args.has("ledge-dump"):
			var all := rocks.get_ledge_slabs(Rect2(-800, -800, 1600, 1600))
			for s in all:
				var nn := 1e9
				var who := {}
				for t in all:
					if t != s and (t["top"] - s["top"]).length() < nn:
						nn = (t["top"] - s["top"]).length()
						who = t
				print("slab %s len %.1f depth %.1f thick %.1f hang %.1f | nearest %.1f m, along-dot %.2f out-dot %.2f dy %.1f" % [s["top"], s["length"], s["depth"], s["thickness"], s["overhang"], nn, absf((who["top"] - s["top"]).normalized().dot(s["along"])), s["out"].dot(who["out"]), who["top"].y - s["top"].y])
		var best_r := []
		var best_sc := -1.0
		for r in runs.values():
			var c := Vector3()
			for s in r:
				c += s["top"]
			c /= r.size()
			var sc := float(r[0]["run_length"]) + (1000.0 if _sunlit(c + Vector3(0, 1, 0)) else 0.0)
			if sc > best_sc:
				best_sc = sc
				best_r = r
		if not best_r.is_empty():
			var c := Vector3()
			var o := Vector3()
			for s in best_r:
				c += s["front"]
				o += s["out"]
			c /= best_r.size()
			form_stone = {"run_centre": c, "run_out": o.normalized(), "run_along": best_r[0]["along"], "run_length": float(best_r[0]["run_length"])}
			print("ledge run: %d slabs, %.1f m, at %s" % [best_r.size(), best_r[0]["run_length"], c])
			return c
	if layer == RockField3D.LAYER_CLIFF and args.get("form", "") == "ledge":
		# --view=cliff --form=ledge: where the ledge slabs stand thickest.
		var ledges := []
		for s in stones:
			if int(s["family"]) == RockGenerator.FAMILY_LEDGE:
				ledges.append(s)
		print("ledge slabs in view range: %d" % ledges.size())
		# The biggest in the sun, framed from out and to one side (the ledge view below).
		var best := {}
		for s in ledges:
			var sc := float(s["scale"]) + (100.0 if _sunlit(s["position"] + Vector3(0, 1, 0)) else 0.0)
			if best.is_empty() or sc > float(best["score"]):
				best = s.duplicate()
				best["score"] = sc
		if not best.is_empty():
			print("ledge slab at %s (scale %.2f)" % [best["position"], best["scale"]])
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


func _test_parts() -> void:
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


func _test() -> void:
	if not args.has("test-world"): # --test-world: only part 3 (the field on a generated world)
		_test_parts()
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
				print("  buried: %s family %d scale %.2f at %s, %.2f under its ground" % [LAYERS[l], int(s["family"]), sc, p, float(s["ground"]) - p.y])
			# A cliff block's centre lies inside the wall: under the terrain at its own plan position. A ledge slab
			# hangs out over its lip: its back half lies under the bench instead.
			if l == RockField3D.LAYER_CLIFF and int(s["family"]) == RockGenerator.FAMILY_LEDGE:
				var lb: Basis = s["basis"]
				var back := p - lb.z * 0.5
				if back.y > gen.get_height(back) + 0.1:
					floating += 1
					print("  ledge slab floating at %s: its back %.2f over the bench (basis %s, ground at the centre %.2f)" % [p, back.y - gen.get_height(back), lb, gen.get_height(p)])
			elif l == RockField3D.LAYER_CLIFF and p.y > gen.get_height(p) + 0.1:
				floating += 1
		var want_some := true
		check(not want_some or st.size() > 0, "%s: %d stones over the world in %d ms" % [LAYERS[l], st.size(), Time.get_ticks_msec() - t0])
		if l == RockField3D.LAYER_CLIFF:
			# Plants keep off the ledge slabs' tops: get_obstacles has a disc over every slab.
			var ledges := 0
			var covered := 0
			for s in st:
				if int(s["family"]) != RockGenerator.FAMILY_LEDGE:
					continue
				ledges += 1
				if ledges > 40:
					continue
				var lp: Vector3 = s["position"]
				var lb: Basis = s["basis"]
				var top := lp + lb.y * 0.25
				for o in rocks.get_obstacles(Rect2(lp.x - 4, lp.z - 4, 8, 8), 0.0):
					if Vector2(o.x - top.x, o.z - top.z).length() < o.w and absf(o.y - top.y) < 1.0:
						covered += 1
						break
			check(ledges > 0 and covered == mini(ledges, 40), "ledge slabs as plant obstacles: %d of %d checked have a disc over their top" % [covered, mini(ledges, 40)])
			# get_ledge_slabs: the same slabs, grouped into runs along their lips.
			var t1 := Time.get_ticks_msec()
			var ls := rocks.get_ledge_slabs(wr)
			var runs := {}
			var longest := 0
			for s in ls:
				runs[int(s["run"])] = int(s["run_slabs"])
				longest = maxi(longest, int(s["run_slabs"]))
			var hist := [0, 0, 0, 0]
			for r in runs.values():
				hist[mini(int(r), 4) - 1] += 1
			print("  ledge runs: %d slabs in %d runs (1/2/3/4+ slabs: %s), longest %d slabs" % [ls.size(), runs.size(), hist, longest])
			check(ls.size() == ledges and longest >= 2, "get_ledge_slabs: %d slabs (get_stones %d) in %d runs, longest %d, in %d ms" % [ls.size(), ledges, runs.size(), longest, Time.get_ticks_msec() - t1])
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
	if args.get("stone-scans", "1") != "0":
		net.load_scan_set(_rock_scan_dir())
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
			var d_up := net.get_rock_distance(pc + Vector3(0, 0.08, 0))
			var d_dn := net.get_meshed_rock_distance(pc - Vector3(0, float(pl["depth"]) + 0.08, 0)) # the floor as drawn: flowstone included
			if d_up < 0.0 and d_dn > -0.05:
				wet_ok += 1
			else:
				print("    pool at %s depth %.3f: %.3f m from the rock 8 cm over the level, %.3f under the floor" % [pc, pl["depth"], d_up, d_dn])
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
		# (Why, when it finds nothing: where the target stands against the rock, and whether the network has caves.)
		print("  cave streaming target %s: %.2f m from the rock (negative in air), %d caves, follow %s, streaming %s" % [walker.global_position, net.get_rock_distance(walker.global_position), net.get_cave_count(), net.get_node_or_null(net.follow_target) != null, net.is_streaming()])
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
		# The sea runs into the cave (CaveNetwork3D as a WorldWater producer): a 2 m swell of 12 s outside the mouth.
		if sid >= 0:
			var swell := SwellStub.new()
			swell.sea = sea_y
			swell.gen = gen
			WorldContracts.add_water_stub(swell)
			var water: WorldWater = WorldContracts.get_water()
			net.step_sea_surge(0.05)
			var sw0: Dictionary = net.get_sea_cave_water(sid)
			var pts0: PackedVector3Array = sw0["points"]
			# A point a few along, where the still water is over a metre deep if there is one. Over a beach-level
			# floor the water cannot fall below the floor in the trough: there the surge need only fill half the
			# swell's reach above the floor (the 2 m swell's crest at sea + 1).
			var dps0: Array = Array(sw0.get("depths", []))
			var ins_k := mini(3, pts0.size() - 1)
			for k in range(3, mini(pts0.size(), dps0.size())):
				if float(dps0[k]) > 1.2 and gen.get_height(pts0[k]) > sea_y + 2.0:
					ins_k = k
					break
			var inside: Vector3 = pts0[ins_k]
			var lo := INF
			var hi := -INF
			var flow_in := 0.0
			var spout := 0.0
			for i in 960: # 48 s in 0.05 s steps
				swell.t = i * 0.05
				net.step_sea_surge(0.05)
				if i >= 480:
					var hgt := water.height_at(Vector3(inside.x, sea_y - 0.5, inside.z))
					if is_finite(hgt):
						lo = minf(lo, hgt)
						hi = maxf(hi, hgt)
					flow_in = maxf(flow_in, water.flow_at(Vector3(inside.x, sea_y - 0.5, inside.z)).length())
					spout = maxf(spout, float(net.get_blowhole_spout(sid).get("speed", 0.0)))
			check(is_finite(lo) and hi - lo > minf(1.0, 0.5 * (sea_y + 1.0 - maxf(lo, sea_y - 1.0))) and absf((hi + lo) * 0.5 - sea_y) < 0.6 and flow_in > 0.3 and gen.get_height(inside) > hi, "sea caves: the sea surges into the passage under the cliff: %.1f to %.1f m (still sea %.1f m, point %d along), currents to %.1f m/s; the blowhole's air up to %.0f m/s" % [lo, hi, sea_y, ins_k, flow_in, spout])
			# A storm swell (4 m) seals the passage and drives the air in the back chamber up its blowhole.
			var bh := -1
			for se in sea_ents:
				var id2 := net.add_sea_cave(se["position"], se["out"])
				if id2 >= 0 and net.get_blowholes(id2).size() > 0:
					bh = id2
					break
			if bh < 0:
				check(true, "blowholes: none among these sea caves (counted, not required)")
			else:
				swell.amplitude = 4.0
				var top := 0.0
				var sealed := false
				for i in 600:
					swell.t = 60.0 + i * 0.05
					net.step_sea_surge(0.05)
					var sp: Dictionary = net.get_blowhole_spout(bh)
					top = maxf(top, float(sp.get("speed", 0.0)))
					sealed = sealed or bool(sp.get("sealed", false))
				check(not sealed or top > 5.0, "blowholes: a storm swell %s; air up the shaft to %.0f m/s" % ["seals the passage" if sealed else "does not seal this passage", top])
			net.sea_surge = false
			check(not is_finite(water.height_at(Vector3(inside.x, sea_y - 0.5, inside.z))), "sea surge off: no water under the cliff")
			WorldContracts.clear_overrides()
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
	var t3 := Time.get_ticks_msec()
	var wind: Array = rocks.get_wind_obstacles(Rect2(scree_at.x - 128, scree_at.z - 128, 256, 256))
	var tall := 0
	for b in wind:
		if (b as AABB).size.y >= 0.5:
			tall += 1
	check(wind.size() > 0 and tall == wind.size() and rocks.is_in_group("weather_shelter"), "wind obstacles: %d boulders standing 0.5 m or more as boxes for the weather's shelter, the field in its group (%d ms)" % [wind.size(), Time.get_ticks_msec() - t3])
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
	# (The stones round this target are young talus and rockfall, which carry no moss since their surface age gates
	# it, so the shells may be none here; the check is that none are left pending.)
	check(rocks.get_moss_pending() == 0, "moss shells: %d on the close-up stones, %d triangles, %.1f MB, none pending" % [rocks.get_moss_shell_count(), rocks.get_moss_triangle_count(), moss_bytes / 1048576.0])
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
		# Stones rest on the drawn ground: the heightfield plus the landscape's bank and cliff relief (WorldGround.relief_at).
		var on_rel := 0
		var seated := 0
		var worst_rel := 0.0
		for lay in [RockField3D.LAYER_ROCKFALL, RockField3D.LAYER_SCREE]:
			for st in rocks.get_stones(lay, Rect2(-120, -120, 240, 240)): # (the whole world's stones at once runs out of memory)
				var sp: Vector3 = st["position"]
				var hg := gen.get_height(sp)
				var rl: Vector2 = WorldContracts.get_ground().relief_at(Vector3(sp.x, hg, sp.z))
				if rl.y <= 0.0 or absf(rl.x) < 0.05:
					continue
				on_rel += 1
				var off := absf(float(st["ground"]) - (hg + rl.x))
				worst_rel = maxf(worst_rel, off)
				if off < 0.02:
					seated += 1
		check(seated == on_rel, "stones on relief-drawn ground: %d of %d seated on the drawn surface (worst %.3f m off)%s" % [seated, on_rel, worst_rel, " (none in the middle 240 m: counted, not required)" if on_rel == 0 else ""])
		# Live rockfall: a cliff block comes loose, out of the wall, falls under gravity and comes to rest on the ground.
		check(is_equal_approx(rocks.get_rockfall_trigger(), 1.0), "rockfall trigger without weather: %.2f (base rate)" % rocks.get_rockfall_trigger())
		var cliff: Array = rocks.get_stones(RockField3D.LAYER_CLIFF, Rect2(-800, -800, 1600, 1600))
		var block: Dictionary = {}
		for st in cliff:
			if int(st["family"]) == RockGenerator.FAMILY_LEDGE:
				continue # (ledge slabs are not loosed)
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
			var r := await _rockfall_run(bp, 1.0, 120, true, 180)
			check(not r.is_empty() and r["fired"] == 1 and rocks.get_broken_count() == before + 1, "trigger_rockfall: a block loosed (signal %d, broken %d), %.1f ms" % [r.get("fired", 0), rocks.get_broken_count() - before, r.get("ms", 0.0)])
			if not r.is_empty():
				check(r["drop"] > 0.5 and r["below"] == 0, "the fall: %.1f m down, %.1f m out, top speed %.1f m/s, %d pieces, none under the ground" % [r["drop"], r["travel"], r["max_speed"], r["pieces"]])
				# The dust over 5 s: a block that slides down the face first lands after the 2 s the fall is checked over.
				check(r["hits"] > 0 and r["dust"] > 0.1, "rockfall dust: %.1f kg of fines from %d impacts in 5 s" % [r["dust"], r["hits"]])
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
		await _ledge_fall(cliff)
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
	var t0 := Time.get_ticks_msec()
	# By time, not frames: the coarse cells the grains wait for are placed on workers too (cell_threads), and frames
	# without drawing pass faster than they land.
	while Time.get_ticks_msec() - t0 < 180000 and still < 10:
		await get_tree().process_frame
		frames += 1
		var n := rocks.get_ground_detail_count()
		still = still + 1 if n == last and n > 0 and rocks.get_ground_detail_pending() == 0 and rocks.get_cells_pending() == 0 else 0
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
	get_viewport().disable_3d = true # streaming cost only: software Vulkan takes seconds to draw a frame
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
	var settle_t := Time.get_ticks_msec()
	while (rocks.get_ground_detail_pending() > 0 or rocks.get_cells_pending() > 0 or settle < 30) and settle < 3000:
		await get_tree().process_frame
		ms.append(rocks.get_last_build_msec())
		dms.append(rocks.get_last_detail_msec())
		settle += 1
	rocks.streaming = false
	get_viewport().disable_3d = false
	print("walk: settled in %d frames (%d ms); %d cells, %d detail grains" % [settle, Time.get_ticks_msec() - settle_t, rocks.get_cell_count(), rocks.get_ground_detail_count()])
	var stats := func(a: Array[float]) -> String:
		var s := a.duplicate()
		s.sort()
		var total := 0.0
		for v in a:
			total += v
		return "mean %.2f ms, p95 %.2f ms, worst %.2f ms" % [total / a.size(), s[int(s.size() * 0.95)], s[-1]]
	print("walk: %.1f m in %d frames after %d warm-up (%s): streaming %s; ground detail %s; %d stones" % [
		dist, ms.size(), warm, ("detail on the main thread" if not rocks.ground_detail_threads else "detail on workers") + (", cells on the main thread" if not rocks.cell_threads else ", cells on workers"),
		stats.call(ms), stats.call(dms), rocks.get_stone_count()])


# --roll[=m/s]: a 0.3 m ball (40 kg, in world_contact) rolled 8 m across the spot from left to right of the view, then
# taken away before the still: the stones it touched (RockField3D.stones_react) lie where it left them.
func _roll() -> void:
	var speed := float(args["roll"]) if args["roll"] != "1" else 3.0
	var side := cam.global_transform.basis.x
	side.y = 0.0
	side = side.normalized()
	var ball := RigidBody3D.new()
	ball.mass = 40.0
	var cs := CollisionShape3D.new()
	var sph := SphereShape3D.new()
	sph.radius = 0.3
	cs.shape = sph
	ball.add_child(cs)
	ball.set_meta("trample_radius", 0.3)
	ball.add_to_group("world_contact")
	add_child(ball)
	var start := spot_pos - side * 4.0
	start.y = gen.get_height(start) + 0.32
	ball.global_position = start
	ball.linear_velocity = side * speed
	ball.angular_velocity = side.cross(Vector3.DOWN) * (speed / 0.3)
	get_viewport().disable_3d = true
	var most := 0
	for i in 300:
		await get_tree().physics_frame
		most = maxi(most, rocks.get_loose_count())
	var settle := 0
	while rocks.get_loose_count() > 0 and settle < 600:
		await get_tree().physics_frame
		settle += 1
	print("roll: ball went %.1f m; stones %s, at most %d loose at once, all at rest %d frames later" % [ball.global_position.distance_to(start), rocks.get_react_stats(), most, settle])
	ball.queue_free()
	await get_tree().process_frame
	get_viewport().disable_3d = false


# A contact for --tracks: a Node3D in world_contact with one collision shape (a boot's box or a tyre's cylinder on
# its side), moved by the bench; RockField3D reads its speed from its last position.
func _contact_node(shape: Shape3D, rot := Basis()) -> Node3D:
	var n := Node3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	cs.basis = rot
	n.add_child(cs)
	n.add_to_group("world_contact")
	add_child(n)
	return n


# --tracks: a walker and a two-wheeled cart cross the spot on curving paths (6 and 8 m radius bends) before the still
# (stones_react), then are taken away: the walker's separate prints and the cart's two ruts with what they pushed
# aside. The walker steps 0.7 m, each boot coming down at 1.5 m/s and standing 0.3 s; the cart rolls at 2 m/s.
func _tracks() -> void:
	var side := cam.global_transform.basis.x
	side.y = 0.0
	side = side.normalized()
	var fwd := Vector3.UP.cross(side).normalized() # away from the camera
	var g := func(p: Vector3) -> float: return gen.get_height(p)
	# Bends: a point at arc length t (m) on a circle of radius rad through start, heading along side, turning away.
	var arc := func(start: Vector3, rad: float, t: float) -> Array: # (a bend away from the camera)
		var a := t / rad
		var p := start + side * (rad * sin(a)) + fwd * (rad * (1.0 - cos(a)))
		var h := (side * cos(a) + fwd * sin(a)).normalized()
		return [p, h]
	var boot_l := _contact_node(_box(Vector3(0.1, 0.06, 0.27)))
	var boot_r := _contact_node(_box(Vector3(0.1, 0.06, 0.27)))
	var tyre := CylinderShape3D.new()
	tyre.radius = 0.32
	tyre.height = 0.2
	var wheel_l := _contact_node(tyre, Basis(Vector3.FORWARD, PI / 2))
	var wheel_r := _contact_node(tyre, Basis(Vector3.FORWARD, PI / 2))
	var walk_start := spot_pos - side * 3.0 - fwd * 0.8
	var cart_start := spot_pos - side * 3.5 + fwd * 0.6
	get_viewport().disable_3d = true
	var dt := 1.0 / 60.0
	var t := 0.0
	var most := 0
	var plants := {} # step k: where it stands (for --tracks-verbose)
	while t < 3.5:
		# The walker: step k plants at arc length 0.7 k, left and right 0.12 m off the line; a boot is in the air
		# (0.3 m up) between plants, comes down over 0.1 s and stands 0.3 s.
		for which in 2:
			var boot: Node3D = boot_l if which == 0 else boot_r
			var phase := t / 0.45 - which # a step every 0.45 s, the feet alternating
			var k := floori(phase / 2.0) * 2 + which
			var in_step := fposmod(phase, 2.0)
			var at: Array = arc.call(walk_start, 6.0, 0.7 * k)
			var lat := Vector3.UP.cross(at[1]).normalized() * (0.12 if which == 0 else -0.12)
			var p: Vector3 = at[0] + lat
			var lift := 0.3
			if in_step < 0.22:
				lift = 0.3 * (1.0 - in_step / 0.22)
			elif in_step < 0.9:
				lift = 0.0
			p.y = g.call(p) + 0.03 - 0.025 + lift # a sole set down sinks 2.5 cm (a walker's weight on gravel over sand)
			boot.global_position = p
			if lift == 0.0:
				plants[k] = p
			boot.basis = Basis.looking_at(at[1], Vector3.UP)
		# The cart: two wheels 1.4 m apart on an 8 m bend, at 2 m/s, pressing 2 cm in.
		var ca: Array = arc.call(cart_start, 8.0, 2.0 * t)
		var axle := Vector3.UP.cross(ca[1]).normalized()
		for which in 2:
			var wn: Node3D = wheel_l if which == 0 else wheel_r
			var wp: Vector3 = ca[0] + axle * (0.7 if which == 0 else -0.7)
			wp.y = g.call(wp) + 0.32 - 0.02
			wn.global_position = wp
			wn.basis = Basis.looking_at(ca[1], Vector3.UP)
		await get_tree().physics_frame
		most = maxi(most, rocks.get_loose_count())
		if args.has("tracks-verbose") and int(t / dt) % 30 == 0:
			print("tracks: t %.2f, left boot %s (%.2f over the ground), %s" % [t, boot_l.global_position, boot_l.global_position.y - g.call(boot_l.global_position), rocks.get_react_stats()])
		t += dt
	for n in [boot_l, boot_r, wheel_l, wheel_r]:
		n.queue_free()
	var settle := 0
	while rocks.get_loose_count() > 0 and settle < 600:
		await get_tree().physics_frame
		settle += 1
	print("tracks: stones %s, at most %d loose at once, all at rest %d frames later" % [rocks.get_react_stats(), most, settle])
	if args.has("tracks-verbose"):
		var gs := get_node_or_null("Beach") as GroundSurface3D
		var mid: Array = arc.call(cart_start, 8.0, 3.0)
		var wpt: Vector3 = mid[0] + Vector3.UP.cross(mid[1]).normalized() * 0.7
		for k in plants:
			var bp: Vector3 = plants[k]
			print("tracks: print %d: generator %.3f, sand %s" % [k, g.call(bp), "-" if gs == null else "%.3f" % gs.get_surface_height_at(bp)])
		for o in [-0.3, -0.15, 0.0, 0.15, 0.3]:
			var q: Vector3 = wpt + Vector3.UP.cross(mid[1]).normalized() * o
			print("tracks: across %.2f: generator %.3f, sand %s" % [o, g.call(q), "-" if gs == null else "%.3f (ground %.3f)" % [gs.get_surface_height_at(q), gs.get_ground_height_at(q)]])
	await get_tree().process_frame
	get_viewport().disable_3d = false


func _box(size: Vector3) -> BoxShape3D:
	var b := BoxShape3D.new()
	b.size = size
	return b


func _render_later() -> void:
	await get_tree().process_frame
	if args.has("walk") and rocks:
		await _walk()
	if args.has("roll") and rocks:
		await _roll()
	if args.has("tracks") and rocks:
		await _tracks()
	while breaking:
		await get_tree().process_frame
	if args.has("hide-stream-water"):
		_hide_class(self, "PhysXRiver3D") # --hide-stream-water: the cave streams' water hidden (what is under it)
	# The beach's sand (grain, ripples, wetness) is drawn by its GroundSurface3D once the ripples are built.
	var gs := get_node_or_null("Beach") as GroundSurface3D
	var rw := 0
	while gs and gs.wind_ripples and not gs.is_ripples_ready() and rw < 1200:
		await get_tree().process_frame
		rw += 1
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
		if args.has("wait-held"):
			# --wait-held=N: up to N more frames while the path tracer holds budget-limited geometry back.
			var held_wait := 0
			while held_wait < int(args["wait-held"]) and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_DYNAMIC_HELD) > 0:
				await RenderingServer.frame_post_draw
				held_wait += 1
				if held_wait % 20 == 0:
					print("  held %d, TLAS %d" % [get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_DYNAMIC_HELD), get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_TLAS_INSTANCES)])
			print("rt held cleared after %d more frames" % held_wait)
	if args.has("fov"):
		get_viewport().get_camera_3d().fov = float(args["fov"]) # --fov=deg: vertical field of view (27: a 50 mm lens)
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
	if rocks:
		var fs: Dictionary = rocks.get_ground_detail_tile_stats()
		print("far thinning: %d groups thinned, %d widened (mean %.2fx)" % [fs.get("far_thinned_groups", 0), fs.get("far_widened_groups", 0), fs.get("far_widen_mean", 1.0)])
	if not raster:
		# The ray-tracing cost of the ground detail (the PC cost bench for "more stones for the same cost": run with and
		# without --no-ground-detail, path traced, and compare these and the frame time).
		var vp := get_viewport()
		print("rt: %d TLAS instances, %d triangles built, %d ground detail grains" % [vp.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_TLAS_INSTANCES), vp.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_TRIANGLES_BUILT), rocks.get_ground_detail_count() if rocks else 0])
		var ri := func(what: int) -> int: return vp.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, what)
		print("rt: captured %d, rebakes %d, held %d, budget limited %d, pending %d" % [ri.call(Viewport.RENDER_INFO_RT_CAPTURED_MESHES), ri.call(Viewport.RENDER_INFO_RT_DYNAMIC_REBAKES), ri.call(Viewport.RENDER_INFO_RT_DYNAMIC_HELD), ri.call(Viewport.RENDER_INFO_RT_DYNAMIC_BUDGET_LIMITED), ri.call(Viewport.RENDER_INFO_RT_PENDING_SURFACES)])
	if rocks and rocks.ground_detail:
		print("tiles: %s" % rocks.get_ground_detail_tile_stats())
		# The grains on the terrain's drawn relief (Landscape3D bank and cliff relief): each one's ground against the
		# heightfield plus WorldGround.relief_at.
		var on_relief := 0
		var on_it := 0
		var worst := ""
		var off := 0.0
		var wg: WorldGround = WorldContracts.get_ground()
		for st in rocks.get_stones(RockField3D.LAYER_DETAIL_PEBBLE, Rect2(spot_pos.x - 30, spot_pos.z - 30, 60, 60)):
			var pos: Vector3 = st["position"]
			var r: Vector2 = wg.relief_at(pos)
			if absf(r.x) > 0.02:
				on_relief += 1
				var o := absf(float(st["ground"]) - (gen.get_height(pos) + r.x))
				if o > off:
					off = o
					worst = "at %s: relief %.3f, stone ground %.3f, heightfield %.3f" % [pos, r.x, float(st["ground"]), gen.get_height(pos)]
				if o < 0.05:
					on_it += 1
		print("relief: %d pebble grains on drawn relief (%d within 5 cm), ground off it by %.3f m at most %s" % [on_relief, on_it, off, worst])
		if rocks.rain_pedestals and args.has("rain-climate"):
			# The grains on rain splash pedestals round the spot: how many, and their columns' mean and tallest.
			var ped := 0
			var all := 0
			var tall := 0.0
			var sum := 0.0
			for st in rocks.get_stones(RockField3D.LAYER_DETAIL_PEBBLE, Rect2(spot_pos.x - 3, spot_pos.z - 3, 6, 6)):
				all += 1
				var pv := float(st.get("pedestal", 0.0))
				if pv > 0.0:
					ped += 1
					sum += pv
					tall = maxf(tall, pv)
			print("pedestals: %d of %d pebble grains within 3 m on pedestals, mean %.3f m, tallest %.3f m" % [ped, all, sum / maxf(ped, 1), tall])
	if rocks and has_node("SnowCover3D"):
		print("snow burial: %d detail stones under the snow so far, %.3f m of snow at the spot" % [int(rocks.get_ground_detail_tile_stats().get("snow_buried", 0)), get_node("SnowCover3D").get_depth_at(spot_pos)])
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
	if rocks and rocks.has_method("get_ground_detail_cover_texture"):
		var ct: Texture2D = rocks.get_ground_detail_cover_texture(0)
		print("ground cover: %s" % ("maps ready" if ct else "no maps"))
		if ct and args.has("save-cover"):
			ct.get_image().save_png(out.get_basename() + "_cover.png")
		if ct and args.has("cover-check"):
			# Each class's cover as placed (sum of footprints over 2 m squares) against the near map's.
			var img := ct.get_image()
			var cp := get_viewport().get_camera_3d().global_position
			var org := Vector2(floor(cp.x) - 128.0, floor(cp.z) - 128.0)
			for k in 3:
				var layer: int = RockField3D.LAYER_DETAIL_PEBBLE + k
				var rad: float = rocks.get_layer_radius(layer) * float(args.get("cover-reach", "0.7"))
				var placed := 0.0
				var mapped := 0.0
				var n := 0
				for gz in range(-int(rad), int(rad), 2):
					for gx in range(-int(rad), int(rad), 2):
						var rc := Rect2(cp.x + gx, cp.z + gz, 2, 2)
						var a := 0.0
						for st in rocks.get_stones(layer, rc):
							a += PI * pow(float(st["footprint"]), 2.0)
						var px := img.get_pixel(int(rc.get_center().x - org.x), int(rc.get_center().y - org.y))
						var m: float = [px.r, px.g, px.b][k] * 0.5
						placed += a / 4.0
						mapped += m
						n += 1
						if args.has("cover-verbose") and (a > 0.0 or m > 0.0):
							print("  class %d at %s: placed %.3f map %.3f" % [k, rc.get_center(), a / 4.0, m])
				print("cover class %d: placed %.4f, map %.4f (mean over %d squares within %.0f m)" % [k, placed / max(n, 1), mapped / max(n, 1), n, rad])
			# The ground contract (WorldGround.coarse_cover_at) reads the same map on the CPU (texel centres of the map
			# as built: it follows the camera only once it has moved an eighth of the map).
			var mr: Rect2 = rocks.get_ground_detail_cover_rect(0)
			var worst := 0.0
			var cov_sum := 0.0
			var cov_n := 0
			for tz in range(8, 248, 3):
				for tx in range(8, 248, 3):
					var q := mr.position + Vector2(tx + 0.5, tz + 0.5)
					var px := img.get_pixel(tx, tz)
					var cv: Vector3 = WorldContracts.get_ground().coarse_cover_at(Vector3(q.x, 0, q.y))
					var dd := absf(cv.x - px.r * 0.5) + absf(cv.y - px.g * 0.5) + absf(cv.z - px.b * 0.5)
					if dd > 0.005 and args.has("cover-verbose"):
						print("  contract %s map %s at %s" % [cv, Vector3(px.r, px.g, px.b) * 0.5, q])
					worst = max(worst, dd)
					cov_sum += cv.x + cv.y
					cov_n += 1
			print("%s cover contract: WorldGround.coarse_cover_at matches the near map within %.4f (pebble + gravel mean %.3f)" % ["PASS" if worst < 0.005 else "FAIL", worst, cov_sum / max(cov_n, 1)])
	for c in get_children():
		if c is CaveNetwork3D and (c as CaveNetwork3D).get_cave_count() > 0 and (c as CaveNetwork3D).is_sea_cave(0):
			var sw: Node = (c as CaveNetwork3D).get_sea_water_node()
			var w: Dictionary = (c as CaveNetwork3D).get_sea_cave_water(0)
			var pts: PackedVector3Array = w.get("points", PackedVector3Array())
			var al: PackedFloat32Array = w.get("along", PackedFloat32Array())
			if sw and args.has("hide-cave-surface"):
				(sw as Node3D).visible = false
			if sw:
				# The short waves take about a minute to run the passage and back: warm them up before the still.
				if args.has("cave-waves"):
					sw.set("wave_inflow", float(args["cave-waves"]))
				if args.has("no-cave-waves"):
					sw.set("wavelets", false)
				sw.warm_up(60.0)
				# The frame on screen is from before the warm-up: draw the warmed surface before saving.
				for f in (24 if args.has("taa") else 3):
					await RenderingServer.frame_post_draw
				print("sea cave waves: sea rms at the mouth %.3f m, short waves rms %.3f m over the cave" % [w.get("wave_rms", 0.0), sw.get_wave_rms(0)])
				var hws: PackedFloat32Array = w.get("half_widths", PackedFloat32Array())
				for k in range(0, pts.size(), 4):
					print("  station %d at %.1f m: %s, half width %.2f" % [k, al[k] if k < al.size() else 0.0, str(pts[k]), hws[k] if k < hws.size() else 0.0])
			print("sea cave water: %d stations over %.1f m, arch from %.1f m, level at the mouth %s, %d drawn, %d sea clip capsules" % [pts.size(), al[al.size() - 1] if al.size() > 0 else 0.0, w.get("covered_from", 0.0), str(pts[0]) if pts.size() > 0 else "-", sw.get_ribbon_count() if sw else -1, (c as CaveNetwork3D).get_water_clip_capsules().size() / 8])
	if rocks:
		print("rocks rt: %s (rt_instanced_cells %s)" % [str(rocks.get_rt_stats()), str(rocks.rt_instanced_cells)])
		print("frame primitives: %d in %d draw calls" % [RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)])
		var vp := get_viewport()
		print("  visible pass %d primitives, %d objects; shadow passes %d primitives, %d objects" % [vp.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME), vp.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_OBJECTS_IN_FRAME), vp.get_render_info(Viewport.RENDER_INFO_TYPE_SHADOW, Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME), vp.get_render_info(Viewport.RENDER_INFO_TYPE_SHADOW, Viewport.RENDER_INFO_OBJECTS_IN_FRAME)])
		if args.has("prim-report"):
			_prim_report(rocks)
	if args.has("mem-report"):
		# --mem-report: video memory at the saved frame; with the engine's --extra-gpu-memory-tracking also the
		# driver's memory by Vulkan object type (buffers, images, acceleration structures).
		print("video memory: total %d MB, textures %d MB, buffers %d MB" % [RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED) >> 20,
				RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED) >> 20, RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED) >> 20])
		var rd := RenderingServer.get_rendering_device()
		if rd:
			print("acceleration structures: %d MB" % [rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) >> 20])
			print(rd.get_driver_and_device_memory_report())
	if args.has("pick"):
		# --pick=x0,y0,x1,y1: the cliff layer's stones whose centre the frame shows in that pixel rectangle.
		var pk: PackedStringArray = str(args["pick"]).split(",")
		var pr := Rect2(float(pk[0]), float(pk[1]), float(pk[2]) - float(pk[0]), float(pk[3]) - float(pk[1]))
		var pcam := get_viewport().get_camera_3d()
		for st in rocks.get_stones(RockField3D.LAYER_CLIFF, Rect2(spot_pos.x - 120, spot_pos.z - 120, 240, 240)):
			var sp: Vector3 = st["position"]
			if not pcam.is_position_behind(sp) and pr.has_point(pcam.unproject_position(sp)):
				print("pick: ", st)
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	get_tree().quit(0)


# The beach as the landscape bench draws it (its _build_prints): a GroundSurface3D over the TerrainMaterial3D with a
# sand type map over the beach band, so the sand has its grain, ripples, wetness and prints, not the beach layer alone.
# --no-ground-surface leaves it out (a before frame).
func _beach_surface(tm: TerrainMaterial3D) -> void:
	var gs := GroundSurface3D.new()
	gs.name = "Beach"
	gs.ground_type = GroundSurface3D.GROUND_SOIL
	gs.material = tm
	# Each bench run starts from fresh sand: kept ruts from earlier runs would show up in the before frames.
	gs.keep_ruts = false
	add_child(gs)
	gs.height_source = gs.get_path_to(gen)
	var n := 512
	var img := Image.create(n, n, false, Image.FORMAT_RGB8)
	var beach := tm.beach_height
	var half := WORLD * 0.5
	for j in n:
		for i in n:
			var x := -half + (i + 0.5) * WORLD / n
			var z := -half + (j + 0.5) * WORLD / n
			var h := gen.get_height(Vector3(x, 0, z))
			var sand := 1.0 - smoothstep(beach * 0.6, beach * 1.4, h - gen.sea_level)
			if h < gen.sea_level - 1.0:
				sand = 0.0
			img.set_pixel(i, j, Color(sand, 0, 0))
	gs.type_map = img
	gs.type_map_rect = Rect2(-half, -half, WORLD, WORLD)
	# --gs-prop=name:value[,name:value...]: any of the beach GroundSurface3D's properties (A/B one layer).
	for kv in str(args.get("gs-prop", "")).split(",", false):
		var nv := kv.split(":")
		gs.set(nv[0], str_to_var(nv[1]))


func _hide_class(n: Node, cls: String) -> void:
	for c in n.get_children(true):
		if c.is_class(cls) and c is Node3D:
			(c as Node3D).visible = false
		_hide_class(c, cls)


# --world --height-check: the generator must give the same heights on every platform and thread count (it does
# its own transcendental math, terrain_dmath.h, and breaks sort ties by index). Prints the heights at fixed
# points and a SHA-256 of the whole height image, compares them with the reference below, prints PASS/FAIL, quits.
# Run with the default world (seed 5, 2048 m); a deliberate change to the generator updates the reference.
const HEIGHT_REF := {
	"points": ["225.946442", "406.352905", "673.358887", "-1.532555", "4.541926", "203.910446", "305.330017", "-4.236583"],
	"sha256": "01517e43cc322726efd52874413eeef0636f9f994331c46f185f94289cc230db",
}


func _height_check() -> void:
	var pts := [Vector3(0, 0, 0), Vector3(-600, 0, -600), Vector3(600, 0, -600), Vector3(-600, 0, 600),
			Vector3(600, 0, 600), Vector3(-250, 0, 120), Vector3(333, 0, -77), Vector3(10, 0, 900)]
	var got: Array[String] = []
	for p in pts:
		got.append("%.6f" % gen.get_height(p))
	var img: Image = gen.get_height_image()
	var hc := HashingContext.new()
	hc.start(HashingContext.HASH_SHA256)
	hc.update(img.get_data())
	var sha := hc.finish().hex_encode()
	print("height check: points %s" % ", ".join(got))
	print("height check: height image %dx%d sha256 %s" % [img.get_width(), img.get_height(), sha])
	var ok := WORLD == 2048.0 and int(args.get("seed", "5")) == 5 and not args.has("gen-prop")
	if not ok:
		print("height check: not the reference world (seed 5, 2048 m, no --gen-prop); nothing compared")
	else:
		var same: bool = ", ".join(got) == ", ".join(HEIGHT_REF["points"]) and sha == HEIGHT_REF["sha256"]
		print("height check: %s" % ("PASS" if same else "FAIL (reference %s, sha256 %s)" % [", ".join(HEIGHT_REF["points"]), HEIGHT_REF["sha256"]]))
	get_tree().quit()


# --repetition: how often a stone stands next to its twin (NO VISIBLE REPETITION, 18 Nov). For each layer in the rect:
# the share of stones whose nearest neighbour (within 4 footprints) is the same shape (library and variant), and the
# share whose neighbour is also within 15 % of its size and 25 degrees of its yaw and tilt (a twin the eye catches).
func _repetition_audit(p_rect: Rect2) -> void:
	for lay in RockField3D.LAYER_MAX:
		var sts: Array = rocks.get_stones(lay, p_rect)
		if sts.size() < 2:
			continue
		var grid := {}
		for i in sts.size():
			var p: Vector3 = sts[i]["position"]
			var key := Vector2i(floori(p.x / 4.0), floori(p.z / 4.0))
			if not grid.has(key):
				grid[key] = []
			grid[key].append(i)
		var same := 0
		var twins := 0
		var with_nb := 0
		for i in sts.size():
			var a: Dictionary = sts[i]
			var pa: Vector3 = a["position"]
			var reach := 4.0 * maxf(float(a["footprint"]), 0.05)
			var best := -1
			var best_d := INF
			var ka := Vector2i(floori(pa.x / 4.0), floori(pa.z / 4.0))
			var rc := int(ceil(reach / 4.0))
			for dz in range(-rc, rc + 1):
				for dx in range(-rc, rc + 1):
					for j in grid.get(ka + Vector2i(dx, dz), []):
						if j == i:
							continue
						var dd := pa.distance_to(sts[j]["position"])
						if dd < reach and dd < best_d:
							best_d = dd
							best = j
			if best < 0:
				continue
			with_nb += 1
			var b: Dictionary = sts[best]
			if a["rock_type"] != b["rock_type"] or a["family"] != b["family"] or a["bin"] != b["bin"] or a["variant"] != b["variant"]:
				continue
			same += 1
			var ba: Basis = (a["basis"] as Basis).orthonormalized()
			var bb: Basis = (b["basis"] as Basis).orthonormalized()
			var rel := ba.inverse() * bb
			var ang := rad_to_deg(rel.get_rotation_quaternion().get_angle())
			if absf(float(a["scale"]) / float(b["scale"]) - 1.0) < 0.15 and ang < 25.0:
				twins += 1
		print("repetition: layer %d, %d stones, %d with a neighbour; same shape %.1f %%, twins %.1f %%" % [lay, sts.size(), with_nb, 100.0 * same / maxf(with_nb, 1), 100.0 * twins / maxf(with_nb, 1)])


# --prim-report: the rock MultiMeshInstance3D nodes ranked by the triangles they draw, with the LOD each one gets by
# the renderer's rule (nearest point of the instance's AABB to the camera, model scale * lod_bias, 1 px threshold).
func _prim_report(rocks: Node) -> void:
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return
	var cp := cam.global_position
	var th: float = get_viewport().mesh_lod_threshold / float(get_viewport().size.x)
	var lodmul: float = 2.0 / cam.get_camera_projection().x.x
	var rows := []
	var lod_cache := {}
	var stack: Array = [rocks]
	while stack.size() > 0:
		var n: Node = stack.pop_back()
		stack.append_array(n.get_children(true))
		if not (n is MultiMeshInstance3D) or not (n as MultiMeshInstance3D).is_visible_in_tree():
			continue
		var mi := n as MultiMeshInstance3D
		var mm := mi.multimesh
		if mm == null or mm.mesh == null:
			continue
		var cnt := mm.visible_instance_count if mm.visible_instance_count >= 0 else mm.instance_count
		var ab := mi.get_aabb()
		ab = mi.global_transform * ab
		var d := Vector3.ZERO.max(ab.position - cp).max(cp - ab.end).length()
		var tris := 0
		var lods := ""
		var scale: float = mi.global_transform.basis.get_scale().abs().x * mi.lod_bias
		for si in mm.mesh.get_surface_count():
			var key := str(mm.mesh.get_rid()) + ":" + str(si)
			if not lod_cache.has(key):
				var sd: Dictionary = RenderingServer.mesh_get_surface(mm.mesh.get_rid(), si)
				var ls := []
				for l in sd.get("lods", []):
					ls.append([l["edge_length"], (l["index_data"] as PackedByteArray).size() / (4 if sd.get("vertex_count", 0) > 65535 else 2) / 3])
				lod_cache[key] = [sd.get("index_count", 0) / 3, ls]
			var c: Array = lod_cache[key]
			var t: int = c[0]
			var li := 0
			for k in (c[1] as Array).size():
				var l: Array = c[1][k]
				if l[0] * scale / max(d * lodmul, 0.0001) > th:
					break
				t = l[1]
				li = k + 1
			tris += t
			lods += "%d/%d " % [li, (c[1] as Array).size()]
		rows.append([tris * cnt, cnt, tris, d, lods, str(mi.name), ab.size])
	rows.sort_custom(func(a, b): return a[0] > b[0])
	var tot := 0
	for r in rows:
		tot += r[0]
	print("prim report: %d rock instances nodes, %d triangles at their LODs (all, not culled)" % [rows.size(), tot])
	for i in min(20, rows.size()):
		var r: Array = rows[i]
		print("  %12d = %6d x %7d tris, dist %.1f m, lod %s, %s, aabb %s" % r)

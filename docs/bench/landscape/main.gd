extends Node3D
# Landscape integration bench: one generated world from mountain to sea, every system on it at once, driven by one
# weather state. TerrainGenerator3D makes the mountains, rivers, coast, seabed and lakes and builds their water
# (PhysXWaterSurface3D sea with shore waves, one surface per lake, PhysXRiver3D rivers ending on them) and
# waterfalls where rivers cross hard rock bands. WeatherMap3D storm with WeatherSkyMaterial clouds, wetness, SnowCover3D and
# SnowFall3D on the peaks, VegetationField3D forest (stand-in meshes) on slope, water and geology rules, GrassField3D
# near the camera. Each link between systems is checked: PASS, FAIL, or GAP for a missing or hand-made join.
#
# godot --path docs/bench/landscape --rendering-driver vulkan -- [options]
#   --test            run the link checks, print PASS/FAIL/GAP, quit
#   --out=path.png    render a view (path traced unless --raster)
#   --raster          raster only
#   --spp=N           path tracing samples (default 16)
#   --boulders=D     rockfall blocks below cliffs at density D (default 1, 0 none), grey stand-ins
#   --far-blend [--far-start=M]   TerrainMaterial3D's far blend over a test plant cover map
#   --lake-pick=high  the lake views and checks use the highest lake (default the biggest)
#   --cam=x,y,z --look=x,y,z  a fixed camera (a negative y: that height above the ground)
#   --view=wide|lake|lakewater|valley|fall|beach|peak|treeline|meadow   camera (default wide; lakewater: low over the lake;
#                         valley: from behind the high lake down to the clearing; treeline: the clearing's edge)
#   --strike[=1] [--strike-x= --strike-z=]   force a lightning strike just before the frame is saved
#   --ground=terrain|standard   ground material: TerrainMaterial3D (default) or the vertex-coloured StandardMaterial3D
#   --falls           print every waterfall the rivers found (lip, landing, height)
#   --probe           print coast profiles, lakes and rivers from the generator
#   --weather=storm|clear         weather state (default storm); --wind=m/s overrides its wind speed
#   --snow=climate|none   the winter baked by SnowClimate (default), or a snow-free ground for stills
#   --month=N --time=H    0-based month of the rivers and the SkyClock date (default 4, 15 May) and local time (16.5)
#   --day=N               day of the month (default 15)
#   --cloud-shadow=0 --sun-shadow=0   no cloud shadow on the sun; no raster sun shadow map
#   --sun-off             no sun at all: what the sky alone lights
#   --sky-occlusion=0     ground ambient light without the hills' sky occlusion; --sky-light=0 an even sky for it
#   --water-param=name:value[;...]   set sea shader uniforms (floats, true/false, colours as r,g,b,a)
#   --prints=0            no GroundSurface3D (footprints) node
#   --layout=range|valley range: the peaks to the sea (default); valley: a cirque with a high lake (--lake-x/z,
#                         --lake-depth) above a valley floor to the sea (needs floodplain deposition, in work)
#   --map=path.png        save a top-down map of the generated land (hillshade, water, rivers, falls) and quit
#   --mesh=landscape|grid ground: Landscape3D, 1 m with LODs (default), or the 4 m grid mesh; --lod-error=px (2)
#   --atmosphere=scattering|physical   sky air: Hillaire 2020 multiple scattering (default) or the older PhysicalSky model
#   --clip=N              with --out, N frames in a row (out_0000.png...), for clips; run with --fixed-fps 24
#   --checks              with --out, run the link checks before the picture (they change the scene)

const STEP := 4.0 # metres between ground vertices
const X0 := -2048.0
const Z0 := -2048.0
const NX := 1025 # x from -2048 to 2048
const NZ := 1281 # z from -2048 to 3072 (the last 1 km is sea)
const GEN_SIZE := 4096.0

var args := {}
var gen: TerrainGenerator3D
var heights := PackedFloat32Array()
var ground_body: StaticBody3D
var ground_mesh: MeshInstance3D
var landscape: Landscape3D
var ground_mat: Material
var ocean: PhysXWaterSurface3D
var lake: PhysXWaterSurface3D
var lakes: Array = []
var water_nodes: Array = []
var lake_center := Vector3.ZERO
var lake_axes := Vector2(140, 90)
var lake_dir := Vector2(0, 1)
var lake_level := 0.0
var fall_point := Vector3.ZERO
var fall_height := 30.0 # the tallest generator fall (m), framing its view
var fall_dir := Vector2(0, 1)
var gen_falls: Array = []
var contact_falls: Array = []
var climate: ClimateRecipe
var rivers: Array = []
var weather: WeatherMap3D
var sky_mat: WeatherSkyMaterial
var snow: SnowCover3D
var snowfall: SnowFall3D
var cornices: SnowCornice3D
var cornice_climate_max := 0.0 # the baked winter's largest cornice (m): 0 where the crests hold too little snow
var lightning: WeatherLightning3D
var probe: Node3D
var data_map: TerrainDataMap3D
var veg: VegetationField3D
var grass: GrassField3D
var sward: Array = [] # the grass layers (tall, undercoat, turf)
var cover: VegetationField3D # meadow herbs and waterside plants near the camera
var prints: GroundSurface3D
var prints_probe: Node3D
var sun: DirectionalLight3D
var boulder_spot := Vector3.ZERO
var cover_tex: ImageTexture
var clock: SkyClock
var cam: Camera3D
var env: Environment
var results: Array = []
var t_start := 0


class Ground extends Node3D:
	# Height source for the data map and the snowpack: bilinear on the bench's grid.
	var bench
	func get_height(p: Vector3) -> float:
		return bench.height_at(p.x, p.z)
	func base_height_at(p: Vector2) -> float:
		return bench.height_at(p.x, p.y)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	if args.get("layout", "range") == "valley":
		# The valley's settings (any can be overridden): a still floor where the rivers drop their load into a flat
		# alluvial plain (Yuan et al. 2019), no general uplift, the high block lower in the middle to gather a trunk
		# river, smaller rivers kept (the mountain streams), lakes only from the high lake marker.
		var vd := {"deposition": "0.5", "base-uplift": "0", "floor-uplift": "0", "funnel": "0.7", "min-area": "500000", "lakes": "0"}
		if not args.has("valley-classic"):
			# The showcase valley (terrain's settings, 11 Nov): the main river's catchment reaches past the map
			# (60 km² flowing in), a hard-rock bar across the cirque holds a glacial lake at 450 m, and its outflow
			# drops about 340 m off the bar into a pool. --valley-classic keeps the earlier valley without them.
			vd.merge({"no-lake": "1", "inflow": "60", "inflow-erosion": "0.3", "bar": "-300,-1650,350,-1500,1000,-1650",
					"bar-level": "450", "bowl": "350,-1800,650,0.1", "glacial": "1", "ela": "450", "glacial-erosion": "10",
					"lake-pick": "high"})
			if args.has("auto-lake"):
				# No hand-made bowl or bar: the ice finds its own basins and bars in rock of varying quality.
				for k2 in ["bar", "bar-level", "bowl"]:
					vd.erase(k2)
				vd["rock-var"] = "1.0"
				# A slightly lower snowline than the marked valley: cirques in every side valley.
				vd["ela"] = "400"
				vd["glacial-erosion"] = "5"
				# The main river's glacier comes in from beyond the map with it (0.03 m/yr over its 60 km²).
				vd["ice-inflow"] = "0.03"
				# Its margins stood still for 3000 years at the end and left moraines.
				vd["moraine-age"] = "3000"
		for k in vd:
			if not args.has(k):
				args[k] = vd[k]
	t_start = Time.get_ticks_msec()
	_generate()
	_lake_and_fall()
	if args.has("map"):
		_save_map(args["map"])
		get_tree().quit(0)
		return
	_build_ground()
	await get_tree().physics_frame
	await get_tree().physics_frame
	_build_weather()
	_weather_settings()
	_build_water()
	await get_tree().physics_frame
	await get_tree().physics_frame
	for r in rivers:
		r.rebuild()
	_build_vegetation()
	if args.get("prints", "1") != "0":
		_build_prints()
	_build_camera()
	if args.get("plants", "1") != "0":
		_build_cover()
	if args.has("far-cover") and veg != null:
		# The world field publishes terrain_cover_map from its rules (crowns, far plant colours, the sward), built
		# when it places: placed again here once the grass is set up.
		var tc := Time.get_ticks_msec()
		veg.cover_grass = veg.get_path_to(grass)
		veg.cover_map_enabled = true
		veg.populate()
		var ct: Texture2D = veg.get_cover_texture()
		_log("far cover map %s in %d ms (forest placed again)" % [str(ct.get_size()) if ct else "missing", Time.get_ticks_msec() - tc])
	_run_weather()
	if veg != null and veg.streaming:
		# A streamed forest settles round the camera; the views on its trees are framed again on what it placed.
		await _forest_settle()
		if args.get("view", "wide") in ["forestfloor", "woodedge"]:
			cam.queue_free()
			_build_camera()
			cam.make_current()
			await _forest_settle()
	_log("setup done in %d ms" % (Time.get_ticks_msec() - t_start))
	if args.has("sun-off"):
		sun.visible = false # an A/B frame: what the sky alone lights
	if args.has("water-param") and ocean != null and ocean.get_water_material() != null:
		# --water-param=name:value[;name:value]: a sea shader uniform for an A/B frame (floats; bools as true/false).
		for kv in String(args["water-param"]).split(";"):
			var pv := kv.split(":")
			var val: Variant = (pv[1] == "true") if pv[1] in ["true", "false"] else float(pv[1])
			if "," in pv[1]:
				var c := pv[1].split_floats(",")
				val = Color(c[0], c[1], c[2], c[3])
			ocean.get_water_material().set_shader_parameter(pv[0], val)
			_log("sea %s = %s" % [pv[0], val])
	for i in 30:
		await get_tree().process_frame
	# The checks move probes, press prints and step the weather: a picture only runs them when asked (--checks),
	# and under path tracing each of their frames is a full traced frame.
	if args.has("test") or args.has("checks"):
		await _checks()
	if args.has("test"):
		get_tree().quit(0)
		return
	# The streamed ground cover settles round the camera before the picture.
	var sw := 0
	get_viewport().disable_3d = true # placing only: the frames draw nothing
	while cover != null and not cover.is_stream_idle() and sw < 1200:
		await get_tree().process_frame
		sw += 1
	get_viewport().disable_3d = false
	if cover != null:
		_log("cover: %d plants in %d tiles after %d frames, %s" % [cover.get_plant_count(), cover.get_stream_tile_count(), sw, str(cover.get_stream_stats())])
	# The snowpack updates on a worker thread: the picture waits for the last update to land.
	var waited := 0
	while snow != null and snow.has_method("is_updating") and snow.is_updating() and waited < 600:
		await get_tree().process_frame
		waited += 1
	if landscape != null:
		# Landscape3D streams heights coarse first: load every chunk exactly before the picture.
		landscape.load_all_heights()
		landscape.update_lods_from_camera()
	var frames := int(args.get("frames", "6" if args.has("raster") else "40"))
	for i in frames:
		await get_tree().process_frame
	if env.pathtracing_enabled:
		var w := 0
		var can_ask := RenderingServer.has_method("is_pathtracing_ready")
		while can_ask and w < 400 and not RenderingServer.call("is_pathtracing_ready"):
			await get_tree().process_frame
			w += 1
		for i in int(args.get("spp-frames", "4")):
			await get_tree().process_frame
	if args.has("strike"):
		lightning.strike(Vector3(float(args.get("strike-x", "-300")), 0, float(args.get("strike-z", "-400"))), true)
		for i in int(args.get("strike-frames", "2")):
			await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var out: String = args.get("out", "")
	if args.has("clip") and out != "":
		# --clip=N: N frames in a row to out with a frame number before its extension (run with --fixed-fps 24 so
		# the wind and the shaders' TIME step a 24th of a second each frame however slow the frame is).
		for i in int(args["clip"]):
			get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%04d.png" % i)
			await RenderingServer.frame_post_draw
		_log("saved %s frames to %s_####.png" % [args["clip"], out.get_basename()])
		get_tree().quit(0)
		return
	if out != "":
		get_viewport().get_texture().get_image().save_png(out)
		_log("saved " + out)
		if gen != null:
			_log("sky light update %.1f ms" % gen.get_last_sky_light_msec())
		if args.has("geo-dump"):
			# Loose cover on the generator's geology as a picture: till red, scree green.
			var gi: Image = gen.get_geology_image()
			var tm := Image.create(gi.get_width(), gi.get_height(), false, Image.FORMAT_RGB8)
			var nt := 0
			for y in gi.get_height():
				for x in gi.get_width():
					var b := gi.get_pixel(x, y).b
					if b < -0.05: nt += 1
					tm.set_pixel(x, y, Color(clampf(-b, 0, 1), clampf(b, 0, 1), 0))
			tm.save_png(args.get("geo-dump"))
			_log("geo %dx%d till cells %d" % [gi.get_width(), gi.get_height(), nt])
		if args.has("surface-dump"):
			# The water surface each river solved along its course, against the ground (dry stretches show depth 0).
			var rv = rivers[int(args.get("surface-dump", "0"))]
			var cv: Curve3D = rv.curve
			for k in range(0, cv.point_count, 2):
				var p: Vector3 = rv.global_transform * cv.get_point_position(k)
				_log("  surf %d (%.0f, %.0f) ground %.2f surface %.2f depth %.2f" % [k, p.x, p.z, gen.get_height(p), rv.sample_height(p), rv.sample_depth(p)])
	get_tree().quit(0)


# Broad-leaved trees of the forest field (any species, stand-ins or generated): crowns of 2 m and more where the
# stands are not conifer.
func _broadleaf_trees() -> PackedVector3Array:
	var out := PackedVector3Array()
	for i in veg.biome.species.size():
		var sp := veg.biome.species[i] as PlantSpecies
		if sp == null or sp.canopy_radius < 2.0:
			continue
		for tp in veg.get_placements(i):
			if veg.get_conifer_at(tp) < 0.3:
				out.push_back(tp)
	return out


func _forest_settle() -> void:
	var n := 0
	# Nothing is drawn while it places (a frame of a generated forest takes seconds on a CPU renderer), and each
	# frame may place for longer than in play.
	get_viewport().disable_3d = true
	var budget: float = veg.stream_budget_msec
	veg.stream_budget_msec = 500.0
	for i in 3:
		await get_tree().process_frame
	while not veg.is_stream_idle() and n < 3000:
		await get_tree().process_frame
		n += 1
	veg.stream_budget_msec = budget
	get_viewport().disable_3d = false
	_log("forest: %d plants in %d tiles after %d frames" % [veg.get_plant_count(), veg.get_stream_tile_count(), n])


func _log(s: String) -> void:
	print("[landscape] ", s)


func check(ok: bool, what: String, gap := false) -> void:
	var tag := "PASS" if ok else ("GAP " if gap else "FAIL")
	results.append([tag, what])
	print("[landscape] ", tag, "  ", what)


# --- Terrain -----------------------------------------------------------------------------------------------------

func _generate() -> void:
	var layout: String = args.get("layout", "range")
	var range_nodes: Array[Node3D] = []
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 1.0 / 1500.0
	noise.fractal_octaves = 3
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	var lake_marker: Node3D = null
	if layout == "range":
		# A range across the north with a spur, uplift fading out towards a coastal plain in the south.
		var rng := _range_path("Range", [Vector3(-2000, 0, -900), Vector3(-200, 0, -1300), Vector3(2000, 0, -1000)], 0.0, 1.0)
		var rc := Curve3D.new()
		rc.add_point(Vector3(-2000, 0, -900), Vector3.ZERO, Vector3(600, 0, -300))
		rc.add_point(Vector3(-200, 0, -1300), Vector3(-500, 0, 0), Vector3(500, 0, 0))
		rc.add_point(Vector3(2000, 0, -1000), Vector3(-600, 0, -200))
		rng.curve = rc
		range_nodes.append(rng)
		range_nodes.append(_range_path("Spur", [Vector3(700, 0, -1200), Vector3(1100, 0, 200)], 450.0, 0.6))
		for y in 64:
			var z := Z0 + (y + 0.5) / 64.0 * GEN_SIZE
			var s := 0.15 + 0.85 * (1.0 - smoothstep(-400.0, 1100.0, z))
			for x in 64:
				up.set_pixel(x, y, Color(s, 0, 0))
	else:
		# A cirque of peaks round the north holding a high lake; its outflow falls off the range front into a wide
		# grassy valley floor that runs south between two low ridges to the sea.
		range_nodes.append(_range_path("Cirque", [Vector3(-1900, 0, -700), Vector3(-900, 0, -1600), Vector3(0, 0, -1850), Vector3(900, 0, -1600), Vector3(1900, 0, -700)], 0.0, 1.0))
		if args.has("bowl"):
			# A high bowl among the peaks (x,z,radius,share): uplift scaled down inside it, so its floor stays gentle above
			# the rock bar that holds its outlet.
			var bw: PackedStringArray = args["bowl"].split(",")
			var bowl := _range_path("Bowl", [Vector3(float(bw[0]), 0, float(bw[1])), Vector3(float(bw[0]) + 1.0, 0, float(bw[1]))], float(bw[2]), float(bw[3]))
			bowl.set_meta("basin", true)
			range_nodes.append(bowl)
		range_nodes.append(_range_path("WestRidge", [Vector3(-1500, 0, -900), Vector3(-1450, 0, 1100)], 380.0, 0.35))
		range_nodes.append(_range_path("EastRidge", [Vector3(1500, 0, -900), Vector3(1450, 0, 1100)], 380.0, 0.35))
		for y in 64:
			var z := Z0 + (y + 0.5) / 64.0 * GEN_SIZE
			for x in 64:
				var xw := X0 + (x + 0.5) / 64.0 * GEN_SIZE
				# High in the north, a low floor south of the range front (z > -700) that is lowest along the middle.
				var north := 1.0 - smoothstep(-1100.0, -600.0, z)
				var floor_w := 1.0 - smoothstep(550.0, 1000.0, absf(xw))
				# The block lower along the middle so its valleys gather into one trunk that leaves the range there.
				var funnel := 1.0 - float(args.get("funnel", "0.5")) * (1.0 - smoothstep(0.0, 1500.0, absf(xw)))
				var s := lerpf(0.12, 0.02, floor_w) * float(args.get("floor-uplift", "1")) * (1.0 - north) + 0.9 * north * funnel
				up.set_pixel(x, y, Color(s, 0, 0))
	if layout != "range" and not args.has("no-lake"):
		lake_marker = Node3D.new()
		lake_marker.name = "HighLake"
		lake_marker.position = Vector3(float(args.get("lake-x", "0")), 0, float(args.get("lake-z", "-1300")))
		lake_marker.set_meta("depth", float(args.get("lake-depth", "60")))
		# A bedrock sill, not a landslide dam: the lake spills over its own lip and the falls below it stay.
		lake_marker.set_meta("rock_lip", true)
		add_child(lake_marker)
	gen = TerrainGenerator3D.new()
	gen.name = "Generator"
	gen.size = Vector2(GEN_SIZE, GEN_SIZE)
	gen.cell_size = float(args.get("cell", "16"))
	gen.seed = 3
	gen.uplift = float(args.get("uplift", "3.0"))
	gen.base_uplift = float(args.get("base-uplift", "0.1"))
	gen.uplift_map = ImageTexture.create_from_image(up)
	gen.uplift_noise = noise
	# The sea along the south edge is the only outlet; the generator shapes the coast, beach and seabed.
	gen.outlet_edges = 0
	gen.ocean_edges = TerrainGenerator3D.OUTLET_POSITIVE_Z
	gen.coast_distance = 600.0
	# Lakes: automatic dammed valley lakes.
	gen.lake_count = int(args.get("lakes", "3" if layout == "range" else "0"))
	gen.strata_enabled = not args.has("no-strata") # hard rock bands: waterfalls where rivers cross them
	# Erosion detail down to 4 m gullies on the 1 m Landscape3D ground (16 m on the 4 m grid, which would alias finer).
	gen.detail_octaves = int(args.get("gdetail", "6" if args.get("mesh", "landscape") == "landscape" else "4"))
	gen.tile_erosion_enabled = args.has("tiles")
	gen.deposition = float(args.get("deposition", "0"))
	# Rock beds stepped into cliff faces (ledges), --ledges=0 for smooth faces.
	gen.cliff_ledge_spacing = float(args.get("ledges", "16"))
	gen.gorge_age = float(args.get("gorge-age", "12000"))
	gen.moraine_age = float(args.get("moraine-age", "0"))
	gen.scree_age = float(args.get("scree-age", "0"))
	gen.meander_age = float(args.get("meander-age", "0"))
	gen.fan_backfill = not args.has("no-backfill")
	if args.has("glacial"):
		# Ice ages: glaciers carve troughs and cirques, and the last one leaves its basins as lakes.
		gen.glacial_enabled = true
		gen.glacial_ela = float(args.get("ela", "500"))
		gen.glacial_share = float(args.get("glacial-share", "0.5"))
		gen.glacial_final = float(args.get("glacial-final", "100000"))
		gen.glacial_rock_variation = float(args.get("rock-var", "0"))
		gen.glacial_rock_scale = float(args.get("rock-scale", "500"))
		gen.glacial_inflow_balance = float(args.get("ice-inflow", "0"))
		gen.glacial_coarse_cell = float(args.get("ice-coarse", "0"))
		gen.ice_worn_detail = args.has("ice-worn")
		gen.glacial_erosion = float(args.get("glacial-erosion", "1"))
	if args.has("refine"):
		gen.refine_steps = int(args["refine"])
	if args.has("steps"):
		gen.steps = int(args["steps"]) # rivers drop sediment where they slow (Yuan et al. 2019)
	add_child(gen)
	var rp: Array[NodePath] = []
	for n in range_nodes:
		rp.append(gen.get_path_to(n))
	gen.ranges = rp
	if lake_marker != null:
		var lp: Array[NodePath] = [gen.get_path_to(lake_marker)]
		gen.lakes = lp
	if layout != "range":
		var markers: Array[NodePath] = []
		if args.has("inflow"):
			# The main river's catchment reaches past the map: that land drains in at --inflow-at: edge (the north
			# edge above the cirque, or --inflow-x/z), lake (the high lake marker), or x,z.
			var at: String = args.get("inflow-at", "edge")
			var inflow := Node3D.new()
			inflow.name = "Inflow"
			if at == "lake":
				inflow.position = lake_marker.position
			elif at.contains(","):
				inflow.position = Vector3(float(at.get_slice(",", 0)), 0, float(at.get_slice(",", 1)))
			else:
				inflow.position = Vector3(float(args.get("inflow-x", "0")), 0, float(args.get("inflow-z", "-2048")))
			inflow.set_meta("area", float(args["inflow"]))
			if args.has("inflow-erosion"):
				# Share of that land's area that erodes (0: it only brings water).
				inflow.set_meta("erosion", float(args["inflow-erosion"]))
			if at == "lake":
				lake_marker.set_meta("inflow", inflow)
			if args.has("inflow-level"):
				# The land beyond the edge is high: the river enters at this height, not cut down to the plain's.
				inflow.set_meta("level", float(args["inflow-level"]))
			add_child(inflow)
			markers.append(gen.get_path_to(inflow))
		if args.has("sill"):
			# A sill of resistant rock (m radius) at the high lake's outlet (or --sill-at=x,z), held at --sill-level:
			# the river upstream grades to it, the lake sits behind it, and the river falls off it.
			var sill := Node3D.new()
			sill.name = "Sill"
			var sa: String = args.get("sill-at", "")
			sill.position = Vector3(float(sa.get_slice(",", 0)), 0, float(sa.get_slice(",", 1))) if sa.contains(",") else lake_marker.position
			sill.set_meta("sill", float(args["sill"]))
			if args.has("sill-level"):
				sill.set_meta("level", float(args["sill-level"]))
			add_child(sill)
			markers.append(gen.get_path_to(sill))
		if args.has("bar"):
			# A bar of resistant rock across the valley (x0,z0,x1,z1), held at --bar-level: a riegel the river and the
			# ice cross slowly, so the ground above it stays high (sill markers every 32 m).
			var bv: PackedStringArray = args["bar"].split(",")
			var bar_pts: Array[Vector3] = []
			for k in range(0, bv.size() - 1, 2):
				bar_pts.append(Vector3(float(bv[k]), 0, float(bv[k + 1])))
			var along: Array[Vector3] = []
			for k in bar_pts.size() - 1:
				var steps := maxi(1, int(bar_pts[k].distance_to(bar_pts[k + 1]) / 32.0))
				for i in steps:
					along.append(bar_pts[k].lerp(bar_pts[k + 1], float(i) / steps))
			along.append(bar_pts[bar_pts.size() - 1])
			for i in along.size():
				var m := Node3D.new()
				m.name = "Bar%d" % i
				m.position = along[i]
				m.set_meta("sill", float(args.get("bar-width", "40")))
				if args.has("bar-level"):
					m.set_meta("level", float(args["bar-level"]))
				add_child(m)
				markers.append(gen.get_path_to(m))
		gen.inflows = markers
	var t := Time.get_ticks_msec()
	# One climate for the rivers (rain and snowmelt by month), the data map's climate fields and the plants: a mild
	# coast with the wet wind from the sea (south, +Z).
	climate = ClimateRecipe.new()
	climate.temperature_january = 1.0 # the snowpack's winter too (SnowClimate.recipe)
	climate.temperature_july = 17.0
	climate.precipitation = 1100.0
	climate.wind_from = 190.0
	gen.climate_recipe = climate
	gen.river_month = int(args.get("month", "4")) # spring: the snowmelt flood
	# --sky-occlusion=0: the ground's raster ambient light without the hills' sky occlusion (an A/B frame).
	gen.sky_occlusion = args.get("sky-occlusion", "1") != "0"
	gen.generate()
	if lake_marker != null and not args.has("lake-x"):
		# The high lake sits above the range's tallest fall: a marker up its river from the lip, then the ground is
		# generated again with the dam (the falls come from the rock bands, which the dam does not move).
		var falls: Array = gen.get_waterfalls(float(args.get("min-area", "500000")), 4.0)
		var rivs: Array = gen.get_rivers(float(args.get("min-area", "500000")))
		var tall: Dictionary = {}
		for f in falls:
			if tall.is_empty() or f["height"] > tall["height"]:
				tall = f
		if not tall.is_empty():
			var pts: PackedVector3Array = rivs[int(tall["river"])]["points"]
			var i := int(tall["index"])
			var run := 0.0
			var up_m := float(args.get("lake-up", "250"))
			while i > 0 and run < up_m:
				run += pts[i].distance_to(pts[i - 1])
				i -= 1
			lake_marker.position = Vector3(pts[i].x, 0, pts[i].z)
			if lake_marker.has_meta("inflow"):
				(lake_marker.get_meta("inflow") as Node3D).position = lake_marker.position
			_log("high lake marker %.0f m up the river from the %.0f m fall at %s" % [run, tall["height"], tall["top"]])
			gen.generate()
	_log("generate %d ms, grid %s, last change %.3f m, fan slots filled %d, sky occlusion %.0f ms" % [Time.get_ticks_msec() - t, gen.get_grid_size(), gen.get_last_change(), gen.get_last_slots_filled(), gen.get_last_sky_msec()])
	if gen.moraine_age > 0.0:
		_log("moraines: %.0f m3 of debris" % gen.get_last_moraine_volume())
	if gen.meander_age > 0.0:
		_log("meanders: %d reaches, %d oxbows" % [gen.get_last_meander_count(), gen.get_last_oxbow_count()])
	if gen.scree_age > 0.0:
		_log("scree: %.0f m3" % gen.get_last_scree_volume())
	if gen.glacial_enabled:
		_log("glaciers: %.0f ms, last glaciation %.2f km2 of ice, eroded up to %.0f m" % [gen.get_last_glacial_msec(), gen.get_last_glacial_ice_area() / 1e6, gen.get_last_glacial_eroded()])
		for l in gen.get_lake_results():
			_log("  lake level %.0f m, depth %.0f m, %.1f ha, catchment %.2f km2 at %s, outlet %s" % [l["level"], l["depth"], l["area"] / 1e4, l["catchment"] / 1e6, l["center"], l.get("outlet")])
	if args.get("boulders", "1") != "0":
		var tb := Time.get_ticks_msec()
		var bl: PackedVector4Array = gen.get_boulders(Rect2(-2048, -2048, 4096, 4096), float(args.get("boulders", "1")))
		var big := 0
		for b in bl:
			if b.w >= 1.0:
				big += 1
		_log("boulders below cliffs: %d (%d of 1 m radius or more) in %d ms" % [bl.size(), big, Time.get_ticks_msec() - tb])
		# For --view=boulders: the densest 40 m patch of blocks near the middle of the map.
		var cells := {}
		for b in bl:
			var sl := absf(gen.get_height(Vector3(b.x + 8.0, 0, b.z)) - gen.get_height(Vector3(b.x - 8.0, 0, b.z))) / 16.0 + absf(gen.get_height(Vector3(b.x, 0, b.z + 8.0)) - gen.get_height(Vector3(b.x, 0, b.z - 8.0))) / 16.0
			if absf(b.x) < 1500.0 and absf(b.z) < 1500.0 and sl < 0.5:
				var key := Vector2i(int(floor(b.x / 40.0)), int(floor(b.z / 40.0)))
				cells[key] = int(cells.get(key, 0)) + 1
		var best_n := 0
		for key in cells:
			if cells[key] > best_n:
				best_n = cells[key]
				boulder_spot = Vector3(key.x * 40.0 + 20.0, 0, key.y * 40.0 + 20.0)
		_log("densest block patch: %d blocks in 40 m at %s" % [best_n, boulder_spot])
		if bl.size() > 0:
			# Stand-ins: flattened low spheres, a third sunk into the ground, until real rock meshes come.
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
			for k in bl.size():
				var b := bl[k]
				var r := b.w
				var basis := Basis(Vector3.UP, fmod(b.x * 12.9898 + b.z * 78.233, TAU)).scaled(Vector3(r * 2.4, r * 1.3, r * 1.9))
				mm.set_instance_transform(k, Transform3D(basis, Vector3(b.x, b.y + r * 0.3, b.z)))
			var mmi := MultiMeshInstance3D.new()
			mmi.name = "Boulders"
			mmi.multimesh = mm
			add_child(mmi)
	if args.has("soil-probe"):
		for zz in [-1100, -900, -600, -300, 0, 300, 600, 900, 1200]:
			var row := PackedStringArray()
			for xx in [-900, -600, -300, 0, 300, 600, 900]:
				var pp := Vector3(xx, 0, zz)
				row.append("%.0f:%.1f/%.2f/%.1f" % [gen.get_height(pp), gen.get_soil_depth(pp), gen.get_rock_exposure(pp), gen.get_sediment_depth(pp)])
			print("SOIL z ", zz, " ", " ".join(row))
	if args.has("face-probe"):
		var fp: PackedStringArray = args["face-probe"].split(",")
		var o := Vector3(float(fp[0]), 0, float(fp[1]))
		var dv := Vector3(float(fp[2]), 0, float(fp[3])).normalized()
		var row := PackedStringArray()
		for i in 80:
			row.append("%.0f/%.0f/%.1f/%.2f/%.1f" % [gen.get_height(o + dv * i * 2.0), gen.get_base_height(o + dv * i * 2.0), gen.get_soil_depth(o + dv * i * 2.0), gen.get_rock_exposure(o + dv * i * 2.0), gen.get_sediment_depth(o + dv * i * 2.0)])
		print("FACE ", " ".join(row))
	if args.has("dump-heights"):
		var hb := PackedFloat32Array()
		for zi in 257:
			for xi in 257:
				hb.append(gen.get_base_height(Vector3(-2048 + xi * 16, 0, -2048 + zi * 16)))
		var fa := FileAccess.open(args["dump-heights"], FileAccess.WRITE)
		fa.store_buffer(hb.to_byte_array())
		fa.close()
		get_tree().quit()
		return
	if args.has("groove-probe"):
		var img := Image.create(1400, 500, false, Image.FORMAT_RGB8)
		for half in 2:
			for j in 250:
				for i in 700:
					var q := Vector3(-1400 + i * 4, 0, 500 + j * 4)
					var hx: float
					var hz: float
					if half == 0:
						hx = gen.get_base_height(q + Vector3(2, 0, 0)) - gen.get_base_height(q - Vector3(2, 0, 0))
						hz = gen.get_base_height(q + Vector3(0, 0, 2)) - gen.get_base_height(q - Vector3(0, 0, 2))
					else:
						hx = gen.get_height(q + Vector3(2, 0, 0)) - gen.get_height(q - Vector3(2, 0, 0))
						hz = gen.get_height(q + Vector3(0, 0, 2)) - gen.get_height(q - Vector3(0, 0, 2))
					var n := Vector3(-hx / 4.0, 1.0, -hz / 4.0).normalized()
					var l := clampf(n.dot(Vector3(-0.5, 0.7, -0.5).normalized()), 0, 1)
					img.set_pixel(i + half * 700, j * 2, Color(l, l, l))
					img.set_pixel(i + half * 700, j * 2 + 1, Color(l, l, l))
		img.save_png(args["groove-probe"])
		var rough := 0
		var rmax := 0.0
		var nalluv := 0
		for zi in range(1, 255):
			for xi in range(1, 255):
				var q := Vector3(-2048 + xi * 16, 0, -2048 + zi * 16)
				if gen.get_sediment_depth(q) < 1.0:
					continue
				nalluv += 1
				var c := gen.get_base_height(q)
				var lx := gen.get_base_height(q + Vector3(16, 0, 0)) + gen.get_base_height(q - Vector3(16, 0, 0)) - 2 * c
				var lz := gen.get_base_height(q + Vector3(0, 0, 16)) + gen.get_base_height(q - Vector3(0, 0, 16)) - 2 * c
				var m := maxf(absf(lx), absf(lz))
				rmax = maxf(rmax, m)
				if m > 4.0:
					rough += 1
		print("ROUGH alluvium cells %d, one-cell bumps over 4 m %d, largest %.1f m" % [nalluv, rough, rmax])
		for z in range(640, 900, 16):
			var row := PackedStringArray()
			for x in range(-160, 176, 16):
				var q := Vector3(x, 0, z)
				row.append("%5.1f/%3.0f" % [gen.get_base_height(q), gen.get_sediment_depth(q)])
			print("z%4d " % z, " ".join(row))
		get_tree().quit()
		return
	if args.has("profile"):
		# Heights down the middle of the valley and across it, for laying out the floor.
		var down := PackedStringArray()
		for z in range(-800, 1300, 200):
			down.append("%d:%.1f/%.1f" % [z, gen.get_height(Vector3(0, 0, z)), gen.get_sediment_depth(Vector3(0, 0, z))])
		var across := PackedStringArray()
		for x in range(-1200, 1300, 200):
			across.append("%d:%.1f" % [x, gen.get_height(Vector3(x, 0, 200))])
		_log("down the valley (z:height/sediment) " + " ".join(down))
		if args.has("profile-fine"):
			var fine := PackedStringArray()
			for z in range(600, 1600, 32):
				var q := Vector3(float(args.get("px", "0")), 0, z)
				fine.append("%d:%.1f/s%.0f/r%.2f" % [z, gen.get_base_height(q), gen.get_sediment_depth(q), gen.get_rock_exposure(q)])
			_log("base heights near the coast " + " ".join(fine))
		_log("across at z=200 (x:h) " + " ".join(across))
	t = Time.get_ticks_msec()
	var img := gen.sample_heights(Vector2(X0, Z0), STEP, Vector2i(NX, NZ))
	heights = img.get_data().to_float32_array()
	var hmax := -INF
	for i in heights.size():
		if is_nan(heights[i]):
			heights[i] = -gen.max_sea_depth # past the generated rectangle: open sea floor
		hmax = maxf(hmax, heights[i])
	_log("water %d ms: sea %s, %d lakes" % [gen.get_last_water_msec(), gen.has_sea_result(), gen.get_lake_result_count()])
	for l in gen.get_lake_results():
		_log("  lake level %.1f m, depth %.1f m, area %.1f ha, outlet %s" % [l["level"], l["depth"], l["area"] / 10000.0, l.has("outlet")])
	_log("sample_heights %d ms, highest %.0f m" % [Time.get_ticks_msec() - t, hmax])



func _range_path(n: String, pts: Array, width: float, uplift: float) -> Path3D:
	var path := Path3D.new()
	path.name = n
	var curve := Curve3D.new()
	for i in pts.size():
		var tin := Vector3.ZERO
		var tout := Vector3.ZERO
		if i > 0 and i < pts.size() - 1:
			var t: Vector3 = (pts[i + 1] - pts[i - 1]) * 0.25
			tin = -t
			tout = t
		curve.add_point(pts[i], tin, tout)
	path.curve = curve
	if width > 0.0:
		path.set_meta("width", width)
	if uplift != 1.0:
		path.set_meta("uplift", uplift)
	add_child(path)
	return path


# A top-down map of the generated land for laying out a scene quickly: hillshade, sea and lakes in blue, rivers in
# cyan, the generator's waterfalls in red. 16 m a pixel, north up.
func _save_map(path: String) -> void:
	var w := (NX - 1) / 4 + 1
	var h := (NZ - 1) / 4 + 1
	var img := Image.create(w, h, false, Image.FORMAT_RGB8)
	var water: Image = gen.get_water_image()
	for j in h:
		for i in w:
			var x := X0 + i * 16.0
			var z := Z0 + j * 16.0
			var e := height_at(x, z)
			var dx := height_at(x + 16.0, z) - height_at(x - 16.0, z)
			var dz := height_at(x, z + 16.0) - height_at(x, z - 16.0)
			var n := Vector3(-dx, 64.0, -dz).normalized()
			var shade := clampf(n.dot(Vector3(-0.5, 0.7, -0.5).normalized()), 0.0, 1.0)
			var tint := Color(0.45, 0.6, 0.35).lerp(Color(0.75, 0.72, 0.68), clampf(e / 900.0, 0.0, 1.0))
			var c := tint * (0.35 + 0.75 * shade)
			var wd := 0.0
			if water != null and i < water.get_width() and j < water.get_height():
				wd = water.get_pixel(i, j).r
			if e < gen.sea_level or wd > 0.3:
				c = Color(0.15, 0.3, 0.6)
			img.set_pixel(i, j, Color(c.r, c.g, c.b))
	for r in gen.get_rivers(float(args.get("min-area", "1500000"))):
		for q in r["points"]:
			var pi := Vector2i(int((q.x - X0) / 16.0), int((q.z - Z0) / 16.0))
			if pi.x >= 0 and pi.y >= 0 and pi.x < w and pi.y < h:
				img.set_pixelv(pi, Color(0.3, 0.9, 1.0))
	for f in gen_falls:
		var q: Vector3 = f["top"]
		var pi := Vector2i(int((q.x - X0) / 16.0), int((q.z - Z0) / 16.0))
		for dy in range(-2, 3):
			for dx in range(-2, 3):
				var pp := pi + Vector2i(dx, dy)
				if pp.x >= 0 and pp.y >= 0 and pp.x < w and pp.y < h:
					img.set_pixelv(pp, Color(1, 0.1, 0.1))
	img.save_png(path)
	_log("map saved " + path)
	# Discharge along the rivers, to place the high lake where the fall below it carries a real curtain.
	var ri := 0
	for r in gen.get_rivers(float(args.get("min-area", "1500000"))):
		var rp: PackedVector3Array = r["points"]
		var ar: PackedFloat32Array = r["areas"]
		_log("river %d: %d pts, head %s, mouth %s, area %.2f km2, Q %.2f m3/s (max month %.2f), ends %s, joins %d" % [ri, rp.size(),
				str(rp[0].round()), str(rp[rp.size() - 1].round()), ar[ar.size() - 1] / 1e6, r["discharge"],
				Array(r["monthly_discharge"]).max(), r["ends_in"], r["joins"]])
		if args.has("river-dump") and ri == int(args.get("river-dump", "0")):
			for k in range(0, rp.size(), 4):
				_log("  pt %d %s ground %.2f" % [k, str(rp[k]), gen.get_height(rp[k])])
		ri += 1
	for f in gen.get_waterfalls(float(args.get("min-area", "1500000")), 2.0):
		_log("fall: top %s h %.1f m, w %.1f m, Q %.2f m3/s, river %d" % [str(f["top"].round()), f["height"], f["width"], f["discharge"], f["river"]])
	for px in [-1200.0, -500.0, 0.0, 500.0]:
		var row := ""
		var pz := -1900.0
		while pz <= 1500.0:
			row += " %.0f" % height_at(px, pz)
			pz += 200.0
		_log("profile x %.0f, z -1900..1500 by 200:%s" % [px, row])

func height_at(x: float, z: float) -> float:
	var fx := clampf((x - X0) / STEP, 0.0, NX - 1.001)
	var fz := clampf((z - Z0) / STEP, 0.0, NZ - 1.001)
	var ix := int(fx)
	var iz := int(fz)
	var tx := fx - ix
	var tz := fz - iz
	var i := iz * NX + ix
	return lerpf(lerpf(heights[i], heights[i + 1], tx), lerpf(heights[i + NX], heights[i + NX + 1], tx), tz)


func _lake_and_fall() -> void:
	# The lake for the camera and the checks: the biggest one the generator made.
	var lr: Array = gen.get_lake_results()
	if lr.size() > 0:
		var big: Dictionary = lr[0]
		for l in lr:
			if (l["level"] > big["level"]) if args.get("lake-pick", "") == "high" else (l["area"] > big["area"]):
				big = l
		lake_center = big["center"]
		lake_level = big["level"]
		var rect: Rect2 = big["rect"]
		lake_axes = Vector2(maxf(rect.size.x, rect.size.y), minf(rect.size.x, rect.size.y)) * 0.5
		lake_dir = Vector2(1, 0) if rect.size.x >= rect.size.y else Vector2(0, 1)
	# Waterfalls: hard rock bands (strata) hold the rivers up and they fall off them; the camera's fall is the
	# tallest the generator reports.
	var raw: Array = gen.get_rivers(float(args.get("min-area", "1500000")))
	_log("rivers from the generator: %d" % raw.size())
	gen_falls = gen.get_waterfalls(float(args.get("min-area", "1500000")), 4.0)
	if gen_falls.size() > 0:
		var tall: Dictionary = gen_falls[0]
		for f in gen_falls:
			if f["height"] > tall["height"]:
				tall = f
		var top: Vector3 = tall["top"]
		var bottom: Vector3 = tall["bottom"]
		fall_point = top
		fall_height = float(tall["height"])
		var fd := bottom - top
		fall_dir = Vector2(fd.x, fd.z).normalized() if Vector2(fd.x, fd.z).length() > 0.1 else Vector2(0, 1)
		if args.has("lip-profile"):
			var lt: Vector3 = tall["top"]
			var lb: Vector3 = tall["bottom"]
			var dd := Vector3(lb.x - lt.x, 0, lb.z - lt.z).normalized()
			var prof := PackedStringArray()
			for k in range(-4, 44, 2):
				var q := lt + dd * float(k)
				prof.append("%d:%.1f" % [k, gen.get_height(q)])
			_log("lip profile (m along: height) " + " ".join(prof))
		if args.has("spray-map"):
			var si: Image = gen.get_spray_image()
			if si:
				si.save_png(args["spray-map"])
				print("SPRAY ", si.get_size(), " at big fall foot ", gen.get_spray_potential(Vector3(400, 0, -1420)))
		if args.has("list-falls"):
			for f in gen_falls:
				var run: float = Vector2(f["top"].x - f["bottom"].x, f["top"].z - f["bottom"].z).length()
				_log("  fall %.0f m over %.0f m, Q %.2f, hardness %.2f, face %.1f m, jet %.1f m, free %s, top %s" % [f["height"], run, f["discharge"], f["hardness"], f["face_run"], f["jet_throw"], str(f["free_fall"]), f["top"]])
				if args.has("pool-probe") and f.has("pool_center"):
					var pc: Vector3 = f["pool_center"]
					var dr: Vector3 = f["direction"]
					var row := PackedStringArray()
					for i in range(-12, 25):
						row.append("%.1f" % gen.get_height(pc + dr * i * 1.0))
					print("POOL ", f.get("id"), " along ", " ".join(row))
				var lp: PackedVector3Array = f.get("lip_points", PackedVector3Array())
				if lp.size() > 0:
					var ly := PackedStringArray()
					for q in lp:
						ly.append("%.1f" % (q.y - f["top"].y))
					_log("    lip width %.1f m, points vs top: %s" % [f.get("lip_width", 0.0), " ".join(ly)])
				_log("    %s %s, dir %s, impact %.1f m/s (free %.1f), tiers %d, lands in %s, pool %s" % [f.get("id", "?"), f.get("type", "?"), f.get("direction", Vector3.ZERO), f.get("impact_speed", 0.0), f.get("impact_speed_free", 0.0), f.get("tiers", []).size(), f.get("lands_in", "?"), ("r %.1f m, depth %.1f m, level %.1f" % [f["pool_radius"], f["pool_depth"], f["pool_level"]]) if f.has("pool_radius") else "none"])
		_log("waterfalls from the generator: %d, tallest %.0f m high, %.0f m wide, %.1f m3/s at %s" % [gen_falls.size(), tall["height"], tall["width"], tall["discharge"], top])
	_log("lake at %s level %.1f m" % [lake_center, lake_level])


func _build_ground() -> void:
	var t := Time.get_ticks_msec()
	var use_landscape: bool = args.get("mesh", "landscape") == "landscape"
	# The ground body: Landscape3D's own 1 m heightfield tiles with the landscape mesh (vertical lips stay vertical
	# for the rivers' falls), the 4 m HeightMapShape3D with the grid mesh or --collision=heightmap.
	if not use_landscape or args.get("collision", "landscape") == "heightmap":
		_build_heightmap_body()
	var gh := Ground.new()
	gh.name = "GroundHeight"
	gh.bench = self
	add_child(gh)
	if args.get("ground", "terrain") == "terrain":
		# TerrainMaterial3D: layers by the generator's geology, height-blended, biplanar cliffs, erosion detail
		# normals, grass tint, wetness and snow. Plain colours here (textures would come from LFS).
		var tm := TerrainMaterial3D.new()
		tm.use_generator(gen)
		tm.detail_octaves = int(args.get("mdetail", "4"))
		if args.has("far-cover"):
			# Far ground blend against the plant cover map the world field builds (cover_map_enabled below).
			tm.far_blend_enabled = true
			tm.far_start = float(args.get("far-start", "300"))
		elif args.has("far-blend"):
			# Far ground blend against a test cover map (vegetation publishes the real one): dark green cover over
			# the low ground, thinning out with height, 8 m texels over the generator's area.
			tm.far_blend_enabled = true
			tm.far_start = float(args.get("far-start", "300"))
			var cn := 512
			var ci := Image.create(cn, cn, false, Image.FORMAT_RGBA8)
			for zz in cn:
				for xx in cn:
					var px := -2048.0 + xx * 8.0
					var pz := -2048.0 + zz * 8.0
					var hh := gen.get_height(Vector3(px, 0, pz))
					var cov := clampf(1.0 - (hh - 60.0) / 300.0, 0.0, 0.85) if hh > 2.0 else 0.0
					ci.set_pixel(xx, zz, Color(0.16, 0.24, 0.09, cov))
			cover_tex = ImageTexture.create_from_image(ci)
			RenderingServer.global_shader_parameter_set("terrain_cover_map", cover_tex.get_rid())
			RenderingServer.global_shader_parameter_set("terrain_cover_rect", Vector4(-2048, -2048, 4096, 4096))
		tm.update_shader()
		ground_mat = tm
	else:
		var sm := StandardMaterial3D.new()
		sm.vertex_color_use_as_albedo = true
		sm.roughness = 0.9
		sm.set_feature(BaseMaterial3D.FEATURE_WETNESS, true)
		sm.wetness_porosity = 0.8
		sm.set_feature(BaseMaterial3D.FEATURE_SNOW, true)
		ground_mat = sm
	if args.get("mesh", "landscape") == "landscape":
		# Landscape3D: the generator's ground at 1 m up close, chunked with crack-free LODs picked from the camera.
		landscape = Landscape3D.new()
		landscape.name = "Landscape"
		add_child(landscape)
		landscape.material = ground_mat
		landscape.lod_error = float(args.get("lod-error", "2"))
		landscape.source = landscape.get_path_to(gen)
		var own_body := ground_body == null
		landscape.collision_enabled = own_body
		if own_body:
			# Every chunk gets its body: the checks probe the whole world (peak, beach, banks), not only round the
			# camera. 4096 tiles of 64 m at 1 m, about 0.2 s.
			var centre := Node3D.new()
			centre.name = "CollisionCentre"
			add_child(centre)
			var wr: Rect2 = gen.get_world_rect()
			centre.global_position = Vector3(wr.get_center().x, 0, wr.get_center().y)
			landscape.collision_target = landscape.get_path_to(centre)
			landscape.collision_radius = wr.size.length() * 0.5 + 64.0
		landscape.rebuild()
		if own_body:
			landscape.load_all_heights()
			landscape.update_collision()
		var st: Dictionary = landscape.get_stats()
		_log("landscape %s, ground %d ms" % [st, Time.get_ticks_msec() - t])
		return
	_build_grid_mesh()
	_log("ground %d ms" % (Time.get_ticks_msec() - t))


# The 4 m HeightMapShape3D ground body (--mesh=grid or --collision=heightmap).
func _build_heightmap_body() -> void:
	var shape := HeightMapShape3D.new()
	shape.map_width = NX
	shape.map_depth = NZ
	shape.map_data = heights
	ground_body = StaticBody3D.new()
	ground_body.name = "Ground"
	var cs := CollisionShape3D.new()
	cs.shape = shape
	cs.scale = Vector3(STEP, 1.0, STEP)
	ground_body.add_child(cs)
	ground_body.position = Vector3(X0 + (NX - 1) * STEP * 0.5, 0.0, Z0 + (NZ - 1) * STEP * 0.5)
	add_child(ground_body)


# The 4 m grid ground (--mesh=grid): one mesh, no LODs, geology vertex colours for the standard material.
func _build_grid_mesh() -> void:
	# Display mesh, coloured by the generator's geology: bare rock, scree, soil, alluvium; sand at the coast.
	# STAND-IN material: a StandardMaterial3D with vertex colours, because the engine's snow and wetness live on
	# BaseMaterial3D only (under path tracing too), while the generator's geology and detail-normal code is shader
	# code for a ShaderMaterial.
	var verts := PackedVector3Array()
	verts.resize(NX * NZ)
	var norms := PackedVector3Array()
	norms.resize(NX * NZ)
	var cols := PackedColorArray()
	cols.resize(NX * NZ)
	var rock := Color(0.36, 0.35, 0.33)
	var scree := Color(0.48, 0.46, 0.42)
	var soil := Color(0.2, 0.22, 0.1)
	var alluvium := Color(0.28, 0.27, 0.16)
	var sand := Color(0.7, 0.62, 0.46)
	for zi in NZ:
		var zu := maxi(zi - 1, 0) * NX
		var zd := mini(zi + 1, NZ - 1) * NX
		for xi in NX:
			var i := zi * NX + xi
			var x := X0 + xi * STEP
			var z := Z0 + zi * STEP
			var h := heights[i]
			verts[i] = Vector3(x, h, z)
			var hx := heights[zi * NX + mini(xi + 1, NX - 1)] - heights[zi * NX + maxi(xi - 1, 0)]
			var hz := heights[zd + xi] - heights[zu + xi]
			norms[i] = Vector3(-hx, 2.0 * STEP, -hz).normalized()
			var g := gen.get_geology_at(Vector3(x, h, z)) if z < Z0 + GEN_SIZE else Color(0, 0, 0, 0)
			var c := soil
			if is_nan(g.r):
				c = sand
			else:
				c = soil.lerp(alluvium, clampf((g.a - 5.5) / 1.5, 0.0, 1.0))
				c = c.lerp(rock, clampf(g.g, 0.0, 1.0))
				c = c.lerp(scree, clampf(g.b, 0.0, 1.0))
			c = c.lerp(sand, 1.0 - smoothstep(1.2, 4.0, h))
			cols[i] = c
	var idx := PackedInt32Array()
	idx.resize((NX - 1) * (NZ - 1) * 6)
	var k := 0
	for zi in NZ - 1:
		for xi in NX - 1:
			var i := zi * NX + xi
			idx[k] = i
			idx[k + 1] = i + 1
			idx[k + 2] = i + NX
			idx[k + 3] = i + 1
			idx[k + 4] = i + NX + 1
			idx[k + 5] = i + NX
			k += 6
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_INDEX] = idx
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	ground_mesh = MeshInstance3D.new()
	ground_mesh.name = "GroundMesh"
	ground_mesh.mesh = mesh
	ground_mesh.material_override = ground_mat
	add_child(ground_mesh)


# --- Water -------------------------------------------------------------------------------------------------------

func _build_water() -> void:
	# The generator's own water: the sea (shore waves, seabed from the ground) and one surface per lake, then the
	# rivers, each ending on the sea, a lake or the river it joins, outflows starting at the lake outlets.
	var t := Time.get_ticks_msec()
	water_nodes = gen.build_water(self)
	for w in water_nodes:
		if String(w.name).begins_with("Sea"):
			ocean = w
			# An open coast: part of the sea is swell from distant storms, long near-parallel crests (Horvath 2015).
			ocean.swell = float(args.get("swell", "0.4"))
		else:
			lakes.append(w)
	if lakes.size() > 0:
		lake = lakes[0]
		for l in lakes:
			if absf(l.global_position.y + l.water_level - lake_level) < absf(lake.global_position.y + lake.water_level - lake_level):
				lake = l
	rivers = gen.build_river_paths(self, float(args.get("min-area", "1500000")))
	_log("water + %d rivers built in %d ms (%d lakes)" % [rivers.size(), Time.get_ticks_msec() - t, lakes.size()])
	# Contact falls (cascades and horsetails down steep rock) from the generator's fall records.
	if ClassDB.class_exists("PhysXWaterfalls3D"):
		var wf = ClassDB.instantiate("PhysXWaterfalls3D")
		wf.name = "Waterfalls"
		add_child(wf)
		wf.source = wf.get_path_to(gen)
		wf.rebuild()
		for f in wf.get_contact_falls():
			_log("contact fall %s" % [f])
		contact_falls = wf.get_contact_falls()
		if args.has("contact-debug"):
			for ch in wf.get_children(true):
				if ch is MeshInstance3D and ch.mesh != null:
					var vs: PackedVector3Array = ch.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
					var below := 0
					var worst := 0.0
					var mean := 0.0
					for v in vs:
						var dh: float = v.y - gen.get_height(v)
						mean += dh
						worst = minf(worst, dh)
						if dh < 0.0:
							below += 1
					_log("contact sheet: %d vertices, %d below the ground, mean %.2f m above, worst %.2f m" % [vs.size(), below, mean / maxf(vs.size(), 1), worst])
			for g in gen.get_waterfalls():
				if g.get("id", "") == args.get("contact", ""):
					var tp: Vector3 = g["top"]
					var dd: Vector3 = g["direction"]
					var lp: PackedVector3Array = g["lip_points"]
					_log("  %s top %s bottom %s height %.1f lip_width %.1f share %s" % [g["id"], tp, g["bottom"], g["height"], g["lip_width"], g["lip_share"]])
					for c in lp.size():
						var line := ""
						for i in 12:
							var q: Vector3 = Vector3(lp[c].x, 0, lp[c].z) + dd * i * 2.0
							line += " %.1f" % (gen.get_height(q) - tp.y)
						_log("    lip %d at %.1f:%s" % [c, lp[c].y - tp.y, line])
				if g.get("type", "") == "plunge":
					continue
				var top: Vector3 = g["top"]
				var lip: PackedVector3Array = g["lip_points"]
				var prof: PackedVector3Array = g["face_profiles"][1]
				var d: Vector3 = g["direction"]
				var s := ""
				for i in [0, 3, 6, 9, 12, 15]:
					var q: Vector3 = (lip[3] + lip[4]) * 0.5 + d * prof[i].x
					s += " %.1f/%.1f" % [prof[i].y + top.y, gen.get_height(q)]
				_log("  %s share %s profile vs ground:%s" % [g["id"], g["lip_share"], s])
				break
		# --contact-falls=0: built (the contact view still finds them) but hidden, for before/after pairs.
		wf.visible = args.get("contact-falls", "1") != "0"



func _river(nm: String, pts: PackedVector3Array, ws: PackedFloat32Array, q: float, target: Node) -> PhysXRiver3D:
	var r := PhysXRiver3D.new()
	r.name = nm
	var c := Curve3D.new()
	for p in pts:
		c.add_point(p)
	r.curve = c
	var w2 := PackedFloat32Array()
	for w in ws:
		w2.append(clampf(w, 3.0, 40.0))
	r.widths = w2
	r.discharge = maxf(q, 0.5)
	r.step_length = 2.0
	add_child(r)
	if target:
		r.mouth_target = r.get_path_to(target)
	return r


func _ellipse_mesh(axes: Vector2, dir: Vector2) -> ArrayMesh:
	var side := Vector2(-dir.y, dir.x)
	var verts := PackedVector3Array()
	var n := 96
	for k in n:
		var a0 := TAU * k / n
		var a1 := TAU * (k + 1) / n
		var p0 := dir * cos(a0) * axes.x + side * sin(a0) * axes.y
		var p1 := dir * cos(a1) * axes.x + side * sin(a1) * axes.y
		verts.append(Vector3.ZERO)
		verts.append(Vector3(p1.x, 0, p1.y))
		verts.append(Vector3(p0.x, 0, p0.y))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return m


# --- Weather, sky, snow ------------------------------------------------------------------------------------------

func _build_weather() -> void:
	sun = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 1.6
	sun.shadow_enabled = args.get("sun-shadow", "1") != "0"
	sun.directional_shadow_max_distance = float(args.get("shadow-max", "6000"))
	add_child(sun)
	# The sun from a date, a time and a place (SkyClock, Meeus): the month is the rivers' month, so the light,
	# the flows and the snowmelt are one day. World -Z is north; mid-latitude mountains on a western coast.
	clock = SkyClock.new()
	clock.name = "SkyClock"
	add_child(clock)
	clock.latitude = 45.0
	clock.longitude = 0.0
	clock.utc_offset = 0.0
	clock.month = int(args.get("month", "4")) + 1
	clock.day = int(args.get("day", "15"))
	clock.time_of_day = float(args.get("time", "16.5"))
	clock.sun_light = clock.get_path_to(sun)
	weather = WeatherMap3D.new()
	weather.name = "Weather"
	weather.extent = 8192.0
	weather.resolution = int(args.get("weather-res", "128"))
	weather.ground_height = 0.0
	# The weather and the snowpack stay on the world, not the camera, so the peaks are covered from any view.
	var anchor := Node3D.new()
	anchor.name = "WorldAnchor"
	add_child(anchor)
	add_child(weather)
	# One season state for every system (WeatherMap3D.get_season): the date from the SkyClock, the year's weather
	# from the same climate as the rivers and the plants.
	weather.climate = climate
	weather.sky_clock = weather.get_path_to(clock)
	weather.follow_target = weather.get_path_to(anchor)
	if args.get("cloud-shadow", "1") != "0":
		weather.sun_light = weather.get_path_to(sun) # the clouds' shadow, as the sun's projector
	env = Environment.new()
	sky_mat = WeatherSkyMaterial.new()
	if args.has("atmosphere"):
		sky_mat.atmosphere_model = WeatherSkyMaterial.ATMOSPHERE_PHYSICAL_SKY if args["atmosphere"] == "physical" else WeatherSkyMaterial.ATMOSPHERE_SCATTERING
	sky_mat.set_shader_parameter("atmo_far_water", 1.0) # the sea runs on past the scene to the horizon
	var sky := Sky.new()
	sky.sky_material = sky_mat
	sky.process_mode = Sky.PROCESS_MODE_INCREMENTAL
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.pathtracing_sky_shader_enabled = true
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not args.has("raster")
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	# The ground's sky occlusion weighs the hidden sky by the sky's own light (read back when it changes).
	if gen != null and args.get("sky-light", "1") != "0":
		gen.sky_light_source = gen.get_path_to(we)
	# Snow: a winter baked over the whole world (SnowClimate: snow line, shaded faces, loaded lee slopes, cornices),
	# carried on by the weather; a finer near square follows the camera.
	snow = SnowCover3D.new()
	snow.name = "Snow"
	snow.extent = float(args.get("snow-extent", "1024"))
	snow.resolution = 256
	add_child(snow)
	snow.height_source = snow.get_path_to(get_node("GroundHeight"))
	snow.sun_light = snow.get_path_to(sun)
	if args.get("snow", "climate") == "climate":
		pass # the winter is baked once the data map has the terrain's climate (_bake_snow_winter())
	else:
		snow.extent = 4096.0
		snow.follow_target = snow.get_path_to(get_node("WorldAnchor"))
	cornices = SnowCornice3D.new()
	cornices.name = "Cornices"
	add_child(cornices)
	# Falling and blowing snow around the camera, from the same snowpack and weather.
	snowfall = SnowFall3D.new()
	snowfall.name = "Snowfall"
	add_child(snowfall)
	snowfall.snow_cover = snowfall.get_path_to(snow)
	snow.global_position = Vector3(0, 0, 0)


func _bake_snow_winter() -> void:
	# The snowpack's winter from the same ClimateRecipe as the rivers, the data map and the plants: its
	# temperatures, wet-day wind and precipitation drive the winter generator, and the data map's orographic
	# precipitation field says where more of it falls.
	if args.get("snow", "climate") != "climate":
		return
	var winter := SnowClimate.new()
	winter.recipe = climate
	snow.climate_map = snow.get_path_to(data_map)
	snow.climate = winter
	var t := Time.get_ticks_msec()
	snow.bake_climate(Rect2(-2048, -2048, 4096, 4096), int(args.get("climate-res", "256")))
	_log("snow climate baked in %d ms" % (Time.get_ticks_msec() - t))
	var sw: PackedFloat32Array = winter.swe
	var mx := 0.0
	var nz := 0
	for v in sw:
		mx = maxf(mx, v)
		if v > 1.0: nz += 1
	var cx := 0.0
	for v in winter.cornice_x: cx = maxf(cx, absf(v))
	cornice_climate_max = cx
	_log("climate swe max %.1f mm, %d of %d cells with snow, cornice max %.2f m, has_world %s" % [mx, nz, sw.size(), cx, snow.has_world()])


func _weather_settings() -> void:
	# One weather state drives all of it. Storm: cold wet air, a storm cell over the hills, strong wind from the sea.
	var storm: bool = args.get("weather", "storm") == "storm"
	weather.wind_direction = Vector2(-0.2, -1.0).normalized() # onshore, from the sea up the valleys
	weather.wind_speed = float(args.get("wind", "13" if storm else "4"))
	weather.cloud_cover = 0.85 if storm else 0.3
	weather.cloud_type = 0.85 if storm else 0.4
	weather.rain_rate = 8.0 if storm else 0.0
	weather.temperature = 5.0 if storm else 12.0
	weather.humidity = 0.95 if storm else 0.6
	weather.snap_to_settings()
	if storm:
		weather.cloud_thickness = 2500.0 # a deep storm cell: about 1.6 flashes a minute
		weather.add_storm(Vector2(-300, -400), 1500.0, 20.0, 12.0, 0.0)
	# Lightning and thunder from the storm cells.
	lightning = WeatherLightning3D.new()
	lightning.name = "Lightning"
	add_child(lightning)


func _run_weather() -> void:
	var t := Time.get_ticks_msec()
	# A night and a day of weather: snow builds on the peaks, rain wets the lowlands.
	var hours := float(args.get("hours", "12"))
	for k in int(hours):
		weather.advance(3600.0)
		snow.advance(3600.0)
	_log("weather + snow %.0f h in %d ms" % [hours, Time.get_ticks_msec() - t])
	# Snowfall beyond the particle box is haze: SnowFall3D pushes it into the weather map's precipitation
	# extinction, which the volumetric fog reads; nothing is wired here.


# --- Vegetation --------------------------------------------------------------------------------------------------

func _build_vegetation() -> void:
	var t := Time.get_ticks_msec()
	data_map = TerrainDataMap3D.new()
	data_map.name = "DataMap"
	data_map.size = 4096.0
	data_map.resolution = int(args.get("map-res", "1024"))
	data_map.bake_on_ready = false
	add_child(data_map)
	data_map.position = Vector3(0, 0, 0)
	data_map.height_source = data_map.get_path_to(get_node("GroundHeight"))
	var ws: Array[NodePath] = []
	for r in rivers:
		ws.append(data_map.get_path_to(r))
	for w in water_nodes:
		ws.append(data_map.get_path_to(w))
	data_map.water_sources = ws
	data_map.geology_source = data_map.get_path_to(gen)
	data_map.climate_recipe = climate
	data_map.bake()
	if not data_map.has_climate():
		data_map.bake_climate()
	_log("data map bake %d ms" % (Time.get_ticks_msec() - t))
	_bake_snow_winter()
	t = Time.get_ticks_msec()
	# STAND-IN trees: tree generation is not built yet, so cones and balls mark where the forest grows.
	var conifer := PlantSpecies.new()
	conifer.meshes = [_tree_mesh(true, 14.0), _tree_mesh(true, 10.0)]
	conifer.spacing = 9.0
	conifer.density = 0.8
	conifer.footprint = 3.0
	conifer.canopy_radius = 3.0 # the cone's base (0.22 of 14 m)
	# Altitude belts from the climate, not from heights: conifers in the cool belt up to the tree line, broadleaves
	# in the mild lowlands where winters stay above -3 C (beech's frost hardiness).
	conifer.temperature_range = Vector2(6.5, 11.0)
	conifer.temperature_fade = 0.6
	conifer.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 32))
	conifer.set_rule_fade(PlantSpecies.RULE_SLOPE, 6.0)
	conifer.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(10, 1000000))
	conifer.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 5.0)
	conifer.set_rule_range(PlantSpecies.RULE_SOIL_DEPTH, Vector2(0.5, 100000))
	conifer.set_rule_fade(PlantSpecies.RULE_SOIL_DEPTH, 0.1)
	conifer.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
	conifer.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	conifer.set_rule_range(PlantSpecies.RULE_TALUS, Vector2(0, 0.2))
	var broad := PlantSpecies.new()
	broad.meshes = [_tree_mesh(false, 11.0), _tree_mesh(false, 8.0)]
	broad.spacing = 10.0
	broad.density = 0.7
	broad.footprint = 3.5
	broad.canopy_radius = 3.3 # the crown ball (0.3 of 11 m)
	broad.temperature_range = Vector2(10.0, 30.0)
	broad.temperature_fade = 0.3
	broad.january_temperature_range = Vector2(-3.0, 100.0)
	broad.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(6, 100000)) # above the beach
	broad.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 25))
	broad.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
	broad.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(12, 1000000))
	broad.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 6.0)
	broad.set_rule_range(PlantSpecies.RULE_SOIL_DEPTH, Vector2(0.5, 100000))
	broad.set_rule_fade(PlantSpecies.RULE_SOIL_DEPTH, 0.1)
	broad.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
	broad.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	broad.set_rule_range(PlantSpecies.RULE_TALUS, Vector2(0, 0.2))
	broad.patch_scale = 300.0
	broad.patch_cover = 0.65
	var biome := Biome.new()
	biome.species = [conifer, broad]
	veg = VegetationField3D.new()
	veg.name = "Forest"
	veg.biome = biome
	veg.tile_size = 128.0
	veg.populate_on_ready = false
	add_child(veg)
	veg.data_map = veg.get_path_to(data_map)
	if args.get("layout", "range") == "valley":
		# The clearing: the trees keep off the alluvial floor, read from the generator's sediment (a floodplain
		# meadow). Built here by script from get_sediment_image (a GAP: the generator publishes no clearing mask).
		var sed: Image = gen.get_sediment_image()
		var mask := Image.create(sed.get_width(), sed.get_height(), false, Image.FORMAT_RGB8)
		var top := float(args.get("clearing-sediment", "1.0"))
		for j in sed.get_height():
			for i in sed.get_width():
				var d := sed.get_pixel(i, j).r
				mask.set_pixel(i, j, Color(1.0 - smoothstep(top * 0.3, top, d), 0, 0))
		veg.density_mask = ImageTexture.create_from_image(mask)
		var wr: Rect2 = gen.get_world_rect()
		veg.density_mask_rect = wr
		for sp in [conifer, broad]:
			sp.mask_channel = 0
		# Nor on the low coastal flat and the beach behind it.
		conifer.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(6, 100000))
	# The forest as shelter for the wind: the field writes its stands into the weather map's canopy.
	veg.weather_map = veg.get_path_to(weather)
	# --forest-radius=R: the forest streams round the camera instead of covering the world at once.
	var fr := float(args.get("forest-radius", "0"))
	if fr > 0.0:
		veg.streaming = true
		veg.stream_radius = fr
		_log("forest streams round the camera, %.0f m" % fr)
	else:
		veg.populate()
		_log("forest %d plants in %d ms" % [veg.get_plant_count(), Time.get_ticks_msec() - t])
	# Grass near the camera, on the same maps, moved by the weather's wind.
	var meadow := PlantSpecies.new()
	meadow.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 28))
	meadow.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(3, 1000000))
	meadow.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(3, 700))
	grass = GrassField3D.new()
	grass.name = "Grass"
	grass.rules = meadow
	grass.radius = float(args.get("grass-radius", "40"))
	add_child(grass)
	grass.data_map = grass.get_path_to(data_map)
	if args.get("plants", "1") != "0":
		# The sward in three layers (the grass bench's meadow): tall blades in patches, a short dense undercoat that
		# owns the ground tint, and a very short turf right at the camera (overlapped density grids, Serious Sam 4).
		meadow.patch_scale = 9.0
		meadow.patch_cover = 0.55
		meadow.patch_seed = 5
		grass.ground_tint_enabled = false
		var under := GrassField3D.new()
		under.name = "Undercoat"
		under.rules = meadow.duplicate()
		(under.rules as PlantSpecies).patch_cover = 1.0
		under.blade_segments = 2
		under.blade_height = Vector2(0.06, 0.18)
		under.blade_width = 0.02
		under.density = 640.0
		under.radius = 14.0
		under.tile_size = 4.0
		under.clump_size = 0.5
		under.lean = 0.5
		under.base_color = Color(0.1, 0.2, 0.05)
		under.tip_color = Color(0.38, 0.52, 0.16)
		under.dryness = 0.35
		under.ground_tint = Color(0.05, 0.085, 0.025, 1.0)
		add_child(under)
		under.data_map = under.get_path_to(data_map)
		var turf := GrassField3D.new()
		turf.name = "Turf"
		turf.rules = under.rules
		turf.blade_segments = 1
		turf.blade_height = Vector2(0.03, 0.07)
		turf.blade_width = 0.012
		turf.density = 1600.0
		turf.radius = 5.0
		turf.tile_size = 2.0
		turf.clump_size = 0.3
		turf.lean = 0.6
		turf.base_color = Color(0.08, 0.16, 0.04)
		turf.tip_color = Color(0.3, 0.42, 0.12)
		turf.dryness = 0.35
		turf.ground_tint_enabled = false
		add_child(turf)
		turf.data_map = turf.get_path_to(data_map)
		sward = [grass, under, turf]
		# The sward thins out under the forest's crowns.
		for g in sward:
			var r: PlantSpecies = g.rules
			r.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.3))
			r.set_rule_fade(PlantSpecies.RULE_SHADE, 0.4)
			g.canopy_field = g.get_path_to(veg)


# Footprints in the ground around the camera: sand on the beach, mud on wet banks, leaf litter under the forest, none
# on bare rock or under snow. The patch is drawn with the terrain's own material.
func _build_prints() -> void:
	prints_probe = Node3D.new()
	prints_probe.name = "PrintsProbe"
	add_child(prints_probe)
	prints = GroundSurface3D.new()
	prints.name = "Prints"
	prints.ground_type = GroundSurface3D.GROUND_SOIL
	prints.material = ground_mat
	add_child(prints)
	prints.height_source = prints.get_path_to(gen)
	prints.snow_cover = prints.get_path_to(snow)
	if args.has("test"):
		prints.follow_target = prints.get_path_to(prints_probe)
	# Where the sand is: the beach band above the sea (the same rule as TerrainMaterial3D's sand layer). Built here
	# by script: the generator does not publish a sand map.
	var n := 512
	var img := Image.create(n, n, false, Image.FORMAT_RGB8)
	var beach := 2.5
	if ground_mat is TerrainMaterial3D:
		beach = (ground_mat as TerrainMaterial3D).beach_height
	for j in n:
		for i in n:
			var x := -2048.0 + (i + 0.5) * 4096.0 / n
			var z := -2048.0 + (j + 0.5) * 4096.0 / n
			var h := height_at(x, z)
			var sand := 1.0 - smoothstep(beach * 0.6, beach * 1.4, h - gen.sea_level)
			if h < gen.sea_level - 1.0:
				sand = 0.0
			img.set_pixel(i, j, Color(sand, 0, 0))
	prints.type_map = img
	prints.type_map_rect = Rect2(-2048, -2048, 4096, 4096)


func _prints_at(p: Vector3) -> Dictionary:
	prints_probe.global_position = p
	for i in 4:
		await get_tree().physics_frame
		await get_tree().process_frame
	# The cells refresh every 2 s; refresh them now, over the weather's shelter square around the probe.
	weather.rebake_shelter()
	prints.litter_depth = prints.litter_depth
	var m: Dictionary = prints.get_material_at(p)
	var sink := prints.get_sinkage_at(p, 75.0, 0.035, 0.108)
	var h := prints.get_surface_height_at(p)
	prints.press_box(Vector3(p.x, h, p.z), Vector2(0.11, 0.29), 0.0, h, sink)
	m["sink"] = sink
	m["dent"] = prints.get_dent_at(p)
	return m


func _tree_mesh(conifer: bool, h: float) -> Mesh:
	var st := SurfaceTool.new()
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.07, 0.16, 0.08) if conifer else Color(0.14, 0.22, 0.07)
	mat.roughness = 0.85
	mat.set_feature(BaseMaterial3D.FEATURE_WETNESS, true)
	mat.wetness_porosity = 0.3
	mat.set_feature(BaseMaterial3D.FEATURE_SNOW, true)
	var m: Mesh
	if conifer:
		var cone := CylinderMesh.new()
		cone.top_radius = 0.0
		cone.bottom_radius = h * 0.22
		cone.height = h * 0.85
		cone.radial_segments = 8
		cone.rings = 1
		m = cone
		st.append_from(cone, 0, Transform3D(Basis(), Vector3(0, h * 0.15 + h * 0.425, 0)))
	else:
		var ball := SphereMesh.new()
		ball.radius = h * 0.3
		ball.height = h * 0.55
		ball.radial_segments = 10
		ball.rings = 5
		st.append_from(ball, 0, Transform3D(Basis(), Vector3(0, h * 0.62, 0)))
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.15
	trunk.bottom_radius = 0.25
	trunk.height = h * 0.4
	trunk.radial_segments = 6
	trunk.rings = 1
	st.append_from(trunk, 0, Transform3D(Basis(), Vector3(0, h * 0.2, 0)))
	var out := st.commit()
	out.surface_set_material(0, mat)
	return out


# --- Camera ------------------------------------------------------------------------------------------------------

func _vec3_arg(v: String) -> Vector3:
	return Vector3(float(v.get_slice(",", 0)), float(v.get_slice(",", 1)), float(v.get_slice(",", 2)))


func _build_camera() -> void:
	cam = Camera3D.new()
	cam.far = 20000.0
	cam.near = 0.5
	cam.fov = 55.0
	add_child(cam)
	var view: String = args.get("view", "wide")
	if view in ["forestfloor", "woodedge"] and veg.streaming and _broadleaf_trees().is_empty():
		# The streamed forest has not placed yet: stand at the clearing's edge first, framed again once it has.
		view = "meadow"
	var coast_z := _coast_z(0.0)
	match view:
		"lake":
			# The generator puts the lake anywhere: look at it from the open side inside the world, high enough that
			# no ground stands between the camera and the water.
			var best_pos := Vector3.ZERO
			var best_y := INF
			for k in 16:
				var a := TAU * k / 16.0
				var d := Vector3(cos(a), 0, sin(a))
				var c := lake_center + d * 260.0
				if absf(c.x) > 1880.0 or c.z < -1880.0 or c.z > 2900.0:
					continue
				var top := lake_center.y
				for t in range(1, 21):
					var q := lake_center.lerp(c, t / 20.0)
					top = maxf(top, height_at(q.x, q.z) + 12.0 * t / 20.0)
				var y := maxf(top + 25.0, lake_center.y + 60.0)
				if y < best_y:
					best_y = y
					best_pos = Vector3(c.x, y, c.z)
			cam.position = best_pos
			cam.look_at(lake_center)
		"boulders":
			# Over the densest patch of fallen blocks, looking back up at the cliff they came from.
			var bs := Vector3(boulder_spot.x, height_at(boulder_spot.x, boulder_spot.z), boulder_spot.z)
			var up := Vector3.ZERO
			for k in 8:
				var a := TAU * k / 8.0
				var d := Vector3(cos(a), 0, sin(a))
				var hh := height_at(bs.x + d.x * 60.0, bs.z + d.z * 60.0)
				if hh - bs.y > up.y:
					up = Vector3(d.x, hh - bs.y, d.z)
			var away := -Vector3(up.x, 0, up.z).normalized() if up.y > 0.0 else Vector3(0, 0, 1)
			var gb := gen.get_height(bs)
			cam.fov = 50.0
			cam.position = Vector3(bs.x, gb + 45.0, bs.z) + away * 25.0
			cam.look_at(Vector3(bs.x, gb, bs.z))
		"valley":
			# From above the falls, down over the high lake's outflow; the clearing and the sea beyond.
			var lk := Vector3(lake_center.x, 0, lake_center.z)
			var dir := Vector3(0, 0, 1)
			var c := lk - dir * float(args.get("valley-back", "350"))
			cam.fov = 60.0
			cam.position = Vector3(c.x, maxf(height_at(c.x, c.z) + 40.0, lake_level + float(args.get("valley-up", "140"))), c.z)
			cam.look_at(Vector3(lk.x, lake_level - 220.0, lk.z + 900.0))
		"treeline":
			# Where the clearing's mask meets the forest: walk north up the valley from the plain until the sediment
			# thins out, then look at that edge from the grass.
			var ex := float(args.get("treeline-x", "300"))
			var ez := 400.0
			while ez > -1500.0 and gen.get_sediment_depth(Vector3(ex, 0, ez)) > 0.4:
				ez -= 16.0
			cam.fov = 50.0
			var cz := ez + 220.0
			cam.position = Vector3(ex - 60.0, height_at(ex - 60.0, cz) + 14.0, cz)
			cam.look_at(Vector3(ex, height_at(ex, ez) + 10.0, ez))
			_log("treeline edge at (%.0f, %.0f)" % [ex, ez])
		"forestfloor":
			# Inside a broad-leaved stand at kneeling height, looking 14 m along the floor under closed crowns.
			var trees2 := _broadleaf_trees()
			var fpick := Vector3.INF
			var fdir := Vector3.ZERO
			var fbest := -1.0
			for tp in trees2:
				if tp.y < 3.0:
					continue
				for k in 8:
					var d := Vector3(cos(TAU * k / 8.0), 0, sin(TAU * k / 8.0))
					var sh := minf(veg.get_stand_shade_at(tp + d * 4.0), veg.get_stand_shade_at(tp + d * 14.0))
					if sh > fbest and rad_to_deg(data_map.get_slope_at(tp + d * 4.0)) < 12.0:
						fbest = sh
						fpick = tp
						fdir = d
				if fbest > 0.8:
					break
			if fpick == Vector3.INF:
				fpick = Vector3(0, height_at(0, 0), 0)
				fdir = Vector3(0, 0, 1)
			var cf := fpick + fdir * 4.0 + Vector3(-fdir.z, 0, fdir.x) * 1.5
			cf.y = height_at(cf.x, cf.z) + 1.1
			var lf := fpick + fdir * 14.0
			lf.y = height_at(lf.x, lf.z) + 0.2
			cam.fov = 65.0
			cam.position = cf
			cam.look_at(lf)
			_log("forestfloor camera at %s, stand shade %.2f, litter %.2f" % [str(cf), fbest, veg.get_litter_at(lf)])
		"woodedge":
			# At a broad-leaved wood's edge at eye height, 9 m out from a rim tree, looking at the leaf drift that thins
			# past its crowns (get_litter_at), on open ground with no stand shade behind the camera.
			var trees := _broadleaf_trees()
			var pick := Vector3.INF
			var pdir := Vector3.ZERO
			var best_h := INF
			for tp in trees:
				if tp.y < 3.0 or tp.y > best_h:
					continue
				for k in 16:
					var d := Vector3(cos(TAU * k / 16.0), 0, sin(TAU * k / 16.0))
					var q := tp + d * 12.0
					if veg.get_stand_shade_at(q) > 0.05 or veg.get_litter_at(tp + d * 3.0) < 0.3:
						continue
					if rad_to_deg(data_map.get_slope_at(q)) > 15.0:
						continue
					pick = tp
					pdir = d
					best_h = tp.y
					break
			if pick == Vector3.INF:
				_log("woodedge: no open rim on a broad-leaved stand, falling back to the meadow view")
				pick = Vector3(0, height_at(0, 0), 0)
				pdir = Vector3(0, 0, 1)
			var ce := pick + pdir * 9.0
			ce.y = height_at(ce.x, ce.z) + 1.6
			var la := pick + pdir * 2.0
			la.y = height_at(la.x, la.z) + 0.4
			cam.fov = 60.0
			cam.position = ce
			cam.look_at(la)
			_log("woodedge camera at %s, litter %.2f at the rim, %.2f under the camera" % [str(ce), veg.get_litter_at(pick + pdir * 3.0), veg.get_litter_at(ce)])
		"meadow":
			# Standing in the clearing at eye height, 60 m short of the forest edge (the treeline view's edge), looking
			# across the herbs and grass at it.
			var ex := float(args.get("treeline-x", "300"))
			var ez := 400.0
			while ez > -1500.0 and gen.get_sediment_depth(Vector3(ex, 0, ez)) > 0.4:
				ez -= 16.0
			cam.fov = 55.0
			var cz := ez + 60.0
			cam.position = Vector3(ex - 10.0, height_at(ex - 10.0, cz) + 1.7, cz)
			cam.look_at(Vector3(ex, height_at(ex, ez) + 4.0, ez))
		"lakewater":
			# Low over the water at one end of the lake, looking along it to the mountains behind: the water cell
			# farthest from the water's centroid, pulled a little back over the water.
			var cells: Array[Vector3] = []
			var span := maxf(lake_axes.x, 60.0) * 1.6
			var gx := -span
			while gx <= span:
				var gz := -span
				while gz <= span:
					var q := Vector3(lake_center.x + gx, 0, lake_center.z + gz)
					if height_at(q.x, q.z) < lake_level - 1.0:
						cells.append(q)
					gz += 6.0
				gx += 6.0
			var mid := Vector3(lake_center.x, 0, lake_center.z)
			if cells.size() > 0:
				mid = Vector3.ZERO
				for q in cells:
					mid += q
				mid /= cells.size()
			# The end facing away from the sun, so the camera looks into the sun's glitter on the ripples.
			var away := Vector3.ZERO
			if clock != null:
				var sd := clock.get_sun_direction()
				away = -Vector3(sd.x, 0, sd.z).normalized()
			var far := mid
			var far_score := -INF
			for q in cells:
				var score := q.distance_to(mid) + 1.5 * (q - mid).dot(away)
				if score > far_score:
					far_score = score
					far = q
			var c := far.lerp(mid, 0.12)
			cam.fov = 50.0
			cam.position = Vector3(c.x, lake_level + 5.0, c.z)
			var ahead := mid + (mid - c) * 1.5
			cam.look_at(Vector3(ahead.x, lake_level + 22.0, ahead.z))
			_log("lakewater camera over %d water cells, %.0f m from the far end" % [cells.size(), c.distance_to(mid) * 2.0])
		"lip":
			# Close on the fall's face: from the plunge pool side, 50 m out, looking at the middle of the drop.
			var fd3l := Vector3(fall_dir.x, 0, fall_dir.y)
			var c := fall_point + fd3l * 70.0 + Vector3(12, 0, 0)
			c.y = height_at(c.x, c.z) + 25.0
			cam.position = c
			cam.look_at(fall_point + fd3l * 18.0 + Vector3(0, -30, 0))
		"fall":
			# Downstream of the fall, at the first spot with a clear view of the whole drop (gorges hide it).
			var fd3 := Vector3(fall_dir.x, 0, fall_dir.y)
			var side := Vector3(-fall_dir.y, 0, fall_dir.x)
			# Framed by the drop: far enough out for the whole height (a 330 m fall wants half a kilometre), the
			# lens narrowed to fit it.
			var h := maxf(fall_height, 20.0)
			var mid := fall_point + Vector3(0, -0.45 * h, 0)
			var best := Vector3.ZERO
			var found := false
			for k in [1.6, 2.2, 3.0, 4.0]:
				var dist: float = maxf(90.0, k * h)
				for off in [0.0, 0.15, -0.15, 0.3, -0.3]:
					var c: Vector3 = fall_point + fd3 * dist + side * off * dist
					c.y = height_at(c.x, c.z) + maxf(10.0, 0.1 * h)
					var clear := true
					for t in range(1, 30):
						var q := c.lerp(mid, t / 30.0)
						if height_at(q.x, q.z) > q.y - 1.0:
							clear = false
							break
					if clear:
						best = c
						found = true
						break
				if found:
					break
			if not found:
				best = fall_point + fd3 * maxf(200.0, 3.0 * h)
				best.y = height_at(best.x, best.z) + maxf(60.0, 0.3 * h)
			cam.position = best
			cam.look_at(mid)
			cam.fov = clampf(rad_to_deg(2.0 * atan(0.75 * h / best.distance_to(mid))), 25.0, 70.0)
		"contact":
			# One contact fall (--contact=id, default the one with most water), seen from below and in front.
			var want: String = args.get("contact", "")
			var pick := {}
			for f in contact_falls:
				if (want != "" and f["id"] == want) or (want == "" and (pick.is_empty() or f["discharge"] * f["height"] > pick["discharge"] * pick["height"])):
					pick = f
			for g in gen.get_waterfalls():
				if not pick.is_empty() and g.get("id", "") == pick["id"]:
					var top: Vector3 = g["top"]
					var bot: Vector3 = g["bottom"]
					var d: Vector3 = g.get("direction", Vector3(0, 0, 1))
					d.y = 0
					d = d.normalized()
					var h := top.y - bot.y
					# --contact-look=f: aim at that fraction of the drop up from the foot (default 0.45).
					var mid := bot.lerp(top, float(args.get("contact-look", "0.45")))
					var side := Vector3(-d.z, 0, d.x)
					var c := bot + d * maxf(h * 1.6, 25.0) + Vector3(0, h * 0.3, 0)
					var found := false
					# The first spot in front of the face with a clear line to its middle (gorges hide it).
					for lift in [0.0, 0.5, 1.0, 2.0]:
						# --contact-dist=m: a close-up from about that far (else two to four and a half drops away).
						var ks := [2.0, 2.6, 3.4, 4.5]
						if args.has("contact-dist"):
							var k0 := float(args["contact-dist"]) / maxf(h, 1.0)
							ks = [k0, k0 * 1.25, k0 * 1.6, k0 * 2.0]
						for k in ks:
							for o in [0.35, -0.35, 0.0, 0.7, -0.7]:
								if found:
									continue
								var q: Vector3 = bot + d * h * k + side * h * k * o
								q.y = maxf(gen.get_height(q) + 5.0, mid.y - h * 0.1) + h * lift
								var clear := true
								for t in range(1, 20):
									var p := q.lerp(mid, t / 20.0)
									if gen.get_height(p) > p.y - 1.0:
										clear = false
										break
								if clear:
									c = q
									found = true
					cam.position = c
					cam.look_at(mid)
					cam.fov = clampf(rad_to_deg(2.0 * atan(0.8 * h / c.distance_to(mid))), 25.0, 70.0)
					if args.has("contact-fov"):
						cam.fov = float(args["contact-fov"])
					if args.has("contact-cam"):
						# --contact-cam=back,up: straight down the fall's own direction from the foot, back and up (m).
						var bu: PackedStringArray = args["contact-cam"].split(",")
						c = bot + d * float(bu[0]) + Vector3(0, float(bu[1]), 0)
						cam.position = c
						cam.look_at(mid)
						cam.fov = 60.0
					_log("contact view of %s from %s, top %s" % [pick["id"], c, top])
					for r in rivers:
						for rf in r.get_waterfalls():
							_log("  river %s fall at %s, %.1f m, %d exclusions" % [r.name, rf["lip"], rf["height"], r.fall_exclusions.size()])
		"beach":
			cam.fov = 60.0
			cam.position = Vector3(40.0, 0.0, coast_z - 60.0)
			cam.position.y = height_at(cam.position.x, cam.position.z) + 2.0
			cam.look_at(Vector3(-60.0, 0.0, coast_z + 140.0))
		"peak":
			var top := _highest_point()
			var off := Vector3(-120, 0, 160)
			cam.position = top + off
			cam.position.y = height_at(cam.position.x, cam.position.z) + 3.0
			cam.look_at(top + Vector3(0, -40, 0))
		_:
			cam.position = Vector3(300, 520, coast_z + 900.0)
			cam.look_at(Vector3(-100, 250, -700))
	if args.has("cam") and args.has("look"):
		# A fixed shot for before/after pairs: --cam=x,y,z --look=x,y,z (y of either below 0: that far above the ground).
		var cv := _vec3_arg(args["cam"])
		var lv := _vec3_arg(args["look"])
		if cv.y < 0.0:
			cv.y = height_at(cv.x, cv.z) - cv.y
		if lv.y < 0.0:
			lv.y = height_at(lv.x, lv.z) - lv.y
		cam.position = cv
		cam.look_at(lv)
	for g in sward:
		g.update_at(cam.global_position)
	if sward.is_empty():
		grass.update_at(cam.global_position)


# Herbs, flowers and waterside plants from PlantGenerator, streamed round the camera on the world's data map
# (VegetationField3D.streaming: tiles placed on workers out to stream_radius, each species to its visibility range).
func _build_cover() -> void:
	var t := Time.get_ticks_msec()
	var b := Biome.new()
	var meadow_sp := _meadow_species()
	for sp in meadow_sp:
		# Meadow plants keep out from under closed crowns (the forest field's, through canopy_field).
		sp.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.4))
		sp.set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
	# The forest floor under the forest field's crowns: the montane conifer set (spruce, fir, beech: the stand-in
	# cones and balls), bigger plants first, sorted by the crowns' conifer share.
	# --floor=beech|oak|hedgerow|montane picks the floor set (beech and oak floors carry the violets and herb robert).
	var floors := {"montane": PlantGenerator.FLOOR_MONTANE_CONIFER, "beech": PlantGenerator.FLOOR_BEECH,
			"oak": PlantGenerator.FLOOR_OAK_MIXED, "hedgerow": PlantGenerator.FLOOR_HEDGEROW}
	var floor_sp: Array = PlantGenerator.make_floor(floors.get(args.get("floor", "montane"), PlantGenerator.FLOOR_MONTANE_CONIFER), 3)
	b.species = floor_sp.slice(0, 3) + _wetland_species() + meadow_sp + floor_sp.slice(3)
	for sp in b.species:
		var ps := sp as PlantSpecies
		# Plants on soil, not on bare rock, scree or steep faces, and not on the beach sand.
		ps.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
		ps.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
		if not ps.floating:
			ps.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 32))
			ps.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
		ps.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(3, 100000))
		if not ps.has_meta("floor"):
			_set_far_color(ps)
	cover = VegetationField3D.new()
	cover.name = "Cover"
	cover.tile_size = 8.0
	cover.biome = b
	cover.forest_floor_enabled = false # the forest's own field publishes the floor
	cover.streaming = true
	cover.stream_radius = 80.0 # the herbs draw to 10 to 60 m
	add_child(cover)
	cover.data_map = cover.get_path_to(data_map)
	cover.canopy_field = cover.get_path_to(veg) # shade and conifer share from the forest's crowns
	_log("cover: %d species streaming round the camera, set up in %d ms" % [b.species.size(), Time.get_ticks_msec() - t])


func _variants(preset: int, count: int) -> Array:
	var pg := PlantGenerator.new()
	pg.preset = preset
	pg.apply_preset_colors()
	var meshes := pg.generate_variants(count)
	for m in meshes:
		m.set_meta("preset", preset)
	return meshes


# Far away the ground takes the plant's colour (PlantSpecies.far_color, as make_floor() sets it).
func _set_far_color(sp: PlantSpecies) -> void:
	if sp.meshes.is_empty() or sp.far_color.a > 0.0:
		return
	var m: Mesh = sp.meshes[0]
	var pg := PlantGenerator.new()
	pg.preset = m.get_meta("preset", 0)
	pg.apply_preset_colors()
	var lc: Color = pg.leaf_color * 0.8
	sp.far_color = Color(lc.r, lc.g, lc.b, clampf(m.get_aabb().end.y / 0.6, 0.2, 0.9))


func _plant(presets: Array, spacing: float, footprint: float, density: float, range_m: float, patch := 0.0, cover_share := 1.0, seed := 0) -> PlantSpecies:
	var sp := PlantSpecies.new()
	var ms: Array = []
	for pr in presets:
		ms += _variants(pr, 3)
	sp.meshes = ms
	sp.spacing = spacing
	sp.footprint = footprint
	sp.density = density
	sp.visibility_range = range_m
	if patch > 0.0:
		sp.patch_scale = patch
		sp.patch_cover = cover_share
		sp.patch_seed = seed
	return sp


# The meadow (the grass bench's set, docs/ideas/plant-generator.md): flowers and weeds in patches, tussocks with the
# tall grass, clover where it is not, last year's dead grass under everything, nettles on moist rich ground.
func _meadow_species() -> Array:
	var flowers := _plant([PlantGenerator.PRESET_DAISY, PlantGenerator.PRESET_BUTTERCUP, PlantGenerator.PRESET_DANDELION], 0.25, 0.12, 0.9, 15.0, 6.0, 0.35, 11)
	flowers.scale_range = Vector2(1.1, 1.5)
	var plantain := _plant([PlantGenerator.PRESET_PLANTAIN], 0.5, 0.15, 0.5, 12.0, 4.0, 0.35, 21)
	var clover := _plant([PlantGenerator.PRESET_CLOVER], 0.22, 0.0, 0.8, 10.0, 9.0, 0.55, 5)
	clover.patch_invert = true
	var tussock := _plant([PlantGenerator.PRESET_COCKSFOOT, PlantGenerator.PRESET_TIMOTHY], 0.7, 0.2, 0.45, 25.0, 9.0, 0.55, 5)
	var nettle := _plant([PlantGenerator.PRESET_NETTLE], 0.35, 0.15, 0.85, 25.0, 5.0, 0.22, 31)
	nettle.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(1.0, 25.0))
	nettle.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 4.0)
	var dock := _plant([PlantGenerator.PRESET_DOCK], 0.9, 0.25, 0.5, 20.0, 7.0, 0.35, 41)
	var thistle := _plant([PlantGenerator.PRESET_THISTLE], 1.6, 0.25, 0.35, 20.0)
	var yarrow := _plant([PlantGenerator.PRESET_YARROW], 0.5, 0.1, 0.6, 15.0, 5.0, 0.3, 51)
	yarrow.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6.0, 1000000.0))
	yarrow.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	var dry := _plant([PlantGenerator.PRESET_DRY_GRASS], 0.3, 0.0, 0.55, 12.0)
	dry.scale_range = Vector2(0.7, 1.2)
	var list := [flowers, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry]
	return list


# Hydrosere zonation (Spence 1982; Keddy 2010): lilies in deeper water, bulrush and reed in the shallows, sedge and
# soft rush on the wet shore. Depth and distance from the data map.
func _wetland_species() -> Array:
	var lily := _plant([PlantGenerator.PRESET_WATER_LILY], 1.1, 0.5, 0.9, 50.0, 6.0, 0.7, 81)
	lily.floating = true
	lily.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.35, 2.5))
	lily.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.2)
	var cattail := _plant([PlantGenerator.PRESET_CATTAIL], 0.7, 0.35, 0.9, 60.0, 5.0, 0.5, 83)
	cattail.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.1, 0.6))
	cattail.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.05)
	var reed := _plant([PlantGenerator.PRESET_REED], 0.6, 0.3, 0.95, 60.0, 8.0, 0.6, 85)
	reed.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.0, 1.2))
	reed.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.15)
	var rush := _plant([PlantGenerator.PRESET_RUSH], 0.5, 0.2, 0.8, 30.0, 4.0, 0.5, 87)
	rush.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(0.0, 4.0))
	rush.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.5)
	var sedge := _plant([PlantGenerator.PRESET_SEDGE], 0.45, 0.2, 0.8, 30.0)
	sedge.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(0.0, 3.0))
	sedge.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.5)
	return [lily, cattail, reed, rush, sedge]


func _suit_one(rule: int, base: PlantSpecies) -> float:
	var sp := PlantSpecies.new()
	sp.set_rule_range(rule, base.get_rule_range(rule))
	sp.set_rule_fade(rule, base.get_rule_fade(rule))
	return sp.get_suitability_at(data_map, _dbg_pt)


var _dbg_pt := Vector3.ZERO


# --- Links -------------------------------------------------------------------------------------------------------

func _checks() -> void:
	if args.has("wind-snow-only"):
		await _wind_snow_check()
		return
	var peak := _highest_point()
	# Terrain -> rivers, lake, sea
	check(rivers.size() >= 3, "terrain generator gives a river network (%d rivers placed)" % rivers.size())
	var lip := 0
	for r in rivers:
		lip += r.get_waterfalls().size()
		if args.has("falls"):
			for f in r.get_waterfalls():
				print("fall on %s: lip %s impact %s height %.1f m q %.2f lip_speed %.2f breakup %.1f" % [r.name, f.lip, f.impact, f.height, f.discharge, f.lip_speed, f.breakup_length])
	if args.has("falls"):
		for r in rivers:
			var c: Curve3D = r.curve
			var steep := 0.0
			var at := Vector3()
			for k in range(1, c.point_count):
				var a := c.get_point_position(k - 1)
				var b := c.get_point_position(k)
				var run := Vector2(b.x - a.x, b.z - a.z).length()
				if run > 0.01 and (a.y - b.y) / run > steep:
					steep = (a.y - b.y) / run
					at = a
			print("river %s: %d points, q %.2f, steepest step %.2f at %s, %d falls" % [r.name, c.point_count, r.discharge, steep, at, r.get_waterfalls().size()])
	check(gen_falls.size() > 0 and lip > 0, "the generator's rock bands make waterfalls and the rivers fall there (%d from the generator, %d lips on the rivers)" % [gen_falls.size(), lip])
	# Waterfall mist: each river makes it at its falls, the weather blows it, and it turns the volumetric fog on itself.
	var mists: Array[Node] = []
	for r in rivers:
		mists.append_array(r.find_children("*", "PhysXWaterMist3D", true, false))
	var mist_max := 0.0
	for m in mists:
		m.advance(20.0)
		mist_max = maxf(mist_max, m.get_max_density())
	await get_tree().process_frame
	check(mists.size() > 0 and mist_max > 0.0 and env.volumetric_fog_enabled, "waterfall mist rises at the falls and shows with no fog set by hand (%d mists, densest %.2f g/m3, volumetric fog %s)" % [mists.size(), mist_max, "on" if env.volumetric_fog_enabled else "off"])
	if args.has("probe"):
		for z in range(1000, 2049, 40):
			var pp := Vector3(0, 0, z)
			print("  z %d  mine %.2f  gen %.2f  water %.2f  depth %.2f" % [z, height_at(0, z), gen.get_height(pp), gen.get_water_level(pp), gen.get_water_depth(pp)])
		for w in lakes:
			var aabb: AABB = w.surface_mesh.get_aabb() if w.surface_mesh else AABB()
			print("  lake node %s pos %s level %.2f mesh aabb %s wet(center) %s" % [w.name, w.global_position, w.water_level, aabb, w.is_wet(w.global_position + aabb.get_center())])
		for l in gen.get_lake_results():
			print("  result ", l)
		for d in gen.get_rivers(float(args.get("min-area", "1500000"))):
			var pts: PackedVector3Array = d["points"]
			print("  river from_lake %s ends_in %s lake %s joins %s area %.2f km2 head %s mouth %s" % [d.get("from_lake"), d.get("ends_in"), d.get("lake"), d.get("joins"), d["areas"][d["areas"].size() - 1] / 1e6, pts[0], pts[pts.size() - 1]])
		for x in [-1800.0, -1000.0, 0.0, 1000.0, 1800.0]:
			var z := 0.0
			while z < 2040.0 and is_nan(gen.get_water_level(Vector3(x, 0, z))):
				z += 4.0
			print("  coast x %d at z %d: inland 10/20/40/80/120 m: %.2f %.2f %.2f %.2f %.2f" % [x, z, height_at(x, z - 10), height_at(x, z - 20), height_at(x, z - 40), height_at(x, z - 80), height_at(x, z - 120)])
	# Coast: a sea with a beach and a seabed deep enough for the open-sea waves.
	check(ocean != null and gen.has_sea_result(), "the generator makes the sea and build_water() places it")
	# Along the coast: share of it with a low beach (under 3 m, 20 m inland), and the depth 300 m out.
	var low := 0
	var n_c := 0
	var depth_sum := 0.0
	for k in 37:
		var x := -1800.0 + k * 100.0
		var cz := _coast_z(x)
		if cz >= 2040.0:
			continue
		n_c += 1
		var b := height_at(x, cz - 40.0)
		if b > 0.3 and b < 4.0:
			low += 1
		depth_sum += -height_at(x, minf(cz + 300.0, 2040.0))
	var beach_share := float(low) / maxf(n_c, 1)
	check(beach_share > 0.3 and depth_sum / maxf(n_c, 1) > 2.0, "beaches rise from the sea (%d%% of %d coast points 0.3 to 4 m high 40 m inland; seabed %.1f m deep 300 m out on average)" % [int(beach_share * 100.0), n_c, depth_sum / maxf(n_c, 1)])
	check(ocean != null and ocean.shore_waves and ocean.seabed_from_floor, "the sea has shore waves on the measured seabed")
	check(ocean != null and ocean.follow_weather and ocean.open_sea_beyond and ocean.render_extent >= 8000.0 and ocean.wave_cascades >= 3, "the generated sea follows the weather and runs past the world with no hand settings (extent %.0f m, %d cascades)" % [ocean.render_extent if ocean else 0.0, ocean.wave_cascades if ocean else 0])
	var lakes_follow := 0
	for l in lakes:
		if l.follow_weather:
			lakes_follow += 1
	check(lakes_follow == lakes.size(), "lake surfaces follow the weather wind (%d of %d)" % [lakes_follow, lakes.size()])
	# Lakes: surfaces at their levels, rivers in and out.
	var lr: Array = gen.get_lake_results()
	check(lakes.size() == lr.size() and lr.size() > 0, "the generator makes lakes and build_water() places a surface on each (%d)" % lakes.size())
	var lake_ok := 0
	for l in lr:
		var y: float = -INF
		for w in lakes:
			var h: float = w.sample_height(l["center"])
			if is_finite(h):
				y = h
		if is_finite(y) and absf(y - float(l["level"])) < 0.5:
			lake_ok += 1
		else:
			_log("  lake at %s level %.2f: water there %.2f, generator says %.2f" % [l["center"], l["level"], y, gen.get_water_level(l["center"])])
	check(lake_ok == lr.size(), "each lake's water stands at its level over its centre (%d of %d)" % [lake_ok, lr.size()])
	var to_sea := 0
	var to_lake := 0
	var from_lake := 0
	var dangling := 0
	for r in rivers:
		var tgt: Node = r.get_node_or_null(r.mouth_target)
		if tgt == ocean:
			to_sea += 1
		elif tgt in lakes:
			to_lake += 1
		elif tgt == null:
			dangling += 1
	for d in gen.get_rivers(float(args.get("min-area", "1500000"))):
		if d.get("from_lake", -1) >= 0:
			from_lake += 1
	check(to_sea > 0 and dangling == 0, "every river ends somewhere: %d in the sea, %d in lakes, %d joins, %d nowhere" % [to_sea, to_lake, rivers.size() - to_sea - to_lake - dangling, dangling])
	var outlets := 0
	for l in lr:
		if l.has("outlet"):
			outlets += 1
	_log("lakes with an outlet: %d" % outlets)
	check(to_lake > 0 or lr.is_empty(), "rivers flow into the lakes (%d)" % to_lake)
	check(from_lake >= outlets, "an outflow river leaves each lake with an outlet (%d rivers for %d outlets)" % [from_lake, outlets])
	# Weather -> everything
	var wet := weather.get_wetness_at(Vector3(0, 0, 900))
	check(wet > 0.05, "rain wets the lowlands (wetness %.2f)" % wet)
	var rain := weather.get_rain_at(Vector3(-300, 0, -400))
	check(rain > 1.0, "storm rains over the hills (%.1f mm/h)" % rain)
	var snow_peak := _snow_depth(peak)
	var snow_coast := _snow_depth(Vector3(0, 0, 1200))
	check(snow_peak > 0.02 and snow_coast < 0.005, "snow lies on the peaks (%.2f m at %.0f m) and not on the coast (%.3f m)" % [snow_peak, peak.y, snow_coast])
	# Snow line from the baked winter: mean depth by height band over the world tier.
	if snow.has_world():
		var bands := [0.0, 0.0, 0.0, 0.0]
		var counts := [0, 0, 0, 0]
		for k in 4000:
			var x := -2000.0 + (k % 80) * 50.0
			var z := -2000.0 + (k / 80) * 70.0
			var h := height_at(x, z)
			if h < 1.0:
				continue
			var b := clampi(int(h / 250.0), 0, 3)
			bands[b] += _snow_depth(Vector3(x, h, z))
			counts[b] += 1
		var txt := ""
		for b in 4:
			txt += "%d-%d m: %.2f m  " % [b * 250, b * 250 + 250, bands[b] / maxf(counts[b], 1)]
		_log("snow by height: " + txt)
		check(bands[3] / maxf(counts[3], 1) > bands[0] / maxf(counts[0], 1) + 0.1, "the baked winter gives a snow line: deeper with height (" + txt.strip_edges() + ")")
	else:
		check(false, "SnowCover3D world tier from bake_climate", false)
	# Cornices on the lee crests near the peak.
	var cprobe := Node3D.new()
	add_child(cprobe)
	cprobe.global_position = peak
	cornices.follow_target = cornices.get_path_to(cprobe)
	cornices.rebuild()
	if cornice_climate_max < 0.3:
		# A mild winter (the valley layout's crests stay under 1000 m): the climate itself builds no cornices, so
		# none is right; the check only applies where the baked winter grows them.
		_log("cornices: none expected, the baked winter's largest is %.2f m (%d pieces)" % [cornice_climate_max, cornices.get_segment_count()])
	else:
		check(cornices.get_segment_count() > 0, "cornices grow on the lee crests around the peak (%d pieces)" % cornices.get_segment_count())
	cornices.follow_target = NodePath("")
	check(ocean.follow_weather and absf(ocean.wind_speed - weather.get_mean_wind_at(ocean.global_position).length()) < 3.0, "sea state follows the weather wind (%.1f m/s vs %.1f)" % [ocean.wind_speed, weather.get_mean_wind_at(ocean.global_position).length()])
	# The sun from the clock: the date and place point the light, nothing placed by hand.
	var sun_to := sun.global_transform.basis.z
	check(clock != null and sun_to.dot(clock.get_sun_direction()) > 0.9995 and clock.get_sun_altitude() > 0.0, "the sun stands where SkyClock puts it for %02d-%02d %.2f h at %.0f N (altitude %.1f, azimuth %.1f deg)" % [clock.month, clock.day, clock.time_of_day, clock.latitude, clock.get_sun_altitude(), clock.get_sun_azimuth()])
	# Whitecaps from the same wind (Monahan & O'Muircheartaigh 1980, W = 3.84e-6 U^3.41): no hand-set foam threshold.
	var wc_u := weather.get_mean_wind_at(ocean.global_position).length()
	var wc_expect := minf(3.84e-6 * pow(wc_u, 3.41), 0.5)
	check(ocean.whitecap_auto and absf(ocean.get_whitecap_target_cover() - wc_expect) <= maxf(0.25 * wc_expect, 0.002), "whitecap cover follows the weather wind (%.2f%% at %.1f m/s, expected %.2f%%)" % [ocean.get_whitecap_target_cover() * 100.0, wc_u, wc_expect * 100.0])
	# Grass on a meadow (the wide camera hangs over the sea, where none grows): the nearest open, gentle, dry cell
	# around the lake, searched outward so the probe follows the terrain's realisation.
	var meadow_pt := lake_center
	var found := false
	for ring in range(1, 40):
		for k in 24:
			var a := TAU * k / 24.0
			var q := lake_center + Vector3(cos(a), 0, sin(a)) * (lake_axes.y + ring * 20.0)
			q.y = height_at(q.x, q.z)
			if q.y > 5.0 and q.y < 650.0 and rad_to_deg(data_map.get_slope_at(q)) < 20.0 and data_map.get_water_distance_at(q) > 6.0 and data_map.get_rock_at(q) < 0.2 and weather.get_canopy_closure_at(q) < 0.1:
				meadow_pt = q
				found = true
				break
		if found:
			break
	# Every layer of the sward counts: the tall blades grow in patches, the undercoat and turf everywhere.
	var layers: Array = sward if not sward.is_empty() else [grass]
	var g := 0
	var per := PackedStringArray()
	for f in layers:
		f.update_at(meadow_pt)
		g += f.get_blade_count()
		var suit: float = f.rules.get_suitability_at(data_map, meadow_pt) if f.rules else -1.0
		per.append("%s %d (suitability %.2f)" % [f.name, f.get_blade_count(), suit])
	_dbg_pt = meadow_pt
	if args.has("grass-debug") and not sward.is_empty():
		var base: PlantSpecies = (sward[1] as GrassField3D).rules.duplicate()
		var r0 := PlantSpecies.new()
		for rr in [PlantSpecies.RULE_SLOPE, PlantSpecies.RULE_WATER_DISTANCE, PlantSpecies.RULE_HEIGHT, PlantSpecies.RULE_SHADE]:
			r0.set_rule_range(rr, base.get_rule_range(rr))
			print("rule %d range %s fade %.2f alone -> %.2f" % [rr, base.get_rule_range(rr), base.get_rule_fade(rr), _suit_one(rr, base)])
		print("default species -> %.2f, data map shade %.2f slope %.1f deg water %.1f" % [PlantSpecies.new().get_suitability_at(data_map, meadow_pt), data_map.get_shade_at(meadow_pt) if data_map.has_method("get_shade_at") else -1.0, rad_to_deg(data_map.get_slope_at(meadow_pt)), data_map.get_water_distance_at(meadow_pt)])
	if veg.has_method("get_stand_shade_at"):
		per.append("stand shade %.2f at %s" % [veg.get_stand_shade_at(meadow_pt), str(meadow_pt.round())])
	check(g > 0, "grass grows from the data map's rules on a meadow (%d blades: %s); it bends with weather_wind" % [g, ", ".join(per)])
	for f in layers:
		f.update_at(cam.global_position)
	check(false, "trees and shrubs bend with the wind (stand-in forest is static; tree generation and bone wind not built)", true)
	# A dry point below the lake's level, far from it and from the rivers (on the coastal plain).
	var plain := Vector3.ZERO
	for k in 400:
		var c := Vector3(-1900.0 + (k % 20) * 200.0, 0.0, -1900.0 + (k / 20) * 200.0)
		c.y = height_at(c.x, c.z)
		if c.y > 3.0 and c.y < lake_level - 5.0 and Vector2(c.x - lake_center.x, c.z - lake_center.z).length() > 1000.0 and is_nan(gen.get_water_level(c)):
			plain = c
			break
	var wd := data_map.get_water_depth_at(plain)
	check(plain.y < lake_level and wd <= 0.0, "data map: land away from the lake stays dry (%.1f m deep at %.0f m, lake at %.0f m)" % [wd, plain.y, lake_level])
	check(veg.get_plant_count() > 1000, "forest placed by slope, climate and water rules (%d plants)" % veg.get_plant_count())
	# The tree line comes from the climate: the coldest tree stands where the mean year is about 6 C.
	var line := 0.0
	var coldest := 100.0
	for sp in 2:
		for p in veg.get_placements(sp):
			line = maxf(line, p.y)
			coldest = minf(coldest, data_map.get_temperature_at(p))
	if args.get("snow", "climate") == "climate":
		var winter: SnowClimate = snow.climate
		check(winter != null and winter.recipe == climate and snow.get_climate_map() == snow.get_path_to(data_map) and is_equal_approx(winter.january_temperature, climate.temperature_january) and snow.has_world(),
				"the snowpack's winter comes from the same climate as rivers and plants (SnowClimate.recipe, climate_map)")
	check(data_map.has_climate() and coldest > 5.5 and line > 0.0, "the climate sets the tree line (%.0f m, where the mean year is %.1f C)" % [line, coldest])
	# Geology rules: no tree stands on bare rock or scree, every tree on soil.
	var on_rock := 0
	var total := 0
	for sp in 2:
		for p in veg.get_placements(sp):
			total += 1
			if data_map.get_rock_at(p) > 0.4 or data_map.get_talus_at(p) > 0.3 or data_map.get_soil_depth_at(p) < 0.4:
				on_rock += 1
	check(total > 0 and on_rock == 0, "trees follow the generator's geology: %d of %d on bare rock, scree or thin soil" % [on_rock, total])
	# Rivers carry the climate's water: rain and snowmelt by month, the same recipe as the data map's climate.
	var big_r: Dictionary = {}
	for r in gen.get_rivers(float(args.get("min-area", "1500000"))):
		if big_r.is_empty() or r["areas"][r["areas"].size() - 1] > big_r["areas"][big_r["areas"].size() - 1]:
			big_r = r
	var rp: PackedVector3Array = big_r.get("points", PackedVector3Array())
	var mouth := rp[maxi(rp.size() - 3, 0)] if rp.size() > 0 else Vector3.ZERO
	# The river's own monthly discharge at its mouth (what build_river_paths gives the river).
	var md: Array = big_r.get("monthly_discharge", [])
	var q_spring := 0.0
	var q_summer := 0.0
	if md.size() == 12:
		q_spring = float(md[3])
		q_summer = float(md[7])
	elif md.size() > 0 and md[md.size() - 1] is Array:
		q_spring = float(md[md.size() - 1][3])
		q_summer = float(md[md.size() - 1][7])
	var melt := gen.get_snow_water_at(_highest_point(), 2)
	var months := ""
	for v in md:
		months += " %.2f" % float(v)
	_log("largest river at its mouth by month (m3/s):%s" % months)
	_log("largest river: %.2f m3/s in April, %.2f in August; snow water on the peak in March %.0f mm" % [q_spring, q_summer, melt])
	check(q_spring > 0.0 and q_summer > 0.0, "rivers carry the climate's rain and snowmelt (%.1f m3/s at the largest mouth in April)" % q_spring)
	var q_wet := 0.0
	for m in [11, 0, 1, 2, 3]:
		q_wet = maxf(q_wet, float(md[m]) if md.size() == 12 else 0.0)
	check(q_wet > q_summer * 1.5, "rivers follow the seasons: winter and spring high water, a late-summer low (%.2f against %.2f m3/s)" % [q_wet, q_summer])
	var season: Dictionary = weather.get_season()
	var want_day: int = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334][clock.month - 1] + clock.day
	check(season.has("day_of_year") and absi(int(season["day_of_year"]) - want_day) <= 1 and season.get("photoperiod_h", 0.0) > 0.0,
			"one season state from the SkyClock and the climate (day %d, %.1f h of daylight, forcing %.2f, snow line %.0f m)" % [int(season.get("day_of_year", -1)), float(season.get("photoperiod_h", 0.0)), float(season.get("forcing", 0.0)), float(season.get("snow_line_m", 0.0))])
	check(melt > 0.0, "snow on the high ground holds winter water for the spring melt (%.0f mm on the peak in March)" % melt)
	check(false, "the live weather's rain raises rivers and lakes (flows follow the climate by month, not the storm overhead)", true)
	# Footprints: sand on the beach, mud on the lake bank in the rain, litter under the forest, none on bare rock.
	var beach_p := Vector3(0, 0, _coast_z(0.0) - 25.0)
	beach_p.y = height_at(beach_p.x, beach_p.z)
	# A soft bank: round the lake, the first shore with soil rather than bare rock (glacier-cut lakes sit in rock).
	var bank_p := Vector3.INF
	var a0 := atan2(lake_dir.y, lake_dir.x)
	for k in 24:
		var a := a0 + TAU * k / 24.0
		var ld := Vector3(cos(a), 0, sin(a))
		var q := lake_center
		for i in 400:
			q += ld * 2.0
			if height_at(q.x, q.z) > lake_level + 0.5:
				break
		q += ld * 3.0
		q.y = height_at(q.x, q.z)
		if data_map.get_rock_at(q) < 0.3:
			bank_p = q
			break
	var soft_bank := bank_p.is_finite()
	if not soft_bank:
		bank_p = lake_center + Vector3(lake_dir.x, 0, lake_dir.y) * (lake_axes.x + 3.0)
		bank_p.y = height_at(bank_p.x, bank_p.z)
	# The densest snow-free stand: the placement with the most trees within 15 m (of the first few hundred).
	var trees: Array = veg.get_placements(1) + veg.get_placements(0)
	var tree_p := Vector3.INF
	var best := 0
	for i in mini(trees.size(), 400):
		if _snow_depth(trees[i]) > 0.01:
			continue # snow takes the print there, not the litter
		var nb := 0
		for q in trees:
			if (q as Vector3).distance_squared_to(trees[i]) < 225.0:
				nb += 1
		if nb > best:
			best = nb
			tree_p = trees[i] + Vector3(1.2, 0, 0.8)
	tree_p.y = height_at(tree_p.x, tree_p.z)
	# The weather's fine near-ground square (canopy shelter) follows its target, as it follows the player in a
	# game: the probe walks there for these checks.
	weather.follow_target = weather.get_path_to(prints_probe)
	var rock_p := Vector3.INF
	seed(7)
	for k in 4000:
		var q := Vector3(randf_range(-1800, 1800), 0, randf_range(-1800, 1400))
		q.y = height_at(q.x, q.z)
		if q.y > 20.0 and data_map.get_rock_at(q) > 0.8 and _snow_depth(q) < 0.01:
			rock_p = q
			break
	var pb := await _prints_at(beach_p)
	var pl := await _prints_at(bank_p)
	var pf := await _prints_at(tree_p)
	var dense := weather.get_canopy_closure_at(tree_p)
	var pr := {"deformable": true, "sink": 0.0, "dent": 0.0}
	if rock_p.is_finite():
		pr = await _prints_at(rock_p)
	weather.follow_target = weather.get_path_to(get_node("WorldAnchor"))
	_log("prints: beach sand %.2f sink %.1f cm dent %.1f cm; bank sink %.1f cm dent %.1f cm; forest litter %.3f m sink %.1f cm; rock deformable %s" % [pb["sand"], pb["sink"] * 100.0, pb["dent"] * 100.0, pl["sink"] * 100.0, pl["dent"] * 100.0, pf["litter"], pf["sink"] * 100.0, pr["deformable"]])
	var storm: bool = args.get("weather", "storm") == "storm"
	# Dry sand takes 1.5 to 3 cm under a boot, rain-damp sand under 1 cm (firmer).
	check(pb["sand"] > 0.5 and pb["dent"] > (0.002 if storm else 0.012), "boots print the beach sand (%.1f cm, %s)" % [pb["dent"] * 100.0, "damp" if storm else "dry"])
	if soft_bank:
		check(pl["dent"] > 0.005 and (not storm or pl["sink"] > 0.04), "boots sink into the lake bank, mud in the rain (%.1f cm)" % (pl["sink"] * 100.0))
	else:
		_log("lake bank: every shore of the lake is bare rock, no mud to print (%.1f cm)" % (pl["sink"] * 100.0))
	check(pf["litter"] > 0.005 and pf["dent"] > 0.005, "the forest floor carries litter from the canopy (%.1f cm) and takes prints" % (pf["litter"] * 100.0))
	check(rock_p.is_finite() and not pr["deformable"] and pr["dent"] < 0.001, "bare rock from the generator takes no print")
	check(prints.get_patch_material() is TerrainMaterial3D or not (ground_mat is TerrainMaterial3D), "the print patch is drawn with the terrain's own material")
	check(false, "sand on the ground follows the generator's beaches by itself (a type map is built by script here)", true)
	if args.has("prints-only"):
		_log("canopy map %s rect %s, densest closure %.2f at %s" % [weather.canopy_map, weather.canopy_map_rect, dense, tree_p])
		return
	# Lightning: a ground flash in the storm cell, its channel lighting the scene, thunder after it.
	var strikes_before := lightning.get_ground_strike_count()
	lightning.strike(Vector3(-300, 0, -400), true)
	var bright := 0.0
	var t0 := Time.get_ticks_msec()
	for i in 120:
		await get_tree().process_frame
		bright = maxf(bright, lightning.get_flash_brightness())
	_log("lightning after %d ms: strikes %d, ground %d (before %d), channel %d points" % [Time.get_ticks_msec() - t0, lightning.get_strike_count(), lightning.get_ground_strike_count(), strikes_before, lightning.get_last_channel().size()])
	var ch: PackedVector3Array = lightning.get_last_channel()
	var ground_end := ch[ch.size() - 1] if ch.size() > 0 else Vector3(INF, INF, INF)
	var on_ground := ch.size() > 0 and absf(ground_end.y - height_at(ground_end.x, ground_end.z)) < 5.0
	check(lightning.get_ground_strike_count() > strikes_before and on_ground and bright > 0.0, "lightning strikes the ground in the storm (channel %d points, ends %.1f m from the ground, brightness %.2f)" % [ch.size(), absf(ground_end.y - height_at(ground_end.x, ground_end.z)) if ch.size() > 0 else -1.0, bright])
	var dist := cam.global_position.distance_to(ground_end) if ch.size() > 0 else 0.0
	var delay := lightning.get_last_thunder_delay()
	# Thunder starts from the nearest piece of the whole discharge (in-cloud arms too), so the ground end only
	# bounds it from above; sound speed from the air temperature.
	var sound := 331.3 + 0.606 * weather.temperature
	check(delay > 0.0 and delay <= dist / sound + 0.1, "thunder follows the flash, no later than its ground end (%.1f s, ground end %.0f m = %.1f s; %.1f s long)" % [delay, dist, dist / sound, lightning.get_last_thunder_length()])
	var top := weather.get_cloud_top_at(Vector3(-300, 0, -400)) - height_at(-300, -400)
	_log("flash rate (Price and Rind) for a cloud %.0f m tall over the ground: %.2f a minute" % [top, lightning.get_flash_rate(top)])
	# The ground material: TerrainMaterial3D carries the engine's own snow and wetness lines.
	var gcode := ""
	if ground_mat is ShaderMaterial:
		gcode = (ground_mat as ShaderMaterial).shader.code
	var shared := gcode.contains(BaseMaterial3D.get_snow_shader_code()) and gcode.contains(BaseMaterial3D.get_wetness_shader_code())
	check(ground_mat is TerrainMaterial3D and shared and gcode.contains("terrain_geology(") and gcode.contains("terrain_detail_normal("), "the ground's geology and detail-normal material gets the engine's snow and wetness (TerrainMaterial3D, shared code)")
	# Falling snow: on the peak in the storm, none at the coast (rain there).
	var probe_node := Node3D.new()
	add_child(probe_node)
	snowfall.follow_target = snowfall.get_path_to(probe_node)
	probe_node.global_position = peak + Vector3(0, 2, 0)
	for i in 20:
		await get_tree().process_frame
	var rate_peak := snowfall.get_snowfall_rate()
	var flakes_peak := snowfall.get_visible_flakes()
	var haze_peak := weather.get_precipitation_extinction_at(probe_node.global_position)
	var vis_peak := snowfall.get_visibility()
	probe_node.global_position = Vector3(0, height_at(0, 1200) + 2.0, 1200)
	for i in 20:
		await get_tree().process_frame
	var rate_coast := snowfall.get_snowfall_rate()
	var haze_coast := weather.get_precipitation_extinction_at(probe_node.global_position)
	check(rate_peak > 0.1 and flakes_peak > 0 and rate_coast < 0.02 * weather.rain_rate, "snow falls on the peak in the storm (%.1f mm/h, %d flakes, visibility %.0f m) and not at the coast (%.2f mm/h, under 2%% of the rain)" % [rate_peak, flakes_peak, vis_peak, rate_coast])
	snowfall.follow_target = NodePath("")
	check(snowfall.haze_enabled and haze_peak > 0.0 and haze_peak > haze_coast * 2.0, "falling and blowing snow thicken the fog on the peak by themselves (extinction %.4f /m on the peak, %.4f at the coast)" % [haze_peak, haze_coast])
	check(false, "falling rain particles from the weather (none in the engine)", true)
	if cover != null:
		# Stream round the grass check's meadow point: the test camera sits too high for the 80 m ring.
		var st := Node3D.new()
		add_child(st)
		st.global_position = _dbg_pt + Vector3(0, 1.7, 0)
		cover.stream_target = cover.get_path_to(st)
		var sf := 0
		for i in 3:
			await get_tree().process_frame
			sf += 1
		while not cover.is_stream_idle() and sf < 1200:
			await get_tree().process_frame
			sf += 1
		check(cover.streaming and cover.is_stream_idle() and cover.get_plant_count() > 0, "ground cover streams round the camera on the world's data map (%d plants, settled after %d frames, %s)" % [cover.get_plant_count(), sf, str(cover.get_stream_stats())])
	if args.get("layout", "range") == "valley":
		check(false, "the clearing on the valley floor comes from the generator by itself (a density mask is built by script from its sediment here)", true)
	# The gale check lays 40 mm of snow and moves the near square to the peak, and leaves them: only in test
	# runs, which quit after the checks, so a picture is never taken of its snow (with --snow=none the square's
	# edge showed as a straight snow line across the range).
	if args.has("test"):
		await _wind_snow_check()
	var passes := 0
	var gaps := 0
	var fails := 0
	for r in results:
		if r[0] == "PASS": passes += 1
		elif r[0] == "FAIL": fails += 1
		else: gaps += 1
	print("[landscape] links: %d pass, %d fail, %d gaps" % [passes, fails, gaps])


# Wind moving lying snow (SnowTran-3D in SnowCover3D): a cold gale over the peak for six hours scours the
# windward side and loads the lee. Run last: it changes the weather.
func _wind_snow_check() -> void:
	var peak := _highest_point()
	var sp := Node3D.new()
	add_child(sp)
	sp.global_position = peak
	snow.follow_target = snow.get_path_to(sp)
	# A dry, cold gale: no new snow, so only the wind moves it.
	weather.clear_storms()
	weather.rain_rate = 0.0
	weather.cloud_cover = 0.2
	weather.temperature = -10.0 # at sea level; the lapse rate cools the peak further
	weather.wind_speed = 15.0
	weather.snap_to_settings()
	for i in 4:
		await get_tree().physics_frame
	# 40 mm of fresh, loose snow on the old pack (old wind-packed snow is too dense to move in a gale).
	snow.add_snow(40.0, 100.0)
	var w2: Vector2 = weather.get_mean_wind_at(peak).normalized()
	var wd := Vector3(w2.x, 0, w2.y)
	var side := Vector3(-wd.z, 0, wd.x)
	var band := func(sign: float) -> float:
		var sum := 0.0
		for d in [20.0, 35.0, 50.0]:
			for o in [-30.0, 0.0, 30.0]:
				sum += snow.get_swe_at(peak + wd * d * sign + side * o)
		return sum / 9.0
	var grid := func() -> PackedFloat32Array:
		var g := PackedFloat32Array()
		for gz in range(-100, 101, 10):
			for gx in range(-100, 101, 10):
				g.append(snow.get_swe_at(peak + Vector3(gx, 0, gz)))
		return g
	var wind0: float = band.call(-1.0)
	var lee0: float = band.call(1.0)
	var g0: PackedFloat32Array = grid.call()
	snow.advance(6.0 * 3600.0)
	var wind1: float = band.call(-1.0)
	var lee1: float = band.call(1.0)
	var g1: PackedFloat32Array = grid.call()
	var moved := 0.0
	for k in g0.size():
		moved += absf(g1[k] - g0[k])
	moved /= g0.size()
	# The physics (scour on the windward shoulder, load in the lee, mass kept) is checked on a clean ridge in
	# docs/bench/snow --test=transport; here the link: the scene's own pack moves in the scene's gale.
	_log("wind on lying snow: windward %.0f to %.0f mm, lee %.0f to %.0f mm SWE, mean change %.1f mm over 200 m around the peak, largest flux %.3f kg/(m s)" % [wind0, wind1, lee0, lee1, moved, snow.get_blown_flux_max()])
	check(snow.wind_transport_enabled and snow.get_blown_flux_max() > 0.05 and moved > 2.0, "a cold gale moves the lying snow around the peak (%.1f mm SWE moved on average, flux up to %.2f kg/(m s))" % [moved, snow.get_blown_flux_max()])


func _snow_depth(p: Vector3) -> float:
	# The near square where it covers the point, else the world tier the climate bake left.
	var r: Rect2 = snow.get_map_rect()
	if r.has_point(Vector2(p.x, p.z)) or not snow.has_world():
		return snow.get_depth_at(p)
	return snow.get_world_depth_at(p)


func _coast_z(x: float) -> float:
	# Where the generator's coast is along x: the first point south with sea over it.
	var z := 0.0
	# (sea level, so lakes on the way do not count)
	while z < 2040.0 and not (gen.get_water_level(Vector3(x, 0, z)) <= gen.sea_level + 0.5):
		z += 4.0
	return z


func _highest_point() -> Vector3:
	var best := 0
	for i in heights.size():
		if heights[i] > heights[best]:
			best = i
	return Vector3(X0 + (best % NX) * STEP, heights[best], Z0 + (best / NX) * STEP)

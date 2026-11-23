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
#   --raster          raster only (TAA + SMAA; --aa=0 without)
#   --spp=N           path tracing samples (default 16)
#   --boulders=D     rockfall blocks below cliffs at density D (default 1, 0 none), grey stand-ins
#   --no-far         no rock far tier (big stones and landmarks to 4 km), the before frame
#   --rocks=off      grey stand-ins instead of RockField3D stones; --rocks-area=M builds the stones in M m round the camera (1200)
#   --stone-plants=0 no crevice plants (PlantGenerator.make_stones) between the stones
#   --far-blend [--far-start=M]   TerrainMaterial3D's far blend over a test plant cover map
#   --lake-pick=high  the lake views and checks use the highest lake (default the biggest)
#   --cam=x,y,z --look=x,y,z  a fixed camera (a negative y: that height above the ground)
#   --view=wide|lake|lakewater|valley|fall|beach|peak|alpine|treeline|meadow|gully   camera (default wide; lakewater: low over the lake;
#                         valley: from behind the high lake down to the clearing; treeline: the clearing's edge)
#   --strike[=1] [--strike-x= --strike-z=]   force a lightning strike just before the frame is saved
#   --ground=terrain|standard   ground material: TerrainMaterial3D (default) or the vertex-coloured StandardMaterial3D
#   --falls           print every waterfall the rivers found (lip, landing, height)
#   --probe           print coast profiles, lakes and rivers from the generator
#   --weather=storm|clear         weather state (default storm); --wind=m/s overrides its wind speed
#   --snow=climate|none   the winter baked by SnowClimate (default), or a snow-free ground for stills
#   --month=N --time=H    0-based month of the rivers and the SkyClock date (default 4, 15 May) and local time (16.5)
#   --day=N               day of the month (default 15)
#   --leaves=0            no falling leaves (LeafFall3D); --leaf-litter=N loose leaves per m2 the gusts can lift (0.5)
#   --cloud-shadow=0 --sun-shadow=0   no cloud shadow on the sun; no raster sun shadow map
#   --sky-params=name:value,...   WeatherSkyMaterial shader parameters (A/B frames)
#   --sun-off             no sun at all: what the sky alone lights
#   --ground-scans=0      plain ground layers instead of the CC0 scans in art/terrain_layers (fetch_art.sh);
#                         --ground-scans=rock,scree scans on those layers only
#   --far-shadows=0       no far sun shadows from the ground's horizon toward the sun (raster A/B)
#   --sky-occlusion=0     ground ambient light without the hills' sky occlusion; --sky-light=0 an even sky for it
#   --water-param=name:value[;...]   set water shader uniforms on the sea and lakes (floats, true/false, colours as r,g,b,a)
#   --caustics                       caustics_enabled on the sea and lakes (raster caustics on clear water)
#   --prints=0            no GroundSurface3D (footprints) node
#   --layout=desert       the range's peaks behind a wide low plain to the sea, with a sand sea over the plain
#                         (--sand-sea defaults to it; Namib-like: the dunes run down to the beach)
#   --buggy=x,z[,deg]|beach the offroad buggy from art/ (fetch_art.sh); --buggy-drive=throttle,s[,steer], --buggy-grains, --buggy-dust,
#                         --buggy-cam=dist,height[,side], --buggy-sun-cam=dist,height,off, --buggy-hold, --buggy-flow
#   --layout=range|valley range: the peaks to the sea (default); valley: a cirque with a high lake (--lake-x/z,
#                         --lake-depth) above a valley floor to the sea (needs floodplain deposition, in work)
#   --hanging             with --layout=valley: the lake and big fall in a side valley hung above a glacial trough
#                         (--bowl-inflow=km² feeds the bowl from beyond the map; --bowl-inflow-at=x,z)
#   --surge-test          sea caves on this terrain in a storm swell: the sea surging in, air up any blowhole; tries
#                         cave seeds 0-15 for one with a blowhole; prints, quits
#   --source-lakes        rivers that start at a lake take their discharge from its outlet (PhysXRiver3D.source_lake)
#   --guides              the bowl, ridges and bar as TerrainGuide3D nodes; --coast-guide, --beach-guide: see the code
#   --ice-converged       with --glacial: solve the steady ice to convergence (glacial_converged)
#   --dunes=steps|0       coastal dunes behind the beaches (dunes_enabled, on); --dune-supply=m3/m, --dune-belt=m,
#                         --dune-cell=m, --dune-wind=degrees from onshore, --dune-veg=0..1 (0: bare sand)
#   --find-nebkhas        print the sand sea's nebkha count and the tallest (then quit)
#   --sand-sea=x0,z0,w,d  a desert sand sea over that rect (sand_sea_rect); --sand-sea-kind=transverse|barchan|
#                         linear|star, --sand-sea-depth=m, --sand-sea-wind=degrees, --sand-sea-scale=m, --sand-sea-steps=n
#   --touch=x,z,w,d,mode,y[,depth,falloff,bowl];...  TerrainBrush3D touch-ups under the generator (lower, raise,
#                         flatten, add), the lakes and rivers re-solved after them
#   --preview-test[=scale] move the first guide and time the generator's coarse guide preview
#   --map=path.png        save a top-down map of the generated land (hillshade, water, rivers, falls) and quit
#                         (--map-clean: without the red fall markers, for docs pairs)
#   --mesh=landscape|grid ground: Landscape3D, 1 m with LODs (default), or the 4 m grid mesh; --lod-error=px (2)
#   --trees=cones|generated|none  forest: stand-in cones (default), TreeGenerator's rigged woodland with wind, or not drawn
#   --fungi-cover=F                 TreeGenerator fungi_cover for the generated forest (0 none, 1 default)
#   --mushrooms[=off]              honey tuft at a sunlit log close up, fly agarics behind (off: mushrooms hidden)
#   --pollards                       willow, beech and hazel pollards 5 m apart ahead of the camera, sun from the side
#   --root-pits=0                    do not dig the uprooted logs' root pits (TerrainBrush3D touch-ups); --pit-radius=M (300)
#                         (--tree-variants=N, default 2)
#   --atmosphere=scattering|physical   sky air: Hillaire 2020 multiple scattering (default) or the older PhysicalSky model
#   --clip=N              with --out, N frames in a row (out_0000.png...), for clips; run with --fixed-fps 24
#   --checks              with --out, run the link checks before the picture (they change the scene)
#   --far-wait=N          stop waiting for the grass far colour bake after N frames (900); 0 for scouts and close shots
#   --link=fog|spray|tracks [--link-off]   world-links docs frames, the link on or off (see _link_shot)
#                         --link-verbose logs the tracks walk every second (ground, water, contact wetness, body load)


# A storm swell on the open sea for --surge-test (answers only over water, as the ocean does).
class SurgeSwell extends WorldWater:
	var sea := 0.0
	var t := 0.0
	var gen: Node
	func _height_at(p: Vector3) -> float:
		if gen.get_height(p) > sea:
			return -INF
		return sea + 2.0 * sin(TAU * t / 12.0)


var no_till_tex: ImageTexture
const WalkCycle = preload("res://walk_cycle.gd")
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
var terrain_mat: Material # the TerrainMaterial3D, for diagnostics
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
var fire_tree: MultiMeshInstance3D = null # --fire-tree
var rivers: Array = []
var weather: WeatherMap3D
var sky_mat: WeatherSkyMaterial
var snow: SnowCover3D
var snowfall: SnowFall3D
var rainfall: RainFall3D
var runoff: WeatherRunoff3D
var cornices: SnowCornice3D
var cornice_climate_max := 0.0 # the baked winter's largest cornice (m): 0 where the crests hold too little snow
var lightning: WeatherLightning3D
var probe: Node3D
var data_map: TerrainDataMap3D
var veg: VegetationField3D
var _tree_gen: Object
var grass: GrassField3D
var sward: Array = [] # the grass layers (tall, undercoat, turf)
var worn_paths: Array = [] # --worn-path: {points, width, passes} shared by the grass fields and the plant fields
var cover: VegetationField3D # meadow herbs and waterside plants near the camera
var prints: GroundSurface3D
var prints_probe: Node3D
var slip_walk: PackedVector3Array # --view=slipface: the walk across the slip face (start, end, along, fall)
var sun: DirectionalLight3D
var boulder_obstacles := PackedVector4Array() # (x, ground y, z, radius at the soil) for the plant fields
var boulder_spot := Vector3.ZERO
var global_overrides := {} # --global-param values, re-set each capture frame with --global-every
var rocks: RockField3D # real rock geometry (rockfall, scree, glacial, river, shore, cliff); --rocks=off: grey stand-ins
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
	if args.get("shadow-filter", "high") == "high":
		# Stills have no TAA: soft-low shadow filtering leaves dithered penumbras in one frame; soft high does not.
		RenderingServer.directional_soft_shadow_filter_set_quality(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)
		RenderingServer.positional_soft_shadow_filter_set_quality(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)
	# 8-bit debanding after tonemapping (the viewport's dither; --debanding=0 to see the raw quantisation).
	get_viewport().use_debanding = args.get("debanding", "1") != "0"
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
		if args.has("hanging"):
			# Hanging valley over a glacial trough (Paulo's pick, 12 Nov): the main river's glacier comes in from beyond
			# the map (0.05 m/yr over its 60 km²) and wears a trough to the valley mouth, which the rivers have since
			# filled flat (3000 years); a side bowl on the east wall, closed by a rock bar (lip at 420 m, its outer side a
			# 45 degree rock slope), holds a lake whose outflow drops about 340 m off the lip into the trough. Land past the
			# ridges drains into the bowl (250 km², --bowl-inflow) so the fall carries about 3 m³/s in the spring melt.
			vd.merge({"ice-inflow": "0.05", "outwash-age": "3000", "bowl": "900,-1550,420,0.5",
					"bar": "450,-2040,450,-1650,450,-1300,700,-1200,1500,-1250,2040,-1250", "bar-level": "520,420,480,500,520,540",
					"bar-apron": "1.0", "bowl-inflow": "250", "guides": "1"}, true)
		if args.has("lake-x"):
			vd.erase("no-lake") # a placed high lake instead of the marked bowl's
		for k in vd:
			if not args.has(k):
				args[k] = vd[k]
	t_start = Time.get_ticks_msec()
	_generate()
	_lake_and_fall()
	if args.has("no-till"):
		# The ground material without the moraine till (its negative talus cleared): a till-only before shot.
		var gi: Image = gen.get_geology_image()
		for y in gi.get_height():
			for x in gi.get_width():
				var gc := gi.get_pixel(x, y)
				if gc.b < 0.0:
					gi.set_pixel(x, y, Color(gc.r, gc.g, 0.0, gc.a))
		no_till_tex = ImageTexture.create_from_image(gi)
		RenderingServer.global_shader_parameter_set("terrain_geology_map", no_till_tex.get_rid())
	if args.has("preview-test"):
		# Guide preview timing: move the first guide 200 m east, let the generator's debounce fire (0.4 s), then time
		# the coarse run it made, against the full run above.
		var guides := gen.find_children("*", "TerrainGuide3D", true, false)
		var full_ms := gen.get_last_generate_msec()
		gen.guide_preview = true
		gen.guide_preview_scale = float(args["preview-test"]) if args["preview-test"] != "1" else 4.0
		if not guides.is_empty():
			guides[0].position += Vector3(200, 0, 0)
		for k in 5000:
			await get_tree().process_frame
			if gen.get_last_preview_msec() > 0.0:
				break
		print("[landscape] guide preview at %.0f m cells: %.0f ms with the write (full run %.0f ms without)" % [gen.cell_size * gen.guide_preview_scale, gen.get_last_preview_msec(), full_ms])
	if args.has("find-caves"):
		# --find-caves[=N]: cave entrances (CaveNetwork3D.find_entrances, then find_sea_entrances) on this terrain, printed, then quit.
		var net := CaveNetwork3D.new()
		gen.add_child(net)
		net.generator = net.get_path_to(gen)
		var n := int(args["find-caves"]) if args["find-caves"] != "1" else 8
		for e in net.find_entrances(gen.get_world_rect(), n):
			print("[landscape] cave entrance %s facing %s" % [e["position"], e["out"]])
		for e in net.find_sea_entrances(gen.get_world_rect(), n):
			print("[landscape] sea cave entrance %s facing %s" % [e["position"], e["out"]])
		get_tree().quit(0)
		return
	if args.has("surge-test"):
		# --surge-test: sea caves on this terrain in a storm swell (4 m, 12 s): the sea surging in, and the air up
		# any blowhole (CaveNetwork3D.sea_surge), printed, then quit.
		var net := CaveNetwork3D.new()
		gen.add_child(net)
		net.generator = net.get_path_to(gen)
		var swell := SurgeSwell.new()
		swell.sea = gen.get_sea_level()
		swell.gen = gen
		WorldContracts.add_water_stub(swell)
		# Half of the sea caves end in a chamber, and a chamber under less than 25 m of rock gets a blowhole: try cave
		# seeds until one has it.
		var ents: Array = net.find_sea_entrances(gen.get_world_rect(), 8)
		var tries: Array = []
		for sd in 16:
			net.clear_caves()
			net.seed = sd
			for e in ents:
				var id0: int = net.add_sea_cave(e["position"], e["out"])
				if id0 >= 0 and net.get_blowholes(id0).size() > 0:
					tries.append([sd, e])
			if tries.size() >= 2:
				break
		if tries.is_empty():
			print("[landscape] no sea cave with a blowhole in seeds 0-15")
			for e in ents:
				tries.append([0, e])
		for tr in tries:
			net.clear_caves()
			net.seed = int(tr[0])
			var e: Dictionary = tr[1]
			var id: int = net.add_sea_cave(e["position"], e["out"])
			if id < 0:
				continue
			var top := 0.0
			var sealed := false
			var span := Vector2(INF, -INF)
			for i in 1200:
				swell.t = i * 0.05
				net.step_sea_surge(0.05)
				var sp: Dictionary = net.get_blowhole_spout(id)
				top = maxf(top, float(sp.get("speed", 0.0)))
				sealed = sealed or bool(sp.get("sealed", false))
				var w: Dictionary = net.get_sea_cave_water(id)
				var pts: PackedVector3Array = w.get("points", PackedVector3Array())
				if i >= 600 and pts.size() > 0:
					var back: Vector3 = pts[pts.size() - 1]
					span = Vector2(minf(span.x, back.y), maxf(span.y, back.y))
			print("[landscape] sea cave (seed %d) at %s: %d blowholes; back of the cave %.1f to %.1f m (sea %.1f); %s; air up the blowhole to %.1f m/s" % [net.seed, e["position"], net.get_blowholes(id).size(), span.x, span.y, swell.sea, "a crest seals the passage" if sealed else "never sealed", top])
		WorldContracts.clear_overrides()
		get_tree().quit(0)
		return
	if args.has("find-nebkhas"):
		# --find-nebkhas: how many nebkhas the sand sea grew and the tallest few (x, top y, z, height).
		var neb := gen.get_nebkhas(gen.sand_sea_rect)
		var tall := Array(neb)
		tall.sort_custom(func(a, b): return a.w > b.w)
		print("[landscape] %d nebkhas" % neb.size())
		for i in mini(12, tall.size()):
			print("[landscape] nebkha %s" % tall[i])
		get_tree().quit(0)
		return
	if args.has("dune-map"):
		_save_dune_map(args["dune-map"])
		get_tree().quit(0)
		return
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
	if args.has("silt-probe"):
		# --silt-probe: where the water is along the view (river depth every 2 m from the camera to the look point), then quit.
		var ca := String(args.get("cam", "0,0,0")).split(",")
		var la := String(args.get("look", "0,0,1")).split(",")
		var c0 := Vector3(float(ca[0]), 0, float(ca[2]))
		var fw := (Vector3(float(la[0]), 0, float(la[2])) - c0).normalized()
		for k in 40:
			var q := c0 + fw * (2.0 * k)
			var dd := 0.0
			for rr in rivers:
				dd = maxf(dd, rr.sample_depth(Vector3(q.x, height_at(q.x, q.z), q.z)))
			_log("silt probe %d m: (%.0f, %.0f) ground %.1f river depth %.2f" % [2 * k, q.x, q.z, height_at(q.x, q.z), dd])
		get_tree().quit(0)
		return
	if args.has("silt") and float(args["silt"]) > 0.0:
		# --silt=MG_L: a muddy load (silt and clay off a burnt or eroding slope) entering river --silt-river (5) at its
		# point nearest --silt-at=x,z, MG_L mg/L once mixed through the discharge, carried down for --silt-wait (20) s.
		var rv = rivers[clampi(int(args.get("silt-river", "5")), 0, rivers.size() - 1)]
		var sa := String(args.get("silt-at", "-420,-416")).split(",")
		var want := Vector3(float(sa[0]), 0.0, float(sa[1]))
		var cv: Curve3D = rv.curve
		var best := Vector3.ZERO
		var bd := INF
		var bs := 0.0
		for j in cv.point_count:
			var q: Vector3 = rv.global_transform * cv.get_point_position(j)
			var dd := Vector2(q.x - want.x, q.z - want.z).length()
			if dd < bd:
				bd = dd
				best = q
				bs = cv.get_closest_offset(cv.get_point_position(j))
		var dq: float = rv.get_discharge_at(bs)
		var rate := float(args["silt"]) * 1e-3 * dq
		# Fed across the section (--silt-spread points, 5) so the river carries the load bank to bank at once: a
		# single cell only mixes across over hundreds of metres (Valence). The section: across the flow at best,
		# out to where the water ends.
		var flow: Vector3 = rv.sample_flow(best)
		var across := Vector3(-flow.z, 0, flow.x).normalized() if flow.length() > 1e-4 else Vector3.RIGHT
		var half := [0.0, 0.0]
		for sgn in 2:
			for k in 60:
				var q := best + across * (0.5 * k) * (1.0 if sgn == 0 else -1.0)
				if rv.sample_depth(Vector3(q.x, height_at(q.x, q.z), q.z)) <= 0.0:
					break
				half[sgn] = 0.5 * k
		var npts := maxi(int(args.get("silt-spread", "5")), 1)
		var pts: Array[Vector3] = []
		for k in npts:
			var f := (k + 0.5) / npts
			pts.append(best + across * lerpf(-half[1], half[0], f) * 0.9)
		_log("silt: flow %.2f m/s at the feed, section %.1f m wide, %d feed points" % [flow.length(), half[0] + half[1], npts])
		for q in pts:
			rv.add_sediment(q, rate / npts, 0.0)
		# A rate, held 2 s: fed again every second for the rest of the run, so the reach below settles at the load.
		var feed := Timer.new()
		feed.wait_time = 1.0
		feed.process_callback = Timer.TIMER_PROCESS_PHYSICS
		add_child(feed)
		feed.timeout.connect(func():
			for q in pts:
				rv.add_sediment(q, rate / npts, 0.0))
		feed.start()
		get_viewport().disable_3d = true # the river runs undrawn (a drawn frame takes seconds here)
		for _i in int(float(args.get("silt-wait", "20")) * Engine.physics_ticks_per_second):
			await get_tree().physics_frame
		if args.has("silt-dry-days"):
			# --silt-dry-days=D: the flood's over (the feed stops), then D days of drying on the ground: the fresh
			# silt skins pale and crack into curled plates (GroundSurface3D flood_silt, desiccation_cracks).
			feed.stop()
			for _i in 120:
				await get_tree().physics_frame
			if prints != null:
				prints.settle(float(args["silt-dry-days"]) * 86400.0, 6.0 * 3600.0)
			for _i in 200: # the coloured deposits are re-baked every 5 s
				await get_tree().physics_frame
		get_viewport().disable_3d = false
		if args.has("silt-bank-cam"):
			# --silt-bank-cam: on the bank (--silt-bank-side 0/1) at the feed, --silt-cam-back (3) m past the waterline,
			# eye --silt-cam-h (1.6) m, looking back across the drying silt to the water's edge, down along the river
			# (--silt-cam-along, 6 m), so the foreground silt at 1-3 m grades to the wet margin.
			var sd := int(args.get("silt-bank-side", "0"))
			var edge: Vector3 = best + across * float(half[sd]) * (1.0 if sd == 0 else -1.0)
			var out := across * (1.0 if sd == 0 else -1.0)
			var fl := flow.normalized() if flow.length() > 1e-4 else Vector3.FORWARD
			var cp: Vector3 = edge + out * float(args.get("silt-cam-back", "3"))
			cp.y = height_at(cp.x, cp.z) + float(args.get("silt-cam-h", "1.6"))
			var lk: Vector3 = edge - out * 1.0 + fl * float(args.get("silt-cam-along", "6"))
			lk.y = height_at(lk.x, lk.z)
			var c4: Camera3D = get_viewport().get_camera_3d()
			c4.global_position = cp
			c4.look_at(lk)
			_log("silt bank cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f; silt at the foot %s" % [cp.x, cp.y, cp.z, lk.x, lk.y, lk.z, str(prints.get_silt_at(edge + out * 1.5)) if prints != null and prints.has_method("get_silt_at") else "?"])
		var probe := best + (best - Vector3(want.x, best.y, want.z)).normalized() * 0.0
		_log("silt: %.0f mg/L into river at %s (discharge %.2f m3/s); load there %s kg/m3" % [float(args["silt"]), str(best.round()), dq, str(rv.get_sediment_at(probe))])
	if args.has("river-pick"):
		# --river-pick: list river reaches fit for a wide view (0.2-1.5 m deep, gentle banks within 25 m), then quit.
		for _i in 30:
			await get_tree().physics_frame
		var n_ok := 0
		for k in rivers.size():
			var cv: Curve3D = rivers[k].curve
			if cv == null:
				continue
			for j in range(0, cv.point_count, 6):
				var p: Vector3 = rivers[k].global_transform * cv.get_point_position(j)
				var d: float = rivers[k].sample_depth(p)
				if d < 0.2 or d > 1.5:
					continue
				var t := Vector3.FORWARD
				if j + 1 < cv.point_count:
					t = (rivers[k].global_transform * cv.get_point_position(j + 1)) - p
				t.y = 0.0
				t = t.normalized()
				var side := Vector3(-t.z, 0, t.x)
				var g0 := gen.get_height(p)
				var ga := gen.get_height(p + side * 25.0)
				var gb := gen.get_height(p - side * 25.0)
				var slope := rad_to_deg(atan(maxf(absf(ga - g0), absf(gb - g0)) / 25.0))
				if slope > 14.0:
					continue
				n_ok += 1
				_log("river pick: river %d pt %d at (%.0f, %.1f, %.0f) depth %.2f bank slope %.0f deg, flow dir (%.2f, %.2f), sediment %.2f, discharge %.2f m3/s" % [k, j, p.x, g0, p.z, d, slope, t.x, t.z, gen.get_sediment_depth(p), rivers[k].get_discharge_at(cv.get_closest_offset(cv.get_point_position(j)))])
		_log("river pick: %d reaches" % n_ok)
		get_tree().quit(0)
		return
	if args.has("bank-pick"):
		# --bank-pick: river cut banks fit for a close turf/roots shot (a 0.8-3 m rise within 2.5 m of the water's
		# edge), each with a low oblique camera along the bank over the water; prints the steepest, then quits.
		for _i in 30:
			await get_tree().physics_frame
		var best: Array = []
		for k in rivers.size():
			var cv: Curve3D = rivers[k].curve
			if cv == null:
				continue
			for j in range(0, cv.point_count - 1, 3):
				var p: Vector3 = rivers[k].global_transform * cv.get_point_position(j)
				var d: float = rivers[k].sample_depth(p)
				if d < 0.15:
					continue
				var t: Vector3 = (rivers[k].global_transform * cv.get_point_position(j + 1)) - p
				t.y = 0.0
				if t.length() < 0.01:
					continue
				t = t.normalized()
				var surf := gen.get_height(p) + d
				for sg in [-1.0, 1.0]:
					var side: Vector3 = Vector3(-t.z, 0, t.x) * float(sg)
					var e := Vector3.INF
					for st in 40:
						var q: Vector3 = p + side * (0.5 * st)
						if gen.get_height(q) > surf:
							e = q
							break
					if e == Vector3.INF:
						continue
					var rise := gen.get_height(e + side * 2.5) - surf
					if rise < 0.8 or rise > 3.0:
						continue
					best.append([rise, e, side, t, surf])
		best.sort_custom(func(a, b): return a[0] > b[0])
		for i in mini(best.size(), 8):
			var r: Array = best[i]
			var e: Vector3 = r[1]
			var cam: Vector3 = e - r[2] * 1.5 - r[3] * 7.0
			cam.y = r[4] + 0.7
			var lk: Vector3 = e + r[2] * 0.6 + r[3] * 2.0
			lk.y = r[4] + 0.5 * r[0]
			# y as height above the ground (negative, the --cam/--look convention), so it reads right at any altitude.
			var ch := -maxf(cam.y - gen.get_height(cam), 0.05)
			var lh := -maxf(lk.y - gen.get_height(lk), 0.05)
			_log("bank pick: rise %.2f cam=%.1f,%.2f,%.1f look=%.1f,%.2f,%.1f" % [r[0], cam.x, ch, cam.z, lk.x, lh, lk.z])
		_log("bank pick: %d banks" % best.size())
		get_tree().quit(0)
		return
	if args.has("beaver-dam") and ClassDB.class_exists("WorldBeaverDam3D"):
		# --beaver-dam[=Y]: a WorldBeaverDam3D (world-links 1f60b5c4) at --dam-at=x,z (else the --look point), pond
		# --pond-radius (30) m; Y > 0 breaches it Y years ago, so the stand turns the pond to a beaver meadow of
		# standing dead trees (trees 771b46b4a2). Placed before the vegetation so the stands read it.
		var da := String(args.get("dam-at", args.get("look", "0,0,0"))).split(",")
		var dx := float(da[0])
		var dz := float(da[2] if da.size() > 2 else da[1])
		var dam: Node3D = ClassDB.instantiate("WorldBeaverDam3D")
		dam.name = "BeaverDam"
		dam.set("pond_radius", float(args.get("pond-radius", "30")))
		add_child(dam)
		dam.global_position = Vector3(dx, height_at(dx, dz), dz)
		var dy := float(args["beaver-dam"]) if String(args["beaver-dam"]) != "" else 0.0
		if dy > 0.0:
			dam.set("years_drained", dy)
			dam.call("breach")
		_log("beaver dam at %s, pond %.0f m, %s" % [str(dam.global_position.round()), float(dam.get("pond_radius")), ("breached %.1f years" % dy) if dy > 0.0 else "active"])
	_build_vegetation()
	if args.get("prints", "1") != "0":
		_build_prints()
	_build_camera()
	if args.has("fov"):
		get_viewport().get_camera_3d().fov = float(args["fov"]) # --fov=DEG: a wider or narrower lens
	if args.has("storm-approach"):
		# --storm-approach[=KM]: a storm cell KM (17) km ahead of the camera, upwind (clouds cd3dd60b): the wind blows
		# from it toward the camera so its shelf faces us; --storm-radius (2500) m, --storm-rain (40) mm/h,
		# --storm-outflow (15) m/s, grown --storm-grow (900) s. Shoot with --frames=24 or more.
		var sc: Camera3D = get_viewport().get_camera_3d()
		var sf := -sc.global_basis.z
		var sf2 := Vector2(sf.x, sf.z).normalized()
		var skm := float(args["storm-approach"]) if String(args["storm-approach"]) != "" else 17.0
		var sp := Vector2(sc.global_position.x, sc.global_position.z) + sf2 * skm * 1000.0
		weather.wind_direction = -sf2
		if args.has("storm-wind-turn"):
			# --storm-wind-turn=DEG: the wind turned DEG from toward the camera (90: across the view, a side-on tower
			# with its anvil and cirrus shield spreading sideways).
			weather.wind_direction = (-sf2).rotated(deg_to_rad(float(args["storm-wind-turn"])))
		weather.add_storm(sp, float(args.get("storm-radius", "2500")), float(args.get("storm-rain", "40")), float(args.get("storm-outflow", "15")), 0.0)
		weather.advance(float(args.get("storm-grow", "900")))
		_log("storm approach: cell at %s, %.1f km ahead, wind toward the camera %s" % [str(sp.round()), skm, str(-sf2)])
	if args.has("deer-lawn"):
		# --deer-lawn[=P]: deer browse a disc --lawn-dist (25) m ahead of the camera, --lawn-radius (22) m, pressure P
		# (0.9) through WorldStand.add_browse (cef8dbd7): grass grazes to a lawn there, bramble bitten at its edge,
		# trees keep a browse line (vegetation 5d2594e2, 6b33a29b).
		var lc: Camera3D = get_viewport().get_camera_3d()
		var lf := -lc.global_basis.z
		lf.y = 0.0
		var lp := lc.global_position + lf.normalized() * float(args.get("lawn-dist", "25"))
		lp.y = height_at(lp.x, lp.z)
		var lpress := float(args["deer-lawn"]) if String(args["deer-lawn"]) != "" else 0.9
		WorldContracts.get_stand().add_browse(lp, float(args.get("lawn-radius", "22")), lpress)
		_log("deer lawn at %s, browse %.2f there" % [str(lp.round()), WorldContracts.get_stand().browse_at(lp)])
	if args.has("sand-vista") and "sand_sea_rect" in gen:
		# --sand-vista: from the sand sea's edge (--vista-at, 0.12 of its width in) --vista-h (8) m up, looking across
		# its middle to the far dunes, so the near ripples and the dune-scale tone far off share the frame.
		var r: Rect2 = gen.sand_sea_rect
		var f := float(args.get("vista-at", "0.12"))
		var cp := Vector3(r.position.x + r.size.x * f, 0.0, r.get_center().y)
		cp.y = height_at(cp.x, cp.z) + float(args.get("vista-h", "8"))
		var lk := Vector3(r.get_center().x + r.size.x * 0.2, 0.0, r.get_center().y + r.size.y * float(args.get("vista-turn", "0.0")))
		lk.y = height_at(lk.x, lk.z)
		var cv: Camera3D = get_viewport().get_camera_3d()
		cv.global_position = cp
		cv.look_at(lk)
		_log("sand vista cam %.0f,%.0f,%.0f look %.0f,%.0f,%.0f (sand sea %s)" % [cp.x, cp.y, cp.z, lk.x, lk.y, lk.z, str(r)])
	if args.has("haboob") and weather.has_method("get_dust_at"):
		# --haboob[=KM]: a dry storm's outflow KM (5) km ahead of the camera raising a dust wall over the loose ground
		# (WeatherMap3D dust layer, clouds 9078fe25): hot dry air (35 C, 15%), dust_source_share 0.8, emission x10, a
		# 25 m/s outflow grown --haboob-grow (30) minutes so the wall runs toward the camera. Low sun from --time.
		var hc: Camera3D = get_viewport().get_camera_3d()
		var hf := -hc.global_basis.z
		var hf2 := Vector2(hf.x, hf.z).normalized()
		var hkm := float(args["haboob"]) if String(args["haboob"]) != "" else 5.0
		var hp := Vector2(hc.global_position.x, hc.global_position.z) + hf2 * hkm * 1000.0
		weather.air_follows_climate = false
		weather.temperature = 35.0
		weather.humidity = 0.15
		weather.wind_speed = 0.5
		weather.wind_direction = -hf2
		weather.dust_source_share = float(args.get("haboob-share", "0.8"))
		weather.dust_emission_scale = float(args.get("haboob-emission", "10"))
		weather.snap_to_settings()
		weather.add_storm(hp, 1500.0, 0.0, 25.0, 0.0)
		for i in int(args.get("haboob-grow", "30")):
			weather.advance(60.0)
			await get_tree().physics_frame
		var prof := ""
		for k in 6:
			var q := hc.global_position + Vector3(hf2.x, 0, hf2.y) * (1000.0 * (k + 1))
			prof += " %d km: %.2f g/m2 top %.0f m," % [k + 1, weather.get_dust_at(q), weather.get_dust_top_at(q)]
		_log("haboob: storm %.1f km ahead at %s;%s" % [hkm, str(hp.round()), prof])
	if args.get("wet-follow-cam", "1") != "0" and weather != null:
		# WeatherMap3D's local reads (smoke haze, cloud shadow, contact wetness) round the camera for the picture;
		# the setup above ran on the world anchor. --wet-follow-cam=0 keeps the origin anchor.
		weather.follow_target = weather.get_path_to(cam)
	if clock != null:
		_log("sun altitude %.1f deg, azimuth %.1f deg" % [clock.get_sun_altitude(), clock.get_sun_azimuth()])
	if args.has("seed-warm") and get_node_or_null("SeedDown") != null:
		# --seed-warm=S: S seconds of the dry wind loosing seeds before the shot, so the air already carries them.
		var sdn = get_node("SeedDown")
		sdn.refresh_sources()
		for i in int(float(args["seed-warm"]) * 10.0):
			sdn.advance(0.1, true)
		_log("seed down: %d sources, %d released, %d aloft, %d lying" % [sdn.get_source_count(), sdn.get_released_count(), sdn.get_aloft_count(), sdn.get_lying_count()])
	if args.has("worn-path"):
		_build_worn_path()
	if args.has("farm-track") and prints != null and prints.has_method("add_wear"):
		_build_farm_track()
	if args.get("view", "wide") == "beach" and args.get("cove-props", "1") != "0":
		_cove_props()
	if args.get("plants", "1") != "0":
		_build_cover()
	_build_mist_pollen()
	if args.has("crop"):
		_build_crop()
	if args.has("solo-tree"):
		_build_solo_tree()
	if args.has("frost-puddle") and prints != null:
		await _frost_puddle()
	if args.has("far-cover") and veg != null:
		# The world field publishes terrain_cover_map from its rules (crowns, far plant colours, the sward), built
		# when it places: placed again here once the grass is set up.
		var tc := Time.get_ticks_msec()
		veg.cover_grass = veg.get_path_to(grass)
		var under_field := get_node_or_null("Undercoat")
		if under_field != null and "cover_grass_layers" in veg:
			# The undercoat fills the gaps between the tall grass patches, far as near.
			veg.cover_grass_layers = [veg.get_path_to(under_field)]
		veg.cover_map_enabled = true
		if not veg.streaming:
			veg.populate() # a streaming field rebuilds its cover map itself and keeps streaming
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
	if veg != null and args.get("trees", "cones") == "generated" and args.get("root-pits", "1") != "0":
		await _dig_root_pits()
	if veg != null and (args.has("trunk-view") or args.has("sticks-view")):
		_frame_wood_closeup()
	if args.has("grass-dump") or args.has("grass-off"):
		# Grass diagnostics (far/near colour step): each layer's tint settings, --grass-off hides every layer.
		var layers: Array = sward if not sward.is_empty() else [grass]
		for g in layers:
			_log("grass layer %s: ground_tint_enabled %s, ground_tint %s, base %s, tip %s, radius %.0f, density %.0f, far_color %s" % [g.name, g.ground_tint_enabled, g.ground_tint, g.base_color, g.tip_color, g.radius, g.density, str(g.get("far_color"))])
			if g.has_method("get_far_color"):
				_log("grass layer %s: far colour baked %s, get_far_color %s" % [g.name, str(g.call("is_far_color_baked")) if g.has_method("is_far_color_baked") else "?", str(g.get_far_color())])
			if args.has("grass-off"):
				g.visible = false
		if terrain_mat != null:
			var props := []
			for pr in terrain_mat.get_property_list():
				var nm: String = pr["name"]
				if nm.contains("grass") or nm.contains("far_"):
					props.append("%s=%s" % [nm, str(terrain_mat.get(nm))])
			_log("terrain material: %s" % ", ".join(props))
		_log("sun elevation %.1f deg" % rad_to_deg(asin(clampf(sun.global_basis.z.y, -1.0, 1.0))))
		_log("sun direction to sun %s, light energy %.2f, ssao %s, ssil %s, sdfgi %s" % [str(sun.global_basis.z), sun.light_energy, env.ssao_enabled, env.ssil_enabled, env.sdfgi_enabled])
		if veg != null and veg.has_method("get_cover_texture"):
			var ct2: Texture2D = veg.get_cover_texture()
			_log("far cover map after settling: %s" % (str(ct2.get_size()) if ct2 else "missing"))
		for f in [veg, cover]:
			if f != null and f.has_method("get_near_radius"):
				_log("%s near radius %.1f m (stream radius %.0f)" % [f.name, f.get_near_radius(), f.stream_radius])
	if args.has("bracken"):
		await _frame_bracken()
	if args.has("lightning-cam"):
		await _frame_struck()
	if args.has("host-cam"):
		await _frame_host()
	if args.has("colonnade-cam"):
		await _frame_colonnade()
	if args.has("log"):
		await _frame_log()
	if args.has("mushrooms"):
		await _frame_mushrooms()
	if args.has("pollards"):
		_place_pollards()
	if args.has("foot"):
		# --foot: 3 m from the foot of the broad-leaved tree nearest the first view, at knee height, side-lit
		# (the moss sleeve and root ridges running into the litter).
		var c4: Camera3D = get_viewport().get_camera_3d()
		var near := Vector3.INF
		for tp in _broadleaf_trees():
			if tp.distance_to(c4.global_position) < near.distance_to(c4.global_position) and veg.get_litter_at(tp) > 0.5:
				near = tp
		if near != Vector3.INF:
			var ts := sun.global_basis.z
			var sd := Vector3(ts.x, 0.0, ts.z).normalized().cross(Vector3.UP).normalized()
			var fp := near + sd * 3.0
			fp.y = height_at(fp.x, fp.z) + 0.9
			c4.global_position = fp
			c4.look_at(near + Vector3.UP * 0.35)
			await _forest_settle()
			_log("foot cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f" % [fp.x, fp.y, fp.z, near.x, near.y + 0.35, near.z])
	_log("setup done in %d ms" % (Time.get_ticks_msec() - t_start))
	_mem_log("after setup")
	if args.has("sun-off"):
		sun.visible = false # an A/B frame: what the sky alone lights
	if args.has("water-param"):
		# --water-param=name:value[;name:value]: a water shader uniform for an A/B frame, on the sea and the lakes
		# (floats; bools as true/false).
		for kv in String(args["water-param"]).split(";"):
			var pv := kv.split(":")
			var val: Variant = (pv[1] == "true") if pv[1] in ["true", "false"] else float(pv[1])
			if "," in pv[1]:
				var c := pv[1].split_floats(",")
				val = Color(c[0], c[1], c[2], c[3])
			for w in water_nodes:
				if w.has_method("get_water_material") and w.get_water_material() != null:
					w.get_water_material().set_shader_parameter(pv[0], val)
			_log("water %s = %s" % [pv[0], val])
	get_viewport().disable_3d = not (args.has("test") or args.has("checks")) # settling frames, not drawn
	for i in 30:
		await get_tree().process_frame
	get_viewport().disable_3d = false
	# The checks move probes, press prints and step the weather: a picture only runs them when asked (--checks),
	# and under path tracing each of their frames is a full traced frame.
	if args.has("test") or args.has("checks"):
		await _checks()
	if args.has("test"):
		get_tree().quit(0)
		return
	if args.has("cave") and ClassDB.class_exists("CaveNetwork3D"):
		# --cave=x,y,z,fx,fz: a cave at that entrance facing (fx, fz), meshed 100 m round it (rocks' CaveNetwork3D).
		# --sea-cave: a sea cave instead. --cave-view: --cave-dist (15) m out along the facing, entrance height + 1.7 m, looking at the entrance + 3 m.
		var cv := String(args["cave"]).split(",")
		var ce := Vector3(float(cv[0]), float(cv[1]), float(cv[2]))
		var cf := Vector3(float(cv[3]), 0, float(cv[4])).normalized()
		var cn = ClassDB.instantiate("CaveNetwork3D")
		cn.name = "Caves"
		gen.add_child(cn)
		cn.set_generator(cn.get_path_to(gen))
		if rocks != null:
			cn.set_rock_field(cn.get_path_to(rocks))
		if landscape != null:
			cn.set_ground_material(landscape.material)
		if args.has("sea-cave"):
			cn.call("add_sea_cave", ce, cf) # --sea-cave: the mouth floor under the sea (rocks' sea caves)
		else:
			cn.add_cave(ce, cf)
		var ct := Time.get_ticks_msec()
		cn.build_meshes(AABB(ce - Vector3(50, 50, 50), Vector3(100, 100, 100)))
		_log("cave at %s facing %s meshed in %d ms" % [str(ce), str(cf), Time.get_ticks_msec() - ct])
		if args.has("cave-view"):
			var c6: Camera3D = get_viewport().get_camera_3d()
			var cq := ce + cf * float(args.get("cave-dist", "15"))
			c6.global_position = Vector3(cq.x, ce.y + 1.7, cq.z)
			c6.look_at(ce + Vector3(0, 3, 0))
			_log("cave cam %s" % str(c6.global_position.round()))
	if rocks != null:
		var rc := get_viewport().get_camera_3d().global_position
		var ra := float(args.get("rocks-area", "1200"))
		var rb := Time.get_ticks_msec()
		if "ground_material" in rocks and landscape != null:
			# The stones' feet take the landscape's own ground layers (--rock-blend=0: no foot blend).
			rocks.ground_material = landscape.material
			rocks.ground_blend = args.get("rock-blend", "1") != "0"
			_log("rocks: ground blend %s on the landscape material (%s)" % [str(rocks.ground_blend), landscape.material.get_class() if landscape.material else "null"])
		if args.has("rock-prop"):
			# --rock-prop=name:value[;name:value]: RockField3D properties set before the build (true/false or numbers).
			for kv in String(args["rock-prop"]).split(";"):
				var pr := kv.split(":")
				if pr.size() == 2 and pr[0] in rocks:
					var v: Variant = (pr[1] == "true") if pr[1] in ["true", "false"] else float(pr[1])
					rocks.set(pr[0], v)
					_log("rocks: %s = %s" % [pr[0], str(rocks.get(pr[0]))])
		if args.has("rock-density"):
			# --rock-density=F: every layer's density times F (memory tests).
			for li in 9:
				rocks.set_layer_density(li, rocks.get_layer_density(li) * float(args["rock-density"]))
		if args.has("find-stones"):
			# --find-stones=L: list the 12 biggest stones of layer L placed on the world's ground, then quit.
			var wr0: Rect2 = gen.get_world_rect()
			var all: Array = rocks.get_stones(int(args["find-stones"]), wr0)
			all = all.filter(func(st):
				var q: Vector3 = st.get("position", Vector3.INF)
				return is_finite(q.x) and is_finite(q.z) and is_finite(height_at(q.x, q.z)))
			all.sort_custom(func(a, b):
				var sa: Variant = a.get("scale", 1.0)
				var sb: Variant = b.get("scale", 1.0)
				return (sa.x if sa is Vector3 else float(sa)) > (sb.x if sb is Vector3 else float(sb)))
			_log("find-stones: %d of layer %s on the ground" % [all.size(), String(args["find-stones"])])
			for k in mini(12, all.size()):
				var q: Vector3 = all[k]["position"]
				_log("  stone %d at (%.0f, %.1f, %.0f) scale %s, sea +%.1f m" % [k, q.x, height_at(q.x, q.z), q.z, str(all[k].get("scale")), height_at(q.x, q.z) - gen.sea_level])
			get_tree().quit()
			return
		var built := rocks.build_area(Rect2(rc.x - ra * 0.5, rc.z - ra * 0.5, ra, ra))
		_log("rocks: %d cells, %d stones in %d ms (shape libraries %.0f ms, %d library triangles)" % [built, rocks.get_stone_count(), Time.get_ticks_msec() - rb, rocks.get_last_library_msec(), rocks.get_library_triangle_count()])
		# The far tier (modules/rocks): stones over 2 m and landmarks past each layer's radius, to far_radius (4 km).
		# --no-far: off (the before frame); --far-thin=0: far_thinning off.
		if args.has("no-far"):
			rocks.far_radius = 0.0
		if args.get("far-thin", "1") == "0":
			rocks.far_thinning = false
		if rocks.far_radius > 0.0 and rocks.has_method("build_far"):
			var tfar := Time.get_ticks_msec()
			rocks.build_far()
			_log("rocks far tier: %d stones to %.0f m in %d ms" % [rocks.get_far_stone_count(), rocks.far_radius, Time.get_ticks_msec() - tfar])
		if args.has("stack-view"):
			# --stack-view[=N]: the N-th biggest coast stone (sea stack, LAYER_COAST = 7) in the built area, framed from
			# the land side at --stack-dist (3) times its size, --stack-cam-h (1.2) m up, the sun across the view.
			# --stack-layer=L (7 coast; 6 tor for a coastal tor).
			var stacks: Array = rocks.get_stones(int(args.get("stack-layer", "7")), Rect2(rc.x - ra * 0.5, rc.z - ra * 0.5, ra, ra))
			# Only stones placed on the world (finite heights inside the generator's rect).
			var wr: Rect2 = gen.get_world_rect()
			var n_all := stacks.size()
			stacks = stacks.filter(func(st):
				var q: Vector3 = st.get("position", Vector3.INF)
				return is_finite(q.x) and is_finite(q.z) and is_finite(height_at(q.x, q.z)))
			# Stone heights can come back unset (NaN); stand them on the ground.
			stacks = stacks.map(func(st):
				var q: Vector3 = st.get("position", Vector3.ZERO)
				if not is_finite(q.y):
					st["position"] = Vector3(q.x, height_at(q.x, q.z), q.z)
				return st)
			_log("stack view: %d of %d stones with ground under them (world %s, built round %s, %.0f m)" % [stacks.size(), n_all, str(wr), str(rc.round()), ra])
			if args.has("stack-near-sea"):
				# --stack-near-sea=H: only stones whose foot is under H m above the sea (a coastal tor, not an inland one).
				stacks = stacks.filter(func(st): return (st.get("position", Vector3.ZERO) as Vector3).y < gen.sea_level + float(args["stack-near-sea"]))
			stacks.sort_custom(func(a, b):
				var sa: Variant = a.get("scale", 1.0)
				var sb: Variant = b.get("scale", 1.0)
				return (sa.x if sa is Vector3 else float(sa)) > (sb.x if sb is Vector3 else float(sb)))
			var pick := mini(int(args.get("stack-pick", "0")), stacks.size() - 1) # --stack-pick=N: the N-th biggest
			if pick >= 0:
				var st: Dictionary = stacks[pick]
				var sp: Vector3 = st.get("position", Vector3.ZERO)
				var sc: Variant = st.get("scale", 1.0)
				var sz: float = (sc.x if sc is Vector3 else float(sc))
				var cam6: Camera3D = get_viewport().get_camera_3d()
				# From the side of the stack where the ground is highest (the land), sun across.
				var best_h := -INF
				var cq := sp
				for k in 16:
					var d := Vector3(cos(TAU * k / 16.0), 0, sin(TAU * k / 16.0))
					var q := sp + d * maxf(sz * float(args.get("stack-dist", "3")), 4.0)
					var h := height_at(q.x, q.z)
					if h > best_h:
						best_h = h
						cq = q
				cam6.global_position = Vector3(cq.x, maxf(height_at(cq.x, cq.z), gen.sea_level) + float(args.get("stack-cam-h", "1.2")), cq.z)
				cam6.look_at(Vector3(sp.x, maxf(sp.y, gen.sea_level) + sz * 0.35, sp.z))
				_log("stack view: %d stones, pick %d size %.1f m at %s, cam %s" % [stacks.size(), pick, sz, str(sp.round()), str(cam6.global_position.round())])
			else:
				_log("stack view: no coast stones in the built area")
	# The streamed ground cover settles round the camera before the picture.
	var sw := 0
	get_viewport().disable_3d = true # placing only: the frames draw nothing
	while cover != null and not cover.is_stream_idle() and sw < 1200:
		await get_tree().process_frame
		sw += 1
	get_viewport().disable_3d = false
	# Every capture waits for the grass far colours to bake (a SubViewport render, ~30 frames, restarted by any data
	# map change) and for the cover to settle again, not for a fixed frame count.
	var layers_bake: Array = sward if not sward.is_empty() else ([grass] if grass != null else [])
	var bw := 0
	get_viewport().disable_3d = args.get("bake-draw", "0") == "0" # the bake draws in its own SubViewport; the main view waits undrawn
	# --far-wait=N: give up on the far colour bake after N frames (default 900). Scouts and close --link frames take
	# --far-wait=0 (the far grass keeps its last colours, ~50 min saved on lavapipe); docs stills keep the default.
	var far_wait := int(args.get("far-wait", "900"))
	while bw < 900:
		var done := cover == null or cover.is_stream_idle()
		for g in layers_bake:
			if bw >= far_wait:
				break
			if g.has_method("is_far_color_baked") and not g.is_far_color_baked():
				done = false
		if done:
			break
		await get_tree().process_frame
		bw += 1
	get_viewport().disable_3d = false
	_log("grass far colours baked and cover idle after %d frames" % bw)
	_mem_log("after first frames")
	if args.has("cover-probe"):
		await _cover_probe()
		get_tree().quit(0)
		return
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
		_mem_log("after load_all_heights")
		landscape.update_lods_from_camera()
		_mem_log("after update_lods_from_camera")
		if args.has("seat-probe"):
			# --seat-probe=x,z: stone seating against the drawn ground (generator vs landscape height, chunk LOD, stones).
			var sv := String(args["seat-probe"]).split(",")
			var sp := Vector3(float(sv[0]), 0, float(sv[1]))
			var wr: Rect2 = gen.get_world_rect()
			var grid: Vector2i = landscape.get_chunk_grid()
			var chunk := Vector2i(int((sp.x - wr.position.x) / (wr.size.x / grid.x)), int((sp.z - wr.position.y) / (wr.size.y / grid.y)))
			_log("seat probe at %s: generator %.3f, landscape %.3f, chunk %s of %s LOD %d, stats %s" % [str(sp), gen.get_height(sp), landscape.get_height(sp), str(chunk), str(grid), landscape.get_chunk_lod(chunk), str(landscape.get_stats())])
			var sn := Vector3(height_at(sp.x - 1.0, sp.z) - height_at(sp.x + 1.0, sp.z), 2.0, height_at(sp.x, sp.z - 1.0) - height_at(sp.x, sp.z + 1.0)).normalized()
			var tsun := sun.global_basis.z
			_log("seat probe light: to sun %s (elevation %.1f deg), slope normal %s, sun on slope (N.L) %.2f, shadow max %.0f m, shadows %s, ssao %s" % [str(tsun), rad_to_deg(asin(tsun.y)), str(sn), sn.dot(tsun), sun.directional_shadow_max_distance, str(sun.shadow_enabled), str(env.ssao_enabled)])
			if rocks != null:
				for st in rocks.get_stones(ClassDB.class_get_integer_constant("RockField3D", "LAYER_SCREE"), Rect2(sp.x - 1.5, sp.z - 1.5, 3.0, 3.0)):
					var p: Vector3 = st.get("position", Vector3.ZERO)
					_log("seat probe stone %s ground %s scale %s, generator there %.3f, landscape %.3f" % [str(p), str(st.get("ground")), str(st.get("scale")), gen.get_height(p), landscape.get_height(p)])
	if args.has("hide-class"):
		# --hide-class=A,B: hide every node of these classes (single-toggle hunts for a stray drawable).
		var hc := String(args["hide-class"]).split(",")
		var hn := 0
		for nd in get_tree().root.find_children("*", "", true, false):
			for c in hc:
				if nd.is_class(c) and nd.has_method("hide"):
					nd.hide()
					hn += 1
		_log("hide-class %s: %d nodes hidden" % [str(hc), hn])
	if args.has("list-drawables"):
		# --list-drawables: every visible node that draws translucency or particles, by class.
		var cnt := {}
		for nd in get_tree().root.find_children("*", "", true, false):
			if nd is GeometryInstance3D or nd is FogVolume or nd is GPUParticles3D or nd is CPUParticles3D:
				var k := nd.get_class()
				cnt[k] = cnt.get(k, 0) + 1
		_log("drawables %s" % str(cnt))
	if args.has("snow-road") and snow != null:
		await _snow_road_pick()
	if args.has("buggy"):
		await _place_buggy()
	if args.has("storm-patch"):
		await _storm_patch()
	if args.has("rills") and prints != null:
		await _rills()
	if args.has("campfire") or args.has("fire-spread") or args.has("curtain") or args.has("laundry"):
		await _campfire()
	if args.has("hut"):
		await _hut()
		if args.has("hut-icicles"):
			await _hut_icicles()
		if args.has("hut-roof-snow"):
			await _hut_roof_snow()
		if args.has("hut-rime"):
			await _hut_rime()
	if slip_walk.size() == 4 and prints != null:
		await _slip_walk()
	if args.has("powder-avalanche") and snow != null:
		await _powder_avalanche()
	if args.has("suncups") and snow != null:
		await _suncups()
	if args.has("slush-prints") and snow != null:
		await _slush_prints()
	if args.has("sastrugi") and snow != null:
		await _sastrugi()
	if args.has("spindrift-cam") and snow != null:
		await _spindrift_cam()
	if (args.has("face-sun") or args.has("face-away")) and sun != null:
		# --face-sun=PITCH: turn the camera to the sun's azimuth, PITCH degrees below the horizon (godray shots).
		var cam5: Camera3D = get_viewport().get_camera_3d()
		var to_sun := sun.global_basis.z
		var flat := Vector3(to_sun.x, 0.0, to_sun.z).normalized()
		if not args.has("face-sun"):
			flat = -flat # --face-away=PITCH: the same, looking down-sun (far grass colour pairs)
		# --face-yaw=DEG: turn that many degrees off the sun's azimuth (left positive), to frame round a landform.
		flat = flat.rotated(Vector3.UP, deg_to_rad(float(args.get("face-yaw", "0"))))
		var pitch := deg_to_rad(float(args.get("face-sun", args.get("face-away", "0"))))
		cam5.look_at(cam5.global_position + flat * cos(pitch) - Vector3.UP * sin(pitch))
		_log("facing the sun: azimuth dir %s, sun elevation %.1f deg" % [str(flat), rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0)))])
	if args.has("stand-trail"):
		_stand_trail()
		if args.has("rooting-tyre") and prints != null:
			await _rooting_tyre()
	if args.has("gully-cam"):
		_gully_cam()
	if args.has("dust-devil") and ClassDB.class_exists("PhysXDustDevils3D"):
		await _dust_devil()
	if args.has("rime-ridge"):
		await _rime_ridge()
	if args.has("fog-sea"):
		await _fog_sea()
	if args.has("late-frost"):
		await _fog_sea("late-frost")
	if args.has("snow-strata"):
		await _snow_strata()
	if args.has("frost-sorted"):
		_frost_sorted_cam()
	if args.has("walker-snow"):
		await _walker_snow()
	if args.has("lodging"):
		await _lodging()
	if args.has("lens-cam"):
		await _lens_cam()
	if args.has("sea-effect"):
		await _sea_effect()
	await _docs_links_setup()
	# Raster: 160 frames, so the sky's reprojected clouds (one pixel of each 4x4 block a frame) have had ten
	# samples a pixel; fewer leave the first frame's upsampled blocks and the march jitter in the picture
	# (a game reaches that within a second or two of a cut).
	if args.has("final-res"):
		# --final-res=WxH: run the setup and any simulation small, then draw the capture frames at this size.
		var fr := String(args["final-res"]).split("x")
		get_window().size = Vector2i(int(fr[0]), int(fr[1]))
		await get_tree().process_frame
		await get_tree().process_frame
		_log("window now %s" % str(get_viewport().get_visible_rect().size))
	var frames := int(args.get("frames", "160" if args.has("raster") else "40"))
	if weather != null and not args.has("clip") and args.get("still-weather", "1") != "0":
		# A still picture: the weather (cloud drift) holds while the frames settle. Here a frame takes
		# seconds, so the clouds would move a long way between a pixel's samples (each pixel of the sky's
		# 4x4 blocks is drawn every 16th frame) and the resolve would average them into a blur.
		weather.time_scale = 0.0
	if args.has("global-param"):
		# --global-param=name:value[;...]: global shader uniforms set just before the capture frames (diagnostics).
		# A vector as a,b,c,d; "=" keeps that component's current value.
		for kv in String(args["global-param"]).split(";"):
			var gp := kv.split(":")
			var cur: Variant = RenderingServer.global_shader_parameter_get(gp[0])
			_log("global shader parameter %s was %s" % [gp[0], str(cur)])
			var val: Variant = float(gp[1])
			if "," in gp[1]:
				var comps := gp[1].split(",")
				var v4 := Vector4.ZERO
				var cv := Vector4.ZERO
				if cur is Vector4:
					cv = cur
				elif cur is Color:
					cv = Vector4(cur.r, cur.g, cur.b, cur.a)
				elif cur is Vector3:
					cv = Vector4(cur.x, cur.y, cur.z, 0)
				for ci in mini(comps.size(), 4):
					v4[ci] = cv[ci] if comps[ci] == "=" else float(comps[ci])
				val = v4
				if cur is Color:
					val = Color(v4.x, v4.y, v4.z, v4.w)
				elif cur is Vector3:
					val = Vector3(v4.x, v4.y, v4.z)
			RenderingServer.global_shader_parameter_set(gp[0], val)
			global_overrides[gp[0]] = val
			_log("global shader parameter %s = %s" % [gp[0], str(val)])
	if args.has("cover-range") and cover != null and cover.biome != null:
		# --cover-range=M: every Cover species ends at min(its range, M) (diagnostics: where the herbs hand over).
		var cr := float(args["cover-range"])
		var nsp := 0
		for sp in cover.biome.species:
			if sp != null and sp.has_method("set_visibility_range"):
				sp.set_visibility_range(cr if sp.get_visibility_range() <= 0.0 else minf(sp.get_visibility_range(), cr))
				nsp += 1
		_log("cover: %d species capped at %.0f m" % [nsp, cr])
	if args.has("hide-cover") and cover != null:
		# --hide-cover: only the Cover VegetationField3D hidden (grass, forest and the far bake kept).
		cover.visible = false
		_log("cover field hidden")
	if args.has("hide-veg"):
		# --hide-veg: the Cover and forest fields not drawn (diagnostics).
		for f in [cover, veg] + sward:
			if f != null and not (args.has("keep-turf") and f.name == "Turf"):
				f.visible = false # --keep-turf: the short turf layer stays (a cropped floor)
		_log("cover, forest and grass fields hidden")
	if args.has("light-probe") and veg != null and veg.has_method("get_light_at"):
		# --light-probe: the forest field's light factor at ground points 20, 40 and 80 m along the view.
		var fwd := -cam.global_transform.basis.z
		fwd.y = 0.0
		fwd = fwd.normalized()
		for d in [20.0, 40.0, 80.0]:
			var q: Vector3 = cam.global_position + fwd * d
			q.y = height_at(q.x, q.z)
			_log("light probe %d m at %s: get_light_at %.4f, shade %.4f" % [int(d), str(q.round()), veg.get_light_at(q), veg.get_shade_at(q)])
	if args.has("unshaded"):
		# --unshaded: albedo only, no light, on the capture frames (splits colour from lighting).
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_UNSHADED
		_log("capture drawn unshaded")
	if not args.has("raster"):
		# Path tracing builds wind-animated surfaces under a per-frame budget (about 50 frames for the meadow):
		# wait until none are pending before the frames that accumulate (--pt-wait-max, default 400 frames).
		var pt_waited := 0
		while pt_waited < int(args.get("pt-wait-max", "400")) and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
			await RenderingServer.frame_post_draw
			pt_waited += 1
		_log("path tracer: surfaces ready after %d frames (%d pending)" % [pt_waited, get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES)])
	for i in frames:
		if args.has("far-published") and (i == frames - 1 or i % 40 == 0):
			# --far-published: the widest grass field's published far state through the capture (view match rounds).
			for g in sward:
				if g.has_method("get_far_published"):
					_log("frame %d %s far published %s" % [i, g.name, str(g.get_far_published())])
					break
		if args.has("no-fog") and env != null:
			# --no-fog: volumetric and depth fog held off for every capture frame (the mist switches it on by itself).
			env.volumetric_fog_enabled = false
			env.fog_enabled = false
			if i == 0:
				_log("fog and volumetric fog forced off for the capture")
		if args.has("false-colour"):
			_false_colour(i == 0)
		if args.has("global-every"):
			# --global-every: the --global-param values set again every frame (fields re-send their globals).
			for gk in global_overrides:
				RenderingServer.global_shader_parameter_set(gk, global_overrides[gk])
		if args.get("mushrooms", "") == "off" and i % 20 == 0:
			_hide_mushrooms() # streamed tiles rebuilt since keep their mushrooms otherwise
		if no_till_tex != null:
			RenderingServer.global_shader_parameter_set("terrain_geology_map", no_till_tex.get_rid())
		await get_tree().process_frame
	if prints != null and prints.has_method("is_ripples_ready"):
		# Wind ripples grow on the sand round the camera: wait for them, then about 2 s for the sand mask.
		var rw := 0
		while rw < 600 and not prints.is_ripples_ready():
			await get_tree().process_frame
			rw += 1
		var t_ready := Time.get_ticks_msec()
		while Time.get_ticks_msec() - t_ready < 2000:
			await get_tree().process_frame
		_log("ripples ready after %d more frames" % rw)
		var rh: PackedFloat32Array = prints.get_ripple_heights()
		var rs := 0.0
		for v in rh:
			rs += v * v
		var cam3 := get_viewport().get_camera_3d()
		if cam3 != null and prints.has_method("get_material_at"):
			_log("ground under the camera: %s" % str(prints.get_material_at(cam3.global_position)))
		_log("ripple window: %d cells, rms %.2f mm; ground holes %d" % [rh.size(), 1000.0 * sqrt(rs / maxf(rh.size(), 1.0)), gen.get_hole_count() if gen != null and gen.has_method("get_hole_count") else -1])
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
	if _tree_gen != null and _tree_gen.has_method("get_near_builds"):
		# Near tree meshes still building: wait (saving, or quitting, with builds in flight hangs; trees.md 8e).
		var nb := 0
		while nb < 1200 and int(_tree_gen.call("get_near_builds")) > 0:
			await get_tree().process_frame
			nb += 1
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
		_log("saved %s (%d primitives in the frame)" % [out, int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))])
		if args.has("pt-pair") and not env.pathtracing_enabled:
			# --pt-pair=PATH (with --raster): the same moment path traced, from this run (Flow is not repeatable run to
			# run): the scene paused, path tracing on, TAA off, --spp-frames more frames once the tracer is ready.
			process_mode = Node.PROCESS_MODE_ALWAYS
			get_tree().paused = true
			get_viewport().use_taa = false
			env.pathtracing_enabled = true
			var w2 := 0
			while RenderingServer.has_method("is_pathtracing_ready") and w2 < 400 and not RenderingServer.call("is_pathtracing_ready"):
				await get_tree().process_frame
				w2 += 1
			for i in int(args.get("spp-frames", "4")) + int(args.get("pt-pair-frames", "8")):
				await get_tree().process_frame
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(args["pt-pair"])
			_log("saved %s (path traced pair, paused)" % args["pt-pair"])
		if data_map != null and data_map.has_method("get_aridity_at"):
			var cq: Camera3D = get_viewport().get_camera_3d()
			var gq := cq.global_position - cq.global_basis.z * 6.0
			gq.y = height_at(gq.x, gq.z)
			_log("ground 6 m ahead: aridity %.2f" % data_map.get_aridity_at(gq))
		if cover != null and cover.has_method("get_far_match_stats"):
			_log("far match %s" % str(cover.get_far_match_stats()))
		if landscape:
			_log("landscape at the frame %s" % [landscape.get_stats()])
		if weather != null and weather.aerosol_haze:
			var cam4: Camera3D = get_viewport().get_camera_3d()
			_log("aerosol haze %s /m at the camera, volumetric fog %s, length %.0f m" % [str(weather.get_precipitation_extinction_at(cam4.global_position)), env.volumetric_fog_enabled, env.volumetric_fog_length])
			for fv in weather.get_children(true):
				if fv is FogVolume:
					_log("haze volume at %s size %s density %s falloff %s cam %s" % [str(fv.global_position), str(fv.size), str(fv.material.density), str(fv.material.height_falloff), str(cam4.global_position)])
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
			var ri := int(args.get("surface-dump", "0"))
			if ri < 0:
				# The river passing nearest the --look point.
				var lp := _vec3_arg(args.get("look", "0,0,0"))
				var bd := 1e30
				for k in rivers.size():
					var cvk: Curve3D = rivers[k].curve
					for j in cvk.point_count:
						var q: Vector3 = rivers[k].global_transform * cvk.get_point_position(j)
						var dq := Vector2(q.x - lp.x, q.z - lp.z).length()
						if dq < bd:
							bd = dq
							ri = k
				_log("  surface-dump: river %d passes %.0f m from the look point" % [ri, bd])
			var rv = rivers[ri]
			var cv: Curve3D = rv.curve
			for k in range(0, cv.point_count, 2):
				var p: Vector3 = rv.global_transform * cv.get_point_position(k)
				_log("  surf %d (%.0f, %.0f) ground %.2f surface %.2f depth %.2f" % [k, p.x, p.z, gen.get_height(p), rv.sample_height(p), rv.sample_depth(p)])
	if args.has("height-line"):
		# --height-line=x0,z0,x1,z1,n: the ground along a line (generator and drawn landscape), for steps and bands.
		var hl: PackedStringArray = args["height-line"].split(",")
		var a0 := Vector2(float(hl[0]), float(hl[1]))
		var a1 := Vector2(float(hl[2]), float(hl[3]))
		var hn := int(hl[4])
		for k in hn + 1:
			var q := a0.lerp(a1, float(k) / hn)
			_log("  line %.2f (%.2f, %.2f) gen %.3f drawn %.3f" % [a0.distance_to(q), q.x, q.y, gen.get_height(Vector3(q.x, 0, q.y)), height_at(q.x, q.y)])
	if args.has("probe-pixel"):
		# --probe-pixel=x,y: what is drawn there (the ground under it, and every visual whose box covers it).
		var pp: PackedStringArray = args["probe-pixel"].split(",")
		var cam3: Camera3D = get_viewport().get_camera_3d()
		var px := Vector2(float(pp[0]), float(pp[1]))
		var o := cam3.project_ray_origin(px)
		var dv := cam3.project_ray_normal(px)
		var hit := Vector3.ZERO
		for i in 40000:
			var q := o + dv * (i * 0.25)
			if q.y < gen.get_height(q):
				hit = q
				break
		_log("probe %s: ground at %s" % [px, hit])
		var stack: Array[Node] = [self]
		while not stack.is_empty():
			var n: Node = stack.pop_back()
			for ch in n.get_children(true):
				stack.append(ch)
			if n is VisualInstance3D and n.is_visible_in_tree():
				var bb: AABB = n.global_transform * n.get_aabb()
				if bb.size.x < 3000.0 and bb.grow(1.0).has_point(Vector3(hit.x, clampf(hit.y, bb.position.y, bb.end.y), hit.z)):
					_log("  %s (%s) box %s" % [n.get_path(), n.get_class(), bb])
	if args.has("river-falls"):
		for r in rivers:
			var pc: Curve3D = r.curve
			if pc != null and pc.point_count > 1:
				_log("  river %s from %s to %s" % [r.name, r.to_global(pc.get_point_position(0)), r.to_global(pc.get_point_position(pc.point_count - 1))])
			for rf in r.get_waterfalls():
				_log("  river %s fall at %s, %.1f m high, %.1f m wide, throw %.1f m" % [r.name, rf["lip"], rf["height"], rf["width"], rf["throw"]])
	for wf in find_children("Waterfalls", "", true, false):
		for ch in wf.get_children(true):
			if ch.name.begins_with("FallVeil") or ch.name.begins_with("FallMist"):
				_log("  %s max density %.3f g/m3, size %s" % [ch.name, ch.get_max_density(), ch.size])
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


# --bracken (with --floor=oak): the camera 8 m from the densest bracken patch the cover placed round the first
# view, side-lit (the sun square to the line of sight), for the season pairs (--day=200 vs --day=345).
func _frame_bracken() -> void:
	var n := 0
	get_viewport().disable_3d = true # placing only: a drawn frame with real trees takes minutes here
	while not cover.is_stream_idle() and n < 600:
		await get_tree().process_frame
		n += 1
	get_viewport().disable_3d = false
	var idx := -1
	for i in cover.biome.species.size():
		if (cover.biome.species[i] as Resource).has_meta("bench_bracken"):
			idx = i
	if idx < 0:
		_log("bracken: no bracken species (needs --floor=oak)")
		return
	var pts: PackedVector3Array = cover.get_placements(idx)
	var best := Vector3.INF
	var best_n := 0
	var cells := {} # 4 m cells: the fullest one is the patch (a pairwise count is too slow on thousands of plants)
	for a in pts:
		var k := Vector2i(floori(a.x / 4.0), floori(a.z / 4.0))
		cells[k] = cells.get(k, 0) + 1
	for a in pts:
		var c: int = cells[Vector2i(floori(a.x / 4.0), floori(a.z / 4.0))]
		if c > best_n:
			best_n = c
			best = a
	if best == Vector3.INF:
		_log("bracken: none placed within the cover's reach of the first view")
		return
	var to_sun := sun.global_basis.z
	var side := Vector3(to_sun.x, 0.0, to_sun.z).normalized().cross(Vector3.UP).normalized()
	var cp := best + side * 8.0
	cp.y = height_at(cp.x, cp.z) + 1.7
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = cp
	c3.look_at(best + Vector3.UP * 0.4)
	_log("bracken cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f" % [cp.x, cp.y, cp.z, best.x, best.y + 0.4, best.z])
	n = 0
	await get_tree().process_frame
	get_viewport().disable_3d = true # placing only: a drawn frame with real trees takes minutes here
	while not cover.is_stream_idle() and n < 600:
		await get_tree().process_frame
		n += 1
	get_viewport().disable_3d = false
	if veg != null and veg.streaming:
		await _forest_settle()
	_log("bracken: %d plants, patch of %d at %s, camera %s (side-lit), cover settled after %d frames" % [pts.size(), best_n, str(best.round()), str(cp.round()), n])


# --log (with --trees=generated): the camera 2.5 m off the end of a fallen log near the first view, looking along it,
# the log picked square to the sun so it is side-lit (deadwood plants pair; VEG_DW_OFF=1 for the before frame).
func _frame_log() -> void:
	var to_sun := sun.global_basis.z
	var sun_flat := Vector2(to_sun.x, to_sun.z).normalized()
	var c3: Camera3D = get_viewport().get_camera_3d()
	var best_score := -INF
	var best_a := Vector3.ZERO
	var best_b := Vector3.ZERO
	var nlogs := 0
	for i in veg.biome.species.size():
		var sp := veg.biome.species[i] as Resource
		if sp == null or not String(sp.resource_name).contains("log)"): # "(log)", "(nurse log)"
			continue
		# --log-match=TEXT: only logs whose species name holds TEXT ("fallen giant": the old-growth gap logs).
		if args.has("log-match") and not String(sp.resource_name).contains(String(args["log-match"])):
			continue
		for tp in veg.get_placements(i):
			if tp.distance_to(c3.global_position) > float(args.get("log-radius", "70")):
				continue
			# The axis: the heading along which the ground stays under the log (get_deadwood_at side < 0).
			var dir := Vector3.ZERO
			var low := INF
			for k in 24:
				var ang := TAU * k / 24.0
				var d := Vector3(cos(ang), 0.0, sin(ang))
				var h: Dictionary = veg.get_deadwood_at(tp + d * 1.5, 1.0)
				if not h.is_empty() and float(h["side"]) < low:
					low = float(h["side"])
					dir = d
			if dir == Vector3.ZERO or low > 0.0:
				continue
			nlogs += 1
			var ends := []
			for sgn in [1.0, -1.0]:
				var e := tp
				for st in 40:
					var q: Vector3 = tp + dir * sgn * 0.5 * (st + 1)
					var h2: Dictionary = veg.get_deadwood_at(q, 1.0)
					if h2.is_empty() or float(h2["side"]) > 0.0:
						break
					e = q
				ends.append(e)
			var length: float = (ends[0] as Vector3).distance_to(ends[1])
			var radius: float = float(veg.get_deadwood_at(tp, 1.0).get("radius", 0.0))
			# Side-lit: the log square to the sun; a long, thick log; open sky over it (little stand shade).
			var square := 1.0 - absf(Vector2(dir.x, dir.z).dot(sun_flat))
			var score := square * 2.0 + minf(length, 10.0) * 0.4 + radius * 2.0 - veg.get_stand_shade_at(tp)
			if score > best_score:
				best_score = score
				best_a = ends[0]
				best_b = ends[1]
	if best_score == -INF:
		_log("log: no fallen log within %s m of the first view" % args.get("log-radius", "70"))
		return
	var axis := (best_a - best_b).normalized()
	# Beside the log a fifth of the way in, looking down along it: the near (sawn-looking) end stays behind the camera
	# and the far end beyond the bottom-up view of the trunk's middle.
	# On the log's sunlit side, so the camera sees the lit flank (--log-side, --log-height, --log-at to frame closer).
	var side := axis.cross(Vector3.UP).normalized()
	if Vector2(side.x, side.z).dot(sun_flat) < 0.0:
		side = -side
	var cp := best_a.lerp(best_b, float(args.get("log-from", "0.15"))) + side * float(args.get("log-side", "1.3"))
	cp.y = height_at(cp.x, cp.z) + float(args.get("log-height", "1.2"))
	var mid := best_a.lerp(best_b, float(args.get("log-at", "0.55")))
	mid.y = height_at(mid.x, mid.z) + 0.1
	c3.global_position = cp
	c3.look_at(mid)
	var n := 0
	await get_tree().process_frame
	get_viewport().disable_3d = true # placing only: a drawn frame with real trees takes minutes here
	while not cover.is_stream_idle() and n < 600:
		await get_tree().process_frame
		n += 1
	get_viewport().disable_3d = false
	if veg.streaming:
		await _forest_settle()
	_log("log: %d logs near, picked %.1f m long, sun squareness score %.2f" % [nlogs, best_a.distance_to(best_b), best_score])
	_log("log cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f" % [cp.x, cp.y, cp.z, mid.x, mid.y, mid.z])


# --mushrooms[=off] (with --trees=generated --floor=beech and an autumn --day): half a metre from a honey fungus tuft
# at the sunlit edge of a fallen log, below knee height, looking slightly down along the log; a birch with three fly
# agarics under it 1.5 to 2 m behind (the beech stand has no birch, so those are set by hand, as on the vegetation
# bench). --mushrooms=off: the same camera with every mushroom hidden. Logs "mushroom cam".
func _frame_mushrooms() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var to_sun := sun.global_basis.z
	var sun_flat := Vector3(to_sun.x, 0.0, to_sun.z).normalized()
	var honey := -1
	for i in cover.biome.species.size():
		var sp := cover.biome.species[i] as PlantSpecies
		if sp != null and sp.has_meta("mushroom") and sp.deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
			honey = i
	if honey < 0:
		_log("mushrooms: no honey fungus in the floor set")
		return
	var best := -INF
	var tuft := Vector3.INF
	var away := Vector3.RIGHT
	var n := 0
	for p in cover.get_placements(honey):
		if p.distance_to(c3.global_position) > float(args.get("log-radius", "70")):
			continue
		var h: Dictionary = veg.get_deadwood_at(p, 1.2)
		if h.is_empty() or float(h["radius"]) < 0.12:
			continue
		n += 1
		var sx: Dictionary = veg.get_deadwood_at(p + Vector3(0.05, 0, 0), 1.2)
		var sz: Dictionary = veg.get_deadwood_at(p + Vector3(0, 0, 0.05), 1.2)
		if sx.is_empty() or sz.is_empty():
			continue
		var a := Vector3(float(sx["side"]) - float(h["side"]), 0, float(sz["side"]) - float(h["side"]))
		if a.length() < 0.005:
			continue
		a = a.normalized()
		# Clear of the bark yet at its edge, on the log's sunlit side, under open sky; the log running on both ways
		# (mid-log: its root flare is not between the camera and the tuft).
		var mid_log := 0.0
		var axis := Vector3(-a.z, 0, a.x)
		for sgn in [1.0, -1.0]:
			var q: Vector3 = p - a * (float(h["side"]) + float(h["radius"])) + axis * sgn * 1.5
			if not veg.get_deadwood_at(q, 1.0).is_empty():
				mid_log += 1.0
		var score := a.dot(sun_flat) * 2.0 - absf(float(h["side"]) - 0.22) * 4.0 - veg.get_stand_shade_at(p) * 2.0 + mid_log
		if score > best:
			best = score
			tuft = p
			away = a
	if tuft == Vector3.INF:
		_log("mushrooms: no honey tuft at a log within reach (%d placed beside logs)" % n)
		return
	var side := Vector3(-away.z, 0, away.x)
	if side.dot(sun_flat) > 0.0:
		side = -side # looking along the log away from the sun: the sun behind the camera
	var dir := (side - away * 0.6).normalized()
	var eye := tuft - dir * float(args.get("mush-dist", "0.5")) + away * 0.2
	eye.y = height_at(eye.x, eye.z) + float(args.get("mush-height", "0.36"))
	var look := Vector3(tuft.x, height_at(tuft.x, tuft.z) + 0.2, tuft.z) + dir * 0.35
	c3.fov = 50.0
	c3.near = 0.15
	c3.global_position = eye
	c3.look_at(look)
	env.tonemap_exposure = float(args.get("exposure", "1.4"))
	# The birch and its fly agarics, behind the tuft along the view.
	var tg := TreeGenerator.new()
	tg.preset = TreeGenerator.PRESET_BIRCH
	tg.seed = 3
	var birch := MeshInstance3D.new()
	birch.name = "BenchBirch"
	birch.mesh = tg.generate()
	var bp := tuft + side * 2.6 + away * 0.6
	birch.position = Vector3(bp.x, height_at(bp.x, bp.z) - 0.05, bp.z)
	add_child(birch)
	var pg := PlantGenerator.new()
	pg.preset = PlantGenerator.PRESET_FLY_AGARIC
	pg.apply_preset_colors()
	for k in 3:
		pg.seed = 7 + k
		var fa := MeshInstance3D.new()
		fa.name = "BenchFlyAgaric%d" % k
		fa.mesh = pg.generate()
		var fp := tuft + side * (1.6 + 0.2 * k) + away * (0.05 + 0.3 * k)
		fa.position = Vector3(fp.x, height_at(fp.x, fp.z), fp.z)
		fa.rotation.y = 1.3 * k
		fa.visible = args.get("mushrooms", "") != "off"
		add_child(fa)
	var w := 0
	get_viewport().disable_3d = true
	await get_tree().process_frame
	while not cover.is_stream_idle() and w < 600:
		await get_tree().process_frame
		w += 1
	get_viewport().disable_3d = false
	if veg.streaming:
		await _forest_settle()
	if args.get("mushrooms", "") == "off":
		_hide_mushrooms()
	_log("mushrooms: %d honey tufts by logs, picked %s (score %.2f), away from the log %s" % [n, str(tuft.snapped(Vector3.ONE * 0.01)), best, str(away.snapped(Vector3.ONE * 0.01))])
	_log("mushroom cam %.2f,%.2f,%.2f look %.2f,%.2f,%.2f" % [eye.x, eye.y, eye.z, look.x, look.y, look.z])


# --mushrooms=off: hide the floor's mushroom MultiMeshes (the cover's tiles round the camera).
func _hide_mushrooms() -> void:
	var meshes := {}
	for sp in cover.biome.species:
		if sp is PlantSpecies and (sp as PlantSpecies).has_meta("mushroom"):
			for m in sp.meshes:
				meshes[m] = true
			for m in sp.near_meshes:
				meshes[m] = true
	for ch in cover.find_children("*", "MultiMeshInstance3D", true, false):
		var mmi := ch as MultiMeshInstance3D
		if mmi.multimesh and meshes.has(mmi.multimesh.mesh):
			mmi.visible = false


# --pollards: a willow, a beech and a hazel pollard (TreeGenerator cut_height/cut_cycle/cut_since, seed 1) 5 m apart,
# --pollard-dist (16) m ahead of the camera across the sun (the sun from the side), the camera 1.7 m up; with an
# --day in winter they stand bare. --pollard-look=willow|beech|hazel aims at one head instead of the middle.
func _place_pollards() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var dir := sf.cross(Vector3.UP).normalized() # looking across the sun
	var across := dir.cross(Vector3.UP).normalized()
	var base := Vector3(c3.global_position.x, 0, c3.global_position.z)
	var mid := base + dir * float(args.get("pollard-dist", "16"))
	var season: Dictionary = weather.get_season() if weather != null and weather.has_method("get_season") else {}
	var specs := [
		{"name": "willow", "preset": 12, "cut_height": 2.2, "cut_cycle": 6, "cut_since": 3, "years": 80},
		{"name": "beech", "preset": 1, "cut_height": 2.5, "cut_cycle": 14, "cut_since": 60, "years": 160, "veteran": 0.7, "lichen_cover": 1.2},
		{"name": "hazel", "preset": 5, "cut_height": 0.15, "cut_cycle": 8, "cut_since": 5, "years": 60},
	]
	if args.get("pollard-set", "") == "bole":
		# --pollard-set=bole: an uncut birch with a beech 7 m behind it (the bark chevron shot), not the pollards.
		specs = [
			{"name": "birch", "preset": 2, "years": 70},
			{"name": "beech", "preset": 1, "years": 140, "lichen_cover": float(args.get("bole-lichen", "0.3"))},
		]
	if args.get("pollard-set", "") == "broadleaves":
		# --pollard-set=broadleaves: open-grown aspen, black poplar, wild cherry, hornbeam and sweet chestnut (trees
		# a3be1409, forest_shade 0) in a row --pollard-gap m apart; the cherry in the middle flowers in late April.
		specs = [
			{"name": "aspen", "preset": 50, "years": 45},
			{"name": "black_poplar", "preset": 51, "years": 70},
			{"name": "wild_cherry", "preset": 52, "years": 50},
			{"name": "hornbeam", "preset": 53, "years": 80},
			{"name": "sweet_chestnut", "preset": 54, "years": 90},
		]
	var aim := mid
	for k in specs.size():
		var sp: Dictionary = specs[k]
		var tg := TreeGenerator.new()
		tg.preset = int(sp["preset"])
		for key in sp:
			if key != "name" and key != "preset":
				tg.set(key, sp[key])
		tg.seed = 1
		# The scene's season (the SkyClock date: --month, 0-based, and --day of the month).
		var ts: Dictionary = season
		if "season" in tg:
			tg.season = ts
		elif tg.has_method("set_season"):
			tg.set_season(ts)
		if k == 0:
			_log("pollards season %s" % str(ts))
		var t := Time.get_ticks_msec()
		var mi := MeshInstance3D.new()
		mi.name = "Pollard_" + String(sp["name"])
		mi.mesh = tg.generate()
		var p := mid + across * (float(k) - 0.5 * float(specs.size() - 1)) * float(args.get("pollard-gap", "5"))
		if args.get("pollard-set", "") == "bole":
			# Behind the birch as seen from the close-up camera (which stands on the sun side, rotated --pollard-sun-side).
			var cd := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("pollard-sun-side", "40"))))
			p = mid - cd * 7.0 * k + cd.cross(Vector3.UP) * 1.5 * k
		mi.position = Vector3(p.x, height_at(p.x, p.z) - 0.05, p.z)
		mi.rotation.y = 0.9 * k
		add_child(mi)
		if args.get("pollard-look", "") == sp["name"]:
			aim = Vector3(p.x, 0, p.z)
		_log("pollard %s at %s in %d ms" % [sp["name"], str(mi.position.snapped(Vector3.ONE * 0.1)), Time.get_ticks_msec() - t])
	c3.global_position = Vector3(base.x, height_at(base.x, base.z) + 1.7, base.z)
	if args.has("pollard-close") and args.has("pollard-look"):
		# --pollard-close=D: a close-up of the --pollard-look tree from D m, the sun --pollard-sun-side (40) degrees
		# off the camera's back, eye at --pollard-eye (1.7) m.
		var rot := deg_to_rad(float(args.get("pollard-sun-side", "40")))
		var cdir := sf.rotated(Vector3.UP, rot)
		var cp := aim + cdir * float(args["pollard-close"])
		c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + float(args.get("pollard-eye", "1.7")), cp.z)
		_log("pollard close-up of %s from %.1f m, sun elevation %.1f deg" % [args["pollard-look"], float(args["pollard-close"]), rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0)))])
	var look_h := float(args.get("pollard-look-h", "3.5"))
	c3.look_at(Vector3(aim.x, height_at(aim.x, aim.z) + look_h, aim.z))
	_log("pollards cam %.1f,%.1f,%.1f looking across the sun" % [c3.global_position.x, c3.global_position.y, c3.global_position.z])


# Uprooted logs (TreeGenerator `uprooted`, mesh meta tree_root_pit = centre in mesh space, radius, depth): the pit the
# root plate came out of, dug into the ground as a bowl TerrainBrush3D touch-up under the generator, within
# --pit-radius (300 m) of the camera; the ground mesh is rebuilt after. Logs "root pits".
# --trunk-view: the nearest tree trunk (a placed mesh over 10 m tall) from --trunk-dist (1.6) m off its axis,
# eye 1.2 m, the sun 40 degrees off the camera's back. --sticks-view: the nearest thin fallen stick (deadwood
# mesh, widest under 6 cm) from --sticks-dist (1.3) m, looking down at its middle. Logs "wood close-up".
func _frame_wood_closeup() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var cp := c3.global_position
	var best := INF
	var target := Vector3.INF
	var sticks := args.has("sticks-view")
	for ch in veg.find_children("*", "MultiMeshInstance3D", true, false):
		var mmi := ch as MultiMeshInstance3D
		if mmi.multimesh == null or mmi.multimesh.mesh == null:
			continue
		var mesh := mmi.multimesh.mesh
		var axis: PackedFloat32Array = mesh.get_meta("tree_deadwood_axis", PackedFloat32Array())
		if sticks:
			if axis.size() < 8 or axis[6] > 0.06:
				continue
		elif axis.size() > 0 or mesh.get_aabb().size.y < 10.0:
			continue
		var mm := mmi.multimesh
		for k in mm.instance_count:
			var xf: Transform3D = mmi.global_transform * mm.get_instance_transform(k)
			var p: Vector3 = xf * ((Vector3(axis[0], axis[1], axis[2]) + Vector3(axis[3], axis[4], axis[5])) * 0.5) if sticks else xf.origin
			var d := Vector2(p.x - cp.x, p.z - cp.z).length()
			if d < best:
				best = d
				target = p
	if target == Vector3.INF:
		_log("wood close-up: nothing found")
		return
	var to_s := sun.global_basis.z
	var sfl := Vector3(to_s.x, 0, to_s.z).normalized().rotated(Vector3.UP, deg_to_rad(float(args.get("wood-sun-side", "40"))))
	var dist := float(args.get("sticks-dist", "1.3")) if sticks else float(args.get("trunk-dist", "1.6"))
	var q := target + sfl * dist
	var eye := float(args.get("wood-eye", "1.2"))
	c3.global_position = Vector3(q.x, height_at(q.x, q.z) + eye, q.z)
	if sticks:
		c3.look_at(Vector3(target.x, height_at(target.x, target.z) + 0.05, target.z))
	else:
		c3.look_at(Vector3(target.x, height_at(target.x, target.z) + eye, target.z))
	c3.near = 0.05
	_log("wood close-up: %s at %s (%.1f m from the start camera), camera %s" % ["sticks" if sticks else "trunk", str(target.round()), best, str(c3.global_position.round())])


func _dig_root_pits() -> void:
	var t := Time.get_ticks_msec()
	var c3: Camera3D = get_viewport().get_camera_3d()
	var reach := float(args.get("pit-radius", "300"))
	var n := 0
	var seen := {}
	for ch in veg.find_children("*", "MultiMeshInstance3D", true, false):
		var mmi := ch as MultiMeshInstance3D
		if mmi.multimesh == null or mmi.multimesh.mesh == null or not mmi.multimesh.mesh.has_meta("tree_root_pit"):
			continue
		var pit: PackedFloat32Array = mmi.multimesh.mesh.get_meta("tree_root_pit")
		var mm := mmi.multimesh
		for k in mm.instance_count:
			var xf: Transform3D = mmi.global_transform * mm.get_instance_transform(k)
			var c: Vector3 = xf * Vector3(pit[0], pit[1], pit[2])
			if c.distance_to(c3.global_position) > reach:
				continue
			var key := Vector2i(roundi(c.x * 4.0), roundi(c.z * 4.0))
			if seen.has(key):
				continue # the same log in two LOD MultiMeshes
			seen[key] = true
			var sc := xf.basis.get_scale().x
			var br := TerrainBrush3D.new()
			br.name = "RootPit%d" % n
			br.mode = TerrainBrush3D.MODE_LOWER
			br.profile = TerrainBrush3D.PROFILE_BOWL
			br.position = Vector3(c.x, height_at(c.x, c.z), c.z)
			br.size = Vector2.ONE * pit[3] * 2.0 * sc
			br.depth = pit[4] * sc
			br.falloff = 0.4
			br.cell_size = 0.25
			gen.add_child(br)
			n += 1
			if n <= 5:
				_log("root pit at %s, radius %.2f m, depth %.2f m" % [str(br.position.snapped(Vector3.ONE * 0.1)), pit[3] * sc, pit[4] * sc])
	if n == 0:
		_log("root pits: no uprooted log within %.0f m" % reach)
		return
	gen.apply_touch_ups()
	if landscape != null:
		landscape.rebuild()
		landscape.load_all_heights()
	_log("root pits: %d dug in %d ms (touch-ups %d cells)" % [n, Time.get_ticks_msec() - t, gen.get_last_touch_up_cells()])
	if args.has("pit-view"):
		# --pit-view: 5 m from the root pit nearest the camera, on its sunlit side, 1.6 m up, looking at the plate.
		var best := Vector3.INF
		for br in gen.find_children("RootPit*", "TerrainBrush3D", true, false):
			var bp: Vector3 = (br as Node3D).position
			if bp.distance_to(c3.global_position) < best.distance_to(c3.global_position):
				best = bp
		var to_sun := sun.global_basis.z
		var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
		var side := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("pit-angle", "25"))))
		var cp := best + side * float(args.get("pit-dist", "5"))
		c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + float(args.get("pit-height", "1.6")), cp.z)
		c3.look_at(best + Vector3(0, 0.6, 0))
		_log("pit cam %.2f,%.2f,%.2f look %.2f,%.2f,%.2f" % [c3.global_position.x, c3.global_position.y, c3.global_position.z, best.x, best.y + 0.6, best.z])
		var w := 0
		get_viewport().disable_3d = true
		while cover != null and not cover.is_stream_idle() and w < 600:
			await get_tree().process_frame
			w += 1
		get_viewport().disable_3d = false
		if veg.streaming:
			await _forest_settle()


# --cover-probe: the far ground's inputs after the bakes (no picture): each sward layer's far colour and lobes, the
# world field's cover map under the view (near 30-38 m and far 45-150 m strips along the camera's heading, 20 m
# either side), the terrain material's far start, the near radius and the sun.
func _cover_probe() -> void:
	for g in sward:
		var lobes: Variant = g.get_far_color_lobes() if g.has_method("get_far_color_lobes") else null
		_log("probe %s: far colour %s, baked %s, lobes %s" % [g.name, str(g.get_far_color()), str(g.is_far_color_baked()), str(lobes)])
	for g in sward:
		if g.has_method("get_far_published"):
			_log("probe %s far published %s" % [g.name, str(g.get_far_published())])
	var cf := -cam.global_transform.basis.z
	_log("probe camera forward on the ground (xz) %s" % str(Vector2(cf.x, cf.z).normalized()))
	for gn in ["grass_ground_far", "grass_ground_far_scale", "grass_ground_sun"]:
		_log("probe global %s = %s" % [gn, str(RenderingServer.global_shader_parameter_get(gn))])
	var tex: Texture2D = veg.get_cover_texture() if veg != null else null
	var rect_v: Variant = RenderingServer.global_shader_parameter_get("terrain_cover_rect")
	var wr: Rect2 = gen.get_world_rect()
	var rect := wr
	if rect_v is Vector4:
		rect = Rect2(rect_v.x, rect_v.y, rect_v.z, rect_v.w)
	_log("probe cover map %s, rect from the global %s, generator world rect %s (used %s)" % [str(tex.get_size()) if tex else "missing", str(rect_v), str(wr), str(rect)])
	if tex != null:
		var img := tex.get_image()
		if img.is_compressed():
			img.decompress()
		var c3: Camera3D = get_viewport().get_camera_3d()
		var fwd := -c3.global_basis.z
		fwd.y = 0.0
		fwd = fwd.normalized()
		var side := fwd.cross(Vector3.UP)
		for strip in [[30.0, 38.0, "near 30-38 m"], [45.0, 150.0, "far 45-150 m"]]:
			var acc := Color(0, 0, 0, 0)
			var n := 0
			var d: float = strip[0]
			while d <= strip[1]:
				for l in range(-20, 21, 4):
					var q: Vector3 = c3.global_position + fwd * d + side * float(l)
					var u := (q.x - rect.position.x) / rect.size.x
					var v := (q.z - rect.position.y) / rect.size.y
					if u >= 0.0 and u < 1.0 and v >= 0.0 and v < 1.0:
						acc += img.get_pixel(int(u * img.get_width()), int(v * img.get_height()))
						n += 1
				d += 2.0
			_log("probe cover map mean under the %s strip: %s (%d texels)" % [strip[2], str(acc / maxf(n, 1)), n])
	var tm: Variant = landscape.material if landscape != null else null
	if tm != null:
		_log("probe terrain material far_start %s, far_fade %s, far_start_from_vegetation %s" % [str(tm.get("far_start")), str(tm.get("far_fade")), str(tm.get("far_start_from_vegetation"))])
	_log("probe vegetation near radius %.1f m" % VegetationField3D.get_near_radius())
	_log("probe sun energy %.2f, elevation %.1f deg" % [sun.light_energy, rad_to_deg(asin(clampf(sun.global_basis.z.y, -1.0, 1.0)))])


# --false-colour (vegetation's lime band diagnostic): the grass ground red, the terrain far blend blue, the plants'
# far colour off, set every capture frame; logs the far start/fade, stream radii and sward settings once.
var _fc_tex: ImageTexture
func _false_colour(log_once: bool) -> void:
	var rs := RenderingServer
	rs.global_shader_parameter_set("grass_ground_far", Vector4(1, 0, 0, 1))
	rs.global_shader_parameter_set("grass_ground_under_far", Vector4(1, 0, 0, 1))
	rs.global_shader_parameter_set("grass_ground_far_scale", Vector4(1, 1, 1, 1))
	rs.global_shader_parameter_set("grass_ground_sun", Vector4(0, 0, 0, 0))
	rs.global_shader_parameter_set("vegetation_far_rect", Vector4(0, 0, 0, 0))
	if _fc_tex == null:
		var img := Image.create(4, 4, false, Image.FORMAT_RGBA8)
		img.fill(Color(0, 0, 1, 1))
		_fc_tex = ImageTexture.create_from_image(img)
	rs.global_shader_parameter_set("terrain_cover_map", _fc_tex.get_rid())
	rs.global_shader_parameter_set("terrain_cover_rect", Vector4(-2048, -2048, 4096, 4096))
	if log_once:
		var tm: Variant = landscape.material if landscape != null else null
		if tm != null:
			var fs: Variant = tm.get_shader_parameter("tm_far_start") if tm.has_method("get_shader_parameter") else null
			var ff: Variant = tm.get_shader_parameter("tm_far_fade") if tm.has_method("get_shader_parameter") else null
			_log("false colour: terrain material %s, tm_far_start %s, tm_far_fade %s (far_start %s, far_fade %s, from vegetation %s)" % [tm.get_class(), str(fs), str(ff), str(tm.get("far_start")), str(tm.get("far_fade")), str(tm.get("far_start_from_vegetation"))])
		for f in [cover, veg]:
			if f != null:
				_log("false colour: %s stream_radius %.1f, streaming %s" % [f.name, f.stream_radius, str(f.streaming)])
		for g in sward:
			_log("false colour: %s radius %.1f, ground_tint_enabled %s" % [g.name, g.radius, str(g.ground_tint_enabled)])


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


# Real ground scans (Poly Haven CC0, art repository terrain/layers, fetched by docs/bench/landscape/fetch_art.sh into
# docs/bench/landscape/art/terrain_layers). Each layer keeps the geology's colour (detail_only: the scan adds detail
# only, to colour and roughness) and tiles at the scan's real size. Loaded from the files directly (the folder is .gdignore'd, no import step).
const GROUND_SCANS := {"rock": ["dark_rock_02", 2.0], "scree": ["rocks_ground_02", 2.0], "soil": ["forest_ground_06", 2.11], "alluvium": ["gravelly_sand", 2.48], "sand": ["sand_03", 2.0]}

func _ground_scan_tex(path: String, normal := false) -> Texture2D:
	var img := Image.load_from_file(path)
	if img == null or img.is_empty():
		return null
	img.generate_mipmaps(normal)
	return ImageTexture.create_from_image(img)

func _apply_ground_scans(tm: TerrainMaterial3D, dir: String, only := "1") -> int:
	var n := 0
	for layer_name in GROUND_SCANS:
		if only != "1" and not (layer_name in only.split(",")):
			continue # --ground-scans=rock,scree: those layers only
		var scan: String = GROUND_SCANS[layer_name][0]
		var base := dir.path_join(scan).path_join(scan)
		if not FileAccess.file_exists(base + "_albedo.jpg"):
			continue
		var l := TerrainMaterialLayer.new()
		var maps: String = args.get("scan-maps", "anrh") # --scan-maps=an: only the albedo and normal maps (A/B)
		if "a" in maps:
			l.albedo_texture = _ground_scan_tex(base + "_albedo.jpg")
		if "n" in maps:
			l.normal_texture = _ground_scan_tex(base + "_normal.jpg", true)
		if "r" in maps:
			l.roughness_texture = _ground_scan_tex(base + "_roughness.jpg")
		if "h" in maps:
			l.height_texture = _ground_scan_tex(base + "_height.jpg")
		l.tile_size = GROUND_SCANS[layer_name][1]
		var c: Color = tm.get_shader_parameter("tm_%s_color" % layer_name) if tm.get_shader_parameter("tm_%s_color" % layer_name) is Color else Color.WHITE
		l.color = c # the geology's colour: the scan's mean is divided out (detail_only)
		tm.set("%s_layer" % layer_name, l)
		n += 1
	return n

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
	# Sculpting guides (--guides: the bowl, ridges and bar as TerrainGuide3D nodes under the generator).
	var guide_nodes: Array[Node3D] = []
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 1.0 / 1500.0
	noise.fractal_octaves = 3
	var up := Image.create(64, 64, false, Image.FORMAT_RF)
	var lake_marker: Node3D = null
	if layout == "range" or layout == "desert":
		# A range across the north with a spur, uplift fading out towards a coastal plain in the south.
		var rng := _range_path("Range", [Vector3(-2000, 0, -900), Vector3(-200, 0, -1300), Vector3(2000, 0, -1000)], 0.0, 1.0)
		var rc := Curve3D.new()
		rc.add_point(Vector3(-2000, 0, -900), Vector3.ZERO, Vector3(600, 0, -300))
		rc.add_point(Vector3(-200, 0, -1300), Vector3(-500, 0, 0), Vector3(500, 0, 0))
		rc.add_point(Vector3(2000, 0, -1000), Vector3(-600, 0, -200))
		rng.curve = rc
		range_nodes.append(rng)
		if layout == "range":
			range_nodes.append(_range_path("Spur", [Vector3(700, 0, -1200), Vector3(1100, 0, 200)], 450.0, 0.6))
		for y in 64:
			var z := Z0 + (y + 0.5) / 64.0 * GEN_SIZE
			var s := 0.15 + 0.85 * (1.0 - smoothstep(-400.0, 1100.0, z))
			if layout == "desert":
				# The range front closer in, a plain barely rising (it stays under the sand's 15 degrees).
				s = 0.005 + 0.995 * (1.0 - smoothstep(-1100.0, -650.0, z))
			for x in 64:
				up.set_pixel(x, y, Color(s, 0, 0))
	else:
		# A cirque of peaks round the north holding a high lake; its outflow falls off the range front into a wide
		# grassy valley floor that runs south between two low ridges to the sea.
		range_nodes.append(_range_path("Cirque", [Vector3(-1900, 0, -700), Vector3(-900, 0, -1600), Vector3(0, 0, -1850), Vector3(900, 0, -1600), Vector3(1900, 0, -700)], 0.0, 1.0))
		if args.get("bowl", "none") != "none":
			# A high bowl among the peaks (x,z,radius,share): uplift scaled down inside it, so its floor stays gentle above
			# the rock bar that holds its outlet.
			var bw: PackedStringArray = args["bowl"].split(",")
			if args.has("guides"):
				guide_nodes.append(_guide("Bowl", TerrainGuide3D.MODE_LOWER, [Vector3(float(bw[0]), 0, float(bw[1]))], float(bw[2]), float(bw[3])))
			else:
				var bowl := _range_path("Bowl", [Vector3(float(bw[0]), 0, float(bw[1])), Vector3(float(bw[0]) + 1.0, 0, float(bw[1]))], float(bw[2]), float(bw[3]))
				bowl.set_meta("basin", true)
				range_nodes.append(bowl)
		if args.has("ridges"):
			# Extra ridges, "x,z,x,z,...,width,uplift" separated by ";": raised-cosine bands of uplift along the line.
			var ri := 0
			for spec in args["ridges"].split(";"):
				var rv: PackedStringArray = spec.split(",")
				var rpts: Array[Vector3] = []
				for k in range(0, rv.size() - 2, 2):
					rpts.append(Vector3(float(rv[k]), 0, float(rv[k + 1])))
				if args.has("guides"):
					guide_nodes.append(_guide("Ridge%d" % ri, TerrainGuide3D.MODE_RAISE, rpts, float(rv[rv.size() - 2]), float(rv[rv.size() - 1])))
				else:
					range_nodes.append(_range_path("Ridge%d" % ri, rpts, float(rv[rv.size() - 2]), float(rv[rv.size() - 1])))
				ri += 1
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
	# Heavy generator options are on by default: this bench sets each from its own flags (dunes on, --dunes=0 off).
	gen.glacial_enabled = false
	gen.glacial_converged = false
	gen.guide_preview = false
	gen.name = "Generator"
	gen.size = Vector2(GEN_SIZE, GEN_SIZE)
	gen.cell_size = float(args.get("cell", "16"))
	gen.seed = 3
	gen.uplift = float(args.get("uplift", "3.0"))
	if args.has("max-height"):
		gen.max_height = float(args["max-height"]) # --max-height=M: relief scaled so the highest point is M over the sea
	# The desert's plain: no uplift under it, filled with the range's alluvium.
	gen.base_uplift = float(args.get("base-uplift", "0.0" if layout == "desert" else "0.1"))
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
	# Droplet erosion's rills and gullies (terrain 01c51a76), on by default; --tiles=0 off (a before frame, fast scouts).
	gen.tile_erosion_enabled = args.get("tiles", "1") != "0"
	if "gully_incision" in gen:
		gen.gully_incision = float(args.get("gully-incision", "1"))
	gen.deposition = float(args.get("deposition", "0.5" if layout == "desert" else "0"))
	# Rock beds stepped into cliff faces (ledges), --ledges=0 for smooth faces.
	gen.cliff_ledge_spacing = float(args.get("ledges", "16"))
	gen.gorge_age = float(args.get("gorge-age", "12000"))
	gen.moraine_age = float(args.get("moraine-age", "0"))
	gen.scree_age = float(args.get("scree-age", "0"))
	gen.meander_age = float(args.get("meander-age", "0"))
	gen.outwash_age = float(args.get("outwash-age", "0"))
	gen.fan_backfill = not args.has("no-backfill")
	gen.dunes_enabled = args.get("dunes", "0" if layout == "desert" else "1") != "0"
	if gen.dunes_enabled:
		if args.get("dunes", "1") != "1":
			gen.dune_steps = int(args["dunes"])
		gen.dune_supply = float(args.get("dune-supply", str(gen.dune_supply)))
		gen.dune_belt_width = float(args.get("dune-belt", str(gen.dune_belt_width)))
		gen.dune_cell = float(args.get("dune-cell", str(gen.dune_cell)))
		gen.dune_wind_angle = float(args.get("dune-wind", "0"))
		gen.dune_vegetation = float(args.get("dune-veg", "1"))
	if layout == "desert" and not args.has("sand-sea"):
		args["sand-sea"] = "-2000,-650,4000,1900"
	if args.has("sand-sea"):
		var ss: PackedFloat64Array = Array(args["sand-sea"].split(",")).map(func(v): return float(v))
		gen.sand_sea_rect = Rect2(ss[0], ss[1], ss[2], ss[3])
		gen.sand_sea_kind = ["transverse", "barchan", "linear", "star"].find(args.get("sand-sea-kind", "transverse"))
		gen.sand_sea_depth = float(args.get("sand-sea-depth", str(gen.sand_sea_depth)))
		gen.sand_sea_wind_angle = float(args.get("sand-sea-wind", "0"))
		gen.sand_sea_scale = float(args.get("sand-sea-scale", str(gen.sand_sea_scale)))
		gen.sand_sea_steps = int(args.get("sand-sea-steps", str(gen.sand_sea_steps)))
	if args.has("glacial"):
		# Ice ages: glaciers carve troughs and cirques, and the last one leaves its basins as lakes.
		gen.glacial_enabled = true
		gen.glacial_ela = float(args.get("ela", "500"))
		gen.glacial_converged = args.has("ice-converged") # the steady ice solved to convergence
		gen.glacial_share = float(args.get("glacial-share", "0.5"))
		gen.glacial_final = float(args.get("glacial-final", "100000"))
		gen.glacial_rock_variation = float(args.get("rock-var", "0"))
		gen.glacial_rock_scale = float(args.get("rock-scale", "500"))
		gen.glacial_inflow_balance = float(args.get("ice-inflow", "0"))
		gen.glacial_coarse_cell = float(args.get("ice-coarse", "0"))
		gen.ice_worn_detail = args.get("ice-worn", "1") != "0"
		gen.glacial_erosion = float(args.get("glacial-erosion", "1"))
	if args.has("refine"):
		gen.refine_steps = int(args["refine"])
	if args.has("steps"):
		gen.steps = int(args["steps"]) # rivers drop sediment where they slow (Yuan et al. 2019)
	add_child(gen)
	# --coast-guide=x,z,x,z,...[;beach]: sea inside the closed line (repeat the first point) or left of an open one;
	# --beach-guide=x,z,x,z,...;width;beach: a drawn beach (0: cliffs) along the coast within width of the line.
	if args.has("coast-guide"):
		var cg: PackedStringArray = args["coast-guide"].split(";")
		var cv: PackedStringArray = cg[0].split(",")
		var cpts: Array[Vector3] = []
		for k in range(0, cv.size() - 1, 2):
			cpts.append(Vector3(float(cv[k]), 0, float(cv[k + 1])))
		var coast := _guide("Coast", TerrainGuide3D.MODE_COAST, cpts, 0.0, 0.0)
		coast.beach_width = float(cg[1]) if cg.size() > 1 else -1.0
		guide_nodes.append(coast)
	if args.has("beach-guide"):
		var bg: PackedStringArray = args["beach-guide"].split(";")
		var bv2: PackedStringArray = bg[0].split(",")
		var bpts: Array[Vector3] = []
		for k in range(0, bv2.size() - 1, 2):
			bpts.append(Vector3(float(bv2[k]), 0, float(bv2[k + 1])))
		var beach_g := _guide("Beach", TerrainGuide3D.MODE_BEACH, bpts, float(bg[1]), 0.0)
		beach_g.beach_width = float(bg[2])
		guide_nodes.append(beach_g)
	for g in guide_nodes:
		gen.add_child(g)
	# --touch=x,z,w,d,mode,y[,depth,falloff,bowl];...: TerrainBrush3D touch-ups under the generator (mode lower, raise,
	# flatten or add; y the brush's height), applied to the generated ground with the water re-solved.
	if args.has("touch"):
		var modes := {"lower": TerrainBrush3D.MODE_LOWER, "raise": TerrainBrush3D.MODE_RAISE, "flatten": TerrainBrush3D.MODE_FLATTEN, "add": TerrainBrush3D.MODE_ADD}
		var ti := 0
		for spec in args["touch"].split(";"):
			var tv: PackedStringArray = spec.split(",")
			var br := TerrainBrush3D.new()
			br.name = "Touch%d" % ti
			ti += 1
			br.position = Vector3(float(tv[0]), float(tv[5]), float(tv[1]))
			br.size = Vector2(float(tv[2]), float(tv[3]))
			br.mode = modes.get(tv[4], TerrainBrush3D.MODE_FLATTEN)
			br.depth = float(tv[6]) if tv.size() > 6 else 0.0
			br.falloff = float(tv[7]) if tv.size() > 7 else 40.0
			br.profile = TerrainBrush3D.PROFILE_BOWL if tv.size() > 8 and tv[8] == "bowl" else TerrainBrush3D.PROFILE_FLAT
			br.cell_size = 4.0
			gen.add_child(br)
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
		if args.has("bowl-inflow") and args.get("bowl", "none") != "none":
			# Land beyond the map draining into the side bowl (--bowl-inflow=km², at the bowl's centre or
			# --bowl-inflow-at=x,z): its own catchment past the ridges, so the hanging fall carries a real flow.
			var bw2: PackedStringArray = args["bowl"].split(",")
			var bin := Node3D.new()
			bin.name = "BowlInflow"
			var ba: String = args.get("bowl-inflow-at", "")
			bin.position = Vector3(float(ba.get_slice(",", 0)), 0, float(ba.get_slice(",", 1))) if ba.contains(",") else Vector3(float(bw2[0]), 0, float(bw2[1]))
			bin.set_meta("area", float(args["bowl-inflow"]))
			bin.set_meta("erosion", float(args.get("bowl-inflow-erosion", "0")))
			add_child(bin)
			markers.append(gen.get_path_to(bin))
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
		if args.get("bar", "none") != "none" and args.has("guides"):
			# The bar as a guide: its points' heights are the levels; the apron falls to the right of the line.
			var gbv: PackedStringArray = args["bar"].split(",")
			var gbl: PackedStringArray = args.get("bar-level", "450").split(",")
			var gpts: Array[Vector3] = []
			for k in range(0, gbv.size() - 1, 2):
				var lv := float(gbl[k / 2]) if gbl.size() == gbv.size() / 2 else float(gbl[0])
				gpts.append(Vector3(float(gbv[k]), lv, float(gbv[k + 1])))
			var bar_guide := _guide("Bar", TerrainGuide3D.MODE_BAR, gpts, 0.0, 0.0)
			bar_guide.bar_width = float(args.get("bar-width", "40"))
			bar_guide.apron_slope = float(args.get("bar-apron", "0"))
			gen.add_child(bar_guide)
		elif args.get("bar", "none") != "none":
			# A bar of resistant rock across the valley (x0,z0,x1,z1), held at --bar-level: a riegel the river and the
			# ice cross slowly, so the ground above it stays high (sill markers every 32 m).
			var bv: PackedStringArray = args["bar"].split(",")
			var bar_pts: Array[Vector3] = []
			for k in range(0, bv.size() - 1, 2):
				bar_pts.append(Vector3(float(bv[k]), 0, float(bv[k + 1])))
			# --bar-level=L, or one level per bar point (L0,L1,...) interpolated along it: a lip lower than the bar's ends.
			var bl: PackedStringArray = args.get("bar-level", "").split(",")
			var along: Array[Vector3] = []
			var along_level: Array[float] = []
			for k in bar_pts.size() - 1:
				var steps := maxi(1, int(bar_pts[k].distance_to(bar_pts[k + 1]) / 32.0))
				for i in steps:
					along.append(bar_pts[k].lerp(bar_pts[k + 1], float(i) / steps))
					if bl.size() == bar_pts.size():
						along_level.append(lerpf(float(bl[k]), float(bl[k + 1]), float(i) / steps))
			along.append(bar_pts[bar_pts.size() - 1])
			if bl.size() == bar_pts.size():
				along_level.append(float(bl[bl.size() - 1]))
			for i in along.size():
				var m := Node3D.new()
				m.name = "Bar%d" % i
				m.position = along[i]
				m.set_meta("sill", float(args.get("bar-width", "40")))
				if not along_level.is_empty():
					m.set_meta("level", along_level[i])
				elif args.has("bar-level"):
					m.set_meta("level", float(args["bar-level"]))
				add_child(m)
				markers.append(gen.get_path_to(m))
			if args.has("bar-apron") and args.get("bowl", "none") != "none" and not along_level.is_empty():
				# The bar's outer side as a rock slope (--bar-apron=rise/run) instead of a wall: hard rock stepping down
				# away from the bowl every 32 m (overlapping discs, so no ribs between them), except within 160 m of the lip
				# (the lowest point), where the river falls.
				var bc: PackedStringArray = args["bowl"].split(",")
				var centre := Vector3(float(bc[0]), 0, float(bc[1]))
				var lip := 0
				for i in along.size():
					if along_level[i] < along_level[lip]:
						lip = i
				var slope := float(args["bar-apron"])
				var na := 0
				for i in along.size():
					if along[i].distance_to(along[lip]) < 160.0:
						continue
					var out := (along[i] - centre).normalized()
					for k in range(1, 10):
						var lv := along_level[i] - slope * 32.0 * k
						if lv < 0.0:
							break
						var a := Node3D.new()
						a.name = "Apron%d" % na
						na += 1
						a.position = along[i] + out * 32.0 * k
						a.set_meta("sill", 30.0)
						a.set_meta("level", lv)
						add_child(a)
						markers.append(gen.get_path_to(a))
		gen.inflows = markers
	var t := Time.get_ticks_msec()
	# One climate for the rivers (rain and snowmelt by month), the data map's climate fields and the plants: a mild
	# coast with the wet wind from the sea (south, +Z).
	climate = ClimateRecipe.new()
	climate.temperature_january = 1.0 # the snowpack's winter too (SnowClimate.recipe)
	climate.temperature_july = 17.0
	# --climate-shift=C: the whole year C degrees warmer or colder (moves the treeline and the alpine belt).
	# --jan=C --jul=C --precip=MM: another climate (e.g. --jan=10 --jul=24 --precip=650 for Mediterranean scrub).
	climate.temperature_january = float(args.get("jan", str(climate.temperature_january)))
	climate.temperature_july = float(args.get("jul", str(climate.temperature_july)))
	climate.temperature_january += float(args.get("climate-shift", "0"))
	climate.temperature_july += float(args.get("climate-shift", "0"))
	climate.precipitation = float(args.get("precip", "1100"))
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
	if gen.outwash_age > 0.0:
		_log("braided outwash: %d reaches" % gen.get_last_braid_count())
		for bp in gen.get_braidplains():
			var ln: PackedVector2Array = bp["line"]
			_log("  braidplain %s to %s, %.0f m wide" % [str(ln[0]), str(ln[ln.size() - 1]), 2.0 * float(bp["half_width"])])
			if args.has("braid-probe"):
				# The ground across the plain at a third of the way down: its channels and bars.
				var k := ln.size() / 3
				var dv: Vector2 = (ln[k + 1] - ln[k - 1]).normalized()
				var nv := Vector2(-dv.y, dv.x)
				var row := ""
				var off := -float(bp["half_width"])
				while off <= float(bp["half_width"]):
					var q: Vector2 = ln[k] + nv * off
					row += " %.2f" % gen.get_height(Vector3(q.x, 0, q.y))
					off += 4.0
				_log("  across:" + row)
				var st: PackedFloat32Array = bp["stage"]
				var wet := 0
				var tot := 0
				for r2 in range(0, ln.size(), maxi(ln.size() / 12, 1)):
					var d3: Vector2 = (ln[mini(r2 + 1, ln.size() - 1)] - ln[maxi(r2 - 1, 0)]).normalized()
					var n3 := Vector2(-d3.y, d3.x)
					var o3 := -float(bp["half_width"])
					while o3 <= float(bp["half_width"]):
						var q3: Vector2 = ln[r2] + n3 * o3
						if gen.get_height(Vector3(q3.x, 0, q3.y)) < st[r2]:
							wet += 1
						tot += 1
						o3 += 6.0
				_log("  stage at a third %.2f, head %.2f, foot %.2f; %d%% of the plain under it; %.1f m3/s at the head" % [st[k], st[0], st[st.size() - 1], 100 * wet / maxi(tot, 1), float(bp["discharge"])])
	if args.has("touch"):
		_log("touch-ups: %d cells moved, %d new lakes, %.0f ms" % [gen.get_last_touch_up_cells(), gen.get_last_touch_up_lakes(), gen.get_last_touch_up_msec()])
		for l in gen.get_lake_results():
			_log("  lake level %.1f m, depth %.1f m, %.1f ha at %s" % [l["level"], l["depth"], l["area"] / 1e4, l["center"]])
		if args.has("touch-undo"):
			# Take the brushes away again: the ground and the water go back to the generated ones.
			var h0 := gen.get_height(Vector3(100, 0, 0))
			for br in gen.find_children("Touch*", "TerrainBrush3D", true, false):
				gen.remove_child(br)
				br.free()
			gen.apply_touch_ups()
			_log("touch-ups undone: %d cells moved, %d lakes, ground at (100, 0) %.1f m (was %.1f m)" % [gen.get_last_touch_up_cells(), gen.get_lake_results().size(), gen.get_height(Vector3(100, 0, 0)), h0])
	if gen.scree_age > 0.0:
		_log("scree: %.0f m3" % gen.get_last_scree_volume())
	if gen.dunes_enabled:
		_log("dunes: %.0f ms, %.0f m3 of sand, deepest %.1f m" % [gen.get_last_dune_msec(), gen.get_last_dune_volume(), gen.get_last_dune_max()])
	if args.has("sand-sea"):
		_log("sand sea: %.0f ms, deepest %.1f m" % [gen.get_last_sand_sea_msec(), gen.get_last_sand_sea_max()])
	if gen.glacial_enabled:
		_log("glaciers: %.0f ms, last glaciation %.2f km2 of ice, eroded up to %.0f m, ice imbalance %.2f m/yr (%d cycles)" % [gen.get_last_glacial_msec(), gen.get_last_glacial_ice_area() / 1e6, gen.get_last_glacial_eroded(), gen.get_last_glacial_residual(), gen.get_last_glacial_steps()])
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
		for b in bl:
			boulder_obstacles.push_back(Vector4(b.x, b.y, b.z, b.w * 1.1)) # the stand-in's footprint, slightly long
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
		if args.get("rocks", "on") != "off" and ClassDB.class_exists("RockField3D"):
			# RockGenerator's stones placed by process (modules/rocks), built round the camera before the picture.
			rocks = RockField3D.new()
			rocks.name = "Rocks"
			rocks.streaming = false
			add_child(rocks)
			rocks.generator = rocks.get_path_to(gen)
			if "close_up_enabled" in rocks:
				rocks.close_up_enabled = args.get("rock-closeup", "1") != "0" # real close-up detail near the camera
			var to := Time.get_ticks_msec()
			boulder_obstacles = rocks.get_obstacles(Rect2(-2048, -2048, 4096, 4096), float(args.get("rock-obstacle-min", "0.25")))
			_log("rocks: %d stones as plant obstacles in %d ms" % [boulder_obstacles.size(), Time.get_ticks_msec() - to])
			bl = PackedVector4Array() # no stand-ins
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



func _guide(n: String, mode: int, pts: Array, width: float, strength: float) -> TerrainGuide3D:
	var g := TerrainGuide3D.new()
	g.name = n
	g.mode = mode
	if width > 0.0:
		g.width = width
	g.strength = strength
	var curve := Curve3D.new()
	for p in pts:
		curve.add_point(p)
	g.curve = curve
	return g


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
# --dune-map=path.png[,x0,z0,size]: the dunes from above at their grid's spacing (or a square of the given size),
# hillshaded, bare sand tan and marram green, then quit.
func _save_dune_map(spec: String) -> void:
	var parts := spec.split(",")
	var r: Rect2 = gen.get_dune_rect()
	if r.size.x <= 0.0 and args.has("sand-sea"):
		r = gen.sand_sea_rect
	if parts.size() >= 4:
		r = Rect2(float(parts[1]), float(parts[2]), float(parts[3]), float(parts[3]))
	if r.size.x <= 0.0:
		_log("dune map: no dunes")
		return
	var step := float(gen.dune_cell) if not args.has("sand-sea") else maxf(r.size.x / 1024.0, 2.0)
	var sz := Vector2i(int(r.size.x / step), int(r.size.y / step))
	var hm: Image = gen.sample_heights(r.position, step, sz)
	var img := Image.create(sz.x, sz.y, false, Image.FORMAT_RGB8)
	var light := Vector3(-0.5, 0.75, -0.45).normalized()
	for z in range(sz.y):
		for x in range(sz.x):
			var hc := hm.get_pixel(x, z).r
			var hx := hm.get_pixel(mini(x + 1, sz.x - 1), z).r - hm.get_pixel(maxi(x - 1, 0), z).r
			var hz := hm.get_pixel(x, mini(z + 1, sz.y - 1)).r - hm.get_pixel(x, maxi(z - 1, 0)).r
			var nrm := Vector3(-hx, 4.0 * step, -hz).normalized()
			var sh := clampf(nrm.dot(light), 0.0, 1.0)
			var p := r.position + Vector2(x, z) * step
			var c := Color(0.2, 0.32, 0.55)
			if hc > float(gen.sea_level) + 0.05:
				var sand := clampf((gen.get_dune_height(Vector3(p.x, 0, p.y)) + gen.get_sand_sea_height(Vector3(p.x, 0, p.y))) / 0.3, 0.0, 1.0)
				var veg := gen.get_dune_stability(Vector3(p.x, 0, p.y))
				c = Color(0.42, 0.45, 0.32).lerp(Color(0.86, 0.78, 0.6), sand).lerp(Color(0.38, 0.52, 0.26), veg * sand)
			img.set_pixel(x, z, c * (0.3 + 0.7 * sh))
	img.save_png(spec.split(",")[0])
	# The heights too (32-bit floats, rows of sz.x), for laying out a camera on a crest.
	var hf := FileAccess.open(spec.split(",")[0] + ".f32", FileAccess.WRITE)
	if hf != null:
		for z in range(sz.y):
			for x in range(sz.x):
				hf.store_float(hm.get_pixel(x, z).r)
	# And the sand alone (its depth over the ground), to tell the dunes' shapes from the ground under them.
	var sf := FileAccess.open(spec.split(",")[0] + ".sand.f32", FileAccess.WRITE)
	if sf != null:
		for z in range(sz.y):
			for x in range(sz.x):
				sf.store_float(gen.get_sand_sea_height(Vector3(r.position.x + x * step, 0, r.position.y + z * step)))
	_log("dune map saved %s (%s, %.1f m a pixel, %d x %d)" % [spec.split(",")[0], r, step, sz.x, sz.y])


func _save_map(path: String) -> void:
	if args.has("profile"):
		# Heights along rows (--profile=z1,z2,...): every 100 m across the map.
		for zs in args["profile"].split(","):
			var row := PackedStringArray()
			for xi in range(-2000, 2001, 100):
				row.append("%d" % int(height_at(float(xi), float(zs))))
			print("[landscape] profile z=%s: %s" % [zs, " ".join(row)])
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
	for f in (gen_falls if not args.has("map-clean") else []):
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
			var reach := maxi(int(args["lip-profile"]), 44) # --lip-profile=N: metres out from the lip
			for k in range(-4, reach, 2 if reach <= 44 else 4):
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
		# normals, grass tint, wetness and snow; the CC0 ground scans from the art repository when fetched.
		var tm := TerrainMaterial3D.new()
		terrain_mat = tm
		tm.use_generator(gen)
		tm.detail_octaves = int(args.get("mdetail", "4"))
		tm.rock_detail_enabled = args.get("rock-detail", "1") != "0"
		tm.cliff_self_shadow = args.get("ledge-shadow", "1") != "0" # --ledge-shadow=0: ledges of --cliff-relief without their per-pixel shadows (raster)
		if args.get("far-shadows", "1") == "0":
			tm.far_shadow_start = 0.0 # --far-shadows=0: no far sun shadows from the ground's horizon (an A/B frame)
		if args.has("far-cover"):
			# Far ground blend against the plant cover map the world field builds (cover_map_enabled below).
			tm.far_blend_enabled = true
			# The cover field streams its plants out to 80 m: the cover map takes over before that, not at 300 m,
			# or the ground between shows the bare grass far colour (a bright band past the plants).
			if "far_start_from_vegetation" in tm and not args.has("far-start"):
				tm.far_start_from_vegetation = true # the blend ends where the Cover field's placed plants end
			else:
				tm.far_start = float(args.get("far-start", "50"))
				tm.far_fade = float(args.get("far-fade", "30"))
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
		if args.get("ground-scans", "1") != "0" and not args.has("test"):
			var ns := _apply_ground_scans(tm, ProjectSettings.globalize_path("res://art/terrain_layers"), args.get("ground-scans", "1"))
			if ns > 0:
				tm.update_shader()
				_log("ground scans: %d layers" % ns)
			else:
				_log("ground scans: art/terrain_layers missing (run fetch_art.sh), plain layers")
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
		landscape.cliff_relief = float(args.get("cliff-relief", "0")) # --cliff-relief=M: rock ledges built into steep faces
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
	if args.has("caustics"):
		# --caustics: the light-space caustics pass on the sea and the lakes (off by default; shallow beds only).
		for w in water_nodes:
			if "caustics_enabled" in w:
				w.caustics_enabled = true
				_log("caustics on %s" % w.name)
	for w in water_nodes:
		if String(w.name).begins_with("Sea"):
			ocean = w
			# An open coast: part of the sea is swell from distant storms, long near-parallel crests (Horvath 2015).
			ocean.swell = float(args.get("swell", "0.4"))
			if "terrain_source" in ocean:
				ocean.terrain_source = ocean.get_path_to(gen) # no sea through terrain holes (cave openings)
			if args.has("sea-prop"):
				# --sea-prop=name:value[;name:value]: sea surface properties (true/false or numbers), e.g. contact_foam:false.
				for kv in String(args["sea-prop"]).split(";"):
					var pr := kv.split(":")
					if pr.size() == 2 and pr[0] in ocean:
						ocean.set(pr[0], (pr[1] == "true") if pr[1] in ["true", "false"] else float(pr[1]))
						_log("sea: %s = %s" % [pr[0], str(ocean.get(pr[0]))])
		else:
			lakes.append(w)
	if lakes.size() > 0:
		lake = lakes[0]
		for l in lakes:
			if absf(l.global_position.y + l.water_level - lake_level) < absf(lake.global_position.y + lake.water_level - lake_level):
				lake = l
	rivers = gen.build_river_paths(self, float(args.get("min-area", "1500000")))
	if args.has("source-lakes"):
		for r in rivers:
			var p0: Vector3 = r.global_transform * r.curve.get_point_position(0)
			for l in lakes:
				var lv: float = l.global_position.y + l.water_level
				if absf(p0.y - lv) < 4.0 and Vector2(p0.x - l.global_position.x, p0.z - l.global_position.z).length() < 600.0:
					r.source_lake = r.get_path_to(l)
					_log("source lake: %s from %s at %s" % [r.name, l.name, p0])
					break
	_log("water + %d rivers built in %d ms (%d lakes)" % [rivers.size(), Time.get_ticks_msec() - t, lakes.size()])
	# Contact falls (cascades and horsetails down steep rock) from the generator's fall records.
	if ClassDB.class_exists("PhysXWaterfalls3D"):
		var wf = ClassDB.instantiate("PhysXWaterfalls3D")
		wf.name = "Waterfalls"
		add_child(wf)
		wf.source = wf.get_path_to(gen)
		# The drawn ground: sheets lie over the ledges --cliff-relief builds into steep faces.
		if landscape != null and "surface" in wf:
			wf.surface = wf.get_path_to(landscape)
		# --rock-step=M: step height of the rock thin contact falls run over (0.01: the flow always covers the face).
		if args.has("rock-step"):
			wf.rock_step = float(args["rock-step"])
		# --plunge-falls=0: tall falls drawn as before the broken column (the river's own ribbon fall), for A/B frames.
		if args.has("plunge-falls"):
			wf.plunge_falls = args["plunge-falls"] != "0"
		if args.has("column-density"):
			wf.column_density = float(args["column-density"])
		wf.rebuild()
		# --contact-shader=path: draw the contact sheets with this shader code instead (look tests without a rebuild).
		if args.has("contact-shader"):
			for ch in wf.get_children(true):
				if ch is MeshInstance3D and ch.material_override is ShaderMaterial and ch.material_override.shader.code.contains("wet_margin"):
					var sh := Shader.new()
					sh.code = FileAccess.get_file_as_string(args["contact-shader"])
					ch.material_override.shader = sh
		for f in wf.get_contact_falls():
			_log("contact fall %s" % [f])
		contact_falls = wf.get_contact_falls()
		if wf.has_method("get_plunge_falls"):
			for f in wf.get_plunge_falls():
				_log("plunge fall %s" % [f])
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
	# --shadow-splits=a,b,c: the cascades' ends as shares of shadow-max (Godot's defaults 0.1,0.2,0.5); a long low-sun
	# view puts more of them into the middle distance. --shadow-atlas=N: the directional shadow atlas side (4096).
	if args.has("shadow-splits"):
		var sp: PackedStringArray = args["shadow-splits"].split(",")
		sun.directional_shadow_split_1 = float(sp[0])
		sun.directional_shadow_split_2 = float(sp[1])
		sun.directional_shadow_split_3 = float(sp[2])
	if args.has("shadow-atlas"):
		RenderingServer.directional_shadow_atlas_set_size(int(args["shadow-atlas"]), true)
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
	if args.has("cloud-still"):
		# --cloud-still[=steps]: stills settings for the clouds (no temporal reprojection, --cloud-still (256) march
		# steps), so a single frame has no stipple that TAA would hide in play.
		sky_mat.set("cloud_reprojection", false)
		sky_mat.set("cloud_steps", int(args["cloud-still"]) if String(args["cloud-still"]).is_valid_int() else 256)
	if args.has("sky-params"):
		# --sky-params=name:value,...: the sky's shader parameters, for A/B docs frames (e.g. cloud_light_grid_enabled:0).
		for pair in String(args["sky-params"]).split(","):
			var nv := pair.split(":")
			sky_mat.set_shader_parameter(nv[0], float(nv[1]))
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
	if args.has("raster") and args.get("aa", "1") != "0":
		# Raster stills anti-aliased as a game would draw them: TAA over the frames before the shot (the camera
		# holds still), plus SMAA. Without them, edges the shaders draw (parallax stones, cracks) stair-step into
		# saw teeth. --aa=0 for the raw image.
		get_viewport().use_taa = true
		get_viewport().screen_space_aa = Viewport.SCREEN_SPACE_AA_SMAA
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	if args.has("ssao"):
		env.ssao_enabled = true # --ssao: screen-space contact darkening in raster tests
	if args.has("taa"):
		get_viewport().use_taa = true # --taa: temporal AA as the game runs it (sea glitter, foam rims)
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
	# Falling rain round the camera (streaks, splashes, drips, haze) and the runoff it feeds, from the same weather.
	rainfall = RainFall3D.new()
	rainfall.name = "Rainfall"
	add_child(rainfall)
	rainfall.snow_cover = rainfall.get_path_to(snow)
	runoff = WeatherRunoff3D.new()
	runoff.name = "Runoff"
	runoff.extent = 4096.0
	runoff.resolution = int(args.get("runoff-res", "256"))
	runoff.bake_on_ready = false
	add_child(runoff)
	runoff.height_source = runoff.get_path_to(get_node("GroundHeight"))
	runoff.bake()
	# Falling and blown leaves (LeafFall3D): shed from the trees' crowns near the camera as their season drops
	# them, carried by the same wind, and lifted off the ground by gusts. --leaves=0 for none.
	if args.get("leaves", "1") != "0" and ClassDB.class_exists("LeafFall3D"):
		var leaf_fall = ClassDB.instantiate("LeafFall3D")
		leaf_fall.name = "LeafFall"
		add_child(leaf_fall)
		leaf_fall.weather_map = leaf_fall.get_path_to(weather)
		leaf_fall.litter_density = float(args.get("leaf-litter", "0.5"))
	# Plumed seeds (SeedDown3D, vegetation 19a84b7e): dandelion, thistle and willowherb heads in seed near the camera
	# loose them in a dry wind, the shared wind carries them into the grass. --seeds=0 for none.
	if args.get("seeds", "1") != "0" and ClassDB.class_exists("SeedDown3D"):
		var sd = ClassDB.instantiate("SeedDown3D")
		sd.name = "SeedDown"
		sd.range = float(args.get("seed-range", "40"))
		add_child(sd)
	# Sod lips breaking off (SodLipFall3D, vegetation 8faf7573): bank overhangs within range of the camera fail
	# under a step (grass_trample), an undercutting river or a soaking, and fall as blocks of sod. --sod-fall=0 for none.
	if args.get("sod-fall", "1") != "0" and ClassDB.class_exists("SodLipFall3D"):
		var sl = ClassDB.instantiate("SodLipFall3D")
		sl.name = "SodLipFall"
		sl.range = float(args.get("sod-range", "40"))
		add_child(sl)
	snow.global_position = Vector3(0, 0, 0)


# World-links docs shots (docs/ideas/world-links.md), on the scene's own systems:
#   --drop-leaves=N[,r[,d]]   N of the vegetation's own leaves dropped from 2-6 m over a disc of radius r (default
#                         15 m) round --look, else d m (25) in front of the camera; they land on what is there (ground, snow, water) and float on water
#   --burn=x,z,r,years    a WorldBurnScar3D (burnt forest years ago) of radius r; with --snow-days=N the snow under it
#                         gathers the burnt trees' char over N days of weather (dirty snow)
func _docs_links_setup() -> void:
	if args.has("link"):
		await _link_shot(String(args["link"]), args.has("link-off"))
	if args.has("burn"):
		var b := String(args["burn"]).split(",")
		var scar = ClassDB.instantiate("WorldBurnScar3D")
		scar.name = "DocsBurn"
		add_child(scar)
		scar.global_position = Vector3(float(b[0]), height_at(float(b[0]), float(b[1])), float(b[1]))
		scar.size = Vector2(float(b[2]), float(b[2])) * 2.0
		scar.age_years = float(b[3]) if b.size() > 3 else 1.0
		await get_tree().process_frame
		_log("burn scar at %s, %s m, %.1f years" % [str(scar.global_position), str(scar.size), scar.age_years])
	if args.has("snow-days") and snow != null:
		var days := float(args["snow-days"])
		var t := Time.get_ticks_msec()
		snow.advance(days * 86400.0)
		var waited := 0
		while snow.is_updating() and waited < 600:
			await get_tree().process_frame
			waited += 1
		if args.has("look"):
			var lp := _vec3_arg(args["look"])
			_log("snow after %.0f days: %.2f m deep, %.1f mg/m2 dirt, albedo %.2f at the look point (%d ms)" % [days, snow.get_depth_at(lp), snow.get_impurity_at(lp), snow.get_albedo_at(lp), Time.get_ticks_msec() - t])
	if args.has("drop-leaves"):
		var leaf_fall := get_node_or_null("LeafFall")
		if leaf_fall == null:
			return
		var d := String(args["drop-leaves"]).split(",")
		var n := int(d[0])
		var r := float(d[1]) if d.size() > 1 else 15.0
		# Round --look, else 25 m (third arg) in front of the camera.
		var cam6: Camera3D = get_viewport().get_camera_3d()
		var ahead := float(d[2]) if d.size() > 2 else 25.0
		var lp := _vec3_arg(args["look"]) if args.has("look") else cam6.global_position - cam6.global_basis.z * ahead
		if not args.has("look"):
			lp.y = maxf(height_at(lp.x, lp.z), cam6.global_position.y - 3.0)
		leaf_fall.lie_time = 100000.0
		leaf_fall.refresh_crowns_near(lp)
		var kinds: int = leaf_fall.get_kind_count()
		if kinds == 0:
			_log("drop-leaves: no leaf kinds from the vegetation near %s" % str(lp))
			return
		var rng := RandomNumberGenerator.new()
		rng.seed = 7
		for i in n:
			var a := rng.randf() * TAU
			var rr := r * sqrt(rng.randf())
			var p := Vector3(lp.x + rr * cos(a), 0.0, lp.z + rr * sin(a))
			p.y = maxf(height_at(p.x, p.z), lp.y) + rng.randf_range(2.0, 6.0)
			leaf_fall.drop_leaf(p, rng.randi() % kinds)
		var waited := 0
		while waited < 900:
			await get_tree().physics_frame
			waited += 1
			if waited % 60 == 0 and leaf_fall.get_floating_count() + leaf_fall.get_buried_count() >= 0:
				var aloft := 0
				for st in leaf_fall.get_leaf_states():
					if st.w < 0.5:
						aloft += 1
				if aloft < n / 20:
					break
		_log("drop-leaves: %d dropped (%d kinds), %d floating, %d buried after %d physics frames" % [n, kinds, leaf_fall.get_floating_count(), leaf_fall.get_buried_count(), waited])



# --link=fog|spray|tracks [--link-off] [--link-hours=H]: world-links docs frames (docs/ideas/world-links.md, round 6),
# the link on or, with --link-off, the same scene without it.
#   fog     the cloud base dropped 40 m under the camera's ground (in the cloud), H hours (18) of fog: the canopy
#           combs it out and drips (fog_drip)
#   spray   H hours (6) of the onshore wind off a surf of at least 1.5 m: spray wets the shore, salt burns the grass
#           (sea_spray); run with --weather=clear --wind=15
#   tracks  --track=x0,z0,x1,z1 a walker wading out of the water along the line; --tyre=x0,z0,x1,z1 a pair of tyres
#           driving out of it (wet_tracks); the bodies are invisible, only their prints show
func _link_shot(kind: String, off: bool) -> void:
	var t := Time.get_ticks_msec()
	var cam7: Camera3D = get_viewport().get_camera_3d()
	match kind:
		"fog":
			weather.fog_drip = not off
			var g := height_at(cam7.global_position.x, cam7.global_position.z)
			weather.cloud_base_height = g - 40.0
			weather.cloud_cover = 1.0
			weather.cloud_variation = 0.0
			weather.rain_rate = 0.0
			weather.humidity = 0.97
			weather.wind_speed = float(args.get("wind", "5"))
			weather.snap_to_settings()
			weather.advance(float(args.get("link-hours", "18")) * 3600.0)
			var at := cam7.global_position
			_log("fog: %.2f g/m3, drip %.3f mm/h, wetness %.2f at the camera" % [weather.get_fog_water_at(at), weather.get_fog_drip_at(at), weather.get_wetness_at(at)])
		"spray":
			weather.sea_spray = not off
			weather.rain_rate = 0.0
			weather.snap_to_settings()
			if ocean != null:
				ocean.shore_wave_height = maxf(ocean.shore_wave_height, 1.5)
			weather.advance(float(args.get("link-hours", "6")) * 3600.0)
			var at2 := cam7.global_position
			_log("spray: %.3f mm/h, salt %.2f g/m2, wetness %.2f at the camera; square %s" % [weather.get_sea_spray_at(at2), weather.get_salt_at(at2), weather.get_wetness_at(at2), str(weather.get_sea_spray_rect())])
		"tracks":
			weather.wet_tracks = not off
			var movers := []
			if not args.has("track") and not args.has("tyre"):
				# Framed by itself: from the camera straight toward the sea to where the water is 0.4 m deep, a walker
				# from 10 m out to 16 m up the beach, tyres beside it from 12 m out, the camera turned onto the prints.
				var c0 := cam7.global_position
				var fwd := Vector2(-cam7.global_basis.z.x, -cam7.global_basis.z.z).normalized()
				var wet_at := Vector2.INF
				for k in 400:
					var q := Vector2(c0.x, c0.z) + fwd * (k * 0.5)
					var gq := height_at(q.x, q.y)
					if WorldContracts.get_water().depth_at(Vector3(q.x, gq, q.y)) > 0.4:
						wet_at = q
						break
				if wet_at == Vector2.INF:
					_log("tracks: no water within 200 m ahead of the camera")
				else:
					var side := Vector2(-fwd.y, fwd.x)
					args["track"] = "%f,%f,%f,%f" % [wet_at.x, wet_at.y, wet_at.x - fwd.x * 26.0, wet_at.y - fwd.y * 26.0]
					var t0 := wet_at + side * 2.5 + fwd * 2.0
					args["tyre"] = "%f,%f,%f,%f" % [t0.x, t0.y, t0.x - fwd.x * 30.0, t0.y - fwd.y * 30.0]
					var end := wet_at - fwd * 16.0
					var cp := end - fwd * 9.0 - side * 5.0
					cam7.global_position = Vector3(cp.x, height_at(cp.x, cp.y) + 1.7, cp.y)
					var lp := wet_at - fwd * 9.0 + side * 1.2
					cam7.look_at(Vector3(lp.x, height_at(lp.x, lp.y), lp.y))
					_log("tracks: water at %s; walker %s; tyres %s; cam %s" % [str(wet_at), args["track"], args["tyre"], str(cam7.global_position)])
			if args.has("track"):
				var tr := String(args["track"]).split_floats(",")
				var walker := Node3D.new()
				walker.name = "LinkWalker"
				walker.set_meta("trample_radius", 0.3)
				walker.add_to_group("world_contact")
				add_child(walker)
				movers.append([[walker], Vector2(tr[0], tr[1]), Vector2(tr[2], tr[3]), 1.4, 0.3])
			if args.has("tyre"):
				var ty := String(args["tyre"]).split_floats(",")
				var pair := []
				for k in 2:
					var w := Node3D.new()
					w.name = "LinkTyre%d" % k
					w.set_meta("deform_wheel_radius", 0.4)
					w.set_meta("deform_wheel_width", 0.28)
					w.add_to_group("world_contact")
					add_child(w)
					pair.append(w)
				movers.append([pair, Vector2(ty[0], ty[1]), Vector2(ty[2], ty[3]), 3.0, 0.4])
				if prints != null:
					# The ground's deform window too, so the stamped prints land inside it.
					prints.follow_target = prints.get_path_to(cam7)
					for _f in 10:
						await get_tree().physics_frame
					_log("tracks: ground deform rect %s" % str(prints.get_deform_rect()))
			get_viewport().disable_3d = true
			for mv in movers:
				var a: Vector2 = mv[1]
				var b: Vector2 = mv[2]
				var dir := (b - a).normalized()
				var side := Vector2(-dir.y, dir.x)
				var steps := int(a.distance_to(b) / (float(mv[3]) / 60.0))
				for i in steps + 90:
					var p := a.lerp(b, minf(float(i) / steps, 1.0))
					var bodies: Array = mv[0]
					var yaw := atan2(dir.x, dir.y)
					for k in bodies.size():
						var q := p + side * (0.0 if bodies.size() == 1 else (k - 0.5) * 1.6)
						bodies[k].global_position = Vector3(q.x, height_at(q.x, q.y) + float(mv[4]), q.y)
						# The walk is stepped directly (no ground frames run), so the ground's prints are stamped here
						# too: soles every 0.7 m, left and right, and the tyres' tread each step.
						if prints == null or i > steps:
							continue
						var hq := height_at(q.x, q.y)
						if bodies.size() == 1:
							var stride := int(0.7 / (float(mv[3]) / 60.0))
							if stride > 0 and i % stride == 0:
								var fq := q + side * (0.12 if (i / stride) % 2 == 0 else -0.12)
								var hf := height_at(fq.x, fq.y)
								prints.press_box(Vector3(fq.x, hf, fq.y), Vector2(0.11, 0.29), yaw, hf - 0.02, 0.0, true)
						else:
							prints.press_wheel(Vector3(q.x, hq, q.y), Vector2(0.8, 0.28), yaw, 0.04, dir * float(mv[3]))
					if args.has("link-verbose") and i % 60 == 0:
						var b0: Node3D = bodies[0]
						var gp := b0.global_position
						_log("tracks: %s step %d at %s: bench ground %.3f, contract ground %.3f, water depth %.3f, wetness %.2f, body %s" % [b0.name, i, str(gp), height_at(gp.x, gp.z), WorldContracts.get_ground().height_at(gp + Vector3(0, 1, 0)), WorldContracts.get_water().depth_at(Vector3(gp.x, height_at(gp.x, gp.z) + 0.05, gp.z)), weather.get_contact_wetness_at(gp), str(weather.get_body_water(b0))] + " square %s" % str(weather.get_contact_wetness_rect()))
					# Stepped directly: at a renderer frame per step the walk would take hours on a software GPU.
					if weather.has_method("step_contact"):
						weather.step_contact(1.0 / 60.0)
					else:
						await get_tree().physics_frame
				if prints != null:
					var mq := a.lerp(b, 0.5)
					_log("tracks: dent %.3f m mid-walk at %s" % [float(prints.get_dent_at(Vector3(mq.x, height_at(mq.x, mq.y), mq.y))), str(mq)])
				# The wet prints as the map holds them: the most wetness within 0.5 m of five points along the way, and the
				# square they must lie in to be drawn (weather_contact_rect).
				var wl := []
				for k in 5:
					var wq := a.lerp(b, (k + 0.5) / 5.0)
					var best := 0.0
					for oz in range(-4, 5):
						for ox in range(-4, 5):
							best = maxf(best, weather.get_contact_wetness_at(Vector3(wq.x + ox * 0.125, 0, wq.y + oz * 0.125)))
					wl.append("%.2f at %s" % [best, str(wq.round())])
				_log("tracks: contact wetness along the way %s; square %s, camera %s" % [", ".join(wl), str(weather.get_contact_wetness_rect()), str(get_viewport().get_camera_3d().global_position.round())])
				for bd in mv[0]:
					_log("tracks: %s water %s at the end" % [bd.name, str(weather.get_body_water(bd))])
					bd.queue_free()
			get_viewport().disable_3d = false
	_log("link %s %s set up in %d ms" % [kind, "off" if off else "on", Time.get_ticks_msec() - t])

func _bake_snow_winter() -> void:
	# The snowpack's winter from the same ClimateRecipe as the rivers, the data map and the plants: its
	# temperatures, wet-day wind and precipitation drive the winter generator, and the data map's orographic
	# precipitation field says where more of it falls.
	if args.get("snow", "climate") != "climate":
		return
	var winter := SnowClimate.new()
	winter.recipe = climate
	if args.has("winter-days"):
		winter.days = int(args["winter-days"]) # --winter-days=N: the bake's length (120: ends late January, lower snow line)
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
	# The storm brings its own cold air mass: the air follows the temperature set here, not the date's climate
	# (air_follows_climate, on by default, would put mid-May air under the storm).
	weather.air_follows_climate = not storm
	weather.humidity = float(args.get("humidity", "0.95" if storm else "0.6"))
	# --haze=1 [--haze-visibility=KM] [--cloud-cover=C]: background aerosol haze, lit through the cloud shadow (godrays).
	weather.aerosol_haze = args.get("haze", "1") != "0" # on by default (all features on); --haze=0 off
	weather.haze_visibility = float(args.get("haze-visibility", "40"))
	if args.has("dew-frost"):
		weather.set("dew_and_frost", args["dew-frost"] != "0") # --dew-frost=0: no dew or hoarfrost (a docs pair)
	if args.has("cloud-cover"):
		weather.cloud_cover = float(args["cloud-cover"])
	# --high-cover=C, --mid-cover=C: the cirrus and the mid (altocumulus) layers' cover.
	if args.has("high-cover") and "high_cloud_cover" in weather:
		weather.high_cloud_cover = float(args["high-cover"])
	if args.has("mid-cover") and "mid_cloud_cover" in weather:
		weather.mid_cloud_cover = float(args["mid-cover"])
		if weather.cloud_cover <= 0.0:
			weather.cloud_variation = 0.0 # a clear sky: the regional variation would still raise clouds out of 0 cover
	if args.has("ice-storm") and "ice_storm_glaze" in weather:
		# --ice-storm=MM: freezing rain's glaze (WeatherMap3D.ice_storm_glaze, mm floor; trees 441bba54 grow ice shells
		# and icicles on near trees by its band), the air below freezing (--air-temp, else -3 C).
		weather.ice_storm_glaze = float(args["ice-storm"]) if String(args["ice-storm"]) != "" else 15.0
		if not args.has("air-temp"):
			weather.air_follows_climate = false
			weather.temperature = -3.0
	if args.has("air-temp"):
		# --air-temp=C: the air mass set here, not the date's climate (lake-effect and wave cloud frames).
		weather.air_follows_climate = false
		weather.temperature = float(args["air-temp"])
	if args.has("water-temp") and "water_temperature" in weather:
		weather.water_temperature = float(args["water-temp"]) # --water-temp=C: open water's surface (weather af97350f)
	if args.has("cloud-type"):
		weather.cloud_type = float(args["cloud-type"]) # --cloud-type=T: 0 stratus to 1 towering cumulus
		# A set cloud type stays put unless --cloud-regime=1 (clouds 2dd4c468: else the type follows time of day).
		if "cloud_regime_from_weather" in weather:
			weather.cloud_regime_from_weather = args.get("cloud-regime", "0") != "0"
	if args.has("cloud-variation"):
		weather.cloud_variation = float(args["cloud-variation"]) # --cloud-variation=V: regional cover variation
	if args.get("weather", "storm") == "afterrain":
		# --weather=afterrain: steady rain through the weather run (_run_weather), then it clears for the shot.
		weather.rain_rate = 6.0
		weather.cloud_cover = 0.8
		weather.cloud_type = 0.6
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
	if args.has("smoke-haze"):
		weather.set("smoke_haze", args["smoke-haze"] != "0") # --smoke-haze=0: fires' smoke does not haze the air
	if args.has("smoke-kg") and weather.has_method("add_smoke"):
		# --smoke-kg=KG: a wildfire's smoke (KG of particulate a source, per hour) fed along a 3 km line --smoke-upwind
		# (1500) m upwind of the camera, carried over by the wind through the hours of weather.
		var c3: Camera3D = get_viewport().get_camera_3d()
		var wd := weather.wind_direction if "wind_direction" in weather else Vector2(1, 0)
		var up := -Vector3(wd.x, 0.0, wd.y).normalized() if wd.length() > 0.01 else Vector3(-1, 0, 0)
		var sideways := up.cross(Vector3.UP)
		var cpos := c3.global_position if c3 != null else Vector3.ZERO
		var src := cpos + up * float(args.get("smoke-upwind", "1500"))
		var kg := float(args["smoke-kg"])
		var span := float(args.get("smoke-span", "3000")) # the burning front's width and depth, upwind and round the camera
		for k in int(hours):
			weather.advance(3600.0)
			snow.advance(3600.0)
			runoff.advance(3600.0)
		# The wind carries the layer off the 4 km map within minutes, so the fire feeds it in 5-minute steps at the end
		# (--smoke-steps, 8): the plume over the camera is the last 40 minutes' smoke.
		for k in int(args.get("smoke-steps", "8")):
			for j in 7:
				for d in 4:
					weather.call("add_smoke", src + sideways * (j - 3) * span / 6.0 - up * d * span / 4.0, kg)
			weather.advance(300.0)
		for probe in [Vector3(500, 0, 0), Vector3(-500, 0, 0), Vector3(0, 0, 500), Vector3(0, 0, -500)]:
			_log("smoke: optical depth %.2f at camera %s" % [float(weather.call("get_smoke_optical_depth_at", cpos + probe)), str(probe)])
		_log("smoke: optical depth %.2f over the camera, %.3f g/m2, haze %s" % [float(weather.call("get_smoke_optical_depth_at", cpos)), float(weather.call("get_smoke_at", cpos)), str(weather.get("smoke_haze"))])
		_log("smoke: aerosol haze %s /m at the camera after the smoke" % str(weather.get_precipitation_extinction_at(cpos)))
		hours = 0.0
	for k in int(hours):
		weather.advance(3600.0)
		snow.advance(3600.0)
		runoff.advance(3600.0)
	if args.get("weather", "storm") == "afterrain":
		# The rain stops and the sky breaks up; an hour of sun and wind dries the tops, puddles and wet banks stay.
		weather.rain_rate = 0.0
		weather.cloud_cover = float(args.get("cloud-cover", "0.4"))
		weather.cloud_type = 0.45
		weather.snap_to_settings()
		weather.advance(float(args.get("dry-hours", "1")) * 3600.0)
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
	data_map.baked.connect(func(): _log("data map baked (frame %d)" % Engine.get_process_frames()))
	data_map.climate_baked.connect(func(): _log("data map climate baked (frame %d)" % Engine.get_process_frames()))
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
	var generated_trees: bool = args.get("trees", "cones") == "generated"
	if generated_trees:
		biome = _generated_forest(args.get("layout", "range") == "valley" and not args.has("montado"))
	veg = VegetationField3D.new()
	veg.name = "Forest"
	veg.biome = biome
	veg.tile_size = 128.0
	veg.populate_on_ready = false
	add_child(veg)
	if "obstacles" in veg and not boulder_obstacles.is_empty():
		veg.obstacles = boulder_obstacles # no trees through the boulders
	if args.get("trees", "cones") == "none":
		veg.visible = false # --trees=none: placed (the other systems still read it) but not drawn, for views with no stand-ins

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
	var fr := float(args.get("forest-radius", "150" if generated_trees else "0"))
	if fr > 0.0:
		veg.streaming = true
		veg.stream_radius = fr
		_log("forest streams round the camera, %.0f m" % fr)
	else:
		veg.populate()
		_log("forest %d plants in %d ms" % [veg.get_plant_count(), Time.get_ticks_msec() - t])
	if generated_trees:
		_tree_wind()
	# Grass near the camera, on the same maps, moved by the weather's wind.
	var meadow := PlantSpecies.new()
	meadow.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 28))
	meadow.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(3, 1000000))
	meadow.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(3, 700))
	# No sward on scree, outwash gravel or bare rock (talus -1..0 is moraine till, which grasses over).
	meadow.set_rule_range(PlantSpecies.RULE_TALUS, Vector2(-1.0, 0.3))
	meadow.set_rule_fade(PlantSpecies.RULE_TALUS, 0.15)
	meadow.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.4))
	meadow.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
	# A closed green sward needs a humid climate (aridity index, precipitation over PET, above about 0.65): in a
	# Mediterranean one (--precip=650) it gives way to the garrigue's bare thin soil.
	meadow.set_rule_range(PlantSpecies.RULE_ARIDITY, Vector2(0.8, 1000.0))
	meadow.set_rule_fade(PlantSpecies.RULE_ARIDITY, 0.1)
	grass = GrassField3D.new()
	grass.name = "Grass"
	grass.rules = meadow
	grass.radius = float(args.get("grass-radius", "40"))
	if "still_shot" in grass:
		grass.set("still_shot", args.get("still-shot", "1") != "0") # settle once, then stop streaming (cloud memory); --still-shot=0 for clips
	if args.has("montado"):
		# The montado's summer sward (as the vegetation bench's _floor_view): sparse tall dry blades in clumps,
		# straw tips over a brown base, thinning under the crowns through canopy_field (the forest field).
		grass.density = float(args.get("sward-density", "90"))
		grass.blade_height = Vector2(0.2, 0.45)
		grass.clump_size = 0.6
		grass.dryness = 0.85
		grass.base_color = Color(0.32, 0.27, 0.13)
		grass.tip_color = Color(0.78, 0.66, 0.38)
		grass.ground_tint = Color(0.2, 0.16, 0.08, 1.0) # canopy_field: set with the rest of the sward below
	if args.get("summer-dry", "0") != "0" and "season_source" in grass and weather != null:
		grass.set("season_source", grass.get_path_to(weather)) # --summer-dry: the meadow browns with the season (a dry summer)
	if args.has("far-rings"):
		grass.set("far_rings", args["far-rings"] != "0") # --far-rings=0: no far blade rings (memory checks)
	if args.has("fibre"):
		for g in [grass]:
			g.set("fibre_shading", float(args["fibre"])) # --fibre=0: no fibre shading on the blades (A/B; default 0.8)
	if args.has("far-shell"):
		grass.set("far_shell", args["far-shell"] != "0") # --far-shell=0: no far voxel shell
	grass.ground_tint_enabled = args.get("ground-tint", "1") != "0" # --ground-tint=0: no sward colour on the ground (isolates the terrain material)
	var desert: bool = args.get("layout", "range") == "desert"
	if desert:
		# No sward in the desert: the meadow rules know soil, rock and scree, not the erg's loose sand.
		grass.ground_tint_enabled = false
		grass.radius = 0.0
	if args.has("hay") and "hay_meadows" in grass:
		grass.hay_meadows = _hay_list()
	add_child(grass)
	grass.data_map = grass.get_path_to(data_map)
	if args.get("plants", "1") != "0" and not desert:
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
		if args.has("hay") and "hay_meadows" in under:
			under.hay_meadows = _hay_list()
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
		if args.has("hay") and "hay_meadows" in turf:
			turf.hay_meadows = _hay_list()
		add_child(turf)
		turf.data_map = turf.get_path_to(data_map)
		sward = [grass, under, turf]
		if args.has("montado"):
			for g in [under, turf]: # the summer-dry montado: the short layers straw too
				g.dryness = 0.85
				g.base_color = Color(0.3, 0.25, 0.12)
				g.tip_color = Color(0.72, 0.6, 0.34)
		# The sward thins out under the forest's crowns.
		for g in sward:
			var r: PlantSpecies = g.rules
			r.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.3))
			r.set_rule_fade(PlantSpecies.RULE_SHADE, 0.4)
			g.canopy_field = g.get_path_to(veg)
			if "season_source" in g:
				g.season_source = g.get_path_to(weather) # the sward's colour follows the season


# Footprints in the ground around the camera: sand on the beach, mud on wet banks, leaf litter under the forest, none
# on bare rock or under snow. The patch is drawn with the terrain's own material.
func _build_prints() -> void:
	prints_probe = Node3D.new()
	prints_probe.name = "PrintsProbe"
	add_child(prints_probe)
	prints = GroundSurface3D.new()
	prints.name = "Prints"
	prints.set("keep_ruts", false) # repeatable shots: no prints carried over from the last run
	if "light_budget" in prints and args.get("ground-light", "1") != "0" and (args.has("buggy") or args.has("prints-light")):
		prints.set("light_budget", true) # a drive or winch sim on the still budget (mud 0ed4ecf1); --ground-light=0 for full
	if args.has("near-field") and "near_field" in prints:
		prints.set("near_field", args["near-field"] != "0") # --near-field=0: wide shots (the drive in about 30 s)
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
			# And the desert's sand sea (its ripples and prints), where some 20 cm of sand lies.
			if gen.sand_sea_rect.has_area():
				sand = maxf(sand, clampf(gen.get_sand_sea_height(Vector3(x, 0, z)) / 0.2, 0.0, 1.0))
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


# --trees=generated: TreeGenerator's European woodland (its stands sorted onto the climate belts, with
# saplings, snags and logs), rigged, and one TreeWind3D posing it from the weather's wind, with trunk and
# limb colliders near the camera. Needs the trees branch until it is on main.
func _generated_forest(clearing: bool) -> Biome:
	var tg := TreeGenerator.new()
	_tree_gen = tg # the species build their near leaves from it on demand: kept alive
	tg.rigged = args.get("rigged", "1") != "0" # --rigged=0: static trees (memory checks)
	if args.get("tree-still", "1") != "0":
		# A still (trees.md 8e): static, far meshes light, near meshes built one at a time with leaves to 25 m and
		# needles to 30 m; --tree-still=0 for the full rig. Fits a stand in about 6 GB instead of dying at first draw.
		tg.rigged = args.get("rigged", "0") != "0"
		if "light_far_meshes" in tg:
			tg.light_far_meshes = args.get("light-far", "1") != "0" # --light-far=0 on PT olive shots (greys their crowns)
		if tg.has_method("set_near_build_limit"):
			tg.set_near_build_limit(1)
		if not args.has("near-leaf-range"):
			args["near-leaf-range"] = "25"
		if not args.has("near-needle-range"):
			args["near-needle-range"] = "30" # the normal range: far needles alone read as wires near the camera
	if "near_leaf_range" in tg:
		tg.near_leaf_range = float(args.get("near-leaf-range", "60"))
	if "near_needle_range" in tg:
		tg.near_needle_range = float(args.get("near-needle-range", "30")) # --near-needle-range=0: far needles only (memory)
	if "fire_variants" in tg:
		# --fire-variants (1): each species also carries its burnt-stand variants (one a fire band, vegetation picks by
		# WorldFire burn age). On by default: they are deferred (trees.md 5bc), grown only where a burn lies over the
		# field, so an unburnt stand costs nothing. --fire-variants=0 skips them.
		tg.fire_variants = args.get("fire-variants", "1") != "0"
	if "lightning_variants" in tg:
		# --strike-variants (1): tall species also carry their two struck variants (vegetation picks them under
		# WorldStand strikes, trees.md 5ax). On by default, deferred like the fire ones; --strike-variants=0 skips them.
		tg.lightning_variants = args.get("strike-variants", "1") != "0"
	if "browse_variants" in tg:
		# --browse-variants (1): open trees, saplings and understory also carry a browsed variant (vegetation picks it
		# under WorldStand browse discs, trees.md 5az). On by default, deferred; --browse-variants=0 skips them.
		tg.browse_variants = args.get("browse-variants", "1") != "0"
	if "flood_height" in tg:
		# --flood-height=M (--flood-years=Y): flood marks on the trees, a silt line and caught debris at M m (trees
		# 591d3247); the floodplain willow stand takes 1.2 m by default.
		var fh := float(args.get("flood-height", "1.2" if args.get("tree-stand", "") == "floodplain_willow" else "0"))
		if fh > 0.0:
			tg.flood_height = fh
			tg.flood_years = float(args.get("flood-years", "1"))
	if args.has("fungi-cover") and "fungi_cover" in tg:
		tg.fungi_cover = float(args["fungi-cover"]) # a fungi off/on pair (0 none, 1 default)
	if clearing:
		tg.mask_channel = 0 # the valley's clearing mask (red), as the cones use it
	# One variant by default: each variant adds about the woodland's memory again (88M vertices at one).
	var variants := int(args.get("tree-variants", "1"))
	if args.has("ice-storm-years") and "ice_storm_years" in tg:
		tg.set("ice_storm_years", float(args["ice-storm-years"])) # --ice-storm-years=Y: STAND_ICE_STORM's age (trees 772002579b)
	var stands := {"beech": TreeGenerator.STAND_BEECH, "oak": TreeGenerator.STAND_OAK_MIXED,
			"montane": TreeGenerator.STAND_MONTANE_CONIFER, "hedgerow": TreeGenerator.STAND_HEDGEROW,
			"stonepine": 13, "windthrow": 17, "old_growth": 18, "taiga": 19, "alder_carr": 20, "floodplain_willow": 21, "beaver_pond": 22, "deer_wood": 23, "ice_storm": 24} # STAND_STONE_PINE, STAND_WINDTHROW (by value: older binaries parse it too)
	var b: Biome
	if args.has("montado"):
		# --montado: the Iberian wood pasture, open-grown cork oaks with stone pines among them over a summer-dry
		# sward (the stone pine parkland stand plus cork oak, open-grown, --montado-spacing m apart (14)).
		b = tg.make_stand(13, variants) # STAND_STONE_PINE
		tg.preset = TreeGenerator.PRESET_CORK_OAK
		var cork: PlantSpecies = tg.make_species(false, variants)
		var msp := float(args.get("montado-spacing", "14"))
		cork.spacing = msp
		var all: Array[PlantSpecies] = [cork]
		all.append_array(b.species)
		b.species = all
	elif args.has("tree-stand"):
		# --tree-stand=beech|oak|montane|hedgerow|stonepine|windthrow|old_growth|taiga|alder_carr|floodplain_willow|beaver_pond|deer_wood: one stand only (its trees, sapling, snag and log).
		b = tg.make_stand(stands.get(args["tree-stand"], TreeGenerator.STAND_BEECH), variants)
	else:
		b = tg.make_woodland(variants, TreeGenerator.REGION_EUROPE)
	if args.has("lightning") and "lightning_years" in tg:
		# --lightning[=Y]: a few Norway spruces struck Y years ago (0.3; trees a9e426e0: the pale strip down the stem,
		# bark and splinters at the foot for two years, greying over 1-4) among the stand, --lightning-spacing m apart
		# (60); --view framing with --lightning-cam (side-lit, --lightning-dist 6 m).
		var keep_preset = tg.preset
		tg.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
		tg.lightning_years = float(args.get("lightning", "0.3")) if String(args["lightning"]) != "" else 0.3
		var struck: PlantSpecies = tg.make_species(false, 1)
		tg.lightning_years = -1.0
		tg.preset = keep_preset
		struck.resource_name = "Norway spruce (lightning struck)"
		struck.spacing = float(args.get("lightning-spacing", "60"))
		var all3: Array[PlantSpecies] = [struck]
		all3.append_array(b.species)
		b.species = all3
	if args.has("host-tree") and "mistletoe" in tg:
		# --host-tree=lime|silver_fir|scots_pine: a few heavily infested hosts among the stand (trees.md 5n/5ay):
		# --mistletoe=A (3) balls, --brooms=B (silver fir broom rust, 3), --host-spacing m apart (50); --host-cam frames
		# one crown from below against the sky (--host-dist 14 m, --host-look 12 m up the stem).
		var hosts := {"lime": TreeGenerator.PRESET_LIME, "silver_fir": TreeGenerator.PRESET_SILVER_FIR, "scots_pine": TreeGenerator.PRESET_SCOTS_PINE, "hawthorn": TreeGenerator.PRESET_HAWTHORN, "white_willow": TreeGenerator.PRESET_WHITE_WILLOW, "birch": TreeGenerator.PRESET_BIRCH, "oak": TreeGenerator.PRESET_OAK}
		var hn := String(args["host-tree"]) if String(args["host-tree"]) != "" else "lime"
		var keep_p = tg.preset
		var keep_m: float = tg.mistletoe
		var keep_w: float = tg.witches_brooms
		tg.preset = hosts.get(hn, TreeGenerator.PRESET_LIME)
		tg.mistletoe = float(args.get("mistletoe", "3"))
		tg.witches_brooms = float(args.get("brooms", "3"))
		var hsp: PlantSpecies = tg.make_species(false, 1)
		tg.preset = keep_p
		tg.mistletoe = keep_m
		tg.witches_brooms = keep_w
		hsp.resource_name = "%s (mistletoe host)" % hn
		hsp.spacing = float(args.get("host-spacing", "50"))
		var all4: Array[PlantSpecies] = [hsp]
		all4.append_array(b.species)
		b.species = all4
	if args.has("forest-edge") and tg.has_method("make_edge"):
		# --forest-edge[=stand]: the edge species (trees.md 5as: the shrub mantle out in the open, edge trees one-sided
		# toward it) with the stand's own; vegetation places them by RULE_FOREST_EDGE off the canopy's edge.
		var en := String(args["forest-edge"])
		var es: int = stands.get(en, stands.get(args.get("tree-stand", "beech"), TreeGenerator.STAND_BEECH))
		var eb: Biome = tg.make_edge(es, variants)
		var all2: Array[PlantSpecies] = []
		all2.append_array(b.species)
		all2.append_array(eb.species)
		b.species = all2
		_log("forest edge: %d edge species" % eb.species.size())
	_log("generated woodland: %d species" % b.species.size())
	return b

func _tree_wind() -> void:
	var tw := TreeWind3D.new()
	tw.name = "TreeWind"
	add_child(tw)
	tw.target = tw.get_path_to(veg)
	tw.weather_map = tw.get_path_to(weather)

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

# Ground mist (VegetationMist3D: Flow gas pooled in hollows, its amount from the weather) and pollen puffs
# (VegetationPollen3D: flowering grass shaken by movers in "grass_trample"). Both inert without a Flow build.
# --mist=0 / --pollen=0 leave them out; --mist-amount=A overrides the weather's amount.
func _build_mist_pollen() -> void:
	if args.get("mist", "1") != "0":
		var m := VegetationMist3D.new()
		m.name = "Mist"
		add_child(m)
		m.terrain = m.get_path_to(gen)
		if weather != null:
			m.weather_map = m.get_path_to(weather)
		m.sun_light = m.get_path_to(sun)
		m.follow_target = m.get_path_to(cam)
		if args.has("mist-amount"):
			m.mist_amount = float(args["mist-amount"])
	if args.get("pollen", "1") != "0" and veg != null:
		var p := VegetationPollen3D.new()
		p.name = "Pollen"
		add_child(p)
		p.vegetation_field = p.get_path_to(veg)
		if weather != null:
			p.weather_map = p.get_path_to(weather)
			if weather.has_method("get_season"):
				p.season = weather.get_season()
	if args.get("pollen-cloud", "1") != "0" and ClassDB.class_exists("PollenCloud3D"):
		# Pollen clouds (vegetation 1a910ada): dry wind sheds pollen from conifer crowns and the flowering sward round
		# the camera into a Flow haze of its own; it finds the fields itself. --pollen-cloud=0 for none.
		var pc = ClassDB.instantiate("PollenCloud3D")
		pc.name = "PollenCloud"
		add_child(pc)
		pc.set("terrain_path", pc.get_path_to(gen))


func _vec2_arg(v: String) -> Vector2:
	var p := v.split(",")
	return Vector2(float(p[0]), float(p[1]) if p.size() > 1 else float(p[0]))


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
		"slipface":
			_slipface_camera()
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
		"talus":
			# Standing 1.7 m high 20 to 30 m out from the foot of a talus cone (the densest 40 m patch of scree stones,
			# --talus-pick=N for the Nth densest), looking back up at the cone and the cliff face above it.
			var tcells := {}
			var talus_layer := 1
			if rocks != null:
				# --rock-layer=scree|tor|coast|rockfall|glacial|river|shore: which layer's densest cells to look at.
				var lname: String = args.get("rock-layer", "scree")
				var layer_id := {"rockfall": 0, "scree": 1, "glacial": 2, "river": 3, "shore": 4, "cliff": 5}.get(lname, 1) as int
				if lname in ["tor", "coast"] and ClassDB.class_has_integer_constant("RockField3D", "LAYER_" + lname.to_upper()):
					layer_id = ClassDB.class_get_integer_constant("RockField3D", "LAYER_" + lname.to_upper())
				elif lname == "scree":
					layer_id = RockField3D.LAYER_SCREE
				talus_layer = layer_id
				for st in rocks.get_stones(layer_id, Rect2(-1800, -1800, 3600, 3600)):
					var tp: Vector3 = st["position"]
					var tk := Vector2i(int(floor(tp.x / 40.0)), int(floor(tp.z / 40.0)))
					tcells[tk] = int(tcells.get(tk, 0)) + 1
			var keys := tcells.keys()
			keys.sort_custom(func(x, y): return tcells[x] > tcells[y])
			var pick := mini(int(args.get("talus-pick", "0")), maxi(keys.size() - 1, 0))
			var ts := boulder_spot
			if keys.size() > 0:
				ts = Vector3(keys[pick].x * 40.0 + 20.0, 0, keys[pick].y * 40.0 + 20.0)
			ts.y = height_at(ts.x, ts.z)
			# Uphill: the steepest rise within 80 m; the camera stands downhill from the cone's foot.
			var upd := Vector3(0, 0, 1)
			var rise := -INF
			for k in 16:
				var ang := TAU * k / 16.0
				var d := Vector3(cos(ang), 0, sin(ang))
				var hh := height_at(ts.x + d.x * 80.0, ts.z + d.z * 80.0) - ts.y
				if hh > rise:
					rise = hh
					upd = d
			# The foot: walk downhill until the ground flattens, then 25 m further out.
			var foot := ts
			for k in (40 if args.get("talus-foot", "1") != "0" else 0):
				# Downhill until the layer's stones end (the toe of the cone), or the ground flattens.
				var q := foot - upd * 4.0
				var drop := height_at(foot.x, foot.z) - height_at(q.x, q.z)
				foot = q
				if rocks != null and rocks.get_stones(talus_layer, Rect2(q.x - 4.0, q.z - 4.0, 8.0, 8.0)).is_empty():
					break
				if drop < 0.6:
					break
			cam.near = float(args.get("near", "0.5")) # stalks right at the lens stay out
			var cp := foot - upd * float(args.get("talus-dist", "25"))
			if args.get("rock-layer", "") == "tor":
				# A tor on a crest: stand on the crest beside it (the heading whose ground stays nearest the tor's level),
				# the sun behind the camera's shoulder.
				var td := float(args.get("talus-dist", "30"))
				var to_sun2 := sun.global_basis.z
				var sf2 := Vector3(to_sun2.x, 0, to_sun2.z).normalized()
				var best_s := INF
				for k in 16:
					var d := Vector3(cos(TAU * k / 16.0), 0, sin(TAU * k / 16.0))
					var q := ts + d * td
					var sc := absf(height_at(q.x, q.z) - ts.y) - d.dot(sf2) * 3.0
					if sc < best_s:
						best_s = sc
						cp = q
			if args.get("rock-layer", "") == "coast":
				# Sea stacks: stand on the land above them, looking out to sea over them.
				var land := ts
				for k in 60:
					land += upd * 4.0
					if height_at(land.x, land.z) > 4.0:
						break
				cp = land + upd * float(args.get("talus-dist", "25")) * 0.4
			cam.fov = 55.0
			cam.position = Vector3(cp.x, height_at(cp.x, cp.z) + float(args.get("talus-height", "1.7")), cp.z)
			cam.look_at(Vector3(ts.x, (maxf(ts.y, 0.0) if args.get("rock-layer", "") == "coast" else ts.y) + float(args.get("talus-look-up", "8")), ts.z))
			if args.has("talus-close"):
				# --talus-close=D: a close look down at the ground D m ahead (stone feet on the soil), the sun across.
				var dd := Vector3(ts.x - cam.position.x, 0, ts.z - cam.position.z).normalized()
				var gp := cam.position + dd * float(args["talus-close"])
				cam.look_at(Vector3(gp.x, height_at(gp.x, gp.z) + 0.2, gp.z))
			if args.has("talus-stone") and rocks != null:
				# --talus-stone=D: frame the nearest mid-size stone (0.5-2 m) from D m, sun 40 deg off the camera's back,
				# eye 1.3 m, looking at its foot (the ground blend band).
				var c0 := cam.position
				var best_d := INF
				var bp := Vector3.INF
				for st in rocks.get_stones(talus_layer, Rect2(c0.x - 25.0, c0.z - 25.0, 50.0, 50.0)):
					var sc: Variant = st.get("scale", 1.0)
					var sz: float = (sc.x if sc is Vector3 else float(sc))
					var sp: Vector3 = st.get("position", Vector3.INF)
					var dd := Vector2(sp.x - c0.x, sp.z - c0.z).length()
					if sz >= 0.5 and sz <= 2.0 and dd < best_d:
						best_d = dd
						bp = sp
				if bp != Vector3.INF:
					var to_s := sun.global_basis.z
					var sfl := Vector3(to_s.x, 0, to_s.z).normalized().rotated(Vector3.UP, deg_to_rad(40.0))
					var cq := bp + sfl * float(args["talus-stone"])
					cam.position = Vector3(cq.x, height_at(cq.x, cq.z) + float(args.get("talus-height", "1.3")), cq.z)
					cam.look_at(Vector3(bp.x, height_at(bp.x, bp.z) + 0.2, bp.z))
					_log("talus stone at %s framed from %.1f m" % [str(bp.round()), float(args["talus-stone"])])
			_log("talus: %d scree cells, pick %d (%d stones) at %s, foot %s, cam %.1f,%.1f,%.1f, rise %.0f m in 80 m" % [keys.size(), pick, tcells.get(keys[pick], 0) if keys.size() > 0 else 0, str(ts.round()), str(foot.round()), cam.position.x, cam.position.y, cam.position.z, rise])
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
		"gully":
			# A raised oblique down a real gully onto its fan (RockField3D.runoff_gravel: a coarse lag on the floor,
			# fine grains on the fan). The strongest open gully on soil (TerrainGenerator3D.get_gully_at, as the rocks
			# bench's --gully-spot: 15-35 deg over 16 m, banks under 4 m) within --gully-search (900) m; the camera
			# --gully-back (14) m up the fall line and --gully-up (20) m over the ground, looking --gully-ahead (22) m
			# down it. Needs the droplet tiles (--tiles=1).
			var gbest := -INF
			var gp := Vector3.ZERO
			var gdown := Vector3.FORWARD
			var gr := int(args.get("gully-search", "900"))
			for zz in range(-gr, gr + 1, 6):
				for xx in range(-gr, gr + 1, 6):
					var p := Vector3(xx, 0, zz)
					var h := gen.get_height(p)
					if gen.get_geology_at(p).g > 0.05 or h < gen.sea_level + 10.0:
						continue
					var gx := gen.get_height(p + Vector3(8, 0, 0)) - gen.get_height(p - Vector3(8, 0, 0))
					var gz := gen.get_height(p + Vector3(0, 0, 8)) - gen.get_height(p - Vector3(0, 0, 8))
					var ny := Vector3(-gx, 16.0, -gz).normalized().y
					if ny < cos(deg_to_rad(35.0)) or ny > cos(deg_to_rad(15.0)):
						continue
					var gl: Vector2 = gen.get_gully_at(p, ny)
					if gl.x < 0.5 or gl.y > 0.3:
						continue
					var tt := Vector3(-gz, 0, gx).normalized() * 10.0
					var bank := maxf(gen.get_height(p + tt), gen.get_height(p - tt)) - h
					if bank > 4.0:
						continue
					var score := gl.x - bank * 0.1
					if score > gbest:
						gbest = score
						gp = p
						gdown = Vector3(-gx, 0, -gz).normalized()
			_log("gully view: spot (%.0f, %.1f, %.0f) score %.2f, down (%.2f, %.2f)" % [gp.x, gen.get_height(gp), gp.z, gbest, gdown.x, gdown.z])
			cam.fov = 50.0
			var gc := gp - gdown * float(args.get("gully-back", "14"))
			cam.position = Vector3(gc.x, height_at(gc.x, gc.z) + float(args.get("gully-up", "20")), gc.z)
			var gl2 := gp + gdown * float(args.get("gully-ahead", "22"))
			cam.look_at(Vector3(gl2.x, height_at(gl2.x, gl2.z), gl2.z))
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
			if args.has("meadow-eye") and not args.has("hay"):
				# A stand from the clearing (trees' plot views): --meadow-eye m (5) up, --meadow-back m (50) out from the
				# edge, looking at --meadow-look-h m (8) up the edge so the crowns fill the frame over the grass.
				var mb := float(args.get("meadow-back", "50"))
				var mz := ez + mb
				cam.position = Vector3(ex - 10.0, height_at(ex - 10.0, mz) + float(args.get("meadow-eye", "5")), mz)
				cam.look_at(Vector3(ex, height_at(ex, ez) + float(args.get("meadow-look-h", "8")), ez))
			if args.has("hay"):
				# Over the hay fields (_hay_list): raised to --hay-eye (5) m, looking down across the rows to the edge.
				cam.position.y += float(args.get("hay-eye", "5")) - 1.7
				cam.look_at(Vector3(ex - 10.0, height_at(ex - 10.0, ez + 12.0) + 1.0, ez + 12.0))
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
		"lakeshore":
			# Standing on the lake's shore with the sun behind, looking down into water 0.5 to 2 m deep (clear water,
			# caustics, shadows in water): the shallow cell on the sunward shore with the most such cells round it.
			var sdir := Vector3(0, 0, 1)
			if clock != null:
				var sd2 := clock.get_sun_direction()
				sdir = Vector3(sd2.x, 0, sd2.z).normalized()
			var span2 := maxf(lake_axes.x, 60.0) * 1.6
			var best_q := Vector3(lake_center.x, 0, lake_center.z)
			var best_s := -INF
			var gx2 := -span2
			while gx2 <= span2:
				var gz2 := -span2
				while gz2 <= span2:
					var q := Vector3(lake_center.x + gx2, 0, lake_center.z + gz2)
					var dep := lake_level - height_at(q.x, q.z)
					var avoid := _vec3_arg(args.get("shore-avoid", "480,0,-1632")) # the hanging lake's outlet and fall
					if dep > 0.5 and dep < 2.0 and Vector2(q.x - avoid.x, q.z - avoid.z).length() > float(args.get("shore-avoid-r", "150")):
						var land := q + sdir * 12.0
						if height_at(land.x, land.z) > lake_level + 0.3:
							var n_sh := 0
							for k in 8:
								var r := q + Vector3(cos(TAU * k / 8.0), 0, sin(TAU * k / 8.0)) * 8.0
								var d2 := lake_level - height_at(r.x, r.z)
								if d2 > 0.3 and d2 < 2.5:
									n_sh += 1
							var sc := n_sh + (q - Vector3(lake_center.x, 0, lake_center.z)).normalized().dot(sdir) * 2.0
							if sc > best_s:
								best_s = sc
								best_q = q
					gz2 += 4.0
				gx2 += 4.0
			var cp2 := best_q + sdir * 6.0
			cam.fov = 55.0
			cam.position = Vector3(cp2.x, maxf(height_at(cp2.x, cp2.z), lake_level) + float(args.get("shallows-height", "3")), cp2.z)
			var tgt := best_q - sdir * float(args.get("shallows-out", "6"))
			cam.look_at(Vector3(tgt.x, lake_level - 1.0, tgt.z))
			_log("lakeshore cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f, depth there %.1f m" % [cam.position.x, cam.position.y, cam.position.z, tgt.x, lake_level - 1.0, tgt.z, lake_level - height_at(tgt.x, tgt.z)])
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
			# --fall-fill=F: the drop fills that share of the frame height (default about two thirds).
			var fill := float(args.get("fall-fill", "0.67"))
			cam.fov = clampf(rad_to_deg(2.0 * atan(0.5 * h / fill / best.distance_to(mid))), 10.0, 70.0)
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
		"dunes":
			# The foredune from the upper beach (docs framing): a marram-held crest --dune-min..--dune-max m of sand
			# (3..8), the camera on the beach below it, back along the shore so the view runs 30 degrees to the
			# shoreline with the sea on the near side, --dune-eye m up (1.8).
			cam.fov = 55.0
			var dr: Rect2 = gen.get_dune_rect()
			var crest := Vector3(40.0, 0.0, coast_z - 40.0)
			var best := -1.0
			var dmin := float(args.get("dune-min", "3"))
			var dmax := float(args.get("dune-max", "8"))
			if dr.size.x > 0.0:
				var stp := 4.0
				for zi in int(dr.size.y / stp):
					for xi in int(dr.size.x / stp):
						var q := Vector3(dr.position.x + xi * stp, 0.0, dr.position.y + zi * stp)
						var dh: float = gen.get_dune_height(q)
						if absf(q.x) > float(args.get("dune-xmax", "1600")):
							continue # keep land beyond the crest in the view
						if dh > best and dh >= dmin and dh <= dmax and gen.get_dune_stability(q) > float(args.get("dune-stable", "0.5")):
							best = dh
							crest = q
			crest.y = height_at(crest.x, crest.z)
			var sea_n := Vector3(0, 0, 1) # the sea lies to +z of this coast
			var beach := crest
			for k in 200:
				beach = crest + sea_n * (k * 1.0)
				if gen.get_dune_height(beach) < 0.3 and height_at(beach.x, beach.z) < gen.sea_level + 3.0:
					break
			var out := crest.distance_to(Vector3(beach.x, crest.y, beach.z)) + float(args.get("dune-beach", "8"))
			while out > 5.0 and height_at(crest.x, crest.z + out) < gen.sea_level + 0.6:
				out -= 1.0
			var along := Vector3(float(args.get("dune-side", "1")), 0, 0)
			var pos := crest + sea_n * out - along * out / tan(deg_to_rad(30.0))
			cam.position = Vector3(pos.x, maxf(height_at(pos.x, pos.z), gen.sea_level) + float(args.get("dune-eye", "1.8")), pos.z)
			cam.look_at(crest + Vector3(0, best * 0.5, 0) + sea_n * out * float(args.get("dune-sea", "0.35"))) # the sea on the near side
			_log("dunes view: crest %s dune %.1f m stability %.2f, beach at %.0f m, cam %s (%.0f m out), ground there %.1f m over the sea, dune there %.2f m" % [str(crest.round()), best, gen.get_dune_stability(crest), crest.distance_to(Vector3(beach.x, crest.y, beach.z)), str(cam.position.round()), out, height_at(pos.x, pos.z) - gen.sea_level, gen.get_dune_height(pos)])
		"rills":
			# Down the beach face to the waterline (the --rills runoff): standing --rills-eye (1.5 m) over the face where
			# it is --rills-top (1.0 m) above the sea, looking at the point --rills-low (0.1 m) above it, at x --rills-x (40).
			var rx := float(args.get("rills-x", "40"))
			var top := Vector3.INF
			var low := Vector3.INF
			for k in 400:
				var z := coast_z - 60.0 + k * 0.25
				var hs := height_at(rx, z) - gen.sea_level
				if top == Vector3.INF and hs <= float(args.get("rills-top", "1.0")):
					top = Vector3(rx, height_at(rx, z), z)
				if hs <= float(args.get("rills-low", "0.1")):
					low = Vector3(rx, height_at(rx, z), z)
					break
			if top == Vector3.INF or low == Vector3.INF:
				top = Vector3(rx, 2.0, coast_z - 6.0)
				low = Vector3(rx, 0.1, coast_z)
			cam.fov = 50.0
			cam.position = top + Vector3(0, float(args.get("rills-eye", "1.5")), 0)
			cam.look_at(low)
			_log("rills view: face %s to waterline %s (%.1f m)" % [str(top.snapped(Vector3.ONE * 0.1)), str(low.snapped(Vector3.ONE * 0.1)), top.distance_to(low)])
		"shallows":
			# Looking down into the shallows from the waterline (clear water, shadows in water): 6 m up on the beach,
			# the view on water about 0.5 to 2 m deep 10 to 30 m out.
			cam.fov = 55.0
			cam.position = Vector3(40.0, 0.0, coast_z - 6.0)
			cam.position.y = maxf(height_at(cam.position.x, cam.position.z), 0.0) + float(args.get("shallows-height", "6"))
			cam.look_at(Vector3(25.0, -0.8, coast_z + float(args.get("shallows-out", "22"))))
			_log("shallows cam %.1f,%.1f,%.1f, coast at z %.1f" % [cam.position.x, cam.position.y, cam.position.z, coast_z])
		"stones":
			# Ground contact: a few stones of --stones-size (0.3,0.6 m) on a soil slope of 8-25 degrees (no talus), seen
			# from --stones-dist (1.5 m) on their uphill side at --stones-eye (1.0 m), the uphill direction picked about
			# 90 degrees off the sun so the light comes across. Rockfall and glacial layers, whole world.
			var szr := _vec2_arg(args.get("stones-size", "0.3,0.6"))
			var to_sun := Vector3(sun.global_basis.z.x, 0, sun.global_basis.z.z).normalized()
			var best_score := -INF
			var bp := Vector3.INF
			var bup := Vector3.ZERO
			var n_seen := 0
			if rocks != null:
				var all: Array = []
				for layer in [0, 2]:
					all.append_array(rocks.get_stones(layer, Rect2(-2048, -2048, 4096, 4096)))
				var pts: Array = []
				for st in all:
					var sc: Variant = st.get("scale", 1.0)
					var sz: float = (sc.x if sc is Vector3 else float(sc))
					if sz >= szr.x and sz <= szr.y:
						pts.append(st.get("position", Vector3.ZERO))
				n_seen = pts.size()
				var cells := {}
				for q in pts:
					var key := Vector2i(floori(q.x / 2.5), floori(q.z / 2.5))
					cells[key] = cells.get(key, 0) + 1
				for i in range(0, pts.size(), maxi(1, pts.size() / 4000)):
					var p: Vector3 = pts[i]
					if p.y < 10.0 or gen.get_talus(p) > 0.15:
						continue
					var g := Vector2(height_at(p.x + 1, p.z) - height_at(p.x - 1, p.z), height_at(p.x, p.z + 1) - height_at(p.x, p.z - 1))
					var sl := rad_to_deg(atan(g.length() / 2.0))
					if sl < 8.0 or sl > 25.0:
						continue
					var nb := -1
					var ck := Vector2i(floori(p.x / 2.5), floori(p.z / 2.5))
					for dx in [-1, 0, 1]:
						for dz in [-1, 0, 1]:
							nb += cells.get(ck + Vector2i(dx, dz), 0)
					if nb < 1 or nb > 8:
						continue
					var up := Vector3(g.x, 0, g.y).normalized()
					var score := 1.0 - absf(up.dot(to_sun)) - absf(nb - 3) * 0.1
					if score > best_score:
						best_score = score
						bp = p
						bup = up
			if bp == Vector3.INF:
				_log("stones view: no stones %s on a soil slope (%d in size)" % [str(szr), n_seen])
			else:
				var cq := bp + bup * float(args.get("stones-dist", "1.5"))
				if args.get("stones-face", "") == "shade":
					# The shaded face (moss): stand on the side away from the sun, a little off the sun line.
					cq = bp - to_sun.rotated(Vector3.UP, deg_to_rad(30.0)) * float(args.get("stones-dist", "1.5"))
				cam.fov = 55.0
				cam.position = Vector3(cq.x, height_at(cq.x, cq.z) + float(args.get("stones-eye", "1.0")), cq.z)
				cam.look_at(Vector3(bp.x, height_at(bp.x, bp.z) + 0.1, bp.z))
				_log("stones view at %.1f,%.1f,%.1f (%d candidates, score %.2f), cam %.1f,%.1f,%.1f" % [bp.x, bp.y, bp.z, n_seen, best_score, cam.position.x, cam.position.y, cam.position.z])
		"marsh":
			# The salt marsh: the most sheltered level ground (under 4 degrees) 0.1 to 2.5 m over the sea
			# (TerrainGenerator3D.get_coast_exposure_at), or with --marsh=shingle the backshore: beach ground
			# (get_beach_at over 0.5) 1.5 to 5 m over the sea. Seen from --marsh-dist (25 m) inland at eye 1.7 m.
			var shingle: bool = args.get("marsh", "") == "shingle"
			var hr2 := Vector2(1.5, 5.0) if shingle else Vector2(0.1, 2.5)
			var mb := Vector3.INF
			var ms := INF
			for i in range(0, heights.size(), 2):
				var p := Vector3(X0 + (i % NX) * STEP, heights[i], Z0 + (i / NX) * STEP)
				var hs: float = p.y - gen.sea_level
				if hs < hr2.x or hs > hr2.y or absf(p.x) > 1800.0 or absf(p.z) > 1800.0:
					continue
				var g := Vector2(height_at(p.x + 4, p.z) - height_at(p.x - 4, p.z), height_at(p.x, p.z + 4) - height_at(p.x, p.z - 4))
				if rad_to_deg(atan(g.length() / 8.0)) > (8.0 if shingle else 4.0):
					continue
				var sc := 0.0
				if shingle:
					if gen.get_beach_at(p) < 0.5:
						continue
					sc = -gen.get_beach_at(p)
				else:
					sc = gen.get_coast_exposure_at(p)
				if sc < ms:
					ms = sc
					mb = p
			if args.has("marsh-at"):
				var ma := _vec2_arg(args["marsh-at"])
				mb = Vector3(ma.x, height_at(ma.x, ma.y), ma.y)
			if mb == Vector3.INF:
				mb = Vector3(0, gen.sea_level, coast_z)
				_log("marsh view: no %s ground found" % ("shingle" if shingle else "marsh"))
			var gm := Vector2(height_at(mb.x + 8, mb.z) - height_at(mb.x - 8, mb.z), height_at(mb.x, mb.z + 8) - height_at(mb.x, mb.z - 8))
			var inland := Vector3(gm.x, 0, gm.y).normalized() if gm.length() > 0.01 else Vector3(0, 0, -1)
			var mp := mb + inland * float(args.get("marsh-dist", "25"))
			cam.position = Vector3(mp.x, height_at(mp.x, mp.z) + 1.7, mp.z)
			cam.look_at(Vector3(mb.x, height_at(mb.x, mb.z) + 0.3, mb.z))
			_log("marsh view (%s) at %.0f,%.1f,%.0f score %.2f, cam %.1f,%.1f,%.1f" % ["shingle" if shingle else "marsh", mb.x, mb.y, mb.z, ms, cam.position.x, cam.position.y, cam.position.z])
		"hollow":
			# Dawn mist: the most concave gentle ground (data map curvature, slope under 10 degrees) at --hollow-h
			# (20,500 m) away from water, seen from --hollow-dist (30 m) on the side away from the sun at eye 1.7 m.
			var hr := _vec2_arg(args.get("hollow-h", "20,500"))
			var hb := Vector3.INF
			var hc := -INF
			for i in range(0, heights.size(), 2):
				var p := Vector3(X0 + (i % NX) * STEP, heights[i], Z0 + (i / NX) * STEP)
				if p.y < hr.x or p.y > hr.y or absf(p.x) > 1800.0 or absf(p.z) > 1800.0 or data_map == null:
					continue
				var c := data_map.get_curvature_at(p)
				if c <= hc:
					continue
				var g := Vector2(height_at(p.x + 4, p.z) - height_at(p.x - 4, p.z), height_at(p.x, p.z + 4) - height_at(p.x, p.z - 4))
				if rad_to_deg(atan(g.length() / 8.0)) > 10.0 or gen.get_talus(p) > 0.1:
					continue
				if data_map.get_water_distance_at(p) < 15.0:
					continue
				hc = c
				hb = p
			if hb == Vector3.INF:
				hb = Vector3(0, height_at(0, 0), 0)
			var away := -Vector3(sun.global_basis.z.x, 0, sun.global_basis.z.z).normalized()
			away = away.rotated(Vector3.UP, deg_to_rad(float(args.get("hollow-side", "60"))))
			var hp := hb + away * float(args.get("hollow-dist", "30"))
			cam.position = Vector3(hp.x, height_at(hp.x, hp.z) + 1.7, hp.z)
			cam.look_at(Vector3(hb.x, height_at(hb.x, hb.z) + 0.5, hb.z))
			_log("hollow view at %.0f,%.0f,%.0f curvature %.4f, cam %.1f,%.1f,%.1f" % [hb.x, hb.y, hb.z, hc, cam.position.x, cam.position.y, cam.position.z])
		"alpineline":
			# The tree line: standing at --line-h (870 m) on gentle ground (under 20 degrees), looking uphill across
			# the last flagged spruce to the krummholz mats and the alpine plants above. Eye 1.7 m, looking up --line-up (3) degrees.
			var lh := float(args.get("line-h", "870"))
			var bestp := Vector3.INF
			var bestd := INF
			for i in range(0, heights.size(), 2):
				var p := Vector3(X0 + (i % NX) * STEP, heights[i], Z0 + (i / NX) * STEP)
				var dh := absf(p.y - lh)
				if dh > 15.0 or absf(p.x) > 1900.0 or absf(p.z) > 1900.0:
					continue
				var g := Vector2(height_at(p.x + 4, p.z) - height_at(p.x - 4, p.z), height_at(p.x, p.z + 4) - height_at(p.x, p.z - 4))
				var sl := rad_to_deg(atan(g.length() / 8.0))
				if sl > 20.0 or sl < 5.0:
					continue
				# Prefer ground that keeps rising for 150 m uphill (the line above, not a knoll).
				var upd := Vector3(g.x, 0, g.y).normalized()
				var rise := height_at(p.x + upd.x * 150.0, p.z + upd.z * 150.0) - p.y
				var score := dh - rise * 0.2
				if score < bestd:
					bestd = score
					bestp = p
			if bestp == Vector3.INF:
				bestp = _highest_point()
			var g2 := Vector2(height_at(bestp.x + 4, bestp.z) - height_at(bestp.x - 4, bestp.z), height_at(bestp.x, bestp.z + 4) - height_at(bestp.x, bestp.z - 4))
			var updir := Vector3(g2.x, 0, g2.y).normalized() if g2.length() > 0.01 else Vector3(0, 0, -1)
			cam.position = Vector3(bestp.x, height_at(bestp.x, bestp.z) + 1.7, bestp.z)
			var tgt := cam.position + updir * 100.0
			tgt.y = cam.position.y + 100.0 * tan(deg_to_rad(float(args.get("line-up", "3"))))
			cam.look_at(tgt)
			_log("alpine line view at %.0f,%.0f,%.0f looking %s, temp %.1f" % [bestp.x, bestp.y, bestp.z, str(updir.snapped(Vector3.ONE * 0.01)), data_map.get_temperature_at(bestp) if data_map else 0.0])
		"alpine":
			# Above the treeline on the scree: the lowest grid point with a mean annual temperature under
			# --alpine-tmax (default 3.6 C mean annual, about 1000 m here: alpine by growing-season warmth), talus over --alpine-talus (0.5) and a
			# slope under 35 degrees; the camera --alpine-dist (5 m) downslope at --alpine-eye (1.3 m), looking up it.
			var tmax := float(args.get("alpine-tmax", "5.0"))
			var tal := float(args.get("alpine-talus", "0"))
			# Cold enough, inside the world's edge, under 35 degrees: the most talus (the lowest such point once
			# talus reaches --alpine-talus), so the scree set shows if there is any scree up there.
			var best := Vector3.INF
			var best_s := -INF
			for i in range(0, heights.size(), 3):
				var p := Vector3(X0 + (i % NX) * STEP, heights[i], Z0 + (i / NX) * STEP)
				if data_map == null or absf(p.x) > 1900.0 or absf(p.z) > 1900.0:
					continue
				if data_map.get_temperature_at(p) > tmax:
					continue
				var gx := height_at(p.x + 2, p.z) - height_at(p.x - 2, p.z)
				var gz := height_at(p.x, p.z + 2) - height_at(p.x, p.z - 2)
				if rad_to_deg(atan(Vector2(gx, gz).length() / 4.0)) > 35.0:
					continue
				# Fellfield: convex ground (curvature below 0) with little talus; --alpine-talus=T > 0.5 asks for scree.
				var sc := (minf(data_map.get_talus_at(p), tal) if tal > 0.5 else -data_map.get_curvature_at(p) - maxf(data_map.get_talus_at(p) - 0.3, 0.0)) - p.y * 0.00001
				if sc > best_s:
					best_s = sc
					best = p
			if best == Vector3.INF:
				best = _highest_point()
				_log("alpine view: nothing above the treeline, peak instead")
			# The camera across the sun (light from the side, --alpine-side degrees off the sun's back, default 90),
			# looking down at the plants.
			var ts2 := Vector3(sun.global_basis.z.x, 0, sun.global_basis.z.z).normalized()
			var down := ts2.rotated(Vector3.UP, deg_to_rad(float(args.get("alpine-side", "90"))))
			var dist := float(args.get("alpine-dist", "3"))
			cam.position = best + down * dist
			cam.position.y = maxf(height_at(cam.position.x, cam.position.z), height_at(best.x, best.z)) + float(args.get("alpine-eye", "1.3"))
			cam.look_at(Vector3(best.x, height_at(best.x, best.z), best.z))
			_log("alpine view at %.0f,%.0f,%.0f temp %.1f talus %.2f" % [best.x, best.y, best.z, data_map.get_temperature_at(best) if data_map else 0.0, data_map.get_talus_at(best) if data_map else 0.0])
		"peak":
			var top := _highest_point()
			var off := Vector3(-120, 0, 160)
			cam.position = top + off
			cam.position.y = height_at(cam.position.x, cam.position.z) + 3.0
			cam.look_at(top + Vector3(0, -40, 0))
		"cliff":
			# A real rock face for the cliff trees (TreeGenerator cliff_angle, trees 37524fb2): the cell on a 16 m grid
			# with the most 58-85 degree ground round it, under --cliff-max-h (1200) m and over the sea, with gentler
			# ground --cliff-dist (35) m out from its foot for the camera. --cliff-cam=front|side (side: the camera
			# turned --cliff-side-deg, 70, round the face), eye 1.7 m, looking at the face --cliff-look-h (12) m up.
			var wr: Rect2 = gen.get_world_rect()
			var e := 4.0
			var best := -1.0
			var bp := Vector3.ZERO
			var bn := Vector3.ZERO
			var maxh := float(args.get("cliff-max-h", "1200"))
			var dist := float(args.get("cliff-dist", "35"))
			var steep := func(x: float, z: float) -> Vector3:
				var gx := (height_at(x + e, z) - height_at(x - e, z)) / (2.0 * e)
				var gz := (height_at(x, z + e) - height_at(x, z - e)) / (2.0 * e)
				return Vector3(gx, rad_to_deg(atan(sqrt(gx * gx + gz * gz))), gz)
			var x := wr.position.x + 64.0
			while x < wr.end.x - 64.0:
				var z := wr.position.y + 64.0
				while z < wr.end.y - 64.0:
					var g: Vector3 = steep.call(x, z)
					var h := height_at(x, z)
					if g.y > 58.0 and g.y < 85.0 and h < maxh and h > float(gen.sea_level) + 8.0:
						var n := 0.0
						for dx in [-16.0, 0.0, 16.0]:
							for dz in [-16.0, 0.0, 16.0]:
								var g2: Vector3 = steep.call(x + dx, z + dz)
								n += 1.0 if g2.y > 52.0 else 0.0
						var down := -Vector3(g.x, 0, g.z).normalized()
						var foot := Vector3(x, 0, z) + down * dist
						var fg: Vector3 = steep.call(foot.x, foot.z)
						if fg.y < 30.0 and height_at(foot.x, foot.z) > float(gen.sea_level) + 0.5 and n > best:
							best = n
							bp = Vector3(x, h, z)
							bn = down
					z += 16.0
				x += 16.0
			var side: bool = args.get("cliff-cam", "front") == "side"
			var cd := bn.rotated(Vector3.UP, deg_to_rad(float(args.get("cliff-side-deg", "70")))) if side else bn
			var cpos := bp + cd * dist
			cam.fov = 60.0
			cam.position = Vector3(cpos.x, height_at(cpos.x, cpos.z) + 1.7, cpos.z)
			cam.look_at(Vector3(bp.x, bp.y + float(args.get("cliff-look-h", "12")), bp.z))
			_log("cliff face at %s (score %.0f), out %s, cam %s" % [str(bp.round()), best, str(bn.snapped(Vector3.ONE * 0.01)), str(cam.position.round())])
		_:
			cam.position = Vector3(300, 520, coast_z + 900.0)
			cam.look_at(Vector3(-100, 250, -700))
	if args.has("cam") and args.has("look"):
		# A fixed shot for before/after pairs: --cam=x,y,z --look=x,y,z (y of either below 0: that far above the ground;
		# the camera's above the sea where the ground is under water).
		var cv := _vec3_arg(args["cam"])
		var lv := _vec3_arg(args["look"])
		if cv.y < 0.0:
			# Above the ground, or above the sea where the ground is under it.
			cv.y = maxf(height_at(cv.x, cv.z), gen.sea_level if gen != null else -INF) - cv.y
		if lv.y < 0.0:
			lv.y = height_at(lv.x, lv.z) - lv.y
		cam.position = cv
		cam.look_at(lv)
		_log("cam %.1f,%.1f,%.1f look %.1f,%.1f,%.1f" % [cv.x, cv.y, cv.z, lv.x, lv.y, lv.z])
		if clock != null:
			_log("sun toward %s (altitude %.1f deg)" % [clock.get_sun_direction(), clock.get_sun_altitude()])
	if args.has("pitch"):
		# --pitch=DEG: the camera tilted to DEG above the horizontal (a sky shot), keeping its heading.
		var hf := -cam.global_basis.z
		hf.y = 0.0
		hf = hf.normalized()
		if args.has("heading"):
			# --heading=DEG: the compass heading instead (0 north = -Z, 90 east = +X, 270 west), e.g. into a sunset.
			var hd := deg_to_rad(float(args["heading"]))
			hf = Vector3(sin(hd), 0.0, -cos(hd))
		var pr := deg_to_rad(float(args["pitch"]))
		cam.look_at(cam.global_position + hf * cos(pr) + Vector3.UP * sin(pr))
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
			"oak": PlantGenerator.FLOOR_OAK_MIXED, "hedgerow": PlantGenerator.FLOOR_HEDGEROW, "pineheath": PlantGenerator.FLOOR_PINE_HEATH,
			"mediterranean": 10, "taiga": 11} # FLOOR_MEDITERRANEAN, FLOOR_BOREAL_TAIGA (montado: cistus, grasses, thistles; by value for older binaries)
	# The floor plants follow the weather's season (flowering, bracken croziers in spring, russet in autumn): --day=N.
	var season: Dictionary = weather.get_season() if weather != null and weather.has_method("get_season") else {}
	var floor_sp: Array = PlantGenerator.make_floor(floors.get(args.get("floor", "taiga" if args.get("tree-stand", "") == "taiga" else "montane"), PlantGenerator.FLOOR_MONTANE_CONIFER), 3, true, 15.0, season)
	if OS.get_environment("VEG_DW_OFF") != "":
		# The deadwood pair's before frame (as the vegetation bench): the floor blind to the logs.
		for sp in floor_sp:
			if sp.deadwood_rule == PlantSpecies.DEADWOOD_ON or sp.deadwood_rule == PlantSpecies.DEADWOOD_BESIDE:
				sp.density = 0.0
			else:
				sp.deadwood_rule = PlantSpecies.DEADWOOD_IGNORE
	if args.get("floor", "") == "oak" and floor_sp.size() > 1:
		floor_sp[1].set_meta("bench_bracken", true) # FLOOR_OAK_MIXED: bramble, bracken, ... (the --bracken view's plant)
	b.species = floor_sp.slice(0, 3) + _wetland_species() + meadow_sp + floor_sp.slice(3)
	if args.get("bank-turf", "1") != "0" and ClassDB.class_has_method("PlantGenerator", "make_bank_turf"):
		# Turf overhangs (PlantGenerator.make_bank_turf, vegetation f785a81d): sod lips on the river banks and scarps
		# where the ground drops past a steep edge (RULE_BANK_EDGE). First, since earlier species win footprints.
		var bt: Array = ClassDB.class_call_static("PlantGenerator", "make_bank_turf", 3, true, season)
		var lead: Array = []
		for bts in bt:
			lead.append(bts)
		b.species = lead + b.species
		_log("bank turf: %d species" % bt.size())
	if args.get("bank-roots", "1") != "0" and ClassDB.class_has_method("PlantGenerator", "make_bank_roots"):
		# Bank roots (PlantGenerator.make_bank_roots, vegetation be31837e): hanging and exposed roots on cut banks;
		# the woody kind only near canopy. Appended to the cover biome (no river biome here). --bank-roots=0 for none.
		var br: Array = ClassDB.class_call_static("PlantGenerator", "make_bank_roots", 6, true, season)
		var tail: Array = []
		for brs in br:
			tail.append(brs)
		b.species = b.species + tail
		_log("bank roots: %d species" % br.size())
	if args.has("mounds") and ClassDB.class_has_method("PlantGenerator", "make_mounds"):
		# --mounds=meadow|wood|both: ant hills and molehills in the meadows, wood ant nests at conifer edges
		# (PlantGenerator.make_mounds, vegetation e90904e7); called by name so older binaries still parse.
		var mk := String(args["mounds"]) if String(args["mounds"]) != "" else "both"
		var msets := []
		if mk == "meadow" or mk == "both":
			msets.append(0) # MOUNDS_MEADOW
		if mk == "wood" or mk == "both":
			msets.append(1) # MOUNDS_WOOD
		for ms in msets:
			var mounds: Array = ClassDB.class_call_static("PlantGenerator", "make_mounds", ms, 3, true, season)
			for msp in mounds:
				b.species.append(msp)
			_log("mounds set %d: %d species" % [ms, mounds.size()])
	if args.has("fairy-rings") and ClassDB.class_has_method("PlantGenerator", "make_fairy_rings"):
		# --fairy-rings: the rings' mushrooms (PlantGenerator.make_fairy_rings, vegetation 98569e47) on the same world
		# rings the grass draws (GrassField3D.fairy_rings, on by default).
		var fr: Array = ClassDB.class_call_static("PlantGenerator", "make_fairy_rings", 3, true, season)
		for fsp in fr:
			b.species.append(fsp)
		_log("fairy rings: %d species" % fr.size())
	if args.has("dew-webs") and ClassDB.class_has_method("PlantGenerator", "make_dew_webs"):
		# --dew-webs[=D]: sheet and orb webs on the open grass (PlantGenerator.make_dew_webs, vegetation 98569e47),
		# shown by the weather's dew and frost on autumn mornings; D (0..1) forces the dew through the season's "dew".
		var ws: Dictionary = season.duplicate()
		if String(args["dew-webs"]) != "":
			ws["dew"] = float(args["dew-webs"])
		var dw: Array = ClassDB.class_call_static("PlantGenerator", "make_dew_webs", 3, true, ws)
		for dsp in dw:
			b.species.append(dsp)
		_log("dew webs: %d species" % dw.size())
	if args.has("grazing") and ClassDB.class_has_method("PlantGenerator", "make_grazing"):
		# --grazing=cattle|horse|deer: dung and the ungrazed roughs round it over the open ground (PlantGenerator.
		# make_grazing, vegetation fbc92404), listed after the stand's species; called by name for older binaries.
		var gk := String(args["grazing"]) if String(args["grazing"]) != "" else "cattle"
		var gset := {"cattle": 0, "horse": 1, "deer": 2}.get(gk, 0) as int
		var graze: Array = ClassDB.class_call_static("PlantGenerator", "make_grazing", gset, 3, true, season)
		for gsp in graze:
			b.species.append(gsp)
		_log("grazing %s: %d species" % [gk, graze.size()])
	if gen.dunes_enabled and args.get("dune-plants", "1") != "0":
		# The dunes' own plants (marram, lyme grass, sea holly), placed by distance to the water on the sand.
		for st in [PlantGenerator.DUNES_EMBRYO, PlantGenerator.DUNES_MOBILE, PlantGenerator.DUNES_FIXED]:
			b.species += PlantGenerator.make_dunes(st, 3, true, season)
	if args.get("alpine-plants", "1") != "0":
		# Above the treeline (the data map's mean annual temperature): fellfield cushions on the ridges, the sedge
		# sward, dwarf willow in the snowbeds and the scree set on the talus, each by its own rules.
		for st in [PlantGenerator.ALPINE_FELLFIELD, PlantGenerator.ALPINE_GRASSLAND, PlantGenerator.ALPINE_SNOWBED, PlantGenerator.ALPINE_SCREE]:
			for sp in PlantGenerator.make_alpine(st, 3, true, season):
				sp.set_meta("alpine", true)
				b.species.append(sp)
	if args.get("heath-plants", "1") != "0":
		# Atlantic heath (bell heather, cross-leaved heath, crowberry) by rain, winter cold and wetness.
		for st in [PlantGenerator.HEATH_DRY, PlantGenerator.HEATH_WET, PlantGenerator.HEATH_MARITIME]:
			for sp in PlantGenerator.make_heath(st, 3, true, season, args.get("woody-shrubs", "1") != "0"):
				sp.set_meta("own_rules", true)
				b.species.append(sp)
	if args.get("scrub-plants", "1") != "0":
		# Mediterranean scrub (rosemary, French lavender, thyme, rockrose): garrigue on thin soil, maquis on deeper,
		# only in a warm dry climate (--jan=10 --jul=24 --precip=650); none in the default one.
		for st in [PlantGenerator.SCRUB_GARRIGUE, PlantGenerator.SCRUB_MAQUIS]:
			for sp in PlantGenerator.make_scrub(st, 3, true, season, args.get("woody-shrubs", "1") != "0"):
				sp.set_meta("own_rules", true)
				b.species.append(sp)
	if args.get("marsh-plants", "1") != "0":
		# Salt marsh zoned by height in the tidal frame and the shingle backshore above it (the data map's
		# geology source is the generator with its sea), each set by its own tide, exposure and beach rules.
		for st in [PlantGenerator.SALTMARSH_PIONEER, PlantGenerator.SALTMARSH_MID, PlantGenerator.SALTMARSH_HIGH, PlantGenerator.SALTMARSH_SHINGLE]:
			for sp in PlantGenerator.make_saltmarsh(st, 3, true, season):
				sp.set_meta("own_rules", true)
				b.species.append(sp)
	for sp in b.species:
		var ps := sp as PlantSpecies
		if ps.has_meta("dunes") or ps.has_meta("alpine") or ps.has_meta("own_rules"):
			continue # the dune plants keep their own rules (sand, water distance, angle of repose)
		# Plants on soil, not on bare rock, scree or steep faces, and not on the beach sand.
		ps.set_rule_range(PlantSpecies.RULE_ROCK, Vector2(0, 0.3))
		ps.set_rule_fade(PlantSpecies.RULE_ROCK, 0.1)
		if not ps.floating:
			ps.set_rule_range(PlantSpecies.RULE_SLOPE, Vector2(0, 32))
			ps.set_rule_fade(PlantSpecies.RULE_SLOPE, 5.0)
		ps.set_rule_range(PlantSpecies.RULE_HEIGHT, Vector2(3, 100000))
		if not ps.has_meta("floor"):
			_set_far_color(ps)
	if rocks != null and args.get("stone-plants", "1") != "0":
		# The plants between the stones (scree, river gravel, shingle): they keep to the gaps between the obstacles.
		var stone_sp: Array = []
		for st in [PlantGenerator.STONES_SCREE, PlantGenerator.STONES_RIVER, PlantGenerator.STONES_SHORE]:
			stone_sp.append_array(PlantGenerator.make_stones(st, 3, true, season))
		b.species = b.species + stone_sp
		_log("cover: %d stone-crevice species" % stone_sp.size())
	cover = VegetationField3D.new()
	if "still_shot" in cover:
		cover.set("still_shot", args.get("still-shot", "1") != "0")
	cover.name = "Cover"
	cover.tile_size = 8.0
	cover.biome = b
	cover.forest_floor_enabled = false # the forest's own field publishes the floor
	if "far_volume" in cover:
		cover.far_volume = args.get("far-volume", "1") != "0" # --far-volume=0: the far cover as flat colour only
	cover.streaming = true
	cover.stream_radius = 80.0 # the herbs draw to 10 to 60 m
	if not worn_paths.is_empty():
		cover.paths = worn_paths
	add_child(cover)
	cover.data_map = cover.get_path_to(data_map)
	cover.canopy_field = cover.get_path_to(veg) # shade and conifer share from the forest's crowns
	for f in sward + [cover]:
		if f != null and "obstacles" in f and not boulder_obstacles.is_empty():
			f.obstacles = boulder_obstacles # nothing grows through the boulders
	for g in sward:
		if "undergrowth_field" in g:
			g.undergrowth_field = g.get_path_to(cover) # the sward thins under bracken and bramble stands
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
	if pg.preset in [PlantGenerator.PRESET_KNAPWEED, PlantGenerator.PRESET_SCABIOUS, PlantGenerator.PRESET_WILD_CARROT, PlantGenerator.PRESET_SORREL]:
		lc = lc.lerp(pg.petal_color, 0.35) # tall flowers: their colour shows from afar
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
	# Tall meadow flowers (the grass bench's recipe): knapweed and sorrel through the meadow, scabious and wild carrot
	# on the drier ground, standing clear of the grass.
	var tall := _plant([PlantGenerator.PRESET_KNAPWEED, PlantGenerator.PRESET_SORREL], 0.35, 0.08, 0.8, 30.0, 7.0, 0.5, 61)
	tall.scale_range = Vector2(1.1, 1.4)
	var tall_dry := _plant([PlantGenerator.PRESET_SCABIOUS, PlantGenerator.PRESET_WILD_CARROT], 0.4, 0.08, 0.75, 30.0, 8.0, 0.35, 67)
	tall_dry.scale_range = Vector2(1.1, 1.4)
	tall_dry.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(8.0, 1000000.0))
	tall_dry.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 4.0)
	var list := [flowers, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry, tall, tall_dry]
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
	check(rainfall.get_rainfall_rate() > 0.1 or weather.rain_rate <= 0.0, "falling rain particles from the weather (RainFall3D: %.1f mm/h, %d drops, visibility %.0f m; runoff %.1f m3/s at most)" % [rainfall.get_rainfall_rate(), rainfall.get_visible_drops(), rainfall.get_visibility(), runoff.get_max_discharge_in(Rect2(-2048, -2048, 4096, 4096))])
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
	# Falling leaves -> trees: LeafFall3D takes its crowns from the vegetation's trees, with their species' leaves.
	var leaf_fall := get_node_or_null("LeafFall")
	if leaf_fall != null and veg != null:
		var cam_p := get_viewport().get_camera_3d().global_position if get_viewport().get_camera_3d() else Vector3()
		var near_trees: Array = veg.get_crowns_near(cam_p, 2000.0)
		var at := cam_p
		for d in near_trees:
			if int(d.preset) >= 0:
				at = d.position
				break
		leaf_fall.refresh_crowns_near(at)
		check(leaf_fall.get_crown_count() > 0 and leaf_fall.get_kind_count() > 0, "falling leaves shed from the vegetation's tree crowns, with their species' leaves (%d crowns, %d leaf kinds, %d trees within 2 km)" % [leaf_fall.get_crown_count(), leaf_fall.get_kind_count(), near_trees.size()])
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
	weather.set("air_follows_climate", false) # a fixed cold gale, whatever the date
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


# --buggy=x,z[,heading deg]: the offroad buggy (CC BY 4.0, Direct Dimensions) from the art repository (art/, fetched by fetch_art.sh; skipped when absent),
# set 5 cm above the ground and left 2 s to settle. --buggy-drive=throttle,seconds[,steer] drives it, then brakes.
# The footprint patch and (--buggy-grains) the MPM roost follow it. --buggy-cam=dist,height,side frames it.
# --worn-path: a footpath winding away from the camera (--path-length 70 m, --path-width 0.7 m, --path-passes 1000
# walkers a year, a busy path), on the grass fields and the plant fields alike: plants thin and shorten by their
# trampling resistance, the tread wears bare. It starts --path-start (2 m) ahead and swings --path-swing (3 m) side
# to side every 30 m, so it winds toward the camera.
func _build_worn_path() -> void:
	var c3 := get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := fwd.cross(Vector3.UP).normalized()
	var pts := PackedVector3Array()
	var start := float(args.get("path-start", "2"))
	var length := float(args.get("path-length", "70"))
	var swing := float(args.get("path-swing", "3"))
	var d := start
	while d <= length:
		var q := c3.global_position + fwd * d + right * swing * sin(d / 30.0 * TAU)
		pts.append(Vector3(q.x, height_at(q.x, q.z), q.z))
		d += 2.0
	worn_paths = [{"points": pts, "width": float(args.get("path-width", "0.7")), "passes": float(args.get("path-passes", "1000"))}]
	for g in sward:
		g.paths = worn_paths
	if grass != null and not sward.has(grass):
		grass.paths = worn_paths
	if veg != null:
		veg.paths = worn_paths
	_log("worn path: %d points from %s" % [pts.size(), str(pts[0].round())])


# --rills: runoff channels on the beach round the camera (GroundSurface3D.runoff_channels): a still tide
# (tide_fake.gd) --rills-high (1.2 m) over the sea soaks the face, then falls to 0.1 m over it and the seepage cuts
# rills down to the waterline for --rills-frames physics frames (2400) at runoff_time_scale --rills-scale (120).
func _rills() -> void:
	var tide := Node3D.new()
	tide.name = "TideStandIn"
	tide.set_script(load("res://tide_fake.gd"))
	add_child(tide)
	tide.set("level", gen.sea_level + float(args.get("rills-high", "1.2")))
	prints.water_sources = [prints.get_path_to(tide)]
	prints.runoff_channels = true
	prints.runoff_time_scale = float(args.get("rills-scale", "120"))
	RenderingServer.render_loop_enabled = false
	for i in 900:
		await get_tree().physics_frame
	tide.set("level", gen.sea_level + 0.1)
	for i in int(args.get("rills-frames", "2400")):
		await get_tree().physics_frame
		if i % 300 == 0:
			_log("rills: falling tide frame %d" % i)
	prints.sync_deformation()
	for i in 3:
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	_log("rills: runoff cut over the beach round %s" % str(get_viewport().get_camera_3d().global_position.round()))


# --storm-patch: a few rigged trees (--storm-presets, default 4,4,1,1: Norway spruce, beech) --storm-ahead (14 m)
# in front of the camera among the standing forest, then a storm (TreeWind3D storm breaks and windthrow; Blast
# falls) of --storm-speed (34 m/s at 10 m, rising from 12 over 3 s) for --storm-seconds (14), wind across the view
# so thrown trees fall sideways. Run godot with --fixed-fps 30. The picture is taken after the storm, in raster.
func _storm_patch() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := fwd.cross(Vector3.UP).normalized()
	var mid := c3.global_position + fwd * float(args.get("storm-ahead", "14"))
	var patch := Node3D.new()
	patch.name = "StormPatch"
	add_child(patch)
	var picks := String(args.get("storm-presets", "4,4,1,1")).split(",")
	var season: Dictionary = weather.get_season() if weather != null and weather.has_method("get_season") else {}
	for k in picks.size():
		var g := TreeGenerator.new()
		g.preset = int(picks[k])
		g.seed = 11 + k
		g.rigged = true
		if "needle_sprays" in g:
			g.needle_sprays = true
		if "season" in g:
			g.season = season
		var m := g.generate()
		var off := (float(k) - (picks.size() - 1) * 0.5) * float(args.get("storm-gap", "7"))
		var p := mid + right * off + fwd * (3.0 if k % 2 == 1 else 0.0)
		p.y = height_at(p.x, p.z) - 0.05
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = TreeWind3D.make_multimesh(m, [Transform3D(Basis(), p)])
		patch.add_child(mmi)
	var wind := TreeWind3D.new()
	wind.name = "StormWind"
	var speed := float(args.get("storm-speed", "34"))
	# Wind across the view (fallback_wind is x, z), so the thrown trees fall sideways in the frame.
	var wdir := Vector2(right.x, right.z)
	wind.fallback_wind = wdir * 12.0
	wind.fallback_turbulence = 0.25
	if "storm_enabled" in wind:
		wind.storm_enabled = true
	wind.storm_anchorage = float(args.get("storm-anchorage", "0.6"))
	wind.lod_enabled = false
	patch.add_child(wind)
	wind.target = wind.get_path_to(patch)
	wind.rescan()
	if landscape != null:
		landscape.collision_target = landscape.get_path_to(patch)
		landscape.update_collision()
	var frames := int(float(args.get("storm-seconds", "14")) * 30.0)
	RenderingServer.render_loop_enabled = false
	for f in frames:
		wind.fallback_wind = wdir * lerpf(12.0, speed, clampf(f / 90.0, 0.0, 1.0))
		await get_tree().physics_frame
		if f % 60 == 0:
			_log("storm t=%.0f s: %d breaks, %d thrown, %d falls" % [f / 30.0, wind.get_storm_break_count(), wind.get_storm_throw_count(), wind.get_storm_fall_count()])
	wind.fallback_wind = wdir * 6.0
	for f in 60:
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	_log("storm: %d breaks, %d thrown, %d Blast falls at %s" % [wind.get_storm_break_count(), wind.get_storm_throw_count(), wind.get_storm_fall_count(), str(mid.round())])


# --campfire: a Flow campfire (CC0 Poly Haven dry branches from docs/media/showcase/fetch_ext.sh) on the ground
# --fire-ahead (3) m in front of the camera and --fire-side (-0.9) m to the side, with --fire-wind (2) m/s blowing
# to the right of the view, so the plume leans across the frame. --flag: a cloth flag on a pole --flag-at (2.5) m
# downwind, blown by the fire's gas. --debris=N: dry leaves, twigs and ash flakes round the fire that its updraft
# lifts (--push-bodies=0 for the pair without). --fire-spread: PhysXFireSpread3D (--fire-size 16 m) on the grass
# beside it, lit at its upwind side. --fire-seconds (9) of fixed-step frames run before the capture.
func _campfire() -> void:
	if not ClassDB.class_exists("PhysXFlow3D"):
		push_error("This build has no PhysX Flow.")
		return
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := fwd.cross(Vector3.UP).normalized()
	var wind := right * float(args.get("fire-wind", "2"))
	var at := c3.global_position + fwd * float(args.get("fire-ahead", "3")) + right * float(args.get("fire-side", "-0.9"))
	at.y = height_at(at.x, at.z)
	var site := Node3D.new()
	site.name = "Campfire"
	add_child(site)
	var flow = null
	if args.has("campfire"):
		var ext := ProjectSettings.globalize_path("res://").path_join("../../media/showcase/ext/branches/dry_branches_medium_01_1k.gltf").simplify_path()
		if FileAccess.file_exists(ext):
			var doc := GLTFDocument.new()
			var gs := GLTFState.new()
			doc.append_from_file(ext, gs)
			var sticks: Node3D = doc.generate_scene(gs)
			var meshes := sticks.find_children("*", "MeshInstance3D", true, false)
			for k in 7:
				var src: MeshInstance3D = meshes[k % meshes.size()]
				var m := MeshInstance3D.new()
				m.mesh = src.mesh
				var a := TAU * k / 7 + 0.3
				# Leaned into a cone: foot on the ground, tip over the fire's centre.
				var foot := at + Vector3(cos(a) * 0.3, height_at(at.x + cos(a) * 0.3, at.z + sin(a) * 0.3) - at.y, sin(a) * 0.3)
				var tip := at + Vector3(0.02 * cos(3.0 * a), 0.36, 0.02 * sin(3.0 * a))
				var z := (tip - foot).normalized()
				var x := z.cross(Vector3.UP).normalized()
				m.transform = Transform3D(Basis(x, z.cross(x), z).scaled(Vector3.ONE * 0.45), (foot + tip) * 0.5)
				site.add_child(m)
			sticks.free()
		else:
			_log("campfire: no branches (run docs/media/showcase/fetch_ext.sh)")
		flow = ClassDB.instantiate("PhysXFlow3D")
		flow.name = "FireFlow"
		flow.cell_size = float(args.get("fire-cell", "0.04"))
		flow.smoke_density = 0.7
		flow.combustion = true
		flow.cooling_rate = 7.0 # flames about 0.7 m tall (Heskestad) for a ~50 kW campfire
		flow.vorticity = 4.0
		flow.buoyancy = 4.0
		flow.emission_strength = 8.0
		flow.temperature_range = 1.0
		flow.smoke_fade = 0.3 # the plume carries downwind past the flag
		flow.push_bodies = args.get("push-bodies", "1") != "0"
		if args.has("heat-shimmer") and "heat_shimmer" in flow:
			flow.set("heat_shimmer", args["heat-shimmer"] != "0") # on by default; --heat-shimmer=0 for the off half of a pair
		if "terrain_path" in flow:
			flow.terrain_floor = true
		site.add_child(flow)
		if "terrain_path" in flow:
			flow.terrain_path = flow.get_path_to(gen)
		var em = ClassDB.instantiate("PhysXFlowEmitter3D")
		em.position = at + Vector3(0, 0.05, 0)
		em.shape = 1 # box: a bed of burning wood
		em.size = Vector3(0.32, 0.06, 0.32)
		em.fuel = 1.0
		em.temperature = 1.0
		em.burn = 0.0
		em.smoke = 0.02
		em.divergence = 0.3
		em.velocity = Vector3(0, 1.0, 0)
		flow.add_child(em)
		em.flow = em.get_path_to(flow)
		# The breeze: a wide box of air at the wind's speed round the fire (as PhysXFireSpread3D drives its own).
		var air = ClassDB.instantiate("PhysXFlowEmitter3D")
		# Only along the plume's path (fire to flag): every cell the air box fills is simulated.
		air.position = at + Vector3(0, 1.1, 0) + right * 1.6
		air.basis = Basis(right, Vector3.UP, right.cross(Vector3.UP))
		air.shape = 1
		air.size = Vector3(4.4, 2.2, 1.4)
		air.smoke = 0.0
		air.temperature = 0.0
		air.couple_rate = 0.5
		air.velocity = wind
		flow.add_child(air)
		air.flow = air.get_path_to(flow)
		_log("campfire at %s, wind %s, push_bodies %s" % [str(at), str(wind), str(flow.push_bodies)])
		if args.get("fire-clearing", "1") != "0" and grass != null:
			# A campsite is trodden bare: a worn patch (the grass fields' paths) from behind the fire to past the flag.
			var cl := PackedVector3Array()
			for k in 6:
				var q := at + right * lerpf(-1.2, float(args.get("flag-at", "2.5")) + 1.0, k / 5.0)
				cl.append(Vector3(q.x, height_at(q.x, q.z), q.z))
			var wp := [{"points": cl, "width": float(args.get("clearing-width", "3.0")), "passes": 4000.0}]
			for g in sward:
				g.paths = wp
			if not sward.has(grass):
				grass.paths = wp
	if args.has("flag") and flow != null and ClassDB.class_exists("PhysXCloth3D"):
		var fp := at + right * float(args.get("flag-at", "2.5"))
		fp.y = height_at(fp.x, fp.z)
		var pole_h := float(args.get("flag-h", "2.2"))
		# The Tripo flagpole (a whitewashed pole in a stone cairn) when the art is there, a plain pole otherwise.
		var tpole := _tripo("flagpole", pole_h, 1)
		if tpole != null:
			tpole.position = fp
			tpole.rotation.y = atan2(right.x, right.z)
			site.add_child(tpole)
		else:
			var pole := MeshInstance3D.new()
			var cm := CylinderMesh.new()
			cm.top_radius = 0.02
			cm.bottom_radius = 0.025
			cm.height = pole_h
			var wood := StandardMaterial3D.new()
			wood.albedo_color = Color(0.32, 0.24, 0.16)
			wood.roughness = 0.8
			cm.material = wood
			pole.mesh = cm
			pole.position = fp + Vector3(0, pole_h * 0.5, 0)
			site.add_child(pole)
		var cloth = ClassDB.instantiate("PhysXCloth3D")
		cloth.name = "Flag"
		cloth.grid_size = Vector2(0.9, 0.6)
		cloth.grid_columns = 30
		cloth.grid_rows = 20
		cloth.pin_mode = 3 # left edge on the pole
		cloth.density = 0.15
		cloth.collision_enabled = false
		cloth.wind = wind
		# The grid lies in its local XY plane: X downwind, Y up, its left edge at the pole.
		cloth.transform = Transform3D(Basis(right, Vector3.UP, right.cross(Vector3.UP)), fp + Vector3(0, pole_h - 0.35, 0) + right * 0.47)
		var fab := StandardMaterial3D.new()
		fab.albedo_color = Color(0.62, 0.08, 0.06)
		fab.roughness = 0.9
		fab.cull_mode = BaseMaterial3D.CULL_DISABLED
		cloth.material_override = fab
		site.add_child(cloth)
		cloth.flow = cloth.get_path_to(flow)
		_log("flag at %s" % str(fp))
	var nd := int(args.get("debris", "0"))
	if nd > 0:
		var rng := RandomNumberGenerator.new()
		rng.seed = 7
		var leaf_mats := [Color(0.42, 0.26, 0.12), Color(0.55, 0.36, 0.16), Color(0.33, 0.22, 0.12)]
		for i in nd:
			var kind := i % 4 # 0, 1 leaves, 2 twig, 3 ash flake
			var rb := RigidBody3D.new()
			var mi := MeshInstance3D.new()
			var col := CollisionShape3D.new()
			var mat := StandardMaterial3D.new()
			mat.cull_mode = BaseMaterial3D.CULL_DISABLED
			mat.roughness = 0.9
			if kind == 2:
				var tm := CylinderMesh.new()
				tm.top_radius = 0.003
				tm.bottom_radius = 0.004
				tm.height = rng.randf_range(0.06, 0.14)
				tm.radial_segments = 5
				tm.rings = 1
				mat.albedo_color = Color(0.3, 0.22, 0.15)
				tm.material = mat
				mi.mesh = tm
				var cs := CylinderShape3D.new()
				cs.radius = 0.005
				cs.height = tm.height
				col.shape = cs
				rb.mass = 0.004
			else:
				# A leaf (an ellipse with a midrib fold) or a grey ash flake.
				var w := rng.randf_range(0.03, 0.06) if kind < 2 else rng.randf_range(0.008, 0.02)
				var l := w * (1.6 if kind < 2 else 1.0)
				var st := SurfaceTool.new()
				st.begin(Mesh.PRIMITIVE_TRIANGLES)
				var n := 10
				for j in n:
					var a0 := TAU * j / n
					var a1 := TAU * (j + 1) / n
					st.add_vertex(Vector3(0, 0.003, 0))
					st.add_vertex(Vector3(cos(a0) * w * 0.5, absf(cos(a0)) * 0.004, sin(a0) * l * 0.5))
					st.add_vertex(Vector3(cos(a1) * w * 0.5, absf(cos(a1)) * 0.004, sin(a1) * l * 0.5))
				st.generate_normals()
				mi.mesh = st.commit()
				mat.albedo_color = leaf_mats[i % 3] if kind < 2 else Color(0.55, 0.55, 0.53)
				mi.material_override = mat
				var bs := CylinderShape3D.new()
				bs.radius = maxf(w, l) * 0.4
				bs.height = 0.004
				col.shape = bs
				rb.mass = 0.001 if kind < 2 else 0.0002
			rb.add_child(mi)
			rb.add_child(col)
			var r := rng.randf_range(0.25, 1.1) if kind != 3 else rng.randf_range(0.0, 0.25)
			var a := rng.randf() * TAU
			var p := at + Vector3(cos(a) * r, 0, sin(a) * r)
			p.y = height_at(p.x, p.z) + (0.05 if kind != 3 else 0.3)
			rb.position = p
			rb.rotation = Vector3(rng.randf() * 0.4, rng.randf() * TAU, rng.randf() * 0.4)
			rb.linear_damp = 0.5
			rb.angular_damp = 0.5
			site.add_child(rb)
		_log("debris: %d bodies round the fire" % nd)
	if args.has("fire-spread") and ClassDB.class_exists("PhysXFireSpread3D"):
		var fs = ClassDB.instantiate("PhysXFireSpread3D")
		fs.name = "FireSpread"
		var sz := float(args.get("fire-size", "16"))
		var fc := at + fwd * float(args.get("spread-ahead", "6")) + right * (sz * 0.25)
		var downwind := args.has("fire-downwind")
		if downwind:
			# --fire-downwind: the wind blows away from the camera; lit near it, the front runs off ahead and its
			# embers start spot fires downwind (PhysXFireSpread3D spotting). The camera stands 3 m behind the light.
			wind = fwd * wind.length()
			fc = at + fwd * (sz * 0.45)
		fc.y = height_at(fc.x, fc.z)
		fs.position = fc
		fs.size = Vector2(sz, sz)
		fs.use_weather_wind = false
		fs.wind = wind
		fs.spread_scale = float(args.get("spread-scale", "2"))
		fs.fuel_type = int(args.get("fuel-type", "1"))
		site.add_child(fs)
		var ig := fc - (fwd if downwind else right) * (sz * 0.35)
		ig.y = height_at(ig.x, ig.z)
		if downwind:
			var dcp := ig - fwd * 3.0 + right * 0.8
			c3.global_position = Vector3(dcp.x, height_at(dcp.x, dcp.z) + float(args.get("fire-cam-h", "1.7")), dcp.z)
			c3.look_at(Vector3(fc.x, fc.y + 1.0, fc.z))
		if veg != null and "vegetation_field" in fs:
			fs.vegetation_field = fs.get_path_to(veg)
		fs.ignite(ig, 0.5)
		_log("fire spread %s m at %s, lit at %s" % [str(sz), str(fc), str(ig)])
		if args.has("fire-pines"):
			# --fire-pines=PRESET: two pines inside the burn (--fire-pines-at (0.15) of the size downwind of the centre,
			# 3.5 m either side along the view). The first is a crown the fire can light (add_tree, from its mesh's
			# bounds); the second is not given to the fire, a survivor whose bark still chars from the ground fire.
			var ptg := TreeGenerator.new()
			ptg.preset = int(args["fire-pines"])
			var pmesh = ptg.generate()
			var bb: AABB = pmesh.get_aabb() if pmesh is Mesh else AABB(Vector3(-3, 0, -3), Vector3(6, 12, 6))
			var pc0 := fc + right * sz * float(args.get("fire-pines-at", "0.15"))
			for k in 2:
				ptg.seed = 11 + k
				var pm2 = pmesh if k == 0 else ptg.generate()
				var tp2 := pc0 + fwd * (3.5 if k == 0 else -3.5)
				tp2.y = height_at(tp2.x, tp2.z) - 0.05
				var mm := MultiMeshInstance3D.new()
				mm.multimesh = TreeWind3D.make_multimesh(pm2, [Transform3D(Basis(), tp2)])
				site.add_child(mm)
				if k == 0:
					var ht := bb.size.y
					fs.add_tree(tp2, ht, maxf(ht * 0.25, 1.0), maxf(bb.size.x, bb.size.z) * 0.5, true)
			_log("fire pines preset %s at %s, %.1f m tall" % [String(args["fire-pines"]), str(pc0.round()), bb.size.y])
		if args.has("fire-tree"):
			# --fire-tree=PRESET: one generated tree in the fire's path (downwind of the centre), hidden while the fire
			# runs (a full crown draws slowly on the CPU renderer) and shown for the last second.
			var tg := TreeGenerator.new()
			tg.preset = int(args["fire-tree"]) if String(args["fire-tree"]).is_valid_int() else 4
			tg.seed = 3
			var tp := fc + right * sz * float(args.get("fire-tree-at", "0.2"))
			tp.y = height_at(tp.x, tp.z) - 0.05
			fire_tree = MultiMeshInstance3D.new()
			fire_tree.multimesh = TreeWind3D.make_multimesh(tg.generate(), [Transform3D(Basis(), tp)])
			fire_tree.visible = false
			site.add_child(fire_tree)
			_log("fire tree preset %d at %s" % [tg.preset, str(tp)])
		if args.has("fire-props") and ClassDB.class_exists("PhysXBurnable3D"):
			# --fire-props: a log pile at the fire's downwind edge and a fallen log beyond it (CC0 Poly Haven, fetch_ext.sh),
			# each a PhysXBurnable3D: the pile catches from the front, the log from its embers.
			var base := ProjectSettings.globalize_path("res://ext/")
			var pile := Node3D.new()
			pile.name = "LogPile"
			var pp := fc + right * sz * float(args.get("pile-at", "0.15")) + fwd * 1.0
			pp.y = height_at(pp.x, pp.z)
			pile.transform = Transform3D(Basis(Vector3.UP, atan2(fwd.x, fwd.z)), pp) # logs along the wind (the trunk model lies along X)
			site.add_child(pile)
			var rows := [[-0.3, 0.0, 0.3], [-0.15, 0.15], [0.0]]
			for r in rows.size():
				for zo in rows[r]:
					var lg := _gltf_node(base + "dead_tree_trunk/dead_tree_trunk_1k.gltf")
					if lg == null:
						break
					lg.position = Vector3(0.1 * r, 0.12 + r * 0.24, float(zo))
					lg.rotation = Vector3(0.05 * (r - 1), 0.0, 0.0)
					pile.add_child(lg)
			var bu1 = ClassDB.instantiate("PhysXBurnable3D")
			pile.add_child(bu1)
			bu1.fire = bu1.get_path_to(fs)
			var fl := _gltf_node(base + "dead_tree_trunk_02/dead_tree_trunk_02_1k.gltf")
			if fl != null:
				var lp2 := fc + right * sz * float(args.get("log-at", "0.38")) - fwd * 1.5
				lp2.y = height_at(lp2.x, lp2.z) + 0.05
				fl.transform = Transform3D(Basis(Vector3.UP, atan2(fwd.x, fwd.z) + 0.6), lp2)
				site.add_child(fl)
				var bu2 = ClassDB.instantiate("PhysXBurnable3D")
				fl.add_child(bu2)
				bu2.fire = bu2.get_path_to(fs)
			_log("fire props: log pile at %s, fallen log beyond" % str(pp.round()))
		if args.has("fire-logs") and ClassDB.class_exists("PhysXBurnable3D"):
			# --fire-logs=N: fallen dead branches (CC0 Poly Haven, docs/media/showcase/fetch_ext.sh) on the ground in the
			# fire's path, each a PhysXBurnable3D that chars, glows and throws embers once the front reaches it.
			var ext2 := ProjectSettings.globalize_path("res://").path_join("../../media/showcase/ext/branches/dry_branches_medium_01_1k.gltf").simplify_path()
			if FileAccess.file_exists(ext2):
				var doc2 := GLTFDocument.new()
				var gs2 := GLTFState.new()
				doc2.append_from_file(ext2, gs2)
				var br: Node3D = doc2.generate_scene(gs2)
				var bms := br.find_children("*", "MeshInstance3D", true, false)
				var rng2 := RandomNumberGenerator.new()
				rng2.seed = 21
				for k in int(args["fire-logs"]):
					var src2: MeshInstance3D = bms[k % bms.size()]
					var lg := MeshInstance3D.new()
					lg.mesh = src2.mesh
					var lp := fc + right * sz * rng2.randf_range(-0.05, 0.3) + fwd * rng2.randf_range(-3.0, 3.0)
					lp.y = height_at(lp.x, lp.z) + 0.04
					# Lying down: the branch's length (its local Z in this set) along the ground, a random heading.
					lg.transform = Transform3D(Basis(Vector3.UP, rng2.randf() * TAU) * Basis(Vector3.RIGHT, PI * 0.5).scaled(Vector3.ONE * 1.6), lp)
					site.add_child(lg)
					var bu = ClassDB.instantiate("PhysXBurnable3D")
					lg.add_child(bu)
					bu.fire = bu.get_path_to(fs)
				br.free()
				_log("fire logs: %d burnable branches" % int(args["fire-logs"]))
		if args.has("fire-cam"):
			# --fire-cam=back,up: side-on to the wind, back m from the fire's centre, up m over the ground.
			var fcv := String(args["fire-cam"]).split(",")
			var cp2 := fc + right * sz * 0.1 - fwd * float(fcv[0])
			cp2.y = height_at(cp2.x, cp2.z) + (float(fcv[1]) if fcv.size() > 1 else 3.0)
			c3.global_position = cp2
			c3.look_at(Vector3(fc.x, height_at(fc.x, fc.z) + 1.5, fc.z) + right * sz * 0.1)
			_log("fire cam %s" % str(cp2))
	if args.has("campfire") and not args.has("fire-spread"):
		# The clip's camera: side-on to the wind, --fire-cam-dist (4) m back from the middle of fire and flag,
		# --fire-cam-h (1.2) m up, looking at --fire-look-h (0.8) m over it.
		var mid := at + right * float(args.get("flag-at", "2.5")) * 0.5
		var cp := mid - fwd * float(args.get("fire-cam-dist", "4"))
		cp.y = height_at(cp.x, cp.z) + float(args.get("fire-cam-h", "1.2"))
		c3.global_position = cp
		c3.look_at(Vector3(mid.x, height_at(mid.x, mid.z) + float(args.get("fire-look-h", "0.8")), mid.z))
		_log("fire cam %s" % str(cp))
	var curtain = null
	var curtain_light := Vector3.ZERO
	if args.has("curtain") and ClassDB.class_exists("PhysXCloth3D"):
		# --curtain: a linen sheet hung from a timber rail on two posts over the ground --fire-ahead m in front,
		# swaying in --curtain-wind (1.2) m/s blowing away from the camera; lit at its lower left corner after
		# --curtain-light-at (1) s, so the flame climbs that corner (PhysXCloth3D burnable). Camera --curtain-cam
		# (3.4,1.3) m back and up. Its own Flow carries the flames when there is no campfire.
		var cw := fwd * float(args.get("curtain-wind", "1.2"))
		if flow == null:
			flow = ClassDB.instantiate("PhysXFlow3D")
			flow.name = "CurtainFlow"
			flow.cell_size = float(args.get("fire-cell", "0.04"))
			flow.smoke_density = 0.5
			flow.combustion = true
			flow.cooling_rate = 7.0
			flow.vorticity = 4.0
			flow.buoyancy = 4.0
			flow.emission_strength = 8.0
			flow.temperature_range = 1.0
			flow.smoke_fade = 0.3
			site.add_child(flow)
			if "terrain_path" in flow:
				flow.terrain_floor = true
				flow.terrain_path = flow.get_path_to(gen)
		var rail_h := 2.1
		var span := 2.3
		var wood := StandardMaterial3D.new()
		wood.albedo_color = Color(0.30, 0.22, 0.15)
		wood.roughness = 0.85
		# Tripo timber posts and rail (the rail stretched along its length: Tripo's is stubby) or code shapes.
		var cyaw := atan2(right.x, right.z) - PI * 0.5
		for sd in [-1.0, 1.0]:
			var pp: Vector3 = at + right * (span * 0.5 * sd)
			var tpost := _tripo("timber_post", rail_h + 0.3, 1)
			if tpost != null:
				tpost.position = Vector3(pp.x, height_at(pp.x, pp.z) - 0.3, pp.z)
				tpost.rotation.y = cyaw + 1.3 * sd
				site.add_child(tpost)
				continue
			var post := MeshInstance3D.new()
			var pm := CylinderMesh.new()
			pm.top_radius = 0.045
			pm.bottom_radius = 0.055
			pm.height = rail_h + 0.3
			pm.material = wood
			post.mesh = pm
			post.position = Vector3(pp.x, height_at(pp.x, pp.z) - 0.3 + pm.height * 0.5, pp.z)
			site.add_child(post)
		var trail := _tripo("timber_rail", 0.08, 1)
		if trail != null:
			var tl: Node3D = trail.get_child(0)
			var box := AABB()
			for mi in tl.find_children("*", "MeshInstance3D", true, false):
				box = box.merge((mi as MeshInstance3D).transform * (mi as MeshInstance3D).get_aabb())
			tl.scale.x *= (span + 0.25) / maxf(box.size.x * tl.scale.x, 1e-4)
			trail.transform = Transform3D(Basis(right, Vector3.UP, right.cross(Vector3.UP)), at + Vector3(0, rail_h, 0))
			site.add_child(trail)
		else:
			var rail := MeshInstance3D.new()
			var rm := BoxMesh.new()
			rm.size = Vector3(span + 0.25, 0.08, 0.07)
			rm.material = wood
			rail.mesh = rm
			rail.transform = Transform3D(Basis(right, Vector3.UP, right.cross(Vector3.UP)), at + Vector3(0, rail_h + 0.04, 0))
			site.add_child(rail)
		curtain = ClassDB.instantiate("PhysXCloth3D")
		curtain.name = "Curtain"
		var csz := Vector2(1.9, 1.6)
		curtain.grid_size = csz
		curtain.grid_columns = 48
		curtain.grid_rows = 40
		curtain.pin_mode = 1 # top edge on the rail
		curtain.density = float(args.get("curtain-density", "0.3"))
		curtain.collision_enabled = false
		curtain.wind = cw
		curtain.transform = Transform3D(Basis(right, Vector3.UP, right.cross(Vector3.UP)), at + Vector3(0, rail_h - csz.y * 0.5, 0))
		var lin := StandardMaterial3D.new()
		lin.albedo_color = Color(0.82, 0.78, 0.68)
		lin.roughness = 0.95
		lin.cull_mode = BaseMaterial3D.CULL_DISABLED
		curtain.material_override = lin
		site.add_child(curtain)
		curtain.flow = curtain.get_path_to(flow)
		curtain_light = at - right * (csz.x * 0.5 - 0.08) + Vector3(0, rail_h - csz.y + 0.06, 0)
		var cc := _vec2_arg(String(args.get("curtain-cam", "3.4,1.3")))
		var ccp := at - fwd * cc.x - right * 0.3
		c3.global_position = Vector3(ccp.x, height_at(ccp.x, ccp.z) + cc.y, ccp.z)
		c3.look_at(at + Vector3(0, rail_h * 0.55, 0))
		_log("curtain at %s, wind %s, cam %s" % [str(at), str(cw), str(c3.global_position)])
	var laundry: Array = []
	var laundry_pegs: Array = []
	if args.has("laundry") and ClassDB.class_exists("PhysXCloth3D"):
		# --laundry=dry|wet: a washing line (rope between two posts) --fire-ahead m in front, a sheet, a towel and a
		# pillowcase pegged along its top edge at 0.8 of their width so they sag in swags (PhysXCloth3D move_pins).
		# dry: a --laundry-wind (2) m/s breeze away from the camera, turbulence 0.7; wet: soaked (wetness 1) and
		# nearly still air, so they hang heavy and dark. Camera --laundry-cam (4.6,1.5) m back and up.
		var wet := String(args["laundry"]) == "wet"
		var lw := fwd * float(args.get("laundry-wind", "0.4" if wet else "2"))
		if flow == null:
			flow = ClassDB.instantiate("PhysXFlow3D")
			flow.name = "LaundryFlow"
			flow.cell_size = 0.08
			site.add_child(flow)
		var line_h := 1.9
		var span := 4.4
		var wood := StandardMaterial3D.new()
		wood.albedo_color = Color(0.36, 0.28, 0.19)
		wood.roughness = 0.85
		for sd in [-1.0, 1.0]:
			var pp: Vector3 = at + right * (span * 0.5 * sd)
			var tpost := _tripo("timber_post", line_h + 0.4, 1)
			if tpost != null:
				tpost.position = Vector3(pp.x, height_at(pp.x, pp.z) - 0.35, pp.z)
				tpost.rotation = Vector3(0, 0.7 + 1.9 * sd, 0.04 * sd)
				site.add_child(tpost)
				continue
			var post := MeshInstance3D.new()
			var pm := CylinderMesh.new()
			pm.top_radius = 0.04
			pm.bottom_radius = 0.05
			pm.height = line_h + 0.4
			pm.material = wood
			post.mesh = pm
			post.position = Vector3(pp.x, height_at(pp.x, pp.z) - 0.35 + pm.height * 0.5, pp.z)
			post.rotation = Vector3(0, 0, 0.04 * sd)
			site.add_child(post)
		var rope := MeshInstance3D.new()
		var ropem := CylinderMesh.new()
		ropem.top_radius = 0.005
		ropem.bottom_radius = 0.005
		ropem.height = span
		ropem.radial_segments = 6
		var rmat := StandardMaterial3D.new()
		rmat.albedo_color = Color(0.72, 0.68, 0.58)
		rmat.roughness = 0.9
		ropem.material = rmat
		rope.mesh = ropem
		rope.transform = Transform3D(Basis(Vector3.UP, right, Vector3.UP.cross(right)), at + Vector3(0, line_h, 0))
		site.add_child(rope)
		# Size (m), grid, colour and place along the line (from its left post).
		var items := [
			[Vector2(1.5, 1.2), Vector2i(40, 32), Color(0.86, 0.85, 0.80), 0.35],
			[Vector2(0.8, 0.55), Vector2i(24, 17), Color(0.30, 0.42, 0.58), 2.25],
			[Vector2(0.75, 0.5), Vector2i(22, 15), Color(0.78, 0.62, 0.55), 3.3]]
		for it in items:
			var sz: Vector2 = it[0]
			var gr: Vector2i = it[1]
			var cl = ClassDB.instantiate("PhysXCloth3D")
			cl.simulation_mode = 2 # SIM_CPU
			cl.grid_size = sz
			cl.grid_columns = gr.x
			cl.grid_rows = gr.y
			cl.pin_mode = 0
			var pegs := PackedInt32Array()
			var npg := 4 if sz.x > 1.0 else 3
			for k in npg:
				pegs.append(int(round(float(k) / (npg - 1) * (gr.x - 1))))
			cl.pinned_vertices = pegs
			cl.bend_stiffness = 0.02
			cl.stiffness = 0.8
			cl.density = 0.16
			cl.wind = lw
			cl.wind_turbulence = 0.7
			cl.weather_wind = false
			cl.weather_wetting = false
			cl.collision_enabled = true
			var x0: float = -span * 0.5 + float(it[3]) + sz.x * 0.5
			cl.transform = Transform3D(Basis(right, Vector3.UP, right.cross(Vector3.UP)), at + right * x0 + Vector3(0, line_h - 0.012 - sz.y * 0.5, 0))
			var fm := StandardMaterial3D.new()
			fm.albedo_color = it[2]
			fm.roughness = 0.92
			fm.cull_mode = BaseMaterial3D.CULL_DISABLED
			cl.material_override = fm
			site.add_child(cl)
			cl.flow = cl.get_path_to(flow)
			if wet:
				cl.wetness = 1.0
			laundry.append(cl)
			var pos := PackedVector3Array()
			for k in npg:
				pos.append(Vector3((float(k) / (npg - 1) - 0.5) * sz.x * 0.8, sz.y * 0.5, 0.0))
			laundry_pegs.append([pegs, pos])
		var lc := _vec2_arg(String(args.get("laundry-cam", "4.6,1.5")))
		var lcp := at - fwd * lc.x + right * 0.4
		c3.global_position = Vector3(lcp.x, height_at(lcp.x, lcp.z) + lc.y, lcp.z)
		c3.look_at(at + Vector3(0, line_h * 0.72, 0))
		_log("laundry (%s) at %s, wind %s, cam %s" % [String(args["laundry"]), str(at), str(lw), str(c3.global_position)])
	if landscape != null:
		landscape.collision_target = landscape.get_path_to(site)
		landscape.update_collision()
	if not laundry.is_empty():
		for i in 2:
			await get_tree().physics_frame
		for k in laundry.size():
			laundry[k].move_pins(laundry_pegs[k][0], laundry_pegs[k][1])
	var steps := int(float(args.get("fire-seconds", "9")) * float(Engine.physics_ticks_per_second))
	var light_f := int(float(args.get("curtain-light-at", "1")) * float(Engine.physics_ticks_per_second))
	for f in steps:
		await get_tree().physics_frame
		if curtain != null and f == light_f:
			curtain.ignite(curtain_light)
			_log("curtain lit at %s" % str(curtain_light))
		if curtain != null and f % 30 == 0:
			_log("curtain: %d burning, %d burnt" % [curtain.get_burning_count(), curtain.get_burnt_count()])
		if fire_tree != null and f == steps - int(Engine.physics_ticks_per_second):
			fire_tree.visible = true
		if f % 30 == 0:
			_log("fire t=%.1f s (%d ms in)" % [f / float(Engine.physics_ticks_per_second), Time.get_ticks_msec()])


# --crop=wheat|barley|oats|rye [--crop-stage=75]: a drilled field (GrassField3D.crop) --crop-size (50x40 m) round the
# ground --crop-ahead (12) m in front of the camera; --crop-close: the camera into its edge at ear height, looking
# 2.5 m down the rows.
# --solo-tree=PRESET: one open-grown TreeGenerator tree --solo-dist (11) m in front of the camera, the camera lowered
# to --solo-eye (1.6) m and turned on it, its crown filling about half the frame height (a species portrait).
func _build_solo_tree() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var tg := TreeGenerator.new()
	tg.preset = int(args["solo-tree"])
	tg.seed = int(args.get("solo-seed", "5"))
	var tp := c3.global_position + fwd * float(args.get("solo-dist", "11"))
	tp.y = height_at(tp.x, tp.z) - 0.05
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = TreeWind3D.make_multimesh(tg.generate(), [Transform3D(Basis(), tp)])
	add_child(mmi)
	var cp := c3.global_position
	cp.y = height_at(cp.x, cp.z) + float(args.get("solo-eye", "1.6"))
	c3.global_position = cp
	c3.look_at(tp + Vector3(0, float(args.get("solo-look-h", "3.5")), 0))
	_log("solo tree preset %s at %s, cam %s" % [String(args["solo-tree"]), str(tp.round()), str(cp.round())])


# --frost-puddle: a farm track (a worn path through the sward) --fire-ahead (4) m in front of the camera with two wheel
# ruts pressed 10 cm into it and a clay puddle (add_puddle) in the near rut; the ground then runs --frost-hours (6) h
# of its own time so the puddle freezes when the air is below 0 C (puddle_ice; use a winter dawn, e.g. --month=0
# --jan=-4 --time=8). The camera sits --frost-cam-h (0.35) m up, --frost-cam-dist (1.3) m from the puddle.
func _frost_puddle() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := fwd.cross(Vector3.UP).normalized()
	var at := c3.global_position + fwd * float(args.get("fire-ahead", "4"))
	at.y = height_at(at.x, at.z)
	prints.follow_target = prints.get_path_to(prints_probe)
	prints_probe.global_position = at
	for i in 4:
		await get_tree().physics_frame
	var track := PackedVector3Array()
	for k in 6:
		var q := at + fwd * lerpf(-14.0, 14.0, k / 5.0) + right * 0.6 * sin(k * 0.9)
		track.append(Vector3(q.x, height_at(q.x, q.z), q.z))
	var wp := [{"points": track, "width": 2.6, "passes": 6000.0}]
	for g in sward:
		g.paths = wp
	if grass != null and not sward.has(grass):
		grass.paths = wp
	var yaw := atan2(fwd.x, fwd.z)
	for sd in [-0.8, 0.8]:
		var rp: Vector3 = at + right * sd
		prints.press_box(Vector3(rp.x, height_at(rp.x, rp.z), rp.z), Vector2(0.17, 9.0), yaw, height_at(rp.x, rp.z), 0.1)
	prints.sync_deformation()
	if args.has("needle-ice"):
		# --needle-ice: no puddle; wet bare soil (--needle-moisture 0.4) between the ruts with a line of boot prints
		# (left/right, 0.75 m stride), a frosty night, then a low close camera beside the prints.
		prints.set("moisture", float(args.get("needle-moisture", "0.4")))
		prints.set("needle_ice", true)
		for k in 9:
			var side := 0.12 if k % 2 == 0 else -0.12
			var bp: Vector3 = at + fwd * (k * 0.75 - 1.5) + right * side
			var bh := height_at(bp.x, bp.z)
			prints.press_box(Vector3(bp.x, bh, bp.z), Vector2(0.11, 0.29), yaw, bh - 0.025, 0.0)
		for i in 30:
			await get_tree().physics_frame
		prints.sync_deformation()
		var nh := float(args.get("frost-hours", "8"))
		prints.settle(nh * 3600.0, 30.0)
		for i in 10:
			await get_tree().physics_frame
		var np := at + fwd * 0.75
		var ncp := np - fwd * float(args.get("frost-cam-dist", "1.3")) + right * 0.55
		c3.global_position = Vector3(ncp.x, height_at(ncp.x, ncp.z) + float(args.get("frost-cam-h", "0.35")), ncp.z)
		c3.look_at(Vector3(np.x, height_at(np.x, np.z), np.z) + fwd * 0.4)
		_log("needle ice at %s: %s (height m, crumbs), moisture %.2f, air %.1f C, %s h settled, cam %s" % [str(np.round()), str(prints.call("get_needle_ice_at", np)), float(prints.get("moisture")), weather.get_air_temperature(), str(nh), str(c3.global_position)])
		return
	prints.standing_water = true
	var pp := at - right * 0.8 + fwd * 0.5
	prints.call("add_puddle", Vector3(pp.x, height_at(pp.x, pp.z), pp.z), float(args.get("frost-radius", "0.75")), 0.09, 20.0)
	for i in 150:
		await get_tree().physics_frame
	prints.sync_deformation()
	var hrs := float(args.get("frost-hours", "6"))
	prints.settle(hrs * 3600.0, 30.0)
	for i in 10:
		await get_tree().physics_frame
	var cp := pp - fwd * float(args.get("frost-cam-dist", "1.3")) - right * 0.35
	c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + float(args.get("frost-cam-h", "0.35")), cp.z)
	c3.look_at(Vector3(pp.x, height_at(pp.x, pp.z), pp.z) + fwd * 0.3)
	_log("frost puddle at %s: water %.1f cm, air %.1f C, %s h settled, cam %s" % [str(pp.round()), prints.get_standing_water_at(pp) * 100.0, weather.get_air_temperature(), str(hrs), str(c3.global_position)])


func _build_crop() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := fwd.cross(Vector3.UP).normalized()
	var mid := c3.global_position + fwd * float(args.get("crop-ahead", "12"))
	var cs := _vec2_arg(String(args.get("crop-size", "50,40")))
	var stage := float(args.get("crop-stage", "75"))
	var poly := PackedVector2Array()
	for k in 4:
		var u := (-1.0 if k == 0 or k == 3 else 1.0) * cs.x * 0.5
		var v := (-1.0 if k < 2 else 1.0) * cs.y * 0.5
		var q := mid + right * u + fwd * v
		poly.append(Vector2(q.x, q.z))
	var crop := GrassField3D.new()
	crop.name = "Crop"
	var names := {"wheat": GrassField3D.CROP_WHEAT, "barley": GrassField3D.CROP_BARLEY, "oats": GrassField3D.CROP_OATS, "rye": GrassField3D.CROP_RYE}
	crop.crop = names.get(String(args["crop"]), GrassField3D.CROP_WHEAT)
	crop.crop_stage = stage
	crop.density = float(args.get("crop-density", "450"))
	var drill := atan2(fwd.x, fwd.z)
	crop.drill_angle = drill
	crop.radius = float(args.get("crop-radius", "30"))
	crop.tile_size = 1.25 # under the path tracer's merged BLAS cap at 400-600 tillers a square metre
	crop.fields = [{"polygon": poly, "drill_angle": drill, "stage": stage, "tramline_spacing": 24.0, "headland": 6.0}]
	if args.has("crop-hedge"):
		# --crop-hedge: a second field across the right-hand edge, two hedge offsets away, so one hedgerow
		# runs between two crops; the camera stands in the first field looking across at it, side sun.
		var gap := 2.0 * float(crop.get("hedge_offset"))
		var poly2 := PackedVector2Array()
		for q2 in poly:
			poly2.append(q2 + Vector2(right.x, right.z) * (cs.x + gap))
		var f2: Array = crop.fields
		f2.append({"polygon": poly2, "drill_angle": drill, "stage": stage, "tramline_spacing": 24.0, "headland": 6.0})
		crop.fields = f2
	add_child(crop)
	crop.data_map = crop.get_path_to(data_map)
	if args.has("crop-close"):
		var eye := 1.1 if String(args["crop"]) in ["wheat", "barley"] else (1.3 if String(args["crop"]) == "oats" else 1.6)
		var cp := mid - fwd * (cs.y * 0.5 - 2.0) + right * 1.0
		c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + eye, cp.z)
		var lp := cp + fwd * 2.5 - right * 0.4
		c3.look_at(Vector3(lp.x, height_at(lp.x, lp.z) + 0.75, lp.z))
	if args.has("crop-hedge"):
		var hb := float(args.get("crop-hedge-back", "16"))
		var edge := mid + right * (cs.x * 0.5 + float(crop.get("hedge_offset")))
		var cp2 := edge - right * hb + fwd * float(args.get("crop-hedge-along", "0"))
		c3.global_position = Vector3(cp2.x, height_at(cp2.x, cp2.z) + float(args.get("crop-hedge-eye", "1.6")), cp2.z)
		var lp2 := cp2 + right * hb
		c3.look_at(Vector3(lp2.x, height_at(lp2.x, lp2.z) + 1.2, lp2.z))
		if String(args["crop-hedge"]) == "along":
			# --crop-hedge=along: on the margin strip between the crop and the hedge, --crop-hedge-eye (2) m up, looking
			# along the hedge (wheat on the left, hedge on the right).
			var ho := float(crop.get("hedge_offset"))
			var ap := mid + right * (cs.x * 0.5 + ho * 0.45) - fwd * (cs.y * 0.5 - float(args.get("crop-hedge-along", "2")))
			c3.global_position = Vector3(ap.x, height_at(ap.x, ap.z) + float(args.get("crop-hedge-eye", "2")), ap.z)
			var al := ap + fwd * 20.0 + right * 0.6
			c3.look_at(Vector3(al.x, height_at(al.x, al.z) + 0.9, al.z))
	_log("crop %s stage %.0f, field %s m round %s" % [String(args["crop"]), stage, str(cs), str(mid.round())])


# --hut: the old wooden hut (art/old_wooden_hut, "Old wooden hut" by LOLIPOP, CC BY 4.0) --hut-ahead (14) m in front of
# the camera, turned --hut-yaw degrees, burning with PhysXBuildingFire3D: one room (the hut's walls), its door and any
# --hut-windows=face,x,y,w,h[;...] openings, the flames drawn in its own PhysXFlow3D (--fire-cell, 0.1 m). Lit inside
# after setup, then --hut-seconds (0: unlit) of fire seconds pass at --hut-time-scale (8) fire seconds a real one. The
# camera stands --hut-cam=dist,height,side (16,1.7,4) m off the door side, on the hut.
func _hut() -> void:
	var b := _gltf_node("res://art/old_wooden_hut/old_wooden_hut.glb")
	if b == null:
		_log("hut: art/old_wooden_hut missing (run fetch_art.sh), skipped")
		return
	var keep := ["Hut_LOD0", "Door_LOD0"]
	for n in b.find_children("*", "Node3D", true, false):
		if n.get_parent() != null and n.get_parent().name == "RootNode" and not (String(n.name) in keep):
			n.queue_free()
	var root := Node3D.new()
	root.name = "Hut"
	add_child(root)
	root.add_child(b)
	await get_tree().process_frame
	var hut_box := AABB()
	var door_box := AABB()
	var first := true
	for mi in b.find_children("*", "MeshInstance3D", true, false):
		var bx: AABB = (root.global_transform.affine_inverse() * mi.global_transform) * (mi as MeshInstance3D).get_aabb()
		if String(mi.name).begins_with("Door"):
			door_box = bx if door_box.size == Vector3.ZERO else door_box.merge(bx)
		elif String(mi.name).begins_with("Hut"):
			hut_box = bx if first else hut_box.merge(bx)
			first = false
	# The door stands open (the fire model has no closed door: its flames leave through the doorway).
	for n in b.find_children("Door_LOD0", "Node3D", true, false):
		n.queue_free()
	var c3: Camera3D = get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var at := c3.global_position + fwd * float(args.get("hut-ahead", "14"))
	root.rotation.y = deg_to_rad(float(args.get("hut-yaw", "0")))
	# Set on its lowest corner, so no corner floats.
	var low := INF
	for cx in [hut_box.position.x, hut_box.end.x]:
		for cz in [hut_box.position.z, hut_box.end.z]:
			var w := at + root.basis * Vector3(cx - hut_box.get_center().x, 0, cz - hut_box.get_center().z)
			low = minf(low, height_at(w.x, w.z))
	root.global_position = Vector3(at.x, low - hut_box.position.y - 0.05, at.z) - root.basis * Vector3(hut_box.get_center().x, 0, hut_box.get_center().z)
	_log("hut: box %s, door %s, at %s" % [str(hut_box), str(door_box), str(root.global_position.round())])
	if args.get("hut-props", "1") != "0":
		# Firewood stacked along the two side walls (Tripo props), 0.3 m off the logs.
		for sd in [-1.0, 1.0]:
			var wp := _tripo("firewood_pile" if sd < 0.0 else "firewood_pile_b", 2.0 if sd < 0.0 else 2.4, 2)
			if wp == null:
				break
			var lx: float = (hut_box.position.x - 0.55) if sd < 0.0 else (hut_box.end.x + 0.65)
			var lz: float = hut_box.get_center().z + 0.25 * hut_box.size.z * sd
			var wpos: Vector3 = root.global_transform * Vector3(lx, 0, lz)
			_put(wp, wpos, root.rotation.y)
	# Its walls as one collider, for firebrands.
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = BoxShape3D.new()
	(cs.shape as BoxShape3D).size = hut_box.size
	cs.position = hut_box.get_center()
	body.add_child(cs)
	root.add_child(body)
	if not ClassDB.class_exists("PhysXBuildingFire3D"):
		_log("hut: this build has no PhysXBuildingFire3D")
		return
	var room := AABB(hut_box.position + Vector3(0.15, 0.1, 0.15), hut_box.size - Vector3(0.3, hut_box.size.y * 0.35, 0.3))
	var openings := []
	if door_box.size != Vector3.ZERO:
		# The door's face: the wall its centre is nearest.
		var dc := door_box.get_center()
		var d := [absf(dc.x - room.position.x), absf(room.end.x - dc.x), absf(dc.z - room.position.z), absf(room.end.z - dc.z)]
		var face := d.find(d.min())
		var along := (door_box.position.z - room.position.z) if face < 2 else (door_box.position.x - room.position.x)
		var wide := door_box.size.z if face < 2 else door_box.size.x
		openings.append({"face": face, "rect": Rect2(maxf(along, 0.0), maxf(door_box.position.y - room.position.y, 0.0), wide, door_box.size.y), "glazed": false})
	if args.has("hut-windows"):
		for wdesc in String(args["hut-windows"]).split(";"):
			var q := wdesc.split(",")
			openings.append({"face": int(q[0]), "rect": Rect2(float(q[1]), float(q[2]), float(q[3]), float(q[4])), "glazed": false})
	var flow = ClassDB.instantiate("PhysXFlow3D")
	flow.name = "HutFlow"
	flow.cell_size = float(args.get("fire-cell", "0.2"))
	flow.combustion = true
	if "terrain_path" in flow:
		flow.terrain_floor = true
	add_child(flow)
	if "terrain_path" in flow:
		flow.terrain_path = flow.get_path_to(gen)
	var bf = ClassDB.instantiate("PhysXBuildingFire3D")
	bf.name = "BuildingFire"
	root.add_child(bf)
	bf.set("rooms", [{"box": room, "fuel_load": float(args.get("hut-fuel", "780")), "openings": openings}])
	bf.set("flow", bf.get_path_to(flow))
	bf.set("time_scale", float(args.get("hut-time-scale", "8")))
	_log("hut fire: room %s, openings %s" % [str(room), str(openings)])
	var cp := String(args.get("hut-cam", "16,1.7,4")).split(",")
	var hc := root.global_transform * hut_box.get_center()
	var dir := (c3.global_position - hc)
	dir.y = 0.0
	dir = dir.normalized()
	var side := dir.cross(Vector3.UP)
	var cam_at := hc + dir * float(cp[0]) + side * float(cp[2])
	c3.global_position = Vector3(cam_at.x, height_at(cam_at.x, cam_at.z) + float(cp[1]), cam_at.z)
	c3.look_at(hc + Vector3(0, float(args.get("hut-look-h", "0.5")), 0))
	var secs := float(args.get("hut-seconds", "0"))
	if secs > 0.0:
		bf.call("ignite", hc)
		var steps := int(secs / float(bf.get("time_scale")) * Engine.physics_ticks_per_second)
		# Flow steps only while frames draw, in real time (time_scale speeds the room model only): the model runs fast
		# with drawing off, then --hut-draw-tail (8) s drawn at time_scale 1 so the flames fill in and the smoke stands up.
		var tail := int(float(args.get("hut-draw-tail", "8")) * Engine.physics_ticks_per_second)
		if args.get("sim-render", "0") == "0":
			RenderingServer.render_loop_enabled = false
		for i in steps:
			await get_tree().physics_frame
			if i % (Engine.physics_ticks_per_second * 2) == 0:
				_log("hut fire t %.0f s: state %d, %.0f C, %.0f kW, fuel %.2f, collapsed %s" % [i * float(bf.get("time_scale")) / Engine.physics_ticks_per_second, int(bf.call("get_room_state", 0)), float(bf.call("get_room_temperature", 0)), float(bf.call("get_heat_release")), float(bf.call("get_room_fuel_left", 0)), str(bf.call("is_room_collapsed", 0))])
		RenderingServer.render_loop_enabled = true
		bf.set("time_scale", 1.0)
		var mats := []
		for mi in b.find_children("*", "MeshInstance3D", true, false):
			var m: Material = (mi as MeshInstance3D).get_active_material(0)
			mats.append("%s:%s" % [mi.name, m.get_class() if m != null else "none"])
		_log("hut materials %s" % str(mats))
		for i in tail:
			await get_tree().physics_frame
			if i % (Engine.physics_ticks_per_second * 5) == 0:
				_log("hut fire t %.0f s: state %d, %.0f C, %.0f kW, fuel %.2f, collapsed %s" % [i * float(bf.get("time_scale")) / Engine.physics_ticks_per_second, int(bf.call("get_room_state", 0)), float(bf.call("get_room_temperature", 0)), float(bf.call("get_heat_release")), float(bf.call("get_room_fuel_left", 0)), str(bf.call("is_room_collapsed", 0))])
		_log("hut fire at %.0f s: state %d, %.0f kW, flashover %.0f s, char %.1f mm, collapsed %s, firebrands %d" % [secs, int(bf.call("get_room_state", 0)), float(bf.call("get_heat_release")), float(bf.call("get_room_flashover_time", 0)), float(bf.call("get_room_char_depth", 0)) * 1000.0, str(bf.call("is_room_collapsed", 0)), int(bf.call("get_firebrand_count"))])


func _rooting_tyre() -> void:
	# --rooting-tyre (with --stand-trail, prints on): a car's two tyres (0.8 m, 0.28 m wide, 1.6 m apart) driven
	# across the boar rooting square to the view, one through its middle, pressed into the ground's print field
	# (GroundSurface3D.press_wheel) so the clods they run over are crushed (ground detail 6024a5a0) and a clean rut
	# runs through the patch. --tyre-sink (0.04) m.
	var tc: Camera3D = get_viewport().get_camera_3d()
	var tf := -tc.global_basis.z
	tf.y = 0.0
	tf = tf.normalized().rotated(Vector3.UP, deg_to_rad(float(args.get("trail-angle", "12"))))
	var ts := tf.cross(Vector3.UP).normalized()
	var rp := tc.global_position + tf * float(args.get("rooting-ahead", "7")) - ts * 2.0
	prints.follow_target = prints.get_path_to(tc)
	for i in 30:
		await get_tree().physics_frame
	var dir := Vector2(ts.x, ts.z)
	var side := Vector2(-dir.y, dir.x)
	var yaw := atan2(dir.x, dir.y)
	var a := Vector2(rp.x, rp.z) - dir * 5.0
	var sink := float(args.get("tyre-sink", "0.04"))
	for i in 101:
		var p := a + dir * (0.1 * i)
		for k in 2:
			var q := p + side * (k * 1.6)
			var hq := height_at(q.x, q.y)
			prints.press_wheel(Vector3(q.x, hq, q.y), Vector2(0.8, 0.28), yaw, sink, dir * 3.0)
		await get_tree().physics_frame
	for i in 30:
		await get_tree().physics_frame
	_log("rooting tyre: 10 m across the rooting at %s, sink %.2f m" % [str(rp.round()), sink])


func _stand_trail() -> void:
	# --stand-trail[=passes]: a game trail (WorldStand.set_trails, 0.8 m, 600 passes a year) from 2 m ahead of the
	# camera away from it, bending a little, and a fresh boar rooting (--rooting-age, 0) 1.5 m across, --rooting-ahead
	# (7) m out and 2 m to the side (terrain 3199eeb8/897cc1dd, rocks 7b898b93, clods ad9de493). Off for the before; --trail-rooting=0 skips the rooting.
	var tc: Camera3D = get_viewport().get_camera_3d()
	var tf := -tc.global_basis.z
	tf.y = 0.0
	tf = tf.normalized().rotated(Vector3.UP, deg_to_rad(float(args.get("trail-angle", "12"))))
	var ts := tf.cross(Vector3.UP).normalized()
	var tpts := PackedVector2Array()
	for i in 40:
		var d := 2.0 + float(i) * 2.0
		var q := tc.global_position + tf * d + ts * (0.9 * sin(d * 0.12))
		tpts.append(Vector2(q.x, q.z))
	var tpass := float(args["stand-trail"]) if String(args["stand-trail"]) != "" else 600.0
	WorldContracts.get_stand().set_trails([{"points": tpts, "width": 0.8, "passes": tpass, "kind": "game_trail"}])
	var rp := tc.global_position + tf * float(args.get("rooting-ahead", "7")) - ts * 2.0
	rp.y = height_at(rp.x, rp.z)
	if args.get("trail-rooting", "1") != "0":
		WorldContracts.get_stand().add_disturbance(rp, 1.5, WorldStand.DISTURBANCE_ROOTING, float(args.get("rooting-age", "0")))
	_log("stand trail: %d points from %s, rooting at %s; trail %s at 6 m" % [tpts.size(), str(tpts[0].round()), str(rp.round()), str(WorldContracts.get_stand().trail_at(Vector3(tpts[2].x, 0, tpts[2].y)))])


func _hut_rime() -> void:
	# --hut-rime[=H]: H (24) hours of freezing fog (--rime-c -6 C, --rime-wind 8 m/s, --rime-fog 0.25 g/m3) on the hut
	# (SnowRime3D, snow d0a9db71/fbfd5e10), the wind blowing from the camera onto it so the feathers grow toward us.
	var root := get_node_or_null("Hut") as Node3D
	if root == null or not ClassDB.class_exists("SnowRime3D"):
		_log("hut rime: no hut or no SnowRime3D")
		return
	var c3: Camera3D = get_viewport().get_camera_3d()
	var wv := root.global_position - c3.global_position
	wv.y = 0.0
	wv = wv.normalized() * float(args.get("rime-wind", "8"))
	var rime: Node3D = ClassDB.instantiate("SnowRime3D")
	rime.name = "Rime"
	rime.set("target_group", "")
	add_child(rime)
	rime.set("target_paths", [rime.get_path_to(root)])
	rime.call("refresh_targets")
	var hrs := float(args["hut-rime"]) if String(args["hut-rime"]) != "" else 24.0
	for h in int(hrs):
		rime.call("grow", 1.0, float(args.get("rime-c", "-6")), wv, float(args.get("rime-fog", "0.25")))
	_log("hut rime: %d targets, %d feathers after %.0f h, wind %s" % [int(rime.call("get_target_count")), int(rime.call("get_feather_count")), hrs, str(wv)])


func _hut_roof_snow() -> void:
	# --hut-roof-snow[=H]: H (24) hours of snowfall (--roof-rate 4 mm/h at --roof-temp -6 C) on the hut, its two roof
	# planes tracked as roof faces (group snow_roof, SnowBodies3D, snow c0c29576) with --roof-material (shingle) and
	# --roof-heat (20 W/m2, a lived-in hut): the load slides off and lands as a bank past the eave (roof_piles).
	# The faces are found from the roof mesh (two planes from the level eaves up to the ridge) and carry an
	# invisible box each for their size.
	var root := get_node_or_null("Hut") as Node3D
	var sb = snow.call("get_bodies") if snow.has_method("get_bodies") else null
	if root == null or sb == null:
		_log("hut roof snow: no hut or no snow bodies")
		return
	var inv := root.global_transform.affine_inverse()
	var pts := PackedVector3Array()
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var m := mi as MeshInstance3D
		if m.mesh == null or not String(m.name).begins_with("Hut"):
			continue
		var xf := inv * m.global_transform
		for v in m.mesh.get_faces():
			pts.append(xf * v)
	if pts.is_empty():
		_log("hut roof snow: no hut mesh")
		return
	var lo := pts[0]
	var hi := pts[0]
	for v in pts:
		lo = lo.min(v)
		hi = hi.max(v)
	# The eaves run along the longer horizontal side; the ridge is the top, over the middle.
	var axis := 0 if (hi.x - lo.x) < (hi.z - lo.z) else 2
	var other := 2 if axis == 0 else 0
	var eave_y := INF
	for v in pts:
		if absf(v[axis] - lo[axis]) < 0.08:
			eave_y = minf(eave_y, v.y)
	var mid: float = 0.5 * (lo[axis] + hi[axis])
	var run: float = 0.5 * (hi[axis] - lo[axis])
	var rise: float = hi.y - eave_y
	var pitch := atan2(rise, run)
	var faces := []
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = Color(1, 1, 1, 0)
	for sgn in [-1.0, 1.0]:
		var f := Node3D.new()
		f.name = "RoofFace%d" % (0 if sgn < 0 else 1)
		f.add_to_group("snow_roof")
		f.set_meta("snow_roof_material", String(args.get("roof-material", "shingle")))
		f.set_meta("snow_roof_heat", float(args.get("roof-heat", "20")))
		var box := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(sqrt(run * run + rise * rise), 0.02, hi[other] - lo[other])
		bm.material = mat
		box.mesh = bm
		box.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		f.add_child(box)
		root.add_child(f)
		var c := Vector3()
		c[axis] = mid + sgn * 0.5 * run
		c[other] = 0.5 * (lo[other] + hi[other])
		c.y = eave_y + 0.5 * rise
		# Local x runs down the slope to the eave on this side.
		var down := Vector3()
		down[axis] = sgn
		var xdir := (down * cos(pitch) - Vector3.UP * sin(pitch)).normalized()
		var zdir := Vector3()
		zdir[other] = 1.0
		var ydir := zdir.cross(xdir).normalized()
		f.transform = Transform3D(Basis(xdir, ydir, zdir), c)
		faces.append(f)
	for i in 2:
		await get_tree().physics_frame
	weather.wind_speed = 1.0
	weather.rain_rate = float(args.get("roof-rate", "4"))
	weather.cloud_cover = 1.0
	weather.air_follows_climate = false
	weather.temperature = float(args.get("roof-temp", "-6"))
	weather.snap_to_settings()
	weather.advance(1.0)
	var hrs := int(float(args["hut-roof-snow"]) if String(args["hut-roof-snow"]) != "" else 24.0)
	var slide_shot := String(args.get("roof-slide", ""))
	if slide_shot != "":
		sb.set("slide_enabled", false) # the load builds first; it is let go for the shot
	var most := 0.0
	for h in hrs:
		snow.advance(3600.0)
		for k in 6:
			sb.call("step", 600.0)
		for f in faces:
			most = maxf(most, float(sb.call("get_body_depth", f)))
	weather.rain_rate = 0.0
	weather.cloud_cover = float(args.get("cloud-cover", "0.3"))
	weather.snap_to_settings()
	weather.advance(1.0)
	_log("hut roof snow: pitch %.0f deg, %d h of snow, roofs held up to %.2f m, %d slides, %.0f kg landed past the eave" % [rad_to_deg(pitch), hrs, most, int(sb.call("get_roof_slides")), float(sb.call("get_roof_landed"))])
	if slide_shot != "":
		await _roof_slide_shot(sb, root, axis, other, lo, hi, mid, run, eave_y, slide_shot)


# --roof-slide=mid|settled (with --hut-roof-snow): one camera at eye height off the gable end, --slide-out (4) m out
# from the +side eave and --slide-back (9) m past the end, looking along the eave. The load is held while it builds,
# then the air warms to --slide-air (2) C and the roof sheds (SnowBodies3D roof_slab_bodies, snow c820c61e). mid: the
# picture is frozen --slide-frames (14) physics frames after the first slab leaves; settled: --slide-wait (120) s on.
func _roof_slide_shot(sb: Object, root: Node3D, axis: int, other: int, lo: Vector3, hi: Vector3, mid: float, run: float, eave_y: float, kind: String) -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var e := Vector3()
	e[axis] = mid + run + 0.6
	e[other] = 0.5 * (lo[other] + hi[other])
	e.y = eave_y - 0.8
	var cl := Vector3()
	cl[axis] = mid + run + float(args.get("slide-out", "4"))
	cl[other] = hi[other] + float(args.get("slide-back", "9"))
	var cw: Vector3 = root.global_transform * cl
	var ew: Vector3 = root.global_transform * e
	c3.global_position = Vector3(cw.x, height_at(cw.x, cw.z) + 1.7, cw.z)
	c3.look_at(ew)
	weather.air_follows_climate = false
	weather.temperature = float(args.get("slide-air", "2"))
	weather.snap_to_settings()
	sb.set("slide_enabled", true)
	var n0 := int(sb.call("get_slabs_slid"))
	var t := 0
	while int(sb.call("get_slabs_slid")) == n0 and t < 2000:
		sb.call("step", 30.0)
		await get_tree().physics_frame
		t += 1
	_log("roof slide: first slab after %d steps of 30 s, %d slid" % [t, int(sb.call("get_slabs_slid"))])
	if kind == "settled":
		get_viewport().disable_3d = true
		for i in int(float(args.get("slide-wait", "120")) * Engine.physics_ticks_per_second):
			await get_tree().physics_frame
		get_viewport().disable_3d = false
		_log("roof slide: settled after %s s, %.0f kg landed" % [args.get("slide-wait", "120"), float(sb.call("get_roof_landed"))])
	else:
		for i in int(args.get("slide-frames", "14")):
			await get_tree().physics_frame
		Engine.time_scale = 0.0
		_log("roof slide: frozen mid-slide")


func _hut_icicles() -> void:
	# --hut-icicles (with --hut, winter): snow on the hut's roof (its materials take the weather's snow) and a week of
	# icicles (SnowIcicles3D, snow 5f68879a) along its longest eave, the lip found from the roof mesh; the camera
	# --ice-dist (1.55) m out from the eave, a little above the icicles' middle, on the longest fused pair, the hut's
	# dark wall behind. --ice-days (7), --ice-fov (26), --ice-up (0.4 m over the icicles' middle), --ice-look-up (0), --ice-snow (40 mm of water on the roof at least).
	var root := get_node_or_null("Hut") as Node3D
	if root == null or not ClassDB.class_exists("SnowIcicles3D"):
		_log("hut icicles: no hut or no SnowIcicles3D")
		return
	var inv := root.global_transform.affine_inverse()
	var pts := PackedVector3Array()
	var snowed := 0
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var m := mi as MeshInstance3D
		if m.mesh == null or not String(m.name).begins_with("Hut"):
			continue
		for si in m.mesh.get_surface_count():
			var mat := m.get_active_material(si)
			if mat is BaseMaterial3D and "snow_enabled" in mat:
				var mm := (mat as BaseMaterial3D).duplicate() as BaseMaterial3D
				mm.set("snow_enabled", true)
				m.set_surface_override_material(si, mm)
				snowed += 1
		var xf := inv * m.global_transform
		for v in m.mesh.get_faces():
			pts.append(xf * v)
	if pts.is_empty():
		_log("hut icicles: no hut mesh")
		return
	var lo := pts[0]
	var hi := pts[0]
	for v in pts:
		lo = lo.min(v)
		hi = hi.max(v)
	# The eave: the side whose outermost vertices run level over a long length (the gable ends rise to the ridge).
	var best_side := -1
	var best_len := 0.0
	var eave_y := 0.0
	var along := Vector2()
	for side in 4:
		var ax := 0 if side < 2 else 2
		var other := 2 if ax == 0 else 0
		var ext: float = (lo if side % 2 == 0 else hi)[ax]
		var ys := PackedFloat32Array()
		var a0 := INF
		var a1 := -INF
		for v in pts:
			if absf(v[ax] - ext) < 0.08:
				ys.append(v.y)
				a0 = minf(a0, v[other])
				a1 = maxf(a1, v[other])
		if ys.size() < 4:
			continue
		ys.sort()
		var spread := ys[ys.size() - 1] - ys[0]
		if spread < 0.4 and a1 - a0 > best_len:
			best_side = side
			best_len = a1 - a0
			eave_y = ys[0]
			along = Vector2(a0, a1)
	if best_side < 0:
		_log("hut icicles: no level eave found (box %s to %s)" % [str(lo), str(hi)])
		return
	var axis := 0 if best_side < 2 else 2
	var out_sign := -1.0 if best_side % 2 == 0 else 1.0
	var lip: float = (lo if best_side % 2 == 0 else hi)[axis]
	var inward := lip - out_sign * 0.1
	var e0 := Vector3()
	var e1 := Vector3()
	e0[axis] = inward
	e1[axis] = inward
	e0[2 if axis == 0 else 0] = along.x + 0.3
	e1[2 if axis == 0 else 0] = along.y - 0.3
	e0.y = eave_y - 0.01
	e1.y = eave_y - 0.01
	var ice: Node3D = ClassDB.instantiate("SnowIcicles3D")
	ice.name = "Icicles"
	ice.set("spacing", 0.05)
	root.add_child(ice)
	ice.set("edge", PackedVector3Array([e0, e1]))
	var catch := 3.4
	for day in int(args.get("ice-days", "7")):
		ice.call("grow", 6.0, -4.0, 0.5, 0.3 * catch)
		ice.call("grow", 18.0, -8.0, 0.5, 0.0)
	ice.call("grow", 0.25, -4.0, 0.5, 0.3 * catch)
	var n: int = ice.call("get_icicle_count")
	var bk := -1
	var bl := 0.0
	for k in n - 1:
		var pa: Vector3 = ice.call("get_icicle_position", k)
		var pb: Vector3 = ice.call("get_icicle_position", k + 1)
		var ra: float = ice.call("get_icicle_radius", k)
		var rb: float = ice.call("get_icicle_radius", k + 1)
		var l := minf(float(ice.call("get_icicle_length", k)), float(ice.call("get_icicle_length", k + 1)))
		var gap := Vector2(pa.x - pb.x, pa.z - pb.z).length() - ra - rb
		var t := float(k) / maxf(n - 1, 1)
		if gap > 0.0 and gap < minf(ra, rb) and t > 0.25 and t < 0.75 and l > bl:
			bk = k
			bl = l
	var mid_l := (e0 + e1) * 0.5
	mid_l.y -= 0.25
	if bk >= 0:
		mid_l = (Vector3(ice.call("get_icicle_position", bk)) + Vector3(ice.call("get_icicle_position", bk + 1))) * 0.5
		mid_l.y -= 0.5 * maxf(float(ice.call("get_icicle_length", bk)), float(ice.call("get_icicle_length", bk + 1)))
	var outward := Vector3()
	outward[axis] = out_sign
	var tang := (e1 - e0).normalized()
	var mid := root.global_transform * mid_l
	var c3: Camera3D = get_viewport().get_camera_3d()
	var wout := (root.global_basis * outward).normalized()
	var wt := (root.global_basis * tang).normalized()
	c3.global_position = mid + wout * float(args.get("ice-dist", "1.55")) + Vector3.UP * float(args.get("ice-up", "0.4")) - wt * 0.4
	c3.look_at(mid + Vector3.UP * float(args.get("ice-look-up", "0")))
	c3.fov = float(args.get("ice-fov", "26"))
	var swe0: float = snow.get_world_swe_at(mid) if snow != null and snow.has_method("get_world_swe_at") else -1.0
	var top_up := float(args.get("ice-snow", "40"))
	if snow != null and swe0 >= 0.0 and swe0 < top_up:
		# --ice-snow=MM (40): a winter's snow on the roof and round the hut where the climate left less (low ground).
		snow.add_snow(top_up - swe0, 120.0)
		for i in 4:
			await get_tree().process_frame
	_log("hut icicles: %d hut surfaces take the snow, snow %.0f mm water at the hut (was %.0f)" % [snowed, snow.get_world_swe_at(mid) if snow != null and snow.has_method("get_world_swe_at") else -1.0, swe0])
	_log("hut icicles: eave side %d, %.1f m long at %.2f m, %d sites, longest %.2f m, %.2f kg; hero pair %d (%.2f m) at %s; cam %s" % [best_side, best_len, eave_y, n, float(ice.call("get_longest")), float(ice.call("get_ice_mass")), bk, bl, str(mid), str(c3.global_position)])


func _mem_log(tag: String) -> void:
	# What the renderer holds (lavapipe's GPU memory is system RAM) next to the process's own heap.
	# Resident set from /proc (Linux): lavapipe, PhysX and driver heaps that the counters above do not see.
	var rss := 0
	var st := FileAccess.open("/proc/self/status", FileAccess.READ)
	while st != null and not st.eof_reached(): # a /proc file reports no length: read it line by line
		var ln := st.get_line()
		if ln.begins_with("VmRSS:"):
			rss = int(ln.substr(6).strip_edges().split(" ")[0]) >> 10
			break
	_log("memory %s: resident %d MB, textures %d MB, buffers %d MB, video total %d MB, static %d MB" % [tag, rss,
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED) >> 20,
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED) >> 20,
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED) >> 20,
			OS.get_static_memory_usage() >> 20])


func _snow_strata() -> void:
	# --snow-strata: a dug trench in a real pack (snow strata walls, a3d61431): the highest gentle snow at least
	# --strata-min (0.8) m deep, its season's pack topped by a week's history (a crust on a storm's snow, buried
	# surface hoar, a gale's wind slab and a fresh fall), then a trench --strata-w (1.2) m wide pressed to the ground,
	# running along the sun's bearing so the low light rakes the far wall. The camera stands --strata-dist (1.5) m from
	# that wall at --strata-eye (1.7) m over the snow, looking down into it.
	var wr: Rect2 = gen.get_world_rect()
	var best := Vector3.INF
	var mind := float(args.get("strata-min", "0.8"))
	var xx := wr.position.x + 200.0
	while xx < wr.end.x - 200.0:
		var zz := wr.position.y + 200.0
		while zz < wr.end.y - 200.0:
			var p := Vector3(xx, 0, zz)
			var h := gen.get_height(p)
			if snow.get_depth_at(p) >= mind and rad_to_deg(atan(_slip_grad(p).length())) < 8.0 and (best == Vector3.INF or h > best.y):
				best = Vector3(xx, h, zz)
			zz += 32.0
		xx += 32.0
	if best == Vector3.INF:
		_log("snow strata: no gentle snow %.1f m deep" % mind)
		return
	var focus := Node3D.new()
	focus.name = "StrataFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	weather.follow_target = weather.get_path_to(focus)
	for i in 30:
		await get_tree().physics_frame
	weather.air_follows_climate = false
	weather.temperature = 0.0
	weather.snap_to_settings()
	weather.temperature = -8.0 - (weather.get_air_temperature_at(best) - 0.0)
	weather.wind_speed = 1.0
	weather.snap_to_settings()
	weather.advance(60.0)
	snow.call("add_depth_hoar", 0.6)
	snow.advance(3600.0)
	snow.call("add_snow", 20.0, 120.0) # a storm's snow
	snow.call("add_crust", 0.02, 3.0) # its top melts and refreezes
	snow.advance(3600.0)
	snow.call("add_hoar", 8.0) # a clear night's surface hoar
	snow.call("add_snow", 16.0, 110.0) # buried by the next storm
	weather.wind_speed = 14.0 # a gale packs the top
	weather.snap_to_settings()
	weather.advance(60.0)
	snow.advance(3.0 * 3600.0)
	weather.wind_speed = 1.0
	weather.snap_to_settings()
	weather.advance(60.0)
	snow.call("add_snow", 10.0, 80.0) # a last fresh fall
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var nrm := Vector3(sf.z, 0, -sf.x)
	var c3: Camera3D = get_viewport().get_camera_3d()
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(focus))
	surf.set("detail_spacing", 0.03125)
	for i in 10:
		await get_tree().physics_frame
	var g: float = snow.get_ground_height_at(best) if snow.has_method("get_ground_height_at") else best.y
	var w := float(args.get("strata-w", "1.2"))
	surf.set("rim_share", 0.0) # dug: the snow is thrown out, no pressed rim
	surf.set("compacted_density", 900.0)
	surf.call("press_box", Vector3(best.x, g, best.z), Vector2(w, 8.0), atan2(sf.x, sf.z), g + 0.01)
	for i in 30:
		await get_tree().physics_frame
	var top := g + snow.get_depth_at(best + nrm * 3.0)
	var wall := best - nrm * (w * 0.5)
	var cp := wall + nrm * float(args.get("strata-dist", "1.5"))
	c3.global_position = Vector3(cp.x, top + float(args.get("strata-eye", "1.7")), cp.z)
	c3.look_at(Vector3(wall.x, g + 0.5 * (top - g), wall.z) + sf * 0.8)
	_log("snow strata: %.2f m of snow at %s (%.0f m), trench %.1f m wide along the sun, cam %s" % [top - g, str(best.round()), best.y, w, str(c3.global_position.snapped(Vector3.ONE * 0.1))])


# Tripo's lobster pots come with their netting as sparse wire strands: each of the three stacked pots gets an alpha-cut
# net of tarred twine (4 cm diamond mesh) over its hoops and across its two ends, an elliptic half-cylinder in the raw
# model's units (glTF: x along the pot, y up, z across), sized from the raw's cross-sections.
func _lobster_net(n: Node3D) -> void:
	var img := Image.create(128, 128, true, Image.FORMAT_RGBA8)
	img.fill(Color(0, 0, 0, 0))
	for y in 128:
		for x in 128:
			# two families of diagonal strands, 3 texels thick, with a knot where they cross
			var a := posmod(x + y, 64)
			var b := posmod(x - y, 64)
			if a < 3 or b < 3:
				var shade := 0.07 + 0.03 * sin(float(x * 7 + y * 3))
				img.set_pixel(x, y, Color(shade, shade * 0.9, shade * 0.75, 1.0))
	img.generate_mipmaps()
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = ImageTexture.create_from_image(img)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat.alpha_scissor_threshold = 0.5
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.roughness = 0.85
	mat.uv1_scale = Vector3(1.0, 1.0, 1.0)
	# per pot: base height, arch height, half width, half length (raw units, the stack about 0.98 tall)
	var pots := [Vector4(0.06, 0.23, 0.28, 0.41), Vector4(0.38, 0.2, 0.26, 0.4), Vector4(0.69, 0.27, 0.26, 0.36)]
	var mesh_cm := 0.04 / 0.98 * 1.3 # one texture tile spans two meshes of 4 cm on a 1.3 m stack
	for pot in pots:
		var st := SurfaceTool.new()
		st.begin(Mesh.PRIMITIVE_TRIANGLES)
		var seg := 24
		var pts := []
		for i in seg + 1:
			var t := PI * float(i) / seg
			pts.append(Vector2(pot.z * cos(t), pot.x + pot.y * sin(t)))
		var arcs := [0.0]
		for i in range(1, pts.size()):
			arcs.append(arcs[-1] + (pts[i] as Vector2).distance_to(pts[i - 1]))
		for i in seg:
			for c in [[i, -1.0], [i + 1, -1.0], [i + 1, 1.0], [i, -1.0], [i + 1, 1.0], [i, 1.0]]:
				var q: Vector2 = pts[c[0]]
				st.set_uv(Vector2(c[1] * pot.w, arcs[c[0]]) / (mesh_cm * 2.0))
				st.add_vertex(Vector3(c[1] * pot.w, q.y, q.x))
		# the two ends: a fan across each half ellipse
		for e in [-1.0, 1.0]:
			for i in seg:
				for q in [Vector2(0.0, pot.x), pts[i], pts[i + 1]]:
					st.set_uv(Vector2(q.x, q.y) / (mesh_cm * 2.0))
					st.add_vertex(Vector3(e * pot.w, q.y, q.x))
		st.generate_normals()
		var mi := MeshInstance3D.new()
		mi.name = "LobsterNet"
		mi.mesh = st.commit()
		mi.material_override = mat
		n.add_child(mi)


func _frost_sorted_cam() -> void:
	# --frost-sorted: frost-sorted patterned ground (RockField3D frost_sorting, 94c5a825/1a8aff72). On a 16 m grid, the
	# flat (under 2 degrees) ground whose sorting gate is strong all round it (eight points 15 m out over 0.6), with
	# 2-5 degree ground --fs-up (25) m upslope where the borders draw out into stripes. The camera stands --fs-back
	# (7) m downslope of it at --fs-eye (1.7) m, looking up over the circles to the stripes; --fs-yaw turns it.
	if rocks == null or not rocks.has_method("get_frost_at"):
		_log("frost sorted: no RockField3D frost map")
		return
	var wr: Rect2 = gen.get_world_rect()
	var best := -1.0
	var bp := Vector3.ZERO
	var bu := Vector3.ZERO
	var upd := float(args.get("fs-up", "25"))
	var xx := wr.position.x + 100.0
	while xx < wr.end.x - 100.0:
		var zz := wr.position.y + 100.0
		while zz < wr.end.y - 100.0:
			var p := Vector3(xx, 0, zz)
			var g0: Vector2 = rocks.call("get_frost_at", p)
			if g0.x > 0.6 and rad_to_deg(atan(_slip_grad(p).length())) < 2.0:
				var ok := true
				var lo := 1.0
				for k in 8:
					var q := p + Vector3(cos(TAU * k / 8.0), 0, sin(TAU * k / 8.0)) * 15.0
					var gq: Vector2 = rocks.call("get_frost_at", q)
					lo = minf(lo, gq.x)
				if lo > 0.6:
					# upslope: the steepest rise within 40 m
					var up := Vector3.ZERO
					var rise := -INF
					for k in 16:
						var d := Vector3(cos(TAU * k / 16.0), 0, sin(TAU * k / 16.0))
						var r := height_at(p.x + d.x * 40.0, p.z + d.z * 40.0) - height_at(p.x, p.z)
						if r > rise:
							rise = r
							up = d
					var sp := p + up * upd
					var sdeg := rad_to_deg(atan(_slip_grad(sp).length()))
					var gs: Vector2 = rocks.call("get_frost_at", sp)
					# the pattern should run on 40-60 m ahead: the gate along the view
					var far := minf((rocks.call("get_frost_at", p + up * 40.0) as Vector2).x, (rocks.call("get_frost_at", p + up * 55.0) as Vector2).x)
					var score := lo + far + (0.5 if sdeg > 2.0 and sdeg < 5.0 and gs.x > 0.6 else 0.0)
					if score > best:
						best = score
						bp = Vector3(p.x, height_at(p.x, p.z), p.z)
						bu = up
			zz += 16.0
		xx += 16.0
	if best < 0.0:
		_log("frost sorted: no sorted ground found")
		return
	var c3: Camera3D = get_viewport().get_camera_3d()
	var dir := bu.rotated(Vector3.UP, deg_to_rad(float(args.get("fs-yaw", "0"))))
	var cp := bp - dir * float(args.get("fs-back", "4"))
	c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + float(args.get("fs-eye", "1.6")), cp.z)
	if args.has("fs-pitch") or not args.has("fs-look-target"):
		# docs framing: --fs-pitch (20) degrees down, the near circles filling the lower half
		var pr := deg_to_rad(float(args.get("fs-pitch", "20")))
		c3.look_at(c3.global_position + dir * cos(pr) - Vector3.UP * sin(pr))
	else:
		var tgt := bp + dir * upd
		c3.look_at(Vector3(tgt.x, height_at(tgt.x, tgt.z), tgt.z))
	weather.follow_target = weather.get_path_to(c3)
	_log("frost sorted: flat at %s (score %.2f), upslope %s, cam %s" % [str(bp.round()), best, str(dir.snapped(Vector3.ONE * 0.01)), str(c3.global_position.round())])


# A rigged walker (ext/mannequin: GDQuest's mannequin, CC BY 4.0) on a CharacterBody3D, its feet planted on the
# world's ground and snow by FootPlacementModifier3D (character motion 47595e0b), which publishes each plant as a
# footfall the snow, mud and sand print (meta world_contact_feet); GazeModifier3D turns its head and eyes to `look`.
# The rig has no walk: walk_cycle.gd builds one from published gait data (--walker-anim, walk; run plays the rig's
# run at --walker-anim-speed, 0.5). Moved by _walk_to.
# --hay[=x,z]: a meadow mid-harvest (GrassField3D.hay_meadows, vegetation da9145ba) on every sward layer, three
# fields side by side across x round x,z (else the meadow view's clearing, else --look), each --hay-w (40; 22 in the
# meadow view) m wide and --hay-l (140; 56) m deep: mown today (swaths), raked on day 3 (windrows), carted on day 5 (stubble); rows at --hay-row (0.35 rad).
# VegetationField3D leaves the tall herbs out there on its own.
func _hay_list() -> Array:
	var at := String(args.get("hay", ""))
	var w := float(args.get("hay-w", "40"))
	var l := float(args.get("hay-l", "140"))
	var cx := 0.0
	var cz := 0.0
	if at == "" and args.get("view", "wide") == "meadow" and gen != null:
		# The meadow view's clearing: between the camera and the forest edge it looks at (as _place_camera).
		var ex := float(args.get("treeline-x", "300"))
		var ez := 400.0
		while ez > -1500.0 and gen.get_sediment_depth(Vector3(ex, 0, ez)) > 0.4:
			ez -= 16.0
		w = float(args.get("hay-w", "22"))
		l = float(args.get("hay-l", "56"))
		cx = ex - 10.0
		cz = ez + 4.0 + 0.5 * l
	else:
		if at == "":
			at = String(args.get("look", "0,0,0"))
		var hp := at.split(",")
		cx = float(hp[0])
		cz = float(hp[2] if hp.size() > 2 else (hp[1] if hp.size() > 1 else "0"))
	var out: Array = []
	var days := [0.0, 3.0, 5.0]
	for i in 3:
		var x0 := cx + (float(i) - 1.5) * w
		var poly := PackedVector2Array([Vector2(x0, cz - 0.5 * l), Vector2(x0 + w, cz - 0.5 * l), Vector2(x0 + w, cz + 0.5 * l), Vector2(x0, cz + 0.5 * l)])
		out.append({"polygon": poly, "row_angle": float(args.get("hay-row", "0.35")), "days": days[i]})
	return out


func _make_walker(look: Node3D = null) -> CharacterBody3D:
	var src := _gltf_node("res://ext/mannequin/mannequiny-0.3.0.glb")
	if src == null:
		return null
	var body := CharacterBody3D.new()
	body.name = "Walker"
	var cs := CollisionShape3D.new()
	var cap := CapsuleShape3D.new()
	cap.radius = 0.22
	cap.height = 1.75
	cs.shape = cap
	cs.position.y = 0.875
	body.add_child(cs)
	body.add_child(src)
	body.add_to_group("grass_trample")
	add_child(body)
	var skel := src.find_children("*", "Skeleton3D", true, false)[0] as Skeleton3D
	if ClassDB.class_exists("FootPlacementModifier3D"):
		var fp = ClassDB.instantiate("FootPlacementModifier3D")
		fp.name = "Feet"
		fp.set("hips_bone", "pelvis")
		fp.set("left_thigh_bone", "thigh.l")
		fp.set("left_shin_bone", "calf.l")
		fp.set("left_foot_bone", "foot.l")
		fp.set("right_thigh_bone", "thigh.r")
		fp.set("right_shin_bone", "calf.r")
		fp.set("right_foot_bone", "foot.r")
		skel.add_child(fp)
		fp.set("walker_node", fp.get_path_to(body))
	if look != null and ClassDB.class_exists("GazeModifier3D"):
		var gz = ClassDB.instantiate("GazeModifier3D")
		gz.name = "Gaze"
		gz.set("head_bone", "head")
		gz.set("neck_bone", "neck_01")
		gz.set("spine_bones", PackedStringArray(["spine_01", "spine_02"]))
		gz.set("spine_shares", PackedFloat32Array([0.4, 0.6]))
		skel.add_child(gz)
		gz.set("target_node", gz.get_path_to(look))
	var ap := src.find_children("*", "AnimationPlayer", true, false)
	if not ap.is_empty():
		var player := ap[0] as AnimationPlayer
		var an := String(args.get("walker-anim", "walk"))
		if an == "walk" and not player.has_animation("walk") and player.has_animation("idle"):
			# no walk in the rig: one built from gait data (walk_cycle.gd) on its idle, played so the planted
			# feet travel at the walker's speed
			var names := {"pelvis": "pelvis", "spine_01": "spine_01", "spine_02": "spine_02"}
			for sd in ["l", "r"]:
				for k in ["thigh", "calf", "foot", "ball", "upperarm", "lowerarm"]:
					names[k + "_" + sd] = k + "." + sd
			var speed := float(args.get("walker-speed", "1.1"))
			var walk: Animation = WalkCycle.build(skel, player.get_animation("idle"), WalkCycle.period_for(speed), names)
			player.get_animation_library("").add_animation("walk", walk)
			var step: float = WalkCycle.step_length(walk, skel, names)
			player.play("walk")
			player.speed_scale = speed * walk.length / (2.0 * step) if step > 0.0 else 1.0
		elif player.has_animation(an):
			player.get_animation(an).loop_mode = Animation.LOOP_LINEAR
			player.play(an)
			player.speed_scale = float(args.get("walker-anim-speed", "0.5"))
	return body


# --walker-snow: the rigged walker crossing deep snow on a slope (prints from its real foot plants). The nearest
# 12-25 degree slope to the camera with --ws-min (0.4) m of snow; the walker crosses it along the contour at
# --walker-speed (1.1) m/s for --ws-len (12) m, the camera --ws-dist (7) m downslope at eye height, square to its path.
func _walker_snow() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var o := c3.global_position
	var best := Vector3.INF
	var bd := INF
	var mind := float(args.get("ws-min", "0.4"))
	for r in range(20, 1500, 20):
		for k in 24:
			var p := o + Vector3(cos(TAU * k / 24.0), 0, sin(TAU * k / 24.0)) * float(r)
			var deg := rad_to_deg(atan(_slip_grad(p).length()))
			if deg > 12.0 and deg < 25.0 and snow.get_depth_at(p) >= mind:
				best = p
				break
		if best != Vector3.INF:
			break
	if best == Vector3.INF:
		_log("walker snow: no snow slope found")
		return
	var g := _slip_grad(best)
	var down := Vector3(-g.x, 0, -g.y).normalized() if g.length() > 0.0 else Vector3(0, 0, 1)
	var along := down.cross(Vector3.UP).normalized()
	var focus := Node3D.new()
	focus.name = "WalkFocus"
	add_child(focus)
	focus.global_position = Vector3(best.x, height_at(best.x, best.z), best.z)
	snow.follow_target = snow.get_path_to(focus)
	weather.follow_target = weather.get_path_to(focus)
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(focus))
	var cp := best + down * float(args.get("ws-dist", "7"))
	c3.global_position = Vector3(cp.x, height_at(cp.x, cp.z) + snow.get_depth_at(cp) + 1.6, cp.z)
	c3.look_at(focus.global_position + Vector3.UP * (snow.get_depth_at(best) + 0.6))
	var w := _make_walker(c3)
	if w == null:
		_log("walker snow: ext/mannequin missing (run fetch_ext.sh)")
		return
	var length := float(args.get("ws-len", "12"))
	var a := best - along * length * 0.5
	var b := best + along * length * 0.5
	_walk_path(w, a, b, float(args.get("walker-speed", "1.1")))
	_log("walker snow: slope at %s, %.0f deg, %.2f m of snow, path %s to %s, cam %s" % [str(best.round()), rad_to_deg(atan(g.length())), snow.get_depth_at(best), str(a.round()), str(b.round()), str(c3.global_position.round())])


# Moves a walker from a to b at speed (m/s), standing on the ground plus its snow, facing its way (the model's +z).
func _walk_path(w: CharacterBody3D, a: Vector3, b: Vector3, speed: float) -> void:
	var d := (b - a)
	d.y = 0.0
	var dist := d.length()
	var dir := d / maxf(dist, 1e-3)
	w.set_meta("walk", [a, dir, dist, speed, 0.0])
	w.global_basis = Basis.looking_at(-dir, Vector3.UP)
	var put := func(s: float) -> void:
		var q := a + dir * s
		w.global_position = Vector3(q.x, height_at(q.x, q.z) + (snow.get_depth_at(q) if snow != null else 0.0), q.z)
	put.call(0.0)
	var cb := func() -> void:
		var m: Array = w.get_meta("walk")
		m[4] = minf(float(m[4]) + speed * get_physics_process_delta_time(), dist)
		put.call(float(m[4]))
	get_tree().physics_frame.connect(cb)


# Sets a prop (from _tripo) on the ground at `p`, turned `yaw` radians, sunk `sink` m so no edge floats on a slope.
func _put(n: Node3D, p: Vector3, yaw: float, sink := 0.05, parent: Node = null) -> Node3D:
	n.rotation.y = yaw
	n.position = Vector3(p.x, height_at(p.x, p.z) - sink, p.z)
	(parent if parent != null else self).add_child(n)
	return n


# --view=beach: the cove dressed with the Tripo props (--cove-props=0: bare). A sea fort on its rock out in the water,
# --fort-off (240) m out along the view and --fort-size (36) m across; its causeway running out from the shore; the
# lighthouse (--lighthouse-h 17 m) and the keeper's house on the highest ground within 120 m of the beach; a fishing
# boat drawn up at the waterline with an overturned one, fish crates, lobster pots, an anchor and a bollard above it.
func _cove_props() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var f := -c3.global_basis.z
	f.y = 0.0
	f = f.normalized()
	var sd := Vector3(f.z, 0, -f.x)
	var sea := float(gen.sea_level)
	# The waterline along the view: the first point out from the camera under the sea.
	var wl := c3.global_position
	for k in 400:
		wl = c3.global_position + f * (k * 1.0)
		if height_at(wl.x, wl.z) < sea + 0.15:
			break
	var placed := []
	var fp := wl + f * float(args.get("fort-off", "240")) - sd * 60.0
	var fort := _tripo("sea_fort", float(args.get("fort-size", "36")), 0)
	if fort != null:
		# Its rock plinth reaches into the seabed; the walls stand clear of the water.
		fort.rotation.y = atan2(f.x, f.z) + 0.6
		fort.position = Vector3(fp.x, minf(height_at(fp.x, fp.z), sea - 6.0), fp.z)
		add_child(fort)
		placed.append("fort %s" % str(fort.position.round()))
		var cw := _tripo("sea_fort_causeway", 48.0, 0)
		if cw != null:
			var cs := wl + (fp - wl) * 0.25
			cw.rotation.y = atan2(-(fp - wl).z, (fp - wl).x)
			cw.position = Vector3(cs.x, minf(height_at(cs.x, cs.z), sea - 7.0), cs.z)
			add_child(cw)
			placed.append("causeway %s" % str(cw.position.round()))
	# The highest ground within 120 m of the waterline, to the side of the view.
	var best := Vector3(0, -INF, 0)
	for i in 24:
		for j in 12:
			var q := wl + sd * (-120.0 + i * 10.0) - f * (j * 10.0)
			var h := height_at(q.x, q.z)
			if h > best.y and rad_to_deg(data_map.get_slope_at(q)) < 20.0 and absf((q - c3.global_position).dot(sd)) > 25.0:
				best = Vector3(q.x, h, q.z)
	if is_finite(best.y):
		var lh := _tripo("lighthouse", float(args.get("lighthouse-h", "17")), 1)
		if lh != null:
			_put(lh, best, atan2(f.x, f.z) + PI, 0.4)
			placed.append("lighthouse %s" % str(best.round()))
		var kh := _tripo("lighthouse_keepers_house", 11.0, 0)
		if kh != null and lh != null:
			# Tripo gave the tower a second door on its -x side: the keeper's house is built against that side, so it
			# reads as the passage from the house into the tower.
			var tx: Vector3 = lh.basis * Vector3(-1, 0, 0)
			var kp := best + tx * float(args.get("keeper-off", "7.4"))
			_put(kh, kp, atan2(tx.x, tx.z) + PI, 0.4)
	# The beach: boats and gear a few metres above the waterline, beside the view.
	var bp := wl - f * 3.0 + sd * 7.0
	var gear := [["fishing_boat", 4.6, 2, bp, 0.3], ["fishing_boat_overturned", 4.4, 0, bp - f * 6.0 + sd * 3.0, 1.2],
		["fish_crates_stack", 1.2, 1, bp - f * 4.0 - sd * 2.5, 0.4], ["lobster_pots", 1.3, 1, bp - f * 5.0 - sd * 4.2, 2.0],
		["old_anchor", 1.9, 0, bp - f * 2.0 - sd * 5.5, 0.9], ["mooring_bollard", 0.9, 0, bp - f * 8.0 - sd * 0.5, 0.0]]
	for g in gear:
		var gn := _tripo(g[0], g[1], g[2])
		if gn != null and g[0] == "lobster_pots":
			_lobster_net(gn.get_child(0))
		if gn != null:
			_put(gn, g[3], atan2(f.x, f.z) + g[4], 0.06)
			placed.append(g[0])
	_log("cove props: waterline %s; %s" % [str(wl.round()), ", ".join(placed)])


# The farm track dressed (--farm-props=0: bare): split-rail fence along its right side --fence-off (3.2) m out with a
# gate 30 m on, round hay bales and a small stack out in the field, a signpost at the start and a high seat at the
# field edge 70 m on (Tripo props).
func _farm_props(start: Vector3, fwd: Vector3, bend: float, n: int, seg: float) -> void:
	var pts := []
	var p := start + Vector3(fwd.z, 0, -fwd.x) * 0.8
	var dir := fwd
	for k in n:
		pts.append([p, dir])
		p += dir * seg
		dir = dir.rotated(Vector3.UP, bend / n)
	var off := float(args.get("fence-off", "3.2"))
	var count := 0
	var along := 6.0
	while along < seg * n - 3.0:
		var i := int(along / seg)
		var q: Vector3 = pts[i][0] + pts[i][1] * fmod(along, seg)
		var d: Vector3 = pts[i][1]
		var r := Vector3(-d.z, 0, d.x) # right of the track
		var kind := "split_rail_fence_gate" if absf(along - 30.0) < 1.5 else ("split_rail_fence" if count % 2 == 0 else "split_rail_fence_b")
		# split_rail_fence runs along its z, the others along x.
		var fence := _tripo(kind, 3.0, 2 if kind == "split_rail_fence" else 0)
		if fence == null:
			return
		var yaw := atan2(d.x, d.z) + (0.0 if kind == "split_rail_fence" else PI * 0.5)
		_put(fence, q + d * 1.5 + r * off, yaw, 0.12)
		along += 3.0
		count += 1
	var mid: Array = pts[mini(int(45.0 / seg), pts.size() - 1)]
	var rr := Vector3(-mid[1].z, 0, mid[1].x)
	for k in 3:
		var hb := _tripo("hay_bale", 1.45, 1)
		if hb != null:
			_put(hb, mid[0] + rr * (off + 9.0 + 3.5 * k) + mid[1] * (2.0 * k - 3.0 * (k % 2)), 0.9 * k, 0.08)
	var st := _tripo("hay_bales_small_stack", 2.0, 2)
	if st != null:
		var gp: Array = pts[mini(int(30.0 / seg), pts.size() - 1)]
		_put(st, gp[0] + Vector3(-gp[1].z, 0, gp[1].x) * (off + 3.0) + gp[1] * 4.0, 0.4, 0.06)
	var sg := _tripo("trail_signpost", 2.3, 1)
	if sg != null:
		var s0: Array = pts[mini(4, pts.size() - 1)]
		_put(sg, s0[0] - Vector3(-s0[1].z, 0, s0[1].x) * 2.3, atan2(s0[1].x, s0[1].z) + PI * 0.5, 0.25)
	var hs := _tripo("hunting_high_seat", 4.5, 1)
	if hs != null:
		var h0: Array = pts[mini(int(70.0 / seg), pts.size() - 1)]
		_put(hs, h0[0] + Vector3(-h0[1].z, 0, h0[1].x) * (off + 14.0), atan2(h0[1].x, h0[1].z) + PI, 0.1)
	_log("farm props: %d fence pieces" % count)


# Showcase Tripo props (art-src props/showcase/<slug>/<tier>/<slug>.glb, tier bakes of the raws by tools/tripo_tiers.py;
# --prop-tier high|medium|low, high by default). Each is loaded once and copied; its origin is the centre of its
# footprint on the ground. Scaled uniformly so its extent along `axis` (0 x, 1 y, 2 z) is `length` m, then by
# `stretch` (a rail made longer). null when the art is missing (run fetch_art.sh): callers keep their code shape.
var _tripo_cache := {}
func _tripo(slug: String, length: float, axis: int = 1, stretch := Vector3.ONE) -> Node3D:
	var path := "res://art/showcase/%s/%s/%s.glb" % [slug, args.get("prop-tier", "high"), slug]
	if not _tripo_cache.has(path):
		_tripo_cache[path] = _gltf_node(path) if FileAccess.file_exists(path) else null
		if _tripo_cache[path] == null:
			_log("prop %s missing (run fetch_art.sh), code shape kept" % slug)
	var src: Node3D = _tripo_cache[path]
	if src == null:
		return null
	var n := src.duplicate() as Node3D
	var box := AABB()
	var first := true
	for mi in n.find_children("*", "MeshInstance3D", true, false):
		var m := mi as MeshInstance3D
		var b: AABB = m.transform * m.get_aabb()
		box = b if first else box.merge(b)
		first = false
	var k := length / maxf(box.size[axis], 1e-4)
	var root := Node3D.new()
	root.name = slug
	n.scale = Vector3.ONE * k * stretch
	root.add_child(n)
	return root


func _gltf_node(path: String) -> Node3D:
	if not FileAccess.file_exists(path):
		_log("missing %s (run fetch_ext.sh)" % path)
		return null
	var doc := GLTFDocument.new()
	var gs := GLTFState.new()
	doc.append_from_file(path, gs)
	return doc.generate_scene(gs) as Node3D


func _place_buggy() -> void:
	var path := "res://art/offroad_buggy/offroad_buggy.tscn"
	if not ResourceLoader.exists(path):
		_log("buggy: %s missing (run fetch_art.sh), skipped" % path)
		return
	var bp := String(args["buggy"]).split(",")
	var bx := float(bp[0])
	var bz := float(bp[1]) if bp.size() > 1 else 0.0
	if bp[0] == "beach":
		# --buggy=beach[,,deg]: on the beach at x 40, the first sand --buggy-band=lo,hi m above the sea walking inland (0.3,0.9: damp).
		bx = 40.0
		var cz := _coast_z(0.0)
		bz = cz - 10.0
		for k in 200:
			var zz := cz + 20.0 - k * 0.5
			var hh := height_at(bx, zz) - gen.sea_level
			var band := String(args.get("buggy-band", "0.3,0.9")).split(",") # 0.3-0.9 m: damp sand near the waterline
			if hh > float(band[0]) and hh < float(band[1]):
				bz = zz
				break
	var car := (load(path) as PackedScene).instantiate() as Node3D
	car.position = Vector3(bx, height_at(bx, bz) + 3.0, bz) # held clear of the ground until the ray places it
	car.rotation.y = deg_to_rad(float(bp[2])) if bp.size() > 2 else 0.0
	car.set("can_sleep", false) # a resting vehicle asleep ignores the throttle
	car.process_mode = Node.PROCESS_MODE_DISABLED
	add_child(car)
	# Spawn from above the fine ground: a ray down the built collision (a heightfield only stops bodies coming from
	# above), else the coarse height plus 1 m; the collision follows the car.
	if landscape != null and landscape.collision_enabled:
		landscape.collision_target = landscape.get_path_to(car)
		landscape.update_collision()
	for i in 2:
		await get_tree().physics_frame
	var gy := height_at(bx, bz) + 1.0
	var hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(bx, gy + 60.0, bz), Vector3(bx, gy - 80.0, bz)))
	if not hit.is_empty():
		gy = (hit["position"] as Vector3).y + 0.3
	car.global_position = Vector3(bx, gy, bz)
	car.process_mode = Node.PROCESS_MODE_INHERIT
	_log("buggy spawn at %.2f m (%s)" % [gy, "ray" if not hit.is_empty() else "coarse + 1 m"])
	if prints != null:
		prints.follow_target = prints.get_path_to(car)
	if args.has("buggy-grains") and prints != null and ClassDB.class_exists("PhysXGroundGrains3D"):
		var g: Node3D = ClassDB.instantiate("PhysXGroundGrains3D")
		add_child(g)
		g.set("ground_surface", g.get_path_to(prints))
		g.set("follow_target", g.get_path_to(car))
	if args.has("buggy-dust") and prints != null and ClassDB.class_exists("GroundDust3D"):
		# --buggy-dust: dust from dry sand carried on the wind until it settles; it draws through volumetric fog.
		var gd: Node3D = ClassDB.instantiate("GroundDust3D")
		add_child(gd)
		gd.set("ground_surface", gd.get_path_to(prints))
		gd.set("follow_target", gd.get_path_to(car))
		env.volumetric_fog_enabled = true
		env.volumetric_fog_ambient_inject = 1.0
		env.volumetric_fog_anisotropy = 0.6
		env.volumetric_fog_length = 32.0
		_log("buggy dust on (volumetric fog on: ambient inject 1, anisotropy 0.6, length 32 m)")
		if args.has("buggy-flow") and ClassDB.class_exists("PhysXGroundDust3D"):
			# --buggy-flow: near dust (within 40 m) as PhysX Flow gas (needs the Flow libraries built): trails, curls round the car, settles; GroundDust3D keeps the far.
			var fd: Node3D = ClassDB.instantiate("PhysXGroundDust3D")
			add_child(fd)
			fd.set("ground_surface", fd.get_path_to(prints))
			fd.set("follow_target", fd.get_path_to(car))
			_log("buggy Flow dust on")
	for i in 120:
		await get_tree().physics_frame
	if args.has("buggy-soak") and prints != null:
		prints.set("moisture", float(args["buggy-soak"])) # --buggy-soak=M: soaked clay after rain, in sun
	if args.has("buggy-puddle") and prints != null:
		# --buggy-puddle=from,to[,depth]: clay puddles on the buggy's line, FROM to TO m ahead, DEPTH (0.15) m deep,
		# so the tyres churn more clay into them.
		var pp := String(args["buggy-puddle"]).split(",")
		var p0 := float(pp[0])
		var p1 := float(pp[1]) if pp.size() > 1 else p0 + 10.0
		var pd := float(pp[2]) if pp.size() > 2 else 0.15
		var pf := Vector3(car.global_basis.z.x, 0.0, car.global_basis.z.z).normalized() # the buggy drives along +Z
		var pc := car.global_position + pf * (p0 + p1) * 0.5
		prints.set("standing_water", true)
		if not args.has("buggy-soak"):
			prints.set("moisture", 0.85)
		# Clay puddles along the line (GroundSurface3D.add_puddle: irregular lobed hollows that fill level, with
		# --buggy-puddle-mud (40) kg/m3 of clay in the water), radius 1-1.5 m every 2.2 m.
		var nmud := float(args.get("buggy-puddle-mud", "40"))
		var np := maxi(1, int(round((p1 - p0) / 2.2)) + 1)
		for k in np:
			var t := p0 + (p1 - p0) * (float(k) / maxf(np - 1, 1))
			var wq := car.global_position + pf * t + pf.cross(Vector3.UP) * (0.25 * sin(k * 2.1))
			prints.call("add_puddle", Vector3(wq.x, height_at(wq.x, wq.z), wq.z), 1.0 + 0.5 * fposmod(k * 0.618, 1.0), pd, nmud)
		for i in 150:
			await get_tree().physics_frame
		prints.call("sync_deformation")
		for i in 10:
			await get_tree().physics_frame
		var wl := []
		for k in np:
			var t2 := p0 + (p1 - p0) * (float(k) / maxf(np - 1, 1))
			var wq2 := car.global_position + pf * t2 + pf.cross(Vector3.UP) * (0.25 * sin(k * 2.1))
			wl.append(snappedf(float(prints.call("get_standing_water_at", wq2)) * 100.0, 0.1))
		_log("buggy puddles %.0f-%.0f m ahead: water %s cm at their centres, owed %s, moisture %s, standing_water %s" % [p0, p1, str(wl), str(prints.call("get_water_owed")) if prints.has_method("get_water_owed") else "?", str(prints.get("moisture")), str(prints.get("standing_water"))])
	var start := car.global_position
	var slide_steer := 0.0
	# The drive, the winch and the drying run with drawing off (lavapipe draws a frame in seconds; the ground's
	# deformation still runs, as for --rills), --sim-render=1 to draw each step.
	var drawn_off: bool = args.get("sim-render", "0") == "0" and (args.has("buggy-drive") or args.has("buggy-winch") or args.has("buggy-crack"))
	if drawn_off:
		RenderingServer.render_loop_enabled = false
	if args.has("buggy-drive"):
		# --buggy-drive=throttle,seconds[,steer][;throttle,seconds,steer...]: phases in turn. Then it brakes to a stop,
		# unless --buggy-hold: the last phase is caught mid-motion (time frozen for the capture: dust and sand hold).
		var phase := 0
		for ph in String(args["buggy-drive"]).split(";"):
			# --buggy-dry-at=K: from phase K on the ground has dried out (moisture 0.1), so the tyres carry the clay
			# they picked up onto firm ground (tracked mud).
			if args.has("buggy-dry-at") and phase == int(args["buggy-dry-at"]) and prints != null:
				prints.set("moisture", 0.1)
				prints.call("settle", 1.0, 0.5)
				_log("buggy: ground dried for phase %d" % phase)
			phase += 1
			var dp := ph.split(",")
			car.set("throttle", float(dp[0]))
			car.set("brake", 0.0)
			slide_steer = float(dp[2]) if dp.size() > 2 else 0.0
			car.set("steer", slide_steer)
			for i in int(float(dp[1]) * Engine.physics_ticks_per_second):
				await get_tree().physics_frame
		if not args.has("buggy-hold"):
			car.set("throttle", 0.0)
			car.set("brake", 1.0)
			for i in 60:
				await get_tree().physics_frame
	if args.has("buggy-winch") and ClassDB.class_exists("PhysXWinch3D"):
		# --buggy-winch=D: a generated tree (--winch-tree preset, 0 oak) D m ahead of where the buggy stopped, a winch at its
		# front (--winch-fairlead=z, 1.7 m), the strap round the trunk; the cable paid out --winch-slack (2) m past the
		# straight line so it lies on the ground, then wound in for --winch-seconds (4) s with the wheels turning gently.
		var wf := Vector3(car.global_basis.z.x, 0.0, car.global_basis.z.z).normalized()
		var tp := car.global_position + wf * float(args["buggy-winch"])
		tp.y = height_at(tp.x, tp.z) - 0.05
		var tg := TreeGenerator.new()
		tg.preset = int(args.get("winch-tree", "0"))
		tg.seed = int(args.get("solo-seed", "5"))
		var tmi := MultiMeshInstance3D.new()
		tmi.multimesh = TreeWind3D.make_multimesh(tg.generate(), [Transform3D(Basis(), tp)])
		add_child(tmi)
		var anchor := Node3D.new()
		add_child(anchor)
		anchor.global_position = tp - wf * 0.22 + Vector3(0, 0.7, 0)
		var w: Node3D = ClassDB.instantiate("PhysXWinch3D")
		w.position = Vector3(0, 0.45, float(args.get("winch-fairlead", "1.7")))
		car.add_child(w)
		await get_tree().physics_frame
		w.set("anchor", w.get_path_to(anchor))
		w.set("anchor_point", Vector3.ZERO)
		w.set("cable_length", float(w.call("get_anchor_distance")) + float(args.get("winch-slack", "2")))
		w.set("attached", true)
		car.set("brake", 0.0)
		for i in 30:
			await get_tree().physics_frame
		w.set("drum", 1) # DRUM_IN
		car.set("throttle", 0.25)
		for i in int(float(args.get("winch-seconds", "4")) * Engine.physics_ticks_per_second):
			await get_tree().physics_frame
		car.set("throttle", 0.0)
		w.set("drum", 0) # DRUM_HOLD
		for i in 10:
			await get_tree().physics_frame
		_log("winch: tree at %s, %.1f m of cable, %.1f m straight, tension %.0f N" % [str(tp.round()), float(w.get("cable_length")), float(w.call("get_anchor_distance")), float(w.call("get_tension"))])
	if args.has("buggy-crack") and weather != null:
		# --buggy-crack: a hot, dry, windy afternoon after the drive until the clay on the body has dried and cracked
		# into plates (wetness under 0.01, at most --crack-seconds, 600 s).
		weather.set("air_follows_climate", false)
		weather.temperature = 32.0
		weather.humidity = 0.12
		weather.wind_speed = 8.0
		weather.cloud_cover = 0.05
		weather.rain_rate = 0.0
		weather.snap_to_settings()
		var t := 0
		var tmax := int(float(args.get("crack-seconds", "600")) * Engine.physics_ticks_per_second)
		while float(car.call("get_body_mud_wetness")) > 0.01 and t < tmax:
			await get_tree().physics_frame
			t += 1
		_log("buggy crack: wetness %.3f after %d frames, mud line %.2f m, flakes shed %d" % [float(car.call("get_body_mud_wetness")), t, float(car.call("get_body_mud_line")), int(car.call("get_mud_flakes_shed"))])
	if drawn_off:
		if prints != null:
			prints.sync_deformation()
		for i in 3:
			await get_tree().physics_frame
		RenderingServer.render_loop_enabled = true
	if args.has("buggy-age") and prints != null:
		# --buggy-age=S: S seconds of the ground's own drying after the drive (the tracked film pales in place).
		prints.call("settle", float(args["buggy-age"]), 10.0)
		_log("buggy: ground aged %s s" % String(args["buggy-age"]))
	var sink := []
	if car.has_method("get_wheel_sinkage"):
		for i in 4:
			sink.append(snappedf(car.call("get_wheel_sinkage", i), 0.001))
	_log("buggy: at %s heading %.0f deg, moved %.1f m, wheel sinkage %s" % [str(car.global_position.snapped(Vector3.ONE * 0.01)), rad_to_deg(car.rotation.y), start.distance_to(car.global_position), str(sink)])
	if args.has("buggy-verge-cam"):
		# --buggy-verge-cam=h: low at the verge where the buggy set off, --buggy-verge-cam (0.45) m up, 1.3 m to the
		# side of its line, looking 1.6 m along it at the grass beside the ruts (clay on the blades, plant_mud).
		var vf := Vector3(car.global_basis.z.x, 0.0, car.global_basis.z.z).normalized()
		var vs := vf.cross(Vector3.UP)
		var vp := start + vs * 1.3 - vf * 0.4
		var vc: Camera3D = get_viewport().get_camera_3d()
		vc.global_position = Vector3(vp.x, height_at(vp.x, vp.z) + float(args["buggy-verge-cam"]) if String(args["buggy-verge-cam"]).is_valid_float() else height_at(vp.x, vp.z) + 0.45, vp.z)
		var vl := start + vf * 1.6 + vs * 0.7
		vc.look_at(Vector3(vl.x, height_at(vl.x, vl.z) + 0.1, vl.z))
		_log("buggy verge cam %s" % str(vc.global_position))
	if args.has("buggy-cam"):
		var cp := String(args["buggy-cam"]).split(",")
		var fwd := car.global_basis.z
		fwd = Vector3(fwd.x, 0.0, fwd.z).normalized()
		var side := fwd.cross(Vector3.UP)
		var cam: Camera3D = get_viewport().get_camera_3d()
		var at := car.global_position - fwd * float(cp[0]) + side * (float(cp[2]) if cp.size() > 2 else 0.0)
		cam.global_position = Vector3(at.x, height_at(at.x, at.z) + (float(cp[1]) if cp.size() > 1 else 1.5), at.z)
		cam.look_at(car.global_position + Vector3(0, 0.5, 0))
		_log("buggy cam %s" % str(cam.global_position.round()))
	if args.has("buggy-sun-cam") and sun != null:
		# --buggy-sun-cam=dist,height,off: backlit, looking at the rear wheel past the low sun, OFF degrees to the
		# side of it; the side is picked outside the turn.
		var cp2 := String(args["buggy-sun-cam"]).split(",")
		var fwd2 := Vector3(car.global_basis.z.x, 0.0, car.global_basis.z.z).normalized()
		var rear := car.global_position - fwd2 * 1.2
		var ts := sun.global_basis.z
		var sun_flat := Vector3(ts.x, 0.0, ts.z).normalized()
		var off := deg_to_rad(float(cp2[2]) if cp2.size() > 2 else 25.0)
		var best_pos := Vector3.ZERO
		var best_out := -INF
		var turn_in := car.global_basis.x * signf(slide_steer if slide_steer != 0.0 else 1.0) # +X is the car's left; steer > 0 turns left
		for sg in [-1.0, 1.0]:
			var d := sun_flat.rotated(Vector3.UP, sg * off)
			var p := rear - d * float(cp2[0])
			var outward := (p - rear).dot(-turn_in)
			if outward > best_out:
				best_out = outward
				best_pos = p
		var cam2: Camera3D = get_viewport().get_camera_3d()
		cam2.global_position = Vector3(best_pos.x, height_at(best_pos.x, best_pos.z) + (float(cp2[1]) if cp2.size() > 1 else 1.2), best_pos.z)
		cam2.look_at(rear + Vector3(0, 0.4, 0))
		if args.has("snow-road") and snow != null and snow.has_method("get_glaze_at"):
			var gl := 0.0
			var pk := 0.0
			for k in 12:
				var q := car.global_position - fwd2 * (0.5 * k)
				gl = maxf(gl, float(snow.call("get_glaze_at", q)))
				pk = maxf(pk, float(snow.call("get_packed_at", q)))
			_log("snow road: behind the car glaze up to %.2f mm, packed up to %.2f, grip at the rear wheel %.2f" % [gl, pk, float(snow.call("get_grip_at", rear))])
		_log("buggy sun cam %s, sun elevation %.1f deg, %s the turn" % [str(cam2.global_position.round()), rad_to_deg(asin(clampf(ts.y, -1.0, 1.0))), "outside" if best_out > 0.0 else "inside"])
	if args.has("buggy-hold"):
		PhysicsServer3D.set_active(false) # mid-slide: hold the moment for the capture frames (a zero time scale makes PhysX vehicles complain)
		_log("buggy held mid-motion: speed %.1f m/s" % (car.get("linear_velocity") as Vector3).length() if car.get("linear_velocity") != null else "buggy held")


# --view=slipface (--layout=desert): a bare slip face of the sand sea at 30-34 degrees with its brink above, the
# camera --slip-dist m (6) off a line of boot prints across it, looking along the face --slip-angle degrees (30) into
# it, eye --slip-eye m (1.6). Faces whose strike runs nearest the sun's azimuth win (side light, so the avalanche
# tongues cast shadow): pick the sun with --time and the faces with --sand-sea-wind. --slip-probe logs and quits.
func _slipface_camera() -> void:
	var r: Rect2 = gen.sand_sea_rect
	if not r.has_area():
		r = gen.get_dune_rect()
	r = r.grow(-80.0)
	var sun_h := Vector3(1, 0, 0)
	if clock != null:
		var sd := clock.get_sun_direction()
		sun_h = Vector3(sd.x, 0, sd.z).normalized()
		_log("slipface: sun toward %s, altitude %.1f deg, azimuth %.1f deg" % [str(sd), clock.get_sun_altitude(), clock.get_sun_azimuth()])
	var stp := float(args.get("slip-step", "3"))
	var amin := deg_to_rad(float(args.get("slip-min", "30")))
	var amax := deg_to_rad(float(args.get("slip-max", "34")))
	var best := {}
	var best_s := -INF
	var found := 0
	var zz := r.position.y
	while zz < r.end.y:
		var xx := r.position.x
		while xx < r.end.x:
			var g := _slip_grad(Vector3(xx, 0, zz))
			var a := atan(g.length())
			if a >= amin and a <= amax:
				var d := Vector3(-g.x, 0, -g.y).normalized() # downslope
				# The brink: uphill until the ground flattens under 12 degrees, 2-10 m up; the toe: downhill under 15.
				var up := -1.0
				for k in 40:
					var q := Vector3(xx, 0, zz) - d * (0.5 + 0.5 * k)
					if atan(_slip_grad(q).length()) < deg_to_rad(12.0):
						up = 0.5 + 0.5 * k
						break
				var down := -1.0
				for k in 60:
					var q := Vector3(xx, 0, zz) + d * (0.5 + 0.5 * k)
					if atan(_slip_grad(q).length()) < deg_to_rad(15.0):
						down = 0.5 + 0.5 * k
						break
				if up >= 2.0 and up <= 10.0 and down >= 3.0:
					found += 1
					var t := Vector3(d.z, 0, -d.x)
					# Side light: the face's strike along the sun, the sun a little in front of the face (onto it).
					var s := absf(t.dot(sun_h)) * 2.0 - absf(sun_h.dot(d) - 0.25) + minf(up + down, 16.0) * 0.05
					if s > best_s:
						best_s = s
						best = {"p": Vector3(xx, 0, zz), "d": d, "t": t, "up": up, "down": down, "a": rad_to_deg(a)}
			xx += stp
		zz += stp
	if best.is_empty():
		_log("slipface: no slip face at %.0f-%.0f deg in %s" % [rad_to_deg(amin), rad_to_deg(amax), str(r)])
		if args.has("slip-probe"):
			get_tree().quit(1)
		return
	var p0: Vector3 = best["p"]
	var d0: Vector3 = best["d"]
	var t0: Vector3 = best["t"]
	if t0.dot(sun_h) < 0.0:
		t0 = -t0 # walk and look toward the sun's side: the camera looks across the light
	# The walk: across the face a third of the way down from the brink, 7 m, drifting 8 cm down a 70 cm stride.
	var up0: float = best["up"]
	var down0: float = best["down"]
	var mid := p0 - d0 * up0 + d0 * (up0 + down0) * float(args.get("slip-walk-at", "0.33"))
	var half := float(args.get("slip-walk-len", "7")) * 0.5
	var a0 := mid - t0 * half - d0 * half * 0.08 / 0.7
	var b0 := mid + t0 * half + d0 * half * 0.08 / 0.7
	slip_walk = PackedVector3Array([a0, b0, t0, d0])
	var ang := deg_to_rad(float(args.get("slip-angle", "30")))
	var v := t0 * cos(ang) - d0 * sin(ang) # along the face, turned into it
	var dist := float(args.get("slip-dist", "6"))
	var cp := mid - v * dist
	cam.fov = float(args.get("slip-fov", "50"))
	cam.near = 0.05
	cam.position = Vector3(cp.x, gen.get_height(cp) + float(args.get("slip-eye", "1.6")), cp.z)
	var look := mid - d0 * float(args.get("slip-look-up", "1.0"))
	cam.look_at(Vector3(look.x, gen.get_height(look) + float(args.get("slip-look-dy", "0.3")), look.z))
	if prints != null:
		prints_probe.global_position = Vector3(mid.x, gen.get_height(mid), mid.z)
		prints.follow_target = prints.get_path_to(prints_probe)
	_log("slipface: %d faces; picked %s at %.1f deg, brink %.1f m up, toe %.1f m down, fall %s, strike.sun %.2f, cam %s, walk %s to %s" % [found, str(p0.round()), best["a"], up0, down0, str(d0), t0.dot(sun_h), str(cam.position), str(a0), str(b0)])
	if args.has("slip-probe"):
		get_tree().quit(0)


# The ground's gradient (dh/dx, dh/dz) at p, over a metre.
func _slip_grad(p: Vector3) -> Vector2:
	var hx := gen.get_height(p + Vector3(0.5, 0, 0)) - gen.get_height(p - Vector3(0.5, 0, 0))
	var hz := gen.get_height(p + Vector3(0, 0, 0.5)) - gen.get_height(p - Vector3(0, 0, 0.5))
	return Vector2(hx, hz)


# The walk across the slip face: two boot bodies in world_contact (80 kg on 0.035 m2, ground_sole), each planted for
# --slip-plant frames (20) then lifted (hidden: GroundSurface3D skips hidden bodies), alternating 9 cm either side of
# the walk line, toed out about 6 degrees; then --slip-run s (12) for the avalanches to run down to the toe. Drawing
# is off while it runs unless --sim-render=1.
func _slip_walk() -> void:
	var a0 := slip_walk[0]
	var b0 := slip_walk[1]
	var t0 := slip_walk[2]
	var d0 := slip_walk[3]
	var feet: Array[Node3D] = []
	for k in 2:
		var foot := Node3D.new()
		foot.name = "SlipFoot%d" % k
		var fs := CollisionShape3D.new()
		fs.shape = BoxShape3D.new()
		(fs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
		foot.add_child(fs)
		foot.set_meta("ground_mass", 80.0)
		foot.set_meta("ground_contact_area", 0.035)
		foot.set_meta("ground_sole", true)
		foot.add_to_group("world_contact")
		foot.visible = false
		add_child(foot)
		feet.append(foot)
	for i in 30:
		await get_tree().physics_frame
	var drawn_off: bool = args.get("sim-render", "0") == "0"
	if drawn_off:
		RenderingServer.render_loop_enabled = false
	var n := int(a0.distance_to(b0) / 0.7) + 1
	var rng := RandomNumberGenerator.new()
	rng.seed = int(args.get("slip-seed", "5"))
	var plant := int(args.get("slip-plant", "20"))
	var peak := 0
	var midp := a0.lerp(b0, 0.5)
	_log("slipface ground at the walk: %s, slope %.1f deg" % [str(prints.get_material_at(midp)), rad_to_deg(atan(_slip_grad(midp).length()))])
	var tongues0: int = prints.call("get_avalanche_count") if prints.has_method("get_avalanche_count") else -1
	for k in n:
		var foot := feet[k % 2]
		var side := 0.09 if k % 2 == 0 else -0.09
		var p := a0.lerp(b0, float(k) / maxf(n - 1, 1)) + d0 * (side + rng.randf_range(-0.02, 0.02)) + t0 * rng.randf_range(-0.04, 0.04)
		var toe := (0.1 if k % 2 == 0 else -0.1) + rng.randf_range(-0.05, 0.05)
		var fwd := t0.rotated(Vector3.UP, toe)
		foot.global_basis = Basis.looking_at(-fwd) # the sole's toe along local +z
		prints.sync_deformation()
		foot.global_position = Vector3(p.x, prints.get_surface_height_at(p) + 0.03, p.z)
		foot.visible = true
		for i in plant:
			await get_tree().physics_frame
			if prints.has_method("get_avalanche_count"):
				peak = maxi(peak, int(prints.call("get_avalanche_count")))
		foot.visible = false
		for i in 6:
			await get_tree().physics_frame
	for i in int(float(args.get("slip-run", "12")) * 60.0):
		await get_tree().physics_frame
		if prints.has_method("get_avalanche_count"):
			peak = maxi(peak, int(prints.call("get_avalanche_count")))
	prints.sync_deformation()
	for i in 3:
		await get_tree().physics_frame
	if drawn_off:
		RenderingServer.render_loop_enabled = true
	var tongues1: int = prints.call("get_avalanche_count") if prints.has_method("get_avalanche_count") else -1
	_log("slipface walk: %d steps, avalanches %d -> %d (at most %d running), dent at the walk's middle %.3f m" % [n, tongues0, tongues1, peak, prints.get_dent_at(a0.lerp(b0, 0.5))])


# --farm-track: an old farm track worn in by wheels (GroundSurface3D.worn_tracks, add_wear): two wheel ruts 1.6 m apart
# and 0.4 m wide running --track-length m (90) ahead from under the camera, bending --track-bend degrees (12) over it,
# --track-passes (400) passes each, a lighter worn crown between them (an eighth). The camera stands in the left rut.
# --track-age=D: then D days of regrowth (the ground's own time, faded over wear_regrowth_days) for a slider pair.
func _build_farm_track() -> void:
	var c3 := get_viewport().get_camera_3d()
	var fwd := -c3.global_basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var side := Vector3(fwd.z, 0, -fwd.x)
	var length := float(args.get("track-length", "90"))
	var bend := deg_to_rad(float(args.get("track-bend", "12")))
	var passes := float(args.get("track-passes", "400"))
	var start := c3.global_position - side * 0.8 - fwd * 2.0 # the camera in the left rut
	var p := start
	var dir := fwd
	var seg := 2.0
	var n := int(length / seg)
	for k in n:
		var mid := p + dir * seg * 0.5
		var yaw := atan2(dir.x, dir.z)
		var sd := Vector3(dir.z, 0, -dir.x)
		for w in [-0.8, 0.8]:
			var q: Vector3 = mid + sd * w
			q.y = height_at(q.x, q.z)
			prints.call("add_wear", q, Vector2(0.4, seg + 0.2), yaw, passes)
		var qc := mid
		qc.y = height_at(qc.x, qc.z)
		prints.call("add_wear", qc, Vector2(0.9, seg + 0.2), yaw, passes * 0.125)
		p += dir * seg
		dir = dir.rotated(Vector3.UP, bend / n)
	if args.has("track-age"):
		var days := float(args["track-age"])
		prints.call("settle", days * 86400.0, 3600.0 * 6.0)
		_log("farm track: aged %.0f days" % days)
	if args.get("farm-props", "1") != "0":
		_farm_props(start, fwd, bend, n, seg)
	_log("farm track: %d segments, %.0f passes, wear under the camera %.0f, 30 m on %.0f" % [n, passes, float(prints.call("get_wear_at", start + side * 0.8 * 0 )), float(prints.call("get_wear_at", start + fwd * 30.0))])


# --powder-avalanche (winter, --month=1): a dry slab released on a real showcase face, run in real time with its powder
# cloud (SnowAvalanche3D, PhysX Flow) and shot --powder-at s (9) after release, three-quarters from the side so the
# head's height and the cloud trailing back up the track both show, the release zone in view. The face: the steepest
# 35-45 degree cell on a --av-step (40) m grid with --av-min-snow (0.3) m of snow on it, scored for side light
# (the fall line square to the sun). --av-probe: log the pick and quit. --av-dist (260) m camera distance.
func _suncups() -> void:
	# --suncups[=M]: a spring snowfield with suncups M (0.25) m deep (SnowCover3D.add_suncups, snow 89d02105), drawn by a
	# SnowSurface3D round the camera. The camera stands 1.6 m over the deepest gentle (under --cups-max-deg, 15) snow
	# at least --cups-min-snow (0.5) m deep, looking square to the sun so the low light rakes the cups.
	var mm := float(args["suncups"]) if String(args["suncups"]) != "" else 0.25
	var wr: Rect2 = gen.get_world_rect()
	var stp := 40.0
	var best := Vector3.INF
	var best_d := 0.0
	var xx := wr.position.x + 400.0
	while xx < wr.end.x - 400.0:
		var zz := wr.position.y + 400.0
		while zz < wr.end.y - 400.0:
			var p := Vector3(xx, 0, zz)
			var ang := rad_to_deg(atan(_slip_grad(p).length()))
			var d: float = snow.get_depth_at(p)
			if ang < float(args.get("cups-max-deg", "15")) and d >= float(args.get("cups-min-snow", "0.5")) and d > best_d:
				best_d = d
				best = Vector3(xx, gen.get_height(p), zz)
			zz += stp
		xx += stp
	if best == Vector3.INF:
		_log("suncups: no gentle deep snow")
		return
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var look_dir := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("cups-yaw", "80"))))
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = best + Vector3.UP * (1.6 + best_d)
	c3.look_at(c3.global_position + look_dir * 6.0 - Vector3.UP * (1.6 + 0.4))
	var focus := Node3D.new()
	focus.name = "CupsFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	for i in 30:
		await get_tree().physics_frame
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(c3))
	snow.call("add_suncups", mm)
	for i in 10:
		await get_tree().process_frame
	if args.has("snow-tracks"):
		await _snow_tracks(surf, c3, look_dir)
	_log("suncups: %.2f m cups on %.2f m snow at %s, cam %s, sun %.1f deg" % [mm, best_d, str(best.round()), str(c3.global_position.round()), rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0)))])


func _snow_road_pick() -> void:
	# --snow-road: the buggy on gentle snow --road-min..--road-max (0.15..0.6 m) deep, heading --road-yaw (90) degrees
	# off the sun so a --buggy-sun-cam looks across its packed track and glaze (snow e5ce60c3: world_contact wheels pack
	# the snow they roll on and polish a glaze where they spin). A SnowSurface3D round the spot draws it; the drive is
	# --buggy-drive (say a wheelspin start then a run), set on the command line.
	var lo := float(args.get("road-min", "0.15"))
	var hi := float(args.get("road-max", "0.6"))
	var wr: Rect2 = gen.get_world_rect()
	var best := Vector3.INF
	var best_s := INF
	var xx := wr.position.x + 300.0
	while xx < wr.end.x - 300.0:
		var zz := wr.position.y + 300.0
		while zz < wr.end.y - 300.0:
			var p := Vector3(xx, 0, zz)
			var d: float = snow.get_depth_at(p)
			var ang := rad_to_deg(atan(_slip_grad(p).length()))
			if d >= lo and d <= hi and ang < best_s and gen.get_height(p) > gen.sea_level + 5.0:
				best_s = ang
				best = Vector3(xx, gen.get_height(p), zz)
			zz += 32.0
		xx += 32.0
	if best == Vector3.INF:
		_log("snow road: no snow %.2f to %.2f m deep" % [lo, hi])
		return
	var to_sun := sun.global_basis.z
	var yaw := atan2(to_sun.x, to_sun.z) + deg_to_rad(float(args.get("road-yaw", "90")))
	args["buggy"] = "%.1f,%.1f,%.1f" % [best.x, best.z, rad_to_deg(yaw)]
	var focus := Node3D.new()
	focus.name = "RoadFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	for i in 30:
		await get_tree().physics_frame
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(focus))
	for i in 10:
		await get_tree().physics_frame
	_log("snow road: %.2f m of snow at %s (%.1f deg slope), buggy heading %.0f deg" % [snow.get_depth_at(best), str(best.round()), best_s, rad_to_deg(yaw)])


func _slush_prints() -> void:
	# --slush-prints[=MM]: a thin spring pack (--slush-min/--slush-max, 0.1 to 0.2 m) on gentle ground (under
	# --slush-max-deg, 20), boot prints walked along the view, then MM (120) mm of rain over 10 h at +8 C (snow 0414a40d): over
	# frozen ground (GroundSurface3D frost, ground e7de58cc) the water the pack cannot hold stays as slush in its base
	# and pools in the prints. The camera --slush-eye (1.7) m up at the line's start, looking down it; the line runs
	# --slush-yaw (70) degrees off the sun so the low light rakes across the prints.
	var mm := float(args["slush-prints"]) if String(args["slush-prints"]) != "" else 120.0
	var lo := float(args.get("slush-min", "0.1"))
	var hi := float(args.get("slush-max", "0.2"))
	var wr: Rect2 = gen.get_world_rect()
	var best := Vector3.INF
	var best_s := INF
	var n_snow := 0
	var dmin := INF
	var dmax := 0.0
	var xx := wr.position.x + 300.0
	while xx < wr.end.x - 300.0:
		var zz := wr.position.y + 300.0
		while zz < wr.end.y - 300.0:
			var p := Vector3(xx, 0, zz)
			var d: float = snow.get_depth_at(p)
			var ang := rad_to_deg(atan(_slip_grad(p).length()))
			# The gentlest place in the band; failing that, the gentle place whose depth is nearest the band.
			if d > 0.03:
				n_snow += 1
				dmin = minf(dmin, d)
				dmax = maxf(dmax, d)
			var miss := maxf(lo - d, d - hi)
			if d > 0.03 and ang < float(args.get("slush-max-deg", "20")) and gen.get_height(p) > gen.sea_level + 5.0:
				var score := ang + 1000.0 * maxf(miss, 0.0)
				if score < best_s:
					best_s = score
					best = Vector3(xx, gen.get_height(p), zz)
			zz += 32.0
		xx += 32.0
	if best == Vector3.INF:
		_log("slush prints: no gentle snow (%d snowy cells, %.2f to %.2f m)" % [n_snow, dmin, dmax])
		return
	_log("slush prints: %d snowy cells %.2f to %.2f m; picked %.2f m (score %.1f)" % [n_snow, dmin, dmax, snow.get_depth_at(best), best_s])
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var look := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("slush-yaw", "70"))))
	# On a slope the line runs along the contour (level), the way nearest the sun-set heading.
	var g2 := _slip_grad(best)
	if g2.length() > 0.05:
		var contour := Vector3(-g2.y, 0, g2.x).normalized()
		look = contour if contour.dot(look) >= 0.0 else -contour
	var c3: Camera3D = get_viewport().get_camera_3d()
	var eye := float(args.get("slush-eye", "1.7"))
	c3.global_position = best - look * 1.0 + Vector3.UP * (eye + snow.get_depth_at(best))
	var tgt := best + look * float(args.get("slush-look", "6"))
	c3.look_at(Vector3(tgt.x, gen.get_height(tgt) + snow.get_depth_at(tgt), tgt.z))
	var focus := Node3D.new()
	focus.name = "SlushFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	if prints != null:
		prints.follow_target = prints.get_path_to(c3)
	weather.follow_target = weather.get_path_to(c3)
	for i in 30:
		await get_tree().physics_frame
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(c3))
	for i in 10:
		await get_tree().physics_frame
	var gw = WorldContracts.get_ground()
	var probe := best + look * 4.0
	var frost := Vector3()
	for i in 120:
		frost = gw.frost_at(Vector3(probe.x, gen.get_height(probe), probe.z)) if gw else Vector3()
		if frost.y > 0.0:
			break
		await get_tree().physics_frame
	# The walker: a step every 0.7 m down the view, from 0.5 m ahead out to --slush-walk (14) m.
	get_viewport().disable_3d = true
	var boot := CharacterBody3D.new()
	boot.name = "SlushBoot"
	var bs := CollisionShape3D.new()
	var bx := BoxShape3D.new()
	bx.size = Vector3(0.11, 0.3, 0.29)
	bs.shape = bx
	boot.add_child(bs)
	boot.set_meta("ground_sole", true)
	boot.add_to_group("snow_deform")
	add_child(boot)
	var side := look.cross(Vector3.UP).normalized()
	var n := int(float(args.get("slush-walk", "14")) / 0.7)
	for i in n + 1:
		var q := best + look * (0.5 + 0.7 * i) + side * (0.12 if i % 2 == 0 else -0.12)
		boot.global_basis = Basis.looking_at(-look, Vector3.UP)
		boot.global_position = Vector3(q.x, float(surf.call("get_surface_height_at", q)) + 0.15, q.z)
		for k in 4:
			surf.call("sync_deformation")
			await get_tree().physics_frame
	boot.queue_free()
	await get_tree().physics_frame
	var before := "depth %.3f m, swe %.0f mm" % [snow.get_depth_at(probe), snow.get_swe_at(probe)]
	# The rain: overcast, +8 C, MM over 10 h, then it clears for the shot.
	var cc := weather.cloud_cover
	weather.air_follows_climate = false
	# +8 C at the spot: the weather's temperature is set where it reads 8 C at the snowfield.
	weather.temperature = 8.0
	weather.snap_to_settings()
	weather.temperature = 16.0 - weather.get_air_temperature_at(best)
	weather.rain_rate = mm / 10.0
	weather.cloud_cover = 1.0
	weather.snap_to_settings()
	weather.advance(1.0)
	snow.advance(10.0 * 3600.0)
	weather.rain_rate = 0.0
	weather.cloud_cover = cc
	weather.snap_to_settings()
	weather.advance(1.0)
	get_viewport().disable_3d = false
	for i in 10:
		await get_tree().process_frame
	_log("slush prints: air at the spot %.1f C during the rain" % weather.get_air_temperature_at(best))
	_log("slush prints: %.0f mm rain on %s at %s (%.1f deg), frost %s; after: depth %.3f m, swe %.0f mm, slush %.3f m, wetness %.2f; %d prints; cam %s" % [mm, before, str(best.round()), best_s, str(frost), snow.get_depth_at(probe), snow.get_swe_at(probe), float(snow.call("get_slush_at", probe)), float(snow.call("get_wetness_at", probe)), n + 1, str(c3.global_position.round())])


func _sastrugi() -> void:
	# --sastrugi[=H]: H (12) hours of a gale (--wind, 20 m/s) over the highest gentle (under --sas-max-deg, 20) snow
	# at least 0.4 m deep (SnowCover3D sastrugi, snow 2e127292; wind slab 7325de7b): the cover carves its own ridges
	# along the wind. The wind blows square to the sun (--sas-yaw, 90) so the low light rakes the ridges; the camera
	# --sas-eye (1.7) m up looks downwind along them, a SnowSurface3D round it draws them.
	var wr: Rect2 = gen.get_world_rect()
	var best := Vector3.INF
	var xx := wr.position.x + 200.0
	while xx < wr.end.x - 200.0:
		var zz := wr.position.y + 200.0
		while zz < wr.end.y - 200.0:
			var p := Vector3(xx, 0, zz)
			var h := gen.get_height(p)
			if snow.get_depth_at(p) >= 0.4 and rad_to_deg(atan(_slip_grad(p).length())) < float(args.get("sas-max-deg", "20")) and (best == Vector3.INF or h > best.y):
				best = Vector3(xx, h, zz)
			zz += 32.0
		xx += 32.0
	if best == Vector3.INF:
		_log("sastrugi: no gentle deep snow")
		return
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var wd := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("sas-yaw", "90"))))
	var focus := Node3D.new()
	focus.name = "SastrugiFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	weather.follow_target = weather.get_path_to(focus)
	for i in 30:
		await get_tree().physics_frame
	var keep := weather.wind_speed
	weather.wind_direction = Vector2(wd.x, wd.z)
	weather.wind_speed = float(args.get("wind", "20"))
	weather.air_follows_climate = false
	weather.temperature = float(args.get("sas-temp", "-8"))
	weather.snap_to_settings()
	weather.advance(60.0)
	var hrs := float(args["sastrugi"]) if String(args["sastrugi"]) != "" else 12.0
	snow.advance(hrs * 3600.0)
	weather.wind_speed = float(args.get("sas-after-wind", "6"))
	weather.snap_to_settings()
	weather.advance(60.0)
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = best - wd * 3.0 + Vector3.UP * (float(args.get("sas-eye", "1.7")) + snow.get_depth_at(best))
	var tgt := best + wd * 6.0
	c3.look_at(Vector3(tgt.x, gen.get_height(tgt) + snow.get_depth_at(tgt), tgt.z))
	var surf: Node3D = ClassDB.instantiate("SnowSurface3D")
	surf.name = "SnowSurface"
	add_child(surf)
	surf.set("snow_cover", surf.get_path_to(snow))
	surf.set("follow_target", surf.get_path_to(c3))
	for i in 10:
		await get_tree().physics_frame
	var sv = snow.call("get_sastrugi_at", best + wd * 5.0) if snow.has_method("get_sastrugi_at") else Vector2()
	_log("sastrugi: %.0f h of %.0f m/s over %.2f m of snow at %s (%.0f m), relief %.3f m (%s), cam %s" % [hrs, float(args.get("wind", "20")), snow.get_depth_at(best), str(best.round()), best.y, sv.length(), str(sv), str(c3.global_position.round())])


func _spindrift_cam() -> void:
	# --spindrift-cam[=D]: a gale (--wind, 20 m/s) blowing away from the sun over --sd-snow (30) mm of new snow at
	# --sd-density (100) kg/m3 on the highest ground: spindrift plumes off the lee crests (snow 09b10f00, PhysX Flow).
	# The camera D (140) m downwind of the strongest plume source, --sd-eye (1.7) m up, looks back up at the crest
	# against the low sun (backlit), --sd-frames (90) frames of plume before the shot.
	var wr: Rect2 = gen.get_world_rect()
	var top := Vector3(0, -INF, 0)
	var xx := wr.position.x + 64.0
	while xx < wr.end.x:
		var zz := wr.position.y + 64.0
		while zz < wr.end.y:
			var h := gen.get_height(Vector3(xx, 0, zz))
			if is_finite(h) and h > top.y:
				top = Vector3(xx, h, zz)
			zz += 32.0
		xx += 32.0
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var wind := -sf.rotated(Vector3.UP, deg_to_rad(float(args.get("sd-wind-yaw", "0"))))
	weather.wind_direction = Vector2(wind.x, wind.z)
	weather.wind_speed = float(args.get("wind", "20"))
	weather.air_follows_climate = false
	weather.temperature = float(args.get("sd-temp", "-10"))
	weather.snap_to_settings()
	var focus := Node3D.new()
	focus.name = "SpindriftFocus"
	add_child(focus)
	focus.global_position = top
	snow.follow_target = snow.get_path_to(focus)
	weather.follow_target = weather.get_path_to(focus)
	for i in 20:
		await get_tree().physics_frame
	snow.call("add_snow", float(args.get("sd-snow", "30")), float(args.get("sd-density", "100")), true)
	var srcs: Array = []
	for i in 120:
		await get_tree().physics_frame
		srcs = snow.call("get_spindrift_sources")
		if srcs.size() > 0 and i > 20:
			break
	if srcs.is_empty():
		_log("spindrift: no plume sources near the peak %s at %.0f m/s" % [str(top.round()), weather.wind_speed])
		return
	var src: Vector3 = srcs[0]["position"]
	var d := float(args["spindrift-cam"]) if String(args["spindrift-cam"]) != "" else 140.0
	var cp := src + wind * d
	cp.y = height_at(cp.x, cp.z) + float(args.get("sd-eye", "1.7"))
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = cp
	c3.look_at(src + Vector3.UP * float(args.get("sd-look-up", "15")))
	for i in int(args.get("sd-frames", "90")):
		await get_tree().process_frame
	_log("spindrift: %d sources, %.1f kg/s, strongest at %s (rate %.2f), cam %s %.0f m downwind, wind %s %.0f m/s, sun %.1f deg" % [srcs.size(), float(snow.call("get_spindrift_rate")), str(src.round()), float(srcs[0]["rate"]), str(cp.round()), d, str(weather.wind_direction), weather.wind_speed, rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0)))])


func _gully_cam() -> void:
	# --gully-cam[=D]: the strongest gully on an open soil slope (TerrainGenerator3D.get_gully_at, terrain 01c51a76; the
	# rocks bench's --gully-spot rule: no rock, 15 to 35 degrees over 16 m, banks under 4 m), the camera D (14) m below
	# it down the fall line, --gully-eye (1.7) m up, looking up the channel.
	var wr: Rect2 = gen.get_world_rect()
	var best := -INF
	var found := Vector3.INF
	var down := Vector3.ZERO
	var zz := wr.position.y + 100.0
	while zz < wr.end.y - 100.0:
		var xx := wr.position.x + 100.0
		while xx < wr.end.x - 100.0:
			var p := Vector3(xx, 0, zz)
			var h := gen.get_height(p)
			if gen.get_geology_at(p).g <= 0.05 and h > gen.sea_level + 10.0:
				var gx := gen.get_height(p + Vector3(8, 0, 0)) - gen.get_height(p - Vector3(8, 0, 0))
				var gz := gen.get_height(p + Vector3(0, 0, 8)) - gen.get_height(p - Vector3(0, 0, 8))
				var ny := Vector3(-gx, 16.0, -gz).normalized().y
				if ny >= cos(deg_to_rad(35.0)) and ny <= cos(deg_to_rad(15.0)):
					var gl: Vector2 = gen.get_gully_at(p, ny)
					if gl.x >= 0.5 and gl.y <= 0.3:
						var t := Vector3(-gz, 0, gx).normalized() * 10.0
						var bank := maxf(gen.get_height(p + t), gen.get_height(p - t)) - h
						var score := gl.x - bank * 0.1 - absf(h - gen.sea_level - 80.0) / 400.0
						if bank <= 4.0 and score > best:
							best = score
							found = Vector3(xx, h, zz)
							down = Vector3(-gx, 0, -gz).normalized()
			xx += 6.0
		zz += 6.0
	if found == Vector3.INF:
		_log("gully cam: no gully on open soil")
		return
	var d := float(args["gully-cam"]) if String(args["gully-cam"]) != "" else 14.0
	var cp := found + down * d
	cp.y = height_at(cp.x, cp.z) + float(args.get("gully-eye", "1.7"))
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = cp
	var tgt := found - down * 4.0
	c3.look_at(Vector3(tgt.x, height_at(tgt.x, tgt.z), tgt.z))
	_log("gully cam: gully %s (score %.2f, channel %s), cam %s" % [str(found.round()), best, str(gen.get_gully_at(found, 0.85)), str(cp.round())])


func _dust_devil() -> void:
	# --dust-devil[=S]: one dust devil (PhysXDustDevils3D, sand 1f249588; it stirs the shared wind, 41dbafbc) spawned
	# --devil-ahead (30) m in front of the camera, upwind of the frame's centre so it drifts across, then S (4) s run
	# before the frames (a --clip). --devil-radius (3) m, --devil-swirl (10) m/s.
	var c3: Camera3D = get_viewport().get_camera_3d()
	var dd: Node3D = ClassDB.instantiate("PhysXDustDevils3D")
	dd.name = "DustDevils"
	dd.set("frequency", 0.0)
	add_child(dd)
	dd.set("follow_target", dd.get_path_to(c3))
	for i in 60:
		await get_tree().physics_frame
	var f := -c3.global_basis.z
	f.y = 0.0
	f = f.normalized()
	var w := Vector3(weather.wind_direction.x, 0, weather.wind_direction.y).normalized()
	var secs := float(args["dust-devil"]) if String(args["dust-devil"]) != "" else 4.0
	var at := c3.global_position + f * float(args.get("devil-ahead", "30")) - w * weather.wind_speed * 0.7 * secs
	at.y = height_at(at.x, at.z)
	var idx: int = dd.call("spawn", at, float(args.get("devil-radius", "3")), float(args.get("devil-swirl", "10")))
	var t := 0.0
	while t < secs:
		await get_tree().physics_frame
		t += get_physics_process_delta_time()
	_log("dust devil: spawned %d at %s, %d running after %.1f s" % [idx, str(at.round()), int(dd.call("get_devil_count")), secs])


func _rime_ridge() -> void:
	# --rime-ridge[=H]: H (6) hours of supercooled cloud on the highest ground (WeatherMap3D.rime, world-links round 9):
	# --rime-air (-6) C at the crest, --rime-wind-speed (10) m/s, full low cloud, then it clears for the shot. The camera
	# stands --rime-dist (25) m upwind of the crest, --rime-eye (1.7) m up, looking at the rimed windward faces.
	var wr: Rect2 = gen.get_world_rect()
	var top := Vector3(0, -INF, 0)
	var xx := wr.position.x + 64.0
	while xx < wr.end.x:
		var zz := wr.position.y + 64.0
		while zz < wr.end.y:
			var h := gen.get_height(Vector3(xx, 0, zz))
			if is_finite(h) and h > top.y:
				top = Vector3(xx, h, zz)
			zz += 32.0
		xx += 32.0
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	# The wind blows from beside the sun toward the crest: the rimed side faces the camera, lit from the side.
	var wd := -sf.rotated(Vector3.UP, deg_to_rad(float(args.get("rime-yaw", "70"))))
	var c3: Camera3D = get_viewport().get_camera_3d()
	var d := float(args.get("rime-dist", "25"))
	var cp := top - wd * d
	cp.y = height_at(cp.x, cp.z) + float(args.get("rime-eye", "1.7"))
	c3.global_position = cp
	c3.look_at(top + Vector3.UP * 2.0)
	weather.follow_target = weather.get_path_to(c3)
	weather.wind_direction = Vector2(wd.x, wd.z)
	weather.wind_speed = float(args.get("rime-wind-speed", "10"))
	weather.air_follows_climate = false
	weather.temperature = 0.0
	weather.snap_to_settings()
	weather.temperature = float(args.get("rime-air", "-6")) - (weather.get_air_temperature_at(top) - 0.0)
	weather.humidity = 1.0
	var cc := weather.cloud_cover
	weather.cloud_cover = 1.0
	weather.cloud_type = 0.0
	weather.snap_to_settings()
	var hrs := float(args["rime-ridge"]) if String(args["rime-ridge"]) != "" else 6.0
	for i in int(hrs * 6.0):
		weather.advance(600.0)
		await get_tree().physics_frame
	var rv: Vector3 = weather.get_rime_at(top - wd * 2.0) if weather.has_method("get_rime_at") else Vector3()
	weather.cloud_cover = cc
	weather.humidity = 0.6
	weather.wind_speed = float(args.get("wind", "4"))
	weather.snap_to_settings()
	weather.advance(60.0)
	_log("rime ridge: %.0f h of cloud at %.1f C on the crest %s (%.0f m), rime %s, cam %s" % [hrs, weather.get_air_temperature_at(top), str(top.round()), top.y, str(rv), str(cp.round())])


func _fog_sea(key := "fog-sea") -> void:
	# --fog-sea[=H]: a clear calm night of H (6) hours before the shot (WeatherMap3D.valley_fog, 2648f0f3): the cold
	# pools fill with radiation fog, then the clock returns to --time for the dawn view over the fog top. --fog-wind
	# (0.5) m/s, --fog-humidity (0.95), --fog-air (6) C. Logs the fog top along the camera's view.
	# --late-frost[=H]: the same night in spring, drier (0.6) and colder (--fog-air 2 C): the hollows pool below -2 C
	# and the young leaves out there brown (WeatherMap3D.late_frost; trees band near meshes by local season, 52d3db3f).
	var frost := key == "late-frost"
	var hrs := float(args[key]) if String(args[key]) != "" else (8.0 if frost else 6.0)
	var t_shot := clock.time_of_day
	weather.air_follows_climate = false
	weather.temperature = float(args.get("fog-air", "2" if frost else "6"))
	weather.humidity = float(args.get("fog-humidity", "0.6" if frost else "0.95"))
	weather.wind_speed = float(args.get("fog-wind", "0.5"))
	weather.cloud_cover = 0.0
	weather.valley_fog = true
	weather.snap_to_settings()
	for i in int(hrs * 6.0):
		clock.time_of_day = fposmod(t_shot - hrs + i / 6.0, 24.0)
		weather.advance(600.0)
		await get_tree().physics_frame
	clock.time_of_day = t_shot
	weather.advance(60.0)
	var c3: Camera3D = get_viewport().get_camera_3d()
	var f := -c3.global_basis.z
	var tops := []
	for k in 6:
		var p := c3.global_position + Vector3(f.x, 0, f.z).normalized() * (150.0 + 250.0 * k)
		p.y = height_at(p.x, p.z)
		tops.append("%.0f m: ground %.0f top %.0f" % [150.0 + 250.0 * k, p.y, weather.get_valley_fog_top_at(p)])
	_log("fog sea: %.0f h calm night, cam at %.0f m; %s" % [hrs, c3.global_position.y, ", ".join(tops)])
	if frost and weather.has_method("get_leaf_frost_at"):
		var lp := c3.global_position + Vector3(f.x, 0, f.z).normalized() * 20.0
		_log("late frost: at the camera %s, 20 m on %s (severity, forcing, days)" % [str(weather.get_leaf_frost_at(c3.global_position)), str(weather.get_leaf_frost_at(lp))])


func _lodging() -> void:
	# --lodging[=H]: H (1) hours of a wet summer gale (--lodge-wind 18 m/s at 10 m, --lodge-rain 15 mm/h) from behind
	# the camera's left over every GrassField3D (GrassField3D.lodging, vegetation 2b052f8e): the tall grass laid down in
	# swirled patches along the wind. Then the rain stops and the wind drops to --wind for the shot.
	var hrs := float(args["lodging"]) if String(args["lodging"]) != "" else 1.0
	var c3: Camera3D = get_viewport().get_camera_3d()
	var f := -c3.global_basis.z
	var wd := Vector3(f.x, 0, f.z).normalized().rotated(Vector3.UP, deg_to_rad(float(args.get("lodge-yaw", "30"))))
	var keep := [weather.wind_speed, weather.rain_rate, weather.cloud_cover]
	weather.wind_direction = Vector2(wd.x, wd.z)
	weather.wind_speed = float(args.get("lodge-wind", "18"))
	weather.rain_rate = float(args.get("lodge-rain", "15"))
	weather.cloud_cover = 0.95
	weather.snap_to_settings()
	weather.advance(60.0)
	var n := 0
	for g in find_children("*", "GrassField3D", true, false):
		if g.has_method("advance_lodging"):
			g.call("advance_lodging", hrs * 3600.0)
			n += 1
	weather.wind_speed = keep[0]
	weather.rain_rate = 0.0
	weather.cloud_cover = keep[2]
	weather.snap_to_settings()
	weather.advance(60.0)
	var probe := c3.global_position + Vector3(f.x, 0, f.z).normalized() * 8.0
	var lv: Vector3 = grass.call("get_lodging_at", probe) if grass != null and grass.has_method("get_lodging_at") else Vector3()
	_log("lodging: %.1f h of gale over %d grass fields, lodging at 8 m %s" % [hrs, n, str(lv)])


func _lens_cam() -> void:
	# --lens-cam[=D]: wave clouds (lenticulars, weather 3876c353/47e03723) over the highest crest. The camera stands D
	# (12000) m downwind of the crest, --lens-up (600) m over the ground or sea, looking back upwind --lens-pitch (30)
	# degrees up; the wind (--wind, --humidity, --mid-cover from the command line) blows toward it, turned so the view
	# sits --lens-sun-off (55) degrees off the sun's azimuth (the sun out of frame beside the lenses). --fov 75 default
	# puts the crest in the lower third.
	var wr: Rect2 = gen.get_world_rect()
	var top := Vector3(0, -INF, 0)
	var xx := wr.position.x + 64.0
	while xx < wr.end.x:
		var zz := wr.position.y + 64.0
		while zz < wr.end.y:
			var h := gen.get_height(Vector3(xx, 0, zz))
			if is_finite(h) and h > top.y:
				top = Vector3(xx, h, zz)
			zz += 64.0
		xx += 64.0
	var to_sun := sun.global_basis.z
	var sf := Vector3(to_sun.x, 0, to_sun.z).normalized()
	var look := sf.rotated(Vector3.UP, deg_to_rad(float(args.get("lens-sun-off", "55"))))
	# --lens-downwind: the camera between the crest and the lenses, looking downwind at them (the crest behind).
	var downwind := args.has("lens-downwind")
	weather.wind_direction = Vector2(look.x, look.z) if downwind else Vector2(-look.x, -look.z)
	if weather.has_method("rebake_terrain_wind"):
		weather.rebake_terrain_wind()
	var d := float(args["lens-cam"]) if String(args["lens-cam"]) != "" else 12000.0
	var cp := Vector3(top.x, 0, top.z) + (look * d if downwind else -look * d)
	var g := gen.get_height(cp)
	var base := maxf(g if is_finite(g) else gen.sea_level, gen.sea_level)
	cp.y = base + float(args.get("lens-up", "600"))
	var c3: Camera3D = get_viewport().get_camera_3d()
	if not args.has("fov"):
		c3.fov = 75.0
	var pitch := deg_to_rad(float(args.get("lens-pitch", "30")))
	c3.global_position = cp
	c3.look_at(cp + look * cos(pitch) + Vector3.UP * sin(pitch))
	if weather.has_method("set_follow_target") or "follow_target" in weather:
		weather.follow_target = weather.get_path_to(c3)
	for i in 120:
		await get_tree().physics_frame
	# Where the lenses are along the view: the wave cloud's extinction on rays from the camera, 0 to 20 km.
	var hits := ""
	for el in [10.0, 20.0, 30.0, 40.0]:
		var ray := look * cos(deg_to_rad(el)) + Vector3.UP * sin(deg_to_rad(el))
		var s := 500.0
		while s < 20000.0:
			if float(weather.call("get_wave_cloud_density_at", cp + ray * s)) > 0.0:
				hits += " %.0f deg: %.1f km at %.0f m;" % [el, s / 1000.0, (cp + ray * s).y]
				break
			s += 250.0
	# Top-down: the wave cloud's optical depth over 40 km round the crest, the densest column and the waves' height.
	var slab: Vector4 = weather.call("get_wave_cloud_slab") if weather.has_method("get_wave_cloud_slab") else Vector4()
	var bt := 0.0
	var bq := Vector3()
	var eta := 0.0
	for j in 21:
		for i in 21:
			var q := Vector3(top.x - 20000.0 + i * 2000.0, 0.0, top.z - 20000.0 + j * 2000.0)
			eta = maxf(eta, absf(float(weather.call("get_mountain_wave_at", Vector3(q.x, 4500.0, q.z)))))
			var tau := 0.0
			for y in range(2500, 9000, 500):
				tau += float(weather.call("get_wave_cloud_density_at", Vector3(q.x, y, q.z))) * 500.0
			if tau > bt:
				bt = tau
				bq = q
	_log("lens cam: wave slabs %s, max wave %.0f m, densest column tau %.2f at %s (%.1f km from the camera)" % [str(slab), eta, bt, str(bq.round()), Vector2(bq.x - cp.x, bq.z - cp.z).length() / 1000.0])
	_log("lens cam: crest %s (%.0f m), cam %s, wind %s %.1f m/s, sun elevation %.1f deg, first wave cloud:%s" % [str(top.round()), top.y, str(cp.round()), str(weather.wind_direction), weather.wind_speed, rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0))), hits if hits != "" else " none in 20 km"])


func _sea_effect() -> void:
	# --sea-effect[=D]: cold air (--air-temp, e.g. -8) off the open sea (--water-temp, e.g. 6), the wind (--wind) along
	# --sea-wind-deg (0: along the coast, toward +x) so the fetch runs down the shore; the camera on the shore at the
	# map's sea edge, --sea-cam-h (40) m up, looking --sea-look-deg (60) off the wind out to sea. Logs the fetch and the
	# lake-effect field round it.
	var wdeg := deg_to_rad(float(args.get("sea-wind-deg", "0")))
	var wd := Vector2(cos(wdeg), sin(wdeg))
	weather.wind_direction = wd
	var wr: Rect2 = gen.get_world_rect()
	var cx := wr.position.x + wr.size.x * float(args.get("sea-cam-x", "0.85"))
	var cz := wr.position.y
	# Walk down the map toward the sea (+z) to the shoreline.
	var z := wr.position.y + wr.size.y * 0.5
	while z < wr.end.y - 8.0 and gen.get_height(Vector3(cx, 0, z)) > gen.sea_level + 2.0:
		z += 8.0
	cz = z - 20.0
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = Vector3(cx, maxf(gen.get_height(Vector3(cx, 0, cz)), gen.sea_level) + float(args.get("sea-cam-h", "40")), cz)
	var w3 := Vector3(wd.x, 0, wd.y)
	var look := w3.rotated(Vector3.UP, -deg_to_rad(float(args.get("sea-look-deg", "60"))))
	c3.look_at(c3.global_position + look + Vector3.UP * sin(deg_to_rad(float(args.get("sea-pitch", "8")))))
	weather.follow_target = weather.get_path_to(c3)
	for i in 200:
		await get_tree().physics_frame
		if weather.has_method("is_lake_effect_ready") and weather.call("is_lake_effect_ready") and i > 30:
			break
	var line := ""
	for k in 9:
		var q := c3.global_position + look * (2000.0 * k)
		line += " %.0f km: fetch %.0f, cloud %.2f, snow %.2f;" % [2.0 * k, float(weather.call("get_lake_fetch_at", q)), float(weather.call("get_lake_effect_at", q)), weather.get_snowfall_at(q)]
	_log("sea effect: cam %s, wind %s %.1f m/s, air %.1f C, water %.1f C, ready %s, along the view:%s" % [str(c3.global_position.round()), str(wd), weather.wind_speed, weather.temperature, float(weather.call("get_water_temperature_setting")), str(weather.call("is_lake_effect_ready")), line])


func _snow_tracks(surf: Node3D, c3: Camera3D, fwd: Vector3) -> void:
	# --snow-tracks: a walker's boot prints and a car's two ruts across the spring snow in front of the camera, pressed
	# by the snow's own movers (a sole-flagged box on a CharacterBody3D, wheels by their deform_wheel_radius meta), so
	# the pack's wetness decides the depth (rotten snow postholes, snow 51d3fa6b) and any slush at the base shows in
	# the holes. --tracks-hours=H (0) first runs the snow H hours on in today's weather (an afternoon's melt).
	var hrs := float(args.get("tracks-hours", "0"))
	if hrs > 0.0:
		snow.advance(hrs * 3600.0)
	var f := Vector3(fwd.x, 0, fwd.z).normalized()
	var side := f.cross(Vector3.UP).normalized()
	var base := Vector3(c3.global_position.x, 0, c3.global_position.z)
	var probe := base + f * 6.0
	var gw = WorldContracts.get_ground()
	_log("snow tracks: at %s depth %.2f m, density %.0f, wetness %.2f, slush %.3f m, frost %s, rotten share %.2f" % [str(probe.round()), snow.get_depth_at(probe), snow.get_density_at(probe), snow.call("get_wetness_at", probe), snow.call("get_slush_at", probe), str(gw.frost_at(Vector3(probe.x, gen.get_height(probe), probe.z))) if gw else "none", ClassDB.class_call_static("SnowSurface3D", "rotten_share", float(snow.call("get_wetness_at", probe)))])
	get_viewport().disable_3d = true
	# The walker: from the near left across to the far right, a step every 0.7 m, feet 0.12 m off the line.
	var boot := CharacterBody3D.new()
	boot.name = "TrackBoot"
	var bs := CollisionShape3D.new()
	var bx := BoxShape3D.new()
	bx.size = Vector3(0.11, 0.3, 0.29)
	bs.shape = bx
	boot.add_child(bs)
	boot.set_meta("ground_sole", true)
	boot.add_to_group("snow_deform")
	add_child(boot)
	var a := base + f * float(args.get("boots-near", "2.5")) - side * 1.5
	var b := base + f * float(args.get("boots-far", "16")) + side * 2.5
	var dir := (b - a).normalized()
	var bside := dir.cross(Vector3.UP).normalized()
	var n := int(a.distance_to(b) / 0.7)
	for i in n + 1:
		var q := a + dir * (0.7 * i) + bside * (0.12 if i % 2 == 0 else -0.12)
		boot.global_basis = Basis.looking_at(-dir, Vector3.UP)
		boot.global_position = Vector3(q.x, surf.call("get_surface_height_at", q) + 0.15, q.z)
		for k in 4:
			surf.call("sync_deformation")
			await get_tree().physics_frame
	boot.queue_free()
	# The car: two tyres 1.6 m apart on a line from the near right to the far left, 0.1 m a frame.
	var tyres := []
	for k in 2:
		var w := Node3D.new()
		w.name = "TrackTyre%d" % k
		w.set_meta("deform_wheel_radius", 0.36)
		w.set_meta("deform_wheel_width", 0.22)
		w.set_meta("snow_mass", float(args.get("tyre-mass", "400")))
		w.add_to_group("snow_deform")
		add_child(w)
		tyres.append(w)
	var ta := base + f * float(args.get("ruts-near", "4")) + side * 3.5
	var tb := base + f * float(args.get("ruts-far", "40")) - side * 6.0
	var tdir := (tb - ta).normalized()
	var tside := tdir.cross(Vector3.UP).normalized()
	var m := int(ta.distance_to(tb) / 0.1)
	for i in m + 1:
		for k in 2:
			var q: Vector3 = ta + tdir * (0.1 * i) + tside * ((k - 0.5) * 1.6)
			var w: Node3D = tyres[k]
			w.global_basis = Basis.looking_at(-tdir, Vector3.UP)
			w.global_position = Vector3(q.x, surf.call("get_surface_height_at", q) + 0.36, q.z)
		surf.call("sync_deformation")
		await get_tree().physics_frame
	for w in tyres:
		w.queue_free()
	get_viewport().disable_3d = false
	for i in 10:
		await get_tree().process_frame
	var hole := a + dir * 0.7 * float(n / 2)
	_log("snow tracks: %d boot steps, posthole %.3f m at %s; %d m of ruts" % [n + 1, snow.get_depth_at(hole) - (surf.call("get_surface_height_at", hole) - gen.get_height(hole)), str(hole.round()), int(ta.distance_to(tb))])


func _powder_avalanche() -> void:
	var av = snow.get_avalanches()
	if av == null:
		_log("powder avalanche: no avalanche node on the snow cover")
		return
	av.natural_release = false
	_log("powder avalanche: %d natural releases before loading" % (int(av.get_release_count()) if av.has_method("get_release_count") else -1))
	# A storm slab on an older bonded pack (as on the snow bench): it breaks away whole.
	# world=true (snow 6f0f0a09): the slab everywhere, not only on the near square.
	snow.call("add_snow", 90.0, 150.0, true)
	snow.advance(8.0 * 24.0 * 3600.0)
	await get_tree().physics_frame
	snow.call("add_snow", 90.0, 90.0, true)
	await get_tree().physics_frame
	var wr: Rect2 = gen.get_world_rect()
	var stp := float(args.get("av-step", "40"))
	var to_sun := sun.global_basis.z
	var sun_h := Vector2(to_sun.x, to_sun.z).normalized()
	var best := Vector3.INF
	var best_fall := Vector2.ZERO
	var best_score := -INF
	var n_steep := 0
	var steep_snow := 0.0
	# --av-margin (400 m): faces this far in from the map's edge, so the camera has ground under it.
	var avm := maxf(stp, float(args.get("av-margin", "400")))
	var xx := wr.position.x + avm
	while xx < wr.end.x - avm:
		var zz := wr.position.y + avm
		while zz < wr.end.y - avm:
			var p := Vector3(xx, 0, zz)
			var g := _slip_grad(p)
			var ang := rad_to_deg(atan(g.length()))
			if ang >= float(args.get("av-min-deg", "32")) and ang <= 45.0:
				n_steep += 1
				steep_snow = maxf(steep_snow, snow.get_depth_at(p))
			if ang >= float(args.get("av-min-deg", "32")) and ang <= 45.0 and snow.get_depth_at(p) >= float(args.get("av-min-snow", "0.3")):
				var fall := -g.normalized()
				var score := (1.0 - absf(fall.dot(sun_h))) * 2.0 + gen.get_height(p) / 1000.0 + minf(snow.get_depth_at(p), 1.5)
				if score > best_score:
					best_score = score
					best = Vector3(xx, gen.get_height(p), zz)
					best_fall = fall
			zz += stp
		xx += stp
	if best == Vector3.INF:
		var deepest := 0.0
		var xs := wr.position.x + stp
		while xs < wr.end.x - stp:
			var zs := wr.position.y + stp
			while zs < wr.end.y - stp:
				deepest = maxf(deepest, snow.get_depth_at(Vector3(xs, 0, zs)))
				zs += stp * 4.0
			xs += stp * 4.0
		_log("powder avalanche: no steep face with snow on it (deepest snow %.2f m; %d faces in range, the most snow on one %.2f m; %d releases)" % [deepest, n_steep, steep_snow, int(av.get_release_count()) if av.has_method("get_release_count") else -1])
		return
	var f3 := Vector3(best_fall.x, 0, best_fall.y)
	var side := f3.cross(Vector3.UP).normalized()
	if Vector2(side.x, side.z).dot(sun_h) > 0.0:
		side = -side # the camera on the face's shaded side looks across it into the light: the cloud side- and back-lit
	var dist := float(args.get("av-dist", "260"))
	var cp := best + f3 * dist * 0.55 + side * dist * 0.8
	if args.has("av-backlit"):
		# --av-backlit: the camera down-sun of the slide's run-out, looking toward the sun, so the cloud is backlit
		# (bright top rim, shaded front) with the slope behind its lower half.
		var run := best + f3 * 70.0
		cp = run - Vector3(sun_h.x, 0, sun_h.y) * dist
	cp.x = clampf(cp.x, wr.position.x + 60.0, wr.end.x - 60.0)
	cp.z = clampf(cp.z, wr.position.y + 60.0, wr.end.y - 60.0)
	var gh: float = gen.get_height(cp)
	if is_nan(gh):
		gh = height_at(cp.x, cp.z)
	cp.y = gh + float(args.get("av-cam-h", "30"))
	var look := best + f3 * 70.0 + Vector3.UP * 15.0
	var c3: Camera3D = get_viewport().get_camera_3d()
	c3.global_position = cp
	c3.look_at(look)
	c3.far = maxf(c3.far, 4000.0)
	_log("powder avalanche: release at %s (%.0f deg, %.2f m snow), fall %s; cam %.0f,%.0f,%.0f look %.0f,%.0f,%.0f" % [str(best.round()), rad_to_deg(atan(_slip_grad(best).length())), snow.get_depth_at(best), str(best_fall), cp.x, cp.y, cp.z, look.x, look.y, look.z])
	if args.has("av-probe"):
		get_tree().quit()
		return
	# Avalanches run on the snow's near square only: move it onto the release (it follows this marker).
	var focus := Node3D.new()
	focus.name = "AvalancheFocus"
	add_child(focus)
	focus.global_position = best
	snow.follow_target = snow.get_path_to(focus)
	var fw := 0
	while fw < 600 and not (snow.get_map_rect() as Rect2).has_point(Vector2(best.x, best.z)):
		await get_tree().physics_frame
		fw += 1
	_log("powder avalanche: near square %s over the release after %d frames, %.2f m snow there" % [str(snow.get_map_rect()), fw, snow.get_depth_at(best)])
	Engine.max_physics_steps_per_frame = 1 # Flow steps once a frame: one tick a frame keeps the gas with the slide
	var pflow: Node = av.get_powder_cloud().get_flow()
	if pflow:
		pflow.set("scattering_resolution", 0)
	var ok: bool = av.trigger(best, float(args.get("av-radius", "60")))
	var frames := 0
	var at_s := float(args.get("powder-at", "9"))
	while av.is_running() and frames < int(at_s * Engine.physics_ticks_per_second):
		await get_tree().physics_frame
		frames += 1
	_log("powder avalanche: trigger %s, %.1f t up, %.1f t in the air, front %.1f m/s, after %d frames" % [str(ok), av.get_powder_taken() / 1000.0, av.get_suspended_mass() / 1000.0, av.get_max_speed(), frames])
	if args.has("dt-before") and pflow != null and "diffuse_transmission" in pflow:
		# --dt-before=PATH: the same moment with the gas's raster diffuse transmission off (PhysX 1529e96b), saved
		# first, then on again for the main still (the scene paused, so the cloud holds).
		process_mode = Node.PROCESS_MODE_ALWAYS
		get_tree().paused = true
		pflow.set("diffuse_transmission", false)
		for i in int(args.get("dt-frames", "12")):
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(args["dt-before"])
		_log("saved %s (diffuse transmission off)" % args["dt-before"])
		pflow.set("diffuse_transmission", true)


# --lightning-cam (with --lightning): the struck spruce nearest the first view, the camera --lightning-dist m (6) from
# it square to the sun (side-lit, so the pale strip and the splinters read against the dark bark), eye 1.7 m, looking
# at the stem --lightning-look m up (2).
func _frame_colonnade() -> void:
	# --colonnade-cam: along a row of spruce on stilts over a vanished nurse log (trees 5az), eye height, a few degrees
	# off the row's axis; picks the row nearest the camera that runs most square to the sun, so the light is from the side.
	var c3: Camera3D = get_viewport().get_camera_3d()
	var to_sun := sun.global_basis.z
	var sun_flat := Vector3(to_sun.x, 0.0, to_sun.z).normalized()
	var pts: Array[Vector3] = []
	for i in veg.biome.species.size():
		var sp := veg.biome.species[i] as Resource
		if sp != null and sp.has_meta("colonnade"):
			for tp in veg.get_placements(i):
				if tp.distance_to(c3.global_position) < 400.0:
					pts.append(tp)
	var best_score := -INF
	var a := Vector3.INF
	var b := Vector3.INF
	for p0 in pts:
		var far := p0
		var n := 0
		for q in pts:
			var dq := Vector2(q.x - p0.x, q.z - p0.z).length()
			if dq > 0.5 and dq < 11.0:
				n += 1
				if dq > Vector2(far.x - p0.x, far.z - p0.z).length():
					far = q
		if n < 3:
			continue
		var dir := Vector3(far.x - p0.x, 0.0, far.z - p0.z).normalized()
		var score := absf(dir.cross(sun_flat).y) * 3.0 + n * 0.3 - p0.distance_to(c3.global_position) * 0.01
		if score > best_score:
			best_score = score
			a = p0
			b = far
	if a == Vector3.INF:
		_log("colonnade: no row of %d stilted spruce found" % pts.size())
		return
	var dir2 := Vector3(b.x - a.x, 0.0, b.z - a.z).normalized()
	var side := dir2.cross(Vector3.UP).normalized()
	var cp := a - dir2 * float(args.get("colonnade-back", "6")) + side * float(args.get("colonnade-side", "1.8"))
	cp.y = height_at(cp.x, cp.z) + float(args.get("colonnade-h", "1.5"))
	c3.global_position = cp
	var mid := (a + b) * 0.5
	c3.look_at(Vector3(mid.x, height_at(mid.x, mid.z) + 1.2, mid.z))
	var n2 := 0
	await get_tree().process_frame
	get_viewport().disable_3d = true
	while not cover.is_stream_idle() and n2 < 600:
		await get_tree().process_frame
		n2 += 1
	get_viewport().disable_3d = false
	if veg.streaming:
		await _forest_settle()
	_log("colonnade cam %.1f,%.1f,%.1f along the row %s to %s (sun across %.2f)" % [cp.x, cp.y, cp.z, str(a.round()), str(b.round()), absf(dir2.cross(sun_flat).y)])

func _frame_host() -> void:
	# --host-cam: under one mistletoe host's crown, looking up into it against the sky (sun behind the camera).
	var c3: Camera3D = get_viewport().get_camera_3d()
	var to_sun := sun.global_basis.z
	var sun_flat := Vector3(to_sun.x, 0.0, to_sun.z).normalized()
	var best := Vector3.INF
	for i in veg.biome.species.size():
		var sp := veg.biome.species[i] as Resource
		if sp == null or not String(sp.resource_name).contains("mistletoe host"):
			continue
		for tp in veg.get_placements(i):
			if best == Vector3.INF or tp.distance_to(c3.global_position) < best.distance_to(c3.global_position):
				best = tp
	if best == Vector3.INF:
		_log("host cam: no mistletoe host placed")
		return
	# --host-backlit: the camera on the far side from the sun, the low sun behind the crown (ice storm frames).
	var cp := best + sun_flat * float(args.get("host-dist", "14")) * (-1.0 if args.has("host-backlit") else 1.0)
	cp.y = height_at(cp.x, cp.z) + 1.7
	c3.global_position = cp
	c3.look_at(Vector3(best.x, height_at(best.x, best.z) + float(args.get("host-look", "12")), best.z))
	var n := 0
	await get_tree().process_frame
	get_viewport().disable_3d = true
	while not cover.is_stream_idle() and n < 600:
		await get_tree().process_frame
		n += 1
	get_viewport().disable_3d = false
	if veg.streaming:
		await _forest_settle()
	_log("host cam %.1f,%.1f,%.1f at the mistletoe host %s" % [cp.x, cp.y, cp.z, str(best.round())])

func _frame_struck() -> void:
	var c3: Camera3D = get_viewport().get_camera_3d()
	var to_sun := sun.global_basis.z
	var sun_flat := Vector3(to_sun.x, 0.0, to_sun.z).normalized()
	var best := Vector3.INF
	for i in veg.biome.species.size():
		var sp := veg.biome.species[i] as Resource
		if sp == null or not String(sp.resource_name).contains("lightning"):
			continue
		for tp in veg.get_placements(i):
			if best == Vector3.INF or tp.distance_to(c3.global_position) < best.distance_to(c3.global_position):
				best = tp
	if best == Vector3.INF:
		_log("lightning: no struck spruce placed")
		return
	var side := sun_flat.cross(Vector3.UP).normalized()
	var cp := best + side * float(args.get("lightning-dist", "6"))
	cp.y = height_at(cp.x, cp.z) + 1.7
	c3.global_position = cp
	c3.look_at(Vector3(best.x, height_at(best.x, best.z) + float(args.get("lightning-look", "2")), best.z))
	var n := 0
	await get_tree().process_frame
	get_viewport().disable_3d = true
	while not cover.is_stream_idle() and n < 600:
		await get_tree().process_frame
		n += 1
	get_viewport().disable_3d = false
	if veg.streaming:
		await _forest_settle()
	_log("lightning cam %.1f,%.1f,%.1f at the struck spruce %s" % [cp.x, cp.y, cp.z, str(best.round())])

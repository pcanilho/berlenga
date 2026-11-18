extends Node3D
# GroundSurface3D bench (docs/ideas/ground-deform.md): footprints in sand, soil, mud and leaf litter.
#
#   --test              print the checks and quit:
#                         boot sinkage by material against Bekker with Wong's values
#                         volume kept through a press and through slumping
#                         dry sand slumps to its repose angle, damp sand keeps steep walls
#                         mud creeps back slowly; an undisturbed 50 degree slope does not move
#                         litter adds to the print; bare rock does not print; turned ground dries
#   --test=soil         soft soil under wheels and feet (needs the PhysX build):
#                         a wheel's sinkage, traction and resistance against Bekker, Wong and Janosi-Hanamoto
#                         a 4x4 on dry sand, damp sand and firm ground; an aired-down tyre; a bogged car
#                         walker footing: effort by print depth, sliding on a slope past the repose
#   --test=grains       sand thrown out of the ground and landing again (needs the PhysX build and a GPU):
#                         a kick digs its volume and lands it downrange; volume kept end to end
#                         a 4x4 floored on dry sand throws its tread's sand behind it
#   --test=wind         wind ripples (stage 3): wavelength and height along the wind, crests across it,
#                         a wind shift turns them, none under the threshold or when damp, a print wipes
#                         them and they grow back, the layer keeps its sand; blowing grains: speed, hop,
#                         streamer cover, flux at 12 m/s, none under the threshold, drawn
#   --test=water        sand and water (stage 4): bed load against the Shields threshold, pore water filling
#                         under water and draining after, a damp wall holding where dry and saturated slump,
#                         a print rounded off under flowing water and kept under slow water, volume kept
#   --test=shelter      dune grass (wind_shelters): Raupach drag partition, flux dies among the plants, sand lands
#   --test=near         the finer near field (docs/ideas/mud.md, stage 0; needs a GPU): a boot print's walls
#                         sharper than on the main field, volume kept, dry sand still slumps to its repose
#                         in 3 s, a print made outside the near field is there when the field arrives
#   --test=mud          deep mud (docs/ideas/mud.md, stage 1): Bingham numbers (O'Brien and Julien 1988), a painted
#                         mud layer only counts when wet, a boot sinks deeper in slurry than in stiff mud, a rut
#                         in stiff mud keeps its walls and one in slurry closes (volume kept); with the PhysX
#                         build a 4x4 sinks to its axles and bogs in slurry and crawls through stiff mud
#   --near=0            the near field off (A/B for any view)
#   --test=clog         mud on wheels and the belly (mud stage 3): sticky clay, a clogging tread grips less and is
#                         flung off when spun fast on dry ground; a low car high-centres in slurry and ploughs a trough
#   --view=bodymud      mud on a car's body (mud stage 3b): a dressed 4x4 crawls through stiff clay, then is floored
#                         so its tyres throw the clay up the arches; a close frame on the rear wheel and side.
#                         --cam=x,y,z,tx,ty,tz in the car's frame (default from its right rear); --spray: the
#                         frame mid-throw, the clods in the air
#   --test=splash       a 4x4 through rut water past Froude 1 (mud stage 4c): sheets thrown, landed back, volume kept
#   --test=ruts         kept ruts (mud round 3): a print back after the field drives 600 m off and in a new node
#   --meadow            --view=ruts: the patch drawn with a TerrainMaterial3D under a full sward (its grass globals set by hand)
#   --test=runoff       runoff channels: a falling tide's soaked beach seeps, the water cuts rills down the face
#                         and lays the sand at the waterline; volume kept; cost of the step
#   --test=drift        a car slid through a turn on dry sand: sand thrown sideways, the sidewall's push, a berm
#                         on the outside; dust: none from damp sand, a plume from dry, Stokes settling, wind drift
#   --view=trail        a walker, a rolling ball and a wheel cross the ground; --ground=KIND picks it:
#                       dry_sand, damp_sand, soil, mud, litter (default soil)
#   --mud=depth,moisture paint a mud layer for --view=ruts (stage 1; the car starts in the frame and drives 12 s)
#   --water=mm   --view=ruts: then rain that much (20 mm/h, sped up) and let it stand an hour (mud stage 2)
#   --dry        --view=ruts with --water: then days of sun, so the soaked ruts dry and crack (stage 2b);
#                  --dry=frames for part of it (600 is all), --dry=0 until the deepest pool starts to crust
#   --view=ruts         a 4x4 drives a curve through the ground and leaves (needs the PhysX build); on dry sand
#                       its tyres are aired down to 60 kPa; --ground=KIND as above (default damp_sand)
#   --view=roost        a 4x4 floored on dry sand throws sand behind it (needs the PhysX build and a GPU)
#   --view=swash        boot prints in sand, the nearer ones after a minute under flowing water (sand stage 4)
#   --view=rills        runoff channels: a beach face 84 minutes after a falling tide (scaled), the seepage's rills
#                         and the sand they laid at the foot; --runoff=0 for the A/B
#   --view=grains       dry sand from 40 cm under a low sun behind the camera: grain colours and glints (sand_glints;
#                       --glints=0 for the A/B)
#   --view=shore        a real sea's swash edge on a beach: the water sheet, damp sand draining behind it and a
#                       row of boot prints crossing the line, rounded in the wash, sharp on the dry sand (stage 4)
#                       --water=uniform:value sets one of the sea's shader uniforms (repeatable); --ebb=from,to
#                       lowers the run-up over the run, as a falling tide, leaving a ladder of swash marks;
#                       --prints=0 leaves out the row of boot prints; --sea=name:value sets a sea property
#                       --sun=pitch,yaw sun direction in degrees (default -20,200: low, from over the sea)
#   --view=ripples      wind ripples on dry sand at a low sun, sand blowing over them; --wind=m/s (default 9)
#   --cam=x,y,z,tx,ty,tz camera for --view=trail, --view=roost and --view=ripples
#   --sun=pitch,yaw[,exposure] sun angles in degrees for --view=trail (default -18,140), also --view=rills (default -14,75), and the exposure
#   --prop=name:value   set one of the surface's properties (repeatable)
#   --defaults          keep every shipped setting on (otherwise each check or view turns on only its own)
#   --raster            raster only (default path traced)
#   --pair              --view=bodymud, ruts and wade: save the raster frame as <out>_raster.png, then the path traced one, from one run
#   --view=wade         a 4x4 drives along two ruts brimming with ground water (mud stage 4c): the bow wave and wake;
#                         --cam=x,y,z,tx,ty,tz in the world
#   --out=path.png      save the frame and quit
#   --spp=N --bounces=N path tracing samples and bounces (default 16, 3)
#   --gpu=0             the field on the CPU (default: compute shaders where there is a GPU; --headless
#                       always runs it on the CPU, so --test under a window and headless compares both)

const HeightScript := preload("res://height.gd")

var view := "trail"
var kind := "soil"
var raster := false
var test := false
var splash_test := false
var soil_test := false
var grains_test := false
var wind_test := false
var water_test := false
var shelter_test := false
var runoff_test := false
var drift_test := false
var near_test := false
var mud_test := false
var clog_test := false
var puddle_test := false
var crack_test := false
var legs_test := false
var muddy_test := false
var tracked_test := false
var ruts_test := false
var pudhelp_test := false
var cracks_on := false # _setup turns desiccation_cracks on (the kept-cracks check needs it from the first lattice)
var ruts_scale := 1.0 # kept ruts' world seconds per real second while away
var ruts_on := false # kept ruts (written to user://): off on the bench, or one run's prints turn up in the next
var near_on := true
var out := ""
var spp := 16
var gpu := true
var bounces := 3
var cam_arg := PackedFloat32Array()
var wind_arg := 9.0
var sun_arg := PackedFloat32Array()
var mud_arg := PackedFloat32Array()
var water_arg := 0.0
var dry_arg := false
var dry_frames := 600
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
		elif a == "--test=soil": soil_test = true
		elif a == "--test=grains": grains_test = true
		elif a == "--test=wind": wind_test = true
		elif a == "--test=water": water_test = true
		elif a == "--test=shelter": shelter_test = true
		elif a == "--test=runoff": runoff_test = true
		elif a == "--test=splash": splash_test = true
		elif a == "--test=drift": drift_test = true
		elif a == "--test=near": near_test = true
		elif a == "--test=mud": mud_test = true
		elif a == "--test=clog": clog_test = true
		elif a == "--test=puddles": puddle_test = true
		elif a == "--test=cracks": crack_test = true
		elif a == "--test=legs": legs_test = true
		elif a == "--test=muddy": muddy_test = true
		elif a == "--test=tracked": tracked_test = true
		elif a == "--test=ruts": ruts_test = true
		elif a == "--test=puddle-helper": pudhelp_test = true
		elif a == "--near=0": near_on = false
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a == "--gpu=0": gpu = false
		elif a.begins_with("--wind="): wind_arg = float(a.substr(7))
		elif a.begins_with("--water="): water_arg = float(a.substr(8))
		elif a == "--dry": dry_arg = true
		elif a.begins_with("--dry="):
			dry_arg = true
			dry_frames = int(a.substr(6))
		elif a.begins_with("--mud="):
			for v in a.substr(6).split(","):
				mud_arg.append(float(v))
		elif a.begins_with("--sun="):
			for v in a.substr(6).split(","):
				sun_arg.append(float(v))
		elif a.begins_with("--cam="):
			for v in a.substr(6).split(","):
				cam_arg.append(float(v))
	if mud_test:
		await _run_mud_tests()
	if pudhelp_test:
		await _run_puddle_helper_tests()
		await _run_world_puddle_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if ruts_test:
		await _run_ruts_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if tracked_test:
		await _run_tracked_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if muddy_test:
		await _run_muddy_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if legs_test:
		await _run_legs_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if clog_test:
		await _run_clog_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if near_test:
		await _run_near_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if crack_test:
		await _run_crack_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if puddle_test:
		await _run_puddle_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if shelter_test:
		await _run_shelter_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if drift_test:
		await _run_drift_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if splash_test:
		await _build_wade()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if runoff_test:
		await _run_runoff_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if water_test:
		await _run_water_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if wind_test:
		await _run_wind_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if grains_test:
		await _run_grains_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if soil_test:
		await _run_soil_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if test:
		await _run_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if view == "ruts":
		await _build_ruts()
		return
	if view == "roost":
		await _build_roost()
		return
	if view == "bodymud":
		await _build_bodymud()
		return
	if view == "legs":
		await _build_legs()
		return
	if view == "tracked":
		await _build_legs(true)
		return
	if view == "wade":
		await _build_wade()
		return
	if view == "ripples":
		await _build_ripples()
		return
	if view == "swash":
		await _build_swash()
		return
	if view == "grains":
		await _build_grains()
		return
	if view == "rills":
		await _build_rills()
		return
	if view == "shore":
		await _build_shore()
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
	surface.gpu_simulation = gpu
	surface.near_field = near_on
	surface.sand_glints = not OS.get_cmdline_user_args().has("--glints=0") # A/B
	surface.keep_ruts = ruts_on
	surface.ruts_time_scale = ruts_scale
	if OS.get_cmdline_user_args().has("--meadow"):
		_meadow(surface)
	surface.ruts_path = "user://bench_ruts"
	# Everything ships on; each check turns on what it is about (--defaults keeps the shipped settings).
	if not OS.get_cmdline_user_args().has("--defaults"):
		surface.wind_ripples = false
		surface.runoff_channels = false
		surface.standing_water = false
		surface.desiccation_cracks = cracks_on
		surface.world_puddles = false
	# --prop=name:value sets any of the surface's properties (repeatable), for an A/B.
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--prop="):
			var pkv := a.substr(7).split(":", true, 1)
			surface.set(pkv[0], str_to_var(pkv[1]))
	add_child(surface)
	await get_tree().physics_frame


# --meadow: the patch drawn with a TerrainMaterial3D under a full sward's turf (the grass cover globals set by
# hand, as a GrassField3D publishes them), so ruts, puddles and mud show what they do to a meadow's cover.
func _meadow(p_surface: GroundSurface3D) -> void:
	var tm := TerrainMaterial3D.new()
	tm.grass_tint_enabled = true
	tm.ground_deform_enabled = true
	tm.sea_level = -100.0
	var soil := TerrainMaterialLayer.new()
	soil.color = Color(0.36, 0.27, 0.19)
	soil.roughness = 0.9
	tm.soil_layer = soil
	tm.set_shader_parameter("ground_deform_patch", true)
	p_surface.material = tm
	RenderingServer.global_shader_parameter_set("grass_ground_color", Color(0.16, 0.28, 0.07, 1.0))
	RenderingServer.global_shader_parameter_set("grass_ground_params", Vector4(0.0, 0.0, 0.0, 0.0))
	var img := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	img.fill(Color(1, 0, 0, 1))
	RenderingServer.global_shader_parameter_set("grass_ground_map", ImageTexture.create_from_image(img))
	RenderingServer.global_shader_parameter_set("grass_ground_rect", Vector4(-200, -200, 400, 400))


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
	# Over the finest field's texel at p (the near field's where it is on).
	var t := surface.deform_extent / surface.deform_resolution
	if surface.get_near_rect().has_point(Vector2(p.x, p.z)):
		t = surface.near_extent / surface.near_resolution
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
	surface.sync_deformation()
	var auto_dent := surface.get_dent_at(Vector3(1.0, 0, 1.0)) - surface.get_rim_at(Vector3(1.0, 0, 1.0))
	_check("a body in ground_deform prints", auto_dent > 0.01, "%.1f cm under the boot" % (auto_dent * 100.0))
	# With the render loop off the GPU field gets no frames: its read backs wait (one in flight), nothing
	# overruns, and prints made meanwhile show once it is back on.
	RenderingServer.render_loop_enabled = false
	for i in 120:
		foot.global_position = Vector3(1.0 + i * 0.02, 0.03, 2.0)
		await get_tree().physics_frame
	RenderingServer.render_loop_enabled = true
	for i in 3:
		await get_tree().process_frame
	surface.sync_deformation()
	var late := surface.get_dent_at(Vector3(2.0, 0, 2.0)) - surface.get_rim_at(Vector3(2.0, 0, 2.0))
	_check("render loop off for 2 s: no crash, prints kept", late > 0.01, "%.1f cm under the boot's path" % (late * 100.0))
	print("step %.2f ms, field on the %s" % [surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU"])


# Width of a print's wall along +x from p: the run from 10% to 90% of the depth at p, m (step: the sampling).
func _wall_width(p: Vector3, step: float) -> float:
	var g: float = height.get_height(p) if height.has_method("get_height") else 0.0
	var depth: float = g - surface.get_surface_height_at(p)
	var x10 := -1.0
	var x90 := -1.0
	var x := 0.0
	while x < 0.4:
		var q := p + Vector3(x, 0, 0)
		var d: float = (height.get_height(q) if height.has_method("get_height") else 0.0) - surface.get_surface_height_at(q)
		if x90 < 0.0 and d < 0.9 * depth:
			x90 = x
		if d < 0.1 * depth:
			x10 = x
			break
		x += step
	return x10 - x90 if x10 >= 0.0 and x90 >= 0.0 else 1.0


# Steepest slope (degrees) at a given spacing in a square around p.
func _max_slope_at(p: Vector3, half: float, t: float) -> float:
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


func _run_near_tests() -> void:
	near_on = true
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 3:
		await get_tree().physics_frame
	_check("near field active on the GPU path", surface.is_near_active(), "near rect %s, main %s" % [surface.get_near_rect(), surface.get_deform_rect()])
	if not surface.is_near_active():
		return
	var tn := surface.near_extent / surface.near_resolution
	var o := Vector3(0.3, 0, 0.2)
	# A boot (0.11 x 0.29 m) pressed 4 cm: the wall from 90% to 10% of the depth, near field against main.
	surface.clear_deformation()
	surface.press_box(o, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	surface.sync_deformation()
	var near_wall := _wall_width(o, tn * 0.5)
	var near_depth := -surface.get_surface_height_at(o)
	var nv: Vector2 = surface.get_near_moved_volume()
	var mv: Vector2 = surface.get_moved_volume()
	surface.near_field = false
	await get_tree().physics_frame
	surface.clear_deformation()
	surface.press_box(o, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	surface.sync_deformation()
	var main_wall := _wall_width(o, tn * 0.5)
	var main_depth := -surface.get_surface_height_at(o)
	_check("boot print walls are sharp", near_wall <= 0.035 and near_wall < main_wall * 0.6,
			"wall %.1f cm on the near field, %.1f cm on the main field; depth %.1f and %.1f cm" % [near_wall * 100.0, main_wall * 100.0, near_depth * 100.0, main_depth * 100.0])
	_check("near field keeps the press's volume", absf((nv.x - nv.y) - (mv.x - mv.y)) < 0.1 * maxf(mv.x - mv.y, 1e-6) and absf(nv.x - mv.x) < 0.1 * mv.x,
			"near dent %.3f l rim %.3f l, main dent %.3f l rim %.3f l" % [nv.x * 1000.0, nv.y * 1000.0, mv.x * 1000.0, mv.y * 1000.0])
	surface.near_field = true
	for i in 3:
		await get_tree().physics_frame
	# A boot sole (a body with the ground_sole meta) prints its tread: lug bars stand in the forefoot's floor.
	surface.clear_deformation()
	var boot := Node3D.new()
	var bs := CollisionShape3D.new()
	bs.shape = BoxShape3D.new()
	(bs.shape as BoxShape3D).size = Vector3(0.11, 0.06, 0.29)
	boot.add_child(bs)
	boot.set_meta("ground_mass", 80.0)
	boot.set_meta("ground_contact_area", 0.035)
	boot.set_meta("ground_sole", true)
	boot.add_to_group("ground_deform")
	add_child(boot)
	boot.global_position = Vector3(o.x, _undisturbed(o) + 0.03, o.z)
	for i in 20:
		await get_tree().physics_frame
	boot.queue_free()
	for i in 3:
		await get_tree().physics_frame
	surface.sync_deformation()
	var lo := 1.0
	var hi := -1.0
	var row := ""
	for j in 13:
		var q := o + Vector3(0, 0, 0.0 + j * 0.008)
		var hq := surface.get_surface_height_at(q) - _undisturbed(q)
		row += "%.1f " % (hq * 100.0)
		lo = minf(lo, hq)
		hi = maxf(hi, hq)
	_check("a boot sole prints its lug bars", hi - lo > 0.004, "forefoot floor %s cm" % row)
	# A ball's pit in dry sand slumps to the repose on the near field too, in the same 3 s.
	surface.ground_type = GroundSurface3D.GROUND_SAND
	surface.moisture = 0.0
	for i in 3:
		await get_tree().physics_frame
	surface.clear_deformation()
	surface.press(o, 0.15, -0.12, 0.0, true)
	surface.sync_deformation()
	var before := _max_slope_at(o, 0.3, tn * 2.0)
	var v0: Vector2 = surface.get_near_moved_volume()
	var t0 := Time.get_ticks_usec()
	surface.settle(3.0)
	var settle_ms := (Time.get_ticks_usec() - t0) / 1000.0
	var after := _max_slope_at(o, 0.3, tn * 2.0)
	var v1: Vector2 = surface.get_near_moved_volume()
	_check("dry sand slumps to its repose on the near field", before > 45.0 and after < 38.0, "steepest wall %.0f before, %.0f degrees after 3 s (over %.1f cm)" % [before, after, tn * 200.0])
	_check("near slumping keeps volume", absf((v1.x - v1.y) - (v0.x - v0.y)) < 2e-4, "net hollow %.3f l before, %.3f l after; 3 s settled in %.0f ms here" % [(v0.x - v0.y) * 1000.0, (v1.x - v1.y) * 1000.0, settle_ms])
	# A print made outside the near field is there when the near field moves over it (seeded from the main).
	surface.ground_type = GroundSurface3D.GROUND_SOIL
	surface.moisture = 0.6
	for i in 3:
		await get_tree().physics_frame
	surface.clear_deformation()
	var nr: Rect2 = surface.get_near_rect()
	var away := Vector3(nr.end.x + 2.0, 0, nr.get_center().y)
	surface.press_box(away, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	surface.sync_deformation()
	var main_there := -surface.get_surface_height_at(away)
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(away.x + 2.0, 0, 0)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	var nr2: Rect2 = surface.get_near_rect()
	var near_there := -surface.get_surface_height_at(away)
	_check("a print outside the near field is kept when it arrives", nr2.has_point(Vector2(away.x, away.z)) and near_there > 0.6 * main_there,
			"%.1f cm deep on the main field, %.1f cm on the near field after it moved (near %s)" % [main_there * 100.0, near_there * 100.0, nr2])
	print("step %.2f ms, near %d slump passes" % [surface.get_step_time_msec(), roundi(pow(surface.deform_extent / surface.deform_resolution / tn, 2.0))])


# Stiff mud: Wong's lean clay at 32% water by mass (a solid share of 0.535, GroundSurface3D.mud_concentration),
# the layer half soft at that moisture.
const STIFF_MUD := 0.45


# A mud layer of p_depth painted over the whole bench (mud_map white), at a moisture.
func _paint_mud(p_depth: float, p_moisture: float) -> void:
	var img := Image.create(4, 4, false, Image.FORMAT_RF)
	img.fill(Color(1, 0, 0))
	surface.mud_max_depth = p_depth
	surface.mud_map_rect = Rect2(-200, -200, 400, 400)
	surface.mud_map = img
	surface.moisture = p_moisture
	for i in 3:
		await get_tree().physics_frame


# Rain fills a rut on p_ground, stops, and the rut dries (sped up): the crack state at the rut after each.
func _rain_and_dry(p_ground: int, o: Vector3) -> Array:
	near_on = false
	await _setup("flat", p_ground, 0.2, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.desiccation_cracks = true
	surface.runoff_time_scale = 100.0
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 20.0
	wm.humidity = 0.6
	wm.temperature = 18.0
	add_child(wm)
	wm.snap_to_settings()
	surface.press_box(o, Vector2(0.3, 1.5), 0.0, 0.0, 0.05)
	surface.sync_deformation()
	surface.settle(20.0, 0.05)
	for i in 40:
		await get_tree().physics_frame
	var wet: Vector4 = surface.get_crack_at(o)
	var water := surface.get_standing_water_at(o)
	wm.rain_rate = 0.0
	wm.cloud_cover = 0.0
	wm.snap_to_settings()
	# Days of drying in seconds: the water soaks away and evaporates, then the drying layer.
	surface.runoff_time_scale = 1e5
	for i in 600:
		await get_tree().physics_frame
	var dry: Vector4 = surface.get_crack_at(o)
	return [wet, dry, water, wm]


func _run_crack_tests() -> void:
	var o := Vector3(0.3, 0, 0.2)
	var r: Array = await _rain_and_dry(GroundSurface3D.GROUND_SOIL, o)
	var wet: Vector4 = r[0]
	var dry: Vector4 = r[1]
	print("  soil: %.1f mm of water in the rut; cracks %s soaked, %s dried" % [r[2] * 1000.0, wet, dry])
	_check("soaked clay does not crack", r[2] > 0.001 and wet.x == 0.0, "opening %.2f under %.1f mm of water" % [wet.x, r[2] * 1000.0])
	_check("dried, it cracks open", dry.x > 0.9 and dry.w > 0.9, "opening %.2f, crust %.2f" % [dry.x, dry.w])
	var flat: Vector4 = surface.get_crack_at(o + Vector3(1.5, 0, 0))
	_check("plates about four times the drying layer (Groisman and Kaplan 1994), finer outside the pool", dry.y > 0.04 and dry.y < 0.25 and (flat.x == 0.0 or flat.y < 0.7 * dry.y), "%.1f cm in the rut, %.1f cm on the flat beside it (opening %.2f)" % [dry.y * 100.0, flat.y * 100.0, flat.x])
	# Rain again: it swells shut, and the cycle makes the next pattern more regular (Goehring et al. 2010).
	var wm: WeatherMap3D = r[3]
	wm.cloud_cover = 1.0
	wm.rain_rate = 20.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 100.0
	surface.settle(20.0, 0.05)
	for i in 40:
		await get_tree().physics_frame
	var shut: Vector4 = surface.get_crack_at(o)
	wm.rain_rate = 0.0
	wm.cloud_cover = 0.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 1e5
	for i in 600:
		await get_tree().physics_frame
	var again: Vector4 = surface.get_crack_at(o)
	_check("wetting closes the cracks", shut.x == 0.0, "opening %.2f" % shut.x)
	_check("the next drying cracks again, more regular", again.x > 0.9 and again.z > dry.z, "opening %.2f, regularity %.2f (was %.2f)" % [again.x, again.z, dry.z])
	wm.queue_free()
	r = await _rain_and_dry(GroundSurface3D.GROUND_SAND, o)
	_check("sand does not crack", (r[1] as Vector4).x == 0.0, "opening %.2f" % (r[1] as Vector4).x)
	(r[3] as WeatherMap3D).queue_free()
	near_on = true


# Standing water (docs/ideas/mud.md, stage 2) in a square of half size p_half round p, m3, and the water
# surface's spread (m) over the samples deeper than 5 mm.
func _water_volume(p: Vector3, half: float) -> Vector2:
	var t := surface.deform_extent / surface.deform_resolution
	if surface.get_near_rect().has_point(Vector2(p.x, p.z)):
		t = surface.near_extent / surface.near_resolution
	var vol := 0.0
	var lo := 1e9
	var hi := -1e9
	# At texel centres, where the field holds its values (between them a shore texel's depth and its bed blend
	# apart and read as a slope that is not there).
	var x := (floorf((p.x - half) / t) + 0.5) * t
	while x <= p.x + half:
		var z := (floorf((p.z - half) / t) + 0.5) * t
		while z <= p.z + half:
			var a := Vector3(x, 0, z)
			var w := surface.get_standing_water_at(a)
			vol += w * t * t
			if w > 0.005:
				var top := surface.get_surface_height_at(a) + w
				lo = minf(lo, top)
				hi = maxf(hi, top)
			z += t
		x += t
	return Vector2(vol, hi - lo if hi >= lo else 0.0)


func _run_puddle_tests() -> void:
	# Ground water seeping into a rut: soaked soil (moisture 0.9) has its water table 6 cm down; a 12 cm rut
	# fills to it and stands level. Conductivity sped up 1000 times (runoff_time_scale) so it fills in minutes.
	# The main field only (6.25 cm): the near field runs the same passes 16 times over, slow on a CPU device.
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 1000.0
	var o := Vector3(0.3, 0, 0.2)
	surface.press_box(o, Vector2(0.3, 1.5), 0.0, 0.0, 0.12)
	surface.sync_deformation()
	var floor_h := surface.get_surface_height_at(o)
	surface.settle(240.0, 0.05)
	var w := surface.get_standing_water_at(o)
	var vs := _water_volume(o, 1.0)
	print("  rut floor %.1f cm, water %.1f cm, surface at %.1f cm; %.1f l, spread %.1f mm" % [floor_h * 100.0, w * 100.0, (floor_h + w) * 100.0, vs.x * 1000.0, vs.y * 1000.0])
	_check("ground water seeps into a rut up to the water table", absf(floor_h + w + 0.06) < 0.01, "surface %.1f cm (table -6 cm)" % ((floor_h + w) * 100.0))
	_check("it stands level", vs.y < 0.003, "%.1f mm from lowest to highest" % (vs.y * 1000.0))
	_check("none outside the rut", surface.get_standing_water_at(o + Vector3(0.6, 0, 0)) == 0.0, "%.2f mm beside it" % (surface.get_standing_water_at(o + Vector3(0.6, 0, 0)) * 1000.0))
	# A tyre wading up the rut at 2 m/s (mud stage 4c): the water rises ahead of it and falls behind, volume kept
	# (sources and sinks stopped).
	surface.runoff_time_scale = 0.0
	surface.settle(1.0, 0.05)
	var ahead0 := surface.get_standing_water_at(o + Vector3(0, 0, 0.52))
	var behind0 := surface.get_standing_water_at(o + Vector3(0, 0, 0.1))
	var vw0 := _water_volume(o, 1.0)
	# It rolls 60 cm up the rut, a physics frame at a time.
	for k in 15:
		surface.add_wader(1, o + Vector3(0, floor_h + 0.33, -0.3 + 0.04 * k), 0.35, 0.12, Vector3(0, 0, 2.0))
		surface.settle(0.02, 0.02)
		await get_tree().physics_frame
	var ahead1 := surface.get_standing_water_at(o + Vector3(0, 0, 0.52))
	var behind1 := surface.get_standing_water_at(o + Vector3(0, 0, 0.1))
	var vw1 := _water_volume(o, 1.0)
	var prof := []
	for k in 13:
		prof.append(snappedf(surface.get_standing_water_at(o + Vector3(0, 0, -0.3 + 0.1 * k)) * 100.0, 0.1))
	print("  water along the rut after the tyre (cm, z -0.3 to 0.9): ", prof)
	_check("a wading tyre pushes the water up ahead of it and out from under it, volume kept", ahead1 > ahead0 + 0.003 and behind1 < behind0 - 0.003 and absf(vw1.x - vw0.x) < 0.02 * vw0.x,
			"ahead %.1f to %.1f cm, under its rear %.1f to %.1f cm; %.2f l to %.2f l" % [ahead0 * 100.0, ahead1 * 100.0, behind0 * 100.0, behind1 * 100.0, vw0.x * 1000.0, vw1.x * 1000.0])
	# Water added and taken by discs (GroundSurface3D.add_water, mud stage 4c: spray landing, sheets thrown up):
	# the field's volume moves by exactly that.
	var va0 := _water_volume(o, 1.0)
	surface.add_water(o + Vector3(0, 0, 0.3), 0.15, 0.002)
	surface.settle(0.02, 0.02)
	await get_tree().physics_frame
	var va1 := _water_volume(o, 1.0)
	surface.add_water(o + Vector3(0, 0, -0.3), 0.1, -0.001)
	surface.settle(0.02, 0.02)
	await get_tree().physics_frame
	var va2 := _water_volume(o, 1.0)
	_check("water added and taken by add_water lands in the field exactly", absf(va1.x - va0.x - 0.002) < 0.00005 and absf(va1.x - va2.x - 0.001) < 0.00005,
		"%.2f l, +2 l: %.2f l, -1 l: %.2f l" % [va0.x * 1000.0, va1.x * 1000.0, va2.x * 1000.0])
	# Flow alone keeps the water: with the sources and sinks stopped (time scale 0), a print pressed beside the
	# pool and cut into it draws water in; the volume holds.
	surface.runoff_time_scale = 0.0
	surface.settle(1.0, 0.05)
	var v0 := _water_volume(o, 1.0)
	surface.press_box(o + Vector3(0.25, 0, 0), Vector2(0.3, 0.4), 0.0, 0.0, 0.16)
	surface.settle(20.0, 0.02)
	var v1 := _water_volume(o, 1.0)
	var w_new := surface.get_standing_water_at(o + Vector3(0.3, 0, 0))
	_check("water runs into a new hollow and keeps its volume", w_new > 0.02 and absf(v1.x - v0.x) < 0.02 * v0.x, "%.1f cm in the new print; %.2f l before, %.2f l after" % [w_new * 100.0, v0.x * 1000.0, v1.x * 1000.0])
	# (the print's rim may dam it off from the rut: level within the print)
	var vp := _water_volume(o + Vector3(0.3, 0, 0), 0.1)
	_check("and stands level in it", vp.y < 0.003, "%.1f mm spread in the print; %.1f mm over print and rut" % [vp.y * 1000.0, v1.y * 1000.0])
	# Rain on dry clay soil (table 48 cm down): 20 mm/h is more than it takes (1 um/s), so a sheet stands on the
	# flat and the rut fills deeper; once the rain stops the flat soaks it up first and the rut, compacted, holds
	# its water longer (Horton overland flow; tyres cut the conductivity).
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 100.0
	var wm := WeatherMap3D.new()
	wm.name = "Weather"
	wm.cloud_cover = 1.0
	wm.rain_rate = 20.0
	wm.humidity = 0.6
	wm.temperature = 18.0
	add_child(wm)
	wm.snap_to_settings()
	for i in 10:
		await get_tree().physics_frame
	print("  rain at the rut %.1f mm/h" % wm.get_rain_at(o))
	surface.press_box(o, Vector2(0.3, 1.5), 0.0, 0.0, 0.05)
	surface.sync_deformation()
	surface.settle(20.0, 0.05)
	var rut_wet := surface.get_standing_water_at(o)
	var flat_wet := surface.get_standing_water_at(o + Vector3(1.0, 0, 0))
	print("  after 20 s (33 min scaled): rut %.1f mm, flat %.2f mm" % [rut_wet * 1000.0, flat_wet * 1000.0])
	_check("rain collects in the rut (the flat's sheet beside it runs in)", rut_wet > 2.0 * flat_wet and flat_wet > 0.0, "%.1f mm in the rut, %.2f mm on the flat" % [rut_wet * 1000.0, flat_wet * 1000.0])
	wm.rain_rate = 0.0
	wm.cloud_cover = 0.0
	wm.snap_to_settings()
	for i in 10:
		await get_tree().physics_frame
	print("  rain after it stops %.1f mm/h" % wm.get_rain_at(o))
	surface.settle(120.0, 0.05)
	var rut_after := surface.get_standing_water_at(o)
	var flat_after := surface.get_standing_water_at(o + Vector3(1.0, 0, 0))
	_check("after the rain the flat soaks up its sheet, the rut keeps water", flat_after == 0.0 and rut_after > 0.5 * rut_wet,
			"rut %.1f mm, flat %.2f mm 200 min (scaled) later" % [rut_after * 1000.0, flat_after * 1000.0])
	surface.settle(600.0, 0.5)
	var rut_dry := surface.get_standing_water_at(o)
	_check("the rut empties in a day or so (soaking in and evaporating)", rut_dry < 0.2 * rut_after, "%.1f mm after 17 h (scaled)" % (rut_dry * 1000.0))
	wm.queue_free()
	near_on = true


func _run_mud_tests() -> void:
	# O'Brien and Julien 1988 (Aspen Pit 1) as the engine has them: a slurry at Cv 0.40, stiff mud at Cv 0.50.
	var ty40 := GroundSurface3D.mud_yield_stress(0.40)
	var ty50 := GroundSurface3D.mud_yield_stress(0.50)
	var mu45 := GroundSurface3D.mud_viscosity(0.45)
	_check("Bingham yield stress and viscosity", ty40 > 400.0 and ty40 < 700.0 and ty50 > 5000.0 and ty50 < 9000.0 and mu45 > 50.0 and mu45 < 200.0,
			"tau_y %.0f Pa at Cv 0.40, %.0f Pa at 0.50; mu %.0f Pa s at 0.45" % [ty40, ty50, mu45])
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	var o := Vector3(0.3, 0, 0.2)
	await _paint_mud(0.8, 0.2)
	var dry_depth := surface.get_mud_depth_at(o)
	await _paint_mud(0.8, 1.0)
	var wet_depth := surface.get_mud_depth_at(o)
	_check("painted mud counts only when wet", dry_depth < 0.01 and wet_depth > 0.7, "%.2f m at moisture 0.2, %.2f m soaked" % [dry_depth, wet_depth])
	var slurry_boot := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	await _paint_mud(0.8, STIFF_MUD)
	var stiff_boot := surface.get_sinkage_at(o, 75.0, 0.035, 0.108)
	_check("a boot sinks deep in slurry, little in stiff mud", slurry_boot > 0.1 and slurry_boot < 0.25 and stiff_boot < 0.04,
			"%.1f cm in slurry, %.1f cm in stiff mud" % [slurry_boot * 100.0, stiff_boot * 100.0])
	# A rut: a 30 cm wide box pressed 25 cm, then a minute. Stiff mud keeps its walls; slurry closes it.
	var walls := {}
	for k in ["stiff", "slurry"]:
		await _paint_mud(0.8, STIFF_MUD if k == "stiff" else 1.0)
		surface.clear_deformation()
		surface.press_box(o, Vector2(0.3, 1.5), 0.0, -0.25, 0.25)
		surface.sync_deformation()
		var v0 := surface.get_moved_volume()
		var w0 := _max_slope(o, 0.4)
		var d0 := -surface.get_surface_height_at(o)
		surface.settle(60.0, 0.05)
		var v1 := surface.get_moved_volume()
		var w1 := _max_slope(o, 0.4)
		var d1 := -surface.get_surface_height_at(o)
		walls[k] = [w0, w1, d0, d1]
		_check("%s mud keeps the volume" % k, absf((v1.x - v1.y) - (v0.x - v0.y)) < 0.02 * maxf(v0.x, 1e-6) + 2e-4,
				"net %.2f l before, %.2f l after a minute" % [(v0.x - v0.y) * 1000.0, (v1.x - v1.y) * 1000.0])
	_check("a rut in stiff mud keeps its walls", walls["stiff"][1] > 40.0 and walls["stiff"][3] > 0.6 * walls["stiff"][2],
			"steepest wall %.0f, %.0f degrees after a minute; %.1f to %.1f cm deep" % [walls["stiff"][0], walls["stiff"][1], walls["stiff"][2] * 100.0, walls["stiff"][3] * 100.0])
	_check("a rut in slurry closes", walls["slurry"][1] < 15.0 and walls["slurry"][3] < 0.4 * walls["slurry"][2],
			"steepest wall %.0f, %.0f degrees after a minute; %.1f to %.1f cm deep" % [walls["slurry"][0], walls["slurry"][1], walls["slurry"][2] * 100.0, walls["slurry"][3] * 100.0])
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: vehicle checks skipped)")
		return
	# A 1500 kg 4x4 at half throttle for 4 s (easing in, as one drives in mud): 80 cm of slurry, stiff mud, firm ground.
	var res := {}
	for k in ["firm", "stiff", "slurry"]:
		await _soil_scene("soil")
		if k != "firm":
			await _paint_mud(0.8, STIFF_MUD if k == "stiff" else 1.0)
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		if k == "firm":
			car.soft_soil = false
		surface.follow_target = surface.get_path_to(car)
		res[k] = await _drive(car, 4.0, 0.5)
		print("  %s: %.1f m in 4 s, sinkage %.1f cm, slip %.2f" % [k, res[k][0], res[k][1] * 100.0, res[k][2]])
	_check("a 4x4 sinks to its axles and bogs in slurry", res["slurry"][1] > 0.3 and res["slurry"][0] < 0.25 * res["firm"][0],
			"%.1f m, sinkage %.1f cm (wheel radius 35)" % [res["slurry"][0], res["slurry"][1] * 100.0])
	# Lean clay at 32% water (Wong table 2.2: c 4.14 kPa, phi 13 deg) gives a tyre little more thrust than its
	# compaction resistance (motion resistance about 0.16 of the load): a crawl, as stiff mud is in MudRunner.
	_check("it crawls through stiff mud, sinking a fraction of slurry's depth", res["stiff"][0] > 0.5 and res["stiff"][0] > 4.0 * res["slurry"][0] and res["stiff"][1] < 0.25 * res["slurry"][1],
			"%.1f m, sinkage %.1f cm; firm %.1f m" % [res["stiff"][0], res["stiff"][1] * 100.0, res["firm"][0]])


# Mud on wheels and the belly (docs/ideas/mud.md, stage 3).
# A stand-in pair of legs for GroundLegMud3D (mud stage 4e): a root, two shin cylinders and marker nodes at the
# soles and knees, walked by moving the markers (no skeleton).
func _make_legs(p: Vector3) -> Node3D:
	var root := Node3D.new()
	root.name = "Legs"
	root.position = p
	# Facing +x (its +z).
	root.rotation.y = PI / 2.0
	add_child(root)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.16, 0.2, 0.27)
	mat.roughness = 0.95
	var boot_mat := StandardMaterial3D.new()
	boot_mat.albedo_color = Color(0.09, 0.07, 0.05)
	boot_mat.roughness = 0.6
	for side in ["L", "R"]:
		var leg := MeshInstance3D.new()
		leg.name = "Shin" + side
		var cyl := CylinderMesh.new()
		cyl.top_radius = 0.055
		cyl.bottom_radius = 0.06
		cyl.height = 0.5
		leg.mesh = cyl
		leg.material_override = mat
		root.add_child(leg)
		# A boot on the sole: a rounded block, toe forward.
		var boot := MeshInstance3D.new()
		boot.name = "Boot" + side
		var bm := CapsuleMesh.new()
		bm.radius = 0.06
		bm.height = 0.3
		boot.mesh = bm
		boot.rotation_degrees = Vector3(90, 0, 0)
		boot.scale = Vector3(0.85, 1.0, 1.0)
		boot.material_override = boot_mat
		root.add_child(boot)
		for m in ["Sole", "Knee"]:
			var mk := Node3D.new()
			mk.name = m + side
			root.add_child(mk)
	var mud := GroundLegMud3D.new()
	mud.name = "LegMud"
	mud.left_sole_node = NodePath("../SoleL")
	mud.left_knee_node = NodePath("../KneeL")
	mud.right_sole_node = NodePath("../SoleR")
	mud.right_knee_node = NodePath("../KneeR")
	root.add_child(mud)
	return root


# Walks the legs p_dist m the way they face at p_speed m/s: the stance foot on the ground, the other swung 10 cm up.
func _walk_legs(legs: Node3D, p_dist: float, p_speed: float) -> void:
	var dt := 1.0 / 60.0
	var t := 0.0
	var stride := 0.7
	while t * p_speed < p_dist:
		legs.position += legs.basis.z.normalized() * (p_speed * dt)
		legs.position.y = surface.get_surface_height_at(legs.position)
		var phase := fmod(t * p_speed / stride, 1.0)
		for k in 2:
			var side: String = ["L", "R"][k]
			var ph := fmod(phase + 0.5 * k, 1.0)
			var lift := 0.1 * sin(PI * (ph - 0.5) / 0.5) if ph > 0.5 else 0.0
			var reach := (0.5 - ph) * stride * 0.5
			var x := 0.1 if k == 0 else -0.1
			(legs.get_node("Sole" + side) as Node3D).position = Vector3(x, lift, reach)
			(legs.get_node("Knee" + side) as Node3D).position = Vector3(x, 0.5 + lift, 0.6 * reach)
			var shin := legs.get_node("Shin" + side) as MeshInstance3D
			shin.position = Vector3(x, 0.25 + lift, 0.8 * reach)
			shin.rotation.x = -atan2(0.4 * reach, 0.5)
			(legs.get_node("Boot" + side) as Node3D).position = Vector3(x, lift + 0.06, reach + 0.06)
		await get_tree().physics_frame
		t += dt


# Muddy water (mud round 3): a pit in wet clay soil fills with ground water (as the puddle test); a tyre churns its
# clay into the water, a stir adds more, it settles out over minutes, and water landing carries its own clay.
func _run_muddy_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 1000.0
	surface.press_box(Vector3(0, 0, 0), Vector2(6.0, 1.2), 0.0, 0.0, 0.2)
	surface.sync_deformation()
	surface.settle(240.0, 0.05)
	surface.runoff_time_scale = 0.0
	surface.settle(1.0, 0.05)
	var a := Vector3(-1.0, 0, 0)
	var b := Vector3(2.5, 0, 0)
	var depth := surface.get_standing_water_at(a)
	var clear0 := surface.get_water_mud_at(a)
	_check("ground water seeping into a pit comes in clear", depth > 0.08 and clear0 < 0.5, "%.1f cm of water, %.2f kg/m3" % [depth * 100.0, clear0])
	# A tyre rolls 2.4 m up the pit's floor at 2 m/s.
	var floor_h := surface.get_surface_height_at(a)
	for k in 72:
		surface.add_wader(1, Vector3(-2.4 + 2.0 * k / 60.0, floor_h + 0.35, 0), 0.35, 0.12, Vector3(2.0, 0, 0))
		surface.settle(1.0 / 60.0, 1.0 / 60.0)
		await get_tree().physics_frame
	surface.settle(0.5, 0.05)
	var churned := surface.get_water_mud_at(a)
	var far := surface.get_water_mud_at(b)
	_check("a tyre rolling through the pit churns its clay into the water", churned > 10.0 and far < 0.25 * churned, "%.1f kg/m3 in its track, %.1f kg/m3 past where it stopped" % [churned, far])
	# Half a kilo of clay stirred into the far end (a boot, a clod landing).
	surface.stir_water(b, 0.2, 0.5)
	surface.settle(0.1, 0.05)
	var stirred := surface.get_water_mud_at(b)
	var expect := 0.5 / (PI * 0.04 * depth)
	_check("clay stirred in clouds the water there by its mass over the water's volume", absf(stirred - far - expect) < 0.35 * expect, "%.1f kg/m3 (%.1f expected)" % [stirred - far, expect])
	# Water landing (a spray's drops) brings its own clay.
	var c := Vector3(0.0, 0, 0.4)
	var before := surface.get_water_mud_at(c)
	surface.add_water(c, 0.1, 0.002, 200.0)
	surface.settle(0.1, 0.05)
	_check("water landing brings the clay it carries", surface.get_water_mud_at(c) > before + 5.0, "%.1f to %.1f kg/m3" % [before, surface.get_water_mud_at(c)])
	# Ten minutes still: the floc settles out at 0.1 mm/s, the clay falling as exp(-w t / depth).
	surface.settle(600.0, 0.5)
	var after := surface.get_water_mud_at(a)
	var fall := churned * exp(-1e-4 * 600.0 / depth)
	_check("left still, the clay settles out at the floc's Stokes speed", absf(after - fall) < 0.15 * fall, "%.1f to %.1f kg/m3 after 10 minutes (%.1f expected)" % [churned, after, fall])
	# Sand (its clay-free bed) stays clear under the same tyre.
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.press_box(Vector3(0, 0, 0), Vector2(6.0, 1.2), 0.0, 0.0, 0.2)
	surface.sync_deformation()
	surface.add_water(Vector3(-1.0, 0, 0), 0.5, 0.05)
	surface.settle(1.0, 0.05)
	floor_h = surface.get_surface_height_at(a)
	for k in 72:
		surface.add_wader(1, Vector3(-2.4 + 2.0 * k / 60.0, floor_h + 0.35, 0), 0.35, 0.12, Vector3(2.0, 0, 0))
		surface.settle(1.0 / 60.0, 1.0 / 60.0)
		await get_tree().physics_frame
	_check("a tyre through water over sand leaves it clear", surface.get_water_mud_at(a) < 1.0, "%.2f kg/m3 (%.1f cm of water)" % [surface.get_water_mud_at(a), surface.get_standing_water_at(a) * 100.0])


# Mud tracked onto firm ground (mud round 3): boots walked out of slurry onto dry soil leave fading prints and come
# out cleaner step by step; the film dries. (The mud dries away where they walked: a soil's mud is its wetness.)
# Ruts kept across the world (mud round 3): a print goes to disk as the field moves off it, is there when the field
# comes back, and in the next session (a new ground node over the same place).
func _run_ruts_tests() -> void:
	var wipe := GroundSurface3D.new()
	wipe.ruts_path = "user://bench_ruts"
	wipe.clear_saved_ruts()
	wipe.free()
	ruts_on = true
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 3:
		await get_tree().physics_frame
	var a := Vector3(1.0, 0.0, 0.5)
	var b := Vector3(-0.6, 0.0, -0.4)
	surface.press_box(a, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	surface.press_box(b, Vector2(0.11, 0.29), 0.3, 0.0, 0.03)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	var d0 := -surface.get_surface_height_at(a)
	var e0 := -surface.get_surface_height_at(b)
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(600.0, 0.0, 0.0)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	var files := surface.get_saved_rut_tile_count()
	probe.global_position = Vector3.ZERO
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var d1 := -surface.get_surface_height_at(a)
	_check("a print goes to disk as the field moves off it and is there when it comes back", files > 0 and d0 > 0.02 and absf(d1 - d0) < 0.1 * d0,
			"%.1f cm deep, %d tiles kept 600 m away, %.1f cm on return" % [d0 * 100.0, files, d1 * 100.0])
	# The next session: a new ground node (the old one keeps what it holds as it leaves the tree).
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var d2 := -surface.get_surface_height_at(a)
	var e2 := -surface.get_surface_height_at(b)
	_check("and in the next session, both prints as they were", absf(d2 - d0) < 0.1 * d0 and absf(e2 - e0) < 0.1 * e0,
			"%.1f and %.1f cm, %.1f and %.1f cm before" % [d2 * 100.0, e2 * 100.0, d0 * 100.0, e0 * 100.0])
	# Weeks away (the world's clock run fast while the tiles were on disk): rain has softened them (mud round 4).
	var away_s := 3.0e6 # 35 days
	var t_left := Time.get_unix_time_from_system()
	ruts_scale = away_s / 4.0
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var real_away := Time.get_unix_time_from_system() - t_left
	var d5 := -surface.get_surface_height_at(a)
	var want := d0 * exp(-surface.rain_smoothing * surface.ruts_away_rain * real_away * ruts_scale / 3600.0)
	_check("kept ruts come back aged by the time away: rain has softened them", d5 < 0.9 * d0 and d5 > 0.05 * d0 and absf(d5 - want) < 0.25 * want,
			"%.1f cm after %.0f days away (%.1f cm expected), %.1f cm when left" % [d5 * 100.0, real_away * ruts_scale / 86400.0, want * 100.0, d0 * 100.0])
	var rg := surface.get_regrown_at(a)
	var rg_want := 1.0 - exp(-real_away * ruts_scale / surface.ruts_regrowth_time)
	_check("and the grass has grown back over them (mud round 6)", absf(rg - rg_want) < 0.1 and surface.get_regrown_at(a + Vector3(3, 0, 3)) == 0.0,
			"%.2f regrown (%.2f expected), %.2f off the print" % [rg, rg_want, surface.get_regrown_at(a + Vector3(3, 0, 3))])
	surface.press_box(a, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	_check("pressed again, it is bare again", surface.get_regrown_at(a) < 0.05, "%.2f regrown" % surface.get_regrown_at(a))
	ruts_scale = 1.0
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 3:
		await get_tree().physics_frame
	surface.press_box(a, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	# Flattened (cleared) and left: its file goes, so a later visit finds it flat.
	surface.clear_deformation()
	for i in 3:
		await get_tree().physics_frame
	surface.sync_deformation()
	(get_node("Probe") as Node3D).global_position = Vector3(600.0, 0.0, 0.0)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	var left := surface.get_saved_rut_tile_count()
	(get_node("Probe") as Node3D).global_position = Vector3.ZERO
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var d3 := -surface.get_surface_height_at(a)
	_check("a cleared field drops its kept tiles as it leaves them", left == 0 and d3 < 0.002, "%d tiles kept, %.2f cm on return" % [left, d3 * 100.0])
	# Off: a new node starts flat even where tiles are kept.
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	surface.press_box(a, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
	for i in 4:
		await get_tree().physics_frame
	surface.save_ruts()
	ruts_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var d4 := -surface.get_surface_height_at(a)
	_check("with keep_ruts off a new node starts flat", d4 < 0.002, "%.2f cm" % (d4 * 100.0))
	surface.clear_saved_ruts()
	await _run_kept_water_tests()


# A puddle for a scene (GroundSurface3D.add_puddle): a hollow pressed and filled.
func _run_puddle_helper_tests() -> void:
	near_on = false
	for kind in ["flat", "beach"]:
		await _setup(kind, GroundSurface3D.GROUND_SOIL, 0.35, 0.0)
		surface.mud_layer = false
		surface.standing_water = true
		var o := Vector3(0.5, 0, 0.3)
		var g0 := surface.get_ground_height_at(o)
		surface.add_puddle(o, 1.5, 0.12, 40.0)
		for i in 300:
			await get_tree().physics_frame
		surface.sync_deformation()
		var w := surface.get_standing_water_at(o)
		var top := surface.get_surface_height_at(o) + w
		# The water's edge round it: how far out it reaches in 16 directions (irregular, not a circle or a box).
		var reach := []
		var tops := []
		for k in 16:
			var a := TAU * k / 16.0
			var dir := Vector3(cos(a), 0, sin(a))
			var e := 0.0
			var tx := surface.deform_extent / surface.deform_resolution
			for j in 40:
				var q := o + dir * (0.05 * j)
				# At texel centres (between them a shore texel's depth and its bed blend apart).
				q = Vector3((floorf(q.x / tx) + 0.5) * tx, 0, (floorf(q.z / tx) + 0.5) * tx)
				var wq := surface.get_standing_water_at(q)
				if wq > 0.003:
					e = 0.05 * j
				if wq > 0.01:
					tops.append(surface.get_surface_height_at(q) + wq)
			reach.append(e)
		var rmin: float = reach.min()
		var rmax: float = reach.max()
		var tmin: float = tops.min() if tops.size() > 0 else 0.0
		var tmax: float = tops.max() if tops.size() > 0 else 0.0
		var outside := surface.get_standing_water_at(o + Vector3(0, 0, 2.4))
		print("  %s: bed %.1f cm below the ground there" % [kind, (g0 - surface.get_surface_height_at(o)) * 100.0])
		print("  %s: %.1f cm deep at the centre, edge %.2f to %.2f m out, surface %.1f mm from lowest to highest, %.1f kg/m3, %.2f mm 2.4 m out" % [kind, w * 100.0, rmin, rmax, (tmax - tmin) * 1000.0, surface.get_water_mud_at(o), outside * 1000.0])
		_check("add_puddle on %s ground: a filled puddle" % kind, w > 0.07 and rmin > 0.6 and rmax < 2.0, "%.1f cm deep, edge %.2f to %.2f m" % [w * 100.0, rmin, rmax])
		_check("its rim is irregular", rmax - rmin > 0.12, "%.2f to %.2f m" % [rmin, rmax])
		# On the slope the bed's texels step 5 mm each along it: the surface read back there is a step or two out.
		_check("its water stands level and stays in it", tmax - tmin < (0.004 if kind == "flat" else 0.02) and outside < 0.0005, "%.1f mm, %.2f mm outside" % [(tmax - tmin) * 1000.0, outside * 1000.0])
		_check("it carries the clay asked for", absf(surface.get_water_mud_at(o) - 40.0) < 8.0, "%.1f kg/m3" % surface.get_water_mud_at(o))


# Puddles the world places (mud round 6): in hollows, filled where the ground is wet, dry hollows where it is not.
func _run_world_puddle_tests() -> void:
	near_on = false
	var dips := [Vector3(-4.0, 0, -3.0), Vector3(5.0, 0, 2.0), Vector3(-2.0, 0, 6.0)]
	for wet in [true, false]:
		await _setup("hollows", GroundSurface3D.GROUND_SOIL, 0.6 if wet else 0.2, 0.0)
		surface.mud_layer = false
		surface.standing_water = true
		surface.world_puddles = true
		surface.world_puddle_density = 8.0
		for i in 240:
			await get_tree().physics_frame
		surface.sync_deformation()
		var filled := 0
		var dug := 0
		for d in dips:
			var best := 0.0
			var dent := 0.0
			for dx in range(-4, 5):
				for dz in range(-4, 5):
					var q: Vector3 = d + Vector3(dx * 0.5, 0, dz * 0.5)
					best = maxf(best, surface.get_standing_water_at(q))
					dent = maxf(dent, surface.get_dent_at(q))
			filled += 1 if best > 0.02 else 0
			dug += 1 if dent > 0.03 else 0
		var flat_water := 0.0
		var flat_dent := 0.0
		for q in [Vector3(10, 0, -10), Vector3(-11, 0, 10), Vector3(11, 0, 11), Vector3(0, 0, -12)]:
			flat_water = maxf(flat_water, surface.get_standing_water_at(q))
			flat_dent = maxf(flat_dent, surface.get_dent_at(q))
		print("  %s: %d of 3 dips dug, %d filled; on the flats %.1f cm dug, %.1f mm of water" % ["wet" if wet else "dry", dug, filled, flat_dent * 100.0, flat_water * 1000.0])
		if wet:
			_check("world puddles: the ground's hollows get puddles, filled where it is wet", dug >= 2 and filled >= 2, "%d dug, %d filled" % [dug, filled])
			_check("none on the flats", flat_dent < 0.01, "%.1f cm" % (flat_dent * 100.0))
		else:
			_check("dry ground: the hollows are dug but stay dry", dug >= 2 and filled == 0, "%d dug, %d filled" % [dug, filled])


# Mud round 5: what stood in a rut is kept with it. Clay churned into a pool's water, and cracks a dried pool left.
func _run_kept_water_tests() -> void:
	ruts_on = true
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 1000.0
	surface.press_box(Vector3(0, 0, 0), Vector2(6.0, 1.2), 0.0, 0.0, 0.2)
	surface.sync_deformation()
	surface.settle(240.0, 0.05)
	surface.runoff_time_scale = 0.0
	var m := Vector3(-1.0, 0, 0)
	surface.stir_water(m, 0.4, 2.0)
	surface.settle(0.1, 0.05)
	var c0 := surface.get_water_mud_at(m)
	var w0 := surface.get_standing_water_at(m)
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(600.0, 0.0, 0.0)
	for i in 4:
		await get_tree().physics_frame
	surface.sync_deformation()
	probe.global_position = Vector3.ZERO
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var c1 := surface.get_water_mud_at(m)
	var w1 := surface.get_standing_water_at(m)
	_check("a pool's clay stays in its water as the field moves off and back", c0 > 5.0 and w1 > 0.8 * w0 and absf(c1 - c0) < 0.2 * c0,
			"%.1f kg/m3 in %.1f cm of water, %.1f kg/m3 in %.1f cm on return" % [c0, w0 * 100.0, c1, w1 * 100.0])
	# The next session, at once: the clay is still in it (a tile written seconds ago has not settled).
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 0.0
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var c2 := surface.get_water_mud_at(m)
	_check("and in the next session", absf(c2 - c0) < 0.25 * c0, "%.1f kg/m3 (%.1f when left)" % [c2, c0])
	surface.clear_saved_ruts()
	# A dried pool's cracks.
	cracks_on = true
	var o := Vector3(0.3, 0, 0.2)
	var r: Array = await _rain_and_dry(GroundSurface3D.GROUND_SOIL, o)
	var dry: Vector4 = r[1]
	probe = get_node("Probe") as Node3D
	probe.global_position = Vector3(600.0, 0.0, 0.0)
	for i in 4:
		await get_tree().physics_frame
	var gone: Vector4 = surface.get_crack_at(o)
	probe.global_position = Vector3.ZERO
	for i in 6:
		await get_tree().physics_frame
	var back: Vector4 = surface.get_crack_at(o)
	_check("a dried pool's cracks are kept as the ground moves off them and back", dry.x > 0.9 and gone.x == 0.0 and back.x > 0.9 and absf(back.y - dry.y) < 0.15 * dry.y,
			"opening %.2f, plates %.1f cm; away %.2f; back %.2f, %.1f cm" % [dry.x, dry.y * 100.0, gone.x, back.x, back.y * 100.0])
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	for i in 6:
		await get_tree().physics_frame
	var next: Vector4 = surface.get_crack_at(o)
	_check("and in the next session", next.x > 0.9 and absf(next.y - dry.y) < 0.15 * dry.y, "opening %.2f, plates %.1f cm" % [next.x, next.y * 100.0])
	cracks_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	for i in 6:
		await get_tree().physics_frame
	_check("with desiccation_cracks off none come back", surface.get_crack_at(o).x == 0.0, "opening %.2f" % surface.get_crack_at(o).x)
	surface.clear_saved_ruts()


func _run_tracked_tests() -> void:
	near_on = false
	await _soil_scene("soil")
	# 15 cm of slurry, walked through for 3 m; then the ground dries out (as the clog test's) and the walk goes on.
	await _paint_mud(0.15, 0.85)
	var legs := _make_legs(Vector3(-3.0, 0, 0))
	var lm := legs.get_node("LegMud") as GroundLegMud3D
	surface.follow_target = surface.get_path_to(legs)
	await _walk_legs(legs, 3.0, 1.4)
	surface.moisture = 0.1
	surface.settle(1.0, 0.5)
	var line0 := lm.get_mud_line(GroundLegMud3D.LEG_LEFT)
	await _walk_legs(legs, 5.5, 1.4)
	var line1 := lm.get_mud_line(GroundLegMud3D.LEG_LEFT)
	for i in 10:
		await get_tree().physics_frame
	# Prints along the walk out: the share of points on a film near the mud's edge, and further on.
	var near_hits := 0
	var near_mud := 0.0
	var far_mud := 0.0
	for k in 100:
		for dz in range(-15, 16):
			var nm := surface.get_tracked_mud_at(Vector3(0.6 + 1.2 * k / 100.0, 0, 0.01 * dz))
			near_hits += 1 if nm.x > 0.0 else 0
			near_mud += nm.x
			far_mud += surface.get_tracked_mud_at(Vector3(3.6 + 1.2 * k / 100.0, 0, 0.01 * dz)).x
	print("  line %.1f cm in the mud, %.1f cm after 5.5 m on dry soil; film over %.0f points' worth 0.6-1.8 m out, %.0f at 3.6-4.8 m" % [line0 * 100.0, line1 * 100.0, near_mud, far_mud])
	_check("boots walked out of the mud leave prints on dry ground", near_hits > 100, "%d points" % near_hits)
	_check("and come out cleaner step by step, the prints fainter", line1 < 0.5 * line0 and far_mud < 0.95 * near_mud, "line %.1f to %.1f cm; film %.0f to %.0f" % [line0 * 100.0, line1 * 100.0, near_mud, far_mud])
	# Wet, the film is dark; an hour later it has dried in place and is still there.
	var w0 := _film_wetness()
	surface.settle(3600.0, 10.0)
	var w1 := _film_wetness()
	_check("the film dries in place and stays", w0.y > 100 and w1.y > 0.7 * w0.y and w1.x < w0.x - 0.4, "wetness %.2f to %.2f over %d to %d points" % [w0.x, w1.x, w0.y, w1.y])


# Points 4 cm apart within p_reach of p_at (XZ) carrying a tracked clay film.
func _film_points(p_at: Vector3, p_reach: float) -> int:
	var hits := 0
	var n := int(p_reach / 0.04)
	for j in range(-n, n + 1):
		for i in range(-n, n + 1):
			if surface.get_tracked_mud_at(p_at + Vector3(i * 0.04, 0.0, j * 0.04)).x > 0.0:
				hits += 1
	return hits


# The tracked mud film's mean wetness and the points it covers, 0.6 to 1.8 m out.
func _film_wetness() -> Vector2:
	var sum := 0.0
	var count := 0
	for k in 100:
		for dz in range(-15, 16):
			var m := surface.get_tracked_mud_at(Vector3(0.6 + 1.2 * k / 100.0, 0, 0.01 * dz))
			if m.x > 0.0:
				sum += m.y
				count += 1
	return Vector2(sum / maxf(count, 1), count)


func _run_legs_tests() -> void:
	near_on = false
	await _soil_scene("soil")
	# Dry soil: no mud on the boots.
	surface.moisture = 0.1
	surface.settle(2.0, 0.5)
	var legs := _make_legs(Vector3(-2, 0, 0))
	var lm := legs.get_node("LegMud") as GroundLegMud3D
	surface.follow_target = surface.get_path_to(legs)
	await _walk_legs(legs, 3.0, 1.4)
	var dry_line := maxf(lm.get_mud_line(GroundLegMud3D.LEG_LEFT), lm.get_mud_line(GroundLegMud3D.LEG_RIGHT))
	_check("walking on dry soil leaves the boots clean", dry_line == 0.0 and lm.get_mud_flecks(GroundLegMud3D.LEG_LEFT) == 0.0, "line %.1f cm" % (dry_line * 100.0))
	# Into 15 cm of slurry: the line rises to about its depth, flecks up the shins at a walk, more at a run.
	await _paint_mud(0.15, 0.85)
	surface.mud_color = Color(0.62, 0.3, 0.2)
	await _walk_legs(legs, 3.0, 1.4)
	var l0 := lm.get_mud_line(GroundLegMud3D.LEG_LEFT)
	var l1 := lm.get_mud_line(GroundLegMud3D.LEG_RIGHT)
	var f_walk := lm.get_mud_flecks(GroundLegMud3D.LEG_LEFT)
	print("  in %.1f cm of mud: lines %.1f and %.1f cm, flecks %.2f, wet %.2f" % [surface.get_mud_depth_at(legs.position) * 100.0, l0 * 100.0, l1 * 100.0, f_walk, lm.get_mud_wetness()])
	_check("walking in slurry coats both boots to about its depth", l0 > 0.12 and l1 > 0.12 and l0 < 0.3 and l1 < 0.3 and lm.get_mud_wetness() == 1.0, "%.1f and %.1f cm" % [l0 * 100.0, l1 * 100.0])
	await _walk_legs(legs, 3.0, 4.0)
	var f_run := lm.get_mud_flecks(GroundLegMud3D.LEG_LEFT)
	_check("running throws more flecks up the shins than walking", f_walk > 0.05 and f_run > f_walk + 0.2, "%.2f walking, %.2f running" % [f_walk, f_run])
	var shin := legs.get_node("ShinL") as MeshInstance3D
	var mat := shin.material_override as BaseMaterial3D
	var w0: Vector4 = shin.get_instance_shader_parameter("mud_wheel0")
	var body: Vector4 = shin.get_instance_shader_parameter("mud_body")
	var col: Vector4 = shin.get_instance_shader_parameter("mud_color")
	_check("the shins' material has the Mud layer on in legs mode, fed the line and the local clay", mat.mud_enabled and body.w == 1.0 and absf(w0.w - l0) < 0.001 and col.x > 1.5 * col.z,
			"mud %s, mode %.0f, line %.1f cm, clay (%.2f, %.2f, %.2f)" % [mat.mud_enabled, body.w, w0.w * 100.0, col.x, col.y, col.z])

	# Wading (seepage fills a pit in wet soil to the water table, as the puddle test): the water rinses the legs.
	var line_before := maxf(lm.get_mud_line(GroundLegMud3D.LEG_LEFT), lm.get_mud_line(GroundLegMud3D.LEG_RIGHT))
	var flecks_before := lm.get_mud_flecks(GroundLegMud3D.LEG_LEFT)
	legs.free()
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.runoff_time_scale = 1000.0
	surface.press_box(Vector3(0, 0, 0), Vector2(6.0, 1.2), 0.0, 0.0, 0.2)
	surface.sync_deformation()
	surface.settle(240.0, 0.05)
	surface.runoff_time_scale = 0.0
	var depth := surface.get_standing_water_at(Vector3.ZERO)
	legs = _make_legs(Vector3(-1.5, 0, 0))
	lm = legs.get_node("LegMud") as GroundLegMud3D
	surface.follow_target = surface.get_path_to(legs)
	# Muddied first on the bank in 8 cm of slurry (below the pit's water), then 5.4 m through the pit at 1 m/s.
	await _paint_mud(0.08, 0.85)
	surface.mud_layer = true
	legs.position.x = -6.0
	await _walk_legs(legs, 2.0, 1.4)
	var coated := maxf(lm.get_mud_line(GroundLegMud3D.LEG_LEFT), lm.get_mud_line(GroundLegMud3D.LEG_RIGHT))
	surface.mud_layer = false
	surface.mud_max_depth = 0.0
	legs.position.x = -2.7
	# (The water's read back lags the mud layer's switch by a few frames.)
	for i in 120:
		if surface.get_standing_water_at(Vector3(-2, 0, 0.1)) > 0.1:
			break
		await get_tree().physics_frame
	await _walk_legs(legs, 5.4, 1.0)
	var rinsed := maxf(lm.get_mud_line(GroundLegMud3D.LEG_LEFT), lm.get_mud_line(GroundLegMud3D.LEG_RIGHT))
	print("  pit water %.1f cm: line %.1f cm on the bank, %.1f cm after wading (dry-ground run gave %.1f cm, flecks %.2f)" % [depth * 100.0, coated * 100.0, rinsed * 100.0, line_before * 100.0, flecks_before])
	_check("wading through water deeper than the mud line rinses it off", depth > coated and coated > 0.07 and rinsed < 0.5 * coated, "%.1f cm to %.1f cm in %.1f cm of water" % [coated * 100.0, rinsed * 100.0, depth * 100.0])


func _run_clog_tests() -> void:
	await _soil_scene("soil")
	var o := Vector3(0.3, 0, 0.2)
	await _paint_mud(0.8, STIFF_MUD)
	var sticky: Dictionary = surface.get_wheel_soil(o, 3000.0, 0.35, 0.26)
	surface.moisture = 0.1
	surface.settle(5.0, 0.5)
	var dry: Dictionary = surface.get_wheel_soil(o, 3000.0, 0.35, 0.26)
	_check("stiff wet clay is sticky, dry soil is not (Fountaine 1954)", sticky["stickiness"] > 0.7 and dry["stickiness"] < 0.1,
			"stickiness %.2f (ca %.1f kPa) in stiff mud, %.2f dry" % [sticky["stickiness"], sticky["adhesion"], dry["stickiness"]])
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: vehicle checks skipped)")
		return
	# A 4x4 at half throttle for 4 s from rest in stiff mud (its wheels slipping, not racing), with and without
	# clogging.
	var res := {}
	for k in ["clean", "clogging"]:
		await _soil_scene("soil")
		await _paint_mud(0.8, STIFF_MUD)
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		car.tyre_clogging = k == "clogging"
		car.belly_drag = false
		car.body_mud = k == "clogging"
		_dress_car(car)
		surface.follow_target = surface.get_path_to(car)
		# Red laterite (stage 4d): the clay the car picks up should be red.
		surface.mud_color = Color(0.62, 0.3, 0.2)
		var r: Array = await _drive(car, 4.0, 0.5)
		var fill := 0.0
		var traction := 0.0
		for w in 4:
			fill += car.get_wheel_mud(w) / 4.0
			traction += car.get_wheel_soil_traction(w) / 4.0
		res[k] = [r[0], r[2], fill, traction, car, ((car.get_node("Body") as MeshInstance3D).get_active_material(0) as BaseMaterial3D).mud_enabled]
		print("  %s: %.1f m in 4 s, slip %.2f, tread %.0f%% full, traction %.0f N a wheel" % [k, r[0], r[2], fill * 100.0, traction])
	_check("spinning in sticky clay packs the tread", res["clogging"][2] > 0.3 and res["clean"][2] == 0.0, "%.0f%% full" % (res["clogging"][2] * 100.0))
	_check("a packed tread grips as a slick: it slips more for no more ground", res["clogging"][1] > 1.15 * res["clean"][1] and res["clogging"][0] <= res["clean"][0] + 0.05,
			"slip %.2f against %.2f clean; %.2f m against %.2f m" % [res["clogging"][1], res["clean"][1], res["clogging"][0], res["clean"][0]])
	# Out onto dry ground, floored: the tread clears (scoured, and flung off past ~7 m/s of tread speed).
	var car: PhysXVehicle3D = res["clogging"][4]
	surface.moisture = 0.1
	surface.settle(2.0, 0.5)
	# Rolled gently out over the dried ground first: some of the clay is pressed off the tread onto it (mud round 3),
	# a film along the tracks that the floored spin below would only dig away.
	var wheels: Array[Node3D] = []
	for c in car.get_children():
		if c is PhysXVehicleWheel3D:
			wheels.append(c)
	var starts: Array[Vector3] = []
	for w in wheels:
		starts.append(w.global_position)
	var from := car.global_position
	await _drive(car, 3.0, 0.35)
	var to := car.global_position
	var along := (to - from) * Vector3(1, 0, 1)
	var side_x := car.global_transform.basis.x.normalized()
	var hits := 0
	var tries := 0
	for wi in wheels.size():
		var path: Vector3 = (wheels[wi].global_position - starts[wi]) * Vector3(1, 0, 1)
		for k in 40:
			var q: Vector3 = starts[wi] + path * (float(k) / 40.0)
			for dz in range(-6, 7):
				tries += 1
				if surface.get_tracked_mud_at(q + side_x * (0.01 * dz)).x > 0.0:
					hits += 1
	_check("the clay a rolling tread presses onto dry ground is left along its tracks", hits > tries / 10, "mud film at %d of %d points along the tracks (%.1f m)" % [hits, tries, along.length()])
	var before := 0.0
	for w in 4:
		before += car.get_wheel_mud(w) / 4.0
	# The clay thrown off flies as mud clods (stage 4): a grains node routes it to its mud child.
	var grains := _make_grains(car)
	var shed0 := 0.0
	for w in 4:
		shed0 += car.get_wheel_mud_shed(w)
	var film0 := _film_points(car.global_position, 5.0)
	await _drive(car, 4.0, 1.0)
	var after := 0.0
	var shed := 0.0
	for w in 4:
		after += car.get_wheel_mud(w) / 4.0
		shed += car.get_wheel_mud_shed(w)
	_check("on dry ground the tread clears, its clay thrown off", after < 0.3 * before and shed > shed0,
			"%.0f%% to %.0f%% full; %.1f kg thrown" % [before * 100.0, after * 100.0, shed - shed0])
	for i in 120:
		await get_tree().physics_frame
	var mud_grains: PhysXGroundGrains3D = grains.get_mud_grains()
	var md := mud_grains.get_dug_volume() if mud_grains else 0.0
	var mk := (mud_grains.get_airborne_volume() + mud_grains.get_deposited_volume()) if mud_grains else 0.0
	_check("the thrown clay flies as mud clods and lands, volume kept", md > 0.5 * (shed - shed0) / 1700.0 and absf(mk - md) < 0.05 * md + 1e-5,
			"%.1f l of clay thrown as clods, %.1f l in the air or landed (%.1f l of the dry ground thrown as sand)" % [md * 1000.0, mk * 1000.0, grains.get_dug_volume() * 1000.0])
	# Landed on the dry ground, the clods lie there as cakes of wet clay (mud round 4): more of it filmed with clay.
	var film1 := _film_points(car.global_position, 5.0)
	_check("landed clods lie as cakes of clay on the dry ground", film1 > film0 + 20, "clay at %d points within 5 m, %d before the clods" % [film1, film0])
	# Clods thrown at the parked car's side stick to it as splats (stage 4b), their volume kept.
	car.throttle = 0.0
	car.brake = 1.0
	for i in 60:
		await get_tree().physics_frame
	var side := car.global_transform.basis.x.normalized()
	var stuck0 := mud_grains.get_stuck_volume() if mud_grains else 0.0
	if mud_grains:
		# Aimed to meet the side at about 80 cm over the ground.
		mud_grains.kick(car.global_position + side * 2.5, 0.15, 0.03, -side * 5.0 + Vector3(0, 4.2, 0))
	for i in 90:
		await get_tree().physics_frame
	var stuck := (mud_grains.get_stuck_volume() - stuck0) if mud_grains else 0.0
	var md2 := mud_grains.get_dug_volume() if mud_grains else 0.0
	var mk2 := (mud_grains.get_airborne_volume() + mud_grains.get_deposited_volume()) if mud_grains else 0.0
	_check("clods thrown at the body stick to it as splats, volume kept", car.get_mud_splat_count() > 0 and stuck > 0.5 * 0.15 * 0.15 * PI * 0.03 and absf(mk2 - md2) < 0.05 * md2 + 1e-5,
			"%d splats, %.2f l stuck" % [car.get_mud_splat_count(), stuck * 1000.0])
	# The clay thrown off lands up the arches (stage 3b): the body's material carries the Mud layer, fed by the car. Some of the clay was pressed onto the ground first, so less is left to throw.
	var spray := 0.0
	for w in 4:
		spray += car.get_wheel_mud_spray(w) / 4.0
	var body_mi: MeshInstance3D = car.get_node("Body")
	var body_mat := body_mi.get_active_material(0) as BaseMaterial3D
	var fed: Vector4 = body_mi.get_instance_shader_parameter("mud_spray")
	var clay := car.get_body_mud_color()
	_check("the body's mud takes the ground's clay colour", clay.r > 1.6 * clay.g and clay.r > 0.5, "clay %s on the body" % str(clay))
	_check("the thrown clay muds the arches, the body's material shows it", spray > 0.2 and body_mat.mud_enabled and fed.x + fed.y + fed.z + fed.w > 1.0 and not res["clean"][5] and car.get_body_mud_wetness() > 0.9,
			"spray %.2f a wheel, uniform %s, wetness %.2f" % [spray, str(fed), car.get_body_mud_wetness()])
	# A low car (15 cm under its belly) in 80 cm of slurry at half throttle: with belly drag it high-centres.
	var belly := {}
	for k in ["off", "on"]:
		await _soil_scene("soil")
		await _paint_mud(0.8, 1.0)
		var low := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		(low.get_child(0) as CollisionShape3D).position = Vector3(0, 0.1, 0)
		low.belly_drag = k == "on"
		low.tyre_clogging = false
		surface.follow_target = surface.get_path_to(low)
		var r: Array = await _drive(low, 4.0, 0.5)
		var under := low.global_position
		belly[k] = [r[0], low.get_belly_support(), low.get_belly_drag(), surface.get_ground_height_at(under) - surface.get_surface_height_at(under), low.get_body_mud_line()]
		print("  belly %s: %.2f m in 4 s, sinkage %.1f cm; belly holds %.0f N, drags %.0f N; %.1f cm trough under it" % [k, r[0], r[1] * 100.0, belly[k][1], belly[k][2], belly[k][3] * 100.0])
	_check("a low car's belly meets the slurry: held up and dragged", belly["on"][1] > 1000.0 and belly["on"][2] > 0.0 and belly["off"][1] == 0.0,
			"%.0f N up, %.0f N drag" % [belly["on"][1], belly["on"][2]])
	_check("the slurry the car sits in marks the body: a mud line round the belly", belly["on"][4] > 0.05 and belly["on"][4] < 0.4, "%.0f cm above the chassis bottom" % (belly["on"][4] * 100.0))
	_check("the belly ploughs a trough", belly["on"][3] > 0.03 and belly["on"][3] > belly["off"][3] + 0.05, "%.1f cm (%.1f cm without)" % [belly["on"][3] * 100.0, belly["off"][3] * 100.0])


# Meshes for a bench car (mud stage 3b): a painted body over the chassis box and a cab, tyres on the wheels.
func _dress_car(car: PhysXVehicle3D) -> void:
	var paint := StandardMaterial3D.new()
	paint.albedo_color = Color(0.16, 0.3, 0.22)
	paint.roughness = 0.35
	var body := MeshInstance3D.new()
	body.name = "Body"
	var bm := BoxMesh.new()
	bm.size = Vector3(1.8, 0.6, 4.0)
	body.mesh = bm
	body.material_override = paint
	body.position = (car.get_child(0) as CollisionShape3D).position
	car.add_child(body)
	var cab := MeshInstance3D.new()
	var cm := BoxMesh.new()
	cm.size = Vector3(1.6, 0.6, 1.8)
	cab.mesh = cm
	cab.material_override = paint
	cab.position = body.position + Vector3(0, 0.6, -0.3)
	car.add_child(cab)
	var rubber := StandardMaterial3D.new()
	rubber.albedo_color = Color(0.05, 0.05, 0.05)
	rubber.roughness = 0.85
	for c in car.get_children():
		if c is PhysXVehicleWheel3D:
			var t := MeshInstance3D.new()
			var tm := CylinderMesh.new()
			tm.top_radius = c.radius
			tm.bottom_radius = c.radius
			tm.height = 2.0 * c.half_width
			tm.radial_segments = 48
			t.mesh = tm
			t.material_override = rubber
			t.rotation_degrees = Vector3(0, 0, 90)
			c.add_child(t)


# Mud stage 3b: a dressed 4x4 crawls 5 s through stiff clay at half throttle (its treads pack), then is floored for
# 3 s (they throw the clay up the arches), then stands; a close frame from its right rear.
func _build_bodymud() -> void:
	kind = "soil"
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
	sun.rotation_degrees = Vector3(-35.0, 120.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 20.0
	add_child(sun)
	await _paint_mud(0.8, STIFF_MUD)
	var car := _make_car(1500.0, 700.0, Vector3(0.0, _undisturbed(Vector3.ZERO) + 0.9, 0.0))
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	var grains := _make_grains(car)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	car.throttle = 0.5
	for i in 300:
		await get_tree().physics_frame
	car.throttle = 1.0
	# --spray: the frame mid-throw (the clods in the air), else after it lands
	for i in (120 if "--spray" in OS.get_cmdline_user_args() else 180):
		await get_tree().physics_frame
	if not "--spray" in OS.get_cmdline_user_args():
		car.throttle = 0.0
		car.brake = 1.0
		for i in 60:
			await get_tree().physics_frame
	var mg: PhysXGroundGrains3D = grains.get_mud_grains()
	if mg and not "--spray" in OS.get_cmdline_user_args():
		# A few clods thrown at the right side (stage 4b), as from a passing wheel, to stick there.
		var side := car.global_transform.basis.x.normalized()
		var fwd := car.global_transform.basis.z.normalized()
		for k in 3:
			mg.kick(car.global_position + side * 2.5 - fwd * (0.6 + 0.5 * k), 0.12, 0.03, -side * 6.0 + Vector3(0, 4.0, 0))
			for i in 10:
				await get_tree().physics_frame
		for i in 60:
			await get_tree().physics_frame
		# --splat: one big clod placed on the side, a check of the splat drawing alone
		if "--splat" in OS.get_cmdline_user_args():
			car.add_mud_splat(car.global_transform * Vector3(0.9, 0.5, -0.6), -side * 5.0 + Vector3(0, 1.0, 0), 3e-4)
			# and two smaller ones overlapping further back, a check that they merge
			car.add_mud_splat(car.global_transform * Vector3(0.9, 0.55, -1.3), -side * 5.0 + Vector3(0, 0.5, 0), 1.2e-4)
			car.add_mud_splat(car.global_transform * Vector3(0.9, 0.5, -1.42), -side * 5.0 + Vector3(0, 1.5, 0), 0.8e-4)
			for i in 5:
				await get_tree().physics_frame
	if mg:
		print("bodymud: %.1f l of clay thrown, %d clods in the air, %d splats stuck" % [mg.get_dug_volume() * 1000.0, mg.get_airborne_count(), car.get_mud_splat_count()])
	var spray := []
	var fill := []
	for w in 4:
		spray.append(snappedf(car.get_wheel_mud_spray(w), 0.01))
		fill.append(snappedf(car.get_wheel_mud(w), 0.01))
	print("bodymud: tread fill %s, spray %s, line %.2f m, wetness %.2f" % [str(fill), str(spray), car.get_body_mud_line(), car.get_body_mud_wetness()])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([2.4, 0.9, -3.2, 0.6, 0.35, -1.1])
	var xf := car.global_transform
	cam.position = xf * Vector3(c[0], c[1], c[2])
	cam.look_at(xf * Vector3(c[3], c[4], c[5]))
	cam.fov = 45.0
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# Mud stage 4e: a pair of legs walked 4 m through 15 cm of slurry, stopped mid-stride (--cam=x,y,z,tx,ty,tz from the
# legs).
func _build_legs(p_out := false) -> void:
	kind = "soil"
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
	sun.rotation_degrees = Vector3(-35.0, 120.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 10.0
	add_child(sun)
	await _paint_mud(0.15, 0.85)
	var legs := _make_legs(Vector3(-4.0, 0, 0))
	surface.follow_target = surface.get_path_to(legs)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	await _walk_legs(legs, 4.0, 1.6)
	if p_out:
		# --view=tracked: on out of the mud onto the dried ground (the clog test's way), the boots leaving prints.
		surface.moisture = 0.1
		surface.settle(1.0, 0.5)
		await _walk_legs(legs, 3.0, 1.4)
	var lm := legs.get_node("LegMud") as GroundLegMud3D
	print("legs: lines %.1f and %.1f cm, flecks %.2f and %.2f" % [lm.get_mud_line(0) * 100.0, lm.get_mud_line(1) * 100.0, lm.get_mud_flecks(0), lm.get_mud_flecks(1)])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else (PackedFloat32Array([-1.2, 1.1, 1.0, -1.6, 0.0, 0.0]) if p_out else PackedFloat32Array([0.6, 0.45, 1.3, 0.0, 0.2, 0.0]))
	cam.position = legs.global_position + Vector3(c[0], c[1], c[2])
	cam.look_at(legs.global_position + Vector3(c[3], c[4], c[5]))
	cam.fov = 40.0
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


func _build_wade() -> void:
	near_on = false
	kind = "soil"
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.9, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	surface.mud_layer = false
	surface.standing_water = true
	surface.moisture = 0.9
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
	sun.rotation_degrees = Vector3(-30.0, 150.0, 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	# Two ruts at the 4x4's track, 12 cm deep, filled by the ground water to its table 6 cm down (sped up).
	for x in [-0.8, 0.8]:
		surface.press_box(Vector3(x, 0, 0), Vector2(0.18, 7.0), 0.0, 0.0, 0.12)
	surface.sync_deformation()
	surface.runoff_time_scale = 1000.0
	surface.settle(240.0, 0.05)
	# Then firm ground to drive on, the water held (its sources and sinks stopped).
	surface.runoff_time_scale = 0.0
	surface.moisture = 0.3
	print("wade: %.1f cm of water in the ruts" % (surface.get_standing_water_at(Vector3(0.8, 0, 0)) * 100.0))
	var car := _make_car(1500.0, 700.0, Vector3(0.0, _undisturbed(Vector3(0, 0, -6.0)) + 0.9, -6.0))
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	var grains := _make_grains(car)
	var vol0 := _water_volume(Vector3(0, 0, 0), 7.2).x

	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	if splash_test:
		# Clods stuck low on a sill, just under the ruts' water line, and high on a door (mud stage 4c): the water
		# should wash the low one off, not the high one.
		# (Its origin rides about 32 cm over the water in the ruts.)
		var cp := car.global_position
		car.add_mud_splat(Vector3(0.9, cp.y - 0.34, cp.z), Vector3(1, 0, 0), 1e-4)
		car.add_mud_splat(Vector3(0.9, cp.y + 0.6, cp.z), Vector3(1, 0, 0), 1e-4)
	car.throttle = 0.6
	var t := 0.0
	while t < 10.0 and car.global_position.z < -0.4:
		await get_tree().physics_frame
		t += 1.0 / 60.0
	var c := car.global_position
	var v := car.get_linear_velocity().length()
	print("wade: car at %s, %.1f m/s, Froude %.1f in the rut" % [c, v, v / sqrt(9.81 * maxf(surface.get_standing_water_at(Vector3(0.8, 0, c.z + 3.0)), 1e-3))])
	print("wade: clay in the rut water %.1f kg/m3 behind the car, %.1f ahead (muddy water)" % [surface.get_water_mud_at(Vector3(0.8, 0, c.z - 2.0)), surface.get_water_mud_at(Vector3(0.8, 0, c.z + 3.0))])
	var wg := grains.get_water_grains()
	if wg:
		print("wade: %d drops in the air (%.2f l), %.2f l thrown" % [wg.get_airborne_count(), wg.get_airborne_volume() * 1000.0, wg.get_dug_volume() * 1000.0])
	if splash_test:
		# Past Froude 1 the tyres throw sheets of the rut's water (mud stage 4c): taken from the field and landed
		# back on it, volume kept. Then the car stops and the drops come down.
		car.throttle = 0.0
		car.brake = 1.0
		var thrown := wg.get_dug_volume() if wg else 0.0
		var flying := wg.get_airborne_volume() if wg else 0.0
		for i in 240:
			await get_tree().physics_frame
		var landed := wg.get_deposited_volume() if wg else 0.0
		var air := wg.get_airborne_volume() if wg else 0.0
		var vol1 := _water_volume(Vector3(0, 0, 0), 7.2).x
		print("  thrown %.2f l (%.2f l in the air at once), landed %.2f l, still in the air %.2f l; ruts %.2f l before, %.2f l after" % [thrown * 1000.0, flying * 1000.0, landed * 1000.0, air * 1000.0, vol0 * 1000.0, vol1 * 1000.0])
		var moved := surface.get_water_moved()
		var owed := surface.get_water_owed()
		print("  the field: %.2f l landed in it, %.2f l taken from it, %.2f l owed (thin films on the dry flat soak in: %.2f l)" % [moved.x * 1000.0, moved.y * 1000.0, owed * 1000.0, (vol0 + moved.x - moved.y - vol1) * 1000.0])
		var low := 1.0
		var high := 0.0
		for k in car.get_mud_splat_count():
			var sp := car.get_mud_splat(k)
			if k == 0:
				low = minf(low, sp.w)
			else:
				high = maxf(high, sp.w)
		print("  splats left: low %.2f, high %.2f" % [low, high])
		_check("the water washes the clods under its line off the body, not those above", low < 0.5 and high > 0.8, "low %.2f, high %.2f left" % [low, high])
		_check("tyres past Froude 1 throw the rut's water up as sheets", thrown > 0.0005 and flying > 0.0001, "%.2f l thrown, %.2f l in the air" % [thrown * 1000.0, flying * 1000.0])
		# The field's books: what landed in it less what was taken equals what is still owed (a take that found less
		# than its read back showed is owed, paid back by the water landing next), and the drops are all down.
		var dug := wg.get_dug_volume()
		_check("the thrown water lands back on the field, volume kept", air < 0.01 * dug and absf(landed - dug) < 0.001 * dug and owed < 0.05 * dug and absf(moved.x - moved.y - owed) < 0.01 * dug,
			"%.2f l thrown, %.2f l landed, %.3f l in the air; the field %.2f l in, %.2f l out, %.2f l owed" % [dug * 1000.0, landed * 1000.0, air * 1000.0, moved.x * 1000.0, moved.y * 1000.0, owed * 1000.0])
		return
	var cam := Camera3D.new()
	add_child(cam)
	var cc := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([2.2, 1.5, c.z + 3.4, 0.8, 0.0, c.z + 1.5])
	cam.position = Vector3(cc[0], cc[1], cc[2])
	cam.look_at(Vector3(cc[3], cc[4], cc[5]))
	cam.fov = 45.0
	cam.current = true
	# Held still for the frame: drawn full size, a frame takes long enough for many physics steps to run.
	get_tree().paused = true
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# The frame, path traced unless --raster; with --pair, the raster frame first as <out>_raster.png and then the path
# traced one from the same run, everything held still between them (the same clods, the same water).
func _save_view(env: Environment) -> void:
	if "--pair" in OS.get_cmdline_user_args() and not out.is_empty():
		var pt_out := out
		out = pt_out.get_basename() + "_raster.png"
		env.pathtracing_enabled = false
		await _save_frame()
		get_tree().paused = true
		out = pt_out
		env.pathtracing_enabled = true
		await _save_frame()
		return
	env.pathtracing_enabled = not raster
	await _save_frame()


# Bekker's rigid wheel, written out here apart from the engine's: z0 = (3 W / ((3 - n) b k sqrt(D)))^(2 / (2n + 1)).
func _bekker_sinkage(load: float, radius: float, width: float, n: float, kc: float, kphi: float) -> float:
	var k := (kc / width + kphi) * 1000.0
	return pow(3.0 * load / ((3.0 - n) * width * k * sqrt(2.0 * radius)), 2.0 / (2.0 * n + 1.0))


func _make_car(mass: float, torque: float, at: Vector3) -> PhysXVehicle3D:
	var car := PhysXVehicle3D.new()
	car.mass = mass
	car.max_engine_torque = torque
	car.can_sleep = false
	var cs := CollisionShape3D.new()
	cs.shape = BoxShape3D.new()
	(cs.shape as BoxShape3D).size = Vector3(1.8, 0.6, 4.0)
	cs.position = Vector3(0, 0.45, 0)
	car.add_child(cs)
	for k in 4:
		var w := PhysXVehicleWheel3D.new()
		w.position = Vector3(0.8 if k % 2 == 0 else -0.8, 0.0, 1.3 if k < 2 else -1.3)
		w.radius = 0.35
		w.half_width = 0.13
		w.use_as_steering = k < 2
		w.use_as_traction = true
		car.add_child(w)
	car.position = at
	add_child(car)
	return car


# Drives a car straight at a steady throttle for p_seconds; returns [distance, mean sinkage, mean |slip|].
func _drive(car: PhysXVehicle3D, p_seconds: float, p_throttle := 1.0) -> Array:
	for i in 30:
		await get_tree().physics_frame
	var start := car.global_position
	car.throttle = p_throttle
	var sink := 0.0
	var slip := 0.0
	var n := 0
	for i in int(p_seconds * 60.0):
		await get_tree().physics_frame
		for w in 4:
			sink += car.get_wheel_sinkage(w)
			slip += absf(car.get_wheel_slip(w))
			n += 1
	car.throttle = 0.0
	var d := car.global_position - start
	return [Vector2(d.x, d.z).length(), sink / maxi(n, 1), slip / maxi(n, 1)]


func _soil_scene(kind_name: String) -> void:
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply(kind_name)
	var floor_body := StaticBody3D.new()
	var fc := CollisionShape3D.new()
	fc.shape = BoxShape3D.new()
	(fc.shape as BoxShape3D).size = Vector3(400, 2, 400)
	fc.position = Vector3(0, -1, 0)
	floor_body.add_child(fc)
	add_child(floor_body)


func _run_soil_tests() -> void:
	await _soil_scene("dry_sand")
	var o := Vector3.ZERO
	# One wheel: 3.5 kN on a 0.35 m radius, 0.26 m wide, rigid (inflation 0).
	var w: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	var ref := _bekker_sinkage(3500.0, 0.35, 0.26, 1.1, 0.99, 1528.43)
	_check("rigid wheel sinkage, dry sand (Wong table 2.2)", absf(w["sinkage"] - ref) < 0.002 and ref > 0.04, "%.1f cm, Bekker %.1f cm" % [w["sinkage"] * 100.0, ref * 100.0])
	var rc: float = w["compaction"]
	var rc_ref := 0.26 * (0.99 / 0.26 + 1528.43) * 1000.0 * pow(ref, 2.1) / 2.1
	_check("compaction resistance", absf(rc - rc_ref) < 0.05 * rc_ref, "%.0f N (Bekker %.0f N), bulldozing %.0f N, %.0f%% of the load in all" % [rc, rc_ref, w["bulldozing"], 100.0 * w["resistance"] / 3500.0])
	var aired: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 50.0, 0.0, 0.0)
	var hard: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 240.0, 0.0, 0.0)
	_check("airing down sinks less", aired["sinkage"] < hard["sinkage"] * 0.8 and aired["resistance"] < hard["resistance"], "50 kPa %.1f cm, %.0f N; 240 kPa %.1f cm, %.0f N" % [aired["sinkage"] * 100.0, aired["resistance"], hard["sinkage"] * 100.0, hard["resistance"]])
	var s50: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.5, 0.0)
	_check("slip sinkage (Lyasko 2010)", absf(s50["sinkage"] / w["sinkage"] - 2.0) < 0.02, "%.1f cm at 50%% slip, %.2f times" % [s50["sinkage"] * 100.0, s50["sinkage"] / w["sinkage"]])
	# Janosi-Hanamoto: traction grows with slip toward the shear limit.
	var curve := []
	var rising := true
	var last := -1.0
	for sl in [0.02, 0.05, 0.1, 0.2, 0.4, 0.8]:
		var t: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, sl, 0.0)
		var share: float = t["traction"] / t["shear_limit"]
		rising = rising and share > last
		last = share
		curve.append("%.0f%%: %.2f" % [sl * 100.0, share])
	_check("traction rises with slip (Janosi-Hanamoto)", rising and last > 0.9, ", ".join(curve))
	var lim: float = w["shear_limit"]
	_check("dry sand shear limit near tan(phi) W", lim > 3500.0 * 0.5 and lim < 3500.0 * 0.75, "%.0f N for 3500 N" % lim)
	surface.moisture = 0.4
	var damp: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	_check("damp sand: firmer and grippier", damp["sinkage"] < w["sinkage"] * 0.6 and damp["shear_limit"] > lim, "%.1f cm, %.0f N resistance, shear limit %.0f N" % [damp["sinkage"] * 100.0, damp["resistance"], damp["shear_limit"]])
	# Multi-pass: in the rut already pressed, little work is left.
	surface.moisture = 0.0
	surface.clear_deformation()
	surface.press_wheel(o, Vector2(0.26, 0.6), 0.0, w["sinkage"])
	surface.sync_deformation()
	var again: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	_check("second pass in the rut", again["resistance"] < 0.3 * w["resistance"], "%.0f N against %.0f N on fresh sand" % [again["resistance"], w["resistance"]])

	# Walker footing.
	surface.clear_deformation()
	var f_dry: Dictionary = surface.get_footing_at(o)
	surface.moisture = 0.4
	var f_damp: Dictionary = surface.get_footing_at(o)
	surface.moisture = 0.0
	_check("walking effort (Lejeune 1998: 1.6 to 2.5 on dry sand)", f_dry["effort"] > 1.6 and f_dry["effort"] < 2.5 and f_damp["effort"] < 1.5, "dry %.2f, damp %.2f" % [f_dry["effort"], f_damp["effort"]])
	_check("no slide on the flat", (f_dry["slide"] as Vector3).length() < 1e-4, "%.3f m/s2" % (f_dry["slide"] as Vector3).length())
	await _setup("slope", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	var sp := Vector3(1.0, height.get_height(Vector3(1.0, 0, 0)), 0.0)
	var f_slope: Dictionary = surface.get_footing_at(sp)
	var slide: Vector3 = f_slope["slide"]
	_check("a footing on a 50 degree sand face slides down it", slide.length() > 2.0 and slide.x < 0.0 and slide.y < 0.0, "%.1f m/s2, %s" % [slide.length(), slide])

	# A 300 kg stone block (0.4 m) shoved across dry sand at 3 m/s on a 0.3 friction floor: the sand it sinks
	# into and ploughs (1.7 cm, Bekker) stops it sooner.
	var slides := []
	for plough in [false, true]:
		await _soil_scene("dry_sand")
		surface.ploughing = plough
		var pm := PhysicsMaterial.new()
		pm.friction = 0.3
		for c in get_children():
			if c is StaticBody3D:
				c.physics_material_override = pm
		var crate := RigidBody3D.new()
		crate.mass = 300.0
		crate.physics_material_override = pm
		var cc := CollisionShape3D.new()
		cc.shape = BoxShape3D.new()
		(cc.shape as BoxShape3D).size = Vector3(0.4, 0.4, 0.4)
		crate.add_child(cc)
		crate.add_to_group("ground_deform")
		crate.position = Vector3(0, 0.201, 0)
		add_child(crate)
		surface.follow_target = surface.get_path_to(crate)
		for i in 10:
			await get_tree().physics_frame
		crate.linear_velocity = Vector3(3.0, 0, 0)
		for i in 180:
			await get_tree().physics_frame
		slides.append(crate.global_position.x)
	_check("a stone block shoved over dry sand ploughs to a stop sooner", slides[1] < 0.9 * slides[0], "%.2f m against %.2f m on the floor's friction alone" % [slides[1], slides[0]])

	# A 1500 kg 4x4, 700 N m a wheel, for 4 s at half throttle (easing in, as one drives on sand): firm ground,
	# damp sand, dry sand at road pressure (200 kPa) and aired down to 60 kPa; then floored on dry sand.
	var results := {}
	for k in ["firm", "damp_sand", "dry_sand", "dry_sand_aired", "dry_sand_floored"]:
		await _soil_scene("damp_sand" if k == "damp_sand" else "dry_sand")
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		if k == "firm":
			car.soft_soil = false
		if k.begins_with("dry_sand_"):
			for c in car.get_children():
				if c is PhysXVehicleWheel3D:
					c.tire_pressure = 60.0
		surface.follow_target = surface.get_path_to(car)
		var start := car.global_position
		results[k] = await _drive(car, 4.0, 1.0 if k == "dry_sand_floored" else 0.5)
		surface.sync_deformation()
		# The deepest rut along the way.
		var rut := 0.0
		var fwd := car.get_forward()
		var side := fwd.cross(Vector3.UP).normalized()
		var steps := int(results[k][0] / 0.1)
		for j in range(-16, steps + 17):
			var q0 := start + fwd * (j * 0.1)
			for x in 41:
				var q := q0 + side * (x - 20) * 0.05
				rut = maxf(rut, surface.get_dent_at(q) - surface.get_rim_at(q))
		results[k].append(rut)
		print("  %s: %.1f m in 4 s, sinkage %.1f cm, slip %.2f, rut %.1f cm" % [k, results[k][0], results[k][1] * 100.0, results[k][2], rut * 100.0])
	_check("damp sand drives, slower than firm ground (motion resistance 0.15 to 0.3 of the load on sand, Wong table 1.1)",
			results["damp_sand"][0] > 3.0 and results["damp_sand"][0] < 0.5 * results["firm"][0], "%.1f m against %.1f m" % [results["damp_sand"][0], results["firm"][0]])
	_check("dry sand at road pressure: the car does not get going", results["dry_sand"][0] < 1.0 and results["dry_sand"][1] > 0.08,
			"%.1f m, sinkage %.1f cm" % [results["dry_sand"][0], results["dry_sand"][1] * 100.0])
	_check("aired down to 60 kPa it sinks less and gets further", results["dry_sand_aired"][0] > 3.0 * results["dry_sand"][0] and results["dry_sand_aired"][1] < 0.7 * results["dry_sand"][1],
			"%.1f m, sinkage %.1f cm" % [results["dry_sand_aired"][0], results["dry_sand_aired"][1] * 100.0])
	_check("floored, the wheels spin and dig in (slip sinkage)", results["dry_sand_floored"][2] > 0.5 and results["dry_sand_floored"][1] > 1.5 * results["dry_sand_aired"][1],
			"slip %.2f, sinkage %.1f cm" % [results["dry_sand_floored"][2], results["dry_sand_floored"][1] * 100.0])
	_check("wheels print ruts", results["damp_sand"][3] > 0.01 and results["dry_sand_aired"][3] > 0.03, "damp %.1f cm, dry aired down %.1f cm" % [results["damp_sand"][3] * 100.0, results["dry_sand_aired"][3] * 100.0])
	_check("firm ground: no sinkage", results["firm"][1] == 0.0, "%.2f cm" % (results["firm"][1] * 100.0))
	print("step %.2f ms, field on the %s" % [surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU"])


# Ripple statistics over the layer (or a square of it): [wavelength along x, along z (m, from zero
# crossings), slope energy along x over z, peak to peak (m), rms (m), mean (m)].
func _ripple_stats(from_x := 0, from_z := 0, size := -1) -> Array:
	var h := surface.get_ripple_heights()
	var n := surface.get_ripple_size()
	var c := surface.get_ripple_cell()
	if size < 0:
		size = n
	var mean := 0.0
	var lo := 1e9
	var hi := -1e9
	for z in range(from_z, from_z + size):
		for x in range(from_x, from_x + size):
			var v := h[z * n + x]
			mean += v
			lo = minf(lo, v)
			hi = maxf(hi, v)
	mean /= float(size * size)
	var rms := 0.0
	var ex := 0.0
	var ez := 0.0
	var zx := 0
	var zz := 0
	for z in range(from_z, from_z + size - 1):
		for x in range(from_x, from_x + size - 1):
			var v := h[z * n + x] - mean
			var vx := h[z * n + x + 1] - mean
			var vz := h[(z + 1) * n + x] - mean
			rms += v * v
			ex += (vx - v) * (vx - v)
			ez += (vz - v) * (vz - v)
			if (v < 0.0) != (vx < 0.0): zx += 1
			if (v < 0.0) != (vz < 0.0): zz += 1
	var rows := float(size - 1)
	var len := (size - 1) * c
	var lx := 2.0 * len * rows / maxf(zx, 1.0)
	var lz := 2.0 * len * rows / maxf(zz, 1.0)
	return [lx, lz, ex / maxf(ez, 1e-12), hi - lo, sqrt(rms / (rows * rows)), mean]


func _run_wind_tests() -> void:
	await _soil_scene("dry_sand")
	surface.wind_override = Vector2(8.0, 0.0)
	surface.wind_ripples = true
	var t0 := Time.get_ticks_msec()
	while not surface.is_ripples_ready() and Time.get_ticks_msec() - t0 < 60000:
		await get_tree().physics_frame
	for i in 3:
		await get_tree().physics_frame
	var grow_s := (Time.get_ticks_msec() - t0) / 1000.0
	var st := _ripple_stats()
	print("  grown in %.1f s (a worker); 8 m/s along x: wavelength %.1f cm along, %.1f cm across, slope energy %.0fx along, %.1f mm peak to peak" % [grow_s, st[0] * 100.0, st[1] * 100.0, st[2], st[3] * 1000.0])
	_check("ripples 5 to 15 cm long along the wind (Bagnold 1941; Andreotti 2006)", st[0] > 0.05 and st[0] < 0.15, "%.1f cm" % (st[0] * 100.0))
	_check("crests across the wind", st[2] > 4.0 and st[1] > 3.0 * st[0], "slope energy along / across %.1f, crests %.0f cm long between turns" % [st[2], st[1] * 100.0])
	_check("ripple index about 15 (7 mm at 11 cm)", st[3] > 0.003 and st[3] < 0.012, "%.1f mm peak to peak" % (st[3] * 1000.0))
	# At real time the wind moves a cell's sand by q: a step every few seconds at 8 m/s.
	var s0 := surface.get_ripple_steps()
	for i in 600:
		await get_tree().physics_frame
	var real_steps := surface.get_ripple_steps() - s0
	print("  real time at 8 m/s: %d steps in 10 s" % real_steps)
	# Under the threshold (4 m/s at 10 m): nothing moves.
	surface.wind_override = Vector2(4.0, 0.0)
	s0 = surface.get_ripple_steps()
	surface.ripple_time_scale = 1000.0
	for i in 60:
		await get_tree().physics_frame
	_check("under the threshold nothing moves", surface.get_ripple_steps() == s0, "%d steps at 4 m/s" % (surface.get_ripple_steps() - s0))
	# The layer keeps its sand: the mean stays where it was over many steps.
	var mean0: float = _ripple_stats()[5]
	# A print in the middle wipes the ripples under it.
	var o := Vector3(0, 0, 0)
	surface.press_box(o, Vector2(0.3, 0.6), 0.0, 0.0, 0.05)
	surface.sync_deformation()
	surface.wind_override = Vector2(0.0, 0.0)
	for i in 5:
		await get_tree().physics_frame
	var org := surface.get_ripple_origin()
	var cell := surface.get_ripple_cell()
	var ci := int((o.x - org.x) / cell) - 8
	var cj := int((o.z - org.y) / cell) - 8
	var wiped: float = _ripple_stats(ci, cj, 16)[4]
	var around: float = _ripple_stats(ci - 60, cj, 16)[4]
	_check("a print wipes the ripples under it", wiped < 0.2 * around, "rms %.2f mm in the print, %.2f mm beside it" % [wiped * 1000.0, around * 1000.0])
	# The wind turns 90 degrees and blows hard for a while (time scaled): the ripples grow back over the print
	# and turn across the new wind.
	surface.wind_override = Vector2(0.0, 9.0)
	surface.ripple_time_scale = 3000.0
	s0 = surface.get_ripple_steps()
	for i in 240:
		await get_tree().physics_frame
	var st2 := _ripple_stats()
	var back: float = _ripple_stats(ci, cj, 16)[4]
	print("  after %d steps at 9 m/s along z: wavelength %.1f cm along z, slope energy along x / z %.2f" % [surface.get_ripple_steps() - s0, st2[1] * 100.0, st2[2]])
	_check("they grow back over the print", back > 0.5 * around, "rms %.2f mm in the print" % (back * 1000.0))
	_check("a wind shift turns them across the new wind", st2[2] < 0.5, "slope energy along x / z %.2f" % st2[2])
	_check("the layer keeps its sand (the mean holds)", absf(st2[5] - mean0) < 2e-5, "mean %.4f mm, was %.4f mm (the print's ripples wiped)" % [st2[5] * 1000.0, mean0 * 1000.0])
	# Damp sand holds: no saltation.
	surface.moisture = 0.4
	for i in 130:
		await get_tree().physics_frame
	var h0 := surface.get_ripple_heights()
	for i in 60:
		await get_tree().physics_frame
	var h1 := surface.get_ripple_heights()
	var diff := 0.0
	for i in h0.size():
		diff = maxf(diff, absf(h1[i] - h0[i]))
	_check("damp sand holds its ripples (cohesion stops saltation)", diff < 1e-4, "largest change %.3f mm" % (diff * 1000.0))
	_check("damp sand gives the wind nothing", float(surface.get_material_at(o)["blown"]) == 0.0, "blown share %.2f at moisture 0.4" % float(surface.get_material_at(o)["blown"]))
	surface.moisture = 0.0
	await _run_streamer_tests()


# Dune grass (sand trapping by plants), with a stand-in for the vegetation module: a 2 m tuft of lambda 0.3 in a
# 12 m/s wind over dry sand.
func _run_shelter_tests() -> void:
	await _soil_scene("dry_sand")
	var r0 := GroundSurface3D.shelter_ratio_for(0.0)
	var r1 := GroundSurface3D.shelter_ratio_for(0.3)
	var u10 := 12.0
	var ustar := 0.4 * u10 / log(10.0 / 1e-4)
	_check("Raupach drag partition: open sand keeps all its stress, marram cover (lambda 0.3) stops saltation", r0 == 1.0 and ustar * r1 < 0.2, "u*s/u* %.2f at 0.3: %.3f m/s against the 0.2 m/s threshold" % [r1, ustar * r1])
	_check("lambda clamped at 0.5", GroundSurface3D.shelter_ratio_for(5.0) == GroundSurface3D.shelter_ratio_for(0.5), "%.3f" % GroundSurface3D.shelter_ratio_for(5.0))
	var grass := Node3D.new()
	grass.set_script(load("res://shelter_fake.gd"))
	add_child(grass)
	surface.wind_shelters = [surface.get_path_to(grass)]
	surface.wind_override = Vector2(u10, 0.0)
	for i in 70:
		await get_tree().physics_frame
	var up := Vector3(-4.0, 0, 0)
	var edge := Vector3(-0.75, 0, 0)
	var inside := Vector3(0.5, 0, 0)
	var q_up := surface.get_sand_flux_at(up)
	var q_in := surface.get_sand_flux_at(inside).length()
	var q_open: float = 2.6 * (1.2 / 9.81) * ustar * ustar * (ustar - 0.2)
	_check("open sand upwind carries the saturated flux, along the wind (Lettau and Lettau)", absf(q_up.length() - q_open) < 0.05 * q_open and q_up.x > 0.0, "%.4f kg/m/s, expected %.4f" % [q_up.length(), q_open])
	_check("the flux dies out inside the tuft (saturation length about 1 m)", q_in < 0.4 * q_open, "%.4f kg/m/s 1.25 m in" % q_in)
	var d_edge := surface.get_deposition_rate_at(edge)
	var d_up := surface.get_deposition_rate_at(up)
	print("  deposition: %.2f mm/h on the tuft's windward edge, %.3f mm/h upwind" % [d_edge * 3.6e6, d_up * 3.6e6])
	_check("sand lands at the tuft's windward edge, at Q / (rho l) (1 to 40 mm/h in a gale)", d_edge * 3.6e6 > 1.0 and d_edge * 3.6e6 < 40.0, "%.2f mm/h" % (d_edge * 3.6e6))
	_check("none on open sand in equilibrium", absf(d_up) < 0.02 * d_edge, "%.4f mm/h" % (d_up * 3.6e6))
	_check("the streamers thin among the grass (the wind's share of the sand)", surface.get_blown_at(inside.x, inside.z) < 0.2 * surface.get_blown_at(up.x, up.z), "%.2f inside, %.2f outside" % [surface.get_blown_at(inside.x, inside.z), surface.get_blown_at(up.x, up.z)])
	# Two hours of gale (time scaled): a mound builds in the tuft.
	var h0 := surface.get_surface_height_at(edge)
	surface.ripple_time_scale = 1200.0
	for i in 360:
		await get_tree().physics_frame
	surface.ripple_time_scale = 1.0
	surface.sync_deformation()
	for i in 3:
		await get_tree().physics_frame
	var dep := surface.get_sand_deposit_at(edge)
	var rise := surface.get_surface_height_at(edge) - h0
	print("  after %.1f h: %.1f mm deposited at the edge, the surface %.1f mm higher; %.1f mm upwind" % [6.0 * 1200.0 / 3600.0, dep * 1000.0, rise * 1000.0, surface.get_sand_deposit_at(up) * 1000.0])
	_check("a mound builds round the grass (nebkha)", dep > 0.005 and rise > 0.5 * dep, "%.1f mm deposited, %.1f mm risen" % [dep * 1000.0, rise * 1000.0])
	_check("open sand gets none", surface.get_sand_deposit_at(up) == 0.0, "%.1f mm" % (surface.get_sand_deposit_at(up) * 1000.0))
	_check("the wind step is cheap", surface.get_wind_step_usec() < 3000.0, "%.2f ms twice a second" % (surface.get_wind_step_usec() / 1000.0))
	surface.wind_override = Vector2.ZERO


# Runoff channels: a falling tide leaves the beach soaked; the water seeping out of it runs down the face and cuts
# rills before the sand drains (time scaled for the sand moved, the draining in real time).
func _runoff_beach(scale: float) -> Node3D:
	await _setup("beach", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	get_node("Probe").position = Vector3(0, 0, 4)
	var sea := Node3D.new()
	sea.set_script(load("res://tide_fake.gd"))
	add_child(sea)
	sea.level = 0.6 # high water at z = 7.2
	surface.water_sources = [surface.get_path_to(sea)]
	surface.runoff_channels = true
	surface.runoff_time_scale = scale
	for i in 900: # soaked under the tide
		await get_tree().physics_frame
	sea.level = 0.1 # low water at z = 1.2
	return sea


# RUNOFF_DUMP=path.png: the cut over the face from above (light cut, brown fill, 20 mm full scale), 1.5 cm texels.
func _runoff_dump() -> void:
	var dump := OS.get_environment("RUNOFF_DUMP")
	if dump == "":
		return
	# Hillshade of the cut over x -2..2, z 1.5..5.5 at 7.8 mm, lit from the left at a low angle, so pits and
	# smooth grooves read apart; the cut's depth in red.
	var n := 512
	var s := 4.0 / n
	var cut := PackedFloat32Array()
	cut.resize(n * n)
	var deepest := 0.0
	for py in n:
		for px in n:
			var c := surface.get_runoff_cut_at(Vector3(-2.0 + px * s, 0, 5.5 - py * s))
			cut[py * n + px] = c
			deepest = maxf(deepest, c)
	var img := Image.create(n, n, false, Image.FORMAT_RGB8)
	for py in n:
		for px in n:
			var c := cut[py * n + px]
			var gx := (cut[py * n + mini(px + 1, n - 1)] - cut[py * n + maxi(px - 1, 0)]) / (2.0 * s)
			var shade := clampf(0.55 + 1.5 * gx, 0.0, 1.0)
			var r := clampf(shade + c / 0.008, 0.0, 1.0)
			img.set_pixel(px, py, Color(r, shade, shade))
	img.save_png(dump)
	# And the raw cut (m, float) beside it for other looks.
	var raw := FileAccess.open(dump.get_basename() + ".f32", FileAccess.WRITE)
	if raw:
		raw.store_buffer(cut.to_byte_array())
	# The standing water's depth (m, float) over the same window.
	var wd := PackedFloat32Array()
	wd.resize(n * n)
	for py in n:
		for px in n:
			wd[py * n + px] = surface.get_standing_water_at(Vector3(-2.0 + px * s, 0, 5.5 - py * s))
	var rawd := FileAccess.open(dump.get_basename() + "_water.f32", FileAccess.WRITE)
	if rawd:
		rawd.store_buffer(wd.to_byte_array())
	var wsum := 0.0
	var wmax := 0.0
	var wet := 0
	for py in range(0, n, 8):
		for px in range(0, n, 8):
			var w: float = surface.get_runoff_at(Vector3(-2.0 + px * s, 0, 5.5 - py * s)).x
			wsum += w
			wmax = maxf(wmax, w)
			wet += 1 if w > 5e-5 else 0
	print("  runoff cut map saved, deepest %.1f mm; water mean %.3f mm, max %.2f mm, %.0f%% over 0.05 mm" % [deepest * 1000.0, wsum / 4096.0 * 1000.0, wmax * 1000.0, wet / 40.96])

func _run_runoff_tests() -> void:
	var tc := 0.047 * 1650.0 * 9.81 * 0.00025
	_check("no bed load under the Shields threshold, by shear (Shields 1936)", GroundSurface3D.bed_load_shear(0.9 * tc) == 0.0 and GroundSurface3D.bed_load_shear(1.2 * tc) > 0.0, "critical shear %.2f Pa" % tc)
	var t_film := 1000.0 * 9.81 * 0.0007 / 12.0
	_check("a 0.7 mm seepage film on a 1 in 12 beach moves fine sand (rho g h S)", GroundSurface3D.bed_load_shear(t_film) > 0.0, "%.2f Pa, %.6f m2/s" % [t_film, GroundSurface3D.bed_load_shear(t_film)])
	await _runoff_beach(120.0)
	for i in 120:
		await get_tree().physics_frame
	var face := Vector3(0.3, 0, 4.0)
	var r: Vector2 = surface.get_runoff_at(face)
	print("  2 s after the tide: saturation %.2f, sheet %.2f mm at %.2f m/s on the face" % [surface.get_saturation_at(face), r.x * 1000.0, r.y])
	_check("the soaked face seeps and water runs down it", r.x > 0.0 and r.y > 0.0, "%.2f mm, %.2f m/s" % [r.x * 1000.0, r.y])
	_check("none above the tide's reach", surface.get_runoff_at(Vector3(0, 0, 11.0)).x == 0.0, "%.2f mm" % (surface.get_runoff_at(Vector3(0, 0, 11.0)).x * 1000.0))
	var peak_us := 0.0
	var frames := int(OS.get_environment("RUNOFF_FRAMES")) if OS.get_environment("RUNOFF_FRAMES") != "" else 2400
	for i in frames: # 40 s (RUNOFF_FRAMES for a quicker look)
		await get_tree().physics_frame
		peak_us = maxf(peak_us, surface.get_runoff_step_usec())
	surface.sync_deformation()
	for i in 3:
		await get_tree().physics_frame
	# Across the face at three heights: rills cut where the flow gathered, not an even lowering.
	var cuts := PackedFloat32Array()
	var total := 0.0
	var moved := 0.0
	var fan := 0.0
	var z := 0.0
	while z < 8.0:
		var x := -6.0
		while x < 6.0:
			var c := surface.get_runoff_cut_at(Vector3(x, 0, z))
			total += c
			moved += absf(c)
			if z < 2.25:
				fan += c
			if z > 2.5 and z < 6.0:
				cuts.append(c)
			x += 0.125
		z += 0.125
	if OS.get_environment("RUNOFF_TRACE") == "1":
		var zz := -1.0
		while zz < 9.0:
			var row := 0.0
			var wrow := 0.0
			var xx := -6.0
			while xx < 6.0:
				row += surface.get_runoff_cut_at(Vector3(xx, 0, zz))
				wrow += surface.get_runoff_at(Vector3(xx, 0, zz)).x
				xx += 0.125
			print("    z %.2f: mean %.2f mm, water %.3f mm" % [zz, row / 96.0 * 1000.0, wrow / 96.0 * 1000.0])
			zz += 0.25
	_runoff_dump()
	var sorted := cuts.duplicate()
	sorted.sort()
	var top := sorted[int(sorted.size() * 0.99)]
	var median := sorted[sorted.size() / 2]
	var deepest := surface.get_surface_height_at(Vector3(0, 0, 4)) # for the print
	print("  after %.0f min (scaled): cut %.1f mm at the 99th percentile, median %.2f mm; %.0f l moved; laid at the waterline %.0f l" % [42.0 * 120.0 / 60.0, top * 1000.0, median * 1000.0, moved * 0.125 * 0.125 * 500.0, -fan * 0.125 * 0.125 * 1000.0])
	_check("rills cut into the face (several mm)", top > 0.003, "%.1f mm" % (top * 1000.0))
	_check("the cuts gather into channels, the face between them barely lowered", median < 0.25 * top, "median %.2f mm against %.1f mm" % [median * 1000.0, top * 1000.0])
	_check("the sand they carry is laid down at the foot of the face, above the waterline", fan < 0.0, "%.1f l" % (-fan * 0.125 * 0.125 * 1000.0))
	_check("sand is kept: what is cut is laid down", absf(total) < 0.3 * moved, "net %.1f l of %.1f l moved" % [total * 0.125 * 0.125 * 1000.0, moved * 0.125 * 0.125 * 500.0])
	_check("the runoff step fits a frame budget on the CPU", peak_us < 40000.0, "%.1f ms at most, twice a second" % (peak_us / 1000.0))
	# Dry sand with no water and no rain: no sources, so the step returns at once.
	await _setup("beach", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	surface.runoff_channels = true
	for i in 120:
		await get_tree().physics_frame
	_check("dry sand runs nothing and costs little", surface.get_runoff_at(face).x == 0.0 and surface.get_runoff_step_usec() < 2000.0, "%.2f mm, %.2f ms" % [surface.get_runoff_at(face).x * 1000.0, surface.get_runoff_step_usec() / 1000.0])

# Sand and water (stage 4), with a stand-in for the water module.
func _run_water_tests() -> void:
	await _soil_scene("dry_sand")
	var o := Vector3.ZERO
	var uc := sqrt(0.047 * 1650.0 * 9.81 * 0.00025 / (1000.0 * 0.003))
	_check("no bed load under the Shields threshold (Shields 1936)", GroundSurface3D.bed_load(0.9 * uc) == 0.0 and GroundSurface3D.bed_load(1.2 * uc) > 0.0, "threshold %.2f m/s depth-averaged" % uc)
	var q := GroundSurface3D.bed_load(1.0)
	_check("bed load at 1 m/s of the order field swash carries (1e-5 to 1e-3 m2/s)", q > 1e-5 and q < 1e-3, "%.5f m2/s" % q)
	var water := Node3D.new()
	water.set_script(load("res://water_fake.gd"))
	add_child(water)
	surface.water_sources = [surface.get_path_to(water)]
	water.rect = Rect2(-3, -3, 6, 6)
	water.depth = 0.03
	water.flow = Vector3.ZERO
	Engine.time_scale = 10.0
	var t0 := Time.get_ticks_msec()
	# 30 s of still water over the sand.
	for i in 180:
		await get_tree().physics_frame
	var s_wet := surface.get_saturation_at(o)
	_check("sand under water saturates in seconds (Green-Ampt)", s_wet > 0.95, "saturation %.2f after 30 s under 3 cm of water" % s_wet)
	var sat_mat: Dictionary = surface.get_material_at(o)
	water.depth = -1.0
	# 3 minutes after it is gone.
	for i in 1080:
		await get_tree().physics_frame
	var s_drained := surface.get_saturation_at(o)
	_check("it drains towards field capacity (15%) in minutes", s_drained < 0.3 and s_drained >= 0.15, "saturation %.2f 3 min later" % s_drained)
	Engine.time_scale = 1.0
	var damp_mat: Dictionary = surface.get_material_at(o)
	var dry_repose := 33.0
	_check("damp sand holds a steep wall, saturated sand does not (Su et al. 2023)", float(damp_mat["repose"]) > 45.0 and absf(float(sat_mat["repose"]) - dry_repose) < 3.0,
			"repose %.0f deg damp (%.2f), %.0f deg saturated, %.0f dry" % [float(damp_mat["repose"]), s_drained, float(sat_mat["repose"]), dry_repose])
	print("  water step %.2f ms" % (surface.get_water_step_usec() / 1000.0))
	_check("the water step is cheap", surface.get_water_step_usec() < 5000.0, "%.2f ms twice a second" % (surface.get_water_step_usec() / 1000.0))
	# A boot print under water flowing at 0.6 m/s and at 0.15 m/s (under the threshold).
	for speed in [0.6, 0.15]:
		surface.clear_deformation()
		water.depth = 0.05
		water.flow = Vector3(speed, 0.0, 0.0)
		for i in 60:
			await get_tree().physics_frame
		surface.press_box(o, Vector2(0.11, 0.29), 0.3, 0.0, 0.03)
		var d0 := surface.get_dent_at(o)
		var v0 := surface.get_moved_volume()
		surface.settle(60.0)
		var d1 := surface.get_dent_at(o)
		var v1 := surface.get_moved_volume()
		if speed > 0.5:
			_check("flowing water rounds off a print within a minute", d1 < 0.35 * d0, "%.1f cm deep, %.1f cm after 60 s at %.1f m/s" % [d0 * 100.0, d1 * 100.0, speed])
			_check("no sand made or lost (Exner)", absf((v1.y - v1.x) - (v0.y - v0.x)) < 0.02 * maxf(v0.x, 1e-6), "net %.3f l, was %.3f l" % [(v1.y - v1.x) * 1000.0, (v0.y - v0.x) * 1000.0])
		else:
			_check("slow water leaves a print (under the threshold)", d1 > 0.6 * d0, "%.1f cm deep, %.1f cm after 60 s at %.2f m/s" % [d0 * 100.0, d1 * 100.0, speed])
	water.queue_free()
	surface.water_sources = []


# Streamers (stage 3): the saltating grains as particles.
func _run_streamer_tests() -> void:
	var v := GroundStreamers3D.grain_speed()
	_check("grains fly at 1 to 4 m/s (Namikas 2003)", v > 1.0 and v < 4.0, "%.2f m/s at the mean hop's top" % v)
	# A hop of h at the shader's speed: its length over its height.
	var h := 0.04
	var hop := 0.8 * 0.2 / 0.4 * log(h / 1e-4) * 2.0 * sqrt(2.0 * h / 9.81)
	_check("a hop runs 10 to 15 times its height (Bagnold 1941)", hop / h > 9.0 and hop / h < 16.0, "%.0f cm hop at 4 cm, %.1fx" % [hop * 100.0, hop / h])
	var ut := 0.2
	_check("streamers near the threshold, a sheet at twice it (Baas and Sherman 2005)", GroundStreamers3D.streamer_coverage(ut * 1.05) < 0.25 and GroundStreamers3D.streamer_coverage(ut * 2.0) >= 1.0, "coverage %.2f at 1.05 u*t, %.2f at 1.5, %.2f at 2" % [GroundStreamers3D.streamer_coverage(ut * 1.05), GroundStreamers3D.streamer_coverage(ut * 1.5), GroundStreamers3D.streamer_coverage(ut * 2.0)])
	var st := GroundStreamers3D.new()
	add_child(st)
	_make_env()
	# Low over the sand, looking along it: grains in the air show against the sky above the horizon.
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(0.0, _undisturbed(Vector3.ZERO) + 0.1, 2.5)
	cam.look_at(Vector3(0.0, _undisturbed(Vector3.ZERO) + 0.1, -10.0))
	surface.wind_override = Vector2(4.0, 0.0)
	for i in 30:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var calm := get_viewport().get_texture().get_image()
	_check("no grains under the threshold", st.get_flux() == 0.0, "flux %.4f kg/m/s at 4 m/s" % st.get_flux())
	surface.wind_override = Vector2(12.0, 0.0)
	for i in 180:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var windy := get_viewport().get_texture().get_image()
	if OS.has_environment("STREAMER_DUMP"):
		calm.save_png(OS.get_environment("STREAMER_DUMP") + "_calm.png")
		windy.save_png(OS.get_environment("STREAMER_DUMP") + "_windy.png")
	var q := st.get_flux()
	var m := GroundStreamers3D.airborne_mass(q)
	print("  12 m/s: flux %.1f g/m/s, %.2f g of sand in the air over a square metre, coverage %.2f, %d grains a particle, %.0f%% of the particles flying" % [q * 1000.0, m * 1000.0, st.get_coverage(), int(st.get_grains_per_particle()), st.get_active_ratio() * 100.0])
	_check("blowing sand at 12 m/s (field fluxes 5 to 50 g/m/s, Sherman 1998)", q > 0.005 and q < 0.05, "%.1f g/m/s" % (q * 1000.0))
	# Round the horizon: the grains within a few metres, against the far sand and the sky.
	var diff := 0.0
	var n := 0
	var w := calm.get_width()
	var hh := calm.get_height()
	for y in range(int(hh * 0.4), int(hh * 0.6), 2):
		for x in range(0, w, 2):
			diff += absf(windy.get_pixel(x, y).get_luminance() - calm.get_pixel(x, y).get_luminance())
			n += 1
	diff /= maxf(n, 1)
	_check("the grains are drawn", diff > 0.002, "mean change %.4f round the horizon" % diff)
	st.queue_free()
	cam.queue_free()
	print("ground step %.2f ms (with ripples)" % surface.get_step_time_msec())


# The field's sand above (+) or below (-) the undisturbed ground, m3.
func _field_net() -> float:
	surface.sync_deformation()
	var v := surface.get_moved_volume()
	return v.y - v.x


func _make_grains(follow: Node3D) -> PhysXGroundGrains3D:
	var g := PhysXGroundGrains3D.new()
	add_child(g)
	g.ground_surface = g.get_path_to(surface)
	if follow:
		g.follow_target = g.get_path_to(follow)
	return g


func _run_grains_tests() -> void:
	# A kick: a 15 cm disc of dry sand, 5 cm deep, thrown at (2, 3, 0) m/s. In a vacuum it lands 1.2 m away
	# after 0.6 s (thrown grains fly drag-free; the clump spreads a little).
	await _soil_scene("dry_sand")
	var g := _make_grains(null)
	for i in 5:
		await get_tree().physics_frame
	var net0 := _field_net()
	var dug := g.kick(Vector3(0, 0, 0), 0.15, 0.05, Vector3(2, 3, 0))
	var disc := PI * 0.15 * 0.15 * 0.05
	_check("a kick digs its disc", dug > 0.5 * disc and dug < 1.5 * disc, "%.2f l (disc %.2f l)" % [dug * 1000.0, disc * 1000.0])
	_check("the dug sand leaves the field", absf((net0 - _field_net()) - dug) < 0.02 * dug, "field %.2f l down" % ((net0 - _field_net()) * 1000.0))
	var worst := 0.0
	var peak := 0
	for i in 90:
		await get_tree().physics_frame
		peak = maxi(peak, g.get_airborne_count())
		var gap := absf(g.get_dug_volume() - g.get_airborne_volume() - g.get_deposited_volume())
		worst = maxf(worst, gap)
	_check("the kick flies as grains", peak > 50, "%d grains at most" % peak)
	_check("volume kept in flight: dug = in the air + landed", worst < 0.01 * dug, "worst gap %.4f l" % (worst * 1000.0))
	for i in 240:
		await get_tree().physics_frame
		if g.get_airborne_count() == 0:
			break
	surface.sync_deformation()
	_check("everything lands", g.get_airborne_count() == 0, "%d grains still up" % g.get_airborne_count())
	_check("volume kept end to end", absf(g.get_deposited_volume() - dug) < 0.02 * dug and absf(_field_net() - net0) < 0.03 * dug,
			"landed %.2f l of %.2f l; field net %.3f l" % [g.get_deposited_volume() * 1000.0, dug * 1000.0, (_field_net() - net0) * 1000.0])
	var near := 0.0
	var far := 0.0
	for x in 41:
		var px := -1.0 + x * 0.1
		for z in 11:
			var q := Vector3(px, 0, (z - 5) * 0.1)
			var r := surface.get_rim_at(q)
			if px > 0.6 and px < 2.2:
				far += r
			elif absf(px) < 0.4:
				near += r
	# Where it came down: the centroid of the new rim, against the ballistic 1.22 m.
	var mass := 0.0
	var cx := 0.0
	for x in 41:
		var px := 0.4 + x * 0.05
		for z in 21:
			var r := surface.get_rim_at(Vector3(px, 0, (z - 10) * 0.05))
			mass += r
			cx += r * px
	cx /= maxf(mass, 1e-6)
	_check("it lands where a thrown body would (1.22 m, drag-free)", absf(cx - 1.22) < 0.25, "centre of the landed sand at %.2f m" % cx)
	g.queue_free()

	# A 4x4 floored on dry sand with aired-down tyres: the spinning wheels throw sand behind them.
	await _soil_scene("dry_sand")
	var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	for c in car.get_children():
		if c is PhysXVehicleWheel3D:
			c.tire_pressure = 60.0
	surface.follow_target = surface.get_path_to(car)
	g = _make_grains(car)
	var behind := [0.0, 0]
	var probe := func() -> void:
		var f := car.get_forward()
		for q in g.get_airborne_positions():
			behind[0] += (q - car.global_position).dot(f)
			behind[1] += 1
	get_tree().physics_frame.connect(probe)
	var res: Array = await _drive(car, 2.0, 1.0)
	get_tree().physics_frame.disconnect(probe)
	var along: float = behind[0] / maxi(behind[1], 1)
	print("  floored: %.1f m, slip %.2f, dug %.1f l, in the air %d grains" % [res[0], res[2], g.get_dug_volume() * 1000.0, g.get_airborne_count()])
	_check("spinning wheels throw sand", g.get_dug_volume() > 0.001 and g.get_airborne_count() + int(g.get_deposited_volume() > 0.0) > 0,
			"%.1f l thrown" % (g.get_dug_volume() * 1000.0))
	_check("it is thrown behind the car", along < -0.5, "grains %.1f m along the car's heading on average" % along)
	var thrown_gap := absf(g.get_dug_volume() - g.get_airborne_volume() - g.get_deposited_volume())
	_check("volume kept under the car", thrown_gap < 0.01 * maxf(g.get_dug_volume(), 1e-6), "gap %.4f l" % (thrown_gap * 1000.0))
	print("grains: MPM step %.2f ms (CPU side; GPU time on the PC queue), field on the %s" % [g.get_mpm_step_msec(), "GPU" if surface.is_gpu_active() else "CPU"])


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
	sun.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-18.0, 140.0, 0.0)
	if sun_arg.size() >= 3:
		env.tonemap_exposure = sun_arg[2]
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([1.2, 1.05, 3.4, -1.2, 0.0, 0.7])
	cam.position = Vector3(c[0], c[1], c[2])
	cam.look_at(Vector3(c[3], c[4], c[5]))
	cam.fov = 55.0
	# The walker: a capsule on the ground's collider, its two boots (boxes in ground_deform, 80 kg on
	# 0.035 m2) printing by Bekker sinkage; a ball and a 25 cm tyre roll through at set depths.
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
		# A boot's sole: heel, waist, toe and lugs instead of a block.
		foot.set_meta("ground_sole", true)
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
	(wc.shape as CylinderShape3D).height = 0.25
	wc.rotation_degrees = Vector3(90, 0, 0)
	wheel.add_child(wc)
	var wmi := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.4
	cyl.bottom_radius = 0.4
	cyl.height = 0.25
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
	# Off the frame: only their tracks are shown (their shadows would cross it at a low sun).
	for nm in ["Ball", "Wheel", "Foot0", "Foot1"]:
		(get_node(nm) as Node3D).global_position = Vector3(0, 100, 0)
	walker.global_position = Vector3(0, 100, 0)
	# Let the walls settle as they would in the second after.
	for i in 60:
		await get_tree().physics_frame
	surface.sync_deformation()
	print("ground %s: step %.2f ms on the %s, moved %s m3, near rect %s" % [kind, surface.get_step_time_msec(), "GPU" if surface.is_gpu_active() else "CPU", surface.get_moved_volume(), surface.get_near_rect()])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


func _build_ruts() -> void:
	if not "--ground=" in " ".join(OS.get_cmdline_user_args()):
		kind = "damp_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	if not OS.get_cmdline_user_args().has("--meadow"):
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
	sun.rotation_degrees = Vector3(-16.0, 150.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	var start_z := -9.0
	if mud_arg.size() == 2:
		await _paint_mud(mud_arg[0], mud_arg[1])
		start_z = -2.0
	var car := _make_car(1500.0, 700.0, Vector3(-1.0, _undisturbed(Vector3(-1.0, 0, start_z)) + 0.9, start_z))
	if kind == "dry_sand":
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				c.tire_pressure = 60.0
	surface.follow_target = surface.get_path_to(car)
	# Nothing to see while it drives: draw it small (lavapipe), full size for the final frame.
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	# A gentle S through the frame, then on out of it.
	car.throttle = 0.5
	var t := 0.0
	while t < (12.0 if mud_arg.size() == 2 else 9.0) and car.global_position.z < 14.0:
		car.steer = 0.25 * sin(t * 0.8)
		await get_tree().physics_frame
		t += 1.0 / 60.0
	car.throttle = 0.0
	car.brake = 1.0
	# The camera on the ruts, low, the car gone past; the field follows the camera now.
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(1.6, _undisturbed(Vector3(1.6, 0, -3.5)) + 0.9, -3.5)
	cam.look_at(Vector3(-0.6, _undisturbed(Vector3(-0.6, 0, 1.5)), 1.5))
	cam.fov = 55.0
	var car_end := car.global_position
	car.queue_free()
	surface.follow_target = surface.get_path_to(cam)
	for i in 60:
		await get_tree().physics_frame
	if water_arg > 0.0:
		# Rain on the ruts (sped up 100 times): what the clay cannot take stands, then an hour after it the flat
		# has soaked its sheet up and the ruts, pressed, still hold theirs.
		surface.standing_water = true
		surface.desiccation_cracks = dry_arg
		surface.runoff_time_scale = 100.0
		var wm := WeatherMap3D.new()
		wm.cloud_cover = 1.0
		wm.rain_rate = 20.0
		wm.humidity = 0.7
		add_child(wm)
		wm.snap_to_settings()
		for i in 5:
			await get_tree().physics_frame
		surface.settle(water_arg / 20.0 * 36.0, 0.05)
		wm.rain_rate = 0.0
		wm.cloud_cover = 0.0
		wm.snap_to_settings()
		for i in 5:
			await get_tree().physics_frame
		surface.settle(36.0, 0.05)
		print("water: %.1f mm in the left rut" % (surface.get_standing_water_at(Vector3(-1.6, 0, 0.0)) * 1000.0))
		if dry_arg:
			surface.desiccation_cracks = true
			surface.runoff_time_scale = 1e5
			# --dry=0: until the deepest pool's crust starts (its rim cracked, its centre still damp).
			var deep0 := Vector3()
			var plate0 := 0.0
			for xi in range(-32, 33):
				for zi in range(-32, 33):
					var q := Vector3(0.25 * xi, 0, -1.0 + 0.25 * zi)
					var c := surface.get_crack_at(q)
					if c.y > plate0:
						plate0 = c.y
						deep0 = q
			var frames := 0
			while (dry_frames > 0 and frames < dry_frames) or (dry_frames == 0 and frames < 1200 and surface.get_crack_at(deep0).w < -0.5):
				await get_tree().physics_frame
				frames += 1
			print("dried: %d frames" % frames)
			surface.runoff_time_scale = 1.0
			print("dried: cracks %s in the left rut" % surface.get_crack_at(Vector3(-1.6, 0, 0.0)))
			# The deepest dried pool (the largest plates), for framing --cam.
			var deep := Vector3()
			var deep_plate := 0.0
			for xi in range(-32, 33):
				for zi in range(-32, 33):
					var q := Vector3(0.25 * xi, 0, -1.0 + 0.25 * zi)
					var c := surface.get_crack_at(q)
					if c.y > deep_plate and (c.x > 0.0 or c.w > 0.0 or surface.get_standing_water_at(q) > 0.0):
						deep_plate = c.y
						deep = q
			print("dried: deepest pool at %s, plate %.1f cm, %s" % [deep, deep_plate * 100.0, surface.get_crack_at(deep)])
			# A map, 0.5 m a character, x across from -8, z down from -9: '.' no pool, '~' water standing; pool beds
			# by plate size (2-6, 6-10, 10-14, over 14 cm): '1'-'4' still damp, 'A'-'D' drying, 'a'-'d' cracked.
			for zi in range(0, 33):
				var row := ""
				for xi in range(0, 33):
					var q := Vector3(-8.0 + 0.5 * xi, 0, -9.0 + 0.5 * zi)
					var c := surface.get_crack_at(q)
					var bin := clampi(int((c.y * 100.0 - 2.0) / 4.0), 0, 3)
					if surface.get_standing_water_at(q) > 0.001:
						row += "~"
					elif c.y < 0.05:
						row += "."
					elif c.x > 0.5:
						row += "abcd"[bin]
					elif c.w < -0.5:
						row += "1234"[bin]
					else:
						row += "ABCD"[bin]
				print("map %5.1f %s" % [-9.0 + 0.5 * zi, row])
			# Where the dried pools are, round the view (for framing --cam).
			var sum := Vector3()
			var cnt := 0
			for xi in range(-24, 25):
				for zi in range(-24, 25):
					var q := Vector3(0.25 * xi, 0, -1.0 + 0.25 * zi)
					if surface.get_crack_at(q).x > 0.9:
						sum += q
						cnt += 1
			print("dried: %d cracked cells, centroid %s" % [cnt, sum / maxf(cnt, 1)])
		if cam_arg.size() == 6:
			cam.position = Vector3(cam_arg[0], _undisturbed(Vector3(cam_arg[0], 0, cam_arg[2])) + cam_arg[1], cam_arg[2])
			cam.look_at(Vector3(cam_arg[3], _undisturbed(Vector3(cam_arg[3], 0, cam_arg[5])) + cam_arg[4], cam_arg[5]))
	surface.sync_deformation()
	get_window().size = full
	print("ruts in %s: car at %s, moved %s m3, field on the %s" % [kind, car_end, surface.get_moved_volume(), "GPU" if surface.is_gpu_active() else "CPU"])
	await _save_view(env)
	get_tree().quit()


func _make_env() -> Environment:
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
	sun.rotation_degrees = Vector3(-24.0, 150.0, 0.0)
	sun.directional_shadow_max_distance = 40.0
	add_child(sun)
	return env


# Sand stage 2: a 4x4 floored from rest on dry sand. The spinning wheels dig in and throw their tread's sand
# back as grains (PhysXGroundGrains3D) that land behind it. The frame is taken mid-spray, looking across the
# sand behind the rear wheels.
func _build_roost() -> void:
	kind = "dry_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	var car := _make_car(1500.0, 700.0, Vector3(0.0, _undisturbed(Vector3.ZERO) + 0.9, 0.0))
	surface.follow_target = surface.get_path_to(car)
	var grains := _make_grains(car)
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([3.0, 0.7, -1.6, 0.4, 0.45, -3.6])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	car.throttle = 1.0
	for i in 100:
		await get_tree().physics_frame
	get_window().size = full
	print("roost: %.1f l thrown, %d grains in the air, field on the %s" % [grains.get_dug_volume() * 1000.0, grains.get_airborne_count(), "GPU" if surface.is_gpu_active() else "CPU"])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


# Sand stage 4: a line of boot prints across sand; water flowed over the near half for a minute (the swash)
# and drew back. Those prints are rounded off and shallow, the far ones keep their walls.
func _build_swash() -> void:
	kind = "damp_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(-18.0, 200.0, 0.0)
	var water := Node3D.new()
	water.set_script(load("res://water_fake.gd"))
	add_child(water)
	surface.water_sources = [surface.get_path_to(water)]
	for i in 30:
		await get_tree().physics_frame
	for k in 8:
		var p := Vector3(-1.6 + 0.45 * k, 0.0, 0.12 if k % 2 == 0 else -0.12)
		p.y = _undisturbed(p)
		surface.press_box(p, Vector2(0.11, 0.29), PI * 0.5, p.y, 0.035)
	water.rect = Rect2(-3.0, -3.0, 3.0, 6.0)
	water.depth = 0.03
	water.flow = Vector3(0.0, 0.0, 0.6)
	for i in 90:
		await get_tree().physics_frame
	surface.settle(60.0)
	water.depth = -1.0
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.0, 0.75, 1.25, 0.0, 0.0, -0.1])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	for i in 10:
		await get_tree().physics_frame
	print("swash: prints %.1f cm deep washed, %.1f cm kept" % [surface.get_dent_at(Vector3(-1.6, 0, 0.12)) * 100.0, surface.get_dent_at(Vector3(1.55, 0, -0.12)) * 100.0])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


# Sand glints: loose dry sand seen from 40 cm, a low sun behind the camera and to one side, so single grains'
# colours and the faces that mirror the sun read.
func _build_grains() -> void:
	kind = "dry_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(-22.0, 30.0, 0.0)
	for i in 40:
		await get_tree().physics_frame
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.0, 0.4, 0.35, 0.0, 0.0, -0.15])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	for i in 10:
		await get_tree().physics_frame
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


# Runoff channels: the --test=runoff beach after 84 minutes of seepage (scaled, as the test), seen from 1.5 m above
# the upper face looking down it to the waterline and the sand laid at the rills' foot, a low sun across it so the
# rills' walls shade and the seeping water glistens. RUNOFF_DUMP=path.png
# also saves the cut from above.
func _build_rills() -> void:
	var on := not OS.get_cmdline_user_args().has("--runoff=0")
	await _runoff_beach(120.0)
	surface.runoff_channels = on
	var mat := _ground_material("dry_sand")
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-14.0, 75.0, 0.0)
	for i in 2400:
		await get_tree().physics_frame
	surface.sync_deformation()
	for i in 3:
		await get_tree().physics_frame
	_runoff_dump()
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.6, 1.5, 6.8, 0.0, 0.0, 2.0])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	for i in 10:
		await get_tree().physics_frame
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


# Sand stage 4 at a real shore: PhysXWaterSurface3D's shore waves run up a 1 in 12 beach as a swash film. A row of
# boot prints walks out of the sea, over the line the swash reaches, onto the dry sand above it. The ground reads
# the sea's depth and the weather's contact wetness, so the sand the swash covers is soaked and darkens, drying
# with distance above the line, and the prints it covers round off while those above keep their walls. Low 3/4
# view from about 2.5 m. --cam=x,dy,z,tx,dy,tz as in the other views.
func _build_shore() -> void:
	kind = "dry_sand"
	await _setup("beach", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			# From over the sea, a little to the side: the wet sand and the film catch its sheen.
			c.rotation_degrees = Vector3(-20.0, 200.0, 0.0)
			# --sun=pitch,yaw (degrees) for other light: e.g. -55,20 puts a high sun behind a camera facing the sea.
			for a in OS.get_cmdline_user_args():
				if a.begins_with("--sun="):
					var sv := a.substr(6).split(",")
					c.rotation_degrees = Vector3(float(sv[0]), float(sv[1]), 0.0)
	if not ClassDB.class_exists("PhysXWaterSurface3D") or not ClassDB.class_exists("WeatherMap3D"):
		push_error("This build has no PhysX water or weather module.")
		get_tree().quit(1)
		return
	var weather: Node3D = ClassDB.instantiate("WeatherMap3D")
	weather.set("rain_rate", 0.0)
	weather.set("drive_environment_fog", false)
	add_child(weather)
	var sea: Node3D = ClassDB.instantiate("PhysXWaterSurface3D")
	# The beach well inside the sea's domain, so its wet-sand edge never shows.
	sea.position = Vector3(0.0, 0.0, -10.0)
	sea.set("domain_size", Vector2(48, 48))
	sea.set("grid_resolution", 192)
	sea.set("seabed_from_floor", true)
	sea.set("ocean_grid_resolution", 128)
	sea.set("wind_speed", 5.0)
	sea.set("wind_direction", Vector2(0.15, 1.0))
	sea.set("render_extent", 400.0)
	sea.set("shore_waves", true)
	sea.set("shore_wave_height", 0.25)
	sea.set("shore_wave_direction", Vector2(0.15, 1.0))
	sea.set("swash_run_up", 0.12)
	sea.set("contact_wetness", true)
	# --sea=name:value sets one of the sea's properties (repeatable), e.g. --sea=shore_wave_curl:0.
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sea="):
			var skv := a.substr(6).split(":", true, 1)
			sea.set(skv[0], str_to_var(skv[1]))
	add_child(sea)
	var wm: ShaderMaterial = sea.call("get_water_material")
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--water=") and wm:
			var wkv := a.substr(8).split(":", true, 1)
			wm.set_shader_parameter(wkv[0], str_to_var(wkv[1]))
	surface.water_sources = [surface.get_path_to(sea)]
	surface.water_coupling = not OS.get_cmdline_user_args().has("--coupling=0") # for an A/B of the look
	# The prints first, then the sea comes up over the lower ones.
	for i in 30:
		await get_tree().physics_frame
	for k in (0 if OS.get_cmdline_user_args().has("--prints=0") else 16):
		var p := Vector3(0.12 if k % 2 == 0 else -0.12, 0.0, -0.6 + 0.3 * k)
		p.y = _undisturbed(p)
		surface.press_box(p, Vector2(0.11, 0.29), 0.0, p.y, 0.045)
	# A few swashes only: prints at the top of the swash are softened, not yet gone (--swash-frames=N, 300).
	var swash_frames := 300
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--swash-frames="):
			swash_frames = int(a.substr(15))
	# --ebb=from,to: the run-up falls from one height to the other over the swash frames, as on a falling tide,
	# so each wave's mark is left below the last ones' and the beach keeps a ladder of them.
	var ebb := PackedFloat32Array()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--ebb="):
			ebb = PackedFloat32Array(Array(a.substr(6).split(",")).map(func(v): return float(v)))
	for i in swash_frames:
		if ebb.size() == 2:
			sea.set("swash_run_up", lerpf(ebb[0], ebb[1], float(i) / swash_frames))
		await get_tree().physics_frame
		if OS.get_environment("SHORE_TRACE") != "" and i % 60 == 0:
			var q := Vector3(0.0, _undisturbed(Vector3(0, 0, 5)), 5.0)
			var pw := Vector3(-0.12, _undisturbed(Vector3(-0.12, 0, 0.66)), 0.66)
			print("t%d sat %.2f depth %.3f swash %s | z0.66 print %.1f cm sat %.2f speed %.2f swash %s" % [i, surface.get_saturation_at(q), sea.call("sample_depth", q), str(sea.call("sample_swash", q)), surface.get_dent_at(pw) * 100.0, surface.get_saturation_at(pw), surface.get_water_speed_at(pw), str(sea.call("sample_swash", pw))])
	if OS.get_environment("SHORE_TRACE") != "":
		for k in 16:
			var pk := Vector3(0.12 if k % 2 == 0 else -0.12, 0.0, -0.6 + 0.3 * k)
			var m: Dictionary = surface.get_material_at(pk)
			print("print %d z%.1f dent %.1f cm speed %.2f sat %.2f repose %.0f rate %.2f" % [k, pk.z, surface.get_dent_at(pk) * 100.0, surface.get_water_speed_at(pk), surface.get_saturation_at(pk), m["repose"], m["rate"]])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([1.9, 0.9, 3.4, -0.1, 0.0, 1.2])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	var prof := ""
	for k in 17:
		var pz := -1.0 + 0.5 * k
		var pp := Vector3(0.0, _undisturbed(Vector3(0.0, 0.0, pz)), pz)
		prof += " z%.1f:%.2f(c%.2f s%s d%.2f)" % [pz, surface.get_saturation_at(pp), weather.call("get_contact_wetness_at", pp), str(sea.call("sample_swash", pp)), sea.call("sample_depth", pp)]
	print("shore: saturation by z" + prof)
	print("shore: %d swash marks laid, %.2f ms a step" % [surface.get_swash_mark_count(), surface.get_swash_step_usec() / 1000.0])
	if OS.get_environment("SWASH_DUMP") != "":
		# The ground's change from undisturbed over x -4..4, z -1..5 at 1 cm (800 x 600 raw floats) for a look from above.
		var buf := PackedFloat32Array()
		for zi in 600:
			for xi in 800:
				var pq := Vector3(-4.0 + xi * 0.01, 0.0, -1.0 + zi * 0.01)
				buf.append(surface.get_surface_height_at(pq) - _undisturbed(pq))
		var fa := FileAccess.open(OS.get_environment("SWASH_DUMP"), FileAccess.WRITE)
		fa.store_buffer(buf.to_byte_array())
	print("shore: saturation %.2f in the wash, %.2f above it; prints %.1f cm deep in the wash, %.1f cm above" % [
		surface.get_saturation_at(Vector3(0.12, 0, -0.6)), surface.get_saturation_at(Vector3(-0.12, 0, 3.6)),
		surface.get_dent_at(Vector3(0.12, 0, -0.6)) * 100.0, surface.get_dent_at(Vector3(-0.12, 0, 3.6)) * 100.0])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


# Sand stage 3: wind ripples on dry sand under a low sun, sand blowing over them. --wind=m/s (default 9).
func _build_ripples() -> void:
	kind = "dry_sand"
	await _setup("meadow", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	var sun: DirectionalLight3D = null
	for c in get_children():
		if c is DirectionalLight3D:
			sun = c
	sun.rotation_degrees = Vector3(-9.0, 120.0, 0.0)
	surface.wind_override = Vector2(wind_arg, 0.0)
	surface.ripple_extent = 8.0
	surface.wind_ripples = true
	var st := GroundStreamers3D.new()
	add_child(st)
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.0, 0.55, 2.2, -0.3, 0.0, -1.0])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	var t0 := Time.get_ticks_msec()
	while not surface.is_ripples_ready() and Time.get_ticks_msec() - t0 < 60000:
		await get_tree().physics_frame
	for i in 90:
		await get_tree().physics_frame
	print("ripples: %d steps, streamers %.1f g/m/s, coverage %.2f" % [surface.get_ripple_steps(), st.get_flux() * 1000.0, st.get_coverage()])
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
	var bp := Vector3(-6.0 + 1.2 * walk_t, 0, -0.6 + 0.12 * sin(walk_t * 0.8))
	b.global_position = Vector3(bp.x, _undisturbed(bp) - 0.06 + 0.25, bp.z)
	b.rotation.z = -bp.x / 0.25
	var w: Node3D = get_node("Wheel")
	var wp := Vector3(-8.0 + 1.7 * walk_t, 0, -2.4 + 0.2 * sin(walk_t * 0.6 + 1.0))
	w.global_position = Vector3(wp.x, _undisturbed(wp) - 0.03 + 0.4, wp.z)
	w.rotation.z = -wp.x / 0.4


func _save_frame() -> void:
	for i in 12:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)


func _make_dust(follow: Node3D) -> GroundDust3D:
	var d := GroundDust3D.new()
	add_child(d)
	d.ground_surface = d.get_path_to(surface)
	if follow:
		d.follow_target = d.get_path_to(follow)
	return d


# The dust's centre of mass (x, height over the ground, z) and its mass, from a lattice over its grid.
func _dust_centroid(d: GroundDust3D, around: Vector3) -> Vector4:
	var e := d.extent
	var h := d.cell_size
	var m := 0.0
	var acc := Vector3()
	for iz in int(e.z / h):
		for iy in int(e.y / h):
			for ix in int(e.x / h):
				var p := around + Vector3((ix + 0.5) * h - e.x * 0.5, (iy + 0.5) * h - 1.0, (iz + 0.5) * h - e.z * 0.5)
				var c := d.get_concentration_at(p)
				if c > 0.0:
					m += c
					acc += p * c
	if m <= 0.0:
		return Vector4()
	acc /= m
	return Vector4(acc.x, acc.y, acc.z, m * h * h * h)


# Drifting on sand (docs/ideas/sand.md): a 1500 kg 4x4 on 60 kPa tyres up to speed on dry sand, then slid through
# a full-lock turn; and the dust on its own.
func _run_drift_tests() -> void:
	# Dust physics: Stokes settling and a puff carried by the wind, then settling to the ground.
	await _soil_scene("dry_sand")
	var d := _make_dust(null)
	for i in 3:
		await get_tree().physics_frame
	var ws := d.get_settling_speed()
	_check("Stokes settling of 15 um silt", absf(ws - 0.0180) < 0.001, "%.4f m/s" % ws)
	d.diffusivity = 0.0
	d.add_dust(Vector3(0, 2.0, 0), Vector3(), 1.0)
	var c0 := _dust_centroid(d, Vector3.ZERO)
	for i in 300:
		await get_tree().physics_frame
	var c1 := _dust_centroid(d, Vector3.ZERO)
	var fall := (c0.y - c1.y) / 5.0
	_check("a still puff sinks at its settling speed", absf(fall - ws) < 0.3 * ws, "%.4f m/s over 5 s (%.4f)" % [fall, ws])
	surface.wind_override = Vector2(6.0, 0.0)
	d.clear()
	d.diffusivity = 0.3
	d.add_dust(Vector3(0, 1.5, 0), Vector3(), 1.0)
	for i in 120:
		await get_tree().physics_frame
	var c2 := _dust_centroid(d, Vector3.ZERO)
	_check("the wind carries a puff downwind", c2.x > 3.0 and absf(c2.z) < 0.3, "%.2f m in 2 s at 6 m/s (10 m)" % c2.x)
	surface.wind_override = Vector2()
	d.clear()
	d.grain_diameter = 60.0
	d.add_dust(Vector3(0, 1.0, 0), Vector3(), 1.0)
	for i in 240:
		await get_tree().physics_frame
	_check("coarse dust settles out to the ground", d.get_deposited_mass() > 0.35 * d.get_emitted_mass(), "%.2f of %.2f kg on the ground after 4 s at 60 um (%.2f m/s)" % [d.get_deposited_mass(), d.get_emitted_mass(), d.get_settling_speed()])

	# The slide.
	var results := {}
	for k in ["dry_sand", "damp_sand"]:
		await _soil_scene(k)
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				c.tire_pressure = 60.0
		surface.follow_target = surface.get_path_to(car)
		var dust := _make_dust(car)
		for i in 30:
			await get_tree().physics_frame
		car.throttle = 1.0
		for i in 240:
			await get_tree().physics_frame
			surface.take_sprays()
		car.steer = 1.0
		var side_throw := 0.0
		var side_force := 0.0
		var plume_top := 0.0
		var prev := car.global_position
		for i in 90:
			await get_tree().physics_frame
			var right := car.global_basis.x
			var cv := (car.global_position - prev) * 60.0
			prev = car.global_position
			for s in surface.take_sprays():
				var v: Vector3 = s["velocity"] - cv
				side_throw = maxf(side_throw, absf(v.dot(right)))
			for w in 4:
				side_force = maxf(side_force, car.get_wheel_soil_side_force(w))
		var behind := car.global_position - car.global_basis.z * 3.0
		var g := surface.get_ground_height_at(behind)
		for y in 10:
			if dust.get_concentration_at(Vector3(behind.x, g + 0.25 + y * 0.25, behind.z)) > 1e-5:
				plume_top = 0.25 + y * 0.25
		results[k] = [side_throw, side_force, dust.get_emitted_mass(), plume_top]
		print("  %s: side throw %.1f m/s, sidewall %.0f N, dust %.2f kg, plume to %.2f m" % [k, side_throw, side_force, dust.get_emitted_mass(), plume_top])
		car.queue_free()
		dust.queue_free()
	_check("a sliding wheel throws sand sideways", results["dry_sand"][0] > 1.0, "%.1f m/s across the car" % results["dry_sand"][0])
	_check("the sand pushes back on a sliding sidewall (Ishigami et al. 2007)", results["dry_sand"][1] > 100.0, "%.0f N" % results["dry_sand"][1])
	_check("dry sand raises a dust plume about as tall as the car", results["dry_sand"][2] > 0.1 and results["dry_sand"][3] >= 1.0, "%.2f kg, to %.2f m" % [results["dry_sand"][2], results["dry_sand"][3]])
	_check("damp sand raises no dust", results["damp_sand"][2] == 0.0, "%.3f kg" % results["damp_sand"][2])

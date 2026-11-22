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
#   --test=shelter      dune grass (wind_shelters): Raupach drag partition, flux dies among the plants, sand lands; a wall's lee (WorldWeather.obstacle_lee_at) drops the sand behind it
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
#                         and treads (mud round 10): mud tyres and chains clog less and get further than road tyres
#   --test=balling      sticky clay balls up on spinning tyres, sheds when floored, the lopsided rest shakes (round 24.1)
#   --test=peat         peat bogs (round 24.2): the root mat holds a car, a heavy truck tears it and drops into the peat
#   --test=treads       each tread's lugs or chain links in its rut floor (mud round 10); --view=ruts --tread=N drives
#                         the car on PhysXVehicleWheel3D.Tread N
#   --test=gullies      rain on a 6 degree slope cuts a gully down a wheel rut in soil and lays a fan below it; the
#                         untouched soil beside it holds, where sand would wash (mud round 11)
#   --test=slides       soaked mud slides down a 15 degree bank into a rut at its foot, stiff mud holds (mud round 10)
#   --view=bodymud      mud on a car's body (mud stage 3b): a dressed 4x4 crawls through stiff clay, then is floored
#                         so its tyres throw the clay up the arches; a close frame on the rear wheel and side.
#                         --cam=x,y,z,tx,ty,tz in the car's frame (default from its right rear); --spray: the
#                         frame mid-throw, the clods in the air
#   --test=splash       a 4x4 through rut water past Froude 1 (mud stage 4c): sheets thrown, landed back, volume kept
#   --test=ruts         kept ruts (mud round 3): a print back after the field drives 600 m off and in a new node
#   --meadow            --view=ruts: the patch drawn with a TerrainMaterial3D under a full sward (its grass globals set by hand)
#   --test=burnscar    rain on a burn scar: water-repellent burnt soil (WorldFire) sheds most of the rain as runoff
#   --test=runoff       runoff channels: a falling tide's soaked beach seeps, the water cuts rills down the face
#                         and lays the sand at the waterline; volume kept; cost of the step
#   --test=rain         rain pits on dry sand: a shower pits it, still air keeps them, saltation wipes them;
#                         saltation grows adhesion warts and rain flattens them
#   --test=avalanche    footsteps on a dune's slip face set off grain avalanches down it; not on the stoss slope
#   --test=infill       wind fills a print from its upwind side; nothing under the threshold
#   --view=fringe       a still pool's shore in a desert: the wet margin and the salt crust
#   --test=steps        a character with no feet of its own prints boot soles step by step, not its capsule
#   --test=fringe       a wet margin of sand up to 15 cm over still water's level (capillary rise), salt in a desert
#   --view=avalanche    a walker's prints across a dune's slip face and the tongues of sand running down from them
#   --test=drift        a car slid through a turn on dry sand: sand thrown sideways, the sidewall's push, a berm
#                         on the outside; dust: none from damp sand, a plume from dry, Stokes settling, wind drift
#   --view=rooting      boar rootings (WorldStand DISTURBANCE_ROOTING) fresh, a year and 1.8 years old, left to right:
#                         the turned soil's clods (GroundClods3D.rooting); --ground=KIND as --view=trail
#   --view=trail        a walker, a rolling ball and a wheel cross the ground; --ground=KIND picks it:
#                       dry_sand, damp_sand, soil, mud, litter (default soil)
#   --mud=depth,moisture paint a mud layer for --view=ruts (stage 1; the car starts in the frame and drives 12 s)
#   --adhesion  --view=fringe: wind over the damp margin until adhesion warts grow (--wind=m/s)
#   --no-nmap  the bench material without its stand-in noise normal map; --taa: TAA on; --sun-alt=deg (ripples view, 9)
#   --rain-pits=mm  --view=ripples: then a still shower of that much rain (120 mm/h), pitting the sand
#   --water=mm   --view=ruts: then rain that much (20 mm/h, sped up) and let it stand an hour (mud stage 2)
#   --dry        --view=ruts with --water: then days of sun, so the soaked ruts dry and crack (stage 2b);
#                  --dry=frames for part of it (600 is all), --dry=0 until the deepest pool starts to crust
#   --frost=hours[,break] --view=ruts with --water: then a frost of that many hours at -5 C freezes the water (mud
#                  round 7); ",break": a walker's boots across the left rut break the ice
#   --test=frost        (and the ruts' guidance of a tyre: pushed back towards a rut's floor, walls as deep as the rut)
#                       frozen ground: a night's frost (Stefan) holds feet off, keeps the ruts, stands water; the thaw
#                         leaves a soaked layer that prints deeper than before
#   --view=ice          a frozen puddle with a wheel's track broken through it (mud round 7)
#   --test=ice          puddle ice: freezes at Stefan's rate, bears a foot, breaks under a wheel, refreezes, thaws;
#                         and mud on plants: thrown clay and muddy splashes spatter the sward, prints do not, rain washes it
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

# A wall 1 m high across x = 5, for the obstacle lee (WorldWeather.obstacle_lee_at, as WeatherMap3D's shelter gives it).
class WallLeeStub extends WorldWeather:
	func _obstacle_lee_at(p: Vector3) -> float:
		var x := p.x - 5.0
		if x < 0.0:
			return 1.0
		return 0.15 + 0.85 * (1.0 - exp(-maxf(x - 2.0, 0.0) / 6.0))


# Pebbles over 60% of the ground for x < -6, none elsewhere (WorldGround.coarse_cover_at, as RockField3D's cover).
class PebbleLagStub extends WorldGround:
	func _coarse_cover_at(p: Vector3) -> Vector3:
		return Vector3(0.6, 0.0, 0.0) if p.x < -6.0 else Vector3.ZERO


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
var burnscar_test := false
var drift_test := false
var avalanche_test := false
var infill_test := false
var fringe_test := false
var rain_test := false
var rain_pits_mm := 0.0
var no_nmap := false
var adhesion := false
var taa := false
var sun_alt := 9.0
var steps_test := false
var near_test := false
var mud_test := false
var clog_test := false
var balling_test := false
var treads_test := false
var slides_test := false
var gullies_test := false
var winch_test := false
var flakes_test := false
var pressure_test := false
var subsoil_test := false
var mats_test := false
var debris_test := false
var fans_test := false
var fords_test := false
var camber_test := false
var mudwave_test := false
var wear_test := false
var buried_test := false
var silt_test := false
var carry_test := false
var trackdust_test := false
var stream_test := false
var fordcar_test := false
var parked_test := false
var relief_test := false
var peat_test := false
var trails_test := false
var setup_mud := Vector2.ZERO # mud depth and moisture _setup lays before the node enters the tree
var offpatch_test := false
var tread_arg := -1 # --view=ruts: the car's PhysXVehicleWheel3D.Tread (mud round 10)
var puddle_test := false
var crack_test := false
var legs_test := false
var muddy_test := false
var tracked_test := false
var ruts_test := false
var pudhelp_test := false
var ice_test := false
var frost_test := false
var frost_arg := 0.0 # --frost=hours: the ruts view's water frozen over that long at -5 C
var frost_break := false # --frost=hours,break: and boots across the left rut
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
		elif a == "--test=burnscar": burnscar_test = true
		elif a == "--test=splash": splash_test = true
		elif a == "--test=drift": drift_test = true
		elif a == "--test=avalanche": avalanche_test = true
		elif a == "--test=infill": infill_test = true
		elif a == "--test=fringe": fringe_test = true
		elif a == "--test=rain": rain_test = true
		elif a.begins_with("--rain-pits="): rain_pits_mm = float(a.substr(12))
		elif a == "--no-nmap": no_nmap = true
		elif a == "--adhesion": adhesion = true
		elif a == "--taa": taa = true
		elif a.begins_with("--sun-alt="): sun_alt = float(a.substr(10))
		elif a == "--test=steps": steps_test = true
		elif a == "--test=near": near_test = true
		elif a == "--test=mud": mud_test = true
		elif a == "--test=clog": clog_test = true
		elif a == "--test=balling": balling_test = true
		elif a == "--test=treads": treads_test = true
		elif a == "--test=slides": slides_test = true
		elif a == "--test=gullies": gullies_test = true
		elif a == "--test=winch": winch_test = true
		elif a == "--test=flakes": flakes_test = true
		elif a == "--test=pressure": pressure_test = true
		elif a == "--test=subsoil": subsoil_test = true
		elif a == "--test=mats": mats_test = true
		elif a == "--test=debris": debris_test = true
		elif a == "--test=fans": fans_test = true
		elif a == "--test=fords": fords_test = true
		elif a == "--test=camber": camber_test = true
		elif a == "--test=mudwave": mudwave_test = true
		elif a == "--test=wear": wear_test = true
		elif a == "--test=buried": buried_test = true
		elif a == "--test=silt": silt_test = true
		elif a == "--test=carry": carry_test = true
		elif a == "--test=trackdust": trackdust_test = true
		elif a == "--test=stream": stream_test = true
		elif a == "--test=fordcar": fordcar_test = true
		elif a == "--test=parked": parked_test = true
		elif a == "--test=relief": relief_test = true
		elif a == "--test=peat": peat_test = true
		elif a == "--test=trails": trails_test = true
		elif a == "--test=offpatch": offpatch_test = true
		elif a == "--test=puddles": puddle_test = true
		elif a == "--test=cracks": crack_test = true
		elif a == "--test=legs": legs_test = true
		elif a == "--test=muddy": muddy_test = true
		elif a == "--test=tracked": tracked_test = true
		elif a == "--test=ruts": ruts_test = true
		elif a == "--test=puddle-helper": pudhelp_test = true
		elif a == "--test=ice": ice_test = true
		elif a == "--test=frost": frost_test = true
		elif a.begins_with("--frost="):
			var fv := a.substr(8).split(",")
			frost_arg = float(fv[0])
			frost_break = fv.size() > 1 and fv[1] == "break"
		elif a == "--near=0": near_on = false
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--tread="): tread_arg = int(a.substr(8))
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
	if frost_test:
		await _run_frost_tests()
		await _run_rut_guidance_tests()
		await _run_needle_tests()
		await _run_needle_radiation_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if ice_test:
		await _run_ice_tests()
		await _run_plant_mud_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
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
	if clog_test or treads_test or slides_test or gullies_test or winch_test or flakes_test or pressure_test or subsoil_test or mats_test or debris_test or fans_test or fords_test or camber_test or mudwave_test or wear_test or buried_test or silt_test or offpatch_test or carry_test or trackdust_test or stream_test or fordcar_test or trails_test or parked_test or relief_test or balling_test or peat_test:
		if clog_test:
			await _run_clog_tests()
		if balling_test or clog_test:
			await _run_balling_tests()
		if treads_test:
			await _run_tread_rut_tests()
		if slides_test:
			await _run_slide_tests()
		if gullies_test:
			await _run_gully_tests()
		if winch_test:
			await _run_winch_tests()
		if flakes_test:
			await _run_flake_tests()
		if pressure_test:
			await _run_pressure_tests()
		if subsoil_test:
			await _run_subsoil_tests()
		if mats_test:
			await _run_mat_tests()
		if debris_test:
			await _run_debris_tests()
		if fans_test:
			await _run_fan_tests()
		if fords_test:
			await _run_ford_tests()
		if camber_test:
			await _run_camber_tests()
		if mudwave_test:
			await _run_mudwave_tests()
		if wear_test:
			await _run_wear_tests()
		if buried_test:
			await _run_buried_tests()
		if silt_test:
			await _run_silt_tests()
		if carry_test:
			await _run_carry_tests()
		if trackdust_test:
			await _run_trackdust_tests()
		if stream_test:
			await _run_stream_stone_tests()
		if parked_test:
			await _run_parked_tests()
		if relief_test:
			await _run_relief_tests()
		if peat_test:
			await _run_peat_tests()
		if fordcar_test:
			await _run_ford_car_tests()
		if trails_test:
			await _run_trail_wear_tests()
		if offpatch_test:
			await _run_offpatch_tests()
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
		await _run_seal_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if shelter_test:
		await _run_shelter_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if avalanche_test:
		await _run_avalanche_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if infill_test:
		await _run_infill_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if fringe_test:
		await _run_fringe_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if steps_test:
		await _run_steps_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if view == "avalanche":
		await _build_avalanche()
		return
	if view == "fringe":
		await _build_fringe()
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
	if rain_test:
		await _run_rain_tests()
		print("RESULT: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if burnscar_test:
		await _run_burnscar_tests()
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
	if view == "gully":
		await _build_gully()
		return
	if view == "winch":
		await _build_winch()
		return
	if view == "mats":
		await _build_mats()
		return
	if view == "camber":
		await _build_camber()
		return
	if view == "silt":
		await _build_silt()
		return
	if view == "stones":
		await _build_stones()
		return
	if view == "debris":
		await _build_debris()
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
	if view == "ice":
		await _build_ice()
		return
	if view == "needle":
		await _build_needle()
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
	if view == "rooting":
		await _build_rooting()
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
		surface.frozen_ground = false
		surface.world_puddles = false
		surface.subsoil = false
		surface.worn_tracks = false
		surface.buried_obstacles = false
	# --prop=name:value sets any of the surface's properties (repeatable), for an A/B.
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--prop="):
			var pkv := a.substr(7).split(":", true, 1)
			surface.set(pkv[0], str_to_var(pkv[1]))
	if setup_mud.x > 0.0: # mud there from the start (a reload test)
		var mimg := Image.create(4, 4, false, Image.FORMAT_RF)
		mimg.fill(Color(1, 0, 0))
		surface.mud_max_depth = setup_mud.x
		surface.mud_map_rect = Rect2(-200, -200, 400, 400)
		surface.mud_map = mimg
		surface.moisture = setup_mud.y
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
	var damp_before := _max_slope(o, 0.3)
	surface.settle(3.0)
	var damp_wall := _max_slope(o, 0.3)
	# Against its own walls before settling, or its repose where they start steeper: the CPU field (headless) presses
	# on a coarser texel, so its walls start near 52 degrees; the GPU's start near 70 and settle to the damp repose
	# (GroundSurface3D::material_cell: 33 to 65 degrees with the cohesion, about 62 at moisture 0.4).
	var damp_repose := lerpf(33.0, 65.0, smoothstep(0.0, 0.5, 0.4))
	_check("damp sand keeps steep walls", damp_wall > 45.0 and damp_wall > minf(damp_before, damp_repose) - 3.0, "steepest wall %.0f before, %.0f degrees after 3 s (repose %.0f)" % [damp_before, damp_wall, damp_repose])
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
	# Regrowth follows the seasons (mud round 7): with a WeatherMap3D, the same weeks away grow it back by the
	# climate's growing degree days: a winter away (back in mid February after 90 days) leaves it bare, a summer away
	# (back at the end of June after 35 days) greens it about as fast as the reference rate (the reference is a late
	# spring day; June is warmer but drier), a Mediterranean August not at all.
	var wm := WeatherMap3D.new()
	get_tree().root.add_child(wm)
	var seasonal := []
	for leg in [[45, 90.0, false], [180, 35.0, false], [240, 35.0, true]]:
		wm.season_day = leg[0]
		wm.climate = load("res://dry_climate.gd").new() if leg[2] else null
		t_left = Time.get_unix_time_from_system()
		ruts_scale = leg[1] * 86400.0 / 4.0
		await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
		for i in 6:
			await get_tree().physics_frame
		surface.sync_deformation()
		seasonal.append(surface.get_regrown_at(a))
		var ref := 1.0 - exp(-(Time.get_unix_time_from_system() - t_left) * ruts_scale / surface.ruts_regrowth_time)
		print("  day %d after %.0f days away: %.2f regrown (%.2f at the reference rate)" % [leg[0], (Time.get_unix_time_from_system() - t_left) * ruts_scale / 86400.0, seasonal[-1], ref])
		seasonal.append(ref)
		surface.press_box(a, Vector2(0.11, 0.29), 0.0, 0.0, 0.04)
		for i in 4:
			await get_tree().physics_frame
		surface.sync_deformation()
	_check("a winter away leaves the rut bare (mud round 7)", seasonal[0] < 0.25 and seasonal[0] < 0.5 * seasonal[1], "%.2f regrown in 90 winter days (%.2f at the reference rate)" % [seasonal[0], seasonal[1]])
	_check("a summer away greens it at about the reference rate", seasonal[2] > 0.7 * seasonal[3] and seasonal[2] < 1.6 * seasonal[3], "%.2f regrown in 35 summer days (%.2f at the reference rate)" % [seasonal[2], seasonal[3]])
	_check("a Mediterranean summer's drought stalls it", seasonal[4] < 0.3 * seasonal[5], "%.2f regrown in 35 August days (%.2f at the reference rate)" % [seasonal[4], seasonal[5]])
	wm.queue_free()
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
		# On the slope the bed's texels step 5 mm each along it: the surface read back there is a few steps out.
		_check("its water stands level and stays in it", tmax - tmin < (0.004 if kind == "flat" else 0.025) and outside < 0.0005, "%.1f mm, %.2f mm outside" % [(tmax - tmin) * 1000.0, outside * 1000.0])
		_check("it carries the clay asked for", absf(surface.get_water_mud_at(o) - 40.0) < 8.0, "%.1f kg/m3" % surface.get_water_mud_at(o))


# Puddle ice (mud round 7): a puddle freezes over a night at -5 C (Stefan's law), bears a foot but breaks under a
# wheel, the plates freeze back together, and a thaw melts it.
func _run_ice_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.35, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	var o := Vector3(0.5, 0, 0.3)
	surface.add_puddle(o, 1.2, 0.1, 0.0)
	for i in 240:
		await get_tree().physics_frame
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 1.0
	wm.temperature = 10.0
	add_child(wm)
	wm.snap_to_settings()
	for i in 10:
		await get_tree().physics_frame
	_check("no ice above freezing", surface.get_ice_at(o).x == 0.0, "%.1f mm" % (surface.get_ice_at(o).x * 1000.0))
	wm.temperature = -5.0
	wm.snap_to_settings()
	# Twelve hours, sped up 5000 times.
	surface.runoff_time_scale = 5000.0
	var t0 := Time.get_ticks_msec()
	var frames := 0
	while frames < 2000 and surface.get_ice_at(o).x < 0.03:
		await get_tree().physics_frame
		frames += 1
	var water := surface.get_standing_water_at(o)
	var ice := surface.get_ice_at(o)
	var hours := frames / 60.0 * 5000.0 / 3600.0
	print("  frost: %.1f cm of ice on %.1f cm of water after %.1f h at -5 C (%d frames, %d ms)" % [ice.x * 100.0, water * 100.0, hours, frames, Time.get_ticks_msec() - t0])
	# Stefan with Ashton's 0.6: h = sqrt(2 * 0.6 * 7.2e-9 * 5 t) - 5 mm: 3 cm in some 6 h.
	_check("the puddle freezes over at Stefan's rate", ice.x >= 0.03 and hours > 3.0 and hours < 12.0, "%.1f cm in %.1f h" % [ice.x * 100.0, hours])
	_check("dry ground grows no ice", surface.get_ice_at(o + Vector3(0, 0, 2.5)).x == 0.0, "%.1f mm" % (surface.get_ice_at(o + Vector3(0, 0, 2.5)).x * 1000.0))
	surface.runoff_time_scale = 1.0
	var g := surface.get_surface_height_at(o)
	surface.press(o, 0.06, g - 0.005, 0.005, true)
	_check("3 cm of ice bears a foot (Gold 1971)", surface.get_ice_at(o).y == 0.0, "broken %.2f" % surface.get_ice_at(o).y)
	surface.press_wheel(o, Vector2(0.25, 0.3), 0.0, 0.02, Vector2())
	var broken := surface.get_ice_at(o)
	var beside := surface.get_ice_at(o + Vector3(0.75, 0, 0))
	_check("a wheel breaks it into plates", broken.y > 0.9 and beside.y < 0.1, "broken %.2f under it, %.2f 0.75 m away" % [broken.y, beside.y])
	surface.runoff_time_scale = 5000.0
	for i in 120:
		await get_tree().physics_frame
	_check("the plates freeze back together", surface.get_ice_at(o).y < 0.5 * broken.y, "broken %.2f" % surface.get_ice_at(o).y)
	wm.temperature = 8.0
	wm.snap_to_settings()
	frames = 0
	while frames < 2000 and surface.get_ice_at(o).x > 0.0:
		await get_tree().physics_frame
		frames += 1
	hours = frames / 60.0 * 5000.0 / 3600.0
	print("  thaw: melted in %.1f h at 8 C, %.1f cm of water left" % [hours, surface.get_standing_water_at(o) * 100.0])
	_check("a thaw melts it (degree days)", surface.get_ice_at(o).x == 0.0 and hours < 24.0, "%.1f mm left after %.1f h" % [surface.get_ice_at(o).x * 1000.0, hours])
	surface.runoff_time_scale = 1.0
	wm.queue_free()
	near_on = true


# Mud on plants (mud round 7): clay thrown onto the sward and muddy water splashed onto it spatter it; a tyre's print
# does not; rain washes it off.
func _run_plant_mud_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.35, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.tracked_mud = true
	var a := Vector3(1.0, 0, 0.0)
	var b := Vector3(-1.5, 0, 0.0)
	var c := Vector3(0.0, 0, 2.0)
	surface.track_mud(a, 0.25, 0.0004, 1.0)
	surface.add_water(b, 0.3, 0.0006, 60.0)
	surface.track_mud(c, 0.25, 0.0004, 1.0, Vector4(1.0, 0.0, 0.0, 0.0))
	for i in 4:
		await get_tree().physics_frame
	var pa := surface.get_plant_mud_at(a)
	var pb := surface.get_plant_mud_at(b)
	var pc := surface.get_plant_mud_at(c)
	_check("thrown clay spatters the plants it lands among", pa > 0.9 and surface.get_plant_mud_at(a + Vector3(1.0, 0, 0)) == 0.0, "%.2f there, %.2f a metre off" % [pa, surface.get_plant_mud_at(a + Vector3(1.0, 0, 0))])
	_check("muddy water splashed out does too", pb > 0.9, "%.2f" % pb)
	_check("a tyre's print does not", pc == 0.0, "%.2f" % pc)
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 4.0
	add_child(wm)
	wm.snap_to_settings()
	surface.runoff_time_scale = 3600.0
	# A second sped up 3600 times: an hour at 4 mm/h, e-fold 2 mm: about a seventh is left.
	for i in 60:
		await get_tree().physics_frame
	var left := surface.get_plant_mud_at(a)
	_check("rain washes it off within millimetres", left < 0.3 * pa, "%.2f left after about 4 mm" % left)
	surface.runoff_time_scale = 1.0
	wm.queue_free()
	near_on = true


# Needle ice (mud round 9): a frosty night on wet bare soil grows needles that lift the top (the frost front held at
# the surface), a boot sinks deeper into the weak mat, and the thaw leaves loose crumbs that print deeper still.
func _run_needle_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.4, 0.0)
	surface.mud_layer = false
	surface.frozen_ground = true
	surface.needle_ice = true
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 0.9
	wm.temperature = 5.0
	wm.season_day = 300
	add_child(wm)
	wm.snap_to_settings()
	for i in 150:
		await get_tree().physics_frame
	# A boot's Bekker sinkage on the unfrozen soil first, for scale.
	var plain := surface.get_sinkage_at(Vector3(-2, 0, -2), 80.0, 0.03, 0.1)
	var q := Vector3(0, 0, -2)
	var g0 := surface.get_surface_height_at(q)
	wm.temperature = -3.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	for i in 430: # about 10 h
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	for i in 150:
		await get_tree().physics_frame
	surface.sync_deformation()
	var nd := surface.get_needle_ice_at(q)
	var fr := surface.get_frost_at(q)
	var lift := surface.get_surface_height_at(q) - g0
	print("  needle ice: %.1f cm after a night at -3 C, frost %.1f cm deep, the ground %.1f cm higher" % [nd.x * 100.0, fr.y * 100.0, lift * 100.0])
	_check("a frosty night on wet bare soil grows needle ice, 1 to 4 cm (Outcalt, Lawler)", nd.x > 0.01 and nd.x < 0.04, "%.1f cm" % (nd.x * 100.0))
	_check("the needles hold the frost front at the surface", fr.y < 0.01, "%.1f cm" % (fr.y * 100.0))
	_check("and lift the ground by their height", absf(lift - nd.x) < 0.004, "%.1f cm for %.1f cm" % [lift * 100.0, nd.x * 100.0])
	for i in 150: # the cells take it (every 2 s)
		await get_tree().physics_frame
	var on_needles := surface.get_sinkage_at(Vector3(2, 0, -2), 80.0, 0.03, 0.1)
	_check("a boot sinks deeper into the needle mat than into the soil before", on_needles > 1.5 * plain, "%.1f mm (%.1f mm before)" % [on_needles * 1000.0, plain * 1000.0])
	# The thaw: the needles melt, the cap drops back as loose crumbs.
	wm.temperature = 5.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	var frames := 0
	while frames < 1500 and surface.get_needle_ice_at(q).x > 0.0:
		await get_tree().physics_frame
		frames += 1
	surface.runoff_time_scale = 1.0
	for i in 150:
		await get_tree().physics_frame
	surface.sync_deformation()
	nd = surface.get_needle_ice_at(q)
	var back := surface.get_surface_height_at(q) - g0
	print("  thaw: needles gone after %.1f h, crumbs %.2f, the ground %.1f mm off where it was" % [frames / 60.0 * 5000.0 / 3600.0, nd.y, back * 1000.0])
	_check("the thaw melts them and the ground drops back, left as loose crumbs", nd.x == 0.0 and nd.y > 0.5 and absf(back) < 0.003, "crumbs %.2f, %.1f mm" % [nd.y, back * 1000.0])
	var loose := surface.get_sinkage_at(Vector3(-2, 0, -2), 80.0, 0.03, 0.1)
	_check("the crumbs print deeper than the soil did", loose > 1.2 * plain, "%.1f mm (%.1f mm before)" % [loose * 1000.0, plain * 1000.0])
	# The ground contract (docs/ideas/world-contracts.md) answers from this ground; a stub overrides only what it sets.
	var wg: WorldGround = WorldContracts.get_ground()
	var p := Vector3(-2, 0, -2)
	var same: bool = wg.has_provider() and absf(wg.height_at(p) - surface.get_surface_height_at(p)) < 1e-5 and wg.needle_ice_at(p) == surface.get_needle_ice_at(p) and bool(wg.soil_at(p)["soft"])
	var stub := GDScript.new()
	stub.source_code = "extends WorldGround\nfunc _standing_water_at(_p: Vector3) -> float:\n\treturn 0.5\n"
	stub.reload()
	WorldContracts.set_ground(stub.new())
	var stubbed: WorldGround = WorldContracts.get_ground()
	var mixed: bool = stubbed.standing_water_at(p) == 0.5 and absf(stubbed.height_at(p) - surface.get_surface_height_at(p)) < 1e-5
	WorldContracts.clear_overrides()
	_check("the ground contract answers from the ground, and a stub replaces only what it overrides", same and mixed and WorldContracts.get_ground().standing_water_at(p) == surface.get_standing_water_at(p), "live %s, stubbed %s" % [same, mixed])
	wm.queue_free()

# Needle ice from a radiation frost (world-links): a clear calm November night with the air at +1.5 C cools the bare
# surface some degrees below freezing (the weather's surface temperature), and the needles grow; under an overcast
# sky the surface stays with the air and none do.
func _run_needle_radiation_tests() -> void:
	near_on = false
	var grown := []
	for overcast in [false, true]:
		await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.4, 0.0)
		surface.mud_layer = false
		surface.frozen_ground = true
		surface.needle_ice = true
		var clock: Node = ClassDB.instantiate("SkyClock")
		clock.latitude = 46.0
		clock.month = 11
		clock.day = 15
		clock.time_of_day = 1.0
		clock.time_scale = 0.0
		add_child(clock)
		var wm := WeatherMap3D.new()
		wm.cloud_cover = 1.0 if overcast else 0.0
		wm.rain_rate = 0.0
		wm.humidity = 0.75
		wm.temperature = 1.5
		wm.wind_speed = 1.0
		wm.air_daily_cycle = false
		wm.air_follows_climate = false
		add_child(wm)
		wm.sky_clock = wm.get_path_to(clock)
		wm.snap_to_settings()
		for i in 150:
			await get_tree().physics_frame
		var q := Vector3(0, 0, -2)
		var ts: float = WorldContracts.get_weather().surface_temperature_at(q)
		surface.runoff_time_scale = 5000.0
		for i in 430: # about 10 h
			await get_tree().physics_frame
		surface.runoff_time_scale = 1.0
		grown.append([surface.get_needle_ice_at(q).x, ts])
		wm.queue_free()
		clock.queue_free()
		await get_tree().physics_frame
	print("  radiation frost: %.1f cm of needles under a clear sky (surface %.1f C), %.1f cm overcast (surface %.1f C), the air +1.5 C" % [grown[0][0] * 100.0, grown[0][1], grown[1][0] * 100.0, grown[1][1]])
	_check("a clear calm night with the air above freezing grows needle ice from the surface's radiative cooling; an overcast one does not", grown[0][0] > 0.005 and grown[1][0] == 0.0, "%.1f cm clear, %.1f cm overcast" % [grown[0][0] * 100.0, grown[1][0] * 100.0])

# Frozen ground (mud round 8): a night at -8 C freezes the top some 10 cm, a boot no longer marks it and an old rut stays
# as it is; a thaw leaves a soaked layer on the frost that a boot sinks deeper into than before the frost.
func _run_frost_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.4, 0.0)
	surface.mud_layer = true
	surface.frozen_ground = true
	surface.needle_ice = false # its own test (_run_needle_tests)
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 0.9
	wm.temperature = 8.0
	wm.season_day = 200 # summer: the climate gives no frost to start from
	add_child(wm)
	wm.snap_to_settings()
	for i in 6:
		await get_tree().physics_frame
	var g := surface.get_surface_height_at(Vector3(-2, 0, 0))
	surface.press_box(Vector3(-2, 0, 0), Vector2(0.11, 0.29), 0.0, g - 0.1, 0.05)
	for i in 30:
		await get_tree().physics_frame
	surface.sync_deformation()
	var before := g - surface.get_surface_height_at(Vector3(-2, 0, 0))
	var sink0: float = surface.get_wheel_soil(Vector3(0.8, 0, 2), 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	var rut := Vector3(1.5, 0, 0)
	g = surface.get_surface_height_at(rut)
	surface.press_box(rut, Vector2(0.3, 1.0), 0.0, g - 0.1, 0.05)
	for i in 30:
		await get_tree().physics_frame
	surface.sync_deformation()
	var rut0 := surface.get_surface_height_at(rut)
	wm.temperature = -8.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	for i in 520: # about 12 h
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	var fr := surface.get_frost_at(Vector3(0, 0, -2))
	print("  frost: %.1f cm deep after about 12 h at -8 C" % (fr.y * 100.0))
	_check("a night's frost freezes the top (Stefan, Berggren)", fr.y > 0.06 and fr.y < 0.2, "%.1f cm" % (fr.y * 100.0))
	for i in 150: # the cells take it (every 2 s)
		await get_tree().physics_frame
	var q := Vector3(0, 0, -2)
	g = surface.get_surface_height_at(q)
	surface.press_box(q, Vector2(0.11, 0.29), 0.0, g - 0.1, 0.05)
	for i in 30:
		await get_tree().physics_frame
	surface.sync_deformation()
	var frozen := g - surface.get_surface_height_at(q)
	_check("a boot barely marks frozen ground", frozen < 0.2 * before, "%.1f mm (%.1f mm unfrozen)" % [frozen * 1000.0, before * 1000.0])
	_check("the rut already there stays", absf(surface.get_surface_height_at(rut) - rut0) < 0.003, "%.1f mm moved" % ((surface.get_surface_height_at(rut) - rut0) * 1000.0))
	wm.temperature = 6.0
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	var frames := 0
	while frames < 1500 and surface.get_frost_at(q).x < 0.04:
		await get_tree().physics_frame
		frames += 1
	surface.runoff_time_scale = 1.0
	fr = surface.get_frost_at(q)
	print("  thaw: %.1f cm thawed over %.1f cm of frost after %.1f h" % [fr.x * 100.0, fr.y * 100.0, frames / 60.0 * 5000.0 / 3600.0])
	for i in 150:
		await get_tree().physics_frame
	# A loaded wheel (3 kN, 70 cm tyre at 200 kPa) on it: Bekker's sinkage, the thawed slurry against the summer soil.
	var sink1: float = surface.get_wheel_soil(Vector3(0.8, 0, 2), 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	_check("a wheel sinks deeper in the thaw's soaked layer than before the frost", fr.x > 0.03 and fr.y > fr.x and sink1 > 1.2 * sink0, "%.1f mm (%.1f mm before the frost), %.1f cm thawed" % [sink1 * 1000.0, sink0 * 1000.0, fr.x * 100.0])
	wm.queue_free()
	# Under a lasting pack (Kane and Stein 1983): frost made before the snow lies on under it, so a new node on a
	# snowy late-winter day starts with concrete frost, not none.
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.4, 0.0)
	var cold := WeatherMap3D.new()
	cold.climate = load("res://cold_climate.gd").new()
	add_child(cold)
	var day := -1
	var st := {}
	for d in range(1, 120):
		st = cold.get_season_on_day(d)
		if float(st["snow_cover"]) > 0.5 and float(st["soil_frost"]) > 50.0:
			day = d
			break
	cold.season_day = maxi(day, 1)
	cold.snap_to_settings()
	surface.frozen_ground = false # read the season again
	for i in 2:
		await get_tree().physics_frame
	surface.frozen_ground = true
	for i in 60:
		await get_tree().physics_frame
	var under: Vector3 = surface.get_frost_at(Vector3(0.5, 0, 0.5))
	print("  day %d: snow cover %.2f, %.0f K days of frost: %.1f cm of frost under the pack" % [day, float(st.get("snow_cover", 0.0)), float(st.get("soil_frost", 0.0)), under.y * 100.0])
	_check("frost made before the snow stays under the pack", day > 0 and under.y > 0.05, "%.1f cm on day %d" % [under.y * 100.0, day])
	cold.queue_free()
	near_on = true


# Ruts steer the wheels (mud round 9): a tyre 25 cm wide over a 30 cm rut 6 cm deep running along z.
func _run_rut_guidance_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.5, 0.0)
	surface.mud_layer = false
	for k in 12:
		var z := -2.5 + 0.45 * k
		var g := surface.get_surface_height_at(Vector3(0, 0, z))
		surface.press_box(Vector3(0, 0, z), Vector2(0.3, 0.5), 0.0, g - 0.06, 0.0)
	for i in 40:
		await get_tree().physics_frame
	surface.sync_deformation()
	var lat := Vector3(1, 0, 0)
	var centred := surface.get_rut_guidance(Vector3(0, 0, 0), lat, 0.125)
	var right := surface.get_rut_guidance(Vector3(0.12, 0, 0), lat, 0.125)
	var left := surface.get_rut_guidance(Vector3(-0.12, 0, 0), lat, 0.125)
	var flat := surface.get_rut_guidance(Vector3(2.0, 0, 0), lat, 0.125)
	print("  rut guidance: centred %s, 12 cm right %s, 12 cm left %s, off the rut %s" % [centred, right, left, flat])
	_check("centred in a rut, no push; walls as high as the rut is deep", absf(centred.x) < 0.05 and centred.y > 0.04 and centred.z > 0.04, "slope %.2f, walls %.1f and %.1f cm" % [centred.x, centred.y * 100.0, centred.z * 100.0])
	_check("half out of it, the tilt pushes it back in", right.x > 0.1 and left.x < -0.1, "slope %.2f right, %.2f left" % [right.x, left.x])
	_check("on unmarked ground, nothing", absf(flat.x) < 0.01 and flat.y < 0.005 and flat.z < 0.005, "%s" % flat)
	if ClassDB.class_exists("PhysXVehicle3D"):
		# A car driven straight with its wheels 15 cm inside a pair of old ruts: the walls under the treads tip it
		# into them (it drifts sideways), where on unmarked soil it keeps its line.
		var drift := []
		for ruts in [false, true]:
			await _soil_scene("soil")
			surface.moisture = 0.3
			surface.mud_layer = false
			if ruts:
				for side in [-1.0, 1.0]:
					for k in 30:
						var q := Vector3(side * 0.8 + 0.15, 0, -1.0 + 0.45 * k)
						var g := surface.get_surface_height_at(q)
						surface.press_box(q, Vector2(0.3, 0.5), 0.0, g - 0.12, 0.0)
				for i in 40:
					await get_tree().physics_frame
				surface.sync_deformation()
			var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
			surface.follow_target = surface.get_path_to(car)
			var dr: Array = await _drive(car, 4.0, 0.4)
			print("  (%.1f m driven, sinkage %.1f cm)" % [dr[0], dr[1] * 100.0])
			drift.append(car.global_position.x)
			car.queue_free()
		print("  a car driven straight: %.1f cm sideways on unmarked soil, %.1f cm with its wheels 15 cm inside old ruts" % [drift[0] * 100.0, drift[1] * 100.0])
		_check("old ruts pull the wheels into them", drift[1] - drift[0] > 0.05 and absf(drift[0]) < 0.05, "%.1f cm, %.1f cm unmarked" % [drift[1] * 100.0, drift[0] * 100.0])
		# The centre ridge (mud round 10): deep old ruts with the soil between them standing 25 cm proud. The wheels drop
		# into the ruts, the belly lands on the ridge and takes the car's weight off them: it hangs up where it drives
		# on along shallow ruts.
		var hang := {}
		for deep in [false, true]:
			await _soil_scene("soil")
			surface.moisture = 0.3
			surface.mud_layer = false
			for side in [-1.0, 1.0]:
				for k in 30:
					var q := Vector3(side * 0.8, 0, -1.0 + 0.45 * k)
					var g := surface.get_surface_height_at(q)
					surface.press_box(q, Vector2(0.3, 0.5), 0.0, g - (0.35 if deep else 0.12), 0.0)
			if deep:
				for k in 40:
					surface.deposit(Vector3(0, 0, 0.5 + 0.3 * k), 0.4, 0.25)
			for i in 40:
				await get_tree().physics_frame
			surface.sync_deformation()
			var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
			surface.follow_target = surface.get_path_to(car)
			for i in 30:
				await get_tree().physics_frame
			var start := car.global_position
			car.throttle = 0.5
			var held := 0.0
			for i in 240:
				await get_tree().physics_frame
				held += car.get_belly_support() / 240.0
			car.throttle = 0.0
			hang[deep] = [(car.global_position - start).length(), held]
			car.queue_free()
		print("  ruts: %.1f m along shallow ones; %.1f m along deep ones over a ridge, the belly holding %.0f N of %.0f on average" % [hang[false][0], hang[true][0], hang[true][1], 1500.0 * 9.81])
		_check("a ridge between deep ruts lifts the car onto its belly and it hangs up", hang[true][1] > 0.3 * 1500.0 * 9.81 and hang[true][0] < 0.3 * hang[false][0] and hang[false][1] == 0.0,
				"%.1f m, belly %.0f N; %.1f m in shallow ruts" % [hang[true][0], hang[true][1], hang[false][0]])
	near_on = true


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
	var held := [0, 0.0]  # boots broken out of the mud, the most one was held by (N)
	lm.boot_released.connect(func(_leg: int, f: float) -> void:
		held[0] += 1
		held[1] = maxf(held[1], f))
	await _walk_legs(legs, 3.0, 1.4)
	_check("on dry soil the boots lift free (no suction)", held[0] == 0, "%d releases" % held[0])
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
	# Suction (mud round 9): each boot is held as it lifts out of the slurry and breaks free with a release.
	print("  suction: %d releases, held by up to %.0f N" % [held[0], held[1]])
	_check("the slurry holds each boot as it lifts, then lets go", held[0] >= 6 and held[1] > 10.0 and held[1] < lm.boot_hold_limit, "%d releases, %.0f N" % [held[0], held[1]])
	var quick := GroundLegMud3D.suction_force(4.14, 0.4, 0.03, 0.1, 1.0)
	var slow := GroundLegMud3D.suction_force(4.14, 0.4, 0.03, 0.1, 0.005)
	var shallow := GroundLegMud3D.suction_force(4.14, 0.05, 0.03, 0.1, 1.0)
	_check("a boot 40 cm deep in stiff clay mud lifted quickly needs about A c Nc = 1.1 kN; slowly a quarter; shallow less", absf(quick - 1118.0) < 5.0 and absf(slow / quick - 0.25) < 0.01 and shallow < 0.2 * quick, "%.0f N quick, %.0f slow, %.0f at 5 cm" % [quick, slow, shallow])
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
		# The plough's own trough: the liquid mud's waves (round 14.1) run it full again within a second.
		surface.mud_waves = false
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
	# Treads (mud round 10): the same 4 s in stiff sticky clay on road tyres, mud tyres and chains.
	var treads := {}
	for t in [PhysXVehicleWheel3D.TREAD_ROAD, PhysXVehicleWheel3D.TREAD_MUD, PhysXVehicleWheel3D.TREAD_CHAINS]:
		await _soil_scene("soil")
		await _paint_mud(0.8, STIFF_MUD)
		var tc := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		tc.belly_drag = false
		for c in tc.get_children():
			if c is PhysXVehicleWheel3D:
				c.tread = t
		surface.follow_target = surface.get_path_to(tc)
		var r: Array = await _drive(tc, 4.0, 0.5)
		var fill := 0.0
		for w in 4:
			fill += tc.get_wheel_mud(w) / 4.0
		var clods := 0.0
		for w in 4:
			clods += tc.get_wheel_clods_thrown(w)
		treads[t] = [r[0], fill, clods]
		print("  tread %d: %.2f m in 4 s, tread %.0f%% full, %.1f kg of clods cut and flung" % [t, r[0], fill * 100.0, clods])
	var rd: Array = treads[PhysXVehicleWheel3D.TREAD_ROAD]
	var mt: Array = treads[PhysXVehicleWheel3D.TREAD_MUD]
	var ch: Array = treads[PhysXVehicleWheel3D.TREAD_CHAINS]
	_check("road tyres pack with clay, mud tyres throw it out", rd[1] > 1.2 * mt[1], "%.0f%% full against %.0f%%" % [rd[1] * 100.0, mt[1] * 100.0])
	# Round 23.3: lugs and above all chains' cross links cut the stiff clay in blocks and fling them as clods.
	_check("lugs and chains cut clods out of stiff clay, chains most; road tyres none", ch[2] > mt[2] and mt[2] > 1.0 and rd[2] == 0.0, "%.1f kg chains, %.1f kg mud tyres, %.1f kg road" % [ch[2], mt[2], rd[2]])
	_check("mud tyres and chains get further in sticky clay than road tyres", mt[0] > rd[0] + 0.1 and ch[0] > rd[0] + 0.1,
			"%.2f m and %.2f m against %.2f m" % [mt[0], ch[0], rd[0]])
	var g_dry := PhysXVehicleWheel3D.tread_firm_grip(PhysXVehicleWheel3D.TREAD_CHAINS, 0.0, 0.0)
	var g_ice := PhysXVehicleWheel3D.tread_firm_grip(PhysXVehicleWheel3D.TREAD_CHAINS, 1.0, 0.0)
	_check("chains slide on dry road and bite on ice", g_dry < 0.8 and g_ice > 2.5, "%.2f dry, %.2f on ice" % [g_dry, g_ice])


# Round 24.1, alone as --test=balling.
func _run_balling_tests() -> void:
	if not ClassDB.class_exists("PhysXVehicle3D"):
		return
	# Road tyres spinning on in stiff sticky clay ball up past their full tread: the tyre grows a cake of
	# clay; spun fast, it sheds in chunks, the lopsided rest shakes the truck, until it is spun clean.
	await _soil_scene("soil")
	await _paint_mud(0.8, STIFF_MUD)
	var bc := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	bc.belly_drag = false
	surface.follow_target = surface.get_path_to(bc)
	var cake_max := 0.0
	for i in 30:
		await get_tree().physics_frame
	bc.throttle = 0.45
	for i in 900:
		await get_tree().physics_frame
		for w in 4:
			cake_max = maxf(cake_max, bc.get_wheel_cake(w))
	var cake_grown := 0.0
	for w in 4:
		cake_grown += bc.get_wheel_cake(w) / 4.0
	print("  balled up: cake %.1f cm a wheel (%.1f cm at most), tread %.0f%% full" % [cake_grown * 100.0, cake_max * 100.0, bc.get_wheel_mud(0) * 100.0])
	_check("sticky clay balls up on the tyres past a full tread", cake_grown > 0.008, "%.1f cm of clay on the tyre" % (cake_grown * 100.0))
	# Floored: the tread races past the cake's shedding speed.
	var shudder_max := 0.0
	bc.brake = 0.0
	bc.throttle = 1.0
	for i in 480:
		await get_tree().physics_frame
		for w in 4:
			shudder_max = maxf(shudder_max, bc.get_wheel_shudder(w))
	bc.throttle = 0.0
	var cake_left := 0.0
	for w in 4:
		cake_left += bc.get_wheel_cake(w) / 4.0
	print("  floored: cake %.1f cm left, shudder up to %.0f N" % [cake_left * 100.0, shudder_max])
	_check("spun fast the cake sheds in chunks, the lopsided rest shakes the truck", cake_left < 0.6 * cake_grown and shudder_max > 50.0,
			"%.1f cm to %.1f cm; up to %.0f N of shake" % [cake_grown * 100.0, cake_left * 100.0, shudder_max])


# Each tread's rut floor (mud round 10), also alone as --test=treads: a 26 cm wide tyre rolled 2 m over damp sand
# (a press every 5 cm, 3 cm deep, as the vehicle prints), its rut floor read along the middle every 5 mm.
func _run_tread_rut_tests() -> void:
	var rough := {}
	for t in [-1, PhysXVehicleWheel3D.TREAD_ROAD, PhysXVehicleWheel3D.TREAD_ALL_TERRAIN, PhysXVehicleWheel3D.TREAD_MUD, PhysXVehicleWheel3D.TREAD_CHAINS]:
		near_on = true
		await _soil_scene("damp_sand")
		surface.follow_target = NodePath()
		for i in 10:
			await get_tree().physics_frame
		var o := Vector3(0.3, 0, 0.2)
		for k in 40:
			surface.press_wheel(o + Vector3(0, 0, 0.05 * k), Vector2(0.26, 0.2), 0.0, 0.03, Vector2(), t)
			await get_tree().physics_frame
		for i in 10:
			await get_tree().physics_frame
		surface.sync_deformation()
		var vals: Array[float] = []
		for k in 300:
			var q := o + Vector3(0.05, 0, 0.25 + 0.005 * k)
			vals.append(surface.get_dent_at(q) - surface.get_rim_at(q))
		var mean := 0.0
		for v in vals:
			mean += v / vals.size()
		var sd := 0.0
		var deep := 0.0
		for v in vals:
			sd += (v - mean) * (v - mean) / vals.size()
			deep = maxf(deep, v - mean)
		rough[t] = [sqrt(sd), deep, mean]
		print("  tread %d rut: %.1f cm deep, floor relief %.2f mm rms, %.1f mm deepest below the mean" % [t, mean * 100.0, sqrt(sd) * 1000.0, deep * 1000.0])
	# Driven: a car on road tyres and one on mud tyres over damp sand at half throttle, the rut floor 1 to 3 m behind
	# the rear left wheel, against its 20 cm running mean.
	var driven := {}
	for t in [PhysXVehicleWheel3D.TREAD_ROAD, PhysXVehicleWheel3D.TREAD_MUD]:
		await _soil_scene("damp_sand")
		var tc := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		tc.belly_drag = false
		var wl: PhysXVehicleWheel3D = null
		for c in tc.get_children():
			if c is PhysXVehicleWheel3D:
				c.tread = t
				wl = c
		surface.follow_target = surface.get_path_to(tc)
		for i in 30:
			await get_tree().physics_frame
		var a0 := wl.global_position
		var r: Array = await _drive(tc, 4.0, 0.5)
		tc.brake = 1.0
		for i in 30:
			await get_tree().physics_frame
		surface.sync_deformation()
		var a1 := wl.global_position
		var dir := ((a1 - a0) * Vector3(1, 0, 1)).normalized()
		var dv: Array[float] = []
		for k in 400:
			var q := a1 - dir * (1.0 + 0.005 * k)
			dv.append(surface.get_dent_at(q) - surface.get_rim_at(q))
		var ds := 0.0
		var dm := 0.0
		for k in range(20, dv.size() - 20):
			var m := 0.0
			for j in range(k - 20, k + 21):
				m += dv[j] / 41.0
			ds += (dv[k] - m) * (dv[k] - m) / (dv.size() - 40)
			dm += dv[k] / (dv.size() - 40)
		driven[t] = sqrt(ds)
		print("  driven, tread %d: %.1f m, rut %.1f cm, floor relief %.2f mm rms, slip %.2f" % [t, r[0], dm * 100.0, sqrt(ds) * 1000.0, r[2]])
	_check("a driven car's mud tyres print their lugs in its rut", driven[PhysXVehicleWheel3D.TREAD_MUD] > 2.0 * driven[PhysXVehicleWheel3D.TREAD_ROAD] + 0.001,
			"%.1f mm rms against %.1f on road tyres" % [driven[PhysXVehicleWheel3D.TREAD_MUD] * 1000.0, driven[PhysXVehicleWheel3D.TREAD_ROAD] * 1000.0])
	_check("each tread prints its own rut floor: lugs over a plain floor, deeper mud lugs, chain links cut in",
			rough[PhysXVehicleWheel3D.TREAD_ALL_TERRAIN][0] > 2.0 * rough[-1][0] + 0.002 and rough[PhysXVehicleWheel3D.TREAD_MUD][0] > 1.2 * rough[PhysXVehicleWheel3D.TREAD_ALL_TERRAIN][0] and rough[PhysXVehicleWheel3D.TREAD_CHAINS][1] > rough[PhysXVehicleWheel3D.TREAD_ALL_TERRAIN][1] + 0.004,
			"rms plain %.1f, road %.1f, all terrain %.1f, mud %.1f, chains %.1f mm; chains %.1f mm below the mean against %.1f" % [rough[-1][0] * 1000.0, rough[0][0] * 1000.0, rough[1][0] * 1000.0, rough[2][0] * 1000.0, rough[3][0] * 1000.0, rough[3][1] * 1000.0, rough[1][1] * 1000.0])

# Mud slides downhill in rain (mud round 10): 40 cm of mud on a 15 degree bank (20 cm counted while only stiff, as
# wet as it is). Stiff, it holds; soaked for a minute, the layer
# flows off the bank down to the depth that still holds (tau_y / (rho g tan 15)), into a rut at the foot, volume kept.
func _run_slide_tests() -> void:
	var res := {}
	for k in ["stiff", "soaked"]:
		near_on = false
		await _setup("bank", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
		await _paint_mud(0.4, STIFF_MUD)
		for i in 60:
			await get_tree().physics_frame
		# A rut at the foot of the bank, 2 m long across it, 25 cm deep.
		var foot := Vector3(-2.6, 0, 0)
		surface.press_box(foot, Vector2(0.4, 2.0), 0.0, height.get_height(foot) - 0.25, 0.25)
		for i in 120:
			await get_tree().physics_frame
		surface.sync_deformation()
		var rut0 := surface.get_dent_at(foot) - surface.get_rim_at(foot)
		var mv0: Vector2 = surface.get_moved_volume()
		var vol0 := mv0.x - mv0.y
		if k == "soaked":
			surface.moisture = 1.0
		for i in 3600:
			await get_tree().physics_frame
		surface.sync_deformation()
		var mid := Vector3(1.0, 0, 0.5) # the upper bank, which drains first
		var prof := ""
		for xi in 13:
			var q := Vector3(-3.0 + 0.5 * xi, 0, 0.5)
			prof += "%.0f/%.0f " % [surface.get_dent_at(q) * 100.0, surface.get_rim_at(q) * 100.0]
		print("  %s profile dent/rim cm from x=-3: %s gpu %s mud %.2f" % [k, prof, surface.is_gpu_active(), surface.get_mud_depth_at(mid)])
		var left := surface.get_mud_depth_at(mid) - surface.get_dent_at(mid) + surface.get_rim_at(mid)
		var rut1 := surface.get_dent_at(foot) - surface.get_rim_at(foot)
		var surf_slope := rad_to_deg(atan((surface.get_surface_height_at(mid + Vector3(0.25, 0, 0)) - surface.get_surface_height_at(mid - Vector3(0.25, 0, 0))) / 0.5))
		var mv1: Vector2 = surface.get_moved_volume()
		res[k] = [left, rut0, rut1, (mv1.x - mv1.y) - vol0, surface.get_mud_slide_angle_at(mid), surface.get_mud_depth_at(mid), surf_slope]
		print("  %s: %.1f cm of mud left on the upper bank, its surface %.1f deg (holds to %.1f deg); rut at the foot %.1f cm, then %.1f cm; volume change %.4f m3" % [k, left * 100.0, res[k][6], res[k][4], rut0 * 100.0, rut1 * 100.0, res[k][3]])
	_check("stiff mud holds on a 15 degree bank", res["stiff"][0] > 0.95 * res["stiff"][5] and absf(res["stiff"][2] - res["stiff"][1]) < 0.02, "%.1f cm left of %.1f" % [res["stiff"][0] * 100.0, res["stiff"][5] * 100.0])
	# A Bingham layer stands where its weight down its free surface stays under the yield stress, rho g h tan(surface)
	# <= tau_y (Coussot 1997): soaked, the bank's mud runs down until its surface is that gentle. On a long even bank
	# the layer that stops is the infinite slope's critical depth h = tau_y / (rho g sin theta), its surface parallel
	# to the ground (as the liquid mud's waves leave it, round 14.1); slumping alone left it gentler.
	_check("soaked, it slides down the bank until its surface is as gentle as it holds", res["soaked"][0] < 0.7 * res["soaked"][5] and res["soaked"][6] <= res["soaked"][4] + 1.5,
			"%.1f cm left of %.1f on the upper bank, its surface %.1f deg (holds to %.1f) on 15 degree ground" % [res["soaked"][0] * 100.0, res["soaked"][5] * 100.0, res["soaked"][6], res["soaked"][4]])
	_check("and fills the rut at the foot, volume kept", res["soaked"][2] < res["soaked"][1] - 0.08 and absf(res["soaked"][3]) < 0.02,
			"%.1f cm to %.1f cm; volume change %.4f m3" % [res["soaked"][1] * 100.0, res["soaked"][2] * 100.0, res["soaked"][3]])


# Gullies in ruts (mud round 11): a 6 degree slope of soil with a rut pressed down it, in an hour of heavy rain (sped
# up 100 times). The rut gathers the runoff and its loosened floor cuts; the untouched soil beside it, past whose
# critical shear the sheet flow never gets, holds; where the slope eases at its foot the cut soil is laid as a fan.
# Sand under the same rain washes off everywhere.
func _run_gully_tests() -> void:
	var res := {}
	var kept_all := 0.0
	for k in ["soil"]:
		near_on = false
		await _setup("flat" if OS.get_cmdline_user_args().has("--flatgully") else "incline", GroundSurface3D.GROUND_SOIL if k == "soil" else GroundSurface3D.GROUND_SAND, 0.3, 0.0)
		surface.mud_layer = false
		surface.standing_water = true
		surface.runoff_channels = not OS.get_cmdline_user_args().has("--noerode")
		# A 20-minute downpour at 10 mm/h, run 30 times faster (its water at 300 mm/h, its soil moving 30 times as fast).
		surface.runoff_time_scale = 30.0
		for a in OS.get_cmdline_user_args():
			if a.begins_with("--tscale="):
				surface.runoff_time_scale = float(a.substr(9))
		var wm := WeatherMap3D.new()
		wm.cloud_cover = 1.0
		wm.rain_rate = 0.0
		wm.humidity = 0.7
		wm.temperature = 15.0
		add_child(wm)
		wm.snap_to_settings()
		for i in 10:
			await get_tree().physics_frame
		# The rut: a tyre's 30 cm print, 6 cm deep, from x = 4 down to x = -7.
		# Pressed every 5 cm: each print's floor is level, so sparse prints on the slope leave a stair of flat treads.
		# Down the slope and on out over the flat below it (x under -5), where the soil it carries settles.
		for j in 220:
			var q := Vector3(4.0 - 0.05 * j, 0, 0.0)
			surface.press_box(q, Vector2(0.3, 0.3), 0.0, height.get_height(q) - 0.06, 0.06)
		for i in 30:
			await get_tree().physics_frame
		# Water poured in the rut stays in it (runoff once took any texel over 2 mm deep for a lake and dropped it).
		surface.add_water(Vector3(1.0, height.get_height(Vector3(1, 0, 0)), 0.0), 0.1, 0.002)
		for i in 60:
			await get_tree().physics_frame
		surface.sync_deformation()
		var kept := surface.get_standing_water_volume(Vector3(-1.5, 0, 0), 6.0) / 0.002
		print("  %.0f%% of 2 L poured in the rut still in it a second later" % (kept * 100.0))
		kept_all = kept
		var grain := 10.0
		var gframes := 2400
		for a in OS.get_cmdline_user_args():
			if a.begins_with("--grain="):
				grain = float(a.substr(8))
			if a.begins_with("--gframes="):
				gframes = int(a.substr(10))
		wm.rain_rate = grain
		wm.snap_to_settings()
		for i in gframes:
			await get_tree().physics_frame
			if i == 300:
				surface.sync_deformation()
				var row0 := ""
				for zi in 13:
					var q := Vector3(1.0, 0, -0.3 + 0.05 * zi)
					var ro: Vector2 = surface.get_runoff_at(q)
					row0 += "%.0f/%.1f/%.0f/%.0f " % [surface.get_runoff_cut_at(q) * 1000.0, ro.x * 1000.0, ro.y * 100.0, (surface.get_dent_at(q) - surface.get_rim_at(q)) * 1000.0]
				print("    early x 1: %s" % row0)
		surface.sync_deformation()
		var rut_cut := 0.0
		var side_cut := 0.0
		for j in 21:
			var x := 3.0 - 0.25 * j
			rut_cut += surface.get_runoff_cut_at(Vector3(x, 0, 0.0)) / 21.0
			side_cut += absf(surface.get_runoff_cut_at(Vector3(x, 0, 1.5))) / 21.0
		var fan := 0.0
		for xi in 12:
			for zi in 9:
				fan = maxf(fan, -surface.get_runoff_cut_at(Vector3(-5.2 - 0.12 * xi, 0, -0.2 + 0.05 * zi)))
		res[k] = [rut_cut, side_cut, fan]
		for xx in [3.0, 1.0, -1.0, -3.0]:
			var row := ""
			for zi in 13:
				var q := Vector3(xx, 0, -0.3 + 0.05 * zi)
				var ro: Vector2 = surface.get_runoff_at(q)
				row += "%.0f/%.1f/%.0f/%.0f " % [surface.get_runoff_cut_at(q) * 1000.0, ro.x * 1000.0, ro.y * 100.0, (surface.get_dent_at(q) - surface.get_rim_at(q)) * 1000.0]
			print("    x %.0f cut mm/depth mm/speed cm/s/dent mm across z -0.3..0.3: %s" % [xx, row])
		print("  %s: rut floor cut %.1f mm, beside it %.2f mm; fan below %.1f mm deep" % [k, rut_cut * 1000.0, side_cut * 1000.0, fan * 1000.0])
		wm.queue_free()
	_check("water poured in a rut stays there with runoff on", kept_all > 0.9, "%.0f%% kept" % (kept_all * 100.0))
	_check("rain cuts a gully down the rut in soil", res["soil"][0] > 0.005 and res["soil"][0] > 5.0 * res["soil"][1], "%.1f mm cut along the rut, %.2f mm beside it" % [res["soil"][0] * 1000.0, res["soil"][1] * 1000.0])
	_check("and lays the soil it cut as a fan where the slope eases", res["soil"][2] > 0.002, "%.1f mm laid" % (res["soil"][2] * 1000.0))


# Mud round 11: the winch. A car on firm ground with a cable to a stump 20 m ahead.
func _winch_scene(p_mud: float, p_slack := 0.3, p_moisture := STIFF_MUD) -> Array:
	await _soil_scene("soil")
	if p_mud > 0.0:
		await _paint_mud(p_mud, p_moisture)
	var stump := StaticBody3D.new()
	var sc := CollisionShape3D.new()
	sc.shape = BoxShape3D.new()
	(sc.shape as BoxShape3D).size = Vector3(0.4, 1.0, 0.4)
	stump.add_child(sc)
	stump.position = Vector3(0, 0.5, 20.0)
	add_child(stump)
	var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	car.belly_drag = p_mud > 0.0
	surface.follow_target = surface.get_path_to(car)
	var w := PhysXWinch3D.new()
	w.position = Vector3(0, 0.5, 2.1)
	car.add_child(w)
	w.anchor = w.get_path_to(stump)
	w.cable_length = 17.9 + p_slack
	for i in 60:
		await get_tree().physics_frame
	return [car, w, stump]


func _run_winch_tests() -> void:
	if not ClassDB.class_exists("PhysXWinch3D"):
		print("(no PhysX: winch checks skipped)")
		return
	# Free rolling on firm ground: the car comes in at the no-load line speed, the cable barely loaded.
	var sc: Array = await _winch_scene(0.0)
	var car: PhysXVehicle3D = sc[0]
	var w: PhysXWinch3D = sc[1]
	w.drum = PhysXWinch3D.DRUM_IN
	for i in 180:
		await get_tree().physics_frame
	var z0 := car.global_position.z
	var t_sum := 0.0
	for i in 300:
		await get_tree().physics_frame
		t_sum += w.get_tension() / 300.0
	var v_free := (car.global_position.z - z0) / 5.0
	print("  rolling: %.3f m/s, tension %.0f N, line speed there %.3f m/s" % [v_free, t_sum, w.line_speed_at(t_sum)])
	_check("a free-rolling car comes in at the line speed its light load allows", absf(v_free - w.line_speed_at(t_sum)) < 0.25 * w.line_speed_at(t_sum) and t_sum < 0.15 * w.rated_pull,
			"%.3f m/s against %.3f m/s, %.0f N" % [v_free, w.line_speed_at(t_sum), t_sum])
	# Braked: dragged on locked wheels, the pull is the tyres' sliding friction (about 0.8 m g) and the line slows.
	sc = await _winch_scene(0.0)
	car = sc[0]
	w = sc[1]
	car.brake = 1.0
	car.handbrake = 1.0
	w.drum = PhysXWinch3D.DRUM_IN
	for i in 240:
		await get_tree().physics_frame
	z0 = car.global_position.z
	t_sum = 0.0
	for i in 300:
		await get_tree().physics_frame
		t_sum += w.get_tension() / 300.0
	var v_brk := (car.global_position.z - z0) / 5.0
	var mg := 1500.0 * 9.81
	print("  braked: %.3f m/s, tension %.0f N (%.2f m g), line speed there %.3f m/s" % [v_brk, t_sum, t_sum / mg, w.line_speed_at(t_sum)])
	# (This soil's friction is about half; it bore a seventh of the weight before braked wheels slid on it, round 11.)
	_check("a braked car is dragged on its sliding tyres: the pull near the soil's friction, the line slowed by it", t_sum > 0.3 * mg and t_sum < 1.0 * mg and absf(v_brk - w.line_speed_at(t_sum)) < 0.1 * w.line_speed_at(t_sum),
			"%.0f N (%.2f m g), %.3f m/s against %.3f m/s" % [t_sum, t_sum / mg, v_brk, w.line_speed_at(t_sum)])
	# Slack, the brake on: the cable hangs in a catenary 2 m longer than the span, lying on the ground at its middle.
	sc = await _winch_scene(0.0, 2.0)
	w = sc[1]
	var pts: PackedVector3Array = w.get_cable_points(24)
	var chord_mid := (pts[0] + pts[24]) * 0.5
	var sag := chord_mid.y - pts[12].y
	print("  slack: tension %.0f N, sag %.2f m, middle %.2f m above the ground" % [w.get_tension(), sag, pts[12].y - surface.get_surface_height_at(pts[12])])
	_check("slack cable carries nothing and sags onto the ground", w.get_tension() < 50.0 and sag > 0.3 and pts[12].y - surface.get_surface_height_at(pts[12]) < 0.02,
			"%.0f N, %.2f m sag" % [w.get_tension(), sag])
	# Wound taut: near straight under load (sag w L^2 / 8 T, millimetres).
	sc = await _winch_scene(0.0)
	car = sc[0]
	w = sc[1]
	car.brake = 1.0
	car.handbrake = 1.0
	w.drum = PhysXWinch3D.DRUM_IN
	for i in 240:
		await get_tree().physics_frame
	pts = w.get_cable_points(24)
	sag = (pts[0] + pts[24]).y * 0.5 - pts[12].y
	var span := pts[0].distance_to(pts[24])
	var sag_theory := w.cable_mass * 9.81 * span * span / (8.0 * maxf(w.get_tension(), 1.0))
	_check("under load the cable runs near straight, its sag w L^2 / 8 T", absf(sag - sag_theory) < 0.3 * sag_theory,
			"%.1f mm sag at %.0f N, theory %.1f mm" % [sag * 1000.0, w.get_tension(), sag_theory * 1000.0])
	# Stuck in deep soft mud (a belly-deep slurry): the car alone at half throttle for 6 s, then the winch with it.
	var wmud := 0.6
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--wmud="):
			wmud = float(a.substr(7))
	sc = await _winch_scene(0.8, 0.3, wmud)
	car = sc[0]
	w = sc[1]
	var r: Array = await _drive(car, 6.0, 0.5)
	car.throttle = 0.0
	z0 = car.global_position.z
	w.cable_length = 20.0 - 2.1 - z0 + 0.1
	w.drum = PhysXWinch3D.DRUM_IN
	car.throttle = 0.3
	var t_max := 0.0
	for i in 1200:
		await get_tree().physics_frame
		t_max = maxf(t_max, w.get_tension())
	var winched := car.global_position.z - z0
	print("  mud: %.2f m alone in 6 s, then %.2f m winched in 20 s, peak tension %.0f N" % [r[0], winched, t_max])
	# Winched it comes in at the drum's no-load line speed (0.14 m/s), so the margin is that over the car's own crawl.
	_check("the winch pulls the car on through deep mud it could not leave alone", winched > 1.5 and r[0] < 0.5 and winched > 1.8 * r[0] / 6.0 * 20.0,
			"%.2f m in 20 s winched, %.2f m in 6 s alone" % [winched, r[0]])
	_check("and its pull stays within the winch's stall", t_max < 1.6 * w.rated_pull, "peak %.0f N" % t_max)
	# Another vehicle as the anchor: a braked car winches a dead one (neutral, no brakes) to it.
	await _soil_scene("soil")
	var a := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	var b := _make_car(1500.0, 700.0, Vector3(0, 0.9, 12.0))
	a.brake = 1.0
	a.handbrake = 1.0
	var w2 := PhysXWinch3D.new()
	w2.position = Vector3(0, 0.5, 2.1)
	a.add_child(w2)
	w2.anchor = w2.get_path_to(b)
	w2.anchor_point = Vector3(0, 0.5, -2.1)
	w2.cable_length = 12.0 - 4.2 + 0.2
	for i in 60:
		await get_tree().physics_frame
	var bz := b.global_position.z
	var az := a.global_position.z
	w2.drum = PhysXWinch3D.DRUM_IN
	for i in 600:
		await get_tree().physics_frame
	print("  tow: the dead car came %.2f m, the braked one %.2f m" % [bz - b.global_position.z, a.global_position.z - az])
	_check("a vehicle anchor: the braked car winches the dead one in", bz - b.global_position.z > 0.8 and absf(a.global_position.z - az) < 0.2,
			"%.2f m against %.2f m" % [bz - b.global_position.z, a.global_position.z - az])


# Mud round 11: body mud dries by the weather, rain wets it, and dried clods shake off on jolts.
func _flake_car(p_weather: Array) -> Array:
	await _soil_scene("soil")
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0 if p_weather[3] <= 0.0 else 1.0
	wm.temperature = p_weather[0]
	wm.humidity = p_weather[1]
	wm.wind_speed = p_weather[2]
	wm.rain_rate = p_weather[3]
	add_child(wm)
	wm.snap_to_settings()
	var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	car.body_mud = true
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	for i in 60:
		await get_tree().physics_frame
	# Twelve clods thrown at the sides and the tailgate.
	for k in 12:
		var side := -1.0 if k % 2 == 0 else 1.0
		var at := car.global_transform * Vector3(0.9 * side, 0.4 + 0.08 * (k % 3), -1.6 + 0.3 * k)
		car.add_mud_splat(at, car.global_basis * Vector3(side * 6.0, 1.0, 0.0), 0.0002)
	return [car, wm]


func _run_flake_tests() -> void:
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: flake checks skipped)")
		return
	# Hot, dry and windy against cool, damp and still: the same mud, 5 s later.
	var r: Array = await _flake_car([35.0, 0.05, 10.0, 0.0])
	var car: PhysXVehicle3D = r[0]
	for i in 300:
		await get_tree().physics_frame
	var hot := car.get_body_mud_wetness()
	r = await _flake_car([8.0, 0.95, 0.0, 0.0])
	car = r[0]
	for i in 300:
		await get_tree().physics_frame
	var damp := car.get_body_mud_wetness()
	print("  wetness after 5 s: %.3f hot, dry and windy; %.3f cool, damp and still" % [hot, damp])
	_check("body mud dries by the weather: fast in hot dry wind, slowly in damp still air", (1.0 - hot) > 8.0 * (1.0 - damp) and hot < 0.95,
			"%.3f against %.3f" % [hot, damp])
	# Dry it, then rain.
	r = await _flake_car([35.0, 0.05, 10.0, 0.0])
	car = r[0]
	var wm: WeatherMap3D = r[1]
	var t := 0
	while car.get_body_mud_wetness() > 0.02 and t < 3600:
		await get_tree().physics_frame
		t += 1
	var dried := car.get_body_mud_wetness()
	wm.cloud_cover = 1.0
	wm.rain_rate = 5.0
	wm.snap_to_settings()
	for i in 600:
		await get_tree().physics_frame
	print("  dried to %.3f in %.0f s, then 10 s of 5 mm/h rain: %.3f" % [dried, t / 60.0, car.get_body_mud_wetness()])
	_check("rain wets dried mud again", car.get_body_mud_wetness() > dried + 0.15, "%.3f from %.3f" % [car.get_body_mud_wetness(), dried])
	# Driven over three 15 cm bars: dried clods shake off, wet ones hold.
	var res := {}
	for k in ["wet", "dry"]:
		r = await _flake_car([8.0, 0.95, 0.0, 0.0] if k == "wet" else [35.0, 0.05, 10.0, 0.0])
		car = r[0]
		for z in [4.0, 6.5, 9.0]:
			var bar := StaticBody3D.new()
			var bc := CollisionShape3D.new()
			bc.shape = BoxShape3D.new()
			(bc.shape as BoxShape3D).size = Vector3(4.0, 0.15, 0.3)
			bar.add_child(bc)
			bar.position = Vector3(0, 0.075, z)
			add_child(bar)
		if k == "dry":
			t = 0
			while car.get_body_mud_wetness() > 0.02 and t < 3600:
				await get_tree().physics_frame
				t += 1
		# On the firm floor (no soft soil to bog in: the bars are the test).
		surface.follow_target = NodePath()
		surface.queue_free()
		await get_tree().physics_frame
		var before := car.get_mud_splat_count()
		car.throttle = 0.5
		for i in 900:
			await get_tree().physics_frame
		res[k] = [before, car.get_mud_splat_count(), car.get_mud_flakes_shed(), car.global_position.z]
		print("  %s: %d clods stuck before the bars, %d after, %d shed; at z %.1f" % [k, before, car.get_mud_splat_count(), car.get_mud_flakes_shed(), car.global_position.z])
	_check("dried clods shake off over bumps", res["dry"][2] >= 3 and res["dry"][3] > 9.5, "%d of %d shed" % [res["dry"][2], res["dry"][0]])
	_check("wet ones hold", res["wet"][2] == 0, "%d shed" % res["wet"][2])
	# Round 23.2: drying runs on the world's clock (a truck parked overnight dries overnight), and the next drive
	# sheds the dried crust without bumps: fast on smooth ground the air past the body lifts its curled plates.
	var clocks := {}
	for scale in [1.0, 60.0]:
		r = await _flake_car([20.0, 0.5, 2.0, 0.0])
		car = r[0]
		wm = r[1]
		wm.time_scale = scale
		for i in 120:
			await get_tree().physics_frame
		clocks[scale] = car.get_body_mud_wetness()
	print("  wetness after 2 s at a world clock of 1 and 60: %.3f, %.3f" % [clocks[1.0], clocks[60.0]])
	_check("parked mud dries on the world's clock", (1.0 - clocks[60.0]) > 30.0 * (1.0 - clocks[1.0]) and clocks[60.0] < 0.8, "%.3f at 60x, %.3f at 1x" % [clocks[60.0], clocks[1.0]])
	var runs := {}
	for k in ["crawl", "fast"]:
		r = await _flake_car([35.0, 0.05, 10.0, 0.0])
		car = r[0]
		car.max_engine_torque = 3000.0 if k == "fast" else 700.0
		# Parked an hour of world time (a minute a frame) to dry.
		(r[1] as WeatherMap3D).time_scale = 60.0
		t = 0
		while car.get_body_mud_wetness() > 0.02 and t < 600:
			await get_tree().physics_frame
			t += 1
		(r[1] as WeatherMap3D).time_scale = 1.0
		surface.follow_target = NodePath()
		surface.queue_free()
		await get_tree().physics_frame
		var before := car.get_mud_splat_count()
		car.throttle = 1.0 if k == "fast" else 0.15
		var top := 0.0
		for i in 600:
			await get_tree().physics_frame
			top = maxf(top, car.get_linear_velocity().length())
		runs[k] = [before, car.get_mud_flakes_shed(), top]
		print("  dried, %s on smooth ground for 10 s (top %.1f m/s): %d of %d clods shed" % [k, top, runs[k][1], before])
		car.queue_free()
	_check("driven fast, the air strips dried crust a crawl leaves", runs["fast"][1] >= 4 and runs["fast"][1] > runs["crawl"][1] + 3, "%d at speed, %d crawling" % [runs["fast"][1], runs["crawl"][1]])


# Tyre pressure (mud round 12): the same car at road pressure and aired down, on firm ground and in mud.
func _pressure_car(p_kpa: float, p_mud: bool) -> PhysXVehicle3D:
	await _soil_scene("soil")
	if p_mud:
		await _paint_mud(0.3, STIFF_MUD)
	var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
	for c in car.get_children():
		if c is PhysXVehicleWheel3D:
			c.tire_pressure = p_kpa
	if p_mud:
		surface.follow_target = surface.get_path_to(car)
	else:
		# The firm floor under the patch: no soft soil.
		surface.follow_target = NodePath()
		surface.queue_free()
	for i in 90:
		await get_tree().physics_frame
	return car


func _run_pressure_tests() -> void:
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: pressure checks skipped)")
		return
	# Standing on firm ground: an aired-down tyre is squashed further and the car stands lower.
	var res := {}
	for kpa in [200.0, 80.0]:
		var car := await _pressure_car(kpa, false)
		var y := car.global_position.y
		var dfl := car.get_wheel_deflection(0)
		# Coasting: up to speed, then off the throttle; the speed lost in 3 s.
		car.throttle = 0.4
		for i in 150:
			await get_tree().physics_frame
		car.throttle = 0.0
		for i in 10:
			await get_tree().physics_frame
		var v0 := car.get_forward_speed()
		for i in 180:
			await get_tree().physics_frame
		res[kpa] = [y, dfl, v0 - car.get_forward_speed()]
		print("  %.0f kPa on firm ground: deflection past road pressure %.1f mm, body at %.3f m, %.2f m/s lost coasting 3 s from %.1f m/s" % [kpa, dfl * 1000.0, y, res[kpa][2], v0])
	var drop: float = res[200.0][0] - res[80.0][0]
	_check("at road pressure the tyre stands as before", absf(res[200.0][1]) < 0.0005, "%.2f mm" % (res[200.0][1] * 1000.0))
	_check("aired down to 80 kPa it squashes 1 to 4 cm further (vertical stiffness after Rhyne 2005)", res[80.0][1] > 0.01 and res[80.0][1] < 0.04, "%.1f mm" % (res[80.0][1] * 1000.0))
	_check("and the car stands that much lower", absf(drop - res[80.0][1]) < 0.3 * res[80.0][1], "%.1f mm lower" % (drop * 1000.0))
	# Gillespie: f_r about 0.018 against 0.010 at these pressures, 0.0078 g more: 0.23 m/s over 3 s.
	var extra: float = res[80.0][2] - res[200.0][2]
	_check("it rolls with more resistance (Gillespie 1992: about 0.22 m/s more lost in 3 s)", extra > 0.12 and extra < 0.4, "%.2f m/s more" % extra)
	# In soft mud: the longer, softer flat sinks less and gets further.
	var mud := {}
	for kpa in [200.0, 80.0]:
		var car := await _pressure_car(kpa, true)
		var rest := 0.0
		for w in 4:
			rest += car.get_wheel_sinkage(w) / 4.0
		mud[kpa] = await _drive(car, 8.0, 0.4)
		mud[kpa].append(rest)
		print("  %.0f kPa in stiff mud: %.1f cm sunk standing; %.2f m in 8 s, sinkage %.1f cm, slip %.2f" % [kpa, rest * 100.0, mud[kpa][0], mud[kpa][1] * 100.0, mud[kpa][2]])
	_check("in mud, aired down it sinks less standing (the elastic mode, Wong 2008)", mud[80.0][3] < 0.85 * mud[200.0][3], "%.1f cm against %.1f cm" % [mud[80.0][3] * 100.0, mud[200.0][3] * 100.0])
	_check("and gets further", mud[80.0][0] > 1.15 * mud[200.0][0], "%.2f m against %.2f m" % [mud[80.0][0], mud[200.0][0]])


# A firm subsoil under the mud (round 12).
func _run_subsoil_tests() -> void:
	await _soil_scene("soil")
	surface.peat_bogs = false # the mud layer's own depth (a bog's peat is a layer of its own)
	await _paint_mud(0.3, 0.9)
	var o := Vector3(0, 0, 0)
	# The layer's depth: one depth with the base off; varied over metres with it on.
	var off_depth := surface.get_subsoil_depth_at(o)
	surface.subsoil = true
	await get_tree().physics_frame
	var lo := 10.0
	var hi := 0.0
	var mean := 0.0
	var m2 := 0.0
	var cnt := 0
	for xi in 21:
		for zi in 21:
			var dd := surface.get_subsoil_depth_at(Vector3(-10.0 + xi, 0, -10.0 + zi))
			lo = minf(lo, dd)
			hi = maxf(hi, dd)
			mean += dd
			m2 += dd * dd
			cnt += 1
	mean /= cnt
	var sd := sqrt(maxf(m2 / cnt - mean * mean, 0.0))
	print("  layer depth: %.2f m with no base; with it %.2f to %.2f m, mean %.2f, sd %.3f" % [off_depth, lo, hi, mean, sd])
	_check("the layer's depth over the base varies over metres, within 0.7 to 1.3 times", lo >= 0.69 * off_depth and hi <= 1.31 * off_depth and sd > 0.06 * mean, "%.2f to %.2f m, sd %.3f" % [lo, hi, sd])
	# A light load stops short of the base as the thin layer stiffens; a heavy one still goes down to it.
	var h := surface.get_subsoil_depth_at(o)
	var light: Dictionary = surface.get_wheel_soil(o, 2000.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	var heavy: Dictionary = surface.get_wheel_soil(o, 150000.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	surface.subsoil = false
	await get_tree().physics_frame
	var light_off: Dictionary = surface.get_wheel_soil(o, 2000.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	print("  in slurry %.2f m deep: a 2 kN wheel sinks %.1f cm (%.1f cm with no base), a 150 kN one %.1f cm; shear limits %.0f N and %.0f N" % [h, light["static_sinkage"] * 100.0, light_off["static_sinkage"] * 100.0, heavy["static_sinkage"] * 100.0, light["shear_limit"], heavy["shear_limit"]])
	_check("as the layer under a wheel thins the soil stiffens against the base (finite depth: Salman 2022)", light["static_sinkage"] < 0.95 * h and light_off["static_sinkage"] >= 0.99 * light_off["static_sinkage"], "%.1f cm of %.1f cm" % [light["static_sinkage"] * 100.0, h * 100.0])
	_check("a heavy load still goes down to it, and grips on it (friction 0.5)", heavy["static_sinkage"] > 0.98 * h and heavy["shear_limit"] >= 0.45 * 150000.0, "%.1f cm, %.0f N" % [heavy["static_sinkage"] * 100.0, heavy["shear_limit"]])
	# A car in slurry deep enough to bog it with no base, through to firm subsoil: it pulls itself out on the base.
	if not ClassDB.class_exists("PhysXVehicle3D"):
		return
	var dist := {}
	for on in [false, true]:
		await _soil_scene("soil")
		# Shallow enough that the belly clears it once the tyres are down on the base.
		await _paint_mud(0.16, 0.9)
		surface.subsoil = on
		var car := _make_car(1500.0, 700.0, Vector3(0, 0.9, 0))
		surface.follow_target = surface.get_path_to(car)
		var r: Array = await _drive(car, 8.0, 0.6)
		dist[on] = r
		print("  slurry 16 cm, base %s: %.2f m in 8 s, sinkage %.1f cm, slip %.2f" % ["on" if on else "off", r[0], r[1] * 100.0, r[2]])
	_check("in slurry down to the base the tyres bite on the subsoil: they stop spinning and the car crawls on", dist[true][0] > 3.0 * dist[false][0] and dist[true][2] < 0.6 and dist[false][2] > 0.9, "%.2f m at slip %.2f against %.2f m at slip %.2f" % [dist[true][0], dist[true][2], dist[false][0], dist[false][2]])


# A log or branch for a corduroy (round 12): a cylinder of green wood lying across the track at p_at.
func _make_log(p_at: Vector3, p_radius: float, p_length: float) -> RigidBody3D:
	var log := RigidBody3D.new()
	log.add_to_group("ground_mats")
	log.mass = 800.0 * PI * p_radius * p_radius * p_length
	var pm := PhysicsMaterial.new()
	pm.friction = 0.5 # rubber on wet bark
	log.physics_material_override = pm
	var cs := CollisionShape3D.new()
	var cy := CylinderShape3D.new()
	cy.radius = p_radius
	cy.height = p_length
	cs.shape = cy
	cs.rotation_degrees = Vector3(0, 0, 90)
	log.add_child(cs)
	var mi := MeshInstance3D.new()
	var cm := CylinderMesh.new()
	cm.top_radius = p_radius * 0.92
	cm.bottom_radius = p_radius
	cm.height = p_length
	cm.radial_segments = 16
	mi.mesh = cm
	mi.rotation_degrees = Vector3(0, 0, 90)
	var bark := StandardMaterial3D.new()
	bark.albedo_color = Color(0.27, 0.21, 0.15)
	bark.roughness = 0.9
	mi.material_override = bark
	log.add_child(mi)
	log.position = p_at
	add_child(log)
	return log


# Logs laid across a slurry track (round 12): the corduroy road.
func _run_mat_tests() -> void:
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: mat checks skipped)")
		return
	var res := {}
	for laid in [false, true]:
		await _soil_scene("soil")
		await _paint_mud(0.25, 0.9)
		var logs := 0
		if laid:
			var z := -2.0
			while z < 10.0:
				_make_log(Vector3(0, surface.get_surface_height_at(Vector3(0, 0, z)) + 0.15, z), 0.08, 2.4)
				logs += 1
				z += 0.42
			for i in 120:
				await get_tree().physics_frame
			print("  %d logs laid, %d taken by the ground" % [logs, surface.get_mat_count()])
			_check("logs settled on the soft ground are taken by it (frozen, lying on it)", surface.get_mat_count() == logs, "%d of %d" % [surface.get_mat_count(), logs])
		var car := _make_car(1500.0, 700.0, Vector3(0, surface.get_surface_height_at(Vector3.ZERO) + 1.0, 0))
		surface.follow_target = surface.get_path_to(car)
		var r: Array = await _drive(car, 8.0, 0.6)
		var sink := 0.0
		var most := 0.0
		for m in surface.get_mat_count():
			sink += surface.get_mat_sinkage(m) / maxi(surface.get_mat_count(), 1)
			most = maxf(most, surface.get_mat_sinkage(m))
		res[laid] = [r[0], r[1], sink, most]
		print("  slurry 25 cm, %s: %.2f m in 8 s, tyre sinkage %.1f cm, slip %.2f; logs sunk %.1f cm on average, %.1f cm at most" % ["on logs" if laid else "bare", r[0], r[1] * 100.0, r[2], sink * 100.0, most * 100.0])
		if laid:
			print("  load spread: %.2f m of a 16 cm log" % surface.get_mat_spread(0))
	_check("on a corduroy of logs the car drives through slurry it bogs in bare", res[true][0] > 4.0 and res[true][0] > 4.0 * res[false][0], "%.2f m against %.2f m" % [res[true][0], res[false][0]])
	_check("the logs spread the load along their length and sink far less than a tyre would (beam on elastic foundation)", res[true][3] > 0.002 and res[true][3] < 0.5 * res[false][1], "%.1f cm at most against %.1f cm" % [res[true][3] * 100.0, res[false][1] * 100.0])
	# A wheel's load on a log and on a branch: the log carries it, the branch snaps (green wood's strength).
	await _soil_scene("soil")
	await _paint_mud(0.25, 0.9)
	_make_log(Vector3(0, surface.get_surface_height_at(Vector3.ZERO) + 0.05, 0), 0.08, 2.4)
	_make_log(Vector3(0, surface.get_surface_height_at(Vector3(0, 0, 2)) + 0.05, 2.0), 0.015, 1.2)
	for i in 120:
		await get_tree().physics_frame
	if surface.get_mat_count() == 2:
		for i in 60:
			surface.add_mat_load(0, 3700.0)
			surface.add_mat_load(1, 3700.0)
			await get_tree().physics_frame
		print("  under a 3.7 kN wheel: log spread %.2f m, %s; 3 cm branch spread %.2f m, %s" % [surface.get_mat_spread(0), "broken" if surface.is_mat_broken(0) else "whole", surface.get_mat_spread(1), "broken" if surface.is_mat_broken(1) else "whole"])
		_check("a 16 cm log carries a wheel; a 3 cm branch snaps under it (bending P / 4 lambda past green wood's 40 MPa)", not surface.is_mat_broken(0) and surface.is_mat_broken(1), "log %s, branch %s" % [surface.is_mat_broken(0), surface.is_mat_broken(1)])
	else:
		_check("both the log and the branch are taken by the ground", false, "%d" % surface.get_mat_count())


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
	# --slurry: soaked to a slurry (mud round 14.1: the bow wave it pushes and the wake running back in)
	await _paint_mud(0.8, 1.0 if "--slurry" in OS.get_cmdline_user_args() else STIFF_MUD)
	surface.mud_waves = not "--no-waves" in OS.get_cmdline_user_args()
	# --buried: stones and roots hidden in the layer, dense (mud round 14.3), for the ruts to uncover
	if "--buried" in OS.get_cmdline_user_args():
		surface.buried_obstacles = true
		surface.buried_density = 0.6
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
	if "--crack" in OS.get_cmdline_user_args():
		# Mud round 11: a hot, dry wind until the clay has dried and cracked.
		var wm := WeatherMap3D.new()
		wm.cloud_cover = 0.0
		wm.temperature = 35.0
		wm.humidity = 0.05
		wm.wind_speed = 10.0
		add_child(wm)
		wm.snap_to_settings()
		var t := 0
		while car.get_body_mud_wetness() > 0.01 and t < 6000:
			await get_tree().physics_frame
			t += 1
		print("bodymud: dried in %d frames" % t)
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


# Mud round 7: a puddle frozen over a night at -5 C, a wheel's track broken through one side of it (--cam=x,y,z,tx,ty,tz
# in the world).
func _build_ice() -> void:
	kind = "soil"
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.35, 0.0)
	_apply(kind)
	surface.mud_layer = false
	surface.standing_water = true
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
	sun.rotation_degrees = Vector3(-25.0, 200.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 10.0
	add_child(sun)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	var o := Vector3(0, 0, 0)
	surface.add_puddle(o, 1.3, 0.08, 15.0)
	for i in 240:
		await get_tree().physics_frame
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 1.0
	wm.temperature = -5.0
	add_child(wm)
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	var frames := 0
	while frames < 2000 and surface.get_ice_at(o).x < 0.035:
		await get_tree().physics_frame
		frames += 1
	surface.runoff_time_scale = 1.0
	for k in 12:
		var q := Vector3(-1.6 + 0.15 * k, 0, 0.35)
		surface.press_wheel(q, Vector2(0.25, 0.3), 0.0, 0.02, Vector2())
	for i in 20:
		await get_tree().physics_frame
	print("ice: %s at the centre, %s in the track, %.1f cm of water" % [surface.get_ice_at(o), surface.get_ice_at(Vector3(-0.8, 0, 0.35)), surface.get_standing_water_at(o) * 100.0])
	if OS.get_cmdline_user_args().has("--banks"):
		var hs := []
		for k in 32:
			var a := TAU * k / 32.0
			var row := ""
			for j in range(10, 22):
				var q := Vector3(cos(a), 0, sin(a)) * (0.1 * j)
				row += " %+5.1f" % ((surface.get_surface_height_at(q) - surface.get_surface_height_at(Vector3(6, 0, 6))) * 100.0)
			hs.append(row)
		print("banks (cm, 1.0 to 2.1 m out):\n" + "\n".join(hs))
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([1.6, 1.0, 2.0, -0.3, 0.0, 0.0])
	cam.position = Vector3(c[0], c[1], c[2])
	cam.look_at(Vector3(c[3], c[4], c[5]))
	cam.fov = 45.0
	surface.follow_target = surface.get_path_to(cam)
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# Needle ice (mud round 9): a night at -3 C on wet bare soil, then a line of boot prints crushing it; --thaw: the next
# day's thaw, the crumbs it leaves (--cam=x,y,z,tx,ty,tz).
func _build_needle() -> void:
	kind = "soil"
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.4, 0.0)
	_apply(kind)
	surface.moisture = 0.4 # wet: needle ice wants a soil near field capacity
	surface.mud_layer = false
	surface.frozen_ground = true
	surface.needle_ice = true
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
	sun.rotation_degrees = Vector3(-25.0, 200.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 10.0
	add_child(sun)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 0.9
	wm.temperature = -3.0
	wm.season_day = 300
	add_child(wm)
	wm.snap_to_settings()
	surface.runoff_time_scale = 5000.0
	for i in 430:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	for i in 150:
		await get_tree().physics_frame
	# Boot prints walked across it, left and right.
	for k in 8:
		var q := Vector3(-1.4 + 0.35 * k, 0, 0.09 if k % 2 == 0 else -0.09)
		var g := surface.get_surface_height_at(q)
		surface.press_box(q, Vector2(0.29, 0.11), 0.0, g - 0.03, 0.01)
	for i in 30:
		await get_tree().physics_frame
	if OS.get_cmdline_user_args().has("--thaw"):
		wm.temperature = 6.0
		wm.snap_to_settings()
		surface.runoff_time_scale = 5000.0
		var frames := 0
		while frames < 1500 and surface.get_needle_ice_at(Vector3(1, 0, 1)).x > 0.0:
			await get_tree().physics_frame
			frames += 1
		surface.runoff_time_scale = 1.0
		for i in 150:
			await get_tree().physics_frame
	print("needle ice: %s away from the prints, %s in one" % [surface.get_needle_ice_at(Vector3(1, 0, 1)), surface.get_needle_ice_at(Vector3(-1.4, 0, 0.09))])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.9, 0.55, 1.1, -0.2, 0.0, 0.0])
	cam.position = Vector3(c[0], c[1], c[2])
	cam.look_at(Vector3(c[3], c[4], c[5]))
	cam.fov = 45.0
	surface.follow_target = surface.get_path_to(cam)
	surface.sync_deformation()
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
	# --still: the ground stops simulating once the view is set up (GroundSurface3D.simulation_paused), as a still
	# shot does; --prop=light_budget:true gives it the smaller fields too.
	if "--still" in OS.get_cmdline_user_args() and is_instance_valid(surface):
		surface.simulation_paused = true
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
	if OS.has_environment("RIPPLE_DUMP"): # the window's heights as raw float32 (n x n), for offline spectra
		var f := FileAccess.open(OS.get_environment("RIPPLE_DUMP"), FileAccess.WRITE)
		f.store_buffer(h.to_byte_array())
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


# Rain pits and rain crust (GroundSurface3D rain_pits; docs/ideas/sand.md): a shower pits the sand, a new pattern each
# rain; still air keeps them; saltation wipes them; a soaking leaves no crust while the skin is wet.
func _run_rain_tests() -> void:
	await _soil_scene("dry_sand")
	surface.wind_ripples = true
	surface.wind_override = Vector2(0.0, 0.0)
	var t0 := Time.get_ticks_msec()
	while not surface.is_ripples_ready() and Time.get_ticks_msec() - t0 < 60000:
		await get_tree().physics_frame
	_check("no rain, no pits", surface.get_rain_pit_cover() == 0.0 and surface.get_rain_crust() == 0.0, "cover %.2f, crust %.2f" % [surface.get_rain_pit_cover(), surface.get_rain_crust()])
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 120.0
	add_child(wm)
	wm.snap_to_settings()
	var o := Vector3(0.3, 0, 0.2)
	print("  rainfall %.1f mm/h at the patch" % WorldContracts.get_weather().rainfall_at(o))
	for i in 30:
		await get_tree().physics_frame
	var c1 := surface.get_rain_pit_cover()
	for i in 90:
		await get_tree().physics_frame
	var c2 := surface.get_rain_pit_cover()
	_check("a shower pits the sand, more as it goes on (covered by about 0.05 mm)", c1 > 0.1 and c2 > c1 and c2 > 0.8, "%.2f after 0.017 mm, %.2f after 0.067 mm" % [c1, c2])
	_check("no crust while the skin is wet", surface.get_rain_crust() < 0.05, "crust %.2f, wetness %.2f" % [surface.get_rain_crust(), WorldContracts.get_weather().surface_wetness_at(o)])
	wm.rain_rate = 0.0
	wm.cloud_cover = 0.0
	wm.snap_to_settings()
	for i in 120:
		await get_tree().physics_frame
	_check("still air keeps the pits", surface.get_rain_pit_cover() > 0.75, "%.2f" % surface.get_rain_pit_cover())
	# Wind past the threshold (sped up 100x): the saltation reworks the top grains.
	surface.wind_override = Vector2(12.0, 0.0)
	surface.ripple_time_scale = 100.0
	for i in 600:
		await get_tree().physics_frame
	_check("saltation wipes the uncrusted pits", surface.get_rain_pit_cover() < 0.05, "%.3f after 10 s at 12 m/s (x100)" % surface.get_rain_pit_cover())
	# The same saltation over damp sand built adhesion warts; rain flattens them.
	var ag := surface.get_adhesion_growth()
	_check("saltation grows adhesion warts", ag > 0.3, "growth %.2f" % ag)
	surface.wind_override = Vector2(0.0, 0.0)
	wm.cloud_cover = 1.0
	wm.rain_rate = 120.0
	wm.snap_to_settings()
	for i in 60:
		await get_tree().physics_frame
	wm.rain_rate = 0.0
	wm.cloud_cover = 0.0
	wm.snap_to_settings()
	_check("rain flattens them", surface.get_adhesion_growth() < ag * 0.99, "%.3f after 0.033 mm, was %.3f" % [surface.get_adhesion_growth(), ag])
	surface.adhesion_warts = false
	_check("adhesion_warts off reads nothing", surface.get_adhesion_growth() == 0.0, "off")
	surface.rain_pits = false
	_check("rain_pits off reads nothing", surface.get_rain_pit_cover() == 0.0 and surface.get_rain_crust() == 0.0, "off")
	wm.queue_free()


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
	# The ground contract's sand reads (docs/ideas/world-contracts.md) answer from this sand.
	var wgp := Vector3(0.3, 0, 0.2)
	var wg: WorldGround = WorldContracts.get_ground()
	_check("the ground contract reads sand, ripples and blown sand from the ground", wg.sand_at(wgp) > 0.9 and wg.ripple_at(wgp) == surface.get_ripple_at(wgp) and wg.blown_sand_at(wgp) == surface.get_blown_at(wgp.x, wgp.z) and wg.blown_sand_at(wgp) > 0.0,
			"sand %.2f, ripple %.1f mm, blown %.2f" % [wg.sand_at(wgp), wg.ripple_at(wgp) * 1000.0, wg.blown_sand_at(wgp)])
	var st := _ripple_stats()
	print("  grown in %.1f s (a worker); 8 m/s along x: wavelength %.1f cm along, %.1f cm across, slope energy %.0fx along, %.1f mm peak to peak" % [grow_s, st[0] * 100.0, st[1] * 100.0, st[2], st[3] * 1000.0])
	_check("ripples 5 to 15 cm long along the wind (Bagnold 1941; Andreotti 2006)", st[0] > 0.05 and st[0] < 0.15, "%.1f cm" % (st[0] * 100.0))
	_check("crests across the wind", st[2] > 4.0 and st[1] > 3.0 * st[0], "slope energy along / across %.1f, crests %.0f cm long between turns" % [st[2], st[1] * 100.0])
	_check("ripple index about 15 (7 mm at 11 cm)", st[3] > 0.005 and st[3] < 0.012, "%.1f mm peak to peak" % (st[3] * 1000.0))
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
	# A 1 m wall across the wind at x = 5 (a stand-in for the weather's obstacle lee, WorldWeather.obstacle_lee_at):
	# the cavity behind it to 2 heights, recovering over 6. The sand slows and drops in its lee (a shadow dune).
	WorldContracts.set_weather(WallLeeStub.new())
	for i in 70:
		await get_tree().physics_frame
	var lee := Vector3(7.0, 0, 0)
	var q_lee := surface.get_sand_flux_at(lee).length()
	var d_lee := surface.get_deposition_rate_at(Vector3(5.5, 0, 0))
	_check("blown sand slows and drops in a wall's lee (the weather's obstacle lee)", q_lee < 0.3 * q_open and d_lee * 3.6e6 > 1.0, "%.4f kg/m/s 2 m behind the wall against %.4f open; %.2f mm/h at its foot" % [q_lee, q_open, d_lee * 3.6e6])
	# Calm lee (lee_calm_ripples): the ripples gone in the cavity, low and stopped where the wind recovers, all upwind.
	var s_lee := surface.get_ripple_show_at(Vector3(6.0, 0, 0))
	var s_mid := surface.get_ripple_show_at(Vector3(13.0, 0, 0))
	var s_up := surface.get_ripple_show_at(up)
	_check("ripples gone in the calm lee, back as the wind recovers (Hesp 1981)", s_lee < 0.05 and s_up > 0.9 and s_mid > s_lee + 0.2, "share %.2f 1 m behind the wall, %.2f at 8 m, %.2f upwind" % [s_lee, s_mid, s_up])
	surface.obstacle_lee = false
	for i in 2:
		await get_tree().physics_frame
	await get_tree().create_timer(0.6).timeout
	_check("the lee switches off with obstacle_lee", surface.get_sand_flux_at(lee).length() > 0.8 * q_open, "%.4f kg/m/s" % surface.get_sand_flux_at(lee).length())
	surface.obstacle_lee = true
	WorldContracts.clear_overrides()
	# A pebble lag over x < -6 (a stand-in for the stone fields' WorldGround.coarse_cover_at): the sand among the
	# stones neither blows nor shows ripples.
	WorldContracts.set_ground(PebbleLagStub.new())
	for i in 70:
		await get_tree().physics_frame
	var lag := Vector3(-9.0, 0, 0)
	var q_lag := surface.get_sand_flux_at(lag).length()
	var s_lag := surface.get_ripple_show_at(lag)
	_check("a pebble lag armours the sand: no blowing, no ripples (Bagnold 1941)", q_lag < 0.05 * q_open and s_lag < 0.05 and surface.get_ripple_show_at(up) > 0.9, "%.4f kg/m/s among the pebbles against %.4f open; ripple share %.2f" % [q_lag, q_open, s_lag])
	WorldContracts.clear_overrides()
	surface.wind_override = Vector2.ZERO


# Runoff channels: a falling tide leaves the beach soaked; the water seeping out of it runs down the face and cuts
# rills before the sand drains (time scaled for the sand moved, the draining in real time).
func _runoff_beach(scale: float, shape := "beach") -> Node3D:
	await _setup(shape, GroundSurface3D.GROUND_SAND, 0.0, 0.0)
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
	# RUNOFF_DUMP_WIN=x0,z_top,size (m) for another window.
	var x0 := -2.0
	var z1 := 5.5
	var size := 4.0
	var win := OS.get_environment("RUNOFF_DUMP_WIN").split(",")
	if win.size() == 3:
		x0 = float(win[0])
		z1 = float(win[1])
		size = float(win[2])
	var n := 512
	var s := size / n
	var cut := PackedFloat32Array()
	cut.resize(n * n)
	var deepest := 0.0
	for py in n:
		for px in n:
			var c := surface.get_runoff_cut_at(Vector3(x0 + px * s, 0, z1 - py * s))
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
			wd[py * n + px] = surface.get_standing_water_at(Vector3(x0 + px * s, 0, z1 - py * s))
	var rawd := FileAccess.open(dump.get_basename() + "_water.f32", FileAccess.WRITE)
	if rawd:
		rawd.store_buffer(wd.to_byte_array())
	var wsum := 0.0
	var wmax := 0.0
	var wet := 0
	for py in range(0, n, 8):
		for px in range(0, n, 8):
			var w: float = surface.get_runoff_at(Vector3(x0 + px * s, 0, z1 - py * s)).x
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
	# --no-nmap: no stand-in noise normal map (its bumps shade as dark blotches under a low sun).
	m.normal_enabled = not no_nmap
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


func _build_rooting() -> void:
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
	sun.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-24.0, 150.0, 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([0.0, 1.6, 3.2, 0.0, 0.0, -0.2])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 55.0
	surface.follow_target = surface.get_path_to(cam)
	var st: WorldStand = WorldContracts.get_stand()
	st.clear_disturbances()
	for i in 3:
		st.add_disturbance(Vector3(-1.5 + 1.5 * i, 0, -0.3), 0.65, WorldStand.DISTURBANCE_ROOTING, [0.0, 1.0, 1.8][i])
	for i in 30:
		await get_tree().physics_frame
	var clods: GroundClods3D = surface.get_clods_node()
	if clods:
		clods.rooting = not OS.get_cmdline_user_args().has("--no-rooting")
		clods.update_now()
		print("rooting: %d turned clods lying, update %.2f ms worst" % [clods.get_rooted_count(), clods.get_update_msec_worst()])
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


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
	# Its boots print themselves: the deformers skip the capsule (WorldContactBody.feet).
	walker.set_meta("world_contact_feet", true)
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
	var clods: GroundClods3D = surface.get_clods_node()
	if clods:
		clods.update_now()
		print("clods: %d lying (%d torn out, %d landed); update %.2f ms mean, %.2f ms worst (%d updates)" % [clods.get_clod_count(), clods.get_spawned_total() - clods.get_landed_total(), clods.get_landed_total(), clods.get_update_msec_mean(), clods.get_update_msec_worst(), clods.get_update_runs()])
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
	sun.rotation_degrees = Vector3(-16.0, 150.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
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
	if tread_arg >= 0:
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				c.tread = tread_arg
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
		if frost_arg > 0.0:
			# A frost (-5 C, sped up 2000 times) on the water, then, with ",break", boots across the left rut.
			wm.temperature = -5.0
			wm.humidity = 1.0
			wm.snap_to_settings()
			surface.runoff_time_scale = 2000.0
			for i in int(frost_arg * 3600.0 / 2000.0 * 60.0):
				await get_tree().physics_frame
			surface.runoff_time_scale = 1.0
			if frost_break:
				for k in 6:
					var q := Vector3(-2.2 + 0.25 * k, 0, 0.3 * (k % 2))
					surface.press_box(q, Vector2(0.11, 0.28), 0.0, surface.get_surface_height_at(q) - 0.01, 0.01)
				for i in 10:
					await get_tree().physics_frame
			print("frost: ice %s in the left rut" % surface.get_ice_at(Vector3(-1.6, 0, 0.0)))
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


# Mud round 11: a car drives down a 6 degree soil slope, then 20 minutes of rain at 10 mm/h (sped up 30 times) run
# down its ruts, cut them into gullies and lay the soil as fans on the flat below; the frame from the flat, up the ruts.
func _build_gully() -> void:
	kind = "soil"
	await _setup("incline", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
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
	sun.rotation_degrees = Vector3(-20.0, 200.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	# Facing -x, down the slope.
	var car := _make_car(1500.0, 700.0, Vector3(6.0, height.get_height(Vector3(6.0, 0, 0)) + 0.9, 0.0))
	car.rotation.y = -PI * 0.5
	if tread_arg >= 0:
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				c.tread = tread_arg
	surface.follow_target = surface.get_path_to(car)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	car.throttle = 0.35
	var t := 0.0
	while t < 20.0 and car.global_position.x > -7.5:
		await get_tree().physics_frame
		t += 1.0 / 60.0
	car.throttle = 0.0
	car.brake = 1.0
	var car_end := car.global_position
	car.queue_free()
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([-8.5, 1.1, 2.2, -2.0, 0.0, 0.0])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 55.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 30:
		await get_tree().physics_frame
	surface.standing_water = true
	surface.runoff_channels = true
	surface.runoff_time_scale = 30.0
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 10.0
	wm.humidity = 0.7
	wm.temperature = 15.0
	add_child(wm)
	wm.snap_to_settings()
	for i in 2400:
		await get_tree().physics_frame
	# The rain eases off; the ruts still run.
	wm.rain_rate = 1.0
	wm.snap_to_settings()
	for i in 120:
		await get_tree().physics_frame
	surface.sync_deformation()
	for z in [-0.8, 0.8]:
		var row := ""
		for xi in 14:
			var x := 5.0 - 1.0 * xi
			var lo := 0.0
			for zi in 9:
				lo = maxf(lo, surface.get_runoff_cut_at(Vector3(x, 0, z - 0.2 + 0.05 * zi)))
			row += "%.0f " % (lo * 1000.0)
		print("gully: deepest cut mm across the rut at z %.1f, x 5 down to -8: %s" % [z, row])
	get_window().size = full
	print("gully: car at %s, field on the %s" % [car_end, "GPU" if surface.is_gpu_active() else "CPU"])
	await _save_view(env)
	get_tree().quit()


# Mud round 11: a 4x4 bogged in soft clay winches itself out to a tree; the frame from the side as the cable takes the load.
func _build_winch() -> void:
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
	sun.rotation_degrees = Vector3(-30.0, 60.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	await _paint_mud(0.8, 0.6)
	# The tree: a trunk 50 cm across, its bark a dark grey-brown.
	var tree := StaticBody3D.new()
	var tc := CollisionShape3D.new()
	tc.shape = CylinderShape3D.new()
	(tc.shape as CylinderShape3D).radius = 0.25
	(tc.shape as CylinderShape3D).height = 8.0
	tc.position = Vector3(0, 4.0, 0)
	tree.add_child(tc)
	var tm := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.2
	cyl.bottom_radius = 0.27
	cyl.height = 8.0
	cyl.radial_segments = 32
	tm.mesh = cyl
	tm.position = Vector3(0, 4.0, 0)
	var bark := StandardMaterial3D.new()
	bark.albedo_color = Color(0.2, 0.16, 0.12)
	bark.roughness = 0.9
	tm.material_override = bark
	tree.add_child(tm)
	tree.position = Vector3(1.2, _undisturbed(Vector3(1.2, 0, 15.0)) - 0.3, 15.0)
	add_child(tree)
	var car := _make_car(1500.0, 700.0, Vector3(0.0, _undisturbed(Vector3.ZERO) + 0.9, 0.0))
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	var w := PhysXWinch3D.new()
	w.position = Vector3(0, 0.45, 2.1)
	car.add_child(w)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	for i in 30:
		await get_tree().physics_frame
	# Bogged: it digs in on its own.
	car.throttle = 0.6
	for i in 240:
		await get_tree().physics_frame
	car.throttle = 0.0
	# The strap round the trunk, on the side facing the car, then wound in with the wheels turning gently.
	w.anchor = w.get_path_to(tree)
	w.anchor_point = Vector3(-0.15, 0.7, -0.22)
	w.cable_length = 0.0
	w.attached = true
	for i in 30:
		await get_tree().physics_frame
	w.drum = PhysXWinch3D.DRUM_IN
	car.throttle = 0.25
	var wframes := 600
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--wframes="):
			wframes = int(a.substr(10))
	for i in wframes:
		await get_tree().physics_frame
	car.throttle = 0.0
	w.drum = PhysXWinch3D.DRUM_HOLD
	for i in 10:
		await get_tree().physics_frame
	print("winch: car at %s, tension %.0f N, %.1f m of cable out" % [car.global_position, w.get_tension(), w.cable_length])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([5.5, 1.8, car.global_position.z - 3.0, 0.0, 0.6, car.global_position.z + 4.0])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# Mud round 12: a corduroy of logs laid across a muddy track, a car driven onto it from its own ruts. --wframes=N
# (default 360) frames of driving before the shot; --cam=x,y,z,tx,ty,tz as for the other views.
func _build_mats() -> void:
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
	sun.rotation_degrees = Vector3(-28.0, 210.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	await _paint_mud(0.3, 0.75)
	surface.subsoil = true
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	# Logs of 12 to 18 cm, 2.4 m long, laid across the track a hand apart, each a little askew.
	var rng := RandomNumberGenerator.new()
	rng.seed = 12
	var z := 1.6
	while z < 7.5:
		var r := rng.randf_range(0.06, 0.09)
		var lg := _make_log(Vector3(rng.randf_range(-0.1, 0.1), surface.get_surface_height_at(Vector3(0, 0, z)) + r + 0.05, z), r, rng.randf_range(2.2, 2.6))
		lg.rotation_degrees.y = rng.randf_range(-6.0, 6.0)
		z += 2.0 * r + rng.randf_range(0.02, 0.08)
	for i in 120:
		await get_tree().physics_frame
	var car := _make_car(1500.0, 700.0, Vector3(0.0, surface.get_surface_height_at(Vector3(0, 0, 0.3)) + 0.9, 0.3))
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	car.throttle = 0.6
	var wframes := 360
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--wframes="):
			wframes = int(a.substr(10))
	for i in wframes:
		await get_tree().physics_frame
	car.throttle = 0.0
	car.brake = 1.0
	for i in 30:
		await get_tree().physics_frame
	print("mats: car at %s, %d logs taken, deepest %.1f cm" % [car.global_position, surface.get_mat_count(), _deepest_mat() * 100.0])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([3.6, 1.4, car.global_position.z - 2.6, 0.0, 0.2, car.global_position.z + 0.6])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


func _deepest_mat() -> float:
	var d := 0.0
	for m in surface.get_mat_count():
		d = maxf(d, surface.get_mat_sinkage(m))
	return d


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
	# --terrace=0: the plain 1 in 12 face down to the water, without the low-tide terrace the fans form on.
	var terrace := not OS.get_cmdline_user_args().has("--terrace=0")
	await _runoff_beach(120.0, "beach_terrace" if terrace else "beach")
	surface.runoff_channels = on
	var mat := _ground_material("dry_sand")
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-14.0, 75.0, 0.0)
	# --rill-frames=N: a longer run (2400 is 84 minutes at x120), for the net to coarsen into fewer trunks.
	var frames := 2400
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--rill-frames="):
			frames = int(a.substr(14))
	for i in frames:
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
	# --cam2=x,dy,z,tx,dy,tz: a second frame of the same run (out with _2 before the extension), e.g. low and
	# side-on along the beach, where the cusps' bays show.
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--cam2=") and not out.is_empty():
			var c2 := a.substr(7).split_floats(",")
			if c2.size() == 6:
				cam.position = Vector3(c2[0], _undisturbed(Vector3(c2[0], 0, c2[2])) + c2[1], c2[2])
				cam.look_at(Vector3(c2[3], _undisturbed(Vector3(c2[3], 0, c2[5])) + c2[4], c2[5]))
				out = out.get_basename() + "_2." + out.get_extension()
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
	# A wider near field (12 m at the same texel): the run-up climbs to about 3 m above the still line, and the
	# default 8 m field's sharp middle ends 3 m from its centre.
	surface.near_resolution = 768
	surface.near_extent = 12.0
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
	sun.rotation_degrees = Vector3(-sun_alt, 120.0, 0.0)
	get_viewport().use_taa = taa
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
	if rain_pits_mm > 0.0:
		# --rain-pits=mm: a still shower of that much rain on the grown ripples (sped up), then the sun.
		surface.wind_override = Vector2(0.5, 0.0)
		var wm := WeatherMap3D.new()
		wm.cloud_cover = 1.0
		wm.rain_rate = 120.0
		add_child(wm)
		wm.snap_to_settings()
		for i in int(ceil(rain_pits_mm / 120.0 * 3600.0 * 60.0)):
			await get_tree().physics_frame
		wm.rain_rate = 0.0
		wm.cloud_cover = 0.0
		wm.snap_to_settings()
		for i in 30:
			await get_tree().physics_frame
		print("rain pits: cover %.2f, crust %.2f" % [surface.get_rain_pit_cover(), surface.get_rain_crust()])
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
	# Path traced: until every surface's acceleration structure is built (the deformed ground's captured vertices
	# among them), or the frame shows geometry from before (b1993654).
	var env_pt := false
	for c in get_children():
		if c is WorldEnvironment and (c as WorldEnvironment).environment and (c as WorldEnvironment).environment.pathtracing_enabled:
			env_pt = true
	var waited := 0
	while env_pt and waited < 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		await get_tree().process_frame
		waited += 1
	if env_pt:
		print("path traced after %d frames waiting for surfaces" % waited)
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
# Slip-face avalanches (docs/ideas/sand.md): a boot print on a dune's 32 degree slip face, between dry sand's
# stopping and starting angles, sets off a tongue of sand that runs down the face; the same print on the 10 degree
# stoss slope only relaxes its own walls; the face away from the print does not move.
# A boot's print (press_box's sole: heel, forefoot, tread), heading p_yaw (the toe along (sin, cos) in x, z).
func _avalanche_print(p: Vector3, p_yaw := 0.0) -> void:
	var h: float = height.get_height(p)
	surface.press_box(Vector3(p.x, h, p.z), Vector2(0.11, 0.29), p_yaw, h - 0.03, 0.03, true)


# The surface along the fall line through p and along a line 1.5 m to the side, every 2 cm from 0.5 m above p to
# 4 m below it: taken before a print and after, so the field's own sampling of the dune cancels.
func _avalanche_profile(p: Vector3) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	for side in [0.0, 1.5]:
		var x := p.x - 0.5
		while x < p.x + 4.0:
			out.append(surface.get_surface_height_at(Vector3(x, 0, p.z + side)))
			x += 0.02
	return out


# From two profiles: how far below p the surface changed by over a millimetre, the most it changed beside it, and
# the deepest it was lowered from 0.3 m below p down.
func _avalanche_reach(p: Vector3, a: PackedFloat32Array, b: PackedFloat32Array) -> Vector3:
	var m := a.size() / 2
	var reach := 0.0
	var deepest := 0.0
	for k in m:
		var dx := -0.5 + 0.02 * k
		var d := b[k] - a[k]
		if dx > 0.25 and absf(d) > 0.001:
			reach = dx
		if dx > 0.3:
			deepest = maxf(deepest, -d)
	var side := 0.0
	for k in m:
		side = maxf(side, absf(b[m + k] - a[m + k]))
	return Vector3(reach, side, deepest)


func _run_avalanche_tests() -> void:
	await _setup("dune", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	var face := Vector3(1.6, 0, 0)
	get_node("Probe").position = Vector3(2.0, height.get_height(Vector3(2.0, 0, 0)), 0.0)
	for i in 10:
		await get_tree().physics_frame
	surface.clear_deformation()
	surface.sync_deformation()
	var a := _avalanche_profile(face)
	_avalanche_print(face)
	var v0 := surface.get_moved_volume()
	# The flow runs at its own pace (about 1 m/s), then the field settles.
	for i in 360:
		await get_tree().physics_frame
	surface.settle(3.0)
	for i in 10:
		await get_tree().physics_frame
	var v1 := surface.get_moved_volume()
	var bb := _avalanche_profile(face)
	var r := _avalanche_reach(face, a, bb)
	_check("a print on a dry slip face sets off an avalanche down it (Daerr and Douady 1999)", r.x > 0.6, "the sand runs %.2f m below the print" % r.x)
	_check("the face beside it stays", r.y < 1e-4, "%.2f mm off 1.5 m to the side" % (r.y * 1000.0))
	_check("the avalanche keeps the sand", absf((v1.x - v1.y) - (v0.x - v0.y)) < 0.02 * maxf(v1.x, 1e-6), "net hollow %.3f l before, %.3f l after; %.2f l moved" % [(v0.x - v0.y) * 1000.0, (v1.x - v1.y) * 1000.0, v1.x * 1000.0])
	_check("it takes only a surface layer from the face", r.z < 0.035, "deepest scar below the print %.1f cm" % (r.z * 100.0))
	var lobe := 0.0
	var lobe_at := 0.0
	for k in bb.size() / 2:
		if -0.5 + 0.02 * k > 1.0 and bb[k] - a[k] > lobe:
			lobe = bb[k] - a[k]
			lobe_at = -0.5 + 0.02 * k
	_check("it piles a lobe at the toe (2 to 5 cm, Breton et al. 2008)", lobe > 0.015 and lobe < 0.06, "%.1f cm high %.2f m below the print" % [lobe * 100.0, lobe_at])
	surface.clear_deformation()
	var stoss := Vector3(-1.5, 0, 0)
	get_node("Probe").position = Vector3(-1.5, height.get_height(stoss), 0.0)
	for i in 10:
		await get_tree().physics_frame
	surface.sync_deformation()
	var sa := _avalanche_profile(stoss)
	_avalanche_print(stoss)
	for i in 120:
		await get_tree().physics_frame
	surface.settle(3.0)
	for i in 10:
		await get_tree().physics_frame
	var rs := _avalanche_reach(stoss, sa, _avalanche_profile(stoss))
	_check("the same print on the 10 degree stoss slope only relaxes its walls", rs.x < 0.3, "changed to %.2f m below it" % rs.x)


# The print's depth along the wind (x) through its middle, every 1 cm from -0.4 to +0.4 m.
func _infill_profile() -> PackedFloat32Array:
	surface.sync_deformation()
	var out := PackedFloat32Array()
	for k in 81:
		var p := Vector3(-0.4 + 0.01 * k, 0, 0)
		out.append(_undisturbed(p) - surface.get_surface_height_at(p))
	return out


# Mean depth over profile samples a..b (inclusive), and where the deepest one lies (m).
func _infill_stats(pr: PackedFloat32Array) -> Vector3:
	var up := 0.0
	var dn := 0.0
	var deep := -1.0
	var at := 0.0
	for k in pr.size():
		var x := -0.4 + 0.01 * k
		if x > -0.29 and x < -0.005:
			up += pr[k]
		elif x > 0.005 and x < 0.29:
			dn += pr[k]
		if pr[k] > deep:
			deep = pr[k]
			at = x
	return Vector3(up / 28.0, dn / 28.0, at)


# A walker with no feet of its own (WorldContactBody.walker): the ground presses its soles, a step about 0.41 of its
# stature, the feet 18 cm apart (WorldWalkerSteps), never its capsule.
func _run_steps_tests() -> void:
	await _setup("flat", GroundSurface3D.GROUND_SAND, 0.4, 0.0)
	_apply("damp_sand")
	var w := CharacterBody3D.new()
	var ws := CollisionShape3D.new()
	ws.shape = CapsuleShape3D.new()
	(ws.shape as CapsuleShape3D).radius = 0.14
	(ws.shape as CapsuleShape3D).height = 1.7
	w.add_child(ws)
	w.add_to_group("world_contact")
	var g0: float = height.get_height(Vector3(-1.5, 0, 0))
	w.position = Vector3(-1.5, g0 + 0.85, 0.0)
	add_child(w)
	for i in 10:
		await get_tree().physics_frame
	surface.clear_deformation()
	for i in 4:
		await get_tree().physics_frame
	for i in 120:
		w.position.x = -1.5 + 3.0 * float(i) / 119.0
		await get_tree().physics_frame
	for i in 10:
		await get_tree().physics_frame
	surface.sync_deformation()
	# Along each foot's line and the body's: where the surface went down over 1.5 mm.
	var dips := []
	for zl in [0.09, -0.09, 0.0]:
		var count := 0
		var gap := 100
		var lowered := 0
		var x := -1.4
		while x < 1.4:
			var d: float = height.get_height(Vector3(x, 0, zl)) - surface.get_surface_height_at(Vector3(x, 0, zl))
			# One print: its heel and forefoot dips (up to 15 cm apart, the raised waist between them) count once.
			if d > 0.0015:
				lowered += 1
				if gap > 15:
					count += 1
				gap = 0
			else:
				gap += 1
			x += 0.01
		dips.append(Vector2(count, lowered / 280.0))
	var prints: float = dips[0].x + dips[1].x
	_check("a footless walker prints boot soles, step by step (WorldWalkerSteps)", prints >= 3 and prints <= 5 and dips[0].x >= 1 and dips[1].x >= 1, "%d prints left, %d right over 2.8 m (a 1.7 m walker steps about 0.7 m)" % [dips[0].x, dips[1].x])
	_check("not its capsule: no trench along its path", dips[2].y < 0.5, "%.0f%% of its centre line lowered" % (dips[2].y * 100.0))
	w.queue_free()


# The point up the beach (x = 0.5) where the ground stands p_above over the level.
func _fringe_point(level: float, above: float) -> Vector3:
	var z := 0.0
	while z < 8.0 and height.get_height(Vector3(0.5, 0, z)) < level + above:
		z += 0.01
	return Vector3(0.5, 0, z)


# A wet margin round still water (GroundSurface3D.water_fringe): capillary rise keeps the sand near saturated a few
# cm over the level, fading out by 15 cm (fine sand, Lu and Likos 2004), and dry above.
func _run_fringe_tests() -> void:
	await _setup("beach", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	var pool := Node3D.new()
	pool.set_script(load("res://water_level_fake.gd"))
	add_child(pool)
	pool.level = 0.1
	surface.water_sources = [surface.get_path_to(pool)]
	Engine.time_scale = 10.0
	for i in 120:
		await get_tree().physics_frame
	Engine.time_scale = 1.0
	var s2 := surface.get_saturation_at(_fringe_point(0.1, 0.03))
	var s10 := surface.get_saturation_at(_fringe_point(0.1, 0.10))
	var s30 := surface.get_saturation_at(_fringe_point(0.1, 0.30))
	_check("a wet margin over still water's level (capillary rise)", s2 > 0.8 and s10 > 0.4 and s10 < s2 and s30 < 0.05, "saturation %.2f 3 cm over the level, %.2f at 10 cm, %.2f at 30 cm" % [s2, s10, s30])
	# No climate: no salt. A desert's (desert_climate.gd): a pale crust at the fringe's top, none at the water's edge
	# or well above it (GroundSurface3D.salt_crusts).
	var c0 := surface.get_salt_crust_at(_fringe_point(0.1, 0.18))
	var wm := WeatherMap3D.new()
	wm.climate = load("res://desert_climate.gd").new()
	get_tree().root.add_child(wm)
	Engine.time_scale = 10.0
	for i in 240:
		await get_tree().physics_frame
	Engine.time_scale = 1.0
	var c3 := surface.get_salt_crust_at(_fringe_point(0.1, 0.03))
	var c18 := surface.get_salt_crust_at(_fringe_point(0.1, 0.18))
	var c50 := surface.get_salt_crust_at(_fringe_point(0.1, 0.50))
	_check("a salt crust at the fringe's top in a desert only", c0 < 0.01 and c18 > 0.6 and c3 < 0.05 and c50 < 0.05, "crust %.2f at 18 cm without a climate; in a desert %.2f at 3 cm, %.2f at 18 cm, %.2f at 50 cm" % [c0, c3, c18, c50])
	wm.queue_free()


# Wind fills a print from its upwind side (GroundSurface3D::_drift_prints()).
func _run_infill_tests() -> void:
	await _soil_scene("dry_sand")
	surface.wind_ripples = false
	surface.wind_override = Vector2(12.0, 0.0)
	for i in 10:
		await get_tree().physics_frame
	surface.clear_deformation()
	var h := _undisturbed(Vector3.ZERO)
	surface.press_box(Vector3(0, h, 0), Vector2(0.29, 0.11), 0.0, h - 0.03, 0.03)
	for i in 10:
		await get_tree().physics_frame
	surface.settle(2.0)
	var a := _infill_stats(_infill_profile())
	# 12 m/s: about 1.4 m of drift an hour; 15 minutes.
	surface.settle(900.0, 5.0)
	var b := _infill_stats(_infill_profile())
	var fu := 1.0 - b.x / maxf(a.x, 1e-6)
	var fd := 1.0 - b.y / maxf(a.y, 1e-6)
	print("  12 m/s, 15 min: upwind half %.1f -> %.1f mm, downwind half %.1f -> %.1f mm, deepest at %.0f -> %.0f cm" % [a.x * 1000.0, b.x * 1000.0, a.y * 1000.0, b.y * 1000.0, a.z * 100.0, b.z * 100.0])
	_check("the wind fills a print from its upwind side (aeolian infilling)", fu > fd + 0.15 and fu > 0.25, "upwind half %.0f%% filled, downwind half %.0f%%" % [fu * 100.0, fd * 100.0])
	_check("the hollow creeps downwind as it fills", b.z > a.z + 0.02, "deepest point %.0f -> %.0f cm" % [a.z * 100.0, b.z * 100.0])
	surface.clear_deformation()
	surface.wind_override = Vector2(4.0, 0.0)
	surface.press_box(Vector3(0, h, 0), Vector2(0.29, 0.11), 0.0, h - 0.03, 0.03)
	for i in 10:
		await get_tree().physics_frame
	surface.settle(2.0)
	var c := _infill_stats(_infill_profile())
	surface.settle(900.0, 5.0)
	var d := _infill_stats(_infill_profile())
	_check("under the threshold the print stays", absf(d.x - c.x) < 1e-4 and absf(d.y - c.y) < 1e-4, "halves moved %.2f and %.2f mm" % [(d.x - c.x) * 1000.0, (d.y - c.y) * 1000.0])


# --view=avalanche: a walker's line of prints across the slip face, each with the tongue of sand it set off running
# down to the toe, seen from below and to the side under a low sun. --cam=x,dy,z,tx,dy,tz as in the other views.
# --view=fringe: the shore of a still pool in a desert (desert_climate.gd): the wet margin over the water's level and
# the salt crust at its top (water_fringe, salt_crusts).
func _build_fringe() -> void:
	await _setup("beach", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	var mat := _ground_material("dry_sand")
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-35.0, 150.0, 0.0)
	var wm := WeatherMap3D.new()
	wm.climate = load("res://desert_climate.gd").new()
	add_child(wm)
	var pool := Node3D.new()
	pool.set_script(load("res://water_level_fake.gd"))
	add_child(pool)
	pool.level = 0.1
	surface.water_sources = [surface.get_path_to(pool)]
	Engine.time_scale = 10.0
	for i in 240:
		await get_tree().physics_frame
	Engine.time_scale = 1.0
	get_viewport().use_taa = taa
	if adhesion:
		# --adhesion: wind over the damp margin (sped up) until the adhesion warts have grown, then still air.
		surface.wind_ripples = true
		var t0 := Time.get_ticks_msec()
		while not surface.is_ripples_ready() and Time.get_ticks_msec() - t0 < 60000:
			await get_tree().physics_frame
		surface.wind_override = Vector2(wind_arg, 0.0)
		surface.ripple_time_scale = 600.0
		for i in 600:
			await get_tree().physics_frame
			if surface.get_adhesion_growth() >= 1.0:
				break
		surface.wind_override = Vector2(0.5, 0.0)
		for i in 20:
			await get_tree().physics_frame
		print("adhesion warts: growth %.2f" % surface.get_adhesion_growth())
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([3.5, 1.4, 6.5, -0.5, 0.0, 2.4])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	for i in 10:
		await get_tree().physics_frame
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


func _build_avalanche() -> void:
	await _setup("dune", GroundSurface3D.GROUND_SAND, 0.0, 0.0)
	_apply("dry_sand")
	var mat := _ground_material("dry_sand")
	surface.material = mat
	_add_terrain(96.0, 192, mat)
	var env := _make_env()
	for c in get_children():
		if c is DirectionalLight3D:
			c.rotation_degrees = Vector3(sun_arg[0], sun_arg[1], 0.0) if sun_arg.size() >= 2 else Vector3(-25.0, 140.0, 0.0)
	get_node("Probe").position = Vector3(2.0, height.get_height(Vector3(2.0, 0, 0)), 0.0)
	for i in 10:
		await get_tree().physics_frame
	# Footsteps traversing the face, a little downhill, left and right feet 18 cm apart across the walk, a stride of 70 cm.
	var rng := RandomNumberGenerator.new()
	rng.seed = 5
	for k in 7:
		var z := -2.1 + 0.7 * k
		var side := 0.09 if k % 2 == 0 else -0.09
		# Heading along the walk (0.08 m down the face a 0.7 m stride), each foot toed out about 6 degrees.
		var heading := atan2(0.08, 0.7) + (0.1 if k % 2 == 0 else -0.1) + rng.randf_range(-0.05, 0.05)
		_avalanche_print(Vector3(1.2 + 0.08 * k + side + rng.randf_range(-0.02, 0.02), 0, z + rng.randf_range(-0.04, 0.04)), heading)
		for i in 40:
			await get_tree().physics_frame
	for i in 360:
		await get_tree().physics_frame
	surface.settle(3.0)
	surface.sync_deformation()
	for i in 10:
		await get_tree().physics_frame
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([6.5, 1.6, 2.5, 1.8, 0.0, -0.3])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	for i in 10:
		await get_tree().physics_frame
	env.pathtracing_enabled = not raster
	await _save_frame()
	get_tree().quit()


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


# Light rain (3 mm/h) on a 15 degree bank whose upper half a fire left bare and water-repellent (DeBano 2000): the
# unburnt clay soaks it all up, the scar sheds it, and the runoff stops when the repellency is gone.
func _burn_runoff(p_repel: float) -> Vector2:
	var stub: WorldFire = load("res://burn_stub.gd").new()
	stub.repel = p_repel
	Engine.get_singleton("WorldContracts").set_fire(stub)
	await _setup("bank", GroundSurface3D.GROUND_SOIL, 0.25, 0.0)
	surface.runoff_channels = true
	surface.runoff_time_scale = 100.0
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 3.0
	wm.humidity = 0.9
	wm.temperature = 15.0
	add_child(wm)
	wm.snap_to_settings()
	var burnt := 0.0
	var green := 0.0
	for i in 360:
		await get_tree().physics_frame
	for i in 60:
		await get_tree().physics_frame
		burnt += surface.get_runoff_at(Vector3(1.0, 0, 0.3)).x / 60.0
		green += surface.get_runoff_at(Vector3(-1.0, 0, 0.3)).x / 60.0
	wm.queue_free()
	Engine.get_singleton("WorldContracts").set_fire(null)
	return Vector2(burnt, green)


func _run_burnscar_tests() -> void:
	var on: Vector2 = await _burn_runoff(1.0)
	var off: Vector2 = await _burn_runoff(0.0)
	print("  runoff sheet: repellent scar %.3f mm (unburnt below %.3f mm); scar gone %.3f mm" % [on.x * 1000.0, on.y * 1000.0, off.x * 1000.0])
	_check("water-repellent burnt soil sheds the rain (Robichaud 2000)", on.x > 0.0 and on.x > 2.0 * off.x, "%.3f mm against %.3f mm" % [on.x * 1000.0, off.x * 1000.0])


# Post-fire debris flow (after mud round 12): a burnt channel down a 25% valley side onto a 4% fan.
class DebrisValley extends WorldGround:
	func _height_at(p: Vector3) -> float:
		# A V-shaped channel along +z: 25% down to z = 120, then a 4% fan; banks rise 15% to each side.
		var z := p.z
		var along := -0.25 * minf(z, 120.0) - 0.04 * maxf(z - 120.0, 0.0)
		var bank := 0.15 * absf(p.x) if z < 120.0 else 0.02 * absf(p.x)
		return along + bank


func _debris_event(p_id: int, p_from: float, p_volume: float) -> Dictionary:
	var path := PackedVector3Array()
	var widths := PackedFloat32Array()
	var sev := PackedFloat32Array()
	var z := p_from
	while z <= 120.0:
		path.append(Vector3(0, 0, z))
		widths.append(4.0)
		sev.append(1.0)
		z += 5.0
	return {"id": p_id, "position": path[0], "volume": p_volume, "path": path, "widths": widths, "severity": sev}


func _run_debris_tests() -> void:
	await _soil_scene("soil")
	var stub := DebrisValley.new()
	WorldContracts.set_ground(stub)
	var flow = ClassDB.instantiate("GroundDebrisFlow3D")
	flow.listen = false
	flow.show_flow = false
	add_child(flow)
	var signalled := {"changed": 0, "finished": []}
	flow.debris_front_changed.connect(func(id: int) -> void:
		if id == 1:
			signalled["changed"] += 1)
	flow.debris_flow_finished.connect(func(id: int) -> void: signalled["finished"].append(id))
	flow.start(_debris_event(1, 0.0, 2000.0))
	# A second gully's small event joins it 20 m down.
	flow.start(_debris_event(2, 20.0, 150.0))
	_check("a small event whose outlet lies on a running flow's path feeds that flow", flow.get_body_count() == 1, "%d bodies" % flow.get_body_count())
	var peak_speed := 0.0
	var fronts := []
	var t := 0.0
	var moving_pos := false
	while t < 900.0 and flow.get_active_bodies() > 0:
		flow.advance(5.0, 0.05)
		t += 5.0
		var f: Dictionary = flow.get_debris_front(1)
		if f.has("position") and not f["done"]:
			moving_pos = true
		peak_speed = maxf(peak_speed, f.get("speed", 0.0))
		fronts.append(f.get("position", Vector3()))
	var f: Dictionary = flow.get_debris_front(1)
	var b: Dictionary = flow.get_budget(1)
	var stop_z: float = f.get("position", Vector3()).z
	print("  ran %.0f s: front stopped at z %.1f m (fan from 120 m), peak front speed %.1f m/s, %.1f m deep there, %.1f m wide" % [t, stop_z, peak_speed, f.get("depth", 0.0), f.get("width", 0.0)])
	print("  budget: %.0f m3 fed, %.0f m3 entrained, %.0f m3 off the end, %.0f m3 lying" % [b["in"], b["entrained"], b["out"], b["now"]])
	_check("the front runs down the channel at debris-flow speeds (Rickenmann 1999: a few to 10 m/s)", peak_speed > 2.0 and peak_speed < 15.0, "%.1f m/s" % peak_speed)
	_check("and stops on the fan, where the slope falls under the basal friction (about 10%)", stop_z > 120.0 and stop_z < 200.0 and f["done"], "z %.1f m, done %s" % [stop_z, f["done"]])
	_check("it bulks up along the burnt channel (McDougall and Hungr 2005)", b["entrained"] > 0.1 * b["in"], "%.0f of %.0f m3" % [b["entrained"], b["in"]])
	_check("no slurry is made or lost", absf(b["now"] + b["out"] - b["in"] - b["entrained"]) < 0.02 * b["in"], "%.0f + %.0f against %.0f + %.0f" % [b["now"], b["out"], b["in"], b["entrained"]])
	_check("PhysX can follow the front while it moves", moving_pos, "")
	_check("debris_front_changed fires as it moves, and both pooled events hear debris_flow_finished", signalled["changed"] > 10 and 1 in signalled["finished"] and 2 in signalled["finished"], "%d changes, finished %s" % [signalled["changed"], signalled["finished"]])
	var lying := 0.0
	for z in range(122, int(stop_z), 2):
		lying = maxf(lying, surface.get_mud_deposit_at(Vector3(0, 0, z)))
	_check("the fan is laid on the ground as fresh slurry", lying > 0.1, "%.2f m at most" % lying)
	WorldContracts.set_ground(null)
	flow.queue_free()


# The bench's own height source as the world's ground, for the debris view.
class HeightGround extends WorldGround:
	var src: Node3D

	func _height_at(p: Vector3) -> float:
		return src.get_height(p)


# --view=debris [--t=45]: a post-fire debris flow down a burnt channel spreading on its fan, --t seconds after it fires.
func _build_debris() -> void:
	await _setup("valley", GroundSurface3D.GROUND_SOIL, 0.5, 0.0)
	_apply("soil")
	var mat := _ground_material("soil")
	surface.material = mat
	_add_terrain(160.0, 320, mat)
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
	sun.rotation_degrees = Vector3(-35.0, 140.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 120.0
	add_child(sun)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([30.0, 45.0, -60.0, 0.0, 0.0, 5.0])
	# The print field sits where the camera looks (round 13: a fan up close is ground there).
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(c[3], 0, c[5])
	for i in 10:
		await get_tree().physics_frame
	var stub := HeightGround.new()
	stub.src = height
	WorldContracts.set_ground(stub)
	var flow = ClassDB.instantiate("GroundDebrisFlow3D")
	flow.listen = false
	add_child(flow)
	var path := PackedVector3Array()
	var widths := PackedFloat32Array()
	var sev := PackedFloat32Array()
	var z := -110.0
	while z <= 0.0:
		path.append(Vector3(0, 0, z))
		widths.append(4.0)
		sev.append(1.0)
		z += 5.0
	flow.start({"id": 1, "position": path[0], "volume": 2000.0, "path": path, "widths": widths, "severity": sev})
	var secs := 40.0
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--t="):
			secs = float(a.substr(4))
	flow.advance(secs, 0.05)
	if "--no-sheet" in OS.get_cmdline_user_args():
		flow.show_flow = false # the ground's own deposit alone
	var f: Dictionary = flow.get_debris_front(1)
	print("debris: front at %s, %.2f m deep, %.1f m/s, %.0f m3, done %s, budget %s" % [f.get("position", Vector3()), f["depth"], f["speed"], f["volume"], f["done"], flow.get_budget(1)])
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 55.0
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	await _save_view(env)
	WorldContracts.set_ground(null)
	get_tree().quit()


# Mud round 13.1: a debris fan up close. The flow comes down the "valley" channel onto its fan at z = 0, where the
# print field sits: levees along its edges, ruts and a log under the fan buried, a light crate shoved, the deposit soft.
func _run_fan_tests() -> void:
	await _setup("valley", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	_apply("soil")
	surface.near_field = false
	surface.mats = true
	_add_terrain(160.0, 320, _ground_material("soil"))
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(0, 0, 12)
	for i in 10:
		await get_tree().physics_frame
	# Ruts across the fan and a log laid in them.
	for z in range(6, 18):
		var p := Vector3(0.8, 0, z)
		p.y = surface.get_surface_height_at(p)
		surface.press_wheel(p, Vector2(0.25, 0.6), 0.0, 0.1)
	var log := _make_log(Vector3(3.0, surface.get_surface_height_at(Vector3(3, 0, 14)) + 0.12, 14.0), 0.07, 2.4)
	log.rotation_degrees.y = 90.0
	var crate := RigidBody3D.new()
	crate.mass = 150.0
	crate.add_to_group("world_contact")
	var cs := CollisionShape3D.new()
	cs.shape = BoxShape3D.new()
	(cs.shape as BoxShape3D).size = Vector3(0.8, 0.8, 0.8)
	crate.add_child(cs)
	add_child(crate)
	crate.global_position = Vector3(-1.5, surface.get_surface_height_at(Vector3(-1.5, 0, 4)) + 0.45, 4.0)
	for i in 120:
		await get_tree().physics_frame
	var rut_at := Vector3(0.8, 0, 10)
	var rut_before := surface.get_surface_height_at(rut_at)
	var crate_from := crate.global_position
	var mats_before := surface.get_mat_count()
	var bare := surface.get_mud_depth_at(rut_at + Vector3(0, 0, 2))
	var stub := HeightGround.new()
	stub.src = height
	WorldContracts.set_ground(stub)
	var flow = ClassDB.instantiate("GroundDebrisFlow3D")
	flow.listen = false
	flow.show_flow = false
	add_child(flow)
	var path := PackedVector3Array()
	var widths := PackedFloat32Array()
	var sev := PackedFloat32Array()
	var z := -110.0
	while z <= 0.0:
		path.append(Vector3(0, 0, z))
		widths.append(4.0)
		sev.append(1.0)
		z += 5.0
	flow.start({"id": 7, "position": path[0], "volume": 2000.0, "path": path, "widths": widths, "severity": sev})
	var t := 0.0
	var crate_peak := 0.0
	while t < 400.0 and flow.get_active_bodies() > 0:
		flow.advance(0.5, 0.05)
		t += 0.5
		await get_tree().physics_frame
		crate_peak = maxf(crate_peak, crate.linear_velocity.length())
	for i in 30:
		await get_tree().physics_frame
	# Levees: across the channel 40 to 70 m up from the fan, the deposit's edges stand above its middle.
	var ridge := 0.0
	var middle := 0.0
	for zz in range(-70, -40, 5):
		var row := []
		for xx in range(-14, 15):
			row.append(flow.get_deposit_depth_at(Vector3(xx, 0, zz)))
		middle += row[14] / 6.0
		var best := 0.0
		for k in row.size():
			if absi(k - 14) >= 2:
				best = maxf(best, row[k])
		ridge += best / 6.0
	print("  ran %.0f s; channel deposit: levee crests %.2f m, middle %.2f m" % [t, ridge, middle])
	_check("the flow leaves levees along its edges, higher than the channel's middle (Johnson et al. 2012)", ridge > 0.15 and ridge > 1.5 * middle, "%.2f against %.2f m" % [ridge, middle])
	var lay: float = surface.get_mud_deposit_at(rut_at)
	var rut_after := surface.get_surface_height_at(rut_at)
	print("  rut at %s: %.2f m of deposit, ground %.3f -> %.3f m" % [rut_at, lay, rut_before, rut_after])
	_check("in the print field the deposit is ground: the rut under it fills and the surface rises", lay > 0.1 and rut_after - rut_before > 0.5 * minf(lay, 0.5), "rose %.2f m under %.2f m" % [rut_after - rut_before, lay])
	var buried := false
	for k in surface.get_mat_count():
		buried = buried or surface.is_mat_buried(k)
	_check("a log under a deposit deeper than it is thick is buried", mats_before > 0 and buried == (surface.get_mud_deposit_at(Vector3(3, 0, 14)) >= 0.14), "%d mats, deposit %.2f m there, buried %s" % [mats_before, surface.get_mud_deposit_at(Vector3(3, 0, 14)), buried])
	var moved := crate.global_position.distance_to(crate_from)
	_check("the flow shoves a crate standing in its way", moved > 1.0 and crate_peak > 1.0, "%.1f m (peak %.1f m/s)" % [moved, crate_peak])
	var soft := surface.get_mud_depth_at(rut_at + Vector3(0, 0, 2))
	_check("the fresh deposit is a soft mud layer for wheels (saturated, whatever the weather)", soft > 0.3 and soft > bare + 0.3, "%.2f m of mud against %.2f m before" % [soft, bare])
	WorldContracts.set_ground(null)
	flow.queue_free()


# A stream 35 cm deep over the "ford" bed (water surface at -0.25 m wherever the bed lies under it), running +z at
# 0.8 m/s; it keeps the fines dropped into it.
class FordStream extends WorldWater:
	var kg := 0.0

	func _height_at(p: Vector3) -> float:
		return -0.25 if p.x < 0.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(0, 0, 0.8) if p.x < 0.0 else Vector3.ZERO

	func _add_sediment_mass(_p: Vector3, mineral: float, ash: float) -> void:
		kg += mineral + ash


# Mud round 13.2: stream fords. A wheel crossing the bed (in the current) and the same wheel on the dry flat; then a
# wheel climbing the saturated bank, and the same bank drier.
func _ford_rut(p: Vector3) -> float:
	var deepest := 0.0
	for k in range(-6, 7):
		var q := p + Vector3(0, 0, k * 0.25)
		deepest = maxf(deepest, surface.get_dent_at(q) - surface.get_rim_at(q))
	return deepest


func _ford_rim(p: Vector3) -> float:
	var high := 0.0
	for k in range(-6, 7):
		for dx in [-0.35, -0.25, 0.25, 0.35]:
			var q := p + Vector3(dx, 0, k * 0.25)
			high = maxf(high, surface.get_rim_at(q) - surface.get_dent_at(q))
	return high


func _ford_drive(x: float, frames: int) -> void:
	for i in frames:
		var p := Vector3(x, 0, -2.0 + 4.0 * float(i) / float(frames))
		p.y = surface.get_surface_height_at(p)
		surface.press_wheel(p, Vector2(0.3, 0.35), 0.0, 0.06, Vector2(0, 2.0))
		await get_tree().physics_frame


func _run_ford_tests() -> void:
	await _setup("ford", GroundSurface3D.GROUND_SOIL, 0.6, 0.0)
	_apply("soil")
	surface.near_field = false
	var stream := FordStream.new()
	WorldContracts.add_water_stub(stream)
	var probe := get_node("Probe") as Node3D
	probe.global_position = Vector3(0, 0, 0)
	for i in 90:
		await get_tree().physics_frame
	await _ford_drive(-4.0, 60)
	await _ford_drive(4.0, 60)
	for i in 30:
		await get_tree().physics_frame
	surface.sync_deformation()
	var wet_rut := _ford_rut(Vector3(-4, 0, 0))
	var dry_rut := _ford_rut(Vector3(4, 0, 0))
	var st: Dictionary = surface.get_ford_stats()
	print("  ford: rut %.3f m in the stream, %.3f m on the flat; plume %.1f kg (stream holds %.1f), scour %.4f m3" % [wet_rut, dry_rut, st["plume_kg"], stream.kg, st["scour_m3"]])
	_check("a wheel in running water scours the bed deeper than on land: its spoil goes downstream", wet_rut > 1.15 * dry_rut, "%.3f against %.3f m" % [wet_rut, dry_rut])
	_check("the scoured fines go into the stream as a plume", stream.kg > 1.0 and absf(stream.kg - st["plume_kg"]) < 0.01 * st["plume_kg"] + 0.01, "%.1f kg" % stream.kg)
	# The current took the rims the wheel threw (loose spoil past its critical shear).
	var wet_rim := _ford_rim(Vector3(-4, 0, 0))
	var dry_rim := _ford_rim(Vector3(4, 0, 0))
	_check("the current takes the rims a wheel throws in the bed", wet_rim < 0.6 * dry_rim, "rims %.3f in the stream against %.3f m on land" % [wet_rim, dry_rim])
	# A saturated bank under a wheel climbing out.
	var b0: int = st["bank_failures"]
	var bank := Vector3(0.0, 0, 3.0)
	var slab := Vector3(0.3, 0, 3.0) # where the slab stood, just above the wheel
	var slab_before := surface.get_surface_height_at(slab)
	var kg0 := stream.kg
	bank.y = surface.get_surface_height_at(bank)
	for i in 3:
		surface.press_wheel(bank, Vector2(0.3, 0.35), PI / 2.0, 0.06, Vector2(-1.0, 0))
		await get_tree().physics_frame
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	st = surface.get_ford_stats()
	var after := surface.get_surface_height_at(slab)
	var toe: float = maxf(surface.get_mud_deposit_at(Vector3(-0.6, 0, 3.0)), surface.get_mud_deposit_at(Vector3(-1.4, 0, 3.0)))
	print("  bank: failures %d, slab ground %.2f -> %.2f m, toe deposit %.2f m, plume +%.1f kg" % [st["bank_failures"] - b0, slab_before, after, toe, stream.kg - kg0])
	_check("a wheel on a saturated bank breaks it down into the stream (Skempton and DeLory's slab, undrained load)", st["bank_failures"] > b0 and after < slab_before - 0.1 and toe > 0.02 and stream.kg > kg0, "%d failures, %.2f m lower, toe %.2f m" % [st["bank_failures"] - b0, slab_before - after, toe])
	# Higher up, above the water table and its fringe, the bank holds.
	var b1: int = st["bank_failures"]
	var high := Vector3(0.9, 0, -4.0)
	high.y = surface.get_surface_height_at(high)
	surface.press_wheel(high, Vector2(0.3, 0.35), PI / 2.0, 0.06, Vector2(-1.0, 0))
	await get_tree().physics_frame
	st = surface.get_ford_stats()
	_check("higher up, above the water table, the bank holds under the same wheel", st["bank_failures"] == b1, "%d failures" % (st["bank_failures"] - b1))
	# Wakes wash the bank (round 22.1): a hull 1.8 m wide driven along the stream 3.6 m off the waterline. At a
	# walking pace its waves barely reach the bank; at 2 m/s they break on it (Havelock's y^-1/3 decay from v^2 / g),
	# wet the strip they run up and, over a day of steady traffic past one stretch (sped up), wear it into the stream.
	var wash := {}
	for speed in [0.5, 2.0]:
		st = surface.get_ford_stats()
		var kg_w0: float = st["wake_wash_kg"]
		var m3_w0: float = st["wake_water_m3"]
		var kg_s0 := stream.kg
		var hb := 0.0
		for i in 60:
			surface.wash_wake(Vector3(-4.0, -0.6, -6.0), Vector3(0, 0, speed), 0.9, -0.25, 1440.0)
			hb = maxf(hb, surface.get_ford_stats()["wake_height"])
			if i % 6 == 0:
				await get_tree().physics_frame
		st = surface.get_ford_stats()
		wash[speed] = [hb, st["wake_wash_kg"] - kg_w0, st["wake_water_m3"] - m3_w0, stream.kg - kg_s0]
		print("  wake at %.1f m/s: %.2f m at the bank, washed %.2f kg into the stream (it took %.2f kg), swash %.3f m3" % [speed, hb, wash[speed][1], wash[speed][3], wash[speed][2]])
	_check("a wading hull's waves break on the bank, higher the faster it goes", wash[2.0][0] > 0.1 and wash[2.0][0] > 4.0 * wash[0.5][0], "%.2f m at 2 m/s, %.3f m at 0.5 m/s" % [wash[2.0][0], wash[0.5][0]])
	_check("and at speed wash the bank into the stream and wet it; a walking pace does not", wash[2.0][1] > 0.1 and wash[2.0][3] > 0.0 and wash[2.0][2] > wash[0.5][2] and wash[0.5][1] == 0.0, "%.2f kg at 2 m/s, %.2f kg at 0.5 m/s" % [wash[2.0][1], wash[0.5][1]])
	WorldContracts.clear_overrides()


# Mud round 13.3: off-camber sliding. A 4x4 driven across a 20 degree side slope, in wet mud and on drier soil: in the
# mud the soil's shear limit cannot hold the slope's pull, the car slides downhill, and its lower wheels, carrying more
# of the load and sliding sideways, dig in deeper than the upper ones.
func _camber_run(wet: bool) -> Array:
	await _setup("camber", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply("soil")
	surface.near_field = false
	_add_terrain(80.0, 160, _ground_material("soil"))
	if wet:
		await _paint_mud(0.4, STIFF_MUD)
	var at := Vector3(0, 0, -6)
	var car := _make_car(1500.0, 700.0, Vector3(at.x, height.get_height(at) + 0.9, at.z))
	surface.follow_target = surface.get_path_to(car)
	for i in 40:
		await get_tree().physics_frame
	var start := car.global_position
	car.throttle = 0.4
	var prev_x := start.x
	var low := 0.0
	var high := 0.0
	var n := 0
	for i in 300:
		await get_tree().physics_frame
		# While it slides downhill (over 0.3 m/s): lower and upper by where the wheels are now (it may swing).
		var vx := (car.global_position.x - prev_x) * 60.0
		prev_x = car.global_position.x
		if vx < -0.3:
			var ws := []
			for c in car.get_children():
				if c is PhysXVehicleWheel3D:
					ws.append(c)
			var order := [0, 1, 2, 3]
			order.sort_custom(func(a, b): return (ws[a] as Node3D).global_position.y < (ws[b] as Node3D).global_position.y)
			low += car.get_wheel_sinkage(order[0]) + car.get_wheel_sinkage(order[1])
			high += car.get_wheel_sinkage(order[2]) + car.get_wheel_sinkage(order[3])
			n += 1
	car.throttle = 0.0
	var d := car.global_position - start
	var res := [d.x, Vector2(d.x, d.z).length(), low / maxi(2 * n, 1), high / maxi(2 * n, 1)]
	car.queue_free()
	return res


func _run_camber_tests() -> void:
	await _soil_scene("soil")
	await _paint_mud(0.4, STIFF_MUD)
	var o := Vector3.ZERO
	var roll: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.0)
	var side: Dictionary = surface.get_wheel_soil(o, 3500.0, 0.35, 0.26, 0.0, 0.0, 0.35)
	_check("a wheel sliding sideways digs in deeper than one rolling (Ishigami et al. 2007)", side["sinkage"] > 1.2 * roll["sinkage"], "%.1f against %.1f cm at 20 degrees of slip" % [side["sinkage"] * 100.0, roll["sinkage"] * 100.0])
	if not ClassDB.class_exists("PhysXVehicle3D"):
		print("(no PhysX: vehicle checks skipped)")
		return
	var w := await _camber_run(true)
	var dr := await _camber_run(false)
	print("  wet mud: %.2f m sideways over %.1f m; sliding, lower wheels %.1f cm, upper %.1f cm" % [w[0], w[1], w[2] * 100.0, w[3] * 100.0])
	print("  drier soil: %.2f m sideways over %.1f m, lower wheels %.1f cm, upper %.1f cm" % [dr[0], dr[1], dr[2] * 100.0, dr[3] * 100.0])
	_check("across a muddy side slope the car slides downhill", w[0] < -0.5 and w[0] < 2.0 * dr[0] - 0.2, "%.2f m against %.2f m on drier soil" % [w[0], dr[0]])
	_check("sliding, its lower wheels dig in deeper than the upper ones", w[2] > 1.2 * w[3] and w[2] > 0.02, "%.1f against %.1f cm" % [w[2] * 100.0, w[3] * 100.0])


# Mud round 13.3 view: the car driven across the 20 degree side slope in stiff mud, sliding down it; the camera
# downhill of the track, looking up at the car and the sideways trenches its lower wheels dug.
func _build_camber() -> void:
	kind = "soil"
	await _setup("camber", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
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
	sun.rotation_degrees = Vector3(-28.0, 120.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	await _paint_mud(0.4, STIFF_MUD)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	var at := Vector3(0, 0, -6)
	var car := _make_car(1500.0, 700.0, Vector3(at.x, height.get_height(at) + 0.9, at.z))
	_dress_car(car)
	surface.follow_target = surface.get_path_to(car)
	for i in 40:
		await get_tree().physics_frame
	car.throttle = 0.4
	var wframes := 300
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--wframes="):
			wframes = int(a.substr(10))
	for i in wframes:
		await get_tree().physics_frame
	car.throttle = 0.0
	car.brake = 1.0
	for i in 30:
		await get_tree().physics_frame
	print("camber: car at %s" % car.global_position)
	var cam := Camera3D.new()
	add_child(cam)
	var p := car.global_position
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([p.x - 6.0, 2.2, p.z - 4.0, p.x + 0.5, 0.3, p.z - 1.5])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# A river's silt record for the silt view: a lobe of the flood's leavings, thickest in its middle and
# thinning out to a wandering margin; "fresh" adds a newer band over its far end.
class SiltViewStub extends WorldWater:
	var fresh := false

	func _height_at(p: Vector3) -> float:
		return -INF

	func _sediment_deposit_at(p: Vector3) -> Vector2:
		# A lobe some 9 m long and 6 m wide, its margin wandering.
		var q := Vector2(p.x + 1.0, p.z * 1.4)
		var ang := atan2(q.y, q.x)
		var rim := 4.2 + 0.6 * sin(ang * 3.0 + 0.7) + 0.35 * sin(ang * 7.0 + 2.1) + 0.2 * sin(ang * 13.0)
		var t := clampf((rim - q.length()) / 3.0, 0.0, 1.0)
		var kg := 11.0 * t # up to about 8 mm
		if fresh:
			var e2 := -2.6 + 0.5 * sin(p.z * 1.3 + 2.0) + 0.25 * sin(p.z * 3.7)
			kg += 4.0 * clampf((e2 - p.x) / 1.5, 0.0, 1.0) * clampf(t * 3.0, 0.0, 1.0)
		return Vector2(kg * 0.9, kg * 0.1) if kg > 0.0 else Vector2.ZERO


# The silt view (mud round 15.1): an older skin dried pale and cracked, a newer one still dark and wet over it.
func _build_silt() -> void:
	kind = "soil"
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	surface.flood_silt = true
	surface.desiccation_cracks = true
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
	sun.rotation_degrees = Vector3(-35.0, 140.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 0.5
	wm.temperature = 20.0
	add_child(wm)
	wm.snap_to_settings()
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	WorldContracts.clear_overrides()
	var stub := SiltViewStub.new()
	WorldContracts.add_water_stub(stub)
	for i in 60:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1e5
	for i in 600:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	if not "--dry" in OS.get_cmdline_user_args():
		stub.fresh = true
		for i in 60:
			await get_tree().physics_frame
	WorldContracts.clear_overrides()
	for i in 200: # the coloured deposits are re-baked every 5 s
		await get_tree().physics_frame
	print("silt: %.1f mm dry at (-3, 0), %s fresh at (-4, 0)" % [surface.get_silt_at(Vector3(-3, 0, 0)).x * 1000.0, surface.get_silt_at(Vector3(-4, 0, 0))])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([3.2, 1.5, 2.2, -1.2, 0.0, -0.3])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# --view=stones (mud round 17): a tyre driven through soft mud over hidden stones knocks the uncovered ones loose and
# shoves them on: they plough furrows with heaps ahead and settle into the mud where they stop.
func _build_stones() -> void:
	kind = "soil"
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply(kind)
	var mat := _ground_material(kind)
	surface.material = mat
	surface.buried_obstacles = true
	surface.buried_density = 0.6
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
	sun.rotation_degrees = Vector3(-30.0, 120.0, 0.0) if sun_arg.size() < 2 else Vector3(sun_arg[0], sun_arg[1], 0.0)
	sun.directional_shadow_max_distance = 30.0
	add_child(sun)
	var full := get_window().size
	get_window().size = Vector2i(160, 90)
	await _paint_mud(0.8, mud_arg[1] if mud_arg.size() == 2 else 0.7)
	for i in 10:
		await get_tree().physics_frame
	# Two tyres a track apart, 3 m/s, deep enough to reach the stones.
	var x := -5.0
	while x < 5.0:
		for zt in [-0.8, 0.8]:
			surface.press_wheel(Vector3(x, 0, zt), Vector2(0.8, 0.5), 0.0, 0.32, Vector2(3.0, 0.0))
		x += 0.05
		await get_tree().physics_frame
	for i in 180:
		await get_tree().physics_frame
	var loose := get_tree().get_nodes_in_group("ground_loose")
	var line := ""
	for b in loose:
		line += " %s" % str((b as Node3D).global_position)
	print("stones: %d loose:%s" % [loose.size(), line])
	var cam := Camera3D.new()
	add_child(cam)
	var c := cam_arg if cam_arg.size() == 6 else PackedFloat32Array([5.5, 1.6, 2.4, 1.0, 0.0, 0.0])
	cam.position = Vector3(c[0], _undisturbed(Vector3(c[0], 0, c[2])) + c[1], c[2])
	cam.look_at(Vector3(c[3], _undisturbed(Vector3(c[3], 0, c[5])) + c[4], c[5]))
	cam.fov = 50.0
	surface.follow_target = surface.get_path_to(cam)
	for i in 20:
		await get_tree().physics_frame
	surface.sync_deformation()
	get_window().size = full
	await _save_view(env)
	get_tree().quit()


# Waves in liquid mud (mud round 14.1): a rut pressed into 80 cm of slurry, with the waves (a shallow Bingham flow)
# and with slumping alone. The waves carry what the press shoved aside further and close the rut faster, keep the
# volume, stop where the yield stress holds the surface, and leave stiff mud alone.
func _run_mudwave_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	var o := Vector3(0.3, 0, 0.2)
	var far := o + Vector3(0.75, 0, 0)
	var res := {}
	for k in ["waves", "slump", "stiff_waves", "stiff_slump"]:
		surface.mud_waves = k.ends_with("waves")
		await _paint_mud(0.8, STIFF_MUD if k.begins_with("stiff") else 1.0)
		surface.clear_deformation()
		for i in 3:
			await get_tree().physics_frame
		surface.press_box(o, Vector2(0.3, 1.5), 0.0, -0.25, 0.25)
		surface.sync_deformation()
		var v0 := surface.get_moved_volume()
		var d0 := -surface.get_surface_height_at(o)
		surface.settle(1.0, 1.0 / 60.0)
		var d1 := -surface.get_surface_height_at(o)
		var f1 := surface.get_surface_height_at(far)
		surface.settle(30.0, 0.05)
		var v2 := surface.get_moved_volume()
		var d2 := -surface.get_surface_height_at(o)
		var s2 := _max_slope(o, 0.5)
		res[k] = {"d0": d0, "d1": d1, "d2": d2, "far": f1, "net0": v0.x - v0.y, "net2": v2.x - v2.y, "slope": s2}
		print("  %s: rut %.1f cm, %.1f after 1 s, %.1f after 31 s; %.1f mm at 75 cm out after 1 s; steepest %.0f deg; net %.2f to %.2f l" % [k, d0 * 100.0, d1 * 100.0, d2 * 100.0, f1 * 1000.0, s2, (v0.x - v0.y) * 1000.0, (v2.x - v2.y) * 1000.0])
	# A wheel driven 3 m through the slurry at 2 m/s, 30 cm deep: the surface behind it after half a second, its
	# roughness the worst step of a texel off the mean of its four neighbours (ripples and spikes show there).
	var rough := {}
	for k in ["waves", "slump"]:
		surface.mud_waves = k == "waves"
		await _paint_mud(0.8, 1.0)
		surface.clear_deformation()
		for i in 3:
			await get_tree().physics_frame
		var p := Vector3(-1.5, 0, 0)
		for i in 90:
			surface.press_wheel(p, Vector2(0.25, 0.3), 0.0, 0.3, Vector2(2.0, 0.0))
			p.x += 2.0 / 60.0
			surface.settle(1.0 / 60.0, 1.0 / 60.0)
		surface.settle(0.5, 1.0 / 60.0)
		var t := surface.deform_extent / surface.deform_resolution
		var worst := 0.0
		var x := -1.5
		while x <= 1.5:
			var z := -0.6
			while z <= 0.6:
				var q := Vector3(x, 0, z)
				var mean := 0.25 * (surface.get_surface_height_at(q + Vector3(t, 0, 0)) + surface.get_surface_height_at(q - Vector3(t, 0, 0)) + surface.get_surface_height_at(q + Vector3(0, 0, t)) + surface.get_surface_height_at(q - Vector3(0, 0, t)))
				worst = maxf(worst, absf(surface.get_surface_height_at(q) - mean))
				z += t
			x += t
		rough[k] = [worst, -surface.get_surface_height_at(Vector3(0, 0, 0))]
		if "--profile" in OS.get_cmdline_user_args():
			var line := ""
			var zz := -0.6
			while zz <= 0.6:
				line += " %.0f" % (surface.get_surface_height_at(Vector3(0, 0, zz)) * 1000.0)
				zz += t
			print("  z profile mm:" + line)
			line = ""
			var xx := -1.0
			while xx <= 1.0:
				line += " %.0f" % (surface.get_surface_height_at(Vector3(xx, 0, 0.2)) * 1000.0)
				xx += t
			print("  x profile at z 0.2 mm:" + line)
		print("  %s: worst %.1f mm off its neighbours behind the wheel, rut %.1f cm" % [k, worst * 1000.0, rough[k][1] * 100.0])
	_check("a wheel's wake in slurry is smooth, no spikes", rough["waves"][0] < 1.5 * rough["slump"][0] + 0.002,
			"%.1f mm (%.1f slumping)" % [rough["waves"][0] * 1000.0, rough["slump"][0] * 1000.0])
	surface.mud_waves = true
	var w: Dictionary = res["waves"]
	var s: Dictionary = res["slump"]
	_check("liquid mud closes a rut faster with waves than by slumping alone", w["d1"] < 0.8 * s["d1"] and w["d1"] < 0.7 * w["d0"],
			"%.1f cm deep after 1 s (%.1f slumping, %.1f pressed)" % [w["d1"] * 100.0, s["d1"] * 100.0, w["d0"] * 100.0])
	# What the press squeezed out (the compaction's 3 l) the whole layer pays back: the surface 75 cm out drops as the
	# mud runs in, where slumping never reaches.
	_check("the wave reaches further than slumping", absf(w["far"]) > 0.001 and absf(w["far"]) > 3.0 * absf(s["far"]),
			"%.1f mm at 75 cm out (%.1f mm slumping)" % [w["far"] * 1000.0, s["far"] * 1000.0])
	_check("the waves keep the volume", absf(w["net2"] - w["net0"]) < 0.02 * absf(w["net0"]) + 2e-4,
			"net %.2f l pressed, %.2f l after 31 s" % [w["net0"] * 1000.0, w["net2"] * 1000.0])
	_check("the yield stress stops it: the surface settles nearly flat, not rippling", w["slope"] < 15.0 and w["d2"] < 0.3 * w["d0"],
			"steepest %.0f degrees, %.1f cm deep" % [w["slope"], w["d2"] * 100.0])
	var a: Dictionary = res["stiff_waves"]
	var b: Dictionary = res["stiff_slump"]
	_check("stiff mud does not wave", absf(a["d2"] - b["d2"]) < 0.002 and absf(a["far"] - b["far"]) < 0.001,
			"%.1f and %.1f cm deep" % [a["d2"] * 100.0, b["d2"] * 100.0])
	near_on = true


# Worn-in tracks (mud round 14.2): wheel passes counted per 25 cm cell, once while a wheel stays on it; the passes pack
# the soil (a loaded wheel sinks less on a used track) and halve over wear_regrowth_days.
func _run_wear_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	surface.worn_tracks = true
	surface.clear_wear()
	var c4 := GroundSurface3D.wear_compaction(4.0)
	_check("most packing comes in the first passes", c4 > 0.55 and c4 < 0.7 and GroundSurface3D.wear_compaction(20.0) > 0.98, "%.2f packed after 4 passes" % c4)
	var o := Vector3(0.5, 0, 0.3)
	# A wheel standing on the cell for a second, then gone half a second, then back: two passes.
	for i in 60:
		surface.press_wheel(o, Vector2(0.25, 0.3), 0.0, 0.02, Vector2())
		await get_tree().physics_frame
	var one := surface.get_wear_at(o)
	for i in 30:
		await get_tree().physics_frame
	for i in 10:
		surface.press_wheel(o, Vector2(0.25, 0.3), 0.0, 0.02, Vector2())
		await get_tree().physics_frame
	var two := surface.get_wear_at(o)
	var beside := surface.get_wear_at(o + Vector3(0.6, 0, 0))
	_check("a wheel resting counts one pass, coming back a second", absf(one - 1.0) < 0.05 and absf(two - 2.0) < 0.05 and beside < 0.01, "%.2f, then %.2f; %.2f beside" % [one, two, beside])
	# A loaded wheel (3 kN, 70 cm tyre at 200 kPa) on fresh soil and on a track of 10 passes.
	var q := Vector3(-1.5, 0, -1.0)
	var s0: float = surface.get_wheel_soil(q, 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	surface.add_wear(q, Vector2(1.0, 1.0), 0.0, 10.0)
	var s1: float = surface.get_wheel_soil(q, 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	_check("a used track bears a wheel better", s1 < 0.7 * s0 and s1 > 0.0, "%.1f mm on the track, %.1f mm fresh" % [s1 * 1000.0, s0 * 1000.0])
	# Regrowth: 30 days (sped up) halve the passes.
	var w0 := surface.get_wear_at(q)
	surface.runoff_time_scale = 30.0 * 86400.0 / 4.0
	for i in 245: # just over 4 s
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	var w1 := surface.get_wear_at(q)
	_check("the passes halve over the regrowth time", w1 > 0.35 * w0 and w1 < 0.65 * w0, "%.1f to %.1f passes" % [w0, w1])
	# Kept with the ruts (round 15.3): saved, cleared, read back as left; read back after weeks away, regrown.
	var dir := OS.get_user_data_dir().path_join("wear_keep_test")
	var was_keeping := surface.keep_ruts
	surface.keep_ruts = true
	surface.ruts_path = dir
	surface.clear_saved_ruts()
	var k := Vector3(2.0, 0, 1.5)
	surface.add_wear(k, Vector2(0.5, 0.5), 0.0, 8.0)
	var kept0 := surface.get_wear_at(k)
	surface.save_ruts()
	surface.clear_wear()
	surface.ruts_path = ""
	surface.ruts_time_scale = 0.0
	surface.ruts_path = dir
	var back := surface.get_wear_at(k)
	surface.clear_wear()
	surface.ruts_path = ""
	surface.ruts_time_scale = 1e13 # any time away (milliseconds here) is years
	surface.ruts_path = dir
	var aged := surface.get_wear_at(k)
	surface.ruts_time_scale = 1.0
	surface.clear_saved_ruts()
	surface.ruts_path = ""
	surface.keep_ruts = was_keeping
	_check("worn tracks are kept with the ruts, regrown for the time away", kept0 > 7.0 and absf(back - kept0) < 0.01 and aged < 0.05, "%.1f passes saved, %.1f read back, %.2f after years" % [kept0, back, aged])
	surface.worn_tracks = false
	var s2: float = surface.get_wheel_soil(q, 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	_check("off, the track is fresh soil again", absf(s2 - s0) < 1e-4, "%.1f mm" % (s2 * 1000.0))
	surface.worn_tracks = true
	near_on = true


# Hidden stones and roots (mud round 14.3): what lies in 80 cm of slurry is the same each time it is asked for; a wheel's
# rut stops on a stone's top; bodies stand near the followed point; dry or without mud there are none.
func _run_buried_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	surface.buried_obstacles = true
	surface.buried_density = 0.5
	await _paint_mud(0.8, 1.0)
	var found := surface.get_buried_near(Vector3.ZERO, 4.0)
	var wide := surface.get_buried_near(Vector3.ZERO, 6.0)
	var again := surface.get_buried_near(Vector3.ZERO, 4.0)
	_check("stones lie hidden in the mud, the same every time", found.size() >= 6 and str(found) == str(again), "%d within 4 m" % found.size())
	# The shallowest stone near the middle, and a spot 0.6 m clear of everything.
	var stone := {}
	for d in found:
		if not d["root"] and (stone.is_empty() or (d["a"] as Vector3).y > (stone["a"] as Vector3).y) and (d["a"] as Vector3).length() < 3.0:
			stone = d
	var c: Vector3 = stone["a"]
	var top_depth := surface.get_surface_height_at(Vector3(c.x, 0, c.z)) - (c.y + float(stone["radius"]))
	var clear := Vector3.ZERO
	for k in 200:
		var q := Vector3(randf_range(-3, 3), 0, randf_range(-3, 3))
		if surface.get_buried_depth_at(q, 0.6) > 5.0:
			clear = q
			break
	var dq := surface.get_buried_depth_at(Vector3(c.x, 0, c.z), 0.05)
	_check("the depth to a stone is its top's", absf(dq - top_depth) < 0.01, "%.1f cm (top %.1f cm down)" % [dq * 100.0, top_depth * 100.0])
	for i in 20:
		surface.press_wheel(Vector3(c.x, 0, c.z), Vector2(0.2, 0.2), 0.0, 0.7, Vector2())
		surface.press_wheel(clear, Vector2(0.2, 0.2), 0.0, 0.7, Vector2())
		await get_tree().physics_frame
	surface.sync_deformation()
	var on_stone := surface.get_dent_at(Vector3(c.x, 0, c.z))
	var off := surface.get_dent_at(clear)
	_check("a wheel's rut stops on the stone", on_stone < top_depth + 0.02 and off > on_stone + 0.1, "%.1f cm deep on it (top %.1f cm), %.1f cm clear of it" % [on_stone * 100.0, top_depth * 100.0, off * 100.0])
	# Dug round (round 15.2): a wide print with a stone under its edge sinks in full; the stone's crown stays in the
	# rut floor. Then a rolling tyre deep past another's middle knocks it loose as a rigid body.
	var picks := []
	for dd in wide:
		var a2: Vector3 = dd["a"]
		var td := surface.get_surface_height_at(Vector3(a2.x, 0, a2.z)) - (a2.y + float(dd["radius"]))
		var alone := true # nothing else under a print beside it
		for o in surface.get_buried_near(a2, 3.0):
			if int(o["id"]) != int(dd["id"]):
				var oa: Vector3 = o["a"]
				var ob: Vector3 = o["b"]
				alone = alone and Geometry2D.get_closest_point_to_segment(Vector2(a2.x, a2.z), Vector2(oa.x, oa.z), Vector2(ob.x, ob.z)).distance_to(Vector2(a2.x, a2.z)) > float(o["radius"]) + float(dd["radius"]) + 0.7
		if alone and not dd["root"] and float(dd["radius"]) < 0.16 and td < 0.45 and Vector2(a2.x - c.x, a2.z - c.z).length() > 1.5:
			var apart := true
			for q in picks:
				apart = apart and Vector2(a2.x - (q["a"] as Vector3).x, a2.z - (q["a"] as Vector3).z).length() > 1.5
			if apart:
				picks.append(dd)
	if picks.size() < 2:
		_check("stones for the dug-round tests", false, "%d found" % picks.size())
	else:
		var s2: Dictionary = picks[0]
		var a2: Vector3 = s2["a"]
		var r2: float = s2["radius"]
		var td2 := surface.get_surface_height_at(Vector3(a2.x, 0, a2.z)) - (a2.y + r2)
		var mid := Vector3(a2.x - (r2 + 0.15), 0, a2.z)
		# Stiff mud: a slurry's own flow would carry off the mud left on the crown (the stone's body shows there).
		await _paint_mud(0.8, STIFF_MUD)
		for i in 20:
			surface.press_wheel(mid, Vector2(0.8, 0.5), 0.0, 0.6, Vector2())
			await get_tree().physics_frame
		surface.sync_deformation()
		var crown := surface.get_dent_at(Vector3(a2.x, 0, a2.z))
		var floor_d := surface.get_dent_at(mid)
		_check("the mud is dug out round a stone under the print's edge", crown < td2 + 0.03 and floor_d > td2 + 0.15, "%.1f cm deep over it (top %.1f cm down), %.1f cm beside it" % [crown * 100.0, td2 * 100.0, floor_d * 100.0])
		# In slurry (round 16.1): slumping and the mud's waves flow round the stone's top and close the rut beside it.
		if picks.size() >= 3:
			await _paint_mud(0.8, 1.0)
			var s4: Dictionary = picks[2]
			var a4: Vector3 = s4["a"]
			var r4: float = s4["radius"]
			var td4 := surface.get_surface_height_at(Vector3(a4.x, 0, a4.z)) - (a4.y + r4)
			var mid4 := Vector3(a4.x - (r4 + 0.15), 0, a4.z)
			for i in 20:
				surface.press_wheel(mid4, Vector2(0.8, 0.5), 0.0, 0.6, Vector2())
				await get_tree().physics_frame
			surface.sync_deformation()
			var beside0 := surface.get_dent_at(mid4)
			var crown0 := surface.get_dent_at(Vector3(a4.x, 0, a4.z))
			for i in 180:
				await get_tree().physics_frame
			surface.sync_deformation()
			var crown4 := surface.get_dent_at(Vector3(a4.x, 0, a4.z))
			var beside1 := surface.get_dent_at(mid4)
			_check("slurry flows round a stone's top, not off it", crown0 < td4 + 0.03 and crown4 < td4 + 0.03 and beside1 < beside0 - 0.01, "%.1f cm deep over it as pressed, %.1f cm later (top %.1f cm down); the rut beside it %.1f cm, then %.1f cm" % [crown0 * 100.0, crown4 * 100.0, td4 * 100.0, beside0 * 100.0, beside1 * 100.0])
			await _paint_mud(0.8, STIFF_MUD)
		var s3: Dictionary = picks[1]
		var a3: Vector3 = s3["a"]
		var r3: float = s3["radius"]
		var td3 := surface.get_surface_height_at(Vector3(a3.x, 0, a3.z)) - (a3.y + r3)
		var wheel := Vector3(a3.x - 0.3, 0, a3.z)
		var before := surface.get_loose_stone_count()
		for i in 6:
			surface.press_wheel(wheel, Vector2(0.8, 0.5), 0.0, td3 + r3 + 0.05, Vector2(2.0, 0.0))
			await get_tree().physics_frame
		var loose := get_tree().get_nodes_in_group("ground_loose")
		var gone := true
		for dd in surface.get_buried_near(a3, 1.0):
			gone = gone and int(dd["id"]) != int(s3["id"])
		var p0: Vector3 = (loose[0] as Node3D).global_position if loose.size() > 0 else Vector3.ZERO
		for i in 20:
			await get_tree().physics_frame
		var p1: Vector3 = (loose[0] as Node3D).global_position if loose.size() > 0 else Vector3.ZERO
		# Shoved along (round 16.3): a tyre rolling on behind it, a little to one side, drives it ahead and off its line.
		var shove0: Vector3 = (loose[0] as Node3D).global_position if loose.size() > 0 else Vector3.ZERO
		if loose.size() > 0:
			for i in 60:
				var sp: Vector3 = (loose[0] as Node3D).global_position
				var tyre := Vector3(maxf(sp.x - 0.3, wheel.x + 2.0 * float(i) / 60.0), 0, a3.z - 0.08)
				surface.press_wheel(tyre, Vector2(0.8, 0.5), 0.0, 0.15, Vector2(2.0, 0.0))
				await get_tree().physics_frame
		var shove1: Vector3 = (loose[0] as Node3D).global_position if loose.size() > 0 else Vector3.ZERO
		_check("a loose stone ahead of a tyre is shoved on and aside", shove1.x - shove0.x > 0.8 and shove1.z - shove0.z > 0.03, "%.2f m ahead, %.2f m aside in 1 s behind a tyre at 2 m/s" % [shove1.x - shove0.x, shove1.z - shove0.z])
		_check("an uncovered stone a tyre runs into is knocked loose", surface.get_loose_stone_count() == before + 1 and loose.size() == 1 and gone and p1.x - p0.x > 0.05, "%d loose, %d bodies, %s from the hidden list; moved %.2f m" % [surface.get_loose_stone_count() - before, loose.size(), "gone" if gone else "still", p1.x - p0.x])
		if loose.size() > 0:
			await _loose_mud_tests(loose[0] as RigidBody3D)
	for i in 40:
		await get_tree().physics_frame
	var bodies := get_tree().get_nodes_in_group("ground_buried").size()
	_check("bodies stand for them near the followed point", bodies >= 6, "%d bodies" % bodies)
	surface.buried_obstacles = false
	var none := surface.get_buried_near(Vector3.ZERO, 4.0).size()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var left := get_tree().get_nodes_in_group("ground_buried").size()
	surface.buried_obstacles = true
	surface.mud_layer = false
	var dry := surface.get_buried_near(Vector3.ZERO, 4.0).size()
	surface.mud_layer = true
	var loose_left := get_tree().get_nodes_in_group("ground_loose").size()
	_check("off, or with no mud layer, there are none", none == 0 and left == 0 and dry == 0 and loose_left == 0, "%d, %d bodies, %d without mud, %d loose" % [none, left, dry, loose_left])
	near_on = true


# Loose stones in the mud (round 17): rolling through soft mud a stone ploughs a furrow with a heap where it stops;
# at rest on mud too weak to hold it, it sinks back in and is hidden again (kept with the deposits); stiff mud holds it.
func _loose_mud_tests(rb: RigidBody3D) -> void:
	# The terrain's collider, as a game has one (the bench has none): the undisturbed ground.
	var floor_body := StaticBody3D.new()
	var fc := CollisionShape3D.new()
	fc.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(fc)
	add_child(floor_body)
	floor_body.global_position = Vector3(0, float(height.get_height(Vector3(-1.0, 0, -3.5))), 0)
	await _paint_mud(0.8, 0.7)
	surface.clear_deformation()
	var start := Vector3(-3.0, 0, -3.5)
	var g0 := surface.get_surface_height_at(start)
	var rc0 := ((rb.get_child(0) as CollisionShape3D).shape as SphereShape3D).radius
	rb.global_position = Vector3(start.x, g0 + rc0, start.z)
	rb.linear_velocity = Vector3.ZERO
	rb.angular_velocity = Vector3.ZERO
	for i in 20:
		await get_tree().physics_frame
	# A narrow tyre barely printing, right behind it, driving it on for a second.
	var path := []
	for i in 60:
		var sp: Vector3 = rb.global_position
		path.append(sp)
		surface.press_wheel(Vector3(sp.x - rc0 - 0.2, 0, start.z), Vector2(0.16, 0.4), 0.0, 0.005, Vector2(1.5, 0.0))
		await get_tree().physics_frame
	surface.sync_deformation()
	for i in 60:
		await get_tree().physics_frame
	surface.sync_deformation()
	var stop: Vector3 = rb.global_position
	var furrow := 0.0
	for k in range(20, 50, 6):
		var q: Vector3 = path[k]
		furrow += surface.get_dent_at(Vector3(q.x, 0, q.z)) / 5.0
	var heap := 0.0
	for k in 10:
		heap = maxf(heap, surface.get_rim_at(Vector3(stop.x + 0.03 * k, 0, stop.z)))
	var run := stop.distance_to(start)
	print("  ploughed: driven %.2f m, furrow %.1f cm, heap %.1f cm ahead" % [run, furrow * 100.0, heap * 100.0])
	_check("a stone shoved through soft mud ploughs a furrow and heaps mud in front", run > 0.5 and furrow > 0.006 and heap > 0.0015, "driven %.2f m, furrow %.1f cm, heap %.1f cm" % [run, furrow * 100.0, heap * 100.0])
	# Stiff mud holds a stone at rest near its surface; in slurry it settles deep; mud laid over it hides it again.
	await _paint_mud(0.8, STIFF_MUD)
	var held_at := Vector3(2.5, 0, -3.5)
	var r_c := ((rb.get_child(0) as CollisionShape3D).shape as SphereShape3D).radius
	rb.global_position = Vector3(held_at.x, surface.get_surface_height_at(held_at) + r_c + 0.05, held_at.z)
	rb.linear_velocity = Vector3.ZERO
	for i in 90:
		await get_tree().physics_frame
	var stiff_in := surface.get_surface_height_at(held_at) - (rb.global_position.y - r_c)
	await _paint_mud(0.8, 1.0)
	for i in 240:
		await get_tree().physics_frame
	var slurry_in := surface.get_surface_height_at(held_at) - (rb.global_position.y - r_c)
	print("  settled: %.1f cm into stiff mud, %.1f cm into slurry (stone %.0f cm across)" % [stiff_in * 100.0, slurry_in * 100.0, r_c * 200.0])
	_check("a stone at rest settles into the mud by its weight: little into stiff mud, deep into slurry", stiff_in > 0.002 and stiff_in < 0.03 and slurry_in > 0.04 and slurry_in < 2.0 * r_c + 0.02, "%.1f cm, %.1f cm" % [stiff_in * 100.0, slurry_in * 100.0])
	var was := surface.get_reburied_stone_count()
	var s_before := surface.get_surface_height_at(held_at)
	surface.add_mud_deposit(held_at, 0.8, 0.6)
	await get_tree().physics_frame
	print("  laid over it: %.1f cm" % ((surface.get_surface_height_at(held_at) - s_before) * 100.0))
	var frames := 0
	for i in 30:
		await get_tree().physics_frame
		frames = i
		if not is_instance_valid(rb):
			break
	var hidden := surface.get_reburied_stone_count() - was
	var bodies := get_tree().get_nodes_in_group("ground_loose").size()
	var near := surface.get_buried_near(held_at, 0.5).size()
	_check("mud laid over a loose stone hides it again", hidden == 1 and bodies == 0 and near >= 1, "%d hidden after %d frames, %d loose bodies, %d hidden near" % [hidden, frames, bodies, near])
	# Kept with the deposits.
	var dir := OS.get_user_data_dir().path_join("rebury_keep_test")
	var was_keeping := surface.keep_ruts
	surface.keep_ruts = true
	surface.ruts_path = dir
	surface.clear_saved_ruts()
	surface.save_ruts()
	surface.clear_mud_deposits()
	var cleared := surface.get_reburied_stone_count()
	surface.ruts_path = ""
	surface.ruts_path = dir
	var kept := surface.get_reburied_stone_count()
	surface.clear_saved_ruts()
	surface.ruts_path = ""
	surface.keep_ruts = was_keeping
	_check("stones hidden again are kept with the deposits", cleared == 0 and kept == hidden and hidden == 1, "%d cleared, %d read back" % [cleared, kept])
	floor_body.queue_free()
	await _paint_mud(0.8, STIFF_MUD)


# Carried (mud round 18.1): soaked mud running down a bank takes a loose stone with it, stiff mud leaves it; a debris
# flow sweeps one along its channel and leaves it where the flow stops.
func _run_carry_tests() -> void:
	var moved := {}
	for k in ["stiff", "soaked"]:
		near_on = false
		await _setup("bank", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
		await _paint_mud(0.4, STIFF_MUD if k == "stiff" else 1.0)
		var p0 := Vector3(1.0, 0, 0.5)
		surface.add_loose_stone(Vector3(p0.x, surface.get_surface_height_at(p0) + 0.11, p0.z), 0.13)
		var rb := get_tree().get_nodes_in_group("ground_loose")[0] as Node3D
		await get_tree().physics_frame
		var a := rb.global_position
		var b := a
		for i in 600:
			await get_tree().physics_frame
			if is_instance_valid(rb):
				b = rb.global_position
		moved[k] = b - a
		print("  %s bank: stone moved %s, %s" % [k, moved[k], "then buried where the mud piled" if surface.get_reburied_stone_count() > 0 else "still in sight"])
	_check("soaked mud running down a bank carries a loose stone; stiff mud holds it", moved["soaked"].x < -0.3 and absf(moved["stiff"].x) < 0.05, "%.2f m down the bank soaked, %.2f m stiff" % [-moved["soaked"].x, -moved["stiff"].x])
	# Rolled onto (round 18.2): a tyre whose floor passes a few centimetres under the top of a stone lying in a rut
	# presses it in under its tread's middle, and squeezes it out sideways under its shoulder.
	var res := {}
	for k in ["middle", "shoulder"]:
		await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
		await _paint_mud(0.8, 0.7)
		var off := 0.0 if k == "middle" else 0.2
		var p1 := Vector3(off, 0, 0)
		# A rut first, the stone lying in its floor (soft mud: it holds the rut).
		for i in 40:
			surface.press_wheel(Vector3(0, 0, -0.6 + 0.03 * i), Vector2(0.6, 0.3), 0.0, 0.3, Vector2(0, 2.0))
			await get_tree().physics_frame
		surface.sync_deformation()
		surface.add_loose_stone(Vector3(off, surface.get_surface_height_at(p1) + 0.12, 0.0), 0.13)
		var st := get_tree().get_nodes_in_group("ground_loose")[0] as RigidBody3D
		for i in 150:
			await get_tree().physics_frame
		var a := st.global_position
		var top := a.y + 0.13
		var floor_y := top - 0.04
		var sink: float = float(height.get_height(a)) - floor_y
		var zz := -0.6
		while zz < 0.6:
			surface.press_wheel(Vector3(0, 0, zz), Vector2(0.5, 0.3), 0.0, sink, Vector2(0, 2.0))
			zz += 2.0 / 60.0
			await get_tree().physics_frame
		for i in 30:
			await get_tree().physics_frame
		var b := st.global_position
		res[k] = [b.y + 0.13 - floor_y, b.x - a.x, b.z - a.z]
		print("  tyre over a stone at its %s: top %.1f cm over the floor after (4 before), %.2f m aside, %.2f m along" % [k, res[k][0] * 100.0, res[k][1], res[k][2]])
	_check("a tyre rolling onto a sunk stone presses it in under its middle", res["middle"][0] < 0.015 and absf(res["middle"][1]) < 0.05 and absf(res["middle"][2]) < 0.1, "top %.1f cm over the floor, %.2f m aside" % [res["middle"][0] * 100.0, res["middle"][1]])
	_check("and squeezes it out sideways under its shoulder", res["shoulder"][1] > 0.1 and surface.get_squeezed_stone_count() > 0, "%.2f m out" % res["shoulder"][1])
	near_on = true
	await _soil_scene("soil")
	var stub := DebrisValley.new()
	WorldContracts.set_ground(stub)
	var flow = ClassDB.instantiate("GroundDebrisFlow3D")
	flow.listen = false
	flow.show_flow = false
	add_child(flow)
	var start := Vector3(0.0, 0.12, 10.0)
	surface.add_loose_stone(start, 0.14)
	var stone := get_tree().get_nodes_in_group("ground_loose")[0] as RigidBody3D
	flow.start(_debris_event(1, 0.0, 2000.0))
	var top := 0.0
	var frames := 0
	while frames < 900 and flow.get_active_bodies() > 0:
		flow.advance(0.1, 0.05)
		await get_tree().physics_frame
		frames += 1
		top = maxf(top, stone.linear_velocity.length())
	for i in 60:
		await get_tree().physics_frame
	var end := stone.global_position
	print("  debris flow: stone from z %.1f to %.1f m, top speed %.1f m/s, at rest %.2f m/s after %d frames" % [start.z, end.z, top, stone.linear_velocity.length(), frames])
	_check("a debris flow sweeps a loose stone down its channel and leaves it", end.z - start.z > 3.0 and top > 1.0 and stone.linear_velocity.length() < 0.2, "%.1f m down, %.1f m/s at most" % [end.z - start.z, top])
	WorldContracts.set_ground(null)
	flow.queue_free()


# Tracked-out mud as dust (mud round 18.3): a muddy tyre's trail on dry ground gives no dust while wet; dried, a tyre
# over it lifts it and the trail thins, and the wind lifts it too.
func _run_trackdust_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.1, 0.0)
	surface.drying_time = 1.0
	var x := -2.0
	while x <= 2.0:
		surface.track_mud(Vector3(x, 0, 2.0), 0.15, 0.001, 1.0)
		x += 0.1
	for i in 5:
		await get_tree().physics_frame
	surface.sync_deformation()
	var p := Vector3(0.0, 0, 2.0)
	var wet: Vector2 = surface.get_tracked_mud_at(p)
	var kg0 := surface.get_tracked_dust_lifted()
	for i in 10:
		surface.add_dust(p, Vector3(0, 1, 0), 3.0)
		await get_tree().physics_frame
	var kg_wet := surface.get_tracked_dust_lifted() - kg0
	for i in 240:
		await get_tree().physics_frame
	surface.sync_deformation()
	var dry: Vector2 = surface.get_tracked_mud_at(p)
	var kg1 := surface.get_tracked_dust_lifted()
	for i in 60:
		surface.add_dust(p, Vector3(0, 1, 0), 3.0)
		await get_tree().physics_frame
	surface.sync_deformation()
	var after: Vector2 = surface.get_tracked_mud_at(p)
	var kg2 := surface.get_tracked_dust_lifted()
	print("  trail: share %.2f wet %.2f; dried %.2f / %.2f; after a tyre %.2f; lifted %.3f kg wet, %.3f kg dry" % [wet.x, wet.y, dry.x, dry.y, after.x, kg_wet, kg2 - kg1])
	_check("a wet mud trail gives no dust; dried, a tyre lifts it and the trail thins", wet.x > 0.5 and kg_wet == 0.0 and dry.y < 0.1 and kg2 - kg1 > 0.05 and after.x < dry.x - 0.1, "%.3f kg wet, %.3f kg dry; share %.2f to %.2f" % [kg_wet, kg2 - kg1, dry.x, after.x])
	var q := Vector3(1.6, 0, 2.0)
	var w0 := _trail_share()
	surface.wind_override = Vector2(12.0, 0.0)
	surface.runoff_time_scale = 2000.0
	for i in 120:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	surface.wind_override = Vector2.ZERO
	surface.sync_deformation()
	var w1 := _trail_share()
	var kg3 := surface.get_tracked_dust_lifted()
	print("  wind 12 m/s on the dried trail: mean share %.2f -> %.2f, %.3f kg lifted" % [w0.x, w1.x, kg3 - kg2])
	_check("the wind lifts a dried trail too", kg3 - kg2 > 0.0 and w1.x < w0.x - 0.01, "share %.2f to %.2f" % [w0.x, w1.x])
	# Kept with the ruts: a dried trail saved and read back by a new node still lifts in the wind.
	surface.clear_saved_ruts()
	ruts_on = true
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.1, 0.0)
	surface.drying_time = 1.0
	x = -2.0
	while x <= 2.0:
		surface.track_mud(Vector3(x, 0, 2.0), 0.15, 0.001, 1.0)
		x += 0.1
	for i in 245:
		await get_tree().physics_frame
	surface.save_ruts()
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.1, 0.0)
	surface.drying_time = 1.0
	for i in 6:
		await get_tree().physics_frame
	surface.sync_deformation()
	var r0 := _trail_share()
	var kg4 := surface.get_tracked_dust_lifted()
	surface.wind_override = Vector2(12.0, 0.0)
	surface.runoff_time_scale = 2000.0
	for i in 120:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	surface.wind_override = Vector2.ZERO
	surface.sync_deformation()
	var r1 := _trail_share()
	var kg5 := surface.get_tracked_dust_lifted() - kg4
	print("  reloaded trail, wind 12 m/s: mean share %.2f -> %.2f, %.3f kg lifted" % [r0.x, r1.x, kg5])
	_check("wind lifts a dried trail read back from a save", r0.x > 0.3 and kg5 > 0.0 and r1.x < r0.x - 0.01, "share %.2f to %.2f, %.3f kg" % [r0.x, r1.x, kg5])
	surface.clear_saved_ruts()
	ruts_on = false
	near_on = true



# The tracked trail's mean share along z 2 over -2 < x < 2, as a Vector2 (x): the wind thins it in patches.
func _trail_share() -> Vector2:
	var sum := 0.0
	var n := 0
	var x := -1.95
	while x < 2.0:
		sum += surface.get_tracked_mud_at(Vector3(x, 0, 2.0)).x
		n += 1
		x += 0.05
	return Vector2(sum / n, 0.0)

# A game trail along z = 0 for |x| < 6, 1 m wide, walked 365 times a year.
# Two daily trails crossing at the origin, 2 m wide.
class WallowStandStub extends WorldStand:
	func _trail_at(p: Vector3) -> Vector2:
		if absf(p.z) < 1.0 or absf(p.x) < 1.0:
			return Vector2(365.0, 1.0)
		return Vector2.ZERO


class WallowAirStub extends WorldWeather:
	var temp := 20.0

	func _air_temperature_at(_p: Vector3) -> float:
		return temp


class TrailStandStub extends WorldStand:
	func _trail_at(p: Vector3) -> Vector2:
		if absf(p.x) > 6.0 or absf(p.z) > 0.5:
			return Vector2.ZERO
		return Vector2(365.0, 1.0 - smoothstep(0.25, 0.5, absf(p.z)))


# Game trails (mud round 20): the stand contract's worn paths wear the ground as wheels do: a trail walked every
# day holds P T / (365 ln 2) passes over its tread (T the regrowth half-life), packed, bearing better, and slower to
# drain, so rain stands on it first.
func _run_trail_wear_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	surface.worn_tracks = true
	surface.standing_water = true
	surface.trail_pugging = false # its own check below: a pock's rim by the probe would decide the race
	WorldContracts.set_stand(TrailStandStub.new())
	surface.refresh_ground()
	for i in 5:
		await get_tree().physics_frame
	var on := Vector3(1.0, 0, 0.0)
	var off := Vector3(1.0, 0, 2.0)
	var want := 365.0 * surface.wear_regrowth_days / (365.0 * log(2.0))
	var w_on := surface.get_wear_at(on)
	var w_off := surface.get_wear_at(off)
	print("  trail: %.1f passes on the tread (%.1f expected), %.1f off it" % [w_on, want, w_off])
	_check("a trail walked daily holds its steady passes over its tread", absf(w_on - want) < 0.5 and w_off == 0.0, "%.1f on, %.1f off" % [w_on, w_off])
	var s_on: float = surface.get_wheel_soil(on, 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	var s_off: float = surface.get_wheel_soil(off, 3000.0, 0.35, 0.25, 200.0, 0.0, 0.0)["sinkage"]
	_check("the trail is packed: it bears a wheel better", s_on < 0.7 * s_off and s_on > 0.0, "%.1f mm on it, %.1f mm off" % [s_on * 1000.0, s_off * 1000.0])
	# Rain: the packed tread drains slower, so water stands on it first.
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 1.0
	wm.rain_rate = 20.0
	wm.humidity = 0.9
	wm.temperature = 12.0
	add_child(wm)
	wm.snap_to_settings()
	surface.runoff_time_scale = 100.0
	var first_on := -1
	var first_off := -1
	for i in 600:
		await get_tree().physics_frame
		if first_on < 0 and surface.get_standing_water_at(on) > 0.0005:
			first_on = i
		if first_off < 0 and surface.get_standing_water_at(off) > 0.0005:
			first_off = i
	surface.runoff_time_scale = 1.0
	print("  rain 20 mm/h: water stands on the trail after %d frames, off it after %d (-1 never)" % [first_on, first_off])
	_check("rain stands on the trail before the ground beside it", first_on >= 0 and (first_off < 0 or first_on < first_off), "%d frames on, %d off" % [first_on, first_off])
	wm.queue_free()
	# Pugging (round 21.2): ten days of the trail's traffic (sped up) pock it with hooves where the soil is wet, and
	# leave it smooth where it is dry.
	var pocks := {}
	for wet in [0.2, 0.8]:
		await _setup("flat", GroundSurface3D.GROUND_SOIL, wet, 0.0)
		surface.worn_tracks = true
		surface.refresh_ground()
		for i in 5:
			await get_tree().physics_frame
		var line := []
		for k in 21:
			line.append(surface.get_surface_height_at(Vector3(-5.0 + 0.5 * k, 0, 0.05 * (k % 3 - 1))))
		var n0 := surface.get_trail_pock_count()
		surface.runoff_time_scale = 86400.0
		for i in 600:
			await get_tree().physics_frame
		surface.runoff_time_scale = 1.0
		surface.sync_deformation()
		var deepest := 0.0
		for k in 21:
			deepest = maxf(deepest, line[k] - surface.get_surface_height_at(Vector3(-5.0 + 0.5 * k, 0, 0.05 * (k % 3 - 1))))
		pocks[wet] = [surface.get_trail_pock_count() - n0, deepest]
		print("  moisture %.1f: %d hoof prints in ten days, deepest along the line %.1f cm" % [wet, pocks[wet][0], deepest * 100.0])
	_check("hooves pock a wet trail and leave a dry one smooth", pocks[0.2][0] == 0 and pocks[0.8][0] > 20 and pocks[0.8][1] > 0.005, "%d prints dry, %d wet (%.1f cm deep)" % [pocks[0.2][0], pocks[0.8][0], pocks[0.8][1] * 100.0])
	# Wallows (round 22.3): two trails crossing, walked daily, over a hundred days (sped up): in wet ground animals
	# roll out muddy bowls at the hashed sites along them, more in the warmth; none in dry ground.
	surface.trail_pugging = false
	var wal := {}
	for leg in [[0.2, 20.0], [0.8, 20.0], [0.8, 2.0]]:
		await _setup("flat", GroundSurface3D.GROUND_SOIL, leg[0], 0.0)
		surface.worn_tracks = true
		surface.trail_pugging = false
		WorldContracts.clear_overrides()
		WorldContracts.set_stand(WallowStandStub.new())
		var air := WallowAirStub.new()
		air.temp = leg[1]
		WorldContracts.set_weather(air)
		surface.refresh_ground()
		for i in 5:
			await get_tree().physics_frame
		var grid := {}
		for gx in 97:
			for gz in 97:
				var q := Vector3(-24.0 + 0.5 * gx, 0, -24.0 + 0.5 * gz)
				if absf(q.x) < 4.5 or absf(q.z) < 4.5:
					grid[q] = surface.get_surface_height_at(q)
		var v0: int = surface.get_wallow_visits()
		surface.runoff_time_scale = 10.0 * 86400.0
		for i in 600:
			await get_tree().physics_frame
		surface.runoff_time_scale = 1.0
		surface.sync_deformation()
		# Against the ground's mean change (frost heave, settling under the weather lift or drop it all).
		var drops := {}
		var mean := 0.0
		for q in grid:
			drops[q] = grid[q] - surface.get_surface_height_at(q)
			mean += drops[q] / grid.size()
		var deepest := 0.0
		for q in grid:
			deepest = maxf(deepest, drops[q] - mean)
		print("    mean change %.1f cm" % (-mean * 100.0))
		wal[leg] = [surface.get_wallow_count(), surface.get_wallow_visits() - v0, deepest]
		print("  moisture %.1f, %.0f C: %d wallows, %d visits in 100 days, deepest %.1f cm" % [leg[0], leg[1], wal[leg][0], wal[leg][1], deepest * 100.0])
	var dry = wal[[0.2, 20.0]]
	var warm = wal[[0.8, 20.0]]
	var cold = wal[[0.8, 2.0]]
	_check("animals roll wallows into wet trails and none into dry ones", dry[0] == 0 and warm[0] >= 1 and warm[2] > 0.05, "%d dry, %d wet, %.1f cm deep" % [dry[0], warm[0], warm[2] * 100.0])
	_check("they wallow more in the warmth", warm[1] > cold[1], "%d visits at 20 C, %d at 2 C" % [warm[1], cold[1]])
	WorldContracts.clear_overrides()
	near_on = true


# Water for the fording car: `depth` m deep over the floor everywhere, flowing along +x at `speed`.
class FordStub extends WorldWater:
	var depth := 0.0
	var speed := 0.0

	func _height_at(_p: Vector3) -> float:
		return depth if depth > 0.0 else -INF

	func _flow_at(_p: Vector3) -> Vector3:
		return Vector3(speed, 0, 0)

	# The bow wave's disturbances (round 21.3), recorded.
	var pushes := 0
	var clears := 0
	var strongest := 0.0

	func _disturb(_owner: int, _p: Vector3, _r: float, s: float) -> void:
		pushes += 1
		strongest = maxf(strongest, s)

	func _clear(_owner: int) -> void:
		clears += 1


# A car in a ford (mud round 20.1): buoyed by the water it keeps out and pushed by the current across its side, it
# holds in a shallow slow ford, is carried off in a deep fast one, and floats in deep still water. The bed is the
# soil soaked by the water: the tyres sink into it as into any ford's mud.
func _run_ford_car_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	var floor_body := StaticBody3D.new()
	var fc := CollisionShape3D.new()
	fc.shape = BoxShape3D.new()
	(fc.shape as BoxShape3D).size = Vector3(400, 2, 400)
	fc.position = Vector3(0, -1, 0)
	floor_body.add_child(fc)
	add_child(floor_body)
	WorldContracts.clear_overrides()
	var stub := FordStub.new()
	WorldContracts.add_water_stub(stub)
	var res := {}
	for leg in [[0.0, 0.0], [0.5, 0.0], [0.5, 0.3], [0.3, 1.0], [0.5, 1.0], [0.5, 2.0], [0.7, 1.5], [0.7, 3.0]]:
		stub.depth = 0.0
		stub.speed = 0.0
		var car := _make_car(1500.0, 800.0, Vector3(0, 0.5, 0))
		for i in 60:
			await get_tree().physics_frame
		car.brake = 1.0
		# The water rises over a second, then 4 s more: how far the current carries it once it has settled in.
		for i in 60:
			stub.depth = leg[0] * (i + 1) / 60.0
			stub.speed = leg[1] * (i + 1) / 60.0
			await get_tree().physics_frame
		for i in 60:
			await get_tree().physics_frame
		var a := car.global_position
		for i in 240:
			await get_tree().physics_frame
		var moved := car.global_position.x - a.x
		var lift := car.get_water_buoyancy()
		var drag := car.get_water_drag().x
		res[leg] = moved
		print("  %.1f m deep at %.1f m/s: carried %.2f m in 4 s; buoyancy %.1f kN, drag %.2f kN (weight 14.7 kN)" % [leg[0], leg[1], moved, lift / 1000.0, drag / 1000.0])
		car.queue_free()
		await get_tree().physics_frame
	_check("a shallow, slow ford leaves a braked car where it stands", absf(res[[0.3, 1.0]]) < 0.2 and absf(res[[0.5, 1.0]]) < 0.2, "%.2f m at 0.3 m, %.2f m at 0.5 m (1 m/s)" % [res[[0.3, 1.0]], res[[0.5, 1.0]]])
	_check("a deep, fast one carries it off, rutted bed and all", res[[0.7, 3.0]] > 1.0, "%.2f m at 0.7 m and 3 m/s" % res[[0.7, 3.0]])
	# Deep still water: it floats once the water it keeps out outweighs it.
	stub.depth = 0.0
	stub.speed = 0.0
	var boat := _make_car(1500.0, 800.0, Vector3(0, 0.5, 0))
	boat.soft_soil = false
	for i in 60:
		await get_tree().physics_frame
	var y0 := boat.global_position.y
	stub.depth = 1.4
	for i in 300:
		await get_tree().physics_frame
	var rise := boat.global_position.y - y0
	print("  1.4 m of still water: lifted %.2f m, buoyancy %.1f kN" % [rise, boat.get_water_buoyancy() / 1000.0])
	_check("in deep still water it floats", rise > 0.2, "lifted %.2f m" % rise)
	# The cab floods (round 21.1): through 0.05 m2 of gaps (five times the default, to be quick) the floating car
	# fills, settles and sinks to its wheels; on land again it drains.
	boat.hull_leak_area = 0.05
	var y_float := boat.global_position.y
	for i in 2400:
		await get_tree().physics_frame
	var cab := boat.get_cab_water()
	var settled := y_float - boat.global_position.y
	stub.depth = 0.0
	for i in 600:
		await get_tree().physics_frame
	var drained := boat.get_cab_water()
	print("  flooding: %.2f m3 in the cab after 40 s, settled %.2f m; %.2f m3 left 10 s after the water went" % [cab, settled, drained])
	_check("a floating car's cab floods and it settles, and drains on land", cab > 0.5 and settled > 0.2 and drained < 0.75 * cab, "%.2f m3, settled %.2f m, %.2f m3 left" % [cab, settled, drained])
	boat.queue_free()
	# The engine drowns (round 21.1): 0.75 m of still water over a low intake (10 cm over the box's bottom) stalls
	# it, a snorkel keeps it running, and it starts again 5 s after the intake clears.
	var runs := {}
	for snork in [false, true]:
		stub.depth = 0.0
		var car := _make_car(1500.0, 800.0, Vector3(0, 0.5, 0))
		car.snorkel = snork
		car.air_intake_height = 0.1
		car.soft_soil = false # a firm ford bottom: the engine is the question here, not the mud
		for i in 60:
			await get_tree().physics_frame
		for i in 60:
			stub.depth = 0.75 * (i + 1) / 60.0
			await get_tree().physics_frame
		for i in 30:
			await get_tree().physics_frame
		var drowned := car.is_engine_drowned()
		var a := car.global_position
		car.throttle = 1.0
		for i in 180:
			await get_tree().physics_frame
		var went := Vector2(car.global_position.x - a.x, car.global_position.z - a.z).length()
		car.throttle = 0.0
		var restarted := false
		if not snork:
			stub.depth = 0.0
			for i in 240:
				await get_tree().physics_frame
			var early := car.is_engine_drowned()
			for i in 150:
				await get_tree().physics_frame
			restarted = early and not car.is_engine_drowned()
		runs[snork] = [drowned, went, restarted]
		print("  0.75 m of water, snorkel %s: drowned %s, drove %.2f m in 3 s%s" % [snork, drowned, went, "; started again 5 s after it cleared" if restarted else ""])
		car.queue_free()
		await get_tree().physics_frame
	_check("water over the intake stalls the engine; a snorkel keeps it running", runs[false][0] and not runs[true][0] and runs[false][1] < 0.3 and runs[true][1] > 1.0, "%.2f m drowned, %.2f m with a snorkel" % [runs[false][1], runs[true][1]])
	_check("and it starts again once the intake has been clear 5 s", runs[false][2], "")
	# The bow wave (round 21.3): a snorkelled car driving through 0.75 m of water pushes it at its keel, harder the faster it
	# goes, and lets it go once it is out of the water.
	stub.depth = 0.0
	var wader := _make_car(1500.0, 800.0, Vector3(0, 0.5, 0))
	wader.soft_soil = false
	wader.snorkel = true
	for i in 60:
		await get_tree().physics_frame
	for i in 60:
		stub.depth = 0.75 * (i + 1) / 60.0
		await get_tree().physics_frame
	for i in 30:
		await get_tree().physics_frame
	var at_rest := wader.get_bow_wave_strength()
	var pushes0 := stub.pushes
	wader.throttle = 1.0
	var moving := 0.0
	var trough_hi := 0.0
	var trough_lo := 0.0
	for i in 180:
		await get_tree().physics_frame
		moving = maxf(moving, wader.get_bow_wave_strength())
		trough_hi = maxf(trough_hi, wader.get_intake_trough())
		trough_lo = minf(trough_lo, wader.get_intake_trough())
	wader.throttle = 0.0
	var pushed := stub.pushes - pushes0
	var clears0 := stub.clears
	stub.depth = 0.0
	for i in 30:
		await get_tree().physics_frame
	var cleared := stub.clears - clears0
	print("  bow wave: strength %.2f at rest, %.2f driving (%.1f m/s); %d pushes, %d clears once out" % [at_rest, moving, absf(wader.get_forward_speed()), pushed, cleared])
	_check("a wading car pushes a bow wave, stronger as it goes faster, and lets the water go once out", pushed > 0 and moving > at_rest + 0.05 and cleared > 0 and wader.get_bow_wave_strength() == 0.0, "%.2f at rest, %.2f driving, %d clears" % [at_rest, moving, cleared])
	# Its trough at the intake (round 22.1): lower water there at a wading pace, the bow wave over it once racing.
	print("  intake: the water %.2f m lower at a wading pace, %.2f m higher racing" % [trough_hi, -trough_lo])
	_check("at a wading pace the bow's trough keeps the water lower at the intake; racing, the bow wave climbs over it", trough_hi > 0.03 and trough_lo < -0.03, "%.2f m lower, %.2f m higher" % [trough_hi, -trough_lo])
	wader.queue_free()
	WorldContracts.clear_overrides()
	floor_body.queue_free()
	near_on = true


# A stream for the stone tests: 0.5 m deep over |z| < 3, flowing along +x at `speed` for x < 2 and still beyond
# (a pool), out to x = 10.
class StreamStoneStub extends WorldWater:
	var bed := 0.0
	var speed := 0.0

	func _height_at(p: Vector3) -> float:
		return bed + 0.5 if absf(p.z) < 3.0 and p.x > -10.0 and p.x < 10.0 else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(speed, 0, 0) if absf(p.z) < 3.0 and p.x > -10.0 and p.x < 2.0 else Vector3.ZERO

	var silt_kg := 0.0

	func _add_sediment_mass(_p: Vector3, mineral: float, _ash: float) -> void:
		silt_kg += mineral


# Stones in a stream (mud round 19.1): on a firm bed a lone stone stays put in a slow stream, rolls off once the
# drag past its pivot beats its submerged weight, and drops in the still pool; one lying in stiff mud is held harder.
func _run_stream_stone_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	var floor_body := StaticBody3D.new()
	var fc := CollisionShape3D.new()
	fc.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(fc)
	add_child(floor_body)
	var start := Vector3(-4.0, 0, 0.3)
	# A rock bed 40 cm over the soil (the soil under water soaks to mud): the colliders hold the stone on it.
	var g0 := surface.get_surface_height_at(start) + 0.4
	floor_body.global_position = Vector3(0, g0, 0)
	WorldContracts.clear_overrides()
	var stub := StreamStoneStub.new()
	stub.bed = g0
	WorldContracts.add_water_stub(stub)
	var r := 0.12
	var rc := 0.85 * r
	var runs := {}
	var onset := -1.0
	for u in [0.5, 1.0, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0]:
		for n in get_tree().get_nodes_in_group("ground_loose"):
			n.free()
		stub.speed = 0.0
		surface.add_loose_stone(Vector3(start.x, g0 + rc + 0.01, start.z), r)
		var rb := get_tree().get_nodes_in_group("ground_loose")[0] as RigidBody3D
		for i in 30:
			await get_tree().physics_frame
		var a := rb.global_position
		stub.speed = u
		for i in 600:
			await get_tree().physics_frame
		var b := rb.global_position
		var v := rb.linear_velocity.length()
		runs[u] = [b.x - a.x, v]
		print("  %.2f m/s: moved %.2f m, now %.2f m/s at x %.2f" % [u, b.x - a.x, v, b.x])
		if onset < 0.0 and b.x - a.x > 0.5:
			onset = u
	# The Shields number at the onset, by the depth-averaged speed over a fine bed (ks 1 cm, h 0.5 m).
	var ustar := onset * 0.4 / (log(0.5 / (0.01 / 30.0)) - 1.0)
	var theta := ustar * ustar / (1.65 * 9.81 * 2.0 * rc)
	print("  onset %.2f m/s: u* %.3f m/s, Shields %.4f (0.045 for a bed of its own size; Komar and Li's exposure about 0.005)" % [onset, ustar, theta])
	_check("a slow stream leaves a lone stone where it lies", absf(runs[0.5][0]) < 0.05 and absf(runs[1.0][0]) < 0.05, "%.2f m at 0.5 m/s, %.2f m at 1 m/s" % [runs[0.5][0], runs[1.0][0]])
	_check("a fast one rolls it off at a lone stone's Shields number", onset > 1.0 and onset <= 2.5 and theta > 0.002 and theta < 0.012, "onset %.2f m/s, Shields %.4f" % [onset, theta])
	_check("and it drops in the still pool downstream", runs[3.0][0] > 4.0 and -4.0 + runs[3.0][0] < 9.0 and runs[3.0][1] < 0.05, "stopped at x %.2f, %.2f m/s" % [-4.0 + runs[3.0][0], runs[3.0][1]])
	# Lying in mud: sunk to its bearing depth, the water reaches less of it and the mud holds it.
	for n in get_tree().get_nodes_in_group("ground_loose"):
		n.free()
	stub.speed = 0.0
	floor_body.global_position.y = g0 - 0.4
	await _paint_mud(0.8, STIFF_MUD)
	surface.clear_deformation()
	var gm := surface.get_surface_height_at(start)
	stub.bed = gm
	surface.add_loose_stone(Vector3(start.x, gm + rc + 0.01, start.z), r)
	var rm := get_tree().get_nodes_in_group("ground_loose")[0] as RigidBody3D
	for i in 150:
		await get_tree().physics_frame
	var am := rm.global_position
	stub.speed = 2.5
	var silt0 := surface.get_stone_silt_lifted()
	var max_v := 0.0
	var got0 := stub.silt_kg
	for i in 600:
		await get_tree().physics_frame
		if is_instance_valid(rm):
			max_v = maxf(max_v, rm.linear_velocity.length())
	var mm := rm.global_position.x - am.x if is_instance_valid(rm) else 0.0
	print("  top speed in the mud %.2f m/s" % max_v)
	var rolled_kg := surface.get_stone_silt_lifted() - silt0
	print("  rolled through the mud under water: %.2f kg of silt stirred up, %.2f kg reached the water" % [rolled_kg, stub.silt_kg - got0])
	_check("a stone rolled along a muddy bed clouds the water with silt", rolled_kg > 0.05 and absf(stub.silt_kg - got0 - rolled_kg) < 1e-3, "%.2f kg" % rolled_kg)
	print("  in stiff mud, sunk %.1f cm: moved %.2f m at 2.5 m/s (%.2f m on the firm bed)" % [(gm + rc - am.y) * 100.0, mm, runs[2.5][0]])
	_check("a stone lying in mud is held harder than on a firm bed", mm < 0.5 * runs[2.5][0], "%.2f m in mud, %.2f m firm" % [mm, runs[2.5][0]])
	WorldContracts.clear_overrides()
	# Kept with the deposits (round 19.2): saved, read back by a new node where and as deep as it lay.
	for i in 60:
		await get_tree().physics_frame
	var kept := rm.global_transform
	surface.clear_saved_ruts()
	ruts_on = true
	surface.keep_ruts = true
	surface.save_ruts()
	setup_mud = Vector2(0.8, STIFF_MUD)
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.0, 0.0)
	setup_mud = Vector2.ZERO
	for i in 10:
		await get_tree().physics_frame
	var back := get_tree().get_nodes_in_group("ground_loose")
	var off := INF
	var turned := INF
	if back.size() == 1:
		var bt := (back[0] as Node3D).global_transform
		off = (bt.origin - kept.origin).length()
		turned = bt.basis.get_rotation_quaternion().angle_to(kept.basis.get_rotation_quaternion())
	print("  read back: %d stones, %.1f cm from where it lay, turned %.1f degrees" % [back.size(), off * 100.0, rad_to_deg(turned)])
	_check("a loose stone is kept with the deposits, where and as deep as it lay", back.size() == 1 and off < 0.01 and turned < 0.05, "%d stones, %.1f cm off" % [back.size(), off * 100.0])
	surface.clear_saved_ruts()
	ruts_on = false
	# Scour (round 20.2): a slower current over a stone at rest in the mud digs a horseshoe hole at its upstream face
	# and lays a tail in its lee (sped up 20000 times: 10 s is two days and a half;
# a cohesive bed erodes at a few millimetres an hour).
	# The soil the river soaks (no mud laid over it): a bed firm enough to hold a hole, where the stiff mud's
	# liquid top fills one back at once.
	for n in get_tree().get_nodes_in_group("ground_loose"):
		n.free()
	stub.speed = 0.0
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	var sp := Vector3(-6.0, 0, 0.3)
	var gs := surface.get_surface_height_at(sp)
	floor_body = StaticBody3D.new()
	var fc2 := CollisionShape3D.new()
	fc2.shape = WorldBoundaryShape3D.new()
	floor_body.add_child(fc2)
	add_child(floor_body)
	floor_body.global_position = Vector3(0, gs, 0)
	for i in 120: # soaked by the river
		await get_tree().physics_frame
	gs = surface.get_surface_height_at(sp)
	WorldContracts.clear_overrides()
	WorldContracts.add_water_stub(stub)
	surface.add_loose_stone(Vector3(sp.x, gs + rc + 0.01, sp.z), r)
	var rs := get_tree().get_nodes_in_group("ground_loose")[0] as RigidBody3D
	for i in 150:
		await get_tree().physics_frame
	surface.sync_deformation()
	var y_before := rs.global_position.y
	var nose := sp - Vector3(1.1 * rc, 0, 0)
	var tail := sp + Vector3(4.0 * rc, 0, 0)
	# Against the same spots mirrored across the stone (its own print slumps round it alike on both sides).
	var lee := sp + Vector3(1.1 * rc, 0, 0)
	var head := sp - Vector3(4.0 * rc, 0, 0)
	var nose_lee0 := surface.get_surface_height_at(lee) - surface.get_surface_height_at(nose)
	var tail_head0 := surface.get_surface_height_at(tail) - surface.get_surface_height_at(head)
	var scour0 := surface.get_stone_scour_volume()
	var silt1 := surface.get_stone_silt_lifted()
	stub.speed = 1.2
	surface.runoff_time_scale = 20000.0
	for i in 600:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	stub.speed = 0.0
	surface.sync_deformation()
	var hole := surface.get_surface_height_at(lee) - surface.get_surface_height_at(nose) - nose_lee0
	var ridge := surface.get_surface_height_at(tail) - surface.get_surface_height_at(head) - tail_head0
	var settled := y_before - rs.global_position.y
	var scoured := surface.get_stone_scour_volume() - scour0
	var drift := rs.global_position.x - sp.x
	print("  scour at 1.2 m/s: hole %.1f cm deeper at its nose than its lee, tail %.1f cm over the bed upstream, %.2f L dug, stone settled %.1f cm, drifted %.2f m, %.2f kg to the water" % [hole * 100.0, ridge * 100.0, scoured * 1000.0, settled * 100.0, drift, surface.get_stone_silt_lifted() - silt1])
	_check("a current scours a hole round a stone on its bed and lays a tail behind it", hole > 0.005 and ridge > 0.0 and absf(drift) < 0.1 and surface.get_stone_silt_lifted() > silt1, "hole %.1f cm, tail %.1f cm" % [hole * 100.0, ridge * 100.0])
	if is_instance_valid(floor_body):
		floor_body.queue_free()
	WorldContracts.clear_overrides()
	near_on = true


# Water for the silt tests: a river over -8 < x < -4 whose bed record (kg/m2, mineral and ash) the test sets, and
# still floodwater 30 cm deep carrying 5 kg/m3 over 2 < x < 5 while flooding is on.
class SiltWaterStub extends WorldWater:
	var bed := Vector2.ZERO
	var flooding := true

	func _in_river(p: Vector3) -> bool:
		return p.x > -8.0 and p.x < -4.0 and absf(p.z) < 3.0

	func _in_flood(p: Vector3) -> bool:
		return flooding and p.x > 2.0 and p.x < 5.0 and absf(p.z) < 3.0

	func _height_at(p: Vector3) -> float:
		if _in_river(p) and bed.length_squared() > 0.0:
			return 0.05
		return 0.3 if _in_flood(p) else -INF

	func _flow_at(p: Vector3) -> Vector3:
		return Vector3(0, 0, 0.8) if _in_river(p) else Vector3.ZERO

	func _sediment_at(p: Vector3) -> Vector2:
		return Vector2(5.0, 0.0) if _in_flood(p) or _in_river(p) else Vector2.ZERO

	func _sediment_deposit_at(p: Vector3) -> Vector2:
		return bed if _in_river(p) else Vector2.ZERO


# Silt skins (mud round 15.1): what a river records as settled on its bed is laid once, at its dry density, as a
# wet skin; still floodwater lays its own; the skin is soft and slick while wet, dries, cracks and bears once dry.
func _run_silt_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	surface.flood_silt = true
	surface.desiccation_cracks = true
	WorldContracts.clear_overrides()
	var stub := SiltWaterStub.new()
	stub.bed = Vector2(2.6, 0.0) # 2 mm of silt at 1300 kg/m3
	WorldContracts.add_water_stub(stub)
	var r := Vector3(-6.3, 0, 0.4)
	var f := Vector3(3.4, 0, 0.4)
	for i in 90:
		await get_tree().physics_frame
	var a := surface.get_silt_at(r)
	stub.bed = Vector2(4.0, 1.2) # 4 mm in all, an ash-dark part
	for i in 90:
		await get_tree().physics_frame
	var b := surface.get_silt_at(r)
	_check("a river's settled silt is laid once, at its dry density", absf(a.x - 0.002) < 2e-4 and absf(b.x - 0.004) < 4e-4 and a.y > 0.99, "%.2f mm, then %.2f mm (record 2, then 4 mm); wet %.2f" % [a.x * 1000.0, b.x * 1000.0, a.y])
	var fl := surface.get_silt_at(f)
	_check("still floodwater lays its own silt", fl.x > 1e-7 and fl.x < 1e-4 and surface.get_silt_at(Vector3(7.0, 0, 0.4)).x == 0.0, "%.1f um" % (fl.x * 1e6))
	# The water goes; days of drying in seconds.
	stub.flooding = false
	stub.bed = Vector2(4.0, 1.2)
	var wm := WeatherMap3D.new()
	wm.cloud_cover = 0.0
	wm.rain_rate = 0.0
	wm.humidity = 0.5
	wm.temperature = 20.0
	add_child(wm)
	wm.snap_to_settings()
	WorldContracts.clear_overrides()
	surface.runoff_time_scale = 1e5
	for i in 600:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	var d := surface.get_silt_at(r)
	var cr: Vector4 = surface.get_crack_at(r)
	print("  silt skin: %.2f mm, wet %.2f -> %.2f; cracks %s" % [b.x * 1000.0, b.y, d.y, cr])
	_check("the skin dries and cracks into small plates", d.y < 0.1 and cr.x > 0.9 and cr.y < 0.08, "water %.2f, opening %.2f, plates %.1f cm" % [d.y, cr.x, cr.y * 100.0])
	# Kept with the ruts (round 16.2): saved, forgotten, read back as left, dry.
	var dir := OS.get_user_data_dir().path_join("silt_keep_test")
	var was_keeping := surface.keep_ruts
	surface.keep_ruts = true
	surface.ruts_path = dir
	surface.clear_saved_ruts()
	surface.save_ruts()
	surface.clear_mud_deposits()
	var gone := surface.get_silt_at(r).x
	surface.ruts_path = ""
	surface.ruts_path = dir
	var kept := surface.get_silt_at(r)
	var kept_dep := surface.get_mud_deposit_at(r)
	surface.clear_saved_ruts()
	surface.ruts_path = ""
	surface.keep_ruts = was_keeping
	_check("silt skins are kept with the ruts", gone == 0.0 and absf(kept.x - d.x) < 1e-6 and kept_dep >= kept.x and kept.y < 0.1, "%.2f mm saved, %.2f cleared, %.2f mm read back (water %.2f)" % [d.x * 1000.0, gone * 1000.0, kept.x * 1000.0, kept.y])
	# As dust (round 17.3): a tyre over the dry skin lifts it and thins it; the wind then lifts more where the tyre
	# broke the crust than off the unbroken skin beside it.
	var r2 := r + Vector3(1.0, 0, 0)
	var s0 := surface.get_silt_at(r).x
	var s0b := surface.get_silt_at(r2).x
	var kg0 := surface.get_silt_dust_lifted()
	for i in 60:
		surface.add_dust(r, Vector3(0, 1, 0), 0.3 * 10.0)
		await get_tree().physics_frame
	var s1 := surface.get_silt_at(r).x
	var kg1 := surface.get_silt_dust_lifted()
	print("  tyre on dry silt: %.3f -> %.3f mm, %.3f kg lifted" % [s0 * 1000.0, s1 * 1000.0, kg1 - kg0])
	_check("a tyre on a dry silt skin lifts it as dust and thins it", kg1 - kg0 > 0.1 and s0 - s1 > 5e-5 and absf((s0 - s1) * 1300.0 - (kg1 - kg0)) < 0.01, "%.2f mm thinner, %.2f kg lifted" % [(s0 - s1) * 1000.0, kg1 - kg0])
	surface.wind_override = Vector2(12.0, 0.0)
	surface.runoff_time_scale = 2000.0
	for i in 120:
		await get_tree().physics_frame
	surface.runoff_time_scale = 1.0
	surface.wind_override = Vector2.ZERO
	var s2 := surface.get_silt_at(r).x
	var s2b := surface.get_silt_at(r2).x
	print("  wind 12 m/s: broken %.3f -> %.3f mm, unbroken %.3f -> %.3f mm" % [s1 * 1000.0, s2 * 1000.0, s0b * 1000.0, s2b * 1000.0])
	_check("wind lifts dust off silt a tyre broke, not off its unbroken crust", s1 - s2 > 1e-4 and absf(s2b - s0b) < 1e-7, "broken %.2f mm thinner, unbroken %.4f mm" % [(s1 - s2) * 1000.0, (s0b - s2b) * 1000.0])
	d = surface.get_silt_at(r)
	# Off, nothing more is laid.
	surface.flood_silt = false
	stub.bed = Vector2(8.0, 1.2)
	stub.flooding = true
	WorldContracts.add_water_stub(stub)
	for i in 90:
		await get_tree().physics_frame
	var off := surface.get_silt_at(r).x
	var off_f := surface.get_silt_at(f).x
	_check("off, rivers and floods lay nothing", absf(off - d.x) < 1e-6 and absf(off_f - fl.x) < 1e-7, "%.2f mm, flood %.1f um" % [off * 1000.0, off_f * 1e6])
	WorldContracts.clear_overrides()
	wm.queue_free()
	surface.flood_silt = false
	near_on = true


# Off the patch (a contract fault, 20 Nov): far outside its lattice the surface asks its height source (or the
# colliders), not the lattice's nearest edge, so WorldGround.height_at answers the real ground everywhere.
func _run_offpatch_tests() -> void:
	near_on = false
	await _setup("camber", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	var h := get_node("Height")
	var worst := 0.0
	var line := ""
	for k in 6:
		var p := Vector3(-40.0 + 16.0 * k, 0, 70.0 + 9.0 * k)
		var want: float = h.get_height(p)
		var got := surface.get_surface_height_at(p)
		var via := WorldContracts.get_ground().height_at(p)
		worst = maxf(worst, maxf(absf(got - want), absf(via - want)))
		line += " %.2f/%.2f" % [got, want]
	print("  off the patch, surface/source m:" + line)
	var near_p := Vector3(0.3, 0, 0.2)
	var on := absf(surface.get_surface_height_at(near_p) - float(h.get_height(near_p)))
	_check("off its lattice the ground answers its height source, on it the lattice", worst < 0.01 and on < 0.05, "worst %.3f m off, %.3f m on" % [worst, on])
	near_on = true


# Ten minutes of world time a second, for the parked trucks.
class ParkedClockStub extends WorldWeather:
	func _time_scale() -> float:
		return 600.0


# Parked trucks sink in (mud round 22.2): two cars stand in the same stiff mud for 30 s, five hours of world time, one
# settling (creep, suction) and one with it off; then both pull away at the same part throttle. The settled one has
# sunk further (Buisman's log-time creep), is held by suction at first and so pulls away later; once it has rolled
# out the suction is gone.
# Peat bogs (mud round 24.2): hashed patches of root mat over soft peat where the ground is waterlogged.
func _peat_spot() -> Vector3:
	var best := Vector3.INF
	var best_v := 0.0
	for z in range(-24, 25, 2):
		for x in range(-24, 25, 2):
			var p := Vector3(x, 0, z)
			var v := 1.0
			for q in [Vector3(0, 0, 0), Vector3(2, 0, 3), Vector3(-2, 0, 3), Vector3(2, 0, -3), Vector3(-2, 0, -3)]:
				v = minf(v, surface.get_peat_bog_at(p + q).x)
			if v > best_v:
				best_v = v
				best = p
	return best if best_v > 0.9 else Vector3.INF


func _run_peat_tests() -> void:
	await _soil_scene("soil")
	surface.moisture = 0.3
	surface.settle(2.0, 0.5)
	var dry := 0.0
	for z in range(-24, 25, 4):
		for x in range(-24, 25, 4):
			dry = maxf(dry, surface.get_peat_bog_at(Vector3(x, 0, z)).x)
	surface.moisture = 0.92
	surface.settle(2.0, 0.5)
	var at := _peat_spot()
	_check("waterlogged flat ground has peat bogs; drained ground none", at != Vector3.INF and dry == 0.0, "bog at %s; %.2f at most when drained" % [str(at), dry])
	if at == Vector3.INF or not ClassDB.class_exists("PhysXVehicle3D"):
		return
	# The mat on its own: a car's wheel load against a heavy truck's (Wong's muskeg model).
	var light: Dictionary = surface.get_wheel_soil(at, 3700.0, 0.35, 0.26)
	var heavy: Dictionary = surface.get_wheel_soil(at, 17000.0, 0.45, 0.32)
	print("  mat: a car's wheel %.1f cm, a truck's %.1f cm" % [light["sinkage"] * 100.0, heavy["sinkage"] * 100.0])
	var res := {}
	for k in ["car", "truck"]:
		await _soil_scene("soil")
		surface.moisture = 0.92
		surface.settle(2.0, 0.5)
		var heavy_one: bool = k == "truck"
		var car := _make_car(7000.0 if heavy_one else 1500.0, 3000.0 if heavy_one else 700.0, at + Vector3(0, 0.45, 0))
		car.belly_drag = false
		surface.follow_target = surface.get_path_to(car)
		var tears0: int = surface.get_mat_tear_count()
		for i in 30:
			await get_tree().physics_frame
		var r: Array = await _drive(car, 3.0, 0.3)
		var sink := 0.0
		for w in 4:
			sink += car.get_wheel_sinkage(w) / 4.0
		var torn := 0.0
		for c in car.get_children():
			if c is PhysXVehicleWheel3D:
				torn = maxf(torn, surface.get_peat_bog_at((c as Node3D).global_position).y)
		res[k] = [sink, surface.get_mat_tear_count() - tears0, torn, r[0]]
		print("  %s: sinks %.1f cm, %d tears, torn %.1f under its wheels, %.2f m in 3 s" % [k, sink * 100.0, res[k][1], torn, r[0]])
	_check("the root mat holds a car up", res["car"][1] == 0 and res["car"][0] < 0.12, "%.1f cm, %d tears" % [res["car"][0] * 100.0, res["car"][1]])
	_check("a heavy truck tears the mat and drops into the peat", res["truck"][1] > 0 and res["truck"][2] > 0.5 and res["truck"][0] > 0.25,
			"%.1f cm, %d tears" % [res["truck"][0] * 100.0, res["truck"][1]])
	# Where the mat is torn the peat bears: a car's wheel sinks far deeper there than on the intact mat beside it.
	var spot: Vector3 = Vector3.INF
	for z in range(-6, 7):
		for x in range(-6, 7):
			var q := at + Vector3(x * 0.25, 0, z * 0.25)
			if surface.get_peat_bog_at(q).y > 0.5:
				spot = q
	if spot != Vector3.INF:
		var torn_soil: Dictionary = surface.get_wheel_soil(spot, 3700.0, 0.35, 0.26)
		_check("a car following in the torn ruts sinks into the peat", torn_soil["static_sinkage"] > 2.0 * light["static_sinkage"],
				"%.1f cm against %.1f cm on the mat" % [torn_soil["static_sinkage"] * 100.0, light["static_sinkage"] * 100.0])


func _run_parked_tests() -> void:
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.2, 0.0)
	_apply("soil")
	surface.near_field = false
	_add_terrain(80.0, 160, _ground_material("soil"))
	await _paint_mud(0.4, STIFF_MUD)
	WorldContracts.clear_overrides()
	WorldContracts.set_weather(ParkedClockStub.new())
	var cars := []
	for k in 2:
		var at := Vector3(-5.0 + 10.0 * k, 0, 0)
		var car := _make_car(1500.0, 700.0, Vector3(at.x, height.get_height(at) + 0.9, at.z))
		car.parked_settling = k == 0
		cars.append(car)
	for i in 1800:
		await get_tree().physics_frame
	var res := []
	for car in cars:
		var sink := 0.0
		var creep := 0.0
		var suction := 0.0
		for w in 4:
			sink += car.get_wheel_sinkage(w) / 4.0
			creep += car.get_wheel_creep(w) / 4.0
		res.append([sink, creep])
	for car in cars:
		car.throttle = 0.5
	var peak := [0.0, 0.0]
	var starts := [cars[0].global_position, cars[1].global_position]
	var moved_1s := [0.0, 0.0]
	for i in 240:
		await get_tree().physics_frame
		for k in 2:
			var s := 0.0
			for w in 4:
				s += cars[k].get_wheel_suction(w)
			peak[k] = maxf(peak[k], s)
			if i == 59:
				moved_1s[k] = (cars[k].global_position - starts[k]).length()
	var moved := [(cars[0].global_position - starts[0]).length(), (cars[1].global_position - starts[1]).length()]
	var left := 0.0
	for w in 4:
		left += cars[0].get_wheel_suction(w)
	for k in 2:
		print("  %s: sank %.3f m (creep %.3f m) in 5 h; suction %.2f kN at the pull; %.2f m in 1 s, %.2f m in 4 s" % ["settling" if k == 0 else "settling off", res[k][0], res[k][1], peak[k] / 1000.0, moved_1s[k], moved[k]])
	_check("a car left five hours in wet mud settles deeper by creep", res[0][1] > 0.01 and res[0][0] > res[1][0] + 0.01, "%.3f m against %.3f m" % [res[0][0], res[1][0]])
	_check("suction holds it as it pulls away, so it gets going later", peak[0] > 200.0 and moved_1s[0] < moved_1s[1], "%.2f kN; %.2f m against %.2f m in the first second" % [peak[0] / 1000.0, moved_1s[0], moved_1s[1]])
	_check("as it rolls out of its hollow the suction lets go", moved[0] > 0.5 and left < 0.3 * peak[0], "%.2f m, %.2f kN left" % [moved[0], left / 1000.0])
	for car in cars:
		car.queue_free()
	WorldContracts.clear_overrides()


# Puddled clay (mud round 23.1): two equal hollows in wet clay soil, one kneaded by ten wheel passes; the same water
# poured into each (rain on a wallow or a rut). The kneaded one's floor is sealed: its water stands days longer.
func _run_seal_tests() -> void:
	near_on = false
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.75, 0.0)
	surface.mud_layer = false
	surface.standing_water = true
	surface.worn_tracks = true
	var a := Vector3(-1.2, 0, 0.2)
	var b := Vector3(1.2, 0, 0.2)
	for p in [a, b]:
		surface.press_box(p, Vector2(0.6, 0.6), 0.0, 0.0, 0.1)
	for i in 10:
		surface.press_wheel(a + Vector3(0, -0.1, 0), Vector2(0.25, 0.3), 0.0, 0.005, Vector2(0, 1))
		for f in 20:
			await get_tree().physics_frame
	var seal_a := surface.get_seal_at(a)
	var seal_b := surface.get_seal_at(b)
	surface.refresh_ground()
	for f in 5:
		await get_tree().physics_frame
	surface.sync_deformation()
	surface.runoff_time_scale = 100.0
	for p in [a, b]:
		surface.add_water(p, 0.25, 0.015)
	surface.settle(0.05, 0.05)
	var a0 := _water_volume(a, 0.6).x
	var b0 := _water_volume(b, 0.6).x
	surface.settle(1200.0, 1.0)
	var a1 := _water_volume(a, 0.6).x
	var b1 := _water_volume(b, 0.6).x
	surface.runoff_time_scale = 1.0
	print("  seal %.2f kneaded, %.2f not; water %.1f -> %.1f l kneaded, %.1f -> %.1f l not, after 33 h" % [seal_a, seal_b, a0 * 1000.0, a1 * 1000.0, b0 * 1000.0, b1 * 1000.0])
	_check("wheels knead wet clay into a seal", seal_a > 0.5 and seal_b == 0.0, "%.2f against %.2f" % [seal_a, seal_b])
	_check("a sealed hollow holds its rain far longer", a0 > 0.005 and b0 > 0.005 and a1 > 0.5 * a0 and a1 > 2.0 * b1, "%.1f l against %.1f l left" % [a1 * 1000.0, b1 * 1000.0])
	near_on = true


# The terrain's drawn relief (WorldGround.relief_at): a 30 cm bulge drawn over x > 2.
class ReliefStub extends WorldGround:
	func _relief_at(p: Vector3) -> Vector2:
		return Vector2(0.3, 1.0) if p.x > 2.0 else Vector2.ZERO


# The ground sits on the drawn surface (round 23, terrain 57b253b5): where the terrain draws relief over its
# heightfield, prints and contact take its height, not the heightfield's.
func _run_relief_tests() -> void:
	WorldContracts.clear_overrides()
	WorldContracts.set_ground(ReliefStub.new())
	await _setup("flat", GroundSurface3D.GROUND_SOIL, 0.3, 0.0)
	surface.refresh_ground()
	for i in 10:
		await get_tree().physics_frame
	var off := surface.get_ground_height_at(Vector3(-3, 0, 0))
	var on := surface.get_ground_height_at(Vector3(4, 0, 0))
	print("  ground height %.3f m off the relief, %.3f m on it" % [off, on])
	_check("the ground stands on the terrain's drawn relief", absf(on - off - 0.3) < 0.02, "%.3f m higher" % (on - off))
	WorldContracts.clear_overrides()

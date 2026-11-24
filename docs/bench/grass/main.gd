extends Node3D
# GrassField3D: blades around the camera on gentle hills with a pond (no grass under water),
# wind bending them, density thinning with distance.
#
# godot --path docs/bench/grass --rendering-driver vulkan -- [options]
#   --test           checks, print PASS/FAIL, quit
#   --raster         raster only (default path traced)
#   --spp=N          path tracing samples (default 16)
#   --wind=x,z       fallback wind in m/s (default 3,1); 0,0 for still grass
#   --weather        a WeatherMap3D drives the wind instead
#   --crop=NAME      a field of wheat, barley, oats or rye instead of the meadow (GrassField3D.crop),
#                    400 tillers per m2 in rows 12.5 cm apart (--grass-density sets the tillers)
#   --stage=N        the crop's growth stage, BBCH (default 75: milky ripe; 99 harvested stubble)
#   --crop-close     the crop seen from 1.1 m, 2.5 m off its ears
#   --fields         three fields (GrassField3D.fields) seen from 16 m up: ripe with tramlines every 24 m
#                    and an 8 m headland, one at stem elongation drilled across, one harvested stubble
#                    with the combine's straw swaths; meadow round them, kept out of the fields
#   --fields=stubble the same, from 1.7 m at the stubble's edge, over its swaths
#   --frames=N       frames before the capture (default 20; path traced, more until no surface waits for its first capture)
#   --grass-density=N  blades per m2 of the tall grass near the camera (default: the field's, 120)
#   --sun-yaw=D      sun heading in degrees (default 150, against the light; -30 lights the view from behind)
#   --sun-elev=D     sun elevation in degrees (default 28)
#   --ambient=K      the sky light's energy times K
#   --sun-energy=E   the sun's energy (1; a scene's may be brighter)
#   --shadow-max=M   the sun's shadow distance (default 100; a landscape's is thousands of metres)
#   env GRASS_DIAG=noshadow|nosun|nograss|notrample|novf|vf1|nounder|bounce0|ssaa: lighting diagnostics (grass.md, "Light inside the sward")
#   --cpu-fill       build grass tiles on the CPU instead of the GPU fill (grass.md, stage 3a)
#   --litter-wind=x,z  downwind lean of the leaf drift (VegetationField3D.litter_wind), e.g. 0.6,0
#   --day=D          the season of day D from a WeatherMap3D: the sward's greenness, flowering,
#                    ferns (docs/ideas/vegetation-seasons.md)
#   --drought        with --day: a dry climate (300 mm a year, falling in winter), for a summer drought
#   --frost          with --day: a dry continental climate (January -8 C, July 18 C, little snow), for a hard frost
#   --pollen-cloud   a meadow in flower shedding pollen in a dry 10 m/s wind (PollenCloud3D; use --day=165, --frames)
#   --river          river scout: a PhysXRiver3D down a valley with a floodplain; its flood mark
#                    (WorldWater.high_water_at) lays wrack lines and combs the grass. Seen from the bank.
#   --flood=H[,D]    a flood peaked H m over the pond D days ago (default 10): wrack lines along the mark, the grass
#                    under it combed flat and silted (WorldWater.high_water_at through a script stub)
#   --seed-down      with --day (late summer, e.g. 245): thistledown loosed by a 10 m/s dry wind (SeedDown3D),
#                    seen from beside a thistle in seed
#   --snowy          with --day: a snowy winter (January -6 C, July 18 C, 1100 mm): a long lie, then the sward after
#                    the melt (GrassField3D.snowmelt); prints GRASS_MELT (days lain, days since the melt, matting)
#   --hay=D          the meadow round the camera mown D days ago (GrassField3D.hay_meadows): 0 swaths, 1-2 tedded,
#                    3 windrows, 4 and on stubble growing back; --hay-rain=MM: rained on while it lay (dull, bleached)
#   --out=path.png   save the last frame
#   --clip=dir       save every frame from 20 on as PNGs (for a GIF)
#   --layers=0       only the tall blades (no undercoat, ground tint, view facing, clover, flowers)
#   --close          the closer view used by --trample
#   --ruts           with a GroundSurface3D of wet soil: a pair of tyre ruts in the close view; the blades lie
#                    flat or are torn out where the ground is pressed (docs/ideas/mud.md)
#   --ruts=worn      a worn-in track instead (mud round 14.2): two wheel lines worn by 60 passes near the eye down
#                    to 2 further off, no ruts; the sward lies flat, thins and goes bare with the passes
#   --ruts=mud       the same, with clay thrown onto the sward beside the left rut and muddy water splashed beside
#                    the right one (mud round 7: mud on the blades and plants)
#   --notex          plants without their generated leaf textures
#   --forest         a wood of stand-in trees: forest floor (litter, moss, bilberry, fern) in
#                    their shade, meadow in the open (PlantSpecies.shade_range)
#   --forest=in      the same, standing inside the wood
#   --stand=S        the wood's trees and their floor set (PlantGenerator.make_floor): beech, oak,
#                    spruce (montane), pine (heath), larch (subalpine) or mixed (beech and spruce);
#                    the ground's litter follows the crowns. Use with --forest=in.
#   --swamp          wetland zonation round the pond: water lilies afloat, bulrush and reed in the
#                    shallows, soft rush on the wet shore (docs/ideas/plant-generator.md)
#   --swamp=close    the same, looking down on a water lily
#   --turf=0         without the near turf layer
#   --floor=0        without the litter on the ground under the crowns (cost comparison)
#   --sward-shade=0  with --forest: the grass grows through the bracken and bramble stands (before shot)
#   --rocks=N        N boulders (lumpy weathered rocks) 3 to 10 m in front of the camera; the fields keep
#                    out of them (obstacles) unless --rocks-through
#   --storm[=H]      a wet 12 m/s gale from +x for H hours (default 1) before the shot, with the wind still blowing:
#                    the meadow lodged in swirled patches (GrassField3D.lodging, docs/ideas/vegetation.md "Storm lodging")
#   --fairy          looks at the meadow fairy ring nearest the default view (GrassField3D.fairy_rings)
#   --grazed         the tall grass grazed short (5 to 14 cm), as on upland pasture
#   --path[=P]       a worn path winding away from the camera, P passes a year (default 3000, a busy footpath; GrassField3D.paths,
#                    VegetationField3D.paths): the sward and plants thin and shorten towards its line
#   --deer-wood=P    a woodland clearing ringed by TreeGenerator's real trees, from 1.6 m inside it across to the far
#                    edge, under deer browsing pressure P over the clearing and the wood (WorldStand.add_browse): P > 0
#                    the deer wood (STAND_DEER_WOOD: the browse line under the crowns) with make_floor's browse set, the
#                    sward cropped; P = 0 the beech stand unbrowsed (the before shot)
#   --taa            temporal antialiasing on (still shots: give it 20 frames or more)
#   --trails         game trails (VegetationField3D.make_game_trails between five places over the meadow, published on
#                    WorldStand.set_trails): the sward and plants worn along them, from 1.6 m on the busiest stretch
#   --graze=P        a deer browse disc of pressure P (WorldStand.add_browse, 6.5 m) 6 m ahead, framed from 1.6 m: a
#                    grazed lawn edged by the standing sward
#   --burn=A         a burn scar A years old (WorldBurnScar3D, 20 x 12 m) in the view: the sward grows back over
#                    GrassField3D.burn_recovery (0.7 years), short and sparse first; GRASS_NO_FLUSH=1 without the
#                    regrowth flush (the before frame)
#   GRASS_FIBRE=ab   with --out: also saves out_nofibre.png, the same frame with GrassField3D.fibre_shading 0
#   --undergrowth-edge  with --undergrowth: its edge from 1 m up, 3.5 m along it (--no-margin: no margin)
#   --no-rock-foot   with --rocks: the floor species without PlantSpecies.rock_foot
#   --undergrowth=bracken|bramble   a stand of it in the open meadow, framed from 5 m and 3 m up in side light; the sward
#                    thins under it (GrassField3D.undergrowth_field), unless --sward-shade=0
#   --far=0          without the plants' colour on the ground beyond their range (before/after)
#   --far            a high view over the land, to see plants thin out and the ground take over
#   --dolly          from that corner, walk in 0.3 m a frame (with --clip and --frames)
#   --terrain-material  the ground drawn with TerrainMaterial3D (its grass tint calls grass_ground())
#   --clearing       a closed wood with a clearing opened by VegetationField3D.density_mask, seen
#                    from 40 m up across its edge; --clearing=0 the same wood and camera, no mask
#   --lone           scattered open-grown trees over the meadow (no litter mat under a lone
#                    crown), seen from the --clearing camera
#   --far-cover      with --far-match: the far ground from the cover map (VegetationField3D.cover_grass) blended
#                    in as TerrainMaterial3D's far blend does, instead of grass_ground()'s far sward
#   --far-match      the default view twice, the blades out to 22-32 m and then the ground's far
#                    colour there instead (radius 10 m): prints both mean colours and their ratio
#                    (GRASS_VIEW_MATCH=0: without the live-view far match; GRASS_DIAG=viewtime: per-frame times)
#   --trample        a ball rolls and a walker strides through the grass, leaving trails
#   --far-grass[=off|rings]  a meadow to the horizon from 1.7 m (farmland ground): the far rings out to 250 m and
#                    the voxel shell past them (GrassField3D.far_rings, far_shell; off: neither, rings: no shell),
#                    prints ring blades, shell columns, tiles, build time,
#                    memory, primitives, draw calls and the frame time (docs/ideas/grass.md, "Far grass")
#   CROP_FAR_ONLY=1  with --crop: no tillers, the fields' far canopy everywhere (match its mean against
#                    the tillers' over the same ground, for crop_canopy_look())

var SIZE := 96.0
var test := false
var raster := false
var spp := 16
var wind := Vector2(3, 1)
var weather := false
var storm_hours := 0.0
var fairy := false
var frames := 20
var out := ""
var clip := ""
var n := 0
var dm: TerrainDataMap3D
var grass: GrassField3D
var season_day := 0
var drought := false
var frost := false
var snowy := false
var seed_down := false
var pollen_cloud := false
var hay_days := -1.0
var hay_rain := 0.0 # --hay-rain=MM: rain on the cut grass while it lay
var flood := Vector2(-1, 10) # --flood: metres over the pond, days since
var season_map: WeatherMap3D
var bench_season := {}
var cam: Camera3D
var fails := 0
var trample := false
var layers := true
var crop_name := "" # --crop=wheat|barley|oats|rye
var crop_stage := 75.0
var crop_close := false
var crop_fields := false
var stubble_view := false # --fields=stubble: from 1.7 m at the stubble field's edge, over its swaths.
var close := false
var ruts_view := false
var ruts_worn := false
var ruts_ground: GroundSurface3D
var ruts_mud := false # --ruts=mud: clay thrown beside the left rut, muddy water splashed beside the right (mud round 7)
var ruts_aged := 0.0 # --ruts=aged[:scale]: kept, then read back by a new ground with this ruts_time_scale (weeks away)
var forest := false
var litter_wind := Vector2()
var grass_density := 0.0
var gpu_fill := true
var sun_yaw := 150.0
var sun_elev := 28.0
var sun_node: DirectionalLight3D
var forest_in := false
var stand := "beech"
var stand_set := false
var no_floor := false
var no_sward := false
var undergrowth := ""
var grazed := false # --grazed: the tall grass grazed short (upland pasture).
var path_passes := 0.0 # --path: a worn path across the meadow, passes a year.
var burn_age := -1.0 # --burn=A: a burn scar A years old in the view.
var graze := -1.0 # --graze=P: a browse disc of pressure P in the view.
var deer_wood := -1.0 # --deer-wood=P: a clearing in real trees under browsing pressure P.
var trails_view := false # --trails
var deer_trees: Array = [] # Its TreeGenerator species (logs, stumps and saplings too): all kept out of the clearing.
var ug_dir := Vector3(0, 0, 1)
var mushroom_view := "" # --mushrooms[=name]: in the wood, close on a group of mushrooms (make_floor's).
var mounds_view := "" # --mounds[=meadow|wood]: PlantGenerator.make_mounds, from 1.6 m, a few metres off the nearest group.
var ug_edge := false # --undergrowth-edge: the stand's edge seen along it from 1 m up.
var no_rock_foot := false # --no-rock-foot: the floor species without their rock_foot.
var no_margin := false # --no-margin: undergrowth stands without their margin.
var rocks := 0 # --rocks=N: boulders in front of the camera.
var rocks_through := false # --rocks-through: the fields not told of them (blades and plants grow through).
var ug_field: VegetationField3D
var no_far := false
var far_view := false
var prims := false
var far_grass := 0 # --far-grass: 1 with the far rings and shell, 2 without (--far-grass=off), 3 rings only (=rings).
var far_mem0 := 0
var no_impostors := false
var clearing := -1 # -1 off, 0 the wood without its mask, 1 with it.
const CLEARING_C := Vector2(0.0, 24.0)
var dolly := false
var swamp := false
var swamp_close := false
var ground_mat: ShaderMaterial
var turf: GrassField3D
var no_turf := false
var terrain_mat := false
var far_cover := false # --far-cover
var notex := false
var t_frames := 0
var under: GrassField3D
var cover: VegetationField3D
var far_match := false
var ambient := 1.0 # --ambient=k: the sky light's energy times k (a scene's sky may light far more than the bench's)
var sun_energy := 1.0 # --sun-energy=e: the sun's energy
var shadow_max := 0.0 # --shadow-max: the sun's shadow distance (0: the light's default)
var far_a := Color()
var far_w := Color()
var ball: StaticBody3D
var walker: Node3D
var t := 0.0
var phase := 0
var phase_t := 0.0
const BALL_R := 0.45
# Tall grass in patches (lawn-like short grass and clover between them).
const TALL_PATCH := 7.0
const TALL_COVER := 0.65

# A wet summer gale from +x (12 m/s at 2 m, soaked, 20 mm/h of rain), or calm and dry (storm false).
class StormWeather extends WorldWeather:
	var storm := true

	func _wind_at(_p: Vector3, _h: float) -> Vector3:
		return Vector3(12, 0, 0) if storm else Vector3(0.5, 0, 0)

	func _surface_wetness_at(_p: Vector3) -> float:
		return 1.0 if storm else 0.0

	func _rainfall_at(_p: Vector3) -> float:
		return 20.0 if storm else 0.0

	func _precipitation_at(_p: Vector3) -> float:
		return _rainfall_at(_p)


class Pond extends Node3D:
	func get_water_level() -> float:
		return -0.6

static var farmland := false # --fields: gently rolling farmland, no pond.
static var river := false # --river: a valley with a meandering channel, no pond.
var river_node: Node3D

static func river_x(z: float) -> float:
	return 2.5 * sin(z * 0.035)

static func height(x: float, z: float) -> float:
	if farmland:
		# Rolling farmland: swells several metres high, slopes up to about 10%.
		return 3.5 * sin(x * 0.028 + 0.6) * cos(z * 0.024) + 2.0 * sin((x - z) * 0.012) + 0.4 * sin(x * 0.06 + z * 0.05) + 6.0
	if river:
		# A channel 1.1 m deep and ~5 m wide, a floodplain rising 5% to the valley sides, falling 0.6% downstream (+z).
		var d := absf(x - river_x(z))
		var bed := -0.006 * z - 1.1
		return bed + 1.1 * smoothstep(1.5, 4.5, d) + 0.05 * maxf(d - 4.5, 0.0) + 0.25 * sin(x * 0.08) * cos(z * 0.06) * smoothstep(4.0, 10.0, d)
	return 0.6 * sin(x * 0.08) * cos(z * 0.06) + 0.4 * sin((x + z) * 0.05) + 0.02 * (x * x + (z + 12.0) * (z + 12.0)) / 40.0 - 0.6

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test":
			test = true
			raster = true # Checks only: no need to path trace every frame.
		elif a == "--raster": raster = true
		elif a == "--weather": weather = true
		elif a == "--fairy": fairy = true
		elif a.begins_with("--storm"): storm_hours = float(a.get_slice("=", 1)) if a.contains("=") else 1.0
		elif a == "--trample": trample = true
		elif a == "--layers=0": layers = false
		elif a.begins_with("--crop="):
			crop_name = a.substr(7)
			layers = false
		elif a.begins_with("--stage="): crop_stage = float(a.substr(8))
		elif a == "--crop-close":
			crop_close = true
			farmland = true # Crop to the horizon: no pond, no bench hills.
			SIZE = 640.0
		elif a == "--fields" or a == "--fields=stubble":
			stubble_view = a == "--fields=stubble"
			crop_fields = true
			farmland = true
			SIZE = 640.0 # Fields and the meadow round them to the horizon.
		elif a == "--close": close = true
		elif a.begins_with("--far-grass"):
			far_grass = 2 if a == "--far-grass=off" else (3 if a == "--far-grass=rings" else 1)
			farmland = true
			SIZE = 640.0
		elif a == "--ruts" or a.begins_with("--ruts=aged") or a == "--ruts=mud" or a == "--ruts=worn":
			ruts_view = true
			close = true
			ruts_mud = a == "--ruts=mud"
			ruts_worn = a == "--ruts=worn"
			if a.begins_with("--ruts=aged"):
				ruts_aged = 6e5 if a == "--ruts=aged" else float(a.substr(12))
		elif a == "--forest": forest = true
		elif a == "--swamp": swamp = true
		elif a == "--swamp=close":
			swamp = true
			swamp_close = true
		elif a == "--turf=0": no_turf = true
		elif a == "--floor=0": no_floor = true
		elif a == "--sward-shade=0": no_sward = true
		elif a.begins_with("--undergrowth="): undergrowth = a.substr(14)
		elif a.begins_with("--rocks="): rocks = int(a.substr(8))
		elif a == "--rocks-through": rocks_through = true
		elif a == "--no-margin": no_margin = true
		elif a == "--undergrowth-edge": ug_edge = true
		elif a == "--grazed": grazed = true
		elif a == "--path": path_passes = 3000.0
		elif a.begins_with("--path="): path_passes = float(a.get_slice("=", 1))
		elif a.begins_with("--burn="): burn_age = float(a.get_slice("=", 1))
		elif a.begins_with("--graze="): graze = float(a.get_slice("=", 1))
		elif a == "--no-rock-foot": no_rock_foot = true
		elif a == "--far=0": no_far = true
		elif a == "--far": far_view = true
		elif a == "--prims": prims = true
		elif a == "--far-match": far_match = true
		elif a.begins_with("--shadow-max="): shadow_max = float(a.substr(13))
		elif a.begins_with("--ambient="): ambient = float(a.substr(10))
		elif a.begins_with("--sun-energy="): sun_energy = float(a.substr(13))
		elif a == "--impostors=0": no_impostors = true
		elif a == "--lone":
			forest = true
			clearing = 2
		elif a == "--trails": trails_view = true
		elif a == "--taa": get_viewport().use_taa = true
		elif a.begins_with("--deer-wood="):
			deer_wood = float(a.get_slice("=", 1))
			graze = deer_wood
			forest = true
			stand = "beech"
			stand_set = true
			clearing = 1
			SIZE = 144.0 # The wood all round to the eye's horizon.
		elif a == "--clearing" or a == "--clearing=0":
			forest = true
			clearing = 0 if a.ends_with("=0") else 1
		elif a == "--dolly":
			far_view = true
			dolly = true
		elif a == "--terrain-material": terrain_mat = true
		elif a == "--far-cover": far_cover = true
		elif a == "--forest=in":
			forest = true
			forest_in = true
		elif a == "--notex": notex = true
		elif a.begins_with("--mushrooms"): mushroom_view = a.substr(12) if a.length() > 12 else "any"
		elif a.begins_with("--mounds"):
			mounds_view = a.substr(9) if a.length() > 9 else "meadow"
			if mounds_view == "wood":
				forest = true
				stand = "spruce"
		elif a.begins_with("--grass-density="): grass_density = float(a.substr(16))
		elif a == "--cpu-fill": gpu_fill = false
		elif a.begins_with("--sun-yaw="): sun_yaw = float(a.substr(10))
		elif a.begins_with("--sun-elev="): sun_elev = float(a.substr(11))
		elif a.begins_with("--litter-wind="):
			var lw := a.substr(14).split(",")
			litter_wind = Vector2(float(lw[0]), float(lw[1]))
		elif a.begins_with("--stand="):
			stand = a.substr(8)
			stand_set = true
			forest = true
		elif a.begins_with("--day="): season_day = int(a.substr(6))
		elif a == "--drought": drought = true
		elif a == "--frost": frost = true
		elif a == "--snowy": snowy = true
		elif a == "--seed-down": seed_down = true
		elif a == "--pollen-cloud": pollen_cloud = true
		elif a.begins_with("--hay="): hay_days = float(a.get_slice("=", 1))
		elif a.begins_with("--hay-rain="): hay_rain = float(a.get_slice("=", 1))
		elif a == "--river": river = true
		elif a.begins_with("--flood="):
			var fp := a.substr(8).split(",")
			flood = Vector2(float(fp[0]), float(fp[1]) if fp.size() > 1 else 10.0)
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--clip="): clip = a.substr(7)
		elif a.begins_with("--wind="):
			var p := a.substr(7).split(",")
			wind = Vector2(float(p[0]), float(p[1]))
	if season_day > 0:
		season_map = WeatherMap3D.new()
		season_map.name = "Season"
		if drought:
			var rc := ClimateRecipe.new()
			rc.precipitation = 300.0
			rc.precipitation_seasonality = 0.9
			rc.precipitation_peak_month = 0
			season_map.climate = rc
		if frost:
			var rf := ClimateRecipe.new()
			rf.temperature_january = -8.0
			rf.temperature_july = 18.0
			rf.precipitation = 120.0 # A dry continental winter: hard frost on bare ground, no snow to hide it.
			rf.precipitation_seasonality = 0.9
			rf.precipitation_peak_month = 6
			season_map.climate = rf
		if snowy:
			var rs := ClimateRecipe.new()
			rs.temperature_january = -6.0
			rs.temperature_july = 18.0
			rs.precipitation = 1100.0
			season_map.climate = rs
		season_map.season_day = season_day
		add_child(season_map)
		bench_season = season_map.get_season()
		print("GRASS_DAY %d %s" % [season_day, str(bench_season)])
	var w := int(SIZE) + 1
	var data := PackedFloat32Array()
	data.resize(w * w)
	for z in w:
		for x in w:
			data[z * w + x] = height(x - SIZE * 0.5, z - SIZE * 0.5)
	var shape := HeightMapShape3D.new()
	shape.map_width = w
	shape.map_depth = w
	shape.map_data = data
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = shape
	body.add_child(cs)
	add_child(body)
	var pond := Pond.new()
	pond.name = "Pond"
	if not river:
		add_child(pond)
	else:
		_add_river()
	if flood.x >= 0.0:
		var fs := FloodMarkStub.new()
		fs.peak = pond.get_water_level() + flood.x
		fs.days = flood.y
		WorldContracts.add_water_stub(fs)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = 192
	dm.water_sources = [] if river else [NodePath("../Pond")]
	add_child(dm)
	grass = GrassField3D.new()
	if OS.get_environment("GRASS_STILL") == "1":
		grass.still_shot = true
	grass.name = "Grass"
	if hay_days >= 0.0:
		grass.hay_meadows = [{"polygon": PackedVector2Array([Vector2(-90, -90), Vector2(90, -90), Vector2(90, 90), Vector2(-90, 90)]), "row_angle": 0.35, "days": hay_days, "rain": hay_rain}]
	grass.fallback_wind = wind
	grass.far_match_view = OS.get_environment("GRASS_VIEW_MATCH") != "0" # 0: without the live-view far match.
	if OS.get_environment("GRASS_RADIUS") != "":
		grass.radius = float(OS.get_environment("GRASS_RADIUS")) # Which level of detail the far-match region sees.
	if grazed:
		grass.blade_height = Vector2(0.05, 0.14) # Grazed pasture: bracken stands over a short sward.
	if grass_density > 0.0:
		grass.density = grass_density
	if OS.get_environment("GRASS_DIAG").contains("vf1"):
		grass.view_facing = 1.0
	grass.gpu_fill = gpu_fill
	if crop_name != "":
		grass.crop = {"wheat": GrassField3D.CROP_WHEAT, "barley": GrassField3D.CROP_BARLEY, "oats": GrassField3D.CROP_OATS, "rye": GrassField3D.CROP_RYE}[crop_name]
		grass.crop_stage = crop_stage
		grass.density = grass_density if grass_density > 0.0 else 400.0
		grass.drill_angle = deg_to_rad(20.0)
		grass.radius = 24.0
		grass.tile_size = 1.0 if crop_name == "oats" else 1.25 # Under the path tracer's merged BLAS cap (65536 triangles) at 400 tillers per m2.
		if crop_close:
			# One field over the whole map: past the tillers the ground carries the crop's own far canopy.
			var e := SIZE * 0.5 - 1.0
			# Oats: the field's edge just ahead, so the first panicles stand against the sky.
			var ez := 19.3 if crop_name == "oats" else e
			grass.fields = [{"polygon": PackedVector2Array([Vector2(-e, -e), Vector2(e, -e), Vector2(e, ez), Vector2(-e, ez)]), "drill_angle": deg_to_rad(20.0), "stage": crop_stage, "tramline_spacing": 0.0, "headland": 0.0}]
		if crop_fields:
			grass.radius = 50.0
			grass.density = grass_density if grass_density > 0.0 else 450.0 # Closed stands, as drilled (450 to 600 tillers per m2).
			# Three fields with grass margins between them: ripe wheat, wheat at stem elongation drilled
			# across it, harvested stubble behind.
			grass.fields = [
				# Old field boundaries: no two sides parallel, edges that bend where a hedge or track ran.
				{"polygon": PackedVector2Array([Vector2(-60, -3), Vector2(-31, 1), Vector2(-5, -1), Vector2(-3, -27), Vector2(-7, -57), Vector2(-38, -61), Vector2(-71, -52), Vector2(-68, -25)]), "drill_angle": deg_to_rad(92.0), "stage": crop_stage, "tramline_spacing": 24.0, "headland": 8.0},
				{"polygon": PackedVector2Array([Vector2(4, 0), Vector2(33, -4), Vector2(61, -2), Vector2(70, -33), Vector2(63, -64), Vector2(30, -59), Vector2(5, -60), Vector2(2, -30)]), "drill_angle": deg_to_rad(8.0), "stage": 37.0, "tramline_spacing": 24.0, "headland": 8.0},
				{"polygon": PackedVector2Array([Vector2(-72, -66), Vector2(-20, -68), Vector2(30, -66), Vector2(71, -70), Vector2(80, -98), Vector2(66, -121), Vector2(10, -114), Vector2(-48, -118), Vector2(-79, -96)]), "drill_angle": deg_to_rad(-30.0), "stage": 99.0, "tramline_spacing": 24.0, "headland": 0.0},
			]
	if far_grass > 0:
		grass.far_rings = far_grass != 2
		grass.far_shell = far_grass == 1
		far_mem0 = OS.get_static_memory_usage()
	add_child(grass)
	grass.data_map = grass.get_path_to(dm)
	if crop_fields:
		# The meadow round the fields (it keeps out of them by itself).
		var meadow := GrassField3D.new()
		meadow.name = "Meadow"
		meadow.fallback_wind = wind
		meadow.radius = 50.0
		add_child(meadow)
		meadow.data_map = meadow.get_path_to(dm)
	if season_map:
		grass.season_source = grass.get_path_to(season_map)
		if snowy:
			print("GRASS_MELT ", grass.get_snowmelt_at(Vector3(0, height(0.0, 0.0), 0)))
	if layers:
		var tall := PlantSpecies.new()
		tall.patch_scale = TALL_PATCH
		tall.patch_cover = TALL_COVER
		tall.patch_seed = 5
		grass.rules = tall
		# Undercoat (the carpet): short two-segment blades, dense and near only.
		under = GrassField3D.new()
		under.name = "Undercoat"
		under.fallback_wind = wind
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
		# The ground under the carpet is the sward's base (GrassField3D.get_ground_shader_code()):
		# the blades' root colour, streaks and thatch, so a gap shows more grass, not soil.
		under.ground_tint = Color(0.05, 0.085, 0.025, 1.0)
		grass.ground_tint_enabled = false # The undercoat grows everywhere, so it owns the tint.
		under.hay_meadows = grass.hay_meadows # Every layer of the sward is mown.
		add_child(under)
		if season_map:
			under.season_source = under.get_path_to(season_map)
		under.data_map = under.get_path_to(dm)
		# Turf: a third, very dense and very short grid near the camera only (Serious Sam 4's
		# overlapped density grids), where gaps between the longer blades show the most.
		turf = GrassField3D.new()
		turf.name = "Turf"
		turf.fallback_wind = wind
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
		turf.hay_meadows = grass.hay_meadows
		add_child(turf)
		if season_map:
			turf.season_source = turf.get_path_to(season_map)
		turf.data_map = turf.get_path_to(dm)
		turf.visible = not no_turf
		cover = VegetationField3D.new()
		cover.far_volume = false # Not under test here.
		cover.name = "Cover"
		cover.populate_on_ready = false
		cover.tile_size = 4.0
		cover.biome = _cover_biome()
		if flood.x >= 0.0 or river:
			var fsp: Array[PlantSpecies] = []
			# Ahead of the ground cover: wrack lies over the sward, which keeps clear of it, not the reverse.
			fsp.assign(PlantGenerator.make_flood_wrack(3, true, {"flood_age": (flood.y if not river else 40.0) / 365.25}))
			fsp.append_array(cover.biome.species)
			if not river:
				print("GRASS_FLOOD peak %.2f" % (-0.6 + flood.x))
			cover.biome.species = fsp
		if OS.get_environment("GRASS_STILL") == "1":
			cover.still_shot = true # Still shots: stop streaming once settled.
		cover.forest_floor_enabled = not no_floor
		cover.litter_wind = litter_wind
		if clearing >= 0:
			_set_clearing()
		add_child(cover)
		cover.data_map = cover.get_path_to(dm)
		if far_cover:
			# The terrain's far cover map from the sward's layers (tall patches over the undercoat).
			cover.cover_map_enabled = true
			cover.cover_grass = cover.get_path_to(grass)
			cover.cover_grass_layers = [cover.get_path_to(under)]
		if forest:
			# The sward stays out from under the crowns.
			for g in [grass, under, turf]:
				var r: PlantSpecies = g.rules if g.rules else PlantSpecies.new()
				# A wide fade: the sward thins out over a few metres under the crown edge.
				r.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.3))
				r.set_rule_fade(PlantSpecies.RULE_SHADE, 0.4)
				g.rules = r
				g.canopy_field = g.get_path_to(cover)
				g.undergrowth_field = g.get_path_to(cover) # Thinned under the bracken and bramble stands.
		if graze >= 0.0:
			var st: WorldStand = WorldContracts.get_stand()
			st.clear_browse()
			if deer_wood >= 0.0:
				st.add_browse(Vector3(CLEARING_C.x, 0.0, CLEARING_C.y), 60.0, deer_wood, 1.8) # The clearing and the wood round it.
			else:
				st.add_browse(Vector3(24.0, 0.0, 22.0) if far_view else Vector3(5.0, 0.0, 14.0), 6.5, graze, 1.8)
		if trails_view:
			_set_trails.call_deferred()
		if burn_age >= 0.0:
			var scar := WorldBurnScar3D.new()
			scar.age_years = burn_age
			scar.size = Vector2(20, 12)
			scar.position = Vector3(24.0, 0.0, 22.0) if far_view else Vector3(3.0, 0.0, 9.0)
			add_child(scar)
		if path_passes > 0.0:
			# A footpath from beside the camera, bending away over the meadow: a line of points
			# along a gentle S, 1.2 m trodden width.
			var pts := PackedVector3Array()
			for k in 40:
				var t := k / 39.0
				var z := lerpf(23.0, -40.0, t)
				pts.append(Vector3(5.5 - 7.0 * t + 2.5 * sin(t * 5.0), 0.0, z))
			var worn := [{"points": pts, "width": 1.2, "passes": path_passes}]
			for g in [grass, under, turf]:
				if g:
					g.paths = worn
			cover.paths = worn
		if undergrowth != "" and not forest:
			# One undergrowth species in the open meadow, in irregular stands (its patches).
			var usp := PlantSpecies.new()
			var bramble := undergrowth == "bramble"
			usp.meshes = _variants(PlantGenerator.PRESET_BRAMBLE if bramble else PlantGenerator.PRESET_BRACKEN, 4)
			usp.spacing = 1.2 if bramble else 0.6
			usp.footprint = 0.0
			usp.density = 0.95
			# Open-ground bracken grows tall (1.5 to 2 m in full light), well above the sward.
			usp.scale_range = Vector2(1.4, 1.8) if not bramble else Vector2(1.0, 1.3)
			usp.patch_scale = 9.0
			usp.patch_cover = 0.22
			usp.patch_seed = 71
			usp.sward_shade = 0.0 if no_sward else (0.7 if bramble else 0.85)
			# Its litter under the stands, as PlantGenerator.make_floor() sets it (dead fronds, leaf litter).
			if not no_sward:
				usp.litter_color = Color(0.17, 0.105, 0.05, 0.7) if bramble else Color(0.30, 0.13, 0.045, 0.95)
			# The stand's margin, as make_floor() sets it (--no-margin: every plant full size, upright).
			if not no_margin:
				usp.margin_width = 2.0 if bramble else 3.0
				usp.margin_scale = 0.7 if bramble else 0.55
				usp.margin_lean = 15.0 if bramble else 20.0
			var ub := Biome.new()
			ub.species = [usp]
			ug_field = VegetationField3D.new()
			ug_field.far_volume = false # Not under test here.
			ug_field.name = "Undergrowth"
			ug_field.populate_on_ready = false
			ug_field.tile_size = 8.0
			ug_field.biome = ub
			add_child(ug_field)
			ug_field.data_map = ug_field.get_path_to(dm)
			for g in [grass, under, turf]:
				if g:
					g.undergrowth_field = g.get_path_to(ug_field)
	else:
		grass.view_facing = 0.0
		grass.ground_tint_enabled = false
	if storm_hours > 0.0:
		WorldContracts.set_weather(StormWeather.new())
		grass.advance_lodging.call_deferred(storm_hours * 3600.0)
	if weather:
		var wm := WeatherMap3D.new()
		wm.wind_speed = wind.length()
		add_child(wm)

	# Soil, tinted towards the grass where grass grows (GrassField3D.get_ground_shader_code()).
	var gm := ShaderMaterial.new()
	var gs := Shader.new()
	gs.code = "shader_type spatial;\n" + GrassField3D.get_ground_shader_code() + """
varying vec3 wp;
// --far-cover: TerrainMaterial3D's far blend over the cover map, from far_from metres.
global uniform sampler2D terrain_cover_map : hint_default_black, filter_linear;
global uniform vec4 terrain_cover_rect;
uniform float far_from = 1e9;
// Under the crowns grass_ground() lays the forest floor's litter (VegetationField3D's
// forest_floor_* globals): leaves, needles and moss by the stand above.
void vertex() { wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	// Bare ground: a brown loam (worn paths show it), dull.
	vec3 a = vegetation_far(wp, grass_ground(wp, vec3(0.13, 0.1, 0.065)), CAMERA_POSITION_WORLD);
	float spec = grass_ground_specular(wp, 0.3);
	float t = smoothstep(far_from, far_from + 2.0, length(wp - CAMERA_POSITION_WORLD));
	vec2 cuv = (wp.xz - terrain_cover_rect.xy) / max(terrain_cover_rect.zw, vec2(1e-3));
	if (terrain_cover_rect.z > 0.0 && t > 0.0 && all(greaterThanEqual(cuv, vec2(0.0))) && all(lessThanEqual(cuv, vec2(1.0)))) {
		vec4 cv = textureLod(terrain_cover_map, cuv, 0.0);
		vec3 cs = cv.rgb;
		vec3 cl = mix(cs / 12.92, pow((cs + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), cs));
		vec2 to = wp.xz - grass_ground_view.xz;
		if (grass_ground_sun.z > 0.0 && length(to) > 1e-3) {
			float c1 = dot(normalize(to), grass_ground_sun.xy);
			cl *= max(1.0 + grass_ground_sun.z * (grass_ground_far_lobe1.rgb * c1 + grass_ground_far_lobe2.rgb * (2.0 * c1 * c1 - 1.0)), vec3(0.1));
		}
		cl *= grass_ground_far_scale.rgb;
		a = mix(a, mix(a, cl, cv.a), t);
		spec = mix(spec, mix(0.5, 0.1, cv.a), t);
	}
	ALBEDO = a;
	ROUGHNESS = 0.95;
	SPECULAR = spec;
}
"""
	gm.shader = gs
	ground_mat = gm
	var ground := MeshInstance3D.new()
	ground.mesh = _ground_mesh()
	ground.material_override = gm
	if terrain_mat:
		ground.material_override = TerrainMaterial3D.new()
	add_child(ground)
	if ruts_view:
		ground.position.y = -0.2 # Under the GroundSurface3D's own ground, which carries the ruts.
	var water := MeshInstance3D.new()
	var wp := PlaneMesh.new()
	wp.size = Vector2(SIZE, SIZE)
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.05, 0.12, 0.15)
	wmat.roughness = 0.05
	wp.material = wmat
	water.mesh = wp
	water.position.y = -0.6
	if deer_wood < 0.0: # The deer wood's view looks away from the pond: no lake plane to the horizon.
		add_child(water)
	var env := Environment.new()
	# A sky, not a flat colour: raster ambient and path traced sky light then come from the same place.
	var sky := Sky.new()
	var psm := ProceduralSkyMaterial.new()
	sky.sky_material = psm
	if deer_wood >= 0.0:
		# Below the horizon the wood goes on: a dark wood floor, not a grey void.
		psm.ground_horizon_color = Color(0.1, 0.12, 0.07)
		psm.ground_bottom_color = Color(0.06, 0.05, 0.03)
	if farmland:
		# The sky's ground half as the land round the farm, not a dark void below the map's edge.
		psm.ground_horizon_color = psm.sky_horizon_color
		psm.ground_bottom_color = Color(0.17, 0.22, 0.12)
		psm.ground_curve = 0.3
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = ambient
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR if far_match else Environment.TONE_MAPPER_AGX # Linear: colours compare as they are.
	env.pathtracing_enabled = not raster
	env.pathtracing_sky_shader_enabled = true
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun_node = sun
	sun.rotation_degrees = Vector3(-sun_elev, sun_yaw, 0)
	sun.light_energy = sun_energy
	sun.shadow_enabled = not OS.get_environment("GRASS_DIAG").contains("noshadow")
	if shadow_max > 0.0:
		sun.directional_shadow_max_distance = shadow_max
	if OS.get_environment("GRASS_DIAG").contains("ssaa"):
		get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		get_viewport().scaling_3d_scale = 2.0
	if OS.get_environment("GRASS_DIAG").contains("nounder"):
		_hide_under.call_deferred()
	if OS.get_environment("GRASS_DIAG").contains("nograss"):
		_hide_grass.call_deferred()
	if OS.get_environment("GRASS_DIAG").contains("notrample"):
		_no_trample.call_deferred()
	if OS.get_environment("GRASS_DIAG").contains("novf"):
		_no_view_facing.call_deferred()
	if OS.get_environment("GRASS_DIAG").contains("nosun"):
		sun.light_energy = 0.0
	if OS.get_environment("GRASS_DIAG").contains("bounce0"):
		env.pathtracing_max_bounces = 0
	add_child(sun)
	# Movers: a ball (radius from its SphereShape3D) and a walker (radius from its meta).
	ball = StaticBody3D.new()
	var bcs := CollisionShape3D.new()
	var bs := SphereShape3D.new()
	bs.radius = BALL_R
	bcs.shape = bs
	ball.add_child(bcs)
	var bm := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = BALL_R
	sm.height = BALL_R * 2.0
	var bmat := StandardMaterial3D.new()
	bmat.albedo_color = Color(0.7, 0.2, 0.15)
	sm.material = bmat
	bm.mesh = sm
	ball.add_child(bm)
	ball.add_to_group("grass_trample")
	ball.collision_layer = 2 # Not ground: the data map's height rays must not hit it.
	ball.position = Vector3(0, 50, 0)
	add_child(ball)
	walker = Node3D.new()
	walker.set_meta("trample_radius", 0.25)
	walker.add_to_group("grass_trample")
	walker.position = Vector3(0, 50, 0)
	var wm2 := MeshInstance3D.new()
	var cm := CapsuleMesh.new()
	cm.radius = 0.22
	cm.height = 1.7
	var cmat := StandardMaterial3D.new()
	cmat.albedo_color = Color(0.25, 0.27, 0.32)
	cm.material = cmat
	wm2.mesh = cm
	wm2.position.y = 0.85
	walker.add_child(wm2)
	add_child(walker)
	if test:
		grass.trample_recovery = 0.5

	cam = Camera3D.new()
	# GRASS_EYE=h: the eye h metres up instead of 1.7 (with --far-match: the sward seen more steeply).
	var eye_h := float(OS.get_environment("GRASS_EYE")) if OS.get_environment("GRASS_EYE") != "" else 1.7
	cam.position = Vector3(6, height(6, 20) + eye_h, 20)
	add_child(cam)
	cam.look_at(Vector3(0, -0.5 - 0.6 * (eye_h - 1.7), -8))
	if graze >= 0.0 and not far_view:
		cam.position = Vector3(6, height(6, 20) + 1.6, 20)
		cam.look_at(Vector3(4.5, height(4.5, 11.0), 11.0))
	if flood.x >= 0.0:
		# The mark: the first point down the view's line under the flood's peak; seen from 3.5 m up the slope.
		var peak := -0.6 + flood.x
		var m := Vector2(6, 20)
		for i in 400:
			var q := Vector2(6, 20).lerp(Vector2(0, -8), i / 400.0)
			if height(q.x, q.y) < peak:
				m = q
				break
		var back := m + (Vector2(6, 20) - m).normalized() * 2.5 + Vector2(1.0, 0)
		cam.position = Vector3(back.x, height(back.x, back.y) + 1.6, back.y)
		cam.look_at(Vector3(m.x, peak - 0.1, m.y - 0.5))
	if crop_fields:
		cam.position = Vector3(0, height(0, 14) + 16.0, 14)
		cam.look_at(Vector3(0, height(0, -30), -30))
	if stubble_view:
		cam.position = Vector3(-6, height(-6, -71) + 1.7, -71)
		cam.look_at(Vector3(-10, height(-10, -80), -80))
	if crop_close:
		# Oats and rye stand taller: the eye above their heads.
		var eye := 1.1 if crop_name in ["wheat", "barley"] else (1.3 if crop_name == "oats" else 1.6)
		cam.position = Vector3(6, height(6, 20) + eye, 20)
		cam.look_at(Vector3(5.2, height(5.2, 17.6) + 0.75, 17.6))
		if crop_name == "oats":
			# At the field's edge, level with the panicles, the nearest half a metre off: they hang from
			# whorled branches against the sky.
			cam.position = Vector3(6, height(6, 19.75) + 0.95, 19.75)
			cam.look_at(Vector3(5.5, height(5.5, 18.0) + 1.0, 18.0))
	if (trample or close) and not test:
		# Closer, looking down on the trails.
		cam.position = Vector3(1.5, height(1.5, 17.0) + 1.6, 17.0)
		cam.look_at(Vector3(0.5, height(0.5, 10.0), 10.0))
	if ruts_view:
		ruts_ground = GroundSurface3D.new()
		ruts_ground.name = "Ruts"
		cam.name = "Eye"
		ruts_ground.follow_target = NodePath("../" + str(cam.name))
		ruts_ground.ground_type = GroundSurface3D.GROUND_SOIL
		ruts_ground.moisture = 0.7
		ruts_ground.keep_ruts = ruts_aged > 0.0
		ruts_ground.ruts_path = "user://grass_bench_ruts"
		if ruts_aged > 0.0:
			ruts_ground.clear_saved_ruts()
		ruts_ground.wind_ripples = false
		ruts_ground.desiccation_cracks = false
		ruts_ground.world_puddles = false
		add_child(ruts_ground)
	if far_view and not test:
		# From a corner, a little up, across the whole map: near plants, thinning, ground beyond.
		cam.position = Vector3(42, height(42, 42) + (2.0 if dolly else 7.0), 42)
		cam.look_at(Vector3(-20, height(-20, -20) - 2.0, -20))
	if far_grass > 0:
		# Across the meadow to the horizon, from standing height.
		cam.position = Vector3(0, height(0, 280) + 1.7, 280)
		cam.look_at(Vector3(0, height(0, 0) + 1.0, 0))
		cam.far = 1200.0
	if swamp and not test:
		# Walk from the usual view towards the deepest water until the shore, stand back from it.
		var from := Vector2(6, 20)
		var to := Vector2(-17, -6)
		var shore := to
		for k in 200:
			var q := from.lerp(to, k / 200.0)
			if height(q.x, q.y) < -0.6:
				shore = q
				break
		var dir := (to - from).normalized()
		var eye := shore - dir * 9.0 + Vector2(dir.y, -dir.x) * 1.5
		cam.position = Vector3(eye.x, height(eye.x, eye.y) + 2.6, eye.y)
		cam.look_at(Vector3(shore.x + dir.x * 6.0, -0.6, shore.y + dir.y * 6.0))
	if fairy:
		# The meadow fairy ring nearest the default view (GrassField3D.fairy_rings), from 8 m off and 4.5 m up.
		var best := Vector2(6, 20)
		var bd := 1e9
		for z in range(-90, 91):
			for x in range(-90, 91):
				var b: Vector3 = GrassField3D.get_fairy_ring_at(Vector3(x, 0, z))
				var d := Vector2(x, z).distance_to(Vector2(6, 20))
				if b.x > 0.8 and d < bd and height(x, z) > -0.3:
					bd = d
					best = Vector2(x, z)
		var away := (Vector2(6, 20) - best).normalized() if bd > 0.5 else Vector2(0, 1)
		var eye := best + away * 8.0
		cam.position = Vector3(eye.x, height(eye.x, eye.y) + 4.5, eye.y)
		cam.look_at(Vector3(best.x - away.x * 3.0, height(best.x, best.y), best.y - away.y * 3.0))
		print("GRASS_FAIRY ring front at ", best)

# A closed wood (no tree patches) and, with the mask, an irregular clearing about 22 m across:
# the trees read the mask's red channel, so they stay out where it is 0, and the meadow, the
# sward and the edge plants fill the light (their shade rules) without a mask of their own.
func _set_clearing() -> void:
	for sp in cover.biome.species:
		var ps := sp as PlantSpecies
		if ps.canopy_radius > 0.0 or deer_trees.has(ps):
			ps.patch_scale = 0.0
			ps.mask_channel = 0 if clearing == 1 else -1
			if clearing == 2:
				ps.density = 0.1
	if clearing != 1:
		return
	var res := 192
	var img := Image.create_empty(res, res, false, Image.FORMAT_RGBA8)
	for y in res:
		for x in res:
			var w := Vector2((x + 0.5) / res * SIZE - SIZE * 0.5, (y + 0.5) / res * SIZE - SIZE * 0.5) - CLEARING_C
			var a := atan2(w.y, w.x)
			var r := (24.0 if deer_wood >= 0.0 else 11.0) + 2.5 * sin(3.0 * a) + 1.5 * sin(5.0 * a + 1.0)
			var v := smoothstep(r - 1.5, r + 1.5, w.length())
			img.set_pixel(x, y, Color(v, v, v, 1.0))
	cover.density_mask = ImageTexture.create_from_image(img)
	cover.density_mask_rect = Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE)

# Game trails between five places round the meadow (clear of the pond), published for every field; the eye on the
# busiest stretch, 1.6 m up, looking 12 m along it.
func _set_trails() -> void:
	var goals := PackedVector3Array([Vector3(-30, 0, 30), Vector3(25, 0, 32), Vector3(30, 0, -5), Vector3(-28, 0, -2), Vector3(4, 0, 40)])
	var trails: Array = VegetationField3D.make_game_trails(dm, Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE), goals, {"seed": 2})
	var st: WorldStand = WorldContracts.get_stand()
	st.set_trails(trails)
	var best: Dictionary = {}
	var best_score := -1.0
	for t in trails:
		var pts: PackedVector3Array = t["points"]
		var score: float = t["passes"] * minf(pts.size(), 40)
		if pts.size() > 12 and score > best_score:
			best_score = score
			best = t
	print("GRASS_TRAILS %d pieces, busiest %.0f passes" % [trails.size(), best.get("passes", 0.0)])
	if best.is_empty():
		return
	var bp: PackedVector3Array = best["points"]
	var e := bp[2]
	var k := 2
	while k < bp.size() - 1 and Vector2(bp[k].x - e.x, bp[k].z - e.z).length() < 12.0:
		k += 1
	cam.position = Vector3(e.x, height(e.x, e.z) + 1.6, e.z)
	cam.look_at(Vector3(bp[k].x, height(bp[k].x, bp[k].z) + 0.2, bp[k].z))

# 40 m up beside the clearing, looking down across it to its far edge and the wood beyond.
func _frame_clearing() -> void:
	if deer_wood >= 0.0:
		# At eye height (1.6 m) in the clearing's near half, looking across it to the far edge: the trees' browse line
		# and the cropped sward between.
		# In the clearing's south half (trunks 48 m apart, the edge trees' crowns leaning in to leave about 30 m of open sky), clear of the rim's
		# crowns, looking over it to the north edge, tipped up for the sky.
		var e2 := CLEARING_C + Vector2(1.0, -6.0)
		var t2 := CLEARING_C + Vector2(-1.0, 20.0)
		cam.fov = 75.0
		cam.position = Vector3(e2.x, height(e2.x, e2.y) + 1.6, e2.y)
		cam.look_at(Vector3(t2.x, height(t2.x, t2.y) + 1.6 + 26.0 * tan(deg_to_rad(19.0)), t2.y))
		return
	# From above the pond side, steep and narrow, so the wood frames the clearing and neither
	# the pond nor the map's edge is in view.
	var eye := CLEARING_C + Vector2(0.0, -13.0)
	var at := CLEARING_C + Vector2(0.0, 3.0)
	cam.fov = 50.0
	cam.position = Vector3(eye.x, height(eye.x, eye.y) + 40.0, eye.y)
	cam.look_at(Vector3(at.x, height(at.x, at.y), at.y))

# An undergrowth stand in the open meadow (--undergrowth): the most compact stand near the usual
# view with tall grass all round it, seen across it in side light from 5 m and 3 m up (at eye height the fronds are
# seen edge-on and sink into the grass).
func _frame_undergrowth() -> void:
	var usp: PlantSpecies = ug_field.biome.species[0]
	var tall: PlantSpecies = grass.rules
	var best := Vector3()
	var best_score := -1.0
	for gz in range(-20, 21):
		for gx in range(-20, 21):
			var c := Vector3(gx * 2.0, 0, gz * 2.0)
			if usp.get_patch_weight(c) < 0.99 or dm.get_water_depth_at(c) > 0.0:
				continue
			# Stand inside (radius 1.5 m), sward round it (a ring at 5 to 7 m), tall grass everywhere.
			var score := 0.0
			for k in 24:
				var a := k * TAU / 24.0
				var dir := Vector3(cos(a), 0, sin(a))
				var inner := c + dir * 1.5
				var ring := c + dir * (5.0 + (k % 3))
				score += usp.get_patch_weight(inner)
				score += 1.0 - usp.get_patch_weight(ring)
				if tall and tall.get_patch_weight(ring) < 0.99:
					score -= 2.0
				if dm.get_water_depth_at(ring) > 0.0:
					score -= 4.0
			if score > best_score:
				best_score = score
				best = c
	# Side light: look across the sun's heading.
	var sun_dir := -sun_node.global_transform.basis.z
	var side := Vector3(-sun_dir.z, 0, sun_dir.x).normalized()
	var eye := best + side * _env_f("UG_DIST", 5.0)
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + _env_f("UG_EYE", 3.0), eye.z)
	cam.look_at(Vector3(best.x, height(best.x, best.z) + _env_f("UG_AT", 0.2), best.z))
	print("GRASS_UNDERGROWTH stand=%s score=%.1f eye=%s sward=%.2f" % [str(best), best_score, str(cam.position), ug_field.get_sward_shade_at(best)])
	if ug_edge:
		_frame_undergrowth_edge(best)

# The stand's edge, looked along: walk from the stand's centre towards the light side to where the
# patch ends, then stand 3.5 m along the edge at 1 m eye height, looking back along it, so the
# fronds' drop towards the grass shows against what is behind.
func _frame_undergrowth_edge(c: Vector3) -> void:
	var usp: PlantSpecies = ug_field.biome.species[0]
	# The straightest long stretch of the stand's edge within 30 m: points where the depth crosses 0,
	# scored by how far the stand runs on (depth > 0.5 m, 1.5 m in) and the meadow stays open (depth
	# < 0, 1 m out) along it.
	var best_score := -1
	var e := c
	var out := Vector3(1, 0, 0)
	for gx in range(-30, 31):
		for gz in range(-30, 31):
			var p := c + Vector3(gx, 0, gz)
			var d0 := usp.get_patch_depth(p)
			if absf(d0) > 0.15:
				continue
			var h := 0.3
			var g := Vector3(usp.get_patch_depth(p + Vector3(h, 0, 0)) - usp.get_patch_depth(p - Vector3(h, 0, 0)), 0, usp.get_patch_depth(p + Vector3(0, 0, h)) - usp.get_patch_depth(p - Vector3(0, 0, h)))
			if g.length() < 1e-6:
				continue
			var o := (-g).normalized()
			for sgn: float in [-1.0, 1.0]:
				var al: Vector3 = Vector3(-o.z, 0, o.x) * sgn
				var score := 0
				for t in range(0, 14):
					var q: Vector3 = p + al * float(t)
					if usp.get_patch_depth(q - o * 1.5) > 0.5 and usp.get_patch_depth(q + o * 1.0) < 0.0:
						score += 1
					else:
						break
				# The meadow to the right of the view: out must be the view's right.
				var view := al
				if Vector3(-view.z, 0, view.x).dot(o) < 0.0:
					continue
				if score > best_score:
					best_score = score
					e = p
					out = o
					ug_dir = al
	# Stand 1.5 m out from the edge, a step back along it, 1 m up, looking along the edge.
	var eye := e + out * 1.5 - ug_dir * 1.0
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + 1.0, eye.z)
	var at := e + ug_dir * 7.0 - out * 1.2
	cam.look_at(Vector3(at.x, height(at.x, at.z) + 0.5, at.z))
	print("GRASS_UNDERGROWTH_EDGE edge=%s out=%s run=%d m eye=%s" % [str(e), str(out), best_score, str(cam.position)])

# --mushrooms[=preset index]: the group of mushrooms nearest the view (of that preset), from 0.9 m at
# 0.35 m up, the sun behind; the stand-in trunks only cast their shadows. Prints each species' count.
# --mounds: the nearest mound to the eye (an ant hill, a wood ant nest), seen from 1.6 m about 5 m off down-sun, so
# its group and the molehills round it are in the frame.
func _frame_mounds() -> void:
	var best := Vector3()
	var best_d := 1e9
	for si in cover.biome.species.size():
		var sp: PlantSpecies = cover.biome.species[si]
		if not sp.has_meta("sward_mound"):
			continue
		var pl := cover.get_placements(si)
		print("GRASS_MOUNDS species %d (%s) count %d" % [si, str(sp.get_meta("plant_preset", -1)), pl.size()])
		if float(sp.get_meta("sward_mound")) < 0.3:
			continue # Frame the ant hills and nests; the molehills come with them.
		for q in pl:
			var d := Vector2(q.x - cam.global_position.x, q.z - cam.global_position.z).length()
			if d < best_d:
				best_d = d
				best = q
	if best_d > 1e8:
		print("GRASS_MOUNDS none placed")
		return
	var f := -sun_node.global_transform.basis.z
	f.y = 0.0
	f = f.normalized()
	var eye := best - f * 5.0
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + 1.6, eye.z)
	cam.look_at(best + Vector3(0, 0.1, 0))

func _frame_mushrooms() -> void:
	var best := Vector3()
	var best_d := 1e9
	for si in cover.biome.species.size():
		var sp: PlantSpecies = cover.biome.species[si]
		if not sp.has_meta("mushroom"):
			continue
		var pl := cover.get_placements(si)
		var gen_preset := -1
		print("GRASS_MUSHROOMS species %d count %d density %.3f" % [si, pl.size(), sp.density])
		if mushroom_view != "any" and str(si) != mushroom_view:
			continue
		for q in pl:
			var d := Vector2(q.x - cam.global_position.x, q.z - cam.global_position.z).length()
			if d < best_d:
				best_d = d
				best = q
	if best_d > 1e8:
		print("GRASS_MUSHROOMS none placed")
		return
	var f := -sun_node.global_transform.basis.z
	f.y = 0.0
	f = f.normalized()
	var eye := best - f * 0.9
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + 0.35, eye.z)
	cam.look_at(best + Vector3(0, 0.06, 0))
	for ch in cover.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh and ch.multimesh.mesh and ch.multimesh.mesh.get_aabb().size.y > 4.0:
			ch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	print("GRASS_MUSHROOMS view at %s" % str(best))

# Boulders for --rocks: lumpy weathered rocks, a little sunk, in front of the camera. Each is a disc
# on the ground for the fields' obstacles, its radius where the rock meets the soil.
func _place_rocks() -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = 23
	var fwd := -cam.global_transform.basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right := Vector3(-fwd.z, 0, fwd.x)
	var list := PackedVector4Array()
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.42, 0.40, 0.37)
	mat.roughness = 0.9
	# Weathered granite: lichen-mottled albedo and a fine grain normal map, triplanar.
	var grain := FastNoiseLite.new()
	grain.frequency = 0.02
	grain.fractal_octaves = 5
	var ramp := Gradient.new()
	ramp.set_color(0, Color(0.30, 0.29, 0.27))
	ramp.set_color(1, Color(0.62, 0.61, 0.56))
	ramp.add_point(0.55, Color(0.45, 0.44, 0.40))
	ramp.add_point(0.8, Color(0.50, 0.52, 0.44))
	var alb := NoiseTexture2D.new()
	alb.noise = grain
	alb.color_ramp = ramp
	alb.seamless = true
	mat.albedo_color = Color(1, 1, 1)
	mat.albedo_texture = alb
	var nrm := NoiseTexture2D.new()
	nrm.noise = grain
	nrm.as_normal_map = true
	nrm.bump_strength = 6.0
	nrm.seamless = true
	mat.normal_enabled = true
	mat.normal_texture = nrm
	mat.uv1_triplanar = true
	mat.uv1_world_triplanar = true
	mat.uv1_scale = Vector3(0.6, 0.6, 0.6)
	var lumps := FastNoiseLite.new()
	lumps.frequency = 1.1
	lumps.fractal_octaves = 3
	for k in rocks:
		var d := rng.randf_range(1.5, 5.0) if forest else rng.randf_range(3.0, 10.0)
		var q := cam.global_position + fwd * d + right * rng.randf_range(-0.45, 0.45) * d
		var r := rng.randf_range(0.3, 1.0)
		if forest and k == 0:
			d = 2.6 # The one in the middle of the frame.
			q = cam.global_position + fwd * d
			r = 0.75
		var y := height(q.x, q.z)
		# A unit sphere made lumpy by noise (+-15%): rounded, weathered, no two alike.
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		sm.radial_segments = 48
		sm.rings = 24
		var arr := sm.get_mesh_arrays()
		var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		lumps.seed = 100 + k
		var widest := 0.0
		for i in verts.size():
			var v := verts[i]
			verts[i] = v * (1.0 + 0.15 * lumps.get_noise_3dv(v))
			widest = maxf(widest, Vector2(verts[i].x, verts[i].z).length())
		arr[Mesh.ARRAY_VERTEX] = verts
		arr[Mesh.ARRAY_NORMAL] = null
		arr[Mesh.ARRAY_TANGENT] = null
		var stl := SurfaceTool.new()
		stl.create_from_arrays(arr)
		stl.deindex()
		stl.generate_normals()
		stl.generate_tangents()
		stl.set_material(mat)
		var mi := MeshInstance3D.new()
		mi.mesh = stl.commit()
		# Semi-axes r, 0.55 r, r; centre 0.15 r up: the rock meets the ground at 0.96 r.
		mi.transform = Transform3D(Basis(Vector3.UP, rng.randf() * TAU).scaled(Vector3(r, 0.55 * r, r)), Vector3(q.x, y + 0.15 * r, q.z))
		add_child(mi)
		# Its keep-out: the widest the lumpy rock gets, so no stem stands under its bulge.
		list.append(Vector4(q.x, y, q.z, widest * r))
	if not rocks_through:
		for f in [grass, under, turf, cover, ug_field]:
			if f:
				f.obstacles = list
	if no_rock_foot and cover:
		for sp in cover.biome.species:
			sp.rock_foot = 0.0
	print("GRASS_ROCKS %d through=%s" % [list.size(), str(rocks_through)])

func _env_f(k: String, d: float) -> float:
	return float(OS.get_environment(k)) if OS.get_environment(k) != "" else d

# Stand at a wood's edge: the tree nearest the usual view, seen from the open side.
func _frame_forest() -> void:
	var trees := cover.get_placements(0)
	if trees.is_empty():
		return
	# The edge tree with the most edge plants (bramble, bracken, foxglove) around it, near the view.
	var edge: Array = []
	for k in [1, 2, 3]:
		edge.append_array(cover.get_placements(k))
	var best: Vector3 = trees[0]
	var best_score := -1.0
	for t in trees:
		if Vector2(t.x - 1.5, t.z - 17.0).length() > 35.0:
			continue
		var score := 0.0
		for e in edge:
			if Vector2(e.x - t.x, e.z - t.z).length() < 7.0:
				score += 1.0
		if score > best_score:
			best_score = score
			best = t
	# Darkest side: away from the other trees.
	var away := Vector3()
	for t in trees:
		var d := Vector3(best.x - t.x, 0, best.z - t.z)
		if d.length() > 0.1 and d.length() < 15.0:
			away += d.normalized() / d.length()
	away = away.normalized() if away.length() > 1e-3 else Vector3(0, 0, 1)
	var eye := best + away * 7.0
	var at := best - away * 3.0
	if forest_in:
		# Between trunks: the point inside the wood farthest from any of them.
		var side := Vector3(-away.z, 0, away.x)
		var far_d := 0.0
		for k in 24:
			var cand := best - away * (2.0 + (k % 6)) + side * (float(k / 6) * 1.5 - 2.25)
			var near_d := 1e9
			for t in trees:
				near_d = min(near_d, Vector2(t.x - cand.x, t.z - cand.z).length())
			if near_d > far_d:
				far_d = near_d
				eye = cand
		at = eye - away * 6.0
	if far_view:
		# Back out into the open: the edge's bracken and bramble 30 to 60 m off.
		eye = best + away * 38.0
		at = best
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + (2.5 if far_view else 1.5), eye.z)
	cam.look_at(Vector3(at.x, height(at.x, at.z) + (-0.4 if forest_in else 0.3), at.z))
	print("GRASS_FOREST tree=%s eye=%s shade_under=%.2f" % [str(best), str(cam.position), cover.get_shade_at(best)])

# Ground plants from PlantGenerator (three variants each), in patches (PlantSpecies.patch_*),
# drawn near only: wildflower drifts, plantain, clover between the tall grass, sedge by the water.
# --prims: triangles drawn in the frame, then again without each group of plants, to show
# where the triangles go (which plants would gain from cheaper far forms).
func _print_prims() -> void:
	var info := func(): return RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)
	print("PRIMS total=%d" % info.call())
	var imp_cells := 0
	for node in find_children("*", "MultiMeshInstance3D", true, false):
		if node.multimesh and node.multimesh.mesh and node.multimesh.mesh.has_meta("impostor_of"):
			imp_cells += 1
	print("PRIMS cells drawing impostors: %d" % imp_cells)
	if not raster:
		return # Path traced frames are too slow to repeat per group.
	var groups := {}
	for node in find_children("*", "MultiMeshInstance3D", true, false):
		var mmi := node as MultiMeshInstance3D
		if mmi.multimesh and mmi.multimesh.mesh:
			var m := mmi.multimesh.mesh
			var key: String = str(m.get_meta("preset")) if m.has_meta("preset") else (m.resource_name if m.resource_name != "" else _mesh_key(mmi))
			if not groups.has(key):
				groups[key] = []
			groups[key].append(mmi)
	for key in groups:
		for mmi in groups[key]: mmi.visible = false
		for i in 3: await RenderingServer.frame_post_draw
		print("PRIMS without %s (%d nodes): %d" % [key, groups[key].size(), info.call()])
		for mmi in groups[key]: mmi.visible = true
		var cam := get_viewport().get_camera_3d().global_position
		for far_d in [10.0, 20.0, 40.0]:
			var hidden := 0
			for mmi in groups[key]:
				var bb: AABB = mmi.global_transform * mmi.get_aabb()
				if bb.get_center().distance_to(cam) > far_d:
					mmi.visible = false; hidden += 1
			for i in 3: await RenderingServer.frame_post_draw
			print("PRIMS   %s beyond %d m (%d nodes): %d" % [key, far_d, hidden, info.call()])
			for mmi in groups[key]: mmi.visible = true
	for i in 3: await RenderingServer.frame_post_draw

func _mesh_key(mmi: MultiMeshInstance3D) -> String:
	var m := mmi.multimesh.mesh
	if not (mmi.get_parent() is VegetationField3D):
		return mmi.get_parent().get_class()
	var tris := 0
	for i in m.get_surface_count():
		var a := m.surface_get_arrays(i)
		tris += (a[Mesh.ARRAY_INDEX].size() if a[Mesh.ARRAY_INDEX] else a[Mesh.ARRAY_VERTEX].size()) / 3
	return "plant %.1fm %d surf %dk tris" % [m.get_aabb().size.y, m.get_surface_count(), tris / 1000]

func _variants(preset: int, count: int, size := 1.0) -> Array:
	var gen := PlantGenerator.new()
	gen.textures = not notex
	gen.preset = preset
	gen.size = size
	gen.apply_preset_colors()
	gen.season = bench_season
	var meshes := gen.generate_variants(count)
	for m in meshes:
		m.set_meta("preset", preset)
	return meshes

# Far away the ground takes each plant's colour (PlantSpecies.far_color, as make_floor() sets it):
# its leaves a little darker, hiding more ground the taller it is. Litter and twigs are the
# ground's own already.
func _set_far_colors(b: Biome) -> void:
	for sp in b.species:
		var spc := sp as PlantSpecies
		if spc.meshes.is_empty() or spc.visibility_range <= 0.0 or spc.far_color.a > 0.0:
			continue
		var m: Mesh = spc.meshes[0]
		if not m.has_meta("preset"):
			continue
		var preset: int = m.get_meta("preset")
		if preset in [PlantGenerator.PRESET_LEAF_LITTER, PlantGenerator.PRESET_NEEDLE_LITTER, PlantGenerator.PRESET_TWIGS]:
			continue
		var gen := PlantGenerator.new()
		gen.preset = preset
		gen.apply_preset_colors()
		var lc: Color = gen.leaf_color * 0.8
		if preset in [PlantGenerator.PRESET_KNAPWEED, PlantGenerator.PRESET_SCABIOUS, PlantGenerator.PRESET_WILD_CARROT, PlantGenerator.PRESET_SORREL]:
			lc = lc.lerp(gen.petal_color, 0.35) # Tall flowers: their colour shows from afar.
		spc.far_color = Color(lc.r, lc.g, lc.b, clampf(m.get_aabb().end.y / 0.6, 0.2, 0.9))

func _cover_biome() -> Biome:
	if OS.get_environment("GRASS_FAR_HEATH") == "1":
		# GRASS_FAR_HEATH=1: dry heath (make_heath: heather, gorse, broom) instead of the meadow's herbs, for the far
		# shell's clumps and gaps.
		var hb := Biome.new()
		var hs: Array[PlantSpecies] = []
		hs.assign(PlantGenerator.make_heath(PlantGenerator.HEATH_DRY, 3, true))
		hb.species = hs
		return hb
	var flowers := PlantSpecies.new()
	flowers.meshes = _variants(PlantGenerator.PRESET_DAISY, 3) + _variants(PlantGenerator.PRESET_BUTTERCUP, 3) + _variants(PlantGenerator.PRESET_DANDELION, 3)
	flowers.spacing = 0.25
	flowers.footprint = 0.12
	flowers.density = 0.9
	flowers.scale_range = Vector2(1.1, 1.5)
	flowers.patch_scale = 6.0
	flowers.patch_cover = 0.35
	flowers.patch_seed = 11
	flowers.visibility_range = 15.0
	var sedge := PlantSpecies.new()
	sedge.meshes = _variants(PlantGenerator.PRESET_SEDGE, 3)
	sedge.spacing = 0.45
	sedge.footprint = 0.2
	sedge.density = 0.8
	sedge.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(0.0, 3.0))
	sedge.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.5)
	sedge.visibility_range = 30.0
	var plantain := PlantSpecies.new()
	plantain.meshes = _variants(PlantGenerator.PRESET_PLANTAIN, 3)
	plantain.spacing = 0.5
	plantain.footprint = 0.15
	plantain.density = 0.5
	plantain.patch_scale = 4.0
	plantain.patch_cover = 0.35
	plantain.patch_seed = 21
	plantain.visibility_range = 12.0
	# Clover where the tall grass is not: the complement of its patches.
	var clover := PlantSpecies.new()
	clover.meshes = _variants(PlantGenerator.PRESET_CLOVER, 3)
	clover.spacing = 0.22
	clover.footprint = 0.0
	clover.density = 0.8
	clover.scale_range = Vector2(0.8, 1.2)
	clover.patch_scale = TALL_PATCH
	clover.patch_cover = TALL_COVER
	clover.patch_seed = 5
	clover.patch_invert = true
	clover.visibility_range = 10.0
	# The rest of a real meadow (docs/ideas/plant-generator.md, "More species"): coarse grass
	# tussocks, weeds in patches, last year's dead grass under everything.
	var tussock := PlantSpecies.new()
	tussock.meshes = _variants(PlantGenerator.PRESET_COCKSFOOT, 3) + _variants(PlantGenerator.PRESET_TIMOTHY, 3)
	tussock.spacing = 0.7
	tussock.footprint = 0.2
	tussock.density = 0.45
	tussock.scale_range = Vector2(0.8, 1.15)
	tussock.patch_scale = TALL_PATCH
	tussock.patch_cover = TALL_COVER
	tussock.patch_seed = 5
	tussock.visibility_range = 25.0
	var nettle := PlantSpecies.new()
	nettle.meshes = _variants(PlantGenerator.PRESET_NETTLE, 3)
	nettle.spacing = 0.35
	nettle.footprint = 0.15
	nettle.density = 0.85
	nettle.patch_scale = 5.0
	nettle.patch_cover = 0.22
	nettle.patch_seed = 31
	nettle.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(1.0, 25.0)) # Moist, rich ground.
	nettle.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 4.0)
	nettle.visibility_range = 25.0
	var dock := PlantSpecies.new()
	dock.meshes = _variants(PlantGenerator.PRESET_DOCK, 3)
	dock.spacing = 0.9
	dock.footprint = 0.25
	dock.density = 0.5
	dock.patch_scale = 7.0
	dock.patch_cover = 0.35
	dock.patch_seed = 41
	dock.visibility_range = 20.0
	var thistle := PlantSpecies.new()
	thistle.meshes = _variants(PlantGenerator.PRESET_THISTLE, 3)
	thistle.set_meta("plant_preset", PlantGenerator.PRESET_THISTLE) # SeedDown3D finds the heads by it
	thistle.spacing = 1.6
	thistle.footprint = 0.25
	thistle.density = 0.35
	thistle.visibility_range = 20.0
	var yarrow := PlantSpecies.new()
	yarrow.meshes = _variants(PlantGenerator.PRESET_YARROW, 3)
	yarrow.spacing = 0.5
	yarrow.footprint = 0.1
	yarrow.density = 0.6
	yarrow.patch_scale = 5.0
	yarrow.patch_cover = 0.3
	yarrow.patch_seed = 51
	yarrow.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6.0, 1000000.0)) # Drier ground.
	yarrow.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	yarrow.visibility_range = 15.0
	var dry := PlantSpecies.new()
	dry.meshes = _variants(PlantGenerator.PRESET_DRY_GRASS, 3)
	dry.spacing = 0.3
	dry.footprint = 0.0
	dry.density = 0.55
	dry.scale_range = Vector2(0.7, 1.2)
	dry.visibility_range = 12.0
	# Tall meadow flowers (plant-generator.md): knapweed and sorrel through the meadow, scabious and
	# wild carrot on the drier ground; they stand clear of the grass and read from further away.
	var tall := PlantSpecies.new()
	tall.meshes = _variants(PlantGenerator.PRESET_KNAPWEED, 3) + _variants(PlantGenerator.PRESET_SORREL, 3)
	tall.spacing = 0.35
	tall.footprint = 0.08
	tall.density = 0.8
	tall.scale_range = Vector2(1.1, 1.4)
	tall.patch_scale = 7.0
	tall.patch_cover = 0.5
	tall.patch_seed = 61
	tall.visibility_range = 30.0
	var tall_dry := PlantSpecies.new()
	tall_dry.meshes = _variants(PlantGenerator.PRESET_SCABIOUS, 3) + _variants(PlantGenerator.PRESET_WILD_CARROT, 3)
	tall_dry.spacing = 0.4
	tall_dry.footprint = 0.08
	tall_dry.density = 0.75
	tall_dry.scale_range = Vector2(1.1, 1.4)
	tall_dry.patch_scale = 8.0
	tall_dry.patch_cover = 0.35
	tall_dry.patch_seed = 67
	tall_dry.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(8.0, 1000000.0))
	tall_dry.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 4.0)
	tall_dry.visibility_range = 30.0
	var b := Biome.new()
	b.species = [flowers, sedge, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry, tall, tall_dry]
	if OS.get_environment("GRASS_DIAG").contains("notall"):
		b.species = b.species.slice(0, 10)
	if graze >= 0.0:
		# Deer grazing (PlantSpecies.RULE_BROWSE, as make_floor's sets): the tall grasses and palatable herbs go where
		# the pressure is high; rosettes (plantain, clover, sedge) and the unpalatable nettle, dock and thistle stay.
		for sp in [flowers, tussock, yarrow, dry, tall, tall_dry]:
			(sp as PlantSpecies).set_rule_range(PlantSpecies.RULE_BROWSE, Vector2(0.0, 0.45))
			(sp as PlantSpecies).set_rule_fade(PlantSpecies.RULE_BROWSE, 0.25)
	if forest:
		for sp in b.species:
			(sp as PlantSpecies).set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.4))
			(sp as PlantSpecies).set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
		# Bigger plants first: the edge shrubs and bracken claim their space before the herbs.
		var floor_sp := _stand_floor()
		b.species = _stand_trees() + floor_sp.slice(0, 3) + b.species + floor_sp.slice(3)
	if mounds_view != "":
		# Ant hills and molehills (or wood ant nests): listed first, so the herbs make room round them.
		# In the wood, after the trees: the nests read the crowns' shade, needle share and edge.
		var k := 0
		while k < b.species.size() and (b.species[k] as PlantSpecies).canopy_radius > 0.0:
			k += 1
		var ms := Array(PlantGenerator.make_mounds(PlantGenerator.MOUNDS_WOOD if mounds_view == "wood" else PlantGenerator.MOUNDS_MEADOW, 3, not notex))
		b.species = b.species.slice(0, k) + ms + b.species.slice(k)
	if swamp:
		b.species = _wetland_species() + b.species
	if no_far:
		for sp in b.species:
			(sp as PlantSpecies).far_color = Color(0, 0, 0, 0)
	else:
		_set_far_colors(b)
	# Worn paths: each plant resists trampling by its growth form (PlantGenerator presets; make_*
	# species are tagged already, the bench's own are not).
	for sp in b.species:
		var ps := sp as PlantSpecies
		if ps.trample_resistance <= 0.0 and not ps.meshes.is_empty() and ps.meshes[0].has_meta("preset"):
			ps.trample_resistance = PlantGenerator.get_trample_resistance(ps.meshes[0].get_meta("preset"))
	return b

# Hydrosere zonation (Spence 1982; Keddy 2010): floating-leaved plants in deeper water,
# bulrush and reed in the shallows and a few metres onto wet land, soft rush on the
# wet shore. Water depth and distance come from the TerrainDataMap3D.
# Standing at the shore of the bench pond, as a person would, from a rise behind it.
func _wetland_species() -> Array:
	var lily := PlantSpecies.new()
	lily.meshes = _variants(PlantGenerator.PRESET_WATER_LILY, 3)
	lily.floating = true
	lily.spacing = 1.1
	lily.footprint = 0.5
	lily.density = 0.9
	lily.patch_scale = 6.0
	lily.patch_cover = 0.7
	lily.patch_seed = 81
	lily.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.35, 2.5))
	lily.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.2)
	lily.visibility_range = 50.0
	var cattail := PlantSpecies.new()
	cattail.meshes = _variants(PlantGenerator.PRESET_CATTAIL, 3)
	cattail.spacing = 0.7
	cattail.footprint = 0.35
	cattail.density = 0.9
	cattail.patch_scale = 5.0
	cattail.patch_cover = 0.5
	cattail.patch_seed = 83
	cattail.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.1, 0.5))
	cattail.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.05)
	cattail.visibility_range = 60.0
	var reed := PlantSpecies.new()
	reed.meshes = _variants(PlantGenerator.PRESET_REED, 3)
	reed.spacing = 0.6
	reed.footprint = 0.3
	reed.density = 0.95
	reed.patch_scale = 8.0
	reed.patch_cover = 0.6
	reed.patch_seed = 85
	# The bench pond is only 0.75 m deep: the bands are squeezed (reed to 0.45 m, not 1 to 2 m)
	# so every zone shows; the note lists the real depths.
	reed.set_rule_range(PlantSpecies.RULE_WATER_DEPTH, Vector2(0.0, 0.45))
	reed.set_rule_fade(PlantSpecies.RULE_WATER_DEPTH, 0.15)
	reed.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(-100.0, 3.0))
	reed.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 1.5)
	reed.visibility_range = 60.0
	var rush := PlantSpecies.new()
	rush.meshes = _variants(PlantGenerator.PRESET_RUSH, 3)
	rush.spacing = 0.8
	rush.footprint = 0.3
	rush.density = 0.8
	rush.patch_scale = 4.0
	rush.patch_cover = 0.5
	rush.patch_seed = 87
	rush.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(0.3, 9.0))
	rush.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 2.0)
	rush.visibility_range = 40.0
	return [lily, cattail, reed, rush]

# Stand-in trees (trunk and a crown blob) until stage 3b grows real ones: they only cast the shade.
func _tree_species() -> PlantSpecies:
	var st := SurfaceTool.new()
	var am := ArrayMesh.new()
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.18
	trunk.bottom_radius = 0.32
	trunk.height = 9.0
	var bark := StandardMaterial3D.new()
	bark.albedo_color = Color(0.17, 0.14, 0.11)
	bark.roughness = 0.9
	st.append_from(trunk, 0, Transform3D(Basis(), Vector3(0, 4.5, 0)))
	st.commit(am)
	am.surface_set_material(0, bark)
	var crown := SphereMesh.new()
	crown.radius = 3.6
	crown.height = 5.5
	var leaves := StandardMaterial3D.new()
	leaves.albedo_color = Color(0.09, 0.15, 0.05)
	leaves.roughness = 0.8
	st = SurfaceTool.new()
	st.append_from(crown, 0, Transform3D(Basis(), Vector3(0, 9.5, 0)))
	st.commit(am)
	am.surface_set_material(1, leaves)
	var tree := PlantSpecies.new()
	tree.meshes = [am]
	tree.spacing = 6.0
	tree.footprint = 1.0
	tree.density = 0.9
	tree.scale_range = Vector2(0.85, 1.15)
	tree.patch_scale = 30.0
	tree.patch_cover = 0.45
	tree.patch_seed = 3
	tree.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6.0, 1000000.0))
	tree.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	tree.canopy_radius = 3.6
	tree.canopy_opacity = 0.9
	return tree

# The floor for --stand from PlantGenerator.make_floor(); the bench's own set without one.
func _stand_floor() -> Array:
	var sets := {"beech": PlantGenerator.FLOOR_BEECH, "oak": PlantGenerator.FLOOR_OAK_MIXED,
			"spruce": PlantGenerator.FLOOR_MONTANE_CONIFER, "mixed": PlantGenerator.FLOOR_MONTANE_CONIFER,
			"pine": PlantGenerator.FLOOR_PINE_HEATH, "larch": PlantGenerator.FLOOR_SUBALPINE}
	if not stand_set or not sets.has(stand):
		return _floor_species()
	return Array(PlantGenerator.make_floor(sets[stand], 3, not notex, 0.0 if no_impostors else 15.0, bench_season))

# The wood's stand-in trees by --stand. Conifers set canopy_conifer, which the floor reads.
func _stand_trees() -> Array:
	if deer_wood >= 0.0:
		# Real trees (TreeGenerator.make_stand), one mesh a species, no near leaf cards (memory).
		var tg := TreeGenerator.new()
		tg.near_leaf_range = 0.0
		tg.near_needle_range = 0.0
		tg.fire_variants = false
		tg.lightning_variants = false
		var which := TreeGenerator.STAND_DEER_WOOD if deer_wood > 0.0 else TreeGenerator.STAND_BEECH
		var tb: Biome = tg.make_stand(which, 2)
		deer_trees = Array(tb.species)
		# The clearing's edge (make_edge, by the canopy's edge distance): unbrowsed, a mantle of shrubs and low-limbed
		# edge trees; under the deer, edge trees clipped to the browse line and no mantle.
		var eb: Biome = tg.make_edge(which, 1)
		deer_trees += Array(eb.species) # Edge trees and mantle too: none stands in the open clearing.
		return deer_trees
	match stand:
		"spruce", "larch": return [_conifer_species(false)]
		"pine": return [_conifer_species(true)]
		"mixed":
			var b := _tree_species()
			b.patch_seed = 5
			var c := _conifer_species(false)
			c.patch_seed = 3
			return [b, c]
	return [_tree_species()]

# Spruce: a dark narrow cone to the ground's shade, dense; pine: a high, flat, open crown on a
# long bare trunk, letting 10 to 30% of the light through.
func _conifer_species(pine: bool) -> PlantSpecies:
	var st := SurfaceTool.new()
	var am := ArrayMesh.new()
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.12 if pine else 0.1
	trunk.bottom_radius = 0.28
	trunk.height = 14.0 if pine else 12.0
	var bark := StandardMaterial3D.new()
	bark.albedo_color = Color(0.3, 0.17, 0.09) if pine else Color(0.16, 0.12, 0.1)
	bark.roughness = 0.9
	st.append_from(trunk, 0, Transform3D(Basis(), Vector3(0, trunk.height * 0.5, 0)))
	st.commit(am)
	am.surface_set_material(0, bark)
	var leaves := StandardMaterial3D.new()
	leaves.albedo_color = Color(0.07, 0.12, 0.05) if pine else Color(0.04, 0.08, 0.04)
	leaves.roughness = 0.8
	st = SurfaceTool.new()
	if pine:
		var crown := SphereMesh.new()
		crown.radius = 3.4
		crown.height = 3.0
		st.append_from(crown, 0, Transform3D(Basis(), Vector3(0, 14.5, 0)))
	else:
		var crown := CylinderMesh.new()
		crown.top_radius = 0.0
		crown.bottom_radius = 2.8
		crown.height = 10.0
		# Lower branches die off in a closed stand: the live crown starts a few metres up.
		st.append_from(crown, 0, Transform3D(Basis(), Vector3(0, 9.0, 0)))
	st.commit(am)
	am.surface_set_material(1, leaves)
	var tree := PlantSpecies.new()
	tree.meshes = [am]
	tree.spacing = 6.5 if pine else 5.0
	tree.footprint = 1.0
	tree.density = 0.9
	tree.scale_range = Vector2(0.85, 1.15)
	tree.patch_scale = 30.0
	tree.patch_cover = 0.45
	tree.patch_seed = 3
	tree.set_rule_range(PlantSpecies.RULE_WATER_DISTANCE, Vector2(6.0, 1000000.0))
	tree.set_rule_fade(PlantSpecies.RULE_WATER_DISTANCE, 3.0)
	tree.canopy_radius = 3.4 if pine else 2.8
	tree.canopy_opacity = 0.55 if pine else 0.95
	tree.canopy_conifer = true
	return tree

# Forest floor (docs/ideas/forest-floor.md; vegetation-placement.md, "Canopy shade rule"): litter everywhere in
# shade, bilberry and fern in drifts, moss in the deepest shade.
func _floor_species() -> Array:
	var fern := PlantSpecies.new()
	fern.meshes = _variants(PlantGenerator.PRESET_FERN, 3)
	fern.spacing = 0.9
	fern.footprint = 0.3
	fern.density = 0.9
	fern.scale_range = Vector2(1.3, 2.0)
	fern.patch_scale = 6.0
	fern.patch_cover = 0.6
	fern.patch_seed = 61
	fern.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.35, 1.0))
	fern.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	fern.visibility_range = 35.0
	var bilberry := PlantSpecies.new()
	bilberry.meshes = _variants(PlantGenerator.PRESET_BILBERRY, 3)
	bilberry.spacing = 0.35
	bilberry.footprint = 0.12
	bilberry.density = 0.9
	bilberry.patch_scale = 7.0
	bilberry.patch_cover = 0.6
	bilberry.scale_range = Vector2(1.0, 1.4)
	bilberry.patch_seed = 71
	bilberry.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.3, 1.0))
	bilberry.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	# Acid needle litter: bilberry carpets under pine and spruce, scarce under beech.
	bilberry.set_rule_range(PlantSpecies.RULE_CONIFER, Vector2(0.4, 1.0))
	bilberry.set_rule_fade(PlantSpecies.RULE_CONIFER, 0.3)
	bilberry.visibility_range = 25.0
	var moss := PlantSpecies.new()
	moss.meshes = _variants(PlantGenerator.PRESET_MOSS, 3)
	moss.spacing = 0.3
	moss.footprint = 0.0
	moss.density = 0.8
	moss.scale_range = Vector2(0.8, 1.6)
	moss.patch_scale = 4.0
	moss.patch_cover = 0.4
	moss.patch_seed = 81
	moss.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.6, 1.0))
	moss.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	moss.visibility_range = 15.0
	var litter := PlantSpecies.new()
	litter.meshes = _variants(PlantGenerator.PRESET_LEAF_LITTER, 3)
	litter.spacing = 0.2
	litter.footprint = 0.0
	litter.density = 0.6 # The ground's shader carries the leaf carpet; these give it relief.
	litter.scale_range = Vector2(1.2, 1.8)
	litter.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.3, 1.0))
	litter.set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
	litter.set_rule_range(PlantSpecies.RULE_CONIFER, Vector2(0.0, 0.5))
	litter.set_rule_fade(PlantSpecies.RULE_CONIFER, 0.15)
	litter.visibility_range = 15.0
	# Needles and cones over the duff under conifers. The feather moss carpet itself is the
	# ground's (its drifts in forest_floor()); a 3D carpet with an outline read as dark holes.
	var needles := PlantSpecies.new()
	needles.meshes = _variants(PlantGenerator.PRESET_NEEDLE_LITTER, 3)
	needles.spacing = 0.35
	needles.footprint = 0.0
	needles.density = 0.8
	needles.scale_range = Vector2(0.8, 1.3)
	needles.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.3, 1.0))
	needles.set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
	needles.set_rule_range(PlantSpecies.RULE_CONIFER, Vector2(0.5, 1.0))
	needles.set_rule_fade(PlantSpecies.RULE_CONIFER, 0.15)
	needles.visibility_range = 15.0
	var carpet := PlantSpecies.new()
	carpet.meshes = _variants(PlantGenerator.PRESET_FEATHER_MOSS, 3)
	carpet.spacing = 0.55
	carpet.footprint = 0.0
	carpet.density = 0.5
	carpet.scale_range = Vector2(0.9, 1.5)
	carpet.patch_scale = 5.0
	carpet.patch_cover = 0.5
	carpet.patch_seed = 83
	carpet.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.5, 1.0))
	carpet.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	carpet.set_rule_range(PlantSpecies.RULE_CONIFER, Vector2(0.5, 1.0))
	carpet.set_rule_fade(PlantSpecies.RULE_CONIFER, 0.2)
	carpet.visibility_range = 20.0
	# Fallen twigs in every stand.
	var twigs := PlantSpecies.new()
	twigs.meshes = _variants(PlantGenerator.PRESET_TWIGS, 3)
	twigs.spacing = 0.9
	twigs.footprint = 0.0
	twigs.density = 0.5
	twigs.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.3, 1.0))
	twigs.set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
	twigs.visibility_range = 18.0
	# Shade herbs: wood sorrel in patches, beech and spruce seedlings waiting for a gap.
	var sorrel := PlantSpecies.new()
	sorrel.meshes = _variants(PlantGenerator.PRESET_WOOD_SORREL, 3)
	sorrel.spacing = 0.25
	sorrel.footprint = 0.1
	sorrel.density = 0.9
	sorrel.patch_scale = 3.0
	sorrel.patch_cover = 0.35
	sorrel.patch_seed = 87
	sorrel.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.55, 1.0))
	sorrel.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	sorrel.visibility_range = 18.0
	var seedling := PlantSpecies.new()
	seedling.meshes = _variants(PlantGenerator.PRESET_SEEDLING, 4)
	seedling.spacing = 0.8
	seedling.footprint = 0.15
	seedling.density = 0.5
	seedling.patch_scale = 5.0
	seedling.patch_cover = 0.3
	seedling.patch_seed = 89
	seedling.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.4, 0.97))
	seedling.set_rule_fade(PlantSpecies.RULE_SHADE, 0.1)
	seedling.visibility_range = 30.0
	# Woodland edge (fringe and mantle): bracken stands, bramble and foxglove where the light
	# falls off under the outer crowns, between the meadow and the forest floor.
	var bracken := PlantSpecies.new()
	bracken.meshes = _variants(PlantGenerator.PRESET_BRACKEN, 3)
	bracken.spacing = 0.7
	bracken.footprint = 0.3
	bracken.density = 0.9
	bracken.scale_range = Vector2(0.9, 1.3)
	bracken.patch_scale = 9.0
	bracken.patch_cover = 0.55
	bracken.patch_seed = 91
	bracken.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.1, 0.8))
	bracken.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	bracken.visibility_range = 45.0
	bracken.sward_shade = 0.0 if no_sward else 0.85 # The grass thins out under the stands (undergrowth_field).
	var bramble := PlantSpecies.new()
	bramble.meshes = _variants(PlantGenerator.PRESET_BRAMBLE, 3)
	bramble.spacing = 1.4
	bramble.footprint = 0.6
	bramble.density = 0.8
	bramble.scale_range = Vector2(0.8, 1.2)
	bramble.patch_scale = 7.0
	bramble.patch_cover = 0.4
	bramble.patch_seed = 93
	bramble.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.05, 0.7))
	bramble.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
	bramble.visibility_range = 45.0
	bramble.sward_shade = 0.0 if no_sward else 0.7
	var foxglove := PlantSpecies.new()
	foxglove.meshes = _variants(PlantGenerator.PRESET_FOXGLOVE, 3)
	foxglove.spacing = 0.8
	foxglove.footprint = 0.2
	foxglove.density = 0.5
	foxglove.patch_scale = 6.0
	foxglove.patch_cover = 0.3
	foxglove.patch_seed = 97
	foxglove.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.1, 0.7))
	foxglove.set_rule_fade(PlantSpecies.RULE_SHADE, 0.1)
	foxglove.visibility_range = 40.0
	return [bramble, bracken, foxglove, fern, seedling, bilberry, sorrel, moss, twigs, litter, needles]

func _ground_mesh() -> Mesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var res := 96
	var step := SIZE / res
	for z in res:
		for x in res:
			var x0 := -SIZE * 0.5 + x * step
			var z0 := -SIZE * 0.5 + z * step
			var q := [Vector3(x0, height(x0, z0), z0), Vector3(x0 + step, height(x0 + step, z0), z0),
					Vector3(x0 + step, height(x0 + step, z0 + step), z0 + step), Vector3(x0, height(x0, z0 + step), z0 + step)]
			for i in [0, 1, 2, 0, 2, 3]:
				st.add_vertex(q[i])
	st.generate_normals()
	return st.commit()

func _fill_compare(field: GrassField3D, label: String) -> void:
	var gpu_set := _blade_keys(field)
	var t0 := Time.get_ticks_usec()
	field.gpu_fill = false
	for i in 20:
		field.update_at(cam.global_position)
	var cpu_ms := (Time.get_ticks_usec() - t0) / 1000.0
	var cpu_set := _blade_keys(field)
	var same := 0
	for k in gpu_set:
		if cpu_set.has(k): same += 1
	var match_share := float(same) / maxf(cpu_set.size(), 1.0)
	_check("GPU fill matches the CPU blades (%s)" % label, absf(gpu_set.size() - cpu_set.size()) < cpu_set.size() * 0.03 and match_share > 0.97, "gpu %d cpu %d shared %.3f" % [gpu_set.size(), cpu_set.size(), match_share])
	t0 = Time.get_ticks_usec()
	field.gpu_fill = true
	for i in 20:
		field.update_at(cam.global_position)
	print("GRASS_FILL_MS %s cpu=%.1f gpu=%.1f (main thread, whole field)" % [label, cpu_ms, (Time.get_ticks_usec() - t0) / 1000.0])

# Live blades of a field as keys (position to the millimetre, height and width to 0.1 mm).
func _blade_keys(field: GrassField3D) -> Dictionary:
	var keys := {}
	for ch in field.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			var buf := RenderingServer.multimesh_get_buffer(mm.get_rid())
			for i in mm.instance_count:
				var b := i * 16
				if buf[b] == 0.0 and buf[b + 2] == 0.0:
					continue
				keys[Vector4i(roundi(buf[b + 3] * 1000.0), roundi(buf[b + 7] * 1000.0), roundi(buf[b + 11] * 1000.0), roundi(buf[b + 12] * 10000.0))] = true
	return keys

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

# Hay meadows (GrassField3D.hay_meadows): days since mown inside a meadow, fixed or by the calendar (the season's day
# of the year, none before the mowing day); -1 outside.
func _hay_checks() -> void:
	var g := GrassField3D.new()
	var sq := PackedVector2Array([Vector2(0, 0), Vector2(20, 0), Vector2(20, 20), Vector2(0, 20)])
	g.hay_meadows = [{"polygon": sq, "row_angle": 0.3, "days": 2.0}, {"polygon": Transform2D(0.0, Vector2(40, 0)) * sq, "mow_day": 170}]
	var a := g.get_mown_days_at(Vector3(5, 0, 5))
	var b := g.get_mown_days_at(Vector3(50, 0, 5))
	var c := g.get_mown_days_at(Vector3(30, 0, 5))
	_check("hay meadows: days since mown inside a meadow (fixed 2; calendar without a season: not mown), -1 outside",
			a == 2.0 and b == -1.0 and c == -1.0 and g.hay_meadows.size() == 2, [a, b, c])
	g.free()

# The sward through the year (docs/ideas/vegetation-seasons.md, step 1): greenness from the
# season state's soil moisture and temperature, into the blades' and ground's dryness.
func _season_checks() -> void:
	var g := func(t: float, m: float) -> float:
		return GrassField3D.season_greenness({"temperature": t, "soil_moisture": m})
	_check("season: mild wet winter stays green", g.call(5.0, 0.9) > 0.95, g.call(5.0, 0.9))
	_check("season: hard winter goes tan", g.call(-7.0, 0.9) < 0.1, g.call(-7.0, 0.9))
	_check("season: summer drought browns it", g.call(20.0, 0.08) < 0.01, g.call(20.0, 0.08))
	_check("season: a dry spell half browns it", absf(g.call(20.0, 0.25) - 0.5) < 0.1, g.call(20.0, 0.25))
	_check("season: no season, as set", GrassField3D.season_greenness({}) == 1.0, GrassField3D.season_greenness({}))
	var base := grass.dryness
	grass.dryness = 0.2
	grass.season_response = 1.0
	# The field's own read of a WeatherMap3D, through the effective dryness.
	var wm := WeatherMap3D.new()
	wm.name = "Seasons"
	add_child(wm)
	grass.season_source = grass.get_path_to(wm)
	print("GRASS_SEASON day=%d season=%s green=%.3f dryness=%.3f" % [wm.get_day_of_year(), str(wm.get_season()), grass.get_season_green(), grass.get_effective_dryness()])
	grass.season_source = NodePath()
	wm.queue_free()
	grass.dryness = base

# Bracken stands shade the sward out (GrassField3D.undergrowth_field, PlantSpecies.sward_shade):
# under the stands the blades thin by the shade, in the open they are as before.
# Mounds (PlantGenerator.make_mounds) the undergrowth field places: their "sward_mound" discs keep the blades out, and
# the grass comes back when they go.
# Storm lodging (docs/ideas/vegetation.md, "Storm lodging"): an hour of a wet gale lays the tall meadow down in
# patches along the wind; ten calm, dry days later it stands again.
func _lodging_check(c: Vector2) -> void:
	var stub := StormWeather.new()
	WorldContracts.set_weather(stub)
	grass.advance_lodging(3600.0)
	var n := 0
	var sum := 0.0
	var low := 0
	var along := 0.0
	for z in range(-60, 61, 4):
		for x in range(-60, 61, 4):
			var l: Vector3 = grass.get_lodging_at(Vector3(c.x + x, 0, c.y + z))
			n += 1
			sum += l.x
			if l.x < 0.2:
				low += 1
			if l.x > 0.0:
				along += l.y * l.x
	var mean := sum / n
	along /= maxf(sum, 1e-4)
	var rect: Vector4 = grass.get_material().get_shader_parameter("lodge_rect")
	_check("lodging: a wet gale lays the meadow down in patches (mean > 0.3, some cells standing) along the wind (+x)", mean > 0.3 and low > 0 and low < n and along > 0.6 and rect.z > 0.0, "mean %.2f, standing %d of %d, along +x %.2f, map %s" % [mean, low, n, along, rect])
	stub.storm = false
	for d in 10:
		grass.advance_lodging(86400.0)
	var most := 0.0
	for z in range(-60, 61, 4):
		for x in range(-60, 61, 4):
			most = maxf(most, grass.get_lodging_at(Vector3(c.x + x, 0, c.y + z)).x)
	rect = grass.get_material().get_shader_parameter("lodge_rect")
	_check("lodging: ten calm dry days later the meadow stands again (most < 0.05, no map)", most < 0.05 and rect.z == 0.0, "most %.3f, map %s" % [most, rect])
	WorldContracts.set_weather(null)

func _mound_check(c: Vector2) -> void:
	var mf := VegetationField3D.new()
	mf.far_volume = false
	mf.populate_on_ready = false
	var sp := PlantSpecies.new()
	sp.meshes = [BoxMesh.new()]
	sp.spacing = 2.5
	sp.scale_range = Vector2(1.0, 1.0)
	sp.set_meta("sward_mound", 0.4)
	var mb := Biome.new()
	mb.species = [sp]
	mf.biome = mb
	add_child(mf)
	mf.data_map = mf.get_path_to(dm)
	mf.populate()
	var was := grass.undergrowth_field
	grass.undergrowth_field = grass.get_path_to(mf)
	var count := func() -> Array:
		var discs := PackedVector3Array()
		for q in mf.get_placements(0):
			if Vector2(q.x, q.z).distance_to(c) < 9.0: # Only the ones over the counted blades (GDScript loops are slow).
				discs.append(q)
		var inside := 0
		var by := 0
		for ch in grass.get_children(true):
			if ch is MultiMeshInstance3D:
				var mm: MultiMesh = ch.multimesh
				for i in mm.instance_count:
					var xf := mm.get_instance_transform(i)
					if xf.basis.x.length_squared() == 0.0 or Vector2(xf.origin.x, xf.origin.z).distance_to(c) > 8.0:
						continue
					for q in discs:
						var d := Vector2(xf.origin.x - q.x, xf.origin.z - q.z).length() - 0.4 * 0.9
						if d < 0.019:
							inside += 1
						elif d < 0.2:
							by += 1
		return [inside, by, discs.size()]
	for i in 40: # The fields poll their mounds twice a second.
		await get_tree().process_frame
		grass.update_at(cam.global_position)
		if i == 20:
			await get_tree().create_timer(0.6).timeout
	var on: Array = count.call()
	grass.undergrowth_field = was
	mf.free()
	for i in 40:
		await get_tree().process_frame
		grass.update_at(cam.global_position)
		if i == 20:
			await get_tree().create_timer(0.6).timeout
	_check("no grass under a mound (sward_mound), grass up to its foot", on[2] > 5 and on[0] == 0 and on[1] > 10, on)

func _sward_shade_check(c: Vector2, open_density: float) -> void:
	var brk := VegetationField3D.new()
	brk.far_volume = false # Not under test here.
	brk.name = "BrackenStands"
	brk.populate_on_ready = false
	var sp := PlantSpecies.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(0.3, 0.6, 0.3)
	sp.meshes = [bm]
	sp.spacing = 0.7
	sp.patch_scale = 6.0
	sp.patch_cover = 0.5
	sp.patch_seed = 11
	sp.sward_shade = 0.85
	sp.litter_color = Color(0.30, 0.13, 0.045, 0.95)
	var bb := Biome.new()
	bb.species = [sp]
	brk.biome = bb
	add_child(brk)
	brk.data_map = brk.get_path_to(dm)
	grass.undergrowth_field = grass.get_path_to(brk)
	for i in 3:
		grass.update_at(cam.global_position)
	var tall: PlantSpecies = grass.rules
	var under_n := 0
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var xf := mm.get_instance_transform(i)
				if xf.basis.x.length_squared() == 0.0:
					continue
				var p := xf.origin
				if Vector2(p.x, p.z).distance_to(c) < 8.0 and (tall == null or tall.get_patch_weight(p) > 0.99) and brk.get_sward_shade_at(p) > 0.8:
					under_n += 1
	seed(2)
	var hits := 0
	for i in 4000:
		var a := randf() * TAU
		var r := sqrt(randf()) * 8.0
		var q := Vector3(c.x + cos(a) * r, 0, c.y + sin(a) * r)
		if (tall == null or tall.get_patch_weight(q) > 0.99) and dm.get_water_depth_at(q) <= 0.0 and brk.get_sward_shade_at(q) > 0.8:
			hits += 1
	var area := PI * 64.0 * hits / 4000.0
	var under_density: float = under_n / maxf(area, 0.01)
	_check("grass thins under bracken stands (undergrowth_field)", hits > 100 and under_density < open_density * 0.2, "%.0f /m2 under, %.0f open, %.1f m2" % [under_density, open_density, area])
	# Their litter lies where the sward has gone, in its own colour; none on open meadow.
	var lit_in := 0.0
	var lit_n := 0
	var lit_out := 0.0
	var lit_col := Color(0, 0, 0)
	for i in 400:
		var q := Vector3(c.x + randf_range(-8.0, 8.0), 0, c.y + randf_range(-8.0, 8.0))
		var l := brk.get_sward_litter_at(q)
		var sh := brk.get_sward_shade_at(q)
		if sh > 0.8:
			lit_in += l.a
			lit_col = l
			lit_n += 1
		elif sh <= 0.0:
			lit_out = maxf(lit_out, l.a)
	lit_in /= maxi(lit_n, 1)
	_check("bracken litter under the stands, none in the open (litter_color)", lit_n > 10 and lit_in > 0.8 and lit_out == 0.0 and absf(lit_col.r - 0.30) < 0.01, "cover %.2f under (%d), %.2f open, colour %s" % [lit_in, lit_n, lit_out, str(lit_col)])
	grass.undergrowth_field = NodePath()
	brk.queue_free()

# --crop --test: tillers placed, in rows along the drill direction, the crop shader in use; then the
# stage set before emergence (the next check, 30 frames on).
func _crop_checks() -> void:
	# Every tile in range built first (the fields' radius holds hundreds of small tiles).
	for i in 600:
		grass.update_at(cam.global_position)
	var count := grass.get_blade_count()
	var off_row := 0
	var total := 0
	var nrm := Vector2(-sin(grass.drill_angle), cos(grass.drill_angle))
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D and ch.multimesh:
			var mm: MultiMesh = ch.multimesh
			for i in mini(mm.instance_count, 400):
				var o: Vector3 = ch.global_transform * mm.get_instance_transform(i).origin
				var u: float = Vector2(o.x, o.z).dot(nrm) / grass.row_spacing
				# Within 4 cm of its row (seed scatter, the drill's wander and the tillers' spread).
				if absf(u - roundf(u)) * grass.row_spacing > 0.04:
					off_row += 1
				total += 1
	var code: String = grass.get_material().get_shader().code
	print("GRASS_CROP %s stage %.0f tillers=%d sampled=%d off_row=%d" % [crop_name, grass.crop_stage, count, total, off_row])
	if count < 1000:
		fails += 1
		print("GRASS_FAIL crop: too few tillers")
	if not crop_fields and (total == 0 or off_row > total / 20):
		fails += 1
		print("GRASS_FAIL crop: tillers off their rows")
	if not code.contains("crop_look"):
		fails += 1
		print("GRASS_FAIL crop: not the crop shader")
	# The sowing calendar: winter wheat sown 7 Oct is at hard dough by late July, stubble in September.
	var july := GrassField3D.crop_calendar_stage(280, -1, 205)
	var sept := GrassField3D.crop_calendar_stage(280, -1, 250)
	var spring := GrassField3D.crop_calendar_stage(75, -1, 165)
	print("GRASS_CROP calendar winter 24 Jul %.1f, 7 Sep %.1f; spring barley 14 Jun %.1f" % [july, sept, spring])
	if july < 80.0 or july > 92.0 or sept != 99.0 or spring < 50.0 or spring > 65.0:
		fails += 1
		print("GRASS_FAIL crop calendar")
	if crop_fields:
		_field_checks()
	if not crop_fields:
		grass.crop_stage = 5.0

# --crop --fields --test: no tillers outside the fields, none in the tramline wheel tracks, headland
# tillers on rows parallel to their edge.
func _field_checks() -> void:
	var f0: Dictionary = grass.fields[0]
	var poly: PackedVector2Array = f0["polygon"]
	var rs: float = grass.row_spacing
	var d := Vector2(cos(f0["drill_angle"]), sin(f0["drill_angle"]))
	var nrm := Vector2(-d.y, d.x)
	var outside := 0
	var in_track := 0
	var body := 0
	var head := 0
	var head_off := 0
	var total := 0
	for ch in grass.get_children(true):
		if not (ch is MultiMeshInstance3D and ch.multimesh):
			continue
		var mm: MultiMesh = ch.multimesh
		for i in range(0, mm.instance_count, 7):
			var o: Vector3 = ch.global_transform * mm.get_instance_transform(i).origin
			var q := Vector2(o.x, o.z)
			total += 1
			var inside := false
			for f in grass.fields:
				if Geometry2D.is_point_in_polygon(q, f["polygon"]):
					inside = true
			if not inside:
				outside += 1
				continue
			if not Geometry2D.is_point_in_polygon(q, poly):
				continue
			var de := 1e9
			for e in poly.size():
				de = minf(de, q.distance_to(Geometry2D.get_closest_point_to_segment(q, poly[e], poly[(e + 1) % poly.size()])))
			var hl: float = f0["headland"]
			var sp: float = f0["tramline_spacing"]
			if de >= hl + 0.1:
				body += 1
				var w := fposmod(q.dot(nrm), sp) - sp * 0.5
				if absf(absf(w) - 0.9) < 0.1:
					in_track += 1
			elif de < hl - 0.5 and de > 0.3:
				head += 1
				var r := fposmod(de / rs, 1.0)
				if absf(r - 0.5) > 0.4:
					head_off += 1
	print("GRASS_FIELDS sampled=%d outside=%d body=%d in_wheel_tracks=%d headland=%d off_row=%d" % [total, outside, body, in_track, head, head_off])
	if total == 0 or outside > 0 or body == 0 or in_track > 0:
		fails += 1
		print("GRASS_FAIL fields: tillers outside the fields or in the tramlines")
	if head == 0 or head_off > head / 4:
		fails += 1
		print("GRASS_FAIL fields: headland rows not parallel to the edge")
	# Hedgerows round the fields (GrassField3D.hedges): segments laid, none standing in a field.
	var hedge_n := grass.get_hedge_segment_count()
	var hedge_in := 0
	var hedge_burnt := 0
	for h in grass.get_children(true):
		if h.name != "Hedges":
			continue
		for ch in h.get_children():
			var mm: MultiMesh = ch.multimesh
			# TreeGenerator's burnt band variants (mesh meta "tree_fire_years") are for VegetationField3D only.
			if mm.mesh and mm.mesh.has_meta("tree_fire_years"):
				hedge_burnt += 1
			for i in mm.instance_count:
				var t: Transform3D = ch.global_transform * mm.get_instance_transform(i)
				var mid := t.origin # Segments are centred on their origin.
				for f in grass.fields:
					if Geometry2D.is_point_in_polygon(Vector2(mid.x, mid.z), f["polygon"]):
						hedge_in += 1
	print("GRASS_HEDGES segments=%d in_fields=%d burnt=%d" % [hedge_n, hedge_in, hedge_burnt])
	if hedge_burnt > 0:
		fails += 1
		print("GRASS_FAIL hedges: a burnt tree variant in an unburnt hedgerow")
	if hedge_n < 40 or hedge_in > 0:
		fails += 1
		print("GRASS_FAIL hedges: too few segments round the fields, or some standing in a field")
	# The meadow keeps out of the fields.
	var meadow := get_node("Meadow") as GrassField3D
	for i in 600:
		meadow.update_at(cam.global_position)
	var m_total := 0
	var m_in := 0
	for ch in meadow.get_children(true):
		if not (ch is MultiMeshInstance3D and ch.multimesh):
			continue
		var mm: MultiMesh = ch.multimesh
		for i in range(0, mm.instance_count, 5):
			var o: Vector3 = ch.global_transform * mm.get_instance_transform(i).origin
			if mm.get_instance_custom_data(i).r <= 0.0:
				continue # GPU fill: rejected candidates are zero records.
			m_total += 1
			for f in grass.fields:
				if Geometry2D.is_point_in_polygon(Vector2(o.x, o.z), f["polygon"]):
					m_in += 1
					break
	print("GRASS_FIELDS meadow blades=%d in_fields=%d" % [m_total, m_in])
	if m_total == 0 or m_in > 0:
		fails += 1
		print("GRASS_FAIL fields: meadow grass inside the fields")

var checks_done := false
func _run_checks() -> void:
	await _checks()
	checks_done = true

func _checks() -> void:
	_season_checks()
	_hay_checks()
	var count := grass.get_blade_count()
	_check("blades placed", count > 10000, count)
	print("GRASS_STATS blades=%d tiles=%d last_build_ms=%.1f" % [count, grass.get_tile_count(), grass.get_build_time_msec()])
	# Collect blades: near tiles are denser than far ones; none under the pond.
	seed(1)
	var near := 0
	var near_area := 0.0
	var far := 0
	var wet := 0
	var c := Vector2(cam.global_position.x, cam.global_position.z)
	var tall: PlantSpecies = grass.rules
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var xf := mm.get_instance_transform(i)
				if xf.basis.x.length_squared() == 0.0:
					continue # A candidate the GPU fill rejected (zero record).
				var p := xf.origin
				var d := Vector2(p.x, p.z).distance_to(c)
				# Inside the tall patches only (the rest is lawn by design).
				var inside := tall == null or tall.get_patch_weight(p) > 0.99
				if d < 8.0 and inside: near += 1
				elif d > 32.0 and d < 36.0 and inside: far += 1
				if dm.get_water_depth_at(p) > 0.06: wet += 1
	# Area of the near disc inside the patches, sampled.
	var hits := 0
	for i in 4000:
		var a := randf() * TAU
		var r := sqrt(randf()) * 8.0
		var q := Vector3(c.x + cos(a) * r, 0, c.y + sin(a) * r)
		if (tall == null or tall.get_patch_weight(q) > 0.99) and dm.get_water_depth_at(q) <= 0.0: hits += 1
	near_area = PI * 64.0 * hits / 4000.0
	var ring_hits := 0
	for i in 4000:
		var a := randf() * TAU
		var r := sqrt(lerp(32.0 * 32.0, 36.0 * 36.0, randf()))
		var q := Vector3(c.x + cos(a) * r, 0, c.y + sin(a) * r)
		if (tall == null or tall.get_patch_weight(q) > 0.99) and dm.get_water_depth_at(q) <= 0.0: ring_hits += 1
	var ring_area := PI * (36.0 * 36.0 - 32.0 * 32.0) * ring_hits / 4000.0
	var near_density: float = near / max(near_area, 0.01)
	var far_density: float = far / max(ring_area, 0.01)
	_check("near density about %d /m2" % int(grass.density), near_density > grass.density * 0.75 and near_density < grass.density * 1.17, near_density)
	# 32 to 36 m of a 50 m field: the half-density level (full over the inner half of the radius, then a half).
	_check("far ring thinned (LOD)", far_density < near_density * 0.6 and far_density > near_density * 0.3, far_density)
	_check("no grass under water", wet == 0, wet)
	# Rocks (GrassField3D.obstacles): no blade roots inside a boulder, blades right up to its foot.
	var rock := Vector4(c.x + 2.0, 0.0, c.y + 1.0, 0.7)
	grass.obstacles = PackedVector4Array([rock])
	for i in 3:
		grass.update_at(cam.global_position)
	var r_in := 0
	var r_by := 0
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var xf := mm.get_instance_transform(i)
				if xf.basis.x.length_squared() == 0.0:
					continue
				var d := Vector2(xf.origin.x - rock.x, xf.origin.z - rock.z).length() - rock.w
				if d < 0.019:
					r_in += 1
				elif d < 0.2:
					r_by += 1
	_check("no grass inside a rock, grass up to its foot (obstacles)", r_in == 0 and r_by > 10, [r_in, r_by])
	grass.obstacles = PackedVector4Array()
	for i in 20:
		grass.update_at(cam.global_position)
	await _mound_check(c)
	_lodging_check(c)
	_snowmelt_check()
	_flood_wash_check()
	# The sward's colour reaches WorldGround.albedo_at (WorldPlantColorProvider): gas over the meadow picks up green.
	var ga: Color = WorldContracts.get_ground().albedo_at(Vector3(c.x, 0, c.y))
	var gfar: Color = grass.get_far_color()
	_check("ground colour: the meadow's sward answers WorldGround.albedo_at in its far colour", ga.a > 0.5 and ga.g > ga.r and absf(ga.g - gfar.g) < 0.02, "%s (far %s)" % [ga, gfar])
	# GPU tile fill (grass.md, stage 3a) against the CPU loop: the same blades but at rule edges.
	if grass.gpu_fill and GrassField3D.is_gpu_fill_available():
		_fill_compare(grass, "tall grass")
		if under:
			_fill_compare(under, "undercoat")
	if under:
		var un := 0
		var uwet := 0
		for ch in under.get_children(true):
			if ch is MultiMeshInstance3D:
				var mm: MultiMesh = ch.multimesh
				for i in mm.instance_count:
					var p := mm.get_instance_transform(i).origin
					if Vector2(p.x, p.z).distance_to(c) < 3.0: un += 1
					if dm.get_water_depth_at(p) > 0.06: uwet += 1
		_check("undercoat about 640 /m2 near", un / (PI * 9.0) > 480.0 and un / (PI * 9.0) < 720.0, un / (PI * 9.0))
		_check("no undercoat under water", uwet == 0, uwet)
		# Clover grows between the tall patches: the complement.
		var in_tall := 0
		var clover := cover.get_placements(3)
		for q in clover:
			if tall.get_patch_weight(q) > 0.9: in_tall += 1
		_check("clover between the tall patches", clover.size() > 1000 and in_tall < clover.size() * 0.02, "%d of %d" % [in_tall, clover.size()])
		_check("flowers placed", cover.get_placements(0).size() > 500, cover.get_placements(0).size())
	_sward_shade_check(c, near_density)
	# A setting changes (rebuild): the old tiles stay drawn until their successors are built,
	# nearest first, so the field never empties; time the first frame's work.
	var tiles_before := grass.get_tile_count()
	grass.rebuild()
	var tr0 := Time.get_ticks_usec()
	grass.update_at(cam.global_position)
	var first_ms := (Time.get_ticks_usec() - tr0) / 1000.0
	print("GRASS_REBUILD tiles_before=%d tiles_after_first_frame=%d first_frame_ms=%.1f" % [tiles_before, grass.get_tile_count(), first_ms])
	_check("rebuild keeps the field drawn", grass.get_tile_count() >= tiles_before, [tiles_before, grass.get_tile_count()])
	# Same camera, same field.
	for i in 20:
		grass.update_at(cam.global_position)
	_check("deterministic", grass.get_blade_count() == count, grass.get_blade_count())
	# The camera moves: tiles follow without rebuilding everything.
	grass.update_at(cam.global_position + Vector3(30, 0, 0))
	print("GRASS_MOVE tiles=%d build_ms=%.1f" % [grass.get_tile_count(), grass.get_build_time_msec()])
	grass.update_at(cam.global_position)
	_far_ring_checks()
	if cover:
		# Far distance: every ranged plant carries its rank (0..1) and its species' range for the
		# shader's thinning (whole metres; the fraction is the light it grew in), and the tile is
		# cut just past that range.
		var ranged := 0
		var bad := 0
		for ch in cover.get_children(true):
			if ch is MultiMeshInstance3D:
				var mm: MultiMesh = ch.multimesh
				for i in mm.instance_count:
					var cd := mm.get_instance_custom_data(i)
					var sp: PlantSpecies = cover.biome.species[int(cd.a)]
					if sp.visibility_range > 0.0:
						ranged += 1
						# The range a species is drawn to (its pixel reach for small ones, VegetationField3D.detail_reach);
						# a cell ends past it by up to its own width (0.75 of a tile cut in cells).
						var rng := cover.get_species_range(int(cd.a))
						if cd.r < 0.0 or cd.r > 1.0 or absf(floorf(cd.g) - floorf(rng)) > 1e-3 or ch.visibility_range_end < rng * 1.05 - 1e-3 or ch.visibility_range_end > rng * 1.05 + 0.75 * cover.tile_size + 1e-3:
							bad += 1
		_check("plants carry rank and range for thinning", ranged > 1000 and bad == 0, "%d ranged, %d bad" % [ranged, bad])
	_path_checks()
	_burn_checks()
	_radius_zero_check()

# Burnt ground (GrassField3D.burn_recovery, WorldFire.burn_age_at): two 8 m scars 6 m in front of the
# camera, a fortnight and four months old, against the same ground unburnt. The sward is gone on the fresh
# one and about half back, shorter, on the older one (smoothstep over 0.7 years), on the GPU and CPU fill.
func _burn_checks() -> void:
	var c := cam.global_position
	var fwd := -cam.global_transform.basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var side := Vector3(-fwd.z, 0.0, fwd.x)
	var mid := c + fwd * 6.0
	var ages := [0.04, 0.35]
	var centres := [mid - side * 5.0, mid + side * 5.0]
	for gpu in [true, false]:
		grass.gpu_fill = gpu
		var before := _burn_count(c, centres)
		var scars := []
		for k in 2:
			var scar := WorldBurnScar3D.new()
			scar.age_years = ages[k]
			scar.size = Vector2(8, 8)
			scar.position = Vector3(centres[k].x, 0.0, centres[k].z)
			add_child(scar)
			scars.append(scar)
		var after := _burn_count(c, centres)
		for scar in scars:
			scar.free()
		var share := float(after[0][1]) / maxf(before[0][1], 1)
		var hr: float = (after[1][1] / maxf(after[0][1], 1)) / maxf(before[1][1] / maxf(before[0][1], 1), 1e-6)
		var res := [before[0], after[0], snappedf(share, 0.01), snappedf(hr, 0.01)]
		_check("burnt ground (%s fill): sward gone on a fresh burn, about half back and shorter at four months" % ("GPU" if gpu else "CPU"), before[0][0] > 20 and after[0][0] * 20 <= before[0][0] and share > 0.3 and share < 0.75 and hr < 0.95, res)
	grass.gpu_fill = true
	_burn_count(c, centres)
	# A fire burning ground after the grass was built (no rebuild): the scar's report (WorldFire.burn_changed)
	# alone thins the tiles over it, and only those; then it ages to four months.
	var far_tiles := grass.get_tile_count()
	var scar := WorldBurnScar3D.new()
	scar.age_years = 0.04
	scar.size = Vector2(8, 8)
	scar.position = Vector3(centres[0].x, 0.0, centres[0].z)
	add_child(scar)
	var live := []
	for step in 2:
		var t0 := Time.get_ticks_msec()
		while Time.get_ticks_msec() - t0 < 600:
			OS.delay_msec(50)
		for i in 3:
			grass.update_at(c)
		live.append(_burn_tally(centres[0]))
		scar.age_years = 0.35
	scar.free()
	var t1 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t1 < 600:
		OS.delay_msec(50)
	for i in 3:
		grass.update_at(c)
	var back := _burn_tally(centres[0])
	_check("burnt ground follows a live fire without a rebuild (burn_changed): gone, then half back, then whole", live[0] * 20 <= back and back > 20 and float(live[1]) / back > 0.3 and float(live[1]) / back < 0.75 and grass.get_tile_count() >= far_tiles, [live, back, far_tiles, grass.get_tile_count()])

func _burn_tally(centre: Vector3) -> int:
	var n := 0
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			var buf := RenderingServer.multimesh_get_buffer(mm.get_rid())
			for i in mm.instance_count:
				var b := i * 16
				if buf[b] == 0.0 and buf[b + 2] == 0.0:
					continue
				if absf(buf[b + 3] - centre.x) < 1.5 and absf(buf[b + 11] - centre.z) < 1.5:
					n += 1
	return n

class FloodMarkStub extends WorldWater:
	var peak := 0.0
	var days := 10.0
	func _height_at(_p: Vector3) -> float:
		return -INF
	func _high_water_at(_p: Vector3) -> Vector4:
		return Vector4(peak, days, 1.0, 0.0)

# GrassField3D.flood_wash: under the last flood's mark (WorldWater.high_water_at, a script stub here) the blades lie
# combed flat downstream and silted, none above it; the shader gets the mark's map, and loses it with the flood.
func _flood_wash_check() -> void:
	var g0 := height(0.0, 0.0)
	var stub := FloodMarkStub.new()
	stub.peak = g0 + 0.3
	WorldContracts.add_water_stub(stub)
	var under: Vector3 = grass.get_flood_wash_at(Vector3(0, g0, 0))
	var above: Vector3 = grass.get_flood_wash_at(Vector3(0, g0 + 0.5, 0))
	grass.update_flood_map(cam.global_position)
	var rect: Vector4 = grass.get_material().get_shader_parameter("flood_rect")
	WorldContracts.clear_overrides()
	grass.update_flood_map(cam.global_position)
	var gone: Vector4 = grass.get_material().get_shader_parameter("flood_rect")
	_check("flood wash: combed and silted under the mark, not above; the map follows the flood", under.x > 0.8 and under.y > 0.9 and above.z < 0.0 and rect.z > 0.0 and gone.z == 0.0, [under, above, rect, gone])

class SeedWindWeather extends WorldWeather:
	var src: WeatherMap3D
	var ground: Callable
	var gust := 1.0
	func _wind_at(_p: Vector3, h: float) -> Vector3:
		return gust * Vector3(10, 0, 2.5) * clampf(log(maxf(h, 0.05) / 0.03) / log(10.0 / 0.03), 0.2, 1.0)
	func _surface_wetness_at(_p: Vector3) -> float:
		return 0.0
	func _rainfall_at(_p: Vector3) -> float:
		return 0.0
	func _ground_height_at(p: Vector3) -> float:
		return ground.call(p.x, p.z)
	func _season_at(_p: Vector3, _d: int) -> Dictionary:
		return src.get_season()

# River scout: a PhysXRiver3D along the valley's channel (5 m³/s; the median annual flood x6 on day 110).
func _add_river() -> void:
	var r := PhysXRiver3D.new()
	r.name = "River"
	var c := Curve3D.new()
	var z := -SIZE * 0.5 + 2.0
	while z <= SIZE * 0.5 - 2.0:
		c.add_point(Vector3(river_x(z), height(river_x(z), z) + 0.5, z))
		z += 6.0
	r.curve = c
	r.discharge = 5.0
	r.step_length = 2.0
	r.rain_response = false
	add_child(r)
	river_node = r
	_river_settle.call_deferred()

func _river_settle() -> void:
	for i in 4:
		await get_tree().physics_frame
	river_node.rebuild()

# Where the mark lies across the floodplain at the view's section, the wrack placed against it, and the eye on the
# bank above it looking back over the strand line to the river.
func _river_wrack_report() -> void:
	var water := WorldContracts.get_water()
	var z0 := 6.0
	var surf: float = river_node.sample_height(Vector3(river_x(z0), 0.0, z0))
	var hw: Vector4 = water.high_water_at(Vector3(river_x(z0), 0.0, z0))
	var mark_x := river_x(z0)
	for i in 400:
		var x := river_x(z0) + i * 0.05
		if height(x, z0) > hw.x:
			mark_x = x
			break
	print("GRASS_RIVER surface %.2f mark %.2f (%.2f m over, %.0f days, flow %.2f,%.2f) mark %.1f m from the centre line" % [surf, hw.x, hw.x - surf, hw.y, hw.z, hw.w, mark_x - river_x(z0)])
	for k in [0, 1]:
		var pl: PackedVector3Array = cover.get_placements(k)
		var dmin := 99.0
		var dmax := -99.0
		for q in pl:
			var m: Vector4 = water.high_water_at(q)
			var d := height(q.x, q.z) - m.x
			dmin = minf(dmin, d)
			dmax = maxf(dmax, d)
		var zs := Vector2(99, -99)
		for q in pl:
			zs = Vector2(minf(zs.x, q.z), maxf(zs.y, q.z))
		print("GRASS_WRACK %d placed, ground minus peak %.2f..%.2f m, z %.1f..%.1f" % [pl.size(), dmin, dmax, zs.x, zs.y])
	var back := Vector2(mark_x + 2.2, z0 + 2.0)
	cam.position = Vector3(back.x, height(back.x, back.y) + 1.6, back.y)
	cam.look_at(Vector3(mark_x - 0.6, hw.x - 0.1, z0 - 1.5))
	if OS.get_environment("GRASS_RIVER_TOP") == "1": # Debug: straight down on the mark from 4 m.
		cam.position = Vector3(mark_x, hw.x + 4.0, z0)
		cam.look_at(Vector3(mark_x, hw.x, z0 - 0.01))
	print("GRASS_RIVER_CAM %s" % cam.global_position)
	grass.update_flood_map(cam.global_position)

# --pollen-cloud: a meadow in flower in a dry 10 m/s wind (relative humidity 40%): PollenCloud3D sheds from the sward
# round the camera into its Flow grid, which runs for the frames before the capture (--frames).
class PollenWindWeather extends SeedWindWeather:
	func _air_humidity_at(_p: Vector3) -> float:
		return 0.4

func _pollen_cloud_view() -> void:
	var w := PollenWindWeather.new()
	w.src = season_map
	w.ground = height
	WorldContracts.set_weather(w)
	var pc := PollenCloud3D.new()
	pc.name = "PollenCloud"
	add_child(pc)
	pc.update_sources()
	print("GRASS_POLLEN release %.2f grass sources %d conifer %d flow %s" % [pc.get_release(), pc.get_grass_sources(), pc.get_conifer_sources(), str(pc.has_flow())])

# --seed-down: thistledown in a dry 10 m/s wind (log profile over short grass), the camera beside the thistle in seed
# nearest the default view, looking downwind; 20 s of release first.
func _seed_down_view() -> void:
	var w := SeedWindWeather.new()
	w.src = season_map
	w.ground = height
	WorldContracts.set_weather(w)
	var ti := -1
	for i in cover.biome.species.size():
		if cover.biome.species[i].get_meta("plant_preset", -1) == PlantGenerator.PRESET_THISTLE:
			ti = i
	var best := Vector3(0, 0, 0)
	var bd := 1e9
	for q in cover.get_placements(ti):
		var d := q.distance_to(cam.global_position)
		if d > 3.0 and d < bd:
			bd = d
			best = q
	var sd := SeedDown3D.new()
	sd.name = "SeedDown"
	add_child(sd)
	# Across the wind, a little behind the head, so the down streams off it across the frame.
	# Downwind of the head and a little to the side, looking back up the plume towards it.
	var top := best + Vector3(0, 0.9, 0)
	cam.global_position = top + Vector3(2.2, 0.1, 1.2)
	cam.look_at(top + Vector3(0.0, -0.1, 0.0))
	cam.fov = 55.0
	sd.release_rate = 4.0 # A gusty peak afternoon: ripe heads emptying fast.
	print("GRASS_SEED_SHARE thistle in seed %.2f" % PlantGenerator.get_bloom_shares(PlantGenerator.PRESET_THISTLE, season_map.get_season()).z)
	sd.refresh_sources()
	sd.advance(6.0, true)
	sd.still_shot = true # The still holds this moment.
	var near := 0
	for q in sd.get_seed_states():
		if Vector3(q.x, q.y, q.z).distance_to(cam.global_position) < 4.0 and q.w < 0.0:
			near += 1
	if OS.get_environment("GRASS_SEED_BURST") == "1":
		(WorldContracts.get_weather() as SeedWindWeather).gust = 0.0
		sd.release_at(cam.global_position + (-cam.global_transform.basis.z) * 1.2, SeedDown3D.KIND_THISTLE, 30)
	print("GRASS_SEED_NEAR aloft within 4 m: %d" % near)
	print("GRASS_SEED_DOWN heads=%d released=%d aloft=%d lying=%d" % [sd.get_source_count(), sd.get_released_count(), sd.get_aloft_count(), sd.get_lying_count()])

func _snowmelt_check() -> void:
	# GrassField3D.snowmelt: a snowy winter (January -6 C, 1100 mm) lies to about day 120 at the meadow; the sward is
	# matted just after the melt and has stood up again by late summer. Read through WorldContracts.weather().
	var wm := WeatherMap3D.new()
	var rs := ClimateRecipe.new()
	rs.temperature_january = -6.0
	rs.temperature_july = 18.0
	rs.precipitation = 1100.0
	wm.climate = rs
	add_child(wm)
	var p := Vector3(0, height(0.0, 0.0), 0)
	var under: Vector3 = grass.get_snowmelt_at(p, 100)
	var fresh: Vector3 = grass.get_snowmelt_at(p, 130)
	var late: Vector3 = grass.get_snowmelt_at(p, 200)
	wm.free()
	_check("snowmelt: under snow on day 100, matted after a long lie on day 130, upright by day 200", under.y < 0.0 and under.x > 60.0 and fresh.x > 60.0 and fresh.y >= 0.0 and fresh.z > 0.5 and late.z == 0.0, [under, fresh, late])


# radius 0 draws no blades: no near tiles, far rings or shell (a field kept for its rules or ground tint only).
func _radius_zero_check() -> void:
	var c := cam.global_position
	var r0: float = grass.radius
	grass.radius = 0.0
	for i in 3:
		grass.update_at(c)
	var res := [grass.get_tile_count(), grass.get_ring_tile_count(), grass.get_shell_tile_count()]
	grass.radius = r0
	for i in 3:
		grass.update_at(c)
	res.append(grass.get_tile_count())
	_check("radius 0: no blades drawn (tiles, rings, shell), back with its radius", res[0] == 0 and res[1] == 0 and res[2] == 0 and res[3] > 0, res)

# Blades and their summed heights in the middle 3 m of each burn scar's square, after a rebuild.
func _burn_count(c: Vector3, centres: Array) -> Array:
	grass.rebuild()
	for i in 3:
		grass.update_at(c)
	var n := []
	var h := []
	for k in centres.size():
		n.append(0)
		h.append(0.0)
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D:
			var mm: MultiMesh = ch.multimesh
			var buf := RenderingServer.multimesh_get_buffer(mm.get_rid())
			for i in mm.instance_count:
				var b := i * 16
				if buf[b] == 0.0 and buf[b + 2] == 0.0:
					continue
				for k in centres.size():
					if absf(buf[b + 3] - centres[k].x) < 1.5 and absf(buf[b + 11] - centres[k].z) < 1.5:
						n[k] += 1
						h[k] += buf[b + 12]
	return [n, h]

# Worn paths (GrassField3D.paths): a busy path across the sward 5 m in front of the camera thins
# the blades on its trodden line and shortens the ones left, the same on the GPU and the CPU fill.
func _path_checks() -> void:
	var c := cam.global_position
	var fwd := -cam.global_transform.basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var mid := c + fwd * 5.0
	var side := Vector3(-fwd.z, 0.0, fwd.x)
	grass.paths = [{"points": PackedVector3Array([mid - side * 20.0, mid + side * 20.0]), "width": 1.5, "passes": 1500.0}]
	for gpu in [true, false]:
		grass.gpu_fill = gpu
		for i in 3:
			grass.update_at(c)
		var on := [0, 0.0]
		var off := [0, 0.0]
		for ch in grass.get_children(true):
			if ch is MultiMeshInstance3D:
				var mm: MultiMesh = ch.multimesh
				var buf := RenderingServer.multimesh_get_buffer(mm.get_rid())
				for i in mm.instance_count:
					var b := i * 16
					if buf[b] == 0.0 and buf[b + 2] == 0.0:
						continue
					var p := Vector3(buf[b + 3], 0.0, buf[b + 11])
					var along := (p - mid).dot(side)
					if absf(along) > 8.0:
						continue
					var across := absf((p - mid).dot(fwd))
					if across < 0.25:
						on[0] += 1
						on[1] += buf[b + 12]
					elif across > 2.0 and across < 2.5:
						off[0] += 1
						off[1] += buf[b + 12]
		var h_on: float = on[1] / maxf(on[0], 1)
		var h_off: float = off[1] / maxf(off[0], 1)
		_check("worn path (%s fill): blades thinned on the trodden line and shorter there" % ("GPU" if gpu else "CPU"), on[0] * 3 < off[0] and off[0] > 50 and (on[0] == 0 or h_on < h_off * 0.8), [on[0], off[0], snappedf(h_on, 0.001), snappedf(h_off, 0.001)])
	grass.gpu_fill = true
	grass.paths = []
	for i in 3:
		grass.update_at(c)

func _place(node: Node3D, x: float, z: float, lift: float) -> void:
	node.global_position = Vector3(x, height(x, z) + lift, z)

# The ball rolls across in front of the camera; the walker crosses the other way, in strides.
func _move(dt: float) -> void:
	t += dt
	var u := t * 1.6
	_place(ball, -6.0 + u, 9.0 + 2.0 * sin(u * 0.25), BALL_R)
	ball.rotation.z = -u / BALL_R
	var s := t * 1.3
	_place(walker, 6.0 - s, 12.5, 0.0)

func _trample_checks(dt: float) -> bool:
	# Ball parked on the ground at P: flat under it, bent away at its rim, untouched 3 m away.
	var p := Vector3(2.0, 0.0, 12.0)
	phase_t += dt
	if phase == 0:
		_place(ball, p.x, p.z, BALL_R)
		phase = 1
		phase_t = 0.0
	elif phase == 1 and phase_t > 0.1:
		var centre := grass.get_trample_at(p).length()
		var rim := grass.get_trample_at(p + Vector3(BALL_R * 0.9, 0, 0))
		var far := grass.get_trample_at(p + Vector3(3, 0, 0)).length()
		_check("pressed flat under the ball", centre > 0.95, centre)
		_check("rim bent away from the ball", rim.x > 0.5 and abs(rim.y) < 0.2, rim)
		_check("untouched 3 m away", far == 0.0, far)
		ball.position.y += 20.0
		phase = 2
		phase_t = 0.0
	elif phase == 2 and phase_t > 2.0:
		# Half-life 0.5 s: after 2 s about 1/16 remains.
		var left := grass.get_trample_at(p).length()
		# Also shows a ball held above the grass does not press it.
		_check("springs back (half-life 0.5 s), none from the air", left < 0.12, left)
		return true
	return false

# Mean linear colour of the screen rows between 12 and 18 m ahead, right part of the width (no pond).
func _region_mean() -> Color:
	var img := get_viewport().get_texture().get_image()
	var fwd := -cam.global_transform.basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var rows := []
	for d in [12.0, 18.0]:
		var q: Vector3 = cam.global_position + fwd * d
		q.y = height(q.x, q.z)
		rows.append(int(cam.unproject_position(q).y))
	var sum := Color(0, 0, 0)
	var cnt := 0
	for y in range(mini(rows[0], rows[1]), maxi(rows[0], rows[1]) + 1):
		for x in range(img.get_width() * 11 / 20, img.get_width() * 19 / 20):
			var c := img.get_pixel(x, y).srgb_to_linear()
			sum += c
			cnt += 1
	print("GRASS_FAR_ROWS ", rows)
	return sum / maxf(cnt, 1)

func _process(dt: float) -> void:
	n += 1
	if crop_name != "" and OS.get_environment("CROP_FAR_ONLY") == "1":
		# Calibration: the fields' far canopy everywhere, no tillers, to match against them.
		grass.visible = false
		var e := cam.global_position
		RenderingServer.global_shader_parameter_set("crop_ground_fade", Vector4(e.x, e.z, e.y, -1))
	if crop_fields and n == 4 and not test:
		# Every crop and meadow tile in range at once, as a player arriving would see them after a
		# few seconds (the fields' radius holds thousands of small tiles).
		for i in 800:
			grass.update_at(cam.global_position)
			(get_node("Meadow") as GrassField3D).update_at(cam.global_position)
	if ruts_worn and n == 6:
		# A worn-in track instead (mud round 14.2): two wheel lines 1.6 m apart, no ruts pressed, worn by 60 wheel
		# passes nearest the eye down to 2 passes 6 m away.
		for side in [0.2, 1.8]:
			for k in 14:
				var z := 16.0 - k * 0.45
				var x: float = side - 0.08 * k
				ruts_ground.add_wear(Vector3(x, 0, z), Vector2(0.3, 0.5), 0.17, 60.0 * pow(2.0 / 60.0, k / 13.0))
	elif ruts_view and n == 6:
		# A pair of tyre ruts 1.6 m apart running away from the eye, 8 cm deep.
		for side in [0.2, 1.8]:
			for k in 12:
				var z := 16.0 - k * 0.45
				var x: float = side - 0.08 * k
				ruts_ground.press_box(Vector3(x, 0, z), Vector2(0.32, 0.5), 0.17, height(x, z) - 0.08, 0.0)
		if ruts_mud:
			# Thrown clay along both sides of the left rut (a spray's clods, no print), thinning outwards; muddy water
			# splashed out along the right one.
			for k in 14:
				var z := 16.0 - k * 0.42
				for o in [-0.45, -0.75, 0.45, 0.75]:
					var x: float = 0.2 - 0.08 * k + o
					ruts_ground.track_mud(Vector3(x, height(x, z), z), 0.25, 0.0004 if absf(o) < 0.6 else 0.00015, 1.0)
				for o in [-0.5, 0.5]:
					var x: float = 1.8 - 0.08 * k + o
					ruts_ground.add_water(Vector3(x, height(x, z), z), 0.3, 0.0006, 60.0)
		# A light pass 6 mm deep to the right: the sward pressed flat, not torn.
		for k in 12:
			var z := 16.0 - k * 0.45
			var x: float = 3.4 - 0.08 * k
			ruts_ground.press_box(Vector3(x, 0, z), Vector2(0.32, 0.5), 0.17, height(x, z) - 0.006, 0.0)
	if ruts_aged > 0.0 and n == 14:
		# Left: the node goes (it keeps its ruts as it leaves), and a new one over the same ground reads them back
		# after the time away, scaled to weeks.
		var cfg := ruts_ground
		var next := GroundSurface3D.new()
		for prop in ["follow_target", "ground_type", "moisture", "keep_ruts", "ruts_path", "wind_ripples", "desiccation_cracks", "world_puddles"]:
			next.set(prop, cfg.get(prop))
		cfg.queue_free()
		await get_tree().create_timer(3.0).timeout
		next.ruts_time_scale = ruts_aged
		next.ruts_away_rain = 0.0 # the hollows kept as they were: only the sward grows back over them
		next.name = "RutsBack"
		add_child(next)
		ruts_ground = next
	if ruts_view and n == 40:
		for q in [Vector3(-0.15, 0, 14.0), Vector3(3.05, 0, 14.0), Vector3(0.3, 0, 14.0), Vector3(1.95, 0, 14.0)]:
			print("GRASS_RUT_LATE ", [q, ruts_ground.get_dent_at(q), ruts_ground.get_regrown_at(q), ruts_ground.get_plant_mud_at(q)])
	if ruts_view and n == 12:
		for q in [Vector3(-0.15, 0, 14.0), Vector3(3.05, 0, 14.0)]:
			print("GRASS_RUT ", [q, ruts_ground.get_dent_at(q), ruts_ground.get_rim_at(q), ruts_ground.get_fresh_at(q), ruts_ground.get_standing_water_at(q), ruts_ground.get_tracked_mud_at(q)])
	if n == 2:
		dm.bake()
		if rocks > 0 and not forest:
			_place_rocks()
		if cover and far_grass > 0:
			# The whole 640 m map at once is too many cells: streamed round the eye instead.
			cover.stream_radius = 80.0
			cover.stream_target = cover.get_path_to(cam)
			if OS.get_environment("GRASS_FAR_REACH") != "":
				cover.far_reach = float(OS.get_environment("GRASS_FAR_REACH")) # GRASS_FAR_REACH=1: the plants fade at their range
			if OS.get_environment("GRASS_FAR_SHELL") == "0":
				cover.far_shell = false # GRASS_FAR_SHELL=0: no far columns for the taller plants
			cover.far_shell_clumps = OS.get_environment("GRASS_FAR_CLUMPS") != "0" # GRASS_FAR_CLUMPS=0: the far shell as an even mass
			cover.streaming = true
		elif cover:
			cover.populate()
			var counts := []
			for i in cover.biome.species.size():
				counts.append(cover.get_placements(i).size())
			if river:
				_river_wrack_report()
			elif flood.x >= 0.0:
				for k in [0, 1]:
					var pl: PackedVector3Array = cover.get_placements(k)
					var dmin := 99.0
					var dmax := -99.0
					for q in pl:
						var d := height(q.x, q.z) - (-0.6 + flood.x)
						dmin = minf(dmin, d)
						dmax = maxf(dmax, d)
					print("GRASS_WRACK %d placed, ground minus peak %.2f..%.2f m" % [pl.size(), dmin, dmax])
			if seed_down and season_map:
				_seed_down_view()
			if pollen_cloud and season_map:
				_pollen_cloud_view()
			print("GRASS_COVER %s = %s ms=%.1f" % ["(tree, bramble, bracken, foxglove) flowers, sedge, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry grass, tall flowers, tall dry flowers (fern, bilberry, moss, litter)", str(counts), cover.get_populate_time_msec()])
			if ug_field:
				ug_field.populate()
				print("GRASS_UNDERGROWTH placements=%d" % ug_field.get_placements(0).size())
				for g in [grass, under, turf]:
					if g:
						g.rebuild()
				_frame_undergrowth()
			if mounds_view != "":
				for g in [grass, under, turf]:
					if g:
						g.undergrowth_field = g.get_path_to(cover) # Its mounds keep the blades out.
				_frame_mounds()
			if forest:
				grass.rebuild()
				under.rebuild()
				turf.rebuild()
				if clearing >= 0:
					_frame_clearing()
				else:
					_frame_forest()
					if mushroom_view != "":
						_frame_mushrooms()
					if rocks > 0:
						# --forest --rocks: deep in the wood (the point within 40 m with the most
						# trunks within 12 m), looking where the wood runs on longest, down at the
						# floor 2.2 m ahead; rocks there (one big one in the middle), the floor placed
						# again around them. The stand-in trunks only cast their shadows.
						var tl := cover.get_placements(0)
						var c0 := cam.global_position
						var in_eye := c0
						var most := -1
						for gx in range(-10, 11):
							for gz in range(-10, 11):
								var cand := c0 + Vector3(gx * 4.0, 0, gz * 4.0)
								var cnt := 0
								for t in tl:
									if Vector2(t.x - cand.x, t.z - cand.z).length() < 12.0:
										cnt += 1
								if cnt > most:
									most = cnt
									in_eye = cand
						# Down-sun, so the rock's foot is lit from behind the camera.
						var f := -sun_node.global_transform.basis.z
						f.y = 0.0
						f = f.normalized()
						in_eye -= f * 4.0
						print("GRASS_ROCK_FRAME eye=%s dir=%s" % [str(in_eye), str(f)])
						cam.position = Vector3(in_eye.x, height(in_eye.x, in_eye.z) + 1.5, in_eye.z)
						var at := in_eye + f * 2.6
						cam.look_at(Vector3(at.x, height(at.x, at.z), at.z))
						_place_rocks()
						cover.populate()
						for ch in cover.get_children(true):
							if ch is MultiMeshInstance3D and ch.multimesh and ch.multimesh.mesh and ch.multimesh.mesh.get_aabb().size.y > 4.0:
								ch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
						grass.rebuild()
						under.rebuild()
						turf.rebuild()
			if swamp:
				var lilies := cover.get_placements(0)
				var floating := 0
				for q in lilies:
					if absf(q.y + 0.6) < 0.02: floating += 1
				print("GRASS_SWAMP lilies=%d on_the_surface=%d" % [lilies.size(), floating])
				if lilies.size() > 0 and swamp_close:
					var l: Vector3 = lilies[0]
					cam.position = l + Vector3(2.5, 1.6, 2.5)
					cam.look_at(l)
	if trample and n > 2:
		_move(dt)
	# GRASS_DIAG=viewtime: each frame's time and the live-view match's state, to see the readback
	# land without a hitch.
	if OS.get_environment("GRASS_DIAG").contains("viewtime") and n > 5:
		var pv: Dictionary = grass.get_far_published()
		print("GRASS_VIEWTIME n=%d dt_ms=%.1f state=%d round=%d scale=%s" % [n, dt * 1000.0, pv["view_state"], pv["view_round"], str(pv["view_scale"])])
	# The hedges (hundreds of dense segments) only for the last 40 frames: the software rasteriser
	# takes tens of seconds a frame with them, and the far bake and live match do not need them.
	if crop_fields and not test and n > 2:
		for h in grass.get_children(true):
			if h.name == "Hedges":
				h.visible = n >= frames - 40 # Long enough for the temporal AA to settle on them.
	if n == frames - 30 and not test:
		t_frames = Time.get_ticks_usec()
	if test and crop_name != "":
		if n == frames:
			_crop_checks()
		elif n == frames + 30:
			# Not yet emerged (below BBCH 10): no tillers at all.
			for i in 600:
				grass.update_at(cam.global_position)
			print("GRASS_CROP stage 5 tillers=%d" % grass.get_blade_count())
			if grass.get_blade_count() != 0 and not crop_fields:
				fails += 1
				print("GRASS_FAIL crop tillers before emergence")
			print("GRASS_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
			get_tree().quit(1 if fails > 0 else 0)
		return
	if test and n == frames:
		_run_checks()
	if test and n > frames:
		if not checks_done: # The checks wait a few frames (mounds are followed twice a second).
			return
		if _trample_checks(dt):
			print("GRASS_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)
			get_tree().quit(1 if fails > 0 else 0)
		return
	if test:
		return
	if dolly and n >= 20:
		# Walk into the land: plants grow in one by one as they come into range.
		var fwd := -cam.global_transform.basis.z
		fwd.y = 0.0
		var p := cam.position + fwd.normalized() * 0.3
		cam.position = Vector3(p.x, height(p.x, p.z) + 2.0, p.z)
	# Before/after of the far colour bake: the earlier hand-matched far colour, set over the baked one.
	if OS.get_environment("GRASS_DIAG").contains("handfar") and n == frames - 5:
		var bl := grass.base_color.srgb_to_linear()
		var tl := grass.tip_color.srgb_to_linear()
		var dl := grass.dry_color.srgb_to_linear()
		var hand := bl.lerp(tl, 0.65).lerp(dl, grass.dryness * 0.35) * 0.355
		RenderingServer.global_shader_parameter_set("grass_ground_far", Vector4(hand.r, hand.g, hand.b, 1.0))
	if not clip.is_empty() and n >= 20:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(clip.path_join("f%03d.png" % (n - 20)))
		if n >= frames:
			get_tree().quit()
		return
	if far_match and n == 3:
		# Variants of the tall grass, to see whether the far colour follows the blades.
		match OS.get_environment("GRASS_VARIANT"):
			"dry":
				grass.dryness = 0.8
				grass.tip_color = Color(0.62, 0.55, 0.25)
			"lawn":
				grass.blade_height = Vector2(0.08, 0.18)
				grass.density = 900.0
			"lean":
				grass.lean = 1.0
			"dark":
				grass.base_color = Color(0.03, 0.08, 0.02)
				grass.tip_color = Color(0.15, 0.3, 0.06)
	if far_match and n == frames:
		await RenderingServer.frame_post_draw
		far_a = _region_mean()
		get_viewport().get_texture().get_image().save_png("user://far_match_a.png")
		grass.radius = 10.0
		if far_cover:
			ground_mat.set_shader_parameter("far_from", 10.0)
		return
	if far_match and n == frames + 12:
		await RenderingServer.frame_post_draw
		var b := _region_mean()
		get_viewport().get_texture().get_image().save_png("user://far_match_b.png")
		print("GRASS_FAR_MATCH blades=%s far_ground=%s ratio=%s" % [str(far_a), str(b), str(Color(far_a.r / b.r, far_a.g / b.g, far_a.b / b.b))])
		var pub: Dictionary = grass.get_far_published()
		print("GRASS_FAR_VIEW scale=%s ratio=%s pairs=%d state=%d msec=%.2f" % [str(pub["view_scale"]), str(pub["view_ratio"]), pub["view_pairs"], pub["view_state"], pub["view_msec"]])
		# The earlier hand-matched colour, for comparison.
		var bl := grass.base_color.srgb_to_linear()
		var tl := grass.tip_color.srgb_to_linear()
		var dl := grass.dry_color.srgb_to_linear()
		var old := bl.lerp(tl, 0.6).lerp(dl, grass.dryness * 0.35) * 0.32
		print("GRASS_FAR_COLOURS published=%s old=%s" % [str(grass.get_far_color()), str(old)])
		RenderingServer.global_shader_parameter_set("grass_ground_far", Vector4(old.r, old.g, old.b, 1.0))
		return
	if far_match and n == frames + 24:
		await RenderingServer.frame_post_draw
		var c := _region_mean()
		print("GRASS_FAR_MATCH old far_ground=%s ratio=%s" % [str(c), str(Color(far_a.r / c.r, far_a.g / c.g, far_a.b / c.b))])
		# A white far colour: the light on the far ground, so blades over it is the albedo it needs.
		RenderingServer.global_shader_parameter_set("grass_ground_far", Vector4(1, 1, 1, 1))
		return
	if far_match and n == frames + 36:
		await RenderingServer.frame_post_draw
		far_w = _region_mean()
		RenderingServer.global_shader_parameter_set("grass_ground_far", Vector4(0, 0, 0, 1))
		return
	if far_match and n == frames + 48:
		# Black: what the far ground shows besides its albedo (bare ground, sheen), so the albedo
		# that matches the blades is (blades - black) / (white - black).
		await RenderingServer.frame_post_draw
		var k := _region_mean()
		var w := far_w
		var need := Color((far_a.r - k.r) / (w.r - k.r), (far_a.g - k.g) / (w.g - k.g), (far_a.b - k.b) / (w.b - k.b))
		var got := grass.get_far_color()
		print("GRASS_FAR_NEED white=%s black=%s needed=%s published=%s baked=%s published/needed=%s" % [str(w), str(k), str(need), str(got), str(grass.is_far_color_baked()), str(Color(got.r / need.r, got.g / need.g, got.b / need.b))])
		# The white and black above went through the sun-azimuth factor too, so published/needed is
		# the match with it; without it the far ground would show the plain mean.
		# The sun term GrassField3D publishes (grass_ground_sun), from the bench's sun.
		var ld := -sun_node.global_transform.basis.z
		var lh := Vector2(ld.x, ld.z).length()
		var sun_v := Vector4(ld.x / lh, ld.z / lh, minf(lh / cos(deg_to_rad(30.0)), 1.15) * clampf((-ld.y + 0.05) / 0.1, 0.0, 1.0), 0.0)
		var lobes := grass.get_far_color_lobes()
		var fwd := -cam.global_transform.basis.z
		var c1 := Vector2(fwd.x, fwd.z).normalized().dot(Vector2(sun_v.x, sun_v.y))
		var f := Color(1, 1, 1)
		for i in 3:
			f[i] = maxf(1.0 + sun_v.z * (lobes[0][i] * c1 + lobes[1][i] * (2.0 * c1 * c1 - 1.0)), 0.1)
		print("GRASS_FAR_AZIMUTH cos=%.2f strength=%.2f factor=%s lobes=%s ratio_with=%s ratio_without=%s" % [c1, sun_v.z, str(f), str(lobes), str(Color(got.r / need.r, got.g / need.g, got.b / need.b)), str(Color(got.r / (need.r * f.r), got.g / (need.g * f.g), got.b / (need.b * f.b)))])
		get_tree().quit()
		return
	if far_match or n < frames:
		return
	# Path traced: wait for every surface's first vertex capture and BLAS (built under a per-frame
	# budget, so a dense meadow takes more than the default frames; captured too early the grass
	# blades were missing from the frame).
	if not raster and n < frames + 600 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
		return
	if not raster and n > frames:
		print("GRASS_PT_READY after %d frames" % n)
	set_process(false)
	if t_frames > 0:
		var blades := grass.get_blade_count() + (under.get_blade_count() if under else 0)
		print("GRASS_FRAME_MS %.1f blades=%d" % [(Time.get_ticks_usec() - t_frames) / 30000.0, blades])
	if far_grass > 0:
		print("GRASS_FAR_RINGS %s: ring blades %d in %d tiles, last build %.1f ms, shell columns %d in %d tiles, near blades %d, memory +%.1f MB, primitives %d, draw calls %d" % [
			["", "on", "off", "rings"][far_grass], grass.get_ring_blade_count(), grass.get_ring_tile_count(), grass.get_ring_build_msec(), grass.get_shell_column_count(), grass.get_shell_tile_count(), grass.get_blade_count(),
			(OS.get_static_memory_usage() - far_mem0) / 1048576.0, Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME), Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)])
		if cover:
			# The plants drawn (the far tail's share: VegetationField3D.far_reach).
			var inst := 0
			var shells := 0
			for c in cover.get_children(true):
				if c.name.begins_with("FarShell"):
					shells += 1
				if c is MultiMeshInstance3D and c.multimesh and not c.name.begins_with("FarVolume"):
					inst += c.multimesh.instance_count
			print("GRASS_FAR_COVER far_reach %.1f: plant instances %d, far shell columns %d in %d tiles" % [cover.far_reach, inst, cover.get_far_shell_column_count(), shells])
	await RenderingServer.frame_post_draw
	if OS.get_environment("GRASS_STILL") == "1":
		# GrassField3D/VegetationField3D.still_shot: settled and frozen by now; memory against the run's start.
		print("GRASS_STILL frozen grass=%s cover=%s memory %.0f MB" % [grass.is_still_frozen(), cover.is_still_frozen() if cover else "-", OS.get_static_memory_usage() / 1048576.0])
	if prims:
		await _print_prims()
	# CPU and GPU memory at the capture (the path tracer's CPU copies of MultiMesh records count in static memory).
	if deer_wood >= 0.0:
		# The crowns' shade along the view (0 open sky): the sward rule wants it under 0.3.
		var sh := PackedStringArray()
		var fwd := -cam.global_transform.basis.z
		fwd = Vector3(fwd.x, 0.0, fwd.z).normalized()
		for d in [1.0, 3.0, 6.0, 10.0, 15.0, 20.0, 25.0]:
			sh.append("%d m %.2f" % [d, cover.get_shade_at(cam.global_position + fwd * d)])
		print("GRASS_DEER_SHADE ", ", ".join(sh))
	print("GRASS_MEM static_mb=%.1f video_mb=%.1f" % [Performance.get_monitor(Performance.MEMORY_STATIC) / 1048576.0, Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0])
	if not out.is_empty():
		if burn_age >= 0.0 and OS.get_environment("GRASS_NO_FLUSH") != "":
			# The before frame: the regrowth flush off (its colour alone).
			if OS.get_environment("GRASS_NO_FLUSH") == "1":
				(grass.get_material() as ShaderMaterial).set_shader_parameter("burn_flush_rect", Vector4())
			for i in 3:
				await RenderingServer.frame_post_draw
		if OS.get_environment("GRASS_FIBRE") == "ab":
			for i in 30: # Everything streamed in before the pair is taken.
				await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
		if burn_age >= 0.0 and OS.get_environment("GRASS_NO_FLUSH") == "ab":
			# The same frame again with the regrowth flush off: out_noflush.png.
			(grass.get_material() as ShaderMaterial).set_shader_parameter("burn_flush_rect", Vector4())
			for i in 3:
				await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out.get_basename() + "_noflush.png")
		if OS.get_environment("GRASS_FIBRE") == "ab":
			# The same frame again without the fibre shading (GrassField3D.fibre_shading 0): out_nofibre.png.
			grass.fibre_shading = 0.0
			for i in 30:
				await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(out.get_basename() + "_nofibre.png")

	get_tree().quit()


# Far rings (GrassField3D.far_rings, docs/ideas/grass.md "Far grass"): one set of blades past the near tiles, each
# with its rank, kept by the shader while rank < (start / distance)^2: the kept blades thin with the square of the
# distance, so many per square metre at the start and a quarter of that at twice it.
func _far_ring_checks() -> void:
	var c := Vector2(cam.global_position.x, cam.global_position.z)
	var start := grass.radius * 0.97 - grass.radius * 0.37
	var near := 0
	var far := 0
	var bad := 0
	for ch in grass.get_children(true):
		if ch is MultiMeshInstance3D and str(ch.name).begins_with("GrassRing"):
			var mm: MultiMesh = ch.multimesh
			for i in mm.instance_count:
				var cd := mm.get_instance_custom_data(i)
				if cd.r <= 0.0:
					continue # Dropped by the rules in the GPU fill (a zero record).
				var p: Vector3 = ch.global_transform * mm.get_instance_transform(i).origin
				var d := Vector2(p.x, p.z).distance_to(c)
				var rank := pow(2.0, -floorf(cd.a) / 1024.0)
				if cd.g >= 0.0:
					bad += 1
				# A sector across the map (-x), clear of its edge and the pond.
				var dir := (Vector2(p.x, p.z) - c).normalized()
				if dir.dot(Vector2(-1, 0)) < cos(deg_to_rad(25.0)):
					continue
				if rank < minf(1.0, (start / d) * (start / d)):
					if d > start and d < start + 2.0:
						near += 1
					elif d > 1.4 * start and d < 1.4 * start + 2.8:
						far += 1
	# Bands at s and 1.4 s, the second 1.4 times as wide: 1.96 times the ground, kept 1 / 1.96 as densely, so as
	# many blades in each.
	var ratio := float(far) / maxf(near, 1)
	_check("far rings: blades past the near tiles, thinning with the square of the distance", grass.get_ring_blade_count() > 1000 and bad == 0 and ratio > 0.7 and ratio < 1.4, [grass.get_ring_blade_count(), near, far, ratio, bad])
	grass.far_rings = false
	grass.update_at(cam.global_position)
	_check("far rings off: none", grass.get_ring_blade_count() == 0 and grass.get_ring_tile_count() == 0, grass.get_ring_blade_count())
	grass.far_rings = true
	grass.update_at(cam.global_position)


func _hide_grass() -> void:
	grass.visible = false
	if under:
		under.visible = false


func _no_trample() -> void:
	grass.trample_enabled = false
	if under:
		under.trample_enabled = false


func _no_view_facing() -> void:
	grass.view_facing = 0.0
	if under:
		under.view_facing = 0.0


func _hide_under() -> void:
	if under:
		under.visible = false

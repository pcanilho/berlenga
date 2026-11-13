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
#   --frames=N       frames before the capture (default 20)
#   --grass-density=N  blades per m2 of the tall grass near the camera (default: the field's, 120)
#   --sun-yaw=D      sun heading in degrees (default 150, against the light; -30 lights the view from behind)
#   env GRASS_DIAG=noshadow|nosun|nograss|notrample|novf|vf1|nounder|bounce0|ssaa: lighting diagnostics (grass.md, "Light inside the sward")
#   --cpu-fill       build grass tiles on the CPU instead of the GPU fill (grass.md, stage 3a)
#   --litter-wind=x,z  downwind lean of the leaf drift (VegetationField3D.litter_wind), e.g. 0.6,0
#   --day=D          the season of day D from a WeatherMap3D: the sward's greenness, flowering,
#                    ferns (docs/ideas/vegetation-seasons.md)
#   --drought        with --day: a dry climate (300 mm a year, falling in winter), for a summer drought
#   --out=path.png   save the last frame
#   --clip=dir       save every frame from 20 on as PNGs (for a GIF)
#   --layers=0       only the tall blades (no undercoat, ground tint, view facing, clover, flowers)
#   --close          the closer view used by --trample
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
#   --rocks=N        N boulders (stand-in rounded rocks) 3 to 10 m in front of the camera; the fields keep
#                    out of them (obstacles) unless --rocks-through
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
#   --far-match      the default view twice, the blades out to 22-32 m and then the ground's far
#                    colour there instead (radius 10 m): prints both mean colours and their ratio
#   --trample        a ball rolls and a walker strides through the grass, leaving trails

const SIZE := 96.0
var test := false
var raster := false
var spp := 16
var wind := Vector2(3, 1)
var weather := false
var frames := 20
var out := ""
var clip := ""
var n := 0
var dm: TerrainDataMap3D
var grass: GrassField3D
var season_day := 0
var drought := false
var season_map: WeatherMap3D
var bench_season := {}
var cam: Camera3D
var fails := 0
var trample := false
var layers := true
var close := false
var forest := false
var litter_wind := Vector2()
var grass_density := 0.0
var gpu_fill := true
var sun_yaw := 150.0
var sun_node: DirectionalLight3D
var forest_in := false
var stand := "beech"
var stand_set := false
var no_floor := false
var no_sward := false
var undergrowth := ""
var rocks := 0 # --rocks=N: boulders in front of the camera.
var rocks_through := false # --rocks-through: the fields not told of them (blades and plants grow through).
var ug_field: VegetationField3D
var no_far := false
var far_view := false
var prims := false
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
var notex := false
var t_frames := 0
var under: GrassField3D
var cover: VegetationField3D
var far_match := false
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

class Pond extends Node3D:
	func get_water_level() -> float:
		return -0.6

static func height(x: float, z: float) -> float:
	return 0.6 * sin(x * 0.08) * cos(z * 0.06) + 0.4 * sin((x + z) * 0.05) + 0.02 * (x * x + (z + 12.0) * (z + 12.0)) / 40.0 - 0.6

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test":
			test = true
			raster = true # Checks only: no need to path trace every frame.
		elif a == "--raster": raster = true
		elif a == "--weather": weather = true
		elif a == "--trample": trample = true
		elif a == "--layers=0": layers = false
		elif a == "--close": close = true
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
		elif a == "--far=0": no_far = true
		elif a == "--far": far_view = true
		elif a == "--prims": prims = true
		elif a == "--far-match": far_match = true
		elif a == "--impostors=0": no_impostors = true
		elif a == "--lone":
			forest = true
			clearing = 2
		elif a == "--clearing" or a == "--clearing=0":
			forest = true
			clearing = 0 if a.ends_with("=0") else 1
		elif a == "--dolly":
			far_view = true
			dolly = true
		elif a == "--terrain-material": terrain_mat = true
		elif a == "--forest=in":
			forest = true
			forest_in = true
		elif a == "--notex": notex = true
		elif a.begins_with("--grass-density="): grass_density = float(a.substr(16))
		elif a == "--cpu-fill": gpu_fill = false
		elif a.begins_with("--sun-yaw="): sun_yaw = float(a.substr(10))
		elif a.begins_with("--litter-wind="):
			var lw := a.substr(14).split(",")
			litter_wind = Vector2(float(lw[0]), float(lw[1]))
		elif a.begins_with("--stand="):
			stand = a.substr(8)
			stand_set = true
			forest = true
		elif a.begins_with("--day="): season_day = int(a.substr(6))
		elif a == "--drought": drought = true
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
	add_child(pond)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = 192
	dm.water_sources = [NodePath("../Pond")]
	add_child(dm)
	grass = GrassField3D.new()
	grass.name = "Grass"
	grass.fallback_wind = wind
	if OS.get_environment("GRASS_RADIUS") != "":
		grass.radius = float(OS.get_environment("GRASS_RADIUS")) # Which level of detail the far-match region sees.
	if grass_density > 0.0:
		grass.density = grass_density
	if OS.get_environment("GRASS_DIAG").contains("vf1"):
		grass.view_facing = 1.0
	grass.gpu_fill = gpu_fill
	add_child(grass)
	grass.data_map = grass.get_path_to(dm)
	if season_map:
		grass.season_source = grass.get_path_to(season_map)
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
		add_child(turf)
		if season_map:
			turf.season_source = turf.get_path_to(season_map)
		turf.data_map = turf.get_path_to(dm)
		turf.visible = not no_turf
		cover = VegetationField3D.new()
		cover.name = "Cover"
		cover.populate_on_ready = false
		cover.tile_size = 4.0
		cover.biome = _cover_biome()
		cover.forest_floor_enabled = not no_floor
		cover.litter_wind = litter_wind
		if clearing >= 0:
			_set_clearing()
		add_child(cover)
		cover.data_map = cover.get_path_to(dm)
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
			var ub := Biome.new()
			ub.species = [usp]
			ug_field = VegetationField3D.new()
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
	if weather:
		var wm := WeatherMap3D.new()
		wm.wind_speed = wind.length()
		add_child(wm)

	# Soil, tinted towards the grass where grass grows (GrassField3D.get_ground_shader_code()).
	var gm := ShaderMaterial.new()
	var gs := Shader.new()
	gs.code = "shader_type spatial;\n" + GrassField3D.get_ground_shader_code() + """
varying vec3 wp;
// Under the crowns grass_ground() lays the forest floor's litter (VegetationField3D's
// forest_floor_* globals): leaves, needles and moss by the stand above.
void vertex() { wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	ALBEDO = vegetation_far(wp, grass_ground(wp, vec3(0.11, 0.12, 0.06)), CAMERA_POSITION_WORLD);
	ROUGHNESS = 0.95;
	SPECULAR = grass_ground_specular(wp, 0.5);
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
	var water := MeshInstance3D.new()
	var wp := PlaneMesh.new()
	wp.size = Vector2(SIZE, SIZE)
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.05, 0.12, 0.15)
	wmat.roughness = 0.05
	wp.material = wmat
	water.mesh = wp
	water.position.y = -0.6
	add_child(water)
	var env := Environment.new()
	# A sky, not a flat colour: raster ambient and path traced sky light then come from the same place.
	var sky := Sky.new()
	var psm := ProceduralSkyMaterial.new()
	sky.sky_material = psm
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
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
	sun.rotation_degrees = Vector3(-28, sun_yaw, 0)
	sun.shadow_enabled = not OS.get_environment("GRASS_DIAG").contains("noshadow")
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
	cam.position = Vector3(6, height(6, 20) + 1.7, 20)
	add_child(cam)
	cam.look_at(Vector3(0, -0.5, -8))
	if (trample or close) and not test:
		# Closer, looking down on the trails.
		cam.position = Vector3(1.5, height(1.5, 17.0) + 1.6, 17.0)
		cam.look_at(Vector3(0.5, height(0.5, 10.0), 10.0))
	if far_view and not test:
		# From a corner, a little up, across the whole map: near plants, thinning, ground beyond.
		cam.position = Vector3(42, height(42, 42) + (2.0 if dolly else 7.0), 42)
		cam.look_at(Vector3(-20, height(-20, -20) - 2.0, -20))
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

# A closed wood (no tree patches) and, with the mask, an irregular clearing about 22 m across:
# the trees read the mask's red channel, so they stay out where it is 0, and the meadow, the
# sward and the edge plants fill the light (their shade rules) without a mask of their own.
func _set_clearing() -> void:
	for sp in cover.biome.species:
		var ps := sp as PlantSpecies
		if ps.canopy_radius > 0.0:
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
			var r := 11.0 + 2.5 * sin(3.0 * a) + 1.5 * sin(5.0 * a + 1.0)
			var v := smoothstep(r - 1.5, r + 1.5, w.length())
			img.set_pixel(x, y, Color(v, v, v, 1.0))
	cover.density_mask = ImageTexture.create_from_image(img)
	cover.density_mask_rect = Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE)

# 40 m up beside the clearing, looking down across it to its far edge and the wood beyond.
func _frame_clearing() -> void:
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

# Boulders for --rocks: rounded stand-ins, a little sunk, in front of the camera. Each is a disc
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
	for k in rocks:
		var d := rng.randf_range(3.0, 10.0)
		var q := cam.global_position + fwd * d + right * rng.randf_range(-0.45, 0.45) * d
		var r := rng.randf_range(0.3, 1.0)
		var y := height(q.x, q.z)
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		sm.radial_segments = 16
		sm.rings = 8
		sm.material = mat
		var mi := MeshInstance3D.new()
		mi.mesh = sm
		# Semi-axes r, 0.55 r, r; centre 0.15 r up: the rock meets the ground at 0.96 r.
		mi.transform = Transform3D(Basis(Vector3.UP, rng.randf() * TAU).scaled(Vector3(r, 0.55 * r, r)), Vector3(q.x, y + 0.15 * r, q.z))
		add_child(mi)
		list.append(Vector4(q.x, y, q.z, 0.96 * r))
	if not rocks_through:
		for f in [grass, under, turf, cover, ug_field]:
			if f:
				f.obstacles = list
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
	if forest:
		for sp in b.species:
			(sp as PlantSpecies).set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.4))
			(sp as PlantSpecies).set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
		# Bigger plants first: the edge shrubs and bracken claim their space before the herbs.
		var floor_sp := _stand_floor()
		b.species = _stand_trees() + floor_sp.slice(0, 3) + b.species + floor_sp.slice(3)
	if swamp:
		b.species = _wetland_species() + b.species
	if no_far:
		for sp in b.species:
			(sp as PlantSpecies).far_color = Color(0, 0, 0, 0)
	else:
		_set_far_colors(b)
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
func _sward_shade_check(c: Vector2, open_density: float) -> void:
	var brk := VegetationField3D.new()
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

func _checks() -> void:
	_season_checks()
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
	_check("far ring thinned (LOD)", far_density < near_density * 0.3, far_density)
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
	# Same camera, same field.
	grass.rebuild()
	for i in 20:
		grass.update_at(cam.global_position)
	_check("deterministic", grass.get_blade_count() == count, grass.get_blade_count())
	# The camera moves: tiles follow without rebuilding everything.
	grass.update_at(cam.global_position + Vector3(30, 0, 0))
	print("GRASS_MOVE tiles=%d build_ms=%.1f" % [grass.get_tile_count(), grass.get_build_time_msec()])
	grass.update_at(cam.global_position)
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
						if cd.r < 0.0 or cd.r > 1.0 or absf(floorf(cd.g) - floorf(sp.visibility_range)) > 1e-3 or absf(ch.visibility_range_end - sp.visibility_range * 1.05) > 1e-3:
							bad += 1
		_check("plants carry rank and range for thinning", ranged > 1000 and bad == 0, "%d ranged, %d bad" % [ranged, bad])

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
	if n == 2:
		dm.bake()
		if rocks > 0:
			_place_rocks()
		if cover:
			cover.populate()
			var counts := []
			for i in cover.biome.species.size():
				counts.append(cover.get_placements(i).size())
			print("GRASS_COVER %s = %s ms=%.1f" % ["(tree, bramble, bracken, foxglove) flowers, sedge, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry grass, tall flowers, tall dry flowers (fern, bilberry, moss, litter)", str(counts), cover.get_populate_time_msec()])
			if ug_field:
				ug_field.populate()
				print("GRASS_UNDERGROWTH placements=%d" % ug_field.get_placements(0).size())
				for g in [grass, under, turf]:
					if g:
						g.rebuild()
				_frame_undergrowth()
			if forest:
				grass.rebuild()
				under.rebuild()
				turf.rebuild()
				if clearing >= 0:
					_frame_clearing()
				else:
					_frame_forest()
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
	if n == frames - 30 and not test:
		t_frames = Time.get_ticks_usec()
	if test and n == frames:
		_checks()
	if test and n > frames:
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
		return
	if far_match and n == frames + 12:
		await RenderingServer.frame_post_draw
		var b := _region_mean()
		get_viewport().get_texture().get_image().save_png("user://far_match_b.png")
		print("GRASS_FAR_MATCH blades=%s far_ground=%s ratio=%s" % [str(far_a), str(b), str(Color(far_a.r / b.r, far_a.g / b.g, far_a.b / b.b))])
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
	set_process(false)
	if t_frames > 0:
		var blades := grass.get_blade_count() + (under.get_blade_count() if under else 0)
		print("GRASS_FRAME_MS %.1f blades=%d" % [(Time.get_ticks_usec() - t_frames) / 30000.0, blades])
	await RenderingServer.frame_post_draw
	if prims:
		await _print_prims()
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()


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

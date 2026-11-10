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
#   --out=path.png   save the last frame
#   --clip=dir       save every frame from 20 on as PNGs (for a GIF)
#   --layers=0       only the tall blades (no undercoat, ground tint, view facing, clover, flowers)
#   --close          the closer view used by --trample
#   --notex          plants without their generated leaf textures
#   --forest         a wood of stand-in trees: forest floor (litter, moss, bilberry, fern) in
#                    their shade, meadow in the open (PlantSpecies.shade_range)
#   --forest=in      the same, standing inside the wood
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
var cam: Camera3D
var fails := 0
var trample := false
var layers := true
var close := false
var forest := false
var forest_in := false
var ground_mat: ShaderMaterial
var notex := false
var t_frames := 0
var under: GrassField3D
var cover: VegetationField3D
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
		elif a == "--forest=in":
			forest = true
			forest_in = true
		elif a == "--notex": notex = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a.begins_with("--clip="): clip = a.substr(7)
		elif a.begins_with("--wind="):
			var p := a.substr(7).split(",")
			wind = Vector2(float(p[0]), float(p[1]))
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
	add_child(grass)
	grass.data_map = grass.get_path_to(dm)
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
		# Under a dense carpet the soil is thatch in the blades' shade: darker than the blades themselves.
		under.ground_tint = Color(0.035, 0.045, 0.018, 1.0)
		grass.ground_tint_enabled = false # The undercoat grows everywhere, so it owns the tint.
		add_child(under)
		under.data_map = under.get_path_to(dm)
		cover = VegetationField3D.new()
		cover.name = "Cover"
		cover.populate_on_ready = false
		cover.tile_size = 4.0
		cover.biome = _cover_biome()
		add_child(cover)
		cover.data_map = cover.get_path_to(dm)
		if forest:
			# The sward stays out from under the crowns.
			for g in [grass, under]:
				var r: PlantSpecies = g.rules if g.rules else PlantSpecies.new()
				# A wide fade: the sward thins out over a few metres under the crown edge.
				r.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.3))
				r.set_rule_fade(PlantSpecies.RULE_SHADE, 0.4)
				g.rules = r
				g.canopy_field = g.get_path_to(cover)
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
// Forest floor: canopy shade (VegetationField3D.get_shade_at, baked) turns the soil to humus
// under a mottled leaf layer, so the litter reads beyond the near 3D leaves.
uniform sampler2D shade_map : filter_linear, hint_default_black;
uniform float map_size = 96.0;
float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vn(vec2 p) {
	vec2 i = floor(p); vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);
	return mix(mix(h21(i), h21(i + vec2(1, 0)), f.x), mix(h21(i + vec2(0, 1)), h21(i + vec2(1, 1)), f.x), f.y);
}
void vertex() { wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }
void fragment() {
	vec3 soil = grass_ground(wp, vec3(0.11, 0.12, 0.06));
	float shade = texture(shade_map, wp.xz / map_size + 0.5).r;
	float leaves = vn(wp.xz * 9.0) * 0.6 + vn(wp.xz * 31.0) * 0.4;
	vec3 humus = mix(vec3(0.035, 0.025, 0.015), vec3(0.13, 0.08, 0.04), smoothstep(0.35, 0.75, leaves));
	humus *= 0.8 + 0.4 * vn(wp.xz * 0.7);
	ALBEDO = mix(soil, humus, smoothstep(0.25, 0.6, shade));
	ROUGHNESS = 0.95;
}
"""
	gm.shader = gs
	ground_mat = gm
	var ground := MeshInstance3D.new()
	ground.mesh = _ground_mesh()
	ground.material_override = gm
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
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_sky_shader_enabled = true
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-28, 150, 0)
	sun.shadow_enabled = true
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

# Stand at a wood's edge: the tree nearest the usual view, seen from the open side.
func _frame_forest() -> void:
	var trees := cover.get_placements(0)
	if trees.is_empty():
		return
	var res := 192
	var img := Image.create(res, res, false, Image.FORMAT_L8)
	for z in res:
		for x in res:
			var p := Vector3((x + 0.5) / res * SIZE - SIZE * 0.5, 0, (z + 0.5) / res * SIZE - SIZE * 0.5)
			img.set_pixel(x, z, Color(cover.get_shade_at(p), 0, 0))
	ground_mat.set_shader_parameter("shade_map", ImageTexture.create_from_image(img))
	var c := Vector3(1.5, 0, 17.0)
	var best: Vector3 = trees[0]
	for t in trees:
		if Vector2(t.x - c.x, t.z - c.z).length() < Vector2(best.x - c.x, best.z - c.z).length():
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
	cam.position = Vector3(eye.x, height(eye.x, eye.z) + 1.5, eye.z)
	cam.look_at(Vector3(at.x, height(at.x, at.z) + (-0.4 if forest_in else 0.3), at.z))
	print("GRASS_FOREST tree=%s eye=%s shade_under=%.2f" % [str(best), str(cam.position), cover.get_shade_at(best)])

# Ground plants from PlantGenerator (three variants each), in patches (PlantSpecies.patch_*),
# drawn near only: wildflower drifts, plantain, clover between the tall grass, sedge by the water.
func _variants(preset: int, count: int, size := 1.0) -> Array:
	var gen := PlantGenerator.new()
	gen.textures = not notex
	gen.preset = preset
	gen.size = size
	gen.apply_preset_colors()
	return gen.generate_variants(count)

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
	var b := Biome.new()
	b.species = [flowers, sedge, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry]
	if forest:
		for sp in b.species:
			(sp as PlantSpecies).set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.0, 0.4))
			(sp as PlantSpecies).set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
		b.species = [_tree_species()] + b.species + _floor_species()
	return b

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

# Forest floor (docs/ideas/vegetation-placement.md, "Canopy shade rule"): litter everywhere in
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
	bilberry.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.45, 1.0))
	bilberry.set_rule_fade(PlantSpecies.RULE_SHADE, 0.15)
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
	litter.density = 1.0
	litter.scale_range = Vector2(1.2, 1.8)
	litter.set_rule_range(PlantSpecies.RULE_SHADE, Vector2(0.3, 1.0))
	litter.set_rule_fade(PlantSpecies.RULE_SHADE, 0.2)
	litter.visibility_range = 15.0
	return [fern, bilberry, moss, litter]

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

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

func _checks() -> void:
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
				var p := mm.get_instance_transform(i).origin
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
	_check("near density about 60 /m2", near_density > 45.0 and near_density < 70.0, near_density)
	_check("far ring thinned (LOD)", far_density < near_density * 0.3, far_density)
	_check("no grass under water", wet == 0, wet)
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
	# Same camera, same field.
	grass.rebuild()
	for i in 20:
		grass.update_at(cam.global_position)
	_check("deterministic", grass.get_blade_count() == count, grass.get_blade_count())
	# The camera moves: tiles follow without rebuilding everything.
	grass.update_at(cam.global_position + Vector3(30, 0, 0))
	print("GRASS_MOVE tiles=%d build_ms=%.1f" % [grass.get_tile_count(), grass.get_build_time_msec()])
	grass.update_at(cam.global_position)

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

func _process(dt: float) -> void:
	n += 1
	if n == 2:
		dm.bake()
		if cover:
			cover.populate()
			var counts := []
			for i in cover.biome.species.size():
				counts.append(cover.get_placements(i).size())
			print("GRASS_COVER %s = %s ms=%.1f" % ["(tree) flowers, sedge, plantain, clover, nettle, dock, thistle, tussock, yarrow, dry grass (fern, bilberry, moss, litter)", str(counts), cover.get_populate_time_msec()])
			if forest:
				grass.rebuild()
				under.rebuild()
				_frame_forest()
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
	if not clip.is_empty() and n >= 20:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(clip.path_join("f%03d.png" % (n - 20)))
		if n >= frames:
			get_tree().quit()
		return
	if n < frames:
		return
	set_process(false)
	if t_frames > 0:
		var blades := grass.get_blade_count() + (under.get_blade_count() if under else 0)
		print("GRASS_FRAME_MS %.1f blades=%d" % [(Time.get_ticks_usec() - t_frames) / 30000.0, blades])
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

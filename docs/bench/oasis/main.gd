extends Node3D
# Oasis spring pools (OasisSprings3D): a spring in a hollow of sand, its pool sized by the water balance
# (A = Q / (E + I)) with a noise outline, and the sand boils at its vents.
#   --view=pool     the whole pool from 8 m up on the shore (default)
#   --view=vent     2 m above the water, steeply down onto the vents through the clear water
#   --view=top      straight down from 40 m (the outline)
#   --view=lag      the interdune lag's granule ripples close up (--lag-at=x,z --lag-yaw --lag-h --lag-look --sun-alt --sun-side --fov)
#   --raster        raster instead of path tracing
#   --taa           temporal AA (raster stills for docs or PT comparison)
#   --no-nmap       the bench ground without its noise normal map
#   --spp=N         path traced samples per pixel (64)
#   --bounces=N     path traced bounces (the Environment's, 3 by default)
#   --frames=N      frames before saving (180; path tracing waits for its surfaces too)
#   --plant-variants=N  meshes per oasis plant species (3; 1 keeps a path traced still within 15 GB)
#   --discharge=Q   the spring's discharge, L/s (0.05)
#   --seed=N        OasisSprings3D.seed
#   --wind=W        wind over the pool, m/s (2)
#   --no-water      hide the water: the bed and the boils alone
#   --linear        linear tone mapping (measuring colours)
#   --no-boils      no sand boils (and no surface dome over them)
#   --synthetic     the bench's own hollow instead of a TerrainGenerator3D sand sea carved by carve_oases()
#   --terrain-seed=N  TerrainGenerator3D.seed (1)
#   --debug=N       path tracing debug view (Environment.pathtracing_debug_mode)
#   --set=S:V       any project setting
#   --pool-set=P:V  set a property of the pool's PhysXWaterSurface3D (diagnostics)
#   --water-param=P:V  set a uniform of its water material
#   --out=PATH      save the frame and quit
#   --test          checks only (no image)
#   --no-palms      terrain mode without the date palm stand and desert shrubs round the pool (TreeGenerator's
#                   STAND_DATE_OASIS on a VegetationField3D, wild clumps kept to the water)

var view := "pool"
var raster := false
var spp := 64
var frames := 180
var plant_variants := 3
var discharge := 0.05
var seed := 0
var wind := 2.0
var out := ""
var test := false
var oasis: OasisSprings3D
var n := 0
var fails := 0
var cam: Camera3D

var origin := Vector3.ZERO # the spring: x, its pool's level, z
var synthetic := false
var terrain_seed := 1
var gen: Node3D
var dry_oasis: OasisSprings3D
var surface: GroundSurface3D
var world_env: Environment


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--view="): view = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--test": test = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--plant-variants="): plant_variants = maxi(int(a.substr(17)), 1)
		elif a.begins_with("--discharge="): discharge = float(a.substr(12))
		elif a.begins_with("--seed="): seed = int(a.substr(7))
		elif a.begins_with("--wind="): wind = float(a.substr(7))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--synthetic": synthetic = true
		elif a.begins_with("--terrain-seed="): terrain_seed = int(a.substr(15))
		elif a.begins_with("--set="):
			var kv := a.substr(6).split(":", true, 1)
			ProjectSettings.set_setting(kv[0], str_to_var(kv[1]))

	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.2, 0.42, 0.78)
	sky_mat.sky_horizon_color = Color(0.72, 0.76, 0.8)
	sky_mat.ground_horizon_color = Color(0.72, 0.76, 0.8)
	# Below the horizon past the generated ground: the far sand seen through the desert's haze, not a dark void.
	sky_mat.ground_bottom_color = Color(0.62, 0.58, 0.52)
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR if "--linear" in OS.get_cmdline_user_args() else Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster and not test
	world_env = env
	env.pathtracing_samples_per_pixel = spp
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--bounces="):
			env.pathtracing_max_bounces = int(a.substr(10))
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--debug="):
			env.pathtracing_debug_mode = int(a.substr(8))
	if "--taa" in OS.get_cmdline_user_args():
		get_viewport().use_taa = true # --taa: temporal AA for raster stills (resolves shadow and dither noise)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45 if view == "hero" else -55, -150, 0)
	sun.shadow_enabled = true
	sun.light_energy = 1.4
	add_child(sun)

	oasis = OasisSprings3D.new()
	oasis.seed = seed
	oasis.sand_boils = not ("--no-boils" in OS.get_cmdline_user_args())
	# The unit checks keep the bench's own hollow: fixed springs, quick and exact.
	if synthetic or test or not ClassDB.class_exists("TerrainGenerator3D"):
		oasis.springs = PackedVector4Array([Vector4(0.0, 0.0, 0.0, discharge)])
		add_child(oasis)
		_ground(oasis.get_outline(0))
	else:
		add_child(oasis)
		if not _terrain():
			get_tree().quit(1)
			return
	# A light air (no weather here): palms round a spring shelter it.
	var pool: PhysXWaterSurface3D = oasis.get_pool(0)
	pool.follow_weather = false
	pool.wind_speed = wind
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--pool-set="):
			var kv := a.substr(11).split(":", true, 1)
			pool.set(kv[0], str_to_var(kv[1]))
		elif a.begins_with("--water-param="):
			var kw := a.substr(14).split(":", true, 1)
			pool.get_water_material().set_shader_parameter(kw[0], str_to_var(kw[1]))
	if "--no-water" in OS.get_cmdline_user_args():
		pool.visible = false # the bed and the boils alone

	cam = Camera3D.new()
	cam.fov = 55
	add_child(cam)
	var r := oasis.get_pool_radius(0) if oasis.get_pool_count() > 0 else 12.0
	match view:
		"vent":
			cam.look_at_from_position(origin + Vector3(-1.2, 2.0, 1.6), origin + Vector3(0.0, -1.8, 0.0))
		"top":
			cam.look_at_from_position(origin + Vector3(0.0, 40.0, 0.01), origin)
		"hero":
			# Three-quarters from 12 m up, outside the palm grove, looking across the pool and its palms at the highest dune ridge round it,
			# so the ridge and the sky close the frame (no edge of the generated ground in view).
			var best_dir := Vector3(0, 0, -1)
			var best_h := -INF
			if gen != null:
				for k in 24:
					var a := TAU * k / 24.0
					var d := Vector3(cos(a), 0.0, sin(a))
					var h := -INF
					for dist in [40.0, 60.0, 80.0, 110.0]:
						h = maxf(h, float(gen.get_height(origin + d * dist)) - origin.y)
					if h > best_h:
						best_h = h
						best_dir = d
			var side := best_dir.cross(Vector3.UP).normalized()
			var eye := origin - best_dir * (r * 3.0) + side * (r * 1.2)
			eye.y = (float(gen.get_height(eye)) if gen != null else origin.y) + 12.0
			cam.fov = 50
			cam.look_at_from_position(eye, origin + best_dir * (r * 0.8) + Vector3(0.0, 1.5, 0.0))
			print("hero: ridge %.1f m above the pool toward %s" % [best_h, str(best_dir)])
		"lag":
			# Down onto the interdune lag (granule ripples): eye --lag-h (1.5) m over --lag-at=x,z (-120,44), looking
			# --lag-yaw degrees (0 = -Z) at the ground --lag-look (4) m ahead; the sun --sun-alt (12) degrees high from
			# --sun-side (90) degrees round from the view, so it rakes across the crests.
			var la := _arg("--lag-at=", "-120,44").split(",")
			var lx := float(la[0])
			var lz := float(la[1])
			# --lag-eye=x,y,z --lag-target=x,y,z: an exact framing (y above the ground there) instead of the search.
			var yaw := deg_to_rad(float(_arg("--lag-yaw=", "0")))
			var fwd := Vector3(-sin(yaw), 0.0, -cos(yaw))
			var ground := func(x: float, z: float) -> float: return float(gen.get_height(Vector3(x, 0.0, z))) if gen != null else origin.y
			var sr := float(_arg("--lag-search=", "0"))
			if sr > 0.0:
				# The lag lies on flat, low interdune floors (thin sand, ground within ~10 degrees of level): the flattest
				# low spot within sr m of --lag-at, flat over the 6 m ahead too.
				var best := INF
				var bx := lx
				var bz := lz
				var s0 := 3.0
				var gx := -sr
				while gx <= sr:
					var gz := -sr
					while gz <= sr:
						var x := lx + gx
						var z := lz + gz
						var h0: float = ground.call(x, z)
						var worst := 0.0
						for k in 4:
							var t := float(k + 1) * 1.6
							var px := x + fwd.x * t
							var pz := z + fwd.z * t
							var hx: float = float(ground.call(px + s0, pz)) - float(ground.call(px - s0, pz))
							var hz: float = float(ground.call(px, pz + s0)) - float(ground.call(px, pz - s0))
							worst = maxf(worst, Vector2(hx, hz).length() / (2.0 * s0))
						var score: float = worst * 10.0 + (h0 - float(ground.call(lx, lz))) * 0.02
						if score < best:
							best = score
							bx = x
							bz = z
						gz += 4.0
					gx += 4.0
				print("lag search: (%.0f, %.0f) slope score %.3f" % [bx, bz, best])
				lx = bx
				lz = bz
			var eye := Vector3(lx, ground.call(lx, lz) + float(_arg("--lag-h=", "1.5")), lz)
			var tgt := eye + fwd * float(_arg("--lag-look=", "4"))
			tgt.y = ground.call(tgt.x, tgt.z)
			if _arg("--lag-eye=", "") != "":
				var ea := _arg("--lag-eye=", "").split(",")
				var ta := _arg("--lag-target=", "0,0,0").split(",")
				eye = Vector3(float(ea[0]), float(ground.call(float(ea[0]), float(ea[2]))) + float(ea[1]), float(ea[2]))
				tgt = Vector3(float(ta[0]), float(ground.call(float(ta[0]), float(ta[2]))) + float(ta[1]), float(ta[2]))
				yaw = atan2(-(tgt - eye).x, -(tgt - eye).z)
			cam.fov = float(_arg("--fov=", "50"))
			cam.look_at_from_position(eye, tgt)
			for n in get_children():
				if n is DirectionalLight3D:
					n.rotation_degrees = Vector3(-float(_arg("--sun-alt=", "12")), rad_to_deg(yaw) + float(_arg("--sun-side=", "90")), 0.0)
			print("lag view: eye %s looking %s" % [str(eye), str(tgt)])
		_:
			cam.look_at_from_position(origin + Vector3(-r * 1.05, 8.0, r * 1.25), origin + Vector3(0.0, -1.0, -r * 0.1))
	cam.current = true


func _arg(prefix: String, fallback: String) -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with(prefix):
			return a.substr(prefix.length())
	return fallback


# A hollow in dune sand carved to the pool's outline (as terrain would: the shore just inside it), 2.4 m deep
# in the middle with an uneven, algae-filmed bed, a gentle beach and low dunes past it. Static collision for the pool's seabed and the vents.
func _ground(outline: PackedVector2Array) -> void:
	var c := Vector2.ZERO
	for p in outline:
		c += p
	c /= outline.size()
	var size := 70.0
	var cells := 280
	var step := size / cells
	var noise := FastNoiseLite.new()
	noise.frequency = 0.035
	noise.seed = 7
	var bed_noise := FastNoiseLite.new()
	bed_noise.frequency = 0.18
	bed_noise.seed = 11
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var h := func(x: float, z: float) -> float:
		var q := Vector2(x, z) - c
		var t := fposmod(q.angle(), TAU) / TAU * outline.size()
		var i := int(t) % outline.size()
		var j := (i + 1) % outline.size()
		var rad := lerpf((outline[i] - c).length(), (outline[j] - c).length(), t - floor(t))
		var d := q.length() / rad
		# The bed: deepest in the middle, uneven (lumps and runnels a few dm high), meeting the water at 0.92 of
		# the outline; a gentle beach past it, so the water's outline lies a few cm under the sand.
		var bowl := -2.4 * pow(maxf(1.0 - pow(d / 0.92, 2.0), 0.0), 1.3)
		var lumps := 0.3 * bed_noise.get_noise_2d(x, z) * clampf((0.85 - d) / 0.3, 0.0, 1.0)
		var beach := 0.35 * smoothstep(0.85, 1.4, d)
		return bowl + lumps + beach + 0.6 * noise.get_noise_2d(x, z) * clampf(d - 1.3, 0.0, 1.0)
	for zi in cells:
		for xi in cells:
			var x0 := -size * 0.5 + xi * step
			var z0 := -size * 0.5 + zi * step
			var p := [Vector3(x0, h.call(x0, z0), z0), Vector3(x0 + step, h.call(x0 + step, z0), z0),
					Vector3(x0, h.call(x0, z0 + step), z0 + step), Vector3(x0 + step, h.call(x0 + step, z0 + step), z0 + step)]
			for i in [0, 1, 2, 1, 3, 2]:
				st.set_color(_bed_color(p[i], 0.0, bed_noise))
				st.set_uv(Vector2(p[i].x, p[i].z) * 0.25)
				st.add_vertex(p[i])
	st.generate_normals()
	_ground_mesh(st.commit())


# The ground's sand (texture times the vertex colour) and its static collision, for the pool's seabed and the vents.
func _ground_mesh(mesh: ArrayMesh) -> void:
	# A shader, not StandardMaterial3D's vertex colour: the path tracer reads COLOR from custom shaders only.
	var mat := ShaderMaterial.new()
	mat.shader = Shader.new()
	mat.shader.code = "shader_type spatial;\nuniform sampler2D sand : source_color, filter_linear_mipmap, repeat_enable;\nvoid fragment() {\n\tALBEDO = texture(sand, UV).rgb * COLOR.rgb;\n\tROUGHNESS = 0.95;\n}\n"
	if "--plain-bed" in OS.get_cmdline_user_args(): # diagnostics: one flat colour, no texture or vertex colour
		mat.shader.code = "shader_type spatial;\nuniform sampler2D sand : source_color;\nvoid fragment() {\n\tALBEDO = vec3(0.75, 0.68, 0.52);\n\tROUGHNESS = 0.95;\n}\n"
	var tex := NoiseTexture2D.new()
	tex.seamless = true
	tex.width = 512
	tex.height = 512
	var tn := FastNoiseLite.new()
	tn.frequency = 0.08
	tex.noise = tn
	var grad := Gradient.new()
	grad.set_color(0, Color(0.68, 0.6, 0.45))
	grad.set_color(1, Color(0.86, 0.77, 0.6))
	tex.color_ramp = grad
	mat.set_shader_parameter("sand", tex)
	mesh.surface_set_material(0, mat)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	add_child(mi)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)


# A sand sea from TerrainGenerator3D with one spring on its lowest interdune floor; the generator carves the hollow to
# the pool (carve_oases()), and a 96 m patch of its ground round the spring becomes the bench's mesh and collision.
func _terrain() -> bool:
	gen = ClassDB.instantiate("TerrainGenerator3D")
	gen.name = "Generator"
	# Only what a sand sea needs (the heavy stages are on by default).
	for kv in [["glacial_enabled", false], ["glacial_converged", false], ["tile_erosion_enabled", false],
			["strata_enabled", false], ["guide_preview", false], ["dunes_enabled", false], ["deposition", 0.0],
			["moraine_age", 0.0], ["scree_age", 0.0], ["meander_age", 0.0], ["outwash_age", 0.0], ["lake_count", 0],
			["base_uplift", 0.02], ["size", Vector2(1024, 1024)], ["cell_size", 16.0], ["seed", terrain_seed],
			["sand_sea_rect", Rect2(-448, -448, 896, 896)], ["oasis_count", 1]]:
		gen.set(kv[0], kv[1])
	add_child(gen)
	oasis.spring_source = oasis.get_path_to(gen)
	gen.oasis_springs = gen.get_path_to(oasis)
	var t0 := Time.get_ticks_msec()
	gen.generate()
	gen.carve_oases()
	var springs: PackedVector4Array = gen.get_springs()
	print("terrain: %d ms, %d spring(s), %d pool(s)" % [Time.get_ticks_msec() - t0, springs.size(), oasis.get_pool_count()])
	if springs.is_empty() or oasis.get_pool_count() == 0:
		push_error("no oasis on this sand sea (try another --terrain-seed)")
		return false
	origin = Vector3(springs[0].x, springs[0].y, springs[0].z)
	print("depth at the spring %.3f m, at the pool centre %.3f m" % [origin.y - gen.get_height(origin), origin.y - gen.get_height(oasis.get_pool(0).global_position)])
	var pts := 321
	var step := 0.3
	var o := Vector2(origin.x, origin.z) - Vector2.ONE * step * (pts - 1) * 0.5
	var img: Image = gen.sample_heights(o, step, Vector2i(pts, pts))
	var bed_noise := FastNoiseLite.new()
	bed_noise.frequency = 0.18
	bed_noise.seed = 11
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for zi in pts:
		for xi in pts:
			var y := img.get_pixel(xi, zi).r
			var p := Vector3(o.x + xi * step, y, o.y + zi * step)
			st.set_color(_bed_color(p, origin.y, bed_noise))
			st.set_uv(Vector2(p.x, p.z) * 0.25)
			st.add_vertex(p)
	for zi in pts - 1:
		for xi in pts - 1:
			var i := zi * pts + xi
			for k in [i, i + 1, i + pts, i + 1, i + pts + 1, i + pts]:
				st.add_index(k)
	st.generate_normals()
	var mesh := st.commit()
	# The ground's own surface (GroundSurface3D, as the terrain has it): dry sand, the wet margin up to 15 cm over the
	# pool's level and, in this desert's climate, the salt crust at its top (water_fringe, salt_crusts).
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.78, 0.68, 0.5)
	mat.roughness = 0.95
	var nt := NoiseTexture2D.new()
	nt.seamless = true
	nt.as_normal_map = true
	nt.bump_strength = 4.0
	nt.width = 512
	nt.height = 512
	var nz := FastNoiseLite.new()
	nz.frequency = 0.03
	nz.fractal_octaves = 5
	nt.noise = nz
	mat.normal_enabled = "--no-nmap" not in OS.get_cmdline_user_args() # --no-nmap: no bench noise normal map (it blotches under a low sun)
	mat.normal_texture = nt
	mat.uv1_triplanar = true
	mat.uv1_world_triplanar = true
	mat.uv1_scale = Vector3(0.5, 0.5, 0.5)
	mat.ground_deform_enabled = true
	mesh.surface_set_material(0, mat)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	add_child(mi)
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = mesh.create_trimesh_shape()
	body.add_child(cs)
	add_child(body)
	var wm := WeatherMap3D.new()
	wm.climate = load("res://desert_climate.gd").new()
	add_child(wm)
	var probe := Marker3D.new()
	probe.name = "Probe"
	add_child(probe)
	probe.global_position = origin
	surface = GroundSurface3D.new()
	surface.ground_type = GroundSurface3D.GROUND_SAND
	surface.moisture = 0.0
	surface.material = mat
	add_child(surface)
	surface.height_source = surface.get_path_to(gen)
	surface.follow_target = surface.get_path_to(probe)
	surface.water_sources = [surface.get_path_to(oasis.get_pool(0))]
	if not test and not ("--no-palms" in OS.get_cmdline_user_args()) and ClassDB.class_exists("TreeGenerator"):
		_palms()
	return true


# Date palms round the spring and desert shrubs on the sand beyond: TreeGenerator's date oasis stand placed by a
# VegetationField3D over a data map of the bench's ground, the pool its water (wild clumps keep to water).
func _palms() -> void:
	var dm := TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = 96.0
	dm.resolution = 96
	dm.bake_on_ready = false
	add_child(dm)
	dm.global_position = Vector3(origin.x, 0.0, origin.z)
	dm.height_source = dm.get_path_to(gen)
	dm.water_sources = [dm.get_path_to(oasis.get_pool(0))]
	# The habitat rules read the map's climate: the bench's hot desert (desert_climate.gd), so the palms, which drink
	# the groundwater not the rain, take the spring and the shrubs the sand.
	var recipe := ClimateRecipe.new()
	recipe.temperature_january = 15.0
	recipe.temperature_july = 34.0
	recipe.reference_height = 0.0
	recipe.precipitation = 60.0
	dm.climate_recipe = recipe
	# The oasis stand (PlantGenerator.make_desert DESERT_OASIS): trees' date palm stand (tended palms, wild clumps by
	# the water, olives beneath) on the spring's wet ground, a ring of desert shrubs where the water lies deeper, and
	# reed and rush round the pool's shore (spring_pools).
	var biome := Biome.new()
	var species: Array = []
	for r in PlantGenerator.make_desert(PlantGenerator.DESERT_OASIS, plant_variants):
		if r is PlantSpecies:
			species.append(r)
	biome.species = species
	var f := VegetationField3D.new()
	f.name = "Palms"
	f.populate_on_ready = false
	f.tile_size = 32.0
	f.biome = biome
	add_child(f)
	f.global_position = Vector3(origin.x, 0.0, origin.z)
	f.data_map = f.get_path_to(dm)
	f.spring_pools = f.get_path_to(oasis)
	var err := dm.bake()
	if err != OK:
		push_error("oasis: palm data map did not bake (%d)" % err)
		return
	if dm.bake_climate() != OK:
		push_error("oasis: palm climate did not bake")
		return
	var perr := f.populate()
	print("palms: %d species, %d plants (populate %d)" % [species.size(), f.get_plant_count(), perr])
	if f.get_plant_count() == 0 or "--palm-debug" in OS.get_cmdline_user_args():
		for d in [6.0, 12.0, 20.0, 30.0]:
			var q := Vector3(origin.x + d, 0.0, origin.z)
			q.y = dm.get_height_at(q)
			print("palm site +%.0f m: water %.1f m depth %.2f slope %.1f T %.1f P %.0f aridity %.3f soil %.2f" % [d,
					dm.get_water_distance_at(q), dm.get_water_depth_at(q), dm.get_slope_at(q), dm.get_temperature_at(q),
					dm.get_precipitation_at(q), dm.get_aridity_at(q), dm.get_soil_depth_at(q)])
			print("    wetness %.2f curvature %.3f height %.1f" % [dm.get_wetness_at(q), dm.get_curvature_at(q), q.y])
			for sp in species:
				print("    ", sp.resource_name, " ", sp.get_suitability_at(dm, q))
		var sp0: PlantSpecies = species[0]
		for r in PlantSpecies.RULE_MAX:
			var rr: Vector2 = sp0.get_rule_range(r)
			if rr != Vector2.ZERO:
				print("  palm rule %d: %s" % [r, str(rr)])


# Wet sand at the waterline, and a film of algae on the bed below (greener with depth, patchy).
func _bed_color(p: Vector3, level: float, bed_noise: FastNoiseLite) -> Color:
	var y := p.y - level
	# Wet sand is some 35% darker (water fills the pores); capillary rise wets it up to about 15 cm over the level.
	var wet := 1.0 - 0.35 * clampf((0.15 - y) / 0.08, 0.0, 1.0) * clampf((y + 0.6) / 0.6, 0.4, 1.0)
	var algae := clampf(-y / 1.5, 0.0, 1.0) * (0.55 + 0.45 * bed_noise.get_noise_2d(p.x * 2.3, p.z * 2.3))
	return Color(wet, wet, wet).lerp(Color(0.5, 0.58, 0.38), clampf(algae, 0.0, 0.8))


func _check(what: String, ok: bool, detail: String) -> void:
	print(("PASS " if ok else "FAIL ") + what + "  (" + detail + ")")
	if not ok:
		fails += 1


func _physics_process(_delta: float) -> void:
	n += 1
	# The wet margin and the salt crust grow over the first frames (ten times faster), as the ground bench lets them.
	if surface != null:
		Engine.time_scale = 10.0 if n < 240 else 1.0
		# Raster while they grow (a path traced frame takes seconds in the cloud), path traced from then on.
		if not raster and not test:
			world_env.pathtracing_enabled = n >= 240
	if "--probe" in OS.get_cmdline_user_args() and n % 30 == 0:
		var pool: PhysXWaterSurface3D = oasis.get_pool(0)
		var lo := 1e9
		var hi := -1e9
		var row := ""
		for i in 40:
			var x := -12.0 + i * 0.6
			var hh := pool.sample_height(origin + Vector3(x, 0.0, 3.0))
			if hh > -1e8:
				lo = minf(lo, hh)
				hi = maxf(hi, hh)
			row += "%.3f " % hh if hh > -1e8 else "  .   "
		print("PROBE %d min %.4f max %.4f | %s" % [n, lo, hi, row])
		var wm := pool.get_water_material()
		print("  medium a %s s %s sediment a %s s %s" % [wm.get_shader_parameter("pathtraced_absorption"), wm.get_shader_parameter("pathtraced_scattering"), wm.get_shader_parameter("sediment_absorption"), wm.get_shader_parameter("sediment_scattering")])
		if n >= 300:
			get_tree().quit()
	if test and n == 30:
		var a := OasisSprings3D.pool_area(0.05, 2.5, 1.0)
		_check("water balance: 0.05 L/s against 3.5 m a year", absf(a - 450.8) < 0.5, "%.1f m2" % a)
		_check("no weather: evaporation 2.5 m a year", absf(oasis.evaporation_at(0.0) - 2.5) < 1e-4, "%.2f" % oasis.evaporation_at(0.0))
		_check("one pool per spring", oasis.get_pool_count() == 1, str(oasis.get_pool_count()))
		var pool: PhysXWaterSurface3D = oasis.get_pool(0)
		_check("pool radius from its area", absf(oasis.get_pool_radius(0) - sqrt(a / PI)) < 0.01, "%.2f m" % oasis.get_pool_radius(0))
		var vents := oasis.get_vents(0)
		_check("one to three vents", vents.size() >= 1 and vents.size() <= 3, str(vents.size()))
		var wet := 0
		for v in vents:
			if pool.sample_depth(Vector3(v.x, origin.y, v.z)) > 0.5:
				wet += 1
		_check("every vent under water, on the bed", wet == vents.size(), "%d of %d, bed %.2f m" % [wet, vents.size(), vents[0].y])
		var boils := 0
		for c in oasis.get_children(true):
			if c is MeshInstance3D:
				boils += 1
		_check("a sand boil at each vent", boils == vents.size(), str(boils))
		# Two springs at different places never share an outline (the no-repetition rule).
		oasis.springs = PackedVector4Array([Vector4(0, 0, 0, 0.05), Vector4(200, 0, 0, 0.05)])
		var m0: Mesh = oasis.get_pool(0).surface_mesh
		var m1: Mesh = oasis.get_pool(1).surface_mesh
		var v0: PackedVector3Array = m0.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var v1: PackedVector3Array = m1.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var diff := 0.0
		for i in v0.size():
			diff = maxf(diff, (v0[i] - v1[i]).length())
		_check("two springs, two outlines", diff > 0.5, "%.2f m apart at most" % diff)
		var area := 0.0
		for i in range(0, v0.size(), 3):
			area += absf((v0[i + 1] - v0[i]).cross(v0[i + 2] - v0[i]).y) * 0.5
		_check("outline holds the balance's area", absf(area - a) < 0.01 * a, "%.1f m2" % area)
		_check("outline for the ground to carve to", oasis.get_outline(0).size() == 128, str(oasis.get_outline(0).size()))
		# A spring whose ground was never hollowed (a dune 1 m over its level): no pool, no boils.
		var dune := StaticBody3D.new()
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(80, 2, 80)
		cs.shape = box
		dune.add_child(cs)
		dune.position = Vector3(600, 0, 0)
		add_child(dune)
		dry_oasis = OasisSprings3D.new()
		dry_oasis.springs = PackedVector4Array([Vector4(600, 0, 0, 0.05)])
		add_child(dry_oasis)
		return
	if test and n == 40:
		_check("no pool where the ground is not carved", dry_oasis.get_pool_count() == 1 and dry_oasis.get_pool(0) == null, str(dry_oasis.get_pool(0)))
		print("oasis: %d failed" % fails)
		get_tree().quit(1 if fails > 0 else 0)
		return
	if out != "" and n >= (frames + 240 if surface != null else frames):
		if not raster and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0 and n < frames + 600:
			return
		set_physics_process(false)
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

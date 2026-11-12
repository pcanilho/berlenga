extends Node3D
# PlantGenerator: every preset at three seeds in a row, on a ground plane.
#
# godot --path docs/bench/plants -- [options]
#   --test           checks, print PASS/FAIL, quit
#   --raster         raster only (default path traced)
#   --spp=N          path tracing samples (default 16)
#   --close=P        close view of preset P (index in NAMES)
#   --macro          with --close, one plant from a hand span away
#   --notex          without the generated leaf textures
#   --push           with --close: a body stands on the middle plant (bent through the trample field)
#   --still          with --push: the body stands there but the trample field is off (the before shot)
#   --top            with --close: straight down on the middle plant (leaf outlines)
#   --wide           with --close: from 2.5 m, a little above (spreading or lodged plants)
#   --head           with --close: the top of the first plant from 15 cm (flower and seed heads)
#   --wear=W         leaf wear 0..1 (default the generator's 0.5)
#   --wind=x,z       fallback wind for the plants (default none)
#   --dump=dir       save the baked textures of the first seed as PNGs
#   --seq=N          save N frames in a row as path_000.png... (run with --fixed-fps 30 for a clip)
#   --stand=S        give the litter and seedling presets the leaves of floor stand S (make_floor's FloorStand)
#   --season=F,D     flowering stage for degree days F (base 5 C, from 1 January) on day of year D
#   --day=D          the season of day D from a default WeatherMap3D (flowering, fronds)
#   --out=path.png   save the frame

const NAMES := ["dandelion", "plantain", "clover", "fern", "sedge", "daisy", "buttercup", "nettle", "thistle", "yarrow", "dock", "cocksfoot", "timothy", "dry_grass", "leaf_litter", "moss", "bilberry", "bracken", "bramble", "foxglove", "reed", "cattail", "rush", "water_lily", "needle_litter", "twigs", "wood_sorrel", "seedling", "feather_moss", "woodruff", "wood_anemone", "ivy", "heather", "violet", "herb_robert", "knapweed", "scabious", "wild_carrot", "sorrel"]
const SEEDS := 3
const STEP := 0.55
var test := false
var raster := false
var spp := 16
var close := -1
var stand := -1
var out := ""
var macro := false
var notex := false
var dump := ""
var push := false
var still := false
var wear := -1.0
var top := false
var head := false
var wide := false
var seq := 0
var wind := Vector2.ZERO
var season := {}
var n := 0
var fails := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test": test = true
		elif a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--close="): close = int(a.substr(8))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--macro": macro = true
		elif a == "--notex": notex = true
		elif a.begins_with("--dump="): dump = a.substr(7)
		elif a == "--push": push = true
		elif a == "--still": still = true
		elif a == "--top": top = true
		elif a == "--head": head = true
		elif a == "--wide": wide = true
		elif a.begins_with("--seq="): seq = int(a.substr(6))
		elif a.begins_with("--wear="): wear = float(a.substr(7))
		elif a.begins_with("--stand="): stand = int(a.substr(8))
		elif a.begins_with("--day="):
			var wm := WeatherMap3D.new()
			add_child(wm)
			wm.season_day = int(a.substr(6))
			season = wm.get_season()
		elif a.begins_with("--season="):
			var sv := a.substr(9).split(",")
			season = {"forcing": float(sv[0]), "day_of_year": int(sv[1]) if sv.size() > 1 else 180}
		elif a.begins_with("--wind="):
			var w := a.substr(7).split(",")
			wind = Vector2(float(w[0]), float(w[1]))
	if test:
		_checks()
		get_tree().quit(1 if fails > 0 else 0)
		return
	var gen := PlantGenerator.new()
	gen.textures = not notex
	if wear >= 0.0:
		gen.leaf_wear = wear
	if stand >= 0:
		gen.litter_leaves = PlantGenerator.stand_litter_leaves(stand)
	gen.season = season
	for p in NAMES.size():
		if close >= 0 and p != close:
			continue
		gen.preset = p
		gen.apply_preset_colors()
		for s in SEEDS:
			gen.seed = s
			var mi := MeshInstance3D.new()
			mi.mesh = gen.generate()
			if dump != "" and s == 0:
				for si in mi.mesh.get_surface_count():
					var mat := mi.mesh.surface_get_material(si) as ShaderMaterial
					if mat and mat.get_shader_parameter("albedo_tex"):
						(mat.get_shader_parameter("albedo_tex") as Texture2D).get_image().save_png("%s/%s_%d_albedo.png" % [dump, NAMES[p], si])
						(mat.get_shader_parameter("normal_tex") as Texture2D).get_image().save_png("%s/%s_%d_normal.png" % [dump, NAMES[p], si])
						(mat.get_shader_parameter("backlight_tex") as Texture2D).get_image().save_png("%s/%s_%d_back.png" % [dump, NAMES[p], si])
			if macro and s != 0:
				mi.free()
				continue
			mi.position = Vector3((p % 9) * STEP, 0, (s + (p / 9) * 3.4) * STEP) if close < 0 else Vector3((s - 1) * 0.36, 0, 0)
			add_child(mi)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(14, 12)
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.3, 0.25, 0.18)
	gm.roughness = 0.95
	pm.material = gm
	ground.mesh = pm
	ground.position = Vector3(2.2, 0, 1.9)
	add_child(ground)
	var env := Environment.new()
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 3
	env.pathtracing_sky_shader_enabled = true
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40, 150, 0)
	sun.shadow_enabled = true
	if close >= 0:
		# Close views: the shadow map over the few metres in view, so plant shadows are sharp and not
		# soft blobs from the default 100 m cascades.
		sun.directional_shadow_max_distance = 12.0
		sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
	sun.light_energy = 2.0
	add_child(sun)
	if push or wind != Vector2.ZERO:
		# A sparse grass field: it runs the trample field and publishes the wind the plants read.
		var g := GrassField3D.new()
		g.density = 0.1
		g.radius = 4.0
		g.fallback_wind = wind
		g.trample_enabled = push and not still
		g.trample_size = 8.0
		add_child(g)
		if push:
			# A boot-sized capsule standing on the middle plant.
			var body := MeshInstance3D.new()
			var cap := CapsuleMesh.new()
			cap.radius = 0.06
			cap.height = 0.3
			body.mesh = cap
			body.set_meta("trample_radius", 0.22)
			body.add_to_group("grass_trample")
			add_child(body)
			body.position = Vector3(0, 0.15, 0)
	var cam := Camera3D.new()
	cam.fov = 40
	add_child(cam)
	if close >= 0 and macro:
		var bb: AABB = (get_child(0) as MeshInstance3D).get_aabb()
		var c: Vector3 = (get_child(0) as Node3D).position + bb.get_center()
		cam.fov = 30
		if bb.size.y < 0.4:
			c.y = min(c.y, 0.06)
			cam.position = c + Vector3(0, max(0.12, bb.size.y * 0.5), max(0.22, max(bb.size.x, bb.size.z) * 0.9))
		else:
			# Tall plants: the whole plant, from a little above.
			var span: float = max(bb.size.y, max(bb.size.x, bb.size.z))
			cam.position = c + Vector3(0, span * 0.35, span * 1.9)
		cam.look_at(c)
	elif close >= 0 and wide:
		cam.position = Vector3(0, 1.3, 2.4)
		cam.look_at(Vector3(0, 0.2, 0))
	elif close >= 0 and head:
		# The highest vertex of the first plant and what is near it: a flower or seed head.
		var mi0 := get_child(0) as MeshInstance3D
		var best := Vector3(0, -1, 0)
		for si in mi0.mesh.get_surface_count():
			for v in mi0.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]:
				if v.y > best.y:
					best = v
		best += mi0.position - Vector3(0, 0.015, 0)
		cam.fov = 30
		cam.position = best + Vector3(0, 0.03, 0.15)
		cam.look_at(best)
	elif close >= 0 and top:
		cam.position = Vector3(0, 0.75, 0.001)
		cam.look_at(Vector3(0, 0, 0))
	elif close >= 0 and push:
		# From above and further back: the pushed plant and its neighbours.
		cam.position = Vector3(0, 1.5, 1.7)
		cam.look_at(Vector3(0, 0.15, 0))
	elif close >= 0:
		# Tall plants (meadow flowers, nettles): back off to the tallest of the three.
		var h := 0.0
		for ch in get_children():
			if ch is MeshInstance3D and ch.mesh:
				h = maxf(h, (ch as MeshInstance3D).get_aabb().end.y)
		if h > 0.45:
			cam.position = Vector3(0, h * 0.75, h * 1.55)
			cam.look_at(Vector3(0, h * 0.42, 0))
		else:
			cam.position = Vector3(0, 0.42, 0.85)
			cam.look_at(Vector3(0, 0.1, 0))
	else:
		cam.position = Vector3(2.2, 2.3, 5.9)
		cam.look_at(Vector3(2.2, 0.1, 1.8))

func _check(label: String, ok: bool, value) -> void:
	print("%s %s: %s" % ["PASS" if ok else "FAIL", label, str(value)])
	if not ok:
		fails += 1

func _tris(m: Mesh) -> int:
	var t := 0
	for s in m.get_surface_count():
		t += m.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	return t

func _checks() -> void:
	# Climber leaves for the trees' vines: single leaves in XZ, base at the origin, +Z, 1 m long.
	for c in [PlantGenerator.CLIMBER_IVY, PlantGenerator.CLIMBER_CLEMATIS, PlantGenerator.CLIMBER_HONEYSUCKLE]:
		var leaves := PlantGenerator.make_climber_leaves(c, 4)
		var ok := leaves.size() == 4
		var box := AABB()
		for lm in leaves:
			var a: AABB = lm.get_aabb()
			box = a if box.size == Vector3() else box.merge(a)
			ok = ok and lm.surface_get_material(0) is ShaderMaterial
		ok = ok and box.position.z > -0.02 and box.end.z > 0.85 and box.end.z < 1.1 and absf(box.position.y) < 0.3 and box.end.y < 0.3
		_check("climber leaves %d: four flat leaves along +Z, 1 m" % c, ok, box)
	# Seedlings of a stand's trees: every leaf kind builds a small plant (oak lobed, sycamore palmate, ash pinnate).
	for st in [PlantGenerator.FLOOR_BEECH, PlantGenerator.FLOOR_OAK_MIXED, PlantGenerator.FLOOR_NORTHERN_HARDWOOD]:
		var sg := PlantGenerator.new()
		sg.preset = PlantGenerator.PRESET_SEEDLING
		sg.litter_leaves = PlantGenerator.stand_litter_leaves(st)
		var ok := true
		var tris := 0
		for sd in 6:
			sg.seed = sd
			var sm := sg.generate()
			tris = maxi(tris, _tris(sm))
			ok = ok and sm.get_surface_count() > 0 and sm.get_aabb().size.y > 0.03 and sm.get_aabb().size.y < 1.0
		_check("stand %d seedlings" % st, ok and tris < 6000, tris)
	var gen := PlantGenerator.new()
	if wear >= 0.0:
		gen.leaf_wear = wear
	for p in NAMES.size():
		gen.preset = p
		gen.seed = 1
		var t0 := Time.get_ticks_usec()
		var m := gen.generate()
		var ms := (Time.get_ticks_usec() - t0) / 1000.0
		var aabb := m.get_aabb()
		var again := gen.generate()
		gen.seed = 2
		var other := gen.generate()
		print("PLANT %s tris=%d height=%.2f width=%.2f ms=%.2f surfaces=%d" % [NAMES[p], _tris(m), aabb.end.y, max(aabb.size.x, aabb.size.z), ms, m.get_surface_count()])
		_check(NAMES[p] + " has geometry", _tris(m) > 50, _tris(m))
		_check(NAMES[p] + " plausible size", aabb.end.y > 0.005 and aabb.end.y < 3.0 and aabb.size.x < 3.0, aabb)
		_check(NAMES[p] + " grows from the ground", aabb.position.y > -0.08, aabb.position.y)
		_check(NAMES[p] + " same seed, same plant", _tris(again) == _tris(m) and again.get_aabb() == aabb, _tris(again))
		_check(NAMES[p] + " other seed, other plant", other.get_aabb() != aabb, other.get_aabb())
	# Forest floor sets per stand type: species with meshes, placed by shade or by water.
	for st in PlantGenerator.FLOOR_MAX:
		var set := PlantGenerator.make_floor(st, 1, false)
		var ok := set.size() >= 6
		for sp in set:
			ok = ok and (sp as PlantSpecies).meshes.size() == 1 and ((sp as PlantSpecies).get_rule_range(PlantSpecies.RULE_SHADE) != Vector2(0, 1) or (sp as PlantSpecies).get_rule_range(PlantSpecies.RULE_LITTER) != Vector2(0, 1) or (sp as PlantSpecies).get_rule_range(PlantSpecies.RULE_WATER_DISTANCE).y < 100.0 or (sp as PlantSpecies).deadwood_rule != PlantSpecies.DEADWOOD_AVOID)
		_check("floor set %d" % st, ok, set.size())
	_bloom_checks()
	print("PLANTS_RESULT ", "PASS" if fails == 0 else "FAIL (%d)" % fails)

# Flowering by degree days (docs/ideas/vegetation-seasons.md, step 2).
func _bloom_checks() -> void:
	var P := PlantGenerator
	var s: Vector3 = P.get_bloom_shares(P.PRESET_KNAPWEED, {"forcing": 300.0, "day_of_year": 160})
	_check("knapweed before its window: leaves only", s.x > 0.99, s)
	s = P.get_bloom_shares(P.PRESET_KNAPWEED, {"forcing": 1300.0, "day_of_year": 220})
	_check("knapweed in August: mostly in flower, some in seed", s.y > 0.5 and s.z > 0.2, s)
	s = P.get_bloom_shares(P.PRESET_KNAPWEED, {"forcing": 1900.0, "day_of_year": 300})
	_check("knapweed in autumn: seed heads standing", s.z > 0.99, s)
	s = P.get_bloom_shares(P.PRESET_KNAPWEED, {"forcing": 0.0, "day_of_year": 30})
	_check("knapweed in January: some of last year's stalks", s.z > 0.2 and s.z < 0.6, s)
	s = P.get_bloom_shares(P.PRESET_WOOD_ANEMONE, {"forcing": 200.0, "day_of_year": 110})
	_check("anemone in April: in flower", s.y > 0.6, s)
	s = P.get_bloom_shares(P.PRESET_WOOD_ANEMONE, {"forcing": 1000.0, "day_of_year": 200})
	_check("anemone in July: done", s.x > 0.99, s)
	s = P.get_bloom_shares(P.PRESET_DAISY, {"forcing": 900.0, "day_of_year": 190})
	_check("daisy in summer: flowering on, a quarter in seed", s.y > 0.6 and s.z > 0.15 and s.z < 0.3, s)
	s = P.get_bloom_shares(P.PRESET_NETTLE, {"forcing": 100.0})
	_check("no window: always as built", s == Vector3(0, 1, 0), s)
	var presets := [P.PRESET_DANDELION, P.PRESET_CLOVER, P.PRESET_DAISY, P.PRESET_BUTTERCUP, P.PRESET_THISTLE, P.PRESET_YARROW, P.PRESET_FOXGLOVE, P.PRESET_WOOD_SORREL, P.PRESET_WOODRUFF, P.PRESET_WOOD_ANEMONE, P.PRESET_VIOLET, P.PRESET_HERB_ROBERT, P.PRESET_HEATHER, P.PRESET_KNAPWEED, P.PRESET_SCABIOUS, P.PRESET_WILD_CARROT, P.PRESET_SORREL]
	var days := [[0.0, 20], [200.0, 110], [500.0, 150], [1000.0, 200], [1500.0, 240], [1900.0, 300]]
	for p in presets:
		var g := PlantGenerator.new()
		g.textures = false
		g.lods = false
		g.preset = p
		g.seed = 3
		var plain := _tris(g.generate())
		g.season = {}
		var ok := _tris(g.generate()) == plain
		var line := ""
		var heights := []
		for d in days:
			g.season = {"forcing": d[0], "day_of_year": d[1]}
			var tot := 0
			var hmax := 0.0
			for sd in 4:
				g.seed = 3 + sd
				var m := g.generate()
				tot += _tris(m)
				hmax = maxf(hmax, m.get_aabb().end.y)
				ok = ok and _tris(m) > 20
			g.seed = 3
			heights.append(hmax)
			line += " %d:%d/%.2f" % [int(d[0]), tot, hmax]
		_check("%s through the year (forcing:tris/height)%s" % [NAMES[p], line], ok, plain)
	# Tall flowers: rosettes before the stems bolt, tall in flower.
	var k := PlantGenerator.new()
	k.textures = false
	k.preset = P.PRESET_KNAPWEED
	k.seed = 5
	k.season = {"forcing": 300.0, "day_of_year": 160}
	var low := k.generate().get_aabb().end.y
	k.season = {"forcing": 1000.0, "day_of_year": 200}
	var high := k.generate().get_aabb().end.y
	_check("knapweed: rosette in May, tall in July", low < 0.15 and high > 0.3, Vector2(low, high))
	# Ferns and spring plants through the year (step 3), on a default WeatherMap3D's days.
	var wm := WeatherMap3D.new()
	add_child(wm)
	var seasons := {}
	for d in [45, 125, 200, 345]:
		wm.season_day = d
		seasons[d] = wm.get_season()
	wm.queue_free()
	for p in [P.PRESET_BRACKEN, P.PRESET_FERN]:
		var g := PlantGenerator.new()
		g.textures = false
		g.preset = p
		g.seed = 2
		var hs := {}
		for d in seasons:
			g.season = seasons[d]
			hs[d] = g.generate().get_aabb().end.y
		var tall := 0.4 if p == P.PRESET_BRACKEN else 0.15
		_check("%s: flat in winter, croziers in May, full in July, no taller in December" % NAMES[p], hs[45] < 0.25 and hs[125] < hs[200] and hs[200] > tall and hs[345] <= hs[200], hs)
	var pres := [P.get_presence(P.PRESET_WOOD_ANEMONE, seasons[45]), P.get_presence(P.PRESET_WOOD_ANEMONE, {"forcing": 200.0}), P.get_presence(P.PRESET_WOOD_ANEMONE, seasons[200]), P.get_presence(P.PRESET_FERN, seasons[200])]
	_check("anemone above ground in spring only", pres[0] == 0.0 and pres[1] == 1.0 and pres[2] == 0.0 and pres[3] == 1.0, pres)

func _process(_d: float) -> void:
	n += 1
	# Raster waits for the wind and the trample field to settle; a traced frame costs minutes here.
	var settle := 90 if (push or wind != Vector2.ZERO) and raster else 8
	if n < settle:
		return
	if seq > 0 and n < settle + seq:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out.get_basename() + "_%03d.png" % (n - settle))
		return
	set_process(false)
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	get_tree().quit()

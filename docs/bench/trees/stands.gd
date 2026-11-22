extends Node3D
# Stand bench: every TreeGenerator stand (and make_woodland()) placed by VegetationField3D on a
# small valley with a stream, checked, and drawn as a placement map (crowns from above, one colour
# per species), or rendered.
#
# godot --path docs/bench/trees --rendering-driver vulkan -- --stands [options]
#   --strikes --stream  a strike after a streamed field is in grows the deferred struck variants (trees.md 5bc)
#   --lightning --strikes  struck trees in a placed beech stand: a WorldStand strike swaps that tree only (trees.md 5ax)
#   --beaver-meadow   a drained beaver pond (WorldStand kind 4): meadow snags, fallen snags and willow come in (trees.md 5bb)
#   --avalanche       an avalanche track 5 years old (WorldStand kind 6): thrown logs down the fall line, snapped stumps, stripped survivors (trees.md 5bh)
#   --colonnade       taiga stilted spruce placed in rows along vanished logs (trees.md 5az)
#   --browse          browsed trees in a placed beech stand: a WorldStand browse disc swaps the trees under it (trees.md 5az)
#   --lightning       a fresh strike's debris round a spruce, none unstruck or three years on (trees.md 5ax)
#   --test            place every stand, print the counts and PASS/FAIL, quit
#   --stand-test=N    the placement checks of stand N only (TreeGenerator.Stand)
#   --forest-edge=N   make_edge(N): mantle shrubs out in the open, edge trees one-sided toward +X (trees.md 5as)
#   --beaver          beaver work on single trees: stump cone, felled stem lying, gnawed waist, resprouts (trees.md 5au)
#   --flood           flood marks on a willow: silt under the waterline only, wrack at it (trees.md 5av)
#   --deer            deer browse and nurse logs: hedged saplings, the browse line, seedlings on logs, stilts (trees.md 5aw)
#   --succession=N    make_succession(N): every stage present, age ranges and kinds sane, sizes by stage (trees.md 5aq)
#   --map=path.png    the placement maps of every stand in one image
#   --edge [--standard]  with --plot: from a fresh coupe across its edge (or at a standard 8 m off)
#   --plot=N          render stand N (TreeGenerator.Stand) on the valley (--out=path.png, --raster, --spp=N)
#   --wind=U          with --plot: rigged trees posed by a U m/s wind (TreeWind3D) at --time=T s
#   --far=R           with --plot: every species ranged R m and streamed (the far tail thins and widens past R/2)
#   EDGE=1            env, with --plot: the stand with make_edge()'s mantle and edge trees
#   AGE=y [KIND=k]    env, with --plot: the make_succession() stage at y years after disturbance kind k
#   --inside          with --plot: the view from inside the stand, 70 m deep
#   --day=N           with --plot: leaves by day of year (main.gd's stand-in climate)
#   --deep            with --inside: from among the canopy trees, away from the stream
#   --variants=N      meshes per species (default 1)
#   --fire            build the fire band variants too (five meshes a species) and check their meta
#   --region=path.png make_woodland() over a 6 km massif with its climate (placement only, no
#                     meshes): the stands as belts on a hillshade
#   --generated       with --region: a TerrainGenerator3D range (4 km, geology and soil) instead
#   --na              with --region: the North American woodland instead of the European one

const SIZE := 160.0
const STREAM_HALF := 3.0 # Stream bed half width, metres.

class Valley extends Node3D:
	func get_height(p: Vector3) -> float:
		# A stream along z at x = 0 in a shallow valley, with low swells.
		var ax: float = abs(p.x)
		var bank: float = -0.8 + 0.1 * max(ax - STREAM_HALF, 0.0) if ax > STREAM_HALF else -0.8
		var h: float = bank + 0.8 * min(ax / 20.0, 1.0) * sin(p.z * 0.05) * cos(p.x * 0.04)
		if OS.get_environment("RIDGE") != "":
			# RIDGE=1: the ground rises to a crest 25 m up at the plot's -z edge (skyline trees, trees.md 9c).
			h += 25.0 * pow(clampf((-p.z + 10.0) / 90.0, 0.0, 1.0), 1.5)
		return h

class Stream extends Node3D:
	func get_water_level() -> float:
		return -0.3

const COLORS := [Color("6b8e23"), Color("2e8b57"), Color("d8d8c0"), Color("8b5a2b"), Color("1f4d2b"), Color("c9a227"),
		Color("b03a2e"), Color("7fbf7f"), Color("e07b39"), Color("9aa0a6"), Color("8e44ad"), Color("3b6e8f"), Color("a3c4a8"),
		Color("d4b483"), Color("335c4c"), Color("e74c3c"), Color("f5e6a8"), Color("0b3d1e"), Color("4a2c1a"), Color("5c7a2e"),
		Color("d35400"), Color("c0392b"), Color("a04000"), Color("b7950b"), Color("4d6b5a"), Color("17332a"), Color("a6acaf"),
		Color("f4f1ea"), Color("caa472"), Color("e5c07b"), Color("6c8f3a")]
const STAND_NAMES := ["beech", "oak mixed", "montane conifer", "subalpine", "pine heath", "riparian", "hedgerow",
		"northern hardwood", "eastern oak", "pine-hemlock", "coppice standards", "wood pasture", "pollard row", "stone pine",
		"olive grove", "date oasis", "beetle spruce", "windthrow", "old-growth beech", "boreal taiga", "alder carr", "floodplain willow", "beaver pond", "deer wood", "ice storm", "woodland", "NA woodland"]

var gen: TreeGenerator
var dm: TerrainDataMap3D
var fails := 0
var variants := 1
var use_generator := false
var north_america := false

func run(args: Dictionary) -> void:
	variants = int(args.get("variants", "1"))
	gen = TreeGenerator.new()
	# GSET="prop:value,prop:value" sets generator properties (a debugging switch).
	for kv in OS.get_environment("GSET").split(",", false):
		var p2 := kv.split(":")
		gen.set(p2[0], float(p2[1]))
	gen.seed = int(args.get("seed", "1"))
	# Full-needle near meshes (about 30M vertices a conifer species) serve no placement check and
	# overrun a 15 GB machine: off here unless asked for with --near-needles.
	if not args.has("near-needles"):
		gen.near_needle_range = 0.0
	if args.has("no-near-leaves"):
		gen.near_leaf_range = 0.0
	# Fire band variants (five more meshes a species, trees.md 5ah) serve no placement check: off unless --fire.
	gen.fire_variants = args.has("fire")
	gen.lightning_variants = args.has("strikes")
	gen.browse_variants = args.has("browse")
	gen.far_thinning = OS.get_environment("NO_FAR_SCALE") == ""
	if args.has("forest-edge"):
		_forest_edge_check(int(args["forest-edge"]))
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("flood"):
		_flood_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("deer"):
		_deer_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("lightning") and not args.has("strikes"):
		_lightning_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("beaver"):
		_beaver_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("succession"):
		_succession_check(int(args["succession"]))
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("region"):
		use_generator = args.has("generated")
		north_america = args.has("na")
		await _region(args["region"])
		get_tree().quit(1 if fails > 0 else 0)
		return
	_build_ground()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var err := dm.bake()
	_check(err == OK, "valley data map bakes")
	if args.has("beaver-meadow"):
		await _beaver_meadow_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("avalanche"):
		await _avalanche_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("colonnade"):
		await _colonnade_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("browse"):
		await _browse_field_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("strikes") and args.has("stream"):
		await _strike_stream_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("strikes") and args.has("lightning"):
		await _strike_field_check()
		get_tree().quit(1 if fails > 0 else 0)
		return
	if args.has("test"):
		_habitat_checks()
		_stand_checks()
		_mask_check()
		await _wind_check()
		await _wind_lod_check(int(args.get("frames", "40")))
		# The near-leaf check grows its own oaks (8.7 GB peak alone) and runs as --near-leaves: inside --test
		# the stands' meshes still held by the scene push the run past 15 GB.
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("stand-test"):
		_stand_checks(int(args["stand-test"]))
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("near-leaves"):
		await _near_leaves_check()
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("wind-lod"):
		await _wind_lod_check(int(args.get("frames", "120")), args.get("view", "eye"))
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("map"):
		_draw_maps(args["map"])
		get_tree().quit(1 if fails > 0 else 0)
	elif args.has("plot"):
		await _render_plot(int(args["plot"]), args)

# Forest edges (trees.md 5as): edge trees carry tree_open_side on species and meshes and lean their crowns to the
# open (+X: the crown's centre off the stem that way), the mantle shrubs sit mostly out in the open.
func _forest_edge_check(i: int) -> void:
	var biome := gen.make_edge(i, variants)
	var trees := 0
	var one_sided := 0
	var mantle := 0
	var mantle_out := 0
	var bad := 0
	var meshes := 0
	for sp in biome.species:
		var r: Vector2 = sp.get_meta("edge_distance", Vector2(1e9, -1e9))
		if r.y <= r.x:
			bad += 1
		var nm: String = sp.resource_name
		if nm.contains("(forest edge)"):
			trees += 1
			var ok: bool = sp.has_meta("tree_open_side")
			var shift := 0.0
			for m in sp.meshes:
				ok = ok and m.has_meta("tree_open_side")
				meshes += 1
				var ab: AABB = m.get_aabb()
				# The leaf mass's centre where the mesh has broad leaves (one limb reaching back moves a box's centre
				# more than the crown); the box's centre for needles.
				var li := _surface(m, "leaves")
				if li >= 0:
					var sx := 0.0
					var verts: PackedVector3Array = m.surface_get_arrays(li)[Mesh.ARRAY_VERTEX]
					for v in verts:
						sx += v.x
					shift += sx / maxf(verts.size(), 1) / maxf(ab.size.x, 0.1)
				else:
					shift += (ab.position.x + 0.5 * ab.size.x) / maxf(ab.size.x, 0.1)
			shift /= maxf(sp.meshes.size(), 1)
			if ok and shift > 0.05:
				one_sided += 1
			print("  %s: %.0f..%.0f m, crown (leaf mass) centre %.2f of its width toward the open" % [nm, r.x, r.y, shift])
		elif nm.contains("(edge mantle)"):
			mantle += 1
			if r.x + r.y < 0.0:
				mantle_out += 1
			print("  %s: %.0f..%.0f m" % [nm, r.x, r.y])
	var name_i: String = STAND_NAMES[i] if i < STAND_NAMES.size() else str(i)
	# Groves, oases and parkland have no leading forest tree: a mantle only.
	var no_lead := i in [TreeGenerator.STAND_STONE_PINE, TreeGenerator.STAND_OLIVE_GROVE, TreeGenerator.STAND_DATE_OASIS]
	# A deer wood's mantle is eaten out to two hedged thorns (trees.md 5bd).
	var deer := i == TreeGenerator.STAND_DEER_WOOD
	var mantle_min := 2 if deer else 3
	# Edge trees all face the open: at least three meshes for the lead and two for the second, whatever --variants says.
	var enough := no_lead or meshes >= 5 or (trees == 1 and meshes >= 3)
	_check(bad == 0 and (trees > 0 or no_lead) and one_sided == trees and mantle >= mantle_min and mantle_out * 2 >= mantle and enough, "forest edge of %s: %d edge trees (%d one-sided to the open, %d meshes), %d mantle shrubs (%d mostly outside), %d bad ranges" % [name_i, trees, one_sided, meshes, mantle, mantle_out, bad])

# Beaver work (trees.md 5au) on willow and birch: a stump stands under a metre with its gnawed cone (wood surface
# above the cut), a felled stem lies (longer than tall) with its butt cone, a gnawed stem's waist is pulled in
# against the same tree ungnawed, and some stumps resprout (leaves on a stump).
func _surface(m: Mesh, name: String) -> int:
	for si in m.get_surface_count():
		if m.surface_get_name(si) == name:
			return si
	return -1

func _band_min_r(m: Mesh, y0: float, y1: float) -> float:
	# The narrowest 2 cm slice of the stem in the band, by its median vertex radius.
	var si := _surface(m, "bark")
	if si < 0:
		return -1.0
	var v: PackedVector3Array = m.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]
	var slices := {}
	for p in v:
		if p.y > y0 and p.y < y1:
			var k := int(floor(p.y / 0.02))
			if not slices.has(k):
				slices[k] = []
			slices[k].append(Vector2(p.x, p.z).length())
	var lo := 1e9
	for k in slices:
		var rs: Array = slices[k]
		if rs.size() < 8:
			continue
		rs.sort()
		lo = minf(lo, rs[int(rs.size() * 0.5)])
	return lo if lo < 1e8 else -1.0

# Flood marks (trees.md 5av): silt coded on the bark's UV2 (u below -0.5) under the waterline and nowhere above it,
# and wrack (more wood-surface geometry) at the line; nothing without a flood.
func _flood_check() -> void:
	var g := TreeGenerator.new()
	g.preset = TreeGenerator.PRESET_WHITE_WILLOW
	g.near_leaf_range = 0.0
	g.set_param(TreeGenerator.PARAM_YEARS, 35.0)
	var found_wrack := 0
	for sd in range(1, 5):
		g.seed = sd
		g.flood_height = 0.0
		var dry := g.generate()
		g.flood_height = 1.1
		g.flood_years = 0.2
		var wet := g.generate()
		var bi := _surface(wet, "bark")
		var arr: Array = wet.surface_get_arrays(bi)
		var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var u2: PackedVector2Array = arr[Mesh.ARRAY_TEX_UV2]
		var under := 0
		var under_n := 0
		var over := 0
		for k in v.size():
			var silty := u2.size() == v.size() and u2[k].x < -0.5
			if v[k].y < 0.9:
				under_n += 1
				if silty:
					under += 1
			elif v[k].y > 1.3 and silty:
				over += 1
		var wd := _surface(dry, "wood")
		var ww := _surface(wet, "wood")
		var nd: int = dry.surface_get_array_len(wd) if wd >= 0 else 0
		var nw: int = wet.surface_get_array_len(ww) if ww >= 0 else 0
		if nw > nd:
			found_wrack += 1
		print("  seed %d: wood vertices %d dry, %d flooded" % [sd, nd, nw])
		_check(under > 0.8 * under_n and over == 0, "willow seed %d: silt on %d of %d bark vertices under 0.9 m, %d over 1.3 m" % [sd, under, under_n, over])
	_check(found_wrack >= 2, "flood wrack on %d of 4 willows" % found_wrack)
	g.flood_height = 0.0

func _beaver_meadow_check() -> void:
	# The beaver meadow (trees.md 5bb): the pond stand carries species for a drained bed (WorldStand
	# DISTURBANCE_BEAVER, 4), none placed without one; a drained disc 12 years on puts snags, fallen snags and incoming
	# willow inside it and none outside, and the pond's own drowned snags are kept off it.
	var ws = WorldContracts.get_stand()
	if ws == null:
		_check(false, "beaver meadow: no WorldStand")
		return
	ws.clear_disturbances()
	var biome := gen.make_stand(TreeGenerator.STAND_BEAVER_POND, variants)
	var sps: Array = biome.species
	var meadow: Array[int] = []
	var pond_off := 0
	for s in sps.size():
		var r: Vector2 = sps[s].get_rule_range(PlantSpecies.RULE_DISTURBANCE)
		if r == Vector2(4, 4):
			meadow.append(s)
		elif String(sps[s].resource_name).contains("drowned, beaver pond") and r.y < 4.0:
			pond_off += 1
	_check(meadow.size() == 6 and pond_off == 2, "beaver pond stand: %d meadow species, %d pond snag species kept off a drained bed" % [meadow.size(), pond_off])
	var f := _field(biome)
	for k in 3:
		await get_tree().process_frame
	var count := func(center: Vector3, r0: float, r1: float) -> int:
		var n := 0
		for s in meadow:
			for p in f.get_placements(s):
				var d := Vector2(p.x - center.x, p.z - center.z).length()
				if d >= r0 and d <= r1:
					n += 1
		return n
	var mid := Vector3(SIZE * 0.5, 0, SIZE * 0.5)
	var before: int = count.call(mid, 0.0, SIZE)
	_check(before == 0, "no drained pond: %d meadow trees" % before)
	var r := SIZE * 0.25
	ws.add_disturbance(mid, r, WorldStand.DISTURBANCE_BEAVER, 12.0)
	f.populate()
	for k in 3:
		await get_tree().process_frame
	var inside: int = count.call(mid, 0.0, r * 0.9)
	var outside: int = count.call(mid, r * 1.1, SIZE)
	var kinds := 0
	for s in meadow:
		if f.get_placements(s).size() > 0:
			kinds += 1
	_check(inside > 10 and outside == 0 and kinds >= 4, "drained 12 years (r %.0f m): %d meadow trees inside, %d outside, %d of %d meadow species placed" % [r, inside, outside, kinds, meadow.size()])
	ws.clear_disturbances()


func _avalanche_check() -> void:
	# Avalanche legacies (trees.md 5bh): the beech stand's succession carries thrown logs, snapped stumps and stripped
	# survivors for WorldStand DISTURBANCE_AVALANCHE (6), none placed without one; a track 5 years old puts them inside
	# it and none outside, and the thrown logs lie down the fall line (mesh +X downhill).
	var ws = WorldContracts.get_stand()
	if ws == null:
		_check(false, "avalanche: no WorldStand")
		return
	ws.clear_disturbances()
	var all := gen.make_succession(TreeGenerator.STAND_BEECH, variants)
	var keep: Array[PlantSpecies] = []
	for sp in all.species:
		if int(sp.get_meta("succession_kind", -1)) == TreeGenerator.SUCCESSION_AVALANCHE:
			keep.append(sp)
	all.species = keep
	_check(keep.size() == 5, "beech succession: %d avalanche legacy species" % keep.size())
	var f := _field(all)
	for k in 3:
		await get_tree().process_frame
	var mid := Vector3(SIZE * 0.5, 0, SIZE * 0.5)
	var placed := func() -> int:
		var n := 0
		for s in keep.size():
			n += f.get_placements(s).size()
		return n
	_check(placed.call() == 0, "no avalanche: %d legacy plants" % placed.call())
	var r := SIZE * 0.3
	ws.add_disturbance(mid, r, WorldStand.DISTURBANCE_AVALANCHE, 5.0)
	f.populate()
	for k in 3:
		await get_tree().process_frame
	var inside := 0
	var outside := 0
	var by := {}
	for s in keep.size():
		for p in f.get_placements(s):
			var d := Vector2(p.x - mid.x, p.z - mid.z).length()
			if d <= r * 0.9:
				inside += 1
				by[keep[s].resource_name] = by.get(keep[s].resource_name, 0) + 1
			elif d >= r * 1.1:
				outside += 1
	_check(inside > 10 and outside == 0 and by.size() >= 3, "avalanche 5 years ago (r %.0f m): %d legacy plants inside, %d outside: %s" % [r, inside, outside, str(by)])
	# Thrown logs down the fall line: mesh +X against the ground's downhill direction, where the ground slopes.
	var down := 0
	var checked := 0
	for s in keep.size():
		if not String(keep[s].resource_name).contains("avalanche thrown"):
			continue
		for mmi in f.find_children("*", "MultiMeshInstance3D", true, false):
			var mm: MultiMesh = mmi.multimesh
			if mm == null or not keep[s].meshes.has(mm.mesh):
				continue
			for k in mm.instance_count:
				var t: Transform3D = mmi.global_transform * mm.get_instance_transform(k)
				var o := t.origin
				var e := 2.0
				var g := Vector2(dm.get_height_at(o + Vector3(e, 0, 0)) - dm.get_height_at(o - Vector3(e, 0, 0)), dm.get_height_at(o + Vector3(0, 0, e)) - dm.get_height_at(o - Vector3(0, 0, e)))
				if g.length() < 0.2:
					continue
				checked += 1
				var x := Vector2(t.basis.x.x, t.basis.x.z).normalized()
				if x.dot(-g.normalized()) > cos(0.6):
					down += 1
	_check(checked > 0 and down > checked * 0.85, "avalanche: %d of %d thrown logs on slopes lie down the fall line" % [down, checked])
	ws.clear_disturbances()


func _colonnade_check() -> void:
	# Colonnades (trees.md 5az): the taiga's stilted spruce stand in rows along vanished logs: most have a row mate
	# 1-2.5 m away, and most trees' two nearest row mates lie on one line through it (not a scatter; a neighbouring
	# log's row can come nearer).
	var biome := gen.make_stand(TreeGenerator.STAND_BOREAL_TAIGA, variants)
	var sps: Array = biome.species
	var idx := -1
	for s in sps.size():
		if sps[s].has_meta("colonnade"):
			idx = s
	_check(idx >= 0, "taiga: a species placed as colonnades (%s)" % (sps[idx].resource_name if idx >= 0 else "none"))
	if idx < 0:
		return
	var f := _field(biome)
	for k in 3:
		await get_tree().process_frame
	var pl: PackedVector3Array = f.get_placements(idx)
	var mates := 0
	var lined := 0
	var triples := 0
	for i in pl.size():
		var a := Vector2(pl[i].x, pl[i].z)
		var near: Array = []
		for j in pl.size():
			if j == i:
				continue
			var d := a.distance_to(Vector2(pl[j].x, pl[j].z))
			if d < 3.3:
				near.append([d, Vector2(pl[j].x, pl[j].z)])
		near.sort_custom(func(x, y): return x[0] < y[0])
		if near.size() > 0 and near[0][0] >= 0.9 and near[0][0] <= 2.5:
			mates += 1
		if near.size() >= 2:
			triples += 1
			var u: Vector2 = (near[0][1] - a).normalized()
			var v: Vector2 = (near[1][1] - a).normalized()
			if absf(u.dot(v)) > 0.95:
				lined += 1
	_check(pl.size() >= 10 and mates >= pl.size() * 0.7 and triples > 0 and lined >= triples * 0.7, "colonnades: %d stilted spruce, %d with a row mate 0.9-2.5 m off, %d of %d with two mates in line" % [pl.size(), mates, lined, triples])


func _browse_field_check() -> void:
	# Browsed variants in the world (trees.md 5az): the beech stand's open, sapling and understory species carry one
	# browsed variant each, not counted living; with no deer none is placed; a WorldStand browse disc at full pressure
	# makes the trees under it browsed and leaves those well outside it alone.
	var ws = WorldContracts.get_stand()
	if ws == null:
		_check(false, "browsed trees: no WorldStand")
		return
	ws.clear_browse()
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	var sps: Array = biome.species
	var with := 0
	var bad := 0
	for s in sps.size():
		var n := 0
		for m in sps[s].meshes:
			if m != null and m.has_meta("tree_browse"):
				n += 1
		if int(sps[s].get_meta("tree_variants_deferred", 0)) & TreeGenerator.VARIANT_BROWSE:
			with += 1
		if n > 0:
			bad += 1
	_check(with > 0 and bad == 0, "beech stand: %d species defer a browsed variant, %d grown before any deer" % [with, bad])
	if with == 0:
		return
	var f := _field(biome)
	for k in 3:
		await get_tree().process_frame
	var count := func(center: Vector3, r0: float, r1: float) -> Array:
		# Instances of species that carry a browsed variant, in the ring r0..r1 round center; browsed by their mesh.
		var n_b := 0
		var n_all := 0
		for c in f.find_children("*", "MultiMeshInstance3D", true, false):
			if not (c is MultiMeshInstance3D) or c.multimesh == null or c.multimesh.mesh == null:
				continue
			var mm: MultiMesh = c.multimesh
			var is_b: bool = mm.mesh.has_meta("tree_browse")
			for k in mm.instance_count:
				var o: Vector3 = (c.global_transform * mm.get_instance_transform(k)).origin
				var d := Vector2(o.x - center.x, o.z - center.z).length()
				if d >= r0 and d <= r1:
					n_all += 1
					if is_b:
						n_b += 1
		return [n_b, n_all]
	var mid := Vector3(SIZE * 0.5, 0, SIZE * 0.5)
	var before: Array = count.call(mid, 0.0, SIZE)
	_check(before[1] > 0 and before[0] == 0, "no deer: %d instances, %d browsed" % [before[1], before[0]])
	var r := SIZE * 0.2
	var id: int = ws.add_browse(mid, r, 1.0, 1.8)
	f.populate()
	for k in 3:
		await get_tree().process_frame
	var grown := 0
	for s in sps.size():
		var n := 0
		for m in sps[s].meshes:
			if m != null and m.has_meta("tree_browse"):
				n += 1
		if n == 1 and TreeGenerator.get_living_meshes(sps[s]).size() < sps[s].meshes.size():
			grown += 1
	_check(grown == with, "under a browse disc the field grew %d of %d species' browsed variant, not counted living" % [grown, with])
	var inside: Array = count.call(mid, 0.0, r * 0.75)
	var outside: Array = count.call(mid, r * 1.05, SIZE)
	_check(inside[0] > 0 and outside[0] == 0, "a browse disc (r %.0f m, pressure 1): %d browsed of %d instances under it, %d browsed outside" % [r, inside[0], inside[1], outside[0]])
	ws.remove_browse(id)
	ws.clear_browse()


func _strike_stream_check() -> void:
	# Deferred struck variants in a streamed field (trees.md 5bc): a strike after the field has streamed in grows the
	# variants on a worker, and the field streams again with the struck tree in it.
	var ws = WorldContracts.get_stand()
	ws.clear_strikes()
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	var cam := Camera3D.new()
	add_child(cam)
	cam.position = Vector3(SIZE * 0.5, 30, SIZE * 0.5)
	cam.rotation_degrees = Vector3(89, 0, 0) # At the sky: frames stay cheap on the software renderer.
	cam.current = true
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 20.0
	f.streaming = true
	f.stream_radius = SIZE
	f.far_shell = false
	f.far_volume = false
	f.biome = biome
	add_child(f)
	f.data_map = f.get_path_to(dm)
	var struck_count := func() -> int:
		var n := 0
		for c in f.find_children("*", "MultiMeshInstance3D", true, false):
			if c is MultiMeshInstance3D and c.multimesh != null and c.multimesh.mesh != null and c.multimesh.mesh.has_meta("tree_lightning_years"):
				n += c.multimesh.instance_count
		return n
	for k in 600:
		await get_tree().process_frame
		if f.get_plant_count() > 1000 and k > 60:
			break
	var before: int = struck_count.call()
	print("streamed in: %d plants, %d struck" % [f.get_plant_count(), before])
	var at := Vector3(SIZE * 0.5, 0, SIZE * 0.5)
	var pl: PackedVector3Array = f.get_placements(0)
	if pl.size() > 0:
		at = pl[pl.size() / 2]
	ws.add_strike(at + Vector3(0, 25, 0), 0.1)
	var t0 := Time.get_ticks_msec()
	var after := 0
	var start_count := f.get_plant_count()
	var lowest := start_count
	var longest := 0
	var tf := Time.get_ticks_usec()
	for k in 3000:
		await get_tree().process_frame
		var now := Time.get_ticks_usec()
		longest = maxi(longest, now - tf)
		tf = now
		lowest = mini(lowest, f.get_plant_count())
		if k % 10 == 0:
			after = struck_count.call()
			if k % 200 == 0:
				print("frame %d: %d plants, %d struck" % [k, f.get_plant_count(), after])
			if after > 0:
				break
	_check(before == 0 and after >= 1, "streamed: no struck tree before (%d), %d after a strike, in %d ms" % [before, after, Time.get_ticks_msec() - t0])
	# The field is placed again tile by tile, the old tiles showing until their replacements are in: nothing blinks out.
	_check(lowest >= start_count * 0.9, "streamed: the field kept its plants while it re-placed (lowest %d of %d; longest frame %d ms on the software renderer)" % [lowest, start_count, longest / 1000])
	ws.clear_strikes()


func _strike_field_check() -> void:
	# Struck variants in the world (trees.md 5ax): the beech stand's tall species carry two struck variants (fresh and
	# healing) that are not living ones; with no strike none is placed; a WorldStand strike at one canopy tree swaps that
	# tree for the fresh variant and no other.
	var ws = WorldContracts.get_stand()
	if ws == null:
		_check(false, "struck trees: no WorldStand")
		return
	ws.clear_strikes()
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	var sps: Array = biome.species
	var with := -1
	for s in sps.size():
		if (int(sps[s].get_meta("tree_variants_deferred", 0)) & TreeGenerator.VARIANT_STRIKE) and with < 0:
			with = s
	# Deferred (trees.md 5bc): none grown until a field has a strike over it.
	var grown := 0
	for s in sps.size():
		for m in sps[s].meshes:
			if m != null and m.has_meta("tree_lightning_years"):
				grown += 1
	_check(with >= 0 and grown == 0, "beech stand: %s defers its struck variants (%d grown before any strike)" % [sps[with].resource_name if with >= 0 else "none", grown])
	if with < 0:
		return
	var f := _field(biome)
	for k in 3:
		await get_tree().process_frame
	var count_struck := func(center: Vector3, radius: float) -> Array:
		# The field's drawn instances (each multimesh draws one species mesh), struck ones by their mesh's meta.
		var n_struck := 0
		var n_all := 0
		for c in f.find_children("*", "MultiMeshInstance3D", true, false):
			if not (c is MultiMeshInstance3D) or c.multimesh == null or c.multimesh.mesh == null:
				continue
			var mm: MultiMesh = c.multimesh
			var is_struck: bool = mm.mesh.has_meta("tree_lightning_years")
			for k in mm.instance_count:
				var o: Vector3 = (c.global_transform * mm.get_instance_transform(k)).origin
				if Vector2(o.x - center.x, o.z - center.z).length() <= radius:
					n_all += 1
					if is_struck:
						n_struck += 1
		return [n_struck, n_all]
	var before: Array = count_struck.call(Vector3(SIZE * 0.5, 0, SIZE * 0.5), SIZE)
	var pl: PackedVector3Array = f.get_placements(with)
	_check(before[1] > 0 and before[0] == 0 and pl.size() > 0, "no strike: %d trunks, %d struck" % [before[1], before[0]])
	if pl.is_empty():
		return
	var at: Vector3 = pl[pl.size() / 2]
	ws.add_strike(at + Vector3(0, 25, 0), 0.1)
	f.populate()
	for k in 3:
		await get_tree().process_frame
	var near: Array = count_struck.call(at, 0.5)
	var all: Array = count_struck.call(Vector3(SIZE * 0.5, 0, SIZE * 0.5), SIZE)
	var struck := 0
	for m in sps[with].meshes:
		if m != null and m.has_meta("tree_lightning_years"):
			struck += 1
	_check(struck == 2 and TreeGenerator.get_living_meshes(sps[with]).size() <= sps[with].meshes.size() - struck, "under a strike the field grew %s's %d struck variants, not counted living" % [sps[with].resource_name, struck])
	_check(near[0] == 1 and all[0] >= 1 and all[0] <= 4, "a strike at %s: the tree there struck (%d), %d struck in the stand (neighbours within 3 m may share it)" % [str(at), near[0], all[0]])
	ws.clear_strikes()

func _lightning_check() -> void:
	# A fresh strike (trees.md 5ax): bark slabs and splinters round a forest spruce's foot, none two years on; the
	# fresh strip itself is checked by eye (raster and path traced, trees.md 5ax).
	var g := TreeGenerator.new()
	g.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
	g.seed = 3
	g.forest_shade = 0.6
	g.near_leaf_range = 0.0
	var counts := []
	var far := 0.0
	for y in [-1.0, 0.05, 3.0]:
		g.lightning_years = y
		var m := g.generate()
		var si := _surface(m, "wood")
		var n := 0
		if si >= 0:
			for q in m.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]:
				if q.y < 0.15 and Vector2(q.x, q.z).length() > 0.6:
					n += 1
					far = maxf(far, Vector2(q.x, q.z).length())
		counts.append(n)
	_check(counts[1] > 2000 and counts[0] == 0 and counts[2] == 0 and far < 25.0, "spruce strike debris: %d vertices on the ground fresh (out to %.1f m), %d unstruck, %d three years on" % [counts[1], far, counts[0], counts[2]])

func _deer_check() -> void:
	# Hedged saplings (trees.md 5aw): fifteen years browsed stay under the reach on every seed; unbrowsed they grow away.
	for pr in [TreeGenerator.PRESET_BEECH, TreeGenerator.PRESET_ASH, TreeGenerator.PRESET_ROWAN, TreeGenerator.PRESET_NORWAY_SPRUCE]:
		var g := TreeGenerator.new()
		g.preset = pr
		g.near_leaf_range = 0.0
		g.set_param(TreeGenerator.PARAM_YEARS, 15.0)
		var tallest := 0.0
		var free := 0.0
		var leaves_min := 1 << 30
		for sd in range(1, 5):
			g.seed = sd
			g.browse = 0.9
			var m := g.generate()
			tallest = maxf(tallest, m.get_aabb().end.y)
			leaves_min = mini(leaves_min, g.get_leaf_count())
			g.browse = 0.0
			free = maxf(free, g.generate().get_aabb().end.y)
		_check(tallest < 2.3 and free > tallest + 0.5 and leaves_min > 0, "%s browsed 15 years: top %.2f m (unbrowsed %.2f m), in leaf (%d leaves at least)" % [{TreeGenerator.PRESET_BEECH: "beech", TreeGenerator.PRESET_ASH: "ash", TreeGenerator.PRESET_ROWAN: "rowan", TreeGenerator.PRESET_NORWAY_SPRUCE: "spruce"}[pr], tallest, free, leaves_min])
	# The browse line: an open-grown holly (leafy to the ground unbrowsed) browsed its last 20 years keeps no leaves
	# under the line, and its crown above.
	var t := TreeGenerator.new()
	t.preset = TreeGenerator.PRESET_HOLLY
	t.seed = 3
	t.near_leaf_range = 0.0
	t.browse = 0.0
	var before := t.generate()
	t.browse = 1.0
	t.browse_years = 20
	var after := t.generate()
	var low := [0, 0]
	var meshes := [before, after]
	for k in 2:
		var m: ArrayMesh = meshes[k]
		var si := _surface(m, "leaves")
		if si < 0:
			continue
		var v: PackedVector3Array = m.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]
		for q in v:
			if q.y < 1.6:
				low[k] += 1
	_check(low[0] > 1000 and low[1] * 10 < low[0] and after.get_aabb().size.y > 0.8 * before.get_aabb().size.y, "holly browse line: %d leaf vertices under 1.6 m before, %d after; crown kept" % [low[0], low[1]])
	# Nurse logs: seedlings on a class 4 spruce log, on its top.
	var lg := TreeGenerator.new()
	lg.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
	lg.seed = 2
	lg.fallen = true
	lg.decay_class = 4
	lg.near_leaf_range = 0.0
	var bare := lg.generate()
	lg.nurse_seedlings = 1.0
	var nursed := lg.generate()
	var nb := _surface(bare, "needles")
	var nn := _surface(nursed, "needles")
	var vb: int = bare.surface_get_array_len(nb) if nb >= 0 else 0
	var vn: int = nursed.surface_get_array_len(nn) if nn >= 0 else 0
	_check(vn > vb + 3000, "nurse log: %d needle vertices of seedlings (bare log %d)" % [vn - vb, vb])
	# A tree on a rotted nurse log stands lifted by the log's girth, a tunnel under its foot along X.
	var st := TreeGenerator.new()
	st.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
	st.seed = 2
	st.near_leaf_range = 0.0
	st.nurse_stilt = 0.8
	var sm := st.generate()
	var lift: float = sm.get_meta("tree_base_lift", 0.0)
	var bi := _surface(sm, "bark")
	var in_tunnel := 0
	if bi >= 0:
		var bv: PackedVector3Array = sm.surface_get_arrays(bi)[Mesh.ARRAY_VERTEX]
		for q in bv:
			# Inside the old log's section, near the stem's foot: none of the bark may hang there.
			if absf(q.z) < 0.15 and absf(q.x) < 0.3 and q.y < -0.25 and q.y > -0.75:
				in_tunnel += 1
	_check(absf(lift - 0.8) < 0.01 and in_tunnel == 0, "tree on a rotted nurse log: lifted %.2f m, %d bark vertices in the tunnel" % [lift, in_tunnel])

func _beaver_check() -> void:
	for pr in [TreeGenerator.PRESET_WHITE_WILLOW, TreeGenerator.PRESET_BIRCH]:
		var g := TreeGenerator.new()
		g.preset = pr
		g.near_leaf_range = 0.0
		g.set_param(TreeGenerator.PARAM_YEARS, 18.0)
		var nm: String = ["willow", "birch"][0 if pr == TreeGenerator.PRESET_WHITE_WILLOW else 1]
		var stumps := 0
		var cones := 0
		var sprouting := 0
		for sd in range(1, 7):
			g.seed = sd
			g.beaver_cut = TreeGenerator.BEAVER_STUMP
			g.beaver_years = 3.0
			var m := g.generate()
			var ab: AABB = m.get_aabb()
			var wi := _surface(m, "wood")
			var top := 0.0
			if wi >= 0:
				for p in m.surface_get_arrays(wi)[Mesh.ARRAY_VERTEX]:
					top = maxf(top, p.y)
			var leafy := _surface(m, "leaves") >= 0
			if leafy:
				sprouting += 1
			elif ab.size.y < 1.2:
				stumps += 1
			if top > 0.25:
				cones += 1
		_check(stumps + sprouting == 6 and cones == 6 and sprouting > 0 and sprouting < 6, "%s: beaver stumps: %d bare, %d resprouting, %d with a gnawed cone over the cut" % [nm, stumps, sprouting, cones])
		g.seed = 3
		g.beaver_cut = TreeGenerator.BEAVER_FELLED
		g.beaver_years = 1.0
		var f := g.generate()
		var fb: AABB = f.get_aabb()
		_check(maxf(fb.size.x, fb.size.z) > 1.5 * fb.size.y and _surface(f, "wood") >= 0, "%s: beaver-felled stem lies (%.1f m long, %.1f m high) with its butt cone" % [nm, maxf(fb.size.x, fb.size.z), fb.size.y])
		g.beaver_cut = TreeGenerator.BEAVER_NONE
		var whole := g.generate()
		g.beaver_cut = TreeGenerator.BEAVER_GNAWED
		g.beaver_depth = 0.7
		var gn := g.generate()
		var r0 := _band_min_r(whole, 0.15, 0.7)
		var r1 := _band_min_r(gn, 0.15, 0.7)
		_check(r0 > 0.0 and r1 > 0.0 and r1 < 0.85 * r0, "%s: gnawed waist pulls the stem in (median radius %.3f against %.3f in the narrowest slice, 0.15-0.7 m)" % [nm, r1, r0])
		g.beaver_cut = TreeGenerator.BEAVER_NONE

# Forests that age (trees.md 5aq): the four structural stages (Oliver and Larson 1996) all present, each species'
# age range forward and its kind a RULE_DISTURBANCE number or -1, the pole stage shorter than the mature lead and
# the veterans no shorter, and some young stage covering every year from 0 to old growth.
func _succession_check(i: int) -> void:
	var biome := gen.make_succession(i, variants)
	var sps: Array = biome.get_species()
	var stage := {"legacy": 0, "initiation": 0, "poles": 0, "mature": 0, "old": 0}
	var bad := 0
	var covered := PackedFloat32Array()
	var pole_h := 0.0
	var mature_h := 0.0
	var vet_h := 0.0
	var old_from := 1e9
	for sp in sps:
		var nm: String = sp.resource_name
		var r: Vector2 = sp.get_meta("succession_age", Vector2(-1, -1))
		var kind: int = sp.get_meta("succession_kind", -9)
		if r.x < 0.0 or r.y <= r.x or kind < -1 or kind > 3:
			bad += 1
			print("  bad range or kind: %s %s %d" % [nm, r, kind])
		var h := 0.0
		for m in sp.meshes:
			h = max(h, m.get_aabb().size.y)
		if nm.contains("legacy"):
			stage.legacy += 1
		elif r.x == 0.0 or nm.contains("seedlings"):
			stage.initiation += 1
		elif nm.contains("pole stage") or nm.contains("self-thinned"):
			stage.poles += 1
			if nm.contains("pole stage") and not nm.begins_with("Birch"):
				pole_h = max(pole_h, h)
		elif r.y < 9999.0:
			stage.mature += 1
			mature_h = max(mature_h, h)
		else:
			stage.old += 1
			old_from = min(old_from, r.x)
			if nm.contains("veteran") or nm.contains("giant"):
				vet_h = max(vet_h, h)
		print("  %s: %.0f-%.0f y, kind %d, %.1f m" % [nm, r.x, min(r.y, 9999.0), kind, h])
	# Every year 0..old growth has a living stage.
	var gaps := 0
	for y in range(0, int(old_from), 1):
		var any := false
		for sp in sps:
			var r2: Vector2 = sp.get_meta("succession_age", Vector2(-1, -1))
			if not sp.resource_name.contains("legacy") and y >= r2.x and y < r2.y:
				any = true
				break
		if not any:
			gaps += 1
	var name_i: String = STAND_NAMES[i] if i < STAND_NAMES.size() else str(i)
	_check(bad == 0 and gaps == 0 and stage.initiation > 0 and stage.poles > 0 and stage.mature > 0 and stage.old > 0, "succession of %s: %d legacies, %d initiation, %d pole stage, %d mature, %d old growth from %.0f y; %d bad, %d years uncovered" % [name_i, stage.legacy, stage.initiation, stage.poles, stage.mature, stage.old, old_from, bad, gaps])
	_check(pole_h > 0.0 and pole_h < mature_h and (vet_h == 0.0 or vet_h >= mature_h * 0.9), "succession of %s: poles %.1f m under mature %.1f m, veterans %.1f m" % [name_i, pole_h, mature_h, vet_h])

func _check(ok: bool, what: String) -> void:
	if not ok:
		fails += 1
	print(("PASS " if ok else "FAIL ") + what)

func _build_ground() -> void:
	var valley := Valley.new()
	valley.name = "Valley"
	add_child(valley)
	var stream := Stream.new()
	stream.name = "Stream"
	add_child(stream)
	dm = TerrainDataMap3D.new()
	dm.name = "Map"
	dm.size = SIZE
	dm.resolution = 160
	dm.bake_on_ready = false
	add_child(dm)
	dm.height_source = dm.get_path_to(valley)
	dm.water_sources = [dm.get_path_to(stream)]

func _field(biome: Biome) -> VegetationField3D:
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 40.0
	f.biome = biome
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	return f

func _biome(i: int) -> Biome:
	if i == TreeGenerator.STAND_MAX:
		return gen.make_woodland(variants)
	if i == TreeGenerator.STAND_MAX + 1:
		return gen.make_woodland(variants, TreeGenerator.REGION_NORTH_AMERICA)
	if OS.get_environment("EDGE") != "":
		# EDGE=1: the stand with its forest edge (make_edge: mantle and one-sided edge trees, RULE_FOREST_EDGE).
		var b := gen.make_stand(i, variants)
		var both: Array[PlantSpecies] = []
		for sp in gen.make_edge(i, variants).species:
			both.append(sp)
		for sp in b.species:
			both.append(sp)
		b.species = both
		return b
	if OS.get_environment("AGE") != "":
		# AGE=years [KIND=n]: make_succession(i) as the stand-age rules would place it at one age (trees.md 5aq),
		# the legacies of disturbance KIND (default 2, wind) only.
		var age := float(OS.get_environment("AGE"))
		var kind := int(OS.get_environment("KIND")) if OS.get_environment("KIND") != "" else 2
		var all := gen.make_succession(i, variants)
		var keep: Array[PlantSpecies] = []
		for sp in all.species:
			var r: Vector2 = sp.get_meta("succession_age")
			var k: int = sp.get_meta("succession_kind")
			if age >= r.x and age < r.y and (k == -1 or k == kind):
				keep.append(sp)
		all.species = keep
		return all
	return gen.make_stand(i, variants)

func _habitat_checks() -> void:
	var sp := PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_BEECH
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_PRECIPITATION, 450.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_PRECIPITATION, 1000.0) == 1.0, "beech needs rain (none at 450 mm, full at 1000 mm)")
	_check(sp.rule_weight(PlantSpecies.RULE_JANUARY_TEMPERATURE, -15.0) == 0.0, "beech absent where January averages -15 C")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_NORWAY_SPRUCE
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 13.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 3.0) == 1.0, "spruce in cold belts (none at 13 C annual, full at 3 C)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_BLACK_ALDER
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 100.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_WATER_DISTANCE, 10.0) == 1.0, "alder by water (none 100 m away, full at 10 m)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_SCOTS_PINE
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_ARIDITY, 0.5) == 1.0, "pine takes dry ground (aridity 0.5)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_PAPER_BIRCH
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 14.0) == 0.0 and sp.rule_weight(PlantSpecies.RULE_JANUARY_TEMPERATURE, -20.0) == 1.0, "paper birch is boreal (none at 14 C annual, full at -20 C January)")
	sp = PlantSpecies.new()
	gen.preset = TreeGenerator.PRESET_WHITE_OAK
	gen.apply_habitat(sp)
	_check(sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 18.0) == 1.0 and sp.rule_weight(PlantSpecies.RULE_TEMPERATURE, 3.0) == 0.0, "white oak in the warm south (full at 18 C, none at 3 C)")

func _stand_checks(only := -1) -> void:
	print("stand              species                     placed  per_ha")
	var area_ha := SIZE * SIZE / 10000.0
	for i in TreeGenerator.STAND_MAX + 2:
		if only >= 0 and i != only:
			continue
		var t := Time.get_ticks_msec()
		var biome := _biome(i)
		var f := _field(biome)
		var sps: Array = biome.species
		# Disturbed stands report their patches to the stand-age map (WorldStand; trees.md 5aq): at a legacy
		# plant's spot the map gives its kind and, the youngest event winning, an age no older than its years.
		var reported := 0
		var wrong_kind := 0
		var ws = WorldContracts.get_stand()
		for s in sps.size():
			if sps[s].has_meta("stand_disturbance") and ws:
				var kd: Vector2 = sps[s].get_meta("stand_disturbance")
				var pl: PackedVector3Array = f.get_placements(s)
				for j in range(0, pl.size(), max(1, pl.size() / 8)):
					reported += 1
					var k: int = ws.disturbance_at(pl[j])
					var age: float = ws.stand_age_at(pl[j])
					if k == 0 or age < 0.0 or age > kd.y + 0.01:
						wrong_kind += 1
		if reported > 0:
			_check(wrong_kind * 4 < reported, "%s: stand-age map has the disturbance at %d of %d legacy plants" % [STAND_NAMES[i], reported - wrong_kind, reported])
			# A populate that places the same stand reports nothing new (fields reading the stand rules would wake).
			var gen0: int = ws.get_stand_generation()
			f.populate()
			_check(ws.get_stand_generation() == gen0, "%s: populating again leaves the stand-age map unchanged" % STAND_NAMES[i])
		if ws:
			ws.clear_disturbances()
		var stand_count := 0
		var open_count := 0
		var canopy_total := 0
		var regen := {"sapling": 0, "snag": 0, "log": 0, "fallen branch": 0, "sticks": 0}
		for s in sps.size():
			var n: int = f.get_placements(s).size()
			var nm: String = sps[s].resource_name
			print("%-18s %-27s %6d  %6.0f" % [STAND_NAMES[i], nm, n, n / area_ha])
			# The wet stands name their stand-grown canopy by its site (trees.md 5at).
			if nm.ends_with("(stand)") or nm.contains("stilt-rooted") or nm.contains("(old, floodplain)"):
				stand_count += n
				canopy_total += n
			elif nm.ends_with("(open)"):
				open_count += n
			for k in regen:
				if nm.ends_with("(%s)" % k):
					regen[k] += n
		print("  %d plants in %d ms (meshes and placement)" % [f.get_plant_count(), Time.get_ticks_msec() - t])
		if gen.fire_variants:
			# Each living tree species carries one variant per fire band after its living ones (world-links.md).
			for s in sps.size():
				var nm2: String = sps[s].resource_name
				if nm2.ends_with("(stand)") or nm2.ends_with("(open)") or nm2.ends_with("(grove)"):
					var bands := []
					for m in sps[s].meshes:
						if m != null and m.has_meta("tree_fire_band"):
							bands.append(m.get_meta("tree_fire_band"))
					_check(bands.size() == 5 and bands[0] == Vector2(0, 1) and bands[4] == Vector2(10, 20), "%s: fire band variants %s" % [nm2, str(bands)])
					break
		var cut := {"coppice": 0, "young coppice": 0, "pollard": 0}
		for s in sps.size():
			for k in cut:
				if String(sps[s].resource_name).ends_with("(%s)" % k):
					cut[k] += f.get_placements(s).size()
		# Aspen clones (trees.md 5bf): with several variants each clone is one variant, so near neighbours mostly share it.
		if variants > 1:
			for s in sps.size():
				if not String(sps[s].resource_name).begins_with("Aspen"):
					continue
				var pts := []
				for mmi in f.find_children("*", "MultiMeshInstance3D", true, false):
					var mm: MultiMesh = mmi.multimesh
					if mm == null:
						continue
					var vi: int = sps[s].meshes.find(mm.mesh)
					if vi < 0:
						continue
					for k in mm.instance_count:
						var o3: Vector3 = (mmi.global_transform * mm.get_instance_transform(k)).origin
						pts.append([Vector2(o3.x, o3.z), vi])
				var same := 0
				var pairs := 0
				for a in pts.size():
					for b in range(a + 1, pts.size()):
						if pts[a][0].distance_to(pts[b][0]) < 4.0:
							pairs += 1
							same += 1 if pts[a][1] == pts[b][1] else 0
				_check(pairs > 0 and same > pairs * 0.6, "%s: %s, %d of %d neighbour pairs within 4 m share a clone" % [STAND_NAMES[i], sps[s].resource_name, same, pairs])
		# Cliff trees (trees.md 5bg): every variant carries its face's slope (placement as for banks), stands out of the
		# face rather than into it (little of the mesh behind the face plane, which the roots and collar sink into), is
		# far smaller than the open-grown tree, and its crown is over the open side, not over the rock.
		for s in sps.size():
			if not String(sps[s].resource_name).ends_with("(cliff)"):
				continue
			var bad := 0
			var worst_behind := 0.0
			var tops := []
			for m in sps[s].meshes:
				if m == null:
					continue
				var gs: Vector2 = m.get_meta("tree_ground_slope", Vector2())
				if gs.x < 0.9:
					bad += 1
					continue
				var nrm := Vector3(0, cos(gs.x), sin(gs.x))
				var behind := 0
				var total := 0
				var top := Vector3(0, -1e9, 0)
				for si in m.get_surface_count():
					var vs: PackedVector3Array = m.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]
					for j in range(0, vs.size(), 7):
						total += 1
						if vs[j].dot(nrm) < -0.35:
							behind += 1
						if vs[j].y > top.y:
							top = vs[j]
				worst_behind = max(worst_behind, float(behind) / max(total, 1))
				tops.append("%.1f m up, %.1f m off the face" % [top.y, top.dot(nrm)])
				if float(behind) / max(total, 1) > 0.03 or top.dot(nrm) < 0.3 or top.y > 16.0:
					bad += 1
			_check(sps[s].meshes.size() >= 1 and bad == 0, "%s: %s, %d variants on faces, worst %.1f%% behind the face; tops %s" % [STAND_NAMES[i], sps[s].resource_name, sps[s].meshes.size(), worst_behind * 100.0, str(tops)])

		if i == TreeGenerator.STAND_HEDGEROW:
			_check(f.get_plant_count() > 0 and open_count == f.get_plant_count(), "hedgerow: shrubs and open-grown standards only (%d)" % open_count)
		elif i == TreeGenerator.STAND_WOOD_PASTURE:
			_check(cut.pollard > 0 and regen.sapling == 0 and regen.log > 0, "wood pasture: %d lapsed pollards, no saplings (grazed), %d logs" % [cut.pollard, regen.log])
		elif i == TreeGenerator.STAND_STONE_PINE:
			_check(open_count > 0 and stand_count == 0 and regen.sapling > 0 and regen.log > 0, "stone pine parkland: %d open-grown umbrellas, %d saplings, %d logs" % [open_count, regen.sapling, regen.log])
		elif i == TreeGenerator.STAND_OLIVE_GROVE:
			# Old olive grove (trees.md, "Ancient olive"): pruned olives about 10 m apart, a tidy
			# worked grove (no saplings or deadwood).
			var olives := 0
			for s in sps.size():
				if String(sps[s].resource_name).ends_with("(grove)"):
					olives += f.get_placements(s).size()
			var per_ha := olives / (SIZE * SIZE / 10000.0)
			_check(olives > 0 and per_ha > 40.0 and per_ha < 130.0 and regen.sapling == 0 and regen.log == 0, "olive grove: %d olives (%.0f a hectare), no saplings or logs" % [olives, per_ha])
		elif i == TreeGenerator.STAND_DATE_OASIS:
			# Date palm oasis (trees.md 5ai): tended grove palms in rows about 8 m apart, a few wild clumps,
			# olives beneath, no deadwood.
			var palms := 0
			for s in sps.size():
				if String(sps[s].resource_name).ends_with("(grove)"):
					palms += f.get_placements(s).size()
			var per_ha := palms / (SIZE * SIZE / 10000.0)
			_check(palms > 0 and per_ha > 35.0 and per_ha < 220.0 and regen.log == 0, "date oasis: %d grove palms (%.0f a hectare), no logs" % [palms, per_ha])
		elif i == TreeGenerator.STAND_BEETLE_SPRUCE:
			# Spruce after bark beetle (trees.md 5aj): a red front and a grey core of killed spruce in spots,
			# living spruce between; every killed variant carries its years since the attack.
			var front := 0
			var grey := 0
			var tagged := true
			for s in sps.size():
				var nm3: String = sps[s].resource_name
				if nm3.contains("(beetle front)"):
					front += f.get_placements(s).size()
				elif nm3.contains("(beetle grey)"):
					grey += f.get_placements(s).size()
				else:
					continue
				for m in sps[s].meshes:
					var yrs: float = m.get_meta("tree_beetle_years", -1.0)
					tagged = tagged and (yrs < 2.5 if nm3.contains("front") else yrs >= 2.5)
			_check(front > 0 and grey > 0 and stand_count > 0 and tagged, "beetle spruce: %d red-front, %d grey, %d living spruce; variants tagged by stage" % [front, grey, stand_count])
		elif i == TreeGenerator.STAND_WINDTHROW:
			# Spruce after windthrow (trees.md 5am): thrown logs and snapped stumps in the gaps, every thrown and
			# leaning tree turned down the storm's track (mesh +X along windthrow_direction), half with root plates.
			var thrown := 0
			var snapped := 0
			var leaning := 0
			var off := 0
			var plates := 0
			var checked := 0
			var dir := Vector2(0.92, -0.38).normalized()
			for s in sps.size():
				var nm4: String = sps[s].resource_name
				var pl: PackedVector3Array = f.get_placements(s)
				if nm4.contains("(windthrow)"):
					thrown += pl.size()
					for m in sps[s].meshes:
						if m.has_meta("tree_root_pit"):
							plates += 1
				elif nm4.contains("(windthrow snapped)"):
					snapped += pl.size()
				elif nm4.contains("(windthrow leaning)"):
					leaning += pl.size()
				else:
					continue
				if not sps[s].has_meta("fall_dir") and not nm4.contains("snapped"):
					off += pl.size()
				if nm4.contains("snapped"):
					continue
				# The drawn instances of its meshes: +X down the storm's track.
				for mmi in f.find_children("*", "MultiMeshInstance3D", true, false):
					var mm: MultiMesh = mmi.multimesh
					if mm == null or not sps[s].meshes.has(mm.mesh):
						continue
					checked += mm.instance_count
					for k in mm.instance_count:
						var x: Vector3 = mm.get_instance_transform(k).basis.x
						if Vector2(x.x, x.z).normalized().dot(dir) < cos(0.5):
							off += 1
			_check(thrown > 0 and snapped > 0 and leaning > 0 and stand_count > 0 and off == 0 and plates > 0, "windthrow: %d thrown (%d variants with root plates), %d snapped, %d leaning, %d standing; %d of %d drawn turned off the storm's track" % [thrown, plates, snapped, leaning, stand_count, off, checked])
		elif i == TreeGenerator.STAND_ICE_STORM:
			# An ice storm stand (trees.md 5be): bent paper birch, broken tops and stripped crowns all placed, each
			# damaged variant carrying its kind in tree_ice_storm (1 bent, 2 top broken, 3 limbs), hemlock whole.
			var placed := [0, 0, 0, 0]
			var kinds := [0, 0, 0, 0]
			var wrong := 0
			for s in sps.size():
				var nm7: String = sps[s].resource_name
				var want := 1 if nm7.contains("bent by ice") else (2 if nm7.contains("ice-broken top") else (3 if nm7.contains("ice-stripped") else 0))
				if want == 0:
					continue
				placed[want] += f.get_placements(s).size()
				for m in sps[s].meshes:
					var meta: Vector2 = m.get_meta("tree_ice_storm", Vector2(-1, 0)) if m != null else Vector2(-1, 0)
					if meta.x < 0.0:
						wrong += 1
					elif int(meta.y) == want or (want == 2 and int(meta.y) == 3):
						kinds[want] += 1
					else:
						wrong += 1
			_check(placed[1] > 0 and placed[2] > 0 and placed[3] > 0 and kinds[1] > 0 and kinds[2] > 0 and kinds[3] > 0 and wrong == 0, "ice storm: placed %d bent, %d broken-top, %d stripped; variants %d / %d / %d, %d without their damage" % [placed[1], placed[2], placed[3], kinds[1], kinds[2], kinds[3], wrong])
		elif i == TreeGenerator.STAND_DEER_WOOD:
			# A deer wood (trees.md 5aw): browsed saplings placed, every species browsed (none unbrowsed), no shrub layer.
			var hedged := 0
			var unbrowsed := 0
			for s in sps.size():
				var nm6: String = sps[s].resource_name
				var pl: PackedVector3Array = f.get_placements(s)
				if nm6.contains("browsed sapling"):
					hedged += pl.size()
				if sps[s].has_meta("preset") and int(sps[s].get_meta("preset")) in [TreeGenerator.PRESET_HAZEL, TreeGenerator.PRESET_HAWTHORN, TreeGenerator.PRESET_HOLLY, TreeGenerator.PRESET_ELDER]:
					unbrowsed += pl.size()
			_check(hedged > 0 and unbrowsed == 0, "deer wood: %d hedged saplings, %d shrub-layer plants" % [hedged, unbrowsed])
		elif i == TreeGenerator.STAND_BEAVER_POND:
			# A beaver pond (trees.md 5au): the colony's work in a band by the water (none past 12 m), the felled stems
			# mostly turned toward the water (mesh +X down the water distance), and drowned snags standing in it.
			var work := 0
			var far := 0
			var drowned := 0
			var felled := 0
			var to_water := 0
			for s in sps.size():
				var nm5: String = sps[s].resource_name
				var pl: PackedVector3Array = f.get_placements(s)
				if nm5.contains("(drowned"):
					drowned += pl.size()
					continue
				if not nm5.contains("(beaver "):
					continue
				work += pl.size()
				for p in pl:
					if dm.get_water_distance_at(f.to_global(p)) > 12.5:
						far += 1
				if not nm5.contains("felled"):
					continue
				for mmi in f.find_children("*", "MultiMeshInstance3D", true, false):
					var mm: MultiMesh = mmi.multimesh
					if mm == null or not sps[s].meshes.has(mm.mesh):
						continue
					for k in mm.instance_count:
						var xf: Transform3D = mmi.global_transform * mm.get_instance_transform(k)
						var o := xf.origin
						var gx := dm.get_water_distance_at(o + Vector3(1.5, 0, 0)) - dm.get_water_distance_at(o - Vector3(1.5, 0, 0))
						var gz := dm.get_water_distance_at(o + Vector3(0, 0, 1.5)) - dm.get_water_distance_at(o - Vector3(0, 0, 1.5))
						var down := -Vector2(gx, gz)
						if down.length() < 1e-4:
							continue
						felled += 1
						var x := xf.basis.x
						if Vector2(x.x, x.z).normalized().dot(down.normalized()) > cos(0.6):
							to_water += 1
			_check(work > 0 and far == 0 and felled > 0 and to_water * 2 > felled and drowned > 0 and stand_count > 0, "beaver pond: %d gnawed, cut and felled (%d past 12 m from the water), %d of %d felled stems turned to the water, %d drowned snags" % [work, far, to_water, felled, drowned])
		elif i == TreeGenerator.STAND_OLD_GROWTH_BEECH:
			# Old-growth beech (trees.md 5an): veteran giants among the canopy (about ten a hectare), a fallen giant in
			# the gaps, poles and seedlings under them, lying deadwood well over standing.
			var giants := 0
			var gap_logs := 0
			var poles := 0
			var lying := 0
			var standing_dead := 0
			var vet_ok := true
			for s in sps.size():
				var nm5: String = sps[s].resource_name
				var n5: int = f.get_placements(s).size()
				if nm5.contains("old-growth") or nm5.ends_with("(log)"):
					var hs := []
					for m in sps[s].meshes:
						var ab: AABB = m.get_aabb()
						hs.append("%.0fx%.0f %s" % [ab.size.y, max(ab.size.x, ab.size.z), str(m.get_meta("crown", Vector3()))])
					print("  %s: %s" % [nm5, ", ".join(hs)])
				if nm5.contains("old-growth veteran"):
					giants += n5
				elif nm5.contains("fallen giant"):
					gap_logs += n5
					lying += n5
				elif nm5.contains("broken giant"):
					standing_dead += n5
				elif nm5.contains("pole"):
					poles += n5
				elif nm5.contains("(log)"):
					lying += n5
				elif nm5.contains("(snag)"):
					standing_dead += n5
			var ha: float = SIZE * SIZE / 10000.0
			_check(giants / ha >= 4.0 and giants / ha <= 25.0 and gap_logs > 0 and poles > 0 and lying > 2 * standing_dead, "old-growth beech: %.1f veteran giants a hectare, %d fallen giants in gaps, %d poles, %d lying to %d standing dead" % [giants / ha, gap_logs, poles, lying, standing_dead])
		elif i == TreeGenerator.STAND_BOREAL_TAIGA:
			# Taiga (trees.md 5ap): spruce and pine in a mosaic, fire-survivor pines, kelo standing and down, birch in burns.
			var tg := {"survivor": 0, "kelo": 0, "burn": 0, "spruce": 0, "pine": 0}
			for s in sps.size():
				var nm6: String = sps[s].resource_name
				var n6: int = f.get_placements(s).size()
				for k in tg.keys():
					if nm6.contains("taiga" if k == "pine" else k) and (k != "pine" or nm6.begins_with("Scots pine (taiga)")):
						tg[k] += n6
				if nm6.contains("taiga") or nm6.contains("kelo"):
					var hs6 := []
					for m in sps[s].meshes:
						var ab6: AABB = m.get_aabb()
						hs6.append("%.0fx%.0f" % [ab6.size.y, max(ab6.size.x, ab6.size.z)])
					print("  %s: %d, %s" % [nm6, n6, ", ".join(hs6)])
			_check(tg.survivor > 0 and tg.kelo > 0 and tg.burn > 0 and tg.spruce > 0 and tg.pine > 0, "boreal taiga: %d fire survivors, %d kelo, %d burn birch, %d spruce, %d pine" % [tg.survivor, tg.kelo, tg.burn, tg.spruce, tg.pine])
		elif i == TreeGenerator.STAND_POLLARD_ROW:
			_check(cut.pollard > 0 and cut.pollard >= open_count, "pollard row: %d willow pollards, %d alders" % [cut.pollard, open_count])
		elif i == TreeGenerator.STAND_COPPICE_STANDARDS:
			# Standards are open-grown (spreading crowns over the underwood).
			_check(open_count > 0 and stand_count == 0, "coppice with standards: %d open-grown standards" % open_count)
			_check(cut.coppice > 0 and cut["young coppice"] > 0 and cut.coppice + cut["young coppice"] > 4 * open_count, "coppice with standards: %d grown and %d young coppice stools under %d standards" % [cut.coppice, cut["young coppice"], open_count])
			_check(regen.sapling > 0 and regen.log > 0 and regen.snag + regen.log < open_count, "coppice with standards: %d saplings, %d snags, %d logs" % [regen.sapling, regen.snag, regen.log])
			# Coupes (trees.md, "Coppice with standards"): every age band placed, each stool in a coupe
			# of its band's age, nothing woody on a ride.
			var bands := 0
			var wrong := 0
			var on_ride := 0
			var ride_points := 0
			for s in sps.size():
				var sp: PlantSpecies = sps[s]
				if sp.coupe_rotation <= 0:
					continue
				var pl: PackedVector3Array = f.get_placements(s)
				if sp.has_meta("coupe_ages") and pl.size() > 0:
					bands += 1
				for p in pl:
					var c: Vector3 = sp.get_coupe_at(f.to_global(p))
					if c.z > 0.5:
						on_ride += 1
					if int(c.x) < sp.coupe_ages.x or int(c.x) > sp.coupe_ages.y:
						wrong += 1
			for k in 400:
				var q := Vector3(randf_range(-SIZE * 0.5, SIZE * 0.5), 0.0, randf_range(-SIZE * 0.5, SIZE * 0.5))
				for s in sps.size():
					if sps[s].coupe_rotation > 0:
						if sps[s].get_coupe_at(q).z > 0.5:
							ride_points += 1
						break
			_check(bands == 4 and wrong == 0 and on_ride == 0 and ride_points > 0, "coppice with standards: %d coupe age bands placed, %d plants in a coupe of the wrong age, %d on rides (rides cover %d of 400 points)" % [bands, wrong, on_ride, ride_points])
		else:
			_check(stand_count > 0, "%s: stand-grown trees form the canopy (%d stand-grown, %d gap and shrub)" % [STAND_NAMES[i], stand_count, open_count])
			_check(regen.sapling > 0 and regen.snag > 0 and regen.log > 0 and regen.snag + regen.log < stand_count and regen["fallen branch"] > 0 and regen.sticks > regen["fallen branch"], "%s: %d saplings, %d snags, %d logs, %d fallen branches, %d sticks" % [STAND_NAMES[i], regen.sapling, regen.snag, regen.log, regen["fallen branch"], regen.sticks])
		# Climbers ride on variants 1-3 of each host (one variant in four), so need --variants=2+.
		if variants >= 2 and (i == TreeGenerator.STAND_OAK_MIXED or i == TreeGenerator.STAND_RIPARIAN):
			var climbing := 0
			for sp in sps:
				for m in sp.meshes:
					if m.has_meta("tree_climber_leaves"):
						climbing += 1
			_check(climbing > 0, "%s: %d tree variants carry climbers" % [STAND_NAMES[i], climbing])
		if i == TreeGenerator.STAND_RIPARIAN:
			var far := 0
			for p in f.get_placements(0):
				if dm.get_water_distance_at(p) > 40.0 + 16.0:
					far += 1
			_check(far == 0, "riparian: no alder beyond its 40 m (+16 m fade) of water (%d)" % far)
		if i >= TreeGenerator.STAND_MAX:
			# All stands at once on a map without climate: the gap rule must stop them stacking.
			var per_ha := canopy_total / area_ha
			_check(per_ha < 700.0, "woodland: canopy trees do not stack across stands (%.0f per ha)" % per_ha)
		f.queue_free()

func _mask_check() -> void:
	# A clearing painted into the field's density mask (red 0 inside a 40 m disc) keeps every tree
	# and woody shrub out once TreeGenerator.mask_channel points at red.
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	img.fill(Color(1, 1, 1, 1))
	for y in 64:
		for x in 64:
			if Vector2(x - 31.5, y - 31.5).length() < 16.0:
				img.set_pixel(x, y, Color(0, 1, 1, 1))
	gen.mask_channel = 0
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	gen.mask_channel = -1
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 40.0
	f.biome = biome
	f.density_mask = ImageTexture.create_from_image(img)
	f.density_mask_rect = Rect2(-SIZE * 0.5, -SIZE * 0.5, SIZE, SIZE)
	add_child(f)
	f.data_map = f.get_path_to(dm)
	f.populate()
	var inside := 0
	var total := 0
	for s in biome.species.size():
		for p in f.get_placements(s):
			total += 1
			if Vector2(p.x, p.z).length() < 16.0 / 64.0 * SIZE - 3.0:
				inside += 1
	_check(total > 0 and inside == 0, "clearing mask: no trees or shrubs in the painted clearing (%d of %d)" % [inside, total])
	f.queue_free()

func _rigged_instances(f: VegetationField3D) -> Array:
	var out := []
	for c in f.get_children(true):
		var mmi := c as MultiMeshInstance3D
		if mmi and mmi.multimesh and TreeWind3D.get_mesh_bone_count(mmi.multimesh.mesh) > 0:
			out.append(mmi)
	return out

func _wind_check() -> void:
	# Rigged stand meshes placed by the field get rest bones (TreeWind3D.fill_rest), and one
	# TreeWind3D aimed at the field poses every cell MultiMesh, near and far, from the wind.
	gen.rigged = true
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	gen.rigged = false
	var f := _field(biome)
	var mmis := _rigged_instances(f)
	_check(mmis.size() > 0, "rigged beech stand: %d cell MultiMeshes carry bones" % mmis.size())
	var wind := TreeWind3D.new()
	wind.fallback_wind = Vector2(12, 0)
	wind.collision_enabled = false
	add_child(wind)
	wind.target = wind.get_path_to(f)
	await get_tree().process_frame
	wind.set_time(1.0)
	wind.update(0.0)
	var before := []
	for mmi in mmis:
		before.append(RenderingServer.multimesh_get_buffer(mmi.multimesh.get_rid()))
	wind.set_time(2.3)
	wind.update(0.0)
	var moved := 0
	for i in mmis.size():
		if RenderingServer.multimesh_get_buffer(mmis[i].multimesh.get_rid()) != before[i]:
			moved += 1
	if wind.get_target_count() == 0:
		# A VegetationField3D older than its rigged-mesh support draws the stand unposed.
		print("SKIP rigged field: this VegetationField3D gives rigged meshes no rest bones")
		wind.queue_free()
		f.queue_free()
		return
	_check(wind.get_target_count() == mmis.size(), "wind finds every rigged cell (%d of %d)" % [wind.get_target_count(), mmis.size()])
	_check(moved == mmis.size(), "wind moves every rigged cell (%d of %d), %d instances solved in %.2f ms" % [moved, mmis.size(), wind.get_solved_instances(), wind.get_solve_msec()])
	wind.queue_free()
	f.queue_free()

# Leaf detail near, on demand (trees.md 8b): with near_leaf_range the species' far meshes carry
# simple leaves, and request_near_mesh() builds, on a worker thread, exactly the mesh the
# generator makes without the split (lobed leaves, LODs, rig and climbers); it is freed once
# nothing holds it.
func _near_leaves_check() -> void:
	# The unsplit oak is two variants of 17M and 30M vertices: keep only a fingerprint of each
	# (a hash of its arrays and LODs) and free it before the split species and its near meshes
	# are built, or the two copies overrun a 15 GB machine.
	var leaves := func(m: Mesh) -> int:
		var n := 0
		for i in m.get_surface_count():
			if m.surface_get_name(i) == "leaves":
				n += m.surface_get_array_len(i)
		return n
	var print_of := func(m: Mesh) -> Array:
		var out := [m.get_surface_count(), m.get_meta("tree_rig", null)]
		for i in m.get_surface_count():
			var a := m.surface_get_arrays(i)
			out.append(hash(a[Mesh.ARRAY_VERTEX]))
			out.append(hash(a[Mesh.ARRAY_INDEX]))
			out.append(hash(a[Mesh.ARRAY_TEX_UV]))
			a.clear()
			for l in RenderingServer.mesh_get_surface(m.get_rid(), i).get("lods", []):
				out.append(l.edge_length)
				out.append(hash(l.index_data))
		return out
	var full_prints := []
	var full_leaves := []
	var plain := TreeGenerator.new()
	plain.seed = 1
	plain.rigged = true
	plain.near_leaf_range = 0.0
	plain.set_preset(TreeGenerator.PRESET_OAK)
	var plain_sp := plain.make_species(false, 2) # Variant 1 carries ivy.
	var plain_src: Object = plain_sp.near_mesh_source
	for m in plain_sp.meshes:
		full_prints.append(print_of.call(m))
		full_leaves.append(leaves.call(m))
	plain_sp = null
	plain = null
	var g := TreeGenerator.new()
	g.seed = 1
	g.rigged = true
	g.near_leaf_range = 60.0
	g.set_preset(TreeGenerator.PRESET_OAK)
	var sp := g.make_species(false, 2)
	var src: Object = sp.near_mesh_source
	_check(plain_src == null and src != null and sp.near_range == 60.0, "near leaves: the split species has a near mesh source, the plain one none")
	if src == null:
		return
	for v in 2:
		var lf: int = leaves.call(sp.meshes[v])
		_check(lf < full_leaves[v], "near leaves: variant %d far leaves %.2fM vertices vs %.2fM" % [v, lf / 1e6, full_leaves[v] / 1e6])
		var t0 := Time.get_ticks_msec()
		var near: Mesh = null
		var calls := 0
		while near == null and Time.get_ticks_msec() - t0 < 120000:
			near = src.request_near_mesh(sp.near_mesh_key, v)
			calls += 1
			await get_tree().process_frame
		var same: bool = near != null and print_of.call(near) == full_prints[v]
		_check(same and calls > 1, "near leaves: variant %d's near mesh is the unsplit mesh, built in the background in %d ms" % [v, Time.get_ticks_msec() - t0])
		_check(src.request_near_mesh(sp.near_mesh_key, v) == near, "near leaves: a second cell gets the same mesh")
		near = null
		await get_tree().process_frame
	_check(src.get_near_mesh_count() == 0, "near leaves: meshes are freed once nothing draws them (%d left)" % src.get_near_mesh_count())
	# An ice storm's band (trees.md 5be): variant + 65536 x band grows that band's glaze as geometry on the near mesh.
	var t1 := Time.get_ticks_msec()
	var iced: Mesh = null
	while iced == null and Time.get_ticks_msec() - t1 < 180000:
		iced = src.request_near_mesh(sp.near_mesh_key, 0 + 65536 * 2)
		await get_tree().process_frame
	var has_ice := false
	if iced != null:
		for i in iced.get_surface_count():
			has_ice = has_ice or iced.surface_get_name(i) == "ice"
	_check(iced != null and has_ice and float(iced.get_meta("tree_glaze_mm", 0.0)) == 9.0, "near leaves: glaze band 2 builds a near mesh with 9 mm of ice as geometry")
	iced = null
	await get_tree().process_frame

# Wind level of detail (trees.md 3c): a camera at eye height on the stand's edge, looking across
# it. One TreeWind3D solves with the LOD, a second solves everything every frame; the pose
# difference of every bone, projected to pixels at that tree's distance, is the LOD's error.
func _wind_lod_check(frames: int, view: String = "eye") -> void:
	gen.rigged = true
	var biome := gen.make_stand(TreeGenerator.STAND_BEECH, variants)
	gen.rigged = false
	var f := _field(biome)
	var mmis := _rigged_instances(f)
	var cam := Camera3D.new()
	cam.fov = 60.0
	add_child(cam)
	var y := 1.7
	if view == "over":
		# Over the stand from outside it, as a high third-person or strategy camera sees a forest.
		cam.look_at_from_position(Vector3(-SIZE * 0.5 - 60.0, 45.0, -SIZE * 0.5 - 60.0), Vector3(0, 0, 0))
	else:
		cam.look_at_from_position(Vector3(-SIZE * 0.5 + 2.0, y, -10.0), Vector3(SIZE * 0.5, y + 6.0, 20.0))
	cam.make_current()
	var winds := []
	for lod in [true, false]:
		var w := TreeWind3D.new()
		w.fallback_wind = Vector2(12, 0)
		w.collision_enabled = false
		w.touch_enabled = false
		w.lod_enabled = lod
		if lod and OS.get_environment("LOD_PX") != "":
			w.lod_pixel_error = float(OS.get_environment("LOD_PX"))
		w.wind_range = 400.0
		add_child(w)
		w.target = w.get_path_to(f)
		winds.append(w)
	await get_tree().process_frame
	var vp_h := float(get_viewport().get_visible_rect().size.y)
	var px_per_rad := 0.5 * vp_h / tan(deg_to_rad(cam.fov * 0.5))
	var ms := [0.0, 0.0]
	var solved := [0, 0]
	var skipped := 0
	var worst := 0.0
	var errs := PackedFloat32Array()
	var dt := 1.0 / 60.0
	for frame in frames:
		var bufs := []
		for k in 2:
			var w: TreeWind3D = winds[k]
			w.set_time(5.0 + frame * dt)
			w.update(dt)
			ms[k] += w.get_solve_msec()
			solved[k] += w.get_solved_instances()
			if k == 0:
				skipped += w.get_skipped_instances()
			var b := []
			for mmi in mmis:
				b.append(RenderingServer.multimesh_get_buffer(mmi.multimesh.get_rid()))
			bufs.append(b)
		if frame < 10:
			continue # Every held instance has been solved once.
		for m in mmis.size():
			var mm: MultiMesh = mmis[m].multimesh
			var k: int = mm.bones_per_instance
			var h: float = mm.mesh.get_aabb().size.y
			var a: PackedFloat32Array = bufs[0][m]
			var c: PackedFloat32Array = bufs[1][m]
			var xf: Transform3D = mmis[m].global_transform
			for i in mm.instance_count / k:
				var o := i * k * 20
				var at := xf * Vector3(c[o + 3], c[o + 7], c[o + 11])
				var rel := cam.global_transform.affine_inverse() * at
				if rel.z > -0.5:
					continue # Behind the camera.
				if not cam.is_position_in_frustum(at + Vector3(0, h * 0.5, 0)) and not cam.is_position_in_frustum(at):
					continue
				var px_per_m := px_per_rad / at.distance_to(cam.global_position)
				var e := 0.0
				for b in k:
					var q := o + b * 20
					# Origin error plus the basis error over the tree's height.
					var d0 := Vector3(a[q + 3] - c[q + 3], a[q + 7] - c[q + 7], a[q + 11] - c[q + 11]).length()
					var d1 := 0.0
					for r in [0, 4, 8]:
						d1 = max(d1, Vector3(a[q + r] - c[q + r], a[q + r + 1] - c[q + r + 1], a[q + r + 2] - c[q + r + 2]).length())
					e = max(e, (d0 + d1 * h * 0.5) * px_per_m)
				errs.append(e)
				worst = max(worst, e)
	errs.sort()
	var p99 := errs[int(errs.size() * 0.99)] if errs.size() > 0 else 0.0
	var n := float(frames)
	print(view + " wind LOD: %.2f ms/frame (%.0f trees solved, %.0f held) vs %.2f ms full (%.0f trees); on-screen error p99 %.2f px, max %.2f px over %d tree frames" % [ms[0] / n, solved[0] / n, skipped / n, ms[1] / n, solved[1] / n, p99, worst, errs.size()])
	_check(ms[0] < ms[1], "wind LOD solves faster than full (%.2f vs %.2f ms)" % [ms[0] / n, ms[1] / n])
	_check(p99 < 1.0, "wind LOD error under 1 px for 99%% of trees in view (%.2f px)" % p99)
	for w in winds:
		w.queue_free()
	cam.queue_free()
	f.queue_free()

func _draw_maps(path: String) -> void:
	var px := 320
	var cols := 4
	var rows := (TreeGenerator.STAND_MAX + 2 + cols - 1) / cols
	var img := Image.create(px * cols, px * rows, false, Image.FORMAT_RGB8)
	img.fill(Color(0.93, 0.92, 0.88))
	for i in TreeGenerator.STAND_MAX + 2:
		var biome := _biome(i)
		var f := _field(biome)
		var ox := (i % cols) * px
		var oy := (i / cols) * px
		var k := (px - 8) / SIZE
		# Water.
		for y in px - 8:
			for x in px - 8:
				var wx := (x / k) - SIZE * 0.5
				if abs(wx) < STREAM_HALF:
					img.set_pixel(ox + 4 + x, oy + 4 + y, Color(0.55, 0.7, 0.85))
		var sps: Array = biome.species
		for s in sps.size():
			var sp: PlantSpecies = sps[s]
			var c: Color = COLORS[_preset_of(sp.resource_name)]
			if sp.resource_name.ends_with("(open)"):
				c = c.lightened(0.25)
			var r: float = max(sp.canopy_radius * k, 1.5)
			for p in f.get_placements(s):
				_disc(img, ox + 4 + (p.x + SIZE * 0.5) * k, oy + 4 + (p.z + SIZE * 0.5) * k, r, c, ox, oy, px)
		f.queue_free()
	img.save_png(path)
	print("saved ", path)

const PRESET_NAMES := ["Oak", "Beech", "Birch", "Scots Pine", "Norway Spruce", "Hazel", "Hawthorn", "Lime", "Sycamore Maple", "Ash",
		"Horse Chestnut", "Black Alder", "White Willow", "European Larch", "Silver Fir", "Rowan", "Elder", "Holly", "Yew", "Box",
		"Sugar Maple", "Red Maple", "Northern Red Oak", "White Oak", "Eastern White Pine", "Eastern Hemlock", "American Beech",
		"Paper Birch", "Yellow Birch", "Witch-hazel", "Mountain Laurel", "Gorse", "Broom", "Gum Rockrose", "Kermes Oak", "Lentisk", "Dwarf Fan Palm", "Stone Pine", "Cork Oak", "Holm Oak", "Wild Olive", "Maritime Pine", "Carob", "Blackthorn", "Field Maple", "Dog Rose"]

func _preset_of(nm: String) -> int:
	var base := nm.substr(0, nm.find(" ("))
	return max(PRESET_NAMES.find(base), 0)

func _disc(img: Image, cx: float, cy: float, r: float, c: Color, ox: int, oy: int, px: int) -> void:
	for y in range(int(cy - r), int(cy + r) + 1):
		for x in range(int(cx - r), int(cx + r) + 1):
			if x < ox + 2 or y < oy + 2 or x >= ox + px - 2 or y >= oy + px - 2:
				continue
			var d := Vector2(x - cx, y - cy).length()
			if d <= r:
				var under := img.get_pixel(x, y)
				img.set_pixel(x, y, under.lerp(c, 0.75 if d < r - 1.0 else 0.4))

func _render_plot(i: int, args: Dictionary) -> void:
	var raster: bool = args.has("raster")
	gen.rigged = args.has("wind")
	if args.has("day"):
		# --day=N: leaves by season, from main.gd's stand-in climate (winter: --day=20).
		gen.season = load("res://main.gd").weather_season(int(args["day"]))
	var biome := _biome(i)
	gen.rigged = false
	var f: VegetationField3D
	if args.has("far"):
		# --far=R: the far tail at a bench's scale (trees.md 9a): every species ranged R m, streamed in 20 m tiles out
		# to 3 R (VegetationField3D.far_reach), so the survivors past R / 2 thin and widen within the valley.
		for sp in biome.species:
			sp.visibility_range = float(args["far"])
			if OS.get_environment("NO_SKYLINE") != "":
				sp.set_meta("far_skyline", false) # Skyline trees thinned too (trees.md 9c).
			if sp.has_meta("far_hull_range"):
				# Crown hulls (trees.md 9b) past the same range; env HULL_AT overrides.
				sp.set_meta("far_hull_range", float(OS.get_environment("HULL_AT")) if OS.get_environment("HULL_AT") != "" else float(args["far"]))
		f = VegetationField3D.new()
		f.populate_on_ready = false
		f.tile_size = 20.0
		f.streaming = true
		f.stream_radius = SIZE * 1.5 if OS.get_environment("CAM") == "" else 3000.0
		f.far_reach = 3.0
		# Vegetation's draped far shell takes over past 0.6 of the reach; at a bench's ranges it would cover the valley.
		f.far_shell = OS.get_environment("FAR_SHELL") != ""
		f.far_volume = false # Its voxel shell from the range to 150 m, likewise.
		f.biome = biome
		add_child(f)
		f.data_map = f.get_path_to(dm)
	else:
		f = _field(biome)
	if args.has("wind"):
		# Posed by the wind at --time seconds (rigged meshes, one TreeWind3D on the field).
		var wind := TreeWind3D.new()
		wind.fallback_wind = Vector2(float(args.get("wind", "10")), 0)
		wind.active = false
		add_child(wind)
		wind.target = wind.get_path_to(f)
		await get_tree().process_frame
		wind.set_time(float(args.get("time", "2")))
		wind.update(0.0)
	print("%s: %d plants" % [STAND_NAMES[i], f.get_plant_count()])
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(SIZE, SIZE)
	pm.subdivide_width = 159
	pm.subdivide_depth = 159
	var st := SurfaceTool.new()
	st.create_from(pm, 0)
	var arr := st.commit_to_arrays()
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var valley: Valley = get_node("Valley")
	for v in verts.size():
		verts[v].y = valley.get_height(verts[v])
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_NORMAL] = null
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	st = SurfaceTool.new()
	st.create_from(am, 0)
	st.generate_normals()
	ground.mesh = st.commit()
	var gm := StandardMaterial3D.new()
	gm.albedo_color = Color(0.2, 0.19, 0.13)
	gm.roughness = 0.95
	ground.material_override = gm
	add_child(ground)
	var water := MeshInstance3D.new()
	var wp := PlaneMesh.new()
	wp.size = Vector2(STREAM_HALF * 2.6, SIZE)
	water.mesh = wp
	water.position.y = -0.3
	var wmat := StandardMaterial3D.new()
	wmat.albedo_color = Color(0.1, 0.15, 0.18)
	wmat.roughness = 0.05
	water.material_override = wmat
	add_child(water)
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = int(args.get("spp", "16"))
	env.pathtracing_max_bounces = 3
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-38, 30, 0)
	sun.light_energy = 1.4
	sun.shadow_enabled = OS.get_environment("NOSHADOW") == ""
	sun.directional_shadow_max_distance = 400.0
	add_child(sun)
	var cam := Camera3D.new()
	cam.fov = 45.0
	cam.far = 1000.0
	add_child(cam)
	cam.position = Vector3(-30, 45, SIZE * 0.62)
	cam.look_at(Vector3(5, 4, 0))
	if args.has("inside"):
		# Standing in the stand: a short view (and shadow) distance keeps a CPU render bounded.
		cam.far = 70.0
		sun.directional_shadow_max_distance = 70.0
		var eye := Vector3(0, 0, 45) # on the stream bed, the one gap in the stand
		var look := Vector3(-0.35, 0.18, -1)
		if args.has("deep"):
			# Among the canopy trees, away from the stream, looking into the closed stand.
			eye = Vector3(-40, 0, 30)
			look = Vector3(-1, 0.22, -0.6)
		eye.y = valley.get_height(eye) + 1.7
		cam.position = eye
		cam.look_at(eye + look)
	if args.has("edge"):
		# --edge: from 1.7 m in a coupe cut 0-1 years ago, across its edge into one of 8+ years
		# (PlantSpecies.get_coupe_at of the stand's first coupe species).
		var csp: PlantSpecies = null
		for sp in biome.species:
			if sp.get_coupe_rotation() > 0 and String(sp.resource_name).begins_with("Hazel"):
				csp = sp
				break
		var found := false
		for need_std in [true, false]:
			if csp != null and not found:
				for r in range(0, 60, 4):
					for a in 32:
						var p := Vector3(r * cos(a * PI / 16), 0, r * sin(a * PI / 16))
						var c: Vector3 = csp.get_coupe_at(p)
						if c.x > 1 or c.z > 0.5 or c.y < 3.0 or c.y > 8.0:
							continue
						for k in 16:
							var d := Vector3(cos(k * PI / 8), 0, sin(k * PI / 8))
							var e := p - d * 8.0
							var q: Vector3 = csp.get_coupe_at(p + d * (c.y + 6.0))
							var ce: Vector3 = csp.get_coupe_at(e)
							if q.x >= 8 and q.z < 0.5 and ce.x <= 1 and ce.z < 0.5 and not found:
								# A standard (the stand's first species, oak) 5-12 m out and in view.
								var std_ok := false
								for sp_pos in f.get_placements(0):
									var o := Vector3(sp_pos.x, 0, sp_pos.z) - e
									if o.length() > 4.0 and o.length() < 15.0 and o.normalized().dot(d) > 0.8:
										std_ok = true
								if need_std and not std_ok:
									continue
								found = true
								e.y = valley.get_height(e) + 1.7
								cam.position = e
								var tgt := p + d * (c.y + 6.0)
								tgt.y = valley.get_height(tgt) + 2.5
								cam.look_at(tgt)
								cam.fov = 55.0
								cam.far = 120.0
								print("edge view from ", e, " to ", tgt, " ages ", c.x, " -> ", q.x)
		if args.has("standard") and csp != null:
			# --edge --standard: an oak standard 8 m off in a young coupe, an older one behind it.
			var best := -1.0
			var stds := PackedVector3Array()
			for si in 3:
				stds.append_array(f.get_placements(si))
			for sp_pos in stds:
				for k in 16:
					var d := Vector3(cos(k * PI / 8), 0, sin(k * PI / 8))
					var e: Vector3 = Vector3(sp_pos.x, 0, sp_pos.z) - d * float(OS.get_environment("SD") if OS.get_environment("SD") != "" else "8")
					var ce: Vector3 = csp.get_coupe_at(e)
					var cb: Vector3 = csp.get_coupe_at(Vector3(sp_pos.x, 0, sp_pos.z) + d * 12.0)
					if ce.x > 7 or ce.z > 0.5 or absf(e.x) > 75 or absf(e.z) > 75:
						continue
					var score := cb.x - ce.x - (0.0 if ce.x <= 3 else 5.0)
					if score > best:
						best = score
						e.y = valley.get_height(e) + 1.7
						cam.position = e
						cam.look_at(Vector3(sp_pos.x, valley.get_height(sp_pos) + float(OS.get_environment("SH") if OS.get_environment("SH") != "" else "4"), sp_pos.z))
						cam.fov = 60.0
						cam.far = 120.0
						found = true
			print("standard view from ", cam.position, " score ", best)
		if not found:
			print("no coupe edge found")
		for si in biome.species.size():
			for pp in f.get_placements(si):
				var o: Vector3 = pp - cam.position
				o.y = 0
				var nm: String = biome.species[si].resource_name
				if o.length() < 25.0 and not nm.begins_with("Hazel") and not nm.contains("sticks") and o.normalized().dot(-cam.global_transform.basis.z * Vector3(1, 0, 1)) > 0.6:
					print("in view %.1f m: %s" % [o.length(), biome.species[si].resource_name])
	if args.has("near"):
		# --near=D: the first species' (env NEAR_SPECIES) plant nearest the plot's centre, from D metres (default 7),
		# eye at 1.7 m, aimed at 1.6 m on it (env SH sets the aim height).
		var best_p := Vector3()
		var best_d := 1e9
		# env NEAR_SPECIES=k: the k-th species instead of the first.
		for pp in f.get_placements(int(OS.get_environment("NEAR_SPECIES")) if OS.get_environment("NEAR_SPECIES") != "" else 0):
			var dd := Vector2(pp.x, pp.z).length()
			if dd < best_d:
				best_d = dd
				best_p = pp
		var dist := float(args.get("near", "7"))
		var e := best_p + Vector3(-0.6, 0, 0.8).normalized() * dist
		e.y = valley.get_height(e) + 1.7
		cam.position = e
		cam.look_at(Vector3(best_p.x, valley.get_height(best_p) + float(OS.get_environment("SH") if OS.get_environment("SH") != "" else "1.6"), best_p.z))
		cam.fov = 55.0
		cam.far = 150.0
	var cv := OS.get_environment("CAM").split(",")
	if cv.size() >= 6:
		# CAM="x,y,z,lx,ly,lz[,fov]" overrides the view (eye height over the ground at x, z).
		cam.position = Vector3(float(cv[0]), float(cv[1]), float(cv[2]))
		cam.position.y += valley.get_height(cam.position)
		cam.look_at(Vector3(float(cv[3]), float(cv[4]), float(cv[5])))
		if cv.size() >= 7:
			cam.fov = float(cv[6])
		# Far views keep sun shadows over the whole stand.
		var dist := cam.position.distance_to(Vector3(float(cv[3]), float(cv[4]), float(cv[5])))
		sun.directional_shadow_max_distance = maxf(sun.directional_shadow_max_distance, dist + SIZE)
		cam.far = maxf(cam.far, dist + SIZE * 2.0)
	if args.has("far"):
		f.stream_target = f.get_path_to(cam)
		var waited := 0
		get_viewport().disable_3d = true # Streams without drawing every step (lavapipe).
		while waited < 3000 and (waited < 10 or not f.is_stream_idle()):
			await RenderingServer.frame_post_draw
			waited += 1
			if waited % 50 == 0:
				print("far: frame %d, %d plants, idle %s" % [waited, f.get_plant_count(), str(f.is_stream_idle())])
		get_viewport().disable_3d = false
		var drawn := 0
		var hulls := 0
		var hull_quads := 0
		for mmi in f.find_children("*", "MultiMeshInstance3D", true, false):
			if mmi.multimesh != null:
				var n: int = mmi.multimesh.visible_instance_count if mmi.multimesh.visible_instance_count >= 0 else mmi.multimesh.instance_count
				if String(mmi.name).begins_with("PlantsHull"):
					hulls += n
					hull_quads = mmi.multimesh.mesh.surface_get_array_index_len(0) / 3
					if OS.get_environment("HULL_LOG") != "":
						var hc: PackedColorArray = mmi.multimesh.mesh.surface_get_arrays(0)[Mesh.ARRAY_COLOR]
						print("hull %s: %d, %d triangles, cover %.2f, first lobe %s" % [mmi.name, n, hull_quads, float(mmi.multimesh.mesh.get_meta("hull_cover", -1.0)), str(hc[0].linear_to_srgb()) if hc.size() > 0 else "-"])
				else:
					drawn += n
		print("far: %d plants placed, %d drawn instances, %d hull instances (%d triangles in the last), after %d frames" % [f.get_plant_count(), drawn, hulls, hull_quads, waited])
	for k in (8 if raster else 12):
		await RenderingServer.frame_post_draw
	if not raster:
		# Path traced: surfaces whose vertex() is captured join once captured (RT_PENDING_SURFACES).
		var pw := 0
		while pw < 300 and get_viewport().get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_RT_PENDING_SURFACES) > 0:
			await RenderingServer.frame_post_draw
			pw += 1
		for k in 12:
			await RenderingServer.frame_post_draw
		print("pt: waited %d draws for pending surfaces" % pw)
	var out: String = args.get("out", "/tmp/plot.png")
	get_viewport().get_texture().get_image().save_png(out)
	print("saved ", out)
	# Near meshes still building (one at a time over all species): let them end before the exit frees
	# their sources (a build cut off by the exit hung it).
	while TreeGenerator.get_near_builds() > 0:
		await RenderingServer.frame_post_draw
	get_tree().quit()


# --- Region: stands as belts over a generated range ------------------------------------------------

const STAND_COLORS := [Color("3f8f4f"), Color("b5a642"), Color("1f4d3b"), Color("7a6aa8"), Color("c0703a"), Color("3b6e8f"), Color("888888"),
		Color("c86f2e"), Color("8a5a2b"), Color("234d45")]

class Mountain extends Node3D:
	# A massif with forest-able slopes (at most about 33 degrees) and a valley floor: the climate
	# belts without the generator's bare rock walls.
	func get_height(p: Vector3) -> float:
		var r := Vector2(p.x - 400.0, p.z + 300.0).length()
		var ridge := 2500.0 * exp(-pow(r / 2100.0, 2.0))
		return ridge + 60.0 * sin(p.x * 0.004) * cos(p.z * 0.003) + 40.0

func _region(path: String) -> void:
	var WORLD := 4096.0 if use_generator else 6000.0
	var tg: Node3D
	var t := Time.get_ticks_msec()
	if use_generator:
		# The terrain bench's range (docs/bench/terrain_gen), mountain view settings.
		var range_path := Path3D.new()
		var curve := Curve3D.new()
		curve.add_point(Vector3(-1900, 0, 700), Vector3.ZERO, Vector3(600, 0, -200))
		curve.add_point(Vector3(-300, 0, 100), Vector3(-500, 0, 100), Vector3(500, 0, -100))
		curve.add_point(Vector3(1900, 0, -600), Vector3(-600, 0, 0))
		range_path.curve = curve
		add_child(range_path)
		var noise := FastNoiseLite.new()
		noise.seed = 7
		noise.frequency = 1.0 / 1500.0
		noise.fractal_octaves = 3
		var g3 := TerrainGenerator3D.new()
		# Heavy generator options are on by default; this bench keeps its own fast choices.
		g3.glacial_enabled = false
		g3.glacial_converged = false
		g3.tile_erosion_enabled = false
		g3.strata_enabled = false
		g3.guide_preview = false
		g3.dunes_enabled = false
		g3.deposition = 0.0
		g3.moraine_age = 0.0
		g3.scree_age = 0.0
		g3.meander_age = 0.0
		g3.outwash_age = 0.0
		g3.size = Vector2(WORLD, WORLD)
		g3.cell_size = 16.0
		g3.seed = 1
		g3.uplift_noise = noise
		g3.uplift = 6.0
		g3.range_width = 1300.0
		add_child(g3)
		g3.ranges = [g3.get_path_to(range_path)]
		g3.generate()
		tg = g3
	else:
		tg = Mountain.new()
		add_child(tg)
	var map := TerrainDataMap3D.new()
	map.bake_on_ready = false
	map.size = WORLD
	map.resolution = 600
	add_child(map)
	map.height_source = map.get_path_to(tg)
	if use_generator:
		map.geology_source = map.get_path_to(tg)
	# A cool temperate climate (Central European uplands to Alps): the belts from beech and oak
	# in the valleys through spruce and fir to larch at the tree line.
	var recipe := ClimateRecipe.new()
	recipe.temperature_january = 0.0
	recipe.temperature_july = 19.0
	recipe.reference_height = 0.0
	recipe.precipitation = 700.0
	map.climate_recipe = recipe
	_check(map.bake() == OK, "region data map bakes")
	_check(map.bake_climate() == OK, "region climate bakes")
	t = Time.get_ticks_msec()
	var biome := gen.make_woodland(0, TreeGenerator.REGION_NORTH_AMERICA if north_america else TreeGenerator.REGION_EUROPE)
	print("woodland species (no meshes) %d ms" % (Time.get_ticks_msec() - t))
	var f := VegetationField3D.new()
	f.populate_on_ready = false
	f.tile_size = 128.0
	f.biome = biome
	add_child(f)
	f.data_map = f.get_path_to(map)
	t = Time.get_ticks_msec()
	f.populate()
	print("woodland: %d plants placed in %d ms" % [f.get_plant_count(), Time.get_ticks_msec() - t])
	# Which stand each species came from, by name (make_woodland keeps make_stand's species objects).
	var stand_of := {}
	for st in TreeGenerator.STAND_MAX:
		if st == TreeGenerator.STAND_HEDGEROW:
			continue
		for sp in gen.make_stand(st, 0).species:
			stand_of[sp.resource_name + str(st)] = st
	# Hillshade with the stands over it.
	var res := 1024
	var img := Image.create(res, res, false, Image.FORMAT_RGB8)
	var k := res / WORLD
	var hmin := INF
	var hmax := -INF
	for z in res:
		for x in res:
			var p := Vector3((x + 0.5) / k - WORLD * 0.5, 0, (z + 0.5) / k - WORLD * 0.5)
			var h := map.get_height_at(p)
			hmin = min(hmin, h)
			hmax = max(hmax, h)
			var hx := map.get_height_at(p + Vector3(4, 0, 0)) - h
			var hz := map.get_height_at(p + Vector3(0, 0, 4)) - h
			var n := Vector3(-hx, 4.0, -hz).normalized()
			var shade: float = clamp(n.dot(Vector3(-0.5, 0.7, -0.5).normalized()), 0.0, 1.0)
			var base := Color(0.86, 0.84, 0.78)
			img.set_pixel(x, z, base * (0.8 + 0.2 * shade))
	print("height %.0f to %.0f m" % [hmin, hmax])
	for hh in ([] if use_generator else [100.0, 600.0, 1100.0, 1600.0, 2100.0, 2450.0]):
		var q := Vector3(400.0 + 2100.0 * sqrt(-log((hh - 40.0) / 2500.0)), 0, -300.0)
		print("  at %.0f m: T %.1f C, January %.1f C, P %.0f mm" % [map.get_height_at(q), map.get_temperature_at(q), map.get_temperature_at(q, 15.0), map.get_precipitation_at(q)])
	var sps: Array = biome.species
	var per_stand := []
	per_stand.resize(TreeGenerator.STAND_MAX)
	per_stand.fill(0)
	for s in sps.size():
		var sp: PlantSpecies = sps[s]
		var st := _stand_of_species(sp)
		var c: Color = STAND_COLORS[st]
		if sp.resource_name.ends_with("(open)"):
			c = c.lightened(0.35)
		var pts := f.get_placements(s)
		per_stand[st] += pts.size()
		for p in pts:
			var x := int((p.x + WORLD * 0.5) * k)
			var z := int((p.z + WORLD * 0.5) * k)
			for dz in 2:
				for dx in 2:
					if x + dx >= 0 and z + dz >= 0 and x + dx < res and z + dz < res:
						img.set_pixel(x + dx, z + dz, img.get_pixel(x + dx, z + dz).lerp(c, 0.7))
	for st in TreeGenerator.STAND_MAX:
		print("  %-16s %7d plants" % [STAND_NAMES[st], per_stand[st]])
	# Belts in order of height: the mean height of each stand's plants.
	var mean_h := {}
	for s in sps.size():
		var st := _stand_of_species(sps[s])
		for p in f.get_placements(s):
			mean_h[st] = mean_h.get(st, Vector2.ZERO) + Vector2(map.get_height_at(p), 1)
	for st in mean_h:
		print("  %-16s mean height %.0f m" % [STAND_NAMES[st], mean_h[st].x / mean_h[st].y])
	if mean_h.has(TreeGenerator.STAND_SUBALPINE) and mean_h.has(TreeGenerator.STAND_MONTANE_CONIFER):
		var sa: Vector2 = mean_h[TreeGenerator.STAND_SUBALPINE]
		var mo: Vector2 = mean_h[TreeGenerator.STAND_MONTANE_CONIFER]
		_check(sa.x / sa.y > mo.x / mo.y, "subalpine belt above the montane belt")
	# The tree line by the growing season's mean (Paulsen and Korner 2014), the alpine plants' rule:
	# upright trees fade out by 6.4 C, krummholz mats reach to 5.2 C.
	var bare := 0
	var upright_above := 0
	for s in sps.size():
		var mat: bool = sps[s].resource_name.ends_with("(krummholz)")
		for p in f.get_placements(s):
			var season := PlantSpecies.season_temperature(map.get_temperature_at(p, 15.0), map.get_temperature_at(p, 15.0 + 365.0 * 0.5))
			if season < 5.2:
				bare += 1
			elif season < 6.4 and not mat:
				upright_above += 1
	_check(bare == 0, "no trees above the tree species line, season mean 5.2 C (%d)" % bare)
	_check(upright_above == 0, "only krummholz mats above the tree line, season mean 6.4 C (%d)" % upright_above)
	img.save_png(path)
	print("saved ", path)

# Species names carry no stand: find it from the species order make_woodland() uses.
var _species_stand := {}
func _stand_of_species(sp: PlantSpecies) -> int:
	if _species_stand.has(sp):
		return _species_stand[sp]
	var best := 0
	# make_woodland() builds stands in this order; match by the patch seed each stand sets, or by
	# the stand's distinctive rules.
	var order := [TreeGenerator.STAND_RIPARIAN, TreeGenerator.STAND_SUBALPINE, TreeGenerator.STAND_MONTANE_CONIFER, TreeGenerator.STAND_BEECH, TreeGenerator.STAND_OAK_MIXED, TreeGenerator.STAND_PINE_HEATH]
	best = sp.get_meta("stand", 0)
	_species_stand[sp] = best
	return best

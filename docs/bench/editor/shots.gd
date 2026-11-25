@tool
# Editor screenshots of the level-design tools (docs/ideas/editor-tools.md), for the site.
# Opens world.tscn in the editor, sets one tool up as a designer would and saves the editor window.
# xvfb-run -a -s "-screen 0 1920x1080x24" godot --path docs/bench/editor --editor world.tscn -- --shot=preview
# Shots: preview, paint, keepout, sculpt. Writes shots/<name>.png and quits.
extends Node

var args := {}
var tree_gen: TreeGenerator # kept alive: the species build their near leaves from it


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	if not Engine.is_editor_hint() or not args.has("shot"):
		return
	_run.call_deferred(args["shot"])


func _ei():
	return Engine.get_singleton("EditorInterface")


func _wait(s: float) -> void:
	# Frames, not a SceneTree timer: those do not run in the editor.
	var end := Time.get_ticks_msec() + int(s * 1000.0)
	while Time.get_ticks_msec() < end:
		await get_tree().process_frame


func _select(n: Node) -> void:
	var sel = _ei().get_selection()
	sel.clear()
	sel.add_node(n)
	_ei().edit_node(n)


# The editor's own controls, found by class and text (the paint tool's toolbar).
func _find(cls: String, text := "") -> Control:
	for c in _ei().get_base_control().find_children("*", cls, true, false):
		if c.is_visible_in_tree() and (text == "" or c.get("text") == text):
			return c
	return null


func _choose(opt: OptionButton, text: String) -> void:
	for i in opt.item_count:
		if opt.get_item_text(i) == text:
			opt.select(i)
			opt.item_selected.emit(i)
			return
	push_error("no option " + text)


func _spin_after(label: String, value: float) -> void:
	var l := _find("Label", label)
	if l == null:
		return
	var p := l.get_parent()
	var s = p.get_child(l.get_index() + 1)
	if s is SpinBox:
		s.allow_greater = true
		s.max_value = maxf(s.max_value, value)
		s.value = value


# A left-button drag across the editor's 3D view, in its own pixels (0..1 of its size).
func _stroke(from: Vector2, to: Vector2, steps := 24) -> void:
	var vp = _ei().get_editor_viewport_3d(0)
	var container := vp.get_parent() as Control
	var r := container.get_global_rect()
	var a := r.position + r.size * from
	var b := r.position + r.size * to
	var mm := InputEventMouseMotion.new()
	mm.position = a
	mm.global_position = a
	Input.parse_input_event(mm)
	await get_tree().process_frame
	var mb := InputEventMouseButton.new()
	mb.button_index = MOUSE_BUTTON_LEFT
	mb.pressed = true
	mb.button_mask = MOUSE_BUTTON_MASK_LEFT
	mb.position = a
	mb.global_position = a
	Input.parse_input_event(mb)
	for i in steps + 1:
		await get_tree().process_frame
		var m := InputEventMouseMotion.new()
		m.position = a.lerp(b, float(i) / steps)
		m.global_position = m.position
		m.button_mask = MOUSE_BUTTON_MASK_LEFT
		m.relative = (b - a) / steps
		Input.parse_input_event(m)
	await get_tree().process_frame
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.pressed = false
	up.position = b
	up.global_position = b
	Input.parse_input_event(up)
	await get_tree().process_frame
	# Leave the cursor where the brush shows, mid-stroke.
	var rest := InputEventMouseMotion.new()
	rest.position = a.lerp(b, 0.5)
	rest.global_position = rest.position
	Input.parse_input_event(rest)


func _ready_world(w: Node) -> bool:
	var veg = w.get_node_or_null("Vegetation")
	var land = w.get_node_or_null("Landscape")
	return veg != null and land != null and w.get_node("Generator").has_result() and int(land.get_stats().get("chunks", 0)) > 0


func _run(shot: String) -> void:
	print("SHOT start ", shot)
	var w: Node = null
	for i in 600:
		w = _ei().get_edited_scene_root()
		if w and _ready_world(w):
			break
		await get_tree().process_frame
	# The editor finishes loading its layout after the scene opens (a progress dialog runs the frames meanwhile).
	var quiet := 0
	while quiet < 30:
		var busy := false
		for d in get_tree().root.find_children("*", "ProgressDialog", true, false):
			busy = busy or d.visible
		quiet = 0 if busy else quiet + 1
		await get_tree().process_frame
	print("SHOT world ready: ", w != null and _ready_world(w), " fps ", Engine.get_frames_per_second())
	await _wait(float(args.get("settle", "6")))
	var veg = w.get_node("Vegetation")
	if shot == "probe":
		_probe(w)
		get_tree().quit()
		return
	if args.get("dress", "1") == "1":
		await _dress(w)
	match shot:
		"preview":
			_select(veg)
		"paint":
			_select(veg)
			await _wait(0.5)
			var b := _find("Button", "Paint Mask")
			b.button_pressed = true
			await _wait(0.3)
			_choose(_slot_option(), "density_mask")
			_spin_after("Value", 0.0)
			_spin_after("Radius", 6.0)
			_spin_after("Strength", 1.0)
			await _wait(0.3)
			await _stroke(Vector2(0.3, 0.62), Vector2(0.7, 0.55))
		"keepout":
			var ex := WorldExclusion3D.new()
			ex.name = "KeepOut"
			ex.size = Vector2(24, 24)
			ex.fade = 2.0
			w.add_child(ex)
			ex.owner = w
			var p: Vector3 = _focus()
			ex.global_position = Vector3(p.x, w.get_node("Generator").get_height(p), p.z)
			_select(ex)
		"sculpt":
			var gen = w.get_node("Generator")
			var layer := TerrainSculptLayer3D.new()
			layer.name = "Sculpt"
			gen.add_child(layer)
			layer.owner = w
			_select(layer)
			await _wait(0.5)
			_find("Button", "Paint Mask").button_pressed = true
			await _wait(0.3)
			_choose(_operation_option(), "Add")
			_spin_after("Value", float(args.get("raise", "4")))
			_spin_after("Radius", 8.0)
			_spin_after("Strength", 1.0)
			await _wait(0.3)
			for k in int(args.get("passes", "3")):
				await _stroke(Vector2(0.25, 0.6), Vector2(0.75, 0.5))
	await _wait(float(args.get("after", "6")))
	var img := get_viewport().get_texture().get_image()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://shots"))
	img.save_png("res://shots/%s.png" % shot)
	print("SHOT ", shot, " ", img.get_size())
	get_tree().quit()


func _focus() -> Vector3:
	return Vector3(float(args.get("fx", "0")), 0, float(args.get("fz", "0")))


func _slot_option() -> OptionButton:
	var pb := _find("Button", "Paint Mask")
	return pb.get_parent().get_child(pb.get_index() + 1) as OptionButton


func _operation_option() -> OptionButton:
	var pb := _find("Button", "Paint Mask")
	return pb.get_parent().get_child(pb.get_index() + 3) as OptionButton


# Heights and plants round the middle, to choose the views (written to the editor's view state by shots.sh).
func _probe(w: Node) -> void:
	var gen = w.get_node("Generator")
	var veg = w.get_node("Vegetation")
	for z in range(-300, 301, 60):
		var row := ""
		for x in range(-300, 301, 60):
			var n := 0
			for i in 2:
				for p in veg.get_placements(i):
					if absf(p.x - x) < 30 and absf(p.z - z) < 30:
						n += 1
			row += "%6.0f/%-3d" % [gen.get_height(Vector3(x, 0, z)), n]
		print("PROBE z=%4d %s" % [z, row])


# What a level has and the bench leaves out: real trees (TreeGenerator), the CC0 ground, floor and stone scans
# (docs/bench/landscape/fetch_art.sh), a sun and a sky. Set at run time, never saved.
func _dress(w: Node) -> void:
	var art := ProjectSettings.globalize_path("res://art")
	var veg = w.get_node("Vegetation")
	if DirAccess.dir_exists_absolute(art + "/trees_scans"):
		TreeGenerator.load_scan_set(art + "/trees_scans")
	# A still, as the landscape bench takes them: static trees, light far meshes, near leaves built one at a time.
	print("SHOT trees")
	tree_gen = TreeGenerator.new()
	tree_gen.preset = TreeGenerator.PRESET_BEECH
	tree_gen.seed = 11
	tree_gen.rigged = false
	tree_gen.light_far_meshes = true
	tree_gen.set_near_build_limit(1)
	tree_gen.near_leaf_range = 25.0
	var trees: PlantSpecies = tree_gen.make_species(true, 2)
	var shrubs: PlantSpecies = TreeGenerator.make_shrub_species(TreeGenerator.PRESET_HAZEL, TreeGenerator.SHRUB_MATURE, 2)
	veg.biome.species = [shrubs] if args.get("trees", "1") == "0" else [trees, shrubs]
	print("SHOT species ready")
	if DirAccess.dir_exists_absolute(art + "/terrain_layers"):
		var land = w.get_node("Landscape")
		print("SHOT ground scans ", land.material.load_scan_set(art + "/terrain_layers", PackedStringArray(), ""))
		print("SHOT floor scans ", ClassDB.class_call_static("VegetationField3D", "load_floor_scans", art + "/terrain_layers"))
		print("SHOT stone scans ", w.get_node("Rocks").load_scan_set(art + "/terrain_layers"))
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-38, 140, 0)
	w.add_child(sun)
	var env := WorldEnvironment.new()
	env.name = "Sky"
	var e := Environment.new()
	e.background_mode = Environment.BG_SKY
	e.sky = Sky.new()
	e.sky.sky_material = ProceduralSkyMaterial.new()
	e.tonemap_mode = Environment.TONE_MAPPER_AGX
	e.ssao_enabled = true
	env.environment = e
	w.add_child(env)
	veg.notify_density_mask_changed(Rect2()) # every tile again, with the new species
	print("SHOT dressed")
	await _wait(float(args.get("dress_wait", "20")))

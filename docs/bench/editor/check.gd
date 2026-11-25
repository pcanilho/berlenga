@tool
# Checks that world.tscn sets itself up with no script: in the editor (editor preview) and in the game.
# godot --headless --path docs/bench/editor --editor world.tscn -- --check   (editor)
# godot --headless --path docs/bench/editor -- --check                       (game)
extends Node

const AUTHORED := 8 # World, its six system nodes and the camera, as make_world.gd saves them

var args := {}
var t := 0.0
var done := false
var fails := 0
var editor := false


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	editor = Engine.is_editor_hint()
	set_process(args.has("check"))


func _root() -> Node:
	if editor:
		var ei = Engine.get_singleton("EditorInterface")
		return ei.get_edited_scene_root() if ei else null
	return get_tree().current_scene


func _internal(n: Node, cls := "") -> int:
	var c := 0
	for i in n.get_child_count(true):
		var ch := n.get_child(i, true)
		if cls == "" or ch.is_class(cls):
			c += 1
		c += _internal(ch, cls) if cls != "" else 0
	return c


func check(ok: bool, what: String) -> void:
	print("%s: %s" % ["ok" if ok else "FAIL", what])
	if not ok:
		fails += 1


func _ready_state(w: Node) -> Dictionary:
	var gen = w.get_node_or_null("Generator")
	var land = w.get_node_or_null("Landscape")
	var dm = w.get_node_or_null("DataMap")
	var veg = w.get_node_or_null("Vegetation")
	var grass = w.get_node_or_null("Grass")
	var rocks = w.get_node_or_null("Rocks")
	return {
		"generated": gen != null and gen.has_result(),
		"water": gen != null and _internal(gen, "Node3D") > 0,
		"landscape": land != null and int(land.get_stats().get("chunks", 0)) > 0,
		"material": land != null and land.material != null and land.material.get_generator() == gen,
		"data_map": dm != null and dm.is_baked(),
		"plants": veg != null and _internal(veg, "MultiMeshInstance3D") > 0,
		"grass": grass != null and _internal(grass) > 0,
		"rocks": rocks != null and rocks.get_stone_count() > 0,
	}


func _process(delta: float) -> void:
	if done:
		return
	t += delta
	var w := _root()
	if w == null:
		return
	var st := _ready_state(w)
	if int(t / 10.0) != int((t - delta) / 10.0):
		print("waiting %.0f s: %s" % [t, st])
	var all := true
	for k in st:
		all = all and st[k]
	if not all and t < float(args.get("timeout", "600")):
		return
	done = true
	print("%s after %.1f s: %s" % ["editor" if editor else "game", t, st])
	for k in st:
		check(st[k], "%s set up with no script (%s)" % [k, "editor preview" if editor else "game"])
	if editor:
		# Nothing generated is saved: the packed scene holds the authored nodes only.
		var ps := PackedScene.new()
		ps.pack(w)
		var n := ps.get_state().get_node_count()
		check(n == AUTHORED, "saving keeps only the authored nodes (%d of %d)" % [n, AUTHORED])
	print("CHECKS %s (%d failed)" % ["PASSED" if fails == 0 else "FAILED", fails])
	get_tree().quit(1 if fails else 0)

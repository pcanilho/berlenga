# A stand-in for the Terrain3D addon's node (never bundled): records the control-map holes RockField3D writes through
# the same calls (get_data().set_control_hole, update_maps, get_collision().update), for the test.
extends Node3D

var vertex_spacing := 1.0
var holes := 0
var map_updates := []
var collision_updates := 0


class Data:
	extends RefCounted
	var owner_node: Node
	func set_control_hole(_global_position: Vector3, enable: bool) -> void:
		if enable:
			owner_node.holes += 1
	func update_maps(map_type: int = 3, _all_regions: bool = true, _mipmaps: bool = false) -> void:
		owner_node.map_updates.append(map_type)


class Collision:
	extends RefCounted
	var owner_node: Node
	func update(_region := Vector2i(2147483647, 2147483647), _rebuild := false) -> void:
		owner_node.collision_updates += 1


var _data := Data.new()
var _collision := Collision.new()


func _init() -> void:
	_data.owner_node = self
	_collision.owner_node = self


func get_data() -> Object:
	return _data


func get_collision() -> Object:
	return _collision

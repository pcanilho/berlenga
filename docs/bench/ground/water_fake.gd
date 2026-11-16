extends Node3D
# A stand-in for the water module in --test=water: water of a given depth and flow over a rectangle, read
# through the same calls GroundSurface3D makes on a river, a lake or live water.

var rect := Rect2()
var depth := 0.0
var flow := Vector3.ZERO


func sample_depth(p: Vector3) -> float:
	return depth if rect.has_point(Vector2(p.x, p.z)) else -1.0


func sample_flow(p: Vector3) -> Vector3:
	return flow if rect.has_point(Vector2(p.x, p.z)) and depth > 0.0 else Vector3.ZERO

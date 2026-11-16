extends Node3D
# A stand-in for the vegetation module in --test=shelter: a round tuft of grass of frontal area index lambda,
# read through the call GroundSurface3D makes on VegetationField3D.

var center := Vector2.ZERO
var radius := 1.0
var lambda := 0.3


func sample_lateral_cover(p: Vector3) -> float:
	return lambda if Vector2(p.x, p.z).distance_to(center) <= radius else 0.0

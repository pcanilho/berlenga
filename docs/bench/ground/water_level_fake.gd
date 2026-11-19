extends Node3D
# A stand-in for still water in --test=fringe: a pool whose surface lies at a fixed level over the ground below it,
# read through the same call GroundSurface3D makes on a lake (its depth at a point on the ground).

var level := 0.0


func sample_depth(p: Vector3) -> float:
	return level - p.y

extends Node3D
# A stand-in for the sea in --test=runoff and --view=rills: still water up to a level that the test lowers, as a
# falling tide leaves a soaked beach face behind it.

var level := 0.0


func sample_depth(p: Vector3) -> float:
	return level - p.y

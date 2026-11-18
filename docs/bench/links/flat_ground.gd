extends Node3D
# A flat ground at y = 0 for snow (SnowCover3D.height_source).

func get_height(_p: Vector3) -> float:
	return 0.0

extends WorldGround
# A slope falling toward +Z (30%) for the storm bench's avalanche check.
func _height_at(p: Vector3) -> float:
	return -0.3 * p.z

extends Node3D
# A height source for GroundSurface3D (any node with get_height(position)).

var kind := "flat"
var noise := FastNoiseLite.new()


func _init() -> void:
	noise.seed = 11
	noise.frequency = 0.02
	noise.fractal_octaves = 3


func get_height(p: Vector3) -> float:
	match kind:
		"slope":
			# A 50 degree face rising toward +x, steeper than any repose angle here.
			return p.x * tan(deg_to_rad(50.0))
		"meadow":
			return 0.12 * noise.get_noise_2d(p.x * 3.0, p.z * 3.0) + 0.02 * p.x
		_:
			return 0.0

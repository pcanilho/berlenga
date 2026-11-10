extends Node3D
# A height source for SnowCover3D (any node with get_height(position)): a mountain with a cliff band
# and a valley, or a plain with a 60 degree wall for the slide checks.

var kind := "mountain"
var noise := FastNoiseLite.new()


func _init() -> void:
	noise.seed = 7
	noise.frequency = 0.004
	noise.fractal_octaves = 5


func get_height(p: Vector3) -> float:
	match kind:
		"flat":
			return 0.0
		"wall":
			# Flat ground, a 60 degree face 40 m high from x = 0 to x = 23 m, a plateau beyond.
			return clampf(p.x * tan(deg_to_rad(60.0)), 0.0, 40.0)
		"ridge":
			# A 100 m ridge along z, 25 degree flanks.
			return maxf(0.0, 100.0 - absf(p.x) * tan(deg_to_rad(25.0)))
		_:
			var r := Vector2(p.x, p.z).length()
			var h := 700.0 * exp(-pow(r / 900.0, 2.0))
			h += 50.0 * noise.get_noise_2d(p.x, p.z)
			return h

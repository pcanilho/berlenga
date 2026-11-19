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
		"bank":
			# A 15 degree bank 4 m wide rising toward +x, flat above and below (mud round 10: mud slides).
			return clampf(p.x, -2.0, 2.0) * tan(deg_to_rad(15.0))
		"incline":
			# A plain 6 degree slope falling toward -x, flat below x = -5 (mud round 11: gullies in ruts).
			return maxf(p.x, -5.0) * tan(deg_to_rad(6.0))
		"beach":
			# A foreshore rising 1 in 12 toward +z out of still water at z = 0, with a little relief.
			return p.z / 12.0 + 0.03 * noise.get_noise_2d(p.x * 2.0, p.z * 2.0)
		"hollows":
			# Flat ground with three shallow dips 4 to 5 m across, 20 to 30 cm deep (where puddles gather).
			var h := 0.0
			for d in [Vector3(-4.0, 0.25, -3.0), Vector3(5.0, 0.3, 2.0), Vector3(-2.0, 0.2, 6.0)]:
				h -= d.y * exp(-(pow(p.x - d.x, 2.0) + pow(p.z - d.z, 2.0)) / 3.0)
			return h
		"meadow":
			return 0.12 * noise.get_noise_2d(p.x * 3.0, p.z * 3.0) + 0.02 * p.x
		_:
			return 0.0

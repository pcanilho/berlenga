extends Node3D
# A height source for SnowCover3D (any node with get_height(position)): a mountain with a cliff band
# and a valley, a plain with a 60 degree wall for the slide checks, or a corniced ridge.

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
		"avalanche":
			# A 38 degree face 80 m long (62 m high) from x = -80 m down to x = 0, a plateau above, a flat run-out
			# below; a shallow gully along it (z = 0) so the flow gathers.
			var t := clampf(-p.x, 0.0, 80.0) * tan(deg_to_rad(38.0))
			return t - 4.0 * exp(-p.z * p.z / 400.0) * clampf(-p.x / 20.0, 0.0, 1.0)
		"avalanche_bowl":
			# The avalanche face as a mountainside, for the powder shot: steepening to about 45 degrees mid-face and
			# easing out into the run-out (no break of slope), the ridge rising on above it (no plateau), the sides
			# rising into spurs (a cirque, not a ramp with straight sides), the gully, and rolling ground over it.
			var s := -p.x
			var t := 80.0 * tan(deg_to_rad(38.0)) * smoothstep(-12.0, 92.0, s) + maxf(s - 92.0, 0.0) * 0.45
			var side := maxf(absf(p.z) - 75.0, 0.0)
			t += 0.012 * side * side * (0.4 + 0.6 * smoothstep(-20.0, 60.0, s))
			t -= 4.0 * exp(-p.z * p.z / 400.0) * clampf(s / 20.0, 0.0, 1.0)
			return t + (0.8 + 2.2 * smoothstep(0.0, 40.0, s)) * noise.get_noise_2d(p.x * 3.0, p.z * 3.0)
		"avalanche_lake":
			# The avalanche face with a lake at its foot: the bed falls at 25 degrees from x = 2 m to 15 m deep
			# (the water stands at 0).
			var t := clampf(-p.x, 0.0, 80.0) * tan(deg_to_rad(38.0))
			t -= 4.0 * exp(-p.z * p.z / 400.0) * clampf(-p.x / 20.0, 0.0, 1.0)
			return t - clampf((p.x - 2.0) * tan(deg_to_rad(25.0)), 0.0, 15.0)
		"snowfield":
			# A level snowfield 40 m across (the rut view), rolling ground beyond it rising into hills on the horizon.
			var r := Vector2(p.x, p.z).length()
			var rise := smoothstep(30.0, 260.0, r)
			return rise * rise * 70.0 * (0.6 + 0.4 * noise.get_noise_2d(p.x * 0.7, p.z * 0.7)) + smoothstep(18.0, 60.0, r) * 2.0 * noise.get_noise_2d(p.x * 4.0, p.z * 4.0)
		"meadow":
			# Gentle ground with knee-high hummocks, for the trail view.
			return 0.25 * noise.get_noise_2d(p.x * 30.0, p.z * 30.0) + 0.03 * p.x
		"cornice":
			# A ridge along z: a 15 degree windward slope from the west up to a crest at x = 0, a 50 degree
			# lee face to the east; the crest 150 m up, rising and falling a little along its length.
			var top := 150.0 + 6.0 * sin(p.z / 90.0)
			if p.x < 0.0:
				return maxf(0.0, top + p.x * tan(deg_to_rad(15.0)))
			return maxf(0.0, top - p.x * tan(deg_to_rad(50.0)))
		"ridge":
			# A 100 m ridge along z, 25 degree flanks.
			return maxf(0.0, 100.0 - absf(p.x) * tan(deg_to_rad(25.0)))
		_:
			var r := Vector2(p.x, p.z).length()
			var h := 700.0 * exp(-pow(r / 900.0, 2.0))
			h += 50.0 * noise.get_noise_2d(p.x, p.z)
			return h

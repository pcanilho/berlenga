extends WorldFire
# A past fire for the storm bench (--char=C): every stem foot charred C metres deep, nothing burning now.
var char_m := 0.0

func _heat_at(_p: Vector3) -> float:
	return 0.0

func _burnt_at(_p: Vector3) -> float:
	return 1.0 if char_m > 0.0 else 0.0

func _wind_at(_p: Vector3) -> Vector3:
	return Vector3()

func _stem_char_at(_p: Vector3) -> float:
	return char_m

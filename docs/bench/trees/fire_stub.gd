extends WorldFire
# A past fire for the storm bench: every stem foot charred C metres deep (--char=C), and with --snag=Y every crown
# killed Y years ago (standing snags); nothing burning now.
var char_m := 0.0
var snag_years := -1.0

func _heat_at(_p: Vector3) -> float:
	return 0.0

func _burnt_at(_p: Vector3) -> float:
	return 1.0 if char_m > 0.0 or snag_years >= 0.0 else 0.0

func _wind_at(_p: Vector3) -> Vector3:
	return Vector3()

func _stem_char_at(_p: Vector3) -> float:
	return char_m

func _crown_scorch_at(_p: Vector3) -> float:
	return 1000.0 if snag_years >= 0.0 else 0.0

func _burn_age_at(_p: Vector3) -> float:
	return snag_years if snag_years >= 0.0 else (0.0 if char_m > 0.0 else -1.0)

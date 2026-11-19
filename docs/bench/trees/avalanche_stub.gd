extends WorldSnow
# An avalanche for the storm bench: the same peak impact pressure (Pa) everywhere (--avalanche-test sets it).
var pressure := 0.0

func _avalanche_pressure_at(_p: Vector3) -> float:
	return pressure

func _avalanche_peak_pressure_at(_p: Vector3) -> float:
	return pressure

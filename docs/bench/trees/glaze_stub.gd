extends WorldWeather
# Glaze for the storm bench (--glaze-test): the same radial clear ice (mm) on every exposed twig, in a calm.
var glaze_mm := 0.0

func _glaze_at(_p: Vector3) -> float:
	return glaze_mm

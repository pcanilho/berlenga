extends WorldSnow
# Snow held in tree crowns for the storm bench (--snow-test, --snow=L): the same load (kg per m2 of ground)
# everywhere; a shake takes its share off and is counted.
var load_kg_m2 := 0.0
var capacity := 30.0
var shaken_kg := 0.0
var shakes := 0

func _canopy_load_at(_p: Vector3) -> float:
	return load_kg_m2

func _canopy_capacity_at(_p: Vector3) -> float:
	return capacity

func _shake_canopy(_p: Vector3, radius: float, share: float) -> float:
	var kg := load_kg_m2 * share * PI * radius * radius
	shaken_kg += kg
	shakes += 1
	return kg

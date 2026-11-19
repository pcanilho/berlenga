extends WorldFire
# A burn scar for --test=burnscar: the uphill half (x > 0) burnt to bare, water-repellent soil (REPEL), nothing alight.
var repel := 1.0

func _heat_at(_p: Vector3) -> float:
	return 0.0

func _burnt_at(p: Vector3) -> float:
	return 1.0 if p.x > 0.0 else 0.0

func _wind_at(_p: Vector3) -> Vector3:
	return Vector3()

func _water_repellency_at(p: Vector3) -> float:
	return repel if p.x > 0.0 else 0.0

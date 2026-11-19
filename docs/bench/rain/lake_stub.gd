extends WorldWater
# A lake at the valley's outlet (valley.gd): its surface at 0 m below z = -1700, nothing elsewhere. It sums the
# sediment the runoff hands it (kg/s rates; the bench steps one hour at a time).

var received := Vector2()
var calls := 0

func _height_at(p: Vector3) -> float:
	return 0.0 if p.z < -1700.0 else -INF

func _add_sediment(_p: Vector3, mineral_kg_s: float, ash_kg_s: float) -> void:
	received += Vector2(mineral_kg_s, ash_kg_s)
	calls += 1

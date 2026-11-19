extends Node3D
# A 30 m cliff over x < 1 with one free fall off its brink, for the waterfall contract check.

func get_height(p: Vector3) -> float:
	return 30.0 if p.x < 1.0 else 0.0


func get_waterfalls() -> Array:
	return [{"type": "plunge", "id": "test", "top": Vector3(0.5, 30, 0), "bottom": Vector3(3, 0, 0), "height": 30.0, "discharge": 2.0, "width": 3.0, "direction": Vector3(1, 0, 0)}]

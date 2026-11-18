extends Node3D
# A V valley falling toward -z: the runoff's test ground (WeatherRunoff3D reads get_height_at()).

func get_height_at(p: Vector3) -> float:
	return 0.05 * absf(p.x) + 0.01 * p.z

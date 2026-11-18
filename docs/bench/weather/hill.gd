extends Node3D
# A test terrain for the wind over terrain (main.gd --test=terrain): a round hill and a long ridge.
# Anything with get_height(position) serves as WeatherMap3D.terrain_source.

var hill_height := 60.0
var hill_radius := 120.0 # Gaussian sigma, m
var ridge_x := 900.0 # a ridge across the wind, 40 m tall, 60 m sigma


func get_height(p: Vector3) -> float:
	var r2 := p.x * p.x + p.z * p.z
	var h := hill_height * exp(-r2 / (2.0 * hill_radius * hill_radius))
	var d := p.x - ridge_x
	h += 40.0 * exp(-d * d / (2.0 * 60.0 * 60.0))
	return h

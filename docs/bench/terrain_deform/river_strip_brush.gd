# A stand-in for PhysXRiver3D's brush: a straight channel along X with a d^(5/3)-like cross profile, cut
# from the UNCARVED ground (deformer.get_base_height), so carving it again never digs deeper.
extends Node3D

signal carve_changed

@export var length := 40.0
@export var width := 8.0
@export var depth := 2.0
@export var bank := 3.0
@export var cell := 0.5
var deformer: TerrainDeformer3D


# Tiled like PhysXRiver3D.get_carve_brushes (one tile here).
func get_carve_brushes(_tile_size: float) -> Array:
	if deformer == null:
		return []
	var o := global_position
	var half_w := width * 0.5 + bank
	var rect := Rect2(o.x - length * 0.5, o.z - half_w, length, half_w * 2.0)
	# Cell-centred samples over nx * cell, as PhysXRiver3D's brushes.
	var nx := int(ceil(rect.size.x / cell))
	var nz := int(ceil(rect.size.y / cell))
	rect.size = Vector2(nx * cell, nz * cell)
	var heights := PackedFloat32Array()
	var mask := PackedFloat32Array()
	heights.resize(nx * nz)
	mask.resize(nx * nz)
	for z in nz:
		for x in nx:
			var wx := rect.position.x + (x + 0.5) * cell
			var wz := rect.position.y + (z + 0.5) * cell
			var across: float = absf(wz - o.z) / (width * 0.5)
			var base: float = deformer.base_height_at(Vector2(wx, wz))
			var d: float = depth * pow(maxf(0.0, 1.0 - across * across), 5.0 / 6.0)
			heights[z * nx + x] = base - d
			var outside: float = maxf(0.0, absf(wz - o.z) - width * 0.5)
			mask[z * nx + x] = 1.0 - smoothstep(0.0, bank, outside)
	return [{"rect": rect, "cell_size": cell, "size": Vector2i(nx, nz), "heights": heights, "mask": mask, "mode": "lower"}]


func changed() -> void:
	carve_changed.emit()

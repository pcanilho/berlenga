extends Node3D
# A mountain ridge for the wave cloud views: a Witch of Agnesi profile (the standard mountain-wave test ridge,
# Queney 1948) lying along z, with a few peaks along it, answering get_height() as a terrain source does.

var crest_x := -6000.0 # m
var height := 2000.0 # m
var half_width := 3000.0 # m
var length := 80000.0 # m, along z, centred on z = -10 km

func get_height(p: Vector3) -> float:
	var dx := (p.x - crest_x) / half_width
	var along := (p.z + 10000.0) / (0.5 * length)
	var ends := clampf(1.0 - along * along, 0.0, 1.0)
	var peaks := 0.8 + 0.2 * sin(p.z / 3100.0) * sin(p.z / 1700.0 + 1.0)
	return height * peaks * ends / (1.0 + dx * dx)

func build_mesh(material: Material) -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var n := 200
	var x0 := crest_x - 15000.0
	var z0 := -10000.0 - 0.5 * length
	var sx := 30000.0 / n
	var sz := length / n
	for j in n:
		for i in n:
			var c := [Vector2(i, j), Vector2(i + 1, j), Vector2(i + 1, j + 1), Vector2(i, j), Vector2(i + 1, j + 1), Vector2(i, j + 1)]
			for k in [0, 1, 2, 3, 4, 5]:
				var q: Vector2 = c[k]
				var p := Vector3(x0 + q.x * sx, 0.0, z0 + q.y * sz)
				p.y = get_height(p)
				st.add_vertex(p)
	st.generate_normals()
	var mi := MeshInstance3D.new()
	mi.mesh = st.commit()
	mi.material_override = material
	add_child(mi)

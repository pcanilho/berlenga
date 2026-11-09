extends Node3D
# Displaced varyings bench: surfaces whose vertex() moves VERTEX and passes the position it started from to
# fragment() in a varying, raster or path traced. The checker is drawn from that rest position, so the path
# traced image matches raster only when the hit re-runs vertex() on the rest pose (issue #169).
#
#   Left:   a mesh, shifted sideways and waved up and down.
#   Right:  a MultiMesh of four tiles with the same shader (the merged path).
#   Front:  a strip whose vertex() writes COLOR from its rest position.
#
#   --raster        raster only, for comparison
#   --spp=N --bounces=N  path tracing samples and bounces (default 4, 2)
#   --frames=N      frames rendered before --out is saved (default 6)
#   --out=path.png  save the last frame and quit

const SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform vec3 shift = vec3(0.6, 0.0, 0.3);
varying vec3 v_rest;
void vertex() {
	v_rest = VERTEX;
	VERTEX += shift;
	VERTEX.y += 0.35 * sin(v_rest.x * 2.5) * cos(v_rest.z * 1.7);
}
void fragment() {
	vec2 c = floor(v_rest.xz * 2.0);
	float k = mod(c.x + c.y, 2.0);
	ALBEDO = mix(vec3(0.85, 0.82, 0.75), vec3(0.15, 0.25, 0.55), k);
	ROUGHNESS = 0.8;
}
"""

const COLOR_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void vertex() {
	COLOR = vec4(0.5 + 0.5 * sin(VERTEX.x * 1.3), 0.3, 0.5 - 0.5 * sin(VERTEX.x * 1.3), 1.0);
	VERTEX.x += 0.75;
}
void fragment() {
	ALBEDO = COLOR.rgb;
}
"""

var raster := false
var spp := 4
var bounces := 2
var frames := 6
var out := ""
var n := 0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--raster": raster = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--bounces="): bounces = int(a.substr(10))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.5, 0.55, 0.6)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.6, 0.6, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = bounces
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-60, -30, 0)
	add_child(sun)

	var shader := Shader.new()
	shader.code = SHADER
	var mat := ShaderMaterial.new()
	mat.shader = shader

	var plane := PlaneMesh.new()
	plane.size = Vector2(4, 4)
	plane.subdivide_width = 24
	plane.subdivide_depth = 24

	var mi := MeshInstance3D.new()
	mi.mesh = plane
	mi.material_override = mat
	mi.position = Vector3(-2.6, 0, 0)
	add_child(mi)

	var tile := PlaneMesh.new()
	tile.size = Vector2(1.8, 1.8)
	tile.subdivide_width = 12
	tile.subdivide_depth = 12
	tile.material = mat
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = tile
	mm.instance_count = 4
	for i in 4:
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3(1.0 + 2.0 * (i % 2), 0, -1.0 + 2.0 * (i / 2))))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)

	var color_shader := Shader.new()
	color_shader.code = COLOR_SHADER
	var color_mat := ShaderMaterial.new()
	color_mat.shader = color_shader
	var strip := PlaneMesh.new()
	strip.size = Vector2(8, 0.8)
	strip.subdivide_width = 32
	var smi := MeshInstance3D.new()
	smi.mesh = strip
	smi.material_override = color_mat
	smi.position = Vector3(-0.5, 0.2, 2.8)
	add_child(smi)

	var cam := Camera3D.new()
	cam.position = Vector3(0, 7.5, 6.5)
	cam.rotation_degrees = Vector3(-50, 0, 0)
	cam.fov = 55
	add_child(cam)


func _process(_delta: float) -> void:
	n += 1
	if out != "" and n >= frames:
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(out)
		get_tree().quit()

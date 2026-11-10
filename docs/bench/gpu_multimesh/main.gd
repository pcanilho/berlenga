extends Node3D
# MultiMesh records written by a compute shader, the way GPU placement and grass write them, against
# the same records set from the CPU. Two MultiMeshes: 64 boxes (under the path tracer's merge cap, so
# one merged BLAS read from the GPU buffer) and 25 dense spheres (above it, so one TLAS record per
# sphere, built from the CPU mirror). Records are written twice: at frame 5, then moved at frame 40.
#
# godot --path docs/bench/gpu_multimesh --rendering-driver vulkan -- [options]
#   --mode=gpu        compute writes, then RenderingServer.multimesh_mark_buffer_rd_changed() (default)
#   --mode=nomark     compute writes without the call (what an engine without it sees)
#   --mode=cpu        RenderingServer.multimesh_set_buffer() with the same records (reference)
#   --raster          raster only
#   --taa             TAA on, so MultiMeshes keep two motion-vector halves
#   --spp=N           path tracing samples per pixel (default 16)
#   --frames=N        frames before the capture and checks (default 80)
#   --out=path.png    save the last frame
#   --test            check the AABBs and the records read back, print PASS or FAIL, exit 1 on FAIL

const STRIDE := 12
var mode := "gpu"
var raster := false
var taa := false
var spp := 16
var frames := 80
var out := ""
var test := false
var n := 0
var boxes: MultiMesh
var spheres: MultiMesh
var shader := RID()
var pipeline := RID()

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--mode="): mode = a.substr(7)
		elif a == "--raster": raster = true
		elif a == "--taa": taa = true
		elif a.begins_with("--spp="): spp = int(a.substr(6))
		elif a.begins_with("--frames="): frames = int(a.substr(9))
		elif a.begins_with("--out="): out = a.substr(6)
		elif a == "--test": test = true
	get_viewport().use_taa = taa
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.55, 0.68, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.45, 0.5, 0.6)
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.pathtracing_enabled = not raster
	env.pathtracing_samples_per_pixel = spp
	env.pathtracing_max_bounces = 2
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55, 35, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(30, 30)
	pm.material = _mat(Color(0.35, 0.33, 0.3))
	ground.mesh = pm
	add_child(ground)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 7, 9)
	cam.rotation_degrees = Vector3(-38, 0, 0)
	add_child(cam)

	var box := BoxMesh.new()
	box.size = Vector3(0.6, 0.6, 0.6)
	box.material = _mat(Color(0.8, 0.3, 0.2))
	boxes = _multimesh(box, 64)
	var sphere := SphereMesh.new()
	sphere.radius = 0.4
	sphere.height = 0.8
	sphere.radial_segments = 64
	sphere.rings = 32
	sphere.material = _mat(Color(0.25, 0.5, 0.85))
	spheres = _multimesh(sphere, 25)

	if mode != "cpu":
		var rd := RenderingServer.get_rendering_device()
		var src := RDShaderSource.new()
		src.source_compute = COMPUTE
		var spirv := rd.shader_compile_spirv_from_source(src)
		assert(spirv.compile_error_compute.is_empty(), spirv.compile_error_compute)
		shader = rd.shader_create_from_spirv(spirv)
		pipeline = rd.compute_pipeline_create(shader)

func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 0.6
	return m

func _multimesh(mesh: Mesh, count: int) -> MultiMesh:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = count
	var mi := MultiMeshInstance3D.new()
	mi.multimesh = mm
	add_child(mi)
	return mm

# Grid placement shared by the compute shader and the CPU reference.
func _layout(mm: MultiMesh, shift: float) -> Dictionary:
	if mm == boxes:
		return {"side": 8, "spacing": 0.9, "x0": -7.5, "z0": -3.0 + shift}
	return {"side": 5, "spacing": 1.1, "x0": 1.5, "z0": -2.0 + shift}

func _cpu_records(mm: MultiMesh, shift: float) -> PackedFloat32Array:
	var l := _layout(mm, shift)
	var d := PackedFloat32Array()
	d.resize(mm.instance_count * STRIDE)
	for i in mm.instance_count:
		var b := i * STRIDE
		d[b + 0] = 1.0; d[b + 5] = 1.0; d[b + 10] = 1.0
		d[b + 3] = l.x0 + float(i % int(l.side)) * l.spacing
		d[b + 7] = 0.45
		d[b + 11] = l.z0 + float(i / int(l.side)) * l.spacing
	return d

func _write(mm: MultiMesh, shift: float) -> void:
	if mode == "cpu":
		RenderingServer.multimesh_set_buffer(mm.get_rid(), _cpu_records(mm, shift))
		return
	var rd := RenderingServer.get_rendering_device()
	var l := _layout(mm, shift)
	var buf := RenderingServer.multimesh_get_buffer_rd_rid(mm.get_rid())
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = 0
	u.add_id(buf)
	var set := rd.uniform_set_create([u], shader, 0)
	var pc := PackedFloat32Array([l.spacing, l.x0, l.z0, 0.45]).to_byte_array()
	pc.append_array(PackedInt32Array([mm.instance_count, l.side, 0, 0]).to_byte_array())
	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, pipeline)
	rd.compute_list_bind_uniform_set(cl, set, 0)
	rd.compute_list_set_push_constant(cl, pc, pc.size())
	rd.compute_list_dispatch(cl, (mm.instance_count + 63) / 64, 1, 1)
	rd.compute_list_end()
	rd.free_rid(set)
	if mode == "gpu":
		RenderingServer.multimesh_mark_buffer_rd_changed(mm.get_rid())

func _process(_delta: float) -> void:
	n += 1
	if n == 5 or n == 40:
		var shift := 0.0 if n == 5 else 2.0
		for mm in [boxes, spheres]:
			RenderingServer.call_on_render_thread(_write.bind(mm, shift))
	if n < frames:
		return
	set_process(false)
	await RenderingServer.frame_post_draw
	if not out.is_empty():
		get_viewport().get_texture().get_image().save_png(out)
		print("saved ", out)
	var ok := true
	if test:
		ok = _check()
	get_tree().quit(0 if ok else 1)

func _check() -> bool:
	var ok := true
	for mm in [boxes, spheres]:
		var want := _cpu_records(mm, 2.0)
		var got := RenderingServer.multimesh_get_buffer(mm.get_rid())
		var same := got.size() >= want.size()
		for i in want.size():
			if not same or absf(got[i] - want[i]) > 1e-5:
				same = false
				break
		var aabb := RenderingServer.multimesh_get_aabb(mm.get_rid())
		var l := _layout(mm, 2.0)
		var far: float = float(int(l.side) - 1) * l.spacing
		var want_aabb: AABB = AABB(Vector3(l.x0, 0.45, l.z0), Vector3(far, 0.0, float((mm.instance_count - 1) / int(l.side)) * l.spacing))
		var mesh_aabb: AABB = mm.mesh.get_aabb()
		want_aabb.position += mesh_aabb.position
		want_aabb.size += mesh_aabb.size
		var aabb_ok := aabb.position.is_equal_approx(want_aabb.position) and aabb.size.is_equal_approx(want_aabb.size)
		print("%s records %s, aabb %s (got %s, want %s)" % ["boxes" if mm == boxes else "spheres", "ok" if same else "WRONG", "ok" if aabb_ok else "WRONG", aabb, want_aabb])
		ok = ok and same and aabb_ok
	print("PASS" if ok else "FAIL")
	return ok

const COMPUTE := """
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) restrict writeonly buffer Records { float d[]; };
layout(push_constant, std430) uniform Params {
	float spacing;
	float x0;
	float z0;
	float y;
	uint count;
	uint side;
	uint pad0;
	uint pad1;
} p;
void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= p.count) {
		return;
	}
	uint b = i * 12u;
	d[b + 0u] = 1.0; d[b + 1u] = 0.0; d[b + 2u] = 0.0; d[b + 3u] = p.x0 + float(i % p.side) * p.spacing;
	d[b + 4u] = 0.0; d[b + 5u] = 1.0; d[b + 6u] = 0.0; d[b + 7u] = p.y;
	d[b + 8u] = 0.0; d[b + 9u] = 0.0; d[b + 10u] = 1.0; d[b + 11u] = p.z0 + float(i / p.side) * p.spacing;
}
"""

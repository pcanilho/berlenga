extends Node3D
# Village fire bench: a wind-driven grass fire runs into a street of timber houses (PhysXBuildingFire3D) and the
# firebrands and radiation between them carry it down the street, the way most homes are lost in wildland-urban
# interface fires (Maranghides and Mell 2011; NIST's Camp Fire study, 2021). Prints each house as it catches (when
# and by what: heat from flames nearby, firebrands, or set alight), one status line a minute, and a summary.
# Headless is fine (no flames drawn); with a window it draws them and can save stills.
#
# godot --headless --fixed-fps 60 --path docs/bench/village_fire -- [options]
#   --houses=N        houses in each row (default 6)
#   --rows=N          1 or 2 rows facing a street (default 2)
#   --gap=M           side gap between neighbouring houses in metres (default 6)
#   --street=M        street width between the rows in metres (default 12)
#   --wind=M          wind speed in m/s, blowing down the street (+X) (default 8)
#   --buffer=M        bare ground between the wildland grass and the first houses (default 10)
#   --fuel=N          the wildland's fuel: 0 short grass, 1 tall grass, 2 litter (default 1)
#   --yards=F         fuel over the whole village (lawns), 0..1 of the wildland's (default 0: mown)
#   --beds=F          fuel in a 1.5 m ring against each house (dry beds, litter, mulch: the immediate zone),
#                     0..1 (default 0.6; 0 = cleared)
#   --minutes=M       fire minutes to run (default 60)
#   --speed=F         runs fire and houses this many times faster than real time (default 10)
#   --no-brands       houses throw no firebrands (radiation and contact only)
#   --shot=PATH@MIN   save a still at that fire minute (needs a window; repeatable)
var houses := 6
var rows := 2
var gap := 6.0
var street := 12.0
var wind := 8.0
var yards := 0.0
var beds := 0.6
var buffer := 10.0
var fuel := 1
var minutes := 60.0
var speed := 10.0
var brands := true
var shots := []

const W := 8.0 # house frontage (two rooms of 4 m)
const D := 5.0 # house depth
const H := 2.6
const UPWIND := 40.0 # open grass upwind of the first house

var fire
var list := [] # [{b, name}]
var t := 0.0
var next_status := 1.0
var wood: StandardMaterial3D

func _arg(a: String, k: String) -> String:
	return a.substr(k.length()) if a.begins_with(k) else ""

func _ready():
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--houses="): houses = int(_arg(a, "--houses="))
		if a.begins_with("--rows="): rows = clampi(int(_arg(a, "--rows=")), 1, 2)
		if a.begins_with("--gap="): gap = float(_arg(a, "--gap="))
		if a.begins_with("--street="): street = float(_arg(a, "--street="))
		if a.begins_with("--wind="): wind = float(_arg(a, "--wind="))
		if a.begins_with("--buffer="): buffer = float(_arg(a, "--buffer="))
		if a.begins_with("--fuel="): fuel = int(_arg(a, "--fuel="))
		if a.begins_with("--yards="): yards = float(_arg(a, "--yards="))
		if a.begins_with("--beds="): beds = float(_arg(a, "--beds="))
		if a.begins_with("--minutes="): minutes = float(_arg(a, "--minutes="))
		if a.begins_with("--speed="): speed = float(_arg(a, "--speed="))
		if a == "--no-brands": brands = false
		if a.begins_with("--shot="):
			var kv := _arg(a, "--shot=").split("@")
			shots.append([kv[0], float(kv[1])])
	shots.sort_custom(func(x, y): return x[1] < y[1])
	Engine.max_physics_steps_per_frame = 64
	seed(7)
	var length := UPWIND + houses * (W + gap) + 30.0
	var width := street + rows * (D + 20.0) + 10.0
	var origin := Vector3(-UPWIND, 0, -width * 0.5)
	_scene(length, width, origin)

	var wm = ClassDB.instantiate("WeatherMap3D")
	wm.wind_speed = wind; wm.wind_direction = Vector2(1, 0); wm.cloud_cover = 0.0; wm.rain_rate = 0.0
	wm.temperature = 32.0; wm.humidity = 0.15
	add_child(wm); wm.snap_to_settings()

	# The grass: full fuel upwind, the yards' share round the houses, none on the street and under the houses.
	var img := Image.create(int(length), int(width), false, Image.FORMAT_RGB8)
	for y in img.get_height():
		for x in img.get_width():
			var p := origin + Vector3(x + 0.5, 0, y + 0.5)
			var f := 1.0
			if p.x > -buffer: f = 0.0
			if p.x > -2.0:
				f = yards
				var hx := fposmod(p.x, W + gap)
				var hz := absf(p.z) - street * 0.5
				var near_x := hx < W + 1.5 or hx > W + gap - 1.5
				if p.x < houses * (W + gap) and near_x and hz > -1.5 and hz < D + 1.5 and (rows == 2 or p.z < 0.0):
					f = maxf(f, beds)
			if rows == 2 and absf(p.z) < street * 0.5: f = 0.0
			img.set_pixel(x, y, Color(f, 0, 0))
	var flow = null
	if DisplayServer.get_name() != "headless":
		flow = ClassDB.instantiate("PhysXFlow3D"); flow.name = "FireGas"; add_child(flow)
	fire = ClassDB.instantiate("PhysXFireSpread3D")
	fire.name = "Fire"
	fire.size = Vector2(length, width); fire.position = origin + Vector3(length, 0, width) * 0.5
	fire.fuel_type = fuel; fire.fuel_moisture = 0.04; fire.use_weather_moisture = false
	fire.cell_size = 1.0; fire.fuel_map = img; fire.spread_scale = speed
	add_child(fire)
	if flow: fire.flow_path = fire.get_path_to(flow)

	for row in rows:
		for i in houses:
			var x := i * (W + gap)
			var z := -street * 0.5 - D if row == 0 else street * 0.5
			_house(Vector3(x, 0, z), row == 0, "%s%d" % ["N" if row == 0 else "S", i + 1])
	await get_tree().physics_frame
	await get_tree().physics_frame
	# A line of fire across the upwind edge.
	for k in range(4, int(width) - 4, 3):
		fire.ignite(origin + Vector3(3, 0, k), 1.0)
	print("Village fire bench: %d x %d houses, gap %.0f m, street %.0f m, wind %.0f m/s, fuel %d, buffer %.0f m, yards %.2f, beds %.2f, brands %s, %.0f min at %.0fx" % [rows, houses, gap, street, wind, fuel, buffer, yards, beds, brands, minutes, speed])

func _scene(length: float, width: float, origin: Vector3):
	var g := StaticBody3D.new(); var cs := CollisionShape3D.new(); var bs := BoxShape3D.new()
	bs.size = Vector3(length + 40, 1, width + 40); cs.shape = bs; cs.position = origin + Vector3(length * 0.5, -0.5, width * 0.5)
	g.add_child(cs); add_child(g)
	wood = StandardMaterial3D.new(); wood.albedo_color = Color(0.55, 0.42, 0.3); wood.roughness = 0.85
	if DisplayServer.get_name() == "headless":
		return
	var gm := MeshInstance3D.new(); var pm := PlaneMesh.new(); pm.size = Vector2(length + 40, width + 40)
	var gmat := StandardMaterial3D.new(); gmat.albedo_color = Color(0.36, 0.31, 0.18); gmat.roughness = 0.95
	pm.material = gmat; gm.mesh = pm; gm.position = cs.position + Vector3(0, 0.5, 0); add_child(gm)
	var env := WorldEnvironment.new(); var e := Environment.new()
	e.background_mode = Environment.BG_COLOR; e.background_color = Color(0.55, 0.58, 0.62)
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR; e.ambient_light_color = Color(0.55, 0.58, 0.62); e.ambient_light_energy = 0.5
	e.tonemap_mode = Environment.TONE_MAPPER_AGX; env.environment = e; add_child(env)
	var sun := DirectionalLight3D.new(); sun.rotation_degrees = Vector3(-40, -50, 0); sun.shadow_enabled = true; add_child(sun)
	var cam := Camera3D.new(); add_child(cam)
	var mid := Vector3(houses * (W + gap) * 0.5, 0, 0)
	cam.position = mid + Vector3(-houses * 4.0 - 20.0, 22.0, -36.0); cam.look_at(mid + Vector3(-6, 0, 0)); cam.fov = 50

# Two rooms side by side along X; the front faces the street. Doors and windows as PhysXBuildingFire3D openings.
func _house(at: Vector3, front_pos_z: bool, label: String):
	var front := 3 if front_pos_z else 2
	var back := 2 if front_pos_z else 3
	var rooms := [
		{"box": AABB(Vector3(0, 0, 0), Vector3(4, H, D)), "openings": [
			{"face": front, "rect": Rect2(1.5, 0, 0.9, 2.0), "glazed": false},
			{"face": back, "rect": Rect2(1.4, 0.9, 1.2, 1.2), "glazed": true},
			{"face": 0, "rect": Rect2(1.9, 0.9, 1.2, 1.2), "glazed": true},
			{"face": 1, "rect": Rect2(2.0, 0, 0.9, 2.0)}]},
		{"box": AABB(Vector3(4, 0, 0), Vector3(4, H, D)), "openings": [
			{"face": front, "rect": Rect2(1.4, 0.9, 1.2, 1.2), "glazed": true},
			{"face": back, "rect": Rect2(1.4, 0.9, 1.2, 1.2), "glazed": true},
			{"face": 1, "rect": Rect2(1.9, 0.9, 1.2, 1.2), "glazed": true}]},
	]
	var body := StaticBody3D.new(); body.name = "House" + label; body.position = at; add_child(body)
	for r in rooms:
		for face in 4:
			var box: AABB = r.box
			var shared: bool = (face == 1 and box.position.x < 1.0) or (face == 0 and box.position.x > 1.0)
			if not shared or face == 1:
				_wall(body, r, face)
	_box(body, Vector3(-0.1, H, -0.1), Vector3(8.1, H + 0.15, D + 0.1))
	var b = ClassDB.instantiate("PhysXBuildingFire3D")
	b.name = "Fire"
	b.rooms = rooms; b.time_scale = speed; b.firebrands = brands
	body.add_child(b)
	b.fire = b.get_path_to(fire)
	b.target = b.get_path_to(body)
	b.ignited.connect(func(cause): _caught(label, b, cause))
	list.append({"b": b, "name": label})

func _caught(label: String, b, cause: String):
	print("  %s caught at %5.1f min by %s" % [label, b.get_ignition_time() / 60.0, cause])

func _box(parent: Node3D, a: Vector3, c: Vector3):
	var s := (c - a).abs()
	if s.x < 0.01 or s.y < 0.01 or s.z < 0.01: return
	var cs := CollisionShape3D.new(); var sh := BoxShape3D.new(); sh.size = s; cs.shape = sh; cs.position = (a + c) * 0.5; parent.add_child(cs)
	if DisplayServer.get_name() == "headless": return
	var mi := MeshInstance3D.new(); var bm := BoxMesh.new(); bm.size = s; bm.material = wood; mi.mesh = bm
	mi.position = cs.position; parent.add_child(mi)

# A wall slab on a room face (thickness outward) with its one opening cut out.
func _wall(parent: Node3D, r: Dictionary, face: int):
	var box: AABB = r.box
	var lo := box.position; var hi := box.end
	var th := 0.1
	var hole := Rect2()
	for o in r.openings:
		if o.face == face: hole = o.rect
	var along := box.size.z if face < 2 else box.size.x
	var h := box.size.y
	var pieces := []
	if hole.size == Vector2():
		pieces.append(Rect2(0, 0, along, h))
	else:
		pieces.append(Rect2(0, 0, hole.position.x, h))
		pieces.append(Rect2(hole.end.x, 0, along - hole.end.x, h))
		pieces.append(Rect2(hole.position.x, 0, hole.size.x, hole.position.y))
		pieces.append(Rect2(hole.position.x, hole.end.y, hole.size.x, h - hole.end.y))
	for p in pieces:
		var a: Vector3; var c: Vector3
		match face:
			0: a = Vector3(lo.x - th, lo.y + p.position.y, lo.z + p.position.x); c = Vector3(lo.x, lo.y + p.end.y, lo.z + p.end.x)
			1: a = Vector3(hi.x, lo.y + p.position.y, lo.z + p.position.x); c = Vector3(hi.x + th, lo.y + p.end.y, lo.z + p.end.x)
			2: a = Vector3(lo.x + p.position.x, lo.y + p.position.y, lo.z - th); c = Vector3(lo.x + p.end.x, lo.y + p.end.y, lo.z)
			3: a = Vector3(lo.x + p.position.x, lo.y + p.position.y, hi.z); c = Vector3(lo.x + p.end.x, lo.y + p.end.y, hi.z + th)
		_box(parent, a, c)

func _lost(b) -> bool:
	for i in b.get_room_count():
		if b.get_room_flashover_time(i) >= 0.0 or b.is_room_collapsed(i): return true
	return false

func _physics_process(dt):
	t += dt * speed
	var m := t / 60.0
	if not shots.is_empty() and m >= shots[0][1]:
		var sh = shots.pop_front()
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(sh[0])
		print("saved ", sh[0])
	if m >= next_status:
		next_status += 1.0
		var burning := 0; var lost := 0; var caught := 0
		for h in list:
			if h.b.get_ignition_time() >= 0.0: caught += 1
			if h.b.is_burning(): burning += 1
			if _lost(h.b): lost += 1
		var hb := 0
		for h in list: hb += h.b.get_firebrand_count()
		print("t=%3.0f min  grass burning %5d cells, burnt %6.0f m2, brands %d, spot fires %d | house brands %d | houses caught %d, burning %d, lost %d of %d" % [m, fire.get_burning_cells(), fire.get_burnt_area(), fire.get_firebrand_count(), fire.get_spot_fire_count(), hb, caught, burning, lost, list.size()])
	if m >= minutes and shots.is_empty():
		_summary()
		get_tree().quit()

func _summary():
	var causes := {}
	var lost := 0
	var times := []
	for h in list:
		var c: String = h.b.get_ignition_cause()
		if c != "":
			causes[c] = causes.get(c, 0) + 1
			times.append(h.b.get_ignition_time() / 60.0)
		if _lost(h.b): lost += 1
	times.sort()
	var span: float = (times[-1] - times[0]) if times.size() > 1 else 0.0
	print("Summary: %d of %d houses caught, %d lost; by cause %s; first %.1f min, last %.1f min (%.1f min across the village)" % [times.size(), list.size(), lost, causes, times[0] if times.size() > 0 else -1.0, times[-1] if times.size() > 0 else -1.0, span])

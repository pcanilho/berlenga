extends RefCounted
## A walk cycle built from published gait data, for rigs that ship without one (the bench's mannequin).
##
## Joint angles over the gait cycle follow the normative sagittal curves of Winter ("Biomechanics and Motor
## Control of Human Movement", 4th ed. 2009, appendix gait data, natural cadence): hip flexion 30 degrees at heel
## strike to 10 degrees extension at 50%, knee 18 degrees in loading and 60 at early swing, ankle plantarflexion
## to 18 degrees at toe-off. Pelvic rotation (4 degrees), obliquity (4) and tilt, the thorax turning against the
## pelvis, the centre of mass rising twice a stride by about 4 cm peak to peak (lowest in double support) and
## shifting over the stance leg (Perry and Burnfield, "Gait Analysis", 2010), and arms swinging against the legs
## with the elbow flexing more on the forward swing. Every curve is a periodic cubic through its key points, so
## velocities have no breaks. The angles are added on top of a standing clip's first frame (arms down, the
## rig's own posture) about axes in the character's frame (+Z forward, +Y up, +X left), so it suits any rig
## facing +Z. The clip walks on the spot; the body is moved by its owner and FootPlacementModifier3D plants the
## feet on the ground.

const KEYS := 40 # samples per cycle

# [percent of the cycle, degrees]; right leg half a cycle later. Heel strike of the left leg at 0%.
const HIP := [[0, 30], [10, 26], [30, 8], [50, -10], [60, -2], [75, 22], [87, 32], [100, 30]]
const KNEE := [[0, 3], [15, 18], [40, 4], [50, 8], [62, 40], [73, 60], [87, 30], [97, 3], [100, 3]]
const ANKLE := [[0, 0], [7, -6], [30, 8], [45, 10], [55, 0], [62, -18], [75, -2], [90, 0], [100, 0]]
const TOE := [[0, 0], [40, 0], [52, 18], [60, 28], [66, 0], [100, 0]] # the ball (toes) extending through push-off


## A periodic cubic (Catmull-Rom) through `keys` at `p` (0..1 of the cycle).
static func _curve(keys: Array, p: float) -> float:
	p = fposmod(p, 1.0) * 100.0
	var n := keys.size() - 1 # the last key repeats the first
	var i := 0
	while i < n - 1 and p > float(keys[i + 1][0]):
		i += 1
	var at := func(k: int) -> Vector2:
		var w := floori(float(k) / n)
		var kk := posmod(k, n)
		return Vector2(float(keys[kk][0]) + 100.0 * w, float(keys[kk][1]))
	var p0: Vector2 = at.call(i - 1)
	var p1: Vector2 = at.call(i)
	var p2: Vector2 = at.call(i + 1)
	var p3: Vector2 = at.call(i + 2)
	var t := (p - p1.x) / maxf(p2.x - p1.x, 1e-4)
	# non-uniform key spacing: tangents scaled to each segment
	var m1 := (p2.y - p0.y) / maxf(p2.x - p0.x, 1e-4) * (p2.x - p1.x)
	var m2 := (p3.y - p1.y) / maxf(p3.x - p1.x, 1e-4) * (p2.x - p1.x)
	var t2 := t * t
	var t3 := t2 * t
	return (2 * t3 - 3 * t2 + 1) * p1.y + (t3 - 2 * t2 + t) * m1 + (-2 * t3 + 3 * t2) * p2.y + (t3 - t2) * m2


## Builds the walk for `sk` over `period` seconds per stride (two steps), on top of `base`'s first frame.
## `names` maps roles to bone names (pelvis, spine_01, spine_02, thigh_l, calf_l, foot_l, ball_l, the same with
## _r, upperarm_l, lowerarm_l, upperarm_r, lowerarm_r); a missing role is skipped. `stride` scales the arm swing
## and centre-of-mass motion with speed (1 = a normal walk).
static func build(sk: Skeleton3D, base: Animation, period: float, names: Dictionary, stride := 1.0) -> Animation:
	var a := Animation.new()
	a.length = period
	a.loop_mode = Animation.LOOP_LINEAR
	# the base pose: the standing clip's first frame (rotations), else the rest
	var local := {}
	for b in sk.get_bone_count():
		local[b] = sk.get_bone_rest(b)
	if base:
		for t in base.get_track_count():
			var path := base.track_get_path(t)
			if path.get_subname_count() == 0:
				continue
			var b := sk.find_bone(path.get_concatenated_subnames())
			if b < 0:
				continue
			var x: Transform3D = local[b]
			match base.track_get_type(t):
				Animation.TYPE_ROTATION_3D:
					x.basis = Basis(base.rotation_track_interpolate(t, 0.0)).scaled(x.basis.get_scale())
				Animation.TYPE_POSITION_3D:
					x.origin = base.position_track_interpolate(t, 0.0)
			local[b] = x
	var glob := {}
	for b in sk.get_bone_count():
		var p := sk.get_bone_parent(b)
		glob[b] = (glob[p] as Transform3D) * (local[b] as Transform3D) if p >= 0 else local[b]
	var X := Vector3(1, 0, 0) # the character's left: flexion axis (negative flexes forward)
	var Y := Vector3(0, 1, 0)
	var Z := Vector3(0, 0, 1)
	# per bone: the list of [axis in the skeleton's frame, Callable(p) -> degrees]
	var rot := {}
	var add := func(role: String, axis: Vector3, f: Callable) -> void:
		var bn: String = names.get(role, "")
		var b := sk.find_bone(bn) if bn != "" else -1
		if b < 0:
			return
		if not rot.has(b):
			rot[b] = []
		rot[b].append([axis, f])
	var s := clampf(stride, 0.3, 1.6)
	for side in [["l", 0.0], ["r", 0.5]]:
		var sfx: String = side[0]
		var ph: float = side[1]
		# Winter's hip angle is the thigh's from the vertical and the knee's the thigh-shank angle: take
		# away what the standing pose already has, so a clip standing with a leg back does not bias the gait
		var hip0 := 0.0
		var knee0 := 0.0
		var th := sk.find_bone(String(names.get("thigh_" + sfx, "")))
		var ca := sk.find_bone(String(names.get("calf_" + sfx, "")))
		var fo := sk.find_bone(String(names.get("foot_" + sfx, "")))
		if th >= 0 and ca >= 0 and fo >= 0:
			var dt: Vector3 = (glob[ca] as Transform3D).origin - (glob[th] as Transform3D).origin
			var ds: Vector3 = (glob[fo] as Transform3D).origin - (glob[ca] as Transform3D).origin
			hip0 = rad_to_deg(atan2(dt.z, -dt.y))
			knee0 = hip0 - rad_to_deg(atan2(ds.z, -ds.y))
		add.call("thigh_" + sfx, -X, func(p): return _curve(HIP, p + ph) - hip0)
		add.call("calf_" + sfx, X, func(p): return _curve(KNEE, p + ph) - knee0)
		add.call("foot_" + sfx, -X, func(p): return _curve(ANKLE, p + ph))
		add.call("ball_" + sfx, -X, func(p): return _curve(TOE, p + ph))
		# arms against the same side's leg: forward when it is back (peak at the other leg's heel strike)
		add.call("upperarm_" + sfx, -X, func(p): return 4.0 - 14.0 * s * cos(TAU * (p + ph)))
		add.call("lowerarm_" + sfx, -X, func(p): return 18.0 + 10.0 * s * maxf(0.0, -cos(TAU * (p + ph))))
	# pelvis: turned forward on the swing side, dropped on the swing side, a little tilt twice a stride
	add.call("pelvis", Y, func(p): return -4.0 * s * cos(TAU * p))
	add.call("pelvis", Z, func(p): return 4.0 * s * sin(TAU * p))
	add.call("pelvis", X, func(p): return 1.5 * s * cos(2.0 * TAU * p))
	# the thorax turns back against the pelvis, so the shoulders stay square
	add.call("spine_01", Y, func(p): return 2.5 * s * cos(TAU * p))
	add.call("spine_02", Y, func(p): return 3.5 * s * cos(TAU * p))
	add.call("spine_01", Z, func(p): return -2.5 * s * sin(TAU * p))

	var pelvis := sk.find_bone(String(names.get("pelvis", "")))
	var sk_path := _skeleton_path(base)
	var tracks := {}
	for b in rot:
		var t := a.add_track(Animation.TYPE_ROTATION_3D)
		a.track_set_path(t, NodePath("%s:%s" % [sk_path, sk.get_bone_name(b)]))
		a.track_set_interpolation_type(t, Animation.INTERPOLATION_CUBIC)
		a.track_set_interpolation_loop_wrap(t, true)
		tracks[b] = t
	var pos_track := -1
	if pelvis >= 0:
		pos_track = a.add_track(Animation.TYPE_POSITION_3D)
		a.track_set_path(pos_track, NodePath("%s:%s" % [sk_path, sk.get_bone_name(pelvis)]))
		a.track_set_interpolation_type(pos_track, Animation.INTERPOLATION_CUBIC)
		a.track_set_interpolation_loop_wrap(pos_track, true)
	for k in KEYS:
		var p := float(k) / KEYS
		var time := p * period
		for b in rot:
			# each turn about its axis as the base pose's bone frame sees it, applied in order
			var gb: Basis = (glob[b] as Transform3D).basis.orthonormalized()
			var q := Quaternion((local[b] as Transform3D).basis.orthonormalized())
			for e in rot[b]:
				var ax: Vector3 = (gb.inverse() * (e[0] as Vector3)).normalized()
				q = q * Quaternion(ax, deg_to_rad(float((e[1] as Callable).call(p))))
			a.rotation_track_insert_key(tracks[b], time, q.normalized())
		if pos_track >= 0:
			# centre of mass: lowest in double support (0, 50%), highest at midstance; over the stance leg
			var up := -0.021 * s * cos(2.0 * TAU * p)
			var side := 0.02 * s * sin(TAU * p)
			var parent := sk.get_bone_parent(pelvis)
			var to_parent: Basis = (glob[parent] as Transform3D).basis.inverse() if parent >= 0 else Basis()
			a.position_track_insert_key(pos_track, time, (local[pelvis] as Transform3D).origin + to_parent * Vector3(side, up, 0.0))
	return a


## The skeleton's path as `base`'s tracks name it (so the walk plays from the same AnimationPlayer).
static func _skeleton_path(base: Animation) -> String:
	if base == null:
		return "Skeleton3D"
	for t in base.get_track_count():
		var path := base.track_get_path(t)
		if path.get_subname_count() > 0:
			return String(path.get_concatenated_names())
	return "Skeleton3D"


## The stride period (s) for a walking speed (m/s) and stature (m): step length about 0.41 of stature at a
## natural walk (Grieve and Gear 1966), so the clip's feet keep up with the body.
static func period_for(speed: float, stature := 1.75) -> float:
	var step := 0.41 * stature * clampf(0.6 + 0.35 * speed, 0.75, 1.15)
	return 2.0 * step / maxf(speed, 0.2)


## How far the stance foot travels backward under the hips in one step of `walk` (m, along +Z), from its own
## keys: forward kinematics of pelvis, thigh, calf and foot at the left heel strike and toe-off of the right
## (0% and 50%). Play the clip at speed_scale = speed * walk.length / (2 * step_length) and the planted feet
## keep up with the body exactly.
static func step_length(walk: Animation, sk: Skeleton3D, names: Dictionary) -> float:
	var chain := []
	for role in ["thigh_l", "calf_l", "foot_l"]:
		var b := sk.find_bone(String(names.get(role, "")))
		if b < 0:
			return 0.0
		chain.append(b)
	var z := []
	for p: float in [0.0, 0.5]:
		var t: float = p * walk.length
		var g := Transform3D()
		var b: int = chain[0]
		# the thigh's parents as the walk (or the rest) has them
		var ups := []
		var q := sk.get_bone_parent(b)
		while q >= 0:
			ups.push_front(q)
			q = sk.get_bone_parent(q)
		for bb: int in ups + chain:
			var x: Transform3D = sk.get_bone_rest(bb)
			for tr in walk.get_track_count():
				var path := walk.track_get_path(tr)
				if path.get_concatenated_subnames() != sk.get_bone_name(bb):
					continue
				if walk.track_get_type(tr) == Animation.TYPE_ROTATION_3D:
					x.basis = Basis(walk.rotation_track_interpolate(tr, t)).scaled(x.basis.get_scale())
				elif walk.track_get_type(tr) == Animation.TYPE_POSITION_3D:
					x.origin = walk.position_track_interpolate(tr, t)
			g = g * x
		z.append(g.origin.z)
	return absf(float(z[0]) - float(z[1]))

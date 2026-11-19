extends Node3D
# A height source for GroundSurface3D (any node with get_height(position)).

var kind := "flat"
var noise := FastNoiseLite.new()


var swash_relief := FastNoiseLite.new()


func _init() -> void:
	noise.seed = 11
	noise.frequency = 0.02
	noise.fractal_octaves = 3
	swash_relief.seed = 23
	swash_relief.noise_type = FastNoiseLite.TYPE_PERLIN
	swash_relief.frequency = 0.4
	swash_relief.fractal_octaves = 3


func get_height(p: Vector3) -> float:
	match kind:
		"slope":
			# A 50 degree face rising toward +x, steeper than any repose angle here.
			return p.x * tan(deg_to_rad(50.0))
		"bank":
			# A 15 degree bank 4 m wide rising toward +x, flat above and below (mud round 10: mud slides).
			return clampf(p.x, -2.0, 2.0) * tan(deg_to_rad(15.0))
		"incline":
			# A plain 6 degree slope falling toward -x, flat below x = -5 (mud round 11: gullies in ruts).
			return maxf(p.x, -5.0) * tan(deg_to_rad(6.0))
		"beach":
			# A foreshore rising 1 in 12 toward +z out of still water at z = 0, with a little relief.
			return p.z / 12.0 + 0.03 * noise.get_noise_2d(p.x * 2.0, p.z * 2.0)
		"beach_terrace":
			# The same face falling onto a low-tide terrace: 1 in 12 above z = 2, 1 in 50 below, the break eased over
			# about 40 cm (the profile of a reflective face over a dissipative terrace, Wright and Short 1984). Rills
			# leaving the face slow on the terrace and lay their sand down as fans (Otvos 1999).
			var t := (p.z - 2.0) / 0.4
			var soft := 0.4 * (log(1.0 + exp(t)) if t < 30.0 else t) # softplus: max(z - 2, 0), eased
			# A real face is not a plane: the swash leaves horns and bays a few metres apart and several centimetres
			# deep, the bays running down the face (beach cusps, Komar 1998; Coco et al. 1999), and the bays gather the
			# seepage into a few trunk rills all the way down, which the small heads join (a plane gave an even comb
			# of parallel rills; relief as round as it is long gathered them only near the top).
			var relief := 0.07 * swash_relief.get_noise_2d(p.x, p.z * 0.35)
			return 2.0 / 12.0 + (p.z - 2.0 - soft) / 50.0 + soft / 12.0 + 0.03 * noise.get_noise_2d(p.x * 2.0, p.z * 2.0) + relief
		"dune":
			# A transverse dune's crest along z at x = 0, 3 m high: a 10 degree stoss slope rising from -x and a slip
			# face at 32 degrees (dry sand rests between its stopping and starting angles, 31 to 33) falling to flat
			# ground at x = 4.8, the toe eased over 30 cm.
			var lee := 3.0 - maxf(p.x, 0.0) * tan(deg_to_rad(32.0))
			var stoss := 3.0 + minf(p.x, 0.0) * tan(deg_to_rad(10.0))
			var top := minf(lee, stoss)
			var k := 0.3 * tan(deg_to_rad(32.0))
			return k * log(exp(top / k) + 1.0) if top < 30.0 * k else top # softplus: max(top, 0), eased
		"hollows":
			# Flat ground with three shallow dips 4 to 5 m across, 20 to 30 cm deep (where puddles gather).
			var h := 0.0
			for d in [Vector3(-4.0, 0.25, -3.0), Vector3(5.0, 0.3, 2.0), Vector3(-2.0, 0.2, 6.0)]:
				h -= d.y * exp(-(pow(p.x - d.x, 2.0) + pow(p.z - d.z, 2.0)) / 3.0)
			return h
		"valley":
			# A burnt channel falling 25% toward +z to the fan apex at z = 0, then a 4% fan (post-fire debris flow).
			var along := -0.25 * minf(p.z, 0.0) - 0.04 * maxf(p.z, 0.0)
			# Above the apex the channel is a gully cut 2.4 m into 15% valley sides, with 31 degree banks.
			var ax := absf(p.x)
			var bank := 0.6 * minf(ax, 4.0) + 0.15 * maxf(ax - 4.0, 0.0) if p.z < 0.0 else 0.02 * ax
			# The gully dies out over the 10 m above the apex.
			if p.z > -10.0 and p.z < 0.0:
				bank = lerpf(bank, 0.02 * ax, (p.z + 10.0) / 10.0)
			return along + bank + 0.15 * noise.get_noise_2d(p.x * 2.0, p.z * 2.0)
		"camber":
			# A 20 degree side slope rising toward +x, with a little roughness (mud round 13.3).
			return 0.364 * p.x + 0.02 * noise.get_noise_2d(p.x * 2.0, p.z * 2.0)
		"ford":
			# A stream bed 60 cm down over x < -1, a 30 degree bank up to a flat at 55 cm from x = 1 (mud round 13.2).
			return clampf((p.x + 1.0) * 0.577, 0.0, 1.155) - 0.6 + 0.01 * noise.get_noise_2d(p.x * 3.0, p.z * 3.0)
		"meadow":
			return 0.12 * noise.get_noise_2d(p.x * 3.0, p.z * 3.0) + 0.02 * p.x
		_:
			return 0.0

#[compute]
#version 450

// The print field on the GPU, for snow (docs/ideas/snow.md, follow-up 13) and bare ground: sand, soil, mud
// and litter (docs/ideas/sand.md, stage 0). The same maths as GroundDeformField and the owners' columns,
// slumping and relaxing (SnowSurface3D) or ageing (GroundSurface3D), on a field of (dent, rim, slumping
// owed, fresh) per texel (fresh: ground only). One shader, one pipeline: the push constant says which pass
// runs, params.kind.x which owner.

#define MODE_STAMPS 0
#define MODE_SLUMP_GIVE 1
#define MODE_SLUMP_TAKE 2
#define MODE_RELAX 3
#define MODE_BLIT 4
#define MODE_COLLISION 5
#define MODE_STORE 6
#define MODE_SHIFT 7
#define MODE_PACK 8
#define MODE_WATER_FLUX 9
#define MODE_WATER_DEPTH 10
#define MODE_WATER_COPY 11
#define MODE_WATER_ERODE 12
#define MODE_WATER_SED 13
#define MODE_DRIFT_GIVE 14
#define MODE_MUD_FLUX 15
#define MODE_MUD_DEPTH 16

#define STAMP_BOX 1u
#define STAMP_ROUND 2u
#define STAMP_HELD 4u
#define STAMP_BROKE 8u
#define STAMP_AMOUNT 16u // dig (sink > 0) or deposit (sink < 0) a set depth, no contact: sand stage 2

#define TILE 64
#define WG 256

layout(local_size_x = WG, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Push {
	int mode;
	int count; // stamps, or tile operations
	int pad0;
	int pad1;
}
push;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 res; // field n, tiles per side, ground lattice n, cover n
	vec4 field; // texel index x, z of texel (0, 0) (the field's origin), texel size, step seconds
	vec4 ground; // lattice index x, z of sample (0, 0), spacing, cover valid (1)
	vec4 cover; // cell index x, z of cell (0, 0), cell size, unused
	vec4 cover_rect; // the cover's map rect: x, z, w, h
	vec4 snow; // compacted density, rim share, min depth, edge fade
	vec4 mesh_rect; // x, z, w, h
	vec4 relax; // keep (wind and settling), refill share, press settle seconds, loose snow tan(repose)
	vec4 collision; // centre x, z, spacing, cells
	vec4 bow; // bow speed, gain, reach, heave zone in footing widths (GroundDeformField::rim_reach)
	vec4 crust_consts; // crack length, plate size, crack sectors, unused
	ivec4 shift; // texel offset of the old field in the new one's tiles: old tile index = new tile index + shift.xy
	vec4 kind; // 1 for the ground (else snow), slow slumping seconds this step, fresh kept by ageing, sand drift per hour
	vec4 age; // rain softening per hour, hours of ageing, distance from the field's edge where prints start, slump passes this step (0 = 1)
	vec4 water; // standing water (docs/ideas/mud.md, stage 2): rain m/s, substep s, flux damping per s, evaporation m/s
	vec4 water2; // seepage length m, compaction's share of the conductivity kept, conductivity scale, thinnest film kept m
	vec4 water3; // runoff channels (sand): on (1), morphological factor, bed roughness m, source landing factor
	vec4 wade; // tyres wading (mud stage 4c): count (up to 4), water discs, stirs, runoff morphological factor (round 11)
	vec4 wade_m[8]; // per tyre: (axle x, y, z, radius), (heading angle, unused, half width, speed m/s)
	vec4 water_disc[16]; // water added (+) or taken (-) this step (add_water, wade.y of them): x, z, radius, depth a pass m
	vec4 water_disc_mud[4]; // the clay each added disc's water carries, kg/m3 (disc k: [k / 4][k % 4])
	vec4 stir[8]; // clay stirred into standing water this step (stir_water, wade.z of them): x, z, radius, kg/m2 a pass
	vec4 turbid; // muddy water (mud round 3): on (1), settling m a substep second, tyre churn kg/m2, most kg/m3
	vec4 drift; // wind filling prints (drift_give()): wind direction x, z, Courant number this ageing, even fade's share kept
	vec4 mudwave; // waves in liquid mud (mud round 14): on (1), substep s, unused, unused
}
params;

layout(set = 0, binding = 1, std430) coherent restrict buffer StateA {
	vec4 state[];
};
layout(set = 0, binding = 2, std430) restrict buffer StateB {
	vec4 state_out[];
};
layout(set = 0, binding = 3, std430) restrict buffer Give {
	vec2 give[];
};
layout(set = 0, binding = 4, std430) restrict readonly buffer Ground {
	float ground_h[];
};
layout(set = 0, binding = 5, std430) restrict readonly buffer Cover {
	// Snow: per cover cell, two: (swe mm, density, crust thickness, crust burial), (new snow m, cover ground m, 0, 0).
	// Ground: per lattice point, four: (rim share, tan repose, slumping rate, deepest dent), (darken, gloss, blown sand,
	// deformable), (water inputs: GroundSurface3D::_gpu_cells), (mud slides: tau_y / (rho g) m, runoff's critical
	// bed shear Pa (round 11), 0, 0).
	vec4 cover_cells[];
};
struct Stamp {
	vec4 a; // position x, y, z, radius
	vec4 b; // half x, half z, yaw, bottom
	vec4 c; // sink, contact tolerance, velocity x, velocity z
	vec4 d; // crack length, flags, seed (bits), unused
	ivec4 e; // x0, x1, z0, z1
	vec4 f; // a hard body under a press (mud round 15.2): capsule end a xyz, radius (0: none)
	vec4 g; // its other end b xyz, unused
};
layout(set = 0, binding = 6, std430) restrict readonly buffer Stamps {
	Stamp stamps[];
};
layout(set = 0, binding = 7, rgba16f) uniform restrict writeonly image2D out_tex;
layout(set = 0, binding = 8, std430) restrict writeonly buffer Collision {
	float collision_h[];
};
layout(set = 0, binding = 9, std430) restrict buffer Cache {
	uint cache[]; // per slot TILE x TILE x cache_stride(): packHalf2x16(dent, rim), then (fresh, water depth), (clay kg/m2, 0)
};
layout(set = 0, binding = 10, std430) restrict readonly buffer TileOps {
	ivec4 tile_ops[]; // store: (tile x, tile z, slot, 0); shift: per new tile (source kind, index, 0, 0)
};
layout(set = 0, binding = 11, std430) restrict writeonly buffer Packed {
	uint packed[]; // per texel stride() uints, then for the ground two more each: (water depth, bed cut), (clay kg/m3, 0)
};
// Standing water (ground only): per texel the outflows to -x, +x, -z, +z (m/s: m3/s over the texel's area) and
// (depth m, bed load along x m2/s, bed cut by the runoff m, bed load along z m2/s). The virtual pipe model (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007).
// Then the clay it carries (mud round 3): (kg/m2 over the texel, its concentration kg/m3 as the substep began, the share
// of the sward grown back over the texel's rut since it was last pressed (mud round 6: set only by the kept ruts'
// ageing, 0..1), 0).
struct WaterCell {
	vec4 flux;
	vec4 w;
	vec4 s;
};
layout(set = 0, binding = 12, std430) restrict buffer Water {
	WaterCell water[];
};
layout(set = 0, binding = 13, std430) restrict buffer WaterTmp {
	float water_tmp[]; // the shifted depth, copied back by MODE_WATER_COPY; then the faces' discharges (water_face()); then the shifted clay; then the shifted regrowth; then the runoff's soil concentration (water_susp())
};
// Waves in liquid mud (ground only): per texel the mud layer's unit discharges through its +x and +z faces, m2/s,
// kept from step to step (xy), and the new ones of this substep (zw) (mud_flux()).
layout(set = 0, binding = 15, std430) restrict buffer MudFlow {
	vec4 mudf[];
};
layout(set = 0, binding = 14, std430) restrict buffer Ledger {
	uint ledger[4]; // add_water's discs, mm3 (cumulative): [0] takes could not find, [1] added, [2] taken
};

const float SWE_MIN = 0.01;

// Mud tracked onto firm ground (docs/ideas/mud.md, round 3): a texel's fresh value from 4 is a laid film of clay,
// 4 + its share (sixteenths, the bin) + 0.06 x its wetness (GroundDeformField::mud_band()): dark and glossy wet,
// a pale matte film once dry, and it stays until a wash takes it.
float mud_band(float p_share, float p_wet) {
	float b = clamp(round(p_share * 16.0) - 1.0, 0.0, 15.0);
	return 4.0 + b / 16.0 + 0.06 * clamp(p_wet, 0.0, 1.0);
}
vec2 mud_band_share_wet(float p_w) {
	float v = p_w - 4.0;
	float b = floor(v * 16.0 + 1e-4);
	return vec2((b + 1.0) / 16.0, clamp((v - b / 16.0) / 0.06, 0.0, 1.0));
}

bool is_ground() {
	return params.kind.x > 0.5;
}

// uints per texel in the cache and the read back: (dent, rim), then (fresh, 0) for the ground.
int stride() {
	return is_ground() ? 2 : 1;
}
// uints per texel in the tile cache (GroundDeformGPU::cache_uints): the ground's third keeps its water's clay (mud round 5).
int cache_stride() {
	return is_ground() ? 3 : 1;
}

// ---------------------------------------------------------------------------------------------------------
// Columns (SnowSurface3D::SnowColumns and SnowCover3D's lookups)

int fn() {
	return params.res.x;
}
float texel() {
	return params.field.z;
}
vec2 texel_world(int x, int z) {
	return (params.field.xy + vec2(x, z) + 0.5) * texel();
}

float ground_at(vec2 p) {
	int n = params.res.z;
	if (n < 2) {
		return 0.0;
	}
	float fx = clamp(p.x / params.ground.z - params.ground.x, 0.0, float(n - 1));
	float fz = clamp(p.y / params.ground.z - params.ground.y, 0.0, float(n - 1));
	int x0 = min(int(fx), n - 2);
	int z0 = min(int(fz), n - 2);
	float tx = fx - float(x0);
	float tz = fz - float(z0);
	float a = mix(ground_h[z0 * n + x0], ground_h[z0 * n + x0 + 1], tx);
	float b = mix(ground_h[(z0 + 1) * n + x0], ground_h[(z0 + 1) * n + x0 + 1], tx);
	return mix(a, b, tz);
}

// The cover's bilinear sample (SnowCover3D::_sample): two vec4s per cell.
vec4 cover_sample(vec2 p, int part) {
	int n = params.res.w;
	if (n < 2 || params.ground.w < 0.5) {
		return vec4(0.0);
	}
	float cs = params.cover.z;
	float fx = clamp(p.x / cs - params.cover.x - 0.5, 0.0, float(n - 1));
	float fz = clamp(p.y / cs - params.cover.y - 0.5, 0.0, float(n - 1));
	int x0 = min(int(fx), n - 2);
	int z0 = min(int(fz), n - 2);
	float tx = fx - float(x0);
	float tz = fz - float(z0);
	vec4 a = mix(cover_cells[(z0 * n + x0) * 2 + part], cover_cells[(z0 * n + x0 + 1) * 2 + part], tx);
	vec4 b = mix(cover_cells[((z0 + 1) * n + x0) * 2 + part], cover_cells[((z0 + 1) * n + x0 + 1) * 2 + part], tx);
	return mix(a, b, tz);
}

bool in_cover_rect(vec2 p) {
	vec4 r = params.cover_rect;
	return p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w;
}

// (depth m, density, crust thickness, crust burial)
vec4 pack_at(vec2 p) {
	vec4 c = cover_sample(p, 0);
	float swe = in_cover_rect(p) ? max(c.x, 0.0) : 0.0;
	float depth = swe > SWE_MIN ? swe / max(c.y, 30.0) : 0.0;
	return vec4(depth, c.y, c.z, c.w);
}

float dent_max(float depth, float density) {
	float r = density / params.snow.x;
	if (r >= 1.0 || depth <= 0.0) {
		return 0.0;
	}
	return depth * (1.0 - r) / max(1.0 - params.snow.y * r, 1e-3);
}

struct Column {
	float surface;
	float max_dent;
	float rim_share;
	float crust;
	bool deformable;
};

// GroundSurface3D::_cell_at: the nearest lattice point's material.
vec4 ground_cell(vec2 p, int part) {
	int n = params.res.w;
	if (n < 1) {
		return vec4(0.0);
	}
	int x = int(round(p.x / params.ground.z)) - int(params.ground.x);
	int z = int(round(p.y / params.ground.z)) - int(params.ground.y);
	if (x < 0 || z < 0 || x >= n || z >= n) {
		return vec4(0.0);
	}
	return cover_cells[(z * n + x) * 4 + part];
}

// Hidden stones (mud round 16.1): after the ground's cells, a header (count) and per stone two vec4s (end a xyz,
// radius; end b xyz). The depth below the undisturbed ground p_g to the highest stone top over p, 1e3 where none.
float stone_dent(vec2 p, float p_g) {
	int n = params.res.w;
	int base = n * n * 4;
	int count = int(cover_cells[base].x);
	float best = 1e3;
	for (int k = 0; k < count; k++) {
		vec4 a = cover_cells[base + 1 + 2 * k];
		vec4 b = cover_cells[base + 2 + 2 * k];
		vec2 lo = min(a.xz, b.xz) - a.w;
		vec2 hi = max(a.xz, b.xz) + a.w;
		if (p.x < lo.x || p.y < lo.y || p.x > hi.x || p.y > hi.y) {
			continue;
		}
		vec2 ab = b.xz - a.xz;
		float l2 = dot(ab, ab);
		float t = l2 > 1e-8 ? clamp(dot(p - a.xz, ab) / l2, 0.0, 1.0) : 0.0;
		vec2 q = a.xz + ab * t - p;
		float d2 = dot(q, q);
		if (d2 < a.w * a.w) {
			best = min(best, max(p_g - (mix(a.y, b.y, t) + sqrt(a.w * a.w - d2)), 0.0));
		}
	}
	return best;
}

bool ground_deformable(vec2 p) {
	if (ground_cell(p, 1).w < 0.5) {
		return false;
	}
	// Only where the terrain is fully lowered under the patch.
	vec4 r = params.mesh_rect;
	float edge = min(min(p.x - r.x, r.x + r.z - p.x), min(p.y - r.y, r.y + r.w - p.y));
	return edge >= params.age.z;
}

Column column_at(vec2 p, uint flags) {
	Column c;
	c.surface = 0.0;
	c.max_dent = 0.0;
	c.rim_share = 0.3;
	c.crust = 0.0;
	c.deformable = false;
	if (is_ground()) {
		if (!ground_deformable(p)) {
			return c;
		}
		vec4 m = ground_cell(p, 0);
		c.deformable = true;
		c.surface = ground_at(p);
		c.max_dent = m.w;
		c.rim_share = m.x;
		return c;
	}
	vec4 pk = pack_at(p);
	float depth = pk.x;
	if (depth < params.snow.z) {
		return c;
	}
	c.deformable = true;
	c.surface = ground_at(p) + depth;
	float density = pk.y;
	c.max_dent = dent_max(depth, density);
	c.rim_share = params.snow.y;
	if ((flags & (STAMP_HELD | STAMP_BROKE)) != 0u) {
		float thickness = pk.z;
		float over = pk.w;
		if (thickness > 0.0 && over < depth) {
			if ((flags & STAMP_HELD) != 0u) {
				c.max_dent = min(c.max_dent, dent_max(over, density));
			} else if (over < 0.05) {
				c.crust = thickness;
			}
		}
	}
	return c;
}

// ---------------------------------------------------------------------------------------------------------
// Pressing (GroundDeformField::press), one stamp after another in one workgroup.

uint murmur3(uint p_in, uint p_seed) {
	p_in *= 0xcc9e2d51u;
	p_in = (p_in << 15) | (p_in >> 17);
	p_in *= 0x1b873593u;
	p_seed ^= p_in;
	p_seed = (p_seed << 13) | (p_seed >> 19);
	p_seed = p_seed * 5u + 0xe6546b64u;
	return p_seed;
}

shared float red_a[WG];
shared float red_b[WG];
shared float red_c[WG];
shared uint touch_flag;

// Sums (or maxes, for c) the three per-thread values over the workgroup, in a fixed order.
vec3 reduce3(float a, float b, float c) {
	uint t = gl_LocalInvocationIndex;
	red_a[t] = a;
	red_b[t] = b;
	red_c[t] = c;
	barrier();
	for (uint s = WG / 2; s > 0; s >>= 1) {
		if (t < s) {
			red_a[t] += red_a[t + s];
			red_b[t] += red_b[t + s];
			red_c[t] = max(red_c[t], red_c[t + s]);
		}
		barrier();
	}
	vec3 r = vec3(red_a[0], red_b[0], red_c[0]);
	barrier();
	return r;
}

// Grains leaving the field (dig, depth > 0) or landing on it (deposit, depth < 0): the set depth over the
// disc, rim first when digging, dent first when depositing, the dent within the deepest. Exact volume except
// where a dig reaches the deepest dent.
float mud_print_mask(vec4 p_b, vec2 p_wp, vec2 p_pos); // below, with the sole

void run_amount_stamp(Stamp s) {
	uint t = gl_LocalInvocationIndex;
	int n = fn();
	float tx = texel();
	vec3 pos = s.a.xyz;
	float radius = s.a.w;
	float depth = s.c.x;
	bool ground = is_ground();
	float settle = ground ? s.d.w : params.relax.z;
	int x0 = s.e.x;
	int z0 = s.e.z;
	int w = s.e.y - x0 + 1;
	int h = s.e.w - z0 + 1;
	if (w <= 0 || h <= 0) {
		return;
	}
	for (int k = int(t); k < w * h; k += WG) {
		int x = x0 + k % w;
		int z = z0 + k / w;
		vec2 wp = texel_world(x, z);
		// GroundDeformField::amount_cover(): a disc antialiased over a texel, or (c.y) a bilinear tent.
		vec2 dd = wp - pos.xz;
		float cover = s.c.y > 0.5 ? max(1.0 - abs(dd.x) / radius, 0.0) * max(1.0 - abs(dd.y) / radius, 0.0) : clamp(0.5 - (length(dd) - radius) / tx, 0.0, 1.0);
		if (cover == 0.0) {
			continue;
		}
		Column c = column_at(wp, 0u);
		if (!c.deformable) {
			continue;
		}
		int i = z * n + x;
		vec4 st = state[i];
		float sa = depth * cover;
		float a = abs(sa);
		if (sa > 0.0 && s.c.z < 0.0) {
			// A line being washed away: what was laid on the surface goes, and its tint with it; the swash that
			// spread it had also filled the hollow beside it, so with -2 (a swash line) that is levelled too (a
			// smeared line is a ghost).
			st.y -= min(st.y, a);
			if (s.c.z < -1.5) {
				st.x -= min(st.x, a);
			}
			if (ground) {
				if (st.w >= 4.0) {
					vec2 sq = mud_band_share_wet(st.w);
					float left = sq.x * (1.0 - cover);
					st.w = left > 1.0 / 32.0 ? mud_band(left, sq.y) : 0.0;
				} else {
					st.w = st.w >= 2.0 ? 2.0 + (st.w - 2.0) * (1.0 - cover) : st.w * (1.0 - cover);
				}
			}
		} else if (sa > 0.0) {
			float take = min(st.y, a);
			st.y -= take;
			st.x = min(st.x + a - take, c.max_dent);
			if (ground) {
				// Turned over by its depth: a skim of a millimetre or two (an avalanche's margin, a tent's tail)
				// barely shows; 3 mm or more is fresh ground (GroundDeformField::add_amount()).
				float f = min(a / 0.003, 1.0) * (s.c.z > 0.0 && s.c.z < 1.0 ? s.c.z : 1.0); // 0..1: a dig's tint scale
				st.w = st.w >= 2.0 ? (f >= 1.0 ? 1.0 : st.w) : max(st.w, f);
				if (f >= 0.5) {
					water[i].s.z = 0.0; // dug again: what had grown back over it is gone (mud round 6)
				}
			}
		} else {
			float fill = min(st.x, a);
			st.x -= fill;
			st.y += a - fill;
			if (ground && s.c.z >= 4.0) {
				// Tracked mud: the film's share, the wetter of the two where it lands on another.
				vec2 sq = mud_band_share_wet(s.c.z);
				float laid = sq.x * cover * mud_print_mask(s.b, wp, pos.xz);
				if (st.w >= 4.0) {
					vec2 o = mud_band_share_wet(st.w);
					st.w = mud_band(max(o.x, laid), max(o.y, sq.y));
				} else if (laid > 1.0 / 32.0) {
					st.w = mud_band(laid, sq.y);
				}
			} else if (ground && s.c.z >= 2.0) {
				// A deposit of a different sand (a swash mark's heavy minerals): 2 plus its share, and it stays.
				st.w = max(st.w, 2.0 + (s.c.z - 2.0) * cover);
			} else if (ground && s.c.z > 0.0) {
				st.w = max(st.w, s.c.z * cover); // a marked deposit, tinted fresh
			}
		}
		st.z = max(st.z, settle);
		state[i] = st;
	}
	memoryBarrierBuffer();
	barrier();
}

// A tyre's tread in its rut (docs/ideas/mud.md, round 10; GroundDeformField::tread_relief()): how far the print's
// floor stands above (voids the soil squeezed into) or below (chain cross links) the plain box, m. p_code: 0 none,
// 1 road (circumferential grooves stand as low ribs), 2 all terrain (chevron lugs, 9 cm), 3 mud terrain (staggered
// open lugs, 13 cm, deep voids), 4 chains (cross links every 22 cm over all terrain lugs). The pattern is fixed on
// the ground along the wheel's heading so the frames of a rolling tyre agree, warped by noise at about 1 m and
// 25 cm so a long rut does not read as a repeated stamp; a slipping tread smears it out.
float runoff_hash(ivec2 c) {
	uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ 0x5bd1e995u;
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return float(h & 0xffffu) / 65535.0;
}
float runoff_noise(vec2 q, int salt) {
	ivec2 c = ivec2(floor(q)) + ivec2(salt * 7919, salt * 104729);
	vec2 f = q - floor(q);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(runoff_hash(c), runoff_hash(c + ivec2(1, 0)), f.x);
	float b = mix(runoff_hash(c + ivec2(0, 1)), runoff_hash(c + ivec2(1, 1)), f.x);
	return mix(a, b, f.y) - 0.5;
}
float tread_relief(uint p_code, vec2 p_wp, vec2 p_lp, vec2 p_half, float p_cy, float p_sy, float p_slip) {
	if (p_code == 0u) {
		return 0.0;
	}
	float a = p_sy * p_wp.x + p_cy * p_wp.y + 0.03 * runoff_noise(p_wp / 0.9, 5) + 0.008 * runoff_noise(p_wp / 0.25, 6);
	float x = p_lp.x / max(p_half.x, 1e-3);
	float keep = (1.0 - smoothstep(0.6, 3.0, p_slip)) * (1.0 - smoothstep(0.85, 1.0, abs(x)));
	float r = 0.0;
	if (p_code == 1u) {
		float d = abs(fract(2.0 * x + 0.5) - 0.5);
		r = abs(x) < 0.75 ? 0.004 * (1.0 - smoothstep(0.04, 0.08, d)) : 0.0;
	} else if (p_code == 3u) {
		float g = fract((a + 0.35 * abs(p_lp.x)) / 0.13 + (x < 0.0 ? 0.0 : 0.5));
		float bar = smoothstep(0.0, 0.05, g) * (1.0 - smoothstep(0.4, 0.45, g));
		r = 0.018 * (1.0 - bar);
	} else {
		float g = fract((a + 0.6 * abs(p_lp.x)) / 0.09);
		float bar = smoothstep(0.0, 0.06, g) * (1.0 - smoothstep(0.44, 0.5, g));
		r = (p_code == 4u ? 0.005 : 0.01) * (1.0 - bar);
		if (p_code == 4u) {
			// The cross chains bear first and press their ladder in, the tread's lugs fainter between.
			float link = abs(fract(a / 0.22) - 0.5) * 0.22;
			r = mix(r, -0.015, 1.0 - smoothstep(0.008, 0.016, link));
		}
	}
	return r * keep;
}

// A boot's sole (GroundDeformField::sole_sd(), sole_relief()): a box stamp flagged round as well. Local p in the box's
// frame, the toe toward + along its longer side. The outline: a heel and a forefoot ellipse joined by a smooth
// union through the waist; the relief raises the print's floor where the sole does not bear: the shank between
// heel and ball, and the grooves between chevron lugs (a few millimetres), with a solid rand round the edge.
float sole_ellipse(vec2 p, vec2 r) {
	return (length(p / r) - 1.0) * min(r.x, r.y);
}
vec2 sole_frame(vec2 p_local, vec2 p_half, out float r_len, out float r_wid) {
	bool along_x = p_half.x > p_half.y;
	r_len = along_x ? p_half.x : p_half.y;
	r_wid = along_x ? p_half.y : p_half.x;
	return along_x ? vec2(p_local.x, p_local.y) : vec2(p_local.y, p_local.x);
}
float sole_sd(vec2 p_local, vec2 p_half) {
	float len;
	float wid;
	vec2 q = sole_frame(p_local, p_half, len, wid);
	float heel = sole_ellipse(vec2(q.x + 0.64 * len, q.y), vec2(0.36 * len, 0.74 * wid));
	float fore = sole_ellipse(vec2(q.x - 0.28 * len, q.y), vec2(0.72 * len, wid));
	float k = 0.12 * wid;
	float hh = clamp(0.5 + 0.5 * (fore - heel) / k, 0.0, 1.0);
	return mix(fore, heel, hh) - k * hh * (1.0 - hh);
}
float sole_relief(vec2 p_local, vec2 p_half) {
	float len;
	float wid;
	vec2 q = sole_frame(p_local, p_half, len, wid);
	float u = q.x / len;
	float shank = smoothstep(-0.4, -0.32, u) * (1.0 - smoothstep(-0.12, -0.04, u));
	float rand = smoothstep(-0.008, -0.004, sole_sd(p_local, p_half));
	float g = fract((q.x + 0.6 * abs(q.y)) / 0.045);
	float groove = (1.0 - rand) * smoothstep(0.0, 0.12, g) * (1.0 - smoothstep(0.5, 0.62, g));
	return max(0.012 * shank, 0.007 * groove);
}

// Tracked mud's print (mud round 4, GroundDeformField::mud_print_mask()): the share of a film laid round p_pos that
// a tread or sole leaves at p_wp. p_b.x: 0 a plain disc; 1 a tyre's chevron lugs, 45 mm bars 9 cm apart across its
// heading (p_b.y the yaw: heading (cos, -sin)), fixed on the ground so the frames of a rolling tyre agree; 2 a sole
// (yaw p_b.y, half extents p_b.zw): what its lugs and rand bear, not the shank or grooves.
float mud_print_mask(vec4 p_b, vec2 p_wp, vec2 p_pos) {
	if (p_b.x < 0.5) {
		return 1.0;
	}
	float cy = cos(p_b.y);
	float sy = sin(p_b.y);
	vec2 d = p_wp - p_pos;
	vec2 lp = vec2(cy * d.x - sy * d.y, sy * d.x + cy * d.y);
	if (p_b.x < 1.5) {
		float u = dot(p_wp, vec2(cy, -sy));
		float g = fract((u + 0.6 * abs(lp.y)) / 0.09);
		float bar = smoothstep(0.0, 0.06, g) * (1.0 - smoothstep(0.44, 0.5, g));
		return mix(0.15, 1.0, bar);
	}
	float inside = 1.0 - smoothstep(-0.003, 0.003, sole_sd(lp, p_b.zw));
	return inside * (1.0 - smoothstep(0.002, 0.005, sole_relief(lp, p_b.zw)));
}

// A capsule's top over a point (GroundDeformField::floor_top()), or -1e9 off it.
float floor_top(vec4 a, vec3 b, vec2 p) {
	if (a.w <= 0.0) {
		return -1e9;
	}
	vec2 ab = b.xz - a.xz;
	float l2 = dot(ab, ab);
	float t = l2 > 1e-8 ? clamp(dot(p - a.xz, ab) / l2, 0.0, 1.0) : 0.0;
	vec2 q = a.xz + ab * t - p;
	float d2 = dot(q, q);
	return d2 < a.w * a.w ? mix(a.y, b.y, t) + sqrt(a.w * a.w - d2) : -1e9;
}

void run_stamp(Stamp s) {
	if ((uint(s.d.y) & STAMP_AMOUNT) != 0u) {
		run_amount_stamp(s);
		return;
	}
	uint t = gl_LocalInvocationIndex;
	int n = fn();
	float tx = texel();
	float area = tx * tx;
	vec3 pos = s.a.xyz;
	float radius = s.a.w;
	vec2 half_ = s.b.xy;
	float yaw = s.b.z;
	float bottom = s.b.w;
	float sink = s.c.x;
	float tol = s.c.y;
	vec2 vel = s.c.zw;
	float crack = s.d.x;
	uint flags = uint(s.d.y);
	uint seed = floatBitsToUint(s.d.z);
	bool ground = is_ground();
	float settle = ground ? s.d.w : params.relax.z; // seconds of slumping a pressed texel owes
	bool box = (flags & STAMP_BOX) != 0u;
	bool round_ = (flags & STAMP_ROUND) != 0u;
	// A box flagged round is a boot's sole.
	bool sole = box && round_;
	uint tread = (flags >> 5u) & 7u; // a wheel's tread (round 10)
	if (sole) {
		round_ = false;
	}
	int x0 = s.e.x;
	int x1 = s.e.y;
	int z0 = s.e.z;
	int z1 = s.e.w;
	int w = x1 - x0 + 1;
	int h = z1 - z0 + 1;
	if (w <= 0 || h <= 0) {
		return;
	}
	int count = w * h;
	float r2 = radius * radius;
	float cy = cos(yaw);
	float sy = sin(yaw);
	float ring = max(0.5 * (box ? sqrt(half_.x * half_.y) : radius), params.bow.w * (box ? 2.0 * min(half_.x, half_.y) : radius));
	float speed = length(vel);
	float bow = clamp(speed / params.bow.x, 0.0, 1.0);
	vec2 heading = speed > 1e-4 ? vel / speed : vec2(0.0);

	// Contact.
	if (t == 0u) {
		touch_flag = 0u;
	}
	barrier();
	for (int k = int(t); k < count; k += WG) {
		int x = x0 + k % w;
		int z = z0 + k / w;
		vec2 wp = texel_world(x, z);
		vec2 d = wp - pos.xz;
		float d2 = dot(d, d);
		float sd;
		vec2 lp = vec2(cy * d.x - sy * d.y, sy * d.x + cy * d.y);
		if (sole) {
			sd = sole_sd(lp, half_);
		} else if (box) {
			float qx = abs(lp.x) - half_.x;
			float qz = abs(lp.y) - half_.y;
			sd = sqrt(max(qx, 0.0) * max(qx, 0.0) + max(qz, 0.0) * max(qz, 0.0)) + min(max(qx, qz), 0.0);
		} else {
			sd = sqrt(d2) - radius;
		}
		float cover = clamp(0.5 - 2.0 * sd / tx, 0.0, 1.0);
		if (cover <= 0.0) {
			continue;
		}
		Column c = column_at(wp, flags);
		if (!c.deformable) {
			continue;
		}
		int i = z * n + x;
		float prof = round_ ? bottom + radius - sqrt(max(r2 - d2, 0.0)) : bottom;
		if (sole) {
			prof += sole_relief(lp, half_);
		} else if (box) {
			prof += tread_relief(tread, wp, lp, half_, cy, sy, speed);
		}
		if (prof <= c.surface - state[i].x + state[i].y + tol) {
			atomicOr(touch_flag, 1u);
		}
	}
	barrier();
	if (touch_flag == 0u) {
		return;
	}

	// Press.
	float moved = 0.0;
	float pushed = 0.0;
	float deepest = 0.0;
	for (int k = int(t); k < count; k += WG) {
		int x = x0 + k % w;
		int z = z0 + k / w;
		vec2 wp = texel_world(x, z);
		vec2 d = wp - pos.xz;
		float d2 = dot(d, d);
		float sd;
		vec2 lp = vec2(cy * d.x - sy * d.y, sy * d.x + cy * d.y);
		if (sole) {
			sd = sole_sd(lp, half_);
		} else if (box) {
			float qx = abs(lp.x) - half_.x;
			float qz = abs(lp.y) - half_.y;
			sd = sqrt(max(qx, 0.0) * max(qx, 0.0) + max(qz, 0.0) * max(qz, 0.0)) + min(max(qx, qz), 0.0);
		} else {
			sd = sqrt(d2) - radius;
		}
		float cover = clamp(0.5 - 2.0 * sd / tx, 0.0, 1.0);
		if (cover <= 0.0) {
			continue;
		}
		Column c = column_at(wp, flags);
		if (!c.deformable) {
			continue;
		}
		int i = z * n + x;
		vec4 st = state[i];
		float sfc = c.surface - st.x + st.y;
		float dd = min(d2, r2);
		float target = round_ ? bottom + radius - sqrt(max(r2 - dd, 0.0)) : bottom;
		float relief = sole ? sole_relief(lp, half_) : (box ? tread_relief(tread, wp, lp, half_, cy, sy, speed) : 0.0);
		target += relief;
		if (sink > 0.0) {
			float wgt = round_ ? sqrt(max(1.0 - d2 / r2, 0.0)) : 1.0;
			target = min(target, c.surface - sink * wgt + relief);
		}
		target = max(target, c.surface - c.max_dent);
		target = max(target, floor_top(s.f, s.g.xyz, wp)); // a stone under it (round 15.2)
		if (target >= sfc) {
			continue;
		}
		float delta = (sfc - target) * cover;
		deepest = max(deepest, c.surface - target);
		float take = min(st.y, delta);
		st.y -= take;
		st.x += delta - take;
		st.z = max(st.z, settle);
		if (ground && st.w < 4.0) { // freshly turned, under a tracked-mud film that stays on top
			st.w = 1.0;
		}
		if (ground) {
			water[i].s.z = 0.0; // pressed again: what had grown back over it is gone (mud round 6)
		}
		state[i] = st;
		moved += delta * area;
		pushed += ((delta - take) * c.rim_share + take * mix(c.rim_share, 1.0, bow)) * area;
	}
	memoryBarrierBuffer();
	vec3 sums = reduce3(moved, pushed, deepest);
	if (sums.x <= 0.0) {
		return;
	}
	pushed = sums.y;
	deepest = sums.z;

	// The broken crust.
	if (crack > 0.0 && deepest > 0.0) {
		float drop_edge = min(0.5 * deepest, 0.5 * crack);
		for (int k = int(t); k < count; k += WG) {
			int x = x0 + k % w;
			int z = z0 + k / w;
			vec2 wp = texel_world(x, z);
			vec2 d = wp - pos.xz;
			float o;
			if (sole) {
				o = max(sole_sd(vec2(cy * d.x - sy * d.y, sy * d.x + cy * d.y), half_), 0.0);
			} else if (box) {
				float lx = cy * d.x - sy * d.y;
				float lz = sy * d.x + cy * d.y;
				float ox = max(abs(lx) - half_.x, 0.0);
				float oz = max(abs(lz) - half_.y, 0.0);
				o = sqrt(ox * ox + oz * oz);
			} else {
				o = max(length(d) - radius, 0.0);
			}
			if (o <= 0.0) {
				continue;
			}
			float a = atan(d.y, d.x) + 3.14159265358979;
			int sectors = int(params.crust_consts.z);
			int sector = int(a / 6.28318530717959 * float(sectors)) % sectors;
			uint hs = murmur3(uint(sector), seed);
			float reach = crack * (0.5 + 1.1 * float(hs & 0xffffu) / 65535.0);
			if (o >= reach) {
				continue;
			}
			Column c = column_at(wp, flags);
			if (!c.deformable || c.crust <= 0.0) {
				continue;
			}
			int i = z * n + x;
			vec4 st = state[i];
			float target = max(c.surface - drop_edge * (1.0 - o / reach), c.surface - c.max_dent);
			float sfc = c.surface - st.x + st.y;
			if (target >= sfc) {
				continue;
			}
			float delta = sfc - target;
			float take = min(st.y, delta);
			st.y -= take;
			st.x += delta - take;
			st.z = max(st.z, settle);
			if (ground && st.w < 4.0) { // freshly turned, under a tracked-mud film that stays on top
				st.w = 1.0;
			}
			if (ground) {
				water[i].s.z = 0.0; // pressed again: what had grown back over it is gone (mud round 6)
			}
			state[i] = st;
		}
		memoryBarrierBuffer();
		barrier();
	}

	// The rim: the pushed share spread over the ring, thickest at the edge. A moving body sends half to each
	// side of its path (GroundDeformField::press()), as a tyre or blade throws up two even berms.
	if (pushed <= 0.0) {
		return;
	}
	float fl = 0.0;
	float fr = 0.0;
	for (int pass = 0; pass < 2; pass++) {
		float local_sum = 0.0;
		float local_sum_r = 0.0;
		for (int k = int(t); k < count; k += WG) {
			int x = x0 + k % w;
			int z = z0 + k / w;
			vec2 wp = texel_world(x, z);
			vec2 d = wp - pos.xz;
			float d2 = dot(d, d);
			float o;
			if (sole) {
				o = max(sole_sd(vec2(cy * d.x - sy * d.y, sy * d.x + cy * d.y), half_), 0.0);
			} else if (box) {
				float lx = cy * d.x - sy * d.y;
				float lz = sy * d.x + cy * d.y;
				float ox = max(abs(lx) - half_.x, 0.0);
				float oz = max(abs(lz) - half_.y, 0.0);
				o = sqrt(ox * ox + oz * oz);
			} else {
				o = max(sqrt(d2) - radius, 0.0);
			}
			float ahead = 0.0;
			if (bow > 0.0 && d2 > 1e-8) {
				ahead = max(dot(d, heading) / sqrt(d2), 0.0) * bow;
			}
			float reach = ring * (1.0 + params.bow.z * ahead);
			if (o <= 0.0 || o >= reach) {
				continue;
			}
			Column c = column_at(wp, flags);
			if (!c.deformable) {
				continue;
			}
			// The ground's lip is rounded (GroundDeformField::press()): highest a little out from the wall.
			float lip = params.bow.w > 0.0 ? 0.35 + 0.65 * smoothstep(0.0, 0.3, o / reach) : 1.0;
			float wgt = (1.0 - o / reach) * lip * (1.0 + params.bow.y * ahead);
			if (c.crust > 0.0) {
				float plate_size = params.crust_consts.y;
				int px = int(floor(wp.x / plate_size));
				int pz = int(floor(wp.y / plate_size));
				uint hsh = murmur3(uint(px) * 73856093u ^ uint(pz) * 19349663u, 0x7F07C65u);
				float hgt = float(hsh & 0xffffu) / 65535.0;
				float tilt = float((hsh >> 16) & 0xffu) / 127.5 - 1.0;
				bool along_x = ((hsh >> 24) & 1u) != 0u;
				float along = along_x ? (wp.x - (float(px) + 0.5) * plate_size) : (wp.y - (float(pz) + 0.5) * plate_size);
				float plate = max(0.1 + 1.8 * hgt + tilt * along / plate_size, 0.0);
				wgt *= 1.0 + min(c.crust / 0.02, 1.0) * (plate - 1.0);
			}
			float lat = bow > 0.0 ? d.x * heading.y - d.y * heading.x : 1.0;
			float wl = wgt * (lat > 1e-4 ? 1.0 : (lat < -1e-4 ? 0.0 : 0.5));
			if (pass == 0) {
				local_sum += wl;
				local_sum_r += wgt - wl;
			} else {
				int i = z * n + x;
				vec4 st = state[i];
				st.y += pushed * (wl * fl + (wgt - wl) * fr) / area;
				st.z = max(st.z, settle);
				if (ground) {
					st.w = max(st.w, wgt);
				}
				state[i] = st;
			}
		}
		if (pass == 0) {
			vec3 sums = reduce3(local_sum, local_sum_r, 0.0);
			if (sums.x + sums.y <= 0.0) {
				return; // nowhere to push: it all compacts in place
			}
			float half_share = sums.x > 0.0 && sums.y > 0.0 ? 0.5 : 1.0;
			fl = sums.x > 0.0 ? half_share / sums.x : 0.0;
			fr = sums.y > 0.0 ? half_share / sums.y : 0.0;
		}
	}
	memoryBarrierBuffer();
	barrier();
}

// ---------------------------------------------------------------------------------------------------------
// Slumping (GroundDeformField::slump), as two gather passes: each texel works out what it gives, then each
// texel takes its neighbours' shares. Snow: SnowSurface3D's loose snow, rim only, crusts stay, at once.
// Ground: the material's repose, rate and deepest dent per lattice point; a column gives its rim, then
// goes down to its deepest dent. Slow material (mud creeping back) only moves on the steps the CPU marks
// slow, by the seconds saved up since (GroundDeformField's tile wait).

// The repose slope, share per second and deepest dent at p, and the mud's slide length tau_y / (rho g) (mud round
// 10: a layer h deep slides off ground steeper than slide / h); false where nothing moves.
bool slump_params(vec2 p, out float tan_r, out float rate, out float max_dent, out float slide) {
	slide = 1e3;
	if (!is_ground()) {
		tan_r = params.relax.w;
		rate = 1e3;
		max_dent = 0.0;
		return true;
	}
	tan_r = 0.0;
	rate = 0.0;
	max_dent = 0.0;
	if (ground_cell(p, 1).w < 0.5) {
		return false;
	}
	vec4 m = ground_cell(p, 0);
	tan_r = m.y;
	rate = m.z;
	max_dent = min(m.w, stone_dent(p, ground_at(p))); // nothing slumps off a stone's top (round 16.1)
	vec4 c3 = ground_cell(p, 3);
	slide = c3.x;
	// Liquid mud moves by its own waves (mud_flux()), not by slumping.
	if (params.mudwave.x > 0.5 && c3.w > 0.0) {
		return false;
	}
	return true;
}

// The steepest ground a mud layer holds with what is left of it at a texel (state st): its depth over the base
// (deepest dent) plus any rim on it (Coussot 1997: a mudflow stops at h = tau_y / (rho g sin theta)).
float slide_tan(float p_slide, float p_max_dent, vec4 p_st) {
	return p_slide >= 1e2 ? 1e3 : p_slide / max(p_max_dent - p_st.x + p_st.y, 0.005);
}

// The step to a neighbour dist away the surface may keep: the repose, or the undisturbed ground's own step where
// steeper (slopes steeper than the repose stay as they are). Where the ground falls faster than the whole mud layer
// holds (a soaked layer, round 10), the layer's own critical slope instead: the layer slides as one mass.
float slump_limit(float p_tan_r, float p_tan_slide, float p_dist, float p_gi, float p_gj) {
	float hold = p_tan_slide * p_dist;
	return p_gi - p_gj > hold ? hold : max(p_tan_r * p_dist, p_gi - p_gj);
}

const ivec2 DIRS[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(1, -1), ivec2(-1, 1), ivec2(-1, -1));

float surface_of(int x, int z, vec4 st) {
	return ground_at(texel_world(x, z)) + st.y - st.x;
}

void slump_give(int x, int z) {
	int n = fn();
	int i = z * n + x;
	give[i] = vec2(0.0);
	if (x < 1 || z < 1 || x >= n - 1 || z >= n - 1) {
		return;
	}
	vec4 st = state[i];
	vec2 wp = texel_world(x, z);
	float tan_r;
	float rate;
	float max_dent;
	float slide;
	if (!slump_params(wp, tan_r, rate, max_dent, slide)) {
		return;
	}
	// Settled texels only move where a soaked mud layer may be sliding (round 10).
	if (st.z <= 0.0 && slide >= 1e2) {
		return;
	}
	float tan_slide = slide_tan(slide, max_dent, st);
	if (st.y <= 0.0 && st.x >= max_dent) {
		return; // nothing here can move
	}
	float dt = params.field.w;
	if (is_ground()) {
		if (rate * dt < 0.05) {
			dt = params.kind.y; // slow: only on the CPU's slow steps
		}
	} else if (pack_at(wp).z > 0.0) {
		return; // plates of a broken crust are slabs, not loose snow
	}
	if (dt <= 0.0) {
		return;
	}
	float tx = texel();
	float gi = ground_at(wp);
	float si = gi + st.y - st.x;
	float sum = 0.0;
	float big = 0.0;
	for (int k = 0; k < 8; k++) {
		ivec2 q = ivec2(x, z) + DIRS[k];
		vec4 sj4 = state[q.y * n + q.x];
		float dist = k < 4 ? tx : tx * 1.41421356;
		float gj = ground_at(texel_world(q.x, q.y));
		float sj = gj + sj4.y - sj4.x;
		float limit = slump_limit(tan_r, tan_slide, dist, gi, gj);
		float e = max((si - sj) - limit, 0.0);
		sum += e;
		big = max(big, e);
	}
	if (big <= 1e-5) {
		return;
	}
	float share = min(1.0, rate * dt);
	float move = min(0.5 * big * share, st.y + max(max_dent - st.x, 0.0));
	if (move <= 1e-6) {
		return;
	}
	// A negative sum: settled to a tenth of a millimetre (the texel's owed slumping then runs out).
	give[i] = vec2(move, big >= 1e-4 ? sum : -sum);
}

void slump_take(int x, int z) {
	int n = fn();
	int i = z * n + x;
	vec4 st = state[i];
	// A finer field runs several slump passes a step (more texels to cross in the same time): each spends
	// only its share of the owed slumping time.
	float dt = params.field.w / max(params.age.w, 1.0);
	vec4 outv = st;
	vec2 own = give[i];
	float own_rim = min(st.y, own.x);
	outv.y -= own_rim;
	outv.x += own.x - own_rim;
	// The ground keeps a moving texel's owed slumping (GroundDeformField marks the tiles it moves into with
	// what it still owes): only settled texels run down. Snow's loose rim always runs down.
	outv.z = (is_ground() && own.x > 0.0 && own.y > 0.0) ? st.z : max(st.z - dt, 0.0);
	vec2 wp = texel_world(x, z);
	float gj = ground_at(wp);
	float sj = gj + st.y - st.x;
	float tx = texel();
	for (int k = 0; k < 8; k++) {
		ivec2 q = ivec2(x, z) - DIRS[k]; // the neighbour whose direction k points here
		if (q.x < 1 || q.y < 1 || q.x >= n - 1 || q.y >= n - 1) {
			continue;
		}
		int qi = q.y * n + q.x;
		vec2 g = give[qi];
		if (g.x <= 0.0) {
			continue;
		}
		vec4 sq = state[qi];
		vec2 qp = texel_world(q.x, q.y);
		float tan_r;
		float q_rate;
		float q_max;
		float q_slide;
		slump_params(qp, tan_r, q_rate, q_max, q_slide);
		float gi = ground_at(qp);
		float si = gi + sq.y - sq.x;
		float dist = k < 4 ? tx : tx * 1.41421356;
		float e = (si - sj) - slump_limit(tan_r, slide_tan(q_slide, q_max, sq), dist, gi, gj);
		if (e <= 0.0) {
			continue;
		}
		float add = g.x * e / abs(g.y);
		float fill = min(outv.x, add);
		outv.x -= fill;
		outv.y += add - fill;
		outv.z = max(outv.z, max(sq.z - dt, 0.0) + dt);
	}
	state_out[i] = outv;
}

// ---------------------------------------------------------------------------------------------------------

float field_sample(vec2 p, int ch) {
	int n = fn();
	float tx = texel();
	float fx = p.x / tx - params.field.x - 0.5;
	float fz = p.y / tx - params.field.y - 0.5;
	if (fx < -0.5 || fz < -0.5 || fx > float(n) - 0.5 || fz > float(n) - 0.5) {
		return 0.0;
	}
	float cx = clamp(fx, 0.0, float(n - 1));
	float cz = clamp(fz, 0.0, float(n - 1));
	int x0 = min(int(cx), n - 2);
	int z0 = min(int(cz), n - 2);
	float ax = cx - float(x0);
	float az = cz - float(z0);
	float a = mix(state[z0 * n + x0][ch], state[z0 * n + x0 + 1][ch], ax);
	float b = mix(state[(z0 + 1) * n + x0][ch], state[(z0 + 1) * n + x0 + 1][ch], ax);
	return mix(a, b, az);
}

// ---------------------------------------------------------------------------------------------------------
// Standing water (docs/ideas/mud.md, stage 2): rain, runoff and seepage gather in ruts and prints as a depth per
// texel over the deformed surface, moved by the virtual pipe model (O'Brien and Hodgins 1995; Mei, Decaudin and
// Hu 2007): each texel's outflows to its four neighbours grow with the head difference, are scaled down so no
// texel sends more than it holds, and the depths change by the net flow, so water runs downhill and stands level.

const float WATER_G = 9.81;

// Runoff channels start from the bed's own roughness (RillGrow, Favis-Mortlock 1998): a few millimetres of fixed
// relief per place, value noise at 3 cm (grain clusters, the swash's and rain's marks), which the water sees and
// follows; the cuts it makes are then the channels.
// Gradient noise (Perlin 2002) rather than value noise: value noise's lattice shows as blobs with edges along the
// grid's axes, which the water followed and cut into square hooks and cross bars. Each octave is also turned by
// its own angle, so no two share a lattice direction.
float runoff_gnoise(vec2 q, int salt) {
	float a = 0.6 + 1.7 * float(salt);
	q = mat2(vec2(cos(a), sin(a)), vec2(-sin(a), cos(a))) * q;
	ivec2 c = ivec2(floor(q)) + ivec2(salt * 7919, salt * 104729);
	vec2 f = q - floor(q);
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float g00 = dot(vec2(cos(6.2831853 * runoff_hash(c)), sin(6.2831853 * runoff_hash(c))), f);
	float g10 = dot(vec2(cos(6.2831853 * runoff_hash(c + ivec2(1, 0))), sin(6.2831853 * runoff_hash(c + ivec2(1, 0)))), f - vec2(1.0, 0.0));
	float g01 = dot(vec2(cos(6.2831853 * runoff_hash(c + ivec2(0, 1))), sin(6.2831853 * runoff_hash(c + ivec2(0, 1)))), f - vec2(0.0, 1.0));
	float g11 = dot(vec2(cos(6.2831853 * runoff_hash(c + ivec2(1, 1))), sin(6.2831853 * runoff_hash(c + ivec2(1, 1)))), f - vec2(1.0));
	// About the value noise's spread (gradient noise peaks near 0.7).
	return 0.75 * mix(mix(g00, g10, u.x), mix(g01, g11, u.x), u.y);
}
// The face's own relief, fixed per place: grain-scale roughness (3 cm) on the swash's low ridges and hollows
// (15 and 50 cm, a few mm), which gather the seepage into fewer, joining rills.
// Each octave fades out where the field's texel cannot hold it (under about 1.5 texels a wavelength): on the
// coarse main field the 3 cm grain roughness aliased into a busy, even pattern over the whole beach.
float runoff_rough(vec2 p) {
	float t = texel();
	float a = 1.0 - smoothstep(0.55 * 0.03, 0.8 * 0.03, t);
	float b = 1.0 - smoothstep(0.55 * 0.15, 0.8 * 0.15, t);
	return params.water3.z * (a * runoff_gnoise(p / 0.03, 0) + 1.5 * b * runoff_gnoise(p / 0.15, 1) + 2.0 * runoff_gnoise(p / 0.5, 2));
}

float water_bed(int x, int z) {
	vec4 st = state[z * fn() + x];
	vec2 p = texel_world(x, z);
	return ground_at(p) + st.y - st.x + (params.water3.x > 0.5 ? runoff_rough(p) : 0.0);
}

// Seepage gathers into the cuts it makes (seepage erosion: Dunne 1990; Howard and McLane 1988; Schorghofer et al.
// 2004). On a seeping face the table stands at the surface; once channels cut below it, the ground between them
// drains to them and stops seeping, and each channel takes the outflow of the strip it drains (drain spacing,
// Hooghoudt 1940): the table settles to a smooth surface over the cuts and water leaves the ground where the bed
// lies below it, in proportion to how far below (here over 1 cm, the 2 cm the face's own rule uses near the
// surface being for a table falling under a plane). A deeper channel lowers the table further round it and takes
// its neighbours' share, so the small heads at the seepage line feed a few trunks that win, rather than an even comb
// (Kirchner 1993's drainage competition). The table is the cut smoothed over 8 cm set against its mean on rings
// 25 and 60 cm out; the ratio to the face's own outflow, 0 to 4, conserved on the mean.
float runoff_seep_gather(int x, int z) {
	int n = fn();
	int r1 = int(round(0.25 / texel()));
	int r2 = int(round(0.6 / texel()));
	if (r1 < 2 || x < r2 || z < r2 || x >= n - r2 || z >= n - r2) {
		return 1.0;
	}
	// The cut only (the field's own height change): the bed's fixed roughness steers the surface flow, not this.
	// The bed under the table smoothed over about a channel's width (a dense 5 x 5 texel box): a pit along a
	// channel's floor draws no more than the floor round it, or the extra seepage digs it on into a chain of pits
	// (sparse taps did the same at their own spacing).
	float b = 0.0;
	for (int dz = -2; dz <= 2; dz++) {
		for (int dx = -2; dx <= 2; dx++) {
			vec4 sj = state[(z + dz) * n + x + dx];
			b += 0.04 * (sj.y - sj.x);
		}
	}
	float m = 0.0;
	for (int k = 0; k < 8; k++) {
		vec2 dir = vec2(cos(0.7853982 * float(k) + 0.3), sin(0.7853982 * float(k) + 0.3));
		vec4 s1 = state[(z + int(round(dir.y * float(r1)))) * n + x + int(round(dir.x * float(r1)))];
		vec4 s2 = state[(z + int(round(dir.y * float(r2)))) * n + x + int(round(dir.x * float(r2)))];
		m += 0.0625 * ((s1.y - s1.x) + (s2.y - s2.x));
	}
	return clamp(1.0 + (m - b) / 0.01, 0.0, 4.0);
}

// Tyres wading (mud stage 4c): a tyre in the water is an obstacle in the pipe model. Water cannot stand inside it,
// so where it dips below a column's water the water there stands that much higher (its head), and the pipes drive it
// out from under the tyre; as the tyre rolls on, its front keeps raising the head ahead of it, which runs off as a
// bow wave, and the water closes in behind. Flux limited by the depth there, so volume is kept by construction. A
// tyre seen from the side is a disc: at s along the heading from the axle its bottom is r - sqrt(r^2 - s^2) under
// the axle.
float wade_head(vec2 p_wp, float p_bed, float p_d) {
	int count = min(int(params.wade.x), 4);
	float head = 0.0;
	float tx = texel();
	for (int k = 0; k < count; k++) {
		vec4 a = params.wade_m[2 * k];
		vec4 b = params.wade_m[2 * k + 1];
		vec2 d = p_wp - a.xz;
		float r = a.w;
		if (dot(d, d) >= (r + b.z + tx) * (r + b.z + tx)) {
			continue;
		}
		vec2 hd = vec2(cos(b.x), sin(b.x));
		float s = dot(d, hd);
		if (abs(s) >= r) {
			continue;
		}
		float side = 1.0 - smoothstep(b.z - 0.5 * tx, b.z + 0.5 * tx, abs(dot(d, vec2(-hd.y, hd.x))));
		float bottom = a.y - sqrt(r * r - s * s);
		head += side * clamp(p_bed + p_d - max(bottom, p_bed), 0.0, p_d);
	}
	return head;
}

// The signed discharge (per texel width, as the fluxes) through the face from texel (x, z) to its +x (p_axis 0) or +z
// (1) neighbour, as the depth pass left it last step; a face off the field takes its neighbour's within.
float water_face(int x, int z, int p_axis, int n) {
	x = clamp(x, 0, n - 1);
	z = clamp(z, 0, n - 1);
	return water_tmp[(1 + p_axis) * n * n + z * n + x];
}

// Muddy water (docs/ideas/mud.md, round 3): the clay standing water carries, kg/m2 over each texel. Tyres churn
// the soft bed they roll on in the water into it (a tread turns over a couple of millimetres of slurry as it passes,
// so a tyre through a 5 cm puddle leaves it at some 40 kg/m3, milky brown), feet and landing clods stir it in
// (stir_water), and it settles out at a silt floc's Stokes speed (0.1 mm/s: a stirred puddle clears in minutes).
// Only cohesive ground clouds the water: sand falls back out at once (from its conductivity, clay to sand).
float water_stir(vec2 p_wp, float p_bed, float p_d, float p_k, float p_sed, float p_c) {
	float sed = p_sed;
	if (p_d > 0.002) {
		float clay = p_k > 0.0 && ground_deformable(p_wp) ? 1.0 - smoothstep(3e-6, 3e-5, p_k) : 0.0;
		if (clay > 0.0) {
			float bed = p_bed;
			int count = min(int(params.wade.x), 4);
			for (int k = 0; k < count; k++) {
				vec4 a = params.wade_m[2 * k];
				vec4 b = params.wade_m[2 * k + 1];
				vec2 o = p_wp - a.xz;
				vec2 hd = vec2(cos(b.x), sin(b.x));
				float s = dot(o, hd);
				float r = a.w;
				// Where the tread meets the bed (its bottom within a centimetre of it) under its width.
				if (abs(s) >= r || abs(dot(o, vec2(-hd.y, hd.x))) > b.z || a.y - sqrt(r * r - s * s) > bed + 0.01) {
					continue;
				}
				// Over the contact's length (about the radius) in the time the tyre takes to cross it.
				sed += clay * params.turbid.z * b.w / r * params.water.y;
			}
			for (int k = 0; k < int(params.wade.z); k++) {
				vec4 st = params.stir[k];
				vec2 o = p_wp - st.xy;
				if (dot(o, o) <= st.z * st.z) {
					sed += clay * st.w;
				}
			}
		}
	}
	// Settling: the floc falls out of the bottom of the column at its Stokes speed.
	sed -= min(params.turbid.y * p_c * params.water.y, max(sed, 0.0));
	return min(max(sed, 0.0), p_d * params.turbid.w);
}

// The clay left in a texel's water once its depth is final: none in a texel that dried (it settled on the bed), and
// never thicker than the water can hold.
void water_sed_close(int i, float p_d) {
	if (params.turbid.x < 0.5) {
		return;
	}
	water[i].s.x = p_d < 1e-4 ? 0.0 : min(water[i].s.x, p_d * params.turbid.w);
}

void water_susp(int x, int z, float p_d, float p_load);

void water_flux(int x, int z) {
	int n = fn();
	int i = z * n + x;
	float d = water[i].w.x;
	// The clay's concentration as this substep begins: the pipes carry it out at the rate they carry the water.
	water[i].s.y = d > 1e-5 ? water[i].s.x / d : 0.0;
	if (params.water3.x > 0.5) {
		// And the soil the runoff carries (water_susp()), kept beside the faces' discharges.
		water_tmp[5 * n * n + i] = d > 1e-5 ? water[i].s.w / d : 0.0;
	}
	if (d <= 0.0) {
		water[i].flux = vec4(0.0);
		return;
	}
	float dt = params.water.y;
	float l = texel();
	float bed = water_bed(x, z);
	bool wading = params.wade.x > 0.0;
	float h = bed + d + (wading ? wade_head(texel_world(x, z), bed, d) : 0.0);
	bool runoff = params.water3.x > 0.5;
	// Runoff channels: the local inertial shallow water equations (Bates, Horritt and Fewtrell 2010), each pipe's
	// unit discharge q = f l gaining g h dt (dh / l) over the flowing depth h, with Manning's bed friction (n 0.025,
	// a sand bed) taken semi-implicitly: a sheet a fraction of a millimetre deep runs at a few centimetres a
	// second, as a seepage film does. Otherwise the pipe model for standing water (its cross-section a texel).
	vec4 f = water[i].flux * (runoff ? 1.0 : max(1.0 - params.water.z * dt, 0.0));
	float hf = max(d, 1e-6);
	float fr = WATER_G * 0.025 * 0.025 * dt / pow(hf, 7.0 / 3.0);
	// Runoff: de Almeida, Bates, Freer and Souvignet (2012): the local inertial scheme left as it is lets the
	// discharge on neighbouring faces decouple, odd and even texels in turn (a checkerboard, along a rill a ladder of
	// one-texel rungs that the bed then cuts in). Each face starts from its last discharge mixed with the two
	// parallel faces' either side, theta 0.8 (their 0.7 to 0.9), from the signed face discharges the depth pass keeps
	// (water_face()); outward is positive for this texel.
	const float THETA = 0.8;
	vec4 q0 = f;
	if (runoff) {
		q0 = vec4(-water_face(x - 1, z, 0, n), water_face(x, z, 0, n), -water_face(x, z - 1, 1, n), water_face(x, z, 1, n));
		vec4 qa = vec4(-water_face(x - 2, z, 0, n), water_face(x - 1, z, 0, n), -water_face(x, z - 2, 1, n), water_face(x, z - 1, 1, n));
		vec4 qb = vec4(-water_face(x, z, 0, n), water_face(x + 1, z, 0, n), -water_face(x, z, 1, n), water_face(x, z + 1, 1, n));
		q0 = THETA * q0 + 0.5 * (1.0 - THETA) * (qa + qb);
		// The open edge's faces (no face record past it): their own outflow.
		q0.x = x > 0 ? q0.x : f.x;
		q0.z = z > 0 ? q0.z : f.z;
	}
	ivec2 nb[4] = ivec2[](ivec2(x - 1, z), ivec2(x + 1, z), ivec2(x, z - 1), ivec2(x, z + 1));
	for (int k = 0; k < 4; k++) {
		ivec2 q = nb[k];
		float dh;
		if (q.x < 0 || q.y < 0 || q.x >= n || q.y >= n) {
			// The field's edge is open: water runs on past it as over the ground beyond (its bed as this
			// texel's, dry), and is lost to the field; a wall there pooled the field's runoff along its edge.
			dh = d;
		} else {
			float qb = water_bed(q.x, q.y);
			float qd = water[q.y * n + q.x].w.x;
			dh = h - (qb + qd + (wading && qd > 0.0 ? wade_head(texel_world(q.x, q.y), qb, qd) : 0.0));
		}
		if (runoff) {
			float qk = q0[k] * l;
			qk = (qk + dt * WATER_G * hf * dh / l) / (1.0 + fr * abs(qk));
			f[k] = max(qk / l, 0.0);
		} else {
			f[k] = max(f[k] + dt * WATER_G * dh / l, 0.0);
		}
	}
	float out_sum = (f.x + f.y + f.z + f.w) * dt;
	if (out_sum > d) {
		f *= d / out_sum;
	}
	water[i].flux = f;
}

void water_depth(int x, int z) {
	int n = fn();
	int i = z * n + x;
	float dt = params.water.y;
	vec4 f = water[i].flux;
	if (params.water3.x > 0.5) {
		// The faces' signed discharges for the next step's mixing (water_flux()); the field's last row and column
		// keep their own outflow.
		water_tmp[n * n + i] = f.y - (x < n - 1 ? water[i + 1].flux.x : 0.0);
		water_tmp[2 * n * n + i] = f.w - (z < n - 1 ? water[i + n].flux.z : 0.0);
	}
	float flow = -(f.x + f.y + f.z + f.w);
	if (x > 0) {
		flow += water[i - 1].flux.y;
	}
	if (x < n - 1) {
		flow += water[i + 1].flux.x;
	}
	if (z > 0) {
		flow += water[i - n].flux.w;
	}
	if (z < n - 1) {
		flow += water[i + n].flux.z;
	}
	float d = water[i].w.x + flow * dt;
	vec2 wp = texel_world(x, z);
	bool turbid = params.turbid.x > 0.5;
	// The clay the water carries (mud round 3): out through the pipes at this texel's concentration, in at each
	// upstream neighbour's (the pipes only carry outflow, so each face's is upwind), kept exactly as the water is.
	float c0 = water[i].s.y;
	float sed = 0.0;
	if (turbid) {
		sed = water[i].s.x - (f.x + f.y + f.z + f.w) * dt * c0;
		if (x > 0) {
			sed += water[i - 1].flux.y * dt * water[i - 1].s.y;
		}
		if (x < n - 1) {
			sed += water[i + 1].flux.x * dt * water[i + 1].s.y;
		}
		if (z > 0) {
			sed += water[i - n].flux.w * dt * water[i - n].s.y;
		}
		if (z < n - 1) {
			sed += water[i + n].flux.z * dt * water[i + n].s.y;
		}
	}
	// Spray landing and water thrown up (GroundSurface3D::add_water): a flat depth over each disc's texels.
	for (int k = 0; k < int(params.wade.y); k++) {
		vec4 wd = params.water_disc[k];
		vec2 o = wp - wd.xy;
		if (dot(o, o) <= wd.z * wd.z) {
			float d0 = max(d, 0.0);
			d = d0 + wd.w;
			if (turbid) {
				sed += wd.w > 0.0 ? wd.w * params.water_disc_mud[k / 4][k % 4] : -min(-wd.w, d0) * c0;
			}
			if (wd.w > 0.0) {
				atomicAdd(ledger[1], uint(wd.w * texel() * texel() * 1e9 + 0.5));
			} else {
				atomicAdd(ledger[2], uint(min(-wd.w, d0) * texel() * texel() * 1e9 + 0.5));
			}
			if (d < 0.0) {
				// A take finding less than it asked for: owed, paid back by the next water added.
				atomicAdd(ledger[0], uint(-d * texel() * texel() * 1e9 + 0.5));
				d = 0.0;
			}
		}
	}
	// The soil the runoff carries (round 11), moved as the clay is, at the faces' upwind concentrations.
	float load = 0.0;
	if (params.water3.x > 0.5) {
		load = water[i].s.w - (f.x + f.y + f.z + f.w) * dt * water_tmp[5 * n * n + i];
		if (x > 0) {
			load += water[i - 1].flux.y * dt * water_tmp[5 * n * n + i - 1];
		}
		if (x < n - 1) {
			load += water[i + 1].flux.x * dt * water_tmp[5 * n * n + i + 1];
		}
		if (z > 0) {
			load += water[i - n].flux.w * dt * water_tmp[5 * n * n + i - n];
		}
		if (z < n - 1) {
			load += water[i + n].flux.z * dt * water_tmp[5 * n * n + i + n];
		}
	}
	vec4 m2 = ground_cell(wp, 2); // conductivity m/s, water table depth m, moisture deficit
	if (turbid) {
		sed = water_stir(wp, water_bed(x, z), max(d, 0.0), m2.x, sed, c0);
		water[i].s.x = sed;
	}
	float rain = params.water.x;
	// Runoff channels: a seepage face (GroundSurface3D::_gpu_cells), where the soaked beach's water table meets the
	// surface and Darcy outflow k sin(beta) leaves it; the ground there is saturated and takes none back.
	float seep = params.water3.x > 0.5 && m2.w > 0.0 ? m2.w * params.water3.w * runoff_seep_gather(x, z) : 0.0;
	if (params.water3.x > 0.5 && m2.w < 0.0) {
		// The sea or a lake over this cell (-1): the runoff joins it.
		water[i].w.x = 0.0;
		water[i].s.x = 0.0;
		water[i].s.w = 0.0;
		return;
	}
	if (seep > 0.0) {
		d = max(d + (rain + seep - (d > 0.0 ? params.water.w : 0.0)) * dt, 0.0);
		water[i].w.x = d;
		water_sed_close(i, d);
		water_susp(x, z, d, load);
		return;
	}
	if (m2.x <= 0.0 && d <= 0.0 && rain <= 0.0) {
		water[i].w.x = 0.0;
		water[i].s.x = 0.0;
		water_susp(x, z, 0.0, load);
		return;
	}
	vec4 st = state[i];
	float g = ground_at(wp);
	float level = g + st.y - st.x + max(d, 0.0);
	float table = g - m2.y;
	// Ground pressed by a tyre or a boot drains slower: compaction closes the large pores.
	float k = m2.x * params.water2.z * (st.x > 0.0 ? params.water2.y : 1.0);
	float src = rain;
	if (level > table + 1e-3) {
		// Above the water table: rain the ground takes, and standing water soaking in, and evaporating. Green and
		// Ampt (1911): the rate is the conductivity times (1 + suction head x moisture deficit / water taken in).
		// Dry ground under rain: the wetting front a few centimetres down, about 1 + 4 x the deficit (m2.z) for
		// loamy soil. Under standing water the top is soaked and the front deep: the conductivity.
		src -= k * (d > 0.0 ? 1.0 : 1.0 + 4.0 * m2.z);
		src -= d > 0.0 ? params.water.w : 0.0;
	}
	d = max(d + src * dt, 0.0);
	if (level < table) {
		// Below it: ground water seeps in through the hollow's walls (Darcy over the seepage length), up to the
		// table and no further.
		d += min(k * (table - level) / params.water2.x * dt, table - level);
	}
	bool fed = src > 0.0 || level < table;
	water[i].w.x = d < params.water2.w && !fed ? 0.0 : d;
	water_sed_close(i, water[i].w.x);
	water_susp(x, z, water[i].w.x, load);
}

// Runoff channels (docs/ideas/sand.md): the water moves the sand as bed load, Meyer-Peter and Mueller's
// transport rate at the bed shear of the flow's own friction, along the flow; the bed follows the divergence of
// that transport (Exner, porosity 0.4), so what is cut upstream is laid where the transport falls off: where the
// water soaks in, slows on the flat or reaches the sea. Transport-limited and in flux form, it keeps the sand
// exactly. The bed moves by the morphological factor (Roelvink 2006): a scaled run cuts in minutes what takes
// hours, the water's own flow unchanged. Where flow gathers it cuts, gathers more and cuts on: rills (Schorghofer
// et al. 2004).
vec2 water_velocity(int x, int z) {
	int n = fn();
	int i = z * n + x;
	float d = water[i].w.x;
	if (d <= 1e-5) {
		return vec2(0.0);
	}
	vec4 f = water[i].flux;
	float fl = x > 0 ? water[i - 1].flux.y : 0.0;
	float fr = x < n - 1 ? water[i + 1].flux.x : 0.0;
	float fd = z > 0 ? water[i - n].flux.w : 0.0;
	float fu = z < n - 1 ? water[i + n].flux.z : 0.0;
	vec2 q = 0.5 * vec2((fl - f.x) + (f.y - fr), (fd - f.z) + (f.w - fu)) * texel();
	vec2 u = q / d;
	// A sheet a few hundredths of a millimetre deep has no meaningful velocity of its own (flux over depth runs
	// away): held to twice its shallow-water wave speed, Froude 2, past what sheet flow on a beach reaches.
	float um = 2.0 * sqrt(WATER_G * d);
	float lu = length(u);
	return lu > um && lu > 0.0 ? u * (um / lu) : u;
}

// Meyer-Peter and Mueller past the bed's own threshold: Shields' for loose sand, a cohesive soil's critical shear
// p_tauc (Pa) where higher (round 11: gullies).
// p_f scales the threshold (seepage force, below).
float bed_load_shear_f(float tau, float p_tauc, float p_f) {
	float dg = 0.00025;
	float theta = tau / (1650.0 * WATER_G * dg);
	float theta_c = p_f * max(0.047, p_tauc / (1650.0 * WATER_G * dg));
	if (theta <= theta_c) {
		return 0.0;
	}
	return 8.0 * sqrt(1.65 * WATER_G * dg * dg * dg) * pow(theta - theta_c, 1.5);
}

float bed_load_shear(float tau, float p_tauc) {
	return bed_load_shear_f(tau, p_tauc, 1.0);
}

// The ground's critical shear at texel i (round 11): the cell's, halved where tyres or boots have freshly turned it
// (loose aggregates: ruts shed several times the sediment of the untouched road, Foltz 1996), raised tenfold where
// the sward has grown back over it (roots, Knapen et al. 2007).
float bed_crit_shear(vec2 p_wp, int p_i) {
	float tc = ground_cell(p_wp, 3).y;
	float f = state[p_i].w;
	float loose = f < 2.0 ? clamp(f, 0.0, 1.0) : 0.5;
	return tc * (1.0 - 0.5 * loose) * (1.0 + 9.0 * clamp(water[p_i].s.z, 0.0, 1.0));
}

// Cohesive ground (round 11: gullies): runoff over soil detaches it rather than rolling it as bed load. WEPP's rill
// detachment (Nearing, Foster, Lane and Finkner 1989), Kr (tau - tau_c), the Partheniades (1965) excess shear law, at
// Kr 0.006 s/m (cropland's baseline; Flanagan and Nearing 1995), three times that freshly turned, over a bulk density of 1600 kg/m3; the soil goes into
// suspension and rides the water (water[].s.w, m of bed a m2), and settles out at an aggregate's 1 mm/s where the
// flow's shear falls under the ground's own (Krone 1962, its deposition threshold taken as the erosion one): so the
// cut deepens where the flow gathers and the soil lands as a fan where it slows or ponds, not on the cut's own floor.
// The share of this against the bed load goes from none on loose sand (tau_c 0.25 Pa) to all of it past 1 Pa. Bed
// moved by the morphological factor as the bed load is; the settling at the water's own pace.
void water_susp(int x, int z, float p_d, float p_load) {
	int n = fn();
	int i = z * n + x;
	if (params.water3.x < 0.5) {
		return;
	}
	vec2 wp = texel_world(x, z);
	float load = max(p_load, 0.0);
	float dt = params.water.y;
	float cut = 0.0;
	float settle = load;
	if (p_d > 1e-4) {
		settle = 0.0;
		float tc = ground_cell(wp, 3).y;
		float coh = smoothstep(0.25, 1.0, tc);
		vec4 f = water[i].flux;
		float fl = x > 0 ? water[i - 1].flux.y : 0.0;
		float fr = x < n - 1 ? water[i + 1].flux.x : 0.0;
		float fd = z > 0 ? water[i - n].flux.w : 0.0;
		float fu = z < n - 1 ? water[i + n].flux.z : 0.0;
		vec2 u = 0.5 * vec2((fl - f.x) + (f.y - fr), (fd - f.z) + (f.w - fu)) * texel() / p_d;
		float um = 2.0 * sqrt(WATER_G * p_d);
		float speed = min(length(u), um);
		float tau = 1000.0 * WATER_G * 0.025 * 0.025 * speed * speed / pow(p_d, 1.0 / 3.0);
		float tcl = max(bed_crit_shear(wp, i), 0.05);
		if (coh > 0.0 && x >= 1 && z >= 1 && x < n - 1 && z < n - 1 && ground_cell(wp, 2).w >= 0.0 && ground_deformable(wp)) {
			// Held to half a millimetre a substep, under the bed's own stability.
			// Freshly turned ground (tread churned, st.w under 2) up to 0.018, WEPP's freshly tilled soils.
			float fr = state[i].w;
			float kr = 0.006 * (1.0 + 2.0 * (fr < 2.0 ? clamp(fr, 0.0, 1.0) : 0.0));
			cut = min(coh * kr * max(tau - tcl, 0.0) / 1600.0 * dt * params.wade.w, 5e-4);
		}
		settle = min(load, 1e-3 * load / p_d * max(1.0 - tau / tcl, 0.0) * dt);
	}
	water[i].s.w = load + cut - settle;
	float dz = settle - cut;
	if (dz == 0.0 || isnan(dz) || isinf(dz)) {
		return;
	}
	water[i].w.z -= dz;
	vec4 st = state[i];
	if (dz < 0.0) {
		float r = min(st.y, -dz);
		st.y -= r;
		st.x = min(st.x - dz - r, 0.3);
	} else {
		float g = min(st.x, dz);
		st.x -= g;
		st.y += dz - g;
	}
	st.z = max(st.z, 2.0);
	state[i] = st;
}

// The bed load (m2/s, along the flow) at each texel, into w.yw.
void water_erode(int x, int z) {
	int n = fn();
	int i = z * n + x;
	if (params.water3.x < 0.5) {
		return;
	}
	vec2 qs = vec2(0.0);
	vec2 wp = texel_world(x, z);
	if (x >= 1 && z >= 1 && x < n - 1 && z < n - 1 && ground_cell(wp, 2).w >= 0.0 && ground_deformable(wp)) {
		// The transport of the flow round the texel (1-2-1 weights), not of the texel alone: a grain moving in a
		// sheet does not stop dead at a texel the film has thinned out of, and a texel-sized pit cannot capture
		// the flow's whole load (the pit-and-mound breakdown of a grid-scale bed under thin flow).
		vec2 qv = vec2(0.0);
		float dm = 0.0;
		float wsum = 0.0;
		// Seepage force (Howard and McLane 1988; Iverson and Major 1986): water leaving the bed at q lifts its grains
		// by the gradient i = q / k against the critical one, (rho_s / rho - 1)(1 - n), about 1 for sand, lowering
		// the threshold in proportion; most where the seepage gathers, at a head, which is what lets it sap back.
		float sf = 1.0;
		float m2s = ground_cell(wp, 2).w;
		if (m2s > 0.0) {
			sf = 1.0 - min(m2s * runoff_seep_gather(x, z) / 1e-4, 0.3);
		}
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				float wgt = float((2 - abs(dx)) * (2 - abs(dz)));
				wsum += wgt;
				float dj = water[i + dz * n + dx].w.x;
				if (dj <= 1e-5) {
					continue;
				}
				vec2 u = water_velocity(x + dx, z + dz);
				float speed = length(u);
				if (speed <= 0.0) {
					continue;
				}
				// The bed shear from the flow's own friction (Manning, n 0.025): rho g n^2 u^2 / h^(1/3).
				float tau = 1000.0 * WATER_G * 0.025 * 0.025 * speed * speed / pow(dj, 1.0 / 3.0);
				vec2 wq = texel_world(x + dx, z + dz);
				// Cohesive ground's share goes to suspension instead (water_susp()).
				float loose = 1.0 - smoothstep(0.25, 1.0, ground_cell(wq, 3).y);
				qv += wgt * loose * bed_load_shear_f(tau, bed_crit_shear(wq, i + dz * n + dx), sf) * u / speed;
				dm += wgt * dj;
			}
		}
		qv /= wsum;
		dm /= wsum;
		float qb = length(qv);
		if (qb > 0.0) {
			// Down the bed's own slope the grains go further (Bagnold 1966; Koch and Flokstra 1981): the transport
			// turns downhill by 1.5 times the slope, which keeps a cut's floor smooth.
			float l = texel();
			vec2 gb = vec2(water_bed(x + 1, z) - water_bed(x - 1, z), water_bed(x, z + 1) - water_bed(x, z - 1)) / (2.0 * l);
			// A separate, larger weight along the flow (morphodynamic models keep the two apart, as Delft3D's
			// streamwise and transverse bed-slope factors): a bump on a channel's floor is planed off where it
			// rises in the flow, so the floor runs smooth instead of breaking into a row of pits.
			vec2 uh = qv / qb;
			qs = qv - qb * (1.5 * gb + 3.0 * dot(gb, uh) * uh);
			// The bed's own wave (celerity 3 q / (0.6 h) for transport going as u^3) kept under half a texel a
			// substep at the morphological factor: past it the explicit bed update overshoots into a chain of
			// pits. Held on the transport itself, so the sand is still kept exactly.
			float qmax = 0.1 * max(dm, 2e-4) * l / max(params.water.y * params.water3.y, 1e-6);
			float lq = length(qs);
			if (lq > qmax) {
				qs *= qmax / lq;
			}
		}
	}
	if (any(isnan(qs)) || any(isinf(qs))) {
		qs = vec2(0.0);
	}
	water[i].w.y = qs.x;
	water[i].w.w = qs.y;
}

// The bed change from the transport's divergence, the faces' transport taken from upwind.
void water_sed(int x, int z) {
	int n = fn();
	int i = z * n + x;
	if (params.water3.x < 0.5 || x < 1 || z < 1 || x >= n - 1 || z >= n - 1) {
		return;
	}
	vec2 q0 = vec2(water[i].w.y, water[i].w.w);
	float qxl = water[i - 1].w.y;
	float qxr = water[i + 1].w.y;
	float qzd = water[i - n].w.w;
	float qzu = water[i + n].w.w;
	if (q0 == vec2(0.0) && qxl <= 0.0 && qxr >= 0.0 && qzd <= 0.0 && qzu >= 0.0) {
		return;
	}
	float fl = (qxl + q0.x) > 0.0 ? max(qxl, 0.0) : min(q0.x, 0.0); // through the -x face, + into this texel
	float frt = (q0.x + qxr) > 0.0 ? max(q0.x, 0.0) : min(qxr, 0.0); // through the +x face, + out of it
	float fd = (qzd + q0.y) > 0.0 ? max(qzd, 0.0) : min(q0.y, 0.0);
	float fu = (q0.y + qzu) > 0.0 ? max(q0.y, 0.0) : min(qzu, 0.0);
	float div = (frt - fl + fu - fd) / texel();
	float dz = -div * params.water.y * params.water3.y / 0.6;
	if (dz == 0.0 || isnan(dz) || isinf(dz)) {
		return;
	}
	water[i].w.z -= dz;
	vec4 st = state[i];
	if (dz < 0.0) {
		float r = min(st.y, -dz);
		st.y -= r;
		st.x = min(st.x - dz - r, 0.3);
	} else {
		float f = min(st.x, dz);
		st.x -= f;
		st.y += dz - f;
	}
	// The cut's walls slump to the sand's repose like any other (two seconds owed, as a fresh press).
	st.z = max(st.z, 2.0);
	state[i] = st;
}

// Wind fills a print from its upwind side and wears its downwind rim (GroundSurface3D::_drift_prints(), which
// says why): the relief (rim less dent) carried downwind at the bed's celerity, first-order upwind, through each
// texel's +x and +z faces into give[i], the share of the two texels' blown sand the lesser; the field's edges
// closed. MODE_RELAX applies the faces' difference.
float drift_blown(int x, int z) {
	return max(ground_cell(texel_world(x, z), 1).z, 0.0);
}

void drift_give(int x, int z) {
	int n = fn();
	int i = z * n + x;
	vec2 g = vec2(0.0);
	float c = params.drift.z;
	if (c > 0.0) {
		vec4 st = state[i];
		float d = st.y - st.x;
		float b = drift_blown(x, z);
		if (x < n - 1) {
			vec4 sq = state[i + 1];
			float u = params.drift.x > 0.0 ? d : sq.y - sq.x;
			g.x = c * params.drift.x * min(b, drift_blown(x + 1, z)) * u;
		}
		if (z < n - 1) {
			vec4 sq = state[i + n];
			float u = params.drift.y > 0.0 ? d : sq.y - sq.x;
			g.y = c * params.drift.y * min(b, drift_blown(x, z + 1)) * u;
		}
	}
	give[i] = g;
}

// Waves in liquid mud (docs/ideas/mud.md, round 14.1). Slurry is a Bingham fluid with inertia: at a vehicle's speed
// its Reynolds number rho U h / mu is of order 100, so what a tyre or a belly shoves aside runs off as a wave that the
// yield stress and the viscosity damp, and the rut closes behind with a slosh rather than a creep. The layer over its
// firm base is a shallow Bingham flow: the local inertial shallow water equations (Bates, Horritt and Fewtrell 2010)
// with the plug's bed stress, the yield stress plus the laminar viscous term tau_b = tau_y + 3 mu u / h (Huang and
// Garcia 1998, Herschel-Bulkley mud on a slope; FLO-2D's quadratic rheology, O'Brien et al. 1993, without its
// turbulent part): each face's unit discharge q gains g h dt (d eta / l), loses the yield's tau_y / rho dt (never past
// zero: the plug stands), and its viscous part is taken semi-implicitly. A layer stands where g h S < tau_y / rho,
// the same critical slope the slumping kept (round 1) and slides use (round 10). Only mud the cells mark liquid
// (cover cell 3: z tau_y / rho, w mu / rho, 0 for stiff mud and plain ground); firm ground is a wall.
bool mud_liquid(vec2 p, out float tau, out float nu, out float max_dent) {
	vec4 c3 = ground_cell(p, 3);
	tau = c3.z;
	nu = c3.w;
	max_dent = ground_cell(p, 0).w;
	if (nu <= 0.0 || ground_cell(p, 1).w <= 0.5) {
		return false;
	}
	max_dent = min(max_dent, stone_dent(p, ground_at(p))); // the mud flows round a stone, not off its top (round 16.1)
	return true;
}

// The mud layer's thickness over its firm base at a texel (the deepest dent below the undisturbed ground, plus rim).
float mud_layer(float p_max_dent, vec4 p_st) {
	return max(p_max_dent - p_st.x + p_st.y, 0.0);
}

// One face's unit discharge, m2/s, from texel a to texel b (b = a + 1 or a + n), its old value q, and the old values of
// the faces before and after it on the same line (de Almeida, Bates, Freer and Souvignet 2012: the theta weighting
// damps the local inertial scheme's checkerboard where little friction holds it).
float mud_face(int n, ivec2 a, ivec2 b, float q, float q_before, float q_after) {
	if (b.x >= n - 1 || b.y >= n - 1) {
		return 0.0; // the field's border is a wall (mud_depth() leaves it as it is)
	}
	vec2 ap = texel_world(a.x, a.y);
	vec2 bp = texel_world(b.x, b.y);
	float ta;
	float na;
	float ma;
	float tb;
	float nb;
	float mb;
	if (!mud_liquid(ap, ta, na, ma) || !mud_liquid(bp, tb, nb, mb)) {
		return 0.0; // firm ground and stiff mud are walls
	}
	vec4 sa = state[a.y * n + a.x];
	vec4 sb = state[b.y * n + b.x];
	float ga = ground_at(ap);
	float gb = ground_at(bp);
	float ea = ga + sa.y - sa.x;
	float eb = gb + sb.y - sb.x;
	// Bates et al. 2010: the flow depth at the face, the higher surface over the higher base.
	float h = max(ea, eb) - max(ga - ma, gb - mb);
	if (h < 0.01) {
		return 0.0;
	}
	float dt = params.mudwave.y;
	float l = texel();
	const float THETA = 0.8;
	float qt = THETA * q + 0.5 * (1.0 - THETA) * (q_before + q_after) - dt * WATER_G * h * (eb - ea) / l;
	// The plug stands until its weight down the slope passes the yield stress, and the stress never reverses it.
	qt = sign(qt) * max(abs(qt) - dt * 0.5 * (ta + tb), 0.0);
	qt /= 1.0 + dt * 1.5 * (na + nb) / (h * h);
	// No texel sends more than a quarter of its layer through one face in a substep.
	float hs = qt > 0.0 ? mud_layer(ma, sa) : mud_layer(mb, sb);
	return clamp(qt, -0.25 * hs * l / dt, 0.25 * hs * l / dt);
}

// Waves in liquid mud: the discharges through each texel's +x and +z faces (mudf .zw from the old .xy).
void mud_flux(int x, int z) {
	int n = fn();
	int i = z * n + x;
	vec4 f = mudf[i];
	if (x < 1 || z < 1 || x >= n - 1 || z >= n - 1) {
		mudf[i] = vec4(f.xy, 0.0, 0.0);
		return;
	}
	float qx = mud_face(n, ivec2(x, z), ivec2(x + 1, z), f.x, mudf[i - 1].x, mudf[i + 1].x);
	float qz = mud_face(n, ivec2(x, z), ivec2(x, z + 1), f.y, mudf[i - n].y, mudf[i + n].y);
	mudf[i] = vec4(f.xy, qx, qz);
}

void mud_depth(int x, int z) {
	int n = fn();
	int i = z * n + x;
	vec4 f = mudf[i];
	if (x < 1 || z < 1 || x >= n - 1 || z >= n - 1) {
		mudf[i] = vec4(f.zw, f.zw);
		return;
	}
	float dv = (mudf[i - 1].z - f.z + mudf[i - n].w - f.w) * params.mudwave.y / texel();
	mudf[i] = vec4(f.zw, f.zw);
	if (abs(dv) < 1e-7) {
		return;
	}
	vec4 st = state[i];
	if (dv > 0.0) {
		// Mud arriving fills a hollow pressed here first, then stands on top.
		float fill = min(st.x, dv);
		st.x -= fill;
		st.y += dv - fill;
	} else {
		// Mud leaving takes what stands on top first, then lowers the surface toward the base.
		float take = min(st.y, -dv);
		st.y -= take;
		st.x += -dv - take;
	}
	// Moving ground is fresh and keeps its slumping owed, so it settles on as the field ages it.
	st.z = max(st.z, 1.0);
	state[i] = st;
}

void main() {
	int n = fn();
	uint gid = gl_GlobalInvocationID.x;
	if (push.mode == MODE_STAMPS) {
		for (int s = 0; s < push.count; s++) {
			run_stamp(stamps[s]);
			memoryBarrierBuffer();
			barrier();
		}
		return;
	}
	if (push.mode == MODE_COLLISION) {
		int cells = int(params.collision.w);
		int wv = cells + 1;
		if (gid >= uint(wv * wv)) {
			return;
		}
		int x = int(gid) % wv;
		int z = int(gid) / wv;
		float sp = params.collision.z;
		vec2 p = params.collision.xy + (vec2(x, z) - float(cells) * 0.5) * sp;
		float g = ground_at(p);
		float hgt = g;
		if (params.ground.w >= 0.5) {
			vec4 r = params.mesh_rect;
			float edge = min(min(p.x - r.x, r.x + r.z - p.x), min(p.y - r.y, r.y + r.w - p.y));
			float fade = params.snow.w > 0.0 ? smoothstep(0.0, params.snow.w, edge) : (edge >= 0.0 ? 1.0 : 0.0);
			float roof = cover_sample(p, 1).y;
			float d = roof > g + 0.5 ? 0.0 : pack_at(p).x * fade;
			d += field_sample(p, 1) - field_sample(p, 0);
			hgt = d >= params.snow.z ? g + d : g;
		}
		collision_h[gid] = hgt / sp;
		return;
	}
	if (push.mode == MODE_STORE) {
		// One tile texel per thread: count tiles x TILE x TILE.
		int op = int(gid) / (TILE * TILE);
		if (op >= push.count) {
			return;
		}
		int k = int(gid) % (TILE * TILE);
		ivec4 o = tile_ops[op];
		int x = o.x * TILE + k % TILE;
		int z = o.y * TILE + k / TILE;
		vec4 st = state[z * n + x];
		int sw = cache_stride();
		cache[(o.z * TILE * TILE + k) * sw] = packHalf2x16(st.xy);
		if (is_ground()) {
			cache[(o.z * TILE * TILE + k) * sw + 1] = packHalf2x16(vec2(st.w, water[z * n + x].w.x));
			cache[(o.z * TILE * TILE + k) * sw + 2] = packHalf2x16(water[z * n + x].s.xz);
		}
		return;
	}
	if (gid >= uint(n * n)) {
		return;
	}
	int x = int(gid) % n;
	int z = int(gid) / n;
	int i = int(gid);
	if (push.mode == MODE_MUD_FLUX) {
		mud_flux(x, z);
	} else if (push.mode == MODE_MUD_DEPTH) {
		mud_depth(x, z);
	} else if (push.mode == MODE_WATER_FLUX) {
		water_flux(x, z);
	} else if (push.mode == MODE_WATER_DEPTH) {
		water_depth(x, z);
	} else if (push.mode == MODE_WATER_COPY) {
		water[i].flux = vec4(0.0);
		water[i].w = vec4(water_tmp[i], 0.0, 0.0, 0.0);
		water[i].s = vec4(water_tmp[3 * n * n + i], 0.0, water_tmp[4 * n * n + i], 0.0);
	} else if (push.mode == MODE_WATER_ERODE) {
		water_erode(x, z);
	} else if (push.mode == MODE_WATER_SED) {
		water_sed(x, z);

	} else if (push.mode == MODE_SLUMP_GIVE) {
		slump_give(x, z);
	} else if (push.mode == MODE_SLUMP_TAKE) {
		slump_take(x, z);
	} else if (push.mode == MODE_DRIFT_GIVE && is_ground()) {
		drift_give(x, z);
	} else if (push.mode == MODE_RELAX && is_ground()) {
		// GroundSurface3D::_age: turned ground dries, wind fills dry sand prints, rain softens soil prints.
		vec4 st = state[i];
		// The relief the wind carried in and out (drift_give()).
		float dd = 0.0;
		if (params.drift.z > 0.0) {
			vec2 g = give[i];
			dd = (x > 0 ? give[i - 1].x : 0.0) - g.x + (z > 0 ? give[i - n].y : 0.0) - g.y;
		}
		if (st.x <= 0.0 && st.y <= 0.0 && st.w <= 0.0 && dd == 0.0) {
			return;
		}
		vec2 wp = texel_world(x, z);
		vec4 m1 = ground_cell(wp, 1);
		float keep = exp(-(params.kind.w * (params.drift.z > 0.0 ? params.drift.w : 1.0) * m1.z + params.age.x) * params.age.y);
		st.x *= keep;
		st.y *= keep;
		if (dd != 0.0) {
			float d = st.y - st.x + dd;
			st.x = max(-d, 0.0);
			st.y = max(d, 0.0);
		}
		if (st.w >= 4.0) {
			// Tracked mud dries in place (its film stays).
			vec2 sq = mud_band_share_wet(st.w);
			st.w = mud_band(sq.x, sq.y * params.kind.z);
		} else {
			st.w = st.w >= 2.0 ? st.w : st.w * params.kind.z; // the fresh tint dries; a laid mineral (2 + share) stays
		}
		// As the CPU: a texel both pressed and piled on loses the smaller from both, then films under 10 microns go.
		float both = min(st.x, st.y);
		st.xy -= vec2(both);
		st.x = st.x < 1e-5 ? 0.0 : st.x;
		st.y = st.y < 1e-5 ? 0.0 : st.y;
		st.w = st.w < 0.01 ? 0.0 : st.w;
		state[i] = st;
	} else if (push.mode == MODE_RELAX) {
		vec4 st = state[i];
		if (st.x <= 0.0 && st.y <= 0.0) {
			return;
		}
		vec2 wp = texel_world(x, z);
		vec4 pk = pack_at(wp);
		float fresh = cover_sample(wp, 1).x;
		float keep = params.relax.x;
		float dn = max(st.x - fresh * params.relax.y, 0.0) * keep;
		float rm = st.y * keep;
		dn = min(dn, dent_max(pk.x, pk.y));
		rm = min(rm, pk.x);
		st.x = dn > 1e-4 ? dn : 0.0;
		st.y = rm > 1e-4 ? rm : 0.0;
		state[i] = st;
	} else if (push.mode == MODE_BLIT) {
		vec4 st = state[i];
		vec2 tint = vec2(1.0);
		if (is_ground() && st.w > 0.0) {
			// Freshly turned ground: darker (damp grains from below) and glossier when wet.
			vec4 m1 = ground_cell(texel_world(x, z), 1);
			// Or a laid band of heavy minerals (st.w 2 plus its share: GroundDeformField::amount_fresh): dark, and it
			// stays dark dry (GroundSurface3D::_publish_tint).
			tint = st.w >= 2.0 ? vec2(1.0 - 0.4 * min(st.w - 2.0, 1.0), 1.0) : vec2(1.0 - st.w * m1.x, mix(1.0, m1.y, st.w));
			if (st.w >= 4.0) {
				// Tracked mud (GroundSurface3D::_publish_tint the same): wet, dark and glossy (Lagarde's 0.52 and a
				// smooth film); dry, a pale matte film of fine clay over the grain.
				vec2 sq = mud_band_share_wet(st.w);
				tint = mix(vec2(1.0), mix(vec2(1.12, 1.0), vec2(0.55, 0.45), sq.y), sq.x);
			}
		}
		if (is_ground()) {
			// The ground's map: the net depression (dent less rim) and the standing water over it.
			// With runoff on, the bed's fixed relief (runoff_rough) is drawn too: the water runs over it and lays its
			// sand in its hollows, so a drawing without it showed the filled hollows as raised ridges.
			float relief = params.water3.x > 0.5 ? runoff_rough(texel_world(x, z)) : 0.0;
			// Muddy water (round 3): its clay in place of the turned ground's gloss, which the water's surface
			// hides (A < 0: minus the concentration over 400 kg/m3).
			float wd = water[i].w.x;
			// Runoff's sheet is drawn from its depth under a 1-2-1 filter: shallow flow down a slope carries a
			// grid-scale wave of a few texels along it (the explicit scheme's short waves, under the scales it
			// resolves, which spectral and finite-volume models filter out as a matter of course), which the
			// water's gloss turned into a ladder of bright rungs on every rill. A quarter of a 3-texel wave is left,
			// nine tenths of a rill's 10-texel width, and the rills' edges round off over a texel as a film's do.
			if (params.water3.x > 0.5 && x >= 1 && z >= 1 && x < n - 1 && z < n - 1) {
				float ws = 0.0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						ws += float((2 - abs(dx)) * (2 - abs(dz))) * water[i + dz * n + dx].w.x;
					}
				}
				wd = ws / 16.0;
			}
			// The soil the runoff carries counts too (round 11), at 1600 kg of it a cubic metre of bed.
			float cm = wd > 0.002 ? (water[i].s.x + 1600.0 * water[i].s.w) / wd : 0.0;
			if (cm > 0.5) {
				tint.y = -min(cm / 400.0, 1.0);
			} else if (st.w < 0.05 && water[i].s.z > 0.0) {
				// The sward grown back over an old rut (mud round 6), for the grass and the ground's look: A just
				// over 1 (1.02 at most, a roughness factor too small to see), never reached otherwise.
				tint.y = 1.0 + 0.02 * clamp(water[i].s.z, 0.0, 1.0);
			}
			imageStore(out_tex, ivec2(x, z), vec4(st.x - st.y - relief, wd, tint));
		} else {
			imageStore(out_tex, ivec2(x, z), vec4(st.x, st.y, tint));
		}
	} else if (push.mode == MODE_SHIFT) {
		// state is the old field, state_out the new one. tile_ops[new tile] = (kind, index): kind 1 an old
		// tile's index, 2 a cache slot, 0 empty.
		int nt = params.res.y;
		int ti = (z / TILE) * nt + x / TILE;
		ivec4 o = tile_ops[ti];
		int lx = x % TILE;
		int lz = z % TILE;
		vec4 v = vec4(0.0);
		float wd = 0.0;
		float ws = 0.0; // the clay in it (kept in the cache with the water since mud round 5)
		float rg = 0.0; // the sward grown back over it (mud round 6)
		if (o.x == 1) {
			int ox = (o.y % nt) * TILE + lx;
			int oz = (o.y / nt) * TILE + lz;
			v = state[oz * n + ox];
			wd = is_ground() ? water[oz * n + ox].w.x : 0.0;
			ws = is_ground() ? water[oz * n + ox].s.x : 0.0;
			rg = is_ground() ? water[oz * n + ox].s.z : 0.0;
		} else if (o.x == 2) {
			int sw = cache_stride();
			int ci = (o.y * TILE * TILE + lz * TILE + lx) * sw;
			v.xy = unpackHalf2x16(cache[ci]);
			if (is_ground()) {
				vec2 fw = unpackHalf2x16(cache[ci + 1]);
				v.w = fw.x;
				wd = fw.y;
				vec2 cr = unpackHalf2x16(cache[ci + 2]);
				ws = wd > 1e-4 ? cr.x : 0.0;
				rg = cr.y;
			}
		}
		state_out[i] = v;
		if (is_ground()) {
			water_tmp[i] = wd;
			water_tmp[3 * n * n + i] = ws;
			water_tmp[4 * n * n + i] = rg;
		}
	} else if (push.mode == MODE_PACK) {
		vec4 st = state[i];
		int sw = stride();
		packed[i * sw] = packHalf2x16(st.xy);
		if (sw == 2) {
			packed[i * 2 + 1] = packHalf2x16(vec2(st.w, st.z)); // fresh, slumping owed
			packed[n * n * 2 + i] = packHalf2x16(vec2(water[i].w.x, water[i].w.z));
			packed[n * n * 3 + i] = packHalf2x16(vec2(water[i].w.x > 1e-5 ? water[i].s.x / water[i].w.x : 0.0, water[i].s.z));
			if (i == 0) {
				packed[n * n * 4] = ledger[0];
				packed[n * n * 4 + 1] = ledger[1];
				packed[n * n * 4 + 2] = ledger[2];
			}
		}
	}
}

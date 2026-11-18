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
	vec4 wade; // tyres wading (mud stage 4c): count (up to 4), unused
	vec4 wade_m[8]; // per tyre: (axle x, y, z, radius), (heading angle, unused, half width, speed m/s)
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
	// Ground: per lattice point, two: (rim share, tan repose, slumping rate, deepest dent), (darken, gloss, blown sand, deformable).
	vec4 cover_cells[];
};
struct Stamp {
	vec4 a; // position x, y, z, radius
	vec4 b; // half x, half z, yaw, bottom
	vec4 c; // sink, contact tolerance, velocity x, velocity z
	vec4 d; // crack length, flags, seed (bits), unused
	ivec4 e; // x0, x1, z0, z1
};
layout(set = 0, binding = 6, std430) restrict readonly buffer Stamps {
	Stamp stamps[];
};
layout(set = 0, binding = 7, rgba16f) uniform restrict writeonly image2D out_tex;
layout(set = 0, binding = 8, std430) restrict writeonly buffer Collision {
	float collision_h[];
};
layout(set = 0, binding = 9, std430) restrict buffer Cache {
	uint cache[]; // per slot TILE x TILE packHalf2x16(dent, rim)
};
layout(set = 0, binding = 10, std430) restrict readonly buffer TileOps {
	ivec4 tile_ops[]; // store: (tile x, tile z, slot, 0); shift: per new tile (source kind, index, 0, 0)
};
layout(set = 0, binding = 11, std430) restrict writeonly buffer Packed {
	uint packed[]; // per texel stride() uints, then for the ground one more each: (water depth, 0)
};
// Standing water (ground only): per texel the outflows to -x, +x, -z, +z (m/s: m3/s over the texel's area) and
// (depth m, bed load along x m2/s, bed cut by the runoff m, bed load along z m2/s). The virtual pipe model (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007).
struct WaterCell {
	vec4 flux;
	vec4 w;
};
layout(set = 0, binding = 12, std430) restrict buffer Water {
	WaterCell water[];
};
layout(set = 0, binding = 13, std430) restrict buffer WaterTmp {
	float water_tmp[]; // the shifted depth, copied back by MODE_WATER_COPY
};

const float SWE_MIN = 0.01;

bool is_ground() {
	return params.kind.x > 0.5;
}

// uints per texel in the cache and the read back: (dent, rim), then (fresh, 0) for the ground.
int stride() {
	return is_ground() ? 2 : 1;
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
	return cover_cells[(z * n + x) * 3 + part];
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
			// A line being washed away: only what was laid on the surface goes, and its tint with it.
			st.y -= min(st.y, a);
			if (ground) {
				st.w *= 1.0 - cover;
			}
		} else if (sa > 0.0) {
			float take = min(st.y, a);
			st.y -= take;
			st.x = min(st.x + a - take, c.max_dent);
			if (ground) {
				st.w = 1.0;
			}
		} else {
			float fill = min(st.x, a);
			st.x -= fill;
			st.y += a - fill;
			if (ground && s.c.z > 0.0) {
				st.w = max(st.w, s.c.z * cover); // a marked deposit (a swash mark's lip)
			}
		}
		st.z = max(st.z, settle);
		state[i] = st;
	}
	memoryBarrierBuffer();
	barrier();
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
		float relief = sole ? sole_relief(lp, half_) : 0.0;
		target += relief;
		if (sink > 0.0) {
			float wgt = round_ ? sqrt(max(1.0 - d2 / r2, 0.0)) : 1.0;
			target = min(target, c.surface - sink * wgt + relief);
		}
		target = max(target, c.surface - c.max_dent);
		if (target >= sfc) {
			continue;
		}
		float delta = (sfc - target) * cover;
		deepest = max(deepest, c.surface - target);
		float take = min(st.y, delta);
		st.y -= take;
		st.x += delta - take;
		st.z = max(st.z, settle);
		if (ground) {
			st.w = 1.0;
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
			if (ground) {
				st.w = 1.0;
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

// The repose slope, share per second and deepest dent at p; false where nothing moves.
bool slump_params(vec2 p, out float tan_r, out float rate, out float max_dent) {
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
	max_dent = m.w;
	return true;
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
	if (st.z <= 0.0) {
		return;
	}
	vec2 wp = texel_world(x, z);
	float tan_r;
	float rate;
	float max_dent;
	if (!slump_params(wp, tan_r, rate, max_dent)) {
		return;
	}
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
		float limit = max(tan_r * dist, gi - gj);
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
		slump_params(qp, tan_r, q_rate, q_max);
		float gi = ground_at(qp);
		float si = gi + sq.y - sq.x;
		float dist = k < 4 ? tx : tx * 1.41421356;
		float e = (si - sj) - max(tan_r * dist, gi - gj);
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
// The face's own relief, fixed per place: grain-scale roughness (3 cm) on the swash's low ridges and hollows
// (15 and 50 cm, a few mm), which gather the seepage into fewer, joining rills.
float runoff_rough(vec2 p) {
	return params.water3.z * (runoff_noise(p / 0.03, 0) + 1.5 * runoff_noise(p / 0.15, 1) + 2.0 * runoff_noise(p / 0.5, 2));
}

float water_bed(int x, int z) {
	vec4 st = state[z * fn() + x];
	vec2 p = texel_world(x, z);
	return ground_at(p) + st.y - st.x + (params.water3.x > 0.5 ? runoff_rough(p) : 0.0);
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

void water_flux(int x, int z) {
	int n = fn();
	int i = z * n + x;
	float d = water[i].w.x;
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
			float qk = f[k] * l;
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
	vec4 m2 = ground_cell(wp, 2); // conductivity m/s, water table depth m, moisture deficit
	float rain = params.water.x;
	// Runoff channels: a seepage face (GroundSurface3D::_gpu_cells), where the soaked beach's water table meets the
	// surface and Darcy outflow k sin(beta) leaves it; the ground there is saturated and takes none back.
	float seep = params.water3.x > 0.5 ? m2.w * params.water3.w : 0.0;
	if (params.water3.x > 0.5 && m2.w < 0.0) {
		// The sea or a lake over this cell (-1): the runoff joins it.
		water[i].w.x = 0.0;
		return;
	}
	if (seep > 0.0) {
		d = max(d + (rain + seep - (d > 0.0 ? params.water.w : 0.0)) * dt, 0.0);
		water[i].w.x = d;
		return;
	}
	if (m2.x <= 0.0 && d <= 0.0 && rain <= 0.0) {
		water[i].w.x = 0.0;
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

float bed_load_shear(float tau) {
	float dg = 0.00025;
	float theta = tau / (1650.0 * WATER_G * dg);
	if (theta <= 0.047) {
		return 0.0;
	}
	return 8.0 * sqrt(1.65 * WATER_G * dg * dg * dg) * pow(theta - 0.047, 1.5);
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
				qv += wgt * bed_load_shear(tau) * u / speed;
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
		int sw = stride();
		cache[(o.z * TILE * TILE + k) * sw] = packHalf2x16(st.xy);
		if (sw == 2) {
			cache[(o.z * TILE * TILE + k) * 2 + 1] = packHalf2x16(vec2(st.w, water[z * n + x].w.x));
		}
		return;
	}
	if (gid >= uint(n * n)) {
		return;
	}
	int x = int(gid) % n;
	int z = int(gid) / n;
	int i = int(gid);
	if (push.mode == MODE_WATER_FLUX) {
		water_flux(x, z);
	} else if (push.mode == MODE_WATER_DEPTH) {
		water_depth(x, z);
	} else if (push.mode == MODE_WATER_COPY) {
		water[i].flux = vec4(0.0);
		water[i].w = vec4(water_tmp[i], 0.0, 0.0, 0.0);
	} else if (push.mode == MODE_WATER_ERODE) {
		water_erode(x, z);
	} else if (push.mode == MODE_WATER_SED) {
		water_sed(x, z);

	} else if (push.mode == MODE_SLUMP_GIVE) {
		slump_give(x, z);
	} else if (push.mode == MODE_SLUMP_TAKE) {
		slump_take(x, z);
	} else if (push.mode == MODE_RELAX && is_ground()) {
		// GroundSurface3D::_age: turned ground dries, wind fills dry sand prints, rain softens soil prints.
		vec4 st = state[i];
		if (st.x <= 0.0 && st.y <= 0.0 && st.w <= 0.0) {
			return;
		}
		vec2 wp = texel_world(x, z);
		vec4 m1 = ground_cell(wp, 1);
		float keep = exp(-(params.kind.w * m1.z + params.age.x) * params.age.y);
		st.x *= keep;
		st.y *= keep;
		st.w *= params.kind.z;
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
			tint = vec2(1.0 - st.w * m1.x, mix(1.0, m1.y, st.w));
		}
		if (is_ground()) {
			// The ground's map: the net depression (dent less rim) and the standing water over it.
			// With runoff on, the bed's fixed relief (runoff_rough) is drawn too: the water runs over it and lays its
			// sand in its hollows, so a drawing without it showed the filled hollows as raised ridges.
			float relief = params.water3.x > 0.5 ? runoff_rough(texel_world(x, z)) : 0.0;
			imageStore(out_tex, ivec2(x, z), vec4(st.x - st.y - relief, water[i].w.x, tint));
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
		if (o.x == 1) {
			int ox = (o.y % nt) * TILE + lx;
			int oz = (o.y / nt) * TILE + lz;
			v = state[oz * n + ox];
			wd = is_ground() ? water[oz * n + ox].w.x : 0.0;
		} else if (o.x == 2) {
			int sw = stride();
			int ci = (o.y * TILE * TILE + lz * TILE + lx) * sw;
			v.xy = unpackHalf2x16(cache[ci]);
			if (sw == 2) {
				vec2 fw = unpackHalf2x16(cache[ci + 1]);
				v.w = fw.x;
				wd = fw.y;
			}
		}
		state_out[i] = v;
		if (is_ground()) {
			water_tmp[i] = wd;
		}
	} else if (push.mode == MODE_PACK) {
		vec4 st = state[i];
		int sw = stride();
		packed[i * sw] = packHalf2x16(st.xy);
		if (sw == 2) {
			packed[i * 2 + 1] = packHalf2x16(vec2(st.w, st.z)); // fresh, slumping owed
			packed[n * n * 2 + i] = packHalf2x16(vec2(water[i].w.x, water[i].w.z));
		}
	}
}

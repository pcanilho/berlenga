#[compute]
#version 450

// The snow print field on the GPU (docs/ideas/snow.md, follow-up 13). The same maths as
// GroundDeformField (modules/ground) and SnowSurface3D's columns, slumping and relaxing, on a field of
// (dent, rim, slumping owed, unused) per texel. One shader, one pipeline: the push constant says which
// pass runs.

#define MODE_STAMPS 0
#define MODE_SLUMP_GIVE 1
#define MODE_SLUMP_TAKE 2
#define MODE_RELAX 3
#define MODE_BLIT 4
#define MODE_COLLISION 5
#define MODE_STORE 6
#define MODE_SHIFT 7
#define MODE_PACK 8

#define STAMP_BOX 1u
#define STAMP_ROUND 2u
#define STAMP_HELD 4u
#define STAMP_BROKE 8u

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
	vec4 bow; // bow speed, gain, reach, unused
	vec4 crust_consts; // crack length, plate size, crack sectors, unused
	ivec4 shift; // texel offset of the old field in the new one's tiles: old tile index = new tile index + shift.xy
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
	vec4 cover_cells[]; // per cell, two: (swe mm, density, crust thickness, crust burial), (new snow m, cover ground m, 0, 0)
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
	uint packed[];
};

const float SWE_MIN = 0.01;

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

Column column_at(vec2 p, uint flags) {
	Column c;
	c.surface = 0.0;
	c.max_dent = 0.0;
	c.rim_share = 0.3;
	c.crust = 0.0;
	c.deformable = false;
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

void mark(int i) {
	state[i].z = max(state[i].z, params.relax.z);
}

void run_stamp(Stamp s) {
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
	bool box = (flags & STAMP_BOX) != 0u;
	bool round_ = (flags & STAMP_ROUND) != 0u;
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
	float ring = 0.5 * (box ? sqrt(half_.x * half_.y) : radius);
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
		if (box) {
			float lx = cy * d.x - sy * d.y;
			float lz = sy * d.x + cy * d.y;
			float qx = abs(lx) - half_.x;
			float qz = abs(lz) - half_.y;
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
		if (box) {
			float lx = cy * d.x - sy * d.y;
			float lz = sy * d.x + cy * d.y;
			float qx = abs(lx) - half_.x;
			float qz = abs(lz) - half_.y;
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
		if (sink > 0.0) {
			float wgt = round_ ? sqrt(max(1.0 - d2 / r2, 0.0)) : 1.0;
			target = min(target, c.surface - sink * wgt);
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
		st.z = max(st.z, params.relax.z);
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
			if (box) {
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
			st.z = max(st.z, params.relax.z);
			state[i] = st;
		}
		memoryBarrierBuffer();
		barrier();
	}

	// The rim: the pushed share spread over the ring, thickest at the edge.
	if (pushed <= 0.0) {
		return;
	}
	float wsum = 0.0;
	for (int pass = 0; pass < 2; pass++) {
		float local_sum = 0.0;
		for (int k = int(t); k < count; k += WG) {
			int x = x0 + k % w;
			int z = z0 + k / w;
			vec2 wp = texel_world(x, z);
			vec2 d = wp - pos.xz;
			float d2 = dot(d, d);
			float o;
			if (box) {
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
			float wgt = (1.0 - o / reach) * (1.0 + params.bow.y * ahead);
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
			if (pass == 0) {
				local_sum += wgt;
			} else {
				int i = z * n + x;
				vec4 st = state[i];
				st.y += pushed * wgt / (wsum * area);
				st.z = max(st.z, params.relax.z);
				state[i] = st;
			}
		}
		if (pass == 0) {
			wsum = reduce3(local_sum, 0.0, 0.0).x;
			if (wsum <= 0.0) {
				return; // nowhere to push: it all compacts in place
			}
		}
	}
	memoryBarrierBuffer();
	barrier();
}

// ---------------------------------------------------------------------------------------------------------
// Slumping (GroundDeformField::slump with SnowSurface3D's loose snow: rim only, crusts stay), as two gather
// passes: each texel works out what it gives, then each texel takes its neighbours' shares.

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
	if (st.z <= 0.0 || st.y <= 0.0) {
		return;
	}
	vec2 wp = texel_world(x, z);
	if (pack_at(wp).z > 0.0) {
		return; // plates of a broken crust are slabs, not loose snow
	}
	float tx = texel();
	float tan_r = params.relax.w;
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
	float share = min(1.0, 1e3 * params.field.w);
	float move = min(0.5 * big * share, st.y);
	if (move <= 1e-6) {
		return;
	}
	give[i] = vec2(move, sum);
}

void slump_take(int x, int z) {
	int n = fn();
	int i = z * n + x;
	vec4 st = state[i];
	float dt = params.field.w;
	vec4 outv = st;
	vec2 own = give[i];
	outv.y -= own.x;
	outv.z = max(st.z - dt, 0.0);
	vec2 wp = texel_world(x, z);
	float gj = ground_at(wp);
	float sj = gj + st.y - st.x;
	float tx = texel();
	float tan_r = params.relax.w;
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
		float gi = ground_at(texel_world(q.x, q.y));
		float si = gi + sq.y - sq.x;
		float dist = k < 4 ? tx : tx * 1.41421356;
		float e = (si - sj) - max(tan_r * dist, gi - gj);
		if (e <= 0.0) {
			continue;
		}
		float add = g.x * e / g.y;
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
		cache[o.z * TILE * TILE + k] = packHalf2x16(st.xy);
		return;
	}
	if (gid >= uint(n * n)) {
		return;
	}
	int x = int(gid) % n;
	int z = int(gid) / n;
	int i = int(gid);
	if (push.mode == MODE_SLUMP_GIVE) {
		slump_give(x, z);
	} else if (push.mode == MODE_SLUMP_TAKE) {
		slump_take(x, z);
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
		imageStore(out_tex, ivec2(x, z), vec4(st.x, st.y, 1.0, 1.0));
	} else if (push.mode == MODE_SHIFT) {
		// state is the old field, state_out the new one. tile_ops[new tile] = (kind, index): kind 1 an old
		// tile's index, 2 a cache slot, 0 empty.
		int nt = params.res.y;
		int ti = (z / TILE) * nt + x / TILE;
		ivec4 o = tile_ops[ti];
		int lx = x % TILE;
		int lz = z % TILE;
		vec4 v = vec4(0.0);
		if (o.x == 1) {
			int ox = (o.y % nt) * TILE + lx;
			int oz = (o.y / nt) * TILE + lz;
			v = state[oz * n + ox];
		} else if (o.x == 2) {
			v.xy = unpackHalf2x16(cache[o.y * TILE * TILE + lz * TILE + lx]);
		}
		state_out[i] = v;
	} else if (push.mode == MODE_PACK) {
		vec4 st = state[i];
		packed[i] = packHalf2x16(st.xy);
	}
}

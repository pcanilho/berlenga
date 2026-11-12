#[compute]

#version 450

#VERSION_DEFINES

// GPU tile fill for GrassField3D (docs/ideas/grass.md, stage 3a): one thread per candidate blade,
// written straight into the tile's MultiMesh records (TRANSFORM_3D + custom data, 16 floats).
// The same hashes, clumps, light and packing as GrassField3D::_build_tile_cpu(); placement reads
// the tile's baked suitability grid instead of the rules, so it can differ by a blade at rule edges.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Records {
	float rec[];
};

// Header (H_* floats), then candidates (ix, iz as int bits, rank), suitability grid, light grid,
// height patch.
layout(set = 0, binding = 1, std430) restrict readonly buffer Inputs {
	float inp[];
};

#define H_X0 0
#define H_Z0 1
#define H_CELL 2
#define H_KEEP 3
#define H_WIDTH 4
#define H_HMIN 5
#define H_HMAX 6
#define H_LEAN 7
#define H_CLUMP 8
#define H_GROUND_Y 9
#define H_HAS_MAP 10
#define H_SG_STEP 11
#define H_SG_N 12
#define H_LG 13
#define H_HP_X0 14
#define H_HP_Z0 15
#define H_HP_W 16
#define H_HP_H 17
#define H_MAP_OX 18
#define H_MAP_OZ 19
#define H_MAP_CELL 20
#define H_MAP_N 21
#define H_XFORM 22
#define H_COUNT 34
#define H_OFF_CAND 35
#define H_OFF_SUIT 36
#define H_OFF_LIGHT 37
#define H_OFF_HEIGHT 38

uint murmur3_one(uint p_in, uint p_seed) {
	p_in *= 0xcc9e2d51u;
	p_in = (p_in << 15) | (p_in >> 17);
	p_in *= 0x1b873593u;
	p_seed ^= p_in;
	p_seed = (p_seed << 13) | (p_seed >> 19);
	return p_seed * 5u + 0xe6546b64u;
}

uint fmix32(uint h) {
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	h ^= h >> 16;
	return h;
}

uint cell_hash(int x, int z, uint salt) {
	return fmix32(murmur3_one(uint(z), murmur3_one(uint(x), salt)));
}

float unit(uint h, int shift) {
	return float((h >> uint(shift)) & 0xFFu) / 255.0;
}

float suitability(float x, float z) {
	const int n = int(inp[H_SG_N]);
	const int base = int(inp[H_OFF_SUIT]);
	const float step = inp[H_SG_STEP];
	const float u = clamp((x - inp[H_X0]) / step, 0.0, float(n - 1));
	const float v = clamp((z - inp[H_Z0]) / step, 0.0, float(n - 1));
	const int i0 = min(int(u), n - 2);
	const int j0 = min(int(v), n - 2);
	const float a = inp[base + j0 * n + i0];
	const float b = inp[base + j0 * n + i0 + 1];
	const float c = inp[base + (j0 + 1) * n + i0];
	const float d = inp[base + (j0 + 1) * n + i0 + 1];
	// A corner where nothing may grow (water, a rule's hard edge) keeps the cell empty, so blades
	// never creep past an edge by interpolation.
	if (min(min(a, b), min(c, d)) <= 0.0) {
		return 0.0;
	}
	const float fx = u - float(i0);
	const float fz = v - float(j0);
	return mix(mix(a, b, fx), mix(c, d, fx), fz);
}

float light_at(float x, float z) {
	const int lg = int(inp[H_LG]);
	const int base = int(inp[H_OFF_LIGHT]);
	const float lx = clamp(x - inp[H_X0], 0.0, float(lg - 1));
	const float lz = clamp(z - inp[H_Z0], 0.0, float(lg - 1));
	const int gx = min(int(lx), lg - 2);
	const int gz = min(int(lz), lg - 2);
	const float a = mix(inp[base + gz * lg + gx], inp[base + gz * lg + gx + 1], lx - float(gx));
	const float b = mix(inp[base + (gz + 1) * lg + gx], inp[base + (gz + 1) * lg + gx + 1], lx - float(gx));
	return mix(a, b, lz - float(gz));
}

// TerrainDataMap3D::_sample(CHANNEL_HEIGHT) over the patch of map texels the tile covers.
float height_at(float x, float z) {
	const int n = int(inp[H_MAP_N]);
	const float gx = (x - inp[H_MAP_OX]) / inp[H_MAP_CELL] - 0.5;
	const float gz = (z - inp[H_MAP_OZ]) / inp[H_MAP_CELL] - 0.5;
	const int x0 = clamp(int(floor(gx)), 0, n - 1);
	const int z0 = clamp(int(floor(gz)), 0, n - 1);
	const int x1 = min(x0 + 1, n - 1);
	const int z1 = min(z0 + 1, n - 1);
	const float fx = clamp(gx - float(x0), 0.0, 1.0);
	const float fz = clamp(gz - float(z0), 0.0, 1.0);
	const int hx = int(inp[H_HP_X0]);
	const int hz = int(inp[H_HP_Z0]);
	const int w = int(inp[H_HP_W]);
	const int h = int(inp[H_HP_H]);
	const int base = int(inp[H_OFF_HEIGHT]);
	const int lx0 = clamp(x0 - hx, 0, w - 1);
	const int lx1 = clamp(x1 - hx, 0, w - 1);
	const int lz0 = clamp(z0 - hz, 0, h - 1);
	const int lz1 = clamp(z1 - hz, 0, h - 1);
	const float a = mix(inp[base + lz0 * w + lx0], inp[base + lz0 * w + lx1], fx);
	const float b = mix(inp[base + lz1 * w + lx0], inp[base + lz1 * w + lx1], fx);
	return mix(a, b, fz);
}

void write_zero(uint b) {
	for (uint k = 0u; k < 16u; k++) {
		rec[b + k] = 0.0;
	}
}

void main() {
	const uint i = gl_GlobalInvocationID.x;
	if (i >= uint(inp[H_COUNT])) {
		return;
	}
	const uint b = i * 16u;
	const int cb = int(inp[H_OFF_CAND]) + int(i) * 3;
	const int ix = floatBitsToInt(inp[cb]);
	const int iz = floatBitsToInt(inp[cb + 1]);
	const float rank = inp[cb + 2];
	const float keep = inp[H_KEEP];
	const float cell = inp[H_CELL];

	const uint hb = cell_hash(ix, iz, 0x9E3779B9u);
	const uint hj = cell_hash(ix, iz, 0x85EBCA6Bu);
	const float x = (float(ix) + unit(hj, 0)) * cell;
	const float z = (float(iz) + unit(hj, 8)) * cell;
	float y = inp[H_GROUND_Y];
	if (inp[H_HAS_MAP] > 0.5) {
		const float suit = suitability(x, z);
		if (rank >= keep * suit) {
			write_zero(b);
			return;
		}
		y = height_at(x, z);
	}
	// Clumps (Ghost of Tsushima): blades of a clump share height, facing and dryness.
	const int cx = int(floor(x / inp[H_CLUMP]));
	const int cz = int(floor(z / inp[H_CLUMP]));
	const uint hc = cell_hash(cx, cz, 0xC2B2AE35u);
	const float clump_h = unit(hc, 0);
	const float clump_yaw = unit(hc, 8) * 6.28318530718;
	const float rnd = unit(hb, 16);
	const float light = light_at(x, z);
	const float shade_t = 1.0 - clamp(light, 0.0, 1.0);
	const float sun_t = smoothstep(1.0, 1.3, light);
	const float height = mix(inp[H_HMIN], inp[H_HMAX], 0.65 * clump_h + 0.35 * unit(hj, 16)) * (1.0 + 0.35 * shade_t - 0.15 * sun_t);
	const float yaw = clump_yaw + (unit(hj, 24) - 0.5) * 1.6;

	// to_local * Transform3D(Basis(Y, yaw), (x, y, z)).
	const float c = cos(yaw);
	const float s = sin(yaw);
	const int t = H_XFORM;
	for (int r = 0; r < 3; r++) {
		const float l0 = inp[t + r * 3 + 0];
		const float l1 = inp[t + r * 3 + 1];
		const float l2 = inp[t + r * 3 + 2];
		rec[b + uint(r) * 4u + 0u] = l0 * c - l2 * s;
		rec[b + uint(r) * 4u + 1u] = l1;
		rec[b + uint(r) * 4u + 2u] = l0 * s + l2 * c;
		rec[b + uint(r) * 4u + 3u] = l0 * x + l1 * y + l2 * z + inp[t + 9 + r];
	}
	const float order = floor(fract(rank * 8.0) * 255.0);
	const float blade_lean = min(inp[H_LEAN] * (0.4 + 1.2 * unit(hc, 16)) * (1.0 + 0.8 * shade_t), 3.99);
	const float packed_light = max(1.0, round(clamp(light / 1.5, 0.0, 1.0) * 100.0));
	rec[b + 12u] = height;
	rec[b + 13u] = inp[H_WIDTH];
	rec[b + 14u] = packed_light * 4.0 + blade_lean;
	rec[b + 15u] = order + 0.999 * mod(rnd + 0.37 * unit(hc, 24), 1.0);
}

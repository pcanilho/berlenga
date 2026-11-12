#[compute]
#version 450

// Strand hair on any GPU (RenderingDevice compute), for PhysXHair3D: a port of AMD TressFX 4.1's
// strand simulation (TressFXSimulation.hlsl, github.com/GPUOpen-Effects/TressFX; Copyright (c) 2019
// Advanced Micro Devices, Inc., MIT licence, see COPYRIGHT.txt), one thread
// per guide strand walking it root to tip. Per substep (MODE_STEP):
//   the first two points are pinned to the head; every other point takes a damped Verlet step under
//   gravity and the air (wind, gusts, turbulence) and is pulled toward the groomed pose near the
//   root (global shape); velocity shock propagation carries the strand with the head's motion;
//   iterated local shape keeps each segment's groomed angle to its parent; iterated length
//   constraints keep the segments' lengths; collision spheres push points out and stop them;
//   the speed is clamped.
// MODE_FOLLOW then places the follow strands on their guides (HairWorks / TressFX follow hairs).
// Per frame:
//   MODE_VOL_CLEAR, MODE_VOL_SPLAT (volume on): hair-hair interaction as Petrovic, Henne and
//                Anderson 2005 (Pixar, "Volumetric Methods for Simulation and Rendering of Hair"):
//                the guide points splat their density and momentum into a grid in groom space;
//                in MODE_STEP each point's velocity is pulled toward the grid's (friction between
//                neighbouring strands) and pushed down the gradient of density above the groom's
//                own (pressure, so the hair keeps its volume instead of packing flat).
//   MODE_SDF_* (a collision body is set): TressFX's SDF collision (TressFXSDFCollision.hlsl): the
//                body's triangles (skinned on the CPU) seed the cells they cross with their
//                distance, jump flooding (Rong and Tan 2006) carries the nearest triangle to every
//                cell of the grid, each cell takes its signed distance to it, and every point past
//                the two pinned ones inside the margin is pushed out along the field's gradient.
//   MODE_DOM_*   (deep shadows on): the deep opacity maps of Yuksel and Keyser 2008 (as Unreal's hair),
//                one per light (pass.pad1; a directional light and up to three omni or spot lights):
//                from the light, each cell keeps the first hair's depth, the strands' coverage in
//                three layers past it, and the first opaque depth (the collision spheres and body);
//                the hair shader reads how much hair (and whether the head) lies toward the light.
//   MODE_RESET   (first frame / teleport) every point placed on the groomed pose.
//   MODE_OUTPUT  positions written, in the render copy's space, to an RGBA32F image the hair
//                material reads (no CPU readback), and last frame's positions of the same points to
//                a second image, for the raster's motion vectors (TAA, FSR 2, DLSS).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define DOM_SLOTS 4

#define MODE_STEP 0
#define MODE_RESET 1
#define MODE_OUTPUT 2
#define MODE_FOLLOW 3
#define MODE_SORT_KEYS 4
#define MODE_SORT_STEP 5
#define MODE_SDF_CLEAR 6
#define MODE_SDF_BUILD 7
#define MODE_SDF_COLLIDE 8
#define MODE_SDF_SEED 9
#define MODE_SDF_JUMP 10
#define MODE_SDF_FINAL 11
#define MODE_VOL_CLEAR 12
#define MODE_VOL_SPLAT 13
#define MODE_COLL_CLEAR 20
#define MODE_COLL_INSERT 21
#define MODE_COLL_SOLVE 22
#define MODE_COLL_APPLY 23
#define MODE_SDF_CLOTH 24
#define MODE_DOM_CLEAR 14
#define MODE_DOM_FRONT 15
#define MODE_DOM_OPAQUE 16
#define MODE_DOM_OPAQUE_TRIS 17
#define MODE_DOM_ACCUM 18
#define MODE_DOM_RESOLVE 19

#define MAX_SPHERES 32

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 counts; // x strands, y points, z output width, w spheres
	vec4 step; // x dt (substep), y velocity decay toward the air's per substep, z global shape stiffness (per substep), w global range (share of the strand)
	vec4 accel; // xyz gravity (m/s^2), w local shape stiffness (per iteration)
	vec4 wind; // xyz air velocity (m/s), w gust strength (0..1)
	vec4 misc; // x VSP coefficient, y collision margin (m), z time (s), w VSP acceleration threshold (m/s^2)
	vec4 air; // x speed clamp (m/s), y turbulence (0..1), z turbulence eddy size (m), w share of strands the rasterizer draws (LOD)
	vec4 inertia; // x local shape iterations, y length iterations, z points per output slot (0: no sorting), w sort size (a power of two)
	mat4 xform_prev; // groom space -> world, last frame
	mat4 xform_cur; // groom space -> world, this frame
	mat4 world_to_out; // world -> the render copy's space
	mat4 world_to_view; // world -> the camera, for the draw order
	vec4 spheres[MAX_SPHERES]; // world centre, radius
	vec4 sdf_origin; // xyz the grid's corner (world), w the cell size (m)
	ivec4 sdf_dims; // xyz cells, w triangles (0: no collision body)
	vec4 sdf_misc; // x the distance field's band, y collision margin (m), z wet clumping (0..1), w hair cards: the strands before the card strands (negative: cards only), 0 none
	vec4 vol_origin; // xyz the volume grid's corner (groom space), w its cell (m)
	vec4 vol_misc; // x friction (per substep), y pressure stiffness (m^2/s^2), z mean groom density, w cells a side (0: off)
	vec4 dom_misc; // deep opacity maps: x cells a side, y the strands' root width (m), z their tip width (m), w maps (0: off)
	vec4 dom_layers; // xyz where the three layers end, metres past the first hair
	// Per map (one light each), four vec4: the hair's centre (world) and the radius it covers; the
	// map's first axis and the light's kind (0 directional, 1 omni or spot); the second axis and,
	// for a local light, the tangent of the half angle the hair fills seen from it; toward the
	// light and, for a local light, its distance from the centre.
	vec4 dom_slot[DOM_SLOTS * 4];
	vec4 coll_misc; // strand-strand collision: x radius (m, 0: off), y hash buckets (a power of two), z friction (0..1), w stiffness (0..1)
	vec4 cloth_misc; // cloth: x triangles (0: none), y its position texture's width, z the first of them in tris[], w its thickness (m)
	mat4 cloth_xform; // the cloth's position texture space -> world
};

layout(push_constant, std430) uniform Pass {
	int mode;
	float frac; // substep fraction, for blending the head's transform
	float frac_prev; // the previous substep's
	int pad1; // MODE_SORT_KEYS: cards only, how many (0: every strand); MODE_SORT_STEP: the bitonic block size k (frac holds the stride j); MODE_SDF_JUMP: the step; MODE_OUTPUT: 1 with history
}
pass;

layout(set = 0, binding = 1, std430) restrict readonly buffer Rest {
	vec4 rest[]; // groom-space position, w the length of the segment from the previous point
};

layout(set = 0, binding = 2, std430) restrict readonly buffer Strands {
	ivec2 strands[]; // first point, point count
};

layout(set = 0, binding = 3, std430) restrict buffer Pos {
	vec4 pos[]; // world
};

layout(set = 0, binding = 4, std430) restrict buffer Prev {
	vec4 prev[]; // world, last substep
};

layout(set = 0, binding = 5, rgba32f) uniform restrict writeonly image2D out_image;

layout(set = 0, binding = 12, rgba32f) uniform restrict writeonly image2D prev_image;

layout(set = 0, binding = 14, std430) restrict buffer DomDepth {
	uint dom_depth[]; // per map: cells, the first hair's depth (float bits, atomicMin); then cells more, the first opaque one's
};

layout(set = 0, binding = 15, std430) restrict buffer DomAccum {
	int dom_accum[]; // per map, per cell, three layers: the strands' coverage in it (fixed point, DOM_SCALE)
};

layout(set = 0, binding = 16, rgba32f) uniform restrict writeonly image2D dom_image;

layout(set = 0, binding = 13, std430) restrict buffer Last {
	vec4 last[]; // per point: where MODE_OUTPUT put it last frame (render copy's space)
};

layout(set = 0, binding = 7, std430) restrict buffer Order {
	vec2 order[]; // draw order, far to near: x the strand's depth key, y the strand
};

layout(set = 0, binding = 8, std430) restrict buffer Tris {
	vec4 tris[]; // the collision body's triangles, world: three points each (w 2: a cloth's, MODE_SDF_CLOTH)
};

layout(set = 0, binding = 9, std430) restrict buffer Sdf {
	uint sdf[]; // cells: the distance (atomicMin of its bits while seeding, then the signed float); then cells more: the nearest triangle, or -1
};

layout(set = 0, binding = 10, std430) restrict buffer Vol {
	int vol[]; // per cell: density, momentum xyz (fixed point, VOL_SCALE)
};

layout(set = 0, binding = 11, std430) restrict readonly buffer VolRest {
	float vol_rest[]; // per cell: the groom's own density (guide points), from the CPU
};

layout(set = 0, binding = 17, std430) restrict readonly buffer RenderPoints {
	int render_points[]; // per strand (first, count) into the list after them: the points drawn (local indices)
};

layout(set = 0, binding = 18, std430) restrict buffer CollCells {
	int coll_cells[]; // per hash bucket: point count, then COLL_BUCKET point indices
};

layout(set = 0, binding = 19, std430) restrict buffer CollDelta {
	vec4 coll_delta[]; // per point: the collision's correction (xyz)
};

// A PhysXSkinnedCloth3D the hair rests on: its simulated positions and its triangles' particles.
layout(set = 0, binding = 21, rgba32f) uniform restrict readonly image2D cloth_image;
layout(set = 0, binding = 22, std430) restrict readonly buffer ClothTris {
	ivec4 cloth_tris[]; // three particles a triangle
};

layout(set = 0, binding = 20, std430) restrict readonly buffer PointStrand {
	int point_strand[]; // per point: its strand
};

layout(set = 0, binding = 6, std430) restrict readonly buffer Follow {
	vec4 follow[]; // per strand, two: guide strands (x, y, z; w < 0 for a guide itself), then their weights
};

vec3 to_world(mat4 m, vec3 p) {
	return (m * vec4(p, 1.0)).xyz;
}

// v turned by the rotation taking unit a to unit b (Rodrigues, without trigonometry).
vec3 rotate_between(vec3 a, vec3 b, vec3 v) {
	vec3 k = cross(a, b);
	float c = dot(a, b);
	if (c < -0.999) {
		return v;
	}
	return v * c + cross(k, v) + k * (dot(k, v) / (1.0 + c));
}

float hash11(float x) {
	return fract(sin(x * 12.9898) * 43758.5453);
}

// A fixed random rank in 0..1 per strand (PCG hash), for the LOD subset.
// Hair cards (sdf_misc.w): strands from card_first() on are the cards' own strands, simulated as
// followers and drawn by the card mesh only. Cards only (negative): the other followers are idle.
int card_first() {
	return int(abs(sdf_misc.w) + 0.5);
}

bool is_card_strand(int s) {
	return sdf_misc.w != 0.0 && s >= card_first();
}

bool skipped_by_cards(int s) {
	return sdf_misc.w < 0.0 && s < card_first() && follow[s * 2].w >= 0.0;
}

float strand_rank(int s) {
	uint v = uint(s) * 747796405u + 2891336453u;
	uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
	w = (w >> 22u) ^ w;
	return float(w) * (1.0 / 4294967296.0);
}

// Smooth divergence-light swirl in -1..1 at q (in eddies): two octaves of crossed sines.
vec3 eddies(vec3 q) {
	vec3 a = sin(q.yzx * vec3(1.7, 1.9, 2.1) + q.zxy * vec3(2.3, 2.9, 1.3) + vec3(1.1, 2.3, 0.7));
	q *= 2.31;
	vec3 b = sin(q.zxy * vec3(1.3, 2.2, 1.6) + q.yzx * vec3(2.7, 1.4, 2.5) + vec3(0.4, 1.9, 2.8));
	return (a + 0.5 * b) / 1.5;
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
vec3 closest_on_triangle(vec3 p, vec3 a, vec3 b, vec3 c) {
	vec3 ab = b - a;
	vec3 ac = c - a;
	vec3 ap = p - a;
	float d1 = dot(ab, ap);
	float d2 = dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) {
		return a;
	}
	vec3 bp = p - b;
	float d3 = dot(ab, bp);
	float d4 = dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) {
		return b;
	}
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		return a + ab * (d1 / (d1 - d3));
	}
	vec3 cp = p - c;
	float d5 = dot(ab, cp);
	float d6 = dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) {
		return c;
	}
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		return a + ac * (d2 / (d2 - d6));
	}
	float va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	}
	float denom = 1.0 / (va + vb + vc);
	return a + ab * (vb * denom) + ac * (vc * denom);
}

const uint SDF_FAR = 0x7f000000u; // a huge positive distance

float sdf_cell(ivec3 c) {
	c = clamp(c, ivec3(0), sdf_dims.xyz - 1);
	return uintBitsToFloat(sdf[(c.z * sdf_dims.y + c.y) * sdf_dims.x + c.x]);
}

int sdf_cells() {
	return sdf_dims.x * sdf_dims.y * sdf_dims.z;
}

vec3 sdf_centre(ivec3 c) {
	return sdf_origin.xyz + (vec3(c) + 0.5) * sdf_origin.w;
}

ivec3 sdf_coord(int i) {
	return ivec3(i % sdf_dims.x, (i / sdf_dims.x) % sdf_dims.y, i / (sdf_dims.x * sdf_dims.y));
}

float tri_distance(vec3 p, int t) {
	return length(p - closest_on_triangle(p, tris[t * 3].xyz, tris[t * 3 + 1].xyz, tris[t * 3 + 2].xyz));
}

// Trilinear signed distance at world point p.
float sdf_at(vec3 p) {
	vec3 g = (p - sdf_origin.xyz) / sdf_origin.w - 0.5; // cell centres at +0.5
	ivec3 c = ivec3(floor(g));
	vec3 f = g - vec3(c);
	float x00 = mix(sdf_cell(c), sdf_cell(c + ivec3(1, 0, 0)), f.x);
	float x10 = mix(sdf_cell(c + ivec3(0, 1, 0)), sdf_cell(c + ivec3(1, 1, 0)), f.x);
	float x01 = mix(sdf_cell(c + ivec3(0, 0, 1)), sdf_cell(c + ivec3(1, 0, 1)), f.x);
	float x11 = mix(sdf_cell(c + ivec3(0, 1, 1)), sdf_cell(c + ivec3(1, 1, 1)), f.x);
	return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}


// Strand-strand collision: the spatial hash's bucket for a cell (Teschner et al. 2003's primes).
#define COLL_BUCKET 8
int coll_hash(ivec3 c, int buckets) {
	uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u);
	return int(h & uint(buckets - 1));
}

const float VOL_SCALE = 1024.0;

int vol_index(ivec3 c) {
	int g = int(vol_misc.w);
	return (c.z * g + c.y) * g + c.x;
}

// Trilinear density (x, in guide points per cell) and velocity (yzw, world) at groom-space q.
vec4 vol_sample(vec3 q) {
	int g = int(vol_misc.w);
	vec3 u = (q - vol_origin.xyz) / vol_origin.w - 0.5;
	ivec3 c = ivec3(floor(u));
	vec3 f = u - vec3(c);
	vec4 acc = vec4(0.0);
	for (int k = 0; k < 8; k++) {
		ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
		ivec3 cc = c + o;
		if (any(lessThan(cc, ivec3(0))) || any(greaterThanEqual(cc, ivec3(g)))) {
			continue;
		}
		vec3 wv = mix(1.0 - f, f, vec3(o));
		float w = wv.x * wv.y * wv.z;
		int i = vol_index(cc) * 4;
		acc += w * vec4(vol[i], vol[i + 1], vol[i + 2], vol[i + 3]);
	}
	acc /= VOL_SCALE;
	return vec4(acc.x, acc.x > 1e-4 ? acc.yzw / acc.x : vec3(0.0));
}

// The density above the groom's own (never below half the groom's mean, so hair blown into
// empty space may gather some before it pushes), relative to that mean.
float vol_excess(vec3 q) {
	int g = int(vol_misc.w);
	vec3 u = (q - vol_origin.xyz) / vol_origin.w - 0.5;
	ivec3 c = ivec3(floor(u));
	vec3 f = u - vec3(c);
	float cur = 0.0;
	float rest = 0.0;
	for (int k = 0; k < 8; k++) {
		ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
		ivec3 cc = c + o;
		if (any(lessThan(cc, ivec3(0))) || any(greaterThanEqual(cc, ivec3(g)))) {
			continue;
		}
		vec3 wv = mix(1.0 - f, f, vec3(o));
		float w = wv.x * wv.y * wv.z;
		int i = vol_index(cc);
		cur += w * float(vol[i * 4]) / VOL_SCALE;
		rest += w * vol_rest[i];
	}
	return max(cur - max(rest, 0.5 * vol_misc.z), 0.0) / max(vol_misc.z, 1e-3);
}


// Deep opacity maps (Yuksel and Keyser 2008), one per light, from the light: cells across, depth
// along the light's rays. A directional light's map is orthographic, its depth measured from the
// plane `radius` toward the light from the centre; an omni or spot light's is a perspective view
// from the light that just holds the hair's bounding sphere (as Unreal's hair deep shadows for
// local lights), its depth the distance from the light less that to the sphere's near side.
const float DOM_SCALE = 65536.0;
const uint DOM_FAR = 0x7f000000u;

// The map pass.pad1 works on, read once in main().
vec4 slot_c;
vec4 slot_u;
vec4 slot_v;
vec4 slot_l;

bool dom_local() {
	return slot_u.w > 0.5;
}

vec3 dom_light_pos() {
	return slot_c.xyz + slot_l.xyz * slot_l.w;
}

vec3 dom_map(vec3 p) {
	int n = int(dom_misc.x);
	if (!dom_local()) {
		vec3 d = p - slot_c.xyz;
		float size = 2.0 * slot_c.w;
		return vec3((dot(d, slot_u.xyz) / size + 0.5) * float(n) - 0.5, (dot(d, slot_v.xyz) / size + 0.5) * float(n) - 0.5, max(slot_c.w - dot(d, slot_l.xyz), 0.0));
	}
	vec3 d = p - dom_light_pos();
	float z = max(-dot(d, slot_l.xyz), 1e-4);
	vec2 s = vec2(dot(d, slot_u.xyz), dot(d, slot_v.xyz)) / (z * slot_v.w);
	return vec3((s * 0.5 + 0.5) * float(n) - 0.5, max(length(d) - (slot_l.w - slot_c.w), 0.0));
}

// The light's ray through p, from the light (world, unit).
vec3 dom_ray(vec3 p) {
	return dom_local() ? normalize(p - dom_light_pos()) : -slot_l.xyz;
}

// A cell's width at p (m).
float dom_cell(vec3 p) {
	float n = dom_misc.x;
	if (!dom_local()) {
		return 2.0 * slot_c.w / n;
	}
	return 2.0 * max(-dot(p - dom_light_pos(), slot_l.xyz), 1e-4) * slot_v.w / n;
}

// The strands' segments, sampled once per cell they cross, each sample into its nearest cell (as a
// line rasterized into the map, the way Unreal draws strands into its deep shadow maps; the hair
// shader filters across 4 cells when it reads): MODE_DOM_FRONT takes each cell's nearest hair,
// MODE_DOM_ACCUM adds each sample's coverage (its width times its length over the cell's area,
// TressFX's strand coverage) to the layer it falls in. Against two samples per cell splatted
// bilinearly into 4 cells the picture is the same (mean 0.04 levels) for a third of the cost.
void dom_strand(int s, int p_slot, bool accumulate) {
	int n = int(dom_misc.x);
	int cells = n * n;
	int first = strands[s].x;
	int count = strands[s].y;
	for (int i = 0; i + 1 < count; i++) {
		vec3 a = pos[first + i].xyz;
		vec3 b = pos[first + i + 1].xyz;
		vec3 ab = b - a;
		vec3 mid = 0.5 * (a + b);
		vec3 ray = dom_ray(mid);
		float cell = dom_cell(mid);
		float across = length(ab - ray * dot(ab, ray));
		int samples = clamp(int(ceil(across / cell)), 1, 64);
		float along = (float(i) + 0.5) / float(max(count - 1, 1));
		float width = mix(dom_misc.y, dom_misc.z, along);
		float coverage = width * (across / float(samples)) / (cell * cell);
		// A hair card's strand stands for its whole clump (width is the clump's): its coverage is
		// spread over a square as wide as the clump, as the clump's own strands would lie, not
		// heaped in one cell (which would let light through the gaps beside it).
		if (is_card_strand(s)) {
			width *= clamp(pos[first + i].w, 0.5, 4.0); // the clump fanned out with its guides
		}
		int spread = is_card_strand(s) ? clamp(int(0.5 * width / cell), 0, 4) : 0;
		float share = coverage / float((2 * spread + 1) * (2 * spread + 1));
		for (int k = 0; k < samples; k++) {
			vec3 m = dom_map(mix(a, b, (float(k) + 0.5) / float(samples)));
			for (int dy = -spread; dy <= spread; dy++) {
				for (int dx = -spread; dx <= spread; dx++) {
					ivec2 cc = ivec2(floor(m.xy + 0.5)) + ivec2(dx, dy);
					if (any(lessThan(cc, ivec2(0))) || any(greaterThanEqual(cc, ivec2(n)))) {
						continue;
					}
					int ci = cc.y * n + cc.x;
					if (!accumulate) {
						atomicMin(dom_depth[p_slot * cells * 2 + ci], floatBitsToUint(m.z));
						continue;
					}
					float rel = m.z - uintBitsToFloat(dom_depth[p_slot * cells * 2 + ci]);
					int layer = rel < dom_layers.x ? 0 : (rel < dom_layers.y ? 1 : 2);
					atomicAdd(dom_accum[(p_slot * cells + ci) * 3 + layer], int(share * DOM_SCALE + 0.5));
				}
			}
		}
	}
}

void main() {
	int id = int(gl_GlobalInvocationID.x);

	int slot_points = int(inertia.z + 0.5);
	if (pass.mode >= MODE_DOM_CLEAR && pass.mode <= MODE_DOM_RESOLVE) {
		int n = int(dom_misc.x);
		int cells = n * n;
		// pass.pad1: which map (light).
		int slot = clamp(pass.pad1, 0, DOM_SLOTS - 1);
		slot_c = dom_slot[slot * 4];
		slot_u = dom_slot[slot * 4 + 1];
		slot_v = dom_slot[slot * 4 + 2];
		slot_l = dom_slot[slot * 4 + 3];
		int depth_base = slot * cells * 2;
		if (pass.mode == MODE_DOM_CLEAR) {
			if (id < cells) {
				dom_depth[depth_base + id] = DOM_FAR;
				dom_depth[depth_base + cells + id] = DOM_FAR;
				dom_accum[(slot * cells + id) * 3] = 0;
				dom_accum[(slot * cells + id) * 3 + 1] = 0;
				dom_accum[(slot * cells + id) * 3 + 2] = 0;
			}
		} else if (pass.mode == MODE_DOM_FRONT || pass.mode == MODE_DOM_ACCUM) {
			// The cards' strands make the maps while the hair is drawn as cards, the others otherwise.
			if (id < counts.x && (sdf_misc.w < 0.0) == is_card_strand(id)) {
				dom_strand(id, slot, pass.mode == MODE_DOM_ACCUM);
			}
		} else if (pass.mode == MODE_DOM_OPAQUE) {
			// The collision spheres (skull, neck, shoulders) are opaque: rays from each cell along the
			// light, nearest hit kept, so the head shadows the hair behind it.
			if (id < cells) {
				vec2 uv = (vec2(id % n, id / n) + 0.5) / float(n) - 0.5;
				vec3 o;
				vec3 dir;
				float t0;
				if (dom_local()) {
					// From the light through the cell; depth from the sphere's near side.
					o = dom_light_pos();
					dir = normalize(-slot_l.xyz + (slot_u.xyz * uv.x + slot_v.xyz * uv.y) * (2.0 * slot_v.w));
					t0 = slot_l.w - slot_c.w;
				} else {
					o = slot_c.xyz + (slot_u.xyz * uv.x + slot_v.xyz * uv.y) * (2.0 * slot_c.w) + slot_l.xyz * slot_c.w;
					dir = -slot_l.xyz;
					t0 = 0.0;
				}
				float nearest = uintBitsToFloat(DOM_FAR);
				for (int k = 0; k < counts.w; k++) {
					vec3 oc = o - spheres[k].xyz;
					float b = dot(oc, dir);
					float disc = b * b - (dot(oc, oc) - spheres[k].w * spheres[k].w);
					if (disc > 0.0) {
						float t = -b - sqrt(disc);
						if (t > 0.0) {
							nearest = min(nearest, max(t - t0, 0.0));
						}
					}
				}
				dom_depth[depth_base + cells + id] = floatBitsToUint(nearest);
			}
		} else if (pass.mode == MODE_DOM_OPAQUE_TRIS) {
			// The collision body's triangles, rasterized into the map (nearest depth; for a local
			// light the depth is interpolated across the triangle, fine at the body's triangle size).
			if (id < sdf_dims.w) {
				vec3 a = dom_map(tris[id * 3].xyz);
				vec3 b = dom_map(tris[id * 3 + 1].xyz);
				vec3 c = dom_map(tris[id * 3 + 2].xyz);
				ivec2 lo = max(ivec2(ceil(min(a.xy, min(b.xy, c.xy)))), ivec2(0));
				ivec2 hi = min(ivec2(floor(max(a.xy, max(b.xy, c.xy)))), ivec2(n - 1));
				hi = min(hi, lo + ivec2(63)); // a huge triangle stays bounded
				float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
				if (abs(area) > 1e-8) {
					for (int y = lo.y; y <= hi.y; y++) {
						for (int x = lo.x; x <= hi.x; x++) {
							vec2 q = vec2(x, y);
							float w0 = ((b.x - q.x) * (c.y - q.y) - (b.y - q.y) * (c.x - q.x)) / area;
							float w1 = ((c.x - q.x) * (a.y - q.y) - (c.y - q.y) * (a.x - q.x)) / area;
							float w2 = 1.0 - w0 - w1;
							if (w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0) {
								atomicMin(dom_depth[depth_base + cells + y * n + x], floatBitsToUint(w0 * a.z + w1 * b.z + w2 * c.z));
							}
						}
					}
				}
			}
		} else if (pass.mode == MODE_DOM_RESOLVE) {
			// x the first hair's depth, yzw the coverage summed to each layer's end; and beside it
			// the first opaque depth. Map `slot` is the slot-th row of maps in the image.
			if (id < cells) {
				ivec2 c = ivec2(id % n, id / n);
				int ai = (slot * cells + id) * 3;
				float c1 = float(dom_accum[ai]) / DOM_SCALE;
				float c2 = c1 + float(dom_accum[ai + 1]) / DOM_SCALE;
				float c3 = c2 + float(dom_accum[ai + 2]) / DOM_SCALE;
				uint front = dom_depth[depth_base + id];
				ivec2 at = c + ivec2(0, slot * n);
				imageStore(dom_image, at, vec4(front == DOM_FAR ? 1e9 : uintBitsToFloat(front), c1, c2, c3));
				uint opaque = dom_depth[depth_base + cells + id];
				imageStore(dom_image, at + ivec2(n, 0), vec4(opaque == DOM_FAR ? 1e9 : uintBitsToFloat(opaque), 0.0, 0.0, 0.0));
			}
		}
		return;
	}

	if (pass.mode >= MODE_COLL_CLEAR && pass.mode <= MODE_COLL_APPLY) {
		// Strand-strand collision between guide strands (position based, Muller et al. 2007, as
		// the particle contacts of Macklin et al. 2014): the guides' points go into a spatial hash
		// of cells one collision diameter wide, and two points of different strands closer than
		// the diameter are pushed apart along the line between them, half each, with friction
		// taking off part of their sliding. The grid (MODE_VOL_*) keeps the hair's volume at the
		// cell scale (2 cm); this keeps neighbouring guides, and so the clumps that follow them,
		// from passing through each other below it.
		float r = coll_misc.x;
		int buckets = int(coll_misc.y);
		float cell = 2.0 * r;
		if (pass.mode == MODE_COLL_CLEAR) {
			if (id < buckets) {
				coll_cells[id * (COLL_BUCKET + 1)] = 0;
			}
			return;
		}
		if (id >= counts.y) {
			return;
		}
		int s = point_strand[id];
		bool guide = follow[s * 2].w < 0.0;
		int local_i = id - strands[s].x;
		if (pass.mode == MODE_COLL_APPLY) {
			// Position based: the correction also becomes velocity (position minus previous), so
			// the strands part instead of grinding through each other next step.
			vec4 d = coll_delta[id];
			if (guide && local_i >= 2 && dot(d.xyz, d.xyz) > 0.0) {
				pos[id].xyz += d.xyz;
			}
			coll_delta[id] = vec4(0.0);
			return;
		}
		if (!guide || local_i < 1) {
			return;
		}
		vec3 p = pos[id].xyz;
		ivec3 c = ivec3(floor(p / cell));
		if (pass.mode == MODE_COLL_INSERT) {
			int b = coll_hash(c, buckets) * (COLL_BUCKET + 1);
			int slot = atomicAdd(coll_cells[b], 1);
			if (slot < COLL_BUCKET) {
				coll_cells[b + 1 + slot] = id;
			}
			return;
		}
		// MODE_COLL_SOLVE: the 27 cells around, Jacobi (each point its own correction).
		if (local_i < 2) {
			return;
		}
		vec3 corr = vec3(0.0);
		int touching = 0;
		vec3 v = p - prev[id].xyz;
		vec3 slide = vec3(0.0);
		for (int k = 0; k < 27; k++) {
			ivec3 cc = c + ivec3(k % 3, (k / 3) % 3, k / 9) - 1;
			int b = coll_hash(cc, buckets) * (COLL_BUCKET + 1);
			int nb = min(coll_cells[b], COLL_BUCKET);
			for (int j = 0; j < nb; j++) {
				int o = coll_cells[b + 1 + j];
				if (point_strand[o] == s) {
					continue;
				}
				vec3 q = pos[o].xyz;
				vec3 d = p - q;
				float dist2 = dot(d, d);
				if (dist2 >= 4.0 * r * r || dist2 < 1e-14) {
					continue;
				}
				float dist = sqrt(dist2);
				vec3 nrm = d / dist;
				corr += nrm * (0.5 * (2.0 * r - dist));
				// Relative sliding this frame, for the friction.
				vec3 rel = v - (q - prev[o].xyz);
				slide += rel - nrm * dot(rel, nrm);
				touching++;
			}
		}
		if (touching > 0) {
			// Averaged over the contacts (Jacobi, as Macklin's constraint averaging), so a point
			// pressed from many sides moves no further than one contact would push it.
			corr *= coll_misc.w / float(touching);
			corr -= coll_misc.z * slide / float(touching); // friction: part of the sliding undone
			coll_delta[id] = vec4(corr, 0.0);
		}
		return;
	}

	if (pass.mode == MODE_VOL_CLEAR) {
		int g = int(vol_misc.w);
		if (id < g * g * g * 4) {
			vol[id] = 0;
		}
		return;
	}
	if (pass.mode == MODE_VOL_SPLAT) {
		// One thread per guide strand: every point splats its density and momentum into the 8
		// cells around it (trilinear weights, fixed point atomics).
		if (id >= counts.x || follow[id * 2].w >= 0.0) {
			return;
		}
		int g = int(vol_misc.w);
		mat4 to_groom = inverse(xform_cur);
		float inv_dt = step.x > 0.0 ? 1.0 / step.x : 0.0;
		int first = strands[id].x;
		int n = strands[id].y;
		for (int i = 1; i < n; i++) {
			vec3 p = pos[first + i].xyz;
			vec3 v = (p - prev[first + i].xyz) * inv_dt;
			vec3 u = ((to_groom * vec4(p, 1.0)).xyz - vol_origin.xyz) / vol_origin.w - 0.5;
			ivec3 c = ivec3(floor(u));
			vec3 f = u - vec3(c);
			for (int k = 0; k < 8; k++) {
				ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
				ivec3 cc = c + o;
				if (any(lessThan(cc, ivec3(0))) || any(greaterThanEqual(cc, ivec3(g)))) {
					continue;
				}
				vec3 wv = mix(1.0 - f, f, vec3(o));
				float w = wv.x * wv.y * wv.z;
				int ci = vol_index(cc) * 4;
				atomicAdd(vol[ci], int(w * VOL_SCALE));
				atomicAdd(vol[ci + 1], int(w * v.x * VOL_SCALE));
				atomicAdd(vol[ci + 2], int(w * v.y * VOL_SCALE));
				atomicAdd(vol[ci + 3], int(w * v.z * VOL_SCALE));
			}
		}
		return;
	}
	if (pass.mode == MODE_SDF_CLEAR) {
		if (id < sdf_cells()) {
			sdf[id] = SDF_FAR;
			sdf[sdf_cells() + id] = 0xffffffffu; // no triangle
		}
		return;
	}
	if (pass.mode == MODE_SDF_BUILD || pass.mode == MODE_SDF_SEED) {
		// One thread per triangle, over the cells around it (TressFX ConstructSignedDistanceField):
		// BUILD keeps each cell's smallest distance (atomicMin of the float's bits, positive
		// floats order as their bits), SEED then marks the triangle that has it.
		if (id >= sdf_dims.w) {
			return;
		}
		vec3 a = tris[id * 3].xyz;
		vec3 b = tris[id * 3 + 1].xyz;
		vec3 c = tris[id * 3 + 2].xyz;
		float cell = sdf_origin.w;
		ivec3 lo = max(ivec3(floor((min(a, min(b, c)) - cell - sdf_origin.xyz) / cell)), ivec3(0));
		ivec3 hi = min(ivec3(floor((max(a, max(b, c)) + cell - sdf_origin.xyz) / cell)), sdf_dims.xyz - 1);
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					vec3 p = sdf_centre(ivec3(x, y, z));
					uint key = floatBitsToUint(length(p - closest_on_triangle(p, a, b, c)));
					int ci = (z * sdf_dims.y + y) * sdf_dims.x + x;
					if (pass.mode == MODE_SDF_BUILD) {
						atomicMin(sdf[ci], key);
					} else if (sdf[ci] == key) {
						sdf[sdf_cells() + ci] = uint(id);
					}
				}
			}
		}
		return;
	}
	if (pass.mode == MODE_SDF_JUMP) {
		// Jump flooding, step pad1 cells: a cell takes a neighbour's triangle when it is nearer.
		if (id >= sdf_cells()) {
			return;
		}
		ivec3 c = sdf_coord(id);
		vec3 p = sdf_centre(c);
		uint best = sdf[sdf_cells() + id];
		float best_d = best == 0xffffffffu ? 3.0e38 : tri_distance(p, int(best));
		int k = pass.pad1;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					ivec3 n = c + ivec3(x, y, z) * k;
					if (any(lessThan(n, ivec3(0))) || any(greaterThanEqual(n, sdf_dims.xyz))) {
						continue;
					}
					uint t = sdf[sdf_cells() + (n.z * sdf_dims.y + n.y) * sdf_dims.x + n.x];
					if (t == 0xffffffffu || t == best) {
						continue;
					}
					float d = tri_distance(p, int(t));
					if (d < best_d) {
						best_d = d;
						best = t;
					}
				}
			}
		}
		sdf[sdf_cells() + id] = best;
		return;
	}
	if (pass.mode == MODE_SDF_CLOTH) {
		// The cloth's triangles from its simulated positions this frame, after the body's, marked
		// (w 2) as a sheet.
		int n = int(cloth_misc.x + 0.5);
		if (id >= n) {
			return;
		}
		int w = max(int(cloth_misc.y + 0.5), 1);
		int base = (int(cloth_misc.z + 0.5) + id) * 3;
		for (int k = 0; k < 3; k++) {
			int p = cloth_tris[id][k];
			vec3 q = imageLoad(cloth_image, ivec2(p % w, p / w)).xyz;
			tris[base + k] = vec4((cloth_xform * vec4(q, 1.0)).xyz, 2.0);
		}
		return;
	}
	if (pass.mode == MODE_SDF_FINAL) {
		// The signed distance to the nearest triangle, signed by its face (TressFX's choice; right
		// past convex edges, approximate in concave ones). A closed body is assumed. A cloth is a
		// sheet worn over the body: signed the same way (its outside is where its normals face),
		// so hair that starts under it near the surface is put back on top, as hair lies over
		// clothes; less its thickness.
		if (id >= sdf_cells()) {
			return;
		}
		uint t = sdf[sdf_cells() + id];
		if (t == 0xffffffffu) {
			sdf[id] = SDF_FAR;
			return;
		}
		vec3 p = sdf_centre(sdf_coord(id));
		vec3 a = tris[t * 3].xyz;
		vec3 b = tris[t * 3 + 1].xyz;
		vec3 c = tris[t * 3 + 2].xyz;
		vec3 q = closest_on_triangle(p, a, b, c);
		float d = length(p - q);
		vec3 n = cross(b - a, c - a);
		float sd = dot(p - q, n) < 0.0 ? -d : d;
		// A cloth's triangles: less its thickness, so hair rests on its surface.
		sdf[id] = floatBitsToUint(tris[t * 3].w > 1.5 ? sd - cloth_misc.w : sd);
		return;
	}
	if (pass.mode == MODE_SDF_COLLIDE) {
		// One thread per strand (followers too, after MODE_FOLLOW), past the two pinned points:
		// a point closer than the margin is pushed out along the gradient and stops there, as on
		// the collision spheres (TressFX CollideHairVerticesWithSdf).
		if (id >= counts.x || skipped_by_cards(id)) {
			return;
		}
		int first = strands[id].x;
		int n = strands[id].y;
		float card_margin = is_card_strand(id) ? rest[first].w : 0.0; // see MODE_FOLLOW
		vec3 lo = sdf_origin.xyz;
		vec3 hi = sdf_origin.xyz + vec3(sdf_dims.xyz) * sdf_origin.w;
		float h = 0.5 * sdf_origin.w;
		for (int i = 2; i < n; i++) {
			vec3 p = pos[first + i].xyz;
			if (any(lessThan(p, lo)) || any(greaterThan(p, hi))) {
				continue;
			}
			float d = sdf_at(p);
			float margin = sdf_misc.y + card_margin;
			if (d >= margin) {
				continue;
			}
			vec3 g = vec3(sdf_at(p + vec3(h, 0, 0)) - sdf_at(p - vec3(h, 0, 0)),
					sdf_at(p + vec3(0, h, 0)) - sdf_at(p - vec3(0, h, 0)),
					sdf_at(p + vec3(0, 0, h)) - sdf_at(p - vec3(0, 0, h)));
			if (dot(g, g) < 1e-12) {
				continue;
			}
			p += normalize(g) * (margin - d);
			pos[first + i] = vec4(p, pos[first + i].w); // w: a card strand's fan (MODE_FOLLOW)
			prev[first + i] = vec4(p, 1.0);
		}
		return;
	}
	if (pass.mode == MODE_SORT_KEYS) {
		// Blended strands must be drawn far to near (TressFX sorts its fragments per pixel; here
		// whole strands are sorted by their middle point's depth, a bitonic sort on the GPU).
		// pass.pad1 > 0: hair cards only, nothing else drawn: just the cards' strands are sorted,
		// pad1 of them (a power of two) from card_first() on.
		if (id >= (pass.pad1 > 0 ? pass.pad1 : int(inertia.w + 0.5))) {
			return;
		}
		int s = pass.pad1 > 0 ? card_first() + id : id;
		float key = 3.0e38; // padding sorts last
		if (s < counts.x && strands[s].y > 0) {
			vec3 mid = pos[strands[s].x + strands[s].y / 2].xyz;
			key = (world_to_view * vec4(mid, 1.0)).z; // more negative is farther
			if (is_card_strand(s)) {
				// Hair cards' strands take the first slots (the card mesh draws them), far to near
				// among themselves as the strands are: blended out of order, a card behind drew over
				// one in front, and the hair read lighter and thinner than its strands.
				key -= 1.0e7;
			}
		}
		order[id] = vec2(key, float(s));
		return;
	}
	if (pass.mode == MODE_SORT_STEP) {
		int j = int(pass.frac + 0.5);
		int partner = id ^ j;
		if (id >= int(inertia.w + 0.5) || partner <= id) {
			return;
		}
		bool ascending = (id & pass.pad1) == 0;
		vec2 a = order[id];
		vec2 b = order[partner];
		if ((a.x > b.x) == ascending) {
			order[id] = b;
			order[partner] = a;
		}
		return;
	}
	if (pass.mode == MODE_OUTPUT) {
		int w = max(counts.z, 1);
		// pass.pad1: 1 when last[] holds last frame's positions (0 after a build or reset: no motion).
		bool history = pass.pad1 == 1;
		if (slot_points <= 0) {
			if (id >= counts.y) {
				return;
			}
			vec3 o = to_world(world_to_out, pos[id].xyz);
			vec3 before = history ? last[id].xyz : o;
			last[id] = vec4(o, 1.0);
			imageStore(out_image, ivec2(id % w, id / w), vec4(o, 1.0));
			imageStore(prev_image, ivec2(id % w, id / w), vec4(before, 1.0));
			return;
		}
		// Sorted: slot s holds the s-th strand from the back, slot_points texels each; w is the
		// share of the strand's length (by point), or -1 past its last point.
		if (id >= counts.x * slot_points) {
			return;
		}
		// The slot's k-th texel is the strand's k-th drawn point (straight runs leave some out).
		int strand = int(order[id / slot_points].y + 0.5);
		int k = id % slot_points;
		int sn = strands[strand].y;
		int rn = render_points[strand * 2 + 1];
		int ri = render_points[strand * 2] + clamp(k, 0, max(rn - 1, 0));
		int local_k = clamp(render_points[counts.x * 2 + ri], 0, max(sn - 1, 0));
		int sp = strands[strand].x + local_k;
		float along = k < rn ? float(local_k) / float(max(sn - 1, 1)) : -1.0;
		// Distance LOD (TressFX): a fixed random subset of the strands is left out of the raster
		// draw, flagged by along + 2; the render shader widens the rest to keep the hair's opacity.
		// The path tracer still draws every strand at its real width.
		if (along >= 0.0 && is_card_strand(strand)) {
			// A hair card's strand (4 + along + 8 * fan in 64ths): drawn by the card mesh, not as a
			// strand.
			along += 4.0 + 8.0 * floor(clamp(pos[sp].w, 0.0, 8.0) * 64.0 + 0.5);
		} else if (along >= 0.0 && strand_rank(strand) >= air.w) {
			along += 2.0;
		}
		vec3 o = to_world(world_to_out, pos[sp].xyz);
		vec3 before = o;
		if (k < rn) {
			// One texel per real point (padding past the end only repeats the last one), so each
			// point's history is read and replaced by a single thread.
			before = history ? last[sp].xyz : o;
			last[sp] = vec4(o, 1.0);
		}
		imageStore(out_image, ivec2(id % w, id / w), vec4(o, along));
		// A card's slot: the previous positions' w names the card drawn there (-2 - card), since
		// the slots follow the draw order. Negative, it reads as past the strand's end to the
		// strand mesh.
		imageStore(prev_image, ivec2(id % w, id / w), vec4(before, is_card_strand(strand) ? -2.0 - float(strand - card_first()) : along));
		return;
	}

	if (id >= counts.x) {
		return;
	}
	int first = strands[id].x;
	int n = strands[id].y;
	if (n <= 0) {
		return;
	}
	bool is_guide = follow[id * 2].w < 0.0;
	// A hair card's strand is the centre of a slab of strands: it keeps half the slab's
	// thickness (held in its first rest point's w, unused otherwise) more off the body, so the
	// card's strands rest on it rather than half inside it.
	float card_margin = is_card_strand(id) ? rest[first].w : 0.0;

	if (pass.mode == MODE_RESET) {
		for (int i = 0; i < n; i++) {
			vec3 p = to_world(xform_cur, rest[first + i].xyz);
			pos[first + i] = vec4(p, 1.0);
			prev[first + i] = vec4(p, 1.0);
		}
		return;
	}

	// The head's transform this substep (blended; fine for the small turn of one frame).
	mat4 m = xform_prev + (xform_cur - xform_prev) * pass.frac;
	mat3 basis = mat3(m);

	if (pass.mode == MODE_FOLLOW) {
		// HairWorks / TressFX follow hairs: only guide strands simulate; every other strand is its
		// groomed self carried by its nearest guides: each guide's point at the same share of its
		// length, plus the groomed offset from it turned with the guide's segment. The strands move
		// in clumps and keep their own curl.
		if (is_guide || skipped_by_cards(id)) {
			return;
		}
		vec4 gi = follow[id * 2];
		vec4 gw = follow[id * 2 + 1];
		// Wet hair clumps: the water between neighbouring fibres pulls them together where their
		// free length beats their bending stiffness (elastocapillary coalescence, Bico et al. 2004),
		// so the clumps close from a little past the root out to the tips. Each follower's groomed
		// offset from its guides shrinks by the clumping (Gupta and Magnenat-Thalmann 2007 pull the
		// child strands to their guides the same way).
		float clump = sdf_misc.z;
		for (int i = 0; i < n; i++) {
			float t = float(i) / float(max(n - 1, 1));
			float keep = 1.0 - clump * smoothstep(0.0, 0.3, t);
			vec3 own = to_world(m, rest[first + i].xyz);
			vec3 p = vec3(0.0);
			vec3 g_now[3];
			vec3 g_rest[3];
			for (int c = 0; c < 3; c++) {
				g_now[c] = vec3(0.0);
				g_rest[c] = vec3(0.0);
				float w = gw[c];
				if (w <= 0.0) {
					continue;
				}
				int g = int(gi[c] + 0.5);
				int gf = strands[g].x;
				int gn = strands[g].y;
				float fi = t * float(gn - 1);
				int i0 = min(int(fi), gn - 1);
				int i1 = min(i0 + 1, gn - 1);
				float f = fi - float(i0);
				int s0 = i0 == i1 ? max(i0 - 1, 0) : i0;
				vec3 gp = mix(pos[gf + i0].xyz, pos[gf + i1].xyz, f);
				vec3 gr = to_world(m, mix(rest[gf + i0].xyz, rest[gf + i1].xyz, f));
				vec3 d_cur = pos[gf + i1].xyz - pos[gf + s0].xyz;
				vec3 d_rest = basis * (rest[gf + i1].xyz - rest[gf + s0].xyz);
				g_now[c] = gp;
				g_rest[c] = gr;
				vec3 off = (own - gr) * keep;
				if (dot(d_cur, d_cur) > 1e-14 && dot(d_rest, d_rest) > 1e-14) {
					off = rotate_between(normalize(d_rest), normalize(d_cur), off);
				}
				p += w * (gp + off);
			}
			if (i == 0) {
				p = own;
			}
			for (int s = 0; s < counts.w; s++) {
				vec3 d = p - spheres[s].xyz;
				float r = spheres[s].w + misc.y + card_margin;
				float l2 = dot(d, d);
				if (l2 < r * r && l2 > 1e-14) {
					p = spheres[s].xyz + d * (r / sqrt(l2));
				}
			}
			// A hair card's strand: how far its guides have fanned apart (or closed) since the groom,
			// in w. Its clump's strands sit between the same guides, so the clump widens with them;
			// the card widens and thins by as much (its strands' total width kept).
			float fan = 1.0;
			if (is_card_strand(id)) {
				float d_now = 0.0;
				float d_rest = 0.0;
				for (int c = 0; c < 3; c++) {
					int e = (c + 1) % 3;
					if (gw[c] > 0.0 && gw[e] > 0.0) {
						d_now += distance(g_now[c], g_now[e]);
						d_rest += distance(g_rest[c], g_rest[e]);
					}
				}
				fan = d_rest > 1e-5 ? clamp(d_now / d_rest, 0.5, 4.0) : 1.0;
			}
			pos[first + i] = vec4(p, fan);
		}
		return;
	}
	if (!is_guide) {
		return;
	}

	// AMD TressFX 4.1 simulation (MIT; TressFXSimulation.hlsl), one thread per guide strand, in
	// metres and seconds. Per substep: integrate, global shape, velocity shock propagation, local
	// shape, length constraints, collisions, then a speed clamp.
	float dt = step.x;
	// A zero step (the first frame after building) only places the strands.
	float inv_dt = dt > 0.0 ? 1.0 / dt : 0.0;

	// The first two points are pinned to the head (TressFX's immovable vertices).
	vec3 old0 = pos[first].xyz;
	vec3 new0 = to_world(m, rest[first].xyz);
	prev[first] = vec4(old0, 1.0);
	pos[first] = vec4(new0, 1.0);
	if (n < 2) {
		return;
	}
	vec3 old1 = pos[first + 1].xyz;
	vec3 oldold1 = prev[first + 1].xyz;
	vec3 new1 = to_world(m, rest[first + 1].xyz);
	prev[first + 1] = vec4(old1, 1.0);
	pos[first + 1] = vec4(new1, 1.0);
	if (n < 3) {
		return;
	}

	// Integrate (Verlet). TressFX's damping, decay = exp(-damping * 60 dt), here relaxes the
	// velocity toward the air's rather than toward rest: in still air it is TressFX's, in wind the
	// strand takes up the air's velocity at that rate. The air carries gusts and turbulence (eddies
	// blown along with the wind, Taylor's frozen turbulence) so neighbouring strands flutter together.
	float gust = 1.0 + wind.w * sin(misc.z * 2.3 + hash11(float(id)) * 6.2832) * (0.6 + 0.4 * sin(misc.z * 0.7 + float(id) * 0.013));
	vec3 air_v = wind.xyz * gust;
	float wind_speed = length(wind.xyz);
	float decay = step.y;
	float global_range = step.w * float(n);
	bool volume = vol_misc.w > 0.5;
	mat4 to_groom = volume ? inverse(xform_cur) : mat4(1.0);
	for (int i = 2; i < n; i++) {
		int k = first + i;
		vec3 p = pos[k].xyz;
		vec3 v = (p - prev[k].xyz) * inv_dt;
		vec3 a = air_v;
		if (air.y > 0.0 && wind_speed > 1e-3) {
			a += air.y * wind_speed * eddies((p - wind.xyz * misc.z) / max(air.z, 1e-3));
		}
		v = a + (v - a) * decay + accel.xyz * dt;
		if (volume) {
			// Hair-hair interaction (Petrovic et al. 2005): friction with the neighbours' mean
			// velocity, and pressure down the gradient of density above the groom's.
			vec3 q = (to_groom * vec4(p, 1.0)).xyz;
			vec4 s = vol_sample(q);
			if (s.x > 1e-3) {
				v = mix(v, s.yzw, vol_misc.x);
			}
			float h = 0.5 * vol_origin.w;
			vec3 gq = vec3(vol_excess(q + vec3(h, 0, 0)) - vol_excess(q - vec3(h, 0, 0)),
							  vol_excess(q + vec3(0, h, 0)) - vol_excess(q - vec3(0, h, 0)),
							  vol_excess(q + vec3(0, 0, h)) - vol_excess(q - vec3(0, 0, h))) /
					(2.0 * h);
			v -= vol_misc.y * (basis * gq) * dt;
		}
		vec3 np = p + v * dt;
		// Global shape: pulled toward the groomed pose carried by the head, over the first
		// global_range share of the strand.
		if (float(i) < global_range) {
			np += step.z * (to_world(m, rest[k].xyz) - np);
		}
		prev[k] = vec4(p, 1.0);
		pos[k] = vec4(np, 1.0);
	}

	// Velocity shock propagation: the head's motion this step (the rotation taking the old root
	// segment to the new one, plus the root's move) carries vsp_coeff of the strand and its
	// velocity along, all of it when the root accelerates past the threshold, so a fast head turn
	// doesn't leave the hair behind and overstretch it.
	vec3 u = old1 - old0;
	vec3 w = new1 - new0;
	if (dot(u, u) > 1e-14 && dot(w, w) > 1e-14) {
		u = normalize(u);
		w = normalize(w);
		float shock = length(new1 - 2.0 * old1 + oldold1) * inv_dt * inv_dt;
		float vsp = shock > misc.w ? 1.0 : misc.x;
		if (vsp > 0.0) {
			for (int i = 2; i < n; i++) {
				int k = first + i;
				vec3 p = pos[k].xyz;
				vec3 pp = prev[k].xyz;
				pos[k].xyz = mix(p, new0 + rotate_between(u, w, p - old0), vsp);
				prev[k].xyz = mix(pp, new0 + rotate_between(u, w, pp - old0), vsp);
			}
		}
	}

	// Local shape: each segment keeps its groomed angle to its parent (bend and curl), the
	// correction split between the two points.
	int local_iterations = int(inertia.x + 0.5);
	for (int it = 0; it < local_iterations; it++) {
		for (int i = 1; i < n - 1; i++) {
			int k = first + i;
			vec3 p = pos[k].xyz;
			vec3 next = pos[k + 1].xyz;
			vec3 last = p - pos[k - 1].xyz;
			vec3 bind_last = basis * (rest[k].xyz - rest[k - 1].xyz);
			vec3 bind = basis * (rest[k + 1].xyz - rest[k].xyz);
			if (dot(last, last) > 1e-14 && dot(bind_last, bind_last) > 1e-14) {
				bind = rotate_between(normalize(bind_last), normalize(last), bind);
			}
			vec3 del = accel.w * (p + bind - next);
			if (i >= 2) {
				pos[k].xyz = p - del;
			}
			pos[k + 1].xyz = next + del;
		}
	}

	// Length constraints, root to tip; a pinned end takes none of the correction.
	int length_iterations = int(inertia.y + 0.5);
	for (int it = 0; it < length_iterations; it++) {
		for (int i = 1; i < n - 1; i++) {
			int k = first + i;
			vec3 p0 = pos[k].xyz;
			vec3 p1 = pos[k + 1].xyz;
			vec3 d = p1 - p0;
			float len = max(length(d), 1e-7);
			d *= 1.0 - rest[k + 1].w / len;
			if (i >= 2) {
				pos[k].xyz = p0 + 0.5 * d;
				pos[k + 1].xyz = p1 - 0.5 * d;
			} else {
				pos[k + 1].xyz = p1 - d;
			}
		}
	}

	// Collisions (a point pushed out loses its velocity), then the speed clamp.
	float max_move = air.x * dt;
	for (int i = 2; i < n; i++) {
		int k = first + i;
		vec3 p = pos[k].xyz;
		bool hit = false;
		for (int s = 0; s < counts.w; s++) {
			vec3 d = p - spheres[s].xyz;
			float r = spheres[s].w + misc.y;
			float l2 = dot(d, d);
			if (l2 < r * r && l2 > 1e-14) {
				p = spheres[s].xyz + d * (r / sqrt(l2));
				hit = true;
			}
		}
		pos[k].xyz = p;
		if (hit) {
			prev[k].xyz = p;
		} else if (max_move > 0.0) {
			vec3 d = p - prev[k].xyz;
			float l = length(d);
			if (l > max_move) {
				prev[k].xyz = p - d * (max_move / l);
			}
		}
	}
}

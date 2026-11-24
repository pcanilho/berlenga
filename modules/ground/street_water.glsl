#[compute]
#version 450

// Street water (StreetWater3D; docs/ideas/street-water.md): shallow water over a road's relief in a window of N x N
// cells that follows the view as a ring buffer (world cell c lives in texel c mod N). One shader; the push constant
// says which pass runs.
//
// - OCCUPY: the height of bodies (spheres: tyres, feet) below the water's surface in each cell, counted as head by the
//   pipes (Chentanez and Mueller 2010, 6.1), so a body pushes its volume of water aside.
// - FLUX: virtual pipes (Mei, Decaudin and Hu 2007) as the local inertial equations (Bates, Horritt and Fewtrell
//   2010) with Manning's friction taken semi-implicitly, the flow depth at a face the higher surface over the higher
//   bed (wet/dry fronts); outflows scaled together so a cell never gives more than it holds.
// - DEPTH: the flows' divergence; rain, pours, soaking in and evaporation; the damp left where water stood.
// - PACK: the map the road's material reads: depth, the water surface's slope in x and z, damp.
// - LOAD: cells entering the window: their bed, and their water to start with (the relief's ponding at the storm's
//   fill, so a street sliding into view shows the puddles it has, not a dry road); negative marks an inlet's grate,
//   which takes all the water reaching it (damp -1 keeps the mark).

#define MODE_OCCUPY 0
#define MODE_FLUX 1
#define MODE_DEPTH 2
#define MODE_PACK 3
#define MODE_LOAD 4

#define G 9.81
#define WALL 50.0 // bed (m over the reference) of cells with no road: water never enters

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// x depth (m), y bed (m over the window's reference height), z damp (0..1; -1 an inlet), w occupied (m of body below
// the surface)
layout(set = 0, binding = 0, std430) restrict buffer State {
	vec4 data[];
}
state;

// Outflow per unit width (m2/s) through the +x, -x, +z, -z faces.
layout(set = 0, binding = 1, std430) restrict buffer Flux {
	vec4 data[];
}
flux;

layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D out_map;

// Bodies, two each: xyz centre (m, from the window's corner and reference height), w radius; xyz velocity (m/s).
// Then pours: x z (m from the corner), radius, volume (m3).
layout(set = 0, binding = 3, std430) restrict readonly buffer Items {
	vec4 data[];
}
items;

layout(set = 0, binding = 4, std430) restrict readonly buffer Load {
	float data[];
}
load_buf;

layout(push_constant, std430) uniform Params {
	ivec4 a; // mode, n, origin mod n (x, z)
	ivec4 b; // load rect: rel x0, z0, w, h
	ivec4 c; // bodies, pours, load offset, edge
	vec4 d; // dt, cell, manning n, rain (m this step)
	vec4 e; // soak (m this step), evaporation (m this step), damp kept per step
}
pc;

int N() {
	return pc.a.y;
}

// Window-relative cell of a texel, and back.
ivec2 rel_of(ivec2 t) {
	return (t - pc.a.zw + N()) % N();
}

int texel_index(ivec2 rel) {
	ivec2 t = (rel + pc.a.zw) % N();
	return t.y * N() + t.x;
}

bool inside(ivec2 rel) {
	return rel.x >= 0 && rel.y >= 0 && rel.x < N() && rel.y < N();
}

void occupy(ivec2 rel, int i) {
	vec4 s = state.data[i];
	float l = pc.d.y;
	vec2 p = (vec2(rel) + 0.5) * l;
	float top = s.y + s.x + s.w; // the surface last step, bodies' head included
	float o = 0.0;
	for (int k = 0; k < pc.c.x; k++) {
		vec4 b = items.data[2 * k];
		vec2 dxz = p - b.xz;
		float r2 = b.w * b.w - dot(dxz, dxz);
		if (r2 <= 0.0) {
			continue;
		}
		float h = sqrt(r2);
		// The body's chord through the column, against the column from the bed to last step's surface: a body
		// standing on the bed and rising out of the water takes the whole column over a few steps (the water leaves),
		// a low one only what lies below the surface.
		o += max(0.0, min(b.y + h, top) - max(b.y - h, s.y));
	}
	state.data[i].w = s.y >= WALL ? 0.0 : o;
}

// The speed (m/s) and direction (xz) at which the first body over a cell throws its water: outward from the body's
// centre, at the speed its face advances that way (the moving boundary's normal velocity), so a tyre sheds water ahead
// and to both sides of its patch rather than ploughing it along. Zero where no body is, or behind a moving one.
vec3 body_throw(ivec2 rel) {
	vec2 p = (vec2(rel) + 0.5) * pc.d.y;
	for (int k = 0; k < pc.c.x; k++) {
		vec4 b = items.data[2 * k];
		vec2 dxz = p - b.xz;
		float r = length(dxz);
		if (r < b.w) {
			vec2 n = r > 1e-4 ? dxz / r : vec2(0.0);
			return vec3(n, max(dot(items.data[2 * k + 1].xz, n), 0.0));
		}
	}
	return vec3(0.0);
}

float eta(vec4 s) {
	return s.y + s.x + s.w;
}

void flux_pass(ivec2 rel, int i) {
	vec4 s = state.data[i];
	if (s.x <= 0.0 || s.y >= WALL) {
		flux.data[i] = vec4(0.0);
		return;
	}
	float dt = pc.d.x;
	float l = pc.d.y;
	float nm = pc.d.z;
	float h = eta(s);
	vec4 q = flux.data[i];
	// A moving body throws the water it stands in ahead and aside at its own speed (the moving boundary's
	// velocity condition, Chentanez and Mueller 2010): the head alone lets it seep away.
	vec3 thr = s.w > 0.0 ? body_throw(rel) : vec3(0.0);
	ivec2 nb[4] = ivec2[](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
	for (int k = 0; k < 4; k++) {
		ivec2 r = rel + nb[k];
		if (!inside(r)) {
			// The window's edge: as if the water beyond stood level with this cell's (no flow out or in), so water
			// is neither lost nor heaped there.
			q[k] = 0.0;
			continue;
		}
		vec4 o = state.data[texel_index(r)];
		float ho = o.y >= WALL ? o.y : eta(o);
		float dh = h - ho;
		// The flow depth at the face: the higher surface over the higher bed (a body counts as bed here: no water
		// runs through a tyre).
		float hf = max(h, ho) - max(s.y + s.w, o.y + o.w);
		float thrown = o.y >= WALL ? 0.0 : max(dot(thr.xy, vec2(nb[k])), 0.0) * thr.z * s.x;
		if (hf <= 1e-6) {
			q[k] = thrown;
			continue;
		}
		float qk = q[k];
		qk = (qk + dt * G * hf * dh / l) / (1.0 + dt * G * nm * nm * abs(qk) / pow(hf, 7.0 / 3.0));
		q[k] = max(max(qk, 0.0), thrown);
	}
	float out_v = (q.x + q.y + q.z + q.w) * dt / l; // m of depth leaving
	if (out_v > s.x) {
		q *= s.x / out_v;
	}
	flux.data[i] = q;
}

void depth_pass(ivec2 rel, int i) {
	vec4 s = state.data[i];
	if (s.y >= WALL) {
		state.data[i].xz = vec2(0.0);
		return;
	}
	float dt = pc.d.x;
	float l = pc.d.y;
	vec4 q = flux.data[i];
	float flow = -(q.x + q.y + q.z + q.w);
	ivec2 r;
	r = rel + ivec2(-1, 0);
	if (inside(r)) {
		flow += flux.data[texel_index(r)].x;
	}
	r = rel + ivec2(1, 0);
	if (inside(r)) {
		flow += flux.data[texel_index(r)].y;
	}
	r = rel + ivec2(0, -1);
	if (inside(r)) {
		flow += flux.data[texel_index(r)].z;
	}
	r = rel + ivec2(0, 1);
	if (inside(r)) {
		flow += flux.data[texel_index(r)].w;
	}
	float d = max(s.x + flow * dt / l, 0.0);
	// Rain on the whole road: it runs off as a film into the ruts, hollows and gutters.
	float rain = pc.d.w;
	d += rain;
	// Pours (drips off eaves, a bucket): their volume over a disc.
	vec2 p = (vec2(rel) + 0.5) * l;
	for (int k = 0; k < pc.c.y; k++) {
		vec4 a = items.data[2 * pc.c.x + k];
		vec2 dxz = p - a.xy;
		if (dot(dxz, dxz) < a.z * a.z) {
			d += a.w / (3.14159265 * a.z * a.z);
		}
	}
	d = max(d - pc.e.x - pc.e.y, 0.0);
	if (s.z < 0.0) {
		// An inlet: the water falls through its grate.
		state.data[i].x = 0.0;
		return;
	}
	float damp = s.z * pc.e.z;
	if (d > 1e-5 || rain > 0.0) {
		damp = 1.0;
	}
	state.data[i].x = d;
	state.data[i].z = damp;
}

void pack(ivec2 rel, int i) {
	vec4 s = state.data[i];
	float l = pc.d.y;
	float h = s.y + s.x;
	ivec2 rx0 = rel + ivec2(-1, 0), rx1 = rel + ivec2(1, 0), rz0 = rel + ivec2(0, -1), rz1 = rel + ivec2(0, 1);
	vec4 a = inside(rx0) ? state.data[texel_index(rx0)] : s;
	vec4 b = inside(rx1) ? state.data[texel_index(rx1)] : s;
	vec4 c = inside(rz0) ? state.data[texel_index(rz0)] : s;
	vec4 e = inside(rz1) ? state.data[texel_index(rz1)] : s;
	// The water surface's slope from its wet neighbours only (a dry neighbour is the road, not the water's edge).
	float ha = a.x > 0.0 && a.y < WALL ? a.y + a.x : h;
	float hb = b.x > 0.0 && b.y < WALL ? b.y + b.x : h;
	float hc = c.x > 0.0 && c.y < WALL ? c.y + c.x : h;
	float he = e.x > 0.0 && e.y < WALL ? e.y + e.x : h;
	vec2 slope = vec2(hb - ha, he - hc) / (2.0 * l);
	ivec2 t = (rel + pc.a.zw) % N();
	// Damp is on or off per cell; averaged with the four neighbours its edge is a soft line, not 2.5 cm steps.
	float damp = (abs(s.z) * 2.0 + abs(a.z) + abs(b.z) + abs(c.z) + abs(e.z)) / 6.0;
	imageStore(out_map, t, vec4(s.y >= WALL ? 0.0 : s.x, slope, damp));
}

void main() {
	ivec2 g = ivec2(gl_GlobalInvocationID.xy);
	int mode = pc.a.x;
	if (mode == MODE_LOAD) {
		if (g.x >= pc.b.z || g.y >= pc.b.w) {
			return;
		}
		ivec2 rel = pc.b.xy + g;
		int i = texel_index(rel);
		int li = pc.c.z + 2 * (g.y * pc.b.z + g.x);
		float bed = load_buf.data[li];
		float start = load_buf.data[li + 1];
		state.data[i] = start < 0.0 ? vec4(0.0, bed, -1.0, 0.0) : vec4(start, bed, start > 0.0003 ? 1.0 : 0.0, 0.0);
		flux.data[i] = vec4(0.0);
		return;
	}
	if (g.x >= N() || g.y >= N()) {
		return;
	}
	ivec2 rel = rel_of(g);
	int i = g.y * N() + g.x;
	if (mode == MODE_OCCUPY) {
		occupy(rel, i);
	} else if (mode == MODE_FLUX) {
		flux_pass(rel, i);
	} else if (mode == MODE_DEPTH) {
		depth_pass(rel, i);
	} else {
		pack(rel, i);
	}
}

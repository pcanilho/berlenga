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
//   MODE_RESET   (first frame / teleport) every point placed on the groomed pose.
//   MODE_OUTPUT  positions written, in the render copy's space, to an RGBA32F image the hair
//                material reads (no CPU readback).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define MODE_STEP 0
#define MODE_RESET 1
#define MODE_OUTPUT 2
#define MODE_FOLLOW 3

#define MAX_SPHERES 32

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 counts; // x strands, y points, z output width, w spheres
	vec4 step; // x dt (substep), y velocity decay toward the air's per substep, z global shape stiffness (per substep), w global range (share of the strand)
	vec4 accel; // xyz gravity (m/s^2), w local shape stiffness (per iteration)
	vec4 wind; // xyz air velocity (m/s), w gust strength (0..1)
	vec4 misc; // x VSP coefficient, y collision margin (m), z time (s), w VSP acceleration threshold (m/s^2)
	vec4 air; // x speed clamp (m/s), y turbulence (0..1), z turbulence eddy size (m), w unused
	vec4 inertia; // x local shape iterations, y length iterations
	mat4 xform_prev; // groom space -> world, last frame
	mat4 xform_cur; // groom space -> world, this frame
	mat4 world_to_out; // world -> the render copy's space
	vec4 spheres[MAX_SPHERES]; // world centre, radius
};

layout(push_constant, std430) uniform Pass {
	int mode;
	float frac; // substep fraction, for blending the head's transform
	float frac_prev; // the previous substep's
	int pad1;
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

// Smooth divergence-light swirl in -1..1 at q (in eddies): two octaves of crossed sines.
vec3 eddies(vec3 q) {
	vec3 a = sin(q.yzx * vec3(1.7, 1.9, 2.1) + q.zxy * vec3(2.3, 2.9, 1.3) + vec3(1.1, 2.3, 0.7));
	q *= 2.31;
	vec3 b = sin(q.zxy * vec3(1.3, 2.2, 1.6) + q.yzx * vec3(2.7, 1.4, 2.5) + vec3(0.4, 1.9, 2.8));
	return (a + 0.5 * b) / 1.5;
}

void main() {
	int id = int(gl_GlobalInvocationID.x);

	if (pass.mode == MODE_OUTPUT) {
		if (id >= counts.y) {
			return;
		}
		int w = max(counts.z, 1);
		imageStore(out_image, ivec2(id % w, id / w), vec4(to_world(world_to_out, pos[id].xyz), 1.0));
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
		if (is_guide) {
			return;
		}
		vec4 gi = follow[id * 2];
		vec4 gw = follow[id * 2 + 1];
		for (int i = 0; i < n; i++) {
			float t = float(i) / float(max(n - 1, 1));
			vec3 own = to_world(m, rest[first + i].xyz);
			vec3 p = vec3(0.0);
			for (int c = 0; c < 3; c++) {
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
				vec3 off = own - gr;
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
				float r = spheres[s].w + misc.y;
				float l2 = dot(d, d);
				if (l2 < r * r && l2 > 1e-14) {
					p = spheres[s].xyz + d * (r / sqrt(l2));
				}
			}
			pos[first + i] = vec4(p, 1.0);
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
	for (int i = 2; i < n; i++) {
		int k = first + i;
		vec3 p = pos[k].xyz;
		vec3 v = (p - prev[k].xyz) * inv_dt;
		vec3 a = air_v;
		if (air.y > 0.0 && wind_speed > 1e-3) {
			a += air.y * wind_speed * eddies((p - wind.xyz * misc.z) / max(air.z, 1e-3));
		}
		v = a + (v - a) * decay + accel.xyz * dt;
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

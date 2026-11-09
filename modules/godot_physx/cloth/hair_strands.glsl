#[compute]
#version 450

// Strand hair on any GPU (RenderingDevice compute), for PhysXHair3D. One thread per strand walks it
// root to tip (Muller et al. 2012, "Fast Simulation of Inextensible Hair and Fur": dynamic
// follow-the-leader, DFTL), with TressFX-style shape constraints (AMD TressFX 4: global shape pulls
// toward the groomed pose carried by the head, local shape keeps each segment's groomed bend
// relative to its parent). Per substep:
//   MODE_STEP    the root follows the head (its transform blended across the substeps); every other
//                point takes a Verlet step under gravity, wind and damping and is pulled toward the
//                groomed pose (global shape, stronger at the root). Then root to tip: the local shape
//                pull, collision spheres, and the segment's length restored by moving the point
//                along it (FTL), with that correction fed back into the parent's velocity (DFTL).
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
	vec4 step; // x dt (substep), y velocity kept per substep, z global shape at the root, w at the tip (per substep)
	vec4 accel; // xyz gravity (m/s^2), w local shape (per substep)
	vec4 wind; // xyz air velocity (m/s), w gust strength (0..1)
	vec4 misc; // x DFTL damping (0..1), y collision margin (m), z time (s), w follow range (share of the strand)
	vec4 air; // x drag rate (1/s), y turbulence (0..1), z turbulence eddy size (m), w damping of the global shape spring (0..1)
	vec4 inertia; // x inertia scale (0 the hair moves with the head, 1 full inertia), y the head speed it starts at (m/s)
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
	// Inertia scale (HairWorks): above the inertia limit, part of the head's motion carries the
	// strand along instead of being left to inertia, so fast moves don't whip the hair.
	mat4 m_last = xform_prev + (xform_cur - xform_prev) * pass.frac_prev;

	float dt = step.x;
	// A zero step (the first frame after building) only places the strands.
	float inv_dt = dt > 0.0 ? 1.0 / dt : 0.0;
	// Gusts: the wind rises and falls, each strand a little out of step with its neighbours.
	float gust = 1.0 + wind.w * sin(misc.z * 2.3 + hash11(float(id)) * 6.2832) * (0.6 + 0.4 * sin(misc.z * 0.7 + float(id) * 0.013));
	vec3 air_v = wind.xyz * gust;
	float wind_speed = length(wind.xyz);
	float drag = air.x;

	// Root: carried by the head.
	vec3 root = to_world(m, rest[first].xyz);
	prev[first] = pos[first];
	pos[first] = vec4(root, 1.0);

	// Verlet step and the global shape pull.
	for (int i = 1; i < n; i++) {
		int k = first + i;
		vec3 p = pos[k].xyz;
		vec3 pp = prev[k].xyz;
		if (inertia.x < 1.0) {
			vec3 carried = to_world(m, rest[k].xyz) - to_world(m_last, rest[k].xyz);
			float speed = length(carried) * inv_dt;
			float share = (1.0 - inertia.x) * clamp((speed - inertia.y) / max(speed, 1e-6), 0.0, 1.0);
			p += carried * share;
			pp += carried * share;
		}
		vec3 v = (p - pp) * (step.y * inv_dt);
		prev[k] = vec4(p, 1.0);
		// Air: a fibre is so light that drag, not inertia, sets how it moves (a loose hair falls at
		// about 0.5 m/s). Velocity relaxes toward the air's at the drag rate (implicit, so stable at
		// any rate), and the air carries turbulence: eddies blown along with the wind (Taylor's
		// frozen turbulence), so neighbouring strands flutter together and differ along a strand.
		vec3 a = air_v;
		if (air.y > 0.0 && wind_speed > 1e-3) {
			a += air.y * wind_speed * eddies((p - wind.xyz * misc.z) / max(air.z, 1e-3));
		}
		v = (v + (accel.xyz + drag * a) * dt) / (1.0 + drag * dt);
		p += v * dt;
		float along = float(i) / float(max(n - 1, 1));
		// TressFX global shape: held near the scalp, free past the follow range (bar the tip pull).
		vec3 pull = (to_world(m, rest[k].xyz) - p) * mix(step.z, step.w, smoothstep(0.0, max(misc.w, 1e-3), along));
		p += pull;
		// The spring's own damping (HairWorks stiffness damping): the pull moves the point without
		// all of it turning into velocity, so the hair settles instead of bouncing.
		prev[k].xyz += pull * air.w;
		pos[k] = vec4(p, 1.0);
	}

	// Root to tip: local shape, collisions, follow-the-leader length.
	for (int i = 1; i < n; i++) {
		int k = first + i;
		vec3 parent = pos[k - 1].xyz;
		vec3 seg = basis * (rest[k].xyz - rest[k - 1].xyz);
		if (i >= 2) {
			vec3 rest_parent = basis * (rest[k - 1].xyz - rest[k - 2].xyz);
			vec3 cur_parent = parent - pos[k - 2].xyz;
			if (dot(rest_parent, rest_parent) > 1e-14 && dot(cur_parent, cur_parent) > 1e-14) {
				seg = rotate_between(normalize(rest_parent), normalize(cur_parent), seg);
			}
		}
		vec3 p = pos[k].xyz;
		p += (parent + seg - p) * accel.w;

		for (int s = 0; s < counts.w; s++) {
			vec3 d = p - spheres[s].xyz;
			float r = spheres[s].w + misc.y;
			float l2 = dot(d, d);
			if (l2 < r * r && l2 > 1e-14) {
				p = spheres[s].xyz + d * (r / sqrt(l2));
			}
		}

		vec3 dir = p - parent;
		float len = length(dir);
		vec3 fixed_p = len > 1e-9 ? parent + dir * (rest[k].w / len) : parent + seg;
		vec3 correction = fixed_p - p;
		pos[k] = vec4(fixed_p, 1.0);
		// DFTL: the parent's velocity takes back part of the correction, which damps the
		// otherwise stiff look of plain FTL (Muller 2012, eq. 9).
		if (i >= 2) {
			prev[k - 1].xyz += correction * misc.x;
		}
	}
}

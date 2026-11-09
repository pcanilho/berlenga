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

#define MAX_SPHERES 32

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 counts; // x strands, y points, z output width, w spheres
	vec4 step; // x dt (substep), y velocity kept per substep, z global shape at the root, w at the tip (per substep)
	vec4 accel; // xyz gravity (m/s^2), w local shape (per substep)
	vec4 wind; // xyz wind and other acceleration (m/s^2), w gust strength (0..1)
	vec4 misc; // x DFTL damping (0..1), y collision margin (m), z time (s), w unused
	mat4 xform_prev; // groom space -> world, last frame
	mat4 xform_cur; // groom space -> world, this frame
	mat4 world_to_out; // world -> the render copy's space
	vec4 spheres[MAX_SPHERES]; // world centre, radius
};

layout(push_constant, std430) uniform Pass {
	int mode;
	float frac; // substep fraction, for blending the head's transform
	int pad0;
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

	float dt = step.x;
	// Gusts: each strand sees the wind rise and fall a little out of step with its neighbours.
	float gust = 1.0 + wind.w * sin(misc.z * 2.3 + hash11(float(id)) * 6.2832) * (0.6 + 0.4 * sin(misc.z * 0.7 + float(id) * 0.013));
	vec3 acc = accel.xyz + wind.xyz * gust;

	// Root: carried by the head.
	vec3 root = to_world(m, rest[first].xyz);
	prev[first] = pos[first];
	pos[first] = vec4(root, 1.0);

	// Verlet step and the global shape pull.
	for (int i = 1; i < n; i++) {
		int k = first + i;
		vec3 p = pos[k].xyz;
		vec3 v = (p - prev[k].xyz) * step.y;
		prev[k] = vec4(p, 1.0);
		p += v + acc * dt * dt;
		float along = float(i) / float(max(n - 1, 1));
		p += (to_world(m, rest[k].xyz) - p) * mix(step.z, step.w, along);
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

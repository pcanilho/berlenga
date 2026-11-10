#[compute]
#version 450

// The whole Stockham FFT of one row (or column) in one workgroup, in shared memory: all log2(N)
// butterfly stages with a barrier between them, instead of one dispatch per stage through global
// memory (water_fft.glsl). The four transforms of a step (height + x displacement, z displacement,
// and the two derivative pairs) are batched along z, so a step is two dispatches -- rows, then
// columns -- instead of 8 log2(N). This is the standard GPU ocean FFT layout (Flügge, "Realtime
// GPGPU FFT ocean water simulation", 2017; the shared-memory radix-2 Stockham of GodotOceanWaves
// and Crest). The butterflies are water_fft.glsl's exactly, so the result is the same field.
//
// Flat, as the other passes here: no helpers beyond the complex multiply, simple loops.

#define MAX_N 1024
#define THREADS 256

layout(local_size_x = THREADS, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 p; // x = N, y = log2(N), z = horiz (1 rows, 0 columns), w unused
};

layout(set = 0, binding = 1, std430) restrict readonly buffer In0 {
	vec2 in0[];
};
layout(set = 0, binding = 2, std430) restrict readonly buffer In1 {
	vec2 in1[];
};
layout(set = 0, binding = 3, std430) restrict readonly buffer In2 {
	vec2 in2[];
};
layout(set = 0, binding = 4, std430) restrict readonly buffer In3 {
	vec2 in3[];
};
layout(set = 0, binding = 5, std430) restrict writeonly buffer Out0 {
	vec2 out0[];
};
layout(set = 0, binding = 6, std430) restrict writeonly buffer Out1 {
	vec2 out1[];
};
layout(set = 0, binding = 7, std430) restrict writeonly buffer Out2 {
	vec2 out2[];
};
layout(set = 0, binding = 8, std430) restrict writeonly buffer Out3 {
	vec2 out3[];
};

shared vec2 buf[2][MAX_N];

vec2 cmul(vec2 a, vec2 b) {
	return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void main() {
	int n = p.x;
	int log2n = p.y;
	bool horiz = p.z == 1;
	int line = int(gl_WorkGroupID.y);
	int field = int(gl_WorkGroupID.z);
	int tid = int(gl_LocalInvocationID.x);
	if (line >= n) {
		return;
	}

	// Load the line (index along it: j).
	for (int j = tid; j < n; j += THREADS) {
		int idx = horiz ? line * n + j : j * n + line;
		vec2 v;
		if (field == 0) {
			v = in0[idx];
		} else if (field == 1) {
			v = in1[idx];
		} else if (field == 2) {
			v = in2[idx];
		} else {
			v = in3[idx];
		}
		buf[0][j] = v;
	}
	barrier();

	int src = 0;
	for (int s = 0; s < log2n; s++) {
		int ns = 2 << s;
		int hs = ns >> 1;
		for (int j = tid; j < n; j += THREADS) {
			int e = (j / ns) * hs + (j % hs);
			vec2 E = buf[src][e];
			vec2 O = buf[src][e + n / 2];
			float ang = 6.28318530718 * float(j % ns) / float(ns);
			buf[1 - src][j] = E + cmul(vec2(cos(ang), sin(ang)), O);
		}
		src = 1 - src;
		barrier();
	}

	for (int j = tid; j < n; j += THREADS) {
		int idx = horiz ? line * n + j : j * n + line;
		vec2 v = buf[src][j];
		if (field == 0) {
			out0[idx] = v;
		} else if (field == 1) {
			out1[idx] = v;
		} else if (field == 2) {
			out2[idx] = v;
		} else {
			out3[idx] = v;
		}
	}
}

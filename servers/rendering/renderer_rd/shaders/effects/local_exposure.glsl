#[compute]

#version 450

#VERSION_DEFINES

// Local exposure: a bilateral grid of the frame's log2 brightness (each pixel's brightest channel,
// as auto exposure meters). Cells cover a block of pixels on screen and one stop of brightness, so
// the tonemapper can read the mean brightness of the pixels near a pixel that are also near it in
// brightness: a lit window and the dark wall around it get separate local means, with no halo.
// BUILD_GRID splats the pixels into the grid; BLUR_GRID smooths it over space and brightness.

#define GRID_SLICES 32
#define LOG2_MIN -16.0
#define LOG2_MAX 16.0

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 grid_size;
	int cell_size;
	int pad[3];
}
params;

#ifdef BUILD_GRID

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(rg32f, set = 1, binding = 0) uniform restrict writeonly image3D grid;

shared vec2 partial[64][GRID_SLICES];

void main() {
	// One workgroup per grid column (x, y); each thread walks part of the cell's pixels.
	ivec2 cell = ivec2(gl_WorkGroupID.xy);
	uint t = gl_LocalInvocationIndex;

	vec2 acc[GRID_SLICES];
	for (int i = 0; i < GRID_SLICES; i++) {
		acc[i] = vec2(0.0);
	}

	ivec2 origin = cell * params.cell_size;
	for (int y = int(gl_LocalInvocationID.y); y < params.cell_size; y += 8) {
		for (int x = int(gl_LocalInvocationID.x); x < params.cell_size; x += 8) {
			ivec2 pos = origin + ivec2(x, y);
			if (any(greaterThanEqual(pos, params.source_size))) {
				continue;
			}
			vec3 v = texelFetch(source_texture, pos, 0).rgb;
			float l = max(v.r, max(v.g, v.b));
			float ev = clamp(l > 0.0 ? log2(l) : LOG2_MIN, LOG2_MIN, LOG2_MAX);
			// Split the pixel between its two nearest slices.
			float z = (ev - LOG2_MIN) / (LOG2_MAX - LOG2_MIN) * float(GRID_SLICES) - 0.5;
			float zf = floor(z);
			float f = z - zf;
			int z0 = clamp(int(zf), 0, GRID_SLICES - 1);
			int z1 = clamp(int(zf) + 1, 0, GRID_SLICES - 1);
			acc[z0] += vec2(ev, 1.0) * (1.0 - f);
			acc[z1] += vec2(ev, 1.0) * f;
		}
	}

	for (int i = 0; i < GRID_SLICES; i++) {
		partial[t][i] = acc[i];
	}
	barrier();

	if (t < GRID_SLICES) {
		vec2 sum = vec2(0.0);
		for (int i = 0; i < 64; i++) {
			sum += partial[i][t];
		}
		// Normalized by the cell's pixel count, so values stay small at any resolution.
		sum /= float(params.cell_size * params.cell_size);
		imageStore(grid, ivec3(cell, int(t)), vec4(sum, 0.0, 0.0));
	}
}

#endif

#ifdef BLUR_GRID

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(rg32f, set = 0, binding = 0) uniform restrict readonly image3D source_grid;
layout(rg32f, set = 1, binding = 0) uniform restrict writeonly image3D dest_grid;

void main() {
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(pos.xy, params.grid_size)) || pos.z >= GRID_SLICES) {
		return;
	}
	// 5x5 binomial over the screen, 3 over brightness.
	const float wxy[5] = float[](1.0, 4.0, 6.0, 4.0, 1.0);
	const float wz[3] = float[](1.0, 2.0, 1.0);
	vec2 sum = vec2(0.0);
	for (int z = -1; z <= 1; z++) {
		int sz = pos.z + z;
		if (sz < 0 || sz >= GRID_SLICES) {
			continue;
		}
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				ivec2 s = clamp(pos.xy + ivec2(x, y), ivec2(0), params.grid_size - 1);
				sum += imageLoad(source_grid, ivec3(s, sz)).rg * (wxy[x + 2] * wxy[y + 2] * wz[z + 1]);
			}
		}
	}
	imageStore(dest_grid, pos, vec4(sum / 1024.0, 0.0, 0.0));
}

#endif

#[compute]
#version 450

// Persistent whitecap foam on the ocean grid. Each step: where the choppy
// displacement folds the surface (area Jacobian below the threshold) foam is
// injected, and everywhere it fades by exp(-dt / persistence). Stored in the
// water's rest coordinates, like the FFT fields, so it rides with the water
// itself rather than the wave shape: a crest passes through and leaves its
// whitecap behind as a fading streak.
//
// It also sums the foam over the grid (fixed point, 1/1024 per texel) into
// cover_sum: the whitecap cover, which PhysXWaterSurface3D steers to the
// measured wind-speed law (Monahan and O'Muircheartaigh 1980).

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 foam_params; // x decay factor this step, y choppiness, z threshold, w injection softness
	ivec4 res; // x = n, y = 1 if foam is enabled, zw unused
};
layout(set = 0, binding = 1, rgba32f) uniform restrict readonly image2D deriv_tex; // (dh/dx, dh/dz, dDx/dx, dDz/dz)
layout(set = 0, binding = 2, rgba32f) uniform restrict readonly image2D disp_tex; // (Dx, Dz, dDx/dz, 0)
layout(set = 0, binding = 3, r32f) uniform restrict image2D foam_tex;
layout(set = 0, binding = 4, std430) restrict buffer Cover {
	uint cover_sum;
};

shared uint group_sum;

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	if (gl_LocalInvocationIndex == 0) {
		group_sum = 0;
	}
	barrier();
	float foam = 0.0;
	if (c.x < res.x && c.y < res.x && res.y != 0) {
		// Same displacement sign and Jacobian as the materials (see
		// PhysXWaterSurface3D's built-in shader).
		float s = -foam_params.y;
		vec4 der = imageLoad(deriv_tex, c);
		float dxz = imageLoad(disp_tex, c).b;
		float jacobian = (1.0 + s * der.z) * (1.0 + s * der.w) - s * s * dxz * dxz;
		float inject = clamp((foam_params.z - jacobian) / max(foam_params.w, 1e-3), 0.0, 1.0);
		foam = max(imageLoad(foam_tex, c).r * foam_params.x, inject);
	}
	if (c.x < res.x && c.y < res.x) {
		// Foam off keeps the texture clear.
		imageStore(foam_tex, c, vec4(foam, 0.0, 0.0, 0.0));
	}
	atomicAdd(group_sum, uint(foam * 1024.0 + 0.5));
	barrier();
	if (gl_LocalInvocationIndex == 0 && group_sum > 0) {
		atomicAdd(cover_sum, group_sum);
	}
}

#[compute]
#version 450

#include "mpm_fluid_inc.glsl"

layout(local_size_x = GROUP) in;

// Pack particle positions straight into the MultiMesh instance buffer on the GPU
// (12 floats / instance: three transform rows, basis + origin; identity basis but for thrown clumps) so the
// render path never round-trips through the CPU.
void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= uint(PCOUNT)) {
		return;
	}
	vec3 x = particles[id].x_d.xyz;
	uint o = id * 12u;
	if (particle_dead(id)) {
		// A slot given back (its grain went back to a height field): no instance drawn.
		for (uint k = 0u; k < 12u; k++) {
			mm[o + k] = 0.0;
		}
		return;
	}
	mat3 m = mat3(1.0);
	if (FREE_FLIGHT) {
		// Thrown clumps (ground grains): each slot its own axis, size (0.75 to 1.25) and spin, and it tumbles
		// as it travels (a turn every ~0.8 m), so a spray never shows the one mesh twice alike.
		uint h = id * 747796405u + 2891336453u;
		h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
		h = (h >> 22u) ^ h;
		vec3 r = vec3(float(h & 1023u), float((h >> 10u) & 1023u), float((h >> 20u) & 1023u)) / 1023.0;
		vec3 axis = normalize(r * 2.0 - 1.0 + vec3(1e-3));
		float angle = r.x * 6.2831853 + (x.x + x.y + x.z) * 8.0;
		float c = cos(angle);
		float sn = sin(angle);
		mat3 k = mat3(0.0, axis.z, -axis.y, -axis.z, 0.0, axis.x, axis.y, -axis.x, 0.0);
		m = (mat3(1.0) + sn * k + (1.0 - c) * (k * k)) * (0.75 + 0.5 * r.y);
	}
	// Rows of the instance basis.
	mm[o + 0] = m[0][0];
	mm[o + 1] = m[1][0];
	mm[o + 2] = m[2][0];
	mm[o + 3] = x.x;
	mm[o + 4] = m[0][1];
	mm[o + 5] = m[1][1];
	mm[o + 6] = m[2][1];
	mm[o + 7] = x.y;
	mm[o + 8] = m[0][2];
	mm[o + 9] = m[1][2];
	mm[o + 10] = m[2][2];
	mm[o + 11] = x.z;
}

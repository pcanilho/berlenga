#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Linear swept spheres of a strand surface (render_raytracing.cpp, process_deformed_surface): its captured
// vertices are four per strand point, the first strip's two sides first, so each point's tube centre is
// halfway between those two and its radius half the strip's width there.

layout(set = 0, binding = 0, std430) restrict readonly buffer Positions {
	float v[]; // float3 per vertex, packed.
}
positions;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Points {
	vec4 v[]; // xyz centre, w radius.
}
points;

layout(push_constant, std430) uniform Params {
	uint point_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint p = gl_GlobalInvocationID.x;
	if (p >= params.point_count) {
		return;
	}
	uint a = p * 12u; // Vertex 4p, three floats each.
	vec3 left = vec3(positions.v[a], positions.v[a + 1u], positions.v[a + 2u]);
	vec3 right = vec3(positions.v[a + 3u], positions.v[a + 4u], positions.v[a + 5u]);
	points.v[p] = vec4(0.5 * (left + right), 0.5 * distance(left, right));
}

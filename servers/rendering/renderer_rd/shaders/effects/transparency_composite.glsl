#[compute]

#version 450

#VERSION_DEFINES

// Lays the transparent surfaces, drawn into a layer of their own at the
// internal size, over the colour the upscaler wrote; then the particles, drawn into a
// second layer at the output size without the jitter, over that. The layer holds colour
// premultiplied by coverage and the coverage, as the mix blend leaves them
// over a cleared target.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D layer_texture;
// The particles' layer at the output size, unjittered (particles_layer).
layout(set = 0, binding = 1) uniform sampler2D particles_texture;
layout(rgba16f, set = 1, binding = 0) uniform restrict image2D color_image;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	// the frame's jitter in UV: the layer was drawn with it, the output has none
	vec2 jitter_uv;
	int pad;
	// the particles are in particles_texture, to lay over everything else
	int particles_layer;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size) + params.jitter_uv;
	vec4 layer = textureLod(layer_texture, uv, 0.0);
	vec4 particles = params.particles_layer != 0 ? texelFetch(particles_texture, pos, 0) : vec4(0.0);
	vec4 base = imageLoad(color_image, pos);
	vec3 res = layer.rgb + base.rgb * (1.0 - layer.a);
	imageStore(color_image, pos, vec4(particles.rgb + res * (1.0 - particles.a), base.a));
}

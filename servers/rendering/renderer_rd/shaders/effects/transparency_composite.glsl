#[compute]

#version 450

#VERSION_DEFINES

// Lays the transparent surfaces, drawn into a layer of their own at the
// internal size, over the colour the upscaler wrote. The layer holds colour
// premultiplied by coverage and the coverage, as the mix blend leaves them
// over a cleared target.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D layer_texture;
// The traced volumetric fog, filtered: in-scatter rgb, transmittance a, at the internal size.
layout(set = 0, binding = 1) uniform sampler2D fog_texture;
// The traced depth (reverse Z, 0 the sky): over the sky the fog lies over the layer too,
// whose clouds are kilometres off; elsewhere the layer's transparents are near.
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(rgba16f, set = 1, binding = 0) uniform restrict image2D color_image;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	// the frame's jitter in UV: the layer was drawn with it, the output has none
	vec2 jitter_uv;
	int fog;
	// 1: Ray Reconstruction already reconstructed the layer into the output (its
	// transparency overlay), so only the fog is laid here, under the layer's coverage.
	int layer_in_output;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size) + params.jitter_uv;
	vec4 layer = textureLod(layer_texture, uv, 0.0);
	vec4 base = imageLoad(color_image, pos);
	vec3 under = base.rgb;
	if (params.fog == 0) {
		imageStore(color_image, pos, vec4(layer.rgb + under * (1.0 - layer.a), base.a));
		return;
	}
	vec4 fog = textureLod(fog_texture, uv, 0.0);
	if (params.layer_in_output != 0) {
		// The output holds transparents over surfaces. Over the sky the fog lies over both, as
		// below; elsewhere it lies under the transparents, so it is laid where they don't cover:
		// out = T + S(1 - a) becomes T + (S fog.a + fog.rgb)(1 - a), with S(1 - a) = out - T.
		if (textureLod(depth_texture, uv, 0.0).r <= 0.0) {
			imageStore(color_image, pos, vec4(base.rgb * fog.a + fog.rgb, base.a));
			return;
		}
		vec3 over = min(layer.rgb, base.rgb);
		vec3 surface = base.rgb - over;
		imageStore(color_image, pos, vec4(over + surface * fog.a + fog.rgb * (1.0 - layer.a), base.a));
		return;
	}
	if (textureLod(depth_texture, uv, 0.0).r <= 0.0) {
		vec3 all = layer.rgb + under * (1.0 - layer.a);
		imageStore(color_image, pos, vec4(all * fog.a + fog.rgb, base.a));
		return;
	}
	// the fog lies between the camera and the surfaces, under the transparents
	under = under * fog.a + fog.rgb;
	imageStore(color_image, pos, vec4(layer.rgb + under * (1.0 - layer.a), base.a));
}

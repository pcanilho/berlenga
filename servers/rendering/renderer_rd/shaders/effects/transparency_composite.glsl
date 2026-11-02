#[compute]

#version 450

#VERSION_DEFINES

// Lays the transparent surfaces, drawn into a layer of their own at the
// internal size (or at the output size, unjittered), over the colour the upscaler wrote. The layer holds colour
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
	// the layer was drawn at the output size without the jitter
	int layer_unjittered;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	vec2 out_uv = (vec2(pos) + 0.5) / vec2(params.size);
	vec2 uv = out_uv + params.jitter_uv;
	vec4 layer = textureLod(layer_texture, params.layer_unjittered != 0 ? out_uv : uv, 0.0);
	vec4 base = imageLoad(color_image, pos);
	vec3 under = base.rgb;
	if (params.fog == 0) {
		imageStore(color_image, pos, vec4(layer.rgb + under * (1.0 - layer.a), base.a));
		return;
	}
	vec4 fog = textureLod(fog_texture, uv, 0.0);
	if (textureLod(depth_texture, uv, 0.0).r <= 0.0) {
		vec3 all = layer.rgb + under * (1.0 - layer.a);
		imageStore(color_image, pos, vec4(all * fog.a + fog.rgb, base.a));
		return;
	}
	// the fog lies between the camera and the surfaces, under the transparents
	under = under * fog.a + fog.rgb;
	imageStore(color_image, pos, vec4(layer.rgb + under * (1.0 - layer.a), base.a));
}

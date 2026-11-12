#[compute]

#version 450

#VERSION_DEFINES

// Temporal reprojection of a sky shader's half resolution pass (render_mode
// half_res_reprojection): Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero
// Dawn" (Advances in Real-Time Rendering, SIGGRAPH 2015) and "Nubis" (2017). Each frame the
// pass draws one pixel of every 4x4 block (a Bayer order over 16 frames); this resolve keeps
// the rest of the full resolution image from last frame, found by turning each pixel's view
// direction into last frame's camera. The sky is at infinity, so the direction is enough.
// Pixels last frame did not see take the new samples, upsampled.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D new_samples; // ceil(size / 4), the pass's output
layout(set = 0, binding = 1) uniform sampler2D history; // last frame's resolve
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D dest;

layout(push_constant, std430) uniform Params {
	vec4 to_prev_view[3]; // this frame's view space to last frame's (columns)
	vec4 projection; // as sky.glsl: x offset, x scale, y offset, y scale
	vec4 prev_projection;
	ivec2 size;
	ivec2 offset; // the pixel of each block drawn this frame
	ivec2 low_size;
	uint history_valid;
	uint drawn; // samples this frame's pixel has had since the history was reset, this one included
}
params;

// Last frame's resolve through a Catmull-Rom filter from 9 bilinear taps (Jimenez, "Filmic SMAA",
// SIGGRAPH 2016 course; as TAA resolves do): a bilinear history blurs a little more every frame.
vec4 sample_history(vec2 uv) {
	vec2 tex_size = vec2(params.size);
	vec2 pos = uv * tex_size;
	vec2 center = floor(pos - 0.5) + 0.5;
	vec2 f = pos - center;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 tc0 = (center - 1.0) / tex_size;
	vec2 tc12 = (center + w2 / w12) / tex_size;
	vec2 tc3 = (center + 2.0) / tex_size;
	vec4 c = textureLod(history, vec2(tc0.x, tc0.y), 0.0) * (w0.x * w0.y) +
			textureLod(history, vec2(tc12.x, tc0.y), 0.0) * (w12.x * w0.y) +
			textureLod(history, vec2(tc3.x, tc0.y), 0.0) * (w3.x * w0.y) +
			textureLod(history, vec2(tc0.x, tc12.y), 0.0) * (w0.x * w12.y) +
			textureLod(history, vec2(tc12.x, tc12.y), 0.0) * (w12.x * w12.y) +
			textureLod(history, vec2(tc3.x, tc12.y), 0.0) * (w3.x * w12.y) +
			textureLod(history, vec2(tc0.x, tc3.y), 0.0) * (w0.x * w3.y) +
			textureLod(history, vec2(tc12.x, tc3.y), 0.0) * (w12.x * w3.y) +
			textureLod(history, vec2(tc3.x, tc3.y), 0.0) * (w3.x * w3.y);
	return max(c, vec4(0.0)); // the filter's negative lobes can ring below zero
}

// Last frame's colour for this pixel, if last frame saw it.
bool reproject(ivec2 px, out vec4 r_color) {
	r_color = vec4(0.0);
	if (params.history_valid == 0u) {
		return false;
	}
	vec2 ndc = (vec2(px) + 0.5) / vec2(params.size) * 2.0 - 1.0;
	vec3 view = vec3((ndc.x + params.projection.x) / params.projection.y, (ndc.y + params.projection.z) / params.projection.w, -1.0);
	vec3 prev_view = mat3(params.to_prev_view[0].xyz, params.to_prev_view[1].xyz, params.to_prev_view[2].xyz) * view;
	if (prev_view.z >= -1e-5) {
		return false;
	}
	vec2 prev_ndc = vec2(prev_view.x / -prev_view.z * params.prev_projection.y - params.prev_projection.x,
			prev_view.y / -prev_view.z * params.prev_projection.w - params.prev_projection.z);
	vec2 prev_uv = prev_ndc * 0.5 + 0.5;
	if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
		return false;
	}
	r_color = sample_history(prev_uv);
	return true;
}

// The history clipped to what this frame's samples around the pixel allow: variance clipping (Salvi,
// "An Excursion in Temporal Supersampling", GDC 2016) over the 3x3 drawn pixels nearest it, as Nubis
// bounds its reprojected clouds. The clouds drift and grow while each pixel waits 16 frames for its
// next sample; unclipped, the history trails behind them and the freshly drawn pixels stand out of
// it as a lattice of dots, and the long mean below would smear them.
vec4 clip_history(ivec2 px, vec4 p_prev) {
	// Bilinear taps at the pixel's own place in the low grid, so the bounds vary smoothly across a
	// block (nearest taps gave every pixel of a block one box, and clipped regions showed as squares).
	vec2 low_uv = ((vec2(px) - vec2(params.offset)) * 0.25 + 0.5) / vec2(params.low_size);
	vec2 texel = 1.0 / vec2(params.low_size);
	vec4 m1 = vec4(0.0);
	vec4 m2 = vec4(0.0);
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec4 s = textureLod(new_samples, low_uv + vec2(x, y) * texel, 0.0);
			m1 += s;
			m2 += s * s;
		}
	}
	m1 /= 9.0;
	vec4 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec4(0.0)));
	const float gamma = 1.25;
	vec4 hi = m1 + gamma * sigma;
	// Clip toward the mean along the line to it (keeps the hue, unlike a per-channel clamp).
	vec4 d = p_prev - m1;
	vec4 ext = max(hi - m1, vec4(1e-5));
	vec4 r = abs(d) / ext;
	float k = max(max(r.x, r.y), max(r.z, r.w));
	return k > 1.0 ? m1 + d / k : p_prev;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, params.size))) {
		return;
	}
	ivec2 q = px >> 2;
	ivec2 o = px & 3;
	vec4 prev;
	bool seen = reproject(px, prev);
	if (seen) {
		prev = clip_history(px, prev);
	}
	if (all(equal(o, params.offset)) && all(lessThan(q, params.low_size))) {
		// Drawn this frame: averaged with what this pixel held, so the march's jitter, which
		// changes every frame, settles instead of showing. Its first sample replaces the upsampled
		// fill it held (blocky, a sixteenth of the pixels), the next ones make a running mean, then
		// an exponential mean over about the last four (the history clip keeps it on the moving clouds; a weight of a
		// half left the jitter visible as a fixed checker across each 4x4 block, and kept the first
		// fill's blocks for several cycles).
		vec4 fresh = texelFetch(new_samples, q, 0);
		float w = 1.0 / float(min(max(params.drawn, 1u), 4u));
		imageStore(dest, px, seen ? mix(prev, fresh, w) : fresh);
		return;
	}
	if (seen) {
		imageStore(dest, px, prev);
		return;
	}

	// Not seen last frame: this frame's samples, bilinear between the blocks' drawn pixels.
	vec2 low_uv = ((vec2(px) - vec2(params.offset)) * 0.25 + 0.5) / vec2(params.low_size);
	imageStore(dest, px, textureLod(new_samples, low_uv, 0.0));
}

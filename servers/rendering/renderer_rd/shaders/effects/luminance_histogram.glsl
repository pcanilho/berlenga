#[compute]

#version 450

#VERSION_DEFINES

// Histogram auto exposure metering. BUILD_HISTOGRAM counts the frame's pixels into 64 bins of
// log2 brightness (the brightest channel, as luminance_reduce.glsl meters). RESOLVE_HISTOGRAM
// averages the pixels between a low and a high percentile, so a few lamps or a black sky do not
// set the exposure, applies the same adaptation as luminance_reduce.glsl and clears the bins.

#define BIN_COUNT 64
#define LOG2_MIN -16.0
#define LOG2_MAX 16.0

layout(set = 0, binding = 0, std430) restrict buffer Histogram {
	uint bins[BIN_COUNT];
}
histogram;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	float max_luminance;
	float min_luminance;
	float exposure_adjust;
	float low_fraction;
	float high_fraction;
	uint set_immediate;
	float dark_luminance;
	float dark_strength;
	float pad[2];
}
params;

float bin_center(uint p_bin) {
	return exp2(LOG2_MIN + (float(p_bin) + 0.5) * ((LOG2_MAX - LOG2_MIN) / float(BIN_COUNT)));
}

#ifdef BUILD_HISTOGRAM

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 1, binding = 0) uniform sampler2D source_texture;

shared uint local_bins[BIN_COUNT];

void main() {
	uint t = gl_LocalInvocationIndex;
	if (t < BIN_COUNT) {
		local_bins[t] = 0;
	}
	barrier();

	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(pos, params.source_size))) {
		vec3 v = texelFetch(source_texture, pos, 0).rgb;
		float l = max(v.r, max(v.g, v.b));
		// Zero, negative and NaN pixels go to the darkest bin.
		float f = l > 0.0 ? (log2(l) - LOG2_MIN) / (LOG2_MAX - LOG2_MIN) : 0.0;
		uint bin = uint(clamp(f * float(BIN_COUNT), 0.0, float(BIN_COUNT - 1)));
		atomicAdd(local_bins[bin], 1u);
	}
	barrier();

	if (t < BIN_COUNT && local_bins[t] != 0) {
		atomicAdd(histogram.bins[t], local_bins[t]);
	}
}

#endif

#ifdef RESOLVE_HISTOGRAM

layout(local_size_x = BIN_COUNT, local_size_y = 1, local_size_z = 1) in;

layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance;
layout(set = 2, binding = 0) uniform sampler2D prev_luminance;

shared uint counts[BIN_COUNT];

void main() {
	uint t = gl_LocalInvocationIndex;
	counts[t] = histogram.bins[t];
	barrier();
	histogram.bins[t] = 0; // Ready for the next frame.

	if (t != 0) {
		return;
	}

	uint total = 0;
	for (uint i = 0; i < BIN_COUNT; i++) {
		total += counts[i];
	}

	// Mean brightness of the pixels ranked between the low and high percentiles.
	float low = float(total) * params.low_fraction;
	float high = float(total) * params.high_fraction;
	float below = 0.0;
	float sum = 0.0;
	float weight = 0.0;
	for (uint i = 0; i < BIN_COUNT; i++) {
		float c = float(counts[i]);
		float take = clamp(below + c, low, high) - clamp(below, low, high);
		sum += take * bin_center(i);
		weight += take;
		below += c;
	}
	float avg = weight > 0.0 ? sum / weight : 0.0;

	if (params.dark_luminance > 0.0 && avg > 0.0 && avg < params.dark_luminance) {
		// Dark-scene key: a scene darker than dark_luminance is shown darker than mid-grey, by
		// dark_strength stops per stop below it (at most 4 stops), so night stays night. The
		// stored value is the luminance the tonemapper exposes for; adaptation runs on it too.
		float compensation = max(pow(avg / params.dark_luminance, params.dark_strength), 1.0 / 16.0);
		avg /= compensation;
	}

	if (params.set_immediate == 0) {
		float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
		// Clamped so a long frame (slow GPU, loading hitch) settles instead of overshooting.
		avg = prev_lum + (avg - prev_lum) * min(params.exposure_adjust, 1.0);
	}
	avg = clamp(avg, params.min_luminance, params.max_luminance);
	imageStore(dest_luminance, ivec2(0, 0), vec4(avg));
}

#endif

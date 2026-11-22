#[compute]
#version 450

// The dispersive wake step in Fourier space (eWave, Tessendorf 2014): every
// mode of the wake grid is an oscillator at its own deep-water frequency
// omega = sqrt(g k), stepped exactly, so long waves outrun short ones as on
// real water and a moving hull leaves the Kelvin pattern.
//
// The spatial pass packs z = (h - rest) + i v (height about the hull's dip,
// and its rate) and transforms it with water_fft_rows.glsl. That transform
// sums with e^{+i}; |k| is the same either way, so the update does not care.
// H and V are split from the one complex transform by Hermitian symmetry
// (h and v are real), each mode is turned by omega dt, and the result is
// written conjugated: transforming it again with the same e^{+i} sum then
// gives N^2 times the conjugate of the field, which the spatial pass undoes.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Pc {
	ivec4 grid; // x cells per side
	vec4 step; // x 2 pi / grid side (m), y dt (s), z damping per second, w grid filter per second at the highest wavenumber
};

layout(set = 0, binding = 0, std430) restrict readonly buffer SpecIn {
	vec2 spec_in[];
};
layout(set = 0, binding = 1, std430) restrict writeonly buffer SpecOut {
	vec2 spec_out[];
};

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	int n = grid.x;
	if (c.x >= n || c.y >= n) {
		return;
	}
	ivec2 m = ivec2(c.x < n / 2 ? c.x : c.x - n, c.y < n / 2 ? c.y : c.y - n);
	ivec2 p = ivec2((n - c.x) % n, (n - c.y) % n); // the mode at -k
	vec2 za = spec_in[c.y * n + c.x];
	vec2 zb = spec_in[p.y * n + p.x];
	zb.y = -zb.y; // conj
	vec2 hk = 0.5 * (za + zb);
	vec2 d = 0.5 * (za - zb);
	vec2 vk = vec2(d.y, -d.x); // (za - conj(zb)) / 2i

	float k = step.x * length(vec2(m));
	float dt = step.y;
	float omega = sqrt(9.81 * k);
	// Damping, plus a light filter that only touches the shortest waves the
	// grid holds (rate grows as (k / k_max)^4), so a sharp hull edge does not
	// leave grid-scale ripples.
	float kmax = step.x * float(n / 2) * 1.41421356;
	float kr = k / kmax;
	float decay = exp(-(step.z + step.w * kr * kr * kr * kr) * dt);
	vec2 h2, v2;
	if (omega < 1e-6) {
		h2 = hk + vk * dt;
		v2 = vk;
	} else {
		float co = cos(omega * dt);
		float si = sin(omega * dt);
		h2 = hk * co + vk * (si / omega);
		v2 = -hk * (omega * si) + vk * co;
	}
	h2 *= decay;
	v2 *= decay;
	vec2 z = vec2(h2.x - v2.y, h2.y + v2.x); // H + iV
	spec_out[c.y * n + c.x] = vec2(z.x, -z.y);
}

#[compute]
#version 450

// One-time (per configure()) generation of the base Tessendorf ocean
// spectrum h0 from a fetch-limited JONSWAP wind-sea spectrum with directional spreading, packed per texel as (xy = h0(k), zw = conj(h0(-k))) --
// both evaluated directly in this single pass via two independently-hashed
// Gaussian pairs (one seeded by (mx,mz), one by (-mx,-mz)), so there's no
// need to read a neighboring texel's already-written data and no ordering
// hazard. Standard technique, same shape as public Tessendorf-ocean
// reference implementations (and caustic-volume's own, though that one
// precomputes h0 differently -- this pass ports the FORMULA, not their
// upload path, since we generate it on GPU instead -- see water_solver.h's
// note on why). Written flat -- runs once at configure(), not every frame,
// but kept simple anyway: no nested helper calls beyond one level, no
// dynamic branching beyond simple guards -- see water_fft.glsl's header for
// why that discipline matters in this module right now.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 domain_wind; // xy domain size (world, full extent), z wind_speed, w wind_dir_x
	vec4 misc; // x wind_dir_z, y gravity, z amplitude (squared multiplier), w swell (0..1)
	ivec4 res; // x N, y spreading (0 = cos^2, 1 = Donelan-Banner, 2 = Hasselmann; 1 and 2 with Horvath's swell), zw unused
	vec4 extra; // x fetch (m), y depth (m), z k_min, w k_max (rad/m, 0 = unbounded: one cascade's band)
	vec4 remote; // remote swell: x significant height (m, 0 = none), y peak period (s), zw direction it travels (unit)
};

layout(set = 0, binding = 1, std430) restrict buffer H0 {
	vec4 h0[];
};

uint hash_u32(uint k) {
	k ^= k >> 16;
	k *= 0x7feb352du;
	k ^= k >> 15;
	k *= 0x846ca68bu;
	k ^= k >> 16;
	return k;
}

float rand01(uint seed) {
	return float(hash_u32(seed) & 0x00ffffffu) / 16777216.0;
}

vec2 gaussian_pair(ivec2 m) {
	uint seed = (uint(m.x + 32768) & 0xffffu) | ((uint(m.y + 32768) & 0xffffu) << 16);
	// The larger cascades (a band below the next smaller one: k_max set) draw their own numbers, salted by their
	// tile size. Seeded by the mode index alone, every cascade's band (the same mode indices on a tile `ratio`
	// times larger) was a scaled copy of the others: at the start their slopes added up in phase round the
	// origin and the summed crests read as folded (wide white patches), until the slower long waves drifted
	// apart. Independent draws per cascade are what the random-phase model asks for (Tessendorf 2001).
	if (extra.w > 0.0) {
		seed = hash_u32(seed ^ hash_u32(floatBitsToUint(domain_wind.x)));
	}
	float u1 = max(rand01(seed * 2u + 1u), 1e-6);
	float u2 = rand01(seed * 2u + 2u);
	float r = sqrt(-2.0 * log(u1));
	return vec2(r * cos(6.28318530718 * u2), r * sin(6.28318530718 * u2));
}

// ln Gamma(x) for x >= 0.5: Stirling's series after shifting x up by 3 (error under 1e-6).
float ln_gamma(float x) {
	float z = x + 3.0;
	float iz = 1.0 / z;
	float st = (z - 0.5) * log(z) - z + 0.918938533 + iz / 12.0 - iz * iz * iz / 360.0;
	return st - log(x * (x + 1.0) * (x + 2.0));
}

// Directional spreading D(w, theta), each integrating to 1 over the circle, as compared by Horvath
// (2015, "Empirical directional wave spectra for computer graphics", DigiPro, section 6):
// - Donelan-Banner (Donelan, Hamilton and Hui 1985; Banner 1990), his default: sech^2(b theta),
//   narrowest at the peak, wider above and below it (the first branch is used below 0.95 wp too).
// - Hasselmann et al. (1980): cos^2s(theta / 2) with the measured s(w), wider still for short waves.
// Horvath's swell (section 6.1) multiplies either by cos^2s(theta / 2) with s = 16 tanh(wp / w) swell^2
// and normalises the product again: long waves from a distant storm, nearly parallel crests.
float donelan_banner(float r, float theta) {
	float b = r < 0.95 ? 2.61 * pow(r, 1.3) : (r < 1.6 ? 2.28 * pow(r, -1.3) : pow(10.0, -0.4 + 0.8393 * exp(-0.567 * log(r * r))));
	float ch = cosh(b * theta);
	return b / (2.0 * tanh(b * 3.14159265) * ch * ch);
}

float hasselmann_s(float r, float wind_speed, float wp, float gravity) {
	return r <= 1.0 ? 6.97 * pow(r, 4.06) : 9.77 * pow(r, -2.33 - 1.45 * (wind_speed * wp / gravity - 1.17));
}

float spreading_horvath(int p_model, float w, float wp, float wind_speed, float gravity, float cos_theta, float swell) {
	float r = w / wp;
	float s_swell = 16.0 * tanh(1.0 / r) * swell * swell;
	if (p_model == 2) {
		// cos^2s(theta / 2) times cos^2s_swell(theta / 2) is the same form with the exponents added;
		// Q(s) = Gamma(s + 1) / (2 sqrt(pi) Gamma(s + 1/2)).
		float s = clamp(hasselmann_s(r, wind_speed, wp, gravity) + s_swell, 0.01, 2000.0);
		float q = exp(ln_gamma(s + 1.0) - ln_gamma(s + 0.5)) / 3.5449077;
		return q * pow(max(0.5 * (1.0 + cos_theta), 0.0), s);
	}
	float theta = acos(clamp(cos_theta, -1.0, 1.0));
	float d = donelan_banner(r, theta);
	if (s_swell < 1e-4) {
		return d;
	}
	// The product's normalisation by quadrature over [0, pi] (symmetric): 64 midpoints.
	float sum = 0.0;
	for (int i = 0; i < 64; i++) {
		float t = (float(i) + 0.5) * (3.14159265 / 64.0);
		sum += donelan_banner(r, t) * pow(0.5 * (1.0 + cos(t)), s_swell);
	}
	float norm = 2.0 * sum * (3.14159265 / 64.0);
	return d * pow(max(0.5 * (1.0 + cos_theta), 0.0), s_swell) / max(norm, 1e-12);
}

// TMA (JONSWAP in finite depth) wind-sea spectrum turned into a 2D wavenumber spectrum E(k)
// (m^4, integrating to the height variance). Fetch -- how far the wind has
// blown over open water -- sets both the energy (alpha) and the peak
// frequency, so a pool's meters of fetch give short, tiny ripples and an
// open sea's kilometers give long, tall waves at the same wind speed.
// Finite-depth dispersion w = sqrt(g k tanh(k d)) converts S(w) to S(k).
float jonswap(vec2 k, vec2 wind_dir, float wind_speed, float gravity, float fetch, float depth, int spreading, float swell) {
	float kl = length(k);
	if (kl < 1e-6) {
		return 0.0;
	}
	float th = tanh(min(kl * depth, 20.0));
	float w = sqrt(gravity * kl * th);
	float dw_dk = gravity * (th + kl * depth * (1.0 - th * th)) / (2.0 * w);
	float alpha = 0.076 * pow(wind_speed * wind_speed / (fetch * gravity), 0.22);
	float wp = 22.0 * pow(gravity * gravity / (wind_speed * fetch), 1.0 / 3.0);
	float sigma = w <= wp ? 0.07 : 0.09;
	float r = exp(-(w - wp) * (w - wp) / (2.0 * sigma * sigma * wp * wp));
	float s_w = alpha * gravity * gravity / pow(w, 5.0) * exp(-1.25 * pow(wp / w, 4.0)) * pow(3.3, r);
	// TMA (Bouws et al. 1985): in finite depth the spectrum is capped by the
	// Kitaigorodskii depth function of w_h = w sqrt(d / g) -- shallow water
	// holds less energy in the long waves (Horvath 2015's approximation).
	float wh = w * sqrt(depth / gravity);
	float phi = wh <= 1.0 ? 0.5 * wh * wh : (wh < 2.0 ? 1.0 - 0.5 * (2.0 - wh) * (2.0 - wh) : 1.0);
	s_w *= phi;
	float c = dot(k / kl, wind_dir);
	float spread;
	if (spreading >= 1) {
		spread = spreading_horvath(spreading, w, wp, wind_speed, gravity, c, swell);
	} else {
		// cos^2 spreading about the wind (normalized over the forward half), and
		// a weak back lobe so waves running against the wind aren't absent.
		spread = 0.63662 * c * c * (c >= 0.0 ? 1.0 : 0.04);
	}
	return s_w * dw_dk / kl * spread;
}

// Remote swell (docs/ideas/water.md "Remote swell"): waves from a distant storm, arriving from their own direction
// with their own period, on top of the local wind sea -- a two-peaked sea state (Ochi and Hubble 1976; Torsethaugen
// 1993; Horvath 2015 section 6.1 sums a swell spectrum the same way). The frequency spectrum is JONSWAP in Goda's
// (1988) significant-height form with a narrow peak (gamma 5: swell that has travelled far):
//   S(f) = beta Hs^2 Tp^-4 f^-5 exp(-1.25 (Tp f)^-4) gamma^r,
// and the spreading Mitsuyasu's cos^2s(theta / 2) with Goda's s_max 75 for long-decay swell (Goda 1985),
// s = s_max (w / wp)^5 below the peak and s_max (w / wp)^-2.5 above. The TMA depth factor as the wind sea's.
float remote_swell(vec2 k, float gravity, float depth) {
	float hs = remote.x;
	float tp = remote.y;
	float kl = length(k);
	if (hs <= 0.0 || tp <= 0.0 || kl < 1e-6) {
		return 0.0;
	}
	float th = tanh(min(kl * depth, 20.0));
	float w = sqrt(gravity * kl * th);
	float dw_dk = gravity * (th + kl * depth * (1.0 - th * th)) / (2.0 * w);
	float f = w / 6.28318530718;
	float fp = 1.0 / tp;
	const float gamma = 5.0;
	float beta = 0.0624 / (0.230 + 0.0336 * gamma - 0.185 / (1.9 + gamma)) * (1.094 - 0.01915 * log(gamma));
	float sg = f <= fp ? 0.07 : 0.09;
	float r = exp(-(f - fp) * (f - fp) / (2.0 * sg * sg * fp * fp));
	float s_f = beta * hs * hs * pow(tp, -4.0) * pow(f, -5.0) * exp(-1.25 * pow(tp * f, -4.0)) * pow(gamma, r);
	float s_w = s_f / 6.28318530718; // S(w) = S(f) / 2 pi
	float wh = w * sqrt(depth / gravity);
	s_w *= wh <= 1.0 ? 0.5 * wh * wh : (wh < 2.0 ? 1.0 - 0.5 * (2.0 - wh) * (2.0 - wh) : 1.0);
	float ratio = f / fp;
	float s = clamp(75.0 * (ratio <= 1.0 ? pow(ratio, 5.0) : pow(ratio, -2.5)), 0.01, 2000.0);
	float q = exp(ln_gamma(s + 1.0) - ln_gamma(s + 0.5)) / 3.5449077; // Q(s): cos^2s(theta / 2) over the circle is 1
	float c = dot(k / kl, normalize(remote.zw));
	float spread = q * pow(max(0.5 * (1.0 + c), 0.0), s);
	return s_w * dw_dk / kl * spread;
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	int n = res.x;
	if (c.x >= n || c.y >= n) {
		return;
	}
	int mx = c.x < n / 2 ? c.x : c.x - n;
	int mz = c.y < n / 2 ? c.y : c.y - n;

	if ((mx == -n / 2) || (mz == -n / 2) || (mx == 0 && mz == 0)) {
		h0[c.y * n + c.x] = vec4(0.0);
		return;
	}

	vec2 domain = domain_wind.xy;
	vec2 wind_dir = normalize(vec2(domain_wind.w, misc.x));
	float wind_speed = domain_wind.z;
	float gravity = misc.y;
	float amplitude = misc.z;

	vec2 k = 6.28318530718 * vec2(float(mx), float(mz)) / domain;
	vec2 g1 = gaussian_pair(ivec2(mx, mz));
	vec2 g2 = gaussian_pair(ivec2(-mx, -mz));
	float fetch = max(extra.x, 1.0);
	float depth = max(extra.y, 0.01);
	// water_fft.glsl's inverse transform is an unnormalized sum,
	// h(x) = sum_k h(k) e^{ikx}, so each mode carries its own share of the
	// variance directly: E(k) dk^2 split between h0(k) and conj(h0(-k)), each
	// a complex Gaussian (unit variance per component). With amplitude 1 the
	// field's height variance is the spectrum's integral over the resolved
	// wavenumbers -- physical meters, no empirical scale.
	float kl = length(k);
	if (kl < extra.z || (extra.w > 0.0 && kl >= extra.w)) {
		h0[c.y * n + c.x] = vec4(0.0);
		return;
	}
	float dk2 = (6.28318530718 / domain.x) * (6.28318530718 / domain.y);
	// The remote swell is not the wind's: wave_amplitude does not scale it.
	float p1 = jonswap(k, wind_dir, wind_speed, gravity, fetch, depth, res.y, misc.w) * dk2 * 0.5 * amplitude + remote_swell(k, gravity, depth) * dk2 * 0.5;
	float p2 = jonswap(-k, wind_dir, wind_speed, gravity, fetch, depth, res.y, misc.w) * dk2 * 0.5 * amplitude + remote_swell(-k, gravity, depth) * dk2 * 0.5;
	vec2 a = g1 * sqrt(p1 * 0.5);
	vec2 b_pre = g2 * sqrt(p2 * 0.5);
	vec2 b = vec2(b_pre.x, -b_pre.y); // conj(h0(-k))

	h0[c.y * n + c.x] = vec4(a, b);
}

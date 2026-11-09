// Hair scattering for the path tracer: Chiang, Bitterli, Tappan and Burley, "A Practical and
// Controllable Hair and Fur Model for Production Path Tracing" (Computer Graphics Forum 2016),
// written as pbrt-v3 section 9.9 / pbrt-v4 HairBxDF lay it out (Pharr, Jakob and Humphreys), with
// the parameter fits of the paper. A fibre is a rough dielectric cylinder (IOR 1.55) with a tilted
// cuticle; light leaves it after p internal paths: R (p = 0, white surface reflection shifted
// towards the root), TT (p = 1, coloured, the back-lit glow), TRT (p = 2, the coloured second
// highlight) and a residual lobe for every longer path, so a fibre without absorption returns all
// the light it receives (white furnace). Cross-checked against RTXCR-Material-Library's
// HairChiangBSDF.hlsli (NVIDIA, MIT); this file keeps pbrt's azimuthal scale sqrt(pi / 8) and draws
// the residual lobe as well, so the sampling density is the one the pdf below reports.
//
// Frame: x along the fibre (the hair tangent), y and z span its cross section. h in [-1, 1] is where
// the ray meets the fibre across its width; shells and cards have no fibre geometry, so the caller
// draws h uniformly, which averages over a fibre as a camera far from it sees.
//
// Values are the fibre's contribution per unit solid angle with the cosine term already in (the
// sum of M_p A_p N_p), as evalCombinedBRDFSeparate() returns its lobes: a light of radiance Li from
// direction wi adds hair_bsdf_eval(...) * Li.

#ifndef RT_HAIR_INC
#define RT_HAIR_INC

#define RT_HAIR_P_MAX 3
#define RT_HAIR_PI 3.14159265358979
#define RT_HAIR_IOR 1.55

struct HairFibre {
	float h; // Offset across the fibre, [-1, 1].
	float gamma_o; // asin(h).
	vec3 sigma_a; // Absorption per fibre diameter.
	float v[RT_HAIR_P_MAX + 1]; // Longitudinal variance per lobe.
	float s; // Azimuthal logistic scale.
	float sin2k_alpha[3]; // sin(2^k alpha), alpha the cuticle tilt.
	float cos2k_alpha[3];
};

float hair_safe_sqrt(float x) {
	return sqrt(max(x, 0.0));
}

float hair_safe_asin(float x) {
	return asin(clamp(x, -1.0, 1.0));
}

// The modified Bessel function I0, its power series to ten terms.
float hair_i0(float x) {
	float val = 0.0;
	float x2i = 1.0;
	float ifact = 1.0;
	float i4 = 1.0;
	for (int i = 0; i < 10; i++) {
		if (i > 1) {
			ifact *= float(i);
		}
		val += x2i / (i4 * ifact * ifact);
		x2i *= x * x;
		i4 *= 4.0;
	}
	return val;
}

float hair_log_i0(float x) {
	if (x > 12.0) {
		return x + 0.5 * (-log(2.0 * RT_HAIR_PI) + log(1.0 / x) + 1.0 / (8.0 * x));
	}
	return log(hair_i0(x));
}

// Longitudinal scattering M_p (d'Eon et al. 2011), in the form stable for small variances.
float hair_mp(float cos_theta_i, float cos_theta_o, float sin_theta_i, float sin_theta_o, float v) {
	float a = cos_theta_i * cos_theta_o / v;
	float b = sin_theta_i * sin_theta_o / v;
	if (v <= 0.1) {
		return exp(hair_log_i0(a) - b - 1.0 / v + 0.6931 + log(1.0 / (2.0 * v)));
	}
	return exp(-b) * hair_i0(a) / (sinh(1.0 / v) * 2.0 * v);
}

// Unpolarised Fresnel reflectance of a dielectric of index eta, entered from air.
float hair_fresnel(float cos_theta_i, float eta) {
	cos_theta_i = clamp(cos_theta_i, -1.0, 1.0);
	if (cos_theta_i < 0.0) {
		eta = 1.0 / eta;
		cos_theta_i = -cos_theta_i;
	}
	float sin2_theta_t = (1.0 - cos_theta_i * cos_theta_i) / (eta * eta);
	if (sin2_theta_t >= 1.0) {
		return 1.0;
	}
	float cos_theta_t = hair_safe_sqrt(1.0 - sin2_theta_t);
	float r_parl = (eta * cos_theta_i - cos_theta_t) / (eta * cos_theta_i + cos_theta_t);
	float r_perp = (cos_theta_i - eta * cos_theta_t) / (cos_theta_i + eta * cos_theta_t);
	return 0.5 * (r_parl * r_parl + r_perp * r_perp);
}

// Attenuation A_p of each lobe for outgoing elevation cos_theta_o and single-pass transmittance t.
void hair_ap(float cos_theta_o, float h, vec3 t, out vec3 ap[RT_HAIR_P_MAX + 1]) {
	float cos_gamma_o = hair_safe_sqrt(1.0 - h * h);
	float f = hair_fresnel(cos_theta_o * cos_gamma_o, RT_HAIR_IOR);
	ap[0] = vec3(f);
	ap[1] = (1.0 - f) * (1.0 - f) * t;
	for (int p = 2; p < RT_HAIR_P_MAX; p++) {
		ap[p] = ap[p - 1] * t * f;
	}
	ap[RT_HAIR_P_MAX] = ap[RT_HAIR_P_MAX - 1] * f * t / max(vec3(1.0) - t * f, vec3(1e-6));
}

float hair_phi(int p, float gamma_o, float gamma_t) {
	return 2.0 * float(p) * gamma_t - 2.0 * gamma_o + float(p) * RT_HAIR_PI;
}

float hair_logistic(float x, float s) {
	x = abs(x);
	float e = exp(-x / s);
	return e / (s * (1.0 + e) * (1.0 + e));
}

float hair_logistic_cdf(float x, float s) {
	return 1.0 / (1.0 + exp(-x / s));
}

float hair_trimmed_logistic(float x, float s) {
	return hair_logistic(x, s) / (hair_logistic_cdf(RT_HAIR_PI, s) - hair_logistic_cdf(-RT_HAIR_PI, s));
}

float hair_sample_trimmed_logistic(float u, float s) {
	float lo = hair_logistic_cdf(-RT_HAIR_PI, s);
	float k = hair_logistic_cdf(RT_HAIR_PI, s) - lo;
	float x = -s * log(1.0 / (u * k + lo) - 1.0);
	return clamp(x, -RT_HAIR_PI, RT_HAIR_PI);
}

// Azimuthal scattering N_p: a trimmed logistic around the lobe's exit angle.
float hair_np(float phi, int p, float s, float gamma_o, float gamma_t) {
	float dphi = phi - hair_phi(p, gamma_o, gamma_t);
	dphi = mod(dphi + RT_HAIR_PI, 2.0 * RT_HAIR_PI) - RT_HAIR_PI;
	return hair_trimmed_logistic(dphi, s);
}

// p_beta_m: longitudinal roughness, p_beta_n: azimuthal roughness, both [0, 1];
// p_alpha_deg: cuticle tilt in degrees; p_sigma_a: absorption per fibre diameter.
HairFibre hair_fibre(float p_h, vec3 p_sigma_a, float p_beta_m, float p_beta_n, float p_alpha_deg) {
	HairFibre f;
	f.h = clamp(p_h, -0.999, 0.999);
	f.gamma_o = hair_safe_asin(f.h);
	f.sigma_a = max(p_sigma_a, vec3(0.0));
	float bm = clamp(p_beta_m, 0.01, 1.0);
	float bn = clamp(p_beta_n, 0.01, 1.0);
	float vm = 0.726 * bm + 0.812 * bm * bm + 3.7 * pow(bm, 20.0);
	f.v[0] = vm * vm;
	f.v[1] = 0.25 * f.v[0];
	f.v[2] = 4.0 * f.v[0];
	f.v[3] = f.v[2];
	f.s = 0.626657069 * (0.265 * bn + 1.194 * bn * bn + 5.372 * pow(bn, 22.0)); // sqrt(pi / 8) * fit
	f.sin2k_alpha[0] = sin(radians(p_alpha_deg));
	f.cos2k_alpha[0] = hair_safe_sqrt(1.0 - f.sin2k_alpha[0] * f.sin2k_alpha[0]);
	for (int i = 1; i < 3; i++) {
		f.sin2k_alpha[i] = 2.0 * f.cos2k_alpha[i - 1] * f.sin2k_alpha[i - 1];
		f.cos2k_alpha[i] = f.cos2k_alpha[i - 1] * f.cos2k_alpha[i - 1] - f.sin2k_alpha[i - 1] * f.sin2k_alpha[i - 1];
	}
	return f;
}

// The outgoing elevation tilted by the cuticle for lobe p.
void hair_tilt(HairFibre f, int p, float sin_theta_o, float cos_theta_o, out float sin_op, out float cos_op) {
	if (p == 0) {
		sin_op = sin_theta_o * f.cos2k_alpha[1] - cos_theta_o * f.sin2k_alpha[1];
		cos_op = cos_theta_o * f.cos2k_alpha[1] + sin_theta_o * f.sin2k_alpha[1];
	} else if (p == 1) {
		sin_op = sin_theta_o * f.cos2k_alpha[0] + cos_theta_o * f.sin2k_alpha[0];
		cos_op = cos_theta_o * f.cos2k_alpha[0] - sin_theta_o * f.sin2k_alpha[0];
	} else if (p == 2) {
		sin_op = sin_theta_o * f.cos2k_alpha[2] + cos_theta_o * f.sin2k_alpha[2];
		cos_op = cos_theta_o * f.cos2k_alpha[2] - sin_theta_o * f.sin2k_alpha[2];
	} else {
		sin_op = sin_theta_o;
		cos_op = cos_theta_o;
	}
	cos_op = abs(cos_op);
}

// gamma_t and the single-pass transmittance for outgoing elevation sin_theta_o.
float hair_gamma_t(HairFibre f, float sin_theta_o, float cos_theta_o) {
	float etap = sqrt(RT_HAIR_IOR * RT_HAIR_IOR - sin_theta_o * sin_theta_o) / max(cos_theta_o, 1e-5);
	return hair_safe_asin(f.h / etap);
}

vec3 hair_transmittance(HairFibre f, float sin_theta_o, float gamma_t) {
	float sin_theta_t = sin_theta_o / RT_HAIR_IOR;
	float cos_theta_t = hair_safe_sqrt(1.0 - sin_theta_t * sin_theta_t);
	return exp(-f.sigma_a * (2.0 * cos(gamma_t) / max(cos_theta_t, 1e-5)));
}

// Lobe selection probabilities: each lobe's share of the luminance A_p carries.
void hair_ap_pdf(HairFibre f, float sin_theta_o, float cos_theta_o, out float ap_pdf[RT_HAIR_P_MAX + 1]) {
	float gamma_t = hair_gamma_t(f, sin_theta_o, cos_theta_o);
	vec3 ap[RT_HAIR_P_MAX + 1];
	hair_ap(cos_theta_o, f.h, hair_transmittance(f, sin_theta_o, gamma_t), ap);
	float sum = 0.0;
	for (int p = 0; p <= RT_HAIR_P_MAX; p++) {
		ap_pdf[p] = dot(ap[p], vec3(0.2126, 0.7152, 0.0722));
		sum += ap_pdf[p];
	}
	float inv = sum > 0.0 ? 1.0 / sum : 0.0;
	for (int p = 0; p <= RT_HAIR_P_MAX; p++) {
		ap_pdf[p] = sum > 0.0 ? ap_pdf[p] * inv : (p == 0 ? 1.0 : 0.0);
	}
}

// wo, wi: unit vectors in the fibre frame (x along the fibre). Returns sum_p M_p A_p N_p.
vec3 hair_f(HairFibre f, vec3 wo, vec3 wi) {
	float sin_theta_o = clamp(wo.x, -1.0, 1.0);
	float cos_theta_o = hair_safe_sqrt(1.0 - sin_theta_o * sin_theta_o);
	float phi_o = atan(wo.z, wo.y);
	float sin_theta_i = clamp(wi.x, -1.0, 1.0);
	float cos_theta_i = hair_safe_sqrt(1.0 - sin_theta_i * sin_theta_i);
	float phi_i = atan(wi.z, wi.y);
	float gamma_t = hair_gamma_t(f, sin_theta_o, cos_theta_o);
	vec3 ap[RT_HAIR_P_MAX + 1];
	hair_ap(cos_theta_o, f.h, hair_transmittance(f, sin_theta_o, gamma_t), ap);
	float phi = phi_i - phi_o;
	vec3 sum = vec3(0.0);
	for (int p = 0; p < RT_HAIR_P_MAX; p++) {
		float sin_op, cos_op;
		hair_tilt(f, p, sin_theta_o, cos_theta_o, sin_op, cos_op);
		sum += hair_mp(cos_theta_i, cos_op, sin_theta_i, sin_op, f.v[p]) * ap[p] * hair_np(phi, p, f.s, f.gamma_o, gamma_t);
	}
	sum += hair_mp(cos_theta_i, cos_theta_o, sin_theta_i, sin_theta_o, f.v[RT_HAIR_P_MAX]) * ap[RT_HAIR_P_MAX] / (2.0 * RT_HAIR_PI);
	return max(sum, vec3(0.0));
}

// Solid-angle density with which hair_sample() draws wi.
float hair_pdf(HairFibre f, vec3 wo, vec3 wi) {
	float sin_theta_o = clamp(wo.x, -1.0, 1.0);
	float cos_theta_o = hair_safe_sqrt(1.0 - sin_theta_o * sin_theta_o);
	float phi_o = atan(wo.z, wo.y);
	float sin_theta_i = clamp(wi.x, -1.0, 1.0);
	float cos_theta_i = hair_safe_sqrt(1.0 - sin_theta_i * sin_theta_i);
	float phi_i = atan(wi.z, wi.y);
	float gamma_t = hair_gamma_t(f, sin_theta_o, cos_theta_o);
	float ap_pdf[RT_HAIR_P_MAX + 1];
	hair_ap_pdf(f, sin_theta_o, cos_theta_o, ap_pdf);
	float phi = phi_i - phi_o;
	float pdf = 0.0;
	for (int p = 0; p < RT_HAIR_P_MAX; p++) {
		float sin_op, cos_op;
		hair_tilt(f, p, sin_theta_o, cos_theta_o, sin_op, cos_op);
		pdf += hair_mp(cos_theta_i, cos_op, sin_theta_i, sin_op, f.v[p]) * ap_pdf[p] * hair_np(phi, p, f.s, f.gamma_o, gamma_t);
	}
	pdf += hair_mp(cos_theta_i, cos_theta_o, sin_theta_i, sin_theta_o, f.v[RT_HAIR_P_MAX]) * ap_pdf[RT_HAIR_P_MAX] / (2.0 * RT_HAIR_PI);
	return pdf;
}

// Draws wi for wo (fibre frame): a lobe by ap_pdf, the elevation from M_p, the azimuth from N_p.
// u0, u1: four uniform numbers.
vec3 hair_sample(HairFibre f, vec3 wo, vec2 u0, vec2 u1) {
	float sin_theta_o = clamp(wo.x, -1.0, 1.0);
	float cos_theta_o = hair_safe_sqrt(1.0 - sin_theta_o * sin_theta_o);
	float phi_o = atan(wo.z, wo.y);
	float ap_pdf[RT_HAIR_P_MAX + 1];
	hair_ap_pdf(f, sin_theta_o, cos_theta_o, ap_pdf);
	int p = 0;
	for (; p < RT_HAIR_P_MAX; p++) {
		if (u0.x < ap_pdf[p]) {
			break;
		}
		u0.x -= ap_pdf[p];
	}
	float sin_op, cos_op;
	hair_tilt(f, p, sin_theta_o, cos_theta_o, sin_op, cos_op);
	float vp = f.v[p];
	float ux = max(u1.x, 1e-5);
	float cos_theta = clamp(1.0 + vp * log(ux + (1.0 - ux) * exp(-2.0 / vp)), -1.0, 1.0);
	float sin_theta = hair_safe_sqrt(1.0 - cos_theta * cos_theta);
	float cos_phi = cos(2.0 * RT_HAIR_PI * u1.y);
	float sin_theta_i = clamp(-cos_theta * sin_op + sin_theta * cos_phi * cos_op, -1.0, 1.0);
	float cos_theta_i = hair_safe_sqrt(1.0 - sin_theta_i * sin_theta_i);
	float gamma_t = hair_gamma_t(f, sin_theta_o, cos_theta_o);
	float dphi;
	if (p < RT_HAIR_P_MAX) {
		dphi = hair_phi(p, f.gamma_o, gamma_t) + hair_sample_trimmed_logistic(u0.y, f.s);
	} else {
		dphi = 2.0 * RT_HAIR_PI * u0.y;
	}
	float phi_i = phi_o + dphi;
	return vec3(sin_theta_i, cos_theta_i * cos(phi_i), cos_theta_i * sin(phi_i));
}

// Chiang et al. 2016, section 4.2: a mass of fibres with absorption sigma_a (per fibre diameter)
// and azimuthal roughness beta_n looks colour exp(-d(beta_n) sqrt(sigma_a)) once light has scattered
// through many of them.
vec3 hair_color_from_sigma_a(vec3 sigma_a, float beta_n) {
	float b = clamp(beta_n, 0.01, 1.0);
	float d = 5.969 - 0.215 * b + 2.532 * b * b - 10.73 * pow(b, 3.0) + 5.574 * pow(b, 4.0) + 0.245 * pow(b, 5.0);
	return exp(-d * sqrt(max(sigma_a, vec3(0.0))));
}

// Shells and cards stand for a whole mass of hair but scatter once, so the light that would wander
// through many fibres gets one fibre with this absorption instead: a fibre's single-scatter albedo
// with sigma_a = -ln(c) / (pi / 2) comes out close to c (within 0.05 for c from 0.3 to 0.9 at
// beta 0.3, checked by integrating hair_f), the colour the mass of hair shows.
vec3 hair_mass_sigma_a(vec3 c) {
	return -log(clamp(c, vec3(1e-4), vec3(1.0))) * (2.0 / RT_HAIR_PI);
}

// World <-> fibre frame. t: unit fibre direction; n: a unit vector off it (the surface normal).
mat3 hair_frame(vec3 t, vec3 n) {
	vec3 z = n - dot(n, t) * t;
	if (dot(z, z) < 1e-8) {
		z = abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		z -= dot(z, t) * t;
	}
	z = normalize(z);
	vec3 y = cross(z, t);
	return mat3(t, y, z); // columns: world = frame * local; local = transpose(frame) * world
}

#endif // RT_HAIR_INC

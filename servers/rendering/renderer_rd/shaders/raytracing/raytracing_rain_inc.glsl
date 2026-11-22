// Raindrop rings on water (docs/ideas/rain.md, step 1). One source for every water surface: the
// sea, lakes, pools and rivers (their ShaderMaterials), the ground's puddles (BaseMaterial3D) and the
// path tracer's twin of them (scene_raytracing_raygen.glsl includes this file; the C++ side reads it
// as rain_ripples_glsl.gen.h, servers/rendering/SCsub). Pure functions: the caller brings the rain.
//
// The impacts: Marshall and Palmer (1948) sizes, N(D) = 8000 exp(-L D) per m3 per mm with
// L = 4.1 R^-0.21 per mm, falling at Atlas, Srivastava and Sekhon's (1973) terminal speed
// v = 9.65 - 10.3 exp(-0.6 D) m/s, so the drops reaching a square metre each second between sizes
// D1 and D2 are the integral of N(D) v(D), in closed form below (about 3,000 above 0.5 mm at 10 mm/h).
//
// The rings: each impact sends out a gravity-capillary wave packet (Le Mehaute 1988, JFM 197). Its
// waves sit near the dispersion minimum of water, wavelength about 1.7 cm and speed about 0.23 m/s,
// so the rings grow at that speed whatever the drop; bigger drops make taller, longer-lived rings with
// longer waves. The packet spreads as it travels (dispersion) and its height falls with the square
// root of its radius (energy spread round a growing circle) and with viscous damping.
//
// Drawn as Lagarde's (2013, Remember Me) ring layers: a jittered grid per drop-size class where each
// cell runs one ring at a time, period its life, at a random phase. The rate sets how many cells are
// live; impacts beyond one ring per cell, and rings finer than the pixel (footprint, m; 0 = full
// detail, as the path tracer has), add their slope variance to the surface's roughness instead
// (Bruneton et al. 2010's split of resolved and unresolved slopes), so heavy rain dulls the
// reflections as it does on a real pond and no ring aliases.

uint rain_ring_hash(uint x) {
	x ^= x >> 16u;
	x *= 2146121005u;
	x ^= x >> 15u;
	x *= 2221713035u;
	x ^= x >> 16u;
	return x;
}

float rain_ring_rand(uint x) {
	return float(rain_ring_hash(x) >> 8u) * (1.0 / 16777216.0);
}

// Drops between p_d1 and p_d2 mm landing on a square metre each second at p_rate mm/h.
float rain_impact_flux(float p_rate, float p_d1, float p_d2) {
	if (p_rate <= 0.0) {
		return 0.0;
	}
	float lam = 4.1 * pow(max(p_rate, 0.01), -0.21);
	float l2 = lam + 0.6;
	float f = 9.65 * (exp(-lam * p_d1) - exp(-lam * p_d2)) / lam - 10.3 * (exp(-l2 * p_d1) - exp(-l2 * p_d2)) / l2;
	return max(8000.0 * f, 0.0);
}

// The slope (dh/dx, dh/dz) of one ring p_age seconds old at offset p_d from its impact.
vec2 rain_ring_slope(vec2 p_d, float p_age, float p_life, float p_wavelength, float p_slope) {
	float r = length(p_d);
	float front = 0.004 + 0.23 * p_age;
	float sig = p_wavelength * (0.7 + 1.3 * p_age / p_life);
	float x = r - front;
	if (abs(x) > 3.0 * sig) {
		return vec2(0.0);
	}
	float k = 6.2831853 / p_wavelength;
	float u = 1.0 - p_age / p_life;
	float g = u * u * sqrt(0.02 / (0.02 + 0.23 * p_age));
	float s = p_slope * g * (-sin(k * x) - 2.0 * x / (k * sig * sig) * cos(k * x)) * exp(-x * x / (sig * sig));
	return s * p_d / max(r, 1e-4);
}

// One size class: cells p_cell wide, rings living p_life seconds, p_live of the cells live per period.
vec2 rain_ring_layer(vec2 p_xz, float p_time, float p_cell, float p_life, float p_wavelength, float p_slope, float p_live, uint p_seed) {
	vec2 g = p_xz / p_cell;
	ivec2 c0 = ivec2(floor(g));
	vec2 acc = vec2(0.0);
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			ivec2 c = c0 + ivec2(i, j);
			uint h = rain_ring_hash((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ p_seed);
			float u = p_time / p_life + rain_ring_rand(h);
			float period = floor(u);
			uint h2 = rain_ring_hash(h ^ (uint(int(period)) * 83492791u));
			if (rain_ring_rand(h2) >= p_live) {
				continue;
			}
			// The impact inside the middle 70% of its cell, so a ring never reaches past the 3 x 3 block.
			vec2 centre = (vec2(c) + 0.15 + 0.7 * vec2(rain_ring_rand(h2 + 1u), rain_ring_rand(h2 + 2u))) * p_cell;
			float age = (u - period) * p_life;
			acc += rain_ring_slope(p_xz - centre, age, p_life, p_wavelength, p_slope * (0.6 + 0.8 * rain_ring_rand(h2 + 3u)));
		}
	}
	return acc;
}

// Rings from rain at p_rate mm/h (liquid) at world p_xz and time p_time (s): (dh/dx, dh/dz, the slope
// variance left to the roughness). Three classes: drops 0.5 to 1 mm, 1 to 2 mm and above 2 mm. Each
// class's cell is (front + three packet widths at the end of its life) / 1.15, so its rings stay in
// the 3 x 3 cells around the point; its variance constant is the ring's integral of slope squared over
// area and life (numerically, at unit peak slope), times the mean square of the 0.6 to 1.4 size spread.
vec3 rain_ripples(vec2 p_xz, float p_time, float p_rate, float p_footprint) {
	if (p_rate < 0.01) {
		return vec3(0.0);
	}
	vec3 r = vec3(0.0);
	for (int i = 0; i < 3; i++) {
		float d1 = i == 0 ? 0.5 : (i == 1 ? 1.0 : 2.0);
		float d2 = i == 0 ? 1.0 : (i == 1 ? 2.0 : 8.0);
		float life = i == 0 ? 0.45 : (i == 1 ? 0.7 : 1.0);
		float wl = i == 0 ? 0.012 : (i == 1 ? 0.016 : 0.022);
		float s0 = i == 0 ? 0.10 : (i == 1 ? 0.18 : 0.28);
		float cell = i == 0 ? 0.16 : (i == 1 ? 0.23 : 0.32);
		float kappa = i == 0 ? 4.35e-5 : (i == 1 ? 1.02e-4 : 2.21e-4);
		float flux = rain_impact_flux(p_rate, d1, d2);
		float n = flux * cell * cell * life; // impacts per cell and period
		float variance = flux * s0 * s0 * kappa * 1.0533;
		float shown = 1.0 - smoothstep(0.3 * wl, wl, p_footprint);
		float live = min(n, 1.0);
		r.z += variance * ((1.0 - shown) + shown * max(n - 1.0, 0.0) / max(n, 1e-6));
		if (shown > 0.01 && live > 1e-4) {
			r.xy += shown * rain_ring_layer(p_xz, p_time, cell, life, wl, s0, live, uint(i + 1) * 2654435761u);
		}
	}
	return r;
}

// A roughness that also carries p_variance of unresolved slope (Beckmann alpha^2 = 2 sigma^2, added to
// the surface's own alpha^2; alpha is roughness squared).
float rain_ripple_roughness(float p_roughness, float p_variance) {
	float a = p_roughness * p_roughness;
	return sqrt(sqrt(a * a + 2.0 * p_variance));
}

// Share of precipitation that falls as rain (Jennings et al. 2018; WeatherMap3D::rain_fraction()).
float rain_liquid_share(float p_celsius, float p_humidity) {
	float e = -10.04 + 1.41 * p_celsius + 0.09 * clamp(p_humidity, 0.0, 1.0) * 100.0;
	return 1.0 - 1.0 / (1.0 + exp(clamp(e, -60.0, 60.0)));
}

// The same under a warm layer aloft (WeatherMap3D::liquid_share()): p_height m above the map's ground, the warm nose's
// peak (C, 1 or less: none) and its base (m above the ground). The snow it melts reaches the ground as rain unless a
// deep cold layer under it refreezes the drops to ice pellets (Bourgouin 2000).
float rain_liquid_share_nose(float p_celsius, float p_humidity, float p_height, float p_nose_celsius, float p_nose_base) {
	float plain = rain_liquid_share(p_celsius, p_humidity);
	if (p_nose_celsius <= 1.0) {
		return plain;
	}
	float x = min((p_nose_celsius - 1.0) / 2.0, 1.0);
	float melt = x * x * (3.0 - 2.0 * x);
	if (p_height >= p_nose_base) {
		return max(plain, melt);
	}
	float pa = 9.81 * 1000.0 * log(1.0 + 0.5 * p_nose_celsius / 273.15);
	float na = p_celsius < 0.0 ? 9.81 * (p_nose_base - p_height) * -log(1.0 + 0.5 * p_celsius / 273.15) : 0.0;
	float stays = 46.0 + 0.66 * pa;
	float pellets = 66.0 + 0.66 * pa;
	return max(plain, melt * (1.0 - clamp((na - stays) / (pellets - stays), 0.0, 1.0)));
}

// Water beaded on a wet leaf (docs/ideas/rain.md, step 5). A waxy leaf is hydrophobic: rain stands on it as drops
// rather than a film (contact angles of 90 to 150 degrees on most leaves; Wang et al. 2014's survey), so a wet leaf
// shows small bright caps, not a darker blade. Lagarde (2013, "Water drop 4a") draws them as a drop texture; here a
// jittered 3D grid of 5 mm cells round p_world, each holding at most one drop (contact radius 0.6 to 1.3 mm, inside
// its cell) with probability rising with p_wet. Returns the slope to add to the normal (world, in the surface's
// plane) and the share of the point the drop covers. Finer than the pixel (p_footprint, m; 0 for full detail) the
// caps fade to their mean cover, which the caller turns into a glossier surface.
vec4 rain_surface_beads(vec3 p_world, vec3 p_normal, float p_wet, float p_footprint) {
	if (p_wet <= 0.01) {
		return vec4(0.0);
	}
	const float cell = 0.005;
	vec3 g = p_world / cell;
	ivec3 c = ivec3(floor(g));
	uint h = rain_ring_hash((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u));
	float mean_cover = 0.35 * p_wet;
	if (rain_ring_rand(h) > 0.6 * p_wet) {
		return vec4(0.0, 0.0, 0.0, 0.0);
	}
	vec3 centre = (vec3(c) + 0.3 + 0.4 * vec3(rain_ring_rand(h + 1u), rain_ring_rand(h + 2u), rain_ring_rand(h + 3u))) * cell;
	float radius = mix(0.0006, 0.0013, rain_ring_rand(h + 4u));
	vec3 d = p_world - centre;
	d -= p_normal * dot(d, p_normal);
	float r = length(d) / radius;
	float shown = 1.0 - smoothstep(0.5 * radius, 2.0 * radius, p_footprint);
	if (r >= 1.0 || shown <= 0.0) {
		return vec4(0.0, 0.0, 0.0, mean_cover * (1.0 - shown));
	}
	// A spherical cap with a contact angle of about 100 degrees: its slope rises to vertical at the rim.
	vec3 slope = d / radius * (1.0 / sqrt(max(1.0 - r * r, 0.02)));
	return vec4(slope * shown, mix(mean_cover, 1.0, shown));
}

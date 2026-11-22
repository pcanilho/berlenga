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

// A footstep, a wheel or a dropped stone in shallow standing water (docs/ideas/rain.md, step 7): one impact
// p_impact = (x, z, age s, amplitude; 1 a walker's step). A sole slapping into a puddle sends out a short train of
// waves from its edge. In water a centimetre deep their wavelength (about 4 cm) sits where gravity and surface tension
// both matter, with depth: w^2 = (g k + sigma k^3 / rho) tanh(k h) (Lamb 1932, 267), about 0.26 m/s at h = 1 cm,
// so the train's front leaves the sole's edge (8 cm) at that speed. The train is wider than a raindrop's packet
// (a sole is not a point) and spreads as it travels; its height falls with the square root of its radius and with
// damping in the shallow layer (Lighthill 1978), over its 1.6 s life. Finer than the pixel (p_footprint, m) it fades.
vec2 water_impact_ripple(vec2 p_xz, vec4 p_impact, float p_footprint) {
	const float life = 1.6;
	const float wl = 0.04;
	const float r0 = 0.08;
	float age = p_impact.z;
	if (p_impact.w <= 0.0 || age < 0.0 || age >= life) {
		return vec2(0.0);
	}
	vec2 d = p_xz - p_impact.xy;
	float r = length(d);
	float front = r0 + 0.26 * age;
	float sig = wl * (1.2 + 2.0 * age / life);
	float x = r - front;
	if (abs(x) > 3.0 * sig) {
		return vec2(0.0);
	}
	float k = 6.2831853 / wl;
	float u = 1.0 - age / life;
	float g = u * u * sqrt(r0 / (r0 + 0.26 * age));
	float shown = 1.0 - smoothstep(0.3 * wl, wl, p_footprint);
	float s = 0.3 * p_impact.w * shown * g * (-sin(k * x) - 2.0 * x / (k * sig * sig) * cos(k * x)) * exp(-x * x / (sig * sig));
	return s * d / max(r, 1e-4);
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

// Rain caught by a face (docs/ideas/engine-gaps-from-game.md, A; docs/ideas/rain.md, step 7). The rain falls slanted
// by the wind, so its flux is a vector: R straight down (mm/h on level ground) and, across, the wind-driven rain of
// ISO 15927-3 (Lacy 1965; Blocken and Carmeliet 2004), (2/9) U R^(8/9) mm/h on a face square to a wind of U m/s
// (the airfield index: the free catch, before a building's own wall factor). A face catches the flux it turns
// into, so a level roof catches R, a windward wall the driven rain, a leeward wall and an underside nothing, and a
// slope leaning into the wind more than R. Canopy drip falls straight down. Pure: the caller brings the weather.
// p_normal: the face's world normal (unit, outward). p_rain: liquid rain (mm/h). p_drip: canopy drip (mm/h).
// p_wind: the 10 m wind (m/s, world xz, the way it blows). p_film: the weather's surface film there (0..1: how long
// the rain has been at it). Returns x the catch (mm/h on the face), y the film, zw the wind's push on a drop along
// the face (world xz, in units of the drop's weight).
vec4 rain_surface_catch(vec3 p_normal, float p_rain, float p_drip, vec2 p_wind, float p_film) {
	float rain = max(p_rain, 0.0);
	float across = rain > 0.0 ? (2.0 / 9.0) * pow(rain, 8.0 / 9.0) : 0.0;
	float c = max(p_normal.y * rain - across * dot(p_normal.xz, p_wind), 0.0) + max(p_drip, 0.0) * max(p_normal.y, 0.0);
	// The push: drag on a sessile drop of about 3 mm (0.5 rho Cd A u^2 with Cd about 1, A about 7 mm2) over its
	// weight (about 0.14 mN) is 0.03 u^2; the air at the face moves at about 0.6 of the 10 m wind (the log profile a
	// few metres up, slowed further against the face), so 0.011 U^2 along the face's own plane.
	vec3 w = vec3(p_wind.x, 0.0, p_wind.y);
	w -= p_normal * dot(w, p_normal);
	vec2 push = 0.011 * w.xz * length(w);
	float pl = length(push);
	push *= pl > 3.0 ? 3.0 / pl : 1.0;
	return vec4(c, clamp(p_film, 0.0, 1.0), push);
}

// The way water runs on a face: gravity along the face plus the wind's push (rain_surface_catch().zw), in the face's
// plane. Not normalised: its length is the drive (1 down a vertical face in still air, 0 on a level one).
vec3 rain_surface_fall_line(vec3 p_normal, vec2 p_push) {
	vec3 f = vec3(p_push.x, -1.0, p_push.y);
	return f - p_normal * dot(f, p_normal);
}

// Rain running down a wall in streaks (docs/ideas/engine-gaps-from-game.md, A): a falling film gathers into
// rivulets where the face's dirt and joints lead it (its roll waves, Kapitza 1948, are centimetres apart and not
// drawn). Each 4.5 cm column of the face holds one streak, 1 to 4 cm wide, meandering a little as it runs (a falling
// rivulet's meander, wavelength 0.3 to 0.9 m) along the fall line (rain_surface_fall_line(): the wind leans it), with
// a thin sheet between. Fixed per face, as Kaneda et al. (1993) keep each runner on the bed of the glass's
// affinity. Returns the share of the face the water covers (0.4 between streaks, 1 in one); finer than the pixel
// (p_footprint, m; 0 = full detail, as the path tracer has) the streaks fade to their mean.
float rain_wall_streaks(vec3 p_world, vec3 p_normal, vec2 p_push, float p_footprint) {
	vec2 th = vec2(p_normal.z, -p_normal.x);
	float tl = length(th);
	if (tl < 1e-3) {
		return 0.4;
	}
	vec3 t = vec3(th.x, 0.0, th.y) / tl;
	vec3 f = rain_surface_fall_line(p_normal, p_push);
	float lean = clamp(dot(f, t) / max(-f.y, 0.2), -2.0, 2.0);
	float s = dot(p_world, t) + lean * p_world.y;
	// Parallel faces apart from each other get their own streaks (the plane's offset, per half metre).
	uint plane = uint(int(floor(dot(p_world, p_normal) * 2.0 + 0.5)) + 32768) * 2654435761u;
	const float cw = 0.045;
	float c = floor(s / cw);
	float a = 0.0;
	for (int j = -1; j <= 1; j++) {
		float cj = c + float(j);
		uint h = rain_ring_hash(uint(int(cj) + 1048576) ^ plane);
		float x0 = (cj + 0.2 + 0.6 * rain_ring_rand(h)) * cw;
		float width = mix(0.009, 0.039, rain_ring_rand(h + 1u));
		float strength = mix(0.35, 1.0, rain_ring_rand(h + 2u));
		float amp = mix(0.004, 0.023, rain_ring_rand(h + 3u));
		float lam = mix(0.31, 0.94, rain_ring_rand(h + 4u));
		float xc = x0 + amp * sin(6.2831853 * (p_world.y / lam + rain_ring_rand(h + 5u)));
		float d = (s - xc) / width;
		a = max(a, exp(-d * d) * strength);
	}
	a = mix(a, 0.55, smoothstep(0.012, 0.05, p_footprint));
	return 0.4 + 0.6 * a;
}

// The wetness of a face from the rain it catches (rain_surface_catch(), p_rain): on walls and steep faces the
// running water shows in streaks by the catch (wet right through from 4 mm/h, as the wall film's scale), a face
// out of the rain keeps a third of the ground's film (splash and damp air); level faces keep the ground's film
// (p_wet). p_running scales the running water (the material's wetness amount, less what a wiper took).
float rain_surface_wetness(float p_wet, float p_running, vec3 p_normal, vec4 p_rain, float p_streaks) {
	float wall = 1.0 - smoothstep(0.2, 0.7, p_normal.y);
	if (wall <= 0.0) {
		return p_wet;
	}
	float running = clamp(p_rain.x / 4.0, 0.0, 1.0) * p_running;
	return mix(p_wet, max(running * p_streaks, 0.35 * p_wet), wall);
}

// Water standing on a built surface (docs/ideas/engine-gaps-from-game.md, B; Lagarde 2012, "Water drop 2a", accumulated
// water): the weather's pooled water (WeatherMap3D's puddle, mm above what the surface film holds, filled by the rain
// and emptied by drainage and evaporation) stands on faces level enough to hold it. Paving laid to a 2 % fall still
// ponds in its hollows, so the hold eases from 20 degrees of slope to 8. Returns the water's depth over the face's
// mean (mm); the caller's relief (heightmap, AO) decides where it lies, filling the lowest first.
float rain_surface_puddle(vec3 p_normal, float p_puddle_mm) {
	return max(p_puddle_mm, 0.0) * smoothstep(0.94, 0.99, p_normal.y);
}

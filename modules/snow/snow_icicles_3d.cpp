/**************************************************************************/
/*  snow_icicles_3d.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "snow_icicles_3d.h"
#include "snow_cover_3d.h"

#include "core/templates/hashfuncs.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"

namespace {
// Ice and air near 0 C.
constexpr float ICE_DENSITY = 917.0f; // kg/m3
constexpr float LATENT_FUSION = 334000.0f; // J/kg
constexpr float AIR_CONDUCTIVITY = 0.024f; // W/(m K)
constexpr float AIR_VISCOSITY = 1.33e-5f; // m2/s, kinematic
constexpr float AIR_DIFFUSIVITY = 1.87e-5f; // m2/s, thermal
constexpr float PRANDTL = 0.71f;
constexpr float LATENT_VAPOUR = 2.5e6f; // J/kg
constexpr float AIR_HEAT = 1005.0f; // J/(kg K)
constexpr float AIR_PRESSURE = 101325.0f; // Pa
// Long-wave loss from ice at 0 C to air-temperature surroundings, linearised: 4 sigma T^3 (emissivity 1, as
// Makkonen 1988 takes it). Under a clear night sky it is more; not modelled.
constexpr float RADIATIVE_COEFFICIENT = 4.0f * 5.670e-8f * 273.15f * 273.15f * 273.15f; // W/(m2 K)
// The pendant drop at the tip: about 5 mm across (Makkonen 1988; Maeno et al. 1994, 2.44 mm radius).
constexpr float TIP_RADIUS = 0.0025f;
// The tip grows as spongy ice holding 26% liquid (Makkonen 1988).
constexpr float SPONGY_LIQUID = 0.26f;
// The share of the wall the running film keeps at 0 C and freezing: the rest of the ice is below 0 C and
// grows nothing. Our pick, 0.25, puts length growth at about 16 times diameter growth in still air at -10 C,
// the middle of the 8 to 32 Maeno et al. (1994) measured (0.5 gave the low end, and a week's icicles came out
// 1:4 cones where dripping icicles are slender, 1:10 and more); heat balance over the whole wall gives 4.
constexpr float WALL_WETTED = 0.25f;
// Ripples along icicles: about a centimetre apart whatever the icicle (Ueno 2003; Chen and Morris 2013).
constexpr float RIPPLE_WAVELENGTH = 0.01f;
// Ripples start flat and grow as the icicle does (Chen and Morris 2013 watched their amplitude grow over
// hours of growth): here none below 10 cm, full at half a metre, and smooth over the last 4 cm to the tip,
// the newest ice. Under path tracing even a slope of 0.01 swings what the ice refracts between the eave and
// the sky, so short icicles with ripples show bands.
constexpr float RIPPLE_SLOPE = 0.06f;
constexpr float RIPPLE_START_LENGTH = 0.1f;
constexpr float RIPPLE_FULL_LENGTH = 0.5f;
constexpr float RIPPLE_TIP_FADE = 0.04f;
constexpr int RIPPLE_COLUMNS = 64; // wide, so mipmaps (halving both ways) only blend near strengths
// The ripples in the mesh on grown icicles: a few millimetres at most in salty lab water, under the 0.1 mm the
// camera resolved in pure water (Chen and Morris 2013); roof melt makes ripply icicles like tap water's. In
// photographs they read as faint, slightly irregular banding in what the ice refracts, not grooves: 0.25 mm
// (slope 0.16) read as a threaded screw and sawtoothed the silhouette. Our pick, 0.03 mm full grown (slope 0.02,
// already a visible swing in refraction), each wave 0.5 to 1.5 times that and its spacing wandering by up to 15%,
// fading over the lower 40% toward the tip. The normal map stays smooth there. Near the outline, where the view
// grazes the ice, even slope 0.02 scalloped the edge of what it refracts into teeth, so the normals carry a
// third of the ripple's slope: faint bands across the face, smooth refraction edges.
constexpr float RIPPLE_AMPLITUDE = 0.00003f;
constexpr float RIPPLE_NORMAL_SHARE = 0.3f;
constexpr float RIPPLE_JITTER = 0.15f;
constexpr float RIPPLE_FADE_SHARE = 0.4f;
// The hazy core: pockets of impure water caught in layers as the icicle thickens (Ladan and Morris 2022) cloud
// the ice inside the newest clear layer; the tip, the newest ice, stays clear. The cloudy part is a thin central
// column of bubbles (a quarter of the radius): the clear ice around it is a cylindrical lens that widens it about
// 1.3 times to the eye, and at 0.4 the path tracer showed broad white blobs where photographs show a thin streak.
constexpr float CORE_RADIUS = 0.25f;
constexpr float CORE_REACH = 0.8f;
// The core stays a thin column however thick the icicle grows (the cloudy ice is the water caught along the axis,
// not a share of the wall): at most 3 mm across. Scaled with the 3/4 power profile, a thick icicle's core
// widened into a white wedge at the root.
constexpr float CORE_MAX_RADIUS = 0.0015f;
// The ideal icicle (Short, Baygents and Goldstein 2006): away from the tip its radius goes as the distance from
// the tip to the power 3/4, r = 4/3 a^(1/4) z^(3/4) (Chen and Morris 2013 found most lab icicles scatter round
// that exponent). Drawn so it holds the growth model's ice: a 3/4 power profile holds 0.4 R^2 L against a
// cone's R^2 L / 3, so its root is 0.913 of the cone's.
constexpr float PROFILE_POWER = 0.75f;
constexpr float PROFILE_ROOT = 0.913f;
constexpr float PROFILE_SCATTER = 0.08f; // each icicle's taper power within 3/4 +- this
// Drip points along an eave (see _place): mean spacing, the width each one wets (standard deviation along the
// edge) and the thin film that reaches the sites between them, as a share of a drip point's mean flow.
constexpr float DRIP_SPACING = 0.4f;
constexpr float DRIP_WIDTH = 0.03f;
constexpr float DRIP_FILM = 0.005f;
constexpr float DRIP_DRY = 0.6f; // the share of sites between drip points the film misses
constexpr float DRIP_HOURS = 8.0f; // hours of melt between a drip point's spell knots
// The axis is kept every 2 cm down from the root, as the tip lays it down.
constexpr float AXIS_STEP = 0.02f;
// The wind leans the pendant drop, and the tip grows under it (Maeno, Makkonen and Takahashi 1994, bent icicles):
// drag on the 2.44 mm drop over its weight, tan(lean) = 3 rho_air Cd U^2 / (8 rho_w r g) = 0.0102 U^2 (Cd 0.5).
// They measured leans well past that (the film runs down the lee side too): twice it here, at most 45 degrees.
constexpr float WIND_LEAN = 2.0f * 0.0102f;
constexpr float MAX_LEAN = 1.0f;
// The film often runs down one side, and an icicle grows bent toward it (Maeno et al. 1994; Chen and Morris
// 2013 saw lab icicles bend and twist): up to a slope of 0.08 per site, our pick.
constexpr float OWN_LEAN = 0.08f;

float buoyancy_rayleigh(float p_delta, float p_size) {
	// g beta dT L^3 / (nu alpha), beta = 1 / 273 K.
	return 9.81f / 273.15f * MAX(p_delta, 0.0f) * p_size * p_size * p_size / (AIR_VISCOSITY * AIR_DIFFUSIVITY);
}

// The tip: a sphere the drop's size. Forced, Ranz and Marshall (1952); free, Churchill (1983) for a sphere; the
// larger (as Makkonen 1988 takes the larger of the two).
float tip_coefficient(float p_delta, float p_wind) {
	const float d = 2.0f * TIP_RADIUS;
	const float re = MAX(p_wind, 0.0f) * d / AIR_VISCOSITY;
	const float forced = 2.0f + 0.6f * Math::sqrt(re) * Math::pow(PRANDTL, 1.0f / 3.0f);
	const float ra = buoyancy_rayleigh(p_delta, d);
	const float free = 2.0f + 0.589f * Math::pow(ra, 0.25f) / Math::pow(1.0f + Math::pow(0.469f / PRANDTL, 9.0f / 16.0f), 4.0f / 9.0f);
	return MAX(forced, free) * AIR_CONDUCTIVITY / d;
}

// The wall: a cylinder across the wind, Churchill and Bernstein (1977); free, a vertical surface as tall as the
// icicle, Churchill and Chu (1975); the larger.
float wall_coefficient(float p_delta, float p_wind, float p_radius, float p_length) {
	const float d = 2.0f * MAX(p_radius, TIP_RADIUS);
	const float re = MAX(p_wind, 0.0f) * d / AIR_VISCOSITY;
	const float forced = 0.3f + 0.62f * Math::sqrt(re) * Math::pow(PRANDTL, 1.0f / 3.0f) / Math::pow(1.0f + Math::pow(0.4f / PRANDTL, 2.0f / 3.0f), 0.25f) * Math::pow(1.0f + Math::pow(re / 282000.0f, 5.0f / 8.0f), 4.0f / 5.0f);
	const float l = MAX(p_length, d);
	const float ra = buoyancy_rayleigh(p_delta, l);
	const float nl = 0.825f + 0.387f * Math::pow(ra, 1.0f / 6.0f) / Math::pow(1.0f + Math::pow(0.492f / PRANDTL, 9.0f / 16.0f), 8.0f / 27.0f);
	return MAX(forced * AIR_CONDUCTIVITY / d, nl * nl * AIR_CONDUCTIVITY / l);
}

// The heat the surface at 0 C loses per unit of its convective coefficient h, as a temperature: the air's
// coldness plus evaporation from the wet ice into air at a relative humidity (Makkonen 1988, by the Lewis
// relation: 0.622 Le (e(0) - RH e(Ta)) / (cp p)); radiation is added with its own coefficient.
float vapour_pressure(float p_celsius) {
	// Over ice, Magnus form (Alduchov and Eskridge 1996), Pa.
	return 611.2f * Math::exp(22.46f * p_celsius / (272.62f + p_celsius));
}

float evaporative_kelvin(float p_celsius, float p_humidity) {
	const float de = 611.2f - CLAMP(p_humidity, 0.0f, 1.0f) * vapour_pressure(p_celsius);
	return 0.622f * LATENT_VAPOUR * MAX(de, 0.0f) / (AIR_HEAT * AIR_PRESSURE);
}

// Heat flux from the wet ice surface (W/m2) at coefficient h.
float surface_flux(float p_h, float p_celsius, float p_humidity) {
	const float delta = -p_celsius;
	return p_h * (delta + evaporative_kelvin(p_celsius, p_humidity)) + RADIATIVE_COEFFICIENT * delta;
}

uint32_t hash_u32(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
}
} // namespace

float SnowIcicles3D::tip_growth_rate(float p_celsius, float p_wind, float p_humidity) {
	// The heat the drop loses (convection, evaporation, radiation) over its lower half freezes a tube of spongy
	// ice as wide as the drop: dL/dt = 2 q / (rho_i Lf (1 - 0.26)).
	const float delta = -p_celsius;
	if (delta <= 0.0f) {
		return 0.0f;
	}
	return 2.0f * surface_flux(tip_coefficient(delta, p_wind), p_celsius, p_humidity) / (ICE_DENSITY * LATENT_FUSION * (1.0f - SPONGY_LIQUID)) * 3600.0f;
}

float SnowIcicles3D::wall_growth_rate(float p_celsius, float p_wind, float p_radius, float p_length, float p_humidity) {
	// The film over the wall freezes as fast as it loses heat, where it runs: dr/dt = 0.5 q / (rho_i Lf).
	const float delta = -p_celsius;
	if (delta <= 0.0f) {
		return 0.0f;
	}
	return WALL_WETTED * surface_flux(wall_coefficient(delta, p_wind, p_radius, p_length), p_celsius, p_humidity) / (ICE_DENSITY * LATENT_FUSION) * 3600.0f;
}

void SnowIcicles3D::_place() {
	// One site every spacing along the edge, jittered. Melt does not leave a roof as an even film: it drains
	// through the snowpack in flow fingers and runs down the roof in rivulets (Waldner, Schneebeli et al. 2004;
	// Marsh and Woo 1984), which reach the eave at drip points, and icicles hang where they do (Makkonen 1988
	// grows each icicle from its own water flux). So the drip points are scattered along the edge (on average
	// DRIP_SPACING apart, each with its own flow, lognormal), each wetting a few sites round it; a thin film
	// reaches the rest. A few sites grow long icicles in clusters (neighbours often fused), many only stubs, and
	// there are bare gaps between. Each icicle's taper scatters round the ideal 3/4 power too (Chen and Morris 2013).
	icicles.clear();
	sites_dirty = false;
	if (edge.size() < 2) {
		return;
	}
	uint32_t h = hash_u32(uint32_t(seed) * 2654435761u + 1u);
	auto rnd = [&]() {
		h = hash_u32(h + 0x9e3779b9u);
		return float(h & 0xffffffu) / float(0xffffff);
	};
	LocalVector<float> along; // each site's distance along the edge
	float total = 0.0f;
	for (int s = 0; s + 1 < edge.size(); s++) {
		const Vector3 a = edge[s];
		const Vector3 b = edge[s + 1];
		const float len = a.distance_to(b);
		const int count = MAX(int(Math::floor(len / spacing)), 1);
		for (int k = 0; k < count; k++) {
			const float t = CLAMP((k + 0.5f + 0.6f * (rnd() - 0.5f)) / count, 0.0f, 1.0f);
			Icicle ic;
			ic.position = a.lerp(b, t);
			const float a = float(Math::TAU) * rnd();
			ic.own_lean = Vector2(Math::cos(a), Math::sin(a)) * OWN_LEAN * rnd();
			ic.power = PROFILE_POWER + PROFILE_SCATTER * (2.0f * rnd() - 1.0f);
			icicles.push_back(ic);
			along.push_back(total + len * t);
		}
		total += len;
	}
	// The drip points: a Poisson scatter along the edge, each flow exp(N(0,1)) (lognormal), each with its own
	// wet and dry spells: rivulets wander as the snowpack above them changes, so a drip point runs for a day or
	// two of melt, dries, and another starts. Sites under one that ran all week grow long, others part way.
	drips.clear();
	melt_hours = 0.0f;
	for (float d = -DRIP_SPACING * Math::log(MAX(rnd(), 1e-6f)); d < total; d -= DRIP_SPACING * Math::log(MAX(rnd(), 1e-6f))) {
		const float g = Math::sqrt(-2.0f * Math::log(MAX(rnd(), 1e-6f))) * Math::cos(float(Math::TAU) * rnd());
		drips.push_back(Vector3(d, Math::exp(g), 100.0f * rnd()));
	}
	for (uint32_t i = 0; i < icicles.size(); i++) {
		// Between drip points most of the edge stays dry; where a little film does come over, a stub grows to
		// the length whose wall freezes all of it.
		const float r = rnd();
		icicles.write[i].film = r < DRIP_DRY ? 0.0f : DRIP_FILM * Math::pow((r - DRIP_DRY) / (1.0f - DRIP_DRY), 2.0f);
		icicles.write[i].along = along[i];
	}
	_update_shares();
}

void SnowIcicles3D::_update_shares() {
	// Each drip point's flow now: its spell noise (smooth, one knot every DRIP_HOURS of melt) above 0.35 runs.
	LocalVector<float> flow;
	flow.resize(drips.size());
	for (uint32_t d = 0; d < drips.size(); d++) {
		const float x = melt_hours / DRIP_HOURS + drips[d].z;
		const int i0 = int(Math::floor(x));
		const float t = x - i0;
		const float st = t * t * (3.0f - 2.0f * t);
		auto knot = [&](int p_i) { return float(hash_murmur3_one_32(uint32_t(p_i), uint32_t(d) * 7919u + uint32_t(seed)) & 0xffffu) / 65535.0f; };
		const float v = Math::lerp(knot(i0), knot(i0 + 1), st);
		flow[d] = drips[d].y * CLAMP((v - 0.35f) / 0.4f, 0.0f, 1.0f);
	}
	float sum = 0.0f;
	for (Icicle &ic : icicles) {
		float share = ic.film;
		for (uint32_t d = 0; d < drips.size(); d++) {
			const float x = (ic.along - drips[d].x) / DRIP_WIDTH;
			if (x * x < 18.0f) {
				share += flow[d] * Math::exp(-0.5f * x * x);
			}
		}
		ic.share = share;
		sum += share;
	}
	// Mean 1, so the edge's supply is shared out whole.
	const float scale = sum > 0.0f ? float(icicles.size()) / sum : 1.0f;
	for (Icicle &ic : icicles) {
		ic.share *= scale;
	}
}

void SnowIcicles3D::grow(float p_hours, float p_celsius, float p_wind, float p_supply, float p_humidity) {
	if (sites_dirty) {
		_place();
	}
	if (icicles.is_empty() || p_hours <= 0.0f) {
		return;
	}
	const float delta = -p_celsius;
	if (p_supply > 0.0f) {
		_update_shares();
		melt_hours += p_hours;
	}
	float supply_total = 0.0f;
	float dripped = 0.0f;
	float change = 0.0f;
	// Sub-steps of at most ten minutes: the rates change as the icicle grows.
	const int steps = MAX(int(Math::ceil(p_hours * 6.0f)), 1);
	const float dt = p_hours / steps; // h
	const Vector2 wind_dir = Vector2(wind_direction.x, wind_direction.z).length_squared() > 1e-6f ? Vector2(wind_direction.x, wind_direction.z).normalized() : Vector2();
	const float wind_lean = MIN(WIND_LEAN * p_wind * p_wind, MAX_LEAN);
	for (Icicle &ic : icicles) {
		const float water = MAX(p_supply, 0.0f) * spacing * ic.share; // kg/h to this icicle
		supply_total += water;
		const float l0 = ic.length;
		const float r0 = ic.radius;
		ic.dripping = false;
		for (int k = 0; k < steps; k++) {
			if (delta <= 0.0f) {
				// Melting: the air's heat (at the same coefficients) melts the wall and the tip back; an icicle
				// thinned to the drop's size at its root falls.
				if (ic.length > 0.0f) {
					const float hw = wall_coefficient(1.0f, p_wind, ic.radius, ic.length);
					const float ht = tip_coefficient(1.0f, p_wind);
					ic.radius -= hw * -delta / (ICE_DENSITY * LATENT_FUSION) * 3600.0f * dt;
					ic.length -= 2.0f * ht * -delta / (ICE_DENSITY * LATENT_FUSION) * 3600.0f * dt;
					if (ic.radius <= TIP_RADIUS || ic.length <= TIP_RADIUS) {
						ic.length = 0.0f;
						ic.radius = 0.0f;
					}
					ic.dripping = ic.length > 0.0f; // the melt runs off the tip
				}
				continue;
			}
			if (water <= 0.0f) {
				continue;
			}
			if (ic.length <= 0.0f) {
				// A drop hangs from the edge and starts to freeze.
				ic.length = TIP_RADIUS;
				ic.radius = TIP_RADIUS;
			}
			// What the wall can freeze over its cone (kg/h), and what the tip needs to lengthen at full rate.
			const float slant = Math::sqrt(ic.length * ic.length + ic.radius * ic.radius);
			const float area = float(Math::PI) * (ic.radius + TIP_RADIUS) * slant;
			const float wall_rate = wall_growth_rate(p_celsius, p_wind, ic.radius, ic.length, p_humidity); // m/h
			const float wall_mass = wall_rate * area * ICE_DENSITY; // kg/h
			const float tip_rate = ic.length < max_length ? tip_growth_rate(p_celsius, p_wind, p_humidity) : 0.0f;
			const float tip_mass = tip_rate * float(Math::PI) * TIP_RADIUS * TIP_RADIUS * ICE_DENSITY * (1.0f - SPONGY_LIQUID);
			if (water <= wall_mass) {
				// The wall freezes all the water: the icicle only thickens (Szilder and Lozowski 1994).
				ic.radius += water / MAX(area * ICE_DENSITY, 1e-9f) * dt;
			} else {
				ic.radius += wall_rate * dt;
				const float left = water - wall_mass;
				const float tip_share = MIN(left / MAX(tip_mass, 1e-9f), 1.0f);
				ic.length = MIN(ic.length + tip_rate * tip_share * dt, max_length);
				dripped += (left - tip_mass * tip_share) * dt / p_hours;
				ic.dripping = left > tip_mass * tip_share * 1.001f;
			}
		}
		// The axis: new length goes down under the drop, leaning with the wind and the film's side; melted
		// length takes its points with it.
		const int need = int(Math::floor(ic.length / AXIS_STEP)) + 1;
		if (ic.length <= 0.0f) {
			ic.bend.clear();
		} else if (ic.bend.size() > need) {
			ic.bend.resize(need);
		}
		if (ic.length > 0.0f && ic.bend.is_empty()) {
			ic.bend.push_back(Vector2());
		}
		while (ic.length > 0.0f && ic.bend.size() < need) {
			const Vector2 slope = wind_dir * wind_lean + ic.own_lean;
			ic.bend.push_back(ic.bend[ic.bend.size() - 1] + slope * AXIS_STEP);
		}
		change = MAX(change, MAX(Math::abs(ic.length - l0), 10.0f * Math::abs(ic.radius - r0)));
	}
	last_supply = supply_total;
	last_dripped = dripped;
	drawn_change += change;
	if (drawn_change > 0.002f) {
		drawn_change = 0.0f;
		_build_mesh();
	}
}

void SnowIcicles3D::advance(float p_hours) {
	if (sites_dirty) {
		_place();
	}
	if (icicles.is_empty() || !is_inside_tree()) {
		return;
	}
	const SnowCover3D *cover = SnowCover3D::get_active();
	const WorldWeather *weather = WorldContracts::weather()->has_provider() ? WorldContracts::weather() : nullptr;
	// The middle of the edge stands for all of it.
	Vector3 mid;
	for (const Vector3 &p : edge) {
		mid += p / float(edge.size());
	}
	const Vector3 at = get_global_transform().xform(mid);
	const float celsius = cover ? cover->get_air_temperature_at(at) : (weather ? weather->air_temperature_at(at) : 0.0f);
	const Vector3 wind_v = weather ? weather->wind_at(at) : Vector3();
	const float wind = wind_v.length();
	Vector3 wind_local = get_global_transform().basis.xform_inv(wind_v);
	wind_local.y = 0.0f;
	wind_direction = wind_local; // grow() takes its way, the speed comes as p_wind
	const float humidity = weather ? weather->air_humidity_at(at) : 0.8f;
	float supply = 0.0f; // kg per metre of edge per hour
	if (cover) {
		// Melt water leaving the pack above (mm/h = kg/(m2 h)) over the catchment, plus the roof's heat melting
		// it from below while there is snow on it.
		supply = cover->get_runoff_at(at) * catchment_length;
		if (heat_loss > 0.0f && cover->get_swe_at(at) > 0.1f) {
			supply += heat_loss / LATENT_FUSION * 3600.0f * catchment_length;
		}
	}
	grow(p_hours, celsius, wind, supply, humidity);
}

void SnowIcicles3D::clear() {
	for (Icicle &ic : icicles) {
		ic.length = 0.0f;
		ic.radius = 0.0f;
	}
	_build_mesh();
}

float SnowIcicles3D::get_icicle_length(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, icicles.size(), 0.0f);
	return icicles[p_index].length;
}

float SnowIcicles3D::get_icicle_radius(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, icicles.size(), 0.0f);
	return icicles[p_index].radius;
}

Vector3 SnowIcicles3D::get_icicle_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, icicles.size(), Vector3());
	return icicles[p_index].position;
}

float SnowIcicles3D::get_longest() const {
	float m = 0.0f;
	for (const Icicle &ic : icicles) {
		m = MAX(m, ic.length);
	}
	return m;
}

float SnowIcicles3D::get_ice_mass() const {
	float m = 0.0f;
	for (const Icicle &ic : icicles) {
		if (ic.length > 0.0f) {
			// A cone frustum from the root radius to the drop's.
			m += ICE_DENSITY * float(Math::PI) * ic.length / 3.0f * (ic.radius * ic.radius + ic.radius * TIP_RADIUS + TIP_RADIUS * TIP_RADIUS);
		}
	}
	return m;
}

static Vector2 _axis_offset(const Vector<Vector2> &p_bend, float p_depth) {
	if (p_bend.is_empty()) {
		return Vector2();
	}
	const float f = MAX(p_depth, 0.0f) / AXIS_STEP;
	const int i = int(Math::floor(f));
	if (i + 1 >= p_bend.size()) {
		// Past the last point (the tip's last bit, or the drop): straight on along the last step's slope.
		const int n = p_bend.size();
		const Vector2 slope = n >= 2 ? (p_bend[n - 1] - p_bend[n - 2]) / AXIS_STEP : Vector2();
		return p_bend[n - 1] + slope * (p_depth - (n - 1) * AXIS_STEP);
	}
	return p_bend[i].lerp(p_bend[i + 1], f - i);
}

Vector2 SnowIcicles3D::get_icicle_offset(int p_index, float p_depth) const {
	ERR_FAIL_INDEX_V(p_index, icicles.size(), Vector2());
	return _axis_offset(icicles[p_index].bend, p_depth);
}

bool SnowIcicles3D::is_icicle_dripping(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, icicles.size(), false);
	return icicles[p_index].dripping;
}

void SnowIcicles3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (default_material.is_null()) {
		// Clear ice: light passes through and bends (n 1.31), reflecting 1.8% head-on (specular 0.335), its
		// ripples a normal map along the icicle.
		default_material.instantiate();
		default_material->set_albedo(Color(0.97f, 0.99f, 1.0f));
		default_material->set_roughness(0.06f);
		default_material->set_specular(0.335f);
		default_material->set_feature(BaseMaterial3D::FEATURE_TRANSMISSION, true);
		default_material->set_transmission(1.0f);
		default_material->set_transmission_thin_walled(false);
		default_material->set_transmission_ior(1.31f);
		// A solid about 4 cm across: raster refraction bends the view again where it leaves the far side, as
		// the path tracer does, instead of carrying on into the scene behind.
		default_material->set_transmission_thickness(0.04f);
		// Clear ice absorbs little over a few centimetres (its blue needs metres), so no absorption.
		// One ripple down its height (UV.y), its strength across (UV.x): column 0 smooth, the last a slope of 0.06
		// (a ripple about a tenth of a millimetre high on its 1 cm wavelength). Steeper, path traced refraction
		// breaks the ice into black and white bands.
		const int w = RIPPLE_COLUMNS, h = 32;
		Ref<Image> img = Image::create_empty(w, h, false, Image::FORMAT_RGB8);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const float s = RIPPLE_SLOPE * float(x) / (w - 1) * Math::cos(float(Math::TAU) * (y + 0.5f) / h);
				const Vector3 nrm = Vector3(0.0f, s, 1.0f).normalized();
				img->set_pixel(x, y, Color(nrm.x * 0.5f + 0.5f, nrm.y * 0.5f + 0.5f, nrm.z * 0.5f + 0.5f));
			}
		}
		img->generate_mipmaps();
		default_material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
		default_material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, ImageTexture::create_from_image(img));
	}
	if (default_core_material.is_null()) {
		// Cloudy ice: the inclusions scatter, so it is white and rough, and lets light through to its back
		// (backlight) as a frosted pane does. Opaque, so the clear ice around it refracts it in raster too.
		// The clear ice is a cylindrical lens round it: a ray entering at b from the axis bends to
		// asin(b/(nR)) and so passes the axis at R sin(asin(b/(nR))) = b/n. A core of radius c is hit by
		// every ray with b < n c, so it shows n = 1.31 times its width, whatever the clear ice's radius (the
		// path tracer traces this). Raster's transmission reads the screen almost straight through the middle,
		// so there the core is drawn at that apparent width (UV: the clear ice's radius there, and the core's);
		// the path tracer and shadow maps keep the real column.
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(R"(shader_type spatial;
uniform vec3 albedo : source_color = vec3(0.72, 0.77, 0.82);
uniform vec3 backlight : source_color = vec3(0.6, 0.66, 0.72);
uniform float ior = 1.31;
void vertex() {
	float big_r = UV.x;
	float c = UV.y;
	vec2 o = NORMAL.xz;
	if (!IN_PATH_TRACER && !IN_SHADOW_PASS && c > 0.0 && big_r > c && dot(o, o) > 1.0e-8) {
		VERTEX.xz += normalize(o) * (min(ior * c, 0.95 * big_r) - c);
	}
}
void fragment() {
	ALBEDO = albedo;
	ROUGHNESS = 0.5;
	SPECULAR = 0.335;
	BACKLIGHT = backlight;
}
)");
		default_core_material.instantiate();
		default_core_material->set_shader(sh);
	}
}

void SnowIcicles3D::_build_mesh() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_children();
	// Each icicle a cone from its root radius to the drop at its tip (Szilder and Lozowski 1994: near
	// conical), rounded at the tip, ten sides; UV.y counts ripples down it, UV.x goes round.
	constexpr int SIDES = 10;
	struct Surface {
		Vector<Vector3> verts;
		Vector<Vector3> normals;
		Vector<float> tangents;
		Vector<Vector2> uvs;
		Vector<int> indices;
	};
	// p_scale shrinks the radius, p_reach the length: the hazy core is a thinner cone that stops short of the tip.
	// p_phase shifts the ripples so neighbours' bands do not line up.
	auto add_cone = [](Surface &p_out, const Icicle &p_ic, float p_scale, float p_reach, float p_phase, bool p_ripples) {
		const float length = p_ic.length * p_reach;
		const float root = p_ic.radius * PROFILE_ROOT * p_scale;
		const float tip = TIP_RADIUS * p_scale;
		const float ripple_grown = CLAMP((p_ic.length - RIPPLE_START_LENGTH) / (RIPPLE_FULL_LENGTH - RIPPLE_START_LENGTH), 0.0f, 1.0f);
		const bool ripples = p_ripples && ripple_grown > 0.0f;
		// Rippled: four rings a wavelength, evenly; smooth: rings closer together toward the tip, where the 3/4 power
		// profile turns fastest.
		const int rings = ripples ? CLAMP(int(Math::ceil(length / (0.25f * RIPPLE_WAVELENGTH))), 2, 600) : CLAMP(int(Math::ceil(length / 0.03f)), 2, 40);
		const int base = p_out.verts.size();
		for (int r = 0; r <= rings; r++) {
			const float q = float(r) / rings;
			const float z = ripples ? length * q : length * (1.0f - (1.0f - q) * (1.0f - q) * 0.5f - (1.0f - q) * 0.5f);
			// Its radius by the distance from the full icicle's tip (the core stops short, as before).
			const float u = MAX(p_ic.length - z, 0.0f) / MAX(p_ic.length, 1e-4f);
			float rad = MAX(root * Math::pow(u, p_ic.power), tip);
			if (!p_ripples && p_scale < 1.0f) {
				rad = MIN(rad, CORE_MAX_RADIUS); // the hazy core: a thin column
			}
			float slope = MIN(root * p_ic.power * Math::pow(MAX(u, 1e-3f), p_ic.power - 1.0f) / MAX(p_ic.length, 1e-4f), 3.0f);
			if (!p_ripples && p_scale < 1.0f && rad >= CORE_MAX_RADIUS) {
				slope = 0.0f; // where the cap holds, the core is a straight column: its sides face out, not down
			}
			if (ripples) {
				// Full grown on long icicles, fading toward the tip (the newer ice), none at the root's cap. The
				// spacing wanders (a smooth noise added to the phase) and each wave has its own depth.
				const float tip_fade = Math::smoothstep(0.0f, RIPPLE_FADE_SHARE * p_ic.length, p_ic.length - z);
				const float w = z / RIPPLE_WAVELENGTH;
				const uint32_t seed_i = uint32_t(p_phase * 65536.0f);
				auto hv = [&](int p_i) { return float(hash_murmur3_one_32(uint32_t(p_i), seed_i) & 0xffff) / 65535.0f; };
				auto smooth = [&](float p_x, int p_salt) {
					const int i0 = int(Math::floor(p_x));
					const float t = p_x - i0;
					const float st = t * t * (3.0f - 2.0f * t);
					return Math::lerp(hv(i0 * 2 + p_salt), hv((i0 + 1) * 2 + p_salt), st);
				};
				const float wander = RIPPLE_JITTER * (2.0f * smooth(0.5f * w, 0) - 1.0f);
				const float depth = 0.5f + smooth(w, 1);
				const float amp = RIPPLE_AMPLITUDE * depth * ripple_grown * tip_fade * CLAMP(z / RIPPLE_TIP_FADE, 0.0f, 1.0f);
				const float k = float(Math::TAU) / RIPPLE_WAVELENGTH;
				const float ph = float(Math::TAU) * (w + wander + p_phase);
				rad += amp * Math::sin(ph);
				slope -= RIPPLE_NORMAL_SHARE * amp * k * Math::cos(ph);
			}
			if (r == rings) {
				rad = 0.0f; // the rounded tip closes to a point below the drop
			}
			const float y = r == rings ? z + tip : z;
			const Vector2 off = _axis_offset(p_ic.bend, y);
			const Vector3 centre = p_ic.position + Vector3(off.x, -y, off.y);
			for (int s = 0; s <= SIDES; s++) {
				const float a = float(Math::TAU) * s / SIDES;
				const Vector3 out(Math::cos(a), 0.0f, Math::sin(a));
				p_out.verts.push_back(centre + out * rad);
				p_out.normals.push_back((out + Vector3(0.0f, -(rad > tip || ripples ? slope : 0.0f), 0.0f)).normalized());
				const Vector3 t(-Math::sin(a), 0.0f, Math::cos(a));
				p_out.tangents.push_back(t.x);
				p_out.tangents.push_back(t.y);
				p_out.tangents.push_back(t.z);
				p_out.tangents.push_back(1.0f);
				// The ripples are round the icicle, so UV.x picks their strength instead of going round.
				const float grown = ripples ? 0.0f : ripple_grown * CLAMP((p_ic.length - z) / RIPPLE_TIP_FADE, 0.0f, 1.0f);
				if (!p_ripples && p_scale < 1.0f) {
					// The core: the clear ice's radius here and its own, for raster's apparent width.
					p_out.uvs.push_back(Vector2(MAX(p_ic.radius * PROFILE_ROOT * Math::pow(u, p_ic.power), TIP_RADIUS), rad));
				} else {
					p_out.uvs.push_back(Vector2((grown * (RIPPLE_COLUMNS - 1) + 0.5f) / RIPPLE_COLUMNS, y / RIPPLE_WAVELENGTH + p_phase));
				}
			}
		}
		for (int r = 0; r < rings; r++) {
			for (int s = 0; s < SIDES; s++) {
				const int a0 = base + r * (SIDES + 1) + s;
				const int a1 = a0 + 1;
				const int b0 = a0 + SIDES + 1;
				const int b1 = b0 + 1;
				// Clockwise seen from outside (Godot's front faces).
				p_out.indices.push_back(a0);
				p_out.indices.push_back(b0);
				p_out.indices.push_back(a1);
				p_out.indices.push_back(a1);
				p_out.indices.push_back(b0);
				p_out.indices.push_back(b1);
			}
		}
		// The root's cap, inside the eave (closes the ice for refraction).
		const int c = p_out.verts.size();
		p_out.verts.push_back(p_ic.position);
		p_out.normals.push_back(Vector3(0, 1, 0));
		p_out.tangents.push_back(1.0f);
		p_out.tangents.push_back(0.0f);
		p_out.tangents.push_back(0.0f);
		p_out.tangents.push_back(1.0f);
		p_out.uvs.push_back(Vector2(0.5f / RIPPLE_COLUMNS, 0.0f));
		for (int s = 0; s < SIDES; s++) {
			p_out.indices.push_back(c);
			p_out.indices.push_back(base + s);
			p_out.indices.push_back(base + s + 1);
		}
	};
	// The pendant drop on a dripping tip (Makkonen 1988: the tip grows round it): a water sphere the drop's size,
	// hanging just under the ice's point. Water bends light as ice does (n 1.33 against 1.31): the ice's material.
	auto add_drop = [](Surface &p_out, const Icicle &p_ic) {
		constexpr int LAT = 6, LON = 10;
		const float y = p_ic.length + 0.6f * TIP_RADIUS;
		const Vector2 off = _axis_offset(p_ic.bend, y);
		const Vector3 centre = p_ic.position + Vector3(off.x, -y, off.y);
		const int base = p_out.verts.size();
		for (int la = 0; la <= LAT; la++) {
			const float th = float(Math::PI) * la / LAT;
			for (int lo = 0; lo <= LON; lo++) {
				const float ph = float(Math::TAU) * lo / LON;
				const Vector3 n(Math::sin(th) * Math::cos(ph), Math::cos(th), Math::sin(th) * Math::sin(ph));
				p_out.verts.push_back(centre + n * TIP_RADIUS);
				p_out.normals.push_back(n);
				const Vector3 t(-Math::sin(ph), 0.0f, Math::cos(ph));
				p_out.tangents.push_back(t.x);
				p_out.tangents.push_back(t.y);
				p_out.tangents.push_back(t.z);
				p_out.tangents.push_back(1.0f);
				p_out.uvs.push_back(Vector2(0.5f / RIPPLE_COLUMNS, 0.0f));
			}
		}
		for (int la = 0; la < LAT; la++) {
			for (int lo = 0; lo < LON; lo++) {
				const int a0 = base + la * (LON + 1) + lo;
				const int a1 = a0 + 1;
				const int b0 = a0 + LON + 1;
				const int b1 = b0 + 1;
				// Clockwise seen from outside, as the cone's rings (top ring first, going round the same way).
				p_out.indices.push_back(a0);
				p_out.indices.push_back(b0);
				p_out.indices.push_back(a1);
				p_out.indices.push_back(a1);
				p_out.indices.push_back(b0);
				p_out.indices.push_back(b1);
			}
		}
	};
	// Curtains: where neighbours hang closer than the thinner one's radius, the film running down one wall bridges
	// to the other and freezes across the gap, and they grow as one sheet down to where they part (fused icicle
	// fringes along eaves; Chen and Morris 2013 saw neighbours join). A web of ice 0.6 of the thinner icicle thick,
	// from one wall's surface to the other's: never inside either (the path tracer takes every front face it meets
	// as air into ice, so ice drawn inside ice bends light twice and shows white), thinning to nothing where the
	// gap opens past the bridge. Where the walls themselves touch, they are the join.
	auto add_curtain = [](Surface &p_out, const Icicle &p_a, const Icicle &p_b) {
		auto radius_at = [](const Icicle &p_ic, float p_z) {
			if (p_z >= p_ic.length) {
				return 0.0f;
			}
			return MAX(p_ic.radius * PROFILE_ROOT * Math::pow((p_ic.length - p_z) / p_ic.length, p_ic.power), TIP_RADIUS);
		};
		auto axis_at = [](const Icicle &p_ic, float p_z) {
			const Vector2 off = _axis_offset(p_ic.bend, p_z);
			return p_ic.position + Vector3(off.x, -p_z, off.y);
		};
		const float reach = MIN(p_a.length, p_b.length);
		float z_from = -1.0f, z_to = -1.0f;
		for (float z = 0.0f; z < reach; z += 0.005f) {
			const Vector3 a = axis_at(p_a, z), b = axis_at(p_b, z);
			const float ra = radius_at(p_a, z), rb = radius_at(p_b, z);
			const float free = Vector2(a.x - b.x, a.z - b.z).length() - ra - rb;
			if (free >= MIN(ra, rb)) {
				break;
			}
			if (free > 0.0f && z_from < 0.0f) {
				z_from = z;
			}
			if (z_from >= 0.0f) {
				z_to = z;
			}
		}
		if (z_from < 0.0f || z_to - z_from < 0.02f) {
			return;
		}
		const int steps = CLAMP(int(Math::ceil((z_to - z_from) / 0.01f)), 2, 120);
		LocalVector<Vector3> fa, fb, ba, bb; // front and back faces' edges at a and b
		Vector3 nrm;
		for (int k = 0; k <= steps; k++) {
			const float z = z_from + (z_to - z_from) * k / steps;
			const Vector3 a = axis_at(p_a, z), b = axis_at(p_b, z);
			const float ra = radius_at(p_a, z), rb = radius_at(p_b, z);
			Vector3 across = Vector3(b.x - a.x, 0.0f, b.z - a.z);
			across = across.length_squared() > 1e-8f ? across.normalized() : Vector3(1, 0, 0);
			nrm = Vector3(-across.z, 0.0f, across.x);
			// The web's ends on each wall, set in by the curve of the wall at its half thickness so its corners stay
			// outside the round ice.
			const float half = k == steps ? 0.0f : 0.3f * MIN(ra, rb);
			const float ea = Math::sqrt(MAX(ra * ra - half * half, 0.0f));
			const float eb = Math::sqrt(MAX(rb * rb - half * half, 0.0f));
			fa.push_back(a + across * ea + nrm * half);
			fb.push_back(b - across * eb + nrm * half);
			ba.push_back(a + across * ea - nrm * half);
			bb.push_back(b - across * eb - nrm * half);
		}
		auto quad = [&](const Vector3 &p0, const Vector3 &p1, const Vector3 &p2, const Vector3 &p3, const Vector3 &p_n) {
			// Wound to face p_n (clockwise front, as Godot's).
			const bool flip = Plane(p0, p1, p2).normal.dot(p_n) < 0.0f;
			const Vector3 v[4] = { p0, p1, p2, p3 };
			const int front[6] = { 0, 1, 2, 0, 2, 3 };
			const int back[6] = { 0, 2, 1, 0, 3, 2 };
			const int base = p_out.verts.size();
			for (int c = 0; c < 4; c++) {
				p_out.verts.push_back(v[c]);
				p_out.normals.push_back(p_n);
				p_out.tangents.push_back(0.0f);
				p_out.tangents.push_back(-1.0f);
				p_out.tangents.push_back(0.0f);
				p_out.tangents.push_back(1.0f);
				p_out.uvs.push_back(Vector2(0.5f / RIPPLE_COLUMNS, 0.0f));
			}
			for (int t : (flip ? back : front)) {
				p_out.indices.push_back(base + t);
			}
		};
		for (int k = 0; k < steps; k++) {
			quad(fa[k], fb[k], fb[k + 1], fa[k + 1], nrm);
			quad(ba[k], bb[k], bb[k + 1], ba[k + 1], -nrm);
		}
	};
	Surface ice;
	Surface core;
	int previous = -1;
	for (int i = 0; i < icicles.size(); i++) {
		const Icicle &ic = icicles[i];
		if (ic.length < 0.01f) {
			previous = -1;
			continue;
		}
		const float phase = float(hash_murmur3_one_32(uint32_t(i) + uint32_t(seed) * 7919u) & 0xffffu) / 65536.0f;
		add_cone(ice, ic, 1.0f, 1.0f, phase, true);
		if (hazy_core && ic.length > 0.05f) {
			add_cone(core, ic, CORE_RADIUS, CORE_REACH, phase, false);
		}
		if (ic.dripping) {
			add_drop(ice, ic);
		}
		if (curtains && previous >= 0) {
			add_curtain(ice, icicles[previous], ic);
		}
		previous = i;
	}
	Ref<ArrayMesh> mesh;
	for (int i = 0; i < 2; i++) {
		const Surface &sf = i == 0 ? ice : core;
		if (sf.indices.is_empty()) {
			continue;
		}
		if (mesh.is_null()) {
			mesh.instantiate();
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = sf.verts;
		arrays[Mesh::ARRAY_NORMAL] = sf.normals;
		arrays[Mesh::ARRAY_TANGENT] = sf.tangents;
		arrays[Mesh::ARRAY_TEX_UV] = sf.uvs;
		arrays[Mesh::ARRAY_INDEX] = sf.indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		Ref<Material> m = i == 0 ? (material.is_valid() ? material : Ref<Material>(default_material)) : (core_material.is_valid() ? core_material : Ref<Material>(default_core_material));
		mesh->surface_set_material(mesh->get_surface_count() - 1, m);
	}
	mesh_instance->set_mesh(mesh);
}

void SnowIcicles3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build_mesh();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// Grow with the snowpack's clock: each of its updates.
			const SnowCover3D *cover = SnowCover3D::get_active();
			if (cover && cover->get_update_count() != seen_update) {
				const bool first = seen_update == 0;
				seen_update = cover->get_update_count();
				if (!first) {
					advance(cover->get_last_step_hours());
				}
			}
		} break;
	}
}

void SnowIcicles3D::set_edge(const PackedVector3Array &p_edge) {
	edge = p_edge;
	sites_dirty = true;
	_place();
	_build_mesh();
}

void SnowIcicles3D::set_spacing(float p_spacing) {
	spacing = MAX(p_spacing, 0.02f);
	sites_dirty = true;
}

void SnowIcicles3D::set_catchment_length(float p_length) {
	catchment_length = MAX(p_length, 0.0f);
}

void SnowIcicles3D::set_heat_loss(float p_watts) {
	heat_loss = MAX(p_watts, 0.0f);
}

void SnowIcicles3D::set_max_length(float p_length) {
	max_length = MAX(p_length, 0.01f);
}

void SnowIcicles3D::set_seed(int p_seed) {
	seed = p_seed;
	sites_dirty = true;
}

void SnowIcicles3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_build_mesh();
}

void SnowIcicles3D::set_core_material(const Ref<Material> &p_material) {
	core_material = p_material;
	_build_mesh();
}

void SnowIcicles3D::set_hazy_core(bool p_enabled) {
	hazy_core = p_enabled;
	_build_mesh();
}

void SnowIcicles3D::set_wind_direction(const Vector3 &p_direction) {
	wind_direction = Vector3(p_direction.x, 0.0f, p_direction.z);
}

void SnowIcicles3D::set_curtains(bool p_enabled) {
	curtains = p_enabled;
	_build_mesh();
}

void SnowIcicles3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("grow", "hours", "celsius", "wind", "supply", "humidity"), &SnowIcicles3D::grow, DEFVAL(0.8f));
	ClassDB::bind_method(D_METHOD("advance", "hours"), &SnowIcicles3D::advance);
	ClassDB::bind_method(D_METHOD("clear"), &SnowIcicles3D::clear);
	ClassDB::bind_method(D_METHOD("get_icicle_count"), &SnowIcicles3D::get_icicle_count);
	ClassDB::bind_method(D_METHOD("get_icicle_length", "index"), &SnowIcicles3D::get_icicle_length);
	ClassDB::bind_method(D_METHOD("get_icicle_radius", "index"), &SnowIcicles3D::get_icicle_radius);
	ClassDB::bind_method(D_METHOD("get_icicle_position", "index"), &SnowIcicles3D::get_icicle_position);
	ClassDB::bind_method(D_METHOD("get_longest"), &SnowIcicles3D::get_longest);
	ClassDB::bind_method(D_METHOD("get_ice_mass"), &SnowIcicles3D::get_ice_mass);
	ClassDB::bind_method(D_METHOD("get_last_supply"), &SnowIcicles3D::get_last_supply);
	ClassDB::bind_method(D_METHOD("get_last_dripped"), &SnowIcicles3D::get_last_dripped);
	ClassDB::bind_static_method("SnowIcicles3D", D_METHOD("tip_growth_rate", "celsius", "wind", "humidity"), &SnowIcicles3D::tip_growth_rate, DEFVAL(0.8f));
	ClassDB::bind_static_method("SnowIcicles3D", D_METHOD("wall_growth_rate", "celsius", "wind", "radius", "length", "humidity"), &SnowIcicles3D::wall_growth_rate, DEFVAL(0.8f));
	ClassDB::bind_method(D_METHOD("set_edge", "edge"), &SnowIcicles3D::set_edge);
	ClassDB::bind_method(D_METHOD("get_edge"), &SnowIcicles3D::get_edge);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &SnowIcicles3D::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &SnowIcicles3D::get_spacing);
	ClassDB::bind_method(D_METHOD("set_catchment_length", "length"), &SnowIcicles3D::set_catchment_length);
	ClassDB::bind_method(D_METHOD("get_catchment_length"), &SnowIcicles3D::get_catchment_length);
	ClassDB::bind_method(D_METHOD("set_heat_loss", "watts"), &SnowIcicles3D::set_heat_loss);
	ClassDB::bind_method(D_METHOD("get_heat_loss"), &SnowIcicles3D::get_heat_loss);
	ClassDB::bind_method(D_METHOD("set_max_length", "length"), &SnowIcicles3D::set_max_length);
	ClassDB::bind_method(D_METHOD("get_max_length"), &SnowIcicles3D::get_max_length);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SnowIcicles3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &SnowIcicles3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SnowIcicles3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SnowIcicles3D::get_material);
	ClassDB::bind_method(D_METHOD("set_core_material", "material"), &SnowIcicles3D::set_core_material);
	ClassDB::bind_method(D_METHOD("get_core_material"), &SnowIcicles3D::get_core_material);
	ClassDB::bind_method(D_METHOD("set_hazy_core", "enabled"), &SnowIcicles3D::set_hazy_core);
	ClassDB::bind_method(D_METHOD("get_hazy_core"), &SnowIcicles3D::get_hazy_core);
	ClassDB::bind_method(D_METHOD("set_wind_direction", "direction"), &SnowIcicles3D::set_wind_direction);
	ClassDB::bind_method(D_METHOD("get_wind_direction"), &SnowIcicles3D::get_wind_direction);
	ClassDB::bind_method(D_METHOD("set_curtains", "enabled"), &SnowIcicles3D::set_curtains);
	ClassDB::bind_method(D_METHOD("get_curtains"), &SnowIcicles3D::get_curtains);
	ClassDB::bind_method(D_METHOD("get_icicle_offset", "index", "depth"), &SnowIcicles3D::get_icicle_offset);
	ClassDB::bind_method(D_METHOD("is_icicle_dripping", "index"), &SnowIcicles3D::is_icicle_dripping);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "edge"), "set_edge", "get_edge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.02,2,0.01,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "catchment_length", PROPERTY_HINT_RANGE, "0,50,0.1,suffix:m"), "set_catchment_length", "get_catchment_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heat_loss", PROPERTY_HINT_RANGE, "0,200,0.1,suffix:W/m²"), "set_heat_loss", "get_heat_loss");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_length", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:m"), "set_max_length", "get_max_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hazy_core"), "set_hazy_core", "get_hazy_core");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind_direction"), "set_wind_direction", "get_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "curtains"), "set_curtains", "get_curtains");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "core_material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_core_material", "get_core_material");
}

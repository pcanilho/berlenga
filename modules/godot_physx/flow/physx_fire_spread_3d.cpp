/**************************************************************************/
/*  physx_fire_spread_3d.cpp                                                */
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

#include "physx_fire_spread_3d.h"

#include "../nodes/physx_particle_fluid_3d.h"
#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/sort_array.h"
#include "scene/3d/decal.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_lightning_3d.h"
#endif
#include "modules/world/world_contracts.h"

// Fuel models from Anderson 1982 ("Aids to Determining Fuel Models for Estimating Fire Behavior", USDA INT-122): load,
// depth, surface-to-volume ratio and moisture of extinction as published; packing ratios from load / (depth * 513
// kg/m3) over Rothermel's optimum 3.348 sigma^-0.8189. r0 is the no-wind, no-slope spread before moisture damping,
// set so dry fuel (6%) spreads as BehavePlus gives for these models (about half a metre a minute in short grass);
// burn_time is WRF-SFIRE's fuel weight (seconds to burn down to 1/e).
const PhysXFireSpread3D::FuelModel &PhysXFireSpread3D::_model(FuelType p_type) {
	static const FuelModel models[FUEL_MAX] = {
		{ 0.166f, 0.305f, 3500.0f, 0.25f, 0.00106f, 0.12f, 0.03f, 7.0f }, // short grass
		{ 0.675f, 0.762f, 1500.0f, 0.21f, 0.00173f, 0.25f, 0.04f, 7.0f }, // tall grass
		{ 0.65f, 0.061f, 2500.0f, 3.8f, 0.0208f, 0.25f, 0.008f, 60.0f }, // hardwood litter
	};
	return models[CLAMP((int)p_type, 0, FUEL_MAX - 1)];
}

static constexpr float HEAT_CONTENT = 18.6e6f; // J/kg, Rothermel's 8,000 BTU/lb
static constexpr float FAR = 1.0e3f; // phi of unburnt ground far from any fire

// Value noise, 0..1, smooth between integer lattice points.
static float _fuel_noise(float p_x, float p_z, uint32_t p_seed) {
	const float fx = Math::floor(p_x), fz = Math::floor(p_z);
	auto h = [p_seed](int x, int z) {
		uint32_t v = (uint32_t(x) * 73856093u) ^ (uint32_t(z) * 19349663u) ^ (p_seed * 83492791u);
		v = (v ^ (v >> 13)) * 0x5bd1e995u;
		return float((v ^ (v >> 15)) & 0xffffu) / 65535.0f;
	};
	const int x0 = int(fx), z0 = int(fz);
	float tx = p_x - fx, tz = p_z - fz;
	tx = tx * tx * (3.0f - 2.0f * tx);
	tz = tz * tz * (3.0f - 2.0f * tz);
	const float a = Math::lerp(h(x0, z0), h(x0 + 1, z0), tx);
	const float b = Math::lerp(h(x0, z0 + 1), h(x0 + 1, z0 + 1), tx);
	return Math::lerp(a, b, tz);
}

void PhysXFireSpread3D::_allocate() {
	nx = MAX(2, (int)Math::ceil(size.x / cell_size));
	nz = MAX(2, (int)Math::ceil(size.y / cell_size));
	const uint32_t n = nx * nz;
	phi.resize(n);
	phi_next.resize(n);
	t_ignite.resize(n);
	load.resize(n);
	height.resize(n);
	fuel_left.resize(n);
	heat.resize(n);
	wet.resize(n);
	rain_water.resize(n);
	quenched.resize(n);
	steamed.resize(n);
	ash_blown.resize(n);
	char_time.resize(n);
	quenched_by_water = 0;
	base_moisture = fuel_moisture;
	rate.resize(n);
	peak_flame.resize(n);
	peak_scorch.resize(n);
	crown_char.resize(n);
	crown_glow.resize(n);
	heavy.resize(n);
	heavy_d.resize(n);
	heavy_left.resize(n);
	heavy_t.resize(n);
	heavy_any = false;
	const FuelModel &m = _model(fuel_type);
	Node3D *veg = vegetation_field.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(vegetation_field));
	const bool veg_paths = veg && veg->has_method("get_path_passes_at");
	const bool veg_litter = veg && veg->has_method("get_litter_at");
	const bool veg_wood = veg && veg->has_method("get_deadwood_at") && int(veg->call("get_deadwood_count")) > 0;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const uint32_t i = z * nx + x;
			phi[i] = FAR;
			t_ignite[i] = -1.0f;
			float l = m.load * fuel_load_scale;
			float extra_wet = 0.0f;
			if (fuel_map.is_valid() && !fuel_map->is_empty()) {
				const int px = CLAMP(int((x + 0.5f) / nx * fuel_map->get_width()), 0, fuel_map->get_width() - 1);
				const int pz = CLAMP(int((z + 0.5f) / nz * fuel_map->get_height()), 0, fuel_map->get_height() - 1);
				const Color c = fuel_map->get_pixel(px, pz);
				l *= c.r;
				extra_wet = c.g * 0.5f;
			}
			// Real fuel beds are patchy (Finney 2003; the clumped grass and bare gaps FARSITE's grids miss): the load
			// varies at a few metres and under a metre, bare gaps break it, and damper hollows hold more moisture, so
			// the front runs unevenly, leaves unburnt islands and a ragged edge. Fixed in world space, so the same
			// ground burns the same way.
			const Vector3 wp = _cell_world(x, z);
			float wet_mul = 1.0f;
			float rate_mul = 1.0f;
			if (fuel_patchiness > 0.0f) {
				const float coarse = _fuel_noise(wp.x / 3.0f, wp.z / 3.0f, 1u);
				const float fine = _fuel_noise(wp.x / 0.7f, wp.z / 0.7f, 2u);
				const float n = 0.65f * coarse + 0.35f * fine;
				l *= MAX(1.0f + fuel_patchiness * 1.4f * (n - 0.5f), 0.0f) * (1.0f - fuel_patchiness * (1.0f - Math::smoothstep(0.2f, 0.32f, n)));
				const float damp = _fuel_noise(wp.x / 4.5f + 17.3f, wp.z / 4.5f + 5.1f, 3u);
				wet_mul = 1.0f + fuel_patchiness * 1.4f * (damp - 0.5f);
				// Bed structure the load alone does not carry (height, packing, tussocks): the front runs faster
				// through some patches than others at metre scale, and fingers ahead where it can.
				const float mid = _fuel_noise(wp.x / 1.3f + 3.7f, wp.z / 1.3f + 9.2f, 4u);
				rate_mul = MAX(1.0f + fuel_patchiness * 1.6f * (0.5f * coarse + 0.5f * mid - 0.5f), 0.15f);
			}
			// Worn paths are bare (cover falls by half at about 500 passes a year: Cole and Bayfield 1993); leaf
			// litter adds a slow fuel under the grass.
			if (veg_paths) {
				l *= 1.0f / (1.0f + float(veg->call("get_path_passes_at", wp)) / 500.0f);
			}
			if (veg_litter) {
				l += float(veg->call("get_litter_at", wp)) * 0.4f * fuel_load_scale;
			}
			load[i] = l;
			// Logs and fallen branches lying here: a log of radius r crossing the cell puts its wood, about
			// 500 kg/m3 * pi r^2 per metre, over its 2r width, or over the cell's width when it is thinner.
			heavy[i] = 0.0f;
			heavy_d[i] = 0.0f;
			heavy_left[i] = 1.0f;
			heavy_t[i] = -1.0f;
			if (veg_wood) {
				const Dictionary d = veg->call("get_deadwood_at", wp, 0.5f * cell_size, 0.0f);
				if (!d.is_empty() && float(d.get("side", 1.0f)) + float(d.get("radius", 0.0f)) < 0.5f * cell_size) {
					const float r = d.get("radius", 0.0f);
					if (r >= 0.01f) {
						heavy[i] = 500.0f * Math::PI * r * r / MAX(2.0f * r, cell_size);
						heavy_d[i] = 2.0f * r;
						heavy_any = true;
					}
				}
			}
			rate[i] = rate_mul;
			rain_water[i] = 0.0f;
			quenched[i] = 0;
			steamed[i] = 0;
			ash_blown[i] = 0.0f;
			char_time[i] = 0.0f;
			wet[i] = MAX(wet_mul, 0.0f) + (fuel_moisture > 1.0e-4f ? extra_wet / fuel_moisture : extra_wet * 100.0f);
			height[i] = get_global_position().y;
			fuel_left[i] = 1.0f;
			heat[i] = 0.0f;
			peak_flame[i] = 0.0f;
			peak_scorch[i] = 0.0f;
			crown_char[i] = 0.0f;
			crown_glow[i] = 0.0f;
		}
	}
	band_x0 = 0;
	band_x1 = -1;
	band_z0 = 0;
	band_z1 = -1;
	total_burnt = 0.0f;
	heights_ready = false;
	grid_dirty = false;
	char_image = Image::create_empty(nx, nz, false, Image::FORMAT_RGBA8);
	char_image->fill(Color(0, 0, 0, 0));
	glow_image = Image::create_empty(nx, nz, false, Image::FORMAT_RGBA8);
	glow_image->fill(Color(0, 0, 0, 1));
	char_texture = ImageTexture::create_from_image(char_image);
	glow_texture = ImageTexture::create_from_image(glow_image);
	state_image = Image::create_empty(nx, nz, false, Image::FORMAT_RGBA8);
	state_image->fill(Color(0, 0, 0, 0));
	state_texture = ImageTexture::create_from_image(state_image);
	if (decal) {
		decal->set_texture(Decal::TEXTURE_ALBEDO, char_texture);
		decal->set_texture(Decal::TEXTURE_EMISSION, glow_texture);
	}
	_gather_trees();
}

Vector3 PhysXFireSpread3D::_cell_world(int p_x, int p_z) const {
	const Vector3 local((p_x + 0.5f) * cell_size - size.x * 0.5f, 0.0f, (p_z + 0.5f) * cell_size - size.y * 0.5f);
	Vector3 w = get_global_transform().xform(local);
	if (!height.is_empty()) {
		w.y = height[p_z * nx + p_x];
	}
	return w;
}

// A steady 0..1 hash of two integers.
static float _hash01(uint32_t p_a, uint32_t p_b) {
	uint32_t h = p_a * 73856093u ^ p_b * 19349663u;
	h = (h ^ (h >> 13)) * 0x5bd1e995u;
	return float((h ^ (h >> 15)) & 0xffffu) / 65535.0f;
}

bool PhysXFireSpread3D::_world_cell(const Vector3 &p_world, int &r_x, int &r_z) const {
	const Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	r_x = (int)Math::floor((local.x + size.x * 0.5f) / cell_size);
	r_z = (int)Math::floor((local.z + size.y * 0.5f) / cell_size);
	return r_x >= 0 && r_z >= 0 && r_x < nx && r_z < nz;
}

// The ground under each cell: a ray down through static bodies (terrain, rocks), once.
void PhysXFireSpread3D::_read_heights() {
	if (heights_ready || !is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	PhysicsDirectSpaceState3D *space = PhysicsServer3D::get_singleton()->space_get_direct_state(get_world_3d()->get_space());
	if (!space) {
		return;
	}
	const float base = get_global_position().y;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			Vector3 p = get_global_transform().xform(Vector3((x + 0.5f) * cell_size - size.x * 0.5f, 0.0f, (z + 0.5f) * cell_size - size.y * 0.5f));
			PhysicsDirectSpaceState3D::RayParameters ray;
			ray.from = Vector3(p.x, base + 50.0f, p.z);
			ray.to = Vector3(p.x, base - 50.0f, p.z);
			ray.collide_with_areas = false;
			PhysicsDirectSpaceState3D::RayResult hit;
			height[z * nx + x] = space->intersect_ray(ray, hit) ? hit.position.y : base;
		}
	}
	heights_ready = true;
}

void PhysXFireSpread3D::_grow_band(int p_x0, int p_z0, int p_x1, int p_z1) {
	if (band_x1 < band_x0) {
		band_x0 = p_x0;
		band_z0 = p_z0;
		band_x1 = p_x1;
		band_z1 = p_z1;
	} else {
		band_x0 = MIN(band_x0, p_x0);
		band_z0 = MIN(band_z0, p_z0);
		band_x1 = MAX(band_x1, p_x1);
		band_z1 = MAX(band_z1, p_z1);
	}
	band_x0 = CLAMP(band_x0, 0, nx - 1);
	band_z0 = CLAMP(band_z0, 0, nz - 1);
	band_x1 = CLAMP(band_x1, 0, nx - 1);
	band_z1 = CLAMP(band_z1, 0, nz - 1);
}

// Keeps the unburnt side a distance to the front, so the upwind step moves it at the spread rate: without this, cells
// that start at FAR relax towards the front (about 9 cells' time per cell) instead of being crossed by it. Fast
// sweeping (Zhao 2005, "A fast sweeping method for Eikonal equations") over the band and a margin, unburnt cells only;
// the update only lowers phi, which is all a spreading fire does.
void PhysXFireSpread3D::_redistance() {
	if (band_x1 < band_x0) {
		return;
	}
	const float h = cell_size;
	const int x0 = MAX(band_x0 - 4, 0), x1 = MIN(band_x1 + 4, nx - 1);
	const int z0 = MAX(band_z0 - 4, 0), z1 = MIN(band_z1 + 4, nz - 1);
	for (int sweep = 0; sweep < 4; sweep++) {
		const int sx = (sweep & 1) ? -1 : 1, sz = (sweep & 2) ? -1 : 1;
		for (int zi = 0; zi <= z1 - z0; zi++) {
			const int z = sz > 0 ? z0 + zi : z1 - zi;
			for (int xi = 0; xi <= x1 - x0; xi++) {
				const int x = sx > 0 ? x0 + xi : x1 - xi;
				const uint32_t i = z * nx + x;
				if (phi[i] <= 0.0f) {
					continue;
				}
				const float a = MAX(MIN(phi[z * nx + MAX(x - 1, 0)], phi[z * nx + MIN(x + 1, nx - 1)]), -h);
				const float b = MAX(MIN(phi[MAX(z - 1, 0) * nx + x], phi[MIN(z + 1, nz - 1) * nx + x]), -h);
				float u;
				if (Math::abs(a - b) >= h) {
					u = MIN(a, b) + h;
				} else {
					u = 0.5f * (a + b + Math::sqrt(2.0f * h * h - (a - b) * (a - b)));
				}
				if (u < phi[i]) {
					phi[i] = MAX(u, 1.0e-4f); // the step, not the sweep, decides when a cell catches
				}
			}
		}
	}
}

// Lightning fires (docs/ideas/world-links.md, link 4). Fuels are lit by the long continuing current that follows
// some return strokes, not by the strokes themselves (Fuquay et al. 1972: continuing currents over about 40 ms;
// about a third of ground flashes carry one), and only fuel dry enough takes: the chance falls with the fuel's
// moisture towards its moisture of extinction (Latham and Schlieter 1989's ignition trials; Anderson 2002's
// lightning-fire ignition model). A strike on a tree lights the litter at its foot as the current runs down
// the trunk into it.
float PhysXFireSpread3D::lightning_ignition_chance(float p_continuing, float p_moisture, float p_extinction) {
	if (p_continuing < 0.04f || p_extinction <= 0.0f) {
		return 0.0f;
	}
	const float dry = CLAMP(1.0f - p_moisture / p_extinction, 0.0f, 1.0f);
	return Math::smoothstep(0.04f, 0.2f, p_continuing) * dry * dry;
}

void PhysXFireSpread3D::_ignite_from_lightning() {
#ifdef MODULE_WEATHER_ENABLED
	if (!lightning_ignition) {
		lightning_seen = WeatherLightning3D::get_last_ground_strike_serial();
		return;
	}
	LocalVector<WeatherLightning3D::GroundStrike> strikes;
	WeatherLightning3D::get_ground_strikes_after(lightning_seen, strikes);
	lightning_seen = WeatherLightning3D::get_last_ground_strike_serial();
	const FuelModel &m = _model(fuel_type);
	for (const WeatherLightning3D::GroundStrike &s : strikes) {
		int x, z;
		if (!_world_cell(s.position, x, z)) {
			continue;
		}
		const uint32_t i = z * nx + x;
		if (load.is_empty() || load[i] <= 1.0e-4f || fuel_left[i] <= 0.0f) {
			continue;
		}
		const float p = lightning_ignition_chance(s.continuing_current, _moisture(i), m.moisture_extinction);
		// A draw from the strike's serial, so a replay lights the same fires.
		const uint32_t h = hash_murmur3_one_64(s.serial ^ (uint64_t(get_instance_id()) << 1));
		if (float(h & 0xffffff) / float(0x1000000) < p) {
			ignite(s.position, cell_size);
			lightning_ignitions++;
		}
	}
#endif
}

void PhysXFireSpread3D::ignite(const Vector3 &p_position, float p_radius) {
	if (grid_dirty) {
		_allocate();
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_position);
	const float r = MAX(p_radius, cell_size * 0.5f);
	const int cx0 = (int)Math::floor((local.x - r + size.x * 0.5f) / cell_size) - 2;
	const int cx1 = (int)Math::floor((local.x + r + size.x * 0.5f) / cell_size) + 2;
	const int cz0 = (int)Math::floor((local.z - r + size.y * 0.5f) / cell_size) - 2;
	const int cz1 = (int)Math::floor((local.z + r + size.y * 0.5f) / cell_size) + 2;
	if (cx1 < 0 || cz1 < 0 || cx0 >= nx || cz0 >= nz) {
		return;
	}
	for (int z = MAX(cz0, 0); z <= MIN(cz1, nz - 1); z++) {
		for (int x = MAX(cx0, 0); x <= MIN(cx1, nx - 1); x++) {
			const float dx = (x + 0.5f) * cell_size - size.x * 0.5f - local.x;
			const float dz = (z + 0.5f) * cell_size - size.y * 0.5f - local.z;
			const uint32_t i = z * nx + x;
			// Distance to the circle: a signed distance near the new fire, so the front starts round.
			phi[i] = MIN(phi[i], Math::sqrt(dx * dx + dz * dz) - r);
		}
	}
	_grow_band(cx0 - 2, cz0 - 2, cx1 + 2, cz1 + 2);
}

void PhysXFireSpread3D::extinguish() {
	for (uint32_t i = 0; i < phi.size(); i++) {
		if (t_ignite[i] >= 0.0f) {
			fuel_left[i] = 0.0f; // what burnt stays charred
		} else {
			phi[i] = FAR;
		}
	}
}

Vector3 PhysXFireSpread3D::_wind() const {
	if (use_weather_wind) {
		if (const WorldWeather *wm = WorldContracts::weather(); wm->has_provider()) {
			// Midflame height: about the fuel's depth; WRF-SFIRE takes the wind at 6.1 m and reduces it, a weather map
			// gives it near the ground directly.
			return wm->wind_at(get_global_position(), 1.0f);
		}
	}
	return wind;
}

// Rothermel's rate of spread along a front normal (m/s): R0 * eta_M * (1 + phi_w + phi_s), as WRF-SFIRE takes it.
float PhysXFireSpread3D::_spread_rate(int p_x, int p_z, const Vector2 &p_normal, const Vector2 &p_wind) const {
	const uint32_t i = p_z * nx + p_x;
	if (load[i] <= 1.0e-4f) {
		return 0.0f;
	}
	const FuelModel &m = _model(fuel_type);
	const float r = MIN(_moisture(i) / m.moisture_extinction, 1.0f);
	const float eta_m = MAX(1.0f - 2.59f * r + 5.11f * r * r - 3.52f * r * r * r, 0.0f);
	if (eta_m <= 0.0f) {
		return 0.0f;
	}
	// Rothermel's wind and slope factors as vectors, summed into one effective wind (Rothermel 1972; Finney 1998,
	// FARSITE), which sets the head-fire rate.
	const float c = 7.47f * Math::exp(-0.133f * Math::pow(m.sigma, 0.55f));
	const float b = 0.02526f * Math::pow(m.sigma, 0.54f);
	const float e = 0.715f * Math::exp(-3.59e-4f * m.sigma);
	const float wind_coef = c * Math::pow(m.beta_ratio, -e);
	const float u = p_wind.length() * 196.85f; // ft/min
	const Vector2 phi_w = u > 1.0e-3f ? p_wind / p_wind.length() * MIN(wind_coef * Math::pow(u, b), 200.0f) : Vector2();
	const int xa = CLAMP(p_x - 1, 0, nx - 1), xb = CLAMP(p_x + 1, 0, nx - 1);
	const int za = CLAMP(p_z - 1, 0, nz - 1), zb = CLAMP(p_z + 1, 0, nz - 1);
	const Vector2 grad((height[p_z * nx + xb] - height[p_z * nx + xa]) / (MAX(xb - xa, 1) * cell_size), (height[zb * nx + p_x] - height[za * nx + p_x]) / (MAX(zb - za, 1) * cell_size));
	const float tan_s = grad.length();
	const Vector2 phi_s = tan_s > 1.0e-4f ? grad / tan_s * (5.275f * Math::pow(m.beta, -0.3f) * tan_s * tan_s) : Vector2();
	const Vector2 phi_e = phi_w + phi_s;
	const float phi_len = phi_e.length();
	// Sparse fuel spreads poorly (the flame has to bridge the gaps: fuel continuity, Finney 2003), so thin patches
	// hold the front back and it runs ragged.
	const float nominal = m.load * fuel_load_scale;
	const float cont = nominal > 1.0e-6f ? CLAMP(load[i] / nominal, 0.0f, 1.0f) : 1.0f;
	const float head = m.r0 * eta_m * (1.0f + phi_len) * spread_scale * Math::sqrt(cont) * rate[i];
	if (phi_len < 1.0e-4f) {
		return head; // no wind or slope: a circle
	}
	// Elliptical fire shape (Anderson 1983 length-to-width from the effective midflame wind in mph; FARSITE's
	// head/back split). A level set moves each normal at the support function of that ellipse (Richards 1990, as CSIRO
	// Spark does): c cos t + sqrt(a^2 cos^2 t + b^2 sin^2 t), with a the semi-major axis, b = a / lw and c the
	// offset of its centre from the ignition point.
	const float u_eff = Math::pow(phi_len / MAX(wind_coef, 1.0e-9f), 1.0f / b) / 88.0f; // ft/min -> mph
	// Capped at 8, as FARSITE does: the fit grows without bound in a strong wind (about 90 at 8 m/s), which would stall
	// the flanks, where real fires in strong winds keep ratios under about 8 (Alexander 1985).
	const float lw = CLAMP(0.936f * Math::exp(0.2566f * u_eff) + 0.461f * Math::exp(-0.1548f * u_eff) - 0.397f, 1.0f, 8.0f);
	const float ecc = Math::sqrt(MAX(1.0f - 1.0f / (lw * lw), 0.0f));
	const float back = head * (1.0f - ecc) / (1.0f + ecc);
	const float semi_a = 0.5f * (head + back);
	const float semi_b = semi_a / lw;
	const float off_c = 0.5f * (head - back);
	const float cos_t = p_normal.dot(phi_e / phi_len);
	const float sin2 = MAX(1.0f - cos_t * cos_t, 0.0f);
	return off_c * cos_t + Math::sqrt(semi_a * semi_a * cos_t * cos_t + semi_b * semi_b * sin2);
}

void PhysXFireSpread3D::_step(float p_delta) {
	if (grid_dirty) {
		_allocate();
	}
	_read_heights();
	if (!enabled || band_x1 < band_x0) {
		return;
	}
	const Vector3 w3 = _wind();
	// Into the grid's frame.
	const Vector3 wl = get_global_transform().basis.inverse().xform(w3);
	const Vector2 w2(wl.x, wl.z);
	const FuelModel &m = _model(fuel_type);

	// The front: phi_t + R(n) |grad phi| = 0, first-order upwind (Osher and Sethian 1988) over the band, in substeps
	// that keep the front within a cell per step.
	float remaining = p_delta;
	const float h = cell_size;
	int guard = 0;
	while (remaining > 1.0e-6f && guard++ < 16) {
		// Fastest spread in the band bounds the substep.
		const float r_max = m.r0 * (1.0f + 200.0f) * spread_scale * (1.0f + 1.5f * fuel_patchiness);
		const float dt = MIN(remaining, MAX(0.5f * h / MAX(r_max, 1.0e-4f), p_delta / 16.0f));
		remaining -= dt;
		const int x0 = MAX(band_x0 - 1, 0), x1 = MIN(band_x1 + 1, nx - 1);
		const int z0 = MAX(band_z0 - 1, 0), z1 = MIN(band_z1 + 1, nz - 1);
		int nbx0 = nx, nbx1 = -1, nbz0 = nz, nbz1 = -1;
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const uint32_t i = z * nx + x;
				const float p = phi[i];
				phi_next[i] = p;
				if (p < -3.0f * h) {
					continue; // well inside the burnt area
				}
				const float pxm = phi[z * nx + MAX(x - 1, 0)], pxp = phi[z * nx + MIN(x + 1, nx - 1)];
				const float pzm = phi[MAX(z - 1, 0) * nx + x], pzp = phi[MIN(z + 1, nz - 1) * nx + x];
				if (p > 3.0f * h && pxm > 3.0f * h && pxp > 3.0f * h && pzm > 3.0f * h && pzp > 3.0f * h) {
					continue; // far from the front
				}
				const float dxm = (p - pxm) / h, dxp = (pxp - p) / h;
				const float dzm = (p - pzm) / h, dzp = (pzp - p) / h;
				// Upwind gradient for a front moving outward (speed >= 0).
				const float gx = MAX(dxm, 0.0f) > -MIN(dxp, 0.0f) ? MAX(dxm, 0.0f) : MIN(dxp, 0.0f);
				const float gz = MAX(dzm, 0.0f) > -MIN(dzp, 0.0f) ? MAX(dzm, 0.0f) : MIN(dzp, 0.0f);
				const float gl = Math::sqrt(gx * gx + gz * gz);
				if (gl < 1.0e-6f) {
					continue;
				}
				// The outward normal from central differences (smoother than the upwind one).
				Vector2 n((pxp - pxm), (pzp - pzm));
				const float nl = n.length();
				n = nl > 1.0e-6f ? n / nl : Vector2(gx, gz) / gl;
				Vector2 wc = w2;
				if (!pull.is_empty()) {
					// Drawn toward fire ahead of this front (not its own plume: Rothermel has that).
					const Vector2 pl = pull[(z / pull_tile) * pull_tx + x / pull_tile];
					if (pl.dot(n) > 0.0f) {
						wc += pl;
					}
				}
				const float r = _spread_rate(x, z, n, wc);
				phi_next[i] = p - dt * r * gl;
				if (phi_next[i] < 3.0f * h) {
					nbx0 = MIN(nbx0, x);
					nbx1 = MAX(nbx1, x);
					nbz0 = MIN(nbz0, z);
					nbz1 = MAX(nbz1, z);
				}
			}
		}
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const uint32_t i = z * nx + x;
				phi[i] = phi_next[i];
				if (phi[i] <= 0.0f && t_ignite[i] < 0.0f && load[i] > 1.0e-4f) {
					t_ignite[i] = time + p_delta - remaining;
				}
			}
		}
		if (nbx1 >= nbx0) {
			_grow_band(nbx0 - 2, nbz0 - 2, nbx1 + 2, nbz1 + 2);
		}
		_redistance();
	}
	time += p_delta;

	// Fuel burns down after ignition (WRF-SFIRE): left = exp(-(t - t_ig) / T_f); the heat it gives off, W/m2.
	total_burnt = 0.0f;
	const float cell_area = cell_size * cell_size;
	// Glowing firebrands big enough to light fuel, per kg burnt. Burning trees and shrubs throw hundreds of hot brands
	// per kg (Caton-Kerr et al. 2021, Frontiers in Mech. Eng. 7), most of them needles and leaves too small to light
	// anything; grass throws fewer that last (flakes of stem and seed head), litter more (bark and leaf), wood the most.
	static const float brands_per_kg[FUEL_MAX] = { 2.0f, 3.0f, 5.0f };
	const float fine_brands = spotting ? brands_per_kg[CLAMP((int)fuel_type, 0, FUEL_MAX - 1)] * spotting_scale : 0.0f;
	brand_points.clear();
	brand_count = 0.0f;
	for (int z = band_z0; z <= band_z1; z++) {
		for (int x = band_x0; x <= band_x1; x++) {
			const uint32_t i = z * nx + x;
			heat[i] = 0.0f;
			if (t_ignite[i] < 0.0f) {
				continue;
			}
			total_burnt += cell_area;
			if (quenched[i]) {
				continue; // put out: what is left stays, unburnt
			}
			const float left = fuel_left[i] <= 0.0f ? 0.0f : Math::exp(-(time - t_ignite[i]) / m.burn_time);
			const float kg = MAX(fuel_left[i] - left, 0.0f) * load[i];
			heat[i] = kg * HEAT_CONTENT / MAX(p_delta, 1.0e-4f);
			fuel_left[i] = left < 0.002f ? 0.0f : left;
			// Flame against a stem here: full charring from about 50 kW/m2 of ground (a flaming zone).
			char_time[i] += p_delta * MIN(heat[i] / 50000.0f, 1.0f);
			if (fine_brands > 0.0f && kg > 0.0f && time - t_ignite[i] < m.burn_time) { // from the flames, not the embers
				const float w = kg * cell_area * fine_brands;
				brand_points.push_back({ _cell_world(x, z) + Vector3(0.0f, 0.1f, 0.0f), w });
				brand_count += w;
			}
		}
	}
	if (heavy_any) {
		_burn_heavy(p_delta);
	}
}

// Large wood (BURNUP's view, Albini and Reinhardt 1995: big pieces keep burning long after the surface front has
// passed). A log catches from the surface fire burning under it unless it is too wet, flames for about Anderson's
// (1969) residence time, 384/sigma minutes (sigma its surface to volume ratio, 4/d per foot: about 3 minutes per cm
// of diameter), then smoulders for longer with no flame. Its heat feeds the flames and the glow over its cells.
void PhysXFireSpread3D::_burn_heavy(float p_delta) {
	const FuelModel &m = _model(fuel_type);
	for (int z = band_z0; z <= band_z1; z++) {
		for (int x = band_x0; x <= band_x1; x++) {
			const uint32_t i = z * nx + x;
			if (heavy[i] <= 0.0f || heavy_left[i] <= 0.0f) {
				continue;
			}
			if (heavy_t[i] < 0.0f) {
				// Catches while the fine fuel under it flames, unless the wood is too wet.
				if (t_ignite[i] >= 0.0f && time - t_ignite[i] < m.burn_time && !quenched[i] && _moisture(i) < m.moisture_extinction) {
					heavy_t[i] = time;
				}
				continue;
			}
			const float tf = MAX(_heavy_flaming_time(i), 30.0f);
			// Flaming burns down at 1/tf; smouldering (or a log put out by rain) at a third of that.
			const float k = _heavy_flaming(i) ? 1.0f / tf : 1.0f / (3.0f * tf);
			const float before = heavy_left[i];
			heavy_left[i] = before * Math::exp(-p_delta * k);
			if (heavy_left[i] < 0.01f) {
				heavy_left[i] = 0.0f;
			}
			// A burning log against a stem chars it all the while; smouldering, at about a third of the rate.
			char_time[i] += p_delta * (_heavy_flaming(i) ? 1.0f : 1.0f / 3.0f);
			if (_heavy_flaming(i)) {
				const float kg = (before - heavy_left[i]) * heavy[i];
				heat[i] += kg * HEAT_CONTENT / MAX(p_delta, 1.0e-4f);
				if (spotting && kg > 0.0f) {
					// Bark and charred twig off burning wood: the brands that carry furthest.
					const float w = kg * cell_size * cell_size * 10.0f * spotting_scale;
					brand_points.push_back({ _cell_world(x, z) + Vector3(0.0f, 0.3f, 0.0f), w });
					brand_count += w;
				}
			}
		}
	}
}

// Crowns (docs/ideas/physx.md, "Trees catch fire from a ground fire"). Crown fine fuel, foliage and twigs under 6 mm,
// from the crown's volume (a cone for a conifer, a paraboloid for a broadleaf) at a bulk density inside the crown of
// about 0.5 kg/m3 for conifers and 0.25 for broadleaves (Cruz et al. 2003 give 0.05 to 0.3 for whole stands, gaps
// included); broadleaf foliage holds more water than needles (Van Wagner's 100% is a conifer's).
static float _crown_fuel(float p_radius, float p_top, float p_base, bool p_conifer) {
	const float depth = MAX(p_top - p_base, 0.1f);
	return (p_conifer ? 0.5f / 3.0f : 0.25f / 2.0f) * Math::PI * p_radius * p_radius * depth;
}

void PhysXFireSpread3D::_gather_trees() {
	for (int64_t k = int64_t(trees.size()) - 1; k >= 0; k--) {
		if (!trees[k].scripted) {
			trees.remove_at(k);
		} else {
			Tree &t = trees[k];
			t.t_ignite = -1.0f;
			t.burnt = 0.0f;
			t.hrr = 0.0f;
			t.grounded = false;
		}
	}
	crowns_lit = 0;
	Node3D *veg = vegetation_field.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(vegetation_field));
	if (!veg || !veg->has_method("get_crowns_in")) {
		return;
	}
	const Vector3 c0 = get_global_transform().xform(Vector3(-size.x * 0.5f, 0, -size.y * 0.5f));
	const Vector3 c1 = get_global_transform().xform(Vector3(size.x * 0.5f, 0, size.y * 0.5f));
	const Rect2 rect(Vector2(MIN(c0.x, c1.x), MIN(c0.z, c1.z)), Vector2(Math::abs(c1.x - c0.x), Math::abs(c1.z - c0.z)));
	const PackedFloat32Array c = veg->call("get_crowns_in", rect);
	for (int k = 0; k + 6 < c.size(); k += 7) {
		Tree t;
		t.pos = Vector3(c[k], get_global_position().y, c[k + 1]);
		t.radius = MAX(c[k + 2], 0.1f);
		t.top = MAX(c[k + 3], 0.2f);
		t.base = CLAMP(c[k + 4], 0.0f, t.top * 0.9f);
		t.conifer = c[k + 5] > 0.5f;
		// A sparse crown (a bare winter broadleaf) holds less.
		t.fuel = _crown_fuel(t.radius, t.top, t.base, t.conifer) * CLAMP(c[k + 6] * 1.25f, 0.2f, 1.0f);
		trees.push_back(t);
	}
}

void PhysXFireSpread3D::add_tree(const Vector3 &p_position, float p_height, float p_crown_base, float p_crown_radius, bool p_conifer) {
	Tree t;
	t.pos = p_position;
	t.top = MAX(p_height, 0.2f);
	t.base = CLAMP(p_crown_base, 0.0f, t.top * 0.9f);
	t.radius = MAX(p_crown_radius, 0.1f);
	t.conifer = p_conifer;
	t.fuel = _crown_fuel(t.radius, t.top, t.base, t.conifer);
	t.scripted = true;
	trees.push_back(t);
}

void PhysXFireSpread3D::clear_trees() {
	trees.clear();
	crowns_lit = 0;
}

int PhysXFireSpread3D::get_crowns_burning() const {
	int n = 0;
	for (const Tree &t : trees) {
		n += t.t_ignite >= 0.0f && t.burnt < 0.98f ? 1 : 0;
	}
	return n;
}

float PhysXFireSpread3D::get_tree_burnt(int p_index) const {
	if (p_index < 0 || p_index >= int(trees.size())) {
		return -1.0f;
	}
	return trees[p_index].burnt;
}

// Crown fire initiation (Van Wagner 1977, as FARSITE, FlamMap and BehavePlus use it): a surface fire lights the
// crown above it once its fireline intensity reaches I0 = (0.010 CBH (460 + 25.9 FMC))^1.5 kW/m, CBH the crown base
// height in m and FMC the foliar moisture in %: about 480 kW/m for a crown 2 m up at 100%, 2,500 at 6 m. A lit crown
// burns as a torching tree does (Albini 1979; the NIST burns of whole trees, Mell et al. 2009): its fine fuel goes as
// a gamma curve, 1 - (1 + t/T) e^(-t/T), heat release peaking at T (8 s) and nearly all gone in 45 s, with a flame
// about one and a half times the tree's height, leaning with the wind by U / sqrt(g Hf). The flame lights the crowns
// it reaches (crown to crown spread, the passive to active crown fire that dense stands carry), the ground under it,
// and throws brands from the crown, lofted by the tree's own column.
template <typename F>
void PhysXFireSpread3D::_update_trees(float p_delta, const Vector3 &p_wind, const F &p_intensity, LocalVector<PhysXFirebrands::Plume> &r_plumes) {
	if (!crown_fire || trees.is_empty()) {
		return;
	}
	const Vector3 up = get_global_transform().basis.get_column(1).normalized();
	const Vector2 wind2(p_wind.x, p_wind.z);
	const float u = wind2.length();
	const Vector2 wdir = u > 1.0e-3f ? wind2 / u : Vector2();
	const float t_air = _air_temperature();
	// Burning crowns this step.
	LocalVector<uint32_t> burning;
	for (uint32_t k = 0; k < trees.size(); k++) {
		Tree &t = trees[k];
		if (!t.grounded && heights_ready) {
			int x, z;
			if (_world_cell(t.pos, x, z)) {
				t.pos.y = height[z * nx + x];
			}
			t.grounded = true;
		}
		t.hrr = 0.0f;
		if (t.t_ignite >= 0.0f && t.burnt < 0.98f) {
			burning.push_back(k);
		}
	}
	// Lighting.
	for (uint32_t k = 0; k < trees.size(); k++) {
		Tree &t = trees[k];
		if (t.t_ignite >= 0.0f || t.fuel <= 0.0f) {
			continue;
		}
		const float fmc = foliar_moisture * (t.conifer ? 1.0f : 1.6f);
		const float i0 = Math::pow(0.010f * MAX(t.base, 0.1f) * (460.0f + 25.9f * fmc * 100.0f), 1.5f);
		bool lit = p_intensity(t.pos, MAX(0.75f, 0.5f * t.radius)) >= i0;
		for (uint32_t b = 0; b < burning.size() && !lit; b++) {
			const Tree &o = trees[burning[b]];
			if (o.burnt < 0.02f) {
				continue; // still catching
			}
			const float hf = 1.5f * o.top;
			if (t.base > hf) {
				continue;
			}
			Vector2 d(t.pos.x - o.pos.x, t.pos.z - o.pos.z);
			const float dist = d.length();
			const float gap = dist - o.radius - t.radius;
			const float lean = u / Math::sqrt(9.8f * hf);
			const float down = dist > 1.0e-3f ? MAX(d.dot(wdir) / dist, 0.0f) : 0.0f;
			// The flame's reach grows as the crown flares (a few seconds).
			const float reach = (0.5f + down * hf * lean) * MIN((time - o.t_ignite) / 4.0f, 1.0f);
			lit = gap <= reach;
		}
		if (lit) {
			t.t_ignite = time;
			crowns_lit++;
			ignite(t.pos, t.radius); // burning needles and twigs fall and light the ground under it
		}
	}
	// Burning.
	const float brands_per_kg = spotting ? 20.0f * spotting_scale : 0.0f;
	for (uint32_t k = 0; k < trees.size(); k++) {
		Tree &t = trees[k];
		if (t.t_ignite < 0.0f || t.burnt >= 0.98f) {
			continue;
		}
		const float a = (time - t.t_ignite) / CROWN_TIME;
		const float c = 1.0f - (1.0f + a) * Math::exp(-a);
		const float kg = MAX(c - t.burnt, 0.0f) * t.fuel;
		t.burnt = c >= 0.98f ? 1.0f : c;
		t.hrr = kg * HEAT_CONTENT / MAX(p_delta, 1.0e-4f);
		const float depth = t.top - t.base;
		const Vector3 centre = t.pos + up * (t.base + 0.4f * depth);
		if (spotting && kg > 0.0f) {
			const float w = kg * brands_per_kg * 0.25f;
			for (int q = 0; q < 4; q++) {
				const float ang = Math::TAU * (q + 0.5f * (k & 1)) / 4.0f;
				const Vector3 off(Math::cos(ang) * 0.5f * t.radius, (0.1f + 0.25f * q) * depth, Math::sin(ang) * 0.5f * t.radius);
				brand_points.push_back({ t.pos + up * t.base + off, w });
				brand_count += w;
			}
			// Burning twigs dropping out of the crown as it burns (about 1.5 a kilogram of crown fuel), from all
			// through it.
			const float wt = kg * 1.5f * spotting_scale;
			const Vector3 off((_hash01(k, uint32_t(time * 60.0f)) - 0.5f) * t.radius, (0.2f + 0.6f * _hash01(k + 7919u, uint32_t(time * 60.0f))) * depth, (_hash01(k + 104729u, uint32_t(time * 60.0f)) - 0.5f) * t.radius);
			brand_points.push_back({ t.pos + up * t.base + off, wt, 0.015f });
			brand_count += wt;
		}
		if (spotting && t.hrr > 0.0f) {
			PhysXFirebrands::Plume pl;
			pl.base = centre;
			pl.w0 = PhysXFirebrands::plume_speed(t.hrr / 1000.0f / (2.0f * t.radius), t_air);
			pl.width = 2.0f * t.radius;
			r_plumes.push_back(pl);
		}
		// What the plants see: the crown charred upward as it flares (fire_state_map's flame length over its
		// footprint), foliage around it killed by the column, and a glow while it flames.
		const float flame_top = MIN(t.base + (1.3f * t.top - t.base) * MIN(c / 0.4f, 1.0f), 32.0f);
		const float glow = c < 0.9f ? 1.0f : 0.0f;
		const float reach = 1.6f * t.radius; // (> the crown's ragged edge, 1.55 radius at most)
		int x0, z0, x1, z1;
		const bool in0 = _world_cell(t.pos - Vector3(reach, 0, reach), x0, z0);
		const bool in1 = _world_cell(t.pos + Vector3(reach, 0, reach), x1, z1);
		if (!in0 || !in1) {
			// Partly outside the grid: clamp to it.
			const Vector3 l0 = get_global_transform().affine_inverse().xform(t.pos - Vector3(reach, 0, reach));
			const Vector3 l1 = get_global_transform().affine_inverse().xform(t.pos + Vector3(reach, 0, reach));
			x0 = CLAMP(int((MIN(l0.x, l1.x) + size.x * 0.5f) / cell_size), 0, nx - 1);
			x1 = CLAMP(int((MAX(l0.x, l1.x) + size.x * 0.5f) / cell_size), 0, nx - 1);
			z0 = CLAMP(int((MIN(l0.z, l1.z) + size.y * 0.5f) / cell_size), 0, nz - 1);
			z1 = CLAMP(int((MAX(l0.z, l1.z) + size.y * 0.5f) / cell_size), 0, nz - 1);
		}
		if (x0 > x1) {
			SWAP(x0, x1);
		}
		if (z0 > z1) {
			SWAP(z0, z1);
		}
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector3 wp = _cell_world(x, z);
				const float d = Vector2(wp.x - t.pos.x, wp.z - t.pos.z).length();
				const uint32_t i = z * nx + x;
				// The crown's outline is not a circle: its lowest, longest branches reach past the mean radius
				// unevenly, so the burnt footprint has a wavering edge (a few lobes and a finer jitter).
				const float ang = Math::atan2(wp.z - t.pos.z, wp.x - t.pos.x);
				const float lobe = 0.5f + 0.5f * Math::sin(3.0f * ang + 2.1f * k) * Math::cos(5.0f * ang + 0.7f * k);
				const float edge = t.radius * (1.0f + 0.4f * lobe + 0.15f * _hash01(uint32_t(x), uint32_t(z)));
				if (d <= edge) {
					const float rim = 1.0f - Math::smoothstep(0.75f * edge, edge, d);
					peak_flame[i] = MAX(peak_flame[i], flame_top);
					crown_char[i] = MAX(crown_char[i], MIN(c * 2.5f, 1.0f) * (0.4f + 0.6f * rim));
					crown_glow[i] = glow > 0.0f ? MAX(crown_glow[i], glow * rim) : crown_glow[i] * 0.9f;
				}
				if (d <= reach) {
					peak_scorch[i] = MAX(peak_scorch[i], MIN(1.5f * t.top * MIN(c / 0.4f, 1.0f), 48.0f));
				}
			}
		}
	}
}

float PhysXFireSpread3D::get_heavy_fuel_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || heavy.is_empty()) {
		return 0.0f;
	}
	const uint32_t i = z * nx + x;
	return heavy[i] * heavy_left[i];
}

float PhysXFireSpread3D::_air_temperature() const {
	if (const WorldWeather *wm = WorldContracts::weather(); wm->has_provider()) {
		return wm->air_temperature_at(get_global_position());
	}
	return 20.0f;
}

// Fuel moisture from the weather (docs/ideas/physx.md, "Weather wets the fire"): the fine dead fuel moves towards
// its equilibrium moisture for the air's temperature and humidity with its time lag (Van Wagner and Pech 1985, the
// Canadian FFMC's drying and wetting EMC; WRF-SFIRE's fuel moisture model), and rain wets it at once by what the
// fuel bed intercepts. A burning cell whose fuel gets wetter than the moisture of extinction goes out.
bool PhysXFireSpread3D::_update_moisture(float p_delta) {
	if (!use_weather_moisture || phi.is_empty()) {
		return false;
	}
	const WorldWeather *wm = WorldContracts::weather();
	if (!wm->has_provider()) {
		return false;
	}
	const float t = wm->air_temperature_at(get_global_position());
	const float hum = CLAMP(wm->air_humidity_at(get_global_position()) * 100.0f, 0.0f, 100.0f);
	const float k = 0.18f * (21.1f - t) * (1.0f - Math::exp(-0.115f * hum));
	const float ed = (0.942f * Math::pow(hum, 0.679f) + 11.0f * Math::exp((hum - 100.0f) / 10.0f) + k) * 0.01f;
	const float ew = (0.618f * Math::pow(hum, 0.753f) + 10.0f * Math::exp((hum - 100.0f) / 10.0f) + k) * 0.01f;
	const float a = 1.0f - Math::exp(-p_delta / moisture_timelag);
	if (base_moisture > ed) {
		base_moisture += (ed - base_moisture) * a;
	} else if (base_moisture < ew) {
		base_moisture += (ew - base_moisture) * a;
	}
	// Rain on 4 m blocks: a fine fuel bed holds about a third of the rain on it (interception; grass and litter
	// hold 0.2-0.5 mm before it drips through), up to about 2.5 times its dry weight (WRF-SFIRE's saturation);
	// standing water on the fuel dries several times faster than the fuel itself.
	const FuelModel &m = _model(fuel_type);
	const int block = MAX(1, (int)Math::round(4.0f / cell_size));
	const float dry = Math::exp(-p_delta * 4.0f / moisture_timelag);
	for (int bz = 0; bz < nz; bz += block) {
		for (int bx = 0; bx < nx; bx += block) {
			const int cx = MIN(bx + block / 2, nx - 1), cz = MIN(bz + block / 2, nz - 1);
			const Vector3 block_at = _cell_world(cx, cz);
			const float rain = MAX(wm->precipitation_at(block_at), 0.0f); // mm/h = kg/m2/h
			// Fuel lying on wet ground (the weather's surface film: rain that fell before the grid was there, dew)
			// holds about as much as the film: soaked ground keeps it past extinction, dewy ground slows the front.
			const float ground_wet = CLAMP(wm->surface_wetness_at(block_at), 0.0f, 1.0f) * 1.2f * m.moisture_extinction;
			for (int z = bz; z < MIN(bz + block, nz); z++) {
				for (int x = bx; x < MIN(bx + block, nx); x++) {
					const uint32_t i = z * nx + x;
					float w = rain_water[i] * dry;
					if (rain > 0.0f && load[i] > 1.0e-4f) {
						w = MIN(w + rain / 3600.0f * p_delta * 0.33f / load[i], 2.5f);
					}
					w = MAX(w, ground_wet - base_moisture * wet[i]);
					rain_water[i] = w;
					const bool burning = (t_ignite[i] >= 0.0f && fuel_left[i] > 0.0f) || (!heavy.is_empty() && _heavy_flaming(i));
					if (burning && !quenched[i] && _moisture(i) >= m.moisture_extinction) {
						quenched[i] = 1;
					}
				}
			}
		}
	}
	return true;
}

// Water from the liquid fluids (docs/ideas/physx.md, "Water puts fires out"): a bucket, a hose or a burst tank of
// PhysXParticleFluid3D. The water lying on a cell's ground (particles up to 0.4 m over it) soaks its fine fuel, up to
// about 2.5 times the fuel's dry weight as rain does (WRF-SFIRE's saturation); a burning cell whose fuel goes over
// the moisture of extinction goes out (Rothermel 1972), the way water on a grass fire works: by cooling and wetting
// the fuel ahead of and in the flames, not by smothering. Without a weather map to dry it, the water on the fuel
// dries at the fuel's own time lag.
void PhysXFireSpread3D::_apply_fluid_water(float p_delta, bool p_weather_dried) {
	if (phi.is_empty() || height.is_empty()) {
		return;
	}
	if (!p_weather_dried) {
		const float dry = Math::exp(-p_delta * 4.0f / moisture_timelag);
		for (float &w : rain_water) {
			w *= dry;
		}
	}
	const LocalVector<PhysXParticleFluid3D *> &fluids = PhysXParticleFluid3D::get_water_fluids();
	if (fluids.is_empty()) {
		return;
	}
	const Transform3D inv = get_global_transform().affine_inverse();
	const float cell_area = cell_size * cell_size;
	const FuelModel &m = _model(fuel_type);
	LocalVector<Vector3> pts;
	water_cells.clear();
	for (const PhysXParticleFluid3D *f : fluids) {
		if (!f->is_inside_tree()) {
			continue;
		}
		f->gather_world_positions(pts);
		const float mass = f->get_particle_water_mass();
		if (pts.is_empty() || mass <= 0.0f) {
			continue;
		}
		for (const Vector3 &p : pts) {
			const Vector3 l = inv.xform(p);
			const int x = (int)Math::floor((l.x + size.x * 0.5f) / cell_size);
			const int z = (int)Math::floor((l.z + size.y * 0.5f) / cell_size);
			if (x < 0 || z < 0 || x >= nx || z >= nz) {
				continue;
			}
			const uint32_t i = z * nx + x;
			const float above = p.y - height[i];
			if (above < -0.2f || above > 0.4f) {
				continue;
			}
			float *w = water_cells.getptr(i);
			if (w) {
				*w += mass;
			} else {
				water_cells.insert(i, mass);
			}
		}
	}
	for (const KeyValue<uint32_t, float> &e : water_cells) {
		const uint32_t i = e.key;
		const float on_fuel = MIN(e.value / cell_area / MAX(load[i], 0.05f), 2.5f); // kg water per kg fuel
		rain_water[i] = MAX(rain_water[i], on_fuel);
		const bool burning = (t_ignite[i] >= 0.0f && fuel_left[i] > 0.0f) || (!heavy.is_empty() && _heavy_flaming(i));
		// Steam: the water takes the hot bed's heat, once: its fuel and char near 600 C (1.5 kJ/kg K), a
		// burning log's more, and about a second of the flames' output where it still burns; no more than the
		// water there can boil.
		const bool hot = burning || (t_ignite[i] >= 0.0f && time - t_ignite[i] < 3.0f * m.burn_time);
		if (hot && !steamed[i]) {
			steamed[i] = 1;
			float j = cell_area * (load[i] * MAX(fuel_left[i], 0.3f) + (heavy.is_empty() ? 0.0f : MIN(heavy[i], 20.0f) * 0.25f)) * 1500.0f * 600.0f;
			j += heat[i] * cell_area;
			j = MIN(j, e.value * PhysXSteam::BOIL_HEAT);
			steam.add_heat(_cell_world(int(i % nx), int(i / nx)), j, 0.5f);
		}
		if (burning && !quenched[i] && _moisture(i) >= m.moisture_extinction) {
			quenched[i] = 1;
			quenched_by_water++;
		}
	}
}

// Fire meeting standing or running water (WorldContracts' water: a river bank, a lake shore, a flooded hollow):
// fuel under the surface cannot burn and a cell the front reaches there goes out in a burst of steam; flames
// beside the water boil some of it off, a tenth of their heat going into the water a metre away (radiation and
// the hot gas over it), as a thin steady steam along the shore.
void PhysXFireSpread3D::_water_edge(float p_delta) {
	WorldWater *ww = WorldContracts::water();
	if (!ww || !ww->has_provider() || height.is_empty()) {
		return;
	}
	const float cell_area = cell_size * cell_size;
	const float step = MAX(1.0f, cell_size);
	const Vector3 offs[4] = { Vector3(step, 0, 0), Vector3(-step, 0, 0), Vector3(0, 0, step), Vector3(0, 0, -step) };
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const uint32_t i = z * nx + x;
			if (t_ignite[i] < 0.0f || fuel_left[i] <= 0.0f || quenched[i]) {
				continue;
			}
			Vector3 wp = _cell_world(x, z);
			wp.y = height[i];
			const float h = ww->height_at(wp);
			if (Math::is_finite(h) && h >= wp.y - 0.02f) {
				quenched[i] = 1;
				if (!steamed[i]) {
					steamed[i] = 1;
					steam.add_heat(wp, heat[i] * cell_area + cell_area * load[i] * 1500.0f * 600.0f, 0.4f);
				}
				continue;
			}
			if (heat[i] <= 0.0f || ((x + z) & 1)) {
				continue; // a checkerboard is enough to find the shore
			}
			for (const Vector3 &o : offs) {
				const Vector3 q = wp + o;
				const float hq = ww->height_at(q);
				if (Math::is_finite(hq) && hq >= q.y - 0.3f && hq <= q.y + 1.0f) {
					steam.add_heat(Vector3(q.x, hq, q.z), 0.1f * heat[i] * cell_area * 2.0f * p_delta, 0.4f, 1.0f);
					break;
				}
			}
		}
	}
}

float PhysXFireSpread3D::get_ground_height_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || height.is_empty()) {
		return get_global_position().y;
	}
	return height[z * nx + x];
}

float PhysXFireSpread3D::get_moisture_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || wet.is_empty()) {
		return base_moisture;
	}
	return _moisture(z * nx + x);
}

// Hot gas from a heat source (a campfire's Flow, its cpu_readback on) lights the ground under it.
void PhysXFireSpread3D::_ignite_from_heat_source() {
	if (heat_source.is_empty()) {
		return;
	}
	PhysXFlow3D *src = Object::cast_to<PhysXFlow3D>(get_node_or_null(heat_source));
	if (!src) {
		return;
	}
	if (!src->is_cpu_readback()) {
		src->set_cpu_readback(true);
		return;
	}
	// The cells within a couple of metres of the source, a few centimetres above the ground.
	int cx, cz;
	_world_cell(src->get_global_position(), cx, cz);
	const int reach = (int)Math::ceil(2.0f / cell_size);
	for (int z = MAX(cz - reach, 0); z <= MIN(cz + reach, nz - 1); z++) {
		for (int x = MAX(cx - reach, 0); x <= MIN(cx + reach, nx - 1); x++) {
			const uint32_t i = z * nx + x;
			if (t_ignite[i] >= 0.0f || load[i] <= 1.0e-4f) {
				continue;
			}
			const float t = src->sample_temperature(_cell_world(x, z) + Vector3(0, 0.05f, 0));
			if (t > ignition_temperature) {
				ignite(_cell_world(x, z), cell_size * 0.5f);
			}
		}
	}
}

// Flames: the flaming front (cells lit less than a burn time ago) merged into half-metre tiles, each of the strongest
// given a sphere emitter of hot fuel gas that Flow's combustion burns. Tiles are picked so that the emitters spread
// along the whole front (a first pass skips tiles next to one already picked), and each is fed by its tile's Byram
// fireline intensity, so the flames stand in a line along the leading arc, tallest at the head fire downwind and low
// at the back. Spheres, not boxes: an emitter holds the gas inside it at its own values, and a box left a block of
// flame with straight sides.
// The fire's own wind. A fire's plume draws air in at its base: FireFlux (Clements et al. 2007) measured an indraft of
// a few m/s at a grass front, opposing the ambient wind ahead of it. Each 2 m stretch of burning front is a sink
// drawing in, per metre of front and per side, q = (pi/2) w* d0, where w* is Byram's buoyant velocity for its
// intensity and d0 half the flame length, so the inflow just outside the flames is about w*/2 and falls as 1/d along
// a long front (a line sink: u = q / (pi d)) and as 1/d^2 past its ends. The strongest 64 stretches are kept. What a
// front feels from its own plume is already in Rothermel's fits (lab and field fires had their indrafts), so the
// spread takes only the pull of fire ahead of a front (another fire, a spot fire, the far side of a closing gap),
// which draws fronts together as they meet (junction fires, Viegas et al. 2012). Grass and cloth take all of it.
void PhysXFireSpread3D::_update_indraft() {
	sinks.clear();
	if (!fire_wind || t_ignite.is_empty()) {
		pull.clear();
		return;
	}
	const float t_air = _air_temperature() + 273.15f;
	pull_tile = MAX(1, (int)Math::round(2.0f / cell_size));
	const float tile_w = pull_tile * cell_size;
	pull_tx = (nx + pull_tile - 1) / pull_tile;
	const int tz = (nz + pull_tile - 1) / pull_tile;
	struct Agg {
		float watts = 0.0f;
		Vector3 c;
	};
	LocalVector<Agg> agg;
	agg.resize(pull_tx * tz);
	const float cell_area = cell_size * cell_size;
	for (int z = MAX(band_z0, 0); z <= MIN(band_z1, nz - 1); z++) {
		for (int x = MAX(band_x0, 0); x <= MIN(band_x1, nx - 1); x++) {
			const uint32_t i = z * nx + x;
			if (heat[i] <= 0.0f) {
				continue;
			}
			Agg &a = agg[(z / pull_tile) * pull_tx + x / pull_tile];
			const float w = heat[i] * cell_area;
			a.watts += w;
			a.c += _cell_world(x, z) * w;
		}
	}
	LocalVector<Sink> all;
	for (const Agg &a : agg) {
		const float intensity = a.watts / 1000.0f / tile_w; // kW/m, the tile's stretch of front
		if (intensity < 50.0f) {
			continue;
		}
		const float ws = Math::pow(9.8f * intensity * 1000.0f / (1.2f * 1005.0f * t_air), 1.0f / 3.0f);
		Sink k;
		k.pos = a.c / a.watts;
		k.d0 = MAX(0.5f * 0.0775f * Math::pow(intensity, 0.46f), 0.5f);
		k.q = 0.5f * Math::PI * ws * k.d0 * tile_w * 2.0f; // both sides
		all.push_back(k);
	}
	struct ByQ {
		bool operator()(const Sink &a, const Sink &b) const { return a.q > b.q; }
	};
	all.sort_custom<ByQ>();
	for (uint32_t k = 0; k < all.size() && k < 64; k++) {
		sinks.push_back(all[k]);
	}
	// The pull on each tile from fire more than 5 m away (beyond its own front's reach), in the grid's frame.
	pull.resize(pull_tx * tz);
	const Basis to_local = get_global_transform().basis.inverse();
	if (wind_image.is_null() || wind_image->get_width() != pull_tx || wind_image->get_height() != tz) {
		wind_image = Image::create_empty(pull_tx, tz, false, Image::FORMAT_RGF);
		wind_texture = ImageTexture::create_from_image(wind_image);
	}
	for (int z = 0; z < tz; z++) {
		for (int x = 0; x < pull_tx; x++) {
			const Vector3 c = _cell_world(MIN(x * pull_tile + pull_tile / 2, nx - 1), MIN(z * pull_tile + pull_tile / 2, nz - 1));
			const Vector3 far = sinks.is_empty() ? Vector3() : to_local.xform(_indraft_at(c, 5.0f));
			pull[z * pull_tx + x] = Vector2(far.x, far.z);
			const Vector3 all_w = sinks.is_empty() ? Vector3() : _indraft_at(c, 0.0f);
			wind_image->set_pixel(x, z, Color(all_w.x, all_w.z, 0.0f));
		}
	}
	wind_texture->update(wind_image);
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 o = get_global_transform().xform(Vector3(-size.x * 0.5f, 0, -size.y * 0.5f));
	rs->global_shader_parameter_set("fire_wind_map", wind_texture);
	rs->global_shader_parameter_set("fire_wind_rect", sinks.is_empty() ? Color() : Color(o.x, o.z, pull_tx * tile_w, tz * tile_w));
}

Vector3 PhysXFireSpread3D::_indraft_at(const Vector3 &p_position, float p_skip) const {
	Vector3 u;
	for (const Sink &k : sinks) {
		const Vector3 r(k.pos.x - p_position.x, 0.0f, k.pos.z - p_position.z);
		const float d = r.length();
		if (d < MAX(p_skip, 0.05f)) {
			continue;
		}
		const float dd = MAX(d, k.d0);
		u += r / d * (k.q / (2.0f * Math::PI * dd * dd));
	}
	return u.limit_length(15.0f);
}

// Char at a stem's foot: the time flames and smouldering logs stood there at softwood's notional charring rate (EN
// 1995-1-2, 0.8 mm a minute), bark and wood together; TreeWind3D takes off the bark.
float PhysXFireSpread3D::get_stem_char_at(const Vector3 &p_position) const {
	int x, z;
	if (char_time.is_empty() || !_world_cell(p_position, x, z)) {
		return 0.0f;
	}
	return char_time[z * nx + x] * 0.8e-3f / 60.0f;
}

void PhysXFireSpread3D::set_fire_wind(bool p_v) {
	fire_wind = p_v;
	if (!fire_wind) {
		sinks.clear();
		pull.clear();
	}
}

// Ash a burnt cell left, kg/m2: the mineral share of the fine fuel burnt (grass and litter ash is about 6% of the
// dry mass at a fire's heat; Bodi et al. 2014 review ash loads of 0.1-5 kg/m2, the low end in grass) and of the
// needles and twigs a burnt crown dropped over it.
float PhysXFireSpread3D::_ash_made(uint32_t p_i) const {
	if (t_ignite[p_i] < 0.0f) {
		return 0.0f;
	}
	float a = 0.06f * load[p_i] * (1.0f - fuel_left[p_i]);
	if (!crown_char.is_empty()) {
		a += 0.15f * crown_char[p_i];
	}
	return a;
}

// Wind erosion of the burnt ground's ash (Wagenbrenner et al. 2013, 2017: fresh burn scars are among the strongest
// dust sources, emitting from winds near 5 m/s until rain crusts the ash). The friction velocity over bare burnt
// ground is about 0.075 of the 2 m wind (the log law with a roughness of about 1 cm); loose ash moves above a
// threshold friction velocity of about 0.25 m/s (some 3.5 m/s at 2 m). The horizontal flux is White's (1979)
// Q = 2.6 rho / g u*^3 (1 - (u*t/u*)^2), and the dust lifted from it F = alpha Q, alpha about 2e-3 1/m for loose ash.
// Gusts are a noise field carried along by the wind (Taylor's frozen turbulence), +/- 35% of the wind, so with the
// flux as u*^3 the ash comes off in travelling puffs. Cells still hot, wet (rain, fluid water) or not burnt give
// none; each patch of about 2 m blows until its ash is gone.
void PhysXFireSpread3D::_blow_ash(float p_delta) {
	ash_sources.clear();
	if (!blowing_ash || t_ignite.is_empty() || total_burnt <= 0.0f) {
		return;
	}
	const Vector3 w = _wind();
	const Vector2 wh(w.x, w.z);
	const float u = wh.length();
	if (u * 1.35f * 0.075f <= 0.25f) {
		return;
	}
	const FuelModel &m = _model(fuel_type);
	const float cell_area = cell_size * cell_size;
	const int tile = MAX(1, (int)Math::round(2.0f / cell_size));
	const int tx = (nx + tile - 1) / tile, tz = (nz + tile - 1) / tile;
	LocalVector<PhysXAsh::Source> tiles;
	tiles.resize(tx * tz);
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const uint32_t i = z * nx + x;
			if (t_ignite[i] < 0.0f || time - t_ignite[i] < 3.0f * m.burn_time || rain_water[i] > 0.02f) { // still hot (as steam takes it), or wet
				continue;
			}
			const float left = _ash_made(i) - ash_blown[i];
			if (left <= 1.0e-5f) {
				continue;
			}
			const Vector3 wp = _cell_world(x, z);
			const float gust = 1.0f + 0.7f * (_fuel_noise((wp.x - wh.x * time) / 6.0f, (wp.z - wh.y * time) / 6.0f, 11u) - 0.5f);
			const float us = 0.075f * u * gust;
			if (us <= 0.25f) {
				continue;
			}
			const float q = 2.6f * 1.2f / 9.8f * us * us * us * (1.0f - 0.0625f / (us * us));
			const float lift = MIN(2.0e-3f * q * p_delta, left); // kg/m2 this sample
			ash_blown[i] += lift;
			PhysXAsh::Source &t = tiles[(z / tile) * tx + (x / tile)];
			const float kg_s = lift * cell_area / p_delta;
			t.position += wp * kg_s;
			t.rate += kg_s;
		}
	}
	for (PhysXAsh::Source &t : tiles) {
		if (t.rate > 0.0f) {
			t.position /= t.rate;
			t.size = tile * cell_size;
			ash_sources.push_back(t);
		}
	}
}

void PhysXFireSpread3D::set_blowing_ash(bool p_v) {
	blowing_ash = p_v;
	if (!blowing_ash) {
		ash_sources.clear();
	}
}

float PhysXFireSpread3D::_whirl_rand() {
	whirl_seed ^= whirl_seed << 13;
	whirl_seed ^= whirl_seed >> 17;
	whirl_seed ^= whirl_seed << 5;
	return float(whirl_seed & 0xffffffu) / float(0xffffff);
}

// Fire whirls (docs/ideas/physx.md, "Fire whirls"; Forthofer and Goodrick 2011): a flank of strong fire in a cross
// shear spins up a vertical vortex that stands over the burning fuel, lifts its gas and brands high and drifts
// downwind. They form when the wind is about the fire's own buoyant velocity scale, w* = (g I / (rho cp T))^(1/3)
// for a front of intensity I (Kuwana et al. 2008: whirls at U / w* near 1 behind line fires; Kuwana et al. 2013 for
// L-shaped fronts). A whirl's core is some third of the flame length across, it spins at about twice w*, its core
// rises at about twice w*, and it lives tens of seconds while it stands over flaming fuel. Drawn in the fire's Flow
// by four emitters around the core, tangential and rising; brands caught in it rise on its updraft.
void PhysXFireSpread3D::_update_whirls(float p_delta, const Vector3 &p_wind, const LocalVector<Vector3> &p_flank, const LocalVector<float> &p_flank_q, const LocalVector<Vector3> &p_lee, LocalVector<PhysXFirebrands::Plume> &r_plumes) {
	const float u = p_wind.length();
	const float t_air = _air_temperature() + 273.15f;
	// Births: each eligible flank tile has about a 1-in-180 chance a second near U / w* = 1 (one whirl every 20 s or so
	// along a strong flank; fewer away from it).
	for (uint32_t k = 0; k < p_flank.size() && whirls.size() < 2; k++) {
		const float ws = Math::pow(9.8f * p_flank_q[k] * 1000.0f / (1.2f * 1005.0f * t_air), 1.0f / 3.0f);
		const float ratio = u / MAX(ws, 0.1f);
		const float favour = Math::exp(-Math::pow(Math::log(MAX(ratio, 1.0e-3f)) / 0.5f, 2.0f)); // 1 at U = w*
		bool near = false;
		for (const Whirl &w : whirls) {
			near = near || w.pos.distance_to(p_flank[k]) < 6.0f;
		}
		if (near || p_flank_q[k] < 300.0f || _whirl_rand() > favour * p_delta / 180.0f) {
			continue;
		}
		Whirl w;
		w.pos = p_flank[k];
		const float flame = 0.0775f * Math::pow(p_flank_q[k], 0.46f);
		w.radius = CLAMP(0.3f * flame, 0.3f, 3.0f);
		// Either way round: both turns are seen on flanks (Forthofer and Goodrick 2011).
		w.spin = 2.0f * ws * (_whirl_rand() < 0.5f ? -1.0f : 1.0f);
		w.rise = 2.0f * ws;
		w.life = 10.0f + 20.0f * _whirl_rand();
		whirls.push_back(w);
		whirls_spawned++;
	}
	// Lee slopes (docs/ideas/physx.md, "Fire whirls behind ridges"; Sharples et al. 2012, Simpson et al. 2013): wind
	// over a ridge separates and rolls into a horizontal eddy in its lee; fire on that slope tilts it upright into
	// whirls that run across the slope, along the ridge, lighting fuel as they go (vorticity-driven lateral spread).
	// The operational test: a lee slope steeper than about 20 degrees facing within 40 degrees of downwind, in a
	// wind over about 20 km/h. Births: about 1 in 600 a second for each strong tile there, with no U/w* condition
	// (the eddy is the ridge's, not the fire's), so a burning lee slope throws one every minute or so.
	for (uint32_t k = 0; k < p_lee.size() && whirls.size() < 2; k += 2) {
		const Vector3 at = p_lee[k];
		const Vector3 contour = p_lee[k + 1];
		bool near = false;
		for (const Whirl &w : whirls) {
			near = near || w.pos.distance_to(at) < 6.0f;
		}
		if (near || _whirl_rand() > p_delta / 600.0f) {
			continue;
		}
		int cx, cz;
		const float q = _world_cell(at, cx, cz) ? heat[cz * nx + cx] * cell_size / 1000.0f : 300.0f; // kW/m, rough
		const float ws = Math::pow(9.8f * MAX(q, 300.0f) * 1000.0f / (1.2f * 1005.0f * t_air), 1.0f / 3.0f);
		Whirl w;
		w.pos = at;
		const float flame = 0.0775f * Math::pow(MAX(q, 300.0f), 0.46f);
		w.radius = CLAMP(0.3f * flame, 0.3f, 3.0f);
		w.spin = 2.0f * ws * (_whirl_rand() < 0.5f ? -1.0f : 1.0f);
		w.rise = 2.0f * ws;
		w.life = 20.0f + 40.0f * _whirl_rand();
		w.lee = true;
		// Across the slope at a fifth of the wind (lateral spread runs near the head fire's rate), either way.
		w.drift = contour * (0.2f * u * (_whirl_rand() < 0.5f ? -1.0f : 1.0f)) + p_wind * 0.1f;
		whirls.push_back(w);
		whirls_spawned++;
		lee_whirls_spawned++;
	}
	const Vector3 up = get_global_transform().basis.get_column(1).normalized();
	for (int64_t k = int64_t(whirls.size()) - 1; k >= 0; k--) {
		Whirl &w = whirls[k];
		w.age += p_delta;
		// Drifts downwind at about a third of the wind, wandering.
		w.pos += ((w.lee ? w.drift : p_wind * 0.3f) + Vector3(_whirl_rand() - 0.5f, 0.0f, _whirl_rand() - 0.5f) * 0.6f) * p_delta;
		int x, z;
		const bool inside = _world_cell(w.pos, x, z);
		if (inside) {
			w.pos.y = height[z * nx + x];
		}
		if (w.lee && inside && t_ignite[z * nx + x] < 0.0f) {
			ignite(w.pos, 0.5f * w.radius); // the whirl carries the fire along the slope
		}
		const bool over = inside && t_ignite[z * nx + x] >= 0.0f && (heat[z * nx + x] > 0.0f || w.lee);
		if (!over) {
			w.age += 2.0f * p_delta; // off the flaming fuel it starves
		}
		if (w.age >= w.life) {
			whirls.remove_at_unordered(k);
			continue;
		}
		PhysXFirebrands::Plume pl;
		pl.base = w.pos;
		pl.w0 = w.rise * 1.5f;
		pl.width = 2.0f * w.radius;
		r_plumes.push_back(pl);
	}
	if (!flow || (whirls.is_empty() && whirl_emitters.is_empty())) {
		return;
	}
	while (whirl_emitters.size() < 8) {
		PhysXFlowEmitter3D *em = memnew(PhysXFlowEmitter3D);
		em->set_name(vformat("Whirl%d", (int)whirl_emitters.size()));
		em->set_enabled(false);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
		flow->add_child(em);
		em->set_flow_path(em->get_path_to(flow));
		whirl_emitters.push_back(em);
	}
	for (uint32_t e = 0; e < whirl_emitters.size(); e++) {
		PhysXFlowEmitter3D *em = whirl_emitters[e];
		const uint32_t k = e / 4;
		if (k >= whirls.size()) {
			em->set_enabled(false);
			continue;
		}
		const Whirl &w = whirls[k];
		const float ramp = MIN(w.age / 2.0f, 1.0f) * MIN((w.life - w.age) / 3.0f, 1.0f);
		// A helix up the core: the flame is drawn up into a column some three flame lengths tall (Forthofer and
		// Goodrick 2011), each source pushing the gas round and up.
		const uint32_t i = e % 4;
		const float ang = Math::TAU * i / 4.0f + w.age * w.spin / MAX(w.radius, 0.3f);
		const Vector3 r(Math::cos(ang), 0.0f, Math::sin(ang));
		const Vector3 tangent = up.cross(r) * w.spin;
		const float src = MAX(0.8f * w.radius, 0.4f);
		em->set_enabled(true);
		em->set_radius(src);
		// Overlapping sources, so the column is one body of flame, not beads.
		em->set_global_position(w.pos + r * w.radius * (1.0f - 0.2f * i) + up * (0.3f + 1.2f * src * i));
		em->set_velocity((tangent + up * w.rise) * ramp + p_wind * 0.3f);
		em->set_temperature(1.0f);
		em->set_fuel(flame_strength * 0.6f * ramp);
		em->set_burn(0.0f);
		em->set_smoke(0.0f);
		em->set_couple_rate(4.0f);
	}
}

void PhysXFireSpread3D::_update_emitters(const Vector3 &p_wind) {
	if (!flow) {
		return;
	}
	const int tile = MAX(1, (int)Math::round(0.5f / cell_size)); // ~0.5 m tiles
	const float tile_w = tile * cell_size;
	const int tx = (nx + tile - 1) / tile, tz = (nz + tile - 1) / tile;
	struct Tile {
		float heat = 0.0f; // W
		Vector3 centre;
		float weight = 0.0f;
	};
	LocalVector<Tile> tiles;
	tiles.resize(tx * tz);
	const float cell_area = cell_size * cell_size;
	const float burn_time = _model(fuel_type).burn_time;
	if (band_x1 >= band_x0) {
		for (int z = band_z0; z <= band_z1; z++) {
			for (int x = band_x0; x <= band_x1; x++) {
				const uint32_t i = z * nx + x;
				const float q = heat[i];
				if (q <= 0.0f || (time - t_ignite[i] > burn_time && !(heavy_any && _heavy_flaming(i)))) {
					continue; // not burning, or past its flaming phase (embers glow on the ground map); logs flame on
				}
				Tile &t = tiles[(z / tile) * tx + x / tile];
				t.heat += q * cell_area;
				t.centre += _cell_world(x, z) * q;
				t.weight += q;
			}
		}
	}
	// What the plants over each flaming cell see (fire_state_map): the flame length of its tile's front (Byram) and the
	// height the plume kills foliage to, where it cools to about 60 C (Van Wagner 1973, as BehavePlus: I in kW/m, U the
	// wind in m/s, the air at 20 C unless a weather map gives it), kept at their peak.
	if (band_x1 >= band_x0) {
		const float u = p_wind.length();
		for (int z = band_z0; z <= band_z1; z++) {
			for (int x = band_x0; x <= band_x1; x++) {
				const uint32_t i = z * nx + x;
				if (heat[i] <= 0.0f || (time - t_ignite[i] > burn_time && !(heavy_any && _heavy_flaming(i)))) {
					continue;
				}
				const Tile &t = tiles[(z / tile) * tx + x / tile];
				const float intensity = t.heat / 1000.0f / tile_w;
				if (intensity <= 0.0f) {
					continue;
				}
				const float flame = 0.0775f * Math::pow(intensity, 0.46f);
				const float scorch = 4.483f * Math::pow(intensity, 7.0f / 6.0f) / (Math::sqrt(0.107f * intensity + u * u * u) * MAX(60.0f - _air_temperature(), 10.0f));
				peak_flame[i] = MAX(peak_flame[i], MIN(flame, 32.0f));
				peak_scorch[i] = MAX(peak_scorch[i], MIN(MAX(scorch, flame), 48.0f));
			}
		}
	}
	LocalVector<int> order;
	for (uint32_t i = 0; i < tiles.size(); i++) {
		if (tiles[i].heat > 500.0f) {
			order.push_back(i);
		}
	}
	struct Stronger {
		const LocalVector<Tile> *tiles;
		bool operator()(int a, int b) const { return (*tiles)[a].heat > (*tiles)[b].heat; }
	};
	SortArray<int, Stronger> sorter;
	sorter.compare.tiles = &tiles;
	if (!order.is_empty()) {
		sorter.sort(order.ptr(), order.size());
	}
	// Spread the picks: strongest first, but not next to a picked tile; then the rest by strength.
	LocalVector<int> picked;
	LocalVector<uint8_t> taken;
	taken.resize(tiles.size());
	memset(taken.ptr(), 0, taken.size());
	for (int pass = 0; pass < 2 && picked.size() < emitters.size(); pass++) {
		for (uint32_t k = 0; k < order.size() && picked.size() < emitters.size(); k++) {
			const int ti = order[k];
			if (taken[ti] == 1) {
				continue;
			}
			const int x = ti % tx, z = ti / tx;
			if (pass == 0) {
				bool near = false;
				for (int dz = -1; dz <= 1 && !near; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nxx = x + dx, nzz = z + dz;
						if (nxx >= 0 && nzz >= 0 && nxx < tx && nzz < tz && taken[nzz * tx + nxx] == 1) {
							near = true;
							break;
						}
					}
				}
				if (near) {
					continue;
				}
			}
			taken[ti] = 1;
			picked.push_back(ti);
		}
	}
	// Crowns: lit by the surface fire's intensity near their stems (the strongest tile within reach) or by a burning
	// neighbour.
	LocalVector<PhysXFirebrands::Plume> plumes;
	const auto tile_intensity = [&](const Vector3 &p_at, float p_reach) {
		const Vector3 l = get_global_transform().affine_inverse().xform(p_at);
		const int x0 = int((l.x - p_reach + size.x * 0.5f) / tile_w), x1 = int((l.x + p_reach + size.x * 0.5f) / tile_w);
		const int z0 = int((l.z - p_reach + size.y * 0.5f) / tile_w), z1 = int((l.z + p_reach + size.y * 0.5f) / tile_w);
		float best = 0.0f;
		for (int z = MAX(z0, 0); z <= MIN(z1, tz - 1); z++) {
			for (int x = MAX(x0, 0); x <= MIN(x1, tx - 1); x++) {
				best = MAX(best, tiles[z * tx + x].heat / 1000.0f / tile_w);
			}
		}
		return best;
	};
	_update_trees(float(get_physics_process_delta_time()), p_wind, tile_intensity, plumes);
	float crown_width = 0.0f;
	LocalVector<uint32_t> crown_em;
	for (uint32_t k = 0; k < trees.size(); k++) {
		// Flaming while it gives off a twentieth of its peak; the last of it glows (fire_state_map).
		if (trees[k].hrr > 0.05f * trees[k].fuel * HEAT_CONTENT / (CROWN_TIME * Math::E)) {
			crown_em.push_back(k);
			crown_width += 2.0f * trees[k].radius;
		}
	}
	struct Hotter {
		const LocalVector<Tree> *trees;
		bool operator()(uint32_t a, uint32_t b) const { return (*trees)[a].hrr > (*trees)[b].hrr; }
	};
	if (crown_em.size() > 1) {
		SortArray<uint32_t, Hotter> hs;
		hs.compare.trees = &trees;
		hs.sort(crown_em.ptr(), crown_em.size());
	}
	// Burning crowns take up to half the emitters, the hottest first.
	const uint32_t n_crown = MIN(crown_em.size(), emitters.size() / 2 + (emitters.size() == 1 ? 1 : 0));
	// Fire whirl sites: strong flaming tiles on a flank, where the front runs along the wind (its normal across it)
	// and the wind's shear against the plume's indraft spins up vertical vorticity.
	LocalVector<Vector3> flank;
	LocalVector<float> flank_q;
	if (fire_whirls && p_wind.length() > 1.0f) {
		const Vector3 wd = p_wind.normalized();
		for (uint32_t k = 0; k < order.size() && k < 32; k++) {
			const Tile &t = tiles[order[k]];
			const Vector3 c = t.centre / t.weight;
			int cx, cz;
			if (!_world_cell(c, cx, cz) || cx < 1 || cz < 1 || cx >= nx - 1 || cz >= nz - 1) {
				continue;
			}
			const Vector3 n(phi[cz * nx + cx + 1] - phi[cz * nx + cx - 1], 0.0f, phi[(cz + 1) * nx + cx] - phi[(cz - 1) * nx + cx]);
			if (n.length_squared() < 1.0e-8f) {
				continue;
			}
			if (Math::abs(n.normalized().dot(wd)) < 0.35f) {
				flank.push_back(c);
				flank_q.push_back(t.heat / 1000.0f / tile_w);
			}
		}
	}
	// Lee-slope sites: strong flaming tiles on ground steeper than 20 degrees falling away within 40 degrees of
	// downwind, in a wind over 5.5 m/s. Stored as pairs: the place, then the contour direction (across the slope).
	LocalVector<Vector3> lee;
	if (fire_whirls && p_wind.length() > 5.5f) {
		const Vector3 wd = Vector3(p_wind.x, 0.0f, p_wind.z).normalized();
		for (uint32_t k = 0; k < order.size() && k < 32; k++) {
			const Tile &t = tiles[order[k]];
			const Vector3 c = t.centre / t.weight;
			int cx, cz;
			if (!_world_cell(c, cx, cz) || cx < 1 || cz < 1 || cx >= nx - 1 || cz >= nz - 1 || t.heat / 1000.0f / tile_w < 300.0f) {
				continue;
			}
			const Vector3 a = _cell_world(cx - 1, cz), b = _cell_world(cx + 1, cz), d = _cell_world(cx, cz - 1), e = _cell_world(cx, cz + 1);
			const Vector2 grad((b.y - a.y) / MAX(b.x - a.x, 1.0e-4f), (e.y - d.y) / MAX(e.z - d.z, 1.0e-4f));
			if (grad.length() < 0.364f) { // tan 20
				continue;
			}
			const Vector3 down = Vector3(-grad.x, 0.0f, -grad.y).normalized();
			if (down.dot(wd) < 0.766f) { // cos 40
				continue;
			}
			lee.push_back(c);
			lee.push_back(Vector3(0.0f, 1.0f, 0.0f).cross(down).normalized());
		}
	}
	_update_whirls(float(get_physics_process_delta_time()), p_wind, flank, flank_q, lee, plumes);
	// The convection columns over the strongest stretches of front, for the firebrands.
	if (spotting) {
		const float t_air = _air_temperature();
		for (uint32_t k = 0; k < order.size() && k < 128; k++) {
			const Tile &t = tiles[order[k]];
			PhysXFirebrands::Plume pl;
			pl.base = t.centre / t.weight;
			pl.w0 = PhysXFirebrands::plume_speed(t.heat / 1000.0f / tile_w, t_air);
			pl.width = tile_w;
			plumes.push_back(pl);
		}
		// Flaming tiles lie along the front, a tile or two deep: their count over the depth is about its length.
		firebrands.set_plumes(plumes, MAX(order.size() * tile_w * 0.5f, crown_width));
	}
	const Vector3 up = get_global_transform().basis.get_column(1).normalized();
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *em = emitters[k];
		if (k < n_crown) {
			// A flaring crown: the gas fills the crown and burns through it, fed by its heat release against the
			// peak of its burn (fuel / (T e)).
			const Tree &tr = trees[crown_em[k]];
			const float depth = tr.top - tr.base;
			const float peak = tr.fuel * HEAT_CONTENT / (CROWN_TIME * Math::E);
			em->set_enabled(true);
			em->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
			em->set_radius(CLAMP(MIN(tr.radius, 0.5f * depth), 0.3f, 4.0f));
			em->set_global_position(tr.pos + up * (tr.base + (tr.conifer ? 0.3f : 0.45f) * depth));
			em->set_temperature(1.0f);
			em->set_fuel(flame_strength * CLAMP(tr.hrr / MAX(peak, 1.0f), 0.0f, 1.0f));
			em->set_burn(0.0f);
			em->set_smoke(0.0f);
			em->set_velocity(p_wind * 0.4f + up * CLAMP(0.25f * Math::sqrt(9.8f * tr.top), 1.0f, 4.0f));
			em->set_couple_rate(2.0f);
			continue;
		}
		if (k - n_crown >= picked.size()) {
			em->set_enabled(false);
			continue;
		}
		const Tile &t = tiles[picked[k - n_crown]];
		const Vector3 c = t.centre / t.weight;
		// Byram's fireline intensity across the tile's width, kW/m, and the flame length it gives.
		const float intensity = t.heat / 1000.0f / tile_w;
		const float flame = CLAMP(0.0775f * Math::pow(intensity, 0.46f), 0.1f, 6.0f);
		const float s = flame_strength * CLAMP(intensity / 300.0f, 0.15f, 1.0f);
		em->set_enabled(true);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
		em->set_radius(0.6f * tile_w);
		em->set_global_position(c + Vector3(0, 0.15f, 0));
		em->set_temperature(1.0f);
		em->set_fuel(s);
		em->set_burn(0.0f);
		em->set_smoke(0.0f);
		// Rising at a share of the flame's buoyant speed and leaning with the wind.
		em->set_velocity(p_wind * 0.4f + Vector3(0, CLAMP(flame * 0.6f, 0.3f, 1.2f), 0));
		em->set_couple_rate(2.0f);
	}
	// The Flow's own ambient wind carries the weather's wind over the whole plume (docs/ideas/wind.md); the fire's
	// box only pushes its wind when that is off, or when the fire has a wind of its own (use_weather_wind off).
	bool ambient = false;
	if (flow) {
		if (!use_weather_wind) {
			flow->set_weather_wind(false);
		}
		ambient = flow->is_weather_wind() && WorldContracts::weather()->has_provider();
	}
	if (air) {
		air->set_enabled(!ambient && (!order.is_empty() || n_crown > 0) && p_wind.length_squared() > 1.0e-4f);
		air->set_velocity(p_wind);
		air->set_global_position(get_global_position() + Vector3(0, 2.0f, 0));
		air->set_size(Vector3(size.x, 4.0f, size.y));
	}
}

// Char (albedo, alpha: how burnt) and glow (emission: the burning band and smouldering cells) maps, a few times a second.
void PhysXFireSpread3D::_update_maps() {
	if (char_image.is_null() || band_x1 < band_x0) {
		return;
	}
	const FuelModel &m = _model(fuel_type);
	const float h = cell_size;
	for (int z = band_z0; z <= band_z1; z++) {
		for (int x = band_x0; x <= band_x1; x++) {
			const uint32_t i = z * nx + x;
			// Coverage of the cell by the burnt side, from the level set's distance to the front, so the edge is
			// smooth under the texture's filtering instead of stepping cell by cell.
			// The edge of a real burn is ragged at the scale of tussocks and gaps, finer than the front the grid can
			// carry: tufts just inside the front's line are left standing, by a noise fixed in world space.
			float edge = 0.0f;
			if (fuel_patchiness > 0.0f && phi[i] > -1.0f) {
				const Vector3 wp = _cell_world(x, z);
				edge = fuel_patchiness * 0.9f * (0.6f * _fuel_noise(wp.x / 0.45f, wp.z / 0.45f, 5u) + 0.4f * _fuel_noise(wp.x / 1.1f, wp.z / 1.1f, 6u));
			}
			const float cover = CLAMP(0.5f - (phi[i] + edge) / h, 0.0f, 1.0f);
			if (cover <= 0.0f) {
				continue;
			}
			// Per-cell variation (patchy fuel burns unevenly): a hash, steady over time.
			uint32_t hs = (uint32_t(x) * 73856093u) ^ (uint32_t(z) * 19349663u);
			hs = (hs ^ (hs >> 13)) * 0x5bd1e995u;
			const float jitter = float((hs ^ (hs >> 15)) & 0xffffu) / 65535.0f;
			const float burnt = t_ignite[i] < 0.0f ? 0.0f : 1.0f - fuel_left[i];
			// Charred fuel and scorched ground: near-black, a little warm (char's albedo is about 0.03-0.05). The
			// front's edge is scorched at once, the rest darkens as the fuel burns down.
			const float a = char_darkness * cover * MIN(0.35f + burnt * (1.2f + 0.4f * jitter), 1.0f);
			// Under a burnt crown the needles and twigs it dropped lie as white-grey ash over the char, patchily
			// (mineral ash's albedo is about 0.3-0.5; Roy et al. 2010).
			float ash = 0.0f;
			if (!crown_char.is_empty() && crown_char[i] > 0.0f) {
				// Drifts and bare patches at the scale of the fallen needle mats, a finer speckle on top.
				const Vector3 wp = _cell_world(x, z);
				const float mats = 0.65f * _fuel_noise(wp.x / 0.7f, wp.z / 0.7f, 7u) + 0.35f * _fuel_noise(wp.x / 2.2f, wp.z / 2.2f, 8u);
				ash = CLAMP(crown_char[i] * 1.6f - 0.6f + 0.9f * (mats - 0.5f), 0.0f, 1.0f) * (0.3f + 0.55f * jitter) * MIN(burnt * 1.5f, 1.0f);
				// Thinner where the wind has blown it off, the char showing through.
				const float made = _ash_made(i);
				ash *= made > 0.0f ? CLAMP(1.0f - ash_blown[i] / made, 0.0f, 1.0f) : 1.0f;
			}
			char_image->set_pixel(x, z, Color(Math::lerp(0.035f, 0.36f, ash), Math::lerp(0.03f, 0.35f, ash), Math::lerp(0.027f, 0.33f, ash), a));
			// Glow: the flaming band (heat release falls as exp(-age)), then embers that die within a few burn times,
			// unevenly.
			const float age = t_ignite[i] < 0.0f ? 0.0f : (time - t_ignite[i]) / m.burn_time;
			// Put out by rain: no flame, a few wet embers.
			const float g = cover * CLAMP(Math::exp(-age * (1.2f + 1.6f * jitter)), 0.0f, 1.0f) * (quenched[i] ? 0.05f : 1.0f);
			// The ground's own glow (the decal); plants and wood read the state map's, which adds the logs'.
			float wood_g = g;
			if (heavy_any && heavy_t[i] >= 0.0f && heavy_left[i] > 0.0f) {
				// A burning log glows over its cells: fully while it flames, then its embers dim as it smoulders
				// away, patchily; a wet one barely.
				const float hg = _heavy_flaming(i) ? 1.0f : (0.25f + 0.5f * jitter) * Math::sqrt(heavy_left[i] / HEAVY_SMOULDER);
				wood_g = MAX(g, cover * MIN(hg, 1.0f) * (quenched[i] ? 0.15f : 1.0f));
			}
			// A blackbody near 1,000 K (linear sRGB, as Flow's heat ramp), yellower while flaming.
			glow_image->set_pixel(x, z, Color(1.0f * g, 0.32f * g * g, 0.06f * g * g * g, 1.0f));
			if (!crown_char.is_empty()) {
				wood_g = MAX(wood_g, crown_glow[i]);
			}
			state_image->set_pixel(x, z, Color(MAX(cover * MAX(burnt, 0.3f), crown_char.is_empty() ? 0.0f : crown_char[i]), wood_g, Math::sqrt(MIN(peak_flame[i] / 32.0f, 1.0f)), Math::sqrt(MIN(peak_scorch[i] / 48.0f, 1.0f))));
		}
	}
	char_texture->update(char_image);
	glow_texture->update(glow_image);
	state_texture->update(state_image);
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 o = get_global_transform().xform(Vector3(-size.x * 0.5f, 0, -size.y * 0.5f));
	rs->global_shader_parameter_set("fire_char_map", char_texture);
	rs->global_shader_parameter_set("fire_state_map", state_texture);
	rs->global_shader_parameter_set("fire_char_rect", Color(o.x, o.z, size.x, size.y));
}

void PhysXFireSpread3D::_build_flow() {
	if (flow) {
		return;
	}
	if (!flow_path.is_empty()) {
		flow = Object::cast_to<PhysXFlow3D>(get_node_or_null(flow_path));
	}
	if (!flow) {
		flow = memnew(PhysXFlow3D);
		flow->set_name("Flow");
		// A grass fire: 10 cm cells (flames are tens of centimetres to metres), combustion on, thin grey smoke, the
		// ground as the floor. Set as the campfire showcase is: the fuel bed's gas at the top of the heat ramp
		// (temperature_range 1, a sooty flame's ~1,500 K core; Flow's default of 2 puts it mid-ramp, about a
		// thousandth as bright by Wien's law), fast-rising gas that cools out of its glow in under half a second of
		// rise, so flames reach about a metre, Byram's length for a grass fire's front (the campfire's 7 gives its
		// 0.7 m).
		flow->set_cell_size(0.1f);
		flow->set_max_blocks(2048);
		flow->set_combustion(true);
		flow->set_temperature_range(1.0f);
		flow->set_emission_strength(8.0f);
		flow->set_buoyancy(4.0f);
		flow->set_cooling_rate(6.0f);
		flow->set_vorticity(2.0f);
		flow->set_smoke_density(0.8f);
		flow->set_smoke_buoyancy(0.5f);
		flow->set_terrain_floor(true);
		add_child(flow, false, INTERNAL_MODE_FRONT);
		flow->set_as_top_level(true);
		flow->set_global_transform(Transform3D());
		own_flow = true;
	}
	air = memnew(PhysXFlowEmitter3D);
	air->set_name("FireWind");
	air->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
	air->set_smoke(0.0f);
	air->set_temperature(0.0f);
	air->set_couple_rate(0.5f);
	air->set_enabled(false);
	flow->add_child(air);
	air->set_flow_path(air->get_path_to(flow));
	set_max_emitters(max_emitters);
}

void PhysXFireSpread3D::_build_decal() {
	if (decal) {
		return;
	}
	decal = memnew(Decal);
	decal->set_name("Char");
	decal->set_size(Vector3(size.x, 6.0f, size.y));
	decal->set_texture(Decal::TEXTURE_ALBEDO, char_texture);
	decal->set_texture(Decal::TEXTURE_EMISSION, glow_texture);
	decal->set_emission_energy(glow_energy);
	decal->set_albedo_mix(1.0f);
	decal->set_upper_fade(0.05f);
	decal->set_lower_fade(0.05f);
	// Ground only: tree trunks, rocks and walls standing in the box would take the char and glow down their sides
	// (plants char by fire_state_map instead).
	decal->set_normal_fade(0.6f);
	add_child(decal, false, INTERNAL_MODE_FRONT);
}

void PhysXFireSpread3D::set_max_emitters(int p_v) {
	max_emitters = CLAMP(p_v, 1, 128);
	if (!flow) {
		return;
	}
	while ((int)emitters.size() > max_emitters) {
		emitters[emitters.size() - 1]->queue_free();
		emitters.resize(emitters.size() - 1);
	}
	while ((int)emitters.size() < max_emitters) {
		PhysXFlowEmitter3D *em = memnew(PhysXFlowEmitter3D);
		em->set_name(vformat("Flame%d", (int)emitters.size()));
		em->set_enabled(false);
		flow->add_child(em);
		em->set_flow_path(em->get_path_to(flow));
		emitters.push_back(em);
	}
}

void PhysXFireSpread3D::set_size(const Vector2 &p_size) {
	size = Vector2(MAX(p_size.x, 0.5f), MAX(p_size.y, 0.5f));
	grid_dirty = true;
	if (decal) {
		decal->set_size(Vector3(size.x, 6.0f, size.y));
	}
}

void PhysXFireSpread3D::set_cell_size(float p_v) {
	cell_size = CLAMP(p_v, 0.05f, 4.0f);
	grid_dirty = true;
}

void PhysXFireSpread3D::set_fuel_type(FuelType p_type) {
	fuel_type = p_type;
	grid_dirty = true;
}

void PhysXFireSpread3D::set_fuel_load_scale(float p_v) {
	fuel_load_scale = MAX(p_v, 0.0f);
	grid_dirty = true;
}

void PhysXFireSpread3D::set_fuel_map(const Ref<Image> &p_map) {
	fuel_map = p_map;
	grid_dirty = true;
}

void PhysXFireSpread3D::set_fuel_patchiness(float p_v) {
	fuel_patchiness = CLAMP(p_v, 0.0f, 1.0f);
	grid_dirty = true;
}

void PhysXFireSpread3D::set_vegetation_field(const NodePath &p_path) {
	vegetation_field = p_path;
	grid_dirty = true;
}

void PhysXFireSpread3D::set_spotting(bool p_v) {
	spotting = p_v;
	if (!spotting) {
		firebrands.clear();
	}
}

void PhysXFireSpread3D::set_glow_energy(float p_v) {
	glow_energy = MAX(p_v, 0.0f);
	if (decal) {
		decal->set_emission_energy(glow_energy);
	}
}

float PhysXFireSpread3D::get_char_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || fuel_left.is_empty()) {
		return 0.0f;
	}
	const uint32_t i = z * nx + x;
	return t_ignite[i] < 0.0f ? 0.0f : 1.0f - fuel_left[i];
}

float PhysXFireSpread3D::get_intensity_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || heat.is_empty()) {
		return 0.0f;
	}
	return heat[z * nx + x] / 1000.0f;
}

float PhysXFireSpread3D::get_flame_length_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || peak_flame.is_empty()) {
		return 0.0f;
	}
	return peak_flame[z * nx + x];
}

float PhysXFireSpread3D::get_scorch_height_at(const Vector3 &p_position) const {
	int x, z;
	if (!_world_cell(p_position, x, z) || peak_scorch.is_empty()) {
		return 0.0f;
	}
	return peak_scorch[z * nx + x];
}

int PhysXFireSpread3D::get_burning_cells() const {
	int n = 0;
	// Flaming: lit, with more than 5% of the fuel left (about three burn times); the tail after that is embers.
	for (uint32_t i = 0; i < heat.size(); i++) {
		n += (heat[i] > 0.0f && fuel_left[i] > 0.05f) ? 1 : 0;
	}
	return n;
}

LocalVector<PhysXFireSpread3D *> PhysXFireSpread3D::fires;

PhysXFireSpread3D *PhysXFireSpread3D::find_at(const Vector3 &p_position) {
	for (PhysXFireSpread3D *f : fires) {
		int x, z;
		if (f->is_inside_tree() && !f->height.is_empty() && f->_world_cell(p_position, x, z)) {
			return f;
		}
	}
	return nullptr;
}

void PhysXFireSpread3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!fires.has(this)) {
				fires.push_back(this);
			}
			WorldContracts::register_fire(this);
#ifdef MODULE_WEATHER_ENABLED
			lightning_seen = WeatherLightning3D::get_last_ground_strike_serial(); // strikes from before it entered do not count
#endif
		} break;
		case NOTIFICATION_EXIT_TREE: {
			fires.erase(this);
			WorldContracts::unregister_fire(this);
			steam.clear();
			ash.clear();
			ash_sources.clear();
		} break;
		case NOTIFICATION_READY: {
			// Once per run, unless the project declares them (global_shader_parameter_get_list is editor only).
			static bool globals_added = false;
			if (!globals_added) {
				globals_added = true;
				RenderingServer *rs = RenderingServer::get_singleton();
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_char_map")) {
					rs->global_shader_parameter_add("fire_char_map", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, Variant());
				}
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_state_map")) {
					rs->global_shader_parameter_add("fire_state_map", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, Variant());
				}
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_char_rect")) {
					rs->global_shader_parameter_add("fire_char_rect", RSE::GLOBAL_VAR_TYPE_VEC4, Color());
				}
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_wind_map")) {
					rs->global_shader_parameter_add("fire_wind_map", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, Variant());
				}
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_wind_rect")) {
					rs->global_shader_parameter_add("fire_wind_rect", RSE::GLOBAL_VAR_TYPE_VEC4, Color());
				}
			}
			if (grid_dirty) {
				_allocate();
			}
			_build_decal();
			_build_flow();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const float dt = float(get_physics_process_delta_time());
			sample_timer -= dt;
			if (sample_timer <= 0.0f) {
				sample_timer = 0.25f;
				_ignite_from_heat_source();
				_apply_fluid_water(0.25f, _update_moisture(0.25f));
				_water_edge(0.25f);
				_ignite_from_lightning();
				_blow_ash(0.25f);
				_update_indraft();
			}
			_step(dt);
			_update_emitters(_wind());
			if (spotting && brand_count > 0.0f) {
				firebrands.release(brand_count, brand_points, flow);
			}
			if (steam.is_steaming() || steam.get_total_kg() > 0.0f) {
				steam.step(this, _wind(), dt);
			}
			if (!ash_sources.is_empty() || ash.is_active()) {
				ash.step(this, ash_sources, _wind(), dt);
			}
			if (firebrands.get_count() > 0) {
				firebrands.step(this, flow, _wind(), dt, glow_energy);
			}
			map_timer -= dt;
			if (map_timer <= 0.0f) {
				map_timer = 0.1f;
				_update_maps();
			}
		} break;
	}
}

void PhysXFireSpread3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("ignite", "position", "radius"), &PhysXFireSpread3D::ignite, DEFVAL(0.5f));
	ClassDB::bind_method(D_METHOD("extinguish"), &PhysXFireSpread3D::extinguish);
	ClassDB::bind_method(D_METHOD("get_char_at", "position"), &PhysXFireSpread3D::get_char_at);
	ClassDB::bind_method(D_METHOD("get_intensity_at", "position"), &PhysXFireSpread3D::get_intensity_at);
	ClassDB::bind_method(D_METHOD("get_flame_length_at", "position"), &PhysXFireSpread3D::get_flame_length_at);
	ClassDB::bind_method(D_METHOD("get_scorch_height_at", "position"), &PhysXFireSpread3D::get_scorch_height_at);
	ClassDB::bind_method(D_METHOD("get_burnt_area"), &PhysXFireSpread3D::get_burnt_area);
	ClassDB::bind_method(D_METHOD("get_burning_cells"), &PhysXFireSpread3D::get_burning_cells);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXFireSpread3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_firebrand_count"), &PhysXFireSpread3D::get_firebrand_count);
	ClassDB::bind_method(D_METHOD("get_spot_fire_count"), &PhysXFireSpread3D::get_spot_fire_count);
	ClassDB::bind_method(D_METHOD("get_doused_cells"), &PhysXFireSpread3D::get_doused_cells);
	ClassDB::bind_method(D_METHOD("get_steam_mass"), &PhysXFireSpread3D::get_steam_mass);
	ClassDB::bind_method(D_METHOD("get_whirl_count"), &PhysXFireSpread3D::get_whirl_count);
	ClassDB::bind_method(D_METHOD("get_whirls_spawned"), &PhysXFireSpread3D::get_whirls_spawned);
	ClassDB::bind_method(D_METHOD("get_lee_whirls_spawned"), &PhysXFireSpread3D::get_lee_whirls_spawned);
	ClassDB::bind_method(D_METHOD("set_fire_wind", "enabled"), &PhysXFireSpread3D::set_fire_wind);
	ClassDB::bind_method(D_METHOD("is_fire_wind"), &PhysXFireSpread3D::is_fire_wind);
	ClassDB::bind_method(D_METHOD("get_fire_wind_at", "position"), &PhysXFireSpread3D::get_fire_wind_at);
	ClassDB::bind_method(D_METHOD("get_stem_char_at", "position"), &PhysXFireSpread3D::get_stem_char_at);
	ClassDB::bind_method(D_METHOD("set_blowing_ash", "enabled"), &PhysXFireSpread3D::set_blowing_ash);
	ClassDB::bind_method(D_METHOD("is_blowing_ash"), &PhysXFireSpread3D::is_blowing_ash);
	ClassDB::bind_method(D_METHOD("get_ash_blown"), &PhysXFireSpread3D::get_ash_blown);
	ClassDB::bind_method(D_METHOD("set_fire_whirls", "enabled"), &PhysXFireSpread3D::set_fire_whirls);
	ClassDB::bind_method(D_METHOD("is_fire_whirls"), &PhysXFireSpread3D::is_fire_whirls);
	ClassDB::bind_method(D_METHOD("set_spotting", "enabled"), &PhysXFireSpread3D::set_spotting);
	ClassDB::bind_method(D_METHOD("is_spotting"), &PhysXFireSpread3D::is_spotting);
	ClassDB::bind_method(D_METHOD("set_spotting_scale", "scale"), &PhysXFireSpread3D::set_spotting_scale);
	ClassDB::bind_method(D_METHOD("get_spotting_scale"), &PhysXFireSpread3D::get_spotting_scale);

	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXFireSpread3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXFireSpread3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PhysXFireSpread3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PhysXFireSpread3D::get_size);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &PhysXFireSpread3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &PhysXFireSpread3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_fuel_type", "type"), &PhysXFireSpread3D::set_fuel_type);
	ClassDB::bind_method(D_METHOD("get_fuel_type"), &PhysXFireSpread3D::get_fuel_type);
	ClassDB::bind_method(D_METHOD("set_fuel_moisture", "moisture"), &PhysXFireSpread3D::set_fuel_moisture);
	ClassDB::bind_method(D_METHOD("get_fuel_moisture"), &PhysXFireSpread3D::get_fuel_moisture);
	ClassDB::bind_method(D_METHOD("get_moisture_at", "position"), &PhysXFireSpread3D::get_moisture_at);
	ClassDB::bind_method(D_METHOD("get_heavy_fuel_at", "position"), &PhysXFireSpread3D::get_heavy_fuel_at);
	ClassDB::bind_method(D_METHOD("set_crown_fire", "enabled"), &PhysXFireSpread3D::set_crown_fire);
	ClassDB::bind_method(D_METHOD("is_crown_fire"), &PhysXFireSpread3D::is_crown_fire);
	ClassDB::bind_method(D_METHOD("set_foliar_moisture", "moisture"), &PhysXFireSpread3D::set_foliar_moisture);
	ClassDB::bind_method(D_METHOD("get_foliar_moisture"), &PhysXFireSpread3D::get_foliar_moisture);
	ClassDB::bind_method(D_METHOD("add_tree", "position", "height", "crown_base", "crown_radius", "conifer"), &PhysXFireSpread3D::add_tree, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("clear_trees"), &PhysXFireSpread3D::clear_trees);
	ClassDB::bind_method(D_METHOD("get_tree_count"), &PhysXFireSpread3D::get_tree_count);
	ClassDB::bind_method(D_METHOD("get_crowns_lit"), &PhysXFireSpread3D::get_crowns_lit);
	ClassDB::bind_method(D_METHOD("get_crowns_burning"), &PhysXFireSpread3D::get_crowns_burning);
	ClassDB::bind_method(D_METHOD("get_tree_burnt", "index"), &PhysXFireSpread3D::get_tree_burnt);
	ClassDB::bind_method(D_METHOD("get_moisture_of_extinction"), &PhysXFireSpread3D::get_moisture_of_extinction);
	ClassDB::bind_method(D_METHOD("get_ground_height_at", "position"), &PhysXFireSpread3D::get_ground_height_at);
	ClassDB::bind_method(D_METHOD("set_use_weather_moisture", "enable"), &PhysXFireSpread3D::set_use_weather_moisture);
	ClassDB::bind_method(D_METHOD("is_using_weather_moisture"), &PhysXFireSpread3D::is_using_weather_moisture);
	ClassDB::bind_method(D_METHOD("set_moisture_timelag", "seconds"), &PhysXFireSpread3D::set_moisture_timelag);
	ClassDB::bind_method(D_METHOD("get_moisture_timelag"), &PhysXFireSpread3D::get_moisture_timelag);
	ClassDB::bind_method(D_METHOD("set_fuel_load_scale", "scale"), &PhysXFireSpread3D::set_fuel_load_scale);
	ClassDB::bind_method(D_METHOD("get_fuel_load_scale"), &PhysXFireSpread3D::get_fuel_load_scale);
	ClassDB::bind_method(D_METHOD("set_fuel_map", "map"), &PhysXFireSpread3D::set_fuel_map);
	ClassDB::bind_method(D_METHOD("get_fuel_map"), &PhysXFireSpread3D::get_fuel_map);
	ClassDB::bind_method(D_METHOD("set_fuel_patchiness", "patchiness"), &PhysXFireSpread3D::set_fuel_patchiness);
	ClassDB::bind_method(D_METHOD("get_fuel_patchiness"), &PhysXFireSpread3D::get_fuel_patchiness);
	ClassDB::bind_method(D_METHOD("set_vegetation_field", "path"), &PhysXFireSpread3D::set_vegetation_field);
	ClassDB::bind_method(D_METHOD("get_vegetation_field"), &PhysXFireSpread3D::get_vegetation_field);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXFireSpread3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXFireSpread3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_use_weather_wind", "enabled"), &PhysXFireSpread3D::set_use_weather_wind);
	ClassDB::bind_method(D_METHOD("set_lightning_ignition", "enabled"), &PhysXFireSpread3D::set_lightning_ignition);
	ClassDB::bind_method(D_METHOD("is_lightning_ignition"), &PhysXFireSpread3D::is_lightning_ignition);
	ClassDB::bind_method(D_METHOD("get_lightning_ignition_count"), &PhysXFireSpread3D::get_lightning_ignition_count);
	ClassDB::bind_static_method("PhysXFireSpread3D", D_METHOD("lightning_ignition_chance", "continuing", "moisture", "extinction"), &PhysXFireSpread3D::lightning_ignition_chance);
	ClassDB::bind_method(D_METHOD("is_using_weather_wind"), &PhysXFireSpread3D::is_using_weather_wind);
	ClassDB::bind_method(D_METHOD("set_spread_scale", "scale"), &PhysXFireSpread3D::set_spread_scale);
	ClassDB::bind_method(D_METHOD("get_spread_scale"), &PhysXFireSpread3D::get_spread_scale);
	ClassDB::bind_method(D_METHOD("set_heat_source", "path"), &PhysXFireSpread3D::set_heat_source);
	ClassDB::bind_method(D_METHOD("get_heat_source"), &PhysXFireSpread3D::get_heat_source);
	ClassDB::bind_method(D_METHOD("set_ignition_temperature", "temperature"), &PhysXFireSpread3D::set_ignition_temperature);
	ClassDB::bind_method(D_METHOD("get_ignition_temperature"), &PhysXFireSpread3D::get_ignition_temperature);
	ClassDB::bind_method(D_METHOD("set_flow_path", "path"), &PhysXFireSpread3D::set_flow_path);
	ClassDB::bind_method(D_METHOD("get_flow_path"), &PhysXFireSpread3D::get_flow_path);
	ClassDB::bind_method(D_METHOD("set_max_emitters", "count"), &PhysXFireSpread3D::set_max_emitters);
	ClassDB::bind_method(D_METHOD("get_max_emitters"), &PhysXFireSpread3D::get_max_emitters);
	ClassDB::bind_method(D_METHOD("set_flame_strength", "strength"), &PhysXFireSpread3D::set_flame_strength);
	ClassDB::bind_method(D_METHOD("get_flame_strength"), &PhysXFireSpread3D::get_flame_strength);
	ClassDB::bind_method(D_METHOD("set_char_darkness", "darkness"), &PhysXFireSpread3D::set_char_darkness);
	ClassDB::bind_method(D_METHOD("get_char_darkness"), &PhysXFireSpread3D::get_char_darkness);
	ClassDB::bind_method(D_METHOD("set_glow_energy", "energy"), &PhysXFireSpread3D::set_glow_energy);
	ClassDB::bind_method(D_METHOD("get_glow_energy"), &PhysXFireSpread3D::get_glow_energy);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.05,4,0.01,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_GROUP("Fuel", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fuel_type", PROPERTY_HINT_ENUM, "Short Grass,Tall Grass,Litter"), "set_fuel_type", "get_fuel_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fuel_moisture", PROPERTY_HINT_RANGE, "0,0.5,0.005"), "set_fuel_moisture", "get_fuel_moisture");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_weather_moisture"), "set_use_weather_moisture", "is_using_weather_moisture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moisture_timelag", PROPERTY_HINT_RANGE, "1,36000,1,or_greater,suffix:s"), "set_moisture_timelag", "get_moisture_timelag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fuel_load_scale", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_fuel_load_scale", "get_fuel_load_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fuel_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_fuel_map", "get_fuel_map");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fuel_patchiness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_fuel_patchiness", "get_fuel_patchiness");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "vegetation_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_vegetation_field", "get_vegetation_field");
	ADD_GROUP("Spread", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_weather_wind"), "set_use_weather_wind", "is_using_weather_wind");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lightning_ignition"), "set_lightning_ignition", "is_lightning_ignition");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_spread_scale", "get_spread_scale");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "heat_source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_heat_source", "get_heat_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ignition_temperature", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_ignition_temperature", "get_ignition_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "spotting"), "set_spotting", "is_spotting");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spotting_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_spotting_scale", "get_spotting_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "crown_fire"), "set_crown_fire", "is_crown_fire");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foliar_moisture", PROPERTY_HINT_RANGE, "0.3,3,0.01"), "set_foliar_moisture", "get_foliar_moisture");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fire_whirls"), "set_fire_whirls", "is_fire_whirls");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blowing_ash"), "set_blowing_ash", "is_blowing_ash");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fire_wind"), "set_fire_wind", "is_fire_wind");
	ADD_GROUP("Flames", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "flow_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_flow_path", "get_flow_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_emitters", PROPERTY_HINT_RANGE, "1,128,1"), "set_max_emitters", "get_max_emitters");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flame_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_flame_strength", "get_flame_strength");
	ADD_GROUP("Char", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "char_darkness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_char_darkness", "get_char_darkness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glow_energy", PROPERTY_HINT_RANGE, "0,32,0.1"), "set_glow_energy", "get_glow_energy");

	BIND_ENUM_CONSTANT(FUEL_SHORT_GRASS);
	BIND_ENUM_CONSTANT(FUEL_TALL_GRASS);
	BIND_ENUM_CONSTANT(FUEL_LITTER);
	BIND_ENUM_CONSTANT(FUEL_MAX);
}

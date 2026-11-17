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

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/templates/sort_array.h"
#include "scene/3d/decal.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

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
	const FuelModel &m = _model(fuel_type);
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const uint32_t i = z * nx + x;
			phi[i] = FAR;
			t_ignite[i] = -1.0f;
			float l = m.load * fuel_load_scale;
			if (fuel_map.is_valid() && !fuel_map->is_empty()) {
				const int px = CLAMP(int((x + 0.5f) / nx * fuel_map->get_width()), 0, fuel_map->get_width() - 1);
				const int pz = CLAMP(int((z + 0.5f) / nz * fuel_map->get_height()), 0, fuel_map->get_height() - 1);
				l *= fuel_map->get_pixel(px, pz).r;
			}
			load[i] = l;
			height[i] = get_global_position().y;
			fuel_left[i] = 1.0f;
			heat[i] = 0.0f;
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
	if (decal) {
		decal->set_texture(Decal::TEXTURE_ALBEDO, char_texture);
		decal->set_texture(Decal::TEXTURE_EMISSION, glow_texture);
	}
}

Vector3 PhysXFireSpread3D::_cell_world(int p_x, int p_z) const {
	const Vector3 local((p_x + 0.5f) * cell_size - size.x * 0.5f, 0.0f, (p_z + 0.5f) * cell_size - size.y * 0.5f);
	Vector3 w = get_global_transform().xform(local);
	if (!height.is_empty()) {
		w.y = height[p_z * nx + p_x];
	}
	return w;
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
#ifdef MODULE_WEATHER_ENABLED
	if (use_weather_wind) {
		if (const WeatherMap3D *wm = WeatherMap3D::get_active()) {
			// Midflame height: about the fuel's depth; WRF-SFIRE takes the wind at 6.1 m and reduces it, a weather map
			// gives it near the ground directly.
			return wm->get_wind_at(get_global_position(), 1.0f);
		}
	}
#endif
	return wind;
}

// Rothermel's rate of spread along a front normal (m/s): R0 * eta_M * (1 + phi_w + phi_s), as WRF-SFIRE takes it.
float PhysXFireSpread3D::_spread_rate(int p_x, int p_z, const Vector2 &p_normal, const Vector2 &p_wind) const {
	const uint32_t i = p_z * nx + p_x;
	if (load[i] <= 1.0e-4f) {
		return 0.0f;
	}
	const FuelModel &m = _model(fuel_type);
	const float r = MIN(fuel_moisture / m.moisture_extinction, 1.0f);
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
	const float head = m.r0 * eta_m * (1.0f + phi_len) * spread_scale;
	if (phi_len < 1.0e-4f) {
		return head; // no wind or slope: a circle
	}
	// Elliptical fire shape (Anderson 1983 length-to-width from the effective midflame wind in mph; FARSITE's
	// head/back split). A level set moves each normal at the support function of that ellipse (Richards 1990, as CSIRO
	// Spark does): c cos t + sqrt(a^2 cos^2 t + b^2 sin^2 t), with a the semi-major axis, b = a / lw and c the
	// offset of its centre from the ignition point.
	const float u_eff = Math::pow(phi_len / MAX(wind_coef, 1.0e-9f), 1.0f / b) / 88.0f; // ft/min -> mph
	const float lw = MAX(0.936f * Math::exp(0.2566f * u_eff) + 0.461f * Math::exp(-0.1548f * u_eff) - 0.397f, 1.0f);
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
		const float r_max = m.r0 * (1.0f + 200.0f) * spread_scale;
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
				const float r = _spread_rate(x, z, n, w2);
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
	for (int z = band_z0; z <= band_z1; z++) {
		for (int x = band_x0; x <= band_x1; x++) {
			const uint32_t i = z * nx + x;
			heat[i] = 0.0f;
			if (t_ignite[i] < 0.0f) {
				continue;
			}
			total_burnt += cell_area;
			const float left = fuel_left[i] <= 0.0f ? 0.0f : Math::exp(-(time - t_ignite[i]) / m.burn_time);
			heat[i] = MAX(fuel_left[i] - left, 0.0f) * load[i] * HEAT_CONTENT / MAX(p_delta, 1.0e-4f);
			fuel_left[i] = left < 0.002f ? 0.0f : left;
		}
	}
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

// Flames: the burning cells merged into tiles, the strongest tiles each given an emitter: a box over the tile, as tall
// as a share of Byram's flame length (L = 0.0775 I^0.46 m, I in kW/m), fuel and heat by the tile's heat release.
void PhysXFireSpread3D::_update_emitters(const Vector3 &p_wind) {
	if (!flow) {
		return;
	}
	const int tile = MAX(1, (int)Math::round(1.0f / cell_size)); // ~1 m tiles
	const int tx = (nx + tile - 1) / tile, tz = (nz + tile - 1) / tile;
	struct Tile {
		float heat = 0.0f; // W
		Vector3 centre;
		float weight = 0.0f;
		float max_flux = 0.0f; // W/m2
	};
	LocalVector<Tile> tiles;
	tiles.resize(tx * tz);
	const float cell_area = cell_size * cell_size;
	if (band_x1 >= band_x0) {
		for (int z = band_z0; z <= band_z1; z++) {
			for (int x = band_x0; x <= band_x1; x++) {
				const float q = heat[z * nx + x];
				if (q <= 0.0f) {
					continue;
				}
				Tile &t = tiles[(z / tile) * tx + x / tile];
				t.heat += q * cell_area;
				t.centre += _cell_world(x, z) * q;
				t.weight += q;
				t.max_flux = MAX(t.max_flux, q);
			}
		}
	}
	// The strongest tiles.
	LocalVector<int> order;
	for (uint32_t i = 0; i < tiles.size(); i++) {
		if (tiles[i].heat > 1000.0f) {
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
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *em = emitters[k];
		if (k >= order.size()) {
			em->set_enabled(false);
			continue;
		}
		const Tile &t = tiles[order[k]];
		const Vector3 c = t.centre / t.weight;
		// Byram's fireline intensity across the tile's width, kW/m, and the flame length it gives.
		const float intensity = t.heat / 1000.0f / (tile * cell_size);
		const float flame = CLAMP(0.0775f * Math::pow(intensity, 0.46f), 0.1f, 6.0f);
		const float s = flame_strength * CLAMP(t.max_flux / 400000.0f, 0.1f, 1.0f);
		em->set_enabled(true);
		em->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		// A thin bed of hot fuel gas on the ground that Flow's combustion burns into flame and smoke (the smoke comes
		// from the burn); the flame's height comes from its rise and cooling.
		em->set_size(Vector3(tile * cell_size, 0.15f, tile * cell_size));
		em->set_global_position(c + Vector3(0, 0.1f, 0));
		em->set_temperature(1.0f);
		em->set_fuel(s);
		em->set_burn(0.0f);
		em->set_smoke(0.0f);
		em->set_velocity(p_wind * 0.3f + Vector3(0, CLAMP(flame * 0.5f, 0.3f, 1.0f), 0));
		em->set_couple_rate(4.0f);
	}
	if (air) {
		air->set_enabled(!order.is_empty() && p_wind.length_squared() > 1.0e-4f);
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
			const float cover = CLAMP(0.5f - phi[i] / h, 0.0f, 1.0f);
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
			char_image->set_pixel(x, z, Color(0.035f, 0.03f, 0.027f, a));
			// Glow: the flaming band (heat release falls as exp(-age)), then embers that die within a few burn times,
			// unevenly.
			const float age = t_ignite[i] < 0.0f ? 0.0f : (time - t_ignite[i]) / m.burn_time;
			const float g = cover * CLAMP(Math::exp(-age * (1.2f + 1.6f * jitter)), 0.0f, 1.0f);
			// A blackbody near 1,000 K (linear sRGB, as Flow's heat ramp), yellower while flaming.
			glow_image->set_pixel(x, z, Color(1.0f * g, 0.32f * g * g, 0.06f * g * g * g, 1.0f));
		}
	}
	char_texture->update(char_image);
	glow_texture->update(glow_image);
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 o = get_global_transform().xform(Vector3(-size.x * 0.5f, 0, -size.y * 0.5f));
	rs->global_shader_parameter_set("fire_char_map", char_texture);
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
	add_child(decal, false, INTERNAL_MODE_FRONT);
}

void PhysXFireSpread3D::set_max_emitters(int p_v) {
	max_emitters = CLAMP(p_v, 1, 64);
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

int PhysXFireSpread3D::get_burning_cells() const {
	int n = 0;
	// Flaming: lit, with more than 5% of the fuel left (about three burn times); the tail after that is embers.
	for (uint32_t i = 0; i < heat.size(); i++) {
		n += (heat[i] > 0.0f && fuel_left[i] > 0.05f) ? 1 : 0;
	}
	return n;
}

void PhysXFireSpread3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Once per run, unless the project declares them (global_shader_parameter_get_list is editor only).
			static bool globals_added = false;
			if (!globals_added) {
				globals_added = true;
				RenderingServer *rs = RenderingServer::get_singleton();
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_char_map")) {
					rs->global_shader_parameter_add("fire_char_map", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, Variant());
				}
				if (!ProjectSettings::get_singleton()->has_setting("shader_globals/fire_char_rect")) {
					rs->global_shader_parameter_add("fire_char_rect", RSE::GLOBAL_VAR_TYPE_VEC4, Color());
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
			}
			_step(dt);
			_update_emitters(_wind());
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
	ClassDB::bind_method(D_METHOD("get_burnt_area"), &PhysXFireSpread3D::get_burnt_area);
	ClassDB::bind_method(D_METHOD("get_burning_cells"), &PhysXFireSpread3D::get_burning_cells);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXFireSpread3D::get_flow);

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
	ClassDB::bind_method(D_METHOD("set_fuel_load_scale", "scale"), &PhysXFireSpread3D::set_fuel_load_scale);
	ClassDB::bind_method(D_METHOD("get_fuel_load_scale"), &PhysXFireSpread3D::get_fuel_load_scale);
	ClassDB::bind_method(D_METHOD("set_fuel_map", "map"), &PhysXFireSpread3D::set_fuel_map);
	ClassDB::bind_method(D_METHOD("get_fuel_map"), &PhysXFireSpread3D::get_fuel_map);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXFireSpread3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXFireSpread3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_use_weather_wind", "enabled"), &PhysXFireSpread3D::set_use_weather_wind);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fuel_load_scale", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_fuel_load_scale", "get_fuel_load_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fuel_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_fuel_map", "get_fuel_map");
	ADD_GROUP("Spread", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_weather_wind"), "set_use_weather_wind", "is_using_weather_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_spread_scale", "get_spread_scale");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "heat_source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_heat_source", "get_heat_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ignition_temperature", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_ignition_temperature", "get_ignition_temperature");
	ADD_GROUP("Flames", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "flow_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_flow_path", "get_flow_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_emitters", PROPERTY_HINT_RANGE, "1,64,1"), "set_max_emitters", "get_max_emitters");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flame_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_flame_strength", "get_flame_strength");
	ADD_GROUP("Char", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "char_darkness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_char_darkness", "get_char_darkness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glow_energy", PROPERTY_HINT_RANGE, "0,32,0.1"), "set_glow_energy", "get_glow_energy");

	BIND_ENUM_CONSTANT(FUEL_SHORT_GRASS);
	BIND_ENUM_CONSTANT(FUEL_TALL_GRASS);
	BIND_ENUM_CONSTANT(FUEL_LITTER);
	BIND_ENUM_CONSTANT(FUEL_MAX);
}

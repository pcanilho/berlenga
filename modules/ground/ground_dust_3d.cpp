/**************************************************************************/
/*  ground_dust_3d.cpp                                                    */
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

#include "ground_dust_3d.h"

#include "modules/world/world_contracts.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/fog_volume.h"
#include "core/math/random_pcg.h"
#include "scene/resources/material.h"

static constexpr float DUST_DENSITY = 2650.0f; // kg/m3, quartz
static constexpr float AIR_DENSITY = 1.2f;
static constexpr float AIR_VISCOSITY = 1.8e-5f; // Pa s
static constexpr float SAND_ROUGHNESS = 0.001f; // m, z0 over a sand bed

// Tiling Worley fBm (three octaves of inverted cell distance), the puff shapes of the plume's edges.
static Ref<ImageTexture3D> _worley_texture() {
	static Ref<ImageTexture3D> tex;
	if (tex.is_valid()) {
		return tex;
	}
	const int n = 48;
	const int cells[3] = { 3, 6, 12 };
	const float weight[3] = { 0.6f, 0.27f, 0.13f };
	LocalVector<float> v;
	v.resize(n * n * n);
	for (uint32_t i = 0; i < v.size(); i++) {
		v[i] = 0.0f;
	}
	for (int o = 0; o < 3; o++) {
		const int c = cells[o];
		LocalVector<Vector3> pts;
		pts.resize(c * c * c);
		RandomPCG rng(1234 + o);
		for (int i = 0; i < c * c * c; i++) {
			pts[i] = Vector3(rng.randf(), rng.randf(), rng.randf());
		}
		for (int z = 0; z < n; z++) {
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const Vector3 p = Vector3(x + 0.5f, y + 0.5f, z + 0.5f) * (float(c) / n);
					const Vector3i ci(int(p.x), int(p.y), int(p.z));
					float best = 9.0f;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dy = -1; dy <= 1; dy++) {
							for (int dx = -1; dx <= 1; dx++) {
								const Vector3i k = ci + Vector3i(dx, dy, dz);
								const Vector3i w((k.x + c) % c, (k.y + c) % c, (k.z + c) % c);
								const Vector3 f = Vector3(k) + pts[(w.z * c + w.y) * c + w.x];
								best = MIN(best, p.distance_squared_to(f));
							}
						}
					}
					v[(z * n + y) * n + x] += weight[o] * (1.0f - CLAMP(Math::sqrt(best), 0.0f, 1.0f));
				}
			}
		}
	}
	Vector<Ref<Image>> slices;
	for (int z = 0; z < n; z++) {
		Ref<Image> img = Image::create_empty(n, n, false, Image::FORMAT_L8);
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				const float f = CLAMP(v[(z * n + y) * n + x], 0.0f, 1.0f);
				img->set_pixel(x, y, Color(f, f, f));
			}
		}
		slices.push_back(img);
	}
	tex.instantiate();
	tex->create(Image::FORMAT_L8, n, n, n, false, slices);
	return tex;
}

static const char *DUST_FOG_SHADER = R"(
shader_type fog;

uniform sampler3D density_texture : filter_linear, repeat_disable;
uniform sampler3D puffs : filter_linear, repeat_enable;
uniform vec3 albedo = vec3(0.82, 0.72, 0.56);
uniform float billow = 0.9;
uniform float billow_size = 2.0;
uniform vec3 noise_offset;

void fog() {
	float ext = texture(density_texture, UVW).r;
	if (ext <= 0.0) {
		DENSITY = 0.0;
	} else {
		// Schneider 2015: the base remapped by the noise. Coverage is how solid the plume is here (its optical
		// depth over a puff); thin parts break into puffs, the core stays whole; mass kept on average.
		vec3 p = WORLD_POSITION / billow_size - noise_offset;
		float w = 0.65 * texture(puffs, p).r + 0.35 * texture(puffs, p * 2.3 + vec3(0.37, 0.11, 0.73) - noise_offset * 0.6).r;
		float coverage = 1.0 - exp(-ext * billow_size * 0.5);
		float cut = (1.0 - w) * billow;
		float shaped = clamp((coverage - cut) / max(1.0 - cut, 1e-3), 0.0, 1.0);
		DENSITY = ext * shaped / max(coverage, 1e-4) * mix(1.0, 0.3 + 1.4 * w, billow);
		// The grid's own box must not show: fade over its outer eighth at the sides and top.
		vec2 side = min(UVW.xz, 1.0 - UVW.xz);
		DENSITY *= smoothstep(0.0, 0.12, min(side.x, side.y)) * smoothstep(0.0, 0.15, 1.0 - UVW.y);
	}
	ALBEDO = albedo;
}
)";

GroundDust3D::GroundDust3D() {
	set_notify_transform(false);
}

GroundSurface3D *GroundDust3D::_surface() const {
	if (!ground_surface.is_empty()) {
		GroundSurface3D *s = Object::cast_to<GroundSurface3D>(get_node_or_null(ground_surface));
		if (s) {
			return s;
		}
	}
	return GroundSurface3D::get_active();
}

Vector3 GroundDust3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n) {
			return n->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

float GroundDust3D::get_settling_speed() const {
	const float d = grain_diameter * 1e-6f;
	return (DUST_DENSITY - AIR_DENSITY) * 9.81f * d * d / (18.0f * AIR_VISCOSITY);
}

float GroundDust3D::get_mass_extinction() const {
	// Geometric optics: extinction efficiency Q ~ 2 for grains much larger than the wavelength; the cross-section
	// per kilogram of spheres of diameter d is 3 Q / (2 rho d).
	return 3.0f * 2.0f / (2.0f * DUST_DENSITY * grain_diameter * 1e-6f);
}

void GroundDust3D::_allocate() {
	nx = MAX(int(Math::round(extent.x / cell_size)), 4);
	ny = MAX(int(Math::round(extent.y / cell_size)), 4);
	nz = MAX(int(Math::round(extent.z / cell_size)), 4);
	const int n = nx * ny * nz;
	conc.resize(n);
	tmp.resize(n);
	jet.resize(n);
	jet_tmp.resize(n);
	floor_y.resize(nx * nz);
	for (int i = 0; i < n; i++) {
		conc[i] = 0.0f;
		jet[i] = Vector3();
	}
	for (uint32_t i = 0; i < floor_y.size(); i++) {
		floor_y[i] = -1e9f;
	}
	airborne = 0.0;
	slices.clear();
	for (int y = 0; y < nz; y++) {
		slices.push_back(Image::create_empty(nx, ny, false, Image::FORMAT_RF));
	}
	density.instantiate();
	density->create(Image::FORMAT_RF, nx, ny, nz, false, slices);
	if (fog.is_valid()) {
		fog->set_shader_parameter("density_texture", density);
	}
	if (volume) {
		volume->set_size(Vector3(nx, ny, nz) * cell_size);
	}
	origin = Vector3i(INT32_MIN, 0, 0);
}

void GroundDust3D::_recenter(const Vector3 &p_center, GroundSurface3D *p_surface) {
	const float ground = p_surface ? p_surface->get_ground_height_at(p_center) : p_center.y;
	const Vector3i want(int(Math::floor(p_center.x / cell_size)) - nx / 2, int(Math::floor((ground - 1.0f) / cell_size)), int(Math::floor(p_center.z / cell_size)) - nz / 2);
	if (origin.x == INT32_MIN) {
		origin = want;
		_floors(p_surface);
		return;
	}
	Vector3i shift = want - origin;
	if (Math::abs(shift.y) < 3) {
		shift.y = 0; // the grid's floor follows the ground only past a metre and a half
	}
	if (Math::abs(shift.x) < 2 && Math::abs(shift.z) < 2 && shift.y == 0) {
		return;
	}
	// Keep the dust already in the air: copy what overlaps the new box.
	for (int i = 0; i < int(conc.size()); i++) {
		tmp[i] = 0.0f;
		jet_tmp[i] = Vector3();
	}
	for (int z = 0; z < nz; z++) {
		const int sz = z + shift.z;
		if (sz < 0 || sz >= nz) {
			continue;
		}
		for (int y = 0; y < ny; y++) {
			const int sy = y + shift.y;
			if (sy < 0 || sy >= ny) {
				continue;
			}
			for (int x = 0; x < nx; x++) {
				const int sx = x + shift.x;
				if (sx < 0 || sx >= nx) {
					continue;
				}
				tmp[_idx(x, y, z)] = conc[_idx(sx, sy, sz)];
				jet_tmp[_idx(x, y, z)] = jet[_idx(sx, sy, sz)];
			}
		}
	}
	SWAP(conc, tmp);
	SWAP(jet, jet_tmp);
	origin += shift;
	_floors(p_surface);
}

void GroundDust3D::_floors(GroundSurface3D *p_surface) {
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector3 p((origin.x + x + 0.5f) * cell_size, 0.0f, (origin.z + z + 0.5f) * cell_size);
			floor_y[z * nx + x] = p_surface ? p_surface->get_ground_height_at(p) : origin.y * cell_size;
		}
	}
}

float GroundDust3D::_sample(const LocalVector<float> &p_field, Vector3 p_pos) const {
	// Trilinear, from world position; zero outside the grid.
	const float fx = p_pos.x / cell_size - origin.x - 0.5f;
	const float fy = p_pos.y / cell_size - origin.y - 0.5f;
	const float fz = p_pos.z / cell_size - origin.z - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int y0 = int(Math::floor(fy));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - x0, ty = fy - y0, tz = fz - z0;
	float r = 0.0f;
	for (int c = 0; c < 8; c++) {
		const int x = x0 + (c & 1), y = y0 + ((c >> 1) & 1), z = z0 + ((c >> 2) & 1);
		if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) {
			continue;
		}
		const float w = ((c & 1) ? tx : 1.0f - tx) * (((c >> 1) & 1) ? ty : 1.0f - ty) * (((c >> 2) & 1) ? tz : 1.0f - tz);
		r += w * p_field[_idx(x, y, z)];
	}
	return r;
}

void GroundDust3D::add_dust(Vector3 p_position, Vector3 p_velocity, float p_mass) {
	if (conc.is_empty() || p_mass <= 0.0f || origin.x == INT32_MIN) {
		return;
	}
	// Lifted from the ground: at least into the first air cell over the column (a sunken wheel's contact is
	// below the undisturbed ground the grid's floor follows).
	const int cx = CLAMP(int(Math::floor(p_position.x / cell_size)) - origin.x, 0, nx - 1);
	const int cz = CLAMP(int(Math::floor(p_position.z / cell_size)) - origin.z, 0, nz - 1);
	p_position.y = MAX(p_position.y, floor_y[cz * nx + cx] + cell_size);
	const float fx = p_position.x / cell_size - origin.x - 0.5f;
	const float fy = p_position.y / cell_size - origin.y - 0.5f;
	const float fz = p_position.z / cell_size - origin.z - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int y0 = int(Math::floor(fy));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - x0, ty = fy - y0, tz = fz - z0;
	const float vol = cell_size * cell_size * cell_size;
	for (int c = 0; c < 8; c++) {
		const int x = x0 + (c & 1), y = y0 + ((c >> 1) & 1), z = z0 + ((c >> 2) & 1);
		if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) {
			continue;
		}
		const float w = ((c & 1) ? tx : 1.0f - tx) * (((c >> 1) & 1) ? ty : 1.0f - ty) * (((c >> 2) & 1) ? tz : 1.0f - tz);
		if (w <= 0.0f) {
			continue;
		}
		const int i = _idx(x, y, z);
		const float add = w * p_mass / vol;
		const float before = conc[i];
		conc[i] += add;
		// The cell's air takes the plume's jet in the share of the dust it brings.
		jet[i] = jet[i].lerp(p_velocity, add / MAX(before + add, 1e-12f));
		airborne += w * p_mass;
	}
	emitted += p_mass;
	texture_dirty = true;
}

void GroundDust3D::clear() {
	for (uint32_t i = 0; i < conc.size(); i++) {
		conc[i] = 0.0f;
		jet[i] = Vector3();
	}
	airborne = deposited = emitted = 0.0;
	texture_dirty = true;
}

float GroundDust3D::get_concentration_at(Vector3 p_position) const {
	return conc.is_empty() ? 0.0f : _sample(conc, p_position);
}

void GroundDust3D::_step(float p_dt) {
	GroundSurface3D *surface = _surface();
	if (conc.is_empty()) {
		_allocate();
	}
	// The plume trails the source: the grid sits up to a quarter of its length behind it along its travel.
	const Vector3 here = _follow_position();
	if (has_last) {
		const Vector3 v = (here - last_follow) / MAX(p_dt, 1e-4f);
		follow_velocity = follow_velocity.lerp(Vector3(v.x, 0.0f, v.z), MIN(p_dt / 0.5f, 1.0f));
	}
	last_follow = here;
	has_last = true;
	const float lead = MIN(follow_velocity.length() * 1.5f, 0.25f * MIN(extent.x, extent.z));
	_recenter(here - (follow_velocity.length() > 0.1f ? follow_velocity.normalized() * lead : Vector3()), surface);
	if (surface) {
		surface->take_dust(sources);
		for (const GroundSurface3D::Dust &d : sources) {
			add_dust(d.position, d.velocity, d.rate * p_dt);
		}
		sources.clear();
	}
	if (airborne <= 1e-9) {
		return;
	}
	const Vector2 w10 = surface ? surface->get_wind_10m() : Vector2();
	const float ws = get_settling_speed();
	const float log10 = Math::log(10.0f / SAND_ROUGHNESS);
	const float decay = Math::exp(-p_dt / jet_time);
	const float h = cell_size;
	double fallen = 0.0;
	// Advection (semi-Lagrangian: each cell takes what the air brings to it) by the wind, the plume's jet and
	// the settling.
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const float fl = floor_y[z * nx + x];
			for (int y = 0; y < ny; y++) {
				const int i = _idx(x, y, z);
				const Vector3 p((origin.x + x + 0.5f) * h, (origin.y + y + 0.5f) * h, (origin.z + z + 0.5f) * h);
				if (p.y < fl) {
					tmp[i] = 0.0f;
					jet_tmp[i] = Vector3();
					continue;
				}
				const float above = MAX(p.y - fl, 0.05f);
				const float prof = MAX(Math::log(above / SAND_ROUGHNESS) / log10, 0.0f);
				const Vector3 vel = Vector3(w10.x * prof, 0.0f, w10.y * prof) + jet[i] + Vector3(0.0f, -ws, 0.0f);
				Vector3 back = p - vel * p_dt;
				// Below the ground there is no dust to bring: what settles past the floor is deposited.
				const float back_floor = floor_y[CLAMP(int(Math::floor(back.z / h)) - origin.z, 0, nz - 1) * nx + CLAMP(int(Math::floor(back.x / h)) - origin.x, 0, nx - 1)];
				tmp[i] = back.y < back_floor - 0.5f * h ? 0.0f : _sample(conc, back);
				// The lowest air cell of a column loses its settling flux to the ground.
				if ((origin.y + y - 0.5f) * h < fl) {
					const float f = conc[i] * MIN(ws * p_dt / h, 1.0f) * h * h * h;
					fallen += double(f);
					if (snow_fallen.size() == uint32_t(nx * nz)) {
						snow_fallen[z * nx + x] += f;
					}
				}
				jet_tmp[i] = jet[i] * decay;
			}
		}
	}
	SWAP(conc, tmp);
	SWAP(jet, jet_tmp);
	// Eddy diffusion (explicit, stable while D dt / h^2 < 1/6) and the tally.
	const float k = MIN(diffusivity * p_dt / (h * h), 0.16f);
	double sum = 0.0;
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			for (int x = 0; x < nx; x++) {
				const int i = _idx(x, y, z);
				const float c = conc[i];
				float lap = 0.0f;
				lap += (x > 0 ? conc[i - 1] : 0.0f) + (x < nx - 1 ? conc[i + 1] : 0.0f);
				// The ground is a wall to eddies: only settling deposits.
				const bool floor_below = y == 0 || (origin.y + y - 0.5f) * h < floor_y[z * nx + x];
				lap += (floor_below ? c : conc[i - nx]) + (y < ny - 1 ? conc[i + nx] : 0.0f);
				lap += (z > 0 ? conc[i - nx * ny] : 0.0f) + (z < nz - 1 ? conc[i + nx * ny] : 0.0f);
				lap -= 6.0f * c;
				float v = MAX(c + k * lap, 0.0f);
				const float py = (origin.y + y + 0.5f) * h;
				if (py < floor_y[z * nx + x]) {
					v = 0.0f;
				}
				tmp[i] = v < 1e-9f ? 0.0f : v;
				sum += tmp[i];
			}
		}
	}
	SWAP(conc, tmp);
	const double now = sum * h * h * h;
	deposited += fallen;
	airborne = now;
	texture_dirty = true;
	_hand_dust_to_snow(p_dt);
}

// Dust settling on snow darkens it (dust on snow, Painter et al. 2007): each second the settled mass per column goes
// to the snow contract as black carbon equivalent, 0.013 of the dust's mass (mass absorption about 0.1 m2/g against
// soot's 7.5).
void GroundDust3D::_hand_dust_to_snow(float p_dt) {
	WorldSnow *snow = WorldContracts::snow();
	if (!snow || !snow->has_provider()) {
		snow_fallen.clear();
		return;
	}
	if (snow_fallen.size() != uint32_t(nx * nz)) {
		snow_fallen.resize(nx * nz);
		for (float &f : snow_fallen) {
			f = 0.0f;
		}
		return;
	}
	snow_timer += p_dt;
	if (snow_timer < 1.0f) {
		return;
	}
	snow_timer = 0.0f;
	const float h = cell_size;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			float &f = snow_fallen[z * nx + x];
			if (f > 1e-9f) {
				const Vector3 p((origin.x + x + 0.5f) * h, floor_y[z * nx + x], (origin.z + z + 0.5f) * h);
				snow->add_impurity(p, h * 0.5f, f * 1.0e6f * 0.013f / (h * h));
			}
			f = 0.0f;
		}
	}
}

void GroundDust3D::_draw() {
	if (!texture_dirty || density.is_null() || !volume) {
		return;
	}
	texture_dirty = false;
	const float kext = get_mass_extinction() * visibility;
	for (int z = 0; z < nz; z++) {
		Ref<Image> img = slices[z];
		uint8_t *w = img->ptrw();
		float *f = reinterpret_cast<float *>(w);
		for (int y = 0; y < ny; y++) {
			for (int x = 0; x < nx; x++) {
				f[y * nx + x] = conc[_idx(x, y, z)] * kext;
			}
		}
	}
	density->update(slices);
	if (fog.is_valid()) {
		// The puffs ride the wind at a metre up and rise with the warm, stirred air of the wake.
		GroundSurface3D *surface = _surface();
		const Vector2 w10 = surface ? surface->get_wind_10m() : Vector2();
		const float prof = Math::log(1.0f / SAND_ROUGHNESS) / Math::log(10.0f / SAND_ROUGHNESS);
		const float dt = (float)get_process_delta_time();
		noise_offset += Vector3(w10.x * prof, 0.25f, w10.y * prof) * (dt / billow_size);
		fog->set_shader_parameter("noise_offset", noise_offset);
	}
	volume->set_global_position((Vector3(origin) + Vector3(nx, ny, nz) * 0.5f) * cell_size);
	volume->set_visible(airborne > 1e-7);
}

void GroundDust3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	if (volume) {
		volume->set_visible(enabled && airborne > 1e-7);
	}
	if (is_inside_tree()) {
		set_physics_process_internal(enabled);
		set_process_internal(enabled);
	}
}

void GroundDust3D::set_extent(const Vector3 &p_extent) {
	extent = p_extent.max(Vector3(2, 2, 2));
	if (!conc.is_empty()) {
		_allocate();
	}
}

void GroundDust3D::set_cell_size(float p_size) {
	cell_size = CLAMP(p_size, 0.1f, 4.0f);
	if (!conc.is_empty()) {
		_allocate();
	}
}

void GroundDust3D::set_billow(float p_v) {
	billow = CLAMP(p_v, 0.0f, 1.0f);
	if (fog.is_valid()) {
		fog->set_shader_parameter("billow", billow);
	}
}

void GroundDust3D::set_billow_size(float p_v) {
	billow_size = CLAMP(p_v, 0.1f, 20.0f);
	if (fog.is_valid()) {
		fog->set_shader_parameter("billow_size", billow_size);
	}
}

void GroundDust3D::set_dust_color(const Color &p_color) {
	dust_color = p_color;
	if (fog.is_valid()) {
		fog->set_shader_parameter("albedo", Vector3(dust_color.r, dust_color.g, dust_color.b));
	}
}

void GroundDust3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			fog_shader.instantiate();
			fog_shader->set_code(DUST_FOG_SHADER);
			fog.instantiate();
			fog->set_shader(fog_shader);
			fog->set_shader_parameter("albedo", Vector3(dust_color.r, dust_color.g, dust_color.b));
			fog->set_shader_parameter("puffs", _worley_texture());
			fog->set_shader_parameter("billow", billow);
			fog->set_shader_parameter("billow_size", billow_size);
			volume = memnew(FogVolume);
			volume->set_shape(RSE::FOG_VOLUME_SHAPE_BOX);
			volume->set_material(fog);
			add_child(volume, false, INTERNAL_MODE_FRONT);
			volume->set_as_top_level(true);
			volume->set_visible(false);
			_allocate();
			set_physics_process_internal(enabled && !Engine::get_singleton()->is_editor_hint());
			set_process_internal(enabled && !Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step((float)get_physics_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_draw();
		} break;
	}
}

void GroundDust3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ground_surface", "path"), &GroundDust3D::set_ground_surface);
	ClassDB::bind_method(D_METHOD("get_ground_surface"), &GroundDust3D::get_ground_surface);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &GroundDust3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &GroundDust3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &GroundDust3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &GroundDust3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &GroundDust3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &GroundDust3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &GroundDust3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &GroundDust3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_grain_diameter", "um"), &GroundDust3D::set_grain_diameter);
	ClassDB::bind_method(D_METHOD("get_grain_diameter"), &GroundDust3D::get_grain_diameter);
	ClassDB::bind_method(D_METHOD("set_diffusivity", "diffusivity"), &GroundDust3D::set_diffusivity);
	ClassDB::bind_method(D_METHOD("get_diffusivity"), &GroundDust3D::get_diffusivity);
	ClassDB::bind_method(D_METHOD("set_jet_time", "seconds"), &GroundDust3D::set_jet_time);
	ClassDB::bind_method(D_METHOD("get_jet_time"), &GroundDust3D::get_jet_time);
	ClassDB::bind_method(D_METHOD("set_dust_color", "color"), &GroundDust3D::set_dust_color);
	ClassDB::bind_method(D_METHOD("get_dust_color"), &GroundDust3D::get_dust_color);
	ClassDB::bind_method(D_METHOD("set_visibility", "visibility"), &GroundDust3D::set_visibility);
	ClassDB::bind_method(D_METHOD("get_visibility"), &GroundDust3D::get_visibility);
	ClassDB::bind_method(D_METHOD("set_billow", "billow"), &GroundDust3D::set_billow);
	ClassDB::bind_method(D_METHOD("get_billow"), &GroundDust3D::get_billow);
	ClassDB::bind_method(D_METHOD("set_billow_size", "size"), &GroundDust3D::set_billow_size);
	ClassDB::bind_method(D_METHOD("get_billow_size"), &GroundDust3D::get_billow_size);
	ClassDB::bind_method(D_METHOD("get_settling_speed"), &GroundDust3D::get_settling_speed);
	ClassDB::bind_method(D_METHOD("get_mass_extinction"), &GroundDust3D::get_mass_extinction);
	ClassDB::bind_method(D_METHOD("get_airborne_mass"), &GroundDust3D::get_airborne_mass);
	ClassDB::bind_method(D_METHOD("get_deposited_mass"), &GroundDust3D::get_deposited_mass);
	ClassDB::bind_method(D_METHOD("get_emitted_mass"), &GroundDust3D::get_emitted_mass);
	ClassDB::bind_method(D_METHOD("get_concentration_at", "position"), &GroundDust3D::get_concentration_at);
	ClassDB::bind_method(D_METHOD("add_dust", "position", "velocity", "mass"), &GroundDust3D::add_dust);
	ClassDB::bind_method(D_METHOD("clear"), &GroundDust3D::clear);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GroundSurface3D"), "set_ground_surface", "get_ground_surface");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "extent", PROPERTY_HINT_NONE, "suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.1,4,0.01,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grain_diameter", PROPERTY_HINT_RANGE, "1,200,0.1,suffix:um"), "set_grain_diameter", "get_grain_diameter");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "diffusivity", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m2/s"), "set_diffusivity", "get_diffusivity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "jet_time", PROPERTY_HINT_RANGE, "0.01,5,0.01,suffix:s"), "set_jet_time", "get_jet_time");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dust_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dust_color", "get_dust_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "billow", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_billow", "get_billow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "billow_size", PROPERTY_HINT_RANGE, "0.1,20,0.01,suffix:m"), "set_billow_size", "get_billow_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_visibility", "get_visibility");
}

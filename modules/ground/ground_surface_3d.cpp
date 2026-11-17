/**************************************************************************/
/*  ground_surface_3d.cpp                                                 */
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

#include "ground_surface_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

GroundSurface3D *GroundSurface3D::active = nullptr;

// ---------------------------------------------------------------------------------------------------------
// The rules

// Bekker values: Wong, "Theory of Ground Vehicles", table of terrain values (dry sand, Land Locomotion
// Lab; sandy loam at 15% moisture). Damp sand is firmer (capillary bridges, Hornbaker et al. 1997) and
// its walls stand far steeper; soft mud is fitted to 5 to 10 cm under a boot, since Wong's clays are
// trafficable soils. Repose of dry sand 33 degrees (Al-Hashemi and Al-Amoudi 2018).
// Bed load of fine sand (0.25 mm quartz) under water flowing at p_water_speed: Meyer-Peter and Mueller (1948),
// q = 8 sqrt((s - 1) g d^3) (theta - 0.047)^1.5, the Shields number from the bed shear of a depth-averaged
// speed with a drag coefficient of 0.003 (tau = rho Cd u^2).
float GroundSurface3D::bed_load(float p_water_speed) {
	return bed_load_shear(1000.0f * 0.003f * p_water_speed * p_water_speed);
}

float GroundSurface3D::bed_load_shear(float p_tau) {
	const float d = 0.00025f;
	const float theta = p_tau / (1650.0f * 9.81f * d);
	if (theta <= 0.047f) {
		return 0.0f;
	}
	return 8.0f * Math::sqrt(1.65f * 9.81f * d * d * d) * Math::pow(theta - 0.047f, 1.5f);
}

float GroundSurface3D::mud_yield_stress(float p_cv) {
	return 0.0181f * Math::exp(25.7f * p_cv); // 0.181 exp(25.7 Cv) dyn/cm2
}

float GroundSurface3D::mud_viscosity(float p_cv) {
	return 0.0036f * Math::exp(22.1f * p_cv); // 0.0360 exp(22.1 Cv) poise
}

GroundSurface3D::Cell GroundSurface3D::material_cell(float p_sand, float p_moisture, float p_litter, float p_saturation, float p_water_speed, float p_mud) {
	// Pore water (stage 4) wets the ground as rain does, and more where it is wetter.
	const float sat = CLAMP(p_saturation, 0.0f, 1.0f);
	const float m = CLAMP(p_saturation >= 0.0f ? MAX(p_moisture, sat) : p_moisture, 0.0f, 1.0f);
	const float cohesion = Math::smoothstep(0.0f, 0.5f, m); // damp sand gets all its strength early
	auto lerp_log = [](float a, float b, float t) { return Math::exp(Math::lerp(Math::log(a), Math::log(b), t)); };
	Cell sand;
	sand.n = Math::lerp(1.1f, 1.0f, m);
	sand.kc = Math::lerp(0.99f, 0.5f, m);
	sand.kphi = lerp_log(1528.43f, 6000.0f, cohesion);
	sand.rim_share = Math::lerp(0.8f, 0.7f, m);
	sand.tan_repose = Math::tan(Math::deg_to_rad(Math::lerp(33.0f, 65.0f, cohesion)));
	sand.rate = lerp_log(4.0f, 0.02f, cohesion);
	sand.max_dent = 0.3f;
	sand.darken = Math::lerp(0.3f, 0.12f, m);
	sand.gloss = Math::lerp(1.0f, 0.85f, m);
	// Dry sand (Land Locomotion Lab): c 1.04 kPa, phi 28 deg, loose (K 2.5 cm); damp sand as packed LETE sand
	// (c 1.3 kPa, phi 31.1 deg) and firm in shear (K 1 cm).
	sand.cohesion = Math::lerp(1.04f, 1.3f, cohesion);
	sand.tan_phi = Math::tan(Math::deg_to_rad(Math::lerp(28.0f, 31.1f, cohesion)));
	sand.shear_k = Math::lerp(0.025f, 0.01f, cohesion);
	sand.unit_weight = Math::lerp(15.7f, 17.5f, m);
	// Under a tyre damp sand bears as packed sand does: LETE sand (Wong table 2.2: n 0.79, kc 102, kphi 5301);
	// dry, the Land Locomotion Lab's dry sand, as the boot.
	const float packed = Math::smoothstep(0.0f, 0.35f, m);
	sand.wheel_n = Math::lerp(1.1f, 0.79f, packed);
	sand.wheel_kc = lerp_log(0.99f, 102.0f, packed);
	sand.wheel_kphi = lerp_log(1528.43f, 5301.0f, packed);
	Cell soil;
	soil.n = Math::lerp(0.7f, 1.0f, m);
	soil.kc = Math::lerp(5.27f, 3.5f, m);
	soil.kphi = lerp_log(1515.04f, 230.0f, m);
	soil.rim_share = Math::lerp(0.4f, 0.95f, m);
	soil.tan_repose = Math::tan(Math::deg_to_rad(Math::lerp(40.0f, 25.0f, m)));
	soil.rate = lerp_log(0.5f, 1.0f / 120.0f, m); // crumbling edges dry, a slow creep as mud (Coussot 1997)
	soil.max_dent = Math::lerp(0.08f, 0.3f, m);
	soil.darken = Math::lerp(0.3f, 0.15f, m);
	soil.gloss = Math::lerp(1.0f, 0.6f, m);
	// Sandy loam at 15% (c 1.72 kPa, phi 29 deg) to clayey soil at 38% (c 4.14 kPa, phi 13 deg), Wong table 2.2.
	soil.cohesion = Math::lerp(1.72f, 4.14f, m);
	soil.tan_phi = Math::tan(Math::deg_to_rad(Math::lerp(29.0f, 13.0f, m)));
	soil.shear_k = 0.025f;
	soil.unit_weight = Math::lerp(15.0f, 17.0f, m);
	soil.wheel_n = soil.n;
	soil.wheel_kc = soil.kc;
	soil.wheel_kphi = soil.kphi;
	const float s = CLAMP(p_sand, 0.0f, 1.0f);
	Cell c;
	c.n = Math::lerp(soil.n, sand.n, s);
	c.kc = Math::lerp(soil.kc, sand.kc, s);
	c.kphi = lerp_log(soil.kphi, sand.kphi, s);
	c.rim_share = Math::lerp(soil.rim_share, sand.rim_share, s);
	c.tan_repose = Math::lerp(soil.tan_repose, sand.tan_repose, s);
	c.rate = lerp_log(soil.rate, sand.rate, s);
	c.max_dent = Math::lerp(soil.max_dent, sand.max_dent, s);
	c.darken = Math::lerp(soil.darken, sand.darken, s);
	c.gloss = Math::lerp(soil.gloss, sand.gloss, s);
	c.cohesion = Math::lerp(soil.cohesion, sand.cohesion, s);
	c.tan_phi = Math::lerp(soil.tan_phi, sand.tan_phi, s);
	c.shear_k = Math::lerp(soil.shear_k, sand.shear_k, s);
	c.unit_weight = Math::lerp(soil.unit_weight, sand.unit_weight, s);
	c.wheel_n = Math::lerp(soil.wheel_n, sand.wheel_n, s);
	c.wheel_kc = lerp_log(soil.wheel_kc, sand.wheel_kc, s);
	c.wheel_kphi = lerp_log(soil.wheel_kphi, sand.wheel_kphi, s);
	c.sand = s;
	if (p_saturation >= 0.0f) {
		// Saturated sand has no water bridges left between its grains (Su et al. 2023's capillary state and past
		// it): its walls stand no steeper than dry sand's and slump as fast.
		const float full = Math::smoothstep(0.8f, 0.95f, sat) * s;
		c.tan_repose = Math::lerp(c.tan_repose, Math::tan(Math::deg_to_rad(33.0f)), full);
		c.rate = Math::exp(Math::lerp(Math::log(c.rate), Math::log(4.0f), full));
		c.cohesion = Math::lerp(c.cohesion, 1.04f, full);
	}
	if (p_water_speed > 0.0f && s > 0.0f) {
		// Flowing water moves the sand (stage 4): the Exner equation with bed load running down the bed's slope
		// (Bagnold's slope term, 1 / tan(33) = 1.5) is a diffusion of the bed, D = 1.5 q / (1 - porosity); at
		// the scale of a print (30 cm) it relaxes the print at D (2 pi / 0.3)^2 per second. Slumping does the
		// moving (volume kept), towards a near-flat bed.
		const float q = bed_load(p_water_speed);
		if (q > 0.0f) {
			const float k = 1.5f * q / 0.6f * Math::pow(2.0f * Math::PI / 0.3f, 2.0f) * s;
			c.rate = MAX(c.rate, k);
			c.tan_repose = Math::lerp(c.tan_repose, 0.05f, s);
		}
	}
	// Deep mud (docs/ideas/mud.md, stage 1): a soft layer of clay and silt over the firm base, a Bingham fluid whose
	// yield stress and viscosity climb steeply with the solid share (O'Brien and Julien 1988).
	if (p_mud > 0.0f && s < 0.5f) {
		const float mw = Math::smoothstep(0.02f, 0.1f, p_mud) * (1.0f - 2.0f * s);
		const float cv = mud_concentration(m);
		const float stiff = CLAMP((cv - 0.40f) / 0.135f, 0.0f, 1.0f); // 0 slurry; 1 stiff, at Cv 0.535: clay at 32% water by mass
		const float tau_y = mud_yield_stress(cv);
		const float mu = mud_viscosity(cv);
		const float rho = 2650.0f * cv + 1000.0f * (1.0f - cv);
		Cell mud;
		// A rut's wall stands where the mud's own weight down its slope stays under the yield stress: the
		// critical slope tau_y / (rho g h) for the mass that would move, about the rut's depth (half the
		// layer, at most 30 cm). Steeper than that it flows at Bingham's rate, the depth-averaged plug flow's
		// diffusivity D = rho g h^3 / (3 mu) over the main field's texel (6.25 cm; the near field takes
		// more passes for its own).
		const float h = CLAMP(0.5f * p_mud, 0.05f, 0.3f);
		mud.tan_repose = CLAMP(tau_y / (rho * 9.81f * h), 0.03f, Math::tan(Math::deg_to_rad(70.0f)));
		const float diff = rho * 9.81f * h * h * h / (3.0f * MAX(mu, 1e-3f));
		mud.rate = CLAMP(2.0f * diff / (0.0625f * 0.0625f), 1.0f / 600.0f, 1000.0f);
		// Nearly incompressible: what a wheel pushes down squeezes up beside it.
		mud.rim_share = 0.97f;
		mud.max_dent = MAX(c.max_dent, p_mud);
		// Pressure-sinkage: stiff mud as lean clay at 32% (Wong table 2.2: n 0.5, kc 13.19, kphi 692.15); slurry
		// fitted so a boot (21 kPa) sinks about 15 cm (n 0.5, kphi 54) and a loaded tyre goes to the base.
		mud.n = 0.5f;
		mud.kc = Math::lerp(0.0f, 13.19f, stiff);
		mud.kphi = lerp_log(54.0f, 692.15f, stiff);
		mud.wheel_n = mud.n;
		mud.wheel_kc = mud.kc;
		mud.wheel_kphi = mud.kphi;
		// Shear: clay carries load by cohesion, friction small (lean clay c 4.14 kPa, phi 13 deg); a slurry
		// only its yield stress and a couple of degrees.
		mud.cohesion = Math::lerp(tau_y * 0.001f, 4.14f, stiff);
		mud.tan_phi = Math::tan(Math::deg_to_rad(Math::lerp(2.0f, 13.0f, stiff)));
		mud.shear_k = 0.02f;
		mud.unit_weight = rho * 9.81f * 0.001f;
		mud.darken = 0.25f;
		mud.gloss = Math::lerp(0.35f, 0.7f, stiff);
		c.n = Math::lerp(c.n, mud.n, mw);
		c.kc = Math::lerp(c.kc, mud.kc, mw);
		c.kphi = lerp_log(c.kphi, mud.kphi, mw);
		c.wheel_n = Math::lerp(c.wheel_n, mud.wheel_n, mw);
		c.wheel_kc = Math::lerp(c.wheel_kc, mud.wheel_kc, mw);
		c.wheel_kphi = lerp_log(c.wheel_kphi, mud.wheel_kphi, mw);
		c.tan_repose = Math::lerp(c.tan_repose, mud.tan_repose, mw);
		c.rate = lerp_log(c.rate, mud.rate, mw);
		c.rim_share = Math::lerp(c.rim_share, mud.rim_share, mw);
		c.max_dent = Math::lerp(c.max_dent, mud.max_dent, mw);
		c.cohesion = Math::lerp(c.cohesion, mud.cohesion, mw);
		c.tan_phi = Math::lerp(c.tan_phi, mud.tan_phi, mw);
		c.shear_k = Math::lerp(c.shear_k, mud.shear_k, mw);
		c.unit_weight = Math::lerp(c.unit_weight, mud.unit_weight, mw);
		c.darken = Math::lerp(c.darken, mud.darken, mw);
		c.gloss = Math::lerp(c.gloss, mud.gloss, mw);
		c.mud = p_mud * mw;
	}
	// Stickiness (docs/ideas/mud.md, stage 3): clay clings to rubber and steel most between its plastic and liquid
	// limits, little dry, less as a slurry that runs off (Fountaine 1954; Ren et al. 2001); sand barely at all.
	// The tangential adhesion of wet clay on rubber or steel, up to about 5 kPa (Wong 2008's ca).
	c.stickiness = Math::smoothstep(0.2f, 0.45f, m) * (1.0f - 0.5f * Math::smoothstep(0.7f, 1.0f, m)) * (1.0f - Math::smoothstep(0.2f, 0.6f, s));
	c.adhesion = 5.0f * c.stickiness;
	// Wind moves only dry sand: a few percent of water binds the grains and the threshold climbs out of reach
	// (Belly 1964; McKenna Neuman and Nickling 1989).
	c.blown = s * (1.0f - Math::smoothstep(0.05f, 0.2f, m));
	// Leaf litter on top: compresses to a third under a boot, pushes little aside, and shows the darker
	// soil where it is pressed through.
	c.litter = MAX(p_litter, 0.0f);
	if (c.litter > 0.0f) {
		const float lw = MIN(c.litter / 0.02f, 1.0f);
		c.max_dent += 0.7f * c.litter;
		c.rim_share = Math::lerp(c.rim_share, 0.1f, lw);
		c.darken = MAX(c.darken, 0.45f * lw);
		c.tan_repose = MAX(c.tan_repose, Math::lerp(c.tan_repose, Math::tan(Math::deg_to_rad(60.0f)), lw));
		c.blown *= 1.0f - lw;
		c.stickiness *= 1.0f - 0.7f * lw;
		c.adhesion *= 1.0f - 0.7f * lw;
	}
	return c;
}

GroundDeformField::Column GroundSurface3D::Columns::column(float p_x, float p_z) const {
	GroundDeformField::Column col;
	const Cell &c = surface->_cell_at(p_x, p_z);
	// Only where the terrain is fully lowered under the patch.
	if (!c.deformable || surface->_edge_distance(p_x, p_z) < surface->terrain_margin + surface->skirt_fade) {
		return col;
	}
	col.deformable = true;
	col.surface = surface->_ground_at(p_x, p_z);
	col.max_dent = c.max_dent;
	col.rim_share = c.rim_share;
	return col;
}

float GroundSurface3D::Columns::sinkage(const GroundDeformField::Body &p_body) const {
	const Cell &c = surface->_cell_at(p_body.center.x, p_body.center.z);
	if (!c.deformable) {
		return 0.0f;
	}
	float sink = GroundDeformField::body_sinkage(p_body, c.n, c.kc, c.kphi);
	if (c.litter > 0.0f && p_body.mass > 0.0f) {
		const float area = p_body.area > 0.0f ? p_body.area : (p_body.flat_area > 0.0f ? p_body.flat_area : Math::PI * p_body.radius * p_body.radius * 0.25f);
		const float kpa = p_body.mass * 9.81f / 1000.0f / MAX(area, 1e-3f);
		sink += 0.7f * c.litter * MIN(kpa / 5.0f, 1.0f);
	}
	return MIN(sink, c.max_dent);
}

// ---------------------------------------------------------------------------------------------------------
// Lattice and cells

float GroundSurface3D::_ground_at(float p_x, float p_z) const {
	const int n = ground_n;
	if (!ground_valid || n < 2) {
		return 0.0f;
	}
	const float fx = CLAMP(p_x / ground_spacing - ground_origin.x, 0.0f, float(n - 1));
	const float fz = CLAMP(p_z / ground_spacing - ground_origin.y, 0.0f, float(n - 1));
	const int x0 = MIN(int(fx), n - 2);
	const int z0 = MIN(int(fz), n - 2);
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int x, int z) { return ground[z * n + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

const GroundSurface3D::Cell &GroundSurface3D::_cell_at(float p_x, float p_z) const {
	static const Cell none = [] { Cell c; c.deformable = false; return c; }();
	const int n = ground_n;
	if (!ground_valid || int(cells.size()) != n * n) {
		return none;
	}
	const int x = int(Math::round(p_x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_z / ground_spacing)) - ground_origin.y;
	if (x < 0 || z < 0 || x >= n || z >= n) {
		return none;
	}
	return cells[z * n + x];
}

float GroundSurface3D::_edge_distance(float p_x, float p_z) const {
	if (!field.valid) {
		return -1.0f;
	}
	const Rect2 r = field.rect();
	return MIN(MIN(p_x - r.position.x, r.position.x + r.size.x - p_x), MIN(p_z - r.position.y, r.position.y + r.size.y - p_z));
}

void GroundSurface3D::_sample_ground(Vector2i p_origin, int p_n) {
	const GroundHeightQuery q = GroundHeightQuery::make(this, height_source, ground_collision_mask);
	LocalVector<float> next;
	next.resize(p_n * p_n);
	const bool keep = ground_valid && ground_n == p_n;
	const Vector2i d = p_origin - ground_origin;
	const float fallback = q.y0;
	for (int z = 0; z < p_n; z++) {
		for (int x = 0; x < p_n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			float h;
			if (keep && sx >= 0 && sz >= 0 && sx < p_n && sz < p_n) {
				h = ground[sz * p_n + sx];
			} else {
				h = q.height(Vector3((p_origin.x + x) * ground_spacing, 0.0f, (p_origin.y + z) * ground_spacing));
				if (!Math::is_finite(h)) {
					h = fallback;
				}
			}
			next[z * p_n + x] = h;
		}
	}
	ground = next;
	// The pore water moves with the lattice (new cells start as wet as the far lattice has them).
	if (keep && (int)pore.size() == p_n * p_n) {
		_shift_lattice(pore, ground_origin, p_origin, p_n, 1);
		for (int z = 0; z < p_n; z++) {
			for (int x = 0; x < p_n; x++) {
				const int sx = x + d.x;
				const int sz = z + d.y;
				if (sx < 0 || sz < 0 || sx >= p_n || sz >= p_n) {
					pore[z * p_n + x] = _far_pore_at((p_origin.x + x) * ground_spacing, (p_origin.y + z) * ground_spacing);
				}
			}
		}
		_shift_lattice(water_speed, ground_origin, p_origin, p_n, 1);
		_shift_lattice(cell_water, ground_origin, p_origin, p_n, 2);
		if ((int)crack_state.size() == p_n * p_n * CRACK_FLOATS) {
			_shift_lattice(crack_state, ground_origin, p_origin, p_n, CRACK_FLOATS); // new cells: never soaked (0)
		}
		if ((int)water_table.size() == p_n * p_n) {
			_shift_lattice(water_table, ground_origin, p_origin, p_n, 1);
			for (int i = 0; i < p_n * p_n; i++) {
				if (water_table[i] == 0.0f) {
					water_table[i] = 10.0f; // new cells: none (shifted in as 0)
				}
			}
		}
	} else {
		water_table.clear();
		pore.clear();
		water_speed.clear();
		cell_water.clear();
	}
	// The sand the wind piled moves with it; the shelter and flux are measured again within half a second.
	if (keep && (int)sand_dep.size() == p_n * p_n) {
		_shift_lattice(sand_dep, ground_origin, p_origin, p_n, 1);
		_shift_lattice(sand_dep_pending, ground_origin, p_origin, p_n, 1);
	} else {
		sand_dep.clear();
		sand_dep_pending.clear();
	}
	shelter_ratio.clear();
	shelter_flux.clear();
	sand_flux.clear();
	sand_dep_rate.clear();
	wind_order.clear();
	ground_n = p_n;
	ground_origin = p_origin;
	ground_valid = true;
	gpu_ground_dirty = true;
	near_ground_dirty = true;
	ground_min = 1e20f;
	ground_max = -1e20f;
	for (float h : ground) {
		ground_min = MIN(ground_min, h);
		ground_max = MAX(ground_max, h);
	}
	Vector<uint8_t> bytes;
	bytes.resize(p_n * p_n * 4);
	memcpy(bytes.ptrw(), ground.ptr(), p_n * p_n * 4);
	if (height_image.is_null() || height_image->get_width() != p_n) {
		height_image = Image::create_from_data(p_n, p_n, false, Image::FORMAT_RF, bytes);
		height_texture = ImageTexture::create_from_image(height_image);
		globals_published = false;
	} else {
		height_image->set_data(p_n, p_n, false, Image::FORMAT_RF, bytes);
		height_texture->update(height_image);
	}
	_update_cells();
}

void GroundSurface3D::_update_cells() {
	const int n = ground_n;
	if (!ground_valid || n <= 0) {
		return;
	}
	cells.resize(n * n);
	cell_inputs.resize(n * n);
	if ((int)pore.size() != n * n) {
		pore.resize(n * n);
		water_speed.resize(n * n);
		cell_water.resize(n * n * 2);
		for (int i = 0; i < n * n; i++) {
			pore[i] = 0.0f;
			water_speed[i] = 0.0f;
			cell_water[i * 2 + 0] = 0.0f;
			cell_water[i * 2 + 1] = 0.0f;
		}
	}
	gpu_cells_dirty = true;
	near_cells_dirty = true;
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	Node *hs = height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(height_source);
	const TerrainGenerator3D *gen = rock_from_geology ? Object::cast_to<TerrainGenerator3D>(hs) : nullptr;
	Node *snow = snow_cover.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(snow_cover);
	if (snow && !snow->has_method("get_depth_at")) {
		snow = nullptr;
	}
	const Node *dmap = data_map.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(data_map);
	if (!Object::cast_to<TerrainDataMap3D>(dmap)) {
		dmap = Object::cast_to<TerrainDataMap3D>(hs);
	}
	const float base_sand = ground_type == GROUND_SAND ? 1.0f : 0.0f;
	const bool map = type_map.is_valid() && !type_map->is_empty() && type_map_rect.size.x > 0.0f && type_map_rect.size.y > 0.0f;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const Vector3 p((ground_origin.x + x) * ground_spacing, ground[i], (ground_origin.y + z) * ground_spacing);
			float sand = base_sand;
			bool rock = false;
			if (map) {
				const Vector2 uv = (Vector2(p.x, p.z) - type_map_rect.position) / type_map_rect.size;
				if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
					const Color c = type_map->get_pixel(CLAMP(int(uv.x * type_map->get_width()), 0, type_map->get_width() - 1), CLAMP(int(uv.y * type_map->get_height()), 0, type_map->get_height() - 1));
					sand = c.r;
					rock = c.g > 0.5f;
				}
			}
			if (gen && gen->get_rock_exposure(p) > 0.5f) {
				rock = true;
			}
			const float m = moisture >= 0.0f ? moisture : (wm ? wm->get_wetness_at(p) : 0.0f);
			const float closure = wm ? wm->get_canopy_closure_at(p) : litter_cover;
			CellInput &in = cell_inputs[i];
			in.sand = sand;
			in.moisture = m;
			in.litter = litter_depth * CLAMP(closure, 0.0f, 1.0f);
			// Under more than a few centimetres of snow the snow surface takes the print.
			in.fixed = rock || (snow && float(snow->call("get_depth_at", p)) > 0.03f);
			const bool wet = water_coupling && (int)pore.size() == n * n && (pore[i] > 0.0f || water_speed[i] > 0.0f);
			in.mud = in.fixed ? 0.0f : _mud_depth_at(p, wet ? MAX(m, pore[i]) : m, sand > 0.5f, dmap);
			Cell c = wet ? material_cell(in.sand, in.moisture, in.litter, pore[i], water_speed[i], in.mud) : material_cell(in.sand, in.moisture, in.litter, -1.0f, 0.0f, in.mud);
			if (wet) {
				cell_water[i * 2 + 0] = pore[i];
				cell_water[i * 2 + 1] = water_speed[i];
			}
			c.deformable = !in.fixed;
			cells[i] = c;
		}
	}
}

float GroundSurface3D::_mud_depth_at(const Vector3 &p_position, float p_moisture, bool p_sand_or_rock, const Node *p_data_map) const {
	if (!mud_layer || p_sand_or_rock) {
		return 0.0f;
	}
	float depth = 0.0f;
	if (mud_map.is_valid() && !mud_map->is_empty() && mud_map_rect.size.x > 0.0f && mud_map_rect.size.y > 0.0f) {
		const Vector2 uv = (Vector2(p_position.x, p_position.z) - mud_map_rect.position) / mud_map_rect.size;
		if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
			const float fx = uv.x * mud_map->get_width() - 0.5f;
			const float fy = uv.y * mud_map->get_height() - 0.5f;
			const int x0 = CLAMP((int)Math::floor(fx), 0, mud_map->get_width() - 1);
			const int y0 = CLAMP((int)Math::floor(fy), 0, mud_map->get_height() - 1);
			const int x1 = MIN(x0 + 1, mud_map->get_width() - 1);
			const int y1 = MIN(y0 + 1, mud_map->get_height() - 1);
			const float tx = CLAMP(fx - x0, 0.0f, 1.0f);
			const float ty = CLAMP(fy - y0, 0.0f, 1.0f);
			const float r = Math::lerp(Math::lerp(mud_map->get_pixel(x0, y0).r, mud_map->get_pixel(x1, y0).r, tx), Math::lerp(mud_map->get_pixel(x0, y1).r, mud_map->get_pixel(x1, y1).r, tx), ty);
			depth = MAX(depth, r * mud_max_depth);
		}
	}
	if (Object::cast_to<TerrainDataMap3D>(p_data_map) && mud_auto_depth > 0.0f) {
		// Where water gathers: a high topographic wetness index (Beven and Kirkby 1979; valley floors, hollows,
		// flats below slopes) and the first few metres of a bank. Slopes drain, so steep ground keeps none.
		const TerrainDataMap3D *dm = Object::cast_to<TerrainDataMap3D>(p_data_map);
		const float twi = dm->get_wetness_at(p_position);
		const float dist = dm->get_water_distance_at(p_position);
		const float slope = dm->get_slope_at(p_position); // radians
		const float gather = MAX(Math::smoothstep(8.0f, 13.0f, twi), 1.0f - Math::smoothstep(0.5f, 4.0f, dist));
		const float flat = 1.0f - Math::smoothstep(Math::deg_to_rad(4.0f), Math::deg_to_rad(12.0f), slope);
		depth = MAX(depth, mud_auto_depth * gather * flat);
	}
	// Only wet mud is soft: a dry track bears like soil (MudRunner's hidden mud, "Extrudes To Wetness").
	return depth * Math::smoothstep(mud_wet_start, mud_wet_full, p_moisture);
}

float GroundSurface3D::get_mud_depth_at(Vector3 p_position) const {
	return _cell_at(p_position.x, p_position.z).mud;
}

void GroundSurface3D::set_mud_layer(bool p_enabled) {
	mud_layer = p_enabled;
	_update_cells();
}

void GroundSurface3D::set_standing_water(bool p_enabled) {
	standing_water = p_enabled;
	if (!p_enabled) {
		// Drop what stands: a reset clears the GPU field's water with the rest.
		gpu_host.reset = true;
		near_host.reset = true;
		field.water.clear();
		near_fld.water.clear();
	}
}

float GroundSurface3D::get_standing_water_at(Vector3 p_position) const {
	if (is_near_active() && _near_inside(p_position.x, p_position.z) && int(near_fld.water.size()) == near_fld.resolution * near_fld.resolution) {
		return near_fld.sample(near_fld.water, p_position.x, p_position.z);
	}
	if (field.valid && int(field.water.size()) == field.resolution * field.resolution) {
		return field.sample(field.water, p_position.x, p_position.z);
	}
	return 0.0f;
}

// Saturated hydraulic conductivity (m/s) of a cell's top layer: clay-rich soil 1e-6 to sand 1e-4 (Rawls,
// Brakensiek and Saxton 1982), forest litter as fast as sand, puddled mud a third of its soil's; rock and snow
// none (they shed it).
float GroundSurface3D::_water_conductivity(const CellInput &p_in) const {
	if (p_in.fixed) {
		return 0.0f;
	}
	float k = Math::exp(Math::lerp(Math::log(1e-6f), Math::log(1e-4f), CLAMP(p_in.sand, 0.0f, 1.0f)));
	k = Math::lerp(k, 1e-4f, Math::smoothstep(0.0f, 0.03f, p_in.litter));
	if (p_in.mud > 0.0f) {
		k *= 1.0f / 3.0f;
	}
	return k;
}

// How far below the ground the water table lies at lattice cell p_i (m): a sea's or lake's (runoff_channels),
// else from the wetness: soaked ground has it at the surface, ground at mud's wetness a few decimetres down.
float GroundSurface3D::_water_table_depth(int p_i) const {
	const CellInput &in = cell_inputs[p_i];
	if (in.fixed) {
		return 10.0f;
	}
	// Not the top layer's pore water: standing water soaks that, and must not raise the table it stands on.
	float d = (1.0f - CLAMP(in.moisture, 0.0f, 1.0f)) * 0.6f;
	if (runoff_channels && (int)water_table.size() > p_i) {
		d = MIN(d, MAX(water_table[p_i], 0.0f));
	}
	return d;
}

// Evaporation from open water (m/s): Penman's (1948) wind function, as WeatherMap3D's surface film; none in rain.
float GroundSurface3D::_evaporation() const {
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	if (!wm) {
		return 2e-8f; // a mild day's, about 2 mm a day
	}
	const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
	if (wm->get_rain_at(at) > 0.1f) {
		return 0.0f;
	}
	const float t = wm->get_air_temperature();
	const float es = 6.112f * Math::exp(17.62f * t / (243.12f + t));
	const float deficit = MAX(es * (1.0f - CLAMP(wm->get_air_humidity(), 0.0f, 1.0f)), 0.0f);
	const float u2 = _wind_10m().length() * 0.75f; // 10 m to 2 m on the log law over short grass
	return 0.26f * (1.0f + 0.54f * u2) * deficit / 24.0f / 3.6e6f;
}

// Pipe-model substeps for a step: the shallow-water wave (sqrt(g h), for water up to 20 cm) crossing half a
// texel each, at most 4 (past that the water just moves slower).
// Bed substeps a pipe-model substep for runoff channels: the transport is held to the bed's own wave crossing half
// a texel a substep at the morphological factor (ground_deform.glsl), so the factor is split over substeps of 40 at
// most, and a scaled run cuts the same whatever the factor (up to 32 substeps, a factor of 1280).
int GroundSurface3D::_morph_passes() const {
	return CLAMP(int(Math::ceil(runoff_time_scale / 40.0f)), 1, 32);
}

int GroundSurface3D::_water_passes(const GroundDeformField &p_field, double p_delta) const {
	const float limit = 0.5f * p_field.texel() / Math::sqrt(9.81f * 0.2f);
	return CLAMP(int(Math::ceil(float(p_delta) / limit)), 1, 4);
}

void GroundSurface3D::set_desiccation_cracks(bool p_enabled) {
	desiccation_cracks = p_enabled;
	if (!p_enabled) {
		crack_state.clear();
	}
}

// Drying (docs/ideas/mud.md, stage 2b). Clay soaked by standing water or to saturation, once the ground round it is
// no longer wet, loses its water to evaporation (Penman, as the standing water) from its drying layer (40%
// porosity); it shrinks, and once it has lost about a third of its water the top cracks (past the shrinkage limit's
// start), the cracks opening fully by the time four fifths have gone. Wetting again swells it shut and counts a
// cycle. The drying layer is the fine clay a pool lets settle: thicker the deeper the pool stood, so a dried puddle's
// thin rim dries and cracks first into small plates and fine cracks, its deep centre later into large ones.
void GroundSurface3D::_crack_update(double p_delta) {
	const int n = ground_n;
	if (!desiccation_cracks || !ground_valid || n < 2 || (int)cell_inputs.size() != n * n) {
		return;
	}
	if ((int)crack_state.size() != n * n * CRACK_FLOATS) {
		crack_state.resize(n * n * CRACK_FLOATS);
		for (float &v : crack_state) {
			v = 0.0f;
		}
	}
	const float evap = _evaporation() * runoff_time_scale;
	const bool pores = (int)pore.size() == n * n;
	for (int i = 0; i < n * n; i++) {
		const CellInput &in = cell_inputs[i];
		float *cs = &crack_state[i * CRACK_FLOATS];
		float &st = cs[0];
		float &cycles = cs[1];
		float &pool = cs[2];
		// Only clay-rich bare ground cracks: not sand, litter, rock or snow.
		if (in.fixed || in.sand > 0.5f || in.litter > 0.01f) {
			st = 0.0f;
			pool = 0.0f;
			continue;
		}
		const float wx = (ground_origin.x + i % n) * ground_spacing;
		const float wz = (ground_origin.y + i / n) * ground_spacing;
		const float standing = standing_water ? get_standing_water_at(Vector3(wx, 0.0f, wz)) : 0.0f;
		const bool soaked = (pores && pore[i] >= 0.95f) || standing > 0.001f;
		if (soaked) {
			if (st > 0.0f && st < 1.7f) {
				cycles = MIN(cycles + 1.0f, 10.0f); // it had cracked: a cycle
				pool = 0.0f; // a new pool lays a new layer
			}
			st = 2.0f;
			pool = MAX(pool, standing);
			continue;
		}
		if (st <= 0.0f || in.moisture >= mud_wet_start) {
			continue; // never soaked, or the ground round it is still wet
		}
		const float thick = _crack_at_cell(i).y * 0.25f;
		st = MAX(st - evap * float(p_delta) / (0.4f * MAX(thick, 0.002f)), 1.0f);
	}
}

float GroundSurface3D::_crack_layer(int p_i) const {
	// The drying layer: the soft top the water worked, about 6 mm where the ground was only saturated, thicker the
	// deeper a pool stood on it (its slurry and the clay it settled) up to 5 cm; thinner where the soil is sandier,
	// and varied a little from place to place. Plates about four times as wide (Groisman and Kaplan 1994: spacing in
	// proportion to the layer), so a dried puddle cracks finer towards its rim.
	const int n = ground_n;
	const CellInput &in = cell_inputs[p_i];
	const float wx = (ground_origin.x + p_i % n) * ground_spacing;
	const float wz = (ground_origin.y + p_i / n) * ground_spacing;
	const float pool = (int)crack_state.size() == n * n * CRACK_FLOATS ? crack_state[p_i * CRACK_FLOATS + 2] : 0.0f;
	const float clay = (0.5f + 0.5f * Math::smoothstep(0.5f, 0.2f, in.sand)) * (0.8f + 0.2f * CLAMP(in.mud / 0.3f, 0.0f, 1.0f));
	return CLAMP(0.006f + 0.6f * pool, 0.006f, 0.05f) * clay * (0.85f + 0.3f * _value_noise(wx / 3.0f, wz / 3.0f));
}

Vector4 GroundSurface3D::_crack_at_cell(int p_i, float *r_damp) const {
	const int n = ground_n;
	const float thick = _crack_layer(p_i);
	if (r_damp) {
		*r_damp = 0.0f;
	}
	if ((int)crack_state.size() != n * n * CRACK_FLOATS) {
		return Vector4(0.0f, 4.0f * thick, 0.0f, 0.0f);
	}
	const float st = crack_state[p_i * CRACK_FLOATS + 0];
	if (st <= 0.0f) {
		return Vector4(0.0f, 4.0f * thick, 0.0f, 0.0f);
	}
	const float water = st - 1.0f;
	// Its tone, from dark with water (-1, soaked) to the pale crust (1) by the time half the water has gone; the
	// cracks open over the second half of that.
	const float tone = (1.0f - 2.0f * Math::smoothstep(0.5f, 0.95f, water)) * Math::smoothstep(0.004f, 0.009f, thick);
	if (r_damp) {
		*r_damp = MAX(-tone, 0.0f); // material.cpp takes the larger of this and the pore water's
	}
	// Ground only saturated (a layer of about 6 mm) cracks too finely to show; a pool's bed does, its cracks finer
	// and fading out towards its rim as the water there stood shallower.
	const float shows = Math::smoothstep(0.006f, 0.012f, thick);
	return Vector4(Math::smoothstep(0.67f, 0.2f, water) * shows, 4.0f * thick, MIN(crack_state[p_i * CRACK_FLOATS + 1] / 5.0f, 1.0f), MAX(tone, 0.0f));
}

Vector4 GroundSurface3D::get_crack_at(Vector3 p_position) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)cell_inputs.size() != n * n) {
		return Vector4();
	}
	const int x = CLAMP(int(Math::round(p_position.x / ground_spacing)) - ground_origin.x, 0, n - 1);
	const int z = CLAMP(int(Math::round(p_position.z / ground_spacing)) - ground_origin.y, 0, n - 1);
	float damp = 0.0f;
	Vector4 c = _crack_at_cell(z * n + x, &damp);
	c.w -= damp;
	return c;
}

void GroundSurface3D::set_mud_map(const Ref<Image> &p_map) {
	mud_map = p_map;
	_update_cells();
}

void GroundSurface3D::set_mud_map_rect(const Rect2 &p_rect) {
	mud_map_rect = p_rect;
	_update_cells();
}

void GroundSurface3D::set_mud_max_depth(float p_m) {
	mud_max_depth = MAX(p_m, 0.0f);
	_update_cells();
}

void GroundSurface3D::set_mud_auto_depth(float p_m) {
	mud_auto_depth = MAX(p_m, 0.0f);
	_update_cells();
}

void GroundSurface3D::set_mud_wet_start(float p_m) {
	mud_wet_start = CLAMP(p_m, 0.0f, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_mud_wet_full(float p_m) {
	mud_wet_full = CLAMP(p_m, 0.0f, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_data_map(const NodePath &p_path) {
	data_map = p_path;
	_update_cells();
}

// ---------------------------------------------------------------------------------------------------------
// Following and drawing

Vector3 GroundSurface3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		if (const Node3D *t = Object::cast_to<Node3D>(get_node_or_null(follow_target))) {
			return t->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		if (const Camera3D *cam = get_viewport()->get_camera_3d()) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void GroundSurface3D::_build_mesh() {
	// A tensor grid over the field: detail_spacing over its middle half, outer_spacing beyond.
	const float half = deform_extent * 0.5f;
	const float inner = deform_extent * 0.25f;
	LocalVector<float> pos;
	for (float x = detail_spacing; x < inner - 1e-4f; x += detail_spacing) {
		pos.push_back(x);
	}
	float x = pos.size() ? pos[pos.size() - 1] : 0.0f;
	while (x < half - 1e-4f) {
		x = MIN(x + outer_spacing, half);
		pos.push_back(x);
	}
	LocalVector<float> c;
	for (int i = int(pos.size()) - 1; i >= 0; i--) {
		c.push_back(-pos[i]);
	}
	c.push_back(0.0f);
	for (float p : pos) {
		c.push_back(p);
	}
	const int n = c.size();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	verts.resize(n * n);
	normals.resize(n * n);
	tangents.resize(n * n * 4);
	uvs.resize(n * n);
	for (int z = 0; z < n; z++) {
		for (int xi = 0; xi < n; xi++) {
			const int i = z * n + xi;
			verts.set(i, Vector3(c[xi], 0.0f, c[z]));
			normals.set(i, Vector3(0, 1, 0));
			uvs.set(i, Vector2(c[xi], c[z]));
			tangents.set(i * 4 + 0, 1.0f);
			tangents.set(i * 4 + 1, 0.0f);
			tangents.set(i * 4 + 2, 0.0f);
			tangents.set(i * 4 + 3, 1.0f);
		}
	}
	// A finer grid in the middle of the patch, so the geometry holds what is finer than the detail spacing for
	// both renderers: the near field's texels (docs/ideas/mud.md, stage 0) and the wind ripples' crests (sand
	// stage 3), the larger of their squares at the finer of their cells. The coarse quads round that hole fan
	// from their outer edge to the fine vertices on their inner one, so the two grids meet without cracks.
	int hole_lo = -1;
	int hole_hi = -1;
	int fine_k = 0;
	int fine_m = 0;
	float ring_w = 0.0f;
	float ring_s = 0.0f;
	if (_near_on()) {
		ring_w = 0.5f * near_extent;
		ring_s = near_extent / near_resolution;
	}
	if (wind_ripples && ripples.n > 0 && ripples.cell > 0.0f) {
		ring_w = MAX(ring_w, 0.5f * ripples.n * ripples.cell);
		ring_s = ring_s > 0.0f ? MIN(ring_s, ripples.cell) : ripples.cell;
	}
	// Inner vertices on the main field's texel centres, half a detail step off its corners: a vertex on a
	// corner reads the mean of four texels, so the coarse grid showed a 2x2 box blur of the field while the
	// finer ring read it unblurred, and a rut changed shape where it crossed the ring (a seam at the near
	// field's edge). On the centres both grids show the same bilinear field. The shift is a whole number of
	// fine steps, so the ring's vertices stay on the ripple cells (2.5 cm with 1.25 cm cells, within 0.6 cm
	// of the centres). The outer edge stays on the patch's rim.
	float shift = 0.5f * detail_spacing;
	if (ring_s > 0.0f) {
		shift = Math::floor(shift / ring_s + 1e-3f) * ring_s;
	}
	for (int i = 1; i + 1 < n; i++) {
		c[i] += shift;
	}
	for (int i = 0; i < n * n; i++) {
		verts.set(i, Vector3(c[i % n], 0.0f, c[i / n]));
		uvs.set(i, Vector2(c[i % n], c[i / n]));
	}
	if (ring_w > 0.0f && ring_s > 0.0f) {
		for (int i = 0; i < n; i++) {
			if (Math::abs(c[i] + ring_w - shift) < 1e-3f) {
				hole_lo = i;
			}
			if (Math::abs(c[i] - ring_w - shift) < 1e-3f) {
				hole_hi = i;
			}
		}
		fine_k = (int)Math::round(detail_spacing / ring_s);
		if (hole_lo < 0 || hole_hi <= hole_lo || fine_k < 2 || Math::abs(fine_k * ring_s - detail_spacing) > 1e-4f) {
			hole_lo = hole_hi = -1; // the grids do not line up (spacings changed): no fine ring
		} else {
			ring_s = detail_spacing / fine_k;
			fine_m = (hole_hi - hole_lo) * fine_k + 1;
		}
	}
	const int fine_base = n * n;
	if (fine_m > 0) {
		verts.resize(fine_base + fine_m * fine_m);
		normals.resize(fine_base + fine_m * fine_m);
		tangents.resize((fine_base + fine_m * fine_m) * 4);
		uvs.resize(fine_base + fine_m * fine_m);
		for (int z = 0; z < fine_m; z++) {
			for (int xi = 0; xi < fine_m; xi++) {
				const int i = fine_base + z * fine_m + xi;
				const Vector3 p(c[hole_lo] + xi * ring_s, 0.0f, c[hole_lo] + z * ring_s);
				verts.set(i, p);
				normals.set(i, Vector3(0, 1, 0));
				uvs.set(i, Vector2(p.x, p.z));
				tangents.set(i * 4 + 0, 1.0f);
				tangents.set(i * 4 + 1, 0.0f);
				tangents.set(i * 4 + 2, 0.0f);
				tangents.set(i * 4 + 3, 1.0f);
			}
		}
	}
	auto fine = [&](int p_x, int p_z) { return fine_base + p_z * fine_m + p_x; };
	LocalVector<int> tri;
	// Wound like the grid's (a, b, d) triangles: the normal of the xz winding points down y.
	auto add_tri = [&](int p_a, int p_b, int p_c) {
		const Vector3 va = verts[p_a];
		const Vector3 vb = verts[p_b];
		const Vector3 vc = verts[p_c];
		if ((vb - va).cross(vc - va).y > 0.0f) {
			SWAP(p_b, p_c);
		}
		tri.push_back(p_a);
		tri.push_back(p_b);
		tri.push_back(p_c);
	};
	const bool hole = fine_m > 0;
	for (int z = 0; z < n - 1; z++) {
		for (int xi = 0; xi < n - 1; xi++) {
			const bool in_x = hole && xi >= hole_lo && xi < hole_hi;
			const bool in_z = hole && z >= hole_lo && z < hole_hi;
			if (in_x && in_z) {
				continue; // the fine grid's
			}
			const int a = z * n + xi;
			const int b = a + 1;
			const int d = a + n;
			const int e = d + 1;
			// A quad on the hole's edge: a fan from its outer edge to the fine vertices on its inner one.
			int inner_side = -1; // 0: -z edge on the hole, 1: +z, 2: -x, 3: +x
			if (in_x && z == hole_lo - 1) {
				inner_side = 1;
			} else if (in_x && z == hole_hi) {
				inner_side = 0;
			} else if (in_z && xi == hole_lo - 1) {
				inner_side = 3;
			} else if (in_z && xi == hole_hi) {
				inner_side = 2;
			}
			if (inner_side < 0) {
				add_tri(a, b, d);
				add_tri(b, e, d);
				continue;
			}
			int o0, o1;
			LocalVector<int> f;
			if (inner_side <= 1) {
				const int fz = inner_side == 1 ? 0 : fine_m - 1;
				o0 = inner_side == 1 ? a : d;
				o1 = inner_side == 1 ? b : e;
				for (int k = 0; k <= fine_k; k++) {
					f.push_back(fine((xi - hole_lo) * fine_k + k, fz));
				}
			} else {
				const int fx = inner_side == 3 ? 0 : fine_m - 1;
				o0 = inner_side == 3 ? a : b;
				o1 = inner_side == 3 ? d : e;
				for (int k = 0; k <= fine_k; k++) {
					f.push_back(fine(fx, (z - hole_lo) * fine_k + k));
				}
			}
			const int mid = fine_k / 2;
			for (int k = 0; k < mid; k++) {
				add_tri(o0, f[k], f[k + 1]);
			}
			add_tri(o0, f[mid], o1);
			for (int k = mid; k < fine_k; k++) {
				add_tri(o1, f[k], f[k + 1]);
			}
		}
	}
	if (hole) {
		for (int z = 0; z < fine_m - 1; z++) {
			for (int xi = 0; xi < fine_m - 1; xi++) {
				const int a = fine(xi, z);
				const int b = a + 1;
				const int d = a + fine_m;
				const int e = d + 1;
				tri.push_back(a);
				tri.push_back(b);
				tri.push_back(d);
				tri.push_back(b);
				tri.push_back(e);
				tri.push_back(d);
			}
		}
	}
	PackedInt32Array idx;
	idx.resize(tri.size());
	for (uint32_t k = 0; k < tri.size(); k++) {
		idx.set(k, tri[k]);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (mesh_instance) {
		mesh_instance->set_mesh(mesh);
	}
	mesh_dirty = false;
}

void GroundSurface3D::_update_patch_material() {
	Ref<Material> src = material;
	if (src.is_null()) {
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(0.42f, 0.35f, 0.27f));
		m->set_roughness(0.95f);
		m->set_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM, true);
		src = m;
	}
	patch_material = src->duplicate();
	if (BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(patch_material.ptr())) {
		bm->set_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM, true);
		bm->set_ground_deform_patch(true);
		patch_uv_offset = bm->get_uv1_offset();
	} else if (ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(patch_material.ptr())) {
		sm->set_shader_parameter("ground_deform_patch", true);
	}
	if (mesh_instance) {
		mesh_instance->set_material_override(patch_material);
	}
}

void GroundSurface3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_as_top_level(true);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
		_update_patch_material();
	}
	if (mesh_dirty) {
		_build_mesh();
	}
}

void GroundSurface3D::_follow(Vector3 p_center) {
	field.extent = deform_extent;
	field.resolution = deform_resolution;
	// On the GPU path the tile cache is the GPU's; the CPU copy keeps none.
	field.max_cached_tiles = _gpu_on() ? 0 : max_cached_tiles;
	field.track_fresh = true;
	const Vector2 xz(p_center.x, p_center.z);
	const Vector2i want = field.origin_for(xz);
	const bool moved = !field.valid || want != field.origin;
	if (moved) {
		if (_gpu_on()) {
			gpu_host.shift_to(field, want, deform_resolution, max_cached_tiles);
		}
		field.shift(want);
	}
	_near_follow();
	const Rect2 r = field.rect();
	const int n = int(Math::ceil(deform_extent / ground_spacing)) + 3;
	const Vector2i go(int(Math::floor(r.position.x / ground_spacing)) - 1, int(Math::floor(r.position.y / ground_spacing)) - 1);
	if (!ground_valid || go != ground_origin || n != ground_n) {
		_sample_ground(go, n);
		globals_published = false;
	}
	const Vector2 c = r.position + r.size * 0.5f;
	if (moved || c != center || (mesh_instance && mesh_instance->get_global_position() != Vector3(c.x, 0, c.y))) {
		center = c;
		if (mesh_instance) {
			mesh_instance->set_global_transform(Transform3D(Basis(), Vector3(c.x, 0.0f, c.y)));
			mesh_instance->set_custom_aabb(AABB(Vector3(-r.size.x * 0.5f, ground_min - skirt - 1.0f, -r.size.y * 0.5f), Vector3(r.size.x, ground_max - ground_min + skirt + 2.0f, r.size.y)));
		}
		// Mesh UVs are metres from the patch centre: keep the material's world mapping.
		if (BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(patch_material.ptr())) {
			const Vector3 s = bm->get_uv1_scale();
			bm->set_uv1_offset(patch_uv_offset + Vector3(Math::fposmod(c.x * s.x, 1.0f), Math::fposmod(c.y * s.y, 1.0f), 0.0f));
		}
		globals_published = false;
	}
}

void GroundSurface3D::_set_global(const StringName &p_name, const Variant &p_value, int p_type) {
	RenderingServer *rs = RenderingServer::get_singleton();
	static HashSet<StringName> added;
	if (!added.has(p_name) && !ProjectSettings::get_singleton()->has_setting("shader_globals/" + String(p_name))) {
		added.insert(p_name);
		rs->global_shader_parameter_add(p_name, RSE::GlobalShaderParameterType(p_type), p_value);
		return;
	}
	rs->global_shader_parameter_set(p_name, p_value);
}

void GroundSurface3D::_publish() {
	const bool gpu_tex = _gpu_on() && gpu_host.texture.is_valid() && gpu_host.texture->get_texture_rd_rid().is_valid();
	if (active != this || globals_published || (!gpu_tex && deform_texture.is_null()) || height_texture.is_null()) {
		return;
	}
	const Rect2 r = field.rect();
	_set_global("ground_deform_map", gpu_tex ? gpu_host.texture->get_rid() : deform_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	_set_global("ground_deform_height", height_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	_set_global("ground_deform_rect", Vector4(r.position.x, r.position.y, r.size.x, field.texel()), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("ground_deform_height_rect", Vector4((ground_origin.x - 0.5f) * ground_spacing, (ground_origin.y - 0.5f) * ground_spacing, ground_n * ground_spacing, ground_n * ground_spacing), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("ground_deform_skirt", Vector4(skirt, terrain_margin, skirt_fade, 0.0f), RSE::GLOBAL_VAR_TYPE_VEC4);
	_publish_near();
	globals_published = true;
}

// ---------------------------------------------------------------------------------------------------------
// The finer near field (docs/ideas/mud.md, stage 0)

bool GroundSurface3D::_near_on() const {
	return near_field && _gpu_on() && near_resolution >= GroundDeformField::TILE && near_extent > 0.0f && near_extent < deform_extent * 0.5f;
}

int GroundSurface3D::_near_passes() const {
	// Slumping spreads like diffusion: a slope n times finer needs n squared passes to move as far in a second.
	const float ratio = near_fld.valid ? field.texel() / MAX(near_fld.texel(), 1e-4f) : 1.0f;
	return CLAMP((int)Math::round(ratio * ratio), 1, 16);
}

bool GroundSurface3D::_near_inside(float p_x, float p_z) const {
	if (!is_near_active()) {
		return false;
	}
	const Rect2 r = near_fld.rect().grow(-NEAR_BLEND);
	return r.has_point(Vector2(p_x, p_z));
}

Rect2 GroundSurface3D::get_near_rect() const {
	return is_near_active() ? near_fld.rect() : Rect2();
}

void GroundSurface3D::_near_release() {
	field.stamp_step = 0.0f;
	near_host.release();
	near_fld.invalidate();
	near_fld.clear();
	near_ground_dirty = true;
	near_cells_dirty = true;
	near_slow_accum = 0.0;
	globals_published = false;
}

void GroundSurface3D::_near_follow() {
	if (!_near_on() || !field.valid) {
		if (near_fld.valid || near_host.gpu.is_valid()) {
			_near_release();
		}
		return;
	}
	near_fld.extent = near_extent;
	near_fld.resolution = near_resolution;
	near_fld.max_cached_tiles = 0; // the GPU's
	near_fld.track_fresh = true;
	// Centred on the main field, whose centre moves in its own (larger) tiles.
	const Rect2 r = field.rect();
	const Vector2i want = near_fld.origin_for(r.position + r.size * 0.5f);
	if (near_fld.valid && want == near_fld.origin) {
		return;
	}
	if (near_fld.valid && !near_host.reset) {
		_near_seed(want);
	}
	near_host.shift_to(near_fld, want, near_resolution, max_cached_tiles);
	near_fld.shift(want);
	field.stamp_step = near_fld.texel();
	globals_published = false;
}

void GroundSurface3D::_near_seed(Vector2i p_origin) {
	// Tiles coming into the near field that it has never held (and so has not cached) start from the main
	// field's copy, sampled bilinearly, so a print made while the near field was elsewhere does not vanish
	// when it arrives. The main copy is the latest read back (at most a fifteenth of a second old).
	const int T = GroundDeformField::TILE;
	const int nt = near_fld.tiles();
	const Vector2i old0 = near_fld.origin / T;
	const Vector2i new0 = p_origin / T;
	const float t = near_fld.texel();
	LocalVector<uint32_t> data;
	data.resize(T * T * 2);
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = new0 + Vector2i(tx, tz);
			const Vector2i o = w - old0;
			if (o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) {
				continue; // kept from the old near field
			}
			if (near_host.has_cached(w)) {
				continue;
			}
			bool any = false;
			for (int z = 0; z < T; z++) {
				for (int x = 0; x < T; x++) {
					const float wx = (w.x * T + x + 0.5f) * t;
					const float wz = (w.y * T + z + 0.5f) * t;
					const float dn = field.dent_at(wx, wz);
					const float rm = field.rim_at(wx, wz);
					const float fr = field.sample(field.fresh, wx, wz);
					const float wd = int(field.water.size()) == field.resolution * field.resolution ? field.sample(field.water, wx, wz) : 0.0f;
					any = any || dn > 0.0f || rm > 0.0f || fr > 0.0f || wd > 0.0f;
					const int k = z * T + x;
					data[k * 2 + 0] = uint32_t(Math::make_half_float(dn)) | (uint32_t(Math::make_half_float(rm)) << 16);
					data[k * 2 + 1] = uint32_t(Math::make_half_float(fr)) | (uint32_t(Math::make_half_float(wd)) << 16);
				}
			}
			if (any) {
				near_host.seed_tile(w, data.ptr(), 2, max_cached_tiles);
			}
		}
	}
}

void GroundSurface3D::_publish_near() {
	const bool on = is_near_active() && near_host.texture.is_valid() && near_host.texture->get_texture_rd_rid().is_valid();
	if (!on) {
		_set_global("ground_deform_near_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
		return;
	}
	const Rect2 r = near_fld.rect();
	_set_global("ground_deform_near_map", near_host.texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	_set_global("ground_deform_near_rect", Vector4(r.position.x, r.position.y, r.size.x, near_fld.texel()), RSE::GLOBAL_VAR_TYPE_VEC4);
}

void GroundSurface3D::set_near_field(bool p_enabled) {
	if (near_field == p_enabled) {
		return;
	}
	near_field = p_enabled;
	mesh_dirty = true;
	if (!near_field) {
		_near_release();
	}
	globals_published = false;
	if (is_inside_tree()) {
		_ensure_children();
		_follow(_follow_position());
		_publish();
	}
}

void GroundSurface3D::set_near_extent(float p_extent) {
	near_extent = MAX(p_extent, 1.0f);
	mesh_dirty = true;
	_near_release();
	if (is_inside_tree()) {
		_ensure_children();
		_follow(_follow_position());
		_publish();
	}
}

void GroundSurface3D::set_near_resolution(int p_resolution) {
	near_resolution = MAX(p_resolution / GroundDeformField::TILE, 1) * GroundDeformField::TILE;
	mesh_dirty = true;
	_near_release();
	if (is_inside_tree()) {
		_ensure_children();
		_follow(_follow_position());
		_publish();
	}
}

void GroundSurface3D::_upload() {
	const int n = field.resolution;
	if (!field.valid) {
		return;
	}
	tint_albedo.resize(n * n);
	tint_roughness.resize(n * n);
	const int nt = field.tiles();
	const float t = field.texel();
	const int T = GroundDeformField::TILE;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const bool on = field.tile_active[tz * nt + tx];
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					const float f = on ? field.fresh[i] : 0.0f;
					if (f <= 0.0f) {
						tint_albedo[i] = 1.0f;
						tint_roughness[i] = 1.0f;
						continue;
					}
					const Cell &c = _cell_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t);
					tint_albedo[i] = 1.0f - f * c.darken;
					tint_roughness[i] = Math::lerp(1.0f, c.gloss, f);
				}
			}
		}
	}
	const RID before = deform_texture.is_valid() ? deform_texture->get_rid() : RID();
	field.upload(deform_image, deform_texture, &tint_albedo, &tint_roughness);
	if (deform_texture.is_valid() && deform_texture->get_rid() != before) {
		globals_published = false;
	}
}

Vector2 GroundSurface3D::_wind_10m() const {
	if (wind_override != Vector2()) {
		return wind_override;
	}
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	if (!wm) {
		return Vector2();
	}
	const Vector3 w = wm->get_wind_at(Vector3(center.x, _ground_at(center.x, center.y), center.y), 10.0f);
	return Vector2(w.x, w.z);
}

void GroundSurface3D::set_wind_ripples(bool p_enabled) {
	if (wind_ripples == p_enabled) {
		return;
	}
	wind_ripples = p_enabled;
	mesh_dirty = true;
	ripples.valid = false;
	ripple_seen.clear();
	ripple_mask_accum = 1e9;
	if (is_inside_tree()) {
		_ensure_children();
	}
	if (!wind_ripples) {
		_ripples_publish();
	}
}

PackedFloat32Array GroundSurface3D::get_ripple_heights() const {
	PackedFloat32Array out;
	out.resize(ripples.h.size());
	for (uint32_t i = 0; i < ripples.h.size(); i++) {
		out.set(i, ripples.h[i]);
	}
	return out;
}

void GroundSurface3D::_ripple_grow_task(void *p_self) {
	GroundSurface3D *self = (GroundSurface3D *)p_self;
	GroundRipples::grow(self->ripple_grown, self->ripples.n, self->ripples, self->ripple_grow_wind, 500, 7);
}

void GroundSurface3D::_shift_lattice(LocalVector<float> &r_values, Vector2i p_old_origin, Vector2i p_new_origin, int p_n, int p_stride) {
	LocalVector<float> out;
	out.resize(p_n * p_n * p_stride);
	const Vector2i d = p_new_origin - p_old_origin;
	for (int z = 0; z < p_n; z++) {
		for (int x = 0; x < p_n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			const bool in = sx >= 0 && sz >= 0 && sx < p_n && sz < p_n;
			for (int k = 0; k < p_stride; k++) {
				out[(z * p_n + x) * p_stride + k] = in ? r_values[(sz * p_n + sx) * p_stride + k] : 0.0f;
			}
		}
	}
	r_values = out;
}

// Sand and water (docs/ideas/sand.md, stage 4). Twice a second, on a 1 m grid over the lattice: the water over
// the ground from the water_sources (a surface's swash film and depth, a river's or live water's depth, and
// their flow) and the weather map's contact wetness; then each cell's pore water in its top 2 cm (porosity 0.4):
// filled under water by Green-Ampt infiltration (fine sand, k 1e-4 m/s, capillary head 15 cm: about 10 s),
// drained by gravity to field capacity (15%) in a minute or two, then dried by evaporation (1e-7 m/s, hours).
// Cells whose water changed are rebuilt with it.
// The water over p (ground height p_ground) from the water sources: the deepest of them, as depth (a swash film
// counts as its depth) and its speed.
static void ground_water_at(const LocalVector<Node *> &p_sources, const Vector3 &p, float p_ground, float &r_depth, float &r_speed) {
	static const StringName s_swash = "sample_swash";
	static const StringName s_depth = "sample_depth";
	static const StringName s_height = "sample_height";
	static const StringName s_flow = "sample_flow";
	const float gh = p_ground;
	float depth = 0.0f;
	float speed = 0.0f;
	for (Node *src : p_sources) {
		float d = 0.0f;
		float film = 0.0f;
		if (src->has_method(s_swash)) {
			const Vector2 sw = src->call(s_swash, p);
			film = MAX(sw.x, 0.0f);
		}
		if (src->has_method(s_depth)) {
			d = src->call(s_depth, p);
		} else if (src->has_method(s_height)) {
			d = float(src->call(s_height, p)) - gh;
		}
		if (!Math::is_finite(d)) {
			d = 0.0f;
		}
		d = MAX(d, 0.0f);
		float v = 0.0f;
		if (d > 0.0f && src->has_method(s_flow)) {
			const Vector3 f = src->call(s_flow, p);
			v = Vector2(f.x, f.z).length();
		}
		if (d > 0.0f && film <= d && src->has_method(s_swash)) {
			// The inner surf: broken waves cross the shallows as bores, the wave height held at 0.8 of the depth
			// (saturated breaking, Thornton and Guza 1983). Behind a bore of height H on depth h, which runs at
			// c = sqrt(g (h + H)), mass conservation gives the water u = c H / (h + H) (Stoker 1957; Svendsen
			// 1984): 0.6 sqrt(g h). That sweeps prints flat that the sea's mean flow alone would leave standing.
			// Shoreward of where the shore waves break (h < H / 0.8).
			static const StringName s_shore = "shore_waves";
			static const StringName s_shore_h = "shore_wave_height";
			bool valid = false;
			const Variant on = src->get(s_shore, &valid);
			if (valid && bool(on)) {
				const float hb = float(src->get(s_shore_h)) / 0.8f;
				if (d < hb) {
					v = MAX(v, 0.6f * Math::sqrt(9.81f * d));
				}
			}
		}
		if (film > d) {
			// The swash's run-up: a thin bore moves at about twice the shallow-water speed of its film.
			v = MAX(v, 2.0f * Math::sqrt(9.81f * film));
			d = film;
		}
		if (d > depth) {
			depth = d;
			speed = v;
		}
	}
	r_depth = depth;
	r_speed = speed;
}

// One cell's pore water over p_dt (see _water_update): Green-Ampt filling under water, gravity drainage to field
// capacity, evaporation; at least the weather's contact wetness; with runoff_channels, the water table a sea or
// lake left.
float GroundSurface3D::_pore_step(float p_s, float p_depth, float p_contact, float p_dt, float &r_table) const {
	const float infil = 1e-4f * (1.0f + 0.15f / 0.02f) / (0.4f * 0.02f); // per s: k (1 + hc / L) / (phi L)
	const float drain = 1e-4f / (0.4f * 0.02f); // per s: k / (phi L)
	const float evap = 1e-7f / (0.4f * 0.02f); // saturation per s
	const float fc = 0.15f;
	float s = p_s;
	if (p_depth > 0.001f) {
		s += (1.0f - s) * MIN(infil * p_dt, 1.0f);
	} else if (s > fc) {
		s = MAX(fc, s - (s - fc) * MIN(drain * p_dt, 1.0f));
	} else {
		s = MAX(0.0f, s - evap * p_dt);
	}
	// Where the water has been (the weather map's contact wetness, drying with the weather), at least that wet.
	s = MAX(s, CLAMP(p_contact, 0.0f, 1.0f));
	if (runoff_channels) {
		// The beach's water table (runoff_channels), as its depth under the surface (negative: a head above
		// it): a sea or lake fills the sand to its own level; once the water goes the table falls slowly, the
		// beach draining through its body to the sea (Boussinesq: k h tan(beta) / (n_e l), k 1e-4 m/s, aquifer
		// 2 m, porosity 0.3, over a 6 m face: about 1e-5 m/s, a few cm an hour). So the table falls behind a
		// falling tide and leaves the lower beach seeping for hours (the decoupled exit point, Turner 1993).
		// The capillary fringe (15 cm in fine sand) keeps the top soaked above it.
		r_table = p_depth > 0.1f ? MIN(r_table, -p_depth) : MIN(r_table + 1e-5f * p_dt * runoff_time_scale, 10.0f);
		s = MAX(s, 1.0f - 0.6f * MAX(r_table, 0.0f) / 0.15f);
	}
	return s;
}

void GroundSurface3D::_far_update(double p_delta, const LocalVector<Node *> &p_sources, const WeatherMap3D *p_weather) {
	const int n = ground_n;
	const int fn = n;
	const float fs = ground_spacing * FAR_STEP;
	// Centred on the fine lattice, in far cells.
	const Vector2i fo(Math::floor((ground_origin.x + 0.5f * n) / FAR_STEP) - fn / 2, Math::floor((ground_origin.y + 0.5f * n) / FAR_STEP) - fn / 2);
	const GroundHeightQuery q = GroundHeightQuery::make(this, height_source, ground_collision_mask);
	auto sample_ground = [&](int x, int z) {
		const float h = q.height(Vector3((fo.x + x) * fs, 0.0f, (fo.y + z) * fs));
		return Math::is_finite(h) ? h : q.y0;
	};
	if (far_n != fn || (int)far_pore.size() != fn * fn) {
		far_n = fn;
		far_origin = fo;
		far_pore.resize(fn * fn);
		far_table.resize(fn * fn);
		far_ground.resize(fn * fn);
		for (int z = 0; z < fn; z++) {
			for (int x = 0; x < fn; x++) {
				far_pore[z * fn + x] = 0.0f;
				far_table[z * fn + x] = 10.0f;
				far_ground[z * fn + x] = sample_ground(x, z);
			}
		}
		for (int k = 0; k < FAR_PHASES; k++) {
			far_age[k] = 0.0;
		}
	} else if (fo != far_origin) {
		LocalVector<float> np, nt, ng;
		np.resize(fn * fn);
		nt.resize(fn * fn);
		ng.resize(fn * fn);
		const Vector2i d = fo - far_origin;
		for (int z = 0; z < fn; z++) {
			for (int x = 0; x < fn; x++) {
				const int sx = x + d.x;
				const int sz = z + d.y;
				const int i = z * fn + x;
				if (sx >= 0 && sz >= 0 && sx < fn && sz < fn) {
					const int j = sz * fn + sx;
					np[i] = far_pore[j];
					nt[i] = far_table[j];
					ng[i] = far_ground[j];
				} else {
					np[i] = 0.0f;
					nt[i] = 10.0f;
					ng[i] = sample_ground(x, z);
				}
			}
		}
		far_pore = np;
		far_table = nt;
		far_ground = ng;
		far_origin = fo;
	}
	for (int k = 0; k < FAR_PHASES; k++) {
		far_age[k] += p_delta;
	}
	const int phase = far_phase;
	far_phase = (far_phase + 1) % FAR_PHASES;
	const float dt = float(far_age[phase]);
	far_age[phase] = 0.0;
	for (int z = phase; z < fn; z += FAR_PHASES) {
		for (int x = 0; x < fn; x++) {
			const int i = z * fn + x;
			const float wx = (far_origin.x + x) * fs;
			const float wz = (far_origin.y + z) * fs;
			// Inside the fine lattice: its own value (the fine lattice rules there).
			const int lx = (int)Math::round(wx / ground_spacing) - ground_origin.x;
			const int lz = (int)Math::round(wz / ground_spacing) - ground_origin.y;
			if (lx >= 0 && lz >= 0 && lx < n && lz < n) {
				far_pore[i] = pore[lz * n + lx];
				if ((int)water_table.size() == n * n) {
					far_table[i] = water_table[lz * n + lx];
				}
				continue;
			}
			const Vector3 p(wx, far_ground[i], wz);
			float depth = 0.0f;
			float speed = 0.0f;
			ground_water_at(p_sources, p, far_ground[i], depth, speed);
			const float contact = p_weather ? p_weather->get_contact_wetness_at(p) : 0.0f;
			far_pore[i] = _pore_step(far_pore[i], depth, contact, dt, far_table[i]);
		}
	}
}

float GroundSurface3D::_far_pore_at(float p_x, float p_z) const {
	const int fn = far_n;
	if (fn < 2 || (int)far_pore.size() != fn * fn) {
		return 0.0f;
	}
	const float fs = ground_spacing * FAR_STEP;
	const float fx = CLAMP(p_x / fs - far_origin.x, 0.0f, float(fn - 1));
	const float fz = CLAMP(p_z / fs - far_origin.y, 0.0f, float(fn - 1));
	const int x0 = MIN(int(fx), fn - 2);
	const int z0 = MIN(int(fz), fn - 2);
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int x, int z) { return far_pore[z * fn + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

void GroundSurface3D::_water_update(double p_delta) {
	const int n = ground_n;
	if (!water_coupling || !ground_valid || n < 2 || (int)pore.size() != n * n || (int)cell_inputs.size() != n * n) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	LocalVector<Node *> sources;
	if (is_inside_tree()) {
		for (int k = 0; k < water_sources.size(); k++) {
			Node *src = get_node_or_null(water_sources[k]);
			if (src) {
				sources.push_back(src);
			}
		}
	}
	if (sources.is_empty() && !wm) {
		return;
	}
	const int step = MAX(1, (int)Math::round(1.0f / ground_spacing));
	const int gn = (n - 1) / step + 2;
	LocalVector<float> g_depth, g_speed, g_contact;
	g_depth.resize(gn * gn);
	g_speed.resize(gn * gn);
	g_contact.resize(gn * gn);
	for (int gz = 0; gz < gn; gz++) {
		for (int gx = 0; gx < gn; gx++) {
			const int lx = MIN(gx * step, n - 1);
			const int lz = MIN(gz * step, n - 1);
			const float gh = ground[lz * n + lx];
			const Vector3 p((ground_origin.x + lx) * ground_spacing, gh, (ground_origin.y + lz) * ground_spacing);
			float depth = 0.0f;
			float speed = 0.0f;
			ground_water_at(sources, p, gh, depth, speed);
			g_depth[gz * gn + gx] = depth;
			g_speed[gz * gn + gx] = speed;
			g_contact[gz * gn + gx] = wm ? wm->get_contact_wetness_at(p) : 0.0f;
		}
	}
	const float dt = float(p_delta);
	bool changed = false;
	LocalVector<float> &cell_depth = water_depth;
	cell_depth.resize(n * n);
	if ((int)water_table.size() != n * n) {
		water_table.resize(n * n);
		for (int i = 0; i < n * n; i++) {
			water_table[i] = 10.0f; // none near the surface
		}
	}
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float fx = float(x) / step;
			const float fz = float(z) / step;
			const int x0 = MIN((int)fx, gn - 2);
			const int z0 = MIN((int)fz, gn - 2);
			const float tx = fx - x0;
			const float tz = fz - z0;
			auto bil = [&](const LocalVector<float> &g) {
				return Math::lerp(Math::lerp(g[z0 * gn + x0], g[z0 * gn + x0 + 1], tx), Math::lerp(g[(z0 + 1) * gn + x0], g[(z0 + 1) * gn + x0 + 1], tx), tz);
			};
			float depth = bil(g_depth);
			if (standing_water) {
				// Standing water in a rut soaks the ground under it (and so softens the mud there).
				depth = MAX(depth, get_standing_water_at(Vector3((ground_origin.x + x) * ground_spacing, 0.0f, (ground_origin.y + z) * ground_spacing)));
			}
			cell_depth[i] = depth;
			const float contact = bil(g_contact);
			const float s = _pore_step(pore[i], depth, contact, dt, water_table[i]);
			pore[i] = s;
			water_speed[i] = depth > 0.001f ? bil(g_speed) : 0.0f;
			// Rebuilt when its water changed enough to matter.
			float &bs = cell_water[i * 2 + 0];
			float &bv = cell_water[i * 2 + 1];
			if (Math::abs(s - bs) > 0.02f || Math::abs(water_speed[i] - bv) > 0.05f || (s == 0.0f && bs != 0.0f)) {
				const CellInput &in = cell_inputs[i];
				Cell c = material_cell(in.sand, in.moisture, in.litter, s, water_speed[i], in.mud);
				c.deformable = !in.fixed;
				cells[i] = c;
				bs = s;
				bv = water_speed[i];
				changed = true;
			}
		}
	}
	if (changed) {
		gpu_cells_dirty = true;
		near_cells_dirty = true;
	}
	_far_update(p_delta, sources, wm);
	water_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
}

// Smooth value noise in 0..1 on a unit lattice (hashed corners, smoothstep blend).
float GroundSurface3D::_value_noise(float p_x, float p_z) {
	const int x0 = (int)Math::floor(p_x);
	const int z0 = (int)Math::floor(p_z);
	auto h = [](int p_a, int p_b) {
		uint32_t v = uint32_t(p_a) * 73856093u ^ uint32_t(p_b) * 19349663u;
		v ^= v >> 16;
		v *= 0x7feb352du;
		v ^= v >> 15;
		v *= 0x846ca68bu;
		v ^= v >> 16;
		return float(v & 0xffffu) / 65535.0f;
	};
	const float fx = p_x - x0;
	const float fz = p_z - z0;
	const float sx = fx * fx * (3.0f - 2.0f * fx);
	const float sz = fz * fz * (3.0f - 2.0f * fz);
	return Math::lerp(Math::lerp(h(x0, z0), h(x0 + 1, z0), sx), Math::lerp(h(x0, z0 + 1), h(x0 + 1, z0 + 1), sx), sz);
}

void GroundSurface3D::_wet_publish(bool p_on) {
	const int n = ground_n;
	const bool wet = water_coupling && (int)pore.size() == n * n;
	const bool on = p_on && active == this && ground_valid && n >= 2 && (int)cells.size() == n * n && (wet || sand_glints);
	if (!on && wet_texture.is_null()) {
		return;
	}
	// Layout: the fine lattice's n rows; then, with pore water, the far lattice's n rows; then a row whose texel 0
	// is the fine lattice's rect (tag 2.5) and texel 1 the far one's (tag 3.5). material.cpp ground_wet_at() and
	// scene_raytracing_raygen.glsl read it.
	const bool far = on && wet && far_n == n && (int)far_pore.size() == n * n;
	// Then, with desiccation cracks, their block (opening, plate size, regularity, crust; texel 2's tag 4.5).
	bool cracks = false;
	if (on && desiccation_cracks && (int)crack_state.size() == n * n * CRACK_FLOATS && n >= 3) {
		for (int i = 0; i < n * n && !cracks; i++) {
			cracks = crack_state[i * CRACK_FLOATS] > 0.0f && crack_state[i * CRACK_FLOATS] < 1.999f;
		}
	}
	const int blocks = 1 + (far ? 1 : 0) + (cracks ? 1 : 0);
	const int w = on ? n : (wet_texture.is_valid() ? wet_texture->get_width() : 1);
	const int h = on ? blocks * n + 1 : (wet_texture.is_valid() ? wet_texture->get_height() : 2);
	Vector<uint8_t> data;
	data.resize(w * h * 16);
	memset(data.ptrw(), 0, data.size());
	if (on) {
		float *d = (float *)data.ptrw();
		for (int i = 0; i < n * n; i++) {
			const float s = wet ? pore[i] : 0.0f;
			d[i * 4 + 0] = Math::smoothstep(0.03f, 0.4f, s);
			// A film of water stands on sand at saturation (under water the water's own surface shows instead).
			d[i * 4 + 1] = Math::smoothstep(0.95f, 1.0f, s);
			// Loose sand shows its grains: not under litter, not where cemented or fixed.
			const Cell &c = cells[i];
			d[i * 4 + 2] = sand_glints && c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) : 0.0f;
			// The fine lattice's weight (A): toward the field's edge the patch tucks under the terrain, and both
			// read the far lattice there. The line where it hands over is wobbled by a smooth value noise (about
			// 2 m across, up to 0.9 m either way). Grains show only on the patch.
			const float wx = (ground_origin.x + i % n) * ground_spacing;
			const float wz = (ground_origin.y + i / n) * ground_spacing;
			const float edge = _edge_distance(wx, wz) + 1.8f * (_value_noise(wx / 2.1f, wz / 2.1f) - 0.5f);
			d[i * 4 + 3] = Math::smoothstep(terrain_margin, terrain_margin + skirt_fade, edge);
		}
		float *r = d + blocks * n * n * 4;
		if (cracks) {
			float *cd = d + (blocks - 1) * n * n * 4;
			for (int i = 0; i < n * n; i++) {
				float damp = 0.0f;
				const Vector4 c = _crack_at_cell(i, &damp);
				cd[i * 4 + 0] = c.x;
				cd[i * 4 + 1] = c.y;
				cd[i * 4 + 2] = c.z;
				cd[i * 4 + 3] = c.w - damp; // the crust, or (negative) the drying layer still dark with water
			}
			r[11] = 4.5f;
		}
		if (far) {
			float *fd = d + n * n * 4;
			for (int i = 0; i < n * n; i++) {
				fd[i * 4 + 0] = Math::smoothstep(0.03f, 0.4f, far_pore[i]);
				fd[i * 4 + 1] = Math::smoothstep(0.95f, 1.0f, far_pore[i]);
			}
			const float fs = ground_spacing * FAR_STEP;
			r[4] = (far_origin.x - 0.5f) * fs;
			r[5] = (far_origin.y - 0.5f) * fs;
			r[6] = n * fs;
			r[7] = 3.5f;
		}
		// Texel centres on the lattice's vertices.
		r[0] = (ground_origin.x - 0.5f) * ground_spacing;
		r[1] = (ground_origin.y - 0.5f) * ground_spacing;
		r[2] = n * ground_spacing;
		r[3] = 2.5f; // tags a published map (material.cpp ground_wet_at)
	}
	if (data == wet_data && wet_published) {
		return;
	}
	wet_data = data;
	Ref<Image> img = Image::create_from_data(w, h, false, Image::FORMAT_RGBAF, data);
	if (wet_texture.is_valid() && wet_texture->get_width() == w && wet_texture->get_height() == h) {
		wet_texture->update(img);
	} else {
		wet_texture = ImageTexture::create_from_image(img);
		wet_published = false;
	}
	if (!wet_published && (on || active == this)) {
		_set_global("ground_wet_map", wet_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		wet_published = true;
	}
}

// Dune grass and other plants on sand (docs/ideas/sand.md). Twice a second: the frontal area index lambda of the
// plants over each lattice cell, from the wind_shelters on a 1 m grid; Raupach's (1993) drag partition gives the
// friction speed left on the sand between them, and with it the saltation flux the cell can carry (its capacity,
// GroundRipples::saltation_flux, times the share of its sand the wind can move). The flux carried relaxes to that
// capacity over the saturation length (about 1 m for dune sand, Andreotti et al. 2010), cell by cell downwind;
// where it falls the sand it drops is the deposition, dh/dt = -dQ/dx / bulk density (Exner), written to the
// ground in 2 mm steps. Sand blowing in from upwind of the patch is taken as saturated over its own cell.
float GroundSurface3D::shelter_ratio_for(float p_lambda) {
	const float lambda = CLAMP(p_lambda, 0.0f, 0.5f);
	// m 0.5, sigma 1 (basal to frontal area), beta 150 (element to surface drag, grass: Wolfe and Nickling 1996).
	const float m = 0.5f;
	return 1.0f / Math::sqrt(MAX((1.0f - m * lambda) * (1.0f + m * 150.0f * lambda), 1e-3f));
}

float GroundSurface3D::_shelter_flux_at(float p_x, float p_z) const {
	const int n = ground_n;
	if ((int)shelter_flux.size() != n * n || n <= 0) {
		return 1.0f;
	}
	const int x = int(Math::round(p_x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? shelter_flux[z * n + x] : 1.0f;
}

float GroundSurface3D::_lattice_value(const LocalVector<float> &p_values, const Vector3 &p_position) const {
	const int n = ground_n;
	if (!ground_valid || (int)p_values.size() != n * n) {
		return 0.0f;
	}
	const int x = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? p_values[z * n + x] : 0.0f;
}

Vector2 GroundSurface3D::get_sand_flux_at(const Vector3 &p_position) const {
	return sand_flux_dir * _lattice_value(sand_flux, p_position);
}

float GroundSurface3D::get_deposition_rate_at(const Vector3 &p_position) const {
	return _lattice_value(sand_dep_rate, p_position);
}

float GroundSurface3D::get_sand_deposit_at(const Vector3 &p_position) const {
	return _lattice_value(sand_dep, p_position);
}

// Runoff channels (docs/ideas/sand.md). Twice a second, on a grid of twice the deform field's texel over the field:
// - Sources. Soaked sand on a slope seeps where its water table meets the surface (a seepage face, Turner 1993):
//   Darcy outflow k sin(beta), k 1e-4 m/s, while the table a falling tide left (_water_update) is within 2 cm. Rain
//   runs off where the ground cannot take it: all of it on saturated sand, the excess over k elsewhere (k from
//   1e-6 m/s on soil to 1e-4 on sand).
// - Routing. Cells in falling height order pass their discharge downhill to all lower neighbours, weighted by
//   slope^1.1 (multiple flow directions, Freeman 1991), so sheet flow spreads on a plane and gathers in hollows.
//   Water reaching standing water (the sea, a river) leaves the grid.
// - Flow. Manning sheet flow (n 0.025): depth h = (n q / sqrt(S))^0.6 for unit discharge q, bed shear rho g h S.
// - Sand. Meyer-Peter and Mueller bed load at that shear is the capacity; a cell carrying less picks the
//   difference up, one carrying more lays it down (transport-limited Exner, porosity 0.4). Where flow gathers
//   it cuts deeper, gathers more and cuts on: rills (Schorghofer et al. 2004). The field's own slumping keeps
//   their walls at the angle of repose. Cuts and fills go to the field in 1 mm steps.
// The seepage face's outflow at lattice cell p_i (m/s): where the table a falling tide left lies within 2 cm of the
// surface, Darcy outflow k sin(beta), k 1e-4 m/s for fine sand (Turner 1993); none under standing water.
float GroundSurface3D::_runoff_seep(int p_i) const {
	const int n = ground_n;
	if (!water_coupling || (int)water_table.size() != n * n || (int)water_depth.size() != n * n || (int)cells.size() != n * n) {
		return 0.0f;
	}
	if (water_depth[p_i] > 0.002f) {
		return 0.0f;
	}
	const float tab = water_table[p_i];
	const Cell &cell = cells[p_i];
	if (tab >= 0.02f || cell.sand <= 0.0f || !cell.deformable) {
		return 0.0f;
	}
	const int lx = CLAMP(p_i % n, 1, n - 2);
	const int lz = CLAMP(p_i / n, 1, n - 2);
	const int l = lz * n + lx;
	const float gx = (ground[l + 1] - ground[l - 1]) / (2.0f * ground_spacing);
	const float gz = (ground[l + n] - ground[l - n]) / (2.0f * ground_spacing);
	const float g2 = gx * gx + gz * gz;
	return 1e-4f * Math::sqrt(g2 / (1.0f + g2)) * MIN(1.0f - tab / 0.02f, 1.0f) * cell.sand;
}

void GroundSurface3D::_runoff_update(double p_delta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	if (_runoff_on_gpu()) {
		// On the GPU the runoff is the standing water's own shallow water with sediment (ground_deform.glsl): here
		// only its sources follow the water table, which moves with the tide.
		gpu_cells_dirty = true;
		near_cells_dirty = true;
		runoff_n = 0;
		runoff_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	if (!ground_valid || !field.valid || ground_n < 2 || (int)cells.size() != ground_n * ground_n) {
		return;
	}
	const int rn = field.resolution / 2;
	const float c = field.texel() * 2.0f;
	const Vector2i origin(field.origin.x / 2, field.origin.y / 2);
	const int nn = rn * rn;
	if (rn != runoff_n || origin != runoff_origin || (int)runoff_depth.size() != nn) {
		// The field moved (in whole tiles): start over; what was cut is already in the field.
		runoff_n = rn;
		runoff_origin = origin;
		runoff_depth.resize(nn);
		runoff_speed.resize(nn);
		runoff_pending.resize(nn);
		runoff_cut.resize(nn);
		for (int i = 0; i < nn; i++) {
			runoff_depth[i] = 0.0f;
			runoff_speed[i] = 0.0f;
			runoff_pending[i] = 0.0f;
			runoff_cut[i] = 0.0f;
		}
	}
	const int n = ground_n;
	const bool wet = water_coupling && (int)pore.size() == n * n && (int)water_depth.size() == n * n;
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	const Vector2 mid = field.rect().get_center();
	const float rain = wm ? MAX(wm->get_rain_at(Vector3(mid.x, _ground_at(mid.x, mid.y), mid.y)), 0.0f) / 3.6e6f : 0.0f; // m/s
	auto lattice = [&](float p_x, float p_z) {
		const int x = CLAMP(int(Math::round(p_x / ground_spacing)) - ground_origin.x, 0, n - 1);
		const int z = CLAMP(int(Math::round(p_z / ground_spacing)) - ground_origin.y, 0, n - 1);
		return z * n + x;
	};
	// Sources first: with none, nothing runs and the grid costs nothing.
	LocalVector<float> src;
	src.resize(nn);
	LocalVector<uint8_t> sink;
	sink.resize(nn);
	float total = 0.0f;
	for (int z = 0; z < rn; z++) {
		for (int x = 0; x < rn; x++) {
			const int i = z * rn + x;
			const float wx = (origin.x + x + 0.5f) * c;
			const float wz = (origin.y + z + 0.5f) * c;
			const int li = lattice(wx, wz);
			const Cell &cell = cells[li];
			const float s = wet ? pore[li] : 0.0f;
			sink[i] = wet && water_depth[li] > 0.002f;
			float r = 0.0f;
			if (!sink[i]) {
				const float tab = (int)water_table.size() == n * n ? water_table[li] : 10.0f;
				if (tab < 0.02f && cell.sand > 0.0f) {
					const float lx = CLAMP(li % n, 1, n - 2);
					const float lz = CLAMP(li / n, 1, n - 2);
					const int l = int(lz) * n + int(lx);
					const float gx = (ground[l + 1] - ground[l - 1]) / (2.0f * ground_spacing);
					const float gz = (ground[l + n] - ground[l - n]) / (2.0f * ground_spacing);
					const float g2 = gx * gx + gz * gz;
					r += 1e-4f * Math::sqrt(g2 / (1.0f + g2)) * MIN(1.0f - tab / 0.02f, 1.0f) * cell.sand;
				}
				if (rain > 0.0f) {
					const float k = Math::exp(Math::lerp(Math::log(1e-6f), Math::log(1e-4f), CLAMP(cell.sand, 0.0f, 1.0f)));
					r += s >= 0.95f ? rain : MAX(rain - k, 0.0f);
				}
			}
			src[i] = r;
			total += r;
		}
	}
	if (total <= 0.0f) {
		for (int i = 0; i < nn; i++) {
			runoff_depth[i] = 0.0f;
			runoff_speed[i] = 0.0f;
		}
		runoff_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	LocalVector<float> h;
	h.resize(nn);
	LocalVector<Pair<float, int>> order;
	order.resize(nn);
	for (int z = 0; z < rn; z++) {
		for (int x = 0; x < rn; x++) {
			const int i = z * rn + x;
			const float wx = (origin.x + x + 0.5f) * c;
			const float wz = (origin.y + z + 0.5f) * c;
			h[i] = _ground_at(wx, wz) + field.rim_at(wx, wz) - field.dent_at(wx, wz);
			order[i] = Pair<float, int>(-h[i], i);
		}
	}
	struct ByKey {
		bool operator()(const Pair<float, int> &a, const Pair<float, int> &b) const { return a.first < b.first; }
	};
	order.sort_custom<ByKey>();
	static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	static const float DL[8] = { 1.0f, 1.0f, 1.0f, 1.0f, (float)Math::SQRT2, (float)Math::SQRT2, (float)Math::SQRT2, (float)Math::SQRT2 };
	LocalVector<float> q_in; // m3/s of water arriving
	LocalVector<float> s_in; // m3/s of sand (solid) arriving
	q_in.resize(nn);
	s_in.resize(nn);
	for (int i = 0; i < nn; i++) {
		q_in[i] = 0.0f;
		s_in[i] = 0.0f;
	}
	const float dt = float(p_delta) * runoff_time_scale;
	const float solid = 1.0f - 0.4f;
	for (int k = 0; k < nn; k++) {
		const int i = order[k].second;
		const int x = i % rn;
		const int z = i / rn;
		const float q = q_in[i] + src[i] * c * c;
		const float sand_in = s_in[i];
		runoff_depth[i] = 0.0f;
		runoff_speed[i] = 0.0f;
		if (q <= 0.0f) {
			continue;
		}
		float w[8];
		float wsum = 0.0f;
		float slope = 0.0f;
		if (!sink[i]) {
			for (int d = 0; d < 8; d++) {
				const int jx = x + DX[d];
				const int jz = z + DZ[d];
				w[d] = 0.0f;
				if (jx < 0 || jz < 0 || jx >= rn || jz >= rn) {
					continue;
				}
				const float drop = (h[i] - h[jz * rn + jx]) / (DL[d] * c);
				if (drop > 0.0f) {
					w[d] = Math::pow(drop, 1.1f) * DL[d];
					wsum += w[d];
					slope = MAX(slope, drop);
				}
			}
		}
		if (wsum <= 0.0f) {
			// Standing water or a hollow: the water stops (into the sea, or soaking in) and lays its sand down.
			runoff_pending[i] -= sand_in * dt / (solid * c * c);
			continue;
		}
		const float unit = q / c;
		const float depth = Math::pow(0.025f * unit / Math::sqrt(slope), 0.6f);
		runoff_depth[i] = depth;
		runoff_speed[i] = unit / MAX(depth, 1e-6f);
		const float wx = (origin.x + x + 0.5f) * c;
		const float wz = (origin.y + z + 0.5f) * c;
		const Cell &cell = _cell_at(wx, wz);
		const float loose = cell.deformable ? CLAMP(cell.sand, 0.0f, 1.0f) : 0.0f;
		const float cap = bed_load_shear(1000.0f * 9.81f * depth * MIN(slope, 1.0f)) * c;
		float sand_out = sand_in;
		if (sand_in < cap * loose) {
			sand_out = cap * loose;
		} else if (sand_in > cap) {
			sand_out = cap;
		}
		runoff_pending[i] += (sand_out - sand_in) * dt / (solid * c * c);
		for (int d = 0; d < 8; d++) {
			if (w[d] > 0.0f) {
				const int j = (z + DZ[d]) * rn + x + DX[d];
				q_in[j] += q * w[d] / wsum;
				s_in[j] += sand_out * w[d] / wsum;
			}
		}
	}
	// To the field, a cell's volume over a disc a little wider than the cell, so the steps blend (on the finer near
	// field a tent one cell wide: no round pits there).
	const float rad = 0.6f * c;
	const float spread = c * c / (Math::PI * rad * rad);
	int writes = 0;
	for (int i = 0; i < nn && writes < 4096; i++) {
		const float p = runoff_pending[i];
		if (Math::abs(p) < 0.001f) {
			continue;
		}
		const float wx = (origin.x + i % rn + 0.5f) * c;
		const float wz = (origin.y + i / rn + 0.5f) * c;
		const Vector3 at(wx, _ground_at(wx, wz), wz);
		const float d = CLAMP(p, -0.01f, 0.01f);
		_amount(at, rad, d * spread, c);
		runoff_cut[i] += d;
		runoff_pending[i] -= d;
		writes++;
	}
	runoff_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
}

void GroundSurface3D::set_runoff_channels(bool p_enabled) {
	runoff_channels = p_enabled;
	if (!p_enabled) {
		runoff_n = 0;
		runoff_depth.clear();
		runoff_speed.clear();
		runoff_pending.clear();
		runoff_cut.clear();
	}
}

Vector2 GroundSurface3D::get_runoff_at(const Vector3 &p_position) const {
	if (_runoff_on_gpu()) {
		// The GPU's water depth, and the sheet's speed by Manning (n 0.025) on the ground's slope there.
		const float d = get_standing_water_at(p_position);
		if (d <= 0.0f) {
			return Vector2();
		}
		const float e = 0.05f;
		const float gx = (get_surface_height_at(p_position + Vector3(e, 0, 0)) - get_surface_height_at(p_position - Vector3(e, 0, 0))) / (2.0f * e);
		const float gz = (get_surface_height_at(p_position + Vector3(0, 0, e)) - get_surface_height_at(p_position - Vector3(0, 0, e))) / (2.0f * e);
		return Vector2(d, Math::pow(d, 2.0f / 3.0f) * Math::sqrt(Math::sqrt(gx * gx + gz * gz)) / 0.025f);
	}
	if (runoff_n <= 0 || (int)runoff_depth.size() != runoff_n * runoff_n) {
		return Vector2();
	}
	const float c = field.texel() * 2.0f;
	const int x = int(Math::floor(p_position.x / c)) - runoff_origin.x;
	const int z = int(Math::floor(p_position.z / c)) - runoff_origin.y;
	if (x < 0 || z < 0 || x >= runoff_n || z >= runoff_n) {
		return Vector2();
	}
	return Vector2(runoff_depth[z * runoff_n + x], runoff_speed[z * runoff_n + x]);
}

float GroundSurface3D::get_runoff_cut_at(const Vector3 &p_position) const {
	if (_runoff_on_gpu()) {
		if (is_near_active() && _near_inside(p_position.x, p_position.z) && int(near_fld.water_cut.size()) == near_fld.resolution * near_fld.resolution) {
			return near_fld.sample(near_fld.water_cut, p_position.x, p_position.z);
		}
		if (field.valid && int(field.water_cut.size()) == field.resolution * field.resolution) {
			return field.sample(field.water_cut, p_position.x, p_position.z);
		}
		return 0.0f;
	}
	if (runoff_n <= 0 || (int)runoff_cut.size() != runoff_n * runoff_n) {
		return 0.0f;
	}
	const float c = field.texel() * 2.0f;
	const int x = int(Math::floor(p_position.x / c)) - runoff_origin.x;
	const int z = int(Math::floor(p_position.z / c)) - runoff_origin.y;
	if (x < 0 || z < 0 || x >= runoff_n || z >= runoff_n) {
		return 0.0f;
	}
	return runoff_cut[z * runoff_n + x] + runoff_pending[z * runoff_n + x];
}

void GroundSurface3D::_wind_update(double p_delta) {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)cells.size() != n * n) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int nn = n * n;
	if ((int)sand_dep.size() != nn) {
		sand_dep.resize(nn);
		sand_dep_pending.resize(nn);
		for (int i = 0; i < nn; i++) {
			sand_dep[i] = 0.0f;
			sand_dep_pending[i] = 0.0f;
		}
	}
	const bool fresh = (int)shelter_flux.size() != nn;
	shelter_ratio.resize(nn);
	shelter_flux.resize(nn);
	sand_flux.resize(nn);
	sand_dep_rate.resize(nn);
	if (fresh) {
		for (int i = 0; i < nn; i++) {
			shelter_ratio[i] = 1.0f;
			shelter_flux[i] = 1.0f;
			sand_flux[i] = 0.0f;
			sand_dep_rate[i] = 0.0f;
		}
	}
	// The plants' frontal area index, on a 1 m grid (the shelters answer on the main thread only, as here).
	LocalVector<Node *> sources;
	if (is_inside_tree()) {
		for (int k = 0; k < wind_shelters.size(); k++) {
			Node *src = get_node_or_null(wind_shelters[k]);
			if (src) {
				sources.push_back(src);
			}
		}
	}
	static const StringName s_cover = "sample_lateral_cover";
	const int step = MAX(1, (int)Math::round(1.0f / ground_spacing));
	const int gn = (n - 1) / step + 2;
	LocalVector<float> g_lambda;
	g_lambda.resize(gn * gn);
	for (int i = 0; i < gn * gn; i++) {
		g_lambda[i] = 0.0f;
	}
	if (!sources.is_empty()) {
		for (int gz = 0; gz < gn; gz++) {
			for (int gx = 0; gx < gn; gx++) {
				const int lx = MIN(gx * step, n - 1);
				const int lz = MIN(gz * step, n - 1);
				const Vector3 p((ground_origin.x + lx) * ground_spacing, ground[lz * n + lx], (ground_origin.y + lz) * ground_spacing);
				float lambda = 0.0f;
				for (Node *src : sources) {
					if (src->has_method(s_cover)) {
						const float v = src->call(s_cover, p);
						if (Math::is_finite(v)) {
							lambda += MAX(v, 0.0f); // frontal areas add
						}
					}
				}
				g_lambda[gz * gn + gx] = MIN(lambda, 0.5f);
			}
		}
	}
	const Vector2 wind = _wind_10m();
	const float speed = wind.length();
	const float ustar = GroundRipples::friction_speed(speed);
	const float q_open = GroundRipples::saltation_flux(ustar);
	bool shelter_changed = false;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float fx = float(x) / step;
			const float fz = float(z) / step;
			const int x0 = MIN((int)fx, gn - 2);
			const int z0 = MIN((int)fz, gn - 2);
			const float tx = fx - x0;
			const float tz = fz - z0;
			const float lambda = Math::lerp(Math::lerp(g_lambda[z0 * gn + x0], g_lambda[z0 * gn + x0 + 1], tx), Math::lerp(g_lambda[(z0 + 1) * gn + x0], g_lambda[(z0 + 1) * gn + x0 + 1], tx), tz);
			const float r = shelter_ratio_for(lambda);
			// The flux factor at a reference gale (u* 0.5 m/s), so the share does not jump with the wind.
			const float f = GroundRipples::saltation_flux(0.5f * r) / GroundRipples::saltation_flux(0.5f);
			if (Math::abs(f - shelter_flux[i]) > 0.02f) {
				shelter_changed = true;
			}
			shelter_ratio[i] = r;
			shelter_flux[i] = f;
		}
	}
	if (shelter_changed) {
		gpu_cells_dirty = true;
		near_cells_dirty = true;
	}
	if (q_open <= 0.0f) {
		for (int i = 0; i < nn; i++) {
			sand_flux[i] = 0.0f;
			sand_dep_rate[i] = 0.0f;
		}
		wind_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	const Vector2 dir = wind / speed;
	sand_flux_dir = dir;
	// Cells in downwind order (sorted again when the wind turns).
	if ((int)wind_order.size() != nn || wind_order_dir.dot(dir) < 0.996f) {
		wind_order.resize(nn);
		LocalVector<Pair<float, int>> keys;
		keys.resize(nn);
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				keys[z * n + x] = Pair<float, int>(x * dir.x + z * dir.y, z * n + x);
			}
		}
		struct ByKey {
			bool operator()(const Pair<float, int> &a, const Pair<float, int> &b) const { return a.first < b.first; }
		};
		keys.sort_custom<ByKey>();
		for (int i = 0; i < nn; i++) {
			wind_order[i] = keys[i].second;
		}
		wind_order_dir = dir;
	}
	const float l_sat = 1.0f; // m
	const float relax = Math::exp(-ground_spacing / l_sat);
	const float rho = 1600.0f; // kg/m3, dry sand's bulk density
	const float dt = float(p_delta) * ripple_time_scale;
	for (int k = 0; k < nn; k++) {
		const int i = wind_order[k];
		const int x = i % n;
		const int z = i / n;
		const Cell &c = cells[i];
		const float supply = c.deformable ? c.blown : 0.0f;
		const float cap = GroundRipples::saltation_flux(ustar * shelter_ratio[i]) * supply;
		// The flux arriving: from one cell upwind, or saturated over open sand of this kind at the patch's edge.
		const float ux = x - dir.x;
		const float uz = z - dir.y;
		float q_up;
		if (ux < 0.0f || uz < 0.0f || ux > n - 1 || uz > n - 1) {
			q_up = q_open * supply;
		} else {
			const int ax = MIN((int)ux, n - 2);
			const int az = MIN((int)uz, n - 2);
			const float bx = ux - ax;
			const float bz = uz - az;
			q_up = Math::lerp(Math::lerp(sand_flux[az * n + ax], sand_flux[az * n + ax + 1], bx), Math::lerp(sand_flux[(az + 1) * n + ax], sand_flux[(az + 1) * n + ax + 1], bx), bz);
		}
		const float q = cap + (q_up - cap) * relax;
		sand_flux[i] = q;
		const float rate = (q_up - q) / (rho * ground_spacing);
		sand_dep_rate[i] = rate;
		if (rate > 0.0f && c.deformable) {
			sand_dep_pending[i] += rate * dt;
		}
		if (sand_dep_pending[i] >= 0.002f && field.valid) {
			// The cell's volume, over a disc a little wider than the cell so the steps blend (a tent on the near field).
			const float d = sand_dep_pending[i];
			const float rad = 0.6f * ground_spacing;
			const Vector3 at((ground_origin.x + x) * ground_spacing, ground[i], (ground_origin.y + z) * ground_spacing);
			_amount(at, rad, -d * ground_spacing * ground_spacing / (Math::PI * rad * rad), ground_spacing);
			sand_dep[i] += d;
			sand_dep_pending[i] = 0.0f;
		}
	}
	wind_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
}

void GroundSurface3D::set_water_coupling(bool p_enabled) {
	water_coupling = p_enabled;
	if (!p_enabled) {
		pore.clear();
		water_speed.clear();
		cell_water.clear();
		_wet_publish(false);
		_update_cells();
	}
}

float GroundSurface3D::get_saturation_at(const Vector3 &p_position) const {
	const int n = ground_n;
	if (!ground_valid || (int)pore.size() != n * n) {
		return 0.0f;
	}
	const int x = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? pore[z * n + x] : 0.0f;
}

float GroundSurface3D::get_water_speed_at(const Vector3 &p_position) const {
	const int n = ground_n;
	if (!ground_valid || (int)water_speed.size() != n * n) {
		return 0.0f;
	}
	const int x = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? water_speed[z * n + x] : 0.0f;
}

// Saltation flux from GroundRipples::saltation_flux. The ripple layer steps when q of sand a cell has moved
// since its last step (q = Q / (bulk density * hop)), a few times a second at most.
void GroundSurface3D::_ripples_update(double p_delta) {
	if (!wind_ripples || !field.valid) {
		return;
	}
	// The ripple cell divides the patch's detail spacing, so the fine ring's vertices sit on its cells.
	const int k = MAX((int)Math::round(detail_spacing / 0.0125f), 1);
	const float cell = detail_spacing / k;
	const int nr = 2 * (int)Math::round(0.5f * ripple_extent / detail_spacing) * k;
	if (ripples.cell != cell || ripples.n != nr) {
		ripples.cell = cell;
		ripples.n = nr;
		ripples.valid = false;
		ripples.tmpl_ready = false;
		mesh_dirty = true;
	}
	Vector2 wind = _wind_10m();
	const float speed = wind.length();
	const Vector2 dir = speed > 0.1f ? wind / speed : Vector2(1.0f, 0.0f);
	// A grown field for the cells that come in, in the background; again when the wind turns past 35 degrees.
	if (ripple_task != WorkerThreadPool::INVALID_TASK_ID) {
		if (WorkerThreadPool::get_singleton()->is_task_completed(ripple_task)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(ripple_task);
			ripple_task = WorkerThreadPool::INVALID_TASK_ID;
			const bool first = !ripples.tmpl_ready;
			ripples.tmpl = ripple_grown;
			ripples.tmpl_wind = ripple_grow_wind;
			ripples.tmpl_ready = true;
			if (first) {
				ripples.valid = false; // the whole window from it
			}
			_ripples_far_tile();
		}
	} else if (!ripples.tmpl_ready || (speed > 0.1f && ripples.tmpl_wind.dot(dir) < 0.82f)) {
		ripple_grow_wind = dir;
		ripple_task = WorkerThreadPool::get_singleton()->add_native_task(&GroundSurface3D::_ripple_grow_task, this, false, "Ground ripples");
	}
	const float w = 0.5f * ripples.n * ripples.cell;
	const Vector2i want((int)Math::round((center.x - w) / ripples.cell), (int)Math::round((center.y - w) / ripples.cell));
	if (!ripples.valid || want != ripples.origin) {
		ripples.move_to(want);
		ripple_dirty = true;
		ripple_mask_accum = 1e9;
	}
	// What moves: dry sand. What shows them: sand, wet or dry (damp sand keeps the ripples the wind left), not
	// under litter. Both in 20 cm blocks, every two seconds.
	ripple_mask_accum += p_delta;
	if (ripple_mask_accum >= 2.0) {
		ripple_mask_accum = 0.0;
		ripple_show.resize(ripples.n * ripples.n);
		const int b = 16;
		for (int bz = 0; bz < ripples.n; bz += b) {
			for (int bx = 0; bx < ripples.n; bx += b) {
				const float x = (ripples.origin.x + bx + 0.5f * b) * ripples.cell;
				const float z = (ripples.origin.y + bz + 0.5f * b) * ripples.cell;
				const Cell &c = _cell_at(x, z);
				const float m = c.deformable ? c.blown * _shelter_flux_at(x, z) : 0.0f;
				const float show = c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) : 0.0f;
				for (int z2 = bz; z2 < MIN(bz + b, ripples.n); z2++) {
					for (int x2 = bx; x2 < MIN(bx + b, ripples.n); x2++) {
						ripples.mask[z2 * ripples.n + x2] = m;
						ripple_show[z2 * ripples.n + x2] = show;
					}
				}
			}
		}
		// The sand share over the patch for the far ripples, in 25 cm texels (at 32 m).
		const int sn = RIPPLE_SAND_N;
		const float size = deform_extent;
		const float st = size / sn;
		ripple_sand_rect = Rect2(Math::snapped(center.x, st) - 0.5f * size, Math::snapped(center.y, st) - 0.5f * size, size, size);
		ripple_sand.resize(sn * sn);
		for (int z = 0; z < sn; z++) {
			for (int x = 0; x < sn; x++) {
				const Cell &c = _cell_at(ripple_sand_rect.position.x + (x + 0.5f) * st, ripple_sand_rect.position.y + (z + 0.5f) * st);
				ripple_sand[z * sn + x] = c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) : 0.0f;
			}
		}
		ripple_dirty = true;
	}
	// Where the field's surface changed (a print, a rut, sand landing), the ripples there are gone; they grow
	// back as the wind works the new surface.
	{
		const float t = field.texel();
		const int fx0 = (int)Math::floor(ripples.origin.x * ripples.cell / t);
		const int fz0 = (int)Math::floor(ripples.origin.y * ripples.cell / t);
		const int fn = (int)Math::ceil(ripples.n * ripples.cell / t) + 1;
		const bool fresh = (int)ripple_seen.size() != fn * fn || ripple_seen_origin != Vector2i(fx0, fz0);
		if (fresh) {
			ripple_seen.resize(fn * fn);
			ripple_seen_origin = Vector2i(fx0, fz0);
		}
		const int per = (int)Math::round(t / ripples.cell);
		for (int z = 0; z < fn; z++) {
			for (int x = 0; x < fn; x++) {
				const float wx = (fx0 + x + 0.5f) * t;
				const float wz = (fz0 + z + 0.5f) * t;
				const float v = field.sample(field.rim, wx, wz) - field.sample(field.dent, wx, wz);
				float &seen = ripple_seen[z * fn + x];
				if (!fresh && Math::abs(v - seen) > 0.001f) {
					// The texel and a cell round it.
					const int cx0 = (int)Math::floor((fx0 + x) * t / ripples.cell) - ripples.origin.x - 1;
					const int cz0 = (int)Math::floor((fz0 + z) * t / ripples.cell) - ripples.origin.y - 1;
					for (int cz = MAX(cz0, 0); cz < MIN(cz0 + per + 2, ripples.n); cz++) {
						for (int cx = MAX(cx0, 0); cx < MIN(cx0 + per + 2, ripples.n); cx++) {
							ripples.h[cz * ripples.n + cx] = 0.0f;
						}
					}
					ripple_dirty = true;
				}
				seen = v;
			}
		}
	}
	// Saltation drives the ripples.
	const float ustar = GroundRipples::friction_speed(speed);
	if (ustar > GroundRipples::SAND_USTAR_T && ripples.tmpl_ready) {
		const float flux = GroundRipples::saltation_flux(ustar);
		ripple_q_accum += flux / (1600.0f * ripples.hop) * ripple_time_scale * p_delta;
		int steps = 0;
		while (ripple_q_accum >= 0.05 * ripples.q_ref && steps < 4) {
			const float q = MIN((float)ripple_q_accum, ripples.q_ref);
			ripples.step(q, dir);
			ripple_q_accum -= q;
			ripple_steps++;
			steps++;
			ripple_dirty = true;
		}
		if (steps == 4) {
			ripple_q_accum = MIN(ripple_q_accum, (double)ripples.q_ref); // past the budget: time slows, no backlog
		}
	}
	if (ripple_dirty) {
		ripple_dirty = false;
		// Rows 0..n-1: the window (height, sand share) and the grown field's slope at world cell mod n; rows
		// n..n+S-1: the sand share over the patch; the last row: the window's rect and cell, the sand rect and
		// the tile's size, and whether far ripples are on.
		const int n = ripples.n;
		const int nn = n * n;
		const int sn = RIPPLE_SAND_N;
		const int w = MAX(n, sn);
		const int hgt = n + sn + 1;
		Vector<uint8_t> data;
		data.resize(w * hgt * 16);
		memset(data.ptrw(), 0, data.size());
		float *d = (float *)data.ptrw();
		const bool tile = (int)ripple_tile_slope.size() == nn * 2;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				float *o = d + (z * w + x) * 4;
				o[0] = ripples.h[i];
				o[1] = (int)ripple_show.size() == nn ? ripple_show[i] : 0.0f;
				o[2] = tile ? ripple_tile_slope[i * 2 + 0] : 0.0f;
				o[3] = tile ? ripple_tile_slope[i * 2 + 1] : 0.0f;
			}
		}
		const bool sand = (int)ripple_sand.size() == sn * sn;
		if (sand) {
			for (int z = 0; z < sn; z++) {
				for (int x = 0; x < sn; x++) {
					d[((n + z) * w + x) * 4] = ripple_sand[z * sn + x];
				}
			}
		}
		float *meta = d + (n + sn) * w * 4;
		meta[0] = (ripples.origin.x - 0.5f) * ripples.cell;
		meta[1] = (ripples.origin.y - 0.5f) * ripples.cell;
		meta[2] = n * ripples.cell;
		meta[3] = ripples.cell;
		meta[4] = ripple_sand_rect.position.x;
		meta[5] = ripple_sand_rect.position.y;
		meta[6] = ripple_sand_rect.size.x;
		meta[7] = n * ripples.cell;
		meta[8] = tile && sand ? 1.0f : 0.0f;
		meta[9] = (float)n;
		meta[10] = (float)sn;
		if (ripple_image.is_valid() && ripple_image->get_width() == w && ripple_image->get_height() == hgt && ripple_texture.is_valid()) {
			ripple_image->set_data(w, hgt, false, Image::FORMAT_RGBAF, data);
			ripple_texture->update(ripple_image);
		} else {
			ripple_image = Image::create_from_data(w, hgt, false, Image::FORMAT_RGBAF, data);
			ripple_texture = ImageTexture::create_from_image(ripple_image);
			ripple_published = false;
		}
		_ripples_publish();
	}
	if (mesh_dirty) {
		_build_mesh();
	}
}

// The grown field's slope at each cell (it is periodic, so the differences wrap), for the far ripples: in
// the ripple map for the path tracer and as its own mipmapped texture for the raster, so far ripples average
// out to a flat normal instead of shimmering.
void GroundSurface3D::_ripples_far_tile() {
	const int n = ripples.n;
	if ((int)ripples.tmpl.size() != n * n) {
		return;
	}
	ripple_tile_slope.resize(n * n * 2);
	Vector<uint8_t> data;
	data.resize(n * n * 8);
	float *d = (float *)data.ptrw();
	const float inv = 0.5f / ripples.cell;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float *t = ripples.tmpl.ptr();
			const float sx = (t[z * n + (x + 1) % n] - t[z * n + (x + n - 1) % n]) * inv;
			const float sz = (t[((z + 1) % n) * n + x] - t[((z + n - 1) % n) * n + x]) * inv;
			ripple_tile_slope[(z * n + x) * 2 + 0] = sx;
			ripple_tile_slope[(z * n + x) * 2 + 1] = sz;
			d[(z * n + x) * 2 + 0] = sx;
			d[(z * n + x) * 2 + 1] = sz;
		}
	}
	ripple_far_image = Image::create_from_data(n, n, false, Image::FORMAT_RGF, data);
	ripple_far_image->generate_mipmaps();
	if (ripple_far_texture.is_valid() && ripple_far_texture->get_width() == n) {
		ripple_far_texture->update(ripple_far_image);
	} else {
		ripple_far_texture = ImageTexture::create_from_image(ripple_far_image);
		ripple_far_published = false;
	}
	ripple_dirty = true;
}

void GroundSurface3D::_ripples_publish() {
	if (active != this) {
		return;
	}
	if (!wind_ripples || ripple_texture.is_null() || !ripples.valid) {
		_set_global("ground_ripple_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
		ripple_published = false;
		return;
	}
	if (!ripple_published) {
		_set_global("ground_ripple_map", ripple_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	}
	if (!ripple_far_published && ripple_far_texture.is_valid()) {
		_set_global("ground_ripple_far", ripple_far_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		ripple_far_published = true;
	}
	// Texel centres on the cells (and so on the fine ring's vertices).
	_set_global("ground_ripple_rect", Vector4((ripples.origin.x - 0.5f) * ripples.cell, (ripples.origin.y - 0.5f) * ripples.cell, ripples.n * ripples.cell, ripples.cell), RSE::GLOBAL_VAR_TYPE_VEC4);
	ripple_published = true;
}

// Every second: turned ground dries, wind fills dry sand prints, rain softens soil prints.
void GroundSurface3D::_age(double p_delta) {
	if (!field.valid) {
		return;
	}
	const float dt = float(p_delta);
	const float dt_h = dt / 3600.0f;
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
	const float wind = _wind_10m().length();
	float rain = 0.0f;
	if (wm) {
		rain = wm->get_rain_at(at);
	}
	// Sand moves above about 5 m/s at 10 m (Bagnold's threshold for fine dry sand).
	const float drift = sand_drift_rate * MAX(wind - 5.0f, 0.0f) / 5.0f;
	const float soften = rain_smoothing * rain;
	const float dry = Math::exp(-dt / drying_time);
	const int n = field.resolution;
	const int nt = field.tiles();
	const float t = field.texel();
	const int T = GroundDeformField::TILE;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			if (!field.tile_active[tz * nt + tx]) {
				continue;
			}
			bool any = false;
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					float &dn = field.dent[i];
					float &rm = field.rim[i];
					if (dn <= 0.0f && rm <= 0.0f && field.fresh[i] <= 0.0f) {
						continue;
					}
					if (drift > 0.0f || soften > 0.0f) {
						const Cell &c = _cell_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t);
						const float keep = Math::exp(-(drift * c.blown * _shelter_flux_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t) + soften) * dt_h);
						dn *= keep;
						rm *= keep;
					}
					field.fresh[i] *= dry;
					// A texel both pressed and piled on is the same surface with the smaller of the two taken off both
					// (water and wind spread sand thin over prints). Then films under 10 microns go, so tiles can rest.
					const float both = MIN(dn, rm);
					dn -= both;
					rm -= both;
					if (dn < 1e-5f) {
						dn = 0.0f;
					}
					if (rm < 1e-5f) {
						rm = 0.0f;
					}
					if (field.fresh[i] < 0.01f) {
						field.fresh[i] = 0.0f;
					}
					any = any || dn > 0.0f || rm > 0.0f || field.fresh[i] > 0.0f;
				}
			}
			field.tile_active[tz * nt + tx] = any ? 1 : 0;
		}
	}
	field.dirty = true;
}

void GroundSurface3D::settle(float p_seconds, float p_step) {
	const float step = MAX(p_step, 1e-3f);
	if (_gpu_on() && field.valid && is_inside_tree()) {
		float left_gpu = p_seconds;
		double age_gpu = 0.0;
		float sync_accum = 0.0f;
		while (left_gpu > 0.0f) {
			const float dt = MIN(step, left_gpu);
			age_gpu += dt;
			const bool age_now = age_gpu >= 1.0;
			// A blocking read back every simulated second keeps slumping going while texels still owe some.
			sync_accum += dt;
			const bool sync_now = sync_accum >= 1.0f;
			_gpu_submit(dt, age_now ? age_gpu : 0.0, false, sync_now);
			if (sync_now) {
				sync_accum = 0.0f;
			}
			if (age_now) {
				age_gpu = 0.0;
			}
			left_gpu -= dt;
		}
		if (age_gpu > 0.0) {
			_gpu_submit(0.0, age_gpu, false, false);
		}
		sync_deformation();
		return;
	}
	float left = p_seconds;
	auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
	auto params_fn = [this](float x, float z, float &r_tan, float &r_rate, float &r_max) {
		const Cell &c = _cell_at(x, z);
		if (!c.deformable) {
			return false;
		}
		r_tan = c.tan_repose;
		r_rate = c.rate;
		r_max = c.max_dent;
		return true;
	};
	double age = 0.0;
	while (left > 0.0f) {
		const float dt = MIN(step, left);
		field.slump(dt, ground_fn, params_fn);
		age += dt;
		if (age >= 1.0) {
			_age(age);
			age = 0.0;
		}
		left -= dt;
	}
	if (age > 0.0) {
		_age(age);
	}
	if (field.dirty) {
		_upload();
	}
}

void GroundSurface3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!active) {
				active = this;
			}
			_ensure_children();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			_follow(_follow_position());
			_upload();
			_publish();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
			_gpu_release();
			if (ripple_task != WorkerThreadPool::INVALID_TASK_ID) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(ripple_task);
				ripple_task = WorkerThreadPool::INVALID_TASK_ID;
			}
			if (active == this) {
				_set_global("ground_deform_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
				_set_global("ground_ripple_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
				ripple_published = false;
				ripple_far_published = false;
				_wet_publish(false);
				wet_published = false;
				active = nullptr;
			}
			globals_published = false;
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
			const double delta = get_physics_process_delta_time();
			if (!active) {
				active = this;
			}
			_follow(_follow_position());
			cells_accum += delta;
			if (cells_accum >= 2.0) {
				cells_accum = 0.0;
				_update_cells();
			}
			water_accum += delta;
			if (water_accum >= 0.5) {
				_water_update(water_accum);
				_crack_update(water_accum);
				water_accum = 0.0;
			}
			wet_publish_accum += delta;
			if (wet_publish_accum >= 0.5) {
				_wet_publish(true);
				wet_publish_accum = 0.0;
			}
			wind_accum += delta;
			if (wind_accum >= 0.5) {
				_wind_update(wind_accum);
				wind_accum = 0.0;
			}
			if (runoff_channels) {
				runoff_accum += delta;
				if (runoff_accum >= 0.5) {
					_runoff_update(runoff_accum);
					runoff_accum = 0.0;
				}
			}
			const bool on_gpu = _gpu_on();
			if (!on_gpu && gpu_host.gpu.is_valid()) {
				_gpu_release();
				field.invalidate();
				_follow(_follow_position());
			}
			if (on_gpu && field.valid) {
				_gpu_take();
				Columns cols;
				cols.surface = this;
				const Cell &c = _cell_at(center.x, center.y);
				field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
				// The bodies press as stamps for the GPU field (the same maths).
				field.stamp_sink = &gpu_sink;
				field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, delta);
				field.stamp_sink = nullptr;
				_plough_bodies();
				age_accum += delta;
				double age = 0.0;
				if (age_accum >= 1.0) {
					age = age_accum;
					age_accum = 0.0;
				}
				gpu_readback_accum += delta;
				bool readback = false;
				if (gpu_readback_accum >= 1.0 / 15.0) {
					gpu_readback_accum = 0.0;
					readback = true;
				}
				_gpu_submit(delta, age, readback, false);
			} else if (field.valid) {
				Columns cols;
				cols.surface = this;
				const Cell &c = _cell_at(center.x, center.y);
				field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
				field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, delta);
				_plough_bodies();
				auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
				auto params_fn = [this](float x, float z, float &r_tan, float &r_rate, float &r_max) {
					const Cell &cl = _cell_at(x, z);
					if (!cl.deformable) {
						return false;
					}
					r_tan = cl.tan_repose;
					r_rate = cl.rate;
					r_max = cl.max_dent;
					return true;
				};
				field.slump(delta, ground_fn, params_fn);
				age_accum += delta;
				if (age_accum >= 1.0) {
					_age(age_accum);
					age_accum = 0.0;
				}
			}
			if (field.dirty && !on_gpu) {
				_upload();
			}
			_ripples_update(delta);
			_publish();
			step_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Soft soil (docs/ideas/sand.md, stage 1)

GroundSoil GroundSurface3D::get_soil(Vector3 p_position) const {
	GroundSoil soil;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	if (!c.deformable || !field.valid || _edge_distance(p_position.x, p_position.z) < terrain_margin + skirt_fade) {
		return soil;
	}
	soil.soft = true;
	soil.n = c.wheel_n;
	soil.kc = c.wheel_kc;
	soil.kphi = c.wheel_kphi;
	soil.cohesion = c.cohesion;
	soil.tan_phi = c.tan_phi;
	soil.shear_k = c.shear_k;
	soil.unit_weight = c.unit_weight;
	soil.tan_repose = c.tan_repose;
	soil.max_sinkage = c.max_dent;
	soil.stickiness = c.stickiness;
	soil.adhesion = c.adhesion;
	return soil;
}

GroundWheelSoil GroundSurface3D::wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle) const {
	const GroundSoil soil = get_soil(p_position);
	const float already = MAX(field.dent_at(p_position.x, p_position.z) - field.rim_at(p_position.x, p_position.z), 0.0f);
	return GroundTerramechanics::wheel(soil, p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle, already);
}

Dictionary GroundSurface3D::get_wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle) const {
	const GroundWheelSoil w = wheel_soil(p_position, p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle);
	Dictionary d;
	d["soft"] = w.soft;
	d["static_sinkage"] = w.static_sinkage;
	d["sinkage"] = w.sinkage;
	d["contact_length"] = w.contact_length;
	d["compaction"] = w.compaction;
	d["bulldozing"] = w.bulldozing;
	d["resistance"] = w.compaction + w.bulldozing;
	d["shear_limit"] = w.shear_limit;
	d["traction"] = w.traction;
	d["lateral"] = w.lateral;
	const GroundSoil soil = get_soil(p_position);
	d["stickiness"] = soil.stickiness;
	d["adhesion"] = soil.adhesion;
	return d;
}

float GroundSurface3D::press_wheel(Vector3 p_position, Vector2 p_size, float p_yaw, float p_sinkage, Vector2 p_velocity) {
	if (!field.valid || p_sinkage <= 0.0f || p_size.x <= 0.0f || p_size.y <= 0.0f) {
		return 0.0f;
	}
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const Vector2 half = p_size * 0.5f;
	const float radius = 0.5f * MAX(p_size.x, p_size.y);
	const float bottom = _ground_at(p_position.x, p_position.z) - p_sinkage;
	if (_gpu_on()) {
		// The bodies' path: only the GPU field takes it (the CPU copy follows at the read back).
		gpu_sink.stamp(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
		return 0.0f;
	}
	return field.press(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
}

void GroundSurface3D::_plough_bodies() {
	// A rigid body sliding or rolling through soft ground (a crate, a rock, a barrel) works the soil it moves
	// into as a wheel does: compaction of its print's new depth and bulldozing of the soil before it (Bekker,
	// Wong 2008), at the body's width, against its travel. It still rests on the undisturbed collider.
	if (!field.valid || !ploughing) {
		return;
	}
	Columns cols;
	cols.surface = this;
	HashSet<ObjectID> done;
	for (const String &group : deform_groups) {
		for (Node *node : get_tree()->get_nodes_in_group(StringName(group))) {
			RigidBody3D *rb = Object::cast_to<RigidBody3D>(node);
			if (!rb || rb->is_freeze_enabled() || rb->is_sleeping() || done.has(rb->get_instance_id())) {
				continue;
			}
			done.insert(rb->get_instance_id());
			const Vector3 v = rb->get_linear_velocity();
			const Vector3 vh(v.x, 0.0f, v.z);
			const float speed = vh.length();
			if (speed < 1e-3f) {
				continue;
			}
			GroundDeformField::Body b;
			field.read_body(rb, character_mass, character_contact_area, b);
			const Cell &c = _cell_at(b.center.x, b.center.z);
			if (!c.deformable) {
				continue;
			}
			// Only while it touches the ground: its underside within the sinkage of the surface.
			const float sink = cols.sinkage(b);
			const float under = b.center.y - b.half_height - _ground_at(b.center.x, b.center.z);
			if (sink <= 0.0f || under > contact_tolerance + sink) {
				continue;
			}
			GroundSoil soil = get_soil(b.center);
			if (!soil.soft) {
				continue;
			}
			// A plate's pressure-sinkage, as its print.
			soil.n = c.n;
			soil.kc = c.kc;
			soil.kphi = c.kphi;
			const Vector3 dir = vh / speed;
			const Vector3 front = b.center + dir * (b.radius + 0.05f);
			const float already = MAX(field.dent_at(front.x, front.z) - field.rim_at(front.x, front.z), 0.0f);
			const float width = b.width > 0.0f ? b.width : 2.0f * b.radius;
			const float resist = GroundTerramechanics::compaction_resistance(soil, width, sink, already) + GroundTerramechanics::bulldozing_resistance(soil, width, sink, already);
			// Eased in over the first 0.3 m/s, and never more than stops it within the step.
			const float step = (float)get_physics_process_delta_time();
			const float stop = rb->get_mass() * speed / MAX(step, 1e-4f);
			const float f = MIN(resist * MIN(speed / 0.3f, 1.0f), stop);
			rb->apply_central_force(-dir * f);
		}
	}
}

Dictionary GroundSurface3D::get_footing_at(Vector3 p_position, float p_mass, float p_area) const {
	Dictionary d;
	const GroundSoil soil = get_soil(p_position);
	float sink = 0.0f;
	Vector3 slide;
	if (soil.soft) {
		Columns cols;
		cols.surface = this;
		GroundDeformField::Body b;
		b.center = p_position;
		b.mass = p_mass;
		b.area = p_area;
		sink = cols.sinkage(b);
		// The surface's slope over a boot length; past what the disturbed material holds (its repose, less the
		// few degrees between the angle at which a granular slope starts and stops flowing; Pouliquen 1999,
		// "Scaling laws in granular flows down rough inclined planes"), the footing slides with the grains:
		// Coulomb friction on the slope, a = g (sin t - cos t tan(repose - 3 deg)).
		const float h = 0.15f;
		const float gx = (get_surface_height_at(p_position + Vector3(h, 0, 0)) - get_surface_height_at(p_position - Vector3(h, 0, 0))) / (2.0f * h);
		const float gz = (get_surface_height_at(p_position + Vector3(0, 0, h)) - get_surface_height_at(p_position - Vector3(0, 0, h))) / (2.0f * h);
		const float grad = Math::sqrt(gx * gx + gz * gz);
		const float theta = Math::atan(grad);
		const float hold = MAX(Math::atan(soil.tan_repose) - Math::deg_to_rad(3.0f), 0.0f);
		if (grad > 1e-4f && theta > hold) {
			const float a = 9.81f * (Math::sin(theta) - Math::cos(theta) * Math::tan(hold));
			// Down the slope, along the surface.
			const Vector3 down = Vector3(-gx, -grad * grad, -gz).normalized();
			slide = down * a;
		}
	}
	// Lejeune, Willems and Heglund 1998, "Mechanics and energetics of human locomotion on sand" (J. Exp. Biol.
	// 201): 1.6 to 2.5 times the work of firm ground. Fitted to the print depth: 2 cm (dry sand) costs 2.1
	// times, damp sand (0.6 cm) 1.3, deep mud at most 2.65; at the same effort the stride slows by the same
	// factor.
	const float cost = 1.0f + 1.1f * MIN(sink / 0.02f, 1.5f);
	d["soft"] = soil.soft;
	d["sinkage"] = sink;
	d["effort"] = cost;
	d["speed_scale"] = 1.0f / cost;
	d["slide"] = slide;
	return d;
}

// ---------------------------------------------------------------------------------------------------------
// The field on the GPU (docs/ideas/sand.md, stage 0)

bool GroundSurface3D::_gpu_on() const {
	return gpu_simulation && GroundDeformGPU::is_available();
}

void GroundSurface3D::set_gpu_simulation(bool p_enabled) {
	if (gpu_simulation == p_enabled) {
		return;
	}
	gpu_simulation = p_enabled;
	// Each path keeps its own field: start the new one empty.
	_gpu_release();
	field.invalidate();
	field.clear();
	globals_published = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
		_upload();
		_publish();
	}
}

void GroundSurface3D::_gpu_release() {
	gpu_host.release();
	_near_release();
	gpu_ground_dirty = true;
	near_ground_dirty = true;
	gpu_cells_dirty = true;
	near_cells_dirty = true;
	globals_published = false;
}

void GroundSurface3D::GPUSink::stamp(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance, Vector2 p_velocity) {
	if (next && surface->is_near_active()) {
		next->stamp(p_columns, p_position, p_radius, p_half, p_yaw, p_box, p_bottom, p_sink, p_round, p_contact_tolerance, p_velocity);
	}
	GroundDeformField &f = is_near ? surface->near_fld : surface->field;
	GroundDeformGPUHost &host = is_near ? surface->near_host : surface->gpu_host;
	if (!f.valid || p_radius <= 0.0f || (p_box && (p_half.x <= 0.0f || p_half.y <= 0.0f))) {
		return;
	}
	// The footprint's reach, as GroundDeformField::press works it out (no crusts in the ground).
	const float ring = GroundDeformField::rim_ring(p_box, p_half, p_radius, f.rim_reach);
	const float bow = CLAMP(p_velocity.length() / GroundDeformField::BOW_SPEED, 0.0f, 1.0f);
	const float span = (p_box ? p_half.length() : p_radius) + ring * (1.0f + bow * GroundDeformField::BOW_REACH);
	int x0, x1, z0, z1;
	if (!GroundDeformGPUHost::stamp_bounds(f, p_position, span, x0, x1, z0, z1)) {
		return;
	}
	const uint32_t flags = (p_box ? 1u : 0u) | (p_round ? 2u : 0u);
	const float v[12] = {
		p_position.x, p_position.y, p_position.z, p_radius,
		p_half.x, p_half.y, p_yaw, p_bottom,
		p_sink, p_contact_tolerance, p_velocity.x, p_velocity.y
	};
	const float settle = MAX(surface->field.press_settle, 0.0f);
	const float d[4] = { 0.0f, float(flags), 0.0f, settle };
	host.add_stamp(f, v, d, x0, x1, z0, z1);
	host.slump_until = MAX(host.slump_until, host.time + settle + 0.1);
}

void GroundSurface3D::GPUSink::amount(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, float p_depth, float p_cell) {
	// On the near field a grid's cell is a tent one cell wide (GroundDeformField::amount_cover()).
	const bool tent = is_near && p_cell > 0.0f;
	if (next && surface->is_near_active()) {
		// The depth was scaled for the main field's texels (amount_scale): the near field's own scale instead, the
		// same volume.
		const float main_scale = surface->field.amount_scale(p_position, p_radius);
		const float depth = main_scale > 0.0f ? p_depth / main_scale : p_depth;
		if (p_cell > 0.0f) {
			const float to_cell = (float)Math::PI * p_radius * p_radius / (p_cell * p_cell);
			next->amount(p_columns, p_position, p_cell, depth * to_cell * surface->near_fld.amount_scale(p_position, p_cell, true), p_cell);
		} else {
			next->amount(p_columns, p_position, p_radius, depth * surface->near_fld.amount_scale(p_position, p_radius), 0.0f);
		}
	}
	GroundDeformField &f = is_near ? surface->near_fld : surface->field;
	GroundDeformGPUHost &host = is_near ? surface->near_host : surface->gpu_host;
	int x0, x1, z0, z1;
	if (!f.valid || p_radius <= 0.0f || !GroundDeformGPUHost::stamp_bounds(f, p_position, p_radius + f.texel(), x0, x1, z0, z1)) {
		return;
	}
	const float v[12] = {
		p_position.x, p_position.y, p_position.z, p_radius,
		0.0f, 0.0f, 0.0f, 0.0f,
		p_depth, tent ? 1.0f : 0.0f, 0.0f, 0.0f
	};
	const float settle = MAX(surface->field.press_settle, 0.0f);
	const float d[4] = { 0.0f, 16.0f, 0.0f, settle }; // STAMP_AMOUNT
	host.add_stamp(f, v, d, x0, x1, z0, z1);
	host.slump_until = MAX(host.slump_until, host.time + settle + 0.1);
}

// dig() (p_depth > 0) and deposit() (p_depth < 0) over a disc; p_cell: the size of the grid cell it writes, if any.
float GroundSurface3D::_amount(Vector3 p_position, float p_radius, float p_depth, float p_cell) {
	if (!field.valid || p_depth == 0.0f) {
		return 0.0f;
	}
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	if (_gpu_on()) {
		gpu_sink.amount(cols, p_position, p_radius, p_depth * field.amount_scale(p_position, p_radius), p_cell);
		// The GPU applies it; what it moves is the disc's, short only where the dent bottoms out.
		return c.deformable ? (float)Math::PI * p_radius * p_radius * Math::abs(p_depth) : 0.0f;
	}
	return field.add_amount(cols, p_position, p_radius, p_depth, p_cell);
}

float GroundSurface3D::dig(Vector3 p_position, float p_radius, float p_depth) {
	return p_depth > 0.0f ? _amount(p_position, p_radius, p_depth, 0.0f) : 0.0f;
}

float GroundSurface3D::deposit(Vector3 p_position, float p_radius, float p_depth) {
	return p_depth > 0.0f ? _amount(p_position, p_radius, -p_depth, 0.0f) : 0.0f;
}

void GroundSurface3D::add_spray(const Spray &p_spray) {
	// Nobody taking them (no grains node): what is older than a couple of physics frames is dropped.
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	if (frame > spray_frame + 2) {
		sprays.clear();
	}
	spray_frame = frame;
	if (sprays.size() < 256) {
		sprays.push_back(p_spray);
	}
}

void GroundSurface3D::add_dust(Vector3 p_position, Vector3 p_velocity, float p_strength) {
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	if (frame > dust_frame + 2) {
		dust.clear();
		near_dust.clear();
	}
	dust_frame = frame;
	const float share = get_blown_at(p_position.x, p_position.z);
	if (share <= 0.0f || p_strength <= 0.0f || dust.size() >= 256) {
		return;
	}
	Dust d;
	d.position = p_position;
	d.velocity = p_velocity;
	d.rate = dust_emission * share * p_strength;
	if (frame <= dust_claim_frame + 2 && p_position.distance_squared_to(dust_claim_center) < dust_claim_radius * dust_claim_radius) {
		if (near_dust.size() < 256) {
			near_dust.push_back(d);
		}
		return;
	}
	dust.push_back(d);
}

void GroundSurface3D::take_dust(LocalVector<Dust> &r_dust) {
	r_dust = dust;
	dust.clear();
}

void GroundSurface3D::claim_dust(Vector3 p_center, float p_radius) {
	dust_claim_center = p_center;
	dust_claim_radius = MAX(p_radius, 0.0f);
	dust_claim_frame = Engine::get_singleton()->get_physics_frames();
}

void GroundSurface3D::take_near_dust(LocalVector<Dust> &r_dust) {
	r_dust = near_dust;
	near_dust.clear();
}

Array GroundSurface3D::take_sprays() {
	Array out;
	for (const Spray &s : sprays) {
		Dictionary d;
		d["position"] = s.position;
		d["velocity"] = s.velocity;
		d["width"] = s.width;
		d["slip_speed"] = s.slip_speed;
		d["reach_center"] = s.reach_center;
		d["reach_radius"] = s.reach_radius;
		d["side_share"] = s.side_share;
		d["side_volume_rate"] = s.side_volume_rate;
		d["stickiness"] = s.stickiness;
		d["volume"] = s.volume;
		out.push_back(d);
	}
	sprays.clear();
	return out;
}

PackedByteArray GroundSurface3D::_gpu_cells() const {
	PackedByteArray out;
	const int n = ground_n;
	if (!ground_valid || int(cells.size()) != n * n) {
		return out;
	}
	out.resize(n * n * 48);
	float *w = reinterpret_cast<float *>(out.ptrw());
	const bool inputs = int(cell_inputs.size()) == n * n;
	bool seep = false;
	const bool runoff_gpu = _runoff_on_gpu();
	bool runoff_any = false;
	for (int i = 0; i < n * n; i++) {
		const Cell &c = cells[i];
		float *o = w + i * 12;
		o[0] = c.rim_share;
		o[1] = c.tan_repose;
		o[2] = c.rate;
		o[3] = c.max_dent;
		o[4] = c.darken;
		o[5] = c.gloss;
		o[6] = c.blown * ((int)shelter_flux.size() == n * n ? shelter_flux[i] : 1.0f);
		o[7] = c.deformable ? 1.0f : 0.0f;
		// Standing water's inputs: conductivity and the water table's depth.
		o[8] = inputs ? _water_conductivity(cell_inputs[i]) : 0.0f;
		o[9] = inputs ? _water_table_depth(i) : 10.0f;
		o[10] = inputs ? 1.0f - CLAMP(MAX(cell_inputs[i].moisture, (int)pore.size() == n * n ? pore[i] : 0.0f), 0.0f, 1.0f) : 0.0f;
		// Runoff channels on the GPU: the seepage face's outflow, m/s; -1 under the sea or a lake (the runoff ends).
		o[11] = runoff_gpu ? ((int)water_depth.size() == n * n && water_depth[i] > 0.002f ? -1.0f : _runoff_seep(i)) : 0.0f;
		runoff_any = runoff_any || o[11] > 0.0f;
		seep = seep || (c.deformable && o[9] < c.max_dent);
	}
	const_cast<GroundSurface3D *>(this)->water_seep = seep;
	const_cast<GroundSurface3D *>(this)->runoff_seep_any = runoff_any;
	return out;
}

PackedByteArray GroundSurface3D::_gpu_params(const GroundDeformField &p_field, float p_edge_margin, double p_delta, double p_age, double p_slow) const {
	PackedByteArray b;
	b.resize(GroundDeformGPU::PARAMS_BYTES);
	uint8_t *w = b.ptrw();
	memset(w, 0, GroundDeformGPU::PARAMS_BYTES);
	const int n = p_field.resolution;
	const int32_t res[4] = { n, n / GroundDeformField::TILE, ground_n, ground_n };
	memcpy(w, res, 16);
	float *f = reinterpret_cast<float *>(w + 16);
	auto put = [&](int p_vec, float a, float bb, float c, float d) {
		f[p_vec * 4 + 0] = a;
		f[p_vec * 4 + 1] = bb;
		f[p_vec * 4 + 2] = c;
		f[p_vec * 4 + 3] = d;
	};
	const Rect2 r = p_field.rect();
	put(0, float(p_field.origin.x), float(p_field.origin.y), p_field.texel(), float(p_delta));
	put(1, float(ground_origin.x), float(ground_origin.y), ground_spacing, 1.0f);
	put(5, r.position.x, r.position.y, r.size.x, r.size.y);
	put(8, GroundDeformField::BOW_SPEED, GroundDeformField::BOW_GAIN, GroundDeformField::BOW_REACH, p_field.rim_reach);
	put(9, GroundDeformField::CRACK_LENGTH, GroundDeformField::PLATE, float(GroundDeformField::CRACK_SECTORS), 0.0f);
	// Ageing (_age): the same numbers over p_age seconds.
	float drift = 0.0f;
	float soften = 0.0f;
	if (p_age > 0.0) {
		const WeatherMap3D *wm = WeatherMap3D::get_active();
		drift = sand_drift_rate * MAX(_wind_10m().length() - 5.0f, 0.0f) / 5.0f;
		if (wm) {
			const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
			soften = rain_smoothing * wm->get_rain_at(at);
		}
	}
	const float dry = p_age > 0.0 ? Math::exp(-float(p_age) / drying_time) : 1.0f;
	put(11, 1.0f, float(p_slow), dry, drift);
	put(12, soften, float(p_age) / 3600.0f, p_edge_margin, float(p_field.valid && &p_field == &near_fld ? _near_passes() : 1));
	// Standing water: the rain, a pipe-model substep, damping, evaporation (rates sped up by runoff_time_scale);
	// then the seepage length, the conductivity compaction keeps, on, the thinnest film kept once its source stops.
	if ((standing_water || _runoff_on_gpu()) && p_delta > 0.0) {
		const WeatherMap3D *wm = WeatherMap3D::get_active();
		const float rain = wm ? MAX(wm->get_rain_at(Vector3(center.x, _ground_at(center.x, center.y), center.y)), 0.0f) / 3.6e6f : 0.0f;
		const int passes = _water_passes(p_field, p_delta);
		const float sub = MIN(float(p_delta) / passes, 0.5f * p_field.texel() / Math::sqrt(9.81f * 0.2f));
		// A long step moves the water only as far as the substeps reach, but its sources and sinks get the
		// whole step: their rates are scaled to land the step's water in the substeps.
		const float gain = runoff_time_scale * float(p_delta) / (sub * passes);
		// Damping: puddles settle in a second or two rather than sloshing on (the pipe model has no friction).
		put(13, rain * gain, sub, 2.0f, _evaporation() * gain);
		// Compacted ruts drain slower: tyres cut the conductivity several fold (Horn et al. 1995): a third kept.
		put(14, 0.02f, 1.0f / 3.0f, gain, 2e-5f);
		if (_runoff_on_gpu()) {
			// Runoff channels: on; the morphological factor (the sand moves runoff_time_scale times faster, the
			// water at its own pace); 2 mm of bed roughness; the seepage landed in the substeps at its own rate.
			put(15, 1.0f, runoff_time_scale / _morph_passes(), 0.002f, gain / MAX(runoff_time_scale, 1e-6f));
		}
	}
	return b;
}

void GroundSurface3D::_gpu_submit(double p_delta, double p_age, bool p_readback, bool p_sync) {
	_gpu_submit_field(gpu_host, field, gpu_ground_dirty, gpu_cells_dirty, gpu_slow_accum, 1, terrain_margin + skirt_fade, p_delta, p_age, p_readback, p_sync);
	if (is_near_active()) {
		// The near field lies inside the main one's deformable middle: no edge margin of its own.
		_gpu_submit_field(near_host, near_fld, near_ground_dirty, near_cells_dirty, near_slow_accum, _near_passes(), 0.0f, p_delta, p_age, p_readback, p_sync);
	}
	if (p_sync) {
		_gpu_take();
	}
}

void GroundSurface3D::_gpu_submit_field(GroundDeformGPUHost &r_host, const GroundDeformField &p_field, bool &r_ground_dirty, bool &r_cells_dirty, double &r_slow_accum, int p_passes, float p_edge_margin, double p_delta, double p_age, bool p_readback, bool p_sync) {
	if (r_host.gpu.is_null()) {
		r_host.gpu.instantiate();
		r_host.reset = true;
		r_ground_dirty = true;
		r_cells_dirty = true;
	}
	if (r_host.texture.is_null()) {
		r_host.texture.instantiate();
	}
	r_host.time += p_delta;
	int flags = 0;
	PackedByteArray ground_bytes;
	if (r_ground_dirty && ground_valid) {
		ground_bytes.resize(ground_n * ground_n * 4);
		memcpy(ground_bytes.ptrw(), ground.ptr(), ground_n * ground_n * 4);
		flags |= GroundDeformGPU::STEP_GROUND;
		r_ground_dirty = false;
	}
	PackedByteArray cell_bytes;
	if (r_cells_dirty && ground_valid) {
		cell_bytes = _gpu_cells();
		if (cell_bytes.size() > 0) {
			flags |= GroundDeformGPU::STEP_COVER;
			r_cells_dirty = false;
		}
	}
	if (r_host.reset) {
		flags |= GroundDeformGPU::STEP_RESET;
		r_host.uploads.clear(); // a reset returns before the tile moves: nothing seeded survives it
	}
	if (r_host.shift.size() > 0 || r_host.stores.size() > 0) {
		flags |= GroundDeformGPU::STEP_SHIFT;
	}
	if (r_host.stamps.size() > 0) {
		flags |= GroundDeformGPU::STEP_STAMPS;
	}
	// Slow material (mud) moves every tenth of a second, by the seconds saved up (the CPU's tile wait).
	double slow = 0.0;
	if (r_host.time <= r_host.slump_until && p_delta > 0.0) {
		flags |= GroundDeformGPU::STEP_SLUMP;
		r_slow_accum += p_delta;
		if (r_slow_accum >= 0.1) {
			slow = r_slow_accum;
			r_slow_accum = 0.0;
		}
	} else {
		r_slow_accum = 0.0;
	}
	if (p_age > 0.0) {
		flags |= GroundDeformGPU::STEP_RELAX;
	}
	// Standing water runs while there is some, rain falls, or ground water can seep into a print.
	int water_passes = 0;
	if ((standing_water || _runoff_on_gpu()) && p_delta > 0.0) {
		const WeatherMap3D *wm = WeatherMap3D::get_active();
		const bool rain = wm && wm->get_rain_at(Vector3(center.x, _ground_at(center.x, center.y), center.y)) > 0.0f;
		const bool runoff = _runoff_on_gpu() && (rain || runoff_seep_any || water_seen > 0.0f);
		if (runoff || (standing_water && (rain || water_seen > 0.0f || water_seep))) {
			flags |= GroundDeformGPU::STEP_WATER;
			water_passes = _water_passes(p_field, p_delta);
		}
		if (runoff) {
			// The water cuts and fills the bed: the cuts' walls slump while it runs.
			flags |= GroundDeformGPU::STEP_RUNOFF | GroundDeformGPU::STEP_SLUMP;
		}
	}
	if (flags & (GroundDeformGPU::STEP_RESET | GroundDeformGPU::STEP_SHIFT | GroundDeformGPU::STEP_STAMPS | GroundDeformGPU::STEP_SLUMP | GroundDeformGPU::STEP_RELAX | GroundDeformGPU::STEP_COVER | GroundDeformGPU::STEP_WATER)) {
		flags |= GroundDeformGPU::STEP_BLIT;
		r_host.changed = true;
	}
	if ((p_readback && r_host.changed) || p_sync) {
		flags |= GroundDeformGPU::STEP_READBACK;
		r_host.changed = false;
	}
	if (p_sync) {
		flags |= GroundDeformGPU::STEP_SYNC;
	}
	GroundDeformGPU::Dims d;
	d.n = p_field.resolution;
	d.gn = ground_n;
	d.cn = ground_n;
	d.slots = MAX(max_cached_tiles, 1);
	d.coll_w = 0;
	d.stride = 2;
	d.slump_passes = p_passes;
	d.cover_vec4s = 3;
	d.water_passes = water_passes;
	d.morph_passes = _morph_passes();
	r_host.gpu->step(d, flags, _gpu_params(p_field, p_edge_margin, p_delta, p_age, slow), ground_bytes, cell_bytes, r_host.stamps, r_host.stores, r_host.shift, p_field.origin, Vector2(), r_host.uploads);
	r_host.stamps.clear();
	r_host.stores.clear();
	r_host.shift.clear();
	r_host.uploads.clear();
	r_host.reset = false;
}

void GroundSurface3D::_gpu_take() {
	if (gpu_host.gpu.is_null()) {
		return;
	}
	if (gpu_host.take_texture()) {
		globals_published = false;
	}
	// Slumping goes on while any texel still owes some (neighbours of a texel that moved are marked again,
	// as on the CPU).
	float owed = 0.0f;
	const bool took = gpu_host.take_field(field, 2, &owed);
	if (took && owed > 0.0f) {
		gpu_host.slump_until = MAX(gpu_host.slump_until, gpu_host.time + owed + 0.1);
	}
	if (took) {
		// The deepest standing water (the near field holds no water the main one lacks for long).
		float deepest = 0.0f;
		for (float w : field.water) {
			deepest = MAX(deepest, w);
		}
		water_seen = deepest;
	}
	if (is_near_active() && near_host.gpu.is_valid()) {
		if (near_host.take_texture()) {
			globals_published = false;
		}
		float near_owed = 0.0f;
		if (near_host.take_field(near_fld, 2, &near_owed) && near_owed > 0.0f) {
			near_host.slump_until = MAX(near_host.slump_until, near_host.time + near_owed + 0.1);
		}
	}
}

void GroundSurface3D::sync_deformation() {
	if (!_gpu_on() || !field.valid || !is_inside_tree()) {
		return;
	}
	_gpu_submit(0.0, 0.0, true, true);
	_publish();
}

// ---------------------------------------------------------------------------------------------------------
// Pressing and queries

float GroundSurface3D::press(Vector3 p_position, float p_radius, float p_bottom, float p_sink, bool p_round) {
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	// On the GPU path the CPU copy takes the press too, so the volume returned and queries made before the
	// next read back see it; the GPU field takes it as a stamp.
	const float moved = field.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance);
	if (is_near_active()) {
		near_fld.press_settle = field.press_settle;
		near_fld.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance);
	}
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance, Vector2());
	}
	return moved;
}

float GroundSurface3D::press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink) {
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const float moved = field.press(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, contact_tolerance);
	if (is_near_active()) {
		near_fld.press_settle = field.press_settle;
		near_fld.press(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, contact_tolerance);
	}
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, contact_tolerance, Vector2());
	}
	return moved;
}

float GroundSurface3D::get_sinkage_at(Vector3 p_position, float p_mass, float p_area, float p_width) const {
	Columns cols;
	cols.surface = this;
	GroundDeformField::Body b;
	b.center = p_position;
	b.mass = p_mass;
	b.area = p_area;
	b.width = p_width;
	return cols.sinkage(b);
}

void GroundSurface3D::clear_deformation() {
	field.clear();
	near_fld.clear();
	gpu_host.reset = true;
	gpu_host.clear_cache();
	gpu_host.stamps.clear();
	gpu_host.slump_until = -1.0;
	near_host.reset = true;
	near_host.clear_cache();
	near_host.stamps.clear();
	near_host.uploads.clear();
	near_host.slump_until = -1.0;
	if (is_inside_tree()) {
		_upload();
	}
}

void GroundSurface3D::refresh_ground() {
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
		_publish();
	}
}

float GroundSurface3D::get_ground_height_at(Vector3 p_position) const {
	return _ground_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_surface_height_at(Vector3 p_position) const {
	return _ground_at(p_position.x, p_position.z) + get_rim_at(p_position) - get_dent_at(p_position);
}

// Inside the near field (less its blend band) the queries read its finer copy.
float GroundSurface3D::get_dent_at(Vector3 p_position) const {
	return _near_inside(p_position.x, p_position.z) ? near_fld.dent_at(p_position.x, p_position.z) : field.dent_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_rim_at(Vector3 p_position) const {
	return _near_inside(p_position.x, p_position.z) ? near_fld.rim_at(p_position.x, p_position.z) : field.rim_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_fresh_at(Vector3 p_position) const {
	if (_near_inside(p_position.x, p_position.z)) {
		return near_fld.sample(near_fld.fresh, p_position.x, p_position.z);
	}
	return field.valid ? field.sample(field.fresh, p_position.x, p_position.z) : 0.0f;
}

Dictionary GroundSurface3D::get_material_at(Vector3 p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	Dictionary d;
	d["n"] = c.n;
	d["kc"] = c.kc;
	d["kphi"] = c.kphi;
	d["rim_share"] = c.rim_share;
	d["repose"] = Math::rad_to_deg(Math::atan(c.tan_repose));
	d["rate"] = c.rate;
	d["max_dent"] = c.max_dent;
	d["litter"] = c.litter;
	d["sand"] = c.sand;
	d["blown"] = c.blown;
	d["deformable"] = c.deformable;
	return d;
}

Vector2 GroundSurface3D::get_moved_volume() const {
	return field.moved_volume();
}

Rect2 GroundSurface3D::get_deform_rect() const {
	return field.valid ? field.rect() : Rect2();
}

PackedStringArray GroundSurface3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	const BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(material.ptr());
	if (material.is_null()) {
		warnings.push_back(RTR("No material: the patch is drawn plain. Give it the terrain's material, with Ground Deform on, so the terrain is lowered under it."));
	} else if (bm && !bm->get_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM)) {
		warnings.push_back(RTR("The material's Ground Deform is off: the terrain drawn with it will not be lowered and will hide the prints."));
	}
	return warnings;
}

// ---------------------------------------------------------------------------------------------------------
// Settings

#define GROUND_REBUILD           \
	ground_valid = false;        \
	field.invalidate();          \
	field.clear();               \
	mesh_dirty = true;           \
	if (is_inside_tree()) {      \
		_ensure_children();      \
		_follow(_follow_position()); \
		_upload();               \
		_publish();              \
	}

void GroundSurface3D::set_height_source(const NodePath &p_path) {
	height_source = p_path;
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
}

void GroundSurface3D::set_ground_type(GroundType p_type) {
	ground_type = p_type;
	_update_cells();
}

void GroundSurface3D::set_moisture(float p_moisture) {
	moisture = p_moisture < 0.0f ? -1.0f : MIN(p_moisture, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_litter_depth(float p_m) {
	litter_depth = CLAMP(p_m, 0.0f, 0.5f);
	_update_cells();
}

void GroundSurface3D::set_litter_cover(float p_cover) {
	litter_cover = CLAMP(p_cover, 0.0f, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_rock_from_geology(bool p_enable) {
	rock_from_geology = p_enable;
	_update_cells();
}

void GroundSurface3D::set_type_map(const Ref<Image> &p_map) {
	type_map = p_map;
	if (type_map.is_valid() && type_map->is_compressed()) {
		type_map = type_map->duplicate();
		type_map->decompress();
	}
	_update_cells();
}

void GroundSurface3D::set_type_map_rect(const Rect2 &p_rect) {
	type_map_rect = p_rect;
	_update_cells();
}

void GroundSurface3D::set_deform_extent(float p_extent) {
	deform_extent = CLAMP(p_extent, 8.0f, 256.0f);
	GROUND_REBUILD
}

void GroundSurface3D::set_deform_resolution(int p_resolution) {
	const int T = GroundDeformField::TILE;
	deform_resolution = CLAMP((p_resolution + T - 1) / T * T, T * 2, 4096);
	GROUND_REBUILD
}

void GroundSurface3D::set_ground_spacing(float p_spacing) {
	ground_spacing = CLAMP(p_spacing, 0.05f, 4.0f);
	GROUND_REBUILD
}

void GroundSurface3D::set_detail_spacing(float p_spacing) {
	detail_spacing = CLAMP(p_spacing, 0.01f, 1.0f);
	mesh_dirty = true;
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void GroundSurface3D::set_outer_spacing(float p_spacing) {
	outer_spacing = CLAMP(p_spacing, 0.01f, 4.0f);
	mesh_dirty = true;
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void GroundSurface3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_update_patch_material();
	if (is_inside_tree()) {
		center = Vector2(Math::INF, Math::INF); // re-apply the UV offset
		_follow(_follow_position());
	}
	update_configuration_warnings();
}

void GroundSurface3D::set_skirt(float p_m) {
	skirt = CLAMP(p_m, 0.0f, 10.0f);
	globals_published = false;
}

void GroundSurface3D::set_terrain_margin(float p_m) {
	terrain_margin = CLAMP(p_m, 0.0f, 32.0f);
	globals_published = false;
}

void GroundSurface3D::set_skirt_fade(float p_m) {
	skirt_fade = CLAMP(p_m, 0.01f, 32.0f);
	globals_published = false;
}

void GroundSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &GroundSurface3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &GroundSurface3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_height_source", "path"), &GroundSurface3D::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &GroundSurface3D::get_height_source);
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &GroundSurface3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &GroundSurface3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &GroundSurface3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &GroundSurface3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_ground_type", "type"), &GroundSurface3D::set_ground_type);
	ClassDB::bind_method(D_METHOD("get_ground_type"), &GroundSurface3D::get_ground_type);
	ClassDB::bind_method(D_METHOD("set_moisture", "moisture"), &GroundSurface3D::set_moisture);
	ClassDB::bind_method(D_METHOD("get_moisture"), &GroundSurface3D::get_moisture);
	ClassDB::bind_method(D_METHOD("set_litter_depth", "depth"), &GroundSurface3D::set_litter_depth);
	ClassDB::bind_method(D_METHOD("get_litter_depth"), &GroundSurface3D::get_litter_depth);
	ClassDB::bind_method(D_METHOD("set_litter_cover", "cover"), &GroundSurface3D::set_litter_cover);
	ClassDB::bind_method(D_METHOD("get_litter_cover"), &GroundSurface3D::get_litter_cover);
	ClassDB::bind_method(D_METHOD("set_rock_from_geology", "enable"), &GroundSurface3D::set_rock_from_geology);
	ClassDB::bind_method(D_METHOD("is_rock_from_geology"), &GroundSurface3D::is_rock_from_geology);
	ClassDB::bind_method(D_METHOD("set_type_map", "map"), &GroundSurface3D::set_type_map);
	ClassDB::bind_method(D_METHOD("get_type_map"), &GroundSurface3D::get_type_map);
	ClassDB::bind_method(D_METHOD("set_type_map_rect", "rect"), &GroundSurface3D::set_type_map_rect);
	ClassDB::bind_method(D_METHOD("get_type_map_rect"), &GroundSurface3D::get_type_map_rect);
	ClassDB::bind_method(D_METHOD("set_deform_extent", "extent"), &GroundSurface3D::set_deform_extent);
	ClassDB::bind_method(D_METHOD("get_deform_extent"), &GroundSurface3D::get_deform_extent);
	ClassDB::bind_method(D_METHOD("set_deform_resolution", "resolution"), &GroundSurface3D::set_deform_resolution);
	ClassDB::bind_method(D_METHOD("get_deform_resolution"), &GroundSurface3D::get_deform_resolution);
	ClassDB::bind_method(D_METHOD("set_ground_spacing", "spacing"), &GroundSurface3D::set_ground_spacing);
	ClassDB::bind_method(D_METHOD("get_ground_spacing"), &GroundSurface3D::get_ground_spacing);
	ClassDB::bind_method(D_METHOD("set_detail_spacing", "spacing"), &GroundSurface3D::set_detail_spacing);
	ClassDB::bind_method(D_METHOD("get_detail_spacing"), &GroundSurface3D::get_detail_spacing);
	ClassDB::bind_method(D_METHOD("set_outer_spacing", "spacing"), &GroundSurface3D::set_outer_spacing);
	ClassDB::bind_method(D_METHOD("get_outer_spacing"), &GroundSurface3D::get_outer_spacing);
	ClassDB::bind_method(D_METHOD("set_deform_groups", "groups"), &GroundSurface3D::set_deform_groups);
	ClassDB::bind_method(D_METHOD("get_deform_groups"), &GroundSurface3D::get_deform_groups);
	ClassDB::bind_method(D_METHOD("set_contact_tolerance", "distance"), &GroundSurface3D::set_contact_tolerance);
	ClassDB::bind_method(D_METHOD("get_contact_tolerance"), &GroundSurface3D::get_contact_tolerance);
	ClassDB::bind_method(D_METHOD("set_character_mass", "mass"), &GroundSurface3D::set_character_mass);
	ClassDB::bind_method(D_METHOD("get_character_mass"), &GroundSurface3D::get_character_mass);
	ClassDB::bind_method(D_METHOD("set_character_contact_area", "area"), &GroundSurface3D::set_character_contact_area);
	ClassDB::bind_method(D_METHOD("get_character_contact_area"), &GroundSurface3D::get_character_contact_area);
	ClassDB::bind_method(D_METHOD("set_max_cached_tiles", "tiles"), &GroundSurface3D::set_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("get_max_cached_tiles"), &GroundSurface3D::get_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &GroundSurface3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &GroundSurface3D::get_material);
	ClassDB::bind_method(D_METHOD("set_skirt", "depth"), &GroundSurface3D::set_skirt);
	ClassDB::bind_method(D_METHOD("get_skirt"), &GroundSurface3D::get_skirt);
	ClassDB::bind_method(D_METHOD("set_terrain_margin", "distance"), &GroundSurface3D::set_terrain_margin);
	ClassDB::bind_method(D_METHOD("get_terrain_margin"), &GroundSurface3D::get_terrain_margin);
	ClassDB::bind_method(D_METHOD("set_skirt_fade", "distance"), &GroundSurface3D::set_skirt_fade);
	ClassDB::bind_method(D_METHOD("get_skirt_fade"), &GroundSurface3D::get_skirt_fade);
	ClassDB::bind_method(D_METHOD("set_drying_time", "seconds"), &GroundSurface3D::set_drying_time);
	ClassDB::bind_method(D_METHOD("get_drying_time"), &GroundSurface3D::get_drying_time);
	ClassDB::bind_method(D_METHOD("set_wind_override", "wind"), &GroundSurface3D::set_wind_override);
	ClassDB::bind_method(D_METHOD("get_wind_override"), &GroundSurface3D::get_wind_override);
	ClassDB::bind_method(D_METHOD("set_mud_layer", "enabled"), &GroundSurface3D::set_mud_layer);
	ClassDB::bind_method(D_METHOD("is_mud_layer"), &GroundSurface3D::is_mud_layer);
	ClassDB::bind_method(D_METHOD("set_mud_map", "map"), &GroundSurface3D::set_mud_map);
	ClassDB::bind_method(D_METHOD("set_standing_water", "enabled"), &GroundSurface3D::set_standing_water);
	ClassDB::bind_method(D_METHOD("is_standing_water"), &GroundSurface3D::is_standing_water);
	ClassDB::bind_method(D_METHOD("get_standing_water_at", "position"), &GroundSurface3D::get_standing_water_at);
	ClassDB::bind_method(D_METHOD("set_desiccation_cracks", "enabled"), &GroundSurface3D::set_desiccation_cracks);
	ClassDB::bind_method(D_METHOD("is_desiccation_cracks"), &GroundSurface3D::is_desiccation_cracks);
	ClassDB::bind_method(D_METHOD("get_crack_at", "position"), &GroundSurface3D::get_crack_at);
	ClassDB::bind_method(D_METHOD("get_mud_map"), &GroundSurface3D::get_mud_map);
	ClassDB::bind_method(D_METHOD("set_mud_map_rect", "rect"), &GroundSurface3D::set_mud_map_rect);
	ClassDB::bind_method(D_METHOD("get_mud_map_rect"), &GroundSurface3D::get_mud_map_rect);
	ClassDB::bind_method(D_METHOD("set_mud_max_depth", "depth"), &GroundSurface3D::set_mud_max_depth);
	ClassDB::bind_method(D_METHOD("get_mud_max_depth"), &GroundSurface3D::get_mud_max_depth);
	ClassDB::bind_method(D_METHOD("set_mud_auto_depth", "depth"), &GroundSurface3D::set_mud_auto_depth);
	ClassDB::bind_method(D_METHOD("get_mud_auto_depth"), &GroundSurface3D::get_mud_auto_depth);
	ClassDB::bind_method(D_METHOD("set_mud_wet_start", "moisture"), &GroundSurface3D::set_mud_wet_start);
	ClassDB::bind_method(D_METHOD("get_mud_wet_start"), &GroundSurface3D::get_mud_wet_start);
	ClassDB::bind_method(D_METHOD("set_mud_wet_full", "moisture"), &GroundSurface3D::set_mud_wet_full);
	ClassDB::bind_method(D_METHOD("get_mud_wet_full"), &GroundSurface3D::get_mud_wet_full);
	ClassDB::bind_method(D_METHOD("set_data_map", "path"), &GroundSurface3D::set_data_map);
	ClassDB::bind_method(D_METHOD("get_data_map"), &GroundSurface3D::get_data_map);
	ClassDB::bind_method(D_METHOD("get_mud_depth_at", "position"), &GroundSurface3D::get_mud_depth_at);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("mud_yield_stress", "concentration"), &GroundSurface3D::mud_yield_stress);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("mud_viscosity", "concentration"), &GroundSurface3D::mud_viscosity);
	ClassDB::bind_method(D_METHOD("set_near_field", "enabled"), &GroundSurface3D::set_near_field);
	ClassDB::bind_method(D_METHOD("is_near_field"), &GroundSurface3D::is_near_field);
	ClassDB::bind_method(D_METHOD("set_near_extent", "extent"), &GroundSurface3D::set_near_extent);
	ClassDB::bind_method(D_METHOD("get_near_extent"), &GroundSurface3D::get_near_extent);
	ClassDB::bind_method(D_METHOD("set_near_resolution", "resolution"), &GroundSurface3D::set_near_resolution);
	ClassDB::bind_method(D_METHOD("get_near_resolution"), &GroundSurface3D::get_near_resolution);
	ClassDB::bind_method(D_METHOD("get_near_rect"), &GroundSurface3D::get_near_rect);
	ClassDB::bind_method(D_METHOD("get_near_moved_volume"), &GroundSurface3D::get_near_moved_volume);
	ClassDB::bind_method(D_METHOD("is_near_active"), &GroundSurface3D::is_near_active);
	ClassDB::bind_method(D_METHOD("set_wind_ripples", "enabled"), &GroundSurface3D::set_wind_ripples);
	ClassDB::bind_method(D_METHOD("is_wind_ripples"), &GroundSurface3D::is_wind_ripples);
	ClassDB::bind_method(D_METHOD("set_water_sources", "sources"), &GroundSurface3D::set_water_sources);
	ClassDB::bind_method(D_METHOD("get_water_sources"), &GroundSurface3D::get_water_sources);
	ClassDB::bind_method(D_METHOD("set_water_coupling", "enabled"), &GroundSurface3D::set_water_coupling);
	ClassDB::bind_method(D_METHOD("is_water_coupling"), &GroundSurface3D::is_water_coupling);
	ClassDB::bind_method(D_METHOD("get_saturation_at", "position"), &GroundSurface3D::get_saturation_at);
	ClassDB::bind_method(D_METHOD("get_water_speed_at", "position"), &GroundSurface3D::get_water_speed_at);
	ClassDB::bind_method(D_METHOD("get_water_step_usec"), &GroundSurface3D::get_water_step_usec);
	ClassDB::bind_method(D_METHOD("set_sand_glints", "enabled"), &GroundSurface3D::set_sand_glints);
	ClassDB::bind_method(D_METHOD("is_sand_glints"), &GroundSurface3D::is_sand_glints);
	ClassDB::bind_method(D_METHOD("set_wind_shelters", "shelters"), &GroundSurface3D::set_wind_shelters);
	ClassDB::bind_method(D_METHOD("get_wind_shelters"), &GroundSurface3D::get_wind_shelters);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("shelter_ratio_for", "lambda"), &GroundSurface3D::shelter_ratio_for);
	ClassDB::bind_method(D_METHOD("get_blown_at", "x", "z"), &GroundSurface3D::get_blown_at);
	ClassDB::bind_method(D_METHOD("get_sand_flux_at", "position"), &GroundSurface3D::get_sand_flux_at);
	ClassDB::bind_method(D_METHOD("get_deposition_rate_at", "position"), &GroundSurface3D::get_deposition_rate_at);
	ClassDB::bind_method(D_METHOD("get_sand_deposit_at", "position"), &GroundSurface3D::get_sand_deposit_at);
	ClassDB::bind_method(D_METHOD("get_wind_step_usec"), &GroundSurface3D::get_wind_step_usec);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("bed_load", "water_speed"), &GroundSurface3D::bed_load);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("bed_load_shear", "shear_stress"), &GroundSurface3D::bed_load_shear);
	ClassDB::bind_method(D_METHOD("set_runoff_channels", "enabled"), &GroundSurface3D::set_runoff_channels);
	ClassDB::bind_method(D_METHOD("is_runoff_channels"), &GroundSurface3D::is_runoff_channels);
	ClassDB::bind_method(D_METHOD("set_runoff_time_scale", "scale"), &GroundSurface3D::set_runoff_time_scale);
	ClassDB::bind_method(D_METHOD("get_runoff_time_scale"), &GroundSurface3D::get_runoff_time_scale);
	ClassDB::bind_method(D_METHOD("get_runoff_at", "position"), &GroundSurface3D::get_runoff_at);
	ClassDB::bind_method(D_METHOD("get_runoff_cut_at", "position"), &GroundSurface3D::get_runoff_cut_at);
	ClassDB::bind_method(D_METHOD("get_runoff_step_usec"), &GroundSurface3D::get_runoff_step_usec);
	ClassDB::bind_method(D_METHOD("set_ripple_extent", "extent"), &GroundSurface3D::set_ripple_extent);
	ClassDB::bind_method(D_METHOD("get_ripple_extent"), &GroundSurface3D::get_ripple_extent);
	ClassDB::bind_method(D_METHOD("set_ripple_time_scale", "scale"), &GroundSurface3D::set_ripple_time_scale);
	ClassDB::bind_method(D_METHOD("get_ripple_time_scale"), &GroundSurface3D::get_ripple_time_scale);
	ClassDB::bind_method(D_METHOD("get_ripple_at", "position"), &GroundSurface3D::get_ripple_at);
	ClassDB::bind_method(D_METHOD("is_ripples_ready"), &GroundSurface3D::is_ripples_ready);
	ClassDB::bind_method(D_METHOD("get_ripple_steps"), &GroundSurface3D::get_ripple_steps);
	ClassDB::bind_method(D_METHOD("get_ripple_heights"), &GroundSurface3D::get_ripple_heights);
	ClassDB::bind_method(D_METHOD("get_ripple_origin"), &GroundSurface3D::get_ripple_origin);
	ClassDB::bind_method(D_METHOD("get_ripple_cell"), &GroundSurface3D::get_ripple_cell);
	ClassDB::bind_method(D_METHOD("get_ripple_size"), &GroundSurface3D::get_ripple_size);
	ClassDB::bind_method(D_METHOD("set_sand_drift_rate", "rate"), &GroundSurface3D::set_sand_drift_rate);
	ClassDB::bind_method(D_METHOD("get_sand_drift_rate"), &GroundSurface3D::get_sand_drift_rate);
	ClassDB::bind_method(D_METHOD("set_rain_smoothing", "rate"), &GroundSurface3D::set_rain_smoothing);
	ClassDB::bind_method(D_METHOD("get_rain_smoothing"), &GroundSurface3D::get_rain_smoothing);

	ClassDB::bind_method(D_METHOD("get_wheel_soil", "position", "load", "radius", "width", "inflation", "slip", "slip_angle"), &GroundSurface3D::get_wheel_soil, DEFVAL(200.0f), DEFVAL(0.0f), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("press_wheel", "position", "size", "yaw", "sinkage", "velocity"), &GroundSurface3D::press_wheel, DEFVAL(Vector2()));
	ClassDB::bind_method(D_METHOD("get_footing_at", "position", "mass", "area"), &GroundSurface3D::get_footing_at, DEFVAL(75.0f), DEFVAL(0.035f));
	ClassDB::bind_method(D_METHOD("dig", "position", "radius", "depth"), &GroundSurface3D::dig);
	ClassDB::bind_method(D_METHOD("deposit", "position", "radius", "depth"), &GroundSurface3D::deposit);
	ClassDB::bind_method(D_METHOD("take_sprays"), &GroundSurface3D::take_sprays);
	ClassDB::bind_method(D_METHOD("set_ploughing", "enabled"), &GroundSurface3D::set_ploughing);
	ClassDB::bind_method(D_METHOD("set_dust_emission", "rate"), &GroundSurface3D::set_dust_emission);
	ClassDB::bind_method(D_METHOD("get_dust_emission"), &GroundSurface3D::get_dust_emission);
	ClassDB::bind_method(D_METHOD("is_ploughing"), &GroundSurface3D::is_ploughing);
	ClassDB::bind_method(D_METHOD("set_gpu_simulation", "enabled"), &GroundSurface3D::set_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_simulation"), &GroundSurface3D::is_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_active"), &GroundSurface3D::is_gpu_active);
	ClassDB::bind_method(D_METHOD("sync_deformation"), &GroundSurface3D::sync_deformation);
	ClassDB::bind_method(D_METHOD("press", "position", "radius", "bottom", "sink", "round"), &GroundSurface3D::press, DEFVAL(0.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("press_box", "position", "size", "yaw", "bottom", "sink"), &GroundSurface3D::press_box, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_sinkage_at", "position", "mass", "area", "width"), &GroundSurface3D::get_sinkage_at, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("settle", "seconds", "step"), &GroundSurface3D::settle, DEFVAL(1.0f / 60.0f));
	ClassDB::bind_method(D_METHOD("clear_deformation"), &GroundSurface3D::clear_deformation);
	ClassDB::bind_method(D_METHOD("refresh_ground"), &GroundSurface3D::refresh_ground);
	ClassDB::bind_method(D_METHOD("get_surface_height_at", "position"), &GroundSurface3D::get_surface_height_at);
	ClassDB::bind_method(D_METHOD("get_ground_height_at", "position"), &GroundSurface3D::get_ground_height_at);
	ClassDB::bind_method(D_METHOD("get_height", "position"), &GroundSurface3D::get_height);
	ClassDB::bind_method(D_METHOD("get_dent_at", "position"), &GroundSurface3D::get_dent_at);
	ClassDB::bind_method(D_METHOD("get_rim_at", "position"), &GroundSurface3D::get_rim_at);
	ClassDB::bind_method(D_METHOD("get_fresh_at", "position"), &GroundSurface3D::get_fresh_at);
	ClassDB::bind_method(D_METHOD("get_material_at", "position"), &GroundSurface3D::get_material_at);
	ClassDB::bind_method(D_METHOD("get_moved_volume"), &GroundSurface3D::get_moved_volume);
	ClassDB::bind_method(D_METHOD("get_deform_rect"), &GroundSurface3D::get_deform_rect);
	ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &GroundSurface3D::get_cached_tile_count);
	ClassDB::bind_method(D_METHOD("get_step_time_msec"), &GroundSurface3D::get_step_time_msec);
	ClassDB::bind_method(D_METHOD("get_patch_material"), &GroundSurface3D::get_patch_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover"), "set_snow_cover", "get_snow_cover");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_GROUP("Material", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_type", PROPERTY_HINT_ENUM, "Sand,Soil"), "set_ground_type", "get_ground_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moisture", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_moisture", "get_moisture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "litter_depth", PROPERTY_HINT_RANGE, "0,0.5,0.005,suffix:m"), "set_litter_depth", "get_litter_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "litter_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_litter_cover", "get_litter_cover");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rock_from_geology"), "set_rock_from_geology", "is_rock_from_geology");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "type_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_type_map", "get_type_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "type_map_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_type_map_rect", "get_type_map_rect");
	ADD_GROUP("Deformation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deform_extent", PROPERTY_HINT_RANGE, "8,256,1,suffix:m"), "set_deform_extent", "get_deform_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deform_resolution", PROPERTY_HINT_RANGE, "128,4096,64"), "set_deform_resolution", "get_deform_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "near_field"), "set_near_field", "is_near_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_extent", PROPERTY_HINT_RANGE, "2,16,1,suffix:m"), "set_near_extent", "get_near_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "near_resolution", PROPERTY_HINT_RANGE, "128,2048,64"), "set_near_resolution", "get_near_resolution");
	ADD_GROUP("Mud", "mud_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mud_layer"), "set_mud_layer", "is_mud_layer");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mud_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_mud_map", "get_mud_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "mud_map_rect"), "set_mud_map_rect", "get_mud_map_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_max_depth", PROPERTY_HINT_RANGE, "0,3,0.05,suffix:m"), "set_mud_max_depth", "get_mud_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_auto_depth", PROPERTY_HINT_RANGE, "0,2,0.05,suffix:m"), "set_mud_auto_depth", "get_mud_auto_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_wet_start", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_mud_wet_start", "get_mud_wet_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_wet_full", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_mud_wet_full", "get_mud_wet_full");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_spacing", PROPERTY_HINT_RANGE, "0.05,4,0.05,suffix:m"), "set_ground_spacing", "get_ground_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "deform_groups"), "set_deform_groups", "get_deform_groups");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ploughing"), "set_ploughing", "is_ploughing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_tolerance", PROPERTY_HINT_RANGE, "0,0.2,0.001,suffix:m"), "set_contact_tolerance", "get_contact_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dust_emission", PROPERTY_HINT_RANGE, "0,0.2,0.001,or_greater,suffix:kg/m2"), "set_dust_emission", "get_dust_emission");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_mass", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:kg"), "set_character_mass", "get_character_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_contact_area", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m²"), "set_character_contact_area", "get_character_contact_area");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cached_tiles", PROPERTY_HINT_RANGE, "0,65536,1"), "set_max_cached_tiles", "get_max_cached_tiles");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_simulation"), "set_gpu_simulation", "is_gpu_simulation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drying_time", PROPERTY_HINT_RANGE, "1,86400,1,suffix:s"), "set_drying_time", "get_drying_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sand_drift_rate", PROPERTY_HINT_RANGE, "0,100,0.1"), "set_sand_drift_rate", "get_sand_drift_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_smoothing", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_rain_smoothing", "get_rain_smoothing");
	ADD_GROUP("Wind", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "wind_override", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind_override", "get_wind_override");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_ripples"), "set_wind_ripples", "is_wind_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_extent", PROPERTY_HINT_RANGE, "1,16,0.25,suffix:m"), "set_ripple_extent", "get_ripple_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_time_scale", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater"), "set_ripple_time_scale", "get_ripple_time_scale");
	ADD_GROUP("Water", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "water_coupling"), "set_water_coupling", "is_water_coupling");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "water_sources", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_water_sources", "get_water_sources");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sand_glints"), "set_sand_glints", "is_sand_glints");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "runoff_channels"), "set_runoff_channels", "is_runoff_channels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "runoff_time_scale", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater"), "set_runoff_time_scale", "get_runoff_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "standing_water"), "set_standing_water", "is_standing_water");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "desiccation_cracks"), "set_desiccation_cracks", "is_desiccation_cracks");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "wind_shelters", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_wind_shelters", "get_wind_shelters");
	ADD_GROUP("Drawing", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_spacing", PROPERTY_HINT_RANGE, "0.01,1,0.005,suffix:m"), "set_detail_spacing", "get_detail_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outer_spacing", PROPERTY_HINT_RANGE, "0.01,4,0.005,suffix:m"), "set_outer_spacing", "get_outer_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:m"), "set_skirt", "get_skirt");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_margin", PROPERTY_HINT_RANGE, "0,32,0.1,suffix:m"), "set_terrain_margin", "get_terrain_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt_fade", PROPERTY_HINT_RANGE, "0.01,32,0.1,suffix:m"), "set_skirt_fade", "get_skirt_fade");

	BIND_ENUM_CONSTANT(GROUND_SAND);
	BIND_ENUM_CONSTANT(GROUND_SOIL);
}

GroundSurface3D::GroundSurface3D() {
	gpu_sink.surface = this;
	gpu_sink.next = &near_sink;
	near_sink.surface = this;
	near_sink.is_near = true;
	field.rim_reach = 1.0f;
	near_fld.rim_reach = 1.0f;
	deform_groups.push_back("ground_deform");
	deform_groups.push_back("snow_deform");
	deform_groups.push_back("grass_trample");
}

GroundSurface3D::~GroundSurface3D() {
	if (active == this) {
		active = nullptr;
	}
}

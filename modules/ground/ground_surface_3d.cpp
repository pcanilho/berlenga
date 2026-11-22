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
#include "ground_clods_3d.h"
#include "ground_debris_flow_3d.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/time.h"
#include "core/math/geometry_2d.h"
#include "core/object/callable_mp.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/math/random_pcg.h"
#include "core/templates/hashfuncs.h"
#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/physics_material.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

// The weather, through its contract (docs/ideas/world-contracts.md): null when nothing provides it, so the ground
// keeps its own settings then.
static const WorldWeather *_weather() {
	const WorldWeather *w = WorldContracts::weather();
	return (w != nullptr && w->has_provider()) ? w : nullptr;
}

static WeatherSeason::Climate _weather_climate(const WorldWeather *p_weather) {
	WeatherSeason::Climate c;
	if (p_weather != nullptr) {
		p_weather->season_climate(c);
	}
	return c;
}

// The climate's aridity index, annual precipitation over Thornthwaite's potential evapotranspiration (UNEP 1992:
// under 0.05 hyperarid, 0.2 arid, 0.5 semiarid); 1e9 without a climate.
static float _aridity_index(const WorldWeather *p_weather, float p_height) {
	WeatherSeason::Climate c;
	if (p_weather == nullptr || !p_weather->season_climate(c)) {
		return 1e9f;
	}
	double heat = 0.0;
	double a = 0.0;
	WeatherSeason::thornthwaite_coefficients(c, p_height, heat, a);
	double pet = 0.0;
	for (int d = 1; d <= 365; d += 4) {
		pet += 4.0 * WeatherSeason::day_pet(c, heat, a, WeatherSeason::day_temperature(c, p_height, float(d)), d);
	}
	return pet > 1.0 ? float(c.precipitation / pet) : 1e9f;
}

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
static constexpr float FORD_RIM_LOSS = 0.7f; // the share of a rut's rims a strong current carries off (round 13.2)

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

// Worn-in tracks (docs/ideas/mud.md, round 14.2): repeated wheel passes pack the soil, most of it in the first few
// (bulk density and cone index climb fast over the first passes and level off: Botta et al. 2006; Tolon-Becerra et al.
// 2012, rut depth and penetration resistance against 1 to 10 tractor passes). The share packed after p_passes.
float GroundSurface3D::wear_compaction(float p_passes) {
	return p_passes > 0.0f ? 1.0f - Math::exp(-p_passes / 4.0f) : 0.0f;
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
	// Runoff cutting (docs/ideas/mud.md, round 11): sand moves past Shields' threshold (0.19 Pa for 0.25 mm grains);
	// cohesive soil only past its own critical shear, 0.5 to 9 Pa for cropland soils with about 3 typical, less wet
	// (Knapen et al. 2007's review; WEPP's rill erodibility tables), so a meadow track sheds rain the sand would not.
	c.crit_shear = Math::lerp(Math::lerp(3.5f, 1.5f, m), 0.19f, s);
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
	if (p_water_speed > 0.0f && s < 0.5f) {
		// A ford's bed (round 13.2): the spoil a wheel throws up in running water is remoulded, past its critical
		// shear (about 0.5 Pa, a sixth of intact soil's; Knapen et al. 2007), and the current takes it before it can
		// settle as a rim: most of it leaves as the plume (GroundSurface3D::_ford() sends it to the water).
		const float tau = 1000.0f * 0.003f * p_water_speed * p_water_speed;
		const float k = Math::smoothstep(0.5f, 3.0f, tau) * (1.0f - 2.0f * s);
		if (k > 0.0f) {
			c.rim_share *= 1.0f - FORD_RIM_LOSS * k;
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
		// The layer on a slope (round 10): an infinite slope of Bingham fluid H deep shears at its base under
		// rho g H sin(theta); past the yield stress the whole layer flows (Johnson 1970; Coussot 1997, the
		// mudflow's stopping depth h = tau_y / (rho g sin theta)). Soaked by rain its solid share falls and
		// its yield stress with it (a hundredfold from stiff to slurry), so mud that held a bank all summer
		// slides off it in a storm, down to the firm base, and piles in the ruts and hollows below.
		// As the layer thins it holds steeper ground: it stops at h = tau_y / (rho g tan theta), so the shader
		// takes the length tau_y / (rho g) and the depth left.
		c.slide_length = mw > 0.5f ? tau_y / (rho * 9.81f) : 1e3f;
		// Waves (round 14.1): slurry to soft mud flows as a shallow Bingham fluid, its plug held by the yield
		// stress and damped by the viscosity (mud_flux() in the shader); stiffer mud only slumps.
		if (mw > 0.5f && stiff < 0.5f) {
			c.wave_tau = tau_y / rho;
			c.wave_nu = mu / rho;
		}
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
		// Mud cuts easily: a slurry near its yield stress, stiff clay about as the soil (round 11).
		c.crit_shear = Math::lerp(c.crit_shear, Math::lerp(0.3f, 2.0f, stiff), mw);
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
		c.crit_shear *= 1.0f + 4.0f * lw; // litter armours the surface (round 11)
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
	// A hidden stone's top is a floor for slumping and flow too (round 16.1).
	if (surface->buried_obstacles && !surface->buried_bodies.is_empty()) {
		col.max_dent = MIN(col.max_dent, surface->_buried_depth(Vector3(p_x, 0.0f, p_z), 0.0f));
	}
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
	// The deposits the kept cells carry move with them; new cells take theirs below (_apply_deposits).
	if (keep && (int)ground_dep.size() == p_n * p_n) {
		_shift_lattice(ground_dep, ground_origin, p_origin, p_n, 1);
	} else {
		ground_dep.resize(p_n * p_n);
		for (float &v : ground_dep) {
			v = 0.0f;
		}
	}
	if (!keep) {
		for (float &v : ground_dep) {
			v = 0.0f;
		}
	}
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
	_cracks_move(p_origin, p_n); // the crack state moves with it, kept cells read back (new ones: never soaked, 0)
	if (ground_valid && ground_n == p_n && (int)ice_state.size() == p_n * p_n * ICE_FLOATS) {
		_shift_lattice(ice_state, ground_origin, p_origin, p_n, ICE_FLOATS);
	} else {
		ice_state.clear();
	}
	if (ground_valid && ground_n == p_n && (int)frost_state.size() == p_n * p_n * FROST_FLOATS) {
		// Cells the lattice comes onto start from the climate's frost (marked -1 for _frost_update()).
		const Vector2i fd = p_origin - ground_origin;
		_shift_lattice(frost_state, ground_origin, p_origin, p_n, FROST_FLOATS);
		for (int z = 0; z < p_n; z++) {
			for (int x = 0; x < p_n; x++) {
				const int sx = x + fd.x;
				const int sz = z + fd.y;
				if (!(sx >= 0 && sz >= 0 && sx < p_n && sz < p_n)) {
					frost_state[(z * p_n + x) * FROST_FLOATS] = -1.0f;
				}
			}
		}
	} else {
		frost_state.clear();
	}
	if (ground_valid && ground_n == p_n && (int)plant_mud_state.size() == p_n * p_n) {
		_shift_lattice(plant_mud_state, ground_origin, p_origin, p_n, 1);
	} else {
		plant_mud_state.clear();
	}
	ground_n = p_n;
	ground_origin = p_origin;
	ground_valid = true;
	_apply_deposits();
	_ground_changed();
	_update_cells();
}

void GroundSurface3D::_ground_changed() {
	const int p_n = ground_n;
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
}

// The mud deposits, a cubic B-spline over their 1 m cells' centres: smooth in slope as well as height, so a fan's
// surface carries no creases along the cells for the field's slumping to work into terraces.
float GroundSurface3D::_deposit_smooth_at(float p_x, float p_z, float *r_coloured) const {
	if (r_coloured) {
		*r_coloured = 0.0f;
	}
	if (mud_deposits.is_empty()) {
		return 0.0f;
	}
	const float fx = p_x - 0.5f;
	const float fz = p_z - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - float(x0);
	const float tz = fz - float(z0);
	auto weights = [](float t, float *w) {
		const float it = 1.0f - t;
		w[0] = it * it * it / 6.0f;
		w[1] = (3.0f * t * t * t - 6.0f * t * t + 4.0f) / 6.0f;
		w[2] = (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) / 6.0f;
		w[3] = t * t * t / 6.0f;
	};
	float wx[4];
	float wz[4];
	weights(tx, wx);
	weights(tz, wz);
	float sum = 0.0f;
	for (int j = 0; j < 4; j++) {
		for (int k = 0; k < 4; k++) {
			const Vector2i key(x0 - 1 + k, z0 - 1 + j);
			const float *v = mud_deposits.getptr(key);
			if (v) {
				sum += wx[k] * wz[j] * *v;
				if (r_coloured && mud_deposit_colors.has(key)) {
					*r_coloured += wx[k] * wz[j] * *v;
				}
			}
		}
	}
	return sum;
}

// Brings the lattice to the deposits: each cell rises by what was laid since it last looked. True if any moved.
bool GroundSurface3D::_apply_deposits() {
	const int n = ground_n;
	if (!ground_valid || n <= 0 || (int)ground_dep.size() != n * n || (int)ground.size() != n * n) {
		return false;
	}
	bool moved = false;
	// A deposit with a colour is one its depositor draws itself beyond the print field (a debris flow's sheet,
	// GroundDebrisFlow3D): the ground takes it over inside the field only, fading in over the field's 1 to 4 m from
	// its edge, where the sheet still draws over it, so the two never stand side by side as a step. Uncoloured
	// deposits (flood silt) are the ground's alone and raise it everywhere.
	const Rect2 field_rect = get_deform_rect();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float wx = (ground_origin.x + x) * ground_spacing;
			const float wz = (ground_origin.y + z) * ground_spacing;
			float drawn = 0.0f;
			const float all = _deposit_smooth_at(wx, wz, &drawn);
			float inside = 0.0f;
			if (field_rect.size.x > 0.0f) {
				const float d = MIN(MIN(wx - field_rect.position.x, field_rect.get_end().x - wx), MIN(wz - field_rect.position.y, field_rect.get_end().y - wz));
				inside = Math::smoothstep(1.0f, 4.0f, d);
			}
			const float want = all - drawn * (1.0f - inside);
			if (Math::abs(want - ground_dep[i]) > 1e-4f) {
				ground[i] += want - ground_dep[i];
				ground_dep[i] = want;
				moved = true;
			}
		}
	}
	return moved;
}

void GroundSurface3D::refresh_deposits() {
	_update_cells();
}

void GroundSurface3D::_update_cells() {
	const int n = ground_n;
	if (!ground_valid || n <= 0) {
		return;
	}
	// Deposits laid since (debris fans, flood silt) become the ground the field prints.
	if (_apply_deposits()) {
		_ground_changed();
	}
	_trails_refresh();
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
	const WorldWeather *wm = _weather();
	const WorldFire *fire = WorldContracts::fire();
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
			const float m = moisture >= 0.0f ? moisture : (wm ? wm->surface_wetness_at(p) : 0.0f);
			const float closure = wm ? wm->canopy_closure_at(p) : litter_cover;
			CellInput &in = cell_inputs[i];
			in.sand = sand;
			in.moisture = m;
			in.litter = litter_depth * CLAMP(closure, 0.0f, 1.0f);
			// A fire burnt the litter off and left the soil water-repellent (WorldFire).
			in.repel = 0.0f;
			if (fire && fire->has_provider()) {
				in.litter *= 1.0f - CLAMP(fire->burnt_at(p), 0.0f, 1.0f);
				in.repel = CLAMP(fire->water_repellency_at(p), 0.0f, 1.0f);
			}
			in.compacted = worn_tracks && _wear_any() ? wear_compaction(get_wear_at(p)) * (1.0f - 0.5f * sand) : 0.0f;
			// Under more than a few centimetres of snow the snow surface takes the print.
			in.fixed = rock || (snow && float(snow->call("get_depth_at", p)) > 0.03f);
			// Frozen ground (round 8): a crust holds; a thaw's wet layer over the frost, or a fresh thaw not yet
			// drained, is soaked and soft, a slurry as deep as the thawed layer.
			in.frozen = in.fixed ? 0.0f : _frost_crust(i);
			float thaw_top = 0.0f;
			if (!in.fixed && (int)frost_state.size() == n * n * FROST_FLOATS) {
				const float *fr = &frost_state[i * FROST_FLOATS];
				in.moisture = MAX(in.moisture, Math::lerp(in.moisture, mud_wet_full + 0.05f, fr[2]));
				thaw_top = fr[0] > 0.0f && fr[1] > fr[0] ? fr[0] : 0.0f;
			}
			const bool wet = water_coupling && (int)pore.size() == n * n && (pore[i] > 0.0f || water_speed[i] > 0.0f);
			in.mud = in.fixed ? 0.0f : _mud_depth_at(p, wet ? MAX(in.moisture, pore[i]) : in.moisture, sand > 0.5f, dmap);
			if (mud_layer && sand < 0.5f && thaw_top > 0.0f) {
				in.mud = MAX(in.mud, thaw_top);
			}
			// A debris flow's fresh deposit is saturated slurry, however dry the weather (round 13).
			const float silt = in.fixed ? 0.0f : _silt_at(p.x, p.z);
			const bool fan = !in.fixed && !mud_deposits.is_empty() && get_mud_deposit_at(p) - silt > 1e-4f;
			if (fan) {
				in.moisture = MAX(in.moisture, mud_wet_full + 0.05f);
			} else if (silt > 0.0f) {
				// A silt skin (round 15.1) is as wet as the water left in it, and buries the litter.
				in.moisture = MAX(in.moisture, Math::lerp(in.moisture, mud_wet_full + 0.05f, _silt_water(p.x, p.z)));
				in.litter *= 1.0f - Math::smoothstep(0.002f, 0.01f, silt);
			}
			Cell c = wet ? material_cell(in.sand, in.moisture, in.litter, pore[i], water_speed[i], in.mud) : material_cell(in.sand, in.moisture, in.litter, -1.0f, 0.0f, in.mud);
			c.max_dent *= _subsoil_factor(p.x, p.z);
			// It came to rest as a body of grains: once its pore pressure drops it holds its snout and levees at
			// their friction angle, some 30 to 35 degrees (Iverson 1997, Major and Iverson 1999), and does not slide
			// on as a weak slurry would.
			if (fan) {
				c.slide_length = 1e3f;
				c.tan_repose = MAX(c.tan_repose, 0.6f);
				c.wave_nu = 0.0f; // nor does it run as a wave (round 14.1)
			}
			if (wet) {
				cell_water[i * 2 + 0] = pore[i];
				cell_water[i * 2 + 1] = water_speed[i];
			}
			c.deformable = !in.fixed && in.frozen < 0.95f;
			if (!in.fixed && (int)frost_state.size() == n * n * FROST_FLOATS) {
				// Needle ice and the crumbs its thaw leaves (round 9): the needles bear almost nothing, the crumbs
				// little (a bulk density well below the soil's), so either prints deep and easily.
				const float *fr = &frost_state[i * FROST_FLOATS];
				const float soft = MAX(Math::smoothstep(0.0f, 0.01f, fr[3]), fr[4]);
				if (soft > 0.0f) {
					const float k = 1.0f - 0.6f * soft;
					c.kc *= k;
					c.kphi *= k;
					c.wheel_kc *= k;
					c.wheel_kphi *= k;
					c.cohesion *= 1.0f - 0.6f * soft;
				}
			}
			if (in.frozen > 0.0f) {
				// Frozen soil: ice-bonded, far stiffer and stronger (frozen silt's cohesion is hundreds of kPa), no
				// slumping; the ruts already in it stay as they are.
				const float k = 1.0f + 30.0f * in.frozen;
				c.kc *= k;
				c.kphi *= k;
				c.wheel_kc *= k;
				c.wheel_kphi *= k;
				c.cohesion += 200.0f * in.frozen;
				c.max_dent *= 1.0f - in.frozen;
				c.rate *= 1.0f - in.frozen;
			}
			// Worn-in tracks (round 14.2): the packed soil bears more and gives less (Wong 2008, repetitive loading:
			// the soil reloaded after a pass is far stiffer than the virgin soil). Soft mud is not packed, it is
			// only churned (the ruts keep the passes there).
			const float pack = in.compacted * (1.0f - Math::smoothstep(0.02f, 0.1f, c.mud)) * (c.wave_nu > 0.0f ? 0.0f : 1.0f);
			if (pack > 0.0f) {
				const float k = 1.0f + 2.0f * pack;
				c.kc *= k;
				c.kphi *= k;
				c.wheel_kc *= k;
				c.wheel_kphi *= k;
				c.cohesion *= 1.0f + 0.5f * pack;
				c.max_dent *= 1.0f - 0.5f * pack;
			}
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
	// Only wet mud is soft: a dry track bears like soil (MudRunner's hidden mud, "Extrudes To Wetness"). A debris
	// flow's fresh deposit is saturated slurry whatever the weather.
	// A silt skin (round 15.1) is soft only while wet.
	const float dep = get_mud_deposit_at(p_position);
	const float silt = MIN(_silt_at(p_position.x, p_position.z), dep);
	const float wet = Math::smoothstep(mud_wet_start, mud_wet_full, p_moisture);
	// The skin holds its own water whatever the weather's (_silt_water()).
	const float silt_wet = silt > 0.0f ? MAX(wet, _silt_water(p_position.x, p_position.z)) : wet;
	return MAX(depth * wet, dep - silt + silt * silt_wet);
}

void GroundSurface3D::add_mud_deposit(Vector3 p_position, float p_radius, float p_depth, bool p_update, Color p_color) {
	if (p_depth <= 0.0f) {
		return;
	}
	const int r = MAX(int(Math::ceil(p_radius)), 0);
	const Vector2i c(int(Math::floor(p_position.x)), int(Math::floor(p_position.z)));
	for (int z = -r; z <= r; z++) {
		for (int x = -r; x <= r; x++) {
			const Vector2 d(float(c.x + x) + 0.5f - p_position.x, float(c.y + z) + 0.5f - p_position.z);
			if (d.length() > MAX(p_radius, 0.5f)) {
				continue;
			}
			const Vector2i k(c.x + x, c.y + z);
			float *v = mud_deposits.getptr(k);
			if (v) {
				*v = MAX(*v, p_depth);
			} else {
				mud_deposits.insert(k, p_depth);
			}
			// The newest layer's colour shows on top.
			if (p_color.a > 0.0f) {
				mud_deposit_colors[k] = p_color;
			}
		}
	}
	if (p_update) {
		_update_cells();
	}
}

// Overbank deposition (Pizzuto 1987; Asselman and Middelkoop 1995): water standing or moving slowly over the ground
// drops its suspended load at the silt's settling speed, D = C w_s dt (kg/m2), w_s about 3e-4 m/s for 20 um silt
// (Stokes), none where the current keeps it up (over about 0.5 m/s, fading from 0.2). It lies as fresh, saturated
// mud (dry bulk density about 1300 kg/m3), so a flood's few millimetres to centimetres near the channel thin out
// away from it as the load settles; mineral and ash alike (the ash darkens the water and the mud it leaves).
void GroundSurface3D::_flood_silt(const Vector3 &p_position, float p_depth, float p_speed, const Vector2 &p_load, float p_dt) {
	if (!flood_silt || p_depth < 0.01f || p_dt <= 0.0f) {
		return;
	}
	const float c = MAX(p_load.x, 0.0f) + MAX(p_load.y, 0.0f); // kg/m3
	if (c <= 1e-5f) {
		return;
	}
	const float still = 1.0f - Math::smoothstep(0.2f, 0.5f, p_speed);
	// What settles cannot be more than the column holds.
	const float kg = MIN(c * 3.0e-4f * p_dt, c * p_depth) * still;
	if (kg <= 0.0f) {
		return;
	}
	const Vector2i k(int(Math::floor(p_position.x)), int(Math::floor(p_position.z)));
	// Mineral and ash in proportion to the load.
	_lay_silt(k, Vector2(MAX(p_load.x, 0.0f), MAX(p_load.y, 0.0f)) * (kg / c));
}

// Silt skins (docs/ideas/mud.md, round 15.1). What settles out of slack water (a flood's backwater, a river's slack
// lanes, Krone 1962) lies as a skin of fine mud, a few millimetres to centimetres: fresh, it is saturated and slick
// (fine earth on a hard surface takes tyre friction down to about half, SAE 2007-01-0736; TRL PPR073); as the water
// goes it dries from the top, pales and cracks into plates in proportion to its thickness (Groisman and Kaplan 1994;
// _crack_update()), and once dry it bears as the soil under it. Dry bulk density about 1300 kg/m3 (Asselman and
// Middelkoop 1995). Its colour: a mineral silt's grey-brown, darkened toward black by the ash in it.
void GroundSurface3D::_lay_silt(const Vector2i &p_cell, const Vector2 &p_kg) {
	const float kg = p_kg.x + p_kg.y;
	if (kg <= 0.0f) {
		return;
	}
	const float dm = kg / 1300.0f;
	float *v = mud_deposits.getptr(p_cell);
	if (v) {
		*v += dm;
	} else {
		mud_deposits.insert(p_cell, dm);
	}
	float &s = silt_depth[p_cell];
	const float before = s;
	s += dm;
	// The skin's colour, mixed by mass with what lay there.
	const float ash = p_kg.y / kg;
	// Dry, the crust's pale tone (material.cpp lifts the crack state's tone by about 1.9) makes it a light
	// grey-brown; soaked, the wet ground's darkening takes it dark.
	const Color fresh = Color(0.42f, 0.37f, 0.30f).lerp(Color(0.12f, 0.12f, 0.12f), ash).srgb_to_linear();
	Color *col = mud_deposit_colors.getptr(p_cell);
	if (col && before > 0.0f) {
		const float w = dm / s;
		*col = Color(Math::lerp(col->r, fresh.r, w), Math::lerp(col->g, fresh.g, w), Math::lerp(col->b, fresh.b, w), 1.0f);
	} else if (!col) {
		mud_deposit_colors[p_cell] = Color(fresh.r, fresh.g, fresh.b, 1.0f);
	}
	silt_wet_new.insert(p_cell);
	silt_total += kg;
	silt_pending += dm;
	if (silt_pending > 0.002f) {
		silt_pending = 0.0f;
		silt_dirty = true;
	}
}

float GroundSurface3D::_silt_at(float p_x, float p_z) const {
	if (silt_depth.is_empty()) {
		return 0.0f;
	}
	const float *s = silt_depth.getptr(Vector2i(int(Math::floor(p_x)), int(Math::floor(p_z))));
	return s ? *s : 0.0f;
}

float GroundSurface3D::_silt_smooth_at(float p_x, float p_z) const {
	if (silt_depth.is_empty()) {
		return 0.0f;
	}
	const float fx = p_x - 0.5f;
	const float fz = p_z - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - float(x0);
	const float tz = fz - float(z0);
	auto at = [&](int x, int z) {
		const float *s = silt_depth.getptr(Vector2i(x, z));
		return s ? *s : 0.0f;
	};
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

// The water left in the skin, 0 (dry) to 1 (soaked): the drying layer's (crack_state's) at the lattice point, soaked
// where silt was laid since.
float GroundSurface3D::_silt_water(float p_x, float p_z) const {
	const Vector2i k(int(Math::floor(p_x)), int(Math::floor(p_z)));
	if (silt_wet_new.has(k)) {
		return 1.0f;
	}
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)crack_state.size() != n * n * CRACK_FLOATS) {
		return 1.0f;
	}
	const int x = CLAMP(int(Math::round(p_x / ground_spacing - ground_origin.x)), 0, n - 1);
	const int z = CLAMP(int(Math::round(p_z / ground_spacing - ground_origin.y)), 0, n - 1);
	const float st = crack_state[(z * n + x) * CRACK_FLOATS];
	return st <= 0.0f ? 0.0f : CLAMP(st - 1.0f, 0.0f, 1.0f); // never soaked: a skin's margin past its cells
}

// Forgets every deposit (fans, silt skins, the rivers' records taken), as for a new game; kept files stay until
// clear_saved_ruts().
void GroundSurface3D::clear_mud_deposits() {
	mud_deposits.clear();
	mud_deposit_colors.clear();
	silt_depth.clear();
	silt_river_taken.clear();
	silt_wet_new.clear();
	silt_dirty = false;
	buried_extra.clear();
	buried_cache.clear();
	silt_broken.clear();
	silt_wind.clear();
	gpu_cells_dirty = true;
	near_cells_dirty = true;
	if (ground_valid) {
		_update_cells();
	}
}

Vector2 GroundSurface3D::get_silt_at(Vector3 p_position) const {
	const float s = _silt_at(p_position.x, p_position.z);
	return s > 0.0f ? Vector2(s, _silt_water(p_position.x, p_position.z)) : Vector2();
}

// The coloured deposits' colour at a point, weighted as _deposit_smooth_at() weights their depths (cubic B-spline
// over 4 x 4 cells), so the colour's edge follows the smooth ground and not the 1 m cells: rgb the colour, a the
// coloured share of the deposit there.
Color GroundSurface3D::_deposit_color_at(float p_x, float p_z) const {
	if (mud_deposit_colors.is_empty()) {
		return Color(0, 0, 0, 0);
	}
	const float fx = p_x - 0.5f;
	const float fz = p_z - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - float(x0);
	const float tz = fz - float(z0);
	auto weights = [](float t, float *w) {
		const float it = 1.0f - t;
		w[0] = it * it * it / 6.0f;
		w[1] = (3.0f * t * t * t - 6.0f * t * t + 4.0f) / 6.0f;
		w[2] = (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) / 6.0f;
		w[3] = t * t * t / 6.0f;
	};
	float wx[4];
	float wz[4];
	weights(tx, wx);
	weights(tz, wz);
	Color sum(0, 0, 0, 0);
	float all = 0.0f;
	for (int j = 0; j < 4; j++) {
		for (int k = 0; k < 4; k++) {
			const Vector2i key(x0 - 1 + k, z0 - 1 + j);
			if (!mud_deposits.has(key)) {
				continue;
			}
			const float w = wx[k] * wz[j];
			all += w;
			if (const Color *c = mud_deposit_colors.getptr(key)) {
				const float wa = w * c->a;
				sum += Color(c->r * wa, c->g * wa, c->b * wa, wa);
			}
		}
	}
	if (sum.a <= 1e-6f || all <= 1e-6f) {
		return Color(0, 0, 0, 0);
	}
	return Color(sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, MIN(sum.a / all, 1.0f));
}

float GroundSurface3D::get_mud_deposit_at(Vector3 p_position) const {
	if (mud_deposits.is_empty()) {
		return 0.0f;
	}
	const float *v = mud_deposits.getptr(Vector2i(int(Math::floor(p_position.x)), int(Math::floor(p_position.z))));
	return v ? *v : 0.0f;
}

// The sand reads of the ground contract (WorldGround): loose sand's share of the top, and how much of it the wind
// moves there now (dry sand, out of shelter).
float GroundSurface3D::contract_ground_sand_at(const Vector3 &p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	return c.deformable ? c.sand : 0.0f;
}

float GroundSurface3D::contract_ground_blown_sand_at(const Vector3 &p_position) const {
	return get_blown_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_mud_depth_at(Vector3 p_position) const {
	return _cell_at(p_position.x, p_position.z).mud;
}

void GroundSurface3D::add_wader(int64_t p_id, const Vector3 &p_axle, float p_radius, float p_half_width, const Vector3 &p_velocity) {
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	for (int64_t k = int64_t(waders.size()) - 1; k >= 0; k--) {
		if (waders[k].frame + 1 < frame || waders[k].id == p_id) {
			waders.remove_at_unordered(k);
		}
	}
	Wader w;
	w.id = p_id;
	w.axle = p_axle;
	w.radius = p_radius;
	w.half_width = p_half_width;
	w.velocity = p_velocity;
	w.frame = frame;
	waders.push_back(w);
}

void GroundSurface3D::_water_owed_update() {
	GroundDeformGPUHost *hosts[2] = { &gpu_host, &near_host };
	for (int k = 0; k < 2; k++) {
		if (hosts[k]->gpu.is_null()) {
			continue;
		}
		const uint32_t now = hosts[k]->gpu->get_water_owed_mm3();
		// Unsigned difference (the ledger wraps at 4 m3); one that went back is a new ledger (the field reset).
		const uint32_t seen = water_owed_seen[k];
		const uint32_t delta = now >= seen || seen - now > 0x80000000u ? uint32_t(now - seen) : now;
		water_owed += double(delta) * 1e-9;
		water_owed_seen[k] = now;
	}
}

void GroundSurface3D::add_water(const Vector3 &p_position, float p_radius, float p_volume, float p_mud) {
	if (p_radius <= 0.0f || p_volume == 0.0f || !Math::is_finite(p_volume)) {
		return;
	}
	if (p_volume > 0.0f && p_mud > 0.0f) {
		// Muddy water splashed out (a wheel through a puddle): its clay spatters the plants it lands on, in full by
		// 50 g/m2.
		_plant_mud_add(p_position, p_radius, CLAMP(p_mud * p_volume / (float(Math::PI) * p_radius * p_radius) / 0.05f, 0.0f, 1.0f));
	}
	if (p_volume > 0.0f) {
		// Water a take asked for but did not find (the field had less than its read back showed) was thrown all
		// the same: this water pays it back first, so the volume is kept.
		_water_owed_update();
		const double pay = MIN(double(p_volume), water_owed);
		water_owed -= pay;
		p_volume -= float(pay);
		if (p_volume <= 0.0f) {
			return;
		}
	}
	// Discs close together merge (a spray lands as many drops): the same texel area holds them.
	for (WaterDisc &d : water_discs) {
		if (d.radius == p_radius && (d.volume < 0.0) == (p_volume < 0.0f) && Vector2(d.position.x - p_position.x, d.position.z - p_position.z).length() < 0.5f * p_radius) {
			if (p_volume > 0.0f) {
				d.mud = float((d.mud * d.volume + double(MAX(p_mud, 0.0f)) * p_volume) / (d.volume + p_volume));
			}
			d.volume += p_volume;
			return;
		}
	}
	if ((int)water_discs.size() >= 4 * WATER_DISCS) {
		// More than a frame can take: the nearest of the same sign takes it.
		int best = -1;
		float bd = 1e30f;
		for (int k = 0; k < (int)water_discs.size(); k++) {
			const float dd = water_discs[k].position.distance_squared_to(p_position);
			if ((water_discs[k].volume < 0.0) == (p_volume < 0.0f) && dd < bd) {
				bd = dd;
				best = k;
			}
		}
		if (best >= 0) {
			WaterDisc &b = water_discs[best];
			if (p_volume > 0.0f) {
				b.mud = float((b.mud * b.volume + double(MAX(p_mud, 0.0f)) * p_volume) / (b.volume + p_volume));
			}
			b.volume += p_volume;
		}
		return;
	}
	WaterDisc d;
	d.position = p_position;
	d.radius = p_radius;
	d.volume = p_volume;
	d.mud = MAX(p_mud, 0.0f);
	water_discs.push_back(d);
}

void GroundSurface3D::stir_water(const Vector3 &p_position, float p_radius, float p_mass) {
	if (!muddy_water || p_radius <= 0.0f || !(p_mass > 0.0f)) {
		return;
	}
	for (StirDisc &d : stir_discs) {
		if (Vector2(d.position.x - p_position.x, d.position.z - p_position.z).length() < 0.5f * MIN(p_radius, d.radius)) {
			d.mass += p_mass;
			return;
		}
	}
	if (stir_discs.size() >= 32) {
		return;
	}
	StirDisc d;
	d.position = p_position;
	d.radius = p_radius;
	d.mass = p_mass;
	stir_discs.push_back(d);
}

float GroundSurface3D::get_water_mud_at(Vector3 p_position) const {
	if (is_near_active() && _near_inside(p_position.x, p_position.z) && int(near_fld.water_mud.size()) == near_fld.resolution * near_fld.resolution) {
		return near_fld.sample(near_fld.water_mud, p_position.x, p_position.z);
	}
	if (field.valid && int(field.water_mud.size()) == field.resolution * field.resolution) {
		return field.sample(field.water_mud, p_position.x, p_position.z);
	}
	return 0.0f;
}

float GroundSurface3D::get_regrown_at(Vector3 p_position) const {
	if (is_near_active() && _near_inside(p_position.x, p_position.z) && int(near_fld.regrown.size()) == near_fld.resolution * near_fld.resolution) {
		return near_fld.sample(near_fld.regrown, p_position.x, p_position.z);
	}
	if (field.valid && int(field.regrown.size()) == field.resolution * field.resolution) {
		return field.sample(field.regrown, p_position.x, p_position.z);
	}
	return 0.0f;
}

float GroundSurface3D::get_standing_water_volume(const Vector3 &p_position, float p_radius) const {
	const bool near = is_near_active() && _near_inside(p_position.x, p_position.z);
	const GroundDeformField &f = near ? near_fld : field;
	const int n = f.resolution;
	if (!f.valid || int(f.water.size()) != n * n || p_radius <= 0.0f) {
		return 0.0f;
	}
	const float t = f.texel();
	const int x0 = MAX((int)Math::floor((p_position.x - p_radius) / t - 0.5f) - f.origin.x, 0);
	const int x1 = MIN((int)Math::ceil((p_position.x + p_radius) / t - 0.5f) - f.origin.x, n - 1);
	const int z0 = MAX((int)Math::floor((p_position.z - p_radius) / t - 0.5f) - f.origin.y, 0);
	const int z1 = MIN((int)Math::ceil((p_position.z + p_radius) / t - 0.5f) - f.origin.y, n - 1);
	double v = 0.0;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float wx = (f.origin.x + x + 0.5f) * t - p_position.x;
			const float wz = (f.origin.y + z + 0.5f) * t - p_position.z;
			if (wx * wx + wz * wz <= p_radius * p_radius) {
				v += f.water[z * n + x];
			}
		}
	}
	return float(v * t * t);
}

int GroundSurface3D::_disc_texels(const GroundDeformField &p_field, const Vector3 &p_position, float p_radius) {
	const float t = p_field.texel();
	const int x0 = (int)Math::floor((p_position.x - p_radius) / t - 0.5f) - p_field.origin.x;
	const int x1 = (int)Math::ceil((p_position.x + p_radius) / t - 0.5f) - p_field.origin.x;
	const int z0 = (int)Math::floor((p_position.z - p_radius) / t - 0.5f) - p_field.origin.y;
	const int z1 = (int)Math::ceil((p_position.z + p_radius) / t - 0.5f) - p_field.origin.y;
	const int n = p_field.resolution;
	int count = 0;
	for (int z = MAX(z0, 0); z <= MIN(z1, n - 1); z++) {
		for (int x = MAX(x0, 0); x <= MIN(x1, n - 1); x++) {
			const float wx = (p_field.origin.x + x + 0.5f) * t - p_position.x;
			const float wz = (p_field.origin.y + z + 0.5f) * t - p_position.z;
			if (wx * wx + wz * wz <= p_radius * p_radius) {
				count++;
			}
		}
	}
	return count;
}

void GroundSurface3D::set_mud_color(const Color &p_color) {
	mud_color = p_color;
}

void GroundSurface3D::set_mud_color_map(const Ref<Image> &p_map) {
	mud_color_map = p_map;
}

Color GroundSurface3D::get_mud_color_at(Vector3 p_position) const {
	if (mud_color_map.is_null() || mud_color_map->is_empty() || mud_map_rect.size.x <= 0.0f || mud_map_rect.size.y <= 0.0f) {
		return mud_color;
	}
	const Vector2 uv = (Vector2(p_position.x, p_position.z) - mud_map_rect.position) / mud_map_rect.size;
	if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
		return mud_color;
	}
	const int x = CLAMP((int)(uv.x * mud_color_map->get_width()), 0, mud_color_map->get_width() - 1);
	const int y = CLAMP((int)(uv.y * mud_color_map->get_height()), 0, mud_color_map->get_height() - 1);
	Color col = mud_color_map->get_pixel(x, y);
	col.a = 1.0f;
	return col;
}

void GroundSurface3D::set_mud_layer(bool p_enabled) {
	buried_cache.clear(); // the hidden stones lie in the layer (round 14.3)
	_buried_clear_bodies();
	mud_layer = p_enabled;
	_update_cells();
}

// ---------------------------------------------------------------------------------------------------------
// Worn-in tracks (docs/ideas/mud.md, round 14.2). Each wheel's press counts a pass on the 25 cm cells its print
// covers, once while the wheel stays on them; a cell left alone for a quarter of a second takes the next press as a
// new pass (the rear wheel after the front, the next vehicle). The passes pack the soil (wear_compaction(), the cells'
// Bekker values and infiltration) and wear the sward away (GrassField3D reads ground_wear_map: Cole 1995's
// cover against passes); they halve over wear_regrowth_days of game time as the grass regrows and frost and roots
// loosen the soil.

void GroundSurface3D::_wear_press(const Vector3 &p_position, const Vector2 &p_half, float p_yaw) {
	const float r = p_half.length();
	const Vector2 ax(Math::cos(p_yaw), -Math::sin(p_yaw));
	const Vector2 az(Math::sin(p_yaw), Math::cos(p_yaw));
	const int x0 = int(Math::floor((p_position.x - r) / WEAR_CELL));
	const int x1 = int(Math::floor((p_position.x + r) / WEAR_CELL));
	const int z0 = int(Math::floor((p_position.z - r) / WEAR_CELL));
	const int z1 = int(Math::floor((p_position.z + r) / WEAR_CELL));
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const Vector2 d((x + 0.5f) * WEAR_CELL - p_position.x, (z + 0.5f) * WEAR_CELL - p_position.z);
			// The cell's centre within the print, grown by half a cell so a narrow tyre still marks the cells it crosses.
			if (Math::abs(d.dot(ax)) > p_half.x + 0.5f * WEAR_CELL || Math::abs(d.dot(az)) > p_half.y + 0.5f * WEAR_CELL) {
				continue;
			}
			Wear &w = wear_cells[Vector2i(x, z)];
			if (wear_clock - w.last > WEAR_GAP) {
				w.passes += 1.0f;
				wear_dirty = true;
			}
			w.last = wear_clock;
		}
	}
}

void GroundSurface3D::add_wear(Vector3 p_position, Vector2 p_size, float p_yaw, float p_passes) {
	if (p_passes == 0.0f) {
		return;
	}
	const Vector2 half = p_size * 0.5f;
	const float r = half.length();
	const Vector2 ax(Math::cos(p_yaw), -Math::sin(p_yaw));
	const Vector2 az(Math::sin(p_yaw), Math::cos(p_yaw));
	for (int z = int(Math::floor((p_position.z - r) / WEAR_CELL)); z <= int(Math::floor((p_position.z + r) / WEAR_CELL)); z++) {
		for (int x = int(Math::floor((p_position.x - r) / WEAR_CELL)); x <= int(Math::floor((p_position.x + r) / WEAR_CELL)); x++) {
			const Vector2 d((x + 0.5f) * WEAR_CELL - p_position.x, (z + 0.5f) * WEAR_CELL - p_position.z);
			if (Math::abs(d.dot(ax)) > half.x + 0.5f * WEAR_CELL || Math::abs(d.dot(az)) > half.y + 0.5f * WEAR_CELL) {
				continue;
			}
			Wear &w = wear_cells[Vector2i(x, z)];
			w.passes = MAX(w.passes + p_passes, 0.0f);
		}
	}
	wear_dirty = true;
	_update_cells();
}

void GroundSurface3D::clear_wear() {
	wear_cells.clear();
	wear_dirty = true;
	_update_cells();
}

// Game trails (docs/ideas/mud.md, round 20): the stand contract's worn paths (WorldStand.trail_at(), passes a year
// and the tread's share) wear the ground as the wheels' passes do (Cole 1995's trampling counts feet, the passes
// here). A trail walked P times a year against the wear's regrowth half-life T days holds a steady P T / (365 ln 2)
// passes over its tread, so it is packed (slower to drain: puddles stand on it after rain) and bare, and its mud is
// the trampled mud the wheels' wear gives.
void GroundSurface3D::_trails_refresh() {
	WorldStand *st = WorldContracts::stand();
	const bool on = worn_tracks && st && (st->get_script_instance() != nullptr || !st->get_trails().is_empty());
	if (st && st->get_instance_id() != trail_stand) {
		if (Object *old = ObjectDB::get_instance(trail_stand)) {
			if (old->is_connected(SNAME("stand_changed"), callable_mp(this, &GroundSurface3D::_on_stand_changed))) {
				old->disconnect(SNAME("stand_changed"), callable_mp(this, &GroundSurface3D::_on_stand_changed));
			}
		}
		trail_stand = st->get_instance_id();
		st->connect(SNAME("stand_changed"), callable_mp(this, &GroundSurface3D::_on_stand_changed));
	}
	if (on != trails_on) {
		trails_on = on;
		wear_dirty = true;
	}
}

void GroundSurface3D::_on_stand_changed(const Rect2 &p_area) {
	_trails_refresh();
	wear_dirty = true;
	_update_cells();
}

// Pugged trails (docs/ideas/mud.md, round 21.2). Hooves on soil wetter than its plastic limit punch in and leave a
// pocked, puddling tread (treading damage, "pugging": Climo and Richardson 1984; Drewry 2006 on its slow recovery).
// Over the trails near the followed point (WorldStand.trail_at(), sampled every 0.5 m), a trail walked P times a
// year gets P / 365 passes a day of game time, each about 11 hoof prints a square metre of tread (a deer's stride of
// about 0.6 m, two prints a stride over a 0.3 m wide line), drawn at random; each print a 6 x 8 cm hoof at 150 kPa
// (a red deer's; Bekker's plate sinkage in the soil there), only where the soil is wet past 0.55 of saturation or
// muddy. At most 64 prints a step; the pocks then slump, fill with rain and grow over as any print.
void GroundSurface3D::_trail_pug_update(float p_delta) {
	if (!trail_pugging || !trails_on || !field.valid || !ground_valid || (int)cell_inputs.size() != ground_n * ground_n) {
		return;
	}
	const float rate = 11.0f / (365.0f * 86400.0f) * MAX(runoff_time_scale, 0.0f) * p_delta; // prints per m2 per pass a year
	if (rate <= 0.0f) {
		return;
	}
	WorldStand *st = WorldContracts::stand();
	const Vector3 c = _follow_position();
	const float range = 15.0f;
	const float step = 0.5f;
	const int n = ground_n;
	int printed = 0;
	for (float z = Math::floor((c.z - range) / step) * step; z < c.z + range && printed < 64; z += step) {
		for (float x = Math::floor((c.x - range) / step) * step; x < c.x + range && printed < 64; x += step) {
			const Vector2 tr = st->trail_at(Vector3(x, 0.0f, z));
			if (tr.x <= 0.0f || tr.y <= 0.0f) {
				continue;
			}
			const int ix = CLAMP(int(Math::round(x / ground_spacing)) - ground_origin.x, 0, n - 1);
			const int iz = CLAMP(int(Math::round(z / ground_spacing)) - ground_origin.y, 0, n - 1);
			const CellInput &in = cell_inputs[iz * n + ix];
			const Cell &cl = _cell_at(x, z);
			if (in.fixed || !cl.deformable || (in.moisture < 0.55f && cl.mud < 0.02f)) {
				continue;
			}
			// Expected prints in this 0.5 m square, drawn by its own hash and the clock.
			const float expect = rate * tr.x * tr.y * step * step;
			uint32_t h = hash_murmur3_one_float(x);
			h = hash_murmur3_one_float(z, h);
			h = hash_murmur3_one_32(pug_ticks, h);
			int k = 0;
			float u = float(h & 0xffffu) / 65536.0f;
			// Poisson draws by inversion, a handful at most.
			float pk = Math::exp(-expect);
			float cum = pk;
			while (u > cum && k < 4) {
				k++;
				pk *= expect / float(k);
				cum += pk;
			}
			for (int j = 0; j < k && printed < 64; j++) {
				h = hash_fmix32(h + 0x9e3779b9u);
				const float ox = (float(h & 0xffu) / 255.0f - 0.5f) * step;
				const float oz = (float((h >> 8) & 0xffu) / 255.0f - 0.5f) * step;
				const float yaw = float((h >> 16) & 0xffu) / 255.0f * (float)Math::TAU;
				const Vector3 p(x + ox, 0.0f, z + oz);
				const GroundSoil soil = get_soil(p);
				if (!soil.soft) {
					continue;
				}
				const float kk = soil.kc / 0.06f + soil.kphi; // kN/m^(n+2), a 6 cm plate
				const float sink = MIN(Math::pow(150.0f / MAX(kk, 1.0f), 1.0f / MAX(soil.n, 0.3f)), MIN(soil.max_sinkage, 0.15f));
				if (sink < 0.003f) {
					continue;
				}
				const float g = get_surface_height_at(p);
				press_box(Vector3(p.x, g, p.z), Vector2(0.06f, 0.08f), yaw, g - 0.5f, sink);
				trail_pocks++;
				printed++;
			}
		}
	}
	pug_ticks++;
}

float GroundSurface3D::_trail_passes(const Vector3 &p_position) const {
	if (!trails_on || wear_regrowth_days <= 0.0f) {
		return 0.0f;
	}
	const Vector2 tr = WorldContracts::stand()->trail_at(p_position);
	if (tr.x <= 0.0f || tr.y <= 0.0f) {
		return 0.0f;
	}
	return tr.x * tr.y * wear_regrowth_days / (365.0f * 0.69314718f);
}

float GroundSurface3D::get_wear_at(Vector3 p_position) const {
	const float trail = _trail_passes(p_position);
	if (wear_cells.is_empty()) {
		return trail;
	}

	const float fx = p_position.x / WEAR_CELL - 0.5f;
	const float fz = p_position.z / WEAR_CELL - 0.5f;
	const int x0 = int(Math::floor(fx));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int x, int z) {
		const Wear *w = wear_cells.getptr(Vector2i(x, z));
		return w ? w->passes : 0.0f;
	};
	return trail + Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

void GroundSurface3D::_wear_update(double p_delta) {
	if (wear_cells.is_empty() || wear_regrowth_days <= 0.0f) {
		return;
	}
	const float keep = Math::exp(-float(p_delta) * MAX(runoff_time_scale, 0.0f) * 0.69314718f / (wear_regrowth_days * 86400.0f));
	if (keep >= 1.0f) {
		return;
	}
	LocalVector<Vector2i> gone;
	for (KeyValue<Vector2i, Wear> &kv : wear_cells) {
		kv.value.passes *= keep;
		if (kv.value.passes < 0.05f && wear_clock - kv.value.last > 1.0) {
			gone.push_back(kv.key);
		}
	}
	for (const Vector2i &k : gone) {
		wear_cells.erase(k);
	}
	wear_dirty = true;
}

// The passes on the fine lattice, ground_wear_map (RF, n x n) over ground_wear_rect (x, z, size, 1 when set).
void GroundSurface3D::_wear_publish() {
	const int n = ground_n;
	if (active != this || !ground_valid || n < 2) {
		return;
	}
	if (!worn_tracks || (!_wear_any() && wear_texture.is_null())) {
		return;
	}
	if (!wear_dirty && wear_origin == ground_origin && wear_texture.is_valid()) {
		return;
	}
	PackedByteArray data;
	data.resize(n * n * 4);
	float *d = reinterpret_cast<float *>(data.ptrw());
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			d[z * n + x] = get_wear_at(Vector3((ground_origin.x + x) * ground_spacing, 0.0f, (ground_origin.y + z) * ground_spacing));
		}
	}
	Ref<Image> img = Image::create_from_data(n, n, false, Image::FORMAT_RF, data);
	if (wear_texture.is_valid() && wear_texture->get_width() == n && wear_texture->get_height() == n) {
		wear_texture->update(img);
	} else {
		wear_texture = ImageTexture::create_from_image(img);
		_set_global("ground_wear_map", wear_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	}
	_set_global("ground_wear_rect", Vector4((ground_origin.x - 0.5f) * ground_spacing, (ground_origin.y - 0.5f) * ground_spacing, n * ground_spacing, 1.0f), RSE::GLOBAL_VAR_TYPE_VEC4);
	wear_origin = ground_origin;
	wear_dirty = false;
}

// ---------------------------------------------------------------------------------------------------------
// Hidden stones and roots (docs/ideas/mud.md, round 14.3). A mud layer hides what lies in it: stones dropped out of
// the soil and tree roots under a canopy. MudRunner's "hidden obstacles" are the same idea: the surface reads as
// even mud, and a wheel sinking into it rides up on a stone or a root it cannot see. Here each 2 m cell with a mud
// layer (as deep as it gets when soaked) holds a few, from the cell's hash: stones rounded 8 to 25 cm in radius at any
// depth in the layer; roots under trees (WorldWeather's canopy closure) 3 to 8 cm thick, a metre or two long, near
// level and shallow. A wheel's sinkage stops on them (PhysXVehicle3D, press_wheel()), so it jolts up and its rut
// floor stays above them; the ruts and the water uncover them as real geometry. Near the followed point each is a
// StaticBody3D (PhysX keeps the bodies: an exposed stone stops a wheel or a body as any rock does) with its mesh.

static inline uint32_t _buried_hash(uint32_t p_a, uint32_t p_b) {
	uint32_t h = hash_murmur3_one_32(p_a);
	h = hash_murmur3_one_32(p_b, h);
	return hash_fmix32(h);
}

const LocalVector<GroundSurface3D::Buried> &GroundSurface3D::_buried_cell(const Vector2i &p_cell) const {
	if (const LocalVector<Buried> *got = buried_cache.getptr(p_cell)) {
		return *got;
	}
	LocalVector<Buried> &list = buried_cache[p_cell];
	if (!buried_obstacles || buried_density <= 0.0f || !ground_valid) {
		return list;
	}
	const Vector3 centre((p_cell.x + 0.5f) * BURIED_CELL, 0.0f, (p_cell.y + 0.5f) * BURIED_CELL);
	const Node *hs = height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(height_source);
	const Node *dmap = data_map.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(data_map);
	if (!Object::cast_to<TerrainDataMap3D>(dmap)) {
		dmap = Object::cast_to<TerrainDataMap3D>(hs);
	}
	// The layer as deep as it gets when soaked: the stones are there whatever the weather; dry, it bears like soil
	// and nothing sinks to them.
	const float layer = _mud_depth_at(centre, 1.0f, ground_type == GROUND_SAND, dmap);
	if (layer < 0.15f) {
		if (const LocalVector<Buried> *extra = buried_extra.getptr(p_cell)) {
			for (const Buried &b : *extra) {
				list.push_back(b);
			}
		}
		return list;
	}
	RandomPCG rng(_buried_hash(uint32_t(p_cell.x), uint32_t(p_cell.y)));
	const float expect = buried_density * BURIED_CELL * BURIED_CELL;
	const int count = int(Math::floor(expect + rng.randf()));
	const WorldWeather *wm = _weather();
	for (int k = 0; k < count; k++) {
		Buried b;
		b.id = _buried_hash(_buried_hash(uint32_t(p_cell.x), uint32_t(p_cell.y)), uint32_t(k + 1)) | 1u;
		const float x = (p_cell.x + rng.randf()) * BURIED_CELL;
		const float z = (p_cell.y + rng.randf()) * BURIED_CELL;
		const float g = _ground_at(x, z);
		const float canopy = wm ? CLAMP(wm->canopy_closure_at(Vector3(x, g, z)), 0.0f, 1.0f) : 0.0f;
		b.root = rng.randf() < 0.7f * canopy;
		if (b.root) {
			b.r = 0.03f + 0.05f * rng.randf();
			const float len = 0.8f + 1.2f * rng.randf();
			const float yaw = rng.randf() * Math::TAU;
			const Vector3 half(0.5f * len * Math::cos(yaw), 0.0f, 0.5f * len * Math::sin(yaw));
			const float top = 0.03f + 0.17f * rng.randf();
			const float dip = (rng.randf() - 0.5f) * 0.1f;
			b.a = Vector3(x, 0.0f, z) - half;
			b.b = Vector3(x, 0.0f, z) + half;
			b.a.y = _ground_at(b.a.x, b.a.z) - top - b.r - dip;
			b.b.y = _ground_at(b.b.x, b.b.z) - top - b.r + dip;
		} else {
			const float u = rng.randf();
			b.r = 0.08f + 0.17f * u * u;
			const float top = 0.05f + rng.randf() * (MIN(layer, 0.8f) - 0.05f);
			b.a = Vector3(x, g - top - b.r, z);
			b.b = b.a;
		}
		list.push_back(b);
	}
	if (const LocalVector<Buried> *extra = buried_extra.getptr(p_cell)) {
		for (const Buried &b : *extra) {
			list.push_back(b);
		}
	}
	return list;
}

float GroundSurface3D::_buried_depth(const Vector3 &p_position, float p_radius) const {
	float best = 1e3f;
	if (!buried_obstacles || buried_density <= 0.0f) {
		return best;
	}
	const float reach = p_radius + 1.25f; // the longest root's half plus its radius
	const int x0 = int(Math::floor((p_position.x - reach) / BURIED_CELL));
	const int x1 = int(Math::floor((p_position.x + reach) / BURIED_CELL));
	const int z0 = int(Math::floor((p_position.z - reach) / BURIED_CELL));
	const int z1 = int(Math::floor((p_position.z + reach) / BURIED_CELL));
	const Vector2 p(p_position.x, p_position.z);
	const float g = _ground_at(p_position.x, p_position.z);
	for (int cz = z0; cz <= z1; cz++) {
		for (int cx = x0; cx <= x1; cx++) {
			for (const Buried &b : _buried_cell(Vector2i(cx, cz))) {
				if (buried_loose.has(b.id)) {
					continue;
				}
				const Vector2 a2(b.a.x, b.a.z);
				const Vector2 ab(b.b.x - b.a.x, b.b.z - b.a.z);
				const float l2 = ab.length_squared();
				const float t = l2 > 1e-8f ? CLAMP((p - a2).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
				const float d = (a2 + ab * t).distance_to(p);
				if (d > b.r + p_radius) {
					continue;
				}
				// The body's top under the nearest point of the disc to its axis (a sphere's or tube's cap).
				const float off = MAX(d - p_radius, 0.0f);
				const float top = Math::lerp(b.a.y, b.b.y, t) + Math::sqrt(MAX(b.r * b.r - off * off, 0.0f));
				best = MIN(best, MAX(g - top, 0.0f));
			}
		}
	}
	return best;
}

float GroundSurface3D::get_buried_depth_at(Vector3 p_position, float p_radius) const {
	return _buried_depth(p_position, MAX(p_radius, 0.0f));
}

Array GroundSurface3D::get_buried_near(Vector3 p_position, float p_radius) const {
	Array out;
	const int x0 = int(Math::floor((p_position.x - p_radius) / BURIED_CELL));
	const int x1 = int(Math::floor((p_position.x + p_radius) / BURIED_CELL));
	const int z0 = int(Math::floor((p_position.z - p_radius) / BURIED_CELL));
	const int z1 = int(Math::floor((p_position.z + p_radius) / BURIED_CELL));
	for (int cz = z0; cz <= z1; cz++) {
		for (int cx = x0; cx <= x1; cx++) {
			for (const Buried &b : _buried_cell(Vector2i(cx, cz))) {
				if (buried_loose.has(b.id)) {
					continue; // knocked loose (round 15.2)
				}
				const Vector3 m = (b.a + b.b) * 0.5f;
				if (Vector2(m.x - p_position.x, m.z - p_position.z).length() > p_radius) {
					continue;
				}
				Dictionary d;
				d["a"] = b.a;
				d["b"] = b.b;
				d["radius"] = b.r;
				d["root"] = b.root;
				d["id"] = int64_t(b.id);
				out.push_back(d);
			}
		}
	}
	return out;
}

Ref<ArrayMesh> GroundSurface3D::_buried_mesh(bool p_root, uint32_t p_seed) {
	const int slot = p_root ? 4 + int(p_seed % 2u) : int(p_seed % 4u);
	if (buried_meshes[slot].is_valid()) {
		return buried_meshes[slot];
	}
	PackedVector3Array verts;
	PackedVector3Array norms;
	PackedInt32Array idx;
	auto noise3 = [&](const Vector3 &q) {
		// Smooth value noise for the rock's lumps and the root's knots.
		const Vector3 f = q.floor();
		const Vector3 t = q - f;
		const Vector3 s = t * t * (Vector3(3, 3, 3) - 2.0f * t);
		auto h = [&](int x, int y, int z) {
			return float(_buried_hash(_buried_hash(uint32_t(x) + p_seed * 7919u, uint32_t(y)), uint32_t(z)) & 0xffffu) / 65535.0f;
		};
		const int x = int(f.x), y = int(f.y), z = int(f.z);
		const float a = Math::lerp(Math::lerp(h(x, y, z), h(x + 1, y, z), s.x), Math::lerp(h(x, y + 1, z), h(x + 1, y + 1, z), s.x), s.y);
		const float b = Math::lerp(Math::lerp(h(x, y, z + 1), h(x + 1, y, z + 1), s.x), Math::lerp(h(x, y + 1, z + 1), h(x + 1, y + 1, z + 1), s.x), s.y);
		return Math::lerp(a, b, s.z);
	};
	if (!p_root) {
		// A water-worn stone: a sphere with two octaves of lumps, flattened a little (rested on its broad side).
		const int rings = 14;
		const int segs = 20;
		for (int i = 0; i <= rings; i++) {
			const float th = Math::PI * i / rings;
			for (int j = 0; j <= segs; j++) {
				const float ph = Math::TAU * j / segs;
				const Vector3 d(Math::sin(th) * Math::cos(ph), Math::cos(th), Math::sin(th) * Math::sin(ph));
				const float r = 0.82f + 0.22f * noise3(d * 1.7f) + 0.06f * noise3(d * 4.3f + Vector3(5, 5, 5));
				verts.push_back(Vector3(d.x, d.y * 0.72f, d.z * 0.9f) * r);
			}
		}
		for (int i = 0; i < rings; i++) {
			for (int j = 0; j < segs; j++) {
				const int a = i * (segs + 1) + j;
				const int b = a + segs + 1;
				idx.push_back(a);
				idx.push_back(a + 1);
				idx.push_back(b);
				idx.push_back(b);
				idx.push_back(a + 1);
				idx.push_back(b + 1);
			}
		}
	} else {
		// A root: a tube along x from -0.5 to 0.5, unit radius, tapering to the tip, wavering and knotted.
		const int rings = 16;
		const int sides = 8;
		for (int i = 0; i <= rings; i++) {
			const float u = float(i) / rings;
			const float x = u - 0.5f;
			const Vector3 bend(0.0f, 0.6f * (noise3(Vector3(u * 3.0f, 0, 0)) - 0.5f), 1.2f * (noise3(Vector3(u * 2.5f, 7, 0)) - 0.5f));
			for (int j = 0; j <= sides; j++) {
				const float ph = Math::TAU * j / sides;
				const float r = Math::lerp(1.15f, 0.55f, u) * (0.85f + 0.3f * noise3(Vector3(u * 6.0f, Math::cos(ph) * 1.5f, Math::sin(ph) * 1.5f)));
				verts.push_back(Vector3(x, 0, 0) + bend + Vector3(0.0f, Math::cos(ph), Math::sin(ph)) * r);
			}
		}
		for (int i = 0; i < rings; i++) {
			for (int j = 0; j < sides; j++) {
				const int a = i * (sides + 1) + j;
				const int b = a + sides + 1;
				idx.push_back(a);
				idx.push_back(b);
				idx.push_back(a + 1);
				idx.push_back(a + 1);
				idx.push_back(b);
				idx.push_back(b + 1);
			}
		}
	}
	// Smooth normals from the faces.
	norms.resize(verts.size());
	for (int i = 0; i < norms.size(); i++) {
		norms.write[i] = Vector3();
	}
	for (int i = 0; i + 2 < idx.size(); i += 3) {
		const Vector3 n = (verts[idx[i + 1]] - verts[idx[i]]).cross(verts[idx[i + 2]] - verts[idx[i]]);
		for (int k = 0; k < 3; k++) {
			norms.write[idx[i + k]] += n;
		}
	}
	for (int i = 0; i < norms.size(); i++) {
		norms.write[i] = norms[i].normalized();
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = norms;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> m;
	m.instantiate();
	m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (buried_materials[p_root ? 1 : 0].is_null()) {
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		// Wet from the mud: a mid grey-brown stone, a dark bark, both glossy with the film on them.
		mat->set_albedo(p_root ? Color(0.11f, 0.075f, 0.05f) : Color(0.27f, 0.24f, 0.2f));
		mat->set_roughness(p_root ? 0.55f : 0.45f);
		buried_materials[p_root ? 1 : 0] = mat;
	}
	m->surface_set_material(0, buried_materials[p_root ? 1 : 0]);
	buried_meshes[slot] = m;
	return m;
}

void GroundSurface3D::_buried_clear_bodies() {
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_bodies) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(kv.value))) {
			n->queue_free();
		}
	}
	buried_bodies.clear();
}

// Dug round, knocked loose (docs/ideas/mud.md, round 15.2). A wheel's print does not stop as a whole on a stone
// under part of it: the tyre wraps round it and the mud is pressed out round the stone (the stamp's floor is the
// stone's top, GroundDeformField::floor_top()), so the stone's crown stands in the rut floor. The body under the
// print that stands highest over its floor is the one the press keeps to (prints are small and stones sparse).
bool GroundSurface3D::_buried_under(const Vector3 &p_position, float p_radius, float p_bottom, Buried &r_hit) const {
	if (!buried_obstacles || buried_density <= 0.0f) {
		return false;
	}
	const float reach = p_radius + 1.25f;
	const int x0 = int(Math::floor((p_position.x - reach) / BURIED_CELL));
	const int x1 = int(Math::floor((p_position.x + reach) / BURIED_CELL));
	const int z0 = int(Math::floor((p_position.z - reach) / BURIED_CELL));
	const int z1 = int(Math::floor((p_position.z + reach) / BURIED_CELL));
	const Vector2 p(p_position.x, p_position.z);
	float best = 0.0f;
	bool found = false;
	for (int cz = z0; cz <= z1; cz++) {
		for (int cx = x0; cx <= x1; cx++) {
			for (const Buried &b : _buried_cell(Vector2i(cx, cz))) {
				if (buried_loose.has(b.id)) {
					continue;
				}
				const Vector2 a2(b.a.x, b.a.z);
				const Vector2 ab(b.b.x - b.a.x, b.b.z - b.a.z);
				const float l2 = ab.length_squared();
				const float t = l2 > 1e-8f ? CLAMP((p - a2).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
				const float d = (a2 + ab * t).distance_to(p);
				if (d > b.r + p_radius) {
					continue;
				}
				const float over = Math::lerp(b.a.y, b.b.y, t) + b.r - p_bottom;
				if (over > best) {
					best = over;
					r_hit = b;
					found = true;
				}
			}
		}
	}
	return found;
}

// A stone the rut has uncovered past its middle is held only by the mud round its lower half; a tyre rolling into
// it shoves it out (the passive resistance of soft mud round a half-buried boulder is small next to a tyre's push;
// MudRunner's loose rocks are the same idea). It becomes a PhysX rigid body moving off with part of the tyre's
// speed, and the mud that lay on and round it closes into its socket. Roots stay: they run on into the ground.
void GroundSurface3D::_buried_kicks_update() {
	if (buried_kicks.is_empty() || !is_inside_tree()) {
		buried_kicks.clear();
		return;
	}
	for (const BuriedKick &k : buried_kicks) {
		const Buried &b = k.b;
		if (buried_loose.has(b.id)) {
			continue;
		}
		buried_loose.insert(b.id);
		if (const ObjectID *got = buried_bodies.getptr(b.id)) {
			if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(*got))) {
				n->queue_free();
			}
			buried_bodies.erase(b.id);
		}
		const float rc = b.r * 0.85f;
		// Lifted clear of the floor it sat in so it does not start inside the ground.
		_make_loose(b, Vector3(b.a.x, MAX(b.a.y, k.floor + rc + 0.01f), b.a.z), k.velocity);
		// The mud on it and round it slumps into its socket: the crown that stood in the rut goes down to the
		// rut's floor, and a little more where it sat.
		const float crown = b.a.y + b.r - k.floor;
		if (crown > 0.0f) {
			dig(Vector3(b.a.x, 0.0f, b.a.z), b.r * 0.8f, crown + 0.3f * b.r);
		}
	}
	buried_kicks.clear();
	gpu_cells_dirty = true;
	near_cells_dirty = true;
}

// A loose stone as a PhysX rigid body: a sphere collider (0.85 of the mesh's radius) and its mesh, granite's density.
RigidBody3D *GroundSurface3D::_make_loose(const Buried &p_b, const Vector3 &p_at, const Vector3 &p_velocity) {
	RigidBody3D *body = memnew(RigidBody3D);
	body->set_name(vformat("Loose%d", int64_t(p_b.id)));
	CollisionShape3D *cs = memnew(CollisionShape3D);
	Ref<SphereShape3D> sp;
	sp.instantiate();
	const float rc = p_b.r * 0.85f;
	sp->set_radius(rc);
	cs->set_shape(sp);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(_buried_mesh(false, p_b.id >> 1));
	mi->set_transform(Transform3D(Basis().scaled(Vector3(p_b.r, p_b.r, p_b.r)), Vector3()));
	body->add_child(cs);
	body->add_child(mi);
	body->set_mass(MAX(2650.0f * 4.18879f * rc * rc * rc, 0.5f));
	body->add_to_group(SNAME("ground_loose"));
	add_child(body, false, INTERNAL_MODE_BACK);
	body->set_global_transform(Transform3D(Basis(Vector3(0, 1, 0), float(p_b.id % 628u) * 0.01f), p_at));
	body->set_linear_velocity(p_velocity);
	body->set_angular_velocity(Vector3(p_velocity.z, 0.0f, -p_velocity.x) / MAX(p_b.r, 0.05f));
	buried_loose_bodies[p_b.id] = body->get_instance_id();
	return body;
}

int64_t GroundSurface3D::add_loose_stone(Vector3 p_position, float p_radius, Vector3 p_velocity) {
	if (!is_inside_tree()) {
		return 0;
	}
	Buried b;
	b.r = CLAMP(p_radius, 0.03f, 1.0f);
	b.a = p_position;
	b.b = p_position;
	uint32_t h = hash_murmur3_one_float(p_position.x);
	h = hash_murmur3_one_float(p_position.y, h);
	h = hash_murmur3_one_float(p_position.z, h);
	b.id = hash_fmix32(hash_murmur3_one_32(loose_added++, h)) | 1u;
	_make_loose(b, p_position, p_velocity);
	return b.id;
}

// Rolled onto (docs/ideas/mud.md, round 18.2). Whether a tyre meets a stone at its front or rolls onto it is the
// wheel-over-step geometry: the tread meets a stone standing p over the rut floor at an angle alpha from the vertical
// with cos(alpha) = (R - p) / R, R the tyre's radius (about the print's length). Under 30 degrees the contact force is
// mostly down: the tyre rolls onto the stone; steeper, it pushes it (_loose_shove()).
bool GroundSurface3D::_loose_rolled_onto(float p_proud, const Vector2 &p_half) {
	const float rt = MAX(2.0f * p_half.length(), 0.3f);
	return p_proud < rt * (1.0f - Math::cos(Math::deg_to_rad(30.0f)));
}

// A loose stone under a rolling tyre is pressed by the carcass: the local pressure where a hard object meets a tyre
// is about its inflation plus the carcass's own stiffness (the tyre envelope models: Zegelaar 1998; Gillespie; round
// 12's tyre_flat uses p_i + 15 kPa), here a soft-soil 100 kPa plus 15, over the stone's section: kilonewtons, well
// past what soft or stiff mud bears under a 10 to 30 cm stone. Under the tread's middle it is pressed in until its top
// is at the tyre's floor (or it reaches the firm base, where it holds the tyre up). Under the tyre's rounded shoulder
// that force leans out by the carcass's slope there, asin(offset / (half width + r)); where that sideways share passes
// what the mud beside it resists (lateral bearing of a buried body in clay, about 10 tau A: Randolph and Houlsby
// 1984), it squirts out sideways, as a pip from fingers, at up to the tyre's speed.
void GroundSurface3D::_loose_under(const Vector3 &p_position, const Vector2 &p_half, float p_yaw, float p_bottom, const Vector2 &p_velocity) {
	if (buried_loose_bodies.is_empty()) {
		return;
	}
	const float cy = Math::cos(p_yaw);
	const float sy = Math::sin(p_yaw);
	const float dt = MAX(float(get_physics_process_delta_time()), 1e-3f);
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	const float hx = Math::abs(p_half.x);
	const float hz = Math::abs(p_half.y);
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(kv.value));
		if (!rb || !rb->is_inside_tree()) {
			continue;
		}
		float rc = 0.1f;
		for (int ci = 0; ci < rb->get_child_count(); ci++) {
			if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(rb->get_child(ci))) {
				if (Ref<SphereShape3D> sp = cs->get_shape(); sp.is_valid()) {
					rc = sp->get_radius();
				}
			}
		}
		const Vector3 at = rb->get_global_position();
		const float dx = at.x - p_position.x;
		const float dz = at.z - p_position.z;
		// The print's own frame, as GroundDeformField::press(): local x across, z along.
		const float lx = cy * dx - sy * dz;
		const float lz = sy * dx + cy * dz;
		if (Math::abs(lz) > hz || Math::abs(lx) > hx + 0.5f * rc) {
			continue;
		}
		const float proud = at.y + rc / 0.85f - p_bottom; // its top over the tyre's floor
		if (proud <= 0.002f || !_loose_rolled_onto(proud, p_half)) {
			continue;
		}
		LooseState &ls = loose_state[kv.key];
		const Cell &c = _cell_at(at.x, at.z);
		const float rho_mud = c.unit_weight * 1000.0f / 9.81f;
		const float tau = c.wave_tau > 0.0f ? c.wave_tau * rho_mud : c.cohesion * 1000.0f;
		const float section = Math::PI * rc * rc;
		const float force = 115000.0f * section;
		const float lean = Math::asin(CLAMP(Math::abs(lx) / (hx + rc), 0.0f, 1.0f));
		const float lateral = force * Math::sin(lean) - 10.0f * MAX(tau, 1.0f) * section;
		if (lateral > 0.0f && Math::abs(lx) > 0.1f * hx) {
			// Squeezed out to its side.
			const float sgn = lx >= 0.0f ? 1.0f : -1.0f;
			const Vector3 side = Vector3(cy, 0.0f, -sy) * sgn; // local +x in world
			const float cap = MAX(p_velocity.length(), 1.0f);
			const float dv = MIN(lateral * dt / rb->get_mass(), cap);
			rb->apply_central_impulse(side * dv * rb->get_mass());
			loose_squeezed++;
		}
		// Pressed down to the tyre's floor either way while under it.
		ls.cap = p_bottom;
		ls.cap_frame = frame;
	}
}

// Shoved along (docs/ideas/mud.md, round 16.3). A loose stone in front of a rolling tyre is pushed as a bulldozing
// blade pushes its berm (Bekker 1956; Ishigami et al. 2007 on the bulldozing force at a wheel's front): the tyre's
// front drives it on at the tyre's speed, and the curved front sends it out sideways off the tyre's line, toward the
// side it sits on, so stones pile at a rut's edges as in a wheel's furrow. The tyre drives it against the mud's drag
// on it (the passive pressure on its face and its rolling resistance, _loose_update(), round 17.1), at most with a
// loaded tyre's 6 kN.
void GroundSurface3D::_loose_shove(const Vector3 &p_position, const Vector2 &p_half, float p_yaw, float p_bottom, const Vector2 &p_velocity) {
	const float speed = p_velocity.length();
	if (buried_loose_bodies.is_empty() || speed < 0.2f) {
		return;
	}
	const Vector2 heading = p_velocity / speed;
	const Vector2 side(-heading.y, heading.x);
	const float dt = MAX(float(get_physics_process_delta_time()), 1e-3f);
	const float reach = p_half.length();
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(kv.value));
		if (!rb || !rb->is_inside_tree()) {
			continue;
		}
		const Vector3 at = rb->get_global_position();
		const Vector2 to(at.x - p_position.x, at.z - p_position.z);
		const float ahead = to.dot(heading);
		const float off = to.dot(side);
		float r = 0.1f;
		for (int c = 0; c < rb->get_child_count(); c++) {
			if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(rb->get_child(c))) {
				if (Ref<SphereShape3D> sp = cs->get_shape(); sp.is_valid()) {
					r = sp->get_radius();
				}
			}
		}
		// In front of the tyre's middle, within its width, and low enough for its front to meet (below the axle's
		// height over the rut floor, about the print's half length).
		if (ahead < 0.0f || ahead > reach + r || Math::abs(off) > Math::abs(p_half.x) + r || at.y - r > p_bottom + reach) {
			continue;
		}
		// Low in the rut the tyre's tread meets its top nearly from above and rolls onto it (_loose_under()); only a
		// stone standing proud meets the tyre's front (round 18.2).
		if (_loose_rolled_onto(at.y + r / 0.85f - p_bottom, p_half)) {
			continue;
		}
		const float f_max = 6000.0f;
		const float lateral = off >= 0.0f ? 1.0f : -1.0f;
		const Vector2 want = heading * speed + side * lateral * 0.5f * speed * (1.0f - Math::abs(off) / MAX(Math::abs(p_half.x) + r, 1e-3f));
		const Vector3 v = rb->get_linear_velocity();
		Vector3 dv(want.x - v.x, 0.0f, want.y - v.z);
		const float need = dv.length() * rb->get_mass();
		const float can = f_max * dt;
		if (need > can) {
			dv *= can / need;
		}
		rb->apply_central_impulse(dv * rb->get_mass());
		// The tyre meets it above its middle, so it drives it down as well as on (round 17.1): the line between
		// the tyre's axle and the stone's centre falls (R - r) / (R + r) over their spacing, R the tyre's radius
		// (about the print's length).
		const float rt = MAX(2.0f * p_half.length(), 0.3f);
		loose_state[kv.key].push_down = dv.length() * rb->get_mass() / dt * (rt - r) / (rt + r);
	}
}

// A body pressed moving: as press(), with the velocity that leans the rim forward into a bow wave.
float GroundSurface3D::_press_moving(const Vector3 &p_position, float p_radius, float p_bottom, float p_sink, const Vector2 &p_velocity) {
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	float moved = 0.0f;
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, true, contact_tolerance, p_velocity);
	} else {
		moved = field.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, true, contact_tolerance, p_velocity);
	}
	if (is_near_active()) {
		near_fld.press_settle = field.press_settle;
		near_fld.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, true, contact_tolerance, p_velocity);
	}
	return moved;
}

// Loose stones in the mud (docs/ideas/mud.md, round 17). On a mud layer a loose stone lies on the deformed mud, not
// on the terrain's collider (which knows nothing of ruts and prints): it drops the ground's collision layers while it
// is at ground level there, and this holds it up instead, at its settled depth in the mud under it.
//
// Settled depth: a sphere pressed into a cohesive soft soil is held by the soil's bearing, Nc tau A, plus the weight
// of the mud it displaces. A the contact's plan area; Nc 6 at the surface (a shallow footing on clay: Skempton 1951,
// Nc = 6 (1 + 0.2 D/B)) rising to 13.9 fully under, the factor that matches Beris, Tsamopoulos, Armstrong and Brown's
// (1985) criterion for a dense sphere held in a Bingham fluid, Y = tau_y / (delta rho g d) > 0.048 (Chhabra 2007,
// ch. 5). The stone sinks until Nc tau A + rho_mud g V_under carries its weight. Past the criterion nothing does: it
// sinks to the firm base under the layer. tau: slurry and soft mud's yield stress, stiff mud's cohesion.
//
// 17.1 Ploughed: moving, the stone's underside at its settled depth is a blunt body ploughing (Bowden and Tabor's
// ploughing term; the furrows of "ploughing boulders", Ballantyne 2001). Its underside presses the mud as a round
// print each step, moving, so the rim leans forward into a bow ahead (GroundDeformField::press()'s bow wave) and
// spills to the sides as levees: a furrow behind, a heap where it stops. It rides the undisturbed mud ahead (the
// ground less the dents, not their rims), so it drops into ruts it meets. The mud brakes it: rolling resistance in
// soft soil, sqrt(z / d) of its weight (Bekker 1969), and the passive pressure on its front over the furrow's depth
// (Rankine, as _loose_shove()).
//
// 17.2 Sunk back: at rest it creeps down toward its settled depth (Stokes with the plastic viscosity, at most 5 cm/s,
// slowed as the yield stress nears what holds it). A stone sunk out of sight, or buried by mud or silt laid over it,
// becomes a hidden stone again, a few centimetres under, kept with the deposits.
static float _loose_settled_depth(float p_rc, float p_mass, float p_tau, float p_rho_mud, bool &r_held) {
	const float w = p_mass * 9.81f;
	const int steps = 40;
	for (int k = 0; k <= steps; k++) {
		const float z = 2.0f * p_rc * float(k) / float(steps);
		const float area = z < p_rc ? Math::PI * (2.0f * p_rc * z - z * z) : Math::PI * p_rc * p_rc;
		const float under = Math::PI * z * z * (3.0f * p_rc - z) / 3.0f;
		const float nc = Math::lerp(6.0f, 13.9f, z / (2.0f * p_rc));
		if (nc * p_tau * area + p_rho_mud * 9.81f * under >= w) {
			r_held = true;
			return z;
		}
	}
	r_held = false;
	return 2.0f * p_rc;
}

// The drag of a flowing mud on a stone in it, as a velocity change over the step (round 18.1): the bluff-body term
// 0.5 rho Cd A u^2 (Cd 1.2, as GroundDebrisFlow3D's push on bodies) and the yield stress's hold on it over the same
// face, Nc tau_y A with Nc 13.9 (the factor matching Beris et al.'s criterion, as the settled depth): a yield-stress
// flow takes a stone held in its plug along at its own speed (boulders rafted in debris flows: Iverson 1997). Never
// more than brings it to the flow's speed.
Vector2 GroundSurface3D::_loose_carry(const Vector2 &p_rel, float p_wet, float p_rc, float p_rho, float p_tau, float p_mass, float p_dt) {
	const float s = p_rel.length();
	if (s < 0.02f || p_wet <= 0.0f || p_mass <= 0.0f) {
		return Vector2();
	}
	const float area = 2.0f * p_rc * p_wet;
	const float f = area * (0.5f * p_rho * 1.2f * s * s + 13.9f * MAX(p_tau, 0.0f));
	return p_rel / s * MIN(f * p_dt / p_mass, s);
}

// A stream's push on a loose stone in it (docs/ideas/mud.md, round 19.1): the force balance on a lone grain
// (Wiberg and Smith 1987; Komar and Li 1986), drag 0.5 rho Cd A u^2 with Cd 0.76 (Schmeeckle et al. 2007, a grain on
// a bed) and lift 0.5 rho Cl A u^2 with Cl 0.2, on the face standing above the bed and under the water, at the
// speed a logarithmic profile over a fine bed (ks 1 cm) gives at that face's middle from the water's depth-averaged
// speed (U ln(z/z0) / (ln(h/z0) - 1)); buoyancy on what is under the surface. A lone stone standing proud of a fine
// bed moves at a Shields number far under the 0.045 of a bed of its own size, as Komar and Li's exposure gives.
GroundSurface3D::StreamPush GroundSurface3D::_loose_stream(const WorldWater *p_water, const Vector3 &p_at, float p_rc, float p_mass, const Vector3 &p_velocity, float p_bed) const {
	StreamPush out;
	Vector3 u3;
	const float ws = p_water->height_except(p_at, this, &u3); // not the ground's own standing water
	const float h = ws - p_bed;
	if (!(h > 0.02f) || p_mass <= 0.0f) {
		return out;
	}
	const float pw = MIN(CLAMP(p_at.y + p_rc - p_bed, 0.0f, 2.0f * p_rc), h); // its face above the bed, under water
	const float z0 = 0.01f / 30.0f;
	out.flow = Vector2(u3.x, u3.z);
	out.depth = h;
	const float scale = pw > 0.002f ? CLAMP(Math::log(MAX(0.5f * pw, 2.0f * z0) / z0) / MAX(Math::log(h / z0) - 1.0f, 1.0f), 0.0f, 1.3f) : 0.0f;
	out.near_bed = Vector2(u3.x, u3.z) * scale;
	const Vector2 rel = out.near_bed - Vector2(p_velocity.x, p_velocity.z);
	const float s = rel.length();
	const float area = 2.0f * p_rc * pw;
	out.drag = rel * (0.5f * 1000.0f * 0.76f * area * s);
	const float sub = CLAMP((ws - (p_at.y - p_rc)) / (2.0f * p_rc), 0.0f, 1.0f);
	out.up = 1000.0f * 9.81f * 4.18879f * p_rc * p_rc * p_rc * sub + 0.5f * 1000.0f * 0.2f * area * s * s;
	out.wet = true;
	return out;
}

// On firm ground (a gravel bed, rock) the colliders hold it: it starts once the drag beats its submerged weight less
// the lift times the tangent of its pivot angle (40 degrees, an angular stone alone on a finer bed), and rolls on
// against a rolling friction of 0.6 of that (Bagnold's dynamic friction for bedload), so it stops in slack water.
void GroundSurface3D::_loose_stream_firm(RigidBody3D *p_body, float p_rc, const StreamPush &p_push, float p_dt) {
	const float m = p_body->get_mass();
	const Vector3 v = p_body->get_linear_velocity();
	Vector2 vh(v.x, v.z);
	const float up = MIN(p_push.up, 0.95f * m * 9.81f);
	const float normal = m * 9.81f - up;
	const float fd = p_push.drag.length();
	const float sp = vh.length();
	float vy = v.y;
	if (Math::abs(v.y) > 0.5f) {
		// Falling through the water, not yet on the bed: buoyed and swept.
		vh += p_push.drag * (p_dt / m);
		vy += up * p_dt / m;
	} else if (sp < 0.05f) {
		if (fd <= 0.84f * normal) {
			// Held where it lies: its pivot holds it.
			p_body->set_linear_velocity(Vector3(0.0f, v.y, 0.0f));
			p_body->set_angular_velocity(Vector3());
			return;
		}
		vh += p_push.drag / fd * ((fd - 0.6f * normal) * p_dt / m);
	} else {
		vh += p_push.drag * (p_dt / m);
		const float s2 = vh.length();
		if (s2 > 1e-4f) {
			vh -= vh / s2 * MIN(0.6f * normal * p_dt / m, s2);
		}
	}
	p_body->set_linear_velocity(Vector3(vh.x, vy, vh.y));
	p_body->set_angular_velocity(vh.length() > 0.02f ? Vector3(vh.y, 0.0f, -vh.x) / p_rc : Vector3());
}

// Scour round a stone in a current (docs/ideas/mud.md, round 20.2): the horseshoe vortex the stone sheds at its foot
// raises the bed shear round its front and sides about fourfold (Melville and Coleman 2000 for piers; Euler et al.
// 2017, obstacle marks at boulders: a crescent hole round the upstream face, a ridge of sediment trailing behind). A
// cohesive bed erodes past its critical shear at Partheniades' rate E = M (tau / tau_c - 1), M 2e-4 kg/m2/s, tau_c
// from 0.5 Pa for a soaked mud rising with its cohesion; the hole deepens towards half the stone's width, slowing as
// it does, a third of what it takes laid as the tail, the rest carried off as silt (round 20.3). In liquid mud the
// hole fills back as it is dug (the mud's own flow); only its silt shows.
void GroundSurface3D::_loose_scour(const Vector3 &p_at, float p_rc, const StreamPush &p_push, const Cell &p_cell, LooseState &r_state, float p_dt) {
	const float u = p_push.flow.length();
	if (u < 0.05f || !p_cell.deformable) {
		return;
	}
	const float d = 2.0f * p_rc;
	const float eq = 0.5f * d;
	if (r_state.scour >= eq) {
		return;
	}
	const float tau = 4.0f * 1000.0f * 0.003f * u * u; // Pa at its foot
	const float tau_c = CLAMP(0.5f + 0.5f * p_cell.cohesion, 0.5f, 5.0f);
	if (tau <= tau_c) {
		return;
	}
	const float e = 2e-4f * (tau / tau_c - 1.0f); // kg/m2/s
	const float dz = e / 1300.0f * MAX(runoff_time_scale, 0.0f) * p_dt * (1.0f - r_state.scour / eq);
	r_state.scour += dz;
	r_state.scour_owed += dz;
	if (r_state.scour_owed < 0.003f) {
		return;
	}
	const float owed = r_state.scour_owed;
	r_state.scour_owed = 0.0f;
	const Vector2 dir = p_push.flow / u;
	const Vector2 side(-dir.y, dir.x);
	const Vector2 c(p_at.x, p_at.z);
	// Discs no smaller than the field's texel (a smaller one covers none), the depth spread to keep the volume.
	const float r0 = 0.7f * p_rc;
	const float rr = MAX(r0, (is_near_active() ? near_fld.texel() : field.texel()));
	const float thin = (r0 * r0) / (rr * rr);
	float vol = 0.0f;
	const Vector2 spots[3] = { c - dir * (1.1f * p_rc), c + side * (1.0f * p_rc) - dir * (0.3f * p_rc), c - side * (1.0f * p_rc) - dir * (0.3f * p_rc) };
	for (int k = 0; k < 3; k++) {
		const float depth = (k == 0 ? owed : 0.7f * owed) * thin; // deepest at the nose
		vol += dig(Vector3(spots[k].x, p_at.y, spots[k].y), rr, depth);
	}
	vol = MAX(vol, (float)Math::PI * rr * rr * owed * thin * 2.4f);
	stone_scour_m3 += vol;
	// The tail: a third laid in its lee, two stone widths back.
	const Vector2 tail = c + dir * (2.0f * d);
	const float tr = MAX(1.0f * p_rc, 0.05f);
	deposit(Vector3(tail.x, p_at.y, tail.y), tr, vol / 3.0f / ((float)Math::PI * tr * tr));
	const float kg = vol * 2.0f / 3.0f * 1300.0f * CLAMP(1.0f - p_cell.sand, 0.0f, 1.0f);
	if (kg > 0.0f) {
		if (WorldWater *ww = WorldContracts::water()) {
			ww->add_sediment_mass(Vector3(p_at.x, p_at.y + MIN(0.5f * p_push.depth, d), p_at.z), kg, 0.0f);
		}
		stone_plume_kg += kg;
	}
}

void GroundSurface3D::_loose_update(float p_delta) {
	// Stones read back from the deposits (round 19.2), placed once the field is up: as they lay, at their depth.
	if (!loose_pending.is_empty() && field.valid && ground_valid && is_inside_tree()) {
		for (const LoosePending &lp : loose_pending) {
			Buried b;
			b.id = lp.id;
			b.r = lp.r;
			b.a = lp.at;
			b.b = lp.at;
			buried_loose.insert(lp.id); // out of the hidden stones for good
			RigidBody3D *rb = _make_loose(b, lp.at, Vector3());
			rb->set_global_transform(Transform3D(Basis(lp.rot), lp.at));
			LooseState &ls = loose_state[lp.id];
			ls.z = lp.z;
			ls.ref = lp.ref;
		}
		loose_pending.clear();
	}
	if (buried_loose_bodies.is_empty() || !field.valid) {
		loose_state.clear();
		return;
	}
	const float dt = MAX(p_delta, 1e-3f);
	LocalVector<uint32_t> hidden;
	LocalVector<GroundDebrisFlow3D *> flows;
	if (is_inside_tree()) {
		for (Node *nd : get_tree()->get_nodes_in_group(SNAME("ground_debris_flows"))) {
			if (GroundDebrisFlow3D *f = Object::cast_to<GroundDebrisFlow3D>(nd); f && f->get_active_bodies() > 0) {
				flows.push_back(f);
			}
		}
	}
	WorldWater *ww = WorldContracts::water();
	const bool streams = stream_stones && ww && ww->has_provider();
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(kv.value));
		if (!rb || !rb->is_inside_tree()) {
			continue;
		}
		float rc = 0.1f;
		for (int ci = 0; ci < rb->get_child_count(); ci++) {
			if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(rb->get_child(ci))) {
				if (Ref<SphereShape3D> sp = cs->get_shape(); sp.is_valid()) {
					rc = sp->get_radius();
				}
			}
		}
		const float r = rc / 0.85f; // the mesh's radius
		LooseState &ls = loose_state[kv.key];
		ls.xf = rb->get_global_transform();
		ls.r = r;
		Vector3 at = ls.xf.origin;
		const Cell &c = _cell_at(at.x, at.z);
		const float g0 = _ground_at(at.x, at.z);
		const bool on_mud = c.deformable && c.mud >= 0.02f && _on_lattice(at.x, at.z);
		// A debris flow over it carries it (round 18.1).
		Vector2 carry;
		for (GroundDebrisFlow3D *f : flows) {
			const float depth = f->get_flow_depth_at(at);
			if (depth > 0.02f) {
				const Vector3 u = f->get_flow_velocity_at(at);
				const Vector3 lv = rb->get_linear_velocity();
				carry += _loose_carry(Vector2(u.x, u.z) - Vector2(lv.x, lv.z), MIN(depth, 2.0f * rc), rc, f->get_density(), f->get_yield_stress(), rb->get_mass(), dt);
			}
		}
		// A stream or ford over it (round 19.1).
		const StreamPush sp = streams ? _loose_stream(ww, at, rc, rb->get_mass(), rb->get_linear_velocity(), on_mud ? get_surface_height_at(at) : at.y - rc) : StreamPush();
		// Off the mud, or well over it (thrown, on a rock): the colliders hold it as any body.
		if (!on_mud || at.y - rc > g0 + 0.3f) {
			if (carry != Vector2()) {
				rb->apply_central_impulse(Vector3(carry.x, 0.0f, carry.y) * rb->get_mass());
			}
			if (sp.wet) {
				_loose_stream_firm(rb, rc, sp, dt);
				// Lying still on a soil or sand bed the colliders hold: scoured round as on mud (round 20.2).
				if (c.deformable && _on_lattice(at.x, at.z) && at.y - rc < g0 + 0.05f && rb->get_linear_velocity().length() < 0.05f) {
					_loose_scour(Vector3(at.x, g0, at.z), rc, sp, c, ls, dt);
				}
			}
			if (ls.ground.is_valid()) {
				PhysicsServer3D::get_singleton()->body_remove_collision_exception(rb->get_rid(), ls.ground);
				ls.ground = RID();
			}
			ls.ref = NAN;
			continue;
		}
		// The terrain's collider under it (the ground's collision layers): passed through while it lies in the mud.
		if (!ls.ground.is_valid() && rb->get_world_3d().is_valid()) {
			if (PhysicsDirectSpaceState3D *space = rb->get_world_3d()->get_direct_space_state()) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = Vector3(at.x, g0 + 2.0f, at.z);
				rp.to = Vector3(at.x, g0 - 2.0f, at.z);
				rp.collision_mask = ground_collision_mask;
				rp.exclude.insert(rb->get_rid());
				PhysicsDirectSpaceState3D::RayResult rr;
				if (space->intersect_ray(rp, rr) && rr.rid.is_valid()) {
					ls.ground = rr.rid;
					PhysicsServer3D::get_singleton()->body_add_collision_exception(rb->get_rid(), ls.ground);
				}
			}
		}
		Vector3 v = rb->get_linear_velocity();
		Vector2 vh(v.x, v.z);
		const float base = g0 - c.max_dent; // the firm base under the layer
		// Liquid mud flowing carries it too: the layer's depth-averaged speed from the same momentum balance as the
		// GPU's mud waves (mud_face() in ground_deform.glsl) held steady, a Bingham sheet's
		// q = h^2 / (3 nu) (g h S - tau_y / rho) down the surface's slope S (Liu and Mei 1989; Coussot 1997).
		if (c.wave_nu > 0.0f) {
			const float e = MAX(1.5f * rc, 0.25f);
			const Vector2 sl((get_surface_height_at(at + Vector3(e, 0, 0)) - get_surface_height_at(at - Vector3(e, 0, 0))) / (2.0f * e),
					(get_surface_height_at(at + Vector3(0, 0, e)) - get_surface_height_at(at - Vector3(0, 0, e))) / (2.0f * e));
			const float s_len = sl.length();
			const float surf = get_surface_height_at(at);
			const float h = surf - base;
			const float drive = 9.81f * h * s_len - c.wave_tau;
			if (h > 0.02f && s_len > 1e-4f && drive > 0.0f) {
				const float u = MIN(h * drive / (3.0f * c.wave_nu), 3.0f);
				const float wet = CLAMP(surf - (at.y - rc), 0.0f, 2.0f * rc);
				const float rho_l = c.unit_weight * 1000.0f / 9.81f;
				carry += _loose_carry(-sl / s_len * u - vh, wet, rc, rho_l, c.wave_tau * rho_l, rb->get_mass(), dt);
			}
		}
		if (sp.wet) {
			// The water's drag on its face above the mud, never more than brings it to the water's speed there.
			const float fl = sp.drag.length();
			const Vector2 rel = sp.near_bed - vh;
			if (fl > 0.0f) {
				carry += sp.drag / fl * MIN(fl * dt / rb->get_mass(), rel.length());
			}
		}
		vh += carry;
		const float speed = vh.length();
		const float rho_mud = c.unit_weight * 1000.0f / 9.81f;
		const float tau = c.wave_tau > 0.0f ? c.wave_tau * rho_mud : c.cohesion * 1000.0f;
		bool held = true;
		// Its weight less the water's buoyancy and lift on it, and a tyre driving it down.
		const float load = MAX(rb->get_mass() - sp.up / 9.81f, 0.1f * rb->get_mass()) + ls.push_down / 9.81f;
		ls.push_down = 0.0f;
		const float z_eq = _loose_settled_depth(rc, load, MAX(tau, 1.0f), rho_mud, held);
		const bool moving = speed > 0.15f;
		// The undisturbed mud it rides: ahead while moving, round it at rest (outside its own print).
		float ref = 1e9f;
		if (moving) {
			const Vector2 q = Vector2(at.x, at.z) + vh / speed * (1.1f * rc);
			ref = _ground_at(q.x, q.y) - get_dent_at(Vector3(q.x, 0.0f, q.y));
			ls.z += (z_eq - ls.z) * MIN(dt / 0.1f, 1.0f); // pressed in, or lifted to it, as it goes
		} else {
			for (int k = 0; k < 4; k++) {
				const float ang = float(k) * Math::PI * 0.5f;
				const Vector2 q = Vector2(at.x, at.z) + Vector2(Math::cos(ang), Math::sin(ang)) * (1.3f * rc);
				ref = MIN(ref, _ground_at(q.x, q.y) - get_dent_at(Vector3(q.x, 0.0f, q.y)));
			}
			// Creeping down to its settled depth (or the base, past the criterion).
			const float target = held ? z_eq : 1e3f;
			if (ls.z < target) {
				const float mu = c.wave_nu > 0.0f ? c.wave_nu * rho_mud : 50.0f; // Pa s
				const float stokes = (2650.0f - rho_mud) * 9.81f * 4.0f * rc * rc / (18.0f * MAX(mu, 1e-2f));
				ls.z = MIN(ls.z + MIN(stokes, 0.05f) * dt, target);
			}
			// Pressed deeper (a tyre on it, round 18.2) it stays: the mud does not push it back up.
		}
		if (!Math::is_nan(ls.ref) && !moving) {
			ref = MIN(ref, ls.ref); // at rest it does not climb: mud laid round it buries it
		}
		ls.ref = ref;
		if (ls.cap_frame + 1 >= Engine::get_singleton()->get_physics_frames()) {
			// A tyre on it: its top no higher than the tyre's floor.
			ls.z = MAX(ls.z, ref - (ls.cap - r - rc));
		}
		const float bottom = MAX(ref - ls.z, base);
		const float ys = bottom + rc;
		if (at.y > ys + 0.005f) {
			continue; // falling onto it
		}
		// Held up at its depth in the mud.
		at.y = ys;
		Transform3D xf = rb->get_global_transform();
		xf.origin = at;
		rb->set_global_transform(xf);
		v.y = MAX(v.y, 0.0f);
		// Down the slope of the mud it lies on, and braked by it.
		const float e = 0.25f;
		const Vector2 grad((_ground_at(at.x + e, at.z) - _ground_at(at.x - e, at.z)) / (2.0f * e), (_ground_at(at.x, at.z + e) - _ground_at(at.x, at.z - e)) / (2.0f * e));
		vh -= grad * (9.81f * dt / (1.0f + grad.length_squared()));
		const float sp2 = vh.length();
		if (sp2 > 1e-4f) {
			const float h = MIN(ls.z, 2.0f * rc);
			const float kp = Math::pow(Math::tan(Math::PI * 0.25f + 0.5f * Math::atan(c.tan_phi)), 2.0f);
			const float plough = 2.0f * rc * (0.5f * kp * c.unit_weight * 1000.0f * h * h + 2.0f * MIN(tau, c.cohesion * 1000.0f) * h * Math::sqrt(kp));
			const float roll = load * 9.81f * Math::sqrt(MAX(h, 0.002f) / (2.0f * rc));
			const float dv = MIN((plough + roll) * dt / rb->get_mass(), sp2);
			vh -= vh / sp2 * dv;
		}
		rb->set_linear_velocity(Vector3(vh.x, v.y, vh.y));
		if (vh.length() > 0.02f) {
			rb->set_angular_velocity(Vector3(vh.y, 0.0f, -vh.x) / rc);
		} else {
			rb->set_angular_velocity(Vector3());
		}
		if (moving && bottom < ref) {
			float ploughed = _press_moving(Vector3(at.x, ref, at.z), 0.9f * rc, bottom, ref - bottom, vh);
			if (ploughed <= 0.0f) {
				// The GPU presses it unread: the furrow's section (its width by its depth, about a segment's 0.6 of the
				// rectangle) over the step's travel.
				ploughed = 1.8f * rc * (ref - bottom) * 0.6f * vh.length() * dt;
			}
			// Under water the bow wave's fines go up as a cloud the current carries off (round 20.3): the mud the
			// stone shoves aside is churned into the water at its face: of its fines, a twentieth to three tenths
			// stays up as the bed shear rises (as the ford's plume, _ford()), the rest settling into the furrow's rims.
			if (sp.wet && ploughed > 0.0f) {
				const float tau_b = 1000.0f * 0.003f * sp.flow.length_squared();
				const float share = (0.05f + 0.25f * Math::smoothstep(0.5f, 3.0f, tau_b)) * CLAMP(1.0f - c.sand, 0.0f, 1.0f);
				const float kg = ploughed * 1300.0f * share;
				if (kg > 0.0f) {
					ww->add_sediment_mass(Vector3(at.x, ref + MIN(0.5f * sp.depth, 2.0f * rc), at.z), kg, 0.0f);
					stone_plume_kg += kg;
				}
			}
		} else if (sp.wet && !moving) {
			_loose_scour(Vector3(at.x, ref, at.z), rc, sp, c, ls, dt);
		}
		// Out of sight: sunk, or buried under what was laid over it.
		if (at.y + r < get_surface_height_at(at) - 0.03f) {
			hidden.push_back(kv.key);
		}
	}
	for (uint32_t id : hidden) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(buried_loose_bodies[id]));
		float rc = 0.1f;
		if (rb) {
			for (int ci = 0; ci < rb->get_child_count(); ci++) {
				if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(rb->get_child(ci))) {
					if (Ref<SphereShape3D> sp = cs->get_shape(); sp.is_valid()) {
						rc = sp->get_radius();
					}
				}
			}
		}
		_loose_rebury(id, rb, rc / 0.85f);
	}
}

int GroundSurface3D::get_reburied_stone_count() const {
	int n = 0;
	for (const KeyValue<Vector2i, LocalVector<Buried>> &kv : buried_extra) {
		n += kv.value.size();
	}
	return n;
}

// Hidden again: the body goes, and a stone where it lies joins the hidden list (kept in deposits.dep).
void GroundSurface3D::_loose_rebury(uint32_t p_id, RigidBody3D *p_body, float p_r) {
	if (p_body) {
		Buried b;
		b.a = p_body->get_global_position();
		b.b = b.a;
		b.r = p_r;
		b.id = _buried_hash(p_id, 99u) | 1u;
		const Vector2i cell(int(Math::floor(b.a.x / BURIED_CELL)), int(Math::floor(b.a.z / BURIED_CELL)));
		buried_extra[cell].push_back(b);
		if (LocalVector<Buried> *got = buried_cache.getptr(cell)) {
			got->push_back(b);
		}
		p_body->queue_free();
		loose_reburied++;
	}
	buried_loose_bodies.erase(p_id);
	loose_state.erase(p_id);
	gpu_cells_dirty = true;
	near_cells_dirty = true;
}

void GroundSurface3D::set_buried_kick(bool p_enabled) {
	buried_kick = p_enabled;
}

// Near the followed point: a StaticBody3D for each hidden stone and root within buried_body_range, freed past it.
void GroundSurface3D::_buried_update() {
	if (!buried_obstacles || buried_density <= 0.0f || !ground_valid || !is_inside_tree()) {
		if (!buried_bodies.is_empty()) {
			_buried_clear_bodies();
		}
		return;
	}
	const Vector3 c = _follow_position();
	// Loose stones stay rigid bodies until well out of range.
	{
		LocalVector<uint32_t> far;
		for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
			Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(kv.value));
			if (!n) {
				far.push_back(kv.key);
			} else if (n->get_global_position().distance_to(c) > 2.0f * buried_body_range) {
				n->queue_free();
				far.push_back(kv.key);
			}
		}
		for (uint32_t id : far) {
			buried_loose_bodies.erase(id);
			loose_state.erase(id);
		}
	}
	const Array near = get_buried_near(c, buried_body_range);
	bool changed = false;
	HashSet<uint32_t> keep;
	for (int i = 0; i < near.size(); i++) {
		const Dictionary d = near[i];
		const uint32_t id = uint32_t(int64_t(d["id"]));
		keep.insert(id);
		if (buried_bodies.has(id)) {
			continue;
		}
		const Vector3 a = d["a"];
		const Vector3 b = d["b"];
		const float r = d["radius"];
		const bool root = d["root"];
		StaticBody3D *body = memnew(StaticBody3D);
		body->set_name(vformat("Buried%d", int64_t(id)));
		CollisionShape3D *cs = memnew(CollisionShape3D);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		Transform3D xf;
		if (root) {
			const Vector3 ax = b - a;
			const float len = ax.length();
			Ref<CapsuleShape3D> cap;
			cap.instantiate();
			cap->set_radius(r);
			cap->set_height(len + 2.0f * r);
			cs->set_shape(cap);
			const Vector3 x = ax / MAX(len, 1e-4f);
			const Vector3 y = Math::abs(x.y) < 0.99f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
			const Vector3 z = x.cross(y).normalized();
			const Basis basis(x, z.cross(x), z);
			xf = Transform3D(basis, (a + b) * 0.5f);
			// The capsule's axis is its local y: lay it along x.
			cs->set_transform(Transform3D(Basis(Vector3(0, 0, 1), -Math::PI * 0.5f), Vector3()));
			mi->set_transform(Transform3D(Basis().scaled(Vector3(len, r, r)), Vector3()));
		} else {
			Ref<SphereShape3D> sp;
			sp.instantiate();
			sp->set_radius(r * 0.85f);
			cs->set_shape(sp);
			xf = Transform3D(Basis(Vector3(0, 1, 0), float(id % 628u) * 0.01f), a);
			mi->set_transform(Transform3D(Basis().scaled(Vector3(r, r, r)), Vector3()));
		}
		mi->set_mesh(_buried_mesh(root, id >> 1));
		body->add_child(cs);
		body->add_child(mi);
		body->add_to_group(SNAME("ground_buried"));
		add_child(body, false, INTERNAL_MODE_BACK);
		body->set_global_transform(xf);
		buried_bodies[id] = body->get_instance_id();
		changed = true;
	}
	LocalVector<uint32_t> gone;
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_bodies) {
		if (!keep.has(kv.key)) {
			gone.push_back(kv.key);
		}
	}
	for (uint32_t id : gone) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(buried_bodies[id]))) {
			n->queue_free();
		}
		buried_bodies.erase(id);
		changed = true;
	}
	if (changed) {
		// The GPU's slumping and mud waves keep to the stones near the followed point (round 16.1).
		gpu_cells_dirty = true;
		near_cells_dirty = true;
	}
}

void GroundSurface3D::set_buried_obstacles(bool p_enabled) {
	buried_obstacles = p_enabled;
	buried_cache.clear();
	gpu_cells_dirty = true;
	near_cells_dirty = true;
	if (!p_enabled) {
		for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
			if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(kv.value))) {
				n->queue_free();
			}
		}
		buried_loose_bodies.clear();
		buried_loose.clear();
		buried_kicks.clear();
	}
	_buried_update();
}

void GroundSurface3D::set_buried_density(float p_density) {
	buried_density = MAX(p_density, 0.0f);
	buried_cache.clear();
	_buried_clear_bodies();
	_buried_update();
}

void GroundSurface3D::set_worn_tracks(bool p_enabled) {
	if (worn_tracks == p_enabled) {
		return;
	}
	worn_tracks = p_enabled;
	if (!worn_tracks && wear_texture.is_valid() && active == this) {
		_set_global("ground_wear_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
		wear_origin = Vector2i(INT32_MAX, INT32_MAX);
	}
	wear_dirty = true;
	_update_cells();
}

void GroundSurface3D::set_wear_regrowth_days(float p_days) {
	wear_regrowth_days = MAX(p_days, 0.0f);
}

void GroundSurface3D::set_mud_waves(bool p_enabled) {
	if (mud_waves == p_enabled) {
		return;
	}
	mud_waves = p_enabled;
	_update_cells();
}

void GroundSurface3D::set_mud_slides(bool p_enabled) {
	mud_slides = p_enabled;
	_update_cells();
}

void GroundSurface3D::set_subsoil(bool p_enabled) {
	subsoil = p_enabled;
	_update_cells();
}

float GroundSurface3D::get_subsoil_depth_at(Vector3 p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	return c.deformable ? c.max_dent : 0.0f;
}

float GroundSurface3D::get_mud_slide_angle_at(Vector3 p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	if (!mud_slides || c.mud <= 0.0f || c.slide_length >= 1e2f) {
		return 90.0f;
	}
	const float left = MAX(c.max_dent - get_dent_at(p_position) + get_rim_at(p_position), 0.005f);
	return Math::rad_to_deg(Math::atan(c.slide_length / left));
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

WorldGroundSoil GroundSurface3D::contract_ground_soil_at(const Vector3 &p_position) const {
	const GroundSoil g = get_soil(p_position);
	WorldGroundSoil s;
	s.soft = g.soft;
	s.cohesion = g.cohesion;
	s.friction_angle = Math::rad_to_deg(Math::atan(g.tan_phi));
	s.stickiness = g.stickiness;
	return s;
}

float GroundSurface3D::contract_water_height_at(const Vector3 &p_position) const {
	const float water = get_standing_water_at(p_position);
	return water > 0.002f ? get_surface_height_at(p_position) + water : -Math::INF;
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
	// Frozen: its pores are ice (meltwater and rain stand on frozen ground).
	k *= 1.0f - 0.99f * CLAMP(p_in.frozen, 0.0f, 1.0f);
	// Burnt water-repellent soil sheds rain: infiltration down to about a tenth on a fresh severe burn (Robichaud 2000;
	// Doerr et al. 2000).
	k *= 1.0f - 0.9f * p_in.repel;
	// A packed wheel track takes in a fraction of the water the loose soil beside it does (Ankeny et al. 1990), so
	// rain stands in worn tracks.
	k *= 1.0f - 0.8f * p_in.compacted;
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
	const WorldWeather *wm = _weather();
	if (!wm) {
		return 2e-8f; // a mild day's, about 2 mm a day
	}
	const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
	if (wm->precipitation_at(at) > 0.1f) {
		return 0.0f;
	}
	const float t = wm->air_temperature_at(_weather_point());
	const float es = 6.112f * Math::exp(17.62f * t / (243.12f + t));
	const float deficit = MAX(es * (1.0f - CLAMP(wm->air_humidity_at(_weather_point()), 0.0f, 1.0f)), 0.0f);
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
		if (!desiccation_cracks) {
			silt_wet_new.clear(); // no drying: the skin reads as soaked (_silt_water()) until it is on
		}
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
		const float wx = (ground_origin.x + i % n) * ground_spacing;
		const float wz = (ground_origin.y + i / n) * ground_spacing;
		// Only clay-rich bare ground cracks: not sand, litter, rock or snow; a silt skin over any of them does.
		const float silt = _silt_at(wx, wz);
		if (in.fixed || ((in.sand > 0.5f || in.litter > 0.01f) && silt <= 0.0f)) {
			st = 0.0f;
			pool = 0.0f;
			continue;
		}
		const float standing = standing_water ? get_standing_water_at(Vector3(wx, 0.0f, wz)) : 0.0f;
		// New silt soaks the skin; weighted between the 1 m cells' centres, so a fresh lobe's wet edge is not stepped.
		float laid = 0.0f;
		if (silt > 0.0f && !silt_wet_new.is_empty()) {
			const float fx = wx - 0.5f;
			const float fz = wz - 0.5f;
			const int x0 = int(Math::floor(fx));
			const int z0 = int(Math::floor(fz));
			const float tx = fx - float(x0);
			const float tz = fz - float(z0);
			auto has = [&](int x, int z) { return silt_wet_new.has(Vector2i(x, z)) ? 1.0f : 0.0f; };
			laid = Math::lerp(Math::lerp(has(x0, z0), has(x0 + 1, z0), tx), Math::lerp(has(x0, z0 + 1), has(x0 + 1, z0 + 1), tx), tz);
		}
		if (laid > 0.0f && laid < 0.999f && !((pores && pore[i] >= 0.95f) || standing > 0.001f)) {
			st = MAX(st, 1.0f + laid); // partly wet: dries on from there
			continue;
		}
		const bool soaked = (pores && pore[i] >= 0.95f) || standing > 0.001f || laid >= 0.999f;
		if (soaked) {
			if (st > 0.0f && st < 1.7f) {
				cycles = MIN(cycles + 1.0f, 10.0f); // it had cracked: a cycle
				pool = 0.0f; // a new pool lays a new layer
			}
			st = 2.0f;
			pool = MAX(pool, standing);
			continue;
		}
		// A silt skin dries from its own top whatever the soil under it (its water is its own).
		if (st <= 0.0f || (in.moisture >= mud_wet_start && silt <= 0.0f)) {
			continue; // never soaked, or the ground round it is still wet
		}
		const float thick = _crack_layer(i);
		st = MAX(st - evap * float(p_delta) / (0.4f * MAX(thick, 0.002f)), 1.0f);
	}
	// The new silt's soaking is taken (the drying runs on from here).
	silt_wet_new.clear();
}

void GroundSurface3D::set_puddle_ice(bool p_enabled) {
	puddle_ice = p_enabled;
	if (!p_enabled) {
		ice_state.clear();
	}
}

// Puddle ice (docs/ideas/mud.md, round 7). Standing water below freezing grows a sheet of ice by Stefan's law, the
// latent heat of the new ice conducted up through the ice already there: dh/dt = k dT / (rho L h), k 2.2 W/(m K),
// rho 917 kg/m3, L 334 kJ/kg, with Ashton's (1989) factor of 0.6 for the air's own resistance (and 5 mm added to h
// so the first film does not grow without bound). A night at -5 C makes some 4 cm. Above freezing it melts by
// degree days, some 7 mm a day per degree (8.1e-8 m/s/C). It never grows thicker than the water. Plates broken by
// a wheel or a foot (_ice_break()) freeze back together as the new ice between them grows: the broken share
// shrinks as the ice it would take to bind them (a few millimetres) forms.
void GroundSurface3D::_ice_update(double p_delta) {
	const int n = ground_n;
	if (!puddle_ice || !standing_water || !ground_valid || n < 2) {
		ice_state.clear();
		return;
	}
	const WorldWeather *wm = _weather();
	const float t = wm ? wm->air_temperature_at(_weather_point()) : 10.0f;
	if ((int)ice_state.size() != n * n * ICE_FLOATS) {
		if (t >= 0.0f) {
			return; // nothing frozen, nothing to freeze
		}
		ice_state.resize(n * n * ICE_FLOATS);
		for (float &v : ice_state) {
			v = 0.0f;
		}
	}
	const float dt = float(p_delta) * runoff_time_scale;
	bool any = false;
	for (int i = 0; i < n * n; i++) {
		float &h = ice_state[i * ICE_FLOATS + 0];
		float &b = ice_state[i * ICE_FLOATS + 1];
		const float wx = (ground_origin.x + i % n) * ground_spacing;
		const float wz = (ground_origin.y + i / n) * ground_spacing;
		const float water = get_standing_water_at(Vector3(wx, 0.0f, wz));
		if (water < 0.0005f) {
			h = 0.0f; // drained or dried out: the ice goes with it
			b = 0.0f;
			continue;
		}
		if (t < 0.0f) {
			const float dh = 0.6f * 7.2e-9f * -t / (h + 0.005f) * dt;
			h += dh;
			b = MAX(b - dh / 0.003f, 0.0f);
		} else {
			h -= 8.1e-8f * t * dt;
		}
		h = CLAMP(h, 0.0f, water);
		if (h <= 0.0f) {
			b = 0.0f;
		}
		any = any || h > 0.0f;
	}
	if (!any && t >= 0.0f) {
		ice_state.clear();
	}
}

// A load on the ice at p_position, over p_radius: ice too thin to bear it breaks into plates. Gold's (1971) bearing
// law P = A h^2 (A about 1 MN/m2 for short loads): the thickness p_bearing that bears the load, 3 cm for a person,
// 6 cm for a car's wheel.
void GroundSurface3D::_ice_break(const Vector3 &p_position, float p_radius, float p_bearing) {
	const int n = ground_n;
	if (ice_state.is_empty() || (int)ice_state.size() != n * n * ICE_FLOATS) {
		return;
	}
	const float r = MAX(p_radius, 0.5f * ground_spacing) + ground_spacing;
	const int x0 = MAX(int(Math::floor((p_position.x - r) / ground_spacing)) - ground_origin.x, 0);
	const int x1 = MIN(int(Math::ceil((p_position.x + r) / ground_spacing)) - ground_origin.x, n - 1);
	const int z0 = MAX(int(Math::floor((p_position.z - r) / ground_spacing)) - ground_origin.y, 0);
	const int z1 = MIN(int(Math::ceil((p_position.z + r) / ground_spacing)) - ground_origin.y, n - 1);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			float *c = &ice_state[(z * n + x) * ICE_FLOATS];
			if (c[0] <= 0.0f || c[0] >= p_bearing) {
				continue;
			}
			const float d = Vector2((ground_origin.x + x) * ground_spacing - p_position.x, (ground_origin.y + z) * ground_spacing - p_position.z).length();
			// Broken under the load; the plates round it crack too, less the farther they are.
			c[1] = MAX(c[1], 1.0f - Math::smoothstep(p_radius, r, d));
		}
	}
}

void GroundSurface3D::set_frozen_ground(bool p_enabled) {
	frozen_ground = p_enabled;
	if (!p_enabled) {
		frost_state.clear();
	}
	cells_accum = 2.0; // the cells take it at the next step
}

// Frozen ground (docs/ideas/mud.md, round 8). The frost front goes down by Stefan's solution with the soil's water
// as the latent heat (the modified Berggren equation's leading term, Aldrich 1956): dz/dt = k dT / (L theta (z +
// z0)), frozen soil's conductivity k 2 W/(m K), L 334 MJ/m3 of water, theta the soil's water content (0.1 to 0.4 by
// its wetness), z0 2 cm for the air and the surface's own resistance; about 9 cm in a night at -5 C, half a metre
// over a cold winter. A thaw works down from the top the same way (unfrozen k 1.2), so for a while a thawed layer
// lies on the frozen ground: its meltwater cannot drain through the ice below, the layer is saturated and weak
// (thaw weakening: forest roads lose about half their bearing in the spring thaw and carry load limits) until the
// frost has gone and it has drained, an e-fold of 5 days. A frost after a thaw refreezes the wet top first. Snow
// (a cell the snow covers) insulates: no change under it. New cells start from the climate's frost index for the
// day (WeatherSeason's soil_frost: freezing degree days, thawed back by positive ones, both shielded by snow: frost made before the pack stays under it).
void GroundSurface3D::_frost_seed(int p_from) {
	const WorldWeather *wm = _weather();
	float depth = 0.0f;
	if (wm) {
		const WeatherSeason::State st = WeatherSeason::compute(_weather_climate(wm), _ground_at(center.x, center.y), wm->day_of_year());
		const float theta = 0.1f + 0.3f * CLAMP(moisture >= 0.0f ? moisture : 0.3f, 0.0f, 1.0f);
		// Under a pack too: the frost index already counts the snow's shield, and frost made before the snow lies
		// on under it until spring (Kane and Stein 1983).
		depth = MIN(Math::sqrt(2.0f * 2.0f * st.soil_frost * 86400.0f / (334e6f * theta)), 1.0f);
	}
	for (int i = p_from; i < (int)frost_state.size() / FROST_FLOATS; i++) {
		float *f = &frost_state[i * FROST_FLOATS];
		if (f[0] < 0.0f) {
			f[0] = 0.0f;
			f[1] = depth;
			f[2] = 0.0f;
			f[3] = 0.0f;
			f[4] = 0.0f;
			f[5] = 0.0f;
		}
	}
}

void GroundSurface3D::_frost_update(double p_delta) {
	const int n = ground_n;
	if (!frozen_ground || !ground_valid || n < 2 || (int)cell_inputs.size() != n * n) {
		frost_state.clear();
		return;
	}
	if ((int)frost_state.size() != n * n * FROST_FLOATS) {
		frost_state.resize(n * n * FROST_FLOATS);
		for (int i = 0; i < n * n; i++) {
			frost_state[i * FROST_FLOATS] = -1.0f;
		}
	}
	_frost_seed(0);
	const WorldWeather *wm = _weather();
	const float t = wm ? wm->air_temperature_at(_weather_point()) : 10.0f;
	const float dt = float(p_delta) * runoff_time_scale;
	for (int i = 0; i < n * n; i++) {
		const CellInput &in = cell_inputs[i];
		float *f = &frost_state[i * FROST_FLOATS];
		float &a = f[0];
		float &b = f[1];
		float &w = f[2];
		float &nd = f[3];
		float &loose = f[4];
		if (in.fixed) {
			continue; // rock, or snow over it
		}
		const float l = 334e6f * (0.1f + 0.3f * CLAMP(in.moisture, 0.0f, 1.0f));
		// Needle ice (round 9): on wet, bare, fine soil whose frost front sits at the surface, a frosty night grows
		// ice needles instead of driving the front down; the latent heat of the water drawn up to it holds the front
		// there (Outcalt 1971; Lawler 1988). Stefan through the needle mat (about half ice, k 1 W/(m K)): about 2 cm
		// in a night at -3 C. Too cold (below about -8 C) the front runs down as concrete frost instead.
		const float needle = needle_ice && a <= 0.0f && b < 0.01f && in.sand < 0.5f && in.litter < 0.005f ? Math::smoothstep(0.25f, 0.4f, in.moisture) * Math::smoothstep(0.0f, 1.5f, -t) * (1.0f - Math::smoothstep(6.0f, 10.0f, -t)) : 0.0f;
		if (t < 0.0f) {
			if (a > 0.0f) {
				// The wet top refreezes first, under a crust as thin as it is.
				a = MAX(a - 2.0f * -t / (l * 0.03f) * dt, 0.0f);
			} else {
				// The soil's own patchiness (its water, its silt) over a metre or two: no field of equal needles.
				const float cx = (ground_origin.x + i % n) * ground_spacing;
				const float cz = (ground_origin.y + i / n) * ground_spacing;
				const float patch = 0.5f + 1.0f * (0.65f * _value_noise(cx / 1.7f, cz / 1.7f) + 0.35f * _value_noise(cx / 0.6f + 17.0f, cz / 0.6f + 5.0f));
				nd = MIN(nd + needle * patch * -t / (167e6f * (nd + 0.02f)) * dt, 0.08f);
				b = MIN(b + (1.0f - needle) * 2.0f * -t / (l * (b + 0.02f)) * dt, 1.0f);
			}
		} else if (nd > 0.0f) {
			// A thaw melts the needles from the top and the soil cap they lifted collapses into loose, fluffy crumbs
			// that print easily and wash away (Lawler 1986's needle ice erosion of river banks).
			loose = MAX(loose, MIN(nd / 0.02f, 1.0f));
			nd = MAX(nd - 1.2f * t / (167e6f * 0.02f) * dt, 0.0f);
		}
		if (loose > 0.0f && nd <= 0.0f) {
			// It settles back over about a week (and a print compacts it at once).
			loose *= Math::exp(-dt / (7.0f * 86400.0f));
			loose = loose < 0.01f ? 0.0f : loose;
		}
		if (t >= 0.0f && b > 0.0f) {
			a += 1.2f * t / (l * (a + 0.02f)) * dt;
			if (a >= b) {
				a = 0.0f;
				b = 0.0f;
				w = 1.0f; // thawed through, still saturated
			}
		}
		if (a > 0.0f && b > a) {
			w = 1.0f; // meltwater perched on the frost
		} else if (b <= 0.0f && w > 0.0f) {
			w *= Math::exp(-dt / (5.0f * 86400.0f));
			w = w < 0.01f ? 0.0f : w;
		}
	}
	// The needles lift the ground: laid on the field (and taken off as they melt) in steps of 2 mm, at most 8192
	// cells a step, taking turns round the lattice (a whole field grows together). Each cell's share is a bilinear
	// tent a lattice step wide, so a field of equal needles rises flat. A foot or wheel on them presses the soft mat
	// (the cells' weak needles let it sink deeper) and the press's own print takes the crushed cap.
	if (!field.valid) {
		return;
	}
	const int nn = n * n;
	int writes = 0;
	field.amount_tent = true;
	for (int k = 0; k < nn && writes < 8192; k++) {
		const int i = (needle_cursor + k) % nn;
		float *f = &frost_state[i * FROST_FLOATS];
		if (Math::abs(f[3] - f[5]) >= 0.002f || (f[3] <= 0.0f && f[5] > 0.0f)) {
			const Vector3 at((ground_origin.x + i % n) * ground_spacing, ground[i], (ground_origin.y + i / n) * ground_spacing);
			_amount(at, ground_spacing, -(f[3] - f[5]), ground_spacing);
			f[5] = f[3];
			writes++;
			needle_cursor = (i + 1) % nn;
		}
	}
	field.amount_tent = false;
}

// How far the frozen ground holds as a crust at lattice cell p_i, 0..1: a frozen layer from 2 cm thick (a boot
// breaks thinner) to 8 cm (a wheel does not), at the surface (a thawed top over it gives way).
float GroundSurface3D::_frost_crust(int p_i) const {
	if ((int)frost_state.size() != ground_n * ground_n * FROST_FLOATS || p_i < 0) {
		return 0.0f;
	}
	const float a = MAX(frost_state[p_i * FROST_FLOATS], 0.0f);
	const float b = frost_state[p_i * FROST_FLOATS + 1];
	return Math::smoothstep(0.02f, 0.08f, b - a) * (1.0f - Math::smoothstep(0.005f, 0.03f, a));
}

Vector3 GroundSurface3D::get_frost_at(Vector3 p_position) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)frost_state.size() != n * n * FROST_FLOATS) {
		return Vector3();
	}
	const int x = CLAMP(int(Math::round(p_position.x / ground_spacing)) - ground_origin.x, 0, n - 1);
	const int z = CLAMP(int(Math::round(p_position.z / ground_spacing)) - ground_origin.y, 0, n - 1);
	const float *f = &frost_state[(z * n + x) * FROST_FLOATS];
	return Vector3(MAX(f[0], 0.0f), MAX(f[1], 0.0f), f[2]);
}

void GroundSurface3D::set_needle_ice(bool p_enabled) {
	needle_ice = p_enabled;
	cells_accum = 2.0;
}

// The needle ice's height (m) and the loose soil a thaw left (0..1) at a place.
Vector2 GroundSurface3D::get_needle_ice_at(Vector3 p_position) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)frost_state.size() != n * n * FROST_FLOATS) {
		return Vector2();
	}
	const int x = CLAMP(int(Math::round(p_position.x / ground_spacing)) - ground_origin.x, 0, n - 1);
	const int z = CLAMP(int(Math::round(p_position.z / ground_spacing)) - ground_origin.y, 0, n - 1);
	const float *f = &frost_state[(z * n + x) * FROST_FLOATS];
	return Vector2(f[3], f[4]);
}

float GroundSurface3D::_needle_loose_at(float p_x, float p_z) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)frost_state.size() != n * n * FROST_FLOATS) {
		return 0.0f;
	}
	const int x = int(Math::round(p_x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? frost_state[(z * n + x) * FROST_FLOATS + 4] : 0.0f;
}

void GroundSurface3D::set_plant_mud(bool p_enabled) {
	plant_mud = p_enabled;
	if (!p_enabled) {
		plant_mud_state.clear();
	}
}

// Mud on plants beside ruts (docs/ideas/mud.md, round 7). Clay thrown by a wheel or splashed out of a muddy puddle
// lands on the sward and the plants as well as on the ground: p_share of their faces over p_radius round
// p_position is spattered (on top of what is there). Rain washes it off: simulated-rain tests on leaves (Xu et al.
// 2017; lawn grass, 2025) find most of a particle deposit goes early in the rain, so here, coarse loose clay, at an
// e-fold of 2 mm of rain.
void GroundSurface3D::_plant_mud_add(const Vector3 &p_position, float p_radius, float p_share) {
	const int n = ground_n;
	if (!plant_mud || !ground_valid || n < 2 || !(p_share > 0.0f)) {
		return;
	}
	if ((int)plant_mud_state.size() != n * n) {
		plant_mud_state.resize(n * n);
		for (float &v : plant_mud_state) {
			v = 0.0f;
		}
	}
	const float r = MAX(p_radius, 0.5f * ground_spacing);
	const int x0 = MAX(int(Math::floor((p_position.x - r) / ground_spacing)) - ground_origin.x, 0);
	const int x1 = MIN(int(Math::ceil((p_position.x + r) / ground_spacing)) - ground_origin.x, n - 1);
	const int z0 = MAX(int(Math::floor((p_position.z - r) / ground_spacing)) - ground_origin.y, 0);
	const int z1 = MIN(int(Math::ceil((p_position.z + r) / ground_spacing)) - ground_origin.y, n - 1);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float d = Vector2((ground_origin.x + x) * ground_spacing - p_position.x, (ground_origin.y + z) * ground_spacing - p_position.z).length();
			const float w = 1.0f - Math::smoothstep(0.5f * r, r + 0.5f * ground_spacing, d);
			float &m = plant_mud_state[z * n + x];
			m = MIN(m + p_share * w * (1.0f - m), 1.0f);
		}
	}
}

void GroundSurface3D::_plant_mud_update(double p_delta) {
	const int n = ground_n;
	if (plant_mud_state.is_empty()) {
		return;
	}
	if (!plant_mud || (int)plant_mud_state.size() != n * n) {
		plant_mud_state.clear();
		return;
	}
	const WorldWeather *wm = _weather();
	const float rain = wm ? wm->precipitation_at(Vector3(center.x, _ground_at(center.x, center.y), center.y)) : 0.0f; // mm/h
	if (rain <= 0.0f) {
		return;
	}
	const float keep = Math::exp(-rain * float(p_delta) * runoff_time_scale / 3600.0f / 2.0f);
	bool any = false;
	for (float &m : plant_mud_state) {
		m = m * keep < 0.01f ? 0.0f : m * keep;
		any = any || m > 0.0f;
	}
	if (!any) {
		plant_mud_state.clear();
	}
}

float GroundSurface3D::get_plant_mud_at(Vector3 p_position) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)plant_mud_state.size() != n * n) {
		return 0.0f;
	}
	const int x = CLAMP(int(Math::round(p_position.x / ground_spacing)) - ground_origin.x, 0, n - 1);
	const int z = CLAMP(int(Math::round(p_position.z / ground_spacing)) - ground_origin.y, 0, n - 1);
	return plant_mud_state[z * n + x];
}

Vector2 GroundSurface3D::get_ice_at(Vector3 p_position) const {
	const int n = ground_n;
	if (!ground_valid || n < 2 || (int)ice_state.size() != n * n * ICE_FLOATS) {
		return Vector2();
	}
	const int x = CLAMP(int(Math::round(p_position.x / ground_spacing)) - ground_origin.x, 0, n - 1);
	const int z = CLAMP(int(Math::round(p_position.z / ground_spacing)) - ground_origin.y, 0, n - 1);
	return Vector2(ice_state[(z * n + x) * ICE_FLOATS], ice_state[(z * n + x) * ICE_FLOATS + 1]);
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
	const float layer = CLAMP(0.006f + 0.6f * pool, 0.006f, 0.05f) * clay;
	// A silt skin is all fine grains: it dries and cracks as a layer its own thickness (round 15.1).
	const float silt = MIN(_silt_smooth_at(wx, wz), 0.05f);
	return MAX(layer, silt) * (0.85f + 0.3f * _value_noise(wx / 3.0f, wz / 3.0f));
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
	// A silt skin's own (round 15.1): its tone and cracks show from a millimetre or two.
	const int skin_x = ground_origin.x + p_i % n;
	const int skin_z = ground_origin.y + p_i / n;
	const float skin = _silt_smooth_at(skin_x * ground_spacing, skin_z * ground_spacing);
	const float skin_shows = Math::smoothstep(0.0005f, 0.003f, skin);
	// Its tone, from dark with water (-1, soaked) to the pale crust (1) by the time half the water has gone; the
	// cracks open over the second half of that.
	const float tone = (1.0f - 2.0f * Math::smoothstep(0.5f, 0.95f, water)) * MAX(Math::smoothstep(0.004f, 0.009f, thick), skin_shows);
	if (r_damp) {
		*r_damp = MAX(-tone, 0.0f); // material.cpp takes the larger of this and the pore water's
	}
	// Ground only saturated (a layer of about 6 mm) cracks too finely to show; a pool's bed does, its cracks finer
	// and fading out towards its rim as the water there stood shallower.
	const float shows = MAX(Math::smoothstep(0.006f, 0.012f, thick), skin_shows);
	// A silt skin's plates (docs/ideas/mud.md, round 15.1 look): a thin layer drying on a substrate that does not
	// shrink cracks into polygons some ten times its thickness across (Groisman and Kaplan 1994; Weinberger 1999 on
	// spacing to thickness), their sizes varying from place to place, and each plate curls up at its edges as its top
	// dries and shrinks first (mud curls: Allen 1986; Minter et al. 2007). The shaders take the skin's share as a
	// negative regularity (ground_cracks()): wider gaps, deeper curl reaching further into the plate.
	if (skin_shows > 0.0f) {
		const float vary = 0.6f + 0.9f * _value_noise(skin_x * ground_spacing / 2.3f + 11.0f, skin_z * ground_spacing / 2.3f + 5.0f);
		const float plate = Math::lerp(4.0f * thick, 10.0f * MIN(skin, 0.02f) * vary, skin_shows);
		return Vector4(Math::smoothstep(0.67f, 0.2f, water) * shows, plate, -skin_shows, MAX(tone, 0.0f));
	}
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
	buried_cache.clear(); // the hidden stones lie in the layer (round 14.3)
	_buried_clear_bodies();
	mud_map = p_map;
	_update_cells();
}

void GroundSurface3D::set_mud_map_rect(const Rect2 &p_rect) {
	buried_cache.clear(); // the hidden stones lie in the layer (round 14.3)
	_buried_clear_bodies();
	mud_map_rect = p_rect;
	_update_cells();
}

void GroundSurface3D::set_mud_max_depth(float p_m) {
	buried_cache.clear(); // the hidden stones lie in the layer (round 14.3)
	_buried_clear_bodies();
	mud_max_depth = MAX(p_m, 0.0f);
	_update_cells();
}

void GroundSurface3D::set_mud_auto_depth(float p_m) {
	buried_cache.clear(); // the hidden stones lie in the layer (round 14.3)
	_buried_clear_bodies();
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

// The height source's generator when it has holes under the patch (else null).
const TerrainGenerator3D *GroundSurface3D::_hole_generator() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	const GroundHeightQuery q = GroundHeightQuery::make(this, height_source, ground_collision_mask);
	if (!q.generator || q.generator->get_hole_count() == 0) {
		return nullptr;
	}
	const float half = deform_extent * 0.5f + 1.0f;
	return q.generator->has_holes_in(Rect2(center - Vector2(half, half), Vector2(2.0f * half, 2.0f * half))) ? q.generator : nullptr;
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
	// Openings in the ground (TerrainGenerator3D holes: a cave mouth's passage): the patch leaves out the triangles
	// whose ground passes through one, as Landscape3D leaves out its quads, so no sand is drawn across the opening.
	// The test is in world space, so the mesh is built again when the patch moves near a hole (_update).
	const TerrainGenerator3D *hgen = _hole_generator();
	mesh_hole_count = hgen ? hgen->get_hole_count() : 0;
	mesh_hole_cut = hgen != nullptr;
	const float hole_quad_reach = 0.75f;
	auto cut = [&](int p_a, int p_b, int p_c) -> bool {
		if (!hgen) {
			return false;
		}
		const Vector3 v[3] = { verts[p_a], verts[p_b], verts[p_c] };
		const Vector2 m = center + Vector2(v[0].x + v[1].x + v[2].x, v[0].z + v[1].z + v[2].z) / 3.0f;
		const float span = MAX(MAX((v[1] - v[0]).length(), (v[2] - v[0]).length()), (v[2] - v[1]).length());
		if (!hgen->has_holes_in(Rect2(m, Vector2()).grow(span))) {
			return false;
		}
		// The ground's span over the triangle and over a terrain quad's reach round it (Landscape3D tests whole quads,
		// so where the ground climbs a cliff at a cave mouth its quads cross the passage while a small triangle of
		// beach in front of them may not; the sand must stop where the terrain does).
		real_t lo = 1e30, hi = -1e30;
		auto take = [&](real_t p_x, real_t p_z) {
			const real_t h = hgen->get_height(Vector3(p_x, 0.0f, p_z));
			lo = MIN(lo, h);
			hi = MAX(hi, h);
		};
		for (int k = 0; k < 3; k++) {
			take(center.x + v[k].x, center.y + v[k].z);
		}
		const float reach = MAX(span, hole_quad_reach);
		take(m.x - reach, m.y);
		take(m.x + reach, m.y);
		take(m.x, m.y - reach);
		take(m.x, m.y + reach);
		return hgen->is_hole_span(m.x, m.y, lo, hi);
	};
	LocalVector<int> tri;
	// Wound like the grid's (a, b, d) triangles: the normal of the xz winding points down y.
	auto add_tri = [&](int p_a, int p_b, int p_c) {
		if (cut(p_a, p_b, p_c)) {
			return;
		}
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
				if (!cut(a, b, d)) {
					tri.push_back(a);
					tri.push_back(b);
					tri.push_back(d);
				}
				if (!cut(b, e, d)) {
					tri.push_back(b);
					tri.push_back(e);
					tri.push_back(d);
				}
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
	const ShaderMaterial *src_sm = Object::cast_to<ShaderMaterial>(src.ptr());
	patch_source_shader = src_sm ? src_sm->get_shader() : Ref<Shader>();
	patch_params_stale = false;
	if (BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(patch_material.ptr())) {
		bm->set_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM, true);
		bm->set_ground_deform_patch(true);
		patch_uv_offset = bm->get_uv1_offset();
		// A fresh copy (the source's parameters changed, _sync_patch_params) keeps the world mapping for the patch's
		// current centre; otherwise it would hold the source's offset until the patch next moves.
		const Vector3 sc = bm->get_uv1_scale();
		bm->set_uv1_offset(patch_uv_offset + Vector3(Math::fposmod(center.x * sc.x, 1.0f), Math::fposmod(center.y * sc.y, 1.0f), 0.0f));
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
	if (clods && !clods_node) {
		clods_node = memnew(GroundClods3D);
		add_child(clods_node, false, INTERNAL_MODE_FRONT);
	} else if (!clods && clods_node) {
		clods_node->queue_free();
		clods_node = nullptr;
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
		_ruts_configure();
		_ruts_leave(field, rut_main, want, rut_main_pending);
		_ruts_arrive(field, rut_main, _gpu_on() ? &gpu_host : nullptr, want, rut_main_pending);
		if (_gpu_on()) {
			gpu_host.shift_to(field, want, deform_resolution, max_cached_tiles);
		}
		field.shift(want);
		if (!_gpu_on()) {
			_ruts_read_cpu(field, rut_main, rut_main_pending);
		}
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
	const bool patch_moved = moved || c != center || (mesh_instance && mesh_instance->get_global_position() != Vector3(c.x, 0, c.y));
	if (patch_moved) {
		center = c;
		// Holes are cut in world space: near one (or leaving one, or when the generator's holes change) the
		// patch's triangles are chosen again.
		const TerrainGenerator3D *hg = _hole_generator();
		if (mesh_hole_cut || hg) {
			mesh_dirty = true;
		}
	} else if (!mesh_dirty) {
		const TerrainGenerator3D *hg = _hole_generator();
		if ((hg ? hg->get_hole_count() : 0) != mesh_hole_count) {
			mesh_dirty = true;
		}
	}
	if (patch_moved) {
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
	if (near_fld.valid && _ruts_on()) {
		_ruts_configure();
		_ruts_save_field(near_fld, rut_near, rut_near_pending);
	}
	rut_near_pending.clear();
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
	_ruts_configure();
	_ruts_leave(near_fld, rut_near, want, rut_near_pending);
	_ruts_arrive(near_fld, rut_near, &near_host, want, rut_near_pending);
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
	const int S = GroundRutStore::STRIDE;
	const bool clay = int(field.water.size()) == field.resolution * field.resolution && int(field.water_mud.size()) == field.resolution * field.resolution;
	const bool grown = int(field.regrown.size()) == field.resolution * field.resolution;
	LocalVector<uint32_t> data;
	data.resize(T * T * S);
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = new0 + Vector2i(tx, tz);
			const Vector2i o = w - old0;
			if (o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) {
				continue; // kept from the old near field
			}
			if (near_host.has_cached(w) || rut_near_pending.has(w)) {
				continue; // cached, or coming back from disk (kept ruts)
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
					const float cl = clay ? MAX(field.sample(field.water_mud, wx, wz), 0.0f) * wd : 0.0f;
					const int k = z * T + x;
					data[k * S + 0] = uint32_t(Math::make_half_float(dn)) | (uint32_t(Math::make_half_float(rm)) << 16);
					data[k * S + 1] = uint32_t(Math::make_half_float(fr)) | (uint32_t(Math::make_half_float(wd)) << 16);
					const float rg = grown ? CLAMP(field.sample(field.regrown, wx, wz), 0.0f, 1.0f) : 0.0f;
					data[k * S + 2] = uint32_t(Math::make_half_float(cl)) | (uint32_t(Math::make_half_float(rg)) << 16);
				}
			}
			if (any) {
				near_host.seed_tile(w, data.ptr(), S, max_cached_tiles);
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

// Still shots on a software GPU: the main field at 256 (12.5 cm over 32 m), the near field at 256 (3 cm) and 64
// cached tiles, about a fifth of the GPU work and 300 MB less memory than the shipped 512, 512 and 1024. Off restores
// those.
void GroundSurface3D::set_light_budget(bool p_light) {
	if (p_light == light_budget) {
		return;
	}
	light_budget = p_light;
	set_deform_resolution(p_light ? 256 : 512);
	set_near_resolution(p_light ? 256 : 512);
	set_max_cached_tiles(p_light ? 64 : 1024);
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
					if (f >= 4.0f) {
						// Tracked mud (ground_deform.glsl): wet, dark and glossy; dry, a pale matte film.
						const Vector2 sq = GroundDeformField::mud_band_share_wet(f);
						tint_albedo[i] = Math::lerp(1.0f, Math::lerp(1.12f, 0.55f, sq.y), sq.x);
						tint_roughness[i] = Math::lerp(1.0f, Math::lerp(1.0f, 0.45f, sq.y), sq.x);
						continue;
					}
					if (f >= 2.0f) {
						// A laid band of heavy minerals (2 plus its share): dark, dry or wet (ground_deform.glsl).
						tint_albedo[i] = 1.0f - 0.4f * MIN(f - 2.0f, 1.0f);
						tint_roughness[i] = 1.0f;
						continue;
					}
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
	deform_serial_dirty = true;
	_bump_deform_serial();
}

// Once a frame at most: the patch material's ground_deform_serial (BaseMaterial3D::_ground_deform_code), so the
// path tracer recaptures the patch's displaced vertices after the field's texture is rewritten in place.
// The terrain round the patch (the source material, lowered under it) is bumped too, at most once a second and once
// more after the last change: a recapture of the whole terrain costs far more than the patch's.
void GroundSurface3D::_bump_deform_serial() {
	if (source_serial_dirty && material.is_valid()) {
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		if (now - source_serial_msec >= 1000) {
			source_serial_dirty = false;
			source_serial_msec = now;
			RenderingServer::get_singleton()->material_set_param(material->get_rid(), "ground_deform_serial", float(deform_serial));
		}
	}
	if (!deform_serial_dirty || patch_material.is_null()) {
		return;
	}
	deform_serial_dirty = false;
	source_serial_dirty = true;
	deform_serial = (deform_serial + 1) % 1000000;
	RenderingServer::get_singleton()->material_set_param(patch_material->get_rid(), "ground_deform_serial", float(deform_serial));
}

Vector2 GroundSurface3D::_wind_10m() const {
	if (wind_override != Vector2()) {
		return wind_override;
	}
	const WorldWeather *wm = _weather();
	if (!wm) {
		return Vector2();
	}
	const Vector3 w = wm->wind_at(Vector3(center.x, _ground_at(center.x, center.y), center.y), 10.0f);
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
	self->ripple_grown_k = GroundRipples::dominant_k(self->ripple_grown, self->ripples.n, self->ripples.cell);
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

void GroundSurface3D::_far_update(double p_delta, const LocalVector<Node *> &p_sources, const WorldWeather *p_weather) {
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
			const float contact = p_weather ? p_weather->contact_wetness_at(p) : 0.0f;
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
	const WorldWeather *wm = _weather();
	LocalVector<Node *> sources;
	if (is_inside_tree()) {
		for (int k = 0; k < water_sources.size(); k++) {
			Node *src = get_node_or_null(water_sources[k]);
			if (src) {
				sources.push_back(src);
			}
		}
	}
	// Every other water producer through the water contract too (a river over its banks, live water, a fall's pool,
	// a lake): floods soak the ground they cover and leave their load on it (docs/ideas/world-links.md).
	const WorldWater *ww = WorldContracts::water();
	const bool contract_water = ww && ww->has_provider();
	if (sources.is_empty() && !wm && !contract_water) {
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
			if (contract_water) {
				Vector3 flow;
				Vector2 sed;
				const float h = ww->height_except(p, this, &flow, &sed);
				const float d = Math::is_finite(h) ? h - gh : 0.0f;
				if (d > 0.0f) {
					const float v = Vector2(flow.x, flow.z).length();
					if (d > depth) {
						depth = d;
						speed = v;
					}
					// A river keeps its own record of what settles on its bed: the ground lays that (below) instead.
					if (ww->sediment_deposit_at(p).length_squared() <= 0.0f) {
						_flood_silt(p, d, v, sed, float(p_delta));
					}
				}
				if (flood_silt) {
					const Vector2 dep = ww->sediment_deposit_at(p);
					if (dep.x > 0.0f || dep.y > 0.0f) {
						const Vector2i k(int(Math::floor(p.x)), int(Math::floor(p.z)));
						Vector2 &taken = silt_river_taken[k];
						const Vector2 add(MAX(dep.x - taken.x, 0.0f), MAX(dep.y - taken.y, 0.0f));
						if (add.x + add.y > 1e-3f) { // a micron at a time
							_lay_silt(k, add);
							taken = taken.max(dep);
						}
					}
				}
			}
			g_depth[gz * gn + gx] = depth;
			g_speed[gz * gn + gx] = speed;
			g_contact[gz * gn + gx] = wm ? wm->contact_wetness_at(p) : 0.0f;
		}
	}
	// The capillary fringe round water (water_fringe): the water's level carried 2 m out from the cells it covers,
	// the nearest wet cell's (still water is level, so its shore's level is the pool's), for the dry ground there.
	LocalVector<float> g_level;
	LocalVector<float> g_lspeed; // the speed of the water each level came from
	if (water_fringe) {
		g_level.resize(gn * gn);
		g_lspeed.resize(gn * gn);
		bool any = false;
		for (int gz = 0; gz < gn; gz++) {
			for (int gx = 0; gx < gn; gx++) {
				const int k = gz * gn + gx;
				const float d = g_depth[k];
				const int lx = MIN(gx * step, n - 1);
				const int lz = MIN(gz * step, n - 1);
				g_level[k] = d > 0.002f ? ground[lz * n + lx] + d : NAN;
				g_lspeed[k] = d > 0.002f ? g_speed[k] : 1e9f;
				any = any || d > 0.002f;
			}
		}
		if (!any) {
			g_level.clear();
		} else {
			LocalVector<float> out;
			out.resize(g_level.size());
			for (uint32_t k = 0; k < g_level.size(); k++) {
				out[k] = g_level[k];
			}
			const int R = 2;
			for (int gz = 0; gz < gn; gz++) {
				for (int gx = 0; gx < gn; gx++) {
					if (!Math::is_nan(g_level[gz * gn + gx])) {
						continue;
					}
					int best = R * R + R * R + 1;
					for (int dz = -R; dz <= R; dz++) {
						for (int dx = -R; dx <= R; dx++) {
							const int qx = gx + dx;
							const int qz = gz + dz;
							const int d2 = dx * dx + dz * dz;
							if (qx < 0 || qz < 0 || qx >= gn || qz >= gn || d2 >= best || Math::is_nan(g_level[qz * gn + qx])) {
								continue;
							}
							best = d2;
							out[gz * gn + gx] = g_level[qz * gn + qx];
							g_lspeed[gz * gn + gx] = g_lspeed[qz * gn + qx];
						}
					}
				}
			}
			for (uint32_t k = 0; k < out.size(); k++) {
				g_level[k] = out[k];
			}
		}
	}
	// Salt crusts (salt_crusts): only in a dry climate, where the water drawn up the fringe evaporates and leaves
	// its salts at the top (UNEP aridity under about 0.3).
	const float arid = (salt_crusts && !g_level.is_empty()) ? 1.0f - Math::smoothstep(0.1f, 0.3f, _aridity_index(wm, _ground_at(center.x, center.y))) : 0.0f;
	if ((int)salt_crust.size() != n * n) {
		salt_crust.resize(n * n);
		for (int i = 0; i < n * n; i++) {
			salt_crust[i] = 0.0f;
		}
	}
	const float dt = float(p_delta);
	bool changed = false;
	LocalVector<float> &cell_depth = water_depth;
	cell_depth.resize(n * n);
	water_body_depth.resize(n * n);
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
			water_body_depth[i] = depth;
			if (standing_water) {
				// Standing water in a rut soaks the ground under it (and so softens the mud there).
				depth = MAX(depth, get_standing_water_at(Vector3((ground_origin.x + x) * ground_spacing, 0.0f, (ground_origin.y + z) * ground_spacing)));
			}
			cell_depth[i] = depth;
			const float contact = bil(g_contact);
			float s = _pore_step(pore[i], depth, contact, dt, water_table[i]);
			if (!g_level.is_empty() && depth <= 0.001f) {
				// Capillary rise (Lu and Likos 2004; fine sand about 15 cm, its height c / (e d10)): the ground up to
				// WATER_FRINGE over the water's level stays near saturated at the top, so a still pool's shore shows a
				// band of dark wet sand (wet sand about 35% darker, Lekner and Dorf 1988) fading out above it.
				float lw = 0.0f;
				float lsum = 0.0f;
				const float w4[4] = { (1.0f - tx) * (1.0f - tz), tx * (1.0f - tz), (1.0f - tx) * tz, tx * tz };
				const int k4[4] = { z0 * gn + x0, z0 * gn + x0 + 1, (z0 + 1) * gn + x0, (z0 + 1) * gn + x0 + 1 };
				for (int k = 0; k < 4; k++) {
					if (!Math::is_nan(g_level[k4[k]])) {
						lsum += w4[k] * g_level[k4[k]];
						lw += w4[k];
					}
				}
				float crust = 0.0f;
				if (lw > 0.05f) {
					const float above = ground[i] - lsum / lw;
					if (above < WATER_FRINGE) {
						s = MAX(s, 1.0f - 0.6f * Math::smoothstep(0.0f, WATER_FRINGE, MAX(above, 0.0f)));
					}
					if (arid > 0.0f) {
						// Salt crust (efflorescence): the fringe's water evaporates at its top and leaves its salts
						// there, a pale crust from about 8 cm over the water to some 35 cm, densest round the
						// fringe's top (Rodriguez-Navarro and Doehne 1999; Nield et al. 2015 for crusts by playas
						// and pools). Still water only: a stream's banks are washed.
						float still = 1.0f;
						for (int k = 0; k < 4; k++) {
							if (w4[k] > 0.25f && !Math::is_nan(g_level[k4[k]]) && g_lspeed[k4[k]] > 0.05f) {
								still = 0.0f;
							}
						}
						// It follows the ground: wider in hollows, where the water stands longer and the evaporating
						// front gathers, narrower on rises, none on steeper banks (over about 8 degrees the fringe's top
						// is a thin line and slumps keep it fresh), and its top wanders along the shore.
						const int kk = MAX(1, (int)Math::round(0.5f / ground_spacing));
						auto gat = [&](int p_x, int p_z) { return ground[CLAMP(p_z, 0, n - 1) * n + CLAMP(p_x, 0, n - 1)]; };
						const float sx = (gat(x + kk, z) - gat(x - kk, z)) / (2.0f * kk * ground_spacing);
						const float sz = (gat(x, z + kk) - gat(x, z - kk)) / (2.0f * kk * ground_spacing);
						const float hollow = 0.25f * (gat(x + kk, z) + gat(x - kk, z) + gat(x, z + kk) + gat(x, z - kk)) - ground[i];
						const float wx = (ground_origin.x + x) * ground_spacing;
						const float wz = (ground_origin.y + z) * ground_spacing;
						const float top = 0.3f + CLAMP(hollow * 40.0f, -0.12f, 0.2f) + 0.16f * (_value_noise(wx / 2.3f + 11.0f, wz / 2.3f + 5.0f) - 0.5f);
						const float steep = Math::smoothstep(0.1f, 0.2f, Math::sqrt(sx * sx + sz * sz));
						crust = arid * still * (1.0f - steep) * Math::smoothstep(0.05f, 0.14f, above) * (1.0f - Math::smoothstep(top - 0.08f, top + 0.06f, above));
					}
				}
				salt_crust[i] = crust;
			} else {
				salt_crust[i] = 0.0f;
			}
			pore[i] = s;
			water_speed[i] = depth > 0.001f ? bil(g_speed) : 0.0f;
			// Rebuilt when its water changed enough to matter.
			float &bs = cell_water[i * 2 + 0];
			float &bv = cell_water[i * 2 + 1];
			if (Math::abs(s - bs) > 0.02f || Math::abs(water_speed[i] - bv) > 0.05f || (s == 0.0f && bs != 0.0f)) {
				const CellInput &in = cell_inputs[i];
				Cell c = material_cell(in.sand, in.moisture, in.litter, s, water_speed[i], in.mud);
				c.max_dent *= _subsoil_factor((ground_origin.x + x) * ground_spacing, (ground_origin.y + z) * ground_spacing);
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
	silt_timer += float(p_delta);
	if (silt_dirty && silt_timer > 5.0f) {
		// The cells take the new silt as fresh mud (a few seconds late: it builds slowly).
		silt_dirty = false;
		silt_timer = 0.0f;
		_update_cells();
	}
	_far_update(p_delta, sources, wm);
	water_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
}

// Smooth value noise in 0..1 on a unit lattice (hashed corners, smoothstep blend).
float GroundSurface3D::_subsoil_factor(float p_x, float p_z) const {
	// The loose layer's depth over the base varies as soil does over a few metres (rooting depth, old channels,
	// stones): two octaves, 2.5 m and 0.9 m, from 0.7 to 1.3 times the material's depth, so no rut bottoms out at
	// one even depth (Paulo's rule: nothing repeating without variation).
	if (!subsoil) {
		return 1.0f;
	}
	const float v = 0.65f * _value_noise(p_x / 2.5f, p_z / 2.5f) + 0.35f * _value_noise(p_x / 0.9f + 31.0f, p_z / 0.9f + 11.0f);
	return 0.7f + 0.6f * v;
}

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
	// Then, with desiccation cracks, their block (opening, plate size, regularity, crust; texel 2's tag 4.5). Every
	// reader counts the blocks by these tags (material.cpp ground_wet_blocks()).
	bool cracks = false;
	if (on && desiccation_cracks && (int)crack_state.size() == n * n * CRACK_FLOATS && n >= 3) {
		for (int i = 0; i < n * n && !cracks; i++) {
			cracks = crack_state[i * CRACK_FLOATS] > 0.0f && crack_state[i * CRACK_FLOATS] < 1.999f;
		}
	}
	// Then, with puddle ice or needle ice, the ice block (puddle ice's thickness m, its broken share; needle ice's
	// height m, the loose crumbs its thaw left; texel 3's tag 5.5).
	bool ice = false;
	const bool pice = on && puddle_ice && (int)ice_state.size() == n * n * ICE_FLOATS && n >= 4;
	const bool nice = on && needle_ice && (int)frost_state.size() == n * n * FROST_FLOATS && n >= 4;
	if (pice) {
		for (int i = 0; i < n * n && !ice; i++) {
			ice = ice_state[i * ICE_FLOATS] > 0.0f;
		}
	}
	if (nice) {
		for (int i = 0; i < n * n && !ice; i++) {
			ice = frost_state[i * FROST_FLOATS + 3] > 0.0f || frost_state[i * FROST_FLOATS + 4] > 0.0f;
		}
	}
	// Then, with mud on plants, its block (share, the clay's linear colour; texel 4's tag 6.5).
	bool pmud = false;
	if (on && plant_mud && (int)plant_mud_state.size() == n * n && n >= 5) {
		for (int i = 0; i < n * n && !pmud; i++) {
			pmud = plant_mud_state[i] > 0.0f;
		}
	}
	// Then, with coloured deposits (a debris fan's ash and char), their block (cover; linear colour; texel 5's tag
	// 7.5): the ground shows the deposit in the colour the flow's sheet draws it with (material.cpp
	// ground_deposit_state()).
	const bool dep_block = on && !mud_deposit_colors.is_empty() && (int)ground_dep.size() == n * n && n >= 6;
	// Then, with salt crusts round still water (water_fringe), their block (crust share; texel 6's tag 8.5).
	bool salt = false;
	if (on && salt_crusts && (int)salt_crust.size() == n * n && n >= 7) {
		for (int i = 0; i < n * n && !salt; i++) {
			salt = salt_crust[i] > 0.0f;
		}
	}
	const int blocks = 1 + (far ? 1 : 0) + (cracks ? 1 : 0) + (ice ? 1 : 0) + (pmud ? 1 : 0) + (dep_block ? 1 : 0) + (salt ? 1 : 0);
	const int w = on ? n : (wet_texture.is_valid() ? wet_texture->get_width() : 1);
	const int h = on ? blocks * n + 1 : (wet_texture.is_valid() ? wet_texture->get_height() : 2);
	Vector<uint8_t> data;
	data.resize(w * h * 16);
	memset(data.ptrw(), 0, data.size());
	if (on) {
		float *d = (float *)data.ptrw();
		for (int i = 0; i < n * n; i++) {
			// A fresh mud deposit (a debris fan) is saturated slurry: it reads as soaked. A silt skin (round 15.1) as
			// wet as the water left in it: a few millimetres of it soaked already show dark.
			const float dep = (int)ground_dep.size() == n * n ? ground_dep[i] : 0.0f;
			const float sx = (ground_origin.x + i % n) * ground_spacing;
			const float sz = (ground_origin.y + i / n) * ground_spacing;
			const float skin = MIN(_silt_smooth_at(sx, sz), dep);
			const float skin_wet = skin > 0.0f ? Math::smoothstep(0.0005f, 0.003f, skin) * _silt_water(sx, sz) : 0.0f;
			const float s = MAX(MAX(wet ? pore[i] : 0.0f, 0.6f * Math::smoothstep(0.0f, 0.05f, dep - skin)), 0.6f * skin_wet);
			d[i * 4 + 0] = Math::smoothstep(0.03f, 0.4f, s);
			// A film of water stands on sand at saturation (under water the water's own surface shows instead). The
			// saturation itself is published and the shaders take the film from it after filtering: classified per
			// lattice point first, the film's edge was drawn as rounded squares a lattice step across.
			d[i * 4 + 1] = CLAMP(s, 0.0f, 1.0f);
			// Loose sand shows its grains: not under litter, not where cemented or fixed, and not damp: water held
			// between the grains (the cell's own moisture, besides the pore water the shaders already read) rounds
			// their facets into menisci, so damp sand has no sparkle. On a damp beach the glints showed as white
			// specks wherever a print bared the sand between stones.
			const Cell &c = cells[i];
			const float damp = (int)cell_inputs.size() == n * n ? Math::smoothstep(0.05f, 0.2f, cell_inputs[i].moisture) : 0.0f;
			d[i * 4 + 2] = sand_glints && c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) * (1.0f - damp) : 0.0f;
			// The fine lattice's weight (A): toward the field's edge the patch tucks under the terrain, and both
			// read the far lattice there. The line where it hands over is wobbled by a smooth value noise (about
			// 2 m across, up to 0.9 m either way). Grains show only on the patch.
			const float wx = (ground_origin.x + i % n) * ground_spacing;
			const float wz = (ground_origin.y + i / n) * ground_spacing;
			const float edge = _edge_distance(wx, wz) + 1.8f * (_value_noise(wx / 2.1f, wz / 2.1f) - 0.5f);
			d[i * 4 + 3] = Math::smoothstep(terrain_margin, terrain_margin + skirt_fade, edge);
		}
		float *r = d + blocks * n * n * 4;
		if (pmud) {
			float *pd = d + (blocks - 1 - (dep_block ? 1 : 0) - (salt ? 1 : 0)) * n * n * 4;
			for (int i = 0; i < n * n; i++) {
				const float wx = (ground_origin.x + i % n) * ground_spacing;
				const float wz = (ground_origin.y + i / n) * ground_spacing;
				const Color mc = get_mud_color_at(Vector3(wx, 0.0f, wz)).srgb_to_linear();
				pd[i * 4 + 0] = plant_mud_state[i];
				pd[i * 4 + 1] = mc.r;
				pd[i * 4 + 2] = mc.g;
				pd[i * 4 + 3] = mc.b;
			}
			r[19] = 6.5f;
		}
		if (salt) {
			float *sd = d + (blocks - 1) * n * n * 4;
			for (int i = 0; i < n * n; i++) {
				sd[i * 4 + 0] = salt_crust[i];
			}
			r[27] = 8.5f;
		}
		if (dep_block) {
			float *dd = d + (blocks - 1 - (salt ? 1 : 0)) * n * n * 4;
			for (int i = 0; i < n * n; i++) {
				if (ground_dep[i] <= 0.0f) {
					continue;
				}
				const float wx = (ground_origin.x + i % n) * ground_spacing;
				const float wz = (ground_origin.y + i / n) * ground_spacing;
				const Color c = _deposit_color_at(wx, wz);
				if (c.a <= 1e-4f) {
					continue;
				}
				// A film of a few millimetres already hides the soil; the edge wanders with a value noise (about 1.7 m),
				// as a lobe's margin.
				const float edge = 0.004f * _value_noise(wx / 1.7f + 31.0f, wz / 1.7f + 7.0f);
				// A silt skin (round 15.1) shows its colour as its crust tone does (_crack_at_cell()), so a thin margin is
				// never the bare dark colour before the drying lifts it.
				const float skin = MIN(_silt_smooth_at(wx, wz), ground_dep[i]);
				const float fan_share = ground_dep[i] > 0.0f ? (ground_dep[i] - skin) / ground_dep[i] : 0.0f;
				const float skin_k = Math::lerp(Math::smoothstep(0.0005f, 0.003f, skin), 1.0f, fan_share);
				dd[i * 4 + 0] = c.a * Math::smoothstep(0.0f, 0.006f, ground_dep[i] - edge) * skin_k;
				dd[i * 4 + 1] = c.r;
				dd[i * 4 + 2] = c.g;
				dd[i * 4 + 3] = c.b;
			}
			r[23] = 7.5f;
		}
		if (ice) {
			float *id = d + (1 + (far ? 1 : 0) + (cracks ? 1 : 0)) * n * n * 4;
			for (int i = 0; i < n * n; i++) {
				if (pice) {
					id[i * 4 + 0] = ice_state[i * ICE_FLOATS];
					id[i * 4 + 1] = ice_state[i * ICE_FLOATS + 1];
				}
				if (nice) {
					id[i * 4 + 2] = frost_state[i * FROST_FLOATS + 3];
					id[i * 4 + 3] = frost_state[i * FROST_FLOATS + 4];
				}
			}
			r[15] = 5.5f;
		}
		if (cracks) {
			float *cd = d + (1 + (far ? 1 : 0)) * n * n * 4;
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
				fd[i * 4 + 1] = CLAMP(far_pore[i], 0.0f, 1.0f);
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

// Calm lee zones (Hesp 1981 for the shadow behind an obstacle; Benes and Roa 2004 for the wind shadow in dune
// models): where the shelter (plants, and rocks, walls and vehicles through WorldWeather.obstacle_lee_at) takes
// the friction speed under the sand's threshold, nothing saltates and the grains falling out of the slowing
// cloud bury the ripples under a smooth drape; in the recovering wind further on they show again, low and stopped
// (the ripple field's own steps already slow there, ripples.mask). The ripples' share there goes as the flux
// factor at the reference gale (shelter_flux): none under 2% of the open flux, all from 40%; bilinear over the
// lattice, so the shadow's edge is soft rather than in lattice squares.
float GroundSurface3D::_ripple_calm_at(float p_x, float p_z) const {
	const int n = ground_n;
	if (!lee_calm_ripples || (int)shelter_flux.size() != n * n || n < 2) {
		return 1.0f;
	}
	const float fx = p_x / ground_spacing - ground_origin.x;
	const float fz = p_z / ground_spacing - ground_origin.y;
	if (fx < 0.0f || fz < 0.0f || fx > n - 1 || fz > n - 1) {
		return 1.0f;
	}
	const int x0 = MIN((int)fx, n - 2);
	const int z0 = MIN((int)fz, n - 2);
	const float ax = fx - x0;
	const float az = fz - z0;
	const float *f = shelter_flux.ptr();
	const float v = Math::lerp(Math::lerp(f[z0 * n + x0], f[z0 * n + x0 + 1], ax), Math::lerp(f[(z0 + 1) * n + x0], f[(z0 + 1) * n + x0 + 1], ax), az);
	return Math::smoothstep(0.02f, 0.4f, v);
}

float GroundSurface3D::get_salt_crust_at(const Vector3 &p_position) const {
	const int n = ground_n;
	if ((int)salt_crust.size() != n * n || n <= 0) {
		return 0.0f;
	}
	const int x = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
	return x >= 0 && z >= 0 && x < n && z < n ? salt_crust[z * n + x] : 0.0f;
}

float GroundSurface3D::get_ripple_show_at(const Vector3 &p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	return c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) * _ripple_calm_at(p_position.x, p_position.z) : 0.0f;
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
	const WorldWeather *wm = _weather();
	const Vector2 mid = field.rect().get_center();
	const float rain = wm ? MAX(wm->precipitation_at(Vector3(mid.x, _ground_at(mid.x, mid.y), mid.y)), 0.0f) / 3.6e6f : 0.0f; // m/s
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
					float k = Math::exp(Math::lerp(Math::log(1e-6f), Math::log(1e-4f), CLAMP(cell.sand, 0.0f, 1.0f)));
					if ((int)cell_inputs.size() == n * n) {
						k *= 1.0f - 0.9f * cell_inputs[li].repel; // burnt water-repellent soil (WorldFire)
					}
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
		// Loose: sand, or the crumbs a needle ice thaw left (round 9).
		const float loose = cell.deformable ? CLAMP(MAX(cell.sand, _needle_loose_at(wx, wz)), 0.0f, 1.0f) : 0.0f;
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

void GroundSurface3D::set_swash_marks(bool p_enabled) {
	swash_marks = p_enabled;
	if (!p_enabled) {
		swash_tr.clear();
	}
}

// SWASH_TRACE=1 prints each mark and every point a mark could not be laid on.
static bool _swash_trace() {
	static const bool on = OS::get_singleton()->has_environment("SWASH_TRACE");
	return on;
}

// A ridge of fine sand from p_from to p_to (world xz), p_to itself left to the next stretch so the joins are not
// laid twice (a doubled point is a bead on the line): small deposits a centimetre apart, a few millimetres high and
// about two near-field texels wide (real ones are a few mm high and about a centimetre wide, Komar 1998; two texels
// is the narrowest the field draws). The sea brings the sand: the swash's own load. p_erase takes it off again
// (a later swash running over the line spreads it and washes its fines away).
void GroundSurface3D::_swash_lay(const Vector2 &p_from, const Vector2 &p_to, bool p_erase) {
	const float len = p_from.distance_to(p_to);
	if (len < 1e-4f) {
		return;
	}
	const int steps = MAX(1, int(Math::ceil(len / 0.01f)));
	// The lip is the swash's finest load, mica, heavy minerals and bits of debris: darker than the sand round it
	// (Komar 1998). That is the line's material, not its moisture, so it stays dark as the beach dries (3: a laid
	// band, 2 plus its share, GroundDeformField::amount_fresh), and the path tracer shows it as the raster does: as
	// a fading fresh tint the old lines were left to the raster's micro-occlusion and faded under PT.
	field.amount_fresh = p_erase ? -2.0f : 3.0f;
	for (int k = 0; k < steps; k++) {
		const Vector2 q = p_from.lerp(p_to, float(k) / steps);
		if (!_near_inside(q.x, q.y)) {
			if (_swash_trace()) {
				print_line(vformat("swash lay outside near at (%.2f, %.2f)", q.x, q.y));
			}
			continue;
		}
		const Cell &c = _cell_at(q.x, q.y);
		if (!c.deformable || c.sand < 0.5f) {
			if (_swash_trace()) {
				print_line(vformat("swash lay skipped at (%.2f, %.2f): sand %.2f deformable %d", q.x, q.y, c.sand, int(c.deformable)));
			}
			continue;
		}
		if (p_erase) {
			dig(Vector3(q.x, _ground_at(q.x, q.y), q.y), 0.12f, 0.006f); // wide (lines keep 15 cm apart): the ridge as saturated sand slumped it before the swash came, no ghost
		} else {
			deposit(Vector3(q.x, _ground_at(q.x, q.y), q.y), 0.012f, 0.004f);
		}
	}
	field.amount_fresh = 0.0f;
}

// Swash marks: run-up followed on shore-normal transects 10 cm apart across the near field, ten times a second.
// The beach's downhill direction there sets the transects; along each, the water's landward edge (swash film or
// sea, over 1 mm) is found to 6 mm. A wave is taken over the whole stretch: while its uprush climbs, each transect
// keeps its highest reach; when the mean reach has fallen back 8 cm, the wave has turned and one line is laid along
// those highest reaches, smoothed and scalloped (below), as a ridge of fine sand. Older lines that a swash covers,
// or that a new line reaches past, are flattened, so lines never cross and the highest stay.
void GroundSurface3D::_swash_update(double p_delta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	swash_time += p_delta;
	if (!is_near_active() || !is_inside_tree()) {
		swash_tr.clear();
		return;
	}
	static const StringName s_swash = "sample_swash";
	LocalVector<Node *> sources;
	bool swash = false;
	for (int k = 0; k < water_sources.size(); k++) {
		Node *src = get_node_or_null(water_sources[k]);
		if (src) {
			sources.push_back(src);
			swash = swash || src->has_method(s_swash);
		}
	}
	if (!swash) {
		swash_tr.clear();
		return;
	}
	// The near field's own area (it follows the main field in whole tiles, so its centre is not the node's).
	const Rect2 nr = near_fld.rect().grow(-NEAR_BLEND - 0.05f);
	const float half = 0.5f * MIN(nr.size.x, nr.size.y);
	const Vector2 mid = nr.position + nr.size * 0.5f;
	swash_frame_age += p_delta;
	// The transects' frame: the beach's downhill direction over the near field, kept while the field stays put
	// (every few seconds and when it moves a metre).
	if (swash_tr.is_empty() || swash_frame_age > 5.0 || swash_mid.distance_to(mid) > 1.0f) {
		const float e = 1.0f;
		const Vector2 g((_ground_at(mid.x + e, mid.y) - _ground_at(mid.x - e, mid.y)) / (2.0f * e), (_ground_at(mid.x, mid.y + e) - _ground_at(mid.x, mid.y - e)) / (2.0f * e));
		if (g.length() < 0.005f) {
			swash_tr.clear();
			swash_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
			return;
		}
		const Vector2 dir = -g.normalized();
		if (swash_tr.is_empty() || dir.dot(swash_dir) < 0.98f || swash_mid.distance_to(mid) > 1.0f) {
			swash_tr.clear();
			swash_tr.resize(int(2.0f * half / 0.1f) + 1);
			swash_lines.clear();
			swash_wave_on = false;
			swash_dir = dir;
			swash_mid = mid;
			if (_swash_trace()) {
				print_line(vformat("swash frame: down (%.2f, %.2f) mid (%.2f, %.2f) half %.2f; ground %.3f landward end, %.3f seaward", dir.x, dir.y, mid.x, mid.y, half, _ground_at(mid.x - dir.x * half, mid.y - dir.y * half), _ground_at(mid.x + dir.x * half, mid.y + dir.y * half)));
			}
		}
		swash_frame_age = 0.0;
	}
	const Vector2 down = swash_dir;
	const Vector2 along(-down.y, down.x);
	const int nt = swash_tr.size();
	auto wet = [&](const Vector2 &q) -> bool {
		const float gh = _ground_at(q.x, q.y);
		float d = 0.0f;
		float v = 0.0f;
		ground_water_at(sources, Vector3(q.x, gh, q.y), gh, d, v);
		return d > 0.001f;
	};
	// Over a line: the film that wets it, or any running water fast enough to move sand (bed_load()), which smears it
	// on the coarse cells the flow's slumping works on; a line left half smeared reads as stepped ghosts.
	auto washes = [&](const Vector2 &q) -> bool {
		const float gh = _ground_at(q.x, q.y);
		float d = 0.0f;
		float v = 0.0f;
		ground_water_at(sources, Vector3(q.x, gh, q.y), gh, d, v);
		return d > 0.001f || (d > 0.0f && bed_load(v) > 0.0f);
	};
	// Each transect's reach now: from the landward end down, the first wet point at 10 cm steps, then halved to
	// 6 mm (m landward of the transect's middle; -1e9 where it is all dry).
	LocalVector<float> reach;
	reach.resize(nt);
	float sum = 0.0f;
	int count = 0;
	for (int t = 0; t < nt; t++) {
		const Vector2 o = swash_mid + along * (-half + 0.1f * t);
		reach[t] = -1e9f;
		for (float sd = -half; sd <= half; sd += 0.1f) {
			if (wet(o + down * sd)) {
				float a = sd - 0.1f;
				float b = sd;
				for (int k = 0; k < 4; k++) {
					const float m = 0.5f * (a + b);
					if (wet(o + down * m)) {
						b = m;
					} else {
						a = m;
					}
				}
				reach[t] = -b;
				break;
			}
		}
		if (reach[t] > -1e8f) {
			sum += reach[t];
			count++;
		}
	}
	// Flattening a stretch digs 12 cm round it, which takes any line kept that close with it (gaps cut into an older
	// line where a flattened one ran beside it): the kept stretches that near are laid again after each pass. The dig
	// takes the whole ridge inside its disc, so laying it again restores it rather than doubling it.
	LocalVector<Vector2> erased;
	auto erase = [&](const Vector2 &p_a, const Vector2 &p_b) {
		_swash_lay(p_a, p_b, true);
		erased.push_back(p_a);
		erased.push_back(p_b);
	};
	auto repair = [&]() {
		if (erased.is_empty()) {
			return;
		}
		for (SwashLine &kl : swash_lines) {
			for (int t = 0; t + 1 < nt; t++) {
				if (!kl.valid[t]) {
					continue;
				}
				bool near = false;
				for (uint32_t e = 0; e + 1 < erased.size() && !near; e += 2) {
					for (int k = 0; k < 3 && !near; k++) {
						const Vector2 q = kl.pts[t].lerp(kl.pts[t + 1], 0.5f * k);
						near = Geometry2D::get_closest_point_to_segment(q, erased[e], erased[e + 1]).distance_to(q) < 0.13f;
					}
				}
				if (near) {
					_swash_lay(kl.pts[t], kl.pts[t + 1]);
				}
			}
		}
		erased.clear();
	};
	// What a swash leaves of a line it partly covered: a stretch under a metre is washed out with the rest (short
	// hooked scraps read as scribbles, not as a line).
	auto drop_scraps = [&](SwashLine &r_ln) {
		int run = 0;
		for (int t = 0; t <= nt; t++) {
			const bool on = t + 1 < nt && r_ln.valid[t];
			if (on) {
				run++;
				continue;
			}
			if (run > 0 && run < 10) {
				for (int u = t - run; u < t; u++) {
					erase(r_ln.pts[u], r_ln.pts[u + 1]);
					r_ln.valid[u] = 0;
				}
			}
			run = 0;
		}
	};
	// The line a wave lays, as each transect's distance landward of the middle: the reach averaged over two and a half
	// metres either side with tent weights (the sea's film is solved on cells of tens of centimetres, so its edge's
	// wiggles finer than a metre or two are the grid's, not the swash's) and its tongues: the leading edge thins and
	// breaks into lobes, convex landward and meeting in cusps that point seaward, so each line is a chain of gentle
	// scallops (Reineck and Singh 1980; Komar 1998), 0.8 to 1.5 m long and 18 to 30 cm deep, new for every wave.
	auto line_reach = [&](uint32_t p_wave, LocalVector<float> &r_d, LocalVector<bool> &r_ok) {
		const uint32_t hw = hash_murmur3_one_32(p_wave);
		const float lw = 0.8f + 0.7f * float(hw & 0xffff) / 65535.0f;
		const float amp = 0.18f + 0.12f * float((hw >> 16) & 0xff) / 255.0f;
		const float phase = float(hw >> 24) / 255.0f;
		r_d.resize(nt);
		r_ok.resize(nt);
		// The wave's mean reach over the stretch: a swash's limit wanders along the shore by tens of centimetres over
		// metres (Komar 1998), not the metre the sea's coarse film drifts by, so the line keeps within 25 cm of it.
		float rmean = 0.0f;
		int rcount = 0;
		for (int t = 0; t < nt; t++) {
			if (swash_max[t] > -1e8f) {
				rmean += swash_max[t];
				rcount++;
			}
		}
		rmean = rcount > 0 ? rmean / rcount : 0.0f;
		for (int t = 0; t < nt; t++) {
			float rs = 0.0f;
			float ws = 0.0f;
			for (int v = MAX(t - 25, 0); v <= MIN(t + 25, nt - 1); v++) {
				if (swash_max[v] > -1e8f) {
					const float w = 26.0f - float(Math::abs(v - t));
					rs += w * swash_max[v];
					ws += w;
				}
			}
			r_ok[t] = ws > 150.0f;
			const float r = rmean + CLAMP((ws > 0.0f ? rs / ws : rmean) - rmean, -0.25f, 0.25f);
			const float s_along = (-half + 0.1f * t) + swash_mid.dot(along);
			// Each lobe a parabolic arc, its ends meeting the next at a cusp of about 40 degrees: rounded arcs (a sine
			// raised to a power under 1 met at right angles and read as a stencil).
			const float u = Math::fposmod(s_along / lw + phase, 1.0f);
			r_d[t] = r + amp * 4.0f * u * (1.0f - u);
		}
	};
	// A swash running over an older line flattens it: the ridge is only a few mm of loose fines. Only inside the
	// line this wave will lay: the sea's coarse film sends tongues past it, and a line washed out where no newer
	// line is laid in its place reads as gaps.
	if (swash_wave_on && (int)swash_max.size() == nt) {
		LocalVector<float> gd;
		LocalVector<bool> gok;
		line_reach(swash_wave + 1, gd, gok);
		auto inside = [&](const Vector2 &q, int t) -> bool {
			return gok[t] && -(q - swash_mid).dot(down) < gd[t];
		};
		for (int l = int(swash_lines.size()) - 1; l >= 0; l--) {
			SwashLine &ln = swash_lines[l];
			bool any = false;
			for (int t = 0; t + 1 < nt; t++) {
				if (!ln.valid[t]) {
					continue;
				}
				const bool in = inside(ln.pts[t], t) || inside(ln.pts[t + 1], t + 1);
				if (in && (washes(ln.pts[t]) || washes(ln.pts[t + 1]) || washes(0.5f * (ln.pts[t] + ln.pts[t + 1])))) {
					erase(ln.pts[t], ln.pts[t + 1]);
					ln.valid[t] = 0;
				} else {
					any = true;
				}
			}
			drop_scraps(ln);
			if (!any) {
				swash_lines.remove_at(l);
			}
		}
		repair();
	}
	if (count < nt / 3) {
		swash_wave_on = false;
		swash_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	// One wave at a time over the whole stretch: its uprush raises the mean reach, and once the mean has fallen
	// back 8 cm from its highest, the wave has turned. Its line is each transect's highest reach in the meantime.
	const float mean = sum / count;
	if ((int)swash_max.size() != nt) {
		swash_max.resize(nt);
		swash_wave_on = false;
	}
	if (!swash_wave_on) {
		swash_wave_on = true;
		swash_r_start = mean;
		swash_r_peak = mean;
		for (int t = 0; t < nt; t++) {
			swash_max[t] = reach[t];
		}
	}
	for (int t = 0; t < nt; t++) {
		swash_max[t] = MAX(swash_max[t], reach[t]);
	}
	swash_r_peak = MAX(swash_r_peak, mean);
	if (mean > swash_r_peak - 0.08f) {
		swash_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	const bool climbed = swash_r_peak - swash_r_start > 0.05f;
	swash_wave_on = false;
	if (!climbed) {
		swash_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
		return;
	}
	swash_wave++;
	SwashLine ln;
	ln.pts.resize(nt);
	ln.valid.resize(nt);
	LocalVector<bool> ok;
	LocalVector<float> ld;
	line_reach(swash_wave, ld, ok);
	for (int t = 0; t < nt; t++) {
		ln.pts[t] = swash_mid + along * (-half + 0.1f * t) - down * ld[t];
		// A lobe past the near field's edge is not drawn there: the line ends at the edge, not in dashes.
		ok[t] = ok[t] && _near_inside(ln.pts[t].x, ln.pts[t].y);
	}
	// valid[t]: the stretch from point t to t + 1 is laid (per stretch, so flattening one never orphans its neighbour).
	for (int t = 0; t < nt; t++) {
		ln.valid[t] = (t + 1 < nt && ok[t] && ok[t + 1]) ? 1 : 0;
	}
	// A run under a metre is not laid at all (a lone scrap reads as a mark of something else).
	{
		int run = 0;
		for (int t = 0; t <= nt; t++) {
			if (t < nt && ln.valid[t]) {
				run++;
				continue;
			}
			if (run > 0 && run < 10) {
				for (int u = t - run; u < t; u++) {
					ln.valid[u] = 0;
					ok[u] = false;
				}
			}
			run = 0;
		}
	}
	// Lines never cross: older lines this one reaches past, or comes within 15 cm of (the same reach twice draws
	// one ridge, not a pair; between two transects a line 10 cm off could still touch), are flattened under it,
	// stretch by stretch.
	for (int l = int(swash_lines.size()) - 1; l >= 0; l--) {
		SwashLine &old = swash_lines[l];
		bool any = false;
		for (int t = 0; t + 1 < nt; t++) {
			if (!old.valid[t]) {
				continue;
			}
			const bool past = (ok[t] && (old.pts[t] - ln.pts[t]).dot(down) > -0.15f) || (ok[t + 1] && (old.pts[t + 1] - ln.pts[t + 1]).dot(down) > -0.15f);
			if (past) {
				erase(old.pts[t], old.pts[t + 1]);
				old.valid[t] = 0;
			} else {
				any = true;
			}
		}
		drop_scraps(old);
		if (!any) {
			swash_lines.remove_at(l);
		}
	}
	repair();
	for (int t = 0; t + 1 < nt; t++) {
		if (ln.valid[t]) {
			_swash_lay(ln.pts[t], ln.pts[t + 1]);
		}
	}
	if (_swash_trace()) {
		print_line(vformat("swash line %d: reach %.2f to %.2f, mean %.2f", swash_wave, swash_r_start, swash_r_peak, mean));
	}
	if (swash_lines.size() >= 16) {
		swash_lines.remove_at(0);
	}
	swash_lines.push_back(ln);
	swash_mark_count++;
	swash_usec = float(OS::get_singleton()->get_ticks_usec() - t0);
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
	// The lee of rocks, walls, buildings and vehicles (WorldWeather.obstacle_lee_at): the wind near the ground there
	// over the open wind, on the same grid. Sand slows and drops behind them (shadow dunes, Hesp 1981).
	LocalVector<float> g_lee;
	const WorldWeather *lee_src = obstacle_lee ? WorldContracts::weather() : nullptr;
	if (lee_src && lee_src->has_provider()) {
		g_lee.resize(gn * gn);
		bool any = false;
		for (int gz = 0; gz < gn; gz++) {
			for (int gx = 0; gx < gn; gx++) {
				const int lx = MIN(gx * step, n - 1);
				const int lz = MIN(gz * step, n - 1);
				const Vector3 p((ground_origin.x + lx) * ground_spacing, ground[lz * n + lx], (ground_origin.y + lz) * ground_spacing);
				const float v = lee_src->obstacle_lee_at(p);
				g_lee[gz * gn + gx] = Math::is_finite(v) ? CLAMP(v, 0.0f, 1.0f) : 1.0f;
				any = any || g_lee[gz * gn + gx] < 0.999f;
			}
		}
		if (!any) {
			g_lee.clear();
		}
	}
	// Coarse grains armour the sand (WorldGround.coarse_cover_at: the stone fields' pebbles and gravel): a lag of
	// them takes the wind's stress and shelters the sand between (the same drag partition as the plants', and the
	// deflation lag's: Bagnold 1941; Nickling and McKenna Neuman 1995), so it no longer blows and grows no ripples
	// (sparse granule ripples aside, Sharp 1963). The flux share none past 35% cover, all under 10%.
	LocalVector<float> g_armour;
	const WorldGround *cover_src = coarse_armour ? WorldContracts::ground() : nullptr;
	if (cover_src) {
		g_armour.resize(gn * gn);
		// One batched read of the whole grid (a provider's lock taken once, not per point).
		LocalVector<Vector2> pts;
		LocalVector<Vector3> cov;
		pts.resize(gn * gn);
		cov.resize(gn * gn);
		for (int gz = 0; gz < gn; gz++) {
			for (int gx = 0; gx < gn; gx++) {
				const int lx = MIN(gx * step, n - 1);
				const int lz = MIN(gz * step, n - 1);
				pts[gz * gn + gx] = Vector2((ground_origin.x + lx) * ground_spacing, (ground_origin.y + lz) * ground_spacing);
			}
		}
		cover_src->coarse_cover_points(pts.ptr(), int(pts.size()), cov.ptr());
		bool any = false;
		for (int gz = 0; gz < gn; gz++) {
			for (int gx = 0; gx < gn; gx++) {
				const Vector3 v = cov[gz * gn + gx];
				const float c = v.x + v.y;
				g_armour[gz * gn + gx] = Math::is_finite(c) ? 1.0f - Math::smoothstep(0.1f, 0.35f, c) : 1.0f;
				any = any || g_armour[gz * gn + gx] < 0.999f;
			}
		}
		if (!any) {
			g_armour.clear();
		}
	}
	const Vector2 wind = _wind_10m();
	const float speed = wind.length();
	const float ustar = GroundRipples::friction_speed(speed);
	const float q_open = GroundRipples::saltation_flux(ustar);
	bool shelter_changed = false;
	LocalVector<float> armour; // the sand's share left open among the stones, per texel
	if (!g_armour.is_empty()) {
		armour.resize(n * n);
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
			const float lambda = Math::lerp(Math::lerp(g_lambda[z0 * gn + x0], g_lambda[z0 * gn + x0 + 1], tx), Math::lerp(g_lambda[(z0 + 1) * gn + x0], g_lambda[(z0 + 1) * gn + x0 + 1], tx), tz);
			float r = shelter_ratio_for(lambda);
			if (!g_lee.is_empty()) {
				// u* scales with the wind at a fixed height over the same roughness: the plants' ratio times the lee.
				r *= Math::lerp(Math::lerp(g_lee[z0 * gn + x0], g_lee[z0 * gn + x0 + 1], tx), Math::lerp(g_lee[(z0 + 1) * gn + x0], g_lee[(z0 + 1) * gn + x0 + 1], tx), tz);
			}
			// The flux factor at a reference gale (u* 0.5 m/s), so the share does not jump with the wind.
			float f = GroundRipples::saltation_flux(0.5f * r) / GroundRipples::saltation_flux(0.5f);
			if (!g_armour.is_empty()) {
				armour[i] = Math::lerp(Math::lerp(g_armour[z0 * gn + x0], g_armour[z0 * gn + x0 + 1], tx), Math::lerp(g_armour[(z0 + 1) * gn + x0], g_armour[(z0 + 1) * gn + x0 + 1], tx), tz);
				f *= armour[i];
			}
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
		const float supply = c.deformable ? c.blown * (armour.is_empty() ? 1.0f : armour[i]) : 0.0f;
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
// Rain pits and rain crust (docs/ideas/sand.md, "Rain pits and rain crust"). Each drop pits dry sand: a hundredth of a
// millimetre of rain lands some 5000 drops of 1.5 mm on a square metre, so the craters cover the sand by about
// 0.05 mm, and each rain lays a new pattern over the last. The wetted skin dries into a crust that seals the surface,
// its fines washed into the pores (Valentin and Bresson 1992): from half a millimetre of rain, whole by 3 mm, over
// half an hour or so of drying. Saltation wipes pits off loose sand in minutes (its impacts rework the top grains)
// and wears a crust away over hours; the crust keeps the wind off what it covers (the ripples' mask, the prints'
// filling and get_blown_at(), so the streamers too). The rain is the weather's (WorldWeather rainfall_at, less the
// canopy's share), at the patch's centre.
void GroundSurface3D::_rain_marks(float p_dt, float p_ustar) {
	if (!rain_pits || p_dt <= 0.0f) {
		return;
	}
	const WorldWeather *wm = _weather();
	float rain = 0.0f;
	float wet = 0.0f;
	if (wm) {
		const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
		rain = wm->rainfall_at(at) * (1.0f - 0.8f * wm->canopy_closure_at(at));
		wet = wm->surface_wetness_at(at);
	}
	if (rain > 0.0f) {
		if (rain_pit_cover < 0.02f) {
			rain_pit_seed = Math::fmod(rain_pit_seed + 1.0f, 4096.0f); // a new rain: a new pattern
		}
		const float mm = rain * p_dt / 3600.0f;
		rain_pit_cover = 1.0f - (1.0f - rain_pit_cover) * Math::exp(-mm / 0.03f);
		rain_total += mm;
	}
	const float seal = Math::smoothstep(0.5f, 3.0f, rain_total) * CLAMP(1.0f - wet / 0.3f, 0.0f, 1.0f);
	if (seal > rain_crust) {
		rain_crust += (seal - rain_crust) * MIN(p_dt / 1800.0f, 1.0f);
	}
	if (p_ustar > GroundRipples::SAND_USTAR_T && wet < 0.3f) {
		const float x = (p_ustar / GroundRipples::SAND_USTAR_T - 1.0f) * p_dt * ripple_time_scale;
		rain_crust *= Math::exp(-x / (6.0f * 3600.0f));
		rain_pit_cover *= Math::exp(-x * (1.0f - rain_crust) / 300.0f);
	}
	if (rain_pit_cover < 0.005f && rain_crust < 0.005f) {
		rain_pit_cover = 0.0f;
		rain_crust = 0.0f;
		rain_total = 0.0f;
	}
	const float pc = get_rain_pit_cover();
	const float kc = get_rain_crust();
	if (Math::abs(pc - rain_published[0]) > 0.01f || Math::abs(kc - rain_published[1]) > 0.01f || ((pc > 0.0f) != (rain_published[0] > 0.0f))) {
		ripple_dirty = true;
	}
}

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
	_rain_marks(float(p_delta), GroundRipples::friction_speed(speed));
	// A grown field for the cells that come in, in the background; again when the wind turns past 35 degrees.
	if (ripple_task != WorkerThreadPool::INVALID_TASK_ID) {
		if (WorkerThreadPool::get_singleton()->is_task_completed(ripple_task)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(ripple_task);
			ripple_task = WorkerThreadPool::INVALID_TASK_ID;
			const bool first = !ripples.tmpl_ready;
			ripples.tmpl = ripple_grown;
			ripples.tmpl_wind = ripple_grow_wind;
			ripples.tmpl_k = ripple_grown_k;
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
				const float m = c.deformable ? c.blown * _shelter_flux_at(x, z) * (1.0f - get_rain_crust()) : 0.0f; // a rain crust holds
				const float show = c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) * _ripple_calm_at(x, z) : 0.0f;
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
		// And for the dune tone: the generator's interdune floors (0..1, -1 off its dune fields or with none) and the sand share.
		ripple_sand_depth.resize(sn * sn);
		ripple_sand_share.resize(sn * sn);
		ripple_sand_blown.resize(sn * sn);
		const TerrainGenerator3D *sgen = is_inside_tree() ? GroundHeightQuery::make(this, height_source, ground_collision_mask).generator : nullptr;
		ripple_sand_wind = sgen ? sgen->get_sand_wind_direction() : Vector2();
		for (int z = 0; z < sn; z++) {
			for (int x = 0; x < sn; x++) {
				const float px = ripple_sand_rect.position.x + (x + 0.5f) * st;
				const float pz = ripple_sand_rect.position.y + (z + 0.5f) * st;
				const Cell &c = _cell_at(px, pz);
				ripple_sand[z * sn + x] = c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) * _ripple_calm_at(px, pz) : 0.0f;
				ripple_sand_share[z * sn + x] = c.deformable ? c.sand * (1.0f - MIN(c.litter / 0.02f, 1.0f)) : 0.0f;
				ripple_sand_blown[z * sn + x] = c.deformable ? c.blown : 0.0f;
				ripple_sand_depth[z * sn + x] = sgen ? float(sgen->get_sand_interdune_at(Vector3(px, 0.0f, pz))) : -1.0f;
			}
		}
		// The dune tone past the patch: the generator's interdune floors over a kilometre round it, in 8 m texels (its own
		// grid's scale), so ground past the patch with a BaseMaterial3D shows the interdune flats too.
		{
			const int fn = RIPPLE_DEPTH_N;
			const float fs = RIPPLE_DEPTH_SIZE / fn;
			ripple_far_depth_rect = Rect2(Math::snapped(center.x, fs) - 0.5f * RIPPLE_DEPTH_SIZE, Math::snapped(center.y, fs) - 0.5f * RIPPLE_DEPTH_SIZE, RIPPLE_DEPTH_SIZE, RIPPLE_DEPTH_SIZE);
			ripple_far_depth.resize(fn * fn);
			for (int z = 0; z < fn; z++) {
				for (int x = 0; x < fn; x++) {
					const Vector3 fp(ripple_far_depth_rect.position.x + (x + 0.5f) * fs, 0.0f, ripple_far_depth_rect.position.y + (z + 0.5f) * fs);
					ripple_far_depth[z * fn + x] = sgen ? float(sgen->get_sand_interdune_at(fp)) : -1.0f;
				}
			}
		}
		ripple_dirty = true;
	}
	// Where the field's surface changed (a print, a rut, sand landing), the ripples there are planed off by the
	// change's depth over a ripple's height (a few mm: a grain flow a centimetre thick buries them, a 1 mm skim only
	// softens them, and a slow creep under half a millimetre an update leaves them); they grow back as the wind works the new surface. The change is read between the field's texels
	// at each ripple cell, so a planed patch has a soft edge and no texel steps.
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
		ripple_change.resize(fn * fn);
		int bx0 = fn, bz0 = fn, bx1 = -1, bz1 = -1;
		for (int z = 0; z < fn; z++) {
			for (int x = 0; x < fn; x++) {
				const float wx = (fx0 + x + 0.5f) * t;
				const float wz = (fz0 + z + 0.5f) * t;
				const float v = field.sample(field.rim, wx, wz) - field.sample(field.dent, wx, wz);
				float &seen = ripple_seen[z * fn + x];
				const float dv = fresh ? 0.0f : Math::abs(v - seen);
				ripple_change[z * fn + x] = dv;
				if (dv > 0.0005f) {
					bx0 = MIN(bx0, x);
					bz0 = MIN(bz0, z);
					bx1 = MAX(bx1, x);
					bz1 = MAX(bz1, z);
				}
				seen = v;
			}
		}
		if (bx1 >= 0) {
			const float tc = t / ripples.cell;
			const int cx0 = MAX((int)Math::floor((fx0 + bx0 - 0.5f) * tc) - ripples.origin.x, 0);
			const int cz0 = MAX((int)Math::floor((fz0 + bz0 - 0.5f) * tc) - ripples.origin.y, 0);
			const int cx1 = MIN((int)Math::ceil((fx0 + bx1 + 1.5f) * tc) - ripples.origin.x, ripples.n - 1);
			const int cz1 = MIN((int)Math::ceil((fz0 + bz1 + 1.5f) * tc) - ripples.origin.y, ripples.n - 1);
			for (int cz = cz0; cz <= cz1; cz++) {
				const float fz = (ripples.origin.y + cz) / tc - fz0 - 0.5f;
				const int iz = CLAMP((int)Math::floor(fz), 0, fn - 2);
				const float tz = CLAMP(fz - iz, 0.0f, 1.0f);
				for (int cx = cx0; cx <= cx1; cx++) {
					const float fx = (ripples.origin.x + cx) / tc - fx0 - 0.5f;
					const int ix = CLAMP((int)Math::floor(fx), 0, fn - 2);
					const float tx = CLAMP(fx - ix, 0.0f, 1.0f);
					const float *c0 = &ripple_change[iz * fn + ix];
					const float dv = Math::lerp(Math::lerp(c0[0], c0[1], tx), Math::lerp(c0[fn], c0[fn + 1], tx), tz);
					if (dv > 0.0005f) {
						ripples.h[cz * ripples.n + cx] *= MAX(1.0f - (dv - 0.0005f) / 0.004f, 0.0f);
					}
				}
			}
			ripple_dirty = true;
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
		const int w = MAX(MAX(n, sn), (int)RIPPLE_DEPTH_N);
		const int dn = RIPPLE_DEPTH_N;
		const int hgt = n + sn + n + dn + 1;
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
					d[((n + z) * w + x) * 4 + 1] = (int)ripple_sand_depth.size() == sn * sn ? ripple_sand_depth[z * sn + x] : -1.0f;
					d[((n + z) * w + x) * 4 + 2] = (int)ripple_sand_share.size() == sn * sn ? ripple_sand_share[z * sn + x] : 0.0f;
					d[((n + z) * w + x) * 4 + 3] = (int)ripple_sand_blown.size() == sn * sn ? ripple_sand_blown[z * sn + x] : 0.0f;
				}
			}
		}
		// Rows n+S..n+S+n-1: the grown field's own height (R), for the grain sorting on the far ripples.
		if ((int)ripple_tile_norm.size() == nn) {
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					d[((n + sn + z) * w + x) * 4] = ripple_tile_norm[z * n + x];
				}
			}
		}
		// Rows n+S+n..n+S+n+D-1: the generator's interdune floors round the patch (R), for the dune tone past it.
		if ((int)ripple_far_depth.size() == dn * dn) {
			for (int z = 0; z < dn; z++) {
				for (int x = 0; x < dn; x++) {
					d[((n + sn + n + z) * w + x) * 4] = ripple_far_depth[z * dn + x];
				}
			}
		}
		float *meta = d + (n + sn + n + dn) * w * 4;
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
		// The dune field past the patch (far_ripples): on, and the ground's own sand share there (all sand for a
		// sand ground with no type map; the terrain material gives its own).
		meta[11] = far_ripples && tile ? 1.0f : 0.0f;
		meta[14] = ground_type == GROUND_SAND && type_map.is_null() ? 1.0f : 0.0f;
		// The grown field's height spread (m), to read the window's heights in the same units; 0 with the
		// grain sorting off.
		meta[15] = ripple_sorting ? ripple_tile_sd : 0.0f;
		meta[16] = dune_tone ? 1.0f : 0.0f; // the dune tone (sand rows G, B)
		meta[17] = ripple_far_depth_rect.position.x; // and the depth rows' rect
		meta[18] = ripple_far_depth_rect.position.y;
		meta[19] = ripple_far_depth_rect.size.x;
		meta[20] = ripple_sand_wind.x; // the dune-building wind, for the granule ripples
		meta[21] = ripple_sand_wind.y;
		meta[22] = granule_ripples ? 1.0f : 0.0f;
		meta[24] = get_rain_pit_cover(); // the rain's marks (_rain_marks())
		meta[25] = get_rain_crust();
		meta[26] = rain_pit_seed;
		rain_published[0] = meta[24];
		rain_published[1] = meta[25];
		meta[12] = ripples.tmpl_k.x; // the template's wave vector, for the shaders' dislocations
		meta[13] = ripples.tmpl_k.y;
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
	// Its height over its mean in standard deviations, for the grain sorting (troughs below -0.2 or so).
	double hm = 0.0, hv = 0.0;
	for (int i = 0; i < n * n; i++) {
		hm += ripples.tmpl[i];
	}
	hm /= double(n * n);
	for (int i = 0; i < n * n; i++) {
		hv += (ripples.tmpl[i] - hm) * (ripples.tmpl[i] - hm);
	}
	ripple_tile_sd = float(Math::sqrt(hv / double(n * n)));
	ripple_tile_norm.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		ripple_tile_norm[i] = ripple_tile_sd > 1e-6f ? float((ripples.tmpl[i] - hm) / ripple_tile_sd) : 0.0f;
	}
	Vector<uint8_t> data;
	data.resize(n * n * 12);
	float *d = (float *)data.ptrw();
	const float inv = 0.5f / ripples.cell;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float *t = ripples.tmpl.ptr();
			const float sx = (t[z * n + (x + 1) % n] - t[z * n + (x + n - 1) % n]) * inv;
			const float sz = (t[((z + 1) % n) * n + x] - t[((z + n - 1) % n) * n + x]) * inv;
			ripple_tile_slope[(z * n + x) * 2 + 0] = sx;
			ripple_tile_slope[(z * n + x) * 2 + 1] = sz;
			d[(z * n + x) * 3 + 0] = sx;
			d[(z * n + x) * 3 + 1] = sz;
			d[(z * n + x) * 3 + 2] = ripple_tile_norm[z * n + x]; // its height (sd), for the grain sorting
		}
	}
	ripple_far_image = Image::create_from_data(n, n, false, Image::FORMAT_RGBF, data);
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
	const WorldWeather *wm = _weather();
	const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
	const float wind = _wind_10m().length();
	float rain = 0.0f;
	if (wm) {
		rain = wm->precipitation_at(at);
	}
	// Sand moves above about 5 m/s at 10 m (Bagnold's threshold for fine dry sand); not under a rain crust.
	const float drift = sand_drift_rate * MAX(wind - 5.0f, 0.0f) / 5.0f * (1.0f - get_rain_crust());
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
						const float keep = Math::exp(-(drift * (wind_fills_prints ? PRINT_DRIFT_SOFTEN : 1.0f) * c.blown * _shelter_flux_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t) + soften) * dt_h);
						dn *= keep;
						rm *= keep;
					}
					if (field.fresh[i] >= 4.0f) {
						// Tracked mud dries in place (its film stays).
						const Vector2 sq = GroundDeformField::mud_band_share_wet(field.fresh[i]);
						field.fresh[i] = GroundDeformField::mud_band(sq.x, sq.y * dry);
					} else if (field.fresh[i] < 2.0f) {
						field.fresh[i] *= dry; // a laid mineral band (2 plus its share) does not dry away
					}
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
					if (field.fresh[i] < 0.01f || (field.fresh[i] >= 2.0f && field.fresh[i] < 2.01f)) {
						field.fresh[i] = 0.0f;
					}
					any = any || dn > 0.0f || rm > 0.0f || field.fresh[i] > 0.0f;
				}
			}
			field.tile_active[tz * nt + tx] = any ? 1 : 0;
		}
	}
	if (wind_fills_prints && drift > 0.0f) {
		_drift_prints(_wind_10m(), drift, dt);
	}
	field.dirty = true;
}

// Wind fills a print from its upwind side and wears its downwind rim (aeolian infilling): in the hollow the
// saltation's shear falls (the upwind wall shelters it) while the load the wind brings keeps landing, and the rim
// facing the wind takes more shear than the plain round it. With the flux lagging the shear by its saturation
// length (Sauermann, Kroy and Herrmann 2001; Andreotti et al. 2002), larger than a print, the net is the
// print's relief carried downwind at the bed's celerity c = q / H (Exner), the upwind wall filling first and the
// hollow shallowing as it creeps on; the first-order upwind scheme's own diffusion (c l / 2) rounds it. c is
// PRINT_DRIFT_LENGTH over the old fade time, so a print about that long still goes in about the same time,
// now from one side; a share of the old even fade stays for the softening the shear's sideways spread gives.
// Conserves the sand: the moved relief leaves one texel as it enters the next, held by each texel's blown share
// (damp or litter-covered ground neither gives nor takes). GPU: ground_deform.glsl drift_give().
void GroundSurface3D::_drift_prints(const Vector2 &p_wind, float p_drift, float p_dt) {
	const float wl = p_wind.length();
	if (wl <= 0.0f) {
		return;
	}
	const Vector2 w = p_wind / wl;
	const int n = field.resolution;
	const int nt = field.tiles();
	const float t = field.texel();
	const int T = GroundDeformField::TILE;
	// Courant number a second's step (well under 1 at any wind: a print moves about a metre an hour).
	const float cr = p_drift * PRINT_DRIFT_LENGTH / 3600.0f * p_dt / t;
	if (cr <= 0.0f) {
		return;
	}
	const int steps = int(Math::ceil(cr / 0.5f));
	const float c = cr / steps;
	// The tiles that move (active, or beside one, which the moved relief may enter).
	LocalVector<uint8_t> on;
	on.resize(nt * nt);
	bool any = false;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			bool a = false;
			for (int dz = -1; dz <= 1 && !a; dz++) {
				for (int dx = -1; dx <= 1 && !a; dx++) {
					const int qx = tx + dx;
					const int qz = tz + dz;
					a = qx >= 0 && qz >= 0 && qx < nt && qz < nt && field.tile_active[qz * nt + qx];
				}
			}
			on[tz * nt + tx] = a ? 1 : 0;
			any = any || a;
		}
	}
	if (!any) {
		return;
	}
	LocalVector<float> d;
	LocalVector<float> b;
	d.resize(n * n);
	b.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		d[i] = field.rim[i] - field.dent[i];
		b[i] = 0.0f;
	}
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			if (!on[tz * nt + tx]) {
				continue;
			}
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const float wx = (field.origin.x + x + 0.5f) * t;
					const float wz = (field.origin.y + z + 0.5f) * t;
					const Cell &cell = _cell_at(wx, wz);
					b[z * n + x] = cell.deformable ? cell.blown * _shelter_flux_at(wx, wz) : 0.0f;
				}
			}
		}
	}
	LocalVector<float> fx;
	LocalVector<float> fz;
	fx.resize(n * n);
	fz.resize(n * n);
	for (int s = 0; s < steps; s++) {
		// Through each texel's +x and +z faces, from the upwind side; the field's edges closed.
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				fx[i] = 0.0f;
				fz[i] = 0.0f;
				if (!on[(z / T) * nt + x / T]) {
					continue;
				}
				if (x < n - 1) {
					const int u = w.x > 0.0f ? i : i + 1;
					fx[i] = c * w.x * MIN(b[i], b[i + 1]) * d[u];
				}
				if (z < n - 1) {
					const int u = w.y > 0.0f ? i : i + n;
					fz[i] = c * w.y * MIN(b[i], b[i + n]) * d[u];
				}
			}
		}
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (!on[(z / T) * nt + x / T]) {
					continue;
				}
				d[i] += (x > 0 ? fx[i - 1] : 0.0f) - fx[i] + (z > 0 ? fz[i - n] : 0.0f) - fz[i];
			}
		}
	}
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			if (!on[tz * nt + tx]) {
				continue;
			}
			bool a = false;
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					field.dent[i] = d[i] < -1e-5f ? -d[i] : 0.0f;
					field.rim[i] = d[i] > 1e-5f ? d[i] : 0.0f;
					a = a || d[i] < -1e-5f || d[i] > 1e-5f || field.fresh[i] > 0.0f;
				}
			}
			field.tile_active[tz * nt + tx] = a ? 1 : 0;
		}
	}
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
			WorldContracts::register_water(this);
			if (active == this) {
				WorldContracts::register_ground(this);
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
			WorldContracts::unregister_water(this);
			WorldContracts::unregister_ground(this);
			set_physics_process_internal(false);
			if (_ruts_on()) {
				save_ruts();
			}
			_gpu_release();
			if (ripple_task != WorkerThreadPool::INVALID_TASK_ID) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(ripple_task);
				ripple_task = WorkerThreadPool::INVALID_TASK_ID;
			}
			if (active == this) {
				_set_global("ground_deform_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
				_set_global("ground_ripple_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
				if (wear_texture.is_valid()) {
					_set_global("ground_wear_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
					wear_origin = Vector2i(INT32_MAX, INT32_MAX);
				}
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
				WorldContracts::register_ground(this);
			}
			if (patch_params_stale) {
				_sync_patch_params();
			}
			// Paused (still shots): the field, its texture and every read stay as they are; nothing steps, follows or
			// reads back, which on a software GPU is most of a frame.
			if (simulation_paused) {
				step_msec = 0.0;
				break;
			}
			_follow(_follow_position());
			if (!buried_kicks.is_empty()) {
				_buried_kicks_update();
			}
			_loose_update(float(delta));
			_silt_wind_emit();
			if (!pending_puddles.is_empty()) {
				_puddles_update(delta);
			}
			puddle_accum += delta;
			if (puddle_accum >= 0.5) {
				_wear_publish();
				_buried_update();
				_world_puddles_update(puddle_accum);
				_silt_wind_update(float(puddle_accum));
				puddle_accum = 0.0;
			}
			wear_clock += delta;
			cells_accum += delta;
			if (cells_accum >= 2.0) {
				_wear_update(cells_accum);
				_trail_pug_update(float(cells_accum));
				cells_accum = 0.0;
				_update_cells();
			}
			water_accum += delta;
			if (water_accum >= 0.5) {
				_water_update(water_accum);
				_crack_update(water_accum);
				_ice_update(water_accum);
				_plant_mud_update(water_accum);
				_frost_update(water_accum);
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
			if (swash_marks) {
				swash_accum += delta;
				if (swash_accum >= 0.1) {
					_swash_update(swash_accum);
					swash_accum = 0.0;
				}
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
				_bump_deform_serial();
				Columns cols;
				cols.surface = this;
				const Cell &c = _cell_at(center.x, center.y);
				field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
				// The bodies press as stamps for the GPU field (the same maths).
				field.stamp_sink = &gpu_sink;
				field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, delta);
				field.stamp_sink = nullptr;
				_plough_bodies();
				_avalanche_update(delta);
				_update_mats(delta);
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
				_avalanche_update(delta);
				_update_mats(delta);
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

// Ruts steer the wheels (docs/ideas/mud.md, round 9). A tyre whose contact sits across uneven ruts rests on a tilted
// patch: the ground's reaction tips with it and pushes the tyre down the tilt by the load times its slope, as a road's
// camber or a rut's wall does (rut wandering: Bridgestone and others on tyres tracking in worn ruts; the same
// resolution of the normal force as on any slope). Only the deformation counts (dent less rim), not the terrain's own
// cross slope, which the vehicle's road plane already carries. Returns (the deformation's slope across the patch
// along p_lateral, rising towards +p_lateral; the wall's height beside the tyre on its -p_lateral side and on its
// +p_lateral side over the patch's middle, m): a wheel moving sideways into a wall shoves it as a bulldozed face that
// high.
Vector3 GroundSurface3D::get_rut_guidance(Vector3 p_contact, Vector3 p_lateral, float p_half_width) const {
	if (!field.valid || p_half_width <= 0.0f) {
		return Vector3();
	}
	Vector3 lat = Vector3(p_lateral.x, 0.0f, p_lateral.z);
	if (lat.length_squared() < 1e-6f) {
		return Vector3();
	}
	lat.normalize();
	auto relief = [&](float p_s) {
		const Vector3 q = p_contact + lat * p_s;
		return get_rim_at(q) - get_dent_at(q);
	};
	// The patch's cross slope by least squares over seven points across the tread.
	float sum_s2 = 0.0f;
	float sum_sh = 0.0f;
	float mean = 0.0f;
	float hs[7];
	for (int k = 0; k < 7; k++) {
		hs[k] = relief(p_half_width * (float(k) / 3.0f - 1.0f));
		mean += hs[k] / 7.0f;
	}
	for (int k = 0; k < 7; k++) {
		const float sk = p_half_width * (float(k) / 3.0f - 1.0f);
		sum_s2 += sk * sk;
		sum_sh += sk * (hs[k] - mean);
	}
	const float slope = CLAMP(sum_sh / MAX(sum_s2, 1e-6f), -1.0f, 1.0f);
	// The walls: the highest ground within 15 cm outside each edge, above the floor under the tread.
	const float floor_h = MIN(MIN(hs[2], hs[3]), hs[4]);
	float wall[2] = { 0.0f, 0.0f };
	for (int side = 0; side < 2; side++) {
		const float sg = side == 0 ? -1.0f : 1.0f;
		for (int k = 0; k < 3; k++) {
			wall[side] = MAX(wall[side], relief(sg * (p_half_width + 0.03f + 0.06f * k)) - floor_h);
		}
	}
	return Vector3(slope, wall[0], wall[1]);
}

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
	soil.layered = subsoil;
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

float GroundSurface3D::press_wheel(Vector3 p_position, Vector2 p_size, float p_yaw, float p_sinkage, Vector2 p_velocity, int p_tread) {
	if (!field.valid || p_sinkage <= 0.0f || p_size.x <= 0.0f || p_size.y <= 0.0f) {
		return 0.0f;
	}
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const Vector2 half = p_size * 0.5f;
	const float radius = 0.5f * MAX(p_size.x, p_size.y);
	// A stone or root hidden in the mud under the tyre's middle holds the wheel up (round 14.3); one under the
	// rest of the print the tyre wraps round, and the mud is pressed out round it (round 15.2, below).
	if (buried_obstacles) {
		p_sinkage = MIN(p_sinkage, _buried_depth(p_position, 0.25f * MIN(p_size.x, p_size.y)));
		if (p_sinkage <= 0.002f) {
			return 0.0f;
		}
	}
	const float bottom = _ground_at(p_position.x, p_position.z) - p_sinkage;
	Buried hit;
	const bool floored = buried_obstacles && _buried_under(p_position, half.length(), bottom, hit);
	if (floored) {
		field.press_floor_a = Vector4(hit.a.x, hit.a.y, hit.a.z, hit.r);
		field.press_floor_b = hit.b;
		near_fld.press_floor_a = field.press_floor_a;
		near_fld.press_floor_b = field.press_floor_b;
		// Uncovered past its middle (the rut floor under its centre) and met by a rolling tyre: knocked loose.
		const float speed = p_velocity.length();
		if (buried_kick && !hit.root && bottom < hit.a.y && speed > 0.3f && !buried_loose.has(hit.id)) {
			const Vector2 to(hit.a.x - p_position.x, hit.a.z - p_position.z);
			if (to.dot(p_velocity) > 0.0f) { // ahead of the wheel's middle: the tyre runs into it
				BuriedKick k;
				k.b = hit;
				k.floor = bottom;
				k.velocity = Vector3(p_velocity.x, 0.25f * speed, p_velocity.y) * 0.6f;
				buried_kicks.push_back(k);
			}
		}
	}
	_ice_break(p_position, radius, 0.06f);
	_ford(p_position, half, p_yaw, p_sinkage, p_velocity);
	if (worn_tracks) {
		_wear_press(p_position, half, p_yaw);
	}
	// The tread's lugs or chains in the rut floor (round 10).
	field.press_tread = p_tread >= 0 ? MIN(p_tread, 3) + 1 : 0;
	near_fld.press_tread = field.press_tread;
	float moved = 0.0f;
	if (_gpu_on()) {
		// The bodies' path: only the GPU field takes it (the CPU copy follows at the read back).
		gpu_sink.stamp(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
	} else {
		moved = field.press(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
		_avalanche_trigger(p_position, half.length(), bottom);
	}
	field.press_tread = 0;
	near_fld.press_tread = 0;
	if (buried_kick) {
		_loose_shove(p_position, half, p_yaw, bottom, p_velocity);
		_loose_under(p_position, half, p_yaw, bottom, p_velocity);
	}
	if (floored) {
		field.press_floor_a = Vector4();
		near_fld.press_floor_a = Vector4();
	}
	return moved;
}

// ---------------------------------------------------------------------------------------------------------
// Stream fords (docs/ideas/mud.md, round 13.2).
// The share of a wheel's rims the current carries off at full shear (material_cell()).
//
// In the bed: a tyre turning in running water remoulds the bed under it; the current carries the loose spoil off as
// a plume before it can settle back as a rim (fine silt settles at about 3e-4 m/s, so a 30 cm stream running at
// 0.5 m/s carries it some 500 m). Off-road vehicle crossings measured as turbidity spikes of hundreds of NTU that
// pass downstream with each crossing (Alaska DEC Kenai ATV crossings study, 2002; Forest Service low-water crossing
// guides). Here the rut a wheel presses in the bed loses a share of its depth to the water: the share that rises
// with the current's shear over the remoulded soil's critical shear (about 0.5 Pa, a sixth of intact soil's; Knapen
// et al. 2007), and its fines (the soil less its sand) go to WorldWater as a load dropped there.
//
// On the bank: a saturated bank fails in shallow slabs (infinite slope, Skempton and DeLory 1957; Simon et al. 1999
// on pore pressure in streambank stability). A wheel's load on saturated soil is first carried by the pore water
// (undrained), so it adds to the slab's weight down the slope but none to its friction:
//   FS = (c' + gamma' z cos^2 b tan phi' + (1 - m) q cos^2 b tan phi') / ((gamma z + q) sin b cos b)
// with the slab z = 0.5 m deep, m the saturated share, gamma 19 kN/m3 saturated (17 moist), gamma' 9.2, and q the
// tyre's Bekker pressure spread 2:1 down to the slab's base. Under 1 the slab slides into the water: a dig where it
// stood, fresh mud at the toe, its fines to the water. Trampled stream banks retreat the same way (Trimble 1994).
void GroundSurface3D::_ford(const Vector3 &p_position, const Vector2 &p_half, float p_yaw, float p_sinkage, const Vector2 &p_velocity) {
	if (!stream_fords || p_sinkage <= 0.0f) {
		return;
	}
	WorldWater *ww = WorldContracts::water();
	if (!ww || !ww->has_provider()) {
		return;
	}
	const Cell &c = _cell_at(p_position.x, p_position.z);
	if (!c.deformable) {
		return;
	}
	const float dt = MAX(float(get_physics_process_delta_time()), 1e-3f);
	const float g0 = _ground_at(p_position.x, p_position.z);
	const float fines = CLAMP(1.0f - c.sand, 0.0f, 1.0f);
	const float dry_density = 1500.0f; // kg/m3 of the bed's soil
	Vector3 flow;
	const float wh = ww->height_except(Vector3(p_position.x, g0, p_position.z), this, &flow);
	const float depth = Math::is_finite(wh) ? wh - g0 : 0.0f;
	if (depth >= 0.03f) {
		const float u = Vector2(flow.x, flow.z).length();
		const float tau = 1000.0f * 0.003f * u * u; // Pa, as bed_load()
		const float share = Math::smoothstep(0.5f, 3.0f, tau) * fines;
		const float travel = p_velocity.length() * dt;
		if (share > 0.0f && travel > 0.0f) {
			const float width = 2.0f * p_half.x;
			const float vol = width * travel * p_sinkage * 0.35f * share;
			const float r = MAX(0.8f * p_half.x, 0.05f);
			dig(Vector3(p_position.x, g0, p_position.z), r, vol / (Math::PI * r * r));
			// With the rims the current took (material_cell(): the share it carries off of what the press
			// threw, about half the rut's volume on soil).
			const float rims = width * travel * p_sinkage * 0.5f * FORD_RIM_LOSS * share;
			const float kg = (vol + rims) * dry_density;
			ww->add_sediment_mass(Vector3(p_position.x, g0 + 0.5f * depth, p_position.z), kg, 0.0f);
			ford_plume_kg += kg;
			ford_scour_m3 += vol;
		}
		return;
	}
	// A bank: the ground falls toward water within a few metres.
	const float e = 0.5f;
	const Vector2 grad((_ground_at(p_position.x + e, p_position.z) - _ground_at(p_position.x - e, p_position.z)) / (2.0f * e),
			(_ground_at(p_position.x, p_position.z + e) - _ground_at(p_position.x, p_position.z - e)) / (2.0f * e));
	const float tb = grad.length();
	if (tb < Math::tan(Math::deg_to_rad(12.0f))) {
		return;
	}
	const Vector2 down = -grad / tb;
	Vector3 toe;
	bool water = false;
	for (int k = 1; k <= 6 && !water; k++) {
		const Vector2 q = Vector2(p_position.x, p_position.z) + down * (0.5f * k);
		const float gq = _ground_at(q.x, q.y);
		const float hq = ww->height_except(Vector3(q.x, gq, q.y), this);
		if (Math::is_finite(hq) && hq - gq > 0.05f) {
			water = true;
			toe = Vector3(q.x, gq, q.y);
		}
	}
	if (!water) {
		return;
	}
	const Vector2i key(int(Math::floor(p_position.x)), int(Math::floor(p_position.z)));
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (const uint64_t *t = bank_failed.getptr(key)) {
		if (now - *t < 4000) {
			return;
		}
	}
	const GroundSoil soil = get_soil(p_position);
	// The slab's saturated share: its pore water (a stream's seepage, rain) or the soil's own moisture there.
	float wet = get_saturation_at(p_position);
	{
		const int n = ground_n;
		const int x = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
		const int zz = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
		if (ground_valid && (int)cell_inputs.size() == n * n && x >= 0 && zz >= 0 && x < n && zz < n) {
			wet = MAX(wet, cell_inputs[zz * n + x].moisture);
		}
	}
	float m = Math::smoothstep(0.5f, 0.95f, wet);
	// A bank's water table stands at the stream's level and wets the soil a capillary fringe above it (about 0.3 m
	// in silt; Simon et al. 1999 take the water table from the stage): the slab's share below that.
	const float z = 0.5f;
	{
		const float level = ww->height_except(toe, this);
		if (Math::is_finite(level)) {
			m = MAX(m, CLAMP((level + 0.3f - (g0 - z)) / z, 0.0f, 1.0f));
		}
	}
	const float b = Math::atan(tb);
	const float cb = Math::cos(b);
	const float sb = Math::sin(b);
	const float bw = 2.0f * p_half.x;
	const float bl = 2.0f * p_half.y;
	const float p_kpa = MIN(GroundTerramechanics::bekker_k(soil, MIN(bw, bl)) * Math::pow(p_sinkage, soil.n), 250.0f); // no more than a tyre carries
	const float q = p_kpa * bw * bl / ((bw + z) * (bl + z));
	const float gamma = Math::lerp(17.0f, 19.0f, m);
	const float gamma_e = Math::lerp(17.0f, 9.2f, m);
	const float resist = soil.cohesion + (gamma_e * z + (1.0f - m) * q) * cb * cb * soil.tan_phi;
	const float drive = (gamma * z + q) * sb * cb;
	if (drive <= 0.0f || resist / drive >= 1.0f) {
		return;
	}
	// The slab gives way: it leaves the bank under the wheel and slumps to the toe, half its fines into the water.
	bank_failed[key] = now;
	bank_failures++;
	const float r = 0.9f;
	const float d = 0.25f;
	const Vector3 top(p_position.x - down.x * 0.3f, g0, p_position.z - down.y * 0.3f);
	const float vol = dig(top, r, d);
	const float slab = vol > 0.0f ? vol : Math::PI * r * r * d;
	const float kg = 0.5f * slab * dry_density * fines;
	ww->add_sediment_mass(toe, kg, 0.0f);
	ford_plume_kg += kg;
	const float lay = slab * (1.0f - 0.5f * fines);
	add_mud_deposit(toe + Vector3(down.x, 0.0f, down.y) * 0.8f, 0.7f, lay / 2.0f, true);
}

Dictionary GroundSurface3D::get_ford_stats() const {
	Dictionary d;
	d["plume_kg"] = ford_plume_kg;
	d["scour_m3"] = ford_scour_m3;
	d["bank_failures"] = bank_failures;
	return d;
}

// ---------------------------------------------------------------------------------------------------------
// Mats (docs/ideas/mud.md, round 12): a corduroy of logs or a brush mat laid in the ruts.

bool GroundSurface3D::_mat_shape(Node *p_body, Vector3 &r_a, Vector3 &r_b, float &r_radius) const {
	Node3D *body = Object::cast_to<Node3D>(p_body);
	if (!body) {
		return false;
	}
	for (int i = 0; i < body->get_child_count(); i++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(body->get_child(i));
		if (!cs || cs->get_shape().is_null()) {
			continue;
		}
		float h = 0.0f;
		if (Ref<CylinderShape3D> cy = cs->get_shape(); cy.is_valid()) {
			h = cy->get_height();
			r_radius = cy->get_radius();
		} else if (Ref<CapsuleShape3D> ca = cs->get_shape(); ca.is_valid()) {
			h = ca->get_height();
			r_radius = ca->get_radius();
		} else {
			continue;
		}
		const Transform3D t = cs->get_global_transform();
		const Vector3 axis = t.basis.get_column(1).normalized() * (0.5f * h);
		r_a = t.origin - axis;
		r_b = t.origin + axis;
		return true;
	}
	return false;
}

int GroundSurface3D::get_mat_at(Vector3 p_contact, float p_tolerance) const {
	for (uint32_t i = 0; i < mat_list.size(); i++) {
		const Mat &m = mat_list[i];
		if (m.broken || m.buried) {
			continue;
		}
		const Vector3 ab = m.b - m.a;
		const float t = CLAMP((p_contact - m.a).dot(ab) / MAX(ab.length_squared(), 1e-6f), 0.0f, 1.0f);
		if ((m.a + ab * t).distance_to(p_contact) <= m.radius + p_tolerance) {
			return (int)i;
		}
	}
	return -1;
}

int GroundSurface3D::get_mat_under_wheel(Vector3 p_center, float p_radius, float &r_support) const {
	// A wheel bridges the gaps of a corduroy: it rests on any log whose round top it touches, at the height where
	// circle meets circle, h = y_axis + sqrt((R + r)^2 - dx^2) - R for a horizontal offset dx across the log.
	int best = -1;
	r_support = -1e9f;
	for (uint32_t i = 0; i < mat_list.size(); i++) {
		const Mat &m = mat_list[i];
		if (m.broken || m.buried) {
			continue;
		}
		const Vector3 ab = m.b - m.a;
		const float t = (p_center - m.a).dot(ab) / MAX(ab.length_squared(), 1e-6f);
		if (t < 0.0f || t > 1.0f) {
			continue;
		}
		const Vector3 on = m.a + ab * t;
		const float dx = Vector2(p_center.x - on.x, p_center.z - on.z).length();
		const float reach = p_radius + m.radius;
		if (dx >= 0.9f * reach) {
			continue;
		}
		const float h = on.y + Math::sqrt(reach * reach - dx * dx) - p_radius;
		if (h > r_support) {
			r_support = h;
			best = (int)i;
		}
	}
	return best;
}

void GroundSurface3D::add_mat_load(int p_mat, float p_load) {
	if (p_mat >= 0 && p_mat < (int)mat_list.size()) {
		mat_list[p_mat].load += MAX(p_load, 0.0f);
	}
}

float GroundSurface3D::get_mat_sinkage(int p_mat) const {
	return p_mat >= 0 && p_mat < (int)mat_list.size() ? mat_list[p_mat].sink : 0.0f;
}

static float _mat_lambda(const GroundSoil &p_soil, float p_radius) {
	const float d = 2.0f * MAX(p_radius, 0.005f);
	const float ks = 1000.0f * (p_soil.kc / d + p_soil.kphi);
	const float ei = 9.0e9f * (float)Math::PI * Math::pow(MAX(p_radius, 0.005f), 4.0f) / 4.0f;
	return MAX(Math::pow(ks * d / (4.0f * ei), 0.25f), 1e-3f);
}

static float _mat_spread(const GroundSoil &p_soil, float p_radius, float p_length) {
	// A log on soft ground is a beam on an elastic foundation (Hetényi 1946): a point load bends it over about
	// pi / lambda, lambda = (k_s d / (4 E I))^(1/4), so a stiff log on soft mud spreads a wheel's load over its whole
	// length and a twig only over a hand's width. k_s from Bekker's modulus (kc / d + kphi, kN/m^(n+2), taken as
	// linear), E 9 GPa for green wood (USDA Wood Handbook, 2021), I = pi r^4 / 4.
	return MIN(p_length, (float)Math::PI / _mat_lambda(p_soil, p_radius));
}

float GroundSurface3D::get_mat_spread(int p_mat) const {
	if (p_mat < 0 || p_mat >= (int)mat_list.size()) {
		return 0.0f;
	}
	const Mat &m = mat_list[p_mat];
	return _mat_spread(get_soil((m.a + m.b) * 0.5f), m.radius, m.length);
}

void GroundSurface3D::_update_mats(float p_dt) {
	if (!mats || !field.valid || !is_inside_tree()) {
		mat_list.clear();
		return;
	}
	// Bodies newly settled on soft ground: the ground takes them.
	for (Node *node : get_tree()->get_nodes_in_group(SNAME("ground_mats"))) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(node);
		if (!rb || rb->is_freeze_enabled()) {
			continue;
		}
		Vector3 a, b;
		float r = 0.05f;
		if (!_mat_shape(rb, a, b, r)) {
			continue;
		}
		const Vector3 c = (a + b) * 0.5f;
		const bool still = rb->is_sleeping() || (rb->get_linear_velocity().length() < 0.15f && rb->get_angular_velocity().length() < 0.5f);
		const bool down = MIN(a.y, b.y) - r <= get_surface_height_at(c) + contact_tolerance + 0.05f;
		if (!still || !down || !get_soil(c).soft) {
			continue;
		}
		rb->set_freeze_mode(RigidBody3D::FREEZE_MODE_KINEMATIC);
		rb->set_freeze_enabled(true);
		Mat m;
		m.id = rb->get_instance_id();
		if (rb->get_physics_material_override().is_valid()) {
			m.friction = rb->get_physics_material_override()->get_friction();
		}
		m.radius = r;
		m.length = a.distance_to(b);
		m.a = a;
		m.b = b;
		mat_list.push_back(m);
	}
	for (uint32_t i = 0; i < mat_list.size();) {
		Mat &m = mat_list[i];
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(m.id));
		if (!rb || !rb->is_inside_tree() || !rb->is_freeze_enabled()) {
			mat_list.remove_at_unordered(i);
			continue;
		}
		Vector3 a, b;
		float r = m.radius;
		_mat_shape(rb, a, b, r);
		const Vector3 c = (a + b) * 0.5f;
		Vector3 d = b - a;
		d.y = 0.0f;
		const float lh = MAX(d.length(), 1e-3f);
		d /= lh;
		GroundSoil soil = get_soil(c);
		// The wheels on it (the heaviest pass presses deepest) and its own weight, spread along the length that bends
		// into the soil.
		m.carried += (m.load - m.carried) * MIN(p_dt / 0.2f, 1.0f);
		m.peak = MAX(m.peak, m.carried);
		m.load = 0.0f;
		// It snaps where the bending under a wheel passes green wood's strength: the beam's moment under a point load,
		// P / (4 lambda) (Hetényi 1946), against MOR pi r^3 / 4 with MOR about 40 MPa (USDA Wood Handbook, green).
		if (!m.broken && soil.soft && m.peak > 0.0f) {
			const float moment = m.peak / (4.0f * _mat_lambda(soil, m.radius));
			if (moment > 40.0e6f * (float)Math::PI * Math::pow(m.radius, 3.0f) / 4.0f) {
				m.broken = true;
			}
		}
		// A debris flow's deposit deeper than the log is thick buries it (round 13): out of sight, out from under the
		// wheels, and no longer lifted onto the raised ground.
		if (!m.buried && get_mud_deposit_at(c) >= 2.0f * m.radius) {
			m.buried = true;
			rb->set_visible(false);
			rb->set_collision_layer(0);
		}
		if (m.buried) {
			i++;
			continue;
		}
		const float w = m.peak + rb->get_mass() * 9.81f;
		if (soil.soft) {
			const float spread = _mat_spread(soil, m.radius, m.length);
			const float dia = 2.0f * m.radius;
			const float k = GroundTerramechanics::bekker_k(soil, dia);
			const float p = w / 1000.0f / (spread * dia);
			const float z = MIN(GroundTerramechanics::layered_sinkage(soil, Math::pow(p / MAX(k, 1e-3f), 1.0f / CLAMP(soil.n, 0.1f, 2.9f)), dia), soil.max_sinkage);
			// Settles over a second, as the soil yields.
			m.sink += (MAX(m.sink, z) - m.sink) * MIN(p_dt, 1.0f);
			if (m.sink > m.pressed + 0.002f) {
				press_wheel(c, Vector2(dia, m.length), Math::atan2(d.x, d.z), m.sink);
				m.pressed = m.sink;
			}
		}
		// It lies on its highest supports along each half: the deformed ground (its own print, the ruts' floors and
		// berms), raised by its radius.
		float ya = -1e9f;
		float yb = -1e9f;
		for (int k = 0; k <= 8; k++) {
			const float t = float(k) / 8.0f;
			const Vector3 q = a.lerp(b, t);
			const float h = get_surface_height_at(q);
			if (t <= 0.5f) {
				ya = MAX(ya, h);
			}
			if (t >= 0.5f) {
				yb = MAX(yb, h);
			}
		}
		const Vector3 na(a.x, ya + m.radius, a.z);
		const Vector3 nb(b.x, yb + m.radius, b.z);
		if (na.distance_to(a) > 0.002f || nb.distance_to(b) > 0.002f) {
			// Turn the body so its axis runs from na to nb and move it so the axis' centre lies there.
			const Vector3 from = (b - a).normalized();
			const Vector3 to = (nb - na).normalized();
			Transform3D t = rb->get_global_transform();
			const Vector3 axis = from.cross(to);
			if (axis.length() > 1e-5f) {
				const float ang = Math::acos(CLAMP(from.dot(to), -1.0f, 1.0f));
				const Basis rot(axis.normalized(), ang);
				t.basis = rot * t.basis;
				t.origin = c + rot.xform(t.origin - c);
			}
			t.origin += (na + nb) * 0.5f - c;
			rb->set_global_transform(t);
		}
		m.a = na;
		m.b = nb;
		i++;
	}
}

// Slip-face avalanches (docs/ideas/sand.md, dunes). A dune's lee face rests between the angle a dry granular layer
// stops flowing at and the one it starts at, a degree or two apart (Daerr and Douady 1999, "Two types of avalanche
// behaviour in granular media", Nature 399; Bagnold's two repose angles). Left alone it stays; a foot or a wheel
// pressing into it is the disturbance that sets the surface layer running. Dune grain flows are tongues a few tens
// of centimetres wide that widen downslope, scour a centimetre or two from the upper face and run to the toe, where
// they stop as a lobe (McDonald and Anderson 1996; Kleinhans 2005; Breton et al. 2008). Built as an event, as the
// flow is: from the press the path follows the fall line down the undisturbed ground until it eases under the
// stopping angle (the toe); the scour runs along it at a grain flow's speed (about 1 m/s) taking a tapering layer,
// and what it took is laid in the lobe once the front reaches the toe. Sand is kept: the lobe gets what was dug.
// Only dry, loose sand (repose 30 to 36 degrees) on ground whose own slope is between the stopping angle (two degrees
// under the repose) and a little over the repose; a steeper face is held by the terrain and stays.
static float _aval_hash(uint32_t p_a, uint32_t p_b) {
	uint32_t h = p_a * 747796405u + p_b * 2891336453u + 1u;
	h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
	h = (h >> 22u) ^ h;
	return float(h) * (1.0f / 4294967296.0f);
}

void GroundSurface3D::_avalanche_trigger(const Vector3 &p_position, float p_radius, float p_bottom) {
	if (!slip_avalanches || !field.valid || !ground_valid) {
		return;
	}
	const float gh = _ground_at(p_position.x, p_position.z);
	if (p_bottom >= gh - 0.002f) {
		return; // not into the ground
	}
	const Cell &c = _cell_at(p_position.x, p_position.z);
	if (!c.deformable || c.sand < 0.5f || c.tan_repose < 0.58f || c.tan_repose > 0.73f) {
		return;
	}
	const float tan_stop = Math::tan(Math::atan(c.tan_repose) - Math::deg_to_rad(2.0f));
	auto grad = [this](float p_x, float p_z) {
		const float h = 0.15f;
		return Vector2(_ground_at(p_x + h, p_z) - _ground_at(p_x - h, p_z), _ground_at(p_x, p_z + h) - _ground_at(p_x, p_z - h)) / (2.0f * h);
	};
	const Vector2 g0 = grad(p_position.x, p_position.z);
	const float s0 = g0.length();
	if (s0 < tan_stop || s0 > 1.15f * c.tan_repose) {
		return;
	}
	// One flow per patch of face for a while: a foot pressing on frame after frame is one disturbance.
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const Vector2 at(p_position.x, p_position.z);
	for (int i = int(avalanche_marks.size()) - 1; i >= 0; i--) {
		if (now - avalanche_marks[i].usec > 4000000) {
			avalanche_marks.remove_at_unordered(i);
		} else if (avalanche_marks[i].at.distance_to(at) < 0.4f) {
			return;
		}
	}
	if (avalanches.size() >= 16) {
		return;
	}
	AvalancheMark m;
	m.at = at;
	m.usec = now;
	avalanche_marks.push_back(m);
	Avalanche a;
	a.seed = uint32_t(int(Math::floor(p_position.x * 37.0f)) * 73856093) ^ uint32_t(int(Math::floor(p_position.z * 37.0f)) * 19349663);
	// The fall line, 4 cm steps, until the ground eases under the stopping angle (well under: a slip face's own
	// texture) or 15 m. The tongue starts the print's width and widens down the face.
	const float step = 0.04f;
	const float w0 = CLAMP(p_radius, 0.08f, 0.4f);
	// Each edge wanders on its own (two sine octaves with random phases and rates), the line meanders, and each flow
	// opens at its own angle, 3 to 10 degrees a side: no two tongues alike.
	const float open = Math::tan(Math::deg_to_rad(3.0f + 7.0f * _aval_hash(a.seed, 4u)));
	auto wander = [&a](float p_s, uint32_t p_salt) {
		const float f1 = 0.8f + 1.2f * _aval_hash(a.seed, p_salt);
		const float f2 = 3.0f + 3.0f * _aval_hash(a.seed, p_salt + 1u);
		return 0.65f * Math::sin(6.2831853f * (f1 * p_s + _aval_hash(a.seed, p_salt + 2u))) + 0.35f * Math::sin(6.2831853f * (f2 * p_s + _aval_hash(a.seed, p_salt + 3u)));
	};
	Vector2 line = at;
	for (int k = 0; k < 375; k++) {
		const Vector2 g = grad(line.x, line.y);
		const float sl = g.length();
		if (k > 0 && sl < 0.85f * tan_stop) {
			break;
		}
		const Cell &ck = _cell_at(line.x, line.y);
		if (!ck.deformable || ck.sand < 0.5f) {
			break;
		}
		const float s = k * step;
		const Vector2 down = sl > 1e-5f ? -g / sl : Vector2(1.0f, 0.0f);
		const Vector2 across(-down.y, down.x);
		const float spread = s * open;
		const float left = w0 + spread * (1.0f + 0.45f * wander(s, 10u)) + 0.02f * wander(s, 20u);
		const float right = w0 + spread * (1.0f + 0.45f * wander(s, 30u)) + 0.02f * wander(s, 40u);
		const float half = MAX(0.5f * (left + right), 0.04f);
		const Vector2 c = line + across * (0.5f * (right - left) + 0.06f * MIN(s, 1.0f) * wander(0.3f * s, 50u));
		a.path.push_back(Vector3(c.x, half, c.y));
		line += down * step;
	}
	const int n = (int)a.path.size();
	if (n < 8) {
		return;
	}
	// The toe: the flow piles up where the face eases (Breton et al. 2008; field lobes 2 to 5 cm thick): the lobe is
	// the run's last 25 cm, bulging a little wider, and a rounded snout 28 cm on past it onto the easing ground,
	// narrowing to its tip. Its thickness rises over the lobe's first quarter and holds to the snout.
	a.lobe_from = MAX(n - MAX(int(0.25f / step), n / 12), 1);
	for (int k = a.lobe_from; k < n; k++) {
		a.path[k].y *= 1.0f + 0.15f * Math::smoothstep(0.0f, 1.0f, float(k - a.lobe_from) / MAX(float(n - a.lobe_from), 1.0f));
	}
	const Vector3 last = a.path[n - 1];
	const Vector3 prev = a.path[n - 2];
	const Vector2 dir = Vector2(last.x - prev.x, last.z - prev.z).normalized();
	const int snout = 7;
	for (int k = 1; k <= snout; k++) {
		const Vector2 q = Vector2(last.x, last.z) + dir * (step * k);
		const float u = float(k) / float(snout + 1);
		a.path.push_back(Vector3(q.x, MAX(last.y * Math::sqrt(1.0f - u * u), 0.04f), q.y));
	}
	for (int k = a.lobe_from; k < (int)a.path.size(); k++) {
		a.lobe_area += _aval_lobe_weight(a, k) * AVAL_TENT * AVAL_TENT * a.path[k].y * a.path[k].y; // tents (_avalanche_update())
	}
	avalanches.push_back(a);
}

// A lobe step's share of the deposit: rising over the lobe's first quarter, so its upslope end feathers into the scar.
float GroundSurface3D::_aval_lobe_weight(const Avalanche &p_a, int p_k) {
	const float u = float(p_k - p_a.lobe_from) / MAX(float((int)p_a.path.size() - p_a.lobe_from), 1.0f);
	return 0.15f + 0.85f * Math::smoothstep(0.0f, 0.25f, u);
}

void GroundSurface3D::_avalanche_update(double p_delta) {
	if (avalanches.is_empty()) {
		return;
	}
	const float step = 0.04f;
	// Tents 1.6 times the flow's half width, not flat discs: discs 4 cm apart left each one's edge as a terrace,
	// a row of arcs down the tongue like contour lines.
	field.amount_tent = true;
	// The face's sand under the flow is the same dry sand: the scar shows by its relief, barely by tint (0..1 for a
	// dig: GroundDeformField::amount_fresh).
	const float fresh_before = field.amount_fresh;
	field.amount_fresh = 0.25f;
	for (int i = int(avalanches.size()) - 1; i >= 0; i--) {
		Avalanche &a = avalanches[i];
		// A grain flow runs at about a metre a second (a few cm thick on a slip face; Breton et al. 2008).
		a.front += float(p_delta) * 1.0f;
		const int n = (int)a.path.size();
		const int reach = MIN(int(a.front / step), n);
		while (a.done < reach) {
			const int k = a.done++;
			const Vector3 q = a.path[k];
			const float r = MAX(q.y, 0.05f);
			const Vector3 at(q.x, _ground_at(q.x, q.z), q.z);
			if (k < a.lobe_from) {
				// The scour: 2.5 cm thinning to 8 mm down the face, as an area-averaged layer over discs 4 cm apart
				// (each point under about 2r / 4 cm of them).
				const float t = float(k) / MAX(float(a.lobe_from), 1.0f);
				const float layer = Math::lerp(0.025f, 0.008f, t) * (0.8f + 0.4f * _aval_hash(a.seed, 100u + uint32_t(k / 5)));
				const float rt = AVAL_TENT * r;
				const float depth = layer * step * 2.0f * r / (rt * rt); // a tent moves rt^2 depth
				a.scoured += rt * rt * depth;
				_amount(at, rt, depth, 0.0f);
			} else if (a.lobe_area > 0.0f) {
				// The lobe: what the scour took, spread over its tents, a share each by its area.
				const float depth = a.scoured * _aval_lobe_weight(a, k) / a.lobe_area;
				_amount(at, AVAL_TENT * r, -depth, 0.0f);
			}
		}
		if (a.done >= n) {
			avalanches.remove_at(i);
		}
	}
	field.amount_tent = false;
	field.amount_fresh = fresh_before;
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
	if (!is_near) {
		surface->_avalanche_trigger(p_position, p_box ? p_half.length() : p_radius, p_bottom);
	}
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
	const uint32_t flags = (p_box ? 1u : 0u) | (p_round ? 2u : 0u) | ((uint32_t)CLAMP(surface->field.press_tread, 0, 7) << 5u);
	const float v[12] = {
		p_position.x, p_position.y, p_position.z, p_radius,
		p_half.x, p_half.y, p_yaw, p_bottom,
		p_sink, p_contact_tolerance, p_velocity.x, p_velocity.y
	};
	const float settle = MAX(surface->field.press_settle, 0.0f);
	const float d[4] = { 0.0f, float(flags), 0.0f, settle };
	const Vector4 &fa = surface->field.press_floor_a;
	const Vector3 &fb = surface->field.press_floor_b;
	const float fl[8] = { fa.x, fa.y, fa.z, fa.w, fb.x, fb.y, fb.z, 0.0f };
	host.add_stamp(f, v, d, x0, x1, z0, z1, fl);
	host.slump_until = MAX(host.slump_until, host.time + settle + 0.1);
}

void GroundSurface3D::GPUSink::amount(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, float p_depth, float p_cell) {
	// On the near field a grid's cell is a tent one cell wide (GroundDeformField::amount_cover()); on either with
	// amount_tent a tent p_radius wide.
	const bool main_tent = surface->field.amount_tent;
	const bool tent = (is_near && p_cell > 0.0f) || main_tent;
	if (next && surface->is_near_active()) {
		// The depth was scaled for the main field's texels (amount_scale): the near field's own scale instead, the
		// same volume.
		const float main_scale = surface->field.amount_scale(p_position, p_radius, main_tent);
		const float depth = main_scale > 0.0f ? p_depth / main_scale : p_depth;
		if (p_cell > 0.0f) {
			const float to_cell = (main_tent ? 1.0f : (float)Math::PI) * p_radius * p_radius / (p_cell * p_cell);
			next->amount(p_columns, p_position, p_cell, depth * to_cell * surface->near_fld.amount_scale(p_position, p_cell, true), p_cell);
		} else {
			next->amount(p_columns, p_position, p_radius, depth * surface->near_fld.amount_scale(p_position, p_radius, main_tent), 0.0f);
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
		surface->field.amount_print.x, surface->field.amount_print.y, surface->field.amount_print.z, surface->field.amount_print.w,
		p_depth, tent ? 1.0f : 0.0f, surface->field.amount_fresh, 0.0f
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
	_ice_break(p_position, p_radius, 1e9f); // a dig breaks any ice
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	if (_gpu_on()) {
		gpu_sink.amount(cols, p_position, p_radius, p_depth * field.amount_scale(p_position, p_radius, field.amount_tent), p_cell);
		// The GPU applies it; what it moves is the disc's, short only where the dent bottoms out.
		return c.deformable ? (float)Math::PI * p_radius * p_radius * Math::abs(p_depth) : 0.0f;
	}
	return field.add_amount(cols, p_position, p_radius, p_depth, p_cell);
}

float GroundSurface3D::dig(Vector3 p_position, float p_radius, float p_depth) {
	return p_depth > 0.0f ? _amount(p_position, p_radius, p_depth, 0.0f) : 0.0f;
}

// A puddle where a scene or a world generator wants one: a hollow of overlapping lobes dug into the ground (an
// irregular, rounded rim, no spoil), below a level the lowest point of its rim sets (on a slope, a low bank laid on
// the downhill side holds it), so its water stands level; then, once the field holds the hollow, filled to that level with water
// carrying p_mud kg/m3 of clay. The ground slumps its walls to their repose as it would any dig.
void GroundSurface3D::add_puddle(const Vector3 &p_position, float p_radius, float p_depth, float p_mud) {
	_add_puddle(p_position, p_radius, p_depth, p_mud, true);
}

void GroundSurface3D::_add_puddle(const Vector3 &p_position, float p_radius, float p_depth, float p_mud, bool p_fill) {
	if (p_radius <= 0.0f || p_depth <= 0.0f || !field.valid) {
		return;
	}
	const float r = p_radius;
	float rim_low = 1e30f;
	float rim[16];
	for (int k = 0; k < 16; k++) {
		const float a = Math::TAU * k / 16.0f;
		rim[k] = _ground_at(p_position.x + 1.1f * r * Math::cos(a), p_position.z + 1.1f * r * Math::sin(a));
		rim_low = MIN(rim_low, rim[k]);
	}
	// The water's level: the rim's lowest point, or on a slope half the depth below the middle's ground, held by a
	// low bank of soil laid along the downhill rim (a puddle on a slope is dammed: a track's own berm, a root).
	const float level = MAX(rim_low - 0.005f, _ground_at(p_position.x, p_position.z) - 0.5f * p_depth);
	// The bank is a continuous berm: discs a seventh of the rim apart, each two-fifths of the bank's height (they
	// overlap about two and a half deep), 2 cm over the water. Where the rim stands higher it needs none: on the flat
	// a lip of a few millimetres, no ring of mounds.
	const int banks = 48;
	for (int k = 0; k < banks; k++) {
		const float a = Math::TAU * k / float(banks);
		const float rk = Math::lerp(rim[(k / 3) % 16], rim[(k / 3 + 1) % 16], float(k % 3) / 3.0f);
		const float bank = level + 0.02f - rk;
		if (bank > 0.0f) {
			deposit(Vector3(p_position.x + 1.1f * r * Math::cos(a), 0.0f, p_position.z + 1.1f * r * Math::sin(a)), 0.15f * r, 0.4f * bank);
		}
	}
	// Lobes from the position's own hash: the same puddle each time it is asked for there.
	RandomPCG rng(uint64_t(hash_murmur3_one_float(p_position.x, hash_murmur3_one_float(p_position.z))));
	auto lobe = [&](float x, float z, float rr, float d) {
		const float below = MAX(_ground_at(x, z) - level, 0.0f);
		dig(Vector3(x, 0.0f, z), rr, below + d);
	};
	// A centre and a ring of lobes that do not reach it (so the depth does not stack), shallower towards the rim.
	lobe(p_position.x, p_position.z, 0.6f * r, p_depth);
	const int lobes = 6;
	const float a0 = rng.randf() * Math::TAU;
	for (int k = 0; k < lobes; k++) {
		const float a = a0 + Math::TAU * (k + 0.35f * (rng.randf() - 0.5f)) / lobes;
		const float d = r * (0.45f + 0.1f * rng.randf());
		const float rr = MIN(r * (0.3f + 0.15f * rng.randf()), r - d);
		lobe(p_position.x + d * Math::cos(a), p_position.z + d * Math::sin(a), rr, p_depth * (0.4f + 0.3f * rng.randf()));
	}
	if (!p_fill) {
		return;
	}
	PendingPuddle pp;
	pp.position = p_position;
	pp.radius = r;
	pp.level = level;
	pp.mud = MAX(p_mud, 0.0f);
	pp.wait = 2.0; // dug, and its walls slumped to their repose
	pending_puddles.push_back(pp);
}

// How well a site suits a puddle, 0..1: where water gathers (the data map's wetness index and the flats by water, as
// the deep mud's) or, without a data map, a hollow in the ground lattice; flat, bare soil only (no sand, rock, litter).
float GroundSurface3D::_world_puddle_suit(const Vector3 &p_position) const {
	const int n = ground_n;
	if (!ground_valid || (int)cell_inputs.size() != n * n) {
		return 0.0f;
	}
	const int cx = int(Math::round(p_position.x / ground_spacing)) - ground_origin.x;
	const int cz = int(Math::round(p_position.z / ground_spacing)) - ground_origin.y;
	if (cx < 0 || cz < 0 || cx >= n || cz >= n) {
		return 0.0f;
	}
	if ((int)cells.size() != n * n) {
		return 0.0f;
	}
	const CellInput &in = cell_inputs[cz * n + cx];
	if (in.fixed || in.sand > 0.5f || in.litter > 0.01f || !cells[cz * n + cx].deformable) {
		return 0.0f;
	}
	const float h0 = _ground_at(p_position.x, p_position.z);
	float ring = 0.0f;
	float lo = 1e30f;
	float hi = -1e30f;
	for (int k = 0; k < 8; k++) {
		const float a = Math::TAU * k / 8.0f;
		const float h = _ground_at(p_position.x + 2.5f * Math::cos(a), p_position.z + 2.5f * Math::sin(a));
		ring += h / 8.0f;
		lo = MIN(lo, h);
		hi = MAX(hi, h);
	}
	const float flat = 1.0f - Math::smoothstep(0.25f, 0.9f, hi - lo); // about 6 to 20 degrees across 5 m
	float suit = Math::smoothstep(0.02f, 0.15f, ring - h0);
	const Node *dmap = data_map.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(data_map);
	if (const TerrainDataMap3D *dm = Object::cast_to<TerrainDataMap3D>(dmap)) {
		const float twi = dm->get_wetness_at(p_position);
		const float dist = dm->get_water_distance_at(p_position);
		suit = MAX(suit, MAX(Math::smoothstep(8.0f, 13.0f, twi), 1.0f - Math::smoothstep(0.5f, 4.0f, dist)));
		if (dist < 0.5f) {
			return 0.0f; // in the lake itself
		}
	}
	return suit * flat;
}

// Puddles the world places (mud round 6): MudRunner's maps are dotted with them where water would gather. Sites on a
// 4 m grid, each with its own hash: kept where the ground suits (_world_puddle_suit) at world_puddle_density; a
// site seen inside the field for a second is dug (add_puddle's hollow) and filled when the ground round it is wet,
// left a dry hollow (which cracks) when it is not. Rain and runoff fill it later like any hollow. A site the field
// has left is forgotten; when it returns, a hollow already there (kept ruts) is not dug again.
void GroundSurface3D::_world_puddles_update(double p_delta) {
	world_puddle_clock += p_delta;
	if (!world_puddles || world_puddle_density <= 0.0f || !field.valid || !ground_valid || !standing_water || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const float G = 4.0f;
	const Rect2 r = field.rect().grow(-3.0f);
	const Vector2i c0(int(Math::floor(r.position.x / G)), int(Math::floor(r.position.y / G)));
	const Vector2i c1(int(Math::floor((r.position.x + r.size.x) / G)), int(Math::floor((r.position.y + r.size.y) / G)));
	LocalVector<Vector2i> gone;
	for (const KeyValue<Vector2i, double> &kv : world_puddle_seen) {
		if (kv.key.x < c0.x || kv.key.y < c0.y || kv.key.x > c1.x || kv.key.y > c1.y) {
			gone.push_back(kv.key);
		}
	}
	for (const Vector2i &g : gone) {
		world_puddle_seen.erase(g);
	}
	const int T = GroundDeformField::TILE;
	for (int gz = c0.y; gz <= c1.y; gz++) {
		for (int gx = c0.x; gx <= c1.x; gx++) {
			const Vector2i g(gx, gz);
			double *seen = world_puddle_seen.getptr(g);
			if (!seen) {
				world_puddle_seen.insert(g, world_puddle_clock);
				continue;
			}
			if (*seen < 0.0 || world_puddle_clock - *seen < 1.0) {
				continue; // done, or wait for the field and any kept tile under it to stand
			}
			*seen = -1.0;
			uint32_t hsh = hash_murmur3_one_32(uint32_t(gx) * 73856093u ^ uint32_t(gz) * 19349663u);
			RandomPCG rng(hsh);
			const float u = rng.randf();
			const Vector3 p((gx + 0.2f + 0.6f * rng.randf()) * G, 0.0f, (gz + 0.2f + 0.6f * rng.randf()) * G);
			const float radius = 0.6f + 1.0f * rng.randf();
			const float depth = 0.05f + 0.07f * rng.randf();
			if (u >= world_puddle_density * (G * G / 100.0f) * _world_puddle_suit(p)) {
				continue;
			}
			const Vector2i tile(int(Math::floor(p.x / field.texel())) / T, int(Math::floor(p.z / field.texel())) / T);
			if (rut_main_pending.has(tile) || get_dent_at(p) > 0.4f * depth) {
				continue; // already dug (a kept tile), or coming back from disk
			}
			const int n = ground_n;
			const int cx = CLAMP(int(Math::round(p.x / ground_spacing)) - ground_origin.x, 0, n - 1);
			const int cz = CLAMP(int(Math::round(p.z / ground_spacing)) - ground_origin.y, 0, n - 1);
			const bool wet = cell_inputs[cz * n + cx].moisture >= mud_wet_start;
			_add_puddle(p, radius, depth, 15.0f, wet);
		}
	}
}

void GroundSurface3D::_puddles_update(double p_delta) {
	for (int k = int(pending_puddles.size()) - 1; k >= 0; k--) {
		PendingPuddle &pp = pending_puddles[k];
		pp.wait -= p_delta;
		if (pp.wait > 0.0) {
			continue;
		}
		// The water the hollow holds up to the level, less what already stands in it.
		const float t = field.texel();
		const float reach = 1.2f * pp.radius;
		double vol = 0.0;
		for (float x = Math::floor((pp.position.x - reach) / t) * t + 0.5f * t; x <= pp.position.x + reach; x += t) {
			for (float z = Math::floor((pp.position.z - reach) / t) * t + 0.5f * t; z <= pp.position.z + reach; z += t) {
				if (Vector2(x - pp.position.x, z - pp.position.z).length() > reach) {
					continue;
				}
				const Vector3 q(x, 0.0f, z);
				const float fill = pp.level - 0.002f - get_surface_height_at(q) - get_standing_water_at(q);
				if (fill > 0.0f) {
					vol += double(fill) * t * t;
				}
			}
		}
		if (vol > 0.0) {
			add_water(pp.position, 0.7f * pp.radius, float(vol), pp.mud);
		}
		pending_puddles.remove_at_unordered(k);
	}
}

float GroundSurface3D::deposit(Vector3 p_position, float p_radius, float p_depth) {
	return p_depth > 0.0f ? _amount(p_position, p_radius, -p_depth, 0.0f) : 0.0f;
}

// Ruts kept across the world (docs/ideas/mud.md, round 3). MudRunner and SnowRunner keep a map's deformation in its
// save; this keeps the field's tiles (dent, rim, tint and standing water) on disk as it moves off them, the way a
// virtual texture pages its tiles out, and reads them back in when it returns. The main and near fields each keep
// their own, named by texel.
bool GroundSurface3D::_ruts_on() const {
	return keep_ruts && !ruts_path.is_empty() && !Engine::get_singleton()->is_editor_hint();
}

void GroundSurface3D::_ruts_configure() {
	const String dir = _ruts_on() ? ruts_path : String();
	rut_main.configure(dir, "m", field.texel());
	rut_near.configure(dir, "n", near_fld.texel());
	crack_store.configure(desiccation_cracks ? dir : String(), "c", ground_spacing);
	if (dir != wear_loaded_dir) {
		wear_loaded_dir = dir;
		_wear_load();
		_deposits_load();
	}
}

// Worn tracks kept with the ruts (docs/ideas/mud.md, round 15.3): the passes of every worn 25 cm cell, written to
// one file (zstd) in ruts_path when the ruts are saved, and read back when the folder is first used, aged for the
// time away as the sward would have regrown over them (wear_regrowth_days' half-life at ruts_time_scale). MudRunner
// and SnowRunner keep a map's deformation in its save; a track driven in yesterday is still a track today.
static const char *WEAR_FILE = "worn_tracks.wear";
static constexpr uint32_t WEAR_MAGIC = 0x52414557; // "WEAR"

void GroundSurface3D::_wear_save() {
	if (!_ruts_on() || !worn_tracks) {
		return;
	}
	const String path = ruts_path.path_join(WEAR_FILE);
	if (wear_cells.is_empty()) {
		if (FileAccess::exists(path)) {
			DirAccess::remove_absolute(path);
		}
		return;
	}
	if (!DirAccess::dir_exists_absolute(ruts_path)) {
		DirAccess::make_dir_recursive_absolute(ruts_path);
	}
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null()) {
		return;
	}
	f->store_32(WEAR_MAGIC);
	f->store_32(1);
	f->store_double(Time::get_singleton()->get_unix_time_from_system());
	f->store_32(wear_cells.size());
	for (const KeyValue<Vector2i, Wear> &kv : wear_cells) {
		f->store_32(uint32_t(kv.key.x));
		f->store_32(uint32_t(kv.key.y));
		f->store_float(kv.value.passes);
	}
}

// Deposits kept with the ruts (docs/ideas/mud.md, round 16.2): what floods, rivers and debris flows laid (depth and
// colour per 1 m cell, the silt skin's share of it, and how much of each river record the ground has taken, so a river
// does not lay it again), and the stones wheels knocked loose. One zstd file, written with the ruts and read back the
// first time the folder is used. How dry a skin is comes back with the crack state (GroundCellStore); a deposit does
// not wear away while the player is away (fresh silt and fans last seasons, Asselman and Middelkoop 1995).
static const char *DEPOSIT_FILE = "deposits.dep";
static constexpr uint32_t DEPOSIT_MAGIC = 0x4f504544; // "DEPO"

void GroundSurface3D::_deposits_save() {
	if (!_ruts_on()) {
		return;
	}
	const String path = ruts_path.path_join(DEPOSIT_FILE);
	HashSet<Vector2i> keys;
	for (const KeyValue<Vector2i, float> &kv : mud_deposits) {
		keys.insert(kv.key);
	}
	for (const KeyValue<Vector2i, Vector2> &kv : silt_river_taken) {
		keys.insert(kv.key);
	}
	if (keys.is_empty() && buried_loose.is_empty() && buried_extra.is_empty() && film_cells.is_empty() && loose_pending.is_empty() && buried_loose_bodies.is_empty()) {
		if (FileAccess::exists(path)) {
			DirAccess::remove_absolute(path);
		}
		return;
	}
	if (!DirAccess::dir_exists_absolute(ruts_path)) {
		DirAccess::make_dir_recursive_absolute(ruts_path);
	}
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null()) {
		return;
	}
	f->store_32(DEPOSIT_MAGIC);
	f->store_32(1);
	f->store_32(keys.size());
	for (const Vector2i &k : keys) {
		const float *dep = mud_deposits.getptr(k);
		const float *silt = silt_depth.getptr(k);
		const Color *col = mud_deposit_colors.getptr(k);
		const Vector2 *taken = silt_river_taken.getptr(k);
		f->store_32(uint32_t(k.x));
		f->store_32(uint32_t(k.y));
		f->store_float(dep ? *dep : 0.0f);
		f->store_float(silt ? *silt : 0.0f);
		const Color c = col ? *col : Color(0, 0, 0, 0);
		f->store_float(c.r);
		f->store_float(c.g);
		f->store_float(c.b);
		f->store_float(c.a);
		f->store_float(taken ? taken->x : 0.0f);
		f->store_float(taken ? taken->y : 0.0f);
	}
	f->store_32(buried_loose.size());
	for (uint32_t id : buried_loose) {
		f->store_32(id);
	}
	// Stones sunk back and hidden again (round 17.2).
	uint32_t extras = 0;
	for (const KeyValue<Vector2i, LocalVector<Buried>> &kv : buried_extra) {
		extras += kv.value.size();
	}
	f->store_32(extras);
	for (const KeyValue<Vector2i, LocalVector<Buried>> &kv : buried_extra) {
		for (const Buried &b : kv.value) {
			f->store_32(b.id);
			f->store_float(b.a.x);
			f->store_float(b.a.y);
			f->store_float(b.a.z);
			f->store_float(b.r);
		}
	}
	// The 1 m cells tracked mud lies in, and the film owed to the dust already lifted off them (round 18.3), so wind
	// still finds a trail after a reload.
	f->store_32(film_cells.size());
	for (const Vector2i &k : film_cells) {
		const float *owed = film_owed.getptr(k);
		f->store_32(uint32_t(k.x));
		f->store_32(uint32_t(k.y));
		f->store_float(owed ? *owed : 0.0f);
	}
	// The loose stones as they lie (round 19.2): where, how turned, how big and how deep in the mud; and those read
	// back but not yet placed.
	LocalVector<LoosePending> lying;
	for (const LoosePending &lp : loose_pending) {
		lying.push_back(lp);
	}
	for (const KeyValue<uint32_t, ObjectID> &kv : buried_loose_bodies) {
		// Live, or as it lay at the last step (saved as this node leaves the tree, its stones have left it already).
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(kv.value));
		const LooseState *ls = loose_state.getptr(kv.key);
		if (!rb || !ls || !(ls->r > 0.0f)) {
			continue;
		}
		LoosePending lp;
		lp.id = kv.key;
		const Transform3D xf = rb->is_inside_tree() ? rb->get_global_transform() : ls->xf;
		lp.at = xf.origin;
		lp.rot = xf.basis.get_rotation_quaternion();
		lp.r = ls->r;
		lp.z = ls->z;
		lp.ref = ls->ref;
		lying.push_back(lp);
	}
	f->store_32(lying.size());
	for (const LoosePending &lp : lying) {
		f->store_32(lp.id);
		f->store_float(lp.at.x);
		f->store_float(lp.at.y);
		f->store_float(lp.at.z);
		f->store_float(lp.rot.x);
		f->store_float(lp.rot.y);
		f->store_float(lp.rot.z);
		f->store_float(lp.rot.w);
		f->store_float(lp.r);
		f->store_float(lp.z);
		f->store_float(lp.ref);
	}
}

void GroundSurface3D::_deposits_load() {
	if (!_ruts_on()) {
		return;
	}
	const String path = ruts_path.path_join(DEPOSIT_FILE);
	if (!FileAccess::exists(path)) {
		return;
	}
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null() || f->get_32() != DEPOSIT_MAGIC || f->get_32() != 1) {
		return;
	}
	const uint32_t count = f->get_32();
	for (uint32_t i = 0; i < count && !f->eof_reached(); i++) {
		const int32_t kx = int32_t(f->get_32());
		const int32_t kz = int32_t(f->get_32());
		const Vector2i k(kx, kz);
		const float dep = f->get_float();
		const float silt = f->get_float();
		const float r = f->get_float();
		const float g = f->get_float();
		const float b = f->get_float();
		const float a = f->get_float();
		const float tx = f->get_float();
		const float tz = f->get_float();
		if (dep > 0.0f) {
			float &d = mud_deposits[k];
			d = MAX(d, dep);
		}
		if (silt > 0.0f) {
			float &s = silt_depth[k];
			s = MAX(s, silt);
		}
		if (a > 0.0f && !mud_deposit_colors.has(k)) {
			mud_deposit_colors[k] = Color(r, g, b, a);
		}
		if (tx > 0.0f || tz > 0.0f) {
			Vector2 &t = silt_river_taken[k];
			t = t.max(Vector2(tx, tz));
		}
	}
	const uint32_t loose = f->eof_reached() ? 0 : f->get_32();
	for (uint32_t i = 0; i < loose && !f->eof_reached(); i++) {
		buried_loose.insert(f->get_32());
	}
	const uint32_t extras = f->eof_reached() ? 0 : f->get_32();
	for (uint32_t i = 0; i < extras && !f->eof_reached(); i++) {
		Buried b;
		b.id = f->get_32();
		const float x = f->get_float();
		const float y = f->get_float();
		const float z = f->get_float();
		b.a = Vector3(x, y, z);
		b.b = b.a;
		b.r = f->get_float();
		const Vector2i cell(int(Math::floor(x / BURIED_CELL)), int(Math::floor(z / BURIED_CELL)));
		bool have = false;
		for (const Buried &e : buried_extra[cell]) {
			have = have || e.id == b.id;
		}
		if (!have) {
			buried_extra[cell].push_back(b);
		}
	}
	const uint32_t films = f->eof_reached() ? 0 : f->get_32();
	for (uint32_t i = 0; i < films && !f->eof_reached(); i++) {
		const int32_t kx = int32_t(f->get_32());
		const int32_t kz = int32_t(f->get_32());
		const Vector2i k(kx, kz);
		const float owed = f->get_float();
		if (film_cells.size() < 65536) {
			film_cells.insert(k);
		}
		if (owed > 0.0f) {
			film_owed[k] = owed;
		}
	}
	const uint32_t lying = f->eof_reached() ? 0 : f->get_32();
	for (uint32_t i = 0; i < lying && !f->eof_reached(); i++) {
		LoosePending lp;
		lp.id = f->get_32();
		const float x = f->get_float();
		const float y = f->get_float();
		const float z = f->get_float();
		lp.at = Vector3(x, y, z);
		const float qx = f->get_float();
		const float qy = f->get_float();
		const float qz = f->get_float();
		const float qw = f->get_float();
		lp.rot = Quaternion(qx, qy, qz, qw);
		lp.r = f->get_float();
		lp.z = f->get_float();
		lp.ref = f->get_float();
		if (!buried_loose_bodies.has(lp.id) || !ObjectDB::get_instance(buried_loose_bodies[lp.id])) {
			bool have = false;
			for (const LoosePending &e : loose_pending) {
				have = have || e.id == lp.id;
			}
			if (!have && lp.r > 0.0f && lp.rot.is_normalized()) {
				buried_loose.insert(lp.id);
				loose_pending.push_back(lp);
			}
		}
	}
	buried_cache.clear();
	silt_dirty = true;
	gpu_cells_dirty = true;
	near_cells_dirty = true;
	if (ground_valid) {
		_update_cells();
	}
}

void GroundSurface3D::_wear_load() {
	if (!_ruts_on() || !worn_tracks) {
		return;
	}
	const String path = ruts_path.path_join(WEAR_FILE);
	if (!FileAccess::exists(path)) {
		return;
	}
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null() || f->get_32() != WEAR_MAGIC || f->get_32() != 1) {
		return;
	}
	const double when = f->get_double();
	const double away = when > 0.0 ? MAX(Time::get_singleton()->get_unix_time_from_system() - when, 0.0) : 0.0;
	const float keep = wear_regrowth_days > 0.0f ? float(Math::exp(-away * double(ruts_time_scale) * 0.69314718 / (double(wear_regrowth_days) * 86400.0))) : 1.0f;
	const uint32_t count = f->get_32();
	for (uint32_t i = 0; i < count && !f->eof_reached(); i++) {
		const int32_t kx = int32_t(f->get_32());
		const int32_t kz = int32_t(f->get_32());
		const Vector2i k(kx, kz);
		const float passes = f->get_float() * keep;
		if (passes < 0.05f) {
			continue;
		}
		Wear &w = wear_cells[k];
		w.passes = MAX(w.passes, passes);
	}
	wear_dirty = true;
}

// The lattice moves to p_origin (p_n per side; a new size or the first sample starts it over): the cells it leaves
// keep their crack state on disk with the ruts, the cells it comes onto read theirs back, aged for the time away.
void GroundSurface3D::_cracks_move(Vector2i p_origin, int p_n) {
	const int n = ground_n;
	const bool had = ground_valid && n > 0 && (int)crack_state.size() == n * n * CRACK_FLOATS;
	const bool kept = had && n == p_n;
	_ruts_configure();
	const bool store = crack_store.is_on();
	const Vector2i d = p_origin - ground_origin;
	if (had && store) {
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int lx = x - d.x;
				const int lz = z - d.y;
				if (kept && lx >= 0 && lz >= 0 && lx < p_n && lz < p_n) {
					continue; // staying
				}
				crack_store.put(ground_origin + Vector2i(x, z), &crack_state[(z * n + x) * CRACK_FLOATS], CRACK_FLOATS);
			}
		}
	}
	if (kept) {
		_shift_lattice(crack_state, ground_origin, p_origin, p_n, CRACK_FLOATS);
	} else {
		crack_state.clear();
	}
	if (!store || !desiccation_cracks) {
		return;
	}
	float v[CRACK_FLOATS];
	for (int z = 0; z < p_n; z++) {
		for (int x = 0; x < p_n; x++) {
			const int ox = x + d.x;
			const int oz = z + d.y;
			if (kept && ox >= 0 && oz >= 0 && ox < p_n && oz < p_n) {
				continue; // was in the lattice already
			}
			double age = 0.0;
			if (!crack_store.get(p_origin + Vector2i(x, z), v, CRACK_FLOATS, &age)) {
				continue;
			}
			if ((int)crack_state.size() != p_n * p_n * CRACK_FLOATS) {
				crack_state.resize(p_n * p_n * CRACK_FLOATS);
				for (float &c : crack_state) {
					c = 0.0f;
				}
			}
			_cracks_age(v, age);
			for (int k = 0; k < CRACK_FLOATS; k++) {
				crack_state[(z * p_n + x) * CRACK_FLOATS + k] = v[k];
			}
		}
	}
}

// A kept cell's drying over the time away, as _crack_update would have stepped it: soaked while the pool that stood
// on it lasts (its water goes as _ruts_age() takes the kept water: evaporation less the mean rain), then its layer
// dries at the evaporation rate until it has cracked through. Rain while away does not close it again: what the
// weather does now, on the way back, does (the live update).
void GroundSurface3D::_cracks_age(float *r_cell, double p_seconds) const {
	double t = p_seconds * double(ruts_time_scale);
	float &st = r_cell[0];
	const float pool = r_cell[2];
	if (t < 1.0 || st <= 1.0f) {
		return;
	}
	const float evap = _evaporation();
	if (st >= 2.0f) {
		const float net = evap - ruts_away_rain / 3.6e6f;
		if (net <= 0.0f) {
			return; // the pool never dries out while away
		}
		t -= double(pool / net);
		if (t <= 0.0) {
			return;
		}
		st = 1.999f;
	}
	const float thick = CLAMP(0.006f + 0.6f * pool, 0.006f, 0.05f) * 0.8f; // _crack_layer() at a typical clay share
	st = MAX(st - float(double(evap) * t / (0.4 * double(MAX(thick, 0.002f)))), 1.0f);
}

void GroundSurface3D::_cracks_save() {
	const int n = ground_n;
	if (!crack_store.is_on()) {
		return;
	}
	if (ground_valid && (int)crack_state.size() == n * n * CRACK_FLOATS) {
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				crack_store.put(ground_origin + Vector2i(x, z), &crack_state[(z * n + x) * CRACK_FLOATS], CRACK_FLOATS);
			}
		}
	}
	crack_store.flush(false);
}

void GroundSurface3D::_ruts_leave(const GroundDeformField &p_field, GroundRutStore &r_store, Vector2i p_origin, const LocalVector<Vector2i> &p_pending) {
	if (!r_store.is_on() || !p_field.valid) {
		return;
	}
	const int T = GroundDeformField::TILE;
	const int nt = p_field.tiles();
	const Vector2i old0 = p_field.origin / T;
	const Vector2i new0 = p_origin / T;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = old0 + Vector2i(tx, tz);
			const Vector2i l = w - new0;
			if ((l.x >= 0 && l.y >= 0 && l.x < nt && l.y < nt) || p_pending.has(w)) {
				continue; // staying, or never read in (its file is still right)
			}
			const bool active = int(p_field.tile_active.size()) == nt * nt && p_field.tile_active[tz * nt + tx];
			if (active || r_store.has(w)) {
				r_store.store(p_field, w, tx, tz);
			}
		}
	}
}

void GroundSurface3D::_ruts_arrive(const GroundDeformField &p_field, GroundRutStore &r_store, const GroundDeformGPUHost *p_host, Vector2i p_origin, LocalVector<Vector2i> &r_pending) {
	const int T = GroundDeformField::TILE;
	const int nt = p_field.resolution / T;
	const Vector2i old0 = p_field.origin / T;
	const Vector2i new0 = p_origin / T;
	LocalVector<Vector2i> keep;
	for (const Vector2i &w : r_pending) {
		const Vector2i l = w - new0;
		if (l.x >= 0 && l.y >= 0 && l.x < nt && l.y < nt) {
			keep.push_back(w);
		}
	}
	r_pending = keep;
	if (!r_store.is_on() || r_store.count() == 0) {
		return;
	}
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = new0 + Vector2i(tx, tz);
			const Vector2i o = w - old0;
			if (p_field.valid && o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) {
				continue;
			}
			bool cached = p_host ? p_host->has_cached(w) : p_field.has_cached(w);
			if (cached && ruts_time_scale > 0.0f && r_store.written_ago(w) * ruts_time_scale > 600.0) {
				// Away long enough to have changed: the kept copy comes back aged, not the cache's as left.
				if (p_host) {
					const_cast<GroundDeformGPUHost *>(p_host)->forget_cached(w);
				} else {
					const_cast<GroundDeformField &>(p_field).forget_cached(w);
				}
				cached = false;
			}
			if (!cached && r_store.has(w) && !r_pending.has(w)) {
				r_pending.push_back(w);
			}
		}
	}
}

void GroundSurface3D::_ruts_read_cpu(GroundDeformField &r_field, GroundRutStore &r_store, LocalVector<Vector2i> &r_pending) {
	const int T = GroundDeformField::TILE;
	const Vector2i t0 = r_field.origin / T;
	LocalVector<uint32_t> data;
	for (const Vector2i &w : r_pending) {
		double age = 0.0;
		if (r_store.load(w, data, &age)) {
			_ruts_age(data, age);
			GroundRutStore::unpack(r_field, w.x - t0.x, w.y - t0.y, data);
		}
	}
	r_pending.clear();
}

void GroundSurface3D::_ruts_read_gpu(const GroundDeformField &p_field, GroundRutStore &r_store, GroundDeformGPUHost &r_host, LocalVector<Vector2i> &r_pending) {
	// Read in once the field stands (a reset or a move this frame first): seeded into the cache, then pulled.
	if (r_pending.is_empty() || r_host.reset || r_host.gpu.is_null() || r_host.shift.size() > 0 || r_host.stores.size() > 0) {
		return;
	}
	LocalVector<uint32_t> data;
	for (const Vector2i &w : r_pending) {
		double age = 0.0;
		if (r_store.load(w, data, &age)) {
			_ruts_age(data, age);
			r_host.seed_tile(w, data.ptr(), GroundRutStore::STRIDE, max_cached_tiles);
		}
	}
	if (r_host.pull_cached(p_field, r_pending)) {
		r_pending.clear();
	}
}

// A kept tile read back after time away is stepped over it as _age() would have (mud round 4): the turned ground's
// tint and the tracked film dry, rain over the time softens soil prints (at the mean rate ruts_away_rain), and the
// water in them goes as evaporation beats that rain, and the clay in that water settles out of it (round 5: the
// floc's 0.1 mm/s over the water's depth, all of it where the water went). Exact for these exponential decays however
// long the absence.
// Regrowth follows the seasons (docs/ideas/mud.md, round 7): of p_seconds (world time, ending today), the seconds of
// growth at the reference rate ruts_regrowth_time is given for. The sward grows with the day's mean temperature above
// 5 C (growing degree days, as WeatherSeason's forcing; the reference 10 K above it, a late spring day), held back
// by drought: the day's precipitation against its Thornthwaite evapotranspiration (no growth under a tenth of it, full
// over seven tenths), so a Mediterranean summer stalls it as a winter does. The days of the climatological year
// (the weather contract's climate at the field's height); without a weather, every day at the reference rate.
double GroundSurface3D::_growing_seconds(double p_seconds) const {
	const WorldWeather *wm = _weather();
	if (!wm || p_seconds <= 0.0) {
		return p_seconds;
	}
	const WeatherSeason::Climate c = _weather_climate(wm);
	const float h = _ground_at(center.x, center.y);
	double heat = 0.0;
	double a = 0.0;
	WeatherSeason::thornthwaite_coefficients(c, h, heat, a);
	auto rate = [&](int p_day) {
		// The climate's day, not the live weather's departure from it (WeatherMap3D's season anomaly: the last
		// month's, while the time away can be months).
		const float t = WeatherSeason::day_temperature(c, h, float(p_day));
		const float pet = WeatherSeason::day_pet(c, heat, a, t, p_day);
		const float wet = pet > 0.01f ? Math::smoothstep(0.1f, 0.7f, WeatherSeason::day_precipitation(c, float(p_day)) / pet) : 1.0f;
		return double(MAX(t - WeatherSeason::FORCING_BASE, 0.0f) / 10.0f * wet);
	};
	const double days = p_seconds / 86400.0;
	const int whole = int(Math::floor(days));
	double sum = 0.0;
	if (whole >= 365) {
		double year = 0.0;
		for (int d = 1; d <= 365; d++) {
			year += rate(d);
		}
		sum += year * double(whole / 365);
	}
	const int today = wm->day_of_year();
	for (int k = 0; k < whole % 365; k++) {
		sum += rate((today - 1 - k + 365 * 2) % 365 + 1);
	}
	sum += rate((today - 1 - (whole % 365) + 365 * 2) % 365 + 1) * (days - double(whole));
	return sum * 86400.0;
}

void GroundSurface3D::_ruts_age(LocalVector<uint32_t> &r_data, double p_seconds) const {
	const double secs = MIN(p_seconds * double(ruts_time_scale), 3.0e7); // a year at most
	if (secs < 60.0) {
		return;
	}
	const float dry = float(Math::exp(-secs / double(drying_time)));
	const float keep = float(Math::exp(-double(rain_smoothing * ruts_away_rain) * secs / 3600.0));
	const float loss = float(double(MAX(_evaporation() - ruts_away_rain / 3.6e6f, 0.0f)) * secs);
	const float regrow = float(Math::exp(-_growing_seconds(secs) / double(MAX(ruts_regrowth_time, 1.0f))));
	const int S = GroundRutStore::STRIDE;
	const int count = int(r_data.size()) / S;
	for (int k = 0; k < count; k++) {
		float dn = Math::half_to_float(uint16_t(r_data[k * S] & 0xffff)) * keep;
		float rm = Math::half_to_float(uint16_t(r_data[k * S] >> 16)) * keep;
		float fr = Math::half_to_float(uint16_t(r_data[k * S + 1] & 0xffff));
		float wd = Math::half_to_float(uint16_t(r_data[k * S + 1] >> 16));
		float cl = Math::half_to_float(uint16_t(r_data[k * S + 2] & 0xffff));
		float rg = Math::half_to_float(uint16_t(r_data[k * S + 2] >> 16));
		// Settling (Stokes, as the water step): the clay falls out over the water's own depth.
		cl = wd > 1e-4f ? cl * float(Math::exp(-1.0e-4 * secs / double(wd))) : 0.0f;
		if (fr >= 4.0f) {
			const Vector2 sq = GroundDeformField::mud_band_share_wet(fr);
			fr = GroundDeformField::mud_band(sq.x, sq.y * dry);
		} else if (fr < 2.0f) {
			fr *= dry;
		}
		wd = MAX(wd - loss, 0.0f);
		cl = wd > 1e-4f && cl > 1e-4f ? cl : 0.0f;
		dn = dn < 1e-5f ? 0.0f : dn;
		rm = rm < 1e-5f ? 0.0f : rm;
		r_data[k * S] = uint32_t(Math::make_half_float(dn)) | (uint32_t(Math::make_half_float(rm)) << 16);
		r_data[k * S + 1] = uint32_t(Math::make_half_float(fr)) | (uint32_t(Math::make_half_float(wd)) << 16);
		// The sward grows back over the rut (mud round 6): what is still bare closes at ruts_regrowth_time's e-fold;
		// a rut softened flat by the rain needs none.
		rg = Math::abs(dn - rm) > 0.002f ? 1.0f - (1.0f - CLAMP(rg, 0.0f, 1.0f)) * regrow : 0.0f;
		r_data[k * S + 2] = uint32_t(Math::make_half_float(cl)) | (uint32_t(Math::make_half_float(rg)) << 16);
	}
}

void GroundSurface3D::_ruts_save_field(const GroundDeformField &p_field, GroundRutStore &r_store, const LocalVector<Vector2i> &p_pending) {
	if (!r_store.is_on() || !p_field.valid) {
		return;
	}
	const int T = GroundDeformField::TILE;
	const int nt = p_field.tiles();
	const Vector2i t0 = p_field.origin / T;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const Vector2i w = t0 + Vector2i(tx, tz);
			if (p_pending.has(w)) {
				continue;
			}
			const bool active = int(p_field.tile_active.size()) == nt * nt && p_field.tile_active[tz * nt + tx];
			if (active || r_store.has(w)) {
				r_store.store(p_field, w, tx, tz);
			}
		}
	}
}

void GroundSurface3D::set_keep_ruts(bool p_enabled) {
	keep_ruts = p_enabled;
	if (!p_enabled) {
		rut_main_pending.clear();
		rut_near_pending.clear();
	}
	_ruts_configure();
}

void GroundSurface3D::set_ruts_path(const String &p_path) {
	ruts_path = p_path;
	rut_main_pending.clear();
	rut_near_pending.clear();
	_ruts_configure();
}

void GroundSurface3D::save_ruts() {
	_ruts_configure();
	_ruts_save_field(field, rut_main, rut_main_pending);
	if (near_fld.valid) {
		_ruts_save_field(near_fld, rut_near, rut_near_pending);
	}
	_cracks_save();
	_wear_save();
	_deposits_save();
}

void GroundSurface3D::clear_saved_ruts() {
	// Every field's tiles in the folder, whatever their texel.
	if (!ruts_path.is_empty() && DirAccess::dir_exists_absolute(ruts_path)) {
		Ref<DirAccess> da = DirAccess::open(ruts_path);
		if (da.is_valid()) {
			LocalVector<String> files;
			da->list_dir_begin();
			for (String f = da->get_next(); !f.is_empty(); f = da->get_next()) {
				if (!da->current_is_dir() && (f.ends_with(".rut") || f.ends_with(".cel") || f.ends_with(".wear") || f.ends_with(".dep"))) {
					files.push_back(f);
				}
			}
			da->list_dir_end();
			for (const String &f : files) {
				da->remove(f);
			}
		}
	}
	rut_main_pending.clear();
	rut_near_pending.clear();
	rut_main.configure(String(), "m", 0.0f);
	rut_near.configure(String(), "n", 0.0f);
	crack_store.clear();
	crack_store.configure(String(), "c", 0.0f);
	_ruts_configure();
}

int GroundSurface3D::get_saved_rut_tile_count() {
	_ruts_configure();
	return rut_main.count() + rut_near.count();
}

Vector2 GroundSurface3D::get_tracked_mud_at(Vector3 p_position) const {
	const bool near = is_near_active() && _near_inside(p_position.x, p_position.z);
	const GroundDeformField &f = near ? near_fld : field;
	const int n = f.resolution;
	if (!f.valid || int(f.fresh.size()) != n * n) {
		return Vector2();
	}
	const float t = f.texel();
	const int x = (int)Math::floor(p_position.x / t) - f.origin.x;
	const int z = (int)Math::floor(p_position.z / t) - f.origin.y;
	if (x < 0 || z < 0 || x >= n || z >= n || f.fresh[z * n + x] < 4.0f) {
		return Vector2();
	}
	return GroundDeformField::mud_band_share_wet(f.fresh[z * n + x]);
}

float GroundSurface3D::track_mud(Vector3 p_position, float p_radius, float p_depth, float p_wet, Vector4 p_print) {
	if (!tracked_mud || !(p_depth > 0.0f)) {
		return 0.0f;
	}
	// A film thinner than a millimetre covers the grain in patches: its share, so the trail fades out as the tread
	// or sole runs clean.
	const float before = field.amount_fresh;
	field.amount_fresh = GroundDeformField::mud_band(CLAMP(p_depth / 0.001f, 0.1f, 1.0f), p_wet);
	field.amount_print = p_print;
	near_fld.amount_print = p_print;
	// A sole's disc is about five times the sole's own area: the relief is spread over the disc by that, the volume
	// the sole's (the share, from the film's thickness, stays the sole's).
	const float v = deposit(p_position, p_radius, p_print.x > 1.5f ? 0.2f * p_depth : p_depth);
	// The 1 m cells a film lies in, for the wind over it once dry (round 18.3).
	for (int cz = int(Math::floor(p_position.z - p_radius)); cz <= int(Math::floor(p_position.z + p_radius)); cz++) {
		for (int cx = int(Math::floor(p_position.x - p_radius)); cx <= int(Math::floor(p_position.x + p_radius)); cx++) {
			if (film_cells.size() < 65536) {
				film_cells.insert(Vector2i(cx, cz));
			}
		}
	}
	if (p_print.x < 0.5f) {
		// Thrown clay (no tyre or sole print): it spatters the plants it lands among too, in full by a film of
		// 0.3 mm.
		_plant_mud_add(p_position, p_radius, CLAMP(p_depth / 0.0003f, 0.0f, 1.0f));
		// And GroundClods3D stands the bigger landings up as lumps.
		const uint64_t frame = Engine::get_singleton()->get_physics_frames();
		if (frame > clod_frame + 2) {
			clod_drops.clear();
		}
		clod_frame = frame;
		if (clods_node && clod_drops.size() < 1024) {
			ClodDrop d;
			d.position = p_position;
			d.radius = p_radius;
			d.volume = float(Math::PI) * p_radius * p_radius * p_depth;
			d.color = get_mud_color_at(p_position);
			if (d.color.r + d.color.g + d.color.b <= 0.01f) {
				d.color = mud_color; // no painted mud there
			}
			d.wet = p_wet;
			clod_drops.push_back(d);
		}
	}
	field.amount_fresh = before;
	field.amount_print = Vector4();
	near_fld.amount_print = Vector4();
	return v;
}

void GroundSurface3D::take_clod_drops(LocalVector<ClodDrop> &r_drops) {
	r_drops = clod_drops;
	clod_drops.clear();
}

void GroundSurface3D::set_clods(bool p_enabled) {
	clods = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
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

void GroundSurface3D::_push_dust(const Dust &p_dust) {
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	if (frame > dust_frame + 2) {
		dust.clear();
		near_dust.clear();
	}
	dust_frame = frame;
	if (frame <= dust_claim_frame + 2 && p_dust.position.distance_squared_to(dust_claim_center) < dust_claim_radius * dust_claim_radius) {
		if (near_dust.size() < 256) {
			near_dust.push_back(p_dust);
		}
		return;
	}
	if (dust.size() < 256) {
		dust.push_back(p_dust);
	}
}

void GroundSurface3D::add_dust(Vector3 p_position, Vector3 p_velocity, float p_strength) {
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	if (frame > dust_frame + 2) {
		dust.clear();
		near_dust.clear();
	}
	dust_frame = frame;
	const float silt = _silt_dust_share(p_position.x, p_position.z);
	const float film = _film_dust_share(p_position);
	const float share = MAX(MAX(get_blown_at(p_position.x, p_position.z), silt), film);
	if (share <= 0.0f || p_strength <= 0.0f || dust.size() >= 256) {
		return;
	}
	Dust d;
	d.position = p_position;
	d.velocity = p_velocity;
	d.rate = dust_emission * share * p_strength;
	if (silt > 0.0f) {
		// Dry silt (round 17.3): the tyre breaks the skin's crust and lifts its fines; what it lifts is gone from it.
		const Vector2i k(int(Math::floor(p_position.x)), int(Math::floor(p_position.z)));
		silt_broken.insert(k);
		_silt_take(k, dust_emission * silt * p_strength * MAX(float(get_physics_process_delta_time()), 1e-3f));
	}
	if (film > 0.0f) {
		// Dried tracked mud (round 18.3): ground to powder by the traffic and lifted; the film goes with it.
		_film_take(p_position, dust_emission * film * p_strength * MAX(float(get_physics_process_delta_time()), 1e-3f));
	}
	_push_dust(d);
}

// Dry silt skins as dust (docs/ideas/mud.md, round 17.3). A dried skin of flood silt is the dustiest ground there is:
// fine, loose once its crust is broken, and nothing binds it (playas and dried floodplains are the main dust sources:
// Gillette et al. 1980, Reynolds et al. 2007). The share of a tyre's dust it gives, as dry bare sand's: full on a
// skin of 2 mm or more that has dried (its water under 0.1), none on one still damp (0.3) or thinner than half a
// millimetre.
float GroundSurface3D::_silt_dust_share(float p_x, float p_z) const {
	if (silt_depth.is_empty()) {
		return 0.0f;
	}
	const float silt = _silt_at(p_x, p_z);
	if (silt <= 0.0005f) {
		return 0.0f;
	}
	return Math::smoothstep(0.0005f, 0.002f, silt) * (1.0f - Math::smoothstep(0.1f, 0.3f, _silt_water(p_x, p_z)));
}

// Tracked-out mud as dust (docs/ideas/mud.md, round 18.3). The clay tyres and boots carry off a muddy site and leave
// on firm ground (track_mud(), round 3) dries to a crumb that traffic grinds to powder: track-out is the main source of
// the silt loading that paved-road dust rises from (US EPA AP-42 13.2.1; Nicholson and Branson 1990; Kuhns et al. 2001
// on emissions rising sharply from track-out next to construction exits). Dry (its water under 0.1, none past 0.3), a
// film gives a tyre's dust as dry bare sand does, and the wind lifts it from a friction speed of 0.3 m/s as a broken
// silt skin (_silt_wind_update()). A film is about a millimetre at full share, 1500 kg/m3.
float GroundSurface3D::_film_dust_share(const Vector3 &p_position) const {
	if (!tracked_mud) {
		return 0.0f;
	}
	const Vector2 sw = get_tracked_mud_at(p_position);
	if (sw.x <= 0.0f) {
		return 0.0f;
	}
	return sw.x * (1.0f - Math::smoothstep(0.1f, 0.3f, sw.y));
}

// Film lifted at a place: kept as owed in its 1 m cell, and taken off in texel-sized discs as the mass owed reaches
// one's (the field's washing-away path: the film's share and the relief it laid go together), at spots scattered
// over the 30 cm round the place, so it thins in patches and the trail fades.
void GroundSurface3D::_film_take(const Vector3 &p_position, float p_kg) {
	const Vector2 sw = get_tracked_mud_at(p_position);
	if (sw.x <= 0.0f || p_kg <= 0.0f || !field.valid) {
		return;
	}
	film_dust_kg += p_kg;
	const float per_m2 = 1500.0f * 0.001f * sw.x;
	const float t = is_near_active() && _near_inside(p_position.x, p_position.z) ? near_fld.texel() : field.texel();
	const float r = 0.75f * t;
	const float disc = per_m2 * Math::PI * r * r;
	const Vector2i k(int(Math::floor(p_position.x)), int(Math::floor(p_position.z)));
	float &owed = film_owed[k];
	owed += p_kg;
	const float before = field.amount_fresh;
	field.amount_fresh = -1.0f;
	int n = 0;
	while (owed >= disc && n < 16) {
		owed -= disc;
		const uint32_t h = hash_fmix32(hash_murmur3_one_32(film_spots++));
		const float ang = float(h & 0xffff) / 65535.0f * Math::TAU;
		const float rad = 0.15f * Math::sqrt(float(h >> 16) / 65535.0f);
		const Vector3 q(p_position.x + rad * Math::cos(ang), p_position.y, p_position.z + rad * Math::sin(ang));
		dig(q, r, 0.001f);
		n++;
	}
	field.amount_fresh = before;
}

// Wind over the dried films near the followed point: the active tiles' texels, gathered into 1 m cells.
void GroundSurface3D::_film_wind_update(float p_dt, float p_ustar) {
	film_wind.clear();
	if (!tracked_mud || !field.valid || p_ustar <= 0.3f) {
		return;
	}
	const int n = field.resolution;
	if (int(field.fresh.size()) != n * n) {
		return;
	}
	const float t = field.texel();
	const Vector3 c = _follow_position();
	const float range = 30.0f;
	HashMap<Vector2i, Vector3> cells; // each 1 m cell: m2 of dry film, and the sum of its texels' x and z by it
	LocalVector<Vector2i> clean;
	for (const Vector2i &k : film_cells) {
		if (Math::abs(k.x + 0.5f - c.x) > range || Math::abs(k.y + 0.5f - c.z) > range) {
			continue;
		}
		const int x0 = int(Math::floor(k.x / t)) - field.origin.x;
		const int z0 = int(Math::floor(k.y / t)) - field.origin.y;
		const int m = int(Math::ceil(1.0f / t));
		bool any = false;
		Vector3 dry;
		for (int z = MAX(z0, 0); z < MIN(z0 + m, n); z++) {
			for (int x = MAX(x0, 0); x < MIN(x0 + m, n); x++) {
				const float f = field.fresh[z * n + x];
				if (f < 4.0f) {
					continue;
				}
				any = true;
				const Vector2 sw = GroundDeformField::mud_band_share_wet(f);
				const float a = sw.x * (1.0f - Math::smoothstep(0.1f, 0.3f, sw.y)) * t * t;
				dry += Vector3(a, a * (field.origin.x + x + 0.5f) * t, a * (field.origin.y + z + 0.5f) * t);
			}
		}
		if (!any) {
			clean.push_back(k);
		} else if (dry.x > 0.0f) {
			cells[k] = dry;
		}
	}
	for (const Vector2i &k : clean) {
		film_cells.erase(k);
		film_owed.erase(k);
	}
	const float ut = 0.3f;
	const float q = 2.61f * 1.2f / 9.81f * p_ustar * p_ustar * p_ustar * (1.0f + ut / p_ustar) * (1.0f - (ut / p_ustar) * (ut / p_ustar));
	const float f = 1e-3f * q; // kg/m2/s of dry film
	for (const KeyValue<Vector2i, Vector3> &kv : cells) {
		SiltWind sw;
		sw.cell = kv.key;
		sw.rate = f * MIN(kv.value.x, 1.0f); // over the cell's square metre
		sw.at = Vector2(kv.value.y, kv.value.z) / kv.value.x; // where its film lies
		film_wind.push_back(sw);
	}
	film_wind.sort_custom<SiltWindStronger>();
	if (film_wind.size() > 64) {
		film_wind.resize(64);
	}
	// The films thin (sped as the drying is, runoff_time_scale): taken where the cell's film lies.
	for (const SiltWind &sw : film_wind) {
		const float kg = sw.rate * p_dt * runoff_time_scale;
		_film_take(Vector3(sw.at.x, _ground_at(sw.at.x, sw.at.y), sw.at.y), kg);
	}
}

// Silt lifted off a 1 m cell: the skin (and the deposit it is part of) thins by the mass over its dry density.
void GroundSurface3D::_silt_take(const Vector2i &p_cell, float p_kg) {
	float *s = silt_depth.getptr(p_cell);
	if (!s || p_kg <= 0.0f) {
		return;
	}
	const float dm = MIN(p_kg / 1300.0f, *s);
	*s -= dm;
	silt_dust_kg += dm * 1300.0f;
	if (float *v = mud_deposits.getptr(p_cell)) {
		*v = MAX(*v - dm, 0.0f);
	}
	if (*s <= 1e-6f) {
		silt_depth.erase(p_cell);
		silt_broken.erase(p_cell);
	}
	silt_pending += dm;
	if (silt_pending > 0.002f) {
		silt_pending = 0.0f;
		silt_dirty = true;
	}
}

// Wind on dry silt: dust rises once the wind's friction speed passes the surface's threshold. A broken skin (driven
// over) lifts from about 0.3 m/s; an unbroken crust holds to about 1 m/s and more (Gillette et al. 1980, 1982,
// thresholds of disturbed and crusted desert soils). u* = kappa U10 / ln(10 / z0), z0 1 mm over a bare flat. The
// saltation flux Q = C (rho_a / g) u*^3 (1 + u*t / u*)(1 - (u*t / u*)^2) (White 1979, C 2.61) drives the dust flux
// F = alpha Q, alpha 1e-3 per metre for silty soils (Gillette 1977; Marticorena et al. 1997's range), kg/m2/s. The
// cells near the followed point are re-read every half second and give their dust each step; the skin thins by it
// (at runoff_time_scale, as its drying).
void GroundSurface3D::_silt_wind_update(float p_dt) {
	silt_wind.clear();
	const Vector2 w10 = _wind_10m();
	const float u10 = w10.length();
	const float ustar = 0.4f * u10 / Math::log(10.0f / 0.001f);
	_film_wind_update(p_dt, ustar);
	if (silt_depth.is_empty()) {
		return;
	}
	if (ustar < 0.3f) {
		// Damp again (rain), a broken skin crusts over as it dries.
		LocalVector<Vector2i> wet;
		for (const Vector2i &k : silt_broken) {
			if (_silt_water(k.x + 0.5f, k.y + 0.5f) > 0.3f) {
				wet.push_back(k);
			}
		}
		for (const Vector2i &k : wet) {
			silt_broken.erase(k);
		}
		return;
	}
	const Vector3 c = _follow_position();
	const float range = 40.0f;
	LocalVector<Vector2i> wet;
	for (const KeyValue<Vector2i, float> &kv : silt_depth) {
		const Vector2 p(kv.key.x + 0.5f, kv.key.y + 0.5f);
		if (Math::abs(p.x - c.x) > range || Math::abs(p.y - c.z) > range) {
			continue;
		}
		if (_silt_water(p.x, p.y) > 0.3f) {
			wet.push_back(kv.key);
			continue;
		}
		const float share = _silt_dust_share(p.x, p.y);
		if (share <= 0.0f) {
			continue;
		}
		const float ut = silt_broken.has(kv.key) ? 0.3f : 1.0f;
		if (ustar <= ut) {
			continue;
		}
		const float q = 2.61f * 1.2f / 9.81f * ustar * ustar * ustar * (1.0f + ut / ustar) * (1.0f - (ut / ustar) * (ut / ustar));
		const float f = 1e-3f * q * share; // kg/m2/s over the 1 m cell
		SiltWind sw;
		sw.cell = kv.key;
		sw.rate = f;
		silt_wind.push_back(sw);
	}
	for (const Vector2i &k : wet) {
		silt_broken.erase(k);
	}
	// The skin thins (after the loop: _silt_take() may erase cells).
	for (const SiltWind &sw : silt_wind) {
		_silt_take(sw.cell, sw.rate * p_dt * runoff_time_scale);
	}
	// The strongest 64 give their dust to the air.
	silt_wind.sort_custom<SiltWindStronger>();
	if (silt_wind.size() > 64) {
		silt_wind.resize(64);
	}
}

void GroundSurface3D::_silt_wind_emit() {
	if (silt_wind.is_empty() && film_wind.is_empty()) {
		return;
	}
	const Vector2 w10 = _wind_10m();
	for (const SiltWind &sw : film_wind) {
		Dust d;
		const float x = sw.at.x;
		const float z = sw.at.y;
		d.position = Vector3(x, get_surface_height_at(Vector3(x, 0.0f, z)) + 0.05f, z);
		d.velocity = Vector3(0.5f * w10.x, 0.3f, 0.5f * w10.y);
		d.rate = sw.rate;
		_push_dust(d);
	}
	for (const SiltWind &sw : silt_wind) {
		if (!silt_depth.has(sw.cell)) {
			continue;
		}
		Dust d;
		const float x = sw.cell.x + 0.5f;
		const float z = sw.cell.y + 0.5f;
		d.position = Vector3(x, get_surface_height_at(Vector3(x, 0.0f, z)) + 0.05f, z);
		// Off the ground with the gust that lifted it: half the 10 m wind along, a little up.
		d.velocity = Vector3(0.5f * w10.x, 0.3f, 0.5f * w10.y);
		d.rate = sw.rate; // over the cell's square metre
		_push_dust(d);
	}
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
		d["water"] = s.water;
		d["water_mud"] = s.water_mud;
		d["volume"] = s.volume;
		out.push_back(d);
	}
	sprays.clear();
	return out;
}

struct CompareStoneDepth {
	_FORCE_INLINE_ bool operator()(const Vector2 &p_a, const Vector2 &p_b) const { return p_a.x < p_b.x; }
};

PackedByteArray GroundSurface3D::_gpu_cells() const {
	PackedByteArray out;
	const int n = ground_n;
	if (!ground_valid || int(cells.size()) != n * n) {
		return out;
	}
	out.resize(n * n * 64 + GroundDeformGPU::STONE_VEC4S * 16);
	memset(out.ptrw(), 0, out.size());
	float *w = reinterpret_cast<float *>(out.ptrw());
	const bool inputs = int(cell_inputs.size()) == n * n;
	bool seep = false;
	bool sliding = false;
	const bool runoff_gpu = _runoff_on_gpu();
	bool runoff_any = false;
	bool liquid_any = false;
	for (int i = 0; i < n * n; i++) {
		const Cell &c = cells[i];
		float *o = w + i * 16;
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
		// The water bodies' depth alone: the GPU's own standing water there (a puddle, a flooded rut) is runoff
		// still, and counting it as a lake deleted every puddle over 2 mm as it formed.
		o[11] = runoff_gpu ? ((int)water_body_depth.size() == n * n && water_body_depth[i] > 0.002f ? -1.0f : _runoff_seep(i)) : 0.0f;
		runoff_any = runoff_any || o[11] > 0.0f;
		// Mud slides (round 10): the steepest slope the layer holds.
		o[12] = mud_slides ? c.slide_length : 1e3f;
		if (mud_slides && c.deformable && c.slide_length < 1e2f && c.mud > 0.0f && !sliding) {
			// The ground's steepest fall to a lattice neighbour against what the full layer holds.
			const int x = i % n;
			const int z = i / n;
			float fall = 0.0f;
			if (x > 0 && x < n - 1 && z > 0 && z < n - 1) {
				fall = MAX(MAX(Math::abs(ground[i + 1] - ground[i - 1]), Math::abs(ground[i + n] - ground[i - n])) / (2.0f * ground_spacing), 0.0f);
			}
			sliding = fall > c.slide_length / c.mud;
		}
		o[13] = c.crit_shear;
		const bool liquid = mud_waves && c.deformable && c.wave_nu > 0.0f;
		o[14] = liquid ? c.wave_tau : 0.0f;
		o[15] = liquid ? c.wave_nu : 0.0f;
		liquid_any = liquid_any || liquid;
		seep = seep || (c.deformable && o[9] < c.max_dent);
	}
	const_cast<GroundSurface3D *>(this)->water_seep = seep;
	const_cast<GroundSurface3D *>(this)->mud_sliding = sliding;
	const_cast<GroundSurface3D *>(this)->runoff_seep_any = runoff_any;
	const_cast<GroundSurface3D *>(this)->mud_liquid_any = liquid_any;
	// The hidden stones (round 16.1): slumping and mud waves keep to their tops as the press does (ground_deform.glsl
	// stone_dent()). Those nearest the followed point, the shallow ones first.
	if (buried_obstacles && buried_density > 0.0f) {
		const Array near = get_buried_near(_follow_position(), MAX(deform_extent * 0.75f, 8.0f));
		LocalVector<Vector2> order; // (top depth, index)
		for (int k = 0; k < near.size(); k++) {
			const Dictionary d = near[k];
			const Vector3 a = d["a"];
			const Vector3 b = d["b"];
			const float r = d["radius"];
			const Vector3 m = (a + b) * 0.5f;
			// Nearest the followed point first (where the wheels are), a stone's depth counting as distance.
			const Vector3 f = _follow_position();
			order.push_back(Vector2(Vector2(m.x - f.x, m.z - f.z).length() + 4.0f * MAX(_ground_at(m.x, m.z) - (MAX(a.y, b.y) + r), 0.0f), float(k)));
		}
		order.sort_custom<CompareStoneDepth>();
		float *s = w + n * n * 16;
		const int count = MIN((int)order.size(), GPU_STONES);
		s[0] = float(count);
		for (int k = 0; k < count; k++) {
			const Dictionary d = near[int(order[k].y)];
			const Vector3 a = d["a"];
			const Vector3 b = d["b"];
			float *o = s + 4 + k * 8;
			o[0] = a.x;
			o[1] = a.y;
			o[2] = a.z;
			o[3] = d["radius"];
			o[4] = b.x;
			o[5] = b.y;
			o[6] = b.z;
		}
	}
	return out;
}

// Waves in liquid mud (round 14.1): substeps under the shallow water CFL limit, dt <= 0.5 l / sqrt(g h), for the
// deepest layer the field can hold (a metre and a bit).
int GroundSurface3D::_mud_wave_passes(const GroundDeformField &p_field, double p_delta) const {
	const float h = 1.2f;
	const double dt_max = 0.5 * p_field.texel() / Math::sqrt(9.81 * h);
	return CLAMP(int(Math::ceil(p_delta / dt_max)), 1, 16);
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
		const WorldWeather *wm = _weather();
		drift = sand_drift_rate * MAX(_wind_10m().length() - 5.0f, 0.0f) / 5.0f * (1.0f - get_rain_crust());
		if (wm) {
			const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
			soften = rain_smoothing * wm->precipitation_at(at);
		}
	}
	const float dry = p_age > 0.0 ? Math::exp(-float(p_age) / drying_time) : 1.0f;
	put(11, 1.0f, float(p_slow), dry, drift);
	// Wind filling prints from upwind (drift_give()): the wind's direction, the Courant number over p_age, the
	// share of the even fade kept.
	{
		const Vector2 w10 = _wind_10m();
		const float wl = w10.length();
		const bool on = wind_fills_prints && drift > 0.0f && wl > 0.0f && p_age > 0.0;
		const float cr = on ? drift * PRINT_DRIFT_LENGTH / 3600.0f * float(p_age) / p_field.texel() : 0.0f;
		put(54, on ? w10.x / wl : 0.0f, on ? w10.y / wl : 0.0f, MIN(cr, 0.5f), on ? PRINT_DRIFT_SOFTEN : 1.0f);
	}
	if (mud_waves && mud_liquid_any && p_delta > 0.0) {
		put(55, 1.0f, float(p_delta) / float(_mud_wave_passes(p_field, p_delta)), 0.0f, 0.0f);
	}
	put(12, soften, float(p_age) / 3600.0f, p_edge_margin, float(p_field.valid && &p_field == &near_fld ? _near_passes() : 1));
	// Standing water: the rain, a pipe-model substep, damping, evaporation (rates sped up by runoff_time_scale);
	// then the seepage length, the conductivity compaction keeps, on, the thinnest film kept once its source stops.
	if ((standing_water || _runoff_on_gpu()) && p_delta > 0.0) {
		const WorldWeather *wm = _weather();
		const float rain = wm ? MAX(wm->precipitation_at(Vector3(center.x, _ground_at(center.x, center.y), center.y)), 0.0f) / 3.6e6f : 0.0f;
		const int passes = _water_passes(p_field, p_delta);
		const float sub = MIN(float(p_delta) / passes, 0.5f * p_field.texel() / Math::sqrt(9.81f * 0.2f));
		// A long step moves the water only as far as the substeps reach, but its sources and sinks get the
		// whole step: their rates are scaled to land the step's water in the substeps.
		const float gain = runoff_time_scale * float(p_delta) / (sub * passes);
		// Damping: puddles settle in a second or two rather than sloshing on (the pipe model has no friction).
		// With runoff on the GPU the water runs at its own pace and its sources land at their own rate, so it
		// evaporates at its own rate too: sped up with the sand, a low-tide terrace's thin seepage dried faster
		// than it seeped and the rills' water vanished on it before it could spread and lay its fans.
		put(13, rain * gain, sub, 2.0f, _evaporation() * (_runoff_on_gpu() ? gain / MAX(runoff_time_scale, 1e-6f) : gain));
		// Compacted ruts drain slower: tyres cut the conductivity several fold (Horn et al. 1995): a third kept.
		put(14, 0.02f, 1.0f / 3.0f, gain, 2e-5f);
		if (_runoff_on_gpu()) {
			// Runoff channels: on; the morphological factor (the sand moves runoff_time_scale times faster, the
			// water at its own pace); 2 mm of bed roughness; the seepage landed in the substeps at its own rate.
			put(15, 1.0f, runoff_time_scale / _morph_passes(), 0.002f, gain / MAX(runoff_time_scale, 1e-6f));
		}
		// Tyres wading (mud stage 4c): this physics frame's over the field, up to four.
		const uint64_t frame = Engine::get_singleton()->get_physics_frames();
		LocalVector<const Wader *> live;
		for (const Wader &wd : waders) {
			if (wd.frame + 1 >= frame && r.has_point(Vector2(wd.axle.x, wd.axle.z))) {
				live.push_back(&wd);
			}
		}
		int count = 0;
		for (const Wader *wd : live) {
			if (count >= 4) {
				break;
			}
			const Vector2 v(wd->velocity.x, wd->velocity.z);
			const float speed = v.length();
			if (speed < 1e-3f) {
				continue;
			}
			put(17 + 2 * count, wd->axle.x, wd->axle.y, wd->axle.z, wd->radius);
			put(18 + 2 * count, Math::atan2(v.y, v.x), 0.0f, wd->half_width, speed);
			count++;
		}
		// Water added and taken (add_water): each disc's depth a pass, over its area in this field's texels.
		int discs = 0;
		for (const WaterDisc &wd : water_discs) {
			if (discs >= WATER_DISCS) {
				break;
			}
			const int texels = _disc_texels(p_field, wd.position, wd.radius);
			if (texels <= 0) {
				continue;
			}
			const float t = p_field.texel();
			put(25 + discs, wd.position.x, wd.position.z, wd.radius, float(wd.volume / (double(texels) * t * t) / passes));
			f[41 * 4 + discs] = wd.mud;
			discs++;
		}
		// Muddy water (round 3): clay stirred in (stir_water), each disc's mass a pass over its texels; the floc's
		// settling (0.1 mm/s) and the tread's churn (2 kg/m2: a couple of millimetres of slurry turned over as it
		// passes) at real time, not runoff_time_scale's: the substeps cover p_delta / (sub passes) of it.
		int stirs = 0;
		if (muddy_water) {
			const float real = float(p_delta) / (sub * passes);
			for (const StirDisc &sd : stir_discs) {
				if (stirs >= 8) {
					break;
				}
				const int texels = _disc_texels(p_field, sd.position, sd.radius);
				if (texels <= 0) {
					continue;
				}
				const float t = p_field.texel();
				put(45 + stirs, sd.position.x, sd.position.z, sd.radius, sd.mass / (float(texels) * t * t) / passes);
				stirs++;
			}
			put(53, 1.0f, 1e-4f * real, 2.0f * real, 300.0f);
		}
		// .w: the whole morphological factor, for the runoff's suspended soil (once a water pass, not a morph pass).
		put(16, float(count), float(discs), float(stirs), _runoff_on_gpu() ? runoff_time_scale : 0.0f);
	} else if (_runoff_on_gpu()) {
		// No water step (a still frame): the bed's relief is still drawn (the blit reads it), nothing moves.
		put(15, 1.0f, 0.0f, 0.002f, 0.0f);
	}
	return b;
}

void GroundSurface3D::_gpu_submit(double p_delta, double p_age, bool p_readback, bool p_sync) {
	_ruts_read_gpu(field, rut_main, gpu_host, rut_main_pending);
	if (is_near_active()) {
		_ruts_read_gpu(near_fld, rut_near, near_host, rut_near_pending);
	}
	_gpu_submit_field(gpu_host, field, gpu_ground_dirty, gpu_cells_dirty, gpu_slow_accum, 1, terrain_margin + skirt_fade, p_delta, p_age, p_readback, p_sync);
	if (is_near_active()) {
		// The near field lies inside the main one's deformable middle: no edge margin of its own.
		_gpu_submit_field(near_host, near_fld, near_ground_dirty, near_cells_dirty, near_slow_accum, _near_passes(), 0.0f, p_delta, p_age, p_readback, p_sync);
	}
	if ((standing_water || _runoff_on_gpu()) && p_delta > 0.0 && !water_discs.is_empty()) {
		// The discs this step applied (the first WATER_DISCS) are done; the rest wait for the next.
		const int done = MIN((int)water_discs.size(), WATER_DISCS);
		for (int k = 0; k < done; k++) {
			water_added += water_discs[k].volume;
		}
		LocalVector<WaterDisc> rest;
		for (int k = done; k < (int)water_discs.size(); k++) {
			rest.push_back(water_discs[k]);
		}
		water_discs = rest;
	}
	if ((standing_water || _runoff_on_gpu()) && p_delta > 0.0) {
		stir_discs.clear();
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
		// The cells refresh every 2 s; most refreshes change nothing (dry still weather), and one that changes
		// nothing would only re-upload them and keep a slide's minute of slumping going for ever.
		const uint32_t hash = cell_bytes.size() > 0 ? hash_murmur3_buffer(cell_bytes.ptr(), cell_bytes.size()) : 0;
		if (cell_bytes.size() > 0 && hash == r_host.cells_hash && !r_host.reset) {
			r_cells_dirty = false;
			cell_bytes.clear();
		} else if (cell_bytes.size() > 0) {
			r_host.cells_hash = hash;
			flags |= GroundDeformGPU::STEP_COVER;
			r_cells_dirty = false;
			if (mud_sliding) {
				// A soaked layer on a slope it no longer holds (round 10): the field slumps for a minute after the
				// cells change (rain renews it as it soaks them), so the slide runs its course and stops.
				r_host.slump_until = MAX(r_host.slump_until, r_host.time + 60.0);
			}
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
		if (mud_waves && mud_liquid_any) {
			// What a press shoves aside in liquid mud runs on as a wave for a few seconds (round 14.1).
			r_host.slump_until = MAX(r_host.slump_until, r_host.time + 4.0);
		}
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
		const WorldWeather *wm = _weather();
		const bool rain = wm && wm->precipitation_at(Vector3(center.x, _ground_at(center.x, center.y), center.y)) > 0.0f;
		const bool runoff = _runoff_on_gpu() && (rain || runoff_seep_any || water_seen > 0.0f);
		if (runoff || (standing_water && (rain || water_seen > 0.0f || water_seep || !water_discs.is_empty()))) {
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
		deform_serial_dirty = true;
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
	// A slide carries mud metres down a slope, further than slumping spreads in a pass: four passes a step while one
	// runs (round 10).
	d.slump_passes = p_passes * (mud_sliding && r_host.time <= r_host.slump_until ? 4 : 1);
	d.cover_vec4s = 4;
	d.water_passes = water_passes;
	d.morph_passes = _morph_passes();
	if (mud_waves && mud_liquid_any && (flags & GroundDeformGPU::STEP_SLUMP) && p_delta > 0.0) {
		flags |= GroundDeformGPU::STEP_MUDWAVE;
		d.mud_passes = _mud_wave_passes(p_field, p_delta);
	}
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
	_ice_break(p_position, p_radius, 0.03f);
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	// On the GPU path the CPU copy takes the press too, so the volume returned and queries made before the
	// next read back see it; the GPU field takes it as a stamp.
	const float moved = field.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance);
	if (!_gpu_on()) {
		_avalanche_trigger(p_position, p_radius, p_bottom);
	}
	if (is_near_active()) {
		near_fld.press_settle = field.press_settle;
		near_fld.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance);
	}
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance, Vector2());
	}
	return moved;
}

float GroundSurface3D::press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink, bool p_sole) {
	_ice_break(p_position, 0.5f * MAX(p_size.x, p_size.y), 0.03f);
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const float moved = field.press(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, p_sole, contact_tolerance);
	if (!_gpu_on()) {
		_avalanche_trigger(p_position, 0.5f * p_size.length(), p_bottom);
	}
	if (is_near_active()) {
		near_fld.press_settle = field.press_settle;
		near_fld.press(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, p_sole, contact_tolerance);
	}
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, p_sole, contact_tolerance, Vector2());
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
	film_cells.clear();
	film_owed.clear();
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
	rut_main_pending.clear(); // what is on disk stays: the next pass over it reads it in
	rut_near_pending.clear();
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

// Outside its lattice the patch knows nothing of the ground (_ground_at() clamps to the lattice's edge, so a query far
// off read the edge's height): it asks the height source there, or the colliders under the point (GroundHeightQuery),
// and answers NaN where nothing does, so the WorldGround contract falls back to physics.
bool GroundSurface3D::_on_lattice(float p_x, float p_z) const {
	if (!ground_valid || ground_n < 2) {
		return false;
	}
	const float fx = p_x / ground_spacing - ground_origin.x;
	const float fz = p_z / ground_spacing - ground_origin.y;
	return fx >= 0.0f && fz >= 0.0f && fx <= float(ground_n - 1) && fz <= float(ground_n - 1);
}

float GroundSurface3D::_off_lattice_height(const Vector3 &p_position) const {
	if (!is_inside_tree()) {
		return Math::NaN;
	}
	return GroundHeightQuery::make(this, height_source, ground_collision_mask).height(p_position);
}

float GroundSurface3D::get_ground_height_at(Vector3 p_position) const {
	if (!_on_lattice(p_position.x, p_position.z)) {
		const float h = _off_lattice_height(p_position);
		if (Math::is_finite(h) || !ground_valid) {
			return h;
		}
	}
	return _ground_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_surface_height_at(Vector3 p_position) const {
	if (!_on_lattice(p_position.x, p_position.z)) {
		// No prints off the lattice: the field lies inside it.
		const float h = _off_lattice_height(p_position);
		if (Math::is_finite(h) || !ground_valid) {
			return h;
		}
	}
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

// The near patch draws a copy of the material (with the ground_deform_patch switch on), so a parameter set on the
// material afterwards (TerrainMaterial3D.scree_blocks_distance from RockField3D, weather) has to reach the copy:
// the source's changed signal marks it, and once a physics frame the copy takes the source's shader parameters
// (a new copy only when the source's shader itself was replaced).
void GroundSurface3D::_sync_patch_params() {
	patch_params_stale = false;
	ShaderMaterial *src = Object::cast_to<ShaderMaterial>(material.ptr());
	if (!src) {
		_update_patch_material(); // a BaseMaterial3D: its copy is cheap
		return;
	}
	ShaderMaterial *dst = Object::cast_to<ShaderMaterial>(patch_material.ptr());
	if (!dst || src->get_shader() != patch_source_shader || src->get_shader().is_null()) {
		_update_patch_material();
		return;
	}
	List<PropertyInfo> params;
	src->get_shader()->get_shader_uniform_list(&params);
	static const StringName patch_flag = "ground_deform_patch";
	for (const PropertyInfo &pi : params) {
		if (pi.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		const StringName name = pi.name;
		if (name == patch_flag) {
			continue;
		}
		dst->set_shader_parameter(name, src->get_shader_parameter(name));
	}
}

void GroundSurface3D::set_material(const Ref<Material> &p_material) {
	if (material.is_valid() && material->is_connected(CoreStringName(changed), callable_mp(this, &GroundSurface3D::_on_material_changed))) {
		material->disconnect(CoreStringName(changed), callable_mp(this, &GroundSurface3D::_on_material_changed));
	}
	material = p_material;
	if (material.is_valid()) {
		material->connect(CoreStringName(changed), callable_mp(this, &GroundSurface3D::_on_material_changed));
	}
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
	ClassDB::bind_method(D_METHOD("set_buried_obstacles", "enabled"), &GroundSurface3D::set_buried_obstacles);
	ClassDB::bind_method(D_METHOD("is_buried_obstacles"), &GroundSurface3D::is_buried_obstacles);
	ClassDB::bind_method(D_METHOD("set_buried_density", "density"), &GroundSurface3D::set_buried_density);
	ClassDB::bind_method(D_METHOD("get_buried_density"), &GroundSurface3D::get_buried_density);
	ClassDB::bind_method(D_METHOD("get_buried_depth_at", "position", "radius"), &GroundSurface3D::get_buried_depth_at, DEFVAL(0.1f));
	ClassDB::bind_method(D_METHOD("get_buried_near", "position", "radius"), &GroundSurface3D::get_buried_near);
	ClassDB::bind_method(D_METHOD("set_buried_kick", "enabled"), &GroundSurface3D::set_buried_kick);
	ClassDB::bind_method(D_METHOD("is_buried_kick"), &GroundSurface3D::is_buried_kick);
	ClassDB::bind_method(D_METHOD("get_loose_stone_count"), &GroundSurface3D::get_loose_stone_count);
	ClassDB::bind_method(D_METHOD("get_reburied_stone_count"), &GroundSurface3D::get_reburied_stone_count);
	ClassDB::bind_method(D_METHOD("get_silt_dust_lifted"), &GroundSurface3D::get_silt_dust_lifted);
	ClassDB::bind_method(D_METHOD("get_tracked_dust_lifted"), &GroundSurface3D::get_tracked_dust_lifted);
	ClassDB::bind_method(D_METHOD("add_dust", "position", "velocity", "strength"), &GroundSurface3D::add_dust);
	ClassDB::bind_method(D_METHOD("get_squeezed_stone_count"), &GroundSurface3D::get_squeezed_stone_count);
	ClassDB::bind_method(D_METHOD("add_loose_stone", "position", "radius", "velocity"), &GroundSurface3D::add_loose_stone, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("set_worn_tracks", "enabled"), &GroundSurface3D::set_worn_tracks);
	ClassDB::bind_method(D_METHOD("is_worn_tracks"), &GroundSurface3D::is_worn_tracks);
	ClassDB::bind_method(D_METHOD("set_wear_regrowth_days", "days"), &GroundSurface3D::set_wear_regrowth_days);
	ClassDB::bind_method(D_METHOD("get_wear_regrowth_days"), &GroundSurface3D::get_wear_regrowth_days);
	ClassDB::bind_method(D_METHOD("get_wear_at", "position"), &GroundSurface3D::get_wear_at);
	ClassDB::bind_method(D_METHOD("add_wear", "position", "size", "yaw", "passes"), &GroundSurface3D::add_wear);
	ClassDB::bind_method(D_METHOD("clear_wear"), &GroundSurface3D::clear_wear);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("wear_compaction", "passes"), &GroundSurface3D::wear_compaction);
	ClassDB::bind_method(D_METHOD("set_mud_waves", "enabled"), &GroundSurface3D::set_mud_waves);
	ClassDB::bind_method(D_METHOD("is_mud_waves"), &GroundSurface3D::is_mud_waves);
	ClassDB::bind_method(D_METHOD("set_mud_slides", "enabled"), &GroundSurface3D::set_mud_slides);
	ClassDB::bind_method(D_METHOD("is_mud_slides"), &GroundSurface3D::is_mud_slides);
	ClassDB::bind_method(D_METHOD("set_simulation_paused", "paused"), &GroundSurface3D::set_simulation_paused);
	ClassDB::bind_method(D_METHOD("is_simulation_paused"), &GroundSurface3D::is_simulation_paused);
	ClassDB::bind_method(D_METHOD("set_light_budget", "light"), &GroundSurface3D::set_light_budget);
	ClassDB::bind_method(D_METHOD("is_light_budget"), &GroundSurface3D::is_light_budget);
	ClassDB::bind_method(D_METHOD("set_subsoil", "enabled"), &GroundSurface3D::set_subsoil);
	ClassDB::bind_method(D_METHOD("is_subsoil"), &GroundSurface3D::is_subsoil);
	ClassDB::bind_method(D_METHOD("get_subsoil_depth_at", "position"), &GroundSurface3D::get_subsoil_depth_at);
	ClassDB::bind_method(D_METHOD("add_mud_deposit", "position", "radius", "depth", "update", "color"), &GroundSurface3D::add_mud_deposit, DEFVAL(true), DEFVAL(Color(0, 0, 0, 0)));
	ClassDB::bind_method(D_METHOD("get_mud_deposit_at", "position"), &GroundSurface3D::get_mud_deposit_at);
	ClassDB::bind_method(D_METHOD("set_stream_fords", "enabled"), &GroundSurface3D::set_stream_fords);
	ClassDB::bind_method(D_METHOD("is_stream_fords"), &GroundSurface3D::is_stream_fords);
	ClassDB::bind_method(D_METHOD("get_ford_stats"), &GroundSurface3D::get_ford_stats);
	ClassDB::bind_method(D_METHOD("set_flood_silt", "enabled"), &GroundSurface3D::set_flood_silt);
	ClassDB::bind_method(D_METHOD("get_silt_at", "position"), &GroundSurface3D::get_silt_at);
	ClassDB::bind_method(D_METHOD("clear_mud_deposits"), &GroundSurface3D::clear_mud_deposits);
	ClassDB::bind_method(D_METHOD("is_flood_silt"), &GroundSurface3D::is_flood_silt);
	ClassDB::bind_method(D_METHOD("get_flood_silt_total"), &GroundSurface3D::get_flood_silt_total);
	ClassDB::bind_method(D_METHOD("set_mats", "enabled"), &GroundSurface3D::set_mats);
	ClassDB::bind_method(D_METHOD("is_mats"), &GroundSurface3D::is_mats);
	ClassDB::bind_method(D_METHOD("get_mat_at", "contact", "tolerance"), &GroundSurface3D::get_mat_at, DEFVAL(0.06));
	ClassDB::bind_method(D_METHOD("add_mat_load", "mat", "load"), &GroundSurface3D::add_mat_load);
	ClassDB::bind_method(D_METHOD("get_mat_count"), &GroundSurface3D::get_mat_count);
	ClassDB::bind_method(D_METHOD("get_mat_sinkage", "mat"), &GroundSurface3D::get_mat_sinkage);
	ClassDB::bind_method(D_METHOD("get_mat_spread", "mat"), &GroundSurface3D::get_mat_spread);
	ClassDB::bind_method(D_METHOD("is_mat_broken", "mat"), &GroundSurface3D::is_mat_broken);
	ClassDB::bind_method(D_METHOD("is_mat_buried", "mat"), &GroundSurface3D::is_mat_buried);
	ClassDB::bind_method(D_METHOD("refresh_deposits"), &GroundSurface3D::refresh_deposits);
	ClassDB::bind_method(D_METHOD("get_mat_friction", "mat"), &GroundSurface3D::get_mat_friction);
	ClassDB::bind_method(D_METHOD("get_mud_slide_angle_at", "position"), &GroundSurface3D::get_mud_slide_angle_at);
	ClassDB::bind_method(D_METHOD("set_mud_map", "map"), &GroundSurface3D::set_mud_map);
	ClassDB::bind_method(D_METHOD("set_standing_water", "enabled"), &GroundSurface3D::set_standing_water);
	ClassDB::bind_method(D_METHOD("is_standing_water"), &GroundSurface3D::is_standing_water);
	ClassDB::bind_method(D_METHOD("get_standing_water_at", "position"), &GroundSurface3D::get_standing_water_at);
	ClassDB::bind_method(D_METHOD("set_desiccation_cracks", "enabled"), &GroundSurface3D::set_desiccation_cracks);
	ClassDB::bind_method(D_METHOD("set_puddle_ice", "enabled"), &GroundSurface3D::set_puddle_ice);
	ClassDB::bind_method(D_METHOD("is_puddle_ice"), &GroundSurface3D::is_puddle_ice);
	ClassDB::bind_method(D_METHOD("get_ice_at", "position"), &GroundSurface3D::get_ice_at);
	ClassDB::bind_method(D_METHOD("get_rut_guidance", "contact", "lateral", "half_width"), &GroundSurface3D::get_rut_guidance);
	ClassDB::bind_method(D_METHOD("set_needle_ice", "enabled"), &GroundSurface3D::set_needle_ice);
	ClassDB::bind_method(D_METHOD("is_needle_ice"), &GroundSurface3D::is_needle_ice);
	ClassDB::bind_method(D_METHOD("get_needle_ice_at", "position"), &GroundSurface3D::get_needle_ice_at);
	ClassDB::bind_method(D_METHOD("set_frozen_ground", "enabled"), &GroundSurface3D::set_frozen_ground);
	ClassDB::bind_method(D_METHOD("is_frozen_ground"), &GroundSurface3D::is_frozen_ground);
	ClassDB::bind_method(D_METHOD("get_frost_at", "position"), &GroundSurface3D::get_frost_at);
	ClassDB::bind_method(D_METHOD("set_plant_mud", "enabled"), &GroundSurface3D::set_plant_mud);
	ClassDB::bind_method(D_METHOD("is_plant_mud"), &GroundSurface3D::is_plant_mud);
	ClassDB::bind_method(D_METHOD("get_plant_mud_at", "position"), &GroundSurface3D::get_plant_mud_at);
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
	ClassDB::bind_method(D_METHOD("set_mud_color", "color"), &GroundSurface3D::set_mud_color);
	ClassDB::bind_method(D_METHOD("get_mud_color"), &GroundSurface3D::get_mud_color);
	ClassDB::bind_method(D_METHOD("set_mud_color_map", "map"), &GroundSurface3D::set_mud_color_map);
	ClassDB::bind_method(D_METHOD("get_mud_color_map"), &GroundSurface3D::get_mud_color_map);
	ClassDB::bind_method(D_METHOD("get_mud_color_at", "position"), &GroundSurface3D::get_mud_color_at);
	ClassDB::bind_method(D_METHOD("add_wader", "id", "axle", "radius", "half_width", "velocity"), &GroundSurface3D::add_wader);
	ClassDB::bind_method(D_METHOD("add_water", "position", "radius", "volume", "mud"), &GroundSurface3D::add_water, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("add_puddle", "position", "radius", "depth", "mud"), &GroundSurface3D::add_puddle, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("set_world_puddles", "enabled"), &GroundSurface3D::set_world_puddles);
	ClassDB::bind_method(D_METHOD("is_world_puddles"), &GroundSurface3D::is_world_puddles);
	ClassDB::bind_method(D_METHOD("set_world_puddle_density", "density"), &GroundSurface3D::set_world_puddle_density);
	ClassDB::bind_method(D_METHOD("get_world_puddle_density"), &GroundSurface3D::get_world_puddle_density);
	ClassDB::bind_method(D_METHOD("stir_water", "position", "radius", "mass"), &GroundSurface3D::stir_water);
	ClassDB::bind_method(D_METHOD("get_water_mud_at", "position"), &GroundSurface3D::get_water_mud_at);
	ClassDB::bind_method(D_METHOD("get_regrown_at", "position"), &GroundSurface3D::get_regrown_at);
	ClassDB::bind_method(D_METHOD("track_mud", "position", "radius", "depth", "wet", "print"), &GroundSurface3D::track_mud, DEFVAL(1.0f), DEFVAL(Vector4()));
	ClassDB::bind_method(D_METHOD("set_clods", "enabled"), &GroundSurface3D::set_clods);
	ClassDB::bind_method(D_METHOD("is_clods"), &GroundSurface3D::is_clods);
	ClassDB::bind_method(D_METHOD("get_clods_node"), &GroundSurface3D::get_clods_node);
	ClassDB::bind_method(D_METHOD("get_tracked_mud_at", "position"), &GroundSurface3D::get_tracked_mud_at);
	ClassDB::bind_method(D_METHOD("set_tracked_mud", "enabled"), &GroundSurface3D::set_tracked_mud);
	ClassDB::bind_method(D_METHOD("is_tracked_mud"), &GroundSurface3D::is_tracked_mud);
	ClassDB::bind_method(D_METHOD("set_stream_stones", "enabled"), &GroundSurface3D::set_stream_stones);
	ClassDB::bind_method(D_METHOD("is_stream_stones"), &GroundSurface3D::is_stream_stones);
	ClassDB::bind_method(D_METHOD("get_stone_scour_volume"), &GroundSurface3D::get_stone_scour_volume);
	ClassDB::bind_method(D_METHOD("get_stone_silt_lifted"), &GroundSurface3D::get_stone_silt_lifted);
	ClassDB::bind_method(D_METHOD("set_trail_pugging", "enabled"), &GroundSurface3D::set_trail_pugging);
	ClassDB::bind_method(D_METHOD("is_trail_pugging"), &GroundSurface3D::is_trail_pugging);
	ClassDB::bind_method(D_METHOD("get_trail_pock_count"), &GroundSurface3D::get_trail_pock_count);
	ClassDB::bind_method(D_METHOD("set_keep_ruts", "enabled"), &GroundSurface3D::set_keep_ruts);
	ClassDB::bind_method(D_METHOD("is_keeping_ruts"), &GroundSurface3D::is_keeping_ruts);
	ClassDB::bind_method(D_METHOD("set_ruts_path", "path"), &GroundSurface3D::set_ruts_path);
	ClassDB::bind_method(D_METHOD("get_ruts_path"), &GroundSurface3D::get_ruts_path);
	ClassDB::bind_method(D_METHOD("set_ruts_time_scale", "scale"), &GroundSurface3D::set_ruts_time_scale);
	ClassDB::bind_method(D_METHOD("get_ruts_time_scale"), &GroundSurface3D::get_ruts_time_scale);
	ClassDB::bind_method(D_METHOD("set_ruts_regrowth_time", "seconds"), &GroundSurface3D::set_ruts_regrowth_time);
	ClassDB::bind_method(D_METHOD("get_ruts_regrowth_time"), &GroundSurface3D::get_ruts_regrowth_time);
	ClassDB::bind_method(D_METHOD("set_ruts_away_rain", "rate"), &GroundSurface3D::set_ruts_away_rain);
	ClassDB::bind_method(D_METHOD("get_ruts_away_rain"), &GroundSurface3D::get_ruts_away_rain);
	ClassDB::bind_method(D_METHOD("save_ruts"), &GroundSurface3D::save_ruts);
	ClassDB::bind_method(D_METHOD("clear_saved_ruts"), &GroundSurface3D::clear_saved_ruts);
	ClassDB::bind_method(D_METHOD("get_saved_rut_tile_count"), &GroundSurface3D::get_saved_rut_tile_count);
	ClassDB::bind_method(D_METHOD("set_muddy_water", "enabled"), &GroundSurface3D::set_muddy_water);
	ClassDB::bind_method(D_METHOD("is_muddy_water"), &GroundSurface3D::is_muddy_water);
	ClassDB::bind_method(D_METHOD("get_water_added"), &GroundSurface3D::get_water_added);
	ClassDB::bind_method(D_METHOD("get_water_owed"), &GroundSurface3D::get_water_owed);
	ClassDB::bind_method(D_METHOD("get_standing_water_volume", "position", "radius"), &GroundSurface3D::get_standing_water_volume);
	ClassDB::bind_method(D_METHOD("get_water_moved"), &GroundSurface3D::get_water_moved);
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
	ClassDB::bind_method(D_METHOD("set_far_ripples", "enabled"), &GroundSurface3D::set_far_ripples);
	ClassDB::bind_method(D_METHOD("set_ripple_sorting", "enabled"), &GroundSurface3D::set_ripple_sorting);
	ClassDB::bind_method(D_METHOD("set_dune_tone", "enabled"), &GroundSurface3D::set_dune_tone);
	ClassDB::bind_method(D_METHOD("set_granule_ripples", "enabled"), &GroundSurface3D::set_granule_ripples);
	ClassDB::bind_method(D_METHOD("is_granule_ripples"), &GroundSurface3D::is_granule_ripples);
	ClassDB::bind_method(D_METHOD("set_rain_pits", "enabled"), &GroundSurface3D::set_rain_pits);
	ClassDB::bind_method(D_METHOD("is_rain_pits"), &GroundSurface3D::is_rain_pits);
	ClassDB::bind_method(D_METHOD("get_rain_pit_cover"), &GroundSurface3D::get_rain_pit_cover);
	ClassDB::bind_method(D_METHOD("get_rain_crust"), &GroundSurface3D::get_rain_crust);
	ClassDB::bind_method(D_METHOD("is_dune_tone"), &GroundSurface3D::is_dune_tone);
	ClassDB::bind_method(D_METHOD("is_ripple_sorting"), &GroundSurface3D::is_ripple_sorting);
	ClassDB::bind_method(D_METHOD("is_far_ripples"), &GroundSurface3D::is_far_ripples);
	ClassDB::bind_method(D_METHOD("set_water_sources", "sources"), &GroundSurface3D::set_water_sources);
	ClassDB::bind_method(D_METHOD("get_water_sources"), &GroundSurface3D::get_water_sources);
	ClassDB::bind_method(D_METHOD("set_water_coupling", "enabled"), &GroundSurface3D::set_water_coupling);
	ClassDB::bind_method(D_METHOD("set_water_fringe", "enabled"), &GroundSurface3D::set_water_fringe);
	ClassDB::bind_method(D_METHOD("set_salt_crusts", "enabled"), &GroundSurface3D::set_salt_crusts);
	ClassDB::bind_method(D_METHOD("is_salt_crusts"), &GroundSurface3D::is_salt_crusts);
	ClassDB::bind_method(D_METHOD("get_salt_crust_at", "position"), &GroundSurface3D::get_salt_crust_at);
	ClassDB::bind_method(D_METHOD("set_coarse_armour", "enabled"), &GroundSurface3D::set_coarse_armour);
	ClassDB::bind_method(D_METHOD("is_coarse_armour"), &GroundSurface3D::is_coarse_armour);
	ClassDB::bind_method(D_METHOD("is_water_fringe"), &GroundSurface3D::is_water_fringe);
	ClassDB::bind_method(D_METHOD("is_water_coupling"), &GroundSurface3D::is_water_coupling);
	ClassDB::bind_method(D_METHOD("get_saturation_at", "position"), &GroundSurface3D::get_saturation_at);
	ClassDB::bind_method(D_METHOD("get_water_speed_at", "position"), &GroundSurface3D::get_water_speed_at);
	ClassDB::bind_method(D_METHOD("get_water_step_usec"), &GroundSurface3D::get_water_step_usec);
	ClassDB::bind_method(D_METHOD("set_sand_glints", "enabled"), &GroundSurface3D::set_sand_glints);
	ClassDB::bind_method(D_METHOD("is_sand_glints"), &GroundSurface3D::is_sand_glints);
	ClassDB::bind_method(D_METHOD("set_wind_shelters", "shelters"), &GroundSurface3D::set_wind_shelters);
	ClassDB::bind_method(D_METHOD("set_obstacle_lee", "enabled"), &GroundSurface3D::set_obstacle_lee);
	ClassDB::bind_method(D_METHOD("is_obstacle_lee_enabled"), &GroundSurface3D::is_obstacle_lee_enabled);
	ClassDB::bind_method(D_METHOD("get_wind_shelters"), &GroundSurface3D::get_wind_shelters);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("shelter_ratio_for", "lambda"), &GroundSurface3D::shelter_ratio_for);
	ClassDB::bind_method(D_METHOD("get_blown_at", "x", "z"), &GroundSurface3D::get_blown_at);
	ClassDB::bind_method(D_METHOD("get_sand_flux_at", "position"), &GroundSurface3D::get_sand_flux_at);
	ClassDB::bind_method(D_METHOD("get_deposition_rate_at", "position"), &GroundSurface3D::get_deposition_rate_at);
	ClassDB::bind_method(D_METHOD("get_sand_deposit_at", "position"), &GroundSurface3D::get_sand_deposit_at);
	ClassDB::bind_method(D_METHOD("get_wind_step_usec"), &GroundSurface3D::get_wind_step_usec);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("bed_load", "water_speed"), &GroundSurface3D::bed_load);
	ClassDB::bind_static_method("GroundSurface3D", D_METHOD("bed_load_shear", "shear_stress"), &GroundSurface3D::bed_load_shear);
	ClassDB::bind_method(D_METHOD("set_swash_marks", "enabled"), &GroundSurface3D::set_swash_marks);
	ClassDB::bind_method(D_METHOD("is_swash_marks"), &GroundSurface3D::is_swash_marks);
	ClassDB::bind_method(D_METHOD("get_swash_mark_count"), &GroundSurface3D::get_swash_mark_count);
	ClassDB::bind_method(D_METHOD("get_swash_step_usec"), &GroundSurface3D::get_swash_step_usec);
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
	ClassDB::bind_method(D_METHOD("set_wind_fills_prints", "enabled"), &GroundSurface3D::set_wind_fills_prints);
	ClassDB::bind_method(D_METHOD("set_lee_calm_ripples", "enabled"), &GroundSurface3D::set_lee_calm_ripples);
	ClassDB::bind_method(D_METHOD("is_lee_calm_ripples"), &GroundSurface3D::is_lee_calm_ripples);
	ClassDB::bind_method(D_METHOD("get_ripple_show_at", "position"), &GroundSurface3D::get_ripple_show_at);
	ClassDB::bind_method(D_METHOD("is_wind_fills_prints"), &GroundSurface3D::is_wind_fills_prints);
	ClassDB::bind_method(D_METHOD("get_sand_drift_rate"), &GroundSurface3D::get_sand_drift_rate);
	ClassDB::bind_method(D_METHOD("set_rain_smoothing", "rate"), &GroundSurface3D::set_rain_smoothing);
	ClassDB::bind_method(D_METHOD("get_rain_smoothing"), &GroundSurface3D::get_rain_smoothing);

	ClassDB::bind_method(D_METHOD("get_wheel_soil", "position", "load", "radius", "width", "inflation", "slip", "slip_angle"), &GroundSurface3D::get_wheel_soil, DEFVAL(200.0f), DEFVAL(0.0f), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("press_wheel", "position", "size", "yaw", "sinkage", "velocity", "tread"), &GroundSurface3D::press_wheel, DEFVAL(Vector2()), DEFVAL(-1));
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
	ClassDB::bind_method(D_METHOD("press_box", "position", "size", "yaw", "bottom", "sink", "sole"), &GroundSurface3D::press_box, DEFVAL(0.0f), DEFVAL(false));
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flood_silt"), "set_flood_silt", "is_flood_silt");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stream_fords"), "set_stream_fords", "is_stream_fords");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mud_slides"), "set_mud_slides", "is_mud_slides");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mud_waves"), "set_mud_waves", "is_mud_waves");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "worn_tracks"), "set_worn_tracks", "is_worn_tracks");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "buried_obstacles"), "set_buried_obstacles", "is_buried_obstacles");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "buried_density", PROPERTY_HINT_RANGE, "0,2,0.001,or_greater"), "set_buried_density", "get_buried_density");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "buried_kick"), "set_buried_kick", "is_buried_kick");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wear_regrowth_days", PROPERTY_HINT_RANGE, "0,365,0.1,or_greater"), "set_wear_regrowth_days", "get_wear_regrowth_days");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "subsoil"), "set_subsoil", "is_subsoil");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulation_paused"), "set_simulation_paused", "is_simulation_paused");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "light_budget"), "set_light_budget", "is_light_budget");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mats"), "set_mats", "is_mats");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mud_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_mud_map", "get_mud_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "mud_map_rect"), "set_mud_map_rect", "get_mud_map_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_max_depth", PROPERTY_HINT_RANGE, "0,3,0.05,suffix:m"), "set_mud_max_depth", "get_mud_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_auto_depth", PROPERTY_HINT_RANGE, "0,2,0.05,suffix:m"), "set_mud_auto_depth", "get_mud_auto_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_wet_start", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_mud_wet_start", "get_mud_wet_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mud_wet_full", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_mud_wet_full", "get_mud_wet_full");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mud_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mud_color", "get_mud_color");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mud_color_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_mud_color_map", "get_mud_color_map");
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_fills_prints"), "set_wind_fills_prints", "is_wind_fills_prints");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lee_calm_ripples"), "set_lee_calm_ripples", "is_lee_calm_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_smoothing", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_rain_smoothing", "get_rain_smoothing");
	ADD_GROUP("Wind", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "wind_override", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind_override", "get_wind_override");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_ripples"), "set_wind_ripples", "is_wind_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_ripples"), "set_far_ripples", "is_far_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ripple_sorting"), "set_ripple_sorting", "is_ripple_sorting");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dune_tone"), "set_dune_tone", "is_dune_tone");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "granule_ripples"), "set_granule_ripples", "is_granule_ripples");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_pits"), "set_rain_pits", "is_rain_pits");
	ClassDB::bind_method(D_METHOD("set_slip_avalanches", "enabled"), &GroundSurface3D::set_slip_avalanches);
	ClassDB::bind_method(D_METHOD("is_slip_avalanches"), &GroundSurface3D::is_slip_avalanches);
	ClassDB::bind_method(D_METHOD("get_avalanche_count"), &GroundSurface3D::get_avalanche_count);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slip_avalanches"), "set_slip_avalanches", "is_slip_avalanches");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_extent", PROPERTY_HINT_RANGE, "1,16,0.25,suffix:m"), "set_ripple_extent", "get_ripple_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_time_scale", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater"), "set_ripple_time_scale", "get_ripple_time_scale");
	ADD_GROUP("Water", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "water_coupling"), "set_water_coupling", "is_water_coupling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "water_fringe"), "set_water_fringe", "is_water_fringe");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "salt_crusts"), "set_salt_crusts", "is_salt_crusts");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "coarse_armour"), "set_coarse_armour", "is_coarse_armour");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "water_sources", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_water_sources", "get_water_sources");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sand_glints"), "set_sand_glints", "is_sand_glints");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "runoff_channels"), "set_runoff_channels", "is_runoff_channels");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "swash_marks"), "set_swash_marks", "is_swash_marks");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "runoff_time_scale", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater"), "set_runoff_time_scale", "get_runoff_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "standing_water"), "set_standing_water", "is_standing_water");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "muddy_water"), "set_muddy_water", "is_muddy_water");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tracked_mud"), "set_tracked_mud", "is_tracked_mud");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stream_stones"), "set_stream_stones", "is_stream_stones");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "trail_pugging"), "set_trail_pugging", "is_trail_pugging");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clods"), "set_clods", "is_clods");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "keep_ruts"), "set_keep_ruts", "is_keeping_ruts");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "ruts_path"), "set_ruts_path", "get_ruts_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ruts_time_scale", PROPERTY_HINT_RANGE, "0,1000,0.01,or_greater"), "set_ruts_time_scale", "get_ruts_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ruts_away_rain", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:mm/h"), "set_ruts_away_rain", "get_ruts_away_rain");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ruts_regrowth_time", PROPERTY_HINT_RANGE, "3600,31536000,1,or_greater,suffix:s"), "set_ruts_regrowth_time", "get_ruts_regrowth_time");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "desiccation_cracks"), "set_desiccation_cracks", "is_desiccation_cracks");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "puddle_ice"), "set_puddle_ice", "is_puddle_ice");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "plant_mud"), "set_plant_mud", "is_plant_mud");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "frozen_ground"), "set_frozen_ground", "is_frozen_ground");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "needle_ice"), "set_needle_ice", "is_needle_ice");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "world_puddles"), "set_world_puddles", "is_world_puddles");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "world_puddle_density", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_world_puddle_density", "get_world_puddle_density");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "wind_shelters", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_wind_shelters", "get_wind_shelters");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "obstacle_lee"), "set_obstacle_lee", "is_obstacle_lee_enabled");
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
	deform_groups.push_back("world_contact"); // every body any system should feel (docs/ideas/world-links.md)
}

GroundSurface3D::~GroundSurface3D() {
	WorldContracts::unregister_ground(this);
	if (active == this) {
		active = nullptr;
	}
}

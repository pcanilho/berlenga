/**************************************************************************/
/*  physx_vehicle_3d.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

#include "physx_vehicle_3d.h"

#include "../godot_physx_conversions.h"
#include "../godot_physx_project_settings.h"
#include "../godot_physx_server_3d.h"
#include "../spaces/godot_physx_space_3d.h"
#include "godot_physx_vehicle4w.h"
#include "physx_vehicle_wheel_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "servers/rendering/rendering_server.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/curve.h"
#include "scene/resources/material.h"

#include "modules/modules_enabled.gen.h" // For ground.

#ifdef MODULE_GROUND_ENABLED
#include "modules/ground/ground_surface_3d.h"
#include "modules/ground/ground_terramechanics.h"
#endif
#include "modules/world/world_contracts.h"


// Soft soil (docs/ideas/sand.md, stage 1): the wheels of a vehicle on a GroundSurface3D's soft material sink, print
// a rut, and feel the soil (Bekker and Wong terramechanics, as Project Chrono's Soil Contact Model; see
// modules/ground/ground_terramechanics.h). Two components in the vehicle's own sequence:
// - SoilRoad, once a step after the road query: each wheel's road plane is lowered by its sinkage (static
//   sinkage from its load and tyre, slip sinkage when it spins), and the rut is pressed into the field.
//   The soil also sets the tyre's grip: its friction to the shear limit (Mohr-Coulomb, A c + W tan phi) and its
//   longitudinal stiffness to the initial slope of the Janosi-Hanamoto curve, W tan phi L / (2 K), so the tyre
//   model's own smooth force curve rises and saturates as the soil's does.
// - SoilTire, in each substep after the tire model: the motion resistance (compaction and bulldozing of the soil
//   not yet pressed) acts at the axle against the travel. A towed wheel's soil reaction passes through
//   its axle (Wong 2008, sec. 2.4), so the resistance gives the wheel no spin torque: a driven wheel's net pull
//   is its traction less the resistance, and a car whose resistance passes its traction digs in.
struct PhysXVehicleSoilWheel {
	float inflation = 200.0f; // kPa, from the wheel node
	int tread = 1; // PhysXVehicleWheel3D::Tread, from the wheel node
	float sinkage = 0.0f; // m, smoothed
	float already = 0.0f; // m, the rut already pressed ahead of the wheel
	float resistance = 0.0f; // N, the last substep's
	float side_bulldozing = 0.0f; // N, the sidewall's push against a side slide, the last substep's
	float traction = 0.0f; // N, the soil's limit at the last substep's slip
	float base_long_stiff = -1.0f; // the tyre's own, restored off the soil
	float base_lat_stiff = -1.0f; // the tyre's own at road pressure (round 12: scaled by its inflation)
	float deflect = 0.0f; // m, the tyre's deflection past its deflection at road pressure (round 12)
	float spin_sum = 0.0f; // rad/s, summed over the step's substeps
	int spin_n = 0;
	float slip = 0.0f; // terramechanics slip, smoothed over a tenth of a second
	bool soft = false;
	bool printed = false;
	Vector3 last_print;
	// Mud in the tread (docs/ideas/mud.md, stage 3): 0 clean, 1 full; and the clay it has thrown off, kg.
	float mud = 0.0f;
	float shed = 0.0f;
	float grip = 1.0f; // the share of the clean shear limit it keeps
	float base_moi = -1.0f;
	// Ruts steer the wheels (docs/ideas/mud.md, round 9): the deformation's cross slope under the tread along the
	// tyre's lateral direction, and the rut wall's height beside each sidewall (GroundSurface3D::get_rut_guidance()).
	Vector3 guide;
	// Parked in mud (docs/ideas/mud.md, round 22.2): world seconds standing on this soil, the creep settlement it has
	// brought, and the suction holding the tyre (N, the last substep's).
	float rest = 0.0f;
	float moved = 0.0f; // m rolled since it last stood
	float bite_owed = 0.0f; // m3 of soil the tread's links have cut and not yet thrown (round 23.3)
	float bite_thrown = 0.0f; // kg thrown since start
	float creep = 0.0f;
	float suction = 0.0f;
#ifdef MODULE_GROUND_ENABLED
	GroundSoil soil;
#endif
};

#ifdef MODULE_GROUND_ENABLED
// The slip of terramechanics (Wong 2008, sec. 2.4), not the tyre model's: i = 1 - v / (omega r) driving, and
// i = omega r / v - 1 skidding, so it stays within -1 and 1 at any speed (the tyre model's own slip runs to
// thousands while a wheel spins up from rest). Below 3 m/s it is measured against 3 m/s, as the tyre model's
// own is on soft soil (see SoilRoad): a wheel creeping under a load it cannot move is not spinning. p_spin: the wheel's
// rotation speed averaged over the last step's substeps (at low speed it swings from one substep to the next;
// PhysX's own note on PxVehicleWheelRigidBody1dState).
static constexpr float SOIL_SLIP_SPEED = 3.0f;

static float _soil_slip(const Vehicle4W &p_v, PxU32 p_i, float p_spin) {
	const float wr = p_spin * p_v.wheelParams[p_i].radius;
	const float vx = p_v.tireSpeedStates[p_i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL];
	const float d = MAX(MAX(Math::abs(wr), Math::abs(vx)), SOIL_SLIP_SPEED);
	return CLAMP((wr - vx) / d, -1.0f, 1.0f);
}

// Mud in the tread (docs/ideas/mud.md, stage 3): sticky clay packs into the tread as it slips through it; a tread
// turning fast enough flings it off (w^2 r past ca / (rho t): GroundTerramechanics::shedding_speed()); standing water
// washes it out; on firm ground it falls off slowly. A full tread grips as a slick and adds its mass to the wheel.
static constexpr float CLAY_DENSITY = 1700.0f; // kg/m3

static void _clog_update(PhysXVehicleSoilWheel &p_sw, Vehicle4W &p_v, PxU32 p_i, float p_spin, float p_dt, bool p_soft, float p_water) {
	const PxVehicleWheelParams &wp = p_v.wheelParams[p_i];
	const float r = wp.radius;
	const float tread = Math::abs(p_spin) * r; // m/s
	const float revs = tread / ((float)Math::TAU * r); // per second
	float &f = p_sw.mud;
	const float before = f;
	const PhysXVehicleWheel3D::TreadSoil ts = PhysXVehicleWheel3D::tread_soil((PhysXVehicleWheel3D::Tread)p_sw.tread);
	if (p_soft && p_sw.soil.stickiness > 0.0f) {
		// A quarter of an all terrain tread fills per turn at full slip in the stickiest clay, a tenth rolling.
		f += 0.25f * ts.fill * p_sw.soil.stickiness * (0.4f + 0.6f * Math::abs(p_sw.slip)) * revs * (1.0f - f) * p_dt;
		const float shed_speed = GroundTerramechanics::shedding_speed(p_sw.soil, r) * ts.shed;
		if (tread > shed_speed) {
			f -= f * MIN(2.0f * (tread / shed_speed - 1.0f) * p_dt, 1.0f);
		}
	} else {
		// Firm ground or no soil: it dries and drops, more as the wheel turns over bumps.
		f -= f * MIN((0.02f + 0.05f * revs) * p_dt, 1.0f);
		if (p_soft) {
			// Clean soil (sand, dry ground) scours it.
			f -= f * MIN(0.2f * revs * p_dt, 1.0f);
		}
	}
	if (p_water > 0.02f) {
		// Washed in standing water, faster as the wheel turns in it.
		f -= f * MIN(MIN(p_water / 0.1f, 1.0f) * (0.2f + 0.5f * revs) * p_dt, 1.0f);
	}
	f = CLAMP(f, 0.0f, 1.0f);
	const float full_kg = (float)Math::TAU * r * 2.0f * wp.halfWidth * ts.depth * 0.5f * CLAY_DENSITY;
	if (f < before) {
		p_sw.shed += (before - f) * full_kg;
	}
	if (p_sw.base_moi < 0.0f) {
		p_sw.base_moi = p_v.wheelParams[p_i].moi;
	}
	p_v.wheelParams[p_i].moi = p_sw.base_moi + f * full_kg * r * r;
}

// Grip of firm ground for its state (docs/ideas/world-links.md, link 3), as a share of the surface's dry friction:
// - wet: a water film takes a tyre's peak friction on asphalt from about 0.9-1.0 to 0.5-0.8, falling with speed
//   as the film has less time to squeeze out (Pacejka 2012, ch. 4; Leu and Henry 1978's speed gradient);
// - aquaplaning: water deeper than the tread can drain lifts the tyre near Horne's speed, v = 6.36 sqrt(p) km/h
//   for an inflation pressure p in kPa (Horne and Dreher 1963), so a car on 220 kPa floats from about 94 km/h (the tyre's own pressure: aired down it floats sooner);
// - ice: about 0.1-0.2 on cold ice and nearer 0.05-0.1 on ice at its melting point under a water film (Hayhoe
//   and Shapley 1989's winter runway measurements): puddle ice from the ground (unless broken into plates), and
//   a glaze where a wet surface's film freezes in the air below zero (black ice).
static constexpr float TYRE_PRESSURE_KPA = 220.0f;

// The tread scales the result by what covers the surface (PhysXVehicleWheel3D::tread_firm_grip: chains bite ice,
// lugs bite snow). Snow from the snow contract (WorldSnow): a tyre on snow holds about 0.2 on new powder to 0.35 on
// packed snow, less as it turns to slush (Shoop et al., CRREL tyre traction on snow; winter-tyre tests put packed
// snow at 0.2-0.4); a hard melt-freeze crust is near ice, about 0.15. Bare lake ice counts as ice. Where the snow
// system keeps its own friction (WorldSnow.grip_at: packed roads, snow polished to ice) that is used instead.
static float _firm_grip(const Vector3 &p_at, float p_speed, float p_standing_water, const Vector2 &p_ice, int p_tread, float p_inflation = TYRE_PRESSURE_KPA) {
	float film = 0.0f;
	float depth = MAX(p_standing_water, 0.0f); // m
	float air = 10.0f;
	if (const WorldWeather *wm = WorldContracts::weather(); wm->has_provider()) {
		film = wm->surface_wetness_at(p_at);
		depth = MAX(depth, wm->puddle_at(p_at) * 0.001f);
		air = wm->air_temperature_at(p_at);
	}
	const float kmh = Math::abs(p_speed) * 3.6f;
	const float wet = MAX(CLAMP(film, 0.0f, 1.0f), Math::smoothstep(0.0f, 0.0005f, depth));
	float grip = Math::lerp(1.0f, 0.75f - 0.15f * Math::smoothstep(0.0f, 120.0f, kmh), wet);
	const float horne = 6.36f * Math::sqrt(MAX(p_inflation, 10.0f));
	grip *= 1.0f - 0.9f * Math::smoothstep(0.001f, 0.006f, depth) * Math::smoothstep(0.7f * horne, horne, kmh);
	const float glaze = wet * Math::smoothstep(0.0f, 2.0f, -air);
	const float sheet = CLAMP(p_ice.x / 0.002f, 0.0f, 1.0f) * (1.0f - CLAMP(p_ice.y, 0.0f, 1.0f));
	const float ice_grip = Math::lerp(0.08f, 0.15f, Math::smoothstep(-0.5f, -5.0f, air));
	float ice = MAX(glaze, sheet);
	float snow = 0.0f;
	float snow_grip = 1.0f;
	float glazed = 0.0f;
	if (const WorldSnow *ws = WorldContracts::snow(); ws && ws->has_provider()) {
		const Vector2 lake = ws->lake_ice_at(p_at);
		ice = MAX(ice, Math::smoothstep(0.005f, 0.02f, (float)lake.x) * (1.0f - CLAMP((float)lake.y, 0.0f, 1.0f)));
		const float d = ws->depth_at(p_at);
		snow = Math::smoothstep(0.003f, 0.02f, d) * (1.0f - ice);
		if (snow > 0.0f) {
			// The snow system's own tyre friction where it keeps one (SnowCover3D: traffic packs the snow and polishes
			// it toward ice); else estimated from the pack's density, wetness and crust.
			const float g = ws->grip_at(p_at);
			if (g >= 0.0f) {
				snow_grip = g;
				// Snow polished to ice (under about 0.15) bites chains and studs as ice does.
				glazed = snow * Math::smoothstep(0.2f, 0.1f, g);
			} else {
				const float rho = ws->density_at(p_at);
				snow_grip = Math::lerp(0.2f, 0.35f, Math::smoothstep(100.0f, 450.0f, rho)) * (1.0f - 0.3f * CLAMP(ws->wetness_at(p_at), 0.0f, 1.0f));
				snow_grip = Math::lerp(snow_grip, 0.15f, Math::smoothstep(50.0f, 300.0f, ws->crust_at(p_at)));
			}
		}
	}
	grip = Math::lerp(grip, MIN(snow_grip, grip), snow);
	return Math::lerp(grip, MIN(ice_grip, grip), ice) * PhysXVehicleWheel3D::tread_firm_grip((PhysXVehicleWheel3D::Tread)p_tread, MAX(ice, glazed), snow - glazed);
}

class PhysXVehicleSoilRoad : public PxVehicleComponent {
public:
	Vehicle4W *vehicle = nullptr;
	PhysXVehicleSoilWheel *wheels = nullptr;
	PxVehicleSimulationContext *context = nullptr;
	float base_slip_denominator = -1.0f;
	float base_passive_denominator = -1.0f;
	float base_sticky_speed = -1.0f;
	float base_lat_sticky_speed = -1.0f;
	bool clogging = true;
	bool parked = true; // round 22.2: parked wheels settle into mud

	virtual bool update(const PxReal p_dt, const PxVehicleSimulationContext &p_context) override {
		Vehicle4W &v = *vehicle;
		GroundSurface3D *ground = GroundSurface3D::get_active();
		if (base_slip_denominator < 0.0f) {
			base_slip_denominator = context->tireSlipParams.minActiveLongSlipDenominator;
			base_passive_denominator = context->tireSlipParams.minPassiveLongSlipDenominator;
			base_sticky_speed = context->tireStickyParams.stickyParams[PxVehicleTireDirectionModes::eLONGITUDINAL].thresholdSpeed;
			base_lat_sticky_speed = context->tireStickyParams.stickyParams[PxVehicleTireDirectionModes::eLATERAL].thresholdSpeed;
		}
		// The tyre model's slip divides by the forward speed, floored at 0.1 m/s while driven: at walking pace its
		// force swings hard with the wheel's speed, and the wheel's explicit integration rings between full
		// drive and full braking from one substep to the next (PhysX's own note on PxVehicleTireSlipParams:
		// larger time steps need a larger floor). A car on firm ground is past that in a moment; one held slow by
		// soft soil is not, so on soft soil the floor is 3 m/s and its stiffness is kept within what the substep
		// integrates stably.
		bool any_soft = false;
		for (PxU32 i = 0; i < 4; i++) {
			PhysXVehicleSoilWheel &sw = wheels[i];
			PxVehicleRoadGeometryState &road = v.roadGeomStates[i];
			if (sw.base_long_stiff < 0.0f) {
				sw.base_long_stiff = v.tireForceParams[i].longStiff;
			}
			v.tireForceParams[i].longStiff = sw.base_long_stiff;
			// Tyre pressure (docs/ideas/mud.md, round 12). An aired-down tyre stands lower on its flat (the deflection
			// past the one at road pressure lowers the axle), and its softer carcass gives less cornering stiffness:
			// MF 6.1's pressure term, (1 + p_py1 dp) with dp = (p - p0) / p0 (Besselink, Schmeitz and Pacejka 2010;
			// p_py1 about 0.6 for passenger and light truck tyres), kept within half to 1.15 times.
			if (sw.base_lat_stiff < 0.0f) {
				sw.base_lat_stiff = v.tireForceParams[i].latStiffY;
			}
			{
				const float dp = (sw.inflation - GroundTerramechanics::TYRE_ROAD_KPA) / GroundTerramechanics::TYRE_ROAD_KPA;
				v.tireForceParams[i].latStiffY = sw.base_lat_stiff * CLAMP(1.0f + 0.6f * dp, 0.5f, 1.15f);
				const PxVehicleWheelParams &tw = v.wheelParams[i];
				const float tl = v.tireGripStates[i].load;
				const float tw2 = 2.0f * tw.halfWidth;
				const float target = tl > 0.0f ? GroundTerramechanics::tyre_flat(tl, tw.radius, tw2, sw.inflation).deflection - GroundTerramechanics::tyre_flat(tl, tw.radius, tw2, GroundTerramechanics::TYRE_ROAD_KPA).deflection : 0.0f;
				// Settles over a tenth of a second, as the load does.
				sw.deflect += (target - sw.deflect) * MIN(p_dt / 0.1f, 1.0f);
				if (road.hitState) {
					road.plane.d += sw.deflect;
				}
			}
			if (!ground || !road.hitState) {
				if (clogging) {
					_clog_update(sw, v, i, v.wheelRigidBody1dStates[i].rotationSpeed, p_dt, false, 0.0f);
				}
				sw.grip = 1.0f;
				if (road.hitState) {
					// No ground surface (a street, a road mesh): the weather's wet, flooded and frozen state still holds.
					const PxTransform hp = v.rigidBodyState.pose * v.wheelLocalPoses[i].localPose;
					sw.grip = _firm_grip(Vector3(hp.p.x, hp.p.y, hp.p.z), v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL], 0.0f, Vector2(), sw.tread, sw.inflation);
					road.friction *= sw.grip;
				}
				sw.soft = false;
				sw.printed = false;
				sw.sinkage = 0.0f;
				sw.slip = 0.0f;
				sw.spin_sum = 0.0f;
				sw.spin_n = 0;
				continue;
			}
			const PxTransform pose = v.rigidBodyState.pose * v.wheelLocalPoses[i].localPose;
			const PxVec3 n = road.plane.n;
			const PxVec3 contact = pose.p - n * (n.dot(pose.p) + road.plane.d);
			const Vector3 at((real_t)contact.x, (real_t)contact.y, (real_t)contact.z);
			sw.soil = ground->get_soil(at);
			sw.soft = sw.soil.soft;
			// On a log or branch laid on the ground (round 12): the tyre bears on the wood (its own friction, from the
			// body's material), which carries the load into the soil along its length and sinks with it.
			// The wheel bridges the gaps between logs (the road query's ray can pass between them): it rests on the
			// highest log whose top it touches.
			if (sw.soft) {
				float support = 0.0f;
				const int mat = ground->get_mat_under_wheel(Vector3(pose.p.x, pose.p.y, pose.p.z), v.wheelParams[i].radius, support);
				if (mat >= 0 && support > (real_t)contact.y - 0.3f) {
					ground->add_mat_load(mat, v.tireGripStates[i].load);
					const PxVec3 on(contact.x, MAX(support, contact.y), contact.z);
					road.plane.d = -n.dot(on) + sw.deflect;
					road.friction = ground->get_mat_friction(mat);
					sw.soft = false;
				}
			}
			if (!sw.soft) {
				if (clogging) {
					_clog_update(sw, v, i, v.wheelRigidBody1dStates[i].rotationSpeed, p_dt, false, ground->get_standing_water_at(at));
				}
				sw.grip = _firm_grip(at, v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL], ground->get_standing_water_at(at), ground->get_ice_at(at), sw.tread, sw.inflation);
				// A skin of silt a flood left (mud round 15.1): fine earth on a firm surface takes the friction down to
				// about half while wet, a little when dry dust (SAE 2007-01-0736; TRL PPR073).
				const Vector2 silt = ground->get_silt_at(at);
				if (silt.x > 0.0f) {
					sw.grip *= Math::lerp(1.0f, Math::lerp(0.85f, 0.5f, CLAMP((float)silt.y, 0.0f, 1.0f)), Math::smoothstep(0.0003f, 0.002f, (float)silt.x));
				}
				road.friction *= sw.grip;
				sw.printed = false;
				sw.sinkage = 0.0f;
				sw.slip = 0.0f;
				sw.spin_sum = 0.0f;
				sw.spin_n = 0;
				continue;
			}
			const PxVehicleWheelParams &wp = v.wheelParams[i];
			const float width = 2.0f * wp.halfWidth;
			const float load = v.tireGripStates[i].load;
			const float spin = sw.spin_n > 0 ? sw.spin_sum / sw.spin_n : v.wheelRigidBody1dStates[i].rotationSpeed;
			sw.spin_sum = 0.0f;
			sw.spin_n = 0;
			sw.slip += (_soil_slip(v, i, spin) - sw.slip) * MIN(p_dt / 0.1f, 1.0f);
			const float slip = sw.slip;
			if (clogging) {
				_clog_update(sw, v, i, spin, p_dt, true, ground->get_standing_water_at(at));
			} else {
				sw.mud = 0.0f;
			}
			const PxVec3 lng = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLONGITUDINAL];
			const PxVec3 lat = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLATERAL];
			const float v_lng = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL];
			// The soil the wheel is about to press: just ahead of its contact.
			// Sliding sideways (off camber, in a drift) it digs in as it does spinning (mud round 13.3).
			// Only a real slide: at a crawl the slip angle is noise, so it comes in over 0.1 to 0.4 m/s of side speed.
			const float side_slip = v.tireSlipStates[i].slips[PxVehicleTireDirectionModes::eLATERAL] * Math::smoothstep(0.1f, 0.4f, Math::abs(v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLATERAL]));
			const GroundWheelSoil probe = GroundTerramechanics::wheel(sw.soil, load, wp.radius, width, sw.inflation, slip, side_slip, 0.0f);
			const float ahead = (v_lng >= 0.0f ? 1.0f : -1.0f) * (0.5f * probe.contact_length + 0.05f);
			const Vector3 front = at + Vector3(lng.x, lng.y, lng.z) * ahead;
			sw.already = MAX(ground->get_dent_at(front) - ground->get_rim_at(front), 0.0f);
			sw.guide = ground->get_rut_guidance(at, Vector3(lat.x, lat.y, lat.z), wp.halfWidth);
			// A rut already there ahead (another wheel's, an earlier pass) takes the wheel down into it.
			const float rut = MIN(ground->get_dent_at(front) - ground->get_rim_at(front), sw.soil.max_sinkage);
			// The load's sinkage settles over a tenth of a second (the soil yields as the load comes). Slip
			// sinkage is a steady state over fresh soil (Lyasko 2010): it builds over the travel of a contact
			// length, and a wheel spinning in place digs in over seconds rather than at once. A skidding wheel
			// ploughs at its static sinkage.
			// Parked in mud (round 22.2): a load left standing on soft saturated soil keeps settling after the
			// undrained sinkage by creep, secondary compression growing with the log of time (Buisman 1936):
			// dz = C D log10(1 + t / 1 s), C 0.02 for soft clays and organic mud (C_alpha / (1 + e0)), D the
			// depth the load stresses (two tyre widths, within the loose layer), scaled by how sticky (saturated,
			// clayey) the soil is. The time is the world's (WorldWeather.time_scale()). Rolling away, the
			// wheel climbs out of its hollow over a contact length.
			float creep = 0.0f;
			if (parked && sw.soil.stickiness > 0.0f) {
				const float travel = Math::abs(v_lng) * p_dt;
				const float cl = MAX(probe.contact_length, 0.05f);
				if (Math::abs(v_lng) < 0.05f) {
					float scale = 1.0f;
					if (const WorldWeather *wm = WorldContracts::weather(); wm && wm->has_provider()) {
						scale = MAX(float(wm->time_scale()), 0.0f);
					}
					if (sw.moved > 3.0f * cl) {
						sw.rest = 0.0f; // stopped somewhere new
					}
					sw.moved = 0.0f;
					sw.rest += p_dt * scale;
				} else {
					sw.moved += travel;
				}
				const float d = MIN(2.0f * width, MAX(sw.soil.max_sinkage - probe.static_sinkage, 0.0f));
				creep = 0.02f * sw.soil.stickiness * d * (Math::log(1.0f + sw.rest) / 2.302585f) * Math::exp(-sw.moved / cl);
			} else {
				sw.rest = 0.0f;
			}
			sw.creep = creep;
			const float static_z = MAX(probe.static_sinkage + creep, rut);
			const float target = MAX((slip > 0.0f ? probe.sinkage : probe.static_sinkage) + creep, rut);
			if (sw.sinkage < static_z) {
				sw.sinkage += (static_z - sw.sinkage) * MIN(p_dt / 0.1f, 1.0f);
			}
			const float tau = MAX(0.1f, probe.contact_length / MAX(Math::abs(v_lng), 0.1f));
			sw.sinkage += (target - sw.sinkage) * MIN(p_dt / tau, 1.0f);
			// A stone or root hidden in the mud under the tyre's middle (mud round 14.3): the wheel sinks no further
			// than its top and rides up onto it. One under the tyre's edge it wraps round, the mud pressed out round
			// it (round 15.2); uncovered, the stone's own body meets the tyre.
			sw.sinkage = MIN(sw.sinkage, ground->get_buried_depth_at(at, 0.5f * wp.halfWidth));
			road.plane.d += sw.sinkage;
			// The grip of the soil: the tyre's peak force to its shear limit, its stiffness to Janosi-Hanamoto's.
			if (load > 0.0f) {
				const PxVehicleTireForceParams &tp = v.tireForceParams[i];
				const float peak = MAX(MAX(tp.frictionVsSlip[0][1], tp.frictionVsSlip[1][1]), MAX(tp.frictionVsSlip[2][1], 0.05f));
				// A clogged tread grips as a slick (stage 3).
				// The tread's lugs or chains shear deeper; chains still bite through a clogged tread's slick (round 10).
				const PhysXVehicleWheel3D::TreadSoil ts = PhysXVehicleWheel3D::tread_soil((PhysXVehicleWheel3D::Tread)sw.tread);
				sw.grip = Math::lerp(1.0f, GroundTerramechanics::clogged_grip(sw.soil, load, probe.contact_length * width), sw.mud * (1.0f - ts.clog_bite)) * ts.grip;
				// Down on a firm subsoil (round 12) the tread grips on the base: from the wheel's actual sinkage, ruts
				// included, not the probe's fresh-soil one.
				// A clogged tread's slick only matters in the soil: on the base the wet rubber grips the firm clay or gravel.
				const float limit = GroundTerramechanics::pan_shear_limit(sw.soil, load, sw.sinkage, probe.shear_limit * sw.grip);
				road.friction = limit / load / peak;
				const float slope = GroundTerramechanics::pan_shear_limit(sw.soil, load, sw.sinkage, probe.shear_limit) * probe.contact_length / (2.0f * MAX(sw.soil.shear_k, 1e-3f));
				const float substep = p_dt / 3.0f;
				const float stable = 1.5f * wp.moi * SOIL_SLIP_SPEED / (MAX(substep, 1e-4f) * wp.radius * wp.radius);
				v.tireForceParams[i].longStiff = MIN(MIN(sw.base_long_stiff, slope), stable);
			}
			any_soft = true;
			// The rut: the contact patch, stretched over the travel since the last print so a fast wheel
			// leaves no gaps. Laid along the travel, at the width the tyre sweeps across it: rolling, its own
			// width; sliding sideways in a drift, up to its contact length (a wide scuff).
			const float v_lat = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLATERAL];
			const PxVec3 omega_r = lng * (v.wheelRigidBody1dStates[i].rotationSpeed * wp.radius);
			// The contact's sliding velocity over the ground: the tread's longitudinal slip and the side slip.
			// It drives the press's bow wave (a berm heaped on the side the wheel slides toward) and the throw.
			const PxVec3 slip_vel = lng * v_lng - omega_r + lat * v_lat;
			const Vector3 travel = sw.printed ? at - sw.last_print : Vector3();
			const float travel_len = travel.length();
			const bool along = sw.printed && travel_len < 2.0f && travel_len > 1e-3f;
			const Vector3 center = along ? at - travel * 0.5f : at;
			const Vector3 lng_v(lng.x, lng.y, lng.z);
			const Vector3 lat_v(lat.x, lat.y, lat.z);
			float across = width;
			float length = probe.contact_length;
			float yaw = Math::atan2(-lat.z, lat.x);
			if (along) {
				const Vector3 dir = travel / travel_len;
				const float c = Math::abs(dir.dot(lng_v));
				const float sn = Math::abs(dir.dot(lat_v));
				across = width * c + probe.contact_length * sn;
				length = travel_len + probe.contact_length * c + width * sn;
				const Vector3 side = dir.cross(Vector3(n.x, n.y, n.z)).normalized();
				yaw = Math::atan2(-side.z, side.x);
			}
			if (sw.sinkage > 0.002f) {
				ground->press_wheel(center, Vector2(across, length), yaw, sw.sinkage, Vector2(slip_vel.x, slip_vel.z), sw.tread);
			}
			// A wheel turning faster than it travels shears its tread through the sand and throws it back
			// (roost); one sliding sideways drags and shoves sand out of the side of the turn: reported for a
			// grains node to lift out of the field (sand stage 2). The grains leave the edge of the contact
			// the tread slides toward, at about the sliding speed, rising as the tread turns up.
			const PxVec3 slide = slip_vel - n * n.dot(slip_vel);
			const float slip_speed = slide.magnitude();
			if (slip_speed > 0.5f) {
				GroundSurface3D::Spray spray;
				const PxVec3 back = slide / slip_speed;
				const float reach = 0.5f * (Math::abs(back.dot(lng)) * probe.contact_length + Math::abs(back.dot(lat)) * width);
				const PxVec3 from = contact + back * reach;
				const PxVec3 up = n;
				// Sand slides on the rubber and leaves slower than the tread, at ~30 degrees from the rear of a
				// sunken contact; past ~8 m/s of slip the throw stops growing (the sand shears off the lugs).
				const float throw_speed = MIN(slip_speed, 8.0f);
				const PxVec3 vel = lng * v_lng + back * (throw_speed * 0.7f) + up * (throw_speed * 0.45f);
				spray.position = Vector3(from.x, from.y - sw.sinkage, from.z);
				spray.velocity = Vector3(vel.x, vel.y, vel.z);
				spray.width = width;
				spray.slip_speed = slip_speed;
				spray.reach_center = Vector3(contact.x, contact.y, contact.z);
				spray.reach_radius = 0.5f * MAX(width, probe.contact_length) + 0.1f;
				// The sidewall sweeps a prism one contact length long and the sinkage deep through the sand per
				// metre of side slide; about half of it heaps into the berm (the press's bow), half is thrown.
				spray.side_share = Math::abs(back.dot(lat));
				spray.side_volume_rate = 0.5f * probe.contact_length * sw.sinkage * Math::abs(v_lat);
				spray.stickiness = sw.soil.stickiness;
				ground->add_spray(spray);
			}
			// Dust: the fines of dry, bare sand lifted by a tyre rolling and sliding over it (Gillies et al. 2005:
			// emission linear in speed), carried off by a dust node if there is one.
			const float ground_speed = Math::sqrt(v_lng * v_lng + v_lat * v_lat);
			if (ground_speed + slip_speed > 0.5f) {
				const PxVec3 cv = lng * v_lng + lat * v_lat;
				// The car's wake stirs the plume up to about its own height: an upward jet growing with speed, which
				// the air slows within the dust node's jet_time.
				const PxVec3 jet = (slip_speed > 0.5f ? slide * (0.5f / slip_speed) * MIN(slip_speed, 8.0f) : PxVec3(0.0f)) + cv * 0.3f + n * (0.5f + 0.35f * ground_speed + 0.15f * slip_speed);
				ground->add_dust(Vector3(contact.x, contact.y - sw.sinkage + 0.05f, contact.z), Vector3(jet.x, jet.y, jet.z), width * (ground_speed + 2.0f * slip_speed));
			}
			sw.last_print = at;
			sw.printed = true;
		}
		// Braked with no throttle (mud round 11, the winch): a car dragged on locked wheels at walking pace (a winch's
		// 0.1 m/s) slid on a seventh of its tyres' friction. Under 0.2 m/s PhysX swaps the tyre's force for its
		// sticky-tyre constraint, a soft damper with no friction limit, too weak to hold against a pull; above it a
		// braked wheel's slip is over the 3 m/s floor kept for driven wheels on soft soil (4 m/s undriven), a few per
		// cent at that pace. Held, the sticky regime starts only at rest (0.02 m/s) and the floors are 0.5 m/s: a locked
		// wheel does not turn, so the floor's ringing cannot start, and it slides on near its full friction (static, then
		// kinetic friction, as a dragged car does).
		const bool held = v.commandState.throttle <= 0.0f && v.commandState.nbBrakes > 0 && (v.commandState.brakes[0] > 0.5f || (v.commandState.nbBrakes > 1 && v.commandState.brakes[1] > 0.5f));
		context->tireSlipParams.minActiveLongSlipDenominator = held ? 0.5f : (any_soft ? MAX(base_slip_denominator, SOIL_SLIP_SPEED) : base_slip_denominator);
		context->tireSlipParams.minPassiveLongSlipDenominator = held ? MIN(base_passive_denominator, 0.5f) : base_passive_denominator;
		context->tireStickyParams.stickyParams[PxVehicleTireDirectionModes::eLONGITUDINAL].thresholdSpeed = held ? MIN(base_sticky_speed, 0.02f) : base_sticky_speed;
		// Sideways too (mud round 20.1): a parked car with a current against its side crept off at 10 cm/s under
		// 40 N, the sticky damper again; held, the tyre's own lateral force holds it to its friction.
		context->tireStickyParams.stickyParams[PxVehicleTireDirectionModes::eLATERAL].thresholdSpeed = held ? MIN(base_lat_sticky_speed, 0.02f) : base_lat_sticky_speed;
		return true;
	}
};

class PhysXVehicleSoilTire : public PxVehicleComponent {
public:
	Vehicle4W *vehicle = nullptr;
	PhysXVehicleSoilWheel *wheels = nullptr;
	bool adhesion = true;
	bool parked = true;

	virtual bool update(const PxReal p_dt, const PxVehicleSimulationContext &p_context) override {
		Vehicle4W &v = *vehicle;
		for (PxU32 i = 0; i < 4; i++) {
			PhysXVehicleSoilWheel &sw = wheels[i];
			sw.spin_sum += v.wheelRigidBody1dStates[i].rotationSpeed;
			sw.spin_n++;
			// An aired-down tyre's carcass flexes more as it rolls: the rolling resistance past the one at road
			// pressure (Gillespie 1992, a hard-surface relation), at the axle against the travel on firm ground (on soft
			// soil the soil's own compaction and bulldozing are the resistance), eased in over 0.3 m/s.
			if (!sw.soft && v.roadGeomStates[i].hitState && sw.inflation < GroundTerramechanics::TYRE_ROAD_KPA && v.tireGripStates[i].load > 0.0f) {
				const float v_roll = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL];
				const float extra = (GroundTerramechanics::tyre_rolling_coefficient(sw.inflation, v_roll) - GroundTerramechanics::tyre_rolling_coefficient(GroundTerramechanics::TYRE_ROAD_KPA, v_roll)) * v.tireGripStates[i].load * CLAMP(v_roll / 0.3f, -1.0f, 1.0f);
				if (extra != 0.0f) {
					const PxVec3 lng_r = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLONGITUDINAL];
					const PxVec3 dF = lng_r * -extra;
					const PxVec3 arm = v.rigidBodyState.pose.rotate(v.suspensionParams[i].suspensionAttachment.transform(v.suspensionComplianceStates[i].tireForceAppPoint));
					v.tireForces[i].forces[PxVehicleTireDirectionModes::eLONGITUDINAL] += dF;
					v.tireForces[i].torques[PxVehicleTireDirectionModes::eLONGITUDINAL] += arm.cross(dF);
				}
			}
			if (!sw.soft || !v.roadGeomStates[i].hitState) {
				sw.resistance = 0.0f;
				sw.side_bulldozing = 0.0f;
				sw.traction = 0.0f;
				continue;
			}
			const PxVehicleWheelParams &wp = v.wheelParams[i];
			const float load = v.tireGripStates[i].load;
			if (load <= 0.0f) {
				continue;
			}
			const float slip = sw.slip;
			const float slip_angle = v.tireSlipStates[i].slips[PxVehicleTireDirectionModes::eLATERAL];
			const GroundWheelSoil ws = GroundTerramechanics::wheel(sw.soil, load, wp.radius, 2.0f * wp.halfWidth, sw.inflation, slip, slip_angle, sw.already);
			PxVehicleTireForce &f = v.tireForces[i];
			const PxVec3 lng = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLONGITUDINAL];
			// The motion resistance, against the travel, through the axle; eased in over the first 0.3 m/s so a
			// car at rest is not pushed back and forth.
			const float v_lng = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL];
			// At the wheel's actual sinkage (which builds over time), not the steady state of this slip.
			const float width = 2.0f * wp.halfWidth;
			// Plus sticky clay holding the tread as it leaves the rut (stage 3).
			// Suction (round 22.2): a footing left in saturated clay is held by the negative pore pressure under it
			// as it is pulled away, a breakout force past its weight that grows as the soil sets up round it (Vesic
			// 1969, breakout of objects from the sea bottom; Das and Seeley 1982 on the time to set up). Here the
			// trailing half of the contact peels at the soil's undrained strength (its cohesion), building over
			// about ten minutes of rest (1 - exp(-t / 600 s)), scaled by the soil's stickiness, and gone once the
			// wheel has rolled out (the rest decays over a contact length of travel).
			sw.suction = 0.0f;
			if (parked && sw.rest > 0.0f && sw.soil.stickiness > 0.0f) {
				sw.suction = 1000.0f * sw.soil.cohesion * 0.5f * ws.contact_length * width * sw.soil.stickiness * (1.0f - Math::exp(-sw.rest / 600.0f)) * Math::exp(-sw.moved / MAX(ws.contact_length, 0.05f));
			}
			const float resist = GroundTerramechanics::compaction_resistance(sw.soil, width, sw.sinkage, sw.already) + GroundTerramechanics::wheel_bulldozing_resistance(sw.soil, width, wp.radius, sw.sinkage, sw.already) + (adhesion ? GroundTerramechanics::adhesion_resistance(sw.soil, width, sw.sinkage) : 0.0f) + sw.suction;
			const float r = resist * CLAMP(v_lng / 0.3f, -1.0f, 1.0f);
			if (r != 0.0f) {
				const PxVec3 dF = lng * -r;
				const PxVec3 arm = v.rigidBodyState.pose.rotate(v.suspensionParams[i].suspensionAttachment.transform(v.suspensionComplianceStates[i].tireForceAppPoint));
				f.forces[PxVehicleTireDirectionModes::eLONGITUDINAL] += dF;
				f.torques[PxVehicleTireDirectionModes::eLONGITUDINAL] += arm.cross(dF);
			}
			// Side bulldozing: a wheel sliding sideways through loose soil shoves a wedge of it with its sidewall
			// (Ishigami et al. 2007, Hegedus' bulldozing on the side face): Bekker's bulldozing on a face one
			// contact length long and the sinkage deep, against the side slip, eased in over 0.3 m/s.
			const float v_lat = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLATERAL];
			// Into a rut's wall the sidewall shoves the wall's whole height, not only what the tyre sank.
			const float wall = v_lat > 0.0f ? sw.guide.z : sw.guide.y;
			const float side = GroundTerramechanics::bulldozing_resistance(sw.soil, ws.contact_length, MAX(sw.sinkage, wall), 0.0f) * CLAMP(v_lat / 0.3f, -1.0f, 1.0f);
			if (side != 0.0f) {
				const PxVec3 lat = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLATERAL];
				const PxVec3 dF = lat * -side;
				const PxVec3 arm = v.rigidBodyState.pose.rotate(v.suspensionParams[i].suspensionAttachment.transform(v.suspensionComplianceStates[i].tireForceAppPoint));
				f.forces[PxVehicleTireDirectionModes::eLATERAL] += dF;
				f.torques[PxVehicleTireDirectionModes::eLATERAL] += arm.cross(dF);
			}
			sw.side_bulldozing = Math::abs(side);
			// Resting across a rut's tilted floor or wall, the ground's reaction tips with it: load x slope down the
			// tilt (rut wandering, as a road's camber), so the wheel slides back into the rut and climbing out of one
			// needs the tyre's own side force to beat it.
			const float wander = -load * CLAMP(sw.guide.x, -0.6f, 0.6f);
			if (wander != 0.0f) {
				const PxVec3 lat_w = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLATERAL];
				const PxVec3 dW = lat_w * wander;
				const PxVec3 arm_w = v.rigidBodyState.pose.rotate(v.suspensionParams[i].suspensionAttachment.transform(v.suspensionComplianceStates[i].tireForceAppPoint));
				f.forces[PxVehicleTireDirectionModes::eLATERAL] += dW;
				f.torques[PxVehicleTireDirectionModes::eLATERAL] += arm_w.cross(dW);
			}
			sw.resistance = Math::abs(r);
			sw.traction = ws.traction * sw.grip;
		}
		return true;
	}
};
#endif

struct PhysXVehicle3D::Impl {
	Vehicle4W vehicle;
	PhysXVehicleSoilWheel soil_wheels[4];
#ifdef MODULE_GROUND_ENABLED
	PhysXVehicleSoilRoad soil_road;
	PhysXVehicleSoilTire soil_tire;
#endif
	PxVehiclePhysXSimulationContext simulationContext;
	PxScene *scene = nullptr;
	PxReal sleep_threshold = 0.0f;
	PxReal time_before_sleep = 0.0f;
	bool built = false;
	// The node transform this node last wrote from the actor: a different one at the next step was set by a script
	// (spawn, respawn, a reset), and the actor is moved there.
	Transform3D synced;
	// wheel_order[Vehicle4W::WHEEL_FL/FR/RL/RR] = index into the parent's own
	// `wheels` vector (child-registration order) for that canonical slot --
	// see configure_vehicle4w()'s own doc comment.
	PxU32 wheel_order[4] = {};
	// The chassis box (its half size and pose in this node's frame), for the belly (stage 3).
	Vector3 chassis_half;
	Transform3D chassis_local;
	float belly_drag = 0.0f;
	float belly_support = 0.0f;
	float belly_deepest = 0.0f; // m under the undisturbed surface, the last step
	float water_buoyancy = 0.0f; // N (round 20.1)
	float cab_water = 0.0f; // m3 inside the cab (round 21.1)
	bool engine_drowned = false;
	bool bow_on = false; // its disturbances are in the water (round 21.3)
	float bow_strength = 0.0f;
	float wake_timer = 0.0f; // since its waves last washed the banks (round 22.1)
	float intake_trough = 0.0f; // the bow's trough at the intake, m (negative: the bow wave over it)
	float intake_dry = 0.0f; // s the intake has been clear since it drowned
	Vector3 water_drag; // N
	// Mud on the body (stage 3b): per wheel the spray level and the shed kg already counted; the belly's mud line,
	// the mud's wetness; the meshes painted (their materials copied with Mud on).
	float mud_spray[4] = {};
	float mud_shed_seen[4] = {};
	float sheet_rate[4] = {}; // m3/s of water each wheel throws as sheets (mud stage 4c): washes its arch
	float sheet_mud[4] = {}; // the clay that water carries, kg/m3 (muddy water, round 3): stains it instead
	float mud_line = 0.0f;
	float mud_wet = 0.0f;
	// Dried mud flaking off on jolts (round 11): the chassis' velocity the last step, for its acceleration.
	Vector3 mud_prev_vel;
	bool mud_prev_ok = false;
	uint32_t flake_seed = 12345u;
	int flakes_shed = 0;
	// The clay's colour on the body, linear (mud stage 4d): the ground's mud colour at the wheels, averaged over the last
	// few metres driven through it; a mid brown until the vehicle has been in any.
	Vector3 mud_rgb = Vector3(0.17f, 0.11f, 0.065f);
	bool mud_written = false;
	bool mud_meshes_ready = false;
	LocalVector<ObjectID> mud_meshes;
	// Clods stuck to the body (stage 4b): a ring of splats (where and how big in this node's frame; flight direction
	// and amount), published as this vehicle's row of the shared splat texture.
	int mud_row = -1;
	Vector4 splat_a[32];
	Vector4 splat_b[32];
	int splat_count = 0;
	int splat_head = 0;
	bool splats_dirty = false;
};

PhysXVehicle3D::PhysXVehicle3D() {
	impl = memnew(Impl);
}

PhysXVehicle3D::~PhysXVehicle3D() {
	_mud_register(false);
	_destroy();
	_control_free();
	memdelete(impl);
}

bool PhysXVehicle3D::_build() {
	if (impl->built) {
		return true;
	}
	// PxVehicleRigidBodyComponent integrates gravity/velocity into the
	// chassis pose itself, inside v.step() -- independent of whether
	// PxScene::simulate() is ever called. In the editor (not Play), nothing
	// steps the scene, but NOTIFICATION_INTERNAL_PHYSICS_PROCESS still fires
	// on this node, so without this guard the car just falls forever right
	// in the viewport the moment it's placed. Same precedent as
	// PhysXDestructible3D: no physics runs in the editor at all, only real
	// gameplay (Engine::is_editor_hint() == false).
	if (Engine::get_singleton()->is_editor_hint()) {
		return false;
	}
	if (!is_inside_world() || get_world_3d().is_null()) {
		return false;
	}
	if (wheels.size() != 4) {
		// Not a real error -- this fires transiently while wheel children are
		// still entering the tree one at a time (see PhysXVehicleWheel3D's
		// own NOTIFICATION_ENTER_TREE), and permanently if the scene author
		// hasn't added exactly 4 yet. get_configuration_warnings() surfaces
		// the permanent case in the Inspector.
		return false;
	}

	CollisionShape3D *chassis_shape_node = nullptr;
	for (int i = 0; i < get_child_count(); i++) {
		chassis_shape_node = Object::cast_to<CollisionShape3D>(get_child(i));
		if (chassis_shape_node) {
			break;
		}
	}
	if (!chassis_shape_node || chassis_shape_node->get_shape().is_null()) {
		return false;
	}
	Ref<BoxShape3D> box_shape = chassis_shape_node->get_shape();
	if (box_shape.is_null()) {
		ERR_PRINT("PhysXVehicle3D: chassis CollisionShape3D must use a BoxShape3D.");
		return false;
	}
	impl->chassis_half = box_shape->get_size() * 0.5f;
	impl->chassis_local = chassis_shape_node->get_transform();

	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	if (!server) {
		return false;
	}
	GodotPhysXSpace3D *space = server->get_space(get_world_3d()->get_space());
	if (!space) {
		// Not running on the PhysX backend -- this node offers PxVehicle2-
		// specific capability and has no fallback for another backend.
		WARN_PRINT_ONCE("PhysXVehicle3D needs the PhysX physics engine (Project Settings > physics/3d/physics_engine = PhysX); it stays still on any other.");
		return false;
	}
	PxPhysics *physics = space->get_px_physics();
	PxScene *scene = space->get_px_scene();
	if (!physics || !scene) {
		return false;
	}

	Vehicle4WConfig cfg;
	cfg.mass = mass;
	cfg.moment_of_inertia = moment_of_inertia;
	cfg.chassis_half_extents = box_shape->get_size() * 0.5;
	cfg.chassis_box_center_local = chassis_shape_node->get_position();
	cfg.chassis_com_local = center_of_mass_mode == CENTER_OF_MASS_MODE_CUSTOM ? center_of_mass : chassis_shape_node->get_position();
	cfg.max_engine_torque = max_engine_torque;
	if (drivetrain == DRIVETRAIN_ENGINE) {
		Vehicle4WEngineConfig &e = cfg.engine;
		_engine_config(e);
		e.limited_slip = differential == DIFFERENTIAL_LIMITED_SLIP;
		e.front_bias = limited_slip_front_bias;
		e.front_target = limited_slip_front_target;
		e.rear_bias = limited_slip_rear_bias;
		e.rear_target = limited_slip_rear_target;
		e.center_bias = limited_slip_center_bias;
		e.center_target = limited_slip_center_target;
		e.rate = limited_slip_rate;
	}
	cfg.max_brake_torque = max_brake_torque;
	cfg.max_steer_angle = max_steer_angle;
	cfg.ackermann_strength = ackermann_strength;
	cfg.front_anti_roll_stiffness = front_anti_roll_stiffness;
	cfg.rear_anti_roll_stiffness = rear_anti_roll_stiffness;
	cfg.tire_force_point = tire_force_point;
	cfg.collision_layer = collision_layer;
	cfg.collision_mask = collision_mask;
	{
		real_t friction, bounce;
		_chassis_material(friction, bounce);
		cfg.chassis_friction = (float)friction;
		cfg.chassis_bounce = (float)bounce;
	}

	for (int i = 0; i < 4; i++) {
		PhysXVehicleWheel3D *w = wheels[i];
		Vehicle4WWheelConfig &wc = cfg.wheels[i];
		// The rest pose, not the node's current (simulated) one -- see get_rest_transform().
		wc.position = w->get_rest_transform().origin;
		wc.basis = w->get_rest_transform().basis;
		wc.radius = w->get_radius();
		wc.half_width = w->get_half_width();
		wc.wheel_mass = w->get_wheel_mass();
		wc.wheel_moment_of_inertia = w->get_wheel_moment_of_inertia();
		wc.damping_rate = w->get_damping_rate();
		wc.suspension_travel = w->get_suspension_travel();
		wc.suspension_stiffness = w->get_suspension_stiffness();
		wc.suspension_damping = w->get_suspension_damping();
		wc.tire_lateral_stiffness = w->get_tire_lateral_stiffness();
		wc.tire_longitudinal_stiffness = w->get_tire_longitudinal_stiffness();
		wc.tire_camber_stiffness = w->get_tire_camber_stiffness();
		wc.tire_friction = w->get_tire_friction();
		wc.tire_rest_grip = w->get_tire_rest_grip();
		wc.tire_slide_grip = w->get_tire_slide_grip();
		wc.use_as_steering = w->is_used_as_steering();
		wc.use_as_traction = w->is_used_as_traction();
	}

	impl->sleep_threshold = (PxReal)space->get_sleep_energy_threshold();
	impl->time_before_sleep = (PxReal)space->get_time_before_sleep();
	const PxTransform pose = to_px(get_global_transform());
	// Made and added where the scene may be touched, and waited for (see GodotPhysXServer3D::run_on_physics_thread()).
#ifdef MODULE_GROUND_ENABLED
	impl->soil_road.vehicle = &impl->vehicle;
	impl->soil_road.wheels = impl->soil_wheels;
	impl->soil_road.context = &impl->simulationContext;
	impl->soil_road.base_slip_denominator = -1.0f;
	impl->soil_road.base_passive_denominator = -1.0f;
	impl->soil_road.base_sticky_speed = -1.0f;
	impl->soil_road.base_lat_sticky_speed = -1.0f;
	impl->soil_tire.vehicle = &impl->vehicle;
	impl->soil_tire.wheels = impl->soil_wheels;
	impl->soil_road.clogging = tyre_clogging;
	impl->soil_road.parked = parked_settling;
	impl->soil_tire.parked = parked_settling;
	impl->vehicle.soil_road_component = soft_soil ? &impl->soil_road : nullptr;
	impl->vehicle.soil_tire_component = soft_soil ? &impl->soil_tire : nullptr;
#endif
	for (PhysXVehicleSoilWheel &sw : impl->soil_wheels) {
		sw = PhysXVehicleSoilWheel();
	}
	impl->synced = get_global_transform();
	GodotPhysXServer3D::run_on_physics_thread([&]() {
		if (!configure_vehicle4w(impl->vehicle, cfg, *physics, *scene, impl->simulationContext, impl->wheel_order)) {
			return;
		}
		Vehicle4W &v = impl->vehicle;
		v.physxActor.rigidBody->setGlobalPose(pose);
		PxRigidDynamic *dynamic_body = v.physxActor.rigidBody->is<PxRigidDynamic>();
		ERR_FAIL_NULL(dynamic_body);
		dynamic_body->setSleepThreshold(can_sleep && GodotPhysXProjectSettings::allow_sleep ? impl->sleep_threshold : 0.0f);
		dynamic_body->setWakeCounter(impl->time_before_sleep);
		scene->addActor(*v.physxActor.rigidBody);
		v.physxActor.rigidBody->setName("PhysXVehicle3D");

		impl->scene = scene;
		impl->built = true;
		_mud_register(true);
		_control_built();
		_cache_chassis();
	},
			true);
	return impl->built;
}

PHYSX_VEHICLE_CONTROL_DEFINE(PhysXVehicle3D, 4)
PHYSX_VEHICLE_ENGINE_DEFINE(PhysXVehicle3D)

void PhysXVehicle3D::_destroy() {
	_mud_register(false);
	if (impl->built) {
		GodotPhysXServer3D::run_on_physics_thread([&]() {
			_control_unbind();
			if (impl->scene) {
				impl->scene->removeActor(*impl->vehicle.physxActor.rigidBody);
			}
			impl->vehicle.destroy();
		},
				true);
		impl->built = false;
		impl->scene = nullptr;
	}
}

void PhysXVehicle3D::_rebuild_if_live() {
	if (!is_inside_world()) {
		// Not in the tree/world yet -- NOTIFICATION_ENTER_WORLD will do the
		// real first build once it is.
		return;
	}
	// Unconditional, not just "if already built" -- a wheel child can finish
	// registering (e.g. the 4th of 4, added after this node's own
	// NOTIFICATION_ENTER_WORLD already ran and failed with too few wheels)
	// and that later success needs a real attempt here, not just a rebuild
	// of something that was never built. Also toggles physics processing
	// itself -- NOTIFICATION_ENTER_WORLD isn't the only path that can
	// transition built false->true (a wheel registering after is exactly
	// that), so it can't be the only place this gets turned on.
	_destroy();
	set_physics_process_internal(_build());
}

void PhysXVehicle3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			if (wind_obstacle) {
				add_to_group(SNAME("weather_shelter"));
			}
			set_physics_process_internal(_build());
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			set_physics_process_internal(false);
			_destroy();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!impl->built) {
				break;
			}
			Vehicle4W &v = impl->vehicle;
			v.commandState.throttle = (PxReal)throttle;
			v.commandState.brakes[0] = (PxReal)brake;
			// The second brake response is the rear wheels' only (configure_vehicle4w).
			v.commandState.brakes[1] = (PxReal)handbrake;
			v.commandState.nbBrakes = 2;
			v.commandState.steer = (PxReal)steer;
			v.transmissionCommandState.gear = reverse ? PxVehicleDirectDriveTransmissionCommandState::eREVERSE : PxVehicleDirectDriveTransmissionCommandState::eFORWARD;
			_engine_controls();
			if (impl->engine_drowned) {
				v.commandState.throttle = 0.0f; // stalled: water in the intake
			}
			for (uint32_t i = 0; i < 4; i++) {
				impl->soil_wheels[i].inflation = (float)wheels[impl->wheel_order[i]]->get_tire_pressure();
				impl->soil_wheels[i].tread = (int)wheels[impl->wheel_order[i]]->get_tread();
			}
			if (!get_global_transform().is_equal_approx(impl->synced)) {
				// Moved by a script since the last step: the actor goes there at rest.
				v.physxActor.rigidBody->setGlobalPose(to_px(get_global_transform()));
				if (PxRigidDynamic *dyn = v.physxActor.rigidBody->is<PxRigidDynamic>()) {
					dyn->setLinearVelocity(PxVec3(0.0f));
					dyn->setAngularVelocity(PxVec3(0.0f));
					dyn->wakeUp();
				}
			}
			// Engine.time_scale 0 gives a zero step: the vehicle holds still (PhysX rejects a step that is not positive).
			if (get_physics_process_delta_time() > 0.0) {
				v.step((PxReal)get_physics_process_delta_time(), impl->simulationContext);
				_belly_step((float)get_physics_process_delta_time());
				_water_step((float)get_physics_process_delta_time());
			}
			// v.rigidBodyState.pose is CoM-relative, not the actor's real
			// origin (confirmed directly by comparing it against a raw
			// getGlobalPose() read while root-causing the chassis-collision
			// regression) -- the CollisionShape3D/wheel/mesh children are all
			// positioned relative to the actor's own origin (that's the frame
			// PxVehiclePhysXActorCreate cooked the real PxShape geometry
			// into), so this node's own transform has to track that same
			// origin, not the CoM. Using rigidBodyState.pose here left the
			// visual body a constant offset away from where the real
			// collision geometry actually sits at rest, and made that offset
			// visibly swing whenever the body rotated (braking, cornering, a
			// bump) since it rotates with the body.
			set_global_transform(to_godot(v.physxActor.rigidBody->getGlobalPose()));
			impl->synced = get_global_transform();
			// Push each wheel's live pose (suspension jounce + steer angle +
			// roll spin, all baked into wheelLocalPoses by PxVehicleWheelComponent)
			// onto that wheel's own node -- same idea as VehicleBody3D's own
			// _update_wheel_transform(): the wheel NODE moves, and any
			// MeshInstance3D the scene author parented under it inherits that
			// motion automatically, no separate mesh-transform wiring needed.
			//
			// wheelLocalPoses[i].localPose is in the CoM frame, NOT the actor's
			// own origin frame (confirmed directly in PhysX's own write-back
			// code, VhPhysXActorFunctions.cpp: "Local pose in actor frame" is
			// computed as cmassLocalPose * wheelLocalPose, not wheelLocalPose
			// alone) -- this node's own transform above is the actor's real
			// origin, so composing wheelLocalPose directly onto it left every
			// wheel a constant offset away (equal to the CoM offset, ~0.35m by
			// default) from where it should be, visible as wheels sunk deep
			// into the ground regardless of wheel radius (a real regression
			// caught by an A/B on radius that showed the SAME absolute
			// penetration depth for two different radii -- ruling out anything
			// radius-proportional and pointing straight at a fixed frame
			// offset instead).
			const PxTransform cmass_local_pose = v.physxActor.rigidBody->getCMassLocalPose();
			for (uint32_t i = 0; i < 4; i++) {
				wheels[impl->wheel_order[i]]->_set_simulated_transform(to_godot(cmass_local_pose * v.wheelLocalPoses[i].localPose));
			}
			_cache_chassis();
			if (get_physics_process_delta_time() > 0.0) {
				_body_mud_step((float)get_physics_process_delta_time());
				_wade_step();
			}
		} break;
	}
}

Vector3 PhysXVehicle3D::get_linear_velocity() const {
	// The chassis actor's own velocity, not the vehicle state's copy (that one is
	// refreshed only when the vehicle steps, so a velocity just set or an impulse
	// just applied would read back a step late).
	return _control_body() != nullptr ? to_godot(_chassis_linear_velocity()) : Vector3();
}

Vector3 PhysXVehicle3D::get_forward() const {
	if (!impl->built) {
		return get_global_transform().basis.get_column(2).normalized();
	}
	const PxTransform actor_pose = _chassis_pose();
	return to_godot(actor_pose.q.getBasisVector2());
}

real_t PhysXVehicle3D::get_forward_speed() const {
	if (!impl->built) {
		return 0.0;
	}
	// frame.getLngAxis() is a fixed LOCAL-frame constant (e.g. (0,0,-1)),
	// not a world-space direction -- rotate it into world space by the
	// actor's current orientation before dotting against the world-space
	// velocity, or this is only correct when yaw matches spawn orientation
	// (found via a real bug report: correct at first, sign-flips as the
	// vehicle yaws further away from its start heading).
	const PxTransform actor_pose = _chassis_pose();
	const PxVec3 fwd = actor_pose.q.rotate(impl->vehicle.frame.getLngAxis());
	return (real_t)impl->vehicle.rigidBodyState.linearVelocity.dot(fwd);
}

real_t PhysXVehicle3D::get_wheel_jounce(int p_wheel) const {
	if (!impl->built) {
		return 0.0;
	}
	ERR_FAIL_INDEX_V((uint32_t)p_wheel, wheels.size(), 0.0);
	for (uint32_t i = 0; i < 4; i++) {
		if (impl->wheel_order[i] == (uint32_t)p_wheel) {
			return (real_t)impl->vehicle.suspensionStates[i].jounce;
		}
	}
	return 0.0;
}

void PhysXVehicle3D::set_soft_soil(bool p_enabled) {
	if (soft_soil == p_enabled) {
		return;
	}
	soft_soil = p_enabled;
	_rebuild_if_live();
}

void PhysXVehicle3D::set_tyre_clogging(bool p_enabled) {
	tyre_clogging = p_enabled;
	impl->soil_road.clogging = p_enabled;
}

void PhysXVehicle3D::set_parked_settling(bool p_enabled) {
	parked_settling = p_enabled;
	impl->soil_road.parked = p_enabled;
	impl->soil_tire.parked = p_enabled;
}

real_t PhysXVehicle3D::get_wheel_clods_thrown(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].bite_thrown;
}

real_t PhysXVehicle3D::get_wheel_creep(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].creep;
}

real_t PhysXVehicle3D::get_wheel_suction(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].suction;
}

void PhysXVehicle3D::set_belly_drag(bool p_enabled) {
	belly_drag = p_enabled;
}

real_t PhysXVehicle3D::get_wheel_mud(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].mud;
}

real_t PhysXVehicle3D::get_wheel_mud_shed(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].shed;
}

real_t PhysXVehicle3D::get_belly_drag() const {
	return impl->belly_drag;
}

real_t PhysXVehicle3D::get_belly_support() const {
	return impl->belly_support;
}

void PhysXVehicle3D::set_river_forces(bool p_enabled) {
	river_forces = p_enabled;
}

void PhysXVehicle3D::set_hull_seal(real_t p_share) {
	hull_seal = CLAMP(p_share, 0.0f, 1.0f);
}

real_t PhysXVehicle3D::get_water_buoyancy() const {
	return impl->water_buoyancy;
}

Vector3 PhysXVehicle3D::get_water_drag() const {
	return impl->water_drag;
}

void PhysXVehicle3D::set_hull_leak_area(real_t p_area) {
	hull_leak_area = MAX(p_area, 0.0f);
}

void PhysXVehicle3D::set_air_intake_height(real_t p_height) {
	air_intake_height = p_height;
}

void PhysXVehicle3D::set_snorkel(bool p_enabled) {
	snorkel = p_enabled;
}

bool PhysXVehicle3D::is_engine_drowned() const {
	return impl->engine_drowned;
}

real_t PhysXVehicle3D::get_cab_water() const {
	return impl->cab_water;
}

real_t PhysXVehicle3D::get_bow_wave_strength() const {
	return impl->bow_strength;
}

real_t PhysXVehicle3D::get_intake_trough() const {
	return impl->intake_trough;
}

// A vehicle in water (docs/ideas/mud.md, round 20.1). Flood studies of vehicles (Shu et al. 2011; Xia et al. 2014;
// Martinez-Gomariz et al. 2017, full-size cars at UNSW's Water Research Laboratory, 2017) take it as a body on its tyres:
// buoyancy from the water it keeps out lightens it, the current's drag 0.5 rho Cd A |u - v| (u - v) (Cd about 1.1 on the
// wet side or front) pushes it, and it is carried off once the drag beats the tyres' friction on what weight is left;
// cars float near 0.5 to 0.6 m of still water as their body keeps out about half its box (hull_seal). PhysX's tyres
// give the friction from the lighter load. The chassis box in 3 x 5 columns, each buoyed by its wet part and dragged by
// the current at its own speed through the box's wet face across the flow; each tyre by its wet segment.
void PhysXVehicle3D::_water_step(float p_delta) {
	impl->water_buoyancy = 0.0f;
	impl->water_drag = Vector3();
	PxRigidDynamic *dyn = impl->vehicle.physxActor.rigidBody ? impl->vehicle.physxActor.rigidBody->is<PxRigidDynamic>() : nullptr;
	WorldWater *ww = WorldContracts::water();
	if (!river_forces || !dyn || !ww || !ww->has_provider() || impl->chassis_half == Vector3()) {
		impl->engine_drowned = false;
		impl->cab_water = 0.0f;
		if (impl->bow_on && ww) {
			for (int k = 0; k < 3; k++) {
				ww->clear((uint64_t(get_instance_id()) << 2) + k);
			}
		}
		impl->bow_on = false;
		return;
	}
	for (int i = 0; i < get_child_count(); i++) {
		if (get_child(i)->is_class("PhysXBuoyancy3D")) {
			return; // floated by its own buoyancy node
		}
	}
	const float rho = 1000.0f;
	const float cd = 1.1f;
	const Transform3D box = get_global_transform() * impl->chassis_local;
	const Vector3 h = impl->chassis_half;
	const PxTransform com_pose = dyn->getGlobalPose() * dyn->getCMassLocalPose();
	const Vector3 com(com_pose.p.x, com_pose.p.y, com_pose.p.z);
	const PxVec3 lin_px = dyn->getLinearVelocity();
	const PxVec3 ang_px = dyn->getAngularVelocity();
	const Vector3 lin(lin_px.x, lin_px.y, lin_px.z);
	const Vector3 ang(ang_px.x, ang_px.y, ang_px.z);
	const Vector3 ax = box.basis.get_column(0).normalized();
	const Vector3 az = box.basis.get_column(2).normalized();
	const Vector3 up = box.basis.get_column(1);
	const float tall = 2.0f * h.y * MAX(up.normalized().y, 0.1f); // the box's height on the vertical
	const int NX = 3;
	const int NZ = 5;
	const float col_area = 4.0f * h.x * h.z / float(NX * NZ);
	Vector3 force;
	Vector3 torque;
	float lift = 0.0f;
	auto push = [&](const Vector3 &p_at, const Vector3 &p_f) {
		force += p_f;
		torque += (p_at - com).cross(p_f);
	};
	for (int iz = 0; iz < NZ; iz++) {
		for (int ix = 0; ix < NX; ix++) {
			const Vector3 local((float(ix) + 0.5f) / NX * 2.0f * h.x - h.x, -h.y, (float(iz) + 0.5f) / NZ * 2.0f * h.z - h.z);
			const Vector3 base = box.xform(local);
			const float ws = ww->height_at(base + Vector3(0.0f, tall, 0.0f));
			const float wet = CLAMP(ws - base.y, 0.0f, tall);
			if (!(wet > 0.0f)) {
				continue;
			}
			const Vector3 mid = base + Vector3(0.0f, 0.5f * wet, 0.0f);
			const float b = rho * 9.81f * col_area * wet * float(hull_seal);
			lift += b;
			Vector3 f(0.0f, b, 0.0f);
			const Vector3 u = ww->flow_at(mid);
			Vector3 rel = u - (lin + ang.cross(mid - com));
			rel.y = 0.0f;
			const float s = rel.length();
			if (s > 1e-3f) {
				const Vector3 d = rel / s;
				// The box's width across the flow, shared by its columns.
				const float across = 2.0f * h.x * Math::abs(d.dot(az)) + 2.0f * h.z * Math::abs(d.dot(ax));
				f += rel * (0.5f * rho * cd * wet * across / float(NX * NZ) * s);
			}
			push(mid, f);
		}
	}
	for (const PhysXVehicleWheel3D *w : wheels) {
		if (!w || !w->is_inside_tree()) {
			continue;
		}
		const Vector3 c = w->get_global_position();
		const float r = (float)w->get_radius();
		const float width = 2.0f * (float)w->get_half_width();
		const float ws = ww->height_at(c + Vector3(0.0f, r, 0.0f));
		const float wet = CLAMP(ws - (c.y - r), 0.0f, 2.0f * r);
		if (!(wet > 0.0f)) {
			continue;
		}
		// The circle's wet segment: area r^2 (t - sin t cos t), t = acos(1 - wet / r).
		const float th = Math::acos(CLAMP(1.0f - wet / r, -1.0f, 1.0f));
		const float seg = r * r * (th - Math::sin(th) * Math::cos(th));
		const float b = rho * 9.81f * seg * width;
		lift += b;
		const Vector3 at(c.x, c.y - r + 0.5f * wet, c.z);
		Vector3 f(0.0f, b, 0.0f);
		Vector3 rel = ww->flow_at(at) - (lin + ang.cross(at - com));
		rel.y = 0.0f;
		const float s = rel.length();
		if (s > 1e-3f) {
			const Vector3 d = rel / s;
			const float across = width * Math::abs(d.dot(az)) + 2.0f * r * Math::abs(d.dot(ax));
			f += rel * (0.5f * rho * cd * wet / (2.0f * r) * across * s);
		}
		push(at, f);
	}
	// The bow wave and wake (round 21.3): the hull pushes the water at three points along its keel, moving
	// disturbances the water's own wave solver turns into a bow wave and a wake (a moving pressure source makes the
	// Kelvin wake; in shallow water its depth Froude number Fr = v / sqrt(g h) sets it: near 1 the bow wave stands
	// up and breaks; wading drill keeps to a walking pace, about 2 m/s, so the bow keeps the water down at the
	// engine bay). The strength is the share of the full push: a resting hull's 0.15, rising to 1 at Fr 0.8,
	// times how much of the hull is wet.
	{
		const uint64_t owner = uint64_t(get_instance_id()) << 2;
		const Vector3 v_h(lin.x, 0.0f, lin.z);
		float ground_y = box.origin.y - h.y;
		for (const PhysXVehicleWheel3D *w : wheels) {
			if (w && w->is_inside_tree()) {
				ground_y = MIN(ground_y, float(w->get_global_position().y - w->get_radius()));
			}
		}
		bool any = false;
		float strongest = 0.0f;
		for (int k = 0; k < 3; k++) {
			const Vector3 keel = box.xform(Vector3(0.0f, -h.y, (float(k) - 1.0f) * 0.66f * h.z));
			const float ws = ww->height_at(keel + Vector3(0.0f, tall, 0.0f));
			const float wet = Math::is_finite(ws) ? ws - keel.y : 0.0f;
			if (!(wet > 0.02f)) {
				ww->clear(owner + k);
				continue;
			}
			const float depth = MAX(ws - ground_y, 0.05f);
			const float fr = v_h.length() / Math::sqrt(9.81f * depth);
			const float s = CLAMP(0.15f + 0.85f * fr / 0.8f, 0.15f, 1.0f) * MIN(wet / 0.3f, 1.0f);
			ww->disturb(owner + k, Vector3(keel.x, ws, keel.z), MAX(h.x, 0.33f * h.z), s);
			strongest = MAX(strongest, s);
			any = true;
		}
		impl->bow_on = any;
		impl->bow_strength = strongest;
		// Its waves wash the banks (round 22.1): the ground takes them from the hull's speed and width, ten times a
		// second.
		impl->wake_timer += p_delta;
		if (any && impl->wake_timer >= 0.1f) {
			GroundSurface3D *ground = GroundSurface3D::get_active();
			const Vector3 mid = box.xform(Vector3(0.0f, -h.y, 0.0f));
			const float level = ww->height_at(mid + Vector3(0.0f, tall, 0.0f));
			if (ground && Math::is_finite(level)) {
				ground->wash_wake(mid, v_h, h.x, level, impl->wake_timer);
			}
			impl->wake_timer = 0.0f;
		} else if (!any) {
			impl->wake_timer = 0.0f;
		}
	}
	// The engine's air intake (round 21.1): hydrolock past it. Wading depths are rated from it (makers' figures,
	// 0.5 to 0.9 m for off-roaders); a snorkel lifts it over the roof. Water drawn in stalls the engine; it starts
	// again once the intake has been clear for 5 s (no lasting damage kept).
	{
		const float bottom_to_top = 2.0f * h.y;
		const float hi = snorkel ? bottom_to_top + 0.25f : (air_intake_height >= 0.0f ? float(air_intake_height) : 0.75f * bottom_to_top);
		const Vector3 intake = box.xform(Vector3(0.0f, -h.y + hi, -0.7f * h.z));
		// The bow's trough (round 22.1): driven forward at a steady wading pace the water drawn round the bow stands
		// lower at the engine bay by about the stagnation head v^2 / 2g (Bernoulli along the sides, the flow sped up
		// round the bluff front); past depth Froude 0.7 the bow wave stands up and by 1 it climbs over the bonnet,
		// as high again. The wading drill: steady, about walking pace.
		float level = ww->height_at(intake);
		if (Math::is_finite(level) && impl->bow_on) {
			const float vf = Vector3(lin.x, 0.0f, lin.z).length(); // forward or backing: the leading end's bow
			const float ground_y = box.origin.y - h.y - 0.5f;
			const float fr = vf / Math::sqrt(9.81f * MAX(level - ground_y, 0.05f));
			const float head = MIN(vf * vf / (2.0f * 9.81f), 0.4f);
			impl->intake_trough = head * (1.0f - 2.0f * Math::smoothstep(0.7f, 1.0f, fr));
			level -= impl->intake_trough;
		} else {
			impl->intake_trough = 0.0f;
		}
		const bool under = level > intake.y;
		if (under) {
			impl->engine_drowned = true;
			impl->intake_dry = 0.0f;
		} else if (impl->engine_drowned) {
			impl->intake_dry += p_delta;
			if (impl->intake_dry > 5.0f) {
				impl->engine_drowned = false;
			}
		}
	}
	// The cab floods through its gaps (doors, floor, vents) at an orifice's rate Q = Cd A sqrt(2 g dh), Cd 0.6, the
	// head between the water outside at the box's middle and the level inside; out again the same way on land.
	// What is in it is held in the sealed share of the box and adds its weight at the box's middle.
	{
		const float sealed = float(hull_seal) * 8.0f * h.x * h.y * h.z;
		const float floor_area = MAX(float(hull_seal) * 4.0f * h.x * h.z, 1e-3f);
		const Vector3 mid = box.xform(Vector3(0.0f, -h.y, 0.0f));
		const float outside = ww->height_at(mid + Vector3(0.0f, tall, 0.0f));
		const float level_in = mid.y + impl->cab_water / floor_area;
		const float out_level = Math::is_finite(outside) ? MAX(outside, mid.y) : mid.y;
		const float dh = out_level - level_in;
		if (sealed > 0.0f && hull_leak_area > 0.0f && Math::abs(dh) > 1e-4f) {
			const float q = 0.6f * float(hull_leak_area) * Math::sqrt(2.0f * 9.81f * Math::abs(dh));
			impl->cab_water = CLAMP(impl->cab_water + SIGN(dh) * q * p_delta, 0.0f, sealed);
		}
		if (impl->cab_water > 0.0f) {
			const Vector3 at = mid + Vector3(0.0f, 0.5f * impl->cab_water / floor_area, 0.0f);
			push(at, Vector3(0.0f, -1000.0f * 9.81f * impl->cab_water, 0.0f));
		}
	}
	if (force == Vector3()) {
		return;
	}
	impl->water_buoyancy = lift;
	impl->water_drag = Vector3(force.x, 0.0f, force.z);
	const PxVec3 pf(force.x, force.y, force.z);
	const PxVec3 pt(torque.x, torque.y, torque.z);
	GodotPhysXServer3D::run_on_physics_thread([=]() {
		dyn->addForce(pf, PxForceMode::eFORCE);
		dyn->addTorque(pt, PxForceMode::eFORCE);
	});
	(void)p_delta;
}

// The belly in the mud (docs/ideas/mud.md, stage 3). Off-road games and NRMM count "belly drag" once the sinkage
// passes the ground clearance; here the chassis box's underside, sampled 3 across by 5 along, meets the ground's
// deformed surface. Where a sample is under it by z: Bekker's plate pressure k z^n (k = kc / b + kphi, b the
// belly's width) holds it up, which takes load off the wheels (high-centring); Mohr-Coulomb shear on smooth steel
// (A ca + N tan delta, delta 0.6 phi) drags against its slide; the leading row bulldozes (Bekker) what is not yet
// pressed; and the belly presses a trough into the field. Depths from the undisturbed surface (the trough is mud
// packed under the belly, still bearing). Damped near critical so the stiff contact does not ring.
void PhysXVehicle3D::_belly_step(float p_delta) {
	impl->belly_drag = 0.0f;
	impl->belly_support = 0.0f;
	impl->belly_deepest = 0.0f;
#ifdef MODULE_GROUND_ENABLED
	GroundSurface3D *ground = GroundSurface3D::get_active();
	PxRigidDynamic *dyn = impl->vehicle.physxActor.rigidBody ? impl->vehicle.physxActor.rigidBody->is<PxRigidDynamic>() : nullptr;
	if (!belly_drag || !soft_soil || !ground || !dyn || impl->chassis_half == Vector3()) {
		return;
	}
	const Transform3D box = get_global_transform() * impl->chassis_local;
	const Vector3 h = impl->chassis_half;
	const PxTransform com_pose = dyn->getGlobalPose() * dyn->getCMassLocalPose();
	const Vector3 com(com_pose.p.x, com_pose.p.y, com_pose.p.z);
	const PxVec3 lin_px = dyn->getLinearVelocity();
	const PxVec3 ang_px = dyn->getAngularVelocity();
	const Vector3 lin(lin_px.x, lin_px.y, lin_px.z);
	const Vector3 ang(ang_px.x, ang_px.y, ang_px.z);
	const Vector3 fwd = -box.basis.get_column(2).normalized();
	const int NX = 3;
	const int NZ = 5;
	const float wx = 2.0f * h.x * 0.85f;
	const float wz = 2.0f * h.z * 0.85f;
	const float area = wx * wz / float(NX * NZ);
	const float m_share = (float)mass / float(NX * NZ);
	Vector3 force;
	Vector3 torque;
	float deepest = 0.0f;
	Vector3 slide_sum;
	for (int iz = 0; iz < NZ; iz++) {
		for (int ix = 0; ix < NX; ix++) {
			const Vector3 local((float(ix) + 0.5f) / NX * wx - 0.5f * wx, -h.y, (float(iz) + 0.5f) / NZ * wz - 0.5f * wz);
			const Vector3 p = box.xform(local);
			// Under the undisturbed surface: the trough the belly presses is mud packed under it, still bearing. Above
			// it, the berms and the centre ridge the wheels shoved up between their ruts (mud round 10): soil the belly
			// rides up on and ploughs (high-centring, the "hang-up" failure of mobility models: Rula and Nuttall
			// 1971; Wong 2008 on ground clearance), so deep old ruts leave a ridge that lifts the car off its wheels.
			const float z = ground->get_ground_height_at(p) + MAX(ground->get_rim_at(p) - ground->get_dent_at(p), 0.0f) - p.y;
			if (z <= 0.0f) {
				continue;
			}
			const GroundSoil soil = ground->get_soil(p);
			if (!soil.soft) {
				continue;
			}
			const float zc = MIN(z, soil.max_sinkage + 0.05f);
			const float k = soil.kc / MAX(wx, 0.1f) + soil.kphi; // kN/m^(n+2)
			const float pressure = MIN(k * Math::pow(zc, soil.n), 300.0f); // kPa
			const Vector3 v = lin + ang.cross(p - com);
			float n_force = 1000.0f * pressure * area;
			const float k_eff = soil.n * n_force / MAX(zc, 1e-3f);
			n_force = MAX(n_force - 2.0f * 0.8f * Math::sqrt(k_eff * m_share) * v.y, 0.0f);
			Vector3 vh(v.x, 0.0f, v.z);
			const float speed = vh.length();
			Vector3 f(0.0f, n_force, 0.0f);
			if (speed > 1e-3f) {
				const Vector3 dir = vh / speed;
				float shear = 1000.0f * area * soil.adhesion + n_force * 0.6f * soil.tan_phi;
				// The leading row's edge pushes a wedge of the mud (its width, the depth under the surface).
				const float lead = dir.dot(fwd);
				if ((iz == 0 && lead < -0.3f) || (iz == NZ - 1 && lead > 0.3f)) {
					const float already = MAX(ground->get_dent_at(p) - ground->get_rim_at(p), 0.0f);
					shear += GroundTerramechanics::bulldozing_resistance(soil, wx / NX, zc, MIN(already, zc));
				}
				// Eased in over the first 0.3 m/s, so a belly at rest is not pushed about.
				f -= dir * shear * MIN(speed / 0.3f, 1.0f);
				impl->belly_drag += shear * MIN(speed / 0.3f, 1.0f);
				slide_sum += vh;
			}
			impl->belly_support += n_force;
			force += f;
			torque += (p - com).cross(f);
			deepest = MAX(deepest, z);
		}
	}
	if (force == Vector3()) {
		return;
	}
	const PxVec3 pf(force.x, force.y, force.z);
	const PxVec3 pt(torque.x, torque.y, torque.z);
	GodotPhysXServer3D::run_on_physics_thread([=]() {
		dyn->addForce(pf, PxForceMode::eFORCE);
		dyn->addTorque(pt, PxForceMode::eFORCE);
	});
	impl->belly_deepest = deepest;
	// The trough: the belly's footprint pressed to its depth under the surface there.
	const Vector3 c = box.xform(Vector3(0.0f, -h.y, 0.0f));
	const float sink = deepest;
	const Vector3 side = box.basis.get_column(0).normalized();
	const Vector3 slide = slide_sum / float(NX * NZ);
	ground->press_wheel(c, Vector2(wx, wz), Math::atan2(-side.z, side.x), sink, Vector2(slide.x, slide.z));
	(void)p_delta;
#endif
}

// Mud on the body (docs/ideas/mud.md, stage 3b). Off-road games keep a mud level per region of the vehicle and let it
// threshold a splat mask (Saber's per-vertex truck mud, the dirt masks of rally games); BaseMaterial3D's Mud layer
// does that in the vehicle's own frame, from the instance uniforms written here (BaseMaterial3D::get_mud_functions_code()):
// each tyre's tread fill (stage 3a), the spray each wheel has thrown up its arch (its shed clay, and the slurry it
// ploughs), the belly's mud line (the deepest the chassis sat under the surface). Wet when it lands, drying over
// minutes; standing water washes it.
static constexpr float MUD_SPRAY_KG = 12.0f; // clay thrown off a wheel to spray its arch fully
static constexpr float MUD_DRY_TIME = 300.0f; // s from fresh to dry

// Stuck clods (mud stage 4b): every vehicle's splats in one RGBA float texture, a row a vehicle, two texels a clod,
// published as the global shader parameter vehicle_mud_splats (BaseMaterial3D Mud reads it; the path tracer too).
static constexpr int MUD_SPLATS = 32;
static constexpr int MUD_SPLAT_ROWS = 16;
static LocalVector<PhysXVehicle3D *> mud_vehicles;
static PhysXVehicle3D *mud_rows[MUD_SPLAT_ROWS] = {};
static Vector<uint8_t> mud_splat_data;
static RID mud_splat_texture;

void PhysXVehicle3D::_mud_register(bool p_on) {
	if (p_on) {
		if (mud_vehicles.find(this) >= 0) {
			return;
		}
		mud_vehicles.push_back(this);
		for (int r = 0; r < MUD_SPLAT_ROWS; r++) {
			if (!mud_rows[r]) {
				mud_rows[r] = this;
				impl->mud_row = r;
				break;
			}
		}
		return;
	}
	const int i = mud_vehicles.find(this);
	if (i < 0) {
		return;
	}
	mud_vehicles.remove_at(i);
	if (impl->mud_row >= 0) {
		mud_rows[impl->mud_row] = nullptr;
		impl->mud_row = -1;
	}
	if (mud_vehicles.is_empty() && mud_splat_texture.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("vehicle_mud_splats", Variant());
		RenderingServer::get_singleton()->free_rid(mud_splat_texture);
		mud_splat_texture = RID();
	}
}

void PhysXVehicle3D::get_mud_vehicles(LocalVector<PhysXVehicle3D *> &r_vehicles) {
	r_vehicles = mud_vehicles;
}

Array PhysXVehicle3D::get_wind_obstacles(const Rect2 &p_rect) const {
	Array out;
	if (!wind_obstacle || !is_inside_tree() || impl->chassis_half == Vector3()) {
		return out;
	}
	const AABB box = (get_global_transform() * impl->chassis_local).xform(AABB(-impl->chassis_half, impl->chassis_half * 2.0f));
	if (p_rect.intersects(Rect2(box.position.x, box.position.z, box.size.x, box.size.z))) {
		out.push_back(box);
	}
	return out;
}

void PhysXVehicle3D::set_wind_obstacle(bool p_enabled) {
	wind_obstacle = p_enabled;
	if (p_enabled) {
		add_to_group(SNAME("weather_shelter"));
	} else if (is_in_group(SNAME("weather_shelter"))) {
		remove_from_group(SNAME("weather_shelter"));
	}
}

bool PhysXVehicle3D::mud_box_contains(const Vector3 &p_world) const {
	if (!body_mud || impl->chassis_half == Vector3()) {
		return false;
	}
	const Vector3 l = (get_global_transform() * impl->chassis_local).affine_inverse().xform(p_world);
	const Vector3 h = impl->chassis_half + Vector3(0.02f, 0.02f, 0.02f);
	return Math::abs(l.x) < h.x && Math::abs(l.y) < h.y && Math::abs(l.z) < h.z;
}

void PhysXVehicle3D::add_mud_splat(const Vector3 &p_world, const Vector3 &p_velocity, float p_volume) {
	if (!body_mud || p_volume <= 0.0f) {
		return;
	}
	Transform3D frame = get_global_transform();
	frame.basis.orthonormalize();
	const Transform3D inv = frame.affine_inverse();
	const Vector3 at = inv.xform(p_world);
	Vector3 dir = inv.basis.xform(p_velocity);
	dir = dir.length() > 1e-3f ? dir.normalized() : Vector3(0, -1, 0);
	// A clod flattens into a splat a few millimetres thick.
	float r = Math::sqrt(p_volume / (Math::PI * 0.005f));
	// A clod landing on a splat thickens and widens it rather than taking a slot of its own.
	for (int j = 0; j < impl->splat_count; j++) {
		Vector4 &a = impl->splat_a[j];
		const float rj = a.w;
		if (Vector3(a.x, a.y, a.z).distance_to(at) < 0.5f * (rj + r)) {
			a.w = MIN(Math::sqrt(rj * rj + r * r), 0.15f);
			impl->splat_b[j].w = 1.0f;
			impl->splats_dirty = true;
			impl->mud_wet = 1.0f;
			return;
		}
	}
	r = CLAMP(r, 0.01f, 0.15f);
	const int k = impl->splat_head;
	impl->splat_a[k] = Vector4(at.x, at.y, at.z, r);
	impl->splat_b[k] = Vector4(dir.x, dir.y, dir.z, 1.0f);
	impl->splat_head = (k + 1) % MUD_SPLATS;
	impl->splat_count = MIN(impl->splat_count + 1, MUD_SPLATS);
	impl->splats_dirty = true;
	impl->mud_wet = 1.0f;
}

Vector4 PhysXVehicle3D::get_mud_splat(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, impl->splat_count, Vector4());
	const Vector4 &a = impl->splat_a[p_index];
	return Vector4(a.x, a.y, a.z, impl->splat_b[p_index].w);
}

int PhysXVehicle3D::get_mud_flakes_shed() const {
	return impl->flakes_shed;
}

int PhysXVehicle3D::get_mud_splat_count() const {
	return impl->splat_count;
}

static void _mud_splats_upload(int p_row, const Vector4 *p_a, const Vector4 *p_b, int p_count) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || p_row < 0) {
		return;
	}
	const int w = 2 * MUD_SPLATS;
	const int row_bytes = w * 4 * sizeof(float);
	if (mud_splat_data.size() != row_bytes * MUD_SPLAT_ROWS) {
		mud_splat_data.resize(row_bytes * MUD_SPLAT_ROWS);
		memset(mud_splat_data.ptrw(), 0, mud_splat_data.size());
	}
	float *f = (float *)(mud_splat_data.ptrw() + p_row * row_bytes);
	memset(f, 0, row_bytes);
	for (int k = 0; k < p_count; k++) {
		const Vector4 a = p_a[k];
		const Vector4 b = p_b[k];
		f[8 * k + 0] = a.x;
		f[8 * k + 1] = a.y;
		f[8 * k + 2] = a.z;
		f[8 * k + 3] = a.w;
		f[8 * k + 4] = b.x;
		f[8 * k + 5] = b.y;
		f[8 * k + 6] = b.z;
		f[8 * k + 7] = b.w;
	}
	Ref<Image> img = Image::create_from_data(w, MUD_SPLAT_ROWS, false, Image::FORMAT_RGBAF, mud_splat_data);
	if (mud_splat_texture.is_valid()) {
		rs->texture_2d_update(mud_splat_texture, img, 0);
		return;
	}
	mud_splat_texture = rs->texture_2d_create(img);
	static bool added = false;
	if (!added && !ProjectSettings::get_singleton()->has_setting("shader_globals/vehicle_mud_splats")) {
		rs->global_shader_parameter_add("vehicle_mud_splats", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, mud_splat_texture);
	} else {
		rs->global_shader_parameter_set("vehicle_mud_splats", mud_splat_texture);
	}
	added = true;
}

void PhysXVehicle3D::_body_mud_meshes() {
	impl->mud_meshes.clear();
	List<Node *> stack;
	stack.push_back(this);
	while (!stack.is_empty()) {
		Node *n = stack.front()->get();
		stack.pop_front();
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
		if (!mi || mi->get_mesh().is_null()) {
			continue;
		}
		bool any = false;
		Ref<BaseMaterial3D> over = mi->get_material_override();
		if (over.is_valid()) {
			if (!over->get_feature(BaseMaterial3D::FEATURE_MUD)) {
				over = over->duplicate();
				over->set_feature(BaseMaterial3D::FEATURE_MUD, true);
				mi->set_material_override(over);
			}
			any = true;
		} else if (mi->get_material_override().is_null()) {
			for (int s = 0; s < mi->get_surface_override_material_count(); s++) {
				Ref<BaseMaterial3D> m = mi->get_active_material(s);
				if (m.is_null()) {
					continue;
				}
				if (!m->get_feature(BaseMaterial3D::FEATURE_MUD)) {
					m = m->duplicate();
					m->set_feature(BaseMaterial3D::FEATURE_MUD, true);
					mi->set_surface_override_material(s, m);
				}
				any = true;
			}
		}
		if (any) {
			impl->mud_meshes.push_back(mi->get_instance_id());
		}
	}
	impl->mud_meshes_ready = true;
}

// Wheels in standing water (mud stage 4c): each tells the ground it is wading, and the water passes push the water
// ahead of it.
void PhysXVehicle3D::_wade_step() {
#ifdef MODULE_GROUND_ENABLED
	GroundSurface3D *ground = GroundSurface3D::get_active();
	if (!ground || !impl->built) {
		return;
	}
	const Vehicle4W &v = impl->vehicle;
	const double dt = get_physics_process_delta_time();
	const Vector3 up = get_global_basis().get_column(1).normalized();
	const Vector3 vel = get_linear_velocity();
	for (uint32_t i = 0; i < 4; i++) {
		const Vector3 c = wheels[impl->wheel_order[i]]->get_global_position();
		const float depth = ground->get_standing_water_at(c);
		if (depth <= 0.005f) {
			continue;
		}
		const float r = v.wheelParams[i].radius;
		const float hw = v.wheelParams[i].halfWidth;
		ground->add_wader(int64_t(get_instance_id()) * 4 + i, c, r, hw, vel);
		// Past the water's wave speed (Froude number over 1) the water the tyre sweeps cannot run ahead of it as a
		// bow wave: it is thrown out as side waves, two sheets out and up from the contact at 20 to 40 degrees
		// (tyre spray studies: Weir, Strange and Wheeler 1978; Pilkington 1990). The share thrown grows with the
		// Froude number, a third at 2 and most of it by 4.
		const Vector3 flat = Vector3(vel.x, 0.0f, vel.z);
		const float speed = flat.length();
		const float h = MIN(depth, r);
		const float fr = speed / Math::sqrt(9.81f * h);
		if (fr <= 1.0f || speed < 0.5f) {
			continue;
		}
		const float share = 0.8f * (1.0f - 1.0f / fr) * (1.0f - 0.5f / fr);
		const Vector3 fwd = flat / speed;
		const Vector3 side = up.cross(fwd).normalized();
		// A wheel running in another's track (a rear wheel behind its front one) meets the water that one has
		// already thrown: only the leading wheel of a track throws.
		bool trailing = false;
		for (uint32_t j = 0; j < 4 && !trailing; j++) {
			if (j == i) {
				continue;
			}
			const Vector3 o = wheels[impl->wheel_order[j]]->get_global_position() - c;
			trailing = o.dot(fwd) > 0.0f && Math::abs(o.dot(side)) < 2.0f * hw;
		}
		if (trailing) {
			continue;
		}
		// Taken from the bow wave just ahead of the tread (the tyre has already pushed the water out from under
		// itself); never more than half the water it sweeps into this step, spread over a disc a few steps
		// long, so the overlapping discs of the next steps (and a read back a few frames old) still find it there.
		const Vector3 contact = Vector3(c.x, c.y - r, c.z);
		const float reach = MAX(hw, 0.05f);
		const Vector3 ahead = contact + fwd * (0.5f * r);
		// The disc's water as last read back, of which this step's advance sweeps speed dt over its 2 reach.
		const double there = ground->get_standing_water_volume(ahead, reach);
		const double swept = double(2.0f * hw * h * speed * share) * dt;
		const double held = 0.5 * there * MIN(double(speed) * dt / (2.0 * reach), 1.0);
		const double water = MIN(swept, held);
		if (water <= 0.0) {
			continue;
		}
		impl->sheet_rate[i] = float(water / MAX(dt, 1e-4));
		impl->sheet_mud[i] = ground->get_water_mud_at(ahead);
		for (int s = -1; s <= 1; s += 2) {
			// Out at about the speed, rising at ~30 degrees, carried a little forward by the tread's push.
			GroundSurface3D::Spray sheet;
			sheet.position = contact + side * (float(s) * hw) + up * h;
			sheet.velocity = (side * float(s) * 0.75f + up * 0.45f + fwd * 0.35f) * speed;
			sheet.width = MAX(hw, 0.04f);
			sheet.reach_center = ahead;
			sheet.reach_radius = reach;
			sheet.water = float(0.5 * water);
			sheet.water_mud = ground->get_water_mud_at(ahead);
			ground->add_spray(sheet);
		}
	}
#endif
}

void PhysXVehicle3D::_body_mud_step(float p_delta) {
	if (!body_mud) {
		return;
	}
	if (!impl->mud_meshes_ready) {
		_body_mud_meshes();
	}
	bool fresh = false;
	float water_body = 0.0f;
	float water_line = -1e9f; // world height of the water the wheels stand in
#ifdef MODULE_GROUND_ENABLED
	GroundSurface3D *ground = GroundSurface3D::get_active();
#endif
	const Vehicle4W &v = impl->vehicle;
	for (uint32_t i = 0; i < 4; i++) {
		const PhysXVehicleSoilWheel &sw = impl->soil_wheels[i];
		float &spray = impl->mud_spray[i];
		const float before = spray;
		const float thrown = MAX(sw.shed - impl->mud_shed_seen[i], 0.0f);
		impl->mud_shed_seen[i] = sw.shed;
		spray += thrown / MUD_SPRAY_KG;
#ifdef MODULE_GROUND_ENABLED
		float flung = thrown;
		if (thrown > 0.0f && ground && !(sw.soft && sw.soil.stickiness >= 0.2f)) {
			// Out of the mud (round 3): the clay the tread drops on firm ground is pressed onto it under the tyre,
			// the more of it the slower the tread turns (a tyre leaves most of its mud within its first tens of
			// metres: construction tracking pads are sized at about 15 m). The rest still flies.
			const PhysXVehicleWheel3D *wn = wheels[impl->wheel_order[i]];
			const float r = v.wheelParams[i].radius;
			const float hw = v.wheelParams[i].halfWidth;
			const float tread = Math::abs(v.wheelRigidBody1dStates[i].rotationSpeed) * r;
			const float pressed = thrown * CLAMP(1.0f - tread / 8.0f, 0.2f, 1.0f);
			const Vector3 up = get_global_basis().get_column(1).normalized();
			const Vector3 contact = wn->get_global_position() - up * r;
			// Through the tread's lugs, on the wheel's heading (mud round 4).
			const Vector3 head = wn->get_global_basis().get_column(2);
			const float yaw = Math::atan2(-head.z, head.x);
			ground->track_mud(contact, hw, pressed / 1700.0f / ((float)Math::PI * hw * hw), 1.0f, Vector4(1.0f, yaw, 0.0f, 0.0f));
			flung = thrown - pressed;
			spray -= pressed / MUD_SPRAY_KG; // pressed down, not thrown up into the arch
		}
		// Clods cut by the tread (round 23.3). A grousered wheel slipping in cohesive soil shears it in blocks
		// between its grousers, as deep as they stand and as long as the slip between one grouser and the next (Wong
		// 2008, sec. 2.6; Bekker's grouser action); chains' cross links act as tall, sharp grousers. Spinning past
		// the speed at which the clay lets go of rubber (shedding_speed), the tread carries the cut blocks round and
		// flings them as clods; slower, they drop off just behind the wheel. Here a quarter of the soil swept by the
		// grouser depth over the slip,
		// width x depth x |slip| x tread speed, the rest pressed into the rut's rims as before. Dug from behind the
		// contact, thrown with the tread's clay; only in cohesive, sticky soil (a liquid mud's cuts slump back in).
		{
			PhysXVehicleSoilWheel &bw = impl->soil_wheels[i];
			const PhysXVehicleWheel3D::TreadSoil ts = PhysXVehicleWheel3D::tread_soil((PhysXVehicleWheel3D::Tread)sw.tread);
			const float bite = CLAMP((ts.grip - 1.0f) / 0.2f, 0.0f, 1.0f);
			const float r = v.wheelParams[i].radius;
			const float tread_v = Math::abs(v.wheelRigidBody1dStates[i].rotationSpeed) * r;
			const bool flings = tread_v > GroundTerramechanics::shedding_speed(sw.soil, r) * ts.shed;
			if (sw.soft && bite > 0.0f && sw.soil.stickiness >= 0.2f) {
				const float stiff = Math::smoothstep(0.2f, 0.5f, sw.soil.stickiness); // cohesive clay (stickiness peaks at plastic wetness)
				bw.bite_owed += 0.25f * 2.0f * v.wheelParams[i].halfWidth * ts.depth * Math::abs(sw.slip) * tread_v * bite * stiff * float(p_delta);
			}
			if (bw.bite_owed >= 2e-4f && ground) {
				const PhysXVehicleWheel3D *wn = wheels[impl->wheel_order[i]];
				const float hw = v.wheelParams[i].halfWidth;
				const float tv = v.wheelRigidBody1dStates[i].rotationSpeed * r;
				const Vector3 up = get_global_basis().get_column(1).normalized();
				const Vector3 fwd = get_global_basis().get_column(2).normalized() * (tv >= 0.0f ? 1.0f : -1.0f);
				const Vector3 back = wn->get_global_position() - up * r - fwd * (0.3f * r);
				const float cut = ground->dig(back, hw, bw.bite_owed / ((float)Math::PI * hw * hw));
				if (cut > 0.0f) {
					if (flings) {
						flung += cut * 1700.0f;
					} else {
						// Too slow to throw: the blocks drop off the tread just behind the wheel as clods.
						const Vector3 drop = back - fwd * (0.5f * r);
						ground->track_mud(Vector3(drop.x, ground->get_surface_height_at(drop), drop.z), hw, cut / ((float)Math::PI * hw * hw), 1.0f);
					}
					bw.bite_thrown += cut * 1700.0f;
				}
				bw.bite_owed = 0.0f;
			}
		}
		if (flung > 0.0f && ground) {
			// The shed clay leaves the rear of the tread, up and back at the tread's speed (mud stage 4): the
			// rooster tail of a wheel spun clean. A grains node throws it as mud clods.
			const PhysXVehicleWheel3D *wn = wheels[impl->wheel_order[i]];
			const float r = v.wheelParams[i].radius;
			const float tread = v.wheelRigidBody1dStates[i].rotationSpeed * r;
			const Vector3 up = get_global_basis().get_column(1).normalized();
			const Vector3 fwd = get_global_basis().get_column(2).normalized() * (tread >= 0.0f ? 1.0f : -1.0f);
			GroundSurface3D::Spray fling;
			fling.position = wn->get_global_position() - fwd * (0.9f * r) + up * (0.25f * r);
			fling.velocity = get_linear_velocity() + (up * 0.85f - fwd * 0.5f).normalized() * Math::abs(tread);
			fling.width = 2.0f * v.wheelParams[i].halfWidth;
			fling.stickiness = 1.0f;
			fling.volume = flung / 1700.0f;
			ground->add_spray(fling);
		}
#endif
#ifdef MODULE_GROUND_ENABLED
		if (sw.soft && sw.soil.stickiness > 0.0f) {
			// Ploughing through sticky slurry splashes the arch too, more as the tread turns faster.
			const float tread = Math::abs(v.wheelRigidBody1dStates[i].rotationSpeed) * v.wheelParams[i].radius;
			spray += 0.05f * sw.soil.stickiness * MIN(tread / 5.0f, 1.0f) * MIN(sw.sinkage / (0.3f * v.wheelParams[i].radius), 1.0f) * p_delta;
		}
		if (ground && sw.soft && sw.mud > 0.0f) {
			// The tread picks up the clay it turns in: blend its colour in over about 5 m of travel.
			const float moved = Math::abs(v.wheelRigidBody1dStates[i].rotationSpeed) * v.wheelParams[i].radius * p_delta;
			const Color clay = ground->get_mud_color_at(wheels[impl->wheel_order[i]]->get_global_position()).srgb_to_linear();
			impl->mud_rgb = impl->mud_rgb.lerp(Vector3(clay.r, clay.g, clay.b), MIN(moved / 5.0f, 1.0f));
		}
		if (ground) {
			const Vector3 c = wheels[impl->wheel_order[i]]->get_global_position();
			// The water the wheel stands in: the tyre pushes it out from under itself, so around it too.
			const Vector3 ax = get_global_basis().get_column(0).normalized() * (2.0f * v.wheelParams[i].halfWidth);
			const Vector3 az = get_global_basis().get_column(2).normalized() * (0.7f * v.wheelParams[i].radius);
			const Vector3 around[5] = { c, c + az, c - az, c + ax, c - ax };
			float water = 0.0f;
			for (const Vector3 &q : around) {
				const float w = ground->get_standing_water_at(q);
				if (w > 0.01f) {
					water = MAX(water, w);
					water_line = MAX(water_line, ground->get_surface_height_at(q) + w);
				}
			}
			if (water > 0.01f) {
				spray -= spray * MIN(MIN(water / 0.1f, 1.0f) * 0.3f * p_delta, 1.0f);
				water_body = MAX(water_body, water);
			}
			// Its own sheets wash the arch they hit: a litre a second rinses most of it in a few seconds.
			if (impl->sheet_rate[i] > 0.0f) {
				// Muddy water (round 3) leaves its clay on what it hits as it runs off: the arch is stained toward a
				// film that grows with the clay (half the spray's darkest at some 60 kg/m3) rather than rinsed.
				const float stain = 0.5f * MIN(impl->sheet_mud[i] / 60.0f, 1.0f);
				const float k = MIN(MIN(impl->sheet_rate[i] / 0.001f, 1.0f) * 0.4f * p_delta, 1.0f);
				spray += (stain - spray) * k;
				impl->sheet_rate[i] = 0.0f;
			}
		}
#endif
		spray = CLAMP(spray, 0.0f, 1.0f);
		// A packed tread keeps the body wet only while it turns in the clay (round 11: parked, it dries).
		fresh = fresh || spray > before + 1e-5f || (sw.mud > 0.05f && sw.soft && Math::abs(v.wheelRigidBody1dStates[i].rotationSpeed) * v.wheelParams[i].radius > 0.1f);
	}
	Transform3D frame = get_global_transform();
	frame.basis.orthonormalize();
	const Transform3D inv = frame.affine_inverse();
	const float bottom = (impl->chassis_local.xform(Vector3(0.0f, -impl->chassis_half.y, 0.0f))).y;
	// The mud line: the deepest the body has sat in mud, from the belly under the surface or the mud's surface at a
	// wheel sunk past the chassis bottom (a car sunk to its axles in slurry is muddy to the sills or above).
	float line = impl->belly_deepest > 0.0f ? impl->belly_deepest : 0.0f;
	for (uint32_t i = 0; i < 4; i++) {
		const PhysXVehicleSoilWheel &sw = impl->soil_wheels[i];
		if (sw.soft && sw.sinkage > 0.0f) {
			const Vector3 c = inv.xform(wheels[impl->wheel_order[i]]->get_global_position());
			line = MAX(line, c.y - v.wheelParams[i].radius + sw.sinkage - bottom);
		}
	}
	if (line > 0.0f && line + 0.05f > impl->mud_line) {
		impl->mud_line = line + 0.05f;
		fresh = true;
	}
	// Drying (round 11): by the weather, as the ground's water evaporates (Penman 1948's wind function on the vapour
	// pressure deficit), plus the sun on the panels and the air the vehicle drives through; MUD_DRY_TIME is the time
	// at 20 C, half humidity, 2 m/s of wind and half sun. Rain wets it again and slowly rinses it.
	float dry_rate = 1.0f;
	float rain = 0.0f;
	// The weather's own clock (round 23.2): a truck parked overnight in world time dries overnight, however fast the
	// world's hours pass.
	float world_dt = float(p_delta);
	const Vector3 vel = get_linear_velocity();
	if (const WorldWeather *wm = WorldContracts::weather(); wm && wm->has_provider()) {
		world_dt *= MAX(float(wm->time_scale()), 0.0f);
		const Vector3 at = frame.origin;
		const float t = wm->air_temperature_at(at);
		const float es = 6.112f * Math::exp(17.62f * t / (243.12f + t));
		const float deficit = MAX(es * (1.0f - CLAMP(wm->air_humidity_at(at), 0.0f, 1.0f)), 0.0f);
		const float u = MIN((wm->wind_at(at) - vel).length(), 30.0f);
		const float sun = CLAMP(wm->sun_visibility_at(at), 0.0f, 1.0f);
		// The reference: 0.26 (1 + 0.54 x 2) x 11.7 hPa, times (1 + 0.5) for half sun.
		dry_rate = 0.26f * (1.0f + 0.54f * u) * deficit * (1.0f + sun) / (0.26f * 2.08f * 11.7f * 1.5f);
		rain = MAX(wm->precipitation_at(at), 0.0f);
	}
	if (fresh || water_body > 0.02f) {
		impl->mud_wet = 1.0f;
	} else if (rain > 0.05f) {
		// A shower soaks it again in a couple of minutes at 2 mm/h, faster harder.
		impl->mud_wet = MIN(impl->mud_wet + world_dt * rain / 2.0f / 120.0f, 1.0f);
	} else {
		impl->mud_wet = MAX(impl->mud_wet - world_dt * dry_rate / MUD_DRY_TIME, 0.0f);
	}
	if (rain > 0.05f) {
		// Rain rinses the clay off over an hour or so at 10 mm/h.
		const float k = MIN(world_dt * rain / 10.0f / 1000.0f, 1.0f);
		impl->mud_line -= impl->mud_line * k;
		for (uint32_t i = 0; i < 4; i++) {
			impl->mud_spray[i] -= impl->mud_spray[i] * k;
		}
		for (int k2 = 0; k2 < impl->splat_count; k2++) {
			impl->splat_b[k2].w -= impl->splat_b[k2].w * k;
		}
		impl->splats_dirty = impl->splats_dirty || impl->splat_count > 0;
	}
	// Flaking (round 11): dried clay has shrunk and cracked off the paint and holds by little more than friction; a jolt
	// past about 0.8 g (a pothole, a kerb, a rut's edge) shakes plates off, more the drier and the harder the jolt. Each
	// stuck clod goes whole, landing under where it was as a clod on the ground; the spray and the belly's coat thin.
	if (impl->mud_prev_ok && p_delta > 0.0) {
		const float jolt = (vel - impl->mud_prev_vel).length() / float(p_delta) / 9.81f;
		const float dryness = Math::smoothstep(0.35f, 0.05f, impl->mud_wet);
		// Round 23.2, the next drive after it has dried: the crust cracks into curling plates as it shrinks (a drying
		// clay film cracks at a spacing of a few times its thickness and its plates curl up at the edges: Kindle 1926;
		// Allain and Limat 1995), and a curled plate is shed two more ways than by a single hard jolt:
		// - road vibration under the 0.8 g jolt wears the plates' hold away by fatigue, Basquin's power law of the
		//   load (exponent 4, as brittle bonds), so a rough track sheds them over a few minutes of driving;
		// - the air past the body lifts curled plates once its dynamic pressure 0.5 rho v^2 passes about three times
		//   the plate's own weight per area (a 4 mm clay plate, 1700 kg/m3: about 200 Pa, some 18 m/s).
		float shake = MAX(jolt - 0.8f, 0.0f) * dryness;
		if (dryness > 0.0f) {
			const float vib = MIN(jolt / 0.8f, 1.0f);
			shake += 0.05f * vib * vib * vib * vib * dryness;
			const float q = 0.5f * 1.2f * vel.length_squared();
			const float q_c = 3.0f * 1700.0f * 9.81f * 0.004f;
			shake += 0.2f * CLAMP(q / q_c - 1.0f, 0.0f, 1.0f) * dryness;
		}
		if (shake > 0.0f) {
			const float rate = MIN(0.6f * shake * float(p_delta), 1.0f);
			impl->mud_line -= impl->mud_line * 0.3f * rate;
			for (uint32_t i = 0; i < 4; i++) {
				impl->mud_spray[i] -= impl->mud_spray[i] * 0.3f * rate;
			}
			for (int k = impl->splat_count - 1; k >= 0; k--) {
				impl->flake_seed = impl->flake_seed * 1664525u + 1013904223u;
				if (float(impl->flake_seed >> 8) / 16777216.0f >= rate) {
					continue;
				}
#ifdef MODULE_GROUND_ENABLED
				if (ground) {
					const Vector3 wp = frame.xform(Vector3(impl->splat_a[k].x, impl->splat_a[k].y, impl->splat_a[k].z));
					const Vector3 below(wp.x, ground->get_surface_height_at(wp), wp.z);
					const float r = impl->splat_a[k].w;
					ground->track_mud(below, r, 0.004f * CLAMP(impl->splat_b[k].w, 0.0f, 1.25f), impl->mud_wet);
				}
#endif
				impl->splat_count--;
				impl->splat_a[k] = impl->splat_a[impl->splat_count];
				impl->splat_b[k] = impl->splat_b[impl->splat_count];
				impl->splats_dirty = true;
				impl->flakes_shed++;
			}
			impl->splat_head = impl->splat_count % MUD_SPLATS;
		}
	}
	impl->mud_prev_vel = vel;
	impl->mud_prev_ok = true;
	if (water_body > 0.01f) {
		// Water washes the body below its line (and a little above it, the bow wave riding up the panels), faster as
		// the water runs past: the shear on the clay grows with the flow (soil erosion's excess shear, as the
		// tyre tread's wash above). Stuck clods under it go, and the belly's mud line drops to it.
		const float speed = get_linear_velocity().length();
		const float rate = MIN(water_body / 0.03f, 1.0f) * 0.3f * (1.0f + 0.5f * speed);
		const float wl = inv.xform(Vector3(frame.origin.x, water_line, frame.origin.z)).y + 0.03f + 0.02f * speed;
		for (int k = 0; k < impl->splat_count; k++) {
			// Above the line only what splashes up: less the higher it sits.
			const float under = impl->splat_a[k].y < wl ? 1.0f : 0.3f * Math::exp(-(impl->splat_a[k].y - wl) / 0.1f);
			impl->splat_b[k].w -= impl->splat_b[k].w * MIN(under * rate * p_delta, 1.0f);
		}
		if (impl->splat_count > 0) {
			impl->splats_dirty = true;
		}
		if (impl->mud_line > 0.0f && wl - bottom >= impl->mud_line) {
			impl->mud_line = MAX(impl->mud_line - impl->mud_line * MIN(rate * p_delta, 1.0f), 0.0f);
		}
	}

	float total = impl->mud_line;
	for (uint32_t i = 0; i < 4; i++) {
		total += impl->mud_spray[i] + impl->soil_wheels[i].mud;
	}
	total += impl->splat_count;
	if (impl->splats_dirty) {
		_mud_splats_upload(impl->mud_row, impl->splat_a, impl->splat_b, impl->splat_count);
		impl->splats_dirty = false;
	}
	if (total <= 0.0f && !impl->mud_written) {
		return;
	}
	impl->mud_written = total > 0.0f;

	Vector4 rows[3];
	for (int k = 0; k < 3; k++) {
		rows[k] = Vector4(inv.basis.rows[k].x, inv.basis.rows[k].y, inv.basis.rows[k].z, inv.origin[k]);
	}
	Vector4 wheel[4];
	Vector4 fill;
	Vector4 spray;
	for (uint32_t i = 0; i < 4; i++) {
		const Vector3 c = inv.xform(wheels[impl->wheel_order[i]]->get_global_position());
		wheel[i] = Vector4(c.x, c.y, c.z, v.wheelParams[i].radius);
		fill[i] = impl->soil_wheels[i].mud;
		spray[i] = impl->mud_spray[i];
	}
	const Vector4 body(bottom, impl->mud_line, impl->mud_wet, 0.0f);
	const Vector4 color(impl->mud_rgb.x, impl->mud_rgb.y, impl->mud_rgb.z, 1.0f);
	const Vector4 splat(float(MAX(impl->mud_row, 0)), impl->mud_row >= 0 ? float(impl->splat_count) : 0.0f, 0.0f, 0.0f);
	static const StringName names[12] = { "mud_frame0", "mud_frame1", "mud_frame2", "mud_wheel0", "mud_wheel1", "mud_wheel2",
		"mud_wheel3", "mud_fill", "mud_spray", "mud_body", "mud_color", "mud_splat" };
	const Vector4 values[12] = { rows[0], rows[1], rows[2], wheel[0], wheel[1], wheel[2], wheel[3], fill, spray, body, color, splat };
	for (const ObjectID &id : impl->mud_meshes) {
		GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(id);
		if (!gi) {
			continue;
		}
		for (int k = 0; k < 12; k++) {
			gi->set_instance_shader_parameter(names[k], values[k]);
		}
	}
}

void PhysXVehicle3D::set_body_mud(bool p_enabled) {
	body_mud = p_enabled;
	if (!body_mud) {
		for (const ObjectID &id : impl->mud_meshes) {
			if (GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(id)) {
				gi->set_instance_shader_parameter("mud_fill", Vector4());
				gi->set_instance_shader_parameter("mud_spray", Vector4());
				gi->set_instance_shader_parameter("mud_body", Vector4());
			}
		}
		for (uint32_t i = 0; i < 4; i++) {
			impl->mud_spray[i] = 0.0f;
		}
		impl->mud_line = 0.0f;
		impl->mud_wet = 0.0f;
		impl->mud_written = false;
		impl->splat_count = 0;
		impl->splat_head = 0;
		if (mud_splat_texture.is_valid()) {
			_mud_splats_upload(impl->mud_row, impl->splat_a, impl->splat_b, 0);
		}
	}
}

real_t PhysXVehicle3D::get_wheel_mud_spray(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->mud_spray[i];
}

real_t PhysXVehicle3D::get_body_mud_line() const {
	return impl->mud_line;
}

Color PhysXVehicle3D::get_body_mud_color() const {
	return Color(impl->mud_rgb.x, impl->mud_rgb.y, impl->mud_rgb.z).linear_to_srgb();
}

real_t PhysXVehicle3D::get_body_mud_wetness() const {
	return impl->mud_wet;
}

int PhysXVehicle3D::_soil_slot(int p_wheel) const {
	if (!impl->built || (uint32_t)p_wheel >= wheels.size()) {
		return -1;
	}
	for (uint32_t i = 0; i < 4; i++) {
		if (impl->wheel_order[i] == (uint32_t)p_wheel) {
			return (int)i;
		}
	}
	return -1;
}

real_t PhysXVehicle3D::get_wheel_sinkage(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 || !impl->soil_wheels[i].soft ? 0.0 : (real_t)impl->soil_wheels[i].sinkage;
}

real_t PhysXVehicle3D::get_wheel_deflection(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].deflect;
}

real_t PhysXVehicle3D::get_wheel_soil_resistance(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 || !impl->soil_wheels[i].soft ? 0.0 : (real_t)impl->soil_wheels[i].resistance;
}
real_t PhysXVehicle3D::get_wheel_soil_side_force(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 || !impl->soil_wheels[i].soft ? 0.0 : (real_t)impl->soil_wheels[i].side_bulldozing;
}


real_t PhysXVehicle3D::get_wheel_soil_traction(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 || !impl->soil_wheels[i].soft ? 0.0 : (real_t)impl->soil_wheels[i].traction;
}

real_t PhysXVehicle3D::get_wheel_slip(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 ? 0.0 : (real_t)impl->soil_wheels[i].slip;
}

bool PhysXVehicle3D::is_wheel_on_soft_soil(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i >= 0 && impl->soil_wheels[i].soft;
}

real_t PhysXVehicle3D::get_wheel_separation(int p_wheel) const {
	if (!impl->built) {
		return 0.0;
	}
	ERR_FAIL_INDEX_V((uint32_t)p_wheel, wheels.size(), 0.0);
	for (uint32_t i = 0; i < 4; i++) {
		if (impl->wheel_order[i] == (uint32_t)p_wheel) {
			return (real_t)impl->vehicle.suspensionStates[i].separation;
		}
	}
	return 0.0;
}

physx::PxRigidActor *PhysXVehicle3D::get_px_actor() const {
	return impl->built ? impl->vehicle.physxActor.rigidBody : nullptr;
}

Vector3 PhysXVehicle3D::get_actor_position() const {
	if (!impl->built) {
		return Vector3();
	}
	return to_godot(_chassis_pose().p);
}

PackedStringArray PhysXVehicle3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	bool has_box_shape = false;
	for (int i = 0; i < get_child_count(); i++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(get_child(i));
		if (cs && cs->get_shape().is_valid() && Object::cast_to<BoxShape3D>(cs->get_shape().ptr())) {
			has_box_shape = true;
			break;
		}
	}
	if (!has_box_shape) {
		warnings.push_back(RTR("PhysXVehicle3D needs a CollisionShape3D child with a BoxShape3D for its chassis."));
	}
	int nb_steering = 0;
	for (uint32_t i = 0; i < wheels.size(); i++) {
		if (wheels[i]->is_used_as_steering()) {
			nb_steering++;
		}
	}
	if (wheels.size() != 4) {
		warnings.push_back(vformat(RTR("PhysXVehicle3D needs exactly 4 PhysXVehicleWheel3D children (has %d)."), (int)wheels.size()));
	} else if (nb_steering != 2) {
		warnings.push_back(vformat(RTR("PhysXVehicle3D needs exactly 2 wheels with use_as_steering enabled (has %d)."), nb_steering));
	}
	return warnings;
}

#define PHYSX_VEHICLE_SETTER(m_name, m_field) \
	void PhysXVehicle3D::set_##m_name(real_t p_v) { \
		m_field = p_v; \
		_rebuild_if_live(); \
	}

void PhysXVehicle3D::set_mass(real_t p_mass) {
	mass = p_mass;
	// Wheel gizmos estimate rest ride height from this vehicle's own mass
	// (sprung_mass = mass * 0.25, matching configure_vehicle4w()) -- refresh
	// them so the editor preview stays accurate as mass is tuned.
	for (PhysXVehicleWheel3D *w : wheels) {
		w->update_gizmos();
	}
	_rebuild_if_live();
}
void PhysXVehicle3D::set_moment_of_inertia(const Vector3 &p_moi) {
	moment_of_inertia = p_moi;
	_rebuild_if_live();
}
void PhysXVehicle3D::set_center_of_mass_mode(CenterOfMassMode p_mode) {
	if (center_of_mass_mode == p_mode) {
		return;
	}
	center_of_mass_mode = p_mode;
	notify_property_list_changed();
	_rebuild_if_live();
}
void PhysXVehicle3D::set_center_of_mass(const Vector3 &p_center_of_mass) {
	if (center_of_mass == p_center_of_mass) {
		return;
	}
	ERR_FAIL_COND(center_of_mass_mode != CENTER_OF_MASS_MODE_CUSTOM);
	center_of_mass = p_center_of_mass;
	_rebuild_if_live();
}
void PhysXVehicle3D::set_can_sleep(bool p_can_sleep) {
	if (can_sleep == p_can_sleep) {
		return;
	}
	can_sleep = p_can_sleep;
	if (impl->built) {
		PxRigidDynamic *body = impl->vehicle.physxActor.rigidBody->is<PxRigidDynamic>();
		ERR_FAIL_NULL(body);
		const PxReal threshold = can_sleep && GodotPhysXProjectSettings::allow_sleep ? impl->sleep_threshold : 0.0f;
		const PxReal wake_counter = impl->time_before_sleep;
		GodotPhysXServer3D::run_on_physics_thread([=]() {
			body->setSleepThreshold(threshold);
			body->setWakeCounter(wake_counter);
			if (!p_can_sleep) {
				body->wakeUp();
			}
		});
	}
}

bool PhysXVehicle3D::is_sleeping() const {
	return impl->built && _chassis_sleeping();
}
PHYSX_VEHICLE_SETTER(max_engine_torque, max_engine_torque)
PHYSX_VEHICLE_SETTER(limited_slip_front_bias, limited_slip_front_bias)
PHYSX_VEHICLE_SETTER(limited_slip_front_target, limited_slip_front_target)
PHYSX_VEHICLE_SETTER(limited_slip_rear_bias, limited_slip_rear_bias)
PHYSX_VEHICLE_SETTER(limited_slip_rear_target, limited_slip_rear_target)
PHYSX_VEHICLE_SETTER(limited_slip_center_bias, limited_slip_center_bias)
PHYSX_VEHICLE_SETTER(limited_slip_center_target, limited_slip_center_target)
PHYSX_VEHICLE_SETTER(limited_slip_rate, limited_slip_rate)
PHYSX_VEHICLE_SETTER(max_brake_torque, max_brake_torque)
PHYSX_VEHICLE_SETTER(max_steer_angle, max_steer_angle)
PHYSX_VEHICLE_SETTER(ackermann_strength, ackermann_strength)
PHYSX_VEHICLE_SETTER(front_anti_roll_stiffness, front_anti_roll_stiffness)
PHYSX_VEHICLE_SETTER(rear_anti_roll_stiffness, rear_anti_roll_stiffness)
PHYSX_VEHICLE_SETTER(tire_force_point, tire_force_point)

#undef PHYSX_VEHICLE_SETTER

void PhysXVehicle3D::set_differential(Differential p_differential) {
	if (differential == p_differential) {
		return;
	}
	differential = p_differential;
	notify_property_list_changed();
	_rebuild_if_live();
}

void PhysXVehicle3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	_rebuild_if_live();
}
void PhysXVehicle3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	_rebuild_if_live();
}

void PhysXVehicle3D::set_physics_material_override(const Ref<PhysicsMaterial> &p_material) {
	if (physics_material_override == p_material) {
		return;
	}
	if (physics_material_override.is_valid()) {
		physics_material_override->disconnect_changed(callable_mp(this, &PhysXVehicle3D::_chassis_material_changed));
	}
	physics_material_override = p_material;
	if (physics_material_override.is_valid()) {
		physics_material_override->connect_changed(callable_mp(this, &PhysXVehicle3D::_chassis_material_changed));
	}
	_chassis_material_changed();
}

void PhysXVehicle3D::_chassis_material(real_t &r_friction, real_t &r_bounce) const {
	if (physics_material_override.is_valid()) {
		r_friction = physics_material_override->computed_friction();
		r_bounce = physics_material_override->computed_bounce();
	} else {
		r_friction = 0.5;
		r_bounce = 0.1;
	}
}

// The chassis material is the vehicle's own (see configure_*()), so it is edited in place -- no rebuild.
void PhysXVehicle3D::_chassis_material_changed() {
	if (!impl->built) {
		return; // the next build reads it
	}
	real_t friction, bounce;
	_chassis_material(friction, bounce);
	PxRigidActor *actor = impl->vehicle.physxActor.rigidBody;
	GodotPhysXServer3D::run_on_physics_thread([=]() {
		godot_physx_apply_material_to_simulation_shapes(actor, friction, bounce);
		// A material change doesn't wake the actor: one resting on its roof would keep sleeping on the old grip.
		PxRigidDynamic *body = actor ? actor->is<PxRigidDynamic>() : nullptr;
		if (body && body->getScene()) {
			body->wakeUp();
		}
	});
}

void PhysXVehicle3D::_validate_property(PropertyInfo &p_property) const {
	if (center_of_mass_mode != CENTER_OF_MASS_MODE_CUSTOM && p_property.name == "center_of_mass") {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	_engine_validate_property(p_property);
	// The differential is the engine drive's, and limited_slip_* only for the limited slip one.
	const String name = p_property.name;
	if ((drivetrain != DRIVETRAIN_ENGINE && name == "differential") ||
			((drivetrain != DRIVETRAIN_ENGINE || differential != DIFFERENTIAL_LIMITED_SLIP) && name.begins_with("limited_slip_"))) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

float PhysXVehicle3D::get_firm_grip_at(const Vector3 &p_position, float p_speed, int p_tread) {
	return _firm_grip(p_position, p_speed, 0.0f, Vector2(), p_tread);
}

void PhysXVehicle3D::_bind_methods() {
	ClassDB::bind_static_method("PhysXVehicle3D", D_METHOD("get_firm_grip_at", "position", "speed", "tread"), &PhysXVehicle3D::get_firm_grip_at);
	BIND_ENUM_CONSTANT(CENTER_OF_MASS_MODE_AUTO);
	BIND_ENUM_CONSTANT(CENTER_OF_MASS_MODE_CUSTOM);

	ClassDB::bind_method(D_METHOD("set_mass", "mass"), &PhysXVehicle3D::set_mass);
	ClassDB::bind_method(D_METHOD("get_mass"), &PhysXVehicle3D::get_mass);
	ClassDB::bind_method(D_METHOD("set_moment_of_inertia", "moi"), &PhysXVehicle3D::set_moment_of_inertia);
	ClassDB::bind_method(D_METHOD("get_moment_of_inertia"), &PhysXVehicle3D::get_moment_of_inertia);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "1,10000,1,or_greater"), "set_mass", "get_mass");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "moment_of_inertia"), "set_moment_of_inertia", "get_moment_of_inertia");
	ClassDB::bind_method(D_METHOD("set_center_of_mass_mode", "mode"), &PhysXVehicle3D::set_center_of_mass_mode);
	ClassDB::bind_method(D_METHOD("get_center_of_mass_mode"), &PhysXVehicle3D::get_center_of_mass_mode);
	ClassDB::bind_method(D_METHOD("set_center_of_mass", "center_of_mass"), &PhysXVehicle3D::set_center_of_mass);
	ClassDB::bind_method(D_METHOD("get_center_of_mass"), &PhysXVehicle3D::get_center_of_mass);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "center_of_mass_mode", PROPERTY_HINT_ENUM, "Auto,Custom"), "set_center_of_mass_mode", "get_center_of_mass_mode");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "center_of_mass", PROPERTY_HINT_RANGE, "-10,10,0.01,or_less,or_greater,suffix:m"), "set_center_of_mass", "get_center_of_mass");
	ClassDB::bind_method(D_METHOD("set_can_sleep", "able_to_sleep"), &PhysXVehicle3D::set_can_sleep);
	ClassDB::bind_method(D_METHOD("is_able_to_sleep"), &PhysXVehicle3D::is_able_to_sleep);
	ClassDB::bind_method(D_METHOD("is_sleeping"), &PhysXVehicle3D::is_sleeping);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "can_sleep"), "set_can_sleep", "is_able_to_sleep");

	BIND_ENUM_CONSTANT(DIFFERENTIAL_OPEN);
	BIND_ENUM_CONSTANT(DIFFERENTIAL_LIMITED_SLIP);
	ClassDB::bind_method(D_METHOD("set_max_engine_torque", "value"), &PhysXVehicle3D::set_max_engine_torque);
	ClassDB::bind_method(D_METHOD("get_max_engine_torque"), &PhysXVehicle3D::get_max_engine_torque);
	ClassDB::bind_method(D_METHOD("set_max_brake_torque", "value"), &PhysXVehicle3D::set_max_brake_torque);
	ClassDB::bind_method(D_METHOD("get_max_brake_torque"), &PhysXVehicle3D::get_max_brake_torque);
	ClassDB::bind_method(D_METHOD("set_max_steer_angle", "value"), &PhysXVehicle3D::set_max_steer_angle);
	ClassDB::bind_method(D_METHOD("get_max_steer_angle"), &PhysXVehicle3D::get_max_steer_angle);
	ClassDB::bind_method(D_METHOD("set_ackermann_strength", "value"), &PhysXVehicle3D::set_ackermann_strength);
	ClassDB::bind_method(D_METHOD("get_ackermann_strength"), &PhysXVehicle3D::get_ackermann_strength);
	ClassDB::bind_method(D_METHOD("set_front_anti_roll_stiffness", "value"), &PhysXVehicle3D::set_front_anti_roll_stiffness);
	ClassDB::bind_method(D_METHOD("get_front_anti_roll_stiffness"), &PhysXVehicle3D::get_front_anti_roll_stiffness);
	ClassDB::bind_method(D_METHOD("set_rear_anti_roll_stiffness", "value"), &PhysXVehicle3D::set_rear_anti_roll_stiffness);
	ClassDB::bind_method(D_METHOD("get_rear_anti_roll_stiffness"), &PhysXVehicle3D::get_rear_anti_roll_stiffness);
	ADD_GROUP("Drivetrain", "");
	PHYSX_VEHICLE_ENGINE_BIND_DRIVETRAIN(PhysXVehicle3D)
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_engine_torque", PROPERTY_HINT_RANGE, "0,5000,10,or_greater"), "set_max_engine_torque", "get_max_engine_torque");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_brake_torque", PROPERTY_HINT_RANGE, "0,20000,10,or_greater"), "set_max_brake_torque", "get_max_brake_torque");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_steer_angle", PROPERTY_HINT_RANGE, "0,1.5708,0.01"), "set_max_steer_angle", "get_max_steer_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ackermann_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ackermann_strength", "get_ackermann_strength");

#define PHYSX_VEHICLE_BIND_FLOAT(m_name, m_hint) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PhysXVehicle3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PhysXVehicle3D::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, #m_name, PROPERTY_HINT_RANGE, m_hint), "set_" #m_name, "get_" #m_name);

	PHYSX_VEHICLE_ENGINE_BIND_SETTINGS(PhysXVehicle3D)

	ADD_GROUP("Differential", "");
	ClassDB::bind_method(D_METHOD("set_differential", "differential"), &PhysXVehicle3D::set_differential);
	ClassDB::bind_method(D_METHOD("get_differential"), &PhysXVehicle3D::get_differential);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "differential", PROPERTY_HINT_ENUM, "Open,Limited Slip"), "set_differential", "get_differential");
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_front_bias, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_front_target, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_rear_bias, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_rear_target, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_center_bias, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_center_target, "0,5,0.01,or_greater")
	PHYSX_VEHICLE_BIND_FLOAT(limited_slip_rate, "0,100,0.1,or_greater")

#undef PHYSX_VEHICLE_BIND_FLOAT

	ADD_GROUP("Anti-Roll", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "front_anti_roll_stiffness", PROPERTY_HINT_RANGE, "-50000,50000,100,or_less,or_greater"), "set_front_anti_roll_stiffness", "get_front_anti_roll_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rear_anti_roll_stiffness", PROPERTY_HINT_RANGE, "-50000,50000,100,or_less,or_greater"), "set_rear_anti_roll_stiffness", "get_rear_anti_roll_stiffness");
	ADD_GROUP("Tires", "");
	ClassDB::bind_method(D_METHOD("set_tire_force_point", "value"), &PhysXVehicle3D::set_tire_force_point);
	ClassDB::bind_method(D_METHOD("get_tire_force_point"), &PhysXVehicle3D::get_tire_force_point);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_force_point", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_tire_force_point", "get_tire_force_point");
	ClassDB::bind_method(D_METHOD("set_soft_soil", "enabled"), &PhysXVehicle3D::set_soft_soil);
	ClassDB::bind_method(D_METHOD("is_soft_soil"), &PhysXVehicle3D::is_soft_soil);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "soft_soil"), "set_soft_soil", "is_soft_soil");

	ClassDB::bind_method(D_METHOD("set_throttle", "value"), &PhysXVehicle3D::set_throttle);
	ClassDB::bind_method(D_METHOD("get_throttle"), &PhysXVehicle3D::get_throttle);
	ClassDB::bind_method(D_METHOD("set_brake", "value"), &PhysXVehicle3D::set_brake);
	ClassDB::bind_method(D_METHOD("get_brake"), &PhysXVehicle3D::get_brake);
	ClassDB::bind_method(D_METHOD("set_steer", "value"), &PhysXVehicle3D::set_steer);
	ClassDB::bind_method(D_METHOD("get_steer"), &PhysXVehicle3D::get_steer);
	ADD_GROUP("Controls", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "throttle", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_throttle", "get_throttle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "brake", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_brake", "get_brake");
	ClassDB::bind_method(D_METHOD("set_handbrake", "value"), &PhysXVehicle3D::set_handbrake);
	ClassDB::bind_method(D_METHOD("get_handbrake"), &PhysXVehicle3D::get_handbrake);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "handbrake", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_handbrake", "get_handbrake");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "steer", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_steer", "get_steer");
	ClassDB::bind_method(D_METHOD("set_reverse", "value"), &PhysXVehicle3D::set_reverse);
	ClassDB::bind_method(D_METHOD("is_reverse"), &PhysXVehicle3D::is_reverse);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverse"), "set_reverse", "is_reverse");
	PHYSX_VEHICLE_ENGINE_BIND_CONTROLS(PhysXVehicle3D)

	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &PhysXVehicle3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &PhysXVehicle3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PhysXVehicle3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PhysXVehicle3D::get_collision_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ClassDB::bind_method(D_METHOD("set_physics_material_override", "physics_material_override"), &PhysXVehicle3D::set_physics_material_override);
	ClassDB::bind_method(D_METHOD("get_physics_material_override"), &PhysXVehicle3D::get_physics_material_override);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "physics_material_override", PROPERTY_HINT_RESOURCE_TYPE, PhysicsMaterial::get_class_static()), "set_physics_material_override", "get_physics_material_override");

	ClassDB::bind_method(D_METHOD("get_linear_velocity"), &PhysXVehicle3D::get_linear_velocity);
	ClassDB::bind_method(D_METHOD("get_forward_speed"), &PhysXVehicle3D::get_forward_speed);
	ClassDB::bind_method(D_METHOD("get_forward"), &PhysXVehicle3D::get_forward);
	ClassDB::bind_method(D_METHOD("get_wheel_jounce", "wheel"), &PhysXVehicle3D::get_wheel_jounce);
	ClassDB::bind_method(D_METHOD("get_wheel_sinkage", "wheel"), &PhysXVehicle3D::get_wheel_sinkage);
	ClassDB::bind_method(D_METHOD("get_wheel_deflection", "wheel"), &PhysXVehicle3D::get_wheel_deflection);
	ClassDB::bind_method(D_METHOD("get_wheel_soil_side_force", "wheel"), &PhysXVehicle3D::get_wheel_soil_side_force);
	ClassDB::bind_method(D_METHOD("get_wheel_soil_resistance", "wheel"), &PhysXVehicle3D::get_wheel_soil_resistance);
	ClassDB::bind_method(D_METHOD("get_wheel_soil_traction", "wheel"), &PhysXVehicle3D::get_wheel_soil_traction);
	ClassDB::bind_method(D_METHOD("set_tyre_clogging", "enabled"), &PhysXVehicle3D::set_tyre_clogging);
	ClassDB::bind_method(D_METHOD("is_tyre_clogging"), &PhysXVehicle3D::is_tyre_clogging);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tyre_clogging"), "set_tyre_clogging", "is_tyre_clogging");
	ClassDB::bind_method(D_METHOD("set_parked_settling", "enabled"), &PhysXVehicle3D::set_parked_settling);
	ClassDB::bind_method(D_METHOD("is_parked_settling"), &PhysXVehicle3D::is_parked_settling);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "parked_settling"), "set_parked_settling", "is_parked_settling");
	ClassDB::bind_method(D_METHOD("get_wheel_creep", "wheel"), &PhysXVehicle3D::get_wheel_creep);
	ClassDB::bind_method(D_METHOD("get_wheel_clods_thrown", "wheel"), &PhysXVehicle3D::get_wheel_clods_thrown);
	ClassDB::bind_method(D_METHOD("get_wheel_suction", "wheel"), &PhysXVehicle3D::get_wheel_suction);
	ClassDB::bind_method(D_METHOD("set_belly_drag", "enabled"), &PhysXVehicle3D::set_belly_drag);
	ClassDB::bind_method(D_METHOD("is_belly_drag"), &PhysXVehicle3D::is_belly_drag);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "belly_drag"), "set_belly_drag", "is_belly_drag");
	ClassDB::bind_method(D_METHOD("set_river_forces", "enabled"), &PhysXVehicle3D::set_river_forces);
	ClassDB::bind_method(D_METHOD("is_river_forces"), &PhysXVehicle3D::is_river_forces);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "river_forces"), "set_river_forces", "is_river_forces");
	ClassDB::bind_method(D_METHOD("set_hull_seal", "share"), &PhysXVehicle3D::set_hull_seal);
	ClassDB::bind_method(D_METHOD("get_hull_seal"), &PhysXVehicle3D::get_hull_seal);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hull_seal", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hull_seal", "get_hull_seal");
	ClassDB::bind_method(D_METHOD("get_water_buoyancy"), &PhysXVehicle3D::get_water_buoyancy);
	ClassDB::bind_method(D_METHOD("set_hull_leak_area", "area"), &PhysXVehicle3D::set_hull_leak_area);
	ClassDB::bind_method(D_METHOD("get_hull_leak_area"), &PhysXVehicle3D::get_hull_leak_area);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hull_leak_area", PROPERTY_HINT_RANGE, "0,0.2,0.001,or_greater,suffix:m\u00B2"), "set_hull_leak_area", "get_hull_leak_area");
	ClassDB::bind_method(D_METHOD("set_air_intake_height", "height"), &PhysXVehicle3D::set_air_intake_height);
	ClassDB::bind_method(D_METHOD("get_air_intake_height"), &PhysXVehicle3D::get_air_intake_height);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "air_intake_height", PROPERTY_HINT_RANGE, "-1,3,0.01,suffix:m"), "set_air_intake_height", "get_air_intake_height");
	ClassDB::bind_method(D_METHOD("set_snorkel", "enabled"), &PhysXVehicle3D::set_snorkel);
	ClassDB::bind_method(D_METHOD("has_snorkel"), &PhysXVehicle3D::has_snorkel);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snorkel"), "set_snorkel", "has_snorkel");
	ClassDB::bind_method(D_METHOD("is_engine_drowned"), &PhysXVehicle3D::is_engine_drowned);
	ClassDB::bind_method(D_METHOD("get_cab_water"), &PhysXVehicle3D::get_cab_water);
	ClassDB::bind_method(D_METHOD("get_bow_wave_strength"), &PhysXVehicle3D::get_bow_wave_strength);
	ClassDB::bind_method(D_METHOD("get_intake_trough"), &PhysXVehicle3D::get_intake_trough);
	ClassDB::bind_method(D_METHOD("get_water_drag"), &PhysXVehicle3D::get_water_drag);
	ClassDB::bind_method(D_METHOD("get_wheel_mud", "wheel"), &PhysXVehicle3D::get_wheel_mud);
	ClassDB::bind_method(D_METHOD("get_wheel_mud_shed", "wheel"), &PhysXVehicle3D::get_wheel_mud_shed);
	ClassDB::bind_method(D_METHOD("get_belly_drag"), &PhysXVehicle3D::get_belly_drag);
	ClassDB::bind_method(D_METHOD("get_belly_support"), &PhysXVehicle3D::get_belly_support);
	ClassDB::bind_method(D_METHOD("get_wind_obstacles", "rect"), &PhysXVehicle3D::get_wind_obstacles);
	ClassDB::bind_method(D_METHOD("set_wind_obstacle", "enabled"), &PhysXVehicle3D::set_wind_obstacle);
	ClassDB::bind_method(D_METHOD("is_wind_obstacle"), &PhysXVehicle3D::is_wind_obstacle);
	ClassDB::bind_method(D_METHOD("set_body_mud", "enabled"), &PhysXVehicle3D::set_body_mud);
	ClassDB::bind_method(D_METHOD("is_body_mud"), &PhysXVehicle3D::is_body_mud);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "body_mud"), "set_body_mud", "is_body_mud");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_obstacle"), "set_wind_obstacle", "is_wind_obstacle");
	ClassDB::bind_method(D_METHOD("get_wheel_mud_spray", "wheel"), &PhysXVehicle3D::get_wheel_mud_spray);
	ClassDB::bind_method(D_METHOD("get_body_mud_line"), &PhysXVehicle3D::get_body_mud_line);
	ClassDB::bind_method(D_METHOD("get_body_mud_wetness"), &PhysXVehicle3D::get_body_mud_wetness);
	ClassDB::bind_method(D_METHOD("get_body_mud_color"), &PhysXVehicle3D::get_body_mud_color);
	ClassDB::bind_method(D_METHOD("add_mud_splat", "position", "velocity", "volume"), &PhysXVehicle3D::add_mud_splat);
	ClassDB::bind_method(D_METHOD("get_mud_splat_count"), &PhysXVehicle3D::get_mud_splat_count);
	ClassDB::bind_method(D_METHOD("get_mud_flakes_shed"), &PhysXVehicle3D::get_mud_flakes_shed);
	ClassDB::bind_method(D_METHOD("get_mud_splat", "index"), &PhysXVehicle3D::get_mud_splat);
	ClassDB::bind_method(D_METHOD("get_wheel_slip", "wheel"), &PhysXVehicle3D::get_wheel_slip);
	ClassDB::bind_method(D_METHOD("is_wheel_on_soft_soil", "wheel"), &PhysXVehicle3D::is_wheel_on_soft_soil);
	ClassDB::bind_method(D_METHOD("get_wheel_separation", "wheel"), &PhysXVehicle3D::get_wheel_separation);
	ClassDB::bind_method(D_METHOD("get_actor_position"), &PhysXVehicle3D::get_actor_position);

	ADD_GROUP("", "");
	PHYSX_VEHICLE_CONTROL_BIND(PhysXVehicle3D)
}

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
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/curve.h"

#include "modules/modules_enabled.gen.h" // For ground.

#ifdef MODULE_GROUND_ENABLED
#include "modules/ground/ground_surface_3d.h"
#include "modules/ground/ground_terramechanics.h"
#endif


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
	float sinkage = 0.0f; // m, smoothed
	float already = 0.0f; // m, the rut already pressed ahead of the wheel
	float resistance = 0.0f; // N, the last substep's
	float traction = 0.0f; // N, the soil's limit at the last substep's slip
	float base_long_stiff = -1.0f; // the tyre's own, restored off the soil
	float spin_sum = 0.0f; // rad/s, summed over the step's substeps
	int spin_n = 0;
	float slip = 0.0f; // terramechanics slip, smoothed over a tenth of a second
	bool soft = false;
	bool printed = false;
	Vector3 last_print;
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

class PhysXVehicleSoilRoad : public PxVehicleComponent {
public:
	Vehicle4W *vehicle = nullptr;
	PhysXVehicleSoilWheel *wheels = nullptr;
	PxVehicleSimulationContext *context = nullptr;
	float base_slip_denominator = -1.0f;

	virtual bool update(const PxReal p_dt, const PxVehicleSimulationContext &p_context) override {
		Vehicle4W &v = *vehicle;
		GroundSurface3D *ground = GroundSurface3D::get_active();
		if (base_slip_denominator < 0.0f) {
			base_slip_denominator = context->tireSlipParams.minActiveLongSlipDenominator;
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
			if (!ground || !road.hitState) {
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
			if (!sw.soft) {
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
			const PxVec3 lng = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLONGITUDINAL];
			const PxVec3 lat = v.tireDirectionStates[i].directions[PxVehicleTireDirectionModes::eLATERAL];
			const float v_lng = v.tireSpeedStates[i].speedStates[PxVehicleTireDirectionModes::eLONGITUDINAL];
			// The soil the wheel is about to press: just ahead of its contact.
			const GroundWheelSoil probe = GroundTerramechanics::wheel(sw.soil, load, wp.radius, width, sw.inflation, slip, 0.0f, 0.0f);
			const float ahead = (v_lng >= 0.0f ? 1.0f : -1.0f) * (0.5f * probe.contact_length + 0.05f);
			const Vector3 front = at + Vector3(lng.x, lng.y, lng.z) * ahead;
			sw.already = MAX(ground->get_dent_at(front) - ground->get_rim_at(front), 0.0f);
			// A rut already there ahead (another wheel's, an earlier pass) takes the wheel down into it.
			const float rut = MIN(ground->get_dent_at(front) - ground->get_rim_at(front), sw.soil.max_sinkage);
			// The load's sinkage settles over a tenth of a second (the soil yields as the load comes). Slip
			// sinkage is a steady state over fresh soil (Lyasko 2010): it builds over the travel of a contact
			// length, and a wheel spinning in place digs in over seconds rather than at once. A skidding wheel
			// ploughs at its static sinkage.
			const float static_z = MAX(probe.static_sinkage, rut);
			const float target = MAX(slip > 0.0f ? probe.sinkage : probe.static_sinkage, rut);
			if (sw.sinkage < static_z) {
				sw.sinkage += (static_z - sw.sinkage) * MIN(p_dt / 0.1f, 1.0f);
			}
			const float tau = MAX(0.1f, probe.contact_length / MAX(Math::abs(v_lng), 0.1f));
			sw.sinkage += (target - sw.sinkage) * MIN(p_dt / tau, 1.0f);
			road.plane.d += sw.sinkage;
			// The grip of the soil: the tyre's peak force to its shear limit, its stiffness to Janosi-Hanamoto's.
			if (load > 0.0f) {
				const PxVehicleTireForceParams &tp = v.tireForceParams[i];
				const float peak = MAX(MAX(tp.frictionVsSlip[0][1], tp.frictionVsSlip[1][1]), MAX(tp.frictionVsSlip[2][1], 0.05f));
				road.friction = probe.shear_limit / load / peak;
				const float slope = probe.shear_limit * probe.contact_length / (2.0f * MAX(sw.soil.shear_k, 1e-3f));
				const float substep = p_dt / 3.0f;
				const float stable = 1.5f * wp.moi * SOIL_SLIP_SPEED / (MAX(substep, 1e-4f) * wp.radius * wp.radius);
				v.tireForceParams[i].longStiff = MIN(MIN(sw.base_long_stiff, slope), stable);
			}
			any_soft = true;
			// The rut: the contact patch, stretched over the travel since the last print so a fast wheel
			// leaves no gaps.
			const PxVec3 omega_r = lng * (v.wheelRigidBody1dStates[i].rotationSpeed * wp.radius);
			const PxVec3 slip_vel = lng * v_lng - omega_r;
			const Vector3 travel = sw.printed ? at - sw.last_print : Vector3();
			const float travel_len = travel.length();
			const Vector3 center = sw.printed && travel_len < 2.0f ? at - travel * 0.5f : at;
			const float length = MAX(probe.contact_length, sw.printed && travel_len < 2.0f ? travel_len + probe.contact_length : 0.0f);
			const float yaw = Math::atan2(-lat.z, lat.x);
			if (sw.sinkage > 0.002f) {
				ground->press_wheel(center, Vector2(width, length), yaw, sw.sinkage, Vector2(slip_vel.x, slip_vel.z));
			}
			sw.last_print = at;
			sw.printed = true;
		}
		context->tireSlipParams.minActiveLongSlipDenominator = any_soft ? MAX(base_slip_denominator, SOIL_SLIP_SPEED) : base_slip_denominator;
		return true;
	}
};

class PhysXVehicleSoilTire : public PxVehicleComponent {
public:
	Vehicle4W *vehicle = nullptr;
	PhysXVehicleSoilWheel *wheels = nullptr;

	virtual bool update(const PxReal p_dt, const PxVehicleSimulationContext &p_context) override {
		Vehicle4W &v = *vehicle;
		for (PxU32 i = 0; i < 4; i++) {
			PhysXVehicleSoilWheel &sw = wheels[i];
			sw.spin_sum += v.wheelRigidBody1dStates[i].rotationSpeed;
			sw.spin_n++;
			if (!sw.soft || !v.roadGeomStates[i].hitState) {
				sw.resistance = 0.0f;
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
			const float resist = GroundTerramechanics::compaction_resistance(sw.soil, width, sw.sinkage, sw.already) + GroundTerramechanics::bulldozing_resistance(sw.soil, width, sw.sinkage, sw.already);
			const float r = resist * CLAMP(v_lng / 0.3f, -1.0f, 1.0f);
			if (r != 0.0f) {
				const PxVec3 dF = lng * -r;
				const PxVec3 arm = v.rigidBodyState.pose.rotate(v.suspensionParams[i].suspensionAttachment.transform(v.suspensionComplianceStates[i].tireForceAppPoint));
				f.forces[PxVehicleTireDirectionModes::eLONGITUDINAL] += dF;
				f.torques[PxVehicleTireDirectionModes::eLONGITUDINAL] += arm.cross(dF);
			}
			sw.resistance = Math::abs(r);
			sw.traction = ws.traction;
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
	// wheel_order[Vehicle4W::WHEEL_FL/FR/RL/RR] = index into the parent's own
	// `wheels` vector (child-registration order) for that canonical slot --
	// see configure_vehicle4w()'s own doc comment.
	PxU32 wheel_order[4] = {};
};

PhysXVehicle3D::PhysXVehicle3D() {
	impl = memnew(Impl);
}

PhysXVehicle3D::~PhysXVehicle3D() {
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

	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	if (!server) {
		return false;
	}
	GodotPhysXSpace3D *space = server->get_space(get_world_3d()->get_space());
	if (!space) {
		// Not running on the PhysX backend -- this node offers PxVehicle2-
		// specific capability and has no fallback for another backend.
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
	impl->soil_tire.vehicle = &impl->vehicle;
	impl->soil_tire.wheels = impl->soil_wheels;
	impl->vehicle.soil_road_component = soft_soil ? &impl->soil_road : nullptr;
	impl->vehicle.soil_tire_component = soft_soil ? &impl->soil_tire : nullptr;
#endif
	for (PhysXVehicleSoilWheel &sw : impl->soil_wheels) {
		sw = PhysXVehicleSoilWheel();
	}
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
		_control_built();
		_cache_chassis();
	},
			true);
	return impl->built;
}

PHYSX_VEHICLE_CONTROL_DEFINE(PhysXVehicle3D, 4)
PHYSX_VEHICLE_ENGINE_DEFINE(PhysXVehicle3D)

void PhysXVehicle3D::_destroy() {
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
			for (uint32_t i = 0; i < 4; i++) {
				impl->soil_wheels[i].inflation = (float)wheels[impl->wheel_order[i]]->get_tire_pressure();
			}
			v.step((PxReal)get_physics_process_delta_time(), impl->simulationContext);
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

real_t PhysXVehicle3D::get_wheel_soil_resistance(int p_wheel) const {
	const int i = _soil_slot(p_wheel);
	return i < 0 || !impl->soil_wheels[i].soft ? 0.0 : (real_t)impl->soil_wheels[i].resistance;
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

void PhysXVehicle3D::_bind_methods() {
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
	ClassDB::bind_method(D_METHOD("get_wheel_soil_resistance", "wheel"), &PhysXVehicle3D::get_wheel_soil_resistance);
	ClassDB::bind_method(D_METHOD("get_wheel_soil_traction", "wheel"), &PhysXVehicle3D::get_wheel_soil_traction);
	ClassDB::bind_method(D_METHOD("get_wheel_slip", "wheel"), &PhysXVehicle3D::get_wheel_slip);
	ClassDB::bind_method(D_METHOD("is_wheel_on_soft_soil", "wheel"), &PhysXVehicle3D::is_wheel_on_soft_soil);
	ClassDB::bind_method(D_METHOD("get_wheel_separation", "wheel"), &PhysXVehicle3D::get_wheel_separation);
	ClassDB::bind_method(D_METHOD("get_actor_position"), &PhysXVehicle3D::get_actor_position);

	ADD_GROUP("", "");
	PHYSX_VEHICLE_CONTROL_BIND(PhysXVehicle3D)
}

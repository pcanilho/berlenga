/**************************************************************************/
/*  physx_flow_3d.cpp                                                     */
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

#include "physx_flow_3d.h"

#include "flow_backend.h"
#include "modules/world/world_contracts.h"
#include "physx_flow_emitter_3d.h"
#include "physx_flow_render_effect.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/object_id.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/gradient_texture.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/shader.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_volumes.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

#ifdef MODULE_GROUND_ENABLED
#include "modules/ground/ground_height_query.h"
#endif

#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#endif

namespace {

void fill_transform(const Transform3D &p_xform, FlowBackend::Emitter &r_emitter) {
	for (int c = 0; c < 3; c++) {
		const Vector3 col = p_xform.basis.get_column(c);
		r_emitter.basis[c][0] = col.x;
		r_emitter.basis[c][1] = col.y;
		r_emitter.basis[c][2] = col.z;
		r_emitter.origin[c] = p_xform.origin[c];
	}
}

FlowBackend::Emitter make_solid(const Transform3D &p_xform, const Vector3 &p_velocity) {
	FlowBackend::Emitter e;
	fill_transform(p_xform, e);
	for (int c = 0; c < 3; c++) {
		e.body_velocity[c] = p_velocity[c];
	}
	e.collision = true;
	return e;
}

} // namespace

// One queued step: what the main thread gathered, for the render thread to
// simulate with.
struct PhysXFlow3D::StepQueue {
	struct Step {
		double time = 0.0;
		float delta = 0.0f;
		bool readback = false;
		FlowBackend::Settings settings;
		LocalVector<FlowBackend::Emitter> emitters;
	};
	LocalVector<Step> queue;
};

PhysXFlow3D::PhysXFlow3D() {
	steps = memnew(StepQueue);
}

void PhysXFlow3D::_post(const Callable &p_call) {
	// The RenderingDevice may only be driven from the render thread. With
	// rendering on its own thread this queues the call in order with the
	// frame's other rendering commands; single-threaded it runs right here.
	RenderingServer::get_singleton()->call_on_render_thread(p_call);
}

void PhysXFlow3D::_rt_create(int p_max_blocks) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	FlowBackend::Grid *g = FlowBackend::grid_create_rd(rd, (uint32_t)p_max_blocks);
	MutexLock lock(step_mutex);
	grid = g;
	stat_start_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

void PhysXFlow3D::_rt_destroy() {
	FlowBackend::Grid *g = nullptr;
	{
		MutexLock lock(step_mutex);
		g = grid;
		grid = nullptr;
		steps->queue.clear();
	}
	if (g != nullptr) {
		FlowBackend::grid_destroy(g);
	}
}

void PhysXFlow3D::_rt_step() {
	StepQueue::Step st;
	{
		MutexLock lock(step_mutex);
		if (steps->queue.is_empty() || grid == nullptr) {
			return;
		}
		st = steps->queue[0];
		steps->queue.remove_at(0);
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	if (!FlowBackend::grid_step(grid, st.time, st.delta, st.settings, st.emitters.ptr(), (int)st.emitters.size())) {
		return;
	}
	const bool full = FlowBackend::grid_active_blocks(grid) >= (uint32_t)max_blocks;
	const double step_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	MutexLock lock(step_mutex);
	stat_step_ms = step_ms;
	blocks_full = blocks_full || full;
	if (st.readback) {
		_read_back();
	}
}

Color PhysXFlow3D::_default_heat_at(float p_t) {
	// Used while heat_ramp is empty (a resource can't be a property default): the glow of soot as a blackbody,
	// the established way to colour fire (Nguyen, Fedkiw and Jensen 2002, "Physically Based Modeling and Animation
	// of Fire"; Pegoraro and Parker 2006). The ramp spans 600 K (no visible glow) to 1,500 K (a sooty flame's
	// core). Colour: the blackbody's sRGB chromaticity (Charity's CIE 1964 table). Brightness (alpha, may exceed 1):
	// Planck's law in Wien's approximation at 580 nm, exp(-c2 / (lambda T)), normalised to 1 at 1,400 K. It rises
	// about twentyfold per 100 K at flame temperatures, so a flame has a bright core and tongues with a sharp
	// edge, and the cooler gas above them reads as smoke, not as an orange glow.
	static const float KELVIN[] = { 600.0f, 800.0f, 1000.0f, 1200.0f, 1400.0f, 1600.0f };
	static const Color CHROMA[] = {
		Color(1.0f, 0.10f, 0.0f), Color(1.0f, 0.16f, 0.0f), Color(1.0f, 0.22f, 0.0f),
		Color(1.0f, 0.33f, 0.0f), Color(1.0f, 0.40f, 0.0f), Color(1.0f, 0.45f, 0.0f)
	};
	const float t_k = 600.0f + 900.0f * CLAMP(p_t, 0.0f, 1.0f);
	int i = 0;
	while (i < 4 && t_k > KELVIN[i + 1]) {
		i++;
	}
	Color c = CHROMA[i].lerp(CHROMA[i + 1], CLAMP((t_k - KELVIN[i]) / (KELVIN[i + 1] - KELVIN[i]), 0.0f, 1.0f));
	constexpr float C2_OVER_LAMBDA = 14388.0f / 0.58f; // second radiation constant (um K) over 580 nm
	c.a = Math::exp(C2_OVER_LAMBDA * (1.0f / 1400.0f - 1.0f / t_k));
	return c;
}

PhysXFlow3D::~PhysXFlow3D() {
	_stop();
	if (library_acquired) {
		FlowBackend::release_library();
		library_acquired = false;
	}
	memdelete(steps);
	steps = nullptr;
}

void PhysXFlow3D::_start() {
	if (grid != nullptr) {
		return;
	}
	if (!library_acquired) {
		char err[512] = {};
		library_acquired = FlowBackend::acquire_library(err, sizeof(err));
		if (!library_acquired) {
			device_error = String::utf8(err);
			// Once a run: every Flow node in the scene fails the same way, and stays inert (no-op).
			static bool warned = false;
			if (!warned) {
				warned = true;
				ERR_PRINT("PhysXFlow3D: NVIDIA Flow unavailable, Flow nodes do nothing: " + device_error);
			}
			update_configuration_warnings();
			return;
		}
	}
	rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		device_error = "it needs a RenderingDevice (Forward+ or Mobile renderer, not Compatibility or headless)";
		update_configuration_warnings();
		return;
	}
	_post(callable_mp(this, &PhysXFlow3D::_rt_create).bind(max_blocks));
	// Wait for it (one stall when a grid starts): everything below -- and the
	// render effect -- wants the grid there.
	RenderingServer::get_singleton()->sync();
	max_blocks_warned = false;
	blocks_full = false;
	device_error = grid != nullptr ? String() : String("Flow couldn't create its grid");
	update_configuration_warnings();
	sim_time = 0.0;
	step_count = 0;
	if (grid != nullptr && get_world_3d().is_valid()) {
		PhysXFlowRenderEffect::register_flow(this);
		PhysXFlowRenderEffect::attach_world(get_world_3d().ptr());
		attached_world = get_world_3d()->get_instance_id();
		PhysXFlowRenderEffect::install(get_world_3d().ptr(), get_viewport() ? get_viewport()->get_camera_3d() : nullptr);
	}
}

void PhysXFlow3D::_stop() {
	PhysXFlowRenderEffect::unregister_flow(this);
	if (attached_world.is_valid()) {
		PhysXFlowRenderEffect::detach_world(attached_world);
		attached_world = ObjectID();
	}
	if (query_shape.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(query_shape);
		query_shape = RID();
	}
	if (grid != nullptr) {
		// After the steps already queued; waited for, so the grid (and this
		// node, when it is being deleted) outlive every call that uses them.
		_post(callable_mp(this, &PhysXFlow3D::_rt_destroy));
		RenderingServer::get_singleton()->sync();
	}
	rd = nullptr;
	MutexLock lock(step_mutex);
	last_smoke.clear();
	last_temperature.clear();
	last_smoke_source = nullptr;
	gas_bounds = AABB();
}

void PhysXFlow3D::_fill_scatter_shadows(LocalVector<FlowScatterRD::Light> &r_lights, const PagedArray<RID> *p_instances, const RID &p_shadow_atlas, uint32_t p_flags) const {
	// Each light's shadow as Godot's renderer filled it for this frame
	// (LightStorage::update_light_buffers), rebuilt from world space: the
	// scattering pass works in world meters, not this view's camera space.
	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	if (ls == nullptr || p_instances == nullptr || !p_shadow_atlas.is_valid() || !ls->owns_shadow_atlas(p_shadow_atlas)) {
		return;
	}
	for (FlowScatterRD::Light &l : r_lights) {
		// Light3D keeps its RenderingServer light to itself: find the frame's
		// instance of it by where it is and what it is.
		const bool spot = l.direction_cutoff[3] > -1.5f;
		if (!(p_flags & (spot ? SCATTERING_SHADOW_SPOT : SCATTERING_SHADOW_OMNI))) {
			continue; // this kind of light's shadows aren't received
		}
		const Vector3 at(l.position_range[0], l.position_range[1], l.position_range[2]);
		RID instance;
		RID base;
		for (uint64_t j = 0; j < p_instances->size(); j++) {
			const RID candidate = (*p_instances)[j];
			const RID candidate_base = ls->light_instance_get_base_light(candidate);
			const RSE::LightType type = ls->light_get_type(candidate_base);
			if (type == (spot ? RSE::LIGHT_SPOT : RSE::LIGHT_OMNI) && ls->light_instance_get_base_transform(candidate).origin.distance_squared_to(at) < 1e-6f) {
				instance = candidate;
				base = candidate_base;
				break;
			}
		}
		if (!instance.is_valid() || !ls->light_has_shadow(base) || !ls->shadow_atlas_owns_light_instance(p_shadow_atlas, instance)) {
			continue;
		}
		Vector2i omni_offset;
		const Rect2 rect = ls->light_instance_get_shadow_atlas_rect(instance, p_shadow_atlas, omni_offset);
		const Transform3D light_xf = ls->light_instance_get_base_transform(instance);
		Projection m;
		float bias = ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS);
		if (spot) {
			Projection light_bias;
			light_bias.set_light_bias();
			Projection correction;
			correction.set_depth_correction(false, true, false);
			m = light_bias * correction * ls->light_instance_get_shadow_camera(instance, 0) * Projection(light_xf.affine_inverse());
			bias = bias / 100.0f * ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BLUR) * RendererSceneRenderRD::get_singleton()->shadows_quality_radius_get();
		} else {
			m = Projection(light_xf.affine_inverse());
			l.shadow[2] = omni_offset.x * rect.size.width;
			l.shadow[3] = omni_offset.y * rect.size.height;
		}
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				l.shadow_matrix[c * 4 + r] = (float)m.columns[c][r];
			}
		}
		l.atlas_rect[0] = rect.position.x;
		l.atlas_rect[1] = rect.position.y;
		l.atlas_rect[2] = rect.size.width;
		l.atlas_rect[3] = rect.size.height;
		l.shadow[0] = ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_OPACITY);
		l.shadow[1] = bias;
	}
}

bool PhysXFlow3D::prepare_traced() {
	traced_this_frame = false;
	if (grid == nullptr) {
		return false;
	}
	FlowBackend::ScatterSource source;
	if (!FlowBackend::grid_prepare_rd(grid, &source)) {
		return false;
	}
	traced_this_frame = true;
	RTSparseVolume v;
	v.density = RID::from_uint64(source.density_rid);
	v.table = RID::from_uint64(source.table_rid);
	memcpy(v.level, source.level, sizeof(v.level));
	v.world_min = Vector3(source.world_min[0], source.world_min[1], source.world_min[2]);
	v.world_max = Vector3(source.world_max[0], source.world_max[1], source.world_max[2]);
	v.block_size = Vector3(source.block_size_world[0], source.block_size_world[1], source.block_size_world[2]);
	v.layer = source.layer_and_level;
	v.units_per_meter = FlowBackend::UNITS_PER_METER;
	{
		MutexLock lock(scatter_mutex);
		v.extinction = traced_extinction;
		v.anisotropy = traced_anisotropy;
		v.smoke_color = traced_smoke_color;
		v.emission_scale = traced_emission;
		v.emission_proxy_light = traced_proxy_light;
		v.temperature_range = traced_temperature_range;
		v.shimmer_kelvin = traced_shimmer_kelvin;
		v.shimmer_air_kelvin = traced_air_kelvin;
		v.shimmer_turbulence = traced_shimmer_turbulence;
		v.shimmer_time = traced_shimmer_time;
		for (int i = 0; i < 8; i++) {
			v.heat_ramp[i] = traced_heat[i];
		}
	}
	v.albedo = scattering_albedo;
	v.density_majorant = 2.0f;
	RTVolumes::set_volume(v);
	return true;
}

// The frame's ambient light, as Godot's volumetric fog reads it (Fog::volumetric_fog_update).
void PhysXFlow3D::_fill_scatter_ambient(FlowScatterRD::Params &r_params, const RID &p_environment) const {
	r_params.ambient = true;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene == nullptr || !p_environment.is_valid() || !scene->is_environment(p_environment)) {
		return; // no environment: no ambient light
	}
	const RSE::EnvironmentAmbientSource source = scene->environment_get_ambient_source(p_environment);
	if (source == RSE::ENV_AMBIENT_SOURCE_DISABLED) {
		return;
	}
	const float energy = scene->environment_get_ambient_light_energy(p_environment);
	const Color c = scene->environment_get_ambient_light(p_environment).srgb_to_linear() * energy;
	r_params.ambient_color[0] = c.r;
	r_params.ambient_color[1] = c.g;
	r_params.ambient_color[2] = c.b;
	if (source == RSE::ENV_AMBIENT_SOURCE_COLOR) {
		return; // the color alone
	}
	const RID sky = scene->environment_get_sky(p_environment);
	if (!sky.is_valid()) {
		return;
	}
	RendererRD::SkyRD *sky_rd = scene->get_sky();
	r_params.sky_radiance = sky_rd->sky_get_radiance_texture_rd(sky);
	const float border = sky_rd->sky_get_uv_border_size(sky);
	r_params.sky_border[0] = border;
	r_params.sky_border[1] = 1.0f - border * 2.0f;
	r_params.sky_contribution = source == RSE::ENV_AMBIENT_SOURCE_SKY ? 1.0f : scene->environment_get_ambient_sky_contribution(p_environment);
	r_params.sky_energy = energy;
	r_params.sky_inverse = scene->environment_get_sky_orientation(p_environment).inverse();
}

void PhysXFlow3D::render_rd(PhysXFlowRenderEffect *p_effect, RenderingDevice *p_rd, const float p_view[16], const float p_projection[16], const Size2i &p_size, const RID &p_depth, const RID &p_color, const Projection &p_inv_view_projection, const Vector3 &p_camera, const PagedArray<RID> *p_lights, const RID &p_shadow_atlas, const RID &p_environment, uint32_t p_directional_count, const Transform3D &p_world_to_view, bool p_first_view, bool p_last_view) {
	if (grid == nullptr || p_rd != rd) {
		return;
	}
	if (traced_this_frame) {
		// The path tracer drew the gas: only end the Flow frame its resources belong to.
		if (p_last_view) {
			traced_this_frame = false;
			FlowBackend::grid_render_rd_finish(grid);
		}
		return;
	}
	LocalVector<FlowScatterRD::Light> lights;
	if (light_scattering) {
		MutexLock lock(scatter_mutex);
		lights = scatter_lights;
	}
	const uint32_t receive = scattering_receive_shadows;
	if (receive != 0 && !lights.is_empty()) {
		_fill_scatter_shadows(lights, p_lights, p_shadow_atlas, receive);
	}
	FlowShimmerRD::Params shimmer;
	{
		MutexLock lock(scatter_mutex);
		shimmer.kelvin = traced_shimmer_kelvin;
		shimmer.air_kelvin = traced_air_kelvin;
		shimmer.turbulence = traced_shimmer_turbulence;
		shimmer.time = traced_shimmer_time;
		shimmer.temperature_range = traced_temperature_range;
	}
	const bool shimmer_on = shimmer.kelvin > 0.0f;
	FlowBackend::ScatterSource source;
	const uint64_t out = FlowBackend::grid_render_rd(grid, p_view, p_projection, p_size.x, p_size.y, p_depth.get_id(), p_size.x, p_size.y, p_color.get_id(), (lights.is_empty() && !ambient_scattering && !sun_scattering && !cast_shadows && !shimmer_on) ? nullptr : &source, p_first_view);
	stat_render_calls++;
	stat_render_outputs += out != 0 ? 1 : 0;
	if (out != 0 && out != p_color.get_id()) {
		// Flow's output carries its transmittance in alpha (misc/physx_patches 0011), which the scattering pass
		// reads; the frame keeps its own alpha.
		p_effect->copy_texture(p_rd, RID::from_uint64(out), p_color, p_size, 1.0f, true);
	}
	if ((!lights.is_empty() || ambient_scattering || sun_scattering || cast_shadows) && !source.valid) {
		stat_scatter_no_source++;
	}
	if (out != 0 && source.valid) {
		stat_scatter_passes++;
		// Before the Flow frame ends: its volume and table are this frame's.
		FlowScatterRD::Params params;
		params.inv_view_projection = p_inv_view_projection;
		params.camera_position = p_camera;
		params.sigma = smoke_density;
		const Color albedo = smoke_color.srgb_to_linear();
		params.color[0] = albedo.r;
		params.color[1] = albedo.g;
		params.color[2] = albedo.b;
		params.strength = scattering_strength;
		params.anisotropy = scattering_anisotropy;
		params.scattering_albedo = scattering_albedo;
		params.ground_albedo = ground_albedo;
		params.shadow_steps = scattering_shadow_steps;
		params.shadow_reach = scattering_shadow_reach;
		params.shadows = receive != 0;
		params.resolution_divisor = scattering_resolution == SCATTERING_RESOLUTION_FULL ? 1 : (scattering_resolution == SCATTERING_RESOLUTION_HALF ? 2 : 4);
		params.temporal = scattering_temporal;
		if (ambient_scattering) {
			_fill_scatter_ambient(params, p_environment);
		}
		if (sun_scattering && p_directional_count > 0) {
			RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
			params.directional_count = (int)p_directional_count;
			params.directional_buffer = ls->get_directional_light_buffer();
			params.directional_shadow_atlas = ls->directional_shadow_get_texture();
			params.world_to_view = p_world_to_view;
		}
		RID atlas;
		if (receive != 0 && p_shadow_atlas.is_valid() && RendererRD::LightStorage::get_singleton()->owns_shadow_atlas(p_shadow_atlas)) {
			atlas = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_shadow_atlas);
		}
		// The sun's volumetric shadow map: the gas shadows the scene through it (cast_shadows), and the
		// scattering pass lights the gas from the sun through it (sun_scattering).
		FlowScatterRD::ShadowMap shadow;
		RID shadow_rid;
		bool shadow_casts = false;
		if ((cast_shadows || sun_scattering) && p_first_view) {
			MutexLock lock(scatter_mutex);
			shadow_rid = shadow_light;
			shadow_casts = cast_shadows && shadow_light_casts;
			shadow.to_light = shadow_to_light;
		}
		shadow.enabled = shadow_rid.is_valid();
		p_effect->get_scatter().run(p_rd, source, params, lights.ptr(), (int)lights.size(), p_depth, p_color, RID::from_uint64(out), atlas, p_size, &shadow);
		if (shadow_casts && shadow.texture.is_valid()) {
			// The opaque pass reads it from the next frame on (Forward+ only).
			RendererRD::LightStorage::get_singleton()->directional_volume_shadow_set(shadow_rid, shadow.texture, shadow.world_to_shadow);
		}
	}
	if (out != 0 && source.valid && shimmer_on) {
		// Last, over the gas and its light: what lies behind the hot air (the gas included) is seen along the
		// bent rays. Before the Flow frame ends: its volume and table are this frame's.
		shimmer.inv_view_projection = p_inv_view_projection;
		shimmer.camera_position = p_camera;
		FlowShimmerRD &pass = p_effect->get_shimmer();
		p_effect->copy_texture(p_rd, p_color, pass.get_source_copy(p_rd, p_size), p_size);
		pass.run(p_rd, source, shimmer, p_depth, p_color, p_size);
	}
	if (p_last_view) {
		FlowBackend::grid_render_rd_finish(grid);
	}
}

void PhysXFlow3D::_gather_emitters(LocalVector<FlowBackend::Emitter> &r_emitters, AABB &r_emitter_bounds) {
	bool first = true;
	for (Node *n : get_tree()->get_nodes_in_group(PhysXFlowEmitter3D::GROUP)) {
		PhysXFlowEmitter3D *e = Object::cast_to<PhysXFlowEmitter3D>(n);
		if (e == nullptr || !e->is_enabled() || !e->is_visible_in_tree() || e->get_flow() != this) {
			continue;
		}
		FlowBackend::Emitter fe;
		const Transform3D xf = e->get_global_transform();
		fill_transform(xf, fe);
		const bool box = e->get_shape() == PhysXFlowEmitter3D::SHAPE_BOX;
		fe.shape = box ? FlowBackend::Emitter::SHAPE_BOX : FlowBackend::Emitter::SHAPE_SPHERE;
		fe.radius = e->get_radius();
		const Vector3 half = e->get_size() * 0.5f;
		const Vector3 vel = e->get_velocity();
		const Vector3 body_vel = e->get_motion_velocity();
		for (int c = 0; c < 3; c++) {
			fe.half_size[c] = half[c];
			fe.velocity[c] = vel[c];
			fe.body_velocity[c] = body_vel[c];
		}
		fe.temperature = e->get_temperature();
		fe.fuel = e->get_fuel();
		fe.burn = e->get_burn();
		fe.smoke = e->get_smoke();
		fe.divergence = e->get_divergence();
		fe.couple_rate = e->get_couple_rate();
		fe.collision = e->is_collision();
		r_emitters.push_back(fe);
		if (first) {
			r_emitter_bounds = AABB(xf.origin, Vector3());
			first = false;
		} else {
			r_emitter_bounds.expand_to(xf.origin);
		}
	}
}

void PhysXFlow3D::_gather_colliders(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters, float p_delta) {
	bool dynamic_near = false;
	if (!collide_with_bodies || get_world_3d().is_null() || !p_region.has_volume()) {
		bodies_near = false;
		return;
	}
	PhysicsDirectSpaceState3D *space = get_world_3d()->get_direct_space_state();
	if (space == nullptr) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!query_shape.is_valid()) {
		query_shape = ps->box_shape_create();
	}
	ps->shape_set_data(query_shape, p_region.size * 0.5f);
	PhysicsDirectSpaceState3D::ShapeParameters query;
	query.shape_rid = query_shape;
	query.transform = Transform3D(Basis(), p_region.get_center());
	query.collision_mask = collision_mask;
	query.collide_with_bodies = true;
	query.collide_with_areas = false;
	constexpr int MAX_RESULTS = 64;
	PhysicsDirectSpaceState3D::ShapeResult results[MAX_RESULTS];
	const int count = space->intersect_shape(query, results, MAX_RESULTS);
	// One shape as solids for the gas.
	auto add_shape = [&](const RID &shape, const Transform3D &xform, const Vector3 &velocity) {
		const Variant shape_data = ps->shape_get_data(shape);
		switch (ps->shape_get_type(shape)) {
			case PhysicsServer3D::SHAPE_BOX: {
				FlowBackend::Emitter e = make_solid(xform, velocity);
				e.shape = FlowBackend::Emitter::SHAPE_BOX;
				const Vector3 half = shape_data;
				for (int c = 0; c < 3; c++) {
					e.half_size[c] = half[c];
				}
				r_emitters.push_back(e);
			} break;
			case PhysicsServer3D::SHAPE_SPHERE: {
				FlowBackend::Emitter e = make_solid(xform, velocity);
				e.shape = FlowBackend::Emitter::SHAPE_SPHERE;
				e.radius = shape_data;
				r_emitters.push_back(e);
			} break;
			case PhysicsServer3D::SHAPE_CAPSULE: {
				// A row of spheres along the capsule's axis (local Y).
				const Dictionary d = shape_data;
				const float radius = d["radius"];
				const float half_line = MAX(float(d["height"]) * 0.5f - radius, 0.0f);
				const int spheres = MAX(2, (int)Math::ceil(2.0f * half_line / MAX(radius, 0.01f)) + 1);
				for (int k = 0; k < spheres; k++) {
					const float y = -half_line + 2.0f * half_line * k / (spheres - 1);
					FlowBackend::Emitter e = make_solid(xform * Transform3D(Basis(), Vector3(0, y, 0)), velocity);
					e.shape = FlowBackend::Emitter::SHAPE_SPHERE;
					e.radius = radius;
					r_emitters.push_back(e);
				}
			} break;
			case PhysicsServer3D::SHAPE_CYLINDER: {
				const Dictionary d = shape_data;
				const float radius = d["radius"];
				FlowBackend::Emitter e = make_solid(xform, velocity);
				e.shape = FlowBackend::Emitter::SHAPE_BOX;
				e.half_size[0] = radius;
				e.half_size[1] = float(d["height"]) * 0.5f;
				e.half_size[2] = radius;
				r_emitters.push_back(e);
			} break;
			default: {
				// Convex / trimesh / heightmap: not yet (needs Flow's mesh
				// collider).
			} break;
		}
	};
	HashSet<RID> native_bodies;
	for (int i = 0; i < count; i++) {
		const RID body = results[i].rid;
		const int index = results[i].shape;
		const Transform3D body_xform = ps->body_get_state(body, PhysicsServer3D::BODY_STATE_TRANSFORM);
		if (ps->body_get_shape_count(body) == 0) {
			// A body whose shapes live on its own PhysX actor (PhysXVehicle3D and the other vehicles): its
			// CollisionShape3D children, once per body.
			if (native_bodies.has(body)) {
				continue;
			}
			native_bodies.insert(body);
			Node3D *owner = ObjectDB::get_instance<Node3D>(ObjectID(ps->body_get_object_instance_id(body)));
			if (owner == nullptr) {
				continue;
			}
			const Transform3D owner_xform = owner->get_global_transform();
			const Vector3 lin = owner->get("linear_velocity");
			const Vector3 ang = owner->get("angular_velocity");
			for (int c = 0; c < owner->get_child_count(); c++) {
				CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(owner->get_child(c));
				if (cs == nullptr || cs->is_disabled() || cs->get_shape().is_null()) {
					continue;
				}
				const Transform3D xform = cs->get_global_transform();
				add_shape(cs->get_shape()->get_rid(), xform, lin + ang.cross(xform.origin - owner_xform.origin));
			}
			continue;
		}
		const RID shape = ps->body_get_shape(body, index);
		if (!shape.is_valid()) {
			continue;
		}
		const Transform3D xform = body_xform * ps->body_get_shape_transform(body, index);
		// The body's velocity where this shape is, spin included: a shape on a
		// spinning or swinging body (a rotor, a door, a walker on a turntable)
		// moves although the body's centre may not.
		Vector3 velocity = ps->body_get_state(body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY);
		if (PhysicsDirectBodyState3D *state = ps->body_get_direct_state(body)) {
			velocity = state->get_velocity_at_local_position(xform.origin - body_xform.origin);
		}
		add_shape(shape, xform, velocity);
		if (push_bodies && p_delta > 0.0f) {
			const PhysicsServer3D::BodyMode mode = ps->body_get_mode(body);
			if (mode == PhysicsServer3D::BODY_MODE_RIGID || mode == PhysicsServer3D::BODY_MODE_RIGID_LINEAR) {
				dynamic_near = true;
				_push_shape(ps, body, shape, xform, p_delta);
			}
		}
	}
	bodies_near = dynamic_near;
}

// The gas's push on one dynamic body's shape: quadratic drag F = 0.5 rho Cd A |u - v| (u - v) at the shape, u the
// gas velocity (read back, a few frames late), v the body's there, A the shape's mean projected area (Cauchy's
// formula: a convex body's surface over four, so it does not depend on how it faces the flow). Applied as an
// impulse at the shape, so an off-centre push spins the body.
void PhysXFlow3D::_push_shape(PhysicsServer3D *p_ps, const RID &p_body, const RID &p_shape, const Transform3D &p_xform, float p_delta) {
	PhysicsDirectBodyState3D *state = p_ps->body_get_direct_state(p_body);
	if (state == nullptr) {
		return;
	}
	const Vector3 rel_pos = p_xform.origin - state->get_transform().origin;
	const Variant data = p_ps->shape_get_data(p_shape);
	const Vector3 sc = p_xform.basis.get_scale();
	float area = 0.0f;
	switch (p_ps->shape_get_type(p_shape)) {
		case PhysicsServer3D::SHAPE_BOX: {
			const Vector3 h = Vector3(data) * sc;
			area = 2.0f * (h.x * h.y + h.y * h.z + h.z * h.x); // surface 8(...), over 4
		} break;
		case PhysicsServer3D::SHAPE_SPHERE: {
			const float r = float(data) * sc.x;
			area = Math::PI * r * r;
		} break;
		case PhysicsServer3D::SHAPE_CAPSULE:
		case PhysicsServer3D::SHAPE_CYLINDER: {
			const Dictionary d = data;
			const float r = float(d["radius"]) * sc.x;
			const float h = float(d["height"]) * sc.y;
			area = 0.5f * Math::PI * r * r + 0.5f * Math::PI * r * h;
		} break;
		case PhysicsServer3D::SHAPE_CONVEX_POLYGON: {
			// The hull's bounds, as a box (debris and Blast pieces are roughly boxy).
			const PackedVector3Array pts = data;
			if (pts.is_empty()) {
				return;
			}
			AABB b(pts[0] * sc, Vector3());
			for (int i = 1; i < pts.size(); i++) {
				b.expand_to(pts[i] * sc);
			}
			const Vector3 h = b.size * 0.5f;
			area = 2.0f * (h.x * h.y + h.y * h.z + h.z * h.x);
		} break;
		default: {
			return; // meshes and heightmaps are static ground
		}
	}
	// The gas velocity around the shape, not at its centre: the body is a solid for the gas, which takes the body's
	// own velocity inside it. Six points on a shell a cell and a half outside it (a sphere of the same mean area).
	const float shell = Math::sqrt(area / Math::PI) + 1.5f * cell_size;
	static const Vector3 DIRS[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	Vector3 u;
	int inside = 0;
	for (int k = 0; k < 6; k++) {
		const Vector3 uk = sample_velocity(p_xform.origin + DIRS[k] * shell);
		if (uk != Vector3()) {
			u += uk;
			inside++;
		}
	}
	if (inside == 0) {
		return; // no gas here (or nothing read back yet)
	}
	u /= (float)inside;
	const Vector3 rel = u - state->get_velocity_at_local_position(rel_pos);
	const float speed = rel.length();
	if (speed < 0.05f) {
		return;
	}
	constexpr float AIR_DENSITY = 1.2f; // kg/m3
	const Vector3 force = rel * (0.5f * AIR_DENSITY * body_drag * area * speed);
	// Never more than the change that would bring the body to the gas's speed this step (stiff drag on light bodies).
	const float inv_mass = state->get_inverse_mass();
	Vector3 impulse = force * p_delta;
	if (inv_mass > 0.0f) {
		const float max_impulse = speed / inv_mass;
		if (impulse.length() > max_impulse) {
			impulse = impulse.normalized() * max_impulse;
		}
	}
	state->apply_impulse(impulse, rel_pos);
	if (state->is_sleeping()) {
		state->set_sleep_state(false);
	}
}

// The scene's lights, for the flows to look theirs up in every second or so: found by one walk of the tree when the
// first flow enters it, then kept from the SceneTree's node_added / node_removed while any flow is in it. (A walk of
// the whole scene per lookup, per flow, cost tens of milliseconds in scenes of a few hundred thousand nodes.)
static ObjectID flow_lights_tree;
static int flow_lights_users = 0;
static HashSet<ObjectID> flow_lights;

static void _flow_light_added(Node *p_node) {
	if (Object::cast_to<Light3D>(p_node) != nullptr) {
		flow_lights.insert(p_node->get_instance_id());
	}
}

static void _flow_light_removed(Node *p_node) {
	if (Object::cast_to<Light3D>(p_node) != nullptr) {
		flow_lights.erase(p_node->get_instance_id());
	}
}

static void _flow_lights_watch(SceneTree *p_tree) {
	if (flow_lights_users++ > 0) {
		return;
	}
	flow_lights_tree = p_tree->get_instance_id();
	p_tree->connect(SNAME("node_added"), callable_mp_static(&_flow_light_added));
	p_tree->connect(SNAME("node_removed"), callable_mp_static(&_flow_light_removed));
	TypedArray<Node> found = p_tree->get_root()->find_children("*", "Light3D", true, false);
	for (int i = 0; i < found.size(); i++) {
		flow_lights.insert(Object::cast_to<Node>(found[i])->get_instance_id());
	}
}

static void _flow_lights_unwatch() {
	if (--flow_lights_users > 0) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(ObjectDB::get_instance(flow_lights_tree));
	if (tree != nullptr) {
		tree->disconnect(SNAME("node_added"), callable_mp_static(&_flow_light_added));
		tree->disconnect(SNAME("node_removed"), callable_mp_static(&_flow_light_removed));
	}
	flow_lights_tree = ObjectID();
	flow_lights.clear();
}

// The lights of class p_class under p_root, in the order p_root->find_children() would list them (tree order).
static void _flow_lights_under(const Node *p_root, const char *p_class, LocalVector<Node *> &r_lights) {
	r_lights.clear();
	for (const ObjectID &id : flow_lights) {
		Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id));
		if (n != nullptr && n->is_inside_tree() && n->is_class(p_class) && p_root->is_ancestor_of(n)) {
			r_lights.push_back(n);
		}
	}
	struct TreeOrder {
		bool operator()(const Node *p_a, const Node *p_b) const { return p_b->is_greater_than(p_a); }
	};
	r_lights.sort_custom<TreeOrder>();
}

void PhysXFlow3D::_gather_light(Vector3 &r_direction, Color &r_color, Color &r_ambient) {
	DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(ObjectDB::get_instance(light_id));
	if (light == nullptr || !light->is_inside_tree() || light_search_countdown-- <= 0) {
		light_search_countdown = 60;
		light = nullptr;
		Node *root = get_tree()->get_edited_scene_root();
		if (root == nullptr) {
			root = get_tree()->get_current_scene();
		}
		if (root == nullptr) {
			root = get_tree()->get_root(); // a scene built from a script has no current scene
		}
		if (root != nullptr) {
			LocalVector<Node *> lights;
			_flow_lights_under(root, "DirectionalLight3D", lights);
			for (uint32_t i = 0; i < lights.size(); i++) {
				DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(lights[i]);
				if (l != nullptr && l->is_visible_in_tree()) {
					light = l;
					break;
				}
			}
		}
		light_id = light != nullptr ? light->get_instance_id() : ObjectID();
	}
	r_direction = Vector3(0.3, 1.0, 0.2).normalized();
	r_color = Color(1, 1, 1);
	if (light != nullptr && light->is_visible_in_tree()) {
		// A directional light shines along its -Z: toward it is +Z.
		r_direction = light->get_global_transform().basis.get_column(2).normalized();
		r_color = light->get_color().srgb_to_linear() * light->get_param(Light3D::PARAM_ENERGY);
	}
	{
		MutexLock lock(scatter_mutex);
		const bool visible = light != nullptr && light->is_visible_in_tree();
		shadow_light = visible ? light->get_light_rid() : RID();
		shadow_light_casts = visible && light->has_shadow();
		shadow_to_light = r_direction;
	}
	r_ambient = Color(0.25, 0.27, 0.3);
	if (get_world_3d().is_valid() && get_world_3d()->get_environment().is_valid()) {
		Ref<Environment> env = get_world_3d()->get_environment();
		r_ambient = env->get_ambient_light_color().srgb_to_linear() * env->get_ambient_light_energy();
	}
}

void PhysXFlow3D::_gather_scatter_lights() {
	LocalVector<FlowScatterRD::Light> picked;
	if (light_scattering) {
		if (scatter_search_countdown-- <= 0) {
			scatter_search_countdown = 60;
			scatter_light_ids.clear();
			Node *root = get_tree()->get_edited_scene_root();
			if (root == nullptr) {
				root = get_tree()->get_current_scene();
			}
			if (root == nullptr) {
				root = get_tree()->get_root(); // a scene built from a script has no current scene
			}
			if (root != nullptr) {
				for (const char *type : { "OmniLight3D", "SpotLight3D" }) {
					LocalVector<Node *> found;
					_flow_lights_under(root, type, found);
					for (Node *l : found) {
						scatter_light_ids.push_back(l->get_instance_id());
					}
				}
			}
		}
		// The lights that can reach the gas, strongest at it first (energy over
		// squared distance to where the gas is, roughly).
		struct Candidate {
			FlowScatterRD::Light light;
			float score = 0.0f;
		};
		LocalVector<Candidate> candidates;
		constexpr float GAS_REACH = 20.0f; // how far gas may have spread from draw_center
		for (const ObjectID &id : scatter_light_ids) {
			Light3D *l = Object::cast_to<Light3D>(ObjectDB::get_instance(id));
			if (l == nullptr || !l->is_inside_tree() || !l->is_visible_in_tree()) {
				continue;
			}
			// Light3D.light_volumetric_fog_energy: the same per-light knob Godot's
			// volumetric fog uses (0 leaves the gas dark).
			const float energy = l->get_param(Light3D::PARAM_ENERGY) * l->get_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY);
			const float range = l->get_param(Light3D::PARAM_RANGE);
			const Transform3D xf = l->get_global_transform();
			const float d = xf.origin.distance_to(draw_center);
			if (energy <= 0.0f || range <= 0.0f || d > range + GAS_REACH) {
				continue;
			}
			Candidate c;
			// The energy as Godot's light buffers carry it (LightStorage::update_light_buffers): times pi
			// without physical light units; with them, the luminous intensity of the light's power.
			float units = Math::PI;
			if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
				units = l->get_param(Light3D::PARAM_INTENSITY) / (Object::cast_to<SpotLight3D>(l) != nullptr ? Math::PI : 4.0f * Math::PI);
			}
			const Color col = l->get_color().srgb_to_linear() * (energy * units);
			c.light.position_range[0] = xf.origin.x;
			c.light.position_range[1] = xf.origin.y;
			c.light.position_range[2] = xf.origin.z;
			c.light.position_range[3] = range;
			c.light.color_decay[0] = col.r;
			c.light.color_decay[1] = col.g;
			c.light.color_decay[2] = col.b;
			c.light.color_decay[3] = l->get_param(Light3D::PARAM_ATTENUATION);
			c.light.direction_cutoff[3] = -2.0f; // omni
			if (Object::cast_to<SpotLight3D>(l) != nullptr) {
				// A spot shines along its -Z.
				const Vector3 dir = -xf.basis.get_column(2).normalized();
				c.light.direction_cutoff[0] = dir.x;
				c.light.direction_cutoff[1] = dir.y;
				c.light.direction_cutoff[2] = dir.z;
				c.light.direction_cutoff[3] = Math::cos(Math::deg_to_rad(l->get_param(Light3D::PARAM_SPOT_ANGLE)));
				c.light.spot[0] = l->get_param(Light3D::PARAM_SPOT_ATTENUATION);
			}
			c.score = energy / MAX(d * d, 1.0f);
			candidates.push_back(c);
		}
		for (uint32_t i = 1; i < candidates.size(); i++) {
			for (uint32_t j = i; j > 0 && candidates[j - 1].score < candidates[j].score; j--) {
				SWAP(candidates[j - 1], candidates[j]);
			}
		}
		for (uint32_t i = 0; i < candidates.size() && (int)i < scattering_max_lights; i++) {
			picked.push_back(candidates[i].light);
		}
	}
	stat_scatter_lights = (int)picked.size();
	MutexLock lock(scatter_mutex);
	scatter_lights = picked;
}

void PhysXFlow3D::_fill_render_settings(FlowBackend::Settings &r_settings) {
	// Flow colors by temperature through a colormap and lights it with its
	// own self-shadowing: the smoke's color under the sun, with the heat
	// ramp's glow; opacity comes from the smoke itself.
	Vector3 dir;
	Color light;
	Color ambient;
	_gather_light(dir, light, ambient);
	if (sun_scattering) {
		light = Color(0, 0, 0); // the scattering pass lights the smoke with the sun instead
	}
	for (int i = 0; i < FlowBackend::Settings::COLORMAP_POINTS; i++) {
		const float t = (float)i / (float)(FlowBackend::Settings::COLORMAP_POINTS - 1);
		const Color smoke = (smoke_ramp.is_valid() ? smoke_ramp->get_color_at_offset(t) : smoke_color).srgb_to_linear();
		const Color heat = heat_ramp.is_valid() ? heat_ramp->get_color_at_offset(t) : _default_heat_at(t);
		// Where it glows, the glow replaces the lit smoke instead of adding to
		// it (bright smoke plus orange reads pink).
		const float glow_amount = emission_strength > 0.0f ? CLAMP(heat.a, 0.0f, 1.0f) : 0.0f;
		const Color glow = heat.srgb_to_linear() * (heat.a * emission_strength);
		r_settings.colormap_rgb[i][0] = smoke.r * light.r * (1.0f - glow_amount) + glow.r;
		r_settings.colormap_rgb[i][1] = smoke.g * light.g * (1.0f - glow_amount) + glow.g;
		r_settings.colormap_rgb[i][2] = smoke.b * light.b * (1.0f - glow_amount) + glow.b;
	}
	r_settings.temperature_range = temperature_range;
	r_settings.attenuation = smoke_density;
	{
		// The path tracer's emission coefficient (per meter) and medium.
		MutexLock lock(scatter_mutex);
		for (int i = 0; i < 8; i++) {
			const float t = (float)i / 7.0f;
			const Color heat = heat_ramp.is_valid() ? heat_ramp->get_color_at_offset(t) : _default_heat_at(t);
			traced_heat[i] = heat.srgb_to_linear() * (heat.a * emission_strength);
		}
		traced_smoke_color = smoke_color.srgb_to_linear();
		traced_extinction = smoke_density;
		traced_anisotropy = scattering_anisotropy;
		traced_emission = emission_strength > 0.0f ? 1.0f : 0.0f; // the path tracer weighs the ramp by the smoke's extinction
		traced_temperature_range = temperature_range;
	}
	r_settings.light_direction[0] = dir.x;
	r_settings.light_direction[1] = dir.y;
	r_settings.light_direction[2] = dir.z;
	// Ambient as the floor of the self-shadowing, relative to the sun.
	// (With ambient_scattering the scattering pass adds the ambient light, with its color and the sky's,
	// so the floor drops to Flow's minimum.)
	r_settings.shadow_min_intensity = ambient_scattering ? 0.02f : CLAMP(ambient.get_luminance() / MAX(light.get_luminance(), 0.001f), 0.02f, 1.0f);
	r_settings.shadow_steps = shadow_steps;
}

// The ground under a grid node (x, z = node * spacing), cached. NaN: no ground, or ground that is already a
// solid through collide_with_bodies (a box, sphere or capsule).
float PhysXFlow3D::_terrain_height_at(const Vector2i &p_node, float p_spacing, float p_top, void *p_query) {
	if (const float *cached = terrain_heights.getptr(p_node)) {
		return *cached;
	}
	const Vector3 p(p_node.x * p_spacing, p_top, p_node.y * p_spacing);
	float h = Math::NaN;
#ifdef MODULE_GROUND_ENABLED
	const GroundHeightQuery *q = (const GroundHeightQuery *)p_query;
	if (q != nullptr && q->has_source()) {
		h = q->height(p);
	}
#endif
	if (!Math::is_finite(h) && get_world_3d().is_valid()) {
		// A ray straight down that only counts static ground: a car or a crate under the gas is a moving
		// solid already, and must not become a floor tile.
		PhysicsDirectSpaceState3D *space = get_world_3d()->get_direct_space_state();
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		if (space != nullptr) {
			PhysicsDirectSpaceState3D::RayParameters rp;
			rp.from = p;
			rp.to = p - Vector3(0, 4000.0f, 0);
			rp.collision_mask = collision_mask;
			rp.collide_with_areas = false;
			for (int tries = 0; tries < 6; tries++) {
				PhysicsDirectSpaceState3D::RayResult rr;
				if (!space->intersect_ray(rp, rr)) {
					break;
				}
				if (ps->body_get_mode(rr.rid) != PhysicsServer3D::BODY_MODE_STATIC) {
					rp.exclude.insert(rr.rid);
					continue;
				}
				const RID shape = rr.shape >= 0 ? ps->body_get_shape(rr.rid, rr.shape) : RID();
				const PhysicsServer3D::ShapeType type = shape.is_valid() ? ps->shape_get_type(shape) : PhysicsServer3D::SHAPE_CUSTOM;
				const bool already_solid = collide_with_bodies && (type == PhysicsServer3D::SHAPE_BOX || type == PhysicsServer3D::SHAPE_SPHERE || type == PhysicsServer3D::SHAPE_CAPSULE);
				if (!already_solid) {
					h = rr.position.y;
				}
				break;
			}
		}
	}
	terrain_heights.insert(p_node, h);
	return h;
}

// The AMBIENT WIND: the weather's wind (the world contract's wind_at) laid over the region as air-only boxes in
// layers, so smoke from every emitter leans and drifts with the same air as the grass and trees. They reserve
// no blocks: they only couple the velocity where the gas already is.
void PhysXFlow3D::_gather_weather_wind(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters) const {
	if (!weather_wind || weather_wind_coupling <= 0.0f || !p_region.has_volume()) {
		return;
	}
	const WorldWeather *wm = WorldContracts::weather();
	const WorldFire *fire = WorldContracts::fire();
	const bool fires = fire && fire->has_provider();
	if ((wm == nullptr || !wm->has_provider()) && !fires) {
		return;
	}
	// Layers of at most 4 m, up to 8: the wind grows with height (log law), so a tall plume bends more aloft.
	const AABB r = p_region.grow(2.0f);
	const int layers = CLAMP(int(Math::ceil(r.size.y / 4.0f)), 1, 8);
	const float lh = r.size.y / layers;
	for (int i = 0; i < layers; i++) {
		const Vector3 c(r.position.x + r.size.x * 0.5f, r.position.y + lh * (i + 0.5f), r.position.z + r.size.z * 0.5f);
		Vector3 w = wm && wm->has_provider() ? wm->wind_at(c) : Vector3();
		if (fires) {
			// Another fire's indraft draws smoke and dust beside it in (near the ground, fading over a few metres);
			// over a fire's own Flow it converges on the box's middle and nets out.
			w += fire->wind_at(c) * (1.0f - Math::smoothstep(2.0f, 6.0f, c.y - r.position.y));
		}
		FlowBackend::Emitter e;
		e.shape = FlowBackend::Emitter::SHAPE_BOX;
		e.origin[0] = c.x;
		e.origin[1] = c.y;
		e.origin[2] = c.z;
		e.half_size[0] = r.size.x * 0.5f;
		e.half_size[1] = lh * 0.5f;
		e.half_size[2] = r.size.z * 0.5f;
		e.velocity[0] = w.x;
		e.velocity[1] = w.y;
		e.velocity[2] = w.z;
		e.temperature = 0.0f;
		e.smoke = 0.0f;
		e.couple_rate = weather_wind_coupling;
		e.allocate = false;
		r_emitters.push_back(e);
	}
}

// The TERRAIN FLOOR: one solid box per grid cell of the ground under the region, its top laid on the plane
// through the cell's four corner heights (tilted to the slope, so the gas meets a smooth ground, not steps),
// a little wider than the cell so neighbours overlap and nothing leaks between them. Solids reserve no blocks.
void PhysXFlow3D::_gather_terrain_floor(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters) {
	if (!terrain_floor || !p_region.has_volume() || !is_inside_tree()) {
		return;
	}
	// At most MAX_TILES a side: a wide region gets coarser cells (power-of-two steps of the spacing, so the
	// height cache keeps its keys while the region drifts).
	constexpr int MAX_TILES = 32;
	const float extent = MAX(p_region.size.x, p_region.size.z);
	float s = terrain_floor_spacing;
	while (extent / s > MAX_TILES) {
		s *= 2.0f;
	}
	// Heights are re-read once a second (deformed ground changes), or when the spacing changes.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (s != terrain_heights_spacing || now - terrain_heights_msec > 1000 || terrain_heights.size() > 16384) {
		terrain_heights.clear();
		terrain_heights_spacing = s;
		terrain_heights_msec = now;
	}
	void *query = nullptr;
#ifdef MODULE_GROUND_ENABLED
	GroundHeightQuery q = GroundHeightQuery::make(this, terrain_path, collision_mask);
	q.space = nullptr; // its ray counts moving bodies too: ours (above) does not
	query = &q;
#endif
	const float top = p_region.get_end().y + 1.0f;
	const Vector2i a(Math::floor(p_region.position.x / s), Math::floor(p_region.position.z / s));
	const Vector2i b(Math::ceil(p_region.get_end().x / s), Math::ceil(p_region.get_end().z / s));
	const float thick = MAX(s, 0.5f); // half thickness: fast gas can't cross it in a step
	for (int j = a.y; j < b.y; j++) {
		for (int i = a.x; i < b.x; i++) {
			const float h00 = _terrain_height_at(Vector2i(i, j), s, top, query);
			const float h10 = _terrain_height_at(Vector2i(i + 1, j), s, top, query);
			const float h01 = _terrain_height_at(Vector2i(i, j + 1), s, top, query);
			const float h11 = _terrain_height_at(Vector2i(i + 1, j + 1), s, top, query);
			if (!Math::is_finite(h00) || !Math::is_finite(h10) || !Math::is_finite(h01) || !Math::is_finite(h11)) {
				continue;
			}
			const float hc = (h00 + h10 + h01 + h11) * 0.25f;
			if (hc > top) {
				continue; // ground above the gas: nothing for it to lie on
			}
			const float dhx = ((h10 - h00) + (h11 - h01)) / (2.0f * s);
			const float dhz = ((h01 - h00) + (h11 - h10)) / (2.0f * s);
			const Vector3 n = Vector3(-dhx, 1.0f, -dhz).normalized();
			const Vector3 tx = Vector3(1.0f, dhx, 0.0f).normalized();
			const Vector3 tz = tx.cross(n);
			const float half = 0.5f * s * Math::sqrt(1.0f + MAX(dhx * dhx, dhz * dhz)) * 1.15f;
			const Vector3 center = Vector3((i + 0.5f) * s, hc, (j + 0.5f) * s) - n * thick;
			FlowBackend::Emitter e = make_solid(Transform3D(Basis(tx, n, tz), center), Vector3());
			e.shape = FlowBackend::Emitter::SHAPE_BOX;
			e.half_size[0] = half;
			e.half_size[1] = thick;
			e.half_size[2] = half;
			r_emitters.push_back(e);
		}
	}
}

void PhysXFlow3D::_step(double p_delta) {
	if (grid == nullptr) {
		return;
	}
#ifdef TOOLS_ENABLED
	// The editor's copy holds still while the project runs from the editor:
	// the game simulates its own, and this one would only take GPU time (and
	// keep the editor redrawing) in the background.
	if (Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton() != nullptr && EditorInterface::get_singleton()->is_playing_scene()) {
		return;
	}
#endif
	LocalVector<FlowBackend::Emitter> emitters;
	AABB emitter_bounds;
	_gather_emitters(emitters, emitter_bounds);
	draw_center = emitters.is_empty() ? get_global_position() : emitter_bounds.get_center();
	{
		// Heat shimmer only where an emitter brings heat (or fuel to burn): dust and mist skip the pass.
		bool hot = false;
		for (const FlowBackend::Emitter &e : emitters) {
			hot = hot || e.temperature > 0.0f || (combustion && e.fuel > 0.0f);
		}
		const WorldWeather *wm = WorldContracts::weather();
		const float air_c = wm != nullptr ? wm->air_temperature_at(draw_center) : 15.0f;
		MutexLock lock(scatter_mutex);
		traced_shimmer_kelvin = (heat_shimmer && hot) ? heat_shimmer_kelvin : 0.0f;
		traced_air_kelvin = air_c + 273.15f;
		traced_shimmer_turbulence = heat_shimmer_turbulence;
		traced_shimmer_time = (float)Math::fmod(sim_time, 3600.0);
	}
	// Solids near where the gas comes from -- and where it has spread to,
	// when it's read back.
	AABB region = emitters.is_empty() ? AABB() : emitter_bounds.grow(collision_range);
	AABB gas;
	bool full = false;
	{
		MutexLock lock(step_mutex);
		gas = gas_bounds;
		full = blocks_full;
	}
	if (gas.has_volume()) {
		region = region.has_volume() ? region.merge(gas.grow(1.0)) : gas.grow(1.0);
	}
	_gather_colliders(region, emitters, (float)p_delta);
	for (uint32_t i = 0; i < transient_solids.size(); i++) {
		emitters.push_back(transient_solids[i]);
	}
	transient_solids.clear();
	_gather_terrain_floor(region, emitters);
	_gather_weather_wind(region, emitters);
	_gather_scatter_lights();
	if (floor_enabled) {
		// The FLOOR: one wide solid box whose top is floor_height, under everything this grid can reach. A solid
		// reserves no blocks and holds no gas, so whatever sinks below the floor is gone (and its blocks with it).
		constexpr float FLOOR_HALF_W = 4096.0f;
		constexpr float FLOOR_HALF_T = 64.0f;
		FlowBackend::Emitter floor_e = make_solid(Transform3D(Basis(), Vector3(draw_center.x, floor_height - FLOOR_HALF_T, draw_center.z)), Vector3());
		floor_e.shape = FlowBackend::Emitter::SHAPE_BOX;
		floor_e.half_size[0] = FLOOR_HALF_W;
		floor_e.half_size[1] = FLOOR_HALF_T;
		floor_e.half_size[2] = FLOOR_HALF_W;
		emitters.push_back(floor_e);
	}

	FlowBackend::Settings settings;
	settings.cell_size = cell_size;
	settings.buoyancy = buoyancy;
	settings.smoke_buoyancy = smoke_buoyancy;
	settings.cooling_rate = cooling_rate;
	settings.smoke_fade = smoke_fade;
	{
		const WorldWeather *wm = WorldContracts::weather();
		washout_rate = (rain_washout && wm != nullptr && wm->has_provider()) ? wm->washout_at(draw_center, smoke_buoyancy < 0.0f) : 0.0f;
		// Flow's damping is the share lost per second: keep (1 - 0.3) e^-Lambda.
		settings.smoke_damping = 1.0f - (1.0f - 0.3f) * Math::exp(-washout_rate);
	}
	settings.vorticity = vorticity;
	settings.ignition_temperature = ignition_temperature;
	settings.combustion = combustion;
	// A full field each time; often enough for queries, and it stalls
	// RenderingDevice's staging memory when done every step on large gas.
	const bool want_readback = cpu_readback || _velocity_wanted();
	settings.readback = want_readback && (step_count++ % READBACK_INTERVAL) == 0;
	_fill_render_settings(settings);
	// The project's gravity sets which way is "up" for hot gas.
	const Vector3 gravity_vector = GLOBAL_GET("physics/3d/default_gravity_vector");
	const float gravity_strength = GLOBAL_GET("physics/3d/default_gravity");
	const Vector3 g = gravity_vector * gravity_strength;
	settings.gravity[0] = g.x;
	settings.gravity[1] = g.y;
	settings.gravity[2] = g.z;

	{
		MutexLock lock(step_mutex);
		steps->queue.push_back(StepQueue::Step());
		StepQueue::Step &q = steps->queue[steps->queue.size() - 1];
		q.time = sim_time + p_delta;
		q.delta = (float)p_delta;
		q.readback = want_readback;
		q.settings = settings;
		q.emitters = emitters;
	}
	_post(callable_mp(this, &PhysXFlow3D::_rt_step));
	sim_time += p_delta;
	if (!max_blocks_warned && full) {
		max_blocks_warned = true;
		WARN_PRINT(vformat("PhysXFlow3D %s: the gas has used all %d max_blocks and is being cut off; raise max_blocks (more GPU memory) or make the effect smaller.", get_path(), max_blocks));
	}
	if (get_world_3d().is_valid() && attached_world.is_valid()) {
		PhysXFlowRenderEffect::install(get_world_3d().ptr(), get_viewport() ? get_viewport()->get_camera_3d() : nullptr);
	}
	if (Engine::get_singleton()->is_editor_hint()) {
		// Nothing the RenderingServer sees changes when the gas moves, and the
		// editor only redraws its viewports on changes.
		RenderingServerDefault::redraw_request();
	}
}

void PhysXFlow3D::_read_back() {
	const uint8_t *temp = nullptr;
	const uint8_t *smoke = nullptr;
	uint64_t temp_size = 0;
	uint64_t smoke_size = 0;
	if (!FlowBackend::grid_get_readback(grid, &temp, &temp_size, &smoke, &smoke_size)) {
		return;
	}
	// (Render thread, step_mutex held.) Copied: the mapped readback is the
	// GPU's to overwrite a few frames on, while the main thread samples it.
	stat_readback_bytes = (int64_t)(temp_size + smoke_size);
	if (smoke == last_smoke_source) {
		return; // the same readback as last step -- the newer one isn't back yet
	}
	last_smoke_source = smoke;
	last_smoke.resize(smoke != nullptr ? (int64_t)smoke_size : 0);
	if (smoke != nullptr && smoke_size > 0) {
		memcpy(last_smoke.ptrw(), smoke, smoke_size);
	}
	last_temperature.resize(temp != nullptr ? (int64_t)temp_size : 0);
	if (temp != nullptr && temp_size > 0) {
		memcpy(last_temperature.ptrw(), temp, temp_size);
	}
	const uint8_t *vel = nullptr;
	uint64_t vel_size = 0;
	if (FlowBackend::grid_get_velocity_readback(grid, &vel, &vel_size) && vel_size > 0) {
		last_velocity.resize((int64_t)vel_size);
		memcpy(last_velocity.ptrw(), vel, vel_size);
		stat_readback_bytes += (int64_t)vel_size;
	} else {
		last_velocity.clear();
	}
	float lo[3];
	float hi[3];
	if (FlowBackend::nanovdb_bounds(smoke, smoke_size, lo, hi)) {
		gas_bounds = AABB(Vector3(lo[0], lo[1], lo[2]), Vector3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]));
	}
}

void PhysXFlow3D::set_max_blocks(int p_v) {
	p_v = CLAMP(p_v, 64, 16384);
	if (p_v == max_blocks) {
		return;
	}
	max_blocks = p_v;
	// The budget is fixed when the grid is made: start over with the new one.
	if (grid != nullptr) {
		_stop();
		_start();
	}
}

void PhysXFlow3D::set_cpu_readback(bool p_v) {
	cpu_readback = p_v;
	if (!cpu_readback) {
		MutexLock lock(step_mutex);
		last_smoke.clear();
		last_temperature.clear();
		last_velocity.clear();
		last_smoke_source = nullptr;
		gas_bounds = AABB();
		stat_readback_bytes = 0;
	}
}

float PhysXFlow3D::_sample_field(const Vector<uint8_t> &p_field, const Vector3 &p_position) const {
	MutexLock lock(step_mutex);
	if (!cpu_readback || p_field.is_empty()) {
		return -1.0f;
	}
	const float origin[3] = { (float)p_position.x - 0.5f * cell_size, (float)p_position.y - 0.5f * cell_size, (float)p_position.z - 0.5f * cell_size };
	const int dims[3] = { 1, 1, 1 };
	float value = 0.0f;
	FlowBackend::sample_dense(p_field.ptr(), (uint64_t)p_field.size(), origin, cell_size, dims, 0, 1, &value, 1);
	return value;
}

float PhysXFlow3D::sample_smoke(const Vector3 &p_position) const {
	return _sample_field(last_smoke, p_position);
}

float PhysXFlow3D::sample_temperature(const Vector3 &p_position) const {
	return _sample_field(last_temperature, p_position);
}

bool PhysXFlow3D::_velocity_wanted() const {
	return (push_bodies && bodies_near) || OS::get_singleton()->get_ticks_msec() - velocity_wanted_msec < 1000;
}

void PhysXFlow3D::request_velocity() {
	velocity_wanted_msec = OS::get_singleton()->get_ticks_msec();
}

void PhysXFlow3D::add_transient_solid(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity) {
	if (transient_solids.size() >= 256) {
		return;
	}
	FlowBackend::Emitter e = make_solid(Transform3D(Basis(), p_position), p_velocity);
	e.shape = FlowBackend::Emitter::SHAPE_SPHERE;
	e.radius = MAX(p_radius, 0.01f);
	transient_solids.push_back(e);
}

Vector3 PhysXFlow3D::sample_velocity(const Vector3 &p_position) const {
	MutexLock lock(step_mutex);
	if (last_velocity.is_empty()) {
		return Vector3();
	}
	const float p[3] = { (float)p_position.x, (float)p_position.y, (float)p_position.z };
	float v[3];
	FlowBackend::sample_velocity(last_velocity.ptr(), (uint64_t)last_velocity.size(), p, v);
	return Vector3(v[0], v[1], v[2]);
}

Dictionary PhysXFlow3D::get_stats() const {
	Dictionary d;
	d["running"] = grid != nullptr;
	d["error"] = device_error;
	d["start_ms"] = stat_start_ms;
	d["step_ms"] = stat_step_ms;
	d["render_calls"] = stat_render_calls;
	d["render_outputs"] = stat_render_outputs;
	d["readback_bytes"] = stat_readback_bytes;
	{
		MutexLock lock(step_mutex);
		d["bounds"] = gas_bounds;
	}
	d["scatter_lights"] = stat_scatter_lights;
	d["scatter_passes"] = stat_scatter_passes;
	d["scatter_no_source"] = stat_scatter_no_source;
	int pipelines = 0;
	int failures = 0;
	int passes = 0;
	int buffers = 0;
	int textures = 0;
	uint64_t frame = 0;
	uint64_t completed = 0;
	if (FlowBackend::grid_rd_stats(grid, pipelines, failures, passes, buffers, textures, frame, completed)) {
		d["pipelines"] = pipelines;
		d["pipeline_failures"] = failures;
		d["passes"] = passes;
		d["buffers"] = buffers;
		d["textures"] = textures;
		d["frame"] = frame;
		d["frame_completed"] = completed;
		d["active_blocks"] = FlowBackend::grid_active_blocks(grid);
	}
	return d;
}

void PhysXFlow3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_EXIT_TREE: {
			if (p_what == NOTIFICATION_ENTER_TREE) {
				add_to_group(GROUP);
				_flow_lights_watch(get_tree());
			} else {
				remove_from_group(GROUP);
				_flow_lights_unwatch();
			}
			// Which flow an emitter without a `flow` path feeds depends on how
			// many there are.
			if (is_inside_tree()) {
				for (Node *n : get_tree()->get_nodes_in_group(PhysXFlowEmitter3D::GROUP)) {
					n->update_configuration_warnings();
				}
			}
		} break;
		case NOTIFICATION_ENTER_WORLD: {
			_start();
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			set_process_internal(false);
			_stop();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// Once per frame, as Flow is meant to be driven: its own fixed-rate
			// stepper takes the frame time and steps at most once. Driven by
			// physics ticks instead, a slow frame is followed by several steps
			// in the next, each uploading Flow's sparse table again -- with a
			// few grids that runs RenderingDevice's staging memory out, which
			// stalls the frame, which makes for more ticks, and so on.
			_step(get_process_delta_time());
			_update_fire(get_process_delta_time());
		} break;
	}
}

// Fire: where emitters feed fuel (with combustion on) or gas hotter than ignition_temperature.
// - Fire light: the gas's glow lights nothing in raster, so a point light stands in the flames, as engines light
//   fire (Unreal's Niagara light renderer, Frostbite). It pulses at the flame's puffing frequency, f = 1.5 / sqrt(D)
//   Hz for a base D metres across (Cetegen and Ahmed 1993, "Experiments on the periodic instability of buoyant
//   plumes and pool fires", Combustion and Flame 93): about 2 Hz for a campfire. The path tracer keeps it too, as the
//   fire's light source for next event estimation (a proxy light for an emissive volume): the glow is met only by
//   chance on bounce rays, too rarely to light the ground at a few samples a pixel. The traced glow then shows only to
//   camera and mirror rays, so the fire is not counted twice (RTVolume::emission_proxy_light).
// - Embers: GPU particles thrown up from the fire's base that cool along their flight, their colour the blackbody
//   colour of their temperature (about 1300 K yellow-orange to 800 K dull red), drawn as emissive billboards.
void PhysXFlow3D::_update_fire(double p_delta) {
	const bool editor = Engine::get_singleton()->is_editor_hint();
	Vector3 centre;
	float weight = 0.0f, diameter = 0.0f;
	if ((fire_light || embers) && !editor && is_inside_tree()) {
		for (Node *n : get_tree()->get_nodes_in_group(PhysXFlowEmitter3D::GROUP)) {
			PhysXFlowEmitter3D *e = Object::cast_to<PhysXFlowEmitter3D>(n);
			if (e == nullptr || !e->is_enabled() || !e->is_visible_in_tree() || e->get_flow() != this) {
				continue;
			}
			const bool burning = (combustion && e->get_fuel() > 0.0f) || e->get_temperature() >= MAX(ignition_temperature, 0.05f);
			if (!burning) {
				continue;
			}
			const Vector3 s = e->get_global_transform().basis.get_scale();
			const float d = e->get_shape() == PhysXFlowEmitter3D::SHAPE_BOX ? 0.5f * (e->get_size().x * s.x + e->get_size().z * s.z) : 2.0f * e->get_radius() * MAX(s.x, s.z);
			const float w = MAX(e->get_fuel() + e->get_temperature(), 0.01f) * MAX(d, 0.01f);
			centre += e->get_global_position() * w;
			diameter += d * w;
			weight += w;
		}
	}
	const bool any = weight > 0.0f;
	if (any) {
		centre /= weight;
		diameter = MAX(diameter / weight, 0.05f);
	}
	fire_clock += p_delta;

	const bool light_on = any && fire_light && fire_light_energy > 0.0f;
	{
		MutexLock lock(scatter_mutex);
		traced_proxy_light = light_on;
	}
	if (light_on && fire_light_node == nullptr) {
		fire_light_node = memnew(OmniLight3D);
		fire_light_node->set_shadow(true);
		fire_light_node->set_param(Light3D::PARAM_SHADOW_BIAS, 0.05f);
		// It stands in the flames, inside the smoke: at full strength its inverse-square near field lights the gas
		// round it brighter than the flame's own glow and washes the tongues into an orange haze (first campfire
		// shots). Smoke above a fire is lit by it only faintly next to the glow, so it scatters a tenth.
		fire_light_node->set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 0.1f);
		add_child(fire_light_node, false, INTERNAL_MODE_BACK);
	}
	if (fire_light_node) {
		fire_light_node->set_visible(light_on);
	}
	if (light_on) {
		// Flames stand about twice their base's width above it; the light sits in their brightest part.
		fire_light_node->set_global_position(centre + Vector3(0.0f, 0.8f * diameter, 0.0f));
		const float f = 1.5f / Math::sqrt(diameter);
		const double t = fire_clock * Math::TAU * f;
		const float pulse = 1.0f + 0.16f * Math::sin(t) + 0.08f * Math::sin(1.73 * t + 1.3) + 0.05f * Math::sin(3.11 * t + 0.4);
		// Flame colour about 1,800 K, as the default heat ramp's warm end.
		Color c(1.0f, 0.52f, 0.2f);
		if (heat_ramp.is_valid() && heat_ramp->get_point_count() > 0) {
			c = heat_ramp->get_color_at_offset(0.65f);
			const float m = MAX(c.r, MAX(c.g, c.b));
			c = m > 0.0f ? Color(c.r / m, c.g / m, c.b / m) : Color(1.0f, 0.52f, 0.2f);
		}
		fire_light_node->set_color(c);
		// Heat release grows with the base's area; a 0.5 m campfire is about 2.5 here.
		const float area = diameter * diameter / 0.25f;
		fire_light_node->set_param(Light3D::PARAM_ENERGY, fire_light_energy * 2.5f * MIN(area, 16.0f) * pulse);
		fire_light_node->set_param(Light3D::PARAM_RANGE, 5.0f + 12.0f * diameter);
		// Flames are a broad source, not a point: the shadows of logs and stones round them are soft.
		fire_light_node->set_param(Light3D::PARAM_SIZE, 0.6f * diameter);
	}

	const bool embers_on = any && embers && ember_rate > 0.0f;
	if (embers_on && ember_node == nullptr) {
		ember_node = memnew(GPUParticles3D);
		ember_node->set_use_local_coordinates(false);
		ember_node->set_lifetime(3.0);
		Ref<ParticleProcessMaterial> pm;
		pm.instantiate();
		pm->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_SPHERE);
		pm->set_direction(Vector3(0, 1, 0));
		pm->set_spread(22.0f);
		pm->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.8f);
		pm->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 2.6f);
		// Carried up by the plume, then falling as they cool: a weak net gravity and air drag.
		pm->set_gravity(Vector3(0, -0.6f, 0));
		pm->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.2f);
		pm->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 0.6f);
		pm->set_turbulence_enabled(true);
		pm->set_turbulence_noise_strength(1.2f);
		pm->set_turbulence_noise_scale(1.5f);
		pm->set_param_min(ParticleProcessMaterial::PARAM_TURB_INFLUENCE_OVER_LIFE, 0.05f);
		pm->set_param_max(ParticleProcessMaterial::PARAM_TURB_INFLUENCE_OVER_LIFE, 0.15f);
		// Sizes from fine sparks to the odd large flake, and a spread of starting temperatures.
		pm->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.4f);
		pm->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 1.5f);
		pm->set_param_min(ParticleProcessMaterial::PARAM_HUE_VARIATION, -0.025f);
		pm->set_param_max(ParticleProcessMaterial::PARAM_HUE_VARIATION, 0.02f);
		pm->set_lifetime_randomness(0.6);
		// Each spark's Y axis follows its velocity: drawn as a streak along its flight, the motion blur a camera
		// shutter gives a fast spark (velocity-aligned sprites, as Unreal's Niagara and film spark passes draw them).
		pm->set_particle_flag(ParticleProcessMaterial::PARTICLE_FLAG_ALIGN_Y_TO_VELOCITY, true);
		// Blackbody colours of a cooling ember, 1,300 K yellow-orange to 800 K dull red. Alpha is brightness: it
		// falls with temperature as the visible blackbody radiance does (Wien), so cooled embers are dim red points.
		Ref<Gradient> g;
		g.instantiate();
		// Linear RGB of the blackbody's sRGB chromaticity: 1,300 K (1, 0.36, 0), 1,000 K (1, 0.22, 0), 800 K (1, 0.16, 0).
		g->set_color(0, Color(1.0f, 0.11f, 0.004f, 1.0f));
		g->set_color(1, Color(1.0f, 0.02f, 0.0f, 0.0f));
		g->add_point(0.25f, Color(1.0f, 0.075f, 0.0f, 0.45f));
		g->add_point(0.6f, Color(1.0f, 0.04f, 0.0f, 0.12f));
		Ref<GradientTexture1D> gt;
		gt.instantiate();
		gt->set_gradient(g);
		pm->set_color_ramp(gt);
		ember_node->set_process_material(pm);
		Ref<QuadMesh> quad;
		quad.instantiate();
		quad->set_size(Vector2(1.0f, 1.0f));
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(R"(shader_type spatial;
render_mode cull_disabled, shadows_disabled;
uniform float energy = 2.5;
uniform float width = 0.004;
uniform float streak_length = 0.03;
void vertex() {
	// A streak facing the camera around the spark's velocity (its Y axis).
	vec3 pos = MODEL_MATRIX[3].xyz;
	float s = length(MODEL_MATRIX[0].xyz);
	vec3 axis = normalize(MODEL_MATRIX[1].xyz);
	vec3 to_cam = normalize(INV_VIEW_MATRIX[3].xyz - pos);
	vec3 side = normalize(cross(axis, to_cam));
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(vec4(side * width * s, 0.0), vec4(axis * streak_length * s, 0.0), vec4(cross(side, axis), 0.0), vec4(pos, 1.0));
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
}
void fragment() {
	vec2 q = (UV - 0.5) * 2.0;
	if (length(q) > 1.0 || COLOR.a < 0.004) {
		discard;
	}
	ALBEDO = vec3(0.0);
	// Brightest at the head (UV.y 0, the way it flies), fading along the tail (where it was).
	EMISSION = COLOR.rgb * energy * COLOR.a * mix(1.0, 0.25, UV.y);
}
)");
		Ref<ShaderMaterial> sm;
		sm.instantiate();
		sm->set_shader(sh);
		quad->set_material(sm);
		ember_node->set_draw_pass_mesh(0, quad);
		ember_node->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(ember_node, false, INTERNAL_MODE_BACK);
	}
	if (ember_node) {
		ember_node->set_emitting(embers_on);
		ember_node->set_visible(embers_on || ember_node->is_emitting());
	}
	if (embers_on) {
		ember_node->set_global_position(centre);
		const int amount = CLAMP(int(Math::ceil(ember_rate * 3.0f)), 1, 20000);
		if (ember_node->get_amount() != amount) {
			ember_node->set_amount(amount);
		}
		Ref<ParticleProcessMaterial> pm = ember_node->get_process_material();
		if (pm.is_valid()) {
			pm->set_emission_sphere_radius(0.35f * diameter);
		}
		ember_node->set_visibility_aabb(AABB(Vector3(-4, -1, -4), Vector3(8, 9, 8)));
	}
}

PackedStringArray PhysXFlow3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!device_error.is_empty()) {
		warnings.push_back(vformat(RTR("NVIDIA Flow isn't running: %s"), device_error));
	}
	return warnings;
}

void PhysXFlow3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &PhysXFlow3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &PhysXFlow3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_buoyancy", "buoyancy"), &PhysXFlow3D::set_buoyancy);
	ClassDB::bind_method(D_METHOD("get_buoyancy"), &PhysXFlow3D::get_buoyancy);
	ClassDB::bind_method(D_METHOD("set_smoke_buoyancy", "buoyancy"), &PhysXFlow3D::set_smoke_buoyancy);
	ClassDB::bind_method(D_METHOD("get_smoke_buoyancy"), &PhysXFlow3D::get_smoke_buoyancy);
	ClassDB::bind_method(D_METHOD("set_cooling_rate", "rate"), &PhysXFlow3D::set_cooling_rate);
	ClassDB::bind_method(D_METHOD("get_cooling_rate"), &PhysXFlow3D::get_cooling_rate);
	ClassDB::bind_method(D_METHOD("set_smoke_fade", "fade"), &PhysXFlow3D::set_smoke_fade);
	ClassDB::bind_method(D_METHOD("get_smoke_fade"), &PhysXFlow3D::get_smoke_fade);
	ClassDB::bind_method(D_METHOD("set_rain_washout", "enabled"), &PhysXFlow3D::set_rain_washout);
	ClassDB::bind_method(D_METHOD("is_rain_washout"), &PhysXFlow3D::is_rain_washout);
	ClassDB::bind_method(D_METHOD("get_washout_rate"), &PhysXFlow3D::get_washout_rate);
	ClassDB::bind_method(D_METHOD("set_vorticity", "vorticity"), &PhysXFlow3D::set_vorticity);
	ClassDB::bind_method(D_METHOD("get_vorticity"), &PhysXFlow3D::get_vorticity);
	ClassDB::bind_method(D_METHOD("set_ignition_temperature", "temperature"), &PhysXFlow3D::set_ignition_temperature);
	ClassDB::bind_method(D_METHOD("get_ignition_temperature"), &PhysXFlow3D::get_ignition_temperature);
	ClassDB::bind_method(D_METHOD("set_combustion", "enabled"), &PhysXFlow3D::set_combustion);
	ClassDB::bind_method(D_METHOD("is_combustion"), &PhysXFlow3D::is_combustion);
	ClassDB::bind_method(D_METHOD("set_collide_with_bodies", "enabled"), &PhysXFlow3D::set_collide_with_bodies);
	ClassDB::bind_method(D_METHOD("is_collide_with_bodies"), &PhysXFlow3D::is_collide_with_bodies);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PhysXFlow3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PhysXFlow3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_range", "range"), &PhysXFlow3D::set_collision_range);
	ClassDB::bind_method(D_METHOD("set_floor_enabled", "enabled"), &PhysXFlow3D::set_floor_enabled);
	ClassDB::bind_method(D_METHOD("is_floor_enabled"), &PhysXFlow3D::is_floor_enabled);
	ClassDB::bind_method(D_METHOD("set_weather_wind", "enabled"), &PhysXFlow3D::set_weather_wind);
	ClassDB::bind_method(D_METHOD("is_weather_wind"), &PhysXFlow3D::is_weather_wind);
	ClassDB::bind_method(D_METHOD("set_weather_wind_coupling", "rate"), &PhysXFlow3D::set_weather_wind_coupling);
	ClassDB::bind_method(D_METHOD("get_weather_wind_coupling"), &PhysXFlow3D::get_weather_wind_coupling);
	ClassDB::bind_method(D_METHOD("set_terrain_floor", "enabled"), &PhysXFlow3D::set_terrain_floor);
	ClassDB::bind_method(D_METHOD("is_terrain_floor"), &PhysXFlow3D::is_terrain_floor);
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &PhysXFlow3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &PhysXFlow3D::get_terrain_path);
	ClassDB::bind_method(D_METHOD("set_terrain_floor_spacing", "spacing"), &PhysXFlow3D::set_terrain_floor_spacing);
	ClassDB::bind_method(D_METHOD("get_terrain_floor_spacing"), &PhysXFlow3D::get_terrain_floor_spacing);
	ClassDB::bind_method(D_METHOD("set_floor_height", "height"), &PhysXFlow3D::set_floor_height);
	ClassDB::bind_method(D_METHOD("get_floor_height"), &PhysXFlow3D::get_floor_height);
	ClassDB::bind_method(D_METHOD("get_collision_range"), &PhysXFlow3D::get_collision_range);
	ClassDB::bind_method(D_METHOD("set_smoke_density", "density"), &PhysXFlow3D::set_smoke_density);
	ClassDB::bind_method(D_METHOD("get_smoke_density"), &PhysXFlow3D::get_smoke_density);
	ClassDB::bind_method(D_METHOD("set_smoke_color", "color"), &PhysXFlow3D::set_smoke_color);
	ClassDB::bind_method(D_METHOD("get_smoke_color"), &PhysXFlow3D::get_smoke_color);
	ClassDB::bind_method(D_METHOD("set_smoke_ramp", "ramp"), &PhysXFlow3D::set_smoke_ramp);
	ClassDB::bind_method(D_METHOD("get_smoke_ramp"), &PhysXFlow3D::get_smoke_ramp);
	ClassDB::bind_method(D_METHOD("set_heat_ramp", "ramp"), &PhysXFlow3D::set_heat_ramp);
	ClassDB::bind_method(D_METHOD("get_heat_ramp"), &PhysXFlow3D::get_heat_ramp);
	ClassDB::bind_method(D_METHOD("set_temperature_range", "range"), &PhysXFlow3D::set_temperature_range);
	ClassDB::bind_method(D_METHOD("get_temperature_range"), &PhysXFlow3D::get_temperature_range);
	ClassDB::bind_method(D_METHOD("set_fire_light", "enabled"), &PhysXFlow3D::set_fire_light);
	ClassDB::bind_method(D_METHOD("is_fire_light"), &PhysXFlow3D::is_fire_light);
	ClassDB::bind_method(D_METHOD("set_fire_light_energy", "energy"), &PhysXFlow3D::set_fire_light_energy);
	ClassDB::bind_method(D_METHOD("get_fire_light_energy"), &PhysXFlow3D::get_fire_light_energy);
	ClassDB::bind_method(D_METHOD("set_embers", "enabled"), &PhysXFlow3D::set_embers);
	ClassDB::bind_method(D_METHOD("is_embers"), &PhysXFlow3D::is_embers);
	ClassDB::bind_method(D_METHOD("set_ember_rate", "rate"), &PhysXFlow3D::set_ember_rate);
	ClassDB::bind_method(D_METHOD("get_ember_rate"), &PhysXFlow3D::get_ember_rate);
	ClassDB::bind_method(D_METHOD("set_emission_strength", "strength"), &PhysXFlow3D::set_emission_strength);
	ClassDB::bind_method(D_METHOD("get_emission_strength"), &PhysXFlow3D::get_emission_strength);
	ClassDB::bind_method(D_METHOD("set_shadow_steps", "steps"), &PhysXFlow3D::set_shadow_steps);
	ClassDB::bind_method(D_METHOD("get_shadow_steps"), &PhysXFlow3D::get_shadow_steps);
	ClassDB::bind_method(D_METHOD("set_light_scattering", "enabled"), &PhysXFlow3D::set_light_scattering);
	ClassDB::bind_method(D_METHOD("is_light_scattering"), &PhysXFlow3D::is_light_scattering);
	ClassDB::bind_method(D_METHOD("set_ambient_scattering", "enabled"), &PhysXFlow3D::set_ambient_scattering);
	ClassDB::bind_method(D_METHOD("is_ambient_scattering"), &PhysXFlow3D::is_ambient_scattering);
	ClassDB::bind_method(D_METHOD("set_sun_scattering", "enabled"), &PhysXFlow3D::set_sun_scattering);
	ClassDB::bind_method(D_METHOD("is_sun_scattering"), &PhysXFlow3D::is_sun_scattering);
	ClassDB::bind_method(D_METHOD("set_heat_shimmer", "enabled"), &PhysXFlow3D::set_heat_shimmer);
	ClassDB::bind_method(D_METHOD("is_heat_shimmer"), &PhysXFlow3D::is_heat_shimmer);
	ClassDB::bind_method(D_METHOD("set_heat_shimmer_kelvin", "kelvin"), &PhysXFlow3D::set_heat_shimmer_kelvin);
	ClassDB::bind_method(D_METHOD("get_heat_shimmer_kelvin"), &PhysXFlow3D::get_heat_shimmer_kelvin);
	ClassDB::bind_method(D_METHOD("set_heat_shimmer_turbulence", "turbulence"), &PhysXFlow3D::set_heat_shimmer_turbulence);
	ClassDB::bind_method(D_METHOD("get_heat_shimmer_turbulence"), &PhysXFlow3D::get_heat_shimmer_turbulence);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enabled"), &PhysXFlow3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_cast_shadows"), &PhysXFlow3D::is_cast_shadows);
	ClassDB::bind_method(D_METHOD("set_scattering_temporal", "enabled"), &PhysXFlow3D::set_scattering_temporal);
	ClassDB::bind_method(D_METHOD("is_scattering_temporal"), &PhysXFlow3D::is_scattering_temporal);
	ClassDB::bind_method(D_METHOD("set_scattering_max_lights", "count"), &PhysXFlow3D::set_scattering_max_lights);
	ClassDB::bind_method(D_METHOD("get_scattering_max_lights"), &PhysXFlow3D::get_scattering_max_lights);
	ClassDB::bind_method(D_METHOD("set_scattering_strength", "strength"), &PhysXFlow3D::set_scattering_strength);
	ClassDB::bind_method(D_METHOD("get_scattering_strength"), &PhysXFlow3D::get_scattering_strength);
	ClassDB::bind_method(D_METHOD("set_scattering_anisotropy", "anisotropy"), &PhysXFlow3D::set_scattering_anisotropy);
	ClassDB::bind_method(D_METHOD("get_scattering_anisotropy"), &PhysXFlow3D::get_scattering_anisotropy);
	ClassDB::bind_method(D_METHOD("set_scattering_albedo", "albedo"), &PhysXFlow3D::set_scattering_albedo);
	ClassDB::bind_method(D_METHOD("get_scattering_albedo"), &PhysXFlow3D::get_scattering_albedo);
	ClassDB::bind_method(D_METHOD("set_ground_albedo", "albedo"), &PhysXFlow3D::set_ground_albedo);
	ClassDB::bind_method(D_METHOD("get_ground_albedo"), &PhysXFlow3D::get_ground_albedo);
	ClassDB::bind_method(D_METHOD("set_scattering_shadow_steps", "steps"), &PhysXFlow3D::set_scattering_shadow_steps);
	ClassDB::bind_method(D_METHOD("get_scattering_shadow_steps"), &PhysXFlow3D::get_scattering_shadow_steps);
	ClassDB::bind_method(D_METHOD("set_scattering_resolution", "resolution"), &PhysXFlow3D::set_scattering_resolution);
	ClassDB::bind_method(D_METHOD("get_scattering_resolution"), &PhysXFlow3D::get_scattering_resolution);
	ClassDB::bind_method(D_METHOD("set_scattering_receive_shadows", "flags"), &PhysXFlow3D::set_scattering_receive_shadows);
	ClassDB::bind_method(D_METHOD("get_scattering_receive_shadows"), &PhysXFlow3D::get_scattering_receive_shadows);
	ClassDB::bind_method(D_METHOD("set_scattering_shadow_reach", "reach"), &PhysXFlow3D::set_scattering_shadow_reach);
	ClassDB::bind_method(D_METHOD("get_scattering_shadow_reach"), &PhysXFlow3D::get_scattering_shadow_reach);
	ClassDB::bind_method(D_METHOD("set_max_blocks", "blocks"), &PhysXFlow3D::set_max_blocks);
	ClassDB::bind_method(D_METHOD("get_max_blocks"), &PhysXFlow3D::get_max_blocks);
	ClassDB::bind_method(D_METHOD("sample_velocity", "position"), &PhysXFlow3D::sample_velocity);
	ClassDB::bind_method(D_METHOD("request_velocity"), &PhysXFlow3D::request_velocity);
	ClassDB::bind_method(D_METHOD("add_transient_solid", "position", "radius", "velocity"), &PhysXFlow3D::add_transient_solid);
	ClassDB::bind_method(D_METHOD("set_push_bodies", "enabled"), &PhysXFlow3D::set_push_bodies);
	ClassDB::bind_method(D_METHOD("is_push_bodies"), &PhysXFlow3D::is_push_bodies);
	ClassDB::bind_method(D_METHOD("set_body_drag", "drag"), &PhysXFlow3D::set_body_drag);
	ClassDB::bind_method(D_METHOD("get_body_drag"), &PhysXFlow3D::get_body_drag);
	ClassDB::bind_method(D_METHOD("set_cpu_readback", "enabled"), &PhysXFlow3D::set_cpu_readback);
	ClassDB::bind_method(D_METHOD("is_cpu_readback"), &PhysXFlow3D::is_cpu_readback);
	ClassDB::bind_method(D_METHOD("sample_smoke", "position"), &PhysXFlow3D::sample_smoke);
	ClassDB::bind_method(D_METHOD("sample_temperature", "position"), &PhysXFlow3D::sample_temperature);
	ClassDB::bind_method(D_METHOD("is_running"), &PhysXFlow3D::is_running);
	ClassDB::bind_method(D_METHOD("get_stats"), &PhysXFlow3D::get_stats);

	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.005,1,0.001,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_blocks", PROPERTY_HINT_RANGE, "64,16384,1,or_greater"), "set_max_blocks", "get_max_blocks");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "buoyancy", PROPERTY_HINT_RANGE, "-10,10,0.01"), "set_buoyancy", "get_buoyancy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_buoyancy", PROPERTY_HINT_RANGE, "-20,20,0.01,or_less,or_greater"), "set_smoke_buoyancy", "get_smoke_buoyancy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cooling_rate", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_cooling_rate", "get_cooling_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_fade", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_smoke_fade", "get_smoke_fade");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_washout"), "set_rain_washout", "is_rain_washout");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vorticity", PROPERTY_HINT_RANGE, "0,5,0.01"), "set_vorticity", "get_vorticity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "combustion"), "set_combustion", "is_combustion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ignition_temperature", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_ignition_temperature", "get_ignition_temperature");
	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collide_with_bodies"), "set_collide_with_bodies", "is_collide_with_bodies");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "push_bodies"), "set_push_bodies", "is_push_bodies");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "body_drag", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_body_drag", "get_body_drag");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_range", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_collision_range", "get_collision_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "floor_enabled"), "set_floor_enabled", "is_floor_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_height", PROPERTY_HINT_NONE, "suffix:m"), "set_floor_height", "get_floor_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weather_wind"), "set_weather_wind", "is_weather_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "weather_wind_coupling", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:1/s"), "set_weather_wind_coupling", "get_weather_wind_coupling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_floor"), "set_terrain_floor", "is_terrain_floor");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path"), "set_terrain_path", "get_terrain_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_floor_spacing", PROPERTY_HINT_RANGE, "0.05,8,0.05,or_greater,suffix:m"), "set_terrain_floor_spacing", "get_terrain_floor_spacing");
	ADD_GROUP("Look", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_density", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater"), "set_smoke_density", "get_smoke_density");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "smoke_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_smoke_color", "get_smoke_color");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "smoke_ramp", PROPERTY_HINT_RESOURCE_TYPE, "Gradient"), "set_smoke_ramp", "get_smoke_ramp");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "heat_ramp", PROPERTY_HINT_RESOURCE_TYPE, "Gradient"), "set_heat_ramp", "get_heat_ramp");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature_range", PROPERTY_HINT_RANGE, "0.01,20,0.01,or_greater"), "set_temperature_range", "get_temperature_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_strength", PROPERTY_HINT_RANGE, "0,50,0.01,or_greater"), "set_emission_strength", "get_emission_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shadow_steps", PROPERTY_HINT_RANGE, "0,128,1"), "set_shadow_steps", "get_shadow_steps");
	ADD_GROUP("Fire", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fire_light"), "set_fire_light", "is_fire_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fire_light_energy", PROPERTY_HINT_RANGE, "0,8,0.01,or_greater"), "set_fire_light_energy", "get_fire_light_energy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "embers"), "set_embers", "is_embers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ember_rate", PROPERTY_HINT_RANGE, "0,500,0.1,or_greater,suffix:/s"), "set_ember_rate", "get_ember_rate");

	ADD_GROUP("Light Scattering", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "light_scattering"), "set_light_scattering", "is_light_scattering");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ambient_scattering"), "set_ambient_scattering", "is_ambient_scattering");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sun_scattering"), "set_sun_scattering", "is_sun_scattering");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_cast_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "scattering_max_lights", PROPERTY_HINT_RANGE, vformat("1,%d,1", FlowScatterRD::MAX_LIGHTS)), "set_scattering_max_lights", "get_scattering_max_lights");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering_strength", PROPERTY_HINT_RANGE, "0,8,0.01,or_greater"), "set_scattering_strength", "get_scattering_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering_anisotropy", PROPERTY_HINT_RANGE, "-0.95,0.95,0.01"), "set_scattering_anisotropy", "get_scattering_anisotropy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering_albedo", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_scattering_albedo", "get_scattering_albedo");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_albedo", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_ground_albedo", "get_ground_albedo");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "scattering_shadow_steps", PROPERTY_HINT_RANGE, "0,64,1"), "set_scattering_shadow_steps", "get_scattering_shadow_steps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "scattering_receive_shadows", PROPERTY_HINT_FLAGS, "Omni:1,Spot:2"), "set_scattering_receive_shadows", "get_scattering_receive_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "scattering_resolution", PROPERTY_HINT_ENUM, "Full,Half,Quarter"), "set_scattering_resolution", "get_scattering_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "scattering_temporal"), "set_scattering_temporal", "is_scattering_temporal");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering_shadow_reach", PROPERTY_HINT_RANGE, "0.05,20,0.05,or_greater,suffix:m"), "set_scattering_shadow_reach", "get_scattering_shadow_reach");
	BIND_ENUM_CONSTANT(SCATTERING_RESOLUTION_FULL);
	BIND_ENUM_CONSTANT(SCATTERING_RESOLUTION_HALF);
	BIND_ENUM_CONSTANT(SCATTERING_RESOLUTION_QUARTER);
	BIND_BITFIELD_FLAG(SCATTERING_SHADOW_OMNI);
	BIND_BITFIELD_FLAG(SCATTERING_SHADOW_SPOT);
	ADD_GROUP("Heat Shimmer", "heat_shimmer");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heat_shimmer"), "set_heat_shimmer", "is_heat_shimmer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heat_shimmer_kelvin", PROPERTY_HINT_RANGE, "0,3000,1,or_greater,suffix:K"), "set_heat_shimmer_kelvin", "get_heat_shimmer_kelvin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heat_shimmer_turbulence", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater"), "set_heat_shimmer_turbulence", "get_heat_shimmer_turbulence");
	ADD_GROUP("Queries", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cpu_readback"), "set_cpu_readback", "is_cpu_readback");
}

/**************************************************************************/
/*  physx_flow_3d.h                                                       */
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

#pragma once

#include "flow_scatter_rd.h"

#include "core/math/vector2i.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/gradient.h"

class PhysicsServer3D;

class GPUParticles3D;
class OmniLight3D;
class RenderingDevice;
class PhysXFlowRenderEffect;

namespace FlowBackend {
struct Grid;
struct Emitter;
struct Settings;
} //namespace FlowBackend

// NVIDIA Flow smoke / fire / dust: a sparse, unbounded grid (it grows
// wherever the gas goes) fed by the PhysXFlowEmitter3D nodes that point at
// it, with nearby physics bodies as solids.
//
// Flow runs on Godot's own RenderingDevice, and PhysXFlowRenderEffect draws
// it into the frame with Flow's ray marcher (self-shadowed by the scene's
// first DirectionalLight3D) -- nothing goes through the CPU unless
// cpu_readback asks for it.
class PhysXFlow3D : public Node3D {
	GDCLASS(PhysXFlow3D, Node3D);

public:
	static constexpr const char *GROUP = "physx_flows";

private:
	RenderingDevice *rd = nullptr;
	bool library_acquired = false;
	FlowBackend::Grid *grid = nullptr;
	String device_error;
	double sim_time = 0.0;

	// Simulation.
	float cell_size = 0.05f;
	// Sparse blocks the gas may take up. Flow sizes its GPU textures for all
	// of them when the grid is made (~0.45 MB a block), so this is a memory
	// budget: Flow's own default of 4096 is ~1.9 GB per flow.
	int max_blocks = 1024;
	bool max_blocks_warned = false;
	float buoyancy = 2.0f;
	float smoke_buoyancy = 0.0f; // Flow's buoyancyPerSmoke: below 0 the smoke itself is heavy (dust), whatever its temperature
	float cooling_rate = 1.5f;
	float smoke_fade = 0.65f;
	// RAIN WASHOUT: rain and snow over the gas wash its smoke out (WorldWeather::washout_at, coarse when the smoke is
	// heavy dust), on top of Flow's own loss of 0.3 of the smoke a second.
	bool rain_washout = true;
	float washout_rate = 0.0f; // 1/s, the last step's
	float vorticity = 0.6f;
	float ignition_temperature = 0.05f;
	bool combustion = true;

	// Collision: physics bodies within collision_range of this flow's
	// emitters become solids for the gas.
	bool collide_with_bodies = true;
	// GAS PUSHES BODIES (APEX Turbulence's pattern): dynamic bodies in the gas take quadratic drag from its velocity
	// (read back while any are near, or while cloth asks), and cloth adds moving solids so it stirs the gas.
	bool push_bodies = true;
	float body_drag = 1.0f; // drag coefficient, Cd
	bool bodies_near = false; // a dynamic body was in the gas region last step
	uint64_t velocity_wanted_msec = 0; // last request_velocity()
	LocalVector<FlowBackend::Emitter> transient_solids; // add_transient_solid(), consumed by the next step
	bool _velocity_wanted() const;
	uint32_t collision_mask = 1;
	float collision_range = 10.0f;
	// A FLOOR: with floor_enabled, gas never goes below floor_height — a wide solid under it (no blocks reserved there).
	// Colliders can't describe terrain (heightmaps / trimeshes), so a blast's or a fire's gas sank into the ground and
	// spent the grid's blocks there.
	bool floor_enabled = false;
	float floor_height = 0.0f;
	RID query_shape;
	// TERRAIN FLOOR: solid tiles laid on the ground under the gas, each tilted to the local slope, from a terrain
	// node (terrain_path: TerrainDeformer3D, TerrainDataMap3D, TerrainGenerator3D, Terrain3D or anything with
	// get_height) or else a downward ray that only counts static bodies. Ground already made of boxes, spheres or
	// capsules is skipped (those are solids through collide_with_bodies).
	bool terrain_floor = true;
	NodePath terrain_path;
	// AMBIENT WIND (docs/ideas/wind.md): the WeatherMap3D's wind over the gas, as air-only boxes in layers up
	// the gas (the log profile, terrain and gusts sampled at each layer's middle) that reserve no blocks: smoke
	// from a chimney or a campfire leans and drifts with the weather. weather_wind_coupling: per second.
	bool weather_wind = true;
	float weather_wind_coupling = 1.0f;
	void _gather_weather_wind(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters) const;
	float terrain_floor_spacing = 0.5f;
	HashMap<Vector2i, float> terrain_heights; // ground height at grid nodes (NaN: none, or already a collider)
	float terrain_heights_spacing = 0.0f;
	uint64_t terrain_heights_msec = 0;
	void _gather_terrain_floor(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters);
	float _terrain_height_at(const Vector2i &p_node, float p_spacing, float p_top, void *p_query);

	// Look.
	float smoke_density = 4.0f; // per meter, per unit of smoke
	Color smoke_color = Color(0.8, 0.8, 0.8);
	// Smoke color by temperature (0..temperature_range); empty: smoke_color.
	Ref<Gradient> smoke_ramp;
	// Empty: a built-in fire ramp (_default_heat_at).
	Ref<Gradient> heat_ramp;
	static Color _default_heat_at(float p_t);
	float temperature_range = 2.0f;
	float emission_strength = 4.0f;
	// Self-shadowing reach, in steps of 3/4 of a cell: 48 covers ~1.8 m at
	// the default 5 cm cells -- fewer leaves wide gas lit almost evenly.
	int shadow_steps = 48;

	// Optional copy of the gas on the CPU, for sample_smoke(),
	// sample_temperature() and stats:
	// every READBACK_INTERVAL steps (a full field each time).
	bool cpu_readback = false;
	// Fire (see _update_fire): a raster light at the flames and glowing embers.
	bool fire_light = true;
	float fire_light_energy = 1.0f;
	bool embers = true;
	float ember_rate = 40.0f;
	OmniLight3D *fire_light_node = nullptr;
	GPUParticles3D *ember_node = nullptr;
	double fire_clock = 0.0;
	void _update_fire(double p_delta);
	static constexpr int READBACK_INTERVAL = 4;
	uint64_t step_count = 0;
	// Flow's GPU work -- making the grid, each step (simulate + flush +
	// readbacks), destroying it -- runs on the RENDER thread
	// (RenderingServer::call_on_render_thread: in single-threaded rendering
	// that is this thread, at once). The main thread gathers emitters and
	// queues each step's inputs; the render thread steps in order and leaves
	// the newest readback here, copied. step_mutex guards the queue and all
	// of these.
	struct StepQueue;
	StepQueue *steps = nullptr;
	Mutex step_mutex;
	Vector<uint8_t> last_smoke; // copies of the newest readback (NanoVDB)
	Vector<uint8_t> last_temperature;
	Vector<uint8_t> last_velocity;
	const uint8_t *last_smoke_source = nullptr; // the mapped readback they were copied from
	AABB gas_bounds;
	bool blocks_full = false; // seen on the render thread, warned about on this one

	// Stats.
	double stat_start_ms = 0.0;
	double stat_step_ms = 0.0;
	uint64_t stat_render_calls = 0;
	uint64_t stat_render_outputs = 0;
	int64_t stat_readback_bytes = 0;
	// Light scattering: lights picked last step, passes run, frames Flow had no volume to hand over.
	int stat_scatter_lights = 0;
	uint64_t stat_scatter_passes = 0;
	uint64_t stat_scatter_no_source = 0;

	// Where the gas is, roughly (its emitters' middle), for drawing flows
	// back to front. Written by _step, read on the render thread.
	Vector3 draw_center;

	// The first visible DirectionalLight3D, looked up every so often.
	ObjectID light_id;
	int light_search_countdown = 0;

	// Light scattering: point / spot lights glowing in the gas (a pass of its
	// own after Flow's, see FlowScatterRD). It marches every pixel the gas
	// covers once more, with a short march toward each light.
	bool light_scattering = true;
	// Ambient and sky light in the gas, through the same pass (see FlowScatterRD::Params::ambient).
	// Off: Flow's grey ambient floor under its sun lighting.
	bool ambient_scattering = true;
	// The directional lights through the same pass, with their shadow cascades (see
	// FlowScatterRD::Params::directional_count). Off: Flow's colormap carries the first one.
	bool sun_scattering = true;
	// The gas shadows the scene from the first directional light: a volumetric shadow map over the grid
	// (see FlowScatterRD::ShadowMap), read by the Forward+ opaque pass a frame later.
	bool cast_shadows = true;
	// The scattering pass blends each frame with the last ones (see FlowScatterRD::Params::temporal).
	bool scattering_temporal = true;
	// Light bends through the gas's hot air (docs/ideas/heat-shimmer.md): a screen-space pass in raster, bent
	// camera rays under path tracing. heat_shimmer_kelvin: kelvin above the air at a temperature of
	// temperature_range; heat_shimmer_turbulence: rms temperature fluctuation below the grid, as a share of the
	// excess (George, Alpert and Tamanini 1977 measure 0.3 to 0.4 on a plume's axis).
	bool heat_shimmer = true;
	float heat_shimmer_kelvin = 1200.0f;
	float heat_shimmer_turbulence = 0.35f;
	int scattering_max_lights = 4;
	float scattering_strength = 1.0f;
	float scattering_anisotropy = 0.4f;
	float scattering_albedo = 0.9f;
	float ground_albedo = 0.25f;
	int scattering_shadow_steps = 4;
	float scattering_shadow_reach = 4.0f;

public:
	// Which lights' shadows the glow receives (a checkbox list). Directional
	// will be a third flag (4) when the sun's shadow reaches the gas.
	enum ScatteringShadowFlags {
		SCATTERING_SHADOW_OMNI = 1,
		SCATTERING_SHADOW_SPOT = 2,
	};

	enum ScatteringResolution {
		SCATTERING_RESOLUTION_FULL,
		SCATTERING_RESOLUTION_HALF,
		SCATTERING_RESOLUTION_QUARTER,
	};

private:
	ScatteringResolution scattering_resolution = SCATTERING_RESOLUTION_QUARTER;
	uint32_t scattering_receive_shadows = SCATTERING_SHADOW_OMNI | SCATTERING_SHADOW_SPOT;
	// Every OmniLight3D / SpotLight3D in the scene, looked up every so often;
	// the ones lighting the gas this step, for the render thread.
	LocalVector<ObjectID> scatter_light_ids;
	int scatter_search_countdown = 0;
	Mutex scatter_mutex;
	// The path tracer's view of this flow (filled with the render settings, read on the render thread).
	Color traced_heat[8];
	Color traced_smoke_color = Color(0.8, 0.8, 0.8);
	float traced_extinction = 4.0f;
	float traced_anisotropy = 0.4f;
	float traced_emission = 0.0f;
	bool traced_proxy_light = false; // the fire light is on: it carries the glow's lighting under the path tracer
	float traced_temperature_range = 2.0f;
	// Heat shimmer, for both renderers: kelvin per temperature_range (0: off, or no emitter carries heat), the
	// air's kelvin, the turbulence and the simulation's time.
	float traced_shimmer_kelvin = 0.0f;
	float traced_air_kelvin = 288.15f;
	float traced_shimmer_turbulence = 0.35f;
	float traced_shimmer_time = 0.0f;
	bool traced_this_frame = false; // render thread only
	// The sun the gas shadows the scene from (its light RID and the world direction toward it).
	RID shadow_light;
	bool shadow_light_casts = false; // it casts shadows (the gas then shadows the scene from it)
	Vector3 shadow_to_light;
	LocalVector<FlowScatterRD::Light> scatter_lights;
	void _gather_scatter_lights();
	void _fill_scatter_ambient(FlowScatterRD::Params &r_params, const RID &p_environment) const;
	void _fill_scatter_shadows(LocalVector<FlowScatterRD::Light> &r_lights, const PagedArray<RID> *p_instances, const RID &p_shadow_atlas, uint32_t p_flags) const;

	void _start();
	void _stop();
	ObjectID attached_world;
	void _step(double p_delta);
	void _gather_emitters(LocalVector<FlowBackend::Emitter> &r_emitters, AABB &r_emitter_bounds);
	void _gather_colliders(const AABB &p_region, LocalVector<FlowBackend::Emitter> &r_emitters, float p_delta);
	void _push_shape(PhysicsServer3D *p_ps, const RID &p_body, const RID &p_shape, const Transform3D &p_xform, float p_delta);
	void _gather_light(Vector3 &r_direction, Color &r_color, Color &r_ambient);
	void _fill_render_settings(FlowBackend::Settings &r_settings);
	void _read_back();
	float _sample_field(const Vector<uint8_t> &p_field, const Vector3 &p_position) const;
	void _post(const Callable &p_call);
	void _rt_create(int p_max_blocks);
	void _rt_destroy();
	void _rt_step();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_cell_size(float p_v) { cell_size = MAX(p_v, 0.005f); }
	float get_cell_size() const { return cell_size; }
	void set_max_blocks(int p_v);
	int get_max_blocks() const { return max_blocks; }
	void set_buoyancy(float p_v) { buoyancy = p_v; }
	float get_buoyancy() const { return buoyancy; }
	void set_smoke_buoyancy(float p_v) { smoke_buoyancy = p_v; }
	float get_smoke_buoyancy() const { return smoke_buoyancy; }
	void set_cooling_rate(float p_v) { cooling_rate = MAX(p_v, 0.0f); }
	float get_cooling_rate() const { return cooling_rate; }
	void set_smoke_fade(float p_v) { smoke_fade = MAX(p_v, 0.0f); }
	float get_smoke_fade() const { return smoke_fade; }
	void set_rain_washout(bool p_v) { rain_washout = p_v; }
	bool is_rain_washout() const { return rain_washout; }
	float get_washout_rate() const { return washout_rate; }
	void set_vorticity(float p_v) { vorticity = MAX(p_v, 0.0f); }
	float get_vorticity() const { return vorticity; }
	void set_ignition_temperature(float p_v) { ignition_temperature = p_v; }
	float get_ignition_temperature() const { return ignition_temperature; }
	void set_combustion(bool p_v) { combustion = p_v; }
	bool is_combustion() const { return combustion; }

	void set_push_bodies(bool p_v) { push_bodies = p_v; }
	bool is_push_bodies() const { return push_bodies; }
	void set_body_drag(float p_v) { body_drag = MAX(p_v, 0.0f); }
	float get_body_drag() const { return body_drag; }
	// Keeps the gas velocity read back for a second (cloth and other samplers call it each step).
	void request_velocity();
	// A sphere solid moving at p_velocity for the next step only (cloth particles stirring the gas).
	void add_transient_solid(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity);
	void set_collide_with_bodies(bool p_v) { collide_with_bodies = p_v; }
	bool is_collide_with_bodies() const { return collide_with_bodies; }
	void set_collision_mask(uint32_t p_mask) { collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_range(float p_v) { collision_range = MAX(p_v, 0.0f); }
	float get_collision_range() const { return collision_range; }
	void set_floor_enabled(bool p_v) { floor_enabled = p_v; }
	bool is_floor_enabled() const { return floor_enabled; }
	void set_floor_height(float p_v) { floor_height = p_v; }
	float get_floor_height() const { return floor_height; }
	void set_weather_wind(bool p_v) { weather_wind = p_v; }
	bool is_weather_wind() const { return weather_wind; }
	void set_weather_wind_coupling(float p_v) { weather_wind_coupling = MAX(p_v, 0.0f); }
	float get_weather_wind_coupling() const { return weather_wind_coupling; }
	void set_terrain_floor(bool p_v) { terrain_floor = p_v; }
	bool is_terrain_floor() const { return terrain_floor; }
	void set_terrain_path(const NodePath &p_path) {
		terrain_path = p_path;
		terrain_heights.clear();
	}
	NodePath get_terrain_path() const { return terrain_path; }
	void set_terrain_floor_spacing(float p_v) {
		terrain_floor_spacing = MAX(p_v, 0.05f);
		terrain_heights.clear();
	}
	float get_terrain_floor_spacing() const { return terrain_floor_spacing; }

	void set_smoke_density(float p_v) { smoke_density = MAX(p_v, 0.0f); }
	float get_smoke_density() const { return smoke_density; }
	void set_smoke_color(const Color &p_v) { smoke_color = p_v; }
	Color get_smoke_color() const { return smoke_color; }
	void set_smoke_ramp(const Ref<Gradient> &p_ramp) { smoke_ramp = p_ramp; }
	Ref<Gradient> get_smoke_ramp() const { return smoke_ramp; }
	void set_heat_ramp(const Ref<Gradient> &p_ramp) { heat_ramp = p_ramp; }
	Ref<Gradient> get_heat_ramp() const { return heat_ramp; }
	void set_temperature_range(float p_v) { temperature_range = MAX(p_v, 0.001f); }
	float get_temperature_range() const { return temperature_range; }
	void set_fire_light(bool p_v) { fire_light = p_v; }
	bool is_fire_light() const { return fire_light; }
	void set_fire_light_energy(float p_v) { fire_light_energy = MAX(p_v, 0.0f); }
	float get_fire_light_energy() const { return fire_light_energy; }
	void set_embers(bool p_v) { embers = p_v; }
	bool is_embers() const { return embers; }
	void set_ember_rate(float p_v) { ember_rate = MAX(p_v, 0.0f); }
	float get_ember_rate() const { return ember_rate; }
	void set_emission_strength(float p_v) { emission_strength = MAX(p_v, 0.0f); }
	float get_emission_strength() const { return emission_strength; }
	void set_shadow_steps(int p_v) { shadow_steps = CLAMP(p_v, 0, 128); }
	int get_shadow_steps() const { return shadow_steps; }

	void set_light_scattering(bool p_v) { light_scattering = p_v; }
	bool is_light_scattering() const { return light_scattering; }
	void set_ambient_scattering(bool p_v) { ambient_scattering = p_v; }
	bool is_ambient_scattering() const { return ambient_scattering; }
	void set_sun_scattering(bool p_v) { sun_scattering = p_v; }
	bool is_sun_scattering() const { return sun_scattering; }
	void set_heat_shimmer(bool p_v) { heat_shimmer = p_v; }
	bool is_heat_shimmer() const { return heat_shimmer; }
	void set_heat_shimmer_kelvin(float p_v) { heat_shimmer_kelvin = MAX(p_v, 0.0f); }
	float get_heat_shimmer_kelvin() const { return heat_shimmer_kelvin; }
	void set_heat_shimmer_turbulence(float p_v) { heat_shimmer_turbulence = MAX(p_v, 0.0f); }
	float get_heat_shimmer_turbulence() const { return heat_shimmer_turbulence; }
	void set_cast_shadows(bool p_v) { cast_shadows = p_v; }
	bool is_cast_shadows() const { return cast_shadows; }
	void set_scattering_temporal(bool p_v) { scattering_temporal = p_v; }
	bool is_scattering_temporal() const { return scattering_temporal; }
	void set_scattering_max_lights(int p_v) { scattering_max_lights = CLAMP(p_v, 1, FlowScatterRD::MAX_LIGHTS); }
	int get_scattering_max_lights() const { return scattering_max_lights; }
	void set_scattering_strength(float p_v) { scattering_strength = MAX(p_v, 0.0f); }
	float get_scattering_strength() const { return scattering_strength; }
	void set_scattering_anisotropy(float p_v) { scattering_anisotropy = CLAMP(p_v, -0.95f, 0.95f); }
	float get_scattering_anisotropy() const { return scattering_anisotropy; }
	void set_scattering_albedo(float p_v) { scattering_albedo = CLAMP(p_v, 0.0f, 1.0f); }
	float get_scattering_albedo() const { return scattering_albedo; }
	void set_ground_albedo(float p_v) { ground_albedo = CLAMP(p_v, 0.0f, 1.0f); }
	float get_ground_albedo() const { return ground_albedo; }
	void set_scattering_shadow_steps(int p_v) { scattering_shadow_steps = CLAMP(p_v, 0, 64); }
	int get_scattering_shadow_steps() const { return scattering_shadow_steps; }
	void set_scattering_shadow_reach(float p_v) { scattering_shadow_reach = MAX(p_v, 0.05f); }
	float get_scattering_shadow_reach() const { return scattering_shadow_reach; }
	void set_scattering_receive_shadows(uint32_t p_flags) { scattering_receive_shadows = p_flags & (SCATTERING_SHADOW_OMNI | SCATTERING_SHADOW_SPOT); }
	uint32_t get_scattering_receive_shadows() const { return scattering_receive_shadows; }
	void set_scattering_resolution(ScatteringResolution p_v) { scattering_resolution = p_v; }
	ScatteringResolution get_scattering_resolution() const { return scattering_resolution; }

	void set_cpu_readback(bool p_v);
	bool is_cpu_readback() const { return cpu_readback; }
	// Smoke at a world position, from the last CPU readback (needs
	// cpu_readback; -1 without it).
	float sample_smoke(const Vector3 &p_position) const;
	// Temperature at a world position, the same way.
	float sample_temperature(const Vector3 &p_position) const;
	// Gas velocity in m/s (needs cpu_readback; zero without it or outside the gas), a few frames late.
	Vector3 sample_velocity(const Vector3 &p_position) const;

	bool is_running() const { return grid != nullptr; }
	Dictionary get_stats() const;

	// From PhysXFlowRenderEffect, on the render thread.
	Vector3 get_draw_center() const { return draw_center; }
	// p_lights / p_shadow_atlas: the frame's light instances and shadow atlas
	// (RenderDataRD), for the scattering pass's shadows. An XR camera's eyes are
	// drawn one after another in one Flow frame: p_first_view / p_last_view.
	// Under path tracing, before the trace (render thread): prepares the grid and publishes it to the
	// path tracer (RTVolumes). render_rd then only ends the Flow frame.
	bool prepare_traced();
	void render_rd(PhysXFlowRenderEffect *p_effect, RenderingDevice *p_rd, const float p_view[16], const float p_projection[16], const Size2i &p_size, const RID &p_depth, const RID &p_color, const Projection &p_inv_view_projection, const Vector3 &p_camera, const PagedArray<RID> *p_lights, const RID &p_shadow_atlas, const RID &p_environment, uint32_t p_directional_count, const Transform3D &p_world_to_view, bool p_first_view = true, bool p_last_view = true);

	PackedStringArray get_configuration_warnings() const override;

	PhysXFlow3D();
	~PhysXFlow3D();
};

VARIANT_ENUM_CAST(PhysXFlow3D::ScatteringResolution);
VARIANT_BITFIELD_CAST(PhysXFlow3D::ScatteringShadowFlags);

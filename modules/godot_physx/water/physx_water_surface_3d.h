/**************************************************************************/
/*  physx_water_surface_3d.h                                              */
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

#include "modules/world/world_contracts.h"
#include "water_body_contacts.h"
#include "water_contact_map.h"
#include "water_raster_reflections.h"
#include "water_solver.h"

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

class Camera3D;
class Mesh;
class MeshInstance3D;
class Material;
class ShaderMaterial;
class Texture2DRD;
class Texture2D;
class Image;
class ImageTexture;
class DirectionalLight3D;
class ArrayMesh;
class GPUParticles3D;
class ParticleProcessMaterial;

// The real water surface node: owns a WaterSolver (ripple + FFT ocean
// layers), renders it via a plain child MeshInstance3D (a subdivided
// PlaneMesh + a spatial ShaderMaterial that samples the solver's height
// fields), and exposes the sample_height()/submit_sphere()/submit_impulse()
// API buoyant_body.gd (or anything else) talks to.
//
// WaterSolver runs on the SHARED main RenderingDevice (dispatched via the
// render thread, same as MPMFluidSolverGPU) and blits its height fields into
// real RD textures every step. Those textures are wrapped here with
// Texture2DRD (a real, already-public Godot class -- see
// scene/resources/texture_rd.h -- no vanilla-engine changes needed) and fed
// straight to the ShaderMaterial: zero-copy, no CPU round trip, the data
// never leaves the GPU. Set once after the solver becomes available, not
// re-created every frame -- Texture2DRD just wraps an RID by reference, so
// whatever the blit shaders write each step is what the material samples
// automatically.
//
// Buoyancy sampling (sample_height()) is a separate, second consumer of the
// same underlying data with very different needs (CPU-side, a few frames of
// staleness is fine, precision doesn't need to match the rendered mesh
// exactly) -- it still goes through the solver's CPU-cached
// get_height_grid()/get_ocean_height_grid() readback, refreshed periodically
// into a local array this node bilinearly samples. Kept deliberately
// separate from the zero-copy texture path above per the project plan's own
// note: two consumers, two different freshness/precision needs, one
// underlying simulation.
class PhysXWaterSurface3D : public Node3D, public WorldWaterProvider {
	GDCLASS(PhysXWaterSurface3D, Node3D);

	WaterBodyContacts body_contacts; // world_contact bodies push the ripples (docs/ideas/world-links.md)

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	enum NormalMode {
		// Exact normals and foam from the FFT's slopes, per pixel: smooth
		// crests at any mesh density (realistic).
		NORMAL_MODE_PER_PIXEL,
		// Finite differences of the displaced mesh, per vertex: shading
		// follows the mesh (faceted / stylized, and cheaper).
		NORMAL_MODE_PER_VERTEX,
	};
	enum WaveSpreading {
		// cos^2 about the wind with a weak back lobe (the first ocean).
		WAVE_SPREADING_COS2,
		// Donelan-Banner's measured sech^2 spreading (Horvath 2015's default), with his swell term.
		WAVE_SPREADING_DONELAN_BANNER,
		// Hasselmann et al. (1980)'s cos-2s(theta / 2) spreading, with the swell term.
		WAVE_SPREADING_HASSELMANN,
	};

	// When the surface advances: every physics tick (each tick posts the whole GPU step -- ripple solve, FFT, blits,
	// foam, readbacks), or once per rendered frame for all of that frame's ticks (the ripple layer still sub-steps
	// per tick with each tick's bodies; the rest runs once with the summed dt -- see WaterSolverGPU::rt_step).
	enum StepMode {
		STEP_MODE_PER_TICK,
		STEP_MODE_PER_FRAME,
	};
	void set_step_mode(StepMode p_mode);
	StepMode get_step_mode() const { return step_mode; }
	void set_ripple_rest_threshold(float p_threshold);
	float get_ripple_rest_threshold() const { return ripple_rest_threshold; }
	bool is_ripple_resting() const { return solver.is_ripple_resting(); }

	void set_domain_size(Vector2 p_size);
	Vector2 get_domain_size() const { return domain_size; }
	void set_grid_resolution(int p_n);
	int get_grid_resolution() const { return grid_resolution; }
	void set_depth(float p_depth);
	float get_depth() const { return depth; }
	// A river flowing in at p_at (world, its mouth) along p_direction: its
	// outflow spreads into this water as a plume. Called by PhysXRiver3D.
	void set_inflow(int64_t p_owner, const Vector3 &p_at, const Vector3 &p_direction, float p_width, float p_discharge);
	void clear_inflow(int64_t p_owner);
	// A river leaving at p_at (world, its sill, at world height p_crest) along p_direction (out of the lake):
	// the lake drains over the sill as a broad-crested weir, and its water converges on the outlet. Called by
	// PhysXRiver3D (source_lake).
	void set_outflow(int64_t p_owner, const Vector3 &p_at, const Vector3 &p_direction, float p_width, float p_crest, float p_coefficient);
	void clear_outflow(int64_t p_owner);
	// Broad-crested weir (Henderson 1966): m^3/s over a sill p_width wide under p_head of water.
	static float weir_discharge(float p_coefficient, float p_width, float p_head);
	void set_level_from_flows(bool p_enabled);
	bool get_level_from_flows() const { return level_from_flows; }
	void set_catchment_inflow(float p_discharge);
	float get_catchment_inflow() const { return catchment_inflow; }
	float get_outflow_discharge() const;
	void set_water_level(float p_level);
	float get_water_level() const { return water_level; }
	void set_damping(float p_damping);
	float get_damping() const { return damping; }
	void set_ripple_amplitude(float p_amplitude);
	float get_ripple_amplitude() const { return ripple_amplitude; }

	void set_ocean_grid_resolution(int p_n);
	int get_ocean_grid_resolution() const { return ocean_grid_resolution; }
	void set_ocean_domain_size(Vector2 p_size);
	Vector2 get_ocean_domain_size() const { return ocean_domain_size; }
	void set_wind_speed(float p_speed);
	float get_wind_speed() const { return wind_speed; }
	void set_wind_direction(Vector2 p_dir);
	Vector2 get_wind_direction() const { return wind_direction; }
	void set_wave_amplitude(float p_amp);
	float get_wave_amplitude() const { return wave_amplitude; }
	// Wave cascades: 1 = the one FFT tile of ocean_domain_size; more add larger
	// tiles (each wave_cascade_ratio times the last) carrying the longer waves,
	// each its own band of the spectrum, so the sea has swell and chop together
	// without visible tiling.
	static constexpr int MAX_WAVE_CASCADES = 4;
	~PhysXWaterSurface3D();
	void set_wave_cascades(int p_count);
	int get_wave_cascades() const { return wave_cascades; }
	// Detail waves: a small FFT tile carrying the short waves (down to about 8 cm) the finest cascade's grid
	// cannot, for the per-pixel normals only; faded out by each pixel's footprint, its slope variance going to
	// the roughness instead (Bruneton, Neyret and Holzschuch 2010). docs/ideas/water.md "Ocean detail".
	void set_detail_waves(bool p_enabled);
	bool get_detail_waves() const { return detail_waves; }
	Vector2 get_detail_wave_tile() const;
	float get_detail_slope_variance() const;
	void set_wave_cascade_ratio(float p_ratio);
	float get_wave_cascade_ratio() const { return wave_cascade_ratio; }
	// Tile size of cascade i (0 = ocean_domain_size).
	Vector2 get_wave_cascade_size(int p_index) const;
	// Shore waves (docs/ideas/water.md "Step 5b"): swell that turns with the
	// depth contours, steepens and breaks on the beach -- two wave trains whose
	// crests follow a phase field solved over the seabed (ray theory), heights
	// from energy flux (shoaling) capped at breaking (H = 0.78 h).
	void set_shore_waves(bool p_enabled);
	bool get_shore_waves() const { return shore_waves; }
	void set_shore_wave_height(float p_height);
	float get_shore_wave_height() const { return shore_wave_height; }
	void set_shore_wave_period(float p_period);
	float get_shore_wave_period() const { return shore_wave_period; }
	void set_shore_wave_direction(Vector2 p_direction);
	Vector2 get_shore_wave_direction() const { return shore_wave_direction; }
	// How strongly plunging breakers curl over (0 = none, the crests only
	// steepen and spill). Whether a beach plunges at all comes from its slope.
	void set_shore_wave_curl(float p_curl);
	float get_shore_wave_curl() const { return shore_wave_curl; }
	void set_shore_wave_groups(float p_groups);
	float get_shore_wave_groups() const { return shore_wave_groups; }
	// Spray thrown from plunging lips (Ihmsen et al. 2012 potentials): droplets
	// leave the lip's edge and splash up where it lands on the water ahead.
	void set_shore_spray(bool p_enabled);
	bool get_shore_spray() const { return shore_spray; }
	// Most spray droplets alive at once.
	void set_shore_spray_amount(int p_amount);
	int get_shore_spray_amount() const { return shore_spray_amount; }
	// Spray torn off breaking crests by the wind (spume, Andreas 1998): droplets leave the open sea's crests
	// where the surface folds (the whitecap test) once the wind passes about 9 m/s, more with the wind, and
	// fly downwind. Drawn like the surf's spray, near the camera. crest_spray_amount: droplets alive at most.
	void set_crest_spray(bool p_enabled);
	bool get_crest_spray() const { return crest_spray; }
	void set_crest_spray_amount(int p_amount);
	int get_crest_spray_amount() const { return crest_spray_amount; }
	// Droplets a second leaving the crests at the last update (for tests and profiling).
	float get_crest_spray_rate() const { return crest_spray_rate; }
	// Torn spray sheets (docs/ideas/water.md "Torn spray sheets"): from a near-gale, thin sheets of water the wind
	// tears off the folding crests, flying downwind and tearing apart; geometry rebuilt each tick, so the path
	// tracer sees them. crest_sheet_amount: sheets alive at most.
	void set_crest_sheets(bool p_enabled);
	bool get_crest_sheets() const { return crest_sheets; }
	void set_crest_sheet_amount(int p_amount);
	int get_crest_sheet_amount() const { return crest_sheet_amount; }
	int get_crest_sheet_count() const { return (int)sheets.size(); }
	// The last breaker build: crest points, lip vertices and triangles, the
	// spray rate (droplets/s) and the CPU time it took (ms).
	Dictionary get_breaker_stats() const;
	// Extra room around the surface before it is culled, for a material displacing it further than the waves.
	void set_cull_margin(float p_margin);
	float get_cull_margin() const { return cull_margin; }
	// How far the wind has blown over open water, in meters (JONSWAP): sets
	// how tall and long the wind waves get. 0 = auto: the surface_mesh
	// footprint's longest side for enclosed water, a fully developed open sea
	// otherwise.
	void set_wave_spreading(WaveSpreading p_spreading);
	WaveSpreading get_wave_spreading() const { return wave_spreading; }
	void set_swell(float p_swell);
	float get_swell() const { return swell; }
	void set_fetch(float p_fetch);
	float get_fetch() const { return fetch; }
	// The fetch actually in use (resolves the 0 = auto case).
	float get_effective_fetch() const;
	// Tessendorf horizontal displacement: pulls the surface toward the
	// ocean waves' crests so they sharpen and the troughs flatten. 0 = pure
	// height field. Where it folds the surface (its Jacobian drops toward
	// zero) the default material draws whitecap foam.
	void set_choppiness(float p_choppiness);
	float get_choppiness() const { return choppiness; }
	// Which the built-in (and a compatible custom) material uses; sets its
	// per_pixel_normals uniform.
	void set_normal_mode(NormalMode p_mode);
	NormalMode get_normal_mode() const { return normal_mode; }
	// Whitecap foam: injected where the choppy displacement folds the surface
	// (area Jacobian below foam_threshold -- 1 = unsqueezed, 0 = folding over)
	// and fading with a time constant of foam_persistence seconds, so crests
	// leave streaks behind. Lower threshold = fewer whitecaps.
	// Off = no whitecap foam at all (a pool, a sheltered pond).
	void set_foam_enabled(bool p_enabled);
	bool get_foam_enabled() const { return foam_enabled; }
	void set_foam_threshold(float p_threshold);
	float get_foam_threshold() const { return foam_threshold; }
	// Whitecap cover from the wind (on by default): the threshold is steered
	// each step so the share of sea under foam follows the measured law of
	// Monahan and O'Muircheartaigh (1980), W = 3.84e-6 U^3.41 (about 1% at
	// 10 m/s, 3% at 14 m/s, none below ~3 m/s), times whitecap_cover_scale.
	// foam_threshold is then only the starting point. Off = the fixed
	// foam_threshold, as before.
	void set_whitecap_auto(bool p_auto);
	bool get_whitecap_auto() const { return whitecap_auto; }
	void set_whitecap_cover_scale(float p_scale);
	float get_whitecap_cover_scale() const { return whitecap_cover_scale; }
	// The cover the wind asks for (0..1) and the cover measured on the
	// foam grids at the last readback (-1 before the first one).
	float get_whitecap_target_cover() const;
	float get_whitecap_cover() const { return whitecap_measured; }
	// The threshold in use (foam_threshold, or the steered one).
	float get_effective_foam_threshold() const { return _live_foam_threshold(); }
	void set_foam_persistence(float p_seconds);
	float get_foam_persistence() const { return foam_persistence; }
	// Shore foam: waves arriving over water shallower than this (meters,
	// from the seabed) leave a foamy sheet at the waterline. 0 = none.
	void set_shore_foam_band(float p_depth);
	float get_shore_foam_band() const { return shore_foam_band; }
	// Rock contact foam (docs/ideas/water.md "Rock contact foam"): a ring of white water where the sea meets a
	// steep face -- a stack, a boulder, a cliff foot -- contact_foam_width wide, widening as each crest arrives,
	// and wash over rock tops up to contact_foam_depth under the surface. Read from a top-down map of the static
	// bodies in contact_foam_mask over contact_foam_extent round the camera (rays, re-measured as it moves).
	// Gentle beaches are left to the shore foam.
	void set_contact_foam(bool p_enabled);
	bool get_contact_foam() const { return contact_foam; }
	void set_contact_foam_mask(uint32_t p_mask);
	uint32_t get_contact_foam_mask() const { return contact_foam_mask; }
	void set_contact_foam_width(float p_width);
	float get_contact_foam_width() const { return contact_foam_width; }
	void set_contact_foam_strength(float p_strength);
	float get_contact_foam_strength() const { return contact_foam_strength; }
	void set_contact_foam_depth(float p_depth);
	float get_contact_foam_depth() const { return contact_foam_depth; }
	void set_contact_foam_extent(float p_extent);
	float get_contact_foam_extent() const { return contact_foam_extent; }
	void remeasure_contact_foam();
	// Cells of the last map with a contact ring on the water, and the rays it took (benches, tests).
	int get_contact_foam_cells() const { return contact_map.get_contact_cells(); }
	int get_contact_foam_rays() const { return contact_map.get_last_ray_count(); }
	// True once the map is measured, including the second measure after the start (benches wait for it).
	bool is_contact_foam_ready() const { return contact_shown && contact_settle_ticks == 0 && !contact_dirty && !contact_map.is_busy(); }
	void set_open_sea_beyond(bool p_open);
	// Wind / direction / height / fetch at once — ONE in-place spectrum update (no rebuild once built).
	void set_sea_state(float p_wind_speed, Vector2 p_wind_direction, float p_wave_amplitude, float p_fetch);
	// Take wind speed and direction from the WeatherMap3D, with duration-limited wave growth.
	void set_follow_weather(bool p_enabled);
	bool get_follow_weather() const { return follow_weather; }
	void recenter(Vector3 p_world_position);
	void remeasure_seabed();
	bool get_open_sea_beyond() const { return open_sea_beyond; }
	// Shore foam moves with the water: each crest pushes it up the beach and
	// the backwash drags it back out, plus this steady seaward undertow (m/s)
	// that slides it back into the water over time.
	void set_shore_undertow(float p_speed);
	float get_shore_undertow() const { return shore_undertow; }
	// Swash: the biggest arriving waves run up the dry sand as a thin film,
	// at most this high (m above still water; 0 = none), then slide back
	// down at swash_drain_speed (m/s, vertical). Sand the water has covered
	// stays dark and glossy, drying over wet_sand_dry_time seconds. Only
	// shows where the depth varies (seabed_from_floor).
	void set_swash_run_up(float p_height);
	float get_swash_run_up() const { return swash_run_up; }
	void set_swash_drain_speed(float p_speed);
	float get_swash_drain_speed() const { return swash_drain_speed; }
	void set_wet_sand_dry_time(float p_seconds);
	float get_wet_sand_dry_time() const { return wet_sand_dry_time; }
	// Wet sand, rocks and props where the sea reached, kept by the weather's contact wetness (WeatherMap3D) and
	// drying with the weather: what the swash wetted along the shore, still water shallower than half a metre.
	void set_contact_wetness(bool p_enabled);
	bool get_contact_wetness() const { return contact_wetness; }

	// Optional shape: any flat mesh whose X/Z footprint (node-local) is the
	// water's outline -- a disc, a kidney bean, a lake with an island. When
	// set, the ripple grid is fitted to its bounds (domain_size is ignored),
	// cells outside it are dry land that waves reflect off, and the rendered
	// surface is the footprint resampled into an even grid clipped to its
	// outline. Unset = the square domain_size / ocean_domain_size plane.
	void set_surface_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_surface_mesh() const { return surface_mesh; }

	// Seabed: when on, each ripple cell's still-water depth is measured once
	// on the first physics tick by a ray straight down onto static bodies in
	// seabed_collision_mask. Waves then travel at the local shallow-water
	// speed, cells where the seabed is above the water are dry shore, and
	// the ocean chop fades out over the shallows. Off = the constant depth.
	void set_seabed_from_floor(bool p_enabled);
	bool get_seabed_from_floor() const { return seabed_from_floor; }
	void set_seabed_collision_mask(uint32_t p_mask);
	uint32_t get_seabed_collision_mask() const { return seabed_collision_mask; }
	// How far above the water the seabed rays start (m): under a roof (a cave) keep it below the roof.
	void set_seabed_probe_height(real_t p_height);
	real_t get_seabed_probe_height() const { return seabed_probe_height; }
	// A terrain with holes (TerrainGenerator3D: is_hole, get_height, signal holes_changed): where a seabed ray
	// falls through a hole (a cave mouth) onto nothing, the terrain's own height closes it, so the sea never shows
	// through an open hole above the waterline; a static floor under the hole (a sea cave's) is used as it is.
	// The seabed is measured again when holes change over the water.
	void set_terrain_source(const NodePath &p_path);
	NodePath get_terrain_source() const { return terrain_source; }
	// Water shallower than this fades the ocean chop out, to none at the
	// waterline.
	void set_shallow_fade_depth(float p_depth);
	float get_shallow_fade_depth() const { return shallow_fade_depth; }

	// Side length (m) of the rendered surface, centered on the simulation.
	// Past the simulated square the FFT ocean tiles seamlessly (it's
	// periodic) and the ripples settle to still water, so open water can run
	// to the horizon; vertex spacing grows with distance. 0 = just the
	// simulated square. Ignored with a surface_mesh.
	void set_render_extent(float p_extent);
	float get_render_extent() const { return render_extent; }
	// Mesh cells per ocean grid cell (1-4). Finer meshes resolve the shape
	// of breaking and plunging waves (about 0.1 m cells for a 1 m breaker);
	// the vertex and path tracing costs grow with the square.
	void set_mesh_detail(int p_detail);
	int get_mesh_detail() const { return mesh_detail; }
	// Open sea (render_extent): the vertex spacing at the camera, growing 8% a ring until it reaches the even core's spacing
	// (docs/ideas/water.md "Camera-centred water mesh"); 0 = the old even core over the simulated square.
	void set_near_mesh_step(float p_step);
	float get_near_mesh_step() const { return near_mesh_step; }

	void set_water_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_water_material() const { return water_material; }

	// Light-space caustics: a direct RenderingDevice draw pass that projects
	// refracted surface-grid samples into a texture, without a SubViewport.
	// The camera under water (raster): a full screen fog pass under the surface, the image split at the
	// water line, and the surface drawn from below (Snell's window, total internal reflection). Only
	// while the camera is near the water. The path tracer needs nothing: it finds the camera's medium.
	// Raster only: the surface mirrors the sky and the sun's glitter by the Fresnel term, as the path tracer
	// sees it. Off = the old unlit tint.
	void set_raster_reflections(bool p_enabled);
	bool get_raster_reflections() const { return raster_reflections; }
	void set_underwater_enabled(bool p_enabled);
	bool get_underwater_enabled() const { return underwater_enabled; }
	void set_underwater_meniscus(float p_strength);
	float get_underwater_meniscus() const { return underwater_meniscus; }
	// True while the active camera is under the surface (as the last frame drew it).
	bool is_camera_underwater() const { return underwater_camera_below; }

	void set_caustics_enabled(bool p_enabled);
	bool get_caustics_enabled() const { return caustics_enabled; }
	void set_caustics_sun_direction(Vector3 p_direction);
	Vector3 get_caustics_sun_direction() const { return caustics_sun_direction; }
	void set_caustics_reference_depth(float p_depth);
	float get_caustics_reference_depth() const { return caustics_reference_depth; }
	// The resulting texture -- assign it to a floor material's albedo/
	// emission using get_caustics_light_right(), get_caustics_light_up(),
	// get_caustics_origin(), and get_caustics_half_extent() as projection
	// metadata. Null until caustics_enabled and the node has built.
	Ref<Texture2D> get_caustics_texture() const;
	Vector3 get_caustics_light_right() const { return solver.get_caustics_light_right(); }
	Vector3 get_caustics_light_up() const { return solver.get_caustics_light_up(); }
	Vector3 get_caustics_origin() const { return solver.get_caustics_origin(); }
	float get_caustics_half_extent() const { return solver.get_caustics_half_extent(); }
	// Period of the caustics (the ocean tile): see WaterSolver::get_caustics_tile_size().
	Vector2 get_caustics_tile_size() const { return solver.get_caustics_tile_size(); }

	// CPU-side, safe every physics tick -- bilinear samples of the last
	// texture refresh (see _refresh_textures()'s throttle), summed across
	// both layers (ripple + ocean), matching what the rendered mesh shows.
	// Returns -INF over dry cells when a surface_mesh is set.
	float sample_height(Vector3 p_world_pos) const;
	// The water contract (WorldWater): this water's surface, -INF where it has none.
	float contract_water_height_at(const Vector3 &p_position) const override;
	void contract_water_disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength) override;
	void contract_water_clear(uint64_t p_owner) override;
	void contract_water_impulse(const Vector3 &p_position, float p_radius, float p_strength) override;
	// The same, leaving out one wake (a boat's buoyancy reading the water
	// under it shouldn't sink into its own wake's dip).
	float sample_height_excluding_wake(Vector3 p_world_pos, ObjectID p_wake) const;
	// Script-side: the wake as a node (null leaves nothing out).
	float _sample_height_excluding_wake_bind(Vector3 p_world_pos, Object *p_wake) const;
	// How fast the ripple + ocean surface rises (m/s, negative falling) above a point, from the last two readbacks.
	// Wakes are left out. 0 over dry cells and until two readbacks have landed.
	float sample_vertical_speed(Vector3 p_world_pos) const;
	// Moving wake grids (PhysXWaterWake3D): up to MAX_WAKES are drawn and
	// sampled. register_wake() returns a slot (-1 when full).
	static constexpr int MAX_WAKES = 4;
	int register_wake(ObjectID p_wake);
	void unregister_wake(int p_slot);
	// The wake's height texture and its rect (xy world xz corner, z side, w
	// shore-grid fade 0..1).
	void set_wake_slot(int p_slot, const Ref<Texture2D> &p_height, const Vector4 &p_rect);
	// False outside the surface_mesh footprint (always true without one).
	bool is_wet(Vector3 p_world_pos) const;
	// Water depth (m) at a world position: the surface as drawn (sample_height) over the measured seabed (or the
	// constant depth without one); <= 0 on dry ground.
	float sample_depth(Vector3 p_world_pos) const;
	// The swash on the shore at a world position: x the run-up film on the beach (m of water over ground above still
	// water, 0 past the run-up and below still water), y the wet sand
	// (0..1, 1 under water and where the run-up reached, fading as it dries). Read back from the GPU every 8th
	// water step (about 0.13 s behind), asked for from the first call on; zero until the first readback.
	Vector2 sample_swash(Vector3 p_world_pos) const;

	// Flow (docs/ideas/water.md step 3b): a current in m/s. With a measured
	// seabed (seabed_from_floor, or a surface_mesh footprint) the flow over
	// the simulated square is solved once round dry ground and rocks (a
	// depth-weighted stream function), so it goes round them, speeds up
	// through narrows and slows in the shallows; past the square, and
	// without a seabed, it is the current everywhere. The waves and foam are
	// carried along it (two-phase flow), and PhysXBuoyancy3D drags floaters
	// with it.
	void set_current(Vector2 p_current);
	Vector2 get_current() const { return current; }
	// Seconds one flow phase lasts before it restarts: longer = waves carried
	// further before they blend back (more stretch), shorter = more blending.
	void set_flow_period(float p_period);
	float get_flow_period() const { return flow_period; }
	// The water's velocity at a world position (m/s, horizontal), as the
	// surface is drawn: zero over dry ground.
	Vector3 sample_flow(Vector3 p_world_pos) const;
	// The solved flow over the simulated square (RG: m/s along world X and Z);
	// null without a seabed or current.
	Ref<Texture2D> get_flow_texture() const;
	// Live flow (docs/ideas/water.md step 3c): the local water square runs the
	// shallow-water equations on top of the baked flow, so rocks and banks
	// shed wakes and eddies and floaters feel them. Off = the baked flow only,
	// and the square only ripples.
	void set_live_flow(bool p_enabled);
	bool is_live_flow() const { return live_flow; }
	// How strongly dry ground slows the flow running along it (1/s): what
	// makes rocks shed eddies. 0 = slippery walls.
	void set_wall_friction(float p_friction);
	float get_wall_friction() const { return wall_friction; }
	// White water the live flow makes where it runs fast (rapids, chutes)
	// and shears (wakes, eddies, the edges of a jet); 0 = none. It is carried
	// with the water and fades over flow_foam_persistence seconds.
	void set_flow_foam(float p_strength);
	float get_flow_foam() const { return flow_foam; }
	void set_flow_foam_persistence(float p_seconds);
	float get_flow_foam_persistence() const { return flow_foam_persistence; }
	// The live flow's velocity at cell centres (RG m/s, B flow foam), or null when off.
	Ref<Texture2D> get_live_flow_texture() const;
	// Solves the flow again (after the seabed or the current changed; done
	// automatically for both).
	void rebake_flow();

	void submit_sphere(int p_owner, Vector3 p_world_pos, float p_radius, float p_strength = 1.0f);
	void clear_sphere(int p_owner);
	void submit_impulse(Vector3 p_world_pos, float p_radius, float p_strength);

private:
	WaterSolver solver;
	StepMode step_mode = STEP_MODE_PER_TICK;
	float ripple_rest_threshold = 0.0f;
	bool water_time_dirty = false; // per-frame mode: water_time advanced since the material last got it
	// NOT cached from immediately after configure() -- configure() dispatches
	// asynchronously (a render-thread post, see water_solver.h), so
	// solver.is_available() only flips true a frame or two later. _update()
	// re-checks it fresh every tick instead of trusting a stale snapshot.
	bool warned_no_device = false;

	Vector2 domain_size = Vector2(20, 20);
	int grid_resolution = 96;
	float depth = 3.0f;
	float water_level = 0.0f;
	float damping = 0.08f; // see WaterSolver::Settings::damping's own note on this value
	float ripple_amplitude = 1.0f;

	int ocean_grid_resolution = 64;
	Vector2 ocean_domain_size = Vector2(40, 40);
	int _resolved_ocean_grid_resolution() const;
	float wind_speed = 8.0f;
	Vector2 wind_direction = Vector2(1, 0);
	float wave_amplitude = 1.0f;
	int wave_cascades = 1;
	bool detail_waves = true;
	float wave_cascade_ratio = 6.0f;
	bool shore_waves = true;
	float shore_wave_height = 0.8f; // m, offshore
	float shore_wave_period = 8.0f; // s
	float shore_wave_curl = 1.0f;
	float shore_wave_groups = 1.0f;
	Vector2 shore_wave_direction; // (0, 0) = wind_direction
	static constexpr int SHORE_TRAINS = 2;
	static constexpr float BREAKER_INDEX = 0.78f; // H / h at breaking (McCowan)
	// Per ripple cell, per train: phase (rad), height (m), phase gradient (k times
	// the travel direction, rad/m); and each train's angular frequency.
	Vector<float> shore_phase[SHORE_TRAINS];
	Vector<float> shore_height[SHORE_TRAINS];
	Vector<float> shore_grad_x[SHORE_TRAINS];
	Vector<float> shore_grad_z[SHORE_TRAINS];
	float shore_omega[SHORE_TRAINS] = {};
	Ref<ImageTexture> shore_wave_tex_a;
	Ref<ImageTexture> shore_wave_tex_b;
	// Per ripple cell: how far each train is through breaking (its unbroken
	// shoaled height over the breaker height 0.78 h: 1 at the break point,
	// rising through the surf zone) and the surf similarity (Iribarren)
	// number from the local beach slope, which sets plunging or spilling.
	Vector<float> shore_stage[SHORE_TRAINS];
	Vector<float> shore_iribarren;
	Ref<ImageTexture> shore_wave_tex_c;
	// Plunging lips (docs/ideas/water.md "Step 5e", after Thurey et al. 2007):
	// a closed sheet swept along each breaking crest, rebuilt every tick from
	// the shore waves' phase field, drawn with the water material (its vertex
	// colour alpha 0 marks it), plus the spray it throws.
	MeshInstance3D *breaker_instance = nullptr;
	Ref<ArrayMesh> breaker_mesh;
	GPUParticles3D *breaker_spray = nullptr;
	Ref<ParticleProcessMaterial> breaker_spray_material;
	Ref<ImageTexture> breaker_spray_points_tex;
	Ref<ImageTexture> breaker_spray_dirs_tex;
	bool shore_spray = true;
	int shore_spray_amount = 3000;
	int breaker_stat_points = 0;
	int breaker_stat_vertices = 0;
	int breaker_stat_triangles = 0;
	float breaker_stat_spray_rate = 0.0f;
	float breaker_stat_ms = 0.0f;
	RandomPCG breaker_rng;
	Ref<ShaderMaterial> breaker_material;
	LocalVector<StringName> breaker_params;
	Ref<Material> _breaker_material();
	void _update_breakers();
	float _sample_height_at(Vector3 p_world_pos, ObjectID p_wake, float p_origin_y) const;
	void _hide_breakers();
	void _update_breaker_spray(const LocalVector<Vector3> &p_points, const LocalVector<Vector3> &p_dirs, const LocalVector<float> &p_weights, float p_rate, float p_speed);
	static Ref<Mesh> _spray_droplet_mesh();
	bool crest_spray = true;
	int crest_spray_amount = 6000;
	float crest_spray_rate = 0.0f;
	uint32_t crest_spray_tick = 0;
	GPUParticles3D *crest_spray_node = nullptr;
	Ref<ParticleProcessMaterial> crest_spray_material;
	Ref<ImageTexture> crest_spray_points_tex;
	GPUParticles3D *crest_spume_node = nullptr;
	Ref<ParticleProcessMaterial> crest_spume_material;
	void _make_crest_spume();
	RandomPCG crest_rng;
	void _update_crest_spray();
	struct CrestSheet {
		Vector3 root; // where the sheet leaves the crest (world)
		Vector3 across; // along the crest (unit, horizontal)
		Vector3 throw_dir; // horizontal launch direction (unit)
		float speed = 0.0f; // horizontal launch speed (m/s)
		float rise = 0.0f; // vertical launch speed (m/s)
		float span = 1.0f; // metres along the crest at launch
		float feed = 0.3f; // seconds the root keeps feeding the sheet
		float life = 1.0f; // seconds
		float age = 0.0f;
		float seed = 0.0f;
	};
	bool crest_sheets = true;
	int crest_sheet_amount = 40;
	LocalVector<CrestSheet> sheets;
	MeshInstance3D *sheet_instance = nullptr;
	Ref<ArrayMesh> sheet_mesh;
	Ref<ShaderMaterial> sheet_material;
	RandomPCG sheet_rng;
	float sheet_fold_share = 0.0f;
	float sheet_spawn = 0.0f;
	void _update_crest_sheets(float p_dt);
	void _update_foam_streaks();
	// The summed cascades' Jacobian (Tessendorf's folding test, as the shader's foam) at the resting point
	// (p_x, p_z) relative to the grid centre, and the surface point it moves to (relative to the node).
	float _ocean_jacobian_at(float p_x, float p_z, float p_fade, Vector3 &r_point) const;
	void _bake_shore_waves();
	void _update_shore_material();
	// The shore waves' height at (p_x, p_z) relative to the grid centre, and its rate of change.
	float _shore_eta_at(float p_x, float p_z, float *r_rate = nullptr) const;
	float _shore_wave_set(float p_theta, int p_train) const;
	// Cascades 1.. (cascade 0 is the main solver's own ocean): an ocean-only
	// WaterSolver each (a tiny resting ripple grid), its textures and readbacks.
	struct WaveCascade {
		WaterSolver solver;
		Vector2 domain;
		Ref<Texture2DRD> height_tex;
		Ref<Texture2DRD> disp_tex;
		Ref<Texture2DRD> deriv_tex;
		Ref<Texture2DRD> foam_tex;
		Vector<float> height;
		Vector<float> dx;
		Vector<float> dz;
		Vector<float> previous_height;
		Vector<float> previous_dx;
		Vector<float> previous_dz;
		int n = 0;
		Vector2 cached_domain;
		double time = 0.0;
		double previous_time = 0.0;
	};
	LocalVector<WaveCascade *> extra_cascades;
	WaveCascade *detail_cascade = nullptr; // the detail waves' tile (the normal; displaced near the camera on the open sea mesh), or null
	int detail_resolution = 128;
	float _detail_k_min() const; // the finest cascade's Nyquist wavenumber (rad/m)
	float _wind_sea_sigma(float p_k_lo, float p_k_hi) const;
	float _shore_height() const;
	float measured_sigma = -1.0f; // the simulated sea's rms height (m), from the CPU grids; < 0 until read
	int sigma_countdown = 0;
	float shore_baked_height = -1.0f; // the swell height the shore waves were last baked for (m)
	void _configure_cascades(const WaterSolver::Settings &p_base);
	void _clear_cascades();
	// The wavenumber where cascade i hands over to cascade i + 1 (rad/m).
	float _cascade_split(int p_index) const;
	float cull_margin = 0.0f;
	float fetch = 0.0f;
	WaveSpreading wave_spreading = WAVE_SPREADING_DONELAN_BANNER;
	float swell = 0.0f;
	float render_extent = 0.0f;
	int mesh_detail = 1;
	float near_mesh_step = 0.0f;
	static constexpr float NEAR_MESH_GROWTH = 0.08f;
	bool follow_weather = false;
	bool weather_sea_valid = false;
	uint64_t weather_snap_serial = 0;
	double weather_advanced_seen = 0.0;
	double weather_check_accum = 0.0;
	float weather_sea_speed = 8.0f;
	Vector2 weather_sea_direction = Vector2(1, 0);
	void _follow_weather(double p_delta);
	double water_time = 0.0; // shader clock (water_time), advanced only in a running game
	float choppiness = 1.0f;
	NormalMode normal_mode = NORMAL_MODE_PER_PIXEL;
	bool foam_enabled = true;
	float foam_threshold = 0.7f;
	bool whitecap_auto = true;
	float whitecap_cover_scale = 1.0f;
	float whitecap_threshold = 0.7f; // steered threshold
	float whitecap_pushed = -100.0f; // last one sent to the solvers and material
	float whitecap_measured = -1.0f;
	double whitecap_clock = -1.0;
	float whitecap_fit_wind = -1.0f; // the wind the threshold's feedforward last followed
	double whitecap_fill_start = 0.0; // when the persistent foam last started filling (a start or a big change)
	float _live_foam_threshold() const { return whitecap_auto ? whitecap_threshold : foam_threshold; }
	void _steer_whitecaps();
	void _push_foam_settings();
	float foam_persistence = 2.5f;
	float shore_foam_band = 0.5f;
	bool contact_foam = true;
	uint32_t contact_foam_mask = 0xFFFFFFFF;
	float contact_foam_width = 1.2f;
	float contact_foam_strength = 0.9f;
	float contact_foam_depth = 0.6f;
	float contact_foam_extent = 96.0f;
	WaterContactMap contact_map;
	Ref<ImageTexture> contact_texture;
	bool contact_shown = false;
	bool contact_dirty = true; // measure again at the next tick
	int contact_settle_ticks = 30; // one more measure this many physics ticks after the start (bodies built late)
	void _update_contact_foam();
	void _push_contact_foam();
	void _push_shore_wave_activity();
	float shore_wave_activity_sent = -1.0f;
	// Open water past the simulated square (render_extent > domain, no surface_mesh): true = it is open, deep sea
	// everywhere past the square (full chop, wet, never lifted onto the ground) -- for a sea whose ripple square
	// follows a focus that may sit over land; false = the square's edge seabed carries on outward (a beach scene).
	bool open_sea_beyond = false;
	float shore_undertow = 0.3f;
	float swash_run_up = 0.3f;
	float swash_drain_speed = 0.12f;
	float wet_sand_dry_time = 12.0f;
	bool contact_wetness = true;
	uint64_t contact_wet_serial = 0;
	Vector<float> contact_wet_cells;
	void _wet_ground();
	// Longest side of the surface_mesh footprint (0 = none), for auto fetch.
	real_t footprint_extent = 0.0;

	MeshInstance3D *mesh_instance = nullptr;
	Ref<Mesh> surface_mesh;
	Ref<Mesh> water_mesh; // what mesh_instance draws: a PlaneMesh, or the resampled surface_mesh footprint

	// World XZ the simulation grids are centered on (node origin, plus the
	// footprint's center when surface_mesh is set), and the ripple domain
	// actually in use -- both fixed at _rebuild().
	Vector2 grid_center;
	Vector2 active_domain_size;
	// Wet/dry cells from the surface_mesh footprint (empty = none set), and
	// the still-water depth per ripple cell actually in use (<= 0 = dry;
	// empty = the constant depth everywhere). CPU copies for is_wet() and
	// sample_height().
	PackedByteArray footprint_mask;
	PackedFloat32Array cell_depth;
	// WaterSolver::shallow_fade() of each cell_depth, for sample_height()'s
	// ocean fade (bilinear on this, so a wall cell doesn't drag it down).
	PackedFloat32Array cell_fade;
	void _update_cell_fade();
	bool seabed_from_floor = false;
	uint32_t seabed_collision_mask = 1;
	real_t seabed_probe_height = 100.0;
	float shallow_fade_depth = 1.0f;
	// Seabed rays need the floor in the physics space, so with
	// seabed_from_floor the solver is configured on the first physics tick.
	bool seabed_pending = false;
	int seabed_tries = 0;
	bool seabed_remeasure_pending = false;
	NodePath terrain_source;
	ObjectID terrain_connected;
	void _connect_terrain();
	void _disconnect_terrain();
	void _on_terrain_holes_changed(const Rect2 &p_rect);
	float _rest_depth_at(float p_rel_x, float p_rel_z) const; // the measured seabed's still-water depth (m)
	mutable SafeFlag swash_queried; // sample_swash() was called: keep the swash readback on // remeasure_seabed(): rays on the next tick, then in place
	Ref<ShaderMaterial> water_material;
	Vector2 current;
	float flow_period = 2.0f;
	// Solved flow per ripple cell (x, z m/s), empty = the uniform current.
	PackedVector2Array flow_cells;
	bool live_flow = true;
	float wall_friction = 1.5f;
	float flow_foam = 1.0f;
	float flow_foam_persistence = 8.0f;
	PackedFloat32Array flow_faces; // baked face fluxes sent to the solver (4 a cell), empty = none
	Vector<float> cached_live_velocity; // live face velocities read back (2 a cell)
	Ref<Texture2DRD> live_flow_texture;
	void _send_flow_to_solver();
	Vector2 _live_flow_at(float p_x, float p_z) const;
	Ref<Image> flow_image;
	Ref<ImageTexture> flow_texture;
	void _bake_flow();
	void _update_flow_material();
	void _update_detail_material();
	// Flow at grid-relative (x, z), as the shader's flow_at().
	Vector2 _flow_at(float p_x, float p_z) const;
	// Rivers flowing in (PhysXRiver3D's mouth): each spreads as a turbulent
	// jet, a plume added to the flow over the simulated square.
	struct Inflow {
		int64_t owner = 0;
		Vector3 at; // world, the shoreline
		Vector3 direction; // world, horizontal unit
		float width = 1.0f;
		float discharge = 0.0f;
	};
	LocalVector<Inflow> inflows;
	// Rivers leaving (PhysXRiver3D's source_lake): each drains over its sill; the water converges on it.
	struct Outflow {
		int64_t owner = 0;
		Vector3 at; // world, the sill's centre
		Vector3 direction; // world, horizontal unit, out of the lake
		float width = 1.0f;
		float crest = 0.0f; // world height of the sill
		float coefficient = 0.85f;
	};
	LocalVector<Outflow> outflows;
	bool level_from_flows = false; // the level settles where the outlets pass the inflows (docs "Lake outlets")
	float catchment_inflow = 0.0f; // m^3/s from rain and springs, besides the rivers flowing in
	void _settle_level();
	PackedVector2Array plume_cells; // m/s per ripple cell, world xz
	Ref<ImageTexture> plume_texture;
	void _bake_plumes();
	Vector2 _plume_at(float p_x, float p_z) const;
	// The shader's flow_phases() at grid-relative (x, z): the two phases' offsets and the first's weight.
	float _flow_phases(float p_x, float p_z, Vector2 &r_offset_a, Vector2 &r_offset_b) const;
	// An ocean grid at grid-relative (x, z), carried by the flow (the shader's ocean_tex()).
	float _ocean_flow_sample(const Vector<float> &p_grid, float p_x, float p_z, float p_weight, const Vector2 &p_offset_a, const Vector2 &p_offset_b, int p_n = -1, Vector2 p_domain = Vector2()) const;
	ObjectID wakes[MAX_WAKES];
	Vector2 mesh_offset; // see _update_mesh_offset()
	void _update_mesh_offset();
	void _update_cull_box();
	Ref<Texture2DRD> ripple_height_tex;
	Ref<Texture2DRD> ocean_height_tex;
	Ref<Texture2DRD> ocean_fade_tex;
	Ref<Texture2DRD> shore_depth_tex;
	Ref<Texture2DRD> ocean_disp_tex;
	Ref<Texture2DRD> ocean_deriv_tex;
	Ref<Texture2DRD> ocean_foam_tex;
	Ref<Texture2DRD> shore_foam_tex;
	Ref<Texture2DRD> swash_tex;
	Ref<Texture2DRD> caustics_texture;
	bool textures_bound = false; // set once, after the solver's RD textures actually exist (see _update())
	// Unhooks every Texture2DRD above from the solver's RD textures, before
	// they're freed: the materials holding them (the water's, and caustics
	// receivers -- often scene sub-resources, so the same objects outlive
	// this node across a scene reload) fall back to their defaults instead
	// of drawing with freed textures.
	void _detach_textures();

	bool underwater_enabled = true;
	float underwater_meniscus = 0.35f;
	bool underwater_shown = false;
	Vector3 water_light_sun_dir = Vector3(0, -1, 0);
	Vector3 water_light_sun_irradiance;
	Vector3 water_light_sun_travel; // the scene sun's direction of travel while it is up, else zero
	Vector3 water_light_sky;
	bool underwater_camera_below = false;
	MeshInstance3D *underwater_fog_instance = nullptr;
	Ref<ShaderMaterial> underwater_fog_material;
	Ref<ShaderMaterial> underwater_surface_material;
	LocalVector<StringName> underwater_shared_params; // uniforms the passes copy from water_material
	// Lamps under the water (docs/ideas/water.md "Lamp beams under water"): the nearest omni and spot lights
	// below the surface, found every half second while the camera is under, scattered into beams.
	LocalVector<ObjectID> underwater_lamps;
	int underwater_lamp_scan = 0;
	void _scan_underwater_lamps(const Vector3 &p_camera, float p_level);
	void _update_underwater_lamps();
	RID underwater_sky_baked;
	int underwater_sky_rebake = 0;
	Color underwater_sky_radiance;
	void _update_water_light(const Camera3D *p_camera);
	void _update_underwater();
	void _set_underwater_shown(bool p_shown);
	// Raster reflections from above (sky panorama + sun glitter), see water_raster_reflections.h.
	bool raster_reflections = true;
	WaterRasterReflections raster_reflection_state;
	void _update_raster_reflections();
	bool caustics_enabled = true; // a draw pass every frame (the desktop measures and trims it)
	Vector3 caustics_sun_direction = Vector3(-0.35f, -1.0f, -0.25f);
	float caustics_reference_depth = 2.5f;

	Vector<float> cached_ripple_height;
	int cached_ripple_n = 0;
	Vector2 cached_ripple_domain;
	Vector<float> cached_ocean_height;
	Vector<float> cached_ocean_dx;
	Vector<float> cached_ocean_dz;
	int cached_ocean_n = 0;
	Vector2 cached_ocean_domain;
	// The readback before the cached one, and when each was read: sample_vertical_speed().
	double cached_ripple_time = 0.0;
	double cached_ocean_time = 0.0;
	Vector<float> previous_ripple_height;
	Vector<float> previous_ocean_height;
	Vector<float> previous_ocean_dx;
	Vector<float> previous_ocean_dz;
	double previous_ripple_time = 0.0;
	double previous_ocean_time = 0.0;
	float _ripple_height_at(const Vector<float> &p_height, float p_x, float p_z) const;
	float _ocean_fade_at(float p_x, float p_z) const;
	// All cascades' height at the surface point now over (p_x, p_z) (inverting the choppy displacement); with
	// p_previous, from the readbacks before the cached ones.
	float _ocean_height_at(bool p_previous, float p_x, float p_z, float p_fade) const;

	int frames_since_refresh = 0;
	// CPU cache refresh for sample_height() only -- rendering is unthrottled/automatic via the zero-copy textures.
	// Every tick: the readback lands every step anyway and the copy is a COW share, while refreshing
	// every 4th tick froze the heights in between and then jumped them several cm (rough seas), which
	// jolted buoyancy and anything resting on sampled water.
	static constexpr int REFRESH_EVERY_FRAMES = 1;

	void _rebuild();
	// Footprint triangles of surface_mesh in node-local XZ (degenerates dropped).
	Vector<Vector2> _collect_footprint() const;
	void _configure_solver();
	void _reinit_spectrum();
	void _set_spreading();
	// Casts the seabed rays; fills cell_depth.
	int _sample_seabed(bool p_quiet = false);
	static PackedByteArray _rasterize_wet_mask(const Vector<Vector2> &p_tris, Vector2 p_center, Vector2 p_domain, int p_n);
	// Plane over [-extent/2, extent/2]^2: uniform cells over the simulated
	// p_inner size, then spacing growing geometrically outward.
	Ref<Mesh> _build_extended_plane(Vector2 p_inner, int p_inner_cells, real_t p_extent, real_t p_near_step) const;
	Ref<Mesh> _build_footprint_mesh(const Vector<Vector2> &p_tris, const Rect2 &p_bounds, int p_cells) const;
	void _bind_textures();
	void _refresh_cpu_cache();
	void _update(double p_delta); // one physics tick
	void _frame_update(); // per-frame mode: the frame's ticks in one step
	void _after_step();
	// p_repeat: wrap around instead of clamping at the edge (the FFT ocean is
	// periodic).
	float _bilinear_sample(const Vector<float> &p_grid, int p_n, Vector2 p_domain, float p_world_x, float p_world_z, bool p_repeat = false) const;
	// The water carries on past the simulated square (render_extent), and
	// whether world XZ (relative to grid_center) is inside what's drawn.
	bool _open_beyond() const;
	bool _inside_render_extent(float p_rel_x, float p_rel_z) const;
};

VARIANT_ENUM_CAST(PhysXWaterSurface3D::NormalMode);
VARIANT_ENUM_CAST(PhysXWaterSurface3D::WaveSpreading);
VARIANT_ENUM_CAST(PhysXWaterSurface3D::StepMode);

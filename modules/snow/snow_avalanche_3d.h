/**************************************************************************/
/*  snow_avalanche_3d.h                                                   */
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

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

#include "snow_powder_cloud_3d.h"

class SnowCover3D;

// Avalanches (docs/ideas/snow.md, follow-up 20). Slab release where the stability index (shear strength of the weak
// layer under the slab against the slab's shear stress, Foehn 1987; Jamieson and Johnston 2001 for strength by
// density) drops below 1 on 28-55 degree slopes; the slab is the new snow since it last settled, or the snow over a
// buried crust. The released snow runs as a depth-averaged Voellmy flow (Voellmy 1955; Salm 1993; Christen et al.
// 2010, RAMMS) on the cover's own grid: gravity down the snow surface, dry friction mu g cos(theta) and turbulent
// friction g u^2 / (xi h), second-order finite-volume transport of mass and momentum (MUSCL, HLL fluxes; follow-up 22), entrainment of the new snow it runs over at a
// rate with its speed, deposition where it can no longer overcome its own friction. The moving snow lives in the
// cover's cells while it runs, so the snow map, prints, plants and every reader of WorldSnow see it move; bodies in
// its path take its dynamic pressure, 1/2 rho u^2. SnowCover3D adds one by itself while avalanches_enabled.
class SnowAvalanche3D : public Node3D {
	GDCLASS(SnowAvalanche3D, Node3D);
	friend class SnowCover3D;

	// Settings.
	float mu = 0.2f; // Voellmy dry friction (RAMMS: 0.155-0.4)
	float xi = 1500.0f; // m/s2, Voellmy turbulent friction (RAMMS: 1000-3000)
	float min_slope = 28.0f; // degrees
	float max_slope = 55.0f;
	float min_slab = 0.2f; // m
	float settle_hours = 72.0f; // e-folding of the new snow layer's weakness
	float entrainment = 0.005f; // m of snow taken per m/s per s
	float flow_density = 300.0f; // kg/m3
	float max_time = 180.0f; // s
	bool natural_release = true;
	// Triggers (follow-up 21): what stands on the slab adds Foehn's shear under it.
	bool triggers_enabled = true;
	PackedStringArray trigger_groups;
	float character_mass = 75.0f; // kg, a CharacterBody3D without snow_mass / ground_mass
	float propagation_index = 1.5f; // a fracture a load opens runs through slab with S under this
	float collapse_range = 60.0f; // m a collapse (whumpf) runs along a buried crust
	// Forests (follow-up 24).
	bool forests_enabled = true;
	float forest_friction = 0.2f; // extra dry friction under closed canopy
	float forest_detrainment = 100.0f; // Pa: snow the stems take out of the flow, K / u kg/m2/s under closed canopy
	float detrained_volume = 0.0f; // m3 stopped among trees
	Vector<float> pressure; // Pa, the highest dynamic pressure the flow put on each cell
	float _anchoring(const SnowCover3D *p_cover, int p_ci) const;
	// Wet snow (follow-up 25).
	bool wet_enabled = true;
	float wet_mu = 0.4f; // Voellmy dry friction of a wet flow
	float wet_xi = 1000.0f; // m/s2, its turbulent friction
	float wet_cohesion = 300.0f; // Pa, the yield stress that holds a wet flow's thin margins (levees)
	float wet_density = 450.0f; // kg/m3 of a wet flow
	float wet_weakening = 0.25f; // the weak layer's strength left when the pack holds all the liquid it can
	bool glide_enabled = true;
	float glide_rate = 0.5f; // m/day the whole pack glides on a wet base, at 35 degrees and 1 m deep
	float glide_release = 1.0f; // m of glide before the pack releases full depth
	Vector<float> glide; // m glided per cover cell
	Vector<uint8_t> glide_open; // a glide crack shows here
	Vector2i glide_origin;
	int glide_count = 0; // glide avalanches released
	int glide_cracks = 0;
	float run_wet = 0.0f; // the running flow's wetness, 0 dry to 1 saturated
	float mu_run = 0.2f, xi_run = 1500.0f, rho_run = 300.0f, cohesion_run = 0.0f;
	float _wet_factor(const SnowCover3D *p_cover, int p_ci) const;
	// Avalanches into water (follow-up 26).
	bool water_enabled = true;
	float water_mass = 0.0f; // kg of this entry so far
	Vector3 water_momentum, water_position; // kg m/s; mass-weighted position sum
	float water_thick = 0.0f, water_depth = 0.0f, water_slope = 0.0f; // mass-weighted sums
	Vector<uint8_t> water_cells; // cells this entry reached
	int water_cell_count = 0;
	bool water_entered_now = false;
	float water_quiet = 0.0f; // s since snow last went in
	float water_total = 0.0f; // kg into water this run
	int water_waves = 0;
	int ice_broken = 0;
	int ice_broken_drawn = 0;
	Vector3 wave_position, wave_direction;
	float wave_height = 0.0f, wave_amplitude = 0.0f, wave_depth = 0.0f, wave_product = 0.0f, wave_mass_ratio = 0.0f;
	void _water_entry(SnowCover3D *p_cover, int p_i, int p_ci, float p_speed, float p_u, float p_v, float p_depth, float p_slope);
	void _emit_wave(SnowCover3D *p_cover);
	float _hold(const SnowCover3D *p_cover, int p_ci) const { return _anchoring(p_cover, p_ci) * _wet_factor(p_cover, p_ci); }
	void _glide_advance(SnowCover3D *p_cover, float p_hours);
	// The crown (follow-up 29): a wall of snow along the fracture line, as geometry (the 2 m cells cannot hold a
	// vertical step); buried by new snow.
	struct Crown {
		MeshInstance3D *mesh = nullptr;
		Vector3 center;
		float height = 0.0f;
		float buried = 0.0f;
	};
	LocalVector<Crown> crowns;
	Vector<uint8_t> released_cells; // the last release's cells (no debris granules there)
	bool crowns_enabled = true;
	Ref<StandardMaterial3D> crown_material;
	void _build_crown(SnowCover3D *p_cover, const Vector<float> &p_released);
	void _bury_crowns(SnowCover3D *p_cover);
	bool _check_glide(SnowCover3D *p_cover);

	// Powder cloud (follow-up 23).
	bool powder_enabled = true;
	float powder_speed = 10.0f; // m/s the front must run at to throw snow up
	float powder_share = 0.3f; // of the front's snow taken into the air per 10 m it runs over the speed
	float powder_settle = 6.0f; // s, e-folding of the cloud's snow settling out
	float powder_deposit_density = 70.0f; // kg/m3 of the layer it leaves
	struct Parcel {
		Vector3 position;
		Vector3 velocity;
		float mass = 0.0f; // kg still in the air
		float owed = 0.0f; // kg settled, not yet laid on the cover
		float radius = 4.0f;
		float age = 0.0f;
	};
	LocalVector<Parcel> parcels;
	struct Bin {
		float mass = 0.0f;
		Vector3 momentum;
		Vector3 position; // mass-weighted sum
	};
	HashMap<Vector2i, Bin> bins; // the front's intake since the last parcels, by 8 m bins
	LocalVector<SnowPowderCloud3D::Source> sources;
	Vector<float> powder_pending; // kg/m2 settled onto each cover cell, not yet written
	double powder_window = 0.0;
	double powder_flush = 0.0;
	double parcel_deposit = 0.0;
	float release_liquid = 0.0f; // the released slab's liquid water, share of its mass
	float powder_taken = 0.0f; // kg into the air this avalanche
	float powder_laid = 0.0f; // kg settled back
	float powder_reach = 0.0f; // m from the release's centre to the farthest settling
	SnowPowderCloud3D *cloud = nullptr;
	void _powder_step(SnowCover3D *p_cover, float p_dt);
	void _powder_lay(SnowCover3D *p_cover, const Parcel &p_parcel, float p_mass);
	void _powder_flush(SnowCover3D *p_cover);

	int trigger_count = 0;
	int collapse_count = 0;
	Vector<uint8_t> collapsed; // cover cells whose buried crust already collapsed
	float debris_share = 0.02f; // of the deposit, broken into clumps on its surface
	float debris_radius = 60.0f; // m round the camera

	// Recent snow per cover cell (m), and its age in hours, in the cover's cell coordinates.
	Vector<float> recent;
	Vector<float> recent_age;
	Vector<float> recent_pack; // the pack's depth when recent was last updated: the new snow settles and melts with it
	Vector2i recent_origin;
	int recent_res = 0;
	uint64_t seen_update = UINT64_MAX;

	// The running flow, on the grid of the cover when it released.
	bool running = false;
	Vector2i flow_origin;
	int res = 0;
	float cell = 1.0f;
	Vector<float> h; // m
	Vector<float> hu; // m2/s
	Vector<float> hv;
	Vector<float> written; // kg/m2 of flow already written into the cover's cells
	Vector<float> deposit; // kg/m2 the flow left in each cell
	Vector<float> peak; // m/s, the fastest the flow ran through each cell
	double flow_time = 0.0;
	double flush_accum = 0.0;
	double pending_time = 0.0; // s of flow owed while a background update had the cover
	float released_volume = 0.0f; // m3
	float entrained_volume = 0.0f;
	float deposited_volume = 0.0f;
	float max_speed = 0.0f;
	float max_speed_terminal = 0.0f; // m/s, Voellmy's steady speed where and when it ran fastest
	float run_out = 0.0f; // m, the farthest the front got from the release's centre
	Vector3 release_center;
	int release_count = 0;

	SnowCover3D *_cover() const;
	void _track_recent(SnowCover3D *p_cover);
	float _slope_at(const SnowCover3D *p_cover, int p_x, int p_z) const;
	bool _check_release(SnowCover3D *p_cover);
	void _release(SnowCover3D *p_cover, const Vector<int> &p_cells, const Vector<float> &p_slab);
	void _flow_step(SnowCover3D *p_cover, float p_dt);
	void _fluxes(const Vector<float> &p_h, const Vector<float> &p_hu, const Vector<float> &p_hv, float p_dt, Vector<float> &r_dh, Vector<float> &r_dhu, Vector<float> &r_dhv) const;
	void _transport(float p_dt);
	void _flush(SnowCover3D *p_cover);
	void _debris(const Vector3 &p_at, float p_volume);
	void _hit_bodies(SnowCover3D *p_cover);
	void _finish(SnowCover3D *p_cover);
	int _cover_index(const SnowCover3D *p_cover, int p_i) const;
	// The weakest layer at a cover cell under a line load: its trigger index (strength over shear with the load)
	// and the slab over it; p_crust says the weak layer is the facets on a buried crust.
	float _trigger_index(const SnowCover3D *p_cover, int p_x, int p_z, float p_line_load, float p_sink, float &r_slab, bool &r_crust) const;
	int _load(SnowCover3D *p_cover, const Vector3 &p_at, float p_line_load, float p_sink, Node *p_body);
	void _check_triggers(SnowCover3D *p_cover);
	void _on_cornice_failed(const Vector3 &p_position, const Vector3 &p_direction, float p_length, float p_mass);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Stability index of a slab: the weak layer's shear strength (Pa) over the slab's shear stress.
	static float weak_layer_strength(float p_density, float p_age_hours); // Pa
	static float stability_index(float p_slab, float p_slab_density, float p_weak_density, float p_age_hours, float p_slope);

	// The shear a line load (N/m along it) on the surface adds at depth p_depth below it, normal to a slope of
	// p_slope radians: its peak over the angle below the load (Foehn 1987).
	static float added_shear(float p_line_load, float p_depth, float p_slope); // Pa
	// Puts a line load on the snow at a point: 0 nothing gave, 1 a slab released, 2 a buried crust collapsed
	// (whumpf) without a release. p_sink: how far the load has sunk into the snow (m).
	int apply_load(const Vector3 &p_at, float p_line_load, float p_sink = 0.0f);
	float get_trigger_index_at(const Vector3 &p_at, float p_line_load, float p_sink = 0.0f) const;
	int get_trigger_count() const { return trigger_count; }
	float get_pressure_at(const Vector3 &p_position) const; // Pa now: dense flow, else powder cloud
	float get_peak_pressure_at(const Vector3 &p_position) const; // Pa, the dense flow's highest this run
	float get_detrained_volume() const { return detrained_volume; }
	float get_glide_at(const Vector3 &p_position) const;
	static float impulse_product(float p_speed, float p_thickness, float p_mass, float p_width, float p_water_depth, float p_slope);
	static float impulse_wave_height(float p_product, float p_water_depth, float p_distance, float p_angle);
	static float impulse_wave_period(float p_mass_ratio, float p_water_depth, float p_distance);
	float get_wave_height_at(const Vector3 &p_position) const;
	float get_wave_period_at(const Vector3 &p_position) const;
	float get_water_entry_mass() const { return water_total; }
	int get_wave_count() const { return water_waves; }
	int get_ice_broken() const { return ice_broken; }
	void set_water_enabled(bool p_enable) { water_enabled = p_enable; }
	bool is_water_enabled() const { return water_enabled; }
	int get_glide_count() const { return glide_count; }
	int get_crown_count() const { return int(crowns.size()); }
	void set_crowns_enabled(bool p_enable) { crowns_enabled = p_enable; }
	bool is_crowns_enabled() const { return crowns_enabled; }
	int get_glide_crack_count() const { return glide_cracks; }
	float get_flow_wetness() const { return run_wet; }
	float get_powder_taken() const { return powder_taken; }
	float get_powder_settled() const { return powder_laid; }
	float get_suspended_mass() const;
	float get_powder_reach() const { return powder_reach; }
	float get_powder_density_at(const Vector3 &p_position) const; // kg/m3 of snow in the air
	SnowPowderCloud3D *get_powder_cloud() const { return cloud; }
	int get_collapse_count() const { return collapse_count; }

	bool trigger(const Vector3 &p_at, float p_radius); // release the slab round a point now (a skier, a cornice, a test)
	void step(double p_delta); // run the flow now, for tests
	bool is_running() const { return running; }
	int get_release_count() const { return release_count; }
	float get_released_volume() const { return released_volume; }
	float get_entrained_volume() const { return entrained_volume; }
	float get_deposited_volume() const { return deposited_volume; }
	float get_flowing_volume() const;
	float get_max_speed() const { return max_speed; }
	float get_max_speed_terminal() const { return max_speed_terminal; }
	float get_run_out() const { return run_out; }
	float get_flow_height_at(const Vector3 &p_position) const;
	Vector3 get_flow_velocity_at(const Vector3 &p_position) const;
	float get_recent_snow_at(const Vector3 &p_position) const;

	void set_mu(float p_mu) { mu = MAX(p_mu, 0.0f); }
	float get_mu() const { return mu; }
	void set_xi(float p_xi) { xi = MAX(p_xi, 1.0f); }
	float get_xi() const { return xi; }
	void set_min_slope(float p_deg) { min_slope = CLAMP(p_deg, 0.0f, 90.0f); }
	float get_min_slope() const { return min_slope; }
	void set_max_slope(float p_deg) { max_slope = CLAMP(p_deg, 0.0f, 90.0f); }
	float get_max_slope() const { return max_slope; }
	void set_min_slab(float p_m) { min_slab = MAX(p_m, 0.0f); }
	float get_min_slab() const { return min_slab; }
	void set_entrainment(float p_rate) { entrainment = MAX(p_rate, 0.0f); }
	float get_entrainment() const { return entrainment; }
	void set_natural_release(bool p_enable) { natural_release = p_enable; }
	bool is_natural_release() const { return natural_release; }
	void set_wet_enabled(bool p_enable) { wet_enabled = p_enable; }
	bool is_wet_enabled() const { return wet_enabled; }
	void set_wet_mu(float p_mu) { wet_mu = MAX(p_mu, 0.0f); }
	float get_wet_mu() const { return wet_mu; }
	void set_wet_xi(float p_xi) { wet_xi = MAX(p_xi, 1.0f); }
	float get_wet_xi() const { return wet_xi; }
	void set_wet_cohesion(float p_pa) { wet_cohesion = MAX(p_pa, 0.0f); }
	float get_wet_cohesion() const { return wet_cohesion; }
	void set_wet_density(float p_rho) { wet_density = MAX(p_rho, 50.0f); }
	float get_wet_density() const { return wet_density; }
	void set_wet_weakening(float p_share) { wet_weakening = CLAMP(p_share, 0.01f, 1.0f); }
	float get_wet_weakening() const { return wet_weakening; }
	void set_glide_enabled(bool p_enable) { glide_enabled = p_enable; }
	bool is_glide_enabled() const { return glide_enabled; }
	void set_glide_rate(float p_rate) { glide_rate = MAX(p_rate, 0.0f); }
	float get_glide_rate() const { return glide_rate; }
	void set_glide_release(float p_m) { glide_release = MAX(p_m, 0.01f); }
	float get_glide_release() const { return glide_release; }
	void set_forests_enabled(bool p_enable) { forests_enabled = p_enable; }
	bool is_forests_enabled() const { return forests_enabled; }
	void set_forest_friction(float p_mu) { forest_friction = MAX(p_mu, 0.0f); }
	float get_forest_friction() const { return forest_friction; }
	void set_forest_detrainment(float p_pa) { forest_detrainment = MAX(p_pa, 0.0f); }
	float get_forest_detrainment() const { return forest_detrainment; }
	void set_powder_enabled(bool p_enable) { powder_enabled = p_enable; }
	bool is_powder_enabled() const { return powder_enabled; }
	void set_powder_speed(float p_speed) { powder_speed = MAX(p_speed, 0.0f); }
	float get_powder_speed() const { return powder_speed; }
	void set_powder_share(float p_share) { powder_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_powder_share() const { return powder_share; }
	void set_powder_settle(float p_seconds) { powder_settle = MAX(p_seconds, 0.1f); }
	float get_powder_settle() const { return powder_settle; }
	void set_triggers_enabled(bool p_enable) { triggers_enabled = p_enable; }
	bool is_triggers_enabled() const { return triggers_enabled; }
	void set_trigger_groups(const PackedStringArray &p_groups) { trigger_groups = p_groups; }
	PackedStringArray get_trigger_groups() const { return trigger_groups; }
	void set_character_mass(float p_kg) { character_mass = MAX(p_kg, 0.0f); }
	float get_character_mass() const { return character_mass; }
	void set_propagation_index(float p_index) { propagation_index = MAX(p_index, 0.0f); }
	float get_propagation_index() const { return propagation_index; }
	void set_collapse_range(float p_m) { collapse_range = MAX(p_m, 0.0f); }
	float get_collapse_range() const { return collapse_range; }

	SnowAvalanche3D();
};

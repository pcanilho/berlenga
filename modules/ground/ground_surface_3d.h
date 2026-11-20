/**************************************************************************/
/*  ground_surface_3d.h                                                   */
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

#pragma once

#include "modules/world/world_contracts.h"
#include "ground_deform_field.h"
#include "ground_deform_gpu.h"
#include "ground_height_query.h"
#include "ground_ripples.h"
#include "ground_rut_store.h"
#include "ground_terramechanics.h"

#include "core/object/worker_thread_pool.h"
#include "core/variant/typed_array.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class GroundClods3D;
class MeshInstance3D;
class WorldWeather;

// Footprints, ruts and dents in bare ground (docs/ideas/ground-deform.md): sand, soil and mud, and the
// leaf litter over them, pressed by bodies in deform_groups with Bekker sinkage (Wong's soil values),
// mass conserving, with walls slumping to the material's angle of repose (Sumner, O'Brien and Hodgins
// 1999). The ground's moisture from the weather map turns soil to mud and dry sand to damp sand; tree
// crowns drop litter; bare rock does not print. The field is drawn as a patch with the ground's own
// material, and the terrain, drawn with the same material (BaseMaterial3D ground_deform_enabled, or
// get_ground_deform_shader_code() in a shader), is lowered under it, so both renderers trace the prints.
class GroundSurface3D : public Node3D, public WorldWaterProvider, public WorldGroundProvider {
	GDCLASS(GroundSurface3D, Node3D);

public:
	enum GroundType {
		GROUND_SAND,
		GROUND_SOIL,
	};

	// What a lattice cell of ground is made of, from the rules (moisture, sand share, litter, rock).
	struct Cell {
		float n = 1.0f;
		float kc = 1.0f;
		float kphi = 1000.0f;
		float rim_share = 0.5f;
		float tan_repose = 0.7f;
		float rate = 1.0f; // slumping share per second
		float max_dent = 0.1f;
		// Mud slides (docs/ideas/mud.md, round 10): the mud's yield stress over its weight, tau_y / (rho g), m; a
		// layer h deep holds on ground up to slide_length / h steep, past it flows down (1e3: holds anywhere).
		float slide_length = 1e3f;
		// Waves in liquid mud (round 14.1): the layer's yield stress and viscosity over its density, m2/s2 and m2/s
		// (wave_nu 0: not liquid, it slumps).
		float wave_tau = 0.0f;
		float wave_nu = 0.0f;
		// Gullies (round 11): the bed shear runoff needs to start cutting the ground, Pa (sand 0.19, Shields).
		float crit_shear = 0.19f;
		float darken = 0.3f; // albedo lost where freshly turned
		float gloss = 1.0f; // roughness factor where freshly turned
		float litter = 0.0f; // m of litter on top
		float sand = 0.0f; // share of sand
		float blown = 0.0f; // share the wind moves: dry, bare sand (damp grains hold, Belly 1964)
		// Soil strength (docs/ideas/sand.md, stage 1; Wong 2008, table 2.2).
		float cohesion = 1.0f; // kPa
		float tan_phi = 0.53f; // internal friction
		float shear_k = 0.025f; // m, Janosi-Hanamoto shear deformation modulus
		float unit_weight = 15.7f; // kN/m3
		// Pressure-sinkage under a wheel. Bekker's values depend on the plate size they were measured with (Wong
		// 2008, sec. 2.2): n, kc, kphi above are a boot's (and set the prints' look); a tyre's come from plates
		// of its own size. They differ where the soil stiffens under load: damp, packed sand.
		float wheel_n = 1.0f;
		float wheel_kc = 1.0f;
		float wheel_kphi = 1000.0f;
		// Deep mud (docs/ideas/mud.md, stage 1): the soft layer's thickness over the firm base, m (0: none).
		float mud = 0.0f;
		// How the soil sticks to rubber and steel (docs/ideas/mud.md, stage 3): 0..1, and the tangential adhesion
		// (kPa, Wong's ca).
		float stickiness = 0.0f;
		float adhesion = 0.0f;
		bool deformable = true;
	};
	// p_saturation: pore water in the top layer, 0..1 (< 0: none known, p_moisture alone); p_water_speed: water
	// flowing over it, m/s (sand stage 4).
	// p_mud: deep mud over the firm base, m (stage 1 of docs/ideas/mud.md), already scaled by how wet it is.
	static Cell material_cell(float p_sand, float p_moisture, float p_litter, float p_saturation = -1.0f, float p_water_speed = 0.0f, float p_mud = 0.0f);
	// Mud as a Bingham fluid (O'Brien and Julien 1988, Aspen Pit 1): yield stress (Pa) and viscosity (Pa s) at a
	// volumetric sediment concentration p_cv (0.40 slurry to 0.56 stiff).
	static float mud_yield_stress(float p_cv);
	static float mud_viscosity(float p_cv);
	// The sediment concentration of mud at a moisture: stiff (0.56) where the ground first turns to mud (0.35),
	// slurry (0.40) soaked.
	static float mud_concentration(float p_moisture) { return Math::lerp(0.56f, 0.40f, CLAMP((p_moisture - 0.35f) / 0.65f, 0.0f, 1.0f)); }
	// Bed load under flowing water (Meyer-Peter and Mueller 1948) for fine sand, m2/s per m across the flow.
	static float bed_load(float p_water_speed);

private:
	NodePath follow_target;
	NodePath height_source;
	NodePath snow_cover;
	uint32_t ground_collision_mask = 1;
	GroundType ground_type = GROUND_SOIL;
	float moisture = -1.0f; // -1: from the weather map
	float litter_depth = 0.04f;
	float litter_cover = 0.0f; // canopy closure to use with no weather map
	bool rock_from_geology = true;
	Ref<Image> type_map; // R: sand share, G: bare rock
	Rect2 type_map_rect;
	// Deep mud (docs/ideas/mud.md, stage 1): painted (mud_map R times mud_max_depth over mud_map_rect, as
	// MudRunner's mud brush) and from the terrain (a TerrainDataMap3D's wetness index and distance to water, at
	// most mud_auto_depth). It counts only when wet: none below mud_wet_start moisture, all at mud_wet_full.
	bool mud_layer = true;
	bool mud_slides = true; // round 10: a soaked layer flows off slopes it no longer holds
	bool mud_waves = true; // round 14.1: liquid mud runs as a shallow Bingham flow (bow waves, rut closing)
	bool mud_liquid_any = false;
	bool simulation_paused = false;
	bool light_budget = false; // still shots: smaller GPU fields and tile cache
	bool subsoil = true; // round 12: a firm base under the loose layer, its depth varied across the ground
	HashMap<Vector2i, float> mud_deposits; // m of fresh slurry per 1 m cell (debris flows' fans, flood silt)
	// Worn-in tracks (docs/ideas/mud.md, round 14.2): the wheel passes each 25 cm cell has taken, decaying as the
	// sward regrows and the soil loosens, and when it was last pressed (a pass starts when a cell pressed is not
	// already under a wheel).
	struct Wear {
		float passes = 0.0f;
		double last = -1e9;
	};
	static constexpr float WEAR_CELL = 0.25f;
	static constexpr float WEAR_GAP = 0.25f; // s a cell must go unpressed before the next press is a new pass
	HashMap<Vector2i, Wear> wear_cells;
	// Hidden stones and roots (docs/ideas/mud.md, round 14.3): what lies in a mud layer, per 2 m cell, made on demand
	// from the cell's hash; a stone a sphere (a == b), a root a capsule from a to b; y's are absolute.
	struct Buried {
		Vector3 a;
		Vector3 b;
		float r = 0.1f;
		bool root = false;
		uint32_t id = 0;
	};
	static constexpr float BURIED_CELL = 2.0f;
	bool buried_obstacles = true;
	float buried_density = 0.05f; // per m2 of deep mud
	float buried_body_range = 16.0f;
	mutable HashMap<Vector2i, LocalVector<Buried>> buried_cache;
	HashMap<uint32_t, ObjectID> buried_bodies;
	double buried_accum = 0.0;
	const LocalVector<Buried> &_buried_cell(const Vector2i &p_cell) const;
	float _buried_depth(const Vector3 &p_position, float p_radius) const;
	void _buried_update();
	void _buried_clear_bodies();
	Ref<ArrayMesh> buried_meshes[6]; // four stones, two roots
	Ref<Material> buried_materials[2];
	Ref<ArrayMesh> _buried_mesh(bool p_root, uint32_t p_seed);
	bool worn_tracks = true;
	float wear_regrowth_days = 30.0f;
	double wear_clock = 0.0;
	bool wear_dirty = false;
	Vector2i wear_origin = Vector2i(INT32_MAX, INT32_MAX);
	Ref<ImageTexture> wear_texture;
	void _wear_press(const Vector3 &p_position, const Vector2 &p_half, float p_yaw);
	void _wear_update(double p_delta);
	void _wear_publish();
	HashMap<Vector2i, Color> mud_deposit_colors; // its linear colour where the depositor gave one (alpha: cover)
	Color _deposit_color_at(float p_x, float p_z) const; // premultiplied by cover, bilinear between cell centres
	bool flood_silt = true; // other water over the ground leaves its suspended load as fresh mud
	// Stream fords (docs/ideas/mud.md, round 13.2): wheels in running water scour the bed and send its fines
	// downstream; wheels on a saturated bank break it down into the water.
	bool stream_fords = true;
	HashMap<Vector2i, uint64_t> bank_failed; // 1 m cell: when its bank last gave way (ms), so one failure is one event
	float ford_plume_kg = 0.0f; // sent into the water by wheels and banks
	float ford_scour_m3 = 0.0f;
	int bank_failures = 0;
	void _ford(const Vector3 &p_position, const Vector2 &p_half, float p_yaw, float p_sinkage, const Vector2 &p_velocity);
	float silt_total = 0.0f; // kg/m2 laid since start, summed over the 1 m points
	float silt_pending = 0.0f;
	float silt_timer = 0.0f;
	bool silt_dirty = false;
	void _flood_silt(const Vector3 &p_position, float p_depth, float p_speed, const Vector2 &p_load, float p_dt);
	// Standing water (docs/ideas/mud.md, stage 2): rain, runoff and ground water gather in ruts and prints, on the
	// GPU field (the virtual pipe model), drawn as the surface there in both renderers.
	bool standing_water = true;
	// Muddy water (docs/ideas/mud.md, round 3): standing water carries the clay tyres churn up and feet stir in.
	bool muddy_water = true;
	// Mud tracked onto firm ground (round 3): tyres and boots leave the clay they carry as a film (track_mud).
	bool tracked_mud = true;
	bool mud_sliding = false; // round 10: some cell's mud layer is on ground steeper than it holds
	bool water_seep = false; // some lattice cell's water table lies within a print's reach: seepage can fill one
	float water_seen = 0.0f; // the deepest standing water at the last read back, m
	float _evaporation() const;
	int _water_passes(const GroundDeformField &p_field, double p_delta) const;
	int _morph_passes() const;
	// Drying and desiccation cracks (docs/ideas/mud.md, stage 2b): per lattice cell (1 + the share of its water the
	// drying layer still holds, or 0 where it was never soaked; the wetting and drying cycles it has cracked in; the
	// deepest water that stood on it, m, which sets the layer of clay it settled).
	static constexpr int CRACK_FLOATS = 3;
	bool desiccation_cracks = true;
	LocalVector<float> crack_state;
	void _crack_update(double p_delta);
	float _crack_layer(int p_i) const; // the drying layer's thickness, m
	Vector4 _crack_at_cell(int p_i, float *r_damp = nullptr) const; // opening 0..1, plate size m, regularity 0..1, crust 0..1
	// Puddle ice (docs/ideas/mud.md, round 7): per lattice cell (the ice's thickness, m; the share of it broken into
	// floating plates, 0..1).
	static constexpr int ICE_FLOATS = 2;
	bool puddle_ice = true;
	LocalVector<float> ice_state;
	void _ice_update(double p_delta);
	void _ice_break(const Vector3 &p_position, float p_radius, float p_bearing);
	// Frozen ground (round 8): per lattice cell (the thawed layer on top, m; the frost's depth, m; the thaw's
	// weakening, 0..1; round 9's needle ice: its height, m; the loose soil it leaves, 0..1; the height laid on the
	// field, m).
	static constexpr int FROST_FLOATS = 6;
	bool frozen_ground = true;
	bool needle_ice = true;
	int needle_cursor = 0;
	float _needle_loose_at(float p_x, float p_z) const;
	LocalVector<float> frost_state;
	void _frost_update(double p_delta);
	void _frost_seed(int p_from);
	float _frost_crust(int p_i) const;
	// Mud on plants (round 7): per lattice cell, the share of the sward's and the plants' faces spattered with clay.
	bool plant_mud = true;
	LocalVector<float> plant_mud_state;
	void _plant_mud_add(const Vector3 &p_position, float p_radius, float p_share);
	void _plant_mud_update(double p_delta);
	Ref<Image> mud_map;
	Rect2 mud_map_rect;
	float mud_max_depth = 1.2f;
	float mud_auto_depth = 0.5f;
	float mud_wet_start = 0.35f;
	float mud_wet_full = 0.55f;
	// The clay's colour (sRGB; mud stage 4d), or where mud_color_map covers mud_map_rect its RGB: a vehicle that drives
	// through it carries this colour on its body (red laterite gives red mud, grey clay grey).
	Color mud_color = Color(0.45f, 0.36f, 0.27f);
	Ref<Image> mud_color_map;
	NodePath data_map;
	float _mud_depth_at(const Vector3 &p_position, float p_moisture, bool p_sand_or_rock, const Node *p_data_map) const;

	float deform_extent = 32.0f;
	int deform_resolution = 512;
	float ground_spacing = 0.25f;
	float detail_spacing = 0.0625f;
	float outer_spacing = 0.125f;
	PackedStringArray deform_groups;
	bool ploughing = true;
	// Mats (docs/ideas/mud.md, round 12): logs and branches laid on soft ground, as a corduroy road. Rigid bodies in
	// the "ground_mats" group with a cylinder or capsule shape: once they settle the ground takes them (frozen,
	// lying on the deformed surface, in the ruts), a wheel on one bears on the wood, and the log spreads that load
	// along its length into the soil and sinks under it.
	bool mats = true;
	struct Mat {
		ObjectID id;
		float length = 1.0f;
		float radius = 0.05f;
		float sink = 0.0f; // m, below the undisturbed ground under it
		float pressed = 0.0f; // m, what the field has been pressed to
		float load = 0.0f; // N, wheels on it since the last update
		float peak = 0.0f; // N, the most it has carried (its load over a fifth of a second)
		float carried = 0.0f; // N, the load smoothed over a fifth of a second
		float friction = 0.5f; // its body's
		bool broken = false; // snapped under a load past its strength: no longer a mat
		bool buried = false; // under a debris-flow deposit deeper than it is thick: gone from sight and from under wheels
		Vector3 a, b; // its axis ends, world
	};
	LocalVector<Mat> mat_list;
	void _update_mats(float p_dt);
	bool _mat_shape(Node *p_body, Vector3 &r_a, Vector3 &r_b, float &r_radius) const;

public:
	struct Spray {
		Vector3 position;
		Vector3 velocity;
		float width = 0.0f;
		float slip_speed = 0.0f;
		// The loose sand the tread reaches: the contact and the rim heaped before and behind it.
		Vector3 reach_center;
		float reach_radius = 0.0f;
		// A wheel sliding sideways: the share of the slide that is sideways, and the sand its sidewall shoves
		// out of the side of the turn, m3/s (beside what its tread throws).
		float side_share = 0.0f;
		float side_volume_rate = 0.0f;
		// The soil's stickiness there (0 sand .. 1 sticky clay): a grains node throws sticky sprays as mud.
		float stickiness = 0.0f;
		// Clay flung off a tyre's tread (mud stage 4), m3: thrown as it is, not dug from the field.
		float volume = 0.0f;
		// Standing water a tyre throws (mud stage 4c), m3: taken from the field over the reach disc when thrown.
		float water = 0.0f;
		// The clay that water carries, kg/m3 (muddy water, round 3).
		float water_mud = 0.0f;
	};
	// Clay thrown and landed (track_mud with no print): for GroundClods3D, which stands the bigger ones up as lumps.
	struct ClodDrop {
		Vector3 position;
		float radius = 0.0f;
		float volume = 0.0f; // m3
		Color color; // the clay's (get_mud_color_at)
		float wet = 0.0f;
	};
	// Fines lifted from dry, bare sand by a tyre (sand drifting, Gillies et al. 2005): for a dust node.
	struct Dust {
		Vector3 position;
		Vector3 velocity; // the plume's starting jet, m/s
		float rate = 0.0f; // kg/s
	};

private:
	LocalVector<Spray> sprays;
	LocalVector<Dust> dust;
	LocalVector<Dust> near_dust; // inside the claim (claim_dust): for a near-field dust node
	uint64_t dust_frame = 0;
	Vector3 dust_claim_center;
	float dust_claim_radius = 0.0f;
	uint64_t dust_claim_frame = 0;
	float dust_emission = 0.06f; // kg/s per metre of tyre width per m/s over fully dry, bare sand
	uint64_t spray_frame = 0;
	LocalVector<ClodDrop> clod_drops;
	uint64_t clod_frame = 0;
	bool clods = true; // GroundClods3D: lumps of clay near the camera (ground detail)
	GroundClods3D *clods_node = nullptr;
	void _plough_bodies();
	// Slip-face avalanches (docs/ideas/sand.md, dunes): a press on a dry dune's slip face sets off a grain flow down
	// it, a tongue that scours the face's surface layer and lays it in a lobe at the toe.
	bool slip_avalanches = true;
	static constexpr float AVAL_TENT = 1.6f; // the scour's and lobe's tents, times the flow's half width
	struct Avalanche {
		LocalVector<Vector3> path; // x, z, the half width at each 4 cm step down the fall line (y)
		int lobe_from = 0; // the first step of the lobe at the toe
		int done = 0; // steps applied
		float front = 0.0f; // m of path the flow has reached
		float scoured = 0.0f; // m3 taken from the face so far
		float lobe_area = 0.0f; // m2 the lobe's discs cover, summed
		uint32_t seed = 0;
	};
	LocalVector<Avalanche> avalanches;
	struct AvalancheMark {
		Vector2 at;
		uint64_t usec = 0;
	};
	LocalVector<AvalancheMark> avalanche_marks;
	void _avalanche_trigger(const Vector3 &p_position, float p_radius, float p_bottom);
	void _avalanche_update(double p_delta);
	static float _aval_lobe_weight(const Avalanche &p_a, int p_k);
	float contact_tolerance = 0.03f;
	float character_mass = 75.0f;
	float character_contact_area = 0.035f;
	int max_cached_tiles = 1024;

	Ref<Material> material;
	float skirt = 0.5f;
	float terrain_margin = 2.0f;
	float skirt_fade = 2.0f;

	float drying_time = 900.0f; // s
	float sand_drift_rate = 2.0f; // per hour at 10 m/s
	float rain_smoothing = 0.02f; // per hour per mm/h
	Vector2 wind_override; // m/s at 10 m; zero: the weather map's

	// Wind ripples (docs/ideas/sand.md, stage 3): a fine height layer over the middle of the field, its own
	// finer ring of the patch. Off by default until its cost is measured.
	bool wind_ripples = true;
	// Wind fills prints from their upwind side (_drift_prints()).
	bool wind_fills_prints = true;
	// Dry ground up to WATER_FRINGE over a water body's level is kept wet by capillary rise (_water_update()).
	bool water_fringe = true;
	// Pebbles and gravel (WorldGround.coarse_cover_at) shelter the sand between them: no blowing, no ripples.
	bool coarse_armour = true;
	static constexpr float WATER_FRINGE = 0.15f; // m, fine sand
	// A pale salt crust at the fringe's top round still water in a dry climate (_water_update()).
	bool salt_crusts = true;
	LocalVector<float> salt_crust; // per lattice point, 0..1
	// Ripples fade out where the wind's lee stops the sand (_ripple_calm_at()).
	bool lee_calm_ripples = true;
	static constexpr float PRINT_DRIFT_LENGTH = 0.5f; // m: c = this times the fade rate (about 1 m an hour at 10 m/s)
	static constexpr float PRINT_DRIFT_SOFTEN = 0.4f; // of the old even fade kept with it
	float ripple_time_scale = 1.0f;
	float ripple_extent = 4.0f; // m, the square of real ripple geometry round the follow point
	GroundRipples ripples;
	double ripple_q_accum = 0.0;
	bool ripple_dirty = false;
	bool ripple_published = false;
	double ripple_mask_accum = 1e9;
	LocalVector<float> ripple_show; // sand share the ripples show on (the texture's second channel)
	// Far ripples: past the window the grown field (periodic) shades the rest of the patch as a normal. The
	// ripple map carries, under the window's rows, the sand share over the patch and a row of numbers, so the
	// path tracer reads all of it from one texture (rt param 68).
	static constexpr int RIPPLE_SAND_N = 128;
	LocalVector<float> ripple_sand; // RIPPLE_SAND_N squared over ripple_sand_rect
	Rect2 ripple_sand_rect;
	LocalVector<float> ripple_tile_slope; // the grown field's slope, 2 per cell, world cell mod n
	Ref<Image> ripple_far_image;
	Ref<ImageTexture> ripple_far_texture;
	bool ripple_far_published = false;
	void _ripples_far_tile();
	LocalVector<float> ripple_seen; // the field's surface change at each of its texels over the window
	Vector2i ripple_seen_origin;
	LocalVector<float> ripple_change; // its change this update (scratch)
	Ref<Image> ripple_image;
	Ref<ImageTexture> ripple_texture;
	WorkerThreadPool::TaskID ripple_task = WorkerThreadPool::INVALID_TASK_ID;
	LocalVector<float> ripple_grown;
	Vector2 ripple_grown_k;
	Vector2 ripple_grow_wind;
	int ripple_steps = 0;
	static void _ripple_grow_task(void *p_self);
	void _ripples_update(double p_delta);
	void _ripples_publish();
	Vector2 _wind_10m() const;

public:
	// For the streamers (GroundStreamers3D): the 10 m wind over the field and the share of sand the wind can
	// move at a place.
	Vector2 get_wind_10m() const { return _wind_10m(); }
	float get_blown_at(float p_x, float p_z) const { return _cell_at(p_x, p_z).blown * _shelter_flux_at(p_x, p_z); }

private:
	GroundDeformField field;
	struct Columns : public GroundDeformField::Columns {
		const GroundSurface3D *surface = nullptr;
		GroundDeformField::Column column(float p_x, float p_z) const override;
		float sinkage(const GroundDeformField::Body &p_body) const override;
		bool skips(const Node3D *p_node) const override { return p_node->has_meta(SNAME("ground_self_pressing")); }
	};

	// The ground lattice over the field, one sample every ground_spacing, and its cells.
	// The field on the GPU (docs/ideas/sand.md, stage 0): the same maths as the CPU path, which stays as the
	// fallback where there is no RenderingDevice. The CPU field is then the copy read back for queries.
	bool gpu_simulation = true;
	struct GPUSink : public GroundDeformField::StampSink {
		GroundSurface3D *surface = nullptr;
		bool is_near = false; // the near field's sink (the main one forwards to it)
		GPUSink *next = nullptr;
		void stamp(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance, Vector2 p_velocity) override;
		void amount(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, float p_depth, float p_cell = 0.0f) override;
	};
	GPUSink gpu_sink;
	GPUSink near_sink;
	GroundDeformGPUHost gpu_host;
	bool gpu_ground_dirty = true;
	bool gpu_cells_dirty = true;
	double gpu_slow_accum = 0.0;
	double gpu_readback_accum = 0.0;
	bool _gpu_on() const;
	bool _runoff_on_gpu() const { return runoff_channels && _gpu_on(); }
	float _runoff_seep(int p_i) const;
	bool runoff_seep_any = false; // some lattice cell seeps (GPU runoff)
	void _gpu_submit(double p_delta, double p_age, bool p_readback, bool p_sync);
	int _mud_wave_passes(const GroundDeformField &p_field, double p_delta) const;
	void _gpu_submit_field(GroundDeformGPUHost &r_host, const GroundDeformField &p_field, bool &r_ground_dirty, bool &r_cells_dirty, double &r_slow_accum, int p_passes, float p_edge_margin, double p_delta, double p_age, bool p_readback, bool p_sync);
	void _gpu_take();
	void _gpu_release();
	PackedByteArray _gpu_params(const GroundDeformField &p_field, float p_edge_margin, double p_delta, double p_age, double p_slow) const;

	// The finer near field (docs/ideas/mud.md, stage 0): a second, smaller field at a quarter of the texel
	// (8 m at 1.5625 cm), centred on the main one, pressed with the same stamps and slumped with the same rules
	// (more passes, so a slope moves as far in a second). The material and the path tracer use it inside its
	// square, blended into the main field over its last half metre; the patch gains a matching fine ring. Tiles it
	// has never held start as the main field's copy. GPU path only.
	bool near_field = true;
	float near_extent = 8.0f;
	int near_resolution = 512;
	GroundDeformField near_fld;
	GroundDeformGPUHost near_host;
	bool near_ground_dirty = true;
	bool near_cells_dirty = true;
	double near_slow_accum = 0.0;
	static constexpr float NEAR_BLEND = 1.0f; // m over which the near field fades into the main one (material.cpp, raygen)
	bool _near_on() const;
	void _near_follow();
	void _near_seed(Vector2i p_origin);
	void _near_release();
	int _near_passes() const;
	bool _near_inside(float p_x, float p_z) const;
	// Ruts kept across the world (mud round 3): tiles leaving a field go to disk, and come back when it returns.
	bool keep_ruts = true;
	String ruts_path = "user://ground_ruts";
	float ruts_time_scale = 1.0f; // world seconds per real second while away (0: kept as left)
	float ruts_away_rain = 0.1f; // mean rain while away, mm/h (a temperate 900 mm a year)
	float ruts_regrowth_time = 2.6e6f; // s: the sward grows back over a kept rut at this e-fold (a month; mud round 6)
	double _growing_seconds(double p_seconds) const;
	void _ruts_age(LocalVector<uint32_t> &r_data, double p_seconds) const;
	// Crack state kept with the ruts (mud round 5): the lattice cells it moves off and onto.
	GroundCellStore crack_store;
	void _cracks_move(Vector2i p_origin, int p_n);
	void _cracks_age(float *r_cell, double p_seconds) const;
	void _cracks_save();
	GroundRutStore rut_main;
	GroundRutStore rut_near;
	LocalVector<Vector2i> rut_main_pending; // world tiles in the field waiting to be read in from disk
	LocalVector<Vector2i> rut_near_pending;
	bool _ruts_on() const;
	void _ruts_configure();
	void _ruts_leave(const GroundDeformField &p_field, GroundRutStore &r_store, Vector2i p_origin, const LocalVector<Vector2i> &p_pending);
	void _ruts_arrive(const GroundDeformField &p_field, GroundRutStore &r_store, const GroundDeformGPUHost *p_host, Vector2i p_origin, LocalVector<Vector2i> &r_pending);
	void _ruts_read_cpu(GroundDeformField &r_field, GroundRutStore &r_store, LocalVector<Vector2i> &r_pending);
	void _ruts_read_gpu(const GroundDeformField &p_field, GroundRutStore &r_store, GroundDeformGPUHost &r_host, LocalVector<Vector2i> &r_pending);
	void _ruts_save_field(const GroundDeformField &p_field, GroundRutStore &r_store, const LocalVector<Vector2i> &p_pending);
	void _publish_near();
	PackedByteArray _gpu_cells() const;

	int ground_n = 0;
	Vector2i ground_origin;
	bool ground_valid = false;
	LocalVector<float> ground;
	LocalVector<Cell> cells;
	// Each cell's inputs, so water can redo one cell without the type map and weather lookups.
	struct CellInput {
		float sand = 0.0f;
		float moisture = 0.0f;
		float litter = 0.0f;
		float mud = 0.0f; // deep mud, m (stage 1 of docs/ideas/mud.md)
		bool fixed = false; // rock or snow: never deforms
		float frozen = 0.0f; // frozen soil at the surface (round 8): 0..1, how far it holds as a crust
		float repel = 0.0f; // a fire's water repellency (WorldFire.water_repellency_at), 0..1
		float compacted = 0.0f; // worn-in tracks (round 14.2): how far the passes have packed the soil, 0..1
	};
	LocalVector<CellInput> cell_inputs;
	float _water_conductivity(const CellInput &p_in) const;
	float _water_table_depth(int p_i) const;
	// Sand and water (docs/ideas/sand.md, stage 4): pore water and the water over each cell.
	TypedArray<NodePath> water_sources;
	bool water_coupling = true;
	LocalVector<float> pore; // saturation of the top 2 cm, 0..1, per lattice cell
	LocalVector<float> water_speed; // m/s of water over the cell, 0 dry
	LocalVector<float> cell_water; // the saturation and speed the cell was last built with (2 per cell)
	LocalVector<float> water_depth; // m of water over the cell (the last water update's)
	LocalVector<float> water_body_depth; // of it, the sea's, a lake's or a river's alone (WorldWater; no standing water)
	struct Wader {
		int64_t id = 0;
		Vector3 axle;
		float radius = 0.0f;
		float half_width = 0.0f;
		Vector3 velocity;
		uint64_t frame = 0;
	};
	mutable LocalVector<Wader> waders; // add_wader()'s, this physics frame's and the last
	struct WaterDisc {
		Vector3 position;
		float radius = 0.0f;
		double volume = 0.0; // m3, < 0 taken
		float mud = 0.0f; // kg/m3 of clay the added water carries
	};
	struct StirDisc {
		Vector3 position;
		float radius = 0.0f;
		float mass = 0.0f; // kg of clay
	};
	LocalVector<StirDisc> stir_discs; // stir_water()'s, applied by the next water step
	LocalVector<WaterDisc> water_discs; // add_water()'s, applied by the next water step
	struct PendingPuddle {
		Vector3 position;
		float radius = 0.0f;
		float level = 0.0f; // the water's surface, m (world y)
		float mud = 0.0f;
		double wait = 0.0; // s until the dug hollow is in the field's read back
	};
	LocalVector<PendingPuddle> pending_puddles; // add_puddle()'s, filled once dug
	void _puddles_update(double p_delta);
	void _add_puddle(const Vector3 &p_position, float p_radius, float p_depth, float p_mud, bool p_fill);
	// Puddles the world places (mud round 6): candidate sites on a 4 m grid, each seen inside the field a while.
	bool world_puddles = true;
	float world_puddle_density = 1.0f; // puddles per 100 m2 where the ground fully suits one
	HashMap<Vector2i, double> world_puddle_seen; // site -> time first seen inside the field (< 0: done)
	double world_puddle_clock = 0.0;
	void _world_puddles_update(double p_delta);
	float _world_puddle_suit(const Vector3 &p_position) const;
	double water_added = 0.0; // m3, the discs applied so far (taken < 0)
	// What takes could not find (the GPU's ledger, read back), and how much of it later water added has paid.
	uint32_t water_owed_seen[2] = { 0, 0 };
	double water_owed = 0.0;
	void _water_owed_update();
	static constexpr int WATER_DISCS = 16;
	// Texel centres of p_field within p_radius of p_position (the disc's area in that field's texels).
	static int _disc_texels(const GroundDeformField &p_field, const Vector3 &p_position, float p_radius);
	LocalVector<float> water_table; // m from the surface down to the water table a sea or lake left (runoff_channels)
	double water_accum = 0.0;
	double puddle_accum = 0.0;
	float water_usec = 0.0f;
	void _water_update(double p_delta);
	// The far pore water (docs/ideas/mud.md, stage 0): the same balance on a lattice FAR_STEP times coarser with as
	// many cells, centred on the patch, so wet ground carries on past it over the terrain with no edge. A
	// FAR_PHASES-th of its rows is sampled each water step; inside the fine lattice it copies the fine one.
	static constexpr int FAR_STEP = 4;
	static constexpr int FAR_PHASES = 8;
	LocalVector<float> far_pore;
	LocalVector<float> far_table;
	LocalVector<float> far_ground;
	Vector2i far_origin;
	int far_n = 0;
	int far_phase = 0;
	double far_age[FAR_PHASES] = {};
	void _far_update(double p_delta, const LocalVector<Node *> &p_sources, const WorldWeather *p_weather);
	float _far_pore_at(float p_x, float p_z) const;
	float _pore_step(float p_s, float p_depth, float p_contact, float p_dt, float &r_table) const;
	static float _value_noise(float p_x, float p_z);
	float _subsoil_factor(float p_x, float p_z) const;
	// The look of the pore water (Lagarde 2013): ground_wet_map, n x (n + 1) RGBAF, R wetness, G the saturation (the standing film from 0.95, taken after filtering),
	// B the share of loose dry sand showing its grains (sand_glints), the last row's texel 0 the rect (x0, z0, size, tag 2.5). Read by the raster and path-traced ground alike.
	Vector<uint8_t> wet_data;
	Ref<ImageTexture> wet_texture;
	bool wet_published = false;
	bool sand_glints = true;
	double wet_publish_accum = 0.0;
	void _wet_publish(bool p_on);
	// Vegetation sheltering the sand (dune grass, docs/ideas/sand.md): plants take a share of the wind's stress
	// (Raupach 1993 drag partition), the saltation flux falls among them and the sand it carried lands there.
	TypedArray<NodePath> wind_shelters;
	bool obstacle_lee = true; // blown sand reads WorldWeather.obstacle_lee_at
	LocalVector<float> shelter_ratio; // the surface's friction speed over the open ground's, per lattice cell
	LocalVector<float> shelter_flux; // the flux factor, Q(u* ratio) / Q(u*), 0..1 (1 with no plants)
	LocalVector<float> sand_flux; // kg/m/s, along the wind
	LocalVector<float> sand_dep_rate; // m/s, + piling up, - scoured
	LocalVector<float> sand_dep; // m written to the ground since the cell came in
	LocalVector<float> ground_dep; // m of mud deposit (debris fans, flood silt) the ground lattice carries, per cell
	float _deposit_smooth_at(float p_x, float p_z, float *r_coloured = nullptr) const; // r_coloured: the share with a colour
	bool _apply_deposits();
	void _ground_changed();
	LocalVector<float> sand_dep_pending; // m owed, written in 2 mm steps
	LocalVector<int> wind_order; // the cells in downwind order
	Vector2 wind_order_dir;
	Vector2 sand_flux_dir;
	double wind_accum = 0.0;
	float wind_usec = 0.0f;
	void _wind_update(double p_delta);
	// Runoff channels (docs/ideas/sand.md): water seeping out of soaked sand and rain the ground cannot take run
	// downhill as sheet flow on a grid of twice the field's texel, gather and carry sand, cutting rills.
	bool runoff_channels = true;
	float runoff_time_scale = 1.0f;
	Vector2i runoff_origin;
	int runoff_n = 0;
	LocalVector<float> runoff_depth; // m of flowing water per runoff cell
	LocalVector<float> runoff_speed; // m/s
	LocalVector<float> runoff_pending; // m owed to the ground, + cut, - laid down, written in 1 mm steps
	LocalVector<float> runoff_cut; // m cut (+) or laid down (-) since the cell came in
	double runoff_accum = 0.0;
	float runoff_usec = 0.0f;
	void _runoff_update(double p_delta);
	// Swash marks (docs/ideas/sand.md): each swash leaves a thin ridge of fine sand at its uprush limit. Tracked on
	// shore-normal transects across the near field; each transect's run-up is followed and, when the swash turns
	// back, a ridge is laid at its highest reach, joined to the neighbours' marks of the same wave.
	struct SwashTransect {
		float peak = -1e9f; // run-up this wave, m landward of the transect's middle
		bool rising = false;
		Vector2 peak_pos;
		double mark_time = -1e9;
		Vector2 mark_pos;
		float mark_reach = 0.0f; // the smoothed reach the mark was laid at
		uint32_t mark_wave = 0; // the wave it was laid by
		// The marks still on the sand along this transect (its stretch of each line: from, mark, to), newest last.
		struct Laid {
			Vector2 from, at, to;
		};
		LocalVector<Laid> laid;
	};
	bool swash_marks = true;
	LocalVector<SwashTransect> swash_tr;
	struct SwashLine {
		LocalVector<Vector2> pts; // one per transect
		LocalVector<uint8_t> valid; // per stretch from point t to t + 1: 0 where not laid or flattened
	};
	LocalVector<SwashLine> swash_lines; // the lines still on the sand, oldest first
	LocalVector<float> swash_max; // each transect's highest reach this wave
	bool swash_wave_on = false;
	float swash_r_start = 0.0f; // the mean reach when the wave began, and its highest since
	float swash_r_peak = 0.0f;
	Vector2 swash_dir; // down the beach
	Vector2 swash_mid;
	double swash_accum = 0.0;
	double swash_time = 0.0;
	double swash_frame_age = 1e9;
	int swash_mark_count = 0;
	uint32_t swash_wave = 0; // counts the waves that left a mark: each draws its own lobes
	double swash_last_mark = -1e9;
	float swash_usec = 0.0f;
	void _swash_update(double p_delta);
	void _swash_lay(const Vector2 &p_from, const Vector2 &p_to, bool p_erase = false);
	float _shelter_flux_at(float p_x, float p_z) const;
	float _lattice_value(const LocalVector<float> &p_values, const Vector3 &p_position) const;
	void _shift_lattice(LocalVector<float> &r_values, Vector2i p_old_origin, Vector2i p_new_origin, int p_n, int p_stride);
	float ground_min = 0.0f;
	float ground_max = 0.0f;
	Ref<Image> height_image;
	Ref<ImageTexture> height_texture;
	Ref<Image> deform_image;
	Ref<ImageTexture> deform_texture;
	LocalVector<float> tint_albedo;
	LocalVector<float> tint_roughness;

	MeshInstance3D *mesh_instance = nullptr;
	Ref<ArrayMesh> mesh;
	Ref<Material> patch_material;
	Ref<Shader> patch_source_shader; // the source material's shader when the patch copy was made
	bool patch_params_stale = false;
	Vector3 patch_uv_offset;
	Vector2 center;
	bool mesh_hole_cut = false; // the mesh was built with triangles left out over holes
	int mesh_hole_count = 0;
	const TerrainGenerator3D *_hole_generator() const;
	bool mesh_dirty = true;
	bool globals_published = false;
	// The patch's deformation was rewritten (a GPU blit or a CPU upload) since the patch material's
	// ground_deform_serial was last bumped: the path tracer recaptures vertex() output when a material parameter
	// changes, not when a texture it samples is rewritten in place (as SnowSurface3D's snow_serial).
	bool deform_serial_dirty = false;
	uint32_t deform_serial = 0;
	// The source material's serial (the terrain lowered under the patch) waits for the next second.
	bool source_serial_dirty = false;
	uint64_t source_serial_msec = 0;
	void _bump_deform_serial();
	double cells_accum = 0.0;
	double age_accum = 0.0;
	double step_msec = 0.0;

	static GroundSurface3D *active;

	Vector3 _follow_position() const;
	void _ensure_children();
	void _build_mesh();
	void _update_patch_material();
	void _on_material_changed() { patch_params_stale = true; }
	void _sync_patch_params();
	void _follow(Vector3 p_center);
	void _sample_ground(Vector2i p_origin, int p_n);
	void _update_cells();
	void _age(double p_delta);
	void _drift_prints(const Vector2 &p_wind, float p_drift, float p_dt);
	float _ripple_calm_at(float p_x, float p_z) const;
	void _upload();
	void _publish();
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	float _ground_at(float p_x, float p_z) const;
	bool _on_lattice(float p_x, float p_z) const;
	float _off_lattice_height(const Vector3 &p_position) const;
	// Where the field reads the air (its centre on the ground): the weather contract's air is per point.
	Vector3 _weather_point() const { return Vector3(center.x, _ground_at(center.x, center.y), center.y); }
	const Cell &_cell_at(float p_x, float p_z) const;
	float _edge_distance(float p_x, float p_z) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_height_source(const NodePath &p_path);
	NodePath get_height_source() const { return height_source; }
	void set_snow_cover(const NodePath &p_path) { snow_cover = p_path; }
	NodePath get_snow_cover() const { return snow_cover; }
	void set_ground_collision_mask(uint32_t p_mask) { ground_collision_mask = p_mask; }
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_ground_type(GroundType p_type);
	GroundType get_ground_type() const { return ground_type; }
	void set_moisture(float p_moisture);
	float get_moisture() const { return moisture; }
	void set_litter_depth(float p_m);
	float get_litter_depth() const { return litter_depth; }
	void set_litter_cover(float p_cover);
	float get_litter_cover() const { return litter_cover; }
	void set_rock_from_geology(bool p_enable);
	bool is_rock_from_geology() const { return rock_from_geology; }
	void set_type_map(const Ref<Image> &p_map);
	Ref<Image> get_type_map() const { return type_map; }
	void set_type_map_rect(const Rect2 &p_rect);
	Rect2 get_type_map_rect() const { return type_map_rect; }

	// The surface the materials and the vehicles use (the first in the tree).
	static GroundSurface3D *get_active() { return active; }

	// Soft soil (docs/ideas/sand.md, stage 1).
	GroundSoil get_soil(Vector3 p_position) const;
	// The soil under a wheel at p_position (its lowest point): load N, radius and width m, tyre inflation kPa (0:
	// rigid), longitudinal slip ratio, slip angle (radians). The rut already pressed there counts as done work.
	GroundWheelSoil wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle) const;
	Dictionary get_wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation = 200.0f, float p_slip = 0.0f, float p_slip_angle = 0.0f) const;
	// Presses a wheel's contact: p_size (width, length along p_yaw) down to p_sinkage below the undisturbed
	// ground. p_velocity is where the loose sand goes (backwards when the wheel spins). p_tread: the tyre's
	// PhysXVehicleWheel3D::Tread, its lugs or chains printed in the rut floor (-1: a plain floor).
	float press_wheel(Vector3 p_position, Vector2 p_size, float p_yaw, float p_sinkage, Vector2 p_velocity = Vector2(), int p_tread = -1);
	// A walker's footing: sinkage, the speed it keeps (Lejeune et al. 1998: walking in sand costs 1.6 to 2.5
	// times the work), and the downhill acceleration where the slope is steeper than the material holds.
	Dictionary get_footing_at(Vector3 p_position, float p_mass = 75.0f, float p_area = 0.035f) const;
	// Grains leaving the ground (dig) or landing on it (deposit): a set depth over a disc; the volume, m3.
	float dig(Vector3 p_position, float p_radius, float p_depth);
	float _amount(Vector3 p_position, float p_radius, float p_depth, float p_cell);
	float deposit(Vector3 p_position, float p_radius, float p_depth);
	// Sand thrown by spinning wheels since the last call (press_wheel with slip): Array of Dictionaries
	// (position, velocity, width, slip_speed, reach_center, reach_radius). For a grains node to dig and spawn (sand stage 2).
	// Dust from a tyre: p_strength is tyre width times (ground speed + 2 slip speed), m2/s; scaled by how much
	// of the ground there the wind could move (dry, bare sand; none from damp sand). Taken by GroundDust3D.
	void add_dust(Vector3 p_position, Vector3 p_velocity, float p_strength);
	void take_dust(LocalVector<Dust> &r_dust);
	// A near-field dust node (PhysXGroundDust3D) claims the dust lifted within p_radius of p_center, renewed each
	// physics frame; that dust goes to take_near_dust() instead of take_dust(). A claim not renewed for two
	// frames lapses.
	void claim_dust(Vector3 p_center, float p_radius);
	void take_near_dust(LocalVector<Dust> &r_dust);
	void set_dust_emission(float p_v) { dust_emission = MAX(p_v, 0.0f); }
	float get_dust_emission() const { return dust_emission; }
	Array take_sprays();
	// The clay landed since the last call (GroundClods3D); kept for a couple of physics frames when nobody takes it.
	void take_clod_drops(LocalVector<ClodDrop> &r_drops);
	void set_clods(bool p_enabled);
	bool is_clods() const { return clods; }
	GroundClods3D *get_clods_node() const { return clods_node; }
	void add_spray(const Spray &p_spray);
	// A tyre moving through standing water this physics frame (mud stage 4c): the GPU water passes push the water
	// ahead of it (a bow wave) and draw it in behind. p_id: the tyre's (a later call with the same replaces it);
	// p_axle: the wheel's centre; p_velocity: the wheel's.
	void add_wader(int64_t p_id, const Vector3 &p_axle, float p_radius, float p_half_width, const Vector3 &p_velocity);
	// Adds p_volume m3 of standing water spread over a disc (taken where negative, never below dry): spray landing
	// and water thrown up (mud stage 4c). Applied by the next water step on the GPU.
	void add_water(const Vector3 &p_position, float p_radius, float p_volume, float p_mud = 0.0f);
	void add_puddle(const Vector3 &p_position, float p_radius, float p_depth, float p_mud = 0.0f);
	void set_world_puddles(bool p_enabled) { world_puddles = p_enabled; }
	bool is_world_puddles() const { return world_puddles; }
	void set_world_puddle_density(float p_density) { world_puddle_density = MAX(p_density, 0.0f); }
	float get_world_puddle_density() const { return world_puddle_density; }
	// Stirs p_mass kg of the bed's clay into the standing water over a disc (feet, clods landing), where the bed is
	// cohesive; muddy water (round 3). Applied by the next water step on the GPU.
	void stir_water(const Vector3 &p_position, float p_radius, float p_mass);
	// The clay the standing water carries at a place, kg/m3, as last read back.
	float get_water_mud_at(Vector3 p_position) const;
	float get_regrown_at(Vector3 p_position) const;
	// Lays a film of clay p_depth m thick over a disc (a tyre or a boot leaving mud on firm ground; mud round 3):
	// dark and glossy as wet as p_wet, a pale film once it dries, and it stays until a wash takes it.
	// p_print: the film's print (ground_deform.glsl mud_print_mask()): (0) a plain disc, (1, yaw) a tyre's lugs on its
	// heading (cos yaw, -sin yaw), (2, yaw, half length, half width) a boot's sole, toe along the heading.
	float track_mud(Vector3 p_position, float p_radius, float p_depth, float p_wet = 1.0f, Vector4 p_print = Vector4());
	// The tracked mud film at a place (the texel under it): (its share 0..1, its wetness 0..1), (0, 0) for none.
	Vector2 get_tracked_mud_at(Vector3 p_position) const;
	void set_tracked_mud(bool p_enabled) { tracked_mud = p_enabled; }
	bool is_tracked_mud() const { return tracked_mud; }
	void set_keep_ruts(bool p_enabled);
	bool is_keeping_ruts() const { return keep_ruts; }
	void set_ruts_path(const String &p_path);
	String get_ruts_path() const { return ruts_path; }
	void set_ruts_time_scale(float p_scale) { ruts_time_scale = MAX(p_scale, 0.0f); }
	float get_ruts_time_scale() const { return ruts_time_scale; }
	void set_ruts_away_rain(float p_rate) { ruts_away_rain = MAX(p_rate, 0.0f); }
	float get_ruts_away_rain() const { return ruts_away_rain; }
	void set_ruts_regrowth_time(float p_seconds) { ruts_regrowth_time = MAX(p_seconds, 1.0f); }
	float get_ruts_regrowth_time() const { return ruts_regrowth_time; }
	void save_ruts();
	void clear_saved_ruts();
	int get_saved_rut_tile_count();
	void set_muddy_water(bool p_enabled) { muddy_water = p_enabled; }
	bool is_muddy_water() const { return muddy_water; }
	// The standing water over a disc, as last read back (m3): what add_water can take there.
	float get_standing_water_volume(const Vector3 &p_position, float p_radius) const;
	float get_water_added() const { return (float)water_added; }
	// Water takes asked for but did not find, not yet paid back by water added (m3).
	// The water add_water's discs put into the main field and took out of it, as the GPU last read back (m3).
	Vector2 get_water_moved() const { return gpu_host.gpu.is_valid() ? Vector2(gpu_host.gpu->get_water_moved_mm3(0) * 1e-9, gpu_host.gpu->get_water_moved_mm3(1) * 1e-9) : Vector2(); }
	float get_water_owed() {
		_water_owed_update();
		return (float)water_owed;
	}
	// Rigid bodies in deform_groups moving through soft ground feel its resistance (on).
	void set_ploughing(bool p_enabled) { ploughing = p_enabled; }
	bool is_ploughing() const { return ploughing; }

	void set_gpu_simulation(bool p_enabled);
	bool is_gpu_simulation() const { return gpu_simulation; }
	bool is_gpu_active() const { return _gpu_on(); }
	// Waits for the GPU field and reads it back now (tests and tools; it stalls the GPU).
	void sync_deformation();

	void set_deform_extent(float p_extent);
	float get_deform_extent() const { return deform_extent; }
	void set_deform_resolution(int p_resolution);
	int get_deform_resolution() const { return deform_resolution; }
	void set_ground_spacing(float p_spacing);
	float get_ground_spacing() const { return ground_spacing; }
	void set_detail_spacing(float p_spacing);
	float get_detail_spacing() const { return detail_spacing; }
	void set_outer_spacing(float p_spacing);
	float get_outer_spacing() const { return outer_spacing; }
	void set_deform_groups(const PackedStringArray &p_groups) { deform_groups = p_groups; }
	PackedStringArray get_deform_groups() const { return deform_groups; }
	void set_contact_tolerance(float p_m) { contact_tolerance = MAX(p_m, 0.0f); }
	float get_contact_tolerance() const { return contact_tolerance; }
	void set_character_mass(float p_kg) { character_mass = MAX(p_kg, 0.0f); }
	float get_character_mass() const { return character_mass; }
	void set_character_contact_area(float p_m2) { character_contact_area = MAX(p_m2, 0.001f); }
	float get_character_contact_area() const { return character_contact_area; }
	void set_max_cached_tiles(int p_tiles) { max_cached_tiles = MAX(p_tiles, 0); }
	int get_max_cached_tiles() const { return max_cached_tiles; }

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_skirt(float p_m);
	float get_skirt() const { return skirt; }
	void set_terrain_margin(float p_m);
	float get_terrain_margin() const { return terrain_margin; }
	void set_skirt_fade(float p_m);
	float get_skirt_fade() const { return skirt_fade; }
	void set_wind_override(const Vector2 &p_wind) { wind_override = p_wind; }
	Vector2 get_wind_override() const { return wind_override; }
	void set_slip_avalanches(bool p_enabled) { slip_avalanches = p_enabled; }
	bool is_slip_avalanches() const { return slip_avalanches; }
	int get_avalanche_count() const { return (int)avalanches.size(); }
	void set_wind_ripples(bool p_enabled);
	void set_wind_fills_prints(bool p_enabled) { wind_fills_prints = p_enabled; }
	bool is_wind_fills_prints() const { return wind_fills_prints; }
	void set_water_fringe(bool p_enabled) { water_fringe = p_enabled; }
	bool is_water_fringe() const { return water_fringe; }
	void set_salt_crusts(bool p_enabled) { salt_crusts = p_enabled; }
	bool is_salt_crusts() const { return salt_crusts; }
	float get_salt_crust_at(const Vector3 &p_position) const;
	void set_coarse_armour(bool p_enabled) {
		coarse_armour = p_enabled;
		ripple_mask_accum = 1e9;
	}
	bool is_coarse_armour() const { return coarse_armour; }
	void set_lee_calm_ripples(bool p_enabled) {
		lee_calm_ripples = p_enabled;
		ripple_mask_accum = 1e9;
	}
	bool is_lee_calm_ripples() const { return lee_calm_ripples; }
	float get_ripple_show_at(const Vector3 &p_position) const;
	bool is_wind_ripples() const { return wind_ripples; }
	void set_mud_layer(bool p_enabled);
	bool is_mud_layer() const { return mud_layer; }
	void set_mud_slides(bool p_enabled);
	bool is_mud_slides() const { return mud_slides; }
	void set_buried_obstacles(bool p_enabled);
	bool is_buried_obstacles() const { return buried_obstacles; }
	void set_buried_density(float p_density);
	float get_buried_density() const { return buried_density; }
	float get_buried_depth_at(Vector3 p_position, float p_radius = 0.1f) const;
	Array get_buried_near(Vector3 p_position, float p_radius) const;
	void set_worn_tracks(bool p_enabled);
	bool is_worn_tracks() const { return worn_tracks; }
	void set_wear_regrowth_days(float p_days);
	float get_wear_regrowth_days() const { return wear_regrowth_days; }
	float get_wear_at(Vector3 p_position) const;
	void add_wear(Vector3 p_position, Vector2 p_size, float p_yaw, float p_passes);
	void clear_wear();
	static float wear_compaction(float p_passes);
	void set_mud_waves(bool p_enabled);
	bool is_mud_waves() const { return mud_waves; }
	void set_simulation_paused(bool p_paused) { simulation_paused = p_paused; }
	bool is_simulation_paused() const { return simulation_paused; }
	void set_light_budget(bool p_light);
	bool is_light_budget() const { return light_budget; }
	void set_subsoil(bool p_enabled);
	bool is_subsoil() const { return subsoil; }
	// Fresh slurry laid over a disc (a debris flow's deposit, GroundDebrisFlow3D): wet mud whatever the weather.
	void add_mud_deposit(Vector3 p_position, float p_radius, float p_depth, bool p_update = true, Color p_color = Color(0, 0, 0, 0));
	void refresh_cells() { _update_cells(); }
	// The ground lattice takes the mud deposits laid since (a fan's surface becomes the ground, smooth across its
	// 1 m cells, that wheels and feet then print), then the cells refresh.
	void refresh_deposits();
	float get_mud_deposit_at(Vector3 p_position) const;
	void set_flood_silt(bool p_enabled) { flood_silt = p_enabled; }
	void set_stream_fords(bool p_enabled) { stream_fords = p_enabled; }
	bool is_stream_fords() const { return stream_fords; }
	Dictionary get_ford_stats() const;
	bool is_flood_silt() const { return flood_silt; }
	float get_flood_silt_total() const { return silt_total; }
	void set_mats(bool p_enabled) { mats = p_enabled; }
	bool is_mats() const { return mats; }
	// The mat a wheel's contact p_contact lies on (within p_tolerance of the wood), or -1.
	int get_mat_at(Vector3 p_contact, float p_tolerance = 0.06f) const;
	// A wheel's load on mat p_mat this step, N.
	void add_mat_load(int p_mat, float p_load);
	bool is_mat_buried(int p_mat) const { return p_mat >= 0 && p_mat < (int)mat_list.size() && mat_list[p_mat].buried; }
	int get_mat_count() const { return (int)mat_list.size(); }
	// How far mat p_mat has sunk below the undisturbed ground, m.
	float get_mat_sinkage(int p_mat) const;
	// The length of mat p_mat that carries a load into the soil (beam on an elastic foundation), m.
	float get_mat_spread(int p_mat) const;
	bool is_mat_broken(int p_mat) const { return p_mat >= 0 && p_mat < (int)mat_list.size() && mat_list[p_mat].broken; }
	float get_mat_friction(int p_mat) const { return p_mat >= 0 && p_mat < (int)mat_list.size() ? mat_list[p_mat].friction : 0.5f; }
	// For a wheel of radius p_radius centred at p_center: the mat it rests on (-1 for none) and the height of the road
	// plane under it there (the wheel touching the log's round top), r_support.
	int get_mat_under_wheel(Vector3 p_center, float p_radius, float &r_support) const;
	// The loose layer's depth over the firm base at p_position, m.
	float get_subsoil_depth_at(Vector3 p_position) const;
	// The steepest ground the mud at p_position stays on now, degrees (90 where there is none or it holds anywhere).
	float get_mud_slide_angle_at(Vector3 p_position) const;
	void set_standing_water(bool p_enabled);
	bool is_standing_water() const { return standing_water; }
	float get_standing_water_at(Vector3 p_position) const;
	// The water contract (WorldWater): standing water on the ground, -INF where there is none.
	float contract_water_height_at(const Vector3 &p_position) const override;
	// The ground contract (WorldGround, docs/ideas/world-contracts.md).
	float contract_ground_height_at(const Vector3 &p_position) const override { return get_surface_height_at(p_position); }
	WorldGroundSoil contract_ground_soil_at(const Vector3 &p_position) const override;
	float contract_ground_standing_water_at(const Vector3 &p_position) const override { return get_standing_water_at(p_position); }
	Vector2 contract_ground_ice_at(const Vector3 &p_position) const override { return get_ice_at(p_position); }
	Vector3 contract_ground_frost_at(const Vector3 &p_position) const override { return get_frost_at(p_position); }
	float contract_ground_dent_at(const Vector3 &p_position) const override { return get_dent_at(p_position); }
	float contract_ground_rim_at(const Vector3 &p_position) const override { return get_rim_at(p_position); }
	Vector2 contract_ground_runoff_at(const Vector3 &p_position) const override { return get_runoff_at(p_position); }
	float contract_ground_mud_depth_at(const Vector3 &p_position) const override { return get_mud_depth_at(p_position); }
	float contract_ground_track_mud(const Vector3 &p_position, float p_radius, float p_depth, float p_wet, const Vector4 &p_print) override { return track_mud(p_position, p_radius, p_depth, p_wet, p_print); }
	Vector2 contract_ground_needle_ice_at(const Vector3 &p_position) const override { return get_needle_ice_at(p_position); }
	Vector3 contract_ground_rut_guidance_at(const Vector3 &p_contact, const Vector3 &p_lateral, float p_half_width) const override { return get_rut_guidance(p_contact, p_lateral, p_half_width); }
	float contract_ground_sand_at(const Vector3 &p_position) const override;
	float contract_ground_ripple_at(const Vector3 &p_position) const override { return get_ripple_at(p_position); }
	float contract_ground_blown_sand_at(const Vector3 &p_position) const override;
	void set_desiccation_cracks(bool p_enabled);
	bool is_desiccation_cracks() const { return desiccation_cracks; }
	void set_puddle_ice(bool p_enabled);
	bool is_puddle_ice() const { return puddle_ice; }
	Vector2 get_ice_at(Vector3 p_position) const;
	void set_frozen_ground(bool p_enabled);
	bool is_frozen_ground() const { return frozen_ground; }
	Vector3 get_frost_at(Vector3 p_position) const;
	void set_needle_ice(bool p_enabled);
	bool is_needle_ice() const { return needle_ice; }
	Vector2 get_needle_ice_at(Vector3 p_position) const;
	void set_plant_mud(bool p_enabled);
	bool is_plant_mud() const { return plant_mud; }
	float get_plant_mud_at(Vector3 p_position) const;
	// The crack state at a point: (opening 0..1, plate size m, regularity 0..1, crust 0..1 or, negative, how damp
	// the drying layer still is).
	Vector4 get_crack_at(Vector3 p_position) const;
	void set_mud_map(const Ref<Image> &p_map);
	Ref<Image> get_mud_map() const { return mud_map; }
	void set_mud_map_rect(const Rect2 &p_rect);
	Rect2 get_mud_map_rect() const { return mud_map_rect; }
	void set_mud_max_depth(float p_m);
	float get_mud_max_depth() const { return mud_max_depth; }
	void set_mud_auto_depth(float p_m);
	float get_mud_auto_depth() const { return mud_auto_depth; }
	void set_mud_wet_start(float p_m);
	float get_mud_wet_start() const { return mud_wet_start; }
	void set_mud_wet_full(float p_m);
	float get_mud_wet_full() const { return mud_wet_full; }
	void set_data_map(const NodePath &p_path);
	NodePath get_data_map() const { return data_map; }
	void set_mud_color(const Color &p_color);
	Color get_mud_color() const { return mud_color; }
	void set_mud_color_map(const Ref<Image> &p_map);
	Ref<Image> get_mud_color_map() const { return mud_color_map; }
	Color get_mud_color_at(Vector3 p_position) const;
	float get_mud_depth_at(Vector3 p_position) const;
	void set_near_field(bool p_enabled);
	bool is_near_field() const { return near_field; }
	void set_near_extent(float p_extent);
	float get_near_extent() const { return near_extent; }
	void set_near_resolution(int p_resolution);
	int get_near_resolution() const { return near_resolution; }
	Rect2 get_near_rect() const;
	Vector2 get_near_moved_volume() const { return is_near_active() ? near_fld.moved_volume() : Vector2(); }
	bool is_near_active() const { return _near_on() && near_fld.valid; }
	// The print fields' CPU copies (read back from the GPU), for GroundLumpScan.
	const GroundDeformField &get_deform_field() const { return field; }
	const GroundDeformField *get_near_deform_field() const { return is_near_active() ? &near_fld : nullptr; }
	void set_water_sources(const TypedArray<NodePath> &p_sources) { water_sources = p_sources; }
	TypedArray<NodePath> get_water_sources() const { return water_sources; }
	void set_water_coupling(bool p_enabled);
	bool is_water_coupling() const { return water_coupling; }
	float get_saturation_at(const Vector3 &p_position) const;
	float get_water_speed_at(const Vector3 &p_position) const;
	float get_water_step_usec() const { return water_usec; }
	void set_sand_glints(bool p_enabled) { sand_glints = p_enabled; }
	bool is_sand_glints() const { return sand_glints; }
	void set_runoff_channels(bool p_enabled);
	bool is_runoff_channels() const { return runoff_channels; }
	void set_runoff_time_scale(float p_scale) { runoff_time_scale = MAX(p_scale, 0.0f); }
	float get_runoff_time_scale() const { return runoff_time_scale; }
	Vector2 get_runoff_at(const Vector3 &p_position) const; // (depth m, speed m/s)
	float get_runoff_cut_at(const Vector3 &p_position) const;
	float get_runoff_step_usec() const { return runoff_usec; }
	void set_swash_marks(bool p_enabled);
	bool is_swash_marks() const { return swash_marks; }
	int get_swash_mark_count() const { return swash_mark_count; }
	float get_swash_step_usec() const { return swash_usec; }
	// Bed load of fine sand under a bed shear stress (Pa): Meyer-Peter and Mueller, m2/s.
	static float bed_load_shear(float p_tau);
	void set_wind_shelters(const TypedArray<NodePath> &p_shelters) { wind_shelters = p_shelters; }
	TypedArray<NodePath> get_wind_shelters() const { return wind_shelters; }
	void set_obstacle_lee(bool p_enabled) { obstacle_lee = p_enabled; }
	bool is_obstacle_lee_enabled() const { return obstacle_lee; }
	// Raupach (1993): the surface's share of the friction speed among roughness elements of frontal area index
	// p_lambda (clamped to 0.5).
	static float shelter_ratio_for(float p_lambda);
	Vector2 get_sand_flux_at(const Vector3 &p_position) const;
	float get_deposition_rate_at(const Vector3 &p_position) const;
	float get_sand_deposit_at(const Vector3 &p_position) const;
	float get_wind_step_usec() const { return wind_usec; }
	void set_ripple_extent(float p_extent) { ripple_extent = CLAMP(p_extent, 1.0f, 16.0f); }
	float get_ripple_extent() const { return ripple_extent; }
	void set_ripple_time_scale(float p_scale) { ripple_time_scale = MAX(p_scale, 0.0f); }
	float get_ripple_time_scale() const { return ripple_time_scale; }
	// The ripple layer's height at a world position, m about its mean (0 off the layer); tests and tools.
	float get_ripple_at(const Vector3 &p_position) const { return ripples.sample(p_position.x, p_position.z); }
	bool is_ripples_ready() const { return wind_ripples && ripples.tmpl_ready; }
	int get_ripple_steps() const { return ripple_steps; }
	PackedFloat32Array get_ripple_heights() const;
	Vector2 get_ripple_origin() const { return Vector2(ripples.origin) * ripples.cell; }
	float get_ripple_cell() const { return ripples.cell; }
	int get_ripple_size() const { return ripples.n; }
	void set_drying_time(float p_s) { drying_time = MAX(p_s, 1.0f); }
	float get_drying_time() const { return drying_time; }
	void set_sand_drift_rate(float p_rate) { sand_drift_rate = MAX(p_rate, 0.0f); }
	float get_sand_drift_rate() const { return sand_drift_rate; }
	void set_rain_smoothing(float p_rate) { rain_smoothing = MAX(p_rate, 0.0f); }
	float get_rain_smoothing() const { return rain_smoothing; }

	// Presses the ground like SnowSurface3D.press(): down to p_bottom under a sphere (p_round) or a flat
	// disc of p_radius, plus p_sink below the undisturbed surface. Returns the volume moved, m3.
	float press(Vector3 p_position, float p_radius, float p_bottom, float p_sink = 0.0f, bool p_round = true);
	float press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink = 0.0f, bool p_sole = false);
	// Bekker sinkage of a body of p_mass kg on p_area m2 (width p_width) at a place, m.
	float get_sinkage_at(Vector3 p_position, float p_mass, float p_area, float p_width = 0.0f) const;
	// Runs slumping and ageing for p_seconds at once (tests, and settling a scene after loading).
	void settle(float p_seconds, float p_step = 1.0f / 60.0f);
	void clear_deformation();
	void refresh_ground();

	float get_surface_height_at(Vector3 p_position) const;
	float get_ground_height_at(Vector3 p_position) const;
	// The deformed surface's height, under the name height sources share (GroundHeightQuery: a PhysXFlow3D's
	// terrain floor), so gas rests on the ruts and berms.
	float get_height(Vector3 p_position) const { return get_surface_height_at(p_position); }
	float get_dent_at(Vector3 p_position) const;
	Vector3 get_rut_guidance(Vector3 p_contact, Vector3 p_lateral, float p_half_width) const;
	float get_rim_at(Vector3 p_position) const;
	float get_fresh_at(Vector3 p_position) const;
	// The rules at a place: n, kc, kphi, rim share, repose angle (degrees), slump rate, max dent,
	// litter depth, sand share, deformable (1 or 0).
	Dictionary get_material_at(Vector3 p_position) const;
	Vector2 get_moved_volume() const;
	Rect2 get_deform_rect() const;
	int get_cached_tile_count() const { return _gpu_on() ? gpu_host.cache.size() : field.cached_tile_count(); }
	double get_step_time_msec() const { return step_msec; }
	Ref<Material> get_patch_material() const { return patch_material; }
	PackedStringArray get_configuration_warnings() const override;

	GroundSurface3D();
	~GroundSurface3D();
};

VARIANT_ENUM_CAST(GroundSurface3D::GroundType);

/**************************************************************************/
/*  terrain_generator_3d.h                                                */
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

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"

#include "climate_recipe.h"
#include "modules/noise/noise.h"

// Generates mountain ranges for a whole world: uplift (where the rock rises: a painted map, Path3D ranges, noise)
// against fluvial erosion by the stream power law, run to equilibrium on a coarse grid (Cordonnier et al. 2016,
// Braun and Willett 2013 solver; see terrain_stream_power.h and docs/ideas/terrain-generation.md). The result is
// sampled bicubically onto Terrain3D (by name) or a HeightMapShape3D; a TerrainDeformer3D's brushes (rivers,
// roads, plots) are lifted before the write and re-applied on the new ground.
class TerrainGenerator3D : public Node3D {
	GDCLASS(TerrainGenerator3D, Node3D);

public:
	enum OutletEdge {
		OUTLET_NEGATIVE_X = 1,
		OUTLET_POSITIVE_X = 2,
		OUTLET_NEGATIVE_Z = 4,
		OUTLET_POSITIVE_Z = 8,
	};

private:
	// Where.
	Vector2 size = Vector2(4096, 4096);
	real_t cell_size = 16.0;
	int seed = 0;

	// Uplift.
	real_t uplift = 2.0; // mm/yr at full strength
	real_t base_uplift = 0.1; // strength everywhere, 0..1, under the ranges and the map
	Ref<Texture2D> uplift_map;
	TypedArray<NodePath> ranges;
	real_t range_width = 800.0;
	Ref<Noise> uplift_noise;
	real_t uplift_noise_strength = 0.5;

	// Erosion.
	real_t erodibility = 2.0e-5;
	real_t area_exponent = 0.5;
	real_t hillslope_diffusion = 0.01; // m^2/yr
	real_t deposition = 0.0; // G of Yuan et al. 2019; 0 = incision only
	real_t critical_slope = 40.0; // degrees
	real_t flow_exponent = 1.0;
	real_t time_step = 20000.0; // years
	int steps = 300;
	int coarse_levels = -1; // -1: automatic
	int refine_steps = 60;
	real_t initial_roughness = 1.0; // metres
	int outlet_edges = 15;
	real_t sea_level = 0.0;
	real_t height_offset = 0.0;
	real_t max_height = 0.0; // 0 = as eroded; otherwise relief scaled so the highest point is this

	// Detail: erosion filter over the simulated ground (stage 3).
	bool detail_enabled = true;
	real_t detail_wavelength = 128.0;
	int detail_octaves = 4;
	real_t detail_depth = 0.08;
	real_t detail_reach = 1.0;
	real_t detail_gully_weight = 0.5;
	real_t detail_assumed_slope = 0.6;
	real_t detail_crest = 1.0; // ridged noise on crests, times the gully amplitude
	real_t cliff_ledge_spacing = 0.0; // m between rock beds stepped into faces steeper than 40 degrees; 0 = off
	real_t cliff_ledge_strength = 1.0;

	// Tile erosion (stage 4): droplet hydraulic plus thermal erosion on a fine grid, in overlapping tiles.
	bool tile_erosion_enabled = false;
	real_t tile_erosion_spacing = 2.0; // m
	real_t tile_erosion_tile_size = 512.0; // m
	real_t tile_erosion_apron = 128.0; // m
	real_t tile_erosion_droplets = 1.0; // per cell
	real_t tile_erosion_max_depth = 15.0; // m
	real_t tile_erosion_rock_hardness = 0.25;

	// Geology and rivers (stage 5).
	real_t soil_production = 0.5; // mm/yr on bare rock
	real_t soil_depth_scale = 0.5; // m
	real_t max_soil_depth = 2.0; // m
	real_t talus_angle = 35.0; // degrees
	real_t talus_reach = 120.0; // m

	// Layered rock (strata): hard bands erode slower and stand in cliffs, so rivers crossing them step down in
	// knickpoints and waterfalls (docs/ideas/terrain-generation.md, "Waterfalls from rock layers").
	bool strata_enabled = false;
	real_t strata_spacing = 90.0; // m, one hard and one soft layer
	real_t strata_hard_fraction = 0.35; // share of the spacing that is hard rock
	real_t strata_hardness = 100.0; // erodibility of soft rock over hard (rock strength squared, Sklar and Dietrich 2001)
	real_t strata_cliff_angle = 75.0; // degrees, critical slope in hard rock
	real_t strata_dip = 1.0; // degrees
	real_t strata_dip_direction = 0.0; // degrees clockwise from north, the way the layers go down
	real_t strata_wobble = 12.0; // m, layer heights wander by this much
	real_t strata_channel_area = 20000.0; // m^2, drainage from which the bed's rock counts (channel heads)

	// Glaciers (docs/ideas/terrain-generation.md, "Glacial erosion"): ice from the shallow ice approximation erodes
	// troughs, cirques and overdeepened basins; ice ages alternate with the rivers, and a last glaciation after them
	// leaves its basins as lakes.
	bool glacial_enabled = false;
	real_t glacial_ela = 500.0; // m, equilibrium line altitude
	real_t glacial_share = 0.5; // share of the run's time under ice
	real_t glacial_final = 100000.0; // years of the last glaciation, after the rivers
	real_t glacial_erosion = 1.0; // multiplier on K_g of Herman et al. 2015
	real_t glacial_sliding = 1.0; // sliding speed over depth-averaged ice speed
	real_t glacial_rock_variation = 0.0; // spread of rock quality under the ice (log of the erosion multiplier); 0 even
	real_t glacial_rock_scale = 500.0; // m, wavelength of that variation
	real_t glacial_inflow_balance = 0.0; // m/yr of ice per m2 of each inflow's off-map catchment (0: water only)
	real_t last_glacial_eroded = 0.0;
	real_t last_glacial_ice_area = 0.0;
	real_t last_glacial_msec = 0.0;
	LocalVector<float> glacial_ice; // m, ice of the last glaciation at its end
	real_t river_runoff = 1000.0; // mm/yr, without a climate_recipe
	Ref<ClimateRecipe> climate_recipe; // rivers from rain and snowmelt (terrain_runoff.h)
	int river_month = -1; // the month build_river_paths and get_rivers use, -1 the annual mean
	real_t river_width_coefficient = 7.0; // width = coefficient * discharge^0.5
	bool publish_shader_globals = true;

	// Coast: sea along the chosen edges (or where sea_map is white), a seabed below it and beaches above it.
	int ocean_edges = 0;
	real_t coast_distance = 600.0; // m from the edge to the mean coastline
	real_t coast_roughness = 400.0; // m the coastline wanders either way
	real_t coast_wavelength = 1200.0; // m, of its largest bays and headlands
	Ref<Texture2D> sea_map;
	real_t seabed_profile = 0.1; // A in the Dean profile depth = A x^(2/3) (m^(1/3)); fine sand ~0.1
	real_t shelf_slope = 0.03; // added past the surf zone (300 m)
	real_t max_sea_depth = 60.0; // m
	real_t beach_width = 80.0; // m, mean; it varies along the coast
	real_t beach_slope = 0.03;

	// Lakes: natural dams across valleys, the water behind them.
	int lake_count = 0;
	TypedArray<NodePath> lakes; // markers: a dam where each one stands (metadata "depth" overrides lake_depth; "rock_lip" or "face_slope" for a steep sill)
	// Inflows: land beyond the map that drains in at a marker (metadata "area" in km2, else inflow_area).
	TypedArray<NodePath> inflows;
	real_t inflow_area = 25.0; // km2
	LocalVector<float> inflow_cells; // m2 entering at each node of the result (empty: none)
	LocalVector<float> sill_cells; // resistant sill round an inflow, 0..1 (empty: none)
	real_t lake_depth = 20.0; // m of dam above the valley floor
	real_t lake_catchment = 2.0e5; // m^2, least drainage area at an automatic dam
	real_t lake_min_area = 10000.0; // m^2
	real_t lake_spacing = 1200.0; // m between automatic dams
	real_t lake_evaporation = 1000.0; // mm/yr off the water

	// Output.
	NodePath terrain_path;
	NodePath deformer_path;
	NodePath ground_mesh_path;

	// Result (not saved: the ground keeps it; generate() rebuilds it).
	int nx = 0;
	int ny = 0;
	LocalVector<float> heights;
	LocalVector<float> drainage;
	LocalVector<float> sediment;
	LocalVector<float> alluvium_reach; // alluvium depth (to 0.5 m) carried past the deposit's edge; fades the detail
	LocalVector<float> uplift_field;
	LocalVector<float> soil;
	LocalVector<float> rock;
	LocalVector<float> talus;
	LocalVector<float> beach; // 1 on the beach face, fading over the backshore; 0 inland and where no beach
	LocalVector<float> hardness; // 0 soft .. 1 hard rock at the surface (strata)
	LocalVector<int32_t> receivers;
	LocalVector<float> channel_area; // D8 drainage area along the receivers, for the rivers
	// Water: per cell 0 dry, 1 sea, 2 + k lake k; the depth of water over the ground; the rivers' routing (lakes and
	// the sea end them; a lake's outflow starts at its outlet carrying its whole catchment).
	LocalVector<int32_t> water_body;
	LocalVector<float> water_depth;
	LocalVector<float> lake_rim; // the level of a lake within three cells, NaN elsewhere
	// Falls over hard rock: the face stands vertical (the caprock lip, Gilbert 1907; Haviv et al. 2010), sharper than
	// the grid can hold, so _ground steps the ground there.
	struct FallLip {
		Vector2 origin; // the lip cell the rivers left, world x z (get_waterfalls matches on it)
		Vector2 top; // where the step starts: the lip cell, or the worn-back lip at a gorge's head
		Vector2 dir; // downstream, unit
		real_t run = 0.0; // m from the lip cell to the foot cell
		real_t h_top = 0.0, h_bottom = 0.0;
		real_t half_width = 0.0; // m held vertical either side of the river's line
	};
	// Plunge pools at the foot of falls (Mason and Arumugam 1985 depth, Pagliara et al. 2006 shape).
	struct PlungePool {
		Vector2 top; // the fall's lip cell, to match it in get_waterfalls
		Vector2 center;
		Vector2 dir; // downstream, unit
		real_t radius = 0.0, depth = 0.0, foot = 0.0; // the floor is foot - depth at the centre
	};
	// Gorges below falls whose lip has worn back upstream (knickpoint retreat).
	struct Gorge {
		Vector2 origin; // the lip cell the rivers left
		LocalVector<Vector2> points; // new lip first, down to the old foot
		real_t half_width = 3.0;
		real_t floor_top = 0.0, floor_bottom = 0.0, length = 0.0;
	};
	double gorge_age = 0.0; // years of lip retreat; 0 none
	double moraine_age = 0.0; // years the last glaciation's margins stood still, building moraines; 0 none
	double last_moraine_volume = 0.0; // m3 of debris the last generate() laid in moraines
	double scree_age = 0.0; // years cliffs have shed scree onto cones at their feet; 0 none
	double last_scree_volume = 0.0; // m3 of scree the last generate() laid
	double gorge_rate = 0.013; // m a year per sqrt(m3/s)
	LocalVector<Gorge> gorges;
	bool fall_lips = true;
	bool plunge_pools = true;
	bool fan_backfill = true;
	int last_slots_filled = 0;
	LocalVector<FallLip> lips;
	LocalVector<PlungePool> pools;
	LocalVector<LocalVector<int>> lip_buckets; // per LIP_BUCKET-metre square of the result
	int lip_bx = 0, lip_bz = 0;
	void _bake_lips();
	float _apply_lips(real_t p_x, real_t p_z, float p_g) const;
	LocalVector<float> lake_rim_weight; // how firmly the ground holds that level: 1 within a cell, 0 at three
	LocalVector<int32_t> river_receivers;
	LocalVector<float> river_area;
	// Water balance on the climate grid (side runoff_n, runoff_f fine cells per side): per cell 12 monthly runoff (mm),
	// 12 snow water equivalents (mm), precipitation (mm a year), annual and winter shortwave (W/m2), surface height.
	LocalVector<float> runoff_fields;
	int runoff_n = 0;
	int runoff_f = 1;
	double runoff_mean = 0.0; // mm a year over land
	double runoff_msec = 0.0;
	// Monthly discharge (m^3/s, 12 per cell) of channel cells (river area of RUNOFF_MIN_CELLS cells or more).
	LocalVector<int32_t> flow_index;
	LocalVector<float> flows;
	struct LakeInfo {
		real_t level = 0.0;
		real_t bed = 0.0;
		int32_t outlet = -1;
		int32_t dam = -1;
		real_t catchment = 0.0;
		bool endorheic = false;
		int cells = 0;
		LocalVector<int32_t> outflow; // over a col: outlet down to where it drains away
		Vector2i lo;
		Vector2i hi;
	};
	LocalVector<LakeInfo> lake_info;
	bool has_sea = false;
	float sea_bottom = 0.0f;
	ObjectID sea_node;
	LocalVector<ObjectID> lake_nodes;
	void _build_sea_mask(LocalVector<uint8_t> &r_sea) const;
	void _shape_coast(const LocalVector<uint8_t> &p_sea);
	void _make_lakes(const LocalVector<uint8_t> &p_sea);
	void _basin_lakes(std::vector<uint8_t> &r_taken);
	struct InflowPoint {
		Vector2 position;
		double area = 0.0; // m2
		double sill = 0.0; // m radius of resistant rock round the entry (0: none)
		double level = NAN; // height (eroded metres) the sill is not worn below
		double erosion = 1.0; // share of the area that erodes (1: all; 0: water only)
	};
	LocalVector<InflowPoint> _inflow_points() const;
	void _inflow_grid(const LocalVector<InflowPoint> &p_points, int p_nx, int p_ny, double p_dx, const uint8_t *p_outlet, LocalVector<double> &r_area, LocalVector<double> &r_sill, LocalVector<double> &r_level, LocalVector<double> *r_water) const;
	void _route_rivers();
	float _water_fade(real_t p_x, real_t p_z) const;
	Ref<ArrayMesh> _lake_outline(int p_lake, const Vector3 &p_origin) const;
	static double _marker_face_slope(const Node3D *p_marker);
	Vector3 _lake_center(int p_lake) const;
	Ref<ImageTexture> geology_texture;
	Ref<ImageTexture> detail_keep_texture; // share of the erosion detail kept, per node (_bake_detail_keep)
	// Static spray potential of the falls (0..1) on its own finer grid over the result (_bake_spray).
	real_t spray_cell = 4.0;
	int spray_nx = 0, spray_ny = 0;
	LocalVector<float> spray;
	Ref<ImageTexture> spray_texture;
	void _bake_spray();
	// Tile erosion result on its own grid (tile_nx x tile_nz nodes from result_rect's corner, tile_cell apart).
	int tile_nx = 0;
	int tile_nz = 0;
	real_t tile_cell = 1.0;
	LocalVector<float> tile_delta; // eroded minus input ground
	LocalVector<float> tile_flow;
	LocalVector<float> tile_deposit;
	double last_tile_erosion_msec = 0.0;
	double last_water_msec = 0.0;
	struct TileJob {
		TerrainGenerator3D *gen;
		int tiles_x;
		int core; // cells
		int apron; // cells
		LocalVector<LocalVector<float>> *results; // per tile: delta, flow, deposit, interleaved by 3
	};
	static void _erode_tile(void *p_job, uint32_t p_tile);
	void _erode_tiles();
	float _tile_sample(const LocalVector<float> &p_grid, real_t p_x, real_t p_z) const;
	float _ground_detail(real_t p_x, real_t p_z) const;
	bool result_deposition = false; // the last generate() deposited (alluvium smooths the detail)
	Rect2 result_rect; // where the last generate() ran; the result stays there if the node moves
	real_t result_cell = 1.0;
	float result_min = 0.0f;
	float result_max = 0.0f;
	double last_generate_msec = 0.0;
	double last_write_msec = 0.0;
	int last_steps = 0;
	int64_t last_deposition_passes = 0;
	int last_coarse_levels = 0;
	real_t last_change = 0.0;

	void _build_uplift(LocalVector<double> &r_uplift) const;
	float _sample(const LocalVector<float> &p_grid, real_t p_x, real_t p_z) const;
	Ref<Image> _grid_image(const LocalVector<float> &p_grid) const;
	Node *_get_terrain_node() const;
	// The final ground at a world point: simulated (bicubic) plus the detail filter.
	float _ground(real_t p_x, real_t p_z) const;
	struct FillJob {
		const TerrainGenerator3D *gen;
		real_t x0;
		real_t z0;
		real_t spacing;
		int width;
		float *out; // width * rows, NaN outside
	};
	static void _fill_row(void *p_job, uint32_t p_row);
	void _publish_globals();
	Vector3 _cell_position(int p_cell) const;
	Array _rivers(real_t p_min_area, bool p_cells) const;
	void _bake_runoff(const LocalVector<uint8_t> &p_sea);
	float _runoff_sample(int p_field, real_t p_x, real_t p_z) const;
	float _flow(int p_cell, int p_month) const;
	void _fall_contract(Dictionary &r_fall, const PackedVector3Array &p_pts, int p_k, int p_e, const PackedInt32Array &p_cells, real_t p_width, real_t p_v0) const;
	void _route_runoff(const LocalVector<int32_t> &p_receivers, const LocalVector<int32_t> &p_order);
	void _alluvium_reach();
	void _bake_detail_keep();
	void _fill(real_t p_x0, real_t p_z0, real_t p_spacing, int p_width, int p_rows, float *r_out) const;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_cell_size(real_t p_size);
	real_t get_cell_size() const { return cell_size; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }

	void set_uplift(real_t p_value) { uplift = MAX(p_value, real_t(0.0)); }
	real_t get_uplift() const { return uplift; }
	void set_base_uplift(real_t p_value) { base_uplift = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_base_uplift() const { return base_uplift; }
	void set_uplift_map(const Ref<Texture2D> &p_map) { uplift_map = p_map; }
	Ref<Texture2D> get_uplift_map() const { return uplift_map; }
	void set_ranges(const TypedArray<NodePath> &p_ranges) { ranges = p_ranges; }
	TypedArray<NodePath> get_ranges() const { return ranges; }
	void set_range_width(real_t p_width) { range_width = MAX(p_width, real_t(1.0)); }
	real_t get_range_width() const { return range_width; }
	void set_uplift_noise(const Ref<Noise> &p_noise) { uplift_noise = p_noise; }
	Ref<Noise> get_uplift_noise() const { return uplift_noise; }
	void set_uplift_noise_strength(real_t p_value) { uplift_noise_strength = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_uplift_noise_strength() const { return uplift_noise_strength; }

	void set_erodibility(real_t p_value) { erodibility = MAX(p_value, real_t(0.0)); }
	real_t get_erodibility() const { return erodibility; }
	void set_area_exponent(real_t p_value) { area_exponent = CLAMP(p_value, real_t(0.1), real_t(1.0)); }
	real_t get_area_exponent() const { return area_exponent; }
	void set_hillslope_diffusion(real_t p_value) { hillslope_diffusion = MAX(p_value, real_t(0.0)); }
	real_t get_hillslope_diffusion() const { return hillslope_diffusion; }
	void set_deposition(real_t p_value) { deposition = MAX(p_value, real_t(0.0)); }
	real_t get_deposition() const { return deposition; }
	void set_critical_slope(real_t p_degrees) { critical_slope = CLAMP(p_degrees, real_t(0.0), real_t(80.0)); }
	real_t get_critical_slope() const { return critical_slope; }
	void set_flow_exponent(real_t p_value) { flow_exponent = CLAMP(p_value, real_t(0.0), real_t(10.0)); }
	real_t get_flow_exponent() const { return flow_exponent; }
	void set_time_step(real_t p_years) { time_step = MAX(p_years, real_t(1.0)); }
	real_t get_time_step() const { return time_step; }
	void set_steps(int p_steps) { steps = MAX(p_steps, 0); }
	int get_steps() const { return steps; }
	void set_coarse_levels(int p_levels) { coarse_levels = CLAMP(p_levels, -1, 8); }
	int get_coarse_levels() const { return coarse_levels; }
	void set_refine_steps(int p_steps) { refine_steps = MAX(p_steps, 0); }
	int get_refine_steps() const { return refine_steps; }
	int get_last_coarse_levels() const { return last_coarse_levels; }
	void set_initial_roughness(real_t p_value) { initial_roughness = MAX(p_value, real_t(0.0)); }
	real_t get_initial_roughness() const { return initial_roughness; }
	void set_outlet_edges(int p_edges) { outlet_edges = p_edges & 15; }
	int get_outlet_edges() const { return outlet_edges; }
	void set_sea_level(real_t p_value) { sea_level = p_value; }
	real_t get_sea_level() const { return sea_level; }
	void set_height_offset(real_t p_value) { height_offset = p_value; }
	real_t get_height_offset() const { return height_offset; }
	void set_max_height(real_t p_value) { max_height = MAX(p_value, real_t(0.0)); }
	real_t get_max_height() const { return max_height; }

	void set_detail_enabled(bool p_enable) { detail_enabled = p_enable; }
	bool is_detail_enabled() const { return detail_enabled; }
	void set_detail_wavelength(real_t p_value) { detail_wavelength = MAX(p_value, real_t(1.0)); }
	real_t get_detail_wavelength() const { return detail_wavelength; }
	void set_detail_octaves(int p_value) { detail_octaves = CLAMP(p_value, 0, 12); }
	int get_detail_octaves() const { return detail_octaves; }
	void set_detail_depth(real_t p_value) { detail_depth = MAX(p_value, real_t(0.0)); }
	real_t get_detail_depth() const { return detail_depth; }
	void set_detail_reach(real_t p_value) { detail_reach = MAX(p_value, real_t(0.01)); }
	real_t get_detail_reach() const { return detail_reach; }
	void set_detail_gully_weight(real_t p_value) { detail_gully_weight = CLAMP(p_value, real_t(0.05), real_t(1.0)); }
	real_t get_detail_gully_weight() const { return detail_gully_weight; }
	void set_cliff_ledge_spacing(real_t p_value) { cliff_ledge_spacing = MAX(p_value, real_t(0.0)); }
	real_t get_cliff_ledge_spacing() const { return cliff_ledge_spacing; }
	void set_cliff_ledge_strength(real_t p_value) { cliff_ledge_strength = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_cliff_ledge_strength() const { return cliff_ledge_strength; }
	void set_detail_crest(real_t p_value) { detail_crest = MAX(p_value, real_t(0.0)); }
	real_t get_detail_crest() const { return detail_crest; }
	void set_detail_assumed_slope(real_t p_value) { detail_assumed_slope = MAX(p_value, real_t(0.01)); }
	real_t get_detail_assumed_slope() const { return detail_assumed_slope; }

	// Tile erosion.
	void set_tile_erosion_enabled(bool p_enable) { tile_erosion_enabled = p_enable; }
	bool is_tile_erosion_enabled() const { return tile_erosion_enabled; }
	void set_tile_erosion_spacing(real_t p_value) { tile_erosion_spacing = MAX(p_value, real_t(0.25)); }
	real_t get_tile_erosion_spacing() const { return tile_erosion_spacing; }
	void set_tile_erosion_tile_size(real_t p_value) { tile_erosion_tile_size = MAX(p_value, real_t(16.0)); }
	real_t get_tile_erosion_tile_size() const { return tile_erosion_tile_size; }
	void set_tile_erosion_apron(real_t p_value) { tile_erosion_apron = MAX(p_value, real_t(0.0)); }
	real_t get_tile_erosion_apron() const { return tile_erosion_apron; }
	void set_tile_erosion_droplets(real_t p_value) { tile_erosion_droplets = MAX(p_value, real_t(0.0)); }
	real_t get_tile_erosion_droplets() const { return tile_erosion_droplets; }
	void set_tile_erosion_max_depth(real_t p_value) { tile_erosion_max_depth = MAX(p_value, real_t(0.1)); }
	real_t get_tile_erosion_max_depth() const { return tile_erosion_max_depth; }
	void set_tile_erosion_rock_hardness(real_t p_value) { tile_erosion_rock_hardness = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_tile_erosion_rock_hardness() const { return tile_erosion_rock_hardness; }

	// Geology and rivers.
	void set_soil_production(real_t p_value) { soil_production = MAX(p_value, real_t(1e-6)); }
	real_t get_soil_production() const { return soil_production; }
	void set_soil_depth_scale(real_t p_value) { soil_depth_scale = MAX(p_value, real_t(0.0)); }
	real_t get_soil_depth_scale() const { return soil_depth_scale; }
	void set_max_soil_depth(real_t p_value) { max_soil_depth = MAX(p_value, real_t(0.0)); }
	real_t get_max_soil_depth() const { return max_soil_depth; }
	void set_talus_angle(real_t p_value) { talus_angle = CLAMP(p_value, real_t(5.0), real_t(80.0)); }
	real_t get_talus_angle() const { return talus_angle; }
	void set_talus_reach(real_t p_value) { talus_reach = MAX(p_value, real_t(1.0)); }
	real_t get_talus_reach() const { return talus_reach; }
	void set_glacial_enabled(bool p_enable) { glacial_enabled = p_enable; }
	bool is_glacial_enabled() const { return glacial_enabled; }
	void set_glacial_ela(real_t p_value) { glacial_ela = p_value; }
	real_t get_glacial_ela() const { return glacial_ela; }
	void set_glacial_share(real_t p_value) { glacial_share = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_glacial_share() const { return glacial_share; }
	void set_glacial_final(real_t p_value) { glacial_final = MAX(p_value, real_t(0.0)); }
	real_t get_glacial_final() const { return glacial_final; }
	void set_glacial_erosion(real_t p_value) { glacial_erosion = MAX(p_value, real_t(0.0)); }
	real_t get_glacial_erosion() const { return glacial_erosion; }
	void set_glacial_rock_variation(real_t p_value) { glacial_rock_variation = MAX(p_value, real_t(0.0)); }
	real_t get_glacial_rock_variation() const { return glacial_rock_variation; }
	void set_glacial_rock_scale(real_t p_value) { glacial_rock_scale = MAX(p_value, real_t(16.0)); }
	real_t get_glacial_rock_scale() const { return glacial_rock_scale; }
	void set_glacial_inflow_balance(real_t p_value) { glacial_inflow_balance = MAX(p_value, real_t(0.0)); }
	real_t get_glacial_inflow_balance() const { return glacial_inflow_balance; }
	void set_glacial_sliding(real_t p_value) { glacial_sliding = MAX(p_value, real_t(0.0)); }
	real_t get_glacial_sliding() const { return glacial_sliding; }
	real_t get_last_glacial_eroded() const { return last_glacial_eroded; }
	real_t get_last_glacial_ice_area() const { return last_glacial_ice_area; }
	real_t get_last_glacial_msec() const { return last_glacial_msec; }
	real_t get_glacier_depth(const Vector3 &p_global_position) const;
	void set_strata_enabled(bool p_enable) { strata_enabled = p_enable; }
	bool is_strata_enabled() const { return strata_enabled; }
	void set_strata_channel_area(real_t p_value) { strata_channel_area = MAX(p_value, real_t(0.0)); }
	real_t get_strata_channel_area() const { return strata_channel_area; }
	void set_strata_spacing(real_t p_value) { strata_spacing = MAX(p_value, real_t(1.0)); }
	real_t get_strata_spacing() const { return strata_spacing; }
	void set_strata_hard_fraction(real_t p_value) { strata_hard_fraction = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_strata_hard_fraction() const { return strata_hard_fraction; }
	void set_strata_hardness(real_t p_value) { strata_hardness = MAX(p_value, real_t(1.0)); }
	real_t get_strata_hardness() const { return strata_hardness; }
	void set_strata_cliff_angle(real_t p_value) { strata_cliff_angle = CLAMP(p_value, real_t(10.0), real_t(85.0)); }
	real_t get_strata_cliff_angle() const { return strata_cliff_angle; }
	void set_strata_dip(real_t p_value) { strata_dip = CLAMP(p_value, real_t(0.0), real_t(80.0)); }
	real_t get_strata_dip() const { return strata_dip; }
	void set_strata_dip_direction(real_t p_value) { strata_dip_direction = p_value; }
	real_t get_strata_dip_direction() const { return strata_dip_direction; }
	void set_strata_wobble(real_t p_value) { strata_wobble = MAX(p_value, real_t(0.0)); }
	real_t get_strata_wobble() const { return strata_wobble; }
	void set_river_runoff(real_t p_value) { river_runoff = MAX(p_value, real_t(0.0)); }
	real_t get_river_runoff() const { return river_runoff; }
	void set_climate_recipe(const Ref<ClimateRecipe> &p_recipe) { climate_recipe = p_recipe; }
	Ref<ClimateRecipe> get_climate_recipe() const { return climate_recipe; }
	void set_river_month(int p_month) { river_month = CLAMP(p_month, -1, 11); }
	int get_river_month() const { return river_month; }
	bool has_runoff() const { return !flow_index.is_empty(); }
	double get_last_runoff_msec() const { return runoff_msec; }
	real_t get_mean_runoff() const { return real_t(runoff_mean); }
	real_t get_runoff_at(const Vector3 &p_global_position, int p_month = -1) const;
	real_t get_snow_water_at(const Vector3 &p_global_position, int p_month) const;
	real_t get_discharge_at(const Vector3 &p_global_position, int p_month = -1) const;
	void set_river_width_coefficient(real_t p_value) { river_width_coefficient = MAX(p_value, real_t(0.0)); }
	real_t get_river_width_coefficient() const { return river_width_coefficient; }
	void set_publish_shader_globals(bool p_enable) { publish_shader_globals = p_enable; }
	bool is_publishing_shader_globals() const { return publish_shader_globals; }

	// Coast and lakes.
	void set_ocean_edges(int p_edges) { ocean_edges = p_edges & 15; }
	int get_ocean_edges() const { return ocean_edges; }
	void set_coast_distance(real_t p_value) { coast_distance = MAX(p_value, real_t(0.0)); }
	real_t get_coast_distance() const { return coast_distance; }
	void set_coast_roughness(real_t p_value) { coast_roughness = MAX(p_value, real_t(0.0)); }
	real_t get_coast_roughness() const { return coast_roughness; }
	void set_coast_wavelength(real_t p_value) { coast_wavelength = MAX(p_value, real_t(10.0)); }
	real_t get_coast_wavelength() const { return coast_wavelength; }
	void set_sea_map(const Ref<Texture2D> &p_map) { sea_map = p_map; }
	Ref<Texture2D> get_sea_map() const { return sea_map; }
	void set_seabed_profile(real_t p_value) { seabed_profile = MAX(p_value, real_t(0.0)); }
	real_t get_seabed_profile() const { return seabed_profile; }
	void set_shelf_slope(real_t p_value) { shelf_slope = MAX(p_value, real_t(0.0)); }
	real_t get_shelf_slope() const { return shelf_slope; }
	void set_max_sea_depth(real_t p_value) { max_sea_depth = MAX(p_value, real_t(0.1)); }
	real_t get_max_sea_depth() const { return max_sea_depth; }
	void set_beach_width(real_t p_value) { beach_width = MAX(p_value, real_t(0.0)); }
	real_t get_beach_width() const { return beach_width; }
	void set_beach_slope(real_t p_value) { beach_slope = CLAMP(p_value, real_t(0.001), real_t(1.0)); }
	real_t get_beach_slope() const { return beach_slope; }
	void set_lake_count(int p_count) { lake_count = CLAMP(p_count, 0, 256); }
	int get_lake_count() const { return lake_count; }
	void set_gorge_age(double p_years) { gorge_age = MAX(p_years, 0.0); }
	double get_gorge_age() const { return gorge_age; }
	void set_moraine_age(double p_years) { moraine_age = MAX(p_years, 0.0); }
	double get_moraine_age() const { return moraine_age; }
	double get_last_moraine_volume() const { return last_moraine_volume; }
	void set_scree_age(double p_years) { scree_age = MAX(p_years, 0.0); }
	double get_scree_age() const { return scree_age; }
	double get_last_scree_volume() const { return last_scree_volume; }
	void set_gorge_rate(double p_rate) { gorge_rate = MAX(p_rate, 0.0); }
	double get_gorge_rate() const { return gorge_rate; }
	void set_fan_backfill(bool p_enable) { fan_backfill = p_enable; }
	bool get_fan_backfill() const { return fan_backfill; }
	int get_last_slots_filled() const { return last_slots_filled; }
	void set_plunge_pools(bool p_enable) { plunge_pools = p_enable; }
	bool get_plunge_pools() const { return plunge_pools; }
	void set_fall_lips(bool p_enable) { fall_lips = p_enable; }
	bool get_fall_lips() const { return fall_lips; }
	void set_inflows(const TypedArray<NodePath> &p_inflows) { inflows = p_inflows; }
	TypedArray<NodePath> get_inflows() const { return inflows; }
	void set_inflow_area(real_t p_km2) { inflow_area = MAX(p_km2, real_t(0.0)); }
	real_t get_inflow_area() const { return inflow_area; }
	Array get_inflow_cells() const;
	void set_lakes(const TypedArray<NodePath> &p_lakes) { lakes = p_lakes; }
	TypedArray<NodePath> get_lakes() const { return lakes; }
	void set_lake_depth(real_t p_value) { lake_depth = MAX(p_value, real_t(1.0)); }
	real_t get_lake_depth() const { return lake_depth; }
	void set_lake_catchment(real_t p_value) { lake_catchment = MAX(p_value, real_t(0.0)); }
	real_t get_lake_catchment() const { return lake_catchment; }
	void set_lake_min_area(real_t p_value) { lake_min_area = MAX(p_value, real_t(0.0)); }
	real_t get_lake_min_area() const { return lake_min_area; }
	void set_lake_spacing(real_t p_value) { lake_spacing = MAX(p_value, real_t(0.0)); }
	real_t get_lake_spacing() const { return lake_spacing; }
	void set_lake_evaporation(real_t p_value) { lake_evaporation = MAX(p_value, real_t(0.0)); }
	real_t get_lake_evaporation() const { return lake_evaporation; }

	void set_terrain(const NodePath &p_path) { terrain_path = p_path; }
	NodePath get_terrain() const { return terrain_path; }
	void set_deformer(const NodePath &p_path) { deformer_path = p_path; }
	NodePath get_deformer() const { return deformer_path; }
	void set_ground_mesh(const NodePath &p_path) { ground_mesh_path = p_path; }
	NodePath get_ground_mesh() const { return ground_mesh_path; }

	// World XZ rectangle the generator covers (centred on the node), position.x = X, position.y = Z.
	Rect2 get_world_rect() const;
	Vector2i get_grid_size() const { return Vector2i(nx, ny); }

	// Runs the simulation. Returns false when nothing could run (empty size).
	bool generate();
	// Writes the result into the terrain. Returns the number of vertices written.
	int write_to_terrain();
	// generate() then write_to_terrain(); the inspector's Generate button.
	int bake();
	bool has_result() const { return nx > 0; }
	void clear_result();

	// Generated ground (bicubic between grid nodes, plus height_offset, plus the detail filter); NaN outside or
	// before generate().
	real_t get_height(const Vector3 &p_global_position) const;
	// The simulated ground without the detail filter.
	real_t get_base_height(const Vector3 &p_global_position) const;
	real_t get_drainage_area(const Vector3 &p_global_position) const;
	real_t get_sediment_depth(const Vector3 &p_global_position) const;
	Ref<Image> get_height_image() const;
	Ref<Image> sample_heights(const Vector2 &p_origin, real_t p_spacing, const Vector2i &p_size) const;
	Ref<Image> get_drainage_image() const;
	Ref<Image> get_sediment_image() const;
	Ref<Image> get_uplift_image() const;

	// Tile erosion.
	real_t get_tile_flow(const Vector3 &p_global_position) const;
	real_t get_tile_deposit(const Vector3 &p_global_position) const;
	double get_last_tile_erosion_msec() const { return last_tile_erosion_msec; }

	// Geology (soil, rock, talus) and rivers.
	real_t get_soil_depth(const Vector3 &p_global_position) const;
	real_t get_rock_exposure(const Vector3 &p_global_position) const;
	real_t get_talus(const Vector3 &p_global_position) const;
	real_t get_rock_hardness(const Vector3 &p_global_position) const;
	Array get_river_sites(real_t p_min_area = 1.0e5, real_t p_min_discharge = 0.0, real_t p_run = 200.0) const;
	Array get_waterfalls(real_t p_min_area = 1.0e5, real_t p_min_height = 5.0) const;
	PackedVector4Array get_boulders(const Rect2 &p_rect, real_t p_density = 1.0) const;
	void set_spray_cell(real_t p_cell) { spray_cell = MAX(p_cell, real_t(0.0)); }
	real_t get_spray_cell() const { return spray_cell; }
	real_t get_spray_potential(const Vector3 &p_global_position) const;
	Ref<Image> get_spray_image() const;
	Color get_geology_at(const Vector3 &p_global_position) const;
	Ref<Image> get_geology_image() const;
	Ref<Texture2D> get_geology_texture() const { return geology_texture; }
	static String get_geology_shader_code();
	String get_detail_shader_code() const;
	real_t get_discharge(real_t p_area) const;
	Array get_rivers(real_t p_min_area) const;
	Array build_river_paths(Node *p_parent, real_t p_min_area);

	// Coast and lakes.
	bool has_sea_result() const { return has_sea; }
	// Time the coast, lakes and their routing took in the last generate() (part of get_last_generate_msec()).
	double get_last_water_msec() const { return last_water_msec; }
	int get_lake_result_count() const { return int(lake_info.size()); }
	Array get_lake_results() const;
	real_t get_water_level(const Vector3 &p_global_position) const;
	real_t get_water_depth(const Vector3 &p_global_position) const;
	Ref<Image> get_water_image() const;
	Array build_water(Node *p_parent);

	double get_last_generate_msec() const { return last_generate_msec; }
	double get_last_write_msec() const { return last_write_msec; }
	int get_last_steps() const { return last_steps; }
	int64_t get_last_deposition_passes() const { return last_deposition_passes; }
	real_t get_last_change() const { return last_change; }

	Callable get_bake_button();

	PackedStringArray get_configuration_warnings() const override;
};

VARIANT_ENUM_CAST(TerrainGenerator3D::OutletEdge);

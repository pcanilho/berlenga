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
	real_t critical_slope = 40.0; // degrees
	real_t flow_exponent = 1.0;
	real_t time_step = 20000.0; // years
	int steps = 300;
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
	LocalVector<float> uplift_field;
	Rect2 result_rect; // where the last generate() ran; the result stays there if the node moves
	real_t result_cell = 1.0;
	float result_min = 0.0f;
	float result_max = 0.0f;
	double last_generate_msec = 0.0;
	double last_write_msec = 0.0;
	int last_steps = 0;
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
	void set_critical_slope(real_t p_degrees) { critical_slope = CLAMP(p_degrees, real_t(0.0), real_t(80.0)); }
	real_t get_critical_slope() const { return critical_slope; }
	void set_flow_exponent(real_t p_value) { flow_exponent = CLAMP(p_value, real_t(0.0), real_t(10.0)); }
	real_t get_flow_exponent() const { return flow_exponent; }
	void set_time_step(real_t p_years) { time_step = MAX(p_years, real_t(1.0)); }
	real_t get_time_step() const { return time_step; }
	void set_steps(int p_steps) { steps = MAX(p_steps, 0); }
	int get_steps() const { return steps; }
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
	void set_detail_assumed_slope(real_t p_value) { detail_assumed_slope = MAX(p_value, real_t(0.01)); }
	real_t get_detail_assumed_slope() const { return detail_assumed_slope; }

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
	Ref<Image> get_drainage_image() const;
	Ref<Image> get_sediment_image() const;
	Ref<Image> get_uplift_image() const;

	double get_last_generate_msec() const { return last_generate_msec; }
	double get_last_write_msec() const { return last_write_msec; }
	int get_last_steps() const { return last_steps; }
	real_t get_last_change() const { return last_change; }

	Callable get_bake_button();

	PackedStringArray get_configuration_warnings() const override;
};

VARIANT_ENUM_CAST(TerrainGenerator3D::OutletEdge);

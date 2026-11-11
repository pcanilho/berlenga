/**************************************************************************/
/*  tree_generator.h                                                      */
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

#include "core/io/resource.h"
#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "plant_species.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

// Trees and woody shrubs grown by a self-organizing growth model (docs/ideas/trees.md):
// Palubicki et al., "Self-organizing Tree Models for Image Synthesis", SIGGRAPH 2009 (buds
// compete for light in a shadow-propagation grid; vigor flows from the leaves to the base and
// back up by the Borchert-Honda model; branches that stay in shade are shed), with the
// developmental parameters of Stava et al., "Inverse Procedural Modelling of Trees", CGF 2014
// (branching and roll angles, phototropism, gravitropism, apical control by age, bud death,
// gravity bending). Branch radii follow the pipe model (Shinozaki et al. 1964).
class TreeGenerator : public Resource {
	GDCLASS(TreeGenerator, Resource);

public:
	enum Preset {
		PRESET_OAK,
		PRESET_BEECH,
		PRESET_BIRCH,
		PRESET_SCOTS_PINE,
		PRESET_NORWAY_SPRUCE,
		PRESET_HAZEL,
		PRESET_HAWTHORN,
		PRESET_LIME,
		PRESET_SYCAMORE_MAPLE,
		PRESET_ASH,
		PRESET_HORSE_CHESTNUT,
		PRESET_BLACK_ALDER,
		PRESET_WHITE_WILLOW,
		PRESET_LARCH,
		PRESET_SILVER_FIR,
		PRESET_ROWAN,
		PRESET_ELDER,
		PRESET_HOLLY,
		PRESET_YEW,
		PRESET_BOX,
		// Eastern North America.
		PRESET_SUGAR_MAPLE,
		PRESET_RED_MAPLE,
		PRESET_RED_OAK,
		PRESET_WHITE_OAK,
		PRESET_WHITE_PINE,
		PRESET_HEMLOCK,
		PRESET_AMERICAN_BEECH,
		PRESET_PAPER_BIRCH,
		PRESET_YELLOW_BIRCH,
		PRESET_WITCH_HAZEL,
		PRESET_MOUNTAIN_LAUREL,
		PRESET_MAX,
	};

	enum Phyllotaxis {
		PHYLLOTAXIS_ALTERNATE, // One lateral bud per node, rotated by the divergence angle.
		PHYLLOTAXIS_OPPOSITE, // Two opposite buds per node, pairs crossed (decussate).
		PHYLLOTAXIS_WHORLED, // A whorl of buds at the end of each year's shoot only (pines, spruces).
	};

	// Stand types: which species grow together, after the European forest vegetation classes
	// (Ellenberg, "Vegetation Ecology of Central Europe", 1988; EUNIS G1-G3 and FA hedgerows).
	enum Stand {
		STAND_BEECH, // Fagion: beech, with sycamore and ash; sparse shrubs in gaps.
		STAND_OAK_MIXED, // Carpinion-type: oak and lime, ash in moist hollows, hazel coppice layer.
		STAND_MONTANE_CONIFER, // Spruce-fir(-beech) of the montane belt.
		STAND_SUBALPINE, // Larch and spruce up to the tree line, rowan.
		STAND_PINE_HEATH, // Scots pine on dry, poor ground, with birch and rowan.
		STAND_RIPARIAN, // Alder carr and willows along water, elder.
		STAND_HEDGEROW, // Hawthorn, hazel, elder, rowan, with open-grown oak, lime and chestnut standards.
		// Eastern North America.
		STAND_NORTHERN_HARDWOOD, // Sugar maple, American beech, yellow birch, hemlock in cool ravines.
		STAND_EASTERN_OAK, // Red and white oak with red maple; mountain laurel and witch-hazel beneath.
		STAND_PINE_HEMLOCK, // White pine and hemlock, paper birch in openings.
		STAND_MAX,
	};

	enum Region {
		REGION_EUROPE,
		REGION_NORTH_AMERICA,
		REGION_MAX,
	};

	enum Param {
		PARAM_YEARS, // Growth iterations (years).
		PARAM_INTERNODE_LENGTH, // Metres, in the first year; also the shadow grid cell.
		PARAM_INTERNODE_LENGTH_AGE, // Per-year factor on the internode length (shoots shorten with age).
		PARAM_MAX_SHOOT, // Most internodes one bud makes in a year.
		PARAM_VIGOR, // Borchert-Honda alpha: vigor at the base per unit of collected light.
		PARAM_APICAL_DOMINANCE, // Borchert-Honda lambda while young (0.5 = no preference).
		PARAM_APICAL_DOMINANCE_OLD, // Lambda once mature: decurrent crowns lose apical control.
		PARAM_MATURITY, // Years over which lambda moves from young to old.
		PARAM_BRANCH_ANGLE, // Degrees between a lateral bud and its parent.
		PARAM_BRANCH_ANGLE_VARIANCE, // Degrees.
		PARAM_DIVERGENCE, // Phyllotaxis roll between successive nodes, degrees.
		PARAM_WHORL_BUDS, // Buds per whorl (whorled phyllotaxis).
		PARAM_DIRECTION_NOISE, // Degrees of random turn per internode.
		PARAM_PHOTOTROPISM, // Weight of the best-lit direction.
		PARAM_GRAVITROPISM, // Upward pull on laterals (negative: drooping).
		PARAM_TRUNK_GRAVITROPISM, // Upward pull on the leader(s).
		PARAM_SHADOW_STRENGTH, // Shadow a leafy bud casts on its own cell (Palubicki a).
		PARAM_SHADOW_FALLOFF, // Per-level falloff of the shadow pyramid (Palubicki b > 1).
		PARAM_SHADOW_DEPTH, // Levels of the shadow pyramid.
		PARAM_BUD_LIGHT_MIN, // Buds below this light die.
		PARAM_BUD_IDLE_YEARS, // Years a bud may stay dormant before it dies.
		PARAM_SHED_RATIO, // A branch whose light per internode falls below this is shed.
		PARAM_DEAD_BRANCH_KEEP, // Share of shed branches that stay on as dead wood.
		PARAM_TIP_RADIUS, // Metres.
		PARAM_PIPE_EXPONENT, // Pipe model exponent (2 = da Vinci's rule).
		PARAM_GRAVITY_BENDING, // Sag of branches under their own weight.
		PARAM_STEMS, // Stems from the base (1 for trees; shrubs have several).
		PARAM_STEM_SPREAD, // Degrees from vertical of the outer stems.
		PARAM_BASAL_SPROUTS, // Chance per year of a new stem from the base (basitony).
		PARAM_ROOT_FLARE, // Extra trunk radius at the ground, as a fraction.
		PARAM_LEAF_SIZE, // Leaf or needle length, metres.
		PARAM_LEAVES_PER_INTERNODE,
		PARAM_LEAF_YEARS, // Years a shoot keeps its leaves (1 deciduous, several for needles).
		PARAM_NEEDLES, // 1: needles instead of broad leaves.
		PARAM_TWIG_DROOP, // Downward hang of thin shoots (birch, spruce), radians at the tip.
		PARAM_LEAF_WIDTH, // Leaf blade width over length (willow narrow, chestnut leaflets broad).
		PARAM_APICAL_CONTROL, // Shoot growth on a branch of order k is scaled by this^k (Stava 2014; excurrent conifers < 1).
		PARAM_LATERAL_EXTENSION, // Shoot length on a branch of order k is scaled by this^k; length only, not vigor.
		PARAM_CROWN_CONE, // Excurrent crown: branches reach at most this times their depth below the top (0: off).
		PARAM_LEADER_GROWTH, // Most metres a leader grows in a year (0: no limit).
		PARAM_MAX,
	};

private:
	struct Node {
		Vector3 start;
		Vector3 dir;
		float length = 0.0f;
		int parent = -1;
		int main = -1; // Child that continues this axis.
		int first_lateral = -1;
		int next_sibling = -1; // Next lateral of the same parent.
		int order = 0; // 0: trunk or a basal stem; laterals add one.
		int born = 0; // Year the internode grew.
		float radius = 0.0f;
		float light_main = 0.0f; // Light collected through the main continuation (and terminal bud).
		float light_lateral = 0.0f; // Light collected through laterals (and lateral buds).
		float vigor = 0.0f;
		int count = 1; // Internodes in this subtree.
		bool dead = false; // Shed but kept as dead wood: no buds, no leaves, frozen radius.
		bool removed = false;
		bool leafy = false;
		Vector3 end() const { return start + dir * length; }
	};

	struct Bud {
		int node = -1; // Internode carrying the bud, at its end.
		Vector3 dir;
		bool terminal = false;
		bool alive = true;
		int idle = 0;
		float light = 0.0f;
		float vigor = 0.0f;
	};

	struct Builder {
		PackedVector3Array verts;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
	};

	Preset preset = PRESET_OAK;
	int seed = 0;
	float params[PARAM_MAX] = {};
	Phyllotaxis phyllotaxis = PHYLLOTAXIS_ALTERNATE;
	float forest_shade = 0.0f;
	float forest_spacing = 4.0f;
	int mask_channel = -1; // Passed to every PlantSpecies the stand methods build.
	bool mask_invert = false;
	Color bark_color = Color(0.3f, 0.26f, 0.21f);
	Color leaf_color = Color(0.16f, 0.3f, 0.08f);

	// Generated meshes per preset and form, reused by make_species() and make_stand().
	HashMap<int, TypedArray<Mesh>> mesh_cache;
	HashMap<int, float> crown_cache;

	LocalVector<Node> nodes;
	LocalVector<uint8_t> spurs; // Per node: carries short-shoot leaves in the mesh (not in growth).
	LocalVector<Bud> buds;
	RandomPCG rng;

	// Shadow-propagation grid (Palubicki 2009, section 4.2).
	LocalVector<float> shadow;
	Vector3i grid_size;
	Vector3 grid_origin;
	float cell = 0.5f;

	float height = 0.0f;
	float crown_radius = 0.0f;
	float crown_base = 0.0f;
	float bole_height = 0.0f;
	int leaf_count = 0;
	float leaf_area_index = 0.0f;
	static constexpr float MAX_LEAF_AREA_INDEX = 5.5f;
	static constexpr float MAX_LEAF_AREA_DENSITY = 1.5f; // m2 of leaf per m3 of crown.
	int shed_count = 0;
	uint64_t generation_usec = 0;

	void _set_north_american(Preset p_preset);
	float _rand(float p_from, float p_to) { return Math::lerp(p_from, p_to, rng.randf()); }
	Vector3 _random_unit();
	float _lambda(int p_year) const;
	float _internode_length(int p_year) const;
	int _add_node(int p_parent, const Vector3 &p_start, const Vector3 &p_dir, float p_length, int p_order, int p_year, bool p_main);
	void _add_lateral_buds(int p_node, float &r_roll, bool p_end_of_shoot);
	Vector3 _bud_direction(const Vector3 &p_axis, float p_roll);

	void _build_shadow_grid(int p_year);
	_FORCE_INLINE_ int _cell_index(const Vector3i &p_c) const { return (p_c.y * grid_size.z + p_c.z) * grid_size.x + p_c.x; }
	Vector3i _cell_of(const Vector3 &p_pos) const;
	float _shadow_at(const Vector3 &p_pos) const;
	Vector3 _best_lit_direction(const Vector3 &p_pos, const Vector3 &p_dir) const;

	void _grow_year(int p_year);
	void _shed(int p_year);
	void _compute_radii();
	void _bend_under_gravity();
	void _measure();

	static void _tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color);
	void _add_leaf(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, const Vector3 &p_up, float p_size, const Color &p_color);
	void _add_needle(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color);
	void _mark_spurs(int p_year);
	Ref<ArrayMesh> _build_mesh();
	Ref<PlantSpecies> _stand_species(Preset p_preset, bool p_stand_grown, int p_variants);

protected:
	static void _bind_methods();

public:
	void set_preset(Preset p_preset);
	Preset get_preset() const { return preset; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }
	void set_param(Param p_param, float p_value);
	float get_param(Param p_param) const;
	void set_phyllotaxis(Phyllotaxis p_phyllotaxis) { phyllotaxis = p_phyllotaxis; }
	Phyllotaxis get_phyllotaxis() const { return phyllotaxis; }
	void set_forest_shade(float p_shade) { forest_shade = p_shade; }
	float get_forest_shade() const { return forest_shade; }
	void set_forest_spacing(float p_spacing) { forest_spacing = p_spacing; }
	float get_forest_spacing() const { return forest_spacing; }
	void set_mask_channel(int p_channel) { mask_channel = CLAMP(p_channel, -1, 3); }
	int get_mask_channel() const { return mask_channel; }
	void set_mask_invert(bool p_invert) { mask_invert = p_invert; }
	bool get_mask_invert() const { return mask_invert; }
	void set_bark_color(const Color &p_color) { bark_color = p_color; }
	Color get_bark_color() const { return bark_color; }
	void set_leaf_color(const Color &p_color) { leaf_color = p_color; }
	Color get_leaf_color() const { return leaf_color; }

	Ref<ArrayMesh> generate();

	// Placement (docs/ideas/trees.md, section 8): the preset's habitat as PlantSpecies rules,
	// a species with generated variants, and stands as Biomes for VegetationField3D.
	void apply_habitat(const Ref<PlantSpecies> &p_species) const;
	Ref<PlantSpecies> make_species(bool p_stand_grown, int p_variants);
	Ref<Biome> make_stand(Stand p_stand, int p_variants);
	Ref<Biome> make_woodland(int p_variants, Region p_region = REGION_EUROPE);
	void clear_cache();

	float get_height() const { return height; }
	float get_crown_radius() const { return crown_radius; }
	float get_crown_base() const { return crown_base; }
	float get_bole_height() const { return bole_height; }
	PackedFloat32Array get_foliage_profile(int p_bins) const;
	int get_internode_count() const;
	int get_dead_branch_count() const;
	int get_leaf_count() const { return leaf_count; }
	float get_leaf_area_index() const { return leaf_area_index; }
	int get_shed_count() const { return shed_count; }
	int get_generation_usec() const { return int(generation_usec); }

	TreeGenerator();
};

VARIANT_ENUM_CAST(TreeGenerator::Preset);
VARIANT_ENUM_CAST(TreeGenerator::Phyllotaxis);
VARIANT_ENUM_CAST(TreeGenerator::Param);
VARIANT_ENUM_CAST(TreeGenerator::Stand);
VARIANT_ENUM_CAST(TreeGenerator::Region);

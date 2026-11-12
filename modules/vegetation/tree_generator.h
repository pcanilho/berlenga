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

	// Leaf architecture (Ellis et al., Manual of Leaf Architecture, 2009).
	enum LeafKind {
		LEAF_SIMPLE,
		LEAF_LOBED, // Pinnately lobed: oaks, hawthorn.
		LEAF_PALMATE, // Palmately lobed: sycamore, maples.
		LEAF_PINNATE, // Pinnately compound: ash, rowan, elder.
		LEAF_DIGITATE, // Palmately compound: horse chestnut.
		LEAF_NEEDLE,
	};

	enum Climber {
		CLIMBER_NONE,
		CLIMBER_IVY, // Hedera helix: root-climbing up the stem, evergreen.
		CLIMBER_LIANA, // Clematis vitalba, wild grape: hanging from the limbs, looping between them.
		CLIMBER_HONEYSUCKLE, // Lonicera: twining in spirals round thin stems and limbs.
		CLIMBER_MAX,
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
		// Bark only: tangents along u (around the stem), w = 1; vertices built without one get
		// a padded tangent in end().
		PackedFloat32Array tangents;
		bool use_tangents = false;
		// Parts for LODs: first index, index count, first vertex, vertex count; and a size (tube
		// radius for bark).
		LocalVector<Vector4i> parts;
		LocalVector<float> part_size;
		// Skin (rigged trees): four bone indices and weights per vertex; parts take `bone`.
		PackedInt32Array bones;
		PackedFloat32Array weights;
		int bone = 0;
		// Bark relief (section 2a): a displaced tube is drawn only at LOD 0 (and by the path
		// tracer), the plain tube it replaces only at the coarser levels.
		enum Role : uint8_t {
			ROLE_ALL,
			ROLE_RELIEF,
			ROLE_PLAIN,
		};
		LocalVector<uint8_t> part_role;
		uint8_t role = ROLE_ALL;
		void begin(Vector4i &r_part) const { r_part = Vector4i(indices.size(), 0, verts.size(), 0); }
		void end(Vector4i &r_part, float p_size) {
			r_part.y = indices.size() - r_part.x;
			r_part.w = verts.size() - r_part.z;
			while (use_tangents && tangents.size() < verts.size() * 4) {
				tangents.push_back(1.0f);
				tangents.push_back(0.0f);
				tangents.push_back(0.0f);
				tangents.push_back(1.0f);
			}
			for (int v = bones.size() / 4; v < verts.size(); v++) {
				bones.push_back(bone);
				bones.push_back(0);
				bones.push_back(0);
				bones.push_back(0);
				weights.push_back(1.0f);
				weights.push_back(0.0f);
				weights.push_back(0.0f);
				weights.push_back(0.0f);
			}
			if (r_part.y > 0) {
				parts.push_back(r_part);
				part_size.push_back(p_size);
				part_role.push_back(role);
			}
		}
	};
	// One bone of the wind rig (docs/ideas/trees.md, section 3a).
	struct RigBone {
		int parent = -1;
		Vector3 pivot;
		Vector3 dir;
		float length = 0.0f;
		float radius = 0.0f; // At the middle of the bone: sets its bending stiffness.
		float zeta = 0.15f;
		int axis = -1; // First node of the axis it belongs to (-1: root).
		int segment = 0; // Segment of a split stem.
		int group = 0; // First bone of its axis.
		double compliance = 0.0; // Radians per N m at its base.
		bool stem = false;
		// Accumulated over everything the bone carries: drag area (and its first moment), mass
		// (and its first and second moments).
		double a0 = 0.0, m0 = 0.0, m2 = 0.0;
		Vector3 a1, m1;
	};
	LocalVector<RigBone> rig;
	LocalVector<int> node_bone;
	LocalVector<float> node_drag; // m2 of effective frontal area per node (wood and foliage).
	LocalVector<float> node_mass; // kg per node.
	LocalVector<float> axis_offset; // Per node: arc length from its axis start to the node's start.
	HashMap<int, Vector2> stem_segments; // Stem axis -> (segments, segment length).
	HashMap<int, int> axis_bone; // Boned axis -> its (first) bone.
	void _build_rig();
	void _apply_decay();
	void _fell();
	void _shed_branch();
	PackedFloat32Array _collider_data() const;
	void _skin_stem(Builder &r_b, const Vector4i &p_part, int p_axis, const LocalVector<Vector3> &p_points, int p_sides) const;
	PackedFloat32Array _rig_data() const;
	Dictionary _bark_lods(const Builder &p_bark) const;
	Dictionary _leaf_lods(Builder &r_leaves, bool p_twins = true) const;
	// Climber leaves from PlantGenerator.make_climber_leaves (one baked set per climber, shared),
	// instanced into their own surface; empty when climber_plant_leaves is off.
	Builder climber_leaf_b;
	bool climber_plant_leaves = true;
	float leaf_flutter = 1.0f; // Leaf flutter amount (trees.md 3b); 0 keeps the plain material.
	Ref<Material> _flutter_material(const Ref<StandardMaterial3D> &p_base, float p_leaf_size) const;
	static Array _plant_climber_leaves(Climber p_climber);
	static Array plant_climber_cache[4];

public:
	static void free_climber_leaves() {
		for (Array &a : plant_climber_cache) {
			a.clear();
		}
	}

private:

	Preset preset = PRESET_OAK;
	int seed = 0;
	float params[PARAM_MAX] = {};
	Phyllotaxis phyllotaxis = PHYLLOTAXIS_ALTERNATE;
	float forest_shade = 0.0f;
	float forest_spacing = 4.0f;
	int mask_channel = -1; // Passed to every PlantSpecies the stand methods build.
	bool needle_sprays = false;
	float near_needle_range = 0.0f; // Metres; > 0 gives placed conifers a full-needle near mesh.
	bool skip_lods = false;
	float bark_relief = 1.0f; // Scales the displaced bark of LOD 0; 0 = plain tubes only.
	static constexpr float RELIEF_MIN_RADIUS = 0.08f; // Stems and limbs this thick get relief.
	bool rigged = false;
	int decay_class = 0; // 0 living; 1-5 deadwood decay classes (Maser et al. 1979).
	bool fallen = false; // Lying on the ground as a log.
	bool fallen_branch = false; // Only one shed limb of the grown tree, lying on the ground.
	AABB envelope; // Clipping volume for hedges and topiary (size 0: off).
	float collider_radius = 0.08f; // Wood thicker than this gets collision capsules.
	int broken_tip = -1; // Stem node whose end is a broken top.
	// Old-tree features (docs/ideas/trees.md 5e): tree-related microhabitats of veteran trees.
	float veteran = 0.0f;
	enum VeteranKind : uint8_t {
		VET_BASAL_HOLLOW,
		VET_ROT_HOLE,
		VET_WOODPECKER,
		VET_BURL,
		VET_EPICORMIC,
	};
	struct VeteranFeature {
		VeteranKind kind;
		float h; // Height above the stem base (m).
		float azimuth; // Radians around the stem.
		float half_h; // Half height (m).
		float half_w; // Half width (m of girth).
		float amount; // Depth or bulge, as a share of the local radius; shoots for epicormics.
	};
	LocalVector<VeteranFeature> veteran_features;
	int veteran_stem = -1;
	void _apply_veteran();
	bool mask_invert = false;
	Color bark_color = Color(0.3f, 0.26f, 0.21f);
	Color leaf_color = Color(0.16f, 0.3f, 0.08f);

	// Generated meshes per preset and form, reused by make_species() and make_stand().
	HashMap<int, TypedArray<Mesh>> mesh_cache;
	bool junction_weld = true; // Limbs welded onto their parent's bark (trees.md 2d).
	Dictionary season; // Empty: midsummer, full green leaves.
	struct LeafSeason {
		float out = 1.0f; // This year's leaves grown (0 none, 1 full size).
		float autumn = 0.0f; // Share of leaves turned.
		float fallen = 0.0f; // Share of the latest leaf crop on the ground.
		Color color; // Turned leaf colour.
		// The turned share of the leaves still on the tree.
		float season_blend() const { return fallen >= 1.0f ? 0.0f : CLAMP((autumn - fallen) / (1.0f - fallen), 0.0f, 1.0f); }
	};
	static LeafSeason _leaf_season(Preset p_preset, const Dictionary &p_season);
	HashMap<int, float> crown_cache;
	HashMap<int, TypedArray<Mesh>> near_cache;

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

public:
	static constexpr int MAX_BONES = 48;
	// Moss on bark (docs/ideas/trees.md section 5c): a tint at every LOD and a displaced moss
	// shell over the bark at LOD 0, by species, height, upper faces and decay.
	float moss_cover = 1.0f;
	bool compress_vertices = true; // Godot's compressed vertex format: 33% less vertex memory, 20% rigged (section 4).
	Climber climber = CLIMBER_NONE;
	float shed_radius_max = 0.14f; // Thickest limb _shed_branch() picks (FORM_STICKS: 4 cm).
	float climber_cover = 0.6f;
	float _moss_amount() const;
	float _moss_at(const Vector3 &p_pos, const Vector3 &p_normal, float p_ground, float p_amount) const;
	static constexpr float MOSS_SHELL_MIN_RADIUS = 0.05f;
	static constexpr float BARK_TILE = 0.4f; // Metres of bark one texture tile covers.
	static constexpr int RIG_STRIDE = 18;
	// Rig record layout (Mesh meta "tree_rig", RIG_STRIDE floats per bone).
	enum RigField {
		RIG_PARENT,
		RIG_PIVOT_X,
		RIG_PIVOT_Y,
		RIG_PIVOT_Z,
		RIG_CENTROID_X, // Drag centroid of the carried load, relative to the pivot.
		RIG_CENTROID_Y,
		RIG_CENTROID_Z,
		RIG_AREA, // m2, effective frontal area of the carried wood and foliage.
		RIG_STIFFNESS, // N m per radian.
		RIG_INERTIA, // kg m2 about the pivot.
		RIG_ZETA,
		RIG_RADIUS,
		RIG_LENGTH,
		RIG_DIR_X,
		RIG_DIR_Y,
		RIG_DIR_Z,
		RIG_GROUP, // First bone of its stem (segments sway as one mode), else the bone itself.
		RIG_MASS, // kg carried.
	};

private:
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

	static void _tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color, const Image *p_height = nullptr, float p_relief = 0.0f);
	void _add_leaf(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, const Vector3 &p_up, float p_size, const Color &p_color);
	// Species leaves (docs/ideas/trees.md section 2b): the detailed blade at LOD 0 and for the
	// path tracer, a simple lens of the same area (its twin) at the coarser levels.
	void _add_blade(Builder &r_b, const Vector3 &p_start, const Vector3 &p_along, const Vector3 &p_side, const Vector3 &p_n, float p_length, float p_half_width, int p_steps, int p_lobes, float p_depth, const Color &p_color, bool p_round = false, float p_shape = 0.75f);
	Builder leaf_twins;
	HashMap<int, Vector4i> leaf_twin_of; // Leaf part index -> its twin's indices and vertices in leaf_twins.
	bool species_leaf_forms = true; // Off for climbers' leaves.
	uint32_t compound_slot = 0;
	Ref<Texture2D> leaf_texture;
	Ref<Texture2D> _leaf_texture();
	void _add_needle(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color);
	void _add_spray(Builder &r_b, const Vector3 &p_base, const Vector3 &p_dir, float p_length, float p_half_width, const Vector3 &p_side, const Color &p_color);
	Ref<Texture2D> spray_texture; // Built on first use; per generator so it never outlives the renderer.
	Ref<Texture2D> _spray_texture();
	// Bark (docs/ideas/trees.md section 2a): one tileable procedural set per bark family.
	enum BarkKind {
		BARK_SMOOTH,
		BARK_FURROWED,
		BARK_PLATED,
		BARK_SCALY,
		BARK_FLAKING,
		BARK_PAPERY,
		BARK_MAX,
	};
	struct BarkSet {
		Ref<Texture2D> albedo;
		Ref<Texture2D> normal;
		Ref<Texture2D> roughness;
		Ref<Image> height; // Metres of relief over the tile, for displaced near trunks.
	};
	BarkSet bark_sets[BARK_MAX];
	BarkSet moss_set;
	const BarkSet &_moss_set();
	static BarkKind _bark_kind(Preset p_preset);
	const BarkSet &_bark_set(BarkKind p_kind);
	void _mark_spurs(int p_year);
	Ref<ArrayMesh> _build_mesh();
	// Climbers (docs/ideas/trees.md section 5b): stems grown over the host's bark, in its bark
	// surface and skinned to its bones; leaves in its leaf surface and published as points.
	struct Support {
		LocalVector<Vector3> pts;
		LocalVector<float> rad;
		LocalVector<int> bone;
		bool stem = false;
	};
	LocalVector<Support> supports;
	PackedFloat32Array climber_leaves;
	void _grow_climbers(Builder &r_bark, Builder &r_leaves);
	void _climber_stem(Builder &r_bark, const LocalVector<Vector3> &p_path, const LocalVector<int> &p_bones, float p_r0, float p_r1, const Color &p_color);
	void _climber_leaf(Builder &r_leaves, const Vector3 &p_at, const Vector3 &p_out, const Vector3 &p_normal, float p_size, int p_bone, const Color &p_color);
	enum Form {
		FORM_OPEN,
		FORM_STAND,
		FORM_SAPLING, // Advance regeneration: the stand-grown form at a fifth of its age.
		FORM_SNAG, // Standing dead, decay classes 2-4 over the variants.
		FORM_LOG, // Fallen dead, decay classes 2-4.
		FORM_BRANCH, // A shed limb on the ground (coarse woody debris under the logs' size).
		FORM_STICKS, // Small shed branches, 1-4 cm, scattered densely between the plants.
	};
	Ref<PlantSpecies> _stand_species(Preset p_preset, bool p_stand_grown, int p_variants, Form p_form = FORM_OPEN);

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
	void set_near_needle_range(float p_range) { near_needle_range = MAX(p_range, 0.0f); }
	float get_near_needle_range() const { return near_needle_range; }
	void set_bark_relief(float p_relief) { bark_relief = MAX(p_relief, 0.0f); }
	float get_bark_relief() const { return bark_relief; }
	void set_needle_sprays(bool p_sprays) { needle_sprays = p_sprays; }
	bool get_needle_sprays() const { return needle_sprays; }
	void set_decay_class(int p_class) { decay_class = CLAMP(p_class, 0, 5); }
	int get_decay_class() const { return decay_class; }
	void set_fallen(bool p_fallen) { fallen = p_fallen; }
	bool is_fallen() const { return fallen; }
	void set_climber(Climber p_climber) { climber = p_climber; }
	Climber get_climber() const { return climber; }
	void set_climber_cover(float p_cover) { climber_cover = CLAMP(p_cover, 0.0f, 1.0f); }
	float get_climber_cover() const { return climber_cover; }
	void set_compress_vertices(bool p_enable) { compress_vertices = p_enable; }
	bool is_compress_vertices() const { return compress_vertices; }
	void set_moss_cover(float p_cover) { moss_cover = MAX(p_cover, 0.0f); }
	float get_moss_cover() const { return moss_cover; }
	void set_fallen_branch(bool p_fallen_branch) { fallen_branch = p_fallen_branch; }
	bool is_fallen_branch() const { return fallen_branch; }
	void set_envelope(const AABB &p_envelope) { envelope = p_envelope; }
	AABB get_envelope() const { return envelope; }
	void set_collider_radius(float p_radius) { collider_radius = MAX(p_radius, 0.0f); }
	float get_collider_radius() const { return collider_radius; }
	PackedFloat32Array get_collision_capsules() const { return _collider_data(); }
	void set_rigged(bool p_rigged) { rigged = p_rigged; }
	bool is_rigged() const { return rigged; }
	int get_bone_count() const { return rig.size(); }
	void set_mask_invert(bool p_invert) { mask_invert = p_invert; }
	bool get_mask_invert() const { return mask_invert; }
	void set_bark_color(const Color &p_color) { bark_color = p_color; }
	Color get_bark_color() const { return bark_color; }
	void set_leaf_color(const Color &p_color) { leaf_color = p_color; }
	Color get_leaf_color() const { return leaf_color; }
	static Dictionary get_leaf_form(Preset p_preset);
	// Phenology from the shared season state (WeatherMap3D.get_season()): leaf_out, autumn,
	// fallen_fraction and autumn_color for one species. docs/ideas/trees.md 2c.
	static Dictionary get_leaf_season(Preset p_preset, const Dictionary &p_season);
	void set_season(const Dictionary &p_season) {
		season = p_season;
		mesh_cache.clear();
	}
	Dictionary get_season() const { return season; }
	void set_junction_weld(bool p_on) {
		junction_weld = p_on;
		mesh_cache.clear();
	}
	bool get_junction_weld() const { return junction_weld; }
	void set_climber_plant_leaves(bool p_on) {
		climber_plant_leaves = p_on;
		mesh_cache.clear();
	}
	bool get_climber_plant_leaves() const { return climber_plant_leaves; }
	void set_leaf_flutter(float p_amount) {
		leaf_flutter = MAX(p_amount, 0.0f);
		mesh_cache.clear();
	}
	float get_leaf_flutter() const { return leaf_flutter; }
	void set_veteran(float p_amount) {
		veteran = CLAMP(p_amount, 0.0f, 1.0f);
		mesh_cache.clear();
	}
	float get_veteran() const { return veteran; }
	int get_veteran_feature_count() const { return veteran_features.size(); }
	Array get_veteran_features() const;

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
VARIANT_ENUM_CAST(TreeGenerator::Climber);
VARIANT_ENUM_CAST(TreeGenerator::LeafKind);
VARIANT_ENUM_CAST(TreeGenerator::RigField);

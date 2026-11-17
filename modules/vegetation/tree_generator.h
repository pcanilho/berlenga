/**************************************************************************/
/*  tree_generator.h                                                      */
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

#include "plant_species.h"

#include "core/io/resource.h"
#include "core/math/random_pcg.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
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
		PRESET_GORSE,
		PRESET_BROOM,
		PRESET_GUM_ROCKROSE, // Cistus ladanifer (Mediterranean scrub, trees.md 5v).
		PRESET_KERMES_OAK, // Quercus coccifera.
		PRESET_LENTISK, // Pistacia lentiscus.
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
		STAND_COPPICE_STANDARDS, // Hazel coppice in coupes of different ages under oak and ash standards.
		STAND_WOOD_PASTURE, // Grazed: lapsed beech and oak pollards, open-grown oaks, thorn and holly.
		STAND_POLLARD_ROW, // Willow pollards along water, placed by the scene along its lines.
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
		bool pole = false; // A resprout's axis after a cut (section 5l): vigorous and straight.
		bool layered = false; // A krummholz stem from a limb rooted in the ground (section 5s): grows as a stem.
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
		bool sprout = false; // A cut stem's resprout (section 5l): keeps its node's order.
		bool layer = false; // A stem rising where a krummholz limb rooted (section 5s).
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
		// Bark only: the lichen layer's UV2 from UV, per part (scale u, scale v, offset u, offset
		// v; zero scale: no lichen there), so its pattern does not repeat with the bark's tile.
		Vector4 uv2_map;
		LocalVector<Vector4> part_uv2;
		// Bark only: vertices that are not bark (a log's broken end face), left out of peeling.
		LocalVector<uint32_t> not_bark;
		// Bark only: per vertex, the relief's offset (metres, 0 on plate tops, negative in furrows)
		// and its scale, so lost bark can sink to the furrow floor; vertices without one read 0.
		LocalVector<Vector2> relief_off;
		// Bark only: per vertex, bare wood where a living stem was wounded (section 5p); vertices
		// without one read 0. _peel_bark takes the larger of it and dead wood's peeling.
		LocalVector<float> wound;
		// Bark only: vertices added after the parts were closed (a log's peel edge, section 5o),
		// each with the part whose UV2 map it takes.
		LocalVector<Vector2i> extra_part;
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
				part_uv2.push_back(uv2_map);
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
	void _promote_lod1(Builder &r_b, Dictionary &r_lods) const;
	// Climber leaves from PlantGenerator.make_climber_leaves (one baked set per climber, shared),
	// instanced into their own surface; empty when climber_plant_leaves is off.
	Builder climber_leaf_b;
	Builder climber_adult_b; // Ivy's adult leaves, on their own material (trees.md 5i).
	bool climber_plant_leaves = true;
	float leaf_flutter = 1.0f; // Leaf flutter amount (trees.md 3b); 0 keeps the plain material.
	Ref<Material> _flutter_material(const Ref<StandardMaterial3D> &p_base, float p_leaf_size) const;
	Ref<Material> _lichen_material(const Ref<StandardMaterial3D> &p_base);
	Ref<Texture2D> lichen_texture;
	Ref<Texture2D> _lichen_texture();
	// Bracket fungi (trees.md 5g): fruiting bodies of wood-decay fungi on veteran trees, snags and
	// logs, by host and decay class, in their own surface.
	void _grow_fungi(const Builder &p_bark, Builder &r_fungi);
	// Small woody and evergreen parts in the crown (section 5n): cones, catkins, seed keys,
	// mistletoe, witches' brooms. Returns the index count of the large parts (mistletoe, brooms),
	// which the coarser LODs keep.
	int _grow_crown_parts(Builder &r_b);
	Ref<Texture2D> fungus_texture;
	Ref<Texture2D> wood_texture;
	Ref<Texture2D> wood_normal_texture;
	Ref<Texture2D> _wood_texture(bool p_normal);
	Builder wood_b; // Broken wood: fracture faces, splinters, torn bark (surface "wood").
	void _fracture(const Vector3 &p_end, const Vector3 &p_dir, float p_r, float p_old, uint32_t p_seed, const Vector3 &p_tension = Vector3());
	PackedFloat32Array fungi_points; // Mesh meta tree_fungi: 8 floats per cluster.
	int64_t fungi_index_end = 0; // Fungi surface indices before the lichen strands (LOD 1 keeps these).
	Ref<Texture2D> _fungus_texture();
	float _lichen_amount() const;
	float _lichen_at(const Vector3 &p_pos, float p_radius, float p_moss) const;
	// Bark lost from dead wood (trees.md 5f): 0 bark, 1 bare, 0.5 the edge.
	float _bare_amount() const;
	float _bare_at(const Vector3 &p_pos, const Vector3 &p_normal, const Vector3 &p_axis, float p_ground) const;
	void _peel_bark(Builder &r_bark);
	void _split_peel_edge(Builder &r_b, const LocalVector<Vector3> &p_orig, const LocalVector<float> &p_bare, const LocalVector<float> &p_depth, const LocalVector<uint8_t> &p_skip);
	// Bark stories (section 5p): wounds on the main stem, planned per tree before the mesh.
	enum WoundKind : uint8_t {
		WOUND_LIGHTNING,
		WOUND_FROST,
		WOUND_DEER,
		WOUND_SQUIRREL,
		WOUND_CHECK, // A drying check down a log (section 5o).
		WOUND_CHEVRON, // A shed branch's scar and its two arms below it (section 5r); geometry only.
	};
	struct Wound {
		WoundKind kind = WOUND_LIGHTNING;
		float s0 = 0.0f, s1 = 0.0f; // Along the stem, metres from its foot (squirrel: centre, half height).
		float az0 = 0.0f; // Round the stem (radians) at s0.
		float twist = 0.0f; // Radians per metre up the stem (the grain's spiral).
		float half_w = 0.0f; // Half width across the stem, metres.
		float age = 0.0f; // Years since the wound.
		float extra = 0.0f; // Chevron: the arms' length (metres); negative when no scar is drawn (a stub stands there).
		uint32_t salt = 0;
	};
	LocalVector<Wound> wounds;
	// The main stem's surface after wounding, for the resin (max radius per 2 cm along and 96
	// steps round), and its centreline.
	LocalVector<float> wound_surf;
	LocalVector<Vector3> wound_pts;
	LocalVector<float> wound_rad;
	LocalVector<float> wound_arc;
	// Per wound_surf bin, the bark vertex that set its radius and that vertex's part (x -1: none).
	LocalVector<Vector2i> wound_vtx;
	float _krumm_cap(const Vector3 &p_q, float *r_slope) const;
	void _plan_wounds();
	void _plan_chevrons();
	void _grow_chevrons(Builder &r_bark);
	bool _wounds_resin() const;
	// Offset out from the stem (metres), bare wood 0..1 and darkening 0..1 at s along the main
	// stem and az round it, on a stem of radius p_r; p_only: one wound (-1 all).
	Vector3 _wound_at(float p_s, float p_az, float p_r, int p_only = -1) const;
	void _grow_checks();
	void _grow_resin(Builder &r_resin);
	// Where a lying stem's features are, for plants that root at them (vegetation's honey
	// fungus): peel edges and checks, 8 floats each (mesh meta "tree_deadwood_features").
	PackedFloat32Array deadwood_features;
	// Slots: the Climber kinds, then CLIMBER_MAX for ivy's adult leaves (trees.md 5i).
	static Array _plant_climber_leaves(int p_slot);
	static Array plant_climber_cache[CLIMBER_MAX + 1];
	// The same leaves' arrays, read once: reading a mesh back from the RenderingServer waits on
	// the render thread for every leaf, and blocks near meshes built on worker threads.
	struct ClimberLeafShape {
		PackedVector3Array verts;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedInt32Array indices;
	};
	static LocalVector<ClimberLeafShape> plant_climber_shapes[CLIMBER_MAX + 1];

public:
	static void free_climber_leaves() {
		for (Array &a : plant_climber_cache) {
			a.clear();
		}
		for (LocalVector<ClimberLeafShape> &a : plant_climber_shapes) {
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
	float near_needle_range = 30.0f; // Metres; > 0 gives placed conifers a full-needle near mesh (on by default: every feature on, the desktop tunes cost).
	// Leaf detail near, on demand (docs/ideas/trees.md 8b): metres; > 0 gives placed trees with
	// lobed or compound leaves a far mesh of simple leaves, and their species a near_mesh_source
	// that builds the full-leaf mesh when a tree first comes within this range.
	float near_leaf_range = 60.0f; // On by default (every feature on; the desktop tunes cost).
	bool far_leaves = false; // _add_leaf(): species leaves as their simple twins only.
	// Far meshes from LOD 1 (trees.md 8d): with near_leaf_range, every broadleaf's far mesh starts
	// at the pruned, area-kept LOD 1 leaves; LOD 0 lives only in the near mesh.
	bool far_lod1 = true;
	bool skip_lods = false;
	float bark_relief = 1.0f; // Scales the displaced bark of LOD 0; 0 = plain tubes only.
	static constexpr float RELIEF_MIN_RADIUS = 0.08f; // Stems and limbs this thick get relief.
	bool rigged = false;
	int decay_class = 0; // 0 living; 1-5 deadwood decay classes (Maser et al. 1979).
	bool fallen = false; // Lying on the ground as a log.
	// Pollards and coppice (section 5l): cut back at cut_height (m; 0 never; up to 0.5 m a coppice
	// stool, above that a pollard head) every cut_cycle years, the last cut cut_since years ago
	// (-1: some time within the last cycle).
	float cut_height = 0.0f;
	int cut_cycle = 12;
	int cut_since = -1;
	HashSet<int> sprout_roots; // First nodes of shoots grown since the last cut.
	LocalVector<int> cut_heads; // Stubs and knuckles the head is made of.
	int last_cut_year = -1000;
	HashSet<int> bolling; // The head's nodes, swollen (mesher: no taper at their ends).
	HashMap<int, int> cut_stubs; // Sawn pole stubs on the head or stool, by the year they were cut.
	HashMap<int, int> seat_cuts; // Knuckles and stool tops: how many times poles were cut from them.
	bool uprooted = false; // A fallen tree thrown with its root plate (section 5k); else snapped at the butt.
	// The root plate of an uprooted tree (section 5k): built upright about base0, then turned
	// with the stem about the hinge.
	struct RootPlate {
		bool on = false;
		Vector3 base0, hinge;
		Basis rot;
		float radius = 0.0f, thick = 0.0f, trunk_r = 0.0f;
		Vector3 to_world(const Vector3 &p_local) const { return hinge + rot.xform(base0 + p_local - hinge); }
	} plate;
	Builder soil_b; // The root plate's soil and stones (section 5k).
	bool fallen_branch = false; // Only one shed limb of the grown tree, lying on the ground.
	AABB envelope; // Clipping volume for hedges and topiary (size 0: off).
	float collider_radius = 0.08f; // Wood thicker than this gets collision capsules.
	int broken_tip = -1; // Stem node whose end is a broken top.
	HashSet<int> broken_ends; // Limbs that rubbed on thicker wood and snapped (_prune_collisions()).
	LocalVector<int> thinned_stubs; // A lapsed pollard's poles that died in the self-thinning.
	// The lying stem of a log or fallen limb (deadwood interface): its first node, axis ends and
	// reach from the axis along it; see _measure_lying().
	static constexpr int DEADWOOD_SAMPLES = 12;
	// A pinnate leaflet's blade over the leaflet's own half-width: the texture cuts the rest.
	static constexpr float LEAFLET_HULL = 1.25f;
	int deadwood_stem = -1;
	Vector3 deadwood_a, deadwood_b;
	float deadwood_profile[DEADWOOD_SAMPLES] = {};
	// The stem's centreline, DEADWOOD_SAMPLES points evenly along it by arc length (mesh space),
	// and the girth measured round each (to the outside of the moss).
	Vector3 deadwood_line[DEADWOOD_SAMPLES];
	float deadwood_girth[DEADWOOD_SAMPLES] = {};
	HashSet<int> hanger_bases; // First nodes of snapped limbs left hanging in the crown (_break_limbs()).
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
	float head_y0 = 0.0f; // A pollard head's lowest point (world y) for its knobs and fissures; head_r 0: none.
	float head_r = 0.0f;
	int veteran_stem = -1;
	void _apply_veteran();
	void _break_limbs();
	void _measure_lying(const Builder &p_b, int p_v0, int p_v1);
	void _root_plate(Builder &r_bark);
	// Storm breaks and windthrow at run time (section 5t): a unit fracture cap and the root plate a
	// standing tree would throw, with the tree's surface materials.
	void _storm_kit(const Ref<ArrayMesh> &p_mesh, const Array &p_static_materials);
	static void _storm_pack(Builder &r_b, bool p_flip, const String &p_name, const Ref<Material> &p_m, Array &r_out);
	Ref<StandardMaterial3D> _wood_material();
	Ref<StandardMaterial3D> _soil_material() const;
	int _main_stem() const;
	void _settle_log(float p_ground, float p_trunk_r);
	void _cut_back(bool p_first, int p_year);
	void _saw_face(const Vector3 &p_end, const Vector3 &p_dir, float p_r, float p_age, uint32_t p_seed);
	// A coppice stool's knuckle: its end is the first cut, an old sloping saw face.
	bool _coppice_lobe(int p_node) const { return cut_height > 0.0f && cut_height <= 0.5f && decay_class == 0 && !fallen && !fallen_branch && cut_heads.has(p_node); }
	float _stem_top() const;
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
	float lichen_cover = 1.0f;
	float fungi_cover = 1.0f; // Bracket fungi on old and dead wood (section 5g); 0 none.
	float fruit_cover = 1.0f; // Cones, catkins and seed keys in season (section 5n); 0 none.
	float mistletoe = 1.0f; // Chance and amount of mistletoe on host species (section 5n); 0 none.
	float witches_brooms = 1.0f; // Chance and amount of witches' brooms (birches; section 5n).
	float limb_breakage = 1.0f; // Storm-snapped and hanging limbs on living trees (section 5j); 0 none.
	float bark_peel = 1.0f; // Dead wood sheds its bark by decay class (section 5f); 0 keeps it.
	// Exposure to a prevailing wind blowing toward +X (trees.md 5q): ice and sand kill the windward
	// buds above the shelter height (flagged crowns); krummholz 0..1 turns it into the low mat of
	// the tree line. Placement turns the mesh's +X down the prevailing wind (meta tree_wind_flag).
	float wind_exposure = 0.0f;
	float krummholz = 0.0f;
	// A tree on a bank (trees.md 5q): the ground falls toward +Z at this slope (radians), and
	// bank_erosion 0..1 has washed the soil from its downhill roots, which arch exposed down the
	// face. Placement turns +Z downhill (meta tree_ground_slope).
	float ground_slope = 0.0f;
	float bank_erosion = 0.0f;
	// A stone the roots drape over (trees.md 5q): radius in metres (0: none), beside the stem on
	// its +Z side, half in the soil; the generator builds it (surface "soil").
	float root_stone = 0.0f;
	struct RootStone {
		bool on = false;
		Vector3 c; // Centre.
		Vector3 r; // Half extents along x, y, z.
		float ground = 0.0f;
		float root_top = 0.0f;
		// The stone's own mesh (RockGenerator, when the rocks module is in), fitted to c and r:
		// counter-clockwise like the root plate, COLOR.r its cavity. Empty: a noise ellipsoid.
		PackedVector3Array verts;
		PackedVector3Array normals;
		PackedFloat32Array cavity;
		PackedInt32Array indices;
		// Its top above the ground on a gn x gn grid over c.xz +- r.xz, for the roots' drape.
		LocalVector<float> top;
		int gn = 0;
	} stone;
	bool storm_kit = true; // Rigged trees carry a fracture cap and a root plate for storm breaks (section 5t).
	float bark_chevrons = 1.0f; // Branch-scar chevrons below shed limbs on smooth and papery bark (section 5r); 0 none.
	float bark_wounds = 1.0f; // How often living stems carry lightning scars, frost cracks, stripping (section 5p); 0 none.
	bool compress_vertices = true; // Godot's compressed vertex format: 33% less vertex memory, 20% rigged (section 4).
	Climber climber = CLIMBER_NONE;
	float shed_radius_max = 0.14f; // Thickest limb _shed_branch() picks (FORM_STICKS: 4 cm).
	float climber_cover = 0.6f;
	float _moss_amount() const;
	// The surface roots of the last generate(), for the moss: bare bark on their upper faces,
	// moss in the crotches between them (trees.md 5d).
	struct FootRoots {
		bool on = false;
		Vector3 axis;
		float trunk_r = 0.0f;
		float width = 0.0f; // Half width of a root's top (m).
		LocalVector<float> az, len, bend;
	} foot_roots;
	float _moss_at(const Vector3 &p_pos, const Vector3 &p_normal, float p_ground, float p_amount, bool p_clamp = true) const;
	static constexpr float MOSS_SHELL_MIN_RADIUS = 0.05f;
	static constexpr float BARK_TILE = 0.4f; // Metres of bark one texture tile covers.
	static constexpr float LICHEN_TILE = 0.6f; // Metres of bark the lichen layer's tile covers.
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
	int collision_prunes = 0;
	int collision_deflects = 0;
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

	float _nurse_scrub_height() const;
	void _grow_year(int p_year);
	void _shed(int p_year);
	void _compute_radii();
	void _bend_under_gravity();
	void _prune_collisions();
	void _measure();

	static void _tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color, const Image *p_height = nullptr, float p_relief = 0.0f, float p_girth_r = -1.0f, bool p_soil_fade = true);
	void _add_leaf(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, const Vector3 &p_up, float p_size, const Color &p_color);
	// Species leaves (docs/ideas/trees.md section 2b): the detailed blade at LOD 0 and for the
	// path tracer, a simple lens of the same area (its twin) at the coarser levels.
	void _add_blade(Builder &r_b, const Vector3 &p_start, const Vector3 &p_along, const Vector3 &p_side, const Vector3 &p_n, float p_length, float p_half_width, int p_steps, int p_lobes, float p_depth, const Color &p_color, bool p_round = false, float p_shape = 0.75f);
	Builder leaf_twins;
	HashMap<int, Vector4i> leaf_twin_of; // Leaf part index -> its twin's indices and vertices in leaf_twins.
	bool species_leaf_forms = true; // Off for climbers' leaves.
	int twin_scale_preset = -1;
	float twin_scale = 1.0f;
	float twin_scale_width = 0.0f;
	float _twin_area_scale();
	void _add_species_blade(Builder &r_b, const Vector3 &p_start, const Vector3 &p_along, const Vector3 &p_side, const Vector3 &p_n, float p_size, float p_hw, float p_slot_area, const Color &p_color);
	uint32_t compound_slot = 0;
	Ref<Texture2D> leaf_texture;
	Ref<Texture2D> _leaf_texture();
	Ref<Texture2D> leaflet_texture;
	Ref<Texture2D> leaflet_entire_texture;
	Ref<Texture2D> _leaflet_texture();
	void _add_needle(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color);
	void _add_spine(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color, uint32_t p_hash, bool p_branched);
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
		BARK_LENTICELLED, // Hazel: glossy, pale lenticels, thin peeling skin.
		BARK_MAX,
	};
	struct BarkSet {
		Ref<Texture2D> albedo;
		Ref<Texture2D> normal;
		Ref<Texture2D> roughness;
		Ref<Image> height; // Metres of relief over the tile, for displaced near trunks.
		float relief_gain = 0.0f; // Metres the displaced near tube is closer to the relief than a smooth one.
		float relief_along = 0.05f; // Metres between the displaced tube's rings.
		float relief_floor = 0.0f; // The height map's lowest value (metres, the deepest furrow).
	};
	BarkSet bark_sets[BARK_MAX];
	BarkSet moss_set;
	const BarkSet &_moss_set();
	static BarkKind _bark_kind(Preset p_preset);
	static float _bark_headroom(BarkKind p_kind);
	void _wind_flag();
	void _bank_shear(Builder &r_b, bool p_drape) const;
	void _plan_root_stone_rock();
	float _root_stone_top(float p_x, float p_z) const;
	void _build_root_stone(Builder &r_b) const;
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
	static constexpr int CLIMBER_LEAF_FLOATS = 13; // Mesh meta tree_climber_leaves, per leaf.
	float climber_stem_age = 0.0f; // The stem the next climber leaf grows on: age 0-1 (meta float 11).
	float climber_stem_up = 0.0f; // How far up that stem the leaf is, 0-1 (meta float 12).
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
		FORM_UNDERSTORY, // A shrub grown in a stand's shade: taller, thinner, fewer leaves.
		FORM_COPPICE, // Cut to a stool on a cycle: a grown coupe, 4-9 years since the cut.
		FORM_COPPICE_YOUNG, // A coupe cut 1-3 years ago: a stool of short rods.
		FORM_POLLARD, // Cut at head height: willow on a short cycle, others lapsed for decades.
		FORM_FLAGGED, // Open-grown in a hard prevailing wind: limbs on the lee side (section 5q).
		FORM_KRUMMHOLZ, // The tree line's low wind- and snow-shorn mat (section 5q).
		FORM_BANK, // On a stream bank or gorge side: variants over slopes 0 to 0.85 rad, roots eroded (section 5q).
		FORM_THICKET, // Heath shrubs grown into one: wider than tall, flat-topped (section 5u).
		FORM_OLD, // A heath shrub near the end of its life: leggy, leaning out, its centre dead and grey (5u).
		FORM_MAX,
	};
	Ref<PlantSpecies> _stand_species(Preset p_preset, bool p_stand_grown, int p_variants, Form p_form = FORM_OPEN);
	// One variant's state in a stand species: seed, decay and climber.
	void _setup_variant(Preset p_preset, Form p_form, int p_variant, int p_base_seed, float p_base_cover);
	// Near meshes on demand: one source (a copy of the generator as set up for the species) per
	// species key, shared by every PlantSpecies of that key and kept alive by them.
	HashMap<int, Ref<TreeGenerator>> near_sources;
	void _copy_generation_state(const TreeGenerator &p_from);
	int near_key = -1;
	Form near_form = FORM_OPEN;
	int near_base_seed = 0;
	float near_base_cover = 0.0f;
	HashMap<int, ObjectID> near_built; // Variant -> mesh, held only by its users (the field's cells).
	Ref<ArrayMesh> near_ready; // A finished mesh not yet handed out.
	int near_ready_variant = -1;
	WorkerThreadPool::TaskID near_task = WorkerThreadPool::INVALID_TASK_ID;
	int near_task_variant = -1;
	Ref<ArrayMesh> near_result;
	TypedArray<Mesh> near_far_meshes; // The species' far meshes, whose materials a near mesh shares.
	Ref<ArrayMesh> material_source;
	static void _near_task(void *p_self);

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
	void set_near_leaf_range(float p_range) {
		near_leaf_range = MAX(p_range, 0.0f);
		mesh_cache.clear();
		near_sources.clear();
	}
	float get_near_leaf_range() const { return near_leaf_range; }
	void set_far_lod1(bool p_on) {
		far_lod1 = p_on;
		mesh_cache.clear();
		near_sources.clear();
	}
	bool get_far_lod1() const { return far_lod1; }
	// For PlantSpecies.near_mesh_source: the full-leaf mesh of one variant (index into the
	// species' meshes), or null while it is being built on a worker thread.
	Ref<Mesh> request_near_mesh(int p_key, int p_variant);
	int get_near_mesh_count() const;
	float get_near_needle_range() const { return near_needle_range; }
	void set_bark_relief(float p_relief) { bark_relief = MAX(p_relief, 0.0f); }
	float get_bark_relief() const { return bark_relief; }
	void set_needle_sprays(bool p_sprays) { needle_sprays = p_sprays; }
	bool get_needle_sprays() const { return needle_sprays; }
	void set_decay_class(int p_class) { decay_class = CLAMP(p_class, 0, 5); }
	int get_decay_class() const { return decay_class; }
	void set_fallen(bool p_fallen) { fallen = p_fallen; }
	bool is_fallen() const { return fallen; }
	void set_uprooted(bool p_uprooted) { uprooted = p_uprooted; }
	void set_cut_height(float p_height) { cut_height = MAX(p_height, 0.0f); }
	float get_cut_height() const { return cut_height; }
	void set_cut_cycle(int p_years) { cut_cycle = MAX(p_years, 2); }
	int get_cut_cycle() const { return cut_cycle; }
	void set_cut_since(int p_years) { cut_since = p_years; }
	int get_cut_since() const { return cut_since; }
	bool is_uprooted() const { return uprooted; }
	void set_climber(Climber p_climber) { climber = p_climber; }
	Climber get_climber() const { return climber; }
	void set_climber_cover(float p_cover) { climber_cover = CLAMP(p_cover, 0.0f, 1.0f); }
	float get_climber_cover() const { return climber_cover; }
	void set_compress_vertices(bool p_enable) { compress_vertices = p_enable; }
	bool is_compress_vertices() const { return compress_vertices; }
	void set_moss_cover(float p_cover) { moss_cover = MAX(p_cover, 0.0f); }
	float get_moss_cover() const { return moss_cover; }
	void set_lichen_cover(float p_cover) { lichen_cover = MAX(p_cover, 0.0f); }
	float get_lichen_cover() const { return lichen_cover; }
	void set_bark_peel(float p_peel) { bark_peel = MAX(p_peel, 0.0f); }
	void set_root_stone(float p_v) { root_stone = CLAMP(p_v, 0.0f, 2.0f); }
	float get_root_stone() const { return root_stone; }
	void set_ground_slope(float p_v) { ground_slope = CLAMP(p_v, 0.0f, 1.2f); }
	float get_ground_slope() const { return ground_slope; }
	void set_bank_erosion(float p_v) { bank_erosion = CLAMP(p_v, 0.0f, 1.0f); }
	float get_bank_erosion() const { return bank_erosion; }
	void set_wind_exposure(float p_v) { wind_exposure = CLAMP(p_v, 0.0f, 1.0f); }
	float get_wind_exposure() const { return wind_exposure; }
	void set_krummholz(float p_v) { krummholz = CLAMP(p_v, 0.0f, 1.0f); }
	float get_krummholz() const { return krummholz; }
	void set_bark_wounds(float p_wounds) { bark_wounds = MAX(p_wounds, 0.0f); }
	float get_bark_wounds() const { return bark_wounds; }
	void set_storm_kit(bool p_on) { storm_kit = p_on; }
	bool get_storm_kit() const { return storm_kit; }
	static Array build_storm_plate(const PackedFloat32Array &p_storm);
	void set_bark_chevrons(float p_amount) { bark_chevrons = CLAMP(p_amount, 0.0f, 2.0f); }
	float get_bark_chevrons() const { return bark_chevrons; }
	void set_fungi_cover(float p_cover) { fungi_cover = MAX(p_cover, 0.0f); }
	float get_fungi_cover() const { return fungi_cover; }
	void set_fruit_cover(float p_cover) { fruit_cover = MAX(p_cover, 0.0f); }
	float get_fruit_cover() const { return fruit_cover; }
	void set_mistletoe(float p_amount) { mistletoe = MAX(p_amount, 0.0f); }
	float get_mistletoe() const { return mistletoe; }
	void set_witches_brooms(float p_amount) { witches_brooms = MAX(p_amount, 0.0f); }
	float get_witches_brooms() const { return witches_brooms; }
	void set_limb_breakage(float p_breakage) { limb_breakage = MAX(p_breakage, 0.0f); }
	float get_limb_breakage() const { return limb_breakage; }
	float get_bark_peel() const { return bark_peel; }
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
	// Heath shrubs for vegetation's placement (trees.md 5u): meshes, footprint, canopy radius and
	// far colour, no climate or terrain rules.
	enum ShrubForm {
		SHRUB_YOUNG,
		SHRUB_MATURE,
		SHRUB_OLD,
		SHRUB_THICKET,
		SHRUB_CUSHION,
		SHRUB_MAX,
	};
	static Ref<PlantSpecies> make_shrub_species(Preset p_preset, ShrubForm p_form, int p_variants, const Dictionary &p_season);
	// Open share of a heath shrub's flowers on a day of the year (0 for other presets).
	static float get_bloom(Preset p_preset, float p_day_of_year);
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
	int get_collision_prune_count() const { return collision_prunes; }
	int get_collision_deflect_count() const { return collision_deflects; }
	int get_generation_usec() const { return int(generation_usec); }

	TreeGenerator();
	~TreeGenerator();
};

VARIANT_ENUM_CAST(TreeGenerator::Preset);
VARIANT_ENUM_CAST(TreeGenerator::Phyllotaxis);
VARIANT_ENUM_CAST(TreeGenerator::Param);
VARIANT_ENUM_CAST(TreeGenerator::Stand);
VARIANT_ENUM_CAST(TreeGenerator::Region);
VARIANT_ENUM_CAST(TreeGenerator::Climber);
VARIANT_ENUM_CAST(TreeGenerator::LeafKind);
VARIANT_ENUM_CAST(TreeGenerator::RigField);
VARIANT_ENUM_CAST(TreeGenerator::ShrubForm);

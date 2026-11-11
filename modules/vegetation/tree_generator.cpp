/**************************************************************************/
/*  tree_generator.cpp                                                    */
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

#include "tree_generator.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/image_texture.h"

static constexpr int MAX_NODES = 150000;

static const char *preset_names[TreeGenerator::PRESET_MAX] = {
	"Oak", "Beech", "Birch", "Scots Pine", "Norway Spruce", "Hazel", "Hawthorn", "Lime", "Sycamore Maple", "Ash",
	"Horse Chestnut", "Black Alder", "White Willow", "European Larch", "Silver Fir", "Rowan", "Elder", "Holly", "Yew", "Box",
	"Sugar Maple", "Red Maple", "Northern Red Oak", "White Oak", "Eastern White Pine", "Eastern Hemlock", "American Beech",
	"Paper Birch", "Yellow Birch", "Witch-hazel", "Mountain Laurel"
};

static const char *stand_names[TreeGenerator::STAND_MAX] = {
	"Beech Forest", "Oak Mixed Forest", "Montane Conifer Forest", "Subalpine Forest", "Pine Heath", "Riparian Woodland", "Hedgerow",
	"Northern Hardwood Forest", "Eastern Oak Forest", "Pine-Hemlock Forest"
};

TreeGenerator::TreeGenerator() {
	set_preset(PRESET_OAK);
}

// Species defaults. Shapes come from the growth model; these numbers are the botany: how strongly
// the leader dominates and for how long (excurrent conifers versus decurrent broadleaves), how
// much shade the species tolerates before it sheds (beech and spruce keep low branches, pine and
// birch prune themselves), branching angles and phyllotaxis, and leaf size.
// The eastern North American species grow by the same rules as their closest European relatives
// (same genus or growth habit), so each starts from that preset and changes what differs: crown
// habit, leaf and needle size, bark and colour (Burns and Honkala, "Silvics of North America",
// USDA Agriculture Handbook 654, 1990; Sibley, "The Sibley Guide to Trees", 2009).
static TreeGenerator::Preset european_relative(TreeGenerator::Preset p_preset) {
	switch (p_preset) {
		case TreeGenerator::PRESET_SUGAR_MAPLE:
		case TreeGenerator::PRESET_RED_MAPLE:
			return TreeGenerator::PRESET_SYCAMORE_MAPLE;
		case TreeGenerator::PRESET_RED_OAK:
		case TreeGenerator::PRESET_WHITE_OAK:
			return TreeGenerator::PRESET_OAK;
		case TreeGenerator::PRESET_WHITE_PINE:
			return TreeGenerator::PRESET_SCOTS_PINE;
		case TreeGenerator::PRESET_HEMLOCK:
			return TreeGenerator::PRESET_SILVER_FIR;
		case TreeGenerator::PRESET_AMERICAN_BEECH:
			return TreeGenerator::PRESET_BEECH;
		case TreeGenerator::PRESET_PAPER_BIRCH:
		case TreeGenerator::PRESET_YELLOW_BIRCH:
			return TreeGenerator::PRESET_BIRCH;
		case TreeGenerator::PRESET_WITCH_HAZEL:
			return TreeGenerator::PRESET_HAZEL;
		case TreeGenerator::PRESET_MOUNTAIN_LAUREL:
			return TreeGenerator::PRESET_BOX;
		default:
			return p_preset;
	}
}

void TreeGenerator::set_preset(Preset p_preset) {
	ERR_FAIL_INDEX(int(p_preset), int(PRESET_MAX));
	const Preset relative = european_relative(p_preset);
	if (relative != p_preset) {
		set_preset(relative);
		preset = p_preset;
		_set_north_american(p_preset);
		emit_changed();
		notify_property_list_changed();
		return;
	}
	preset = p_preset;
	float *p = params;
	// Shared defaults.
	p[PARAM_YEARS] = 40;
	p[PARAM_INTERNODE_LENGTH] = 0.4f;
	p[PARAM_INTERNODE_LENGTH_AGE] = 0.985f;
	p[PARAM_MAX_SHOOT] = 2;
	p[PARAM_VIGOR] = 2.0f;
	p[PARAM_APICAL_DOMINANCE] = 0.58f;
	p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
	p[PARAM_MATURITY] = 25;
	p[PARAM_BRANCH_ANGLE] = 45;
	p[PARAM_BRANCH_ANGLE_VARIANCE] = 10;
	p[PARAM_DIVERGENCE] = 137.5f;
	p[PARAM_WHORL_BUDS] = 5;
	p[PARAM_DIRECTION_NOISE] = 8;
	p[PARAM_PHOTOTROPISM] = 0.12f;
	p[PARAM_GRAVITROPISM] = 0.08f;
	p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
	// Shade and shedding are soft: sky light reaches a crown from the whole hemisphere, not only
	// from above, so a short, light shadow pyramid stands in for it; with a deeper or darker one
	// every upper layer killed the branches below and open-grown trees kept leaves only on top
	// (crown base 14 m on an open oak against 2-4 m in nature).
	p[PARAM_SHADOW_STRENGTH] = 0.08f;
	p[PARAM_SHADOW_FALLOFF] = 1.25f;
	p[PARAM_SHADOW_DEPTH] = 4;
	// Buds die of shade only where no light reaches at all. A threshold above that killed the
	// buds of every crowded crown a few metres under its top (stand-grown trees kept a leafy cap
	// and no crown); shade-tolerant buds live on a few percent of open light (Larcher 2003), and
	// branches that do not pay for themselves still go by shedding.
	p[PARAM_BUD_LIGHT_MIN] = 0.0f;
	p[PARAM_BUD_IDLE_YEARS] = 3;
	p[PARAM_SHED_RATIO] = 0.05f;
	p[PARAM_DEAD_BRANCH_KEEP] = 0.15f;
	p[PARAM_TIP_RADIUS] = 0.004f;
	p[PARAM_PIPE_EXPONENT] = 2.1f;
	p[PARAM_GRAVITY_BENDING] = 0.0f;
	p[PARAM_STEMS] = 1;
	p[PARAM_STEM_SPREAD] = 0;
	p[PARAM_BASAL_SPROUTS] = 0;
	p[PARAM_ROOT_FLARE] = 0.5f;
	p[PARAM_LEAF_SIZE] = 0.1f;
	p[PARAM_LEAVES_PER_INTERNODE] = 32;
	p[PARAM_LEAF_YEARS] = 1;
	p[PARAM_NEEDLES] = 0;
	p[PARAM_TWIG_DROOP] = 0;
	p[PARAM_APICAL_CONTROL] = 1.0f;
	p[PARAM_LATERAL_EXTENSION] = 1.0f;
	p[PARAM_CROWN_CONE] = 0.0f;
	p[PARAM_LEADER_GROWTH] = 0.7f;
	p[PARAM_LEAF_WIDTH] = 0.32f;
	phyllotaxis = PHYLLOTAXIS_ALTERNATE;
	switch (p_preset) {
		case PRESET_OAK: // Quercus robur: decurrent, wide, gnarled, light-demanding.
			p[PARAM_LEADER_GROWTH] = 0.65f;
			p[PARAM_YEARS] = 45;
			p[PARAM_INTERNODE_LENGTH] = 0.45f;
			p[PARAM_APICAL_DOMINANCE] = 0.57f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.49f;
			p[PARAM_MATURITY] = 20;
			p[PARAM_BRANCH_ANGLE] = 50;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_GRAVITROPISM] = 0.04f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.35f;
			p[PARAM_SHED_RATIO] = 0.056f;
			p[PARAM_ROOT_FLARE] = 0.7f;
			p[PARAM_LEAF_SIZE] = 0.11f;
			bark_color = Color(0.27f, 0.24f, 0.2f);
			leaf_color = Color(0.13f, 0.25f, 0.06f);
			break;
		case PRESET_BEECH: // Fagus sylvatica: tall, smooth grey bark, very shade tolerant.
			p[PARAM_LEADER_GROWTH] = 0.6f;
			p[PARAM_YEARS] = 50;
			p[PARAM_APICAL_DOMINANCE] = 0.57f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.51f;
			p[PARAM_MATURITY] = 35;
			p[PARAM_BRANCH_ANGLE] = 42;
			p[PARAM_DIVERGENCE] = 180; // Distichous.
			p[PARAM_DIRECTION_NOISE] = 6;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
			p[PARAM_SHED_RATIO] = 0.026f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.05f;
			p[PARAM_LEAF_SIZE] = 0.08f;
			p[PARAM_LEAVES_PER_INTERNODE] = 36;
			bark_color = Color(0.45f, 0.45f, 0.42f);
			leaf_color = Color(0.17f, 0.3f, 0.07f);
			break;
		case PRESET_BIRCH: // Betula pendula: narrow, pendulous twigs, white bark, pioneer.
			p[PARAM_LEADER_GROWTH] = 0.9f;
			p[PARAM_YEARS] = 28;
			p[PARAM_APICAL_DOMINANCE] = 0.58f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.54f;
			p[PARAM_BRANCH_ANGLE] = 38;
			p[PARAM_DIRECTION_NOISE] = 6;
			p[PARAM_GRAVITROPISM] = 0.12f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.75f;
			p[PARAM_SHED_RATIO] = 0.059f;
			p[PARAM_TWIG_DROOP] = 1.2f;
			// Birch crowns are narrow (about a quarter as wide as tall); branches lengthen slower
			// than the leader.
			p[PARAM_LATERAL_EXTENSION] = 0.65f;
			p[PARAM_LEAF_SIZE] = 0.05f;
			p[PARAM_LEAVES_PER_INTERNODE] = 40;
			p[PARAM_ROOT_FLARE] = 0.3f;
			bark_color = Color(0.74f, 0.72f, 0.68f);
			leaf_color = Color(0.2f, 0.34f, 0.08f);
			break;
		case PRESET_SCOTS_PINE: // Pinus sylvestris: whorls, self-pruning, flat crown when old.
			p[PARAM_YEARS] = 34;
			p[PARAM_INTERNODE_LENGTH] = 0.4f;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 5;
			p[PARAM_APICAL_DOMINANCE] = 0.74f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.52f;
			p[PARAM_MATURITY] = 30;
			p[PARAM_BRANCH_ANGLE] = 65;
			p[PARAM_DIRECTION_NOISE] = 9;
			p[PARAM_GRAVITROPISM] = 0.0f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.9f;
			p[PARAM_APICAL_CONTROL] = 0.7f;
			p[PARAM_PHOTOTROPISM] = 0.08f;
			p[PARAM_SHED_RATIO] = 0.099f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.1f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.065f;
			p[PARAM_LEAVES_PER_INTERNODE] = 120;
			p[PARAM_LEAF_YEARS] = 3;
			bark_color = Color(0.42f, 0.27f, 0.18f);
			leaf_color = Color(0.12f, 0.22f, 0.1f);
			p[PARAM_CROWN_CONE] = 0.45f;
			p[PARAM_SHADOW_STRENGTH] = 0.04f;
			break;
		// Conifers keep a cone: branches reach about 0.3 of their depth below the top (open-grown
		// spruce and fir crowns are about a third as wide as tall), with a lighter shadow so the
		// denser crown keeps its buds.
		case PRESET_NORWAY_SPRUCE: // Picea abies: strong leader, drooping laterals, dead lower limbs.
			p[PARAM_LEADER_GROWTH] = 0.8f;
			p[PARAM_YEARS] = 50;
			p[PARAM_INTERNODE_LENGTH] = 0.35f;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 5;
			p[PARAM_APICAL_DOMINANCE] = 0.8f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.76f;
			p[PARAM_BRANCH_ANGLE] = 80;
			p[PARAM_DIRECTION_NOISE] = 5;
			p[PARAM_GRAVITROPISM] = -0.08f;
			p[PARAM_TRUNK_GRAVITROPISM] = 1.2f;
			p[PARAM_APICAL_CONTROL] = 0.65f;
			p[PARAM_PHOTOTROPISM] = 0.04f;
			p[PARAM_SHED_RATIO] = 0.033f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.7f;
			p[PARAM_TWIG_DROOP] = 0.8f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.03f;
			p[PARAM_LEAVES_PER_INTERNODE] = 160;
			p[PARAM_LEAF_YEARS] = 6;
			bark_color = Color(0.33f, 0.27f, 0.23f);
			leaf_color = Color(0.07f, 0.17f, 0.08f);
			p[PARAM_CROWN_CONE] = 0.3f;
			p[PARAM_SHADOW_STRENGTH] = 0.022f;
			break;
		case PRESET_HAZEL: // Corylus avellana: multi-stemmed, basitonic, sprouts from the base.
			p[PARAM_YEARS] = 15;
			p[PARAM_INTERNODE_LENGTH] = 0.25f;
			p[PARAM_APICAL_DOMINANCE] = 0.52f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.48f;
			p[PARAM_MATURITY] = 10;
			p[PARAM_BRANCH_ANGLE] = 40;
			p[PARAM_DIVERGENCE] = 180;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_STEMS] = 6;
			p[PARAM_STEM_SPREAD] = 25;
			p[PARAM_BASAL_SPROUTS] = 0.3f;
			p[PARAM_ROOT_FLARE] = 0.2f;
			p[PARAM_LEAF_SIZE] = 0.1f;
			p[PARAM_LEAVES_PER_INTERNODE] = 28;
			bark_color = Color(0.36f, 0.27f, 0.2f);
			leaf_color = Color(0.17f, 0.3f, 0.08f);
			break;
		case PRESET_HAWTHORN: // Crataegus monogyna: small, dense, twiggy, crooked.
			p[PARAM_YEARS] = 28;
			p[PARAM_INTERNODE_LENGTH] = 0.2f;
			p[PARAM_APICAL_DOMINANCE] = 0.53f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.48f;
			p[PARAM_MATURITY] = 12;
			p[PARAM_BRANCH_ANGLE] = 55;
			p[PARAM_DIRECTION_NOISE] = 18;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_SHED_RATIO] = 0.033f;
			p[PARAM_STEMS] = 2;
			p[PARAM_STEM_SPREAD] = 15;
			p[PARAM_LEAF_SIZE] = 0.045f;
			p[PARAM_LEAVES_PER_INTERNODE] = 20;
			bark_color = Color(0.32f, 0.29f, 0.25f);
			leaf_color = Color(0.13f, 0.27f, 0.07f);
			break;
		case PRESET_LIME: // Tilia cordata: dense ovate crown, distichous, shade tolerant.
			p[PARAM_YEARS] = 45;
			p[PARAM_APICAL_DOMINANCE] = 0.58f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.52f;
			p[PARAM_DIVERGENCE] = 180;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.6f;
			p[PARAM_SHED_RATIO] = 0.026f;
			p[PARAM_LEAF_SIZE] = 0.075f;
			p[PARAM_LEAF_WIDTH] = 0.42f;
			p[PARAM_LEAVES_PER_INTERNODE] = 28;
			bark_color = Color(0.3f, 0.28f, 0.25f);
			leaf_color = Color(0.17f, 0.31f, 0.08f);
			break;
		case PRESET_SYCAMORE_MAPLE: // Acer pseudoplatanus: opposite buds, broad dome, big leaves.
			p[PARAM_LEADER_GROWTH] = 0.7f;
			p[PARAM_YEARS] = 42;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.57f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
			p[PARAM_SHED_RATIO] = 0.04f;
			p[PARAM_LEAF_SIZE] = 0.13f;
			p[PARAM_LEAF_WIDTH] = 0.5f;
			p[PARAM_LEAVES_PER_INTERNODE] = 16;
			bark_color = Color(0.4f, 0.37f, 0.33f);
			leaf_color = Color(0.13f, 0.27f, 0.07f);
			break;
		case PRESET_ASH: // Fraxinus excelsior: opposite, open light-demanding crown, upcurved limbs.
			p[PARAM_YEARS] = 42;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.6f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.53f;
			p[PARAM_BRANCH_ANGLE] = 40;
			p[PARAM_GRAVITROPISM] = 0.16f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.7f;
			p[PARAM_SHED_RATIO] = 0.066f;
			p[PARAM_LEAF_SIZE] = 0.07f; // Leaflets of the pinnate leaf.
			p[PARAM_LEAF_WIDTH] = 0.3f;
			p[PARAM_LEAVES_PER_INTERNODE] = 30;
			bark_color = Color(0.45f, 0.44f, 0.4f);
			leaf_color = Color(0.17f, 0.29f, 0.08f);
			break;
		case PRESET_HORSE_CHESTNUT: // Aesculus hippocastanum: opposite, dense dome, palmate leaves.
			p[PARAM_YEARS] = 40;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 50;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
			p[PARAM_SHED_RATIO] = 0.033f;
			p[PARAM_TWIG_DROOP] = 0.4f;
			p[PARAM_LEAF_SIZE] = 0.16f; // Leaflets of the palmate leaf.
			p[PARAM_LEAF_WIDTH] = 0.36f;
			p[PARAM_LEAVES_PER_INTERNODE] = 20;
			bark_color = Color(0.33f, 0.28f, 0.24f);
			leaf_color = Color(0.12f, 0.25f, 0.06f);
			break;
		case PRESET_BLACK_ALDER: // Alnus glutinosa: conical broadleaf of wet ground.
			p[PARAM_YEARS] = 27;
			p[PARAM_APICAL_DOMINANCE] = 0.64f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.58f;
			p[PARAM_APICAL_CONTROL] = 0.85f;
			p[PARAM_BRANCH_ANGLE] = 55;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.8f;
			p[PARAM_SHED_RATIO] = 0.073f;
			p[PARAM_LEAF_SIZE] = 0.07f;
			p[PARAM_LEAF_WIDTH] = 0.5f;
			p[PARAM_LEAVES_PER_INTERNODE] = 26;
			bark_color = Color(0.25f, 0.23f, 0.21f);
			leaf_color = Color(0.1f, 0.22f, 0.06f);
			break;
		case PRESET_WHITE_WILLOW: // Salix alba: forked upright limbs, hanging twigs, narrow leaves.
			p[PARAM_YEARS] = 32;
			p[PARAM_APICAL_DOMINANCE] = 0.53f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.49f;
			p[PARAM_BRANCH_ANGLE] = 32;
			p[PARAM_GRAVITROPISM] = 0.14f;
			p[PARAM_DIRECTION_NOISE] = 10;
			p[PARAM_TWIG_DROOP] = 1.2f;
			p[PARAM_SHED_RATIO] = 0.059f;
			p[PARAM_LEAF_SIZE] = 0.09f;
			p[PARAM_LEAF_WIDTH] = 0.13f;
			p[PARAM_LEAVES_PER_INTERNODE] = 36;
			bark_color = Color(0.36f, 0.33f, 0.28f);
			leaf_color = Color(0.25f, 0.33f, 0.2f);
			break;
		case PRESET_LARCH: // Larix decidua: whorled conifer, deciduous needle tufts, hanging twigs.
			p[PARAM_LEADER_GROWTH] = 0.9f;
			p[PARAM_YEARS] = 38;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 4;
			p[PARAM_APICAL_DOMINANCE] = 0.74f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.62f;
			p[PARAM_APICAL_CONTROL] = 0.72f;
			p[PARAM_BRANCH_ANGLE] = 70;
			p[PARAM_GRAVITROPISM] = 0.04f;
			p[PARAM_TRUNK_GRAVITROPISM] = 1.0f;
			p[PARAM_PHOTOTROPISM] = 0.06f;
			p[PARAM_TWIG_DROOP] = 0.9f;
			p[PARAM_SHED_RATIO] = 0.099f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.03f;
			p[PARAM_LEAVES_PER_INTERNODE] = 110;
			p[PARAM_LEAF_YEARS] = 1;
			bark_color = Color(0.36f, 0.25f, 0.2f);
			leaf_color = Color(0.25f, 0.4f, 0.12f);
			p[PARAM_CROWN_CONE] = 0.35f;
			p[PARAM_SHADOW_STRENGTH] = 0.03f;
			break;
		case PRESET_SILVER_FIR: // Abies alba: very shade tolerant, flat horizontal sprays.
			p[PARAM_LEADER_GROWTH] = 0.7f;
			p[PARAM_YEARS] = 50;
			p[PARAM_INTERNODE_LENGTH] = 0.35f;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 4;
			p[PARAM_APICAL_DOMINANCE] = 0.8f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.74f;
			p[PARAM_APICAL_CONTROL] = 0.62f;
			p[PARAM_BRANCH_ANGLE] = 85;
			p[PARAM_GRAVITROPISM] = 0.03f;
			p[PARAM_TRUNK_GRAVITROPISM] = 1.2f;
			p[PARAM_PHOTOTROPISM] = 0.04f;
			p[PARAM_SHED_RATIO] = 0.033f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.5f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.028f;
			p[PARAM_LEAVES_PER_INTERNODE] = 150;
			p[PARAM_LEAF_YEARS] = 8;
			bark_color = Color(0.5f, 0.5f, 0.48f);
			leaf_color = Color(0.06f, 0.16f, 0.07f);
			p[PARAM_CROWN_CONE] = 0.3f;
			p[PARAM_SHADOW_STRENGTH] = 0.022f;
			break;
		case PRESET_ROWAN: // Sorbus aucuparia: small tree, ascending branches, pinnate leaves.
			p[PARAM_YEARS] = 22;
			p[PARAM_INTERNODE_LENGTH] = 0.25f;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 35;
			p[PARAM_GRAVITROPISM] = 0.15f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
			p[PARAM_SHED_RATIO] = 0.053f;
			p[PARAM_LEAF_SIZE] = 0.05f; // Leaflets.
			p[PARAM_LEAF_WIDTH] = 0.3f;
			p[PARAM_LEAVES_PER_INTERNODE] = 30;
			bark_color = Color(0.42f, 0.4f, 0.37f);
			leaf_color = Color(0.15f, 0.3f, 0.08f);
			break;
		case PRESET_ELDER: // Sambucus nigra: multi-stemmed shrub, opposite, arching.
			p[PARAM_YEARS] = 14;
			p[PARAM_INTERNODE_LENGTH] = 0.3f;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.48f;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.25f;
			p[PARAM_TWIG_DROOP] = 0.5f;
			p[PARAM_STEMS] = 5;
			p[PARAM_STEM_SPREAD] = 30;
			p[PARAM_BASAL_SPROUTS] = 0.35f;
			p[PARAM_ROOT_FLARE] = 0.2f;
			p[PARAM_LEAF_SIZE] = 0.08f;
			p[PARAM_LEAF_WIDTH] = 0.35f;
			p[PARAM_LEAVES_PER_INTERNODE] = 24;
			bark_color = Color(0.4f, 0.36f, 0.3f);
			leaf_color = Color(0.13f, 0.27f, 0.07f);
			break;
		case PRESET_HOLLY: // Ilex aquifolium: evergreen, dense pyramid leafy to the ground, deep shade tolerance.
			p[PARAM_YEARS] = 40;
			p[PARAM_INTERNODE_LENGTH] = 0.18f;
			p[PARAM_APICAL_DOMINANCE] = 0.62f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.58f;
			p[PARAM_APICAL_CONTROL] = 0.85f;
			p[PARAM_BRANCH_ANGLE] = 55;
			p[PARAM_DIRECTION_NOISE] = 10;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.8f;
			p[PARAM_SHED_RATIO] = 0.015f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_LEAF_SIZE] = 0.065f;
			p[PARAM_LEAF_WIDTH] = 0.5f;
			p[PARAM_LEAVES_PER_INTERNODE] = 14;
			p[PARAM_LEAF_YEARS] = 3; // Leaves live about three years.
			bark_color = Color(0.45f, 0.45f, 0.42f);
			leaf_color = Color(0.05f, 0.15f, 0.05f);
			break;
		case PRESET_YEW: // Taxus baccata: evergreen, broad dense crown, flat needles, often several stems.
			p[PARAM_YEARS] = 60;
			p[PARAM_INTERNODE_LENGTH] = 0.15f;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.52f;
			p[PARAM_MATURITY] = 30;
			p[PARAM_BRANCH_ANGLE] = 65;
			p[PARAM_DIRECTION_NOISE] = 12;
			p[PARAM_GRAVITROPISM] = 0.02f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.5f;
			p[PARAM_SHED_RATIO] = 0.01f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_STEMS] = 3;
			p[PARAM_STEM_SPREAD] = 12;
			p[PARAM_ROOT_FLARE] = 0.9f;
			p[PARAM_TWIG_DROOP] = 0.4f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.025f;
			p[PARAM_LEAVES_PER_INTERNODE] = 90;
			p[PARAM_LEAF_YEARS] = 6;
			bark_color = Color(0.45f, 0.26f, 0.18f);
			leaf_color = Color(0.04f, 0.12f, 0.05f);
			break;
		case PRESET_BOX: // Buxus sempervirens: evergreen shrub, small opposite leaves, very dense.
			p[PARAM_YEARS] = 25;
			p[PARAM_INTERNODE_LENGTH] = 0.08f;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.52f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 40;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_SHED_RATIO] = 0.01f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_STEMS] = 4;
			p[PARAM_STEM_SPREAD] = 25;
			p[PARAM_BASAL_SPROUTS] = 0.2f;
			p[PARAM_ROOT_FLARE] = 0.2f;
			p[PARAM_LEAF_SIZE] = 0.022f;
			p[PARAM_LEAF_WIDTH] = 0.55f;
			p[PARAM_LEAVES_PER_INTERNODE] = 12;
			p[PARAM_LEAF_YEARS] = 3;
			bark_color = Color(0.5f, 0.45f, 0.36f);
			leaf_color = Color(0.07f, 0.17f, 0.05f);
			break;
		case PRESET_MAX:
			break;
	}
	emit_changed();
	notify_property_list_changed();
}

void TreeGenerator::_set_north_american(Preset p_preset) {
	float *p = params;
	switch (p_preset) {
		case PRESET_SUGAR_MAPLE: // Acer saccharum: tall, dense oval crown, very shade tolerant.
			p[PARAM_YEARS] = 50;
			p[PARAM_APICAL_DOMINANCE] = 0.6f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.53f;
			p[PARAM_BRANCH_ANGLE] = 40;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.6f;
			p[PARAM_SHED_RATIO] = 0.03f;
			p[PARAM_LEAF_SIZE] = 0.12f;
			p[PARAM_LEAF_WIDTH] = 0.55f;
			bark_color = Color(0.38f, 0.35f, 0.31f);
			leaf_color = Color(0.12f, 0.27f, 0.06f);
			break;
		case PRESET_RED_MAPLE: // Acer rubrum: smaller, irregular oval crown, red twigs, wet to dry ground.
			p[PARAM_YEARS] = 38;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.51f;
			p[PARAM_BRANCH_ANGLE] = 38;
			p[PARAM_DIRECTION_NOISE] = 10;
			p[PARAM_LEAF_SIZE] = 0.09f;
			p[PARAM_LEAF_WIDTH] = 0.5f;
			p[PARAM_LEAVES_PER_INTERNODE] = 20;
			bark_color = Color(0.46f, 0.44f, 0.41f);
			leaf_color = Color(0.13f, 0.26f, 0.08f);
			break;
		case PRESET_RED_OAK: // Quercus rubra: straighter and more upright than pedunculate oak, fast.
			p[PARAM_YEARS] = 45;
			p[PARAM_INTERNODE_LENGTH] = 0.5f;
			p[PARAM_APICAL_DOMINANCE] = 0.6f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.51f;
			p[PARAM_BRANCH_ANGLE] = 42;
			p[PARAM_DIRECTION_NOISE] = 9;
			p[PARAM_GRAVITROPISM] = 0.1f;
			p[PARAM_LEAF_SIZE] = 0.16f;
			p[PARAM_LEAF_WIDTH] = 0.42f;
			p[PARAM_LEAVES_PER_INTERNODE] = 26;
			bark_color = Color(0.3f, 0.29f, 0.27f);
			leaf_color = Color(0.11f, 0.24f, 0.06f);
			break;
		case PRESET_WHITE_OAK: // Quercus alba: open-grown, a short bole and a wide-spreading crown.
			p[PARAM_YEARS] = 50;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.47f;
			p[PARAM_BRANCH_ANGLE] = 55;
			p[PARAM_GRAVITROPISM] = 0.02f;
			p[PARAM_LEAF_SIZE] = 0.15f;
			p[PARAM_LEAF_WIDTH] = 0.45f;
			p[PARAM_LEAVES_PER_INTERNODE] = 26;
			bark_color = Color(0.5f, 0.48f, 0.44f);
			leaf_color = Color(0.13f, 0.26f, 0.08f);
			break;
		case PRESET_WHITE_PINE: // Pinus strobus: tall, level whorls in tiers, long soft needles.
			p[PARAM_LEADER_GROWTH] = 0.75f;
			p[PARAM_YEARS] = 50;
			p[PARAM_INTERNODE_LENGTH] = 0.45f;
			p[PARAM_APICAL_DOMINANCE] = 0.78f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.6f;
			p[PARAM_BRANCH_ANGLE] = 80;
			p[PARAM_GRAVITROPISM] = 0.04f;
			p[PARAM_TRUNK_GRAVITROPISM] = 1.0f;
			p[PARAM_SHED_RATIO] = 0.06f;
			p[PARAM_LEAF_SIZE] = 0.1f;
			p[PARAM_LEAVES_PER_INTERNODE] = 100;
			p[PARAM_LEAF_YEARS] = 2;
			p[PARAM_TWIG_DROOP] = 0.3f;
			p[PARAM_CROWN_CONE] = 0.45f;
			bark_color = Color(0.3f, 0.28f, 0.26f);
			leaf_color = Color(0.1f, 0.2f, 0.14f);
			break;
		case PRESET_HEMLOCK: // Tsuga canadensis: nodding leader, fine drooping sprays, deepest shade.
			p[PARAM_LEADER_GROWTH] = 0.6f;
			p[PARAM_YEARS] = 55;
			p[PARAM_INTERNODE_LENGTH] = 0.3f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.9f;
			p[PARAM_BRANCH_ANGLE] = 75;
			p[PARAM_GRAVITROPISM] = -0.05f;
			p[PARAM_TWIG_DROOP] = 0.9f;
			p[PARAM_LEAF_SIZE] = 0.016f;
			p[PARAM_LEAVES_PER_INTERNODE] = 170;
			p[PARAM_LEAF_YEARS] = 4;
			p[PARAM_CROWN_CONE] = 0.35f;
			bark_color = Color(0.36f, 0.28f, 0.24f);
			leaf_color = Color(0.06f, 0.15f, 0.07f);
			break;
		case PRESET_AMERICAN_BEECH: // Fagus grandifolia: like European beech, rounder crown, root suckers.
			p[PARAM_YEARS] = 48;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 48;
			p[PARAM_LEAF_SIZE] = 0.1f;
			p[PARAM_LEAVES_PER_INTERNODE] = 30;
			bark_color = Color(0.52f, 0.52f, 0.5f);
			break;
		case PRESET_PAPER_BIRCH: // Betula papyrifera: chalk-white peeling bark, twigs not pendulous.
			p[PARAM_YEARS] = 30;
			p[PARAM_TWIG_DROOP] = 0.35f;
			p[PARAM_BRANCH_ANGLE] = 42;
			p[PARAM_LEAF_SIZE] = 0.08f;
			p[PARAM_LEAVES_PER_INTERNODE] = 28;
			bark_color = Color(0.85f, 0.84f, 0.8f);
			leaf_color = Color(0.18f, 0.32f, 0.08f);
			break;
		case PRESET_YELLOW_BIRCH: // Betula alleghaniensis: bronze peeling bark, broad crown, long-lived.
			p[PARAM_YEARS] = 45;
			p[PARAM_LATERAL_EXTENSION] = 0.8f;
			p[PARAM_TWIG_DROOP] = 0.2f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_GRAVITROPISM] = 0.06f;
			p[PARAM_LEAF_SIZE] = 0.09f;
			p[PARAM_LEAVES_PER_INTERNODE] = 28;
			bark_color = Color(0.62f, 0.52f, 0.38f);
			leaf_color = Color(0.15f, 0.29f, 0.07f);
			break;
		case PRESET_WITCH_HAZEL: // Hamamelis virginiana: understorey shrub, spreading zigzag stems.
			p[PARAM_YEARS] = 18;
			p[PARAM_STEMS] = 4;
			p[PARAM_STEM_SPREAD] = 35;
			p[PARAM_BRANCH_ANGLE] = 50;
			p[PARAM_DIRECTION_NOISE] = 16;
			p[PARAM_LEAF_SIZE] = 0.11f;
			p[PARAM_LEAF_WIDTH] = 0.6f;
			bark_color = Color(0.44f, 0.38f, 0.32f);
			break;
		case PRESET_MOUNTAIN_LAUREL: // Kalmia latifolia: evergreen thicket shrub of acid oak woods.
			p[PARAM_YEARS] = 20;
			p[PARAM_INTERNODE_LENGTH] = 0.12f;
			phyllotaxis = PHYLLOTAXIS_ALTERNATE;
			p[PARAM_DIRECTION_NOISE] = 18;
			p[PARAM_STEMS] = 5;
			p[PARAM_STEM_SPREAD] = 35;
			p[PARAM_LEAF_SIZE] = 0.08f;
			p[PARAM_LEAF_WIDTH] = 0.3f;
			p[PARAM_LEAVES_PER_INTERNODE] = 10;
			p[PARAM_LEAF_YEARS] = 2;
			bark_color = Color(0.4f, 0.3f, 0.24f);
			leaf_color = Color(0.08f, 0.2f, 0.07f);
			break;
		default:
			break;
	}
}

void TreeGenerator::set_param(Param p_param, float p_value) {
	ERR_FAIL_INDEX(int(p_param), int(PARAM_MAX));
	params[p_param] = p_value;
	emit_changed();
}

float TreeGenerator::get_param(Param p_param) const {
	ERR_FAIL_INDEX_V(int(p_param), int(PARAM_MAX), 0.0f);
	return params[p_param];
}

Vector3 TreeGenerator::_random_unit() {
	const float z = _rand(-1.0f, 1.0f);
	const float a = _rand(0.0f, Math::TAU);
	const float r = Math::sqrt(MAX(0.0f, 1.0f - z * z));
	return Vector3(r * Math::cos(a), z, r * Math::sin(a));
}

// Apical control fades with age in decurrent trees (oak), stays in excurrent ones (spruce).
float TreeGenerator::_lambda(int p_year) const {
	const float t = CLAMP(float(p_year) / MAX(1.0f, params[PARAM_MATURITY]), 0.0f, 1.0f);
	return Math::lerp(params[PARAM_APICAL_DOMINANCE], params[PARAM_APICAL_DOMINANCE_OLD], t);
}

float TreeGenerator::_internode_length(int p_year) const {
	return params[PARAM_INTERNODE_LENGTH] * Math::pow(params[PARAM_INTERNODE_LENGTH_AGE], float(p_year));
}

int TreeGenerator::_add_node(int p_parent, const Vector3 &p_start, const Vector3 &p_dir, float p_length, int p_order, int p_year, bool p_main) {
	Node n;
	n.start = p_start;
	n.dir = p_dir;
	n.length = p_length;
	n.parent = p_parent;
	n.order = p_order;
	n.born = p_year;
	n.radius = params[PARAM_TIP_RADIUS];
	n.leafy = true;
	const int idx = int(nodes.size());
	nodes.push_back(n);
	if (p_parent >= 0) {
		Node &par = nodes[p_parent];
		if (p_main) {
			par.main = idx;
		} else {
			nodes[idx].next_sibling = par.first_lateral;
			par.first_lateral = idx;
		}
	}
	return idx;
}

Vector3 TreeGenerator::_bud_direction(const Vector3 &p_axis, float p_roll) {
	Vector3 ref = Math::abs(p_axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	Vector3 side = p_axis.cross(ref).normalized().rotated(p_axis, p_roll);
	const float angle = Math::deg_to_rad(params[PARAM_BRANCH_ANGLE] + _rand(-1.0f, 1.0f) * params[PARAM_BRANCH_ANGLE_VARIANCE]);
	return p_axis.rotated(side.cross(p_axis).normalized(), angle).normalized();
}

void TreeGenerator::_add_lateral_buds(int p_node, float &r_roll, bool p_end_of_shoot) {
	const Vector3 axis = nodes[p_node].dir;
	int count = 0;
	float step = 0.0f;
	switch (phyllotaxis) {
		case PHYLLOTAXIS_ALTERNATE:
			count = 1;
			break;
		case PHYLLOTAXIS_OPPOSITE:
			count = 2;
			step = Math::PI;
			break;
		case PHYLLOTAXIS_WHORLED:
			if (nodes[p_node].order == 0) {
				// The leader: a whorl at the end of each year's shoot.
				if (!p_end_of_shoot) {
					return;
				}
				count = MAX(1, int(params[PARAM_WHORL_BUDS]));
				step = Math::TAU / count;
			} else if (p_end_of_shoot) {
				// Branches of pines and spruces fork flat: a bud to each side at the shoot end.
				count = 2;
				step = Math::PI;
				Vector3 side = axis.cross(Vector3(0, 1, 0));
				if (side.length_squared() > 1e-6f) {
					Vector3 ref = Math::abs(axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
					const Vector3 base_side = axis.cross(ref).normalized();
					r_roll = base_side.signed_angle_to(side.normalized(), axis) + _rand(-0.3f, 0.3f);
				}
			} else {
				// Interwhorl buds along branch shoots, alternating sides.
				count = 1;
				Vector3 side = axis.cross(Vector3(0, 1, 0));
				if (side.length_squared() > 1e-6f) {
					Vector3 ref = Math::abs(axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
					const Vector3 base_side = axis.cross(ref).normalized();
					r_roll = base_side.signed_angle_to(side.normalized(), axis) + (rng.randf() < 0.5f ? 0.0f : Math::PI);
				}
			}
			break;
	}
	for (int i = 0; i < count; i++) {
		Bud b;
		b.node = p_node;
		const float jitter = (phyllotaxis == PHYLLOTAXIS_WHORLED && nodes[p_node].order == 0) ? _rand(-0.25f, 0.25f) * step : 0.0f;
		b.dir = _bud_direction(axis, r_roll + step * i + jitter);
		buds.push_back(b);
	}
	r_roll += Math::deg_to_rad(phyllotaxis == PHYLLOTAXIS_OPPOSITE ? 90.0f : params[PARAM_DIVERGENCE]);
}

Vector3i TreeGenerator::_cell_of(const Vector3 &p_pos) const {
	const Vector3 c = (p_pos - grid_origin) / cell;
	return Vector3i(int(Math::floor(c.x)), int(Math::floor(c.y)), int(Math::floor(c.z)));
}

float TreeGenerator::_shadow_at(const Vector3 &p_pos) const {
	const Vector3i c = _cell_of(p_pos);
	if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= grid_size.x || c.y >= grid_size.y || c.z >= grid_size.z) {
		return 0.0f;
	}
	return shadow[_cell_index(c)];
}

// The direction of most light around a point (the shadow gradient over the 26 neighbours),
// restricted to the half space ahead of the shoot (Palubicki's perception cone).
Vector3 TreeGenerator::_best_lit_direction(const Vector3 &p_pos, const Vector3 &p_dir) const {
	const float s0 = _shadow_at(p_pos);
	Vector3 g;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (dx == 0 && dy == 0 && dz == 0) {
					continue;
				}
				const Vector3 o = Vector3(dx, dy, dz).normalized();
				if (o.dot(p_dir) < -0.1f) {
					continue;
				}
				const float s = _shadow_at(p_pos + Vector3(dx, dy, dz) * cell);
				g += o * (s0 - s);
			}
		}
	}
	return g.length_squared() > 1e-8f ? g.normalized() : Vector3();
}

// Palubicki 2009, shadow propagation: every leafy shoot end shades a pyramid of cells below it,
// a * b^-q at depth q, spreading by one cell per level. A closed stand around the tree (forest
// growth) adds side shade below the neighbours' crown top, which rises with the tree.
void TreeGenerator::_build_shadow_grid(int p_year) {
	cell = MAX(0.05f, params[PARAM_INTERNODE_LENGTH]);
	const int depth = CLAMP(int(params[PARAM_SHADOW_DEPTH]), 1, 64);
	AABB box(nodes[0].start, Vector3());
	float top = 0.0f;
	for (const Node &n : nodes) {
		if (n.removed) {
			continue;
		}
		box.expand_to(n.end());
		top = MAX(top, n.end().y);
	}
	// Buds only look one cell around them; shade below the lowest leaves is never read.
	const float margin = 2.0f * cell;
	box.position -= Vector3(margin, margin, margin);
	box.size += Vector3(margin * 2.0f, margin * 2.0f, margin * 2.0f);
	grid_origin = box.position;
	grid_size = Vector3i(int(Math::ceil(box.size.x / cell)) + 1, int(Math::ceil(box.size.y / cell)) + 1, int(Math::ceil(box.size.z / cell)) + 1);
	shadow.resize(grid_size.x * grid_size.y * grid_size.z);
	memset(shadow.ptr(), 0, shadow.size() * sizeof(float));

	const float a = params[PARAM_SHADOW_STRENGTH];
	const float b = MAX(1.01f, params[PARAM_SHADOW_FALLOFF]);
	// The pyramid of every source at depth q below it covers a (2q+1)^2 square, so the shadow in
	// a cell is sum over q of a b^-q times the sources in the square of radius q one level q up.
	// Summed-area tables per layer make each square O(1): cost cells x depth, any source count.
	const int sx = grid_size.x + 1;
	const int sz = grid_size.z + 1;
	LocalVector<float> sat;
	sat.resize(size_t(grid_size.y) * sx * sz);
	memset(sat.ptr(), 0, sat.size() * sizeof(float));
	for (const Node &n : nodes) {
		if (n.removed || n.dead || !n.leafy || n.length <= 0.0f) {
			continue;
		}
		const Vector3i c = _cell_of(n.end());
		if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= grid_size.x || c.y >= grid_size.y || c.z >= grid_size.z) {
			continue;
		}
		sat[(size_t(c.y) * sz + c.z + 1) * sx + c.x + 1] += 1.0f;
	}
	for (int y = 0; y < grid_size.y; y++) {
		float *l = sat.ptr() + size_t(y) * sx * sz;
		for (int z = 1; z < sz; z++) {
			for (int x = 1; x < sx; x++) {
				l[z * sx + x] += l[(z - 1) * sx + x] + l[z * sx + x - 1] - l[(z - 1) * sx + x - 1];
			}
		}
	}
	float fall[65];
	for (int q = 0; q <= depth; q++) {
		fall[q] = a * Math::pow(b, -float(q));
	}
	for (int y = 0; y < grid_size.y; y++) {
		for (int q = 0; q <= depth; q++) {
			const int ly = y + q;
			if (ly >= grid_size.y) {
				break;
			}
			const float *l = sat.ptr() + size_t(ly) * sx * sz;
			if (l[sz * sx - 1] <= 0.0f) {
				continue; // An empty layer.
			}
			const float w = fall[q];
			for (int z = 0; z < grid_size.z; z++) {
				const int z0 = MAX(0, z - q);
				const int z1 = MIN(grid_size.z, z + q + 1);
				float *row = shadow.ptr() + (y * grid_size.z + z) * grid_size.x;
				for (int x = 0; x < grid_size.x; x++) {
					const int x0 = MAX(0, x - q);
					const int x1 = MIN(grid_size.x, x + q + 1);
					const float sum = l[z1 * sx + x1] - l[z0 * sx + x1] - l[z1 * sx + x0] + l[z0 * sx + x0];
					row[x] += w * sum;
				}
			}
		}
	}
	if (forest_shade > 0.0f && top > 0.0f) {
		// A stand starts dense and thins as it grows: spacing stays near a quarter of the top
		// height (the Hart-Becking spacing index of 20-30% that forest stands keep through their
		// life; Wilson 1946), up to the mature spacing. Grown at the final spacing from the first
		// year, a stand tree kept the low limbs of an open sapling and grew broom-shaped; closing
		// the canopy early shades them out and leaves the clear bole of real stand-grown trees.
		const float r0 = 0.5f * CLAMP(0.25f * top, 2.0f * cell, forest_spacing);
		const float r1 = r0 + 2.0f * cell;
		const Vector3 base = nodes[0].start;
		for (int y = 0; y < grid_size.y; y++) {
			const float wy = grid_origin.y + (y + 0.5f) * cell;
			if (wy >= top + cell) {
				continue;
			}
			// Past half the spacing the neighbours' crowns fill the space at every height up to
			// their tops (in Palubicki 2009 trees grown in one grid meet at a shared boundary, the
			// crown shyness of real stands), so the crown keeps to its own cell; below the canopy
			// band the closed canopy overhead (theirs and this tree's own) leaves the whole
			// understorey dim, near the stem as well, so lower branches die and the stem self-prunes.
			const float below = CLAMP((0.7f * top - wy) / MAX(0.2f * top, cell), 0.0f, 1.0f);
			const float sy = forest_shade * 1.5f;
			const float under = forest_shade * 2.0f * below;
			for (int z = 0; z < grid_size.z; z++) {
				for (int x = 0; x < grid_size.x; x++) {
					const float dx = grid_origin.x + (x + 0.5f) * cell - base.x;
					const float dz = grid_origin.z + (z + 0.5f) * cell - base.z;
					const float r = Math::sqrt(dx * dx + dz * dz);
					const float t = CLAMP((r - r0) / (r1 - r0), 0.0f, 1.0f);
					shadow[_cell_index(Vector3i(x, y, z))] += under + sy * t * t * (3.0f - 2.0f * t);
				}
			}
		}
	}
	(void)p_year;
}

void TreeGenerator::_grow_year(int p_year) {
	_build_shadow_grid(p_year);
	const float a = params[PARAM_SHADOW_STRENGTH];
	const int node_count = int(nodes.size());

	// Light reaching each bud; buds in deep shade die (Stava's light-dependent bud fate).
	for (Bud &b : buds) {
		if (!b.alive) {
			continue;
		}
		const Node &n = nodes[b.node];
		if (n.removed || n.dead) {
			b.alive = false;
			continue;
		}
		const Vector3 p = n.end() + b.dir * (cell * 0.5f);
		b.light = CLAMP(1.0f + a - _shadow_at(p), 0.0f, 1.0f);
		// Branch buds lying on the ground die (contact, snow press, browsing), so lower limbs do not
		// keep spreading along the ground; stems and basal sprouts are exempt.
		if (b.node != 0 && n.order > 0 && p.y < 0.3f) {
			b.alive = false;
			continue;
		}
		if (b.light < params[PARAM_BUD_LIGHT_MIN]) {
			b.alive = false;
		}
	}

	// Basipetal pass: light gathered by each subtree, kept apart for the axis and its laterals.
	for (Node &n : nodes) {
		n.light_main = 0.0f;
		n.light_lateral = 0.0f;
		n.vigor = 0.0f;
		n.count = 1;
	}
	LocalVector<int> bud_head;
	LocalVector<int> bud_next;
	bud_head.resize(node_count);
	bud_next.resize(buds.size());
	for (int i = 0; i < node_count; i++) {
		bud_head[i] = -1;
	}
	for (int i = 0; i < int(buds.size()); i++) {
		bud_next[i] = -1;
		const Bud &b = buds[i];
		if (!b.alive) {
			continue;
		}
		bud_next[i] = bud_head[b.node];
		bud_head[b.node] = i;
		if (b.terminal) {
			nodes[b.node].light_main += b.light;
		} else {
			nodes[b.node].light_lateral += b.light;
		}
	}
	for (int i = node_count - 1; i > 0; i--) {
		const Node &n = nodes[i];
		if (n.removed || n.dead) {
			continue;
		}
		Node &par = nodes[n.parent];
		if (par.main == i) {
			par.light_main += n.light_main + n.light_lateral;
		} else {
			par.light_lateral += n.light_main + n.light_lateral;
		}
		par.count += n.count;
	}

	// Acropetal pass: Borchert-Honda allocation of vigor between the axis and its laterals.
	nodes[0].vigor = params[PARAM_VIGOR] * (nodes[0].light_main + nodes[0].light_lateral);
	const float lambda = _lambda(p_year);
	for (int i = 0; i < node_count; i++) {
		Node &n = nodes[i];
		if (n.removed || n.dead || n.vigor <= 0.0f) {
			continue;
		}
		const float qm = n.light_main;
		const float ql = n.light_lateral;
		const float denom = lambda * qm + (1.0f - lambda) * ql;
		if (denom <= 0.0f) {
			continue;
		}
		const float vm = n.vigor * lambda * qm / denom;
		const float vl = n.vigor - vm;
		for (int bi = bud_head[i]; bi >= 0; bi = bud_next[bi]) {
			Bud &b = buds[bi];
			b.vigor = b.terminal ? vm : (ql > 0.0f ? vl * b.light / ql : 0.0f);
		}
		if (n.main >= 0 && !nodes[n.main].removed && !nodes[n.main].dead) {
			nodes[n.main].vigor = vm;
		}
		for (int c = n.first_lateral; c >= 0; c = nodes[c].next_sibling) {
			Node &ch = nodes[c];
			if (ch.removed || ch.dead || ql <= 0.0f) {
				continue;
			}
			ch.vigor = vl * (ch.light_main + ch.light_lateral) / ql;
		}
	}

	// Shoot growth: a bud with vigor v grows floor(v) internodes, bending toward the light and
	// away from (or toward) gravity, with lateral buds set by the phyllotaxis.
	const int bud_count = int(buds.size());
	const float base_len = _internode_length(p_year);
	const float noise = Math::deg_to_rad(params[PARAM_DIRECTION_NOISE]);
	// Crown cone of excurrent trees: a branch reaches out at most crown_cone times its depth below
	// the leader's tip (Kantola and Makela 2004: Norway spruce branch length grows with depth in
	// the crown, the whole crown keeping a near-constant shape). Shoots that would leave the cone
	// wait for the leader to rise, so young branches near the top grow at full rate but stay short.
	const float cone = params[PARAM_CROWN_CONE];
	float top = 0.0f;
	const Vector3 root = nodes[0].start;
	if (cone > 0.0f) {
		for (const Node &n : nodes) {
			if (n.order == 0 && !n.removed) {
				top = MAX(top, n.end().y);
			}
		}
	}
	for (int bi = 0; bi < bud_count; bi++) {
		if (!buds[bi].alive) {
			continue;
		}
		// Apical control (Stava 2014): shoots on branches grow less the further their order is from
		// the leader, which keeps conifers excurrent.
		{
			const Node &on = nodes[buds[bi].node];
			const int ord = (buds[bi].terminal || buds[bi].node == 0) ? on.order : on.order + 1;
			buds[bi].vigor *= Math::pow(params[PARAM_APICAL_CONTROL], float(ord));
		}
		// Stochastic rounding: a bud with vigor 0.4 grows one internode four years in ten, so weak
		// buds are slow rather than starved outright.
		int count = MIN(int(Math::floor(buds[bi].vigor + rng.randf())), int(params[PARAM_MAX_SHOOT]));
		bool waiting = false;
		if (cone > 0.0f && count > 0) {
			const Node &on = nodes[buds[bi].node];
			const bool at_root = buds[bi].node == 0;
			if (!at_root && (!buds[bi].terminal || on.order > 0)) {
				const Vector3 p0 = on.end();
				const float l0 = base_len * CLAMP(buds[bi].vigor / float(count), 0.25f, 1.5f);
				int allowed = 0;
				for (int k = 0; k < count; k++) {
					const Vector3 q = p0 + buds[bi].dir * (l0 * (k + 1));
					const float reach = Vector2(q.x - root.x, q.z - root.z).length();
					// One internode of slack, so a new whorl at the tip can start its first shoot.
					if (reach > cone * MAX(top - q.y, 0.0f) + l0) {
						break;
					}
					allowed++;
				}
				waiting = allowed == 0;
				count = allowed;
			}
		}
		if (count < 1) {
			// A bud held back by the cone is not starved: it waits for the leader without ageing.
			if (!waiting) {
				buds[bi].idle++;
			}
			// Terminal buds that cannot extend stay alive as spurs (they die only of shade); dormant
			// lateral buds die after a few years.
			if (!buds[bi].terminal && buds[bi].idle > int(params[PARAM_BUD_IDLE_YEARS])) {
				buds[bi].alive = false;
			}
			continue;
		}
		if (int(nodes.size()) + count >= MAX_NODES) {
			break;
		}
		const Bud bud = buds[bi];
		buds[bi].alive = false;
		// Vigor beyond the internode count lengthens the internodes and a weak bud makes a short
		// shoot (Palubicki: l = v / n), so a dominant leader outgrows its laterals and shaded
		// shoots stay short inside the crown instead of reaching out at full length.
		const Node &from = nodes[bud.node];
		const bool from_root = bud.node == 0;
		const int order = (bud.terminal || from_root) ? from.order : from.order + 1;
		// Lateral extension (Wilson 2000; Kantola and Makela 2004): in excurrent conifers the
		// branches lengthen far more slowly than the leader, which sets the narrow cone. Scaling the
		// vigor for it starved the crown of buds and leaves (vigor comes from collected light) and
		// the tree collapsed to a pole; scaling the shoot length alone keeps every bud and needle
		// and only draws the branches in.
		float len = base_len * CLAMP(bud.vigor / float(count), 0.25f, 1.5f) * Math::pow(params[PARAM_LATERAL_EXTENSION], float(MIN(order, 2)));
		// Height growth has a ceiling (the largest annual leader shoots: about 0.6 m for beech and
		// maple, 0.8-1 m for spruce, larch, birch and white pine; Burns and Honkala 1990; Schutt et
		// al., Enzyklopadie der Holzgewachse). Without it a stand-grown tree, its sides shaded,
		// poured all vigor into the leader and reached 36-46 m in 50 years with a crown a few
		// metres deep.
		if (params[PARAM_LEADER_GROWTH] > 0.0f) { // Every shoot: a branch turned up to replace the leader obeys it too.
			len = MIN(len, params[PARAM_LEADER_GROWTH] / float(count));
		}
		const float grav = order == 0 ? params[PARAM_TRUNK_GRAVITROPISM] : params[PARAM_GRAVITROPISM];
		Vector3 pos = from_root ? from.start : from.end();
		Vector3 dir = bud.dir;
		int parent = bud.node;
		float roll = _rand(0.0f, Math::TAU);
		for (int k = 0; k < count; k++) {
			if (k > 0 || !bud.terminal) {
				Vector3 d = dir + _best_lit_direction(pos, dir) * params[PARAM_PHOTOTROPISM] + Vector3(0, grav, 0) * 0.5f;
				d += _random_unit() * Math::sin(noise);
				dir = d.length_squared() > 1e-8f ? d.normalized() : dir;
			} else {
				Vector3 d = dir + _best_lit_direction(pos, dir) * params[PARAM_PHOTOTROPISM] * 0.5f + Vector3(0, grav, 0) * 0.3f + _random_unit() * Math::sin(noise) * 0.5f;
				dir = d.length_squared() > 1e-8f ? d.normalized() : dir;
			}
			const bool main = k > 0 || (bud.terminal && !from_root);
			const int idx = _add_node(parent, pos, dir, len, order, p_year, main);
			const bool last = k == count - 1;
			_add_lateral_buds(idx, roll, last);
			if (last) {
				Bud t;
				t.node = idx;
				t.dir = dir;
				t.terminal = true;
				buds.push_back(t);
			}
			pos = nodes[idx].end();
			parent = idx;
		}
	}

	// Leaves stay on the shoots of the last leaf_years years.
	const int leaf_years = MAX(1, int(params[PARAM_LEAF_YEARS]));
	_compute_radii();
	// Leaves on the last leaf_years of shoots, and wherever a terminal bud is alive: a bud with too
	// little vigor to extend still opens a short shoot (spur) with its leaves (Palubicki: leaves at
	// the buds).
	for (Node &n : nodes) {
		n.leafy = !n.dead && !n.removed && n.length > 0.0f && n.born > p_year - leaf_years;
	}
	for (const Bud &b : buds) {
		if (b.alive && (b.terminal || phyllotaxis != PHYLLOTAXIS_WHORLED)) {
			Node &n = nodes[b.node];
			n.leafy = !n.dead && !n.removed;
		}
	}
	if (p_year >= 2) {
		_shed(p_year);
	}
}

// Palubicki's shedding: a lateral branch whose light per internode stays below the threshold
// costs more than it gains and is dropped. Some shed branches stay on as dead wood (spruces keep
// their lower dead limbs for decades; pines and birches prune themselves clean).
void TreeGenerator::_shed(int p_year) {
	const float ratio = params[PARAM_SHED_RATIO];
	LocalVector<int> stack;
	for (int i = 1; i < int(nodes.size()); i++) {
		Node &n = nodes[i];
		if (n.removed || n.dead || n.parent <= 0 || nodes[n.parent].main == i || n.born > p_year - 2) {
			continue;
		}
		const float light = (n.light_main + n.light_lateral) / float(MAX(1, n.count));
		if (light >= ratio) {
			continue;
		}
		const bool keep = rng.randf() < params[PARAM_DEAD_BRANCH_KEEP];
		shed_count++;
		stack.clear();
		stack.push_back(i);
		while (!stack.is_empty()) {
			const int j = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			Node &m = nodes[j];
			if (keep) {
				m.dead = true;
				m.leafy = false;
			} else {
				m.removed = true;
			}
			if (m.main >= 0) {
				stack.push_back(m.main);
			}
			for (int c = m.first_lateral; c >= 0; c = nodes[c].next_sibling) {
				stack.push_back(c);
			}
		}
	}
}

// Pipe model: a branch carries the sum of its children's pipes, r^n = sum r_i^n.
void TreeGenerator::_compute_radii() {
	const float e = MAX(1.0f, params[PARAM_PIPE_EXPONENT]);
	const float tip = params[PARAM_TIP_RADIUS];
	LocalVector<float> acc;
	acc.resize(nodes.size());
	for (int i = 0; i < int(nodes.size()); i++) {
		acc[i] = 0.0f;
	}
	for (int i = int(nodes.size()) - 1; i >= 0; i--) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		if (!n.dead) {
			float s = acc[i];
			if (n.main < 0 || nodes[n.main].removed) {
				s += Math::pow(tip, e); // The shoot tip.
			}
			n.radius = MAX(n.radius, MAX(tip, Math::pow(s, 1.0f / e)));
		}
		if (n.parent >= 0) {
			acc[n.parent] += Math::pow(n.radius, e);
		}
	}
}

// Branches sag under their own weight and their leaves: Euler-Bernoulli bending per internode,
// angle = moment * length / (E * I), I = pi r^4 / 4, with the moment of the whole subtree about
// the internode's base. Thin shoots of pendulous species droop on top of that.
void TreeGenerator::_bend_under_gravity() {
	const int count = int(nodes.size());
	LocalVector<float> mass;
	LocalVector<Vector3> moment; // Mass-weighted centre, summed.
	mass.resize(count);
	moment.resize(count);
	const float leaf_mass = params[PARAM_NEEDLES] > 0.5f ? 0.0004f : 0.6f * params[PARAM_LEAF_SIZE] * params[PARAM_LEAF_SIZE] * 0.15f;
	for (int i = count - 1; i >= 0; i--) {
		const Node &n = nodes[i];
		mass[i] = 0.0f;
		moment[i] = Vector3();
		if (n.removed) {
			continue;
		}
		const float wood = Math::PI * n.radius * n.radius * n.length * 700.0f;
		const float leaves = n.leafy ? leaf_mass * params[PARAM_LEAVES_PER_INTERNODE] : 0.0f;
		mass[i] += wood + leaves;
		moment[i] += (n.start + n.dir * (n.length * 0.5f)) * (wood + leaves);
	}
	for (int i = count - 1; i > 0; i--) {
		if (nodes[i].removed) {
			continue;
		}
		mass[nodes[i].parent] += mass[i];
		moment[nodes[i].parent] += moment[i];
	}
	const float k = params[PARAM_GRAVITY_BENDING] * 9.81f / 1.0e10f;
	const float droop = params[PARAM_TWIG_DROOP];
	LocalVector<Quaternion> acc;
	acc.resize(count);
	acc[0] = Quaternion();
	const Vector3 down(0, -1, 0);
	for (int i = 1; i < count; i++) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		Quaternion q = acc[n.parent];
		Vector3 axis = n.dir.cross(down);
		if (axis.length_squared() > 1e-6f && n.order > 0) {
			axis.normalize();
			const Vector3 centre = moment[i] / MAX(mass[i], 1e-9f);
			Vector3 lever = centre - n.start;
			lever.y = 0.0f;
			const float inertia = Math::PI * Math::pow(n.radius, 4.0f) * 0.25f;
			float angle = MIN(0.2f, k * mass[i] * lever.length() * n.length / MAX(inertia, 1e-14f));
			if (droop > 0.0f && n.radius < 0.012f) {
				angle += droop * (1.0f - n.radius / 0.012f) * 0.3f;
			}
			// Never bend past hanging straight down.
			angle = MIN(angle, Math::acos(CLAMP(n.dir.dot(down), -1.0f, 1.0f)) * 0.5f);
			q = q * Quaternion(axis, angle);
		}
		acc[i] = q;
	}
	for (int i = 1; i < count; i++) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		n.dir = acc[i].xform(n.dir).normalized();
		const Node &par = nodes[n.parent];
		n.start = n.parent == 0 ? par.start : par.end();
		// Branches resting on the ground run along it instead of through it.
		if (n.start.y + n.dir.y * n.length < 0.08f && n.dir.y < 0.0f) {
			n.dir.y = 0.0f;
			n.dir = n.dir.length_squared() > 1e-6f ? n.dir.normalized() : Vector3(1, 0, 0);
		}
	}
}

void TreeGenerator::_measure() {
	height = 0.0f;
	crown_radius = 0.0f;
	LocalVector<float> leaf_heights;
	LocalVector<float> leaf_reach;
	for (const Node &n : nodes) {
		if (n.removed) {
			continue;
		}
		const Vector3 e = n.end();
		height = MAX(height, e.y);
		if (n.leafy) {
			leaf_reach.push_back(Vector2(e.x, e.z).length());
			leaf_heights.push_back(e.y);
		}
	}
	// Clear bole: the lowest live limb (over 2 cm thick) leaving a stem.
	bole_height = height;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.removed || n.dead || n.order == 0 || n.radius < 0.02f || nodes[n.parent].order != 0 || nodes[n.parent].main == i) {
			continue;
		}
		bole_height = MIN(bole_height, n.start.y);
	}
	crown_base = 0.0f;
	if (!leaf_heights.is_empty()) {
		leaf_heights.sort();
		crown_base = leaf_heights[leaf_heights.size() / 50]; // 2nd percentile of foliage.
		// Crown radius as foresters measure it (the drip line), not the one longest twig: the
		// 95th percentile of foliage reach.
		leaf_reach.sort();
		crown_radius = leaf_reach[MIN(int(leaf_reach.size()) - 1, int(leaf_reach.size()) * 95 / 100)];
	}
}

int TreeGenerator::get_internode_count() const {
	int c = 0;
	for (const Node &n : nodes) {
		c += (!n.removed && n.length > 0.0f) ? 1 : 0;
	}
	return c;
}

int TreeGenerator::get_dead_branch_count() const {
	int c = 0;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		c += (n.dead && !n.removed && !nodes[n.parent].dead) ? 1 : 0;
	}
	return c;
}

// A generalized cylinder along a polyline, framed by parallel transport (Bishop frames).
void TreeGenerator::_tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color) {
	const int rings = int(p_points.size());
	if (rings < 2) {
		return;
	}
	const int base = r_b.verts.size();
	Vector3 t = (p_points[1] - p_points[0]).normalized();
	Vector3 ref = Math::abs(t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	Vector3 nrm = t.cross(ref).normalized();
	float v = p_v0;
	const float circumference = Math::TAU * MAX(p_radii[0], 0.001f);
	for (int r = 0; r < rings; r++) {
		Vector3 t2;
		if (r == 0) {
			t2 = t;
		} else if (r == rings - 1) {
			t2 = (p_points[r] - p_points[r - 1]).normalized();
		} else {
			t2 = (p_points[r + 1] - p_points[r - 1]).normalized();
		}
		// Transport the normal from the previous tangent to this one.
		const Vector3 axis = t.cross(t2);
		if (axis.length_squared() > 1e-10f) {
			nrm = nrm.rotated(axis.normalized(), Math::acos(CLAMP(t.dot(t2), -1.0f, 1.0f)));
		}
		nrm = (nrm - t2 * nrm.dot(t2)).normalized();
		t = t2;
		const Vector3 bin = t.cross(nrm);
		if (r > 0) {
			v += p_points[r].distance_to(p_points[r - 1]) / circumference;
		}
		for (int s = 0; s <= p_sides; s++) {
			const float ang = Math::TAU * s / p_sides;
			const Vector3 dir = nrm * Math::cos(ang) + bin * Math::sin(ang);
			r_b.verts.push_back(p_points[r] + dir * p_radii[r]);
			r_b.normals.push_back(dir);
			r_b.uvs.push_back(Vector2(float(s) / p_sides, v));
			r_b.colors.push_back(p_color);
		}
	}
	for (int r = 0; r < rings - 1; r++) {
		for (int s = 0; s < p_sides; s++) {
			const int a = base + r * (p_sides + 1) + s;
			const int b = a + p_sides + 1;
			// Clockwise front faces (Godot).
			r_b.indices.push_back(a);
			r_b.indices.push_back(a + 1);
			r_b.indices.push_back(b);
			r_b.indices.push_back(a + 1);
			r_b.indices.push_back(b + 1);
			r_b.indices.push_back(b);
		}
	}
}

// A broad leaf: a short petiole, then an ovate blade folded along its midrib, turned to face
// mostly upward (leaves orient to the light).
void TreeGenerator::_add_leaf(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, const Vector3 &p_up, float p_size, const Color &p_color) {
	Vector3 n = p_up;
	Vector3 along = (p_out - n * p_out.dot(n));
	if (along.length_squared() < 1e-6f) {
		along = n.cross(Vector3(1, 0, 0));
	}
	along.normalize();
	const Vector3 side = n.cross(along).normalized();
	const Vector3 start = p_base + p_out * (p_size * 0.15f);
	const int steps = 3;
	const int base = r_b.verts.size();
	for (int i = 0; i <= steps; i++) {
		const float t = float(i) / steps;
		const float w = params[PARAM_LEAF_WIDTH] * p_size * Math::sin(Math::PI * Math::pow(t, 0.75f));
		const float droop = -0.25f * p_size * t * t;
		const Vector3 mid = start + along * (t * p_size) + n * droop;
		const Vector3 fold = n * (0.12f * w);
		r_b.verts.push_back(mid);
		r_b.normals.push_back(n);
		r_b.uvs.push_back(Vector2(0.5f, t));
		r_b.colors.push_back(p_color);
		r_b.verts.push_back(mid + side * w + fold);
		r_b.normals.push_back((n - side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(1.0f, t));
		r_b.colors.push_back(p_color);
		r_b.verts.push_back(mid - side * w + fold);
		r_b.normals.push_back((n + side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(0.0f, t));
		r_b.colors.push_back(p_color);
	}
	for (int i = 0; i < steps; i++) {
		const int m0 = base + i * 3, m1 = m0 + 3;
		r_b.indices.push_back(m0);
		r_b.indices.push_back(m1);
		r_b.indices.push_back(m0 + 1);
		r_b.indices.push_back(m0 + 1);
		r_b.indices.push_back(m1);
		r_b.indices.push_back(m1 + 1);
		r_b.indices.push_back(m0);
		r_b.indices.push_back(m0 + 2);
		r_b.indices.push_back(m1);
		r_b.indices.push_back(m0 + 2);
		r_b.indices.push_back(m1 + 2);
		r_b.indices.push_back(m1);
	}
}

void TreeGenerator::_add_needle(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color) {
	Vector3 side = p_out.cross(Vector3(0, 1, 0));
	if (side.length_squared() < 1e-6f) {
		side = p_out.cross(Vector3(1, 0, 0));
	}
	side = side.normalized() * 0.0018f;
	const Vector3 n = side.cross(p_out).normalized();
	const Vector3 tip = p_base + p_out * p_size;
	const int base = r_b.verts.size();
	r_b.verts.push_back(p_base - side);
	r_b.verts.push_back(p_base + side);
	r_b.verts.push_back(tip);
	for (int i = 0; i < 3; i++) {
		r_b.normals.push_back(n);
		r_b.colors.push_back(p_color);
	}
	r_b.uvs.push_back(Vector2(0, 0));
	r_b.uvs.push_back(Vector2(1, 0));
	r_b.uvs.push_back(Vector2(0.5f, 1));
	r_b.indices.push_back(base);
	r_b.indices.push_back(base + 2);
	r_b.indices.push_back(base + 1);
}

// A needle spray: a quad along the shoot, its texture a bottle brush of needles (Ref: SpeedTree
// fronds; Witcher 4 and UE foliage). Alpha-cut, so the path tracer sees the same needles through
// opacity micromaps where the hardware has them.
void TreeGenerator::_add_spray(Builder &r_b, const Vector3 &p_base, const Vector3 &p_dir, float p_length, float p_half_width, const Vector3 &p_side, const Color &p_color) {
	const Vector3 n = p_dir.cross(p_side).normalized();
	const int base = r_b.verts.size();
	const Vector3 corners[4] = { p_base - p_side * p_half_width, p_base + p_side * p_half_width,
		p_base + p_dir * p_length + p_side * p_half_width, p_base + p_dir * p_length - p_side * p_half_width };
	const Vector2 uv[4] = { Vector2(0, 1), Vector2(1, 1), Vector2(1, 0), Vector2(0, 0) };
	for (int k = 0; k < 4; k++) {
		r_b.verts.push_back(corners[k]);
		r_b.normals.push_back(n);
		r_b.uvs.push_back(uv[k]);
		r_b.colors.push_back(p_color);
	}
	const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int k = 0; k < 6; k++) {
		r_b.indices.push_back(base + idx[k]);
	}
}

// The needle image: a twig up the middle and needles leaving it forward at about 50 degrees on
// both sides. Mipmaps keep the needle coverage of the full image (Castano, "Computing Alpha
// Mipmaps", 2010), so sprays do not fade to bare twigs at distance.
Ref<Texture2D> TreeGenerator::_spray_texture() {
	if (spray_texture.is_valid()) {
		return spray_texture;
	}
	const int w = 128, h = 256;
	Vector<uint8_t> alpha;
	alpha.resize(w * h);
	memset(alpha.ptrw(), 0, alpha.size());
	RandomPCG r;
	r.seed(1234);
	auto line = [&](float x0, float y0, float x1, float y1, float th) {
		const int steps = int(Math::ceil(Math::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) * 2.0f));
		for (int k = 0; k <= steps; k++) {
			const float t = float(k) / MAX(steps, 1);
			const float cx = Math::lerp(x0, x1, t), cy = Math::lerp(y0, y1, t);
			const float rr = th * (1.0f - 0.6f * t);
			for (int yy = int(cy - rr - 1); yy <= int(cy + rr + 1); yy++) {
				for (int xx = int(cx - rr - 1); xx <= int(cx + rr + 1); xx++) {
					if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
						continue;
					}
					const float d = Math::sqrt((xx + 0.5f - cx) * (xx + 0.5f - cx) + (yy + 0.5f - cy) * (yy + 0.5f - cy));
					const float a = CLAMP(rr + 0.5f - d, 0.0f, 1.0f);
					uint8_t &px = alpha.ptrw()[yy * w + xx];
					px = MAX(px, uint8_t(a * 255.0f));
				}
			}
		}
	};
	line(w * 0.5f, h, w * 0.5f, 0.0f, 1.6f); // The twig.
	for (int k = 0; k < 90; k++) {
		const float y = h * (0.02f + 0.96f * (k + r.randf()) / 90.0f);
		const float side = (k & 1) ? 1.0f : -1.0f;
		const float ang = Math::deg_to_rad(40.0f + 25.0f * r.randf());
		const float len = w * 0.5f * (0.75f + 0.25f * r.randf()) / Math::sin(ang);
		line(w * 0.5f, y, w * 0.5f + side * Math::sin(ang) * len * 0.97f, y - Math::cos(ang) * len, 1.3f);
	}
	Ref<Image> img = Image::create_empty(w, h, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			img->set_pixel(x, y, Color(1, 1, 1, alpha[y * w + x] / 255.0f));
		}
	}
	img->generate_mipmaps();
	// Coverage-preserving alpha: scale each mip's alpha so as many texels pass 0.5 as in mip 0.
	auto coverage = [](const Ref<Image> &p_img, int p_mip, float p_scale) {
		int64_t ofs, size;
		int mw, mh;
		p_img->get_mipmap_offset_size_and_dimensions(p_mip, ofs, size, mw, mh);
		const uint8_t *d = p_img->get_data().ptr() + ofs;
		int pass = 0;
		for (int k = 0; k < mw * mh; k++) {
			pass += d[k * 4 + 3] * p_scale >= 127.5f ? 1 : 0;
		}
		return float(pass) / float(mw * mh);
	};
	const float target = coverage(img, 0, 1.0f);
	Vector<uint8_t> data = img->get_data();
	for (int m = 1; m <= img->get_mipmap_count(); m++) {
		float lo = 0.0f, hi = 8.0f;
		for (int it = 0; it < 16; it++) {
			const float mid = 0.5f * (lo + hi);
			(coverage(img, m, mid) < target ? lo : hi) = mid;
		}
		int64_t ofs, size;
		int mw, mh;
		img->get_mipmap_offset_size_and_dimensions(m, ofs, size, mw, mh);
		uint8_t *d = data.ptrw() + ofs;
		for (int k = 0; k < mw * mh; k++) {
			d[k * 4 + 3] = uint8_t(MIN(255.0f, d[k * 4 + 3] * hi));
		}
	}
	img = Image::create_from_data(w, h, true, Image::FORMAT_RGBA8, data);
	spray_texture = ImageTexture::create_from_image(img);
	return spray_texture;
}

// Short shoots. Broadleaves carry much of their foliage on spurs along the branches of the last
// years, not only at the shoot tips: oak, beech and lime short shoots live 5 to 10 years and new
// ones break from older wood up to limb size
// (Kozlowski and Pallardy, "Growth Control in Woody Plants", 1997). The growth model counts only
// the shoot tips as light-gathering foliage; letting spurs shade during growth thinned the crowns
// (each layer of leaves darkened the next), so they are added to the mesh after growth, where the
// final light still reaches them.
void TreeGenerator::_mark_spurs(int p_year) {
	spurs.resize(nodes.size());
	memset(spurs.ptr(), 0, spurs.size());
	if (phyllotaxis == PHYLLOTAXIS_WHORLED || params[PARAM_NEEDLES] > 0.5f) {
		return;
	}
	_build_shadow_grid(p_year);
	// The growth model's light (1 + a - shadow) reaches zero a few leaf layers in, where buds
	// stop extending; spurs live on much less. Read the shadow as optical depth instead (Beer-
	// Lambert, transmittance exp(-shadow)) and keep spurs down to about 10% of open light, the
	// low end of what shade leaves of beech and lime live on (Larcher, "Physiological Plant
	// Ecology", 2003).
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.leafy || n.dead || n.removed || n.length <= 0.0f || n.order == 0 || n.born <= p_year - 30 || n.radius > 0.12f) {
			continue;
		}
		spurs[i] = Math::exp(-_shadow_at(n.end())) > 0.02f ? 1 : 0;
	}
}

Ref<ArrayMesh> TreeGenerator::_build_mesh() {
	Builder bark;
	Builder leaves;
	leaf_count = 0;
	const float flare = params[PARAM_ROOT_FLARE];
	const float trunk_r = nodes[0].main >= 0 ? nodes[nodes[0].main].radius : (nodes[0].first_lateral >= 0 ? nodes[nodes[0].first_lateral].radius : 0.1f);
	LocalVector<Vector3> pts;
	LocalVector<float> rad;
	LocalVector<int> chain;
	for (int s = 1; s < int(nodes.size()); s++) {
		const Node &first = nodes[s];
		if (first.removed || (first.parent > 0 && nodes[first.parent].main == s)) {
			continue;
		}
		chain.clear();
		for (int i = s; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
			chain.push_back(i);
		}
		const float r0 = first.radius;
		const bool smooth = r0 > 0.015f;
		pts.clear();
		rad.clear();
		pts.push_back(first.start);
		rad.push_back(r0);
		for (int c = 0; c < int(chain.size()); c++) {
			const Node &n = nodes[chain[c]];
			const float r_end = c + 1 < int(chain.size()) ? nodes[chain[c + 1]].radius : n.radius * 0.6f;
			if (smooth) {
				// Catmull-Rom midpoint between this internode's ends.
				const Vector3 p0 = c > 0 ? nodes[chain[c - 1]].start : n.start - n.dir * n.length;
				const Vector3 p3 = c + 1 < int(chain.size()) ? nodes[chain[c + 1]].end() : n.end() + n.dir * n.length;
				const Vector3 mid = Vector3(0.5f, 0.5f, 0.5f) * (n.start + n.end()) + (n.start - p0 + n.end() - p3) * 0.0625f;
				pts.push_back(mid);
				rad.push_back((n.radius + r_end) * 0.5f);
			}
			pts.push_back(n.end());
			rad.push_back(r_end);
		}
		const bool stem = first.parent == 0;
		if (stem && flare > 0.0f) {
			for (int i = 0; i < int(pts.size()); i++) {
				const float h = MAX(0.0f, pts[i].y - nodes[0].start.y);
				rad[i] *= 1.0f + flare * Math::exp(-h / MAX(0.05f, trunk_r * 2.5f));
			}
			// Sink the base below the ground line.
			pts[0] -= Vector3(0, 0.15f, 0);
		}
		int sides = CLAMP(int(Math::ceil(Math::TAU * r0 / 0.03f)), 3, 24);
		if (stem) {
			sides = MAX(sides, 10);
		}
		Color col = bark_color;
		if (first.dead) {
			col = col.lerp(Color(0.42f, 0.4f, 0.37f), 0.6f);
		}
		const float tint = 0.92f + 0.16f * float((uint32_t(s) * 2654435761u) >> 24) / 255.0f;
		col = Color(col.r * tint, col.g * tint, col.b * tint);
		{ Vector4i part; bark.begin(part); _tube(bark, pts, rad, sides, 0.0f, col); bark.end(part, r0); }
	}

	const bool needles = params[PARAM_NEEDLES] > 0.5f;
	const int per = MAX(0, int(params[PARAM_LEAVES_PER_INTERNODE]));
	const float size = params[PARAM_LEAF_SIZE];
	RandomPCG lr;
	lr.seed(uint64_t(seed) * 7919u + 13u);
	// Leaf area budget. One internode stands for a whole twig system, so the raw count can put far
	// more leaf area on the tree than it carries: an open oak came out near leaf area index 17.
	// Two real limits thin it. Locally, leaf area density in a broadleaf crown peaks near 1.5 m2 of
	// leaf per m3 (beech and oak canopies, 1-2; Lalic and Mihailovic, J. Appl. Meteorol. 43, 2004),
	// so leaves are thinned first where shoots crowd, mostly the sunlit top shell, and the sparse
	// lower and inner crown keeps what it has. Over the whole crown, temperate broadleaves hold LAI
	// 3-8, about 5-6 (Breda, J. Exp. Bot. 54, 2003; Asner et al., Global Ecol. Biogeogr. 12, 2003).
	// Leaf size stays true; the count and the mesh fall to real numbers.
	LocalVector<float> node_keep;
	node_keep.resize(nodes.size());
	for (float &k : node_keep) {
		k = 1.0f;
	}
	leaf_area_index = 0.0f;
	if (!needles && per > 0 && crown_radius > 0.0f) {
		// Blade area: 2 * width * size^2 * integral of sin(pi t^0.75) over t (0.656).
		const float blade = 2.0f * params[PARAM_LEAF_WIDTH] * size * size * 0.656f;
		const float voxel = 1.0f; // Metres.
		HashMap<Vector3i, float> density;
		LocalVector<float> node_area;
		node_area.resize(nodes.size());
		for (int i = 1; i < int(nodes.size()); i++) {
			const Node &n = nodes[i];
			node_area[i] = 0.0f;
			if ((!n.leafy && !spurs[i]) || n.removed || n.dead) {
				continue;
			}
			const int tw = spurs[i] ? MAX(1, per / 12) : MAX(1, per / 4);
			node_area[i] = float(tw * MAX(1, per / tw)) * blade;
			const Vector3i v = Vector3i((n.end() / voxel).floor());
			density[v] = (density.has(v) ? density[v] : 0.0f) + node_area[i];
		}
		double area = 0.0;
		for (int i = 1; i < int(nodes.size()); i++) {
			if (node_area[i] <= 0.0f) {
				continue;
			}
			const float d = density[Vector3i((nodes[i].end() / voxel).floor())] / (voxel * voxel * voxel);
			node_keep[i] = d > MAX_LEAF_AREA_DENSITY ? MAX_LEAF_AREA_DENSITY / d : 1.0f;
			area += double(node_area[i] * node_keep[i]);
		}
		const float lai = float(area / (Math::PI * crown_radius * crown_radius));
		const float keep = lai > MAX_LEAF_AREA_INDEX ? MAX_LEAF_AREA_INDEX / lai : 1.0f;
		for (float &k : node_keep) {
			k *= keep;
		}
		leaf_area_index = lai * keep;
	}
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if ((!n.leafy && !spurs[i]) || n.removed || n.dead) {
			continue;
		}
		// Thin by twigs and by leaves on the kept twigs in equal parts: dropping whole twigs alone
		// left heavily thinned crowns as isolated clumps with daylight between them.
		const float keep_twig = Math::sqrt(node_keep[i]);
		const float keep_leaf = keep_twig;
		Vector3 ref = Math::abs(n.dir.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
		const Vector3 perp = n.dir.cross(ref).normalized();
		float roll = lr.randf() * Math::TAU;
		if (needles) {
			// The internode stands for a whole year's spray: the shoot itself and short side shoots
			// in the branch plane, each a bottle brush of needles pointing forward.
			Vector3 plane_side = n.dir.cross(Vector3(0, 1, 0));
			plane_side = plane_side.length_squared() > 1e-6f ? plane_side.normalized() : perp;
			const int sides = n.order == 0 ? 0 : 4;
			const int brushes = sides + 1;
			const int per_brush = MAX(4, per / brushes);
			for (int sh = 0; sh < brushes; sh++) {
				Vector3 b0 = n.start;
				Vector3 bdir = n.dir;
				float blen = n.length;
				float br = n.radius;
				if (sh > 0) {
					const float t = (sh - 0.5f) / sides;
					b0 = n.start + n.dir * (n.length * t);
					const float sgn = (sh & 1) ? 1.0f : -1.0f;
					bdir = (plane_side * sgn * 0.8f + n.dir * 0.6f + Vector3(0, -0.15f, 0)).normalized();
					blen = n.length * (0.35f + 0.2f * lr.randf()) * (1.0f - 0.4f * t);
					br = params[PARAM_TIP_RADIUS] * 0.5f;
					pts.clear();
					rad.clear();
					pts.push_back(b0);
					rad.push_back(br);
					pts.push_back(b0 + bdir * blen);
					rad.push_back(br * 0.6f);
					{ Vector4i part; bark.begin(part); _tube(bark, pts, rad, 3, 0.0f, bark_color); bark.end(part, rad[0]); }
				}
				if (needle_sprays) {
					// One brush as two crossed alpha-cut sprays (the needle image is drawn from the
					// same needle angle and length), in the branch plane and across it.
					const float jitter = 0.85f + 0.3f * lr.randf();
					const Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
					Vector3 flat = plane_side - bdir * plane_side.dot(bdir);
					flat = flat.length_squared() > 1e-6f ? flat.normalized() : perp;
					const Vector3 cross = bdir.cross(flat).normalized();
					const float half = size * 0.8f;
					{ Vector4i part; leaves.begin(part); _add_spray(leaves, b0, bdir, blen + size * 0.5f, half, flat, c); leaves.end(part, 0.0f); }
					{ Vector4i part; leaves.begin(part); _add_spray(leaves, b0, bdir, blen + size * 0.5f, half, cross, c); leaves.end(part, 0.0f); }
					leaf_count += per_brush;
					continue;
				}
				for (int k = 0; k < per_brush; k++) {
					const float t = (k + 0.5f) / per_brush;
					roll += 2.39996f;
					const Vector3 around = perp.rotated(n.dir, roll);
					const Vector3 ar = (around - bdir * around.dot(bdir)).normalized();
					const Vector3 base = b0 + bdir * (blen * t) + ar * br;
					const float jitter = 0.85f + 0.3f * lr.randf();
					const Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
					const Vector3 out = (ar * 0.75f + bdir * 0.66f).normalized();
					{ Vector4i part; leaves.begin(part); _add_needle(leaves, base, out, size * (0.8f + 0.4f * lr.randf()), c); leaves.end(part, 0.0f); }
					leaf_count++;
				}
			}
			continue;
		}
		// Broad leaves sit on short side twigs (the year's short shoots), a few leaves each, so a
		// metre-scale internode carries the leaf mass of the real twig system it stands for. Twigs
		// leave in every direction around the shoot, rising or hanging, and some fork once, so the
		// foliage fills the crown volume instead of lying in flat plates; leaf blades are tilted by
		// a spherical-like leaf angle distribution (Campbell 1986: most broadleaf canopies have a
		// mean leaf inclination near 45-60 degrees, not horizontal).
		// Spurs on older wood carry a few short shoots, not a whole year's spray.
		const int twigs = spurs[i] ? MAX(1, per / 12) : MAX(1, per / 4);
		const int per_twig = MAX(1, per / twigs);
		const float twig_r = params[PARAM_TIP_RADIUS] * 0.6f;
		const float droop = params[PARAM_TWIG_DROOP];
		auto add_twig = [&](const Vector3 &p_base, const Vector3 &p_dir, float p_len, float p_r, int p_leaves) {
			pts.clear();
			rad.clear();
			pts.push_back(p_base);
			rad.push_back(p_r);
			const Vector3 mid = p_base + p_dir * (p_len * 0.5f) + Vector3(0, -0.08f - 0.3f * droop, 0) * p_len;
			pts.push_back(mid);
			rad.push_back(p_r * 0.8f);
			const Vector3 tip_dir = (p_dir + Vector3(0, 0.15f - 0.6f * droop, 0)).normalized();
			const Vector3 tip = mid + tip_dir * (p_len * 0.5f);
			pts.push_back(tip);
			rad.push_back(p_r * 0.5f);
			{ Vector4i part; bark.begin(part); _tube(bark, pts, rad, 3, 0.0f, bark_color); bark.end(part, rad[0]); }
			Vector3 side0 = p_dir.cross(Vector3(0, 1, 0));
			side0 = side0.length_squared() > 1e-6f ? side0.normalized() : perp;
			for (int k = 0; k < p_leaves; k++) {
				if (keep_leaf < 1.0f && lr.randf() > keep_leaf) {
					continue;
				}
				const float u = (k + 1.0f) / p_leaves;
				const Vector3 at = u < 0.5f ? p_base.lerp(mid, u * 2.0f) : mid.lerp(tip, u * 2.0f - 1.0f);
				const Vector3 side = side0 * ((k & 1) ? 1.0f : -1.0f);
				const Vector3 out = (side * 0.8f + tip_dir * 0.6f + Vector3(0, 0.1f, 0)).normalized();
				// Leaf normal: azimuth random, inclination with cos uniform over 0-75 degrees.
				const float inc = Math::acos(1.0f - lr.randf() * (1.0f - Math::cos(Math::deg_to_rad(75.0f))));
				const float az = lr.randf() * Math::TAU;
				const Vector3 up = Vector3(Math::sin(inc) * Math::cos(az), Math::cos(inc), Math::sin(inc) * Math::sin(az));
				const float jitter = 0.85f + 0.3f * lr.randf();
				const Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
				{ Vector4i part; leaves.begin(part); _add_leaf(leaves, at, k == p_leaves - 1 ? tip_dir : out, up, size * (0.75f + 0.5f * lr.randf()), c); leaves.end(part, 0.0f); }
				leaf_count++;
			}
		};
		for (int t_i = 0; t_i < twigs; t_i++) {
			const float t = (t_i + 0.5f + 0.4f * (lr.randf() - 0.5f)) / twigs;
			roll += 2.39996f;
			const Vector3 around = perp.rotated(n.dir, roll);
			const Vector3 base = n.start + n.dir * (n.length * t);
			// Elevation from -35 to +45 degrees off the shoot's side plane.
			const float elev = Math::deg_to_rad(Math::lerp(-35.0f, 45.0f, lr.randf()));
			const Vector3 tdir = (around * (0.8f * Math::cos(elev)) + n.dir * 0.5f + Vector3(0, Math::sin(elev), 0)).normalized();
			const float tlen = n.length * (0.45f + 0.45f * lr.randf());
			if (lr.randf() > keep_twig) {
				continue;
			}
			if (lr.randf() < 0.4f && per_twig >= 4) {
				// A forked twig: two shorter shoots sharing the leaves.
				const Vector3 fork = tdir.cross(n.dir).normalized();
				const Vector3 d1 = (tdir + fork * 0.45f).normalized();
				const Vector3 d2 = (tdir - fork * 0.45f + Vector3(0, 0.2f, 0)).normalized();
				add_twig(base, d1, tlen * 0.8f, twig_r, per_twig / 2);
				add_twig(base + tdir * (tlen * 0.2f), d2, tlen * 0.7f, twig_r * 0.8f, per_twig - per_twig / 2);
			} else {
				add_twig(base, tdir, tlen, twig_r, per_twig);
			}
		}
	}

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Builder *builders[2] = { &bark, &leaves };
	for (int s = 0; s < 2; s++) {
		Builder &b = *builders[s];
		if (b.indices.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = b.verts;
		arrays[Mesh::ARRAY_NORMAL] = b.normals;
		arrays[Mesh::ARRAY_TEX_UV] = b.uvs;
		arrays[Mesh::ARRAY_COLOR] = b.colors;
		// Index LODs (the renderer picks one per instance by screen-space error): thin twigs and
		// then limbs drop out of the bark, and leaves are stochastically pruned with the survivors
		// enlarged to keep the crown's leaf area (Cook, Halstead, Planck and Ryu, Stochastic
		// Simplification of Aggregate Detail, SIGGRAPH 2007). Real leaves at every level, never
		// cards, so shadows and path-traced rays still meet foliage; the path tracer uses LOD 0.
		const Dictionary lods = s == 0 ? _bark_lods(b) : _leaf_lods(b);
		arrays[Mesh::ARRAY_VERTEX] = b.verts;
		arrays[Mesh::ARRAY_NORMAL] = b.normals;
		arrays[Mesh::ARRAY_TEX_UV] = b.uvs;
		arrays[Mesh::ARRAY_COLOR] = b.colors;
		arrays[Mesh::ARRAY_INDEX] = b.indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), lods);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(1, 1, 1));
		m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		m->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
		m->set_snow_from_weather(true);
		if (s == 0) {
			m->set_roughness(0.92f);
			mesh->surface_set_name(s, "bark");
		} else {
			m->set_roughness(needles ? 0.5f : 0.6f);
			if (needles && needle_sprays) {
				m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _spray_texture());
				// Hashed alpha with alpha to coverage: a spray seen as a pixel-wide line keeps its
				// needle coverage as a fraction of samples (Wyman and McGuire, Hashed Alpha Testing,
				// I3D 2017). A 0.5 alpha cut turned far sprays fully transparent once the mip
				// averaged their needles below the cut.
				m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_HASH);
				m->set_alpha_antialiasing(BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
			}
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
			m->set_backlight(leaf_color * 0.6f);
			mesh->surface_set_name(mesh->get_surface_count() - 1, needles ? "needles" : "leaves");
		}
		mesh->surface_set_material(mesh->get_surface_count() - 1, m);
	}
	return mesh;
}

// LOD levels: screen-space error in metres (what Godot compares against the pixel threshold),
// share of leaves kept, and the thinnest bark tube kept.
static const struct {
	float error;
	float keep;
	float min_radius;
} TREE_LODS[] = {
	{ 0.05f, 0.5f, 0.01f },
	{ 0.2f, 0.15f, 0.03f },
	{ 0.8f, 0.04f, 0.08f },
};

Dictionary TreeGenerator::_bark_lods(const Builder &p_bark) const {
	Dictionary lods;
	for (const auto &level : TREE_LODS) {
		PackedInt32Array idx;
		for (uint32_t i = 0; i < p_bark.parts.size(); i++) {
			if (p_bark.part_size[i] < level.min_radius) {
				continue;
			}
			const Vector4i &pt = p_bark.parts[i];
			const int at = idx.size();
			idx.resize(at + pt.y);
			memcpy(idx.ptrw() + at, p_bark.indices.ptr() + pt.x, sizeof(int32_t) * pt.y);
		}
		if (idx.is_empty()) {
			break;
		}
		lods[level.error] = idx;
	}
	return lods;
}

Dictionary TreeGenerator::_leaf_lods(Builder &r_leaves) const {
	Dictionary lods;
	const uint32_t count = r_leaves.parts.size();
	for (const auto &level : TREE_LODS) {
		// Survivors grow about their base by 1/sqrt(keep), so the kept leaf area matches.
		const float grow = 1.0f / Math::sqrt(level.keep);
		PackedInt32Array idx;
		for (uint32_t i = 0; i < count; i++) {
			// A fixed hash per leaf, so each level's survivors are a subset of the level before.
			const float u = float((uint32_t(i) * 2654435761u) >> 8) / 16777216.0f;
			if (u >= level.keep) {
				continue;
			}
			const Vector4i pt = r_leaves.parts[i];
			const int v0 = r_leaves.verts.size();
			const Vector3 pivot = r_leaves.verts[pt.z];
			for (int v = 0; v < pt.w; v++) {
				r_leaves.verts.push_back(pivot + (r_leaves.verts[pt.z + v] - pivot) * grow);
				r_leaves.normals.push_back(r_leaves.normals[pt.z + v]);
				r_leaves.uvs.push_back(r_leaves.uvs[pt.z + v]);
				r_leaves.colors.push_back(r_leaves.colors[pt.z + v]);
			}
			for (int k = 0; k < pt.y; k++) {
				idx.push_back(r_leaves.indices[pt.x + k] - pt.z + v0);
			}
		}
		if (idx.is_empty()) {
			break;
		}
		lods[level.error] = idx;
	}
	return lods;
}

// Habitat of each preset: where the species grows wild in Europe, as climate envelopes from the
// species range maps and climate profiles of the European Atlas of Forest Tree Species (San-Miguel-
// Ayanz et al., 2016) and Ellenberg's indicator values (light L for the shade a shrub or an
// open-grown tree tolerates). Temperatures are annual and January means in C, precipitation in
// mm a year, aridity is precipitation over potential evapotranspiration.
struct Habitat {
	float t_min, t_max; // Annual mean.
	float jan_min, jan_max; // January mean: frost hardiness, and the chill some need.
	float p_min, p_max; // Precipitation; p_max is the range maps' wet end, kept for reference.
	float aridity_min; // Drought limit.
	float slope_max; // Degrees.
	float soil_min; // Metres of soil (only with geology).
	float water_max; // Water distance limit in metres (0: none): alder and willow stay by water.
	float shade_max; // Canopy shade the open-grown form and the shrubs still grow under.
	float opacity; // Share of light the crown stops (beech, spruce, fir and lime cast deep shade).
	float stand_spacing; // Metres between stems in a mature stand.
	float footprint; // Clearance other species keep from the stem.
};

static const Habitat HABITATS[TreeGenerator::PRESET_MAX] = {
	// t       jan          precip       arid  slope soil  water shade  opac  space foot
	{ 5, 14, -12, 8, 450, 1200, 0.5f, 40, 0.4f, 0, 0.3f, 0.70f, 9.0f, 1.2f }, // Oak
	{ 5, 12, -8, 6, 650, 2000, 0.9f, 45, 0.3f, 0, 0.45f, 0.92f, 7.0f, 1.0f }, // Beech
	{ -2, 12, -25, 6, 400, 1500, 0.5f, 40, 0.15f, 0, 0.25f, 0.50f, 5.0f, 0.8f }, // Birch
	{ -3, 13, -30, 6, 300, 1500, 0.35f, 45, 0.1f, 0, 0.25f, 0.60f, 5.5f, 0.8f }, // Scots pine
	{ -2, 9, -25, 2, 700, 2500, 0.9f, 45, 0.2f, 0, 0.4f, 0.90f, 4.5f, 0.8f }, // Norway spruce
	{ 3, 13, -15, 7, 500, 1500, 0.6f, 40, 0.2f, 0, 0.75f, 0.80f, 3.0f, 0.6f }, // Hazel
	{ 4, 14, -15, 8, 450, 1400, 0.5f, 40, 0.15f, 0, 0.55f, 0.70f, 3.0f, 0.6f }, // Hawthorn
	{ 4, 12, -18, 5, 500, 1200, 0.6f, 40, 0.4f, 0, 0.45f, 0.85f, 7.0f, 1.0f }, // Lime
	{ 4, 11, -12, 5, 700, 2000, 0.8f, 45, 0.4f, 0, 0.45f, 0.85f, 7.0f, 1.0f }, // Sycamore
	{ 5, 13, -12, 7, 600, 1500, 0.7f, 35, 0.5f, 0, 0.3f, 0.60f, 7.0f, 1.0f }, // Ash
	{ 6, 14, -10, 6, 600, 1300, 0.6f, 25, 0.5f, 0, 0.3f, 0.90f, 9.0f, 1.2f }, // Horse chestnut
	{ 2, 13, -20, 7, 500, 1600, 0.7f, 15, 0.3f, 40, 0.3f, 0.75f, 5.0f, 0.8f }, // Black alder
	{ 3, 15, -20, 8, 400, 1500, 0.5f, 15, 0.3f, 25, 0.25f, 0.65f, 7.0f, 1.0f }, // White willow
	{ -3, 8, -18, 0, 600, 2000, 0.7f, 45, 0.15f, 0, 0.25f, 0.50f, 6.0f, 0.8f }, // Larch
	{ 4, 10, -12, 2, 900, 2500, 1.0f, 45, 0.3f, 0, 0.5f, 0.90f, 5.0f, 0.8f }, // Silver fir
	{ -2, 11, -25, 5, 500, 2000, 0.6f, 45, 0.1f, 0, 0.45f, 0.60f, 4.0f, 0.6f }, // Rowan
	{ 5, 14, -12, 8, 500, 1400, 0.6f, 35, 0.3f, 0, 0.7f, 0.70f, 3.0f, 0.6f }, // Elder
	// Holly's cold limit is the January mean of about -0.5 C (Iversen 1944, the classic
	// climatic indicator species); it fills the shrub layer of oceanic beech and oak woods.
	{ 6, 15, -1.5f, 9, 600, 2500, 0.7f, 40, 0.2f, 0, 0.92f, 0.90f, 4.0f, 0.7f }, // Holly
	{ 6, 13, -6, 8, 600, 2000, 0.6f, 45, 0.1f, 0, 0.95f, 0.95f, 5.0f, 0.8f }, // Yew
	{ 8, 15, -3, 9, 500, 1400, 0.4f, 45, 0.05f, 0, 0.9f, 0.85f, 2.5f, 0.5f }, // Box
	// Eastern North America: Silvics of North America (Burns and Honkala 1990) where it gives
	// figures (white oak 7-21 C annual and 760 mm; yellow birch from the 2 C isotherm and 640 mm;
	// hemlock, January -12 to 6 C and over 740 mm; sugar maple, January -18 to 10 C and 510 mm;
	// white pine 510 mm), the rest from the climate percentiles of Thompson, Anderson and
	// Bartlein, USGS Professional Paper 1650-A, 1999.
	{ 2, 14, -18, 6, 600, 2000, 0.7f, 40, 0.3f, 0, 0.9f, 0.90f, 7.0f, 1.0f }, // Sugar maple
	{ 2, 21, -18, 12, 650, 2000, 0.6f, 40, 0.15f, 0, 0.7f, 0.70f, 6.0f, 0.9f }, // Red maple
	{ 4, 16, -15, 6, 650, 1600, 0.55f, 40, 0.3f, 0, 0.55f, 0.70f, 8.0f, 1.1f }, // Red oak
	{ 7, 21, -12, 12, 760, 2000, 0.55f, 40, 0.3f, 0, 0.45f, 0.65f, 9.0f, 1.2f }, // White oak
	{ 2, 14, -18, 4, 510, 2000, 0.4f, 45, 0.1f, 0, 0.6f, 0.55f, 6.0f, 0.9f }, // White pine
	{ 3, 13, -14, 6, 740, 2000, 0.9f, 45, 0.15f, 0, 0.95f, 0.92f, 5.0f, 0.8f }, // Hemlock
	{ 3, 20, -14, 12, 750, 2000, 0.8f, 45, 0.3f, 0, 0.9f, 0.90f, 7.0f, 1.0f }, // American beech
	{ -5, 8, -30, -4, 300, 1500, 0.4f, 45, 0.1f, 0, 0.4f, 0.45f, 4.5f, 0.7f }, // Paper birch
	{ 2, 10, -18, -3, 640, 1500, 0.8f, 40, 0.2f, 0, 0.65f, 0.70f, 6.0f, 0.9f }, // Yellow birch
	{ 3, 19, -15, 10, 650, 1600, 0.6f, 40, 0.2f, 0, 0.85f, 0.65f, 3.0f, 0.6f }, // Witch-hazel
	{ 7, 19, -8, 10, 750, 2000, 0.6f, 45, 0.1f, 0, 0.85f, 0.80f, 2.5f, 0.5f }, // Mountain laurel
};

static bool is_shrub(TreeGenerator::Preset p_preset) {
	return p_preset == TreeGenerator::PRESET_HAZEL || p_preset == TreeGenerator::PRESET_HAWTHORN || p_preset == TreeGenerator::PRESET_ELDER || p_preset == TreeGenerator::PRESET_HOLLY || p_preset == TreeGenerator::PRESET_BOX ||
			p_preset == TreeGenerator::PRESET_WITCH_HAZEL || p_preset == TreeGenerator::PRESET_MOUNTAIN_LAUREL;
}

void TreeGenerator::apply_habitat(const Ref<PlantSpecies> &p_species) const {
	ERR_FAIL_COND(p_species.is_null());
	const Habitat &h = HABITATS[preset];
	// Soft edges: about 1.5 C of annual mean, 3 C of January mean and 150 mm of rain, so ranges
	// thin out across a belt instead of stopping at a line.
	p_species->set_rule_range(PlantSpecies::RULE_TEMPERATURE, Vector2(h.t_min, h.t_max));
	p_species->set_rule_fade(PlantSpecies::RULE_TEMPERATURE, 1.5f);
	p_species->set_rule_range(PlantSpecies::RULE_JANUARY_TEMPERATURE, Vector2(h.jan_min, h.jan_max));
	p_species->set_rule_fade(PlantSpecies::RULE_JANUARY_TEMPERATURE, 3.0f);
	// Only the dry end limits: the wet ends of the range maps follow where the climate is also cold
	// or oceanic, which the temperature rules already cover; rain itself does not stop a tree.
	p_species->set_rule_range(PlantSpecies::RULE_PRECIPITATION, Vector2(h.p_min, 100000.0f));
	p_species->set_rule_fade(PlantSpecies::RULE_PRECIPITATION, 150.0f);
	p_species->set_rule_range(PlantSpecies::RULE_ARIDITY, Vector2(h.aridity_min, 1000.0f));
	p_species->set_rule_fade(PlantSpecies::RULE_ARIDITY, 0.15f);
	p_species->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, h.slope_max));
	p_species->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
	p_species->set_rule_range(PlantSpecies::RULE_SOIL_DEPTH, Vector2(h.soil_min, 100000.0f));
	p_species->set_rule_fade(PlantSpecies::RULE_SOIL_DEPTH, h.soil_min * 0.5f);
	// Woody plants root on land: no trees standing in water.
	p_species->set_rule_range(PlantSpecies::RULE_WATER_DEPTH, Vector2(0.0f, 0.0f));
	p_species->set_rule_fade(PlantSpecies::RULE_WATER_DEPTH, 0.05f);
	if (h.water_max > 0.0f) {
		p_species->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.0f, h.water_max));
		p_species->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, h.water_max * 0.4f);
	}
}

// One species of a stand. The stand-grown form (grown with neighbours: tall, narrow, high crown
// base) fills the stand; the open-grown form is the same tree in a clearing or at an edge, kept
// there by a shade rule, and shrubs get the shade their light value allows.
Ref<PlantSpecies> TreeGenerator::_stand_species(Preset p_preset, bool p_stand_grown, int p_variants) {
	const int key = int(p_preset) * 2 + (p_stand_grown ? 1 : 0);
	const Habitat &h = HABITATS[p_preset];
	if (!mesh_cache.has(key) || mesh_cache[key].size() < MAX(p_variants, 1)) {
		// Generate with this generator's colours and seed, under the preset's own parameters.
		const int saved_seed = seed;
		const Preset saved_preset = preset;
		float saved_params[PARAM_MAX];
		memcpy(saved_params, params, sizeof(params));
		const Phyllotaxis saved_phyllotaxis = phyllotaxis;
		const float saved_shade = forest_shade;
		const float saved_spacing = forest_spacing;
		const Color saved_bark = bark_color;
		const Color saved_leaf = leaf_color;

		set_preset(p_preset);
		// Placed trees are seen in numbers and mostly from a distance: conifers there use needle
		// sprays (a spruce is 7M vertices instead of 53M). A single tree near the camera is grown
		// with generate() and keeps every needle.
		const bool saved_sprays = needle_sprays;
		needle_sprays = true;
		forest_shade = p_stand_grown ? 0.6f : 0.0f;
		// Neighbours in a Poisson-disk stand stand on average about 1.3 times the minimum spacing
		// apart, so crowns grown to that boundary close the canopy as real stands do.
		forest_spacing = (is_shrub(p_preset) ? 3.0f : h.stand_spacing) * 1.3f;
		TypedArray<Mesh> meshes;
		float crown = 0.0f;
		const int n = MAX(p_variants, 1);
		for (int v = 0; v < n; v++) {
			seed = saved_seed * 31 + v;
			Ref<ArrayMesh> mesh = generate();
			crown += crown_radius;
			if (p_variants > 0) {
				meshes.push_back(mesh);
			}
		}
		mesh_cache[key] = meshes;
		crown_cache[key] = crown / n;

		seed = saved_seed;
		preset = saved_preset;
		memcpy(params, saved_params, sizeof(params));
		phyllotaxis = saved_phyllotaxis;
		forest_shade = saved_shade;
		forest_spacing = saved_spacing;
		needle_sprays = saved_sprays;
		bark_color = saved_bark;
		leaf_color = saved_leaf;
	}

	Ref<PlantSpecies> sp;
	sp.instantiate();
	sp->set_name(String(preset_names[p_preset]) + (p_stand_grown ? " (stand)" : " (open)"));
	TypedArray<Mesh> meshes = mesh_cache[key];
	if (p_variants < meshes.size()) {
		meshes.resize(p_variants);
	}
	sp->set_meshes(meshes);
	{
		const Preset saved_preset = preset;
		preset = p_preset;
		apply_habitat(sp);
		preset = saved_preset;
	}
	const float crown = crown_cache[key];
	// Needle cover overhead for the forest floor (VegetationField3D.get_conifer_at on the
	// vegetation branch); a no-op until PlantSpecies has the property.
	const bool conifer = p_preset == PRESET_SCOTS_PINE || p_preset == PRESET_NORWAY_SPRUCE || p_preset == PRESET_SILVER_FIR || p_preset == PRESET_LARCH || p_preset == PRESET_YEW ||
			p_preset == PRESET_WHITE_PINE || p_preset == PRESET_HEMLOCK;
	sp->set("canopy_conifer", conifer);
	sp->set_canopy_radius(crown);
	sp->set_canopy_opacity(h.opacity);
	sp->set_footprint(h.footprint);
	sp->set_scale_range(Vector2(0.85f, 1.15f));
	sp->set_align_to_ground(0.0f); // Trunks grow plumb on slopes (gravitropism).
	// Clearings and paths painted into VegetationField3D.density_mask keep the woody layer out.
	sp->set_mask_channel(mask_channel);
	sp->set_mask_invert(mask_invert);
	if (p_stand_grown || is_shrub(p_preset)) {
		sp->set_spacing(h.stand_spacing);
	} else {
		// Open-grown trees stand apart: at least a crown across.
		sp->set_spacing(MAX(h.stand_spacing * 1.6f, crown * 2.0f));
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, h.shade_max));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
	}
	if (is_shrub(p_preset)) {
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, h.shade_max));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
	}
	return sp;
}

Ref<PlantSpecies> TreeGenerator::make_species(bool p_stand_grown, int p_variants) {
	return _stand_species(preset, p_stand_grown, p_variants);
}

void TreeGenerator::clear_cache() {
	mesh_cache.clear();
	crown_cache.clear();
}

// Stands, in placement order: the stand's canopy trees first (stand-grown form; a second species
// splits the ground with the first through complementary patches, the way mixed stands form
// clumps), then the open-grown forms in the gaps and at the edges, then the shrub layer. Each
// canopy tree after the first takes a shade rule, so it fills only gaps the earlier crowns left:
// in make_woodland() that also lets a later stand grow only where an earlier one did not.
Ref<Biome> TreeGenerator::make_stand(Stand p_stand, int p_variants) {
	ERR_FAIL_INDEX_V(int(p_stand), int(STAND_MAX), Ref<Biome>());
	TypedArray<PlantSpecies> list;
	const int patch_seed = 7919 * (int(p_stand) + 1) + seed;
	auto canopy = [&](Preset p, float p_density, float p_patch_scale, float p_cover, bool p_invert, bool p_first) {
		Ref<PlantSpecies> sp = _stand_species(p, true, p_variants);
		sp->set_density(p_density);
		if (p_patch_scale > 0.0f) {
			sp->set_patch_scale(p_patch_scale);
			sp->set_patch_cover(p_cover);
			sp->set_patch_seed(patch_seed);
			sp->set_patch_invert(p_invert);
		}
		if (!p_first) {
			// Only where the crowns placed before leave light.
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
			sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
		}
		list.push_back(sp);
		return sp;
	};
	auto open = [&](Preset p, float p_density) {
		Ref<PlantSpecies> sp = _stand_species(p, false, p_variants);
		sp->set_density(p_density);
		list.push_back(sp);
		return sp;
	};
	auto shrub = [&](Preset p, float p_density) {
		Ref<PlantSpecies> sp = _stand_species(p, false, p_variants);
		sp->set_density(p_density);
		list.push_back(sp);
		return sp;
	};
	auto clip = [](const Ref<PlantSpecies> &p_sp, PlantSpecies::Rule p_rule, const Vector2 &p_range, float p_fade) {
		// Narrow a species' range to the stand's belt (never widen past its habitat).
		const Vector2 r = p_sp->get_rule_range(p_rule);
		p_sp->set_rule_range(p_rule, Vector2(MAX(r.x, p_range.x), MIN(r.y, p_range.y)));
		p_sp->set_rule_fade(p_rule, p_fade);
	};

	switch (p_stand) {
		case STAND_BEECH: {
			// Closed beech canopy; sycamore and ash in clumps between, thinner, with gaps the open
			// forms and shrubs take; little shrub layer survives the beech shade (Ellenberg 1988:
			// Fagion).
			canopy(PRESET_BEECH, 1.0f, 60.0f, 0.8f, false, true);
			canopy(PRESET_SYCAMORE_MAPLE, 0.6f, 60.0f, 0.8f, true, false);
			canopy(PRESET_ASH, 0.5f, 60.0f, 0.8f, true, false);
			open(PRESET_BEECH, 0.3f);
			// Holly and yew are what does grow under beech (Taxo-Fagetum, Ilici-Fagetum).
			shrub(PRESET_HOLLY, 0.5f);
			Ref<PlantSpecies> yew = shrub(PRESET_YEW, 0.25f);
			yew->set_patch_scale(40.0f);
			yew->set_patch_cover(0.35f);
			yew->set_patch_seed(patch_seed + 3);
			shrub(PRESET_HAZEL, 0.25f);
			shrub(PRESET_ELDER, 0.15f);
		} break;
		case STAND_OAK_MIXED: {
			// Oak and small-leaved lime in clumps, ash in moist hollows, a hazel coppice layer,
			// birch and hawthorn in the clearings.
			Ref<PlantSpecies> ash = canopy(PRESET_ASH, 1.0f, 0.0f, 0.0f, false, true);
			ash->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(7.0f, 1000.0f));
			ash->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.5f);
			canopy(PRESET_OAK, 1.0f, 70.0f, 0.55f, false, false);
			canopy(PRESET_LIME, 1.0f, 70.0f, 0.55f, true, false);
			open(PRESET_OAK, 0.3f);
			open(PRESET_BIRCH, 0.4f);
			shrub(PRESET_HAZEL, 0.7f);
			shrub(PRESET_HOLLY, 0.3f);
			shrub(PRESET_HAWTHORN, 0.35f);
			Ref<PlantSpecies> box = shrub(PRESET_BOX, 0.4f);
			box->set_patch_scale(30.0f);
			box->set_patch_cover(0.3f);
			box->set_patch_seed(patch_seed + 5);
		} break;
		case STAND_MONTANE_CONIFER: {
			// Spruce-fir with beech of the montane belt, mixed in clumps.
			// The montane belt: about 1.5 to 6 C annual mean (Ellenberg 1988; Ozenda 1985).
			const Vector2 belt(1.5f, 6.0f);
			clip(canopy(PRESET_NORWAY_SPRUCE, 1.0f, 60.0f, 0.55f, false, true), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(canopy(PRESET_SILVER_FIR, 1.0f, 60.0f, 0.55f, true, false), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(canopy(PRESET_BEECH, 0.4f, 0.0f, 0.0f, false, false), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(open(PRESET_NORWAY_SPRUCE, 0.3f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(shrub(PRESET_ROWAN, 0.3f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(shrub(PRESET_ELDER, 0.1f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
		} break;
		case STAND_SUBALPINE: {
			// Larch and spruce thinning towards the tree line; rowan in the gaps.
			// Subalpine up to the tree line (about the 10 C July isotherm; Korner 2012).
			// Its cold end is the tree line, about -1.5 C annual mean in the Alps.
			const Vector2 belt(-1.5f, 2.5f);
			clip(canopy(PRESET_LARCH, 0.8f, 50.0f, 0.6f, false, true), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(canopy(PRESET_NORWAY_SPRUCE, 0.8f, 50.0f, 0.6f, true, false), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(open(PRESET_LARCH, 0.4f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(open(PRESET_ROWAN, 0.4f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
		} break;
		case STAND_PINE_HEATH: {
			// Scots pine on dry ground, open enough for birch and rowan beneath.
			const Vector2 dry(0.0f, 750.0f);
			clip(canopy(PRESET_SCOTS_PINE, 0.9f, 80.0f, 0.85f, false, true), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(open(PRESET_SCOTS_PINE, 0.35f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(open(PRESET_BIRCH, 0.4f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(shrub(PRESET_ROWAN, 0.3f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
		} break;
		case STAND_RIPARIAN: {
			// Alder carr on the banks, white willows at the water's edge, elder.
			canopy(PRESET_BLACK_ALDER, 1.0f, 0.0f, 0.0f, false, true);
			open(PRESET_WHITE_WILLOW, 0.6f);
			open(PRESET_BLACK_ALDER, 0.4f);
			clip(shrub(PRESET_ELDER, 0.4f), PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.0f, 40.0f), 16.0f);
		} break;
		case STAND_HEDGEROW: {
			// Field boundary scrub: a dense shrub mix with open-grown standards. Placed by the
			// scene along its own lines (no woodland climate belt of its own).
			open(PRESET_OAK, 0.25f);
			open(PRESET_LIME, 0.15f);
			open(PRESET_HORSE_CHESTNUT, 0.1f);
			shrub(PRESET_HAWTHORN, 1.0f);
			shrub(PRESET_HAZEL, 0.7f);
			shrub(PRESET_HOLLY, 0.3f);
			shrub(PRESET_ELDER, 0.4f);
			open(PRESET_ROWAN, 0.3f);
		} break;
		case STAND_NORTHERN_HARDWOOD: {
			// Sugar maple-beech-yellow birch (SAF cover type 25; Eyre 1980; Braun 1950): maple and
			// beech in clumps, yellow birch in gaps, hemlock where it is moist; witch-hazel beneath.
			canopy(PRESET_SUGAR_MAPLE, 1.0f, 60.0f, 0.6f, false, true);
			canopy(PRESET_AMERICAN_BEECH, 1.0f, 60.0f, 0.6f, true, false);
			Ref<PlantSpecies> hemlock = canopy(PRESET_HEMLOCK, 0.8f, 0.0f, 0.0f, false, false);
			hemlock->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(7.0f, 1000.0f));
			hemlock->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.5f);
			canopy(PRESET_YELLOW_BIRCH, 0.6f, 0.0f, 0.0f, false, false);
			open(PRESET_SUGAR_MAPLE, 0.25f);
			open(PRESET_PAPER_BIRCH, 0.3f);
			shrub(PRESET_WITCH_HAZEL, 0.5f);
		} break;
		case STAND_EASTERN_OAK: {
			// Oak forest of the eastern uplands (SAF 52-55, white oak-red oak): the two oaks in
			// clumps, red maple between; mountain laurel thickets and witch-hazel in the shrub layer.
			canopy(PRESET_WHITE_OAK, 1.0f, 70.0f, 0.55f, false, true);
			canopy(PRESET_RED_OAK, 1.0f, 70.0f, 0.55f, true, false);
			canopy(PRESET_RED_MAPLE, 0.7f, 0.0f, 0.0f, false, false);
			open(PRESET_WHITE_OAK, 0.3f);
			open(PRESET_RED_MAPLE, 0.3f);
			Ref<PlantSpecies> laurel = shrub(PRESET_MOUNTAIN_LAUREL, 0.8f);
			laurel->set_patch_scale(35.0f);
			laurel->set_patch_cover(0.45f);
			laurel->set_patch_seed(patch_seed + 7);
			shrub(PRESET_WITCH_HAZEL, 0.4f);
		} break;
		case STAND_PINE_HEMLOCK: {
			// White pine-hemlock (SAF 22): emergent pines over hemlock, which takes the shade and
			// the moist slopes; paper birch and red maple in openings.
			canopy(PRESET_WHITE_PINE, 1.0f, 60.0f, 0.6f, false, true);
			canopy(PRESET_HEMLOCK, 1.0f, 60.0f, 0.6f, true, false);
			canopy(PRESET_RED_MAPLE, 0.4f, 0.0f, 0.0f, false, false);
			open(PRESET_WHITE_PINE, 0.35f);
			open(PRESET_PAPER_BIRCH, 0.4f);
			shrub(PRESET_WITCH_HAZEL, 0.3f);
		} break;
		case STAND_MAX:
			break;
	}
	if (p_stand == STAND_HEDGEROW) {
		// A hedge is a line of light: no shade rules within it.
		for (int i = 0; i < list.size(); i++) {
			Ref<PlantSpecies> sp = list[i];
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
		}
	}
	for (int i = 0; i < list.size(); i++) {
		Ref<PlantSpecies> sp = list[i];
		sp->set_meta("stand", int(p_stand)); // Which stand a species of make_woodland() came from.
	}
	Ref<Biome> biome;
	biome.instantiate();
	biome->set_name(stand_names[p_stand]);
	biome->set_species(list);
	return biome;
}

// Every woodland stand in one Biome, in order of competitive strength on its own ground: water
// edges, then the cold belts, then the shade casters (beech before oak), then pine on what the
// broadleaves leave dry. Climate rules keep each stand to its belt; where belts overlap, the shade
// rule of the later stand's trees leaves the ground to the earlier canopy.
Ref<Biome> TreeGenerator::make_woodland(int p_variants, Region p_region) {
	ERR_FAIL_INDEX_V(int(p_region), int(REGION_MAX), Ref<Biome>());
	static const Stand europe[] = { STAND_RIPARIAN, STAND_SUBALPINE, STAND_MONTANE_CONIFER, STAND_BEECH, STAND_OAK_MIXED, STAND_PINE_HEATH };
	// North America: hemlock-pine holds the cool, moist ground, northern hardwoods the cool
	// uplands, oak the warmer and drier south, as in Braun's deciduous forest regions (1950).
	static const Stand north_america[] = { STAND_PINE_HEMLOCK, STAND_NORTHERN_HARDWOOD, STAND_EASTERN_OAK };
	LocalVector<Stand> order;
	if (p_region == REGION_EUROPE) {
		for (Stand st : europe) {
			order.push_back(st);
		}
	} else {
		for (Stand st : north_america) {
			order.push_back(st);
		}
	}
	TypedArray<PlantSpecies> canopies, rest;
	for (Stand st : order) {
		Ref<Biome> b = make_stand(st, p_variants);
		const TypedArray<PlantSpecies> sps = b->get_species();
		for (int i = 0; i < sps.size(); i++) {
			Ref<PlantSpecies> sp = sps[i];
			// Each stand's first canopy tree would ignore the stands before it: give it the same
			// gap rule. (The first stand's own first tree, alder, keeps the water edge.)
			if (i == 0 && st != order[0]) {
				sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
				sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
			}
			const bool tree_layer = sp->get_name().ends_with("(stand)");
			(tree_layer ? canopies : rest).push_back(sp);
		}
	}
	// All canopy trees before any gap filler or shrub, so shrubs see the whole canopy.
	TypedArray<PlantSpecies> list = canopies;
	list.append_array(rest);
	Ref<Biome> biome;
	biome.instantiate();
	biome->set_name(p_region == REGION_EUROPE ? "Woodland" : "North American Woodland");
	biome->set_species(list);
	return biome;
}

// Foliage by height: the mean horizontal reach of leafy shoots in each of p_bins equal slices from
// the ground to the top, so tests can see where a crown is full and where it is bare.
PackedFloat32Array TreeGenerator::get_foliage_profile(int p_bins) const {
	PackedFloat32Array out;
	ERR_FAIL_COND_V(p_bins < 1 || nodes.is_empty(), out);
	out.resize(p_bins);
	PackedInt32Array count;
	count.resize(p_bins);
	out.fill(0.0f);
	count.fill(0);
	const Vector3 root = nodes[0].start;
	const float h = MAX(height, 0.01f);
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.removed || n.dead || !(n.leafy || (i < int(spurs.size()) && spurs[i]))) {
			continue;
		}
		const Vector3 e = n.end();
		const int b = CLAMP(int((e.y - root.y) / h * p_bins), 0, p_bins - 1);
		out.write[b] += Vector2(e.x - root.x, e.z - root.z).length();
		count.write[b]++;
	}
	for (int b = 0; b < p_bins; b++) {
		out.write[b] = count[b] > 0 ? out[b] / count[b] : 0.0f;
	}
	return out;
}

Ref<ArrayMesh> TreeGenerator::generate() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	nodes.clear();
	buds.clear();
	shed_count = 0;
	rng.seed(uint64_t(seed) * 0x9E3779B97F4A7C15ull + uint64_t(preset) * 1013904223ull + 1);

	// Node 0 is the root collar: zero length, the stems are its laterals.
	Node root;
	root.dir = Vector3(0, 1, 0);
	root.leafy = false;
	nodes.push_back(root);
	const int stems = MAX(1, int(params[PARAM_STEMS]));
	const float spread = Math::deg_to_rad(params[PARAM_STEM_SPREAD]);
	auto add_stem_bud = [&](float p_angle, float p_tilt) {
		Bud b;
		b.node = 0;
		b.dir = Vector3(0, 1, 0).rotated(Vector3(Math::cos(p_angle), 0, Math::sin(p_angle)), p_tilt).normalized();
		buds.push_back(b);
	};
	for (int s = 0; s < stems; s++) {
		const float tilt = stems == 1 ? _rand(0.0f, 0.04f) : spread * _rand(0.4f, 1.0f);
		add_stem_bud(Math::TAU * s / stems + _rand(-0.4f, 0.4f), tilt);
	}
	const int years = CLAMP(int(params[PARAM_YEARS]), 1, 300);
	for (int y = 1; y <= years; y++) {
		if (params[PARAM_BASAL_SPROUTS] > 0.0f && rng.randf() < params[PARAM_BASAL_SPROUTS]) {
			add_stem_bud(_rand(0.0f, Math::TAU), spread * _rand(0.3f, 1.0f));
		}
		_grow_year(y);
		if (int(nodes.size()) >= MAX_NODES - 64) {
			break;
		}
	}
	_compute_radii();
	_bend_under_gravity();
	_measure();
	_mark_spurs(years);
	Ref<ArrayMesh> mesh = _build_mesh();
	generation_usec = OS::get_singleton()->get_ticks_usec() - t0;
	return mesh;
}

void TreeGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_preset", "preset"), &TreeGenerator::set_preset);
	ClassDB::bind_method(D_METHOD("get_preset"), &TreeGenerator::get_preset);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &TreeGenerator::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &TreeGenerator::get_seed);
	ClassDB::bind_method(D_METHOD("set_param", "param", "value"), &TreeGenerator::set_param);
	ClassDB::bind_method(D_METHOD("get_param", "param"), &TreeGenerator::get_param);
	ClassDB::bind_method(D_METHOD("set_phyllotaxis", "phyllotaxis"), &TreeGenerator::set_phyllotaxis);
	ClassDB::bind_method(D_METHOD("get_phyllotaxis"), &TreeGenerator::get_phyllotaxis);
	ClassDB::bind_method(D_METHOD("set_forest_shade", "shade"), &TreeGenerator::set_forest_shade);
	ClassDB::bind_method(D_METHOD("get_forest_shade"), &TreeGenerator::get_forest_shade);
	ClassDB::bind_method(D_METHOD("set_forest_spacing", "spacing"), &TreeGenerator::set_forest_spacing);
	ClassDB::bind_method(D_METHOD("get_forest_spacing"), &TreeGenerator::get_forest_spacing);
	ClassDB::bind_method(D_METHOD("set_mask_channel", "channel"), &TreeGenerator::set_mask_channel);
	ClassDB::bind_method(D_METHOD("get_mask_channel"), &TreeGenerator::get_mask_channel);
	ClassDB::bind_method(D_METHOD("set_mask_invert", "invert"), &TreeGenerator::set_mask_invert);
	ClassDB::bind_method(D_METHOD("get_mask_invert"), &TreeGenerator::get_mask_invert);
	ClassDB::bind_method(D_METHOD("set_needle_sprays", "sprays"), &TreeGenerator::set_needle_sprays);
	ClassDB::bind_method(D_METHOD("get_needle_sprays"), &TreeGenerator::get_needle_sprays);
	ClassDB::bind_method(D_METHOD("set_bark_color", "color"), &TreeGenerator::set_bark_color);
	ClassDB::bind_method(D_METHOD("get_bark_color"), &TreeGenerator::get_bark_color);
	ClassDB::bind_method(D_METHOD("set_leaf_color", "color"), &TreeGenerator::set_leaf_color);
	ClassDB::bind_method(D_METHOD("get_leaf_color"), &TreeGenerator::get_leaf_color);
	ClassDB::bind_method(D_METHOD("generate"), &TreeGenerator::generate);
	ClassDB::bind_method(D_METHOD("apply_habitat", "species"), &TreeGenerator::apply_habitat);
	ClassDB::bind_method(D_METHOD("make_species", "stand_grown", "variants"), &TreeGenerator::make_species, DEFVAL(3));
	ClassDB::bind_method(D_METHOD("make_stand", "stand", "variants"), &TreeGenerator::make_stand, DEFVAL(3));
	ClassDB::bind_method(D_METHOD("make_woodland", "variants", "region"), &TreeGenerator::make_woodland, DEFVAL(3), DEFVAL(REGION_EUROPE));
	ClassDB::bind_method(D_METHOD("clear_cache"), &TreeGenerator::clear_cache);
	ClassDB::bind_method(D_METHOD("get_height"), &TreeGenerator::get_height);
	ClassDB::bind_method(D_METHOD("get_crown_radius"), &TreeGenerator::get_crown_radius);
	ClassDB::bind_method(D_METHOD("get_crown_base"), &TreeGenerator::get_crown_base);
	ClassDB::bind_method(D_METHOD("get_bole_height"), &TreeGenerator::get_bole_height);
	ClassDB::bind_method(D_METHOD("get_foliage_profile", "bins"), &TreeGenerator::get_foliage_profile, DEFVAL(10));
	ClassDB::bind_method(D_METHOD("get_internode_count"), &TreeGenerator::get_internode_count);
	ClassDB::bind_method(D_METHOD("get_dead_branch_count"), &TreeGenerator::get_dead_branch_count);
	ClassDB::bind_method(D_METHOD("get_leaf_count"), &TreeGenerator::get_leaf_count);
	ClassDB::bind_method(D_METHOD("get_leaf_area_index"), &TreeGenerator::get_leaf_area_index);
	ClassDB::bind_method(D_METHOD("get_shed_count"), &TreeGenerator::get_shed_count);
	ClassDB::bind_method(D_METHOD("get_generation_usec"), &TreeGenerator::get_generation_usec);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "preset", PROPERTY_HINT_ENUM, "Oak,Beech,Birch,Scots Pine,Norway Spruce,Hazel,Hawthorn,Lime,Sycamore Maple,Ash,Horse Chestnut,Black Alder,White Willow,European Larch,Silver Fir,Rowan,Elder,Holly,Yew,Box,Sugar Maple,Red Maple,Northern Red Oak,White Oak,Eastern White Pine,Eastern Hemlock,American Beech,Paper Birch,Yellow Birch,Witch-hazel,Mountain Laurel"), "set_preset", "get_preset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "phyllotaxis", PROPERTY_HINT_ENUM, "Alternate,Opposite,Whorled"), "set_phyllotaxis", "get_phyllotaxis");
	ADD_GROUP("Stand", "forest_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forest_shade", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_forest_shade", "get_forest_shade");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask_channel", PROPERTY_HINT_ENUM, "None:-1,Red:0,Green:1,Blue:2,Alpha:3"), "set_mask_channel", "get_mask_channel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mask_invert"), "set_mask_invert", "get_mask_invert");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "needle_sprays"), "set_needle_sprays", "get_needle_sprays");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forest_spacing", PROPERTY_HINT_RANGE, "1,20,0.1,suffix:m"), "set_forest_spacing", "get_forest_spacing");
	ADD_GROUP("Colors", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "bark_color"), "set_bark_color", "get_bark_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "leaf_color"), "set_leaf_color", "get_leaf_color");
	ADD_GROUP("Growth", "");
	static const char *names[PARAM_MAX] = {
		"years", "internode_length", "internode_length_age", "max_shoot", "vigor", "apical_dominance",
		"apical_dominance_old", "maturity", "branch_angle", "branch_angle_variance", "divergence", "whorl_buds",
		"direction_noise", "phototropism", "gravitropism", "trunk_gravitropism", "shadow_strength",
		"shadow_falloff", "shadow_depth", "bud_light_min", "bud_idle_years", "shed_ratio", "dead_branch_keep",
		"tip_radius", "pipe_exponent", "gravity_bending", "stems", "stem_spread", "basal_sprouts", "root_flare",
		"leaf_size", "leaves_per_internode", "leaf_years", "needles", "twig_droop", "leaf_width", "apical_control",
		"lateral_extension", "crown_cone", "leader_growth"
	};
	for (int i = 0; i < PARAM_MAX; i++) {
		ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, names[i]), "set_param", "get_param", i);
	}

	BIND_ENUM_CONSTANT(PRESET_OAK);
	BIND_ENUM_CONSTANT(PRESET_BEECH);
	BIND_ENUM_CONSTANT(PRESET_BIRCH);
	BIND_ENUM_CONSTANT(PRESET_SCOTS_PINE);
	BIND_ENUM_CONSTANT(PRESET_NORWAY_SPRUCE);
	BIND_ENUM_CONSTANT(PRESET_HAZEL);
	BIND_ENUM_CONSTANT(PRESET_HAWTHORN);
	BIND_ENUM_CONSTANT(PRESET_LIME);
	BIND_ENUM_CONSTANT(PRESET_SYCAMORE_MAPLE);
	BIND_ENUM_CONSTANT(PRESET_ASH);
	BIND_ENUM_CONSTANT(PRESET_HORSE_CHESTNUT);
	BIND_ENUM_CONSTANT(PRESET_BLACK_ALDER);
	BIND_ENUM_CONSTANT(PRESET_WHITE_WILLOW);
	BIND_ENUM_CONSTANT(PRESET_LARCH);
	BIND_ENUM_CONSTANT(PRESET_SILVER_FIR);
	BIND_ENUM_CONSTANT(PRESET_ROWAN);
	BIND_ENUM_CONSTANT(PRESET_ELDER);
	BIND_ENUM_CONSTANT(PRESET_HOLLY);
	BIND_ENUM_CONSTANT(PRESET_YEW);
	BIND_ENUM_CONSTANT(PRESET_BOX);
	BIND_ENUM_CONSTANT(PRESET_SUGAR_MAPLE);
	BIND_ENUM_CONSTANT(PRESET_RED_MAPLE);
	BIND_ENUM_CONSTANT(PRESET_RED_OAK);
	BIND_ENUM_CONSTANT(PRESET_WHITE_OAK);
	BIND_ENUM_CONSTANT(PRESET_WHITE_PINE);
	BIND_ENUM_CONSTANT(PRESET_HEMLOCK);
	BIND_ENUM_CONSTANT(PRESET_AMERICAN_BEECH);
	BIND_ENUM_CONSTANT(PRESET_PAPER_BIRCH);
	BIND_ENUM_CONSTANT(PRESET_YELLOW_BIRCH);
	BIND_ENUM_CONSTANT(PRESET_WITCH_HAZEL);
	BIND_ENUM_CONSTANT(PRESET_MOUNTAIN_LAUREL);
	BIND_ENUM_CONSTANT(PRESET_MAX);
	BIND_ENUM_CONSTANT(STAND_BEECH);
	BIND_ENUM_CONSTANT(STAND_OAK_MIXED);
	BIND_ENUM_CONSTANT(STAND_MONTANE_CONIFER);
	BIND_ENUM_CONSTANT(STAND_SUBALPINE);
	BIND_ENUM_CONSTANT(STAND_PINE_HEATH);
	BIND_ENUM_CONSTANT(STAND_RIPARIAN);
	BIND_ENUM_CONSTANT(STAND_HEDGEROW);
	BIND_ENUM_CONSTANT(STAND_NORTHERN_HARDWOOD);
	BIND_ENUM_CONSTANT(STAND_EASTERN_OAK);
	BIND_ENUM_CONSTANT(STAND_PINE_HEMLOCK);
	BIND_ENUM_CONSTANT(STAND_MAX);
	BIND_ENUM_CONSTANT(REGION_EUROPE);
	BIND_ENUM_CONSTANT(REGION_NORTH_AMERICA);
	BIND_ENUM_CONSTANT(REGION_MAX);
	BIND_ENUM_CONSTANT(PHYLLOTAXIS_ALTERNATE);
	BIND_ENUM_CONSTANT(PHYLLOTAXIS_OPPOSITE);
	BIND_ENUM_CONSTANT(PHYLLOTAXIS_WHORLED);
	BIND_ENUM_CONSTANT(PARAM_YEARS);
	BIND_ENUM_CONSTANT(PARAM_INTERNODE_LENGTH);
	BIND_ENUM_CONSTANT(PARAM_INTERNODE_LENGTH_AGE);
	BIND_ENUM_CONSTANT(PARAM_MAX_SHOOT);
	BIND_ENUM_CONSTANT(PARAM_VIGOR);
	BIND_ENUM_CONSTANT(PARAM_APICAL_DOMINANCE);
	BIND_ENUM_CONSTANT(PARAM_APICAL_DOMINANCE_OLD);
	BIND_ENUM_CONSTANT(PARAM_MATURITY);
	BIND_ENUM_CONSTANT(PARAM_BRANCH_ANGLE);
	BIND_ENUM_CONSTANT(PARAM_BRANCH_ANGLE_VARIANCE);
	BIND_ENUM_CONSTANT(PARAM_DIVERGENCE);
	BIND_ENUM_CONSTANT(PARAM_WHORL_BUDS);
	BIND_ENUM_CONSTANT(PARAM_DIRECTION_NOISE);
	BIND_ENUM_CONSTANT(PARAM_PHOTOTROPISM);
	BIND_ENUM_CONSTANT(PARAM_GRAVITROPISM);
	BIND_ENUM_CONSTANT(PARAM_TRUNK_GRAVITROPISM);
	BIND_ENUM_CONSTANT(PARAM_SHADOW_STRENGTH);
	BIND_ENUM_CONSTANT(PARAM_SHADOW_FALLOFF);
	BIND_ENUM_CONSTANT(PARAM_SHADOW_DEPTH);
	BIND_ENUM_CONSTANT(PARAM_BUD_LIGHT_MIN);
	BIND_ENUM_CONSTANT(PARAM_BUD_IDLE_YEARS);
	BIND_ENUM_CONSTANT(PARAM_SHED_RATIO);
	BIND_ENUM_CONSTANT(PARAM_DEAD_BRANCH_KEEP);
	BIND_ENUM_CONSTANT(PARAM_TIP_RADIUS);
	BIND_ENUM_CONSTANT(PARAM_PIPE_EXPONENT);
	BIND_ENUM_CONSTANT(PARAM_GRAVITY_BENDING);
	BIND_ENUM_CONSTANT(PARAM_STEMS);
	BIND_ENUM_CONSTANT(PARAM_STEM_SPREAD);
	BIND_ENUM_CONSTANT(PARAM_BASAL_SPROUTS);
	BIND_ENUM_CONSTANT(PARAM_ROOT_FLARE);
	BIND_ENUM_CONSTANT(PARAM_LEAF_SIZE);
	BIND_ENUM_CONSTANT(PARAM_LEAVES_PER_INTERNODE);
	BIND_ENUM_CONSTANT(PARAM_LEAF_YEARS);
	BIND_ENUM_CONSTANT(PARAM_NEEDLES);
	BIND_ENUM_CONSTANT(PARAM_TWIG_DROOP);
	BIND_ENUM_CONSTANT(PARAM_LEAF_WIDTH);
	BIND_ENUM_CONSTANT(PARAM_APICAL_CONTROL);
	BIND_ENUM_CONSTANT(PARAM_LATERAL_EXTENSION);
	BIND_ENUM_CONSTANT(PARAM_CROWN_CONE);
	BIND_ENUM_CONSTANT(PARAM_LEADER_GROWTH);
	BIND_ENUM_CONSTANT(PARAM_MAX);
}

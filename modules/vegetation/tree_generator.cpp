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
			// Clipped forms (hedges, topiary; Palubicki 2009 section 6): a shoot that would leave the
			// envelope is cut back, as a hedge trimmer does each year.
			if (envelope.size.x > 0.0f && !envelope.has_point(pos + dir * len)) {
				break;
			}
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
void TreeGenerator::_tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color, const Image *p_height, float p_relief) {
	const int rings = int(p_points.size());
	if (rings < 2) {
		return;
	}
	const int base = r_b.verts.size();
	Vector3 t = (p_points[1] - p_points[0]).normalized();
	Vector3 ref = Math::abs(t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	Vector3 nrm = t.cross(ref).normalized();
	float v = p_v0;
	// A whole number of bark tiles around the stem (no seam), v in the same tile width so the
	// pattern keeps its aspect; thin shoots carry one tile, a finer pattern, as young bark is.
	const float girth = Math::TAU * MAX(p_radii[0], 0.001f);
	const float repeats = MAX(1.0f, Math::round(girth / BARK_TILE));
	const float circumference = girth / repeats;
	// Displacement: the bark family's height (metres, 0 on plate tops, negative in cracks),
	// bilinear and wrapped, along the surface normal.
	const float *hdata = nullptr;
	int hn = 0;
	Vector<uint8_t> hbuf;
	if (p_height && p_relief > 0.0f) {
		hbuf = p_height->get_data();
		hdata = reinterpret_cast<const float *>(hbuf.ptr());
		hn = p_height->get_width();
	}
	auto relief_at = [&](float p_u, float p_v) -> float {
		const float x = (p_u - Math::floor(p_u)) * hn - 0.5f, y = (p_v - Math::floor(p_v)) * hn - 0.5f;
		const int x0 = int(Math::floor(x)), y0 = int(Math::floor(y));
		const float fx = x - x0, fy = y - y0;
		auto at = [&](int i, int j) { return hdata[((j % hn + hn) % hn) * hn + ((i % hn + hn) % hn)]; };
		return Math::lerp(Math::lerp(at(x0, y0), at(x0 + 1, y0), fx), Math::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
	};
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
			float rr = p_radii[r];
			if (hdata) {
				rr += p_relief * relief_at(float(s) / p_sides * repeats, v);
			}
			r_b.verts.push_back(p_points[r] + dir * rr);
			r_b.normals.push_back(dir);
			r_b.uvs.push_back(Vector2(float(s) / p_sides * repeats, v));
			r_b.colors.push_back(p_color);
			if (r_b.use_tangents) {
				// Along u; the binormal (normal x tangent) then runs along the stem, with v.
				const Vector3 tan = bin * Math::cos(ang) - nrm * Math::sin(ang);
				r_b.tangents.push_back(tan.x);
				r_b.tangents.push_back(tan.y);
				r_b.tangents.push_back(tan.z);
				r_b.tangents.push_back(1.0f);
			}
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
// Each species' leaf: kind, lobes or leaflets, and lobe depth (sinus depth as a share of the
// half width). Simple leaves keep the lens of PARAM_LEAF_WIDTH.
struct LeafFormSpec {
	TreeGenerator::LeafKind kind;
	int count;
	float depth;
	bool round = false; // Rounded lobes (three rows a lobe) instead of pointed ones (two).
};
static const LeafFormSpec LEAF_FORMS[TreeGenerator::PRESET_MAX] = {
	{ TreeGenerator::LEAF_LOBED, 4, 0.55f, true }, // Oak: four or five rounded lobes a side.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Beech.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Birch.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Scots pine.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Norway spruce.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Hazel.
	{ TreeGenerator::LEAF_LOBED, 2, 0.7f }, // Hawthorn: two or three deep lobes a side.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Lime.
	{ TreeGenerator::LEAF_PALMATE, 5, 0.45f }, // Sycamore maple.
	{ TreeGenerator::LEAF_PINNATE, 9, 0.0f }, // Ash: 7-13 leaflets.
	{ TreeGenerator::LEAF_DIGITATE, 7, 0.0f }, // Horse chestnut: 5-7 leaflets.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Black alder.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // White willow.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Larch.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Silver fir.
	{ TreeGenerator::LEAF_PINNATE, 13, 0.0f }, // Rowan: 11-15 leaflets.
	{ TreeGenerator::LEAF_PINNATE, 5, 0.0f }, // Elder: 5-7 leaflets.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Holly.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Yew.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Box.
	{ TreeGenerator::LEAF_PALMATE, 5, 0.6f }, // Sugar maple.
	{ TreeGenerator::LEAF_PALMATE, 3, 0.35f }, // Red maple.
	{ TreeGenerator::LEAF_LOBED, 4, 0.65f }, // Red oak: bristle-tipped lobes, deep sinuses.
	{ TreeGenerator::LEAF_LOBED, 4, 0.6f, true }, // White oak: rounded lobes.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // White pine.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Hemlock.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // American beech.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Paper birch.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Yellow birch.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Witch hazel.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Mountain laurel.
};

Dictionary TreeGenerator::get_leaf_form(Preset p_preset) {
	ERR_FAIL_INDEX_V(int(p_preset), int(PRESET_MAX), Dictionary());
	TreeGenerator look;
	look.set_preset(p_preset);
	Dictionary d;
	d["kind"] = int(LEAF_FORMS[p_preset].kind);
	d["count"] = LEAF_FORMS[p_preset].count; // Lobes a side (lobed), lobes (palmate) or leaflets (compound).
	d["lobes"] = LEAF_FORMS[p_preset].count;
	d["depth"] = LEAF_FORMS[p_preset].depth;
	d["width"] = look.get_param(PARAM_LEAF_WIDTH); // Half width over length.
	d["size"] = look.get_param(PARAM_LEAF_SIZE); // Metres.
	d["color"] = look.get_leaf_color();
	return d;
}

// Phenology per species (docs/ideas/trees.md 2c). Budburst: thermal time above 5 C from 1 Jan
// (Cannell and Smith 1983), the requirement falling with winter chilling (Murray, Cannell and
// Smith 1989: F* = a + b exp(r C)); leaves then expand over about 180 degree days. Autumn:
// Delpierre et al. 2009 senescence sum (cold degree days under shortening days); colouring
// spreads from onset over the span, leaf fall follows the lag behind. Forcing requirements
// order the species as observed in Europe (Vitasse et al. 2009, Pan European Phenology
// network): elder, hazel and willow first, birch, larch and horse chestnut early, beech and
// oak late, ash last. Autumn colours: the species' carotenoid yellows, anthocyanin reds (red
// maple, rowan, red oak) and tannin browns (oaks, beech).
struct LeafSeasonSpec {
	bool deciduous;
	float forcing; // Degree days above 5 C to budburst at typical chilling.
	float onset; // Senescence sum where colouring starts.
	float span; // Senescence sum from first to last leaf turned.
	float lag; // Senescence sum from colouring to falling.
	Color autumn;
};
static const LeafSeasonSpec LEAF_SEASONS[TreeGenerator::PRESET_MAX] = {
	{ true, 270.0f, 330.0f, 260.0f, 140.0f, Color(0.3f, 0.18f, 0.05f) }, // Oak: late, brown.
	{ true, 260.0f, 290.0f, 220.0f, 120.0f, Color(0.38f, 0.17f, 0.04f) }, // Beech: copper.
	{ true, 150.0f, 180.0f, 220.0f, 90.0f, Color(0.45f, 0.33f, 0.04f) }, // Birch: early, yellow.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Scots pine.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Norway spruce.
	{ true, 120.0f, 220.0f, 220.0f, 100.0f, Color(0.4f, 0.31f, 0.05f) }, // Hazel.
	{ true, 160.0f, 230.0f, 200.0f, 100.0f, Color(0.32f, 0.11f, 0.04f) }, // Hawthorn: red brown.
	{ true, 230.0f, 200.0f, 200.0f, 90.0f, Color(0.42f, 0.32f, 0.05f) }, // Lime: yellow.
	{ true, 200.0f, 230.0f, 220.0f, 100.0f, Color(0.38f, 0.28f, 0.05f) }, // Sycamore: yellow brown.
	{ true, 320.0f, 200.0f, 160.0f, 40.0f, Color(0.32f, 0.32f, 0.09f) }, // Ash: last out, falls barely turned.
	{ true, 160.0f, 170.0f, 220.0f, 110.0f, Color(0.42f, 0.2f, 0.04f) }, // Horse chestnut: orange brown, early.
	{ true, 170.0f, 330.0f, 140.0f, 30.0f, Color(0.17f, 0.21f, 0.06f) }, // Black alder: falls green.
	{ true, 130.0f, 280.0f, 220.0f, 100.0f, Color(0.38f, 0.33f, 0.07f) }, // White willow: yellow.
	{ true, 140.0f, 300.0f, 200.0f, 120.0f, Color(0.45f, 0.3f, 0.05f) }, // Larch: gold needles, then bare.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Silver fir.
	{ true, 160.0f, 190.0f, 200.0f, 100.0f, Color(0.45f, 0.1f, 0.03f) }, // Rowan: red orange.
	{ true, 100.0f, 250.0f, 200.0f, 80.0f, Color(0.3f, 0.22f, 0.08f) }, // Elder: first out.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Holly.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Yew.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Box.
	{ true, 220.0f, 170.0f, 200.0f, 110.0f, Color(0.55f, 0.2f, 0.02f) }, // Sugar maple: orange.
	{ true, 200.0f, 150.0f, 200.0f, 110.0f, Color(0.5f, 0.05f, 0.03f) }, // Red maple: scarlet, first to turn.
	{ true, 260.0f, 260.0f, 240.0f, 130.0f, Color(0.35f, 0.07f, 0.03f) }, // Red oak: russet red.
	{ true, 290.0f, 300.0f, 240.0f, 140.0f, Color(0.28f, 0.1f, 0.06f) }, // White oak: purple brown.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // White pine.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Hemlock.
	{ true, 270.0f, 260.0f, 220.0f, 120.0f, Color(0.42f, 0.28f, 0.06f) }, // American beech: bronze.
	{ true, 160.0f, 170.0f, 200.0f, 90.0f, Color(0.48f, 0.36f, 0.04f) }, // Paper birch: yellow.
	{ true, 190.0f, 190.0f, 200.0f, 90.0f, Color(0.46f, 0.35f, 0.05f) }, // Yellow birch.
	{ true, 200.0f, 210.0f, 200.0f, 100.0f, Color(0.47f, 0.37f, 0.05f) }, // Witch hazel: yellow.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Mountain laurel.
};

TreeGenerator::LeafSeason TreeGenerator::_leaf_season(Preset p_preset, const Dictionary &p_season) {
	LeafSeason ls;
	ERR_FAIL_INDEX_V(int(p_preset), int(PRESET_MAX), ls);
	const LeafSeasonSpec &sp = LEAF_SEASONS[p_preset];
	ls.color = sp.autumn;
	if (!sp.deciduous || p_season.is_empty()) {
		return ls;
	}
	const float forcing = p_season.get("forcing", 1000.0f);
	const float chilling = p_season.get("chilling", 80.0f);
	const float senescence = p_season.get("senescence", 0.0f);
	const float day = p_season.get("day_of_year", 182.0f);
	// Murray et al. 1989 shape, normalised to 1 at 80 chilling days.
	const float need = 0.55f * sp.forcing * (0.55f + 0.9f * Math::exp(-0.012f * MAX(chilling, 0.0f))) / 0.894f;
	ls.out = CLAMP((forcing - need) / 150.0f, 0.0f, 1.0f);
	ls.autumn = CLAMP((senescence - sp.onset) / sp.span, 0.0f, 1.0f);
	ls.fallen = CLAMP((senescence - sp.onset - sp.lag) / sp.span, 0.0f, 1.0f);
	if (forcing <= need && day < 182.0f) {
		// Winter and early spring: last year's crop is all down, none grown yet.
		ls.out = 0.0f;
		ls.fallen = 1.0f;
	}
	return ls;
}

Dictionary TreeGenerator::get_leaf_season(Preset p_preset, const Dictionary &p_season) {
	const LeafSeason ls = _leaf_season(p_preset, p_season);
	Dictionary d;
	d["leaf_out"] = ls.out;
	d["autumn"] = ls.autumn;
	d["fallen_fraction"] = ls.fallen;
	d["autumn_color"] = ls.color;
	return d;
}

// A folded lens blade: rows of three vertices (midrib, two margins) from base to tip, the width
// profile sin(pi t^p_shape) (0.75 ovate, widest below the middle; 1.6 obovate, widest toward the
// tip), with p_lobes lobes a side cut p_depth into it: pointed (a tip row between sinus rows) or
// rounded (two shoulder rows between sinus rows, as in oak).
void TreeGenerator::_add_blade(Builder &r_b, const Vector3 &p_start, const Vector3 &p_along, const Vector3 &p_side, const Vector3 &p_n, float p_length, float p_half_width, int p_steps, int p_lobes, float p_depth, const Color &p_color, bool p_round, float p_shape) {
	const int base = r_b.verts.size();
	for (int i = 0; i <= p_steps; i++) {
		const float t = float(i) / p_steps;
		float w = p_half_width * Math::sin(Math::PI * Math::pow(t, p_shape));
		if (p_lobes > 0 && i > 0 && i < p_steps) {
			const int rows = p_round ? 3 : 2;
			if (i % rows == 0) {
				w *= 1.0f - p_depth; // Sinus.
			} else if (p_round) {
				w *= 1.0f - 0.08f * p_depth; // Shoulders: a lobe with a broad, round end.
			}
		}
		const float droop = -0.25f * p_length * t * t;
		const Vector3 mid = p_start + p_along * (t * p_length) + p_n * droop;
		const Vector3 fold = p_n * (0.12f * w);
		r_b.verts.push_back(mid);
		r_b.normals.push_back(p_n);
		r_b.uvs.push_back(Vector2(0.5f, t));
		r_b.colors.push_back(p_color);
		r_b.verts.push_back(mid + p_side * w + fold);
		r_b.normals.push_back((p_n - p_side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(1.0f, t));
		r_b.colors.push_back(p_color);
		r_b.verts.push_back(mid - p_side * w + fold);
		r_b.normals.push_back((p_n + p_side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(0.0f, t));
		r_b.colors.push_back(p_color);
	}
	for (int i = 0; i < p_steps; i++) {
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

// A leaf: a short petiole, then the species' blade turned to face mostly upward (leaves orient
// to the light). Lobed, palmate and compound leaves keep a simple lens of the same area as a twin
// for the coarser LODs.
void TreeGenerator::_add_leaf(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, const Vector3 &p_up, float p_size, const Color &p_color) {
	Vector3 n = p_up;
	Vector3 along = (p_out - n * p_out.dot(n));
	if (along.length_squared() < 1e-6f) {
		along = n.cross(Vector3(1, 0, 0));
	}
	along.normalize();
	const Vector3 side = n.cross(along).normalized();
	const Vector3 start = p_base + p_out * (p_size * 0.15f);
	const float hw = params[PARAM_LEAF_WIDTH] * p_size;
	const LeafFormSpec &form = LEAF_FORMS[preset];
	if (!species_leaf_forms || form.kind == LEAF_SIMPLE || form.kind == LEAF_NEEDLE) {
		_add_blade(r_b, start, along, side, n, p_size, hw, 3, 0, 0.0f, p_color);
		return;
	}
	// Compound leaves are big: one fills the slots of several simple ones (n_eff), with the same
	// leaf area (the crown's density, drag and mass stay those of the slots).
	const bool compound = form.kind == LEAF_PINNATE || form.kind == LEAF_DIGITATE;
	const int n_eff = compound ? MAX(2, form.count / 3) : 1;
	if (compound && (compound_slot++ % n_eff) != 0) {
		return;
	}
	const float slot_area = 2.0f * 0.656f * hw * p_size * float(n_eff); // Lens area: 2 W L x 0.656.
	// The twin: today's lens (n_eff slots of it), for LOD 1 and coarser.
	{
		const float k = Math::sqrt(float(n_eff));
		const int ti = leaf_twins.indices.size(), tv = leaf_twins.verts.size();
		_add_blade(leaf_twins, start, along, side, n, p_size * k, hw * k, 3, 0, 0.0f, p_color);
		leaf_twin_of[int(r_b.parts.size())] = Vector4i(ti, leaf_twins.indices.size() - ti, tv, leaf_twins.verts.size() - tv);
	}
	switch (form.kind) {
		case LEAF_LOBED: {
			// Lobes widen the blade: the same area with lobes a little wider than the lens.
			const float w = hw * (1.0f + 0.5f * form.depth);
			// Rounded lobes end in a terminal lobe (one more shoulder row before the tip), on an
			// obovate blade, widest above the middle, as in oak.
			const int steps = form.round ? 3 * form.count + 2 : 2 * form.count + 1;
			_add_blade(r_b, start, along, side, n, p_size, w, steps, form.count, form.depth, p_color, form.round, form.round ? 1.3f : 0.75f);
		} break;
		case LEAF_PALMATE: {
			// Lobes fanned from the petiole: the middle one longest, the outer ones short. Each
			// is narrow; together they make the palm.
			const int k = form.count;
			const float spread = Math::deg_to_rad(k > 3 ? 75.0f : 50.0f);
			for (int i = 0; i < k; i++) {
				const float f = k > 1 ? float(i) / (k - 1) * 2.0f - 1.0f : 0.0f;
				const float a = f * spread;
				const Vector3 dir = (along * Math::cos(a) + side * Math::sin(a)).normalized();
				const Vector3 sd = n.cross(dir).normalized();
				const float len = p_size * Math::lerp(1.0f, 0.55f, Math::abs(f)) * 0.72f;
				_add_blade(r_b, start, dir, sd, n, len, len * 0.42f * (1.0f - 0.4f * form.depth), 2, 0, 0.0f, p_color);
			}
		} break;
		case LEAF_PINNATE: {
			// Leaflets in opposite pairs along the rachis, with one at the tip.
			const int pairs = form.count / 2;
			// Leaflets of 2 x 0.3 l x l x 0.656 each share the slots' area.
			const float leaflet = Math::sqrt(slot_area / (float(form.count) * 2.0f * 0.656f * 0.3f));
			const float rachis = leaflet * 0.6f * float(pairs + 1);
			for (int i = 0; i < pairs; i++) {
				const float t = Math::lerp(0.25f, 0.9f, pairs > 1 ? float(i) / (pairs - 1) : 0.5f);
				const Vector3 at = start + along * (rachis * t) + n * (-0.15f * rachis * t * t);
				for (int sgn = -1; sgn <= 1; sgn += 2) {
					const Vector3 dir = (side * float(sgn) * 0.85f + along * 0.5f).normalized();
					_add_blade(r_b, at, dir, n.cross(dir).normalized(), n, leaflet, leaflet * 0.3f, 2, 0, 0.0f, p_color);
				}
			}
			const Vector3 tip = start + along * rachis + n * (-0.15f * rachis);
			_add_blade(r_b, tip, along, side, n, leaflet, leaflet * 0.3f, 2, 0, 0.0f, p_color);
		} break;
		case LEAF_DIGITATE: {
			const int k = form.count;
			const float spread = Math::deg_to_rad(80.0f);
			// Obovate leaflets, widest toward the tip. Lengths fall from 1 in the middle to 0.5
			// outside: sum of squares about 0.56 k, scaled to the slots' area.
			const float unit = Math::sqrt(slot_area / (2.0f * 0.656f * 0.34f * 0.56f * float(k)));
			for (int i = 0; i < k; i++) {
				const float f = float(i) / (k - 1) * 2.0f - 1.0f;
				const float a = f * spread;
				const Vector3 dir = (along * Math::cos(a) + side * Math::sin(a)).normalized();
				const float len = unit * Math::lerp(1.0f, 0.5f, Math::abs(f));
				_add_blade(r_b, start + dir * (unit * 0.05f), dir, n.cross(dir).normalized(), n, len, len * 0.34f, 3, 0, 0.0f, p_color, false, 1.6f);
			}
		} break;
		default:
			break;
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
// Bark families by species (docs/ideas/trees.md section 2a).
TreeGenerator::BarkKind TreeGenerator::_bark_kind(Preset p_preset) {
	switch (p_preset) {
		case PRESET_BEECH:
		case PRESET_AMERICAN_BEECH:
		case PRESET_HAZEL:
		case PRESET_ROWAN:
		case PRESET_HOLLY:
		case PRESET_BOX:
		case PRESET_RED_MAPLE:
		case PRESET_WITCH_HAZEL:
			return BARK_SMOOTH;
		case PRESET_SCOTS_PINE:
		case PRESET_WHITE_PINE:
		case PRESET_LARCH:
			return BARK_PLATED;
		case PRESET_NORWAY_SPRUCE:
		case PRESET_HEMLOCK:
		case PRESET_SILVER_FIR:
		case PRESET_HAWTHORN:
		case PRESET_YEW:
		case PRESET_MOUNTAIN_LAUREL:
			return BARK_SCALY;
		case PRESET_SYCAMORE_MAPLE:
		case PRESET_HORSE_CHESTNUT:
			return BARK_FLAKING;
		case PRESET_BIRCH:
		case PRESET_PAPER_BIRCH:
		case PRESET_YELLOW_BIRCH:
			return BARK_PAPERY;
		default:
			return BARK_FURROWED; // Oaks, lime, ash, alder, willow, elder, sugar maple.
	}
}

namespace {
// Tileable value noise: lattice of p_period cells over [0, 1), wrapped.
float bark_hash(int p_x, int p_y, uint32_t p_salt) {
	uint32_t h = uint32_t(p_x) * 374761393u + uint32_t(p_y) * 668265263u + p_salt * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float((h ^ (h >> 16)) & 0xffffffu) / float(0xffffff);
}

float bark_value(float p_x, float p_y, int p_px, int p_py, uint32_t p_salt) {
	const float x = p_x * p_px, y = p_y * p_py;
	const int ix = int(Math::floor(x)), iy = int(Math::floor(y));
	const float fx = x - ix, fy = y - iy;
	const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
	auto at = [&](int i, int j) { return bark_hash(((i % p_px) + p_px) % p_px, ((j % p_py) + p_py) % p_py, p_salt); };
	const float a = Math::lerp(at(ix, iy), at(ix + 1, iy), sx);
	const float b = Math::lerp(at(ix, iy + 1), at(ix + 1, iy + 1), sx);
	return Math::lerp(a, b, sy);
}

// Fractal sum, -0.5..0.5, periods doubling from (p_px, p_py).
float bark_fbm(float p_x, float p_y, int p_px, int p_py, int p_octaves, uint32_t p_salt) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < p_octaves; o++) {
		sum += amp * (bark_value(p_x, p_y, p_px << o, p_py << o, p_salt + o) - 0.5f);
		norm += amp;
		amp *= 0.5f;
	}
	return sum / norm * 0.5f;
}

// Wrapped Voronoi in cell units (cells p_cu by p_cv over the tile, so cells stretch with the
// tile's aspect): distance to the nearest site, to the edge between the two nearest, the nearest
// cell's id and its site.
struct BarkCell {
	float f1 = 1e9f;
	float edge = 1e9f;
	uint32_t id = 0;
	uint32_t across = 0; // The neighbour across the nearest edge.
	Vector2 site;
};

// A wall between two cells cracks unless the pair is one of the share p_merge that never split;
// with p_scar > 0 only walls between a scar cell (hash over p_scar) and a whole one crack.
bool bark_cracks(uint32_t p_a, uint32_t p_b, float p_merge, float p_scar, uint32_t p_salt) {
	if (p_scar > 0.0f) {
		return (bark_hash(int(p_a), 5, p_salt) > p_scar) != (bark_hash(int(p_b), 5, p_salt) > p_scar);
	}
	return bark_hash(int(MIN(p_a, p_b)), int(MAX(p_a, p_b)), p_salt + 77u) >= p_merge;
}

BarkCell bark_voronoi(float p_x, float p_y, int p_cu, int p_cv, uint32_t p_salt, float p_merge = 0.0f, float p_scar = 0.0f, uint32_t p_msalt = 0) {
	const float x = p_x * p_cu, y = p_y * p_cv;
	const int ix = int(Math::floor(x)), iy = int(Math::floor(y));
	Vector2 sites[25];
	uint32_t ids[25];
	int n = 0;
	for (int j = -2; j <= 2; j++) {
		for (int i = -2; i <= 2; i++) {
			const int cx = ((ix + i) % p_cu + p_cu) % p_cu, cy = ((iy + j) % p_cv + p_cv) % p_cv;
			const float jx = bark_hash(cx, cy, p_salt), jy = bark_hash(cx, cy, p_salt + 7u);
			sites[n] = Vector2(ix + i + jx, iy + j + jy);
			ids[n] = uint32_t(cy * p_cu + cx);
			n++;
		}
	}
	const Vector2 p(x, y);
	int a = 0;
	for (int k = 1; k < n; k++) {
		if (p.distance_squared_to(sites[k]) < p.distance_squared_to(sites[a])) {
			a = k;
		}
	}
	BarkCell c;
	c.f1 = p.distance_to(sites[a]);
	c.id = ids[a];
	c.site = sites[a];
	// Distance to the bisector with each other site (exact distance to the cell's edge).
	for (int k = 0; k < n; k++) {
		if (k == a) {
			continue;
		}
		const Vector2 d = sites[k] - sites[a];
		const float len = d.length();
		if (len < 1e-6f) {
			continue;
		}
		const Vector2 mid = (sites[a] + sites[k]) * 0.5f;
		const float e = (mid - p).dot(d / len);
		if (e < c.edge && bark_cracks(ids[a], ids[k], p_merge, p_scar, p_msalt)) {
			c.edge = e;
			c.across = ids[k];
		}
	}
	return c;
}
} // namespace

// A tileable bark set (albedo multiplier, tangent-space normal, roughness, height in metres) for
// one family, BARK_TILE metres square. Cracks follow Lefebvre and Neyret 2002 in spirit: a
// stiff outer layer split into plates whose shape (long ridges, plates, scales, flakes) is the
// family's, with depth from the species' bark thickness.
const TreeGenerator::BarkSet &TreeGenerator::_bark_set(BarkKind p_kind) {
	BarkSet &set = bark_sets[p_kind];
	if (set.albedo.is_valid()) {
		return set;
	}
	const int N = 512;
	const float texel = BARK_TILE / N;
	struct Look {
		int cu, cv; // Plates around and along one tile.
		float crack; // Crack half-width, in cell units.
		float depth; // Metres from plate top to crack floor.
		float grain; // Fibre and surface noise, metres.
		float dark; // Albedo in the cracks.
		float mottle; // Albedo variation over plates.
	};
	static const Look LOOKS[BARK_MAX] = {
		{ 6, 6, 0.0f, 0.0006f, 0.0004f, 1.0f, 0.06f }, // Smooth: no plates, fine grain, lenticels.
		{ 6, 3, 0.1f, 0.028f, 0.004f, 0.3f, 0.12f }, // Furrowed: long interlaced ridges.
		{ 4, 2, 0.16f, 0.03f, 0.0025f, 0.25f, 0.2f }, // Plated: wide flat plates, deep furrows.
		{ 12, 9, 0.06f, 0.006f, 0.0015f, 0.45f, 0.14f }, // Scaly: small overlapping scales.
		{ 5, 4, 0.03f, 0.003f, 0.001f, 0.6f, 0.0f }, // Flaking: shallow edges, colour mosaic.
		{ 3, 40, 0.0f, 0.0008f, 0.0004f, 1.0f, 0.04f }, // Papery: smooth, lenticel bands.
	};
	const Look &L = LOOKS[p_kind];
	const uint32_t salt = 101u + uint32_t(p_kind) * 31u;
	Vector<float> h;
	h.resize(N * N);
	Vector<float> alb;
	alb.resize(N * N * 3);
	Vector<float> rough;
	rough.resize(N * N);
	float *hw = h.ptrw();
	float *aw = alb.ptrw();
	float *rw = rough.ptrw();
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
			const int k = y * N + x;
			// Fibres run along the stem: noise stretched along v.
			const float fibre = bark_fbm(u, v, 48, 6, 3, salt + 3u);
			float height = L.grain * (fibre + 0.5f * bark_fbm(u, v, 16, 16, 4, salt + 9u));
			Color a(1, 1, 1);
			float r = 0.88f + 0.08f * fibre;
			if (L.crack > 0.0f) {
				// Warp the lookup so ridges interlace and wander rather than sit on a grid.
				const float warp = p_kind == BARK_PLATED ? 2.0f : (p_kind == BARK_FLAKING ? 10.0f : 1.0f);
				const float wu = u + 0.035f * warp * bark_fbm(u, v, 4, 4, 2, salt + 21u);
				const float wv = v + 0.05f * warp * bark_fbm(u, v, 4, 4, 2, salt + 27u);
				const float merge = p_kind == BARK_PLATED ? 0.35f : (p_kind == BARK_SCALY ? 0.1f : 0.0f);
				const float scar_cut = p_kind == BARK_FLAKING ? 0.62f : 0.0f;
				const BarkCell c = bark_voronoi(wu - Math::floor(wu), wv - Math::floor(wv), L.cu, L.cv, salt, merge, scar_cut, salt + 43u);
				// Cracks open unevenly: wider where the layer split first. Some cell walls never
				// cracked, so plates join into larger irregular shapes, and the plate rims are
				// ragged rather than straight (what makes a Voronoi read as tiles).
				const float crack_w = L.crack * (0.5f + 2.0f * (bark_fbm(u, v, 6, 3, 2, salt + 33u) + 0.5f));
				const float ragged = c.edge + 0.12f * bark_fbm(u, v, 24, 24, 3, salt + 81u) + (p_kind == BARK_FLAKING ? 1.2f * bark_fbm(u, v, 8, 8, 2, salt + 83u) : 0.0f);
				float open = Math::smoothstep(0.0f, crack_w, ragged);
				// Flaking bark is mostly smooth: only a fallen flake leaves an edge, around a shallow
				// scar of fresh, warmer inner bark (sycamore, plane, horse chestnut).
				const bool scar = scar_cut > 0.0f && bark_hash(int(c.id), 5, salt + 43u) > scar_cut; // 0 in the crack, 1 on the plate.
				float plate = open;
				if (p_kind == BARK_FURROWED) {
					// Furrows are the zero lines of noise stretched along the stem: long, wavy, merging
					// and splitting like an oak's interlaced ridges. The cells only add the shallower
					// cross breaks that cut ridges into lengths.
					const float n = bark_fbm(wu, wv, L.cu, 1, 3, salt + 51u);
					const float furrow = Math::smoothstep(0.0f, 0.18f, Math::abs(n));
					const float cross = Math::lerp(1.0f, open, 0.55f);
					open = MIN(furrow, cross);
					plate = Math::sqrt(open); // Rounded ridge tops.
				} else if (p_kind == BARK_PLATED) {
					// Flat plate tops, steep walls, and each plate built of thin flaking layers.
					const float layers = Math::floor((bark_fbm(u, v, 12, 6, 3, salt + 57u) + 0.5f) * 4.0f) / 4.0f;
					plate = Math::smoothstep(0.0f, 0.5f, open) * (0.75f + 0.25f * layers);
				} else if (p_kind == BARK_SCALY) {
					// Shingles: each scale lifts toward its lower edge, over the next one down.
					const float below = (wv - Math::floor(wv)) * L.cv - c.site.y; // -1..1 cell units
					plate = open * (0.55f + 0.45f * CLAMP(0.5f + below, 0.0f, 1.0f));
				}
				// Plate tone drifts smoothly (a per-cell tone outlines joined plates).
				const float plate_noise = 0.5f + bark_fbm(u, v, 5, 3, 2, salt + 41u) * 1.6f;
				height += L.depth * (plate - 1.0f) + L.depth * 0.15f * (plate_noise - 0.5f) * plate;
				const float shade = Math::lerp(L.dark, 1.0f + L.mottle * (plate_noise - 0.5f), plate);
				a = Color(shade, shade, shade);
				r = Math::lerp(1.0f, r, plate);
				if (scar) {
					const float t = 0.5f + bark_fbm(u, v, 4, 4, 2, salt + 47u) * 1.6f;
					a = Color(1.08f + 0.12f * t, 0.9f + 0.06f * t, 0.74f - 0.08f * t) * shade;
					height -= L.depth * 0.5f * open;
				}
			}
			if (p_kind == BARK_SMOOTH || p_kind == BARK_PAPERY) {
				// Lenticels: short dashes across the stem (long and dark on birch).
				const int cu = p_kind == BARK_PAPERY ? 5 : 14, cv = p_kind == BARK_PAPERY ? 26 : 24;
				const float lx = u * cu, ly = v * cv;
				const int ix = int(lx), iy = int(ly);
				const float on = bark_hash(ix, iy, salt + 61u);
				if (on > (p_kind == BARK_PAPERY ? 0.62f : 0.6f)) {
					const float cx = 0.5f + 0.3f * (bark_hash(ix, iy, salt + 63u) - 0.5f);
					const float len = p_kind == BARK_PAPERY ? 0.1f + 0.4f * bark_hash(ix, iy, salt + 65u) : 0.1f + 0.08f * on;
					const float dx = (lx - ix - cx) / len, dy = (ly - iy - 0.5f) / 0.18f;
					const float m = 1.0f - Math::smoothstep(0.6f, 1.0f, dx * dx + dy * dy);
					const float lent = p_kind == BARK_PAPERY ? 0.18f : 0.7f;
					a = a * Math::lerp(1.0f, lent, m);
					height += m * 0.0004f;
					r = Math::lerp(r, 0.97f, m);
				}
				const float tone = 1.0f + L.mottle * bark_fbm(u, v, 3, 6, 3, salt + 71u) * 2.0f;
				a = a * tone;
			}
			hw[k] = height;
			aw[k * 3 + 0] = a.r;
			aw[k * 3 + 1] = a.g;
			aw[k * 3 + 2] = a.b;
			rw[k] = CLAMP(r, 0.0f, 1.0f);
		}
	}
	Vector<uint8_t> abytes, nbytes, rbytes;
	abytes.resize(N * N * 3);
	nbytes.resize(N * N * 3);
	rbytes.resize(N * N * 3);
	Vector<uint8_t> hbytes;
	hbytes.resize(N * N * 4);
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const int k = y * N + x;
			for (int c = 0; c < 3; c++) {
				// The albedo multiplies the vertex colour (species and weathering), so it is stored
				// as a linear factor encoded to sRGB for the albedo slot.
				const float f = CLAMP(aw[k * 3 + c] * 0.92f, 0.0f, 1.0f);
				abytes.write[k * 3 + c] = uint8_t(Math::round(Color(f, f, f).linear_to_srgb().r * 255.0f));
			}
			const float hx = hw[y * N + (x + 1) % N] - hw[y * N + (x + N - 1) % N];
			const float hy = hw[((y + 1) % N) * N + x] - hw[((y + N - 1) % N) * N + x];
			// OpenGL convention (Godot): +y up the image is +v, so the y slope flips.
			Vector3 n = Vector3(-hx / (2.0f * texel), hy / (2.0f * texel), 1.0f).normalized();
			nbytes.write[k * 3 + 0] = uint8_t(Math::round((n.x * 0.5f + 0.5f) * 255.0f));
			nbytes.write[k * 3 + 1] = uint8_t(Math::round((n.y * 0.5f + 0.5f) * 255.0f));
			nbytes.write[k * 3 + 2] = uint8_t(Math::round((n.z * 0.5f + 0.5f) * 255.0f));
			// ORM layout (occlusion 1, roughness, metal 0): the path tracer reads a roughness
			// texture as ORM, so a one-channel map would read as roughness 0 there.
			rbytes.write[k * 3 + 0] = 255;
			rbytes.write[k * 3 + 1] = uint8_t(Math::round(rw[k] * 255.0f));
			rbytes.write[k * 3 + 2] = 0;
			memcpy(hbytes.ptrw() + k * 4, &hw[k], 4);
		}
	}
	Ref<Image> ai = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, abytes);
	ai->generate_mipmaps();
	Ref<Image> ni = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, nbytes);
	ni->generate_mipmaps(true);
	Ref<Image> ri = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, rbytes);
	ri->generate_mipmaps();
	set.albedo = ImageTexture::create_from_image(ai);
	set.normal = ImageTexture::create_from_image(ni);
	set.roughness = ImageTexture::create_from_image(ri);
	set.height = Image::create_from_data(N, N, false, Image::FORMAT_RF, hbytes);
	return set;
}

// Epiphytic moss on bark, by species. Bryophyte cover follows bark texture and pH and its water
// holding: high on rough, nutrient-rich bark (ash, sycamore, elder, oak, alder, willow), low on
// acid or shedding bark (pine, birch, larch) and on the smooth, fast-drying bark of beech except
// at the foot (Barkman, Phytosociology and Ecology of Cryptogamic Epiphytes, 1958).
static const float MOSS_AFFINITY[TreeGenerator::PRESET_MAX] = {
	0.8f, // Oak.
	0.55f, // Beech.
	0.3f, // Birch.
	0.2f, // Scots pine.
	0.4f, // Norway spruce.
	0.7f, // Hazel.
	0.5f, // Hawthorn.
	0.6f, // Lime.
	0.85f, // Sycamore maple.
	0.8f, // Ash.
	0.5f, // Horse chestnut.
	0.9f, // Black alder.
	0.85f, // White willow.
	0.3f, // Larch.
	0.5f, // Silver fir.
	0.5f, // Rowan.
	0.8f, // Elder.
	0.3f, // Holly.
	0.4f, // Yew.
	0.3f, // Box.
	0.75f, // Sugar maple.
	0.6f, // Red maple.
	0.6f, // Red oak.
	0.7f, // White oak.
	0.3f, // White pine.
	0.6f, // Hemlock.
	0.5f, // American beech.
	0.25f, // Paper birch.
	0.6f, // Yellow birch.
	0.6f, // Witch hazel.
	0.3f, // Mountain laurel.
};

float TreeGenerator::_moss_amount() const {
	if (moss_cover <= 0.0f) {
		return 0.0f;
	}
	// Open-grown trees dry in sun and wind; stand-grown ones sit in the stand's humid shade.
	const float shade = Math::lerp(0.65f, 1.0f, CLAMP(forest_shade / 0.6f, 0.0f, 1.0f));
	// Dead wood holds water as it softens and is colonised from the ground up (Soderstrom,
	// Bryophyte succession on decaying logs, 1988): mossier with each decay class.
	static const float DECAY[6] = { 1.0f, 0.8f, 1.1f, 1.3f, 1.3f, 1.4f };
	float k = moss_cover * MOSS_AFFINITY[preset] * shade * DECAY[CLAMP(decay_class, 0, 5)];
	if (fallen || fallen_branch) {
		k *= 1.3f; // Wood on the ground sits in the litter's moisture.
	}
	return k;
}

namespace {
float moss_noise(const Vector3 &p_p, uint32_t p_salt) {
	const int ix = int(Math::floor(p_p.x)), iy = int(Math::floor(p_p.y)), iz = int(Math::floor(p_p.z));
	const float fx = p_p.x - ix, fy = p_p.y - iy, fz = p_p.z - iz;
	const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy), sz = fz * fz * (3.0f - 2.0f * fz);
	auto at = [&](int i, int j, int k) { return bark_hash(i, j, p_salt + uint32_t(k) * 2654435761u); };
	auto plane = [&](int k) {
		return Math::lerp(Math::lerp(at(ix, iy, k), at(ix + 1, iy, k), sx), Math::lerp(at(ix, iy + 1, k), at(ix + 1, iy + 1, k), sx), sy);
	};
	return Math::lerp(plane(iz), plane(iz + 1), sz);
}
} // namespace

// Moss weight 0..1 at a bark point: a skirt at the foot of the stem where stemflow and splash
// keep the bark wet, the upper faces of level limbs and logs where water and litter settle, broken
// into patches (Barkman 1958). Aspect is left out: placed trees turn about their axis.
float TreeGenerator::_moss_at(const Vector3 &p_pos, const Vector3 &p_normal, float p_ground, float p_amount) const {
	const float h = MAX(p_pos.y - p_ground, 0.0f);
	const float foot = Math::exp(-h / 1.1f);
	const float up = p_normal.y;
	const float top = CLAMP((up - 0.35f) / 0.5f, 0.0f, 1.0f) * 0.75f;
	const float patch = 0.65f * moss_noise(p_pos * 2.2f, 0x6d6f7373u + uint32_t(seed)) + 0.35f * moss_noise(p_pos * 7.0f, 0x6d6f7374u);
	return CLAMP(p_amount * MAX(foot, top) * (1.9f * patch - 0.25f), 0.0f, 1.0f);
}

// Moss texture over one bark tile: a turf of tiny shoots (2-4 mm) gathered into cushions a few
// centimetres across, dark in the gaps. Albedo multiplies the vertex colour, which carries the
// green; the height also lifts the shell into cushions.
const TreeGenerator::BarkSet &TreeGenerator::_moss_set() {
	if (moss_set.albedo.is_valid()) {
		return moss_set;
	}
	const int N = 512;
	const float texel = BARK_TILE / N;
	Vector<float> h;
	h.resize(N * N);
	float *hw = h.ptrw();
	Vector<uint8_t> abytes, nbytes, rbytes, hbytes;
	abytes.resize(N * N * 3);
	nbytes.resize(N * N * 3);
	rbytes.resize(N * N * 3);
	hbytes.resize(N * N * 4);
	Vector<float> tone;
	tone.resize(N * N);
	float *tw = tone.ptrw();
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
			// Cushions about 4 cm across (10 per tile), shoots about 3 mm (128 per tile).
			// Domain-warped so the value-noise lattice does not show as a grid of dots.
			const float wu = u + 0.012f * bark_fbm(u, v, 24, 24, 2, 517u), wv = v + 0.012f * bark_fbm(u, v, 24, 24, 2, 519u);
			const float cushion = bark_fbm(wu, wv, 10, 10, 3, 501u) + 0.5f;
			const float shoots = bark_fbm(wu, wv, 96, 96, 3, 509u) + 0.5f;
			const float tips = CLAMP((bark_fbm(wv, wu, 192, 192, 2, 513u) + 0.5f - 0.55f) * 4.0f, 0.0f, 1.0f);
			const float c = CLAMP((cushion - 0.3f) / 0.5f, 0.0f, 1.0f);
			hw[y * N + x] = 0.006f * c + 0.0025f * shoots + 0.001f * tips;
			// Shoot tips are paler and yellower; the turf between them and the gaps between
			// cushions are dark.
			tw[y * N + x] = 0.55f + 0.35f * shoots + 0.25f * tips - 0.35f * (1.0f - c);
		}
	}
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const int k = y * N + x;
			const float t = CLAMP(tw[k], 0.15f, 1.15f);
			const float r = t * 1.1f, g = t, b = t * 0.8f;
			const float f[3] = { r, g, b };
			for (int c = 0; c < 3; c++) {
				const float fc = CLAMP(f[c] * 0.85f, 0.0f, 1.0f);
				abytes.write[k * 3 + c] = uint8_t(Math::round(Color(fc, fc, fc).linear_to_srgb().r * 255.0f));
			}
			const float hx = hw[y * N + (x + 1) % N] - hw[y * N + (x + N - 1) % N];
			const float hy = hw[((y + 1) % N) * N + x] - hw[((y + N - 1) % N) * N + x];
			Vector3 n = Vector3(-hx / (2.0f * texel), hy / (2.0f * texel), 1.0f).normalized();
			nbytes.write[k * 3 + 0] = uint8_t(Math::round((n.x * 0.5f + 0.5f) * 255.0f));
			nbytes.write[k * 3 + 1] = uint8_t(Math::round((n.y * 0.5f + 0.5f) * 255.0f));
			nbytes.write[k * 3 + 2] = uint8_t(Math::round((n.z * 0.5f + 0.5f) * 255.0f));
			// Moss is matte: a turf of shoots scatters light in every direction.
			rbytes.write[k * 3 + 0] = 255;
			rbytes.write[k * 3 + 1] = uint8_t(Math::round(CLAMP(0.92f - 0.1f * (tw[k] - 0.6f), 0.7f, 1.0f) * 255.0f));
			rbytes.write[k * 3 + 2] = 0;
			memcpy(hbytes.ptrw() + k * 4, &hw[k], 4);
		}
	}
	Ref<Image> ai = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, abytes);
	ai->generate_mipmaps();
	Ref<Image> ni = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, nbytes);
	ni->generate_mipmaps(true);
	Ref<Image> ri = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, rbytes);
	ri->generate_mipmaps();
	moss_set.albedo = ImageTexture::create_from_image(ai);
	moss_set.normal = ImageTexture::create_from_image(ni);
	moss_set.roughness = ImageTexture::create_from_image(ri);
	moss_set.height = Image::create_from_data(N, N, false, Image::FORMAT_RF, hbytes);
	return moss_set;
}

// A climber's stem: a thin tube along a path, in pieces so each piece follows the bone of the
// host where it lies; it tapers from p_r0 to p_r1 and drops out with the twigs at coarser LODs.
void TreeGenerator::_climber_stem(Builder &r_bark, const LocalVector<Vector3> &p_path, const LocalVector<int> &p_bones, float p_r0, float p_r1, const Color &p_color) {
	const int n = int(p_path.size());
	if (n < 2) {
		return;
	}
	const int piece = 12;
	for (int a = 0; a < n - 1; a += piece) {
		const int b = MIN(a + piece, n - 1);
		LocalVector<Vector3> pts;
		LocalVector<float> rad;
		for (int i = a; i <= b; i++) {
			pts.push_back(p_path[i]);
			rad.push_back(Math::lerp(p_r0, p_r1, float(i) / float(n - 1)));
		}
		const int sides = rad[0] > 0.006f ? 5 : (rad[0] > 0.003f ? 4 : 3);
		r_bark.bone = p_bones[a];
		r_bark.role = Builder::ROLE_ALL;
		Vector4i part;
		r_bark.begin(part);
		_tube(r_bark, pts, rad, sides, 0.0f, p_color);
		r_bark.end(part, rad[0]);
	}
}

void TreeGenerator::_climber_leaf(Builder &r_leaves, const Vector3 &p_at, const Vector3 &p_out, const Vector3 &p_normal, float p_size, int p_bone, const Color &p_color) {
	// A climber's leaf is about as broad as long (ivy, clematis leaflets, honeysuckle), whatever
	// the host's leaf width.
	const float saved_width = params[PARAM_LEAF_WIDTH];
	params[PARAM_LEAF_WIDTH] = 0.42f;
	r_leaves.bone = p_bone;
	Vector4i part;
	r_leaves.begin(part);
	species_leaf_forms = false;
	_add_leaf(r_leaves, p_at, p_out, p_normal, p_size, p_color);
	species_leaf_forms = true;
	r_leaves.end(part, 0.0f);
	params[PARAM_LEAF_WIDTH] = saved_width;
	leaf_count++;
	// Published for vegetation's climber leaf meshes: position, blade normal, petiole direction,
	// size, light.
	const float vals[11] = { p_at.x, p_at.y, p_at.z, p_normal.x, p_normal.y, p_normal.z, p_out.x, p_out.y, p_out.z, p_size, 0.5f };
	for (float v : vals) {
		climber_leaves.push_back(v);
	}
}

// Climbers over the host's own wood (section 5b). Ivy follows Luft's Ivy Generator (2007): a
// tip that keeps to the surface it climbs, steered by a random walk and upward phototropism,
// branching as it goes, with leaves along it; the support here is the analytic stem tube, so
// staying on the surface is exact. Lianas hang from limbs or loop between two of them as
// catenaries (the unsupported stems of Hadrich, Benes, Deussen and Pirk, 2017). Honeysuckle
// twines in helices round thin stems.
void TreeGenerator::_grow_climbers(Builder &r_bark, Builder &r_leaves) {
	if (supports.is_empty()) {
		return;
	}
	RandomPCG cr;
	cr.seed(uint64_t(seed) * 1013904223u + uint64_t(climber) * 31u + 7u);
	auto gauss = [&]() { return (cr.randf() + cr.randf() + cr.randf() - 1.5f) * 1.414f; };
	const Vector3 world_up(0, 1, 0);
	// Per support: arc length at each point, tangents and a transported normal (as _tube does).
	struct Frames {
		LocalVector<float> s;
		LocalVector<Vector3> t;
		LocalVector<Vector3> n;
	};
	LocalVector<Frames> frames;
	frames.resize(supports.size());
	for (uint32_t k = 0; k < supports.size(); k++) {
		const Support &sp = supports[k];
		Frames &f = frames[k];
		const int m = int(sp.pts.size());
		Vector3 t = m > 1 ? (sp.pts[1] - sp.pts[0]).normalized() : world_up;
		Vector3 nrm = t.cross(Math::abs(t.y) < 0.9f ? world_up : Vector3(1, 0, 0)).normalized();
		float acc = 0.0f;
		for (int i = 0; i < m; i++) {
			Vector3 t2 = t;
			if (m > 1) {
				t2 = (sp.pts[MIN(i + 1, m - 1)] - sp.pts[MAX(i - 1, 0)]).normalized();
			}
			const Vector3 axis = t.cross(t2);
			if (axis.length_squared() > 1e-10f) {
				nrm = nrm.rotated(axis.normalized(), Math::acos(CLAMP(t.dot(t2), -1.0f, 1.0f)));
			}
			nrm = (nrm - t2 * nrm.dot(t2)).normalized();
			t = t2;
			if (i > 0) {
				acc += sp.pts[i].distance_to(sp.pts[i - 1]);
			}
			f.s.push_back(acc);
			f.t.push_back(t);
			f.n.push_back(nrm);
		}
	}
	// A point on support k at arc length p_s and angle p_theta, p_lift off the bark.
	struct Surf {
		Vector3 pos, out, t;
		float r = 0.0f;
		int bone = 0;
	};
	auto surf = [&](int k, float p_s, float p_theta, float p_lift) -> Surf {
		const Support &sp = supports[k];
		const Frames &f = frames[k];
		const int m = int(sp.pts.size());
		int i = 0;
		while (i + 2 < m && f.s[i + 1] < p_s) {
			i++;
		}
		const int j = MIN(i + 1, m - 1);
		const float len = MAX(f.s[j] - f.s[i], 1e-5f);
		const float u = CLAMP((p_s - f.s[i]) / len, 0.0f, 1.0f);
		Surf o;
		o.t = f.t[i].lerp(f.t[j], u).normalized();
		Vector3 nrm = f.n[i].lerp(f.n[j], u);
		nrm = (nrm - o.t * nrm.dot(o.t)).normalized();
		const Vector3 bin = o.t.cross(nrm);
		o.out = nrm * Math::cos(p_theta) + bin * Math::sin(p_theta);
		o.r = Math::lerp(sp.rad[i], sp.rad[j], u);
		o.pos = sp.pts[i].lerp(sp.pts[j], u) + o.out * (o.r + p_lift);
		o.bone = u < 0.5f ? sp.bone[i] : sp.bone[j];
		return o;
	};
	const float ground = nodes[0].start.y;

	if (climber == CLIMBER_IVY) {
		const Color stem_col(0.2f, 0.17f, 0.13f);
		struct Shoot {
			int k;
			float s, theta, phi, length, r0;
			int depth;
		};
		LocalVector<Shoot> todo;
		for (uint32_t k = 0; k < supports.size(); k++) {
			if (!supports[k].stem) {
				continue;
			}
			const float total = frames[k].s[frames[k].s.size() - 1];
			// From the foot up to where the crown starts, further with more cover (ivy reaches
			// the crown of old trees; it thins where the bark is in full light).
			const float reach = MIN(total * 0.95f, Math::lerp(1.2f, MAX(crown_base, 2.0f) + 0.3f * (height - crown_base), climber_cover));
			const float side = cr.randf() * float(Math::TAU);
			const int count = 3 + int(climber_cover * 10.0f);
			for (int c = 0; c < count; c++) {
				todo.push_back({ int(k), 0.12f + 0.1f * cr.randf(), side + Math::lerp(-1.7f, 1.7f, cr.randf()), 0.0f, reach * Math::lerp(0.55f, 1.0f, cr.randf()), Math::lerp(0.006f, 0.011f, cr.randf()), 0 });
			}
		}
		const float step = 0.03f;
		int shoots = 0;
		// First in, first out: every main shoot grows before the budget goes to side shoots.
		for (uint32_t next = 0; next < todo.size() && shoots < 120; next++) {
			Shoot sh = todo[next];
			shoots++;
			LocalVector<Vector3> path;
			LocalVector<int> bones;
			const float s_end = MIN(sh.s + sh.length, frames[sh.k].s[frames[sh.k].s.size() - 1] - 0.05f);
			int leaf_side = 1;
			for (int st = 0; sh.s < s_end && st < 4000; st++) {
				const Surf p = surf(sh.k, sh.s, sh.theta, 0.006f);
				path.push_back(p.pos);
				bones.push_back(p.bone);
				if (st > 0) {
					// Leaves on petioles off the shaded face, blades turned out and up to the light,
					// larger low down (shade leaves).
					const float h = p.pos.y - ground;
					const float size = Math::lerp(0.095f, 0.055f, CLAMP(h / 12.0f, 0.0f, 1.0f)) * Math::lerp(0.75f, 1.15f, cr.randf());
					const Vector3 side_dir = p.out.cross(p.t).normalized() * float(leaf_side);
					leaf_side = -leaf_side;
					const Vector3 normal = (p.out * 0.85f + world_up * 0.35f).normalized();
					const Vector3 out = (p.out * 0.55f + side_dir * 0.6f - p.t * 0.35f).normalized();
					const float tone = Math::lerp(0.8f, 1.15f, cr.randf());
					_climber_leaf(r_leaves, p.pos + p.out * 0.015f, out, normal, size, p.bone, Color(0.045f * tone, 0.09f * tone, 0.025f * tone));
				}
				if (sh.depth < 3 && st > 4 && cr.randf() < 0.05f) {
					todo.push_back({ sh.k, sh.s, sh.theta, sh.phi + (cr.randf() < 0.5f ? 1.0f : -1.0f), (s_end - sh.s) * Math::lerp(0.3f, 0.7f, cr.randf()), sh.r0 * 0.6f, sh.depth + 1 });
				}
				sh.s += step * Math::cos(sh.phi);
				sh.theta += step * Math::sin(sh.phi) / MAX(p.r + 0.006f, 0.02f);
				sh.phi += 0.4f * gauss();
				sh.phi *= 0.95f;
				sh.phi = CLAMP(sh.phi, -1.3f, 1.3f);
			}
			_climber_stem(r_bark, path, bones, sh.r0, 0.0025f, stem_col);
		}
	} else if (climber == CLIMBER_LIANA) {
		const Color stem_col(0.26f, 0.21f, 0.15f);
		const Color leaf_col(0.05f, 0.085f, 0.025f);
		// Anchors on limbs above head height.
		LocalVector<Vector2i> anchors;
		for (uint32_t k = 0; k < supports.size(); k++) {
			const Support &sp = supports[k];
			if (sp.stem) {
				continue;
			}
			for (uint32_t i = 0; i < sp.pts.size(); i++) {
				if (sp.rad[i] >= 0.035f && sp.pts[i].y - ground > 2.5f) {
					anchors.push_back(Vector2i(k, i));
				}
			}
		}
		// Up the stem first: one or two woody stems straight up to the crown.
		for (uint32_t k = 0; k < supports.size(); k++) {
			if (!supports[k].stem) {
				continue;
			}
			const float total = frames[k].s[frames[k].s.size() - 1];
			const float reach = MIN(total * 0.9f, MAX(crown_base, 2.0f));
			const int count = 1 + int(climber_cover * 1.5f);
			for (int c = 0; c < count; c++) {
				LocalVector<Vector3> path;
				LocalVector<int> bones;
				float theta = cr.randf() * float(Math::TAU);
				for (float sa = 0.05f; sa < reach; sa += 0.08f) {
					theta += 0.04f * gauss();
					const Surf p = surf(k, sa, theta, 0.012f);
					path.push_back(p.pos);
					bones.push_back(p.bone);
				}
				_climber_stem(r_bark, path, bones, Math::lerp(0.012f, 0.02f, cr.randf()), 0.008f, stem_col);
			}
		}
		const int strands = anchors.is_empty() ? 0 : 3 + int(climber_cover * 20.0f);
		for (int c = 0; c < strands; c++) {
			const Vector2i a = anchors[cr.rand() % anchors.size()];
			const Vector3 pa = supports[a.x].pts[a.y];
			const int bone_a = supports[a.x].bone[a.y];
			LocalVector<Vector3> path;
			LocalVector<int> bones;
			// A loop to another limb 1.5 to 5 m away, else a strand hanging free.
			int pick = -1;
			for (int tries = 0; tries < 12 && pick < 0; tries++) {
				const int b = cr.rand() % anchors.size();
				const float d = supports[anchors[b].x].pts[anchors[b].y].distance_to(pa);
				if (anchors[b].x != a.x && d > 1.5f && d < 5.0f) {
					pick = b;
				}
			}
			const Vector3 side = Vector3(gauss(), 0.0f, gauss()).normalized();
			if (pick >= 0 && cr.randf() < 0.6f) {
				const Vector3 pb = supports[anchors[pick].x].pts[anchors[pick].y];
				const int bone_b = supports[anchors[pick].x].bone[anchors[pick].y];
				const float span = pa.distance_to(pb);
				const float sag = span * Math::lerp(0.2f, 0.5f, cr.randf());
				const int n = MAX(4, int(span / 0.08f));
				for (int i = 0; i <= n; i++) {
					const float t = float(i) / n;
					// A parabola is within a few per cent of the catenary at these sags.
					path.push_back(pa.lerp(pb, t) - world_up * (sag * 4.0f * t * (1.0f - t)) + side * (0.05f * Math::sin(t * float(Math::PI))));
					bones.push_back(t < 0.5f ? bone_a : bone_b);
				}
			} else {
				const float len = MIN(pa.y - ground - 0.4f, Math::lerp(1.0f, 6.0f, cr.randf()));
				const int n = MAX(3, int(len / 0.08f));
				for (int i = 0; i <= n; i++) {
					const float t = float(i) / n;
					path.push_back(pa - world_up * (len * t) + side * (0.25f * len * t * t));
					bones.push_back(bone_a);
				}
			}
			const float r0 = Math::lerp(0.004f, 0.012f, cr.randf());
			_climber_stem(r_bark, path, bones, r0, 0.003f, stem_col);
			// Opposite pairs of leaves, more toward the light at the strand's ends.
			for (uint32_t i = 2; i + 1 < path.size(); i += 2) {
				if (cr.randf() > 0.85f) {
					continue;
				}
				const Vector3 t = (path[i + 1] - path[i - 1]).normalized();
				Vector3 sdir = t.cross(world_up);
				if (sdir.length_squared() < 1e-4f) {
					sdir = Vector3(1, 0, 0);
				}
				sdir.normalize();
				for (int pair = -1; pair <= 1; pair += 2) {
					const Vector3 out = (sdir * float(pair) - world_up * 0.4f).normalized();
					const Vector3 normal = (world_up + sdir * float(pair) * 0.3f).normalized();
					_climber_leaf(r_leaves, path[i], out, normal, Math::lerp(0.04f, 0.065f, cr.randf()), bones[i], leaf_col);
				}
			}
		}
	} else if (climber == CLIMBER_HONEYSUCKLE) {
		const Color stem_col(0.3f, 0.22f, 0.16f);
		const Color leaf_col(0.045f, 0.08f, 0.03f);
		LocalVector<int> thin;
		for (uint32_t k = 0; k < supports.size(); k++) {
			if (supports[k].rad[0] <= 0.09f) {
				thin.push_back(k);
			}
		}
		const int vines = thin.is_empty() ? 0 : 1 + int(climber_cover * 5.0f);
		for (int c = 0; c < vines; c++) {
			const int k = thin[cr.rand() % thin.size()];
			const float total = frames[k].s[frames[k].s.size() - 1];
			const float len = MIN(total * 0.9f, Math::lerp(1.0f, 3.5f, cr.randf()));
			// Lonicera twines clockwise seen from above (right-handed), one turn per 12 cm or so.
			const float pitch = Math::lerp(0.1f, 0.16f, cr.randf());
			float theta = cr.randf() * float(Math::TAU);
			LocalVector<Vector3> path;
			LocalVector<int> bones;
			int pair_at = 0;
			for (float sa = 0.02f; sa < len; sa += 0.02f) {
				const Surf p = surf(k, sa, theta, 0.004f);
				theta -= 0.02f / pitch * float(Math::TAU);
				path.push_back(p.pos);
				bones.push_back(p.bone);
				if (++pair_at % 4 == 0) {
					for (int pair = -1; pair <= 1; pair += 2) {
						const Vector3 sdir = p.out.cross(p.t).normalized() * float(pair);
						const Vector3 out = (p.out * 0.6f + sdir * 0.6f).normalized();
						_climber_leaf(r_leaves, p.pos, out, (world_up + p.out * 0.4f).normalized(), Math::lerp(0.035f, 0.055f, cr.randf()), p.bone, leaf_col);
					}
				}
			}
			_climber_stem(r_bark, path, bones, 0.004f, 0.002f, stem_col);
		}
	}
	r_bark.bone = 0;
	r_leaves.bone = 0;
}

// Venation over a blade's UVs (u across, 0.5 the midrib; v base to tip), as a factor on the
// vertex colour: a pale midrib, secondaries running out toward the margin at about 45 degrees
// and fading there, a slightly darker margin band and fine mottling (after the pinnate
// patterns of Runions et al., Modeling and Visualization of Leaf Venation Patterns, 2005). One
// texture serves every blade, lobe and leaflet.
Ref<Texture2D> TreeGenerator::_leaf_texture() {
	if (leaf_texture.is_valid()) {
		return leaf_texture;
	}
	const int w = 128, h = 256;
	Vector<uint8_t> px;
	px.resize(w * h * 3);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			const float d = Math::abs(u - 0.5f) * 2.0f; // 0 at the midrib, 1 at the margin.
			float f = 0.86f + 0.06f * (bark_fbm(u, v, 8, 16, 3, 701u) * 2.0f);
			// Midrib, thinning toward the tip.
			const float mid_w = Math::lerp(0.035f, 0.01f, v);
			f = Math::lerp(f, 1.0f, CLAMP(1.0f - d / mid_w, 0.0f, 1.0f));
			// Secondaries: lines of constant v - 0.35 d, eight to a blade, fading at the margin.
			const float phase = (v - 0.35f * d) * 8.0f;
			const float line = Math::abs(phase - Math::round(phase));
			const float sec = CLAMP(1.0f - line / 0.06f, 0.0f, 1.0f) * (1.0f - Math::smoothstep(0.7f, 0.95f, d)) * (d > mid_w ? 1.0f : 0.0f);
			f = Math::lerp(f, 0.97f, sec * 0.8f);
			// Margin band.
			f *= 1.0f - 0.08f * Math::smoothstep(0.85f, 1.0f, d);
			const uint8_t b = uint8_t(Math::round(Color(f, f, f).linear_to_srgb().r * 255.0f));
			for (int c = 0; c < 3; c++) {
				px.write[(y * w + x) * 3 + c] = b;
			}
		}
	}
	Ref<Image> img = Image::create_from_data(w, h, false, Image::FORMAT_RGB8, px);
	img->generate_mipmaps();
	leaf_texture = ImageTexture::create_from_image(img);
	return leaf_texture;
}

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

// Deadwood (docs/ideas/trees.md section 5): the five decay classes foresters use for snags and
// logs (Maser, Anderson, Cromack, Williams and Martin 1979; Mellen and Ager 2002), applied to a
// grown tree so snags and logs match the living stand. 1: dead, twigs and bark intact. 2: twigs
// lost. 3: bark sloughing, top broken, small limbs gone. 4: only limb stubs, a soft stem broken
// lower. 5: a stump.
void TreeGenerator::_apply_decay() {
	RandomPCG dr;
	dr.seed(uint64_t(seed) * 2654435761u + uint64_t(decay_class) * 97u + 11u);
	static const float MIN_RADIUS[6] = { 0.0f, 0.0f, 0.012f, 0.03f, 0.0f, 0.0f };
	// A conifer dead a year or two keeps its needles, gone red-brown (the "red stage" of beetle-
	// killed spruce and pine; they fall in the second or third year). Broad leaves are shed by the
	// first winter.
	const bool red_needles = decay_class == 1 && params[PARAM_NEEDLES] > 0.5f;
	if (red_needles) {
		leaf_color = Color(0.3f, 0.13f, 0.05f);
	}
	for (uint32_t i = 0; i < nodes.size() && !red_needles; i++) {
		nodes[i].leafy = false;
		if (i < spurs.size()) {
			spurs[i] = 0;
		}
	}
	// A shed limb keeps its length: only its finest twigs drop (Harmon 1986), and no top breaks.
	const float min_r = MIN_RADIUS[decay_class] * (fallen_branch ? 0.35f : 1.0f);
	float cut_y = 1e9f;
	if (fallen_branch) {
		// The limb falls whole; class 4 and 5 stubs and stumps only apply to standing wood.
	} else if (decay_class == 3) {
		cut_y = height * Math::lerp(0.55f, 0.85f, dr.randf());
	} else if (decay_class == 4) {
		cut_y = height * Math::lerp(0.3f, 0.6f, dr.randf());
	} else if (decay_class == 5) {
		cut_y = Math::lerp(0.4f, 1.5f, dr.randf());
	}
	const float base_y = nodes[0].start.y;
	for (int i = 1; i < int(nodes.size()); i++) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		if (n.parent > 0 && nodes[n.parent].removed) {
			n.removed = true;
			continue;
		}
		const bool stem_node = n.order == 0;
		if (!stem_node) {
			const bool off_stem = nodes[n.parent].order == 0 && nodes[n.parent].main != i;
			if (!fallen_branch && (decay_class >= 5 || (decay_class == 4 && !off_stem))) {
				n.removed = true;
				continue;
			}
			if (decay_class == 4 && !fallen_branch) {
				// A stub: the first few decimetres of the limb, its tip snapped.
				n.length = MIN(n.length, Math::lerp(0.15f, 0.5f, dr.randf()));
				n.main = -1;
				n.first_lateral = -1;
			}
			if (n.radius < min_r || n.start.y - base_y > cut_y) {
				n.removed = true;
				continue;
			}
		} else if (n.start.y - base_y >= cut_y) {
			n.removed = true;
			continue;
		} else if (n.end().y - base_y > cut_y && n.dir.y > 0.05f) {
			// The break: this internode ends at the cut, with a splintered top.
			n.length = MAX((cut_y - (n.start.y - base_y)) / n.dir.y, 0.02f);
			n.main = -1;
			broken_tip = i;
		}
	}
	// Anything whose parent went is gone too (children come after their parents).
	for (int i = 1; i < int(nodes.size()); i++) {
		if (!nodes[i].removed && nodes[i].parent > 0 && nodes[nodes[i].parent].removed) {
			nodes[i].removed = true;
		}
	}
	height = 0.0f;
	for (const Node &n : nodes) {
		if (!n.removed) {
			height = MAX(height, n.end().y - base_y);
		}
	}
	// Bark greys as it weathers, then sloughs off to pale, then darkening, wood.
	static const Color WEATHERED[6] = { Color(), Color(0.33f, 0.3f, 0.26f), Color(0.4f, 0.38f, 0.34f), Color(0.42f, 0.39f, 0.33f),
		Color(0.42f, 0.36f, 0.28f), Color(0.3f, 0.25f, 0.19f) };
	bark_color = bark_color.lerp(WEATHERED[decay_class], decay_class >= 3 ? 0.85f : 0.55f);
}

// A log: the snag felled where it stood, lying along a random bearing, its stem resting on the
// ground. Limbs that would reach into the ground broke off in the fall.
void TreeGenerator::_fell() {
	RandomPCG fr;
	fr.seed(uint64_t(seed) * 40503u + 5u);
	const float bearing = fr.randf() * float(Math::TAU);
	const Vector3 axis = Vector3(Math::cos(bearing), 0.0f, Math::sin(bearing));
	const Vector3 base = nodes[0].start;
	float trunk_r = 0.1f;
	for (const Node &n : nodes) {
		if (!n.removed && n.parent == 0) {
			trunk_r = MAX(trunk_r, n.radius);
		}
	}
	const Basis turn(axis, float(Math::PI) * 0.5f * (0.96f + 0.04f * fr.randf()));
	for (Node &n : nodes) {
		n.start = base + turn.xform(n.start - base) + Vector3(0, trunk_r * 0.85f, 0);
		n.dir = turn.xform(n.dir);
	}
	for (int i = 1; i < int(nodes.size()); i++) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		// Limbs that would reach into the ground broke in the fall; on a decayed log the ones
		// standing up have rotted off too.
		const bool upright = decay_class >= 3 && n.order > 0 && n.end().y > base.y + trunk_r * 2.0f + 1.0f;
		if ((n.parent > 0 && nodes[n.parent].removed) || (n.order > 0 && MIN(n.start.y, n.end().y) < base.y - 0.02f) || upright) {
			n.removed = true;
		}
	}
	height = 0.0f;
	for (const Node &n : nodes) {
		if (!n.removed) {
			height = MAX(height, n.end().y - base.y);
		}
	}
}

// A shed limb: one first-order limb of the grown (and decayed) tree, 4 to 14 cm at its base, cut
// from the stem and laid on the ground along a random bearing. Most fallen wood in a forest is
// limbs and tops rather than whole trees (Harmon et al. 1986, coarse woody debris); twig loss
// and weathering follow its decay class.
void TreeGenerator::_shed_branch() {
	RandomPCG br;
	br.seed(uint64_t(seed) * 2246822519u + 17u);
	// Small limbs far outnumber big ones on the floor: 1-14 cm, weighted to the thin end
	// (thinner twig litter is the ground plants').
	const float pick_u = br.randf();
	const float target = Math::lerp(0.01f, MAX(shed_radius_max, 0.01f), pick_u * pick_u);
	int pick = -1;
	float best = 1e9f;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		// A limb off a stem; sticks may be any branch's side shoot.
		const bool limb = n.order == 1 && nodes[n.parent].order == 0;
		if (n.removed || n.order < 1 || nodes[n.parent].main == i || (!limb && shed_radius_max > 0.05f)) {
			continue;
		}
		// Prefer a limb of about the target girth that is also long: a short stub of the right
		// radius reads as a stick, not a fallen branch.
		float chain = 0.0f;
		for (int k = i; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
			chain += nodes[k].length;
		}
		const float d = Math::abs(Math::log(MAX(n.radius, 0.002f) / target)) - 0.15f * MIN(chain, 4.0f) + 0.05f * br.randf();
		if (d < best) {
			best = d;
			pick = i;
		}
	}
	if (pick < 0) {
		return;
	}
	// Keep the picked limb's subtree only (children come after their parents).
	LocalVector<bool> keep;
	keep.resize(nodes.size());
	for (int i = 0; i < int(nodes.size()); i++) {
		keep[i] = i == pick || (i > pick && nodes[i].parent >= 0 && keep[nodes[i].parent]);
	}
	for (int i = 1; i < int(nodes.size()); i++) {
		if (!keep[i]) {
			nodes[i].removed = true;
		}
	}
	// Lay it down: its overall direction (base to the mean of its wood) turned level along a
	// random bearing, resting on its thickest part.
	const Vector3 base = nodes[pick].start;
	Vector3 mean;
	int count = 0;
	for (int i = pick; i < int(nodes.size()); i++) {
		if (keep[i]) {
			mean += nodes[i].end();
			count++;
		}
	}
	Vector3 along = (mean / float(MAX(count, 1)) - base).normalized();
	if (along.length_squared() < 0.5f) {
		along = nodes[pick].dir;
	}
	const float bearing = br.randf() * float(Math::TAU);
	const Vector3 level(Math::cos(bearing), 0.0f, Math::sin(bearing));
	Basis turn;
	const Vector3 axis = along.cross(level);
	if (axis.length_squared() > 1e-8f) {
		turn = Basis(axis.normalized(), Math::acos(CLAMP(along.dot(level), -1.0f, 1.0f)));
	}
	// Roll about its own length to the rest that lays its side branches flattest: a fallen limb
	// settles onto its laterals instead of balancing with them upright.
	{
		const float roll0 = br.randf() * float(Math::TAU);
		float best_roll = roll0;
		float best_rise = 1e30f;
		for (int k = 0; k < 12; k++) {
			const float roll = roll0 + float(k) * float(Math::TAU) / 12.0f;
			const Basis t = Basis(level, roll) * turn;
			float rise = 0.0f;
			for (int i = pick + 1; i < int(nodes.size()); i++) {
				if (keep[i] && !nodes[i].removed) {
					rise += Math::abs(t.xform(nodes[i].dir).y) * nodes[i].length * nodes[i].radius;
				}
			}
			if (rise < best_rise) {
				best_rise = rise;
				best_roll = roll;
			}
		}
		turn = Basis(level, best_roll) * turn;
	}
	const Vector3 ground = nodes[0].start;
	for (int i = pick; i < int(nodes.size()); i++) {
		if (keep[i]) {
			nodes[i].start = ground + turn.xform(nodes[i].start - base);
			nodes[i].dir = turn.xform(nodes[i].dir);
		}
	}
	// Rest the limb's own axis on the ground: its lowest point (a bent limb sags at one end or
	// the middle) sits at ground level plus its radius there.
	LocalVector<bool> axis_node;
	axis_node.resize(nodes.size());
	for (int i = 0; i < int(nodes.size()); i++) {
		axis_node[i] = false;
	}
	float lift = -1e30f;
	for (int k = pick; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
		axis_node[k] = true;
		const Node &n = nodes[k];
		lift = MAX(lift, ground.y + n.radius * 0.85f - MIN(n.start.y, n.end().y));
	}
	for (int i = pick; i < int(nodes.size()); i++) {
		if (keep[i]) {
			nodes[i].start.y += lift;
		}
	}
	// Side branches pointing into the ground broke off when it fell, and so did those standing
	// high: a dead lateral more than a few limb diameters up snaps on impact or soon after.
	const float stand_max = ground.y + MAX(0.6f, 8.0f * nodes[pick].radius);
	for (int i = pick + 1; i < int(nodes.size()); i++) {
		Node &n = nodes[i];
		if (n.removed || axis_node[i]) {
			continue;
		}
		if (nodes[n.parent].removed || MIN(n.start.y, n.end().y) < ground.y - 0.02f || n.end().y > stand_max) {
			n.removed = true;
		}
	}
	height = 0.0f;
	for (const Node &n : nodes) {
		if (!n.removed) {
			height = MAX(height, n.end().y - ground.y);
		}
	}
}

// Collision capsules for wood thicker than collider_radius: each axis in pieces of up to 2 m,
// with the bone that carries it (rigged trees). 8 floats each: a, b, radius, bone.
PackedFloat32Array TreeGenerator::_collider_data() const {
	PackedFloat32Array d;
	if (collider_radius <= 0.0f) {
		return d;
	}
	for (int s = 1; s < int(nodes.size()); s++) {
		const Node &first = nodes[s];
		if (first.removed || (first.parent > 0 && nodes[first.parent].main == s) || first.radius < collider_radius) {
			continue;
		}
		Vector3 a = first.start;
		float len = 0.0f;
		float rsum = 0.0f;
		int count = 0;
		for (int i = s; i >= 0 && !nodes[i].removed && nodes[i].radius >= collider_radius; i = nodes[i].main) {
			const Node &n = nodes[i];
			len += n.length;
			rsum += n.radius;
			count++;
			const bool last = n.main < 0 || nodes[n.main].removed || nodes[n.main].radius < collider_radius;
			if (len >= 2.0f || last) {
				const Vector3 b = n.end();
				const int bone = rigged && i < int(node_bone.size()) ? node_bone[i] : 0;
				const float r = rsum / count;
				const float vals[8] = { a.x, a.y, a.z, b.x, b.y, b.z, r, float(bone) };
				for (float v : vals) {
					d.push_back(v);
				}
				a = b;
				len = 0.0f;
				rsum = 0.0f;
				count = 0;
			}
		}
	}
	return d;
}

// Wind rig (docs/ideas/trees.md, section 3a): rigid parts, as in The Witcher 4's forest. Each stem
// is split into up to four segments; then the thickest limbs, down to MIN_BONE_RADIUS, get a bone
// each until MAX_BONES. Every node takes the bone of its axis, or of the nearest boned axis below.
static constexpr float MIN_BONE_RADIUS = 0.02f; // m
static constexpr float WOOD_MODULUS = 8.5e9f; // Pa, green wood (Wood Handbook, FPL 2010).
static constexpr float WOOD_DENSITY = 900.0f; // kg/m3, green.
static constexpr float LEAF_MASS = 0.2f; // kg per m2 of broad leaf, fresh.
static constexpr float SHOOT_MASS = 0.5f; // kg per m2 of conifer shoot silhouette.

void TreeGenerator::_build_rig() {
	rig.clear();
	stem_segments.clear();
	axis_bone.clear();
	const int n = nodes.size();
	node_bone.resize(n);
	axis_offset.resize(n);
	LocalVector<int> axis_of;
	axis_of.resize(n);
	LocalVector<float> axis_len;
	axis_len.resize(n);
	for (int i = 0; i < n; i++) {
		axis_len[i] = 0.0f;
	}
	axis_of[0] = 0;
	axis_offset[0] = 0.0f;
	struct Cand {
		int axis;
		float radius;
		bool operator<(const Cand &p_o) const { return radius > p_o.radius; }
	};
	LocalVector<Cand> cands;
	LocalVector<int> stems;
	auto is_start = [&](int i) { return nodes[i].parent <= 0 || nodes[nodes[i].parent].main != i; };
	for (int i = 1; i < n; i++) {
		const Node &nd = nodes[i];
		const bool start = is_start(i);
		axis_of[i] = start ? i : axis_of[nd.parent];
		axis_offset[i] = start ? 0.0f : axis_offset[nd.parent] + nodes[nd.parent].length;
		if (!nd.removed) {
			axis_len[axis_of[i]] = MAX(axis_len[axis_of[i]], axis_offset[i] + nd.length);
		}
		if (start && !nd.removed) {
			if (nd.parent == 0) {
				stems.push_back(i);
			} else if (nd.radius >= MIN_BONE_RADIUS) {
				cands.push_back({ i, nd.radius });
			}
		}
	}
	LocalVector<uint8_t> boned;
	boned.resize(n);
	memset(boned.ptr(), 0, n);
	int budget = MAX_BONES - 1;
	// Long axes bend along their length, so they are split: stems into up to four segments, limbs
	// longer than 5 m into two.
	for (int st : stems) {
		const float len = axis_len[st];
		const int segs = len > 4.0f ? CLAMP(int(len / 3.0f), 2, 4) : 1;
		if (budget < segs) {
			break;
		}
		budget -= segs;
		stem_segments[st] = Vector2(segs, len / segs);
		boned[st] = 1;
	}
	cands.sort();
	for (const Cand &c : cands) {
		const int segs = axis_len[c.axis] > 5.0f ? 2 : 1;
		if (budget < segs) {
			break;
		}
		budget -= segs;
		stem_segments[c.axis] = Vector2(segs, axis_len[c.axis] / segs);
		boned[c.axis] = 1;
	}
	// The point and node at an arc length along an axis.
	auto along = [&](int p_axis, float p_s, int &r_node) {
		int i = p_axis;
		while (nodes[i].main >= 0 && !nodes[nodes[i].main].removed && axis_offset[i] + nodes[i].length < p_s) {
			i = nodes[i].main;
		}
		r_node = i;
		const float t = CLAMP(p_s - axis_offset[i], 0.0f, nodes[i].length);
		return nodes[i].start + nodes[i].dir * t;
	};

	RigBone root;
	root.pivot = nodes[0].start;
	root.dir = Vector3(0, 1, 0);
	root.zeta = 1.0f;
	rig.push_back(root);
	node_bone[0] = 0;
	for (int i = 1; i < n; i++) {
		const Node &nd = nodes[i];
		const int parent_bone = node_bone[MAX(nd.parent, 0)];
		if (is_start(i) && boned[i]) {
			const int first = rig.size();
			axis_bone[i] = first;
			const bool stem = nd.parent == 0;
			const int segs = int(stem_segments[i].x);
			const float seg = axis_len[i] / segs;
			for (int j = 0; j < segs; j++) {
				RigBone b;
				b.parent = j == 0 ? parent_bone : first + j - 1;
				int at = i;
				b.pivot = along(i, seg * j, at);
				int end_node = i;
				const Vector3 end = along(i, seg * (j + 1), end_node);
				b.dir = (end - b.pivot).normalized();
				if (b.dir.length_squared() < 0.5f) {
					b.dir = nd.dir;
				}
				b.length = MAX(seg, 0.05f);
				// Bending is set by the wood near the loaded base: radius a third of the way up.
				int mid = i;
				along(i, seg * (j + 0.33f), mid);
				b.radius = MAX(nodes[mid].radius, params[PARAM_TIP_RADIUS]);
				// Rotational compliance of the segment: the integral of the bending moment, relative to
				// its value at the base, over E I along the wood. The moment of a load spread to the
				// axis tip falls as the square of the distance left, so the thick base dominates.
				{
					const float total = axis_len[i];
					const float s0 = seg * j;
					const float s1 = seg * (j + 1);
					for (int q = i; q >= 0 && !nodes[q].removed; q = nodes[q].main) {
						const float sm = axis_offset[q] + nodes[q].length * 0.5f;
						if (sm >= s0 && sm < s1 + 1e-4f) {
							const float rel = (total - sm) / MAX(total - s0, 1e-3f);
							const float rq = MAX(nodes[q].radius, params[PARAM_TIP_RADIUS]);
							const double ei = double(WOOD_MODULUS) * Math::PI * double(rq) * rq * rq * rq * 0.25;
							b.compliance += double(nodes[q].length) * rel * rel / ei;
						}
					}
				}
				b.zeta = stem ? 0.1f : 0.15f;
				b.stem = stem;
				b.group = first;
				b.axis = i;
				b.segment = j;
				rig.push_back(b);
			}
		}
		const int ax = axis_of[i];
		if (boned[ax] && axis_bone.has(ax)) {
			const int first = axis_bone[ax];
			if (stem_segments.has(ax)) {
				const Vector2 sg = stem_segments[ax];
				const int j = CLAMP(int((axis_offset[i] + nd.length * 0.5f) / sg.y), 0, int(sg.x) - 1);
				node_bone[i] = first + j;
			} else {
				node_bone[i] = first;
			}
		} else {
			node_bone[i] = parent_bone;
		}
	}
}

// Stem vertices blend between consecutive segment bones over a fifth of a segment either side of
// each joint, so a split trunk bends as one smooth curve.
void TreeGenerator::_skin_stem(Builder &r_b, const Vector4i &p_part, int p_axis, const LocalVector<Vector3> &p_points, int p_sides) const {
	if (!stem_segments.has(p_axis) || !axis_bone.has(p_axis)) {
		return;
	}
	const Vector2 sg = stem_segments[p_axis];
	const int segs = int(sg.x);
	const int first = axis_bone[p_axis];
	const float blend = 0.2f * sg.y;
	float s = 0.0f;
	int32_t *bw = r_b.bones.ptrw();
	float *ww = r_b.weights.ptrw();
	for (int r = 0; r < int(p_points.size()); r++) {
		if (r > 0) {
			s += p_points[r].distance_to(p_points[r - 1]);
		}
		int b0 = CLAMP(int(s / sg.y), 0, segs - 1);
		int b1 = b0;
		float w1 = 0.0f;
		const float local = s - b0 * sg.y;
		if (local < blend && b0 > 0) {
			b1 = b0;
			b0 = b0 - 1;
			w1 = 0.5f + 0.5f * local / blend;
		} else if (local > sg.y - blend && b0 + 1 < segs) {
			b1 = b0 + 1;
			w1 = 0.5f * (local - (sg.y - blend)) / blend;
		}
		for (int k = 0; k <= p_sides; k++) {
			const int v = p_part.z + r * (p_sides + 1) + k;
			if (v >= p_part.z + p_part.w) {
				break;
			}
			bw[v * 4 + 0] = first + b0;
			bw[v * 4 + 1] = first + b1;
			ww[v * 4 + 0] = 1.0f - w1;
			ww[v * 4 + 1] = w1;
		}
	}
}

PackedFloat32Array TreeGenerator::_rig_data() const {
	PackedFloat32Array d;
	d.resize(rig.size() * RIG_STRIDE);
	float *w = d.ptrw();
	for (uint32_t b = 0; b < rig.size(); b++) {
		const RigBone &rb = rig[b];
		float *o = w + b * RIG_STRIDE;
		o[RIG_PARENT] = float(rb.parent);
		o[RIG_PIVOT_X] = rb.pivot.x;
		o[RIG_PIVOT_Y] = rb.pivot.y;
		o[RIG_PIVOT_Z] = rb.pivot.z;
		const Vector3 c = rb.a0 > 1e-9 ? rb.a1 / float(rb.a0) - rb.pivot : Vector3();
		o[RIG_CENTROID_X] = c.x;
		o[RIG_CENTROID_Y] = c.y;
		o[RIG_CENTROID_Z] = c.z;
		o[RIG_AREA] = float(rb.a0);
		// Bending stiffness of a segment as a joint at its base: one over its integrated compliance
		// (_build_rig), or 2 E I / L where that is missing.
		const float r = rb.radius;
		const float ei = WOOD_MODULUS * float(Math::PI) * r * r * r * r * 0.25f;
		o[RIG_STIFFNESS] = b == 0 ? 0.0f : (rb.compliance > 0.0 ? float(1.0 / rb.compliance) : 2.0f * ei / MAX(rb.length, 0.05f));
		const double j = rb.m2 - 2.0 * double(rb.pivot.dot(rb.m1)) + double(rb.pivot.length_squared()) * rb.m0;
		o[RIG_INERTIA] = float(MAX(j, 1e-6));
		o[RIG_ZETA] = rb.zeta;
		o[RIG_RADIUS] = r;
		o[RIG_LENGTH] = rb.length;
		o[RIG_DIR_X] = rb.dir.x;
		o[RIG_DIR_Y] = rb.dir.y;
		o[RIG_DIR_Z] = rb.dir.z;
		o[RIG_GROUP] = float(b == 0 ? 0 : rb.group);
		o[RIG_MASS] = float(rb.m0);
	}
	return d;
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

Ref<ArrayMesh> TreeGenerator::_build_mesh() {
	Builder bark;
	bark.use_tangents = true;
	Builder leaves;
	Builder moss;
	moss.use_tangents = true;
	leaf_count = 0;
	supports.clear();
	climber_leaves.clear();
	leaf_twins = Builder();
	leaf_twin_of.clear();
	compound_slot = 0;
	const float moss_k = _moss_amount();
	const float moss_ground = nodes.is_empty() ? 0.0f : nodes[0].start.y;
	// Moss greens the bark under it at every LOD (the shell's footprint, and all the moss there
	// is at coarser levels): the tint follows the same weight as the shell.
	const Color moss_green(0.085f, 0.13f, 0.025f);
	auto moss_tint = [&](Builder &r_b, const Vector4i &p_part) {
		if (moss_k <= 0.0f) {
			return;
		}
		for (int v = p_part.z; v < p_part.z + p_part.w; v++) {
			const float w = _moss_at(r_b.verts[v], r_b.normals[v], moss_ground, moss_k);
			const float t = 0.85f * CLAMP((w - 0.15f) / 0.4f, 0.0f, 1.0f);
			if (t > 0.0f) {
				r_b.colors.write[v] = r_b.colors[v].lerp(moss_green, t);
			}
		}
	};
	if (rigged) {
		_build_rig();
	} else {
		rig.clear();
		node_bone.resize(nodes.size());
		for (int &b : node_bone) {
			b = 0;
		}
	}
	node_drag.resize(nodes.size());
	node_mass.resize(nodes.size());
	for (uint32_t i = 0; i < nodes.size(); i++) {
		node_drag[i] = 0.0f;
		node_mass[i] = 0.0f;
	}
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
			// Load for the wind rig: frontal area and green mass of the wood. Wood thinner than
			// 5 cm is inside the crown and as sheltered as the leaves there.
			node_drag[chain[c]] += (n.radius + r_end) * n.length;
			node_mass[chain[c]] += float(Math::PI) * n.radius * n.radius * n.length * WOOD_DENSITY;
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
		if (!stem && r0 > 0.01f && pts.size() >= 2) {
			// Branch collar: a limb swells where it leaves its parent, so its base meets the
			// parent's bark in a smooth fillet instead of a pipe pushed into a pipe (the welded
			// junctions of SpeedTree; Bloomenthal 1985 for the implicit version). The swelling
			// grows with the limb's share of the parent's girth and fades over about one limb
			// radius past the parent's surface.
			const Node &par = nodes[first.parent];
			const float rp = MAX(par.radius, r0);
			const float sin_a = MAX(first.dir.cross(par.dir).length(), 0.35f);
			const float surf = rp / sin_a; // From the parent's axis to its surface along the limb.
			// Forks of near-equal stems get none: their join is a crotch, not a collar.
			const float ratio = r0 / rp;
			const float swell = 0.2f * CLAMP(ratio * 2.5f, 0.2f, 1.0f) * (1.0f - Math::smoothstep(0.35f, 0.6f, ratio));
			const float seg = pts[0].distance_to(pts[1]);
			LocalVector<Vector3> cp;
			LocalVector<float> cr;
			cp.push_back(pts[0]);
			cr.push_back(rad[0] * (1.0f + swell));
			const float marks[5] = { 0.85f, 1.0f, 1.0f, 1.0f, 1.0f };
			const float after[5] = { 0.0f, 0.0f, 0.4f, 1.0f, 2.0f };
			for (int k = 0; k < 5; k++) {
				const float d = surf * marks[k] + r0 * after[k];
				if (d >= seg * 0.95f) {
					break;
				}
				const float past = MAX(d - surf, 0.0f);
				cp.push_back(pts[0].lerp(pts[1], d / seg));
				cr.push_back(Math::lerp(rad[0], rad[1], d / seg) * (1.0f + swell * Math::exp(-past / (0.7f * r0))));
			}
			for (uint32_t i = 1; i < pts.size(); i++) {
				cp.push_back(pts[i]);
				cr.push_back(rad[i]);
			}
			pts = cp;
			rad = cr;
		}
		if (stem && flare > 0.0f) {
			for (int i = 0; i < int(pts.size()); i++) {
				const float h = MAX(0.0f, pts[i].y - nodes[0].start.y);
				rad[i] *= 1.0f + flare * Math::exp(-h / MAX(0.05f, trunk_r * 2.5f));
			}
			// Sink the base below the ground line.
			pts[0] -= Vector3(0, 0.15f, 0);
		}
		if (climber != CLIMBER_NONE && r0 >= 0.02f) {
			// Kept for the climbers: the axis as meshed, its radii and the bone at each point.
			Support sup;
			sup.pts = pts;
			sup.rad = rad;
			sup.stem = stem;
			int c = 0;
			for (uint32_t i = 0; i < pts.size(); i++) {
				while (c + 1 < int(chain.size()) && (pts[i] - nodes[chain[c]].end()).dot(nodes[chain[c]].dir) > 0.0f) {
					c++;
				}
				sup.bone.push_back(node_bone[chain[c]]);
			}
			supports.push_back(sup);
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
		bark.bone = node_bone[s];
		const bool relief = bark_relief > 0.0f && r0 >= RELIEF_MIN_RADIUS;
		// A mesh without LODs (the near variants) carries the relief tube alone.
		if (!(relief && skip_lods)) {
			Vector4i part;
			bark.role = relief ? Builder::ROLE_PLAIN : Builder::ROLE_ALL;
			bark.begin(part);
			_tube(bark, pts, rad, sides, 0.0f, col);
			bark.end(part, r0);
			moss_tint(bark, part);
			if (rigged) {
				_skin_stem(bark, part, s, pts, sides);
			}
		}
		const bool moss_shell = moss_k > 0.05f && r0 >= MOSS_SHELL_MIN_RADIUS;
		if (relief || moss_shell) {
			// LOD 0 and the path tracer: the same tube at about 2.5 cm around and 5 cm along,
			// displaced by the bark's relief. Bark thickens with girth, so thin limbs carry a
			// quarter of a mature trunk's furrow depth (Kratt et al. 2015, cambial growth).
			LocalVector<Vector3> rp;
			LocalVector<float> rr;
			for (int i = 0; i + 1 < int(pts.size()); i++) {
				const int steps = MAX(1, int(Math::ceil(pts[i].distance_to(pts[i + 1]) / 0.05f)));
				for (int k = 0; k < steps; k++) {
					const float t = float(k) / steps;
					rp.push_back(pts[i].lerp(pts[i + 1], t));
					rr.push_back(Math::lerp(rad[i], rad[i + 1], t));
				}
			}
			rp.push_back(pts[pts.size() - 1]);
			rr.push_back(rad[rad.size() - 1]);
			const int rsides = CLAMP(int(Math::ceil(Math::TAU * r0 / 0.025f)), 24, 128);
			const float scale = bark_relief * CLAMP(r0 / 0.35f, 0.25f, 1.0f);
			const BarkSet &bs = _bark_set(_bark_kind(preset));
			if (relief) {
				Vector4i part;
				bark.role = skip_lods ? Builder::ROLE_ALL : Builder::ROLE_RELIEF;
				bark.begin(part);
				_tube(bark, rp, rr, rsides, 0.0f, col, bs.height.ptr(), scale);
				bark.end(part, r0);
				moss_tint(bark, part);
				if (rigged) {
					_skin_stem(bark, part, s, rp, rsides);
				}
			}
			if (moss_shell) {
				// The moss shell (section 5c): the same displaced tube, lifted by the moss's depth
				// where the weight is high and sunk below the bark where it fades, so the moss edge
				// needs no alpha; quads wholly under the bark are dropped. Cushions thin out on
				// thin limbs.
				const float depth = 0.018f * CLAMP(r0 / 0.15f, 0.35f, 1.0f);
				Vector4i part;
				moss.bone = node_bone[s];
				moss.begin(part);
				_tube(moss, rp, rr, rsides, 0.0f, col);
				moss.end(part, r0);
				// The bark's own displacement under each shell vertex (as the relief tube has it).
				const Vector<uint8_t> bh_buf = bs.height->get_data();
				const float *bh = reinterpret_cast<const float *>(bh_buf.ptr());
				const int bn = bs.height->get_width();
				auto bark_disp = [&](const Vector2 &p_uv) -> float {
					if (!relief) {
						return 0.0f;
					}
					const float x = (p_uv.x - Math::floor(p_uv.x)) * bn - 0.5f, y = (p_uv.y - Math::floor(p_uv.y)) * bn - 0.5f;
					const int x0 = int(Math::floor(x)), y0 = int(Math::floor(y));
					const float fx = x - x0, fy = y - y0;
					auto at = [&](int i, int j) { return bh[((j % bn + bn) % bn) * bn + ((i % bn + bn) % bn)]; };
					return scale * Math::lerp(Math::lerp(at(x0, y0), at(x0 + 1, y0), fx), Math::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
				};
				// Moss fills the furrows first, then grows over the plates (Barkman 1958): the moss
				// surface rises from the furrow floor to above the plate tops as its weight grows,
				// and wherever it is below the bark it stays hidden just under it.
				const float floor_d = relief ? -0.045f * scale : -0.004f; // Below the deepest furrow (3 cm plus grain).
				if (rigged) {
					_skin_stem(moss, part, s, rp, rsides);
				}
				LocalVector<float> wv;
				wv.resize(part.w);
				for (int i = 0; i < part.w; i++) {
					const int v = part.z + i;
					const Vector3 n = moss.normals[v];
					const float w = _moss_at(moss.verts[v], n, moss_ground, moss_k);
					// Hummocks a hand across: the vertices are 2.5 cm apart, so the texture's 4 cm
					// cushions stay in the normal map and the geometry carries only what it resolves.
					const float hummock = moss_noise(moss.verts[v] * 9.0f, 0x6d6f7376u);
					// A narrow ramp: the moss edge is a contour of the smooth weight that only dips into
					// the furrows just ahead of it; a wide one shows the 2.5 cm vertex grid as blocks
					// where furrows are partly filled.
					const float ramp = CLAMP((w - 0.35f) / 0.25f, 0.0f, 1.0f);
					const float fill = Math::lerp(floor_d, depth * (0.6f + 0.8f * hummock), ramp * ramp * (3.0f - 2.0f * ramp));
					const float under = bark_disp(moss.uvs[v]);
					// Shown where the fill stands above the bark; wv carries the margin.
					wv[i] = fill - under;
					moss.verts.write[v] = moss.verts[v] + n * MAX(fill, under - 0.004f);
					// Wetter, deeper moss is darker; thin moss on dry faces yellower.
					const float dry = moss_noise(moss.verts[v] * 1.3f, 0x6d6f7375u);
					moss.colors.write[v] = Color(0.075f, 0.13f, 0.025f).lerp(Color(0.15f, 0.18f, 0.04f), CLAMP(dry * 1.2f - w * 0.4f, 0.0f, 1.0f));
				}
				// Keep quads (two triangles each) with any corner showing.
				int at = part.x;
				int32_t *iw = moss.indices.ptrw();
				for (int q = part.x; q + 5 < part.x + part.y; q += 6) {
					bool show = false;
					for (int k = 0; k < 6 && !show; k++) {
						show = wv[iw[q + k] - part.z] > 0.0f;
					}
					if (show) {
						for (int k = 0; k < 6; k++) {
							iw[at + k] = iw[q + k];
						}
						at += 6;
					}
				}
				const int kept = at - part.x;
				// Compact the part's vertices to those still referenced.
				LocalVector<int> remap;
				remap.resize(part.w);
				for (int i = 0; i < part.w; i++) {
					remap[i] = -1;
				}
				// Mark, then number in vertex order, so the in-place copy below only moves down.
				for (int q = part.x; q < at; q++) {
					remap[iw[q] - part.z] = 0;
				}
				int nv = 0;
				for (int i = 0; i < part.w; i++) {
					if (remap[i] == 0) {
						remap[i] = nv++;
					}
				}
				for (int i = 0; i < part.w; i++) {
					if (remap[i] < 0) {
						continue;
					}
					const int from = part.z + i, to = part.z + remap[i];
					moss.verts.write[to] = moss.verts[from];
					moss.normals.write[to] = moss.normals[from];
					moss.uvs.write[to] = moss.uvs[from];
					moss.colors.write[to] = moss.colors[from];
					for (int c = 0; c < 4; c++) {
						moss.tangents.write[to * 4 + c] = moss.tangents[from * 4 + c];
						moss.bones.write[to * 4 + c] = moss.bones[from * 4 + c];
						moss.weights.write[to * 4 + c] = moss.weights[from * 4 + c];
					}
				}
				for (int q = part.x; q < at; q++) {
					iw[q] = part.z + remap[iw[q] - part.z];
				}
				moss.indices.resize(at);
				moss.verts.resize(part.z + nv);
				moss.normals.resize(part.z + nv);
				moss.uvs.resize(part.z + nv);
				moss.colors.resize(part.z + nv);
				moss.tangents.resize((part.z + nv) * 4);
				moss.bones.resize((part.z + nv) * 4);
				moss.weights.resize((part.z + nv) * 4);
				if (kept > 0) {
					moss.parts[moss.parts.size() - 1] = Vector4i(part.x, kept, part.z, nv);
				} else {
					moss.parts.resize(moss.parts.size() - 1);
					moss.part_size.resize(moss.part_size.size() - 1);
					moss.part_role.resize(moss.part_role.size() - 1);
				}
			}
		}
		bark.role = Builder::ROLE_ALL;
		if (broken_tip >= 0 && chain[chain.size() - 1] == broken_tip) {
			// A snapped top: splinters of pale wood standing up from the rim.
			const Node &tn = nodes[broken_tip];
			const Vector3 e = pts[pts.size() - 1];
			const float r = rad[rad.size() - 1];
			const Vector3 ref = Math::abs(tn.dir.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
			const Vector3 u = tn.dir.cross(ref).normalized();
			const Vector3 v = tn.dir.cross(u);
			RandomPCG sr;
			sr.seed(uint64_t(broken_tip) * 2246822519u + uint64_t(seed));
			const Color wood = Color(0.62f, 0.55f, 0.44f);
			Vector4i part;
			bark.begin(part);
			const int spikes = 9;
			for (int k = 0; k < spikes; k++) {
				const float a0 = Math::TAU * k / spikes;
				const float a1 = Math::TAU * (k + 1) / spikes;
				const Vector3 p0 = e + (u * Math::cos(a0) + v * Math::sin(a0)) * r;
				const Vector3 p1 = e + (u * Math::cos(a1) + v * Math::sin(a1)) * r;
				const float am = (a0 + a1) * 0.5f;
				const Vector3 tip = e + (u * Math::cos(am) + v * Math::sin(am)) * (r * Math::lerp(0.2f, 0.8f, sr.randf())) + tn.dir * (r * Math::lerp(0.3f, 3.0f, sr.randf()));
				const Vector3 nrm = (p1 - p0).cross(tip - p0).normalized();
				const int b0 = bark.verts.size();
				const Vector3 tri[3] = { p0, tip, p1 };
				for (int side = 0; side < 2; side++) {
					for (int q = 0; q < 3; q++) {
						bark.verts.push_back(tri[q]);
						bark.normals.push_back(side == 0 ? nrm : -nrm);
						bark.uvs.push_back(Vector2(float(q) * 0.5f, 0.0f));
						bark.colors.push_back(wood);
					}
				}
				bark.indices.push_back(b0);
				bark.indices.push_back(b0 + 1);
				bark.indices.push_back(b0 + 2);
				bark.indices.push_back(b0 + 3);
				bark.indices.push_back(b0 + 5);
				bark.indices.push_back(b0 + 4);
			}
			bark.end(part, r);
		}
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
	// Leaves inside a crown shelter each other: the mean exposure through a canopy of leaf area
	// index L is (1 - exp(-L/2)) / (L/2) (Beer-Lambert with G = 0.5), about 0.37 at L = 5.
	const float crown_lai = leaf_area_index > 0.0f ? leaf_area_index : 4.0f;
	const float exposure = (1.0f - Math::exp(-0.5f * crown_lai)) / (0.5f * crown_lai);
	for (uint32_t i = 1; i < nodes.size(); i++) {
		if (nodes[i].radius < 0.05f) {
			node_drag[i] *= exposure;
		}
	}
	// The season (2c): leaves not yet out or fallen are left off, so drag and mass follow; the
	// rest grow from small and pale in spring to full size, and turn in autumn in a fixed order
	// per leaf (the first to turn falls first).
	const LeafSeason leaf_season = _leaf_season(preset, season);
	uint32_t leaf_slot = 0; // Every broad leaf the summer tree has, kept or not: a stable key.
	const uint32_t season_seed = hash_murmur3_one_32(uint32_t(seed) * 2654435761u + 977u);
	auto season_leaf = [&](uint32_t p_key, Color &r_color, float &r_size) -> bool {
		if (season.is_empty()) {
			return true;
		}
		if (leaf_season.out <= 0.0f || leaf_season.fallen >= 1.0f) {
			return false;
		}
		const float h = float(hash_murmur3_one_32(p_key, season_seed) & 0xffffff) / float(0x1000000);
		if (h < leaf_season.fallen) {
			return false;
		}
		if (leaf_season.out < 1.0f) {
			const float young = 1.0f - leaf_season.out;
			r_size *= Math::lerp(1.0f, 0.3f, young);
			r_color = r_color.lerp(Color(r_color.r * 1.6f, r_color.g * 1.45f, r_color.b * 0.9f), young * 0.7f);
		}
		const float turn = CLAMP((leaf_season.autumn - h) * 6.0f, 0.0f, 1.0f);
		if (turn > 0.0f) {
			const float jitter = 0.85f + 0.3f * float((hash_murmur3_one_32(p_key ^ 0x9e3779b9u, season_seed) >> 8) & 0xffff) / 65535.0f;
			r_color = r_color.lerp(Color(leaf_season.color.r * jitter, leaf_season.color.g * jitter, leaf_season.color.b), turn);
		}
		return true;
	};
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if ((!n.leafy && !spurs[i]) || n.removed || n.dead) {
			continue;
		}
		// Thin by twigs and by leaves on the kept twigs in equal parts: dropping whole twigs alone
		// left heavily thinned crowns as isolated clumps with daylight between them.
		const float keep_twig = Math::sqrt(node_keep[i]);
		const float keep_leaf = keep_twig;
		bark.bone = node_bone[i];
		leaves.bone = node_bone[i];
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
					Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
					float spray_size = 1.0f;
					if (!season_leaf(uint32_t(i) * 4099u + uint32_t(sh), c, spray_size)) {
						continue;
					}
					Vector3 flat = plane_side - bdir * plane_side.dot(bdir);
					flat = flat.length_squared() > 1e-6f ? flat.normalized() : perp;
					const Vector3 cross = bdir.cross(flat).normalized();
					const float half = size * 0.8f * spray_size;
					{ Vector4i part; leaves.begin(part); _add_spray(leaves, b0, bdir, blen + size * 0.5f, half, flat, c); leaves.end(part, 0.0f); }
					{ Vector4i part; leaves.begin(part); _add_spray(leaves, b0, bdir, blen + size * 0.5f, half, cross, c); leaves.end(part, 0.0f); }
					leaf_count += per_brush;
				}
				// The brush as a bottle-brush silhouette, for the wind rig.
				const float shoot = (blen + size * 0.5f) * size * 1.6f;
				node_drag[i] += shoot * exposure;
				node_mass[i] += shoot * SHOOT_MASS;
				if (needle_sprays) {
					continue;
				}
				for (int k = 0; k < per_brush; k++) {
					const float t = (k + 0.5f) / per_brush;
					roll += 2.39996f;
					const Vector3 around = perp.rotated(n.dir, roll);
					const Vector3 ar = (around - bdir * around.dot(bdir)).normalized();
					const Vector3 base = b0 + bdir * (blen * t) + ar * br;
					const float jitter = 0.85f + 0.3f * lr.randf();
					Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
					const Vector3 out = (ar * 0.75f + bdir * 0.66f).normalized();
					float needle_size = size * (0.8f + 0.4f * lr.randf());
					if (!season_leaf(uint32_t(i) * 4099u + uint32_t(sh * 257 + k), c, needle_size)) {
						continue;
					}
					{ Vector4i part; leaves.begin(part); _add_needle(leaves, base, out, needle_size, c); leaves.end(part, 0.0f); }
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
			node_drag[i] += 1.5f * p_r * p_len * exposure;
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
				Color c = Color(leaf_color.r * jitter, leaf_color.g * jitter, leaf_color.b * (0.9f + 0.2f * lr.randf()));
				float leaf_size = size * (0.75f + 0.5f * lr.randf());
				if (!season_leaf(leaf_slot++, c, leaf_size)) {
					continue;
				}
				{ Vector4i part; leaves.begin(part); _add_leaf(leaves, at, k == p_leaves - 1 ? tip_dir : out, up, leaf_size, c); leaves.end(part, 0.0f); }
				leaf_count++;
				const float blade_area = 2.0f * params[PARAM_LEAF_WIDTH] * leaf_size * leaf_size * 0.656f;
				// Randomly turned leaves present about half their area (G = 0.5).
				node_drag[i] += 0.5f * blade_area * exposure;
				node_mass[i] += blade_area * LEAF_MASS;
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

	if (rigged) {
		// One internode stands for a whole twig system, so the summed wood and leaf area overstate
		// what a real tree carries. Totals are set from measured trees instead and spread as the
		// model distributes them. Mass: green stem volume pi r^2 H times a form factor of 0.5 for
		// the taper, plus 40% for the crown's branches (Zianis et al., Silva Fennica Monographs 4,
		// 2005: branch biomass 15-40% of the stem in temperate trees). Drag: the crown's frontal
		// silhouette, an ellipse of its width and depth, so Cd is a whole-crown coefficient (0.2-0.3
		// at 20 m/s in wind tunnel tests of whole trees; Vollsinger et al., Can. J. For. Res. 35,
		// 2005), reached here through Vogel's reconfiguration in TreeWind3D.
		double drag_total = 0.0, mass_total = 0.0;
		for (uint32_t i = 1; i < nodes.size(); i++) {
			if (!nodes[i].removed) {
				drag_total += node_drag[i];
				mass_total += node_mass[i];
			}
		}
		// Basal area of every stem, at breast height (about 0.9 of the radius at the collar).
		float basal_r2 = 0.0f;
		for (uint32_t i = 1; i < nodes.size(); i++) {
			if (nodes[i].parent == 0 && !nodes[i].removed) {
				basal_r2 += nodes[i].radius * nodes[i].radius * 0.81f;
			}
		}
		const float mass_target = WOOD_DENSITY * 0.5f * float(Math::PI) * MAX(basal_r2, 1e-6f) * MAX(height, 1.0f) * 1.4f;
		const float depth = MAX(height - crown_base, height * 0.3f);
		const float drag_target = float(Math::PI) * 0.25f * (2.0f * MAX(crown_radius, 0.3f)) * depth;
		const float mass_k = mass_total > 0.0 ? float(mass_target / mass_total) : 1.0f;
		const float drag_k = drag_total > 0.0 ? float(drag_target / drag_total) : 1.0f;
		for (uint32_t i = 1; i < nodes.size(); i++) {
			node_mass[i] *= mass_k;
			node_drag[i] *= drag_k;
		}
		// Each bone carries its own nodes and everything on its child bones.
		for (uint32_t i = 1; i < nodes.size(); i++) {
			const Node &n = nodes[i];
			if (n.removed) {
				continue;
			}
			RigBone &b = rig[node_bone[i]];
			const Vector3 x = n.start + n.dir * (n.length * 0.5f);
			b.a0 += node_drag[i];
			b.a1 += x * node_drag[i];
			b.m0 += node_mass[i];
			b.m1 += x * node_mass[i];
			b.m2 += double(node_mass[i]) * double(x.length_squared());
		}
		for (int b = int(rig.size()) - 1; b > 0; b--) {
			RigBone &c = rig[b];
			RigBone &p = rig[MAX(c.parent, 0)];
			p.a0 += c.a0;
			p.a1 += c.a1;
			p.m0 += c.m0;
			p.m1 += c.m1;
			p.m2 += c.m2;
		}
	}

	if (climber != CLIMBER_NONE && climber_cover > 0.0f) {
		_grow_climbers(bark, leaves);
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Builder *builders[3] = { &bark, &leaves, &moss };
	for (int s = 0; s < 3; s++) {
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
		Dictionary lods;
		if (!skip_lods) {
			if (s == 0) {
				lods = _bark_lods(b);
			} else if (s == 1) {
				lods = _leaf_lods(b);
			} else {
				// The moss shell is LOD 0 only (and the path tracer's); coarser levels keep the tint.
				PackedInt32Array none;
				none.push_back(0);
				none.push_back(0);
				none.push_back(0);
				lods[TREE_LODS[0].error] = none;
			}
		}
		if (s == 0) {
			// LOD 0 draws the relief tubes in place of the plain ones they cover.
			bool any_plain = false;
			for (uint32_t i = 0; i < b.parts.size(); i++) {
				any_plain = any_plain || b.part_role[i] == Builder::ROLE_PLAIN;
			}
			if (any_plain) {
				PackedInt32Array full;
				full.resize(b.indices.size());
				int at = 0, from = 0;
				int32_t *fw = full.ptrw();
				for (uint32_t i = 0; i < b.parts.size(); i++) {
					const Vector4i &pt = b.parts[i];
					if (b.part_role[i] != Builder::ROLE_PLAIN) {
						continue;
					}
					// Keep everything up to this part, then skip it.
					memcpy(fw + at, b.indices.ptr() + from, sizeof(int32_t) * (pt.x - from));
					at += pt.x - from;
					from = pt.x + pt.y;
				}
				memcpy(fw + at, b.indices.ptr() + from, sizeof(int32_t) * (b.indices.size() - from));
				at += b.indices.size() - from;
				full.resize(at);
				b.indices = full;
			}
		}
		arrays[Mesh::ARRAY_VERTEX] = b.verts;
		arrays[Mesh::ARRAY_NORMAL] = b.normals;
		arrays[Mesh::ARRAY_TEX_UV] = b.uvs;
		arrays[Mesh::ARRAY_COLOR] = b.colors;
		arrays[Mesh::ARRAY_INDEX] = b.indices;
		if (b.use_tangents && b.tangents.size() == b.verts.size() * 4) {
			arrays[Mesh::ARRAY_TANGENT] = b.tangents;
		}
		if (rigged) {
			arrays[Mesh::ARRAY_BONES] = b.bones;
			arrays[Mesh::ARRAY_WEIGHTS] = b.weights;
		}
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), lods, compress_vertices ? Mesh::ARRAY_FLAG_COMPRESS_ATTRIBUTES : 0);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(1, 1, 1));
		m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		m->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
		m->set_snow_from_weather(true);
		if (rigged) {
			// Skinned from MultiMesh instance bones (TreeWind3D writes them).
			m->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, true);
		}
		if (s == 0) {
			const BarkSet &bs = _bark_set(_bark_kind(preset));
			m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, bs.albedo);
			m->set_roughness(1.0f);
			m->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, bs.roughness);
			m->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
			if (b.use_tangents) {
				m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
				m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, bs.normal);
			}
			mesh->surface_set_name(mesh->get_surface_count() - 1, "bark");
		} else if (s == 2) {
			const BarkSet &ms = _moss_set();
			m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, ms.albedo);
			m->set_roughness(1.0f);
			m->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, ms.roughness);
			m->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
			m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
			m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, ms.normal);
			mesh->surface_set_name(mesh->get_surface_count() - 1, "moss");
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
			} else if (!needles) {
				// Venation; the factor averages about 0.88, so the albedo lifts it back to the
				// species' leaf colour on average.
				m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _leaf_texture());
				m->set_albedo(Color(1.13f, 1.13f, 1.13f));
			}
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
			m->set_backlight(leaf_color * 0.6f);
			mesh->surface_set_name(mesh->get_surface_count() - 1, needles ? "needles" : "leaves");
		}
		mesh->surface_set_material(mesh->get_surface_count() - 1, m);
	}
	mesh->set_meta(SNAME("tree_colliders"), _collider_data());
	if (!climber_leaves.is_empty()) {
		mesh->set_meta(SNAME("tree_climber_leaves"), climber_leaves);
	}
	if (rigged) {
		mesh->set_meta(SNAME("tree_rig"), _rig_data());
		// Vogel's exponent: drag grows as U^(2 + B) as foliage streamlines (Vogel 1989).
		mesh->set_meta(SNAME("tree_vogel"), params[PARAM_NEEDLES] > 0.5f ? -0.5f : -0.7f);
	}
	return mesh;
}


Dictionary TreeGenerator::_bark_lods(const Builder &p_bark) const {
	Dictionary lods;
	const int s_full_count = p_bark.indices.size();
	for (const auto &level : TREE_LODS) {
		PackedInt32Array idx;
		for (uint32_t i = 0; i < p_bark.parts.size(); i++) {
			if (p_bark.part_size[i] < level.min_radius || p_bark.part_role[i] == Builder::ROLE_RELIEF) {
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
		if (idx.size() >= (s_full_count)) {
			continue; // Nothing to drop at this level (deadwood keeps only thick wood).
		}
		lods[level.error] = idx;
	}
	return lods;
}

Dictionary TreeGenerator::_leaf_lods(Builder &r_leaves) const {
	Dictionary lods;
	const int s_full_count = r_leaves.indices.size();
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
			// A species leaf (lobed, palmate, compound) is drawn as its simple twin here.
			const Vector4i *twin = leaf_twin_of.getptr(int(i));
			const Builder &src = twin ? leaf_twins : r_leaves;
			const Vector4i sp = twin ? *twin : pt;
			const bool skinned = r_leaves.bones.size() >= (pt.z + 1) * 4;
			for (int v = 0; v < sp.w; v++) {
				r_leaves.verts.push_back(pivot + (src.verts[sp.z + v] - pivot) * grow);
				r_leaves.normals.push_back(src.normals[sp.z + v]);
				r_leaves.uvs.push_back(src.uvs[sp.z + v]);
				r_leaves.colors.push_back(src.colors[sp.z + v]);
				if (skinned) {
					// The leaf's own skin (one bone per leaf) from its first vertex.
					for (int k = 0; k < 4; k++) {
						r_leaves.bones.push_back(r_leaves.bones[pt.z * 4 + k]);
						r_leaves.weights.push_back(r_leaves.weights[pt.z * 4 + k]);
					}
				}
			}
			for (int k = 0; k < sp.y; k++) {
				idx.push_back(src.indices[sp.x + k] - sp.z + v0);
			}
		}
		if (idx.is_empty()) {
			break;
		}
		if (idx.size() >= (s_full_count)) {
			continue; // Nothing to drop at this level (deadwood keeps only thick wood).
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
	if (preset == PRESET_BLACK_ALDER || preset == PRESET_WHITE_WILLOW) {
		// Wet-ground trees stand in waterfall spray too (gorge alder and willow carr).
		p_species->set_rule_range(PlantSpecies::RULE_SPRAY, Vector2(0.0f, 1.0f));
	}
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
Ref<PlantSpecies> TreeGenerator::_stand_species(Preset p_preset, bool p_stand_grown, int p_variants, Form p_form) {
	if (p_form == FORM_OPEN && p_stand_grown) {
		p_form = FORM_STAND;
	}
	p_stand_grown = p_form != FORM_OPEN;
	// Wood lying on the ground does not sway: logs and fallen branches are never rigged.
	const bool saved_rigged = rigged;
	if (p_form == FORM_LOG || p_form == FORM_BRANCH || p_form == FORM_STICKS) {
		rigged = false;
	}
	const int key = (int(p_preset) * 8 + int(p_form)) * 2 + (rigged ? 1 : 0);
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
		const int saved_decay = decay_class;
		const bool saved_fallen = fallen;
		const bool saved_branch = fallen_branch;
		const Climber saved_climber = climber;
		const float saved_climber_cover = climber_cover;
		if (p_form == FORM_SAPLING) {
			params[PARAM_YEARS] = MAX(6.0f, Math::round(params[PARAM_YEARS] * 0.2f));
		}
		for (int v = 0; v < n; v++) {
			seed = saved_seed * 31 + v;
			if (p_form == FORM_SNAG || p_form == FORM_LOG) {
				decay_class = 2 + v % 3;
				fallen = p_form == FORM_LOG;
			} else if (p_form == FORM_BRANCH || p_form == FORM_STICKS) {
				// Freshly shed (twigs still on) to well rotted.
				decay_class = 1 + v % 3;
				fallen_branch = true;
				shed_radius_max = p_form == FORM_STICKS ? 0.04f : 0.14f;
			}
			// Climbers on some of the variants of their usual hosts (section 5b): ivy on about a
			// quarter of rough- and smooth-barked broadleaves (and their snags), lianas on another
			// share in rich woods, honeysuckle twining in shrubs. The generator's climber_cover
			// scales them (0: none).
			climber = CLIMBER_NONE;
			if (saved_climber_cover > 0.0f && (p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_SNAG)) {
				const uint32_t bit = 1u << int(p_preset);
				const uint32_t ivy_hosts = (1u << PRESET_OAK) | (1u << PRESET_BEECH) | (1u << PRESET_ASH) | (1u << PRESET_SYCAMORE_MAPLE) | (1u << PRESET_LIME) |
						(1u << PRESET_BLACK_ALDER) | (1u << PRESET_WHITE_WILLOW) | (1u << PRESET_HORSE_CHESTNUT) | (1u << PRESET_HAWTHORN) | (1u << PRESET_ELDER) |
						(1u << PRESET_SUGAR_MAPLE) | (1u << PRESET_RED_MAPLE) | (1u << PRESET_RED_OAK) | (1u << PRESET_WHITE_OAK) | (1u << PRESET_AMERICAN_BEECH) | (1u << PRESET_YELLOW_BIRCH);
				const uint32_t liana_hosts = (1u << PRESET_OAK) | (1u << PRESET_ASH) | (1u << PRESET_LIME) | (1u << PRESET_SYCAMORE_MAPLE) | (1u << PRESET_BLACK_ALDER) |
						(1u << PRESET_WHITE_WILLOW) | (1u << PRESET_SUGAR_MAPLE) | (1u << PRESET_RED_MAPLE) | (1u << PRESET_RED_OAK) | (1u << PRESET_WHITE_OAK);
				const uint32_t twine_hosts = (1u << PRESET_HAZEL) | (1u << PRESET_HAWTHORN) | (1u << PRESET_ROWAN) | (1u << PRESET_ELDER) | (1u << PRESET_WITCH_HAZEL);
				if (v % 4 == 1 && (ivy_hosts & bit)) {
					climber = CLIMBER_IVY;
					climber_cover = saved_climber_cover * Math::lerp(0.5f, 1.0f, float((uint32_t(seed) * 2654435761u) >> 24) / 255.0f);
				} else if (v % 4 == 3 && p_form != FORM_SNAG && (liana_hosts & bit)) {
					climber = CLIMBER_LIANA;
					climber_cover = saved_climber_cover;
				} else if (v % 4 == 2 && p_form != FORM_SNAG && (twine_hosts & bit)) {
					climber = CLIMBER_HONEYSUCKLE;
					climber_cover = saved_climber_cover;
				}
			}
			Ref<ArrayMesh> mesh = generate();
			crown += crown_radius;
			if (p_variants > 0) {
				meshes.push_back(mesh);
			}
		}
		decay_class = saved_decay;
		fallen = saved_fallen;
		fallen_branch = saved_branch;
		shed_radius_max = 0.14f;
		climber = saved_climber;
		climber_cover = saved_climber_cover;
		mesh_cache[key] = meshes;
		crown_cache[key] = crown / n;
		// One full-needle variant for the near range (the field repeats it over the variants):
		// about 30M vertices for a spruce, so one only, and no LOD copies since it is only drawn
		// close. GPU memory and path tracing cost of it are not yet measured.
		if (params[PARAM_NEEDLES] > 0.5f && near_needle_range > 0.0f && p_variants > 0 && (p_form == FORM_OPEN || p_form == FORM_STAND)) {
			needle_sprays = false;
			skip_lods = true;
			seed = saved_seed * 31;
			TypedArray<Mesh> near;
			near.push_back(generate());
			near_cache[key] = near;
			skip_lods = false;
		}

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
	sp->set_meta("preset", int(p_preset));
	sp->set_meta("leaf_preset", int(p_preset)); // The leaves it drops: get_leaf_form(leaf_preset).
	if (p_form == FORM_LOG || p_form == FORM_BRANCH || p_form == FORM_STICKS) {
		sp->set_meta("deadwood", true);
	}
	static const char *form_names[] = { " (open)", " (stand)", " (sapling)", " (snag)", " (log)", " (fallen branch)", " (sticks)" };
	sp->set_name(String(preset_names[p_preset]) + form_names[p_form]);
	TypedArray<Mesh> meshes = mesh_cache[key];
	if (p_variants < meshes.size()) {
		meshes.resize(p_variants);
	}
	sp->set_meshes(meshes);
	if (near_needle_range > 0.0f && near_cache.has(key)) {
		// PlantSpecies near_meshes/near_range (vegetation module); a no-op without them.
		sp->set("near_meshes", near_cache[key]);
		sp->set("near_range", near_needle_range);
	}
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
	switch (p_form) {
		case FORM_SAPLING:
			// Advance regeneration waits under the canopy as far as the species bears shade
			// (beech, fir, hemlock, sugar maple deep; oak, pine, birch, larch only in gaps).
			sp->set_spacing(h.stand_spacing * 0.5f);
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, MIN(h.shade_max + 0.1f, 0.97f)));
			sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.05f);
			sp->set_canopy_opacity(h.opacity * 0.5f);
			break;
		case FORM_SNAG:
			// A dead crown casts little shade; snags stand between the living trees.
			sp->set_spacing(h.stand_spacing * 2.5f);
			sp->set_canopy_radius(crown * 0.3f);
			sp->set_canopy_opacity(0.1f);
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
			break;
		case FORM_BRANCH:
			sp->set_spacing(h.stand_spacing * 0.9f);
			sp->set_canopy_radius(0.0f);
			sp->set_canopy_opacity(0.0f);
			sp->set_align_to_ground(1.0f);
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
			break;
		case FORM_STICKS:
			// Dense and close. Beech branches of 1-10 cm rot within about 18 years (t95; Muller-
			// Using and Bartsch, Eur. J. Forest Res. 128, 2009), a mean stay near 6 years, so a
			// branch fall of roughly 1 Mg/ha a year keeps about 6 Mg/ha (11 m3/ha) on the floor.
			// Sticks every 1.5 m (about 1,600/ha, 1-4 cm) plus the shed limbs come close to that.
			sp->set_spacing(1.5f);
			sp->set_canopy_radius(0.0f);
			sp->set_canopy_opacity(0.0f);
			sp->set_align_to_ground(1.0f);
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
			break;
		case FORM_LOG:
			sp->set_spacing(h.stand_spacing * 2.5f);
			sp->set_canopy_radius(0.0f);
			sp->set_canopy_opacity(0.0f);
			sp->set_align_to_ground(1.0f); // Lies along the slope.
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
			break;
		default:
			break;
	}
	{
		// Seen from far above (the distant terrain's plant cover map): a crown is its leaf colour
		// darkened by its own shade, hiding most of the ground; snags and logs show weathered
		// wood over little ground.
		TreeGenerator look;
		look.set_preset(p_preset);
		const Color leaf = look.get_leaf_color(), bark = look.get_bark_color();
		const float self_shade = conifer ? 0.7f : 0.8f;
		// Seasons: the turned share takes the autumn colour; a bare crown hides little ground.
		const LeafSeason ls = _leaf_season(p_preset, season);
		const Color crown = leaf.lerp(ls.color, ls.season_blend());
		const float leafy = ls.out * (1.0f - ls.fallen);
		Color far = Color(crown.r * self_shade, crown.g * self_shade, crown.b * self_shade, Math::lerp(0.35f, conifer ? 0.85f : 0.9f, leafy));
		if (p_form == FORM_SAPLING) {
			far.a = 0.5f;
		} else if (p_form == FORM_SNAG || p_form == FORM_LOG || p_form == FORM_BRANCH || p_form == FORM_STICKS) {
			const Color wood = bark.lerp(Color(0.42f, 0.39f, 0.33f), 0.6f);
			far = Color(wood.r, wood.g, wood.b, p_form == FORM_SNAG ? 0.15f : 0.1f);
		}
		sp->set_far_color(far);
	}
	rigged = saved_rigged;
	return sp;
}

Ref<PlantSpecies> TreeGenerator::make_species(bool p_stand_grown, int p_variants) {
	return _stand_species(preset, p_stand_grown, p_variants);
}

void TreeGenerator::clear_cache() {
	mesh_cache.clear();
	crown_cache.clear();
	near_cache.clear();
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

	Preset lead = PRESET_MAX; // The stand's leading tree, whose saplings, snags and logs it holds.
	switch (p_stand) {
		case STAND_BEECH: {
			lead = PRESET_BEECH;
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
			lead = PRESET_OAK;
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
			lead = PRESET_SILVER_FIR;
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
			lead = PRESET_NORWAY_SPRUCE;
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
			lead = PRESET_SCOTS_PINE;
			// Scots pine on dry ground, open enough for birch and rowan beneath.
			const Vector2 dry(0.0f, 750.0f);
			clip(canopy(PRESET_SCOTS_PINE, 0.9f, 80.0f, 0.85f, false, true), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(open(PRESET_SCOTS_PINE, 0.35f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(open(PRESET_BIRCH, 0.4f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
			clip(shrub(PRESET_ROWAN, 0.3f), PlantSpecies::RULE_PRECIPITATION, dry, 100.0f);
		} break;
		case STAND_RIPARIAN: {
			lead = PRESET_BLACK_ALDER;
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
			lead = PRESET_SUGAR_MAPLE;
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
			lead = PRESET_RED_OAK;
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
			lead = PRESET_HEMLOCK;
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
	if (lead != PRESET_MAX && !list.is_empty()) {
		// Every stand also regenerates and dies (docs/ideas/trees.md section 5): saplings of its
		// leading tree wait beneath it, and snags and logs of it lie between the living trees,
		// about what an unmanaged temperate forest holds (Christensen et al., For. Ecol. Manage.
		// 210, 2005: 30-130 m3/ha of deadwood in European beech reserves; managed stands far less).
		// The belt rules come from the lead's own canopy entry (the stand's first species can be
		// one held to wet hollows).
		Ref<PlantSpecies> first = list[0];
		for (int i = 0; i < list.size(); i++) {
			const Ref<PlantSpecies> c = list[i];
			if (int(c->get_meta("preset", -1)) == int(lead)) {
				first = c;
				break;
			}
		}
		Ref<PlantSpecies> sap = _stand_species(lead, true, p_variants, FORM_SAPLING);
		sap->set_density(0.6f);
		Ref<PlantSpecies> snag = _stand_species(lead, true, p_variants, FORM_SNAG);
		snag->set_density(0.25f);
		Ref<PlantSpecies> log = _stand_species(lead, true, p_variants, FORM_LOG);
		log->set_density(0.4f);
		Ref<PlantSpecies> branch = _stand_species(lead, true, p_variants, FORM_BRANCH);
		branch->set_density(0.7f);
		Ref<PlantSpecies> sticks = _stand_species(lead, true, p_variants, FORM_STICKS);
		sticks->set_density(1.0f);
		for (const Ref<PlantSpecies> &extra : { sap, snag, log, branch, sticks }) {
			// Inside the stand's own belt.
			for (int r = 0; r < PlantSpecies::RULE_MAX; r++) {
				if (PlantSpecies::Rule(r) == PlantSpecies::RULE_SHADE) {
					continue;
				}
				extra->set_rule_range(PlantSpecies::Rule(r), first->get_rule_range(PlantSpecies::Rule(r)));
				extra->set_rule_fade(PlantSpecies::Rule(r), first->get_rule_fade(PlantSpecies::Rule(r)));
			}
			list.push_back(extra);
		}
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
	broken_tip = -1;
	// Decay recolours bark and needles for this mesh only, so a generator reused for several
	// classes (the stand snags and logs) starts each one from the living colours.
	const Color living_bark = bark_color;
	const Color living_leaf = leaf_color;
	if (decay_class > 0) {
		_apply_decay();
	}
	if (fallen_branch) {
		_shed_branch();
	} else if (fallen) {
		_fell();
	}
	Ref<ArrayMesh> mesh = _build_mesh();
	if (fallen || fallen_branch) {
		// Where the wood lies (mesh space), for plants grown through and around it: the thickest
		// axis from the root, start, end and radius at its start.
		int s0 = -1;
		for (int i = 1; i < int(nodes.size()); i++) {
			const Node &n = nodes[i];
			if (n.removed || (n.parent >= 0 && !nodes[n.parent].removed && nodes[n.parent].main == i)) {
				continue;
			}
			if (s0 < 0 || n.radius > nodes[s0].radius) {
				s0 = i;
			}
		}
		if (s0 >= 0) {
			int last = s0;
			for (int k = s0; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
				last = k;
			}
			const Vector3 a = nodes[s0].start, b = nodes[last].end();
			PackedFloat32Array axis;
			for (float v : { a.x, a.y, a.z, b.x, b.y, b.z, nodes[s0].radius }) {
				axis.push_back(v);
			}
			mesh->set_meta(SNAME("tree_deadwood_axis"), axis);
		}
	}
	bark_color = living_bark;
	leaf_color = living_leaf;
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
	ClassDB::bind_method(D_METHOD("set_decay_class", "decay_class"), &TreeGenerator::set_decay_class);
	ClassDB::bind_method(D_METHOD("get_decay_class"), &TreeGenerator::get_decay_class);
	ClassDB::bind_method(D_METHOD("set_fallen", "fallen"), &TreeGenerator::set_fallen);
	ClassDB::bind_method(D_METHOD("is_fallen"), &TreeGenerator::is_fallen);
	ClassDB::bind_method(D_METHOD("set_fallen_branch", "fallen_branch"), &TreeGenerator::set_fallen_branch);
	ClassDB::bind_method(D_METHOD("set_moss_cover", "cover"), &TreeGenerator::set_moss_cover);
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("get_leaf_form", "preset"), &TreeGenerator::get_leaf_form);
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("get_leaf_season", "preset", "season"), &TreeGenerator::get_leaf_season);
	ClassDB::bind_method(D_METHOD("set_season", "season"), &TreeGenerator::set_season);
	ClassDB::bind_method(D_METHOD("get_season"), &TreeGenerator::get_season);
	ClassDB::bind_method(D_METHOD("set_compress_vertices", "enable"), &TreeGenerator::set_compress_vertices);
	ClassDB::bind_method(D_METHOD("is_compress_vertices"), &TreeGenerator::is_compress_vertices);
	ClassDB::bind_method(D_METHOD("set_climber", "climber"), &TreeGenerator::set_climber);
	ClassDB::bind_method(D_METHOD("get_climber"), &TreeGenerator::get_climber);
	ClassDB::bind_method(D_METHOD("set_climber_cover", "cover"), &TreeGenerator::set_climber_cover);
	ClassDB::bind_method(D_METHOD("get_climber_cover"), &TreeGenerator::get_climber_cover);
	ClassDB::bind_method(D_METHOD("get_moss_cover"), &TreeGenerator::get_moss_cover);
	ClassDB::bind_method(D_METHOD("is_fallen_branch"), &TreeGenerator::is_fallen_branch);
	ClassDB::bind_method(D_METHOD("set_envelope", "envelope"), &TreeGenerator::set_envelope);
	ClassDB::bind_method(D_METHOD("get_envelope"), &TreeGenerator::get_envelope);
	ClassDB::bind_method(D_METHOD("set_collider_radius", "radius"), &TreeGenerator::set_collider_radius);
	ClassDB::bind_method(D_METHOD("get_collider_radius"), &TreeGenerator::get_collider_radius);
	ClassDB::bind_method(D_METHOD("get_collision_capsules"), &TreeGenerator::get_collision_capsules);
	ClassDB::bind_method(D_METHOD("set_bark_relief", "relief"), &TreeGenerator::set_bark_relief);
	ClassDB::bind_method(D_METHOD("get_bark_relief"), &TreeGenerator::get_bark_relief);
	ClassDB::bind_method(D_METHOD("set_rigged", "rigged"), &TreeGenerator::set_rigged);
	ClassDB::bind_method(D_METHOD("is_rigged"), &TreeGenerator::is_rigged);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &TreeGenerator::get_bone_count);
	ClassDB::bind_method(D_METHOD("set_near_needle_range", "range"), &TreeGenerator::set_near_needle_range);
	ClassDB::bind_method(D_METHOD("get_near_needle_range"), &TreeGenerator::get_near_needle_range);
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
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "season"), "set_season", "get_season");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "phyllotaxis", PROPERTY_HINT_ENUM, "Alternate,Opposite,Whorled"), "set_phyllotaxis", "get_phyllotaxis");
	ADD_GROUP("Stand", "forest_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forest_shade", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_forest_shade", "get_forest_shade");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask_channel", PROPERTY_HINT_ENUM, "None:-1,Red:0,Green:1,Blue:2,Alpha:3"), "set_mask_channel", "get_mask_channel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mask_invert"), "set_mask_invert", "get_mask_invert");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "needle_sprays"), "set_needle_sprays", "get_needle_sprays");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rigged"), "set_rigged", "is_rigged");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bark_relief", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_bark_relief", "get_bark_relief");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "decay_class", PROPERTY_HINT_RANGE, "0,5,1"), "set_decay_class", "get_decay_class");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fallen"), "set_fallen", "is_fallen");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fallen_branch"), "set_fallen_branch", "is_fallen_branch");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moss_cover", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_moss_cover", "get_moss_cover");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "compress_vertices"), "set_compress_vertices", "is_compress_vertices");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "climber", PROPERTY_HINT_ENUM, "None,Ivy,Liana,Honeysuckle"), "set_climber", "get_climber");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "climber_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_climber_cover", "get_climber_cover");
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "envelope"), "set_envelope", "get_envelope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collider_radius", PROPERTY_HINT_RANGE, "0,1,0.01,suffix:m"), "set_collider_radius", "get_collider_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_needle_range", PROPERTY_HINT_RANGE, "0,100,0.5,suffix:m"), "set_near_needle_range", "get_near_needle_range");
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
	BIND_ENUM_CONSTANT(LEAF_SIMPLE);
	BIND_ENUM_CONSTANT(LEAF_LOBED);
	BIND_ENUM_CONSTANT(LEAF_PALMATE);
	BIND_ENUM_CONSTANT(LEAF_PINNATE);
	BIND_ENUM_CONSTANT(LEAF_DIGITATE);
	BIND_ENUM_CONSTANT(LEAF_NEEDLE);
	BIND_ENUM_CONSTANT(CLIMBER_NONE);
	BIND_ENUM_CONSTANT(CLIMBER_IVY);
	BIND_ENUM_CONSTANT(CLIMBER_LIANA);
	BIND_ENUM_CONSTANT(CLIMBER_HONEYSUCKLE);
	BIND_ENUM_CONSTANT(CLIMBER_MAX);
	BIND_ENUM_CONSTANT(STAND_RIPARIAN);
	BIND_ENUM_CONSTANT(STAND_HEDGEROW);
	BIND_ENUM_CONSTANT(STAND_NORTHERN_HARDWOOD);
	BIND_ENUM_CONSTANT(STAND_EASTERN_OAK);
	BIND_ENUM_CONSTANT(STAND_PINE_HEMLOCK);
	BIND_ENUM_CONSTANT(STAND_MAX);
	BIND_ENUM_CONSTANT(REGION_EUROPE);
	BIND_ENUM_CONSTANT(REGION_NORTH_AMERICA);
	BIND_ENUM_CONSTANT(REGION_MAX);

	BIND_CONSTANT(MAX_BONES);
	BIND_CONSTANT(RIG_STRIDE);
	BIND_ENUM_CONSTANT(RIG_PARENT);
	BIND_ENUM_CONSTANT(RIG_PIVOT_X);
	BIND_ENUM_CONSTANT(RIG_PIVOT_Y);
	BIND_ENUM_CONSTANT(RIG_PIVOT_Z);
	BIND_ENUM_CONSTANT(RIG_CENTROID_X);
	BIND_ENUM_CONSTANT(RIG_CENTROID_Y);
	BIND_ENUM_CONSTANT(RIG_CENTROID_Z);
	BIND_ENUM_CONSTANT(RIG_AREA);
	BIND_ENUM_CONSTANT(RIG_STIFFNESS);
	BIND_ENUM_CONSTANT(RIG_INERTIA);
	BIND_ENUM_CONSTANT(RIG_ZETA);
	BIND_ENUM_CONSTANT(RIG_RADIUS);
	BIND_ENUM_CONSTANT(RIG_LENGTH);
	BIND_ENUM_CONSTANT(RIG_DIR_X);
	BIND_ENUM_CONSTANT(RIG_DIR_Y);
	BIND_ENUM_CONSTANT(RIG_DIR_Z);
	BIND_ENUM_CONSTANT(RIG_GROUP);
	BIND_ENUM_CONSTANT(RIG_MASS);
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

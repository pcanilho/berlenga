/**************************************************************************/
/*  tree_generator.cpp                                                    */
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

#include "tree_generator.h"
#include "plant_generator.h"
#include "modules/weather/weather_map_3d.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

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
	"Northern Hardwood Forest", "Eastern Oak Forest", "Pine-Hemlock Forest", "Coppice with Standards", "Wood Pasture", "Pollard Row"
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
			// Grey-brown: the old red-brown read as plastic pipe on a coppice stool's rods.
			bark_color = Color(0.43f, 0.36f, 0.3f);
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
	// A willow pollard's withies grow unbranched, long straight rods, for their first years
	// (trees.md 5l; osier and pollard willow, Rackham 2006).
	if (preset == PRESET_WHITE_WILLOW && cut_height > 0.5f && nodes[p_node].pole && nodes[p_node].born - last_cut_year <= 6) {
		return;
	}
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
	if (forest_shade <= 0.0f && _nurse_scrub_height() > 0.0f) {
		// Shelter in youth (trees.md 2f): most open-grown oaks came up through thorny scrub or a
		// browse line (Vera 2000; the wood-pasture oaks of Rackham 2006), which shaded out and
		// browsed their low limbs and left a clear trunk under a crown that spread above it.
		// Shade to the scrub's height, off the stem's own line, kills those limbs as a stand does.
		const float scrub = _nurse_scrub_height();
		const Vector3 base = nodes[0].start;
		for (int y = 0; y < grid_size.y; y++) {
			const float wy = grid_origin.y + (y + 0.5f) * cell - base.y;
			const float below = CLAMP((scrub - wy) / MAX(0.6f, cell), 0.0f, 1.0f);
			if (below <= 0.0f) {
				continue;
			}
			for (int z = 0; z < grid_size.z; z++) {
				for (int x = 0; x < grid_size.x; x++) {
					const float dx = grid_origin.x + (x + 0.5f) * cell - base.x;
					const float dz = grid_origin.z + (z + 0.5f) * cell - base.z;
					const float r = Math::sqrt(dx * dx + dz * dz);
					shadow[_cell_index(Vector3i(x, y, z))] += 1.6f * below * Math::smoothstep(0.2f, 0.2f + 1.5f * cell, r);
				}
			}
		}
	}
	(void)p_year;
}

// The height of the scrub an open-grown tree came up through, 0 for none: about two in three
// pedunculate oaks by seed, 3 to 4.5 m (trees.md 2f); the rest forked low in the open.
float TreeGenerator::_nurse_scrub_height() const {
	if (preset != PRESET_OAK || fallen || fallen_branch) {
		return 0.0f;
	}
	const uint32_t h = uint32_t(seed) * 2654435761u + 0x6e757273u;
	const uint32_t m = (h ^ (h >> 15)) * 2246822519u;
	const float u = float((m ^ (m >> 13)) & 0xffffu) / 65535.0f;
	if (u >= 0.67f) {
		return 0.0f;
	}
	return Math::lerp(3.0f, 4.5f, u / 0.67f);
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
	// Cutting rejuvenates (Read 2000; Rackham 2006): the poles of a pollard or a stool grow like
	// a young tree's shoots, on a root system already grown. Aged by the tree's own years, a
	// lapsed beech pollard's poles made internodes a tenth of a young tree's and after sixty
	// years stood a 10 m crown on a 2.5 m head. Once lapsed (a cycle past the last cut; worked
	// rods keep their tuned growth) they age from the last cut, ten years on.
	const bool lapsed = cut_height > 0.0f && last_cut_year > 0 && p_year - last_cut_year > cut_cycle;
	const float base_len = _internode_length(lapsed ? MIN(p_year, p_year - last_cut_year + 10) : p_year);
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
			const int ord = (buds[bi].terminal || buds[bi].sprout || buds[bi].node == 0) ? on.order : on.order + 1;
			buds[bi].vigor *= Math::pow(params[PARAM_APICAL_CONTROL], float(ord));
			// A cut stem's root system feeds a few poles: they shoot up 1-3 m a year (coppice
			// regrowth; Rackham 2006).
			if ((buds[bi].sprout || (buds[bi].terminal && on.pole)) && p_year - last_cut_year <= 4) {
				buds[bi].vigor *= 1.6f;
			}
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
		const int order = (bud.terminal || bud.sprout || from_root) ? from.order : from.order + 1;
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
		const bool pole = bud.sprout || (bud.terminal && from.pole);
		if (params[PARAM_LEADER_GROWTH] > 0.0f) { // Every shoot: a branch turned up to replace the leader obeys it too.
			// A resprout's first few years only: then the poles grow on as limbs.
			len = MIN(len, params[PARAM_LEADER_GROWTH] * (pole && p_year - last_cut_year <= 4 ? 2.0f : 1.0f) / float(count));
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
			nodes[idx].pole = pole;
			if (k == 0 && bud.sprout) {
				sprout_roots.insert(idx);
			}
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
	// Under the scrub of an open-grown tree's youth (trees.md 2f) the limbs off the stem die once
	// the leader has grown clear of it: shaded and browsed, they drop and leave a clear trunk.
	float scrub = forest_shade <= 0.0f ? _nurse_scrub_height() : 0.0f;
	if (scrub > 0.0f) {
		float stem_top = nodes[0].start.y;
		for (const Node &n : nodes) {
			if (!n.removed && n.order == 0) {
				stem_top = MAX(stem_top, n.end().y);
			}
		}
		if (stem_top - nodes[0].start.y < scrub + 0.5f) {
			scrub = 0.0f;
		}
	}
	LocalVector<int> stack;
	for (int i = 1; i < int(nodes.size()); i++) {
		Node &n = nodes[i];
		if (n.removed || n.dead || n.parent <= 0 || nodes[n.parent].main == i || n.born > p_year - 2) {
			continue;
		}
		// A cut tree's young poles are fed by the whole root system: on a worked tree they are
		// cut again before they thin out among themselves (Rackham 2006).
		// The knuckles round the head are its own wood, not shaded limbs: shed, they took
		// every pole on them.
		if ((n.pole && (p_year - last_cut_year <= MAX(5, cut_cycle))) || (cut_height > 0.0f && n.order == 0 && n.born == 0)) {
			continue;
		}
		const bool under_scrub = scrub > 0.0f && nodes[n.parent].order == 0 && n.start.y - nodes[0].start.y < scrub;
		const float light = (n.light_main + n.light_lateral) / float(MAX(1, n.count));
		if (light >= ratio && !under_scrub) {
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
		// Branches resting on the ground run along it instead of through it. The axis keeps its
		// own radius clear of the soil: held only at 8 cm, a thick low limb sank half its girth
		// into the ground and its bark showed a ragged line where it crossed the surface.
		const float clear = MAX(0.08f, n.radius + 0.03f);
		if (n.order > 0 && n.start.y + n.dir.y * n.length < clear && n.dir.y < 0.0f) {
			Vector3 h(n.dir.x, 0.0f, n.dir.z);
			h = h.length_squared() > 1e-6f ? h.normalized() : Vector3(1, 0, 0);
			const float drop = MIN(0.0f, (clear - n.start.y) / MAX(n.length, 1e-4f));
			n.dir = (h + Vector3(0, drop, 0)).normalized();
		}
	}
}

// Limbs through wood. Growth keeps buds apart through the shadow grid, not the wood they become,
// and the radii (pipe model) and the sag under gravity come after growth, so a limb can end up
// running through a neighbouring stem or limb. In a real crown two touching branches rub, the
// bark wears through and the thinner one dies back to the contact (branch abrasion; crown
// shyness, Putz et al. 1984, Fish et al. 2006). So the thinner wood is cut just short of the
// contact and everything beyond it goes. Stems (order 0) are never cut. Contacts within the
// junction where a limb leaves its parent are the weld's, not collisions.
void TreeGenerator::_prune_collisions() {
	collision_prunes = 0;
	collision_deflects = 0;
	broken_ends.clear();
	const int count = int(nodes.size());
	const float min_r = 0.012f; // Thinner wood is twigs, hidden in the foliage.
	// A clipped hedge keeps to its envelope: a limb swung away would leave it, so it is cut.
	const bool clipped = envelope.size != Vector3();
	const float cell_size = 0.75f;
	HashMap<Vector3i, LocalVector<int>> grid;
	auto cell_of = [&](const Vector3 &p) {
		return Vector3i(int(Math::floor(p.x / cell_size)), int(Math::floor(p.y / cell_size)), int(Math::floor(p.z / cell_size)));
	};
	for (int i = 1; i < count; i++) {
		const Node &n = nodes[i];
		if (n.removed || n.radius < min_r || n.length <= 0.0f) {
			continue;
		}
		const Vector3 a = n.start, b = n.end();
		const Vector3 lo = a.min(b) - Vector3(n.radius, n.radius, n.radius);
		const Vector3 hi = a.max(b) + Vector3(n.radius, n.radius, n.radius);
		const Vector3i c0 = cell_of(lo), c1 = cell_of(hi);
		for (int x = c0.x; x <= c1.x; x++) {
			for (int y = c0.y; y <= c1.y; y++) {
				for (int z = c0.z; z <= c1.z; z++) {
					grid[Vector3i(x, y, z)].push_back(i);
				}
			}
		}
	}
	// Closest points of two segments (Ericson, Real-Time Collision Detection 5.1.9).
	auto closest = [](const Vector3 &p1, const Vector3 &q1, const Vector3 &p2, const Vector3 &q2, float &r_s, float &r_t) {
		const Vector3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
		const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
		float s = 0.0f, t = 0.0f;
		if (a <= 1e-9f && e <= 1e-9f) {
			r_s = r_t = 0.0f;
			return r.length();
		}
		if (a <= 1e-9f) {
			t = CLAMP(f / e, 0.0f, 1.0f);
		} else {
			const float c = d1.dot(r);
			if (e <= 1e-9f) {
				s = CLAMP(-c / a, 0.0f, 1.0f);
			} else {
				const float bb = d1.dot(d2);
				const float denom = a * e - bb * bb;
				s = denom > 1e-9f ? CLAMP((bb * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
				t = (bb * s + f) / e;
				if (t < 0.0f) {
					t = 0.0f;
					s = CLAMP(-c / a, 0.0f, 1.0f);
				} else if (t > 1.0f) {
					t = 1.0f;
					s = CLAMP((bb - c) / a, 0.0f, 1.0f);
				}
			}
		}
		r_s = s;
		r_t = t;
		return (p1 + d1 * s - (p2 + d2 * t)).length();
	};
	// Ancestors within a few internodes share the junction; they are never a collision.
	auto near_kin = [&](int p_a, int p_b) {
		for (int k = 0, x = p_a; k < 4 && x > 0; k++, x = nodes[x].parent) {
			if (nodes[x].parent == p_b) {
				return true;
			}
		}
		for (int k = 0, x = p_b; k < 4 && x > 0; k++, x = nodes[x].parent) {
			if (nodes[x].parent == p_a) {
				return true;
			}
		}
		return false;
	};
	// The first node of the axis a node is on (axes continue through main children).
	auto axis_of = [&](int p_node) {
		int x = p_node;
		while (nodes[x].parent > 0 && nodes[nodes[x].parent].main == x) {
			x = nodes[x].parent;
		}
		return x;
	};
	LocalVector<int> subtree;
	auto deflect = [&](int p_node, const Vector3 &p_on_n, const Vector3 &p_on_o, float p_clear) -> bool {
		const int root = axis_of(p_node);
		if (root <= 0 || nodes[root].parent <= 0 || nodes[root].order == 0) {
			return false;
		}
		const Vector3 pivot = nodes[root].start;
		const Vector3 arm = p_on_n - pivot;
		Vector3 away = p_on_n - p_on_o;
		const float dist = away.length();
		if (arm.length() < 1e-3f || dist < 1e-5f) {
			return false;
		}
		away /= dist;
		const Vector3 axis = arm.cross(away);
		if (axis.length_squared() < 1e-10f) {
			return false;
		}
		const float need = 1.05f * p_clear - dist + 0.25f * p_clear;
		const float angle = need / arm.length();
		if (angle > Math::deg_to_rad(30.0f)) {
			return false;
		}
		const Basis rot(axis.normalized(), angle);
		subtree.clear();
		subtree.push_back(root);
		for (uint32_t k = 0; k < subtree.size(); k++) {
			const Node &m = nodes[subtree[k]];
			if (m.main >= 0 && !nodes[m.main].removed) {
				subtree.push_back(m.main);
			}
			for (int c = m.first_lateral; c >= 0; c = nodes[c].next_sibling) {
				if (!nodes[c].removed) {
					subtree.push_back(c);
				}
			}
		}
		for (int k : subtree) {
			Node &m = nodes[k];
			m.start = pivot + rot.xform(m.start - pivot);
			m.dir = rot.xform(m.dir).normalized();
		}
		return true;
	};
	// After a swing: the moved wood in its new cells. The grid keeps the old cells too; tests
	// use the current positions, so a stale entry only costs a distance test.
	auto regrid = [&]() {
		for (int k : subtree) {
			const Node &m = nodes[k];
			if (m.radius < min_r || m.length <= 0.0f) {
				continue;
			}
			const Vector3 ea = m.start, eb = m.end();
			const Vector3 rr(m.radius, m.radius, m.radius);
			const Vector3i c0 = cell_of(ea.min(eb) - rr), c1 = cell_of(ea.max(eb) + rr);
			for (int x = c0.x; x <= c1.x; x++) {
				for (int y = c0.y; y <= c1.y; y++) {
					for (int z = c0.z; z <= c1.z; z++) {
						grid[Vector3i(x, y, z)].push_back(k);
					}
				}
			}
		}
	};
	auto is_ancestor = [&](int p_anc, int p_of) {
		for (int x = nodes[p_of].parent; x > 0; x = nodes[x].parent) {
			if (x == p_anc) {
				return true;
			}
		}
		return false;
	};
	int tries = 0;
	for (int i = 1; i < count; i++) {
		Node &n = nodes[i];
		if (n.removed) {
			continue;
		}
		if (n.parent > 0 && nodes[n.parent].removed) {
			n.removed = true;
			continue;
		}
		// Twigs (4-12 mm) are tested only against scaffold wood (5 cm and up): inside the crown
		// they hide among the leaves, but one passing through a big limb shows in plain view.
		const bool twig = n.radius < min_r;
		if (n.order == 0 || n.radius < 0.004f || n.length <= 0.0f) {
			continue;
		}
		const Vector3 a = n.start, b = n.end();
		const Vector3i c0 = cell_of(a.min(b)), c1 = cell_of(a.max(b));
		float cut = 2.0f; // Fraction of the internode kept, 2: no contact.
		bool moved = false;
		for (int x = c0.x; x <= c1.x && cut > 1.0f && !moved; x++) {
			for (int y = c0.y; y <= c1.y && cut > 1.0f && !moved; y++) {
				for (int z = c0.z; z <= c1.z && cut > 1.0f && !moved; z++) {
					const LocalVector<int> *cell = grid.getptr(Vector3i(x, y, z));
					if (!cell) {
						continue;
					}
					for (int j : *cell) {
						const Node &o = nodes[j];
						if (j == i || o.removed || o.radius < n.radius || (twig && o.radius < 0.05f) || near_kin(i, j) || is_ancestor(j, i) || is_ancestor(i, j)) {
							continue;
						}
						float s, t;
						const float d = closest(a, b, o.start, o.end(), s, t);
						if (d >= 0.85f * (n.radius + o.radius)) {
							continue;
						}
						// Inside the parent's wood (where every limb starts, on the parent's axis) the
						// limbs overlap by construction: the weld's business. Outside it, a sibling
						// limb crossing near the trunk is a collision like any other.
						if (n.parent > 0) {
							const Node &par = nodes[n.parent];
							const Vector3 p = a + (b - a) * s;
							float ps, pt;
							const Vector3 pa = par.start, pb2 = par.end();
							const Vector3 pm = par.main >= 0 && !nodes[par.main].removed ? nodes[par.main].end() : pb2;
							const float dp = MIN(closest(p, p, pa, pb2, ps, pt), closest(p, p, pb2, pm, ps, pt));
							if (dp < 1.05f * par.radius + 0.25f * n.radius) {
								continue;
							}
						}
						// Plastic trees (Pirk et al., SIGGRAPH 2012): a limb grows away from an
						// obstacle rather than into it. Swing the thinner limb's whole branch about
						// where it leaves its parent, away from the other wood, up to 20 degrees;
						// only if that cannot clear it does the limb rub through and die back.
						if (tries < 3 && !clipped && deflect(i, a + (b - a) * s, o.start + (o.end() - o.start) * t, n.radius + o.radius)) {
							collision_deflects++;
							moved = true;
							break;
						}
						cut = MIN(cut, s);
						break;
					}
				}
			}
		}
		if (moved) {
			// Test this internode again where it now is.
			regrid();
			tries++;
			i--;
			continue;
		}
		tries = 0;
		if (cut <= 1.0f) {
			const float keep = cut * n.length - 1.2f * n.radius;
			// A thick limb cut short reads as a sawn-off pipe (no tree self-prunes a living limb
			// that way): the whole limb goes back to its parent instead.
			const int root = axis_of(i);
			const bool stub = n.radius >= 0.03f;
			if (stub) {
				nodes[root].removed = true; // The axis and everything on it follow.
				for (int x = i; x != root; x = nodes[x].parent) {
					nodes[x].removed = true;
				}
			} else if (keep < 0.05f || keep < 2.0f * n.radius) {
				n.removed = true; // Children follow as the loop reaches them.
			} else {
				n.length = keep;
				if (n.main >= 0) {
					nodes[n.main].removed = true;
				}
				for (int c = n.first_lateral; c >= 0; c = nodes[c].next_sibling) {
					nodes[c].removed = true;
				}
				n.leafy = false;
				if (n.radius >= 0.015f) {
					// Rubbed through and snapped: a broken end with splinters, not a saw cut.
					broken_ends.insert(i);
				}
			}
			collision_prunes++;
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

// _tube's first ring frame when non-zero (a stem built in pieces carries it on), and its last.
// Per thread: meshes are built on worker threads too.
static thread_local Vector3 tube_carry_nrm;
static thread_local Vector3 tube_last_nrm;

// A generalized cylinder along a polyline, framed by parallel transport (Bishop frames).
void TreeGenerator::_tube(Builder &r_b, const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, int p_sides, float p_v0, const Color &p_color, const Image *p_height, float p_relief, float p_girth_r, bool p_soil_fade) {
	const int rings = int(p_points.size());
	if (rings < 2) {
		return;
	}
	const int base = r_b.verts.size();
	Vector3 t = (p_points[1] - p_points[0]).normalized();
	Vector3 ref = Math::abs(t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	Vector3 nrm = t.cross(ref).normalized();
	if (tube_carry_nrm != Vector3()) {
		// Continue a previous tube's frame (a stem built in pieces), so the sides line up.
		const Vector3 c = tube_carry_nrm - t * tube_carry_nrm.dot(t);
		if (c.length_squared() > 1e-8f) {
			nrm = c.normalized();
		}
	}
	float v = p_v0;
	// A whole number of bark tiles around the stem (no seam), v in the same tile width so the
	// pattern keeps its aspect; thin shoots carry one tile, a finer pattern, as young bark is.
	// A tube that starts in a closed cap (a log's butt) passes the stem's radius: from the cap's
	// zero radius the bark tiled every few millimetres and mipmapped to a flat colour.
	const float girth = Math::TAU * MAX(p_girth_r > 0.0f ? p_girth_r : p_radii[0], 0.001f);
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
		// The ring's plane follows the axis averaged over about a radius either way: square to
		// the local tangent, a bend tighter than the stem is thick folded the rings on its inner
		// side over each other (rings stacked out of order up a thick bole), which drew as a
		// horizontal seam across the bark.
		Vector3 t2;
		if (r == 0) {
			t2 = t;
		} else {
			int a = r - 1, b = MIN(r + 1, rings - 1);
			const float reach = p_radii[r];
			while (a > 0 && p_points[r].distance_to(p_points[a]) < reach) {
				a--;
			}
			while (b < rings - 1 && p_points[r].distance_to(p_points[b]) < reach) {
				b++;
			}
			t2 = (p_points[b] - p_points[a]).normalized();
		}
		// Transport the normal from the previous tangent to this one.
		const Vector3 axis = t.cross(t2);
		if (axis.length_squared() > 1e-10f) {
			nrm = nrm.rotated(axis.normalized(), Math::acos(CLAMP(t.dot(t2), -1.0f, 1.0f)));
		}
		nrm = (nrm - t2 * nrm.dot(t2)).normalized();
		t = t2;
		tube_last_nrm = nrm;
		const Vector3 bin = t.cross(nrm);
		// A tube that starts in a closed cap: v runs over the surface (the cap's rings are close
		// along the axis but far apart across it, which smeared the bark into radial streaks), and
		// the normals lean back with the radius' slope, so the cap is lit as an end face rather
		// than as more of the cylinder.
		const bool capped = p_girth_r > 0.0f;
		if (r > 0) {
			const float d = p_points[r].distance_to(p_points[r - 1]);
			v += (capped ? Math::sqrt(d * d + (p_radii[r] - p_radii[r - 1]) * (p_radii[r] - p_radii[r - 1])) : d) / circumference;
		}
		float slope = 0.0f;
		if (capped) {
			const int r0 = MAX(r - 1, 0), r1 = MIN(r + 1, rings - 1);
			const float len = p_points[r1].distance_to(p_points[r0]);
			slope = len > 1e-6f ? CLAMP((p_radii[r1] - p_radii[r0]) / len, -6.0f, 6.0f) : 0.0f;
		}
		for (int s = 0; s <= p_sides; s++) {
			const float ang = Math::TAU * s / p_sides;
			const Vector3 dir = nrm * Math::cos(ang) + bin * Math::sin(ang);
			float rr = p_radii[r];
			if (hdata) {
				// Faded out at the soil line (the tree's ground is y = 0): plates and furrows cut
				// the ground plane in a ragged line, read as a rim around the root collar. Not on
				// fallen wood, which lies across that plane: its underside lost its furrows.
				const float gy = p_points[r].y + dir.y * rr;
				// None on a cap's end face: the furrows ran into its centre as pleats.
				const float face = 1.0f - CLAMP(Math::abs(slope) - 0.5f, 0.0f, 1.0f);
				rr += p_relief * relief_at(float(s) / p_sides * repeats, v) * (p_soil_fade ? Math::smoothstep(-0.02f, 0.2f, gy) : 1.0f) * face;
			}
			const Vector3 n_out = capped ? (dir - t * slope).normalized() : dir;
			// The cap's end face is mapped flat across it, in the colour of weathered end grain:
			// mapped round the stem, the bark tiles converged on its centre as radial pleats.
			const bool end_face = capped && Math::abs(slope) > 1.0f;
			Vector3 tongue;
			if (end_face) {
				// A diffuse fracture (trees.md 5d): the fibres broke at different lengths, so the
				// face stands out in tongues along the grain, a sector of the girth each, longest
				// in the heartwood, shorter at the bark rim.
				// The length runs between 22 sectors' values in straight lines, so neighbouring
				// tongues meet in sloped ridges: stepped, the walls between them faced away from
				// their own shading normals and rendered black under the path tracer.
				auto len_at = [&](int p_sector) {
					uint32_t h = uint32_t(p_sector % 22) * 374761393u + uint32_t(Math::round(p_girth_r * 1000.0f)) * 2246822519u;
					h = (h ^ (h >> 13)) * 1274126177u;
					const float u = float((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
					return u * Math::sqrt(u);
				};
				const float sa = ang / float(Math::TAU) * 22.0f;
				const int s0 = int(Math::floor(sa));
				const float u01 = Math::lerp(len_at(((s0 % 22) + 22) % 22), len_at((((s0 + 1) % 22) + 22) % 22), sa - Math::floor(sa));
				const float heart = 1.0f - 0.6f * CLAMP(p_radii[r] / p_girth_r, 0.0f, 1.0f);
				tongue = -t * (p_girth_r * (0.03f + 0.6f * u01) * heart);
			}
			if (hdata) {
				while (r_b.relief_off.size() < uint32_t(r_b.verts.size())) {
					r_b.relief_off.push_back(Vector2());
				}
				r_b.relief_off.push_back(Vector2(rr - p_radii[r], p_relief));
			}
			r_b.verts.push_back(p_points[r] + dir * rr + tongue);
			r_b.normals.push_back(n_out);
			if (end_face) {
				const float rf = p_radii[r] / BARK_TILE;
				r_b.not_bark.push_back(uint32_t(r_b.verts.size() - 1));
				r_b.uvs.push_back(Vector2(0.5f + rf * Math::cos(ang), 0.5f + rf * Math::sin(ang)));
				r_b.colors.push_back(p_color.lerp(Color(0.3f, 0.24f, 0.17f), 0.7f));
			} else {
				r_b.uvs.push_back(Vector2(float(s) / p_sides * repeats, v));
				r_b.colors.push_back(p_color);
			}
			if (r_b.use_tangents) {
				// Along u; the binormal (normal x tangent) then runs along the stem, with v. On the
				// end face u runs along the frame's normal axis.
				const Vector3 tan = end_face ? (nrm - n_out * nrm.dot(n_out)).normalized() : bin * Math::cos(ang) - nrm * Math::sin(ang);
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
			// Counter-clockwise seen from outside, as the other bark builders wind; _build_mesh()
			// flips the bark and moss to Godot's clockwise front faces.
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
	// Rows of three vertices (midrib, two margins) along the blade. The base and the tip have no
	// width, so they are one vertex each: three there would make four zero-area triangles and
	// four spare vertices per blade (half of a two-step leaflet), and under flutter the margin
	// pair at the tip would split into a sliver.
	LocalVector<int> row;
	row.resize(p_steps + 1);
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
		row[i] = r_b.verts.size();
		r_b.verts.push_back(mid);
		r_b.normals.push_back(p_n);
		r_b.uvs.push_back(Vector2(0.5f, t));
		r_b.colors.push_back(p_color);
		if (i == 0 || i == p_steps) {
			continue;
		}
		r_b.verts.push_back(mid + p_side * w + fold);
		r_b.normals.push_back((p_n - p_side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(1.0f, t));
		r_b.colors.push_back(p_color);
		r_b.verts.push_back(mid - p_side * w + fold);
		r_b.normals.push_back((p_n + p_side * 0.2f).normalized());
		r_b.uvs.push_back(Vector2(0.0f, t));
		r_b.colors.push_back(p_color);
	}
	auto tri = [&](int a, int b, int c) {
		r_b.indices.push_back(a);
		r_b.indices.push_back(b);
		r_b.indices.push_back(c);
	};
	for (int i = 0; i < p_steps; i++) {
		const int m0 = row[i], m1 = row[i + 1];
		const bool base = i == 0, tip = i + 1 == p_steps;
		if (base && tip) {
			continue; // One step: a line.
		}
		if (base) {
			tri(m0, m1, m1 + 1);
			tri(m0, m1 + 2, m1);
		} else if (tip) {
			tri(m0, m1, m0 + 1);
			tri(m0, m0 + 2, m1);
		} else {
			tri(m0, m1, m0 + 1);
			tri(m0 + 1, m1, m1 + 1);
			tri(m0, m0 + 2, m1);
			tri(m0 + 2, m1 + 2, m1);
		}
	}
}

// The twin's area over the lens formula's, so the twin covers what the species blade covers
// (lobes cut into the blade, leaflets with gaps between them): measured once per species on
// one unit leaf. Twins sized by the formula alone came out up to 16% larger than the leaflets of
// ash or horse chestnut, and far crowns read darker than near ones.
float TreeGenerator::_twin_area_scale() {
	if (twin_scale_preset == preset && twin_scale_width == params[PARAM_LEAF_WIDTH]) {
		return twin_scale;
	}
	twin_scale_preset = preset;
	twin_scale_width = params[PARAM_LEAF_WIDTH];
	twin_scale = 1.0f;
	const LeafFormSpec &form = LEAF_FORMS[preset];
	if (form.kind == LEAF_SIMPLE || form.kind == LEAF_NEEDLE) {
		return twin_scale;
	}
	const bool compound = form.kind == LEAF_PINNATE || form.kind == LEAF_DIGITATE;
	const int n_eff = compound ? MAX(2, form.count / 3) : 1;
	const float hw = params[PARAM_LEAF_WIDTH];
	const float slot_area = 2.0f * 0.656f * hw * float(n_eff);
	const float k = Math::sqrt(float(n_eff));
	auto area = [](const Builder &b) {
		double a = 0.0;
		for (int i = 0; i + 2 < b.indices.size(); i += 3) {
			const Vector3 p0 = b.verts[b.indices[i]], p1 = b.verts[b.indices[i + 1]], p2 = b.verts[b.indices[i + 2]];
			a += 0.5 * (p1 - p0).cross(p2 - p0).length();
		}
		return a;
	};
	Builder sb, lb;
	const Vector3 along(0, 0, 1), side(1, 0, 0), n(0, 1, 0);
	_add_species_blade(sb, Vector3(), along, side, n, 1.0f, hw, slot_area, Color(1, 1, 1));
	_add_blade(lb, Vector3(), along, side, n, k, hw * k, 3, 0, 0.0f, Color(1, 1, 1));
	const double sa = area(sb), la = area(lb);
	if (sa > 0.0 && la > 0.0) {
		twin_scale = CLAMP(float(sa / la), 0.5f, 2.0f);
	}
	return twin_scale;
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
	if (far_leaves) {
		// A far mesh (near_leaf_range): the twin is the leaf at every level. Lobes and leaflets
		// are under a pixel beyond the near range; the near mesh keeps them.
		const float k = Math::sqrt(float(n_eff) * _twin_area_scale());
		_add_blade(r_b, start, along, side, n, p_size * k, hw * k, 3, 0, 0.0f, p_color);
		return;
	}
	{
		const float k = Math::sqrt(float(n_eff) * _twin_area_scale());
		const int ti = leaf_twins.indices.size(), tv = leaf_twins.verts.size();
		_add_blade(leaf_twins, start, along, side, n, p_size * k, hw * k, 3, 0, 0.0f, p_color);
		leaf_twin_of[int(r_b.parts.size())] = Vector4i(ti, leaf_twins.indices.size() - ti, tv, leaf_twins.verts.size() - tv);
	}
	_add_species_blade(r_b, start, along, side, n, p_size, hw, slot_area, p_color);
}

// The species' own blade: lobes, a palm of lobes, or leaflets, on the slot area of the lens.
void TreeGenerator::_add_species_blade(Builder &r_b, const Vector3 &start, const Vector3 &along, const Vector3 &side, const Vector3 &n, float p_size, float hw, float slot_area, const Color &p_color) {
	const LeafFormSpec &form = LEAF_FORMS[preset];
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
			// Leaflets in opposite pairs along the rachis, with one at the tip. Each is lanceolate
			// (rowan narrow, about 3:1; Stace, New Flora of the British Isles, 2019); two rows made
			// a flat diamond.
			const int pairs = form.count / 2;
			// Half-width over length: rowan's leaflets are narrow (about 3:1 to 4:1), ash's a
			// little broader, elder's broadest.
			const float hwr = preset == PRESET_ROWAN ? 0.15f : (preset == PRESET_ELDER ? 0.24f : 0.19f);
			// Three rows, a quarter wider than the leaflet: the leaflet texture cuts the
			// lanceolate outline and its teeth from it (_leaflet_texture).
			const int steps = 3;
			const float hull = hwr * LEAFLET_HULL;
			// Leaflets of 2 x hwr l x l x 0.656 each share the slots' area.
			const float leaflet = Math::sqrt(slot_area / (float(form.count) * 2.0f * 0.656f * hwr));
			const float rachis = leaflet * 0.6f * float(pairs + 1);
			for (int i = 0; i < pairs; i++) {
				const float t = Math::lerp(0.25f, 0.9f, pairs > 1 ? float(i) / (pairs - 1) : 0.5f);
				const Vector3 at = start + along * (rachis * t) + n * (-0.15f * rachis * t * t);
				for (int sgn = -1; sgn <= 1; sgn += 2) {
					const Vector3 dir = (side * float(sgn) * 0.85f + along * 0.5f).normalized();
					_add_blade(r_b, at, dir, n.cross(dir).normalized(), n, leaflet, leaflet * hull, steps, 0, 0.0f, p_color, false, 1.0f);
				}
			}
			const Vector3 tip = start + along * rachis + n * (-0.15f * rachis);
			_add_blade(r_b, tip, along, side, n, leaflet, leaflet * hull, steps, 0, 0.0f, p_color, false, 1.0f);
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
		case PRESET_HAZEL:
			// Glossy grey-brown with dense pale lenticels and thin skin peeling in strips
			// (Corylus avellana; Vaucher, Tree Bark, 2003).
		case PRESET_ROWAN:
			// Rowan too: smooth, grey to purplish, with pale lenticels drawn out across the stem
			// (Raspe et al. 2000); a plain smooth bark read as bare grey tubes up close.
			return BARK_LENTICELLED;
		case PRESET_BEECH:
		case PRESET_AMERICAN_BEECH:
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
		{ 6, 6, 0.0f, 0.0006f, 0.0004f, 1.0f, 0.1f }, // Smooth: no plates, fine grain, lenticels, creases.
		{ 6, 3, 0.1f, 0.028f, 0.004f, 0.3f, 0.12f }, // Furrowed: long interlaced ridges.
		{ 4, 2, 0.16f, 0.03f, 0.0025f, 0.25f, 0.2f }, // Plated: wide flat plates, deep furrows.
		{ 12, 9, 0.06f, 0.006f, 0.0015f, 0.45f, 0.14f }, // Scaly: small overlapping scales.
		{ 5, 4, 0.03f, 0.003f, 0.001f, 0.6f, 0.0f }, // Flaking: shallow edges, colour mosaic.
		{ 3, 40, 0.0f, 0.0008f, 0.0004f, 1.0f, 0.04f }, // Papery: smooth, lenticel bands.
		{ 6, 6, 0.0f, 0.0006f, 0.0004f, 1.0f, 0.12f }, // Lenticelled: smooth, dense raised pale lenticels, peeling skin.
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
			if (p_kind == BARK_LENTICELLED) {
				// Thin outer skin peeling in short strips along the stem, the fresh bark under
				// it paler and coppery; glossy elsewhere.
				const float strip = bark_fbm(u, v, 10, 3, 3, salt + 81u);
				const float peel = Math::smoothstep(0.18f, 0.26f, strip);
				a = a * Math::lerp(1.0f, 1.25f, peel);
				a.r *= Math::lerp(1.0f, 1.08f, peel);
				height -= 0.0004f * peel;
				r = Math::lerp(0.55f, 0.7f, peel) + 0.05f * fibre;
				// Lenticels: dense short pale dashes across the stem, a little raised.
				// Fewer and larger than the fine scale would give: at a few metres they still
				// read as the rods' pale speckle rather than mip away to a flat tone.
				const int cu = 11, cv = 24;
				const float lx = u * cu, ly = v * cv;
				const int ix = int(lx), iy = int(ly);
				const float on = bark_hash(ix, iy, salt + 61u);
				if (on > 0.4f) {
					const float cx = 0.5f + 0.3f * (bark_hash(ix, iy, salt + 63u) - 0.5f);
					const float cy = 0.5f + 0.4f * (bark_hash(ix, iy, salt + 67u) - 0.5f);
					const float len = 0.18f + 0.2f * bark_hash(ix, iy, salt + 65u);
					const float dx = (lx - ix - cx) / len, dy = (ly - iy - cy) / 0.16f;
					const float m = 1.0f - Math::smoothstep(0.55f, 1.0f, dx * dx + dy * dy);
					a = a.lerp(Color(2.1f, 1.95f, 1.65f), 0.85f * m);
					height += m * 0.00015f;
					r = Math::lerp(r, 0.75f, m);
				}
				const float tone = 1.0f + L.mottle * bark_fbm(u, v, 3, 6, 3, salt + 71u) * 2.0f;
				a = a * tone;
			}
			if (p_kind == BARK_SMOOTH || p_kind == BARK_PAPERY) {
				// Lenticels: short dashes across the stem (long and dark on birch).
				const int cu = p_kind == BARK_PAPERY ? 5 : 14, cv = p_kind == BARK_PAPERY ? 26 : 24;
				const float lx = u * cu, ly = v * cv;
				const int ix = int(lx), iy = int(ly);
				const float on = bark_hash(ix, iy, salt + 61u);
				if (on > (p_kind == BARK_PAPERY ? 0.62f : 0.74f)) {
					const float cx = 0.5f + 0.3f * (bark_hash(ix, iy, salt + 63u) - 0.5f);
					const float len = p_kind == BARK_PAPERY ? 0.1f + 0.4f * bark_hash(ix, iy, salt + 65u) : 0.1f + 0.08f * on;
					// Off the cell's middle both ways, so they do not line up in rows.
					const float cy = 0.5f + 0.5f * (bark_hash(ix, iy, salt + 67u) - 0.5f);
					const float dx = (lx - ix - cx) / len, dy = (ly - iy - cy) / (p_kind == BARK_PAPERY ? 0.18f : 0.1f);
					const float m = 1.0f - Math::smoothstep(0.6f, 1.0f, dx * dx + dy * dy);
					const float lent = p_kind == BARK_PAPERY ? 0.18f : 0.84f;
					a = a * Math::lerp(1.0f, lent, m);
					height += m * 0.0004f;
					r = Math::lerp(r, 0.97f, m);
				}
				if (p_kind == BARK_SMOOTH) {
					// Creases: short fine wrinkles across the stem where the thin bark stretched
					// as the stem thickened (beech, hornbeam, hazel; Vaucher, Tree Bark, 2003),
					// a few centimetres long, darker in their fold.
					const int wu_n = 9, wv_n = 70;
					const float qx = u * wu_n, qy = v * wv_n;
					const int jx = int(qx), jy = int(qy);
					const float pick = bark_hash(jx, jy, salt + 91u);
					if (pick > 0.55f) {
						const float cx = 0.5f + 0.4f * (bark_hash(jx, jy, salt + 93u) - 0.5f);
						const float half = 0.25f + 0.35f * bark_hash(jx, jy, salt + 95u);
						const float cy = 0.5f + 0.4f * (bark_hash(jx, jy, salt + 97u) - 0.5f);
						// A gentle bow: the fold sags towards its ends.
						const float dxw = (qx - jx - cx) / half;
						const float dyw = (qy - jy - cy - 0.15f * dxw * dxw) / 0.09f;
						const float m = (1.0f - Math::smoothstep(0.5f, 1.0f, Math::abs(dxw))) * (1.0f - Math::smoothstep(0.3f, 1.0f, Math::abs(dyw)));
						a = a * Math::lerp(1.0f, 0.8f, m);
						height -= m * 0.0005f;
					}
				}
				// Patches over a tile and finer blotches: the bark's own tone and the thin films of
				// green algae and crust lichens on it.
				const float tone = 1.0f + L.mottle * (bark_fbm(u, v, 3, 6, 3, salt + 71u) * 2.0f + (p_kind == BARK_SMOOTH ? bark_fbm(u, v, 2, 2, 3, salt + 73u) * 1.5f : 0.0f));
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
	{
		// The roughness map's free blue channel carries the relief, 0 in the deepest furrow and
		// 1 on the highest ridge, for layers that keep to ridges or furrows (lichen, trees.md 2h).
		float lo = 1e9f, hi = -1e9f;
		for (int k = 0; k < N * N; k++) {
			lo = MIN(lo, hw[k]);
			hi = MAX(hi, hw[k]);
		}
		set.relief_floor = lo;
		const float span = MAX(hi - lo, 1e-6f);
		for (int k = 0; k < N * N; k++) {
			rbytes.write[k * 3 + 2] = uint8_t(CLAMP((hw[k] - lo) / span, 0.0f, 1.0f) * 255.0f);
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
	// What the displaced near tube adds over a smooth one (trees.md 2g). The relief tube samples
	// this map on a grid about 2.5 cm around and 5 cm along (32 x 64 texels); bilinear between
	// its vertices, it differs from the map by its reconstruction error. A smooth tube differs by
	// the relief itself. Where the two are about the same (smooth, papery, flaking and scaly bark:
	// their features are under a millimetre deep or smaller than the grid), the displaced tube
	// costs four to ten times the triangles and shows nothing the normal map does not. Errors are
	// the 90th percentile over the tile, in metres.
	{
		const float *H = reinterpret_cast<const float *>(hbytes.ptr());
		auto at = [&](int i, int j) { return H[((j % N + N) % N) * N + ((i % N + N) % N)]; };
		auto grid_error = [&](int p_ku, int p_kv) {
			LocalVector<float> errs;
			for (int j = 0; j < N; j += 4) {
				for (int i = 0; i < N; i += 4) {
					float rec = 0.0f;
					if (p_ku > 0) {
						const int i0 = (i / p_ku) * p_ku, j0 = (j / p_kv) * p_kv;
						const float fx = float(i - i0) / p_ku, fy = float(j - j0) / p_kv;
						rec = Math::lerp(Math::lerp(at(i0, j0), at(i0 + p_ku, j0), fx), Math::lerp(at(i0, j0 + p_kv), at(i0 + p_ku, j0 + p_kv), fx), fy);
					}
					errs.push_back(Math::abs(rec - at(i, j)));
				}
			}
			errs.sort();
			return errs[int(errs.size() * 0.9f)];
		};
		// The smooth tube sits at the plate tops (height 0), so its error is the relief's depth.
		const float flat = grid_error(0, 1);
		const float grid = grid_error(32, 64);
		set.relief_gain = MAX(0.0f, flat - grid);
		// Rings can be further apart along a stem when the pattern runs along it (furrowed bark's
		// long ridges): the longest spacing whose error stays within half a millimetre of 5 cm.
		set.relief_along = 0.05f;
		for (int kv : { 96, 128 }) {
			if (grid_error(32, kv) <= grid + 0.0005f) {
				set.relief_along = kv * texel;
			}
		}
		if (OS::get_singleton()->has_environment("TREE_RELIEF_DEBUG")) {
			print_line(vformat("bark kind %d: relief p90 %.1f mm, grid error %.1f mm, gain %.1f mm, rings every %.1f cm", p_kind, flat * 1000.0f, grid * 1000.0f, set.relief_gain * 1000.0f, set.relief_along * 100.0f));
		}
	}
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
float TreeGenerator::_moss_at(const Vector3 &p_pos, const Vector3 &p_normal, float p_ground, float p_amount, bool p_clamp) const {
	const float h = MAX(p_pos.y - p_ground, 0.0f);
	// Fallen wood lies within the skirt's height all round, which spread the moss evenly over
	// its underside too. On a log it grows over the upper half and down the flanks, and the
	// face pressed into the litter stays bare (Soderstrom 1988).
	// A coppice stool stands open to sun and wind after every cut: moss in patches only.
	const float foot = (fallen || fallen_branch) ? Math::smoothstep(-0.45f, 0.25f, p_normal.y) : Math::exp(-h / 1.1f) * (cut_height > 0.0f && cut_height <= 0.5f ? 0.5f : 1.0f);
	const float up = p_normal.y;
	const float top = CLAMP((up - 0.35f) / 0.5f, 0.0f, 1.0f) * 0.75f;
	const float patch = 0.65f * moss_noise(p_pos * 2.2f, 0x6d6f7373u + uint32_t(seed)) + 0.35f * moss_noise(p_pos * 7.0f, 0x6d6f7374u);
	float w = p_amount * MAX(foot, top) * (1.9f * patch - 0.25f);
	float bare = 0.0f;
	if (foot_roots.on && !fallen && !fallen_branch && h < 0.6f) {
		// Out on a surface root's upper face the bark shows (it is drier, scuffed and shed of
		// litter); the moss gathers in the crotches between roots and round the stem's foot.
		const Vector2 rel(p_pos.x - foot_roots.axis.x, p_pos.z - foot_roots.axis.z);
		const float r = rel.length();
		const float out = (r - foot_roots.trunk_r * 1.25f);
		if (out > 0.0f) {
			const float az = Math::atan2(rel.y, rel.x);
			for (uint32_t k = 0; k < foot_roots.az.size(); k++) {
				if (foot_roots.len[k] <= 0.0f) {
					continue;
				}
				// About where along the root this distance falls (the root's centre runs out
				// to the flare's edge plus its length).
				const float t = CLAMP((r - foot_roots.trunk_r * 1.1f - foot_roots.width) / foot_roots.len[k], 0.0f, 1.0f);
				const float lat = Math::abs(Math::angle_difference(az, foot_roots.az[k] + foot_roots.bend[k] * t * t)) * r;
				const float on_root = 1.0f - Math::smoothstep(1.0f, 2.0f, lat / foot_roots.width);
				bare = MAX(bare, on_root * Math::smoothstep(0.0f, 0.2f, out / foot_roots.len[k]) * Math::smoothstep(-0.4f, 0.3f, p_normal.y));
			}
			// Patchy, not a clean line: moss creeps up the roots' sides in tongues.
			bare *= Math::smoothstep(0.0f, 0.4f, patch + 0.3f * bare);
		}
	}
	if (!fallen && !fallen_branch) {
		// The collar at the foot (trees.md 5d; Studlar 1982): stemflow and splash keep the lowest
		// bark wet all round, so the moss there is a near-continuous sleeve rather than patches,
		// its upper edge wandering between about 15 and 60 cm and frayed into tongues; the
		// patches only take over above it.
		float edge = (0.15f + 0.45f * moss_noise(p_pos * Vector3(2.6f, 0.4f, 2.6f), 0x636f6c6cu + uint32_t(seed))) * CLAMP(p_amount * 1.5f, 0.35f, 1.0f);
		if (cut_height > 0.0f && cut_height <= 0.5f) {
			// A coppice stool is cut every few years and stands in the open after each cut: the
			// sleeve keeps to its foot, under the stubs and knuckles.
			edge = MIN(edge, cut_height * 0.45f);
		}
		const float fray = 0.2f * (moss_noise(p_pos * 9.0f, 0x66726179u) - 0.5f);
		const float collar = 1.0f - Math::smoothstep(edge - 0.18f, edge + 0.18f, h + fray);
		w = MAX(w, collar * MIN(1.0f, p_amount * 1.6f) * (0.95f + 0.25f * patch));
		w *= 1.0f - 0.9f * bare;
	}
	return p_clamp ? CLAMP(w, 0.0f, 1.0f) : w;
}

// Epiphytic lichens, by species (trees.md 2h): how readily the bark carries them, the share of
// leafy (foliose) rosettes against crusts, and of orange Xanthoria. Smooth bark (beech, hazel,
// holly, rowan, red maple) carries mostly crustose script and wart lichens (Graphis, Pertusaria,
// Lecanora); acid, rough bark (oaks, birch, conifers) leafy Parmelia and Hypogymnia; nutrient-rich
// bark (ash, sycamore, elder, willow) Physcia, Flavoparmelia and orange Xanthoria. Flaking and
// shedding bark (yew, plane-like flakes, box) keeps few (Barkman 1958; Gilbert, Lichens, 2000).
struct LichenAffinity {
	float amount, leafy, orange;
};
static const LichenAffinity LICHEN_AFFINITY[TreeGenerator::PRESET_MAX] = {
	{ 0.8f, 0.6f, 0.03f }, // Oak.
	{ 0.85f, 0.15f, 0.0f }, // Beech.
	{ 0.5f, 0.6f, 0.0f }, // Birch.
	{ 0.45f, 0.7f, 0.0f }, // Scots pine.
	{ 0.6f, 0.7f, 0.0f }, // Norway spruce.
	{ 0.9f, 0.2f, 0.03f }, // Hazel.
	{ 0.8f, 0.5f, 0.12f }, // Hawthorn.
	{ 0.6f, 0.4f, 0.08f }, // Lime.
	{ 0.75f, 0.35f, 0.18f }, // Sycamore maple.
	{ 0.85f, 0.35f, 0.22f }, // Ash.
	{ 0.5f, 0.3f, 0.08f }, // Horse chestnut.
	{ 0.7f, 0.4f, 0.08f }, // Black alder.
	{ 0.6f, 0.4f, 0.28f }, // White willow.
	{ 0.6f, 0.6f, 0.0f }, // Larch.
	{ 0.7f, 0.45f, 0.0f }, // Silver fir.
	{ 0.8f, 0.3f, 0.04f }, // Rowan.
	{ 0.8f, 0.3f, 0.4f }, // Elder.
	{ 0.8f, 0.1f, 0.0f }, // Holly.
	{ 0.2f, 0.3f, 0.0f }, // Yew.
	{ 0.3f, 0.2f, 0.0f }, // Box.
	{ 0.7f, 0.5f, 0.05f }, // Sugar maple.
	{ 0.8f, 0.3f, 0.0f }, // Red maple.
	{ 0.6f, 0.5f, 0.0f }, // Red oak.
	{ 0.7f, 0.6f, 0.0f }, // White oak.
	{ 0.5f, 0.6f, 0.0f }, // White pine.
	{ 0.5f, 0.6f, 0.0f }, // Hemlock.
	{ 0.9f, 0.15f, 0.0f }, // American beech.
	{ 0.4f, 0.6f, 0.0f }, // Paper birch.
	{ 0.5f, 0.5f, 0.0f }, // Yellow birch.
	{ 0.8f, 0.3f, 0.0f }, // Witch hazel.
	{ 0.5f, 0.4f, 0.0f }, // Mountain laurel.
};

float TreeGenerator::_lichen_amount() const {
	if (lichen_cover <= 0.0f) {
		return 0.0f;
	}
	// Standing dead wood keeps its lichens for a while and gains more in the light; once it
	// softens and falls, mosses and fungi take over (Soderstrom 1988; Peytavie et al., DeadWood,
	// 2024).
	static const float DECAY[6] = { 1.0f, 1.15f, 1.0f, 0.6f, 0.3f, 0.1f };
	float k = lichen_cover * LICHEN_AFFINITY[preset].amount * DECAY[CLAMP(decay_class, 0, 5)];
	if (fallen || fallen_branch) {
		k *= 0.5f; // In the litter's damp and shade.
	}
	return k;
}

// Lichen weight 0..1 at a bark point. Lichens need light and dry spells, so they grow where the
// moss does not: up the bole above the splash zone and out on the limbs in the crown, and in a
// stand mostly in its lit upper part (Desbenoit, Galin and Akkouche, Simulating and modeling
// lichen growth, 2004, place them by light and moisture; Barkman 1958). Twigs under a few years
// old have not been colonised yet. Broken into colonies a metre or so apart.
float TreeGenerator::_lichen_at(const Vector3 &p_pos, float p_radius, float p_moss) const {
	const float a = _lichen_amount();
	if (a <= 0.0f) {
		return 0.0f;
	}
	const float ground = nodes.is_empty() ? 0.0f : nodes[0].start.y;
	const float h = p_pos.y - ground;
	const float rise = (fallen || fallen_branch) ? 1.0f : Math::smoothstep(0.15f, 1.4f, h);
	const float crown = Math::smoothstep(0.15f, 0.75f, h / MAX(height, 1.0f));
	const float light = Math::lerp(1.0f, 0.3f + 0.7f * crown, CLAMP(forest_shade / 0.6f, 0.0f, 1.0f));
	const float young = Math::smoothstep(0.004f, 0.02f, p_radius);
	const float patch = 1.7f * moss_noise(p_pos * 1.3f, 0x6c696368u + uint32_t(seed)) - 0.35f;
	return CLAMP(a * light * rise * young * patch * (1.0f - CLAMP(p_moss * 1.3f, 0.0f, 1.0f)), 0.0f, 1.0f);
}

// Bracket fungi (docs/ideas/trees.md 5g). The fruiting bodies of the wood-decay fungi that rot
// a tree's heart or its dead wood: perennial hooves on standing snags (Fomes fomentarius on beech
// and birch, Fomitopsis pinicola on conifers, Fomitopsis betulina on birch), flat shelves at the
// foot of old broadleaves (Ganoderma applanatum), sulphur fans on old oaks (Laetiporus), and tiers
// of thin zoned brackets on logs and stumps (Trametes versicolor). Their tubes grow downwards,
// so every bracket is level whatever the wood does (gravitropism; Rayner and Boddy 1988, Fungal
// Decomposition of Wood; Stokland, Siitonen and Jonsson 2012, Biodiversity in Dead Wood).
namespace {
enum FungusKind {
	FUNGUS_FOMES,
	FUNGUS_PINICOLA,
	FUNGUS_BETULINA,
	FUNGUS_GANODERMA,
	FUNGUS_LAETIPORUS,
	FUNGUS_TRAMETES,
	FUNGUS_BRACKETS, // Kinds below are brackets; from here, gilled mushrooms in clusters.
	FUNGUS_HONEY = FUNGUS_BRACKETS, // Armillaria: at the foot of dying and dead broadleaves.
	FUNGUS_TUFT, // Hypholoma fasciculare, sulphur tuft: on stumps and logs.
	FUNGUS_MAX,
};
struct FungusLook {
	float w0, w1; // Width across the wood (metres).
	float depth; // Out from the wood, x width.
	float height; // Thickness at the back, x depth.
	float margin; // Thickness at the margin, x height.
	float dome; // Top profile exponent: low = hoof, high = flat shelf.
	int zones; // Growth zones on the top.
	float wave; // Wavy margin.
	int tier0, tier1; // Brackets in a cluster.
	Color back, mid, rim, pores, zone;
};
const FungusLook FUNGUS_LOOKS[FUNGUS_BRACKETS] = {
	// Fomes: grey hoof with ridged annual zones, brown margin.
	{ 0.12f, 0.24f, 0.75f, 0.7f, 0.25f, 1.0f, 6, 0.0f, 1, 2, Color(0.17f, 0.16f, 0.15f), Color(0.36f, 0.35f, 0.33f), Color(0.5f, 0.37f, 0.24f), Color(0.42f, 0.32f, 0.22f), Color(0.22f, 0.21f, 0.2f) },
	// Fomitopsis pinicola: dark crust, red band, cream margin.
	{ 0.1f, 0.26f, 0.65f, 0.6f, 0.3f, 1.1f, 5, 0.0f, 1, 2, Color(0.13f, 0.09f, 0.07f), Color(0.5f, 0.14f, 0.07f), Color(0.82f, 0.77f, 0.6f), Color(0.85f, 0.8f, 0.65f), Color(0.2f, 0.1f, 0.07f) },
	// Fomitopsis betulina: smooth buff cushion, rolled white margin.
	{ 0.08f, 0.2f, 0.75f, 0.45f, 0.45f, 1.6f, 0, 0.0f, 1, 1, Color(0.5f, 0.42f, 0.32f), Color(0.72f, 0.66f, 0.55f), Color(0.86f, 0.84f, 0.78f), Color(0.86f, 0.84f, 0.79f), Color(0.66f, 0.6f, 0.5f) },
	// Ganoderma applanatum: flat cocoa-brown shelf, white margin and pores.
	{ 0.15f, 0.38f, 0.6f, 0.28f, 0.35f, 2.0f, 8, 0.03f, 1, 2, Color(0.24f, 0.17f, 0.12f), Color(0.36f, 0.25f, 0.17f), Color(0.84f, 0.81f, 0.74f), Color(0.86f, 0.83f, 0.77f), Color(0.28f, 0.19f, 0.13f) },
	// Laetiporus: orange fans grading to a sulphur margin, in tiers.
	{ 0.1f, 0.28f, 0.6f, 0.22f, 0.45f, 2.5f, 2, 0.08f, 3, 6, Color(0.8f, 0.42f, 0.1f), Color(0.88f, 0.55f, 0.12f), Color(0.95f, 0.82f, 0.25f), Color(0.94f, 0.85f, 0.35f), Color(0.85f, 0.5f, 0.12f) },
	// Trametes: thin, velvety, strongly zoned in browns and greys, cream margin; many tiers.
	{ 0.03f, 0.08f, 0.6f, 0.13f, 0.5f, 3.0f, 10, 0.07f, 4, 9, Color(0.24f, 0.2f, 0.17f), Color(0.42f, 0.38f, 0.33f), Color(0.82f, 0.78f, 0.68f), Color(0.84f, 0.8f, 0.7f), Color(0.2f, 0.22f, 0.25f) },
};
// Gilled mushrooms: a stalk bending up from the wood, a revolved cap kept level, and real gill
// blades under it (Desbenoit, Vanderhaghe, Galin and Grosjean, Interactive modeling of mushrooms,
// Eurographics 2004 short paper: caps as revolved profiles, gills as radial blades).
struct AgaricLook {
	float stalk0, stalk1; // Stalk length (metres).
	float stalk_r; // Stalk radius at the cap.
	float cap0, cap1; // Cap radius.
	int count0, count1; // Mushrooms in a cluster.
	Color cap_mid, cap_centre, gills, stalk, base;
};
const AgaricLook AGARIC_LOOKS[FUNGUS_MAX - FUNGUS_BRACKETS] = {
	// Honey fungus: honey-brown caps with a darker scaly centre, cream gills, a ring on the stalk.
	{ 0.05f, 0.1f, 0.005f, 0.022f, 0.05f, 6, 14, Color(0.62f, 0.44f, 0.2f), Color(0.38f, 0.25f, 0.12f), Color(0.86f, 0.79f, 0.62f), Color(0.72f, 0.62f, 0.45f), Color(0.36f, 0.26f, 0.16f) },
	// Sulphur tuft: sulphur caps with an orange-tan centre, greenish-yellow gills, dense tufts.
	{ 0.035f, 0.075f, 0.0035f, 0.016f, 0.034f, 8, 18, Color(0.86f, 0.77f, 0.32f), Color(0.72f, 0.45f, 0.18f), Color(0.66f, 0.68f, 0.36f), Color(0.82f, 0.74f, 0.42f), Color(0.55f, 0.4f, 0.2f) },
};
} // namespace

// Growth lines and pores, 256 square: the top half is the bracket's top (u across, v out from
// the wood: fine concentric lines and radial fibrils, a velvet), the bottom half its pore face
// (tubes of a quarter to half a millimetre, seen as a fine dark dot grid).
Ref<Texture2D> TreeGenerator::_fungus_texture() {
	if (fungus_texture.is_valid()) {
		return fungus_texture;
	}
	const int N = 256;
	Vector<uint8_t> data;
	data.resize(N * N * 3);
	uint8_t *w = data.ptrw();
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
			float t;
			if (v < 0.5f) {
				const float r = v * 2.0f;
				const float lines = 0.5f + 0.5f * Math::sin((r * 46.0f + 1.5f * bark_fbm(u, r, 8, 8, 2, 0x66756e67u)) * float(Math::TAU));
				const float fibril = bark_fbm(u * 1.0f, r, 96, 6, 2, 0x66696272u);
				t = 0.86f + 0.1f * lines + 0.12f * fibril;
			} else {
				const float px = u * 48.0f, py = (v - 0.5f) * 96.0f;
				const float dx = px - Math::floor(px) - 0.5f, dy = py - Math::floor(py) - 0.5f;
				const float pore = Math::smoothstep(0.18f, 0.32f, Math::sqrt(dx * dx + dy * dy));
				t = 0.72f + 0.26f * pore + 0.06f * bark_fbm(u, v, 16, 16, 2, 0x706f7265u);
			}
			const uint8_t c = uint8_t(CLAMP(t, 0.0f, 1.0f) * 255.0f);
			w[(y * N + x) * 3 + 0] = c;
			w[(y * N + x) * 3 + 1] = c;
			w[(y * N + x) * 3 + 2] = c;
		}
	}
	Ref<Image> img = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, data);
	img->generate_mipmaps();
	fungus_texture = ImageTexture::create_from_image(img);
	return fungus_texture;
}

// Broken wood's texture (trees.md 5j): long fibres along v, a few darker splits, as albedo
// (grey, the vertex colour tints it) or as its tangent-space normal map.
Ref<Texture2D> TreeGenerator::_wood_texture(bool p_normal) {
	Ref<Texture2D> &cache = p_normal ? wood_normal_texture : wood_texture;
	if (cache.is_valid()) {
		return cache;
	}
	const int N = 256;
	LocalVector<float> h;
	h.resize(N * N);
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
			const float fibre = bark_fbm(u, v, 48, 3, 3, 0x776f6f64u);
			const float wander = bark_fbm(u, v, 6, 2, 2, 0x77616e64u);
			const float split = Math::abs(Math::fmod(u * 7.0f + 0.8f * wander + 10.0f, 1.0f) - 0.5f);
			const float run = Math::smoothstep(0.5f, 0.62f, bark_fbm(u, v, 9, 2, 2, 0x72756e73u));
			const float check = Math::lerp(1.0f, Math::smoothstep(0.0f, 0.05f, split), run);
			h[y * N + x] = 0.55f * fibre + 0.45f * check;
		}
	}
	Vector<uint8_t> data;
	data.resize(N * N * 3);
	uint8_t *w = data.ptrw();
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float c = h[y * N + x];
			if (p_normal) {
				const float dx = h[y * N + (x + 1) % N] - h[y * N + (x + N - 1) % N];
				const float dy = h[((y + 1) % N) * N + x] - h[((y + N - 1) % N) * N + x];
				const Vector3 n = Vector3(-dx * 3.0f, -dy * 3.0f, 1.0f).normalized();
				w[(y * N + x) * 3 + 0] = uint8_t(CLAMP(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
				w[(y * N + x) * 3 + 1] = uint8_t(CLAMP(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
				w[(y * N + x) * 3 + 2] = uint8_t(CLAMP(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
			} else {
				const uint8_t g = uint8_t(CLAMP(0.55f + 0.5f * c, 0.0f, 1.0f) * 255.0f);
				w[(y * N + x) * 3 + 0] = g;
				w[(y * N + x) * 3 + 1] = g;
				w[(y * N + x) * 3 + 2] = g;
			}
		}
	}
	Ref<Image> img = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, data);
	img->generate_mipmaps(p_normal);
	cache = ImageTexture::create_from_image(img);
	return cache;
}

// The lying stem's girth along its axis (deadwood interface, trees.md 5b). A root flare, root
// stubs or a torn-out root plate stand out well past the stem's node radius, and the stem's
// tube is often wider than its first node's radius: plants placed beside the log stood inside
// the wood. Measured from the stem's own bark and moss vertices [p_v0, p_v1): the furthest
// reach from the axis in DEADWOOD_SAMPLES bins from end a to end b (the first bin also takes
// all wood behind a, the last all beyond b).
void TreeGenerator::_measure_lying(const Builder &p_b, int p_v0, int p_v1) {
	const Vector3 a = deadwood_a;
	const float len = a.distance_to(deadwood_b);
	if (len < 0.01f) {
		return;
	}
	const Vector3 u = (deadwood_b - a) / len;
	for (int i = p_v0; i < p_v1; i++) {
		const Vector3 rel = p_b.verts[i] - a;
		const float d = rel.dot(u);
		const float perp = (rel - u * d).length();
		const int bin = CLAMP(int(Math::round(d / len * (DEADWOOD_SAMPLES - 1))), 0, DEADWOOD_SAMPLES - 1);
		deadwood_profile[bin] = MAX(deadwood_profile[bin], perp);
		// Round the bent centreline too: the nearest of its spans, binned to the nearest point
		// (a bow in a long stick is not girth).
		const Vector3 v = p_b.verts[i];
		float best = 1e30f;
		int best_k = 0;
		for (int k = 0; k + 1 < DEADWOOD_SAMPLES; k++) {
			const Vector3 p0 = deadwood_line[k], p1 = deadwood_line[k + 1];
			const Vector3 s = p1 - p0;
			const float ls = s.length_squared();
			const float t = ls > 1e-12f ? CLAMP((v - p0).dot(s) / ls, 0.0f, 1.0f) : 0.0f;
			const float dist = v.distance_to(p0 + s * t);
			if (dist < best) {
				best = dist;
				best_k = t < 0.5f ? k : k + 1;
			}
		}
		deadwood_girth[best_k] = MAX(deadwood_girth[best_k], best);
	}
}

// Pollards and coppice (trees.md 5l): woodmen cut trees back for poles, fodder and firewood,
// at head height out of reach of browsing stock (pollards) or at the ground (coppice), every 5-30
// years. Dormant and adventitious buds round the cut resprout into a ring of straight poles of
// one age; cut after cut the stubs and the callus over them build a swollen head (the bolling)
// or a widening stool (Rackham 2006, Woodlands; Read 2000, Veteran Trees: a guide to good
// management). The first cut takes each stem at cut_height and clears the bole below it; later
// cuts take every pole grown since back to a stub, and new sprouts break from the stubs.
float TreeGenerator::_stem_top() const {
	float top = 0.0f;
	for (const Node &n : nodes) {
		if (!n.removed && n.order == 0) {
			top = MAX(top, n.end().y - nodes[0].start.y);
		}
	}
	return top;
}

void TreeGenerator::_cut_back(bool p_first, int p_year) {
	// The girth each piece had when it was cut stays: the stubs and the head keep it.
	_compute_radii();
	const float base_y = nodes[0].start.y;
	const bool coppice = cut_height <= 0.5f;
	LocalVector<int> fresh; // Where the sprouts break this time.
	HashSet<int> cut; // Cut wood: its own buds go with the cut.
	auto clear_children = [&](int p_i) {
		Node &n = nodes[p_i];
		if (n.main >= 0) {
			nodes[n.main].removed = true;
		}
		n.main = -1;
		for (int c = n.first_lateral; c >= 0; c = nodes[c].next_sibling) {
			nodes[c].removed = true;
		}
		n.first_lateral = -1;
		n.leafy = false;
	};
	if (p_first) {
		// Every stem for a coppice stool; a pollard keeps its thickest.
		int thickest = -1;
		for (int i = 1; i < int(nodes.size()); i++) {
			const Node &n = nodes[i];
			if (!n.removed && n.parent == 0 && (thickest < 0 || n.radius > nodes[thickest].radius)) {
				thickest = i;
			}
		}
		for (int i = 1; i < int(nodes.size()); i++) {
			Node &first = nodes[i];
			if (first.removed || first.parent != 0) {
				continue;
			}
			if (!coppice && i != thickest) {
				first.removed = true;
				continue;
			}
			int k = i;
			while (true) {
				Node &n = nodes[k];
				cut.insert(k);
				// The bole below the cut is cleared of its limbs.
				const int keep_main = n.main;
				n.main = -1;
				for (int c = n.first_lateral; c >= 0; c = nodes[c].next_sibling) {
					nodes[c].removed = true;
				}
				n.first_lateral = -1;
				n.leafy = false;
				if (n.end().y - base_y >= cut_height || keep_main < 0 || nodes[keep_main].removed) {
					if (n.dir.y > 0.2f) {
						n.length = MAX((cut_height - (n.start.y - base_y)) / n.dir.y, 0.02f);
					}
					if (keep_main >= 0) {
						nodes[keep_main].removed = true;
					}
					break;
				}
				n.main = keep_main;
				k = keep_main;
			}
			// The head: short knuckles round the cut's rim, each a bud's seat.
			const Node &top = nodes[k];
			const float r = MAX(top.radius, 0.02f);
			const int knuckles = coppice ? 3 + int(rng.rand() % 4) : 6 + int(rng.rand() % 7);
			for (int q = 0; q < knuckles; q++) {
				const float a = float(Math::TAU) * (q + 0.35f * (rng.randf() - 0.5f)) / knuckles;
				const Vector3 out(Math::cos(a), 0.0f, Math::sin(a));
				const Vector3 d = (out + Vector3(0, coppice ? 0.12f : 0.35f, 0)).normalized();
				const int kn = _add_node(k, top.end() - Vector3(0, r * (coppice ? 0.6f : 0.4f), 0), d, r * Math::lerp(1.0f, 1.35f, rng.randf()), top.order, 0, false);
				nodes[kn].leafy = false;
				nodes[kn].radius = r * Math::lerp(0.5f, 0.7f, rng.randf());
				cut_heads.push_back(kn);
				fresh.push_back(kn);
			}
			cut_heads.push_back(k);
		}
	} else {
		// Each pole is sawn off close to the knuckle or stool it grew from, leaving a short stub
		// with a flat cut face; the new sprouts break from round the cut on the knuckle again,
		// not from the stub, so the head swells in place instead of stacking stub on stub. The
		// stubs die back and the callus closes over them within a couple of cycles.
		for (KeyValue<int, int> &kv : cut_stubs) {
			if (p_year - kv.value > 2 * cut_cycle && !nodes[kv.key].removed) {
				nodes[kv.key].removed = true;
			}
		}
		HashSet<int> seats;
		for (const int i : sprout_roots) {
			Node &n = nodes[i];
			if (n.removed || n.parent <= 0) {
				continue;
			}
			bool gone = false;
			for (int p = n.parent; p > 0 && !gone; p = nodes[p].parent) {
				gone = nodes[p].removed;
			}
			if (gone) {
				continue;
			}
			clear_children(i);
			cut.insert(i);
			n.length = MIN(n.length, CLAMP(n.radius * 0.9f, 0.03f, 0.1f));
			n.pole = false;
			cut_stubs.insert(i, p_year);
			if (!seats.has(n.parent)) {
				seats.insert(n.parent);
				fresh.push_back(n.parent);
				// The callus over each cut builds the knuckle out a little. A coppice stool spreads
				// outward by about a centimetre a year as its rim resprouts (Rackham 2006): old
				// stools are rings of stubs a metre and more across.
				Node &seat = nodes[n.parent];
				float grow = 0.0f;
				if (coppice) {
					grow = MIN(0.012f * cut_cycle, MAX(0.0f, 0.45f - seat.length));
				} else if (seat.order == n.order && seat.length < seat.radius * 2.5f) {
					grow = MIN(0.3f * seat.radius, 0.03f);
				}
				if (grow > 0.0f) {
					seat.length += grow;
					const Vector3 shift = seat.dir * grow;
					LocalVector<uint8_t> moved;
					moved.resize(nodes.size());
					memset(moved.ptr(), 0, moved.size());
					moved[n.parent] = 1;
					for (int c = n.parent + 1; c < int(nodes.size()); c++) {
						const int pc = nodes[c].parent;
						if (pc >= 0 && moved[pc]) {
							moved[c] = 1;
							nodes[c].start += shift;
						}
					}
				}
				seat_cuts[n.parent] = (seat_cuts.has(n.parent) ? seat_cuts[n.parent] : 0) + 1;
			}
		}
	}
	sprout_roots.clear();
	for (int i = 1; i < int(nodes.size()); i++) {
		if (!nodes[i].removed && nodes[i].parent > 0 && nodes[nodes[i].parent].removed) {
			nodes[i].removed = true;
		}
	}
	for (Bud &b : buds) {
		if (cut.has(b.node)) {
			b.alive = false;
		}
	}
	// Sprouts: straight poles, steeply up from a pollard head, spreading a little more from a
	// stool, one or two from each stub.
	for (const int i : fresh) {
		if (nodes[i].removed) {
			continue;
		}
		// A willow's head bears a dense fan of withies, five to nine from every seat.
		const bool osier = preset == PRESET_WHITE_WILLOW && !coppice;
		const int n_buds = osier ? 5 + int(rng.rand() % 5) : (p_first ? 1 + int(rng.randf() < 0.4f) : 1 + int(rng.randf() < 0.3f));
		for (int q = 0; q < n_buds; q++) {
			// Out along the knuckle or stub, else any bearing.
			Vector3 out = nodes[i].dir;
			out.y = 0.0f;
			if (out.length() < 0.2f) {
				out = Vector3();
			}
			if (out.length() < 0.01f) {
				const float a = rng.randf() * float(Math::TAU);
				out = Vector3(Math::cos(a), 0.0f, Math::sin(a));
			}
			out.normalize();
			const float tilt = Math::deg_to_rad(Math::lerp(coppice ? 15.0f : 20.0f, coppice ? 40.0f : 55.0f, rng.randf()));
			Bud b;
			b.node = i;
			b.dir = (Vector3(0, Math::cos(tilt), 0) + out * Math::sin(tilt) + _random_unit() * 0.08f).normalized();
			b.sprout = true;
			buds.push_back(b);
		}
	}
}

// The root plate of an uprooted tree (trees.md 5k): a thick disc of soil torn out with the
// lateral roots, standing on edge where the tree tipped over it. Built upright about the old
// base (y up, the ground at 0) and turned with the stem. The old ground surface (litter, moss)
// faces away from the fall; the underside is raw soil, darker humus under the top, stones, and
// the roots running out through it to torn ends past the rim; sinkers hang from under the stem.
void TreeGenerator::_root_plate(Builder &r_bark) {
	Builder &w = soil_b;
	RandomPCG pr;
	pr.seed(uint64_t(seed) * 0x2545f491u + 0x706c6174u);
	const float R = plate.radius, T = plate.thick, tr = plate.trunk_r;
	const float rotten = CLAMP(float(decay_class - 1) / 2.0f, 0.0f, 1.0f);
	const uint32_t salt = uint32_t(seed) * 2654435761u;
	const int sectors = 72;
	// The rim: torn ragged, lobed where the big roots held more soil.
	float rim[sectors];
	for (int k = 0; k < sectors; k++) {
		const float a = float(Math::TAU) * k / sectors;
		const Vector3 q(Math::cos(a) * 1.7f, 0.0f, Math::sin(a) * 1.7f);
		rim[k] = R * (0.82f + 0.3f * moss_noise(q + Vector3(3.1f, 0, 0), salt + 1u) + 0.08f * (pr.randf() - 0.5f));
	}
	auto rim_at = [&](float p_a) {
		const float f = p_a / float(Math::TAU) * sectors;
		const int k0 = int(Math::floor(f));
		const float t = f - k0;
		return Math::lerp(rim[((k0 % sectors) + sectors) % sectors], rim[((k0 + 1) % sectors + sectors) % sectors], t);
	};
	// The underside: a shallow dome, thickest under the stem, lumpy.
	auto under = [&](float p_rho, const Vector3 &p_q) {
		// Clods torn out with the roots: lumps over a hand to a pace across.
		const float lump = moss_noise(p_q * 2.3f, salt + 2u) - 0.5f + 0.5f * (moss_noise(p_q * 6.0f, salt + 3u) - 0.5f);
		const float clod = moss_noise(p_q * 13.0f, salt + 10u) - 0.5f;
		return -T * (0.2f + 0.8f * Math::pow(MAX(1.0f - p_rho * p_rho, 0.0f), 0.6f)) * (1.0f - 0.25f * rotten) + T * 0.45f * lump + 0.035f * clod;
	};
	auto top = [&](const Vector3 &p_q) {
		// The old forest floor: hummocks of litter and moss, small lumps of humus.
		return 0.03f + 0.07f * (moss_noise(p_q * 3.0f, salt + 4u) - 0.5f) + 0.03f * (moss_noise(p_q * 9.0f, salt + 14u) - 0.5f);
	};
	const Color litter = Color(0.31f, 0.23f, 0.14f).srgb_to_linear();
	const Color moss_c = Color(0.25f, 0.3f, 0.12f).srgb_to_linear();
	const Color humus = Color(0.13f, 0.1f, 0.075f).srgb_to_linear();
	const Color mineral = Color(0.34f, 0.28f, 0.21f).srgb_to_linear();
	const Color stone_c = Color(0.46f, 0.45f, 0.42f).srgb_to_linear();
	auto soil_colour = [&](const Vector3 &p_q, float p_depth) {
		// Humus under the old surface, then the mineral soil, mottled; drying paler with age.
		const float m = moss_noise(p_q * 4.0f, salt + 5u);
		const float m2 = moss_noise(p_q * 11.0f, salt + 11u);
		Color c = humus.lerp(mineral, Math::smoothstep(0.06f, 0.22f, p_depth + 0.06f * (m - 0.5f)));
		// Mottled: grey gley and rusty patches in the mineral soil, the clods' tops dried paler.
		c = c.lerp(Color(0.36f, 0.34f, 0.3f).srgb_to_linear(), 0.5f * Math::smoothstep(0.62f, 0.8f, moss_noise(p_q * 2.7f, salt + 12u)));
		c = c.lerp(Color(0.45f, 0.3f, 0.18f).srgb_to_linear(), 0.3f * Math::smoothstep(0.65f, 0.85f, moss_noise(p_q * 3.3f, salt + 13u)));
		c = c * (0.7f + 0.35f * m + 0.3f * m2);
		c = c.lerp(c * 1.25f, 0.4f * rotten);
		c.a = 1.0f;
		return c;
	};
	LocalVector<Vector3> outward; // Per vertex: which way is out, for the winding.
	auto push = [&](const Vector3 &p_local, const Vector3 &p_out, const Color &p_c, const Vector2 &p_uv) {
		w.verts.push_back(plate.to_world(p_local));
		w.normals.push_back(Vector3());
		w.uvs.push_back(p_uv);
		w.colors.push_back(p_c);
		outward.push_back(plate.rot.xform(p_out));
		return w.verts.size() - 1;
	};
	auto tri = [&](int a, int b, int c) {
		// Counter-clockwise seen from outside, as the bark (flipped for Godot with it).
		const Vector3 n = (w.verts[b] - w.verts[a]).cross(w.verts[c] - w.verts[a]);
		if (n.dot(outward[a] + outward[b] + outward[c]) < 0.0f) {
			SWAP(b, c);
		}
		w.indices.push_back(a);
		w.indices.push_back(b);
		w.indices.push_back(c);
	};
	Vector4i part;
	w.begin(part);
	const int rings = 16;
	// Top (rings 0..rings), rim wall (3 rings), underside (rings..0): one grid of sectors.
	LocalVector<int> grid;
	int rows = 0;
	auto row = [&](auto p_point) {
		for (int k = 0; k < sectors; k++) {
			grid.push_back(p_point(k));
		}
		rows++;
	};
	for (int i = 0; i <= rings; i++) {
		const float rho = float(i) / rings;
		row([&](int k) {
			const float a = float(Math::TAU) * k / sectors;
			const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
			Vector3 q = d * (rim_at(a) * rho);
			q.y = top(q);
			const float mz = moss_noise(q * 1.6f, salt + 6u);
			Color c = litter.lerp(moss_c, Math::smoothstep(0.45f, 0.7f, mz) * (1.0f - 0.6f * rotten)) * (0.85f + 0.3f * moss_noise(q * 7.0f, salt + 7u));
			c.a = 1.0f;
			return push(q, Vector3(0, 1, 0), c, Vector2(q.x, q.z));
		});
	}
	for (int j = 1; j <= 3; j++) {
		const float f = float(j) / 4.0f;
		row([&](int k) {
			const float a = float(Math::TAU) * k / sectors;
			const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
			Vector3 q = d * (rim_at(a) * (1.0f + 0.05f * Math::sin(f * float(Math::PI)) + 0.04f * (moss_noise(d * 5.0f + Vector3(0, f * 3.0f, 0), salt + 8u) - 0.5f)));
			const float y0 = top(q), y1 = under(1.0f, q);
			q.y = Math::lerp(y0, y1, f);
			return push(q, d, soil_colour(q, y0 - q.y), Vector2(a * R, q.y));
		});
	}
	for (int i = rings; i >= 1; i--) {
		const float rho = float(i) / rings;
		row([&](int k) {
			const float a = float(Math::TAU) * k / sectors;
			const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
			Vector3 q = d * (rim_at(a) * rho);
			q.y = under(rho, q);
			return push(q, (Vector3(0, -1, 0) + d * rho).normalized(), soil_colour(q, top(q) - q.y), Vector2(q.x, q.z));
		});
	}
	for (int rr = 0; rr + 1 < rows; rr++) {
		for (int k = 0; k < sectors; k++) {
			const int k1 = (k + 1) % sectors;
			const int a0 = grid[rr * sectors + k], a1 = grid[rr * sectors + k1];
			const int b0 = grid[(rr + 1) * sectors + k], b1 = grid[(rr + 1) * sectors + k1];
			tri(a0, b0, a1);
			tri(a1, b0, b1);
		}
	}
	// Close the underside under the stem.
	{
		const Vector3 q(0, under(0.0f, Vector3()), 0);
		const int c = push(q, Vector3(0, -1, 0), soil_colour(q, top(q) - q.y), Vector2());
		for (int k = 0; k < sectors; k++) {
			tri(grid[(rows - 1) * sectors + k], c, grid[(rows - 1) * sectors + (k + 1) % sectors]);
		}
	}
	// Stones in the underside: lumpy, flattened octahedra pushed half out of the soil.
	const int stones = 6 + int(pr.rand() % 10);
	for (int st = 0; st < stones; st++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::sqrt(pr.randf()) * 0.92f;
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 c = d * (rim_at(a) * rho);
		c.y = under(rho, c);
		const float sz = Math::lerp(0.04f, 0.16f, pr.randf() * pr.randf()) * (0.6f + 0.4f * R / 2.0f);
		const Basis b = Basis(Vector3(pr.randf(), pr.randf(), pr.randf()).normalized(), pr.randf() * 3.0f);
		const Vector3 sc(sz * Math::lerp(0.8f, 1.3f, pr.randf()), sz * Math::lerp(0.5f, 0.8f, pr.randf()), sz * Math::lerp(0.8f, 1.2f, pr.randf()));
		const float tone = 0.8f + 0.35f * pr.randf();
		const Color col = Color(stone_c.r * tone, stone_c.g * tone, stone_c.b * tone);
		// A subdivided octahedron, its points jittered.
		const Vector3 oct[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
		const int faces[8][3] = { { 0, 2, 4 }, { 4, 2, 1 }, { 1, 2, 5 }, { 5, 2, 0 }, { 0, 4, 3 }, { 4, 1, 3 }, { 1, 5, 3 }, { 5, 0, 3 } };
		auto sp = [&](const Vector3 &p_u) {
			const Vector3 u = p_u.normalized();
			const float j = 0.85f + 0.3f * moss_noise(u * 2.5f + c * 3.0f, salt + 9u);
			return c + b.xform(u * sc * j);
		};
		for (const auto &f : faces) {
			const Vector3 A = oct[f[0]], B = oct[f[1]], C = oct[f[2]];
			const Vector3 AB = (A + B) * 0.5f, BC = (B + C) * 0.5f, CA = (C + A) * 0.5f;
			const Vector3 sub[4][3] = { { A, AB, CA }, { AB, B, BC }, { CA, BC, C }, { AB, BC, CA } };
			for (const auto &sf : sub) {
				int ids[3];
				for (int e = 0; e < 3; e++) {
					const Vector3 lp = sp(sf[e]);
					ids[e] = push(lp, b.xform(sf[e].normalized()), col, Vector2(lp.x, lp.z) * 4.0f);
				}
				tri(ids[0], ids[1], ids[2]);
			}
		}
	}
	// Normals from the faces (smooth over the soil; the stones' corners share none).
	{
		Vector3 *nw = w.normals.ptrw();
		const Vector3 *vw = w.verts.ptr();
		for (int64_t i = part.x; i + 2 < w.indices.size(); i += 3) {
			const int a = w.indices[i], b = w.indices[i + 1], c = w.indices[i + 2];
			const Vector3 fn = (vw[b] - vw[a]).cross(vw[c] - vw[a]);
			nw[a] += fn;
			nw[b] += fn;
			nw[c] += fn;
		}
		for (int v = part.z; v < w.verts.size(); v++) {
			nw[v] = nw[v].length_squared() > 1e-20f ? nw[v].normalized() : outward[v];
		}
	}
	w.end(part, R);
	// The roots: laterals from the stem's foot out through the plate, half out of its underside,
	// to torn ends past the rim; a few side roots off each; sinkers hanging under the stem.
	const Color root_c = Color(0.3f, 0.22f, 0.15f).srgb_to_linear().lerp(Color(0.4f, 0.37f, 0.33f).srgb_to_linear(), 0.5f * rotten);
	LocalVector<Vector3> pts;
	LocalVector<float> rad;
	// p_free: from which point on the root is out of the soil and hangs under its own weight
	// (world down), p_droop metres at its end.
	auto root = [&](const LocalVector<Vector3> &p_local, const LocalVector<float> &p_r, bool p_torn, int p_free = 1 << 20, float p_droop = 0.0f) {
		pts.clear();
		rad.clear();
		const int n = int(p_local.size());
		for (int i = 0; i < n; i++) {
			const float f = i > p_free ? float(i - p_free) / MAX(n - 1 - p_free, 1) : 0.0f;
			pts.push_back(plate.to_world(p_local[i]) - Vector3(0, p_droop * f * f, 0));
			rad.push_back(p_r[i]);
		}
		const int sides = CLAMP(int(p_r[0] * 160.0f), 5, 12);
		Vector4i rp;
		r_bark.bone = 0;
		r_bark.begin(rp);
		_tube(r_bark, pts, rad, sides, 0.0f, root_c, nullptr, 0.0f, -1.0f, false);
		r_bark.end(rp, p_r[0]);
		if (p_torn && pts.size() >= 2 && p_r[p_r.size() - 1] >= 0.012f) {
			wood_b.bone = 0;
			_fracture(pts[pts.size() - 1], (pts[pts.size() - 1] - pts[pts.size() - 2]).normalized(), rad[rad.size() - 1], 0.2f + 0.6f * rotten, pr.rand());
		}
	};
	LocalVector<Vector3> lp;
	LocalVector<float> lr;
	const int laterals = 8 + int(pr.rand() % 6);
	for (int l = 0; l < laterals; l++) {
		const float a0 = float(Math::TAU) * (l + 0.3f * (pr.randf() - 0.5f)) / laterals;
		const float r0 = tr * Math::lerp(0.3f, 0.48f, pr.randf());
		const float reach = Math::lerp(1.0f, 1.25f, pr.randf());
		lp.clear();
		lr.clear();
		const int steps = 12;
		float a = a0;
		for (int i = 0; i <= steps; i++) {
			const float f = float(i) / steps;
			a += 0.12f * (pr.randf() - 0.5f);
			const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
			const float rho = Math::lerp(0.6f * tr / MAX(rim_at(a), 0.1f), reach, f);
			Vector3 q = d * (rim_at(a) * rho);
			// Thick at the root crown under the stem, tapering fast (Coutts 1983, the zone of rapid
			// taper), then slowly to the rim.
			const float rr = r0 * (0.3f + 0.7f * Math::pow(1.0f - f, 2.0f)) + tr * 0.25f * MAX(0.0f, 1.0f - f * 4.0f);
			// Inside the plate near the stem, then along the underside mostly out of the soil
			// (washed and shaken bare), and past the rim hanging under their own weight.
			const float floor_y = under(MIN(rho, 1.0f), q) + rr * 0.15f;
			q.y = rho <= 1.0f ? MIN(floor_y, Math::lerp(-0.12f, floor_y, Math::smoothstep(0.0f, 0.35f, rho))) : under(1.0f, q) + rr * 0.15f;
			lp.push_back(q);
			lr.push_back(rr);
		}
		int free_at = steps;
		for (int i = 0; i <= steps; i++) {
			if (Vector2(lp[i].x, lp[i].z).length() > rim_at(Math::atan2(lp[i].z, lp[i].x))) {
				free_at = i;
				break;
			}
		}
		root(lp, lr, true, free_at, 0.25f * R * (reach - 1.0f) + 0.1f);
		// Side roots.
		const int sides_n = 1 + int(pr.rand() % 3);
		for (int sr = 0; sr < sides_n; sr++) {
			const int at = 4 + int(pr.rand() % 6);
			const Vector3 o = lp[at];
			const float sa = Math::atan2(o.z, o.x) + (pr.randf() < 0.5f ? -1.0f : 1.0f) * Math::lerp(0.35f, 0.7f, pr.randf());
			const float s0 = lr[at] * 0.55f;
			LocalVector<Vector3> sp2;
			LocalVector<float> sr2;
			const float rho0 = Vector2(o.x, o.z).length() / MAX(rim_at(sa), 0.1f);
			for (int i = 0; i <= 6; i++) {
				const float f = float(i) / 6.0f;
				const Vector3 d(Math::cos(sa), 0.0f, Math::sin(sa));
				const float rho = Math::lerp(rho0, Math::lerp(1.0f, 1.15f, pr.randf()), f);
				Vector3 q = i == 0 ? o : d * (rim_at(sa) * rho);
				const float rr = s0 * Math::lerp(1.0f, 0.35f, f);
				if (i > 0) {
					q.y = under(MIN(rho, 1.0f), q) + rr * 0.2f;
				}
				sp2.push_back(q);
				sr2.push_back(rr);
			}
			root(sp2, sr2, true, 4, 0.15f);
		}
	}
	// Fine roots torn out of the underside and the rim: thin, many, hanging down.
	const int fines = int(30.0f * R);
	for (int k = 0; k < fines; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::lerp(0.25f, 1.02f, Math::sqrt(pr.randf()));
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 o = d * (rim_at(a) * rho);
		o.y = under(MIN(rho, 1.0f), o) + 0.01f;
		const float len = Math::lerp(0.12f, 0.45f, pr.randf());
		const float r0 = Math::lerp(0.004f, 0.012f, pr.randf());
		lp.clear();
		lr.clear();
		const Vector3 go = (d * Math::lerp(0.3f, 1.0f, pr.randf()) + Vector3(0, -1, 0)).normalized();
		for (int i = 0; i <= 4; i++) {
			const float f = float(i) / 4.0f;
			lp.push_back(o + go * (len * f) + Vector3(pr.randf() - 0.5f, pr.randf() - 0.5f, pr.randf() - 0.5f) * (0.04f * f));
			lr.push_back(r0 * Math::lerp(1.0f, 0.4f, f));
		}
		root(lp, lr, false, 0, len * 0.5f);
	}
	// Sinkers: straight down from under the stem, snapped where the soil held them.
	const int sinkers = 3 + int(pr.rand() % 3);
	for (int k = 0; k < sinkers; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		const Vector3 o = d * (tr * Math::lerp(0.3f, 1.2f, pr.randf()));
		const float len = Math::lerp(0.25f, 0.7f, pr.randf()) * (1.0f - 0.4f * rotten);
		const float r0 = tr * Math::lerp(0.12f, 0.22f, pr.randf());
		lp.clear();
		lr.clear();
		const float y0 = under(0.0f, o) + 0.1f;
		for (int i = 0; i <= 5; i++) {
			const float f = float(i) / 5.0f;
			lp.push_back(o + d * (0.12f * len * f) + Vector3(0, y0 - len * f, 0) + Vector3(pr.randf() - 0.5f, 0, pr.randf() - 0.5f) * (0.03f * f));
			lr.push_back(r0 * Math::lerp(1.0f, 0.45f, f));
		}
		root(lp, lr, true);
	}
}

// A branch or stem snapped by wind, snow or its own rot (trees.md 5j). Wood fails in tension on
// the side the load pulls and buckles on the side it pushes: the tension side tears out long
// splinters of the outer wood, the compression side breaks short and crumpled, and the bark tears
// back on the tension side (van Casteren et al. 2012, Why don't branches snap?; Li et al.,
// Stressful Tree Modeling: Breaking Branches with Strands, SIGGRAPH 2025). Built from p_end (the
// stem's end centre, its radius p_r, axis p_dir): a jagged face over the end, splinters as
// tapering three-sided slivers, a torn strip of bark. p_old 0 fresh (pale), 1 weathered (grey).
void TreeGenerator::_fracture(const Vector3 &p_end, const Vector3 &p_dir, float p_r, float p_old, uint32_t p_seed) {
	Builder &w = wood_b;
	RandomPCG sr;
	sr.seed(uint64_t(p_seed) * 6364136223846793005ull + 0x66726163u);
	const Vector3 g = p_dir.normalized();
	const Vector3 world_up(0, 1, 0);
	// The tension side: up for a limb that bent down under its weight; for a near-vertical stem,
	// downwind (here a random bearing).
	Vector3 td = world_up - g * world_up.dot(g);
	if (td.length_squared() < 0.04f) {
		const float a = sr.randf() * float(Math::TAU);
		td = Vector3(Math::cos(a), 0.0f, Math::sin(a));
		td = td - g * td.dot(g);
	}
	td.normalize();
	const Vector3 sd = g.cross(td);
	const Color fresh = Color(0.74f, 0.66f, 0.52f).srgb_to_linear();
	const Color grey = Color(0.52f, 0.5f, 0.46f).srgb_to_linear();
	const Color wood = fresh.lerp(grey, p_old);
	const Color heart = wood.lerp(Color(0.42f, 0.33f, 0.24f).srgb_to_linear(), 0.35f);
	// Green wood is tough and breaks fibrous: the tension side tears out far and in many long
	// splinters, a greenstick break (van Casteren et al. 2012). Dry dead wood snaps short and flat.
	const float green = CLAMP(1.0f - 2.0f * p_old, 0.0f, 1.0f);
	auto push = [&](const Vector3 &p_p, const Vector2 &p_uv, const Color &p_c) {
		w.verts.push_back(p_p);
		w.normals.push_back(Vector3());
		w.uvs.push_back(p_uv);
		w.colors.push_back(p_c);
		return w.verts.size() - 1;
	};
	auto tri = [&](int a, int b, int c) {
		w.indices.push_back(a);
		w.indices.push_back(b);
		w.indices.push_back(c);
	};
	Vector4i part;
	w.begin(part);
	// The face: rings from the rim inwards, heights along the axis. The rim ring sits on the stem's
	// end (height 0) so the face closes it; the face rises steeply inside it and is higher and
	// more ragged on the tension side.
	const int sectors = 20;
	const float ring_rho[5] = { 1.0f, 0.95f, 0.7f, 0.4f, 0.12f };
	const float phase = sr.randf() * 10.0f;
	auto face_h = [&](float p_rho, float p_a) {
		const float c = Math::cos(p_a); // 1 on the tension side.
		const float jag = 0.5f + 0.5f * Math::sin(p_a * 5.0f + phase + p_rho * 7.0f) * Math::sin(p_a * 3.0f - phase * 1.7f);
		return p_r * (0.04f + (0.6f + 0.7f * green) * MAX(c, 0.0f) * MAX(c, 0.0f) * (0.3f + 0.7f * p_rho) + 0.3f * jag * p_rho);
	};
	const int f0 = w.verts.size();
	for (int i = 0; i < 5; i++) {
		for (int k = 0; k < sectors; k++) {
			const float a = float(Math::TAU) * k / sectors;
			const float rho = ring_rho[i];
			// The outer ring a little lower than the face inside it: the bark and the outer wood
			// break first, so the rim is a ragged step, not a sleeve.
			const float hh = i == 0 ? 0.0f : face_h(rho, a) * (i == 1 ? 0.55f : 1.0f) * (1.0f + 0.3f * (sr.randf() - 0.5f));
			const Vector3 p = p_end + (td * Math::cos(a) + sd * Math::sin(a)) * (p_r * rho) + g * hh;
			// Fibres run along the axis: v up the face's relief, u around.
			push(p, Vector2(float(k) / sectors * 2.0f, hh / MAX(p_r, 0.01f) * 0.5f + rho * 0.3f), i == 0 ? wood : wood.lerp(heart, 1.0f - rho));
		}
	}
	const int fc = push(p_end + g * (face_h(0.0f, 0.0f) * 0.8f), Vector2(0.5f, 0.1f), heart);
	for (int i = 0; i < 4; i++) {
		for (int k = 0; k < sectors; k++) {
			const int k1 = (k + 1) % sectors;
			const int a0 = f0 + i * sectors + k, a1 = f0 + i * sectors + k1;
			const int b0 = f0 + (i + 1) * sectors + k, b1 = f0 + (i + 1) * sectors + k1;
			tri(a0, b0, a1);
			tri(a1, b0, b1);
		}
	}
	for (int k = 0; k < sectors; k++) {
		tri(f0 + 4 * sectors + k, fc, f0 + 4 * sectors + (k + 1) % sectors);
	}
	// Splinters: slivers of the outer wood pulled out on the tension side, longest there, a few
	// short ones elsewhere; each a three-sided sliver tapering to a point, bent a little outwards.
	const int count = 12 + int(sr.rand() % 9) + int(12.0f * green);
	for (int q = 0; q < count; q++) {
		const float a = (sr.randf() < 0.75f ? 0.0f : float(Math::PI)) + (sr.randf() - 0.5f) * 2.2f;
		const float pull = MAX(Math::cos(a), 0.0f);
		const float rho = Math::lerp(0.5f, 0.92f, sr.randf());
		const float len = p_r * Math::lerp(0.25f, 0.6f, sr.randf()) * (0.5f + (1.8f + 2.2f * green) * pull * pull);
		const float wid = p_r * Math::lerp(0.1f, 0.24f, sr.randf());
		const Vector3 radial = td * Math::cos(a) + sd * Math::sin(a);
		const Vector3 tang = g.cross(radial);
		const Vector3 base = p_end + radial * (p_r * rho) + g * (face_h(rho, a) * 0.6f);
		const Vector3 dirs = (g + radial * Math::lerp(0.05f, 0.3f, sr.randf()) + tang * (sr.randf() - 0.5f) * 0.25f).normalized();
		const int segs = 3;
		const int s0 = w.verts.size();
		for (int e = 0; e <= segs; e++) {
			const float f = float(e) / segs;
			const Vector3 c = base + dirs * (len * f) + radial * (len * 0.15f * f * f);
			// Splinters end ragged and blunt-ish, not in needles.
			const float ww = wid * (1.0f - 0.7f * f);
			// A sliver's section: wide across the grain's tangent, thin radially.
			const Vector3 c0 = c - tang * ww;
			const Vector3 c1 = c + tang * ww;
			const Vector3 c2 = c + radial * (ww * 0.45f);
			const float vv = f * len / MAX(p_r, 0.01f);
			push(c0, Vector2(0.0f, vv), wood);
			push(c1, Vector2(0.15f, vv), wood);
			push(c2, Vector2(0.08f, vv), wood.lerp(heart, 0.3f));
		}
		for (int e = 0; e < segs; e++) {
			for (int side = 0; side < 3; side++) {
				const int a0 = s0 + e * 3 + side, a1 = s0 + e * 3 + (side + 1) % 3;
				const int b0 = a0 + 3, b1 = a1 + 3;
				tri(a0, b0, a1);
				tri(a1, b0, b1);
			}
		}
	}
	// Bark torn back on the tension side: a curved strip lifted off the stem from the rim.
	{
		const Color bark_c = bark_color.srgb_to_linear();
		const float a_mid = (sr.randf() - 0.5f) * 0.8f;
		const float half = Math::lerp(0.18f, 0.35f, sr.randf());
		const float len = p_r * Math::lerp(0.3f, 0.8f, sr.randf());
		const int cols = 4, rows = 3;
		const int s0 = w.verts.size();
		for (int j = 0; j <= rows; j++) {
			const float f = float(j) / rows;
			for (int c = 0; c <= cols; c++) {
				const float a = a_mid + (float(c) / cols - 0.5f) * 2.0f * half;
				const Vector3 radial = td * Math::cos(a) + sd * Math::sin(a);
				// Peeling: lifts off the stem as it goes out, and its free end is ragged.
				const float lift = p_r * (0.015f + 0.12f * f * f);
				const float out = len * f * (1.0f - 0.25f * Math::abs(float(c) / cols - 0.5f) * (j == rows ? 1.0f + sr.randf() : 0.0f));
				push(p_end + radial * (p_r * 1.01f + lift) + g * out, Vector2(float(c) / cols * 0.3f, f), bark_c);
			}
		}
		for (int j = 0; j < rows; j++) {
			for (int c = 0; c < cols; c++) {
				const int a0 = s0 + j * (cols + 1) + c;
				tri(a0, a0 + cols + 1, a0 + 1);
				tri(a0 + 1, a0 + cols + 1, a0 + cols + 2);
			}
		}
	}
	// Normals from the faces (two-sided material; slivers and strip are thin).
	Vector3 *nw = w.normals.ptrw();
	const Vector3 *vw = w.verts.ptr();
	for (int64_t i = part.x; i + 2 < w.indices.size(); i += 3) {
		const int a = w.indices[i], b = w.indices[i + 1], c = w.indices[i + 2];
		const Vector3 fn = (vw[c] - vw[a]).cross(vw[b] - vw[a]);
		nw[a] += fn;
		nw[b] += fn;
		nw[c] += fn;
	}
	for (int v = part.z; v < w.verts.size(); v++) {
		nw[v] = nw[v].length_squared() > 1e-20f ? nw[v].normalized() : g;
	}
	w.end(part, p_r);
}

// A sawn pole stub on a pollard head or coppice stool (trees.md 5l): a flat face, a little domed
// where the saw tore the last fibres, with the annual rings of the pole showing, pale fresh wood
// greying and darkening from the heart as it weathers (p_age 0 fresh to 1 old), and the bark's
// thin dark edge at the rim.
void TreeGenerator::_saw_face(const Vector3 &p_end, const Vector3 &p_dir, float p_r, float p_age, uint32_t p_seed) {
	Builder &w = wood_b;
	RandomPCG sr;
	sr.seed(uint64_t(p_seed) * 6364136223846793005ull + 0x73617775u);
	const Vector3 g = p_dir.normalized();
	Vector3 td = Vector3(0, 1, 0) - g * g.y;
	if (td.length_squared() < 0.04f) {
		td = Vector3(1, 0, 0) - g * g.x;
	}
	td.normalize();
	const Vector3 sd = g.cross(td);
	const float age = CLAMP(p_age, 0.0f, 1.0f);
	const Color fresh = Color(0.8f, 0.7f, 0.52f).srgb_to_linear();
	const Color grey = Color(0.5f, 0.48f, 0.43f).srgb_to_linear();
	const Color wood = fresh.lerp(grey, age);
	// Past 1, the face is rotting: dark brown-black, crumbled, with moss and algae in its hollows.
	const float rot = CLAMP(p_age - 1.0f, 0.0f, 1.0f);
	const Color heart = wood.lerp(Color(0.3f, 0.24f, 0.17f).srgb_to_linear(), 0.25f + 0.45f * age);
	const Color rotten = Color(0.17f, 0.13f, 0.1f).srgb_to_linear();
	const Color green = Color(0.2f, 0.25f, 0.12f).srgb_to_linear();
	const Color rim = bark_color.srgb_to_linear().lerp(Color(0.1f, 0.08f, 0.06f), 0.4f);
	// Annual rings: about one per 3 mm of radius for a fast coppice pole, offset by the seed.
	const float rings = MAX(3.0f, p_r / 0.003f);
	const float phase = sr.randf();
	Vector4i part;
	w.begin(part);
	const int sectors = 18;
	const int nr = 9;
	const int f0 = w.verts.size();
	for (int i = 0; i < nr; i++) {
		// From the rim (rho 1) inwards; the outermost ring is the bark's edge.
		const float rho = i == 0 ? 1.0f : 0.96f * (1.0f - float(i - 1) / (nr - 1));
		const float band = 0.5f + 0.5f * Math::cos(float(Math::TAU) * (rho * rings + phase));
		Color c = i == 0 ? rim : wood.lerp(heart, (1.0f - rho) * (1.0f - rho)) * (0.86f + 0.14f * band);
		for (int k = 0; k < sectors; k++) {
			const float a = float(Math::TAU) * k / sectors;
			float hh = i == 0 ? 0.0f : p_r * (0.05f + 0.03f * (1.0f - rho) + 0.01f * (sr.randf() - 0.5f));
			Color ck = c;
			if (rot > 0.0f && i > 0) {
				// Rot hollows the heart first and leaves the outer rings standing.
				const float n = sr.randf();
				hh -= rot * p_r * (0.18f * (1.0f - rho * rho) + 0.08f * n);
				ck = c.lerp(rotten, rot * (0.55f + 0.35f * n)).lerp(green, rot * 0.5f * MAX(0.0f, sr.randf() - 0.4f));
			}
			w.verts.push_back(p_end + (td * Math::cos(a) + sd * Math::sin(a)) * (p_r * rho) + g * hh);
			w.normals.push_back(g);
			// Planar across the face: the wood texture's fibres lie as parallel saw marks. Polar,
			// they fanned out from the pith as a pale pinwheel.
			w.uvs.push_back(Vector2(Math::cos(a), Math::sin(a)) * (p_r * rho / 0.15f) + Vector2(phase, 0.0f));
			w.colors.push_back(ck);
		}
	}
	const int fc = w.verts.size();
	w.verts.push_back(p_end + g * (p_r * (0.09f - 0.22f * rot)));
	w.normals.push_back(g);
	w.uvs.push_back(Vector2(phase, 0.0f));
	w.colors.push_back(heart.lerp(rotten, rot));
	for (int i = 0; i + 1 < nr; i++) {
		for (int k = 0; k < sectors; k++) {
			const int k1 = (k + 1) % sectors;
			const int a0 = f0 + i * sectors + k, a1 = f0 + i * sectors + k1;
			const int b0 = f0 + (i + 1) * sectors + k, b1 = f0 + (i + 1) * sectors + k1;
			w.indices.push_back(a0);
			w.indices.push_back(b0);
			w.indices.push_back(a1);
			w.indices.push_back(a1);
			w.indices.push_back(b0);
			w.indices.push_back(b1);
		}
	}
	for (int k = 0; k < sectors; k++) {
		w.indices.push_back(f0 + (nr - 1) * sectors + k);
		w.indices.push_back(fc);
		w.indices.push_back(f0 + (nr - 1) * sectors + (k + 1) % sectors);
	}
	if (rot > 0.0f) {
		// The rotted hollow's own shading: normals from its faces, kept facing out of the face.
		Vector3 *nw = w.normals.ptrw();
		const Vector3 *vw = w.verts.ptr();
		for (int v = f0; v < w.verts.size(); v++) {
			nw[v] = Vector3();
		}
		for (int64_t i = part.x; i + 2 < w.indices.size(); i += 3) {
			const int ia = w.indices[i], ib = w.indices[i + 1], ic = w.indices[i + 2];
			Vector3 fn = (vw[ib] - vw[ia]).cross(vw[ic] - vw[ia]);
			if (fn.dot(g) < 0.0f) {
				fn = -fn;
			}
			nw[ia] += fn;
			nw[ib] += fn;
			nw[ic] += fn;
		}
		for (int v = f0; v < w.verts.size(); v++) {
			nw[v] = nw[v].length_squared() > 1e-20f ? nw[v].normalized() : g;
		}
	}
	w.end(part, p_r);
}

// Crown parts (trees.md 5n): what a crown carries besides its leaves, as geometry. Cones (pine,
// spruce, larch, hemlock, alder), catkins (hazel, birch, alder), seed keys (ash, maples), berries
// (holly, hawthorn, rowan, yew, elder), mistletoe balls on its hosts (Viscum album in Europe,
// Phoradendron in eastern North America) and witches' brooms on birches (Taphrina betulina).
// Seasons from the shared season state's day of year: catkins form in late summer, hang closed
// through winter and open before the leaves; keys and haws hang on into winter; cones persist.
// Built like Unreal 5.7's procedural vegetation assemblies, in that each part is a small fixed
// shape placed many times, but merged into one surface here: LOD 0 carries all of it, the coarser
// levels only the mistletoe and brooms, which read from far.
int TreeGenerator::_grow_crown_parts(Builder &r_b) {
	r_b = Builder();
	if (nodes.size() < 2 || fallen || fallen_branch || decay_class >= 2 || (fruit_cover <= 0.0f && mistletoe <= 0.0f && witches_brooms <= 0.0f)) {
		return 0;
	}
	RandomPCG cr;
	cr.seed(uint64_t(seed) * 0x2545F4914F6CDD1Dull + uint64_t(preset) * 977u + 0x63726f77u);
	const float base_y = nodes[0].start.y;
	const float tall = MAX(height, 0.5f);
	const float day = season.get("day_of_year", 182.0f);
	// Inside [p_from, p_to] on the year's wheel (the span may run over New Year).
	auto in_days = [&](float p_from, float p_to) {
		return p_from <= p_to ? (day >= p_from && day <= p_to) : (day >= p_from || day <= p_to);
	};
	auto lin = [](float r, float g, float b) { return Color(r, g, b).srgb_to_linear(); };
	auto bone_of = [&](int p_node) { return p_node >= 0 && p_node < int(node_bone.size()) ? node_bone[p_node] : 0; };
	auto perp = [](const Vector3 &p_d) {
		const Vector3 ref = Math::abs(p_d.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
		return p_d.cross(ref).normalized();
	};
	auto vert = [&](const Vector3 &p_p, const Vector2 &p_uv, const Color &p_c) {
		r_b.verts.push_back(p_p);
		r_b.normals.push_back(Vector3());
		r_b.uvs.push_back(p_uv);
		r_b.colors.push_back(p_c);
		return int(r_b.verts.size()) - 1;
	};
	auto tri = [&](int a, int b, int c) {
		r_b.indices.push_back(a);
		r_b.indices.push_back(b);
		r_b.indices.push_back(c);
	};
	// Face normals, averaged, for a finished part (all parts are seen from both sides).
	auto close_part = [&](Vector4i &p_part, float p_size) {
		Vector3 *nw = r_b.normals.ptrw();
		const Vector3 *vw = r_b.verts.ptr();
		for (int64_t i = p_part.x; i + 2 < r_b.indices.size(); i += 3) {
			const int a = r_b.indices[i], b = r_b.indices[i + 1], c = r_b.indices[i + 2];
			const Vector3 fn = (vw[b] - vw[a]).cross(vw[c] - vw[a]);
			nw[a] += fn;
			nw[b] += fn;
			nw[c] += fn;
		}
		for (int v = p_part.z; v < r_b.verts.size(); v++) {
			nw[v] = nw[v].length_squared() > 1e-24f ? nw[v].normalized() : Vector3(0, 1, 0);
		}
		r_b.end(p_part, p_size);
	};
	// A tube along a polyline, p_sides round, closed at its end.
	auto tube = [&](const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_r, int p_sides, const LocalVector<Color> &p_c) {
		const int n = int(p_pts.size());
		Vector3 side = perp((p_pts[1] - p_pts[0]).normalized());
		const int v0 = r_b.verts.size();
		for (int i = 0; i < n; i++) {
			const Vector3 d = (p_pts[MIN(i + 1, n - 1)] - p_pts[MAX(i - 1, 0)]).normalized();
			side = (side - d * side.dot(d)).normalized();
			const Vector3 up = d.cross(side);
			for (int k = 0; k < p_sides; k++) {
				const float a = float(Math::TAU) * k / p_sides;
				vert(p_pts[i] + (side * Math::cos(a) + up * Math::sin(a)) * p_r[i], Vector2(float(k) / p_sides, float(i) / n), p_c[i]);
			}
		}
		for (int i = 0; i + 1 < n; i++) {
			for (int k = 0; k < p_sides; k++) {
				const int k1 = (k + 1) % p_sides;
				// Wound so the face normals (close_part) point out of the tube.
				tri(v0 + i * p_sides + k, v0 + i * p_sides + k1, v0 + (i + 1) * p_sides + k);
				tri(v0 + i * p_sides + k1, v0 + (i + 1) * p_sides + k1, v0 + (i + 1) * p_sides + k);
			}
		}
		const int tip = vert(p_pts[n - 1] + (p_pts[n - 1] - p_pts[n - 2]).normalized() * p_r[n - 1] * 0.5f, Vector2(0.5f, 1.0f), p_c[n - 1]);
		for (int k = 0; k < p_sides; k++) {
			tri(v0 + (n - 1) * p_sides + k, v0 + (n - 1) * p_sides + (k + 1) % p_sides, tip);
		}
	};
	// A berry or seed: a small ball (an octahedron split once).
	auto ball = [&](const Vector3 &p_c, float p_r, const Color &p_col) {
		const Vector3 ax[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
		const int faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
		for (int f = 0; f < 8; f++) {
			const Vector3 a = ax[faces[f][0]], b = ax[faces[f][1]], c = ax[faces[f][2]];
			const Vector3 ab = (a + b).normalized(), bc = (b + c).normalized(), ca = (c + a).normalized();
			const int ia = vert(p_c + a * p_r, Vector2(), p_col), ib = vert(p_c + b * p_r, Vector2(), p_col), ic = vert(p_c + c * p_r, Vector2(), p_col);
			const int iab = vert(p_c + ab * p_r, Vector2(), p_col), ibc = vert(p_c + bc * p_r, Vector2(), p_col), ica = vert(p_c + ca * p_r, Vector2(), p_col);
			tri(ia, iab, ica);
			tri(iab, ib, ibc);
			tri(ica, ibc, ic);
			tri(iab, ibc, ica);
		}
	};
	// A thin stalk, two points, three sides: what fruit and keys hang from.
	auto stalk = [&](const Vector3 &p_a, const Vector3 &p_b, float p_r, const Color &p_c) {
		if (p_a.distance_squared_to(p_b) < 1e-8f) {
			return;
		}
		LocalVector<Vector3> sp;
		LocalVector<float> sr;
		LocalVector<Color> sc;
		sp.push_back(p_a);
		sp.push_back(p_b);
		sr.push_back(p_r);
		sr.push_back(p_r * 0.8f);
		sc.push_back(p_c);
		sc.push_back(p_c);
		tube(sp, sr, 3, sc);
	};
	// A flat blade (key wing, mistletoe leaf): p_len along p_dir, its width profile over five
	// stations, turned by p_twist (radians) from base to tip, colour from base to tip.
	auto blade = [&](const Vector3 &p_at, const Vector3 &p_dir, const Vector3 &p_width, float p_len, float p_w, const float *p_prof, float p_twist, float p_fold, const Color &p_c0, const Color &p_c1) {
		const int v0 = r_b.verts.size();
		const Vector3 nrm = p_dir.cross(p_width).normalized();
		for (int i = 0; i < 5; i++) {
			const float t = i / 4.0f;
			const Vector3 wd = p_width.rotated(p_dir, p_twist * t);
			const Vector3 c = p_at + p_dir * (p_len * t);
			const float w = p_w * p_prof[i] * 0.5f;
			const Color col = p_c0.lerp(p_c1, t);
			vert(c - wd * w + nrm * (p_fold * w), Vector2(0.0f, t), col);
			vert(c + wd * w + nrm * (p_fold * w), Vector2(1.0f, t), col);
		}
		for (int i = 0; i < 4; i++) {
			const int a = v0 + i * 2;
			tri(a, a + 2, a + 1);
			tri(a + 1, a + 2, a + 3);
		}
	};
	// A cone of spiral scales (phyllotaxis 137.5 degrees): p_axis from the stalk to the tip, a
	// profile fat at the base (ovoid, pines) or even (cylindrical, spruce); open cones' scales
	// stand out from the axis, closed ones lie along it.
	auto cone = [&](const Vector3 &p_base, const Vector3 &p_axis, float p_len, float p_r, int p_scales, bool p_cyl, float p_open, const Color &p_in, const Color &p_out) {
		const Vector3 s0 = perp(p_axis);
		const Vector3 s1 = p_axis.cross(s0);
		const float tilt = Math::deg_to_rad(Math::lerp(25.0f, 70.0f, p_open));
		for (int i = 0; i < p_scales; i++) {
			const float t = (i + 0.5f) / p_scales;
			const float prof = p_cyl ? Math::smoothstep(0.0f, 0.12f, t) * (1.0f - 0.7f * Math::smoothstep(0.75f, 1.0f, t)) : Math::pow(MAX(Math::sin(float(Math::PI) * CLAMP(t * 0.85f + 0.1f, 0.0f, 1.0f)), 0.0f), 0.7f);
			const float a = i * 2.39996f;
			const Vector3 rad = s0 * Math::cos(a) + s1 * Math::sin(a);
			const Vector3 root = p_base + p_axis * (p_len * t * 0.92f) + rad * (p_r * 0.25f * prof);
			const Vector3 d = (rad * Math::sin(tilt) + p_axis * Math::cos(tilt)).normalized();
			const float sl = p_r * (0.35f + 0.9f * prof);
			const float sw = p_r * (0.45f + 0.5f * prof);
			const Vector3 w = d.cross(rad).normalized();
			const int ia = vert(root, Vector2(0.5f, 0.0f), p_in);
			const int il = vert(root + d * (sl * 0.55f) - w * (sw * 0.5f) + rad * (sl * 0.08f), Vector2(0.0f, 0.55f), p_in.lerp(p_out, 0.5f));
			const int ir = vert(root + d * (sl * 0.55f) + w * (sw * 0.5f) + rad * (sl * 0.08f), Vector2(1.0f, 0.55f), p_in.lerp(p_out, 0.5f));
			const int it = vert(root + d * sl + rad * (sl * 0.12f), Vector2(0.5f, 1.0f), p_out);
			tri(ia, il, ir);
			tri(il, it, ir);
		}
		// The core under the scales and the stalk.
		LocalVector<Vector3> cp;
		LocalVector<float> cr2;
		LocalVector<Color> cc;
		for (int k = 0; k < 4; k++) {
			cp.push_back(p_base + p_axis * (p_len * (k / 3.0f) * 0.95f - (k == 0 ? p_r * 0.3f : 0.0f)));
			cr2.push_back(p_r * (k == 0 ? 0.12f : 0.3f * (1.0f - 0.5f * k / 3.0f)));
			cc.push_back(p_in);
		}
		tube(cp, cr2, 4, cc);
	};
	// A catkin, modelled: a thin axis (rachis) curving under its weight, clothed in spiral bract
	// scales, each a small peaked shield; closed, they lie flat and overlap like a tiny cone;
	// open, the axis lengthens, the bracts stand off it and each shows its stamens, a tuft of
	// pollen-yellow anthers under the bract (hazel, birch and alder male catkins; Proctor and Yeo,
	// The Pollination of Flowers, 1973). As a plain tube it read as a sausage.
	auto catkin = [&](const Vector3 &p_at, const Vector3 &p_dir, float p_len, float p_r, const Color &p_a, const Color &p_b, float p_hang, bool p_open, const Color &p_pollen) {
		// The axis.
		const int n = 10;
		LocalVector<Vector3> cp;
		LocalVector<Vector3> cd;
		Vector3 pos = p_at;
		Vector3 d = p_dir;
		for (int k = 0; k <= n; k++) {
			cp.push_back(pos);
			cd.push_back(d);
			d = d.lerp(Vector3(0, -1, 0), p_hang).normalized();
			pos += d * (p_len / n);
		}
		{
			LocalVector<float> rr;
			LocalVector<Color> cc;
			for (int k = 0; k <= n; k++) {
				rr.push_back(p_r * (k == 0 ? 0.25f : 0.38f));
				cc.push_back(p_b * 0.7f);
			}
			tube(cp, rr, 3, cc);
		}
		// Bract scales on a 137.5 degree spiral, one per fifth of the radius along the axis.
		const int scales = CLAMP(int(p_len / (p_r * 0.2f)), 16, 90);
		Vector3 side = perp(cd[0]);
		for (int i = 0; i < scales; i++) {
			const float t = (i + 0.5f) / scales;
			const float ft = t * n;
			const int k = MIN(int(ft), n - 1);
			const float f = ft - k;
			const Vector3 ax = cd[k].lerp(cd[k + 1], f).normalized();
			const Vector3 c = cp[k].lerp(cp[k + 1], f);
			side = (side - ax * side.dot(ax)).normalized();
			const float a = i * 2.39996f;
			const Vector3 rad = (side * Math::cos(a) + ax.cross(side) * Math::sin(a)).normalized();
			// Tapering at both ends; open bracts stand out at 45-60 degrees.
			const float prof = Math::smoothstep(0.0f, 0.12f, t) * (1.0f - 0.6f * Math::smoothstep(0.75f, 1.0f, t));
			const float tilt = Math::deg_to_rad(p_open ? 34.0f : 14.0f);
			const Vector3 sd = (rad * Math::sin(tilt) + ax * Math::cos(tilt)).normalized();
			const Vector3 w = sd.cross(rad).normalized();
			const float sl = p_r * (0.55f + 0.35f * prof) * (p_open ? 0.85f : 1.0f);
			const float sw = p_r * (0.8f + 0.35f * prof);
			const Vector3 root = c + rad * (p_r * (p_open ? 0.35f : 0.5f) * (0.4f + 0.6f * prof));
			const Color cin = p_b, cout = p_a.lerp(p_b, 0.3f * (i & 1));
			const int ia = vert(root, Vector2(0.5f, 0.0f), cin);
			const int il = vert(root + sd * (sl * 0.45f) - w * (sw * 0.5f) + rad * (sl * 0.1f), Vector2(0.0f, 0.5f), cin.lerp(cout, 0.6f));
			const int ir = vert(root + sd * (sl * 0.45f) + w * (sw * 0.5f) + rad * (sl * 0.1f), Vector2(1.0f, 0.5f), cin.lerp(cout, 0.6f));
			// The peak bends back out a little: the bract's tip.
			const int it = vert(root + sd * sl + rad * (sl * (p_open ? 0.3f : 0.18f)), Vector2(0.5f, 1.0f), cout);
			tri(ia, il, ir);
			tri(il, it, ir);
			if (p_open && prof > 0.15f) {
				// Stamens under the bract: a tuft of anthers, a small diamond turned alternately
				// across and along the bract, so the tufts read from every side.
				const Vector3 ac = root + rad * (p_r * 0.45f) + sd * (sl * 0.1f) - ax * (sl * 0.2f);
				const float ar = p_r * 0.42f;
				for (int q = i & 1; q < 1 + (i & 1); q++) {
					const Vector3 u1 = q ? w : rad;
					const Vector3 u2 = ax;
					const int b0 = vert(ac - u2 * ar, Vector2(), p_pollen * 0.85f);
					const int b1 = vert(ac + u1 * ar, Vector2(), p_pollen);
					const int b2 = vert(ac + u2 * ar * 0.6f, Vector2(), p_pollen);
					const int b3 = vert(ac - u1 * ar, Vector2(), p_pollen);
					tri(b0, b1, b2);
					tri(b0, b2, b3);
				}
			}
		}
	};

	// Shoot tips: where cones, catkins, keys and berries hang.
	struct Tip {
		int node;
		float f; // Height in the crown, 0 base, 1 top.
	};
	LocalVector<Tip> tips;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.removed || n.dead || n.order < 1 || n.radius > 0.02f || (n.main >= 0 && !nodes[n.main].removed)) {
			continue;
		}
		tips.push_back({ i, CLAMP((n.end().y - base_y) / tall, 0.0f, 1.0f) });
	}
	const LeafSeason ls = _leaf_season(preset, season);
	int big_end = 0;
	// Mistletoe and brooms first: the coarser LODs keep the index range before big_end.
	{
		float host = 0.0f;
		switch (preset) {
			case PRESET_LIME:
				host = 0.5f;
				break;
			case PRESET_HAWTHORN:
				host = 0.35f;
				break;
			case PRESET_WHITE_WILLOW:
				host = 0.3f;
				break;
			case PRESET_ROWAN:
				host = 0.25f;
				break;
			case PRESET_SYCAMORE_MAPLE:
			case PRESET_BIRCH:
			case PRESET_RED_MAPLE:
			case PRESET_RED_OAK:
			case PRESET_BLACK_ALDER:
				host = 0.15f;
				break;
			default:
				break;
		}
		const bool american = preset == PRESET_RED_MAPLE || preset == PRESET_RED_OAK;
		if (host > 0.0f && mistletoe > 0.0f && cr.randf() < MIN(host * mistletoe, 0.95f) && tall > 4.0f) {
			// Hosts: limbs 1.5-8 cm in the upper, outer crown, where thrushes perch and wipe the
			// seeds off their bills.
			LocalVector<int> seats;
			for (int i = 1; i < int(nodes.size()); i++) {
				const Node &n = nodes[i];
				if (!n.removed && !n.dead && n.order >= 1 && n.radius > 0.015f && n.radius < 0.08f && n.end().y - base_y > crown_base + 0.35f * (tall - crown_base)) {
					seats.push_back(i);
				}
			}
			const int balls = MIN(int(seats.size()), 1 + int(cr.randf() * 7.0f * MIN(mistletoe, 2.0f)));
			const Color stem_c = american ? lin(0.42f, 0.5f, 0.2f) : lin(0.5f, 0.56f, 0.22f);
			const Color leaf0 = american ? lin(0.3f, 0.42f, 0.14f) : lin(0.44f, 0.52f, 0.17f);
			const Color leaf1 = american ? lin(0.34f, 0.46f, 0.16f) : lin(0.58f, 0.62f, 0.22f);
			const Color berry = american ? lin(0.92f, 0.92f, 0.86f) : lin(0.93f, 0.94f, 0.88f);
			const bool berries = in_days(300.0f, 60.0f);
			for (int b = 0; b < balls && !seats.is_empty(); b++) {
				const int si = int(cr.rand() % seats.size());
				const int at_node = seats[si];
				seats.remove_at_unordered(si);
				const Node &hn = nodes[at_node];
				const float u = cr.randf();
				const Vector3 at = hn.start + hn.dir * (hn.length * u);
				r_b.bone = bone_of(at_node);
				Vector4i part;
				r_b.begin(part);
				// A ball grows one fork a year: 4-8 years.
				const int levels = 4 + int(cr.randf() * 4.0f);
				struct Seg {
					Vector3 p, d;
					int level;
					float roll;
				};
				LocalVector<Seg> stack;
				const int first = 3 + int(cr.randf() * 4.0f);
				for (int k = 0; k < first; k++) {
					Vector3 d = Vector3(cr.randf() - 0.5f, cr.randf() * 0.8f - 0.1f, cr.randf() - 0.5f).normalized();
					stack.push_back({ at, d, 0, cr.randf() * float(Math::TAU) });
				}
				float reach = 0.0f;
				while (!stack.is_empty()) {
					const Seg sg = stack[stack.size() - 1];
					stack.resize(stack.size() - 1);
					const float len = Math::lerp(0.035f, 0.06f, cr.randf()) * (sg.level == 0 ? 0.7f : 1.0f);
					const float r = 0.0035f * (1.0f - 0.08f * sg.level) + 0.0015f * (levels - sg.level) / levels;
					// Stems swing up a little toward the light.
					const Vector3 d = (sg.d + Vector3(0, 0.15f, 0)).normalized();
					const Vector3 e = sg.p + d * len;
					reach = MAX(reach, e.distance_to(at));
					LocalVector<Vector3> cp;
					LocalVector<float> rr;
					LocalVector<Color> cc;
					cp.push_back(sg.p);
					cp.push_back(e);
					rr.push_back(r);
					rr.push_back(r * 0.9f);
					cc.push_back(stem_c);
					cc.push_back(stem_c);
					tube(cp, rr, 4, cc);
					const Vector3 side = perp(d).rotated(d, sg.roll);
					if (sg.level + 1 < levels) {
						// Forks in two (now and then three), each plane turned a quarter from the last.
						const float spread = Math::deg_to_rad(Math::lerp(28.0f, 42.0f, cr.randf()));
						stack.push_back({ e, d.rotated(side, spread), sg.level + 1, sg.roll + float(Math::PI) * 0.5f });
						stack.push_back({ e, d.rotated(side, -spread), sg.level + 1, sg.roll + float(Math::PI) * 0.5f });
						if (cr.randf() < 0.12f) {
							stack.push_back({ e, d, sg.level + 1, sg.roll + float(Math::PI) * 0.5f });
						}
						if (berries && sg.level + 2 >= levels && cr.randf() < 0.35f) {
							ball(e + side.cross(d) * 0.004f, 0.0045f, berry);
						}
					} else {
						// A pair of leathery, strap-shaped leaves at the end of each shoot.
						static const float prof[5] = { 0.3f, 0.8f, 1.0f, 0.85f, 0.35f };
						for (int q = 0; q < 2; q++) {
							const Vector3 ld = d.rotated(side, (q ? 1.0f : -1.0f) * Math::deg_to_rad(Math::lerp(20.0f, 40.0f, cr.randf())));
							const float ll = (american ? 0.035f : 0.045f) * Math::lerp(0.8f, 1.2f, cr.randf());
							blade(e, ld, side.cross(ld).normalized(), ll, ll * (american ? 0.45f : 0.28f), prof, 0.3f * (cr.randf() - 0.5f), 0.15f, leaf0, leaf1);
						}
					}
				}
				close_part(part, MAX(reach, 0.2f));
			}
		}
		const bool birchy = preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH || preset == PRESET_YELLOW_BIRCH;
		if (birchy && witches_brooms > 0.0f && cr.randf() < MIN(0.45f * witches_brooms, 0.95f) && tall > 5.0f) {
			LocalVector<int> seats;
			for (int i = 1; i < int(nodes.size()); i++) {
				const Node &n = nodes[i];
				if (!n.removed && !n.dead && n.order >= 1 && n.radius > 0.02f && n.radius < 0.1f && n.end().y - base_y > crown_base) {
					seats.push_back(i);
				}
			}
			const int brooms = MIN(int(seats.size()), 1 + int(cr.randf() * 4.0f * MIN(witches_brooms, 2.0f)));
			const Color twig_c = lin(0.27f, 0.15f, 0.11f);
			const Color knob_c = bark_color.srgb_to_linear().lerp(Color(0.12f, 0.09f, 0.07f), 0.6f); // Dark gall bark.
			for (int b = 0; b < brooms && !seats.is_empty(); b++) {
				const int si = int(cr.rand() % seats.size());
				const int at_node = seats[si];
				seats.remove_at_unordered(si);
				const Node &hn = nodes[at_node];
				const Vector3 at = hn.start + hn.dir * (hn.length * cr.randf());
				r_b.bone = bone_of(at_node);
				Vector4i part;
				r_b.begin(part);
				// The infection's swelling: a lumpy gall of a few overlapping knobs, not one ball.
				const int knobs = 3 + int(cr.randf() * 4.0f);
				for (int q = 0; q < knobs; q++) {
					const Vector3 o = Vector3(cr.randf() - 0.5f, cr.randf() - 0.3f, cr.randf() - 0.5f) * (hn.radius * 2.0f);
					ball(at + o, hn.radius * Math::lerp(0.7f, 1.3f, cr.randf()), knob_c * Math::lerp(0.75f, 1.0f, cr.randf()));
				}
				// Then the broom, modelled (Taphrina betulina; Spanos and Woodward 1994): from the gall
				// many shoots, each a run of short internodes kinked at every bud, branching again
				// and again, all turning upward: a dense bird's-nest of twigs. Straight rays from one
				// point read as a sea urchin.
				const Vector3 out = (hn.dir.cross(Vector3(0, 1, 0)).cross(hn.dir)).normalized();
				struct Shoot {
					Vector3 p, d;
					float r;
					int level;
				};
				LocalVector<Shoot> stack;
				const int primaries = 16 + int(cr.randf() * 18.0f);
				for (int q = 0; q < primaries; q++) {
					const Vector3 d = (out * 0.4f + Vector3(0, 0.6f, 0) + Vector3(cr.randf() - 0.5f, cr.randf() - 0.4f, cr.randf() - 0.5f) * 1.8f).normalized();
					stack.push_back({ at + d * (hn.radius * 1.1f), d, Math::lerp(0.0022f, 0.0032f, cr.randf()), 0 });
				}
				float reach = 0.0f;
				int budget_shoots = 220;
				while (!stack.is_empty() && budget_shoots-- > 0) {
					const Shoot sh = stack[stack.size() - 1];
					stack.resize(stack.size() - 1);
					const int internodes = sh.level == 0 ? 4 + int(cr.randf() * 4.0f) : 2 + int(cr.randf() * 3.0f);
					LocalVector<Vector3> cp;
					LocalVector<float> rr;
					LocalVector<Color> cc;
					Vector3 p = sh.p;
					Vector3 d = sh.d;
					for (int k = 0; k <= internodes; k++) {
						const float t = float(k) / internodes;
						cp.push_back(p);
						rr.push_back(sh.r * (1.0f - 0.6f * t));
						cc.push_back(twig_c * Math::lerp(1.0f, 1.25f, t));
						if (k == internodes) {
							break;
						}
						// A kink at each bud, alternate sides; the shoot turns upward as it goes.
						const Vector3 kick = perp(d).rotated(d, cr.randf() * float(Math::TAU));
						d = (d + kick * Math::lerp(0.25f, 0.5f, cr.randf()) + Vector3(0, 0.18f, 0)).normalized();
						const float il = Math::lerp(0.025f, 0.055f, cr.randf()) * (sh.level == 0 ? 1.0f : 0.8f);
						p += d * il;
						if (sh.level < 2 && k > 0 && cr.randf() < (sh.level == 0 ? 0.45f : 0.3f)) {
							const Vector3 sd2 = (d + perp(d).rotated(d, cr.randf() * float(Math::TAU)) * 0.9f + Vector3(0, 0.2f, 0)).normalized();
							stack.push_back({ p, sd2, sh.r * (1.0f - 0.6f * t) * 0.75f, sh.level + 1 });
						}
					}
					reach = MAX(reach, p.distance_to(at));
					tube(cp, rr, 3, cc);
				}
				close_part(part, MAX(reach, 0.2f));
			}
		}
		big_end = r_b.indices.size();
	}
	if (fruit_cover > 0.0f && params[PARAM_NEEDLES] <= 0.5f && !tips.is_empty()) {
		// Winter buds at the shoot tips, modelled: each species' own (Poland and Clement, The
		// Vegetative Key to the British Flora, 2009): rowan's big dark purple woolly cones, ash's
		// black, beech's long brown spindles, oak's orange-brown clusters, horse chestnut's large
		// sticky red-brown ones, sycamore's green. Set from late summer, full size through winter,
		// gone at budburst; a bare tip read as a cut stick.
		float blen = 0.006f, bwr = 0.35f;
		Color bc = lin(0.35f, 0.24f, 0.16f), bt = lin(0.25f, 0.16f, 0.11f);
		int cluster = 1;
		switch (preset) {
			case PRESET_ROWAN:
				blen = 0.014f, bwr = 0.32f, bc = lin(0.28f, 0.17f, 0.2f), bt = lin(0.42f, 0.36f, 0.38f);
				break;
			case PRESET_ASH:
				blen = 0.008f, bwr = 0.6f, bc = lin(0.07f, 0.06f, 0.06f), bt = lin(0.12f, 0.11f, 0.1f);
				break;
			case PRESET_BEECH:
			case PRESET_AMERICAN_BEECH:
				blen = 0.02f, bwr = 0.16f, bc = lin(0.55f, 0.36f, 0.22f), bt = lin(0.42f, 0.27f, 0.17f);
				break;
			case PRESET_OAK:
			case PRESET_RED_OAK:
			case PRESET_WHITE_OAK:
				blen = 0.006f, bwr = 0.45f, bc = lin(0.55f, 0.33f, 0.17f), bt = lin(0.4f, 0.24f, 0.13f), cluster = 3;
				break;
			case PRESET_HORSE_CHESTNUT:
				blen = 0.022f, bwr = 0.4f, bc = lin(0.35f, 0.14f, 0.08f), bt = lin(0.2f, 0.08f, 0.05f);
				break;
			case PRESET_SYCAMORE_MAPLE:
			case PRESET_SUGAR_MAPLE:
			case PRESET_RED_MAPLE:
				blen = 0.009f, bwr = 0.4f, bc = preset == PRESET_SYCAMORE_MAPLE ? lin(0.3f, 0.42f, 0.18f) : lin(0.45f, 0.18f, 0.12f), bt = lin(0.2f, 0.17f, 0.1f);
				break;
			case PRESET_LIME:
				blen = 0.007f, bwr = 0.55f, bc = lin(0.55f, 0.15f, 0.1f), bt = lin(0.4f, 0.32f, 0.15f);
				break;
			case PRESET_BLACK_ALDER:
				blen = 0.008f, bwr = 0.4f, bc = lin(0.32f, 0.15f, 0.22f), bt = lin(0.25f, 0.12f, 0.17f);
				break;
			case PRESET_HAZEL:
				blen = 0.005f, bwr = 0.55f, bc = lin(0.35f, 0.33f, 0.2f), bt = lin(0.3f, 0.22f, 0.15f);
				break;
			default:
				break;
		}
		// Summer: forming, half size; gone from budburst (about day 100-130) to midsummer.
		const float bud_size = in_days(200.0f, 105.0f) ? 1.0f : (in_days(160.0f, 200.0f) ? 0.5f : 0.0f);
		if (bud_size > 0.0f) {
			Vector4i part;
			r_b.begin(part);
			int bone_now = -1;
			for (const Tip &t : tips) {
				const Node &n = nodes[t.node];
				if (n.radius > 0.005f || n.main >= 0) {
					continue; // A stub (its leader shed or cut), not a growing tip.
				}
				const int b = bone_of(t.node);
				if (b != bone_now) {
					// One part a bone: the part's bone weights are per part.
					if (bone_now >= 0) {
						close_part(part, 0.02f);
						r_b.begin(part);
					}
					r_b.bone = b;
					bone_now = b;
				}
				const Vector3 d = n.dir;
				const Vector3 sd = perp(d);
				for (int q = 0; q < MIN(cluster, 2); q++) {
					const Vector3 bd = q == 0 ? d : (d + sd.rotated(d, q * 2.4f) * 0.7f).normalized();
					const float L = blen * bud_size * (q == 0 ? 1.0f : 0.7f) * Math::lerp(0.85f, 1.15f, cr.randf());
					const float W = L * bwr;
					const Vector3 b0 = n.end() - d * (0.15f * L) + (q == 0 ? Vector3() : bd * (0.2f * L));
					// Rings of scales: each a little narrower, alternately darker at its edge.
					LocalVector<Vector3> bp;
					LocalVector<float> br;
					LocalVector<Color> bcol;
					// Closing to a point: a bud is a cone of scales, not a cut stub.
					static const float ts[4] = { 0.0f, 0.35f, 0.8f, 1.0f };
					static const float ws[4] = { 0.8f, 1.0f, 0.55f, 0.08f };
					for (int k = 0; k < 4; k++) {
						bp.push_back(b0 + bd * (L * ts[k]));
						// Seated on the shoot's end, never thinner than it there.
						br.push_back(k < 2 ? MAX(W * ws[k], n.radius * (k == 0 ? 0.8f : 0.75f)) : W * ws[k]);
						bcol.push_back((k & 1) ? bc : bt);
					}
					tube(bp, br, 4, bcol);
				}
			}
			close_part(part, 0.02f);
		}
	}
	if (fruit_cover <= 0.0f || tips.is_empty()) {
		return big_end;
	}
	// Per species: what hangs at the tips, how many tips carry it (in the part of the crown it
	// keeps to), and how many in a cluster.
	enum Kind {
		NONE,
		CONE_PINE,
		CONE_SPRUCE,
		CONE_LARCH,
		CONE_HEMLOCK,
		CONE_WHITE_PINE,
		CATKIN_HAZEL,
		CATKIN_BIRCH,
		CATKIN_ALDER,
		KEYS_ASH,
		KEYS_MAPLE,
		BERRY_HOLLY,
		BERRY_HAW,
		BERRY_ROWAN,
		BERRY_YEW,
		BERRY_ELDER,
	};
	Kind kind = NONE;
	float share = 0.0f, low = 0.0f; // Share of the tips above `low` (0-1 up the crown).
	switch (preset) {
		case PRESET_SCOTS_PINE:
			kind = CONE_PINE, share = 0.12f, low = 0.45f;
			break;
		case PRESET_NORWAY_SPRUCE:
			kind = CONE_SPRUCE, share = 0.15f, low = 0.7f;
			break;
		case PRESET_LARCH:
			kind = CONE_LARCH, share = 0.3f, low = 0.2f;
			break;
		case PRESET_HEMLOCK:
			kind = CONE_HEMLOCK, share = 0.2f, low = 0.3f;
			break;
		case PRESET_WHITE_PINE:
			kind = CONE_WHITE_PINE, share = 0.08f, low = 0.6f;
			break;
		case PRESET_HAZEL:
		case PRESET_WITCH_HAZEL:
			kind = preset == PRESET_HAZEL ? CATKIN_HAZEL : NONE, share = 0.45f, low = 0.2f;
			break;
		case PRESET_BIRCH:
		case PRESET_PAPER_BIRCH:
		case PRESET_YELLOW_BIRCH:
			kind = CATKIN_BIRCH, share = 0.35f, low = 0.3f;
			break;
		case PRESET_BLACK_ALDER:
			kind = CATKIN_ALDER, share = 0.35f, low = 0.25f;
			break;
		case PRESET_ASH:
			kind = KEYS_ASH, share = 0.1f, low = 0.4f;
			break;
		case PRESET_SYCAMORE_MAPLE:
		case PRESET_SUGAR_MAPLE:
		case PRESET_RED_MAPLE:
			kind = KEYS_MAPLE, share = 0.1f, low = 0.3f;
			break;
		case PRESET_HOLLY:
			kind = BERRY_HOLLY, share = 0.3f, low = 0.0f;
			break;
		case PRESET_HAWTHORN:
			kind = BERRY_HAW, share = 0.35f, low = 0.2f;
			break;
		case PRESET_ROWAN:
			kind = BERRY_ROWAN, share = 0.12f, low = 0.3f;
			break;
		case PRESET_YEW:
			kind = BERRY_YEW, share = 0.25f, low = 0.0f;
			break;
		case PRESET_ELDER:
			kind = BERRY_ELDER, share = 0.15f, low = 0.2f;
			break;
		default:
			break;
	}
	// Dioecious species: only the female trees fruit (holly, yew), by seed.
	if ((kind == BERRY_HOLLY || kind == BERRY_YEW) && (uint32_t(seed) * 2654435761u >> 16) % 2 == 1) {
		kind = NONE;
	}
	// In season (day of year): keys hang from late summer into spring (ash) or fall by early
	// winter (maples, a few left); berries ripen in autumn and the birds strip them by midwinter
	// (holly and haws keep longest); catkins form in late summer, open in late winter and drop.
	float present = 1.0f;
	switch (kind) {
		case KEYS_ASH:
			present = in_days(200.0f, 120.0f) ? 1.0f : 0.0f;
			break;
		case KEYS_MAPLE:
			present = in_days(180.0f, 320.0f) ? 1.0f : (in_days(320.0f, 60.0f) ? 0.2f : 0.0f);
			break;
		case BERRY_HOLLY:
			present = in_days(270.0f, 75.0f) ? 1.0f : 0.0f;
			break;
		case BERRY_HAW:
			present = in_days(250.0f, 30.0f) ? 1.0f : 0.0f;
			break;
		case BERRY_ROWAN:
			present = in_days(220.0f, 300.0f) ? 1.0f : 0.0f;
			break;
		case BERRY_YEW:
			present = in_days(240.0f, 320.0f) ? 1.0f : 0.0f;
			break;
		case BERRY_ELDER:
			present = in_days(225.0f, 270.0f) ? 1.0f : 0.0f;
			break;
		case CATKIN_HAZEL:
			present = in_days(210.0f, 100.0f) ? 1.0f : 0.0f;
			break;
		case CATKIN_BIRCH:
			present = in_days(220.0f, 135.0f) ? 1.0f : 0.0f;
			break;
		default:
			break;
	}
	if (kind == NONE || present <= 0.0f) {
		return big_end;
	}
	// A budget of clusters, so a big crown carries a few hundred, not tens of thousands.
	int eligible = 0;
	for (const Tip &t : tips) {
		eligible += t.f >= low ? 1 : 0;
	}
	// Corymbs carry a hundred berries or more each: a rowan bears some dozens of heads.
	const float budget = (kind == BERRY_ROWAN || kind == BERRY_ELDER ? 80.0f : 360.0f) * fruit_cover;
	const float p = MIN(share * fruit_cover * present, eligible > 0 ? budget / eligible : 0.0f);
	// Catkins open before the leaves: hazel from late January, alder February, birch April.
	const bool open_catkin = (kind == CATKIN_HAZEL && in_days(20.0f, 100.0f)) || (kind == CATKIN_ALDER && in_days(40.0f, 100.0f)) || (kind == CATKIN_BIRCH && in_days(100.0f, 135.0f));
	for (const Tip &t : tips) {
		if (t.f < low || cr.randf() >= p) {
			continue;
		}
		const Node &n = nodes[t.node];
		const Vector3 at = n.end();
		const Vector3 d = n.dir;
		r_b.bone = bone_of(t.node);
		Vector4i part;
		r_b.begin(part);
		const Vector3 sd = perp(d);
		float size = 0.05f;
		switch (kind) {
			case CONE_PINE: {
				// One to three, ovoid, 4-7 cm, bent back on the shoot; grey-tipped scales.
				const int k = 1 + int(cr.randf() * cr.randf() * 3.0f);
				for (int q = 0; q < k; q++) {
					const Vector3 ax = (d * 0.2f + Vector3(0, -1, 0) + sd.rotated(d, q * 2.1f) * 0.6f).normalized();
					const float len = Math::lerp(0.04f, 0.065f, cr.randf());
					const float op = cr.randf() < 0.6f ? Math::lerp(0.5f, 1.0f, cr.randf()) : 0.0f;
					cone(at - d * 0.02f * q, ax, len, len * 0.32f * (1.0f + 0.4f * op), 56, false, op, lin(0.36f, 0.25f, 0.15f), op > 0.0f ? lin(0.52f, 0.47f, 0.38f) : lin(0.42f, 0.36f, 0.22f));
				}
				size = 0.07f;
			} break;
			case CONE_WHITE_PINE: {
				const Vector3 ax = (d * 0.1f + Vector3(0, -1, 0)).normalized();
				const float len = Math::lerp(0.1f, 0.16f, cr.randf());
				cone(at, ax, len, len * 0.14f, 70, true, Math::lerp(0.4f, 0.8f, cr.randf()), lin(0.4f, 0.28f, 0.16f), lin(0.6f, 0.5f, 0.36f));
				size = len;
			} break;
			case CONE_SPRUCE: {
				// Long pendant cylinders, 10-16 cm, at the very top of the crown.
				const int k = 1 + int(cr.randf() * 2.5f);
				for (int q = 0; q < k; q++) {
					const Vector3 ax = (Vector3(0, -1, 0) + sd.rotated(d, q * 2.4f) * 0.25f).normalized();
					const float len = Math::lerp(0.1f, 0.16f, cr.randf());
					cone(at - d * 0.03f * q, ax, len, len * 0.17f, 96, true, Math::lerp(0.0f, 0.4f, cr.randf()), lin(0.42f, 0.27f, 0.14f), lin(0.56f, 0.4f, 0.24f));
				}
				size = 0.15f;
			} break;
			case CONE_LARCH: {
				// Small upright cones, 2-3.5 cm, kept for years along the branch.
				const int k = 1 + int(cr.randf() * 3.0f);
				for (int q = 0; q < k; q++) {
					const float u = cr.randf();
					const Vector3 base = n.start + n.dir * (n.length * u) + sd.rotated(d, q * 2.0f) * 0.004f;
					const Vector3 ax = (Vector3(0, 1, 0) + sd.rotated(d, q * 2.0f) * 0.2f).normalized();
					const float len = Math::lerp(0.02f, 0.035f, cr.randf());
					cone(base, ax, len, len * 0.42f, 28, false, 0.85f, lin(0.38f, 0.27f, 0.17f), lin(0.55f, 0.43f, 0.3f));
				}
				size = 0.04f;
			} break;
			case CONE_HEMLOCK: {
				const Vector3 ax = (d * 0.3f + Vector3(0, -1, 0)).normalized();
				const float len = Math::lerp(0.015f, 0.025f, cr.randf());
				cone(at, ax, len, len * 0.4f, 22, false, 0.8f, lin(0.4f, 0.29f, 0.18f), lin(0.58f, 0.47f, 0.33f));
				size = 0.03f;
			} break;
			case CATKIN_HAZEL:
			case CATKIN_BIRCH:
			case CATKIN_ALDER: {
				const int k = kind == CATKIN_ALDER ? 3 + int(cr.randf() * 3.0f) : 1 + int(cr.randf() * (kind == CATKIN_HAZEL ? 3.0f : 2.0f));
				const float len0 = kind == CATKIN_HAZEL ? 0.025f : (kind == CATKIN_BIRCH ? 0.025f : 0.03f);
				const float len1 = kind == CATKIN_HAZEL ? 0.065f : (kind == CATKIN_BIRCH ? 0.06f : 0.08f);
				const float len = (open_catkin ? len1 : len0) * Math::lerp(0.85f, 1.15f, cr.randf());
				Color a, b;
				if (open_catkin) {
					a = kind == CATKIN_HAZEL ? lin(0.64f, 0.58f, 0.3f) : (kind == CATKIN_BIRCH ? lin(0.72f, 0.6f, 0.3f) : lin(0.55f, 0.32f, 0.2f));
					b = kind == CATKIN_HAZEL ? lin(0.48f, 0.4f, 0.22f) : (kind == CATKIN_BIRCH ? lin(0.56f, 0.44f, 0.24f) : lin(0.42f, 0.24f, 0.17f));
				} else {
					a = kind == CATKIN_HAZEL ? lin(0.52f, 0.42f, 0.28f) : (kind == CATKIN_BIRCH ? lin(0.42f, 0.28f, 0.2f) : lin(0.4f, 0.22f, 0.2f));
					b = a * 0.8f;
				}
				for (int q = 0; q < k; q++) {
					const Vector3 cd = (d * 0.5f + sd.rotated(d, q * 2.3f) * 0.5f + Vector3(0, open_catkin ? -0.6f : 0.0f, 0)).normalized();
					const Color pollen = kind == CATKIN_ALDER ? lin(0.75f, 0.55f, 0.2f) : lin(0.93f, 0.82f, 0.3f);
					catkin(at + cd * 0.004f, cd, len, open_catkin ? 0.0045f : 0.0035f, a, b, open_catkin ? 0.35f : 0.12f, open_catkin, pollen);
				}
				if (kind == CATKIN_ALDER) {
					// Last year's woody fruit cones, 1.5 cm, in clusters behind the catkins.
					const int c = 2 + int(cr.randf() * 4.0f);
					for (int q = 0; q < c; q++) {
						const Vector3 ax = (Vector3(0, -1, 0) + sd.rotated(d, q * 1.7f) * 0.6f).normalized();
						cone(n.start + n.dir * (n.length * 0.5f) + ax * 0.01f, ax, 0.016f, 0.0065f, 18, false, 0.7f, lin(0.25f, 0.16f, 0.1f), lin(0.36f, 0.26f, 0.17f));
					}
				}
				size = len;
			} break;
			case KEYS_ASH:
			case KEYS_MAPLE: {
				// Ash: bunches of single twisted keys on stalks, buff-brown in winter. Maples:
				// paired keys, wings spread, green to straw.
				const bool ash = kind == KEYS_ASH;
				const int k = ash ? 10 + int(cr.randf() * 18.0f) : 3 + int(cr.randf() * 5.0f);
				const bool ripe = !in_days(130.0f, 230.0f);
				const Color c0 = ripe ? lin(0.5f, 0.38f, 0.22f) : lin(0.45f, 0.55f, 0.22f);
				const Color c1 = ripe ? lin(0.66f, 0.55f, 0.36f) : lin(0.58f, 0.65f, 0.3f);
				static const float key_prof[5] = { 0.35f, 0.6f, 0.9f, 1.0f, 0.55f };
				const Vector3 hub = at + Vector3(0, -0.02f, 0);
				const Color stalk_c = c0 * 0.7f;
				// The bunch hangs on its own stalk, each key on a short branch of it.
				stalk(at, hub, 0.0012f, stalk_c);
				// A panicle: the stalk branches into a few sub-stalks and the keys hang along those,
				// not from one point as a skirt.
				LocalVector<Vector3> subs;
				const int nsub = ash ? 3 + int(cr.randf() * 3.0f) : 1;
				for (int q = 0; q < nsub; q++) {
					const Vector3 sd2 = (Vector3(0, -1, 0) + Vector3(cr.randf() - 0.5f, 0.0f, cr.randf() - 0.5f) * 1.4f).normalized();
					const Vector3 e = ash ? hub + sd2 * Math::lerp(0.015f, 0.035f, cr.randf()) : hub;
					stalk(hub, e, 0.0009f, stalk_c);
					subs.push_back(e);
				}
				for (int q = 0; q < k; q++) {
					const Vector3 sub = subs[q % subs.size()];
					const Vector3 from = ash ? hub.lerp(sub, Math::lerp(0.3f, 1.0f, cr.randf())) : hub;
					const Vector3 kd = (Vector3(0, -1, 0) + Vector3(cr.randf() - 0.5f, 0.0f, cr.randf() - 0.5f) * 0.9f).normalized();
					const Vector3 st = from + kd * Math::lerp(0.006f, 0.015f, cr.randf());
					stalk(from, st, 0.0006f, stalk_c);
					if (ash) {
						blade(st, kd, perp(kd).rotated(kd, cr.randf() * 3.0f), 0.035f, 0.008f, key_prof, Math::lerp(0.5f, 1.2f, cr.randf()), 0.05f, c0 * 0.8f, c1);
						// The seed: a slim swelling down the key's first third, darker.
						LocalVector<Vector3> sp;
						LocalVector<float> sr;
						LocalVector<Color> sc;
						for (int j = 0; j < 3; j++) {
							sp.push_back(st + kd * (0.013f * j / 2.0f));
							sr.push_back(j == 1 ? 0.0016f : 0.0008f);
							sc.push_back(c0 * 0.75f);
						}
						tube(sp, sr, 4, sc);
					} else {
						const Vector3 w = perp(kd).rotated(kd, cr.randf() * 3.0f);
						const float spread = Math::deg_to_rad(preset == PRESET_SUGAR_MAPLE ? 30.0f : 45.0f);
						for (int h = 0; h < 2; h++) {
							const Vector3 wd = kd.rotated(w.cross(kd).normalized(), (h ? 1.0f : -1.0f) * spread);
							blade(st, wd, w, 0.04f, 0.012f, key_prof, 0.2f, 0.1f, c0 * 0.75f, c1);
						}
						ball(st, 0.004f, c0 * 0.8f);
					}
				}
				size = 0.08f;
			} break;
			case BERRY_ROWAN:
			case BERRY_ELDER: {
				// A corymb (rowan) or flat cyme (elder), modelled: a dense, flat-topped dome of
				// 90-160 berries (elder 150-240), 10-14 cm across (elder 14-18), on stalks that
				// branch three times (peduncle, five to seven rays, branchlets, pedicels), red-brown
				// on rowan and purple-red on elder, the whole head bowed under its weight when ripe
				// (Rich, Sorbus aucuparia, Biological Flora of the British Isles, 2009; Atkinson and
				// Atkinson, Sambucus nigra, J. Ecol. 90, 2002). A loose burst of berries on straight
				// hairs from one point read as a firework.
				const bool rowan = kind == BERRY_ROWAN;
				const int nb = rowan ? 90 + int(cr.randf() * 70.0f) : 150 + int(cr.randf() * 90.0f);
				const float rb0 = rowan ? 0.0042f : 0.0026f;
				const float R = (rowan ? 0.05f : 0.07f) * Math::lerp(0.9f, 1.15f, cr.randf());
				const float H = rowan ? 0.05f : 0.06f; // From the first fork to the dome's top.
				const Color c = rowan ? lin(0.86f, 0.3f, 0.07f) : lin(0.08f, 0.04f, 0.08f);
				const Color stalk_c = rowan ? lin(0.55f, 0.27f, 0.17f) : lin(0.45f, 0.13f, 0.2f);
				const Vector3 down(0, -1, 0);
				const Vector3 a = (d * 0.35f + down * (rowan ? 0.8f : 1.1f) + sd * 0.3f).normalized();
				const Vector3 u1 = perp(a), u2 = a.cross(u1).normalized();
				// A stalk bowed by the weight it carries: the middle sags.
				auto bowed = [&](const Vector3 &p_a, const Vector3 &p_b, float p_r, float p_sag) {
					LocalVector<Vector3> sp;
					LocalVector<float> sr;
					LocalVector<Color> sc;
					const float len = p_a.distance_to(p_b);
					sp.push_back(p_a);
					sp.push_back((p_a + p_b) * 0.5f + down * (p_sag * len));
					sp.push_back(p_b);
					sr.push_back(p_r);
					sr.push_back(p_r * 0.9f);
					sr.push_back(p_r * 0.8f);
					for (int q = 0; q < 3; q++) {
						sc.push_back(stalk_c);
					}
					tube(sp, sr, 3, sc);
				};
				const Vector3 j0 = at + a * 0.03f + down * 0.006f;
				bowed(at, j0, 0.0019f, 0.12f);
				const int rays = rowan ? 5 + int(cr.randf() * 3.0f) : 5;
				const float rot = cr.randf() * float(Math::TAU);
				// Berries on the dome, a sunflower spiral (Vogel 1979) jittered, so they pack
				// like the real head; each ray serves its sector, a branchlet per ring band.
				struct B {
					Vector3 p;
					float r;
					int ray, band;
				};
				LocalVector<B> berries;
				for (int i = 0; i < nb; i++) {
					const float rr = R * Math::sqrt((i + 0.5f) / nb) * Math::lerp(0.94f, 1.04f, cr.randf());
					const float th = i * 2.39996f + 0.15f * (cr.randf() - 0.5f);
					const Vector3 radial = u1 * Math::cos(th) + u2 * Math::sin(th);
					const float f = rr / R;
					const float z = H * (1.0f - 0.3f * f * f) + 0.004f * (cr.randf() - 0.5f);
					const float sec = Math::fposmod(th - rot, float(Math::TAU)) / float(Math::TAU) * rays;
					B b;
					b.p = j0 + a * z + radial * rr;
					b.r = rb0 * Math::lerp(0.85f, 1.15f, cr.randf());
					b.ray = MIN(int(sec), rays - 1);
					b.band = f < 0.45f ? 0 : (f < 0.75f ? 1 : 2);
					berries.push_back(b);
				}
				for (int ry = 0; ry < rays; ry++) {
					const float th = rot + (ry + 0.5f) / rays * float(Math::TAU);
					const Vector3 radial = u1 * Math::cos(th) + u2 * Math::sin(th);
					const Vector3 pj = j0 + a * (0.4f * H) + radial * (0.3f * R);
					bowed(j0, pj, 0.0013f, 0.1f);
					for (int band = 0; band < 3; band++) {
						Vector3 cen;
						int cnt = 0;
						for (const B &b : berries) {
							if (b.ray == ry && b.band == band) {
								cen += b.p;
								cnt++;
							}
						}
						if (cnt == 0) {
							continue;
						}
						cen /= float(cnt);
						// The branchlet ends under its berries, a little short of them.
						const Vector3 sj = pj.lerp(cen, 0.7f) - a * 0.006f;
						bowed(pj, sj, 0.0009f, 0.08f);
						for (const B &b : berries) {
							if (b.ray != ry || b.band != band) {
								continue;
							}
							const Vector3 to = (b.p - sj).normalized();
							stalk(sj, b.p - to * (b.r * 0.85f), 0.00045f, stalk_c);
							ball(b.p, b.r, c * Math::lerp(0.82f, 1.1f, cr.randf()));
							// The calyx: five dark teeth round a pit at the berry's far end.
							const Vector3 cc0 = b.p + to * (b.r * 0.98f);
							const Vector3 v1 = perp(to), v2 = to.cross(v1);
							const float cs = b.r * 0.5f;
							const Color dk = rowan ? lin(0.18f, 0.08f, 0.04f) : c * 0.5f;
							const int s0 = vert(cc0 + v1 * cs, Vector2(), dk), s1 = vert(cc0 + (v1 * -0.5f + v2 * 0.87f) * cs, Vector2(), dk), s2 = vert(cc0 + (v1 * -0.5f - v2 * 0.87f) * cs, Vector2(), dk);
							const int s3 = vert(cc0 - v1 * cs, Vector2(), dk), s4 = vert(cc0 + (v1 * 0.5f - v2 * 0.87f) * cs, Vector2(), dk), s5 = vert(cc0 + (v1 * 0.5f + v2 * 0.87f) * cs, Vector2(), dk);
							tri(s0, s1, s2);
							tri(s3, s4, s5);
						}
					}
				}
				size = 2.0f * R;
			} break;
			case BERRY_HOLLY:
			case BERRY_HAW:
			case BERRY_YEW: {
				const int k = kind == BERRY_ROWAN ? 15 + int(cr.randf() * 20.0f) : (kind == BERRY_ELDER ? 25 + int(cr.randf() * 30.0f) : 2 + int(cr.randf() * 5.0f));
				const float br = kind == BERRY_ELDER ? 0.0028f : (kind == BERRY_ROWAN ? 0.0045f : (kind == BERRY_YEW ? 0.004f : 0.005f));
				const Color c = kind == BERRY_ELDER ? lin(0.08f, 0.05f, 0.08f) : (kind == BERRY_ROWAN ? lin(0.88f, 0.35f, 0.08f) : (kind == BERRY_HAW ? lin(0.55f, 0.08f, 0.06f) : lin(0.75f, 0.06f, 0.05f)));
				const float spread = kind == BERRY_ROWAN || kind == BERRY_ELDER ? 0.045f : 0.012f;
				const Vector3 hub = kind == BERRY_HOLLY || kind == BERRY_YEW ? n.start + n.dir * (n.length * 0.6f) : at + Vector3(0, -0.015f, 0);
				// Each berry on its own stalk from the cluster's (a corymb on rowan and elder), with
				// the dark remains of its calyx at the far end.
				const Color stalk_c = kind == BERRY_ELDER ? lin(0.35f, 0.12f, 0.18f) : (kind == BERRY_ROWAN ? lin(0.5f, 0.38f, 0.2f) : lin(0.35f, 0.3f, 0.18f));
				if (kind != BERRY_HOLLY && kind != BERRY_YEW) {
					stalk(at, hub, 0.0013f, stalk_c);
				}
				for (int q = 0; q < k; q++) {
					const Vector3 o = Vector3(cr.randf() - 0.5f, (cr.randf() - 0.8f) * 0.6f, cr.randf() - 0.5f) * (2.0f * spread);
					const float rb = br * Math::lerp(0.85f, 1.15f, cr.randf());
					const Vector3 bc = hub + o;
					const Vector3 away = o.length_squared() > 1e-10f ? o.normalized() : Vector3(0, -1, 0);
					stalk(hub, bc - away * (rb * 0.8f), 0.00045f, stalk_c);
					ball(bc, rb, c * Math::lerp(0.85f, 1.1f, cr.randf()));
					if (kind != BERRY_YEW) {
						// The calyx: a small dark five-pointed scar, as a two-triangle star.
						const Vector3 cc0 = bc + away * (rb * 0.97f);
						const Vector3 u1 = perp(away), u2 = away.cross(u1);
						const float cs = rb * 0.35f;
						const Color dk = c * 0.2f;
						const int s0 = vert(cc0 + u1 * cs, Vector2(), dk), s1 = vert(cc0 + (u1 * -0.5f + u2 * 0.87f) * cs, Vector2(), dk), s2 = vert(cc0 + (u1 * -0.5f - u2 * 0.87f) * cs, Vector2(), dk);
						const int s3 = vert(cc0 - u1 * cs, Vector2(), dk), s4 = vert(cc0 + (u1 * 0.5f - u2 * 0.87f) * cs, Vector2(), dk), s5 = vert(cc0 + (u1 * 0.5f + u2 * 0.87f) * cs, Vector2(), dk);
						tri(s0, s1, s2);
						tri(s3, s4, s5);
					}
				}
				size = spread * 2.0f;
			} break;
			default:
				break;
		}
		close_part(part, size);
	}
	(void)ls;
	return big_end;
}

void TreeGenerator::_grow_fungi(const Builder &p_bark, Builder &r_fungi) {
	r_fungi = Builder();
	fungi_points.clear();
	if ((fungi_cover <= 0.0f && lichen_cover <= 0.0f) || nodes.is_empty() || p_bark.verts.is_empty()) {
		return;
	}
	RandomPCG fr;
	fr.seed(uint64_t(seed) * 2862933555777941757ull + uint64_t(decay_class) * 131u + 0x66756eu);
	const bool conifer = params[PARAM_NEEDLES] > 0.5f;
	const bool birch = preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH || preset == PRESET_YELLOW_BIRCH;
	const bool oak = preset == PRESET_OAK || preset == PRESET_RED_OAK || preset == PRESET_WHITE_OAK;
	const bool lying = fallen || fallen_branch;
	const float ground = nodes[0].start.y;
	const float moss_k = _moss_amount();
	// Clusters to place: (kind, at the foot).
	struct Want {
		FungusKind kind;
		bool foot;
	};
	LocalVector<Want> want;
	auto chance = [&](float p_p) { return fr.randf() < p_p * fungi_cover; };
	auto count = [&](float p_mean) {
		const float m = p_mean * fungi_cover;
		int n = int(Math::floor(m));
		if (fr.randf() < m - n) {
			n++;
		}
		return n;
	};
	const FungusKind snag_kind = conifer ? FUNGUS_PINICOLA : (birch && fr.randf() < 0.6f ? FUNGUS_BETULINA : FUNGUS_FOMES);
	if (fallen_branch) {
		if (decay_class >= 2 && decay_class <= 4) {
			for (int i = count(0.8f); i > 0; i--) {
				want.push_back({ FUNGUS_TRAMETES, false });
			}
		}
	} else if (fallen) {
		static const float LOG_TIERS[6] = { 0.0f, 1.5f, 4.0f, 5.0f, 2.5f, 0.8f };
		for (int i = count(LOG_TIERS[CLAMP(decay_class, 0, 5)]); i > 0; i--) {
			want.push_back({ FUNGUS_TRAMETES, false });
		}
		if (decay_class >= 2 && decay_class <= 3 && chance(0.5f)) {
			want.push_back({ snag_kind, false });
		}
	} else if (decay_class > 0) {
		// Perennial brackets of the snag's own decayer, most on class 2-3 snags.
		static const float SNAG[6] = { 0.0f, 1.0f, 2.5f, 3.0f, 1.5f, 0.6f };
		for (int i = count(SNAG[CLAMP(decay_class, 0, 5)]); i > 0; i--) {
			want.push_back({ snag_kind, false });
		}
		if (!conifer && decay_class <= 3 && chance(0.45f)) {
			want.push_back({ FUNGUS_GANODERMA, true });
		}
		if (decay_class >= 2 && chance(0.6f)) {
			want.push_back({ FUNGUS_TRAMETES, true });
		}
	} else if (veteran > 0.0f) {
		// Heart rot of old living trees: the fruit bodies show where it reached the surface.
		if (oak && chance(0.6f * veteran)) {
			want.push_back({ FUNGUS_LAETIPORUS, false });
		}
		if (!conifer && chance(0.4f * veteran)) {
			want.push_back({ FUNGUS_GANODERMA, true });
		}
		if (!conifer && !oak && chance(0.25f * veteran)) {
			want.push_back({ FUNGUS_FOMES, false });
		}
	}
	// Gilled mushrooms in clusters: honey fungus at the foot of dying and dead broadleaves (its
	// rhizomorphs kill and then rot them), sulphur tuft on stumps and logs. Autumn fruiters; the
	// season is left to the placement for now.
	if (!fallen_branch) {
		if (fallen) {
			if (decay_class >= 2 && decay_class <= 4) {
				for (int i = count(1.2f); i > 0; i--) {
					want.push_back({ FUNGUS_TUFT, false });
				}
				if (!conifer && chance(0.35f)) {
					want.push_back({ FUNGUS_HONEY, false });
				}
			}
		} else if (!conifer && ((decay_class >= 1 && decay_class <= 3 && chance(0.55f)) || (decay_class == 0 && chance(0.3f * veteran)))) {
			want.push_back({ FUNGUS_HONEY, true });
		}
		if (!fallen && decay_class >= 2 && decay_class <= 4 && chance(0.4f)) {
			want.push_back({ FUNGUS_TUFT, true });
		}
	}
	const Vector3 up(0, 1, 0);
	const int nv = p_bark.verts.size();
	const Vector3 *bv = p_bark.verts.ptr();
	const Vector3 *bn = p_bark.normals.ptr();
	const bool skin = rigged && p_bark.bones.size() >= nv * 4;
	r_fungi.bones.clear();
	auto push_v = [&](const Vector3 &p_pos, const Vector2 &p_uv, const Color &p_col, int p_bv) {
		r_fungi.verts.push_back(p_pos);
		r_fungi.uvs.push_back(p_uv);
		r_fungi.colors.push_back(p_col.srgb_to_linear()); // Looks are sRGB; vertex colour is linear.
		r_fungi.normals.push_back(Vector3());
		if (skin) {
			for (int q = 0; q < 4; q++) {
				r_fungi.bones.push_back(p_bark.bones[p_bv * 4 + q]);
				r_fungi.weights.push_back(p_bark.weights[p_bv * 4 + q]);
			}
		}
	};
	auto quad = [&](int i0, int i1, int i2, int i3) { // i0-i1 one edge, i2-i3 the next, clockwise from outside.
		r_fungi.indices.push_back(i0);
		r_fungi.indices.push_back(i1);
		r_fungi.indices.push_back(i2);
		r_fungi.indices.push_back(i1);
		r_fungi.indices.push_back(i3);
		r_fungi.indices.push_back(i2);
	};
	// One cluster: stalks from a shared foot on the wood, bending up; caps level.
	auto add_cluster = [&](const AgaricLook &p_look, const Vector3 &p_at, const Vector3 &p_normal, float p_width, int p_bv) {
		Vector3 side = p_normal.cross(up);
		if (side.length_squared() < 1e-4f) {
			side = Vector3(1, 0, 0);
		}
		side.normalize();
		const Vector3 lift = side.cross(p_normal).normalized() * (side.cross(p_normal).dot(up) < 0.0f ? -1.0f : 1.0f);
		const int nm = p_look.count0 + int(fr.rand() % uint32_t(p_look.count1 - p_look.count0 + 1));
		const int SIDES = 7, CAP_SEG = 18, CAP_RINGS = 6, GILLS = 22;
		for (int m = 0; m < nm; m++) {
			Vector4i part;
			r_fungi.begin(part);
			// Age: young ones small and domed, old ones wide and flat, some with an upturned rim.
			const float age = fr.randf();
			const float stalk_len = Math::lerp(p_look.stalk0, p_look.stalk1, fr.randf()) * Math::lerp(0.55f, 1.0f, age);
			const float rs = p_look.stalk_r * Math::lerp(0.7f, 1.15f, fr.randf());
			const float rc = Math::lerp(p_look.cap0, p_look.cap1, fr.randf()) * Math::lerp(0.6f, 1.0f, Math::sqrt(age));
			const float cap_h = rc * Math::lerp(0.7f, 0.22f, age);
			// The foot: within the cluster's patch on the wood, a little inside it.
			// Feet close together (the tuft grows from one point of the mycelium), stalks fanning out.
			const float fan_a = (fr.randf() - 0.5f) * 2.4f;
			const Vector3 foot = p_at + side * ((fr.randf() - 0.5f) * p_width * 0.35f) + lift * ((fr.randf() - 0.5f) * p_width * 0.2f) - p_normal * (rs * 1.5f);
			// The stalk leaves the wood outwards and turns up (quadratic Bezier).
			const Vector3 lean = (side * (fr.randf() - 0.5f) * 0.6f + lift * 0.1f).normalized() * 0.0f;
			const Vector3 p1 = foot + (p_normal * 0.8f + side * Math::sin(fan_a) + lift * 0.2f).normalized() * (stalk_len * 0.5f) + lean;
			const Vector3 p2 = p1 + up * (stalk_len * 0.7f) + (p1 - foot) * 0.25f;
			const int RINGS = 6;
			const int sbase = r_fungi.verts.size();
			Vector3 prev_t = (p1 - foot).normalized();
			for (int r = 0; r <= RINGS; r++) {
				const float t = float(r) / RINGS;
				const Vector3 c = foot * ((1 - t) * (1 - t)) + p1 * (2 * (1 - t) * t) + p2 * (t * t);
				Vector3 tg = ((p1 - foot) * (2 * (1 - t)) + (p2 - p1) * (2 * t)).normalized();
				if (tg.length_squared() < 1e-6f) {
					tg = prev_t;
				}
				prev_t = tg;
				Vector3 bx = tg.cross(side);
				if (bx.length_squared() < 1e-6f) {
					bx = tg.cross(up);
				}
				bx.normalize();
				const Vector3 by = tg.cross(bx).normalized();
				const float rr = rs * Math::lerp(1.35f, 1.0f, Math::sqrt(t));
				const Color col = p_look.base.lerp(p_look.stalk, Math::smoothstep(0.0f, 0.5f, t));
				for (int k = 0; k <= SIDES; k++) {
					const float a = float(Math::TAU) * k / SIDES;
					push_v(c + (bx * Math::cos(a) + by * Math::sin(a)) * rr, Vector2(float(k) / SIDES, 0.5f + 0.4f * t), col, p_bv);
				}
			}
			for (int r = 0; r < RINGS; r++) {
				for (int k = 0; k < SIDES; k++) {
					const int i0 = sbase + r * (SIDES + 1) + k;
					quad(i0, i0 + SIDES + 1, i0 + 1, i0 + SIDES + 2);
				}
			}
			// The cap, revolved about the vertical through the stalk's top: a dome that flattens
			// with age, the margin rolled down, a broad bump (umbo) in the middle.
			const Vector3 cap_c = p2 + up * (cap_h * 0.15f);
			const float spin = fr.randf() * float(Math::TAU);
			const float wob = fr.randf() * float(Math::TAU);
			auto cap_point = [&](float p_r, float p_a, float p_dy) {
				const float x = p_r / rc;
				const float y = cap_h * (Math::pow(MAX(1.0f - x * x, 0.0f), 0.7f) + 0.12f * (1.0f - age) * Math::exp(-x * x * 12.0f)) - cap_h * 0.25f * x * x * x * x;
				const float rim = 1.0f + 0.05f * Math::sin(p_a * 5.0f + wob) * x;
				return cap_c + (side * Math::cos(p_a + spin) + side.cross(up) * Math::sin(p_a + spin)) * (p_r * rim) + up * (y + p_dy);
			};
			const int cbase = r_fungi.verts.size();
			for (int j = 0; j <= CAP_RINGS; j++) {
				const float x = float(j) / CAP_RINGS;
				const Color col = p_look.cap_centre.lerp(p_look.cap_mid, Math::smoothstep(0.0f, 0.75f, x)).lerp(p_look.cap_mid * 1.12f, Math::smoothstep(0.85f, 1.0f, x));
				for (int k = 0; k <= CAP_SEG; k++) {
					const float a = float(Math::TAU) * k / CAP_SEG;
					push_v(cap_point(rc * x, a, 0.0f), Vector2(float(k) / CAP_SEG, 0.5f * x), col, p_bv);
				}
			}
			for (int j = 0; j < CAP_RINGS; j++) {
				for (int k = 0; k < CAP_SEG; k++) {
					const int i0 = cbase + j * (CAP_SEG + 1) + k;
					quad(i0, i0 + CAP_SEG + 1, i0 + 1, i0 + CAP_SEG + 2);
				}
			}
			// The underside: from the margin in to the stalk, just under the cap's flesh.
			const float flesh = cap_h * 0.35f + rc * 0.06f;
			const int ubase = r_fungi.verts.size();
			for (int j = 0; j <= 2; j++) {
				const float x = Math::lerp(1.0f, rs / rc, float(j) / 2.0f);
				for (int k = 0; k <= CAP_SEG; k++) {
					const float a = float(Math::TAU) * k / CAP_SEG;
					const Vector3 top = cap_point(rc * x, a, 0.0f);
					push_v(j == 0 ? top : top - up * flesh, Vector2(float(k) / CAP_SEG, 0.25f), p_look.gills * 0.85f, p_bv);
				}
			}
			for (int j = 0; j < 2; j++) {
				for (int k = 0; k < CAP_SEG; k++) {
					const int i0 = ubase + j * (CAP_SEG + 1) + k;
					quad(i0, i0 + CAP_SEG + 1, i0 + 1, i0 + CAP_SEG + 2);
				}
			}
			// Gills: thin blades from the stalk to the margin, deepest a third of the way out.
			const float gt = rc * 0.012f + 0.0003f;
			for (int g = 0; g < GILLS; g++) {
				const float a = float(Math::TAU) * (g + 0.5f) / GILLS;
				const float r_in = (g % 2) ? rc * 0.45f : rs * 1.1f; // Short gills between long ones.
				const Vector3 dir = side * Math::cos(a + spin) + side.cross(up) * Math::sin(a + spin);
				const Vector3 tn = up.cross(dir).normalized();
				const int GS = 4;
				for (int face = 0; face < 2; face++) {
					const float off = face == 0 ? gt : -gt;
					const int gb = r_fungi.verts.size();
					for (int q = 0; q <= GS; q++) {
						const float x = Math::lerp(r_in, rc * 0.9f, float(q) / GS);
						const Vector3 top = cap_point(x, a, -flesh * 0.9f) + tn * off;
						// Never below the cap's margin: from the side the cap hides its gills.
						const float room = MAX(cap_point(x, a, -flesh * 0.9f).dot(up) - cap_point(rc, a, 0.0f).dot(up), 0.0f);
						const float depth = MIN(rc * 0.14f * Math::sin(Math::PI * CLAMP((x - r_in) / (rc - r_in), 0.0f, 1.0f)) * (g % 2 ? 0.7f : 1.0f), room * 0.7f);
						push_v(top, Vector2(0.5f, 0.2f), p_look.gills, p_bv);
						push_v(top - up * depth, Vector2(0.5f, 0.22f), p_look.gills * 0.92f, p_bv);
					}
					for (int q = 0; q < GS; q++) {
						const int i0 = gb + q * 2;
						if (face == 0) {
							quad(i0, i0 + 1, i0 + 2, i0 + 3);
						} else {
							quad(i0, i0 + 2, i0 + 1, i0 + 3);
						}
					}
				}
			}
			r_fungi.end(part, rc);
		}
	};
	for (const Want &wt : want) {
		const bool agaric = wt.kind >= FUNGUS_BRACKETS;
		const FungusLook &lk = FUNGUS_LOOKS[agaric ? 0 : wt.kind];
		const float width = agaric ? Math::lerp(0.08f, 0.16f, fr.randf()) : Math::lerp(lk.w0, lk.w1, fr.randf());
		const float min_r = MAX(width * 0.55f, 0.035f);
		// A node of the main wood thick enough to carry it, at the wanted height, picked by its
		// volume of wood.
		int pick = -1;
		float pick_t = 0.0f;
		{
			LocalVector<int> cand;
			LocalVector<float> acc;
			float total = 0.0f;
			for (int i = 0; i < int(nodes.size()); i++) {
				const Node &c = nodes[i];
				if (c.removed || c.radius < min_r || c.length <= 0.0f) {
					continue;
				}
				if (!lying) {
					const float h0 = MIN(c.start.y, c.end().y) - ground, h1 = MAX(c.start.y, c.end().y) - ground;
					if (wt.foot ? (h1 < 0.15f || h0 > 0.7f) : (h1 < 0.6f || h0 > MAX(height * 0.7f, 1.5f))) {
						continue;
					}
				}
				// Area times radius: fruit bodies come from the thick wood, where the mycelium has the
				// most to rot.
				total += c.radius * c.radius * c.length;
				cand.push_back(i);
				acc.push_back(total);
			}
			for (int tries = 0; tries < 16 && pick < 0 && total > 0.0f; tries++) {
				const float x = fr.randf() * total;
				uint32_t k = 0;
				while (k + 1 < acc.size() && acc[k] < x) {
					k++;
				}
				const Node &c = nodes[cand[k]];
				const float t = fr.randf();
				const float h = c.start.y + c.dir.y * c.length * t - ground;
				if (!lying && (wt.foot ? (h < 0.15f || h > 0.7f) : (h < 0.6f || h > MAX(height * 0.7f, 1.5f)))) {
					continue;
				}
				pick = cand[k];
				pick_t = t;
			}
		}
		if (pick < 0) {
			continue;
		}
		const Node &n = nodes[pick];
		const Vector3 axis_p = n.start + n.dir * (n.length * pick_t);
		// A bearing round the stem; on logs only the flanks (|normal.y| small).
		Vector3 side = n.dir.cross(Math::abs(n.dir.y) < 0.9f ? up : Vector3(1, 0, 0)).normalized();
		Vector3 out;
		for (int tries = 0; tries < 12; tries++) {
			const float a = fr.randf() * float(Math::TAU);
			out = side.rotated(n.dir, a);
			if (Math::abs(out.y) < 0.55f || (agaric && out.y > 0.0f)) {
				break;
			}
		}
		// The bark surface there: the bark vertex nearest the ray out from the axis.
		const float r_guess = n.radius;
		const Vector3 target = axis_p + out * r_guess;
		float best = 1e18f;
		int bi = -1;
		const float reach2 = (r_guess * 1.5f + 0.1f) * (r_guess * 1.5f + 0.1f);
		for (int v = 0; v < nv; v++) {
			const float d2 = bv[v].distance_squared_to(target);
			if (d2 < best && d2 < reach2 && bn[v].dot(out) > 0.5f) {
				best = d2;
				bi = v;
			}
		}
		if (bi < 0) {
			continue;
		}
		// Fruit bodies push up through moss: on mossed wood the cluster stands on the moss shell's
		// surface (built on the bark, up to its hummock height above the tube), not on the bark under
		// it, where a turkey tail or a tuft was buried. The ramp is the shell's own (0.35 to 0.6 of
		// the weight); the bracket's embedding and the stalks' foot still sink into the cushion.
		Vector3 anchor = bv[bi];
		if (moss_k > 0.05f && n.radius >= MOSS_SHELL_MIN_RADIUS) {
			const float mw = CLAMP(0.5f + (_moss_at(bv[bi], bn[bi], ground, moss_k, false) - 0.475f) / 0.25f, 0.0f, 1.0f);
			const float shell = 0.018f * CLAMP(n.radius / 0.15f, 0.35f, 1.0f) * 1.3f;
			anchor += bn[bi] * (shell * mw * mw * (3.0f - 2.0f * mw));
		}
		fungi_points.push_back(anchor.x);
		fungi_points.push_back(anchor.y);
		fungi_points.push_back(anchor.z);
		fungi_points.push_back(bn[bi].x);
		fungi_points.push_back(bn[bi].y);
		fungi_points.push_back(bn[bi].z);
		fungi_points.push_back(width);
		fungi_points.push_back(float(wt.kind));
		if (agaric) {
			add_cluster(AGARIC_LOOKS[wt.kind - FUNGUS_BRACKETS], anchor, bn[bi], width, bi);
			continue;
		}
		Vector3 nout = Vector3(bn[bi].x, 0.0f, bn[bi].z);
		if (nout.length_squared() < 0.05f) {
			fungi_points.resize(fungi_points.size() - 8);
			continue;
		}
		nout.normalize();
		const Vector3 tan = up.cross(nout).normalized();
		// The stem's radius across the bracket: distance of the surface point from the axis.
		const Vector3 rel = anchor - n.start;
		const float surf_r = MAX((rel - n.dir * rel.dot(n.dir)).length(), min_r * 0.8f);
		const int tiers = lk.tier0 + int(fr.rand() % uint32_t(lk.tier1 - lk.tier0 + 1));
		float y_off = 0.0f;
		for (int k = 0; k < tiers; k++) {
			const float w = width * (k == 0 ? 1.0f : Math::lerp(0.55f, 1.0f, fr.randf()));
			const float depth = w * lk.depth;
			const float hgt = depth * lk.height;
			const float mth = hgt * lk.margin;
			const float shift = (k == 0 ? 0.0f : (fr.randf() - 0.5f) * width * 0.6f);
			// The back edge follows the stem's round, sunk a fifth of the depth into the wood.
			const Vector3 o = anchor + up * y_off + tan * shift;
			const float embed = depth * 0.22f;
			const float phase = fr.randf() * float(Math::TAU);
			const int cols = 16, rows_top = 18, rows_rim = 4, rows_bot = 5;
			const int rows = rows_top + rows_rim + rows_bot + 1;
			const int base = r_fungi.verts.size();
			Vector4i part;
			r_fungi.begin(part);
			for (int j = 0; j < rows; j++) {
				for (int c = 0; c <= cols; c++) {
					const float a = -1.0f + 2.0f * c / cols;
					const float reach = Math::pow(MAX(1.0f - a * a, 0.0f), 0.55f) * (1.0f + lk.wave * Math::sin(a * 7.0f + phase));
					const float sag = surf_r - Math::sqrt(MAX(surf_r * surf_r - (a * w * 0.5f) * (a * w * 0.5f), 0.0f));
					float b, y, rim_out = 0.0f;
					float vv;
					if (j <= rows_top) {
						b = float(j) / rows_top;
						float thick = mth + (hgt - mth) * (1.0f - Math::pow(b, lk.dome)) * Math::pow(MAX(1.0f - a * a, 0.0f), 0.35f);
						if (lk.zones > 0) {
							// Each season's growth adds a ridge round the margin (a furrow between).
							const float z = Math::fract(b * lk.zones + 0.15f * Math::sin(a * 3.0f + phase));
							thick += hgt * 0.1f * Math::sin(z * float(Math::PI)) * (1.0f - b * 0.4f);
						}
						y = thick - mth * 0.5f;
						vv = 0.5f * b;
					} else if (j <= rows_top + rows_rim) {
						const float th = float(j - rows_top) / (rows_rim + 1) * float(Math::PI);
						b = 1.0f;
						y = mth * 0.5f * Math::cos(th);
						rim_out = mth * 0.5f * Math::sin(th);
						vv = 0.5f;
					} else {
						b = 1.0f - float(j - rows_top - rows_rim) / rows_bot;
						y = -mth * 0.5f - hgt * 0.06f * (1.0f - b) * b * 4.0f;
						vv = 0.5f + 0.5f * b;
					}
					const float o_out = b * depth * reach + rim_out * reach - embed - sag;
					const Vector3 pos = o + tan * (a * w * 0.5f) + nout * o_out + up * (y + hgt * 0.15f);
					r_fungi.verts.push_back(pos);
					r_fungi.uvs.push_back(Vector2(0.5f + 0.5f * a, vv));
					Color col;
					if (j <= rows_top) {
						col = lk.back.lerp(lk.mid, Math::smoothstep(0.0f, 0.6f, b));
						if (lk.zones > 0) {
							const float z = Math::fract(b * lk.zones + 0.15f * Math::sin(a * 3.0f + phase));
							col = col.lerp(lk.zone, 0.5f * Math::smoothstep(0.55f, 0.85f, z));
						}
						col = col.lerp(lk.rim, Math::smoothstep(0.96f, 1.0f, b) * 0.7f);
					} else if (j <= rows_top + rows_rim) {
						col = lk.mid.lerp(lk.rim, 0.75f);
					} else {
						col = lk.pores.lerp(lk.rim, Math::smoothstep(0.85f, 1.0f, b) * 0.5f);
					}
					r_fungi.colors.push_back(col.srgb_to_linear());
					r_fungi.normals.push_back(Vector3());
					if (skin) {
						for (int q = 0; q < 4; q++) {
							r_fungi.bones.push_back(p_bark.bones[bi * 4 + q]);
							r_fungi.weights.push_back(p_bark.weights[bi * 4 + q]);
						}
					}
				}
			}
			// Front faces wind clockwise seen from outside (Godot's convention).
			for (int j = 0; j < rows - 1; j++) {
				for (int c = 0; c < cols; c++) {
					const int i0 = base + j * (cols + 1) + c, i1 = i0 + 1, i2 = i0 + cols + 1, i3 = i2 + 1;
					r_fungi.indices.push_back(i0);
					r_fungi.indices.push_back(i1);
					r_fungi.indices.push_back(i2);
					r_fungi.indices.push_back(i1);
					r_fungi.indices.push_back(i3);
					r_fungi.indices.push_back(i2);
				}
			}
			r_fungi.end(part, w);
			y_off -= hgt * Math::lerp(0.9f, 1.4f, fr.randf());
		}
	}
	// The fruit bodies end here: the strands below are LOD 0 only.
	fungi_index_end = r_fungi.indices.size();
	// Fruticose lichens on twigs (trees.md 5h): beard lichens (Usnea) hanging in tufts from thin
	// branches, mostly on conifers and old oaks in clean humid air, and oakmoss (Evernia prunastri),
	// forked grey-green straps with a white underside standing off the twigs of oak, ash and hazel.
	// Real strands and straps, placed by the same light and moisture weight as the crust lichens.
	{
		const float k = _lichen_amount();
		const bool usnea_host = conifer || oak || preset == PRESET_BIRCH || preset == PRESET_BLACK_ALDER;
		const bool evernia_host = !conifer;
		const float clumps = k * (fallen || fallen_branch ? 0.0f : 1.0f) * (110.0f * CLAMP(height / 20.0f, 0.25f, 1.0f) * (0.4f + 0.6f * CLAMP(forest_shade / 0.6f, 0.0f, 1.0f)));
		if (clumps > 0.0f && (usnea_host || evernia_host)) {
			LocalVector<int> cand;
			LocalVector<float> acc;
			float total = 0.0f;
			for (int i = 1; i < int(nodes.size()); i++) {
				const Node &c = nodes[i];
				if (c.removed || c.order == 0 || c.radius < 0.004f || c.radius > 0.035f || c.length <= 0.0f || c.start.y - ground < crown_base * 0.7f) {
					continue;
				}
				total += c.length;
				cand.push_back(i);
				acc.push_back(total);
			}
			const int n_clumps = cand.is_empty() ? 0 : int(clumps);
			const Color usnea = Color(0.64f, 0.69f, 0.55f).srgb_to_linear();
			const Color usnea_tip = Color(0.72f, 0.75f, 0.62f).srgb_to_linear();
			const Color evernia_top = Color(0.58f, 0.64f, 0.5f).srgb_to_linear();
			const Color evernia_under = Color(0.82f, 0.84f, 0.78f).srgb_to_linear();
			auto push_raw = [&](const Vector3 &p_pos, const Color &p_col, int p_bone, float p_v) {
				r_fungi.verts.push_back(p_pos);
				r_fungi.uvs.push_back(Vector2(0.5f, p_v));
				r_fungi.colors.push_back(p_col);
				r_fungi.normals.push_back(Vector3());
				if (skin) {
					for (int q = 0; q < 4; q++) {
						r_fungi.bones.push_back(q == 0 ? p_bone : 0);
						r_fungi.weights.push_back(q == 0 ? 1.0f : 0.0f);
					}
				}
			};
			// A strand: a thin tube (3 sides) along a polyline.
			auto strand = [&](const LocalVector<Vector3> &p_pts, float p_r0, float p_r1, int p_bone) {
				const int m = p_pts.size();
				if (m < 2) {
					return;
				}
				const int b0 = r_fungi.verts.size();
				for (int i = 0; i < m; i++) {
					const Vector3 t = (p_pts[MIN(i + 1, m - 1)] - p_pts[MAX(i - 1, 0)]).normalized();
					Vector3 bx = t.cross(Math::abs(t.y) < 0.9f ? up : Vector3(1, 0, 0)).normalized();
					const Vector3 by = t.cross(bx);
					const float f = float(i) / (m - 1);
					const float r = Math::lerp(p_r0, p_r1, f);
					for (int q = 0; q <= 3; q++) {
						const float a = float(Math::TAU) * q / 3;
						push_raw(p_pts[i] + (bx * Math::cos(a) + by * Math::sin(a)) * r, usnea.lerp(usnea_tip, f), p_bone, 0.2f);
					}
				}
				for (int i = 0; i < m - 1; i++) {
					for (int q = 0; q < 3; q++) {
						const int i0 = b0 + i * 4 + q;
						quad(i0, i0 + 4, i0 + 1, i0 + 5);
					}
				}
			};
			for (int c = 0; c < n_clumps; c++) {
				const float x = fr.randf() * total;
				uint32_t ki = 0;
				while (ki + 1 < acc.size() && acc[ki] < x) {
					ki++;
				}
				const int ni = cand[ki];
				const Node &nd = nodes[ni];
				const Vector3 at_axis = nd.start + nd.dir * (nd.length * fr.randf());
				// The crusts' light and patch weight; fruticose lichens take to young twigs, so
				// without the crusts' preference for older bark.
				const float w = _lichen_at(at_axis, 0.02f, 0.0f);
				if (fr.randf() > w) {
					continue;
				}
				const int bone = rigged && ni < int(node_bone.size()) ? node_bone[ni] : 0;
				Vector3 side = nd.dir.cross(up);
				if (side.length_squared() < 1e-4f) {
					side = Vector3(1, 0, 0);
				}
				side.normalize();
				const bool beard = usnea_host && (!evernia_host || fr.randf() < 0.45f);
				Vector4i part;
				r_fungi.begin(part);
				if (beard) {
					// A tuft of 6-14 strands from the twig's underside, hanging and branching.
					const Vector3 base = at_axis - up * (nd.radius * 0.8f);
					const int strands = 6 + int(fr.rand() % 9);
					const float len = Math::lerp(0.05f, 0.16f, fr.randf());
					for (int sidx = 0; sidx < strands; sidx++) {
						LocalVector<Vector3> pts;
						Vector3 p = base + side * ((fr.randf() - 0.5f) * 0.02f) + nd.dir * ((fr.randf() - 0.5f) * 0.03f);
						Vector3 d = (side * (fr.randf() - 0.5f) * 1.6f + nd.dir * (fr.randf() - 0.5f) * 1.0f - up * 0.6f).normalized();
						const float sl = len * Math::lerp(0.5f, 1.0f, fr.randf());
						const int segs = 8;
						pts.push_back(p);
						for (int q = 0; q < segs; q++) {
							d = (d - up * 0.3f + Vector3(fr.randf() - 0.5f, 0.0f, fr.randf() - 0.5f) * 0.25f).normalized();
							p += d * (sl / segs);
							pts.push_back(p);
							if (q == 2 || q == 4) {
								// A side strand.
								LocalVector<Vector3> br;
								Vector3 bp = p, bd = (d + side * (fr.randf() - 0.5f) * 1.5f).normalized();
								br.push_back(bp);
								for (int e = 0; e < 4; e++) {
									bd = (bd - up * 0.35f).normalized();
									bp += bd * (sl * 0.3f / 4);
									br.push_back(bp);
								}
								strand(br, 0.0006f, 0.00035f, bone);
							}
						}
						strand(pts, 0.0009f, 0.0004f, bone);
						// Short fibrils standing out from the main strands (the beard lichens' spinules).
						for (uint32_t q = 1; q + 1 < pts.size(); q++) {
							if (fr.randf() < 0.35f) {
								const Vector3 t = (pts[q + 1] - pts[q - 1]).normalized();
								Vector3 o = t.cross(Vector3(fr.randf() - 0.5f, fr.randf() - 0.5f, fr.randf() - 0.5f));
								if (o.length_squared() < 1e-6f) {
									continue;
								}
								LocalVector<Vector3> fb;
								fb.push_back(pts[q]);
								fb.push_back(pts[q] + (o.normalized() + t * 0.4f).normalized() * Math::lerp(0.003f, 0.007f, fr.randf()));
								strand(fb, 0.0003f, 0.00015f, bone);
							}
						}
					}
				} else {
					// Oakmoss: 5-9 lobes from one holdfast, each forking dichotomously two or three
					// times (Evernia prunastri: soft, flattened, 2-4 mm wide below, 3-8 cm long),
					// standing off the twig and drooping as they lengthen; grey-green above, white
					// beneath.
					struct Lobe {
						Vector3 p, d;
						float len, width;
						int depth;
					};
					LocalVector<Lobe> stack;
					const Vector3 base = at_axis + up * (nd.radius * 0.6f);
					const int lobes = 5 + int(fr.rand() % 5);
					const int max_depth = 2 + int(fr.rand() % 2);
					for (int sidx = 0; sidx < lobes; sidx++) {
						const Vector3 d = (up * 0.7f + side * (fr.randf() - 0.5f) * 1.8f + nd.dir * (fr.randf() - 0.5f) * 1.0f).normalized();
						stack.push_back({ base + nd.dir * ((fr.randf() - 0.5f) * 0.02f), d, Math::lerp(0.009f, 0.018f, fr.randf()), Math::lerp(0.004f, 0.006f, fr.randf()), 0 });
					}
					while (!stack.is_empty()) {
						const Lobe lb = stack[stack.size() - 1];
						stack.resize(stack.size() - 1);
						const int segs = 4;
						const int b0 = r_fungi.verts.size();
						Vector3 fp = lb.p, fd = lb.d;
						const float curl = (fr.randf() - 0.5f) * 0.6f;
						Vector3 nrm_end = up;
						for (int q = 0; q <= segs; q++) {
							const float f = float(q) / segs;
							Vector3 wdir = fd.cross(up);
							if (wdir.length_squared() < 1e-4f) {
								wdir = side;
							}
							wdir.normalize();
							const float half = lb.width * 0.5f * (1.0f - 0.15f * f);
							const Vector3 nrm = wdir.cross(fd).normalized();
							nrm_end = nrm;
							const Vector3 off = nrm * 0.00025f;
							// Upper face then lower face (two sides, each wound to face out).
							push_raw(fp - wdir * half + off, evernia_top, bone, 0.15f);
							push_raw(fp + wdir * half + off, evernia_top, bone, 0.15f);
							push_raw(fp - wdir * half - off, evernia_under, bone, 0.15f);
							push_raw(fp + wdir * half - off, evernia_under, bone, 0.15f);
							fp += fd * (lb.len / segs);
							// The thallus is soft: it droops more the further out it is.
							fd = (fd + nrm * curl * 0.2f - up * (0.06f + 0.05f * lb.depth)).normalized();
						}
						for (int q = 0; q < segs; q++) {
							const int i0 = b0 + q * 4;
							quad(i0, i0 + 4, i0 + 1, i0 + 5);
							quad(i0 + 2, i0 + 3, i0 + 6, i0 + 7);
						}
						if (lb.depth < max_depth) {
							// Two daughter lobes, splayed in the lobe's own plane.
							const float spread = Math::lerp(0.3f, 0.55f, fr.randf());
							for (int c2 = 0; c2 < 2; c2++) {
								const Vector3 cd = fd.rotated(nrm_end, c2 == 0 ? spread : -spread);
								stack.push_back({ fp - fd * 0.0005f, cd, lb.len * Math::lerp(0.75f, 1.05f, fr.randf()), lb.width * 0.8f, lb.depth + 1 });
							}
						}
					}
				}
				r_fungi.end(part, 0.01f);
			}
		}
	}
	// Smooth normals from the faces; the winding is checked against the top facing up.
	Vector3 *nw = r_fungi.normals.ptrw();
	const Vector3 *vw = r_fungi.verts.ptr();
	const int32_t *iw = r_fungi.indices.ptr();
	for (int64_t i = 0; i + 2 < r_fungi.indices.size(); i += 3) {
		const Vector3 fnrm = (vw[iw[i + 2]] - vw[iw[i]]).cross(vw[iw[i + 1]] - vw[iw[i]]);
		nw[iw[i]] += fnrm;
		nw[iw[i + 1]] += fnrm;
		nw[iw[i + 2]] += fnrm;
	}
	for (int v = 0; v < r_fungi.normals.size(); v++) {
		nw[v] = nw[v].length_squared() > 1e-20f ? nw[v].normalized() : up;
	}
}

// Bark lost from dead wood (docs/ideas/trees.md 5f). Bark loosens as the cambium under it rots
// and falls in plates and strips, more each decay class (Maser et al. 1979: class 1 bark intact,
// 2 loosening, 3 trace, 4 and 5 none); a standing snag sheds it from the top down and keeps it
// longest at the foot, a log first on its upper side, where it dries and splits.
float TreeGenerator::_bare_amount() const {
	static const float AMOUNT[6] = { 0.0f, 0.08f, 0.35f, 0.65f, 0.95f, 0.97f };
	return bark_peel * AMOUNT[CLAMP(decay_class, 0, 5)];
}

float TreeGenerator::_bare_at(const Vector3 &p_pos, const Vector3 &p_normal, const Vector3 &p_axis, float p_ground) const {
	const float a = _bare_amount();
	if (a <= 0.0f) {
		return 0.0f;
	}
	const float h = p_pos.y - p_ground;
	const float lean = (fallen || fallen_branch) ? 0.35f * (0.5f + 0.5f * p_normal.y) : 0.35f * Math::smoothstep(0.1f, 2.5f, h);
	// Patches stretched along the stem (bark splits along the grain): the noise is read with the
	// along-stem coordinate compressed fivefold.
	const Vector3 q = p_pos - p_axis * (p_pos.dot(p_axis) * 0.8f);
	const float n = 0.65f * moss_noise(q * 4.0f, 0x7065656cu + uint32_t(seed)) + 0.35f * moss_noise(q * 12.0f, 0x7065656du);
	const float t = 0.65f * n + lean;
	return CLAMP(0.5f + 3.0f * (t - (1.0f - a)), 0.0f, 1.0f);
}

// The bark surface's vertex alpha, written last: 0.5 + 0.5 x lichen weight on bark, 0.5 - 0.5 x
// bare on lost bark (the bark shader decodes both; trees.md 2h and 5f). Bare vertices sink by the
// bark's thickness, so the wood lies below the bark's edge. The sinking is per vertex and
// interpolated, never stepped (stepped walls render black under the path tracer).
void TreeGenerator::_peel_bark(Builder &r_bark) {
	const int nv = r_bark.verts.size();
	if (nv == 0) {
		return;
	}
	const float amount = _bare_amount();
	const float ground = nodes.is_empty() ? 0.0f : nodes[0].start.y;
	const float moss_k = _moss_amount();
	const float floor_k = amount > 0.0f ? _bark_set(_bark_kind(preset)).relief_floor : 0.0f;
	LocalVector<float> radius;
	radius.resize(nv);
	for (int v = 0; v < nv; v++) {
		radius[v] = 0.05f;
	}
	for (uint32_t i = 0; i < r_bark.parts.size(); i++) {
		const Vector4i &pt = r_bark.parts[i];
		for (int v = pt.z; v < pt.z + pt.w && v < nv; v++) {
			radius[v] = r_bark.part_size[i];
		}
	}
	Vector3 *vw = r_bark.verts.ptrw();
	Color *cw = r_bark.colors.ptrw();
	const Vector3 *nr = r_bark.normals.ptr();
	const bool tang = r_bark.use_tangents && r_bark.tangents.size() >= nv * 4;
	LocalVector<uint8_t> skip;
	skip.resize(nv);
	memset(skip.ptr(), 0, nv);
	for (uint32_t v : r_bark.not_bark) {
		if (int(v) < nv) {
			skip[v] = 1;
		}
	}
	const float *tr = r_bark.tangents.ptr();
	for (int v = 0; v < nv; v++) {
		const float lw = CLAMP(1.0f - cw[v].a, 0.0f, 1.0f);
		float bare = 0.0f;
		if (amount > 0.0f && !skip[v]) {
			Vector3 axis(0, 1, 0);
			if (tang) {
				const Vector3 t(tr[v * 4], tr[v * 4 + 1], tr[v * 4 + 2]);
				const Vector3 b = nr[v].cross(t);
				if (b.length_squared() > 1e-8f) {
					axis = b.normalized();
				}
			}
			bare = _bare_at(vw[v], nr[v], axis, ground);
			if (bare > 0.0f && moss_k > 0.0f) {
				// Moss holds the bark under it (and its shell is built on the bark).
				// From where the moss tint starts (0.15): the shell's edge is cut by its grid and only
				// the green-tinted bark under it hides that; bare wood there showed the cut as stairs.
				bare *= 1.0f - Math::smoothstep(0.05f, 0.15f, _moss_at(vw[v], nr[v], ground, moss_k));
			}
		}
		if (bare > 0.0f) {
			const float r = radius[v];
			// Thin bark on smooth tubes (beech, birch, young stems): a few millimetres. A deeper
			// step stood the moss shell, built on the bark, out of the sunk wood as a ledge.
			float depth = MIN(0.25f * r, 0.002f + 0.008f * r);
			if (v < int(r_bark.relief_off.size()) && r_bark.relief_off[v].y > 0.0f) {
				// Displaced bark: the wood lies at the furrow floor (what was bark above it is gone).
				depth = MAX(0.0f, r_bark.relief_off[v].x - 0.9f * floor_k * r_bark.relief_off[v].y) + 0.002f;
			}
			vw[v] -= nr[v] * (depth * Math::smoothstep(0.1f, 0.9f, bare));
			cw[v].a = 0.5f - 0.5f * bare;
		} else {
			cw[v].a = 0.5f + 0.5f * lw;
		}
	}
}

// The lichen layer, LICHEN_TILE metres square, as growth orders: a colony covers the texels whose
// order is under its weight, so a heavier weight grows each colony out from its centre (as in
// Desbenoit et al.'s open DLA growth; here a distance field with ragged edges, which is what that
// growth looks like from a few metres). R: crusts (round colonies, centre 0, edge 1), G: leafy
// rosettes (lobed edges), B: the crust colony's id (picks the orange ones), A: dark marks, the
// black prothallus lines where two crusts meet and the fruiting discs (apothecia).
Ref<Texture2D> TreeGenerator::_lichen_texture() {
	if (lichen_texture.is_valid()) {
		return lichen_texture;
	}
	const int N = 512;
	const float texel = LICHEN_TILE / N;
	struct Colony {
		Vector2 c;
		float r;
		float id;
		int lobes;
		float phase;
		float stretch; // Along v (a colony spreads a little further along the grain).
	};
	RandomPCG rng;
	rng.seed(0x6c696368u);
	LocalVector<Colony> crusts, rosettes;
	for (int i = 0; i < 9; i++) {
		const float r = Math::lerp(0.08f, 0.24f, rng.randf() * rng.randf());
		// Ids under 0.5 can be drawn orange (Xanthoria): only small colonies, as its rosettes are.
		const float id = r < 0.11f ? rng.randf() : 0.5f + 0.5f * rng.randf();
		crusts.push_back({ Vector2(rng.randf(), rng.randf()), r / LICHEN_TILE, id, 0, 0.0f, Math::lerp(1.0f, 1.5f, rng.randf()) });
	}
	for (int i = 0; i < 7; i++) {
		rosettes.push_back({ Vector2(rng.randf(), rng.randf()), Math::lerp(0.035f, 0.08f, rng.randf()) / LICHEN_TILE, rng.randf(), 7 + int(rng.rand() % 6), rng.randf() * float(Math::TAU), 1.0f });
	}
	auto wrap = [](Vector2 d) {
		d.x -= Math::round(d.x);
		d.y -= Math::round(d.y);
		return d;
	};
	Vector<uint8_t> data;
	data.resize(N * N * 4);
	uint8_t *w = data.ptrw();
	Vector<float> order;
	order.resize(N * N);
	float *ow = order.ptrw();
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const Vector2 p((x + 0.5f) / N, (y + 0.5f) / N);
			// Ragged, map-like edges: the position is warped at two scales (a third of a colony and
			// a centimetre), and the order is roughened in proportion to it, so no colony stays a
			// disc but a young one is still one piece rather than a scatter of specks.
			const Vector2 warp(bark_fbm(p.x, p.y, 16, 16, 3, 0x77617270u), bark_fbm(p.x, p.y, 16, 16, 3, 0x77617271u));
			const Vector2 fine(bark_fbm(p.x, p.y, 64, 64, 2, 0x77617272u), bark_fbm(p.x, p.y, 64, 64, 2, 0x77617273u));
			const Vector2 q = p + warp * (0.035f / LICHEN_TILE) + fine * (0.008f / LICHEN_TILE);
			const float rough = 0.4f * bark_fbm(p.x, p.y, 32, 32, 3, 0x726f7567u);
			float o1 = 9.0f, o2 = 9.0f, id = 0.0f;
			for (const Colony &c : crusts) {
				Vector2 d = wrap(q - c.c);
				d.y /= c.stretch;
				const float o = d.length() / c.r * (1.0f + rough);
				if (o < o1) {
					o2 = o1;
					o1 = o;
					id = c.id;
				} else if (o < o2) {
					o2 = o;
				}
			}
			float of = 9.0f;
			for (const Colony &c : rosettes) {
				const Vector2 d = wrap(q - c.c);
				const float ang = Math::atan2(d.y, d.x) + c.phase + 0.6f * bark_fbm(p.x, p.y, 16, 16, 2, 0x6c6f6265u);
				// Lobes radiate with rounded tips and narrow sinuses between them.
				const float lobe = Math::pow(Math::abs(Math::cos(ang * c.lobes * 0.5f)), 0.35f);
				// A rosette's age (its id) scales how soon it shows: at a thin cover only the old ones.
				of = MIN(of, d.length() / (c.r * (0.6f + 0.4f * lobe)) * (1.0f + 0.5f * rough) / Math::lerp(0.15f, 1.5f, c.id * c.id));
			}
			// The thallus is cracked into areolae of a few millimetres (crusts) and wrinkled
			// (rosettes): a fine tone that also darkens the cracks.
			const float areola = 0.5f + bark_fbm(p.x, p.y, 128, 128, 2, 0x6172656fu);
			float mark = 0.8f + 0.25f * areola;
			if (o1 < 1.0f && o2 - o1 < 0.06f) {
				mark = 0.25f; // Black prothallus line between two crusts.
			}
			ow[y * N + x] = o1;
			w[(y * N + x) * 4 + 1] = uint8_t(CLAMP(of, 0.0f, 1.0f) * 255.0f);
			w[(y * N + x) * 4 + 2] = uint8_t(id * 255.0f);
			w[(y * N + x) * 4 + 3] = uint8_t(CLAMP(mark, 0.0f, 1.0f) * 255.0f);
		}
	}
	// The crust order as its rank over the tile, so a weight w covers the share w of it (before
	// the shader's per-colony start): the raw distance kept a thin cover to every colony's core.
	{
		Vector<float> sorted = order;
		sorted.sort();
		const float *sp = sorted.ptr();
		for (int i = 0; i < N * N; i++) {
			int lo = 0, hi = N * N;
			while (lo < hi) {
				const int mid = (lo + hi) / 2;
				if (sp[mid] < ow[i]) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			w[i * 4 + 0] = uint8_t(CLAMP(float(lo) / float(N * N), 0.0f, 1.0f) * 255.0f);
		}
	}
	// Apothecia: discs of a millimetre or two, darker brown-black, mostly in the colonies' older
	// middles.
	for (int i = 0; i < 700; i++) {
		const float cx = rng.randf() * N, cy = rng.randf() * N;
		const float r = (0.4f + 0.8f * rng.randf()) * 0.001f / texel;
		for (int dy = -2; dy <= 2; dy++) {
			for (int dx = -2; dx <= 2; dx++) {
				const int x = (int(cx) + dx + N) % N, y = (int(cy) + dy + N) % N;
				if (Vector2(x + 0.5f - cx, y + 0.5f - cy).length() <= r + 0.5f && w[(y * N + x) * 4] < 90) {
					uint8_t &m = w[(y * N + x) * 4 + 3];
					m = MIN(m, uint8_t(0.45f * 255.0f));
				}
			}
		}
	}
	Ref<Image> img = Image::create_from_data(N, N, false, Image::FORMAT_RGBA8, data);
	img->generate_mipmaps();
	lichen_texture = ImageTexture::create_from_image(img);
	return lichen_texture;
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
	{
		// The noises are far narrower than their nominal range (a tone spread of about 0.06):
		// the turf read as a flat colour. Scanned moss varies by about 40% of its mean across a
		// few centimetres (lit tips against shaded gaps), so the tone is stretched to that.
		double sum = 0.0, sq = 0.0;
		for (int k = 0; k < N * N; k++) {
			sum += tw[k];
			sq += double(tw[k]) * tw[k];
		}
		const float mean = float(sum / (N * N));
		const float sd = float(Math::sqrt(MAX(sq / (N * N) - double(mean) * mean, 1e-8)));
		for (int k = 0; k < N * N; k++) {
			tw[k] = 0.55f + (tw[k] - mean) * (0.22f / sd);
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
	// One side count for the whole stem, and the bark's v carried on from piece to piece: a piece
	// that dropped a side or restarted its texture showed a notch at every joint (each piece is
	// its own tube, for its own bone).
	const int sides = p_r0 > 0.006f ? 5 : (p_r0 > 0.003f ? 4 : 3);
	float v0 = 0.0f;
	tube_carry_nrm = Vector3();
	for (int a = 0; a < n - 1; a += piece) {
		const int b = MIN(a + piece, n - 1);
		LocalVector<Vector3> pts;
		LocalVector<float> rad;
		for (int i = a; i <= b; i++) {
			pts.push_back(p_path[i]);
			rad.push_back(Math::lerp(p_r0, p_r1, float(i) / float(n - 1)));
		}
		r_bark.bone = p_bones[a];
		r_bark.role = Builder::ROLE_ALL;
		Vector4i part;
		r_bark.begin(part);
		_tube(r_bark, pts, rad, sides, v0, p_color);
		tube_carry_nrm = tube_last_nrm;
		r_bark.end(part, rad[0]);
		const float circ = Math::TAU * MAX(rad[0], 0.001f) / MAX(1.0f, Math::round(Math::TAU * MAX(rad[0], 0.001f) / BARK_TILE));
		for (int i = 1; i < int(pts.size()); i++) {
			v0 += pts[i].distance_to(pts[i - 1]) / circ;
		}
	}
	tube_carry_nrm = Vector3();
}

// Liana stands for clematis (old man's beard), the hanging climber of rich lowland woods.
Array TreeGenerator::plant_climber_cache[CLIMBER_MAX + 1];
LocalVector<TreeGenerator::ClimberLeafShape> TreeGenerator::plant_climber_shapes[CLIMBER_MAX + 1];

Array TreeGenerator::_plant_climber_leaves(int p_climber) {
	Array *cache = plant_climber_cache;
	if (p_climber <= CLIMBER_NONE || p_climber > CLIMBER_MAX) {
		return Array();
	}
	if (cache[p_climber].is_empty()) {
		const int kind = p_climber == CLIMBER_MAX ? PlantGenerator::CLIMBER_IVY_ADULT : (p_climber == CLIMBER_IVY ? PlantGenerator::CLIMBER_IVY : (p_climber == CLIMBER_LIANA ? PlantGenerator::CLIMBER_CLEMATIS : PlantGenerator::CLIMBER_HONEYSUCKLE));
		cache[p_climber] = PlantGenerator::make_climber_leaves(kind, 4);
		LocalVector<ClimberLeafShape> &shapes = plant_climber_shapes[p_climber];
		shapes.clear();
		for (int i = 0; i < cache[p_climber].size(); i++) {
			const Ref<ArrayMesh> m = cache[p_climber][i];
			const Array a = m->surface_get_arrays(0);
			ClimberLeafShape sh;
			sh.verts = a[Mesh::ARRAY_VERTEX];
			sh.normals = a[Mesh::ARRAY_NORMAL];
			sh.uvs = a[Mesh::ARRAY_TEX_UV];
			sh.indices = a[Mesh::ARRAY_INDEX];
			shapes.push_back(sh);
		}
	}
	return cache[p_climber];
}

void TreeGenerator::_climber_leaf(Builder &r_leaves, const Vector3 &p_at, const Vector3 &p_out, const Vector3 &p_normal, float p_size, int p_bone, const Color &p_color) {
	// A climber's leaf is about as broad as long (ivy, clematis leaflets, honeysuckle), whatever
	// the host's leaf width.
	const float saved_width = params[PARAM_LEAF_WIDTH];
	params[PARAM_LEAF_WIDTH] = 0.42f;
	const uint32_t h = hash_murmur3_one_32(uint32_t(climber_leaves.size() / CLIMBER_LEAF_FLOATS), uint32_t(seed) * 7919u + 13u);
	// Old ivy flowers on adult shoots where it reaches the light: unlobed, ovate leaves on the
	// upper third of old stems (Metcalfe 2005), the change spread over a band of the stem.
	const bool adult = climber == CLIMBER_IVY && climber_stem_age > 0.35f &&
			climber_stem_up > 0.6f + 0.15f * float((h >> 16) & 0xff) / 255.0f - 0.1f * climber_stem_age;
	const int slot = adult ? int(CLIMBER_MAX) : int(climber);
	const Array plant = climber_plant_leaves ? _plant_climber_leaves(slot) : Array();
	if (!plant.is_empty()) {
		// The ground plants' leaf for this climber (lies in XZ, base at the origin, +Z, 1 m),
		// one of its variants, turned onto our blade frame and scaled to the leaf's length.
		const ClimberLeafShape &shape = plant_climber_shapes[slot][h % plant_climber_shapes[slot].size()];
		const PackedVector3Array &mv = shape.verts;
		const PackedVector3Array &mn = shape.normals;
		const PackedVector2Array &mu = shape.uvs;
		const PackedInt32Array &mi = shape.indices;
		Vector3 z = p_out - p_normal * p_out.dot(p_normal);
		z = z.length_squared() > 1e-8f ? z.normalized() : p_normal.cross(Vector3(1, 0, 0)).normalized();
		const Vector3 y = p_normal.normalized();
		const Basis b = Basis(y.cross(z), y, z) * p_size; // Columns: X, Y (blade normal), Z (along).
		Builder &cb = adult ? climber_adult_b : climber_leaf_b;
		cb.bone = p_bone;
		Vector4i part;
		cb.begin(part);
		const int v0 = cb.verts.size();
		const float tone = 0.85f + 0.3f * float((h >> 8) & 0xff) / 255.0f;
		for (int i = 0; i < mv.size(); i++) {
			cb.verts.push_back(p_at + b.xform(mv[i]));
			cb.normals.push_back((i < mn.size() ? Basis(y.cross(z), y, z).xform(mn[i]) : y).normalized());
			cb.uvs.push_back(i < mu.size() ? mu[i] : Vector2());
			cb.colors.push_back(Color(tone, tone, tone));
		}
		for (int i = 0; i < mi.size(); i++) {
			cb.indices.push_back(v0 + mi[i]);
		}
		cb.end(part, 0.0f);
	} else {
		r_leaves.bone = p_bone;
		Vector4i part;
		r_leaves.begin(part);
		species_leaf_forms = false;
		_add_leaf(r_leaves, p_at, p_out, p_normal, p_size, p_color);
		species_leaf_forms = true;
		r_leaves.end(part, 0.0f);
	}
	params[PARAM_LEAF_WIDTH] = saved_width;
	leaf_count++;
	// Published for vegetation's climber leaf meshes: position, blade normal, petiole direction,
	// size, light, the stem's age (0 young, 1 old) and how far up the stem the leaf is (0 foot, 1 tip):
	// old ivy grows adult shoots with unlobed leaves where it reaches the light high up.
	const float vals[CLIMBER_LEAF_FLOATS] = { p_at.x, p_at.y, p_at.z, p_normal.x, p_normal.y, p_normal.z, p_out.x, p_out.y, p_out.z, p_size, 0.5f, climber_stem_age, climber_stem_up };
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
		// Old ivy (Metcalfe 2005, Biological Flora of the British Isles: Hedera helix): stems
		// thicken for decades, to several centimetres on old trunks, grey-brown and fissured,
		// pressed to the bark by their aerial rootlets. The ivy's age follows the host's (a tall
		// or veteran tree has carried it longest). The rootlets themselves (3-9 mm hairs) were
		// built and dropped: under a pixel at a metre, at half a million triangles a tree.
		const float ivy_old = CLAMP(Math::lerp(CLAMP((height - 6.0f) / 22.0f, 0.0f, 1.0f), 1.0f, veteran) * climber_cover, 0.0f, 1.0f);
		struct Shoot {
			int k;
			float s, theta, phi, length, r0;
			int depth;
			float age;
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
				const float age = ivy_old * Math::lerp(0.55f, 1.0f, cr.randf());
				todo.push_back({ int(k), 0.12f + 0.1f * cr.randf(), side + Math::lerp(-1.7f, 1.7f, cr.randf()), 0.0f, reach * Math::lerp(0.55f, 1.0f, cr.randf()), Math::lerp(0.006f, 0.011f, cr.randf()) * (1.0f + 3.5f * age), 0, age });
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
			const float s0 = sh.s;
			const float s_end = MIN(sh.s + sh.length, frames[sh.k].s[frames[sh.k].s.size() - 1] - 0.05f);
			int leaf_side = 1;
			climber_stem_age = sh.age;
			const float r_tip = 0.0025f;
			for (int st = 0; sh.s < s_end && st < 4000; st++) {
				// The stem lies on the bark: its axis a little less than its radius out (pressed
				// into the bark's furrows), not floating at a fixed lift.
				const float f_up = CLAMP((sh.s - s0) / MAX(s_end - s0, 1e-4f), 0.0f, 1.0f);
				const float r_here = Math::lerp(sh.r0, r_tip, f_up);
				// Over the bark's relief where LOD 0 displaces it (its ridges stand up to about half
				// the furrow depth, 4.5 cm times the relief scale, out of the plain tube).
				const float host_r = surf(sh.k, sh.s, sh.theta, 0.0f).r;
				const float ridge = (bark_relief > 0.0f && host_r >= RELIEF_MIN_RADIUS) ? 0.0225f * bark_relief * CLAMP(host_r / 0.35f, 0.25f, 1.0f) : 0.0f;
				const Surf p = surf(sh.k, sh.s, sh.theta, 0.002f + 0.8f * r_here + ridge);
				climber_stem_up = f_up;
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
					todo.push_back({ sh.k, sh.s, sh.theta, sh.phi + (cr.randf() < 0.5f ? 1.0f : -1.0f), (s_end - sh.s) * Math::lerp(0.3f, 0.7f, cr.randf()), MAX(Math::lerp(sh.r0, r_tip, CLAMP((sh.s - s0) / MAX(s_end - s0, 1e-4f), 0.0f, 1.0f)) * 0.6f, 0.003f), sh.depth + 1, sh.age * 0.6f });
				}
				sh.s += step * Math::cos(sh.phi);
				sh.theta += step * Math::sin(sh.phi) / MAX(p.r + 0.006f, 0.02f);
				sh.phi += 0.4f * gauss();
				sh.phi *= 0.95f;
				sh.phi = CLAMP(sh.phi, -1.3f, 1.3f);
			}
			// Young stems brown, old ones grey-brown and fissured (the bark texture carries it).
			_climber_stem(r_bark, path, bones, sh.r0, r_tip, Color(0.2f, 0.17f, 0.13f).lerp(Color(0.3f, 0.27f, 0.23f), sh.age));
		}
		climber_stem_age = 0.0f;
		climber_stem_up = 0.0f;
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
// A pinnate leaflet (rowan, ash, elder): lanceolate with a sharply toothed margin, cut by alpha
// from a blade a quarter wider than the leaflet (trees.md 5n). Teeth as geometry cost 64M
// triangles on one near rowan; as alpha they cost nothing but the cut. The blade's UVs run 0-1
// across at every row and its base and tip rows are single vertices, so with p_shape 1 the
// lateral offset is d x 0.866 x 1.25 of the leaflet's half-width everywhere (d = |2u - 1|) and
// the outline is set in that measure. Serrate teeth point to the apex: along the margin from
// the base each rises slowly to its tip and drops back sharply (Ellis et al., Manual of Leaf
// Architecture, 2009). RGB is the venation factor (as _leaf_texture), A the cut.
Ref<Texture2D> TreeGenerator::_leaflet_texture() {
	if (leaflet_texture.is_valid()) {
		return leaflet_texture;
	}
	const int w = 128, h = 512;
	const float lat = 0.866f * LEAFLET_HULL; // Lateral offset per unit d, in leaflet half-widths.
	Vector<uint8_t> px;
	px.resize(w * h * 4);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			const float d = Math::abs(u - 0.5f) * 2.0f;
			const float xl = d * lat; // Offset from the midrib in half-widths.
			// Lanceolate: widest a little below the middle, an acuminate tip.
			const float half = Math::sin(float(Math::PI) * Math::pow(v, 0.85f)) * (1.0f - 0.25f * Math::smoothstep(0.6f, 1.0f, v));
			// Teeth on the upper three quarters, about 22 a side, a tenth of the half-width deep.
			const float s = v * 22.0f - Math::floor(v * 22.0f);
			const float teeth = Math::smoothstep(0.12f, 0.3f, v) * (1.0f - Math::smoothstep(0.93f, 1.0f, v));
			const float edge = half * (1.0f - 0.11f * teeth * (1.0f - Math::pow(s, 0.6f)));
			// Antialiased over a texel across.
			const float aa = lat * 2.0f / w;
			const float a = CLAMP((edge - xl) / aa + 0.5f, 0.0f, 1.0f);
			float f = 0.74f + 0.06f * (bark_fbm(u, v, 8, 32, 3, 709u) * 2.0f);
			// Midrib, pale and raised, thinning to the tip.
			const float mid_w = Math::lerp(0.07f, 0.02f, v);
			f = Math::lerp(f, 1.0f, CLAMP(1.6f - 1.2f * xl / mid_w, 0.0f, 1.0f));
			// Secondaries: about 14 a side, curving forward and looping short of the margin.
			const float rel = edge > 0.0f ? xl / edge : 1.0f;
			const float phase = (v - 0.22f * rel) * 14.0f;
			const float line = Math::abs(phase - Math::round(phase));
			const float sec = CLAMP(1.4f - line / 0.07f, 0.0f, 1.0f) * (1.0f - Math::smoothstep(0.75f, 0.92f, rel)) * (xl > mid_w ? 1.0f : 0.0f);
			f = Math::lerp(f, 0.94f, sec * 0.85f);
			// Between the veins the lamina puckers slightly darker; the margin band too.
			f *= 1.0f - 0.05f * (1.0f - sec) * Math::smoothstep(0.2f, 0.6f, rel);
			f *= 1.0f - 0.1f * Math::smoothstep(0.88f, 1.0f, rel);
			const uint8_t b = uint8_t(Math::round(CLAMP(Color(f, f, f).linear_to_srgb().r, 0.0f, 1.0f) * 255.0f));
			uint8_t *o = px.ptrw() + (y * w + x) * 4;
			o[0] = o[1] = o[2] = b;
			o[3] = uint8_t(Math::round(a * 255.0f));
		}
	}
	Ref<Image> img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, px);
	img->generate_mipmaps();
	leaflet_texture = ImageTexture::create_from_image(img);
	return leaflet_texture;
}

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
			float f = 0.82f + 0.07f * (bark_fbm(u, v, 8, 16, 3, 701u) * 2.0f);
			// Midrib, thinning toward the tip.
			const float mid_w = Math::lerp(0.035f, 0.01f, v);
			f = Math::lerp(f, 1.0f, CLAMP(1.0f - d / mid_w, 0.0f, 1.0f));
			// Secondaries: lines of constant v - 0.35 d, eight to a blade, fading at the margin.
			const float phase = (v - 0.35f * d) * 8.0f;
			const float line = Math::abs(phase - Math::round(phase));
			const float sec = CLAMP(1.0f - line / 0.06f, 0.0f, 1.0f) * (1.0f - Math::smoothstep(0.7f, 0.95f, d)) * (d > mid_w ? 1.0f : 0.0f);
			f = Math::lerp(f, 0.98f, sec * 0.85f);
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
	// With bark_peel the lost bark shows as wood (trees.md 5f), so what bark is left keeps more of
	// its own colour.
	bark_color = bark_color.lerp(WEATHERED[decay_class], decay_class >= 3 && bark_peel <= 0.0f ? 0.85f : 0.55f);
}

// Old trees (docs/ideas/trees.md 5e): the tree-related microhabitats that come with age and girth
// (Larrieu et al., Ecological Indicators 84, 2018, the TreM typology; Vuidot et al. 2011 and
// Larrieu and Cabanettes 2012: their number rises steeply with diameter). A crown broken by
// storm or snow, a basal hollow where heart rot reached the root collar, rot holes and woodpecker
// holes higher up, burls, and epicormic shoots on the stem (oak and lime above all). `veteran`
// 0-1 sets how many; the stem keeps its shape and growth, the features are cut into its mesh.
// Storm-broken limbs on living trees (docs/ideas/trees.md 5j). A gust loads a limb past the
// strain its wood takes: it snaps partway out, the fibres on the stretched (upper) side
// tearing into splinters and the compressed side buckling (Li et al., SIGGRAPH 2025, fracture
// by strain thresholds along the fibre). Old and tall trees carry more of them, veterans most
// (TreMs: broken limbs and hangers are among the commonest microhabitats, Larrieu et al. 2018).
// About a third of the snapped limbs stay hung in the crown by a strip of bark and fibre
// ("hangers", the arborist's term): they swing down beside the break and die there.
void TreeGenerator::_break_limbs() {
	if (limb_breakage <= 0.0f || nodes.size() < 8) {
		return;
	}
	RandomPCG br;
	br.seed(uint64_t(seed) * 0x9e3779b1u + 0x5b5b5bu);
	const float base_y = nodes[0].start.y;
	const float mean = limb_breakage * (0.5f * CLAMP((height - 8.0f) / 20.0f, 0.0f, 1.0f) + veteran);
	if (mean < 0.01f) {
		return;
	}
	int count = 0;
	{
		// Poisson: storms break limbs one at a time and independently.
		const float lim = Math::exp(-mean);
		float p = br.randf();
		while (p > lim && count < 12) {
			count++;
			p *= br.randf();
		}
	}
	if (count == 0) {
		return;
	}
	// Limbs from forearm to thigh thick, high enough not to be the lowest (shaded-out) ones and
	// long enough to carry a break and something beyond it.
	LocalVector<int> cands;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.removed || n.dead || n.parent <= 0 || nodes[n.parent].main == i || n.radius < 0.05f || n.radius > 0.22f ||
				n.start.y - base_y < 2.5f) {
			continue;
		}
		float arc = 0.0f;
		for (int k = i; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
			arc += nodes[k].length;
		}
		if (arc >= 1.5f) {
			cands.push_back(i);
		}
	}
	LocalVector<int> chain;
	LocalVector<uint8_t> in_sub;
	for (int b = 0; b < count && !cands.is_empty(); b++) {
		// The wind's load on a limb grows with its sail and lever, so thick limbs go first
		// (picked by cross-section), and spreading ones, whose own weight bends them too, before
		// steep ones.
		auto load = [&](int p_i) { return nodes[p_i].radius * nodes[p_i].radius * (1.25f - MAX(nodes[p_i].dir.y, 0.0f)); };
		float total = 0.0f;
		for (const int c : cands) {
			total += load(c);
		}
		float u = br.randf() * total;
		uint32_t pick = 0;
		while (pick + 1 < cands.size() && u > load(cands[pick])) {
			u -= load(cands[pick]);
			pick++;
		}
		const int a = cands[pick];
		cands[pick] = cands[cands.size() - 1];
		cands.resize(cands.size() - 1);
		if (nodes[a].removed) {
			continue; // Taken away with an earlier break.
		}
		bool gone = false;
		for (int p = nodes[a].parent; p > 0; p = nodes[p].parent) {
			gone = gone || nodes[p].removed || hanger_bases.has(p);
		}
		if (gone) {
			continue;
		}
		chain.clear();
		float arc = 0.0f;
		for (int k = a; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
			chain.push_back(k);
			arc += nodes[k].length;
		}
		const float target = arc * Math::lerp(0.25f, 0.65f, br.randf());
		int k = -1;
		float run = 0.0f;
		for (int c = 0; c + 1 < int(chain.size()); c++) {
			run += nodes[chain[c]].length;
			if (run >= target) {
				k = chain[c];
				break;
			}
		}
		if (k < 0 || nodes[k].radius < 0.025f) {
			continue;
		}
		const int next = nodes[k].main;
		for (int l = nodes[k].first_lateral; l >= 0; l = nodes[l].next_sibling) {
			nodes[l].removed = true;
		}
		nodes[k].first_lateral = -1;
		nodes[k].main = -1;
		nodes[k].leafy = false;
		broken_ends.insert(k);
		// Hung in the crown: the broken part turns down about the break, beside the stub.
		bool hung = false;
		// Only a spreading limb leaves its broken part hanging clear; a steep one folds it back
		// down its own length.
		if (br.randf() < 0.45f && nodes[next].radius >= 0.02f && nodes[next].dir.y < 0.75f) {
			in_sub.resize(nodes.size());
			memset(in_sub.ptr(), 0, in_sub.size());
			in_sub[next] = 1;
			for (int i = next + 1; i < int(nodes.size()); i++) {
				in_sub[i] = (!nodes[i].removed && nodes[i].parent >= 0 && in_sub[nodes[i].parent]) ? 1 : 0;
			}
			const Vector3 pivot = nodes[k].end();
			const Vector3 d = nodes[next].dir;
			Vector3 hd = Vector3(d.x, 0.0f, d.z);
			hd = hd.length() > 0.05f ? hd.normalized() : Vector3(Math::cos(br.randf() * Math::TAU), 0.0f, Math::sin(br.randf() * Math::TAU));
			const Vector3 h = (Vector3(0, -1, 0) + hd * Math::lerp(0.15f, 0.55f, br.randf())).normalized();
			Vector3 axis = d.cross(h);
			if (axis.length() > 0.05f) {
				axis.normalize();
				const float ang = d.angle_to(h);
				const float r = nodes[k].radius;
				const Vector3 shift = axis * (r * 1.1f) + h * (r * 0.4f);
				float low = 1e9f;
				for (int i = next; i < int(nodes.size()); i++) {
					if (in_sub[i]) {
						low = MIN(low, (pivot + (nodes[i].end() - pivot).rotated(axis, ang) + shift).y);
					}
				}
				if (low - base_y > 0.4f) {
					for (int i = next; i < int(nodes.size()); i++) {
						if (!in_sub[i]) {
							continue;
						}
						Node &n = nodes[i];
						n.start = pivot + (n.start - pivot).rotated(axis, ang) + shift;
						n.dir = n.dir.rotated(axis, ang).normalized();
						n.dead = true;
						n.leafy = false;
					}
					nodes[next].next_sibling = -1;
					nodes[k].first_lateral = next;
					hanger_bases.insert(next);
					hung = true;
				}
			}
		}
		if (!hung) {
			nodes[next].removed = true; // Down on the ground below.
		}
	}
	for (int i = 1; i < int(nodes.size()); i++) {
		if (!nodes[i].removed && nodes[i].parent > 0 && nodes[nodes[i].parent].removed) {
			nodes[i].removed = true;
		}
	}
	for (const int hb : hanger_bases) {
	}
}

void TreeGenerator::_apply_veteran() {
	RandomPCG vr;
	vr.seed(uint64_t(seed) * 2654435761u + 0x7e7e7eu);
	const float base_y = nodes[0].start.y;
	// The main stem: the thickest axis from the root.
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (n.parent == 0 && !n.removed && (veteran_stem < 0 || n.radius > nodes[veteran_stem].radius)) {
			veteran_stem = i;
		}
	}
	if (veteran_stem < 0 || nodes[veteran_stem].radius < 0.08f) {
		veteran_stem = -1;
		return; // Too thin to carry any (TreMs are rare under 40 cm diameter).
	}
	const float r_stem = nodes[veteran_stem].radius;
	const bool conifer = params[PARAM_NEEDLES] > 0.5f;
	// Crown breakage: the top snapped at 55-85% of the height, the stem's leader gone above it.
	if (vr.randf() < 0.45f * veteran && !conifer) {
		const float cut_y = height * Math::lerp(0.6f, 0.85f, vr.randf());
		for (int i = veteran_stem; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
			Node &n = nodes[i];
			if (n.start.y - base_y >= cut_y) {
				n.removed = true;
			} else if (n.end().y - base_y > cut_y && n.dir.y > 0.05f) {
				n.length = MAX((cut_y - (n.start.y - base_y)) / n.dir.y, 0.02f);
				const int next = n.main;
				n.main = -1;
				broken_tip = i;
				if (next >= 0) {
					nodes[next].removed = true;
				}
				break;
			}
		}
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
	}
	// Features sit on the clear bole, below the crown base and the first big fork: above that the
	// stem is a tangle of limbs and a hollow or burl would hide in it (or open into a limb).
	float bole = crown_base;
	{
		LocalVector<bool> on_stem;
		on_stem.resize(nodes.size());
		for (uint32_t i = 0; i < nodes.size(); i++) {
			on_stem[i] = false;
		}
		for (int i = veteran_stem; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
			on_stem[i] = true;
		}
		for (uint32_t i = 1; i < nodes.size(); i++) {
			const Node &n = nodes[i];
			if (!n.removed && n.parent >= 0 && on_stem[n.parent] && !on_stem[i] && n.radius > 0.45f * r_stem) {
				bole = MIN(bole, n.start.y - base_y);
			}
		}
	}
	const float crown = MAX(bole - 0.3f, 1.5f);
	auto add = [&](VeteranKind p_kind, float p_h, float p_half_h, float p_half_w, float p_amount) {
		// Features that change the stem's surface keep apart: a burl swelling out of a hollow's
		// dark mouth reads as a dark lump. A few bearings are tried, else the feature is dropped.
		const bool surface = p_kind != VET_EPICORMIC;
		for (int attempt = 0; attempt < 6; attempt++) {
			const float az = vr.randf() * float(Math::TAU);
			bool clear = true;
			if (surface) {
				for (const VeteranFeature &o : veteran_features) {
					if (o.kind == VET_EPICORMIC) {
						continue;
					}
					const float dh = Math::abs(p_h - o.h) / (p_half_h + o.half_h + 0.05f);
					const float da = Math::abs(Math::angle_difference(az, o.azimuth)) * r_stem / (p_half_w + o.half_w + 0.05f);
					if (dh * dh + da * da < 1.4f * 1.4f) {
						clear = false;
						break;
					}
				}
			}
			if (clear) {
				veteran_features.push_back({ p_kind, p_h, az, p_half_h, p_half_w, p_amount });
				return;
			}
		}
	};
	const float big = CLAMP((r_stem - 0.08f) / 0.4f, 0.2f, 1.0f); // Girth: more and larger features.
	// A basal hollow, opening at the root collar.
	if (vr.randf() < 0.55f * veteran * big) {
		const float hh = r_stem * Math::lerp(0.9f, 2.2f, vr.randf());
		add(VET_BASAL_HOLLOW, hh * 0.85f, hh, r_stem * Math::lerp(0.5f, 0.9f, vr.randf()), Math::lerp(0.5f, 0.7f, vr.randf()));
	}
	// Rot holes where a limb broke off long ago, on the stem below the crown.
	const int rot = int(Math::floor(veteran * big * Math::lerp(0.5f, 2.5f, vr.randf())));
	for (int k = 0; k < rot; k++) {
		const float hh = Math::lerp(0.1f, 0.25f, vr.randf());
		// Clear of the crotch: one cut into the first fork read as a missing polygon.
		add(VET_ROT_HOLE, Math::lerp(1.2f, MAX(1.2f, crown - hh - 0.4f), vr.randf()), hh, hh * Math::lerp(0.5f, 0.8f, vr.randf()), Math::lerp(0.45f, 0.6f, vr.randf()));
	}
	// Woodpecker holes, round, 4-6 cm across (great spotted; black woodpecker 9-12 cm, oval).
	const int pecks = int(Math::floor(veteran * Math::lerp(0.3f, 3.0f, vr.randf())));
	for (int k = 0; k < pecks; k++) {
		const bool black = vr.randf() < 0.25f;
		const float rh = black ? 0.06f : 0.025f;
		add(VET_WOODPECKER, Math::lerp(2.5f, MAX(crown * 1.2f, 3.0f), vr.randf()), rh, black ? rh * 0.75f : rh, 0.75f);
	}
	// Burls: rounded swellings of tangled wood.
	const int burls = int(Math::floor(veteran * big * Math::lerp(0.5f, 3.5f, vr.randf())));
	for (int k = 0; k < burls; k++) {
		const float hh = r_stem * Math::lerp(0.3f, 0.7f, vr.randf());
		// Clear of the ground: a burl reaching into the root flare ballooned out over the soil.
		const float lo = MIN(MAX(0.4f, hh + 0.25f), crown);
		add(VET_BURL, Math::lerp(lo, MAX(crown, lo), vr.randf()), hh, hh * Math::lerp(0.8f, 1.3f, vr.randf()), Math::lerp(0.3f, 0.55f, vr.randf()));
	}
	// Epicormic shoots: clusters from dormant buds on the stem, many on oak and lime.
	if (!conifer) {
		const bool prone = preset == PRESET_OAK || preset == PRESET_LIME || preset == PRESET_WHITE_OAK || preset == PRESET_RED_OAK;
		const int clusters = int(Math::floor(veteran * (prone ? Math::lerp(3.0f, 8.0f, vr.randf()) : Math::lerp(0.0f, 2.5f, vr.randf()))));
		for (int k = 0; k < clusters; k++) {
			add(VET_EPICORMIC, Math::lerp(0.6f, crown, vr.randf()), 0.0f, 0.0f, Math::lerp(3.0f, 8.0f, vr.randf()));
		}
	}
}

// The old-tree features of the last generate(): kind (0 basal hollow, 1 rot hole, 2 woodpecker hole,
// 3 burl, 4 epicormic shoots), height above the stem base, azimuth (radians from +X towards +Z),
// half height and half width in metres. Cavities are where birds, bats and beetles live.
Array TreeGenerator::get_veteran_features() const {
	Array out;
	for (const VeteranFeature &f : veteran_features) {
		Dictionary d;
		d["kind"] = int(f.kind);
		d["height"] = f.h;
		d["azimuth"] = f.azimuth;
		d["half_height"] = f.half_h;
		d["half_width"] = f.half_w;
		out.push_back(d);
	}
	return out;
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
	plate.on = uprooted && decay_class <= 3;
	if (plate.on) {
		// Thrown by the roots (trees.md 5k): the root plate hinges up about its edge on the side
		// the tree falls to, and the stem turns with it, its butt held up by the plate, until
		// the top comes down on the ground. Plate radius about 5.5-6.5 times the stem's
		// diameter, 0.3-0.5 m thick (Krisans et al. 2020); smaller and slumped once rotting.
		const float rot_k = decay_class >= 3 ? 0.8f : 1.0f;
		plate.trunk_r = trunk_r;
		plate.radius = CLAMP(11.0f * trunk_r * Math::lerp(0.9f, 1.2f, fr.randf()), 0.9f, 2.8f) * rot_k;
		plate.thick = CLAMP(0.22f * plate.radius, 0.25f, 0.6f) * rot_k;
		Vector3 fall = turn.xform(Vector3(0, 1, 0));
		fall.y = 0.0f;
		fall.normalize();
		plate.base0 = base;
		plate.hinge = base + fall * (0.75f * plate.radius) - Vector3(0, 0.1f, 0);
		// The stem's top: its furthest point from the base.
		Vector3 tip = base;
		float tip_r = trunk_r;
		for (const Node &n : nodes) {
			if (!n.removed && n.order == 0 && n.end().distance_squared_to(base) > tip.distance_squared_to(base)) {
				tip = n.end();
				tip_r = n.radius;
			}
		}
		float ang = float(Math::PI) * 0.5f;
		for (int it = 0; it < 4; it++) {
			const Basis r(axis, ang);
			const Vector3 b2 = plate.hinge + r.xform(base - plate.hinge);
			const Vector3 t2 = plate.hinge + r.xform(tip - plate.hinge);
			const float d = MAX(Vector2(t2.x - b2.x, t2.z - b2.z).length(), 1.0f);
			ang += Math::atan((t2.y - (base.y + tip_r + 0.05f)) / d);
		}
		plate.rot = Basis(axis, ang);
		for (Node &n : nodes) {
			n.start = plate.hinge + plate.rot.xform(n.start - plate.hinge);
			n.dir = plate.rot.xform(n.dir);
		}
	} else {
		// Bedding (trees.md 5d): a fresh log rests on the litter, a decayed one has settled into
		// the soil and moss (Harmon et al. 1986), sunk 15% of its radius at class 1-2 up to 45%
		// at 5.
		static const float SINK[6] = { 0.15f, 0.15f, 0.15f, 0.25f, 0.35f, 0.45f };
		const float lift = trunk_r * (1.0f - SINK[CLAMP(decay_class, 0, 5)]);
		for (Node &n : nodes) {
			n.start = base + turn.xform(n.start - base) + Vector3(0, lift, 0);
			n.dir = turn.xform(n.dir);
		}
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
	wood_b = Builder();
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
	// Lichen (trees.md 2h): its weight goes in the vertex colour's alpha as 1 - weight (bark
	// built elsewhere keeps 1, no lichen), and the part gets its lichen UV2 mapping: a whole
	// number of LICHEN_TILE repeats round the stem (no seam) and an offset per stem, so the layer
	// neither repeats with the bark's tile nor lines up between stems.
	const float lichen_k = _lichen_amount();
	auto lichen_tint = [&](Builder &r_b, const Vector4i &p_part, float p_radius, float p_girth, uint32_t p_salt) {
		if (lichen_k <= 0.0f || r_b.parts.is_empty() || r_b.parts[r_b.parts.size() - 1] != p_part) {
			return;
		}
		bool any = false;
		for (int v = p_part.z; v < p_part.z + p_part.w; v++) {
			const float mw = moss_k > 0.0f ? _moss_at(r_b.verts[v], r_b.normals[v], moss_ground, moss_k) : 0.0f;
			const float lw = _lichen_at(r_b.verts[v], p_radius, mw);
			Color c = r_b.colors[v];
			c.a = 1.0f - lw;
			r_b.colors.write[v] = c;
			any = any || lw > 0.0f;
		}
		if (!any) {
			return;
		}
		const float girth = MAX(p_girth, 0.001f);
		const float repeats = MAX(1.0f, Math::round(girth / BARK_TILE));
		const float n2 = MAX(1.0f, Math::round(girth / LICHEN_TILE));
		const uint32_t h = (p_salt * 2654435761u) ^ uint32_t(seed) * 2246822519u;
		r_b.part_uv2[r_b.part_uv2.size() - 1] = Vector4(n2 / repeats, girth / (repeats * LICHEN_TILE), float(h & 0xffff) / 65535.0f, float((h >> 16) & 0xffff) / 65535.0f * 7.0f);
	};
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
	// Welded junctions (docs/ideas/trees.md 2d): the limb's tube is cut where it meets its
	// parent, every vertex inside the parent slid along the limb's axis onto the parent's bark
	// (SpeedTree's weld; the contact curve is the intersection of the two generalized cylinders,
	// as in Bloomenthal 1985 and Lluch et al. 2004, without their implicit surface). Normals blend
	// to the parent's over the collar so the seam shades as one surface, and on rigged trees the
	// contact follows the parent's bone so wind does not open it.
	auto weld = [&](Builder &r_b, const Vector4i &p_part, int p_node, const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_rad, int p_sides) {
		const Node &child = nodes[p_node];
		const Node &par = nodes[child.parent];
		const Vector3 d = child.dir;
		if (d.cross(par.dir).length() < 0.12f || child.radius < 0.01f || child.radius > 0.6f * par.radius) {
			// A crotch of near-parallel or near-equal stems: no single surface to weld onto (the
			// collar code leaves these alone too).
			return;
		}
		// A limb on a fork's first internodes: the fork's base runs inside its own parent there
		// (the fork start in the mesh loop), so the parent's axis is not where its bark is, and
		// the slide and spread tore the limb's base into flaps. It stays a plain crotch.
		{
			int x = child.parent;
			for (int k = 0; k < 2 && x > 0; k++) {
				const int px = nodes[x].parent;
				if (px >= 0 && nodes[px].main != x) {
					if (nodes[x].radius > 0.6f * nodes[px].radius) {
						return;
					}
					break;
				}
				x = px;
			}
		}
		// The parent axis around the junction as up to three straight segments (the internode
		// before, the parent internode, the one after), each a tapered cylinder: at a fork the
		// parent's axis bends, and a single infinite cylinder would weld onto bark that is not
		// there.
		struct Seg {
			Vector3 a, u;
			float len, ra, rb;
		};
		Seg segs[6];
		int nseg = 0;
		auto add_seg = [&](int p_n) {
			if (p_n <= 0 || nodes[p_n].removed || nodes[p_n].length < 1e-4f) {
				return;
			}
			const Node &n = nodes[p_n];
			const int nx = n.main;
			float rb = nx >= 0 && !nodes[nx].removed ? nodes[nx].radius : n.radius * 0.6f;
			for (int l = n.first_lateral; l >= 0; l = nodes[l].next_sibling) {
				if (!nodes[l].removed) {
					rb = MAX(rb, MIN(nodes[l].radius, n.radius)); // As the tube (a fork keeps its girth).
				}
			}
			segs[nseg++] = { n.start, n.dir, n.length, n.radius, rb };
		};
		// Up to two internodes before and three after along the parent's axis: a steep limb's
		// footprint runs a long way up its parent.
		{
			int before[2] = { -1, -1 };
			int x = child.parent;
			for (int k = 0; k < 2; k++) {
				const int px = nodes[x].parent;
				if (px > 0 && nodes[px].main == x) {
					before[k] = px;
					x = px;
				} else {
					break;
				}
			}
			for (int k = 1; k >= 0; k--) {
				if (before[k] > 0) {
					add_seg(before[k]);
				}
			}
			add_seg(child.parent);
			x = child.parent;
			for (int k = 0; k < 3; k++) {
				const int nx = nodes[x].main;
				if (nx < 0 || nodes[nx].removed) {
					break;
				}
				add_seg(nx);
				x = nx;
			}
		}
		if (nseg == 0) {
			return;
		}
		// Signed distance from a segment's bark, and the axial parameter; outside the segment's
		// length it does not count. The bark is taken a little under the parent's polygon: between
		// its vertices the tube lies inside the round radius by r (1 - cos(pi / sides)), up to 4%
		// on a thin limb, and a seam on the round radius would stand proud of the faces.
		const int par_sides = CLAMP(int(Math::ceil(Math::TAU * par.radius / 0.03f)), 3, 24);
		const float bark_in = 0.98f * Math::cos(float(Math::PI) / float(par_sides));
		auto seg_at = [&](const Seg &p_s, const Vector3 &p_q, float &r_t, Vector3 &r_radial) {
			const Vector3 rel = p_q - p_s.a;
			r_t = rel.dot(p_s.u) / p_s.len;
			r_radial = rel - p_s.u * rel.dot(p_s.u);
			return bark_in * Math::lerp(p_s.ra, p_s.rb, CLAMP(r_t, 0.0f, 1.0f));
		};
		const float r0 = child.radius;
		const bool steep = d.cross(par.dir).length() < 0.5f;
		const float reach = MAX(par.radius, r0) / MAX(d.cross(par.dir).length(), 0.2f) + 2.0f * r0;
		const int pb = node_bone.size() > uint32_t(child.parent) ? node_bone[child.parent] : 0;
		Vector3 *vv = r_b.verts.ptrw();
		Vector3 *nn = r_b.normals.ptrw();
		Vector2 *uv = r_b.uvs.ptrw();
		// The tube's bark tile length along the axis (as _tube lays it), so a vertex slid along
		// the axis keeps its texel density instead of stretching the bark over the footprint.
		const float girth = Math::TAU * MAX(p_rad[0], 0.001f);
		const float circumference = girth / MAX(1.0f, Math::round(girth / BARK_TILE));
		// Two passes: find every slide first, and weld nothing if any vertex would leave through
		// open bark (a crotch between near-equal stems): a ragged, half-welded tube end shows
		// its inside, the unwelded one stays hidden in its parent as before.
		LocalVector<float> slides;
		slides.resize(p_part.w);
		LocalVector<Vector3> qf; // Each vertex on the undisplaced, welded surface.
		qf.resize(p_part.w);
		for (int i = 0; i < p_part.w; i++) {
			qf[i] = r_b.verts[p_part.z + i];
			slides[i] = 0.0f;
		}
		for (int pass_apply = 0; pass_apply < 2; pass_apply++)
		for (int v = p_part.z; v < p_part.z + p_part.w; v++) {
			// Decide and slide on the undisplaced tube (ring centre + normal x radius), then move
			// the displaced vertex by the same amount: bark relief keeps its shape at the seam
			// instead of furrow vertices jumping ahead of their plate-top neighbours.
			const int ring = (v - p_part.z) / (p_sides + 1);
			if (ring >= int(p_pts.size())) {
				break;
			}
			Vector3 q = p_pts[ring] + nn[v] * p_rad[ring];
			if (pass_apply == 1) {
				qf[v - p_part.z] = q;
			}
			if ((q - child.start).dot(d) > reach) {
				continue;
			}
			// Slide along +d out of the parent's wood, the segments taken as capsules (rounded
			// joints, so the bark has no gaps where the parent's axis bends or narrows). Marched
			// in steps of a fifth of the limb's radius, then refined by bisection. Give up if the
			// point would leave through the far end of the modelled parent (open bark).
			auto inside_at = [&](const Vector3 &p_q) {
				float best = 1e9f;
				for (int k = 0; k < nseg; k++) {
					const Vector3 rel = p_q - segs[k].a;
					const float t = CLAMP(rel.dot(segs[k].u) / segs[k].len, 0.0f, 1.0f);
					const float rk = bark_in * Math::lerp(segs[k].ra, segs[k].rb, t);
					best = MIN(best, (rel - segs[k].u * (t * segs[k].len)).length() - rk);
				}
				return best; // < 0: inside.
			};
			float slid = 0.0f;
			bool ok = true;
			// A steep limb (under 30 degrees off its parent) meets it along a long sliver: slid
			// onto the bark it lies there almost tangent and shows through the bark's furrows.
			// It stays a crotch: the limb enters the parent, and only the normals blend.
			if (!steep && inside_at(q) < 0.0f) {
				const float stepl = 0.2f * r0;
				float x0 = 0.0f, x1 = 0.0f;
				bool out = false;
				for (int st = 1; st <= 400; st++) {
					x1 = stepl * st;
					if (inside_at(q + d * x1) >= 0.0f) {
						out = true;
						break;
					}
					x0 = x1;
				}
				if (!out) {
					ok = false;
				} else {
					for (int it = 0; it < 12; it++) {
						const float xm = 0.5f * (x0 + x1);
						if (inside_at(q + d * xm) < 0.0f) {
							x0 = xm;
						} else {
							x1 = xm;
						}
					}
					const Vector3 nq = q + d * x1;
					// Leaving through the far ends of the modelled parent: no bark there.
					float t_first, t_last;
					Vector3 rr;
					seg_at(segs[0], nq, t_first, rr);
					seg_at(segs[nseg - 1], nq, t_last, rr);
					if (t_first < -0.05f && nseg == 1) {
						ok = false;
					} else if (t_last > 1.05f + r0 / MAX(segs[nseg - 1].len, 1e-3f)) {
						ok = false;
					} else {
						slid = x1 + 1e-4f;
						q = q + d * slid;
					}
				}
			}
			if (pass_apply == 0) {
				if (!ok) {
					return;
				}
				slides[v - p_part.z] = slid;
				continue;
			}
			if (slid > 0.0f) {
				vv[v] += d * slid;
				uv[v].y += slid / circumference;
			}
			qf[v - p_part.z] = q;
		}
		// No folds: on the side where the limb meets its parent far out (the top of a steep
		// limb), slid vertices would pass rings that were already outside and the tube would fold
		// back over itself in a hard crescent. Down each line of the tube, the rings after the
		// last slid one are spread evenly from it to the first ring well clear of it.
		LocalVector<int> seam_ring;
		seam_ring.resize(p_sides + 1);
		{
			const int nrings = MIN(int(p_pts.size()), p_part.w / (p_sides + 1));
			for (int k = 0; k <= p_sides; k++) {
				int m = -1;
				for (int r = 0; r < nrings; r++) {
					if (slides[r * (p_sides + 1) + k] > 0.0f) {
						m = r;
					}
				}
				seam_ring[k] = m;
				// Only the outermost slid ring makes the seam. The rings before it go back inside
				// the parent: collapsed onto the seam they lay just under the bark and showed
				// through its furrows as a disc.
				for (int r = 0; r < m; r++) {
					const int i = r * (p_sides + 1) + k;
					if (slides[i] > 0.0f) {
						qf[i] -= d * slides[i];
						vv[p_part.z + i] -= d * slides[i];
						uv[p_part.z + i].y -= slides[i] / circumference;
						slides[i] = 0.0f;
					}
				}
				if (m < 0 || m + 1 >= nrings) {
					continue;
				}
				auto along = [&](int r) { return (qf[r * (p_sides + 1) + k] - child.start).dot(d); };
				const float lm = along(m);
				int e = m + 1;
				while (e < nrings - 1 && along(e) < lm + 0.6f * r0) {
					e++;
				}
				const float le = MAX(along(e), lm);
				for (int r = m + 1; r < e; r++) {
					const float want = lm + (le - lm) * float(r - m) / float(e - m);
					const float delta = want - along(r);
					if (delta <= 0.0f) {
						continue;
					}
					const int i = r * (p_sides + 1) + k;
					qf[i] += d * delta;
					vv[p_part.z + i] += d * delta;
					uv[p_part.z + i].y += delta / circumference;
				}
			}
		}
		// Weld spread (SpeedTree's weld spread; the branch collar of Shigo 1985): a limb's wood
		// runs down its parent before it turns out, so the base is a teardrop drawn down the
		// parent's bark, widest under the limb, and the limb leaves the bark at a grazing angle
		// instead of a pipe meeting a pipe. Moving a point along the parent's axis keeps it on
		// the parent's surface, so the seam stays shut. The weight falls off with distance from
		// the bark, over about 1.5 limb radii.
		Vector3 pd = par.dir - d * d.dot(par.dir);
		const bool has_down = pd.length_squared() > 1e-4f;
		pd = has_down ? pd.normalized() : Vector3();
		LocalVector<float> wts;
		wts.resize(p_part.w);
		LocalVector<uint8_t> inside;
		inside.resize(p_part.w);
		for (int i = 0; i < p_part.w; i++) {
			const int ring = i / (p_sides + 1);
			wts[i] = 0.0f;
			inside[i] = 0;
			if (ring >= int(p_pts.size())) {
				continue;
			}
			// Distance outside the nearest parent segment's bark.
			float out = 1e9f;
			for (int k = 0; k < nseg; k++) {
				float t;
				Vector3 radial;
				const float rk = seg_at(segs[k], qf[i], t, radial) / bark_in;
				if (t < -0.05f || t > 1.05f || radial.length_squared() < 1e-12f) {
					continue;
				}
				out = MIN(out, radial.length() - rk);
			}
			const float w = Math::pow(CLAMP(1.0f - out / (1.5f * r0), 0.0f, 1.0f), 2.0f);
			wts[i] = w;
			inside[i] = out < -0.05f * r0;
			}
			// The spread is set per line down the tube, from where that line's seam lies under the
			// limb, then smoothed round the tube: set vertex by vertex, neighbouring lines met the
			// bark one ring apart and the collar's edge came out in teeth.
			const int S1 = p_sides + 1;
			const int nrings_w = MIN(int(p_pts.size()), p_part.w / S1);
			if (has_down && !steep && nrings_w > 1) {
				LocalVector<float> col_down, col_lower;
				col_down.resize(p_sides);
				col_lower.resize(p_sides);
				int known = 0;
				for (int k = 0; k < p_sides; k++) {
					const int m = seam_ring[k];
					col_down[k] = -1.0f;
					col_lower[k] = 0.0f;
					if (m < 0) {
						continue;
					}
					const int i = m * S1 + k;
					const Vector3 radial0 = qf[i] - p_pts[m];
					// Under the limb (toward the parent's base) the spread is longest; on top, in
					// the crotch, the limb barely spreads and the bark ridges up instead.
					const float lower = radial0.length_squared() > 1e-12f ? -radial0.normalized().dot(pd) : 0.0f;
					col_lower[k] = lower;
					col_down[k] = r0 * (0.1f + 0.9f * Math::pow(MAX(lower, 0.0f), 1.5f));
					known++;
				}
				if (known > 0) {
					// Lines that never reached the bark take their neighbours' spread.
					for (int pass = 0; pass < p_sides && known < p_sides; pass++) {
						LocalVector<float> nd;
						nd.resize(p_sides);
						for (int k = 0; k < p_sides; k++) {
							nd[k] = col_down[k];
						}
						for (int k = 0; k < p_sides; k++) {
							if (col_down[k] >= 0.0f) {
								continue;
							}
							const float a = col_down[(k + p_sides - 1) % p_sides], b = col_down[(k + 1) % p_sides];
							if (a >= 0.0f || b >= 0.0f) {
								nd[k] = a >= 0.0f && b >= 0.0f ? 0.5f * (a + b) : MAX(a, b);
								known++;
							}
						}
						col_down = nd;
					}
					// The same smoothing arc on every tube: the relief tube has up to five times the
					// sides of the plain one, and two passes there left its collar edge in teeth.
					const int smooth_passes = MAX(2, int(Math::round(2.0f * Math::pow(float(p_sides) / 24.0f, 2.0f))));
					for (int pass = 0; pass < smooth_passes; pass++) {
						LocalVector<float> sm;
						sm.resize(p_sides);
						for (int k = 0; k < p_sides; k++) {
							sm[k] = 0.25f * MAX(col_down[(k + p_sides - 1) % p_sides], 0.0f) + 0.5f * MAX(col_down[k], 0.0f) + 0.25f * MAX(col_down[(k + 1) % p_sides], 0.0f);
						}
						col_down = sm;
					}
					for (int k = 0; k <= p_sides; k++) {
						const int kc = k % p_sides;
						const int m = seam_ring[k] >= 0 ? seam_ring[k] : seam_ring[kc];
						const float dn = col_down[kc];
						const int m0 = MAX(m, 0);
						const float a0 = (qf[m0 * S1 + k] - child.start).dot(d);
						for (int r = m0; r < nrings_w; r++) {
							const int i = r * S1 + k;
							const float dist = (qf[i] - child.start).dot(d) - a0;
							// The seam moves fully along the parent's axis, which keeps it on the
							// bark; the rings after it follow less and less over 1.5 limb radii, a
							// teardrop drawn down the bark.
							const float f = r == m0 ? 1.0f : Math::pow(CLAMP(1.0f - dist / (1.5f * r0), 0.0f, 1.0f), 2.0f);
							if (f <= 0.0f) {
								break;
							}
							const Vector3 shift = -par.dir * (dn * f);
							vv[p_part.z + i] += shift;
							qf[i] += shift;
							// The bark is drawn out over the whole distance the point moved, which keeps
							// its texel density over the collar of a limb square to its parent.
							uv[p_part.z + i].y -= dn * f / circumference;
							// Branch bark ridge (Shigo 1985): in the crotch the two barks push
							// together into a low ridge just above the seam.
							const float lw = col_lower[kc];
							if (lw < -0.3f && r > m0) {
								const Vector3 rad = qf[i] - p_pts[MIN(r, int(p_pts.size()) - 1)];
								const float bump = Math::sin(float(Math::PI) * CLAMP(dist / (1.0f * r0), 0.0f, 1.0f));
								const float ridge = r0 * 0.1f * Math::smoothstep(0.3f, 1.0f, -lw) * bump;
								if (rad.length_squared() > 1e-12f) {
									const Vector3 up = rad.normalized() * ridge;
									vv[p_part.z + i] += up;
									qf[i] += up;
								}
							}
						}
					}
				}
			}
		// Normals from the welded shape (the undisplaced surface, so bark relief keeps its own
		// shading from the normal map), turning to the parent's bark at the seam.
		LocalVector<Vector3> acc;
		acc.resize(p_part.w);
		for (int i = 0; i < p_part.w; i++) {
			acc[i] = Vector3();
		}
		const int rings = p_part.w / (p_sides + 1);
		for (int r = 0; r + 1 < rings; r++) {
			for (int k = 0; k < p_sides; k++) {
				const int a = r * (p_sides + 1) + k, b = a + 1, c = a + p_sides + 1, e = c + 1;
				if (wts[a] <= 0.0f && wts[b] <= 0.0f && wts[c] <= 0.0f && wts[e] <= 0.0f) {
					continue;
				}
				const Vector3 n1 = (qf[c] - qf[a]).cross(qf[b] - qf[a]);
				const Vector3 n2 = (qf[e] - qf[b]).cross(qf[c] - qf[b]);
				acc[a] += n1;
				acc[b] += n1 + n2;
				acc[c] += n1 + n2;
				acc[e] += n2;
			}
		}
		for (int i = 0; i < p_part.w; i++) {
			const int v = p_part.z + i;
			const float w = wts[i];
			if (w <= 0.0f) {
				continue;
			}
			if (inside[i]) {
				// Well inside the parent's model: hidden if the model is right, and where it is
				// not (it is not the mesh everywhere), the tube's own normal is the right one;
				// turned to the parent's, such points lit up as pale flaps in the sun.
				continue;
			}
			Vector3 g = acc[i];
			if (g.length_squared() > 1e-16f) {
				g.normalize();
				if (g.dot(nn[v]) < 0.0f) {
					g = -g; // Winding: keep the outward side.
				}
				nn[v] = nn[v].lerp(g, CLAMP(w * 2.0f, 0.0f, 1.0f)).normalized();
			}
			// Toward the nearest segment's bark normal over the seam itself.
			float out = 1e9f;
			Vector3 pn = nn[v];
			for (int k = 0; k < nseg; k++) {
				float t;
				Vector3 radial;
				const float rk = seg_at(segs[k], qf[i], t, radial) / bark_in;
				if (t < -0.05f || t > 1.05f || radial.length_squared() < 1e-12f) {
					continue;
				}
				const float o = radial.length() - rk;
				if (o < out) {
					out = o;
					pn = radial.normalized();
				}
			}
			const float ws = Math::pow(CLAMP(1.0f - out / (0.8f * r0), 0.0f, 1.0f), 2.0f);
			if (ws > 0.0f) {
				nn[v] = nn[v].lerp(pn, ws).normalized();
			}
			// The tangent frame follows the turned normal (Gram-Schmidt). Left on the tube's own
			// frame it skewed the bark's normal map into pale, speckled glare over the collar.
			if (r_b.use_tangents && r_b.tangents.size() >= (v + 1) * 4) {
				float *tt = r_b.tangents.ptrw() + v * 4;
				Vector3 t(tt[0], tt[1], tt[2]);
				t -= nn[v] * nn[v].dot(t);
				if (t.length_squared() > 1e-10f) {
					t.normalize();
					tt[0] = t.x;
					tt[1] = t.y;
					tt[2] = t.z;
				}
			}
			if (rigged && r_b.bones.size() >= (v + 1) * 4) {
				int32_t *bw = r_b.bones.ptrw();
				float *ww = r_b.weights.ptrw();
				if (ww[v * 4 + 1] == 0.0f && bw[v * 4] != pb) {
					bw[v * 4 + 1] = pb;
					ww[v * 4 + 1] = w;
					ww[v * 4] = 1.0f - w;
				}
			}
		}
	};
	// Old-tree features cut into the main stem's tubes (5e): hollows and rot holes pushed in with a
	// callus lip and darkened, burls pushed out with lumps. Works on the undisplaced tube (ring centre
	// + radial normal), so plain, relief and moss tubes move alike; then normals are rebuilt there.
	const float vet_base_y = nodes[0].start.y;
	// Stems that share the bole: forks off the main stem low down, as thick as it nearly. On an
	// open-grown broadleaf they wrap the lower bole, and features cut into the main stem alone
	// were hidden behind them. They carry the same features, each on its own surface.
	LocalVector<uint8_t> vet_share;
	vet_share.resize(nodes.size());
	for (uint32_t i = 0; i < nodes.size(); i++) {
		vet_share[i] = 0;
	}
	if (veteran_stem >= 0 && !veteran_features.is_empty()) {
		float top = 0.0f;
		for (const VeteranFeature &f : veteran_features) {
			top = MAX(top, f.h + f.half_h);
		}
		const float r_vs = nodes[veteran_stem].radius;
		for (int i = veteran_stem; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
			for (int l = nodes[i].first_lateral; l >= 0; l = nodes[l].next_sibling) {
				if (!nodes[l].removed && nodes[l].radius > 0.4f * r_vs && nodes[l].start.y - vet_base_y < top) {
					vet_share[l] = 1;
				}
			}
		}
	}
	const float vet_r_stem = veteran_stem >= 0 ? nodes[veteran_stem].radius * 1.15f : 1.0f;
	// The root collar meeting the soil (trees.md 5d): main roots leave the stem as 5-7 ridges
	// (Mattheck's buttress form; Coutts 1983) and the whole foot swells, both pushed out sideways
	// (along the normal they folded under), growing over the last half metre and still widening
	// below the soil line, so the surface runs into the ground at a shallow angle with no rim and
	// no undercut. The soil darkens the bark's foot (splash); the bark, its relief and the moss
	// shell all take the same push, so the shell stays on the bark.
	RandomPCG root_rng;
	root_rng.seed(uint64_t(seed) * 2654435761u + 0x726f6f74u);
	const int root_ridges = 5 + int(root_rng.rand() % 3u);
	const float root_phase = root_rng.randf() * float(Math::TAU);
	const float ground_y = nodes[0].start.y;
	auto root_push = [&](float p_h, float p_az) -> float {
		const float ridge = Math::pow(0.5f + 0.5f * Math::cos(float(root_ridges) * (p_az - root_phase)), 3.0f);
		// Above the soil the swell dies away over about half a stem radius (the ridges) and a
		// stem radius (the foot); below it keeps widening for 20 cm as the roots spread.
		const float up = p_h >= 0.0f ? 1.0f : 1.0f + MIN(-p_h, 0.2f) / 0.08f;
		const float foot = p_h >= 0.0f ? Math::exp(-p_h / MAX(0.05f, trunk_r)) : up;
		const float rid = p_h >= 0.0f ? Math::exp(-p_h / MAX(0.05f, 0.6f * trunk_r)) : up;
		return trunk_r * flare * (0.18f * foot + 0.5f * ridge * rid);
	};
	// Splash and soil over the bottom 25 cm: darker, browner bark (and moss), applied after the
	// moss tint so the green does not cover it.
	auto soil_tint = [&](Builder &r_b, const Vector4i &p_part, float p_k) {
		const Vector3 *vv = r_b.verts.ptr();
		Color *cc = r_b.colors.ptrw();
		for (int i = 0; i < p_part.w; i++) {
			const int v = p_part.z + i;
			const float h = vv[v].y - ground_y;
			if (h > 0.4f) {
				continue;
			}
			const float soil = 1.0f - Math::smoothstep(0.0f, 0.3f, h);
			const Color c = cc[v];
			cc[v] = c.lerp(Color(0.085f, 0.065f, 0.045f, c.a), p_k * 0.7f * soil);
		}
	};
	// The roots' bearings (on the ridges) and run lengths. None under a basal hollow: the rot
	// opened there, and a root swept through the hollow's floor.
	LocalVector<float> root_az, root_len, root_bend;
	// No surface roots on saplings and shrubs, nor on hedges and topiary (clipped to an envelope).
	const bool roots_on = trunk_r >= 0.08f && envelope.size == Vector3();
	for (int k = 0; k < root_ridges; k++) {
		const float az = root_phase + float(Math::TAU) * k / root_ridges;
		float len = trunk_r * Math::lerp(1.4f, 2.4f, root_rng.randf()) * (0.6f + 0.8f * MIN(flare, 1.0f));
		for (const VeteranFeature &f : veteran_features) {
			if (f.kind == VET_BASAL_HOLLOW && Math::abs(Math::angle_difference(az, f.azimuth)) < f.half_w / MAX(0.05f, trunk_r) + 0.35f) {
				len = 0.0f;
			}
		}
		root_az.push_back(az);
		root_len.push_back(roots_on ? len : 0.0f);
		// Roots wander round stones and older roots rather than run out as straight spokes.
		root_bend.push_back(Math::lerp(-0.45f, 0.45f, root_rng.randf()));
	}
	// The root collar and the roots as one surface (trees.md 5d, root weld). Above the roots the
	// flare and ridges push the bark out sideways. From about a stem radius above the soil down
	// through the sunk base, the stem's own rings are swept out into the roots: on a root's
	// bearing they run outward and down along the root (full girth to the flare, then the rapid
	// taper, diving under the soil); between roots they keep the flare and sink. A bump across
	// each root, narrowing along it, blends the two, so trunk, root crown and flanks share one
	// mesh with no crease. Normals are then rebuilt from the displaced mesh. Bark, relief tube and
	// moss shell take the same map (bearing from the axis, not the normals, so they agree).
	const float root_top = trunk_r * 1.2f * MIN(1.0f, 0.5f + flare);
	const float root_sink = 0.3f;
	// Each root's radius where it leaves the flare; round or a little wider than tall in section
	// (Eis 1974: laterals are oval near the stem), sinking into the soil while still thick.
	const float root_r0 = trunk_r * 0.36f * MIN(1.0f, 0.5f + flare);
	foot_roots.on = roots_on && !fallen && !fallen_branch && flare > 0.0f && !nodes.is_empty();
	foot_roots.axis = nodes.is_empty() ? Vector3() : nodes[0].start;
	foot_roots.trunk_r = trunk_r;
	foot_roots.width = root_r0 * 1.15f;
	foot_roots.az = root_az;
	foot_roots.len = root_len;
	foot_roots.bend = root_bend;
	auto root_collar = [&](Builder &r_b, const Vector4i &p_part, int p_sides, const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_rad, bool p_colour) {
		const Vector3 p_axis = p_pts[0];
		// Down to the sunk base ring, so the swept rings stay spread over the whole root.
		const float sink_here = MAX(0.05f, ground_y - p_axis.y);
		Vector3 *vv = r_b.verts.ptrw();
		Vector3 *nw = r_b.normals.ptrw();
		const int ring = p_sides + 1;
		LocalVector<uint8_t> moved;
		moved.resize(p_part.w);
		// The bark relief the tube was displaced by, taken off before the sweep and put back
		// after it, faded out at the swept surface's own soil line: kept at the height it was
		// built at, the furrows of the roots' tips stood out of the ground as slivers.
		LocalVector<float> relief_off, root_w, root_sda, root_ratio;
		relief_off.resize(p_part.w);
		root_w.resize(p_part.w);
		root_sda.resize(p_part.w);
		root_ratio.resize(p_part.w);
		for (int i = 0; i < p_part.w; i++) {
			moved[i] = 0;
			relief_off[i] = 0.0f;
			root_w[i] = 0.0f;
			root_sda[i] = 0.0f;
			root_ratio[i] = 1.0f;
			const int v = p_part.z + i;
			const float h = vv[v].y - ground_y;
			if (h > 6.0f * trunk_r) {
				continue;
			}
			Vector3 hd(vv[v].x - p_axis.x, 0.0f, vv[v].z - p_axis.z);
			float r_t = hd.length();
			if (r_t < 1e-4f) {
				continue;
			}
			hd /= r_t;
			const int ri = i / ring;
			if (h < root_top && ri < int(p_rad.size())) {
				relief_off[i] = vv[v].distance_to(p_pts[ri]) - p_rad[ri];
				r_t -= relief_off[i];
			}
			const float az = Math::atan2(hd.z, hd.x);
			float d = r_t + root_push(h, az);
			float y = h;
			if (h < root_top) {
				int best = -1;
				float best_da = 1e9f;
				for (int k = 0; k < int(root_az.size()); k++) {
					const float da = Math::abs(Math::angle_difference(az, root_az[k]));
					if (root_len[k] > 0.0f && da < best_da) {
						best_da = da;
						best = k;
					}
				}
				if (best >= 0) {
					const float t = CLAMP((root_top - h) / (root_top + sink_here), 0.0f, 1.0f);
					const float sda = Math::angle_difference(root_az[best] + root_bend[best] * t * t, az);
					// The root as an oval tube (half width a, half height b) whose centre runs out
					// from the flare and sinks: about level with the soil for most of its run and
					// below it at the end, so it enters the ground still thick, its side steep.
					const float rr = root_r0 * (1.0f - 0.3f * t);
					const float a_w = rr * 1.15f, b_h = rr * 0.85f;
					const float d_c = r_t + root_push(root_top, root_az[best]) * (1.0f - t) + root_len[best] * Math::pow(t, 1.1f) + rr * Math::smoothstep(0.0f, 0.3f, t);
					const float s1 = Math::smoothstep(0.0f, 0.35f, t);
					const float yc = (root_top - b_h) * (1.0f - s1) * (1.0f - s1) - rr * (0.1f + 1.6f * Math::smoothstep(0.45f, 0.95f, t)) * s1;
					const float lat = Math::abs(sda) * d_c;
					const float qe = lat / a_w;
					// The oval's top, then straight down past its widest point.
					const float y_e = qe < 1.0f ? yc + b_h * Math::sqrt(1.0f - qe * qe) : yc - (lat - a_w) * 1.2f;
					// Between roots: the stem's own wall near it, then below the soil.
					const float y_floor = Math::lerp(h, MIN(h, -0.35f * trunk_r), Math::smoothstep(0.0f, 0.12f, t));
					// Smooth maximum of the two: a rounded fillet in the crotch where root and
					// stem meet, no crease.
					const float kf = rr * Math::lerp(0.55f, 0.12f, Math::smoothstep(0.0f, 0.25f, t));
					const float hm = CLAMP(0.5f + 0.5f * (y_e - y_floor) / kf, 0.0f, 1.0f);
					y = Math::lerp(y_floor, y_e, hm) + kf * hm * (1.0f - hm) * 0.5f;
					d = Math::lerp(d, d_c, hm);
					root_w[i] = hm;
					root_sda[i] = sda;
					root_ratio[i] = d / MAX(1e-3f, r_t);
					// The furrows were cut for the stem's girth; swept out along a root they
					// stretched into deep black grooves. Shallower the further out they go.
					relief_off[i] *= 1.0f - 0.65f * hm * Math::smoothstep(0.1f, 0.6f, t);
				}
				moved[i] = 1;
			}
			vv[v] = Vector3(p_axis.x + hd.x * d, ground_y + y, p_axis.z + hd.z * d);
			if (!moved[i]) {
				// Above the roots: the normal of the pushed surface, from the push's slopes.
				const float dh = 0.01f, da = 0.01f;
				const float dpdh = (root_push(h + dh, az) - root_push(h - dh, az)) / (2.0f * dh);
				const float dpds = (root_push(h, az + da) - root_push(h, az - da)) / (2.0f * da * MAX(0.05f, d));
				const Vector3 side = Vector3(0, 1, 0).cross(hd);
				nw[v] = (nw[v] - Vector3(0, dpdh, 0) - side * dpds).normalized();
			}
		}
		const int rings = p_part.w / ring;
		// No undercut: down each column of the swept part a ring never comes in nearer the axis
		// or rises above the ring over it. Where a root's flank narrows faster than it sinks,
		// the blend pulled lower rings back towards the stem wall, and the flank curled under
		// itself into thin dark slits (darkest under the path tracer).
		for (int k = 0; k < ring; k++) {
			for (int r = rings - 2; r >= 0; r--) {
				const int i = r * ring + k, above = (r + 1) * ring + k;
				if (!moved[i] || !moved[above]) {
					continue;
				}
				Vector3 &a = vv[p_part.z + i];
				const Vector3 &b = vv[p_part.z + above];
				Vector2 ra(a.x - p_axis.x, a.z - p_axis.z);
				const float rb = Vector2(b.x - p_axis.x, b.z - p_axis.z).length();
				const float la = ra.length();
				if (la < rb && la > 1e-5f) {
					ra *= rb / la;
					a.x = p_axis.x + ra.x;
					a.z = p_axis.z + ra.y;
				}
				a.y = MIN(a.y, b.y);
			}
		}
		// Normals of the swept part from the mesh's own grid (round the ring and along the stem),
		// one winding sign for the part.
		LocalVector<Vector3> fn;
		fn.resize(p_part.w);
		float sign_sum = 0.0f;
		for (int r = 0; r < rings; r++) {
			for (int k = 0; k < ring; k++) {
				const int i = r * ring + k;
				fn[i] = Vector3();
				if (!moved[i]) {
					continue;
				}
				const int km = (k + p_sides - 1) % p_sides, kp = (k + 1) % p_sides;
				const int ra = MAX(r - 1, 0), rb = MIN(r + 1, rings - 1);
				const Vector3 du = vv[p_part.z + r * ring + kp] - vv[p_part.z + r * ring + km];
				const Vector3 dr = vv[p_part.z + rb * ring + k] - vv[p_part.z + ra * ring + k];
				const Vector3 n = du.cross(dr);
				if (n.length_squared() > 1e-14f) {
					fn[i] = n.normalized();
					sign_sum += fn[i].dot(nw[p_part.z + i]);
				}
			}
		}
		const float sgn = sign_sum < 0.0f ? -1.0f : 1.0f;
		for (int i = 0; i < p_part.w; i++) {
			if (!moved[i] || fn[i] == Vector3()) {
				continue;
			}
			const int v = p_part.z + i;
			nw[v] = fn[i] * sgn;
			vv[v] += nw[v] * (relief_off[i] * Math::smoothstep(0.02f, 0.2f, vv[v].y - ground_y));
			if (r_b.use_tangents && int(r_b.tangents.size()) >= (v + 1) * 4) {
				float *tw = r_b.tangents.ptrw() + v * 4;
				Vector3 tg(tw[0], tw[1], tw[2]);
				tg -= nw[v] * nw[v].dot(tg);
				if (tg.length_squared() < 1e-10f) {
					tg = nw[v].get_any_perpendicular();
				}
				tg.normalize();
				tw[0] = tg.x;
				tw[1] = tg.y;
				tw[2] = tg.z;
			}
		}
		// Even texel density along the roots: v (along the stem, in tile widths) was laid down
		// for the stem's rings a few centimetres apart; swept out along a root the same rings lie
		// far apart and the bark and moss tiles stretched into streaks. Down each column, v is
		// laid again from the swept surface's own arc length, at the stem's scale (from the
		// unswept rings above, where v already advances by distance over the tile width).
		{
			Vector2 *uw = r_b.uvs.ptrw();
			for (int k = 0; k < ring; k++) {
				int top = -1;
				for (int r = rings - 1; r >= 0; r--) {
					if (moved[r * ring + k]) {
						top = r;
						break;
					}
				}
				if (top < 0 || top + 2 >= rings) {
					continue;
				}
				const int a = p_part.z + (top + 1) * ring + k, b = p_part.z + (top + 2) * ring + k;
				const float len_ab = vv[a].distance_to(vv[b]);
				if (len_ab < 1e-5f) {
					continue;
				}
				const float per_m = (uw[b].y - uw[a].y) / len_ab;
				float vcur = uw[a].y;
				for (int r = top; r >= 0; r--) {
					const int c = p_part.z + r * ring + k, above = p_part.z + (r + 1) * ring + k;
					vcur -= vv[c].distance_to(vv[above]) * per_m;
					uw[c].y = vcur;
				}
			}
			// Across a root too: u spreads with the distance from the axis, so the tiles keep
			// their width where the rings are swept out wide (they stretched sideways).
			for (int r = 0; r < rings; r++) {
				const int i0 = r * ring;
				if (i0 + 1 >= p_part.w || !moved[i0]) {
					continue;
				}
				// u per radian of bearing, from the ring's first two columns.
				const Vector3 &p0 = vv[p_part.z + i0], &p1 = vv[p_part.z + i0 + 1];
				const float a0 = Math::atan2(p0.z - p_axis.z, p0.x - p_axis.x), a1 = Math::atan2(p1.z - p_axis.z, p1.x - p_axis.x);
				const float dang = Math::angle_difference(a0, a1);
				if (Math::abs(dang) < 1e-5f) {
					continue;
				}
				const float dudang = (uw[p_part.z + i0 + 1].x - uw[p_part.z + i0].x) / dang;
				for (int k = 0; k < ring; k++) {
					const int i = i0 + k;
					if (root_w[i] > 0.0f || root_ratio[i] != 1.0f) {
						uw[p_part.z + i].x += root_sda[i] * dudang * (root_ratio[i] - 1.0f);
					}
				}
			}
		}
		// Final normals of the swept part from its own triangles, after the relief went back on:
		// grid normals taken before it disagreed with the faces where a root's flank turns
		// sharply into the soil, and the path tracer shaded those triangles black (the shading
		// normal faced away from the light the faces received).
		{
			LocalVector<Vector3> acc;
			acc.resize(p_part.w);
			for (Vector3 &a : acc) {
				a = Vector3();
			}
			const int *ix = r_b.indices.ptr();
			for (int q = p_part.x; q + 2 < p_part.x + p_part.y; q += 3) {
				const int a = ix[q] - p_part.z, b = ix[q + 1] - p_part.z, c = ix[q + 2] - p_part.z;
				if (a < 0 || b < 0 || c < 0 || a >= p_part.w || b >= p_part.w || c >= p_part.w) {
					continue;
				}
				if (!moved[a] && !moved[b] && !moved[c]) {
					continue;
				}
				const Vector3 f = (vv[p_part.z + c] - vv[p_part.z + a]).cross(vv[p_part.z + b] - vv[p_part.z + a]);
				acc[a] += f;
				acc[b] += f;
				acc[c] += f;
			}
			// The ring's seam pair shares one normal.
			for (int i = 0; i + p_sides < p_part.w; i += ring) {
				const Vector3 sum = acc[i] + acc[i + p_sides];
				acc[i] = sum;
				acc[i + p_sides] = sum;
			}
			float ssum = 0.0f;
			for (int i = 0; i < p_part.w; i++) {
				if (moved[i] && acc[i].length_squared() > 1e-16f) {
					ssum += acc[i].normalized().dot(nw[p_part.z + i]);
				}
			}
			const float fs = ssum < 0.0f ? -1.0f : 1.0f;
			for (int i = 0; i < p_part.w; i++) {
				if (moved[i] && acc[i].length_squared() > 1e-16f) {
					nw[p_part.z + i] = acc[i].normalized() * fs;
				}
			}
		}
		if (p_colour) {
			soil_tint(r_b, p_part, 1.0f);
		}
	};
	// Cut trees (trees.md 5l) carry their history as modelled wood, not paint: the callus over
	// every cut builds hard-edged knobs, clustered into burrs (each knob a dormant bud's trace
	// that swelled; Read 2000, Veteran Trees), and the poles carry the girdle scars of each
	// year's bud scales, shallow fluting, and folds of bark where they leave the head.
	// The pollard head's centre, for the knuckles round it.
	Vector3 head_c;
	if (head_r > 0.0f && veteran_stem >= 0) {
		head_c = Vector3(nodes[veteran_stem].start.x, head_y0 + 1.2f * head_r, nodes[veteran_stem].start.z);
		for (int i = veteran_stem; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
			head_c.x = nodes[i].end().x;
			head_c.z = nodes[i].end().z;
		}
	}
	auto hash3 = [](int p_x, int p_y, int p_z, uint32_t p_salt) -> float {
		uint32_t h = uint32_t(p_x) * 73856093u ^ uint32_t(p_y) * 19349663u ^ uint32_t(p_z) * 83492791u ^ p_salt;
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return float(h & 0xffffffu) / float(0x1000000);
	};
	// Knobs as spherical caps on a jittered grid, each with a steep foot (the cap's profile is
	// (1 - d^2)^0.3, near vertical at its edge), present where a cluster noise is high: burrs, not
	// a melted swelling. r_crease is the crevice between and around them.
	auto callus_knobs = [&](const Vector3 &p_p, float p_cell, float p_gate_scale, float p_gate, uint32_t p_salt, float &r_crease) -> float {
		const Vector3 g = p_p / p_cell;
		const int ix = int(Math::floor(g.x)), iy = int(Math::floor(g.y)), iz = int(Math::floor(g.z));
		float best = 0.0f, near = 0.0f;
		for (int dz = -1; dz <= 1; dz++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
					const Vector3 cell_c = Vector3(cx + 0.5f, cy + 0.5f, cz + 0.5f) * p_cell;
					const float pres = moss_noise(cell_c / p_gate_scale, p_salt);
					if (pres < p_gate) {
						continue;
					}
					const Vector3 c = (Vector3(cx, cy, cz) + Vector3(0.2f + 0.6f * hash3(cx, cy, cz, p_salt + 1u), 0.2f + 0.6f * hash3(cx, cy, cz, p_salt + 2u), 0.2f + 0.6f * hash3(cx, cy, cz, p_salt + 3u))) * p_cell;
					const float rk = p_cell * Math::lerp(0.5f, 0.85f, hash3(cx, cy, cz, p_salt + 4u)) * Math::lerp(0.75f, 1.0f, Math::smoothstep(p_gate, p_gate + 0.2f, pres));
					const float d = p_p.distance_to(c) / rk;
					if (d < 1.0f) {
						best = MAX(best, rk * 0.85f * Math::pow(1.0f - d * d, 0.25f));
					}
					near = MAX(near, 1.0f - Math::smoothstep(1.0f, 1.4f, d));
				}
			}
		}
		r_crease = near * (1.0f - Math::smoothstep(0.0f, 0.2f * p_cell, best));
		return best;
	};
	// Per chain, set before its tubes are deformed (p_features 3): the year boundaries up a pole
	// (girdle scars), and where a pole leaves its knuckle (base point, heading, radius).
	LocalVector<Vector3> cut_scars;
	Vector3 cut_j, cut_jdir;
	float cut_jr = 0.0f;
	int knob_cap_from = INT_MAX; // A pollard knuckle's closing dome: from this ring the head's knobs cover it.
	int s_chain_salt = 0; // The chain's first node, salting its branch scars.
	float cut_arc0 = 0.0f; // Distance up the chain to the tube's first ring, when it is built in pieces.
	const bool cut_tree = cut_height > 0.0f && !cut_heads.is_empty() && decay_class == 0 && !fallen && !fallen_branch;
	// A coppice stool's height over the soil: its knobs cover everything below it.
	float stool_top = -1.0f;
	float stool_r = 0.0f;
	if (cut_tree && cut_height <= 0.5f) {
		for (const int i : bolling) {
			if (!nodes[i].removed) {
				stool_top = MAX(stool_top, nodes[i].end().y - vet_base_y + 0.5f * nodes[i].radius);
				stool_r = MAX(stool_r, nodes[i].radius);
			}
		}
	}
	auto veteran_deform = [&](Builder &r_b, const Vector4i &p_part, const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_rad, int p_sides, bool p_colour, int p_features) {
		if ((veteran_features.is_empty() && p_features != 3 && !(p_features == 2 && head_r > 0.0f)) || !p_features) {
			return;
		}
		Vector3 *vv = r_b.verts.ptrw();
		Vector3 *nn = r_b.normals.ptrw();
		Color *cc = r_b.colors.ptrw();
		LocalVector<float> touched;
		touched.resize(p_part.w);
		bool any = false;
		// Distance up the chain, ring by ring.
		LocalVector<float> arc;
		arc.resize(p_pts.size());
		for (uint32_t q = 0; q < p_pts.size(); q++) {
			arc[q] = q > 0 ? arc[q - 1] + p_pts[q].distance_to(p_pts[q - 1]) : cut_arc0;
		}
		for (int i = 0; i < p_part.w; i++) {
			touched[i] = 0.0f;
			const int v = p_part.z + i;
			const int ring = i / (p_sides + 1);
			if (ring >= int(p_pts.size())) {
				break;
			}
			// Bearing and push from the ring's centre, not the vertex normal: the bark relief's
			// normals swing by tens of degrees, so vertices at the hollow's edge fell in or out of
			// it at random and folded into pale blades across the opening.
			Vector3 n = vv[v] - p_pts[ring];
			if (p_features == 3) {
				// Square to the chain's own axis: poles lean and bend.
				const Vector3 ax = p_pts[MIN(ring + 1, int(p_pts.size()) - 1)] - p_pts[MAX(ring - 1, 0)];
				if (ax.length_squared() > 1e-10f) {
					const Vector3 an = ax.normalized();
					n -= an * n.dot(an);
				}
			} else {
				n.y = 0.0f;
			}
			if (n.length_squared() < 1e-8f) {
				continue;
			}
			n.normalize();
			const float r = p_rad[ring];
			const float h = p_pts[ring].y - vet_base_y;
			const float az = Math::atan2(n.z, n.x);
			float off = 0.0f;
			float dark = 1.0f;
			float rot = 0.0f;
			float w_any = 0.0f;
			float algae = 0.0f;
			float pale = 0.0f; // Crust lichen on a coppice rod.
			float head_win = 0.0f;
			float hoff = 0.0f; // Pushed along hdir: over the head's top, out from its centre.
			Vector3 hdir = n;
			if ((p_features == 2 || p_features == 3) && head_r > 0.0f && p_pts[ring].y > head_y0) {
				head_win = Math::smoothstep(head_y0, head_y0 + 0.5f * head_r, p_pts[ring].y);
				if (p_features == 3) {
					// The knuckles round the head share its knobs, which fade up each pole.
					head_win *= 1.0f - Math::smoothstep(1.5f * head_r, 1.9f * head_r, (p_pts[ring] + n * r).distance_to(head_c));
					if (cut_jr > 0.0f) {
						head_win *= 1.0f - Math::smoothstep(0.0f, 1.2f * cut_jr, (p_pts[ring] - cut_j).dot(cut_jdir));
					}
				}
			}
			if (p_features == 3 && head_r > 0.0f && int(ring) >= knob_cap_from) {
				// The callus dome over a knuckle whose poles are gone: knobbed like the head. Plain,
				// its bark converged to the apex as a pinwheel.
				head_win = MAX(head_win, CLAMP(0.5f * float(int(ring) - knob_cap_from), 0.0f, 1.0f));
			}
			if (head_win > 0.0f) {
				// A pollard's head (trees.md 5l): broad swellings of the callus of every cut, split
				// by deep fissures where they met, and over them clusters of hard-edged knobs, the
				// burrs (Read 2000), whatever the bark's own texture. All modelled: the head's
				// rings are about a centimetre apart each way.
				const float win = head_win;
				// The surface point itself, and out from the head's centre over its top: pushed
				// sideways from the ring's centre, the rounded top's knobs smeared into radial
				// pleats round its pole.
				// From the ring's centre at the ring's radius, so the bark, its relief and the moss
				// shell sample the same point.
				const Vector3 hp = p_pts[ring] + (vv[v] - p_pts[ring]).normalized() * r;
				const Vector3 from_c = hp - head_c;
				if (from_c.length_squared() > 1e-8f) {
					hdir = n.lerp(from_c.normalized(), Math::smoothstep(head_c.y, head_c.y + 0.6f * head_r, hp.y)).normalized();
				}
				const float k1 = moss_noise(hp * (1.0f / (0.32f * head_r)), 0x6b6e6231u) - 0.5f;
				const float ridge = 1.0f - Math::abs(2.0f * moss_noise(hp * Vector3(1.0f / (0.22f * head_r), 1.0f / (0.7f * head_r), 1.0f / (0.22f * head_r)), 0x6b6e6233u) - 1.0f);
				const float fis = Math::smoothstep(0.8f, 0.97f, ridge);
				float crease = 0.0f;
				const float kn = callus_knobs(hp, CLAMP(0.12f * head_r, 0.035f, 0.07f), 0.45f * head_r, 0.38f, 0x6b6e6234u, crease);
				hoff += win * (head_r * (0.07f * k1 - 0.14f * fis) + kn * (1.0f - fis));
				// The bark's own furrows mostly closed over by the callus: at full depth they
				// crossed the knobs as long pleats.
				hoff -= 0.75f * win * (vv[v].distance_to(p_pts[ring]) - r);
				dark = MIN(dark, Math::lerp(1.0f, 0.42f, win * fis));
				dark = MIN(dark, Math::lerp(1.0f, 0.45f, win * crease));
				// Green algae (Desmococcus) and moss film where water lingers between the knobs
				// and on their tops; drier, paler grey on the bulges' flanks.
				algae = win * CLAMP(0.35f + 1.6f * (-k1) + 0.5f * n.y + 0.3f * fis + 0.3f * crease, 0.0f, 0.85f);
				w_any = 1.0f;
			}
			if (p_features == 3) {
				const Vector3 hp = p_pts[ring] + n * r;
				if (stool_top > 0.0f && h < stool_top + 0.06f) {
					// A coppice stool: the same knobs and fissures over the stool's lobes, fading
					// up the stems' first few centimetres.
					const float win = 1.0f - Math::smoothstep(stool_top - 0.05f, stool_top + 0.05f, h);
					const float k1 = moss_noise(hp * (1.0f / MAX(0.5f * stool_r, 0.05f)), 0x73746c31u) - 0.5f;
					const float ridge = 1.0f - Math::abs(2.0f * moss_noise(hp * (1.0f / MAX(0.35f * stool_r, 0.04f)), 0x73746c32u) - 1.0f);
					const float fis = Math::smoothstep(0.8f, 0.97f, ridge);
					float crease = 0.0f;
					const float kn = callus_knobs(hp, 0.05f, 0.15f, 0.4f, 0x73746c33u, crease);
					off += win * (MIN(r, 0.12f) * (0.25f * k1 - 0.3f * fis) + kn * (1.0f - fis));
					dark = MIN(dark, Math::lerp(1.0f, 0.45f, win * fis));
					dark = MIN(dark, Math::lerp(1.0f, 0.62f, win * crease));
					algae = MAX(algae, win * CLAMP(0.2f + 1.2f * (-k1) + 0.3f * crease, 0.0f, 0.6f));
					w_any = 1.0f;
				}
				// Up the pole: shallow, wavering flutes along it, a few round it (as on hornbeam
				// and old hazel rods), read on the tube's own frame so they follow its bends.
				const int col = i % (p_sides + 1);
				const float th = float(Math::TAU) * float(col % p_sides) / float(p_sides);
				const float along = arc[ring];
				const float flute = moss_noise(Vector3(1.4f * Math::cos(th), 1.4f * Math::sin(th), along / MAX(8.0f * r, 0.15f)), 0x666c7574u) - 0.5f;
				// Rods and withies are round, if not perfectly: a faint unevenness. A pole fattening
				// into a limb grows its flutes.
				const float thick = Math::smoothstep(0.03f, 0.08f, r);
				off += 0.35f * r * flute * thick;
				off += 0.07f * r * (1.0f - thick) * (moss_noise(hp * (1.0f / MAX(1.5f * r, 0.01f)), 0x726f6475u) - 0.5f);
				if (thick < 1.0f) {
					// A rod's nodes: every 12-20 cm a bud or a shed side shoot left a low swelling
					// on one side, the rod kinking a little there (hazel and willow rods are never
					// straight tubes; Rackham 2006), its scar darker.
					const float cell_l = 0.16f;
					const int ia = int(Math::floor(along / cell_l));
					float bump = 0.0f;
					for (int da = -1; da <= 1; da++) {
						const int ca = ia + da;
						const uint32_t salt = uint32_t(s_chain_salt) * 131u + 5u;
						const float ac = (ca + 0.25f + 0.5f * hash3(ca, 1, 11, salt)) * cell_l;
						// Alternate sides, as hazel's buds are alternate.
						const float tc = float(Math::TAU) * (0.5f * float(ca & 1) + 0.3f * hash3(ca, 2, 12, salt));
						const float dl = (along - ac) / MAX(1.2f * r, 0.008f);
						const float dw = Math::angle_difference(tc, th) / 1.1f;
						bump = MAX(bump, Math::exp(-(dl * dl + dw * dw)) * (0.6f + 0.4f * hash3(ca, 3, 13, salt)));
					}
					off += (1.0f - thick) * r * 0.16f * bump;
					dark = MIN(dark, Math::lerp(1.0f, 0.78f, (1.0f - thick) * Math::smoothstep(0.75f, 0.95f, bump)));
				}
				if (thick > 0.0f) {
					// Branch scars on a thick pole: the twigs it shed as it grew, each a low ring of
					// callus round a darker hollow, the bark wrinkled into a chevron above it (the
					// "Chinese moustache" of beech; Vaucher 2003). Cells half a metre along and a
					// quarter of the way round.
					const float cell_l = 0.5f;
					const int ia = int(Math::floor(along / cell_l)), it = int(Math::floor(th / (float(Math::TAU) * 0.25f)));
					float lip = 0.0f, pit = 0.0f, chev = 0.0f;
					for (int da = -1; da <= 1; da++) {
						for (int dt = -1; dt <= 1; dt++) {
							const int ca = ia + da, ct = (it + dt + 4) % 4;
							const uint32_t salt = uint32_t(s_chain_salt) * 31u;
							if (hash3(ca, ct, 7, salt) > 0.45f) {
								continue;
							}
							const float ac = (ca + 0.2f + 0.6f * hash3(ca, ct, 8, salt)) * cell_l;
							const float tc = (ct + 0.2f + 0.6f * hash3(ca, ct, 9, salt)) * float(Math::TAU) * 0.25f;
							const float rs = r * Math::lerp(0.18f, 0.35f, hash3(ca, ct, 10, salt));
							const float dl = along - ac, dw = Math::angle_difference(tc, th) * r;
							const float q = Math::sqrt(dl * dl + dw * dw) / rs;
							lip = MAX(lip, Math::exp(-(q - 1.0f) * (q - 1.0f) * 6.0f));
							pit = MAX(pit, 1.0f - Math::smoothstep(0.5f, 0.95f, q));
							// The chevron: two short creases running up and out from above the scar.
							if (dl > 0.0f && dl < 3.0f * rs) {
								const float v = Math::abs(dw) - 0.9f * dl;
								chev = MAX(chev, Math::exp(-v * v / (0.04f * rs * rs)) * (1.0f - dl / (3.0f * rs)) * Math::smoothstep(0.3f * rs, 0.8f * rs, Math::abs(dw)));
							}
						}
					}
					off += thick * r * (0.07f * lip - 0.05f * pit - 0.025f * chev);
					dark = MIN(dark, Math::lerp(1.0f, 0.55f, thick * pit));
					dark = MIN(dark, Math::lerp(1.0f, 0.8f, thick * chev));
				}
				if (cut_height <= 0.5f) {
					// A coppice rod is not one flat colour: a green film of algae up its shaded
					// foot, pale crust lichen in patches, coppery young bark up top (hazel).
					const float low = 1.0f - Math::smoothstep(0.2f, 1.4f, h);
					algae = MAX(algae, 0.45f * low * Math::smoothstep(0.35f, 0.7f, moss_noise(hp * 3.0f, 0x72616c67u)));
					pale = 0.7f * Math::smoothstep(0.56f, 0.7f, moss_noise(hp * Vector3(14.0f, 5.0f, 14.0f), 0x72636c6eu)) * (1.0f - 0.6f * Math::smoothstep(1.0f, 2.5f, h));
				}
				// Girdle scars: each year's bud-scale scar, a raised ring with a slight waist above.
				float scar = 0.0f;
				const float sw = MAX(0.2f * r, 0.004f);
				for (const Vector3 &sp : cut_scars) {
					const float d = p_pts[ring].distance_to(sp);
					if (d < 4.0f * sw) {
						const float a = d / sw;
						scar = MAX(scar, Math::exp(-a * a));
						off -= 0.012f * r * Math::exp(-(a - 2.0f) * (a - 2.0f));
					}
				}
				// Faint: a hazel rod's or a withy's scars are low rings, not a bamboo's nodes.
				off += 0.03f * r * scar;
				dark = MIN(dark, Math::lerp(1.0f, 0.92f, scar));
				if (cut_jr > 0.0f) {
					// Folds of bark round the pole's base, where it swells out of the head: the
					// stretched bark buckles in rings (the "elephant skin" of beech and hornbeam).
					const float dc = (p_pts[ring] - cut_j).dot(cut_jdir);
					if (dc > 0.0f && dc < 2.5f * cut_jr) {
						// Irregular and partial: the folds wander in spacing, die out round the
						// pole, and are deepest in patches (on the crotch side most of all).
						const float fade = 1.0f - dc / (2.5f * cut_jr);
						const float wob = 1.6f * (moss_noise(hp * (1.0f / MAX(0.7f * cut_jr, 0.02f)), 0x666f6c64u) - 0.5f);
						const float patch = Math::smoothstep(0.4f, 0.62f, moss_noise(hp * (1.0f / MAX(0.9f * cut_jr, 0.02f)), 0x666f6c65u));
						const float depth = Math::lerp(0.5f, 1.3f, moss_noise(hp * (1.0f / MAX(0.4f * cut_jr, 0.02f)), 0x666f6c66u));
						const float fold = Math::sin(float(Math::TAU) * (dc / (0.32f * cut_jr) + wob));
						// Sharp creases between soft rolls.
						const float crease = 1.0f - Math::abs(fold);
						const float shape = 0.6f * fold - 0.8f * crease * crease * crease;
						off += 0.04f * cut_jr * fade * patch * depth * shape;
						dark = MIN(dark, Math::lerp(1.0f, 0.75f, fade * patch * crease * crease * crease));
					}
				}
				w_any = 1.0f;
			}
			for (const VeteranFeature &f : veteran_features) {
				if (p_features == 3) {
					break;
				}
				// A basal hollow is the main stem's alone: the forks around it took a second dent
				// at the same bearing, and the two folded through each other.
				if (f.kind == VET_EPICORMIC || f.kind == VET_WOODPECKER || (f.kind == VET_BASAL_HOLLOW && p_features < 2)) {
					continue;
				}
				const float dh = (h - f.h) / f.half_h;
				if (Math::abs(dh) > 1.5f) {
					continue;
				}
				float da = Math::angle_difference(f.azimuth, az) * r / f.half_w;
				if (f.kind == VET_BASAL_HOLLOW) {
					// A basal hollow opens as a pointed arch, widest at the soil, where the rot
					// came up through the root collar.
					da /= 1.0f - 0.45f * CLAMP(dh, 0.0f, 1.0f);
				}
				const float e = Math::sqrt(dh * dh + da * da);
				if (e > 1.4f) {
					continue;
				}
				if (f.kind == VET_BURL) {
					// (1 - e^2)^2 meets the bark with no crease, and the lumps are broad: fine noise
					// on the tube's rings came out as crumpled facets, not a rounded burl.
					const float k0 = 1.0f - MIN(e * e, 1.0f);
					const float k = k0 * k0;
					// A burr is a cluster of knobs (each a bud trace that swelled), not one dome.
					const Vector3 bp = p_pts[ring] + n * r;
					const float lump = (0.85f + 0.3f * moss_noise(p_pts[ring] + n * r * 1.5f, 0x6275726cu)) *
							(0.7f + 0.6f * moss_noise(bp * (2.5f / MAX(f.half_w, 0.02f)), 0x6b6e6f62u));
					// Sized by the stem, not the root flare's swollen radius.
					off += f.amount * MIN(r, vet_r_stem) * k * lump;
					if (head_r > 0.0f && k > 0.0f) {
						// On a pollard's head every burr is crowded with knobs: a bare dome read as
						// a smooth bulge on the head's outline.
						float crease = 0.0f;
						const float kn = callus_knobs(bp, CLAMP(0.1f * head_r, 0.03f, 0.06f), 0.3f * head_r, 0.2f, 0x6b6e6235u, crease);
						off += Math::sqrt(k) * kn;
						dark = MIN(dark, Math::lerp(1.0f, 0.5f, Math::sqrt(k) * crease));
					}
					w_any = MAX(w_any, k > 0.0f ? 1.0f : 0.0f);
				} else {
					// A hollow: deep inside, the floor rough; a callus lip of new wood around it.
					// The wall falls away gradually from the rim into a deep, dark floor, and the
					// callus (woundwood, Shigo 1985) is a rounded roll over the edge, so the hole
					// has depth and a soft lip instead of a flat-sided slot.
					if (e < 1.1f) {
						// Broad, gentle roughness, and no deeper than half the stem's own radius:
						// sized by the flare's swollen radius and roughened finely, the floor
						// folded through itself in pale blades.
						const float rough = 0.9f + 0.2f * moss_noise(p_pts[ring] + n * r * 1.2f, 0x686f6cu);
						off -= MIN(f.amount, 0.5f) * MIN(r, vet_r_stem) * Math::smoothstep(1.0f, 0.45f, e) * rough;
						dark = MIN(dark, Math::lerp(1.0f, 0.12f, Math::smoothstep(0.95f, 0.5f, e)));
						// Past the rim the wall is rotten heartwood, red-brown, not bark.
						rot = MAX(rot, Math::smoothstep(1.0f, 0.75f, e));
					}
					// The callus roll stands on the rim and curls in over it: centred just inside
					// the opening, broad enough to read as a rounded lip, not a ridge line.
					const float lip = (e - 0.97f) / 0.2f;
					off += 0.16f * MIN(r, vet_r_stem) * Math::exp(-lip * lip);
					w_any = 1.0f;
				}
			}
			if (w_any > 0.0f) {
				if (!p_colour) {
					// No moss on the rotten wall: the shell sinks under the bark past the rim.
					off -= 0.08f * rot;
				}
				vv[v] += n * off + hdir * hoff;
				if (p_colour) {
					Color c = cc[v].lerp(Color(0.085f, 0.048f, 0.028f, cc[v].a), rot);
					c = c.lerp(Color(0.13f, 0.16f, 0.07f, c.a), algae);
					c = c.lerp(Color(0.3f, 0.31f, 0.27f, c.a), pale);
					cc[v] = Color(c.r * dark, c.g * dark, c.b * dark, c.a);
				}
				touched[i] = 1.0f;
				any = true;
			}
		}
		if (!any) {
			return;
		}
		// Normals of the touched vertices from the faces around them; a ring's seam pair shares one.
		LocalVector<Vector3> acc;
		acc.resize(p_part.w);
		for (Vector3 &a : acc) {
			a = Vector3();
		}
		const int *ix = r_b.indices.ptr();
		for (int q = p_part.x; q + 2 < p_part.x + p_part.y; q += 3) {
			const int a = ix[q] - p_part.z, b = ix[q + 1] - p_part.z, c = ix[q + 2] - p_part.z;
			if (a < 0 || b < 0 || c < 0 || a >= p_part.w || b >= p_part.w || c >= p_part.w) {
				continue;
			}
			const Vector3 fn = (vv[p_part.z + c] - vv[p_part.z + a]).cross(vv[p_part.z + b] - vv[p_part.z + a]);
			acc[a] += fn;
			acc[b] += fn;
			acc[c] += fn;
		}
		for (int i = 0; i < p_part.w; i += p_sides + 1) {
			if (i + p_sides < p_part.w) {
				const Vector3 sum = acc[i] + acc[i + p_sides];
				acc[i] = sum;
				acc[i + p_sides] = sum;
			}
		}
		// The winding's sign, once for the part from the untouched vertices: flipped vertex by
		// vertex against the old normal, a hollow's steep side walls (turned near 90 degrees from
		// the bark's outward normal) flipped at random and shaded as ragged dark and sky-blue
		// patches under the path tracer.
		float sign_sum = 0.0f;
		for (int i = 0; i < p_part.w; i++) {
			if (touched[i] <= 0.0f && acc[i].length_squared() > 1e-16f) {
				sign_sum += acc[i].normalized().dot(nn[p_part.z + i]);
			}
		}
		const float wsign = sign_sum < 0.0f ? -1.0f : 1.0f;
		for (int i = 0; i < p_part.w; i++) {
			// Touched vertices and their ring neighbours, so the edge of a feature shades smoothly.
			const bool near = touched[i] > 0.0f || (i > 0 && touched[i - 1] > 0.0f) || (i + 1 < p_part.w && touched[i + 1] > 0.0f) ||
					(i >= p_sides + 1 && touched[i - p_sides - 1] > 0.0f) || (i + p_sides + 1 < p_part.w && touched[i + p_sides + 1] > 0.0f);
			if (near && acc[i].length_squared() > 1e-16f) {
				const Vector3 fnrm = acc[i].normalized() * wsign;
				nn[p_part.z + i] = fnrm;
				// The tangent square to the new normal, or the bark's normal map tips on the walls.
				if (r_b.use_tangents && int(r_b.tangents.size()) >= (p_part.z + i + 1) * 4) {
					float *tw = r_b.tangents.ptrw() + (p_part.z + i) * 4;
					Vector3 tg(tw[0], tw[1], tw[2]);
					tg -= fnrm * fnrm.dot(tg);
					if (tg.length_squared() < 1e-10f) {
						tg = fnrm.get_any_perpendicular();
					}
					tg.normalize();
					tw[0] = tg.x;
					tw[1] = tg.y;
					tw[2] = tg.z;
				}
			}
		}
	};
	// A log's broken butt keeps the colour of weathered end grain after the moss tint (the face
	// is the tube's rings within the cap's length of its start; trees.md 5d).
	auto end_grain = [&](Builder &r_b, const Vector4i &p_part, const LocalVector<Vector3> &p_pts, int p_sides, float p_girth_r, const Color &p_col) {
		if (p_girth_r <= 0.0f || p_pts.is_empty()) {
			return;
		}
		// Weathered grey-brown end grain, the heart darker with rot, blotched by stain and the
		// splinters' fresher faces.
		const Color grain = p_col.lerp(Color(0.17f, 0.135f, 0.095f), 0.75f);
		const int ring = p_sides + 1;
		for (int r = 0; r < int(p_pts.size()) && p_pts[r].distance_to(p_pts[0]) < p_girth_r * 0.15f - 1e-4f; r++) {
			for (int k = 0; k < ring && r * ring + k < p_part.w; k++) {
				const int v = p_part.z + r * ring + k;
				const Vector3 q = r_b.verts[v];
				const float rr = CLAMP(Vector2(q.x - p_pts[0].x, q.z - p_pts[0].z).length() / p_girth_r, 0.0f, 1.0f);
				const float heart = Math::lerp(0.62f, 1.0f, Math::smoothstep(0.1f, 0.7f, rr));
				const float blot = 0.75f + 0.5f * moss_noise(q * 9.0f, 0x6772616eu);
				r_b.colors.write[v] = Color(grain.r * heart * blot, grain.g * heart * blot, grain.b * heart * blot, grain.a);
			}
		}
		// The fibre tongues stand the face's rings at different depths, so its walls are steep
		// while the face kept the planar normal: under the path tracer the shading normal then lay
		// below the walls' surface and they rendered black. Normals from the displaced face.
		int face_rings = 0;
		while (face_rings < int(p_pts.size()) && p_pts[face_rings].distance_to(p_pts[0]) < p_girth_r * 0.15f - 1e-4f) {
			face_rings++;
		}
		LocalVector<Vector3> fn;
		fn.resize(face_rings * ring);
		const Vector3 *vv = r_b.verts.ptr();
		const Vector3 *nv = r_b.normals.ptr();
		for (int r = 1; r < face_rings; r++) {
			const int ra = r - 1, rb = MIN(r + 1, int(p_pts.size()) - 1);
			for (int k = 0; k < ring; k++) {
				const int km = (k + p_sides - 1) % p_sides, kp = (k + 1) % p_sides;
				const Vector3 du = vv[p_part.z + r * ring + kp] - vv[p_part.z + r * ring + km];
				const Vector3 dr = vv[p_part.z + rb * ring + k] - vv[p_part.z + ra * ring + k];
				Vector3 n = du.cross(dr);
				const Vector3 n0 = nv[p_part.z + r * ring + k];
				if (n.length_squared() < 1e-12f) {
					n = n0;
				} else {
					n.normalize();
					if (n.dot(n0) < 0.0f) {
						n = -n;
					}
				}
				fn[r * ring + k] = n;
			}
		}
		Vector3 *nw = r_b.normals.ptrw();
		for (int r = 1; r < face_rings; r++) {
			for (int k = 0; k < ring && r * ring + k < p_part.w; k++) {
				const int v = p_part.z + r * ring + k;
				nw[v] = fn[r * ring + k];
				// Keep the tangent square to the new normal, or the normal map's frame skews
				// and tips the shading normal into the wall.
				if (r_b.use_tangents && int(r_b.tangents.size()) >= (v + 1) * 4) {
					float *tw = r_b.tangents.ptrw() + v * 4;
					Vector3 tg(tw[0], tw[1], tw[2]);
					tg = tg - nw[v] * nw[v].dot(tg);
					if (tg.length_squared() < 1e-10f) {
						tg = nw[v].get_any_perpendicular();
					}
					tg.normalize();
					tw[0] = tg.x;
					tw[1] = tg.y;
					tw[2] = tg.z;
				}
			}
		}
	};
	LocalVector<Vector3> pts;
	LocalVector<float> rad;
	LocalVector<int> chain;
	// Extra rings along a tube wherever p_step (metres at a point, given its radius) asks for
	// them; the radius is interpolated.
	auto densify = [&](LocalVector<Vector3> &r_p, LocalVector<float> &r_r, auto p_step) {
		LocalVector<Vector3> dp;
		LocalVector<float> dr;
		for (int i = 0; i < int(r_p.size()); i++) {
			dp.push_back(r_p[i]);
			dr.push_back(r_r[i]);
			if (i + 1 < int(r_p.size())) {
				const Vector3 mid = r_p[i].lerp(r_p[i + 1], 0.5f);
				const float st = MIN(MIN(p_step(r_p[i], r_r[i]), p_step(r_p[i + 1], r_r[i + 1])), p_step(mid, 0.5f * (r_r[i] + r_r[i + 1])));
				const float len = r_p[i].distance_to(r_p[i + 1]);
				if (st < len) {
					const int steps = MIN(int(Math::ceil(len / MAX(st, 0.002f))), 400);
					for (int k = 1; k < steps; k++) {
						const float f = float(k) / steps;
						dp.push_back(r_p[i].lerp(r_p[i + 1], f));
						dr.push_back(Math::lerp(r_r[i], r_r[i + 1], f));
					}
				}
			}
		}
		r_p = dp;
		r_r = dr;
	};
	// The lying stem's own bark and moss (deadwood interface): vertex ranges for _measure_lying().
	int dw_v0 = -1, dw_v1 = -1, dw_m0 = -1, dw_m1 = -1;
	for (int s = 1; s < int(nodes.size()); s++) {
		const Node &first = nodes[s];
		if (first.removed || (first.parent > 0 && nodes[first.parent].main == s)) {
			continue;
		}
		if (dw_v0 >= 0 && dw_v1 < 0) {
			dw_v1 = int(bark.verts.size());
			dw_m1 = int(moss.verts.size());
		}
		if (s == deadwood_stem) {
			dw_v0 = int(bark.verts.size());
			dw_m0 = int(moss.verts.size());
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
			// A pollard's head or a stool's stub ends full-girth in its callus knob (trees.md 5l).
			float r_end = c + 1 < int(chain.size()) ? nodes[chain[c + 1]].radius : n.radius * (bolling.has(chain[c]) || cut_stubs.has(chain[c]) ? 1.0f : 0.6f);
			// Where a lateral has outgrown the axis's own continuation (a leader overtaken at a
			// fork), the axis keeps the lateral's girth to the fork: narrowing to the thinner
			// continuation, it left the lateral's base standing out as a stub.
			for (int l = n.first_lateral; l >= 0; l = nodes[l].next_sibling) {
				if (!nodes[l].removed) {
					r_end = MAX(r_end, MIN(nodes[l].radius, n.radius));
				}
			}
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
		// A hanger's base is a fracture hanging beside its stub, not a junction: no branch
		// collar, fork crotch or weld into the parent.
		const bool hanger = hanger_bases.has(s);
		// The root collar (flare and sink) belongs to a stem standing in the soil. A log or a
		// fallen limb lies along the ground, every ring of it near the base height: the flare
		// swelled the whole log and the sink dove its open butt into the soil, where it showed
		// from above as a log with its top missing.
		const bool collar = stem && flare > 0.0f && !fallen && !fallen_branch;
		if (!stem && !hanger && r0 > 0.01f && pts.size() >= 2) {
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
			// No swell on steep limbs: weld() leaves them as crotches, where a swollen base would
			// poke out of the parent's bark.
			float swell = 0.2f * CLAMP(ratio * 2.5f, 0.2f, 1.0f) * (1.0f - Math::smoothstep(0.35f, 0.6f, ratio)) *
					Math::smoothstep(0.45f, 0.6f, first.dir.cross(par.dir).length());
			if (first.pole && bolling.has(first.parent)) {
				// A pole swells where it leaves its knuckle (trees.md 5l): its base is a socket of
				// callus, whatever its angle; the knuckle is fat enough to take it.
				swell = 0.3f;
			}
			const float seg = pts[0].distance_to(pts[1]);
			LocalVector<Vector3> cp;
			LocalVector<float> cr;
			cp.push_back(pts[0]);
			cr.push_back(rad[0] * (1.0f + swell));
			// Limbs seen up close (3 cm and up) get more rings over the weld, as SpeedTree's
			// weld segments: the spread of the base down the parent (weld()) needs them to bend
			// smoothly. Thinner limbs keep the few rings they had.
			const bool fine = r0 >= 0.03f;
			const float marks_fine[8] = { 0.85f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
			const float after_fine[8] = { 0.0f, 0.0f, 0.2f, 0.45f, 0.8f, 1.3f, 2.0f, 3.0f };
			const float marks_coarse[5] = { 0.85f, 1.0f, 1.0f, 1.0f, 1.0f };
			const float after_coarse[5] = { 0.0f, 0.0f, 0.4f, 1.0f, 2.0f };
			const float *marks = fine ? marks_fine : marks_coarse;
			const float *after = fine ? after_fine : after_coarse;
			for (int k = 0; k < (fine ? 8 : 5); k++) {
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
		// Forks (a limb over 0.6 of its parent's girth, which weld() leaves as a crotch) started
		// on the parent's axis with their first ring tilted to their own heading: where the
		// parent narrowed past the fork, that ring's far side stood out as an open rim, a sawn
		// stub. As in a branching-tube mesh (Lluch et al. 2004; SpeedTree's fork splits), the
		// fork's base now runs up inside the parent, its rings parallel to the parent's and
		// shifted toward the side it leaves on, then turns out through the crotch.
		if (!stem && !hanger && pts.size() >= 2 && r0 > 0.6f * nodes[first.parent].radius) {
			const Node &par = nodes[first.parent];
			const float rp = par.radius;
			Vector3 side = first.dir - par.dir * first.dir.dot(par.dir);
			side = side.length() > 1e-4f ? side.normalized() : Vector3();
			const float rc = MIN(rad[0], rp * 0.9f);
			const float off = MAX(0.0f, rp * 0.92f - rc);
			const Vector3 base = pts[0] + side * off;
			LocalVector<Vector3> cp;
			LocalVector<float> cr;
			const float ks[3] = { 1.2f, 0.7f, 0.25f };
			// A fork low on the stem must not run down out of the ground: its open start
			// showed beside the root flare.
			const float room = MAX(0.0f, (base.y - nodes[0].start.y - 0.1f) / MAX(par.dir.y, 0.2f));
			const float drop = MIN(rc * ks[0], room) / ks[0];
			for (int k = 0; k < 3; k++) {
				cp.push_back(base - par.dir * (drop * ks[k]));
				cr.push_back(rc);
			}
			// The collar's extra rings near the axis would pull the turn back to the axis.
			uint32_t i = 1;
			while (i + 1 < pts.size() && pts[i].distance_to(pts[0]) < rp * 0.8f) {
				i++;
			}
			for (; i < pts.size(); i++) {
				cp.push_back(pts[i]);
				cr.push_back(rad[i]);
			}
			pts = cp;
			rad = cr;
		}
		// The tube is open at its end. Thin tips close to under a millimetre on their own; an
		// axis that stops thick (a shed or snapped continuation, a pruned limb) would show its
		// hollow inside as a flat dark disc. It ends in a rounded callus instead (wound wood
		// rolls over a stub; Shigo 1985), or under the splinters of a break.
		Vector3 rim_pt = pts[pts.size() - 1];
		float rim_r = rad[rad.size() - 1];
		int cap_n = 3; // Rings closing the end.
		if (rim_r >= 0.006f && pts.size() >= 2) {
			const Vector3 dir_end = (pts[pts.size() - 1] - pts[pts.size() - 2]).normalized();
			const bool broken = (broken_tip >= 0 && chain[chain.size() - 1] == broken_tip) || broken_ends.has(chain[chain.size() - 1]) || cut_stubs.has(chain[chain.size() - 1]) || _coppice_lobe(chain[chain.size() - 1]);
			const float cap_len = broken ? 0.04f : 0.8f; // Under the fracture face, which closes the end.
			const float ks[3] = { 0.4f, 0.75f, 1.0f };
			const float rs[3] = { 0.93f, 0.68f, 0.0f };
			// A coppice lobe's old cut: the callus has rolled over its rim and stands proud of the
			// sunken face (Shigo 1985), a lip round a dark, rotted centre.
			const float roll_k[3] = { 0.1f, 0.18f, 0.24f };
			const float roll_r[3] = { 1.05f, 0.88f, 0.6f };
			const bool lobe = _coppice_lobe(chain[chain.size() - 1]);
			const int ce = chain[chain.size() - 1];
			if (!broken && cut_height > 0.5f && cut_heads.has(ce)) {
				// A pollard knuckle whose poles are all gone: a broad callus dome, knobbed like
				// the head (veteran_deform). On three rings its last fan of long triangles shaded
				// as a dark pinwheel.
				cap_n = 8;
				for (int k = 1; k <= cap_n; k++) {
					const float t = float(k) / cap_n * float(Math::PI) * 0.5f;
					pts.push_back(rim_pt + dir_end * (rim_r * 0.55f * Math::sin(t)));
					rad.push_back(rim_r * (k == cap_n ? 0.0f : Math::cos(t)));
				}
			} else {
				for (int k = 0; k < 3; k++) {
					pts.push_back(rim_pt + dir_end * (rim_r * (lobe ? roll_k[k] : cap_len * ks[k])));
					rad.push_back(rim_r * (lobe ? roll_r[k] : rs[k]));
				}
			}
		}
		if (head_r > 0.0f && s == veteran_stem) {
			// Rings every 4 cm over a pollard's head, for its knobs and fissures (veteran_deform).
			LocalVector<Vector3> dp;
			LocalVector<float> dr;
			for (int i = 0; i < int(pts.size()); i++) {
				dp.push_back(pts[i]);
				dr.push_back(rad[i]);
				if (i + 1 < int(pts.size()) && MAX(pts[i].y, pts[i + 1].y) > head_y0) {
					const int steps = int(Math::ceil(pts[i].distance_to(pts[i + 1]) / 0.04f));
					for (int k = 1; k < steps; k++) {
						const float f = float(k) / steps;
						dp.push_back(pts[i].lerp(pts[i + 1], f));
						dr.push_back(Math::lerp(rad[i], rad[i + 1], f));
					}
				}
			}
			pts = dp;
			rad = dr;
		}
		// A cut tree's poles and stool (trees.md 5l): their year boundaries, and the swollen base
		// where a pole leaves its knuckle, with rings enough for the flare to be round.
		cut_scars.clear();
		cut_jr = 0.0f;
		s_chain_salt = s;
		const bool head_chain = cut_tree && head_r > 0.0f && s != veteran_stem && first.start.distance_to(head_c) < 1.9f * head_r;
		const bool stool_chain = cut_tree && stool_top > 0.0f && first.start.y - vet_base_y < stool_top;
		const bool cut_chain = cut_tree && (first.pole || bolling.has(s) || head_chain || stool_chain);
		if (cut_chain) {
			for (int c = 1; c < int(chain.size()); c++) {
				const Node &a = nodes[chain[c - 1]], &b = nodes[chain[c]];
				if (a.pole && b.pole && a.born != b.born) {
					cut_scars.push_back(b.start);
				}
			}
			if (first.pole && first.parent > 0 && bolling.has(first.parent)) {
				cut_j = first.start;
				cut_jdir = first.dir;
				cut_jr = first.radius;
			} else {
				for (int c = 0; c + 1 < int(chain.size()); c++) {
					const Node &b = nodes[chain[c + 1]];
					if (bolling.has(chain[c]) && !bolling.has(chain[c + 1]) && b.pole) {
						cut_j = b.start;
						cut_jdir = b.dir;
						cut_jr = b.radius;
						break;
					}
				}
				if (cut_jr > 0.0f) {
					// The knuckle's own continuation: swell the pole's first radii out of it, as the
					// branch collar does for a lateral one.
					densify(pts, rad, [&](const Vector3 &p_p, float) {
						const float d = (p_p - cut_j).dot(cut_jdir);
						return d > -0.5f * cut_jr && d < 3.0f * cut_jr ? 0.3f * cut_jr : 1e9f;
					});
					for (uint32_t i = 0; i < pts.size(); i++) {
						const float d = (pts[i] - cut_j).dot(cut_jdir);
						if (d >= -1e-4f && d < 4.0f * cut_jr && pts[i].distance_to(cut_j) < 5.0f * cut_jr) {
							rad[i] *= 1.0f + 0.3f * Math::exp(-MAX(d, 0.0f) / (0.8f * cut_jr));
						}
					}
				}
			}
		}
		if (collar) {
			// Sink the base below the ground line, deeper on a leaning stem: the base ring lies
			// square to the stem, and on a lean its high side rose out of the ground, an open rim.
			// Deep enough for the rings swept out into the roots to end under the soil.
			{
				const Vector3 d0 = pts.size() >= 2 ? (pts[1] - pts[0]).normalized() : Vector3(0, 1, 0);
				const float lean = Math::sqrt(MAX(0.0f, 1.0f - d0.y * d0.y));
				pts[0] -= Vector3(0, (roots_on ? root_sink : 0.15f) + rad[0] * (1.0f + flare) * 1.15f * lean, 0);
			}
			// Rings every 7 cm up to above the roots, which are swept out of them: the internodes
			// left two rings over the whole root zone.
			{
				LocalVector<Vector3> dp;
				LocalVector<float> dr;
				for (int i = 0; i < int(pts.size()); i++) {
					dp.push_back(pts[i]);
					dr.push_back(rad[i]);
					if (i + 1 < int(pts.size()) && MIN(pts[i].y, pts[i + 1].y) - nodes[0].start.y < root_top + 0.2f) {
						const int steps = int(Math::ceil(pts[i].distance_to(pts[i + 1]) / 0.07f));
						for (int k = 1; k < steps; k++) {
							const float f = float(k) / steps;
							dp.push_back(pts[i].lerp(pts[i + 1], f));
							dr.push_back(Math::lerp(rad[i], rad[i + 1], f));
						}
					}
				}
				pts = dp;
				rad = dr;
			}
			for (int i = 0; i < int(pts.size()); i++) {
				const float h = MAX(0.0f, pts[i].y - nodes[0].start.y);
				// The root collar: over the last ~20 cm the flare splays out into the roots, so
				// the bark runs into the ground at a shallow angle instead of standing on it as
				// a wall with a hard rim (the buttress roots continue under the surface).
				rad[i] *= (1.0f + flare * Math::exp(-h / MAX(0.05f, trunk_r * 2.5f))) * (1.0f + 0.15f * flare * Math::exp(-h / 0.12f));
			}
		}
		float girth_r = -1.0f; // The bark tiles' radius when the tube starts in a cap.
		if (stem && (fallen || fallen_branch) && pts.size() >= 2 && rad[0] >= 0.006f) {
			// A standing stem's base is under the soil; a log's or a fallen limb's lies in the
			// open and showed the tube's hollow inside. It closes under a short broken face, as
			// the snapped tip does.
			const Vector3 d0 = (pts[1] - pts[0]).normalized();
			const Vector3 p0 = pts[0];
			const float r_base = rad[0];
			girth_r = r_base;
			// Six rings, so the fibre tongues of the break (_tube) have room across the face.
			const float ks[6] = { 1.0f, 0.97f, 0.9f, 0.78f, 0.6f, 0.35f };
			const float rs[6] = { 0.0f, 0.25f, 0.5f, 0.7f, 0.85f, 0.96f };
			LocalVector<Vector3> cp;
			LocalVector<float> cr;
			for (int k = 0; k < 6; k++) {
				cp.push_back(p0 - d0 * (r_base * 0.15f * ks[k]));
				cr.push_back(r_base * rs[k]);
			}
			for (uint32_t i = 0; i < pts.size(); i++) {
				cp.push_back(pts[i]);
				cr.push_back(rad[i]);
			}
			pts = cp;
			rad = cr;
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
		// Thin shoots keep a ring at every internode, though most run nearly straight: under a
		// centimetre of radius the axis is simplified (Douglas and Peucker 1973, as hair strands
		// are) where dropping a ring moves the axis and the radius by under a quarter of the radius.
		// An open oak has some 200,000 shoots of three rings each; they were half its bark.
		if (r0 < 0.01f && !collar && pts.size() > 2) {
			LocalVector<uint8_t> keep;
			keep.resize(pts.size());
			for (uint32_t i = 0; i < pts.size(); i++) {
				keep[i] = 0;
			}
			keep[0] = keep[pts.size() - 1] = 1;
			LocalVector<Vector2i> todo;
			todo.push_back(Vector2i(0, pts.size() - 1));
			while (!todo.is_empty()) {
				const Vector2i span = todo[todo.size() - 1];
				todo.resize(todo.size() - 1);
				const Vector3 a = pts[span.x], b = pts[span.y];
				const float len = a.distance_to(b);
				int worst = -1;
				float worst_e = 0.0f;
				for (int i = span.x + 1; i < span.y; i++) {
					const float t = len > 1e-6f ? CLAMP((pts[i] - a).dot(b - a) / (len * len), 0.0f, 1.0f) : 0.0f;
					const float d = pts[i].distance_to(a.lerp(b, t));
					const float dr = Math::abs(rad[i] - Math::lerp(rad[span.x], rad[span.y], t));
					const float e = MAX(d, dr) / MAX(0.25f * rad[i], 0.0003f);
					if (e > worst_e) {
						worst_e = e;
						worst = i;
					}
				}
				if (worst >= 0 && worst_e > 1.0f) {
					keep[worst] = 1;
					todo.push_back(Vector2i(span.x, worst));
					todo.push_back(Vector2i(worst, span.y));
				}
			}
			LocalVector<Vector3> kp;
			LocalVector<float> kr;
			for (uint32_t i = 0; i < pts.size(); i++) {
				if (keep[i]) {
					kp.push_back(pts[i]);
					kr.push_back(rad[i]);
				}
			}
			pts = kp;
			rad = kr;
		}
		int sides = CLAMP(int(Math::ceil(Math::TAU * r0 / 0.03f)), 3, 24);
		if (stem) {
			sides = MAX(sides, 10);
		}
		if (collar && roots_on) {
			// Enough sides round the foot to carry the roots swept out of it.
			sides = MAX(sides, 48);
		}
		// The preset bark colours (and bark_color, an inspector colour) are sRGB, as measured bark
		// is quoted; the bark material reads vertex colour as linear, so they convert here. Read
		// as linear, beech came out near 0.45 albedo and birch 0.74, about twice real bark (beech
		// about 0.15 to 0.2, birch about 0.5), and logs and trunks rendered almost white.
		Color col = bark_color;
		if (preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH || preset == PRESET_YELLOW_BIRCH) {
			// A birch's white (or yellow) bark comes with girth: its twigs and thin shoots are dark
			// red-brown and glossy, turning pale from about 2-4 cm (Rushforth, Trees of Britain and
			// Europe). Thin shoots took the trunk's white and read as frosted sprays.
			col = Color(0.3f, 0.18f, 0.14f).lerp(col, Math::smoothstep(0.008f, 0.03f, r0));
		}
		if (first.dead) {
			col = col.lerp(Color(0.42f, 0.4f, 0.37f), 0.6f);
		}
		col = col.srgb_to_linear();
		const float tint = 0.92f + 0.16f * float((uint32_t(s) * 2654435761u) >> 24) / 255.0f;
		col = Color(col.r * tint, col.g * tint, col.b * tint);
		bark.bone = node_bone[s];
		const BarkSet &bs = _bark_set(_bark_kind(preset));
		const float scale = bark_relief * CLAMP(r0 / 0.35f, 0.25f, 1.0f);
		// A cut tree's poles and stool get a near tube however thin, for their modelled relief.
		const bool near_tube = bark_relief > 0.0f && (r0 >= RELIEF_MIN_RADIUS || (cut_chain && r0 >= 0.012f));
		// Displaced only where that shows (a millimetre or more nearer the relief than a smooth
		// tube); otherwise LOD 0 gets a smooth tube with sides for a millimetre of chord error.
		// A knuckle's chain runs on into its pole, as thin as a withy: the map's relief, set by
		// the knuckle's girth, would furrow it; the knuckle takes the head's knobs instead.
		const bool relief = near_tube && r0 >= RELIEF_MIN_RADIUS && !(cut_chain && s != veteran_stem && (bolling.has(s) || first.pole)) && bs.relief_gain * scale >= 0.001f;
		const int vf = s == veteran_stem ? 2 : (vet_share[s] ? 1 : (cut_chain ? 3 : 0));
		// A mesh without LODs (the near variants) carries the near tube alone.
		if (!(near_tube && skip_lods)) {
			Vector4i part;
			bark.role = near_tube ? Builder::ROLE_PLAIN : Builder::ROLE_ALL;
			bark.begin(part);
			_tube(bark, pts, rad, sides, 0.0f, col, nullptr, 0.0f, girth_r);
			bark.end(part, r0);
			if (collar) {
				root_collar(bark, part, sides, pts, rad, false);
			}
			moss_tint(bark, part);
			lichen_tint(bark, part, r0, float(Math::TAU) * (girth_r > 0.0f ? girth_r : rad[0]), uint32_t(s));
			end_grain(bark, part, pts, sides, girth_r, col);
			if (collar) {
				soil_tint(bark, part, 1.0f);
			}
			if (rigged) {
				_skin_stem(bark, part, s, pts, sides);
			}
			if (s == veteran_stem || vet_share[s]) {
				veteran_deform(bark, part, pts, rad, sides, true, s == veteran_stem ? 2 : (vet_share[s] ? 1 : 0));
			}
			if (!stem && !hanger && junction_weld) {
				weld(bark, part, s, pts, rad, sides);
			}
		}
		const bool moss_shell = moss_k > 0.05f && r0 >= MOSS_SHELL_MIN_RADIUS;
		if (near_tube || moss_shell) {
			// LOD 0 and the path tracer: the same tube at about 2.5 cm around and 5 cm along,
			// displaced by the bark's relief. Bark thickens with girth, so thin limbs carry a
			// quarter of a mature trunk's furrow depth (Kratt et al. 2015, cambial growth).
			auto make_rings = [&](float p_step, LocalVector<Vector3> &r_p, LocalVector<float> &r_r) {
				for (int i = 0; i + 1 < int(pts.size()); i++) {
					const int steps = MAX(1, int(Math::ceil(pts[i].distance_to(pts[i + 1]) / p_step)));
					for (int k = 0; k < steps; k++) {
						const float t = float(k) / steps;
						r_p.push_back(pts[i].lerp(pts[i + 1], t));
						r_r.push_back(Math::lerp(rad[i], rad[i + 1], t));
					}
				}
				r_p.push_back(pts[pts.size() - 1]);
				r_r.push_back(rad[rad.size() - 1]);
			};
			LocalVector<Vector3> rp;
			LocalVector<float> rr;
			if (vf == 3) {
				// A pole's rings come from cut_step below, by its girth.
				rp = pts;
				rr = rad;
			} else {
				make_rings(0.05f, rp, rr);
			}
			int rsides = CLAMP(int(Math::ceil(Math::TAU * r0 / 0.025f)), 24, 128);
			if (vf == 3) {
				// Enough sides for the flutes, a few round a pole.
				rsides = CLAMP(int(Math::ceil(Math::TAU * r0 / 0.01f)), 12, 32);
			}
			// A cut tree's modelled wood: rings about a centimetre apart over a pollard's head and
			// a stool, closer over a girdle scar and the folds at a pole's base; as many sides.
			const bool knobbly = (head_r > 0.0f && (s == veteran_stem || head_chain)) || stool_chain;
			auto cut_step = [&](const Vector3 &p_p, float p_r) -> float {
				float st = 1e9f;
				if (head_r > 0.0f && (s == veteran_stem || head_chain) && p_p.y > head_y0 && (s == veteran_stem || (p_p.distance_to(head_c) < 2.0f * head_r && (cut_jr <= 0.0f || (p_p - cut_j).dot(cut_jdir) < 1.4f * cut_jr)))) {
					st = s == veteran_stem ? 0.013f : 0.016f;
				}
				if (stool_chain && p_p.y - vet_base_y < stool_top + 0.08f) {
					st = MIN(st, 0.018f);
				}
				if (vf == 3) {
					// Thick poles every 5 cm, for their branch scars.
					st = MIN(st, p_r > 0.05f ? 0.05f : CLAMP(4.0f * p_r, 0.05f, 0.2f));
					const float sw = MAX(0.2f * p_r, 0.004f);
					for (const Vector3 &sp : cut_scars) {
						if (p_p.distance_to(sp) < 3.0f * sw && p_r > 0.006f) {
							st = MIN(st, sw);
						}
					}
					if (cut_jr > 0.0f) {
						const float dc = (p_p - cut_j).dot(cut_jdir);
						if (dc > -0.2f * cut_jr && dc < 2.7f * cut_jr) {
							st = MIN(st, 0.07f * cut_jr);
						}
					}
				}
				return st;
			};
			if (vf == 3 && rp.size() > uint32_t(cap_n) + 2) {
				// Resampled along the chain at the step asked for: a hazel rod's internodes are a
				// few centimetres, and a ring at each of them doubled its triangles for nothing.
				LocalVector<Vector3> qp;
				LocalVector<float> qr;
				qp.push_back(rp[0]);
				qr.push_back(rr[0]);
				float since = 0.0f;
				const uint32_t body = rp.size() - cap_n; // The last rings close the end (a cap).
				for (uint32_t i = 0; i + 1 < body; i++) {
					const float len = rp[i].distance_to(rp[i + 1]);
					float t = 0.0f;
					while (len > 1e-6f) {
						const Vector3 at = rp[i].lerp(rp[i + 1], t / len);
						const float want = cut_step(at, Math::lerp(rr[i], rr[i + 1], t / len));
						const float need = want - since;
						if (t + need >= len) {
							since += len - t;
							break;
						}
						t += need;
						since = 0.0f;
						qp.push_back(rp[i].lerp(rp[i + 1], t / len));
						qr.push_back(Math::lerp(rr[i], rr[i + 1], t / len));
					}
				}
				if (qp[qp.size() - 1].distance_to(rp[body - 1]) < 0.3f * MAX(cut_step(rp[body - 1], rr[body - 1]), 0.01f) && qp.size() > 1) {
					qp.resize(qp.size() - 1);
					qr.resize(qr.size() - 1);
				}
				for (uint32_t i = body - 1; i < rp.size(); i++) {
					qp.push_back(rp[i]);
					qr.push_back(rr[i]);
				}
				rp = qp;
				rr = qr;
			} else if (vf >= 2 && cut_tree) {
				densify(rp, rr, cut_step);
			}
			// A knuckle or stool lobe runs on into its pole: the knobs need a centimetre between
			// sides, the pole a fraction of that. The chain is built in two tubes, crossing over
			// three rings past where the knobs have faded, each with its own sides.
			int split = -1;
			if (knobbly && s != veteran_stem && cut_jr > 0.0f) {
				for (uint32_t i = 0; i < rp.size(); i++) {
					if ((rp[i] - cut_j).dot(cut_jdir) > 1.6f * cut_jr && i >= 3 && i + 3 < rp.size()) {
						split = int(i);
						break;
					}
				}
			}
			LocalVector<Vector3> full_rp;
			LocalVector<float> full_rr;
			full_rp = rp;
			full_rr = rr;
			const int pieces = split > 0 ? 2 : 1;
			cut_arc0 = 0.0f;
			for (int piece = 0; piece < pieces; piece++) {
				if (split > 0) {
					// Piece 0 to three rings past the split, sinking inside piece 1 over the last two;
					// piece 1 from one ring before it, starting inside piece 0.
					rp.clear();
					rr.clear();
					const int a = piece == 0 ? 0 : split - 1, b = piece == 0 ? split + 2 : int(full_rp.size()) - 1;
					for (int i = a; i <= b; i++) {
						rp.push_back(full_rp[i]);
						float f = 1.0f;
						if (piece == 0) {
							f = i == split + 1 ? 0.9f : (i == split + 2 ? 0.75f : 1.0f);
						} else if (i == split - 1) {
							f = 0.9f;
						}
						rr.push_back(full_rr[i] * f);
					}
					cut_arc0 = 0.0f;
					for (int i = 1; i <= a; i++) {
						cut_arc0 += full_rp[i].distance_to(full_rp[i - 1]);
					}
					if (piece == 1) {
						tube_carry_nrm = tube_last_nrm;
					}
				}
				if (knobbly) {
					float rmax = 0.0f;
					for (uint32_t i = 0; i < rr.size(); i++) {
						rmax = MAX(rmax, rr[i]);
					}
					rsides = piece == 1 ? CLAMP(int(Math::ceil(Math::TAU * rr[1] / 0.01f)), 12, 32) : CLAMP(int(Math::ceil(Math::TAU * rmax * (s == veteran_stem ? 1.3f / 0.013f : (head_chain ? 1.2f / 0.016f : 1.15f / 0.018f)))), 24, s == veteran_stem ? 256 : 128);
				}
				if (near_tube) {
					// The bark's own grid. The moss shell reads the bark's displacement at its vertices,
					// so under a shell the two share one grid; elsewhere a displaced tube takes rings as
					// far apart as its pattern allows along the stem (bs.relief_along), and a smooth one
					// sides for a millimetre of chord error and rings every 10 cm.
					LocalVector<Vector3> bp;
					LocalVector<float> br;
					int bsides = rsides;
					if (moss_shell || split > 0 || vf == 3) {
						// Smooth bark too: on a coarser bark grid the shell's edge, where it meets the
						// bark, ran straight along the bark's long triangles.
						bp = rp;
						br = rr;
					} else if (relief) {
						make_rings(MAX(0.05f, bs.relief_along), bp, br);
					} else {
						make_rings(0.1f, bp, br);
						bsides = CLAMP(int(Math::ceil(float(Math::PI) / Math::acos(1.0f - 0.001f / MAX(r0, 0.001f)))), 24, rsides);
					}
					if (!moss_shell && split < 0 && vf == 2 && cut_tree) {
						densify(bp, br, cut_step);
					}
					if (knobbly) {
						bsides = rsides;
					}
					Vector4i part;
					bark.role = skip_lods ? Builder::ROLE_ALL : Builder::ROLE_RELIEF;
					bark.begin(part);
					_tube(bark, bp, br, bsides, 0.0f, col, relief ? bs.height.ptr() : nullptr, relief ? scale : 0.0f, girth_r, !(fallen || fallen_branch));
					bark.end(part, r0);
					if (collar) {
						root_collar(bark, part, bsides, bp, br, false);
					}
					moss_tint(bark, part);
					lichen_tint(bark, part, r0, float(Math::TAU) * (girth_r > 0.0f ? girth_r : br[0]), uint32_t(s));
					end_grain(bark, part, bp, bsides, girth_r, col);
					if (collar) {
						soil_tint(bark, part, 1.0f);
					}
					if (rigged) {
						_skin_stem(bark, part, s, bp, bsides);
					}
					knob_cap_from = INT_MAX;
					if (vf == 3 && piece == pieces - 1 && head_chain && cut_height > 0.5f && bp.size() > 6) {
						const int ce = chain[chain.size() - 1];
						if (cut_heads.has(ce) && !cut_stubs.has(ce) && !broken_ends.has(ce) && ce != broken_tip) {
							knob_cap_from = int(bp.size()) - cap_n - 3;
						}
					}
					if (vf) {
						veteran_deform(bark, part, bp, br, bsides, true, vf);
					}
					if (!stem && !hanger && junction_weld) {
						weld(bark, part, s, bp, br, bsides);
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
					_tube(moss, rp, rr, rsides, 0.0f, col, nullptr, 0.0f, girth_r);
					moss.end(part, r0);
					if (collar) {
						root_collar(moss, part, rsides, rp, rr, false);
					}
					if (vf) {
						veteran_deform(moss, part, rp, rr, rsides, false, vf);
					}
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
					// and wherever it is below the bark it stays hidden under it.
					const float floor_d = relief ? -0.045f * scale : -0.004f; // Below the deepest furrow (3 cm plus grain).
					if (rigged) {
						_skin_stem(moss, part, s, rp, rsides);
					}
					LocalVector<float> wv;
					wv.resize(part.w);
					// The weight before its clamp, and how much it changes from one vertex to the next:
					// on heavily mossed wood (logs) the weight crossed the whole ramp between two
					// vertices, so the edge could only fall on the grid and drew it as stairs. The ramp
					// is widened to span at least two and a half vertices, so the surface crosses the
					// bark at the weight's own contour, between vertices.
					const int ring = rsides + 1;
					LocalVector<float> raw;
					raw.resize(part.w);
					// No moss on a log's broken butt face (the closed cap's rings).
					const float cap_end = girth_r > 0.0f ? girth_r * 0.15f + 1e-4f : -1.0f;
					for (int i = 0; i < part.w; i++) {
						raw[i] = _moss_at(moss.verts[part.z + i], moss.normals[part.z + i], moss_ground, moss_k, false);
						if (cap_end > 0.0f && i / ring < int(rp.size()) && rp[i / ring].distance_to(rp[0]) < cap_end) {
							raw[i] = -1.0f;
						}
					}
					for (int i = 0; i < part.w; i++) {
						const int v = part.z + i;
						const Vector3 n = moss.normals[v];
						const float w = CLAMP(raw[i], 0.0f, 1.0f);
						float step = 0.0f;
						if (part.w % ring == 0) {
							const int rs = i / ring, sd = i % ring;
							const int nb[4] = { rs > 0 ? i - ring : -1, i + ring < part.w ? i + ring : -1, rs * ring + (sd + rsides - 1) % rsides, rs * ring + (sd + 1) % rsides };
							for (int k = 0; k < 4; k++) {
								if (nb[k] >= 0) {
									step = MAX(step, Math::abs(raw[nb[k]] - raw[i]));
								}
							}
						}
						// Hummocks a hand across: the vertices are 2.5 cm apart, so the texture's 4 cm
						// cushions stay in the normal map and the geometry carries only what it resolves.
						const float hummock = moss_noise(moss.verts[v] * 9.0f, 0x6d6f7376u);
						// A narrow ramp: the moss edge is a contour of the smooth weight that only dips into
						// the furrows just ahead of it; a wide one shows the 2.5 cm vertex grid as blocks
						// where furrows are partly filled.
						// Centred where it was (0.35 to 0.6), so the moss covers as much as before.
						const float ramp = CLAMP(0.5f + (raw[i] - 0.475f) / MAX(0.25f, 2.5f * step), 0.0f, 1.0f);
						float fill = Math::lerp(floor_d, depth * (0.6f + 0.8f * hummock), ramp * ramp * (3.0f - 2.0f * ramp));
						// As the relief tube has it: faded out at a standing stem's soil line.
						const float under = bark_disp(moss.uvs[v]) * ((fallen || fallen_branch) ? 1.0f : Math::smoothstep(-0.02f, 0.2f, moss.verts[v].y));
						// Shown where the fill stands above the bark; wv carries the margin.
						if (collar && raw[i] > 0.3f) {
							// The foot sleeve covers the bark down to the soil: on the roots' flanks it
							// broke into tongues with the bark between them standing in a crevice under
							// the shell's free edge, black under the path tracer.
							const float hh = moss.verts[v].y - ground_y;
							if (hh > -0.04f && hh < 0.25f) {
								fill = MAX(fill, under + 0.004f);
							}
						}
						wv[i] = fill - under;
						// Below the bark the shell keeps its own depth rather than hugging the bark's
						// underside: the edge then falls where the fill crosses the bark between two
						// vertices, not pulled out to the nearer vertex, which drew the edge as stairs
						// of the 2.5 cm grid.
						moss.verts.write[v] = moss.verts[v] + n * fill;
						// Wetter, deeper moss is darker; thin moss on dry faces yellower.
						const float dry = moss_noise(moss.verts[v] * 1.3f, 0x6d6f7375u);
						moss.colors.write[v] = Color(0.075f, 0.13f, 0.025f).lerp(Color(0.15f, 0.18f, 0.04f), CLAMP(dry * 1.2f - w * 0.4f, 0.0f, 1.0f));
					}
					if (collar) {
						soil_tint(moss, part, 0.7f);
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
			cut_arc0 = 0.0f;
			tube_carry_nrm = Vector3();
		}
		bark.role = Builder::ROLE_ALL;
		const int chain_end = chain[chain.size() - 1];
		if ((broken_tip >= 0 && chain_end == broken_tip) || broken_ends.has(chain_end)) {
			// A snapped end: a jagged fracture face and splinters (trees.md 5j). (A log's broken
			// butt gets its fibre tongues from _tube instead.)
			wood_b.bone = node_bone[chain_end];
			_fracture(rim_pt, nodes[chain_end].dir, rim_r, nodes[chain_end].dead || decay_class > 0 ? 1.0f : 0.0f, uint32_t(chain_end) * 2246822519u + uint32_t(seed));
		} else if (cut_stubs.has(chain_end) && rim_r >= 0.006f) {
			// A sawn stub (trees.md 5l): weathered by the years since the cut.
			wood_b.bone = node_bone[chain_end];
			const float since_cut = float(CLAMP(int(params[PARAM_YEARS]), 1, 300) - cut_stubs[chain_end]);
			// Square to the tube's own end, which gravity bent off the node's heading: tilted,
			// the face let the tube's closing cap show through as a pinwheel of bark.
			const Vector3 to_end = pts[pts.size() - 1] - rim_pt;
			const Vector3 face_dir = to_end.length_squared() > 1e-10f ? to_end.normalized() : nodes[chain_end].dir;
			_saw_face(rim_pt, face_dir, rim_r, since_cut / 6.0f + (decay_class > 0 ? 1.0f : 0.0f), uint32_t(chain_end) * 2246822519u + uint32_t(seed));
		} else if (_coppice_lobe(chain_end) && rim_r >= 0.006f && pts.size() >= 2) {
			// A coppice stool's lobe ends in the first cut, made sloping outward so rain runs off
			// (Rackham 2006), long weathered grey. Closed by a bark dome, the converging bark read
			// as a pale pinwheel on the stool's side.
			wood_b.bone = node_bone[chain_end];
			const Vector3 g = (pts[pts.size() - 1] - pts[pts.size() - 2]).normalized();
			_saw_face(rim_pt + g * (0.17f * rim_r), g, 0.8f * rim_r, 1.7f, uint32_t(chain_end) * 2246822519u + uint32_t(seed));
		}
		if (hanger && pts.size() >= 2) {
			// The hanger's own half of the break, facing back up at the stub.
			wood_b.bone = node_bone[s];
			_fracture(pts[0], (pts[0] - pts[1]).normalized(), rad[0], 0.0f, uint32_t(s) * 3266489917u + uint32_t(seed));
		}
		if (s == veteran_stem && !veteran_features.is_empty()) {
			// Woodpecker holes and epicormic shoots: their own small geometry on the stem.
			auto stem_at = [&](float p_h, float p_az, Vector3 &r_p, Vector3 &r_out, float &r_r, int &r_bone) {
				int k = 0;
				while (k + 2 < int(pts.size()) && pts[k + 1].y - vet_base_y < p_h) {
					k++;
				}
				const float y0 = pts[k].y - vet_base_y, y1 = pts[k + 1].y - vet_base_y;
				const float t = CLAMP((p_h - y0) / MAX(y1 - y0, 1e-4f), 0.0f, 1.0f);
				const Vector3 c = pts[k].lerp(pts[k + 1], t);
				r_r = Math::lerp(rad[k], rad[k + 1], t);
				r_out = Vector3(Math::cos(p_az), 0.0f, Math::sin(p_az));
				r_p = c + r_out * r_r;
				int ci = 0;
				for (int q = 0; q < int(chain.size()); q++) {
					if (nodes[chain[q]].start.y - vet_base_y <= p_h) {
						ci = q;
					}
				}
				r_bone = node_bone[chain[ci]];
			};
			RandomPCG er;
			er.seed(uint64_t(seed) * 40503u + 0x5e5e5eu);
			const LeafSeason vls = _leaf_season(preset, season);
			for (const VeteranFeature &f : veteran_features) {
				Vector3 p, out;
				float r;
				int bone;
				stem_at(f.h, f.azimuth, p, out, r, bone);
				if (f.kind == VET_WOODPECKER) {
					// A dark opening with a pale rim of chiselled wood, just proud of the bark.
					const Vector3 up(0, 1, 0);
					const Vector3 side = up.cross(out).normalized();
					const int k = 14;
					bark.bone = bone;
					Vector4i part;
					bark.begin(part);
					const int b0 = bark.verts.size();
					const Color hole(0.02f, 0.018f, 0.015f), rim = bark_color.lerp(Color(0.55f, 0.47f, 0.36f), 0.6f).srgb_to_linear();
					bark.verts.push_back(p + out * 0.004f);
					bark.normals.push_back(out);
					bark.uvs.push_back(Vector2(0.5f, 0.5f));
					bark.colors.push_back(hole);
					for (int ring = 0; ring < 2; ring++) {
						const float sc = ring == 0 ? 1.0f : 1.3f;
						for (int q = 0; q < k; q++) {
							const float a = float(Math::TAU) * q / k;
							bark.verts.push_back(p + out * (ring == 0 ? 0.005f : 0.002f) + side * (Math::cos(a) * f.half_w * sc) + up * (Math::sin(a) * f.half_h * sc));
							bark.normals.push_back(out);
							bark.uvs.push_back(Vector2(0.5f + 0.5f * Math::cos(a), 0.5f + 0.5f * Math::sin(a)));
							bark.colors.push_back(ring == 0 ? hole : rim);
						}
					}
					for (int q = 0; q < k; q++) {
						const int a = b0 + 1 + q, b = b0 + 1 + (q + 1) % k;
						bark.indices.push_back(b0);
						bark.indices.push_back(b);
						bark.indices.push_back(a);
						bark.indices.push_back(a);
						bark.indices.push_back(b);
						bark.indices.push_back(b + k);
						bark.indices.push_back(a);
						bark.indices.push_back(b + k);
						bark.indices.push_back(a + k);
					}
					bark.end(part, f.half_w);
				} else if (f.kind == VET_EPICORMIC) {
					// A cluster of water shoots from dormant buds, leafy in season. They live for
					// years and branch, so a cluster on an old oak is a twiggy brush that reads in
					// winter too (Meier, Saunders and Michler 2012; Colin et al. 2010, oak
					// epicormics): a few new shoots and some older ones with side twigs. Young
					// bark is a warmer brown than the grey stem.
					const int shoots = MAX(1, int(f.amount));
					const Color shoot_bark = bark_color.lerp(Color(0.33f, 0.24f, 0.15f), 0.6f).srgb_to_linear();
					for (int k = 0; k < shoots; k++) {
						Vector3 bp, bo;
						float br;
						int bb;
						const float sh = f.h + (er.randf() - 0.5f) * (head_r > 0.0f ? 0.12f : 0.3f);
						const float sa = f.azimuth + (er.randf() - 0.5f) * (head_r > 0.0f ? 0.3f : 0.6f);
						stem_at(sh, sa, bp, bo, br, bb);
						// Out of the burr they break from: from the plain stem's radius they started
						// inside a pollard head's burrs, and only those on its small ones showed.
						for (const VeteranFeature &g : veteran_features) {
							if (g.kind == VET_BURL && Math::abs(g.h - f.h) < 1e-4f && Math::abs(g.azimuth - f.azimuth) < 1e-4f) {
								const float dh = (sh - g.h) / g.half_h;
								const float da = Math::angle_difference(g.azimuth, sa) * br / g.half_w;
								const float k0 = 1.0f - MIN(dh * dh + da * da, 1.0f);
								bp += bo * MAX(0.0f, g.amount * MIN(br, vet_r_stem) * k0 * k0 * 0.8f - 0.01f);
							}
						}
						const Vector3 dir = (bo * 0.75f + Vector3(0, 0.65f, 0) + Vector3(er.randf() - 0.5f, er.randf() * 0.3f, er.randf() - 0.5f) * 0.4f).normalized();
						const float age = er.randf();
						const float len = Math::lerp(0.3f, 1.1f, age);
						const float sr = Math::lerp(0.004f, 0.011f, age);
						LocalVector<Vector3> sp;
						LocalVector<float> srr;
						sp.push_back(bp - bo * 0.01f);
						srr.push_back(sr);
						// Out from the bark, then turning up toward the light.
						const Vector3 up_dir = (dir + Vector3(0, 0.6f, 0)).normalized();
						sp.push_back(bp + dir * (len * 0.4f) + Vector3(0, -0.02f, 0));
						srr.push_back(sr * 0.75f);
						sp.push_back(sp[1] + up_dir * (len * 0.35f));
						srr.push_back(sr * 0.5f);
						sp.push_back(sp[2] + up_dir * (len * 0.25f));
						srr.push_back(sr * 0.25f);
						bark.bone = bb;
						Vector4i part;
						bark.begin(part);
						_tube(bark, sp, srr, 3, 0.0f, shoot_bark);
						bark.end(part, sr);
						// Older shoots carry side twigs from last years' buds.
						const int twigs = age > 0.35f ? 1 + int(age * 3.0f) : 0;
						for (int q = 0; q < twigs; q++) {
							const float u = Math::lerp(0.3f, 0.8f, (q + er.randf()) / twigs);
							const int seg = u < 0.4f ? 0 : (u < 0.75f ? 1 : 2);
							const float uu = seg == 0 ? u / 0.4f : (seg == 1 ? (u - 0.4f) / 0.35f : (u - 0.75f) / 0.25f);
							const Vector3 at = sp[seg].lerp(sp[seg + 1], uu);
							const float ar = Math::lerp(srr[seg], srr[seg + 1], uu) * 0.6f;
							const Vector3 side = up_dir.cross(Vector3(er.randf() - 0.5f, 0.0f, er.randf() - 0.5f) + bo * 0.01f).normalized();
							const Vector3 td = (side * 0.8f + up_dir * 0.6f).normalized();
							const float tl = len * Math::lerp(0.2f, 0.4f, er.randf());
							LocalVector<Vector3> tp;
							LocalVector<float> trr;
							tp.push_back(at);
							trr.push_back(ar);
							tp.push_back(at + td * (tl * 0.5f));
							trr.push_back(ar * 0.6f);
							tp.push_back(at + (td + Vector3(0, 0.4f, 0)).normalized() * tl);
							trr.push_back(ar * 0.25f);
							Vector4i tpart;
							bark.begin(tpart);
							_tube(bark, tp, trr, 3, 0.0f, shoot_bark);
							bark.end(tpart, ar);
						}
						if (vls.out <= 0.0f || vls.fallen >= 1.0f) {
							continue;
						}
						const int nl = 4 + int(er.randf() * 5.0f);
						for (int q = 0; q < nl; q++) {
							const float u = (q + 1.0f) / nl;
							const Vector3 at = u < 0.5f ? sp[1].lerp(sp[2], u * 2.0f) : sp[2].lerp(sp[3], u * 2.0f - 1.0f);
							const Vector3 sd = dir.cross(Vector3(0, 1, 0)).normalized() * ((q & 1) ? 1.0f : -1.0f);
							const Vector3 lo = (sd * 0.8f + dir * 0.5f).normalized();
							const float j = 0.85f + 0.3f * er.randf();
							Color c(leaf_color.r * j, leaf_color.g * j, leaf_color.b);
							if (vls.autumn > er.randf()) {
								c = c.lerp(vls.color, 0.8f);
							}
							const Vector3 lup = (Vector3(0, 1, 0) + bo * 0.4f).normalized();
							leaves.bone = bb;
							Vector4i lp;
							leaves.begin(lp);
							_add_leaf(leaves, at, lo, lup, params[PARAM_LEAF_SIZE] * Math::lerp(0.8f, 1.1f, er.randf()) * MAX(vls.out, 0.3f), c);
							leaves.end(lp, 0.0f);
							leaf_count++;
						}
					}
				}
			}
		}
	}
	soil_b = Builder();
	if (plate.on) {
		const int roots_v0 = int(bark.verts.size());
		_root_plate(bark);
		_measure_lying(bark, roots_v0, int(bark.verts.size()));
		_measure_lying(soil_b, 0, int(soil_b.verts.size()));
	}
	if (dw_v0 >= 0) {
		_measure_lying(bark, dw_v0, dw_v1 >= 0 ? dw_v1 : int(bark.verts.size()));
		_measure_lying(moss, dw_m0, dw_m1 >= 0 ? dw_m1 : int(moss.verts.size()));
	}

	const bool needles = params[PARAM_NEEDLES] > 0.5f;
	// The year's shoots carry young bark: a birch's are dark red-brown, not its trunk's white.
	const bool birch_twigs = preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH || preset == PRESET_YELLOW_BIRCH;
	const Color twig_col = (birch_twigs ? Color(0.3f, 0.18f, 0.14f) : bark_color).srgb_to_linear();
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
					{ Vector4i part; bark.begin(part); _tube(bark, pts, rad, 3, 0.0f, twig_col); bark.end(part, rad[0]); }
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
			{ Vector4i part; bark.begin(part); _tube(bark, pts, rad, 3, 0.0f, twig_col); bark.end(part, rad[0]); }
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
			// A willow pollard's withies are one- to few-year rods, unbranched: their leaves sit
			// on the rod itself, not on side twigs (which drew them as fishbone combs in winter).
			const bool rod = preset == PRESET_WHITE_WILLOW && cut_height > 0.5f && n.pole && n.born - last_cut_year <= 6;
			const float tlen = n.length * (0.45f + 0.45f * lr.randf()) * (rod ? 0.06f : 1.0f);
			if (lr.randf() > keep_twig) {
				continue;
			}
			if (!rod && lr.randf() < 0.4f && per_twig >= 4) {
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

	climber_leaf_b = Builder();
	climber_adult_b = Builder();
	if (climber != CLIMBER_NONE && climber_cover > 0.0f) {
		_grow_climbers(bark, leaves);
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Builder fungi;
	_grow_fungi(bark, fungi);
	Builder crown_parts;
	const int crown_big_end = _grow_crown_parts(crown_parts);
	Builder *builders[9] = { &bark, &leaves, &moss, &climber_leaf_b, &fungi, &wood_b, &climber_adult_b, &soil_b, &crown_parts };
	for (int s = 0; s < 9; s++) {
		Builder &b = *builders[s];
		if (b.indices.is_empty()) {
			continue;
		}
		if (s == 0 || s == 2 || s == 7) {
			// The bark, moss and soil builders wind their triangles counter-clockwise seen from outside;
			// Godot's front faces wind clockwise (as PlantGenerator flips its builders). Unflipped,
			// the raster drew every stem's far inner wall, and the moss shell lay behind it.
			int32_t *iw = b.indices.ptrw();
			for (int64_t i = 0; i + 2 < b.indices.size(); i += 3) {
				SWAP(iw[i + 1], iw[i + 2]);
			}
		}
		bool bark_custom = false;
		if (s == 0) {
			_peel_bark(b);
			bark_custom = _bare_amount() > 0.0f;
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
			} else if (s == 3 || s == 6) {
				lods = _leaf_lods(b, false);
			} else if (s == 8) {
				// Crown parts (5n): mistletoe balls and brooms are seen from far; cones, catkins
				// and keys are centimetres across, LOD 0 only.
				if (crown_big_end < b.indices.size()) {
					PackedInt32Array keep;
					if (crown_big_end > 0) {
						keep = b.indices.slice(0, crown_big_end);
					} else {
						keep.push_back(0);
						keep.push_back(0);
						keep.push_back(0);
					}
					lods[TREE_LODS[0].error] = keep;
				}
			} else if (s == 5 || s == 7) {
				// Broken wood: a few hundred triangles a break, seen from every distance; a root
				// plate's soil a few thousand.
			} else if (s == 4) {
				// Fungi are a few thousand triangles at most: one level, seen from every distance.
				// The lichen strands after them (millimetre threads, sub-pixel past a few metres)
				// are LOD 0 only, as the moss shell.
				if (fungi_index_end < b.indices.size()) {
					PackedInt32Array keep;
					if (fungi_index_end > 0) {
						keep = b.indices.slice(0, fungi_index_end);
					} else {
						keep.push_back(0);
						keep.push_back(0);
						keep.push_back(0);
					}
					lods[TREE_LODS[0].error] = keep;
				}
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
		bool lichen_uv2 = false;
		if (s == 0 && _lichen_amount() > 0.0f) {
			for (uint32_t i = 0; i < b.part_uv2.size() && !lichen_uv2; i++) {
				lichen_uv2 = b.part_uv2[i].x > 0.0f;
			}
		}
		if (lichen_uv2) {
			PackedVector2Array uv2;
			uv2.resize(b.verts.size());
			Vector2 *u2 = uv2.ptrw();
			for (int v = 0; v < uv2.size(); v++) {
				u2[v] = Vector2();
			}
			for (uint32_t i = 0; i < b.parts.size(); i++) {
				const Vector4 &m = b.part_uv2[i];
				if (m.x <= 0.0f) {
					continue;
				}
				const Vector4i &pt = b.parts[i];
				for (int v = pt.z; v < pt.z + pt.w; v++) {
					u2[v] = Vector2(b.uvs[v].x * m.x + m.z, b.uvs[v].y * m.y + m.w);
				}
			}
			arrays[Mesh::ARRAY_TEX_UV2] = uv2;
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
		if (material_source.is_valid()) {
			// A near mesh shares its far mesh's materials: the same shaders and batches, and no
			// reads back from the RenderingServer (which wait on the render thread) off the main
			// thread.
			const char *names[9] = { "bark", params[PARAM_NEEDLES] > 0.5f ? "needles" : "leaves", "moss", "climber_leaves", "fungi", "wood", "climber_leaves_adult", "soil", "crown_parts" };
			int found = -1;
			for (int i = 0; i < material_source->get_surface_count(); i++) {
				if (material_source->surface_get_name(i) == names[s]) {
					found = i;
					break;
				}
			}
			if (found >= 0) {
				const int at = mesh->get_surface_count() - 1;
				mesh->surface_set_name(at, names[s]);
				mesh->surface_set_material(at, material_source->surface_get_material(found));
				continue;
			}
		}
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
			if (lichen_uv2 || bark_custom) {
				mesh->surface_set_material(mesh->get_surface_count() - 1, _lichen_material(m));
				continue;
			}
		} else if (s == 3 || s == 6) {
			// The climber's own leaf textures from the plant material; vertex colour is a tone.
			const Ref<ArrayMesh> pmesh = _plant_climber_leaves(s == 6 ? int(CLIMBER_MAX) : int(climber))[0];
			const Ref<ShaderMaterial> pm = pmesh->surface_get_material(0);
			if (pm.is_valid()) {
				m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, pm->get_shader_parameter("albedo_tex"));
				m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
				m->set_texture(BaseMaterial3D::TEXTURE_BACKLIGHT, pm->get_shader_parameter("backlight_tex"));
				m->set_backlight(Color(1, 1, 1));
				m->set_roughness(pm->get_shader_parameter("roughness"));
			}
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			mesh->surface_set_name(mesh->get_surface_count() - 1, s == 6 ? "climber_leaves_adult" : "climber_leaves");
		} else if (s == 8) {
			// Cones, catkins, keys, mistletoe and brooms (trees.md 5n): vertex colour, the form
			// is geometry; thin scales, wings and leaves are seen from both sides.
			m->set_roughness(0.7f);
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			// Millimetre twigs and scales caught snow on every upward face and read as frosted
			// white sprays; snow on them is below what the snow cover resolves.
			m->set_feature(BaseMaterial3D::FEATURE_SNOW, false);
			m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
			m->set_backlight(Color(0.12f, 0.14f, 0.05f));
			mesh->surface_set_name(mesh->get_surface_count() - 1, "crown_parts");
		} else if (s == 7) {
			// A root plate's soil and stones (trees.md 5k): vertex colour, the relief is geometry.
			m->set_roughness(0.95f);
			mesh->surface_set_name(mesh->get_surface_count() - 1, "soil");
		} else if (s == 5) {
			// Broken wood (trees.md 5j): fracture faces and splinters, vertex colour by how fresh
			// the break is, a texture of fibres along v with their relief.
			m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _wood_texture(false));
			m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
			m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, _wood_texture(true));
			m->set_normal_scale(0.8f);
			m->set_roughness(0.85f);
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			mesh->surface_set_name(mesh->get_surface_count() - 1, "wood");
		} else if (s == 4) {
			// Fungi: vertex colour per species and zone, a texture of growth lines and pores.
			m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _fungus_texture());
			m->set_roughness(0.72f);
			m->set_feature(BaseMaterial3D::FEATURE_SNOW, false);
			mesh->surface_set_name(mesh->get_surface_count() - 1, "fungi");
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
				// Venation; the factor averages about 0.85, so the albedo lifts it back to the
				// species' leaf colour on average.
				if (species_leaf_forms && LEAF_FORMS[preset].kind == LEAF_PINNATE) {
					// Leaflets cut by alpha (outline and teeth). Alpha to coverage keeps the cut's
					// edge smooth and the far mips' partial coverage.
					m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _leaflet_texture());
					m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
					m->set_alpha_scissor_threshold(0.5f);
					m->set_alpha_antialiasing(BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
					// Its venation factor averages about 0.78.
					m->set_albedo(Color(1.28f, 1.28f, 1.28f));
				} else {
					m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _leaf_texture());
					m->set_albedo(Color(1.17f, 1.17f, 1.17f));
				}
			}
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
			m->set_backlight(leaf_color * 0.6f);
			mesh->surface_set_name(mesh->get_surface_count() - 1, needles ? "needles" : "leaves");
		}
		if ((s == 1 && !needles) || s == 3) {
			mesh->surface_set_material(mesh->get_surface_count() - 1, _flutter_material(m, s == 3 ? 0.05f : params[PARAM_LEAF_SIZE]));
		} else {
			mesh->surface_set_material(mesh->get_surface_count() - 1, m);
		}
	}
	mesh->set_meta(SNAME("tree_colliders"), _collider_data());
	if (!climber_leaves.is_empty()) {
		mesh->set_meta(SNAME("tree_climber_leaves"), climber_leaves);
	}
	if (!fungi_points.is_empty()) {
		// Where the tree's own fungi grow (trees.md 5g), for ground plants of the same rot: per
		// cluster position (3), the wood's outward normal there (3), the cluster's width (m) and
		// its kind (0 Fomes, 1 Fomitopsis pinicola, 2 F. betulina, 3 Ganoderma, 4 Laetiporus,
		// 5 Trametes, 6 honey fungus, 7 sulphur tuft); mesh space.
		mesh->set_meta(SNAME("tree_fungi"), fungi_points);
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

// Leaf flutter (docs/ideas/trees.md 3b): the leaf material's own shader (BaseMaterial3D's
// generated code, so skinning from instance bones, snow and wetness stay as they are) with Sousa's
// detail bending (GPU Gems 3, ch. 16, Crysis): each leaf bends along its normal with distance
// from the petiole squared, and twists about its midrib (the blade's UV.x, 0.5 on the midrib),
// at a few hertz out of phase leaf to leaf (the phase from the leaf's vertex colour, the same on
// every vertex of a leaf at every LOD), scaled by the local wind (plant_wind, the ground plants'
// weather-map wind). Twisting about the stalk is what SpeedTree's leaf ripple and Unreal's Pivot
// Painter show; bending and twist are a few millimetres to a centimetre, which raster and the
// path tracer's captured vertex shader both see.
// The bark material with the lichen layer (trees.md 2h): BaseMaterial3D's own shader with the
// lichen laid over the bark's albedo and roughness, before wetness and snow, which then lie on
// the lichen as on the bark. Raster and path tracer run the same lines.
Ref<Material> TreeGenerator::_lichen_material(const Ref<StandardMaterial3D> &p_base) {
	if (p_base.is_null() || RenderingServer::get_singleton() == nullptr) {
		return p_base;
	}
	BaseMaterial3D::flush_changes();
	String code = RenderingServer::get_singleton()->shader_get_code(p_base->get_shader_rid());
	const int vx = code.find("void vertex() {");
	const String anchor = "	ROUGHNESS = roughness_tex * roughness;\n";
	const int ra = code.find(anchor);
	if (code.is_empty() || vx < 0 || ra < 0) {
		return p_base;
	}
	const String body = R"(
	float bark_bare = 0.0;
	vec3 bark_wood_n = vec3(0.5, 0.5, 1.0);
	{
		// Vertex alpha (TreeGenerator::_peel_bark): 0.5 + 0.5 x lichen weight on bark, 0.5 - 0.5 x
		// bare where the bark is lost. The bark breaks along its furrows, so the edge follows the
		// relief (the roughness map's blue channel).
		float bark_relief = texture(texture_roughness, base_uv).b;
		float bare_field = clamp(1.0 - 2.0 * COLOR.a, 0.0, 1.0);
		if (bare_field > 0.0) {
			bare_field += 0.35 * (0.5 - bark_relief);
			bark_bare = smoothstep(0.47, 0.53, bare_field);
		}
		// Lichen: a colony covers the texels whose growth order is under the weight.
		float lw = clamp(2.0 * COLOR.a - 1.0, 0.0, 1.0);
		if (lw > 0.004) {
			vec4 lm = texture(lichen_map, UV2);
			// A crust is a skin a fraction of a millimetre thick: the bark's own tone shows through
			// it (darker in the bark's cracks), as do its relief and normal map.
			float bark_tone = clamp(dot(albedo_tex.rgb, vec3(0.3, 0.5, 0.2)) / max(dot(COLOR.rgb, vec3(0.3, 0.5, 0.2)), 1e-3), 0.4, 1.4);
			// Crusts take the exposed ridges and plates first and reach into the furrows last
			// (the furrows hold water and dirt, where moss and powdery Lepraria win).
			float ridge = mix(0.2, 1.3, smoothstep(0.35, 0.75, texture(texture_roughness, base_uv).b));
			// Each colony starts at its own weight and then soon spreads, so a thin cover is a few
			// grown patches and many bare spaces, not every colony's first speck.
			float start = 0.6 * fract(lm.b * 7.13);
			float grown = smoothstep(start, start + 0.2, lw);
			float tc = grown * lw * 1.2 * (1.0 - 0.4 * lichen_leafy_share) * ridge;
			float tf = lw * (0.15 + 0.5 * lichen_leafy_share);
			float crust = 1.0 - smoothstep(tc - 0.08, tc, lm.r);
			float leafy = 1.0 - smoothstep(tf - 0.06, tf, lm.g);
			// Each crust its own species' tone, from whitish to grey-green; paler at the growing
			// rim than in the older middle.
			vec3 crust_col = lichen_crust_color * mix(vec3(1.0), vec3(0.9, 1.0, 0.82), fract(lm.b * 5.3)) * mix(0.82, 1.1, fract(lm.b * 13.7));
			vec3 cc = (lm.b < lichen_orange_share ? lichen_orange_color : crust_col) * mix(0.78, 1.05, lm.r) * mix(1.0, bark_tone, 0.45);
			// Rosettes: darker and wrinkled in the old middle, paler at the growing lobe tips.
			vec3 lc = lichen_leafy_color * mix(0.62, 1.04, smoothstep(0.1, 0.9, lm.g / max(tf, 1e-3))) * mix(1.0, bark_tone, 0.25);
			ALBEDO = mix(ALBEDO, cc * lm.a, crust * 0.85);
			ALBEDO = mix(ALBEDO, lc * mix(1.0, lm.a, 0.6), leafy);
			ROUGHNESS = mix(ROUGHNESS, 0.9, max(crust, leafy));
		}
		if (bare_field > 0.0) {
			// Wood under lost bark: fibres along the stem (v), long drying checks, grey where it
			// weathers in the open, browner as it rots. The bark's broken edge is darker (its
			// thickness in shadow).
			float grain = texture(lichen_map, vec2(base_uv.x * 1.7, base_uv.y * 0.08)).r;
			// Checks: thin dark splits along the grain, wavering with it, broken into lengths.
			float wander = texture(lichen_map, vec2(base_uv.x * 0.7 + 0.53, base_uv.y * 0.02)).r;
			float split = abs(fract(base_uv.x * 3.0 + 1.2 * wander) - 0.5);
			float run = smoothstep(0.5, 0.65, texture(lichen_map, vec2(base_uv.x * 0.9 + 0.21, base_uv.y * 0.03)).r);
			float checks = mix(1.0, smoothstep(0.01, 0.07, split), run);
			vec3 wood = bark_wood_color * (0.72 + 0.45 * grain) * mix(0.35, 1.0, checks);
			float rim = smoothstep(0.25, 0.47, bare_field) * (1.0 - bark_bare);
			ALBEDO *= 1.0 - 0.45 * rim;
			ALBEDO = mix(ALBEDO, wood, bark_bare);
			ROUGHNESS = mix(ROUGHNESS, 0.8, bark_bare);
			// The wood's own relief: fibre ridges across u and the checks as grooves, as a tangent
			// space normal from the pattern's slope (finite differences of the same samples).
			float du = 0.004;
			float g1 = texture(lichen_map, vec2((base_uv.x + du) * 1.7, base_uv.y * 0.08)).r;
			float split1 = abs(fract((base_uv.x + du) * 3.0 + 1.2 * wander) - 0.5);
			float c1 = mix(1.0, smoothstep(0.01, 0.07, split1), run);
			float h0 = 0.4 * grain + checks, h1 = 0.4 * g1 + c1;
			bark_wood_n = vec3(clamp(0.5 - (h1 - h0) * 2.5, 0.0, 1.0), 0.5, 1.0);
		}
	}
)";
	// The wood under lost bark is smoother than the bark: its normal map fades there. Inserted
	// first, after the normal map is read (later in the shader than the roughness).
	const String nanchor = "	NORMAL_MAP_DEPTH = normal_scale;\n";
	const int na = code.find(nanchor);
	if (na > ra) {
		code = code.insert(na + nanchor.length(), "	NORMAL_MAP = mix(NORMAL_MAP, bark_wood_n, bark_bare * 0.85);\n");
	}
	code = code.insert(ra + anchor.length(), body);
	const String uniforms = R"(uniform sampler2D lichen_map : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform vec3 lichen_crust_color : source_color = vec3(0.5, 0.53, 0.47);
uniform vec3 lichen_leafy_color : source_color = vec3(0.6, 0.64, 0.55);
uniform vec3 lichen_orange_color : source_color = vec3(0.74, 0.58, 0.28);
uniform float lichen_leafy_share = 0.5;
uniform float lichen_orange_share = 0.0;
uniform vec3 bark_wood_color : source_color = vec3(0.55, 0.53, 0.49);
)";
	code = code.insert(vx, uniforms + "\n");
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(code);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(sh);
	// The base material's parameters, as BaseMaterial3D set them on its own RID.
	List<PropertyInfo> uniforms_list;
	sh->get_shader_uniform_list(&uniforms_list);
	const RID base_rid = p_base->get_rid();
	for (const PropertyInfo &pi : uniforms_list) {
		Variant v = RenderingServer::get_singleton()->material_get_param(base_rid, pi.name);
		if (v.get_type() == Variant::RID) {
			// Textures go back as the Texture resources (the path tracer reads those).
			for (int t = 0; t < BaseMaterial3D::TEXTURE_MAX; t++) {
				const Ref<Texture2D> tex = p_base->get_texture(BaseMaterial3D::TextureParam(t));
				if (tex.is_valid() && tex->get_rid() == RID(v)) {
					v = tex;
					break;
				}
			}
		}
		if (v.get_type() != Variant::NIL) {
			m->set_shader_parameter(pi.name, v);
		}
	}
	const LichenAffinity &la = LICHEN_AFFINITY[preset];
	m->set_shader_parameter("lichen_map", _lichen_texture());
	m->set_shader_parameter("lichen_leafy_share", la.leafy);
	// Standing dead wood weathers silver-grey in sun and rain; rotting and on the ground it browns.
	const Color wood = (fallen || fallen_branch || decay_class >= 5) ? Color(0.38f, 0.31f, 0.23f) : decay_class == 4 ? Color(0.42f, 0.38f, 0.32f) : Color(0.47f, 0.45f, 0.41f);
	m->set_shader_parameter("bark_wood_color", wood);
	m->set_shader_parameter("lichen_orange_share", la.orange);
	// Leafy lichens of acid bark are grey-green (Parmelia, Hypogymnia); of nutrient-rich bark
	// yellower (Flavoparmelia, Physcia with Xanthoria about).
	m->set_shader_parameter("lichen_leafy_color", Color(0.55f, 0.6f, 0.5f).lerp(Color(0.6f, 0.63f, 0.4f), CLAMP(la.orange * 3.0f, 0.0f, 1.0f)));
	return m;
}

Ref<Material> TreeGenerator::_flutter_material(const Ref<StandardMaterial3D> &p_base, float p_leaf_size) const {
	if (leaf_flutter <= 0.0f || p_base.is_null() || RenderingServer::get_singleton() == nullptr) {
		return p_base;
	}
	BaseMaterial3D::flush_changes();
	String code = RenderingServer::get_singleton()->shader_get_code(p_base->get_shader_rid());
	const int vx = code.find("void vertex() {");
	if (code.is_empty() || vx < 0) {
		return p_base;
	}
	// The wind lines, minus any global the generated code already declares.
	String wind;
	const String plant_wind_code = R"(
global uniform vec4 grass_wind;
vec3 plant_wind(vec3 wp, float h) {
	if (weather_map_rect.z > 0.0) {
		return weather_wind(wp, h);
	}
	vec2 w = grass_wind.xy;
	float s = length(w);
	if (s < 1e-4) {
		return vec3(0.0);
	}
	vec2 d = w / s;
	float phase = dot(wp.xz, d) * 0.12 - TIME * s * 0.12;
	float g = 1.0 + 0.45 * sin(phase) + 0.2 * sin(phase * 2.7 + dot(wp.xz, vec2(-d.y, d.x)) * 0.2);
	return vec3(w.x, 0.0, w.y) * g;
}
)";
	// The ground plants' wind (PlantGenerator's plant_wind, the same weather-map wind and fallback).
	const Vector<String> lines = (WeatherMap3D::get_wind_shader_code() + plant_wind_code).split("\n");
	for (const String &l : lines) {
		const String t = l.strip_edges();
		if (t.begins_with("global uniform") && code.contains(t)) {
			continue;
		}
		wind += l + "\n";
	}
	const String uniforms = "uniform float flutter_amount = 1.0;\nuniform float flutter_size = 0.1;\n";
	const String body = R"(
	{
		// Leaf flutter (TreeGenerator::_flutter_material).
		vec3 fw = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
		float fs = length(plant_wind(fw, max(fw.y - MODEL_MATRIX[3].y, 1.0)));
		float fa = flutter_amount * smoothstep(0.5, 9.0, fs);
		if (fa > 0.0) {
			float fph = fract(dot(COLOR.rgb, vec3(91.37, 47.13, 13.71))) * 6.2831853;
			float fq = 5.0 + 4.0 * fract(fph * 3.17) + 0.25 * fs;
			float fx = UV.x - 0.5;
			float ft = UV.y;
			VERTEX += NORMAL * (flutter_size * fa * (0.07 * ft * ft * sin(TIME * fq + fph) + 0.3 * fx * ft * sin(TIME * fq * 1.31 + fph * 2.3)));
		}
	}
)";
	code = code.insert(vx + String("void vertex() {").length(), body);
	// The underside: paler and greyer than the top on nearly every broadleaf (a thinner cuticle,
	// spongy mesophyll and often hairs; rowan grey-green and downy, white willow silvery: Stace
	// 2019). Seen from below a crown is mostly undersides. The path tracer follows FRONT_FACING.
	float leaf_underside = 0.2f;
	switch (preset) {
		case PRESET_ROWAN:
			leaf_underside = 0.5f;
			break;
		case PRESET_WHITE_WILLOW:
			leaf_underside = 0.75f;
			break;
		case PRESET_SYCAMORE_MAPLE:
		case PRESET_LIME:
			leaf_underside = 0.35f;
			break;
		case PRESET_BEECH:
		case PRESET_AMERICAN_BEECH:
		case PRESET_BLACK_ALDER:
		case PRESET_ELDER:
			leaf_underside = 0.12f;
			break;
		default:
			break;
	}
	const String albedo_line = "ALBEDO = albedo.rgb * albedo_tex.rgb;";
	const int al = code.find(albedo_line);
	if (al >= 0 && leaf_underside > 0.0f) {
		code = code.insert(al + albedo_line.length(), R"(
	if (!FRONT_FACING) {
		float leaf_lum = dot(ALBEDO, vec3(0.299, 0.587, 0.114));
		ALBEDO = mix(ALBEDO, vec3(leaf_lum) * vec3(0.93, 1.04, 0.9) * 1.2, leaf_underside);
	})");
	}
	code = code.insert(vx, uniforms + "uniform float leaf_underside = 0.0;\n" + wind + "\n");
	// The path tracer: flutter is a raster motion of millimetres; the traced leaves keep their
	// shape and follow the bones. A static tree is traced from its rest mesh (no capture), a
	// rigged one is captured only when its bones move (as the plain material was), never because
	// TIME moved: capturing a whole crown every frame for flutter costs memory and BLAS builds.
	const int rm = code.find("render_mode ");
	if (rm >= 0) {
		code = code.insert(rm + String("render_mode ").length(), rigged ? "rt_vertex_frozen, " : "rt_vertex_static, ");
	}
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(code);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(sh);
	// The base material's parameters, as BaseMaterial3D set them on its own RID.
	List<PropertyInfo> uniforms_list;
	sh->get_shader_uniform_list(&uniforms_list);
	const RID base_rid = p_base->get_rid();
	for (const PropertyInfo &pi : uniforms_list) {
		Variant v = RenderingServer::get_singleton()->material_get_param(base_rid, pi.name);
		if (v.get_type() == Variant::RID) {
			// Textures go back as the Texture resources (the path tracer reads those).
			for (int t = 0; t < BaseMaterial3D::TEXTURE_MAX; t++) {
				const Ref<Texture2D> tex = p_base->get_texture(BaseMaterial3D::TextureParam(t));
				if (tex.is_valid() && tex->get_rid() == RID(v)) {
					v = tex;
					break;
				}
			}
		}
		if (v.get_type() != Variant::NIL) {
			m->set_shader_parameter(pi.name, v);
		}
	}
	m->set_shader_parameter("flutter_amount", leaf_flutter);
	m->set_shader_parameter("flutter_size", p_leaf_size);
	m->set_shader_parameter("leaf_underside", leaf_underside);
	return m;
}

Dictionary TreeGenerator::_leaf_lods(Builder &r_leaves, bool p_twins) const {
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
			const Vector4i *twin = p_twins ? leaf_twin_of.getptr(int(i)) : nullptr;
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
// One variant of a stand species (the loop in _stand_species(), and near meshes on demand).
void TreeGenerator::_setup_variant(Preset p_preset, Form p_form, int p_variant, int p_base_seed, float p_base_cover) {
	seed = p_base_seed * 31 + p_variant;
	if (p_form == FORM_SNAG || p_form == FORM_LOG) {
		decay_class = 2 + p_variant % 3;
		fallen = p_form == FORM_LOG;
		// Windthrow tips trees out by the roots about as often as it snaps them (trees.md 5k):
		// the freshest log of each three lies with its root plate.
		uprooted = fallen && p_variant % 3 == 0;
	} else if (p_form == FORM_BRANCH || p_form == FORM_STICKS) {
		// Freshly shed (twigs still on) to well rotted.
		decay_class = 1 + p_variant % 3;
		fallen_branch = true;
		shed_radius_max = p_form == FORM_STICKS ? 0.04f : 0.14f;
	} else if (p_form == FORM_COPPICE || p_form == FORM_COPPICE_YOUNG) {
		// Coppice (trees.md 5l): a stool cut near the ground every 7-10 years (hazel 7-8; ash,
		// alder and hornbeam longer), worked for decades. Each variant is one stool at its own
		// point in the cycle, so a coupe of one age still varies (Rackham, Woodlands, 2006).
		const uint32_t h = uint32_t(seed) * 2654435761u;
		cut_height = 0.15f;
		cut_cycle = p_preset == PRESET_HAZEL ? 8 : 10;
		cut_since = p_form == FORM_COPPICE_YOUNG ? 1 + int((h >> 8) % 3) : 4 + int((h >> 8) % MAX(cut_cycle - 3, 1));
		params[PARAM_YEARS] = float(40 + int((h >> 16) % 50));
	} else if (p_form == FORM_POLLARD) {
		// Pollards: willow still worked, cut every 5-8 years; beech, oak and the rest lapsed, last
		// cut 40-150 years ago, their old poles now trunks (Read, Pollards, 2000).
		const uint32_t h = uint32_t(seed) * 2246822519u;
		if (p_preset == PRESET_WHITE_WILLOW) {
			cut_height = 2.0f + 0.1f * float((h >> 4) % 5);
			cut_cycle = 5 + int((h >> 8) % 4);
			cut_since = 1 + int((h >> 12) % cut_cycle);
			params[PARAM_YEARS] = float(50 + int((h >> 16) % 50));
		} else {
			cut_height = 2.2f + 0.1f * float((h >> 4) % 6);
			cut_cycle = 12 + int((h >> 8) % 6);
			// Under 200 years: past that the generator's old-age dieback leaves a beech pollard's
			// crown a few shoots on a bare head (64k leaf triangles at 220 years against 600k).
			params[PARAM_YEARS] = float(140 + int((h >> 16) % 60));
			cut_since = MIN(40 + int((h >> 12) % 60), int(params[PARAM_YEARS]) - 50);
			veteran = MAX(veteran, 0.6f + 0.1f * float((h >> 20) % 4));
		}
	}
	// Climbers on some of the variants of their usual hosts (section 5b): ivy on about a
	// quarter of rough- and smooth-barked broadleaves (and their snags), lianas on another
	// share in rich woods, honeysuckle twining in shrubs. The generator's climber_cover
	// scales them (0: none).
	climber = CLIMBER_NONE;
	if (p_base_cover > 0.0f && (p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_SNAG || p_form == FORM_UNDERSTORY)) {
		const uint32_t bit = 1u << int(p_preset);
		const uint32_t ivy_hosts = (1u << PRESET_OAK) | (1u << PRESET_BEECH) | (1u << PRESET_ASH) | (1u << PRESET_SYCAMORE_MAPLE) | (1u << PRESET_LIME) |
				(1u << PRESET_BLACK_ALDER) | (1u << PRESET_WHITE_WILLOW) | (1u << PRESET_HORSE_CHESTNUT) | (1u << PRESET_HAWTHORN) | (1u << PRESET_ELDER) |
				(1u << PRESET_SUGAR_MAPLE) | (1u << PRESET_RED_MAPLE) | (1u << PRESET_RED_OAK) | (1u << PRESET_WHITE_OAK) | (1u << PRESET_AMERICAN_BEECH) | (1u << PRESET_YELLOW_BIRCH);
		const uint32_t liana_hosts = (1u << PRESET_OAK) | (1u << PRESET_ASH) | (1u << PRESET_LIME) | (1u << PRESET_SYCAMORE_MAPLE) | (1u << PRESET_BLACK_ALDER) |
				(1u << PRESET_WHITE_WILLOW) | (1u << PRESET_SUGAR_MAPLE) | (1u << PRESET_RED_MAPLE) | (1u << PRESET_RED_OAK) | (1u << PRESET_WHITE_OAK);
		const uint32_t twine_hosts = (1u << PRESET_HAZEL) | (1u << PRESET_HAWTHORN) | (1u << PRESET_ROWAN) | (1u << PRESET_ELDER) | (1u << PRESET_WITCH_HAZEL);
		if (p_variant % 4 == 1 && (ivy_hosts & bit)) {
			climber = CLIMBER_IVY;
			climber_cover = p_base_cover * Math::lerp(0.5f, 1.0f, float((uint32_t(seed) * 2654435761u) >> 24) / 255.0f);
		} else if (p_variant % 4 == 3 && p_form != FORM_SNAG && (liana_hosts & bit)) {
			climber = CLIMBER_LIANA;
			climber_cover = p_base_cover;
		} else if (p_variant % 4 == 2 && p_form != FORM_SNAG && (twine_hosts & bit)) {
			climber = CLIMBER_HONEYSUCKLE;
			climber_cover = p_base_cover;
		}
	}
}

// Everything generate() reads, so a copy grows the same tree; the textures are shared.
void TreeGenerator::_copy_generation_state(const TreeGenerator &p_from) {
	preset = p_from.preset;
	seed = p_from.seed;
	memcpy(params, p_from.params, sizeof(params));
	phyllotaxis = p_from.phyllotaxis;
	forest_shade = p_from.forest_shade;
	forest_spacing = p_from.forest_spacing;
	needle_sprays = p_from.needle_sprays;
	skip_lods = p_from.skip_lods;
	bark_relief = p_from.bark_relief;
	rigged = p_from.rigged;
	decay_class = p_from.decay_class;
	fallen = p_from.fallen;
	fallen_branch = p_from.fallen_branch;
	envelope = p_from.envelope;
	collider_radius = p_from.collider_radius;
	veteran = p_from.veteran;
	bark_color = p_from.bark_color;
	leaf_color = p_from.leaf_color;
	junction_weld = p_from.junction_weld;
	season = p_from.season.duplicate(true);
	climber_plant_leaves = p_from.climber_plant_leaves;
	leaf_flutter = p_from.leaf_flutter;
	moss_cover = p_from.moss_cover;
	lichen_cover = p_from.lichen_cover;
	bark_peel = p_from.bark_peel;
	fungi_cover = p_from.fungi_cover;
	fruit_cover = p_from.fruit_cover;
	mistletoe = p_from.mistletoe;
	witches_brooms = p_from.witches_brooms;
	limb_breakage = p_from.limb_breakage;
	uprooted = p_from.uprooted;
	cut_height = p_from.cut_height;
	cut_cycle = p_from.cut_cycle;
	cut_since = p_from.cut_since;
	compress_vertices = p_from.compress_vertices;
	climber = p_from.climber;
	shed_radius_max = p_from.shed_radius_max;
	climber_cover = p_from.climber_cover;
	species_leaf_forms = p_from.species_leaf_forms;
	leaf_texture = p_from.leaf_texture;
	leaflet_texture = p_from.leaflet_texture;
	spray_texture = p_from.spray_texture;
	for (int i = 0; i < BARK_MAX; i++) {
		bark_sets[i] = p_from.bark_sets[i];
	}
	moss_set = p_from.moss_set;
}

void TreeGenerator::_near_task(void *p_self) {
	TreeGenerator *self = static_cast<TreeGenerator *>(p_self);
	self->_setup_variant(self->preset, self->near_form, self->near_task_variant, self->near_base_seed, self->near_base_cover);
	self->material_source = self->near_far_meshes.size() > self->near_task_variant ? Ref<ArrayMesh>(self->near_far_meshes[self->near_task_variant]) : Ref<ArrayMesh>();
	self->near_result = self->generate();
	self->material_source.unref();
	// Keep the copy small between requests: the growth state is only needed while growing.
	self->nodes.reset();
	self->buds.reset();
	self->spurs.reset();
	self->shadow.reset();
	self->supports.reset();
	self->leaf_twins = Builder();
	self->leaf_twin_of.clear();
	self->climber_leaf_b = Builder();
	self->climber_adult_b = Builder();
}

Ref<Mesh> TreeGenerator::request_near_mesh(int p_key, int p_variant) {
	ERR_FAIL_COND_V_MSG(near_key < 0 || p_key != near_key, Ref<Mesh>(), "Not a near mesh source for this key.");
	ERR_FAIL_COND_V(p_variant < 0, Ref<Mesh>());
	// A finished build: hand it over.
	if (near_task != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton()->is_task_completed(near_task)) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(near_task);
		near_task = WorkerThreadPool::INVALID_TASK_ID;
		near_ready = near_result;
		near_ready_variant = near_task_variant;
		near_result.unref();
	}
	// Still in use somewhere (another cell of the same variant): the same mesh. Only the users
	// hold it, so it is freed as soon as the field frees its last near cell.
	if (ObjectID *id = near_built.getptr(p_variant)) {
		Mesh *m = ObjectDB::get_instance<Mesh>(*id);
		if (m) {
			return Ref<Mesh>(m);
		}
		near_built.erase(p_variant);
	}
	if (near_ready.is_valid() && near_ready_variant == p_variant) {
		Ref<Mesh> m = near_ready;
		near_built[p_variant] = m->get_instance_id();
		near_ready.unref();
		near_ready_variant = -1;
		return m;
	}
	if (near_task == WorkerThreadPool::INVALID_TASK_ID) {
		// One build at a time per species, at low priority.
		near_ready.unref(); // Not claimed: the camera moved on.
		near_ready_variant = -1;
		near_task_variant = p_variant;
		near_task = WorkerThreadPool::get_singleton()->add_native_task(&TreeGenerator::_near_task, this, false, "TreeGenerator near mesh");
	}
	return Ref<Mesh>();
}

int TreeGenerator::get_near_mesh_count() const {
	int n = 0;
	for (const KeyValue<int, ObjectID> &b : near_built) {
		n += ObjectDB::get_instance(b.value) ? 1 : 0;
	}
	for (const KeyValue<int, Ref<TreeGenerator>> &kv : near_sources) {
		for (const KeyValue<int, ObjectID> &b : kv.value->near_built) {
			n += ObjectDB::get_instance(b.value) ? 1 : 0;
		}
	}
	return n;
}

TreeGenerator::~TreeGenerator() {
	if (near_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(near_task);
	}
}

Ref<PlantSpecies> TreeGenerator::_stand_species(Preset p_preset, bool p_stand_grown, int p_variants, Form p_form) {
	if (p_form == FORM_OPEN && p_stand_grown) {
		p_form = FORM_STAND;
	}
	// Pollards stand in pasture and along ditches: open-grown (in forest shade their lapsed poles
	// grew 22 m whips with a crown at the top).
	p_stand_grown = p_form != FORM_OPEN && p_form != FORM_POLLARD;
	// Wood lying on the ground does not sway: logs and fallen branches are never rigged.
	const bool saved_rigged = rigged;
	if (p_form == FORM_LOG || p_form == FORM_BRANCH || p_form == FORM_STICKS) {
		rigged = false;
	}
	const int key = (int(p_preset) * int(FORM_MAX) + int(p_form)) * 2 + (rigged ? 1 : 0);
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
		const bool saved_uprooted = uprooted;
		const Climber saved_climber = climber;
		const float saved_climber_cover = climber_cover;
		const float saved_cut_height = cut_height;
		const int saved_cut_cycle = cut_cycle;
		const int saved_cut_since = cut_since;
		const float saved_veteran = veteran;
		if (p_form == FORM_SAPLING) {
			params[PARAM_YEARS] = MAX(6.0f, Math::round(params[PARAM_YEARS] * 0.2f));
		}
		// Leaf detail near, on demand: lobed and compound leaves are simple lenses in the far
		// meshes, and a copy of this generator, as set up here, builds the full-leaf mesh of a
		// variant when the field asks for it (request_near_mesh()).
		const LeafKind leaf_kind = LEAF_FORMS[p_preset].kind;
		const bool near_split = near_leaf_range > 0.0f && p_variants > 0 && params[PARAM_NEEDLES] <= 0.5f &&
				(leaf_kind == LEAF_LOBED || leaf_kind == LEAF_PALMATE || leaf_kind == LEAF_PINNATE || leaf_kind == LEAF_DIGITATE) &&
				(p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_UNDERSTORY || p_form == FORM_SAPLING || p_form == FORM_COPPICE || p_form == FORM_COPPICE_YOUNG || p_form == FORM_POLLARD);
		if (near_split) {
			Ref<TreeGenerator> src;
			src.instantiate();
			src->_copy_generation_state(*this);
			src->near_key = key;
			src->near_form = p_form;
			src->near_base_seed = saved_seed;
			src->near_base_cover = saved_climber_cover;
			near_sources[key] = src;
		} else {
			near_sources.erase(key);
		}
		far_leaves = near_split;
		for (int v = 0; v < n; v++) {
			_setup_variant(p_preset, p_form, v, saved_seed, saved_climber_cover);
			Ref<ArrayMesh> mesh = generate();
			crown += crown_radius;
			if (p_variants > 0) {
				meshes.push_back(mesh);
			}
		}
		far_leaves = false;
		if (near_split) {
			near_sources[key]->near_far_meshes = meshes;
		}
		decay_class = saved_decay;
		fallen = saved_fallen;
		fallen_branch = saved_branch;
		uprooted = saved_uprooted;
		shed_radius_max = 0.14f;
		climber = saved_climber;
		climber_cover = saved_climber_cover;
		cut_height = saved_cut_height;
		cut_cycle = saved_cut_cycle;
		cut_since = saved_cut_since;
		veteran = saved_veteran;
		mesh_cache[key] = meshes;
		crown_cache[key] = crown / n;
		// One full-needle variant for the near range (the field repeats it over the variants):
		// about 30M vertices for a spruce, so one only, and no LOD copies since it is only drawn
		// close. GPU memory and path tracing cost of it are not yet measured.
		if (params[PARAM_NEEDLES] > 0.5f && near_needle_range > 0.0f && p_variants > 0 && (p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_UNDERSTORY)) {
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
	static const char *form_names[FORM_MAX] = { " (open)", " (stand)", " (sapling)", " (snag)", " (log)", " (fallen branch)", " (sticks)", " (understory)", " (coppice)", " (young coppice)", " (pollard)" };
	sp->set_name(String(preset_names[p_preset]) + form_names[p_form]);
	TypedArray<Mesh> meshes = mesh_cache[key];
	if (p_variants < meshes.size()) {
		meshes.resize(p_variants);
	}
	sp->set_meshes(meshes);
	if (near_leaf_range > 0.0f && near_sources.has(key)) {
		// VegetationField3D builds the near cells from it as trees come close; the species
		// holds the source (the field keeps only its id).
		const Ref<TreeGenerator> &src = near_sources[key];
		sp->set_near_mesh_source(src.ptr());
		sp->set_near_mesh_key(key);
		sp->set_meta(SNAME("_near_mesh_source"), src);
		sp->set("near_range", near_leaf_range);
	}
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
		const Color crown_color = leaf.lerp(ls.color, ls.season_blend());
		const float leafy = ls.out * (1.0f - ls.fallen);
		Color far = Color(crown_color.r * self_shade, crown_color.g * self_shade, crown_color.b * self_shade, Math::lerp(0.35f, conifer ? 0.85f : 0.9f, leafy));
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
	near_sources.clear();
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
		// Open-grown crowns (low, wide, limbs to the ground) only where the canopy leaves real
		// light: edges, streams and large gaps. Under a closed canopy they read as orchard trees.
		// Shade tolerance (the habitat's shade_max) says where the species survives; this form
		// also needs the light it grew in.
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, MIN(sp->get_rule_range(PlantSpecies::RULE_SHADE).y, 0.3f)));
		list.push_back(sp);
		return sp;
	};
	int shrub_start = -1; // Where the shrub layer begins in the list.
	auto shrub = [&](Preset p, float p_density) {
		if (shrub_start < 0) {
			shrub_start = list.size();
		}
		// Under a canopy the shrub layer grows in shade: drawn up and thin (the shade form), not
		// the dense dome of a shrub in the open. Hedgerows stand in the open.
		Ref<PlantSpecies> sp = _stand_species(p, false, p_variants, p_stand == STAND_HEDGEROW || p_stand == STAND_WOOD_PASTURE ? FORM_OPEN : FORM_UNDERSTORY);
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
		case STAND_COPPICE_STANDARDS: {
			lead = PRESET_OAK;
			// Coppice with standards, the managed lowland wood of England and France (Rackham,
			// Woodlands, 2006; Peterken 1993): oak standards thinned wide apart (a dozen to thirty
			// to the hectare) so light reaches the underwood, ash among them on moist ground, and
			// beneath them hazel cut in coupes, each a patch cut in its own year; side by side a
			// grown coupe and one cut a year or three ago, of short rods on bare stools.
			// Standards grow up with the underwood round their boles but crowns free on every
			// side: the open-grown form (short clear bole, spreading crown), not the drawn-up
			// high-forest one.
			Ref<PlantSpecies> oak = _stand_species(PRESET_OAK, false, p_variants);
			oak->set_density(0.35f);
			oak->set_spacing(oak->get_spacing() * 1.4f);
			list.push_back(oak);
			Ref<PlantSpecies> ash = _stand_species(PRESET_ASH, false, p_variants);
			ash->set_density(0.3f);
			ash->set_spacing(ash->get_spacing() * 1.4f);
			ash->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
			ash->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
			list.push_back(ash);
			ash->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(6.0f, 1000.0f));
			ash->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.5f);
			for (int q = 0; q < 2; q++) {
				Ref<PlantSpecies> hz = _stand_species(PRESET_HAZEL, true, p_variants, q == 0 ? FORM_COPPICE : FORM_COPPICE_YOUNG);
				if (shrub_start < 0) {
					shrub_start = list.size();
				}
				hz->set_density(1.0f);
				// Coupes about 40 m across, alternating.
				hz->set_patch_scale(40.0f);
				hz->set_patch_cover(0.5f);
				hz->set_patch_seed(patch_seed + 11);
				hz->set_patch_invert(q == 1);
				// The standards are spaced for the underwood's light: it grows under them.
				hz->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.9f));
				list.push_back(hz);
			}
			shrub(PRESET_HAWTHORN, 0.15f);
		} break;
		case STAND_WOOD_PASTURE: {
			// Wood pasture (Vera 2000; Read, Pollards, 2000): grazed, so no underwood and almost no
			// regeneration; lapsed beech and oak pollards, open-grown oaks between, thorn and holly
			// in the grazing's shelter, old wood lying where it fell.
			Ref<PlantSpecies> oak = open(PRESET_OAK, 0.3f);
			oak->set_spacing(oak->get_spacing() * 3.0f);
			for (Preset pp : { PRESET_BEECH, PRESET_OAK }) {
				Ref<PlantSpecies> pol = _stand_species(pp, false, p_variants, FORM_POLLARD);
				pol->set_density(0.35f);
				// Grazed parkland: a tree every 20-30 m, crowns apart (Vera 2000).
				pol->set_spacing(pol->get_spacing() * 3.0f);
				pol->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.4f));
				list.insert(0, pol);
			}
			// Thorn and holly as scattered bushes in the grass, not a thicket: browsing keeps
			// them apart.
			for (Preset sh : { PRESET_HAWTHORN, PRESET_HOLLY }) {
				Ref<PlantSpecies> bush = shrub(sh, sh == PRESET_HAWTHORN ? 0.3f : 0.2f);
				bush->set_spacing(bush->get_spacing() * 5.0f);
			}
			Ref<PlantSpecies> log = _stand_species(PRESET_OAK, true, p_variants, FORM_LOG);
			log->set_density(0.2f);
			list.push_back(log);
			Ref<PlantSpecies> branch = _stand_species(PRESET_OAK, true, p_variants, FORM_BRANCH);
			branch->set_density(0.4f);
			list.push_back(branch);
		} break;
		case STAND_POLLARD_ROW: {
			// Willow pollards along ditches and rivers, cut every few years for poles and withies,
			// a few metres apart, black alder between (placed by the scene along its own lines,
			// like a hedgerow).
			Ref<PlantSpecies> wil = _stand_species(PRESET_WHITE_WILLOW, false, p_variants, FORM_POLLARD);
			wil->set_density(1.0f);
			wil->set_spacing(5.0f);
			list.push_back(wil);
			open(PRESET_BLACK_ALDER, 0.2f);
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
		// A worked coppice is tidied: its standards are felled for timber before they die, so it
		// holds a fraction of a wild wood's deadwood.
		const float dead = p_stand == STAND_COPPICE_STANDARDS ? 0.35f : 1.0f;
		Ref<PlantSpecies> snag = _stand_species(lead, true, p_variants, FORM_SNAG);
		snag->set_density(0.25f * dead);
		Ref<PlantSpecies> log = _stand_species(lead, true, p_variants, FORM_LOG);
		log->set_density(0.4f * dead);
		Ref<PlantSpecies> branch = _stand_species(lead, true, p_variants, FORM_BRANCH);
		branch->set_density(0.7f);
		Ref<PlantSpecies> sticks = _stand_species(lead, true, p_variants, FORM_STICKS);
		sticks->set_density(1.0f);
		for (const Ref<PlantSpecies> &extra : { snag, log, sap, branch, sticks }) {
			// Inside the stand's own belt.
			for (int r = 0; r < PlantSpecies::RULE_MAX; r++) {
				if (PlantSpecies::Rule(r) == PlantSpecies::RULE_SHADE) {
					continue;
				}
				extra->set_rule_range(PlantSpecies::Rule(r), first->get_rule_range(PlantSpecies::Rule(r)));
				extra->set_rule_fade(PlantSpecies::Rule(r), first->get_rule_fade(PlantSpecies::Rule(r)));
			}
			if ((extra == snag || extra == log) && shrub_start >= 0) {
				// Dead trees stand and lie where they fell, before the shrub layer fills the
				// floor around them (placed after it, a dense understory left no room for them).
				list.insert(shrub_start, extra);
				shrub_start++;
			} else {
				list.push_back(extra);
			}
		}
	}
	if (p_stand == STAND_HEDGEROW || p_stand == STAND_POLLARD_ROW) {
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
	sprout_roots.clear();
	cut_heads.clear();
	cut_stubs.clear();
	seat_cuts.clear();
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
	int last_cut = -1;
	int first_cut = -1;
	int cuts = 0;
	last_cut_year = -1000;
	const int since = cut_since >= 0 ? cut_since : 2 + int(uint32_t(seed) * 2654435761u % uint32_t(MAX(cut_cycle - 2, 1)));
	for (int y = 1; y <= years; y++) {
		// A cut tree's sprouts are its poles, from the head or the stool: no more stems from the base.
		if (params[PARAM_BASAL_SPROUTS] > 0.0f && rng.randf() < params[PARAM_BASAL_SPROUTS] && last_cut < 0) {
			add_stem_bud(_rand(0.0f, Math::TAU), spread * _rand(0.3f, 1.0f));
		}
		_grow_year(y);
		if (cut_height > 0.0f) {
			// Cut back every cycle (trees.md 5l), first once the stem stands well over the cut;
			// the last cut cut_since years before the end.
			const float top = _stem_top();
			if (last_cut < 0 ? (y >= 8 && top > cut_height + 2.0f && y <= years - since) : (y - last_cut >= cut_cycle && y <= years - since)) {
				_cut_back(last_cut < 0, y);
				if (last_cut < 0) {
					first_cut = y;
				}
				last_cut = y;
				cuts++;
				last_cut_year = y;
			}
			if (first_cut > 0 && cut_height > 0.5f) {
				// The pipe model sizes wood by the crown it carries, and a pollard's crown is a
				// few young poles: its bole stopped thickening at the first cut. It lays a ring a
				// year all the same (2-3 mm on beech and oak, 5-6 mm on willow; Read 2000), so an
				// old pollard is a broad bole under a broader head.
				const float ring = preset == PRESET_WHITE_WILLOW ? 0.003f : 0.0015f;
				for (Node &n : nodes) {
					// The bole only: knuckles (born 0) swell with the head's callus instead.
					if (!n.removed && n.order == 0 && n.born > 0 && n.born <= first_cut) {
						n.radius += ring;
					}
				}
			}
		}
		if (cut_height > 0.5f && last_cut > 0 && y == last_cut + cut_cycle + 2 && y > years - since && preset != PRESET_WHITE_WILLOW) {
			// A lapsed pollard self-thins (Read 2000; Lonsdale, Ancient and other veteran trees,
			// 2013): a few poles take the lead and grow on into the limbs of a new crown, the
			// rest die back to stubs. Left all to compete, dozens of poles shared the head's
			// vigour and after sixty years made a 10 m crown of whips.
			LocalVector<Pair<int, int>> rank; // (subtree nodes, first node)
			LocalVector<int> st;
			for (const int r : sprout_roots) {
				if (r < 0 || r >= int(nodes.size()) || nodes[r].removed || nodes[r].dead) {
					continue;
				}
				int count = 0;
				st.clear();
				st.push_back(r);
				while (!st.is_empty()) {
					const int j = st[st.size() - 1];
					st.resize(st.size() - 1);
					if (nodes[j].removed) {
						continue;
					}
					count++;
					if (nodes[j].main >= 0) {
						st.push_back(nodes[j].main);
					}
					for (int c = nodes[j].first_lateral; c >= 0; c = nodes[c].next_sibling) {
						st.push_back(c);
					}
				}
				rank.push_back(Pair<int, int>(-count, r));
			}
			rank.sort_custom<PairSort<int, int>>();
			const int keep = 5 + int(uint32_t(seed) * 2246822519u % 4u);
			for (uint32_t k = keep; k < rank.size(); k++) {
				// The loser dies where it stands: a dead stub of its first internode, the rest gone.
				const int r = rank[k].second;
				nodes[r].dead = true;
				nodes[r].leafy = false;
				st.clear();
				if (nodes[r].main >= 0) {
					st.push_back(nodes[r].main);
				}
				for (int c = nodes[r].first_lateral; c >= 0; c = nodes[c].next_sibling) {
					st.push_back(c);
				}
				while (!st.is_empty()) {
					const int j = st[st.size() - 1];
					st.resize(st.size() - 1);
					nodes[j].removed = true;
					if (nodes[j].main >= 0) {
						st.push_back(nodes[j].main);
					}
					for (int c = nodes[j].first_lateral; c >= 0; c = nodes[c].next_sibling) {
						st.push_back(c);
					}
				}
			}
		}
		if (int(nodes.size()) >= MAX_NODES - 64) {
			break;
		}
	}
	_compute_radii();
	bolling.clear();
	if (!cut_heads.is_empty()) {
		// The bolling (trees.md 5l): callus rolled over the cuts, cycle after cycle, swells the
		// head and its knuckles past the stem below; the stem flares into it over its last half
		// metre.
		HashSet<int> &head = bolling;
		// The ramp below a knuckle must not take its stem top first: the top gets its own swell.
		HashSet<int> heads;
		for (const int i : cut_heads) {
			heads.insert(i);
		}
		for (const int i : cut_heads) {
			if (!nodes[i].removed && !head.has(i)) {
				head.insert(i);
				// The stem's top swells most, the knuckles and stubs on it less.
				// Each cut adds its roll of callus: an old pollard's head stands half as wide again as the
				// bole.
				const bool stem_top = nodes[i].order == 0 && nodes[i].parent > 0 && nodes[nodes[i].parent].order == 0 && nodes[nodes[i].parent].main == i && nodes[i].first_lateral >= 0;
				const int seat_n = seat_cuts.has(i) ? seat_cuts[i] : 0;
				// The bole under it lays its annual rings too (generate()), so the head stands
				// out from it by its callus alone.
				nodes[i].radius *= stem_top ? MIN(1.2f + 0.05f * cuts, 1.5f) : MIN(1.15f + 0.04f * seat_n, 1.35f);
				if (!stem_top && cut_height <= 0.5f && seat_n > 0) {
					// A stool's lobes: the callus of every cut, fused into a low knobbly mound.
					nodes[i].radius = MAX(nodes[i].radius, 0.4f * nodes[i].length);
				}
				for (int p = nodes[i].parent, k = 0; p > 0 && k < 6 && nodes[i].start.distance_to(nodes[p].end()) < 0.6f; p = nodes[p].parent, k++) {
					if (!head.has(p) && !heads.has(p)) {
						nodes[p].radius *= Math::lerp(1.3f, 1.05f, nodes[i].start.distance_to(nodes[p].start) / 0.6f);
						head.insert(p);
					}
				}
			}
		}
		// The knuckles were set on the stem when it was first cut, a few centimetres long; the
		// head has swollen round them since. Each reaches out to the head's surface, carrying
		// its stubs and poles with it.
		LocalVector<uint8_t> moved;
		for (const int i : cut_heads) {
			Node &kn = nodes[i];
			if (kn.removed || kn.parent <= 0 || !bolling.has(kn.parent) || nodes[kn.parent].main == i || sprout_roots.has(i)) {
				continue;
			}
			const Node &par = nodes[kn.parent];
			if (par.order != kn.order || kn.born != 0 || kn.length >= par.radius * 0.9f) {
				continue;
			}
			const float grow = par.radius * 0.9f - kn.length;
			const Vector3 shift = kn.dir * grow;
			kn.length += grow;
			moved.resize(nodes.size());
			memset(moved.ptr(), 0, moved.size());
			moved[i] = 1;
			for (int c = i + 1; c < int(nodes.size()); c++) {
				const int p = nodes[c].parent;
				if (p >= 0 && moved[p]) {
					moved[c] = 1;
					nodes[c].start += shift;
				}
			}
		}
		// Sprouts grow as laterals from the knuckle's end, and the knuckle itself has no
		// continuation: it ended in a rounded cap with its thickest pole leaving it as a fork,
		// which sank the pole's base into a round socket in the cap. The thickest pole is the
		// knuckle's continuation instead, so the knuckle runs on into it.
		for (const int i : cut_heads) {
			Node &kn = nodes[i];
			if (kn.removed || (kn.main >= 0 && !nodes[kn.main].removed)) {
				continue;
			}
			int best = -1, prev_best = -1;
			for (int l = kn.first_lateral, prev = -1; l >= 0; prev = l, l = nodes[l].next_sibling) {
				if (!nodes[l].removed && nodes[l].pole && !cut_stubs.has(l) && (best < 0 || nodes[l].radius > nodes[best].radius)) {
					best = l;
					prev_best = prev;
				}
			}
			if (best >= 0) {
				if (prev_best >= 0) {
					nodes[prev_best].next_sibling = nodes[best].next_sibling;
				} else {
					kn.first_lateral = nodes[best].next_sibling;
				}
				nodes[best].next_sibling = -1;
				kn.main = best;
			}
		}
	}
	_bend_under_gravity();
	_prune_collisions();
	_measure();
	_mark_spurs(years);
	broken_tip = -1;
	// Decay recolours bark and needles for this mesh only, so a generator reused for several
	// classes (the stand snags and logs) starts each one from the living colours.
	const Color living_bark = bark_color;
	const Color living_leaf = leaf_color;
	veteran_features.clear();
	veteran_stem = -1;
	head_r = 0.0f;
	if (decay_class > 0) {
		_apply_decay();
	} else if (veteran > 0.0f && !fallen && !fallen_branch) {
		_apply_veteran();
	}
	if (decay_class == 0 && !fallen && !fallen_branch && cut_height > 0.5f && cuts > 0) {
		// A pollard's head is knobbly, not a smooth bulb: each cut's callus rolls over the stubs
		// and dormant buds as burrs and knuckles (Read 2000), more of them with every cycle. They
		// ride on the old-tree features' burls (5e), round the head.
		int stem = veteran_stem;
		for (int i = 1; i < int(nodes.size()) && stem < 0; i++) {
			if (!nodes[i].removed && nodes[i].parent == 0) {
				stem = i;
			}
		}
		if (stem >= 0) {
			veteran_stem = stem;
			RandomPCG hr;
			hr.seed(uint64_t(seed) * 0x9e3779b1u + 0x68656164u);
			float r_head = 0.0f;
			for (int i = stem; i >= 0 && !nodes[i].removed; i = nodes[i].main) {
				r_head = MAX(r_head, nodes[i].radius);
			}
			const int knobs = MIN(4 + 2 * cuts, 14);
			const float az0 = hr.randf() * float(Math::TAU);
			for (int k = 0; k < knobs; k++) {
				const float hh = r_head * Math::lerp(0.18f, 0.32f, hr.randf());
				const float h = cut_height + r_head * Math::lerp(-0.9f, 0.1f, hr.randf());
				const float az = az0 + float(Math::TAU) * (k + 0.4f * (hr.randf() - 0.5f)) / knobs;
				// Low: the knobs over them make the burr; a tall smooth dome read as clay.
				veteran_features.push_back({ VET_BURL, h, az, hh, hh * Math::lerp(0.8f, 1.3f, hr.randf()), Math::lerp(0.12f, 0.25f, hr.randf()) });
				if (hr.randf() < 0.6f) {
					// Water shoots from the burr's dormant buds.
					veteran_features.push_back({ VET_EPICORMIC, h, az, 0.0f, 0.0f, Math::lerp(2.0f, 5.0f, hr.randf()) });
				}
			}
			head_y0 = nodes[0].start.y + cut_height - 1.4f * r_head;
			head_r = r_head;
		}
	}
	plate.on = false;
	hanger_bases.clear();
	if (decay_class == 0 && !fallen && !fallen_branch && envelope.size == Vector3()) {
		_break_limbs();
	}
	if (fallen_branch) {
		_shed_branch();
	} else if (fallen) {
		_fell();
	}
	// Where the wood lies (mesh space), for plants grown through and around it: the thickest
	// axis from the root. Its butt is measured from its bark as the mesh is built.
	deadwood_stem = -1;
	int dw_last = -1;
	if (fallen || fallen_branch) {
		for (int i = 1; i < int(nodes.size()); i++) {
			const Node &n = nodes[i];
			if (n.removed || (n.parent >= 0 && !nodes[n.parent].removed && nodes[n.parent].main == i)) {
				continue;
			}
			if (deadwood_stem < 0 || n.radius > nodes[deadwood_stem].radius) {
				deadwood_stem = i;
			}
		}
		if (deadwood_stem >= 0) {
			for (int k = deadwood_stem; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
				dw_last = k;
			}
			deadwood_a = nodes[deadwood_stem].start;
			deadwood_b = nodes[dw_last].end();
			for (float &r : deadwood_profile) {
				r = 0.0f;
			}
			// The centreline through the stem's nodes, resampled evenly by arc length.
			LocalVector<Vector3> pl;
			pl.push_back(deadwood_a);
			for (int k = deadwood_stem; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
				pl.push_back(nodes[k].end());
			}
			LocalVector<float> arc;
			arc.push_back(0.0f);
			for (uint32_t k = 1; k < pl.size(); k++) {
				arc.push_back(arc[k - 1] + pl[k].distance_to(pl[k - 1]));
			}
			uint32_t seg = 0;
			for (int k = 0; k < DEADWOOD_SAMPLES; k++) {
				const float want = arc[arc.size() - 1] * float(k) / float(DEADWOOD_SAMPLES - 1);
				while (seg + 2 < pl.size() && arc[seg + 1] < want) {
					seg++;
				}
				const float span = arc[seg + 1] - arc[seg];
				const float t = span > 1e-6f ? CLAMP((want - arc[seg]) / span, 0.0f, 1.0f) : 0.0f;
				deadwood_line[k] = pl[seg].lerp(pl[seg + 1], t);
				deadwood_girth[k] = 0.0f;
			}
		}
	}
	Ref<ArrayMesh> mesh = _build_mesh();
	if (deadwood_stem >= 0) {
		// Layout (trees.md 5b): a (3), b (3), the widest reach from the axis anywhere along it
		// (a reader of the first 7 floats keeps out of all the wood), the sample count N, then
		// the reach at N points evenly from a to b (linear between them); then, appended (8 + 5N
		// floats), N points along the stem's own centreline, each x, y, z and the girth round it,
		// so a bent stick's bow is not read as girth.
		const Vector3 a = deadwood_a, b = deadwood_b;
		float widest = nodes[deadwood_stem].radius;
		for (int i = 0; i < DEADWOOD_SAMPLES; i++) {
			// A bin the tube skips (a short stem) takes its neighbours' reach.
			if (deadwood_profile[i] <= 0.0f) {
				deadwood_profile[i] = i > 0 ? deadwood_profile[i - 1] : nodes[deadwood_stem].radius;
			}
			widest = MAX(widest, deadwood_profile[i]);
		}
		PackedFloat32Array axis;
		for (float v : { a.x, a.y, a.z, b.x, b.y, b.z, widest, float(DEADWOOD_SAMPLES) }) {
			axis.push_back(v);
		}
		for (int i = 0; i < DEADWOOD_SAMPLES; i++) {
			axis.push_back(deadwood_profile[i]);
		}
		for (int i = 0; i < DEADWOOD_SAMPLES; i++) {
			if (deadwood_girth[i] <= 0.0f) {
				deadwood_girth[i] = i > 0 ? deadwood_girth[i - 1] : nodes[deadwood_stem].radius;
			}
			const Vector3 &q = deadwood_line[i];
			for (float v : { q.x, q.y, q.z, deadwood_girth[i] }) {
				axis.push_back(v);
			}
		}
		mesh->set_meta(SNAME("tree_deadwood_axis"), axis);
		if (plate.on) {
			// The pit the plate came out of (trees.md 5k), for the terrain to dig and the ground
			// plants to keep bare: centre (the old foot), radius, depth. Deepest when fresh: about
			// 0.95 m at a year, 0.5 m at five (Zadrozny et al. 2017).
			const float depth = CLAMP(1.6f * plate.thick, 0.35f, 0.95f) * (1.0f - 0.2f * MAX(decay_class - 1, 0));
			PackedFloat32Array pit;
			for (float v : { plate.base0.x, plate.base0.y, plate.base0.z, plate.radius * 0.9f, depth }) {
				pit.push_back(v);
			}
			mesh->set_meta(SNAME("tree_root_pit"), pit);
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
	ClassDB::bind_method(D_METHOD("set_uprooted", "uprooted"), &TreeGenerator::set_uprooted);
	ClassDB::bind_method(D_METHOD("set_cut_height", "height"), &TreeGenerator::set_cut_height);
	ClassDB::bind_method(D_METHOD("get_cut_height"), &TreeGenerator::get_cut_height);
	ClassDB::bind_method(D_METHOD("set_cut_cycle", "years"), &TreeGenerator::set_cut_cycle);
	ClassDB::bind_method(D_METHOD("get_cut_cycle"), &TreeGenerator::get_cut_cycle);
	ClassDB::bind_method(D_METHOD("set_cut_since", "years"), &TreeGenerator::set_cut_since);
	ClassDB::bind_method(D_METHOD("get_cut_since"), &TreeGenerator::get_cut_since);
	ClassDB::bind_method(D_METHOD("is_uprooted"), &TreeGenerator::is_uprooted);
	ClassDB::bind_method(D_METHOD("set_moss_cover", "cover"), &TreeGenerator::set_moss_cover);
	ClassDB::bind_method(D_METHOD("set_lichen_cover", "cover"), &TreeGenerator::set_lichen_cover);
	ClassDB::bind_method(D_METHOD("get_lichen_cover"), &TreeGenerator::get_lichen_cover);
	ClassDB::bind_method(D_METHOD("set_bark_peel", "peel"), &TreeGenerator::set_bark_peel);
	ClassDB::bind_method(D_METHOD("get_bark_peel"), &TreeGenerator::get_bark_peel);
	ClassDB::bind_method(D_METHOD("set_fungi_cover", "cover"), &TreeGenerator::set_fungi_cover);
	ClassDB::bind_method(D_METHOD("set_fruit_cover", "cover"), &TreeGenerator::set_fruit_cover);
	ClassDB::bind_method(D_METHOD("get_fruit_cover"), &TreeGenerator::get_fruit_cover);
	ClassDB::bind_method(D_METHOD("set_mistletoe", "amount"), &TreeGenerator::set_mistletoe);
	ClassDB::bind_method(D_METHOD("get_mistletoe"), &TreeGenerator::get_mistletoe);
	ClassDB::bind_method(D_METHOD("set_witches_brooms", "amount"), &TreeGenerator::set_witches_brooms);
	ClassDB::bind_method(D_METHOD("get_witches_brooms"), &TreeGenerator::get_witches_brooms);
	ClassDB::bind_method(D_METHOD("get_fungi_cover"), &TreeGenerator::get_fungi_cover);
	ClassDB::bind_method(D_METHOD("set_limb_breakage", "breakage"), &TreeGenerator::set_limb_breakage);
	ClassDB::bind_method(D_METHOD("get_limb_breakage"), &TreeGenerator::get_limb_breakage);
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("get_leaf_form", "preset"), &TreeGenerator::get_leaf_form);
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("get_leaf_season", "preset", "season"), &TreeGenerator::get_leaf_season);
	ClassDB::bind_method(D_METHOD("set_season", "season"), &TreeGenerator::set_season);
	ClassDB::bind_method(D_METHOD("set_junction_weld", "enabled"), &TreeGenerator::set_junction_weld);
	ClassDB::bind_method(D_METHOD("set_climber_plant_leaves", "enabled"), &TreeGenerator::set_climber_plant_leaves);
	ClassDB::bind_method(D_METHOD("set_leaf_flutter", "amount"), &TreeGenerator::set_leaf_flutter);
	ClassDB::bind_method(D_METHOD("set_veteran", "amount"), &TreeGenerator::set_veteran);
	ClassDB::bind_method(D_METHOD("get_veteran"), &TreeGenerator::get_veteran);
	ClassDB::bind_method(D_METHOD("get_veteran_feature_count"), &TreeGenerator::get_veteran_feature_count);
	ClassDB::bind_method(D_METHOD("get_veteran_features"), &TreeGenerator::get_veteran_features);
	ClassDB::bind_method(D_METHOD("get_collision_prune_count"), &TreeGenerator::get_collision_prune_count);
	ClassDB::bind_method(D_METHOD("get_collision_deflect_count"), &TreeGenerator::get_collision_deflect_count);
	ClassDB::bind_method(D_METHOD("get_leaf_flutter"), &TreeGenerator::get_leaf_flutter);
	ClassDB::bind_method(D_METHOD("get_climber_plant_leaves"), &TreeGenerator::get_climber_plant_leaves);
	ClassDB::bind_method(D_METHOD("get_junction_weld"), &TreeGenerator::get_junction_weld);
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
	ClassDB::bind_method(D_METHOD("set_near_leaf_range", "range"), &TreeGenerator::set_near_leaf_range);
	ClassDB::bind_method(D_METHOD("get_near_leaf_range"), &TreeGenerator::get_near_leaf_range);
	ClassDB::bind_method(D_METHOD("request_near_mesh", "key", "variant"), &TreeGenerator::request_near_mesh);
	ClassDB::bind_method(D_METHOD("get_near_mesh_count"), &TreeGenerator::get_near_mesh_count);
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "junction_weld"), "set_junction_weld", "get_junction_weld");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "climber_plant_leaves"), "set_climber_plant_leaves", "get_climber_plant_leaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "leaf_flutter", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_leaf_flutter", "get_leaf_flutter");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "veteran", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_veteran", "get_veteran");
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "uprooted"), "set_uprooted", "is_uprooted");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cut_height", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m"), "set_cut_height", "get_cut_height");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cut_cycle", PROPERTY_HINT_RANGE, "2,60,1,suffix:years"), "set_cut_cycle", "get_cut_cycle");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cut_since", PROPERTY_HINT_RANGE, "-1,300,1,suffix:years"), "set_cut_since", "get_cut_since");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moss_cover", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_moss_cover", "get_moss_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lichen_cover", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_lichen_cover", "get_lichen_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bark_peel", PROPERTY_HINT_RANGE, "0,1.5,0.01"), "set_bark_peel", "get_bark_peel");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fungi_cover", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_fungi_cover", "get_fungi_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fruit_cover", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_fruit_cover", "get_fruit_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mistletoe", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_mistletoe", "get_mistletoe");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "witches_brooms", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_witches_brooms", "get_witches_brooms");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "limb_breakage", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_limb_breakage", "get_limb_breakage");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "compress_vertices"), "set_compress_vertices", "is_compress_vertices");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "climber", PROPERTY_HINT_ENUM, "None,Ivy,Liana,Honeysuckle"), "set_climber", "get_climber");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "climber_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_climber_cover", "get_climber_cover");
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "envelope"), "set_envelope", "get_envelope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collider_radius", PROPERTY_HINT_RANGE, "0,1,0.01,suffix:m"), "set_collider_radius", "get_collider_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_needle_range", PROPERTY_HINT_RANGE, "0,100,0.5,suffix:m"), "set_near_needle_range", "get_near_needle_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_leaf_range", PROPERTY_HINT_RANGE, "0,200,0.5,suffix:m"), "set_near_leaf_range", "get_near_leaf_range");
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
	BIND_ENUM_CONSTANT(STAND_COPPICE_STANDARDS);
	BIND_ENUM_CONSTANT(STAND_WOOD_PASTURE);
	BIND_ENUM_CONSTANT(STAND_POLLARD_ROW);
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

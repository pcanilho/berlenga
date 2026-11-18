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
#include "vegetation_fire.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

#include "modules/weather/weather_map_3d.h"

static constexpr int MAX_NODES = 150000;

static const char *preset_names[TreeGenerator::PRESET_MAX] = {
	"Oak", "Beech", "Birch", "Scots Pine", "Norway Spruce", "Hazel", "Hawthorn", "Lime", "Sycamore Maple", "Ash",
	"Horse Chestnut", "Black Alder", "White Willow", "European Larch", "Silver Fir", "Rowan", "Elder", "Holly", "Yew", "Box",
	"Sugar Maple", "Red Maple", "Northern Red Oak", "White Oak", "Eastern White Pine", "Eastern Hemlock", "American Beech",
	"Paper Birch", "Yellow Birch", "Witch-hazel", "Mountain Laurel", "Gorse", "Broom", "Gum Rockrose", "Kermes Oak", "Lentisk", "Dwarf Fan Palm",
	"Stone Pine", "Cork Oak", "Holm Oak", "Wild Olive", "Maritime Pine", "Carob",
	"Blackthorn", "Field Maple", "Dog Rose"
};

// The woody shrubs vegetation places through make_shrub_species (trees.md 5u, 5v): Atlantic heath
// and Mediterranean scrub.
// Bank trees (trees.md 5q) step through four slopes for placement to pick from. A stand asked for
// one variant (scouts and tests on the cloud) gets one bank tree: four slope variants of three
// bank species added some 47M triangles to make_woodland(1) (a bank willow is about 10M) and the
// forest no longer fit in 15 GB.
static int _bank_variants(int p_variants) {
	return p_variants <= 0 ? 0 : (p_variants == 1 ? 1 : MAX(p_variants, 4));
}

static bool scrub_shrub(TreeGenerator::Preset p_preset) {
	return p_preset == TreeGenerator::PRESET_GORSE || p_preset == TreeGenerator::PRESET_BROOM || p_preset == TreeGenerator::PRESET_GUM_ROCKROSE ||
			p_preset == TreeGenerator::PRESET_KERMES_OAK || p_preset == TreeGenerator::PRESET_LENTISK || p_preset == TreeGenerator::PRESET_DWARF_FAN_PALM;
}

// Woody hedge shrubs (trees.md 5ad): make_shrub_species takes them too, with the cut hedge form.
static bool hedge_shrub(TreeGenerator::Preset p_preset) {
	return p_preset == TreeGenerator::PRESET_HAWTHORN || p_preset == TreeGenerator::PRESET_BLACKTHORN || p_preset == TreeGenerator::PRESET_HAZEL ||
			p_preset == TreeGenerator::PRESET_FIELD_MAPLE || p_preset == TreeGenerator::PRESET_DOG_ROSE;
}

static const char *stand_names[TreeGenerator::STAND_MAX] = {
	"Beech Forest", "Oak Mixed Forest", "Montane Conifer Forest", "Subalpine Forest", "Pine Heath", "Riparian Woodland", "Hedgerow",
	"Northern Hardwood Forest", "Eastern Oak Forest", "Pine-Hemlock Forest", "Coppice with Standards", "Wood Pasture", "Pollard Row",
	"Stone Pine Parkland"
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
		case PRESET_GORSE: // Ulex europaeus: dense, rounded, many-stemmed; spines for leaves (trees.md 5u).
			// Many stems from one rootstock, the outer ones splayed near flat, branching wide and
			// densely, so it grows a dome leafy nearly to the ground rather than a crown on stems.
			p[PARAM_YEARS] = 12;
			p[PARAM_MATURITY] = 6;
			p[PARAM_INTERNODE_LENGTH] = 0.08f;
			p[PARAM_INTERNODE_LENGTH_AGE] = 0.96f;
			p[PARAM_MAX_SHOOT] = 3;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.49f;
			p[PARAM_BRANCH_ANGLE] = 60;
			p[PARAM_BRANCH_ANGLE_VARIANCE] = 14;
			p[PARAM_DIRECTION_NOISE] = 16;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.15f;
			p[PARAM_GRAVITROPISM] = -0.05f;
			p[PARAM_SHED_RATIO] = 0.04f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.7f; // The dead inside stays, grey and spiny.
			p[PARAM_TIP_RADIUS] = 0.0016f;
			p[PARAM_PIPE_EXPONENT] = 2.8f; // Many thin stems, not one trunk.
			p[PARAM_STEMS] = 12;
			p[PARAM_STEM_SPREAD] = 75;
			p[PARAM_BASAL_SPROUTS] = 0.35f;
			p[PARAM_ROOT_FLARE] = 0.15f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.022f; // Spines 15-30 mm (primary to 40).
			p[PARAM_LEAVES_PER_INTERNODE] = 12;
			p[PARAM_LEAF_YEARS] = 5;
			bark_color = Color(0.4f, 0.37f, 0.31f);
			leaf_color = Color(0.09f, 0.16f, 0.08f); // Glaucous dark green.
			break;
		case PRESET_BROOM: // Cytisus scoparius: erect, many straight green switches, few small leaves (5u).
			p[PARAM_YEARS] = 10;
			p[PARAM_MATURITY] = 5;
			p[PARAM_INTERNODE_LENGTH] = 0.12f;
			p[PARAM_INTERNODE_LENGTH_AGE] = 0.96f;
			p[PARAM_MAX_SHOOT] = 4;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_BRANCH_ANGLE] = 28;
			p[PARAM_BRANCH_ANGLE_VARIANCE] = 6;
			p[PARAM_DIRECTION_NOISE] = 5;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.7f;
			p[PARAM_GRAVITROPISM] = 0.35f;
			p[PARAM_BUD_LIGHT_MIN] = 0.08f;
			p[PARAM_SHED_RATIO] = 0.04f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.5f;
			p[PARAM_TIP_RADIUS] = 0.0015f;
			p[PARAM_PIPE_EXPONENT] = 2.6f;
			p[PARAM_STEMS] = 14;
			p[PARAM_STEM_SPREAD] = 30;
			p[PARAM_BASAL_SPROUTS] = 0.25f;
			p[PARAM_ROOT_FLARE] = 0.15f;
			p[PARAM_LEAF_SIZE] = 0.011f; // Leaflets 4-16 mm.
			p[PARAM_LEAF_WIDTH] = 0.42f;
			p[PARAM_LEAVES_PER_INTERNODE] = 6;
			bark_color = Color(0.42f, 0.38f, 0.3f);
			leaf_color = Color(0.1f, 0.2f, 0.06f);
			break;
		case PRESET_GUM_ROCKROSE: // Cistus ladanifer: few erect, sticky, little-branched stems; open and leggy (5v).
			p[PARAM_YEARS] = 8;
			p[PARAM_MATURITY] = 4;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_INTERNODE_LENGTH] = 0.12f;
			p[PARAM_INTERNODE_LENGTH_AGE] = 0.95f;
			p[PARAM_MAX_SHOOT] = 3;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.52f;
			p[PARAM_BRANCH_ANGLE] = 30;
			p[PARAM_BRANCH_ANGLE_VARIANCE] = 8;
			p[PARAM_DIRECTION_NOISE] = 8;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.8f;
			p[PARAM_GRAVITROPISM] = 0.3f;
			p[PARAM_BUD_LIGHT_MIN] = 0.15f; // Few shoots: an open shrub, its stems seen through it.
			p[PARAM_SHED_RATIO] = 0.06f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.5f;
			p[PARAM_TIP_RADIUS] = 0.002f;
			p[PARAM_PIPE_EXPONENT] = 2.5f;
			p[PARAM_STEMS] = 6;
			p[PARAM_STEM_SPREAD] = 38; // Several stems from the base, fanning out.
			p[PARAM_BASAL_SPROUTS] = 0.2f;
			p[PARAM_ROOT_FLARE] = 0.1f;
			p[PARAM_LEAF_SIZE] = 0.065f; // Leaves 4-8 cm by 0.6-2 cm, linear-lanceolate.
			p[PARAM_LEAF_WIDTH] = 0.11f;
			p[PARAM_LEAVES_PER_INTERNODE] = 10;
			p[PARAM_LEAF_YEARS] = 2;
			bark_color = Color(0.38f, 0.28f, 0.2f); // Red-brown, glossy with resin.
			leaf_color = Color(0.05f, 0.1f, 0.04f); // Dark, varnished.
			break;
		case PRESET_KERMES_OAK: // Quercus coccifera: dense, intricately branched, spiny-leaved evergreen mound (5v).
			p[PARAM_YEARS] = 18;
			p[PARAM_MATURITY] = 8;
			p[PARAM_INTERNODE_LENGTH] = 0.06f;
			p[PARAM_INTERNODE_LENGTH_AGE] = 0.96f;
			p[PARAM_MAX_SHOOT] = 3;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.49f;
			p[PARAM_BRANCH_ANGLE] = 55;
			p[PARAM_BRANCH_ANGLE_VARIANCE] = 15;
			p[PARAM_DIRECTION_NOISE] = 20; // Zigzag, intricate twigs.
			p[PARAM_TRUNK_GRAVITROPISM] = 0.12f;
			p[PARAM_GRAVITROPISM] = -0.05f;
			p[PARAM_BUD_LIGHT_MIN] = 0.0f; // Leafy to the ground: a closed mound, not a crown on stems.
			p[PARAM_SHED_RATIO] = 0.015f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.5f;
			p[PARAM_TIP_RADIUS] = 0.0014f;
			p[PARAM_PIPE_EXPONENT] = 2.8f;
			p[PARAM_STEMS] = 14; // Suckering: many stems from one rootstock.
			p[PARAM_STEM_SPREAD] = 82;
			p[PARAM_BASAL_SPROUTS] = 0.8f;
			p[PARAM_ROOT_FLARE] = 0.1f;
			p[PARAM_LEAF_SIZE] = 0.028f; // Leaves 1.5-4 cm, oval, spiny toothed.
			p[PARAM_LEAF_WIDTH] = 0.36f;
			p[PARAM_LEAVES_PER_INTERNODE] = 14;
			p[PARAM_LEAF_YEARS] = 3; // Leaves live 2-3 years.
			bark_color = Color(0.45f, 0.43f, 0.38f);
			leaf_color = Color(0.1f, 0.2f, 0.06f); // Glossy green both sides.
			break;
		case PRESET_LENTISK: // Pistacia lentiscus: dense, rounded, many-stemmed evergreen; paripinnate leaves (5v).
			p[PARAM_YEARS] = 16;
			p[PARAM_MATURITY] = 8;
			p[PARAM_INTERNODE_LENGTH] = 0.07f;
			p[PARAM_INTERNODE_LENGTH_AGE] = 0.96f;
			p[PARAM_MAX_SHOOT] = 4;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.49f;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_BRANCH_ANGLE_VARIANCE] = 12;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_GRAVITROPISM] = 0.05f;
			p[PARAM_BUD_LIGHT_MIN] = 0.0f; // Dense to the ground.
			p[PARAM_SHED_RATIO] = 0.01f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.4f;
			p[PARAM_TIP_RADIUS] = 0.0016f;
			p[PARAM_PIPE_EXPONENT] = 2.8f;
			p[PARAM_STEMS] = 12;
			p[PARAM_STEM_SPREAD] = 60;
			p[PARAM_BASAL_SPROUTS] = 0.7f;
			p[PARAM_ROOT_FLARE] = 0.12f;
			p[PARAM_LEAF_SIZE] = 0.02f; // Leaflets 1-5 cm (mostly 2-3).
			p[PARAM_LEAF_WIDTH] = 0.3f;
			p[PARAM_LEAVES_PER_INTERNODE] = 24; // A dense evergreen mass.
			p[PARAM_LEAF_YEARS] = 4;
			bark_color = Color(0.42f, 0.4f, 0.36f);
			leaf_color = Color(0.06f, 0.13f, 0.04f); // Dark, leathery, glossy.
			break;
		case PRESET_DWARF_FAN_PALM: // Chamaerops humilis: a clump of short fibrous trunks, fan leaves on spiny petioles (5w).
			// Not grown by shoots (_grow_palm): years set its age, stems its clump.
			p[PARAM_YEARS] = 30;
			p[PARAM_STEMS] = 4;
			p[PARAM_STEM_SPREAD] = 20;
			p[PARAM_BASAL_SPROUTS] = 0.0f;
			p[PARAM_TIP_RADIUS] = 0.006f;
			p[PARAM_LEAF_SIZE] = 0.3f; // Blade radius.
			p[PARAM_LEAF_WIDTH] = 0.1f;
			p[PARAM_LEAF_YEARS] = 3;
			bark_color = Color(0.24f, 0.18f, 0.12f); // Dark brown fibre over the old leaf bases.
			leaf_color = Color(0.12f, 0.2f, 0.1f); // Green, a little glaucous.
			break;
		case PRESET_STONE_PINE: // Pinus pinea: the umbrella pine (trees.md 5y).
			// Uninodal whorls; a strong leader while young, which gives up its dominance by middle
			// age, so the crown becomes many equal limbs spreading to one height (laterals keep
			// near the leader's vigor: apical control near 1); _shed lifts the crown onto a clear
			// bole of about 40% of the height.
			p[PARAM_YEARS] = 60;
			p[PARAM_INTERNODE_LENGTH] = 0.35f;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 5;
			p[PARAM_APICAL_DOMINANCE] = 0.78f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.32f;
			p[PARAM_MATURITY] = 35;
			p[PARAM_BRANCH_ANGLE] = 65;
			p[PARAM_DIRECTION_NOISE] = 13;
			p[PARAM_GRAVITROPISM] = 0.03f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.7f;
			p[PARAM_APICAL_CONTROL] = 0.95f; // Side shoots well under the forked stems (_fork_rule).
			p[PARAM_PHOTOTROPISM] = 0.12f;
			p[PARAM_SHED_RATIO] = 0.13f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.0f;
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.17f; // Needles in pairs, 10-18 cm: the long end, so the tufts close up into one cushion.
			p[PARAM_LEAVES_PER_INTERNODE] = 260;
			p[PARAM_LEAF_YEARS] = 4;
			p[PARAM_CROWN_CONE] = 0.0f; // No cone: a dome, then an umbrella.
			p[PARAM_SHADOW_STRENGTH] = 0.04f;
			p[PARAM_PIPE_EXPONENT] = 2.05f; // A stout bole under a heavy crown.
			p[PARAM_ROOT_FLARE] = 0.2f;
			p[PARAM_TIP_RADIUS] = 0.005f;
			bark_color = Color(0.44f, 0.37f, 0.32f); // Grey-brown plate tops; BARK_PINEA makes the fissures orange-red.
			leaf_color = Color(0.07f, 0.16f, 0.05f); // A dense, dark, glossy green mass from outside.
			break;
		case PRESET_CORK_OAK: // Quercus suber (trees.md 5aa): evergreen, a short bole under a few thick,
			// sinuous, spreading limbs (the leader split, _fork_rule), a broad, low, rounded crown.
			p[PARAM_YEARS] = 60;
			p[PARAM_INTERNODE_LENGTH] = 0.3f;
			p[PARAM_LEADER_GROWTH] = 0.5f;
			p[PARAM_APICAL_DOMINANCE] = 0.6f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_MATURITY] = 25;
			p[PARAM_BRANCH_ANGLE] = 62;
			p[PARAM_DIRECTION_NOISE] = 16;
			p[PARAM_GRAVITROPISM] = 0.0f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.6f;
			p[PARAM_APICAL_CONTROL] = 0.95f; // As the holm oak's dome: strong side shoots fill it.
			// A broad, low dome (docs, 18 Nov: a vase of long thin limbs read as eucalyptus).
			// A dense, dark crown: shade-tolerant buds and little shedding keep leaves deep inside
			// (with shed 0.1 the crown read thin over bare twiggy limbs, docs 18 Nov).
			p[PARAM_SHED_RATIO] = 0.02f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_ROOT_FLARE] = 0.7f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.0f; // Worked trees are pruned of dead wood.
			p[PARAM_LEAF_SIZE] = 0.07f; // 3-7 cm.
			p[PARAM_LEAF_WIDTH] = 0.45f;
			p[PARAM_LEAVES_PER_INTERNODE] = 34;
			p[PARAM_LEAF_YEARS] = 4; // Leaves kept up to two years, on short shoots of the last four.
			bark_color = Color(0.36f, 0.34f, 0.31f); // Virgin cork: grey-buff, darker in the fissures.
			leaf_color = Color(0.05f, 0.13f, 0.05f); // Dark, glossy above; grey felt below.
			break;
		case PRESET_HOLM_OAK: // Quercus ilex (trees.md 5ab): evergreen, a short bole and a dense, dark,
			// rounded dome, leafy deep inside (shade tolerant), small grey-felted leaves.
			p[PARAM_YEARS] = 60;
			p[PARAM_INTERNODE_LENGTH] = 0.3f;
			p[PARAM_LEADER_GROWTH] = 0.5f;
			p[PARAM_APICAL_DOMINANCE] = 0.6f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_MATURITY] = 25;
			p[PARAM_BRANCH_ANGLE] = 60;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_GRAVITROPISM] = 0.02f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.6f;
			p[PARAM_APICAL_CONTROL] = 0.95f;
			p[PARAM_SHED_RATIO] = 0.02f; // Shade-tolerant: leafy deep inside and low down.
			p[PARAM_DEAD_BRANCH_KEEP] = 0.02f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_ROOT_FLARE] = 0.6f;
			p[PARAM_LEAF_SIZE] = 0.06f; // 2-8 cm.
			p[PARAM_LEAF_WIDTH] = 0.4f;
			p[PARAM_LEAVES_PER_INTERNODE] = 40;
			p[PARAM_LEAF_YEARS] = 3; // Leaves live two to three years.
			bark_color = Color(0.3f, 0.29f, 0.27f); // Dark grey, cracked into small squares.
			leaf_color = Color(0.05f, 0.12f, 0.05f); // Very dark, glossy green above.
			break;
		case PRESET_WILD_OLIVE: // Olea europaea var. sylvestris (trees.md 5ac): a gnarled small tree of
			// two or three crooked stems and a dense, rounded, grey-green crown; opposite leaves,
			// narrow, silvery below.
			p[PARAM_YEARS] = 50;
			p[PARAM_INTERNODE_LENGTH] = 0.18f;
			p[PARAM_LEADER_GROWTH] = 0.35f;
			phyllotaxis = PHYLLOTAXIS_OPPOSITE;
			p[PARAM_APICAL_DOMINANCE] = 0.55f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_MATURITY] = 20;
			p[PARAM_BRANCH_ANGLE] = 50;
			p[PARAM_DIRECTION_NOISE] = 22; // Crooked, twisting stems.
			p[PARAM_GRAVITROPISM] = 0.03f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_APICAL_CONTROL] = 0.92f;
			p[PARAM_SHED_RATIO] = 0.03f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.1f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_STEMS] = 3;
			p[PARAM_STEM_SPREAD] = 20;
			p[PARAM_ROOT_FLARE] = 0.8f;
			p[PARAM_LEAF_SIZE] = 0.05f; // 3-6 cm.
			p[PARAM_LEAF_WIDTH] = 0.18f; // Narrow, lanceolate.
			p[PARAM_LEAVES_PER_INTERNODE] = 30;
			p[PARAM_LEAF_YEARS] = 3;
			bark_color = Color(0.42f, 0.41f, 0.37f); // Grey.
			leaf_color = Color(0.2f, 0.23f, 0.15f); // Pale silver grey-green above.
			break;
		case PRESET_MARITIME_PINE: // Pinus pinaster (trees.md 5ac): tall, a long clear bole and an
			// irregular crown in the upper part; stout long needles; very thick, deeply furrowed
			// bark. Built on the Scots pine growth rules (both are two-needle pines with whorls
			// that self-prune), with weaker apical dominance and control so the laterals fill a broad crown (as a Scots
			// pine it read as a spindly mast of thin whorls, docs 18 Nov), and stouter needles.
			p[PARAM_YEARS] = 45;
			p[PARAM_INTERNODE_LENGTH] = 0.4f;
			phyllotaxis = PHYLLOTAXIS_WHORLED;
			p[PARAM_WHORL_BUDS] = 5;
			p[PARAM_APICAL_DOMINANCE] = 0.68f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.52f;
			p[PARAM_MATURITY] = 30;
			p[PARAM_BRANCH_ANGLE] = 65;
			p[PARAM_DIRECTION_NOISE] = 9;
			p[PARAM_GRAVITROPISM] = 0.0f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.9f;
			p[PARAM_APICAL_CONTROL] = 0.9f; // Strong laterals: a full, dense crown.
			p[PARAM_PHOTOTROPISM] = 0.08f;
			p[PARAM_GRAVITY_BENDING] = 0.35f; // Heavy old limbs bow out and down, tips turned up: the crown spreads as a mass, not a fan of rising limbs.
			p[PARAM_SHED_RATIO] = 0.02f; // Keeps shoots inside the crown: a dense mass, not tufts at the limb tips (docs, 18 Nov).
			p[PARAM_DEAD_BRANCH_KEEP] = 0.0f; // A long dead limb read as an artefact (docs, 18 Nov).
			p[PARAM_NEEDLES] = 1;
			p[PARAM_LEAF_SIZE] = 0.18f; // Pairs, 10-25 cm, stout.
			p[PARAM_LEAVES_PER_INTERNODE] = 300; // Dense: the needles hide most of the branches.
			p[PARAM_LEAF_YEARS] = 6; // Needles stay 4-6 years on old crowns.
			p[PARAM_CROWN_CONE] = 0.0f; // No excurrent cone: it held the top to a sparse spire over a wide lowest whorl (docs, 18 Nov).
			p[PARAM_SHADOW_STRENGTH] = 0.03f;
			p[PARAM_PIPE_EXPONENT] = 1.85f; // A stout bole under the heavy crown.
			bark_color = Color(0.4f, 0.26f, 0.2f); // Purplish red-brown plates.
			leaf_color = Color(0.11f, 0.17f, 0.08f); // Grey-green.
			break;
		case PRESET_CAROB: // Ceratonia siliqua (trees.md 5ac): evergreen, a short trunk under a broad,
			// dense, rounded crown; leathery, glossy, paripinnate leaves of rounded leaflets.
			p[PARAM_YEARS] = 50;
			p[PARAM_INTERNODE_LENGTH] = 0.25f;
			p[PARAM_LEADER_GROWTH] = 0.45f;
			p[PARAM_APICAL_DOMINANCE] = 0.56f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_MATURITY] = 20;
			p[PARAM_BRANCH_ANGLE] = 60;
			p[PARAM_DIRECTION_NOISE] = 15;
			p[PARAM_GRAVITROPISM] = 0.01f;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.45f;
			p[PARAM_APICAL_CONTROL] = 0.95f;
			p[PARAM_SHED_RATIO] = 0.025f;
			p[PARAM_DEAD_BRANCH_KEEP] = 0.05f;
			p[PARAM_BUD_LIGHT_MIN] = 0.02f;
			p[PARAM_ROOT_FLARE] = 0.7f;
			p[PARAM_LEAF_SIZE] = 0.16f; // Leaves 10-20 cm, leaflets 3-7 cm.
			p[PARAM_LEAVES_PER_INTERNODE] = 6;
			p[PARAM_LEAF_YEARS] = 2;
			bark_color = Color(0.36f, 0.32f, 0.28f); // Grey-brown, rough.
			leaf_color = Color(0.05f, 0.13f, 0.04f); // Dark, glossy.
			break;
		case PRESET_BLACKTHORN: // Prunus spinosa (trees.md 5ad): a suckering, thorny thicket shrub,
			// stiff side shoots at wide angles, dark bark; flowers on bare wood, sloes.
			p[PARAM_YEARS] = 18;
			p[PARAM_INTERNODE_LENGTH] = 0.15f;
			p[PARAM_APICAL_DOMINANCE] = 0.5f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.48f;
			p[PARAM_MATURITY] = 8;
			p[PARAM_BRANCH_ANGLE] = 70; // Stiff, spreading spur shoots.
			p[PARAM_DIRECTION_NOISE] = 18;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.4f;
			p[PARAM_SHED_RATIO] = 0.03f;
			p[PARAM_STEMS] = 4;
			p[PARAM_STEM_SPREAD] = 20;
			p[PARAM_BASAL_SPROUTS] = 0.3f; // Suckers.
			p[PARAM_LEAF_SIZE] = 0.035f;
			p[PARAM_LEAVES_PER_INTERNODE] = 18;
			bark_color = Color(0.2f, 0.18f, 0.17f); // Almost black.
			leaf_color = Color(0.12f, 0.2f, 0.08f); // Dull green.
			break;
		case PRESET_FIELD_MAPLE: // Acer campestre (trees.md 5ad): a small round-headed tree, often a
			// cut hedge shrub; small five-lobed leaves, corky twigs, butter-yellow in autumn.
			p[PARAM_YEARS] = 40;
			p[PARAM_INTERNODE_LENGTH] = 0.3f;
			p[PARAM_APICAL_DOMINANCE] = 0.58f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.5f;
			p[PARAM_MATURITY] = 20;
			p[PARAM_BRANCH_ANGLE] = 50;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.55f;
			p[PARAM_SHED_RATIO] = 0.03f;
			p[PARAM_LEAF_SIZE] = 0.06f;
			p[PARAM_LEAVES_PER_INTERNODE] = 20;
			bark_color = Color(0.42f, 0.37f, 0.3f); // Light grey-brown, finely fissured.
			leaf_color = Color(0.12f, 0.25f, 0.07f);
			break;
		case PRESET_DOG_ROSE: // Rosa canina (trees.md 5ad): arching prickly canes from the base,
			// pinnate leaves; pale pink flowers in June, scarlet hips.
			p[PARAM_YEARS] = 8;
			p[PARAM_INTERNODE_LENGTH] = 0.25f;
			p[PARAM_APICAL_DOMINANCE] = 0.45f;
			p[PARAM_APICAL_DOMINANCE_OLD] = 0.45f;
			p[PARAM_MATURITY] = 4;
			p[PARAM_BRANCH_ANGLE] = 45;
			p[PARAM_DIRECTION_NOISE] = 14;
			p[PARAM_TRUNK_GRAVITROPISM] = 0.3f;
			p[PARAM_GRAVITY_BENDING] = 0.6f; // Canes arch over under their own weight.
			p[PARAM_SHED_RATIO] = 0.05f;
			p[PARAM_STEMS] = 6;
			p[PARAM_STEM_SPREAD] = 30;
			p[PARAM_BASAL_SPROUTS] = 0.5f; // New canes from the base every year.
			p[PARAM_LEAF_SIZE] = 0.03f; // Leaflets 1.5-4 cm.
			p[PARAM_LEAVES_PER_INTERNODE] = 10;
			bark_color = Color(0.3f, 0.32f, 0.2f); // Green to brown canes.
			leaf_color = Color(0.12f, 0.22f, 0.08f);
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
		if (p_main || p_order == par.order) {
			nodes[idx].stem_level = par.stem_level;
			nodes[idx].stem_born = par.stem_born;
		}
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

namespace {
float moss_noise(const Vector3 &p_p, uint32_t p_salt);
} // namespace

// Decurrent crowns (trees.md 5z): Pinus pinea's leader gives up at a few metres and a handful
// of equal limbs take the crown, each dividing again higher up (Mutke et al. 2005; Gymnosperm
// Database).
TreeGenerator::ForkRule TreeGenerator::_fork_rule() const {
	ForkRule r;
	if (preset == PRESET_STONE_PINE && forest_shade <= 0.0f) {
		r.fork_height = 5.0f;
		r.stems = 3;
		// Three to five thick limbs that stay clear and divide again only near the crown (docs, 18
		// Nov: a third fork level gave a fan of 20-25 thin limbs).
		r.levels = 2;
		r.fork_every = 12;
		r.angle = 32.0f;
		r.gravitropism = 0.12f;
		r.lift = 0.55f;
		r.side = 0.5f;
	} else if (preset == PRESET_CORK_OAK && forest_shade <= 0.0f) {
		// A bole of 2-4 m, then three or four thick, sinuous limbs spreading wide (Natividade
		// 1950). One split only: a second gave sixteen limbs, a broom of thin pale stems (docs,
		// 18 Nov); their side branches stay under them.
		r.fork_height = 3.0f;
		r.stems = 4;
		r.fork_every = 10;
		r.levels = 1;
		r.angle = 62.0f; // Limbs spread out more than up.
		r.gravitropism = 0.02f;
		r.lift = 0.22f; // Pruned up to the cork harvest height in the montado.
		r.side = 0.8f; // Their branches fill a dense dome; the single split keeps the limbs few.
	} else if (preset == PRESET_MARITIME_PINE && forest_shade <= 0.0f) {
		// A long clear bole under the crown (Alia et al. 1996; the Landes pine): open-grown, the
		// crown starts at about 40% of the height.
		r.lift = 0.42f;
	} else if (preset == PRESET_CAROB && forest_shade <= 0.0f) {
		// A short trunk into a few spreading limbs under a broad dome (Batlle and Tous 1997).
		r.fork_height = 2.0f;
		r.stems = 3;
		r.fork_every = 10;
		r.levels = 2;
		r.angle = 50.0f;
		r.gravitropism = 0.03f;
		r.lift = 0.08f;
		r.side = 1.0f;
	} else if (preset == PRESET_HOLM_OAK && forest_shade <= 0.0f) {
		// A short bole into a few limbs under a full dome (Rameau et al., Flore forestiere
		// francaise 3, 2008); side shoots stay strong: the dome is leafy throughout.
		r.fork_height = 2.0f;
		r.stems = 3;
		r.fork_every = 10;
		r.levels = 2;
		r.angle = 45.0f;
		r.gravitropism = 0.05f;
		r.lift = 0.0f;
		r.side = 1.0f;
	}
	return r;
}

void TreeGenerator::_grow_year(int p_year) {
	_build_shadow_grid(p_year);
	{
		// The leader split (trees.md 5z): the terminal bud of a stem due to fork dies and a few
		// buds of the stem's own rank open around it.
		const ForkRule fr = _fork_rule();
		if (fr.fork_height > 0.0f && cut_height <= 0.0f && krummholz <= 0.0f) {
			LocalVector<Bud> forks;
			for (Bud &b : buds) {
				if (!b.alive || !b.terminal || b.node <= 0) {
					continue;
				}
				const Node &n = nodes[b.node];
				if (n.order != 0 || n.removed || n.dead || n.stem_level >= fr.levels || n.layered) {
					continue;
				}
				const bool due = n.stem_level == 0 ? n.end().y - nodes[0].start.y >= fr.fork_height : p_year - n.stem_born >= fr.fork_every;
				if (!due) {
					continue;
				}
				b.alive = false;
				const Vector3 axis = b.dir;
				const float roll0 = _rand(0.0f, Math::TAU);
				Vector3 ref = Math::abs(axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
				const Vector3 side0 = axis.cross(ref).normalized();
				for (int k = 0; k < fr.stems; k++) {
					const Vector3 side = side0.rotated(axis, roll0 + Math::TAU * (k + _rand(-0.15f, 0.15f)) / fr.stems);
					const float ang = Math::deg_to_rad(fr.angle * _rand(0.8f, 1.2f));
					Bud f;
					f.node = b.node;
					f.dir = axis.rotated(side.cross(axis).normalized(), ang).normalized();
					f.fork = true;
					forks.push_back(f);
				}
			}
			for (const Bud &f : forks) {
				buds.push_back(f);
			}
		}
	}
	if (envelope_sprouts > 0 && envelope.size.x > 0.0f && p_year > 2) {
		// A flailed hedge (trees.md 5ad): the cut wakes buds all through the outer half metre,
		// not only at the cut ends, and the side cuts push the low wood out sideways; the hedge
		// fills to an opaque twiggy mass out to its faces, widest at the foot (docs, 18 Nov: a
		// thin leafy shell over bare stems, an upright box where an A was asked for).
		const float top = envelope.position.y + envelope.size.y;
		const float zc = envelope.position.z + envelope.size.z * 0.5f;
		const int count0 = int(nodes.size());
		LocalVector<Bud> wake;
		for (int i = 1; i < count0; i++) {
			const Node &n = nodes[i];
			if (n.removed || n.dead || n.radius > 0.03f) {
				continue;
			}
			const Vector3 q = n.end();
			if (!_in_envelope(q)) {
				continue;
			}
			const float t = CLAMP(q.y / MAX(top, 0.01f), 0.0f, 1.0f);
			const float half = envelope.size.z * 0.5f * (1.0f - envelope_batter * t);
			const float side_gap = half - Math::abs(q.z - zc);
			const float top_gap = top - q.y;
			const float gap = MIN(side_gap, top_gap);
			if (gap > 0.5f || _rand(0.0f, 1.0f) > 0.12f) {
				continue;
			}
			Bud b;
			b.node = i;
			// Out toward the nearer face, and level more than up near the foot.
			Vector3 out = side_gap < top_gap ? Vector3(0, 0.15f, q.z >= zc ? 1.0f : -1.0f) : Vector3(0, 1, 0);
			b.dir = (out.normalized() + _random_unit() * 0.6f).normalized();
			b.terminal = true;
			wake.push_back(b);
		}
		for (const Bud &b : wake) {
			buds.push_back(b);
		}
	}
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
		// A krummholz mat spreads along the ground and roots where its limbs touch (layering;
		// Marr 1977): only buds in the soil die.
		if (b.node != 0 && n.order > 0 && p.y < (krummholz > 0.0f ? Math::lerp(0.3f, 0.02f, krummholz) : 0.3f)) {
			b.alive = false;
			continue;
		}
		// A layered stem's first bud is fed by its new roots and the limb, not by its own light.
		if (b.light < params[PARAM_BUD_LIGHT_MIN] && !b.layer) {
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
	// A krummholz mat has no leader to set a cone: its limbs spread at the snow's surface.
	const float cone = krummholz > 0.0f ? 0.0f : params[PARAM_CROWN_CONE];
	float top = 0.0f;
	const Vector3 root = nodes[0].start;
	if (cone > 0.0f) {
		for (const Node &n : nodes) {
			if (n.order == 0 && !n.removed) {
				top = MAX(top, n.end().y);
			}
		}
	}
	const float fork_side = _fork_rule().side;
	for (int bi = 0; bi < bud_count; bi++) {
		if (!buds[bi].alive) {
			continue;
		}
		// Apical control (Stava 2014): shoots on branches grow less the further their order is from
		// the leader, which keeps conifers excurrent.
		{
			const Node &on = nodes[buds[bi].node];
			int ord = (buds[bi].terminal || buds[bi].sprout || buds[bi].fork || buds[bi].node == 0) ? on.order : on.order + 1;
			if (buds[bi].layer || (buds[bi].terminal && on.layered)) {
				ord = 0; // A layered stem grows as a stem of its own (section 5s).
			}
			buds[bi].vigor *= Math::pow(params[PARAM_APICAL_CONTROL], float(ord));
			if (!buds[bi].terminal && !buds[bi].fork && on.order == 0 && on.stem_level > 0) {
				// Side shoots off a forked stem stay thin under it (trees.md 5z, docs 18 Nov).
				buds[bi].vigor *= fork_side;
			}
			// A cut stem's root system feeds a few poles: they shoot up 1-3 m a year (coppice
			// regrowth; Rackham 2006).
			if ((buds[bi].sprout || (buds[bi].terminal && on.pole)) && p_year - last_cut_year <= 4) {
				buds[bi].vigor *= 1.6f;
			}
			if (wind_exposure > 0.0f && buds[bi].terminal && on.order == 0) {
				// Exposed trees stay short: the leader's shoots are shortened by the wind's drying
				// and loading (Holtmeier 2009), most of all at the tree line.
				const float above = Math::smoothstep(0.5f, 3.0f, on.end().y - root.y);
				buds[bi].vigor *= 1.0f - (0.45f + 0.4f * krummholz) * wind_exposure * above;
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
		const int order = (bud.terminal || bud.sprout || bud.fork || from_root) ? from.order : from.order + 1;
		// Lateral extension (Wilson 2000; Kantola and Makela 2004): in excurrent conifers the
		// branches lengthen far more slowly than the leader, which sets the narrow cone. Scaling the
		// vigor for it starved the crown of buds and leaves (vigor comes from collected light) and
		// the tree collapsed to a pole; scaling the shoot length alone keeps every bud and needle
		// and only draws the branches in.
		const bool layered = bud.layer || (bud.terminal && from.layered);
		float len = base_len * CLAMP(bud.vigor / float(count), 0.25f, 1.5f) * Math::pow(params[PARAM_LATERAL_EXTENSION], float(layered ? 0 : MIN(order, 2)));
		if (wind_exposure > 0.0f && order == 0) {
			// Exposed stems stay short (Holtmeier 2009): the wind's drying and load shorten the
			// leader's shoots, a flagged spruce at the tree line 3-8 m after a century.
			len *= 1.0f - 0.75f * wind_exposure * Math::smoothstep(1.0f, 6.0f, from.end().y - root.y);
		}
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
		if (krummholz > 0.0f) {
			// Dwarfed: short shoots on every axis under the snow (Holtmeier 2009), so the mat is
			// dense with needles.
			len *= Math::lerp(1.0f, 0.55f, krummholz);
		}
		const bool forked = bud.fork || (order == 0 && from.stem_level > 0);
		const float grav = forked ? _fork_rule().gravitropism : ((order == 0 || layered) ? params[PARAM_TRUNK_GRAVITROPISM] : params[PARAM_GRAVITROPISM]);
		Vector3 pos = from_root ? from.start : from.end();
		Vector3 dir = bud.dir;
		int parent = bud.node;
		float roll = _rand(0.0f, Math::TAU);
		float krumm_cap = 0.0f;
		for (int k = 0; k < count; k++) {
			if (k > 0 || !bud.terminal) {
				Vector3 d = dir + _best_lit_direction(pos, dir) * params[PARAM_PHOTOTROPISM] + Vector3(0, grav, 0) * 0.5f;
				d += _random_unit() * Math::sin(noise);
				if (krummholz > 0.0f && order > 0 && !layered) {
					// Mat branches press down under the snow and spread (krummholz, trees.md 5q).
					d.y -= 0.08f * krummholz;
				}
				dir = d.length_squared() > 1e-8f ? d.normalized() : dir;
			} else {
				Vector3 d = dir + _best_lit_direction(pos, dir) * params[PARAM_PHOTOTROPISM] * 0.5f + Vector3(0, grav, 0) * 0.3f + _random_unit() * Math::sin(noise) * 0.5f;
				dir = d.length_squared() > 1e-8f ? d.normalized() : dir;
			}
			const bool main = k > 0 || (bud.terminal && !from_root);
			// Clipped forms (hedges, topiary; Palubicki 2009 section 6): a shoot that would leave the
			// envelope is cut back, as a hedge trimmer does each year.
			if (envelope.size.x > 0.0f && !_in_envelope(pos + dir * len)) {
				if (envelope_sprouts > 0 && bud.terminal && bud.idle < 4) {
					// A flailed hedge (trees.md 5ad): the cut stub breaks into short shoots from the
					// buds behind the cut, turned back along the hedge's face, so each year's cut
					// thickens the twiggy shell instead of stopping the shoot.
					for (int q = 0; q < envelope_sprouts; q++) {
						Bud t;
						t.node = parent;
						Vector3 sd = _random_unit() - dir * 0.6f;
						t.dir = sd.length_squared() > 1e-6f ? sd.normalized() : -dir;
						t.terminal = true;
						t.idle = bud.idle + 1;
						buds.push_back(t);
					}
				}
				break;
			}
			// Krummholz (trees.md 5q, 5s): what rises above the snowpack is killed back each winter,
			// so the tree grows as a cushion under the snow's surface.
			float dome_slope = 0.0f;
			Vector2 out_h;
			if (krummholz > 0.0f) {
				// The mat is a low dome a few metres across (Marr 1977; Holtmeier 2009), its top
				// the snowpack's lumpy surface, highest over its oldest stems and falling to the
				// rim; it reaches further downwind, where the snow lies deeper and limbs that root
				// in the lee carry it on (tree islands migrate to the lee, Marr 1977; Benedict
				// 1984). Shoots past its rim die back.
				const Vector3 q = pos + dir * len;
				out_h = Vector2(q.x - root.x, q.z - root.z);
				krumm_cap = _krumm_cap(q, &dome_slope);
				if (krumm_cap < 0.0f) {
					break;
				}
			}
			// The limbs run a hand or two under the surface, the shoots on them filling up to it,
			// so the mat's outside is needles over a frame of wood, not limbs lying on the dome.
			const float limb_depth = order <= 2 ? 0.3f * krumm_cap : 0.0f;
			if (krummholz > 0.0f && (pos + dir * len).y - root.y > krumm_cap - limb_depth) {
				if (order == 0 || layered || order >= 3) {
					// A stem's leader and the outer shoots are killed back at the snow's surface every
					// winter; the shoot's tip lives on under it as a spur, in needles, and pushes again
					// next year, so the mat's surface is a dense skin of short shoots.
					if (parent != bud.node || bud.terminal) {
						Bud t;
						t.node = parent;
						t.dir = dir;
						t.terminal = true;
						t.idle = bud.idle + 1;
						if (t.idle <= 12) {
							buds.push_back(t);
						}
					}
					break;
				}
				// Under the snow's surface the shoot runs on outward along the dome: limbs at each
				// height lie in their own skirt under it, not on one level table.
				Vector3 hd(out_h.x, 0.0f, out_h.y);
				if (hd.length_squared() < 0.01f) {
					hd = Vector3(dir.x, 0.0f, dir.z);
				}
				if (hd.length_squared() < 0.01f) {
					hd = Vector3(Math::cos(roll), 0.0f, Math::sin(roll));
				}
				hd.normalize();
				dir = (hd + Vector3(0.0f, MIN(dome_slope, 0.0f) - 0.06f, 0.0f)).normalized();
			}
			const int idx = _add_node(parent, pos, dir, len, order, p_year, main);
			if (k == 0 && bud.fork) {
				nodes[idx].stem_level = from.stem_level + 1;
				nodes[idx].stem_born = p_year;
			}
			nodes[idx].pole = pole;
			nodes[idx].layered = layered;
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
	// At the tree line conifers keep their needles longer, 8-12 years (Reich et al. 1996).
	const int leaf_years = MAX(1, int(params[PARAM_LEAF_YEARS]) + int(4.0f * krummholz));
	_compute_radii();
	// Leaves on the last leaf_years of shoots, and wherever a terminal bud is alive: a bud with too
	// little vigor to extend still opens a short shoot (spur) with its leaves (Palubicki: leaves at
	// the buds).
	for (Node &n : nodes) {
		n.leafy = !n.dead && !n.removed && n.length > 0.0f && n.born > p_year - leaf_years;
	}
	if (krummholz > 0.0f) {
		// A mat's skin (trees.md 5s): the shoots in its top few tens of centimetres are cut back
		// and push again every year, so all of it is in young needles over the old wood inside.
		for (Node &n : nodes) {
			if (n.leafy || n.dead || n.removed || n.length <= 0.0f || n.order < 2) {
				continue;
			}
			const float cap = _krumm_cap(n.end(), nullptr);
			n.leafy = cap > 0.0f && n.end().y - root.y > cap - 0.35f * cap - 0.05f;
		}
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

// The krummholz mat's top over a point (trees.md 5q, 5s): a low dome a few metres across (Marr
// 1977; Holtmeier 2009), the snowpack's lumpy surface, highest over its oldest stems and falling to
// the rim; it reaches further downwind, where the snow lies deeper and limbs that root in the lee
// carry it on (tree islands migrate to the lee, Marr 1977; Benedict 1984). Height over the root,
// -1 past the rim; r_slope its fall per metre outward.
float TreeGenerator::_krumm_cap(const Vector3 &p_q, float *r_slope) const {
	const Vector3 root = nodes[0].start;
	// A heath shrub's wind-pruned cushion (trees.md 5u) is the same dome at a shrub's scale: a
	// metre or two across and under half a metre high.
	const bool heath = scrub_shrub(preset);
	const float rim = Math::lerp(9.0f, 2.5f, krummholz) * (heath ? 0.3f : 1.0f);
	Vector2 off(p_q.x - root.x, p_q.z - root.z);
	off.x /= off.x > 0.0f ? 1.0f + 0.6f * wind_exposure : 1.0f - 0.35f * wind_exposure;
	const float rq = off.length() / rim;
	if (rq > 1.0f) {
		return -1.0f;
	}
	const float cap_h = Math::lerp(3.0f, 0.75f, krummholz) * (heath ? 0.25f : 1.0f) * (0.75f + 0.5f * moss_noise(p_q * 0.8f, 0x6b72756du));
	const float e = MAX(1.0f - rq * rq, 0.02f);
	if (r_slope) {
		*r_slope = -cap_h * 0.88f * 0.6f * Math::pow(e, -0.4f) * 2.0f * rq / rim;
	}
	return cap_h * (0.12f + 0.88f * Math::pow(e, 0.6f));
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
	// A stone pine's umbrella (trees.md 5y): the limbs off the stem low down die in the shade of
	// the dome the upper limbs spread over them, and the crown lifts to sit on a clear bole of
	// about half the tree's height by middle age, open-grown too.
	float lift = 0.0f;
	const float lift_share = _fork_rule().lift;
	if (lift_share > 0.0f) {
		float top = nodes[0].start.y;
		for (const Node &n : nodes) {
			if (!n.removed) {
				top = MAX(top, n.end().y);
			}
		}
		lift = (top - nodes[0].start.y) * lift_share * Math::smoothstep(8.0f, 35.0f, float(p_year));
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
		const bool under_scrub = (scrub > 0.0f && nodes[n.parent].order == 0 && n.start.y - nodes[0].start.y < scrub) ||
				(lift > 0.0f && nodes[n.parent].order == 0 && n.stem_level <= nodes[n.parent].stem_level && n.start.y - nodes[0].start.y < lift);
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
// The dwarf fan palm (trees.md 5w; Flora iberica vol. 18; Tomlinson 1990): no shoot growth, the
// clump laid out directly. Each stem is a short chain of trunk nodes (0-2.5 m, often none where
// grazed or wind-cut), its crown 10-20 leaves on golden-angle petioles, the young ones upright,
// the old ones spread and the dead ones hanging; each petiole three nodes, so bark, wind bones
// and colliders treat it as a limb. Age is PARAM_YEARS over the preset's 30; the cushion form
// (krummholz) keeps the crowns at the ground on short petioles.
void TreeGenerator::_grow_palm() {
	palm_fronds.clear();
	palm_trunks.clear();
	palm_crowns.clear();
	const float age = CLAMP(params[PARAM_YEARS] / 30.0f, 0.1f, 2.5f);
	const float grown = MIN(age, 1.0f);
	const bool cushion = krummholz > 0.0f;
	const int stems = MAX(1, int(params[PARAM_STEMS]));
	const float clump = 0.16f * Math::sqrt(float(stems)) * (cushion ? 1.3f : 1.0f);
	const Vector3 up(0, 1, 0);
	const Vector3 root = nodes[0].start;
	for (int s = 0; s < stems; s++) {
		const float a = Math::TAU * s / stems + _rand(-0.4f, 0.4f);
		const Vector3 out(Math::cos(a), 0.0f, Math::sin(a));
		const float r = stems == 1 ? 0.0f : clump * Math::sqrt(_rand(0.15f, 1.0f));
		const Vector3 base = root + out * r;
		// Trunk height: none on young plants and cushions; mature clumps mix stemless crowns
		// with trunks to about a metre, old ones to 2.5 m.
		float th = 0.0f;
		if (!cushion && age > 0.45f) {
			th = MAX(0.0f, _rand(-0.35f, 1.0f)) * Math::lerp(0.4f, 1.0f, grown) * (age > 1.0f ? Math::lerp(1.0f, 2.5f, (age - 1.0f) / 0.7f) : 1.0f);
			th = MIN(th, 2.6f);
		}
		const Vector3 lean = (up + out * (stems > 1 ? _rand(0.15f, 0.45f) : _rand(0.0f, 0.1f))).normalized();
		const float trunk_r = Math::lerp(0.05f, 0.11f, grown) * _rand(0.9f, 1.1f);
		const int segs = th < 0.06f ? 1 : MAX(1, int(Math::ceil(th / 0.15f)));
		const float seg_len = th < 0.06f ? 0.06f : th / segs;
		int parent = 0;
		Vector3 p = base;
		for (int k = 0; k < segs; k++) {
			// Leaning out at the foot, turning up toward the crown.
			const Vector3 d = lean.lerp(up, 0.6f * float(k) / MAX(segs, 1)).normalized();
			const int idx = _add_node(parent, p, d, seg_len, 0, 1, parent != 0);
			nodes[idx].leafy = false;
			nodes[idx].radius = trunk_r * (1.0f - 0.12f * float(k) / MAX(segs, 1));
			palm_trunks.push_back(idx);
			p = nodes[idx].end();
			parent = idx;
		}
		const int top = parent;
		palm_crowns.push_back(top);
		const Vector3 crown_axis = nodes[top].dir;
		// The leaves: fewer and smaller on a young plant; cushions keep many short ones.
		const int alive = cushion ? int(_rand(9.0f, 13.0f)) : int(Math::lerp(6.0f, 15.0f, grown) + _rand(0.0f, 4.0f));
		int dead = cushion ? 0 : (age > 1.0f ? int(_rand(5.0f, 10.0f)) : (age > 0.45f ? int(_rand(0.0f, 3.0f)) : 0));
		const float blade = (cushion ? _rand(0.17f, 0.22f) : Math::lerp(0.16f, params[PARAM_LEAF_SIZE], grown) * _rand(0.9f, 1.1f));
		const float pet = cushion ? _rand(0.1f, 0.16f) : Math::lerp(0.18f, 0.55f, grown) * _rand(0.85f, 1.15f);
		// Dead leaves hang against the trunk below the crown; a crown too low for them to hang
		// clear of the ground has shed them (they lay on the ground as loose fans).
		if (nodes[top].end().y - root.y < pet * 0.3f + blade * 0.85f + 0.05f) {
			dead = 0;
		}
		const Vector3 ref = Math::abs(crown_axis.y) < 0.95f ? up : Vector3(1, 0, 0);
		const Vector3 cx = crown_axis.cross(ref).normalized();
		const Vector3 cz = cx.cross(crown_axis).normalized();
		float roll = _rand(0.0f, Math::TAU);
		for (int f = 0; f < alive + dead; f++) {
			const bool is_dead = f >= alive;
			// Youngest first: upright in the centre; older ones spread out to near level; the
			// dead hang along the trunk.
			const float t = is_dead ? 1.0f : float(f) / MAX(alive - 1, 1);
			roll += 2.39996f;
			const float elev = is_dead ? _rand(-1.0f, -0.65f) : Math::lerp(1.35f, cushion ? 0.15f : 0.3f, Math::pow(t, 0.8f)) + _rand(-0.12f, 0.12f);
			const Vector3 h = cx * Math::cos(roll) + cz * Math::sin(roll);
			Vector3 d = (h * Math::cos(elev) + crown_axis * Math::sin(elev)).normalized();
			const float plen = pet * (is_dead ? 0.3f : Math::lerp(0.75f, 1.05f, t)) / 3.0f;
			int pp = top;
			Vector3 q = nodes[top].end() - crown_axis * (0.02f + 0.05f * t) + h * (trunk_r * 0.6f);
			for (int k = 0; k < 3; k++) {
				// The petiole bows a little under the blade.
				if (!is_dead) {
					d = (d - up * 0.06f * float(k)).normalized();
				}
				const int idx = _add_node(pp, q, d, plen, 1, 1, k > 0);
				nodes[idx].leafy = false;
				nodes[idx].radius = Math::lerp(0.006f, 0.004f, float(k) / 2.0f) * Math::lerp(0.6f, 1.0f, grown); // 0.5-1.2 cm wide.
				nodes[idx].dead = is_dead;
				q = nodes[idx].end();
				pp = idx;
			}
			PalmFrond fr;
			fr.node = pp;
			fr.blade = blade * (is_dead ? 0.85f : Math::lerp(0.75f, 1.0f, MIN(1.0f, t * 3.0f)));
			fr.segments = int(_rand(16.0f, 23.0f)); // Close-set near the hastula.
			fr.dead = is_dead;
			palm_fronds.push_back(fr);
		}
	}
}

// The palm's own geometry on the generic stems and petioles (trees.md 5w): each blade a fan of
// three quarters of a circle split into induplicate segments (each a V-folded blade, the fold
// up, fused at the base and free for the outer two thirds); straight spines up to 2 cm along both
// edges of the petiole's lower two thirds; on the trunks the stubs of old leaf bases and their
// fibre.
void TreeGenerator::_palm_geometry(Builder &r_bark, Builder &r_leaves) {
	RandomPCG pr;
	pr.seed(uint64_t(seed) * 0x9E3779B97F4A7C15ull + 0x70616c6du);
	const Vector3 up(0, 1, 0);
	const Color live = leaf_color;
	const Color dead_c = Color(0.23f, 0.16f, 0.09f); // Straw-brown (on the leaf scale: live is 0.12, 0.2, 0.1).
	const Color spine_c = Color(0.55f, 0.47f, 0.3f);
	for (const PalmFrond &f : palm_fronds) {
		const Node &n = nodes[f.node];
		const Vector3 hast = n.end();
		const Vector3 fwd = n.dir;
		Vector3 nb = up - fwd * fwd.dot(up);
		nb = nb.length_squared() > 1e-4f ? nb.normalized() : fwd.cross(Vector3(1, 0, 0)).normalized();
		const float R = f.blade;
		const int K = f.segments;
		const float span = Math::deg_to_rad(f.dead ? 220.0f : 270.0f);
		r_leaves.bone = node_bone.size() > uint32_t(f.node) ? node_bone[f.node] : 0;
		// Plicate fan (Dransfield et al., Genera Palmarum, 2008; Flora iberica: Chamaerops's blade
		// divided nearly to the base): stiff, narrow, induplicate segments, each V-folded with the
		// margins up and a midrib crease below, free almost from the hastula, where a small pleated
		// disc joins them. Each segment is a leaf part, sized so the LODs keep the fan whole. A dead
		// leaf folds shut and hangs, straw-brown, along its petiole.
		const float rp = R * 0.08f; // The hastula: the segments are free beyond it.
		const float seg_a = span / K;
		const float pleat = 0.7f; // Fold depth over the half-width: about 35 degrees each side.
		auto ray = [&](float p_ang) {
			const Vector3 r = fwd.rotated(nb, p_ang);
			// A dead leaf's fan half closes and hangs.
			return f.dead ? (r + fwd * 0.4f - up * 0.35f).normalized() : r;
		};
		auto tri = [&](int a, int b, int c, const Vector3 &p_n) {
			// Godot's front faces wind clockwise: (c - a) x (b - a) along the normal.
			const Vector3 fn = (r_leaves.verts[c] - r_leaves.verts[a]).cross(r_leaves.verts[b] - r_leaves.verts[a]);
			if (fn.dot(p_n) < 0.0f) {
				SWAP(b, c);
			}
			r_leaves.indices.push_back(a);
			r_leaves.indices.push_back(b);
			r_leaves.indices.push_back(c);
		};
		auto vert = [&](const Vector3 &p_p, const Vector3 &p_n, const Vector2 &p_uv, const Color &p_c) {
			r_leaves.verts.push_back(p_p);
			r_leaves.normals.push_back(p_n);
			r_leaves.uvs.push_back(p_uv);
			r_leaves.colors.push_back(p_c);
			return int(r_leaves.verts.size()) - 1;
		};
		// Per-segment shade and length, and the tears of a dead leaf.
		LocalVector<Color> sc;
		LocalVector<float> sl;
		LocalVector<bool> torn;
		for (int k = 0; k < K; k++) {
			const float u = (k + 0.5f) / K * 2.0f - 1.0f;
			const float jit = 0.88f + 0.24f * pr.randf();
			sc.push_back(f.dead ? Color(dead_c.r * jit, dead_c.g * jit, dead_c.b * jit) : Color(live.r * jit, live.g * jit, live.b * (0.95f + 0.2f * pr.randf())));
			sl.push_back(R * Math::lerp(1.0f, 0.62f, Math::pow(Math::abs(u), 1.5f)) * (0.92f + 0.16f * pr.randf()));
			torn.push_back(f.dead && pr.randf() < 0.25f);
		}
		// The hastula: a small pleated disc, vertices on the seams (up) and midribs (down).
		const int RINGS = 2;
		{
			Vector4i part;
			r_leaves.begin(part);
			LocalVector<int> prev;
			for (int j = 0; j <= RINGS; j++) {
				const float r = Math::lerp(R * 0.02f, rp, float(j) / RINGS);
				const float hw = r * seg_a * 0.5f;
				LocalVector<int> ring;
				for (int q = 0; q <= 2 * K; q++) {
					const float ang = -span * 0.5f + seg_a * 0.5f * q;
					const bool seam = (q & 1) == 0;
					const int k = MIN(q / 2, K - 1);
					const Vector3 p = hast + ray(ang) * r + nb * ((seam ? 1.0f : -1.0f) * pleat * hw);
					ring.push_back(vert(p, nb, Vector2(seam ? 0.0f : 0.5f, 0.3f * r / rp), sc[k]));
				}
				if (j > 0) {
					for (int q = 0; q < 2 * K; q++) {
						tri(prev[q], prev[q + 1], ring[q + 1], nb);
						tri(prev[q], ring[q + 1], ring[q], nb);
					}
				}
				prev = ring;
			}
			r_leaves.end(part, R);
		}
		// The segments: rows of a left margin, the midrib (twice: each half of the V has its own
		// face normal, so the crease shows in the shading) and a right margin. 1.5-2.5 cm wide,
		// widest a tenth of the way out, tapering to a fine tip; stiff, the tip bending down a
		// little (a dead leaf's hang straight).
		const int STEPS = 4;
		const float wmax = R * 0.045f; // 2-3.5 cm wide.
		for (int k = 0; k < K; k++) {
			if (torn[k]) {
				continue;
			}
			const float ang = -span * 0.5f + seg_a * (k + 0.5f);
			const float u = ang / (span * 0.5f);
			Vector3 dir = ray(ang);
			if (!f.dead) {
				dir = (dir - up * (0.1f * (1.0f - Math::abs(u)))).normalized();
			}
			Vector3 sn = nb - dir * nb.dot(dir);
			sn = sn.length_squared() > 1e-6f ? sn.normalized() : up;
			const Vector3 side = sn.cross(dir).normalized();
			const float w0 = rp * seg_a * 0.5f;
			const float L = MAX(sl[k] - rp, R * 0.3f);
			const Vector3 base = hast + ray(ang) * rp;
			const float wk = wmax * (0.85f + 0.3f * pr.randf());
			Vector4i part;
			r_leaves.begin(part);
			int il = -1, iml = -1, imr = -1, ir = -1;
			for (int i = 0; i <= STEPS; i++) {
				const float t = float(i) / STEPS;
				const float w = (t < 0.1f ? Math::lerp(w0, wk, t / 0.1f) : wk * Math::pow((1.0f - t) / 0.9f, 0.6f));
				const float droop = (f.dead ? 0.0f : -0.32f) * L * t * t * t; // The tips droop.
				const Vector3 mid = base + dir * (t * L) + up * droop - sn * (pleat * w);
				const Vector3 lm = mid + side * w + sn * (pleat * w);
				const Vector3 rm = mid - side * w + sn * (pleat * w);
				const float vy = 0.3f + 0.7f * t;
				// Each half's face normal: across the half, crossed with the segment's run.
				const Vector3 nl0 = (lm - mid).cross(dir).normalized();
				const Vector3 nl = nl0.dot(sn) < 0.0f ? -nl0 : nl0;
				const Vector3 nr0 = (rm - mid).cross(dir).normalized();
				const Vector3 nr = nr0.dot(sn) < 0.0f ? -nr0 : nr0;
				const Color crease = f.dead ? sc[k] : sc[k] * 1.08f;
				if (i == STEPS) {
					const int tl = vert(mid, nl, Vector2(0.5f, vy), crease);
					const int tr = vert(mid, nr, Vector2(0.5f, vy), crease);
					tri(iml, il, tl, nl);
					tri(imr, tr, ir, nr);
					break;
				}
				const int l = vert(lm, nl, Vector2(1.0f, vy), sc[k]);
				const int ml = vert(mid, nl, Vector2(0.5f, vy), crease);
				const int mr = vert(mid, nr, Vector2(0.5f, vy), crease);
				const int rr = vert(rm, nr, Vector2(0.0f, vy), sc[k]);
				if (i > 0) {
					tri(iml, il, l, nl);
					tri(iml, l, ml, nl);
					tri(imr, mr, rr, nr);
					tri(imr, rr, ir, nr);
				}
				il = l;
				iml = ml;
				imr = mr;
				ir = rr;
			}
			r_leaves.end(part, R);
		}
		node_drag[f.node] += (f.dead ? 0.3f : 1.0f) * 2.36f * R * R * 0.5f;
		node_mass[f.node] += 2.36f * R * R * 0.4f;
		leaf_count += K;
		{
			// The petiole: smooth, green, stiff, flattened above, from a brown fibrous sheath at
			// the crown (leaves surface, so it is not barked).
			LocalVector<int> pc;
			for (int x = f.node; x > 0 && nodes[x].order > 0; x = nodes[x].parent) {
				pc.push_back(x);
			}
			pc.reverse();
			LocalVector<Vector3> pts;
			LocalVector<float> rad;
			pts.push_back(nodes[pc[0]].start);
			rad.push_back(nodes[pc[0]].radius * 1.6f); // The sheath.
			for (const int x : pc) {
				pts.push_back(nodes[x].end());
				rad.push_back(nodes[x].radius);
				node_drag[x] += 2.0f * nodes[x].radius * nodes[x].length;
				node_mass[x] += float(Math::PI) * nodes[x].radius * nodes[x].radius * nodes[x].length * 900.0f;
			}
			const Color pet_c = f.dead ? dead_c * 0.9f : Color(live.r * 1.05f + 0.02f, live.g * 1.12f + 0.02f, live.b * 0.95f);
			r_leaves.bone = node_bone.size() > uint32_t(pc[0]) ? node_bone[pc[0]] : 0;
			Vector4i pp;
			r_leaves.begin(pp);
			_tube(r_leaves, pts, rad, 5, 0.0f, pet_c);
			// _tube winds for the bark surface, which is flipped when the mesh is built; the leaves
			// are not, so the petiole's faces pointed in against its outward normals, and the path
			// tracer, trusting the winding, lit them as seen from inside: green sparkle. Rewind.
			for (int i = pp.x; i + 2 < r_leaves.indices.size(); i += 3) {
				SWAP(r_leaves.indices.write[i + 1], r_leaves.indices.write[i + 2]);
			}
			// One texel of the leaf texture (its midrib's plain light line): the venation squeezed
			// round a centimetre-thin tube would alias without mips.
			for (int v = pp.z; v < r_leaves.uvs.size(); v++) {
				r_leaves.uvs.write[v] = Vector2(0.5f, 0.5f);
			}
			r_leaves.end(pp, 1.0f); // Sized: kept whole in every LOD.
		}
		// Spines along the petiole's lower two thirds, both edges, pointing forward.
		if (!f.dead) {
			int pn = f.node;
			for (int seg = 0; seg < 3 && pn > 0; seg++, pn = nodes[pn].parent) {
				if (seg == 0) {
					continue; // The top third is unarmed.
				}
				const Node &p = nodes[pn];
				Vector3 sd = p.dir.cross(up);
				sd = sd.length_squared() > 1e-6f ? sd.normalized() : Vector3(1, 0, 0);
				r_leaves.bone = node_bone.size() > uint32_t(pn) ? node_bone[pn] : 0;
				for (int q = 0; q < 6; q++) {
					const float t = (q + 0.5f) / 6.0f;
					for (int side = -1; side <= 1; side += 2) {
						const Vector3 b = p.start + p.dir * (p.length * t) + sd * (p.radius * float(side));
						const Vector3 o = (sd * (0.6f * side) + p.dir * 0.8f).normalized();
						Vector4i sp;
						r_leaves.begin(sp);
						_add_spine(r_leaves, b, o, Math::lerp(0.008f, 0.018f, pr.randf()), spine_c, hash_murmur3_one_32(uint32_t(pn * 16 + q * 2 + side + 1), uint32_t(seed)), false);
						r_leaves.end(sp, -1.0f); // Negative: LOD 0 only (_leaf_lods).
					}
				}
			}
		}
	}
	// Leaf-base stubs and fibre on the trunks, in rings up each trunk.
	// Bark vertex colours are linear (as the bark builder's own): the leaf bases mid brown, the
	// fibre dark brown.
	const Color stub_c = Color(0.3f, 0.22f, 0.15f).srgb_to_linear();
	const Color fibre_c = Color(0.25f, 0.18f, 0.12f).srgb_to_linear();
	for (const int ti : palm_trunks) {
		const Node &n = nodes[ti];
		if (n.length < 0.08f) {
			continue;
		}
		r_bark.bone = node_bone.size() > uint32_t(ti) ? node_bone[ti] : 0;
		Vector3 sx = n.dir.cross(up);
		sx = sx.length_squared() > 1e-6f ? sx.normalized() : Vector3(1, 0, 0);
		const Vector3 sz = sx.cross(n.dir).normalized();
		const int rings = MAX(1, int(n.length / 0.05f));
		for (int ring = 0; ring < rings; ring++) {
			const float t = (ring + 0.5f) / rings;
			const int per = 7;
			const float a0 = pr.randf() * Math::TAU;
			for (int q = 0; q < per; q++) {
				const float a = a0 + Math::TAU * q / per + pr.randf() * 0.3f;
				const Vector3 rd = sx * Math::cos(a) + sz * Math::sin(a);
				const Vector3 b = n.start + n.dir * (n.length * t) + rd * (n.radius * 0.85f);
				LocalVector<Vector3> pts;
				LocalVector<float> rad;
				const float sl = Math::lerp(0.05f, 0.09f, pr.randf());
				pts.push_back(b);
				pts.push_back(b + (rd * 0.55f + n.dir * 0.85f).normalized() * sl);
				rad.push_back(Math::lerp(0.018f, 0.026f, pr.randf()));
				rad.push_back(0.007f);
				Vector4i part;
				r_bark.begin(part);
				_tube(r_bark, pts, rad, 4, 0.0f, stub_c * (0.85f + 0.3f * pr.randf()));
				r_bark.end(part, rad[0]);
				// Fibre: the leaf sheaths' dark brown net, frayed into strands that hang from between
				// the stubs and mat the trunk (most on old trunks): three to five a stub, wavy.
				const int strands = 2 + int(pr.randf() * 2.5f);
				for (int fs = 0; fs < strands; fs++) {
					// Half hang from the stub's foot; half lie across the trunk, the sheath's net.
					const float fa = a + Math::TAU / per * (pr.randf() - 0.5f);
					const Vector3 frd = sx * Math::cos(fa) + sz * Math::sin(fa);
					const Vector3 tang = n.dir.cross(frd).normalized() * (pr.randf() < 0.5f ? 1.0f : -1.0f);
					const bool across = fs & 1;
					const Vector3 f0 = n.start + n.dir * (n.length * t) + frd * (n.radius * Math::lerp(0.98f, 1.1f, pr.randf()));
					const float fl = Math::lerp(0.06f, 0.16f, pr.randf());
					const Vector3 run = across ? (tang * 0.75f - n.dir * 0.65f).normalized() : (-n.dir + frd * 0.15f).normalized();
					// Wavy and soft: five points, each pulled to the trunk and kinked sideways, thin.
					LocalVector<Vector3> fp;
					LocalVector<float> fr;
					const float ph = pr.randf() * Math::TAU;
					for (int fk = 0; fk <= 4; fk++) {
						const float u = fk / 4.0f;
						const float wav = Math::sin(ph + u * 9.0f) * 0.008f;
						fp.push_back(f0 + run * (fl * u) - frd * ((across ? 0.012f : -0.006f) * u) + tang * wav + n.dir * (0.004f * Math::sin(ph * 1.7f + u * 7.0f)));
						fr.push_back(Math::lerp(0.0028f, 0.0012f, u));
					}
					Vector4i fpart;
					r_bark.begin(fpart);
					_tube(r_bark, fp, fr, 3, 0.0f, fibre_c * (0.8f + 0.4f * pr.randf()));
					r_bark.end(fpart, 0.003f);
				}
			}
		}
	}
}

void TreeGenerator::_compute_radii() {
	// A mat's many short shoots would pipe into logs: its limbs are thin for the foliage they
	// carry, a few centimetres (trees.md 5s).
	const float e = MAX(1.0f, params[PARAM_PIPE_EXPONENT] + 0.6f * krummholz);
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
// Wind-flagging (trees.md 5q; Holtmeier 2009, Mountain Timberlines; Yoshino 1973): above the
// snow and the shelter of the ground, wind-driven ice and sand abrade the shoots facing the wind
// and drying kills them, so a flagged tree keeps its limbs on the lee side, swept downwind, and
// on the windward side only dead stubs. Done on the grown tree (as Pirk et al.'s Plastic Trees
// adapt a grown skeleton): killing buds during growth starved the crown into a bare whip. The wind
// blows toward +X.
void TreeGenerator::_wind_flag() {
	if (wind_exposure <= 0.0f || nodes.size() < 3) {
		return;
	}
	const int count = int(nodes.size());
	const float ground = nodes[0].start.y;
	const float shelter = Math::lerp(0.6f, 1.0f, krummholz);
	// Each limb's primary branch: the first node off a stem.
	LocalVector<int> prim;
	prim.resize(count);
	for (int i = 0; i < count; i++) {
		const Node &n = nodes[i];
		prim[i] = -1;
		if (i == 0 || n.order == 0 || n.parent < 0) {
			continue;
		}
		prim[i] = nodes[n.parent].order == 0 ? i : prim[n.parent];
	}
	RandomPCG fr;
	fr.seed(uint64_t(seed) * 0x9e3779b9u + 0x666c6167u);
	LocalVector<float> turn; // Rotation about the vertical toward +X; NaN: windward, killed.
	LocalVector<Vector3> pivot;
	turn.resize(count);
	pivot.resize(count);
	for (int i = 0; i < count; i++) {
		turn[i] = 0.0f;
		if (prim[i] != i || nodes[i].removed) {
			continue;
		}
		const Node &n = nodes[i];
		pivot[i] = n.start;
		const float above = Math::smoothstep(shelter, shelter + 1.5f, n.start.y - ground);
		Vector3 h(n.dir.x, 0.0f, n.dir.z);
		if (h.length_squared() < 1e-6f || above <= 0.0f) {
			continue;
		}
		h.normalize();
		const float windward = -h.x;
		const float kill = wind_exposure * above * Math::smoothstep(-0.6f, 0.25f, windward) * (0.6f + 0.4f * wind_exposure);
		if (fr.randf() < kill) {
			turn[i] = NAN;
			continue;
		}
		// Swept downwind: the limb's bearing turned toward +X by up to about half the way.
		const float az = Math::atan2(h.z, h.x);
		turn[i] = -az * 0.8f * wind_exposure * above;
	}
	for (int i = 0; i < count; i++) {
		const int p = prim[i];
		if (p < 0 || nodes[i].removed) {
			continue;
		}
		Node &n = nodes[i];
		const float t = turn[p];
		if (Math::is_nan(t)) {
			// A dead stub a hand to a forearm long where the limb left the stem.
			if (n.start.distance_to(pivot[p]) < 0.4f) {
				n.dead = true;
				n.leafy = false;
			} else {
				n.removed = true;
			}
			continue;
		}
		if (t != 0.0f) {
			const Basis r(Vector3(0, 1, 0), t);
			n.start = pivot[p] + r.xform(n.start - pivot[p]);
			n.dir = r.xform(n.dir);
		}
	}
	for (Bud &b : buds) {
		if (b.node >= 0 && b.node < count && (nodes[b.node].removed || nodes[b.node].dead)) {
			b.alive = false;
		}
	}
}

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
	// A mat's twigs are pressed flat under the snow, not hanging in curtains (trees.md 5s).
	const float droop = params[PARAM_TWIG_DROOP] * (1.0f - 0.85f * krummholz);
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
// Inside the clipping envelope; with a batter, its depth (z) narrows linearly from the ground to
// the top, an A-shaped hedge's section (trees.md 5ad).
bool TreeGenerator::_in_envelope(const Vector3 &p_point) const {
	if (!envelope.has_point(p_point)) {
		return false;
	}
	if (envelope_batter <= 0.0f) {
		return true;
	}
	const float top = envelope.position.y + envelope.size.y;
	const float t = CLAMP(p_point.y / MAX(top, 0.01f), 0.0f, 1.0f);
	const float half = envelope.size.z * 0.5f * (1.0f - envelope_batter * t);
	return Math::abs(p_point.z - (envelope.position.z + envelope.size.z * 0.5f)) <= half;
}

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
		Vector3 axis = arm.cross(away);
		if (axis.length_squared() < 1e-10f) {
			return false;
		}
		float reach = arm.length();
		if (krummholz > 0.0f) {
			// A mat's limbs lie under the snow: swung aside, never up out of it (a 30 degree swing
			// of a 3 m limb lifted it a metre and a half over the mat).
			axis = Vector3(0.0f, axis.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
			reach = Vector2(arm.x, arm.z).length();
			if (reach < 0.05f) {
				return false;
			}
		}
		const float need = 1.05f * p_clear - dist + 0.25f * p_clear;
		const float angle = need / reach;
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
	// A palm's foliage is its fans (_grow_palm): the petiole tip plus the blade reaching on.
	for (const PalmFrond &f : palm_fronds) {
		const Node &n = nodes[f.node];
		const Vector3 e = n.end() + n.dir * (f.blade * 0.8f);
		height = MAX(height, e.y);
		if (!f.dead) {
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
// Metres of bark one texture tile covers on the tree being built (_build_mesh sets it per bark family).
static thread_local float tube_tile = 0.4f;
// A leader's split (trees.md 5z): the displaced relief fades out within tube_fade_r of
// tube_fade_c, on the bole and on its stems, so their surfaces meet smooth (the normal map keeps
// the bark); displaced, the bole's ridges stood through the stems' furrows as teeth (docs, 18 Nov).
static thread_local Vector3 tube_fade_c;
static thread_local float tube_fade_r = 0.0f;
static thread_local float tube_fade_r0 = 0.0f; // No relief at all within this.
// The swept crotch's height over a split, in bole radii.
static constexpr float TREE_CROTCH_TOP = 1.1f;

// A leader's swept crotch (trees.md 5z, cork round 8): over its split the bole runs on as one
// surface wrapping its stems, the outline of their smooth union (Bloomenthal and Wyvill's
// blended skeletal surfaces; the exponential smooth minimum) cut by each of the bole's rings
// along its own radii, so its bark runs on unbroken; its UVs turn into each stem's own by the top
// ring, which lies on the stems. As separate tubes pushed into the bole each stem showed a
// sleeve rim and its bark changed across it (docs, 18 Nov).
struct TubeCrotch {
	struct Stem {
		LocalVector<Vector3> p;
		LocalVector<float> r;
		LocalVector<float> arc; // Metres along the polyline to each point.
		Vector3 n0; // The stem tube's first frame normal (its u origin).
		float repeats = 1.0f;
		float circ = 1.0f; // Metres per v unit.
	};
	bool active = false;
	Vector3 c, up;
	float rt = 0.0f, h_top = 0.0f;
	float h0 = 0.0f; // Where it starts, under the split (negative).
	LocalVector<Stem> stems;
	float bole_r(float p_h) const {
		// The bole's own top, a blunt dome the stems leave from (its sides the saddles).
		static const float hk[7] = { 0.0f, 0.15f, 0.3f, 0.45f, 0.6f, 0.75f, 0.95f };
		static const float rk[7] = { 1.0f, 0.95f, 0.86f, 0.74f, 0.6f, 0.45f, 0.0f };
		const float h = p_h / rt;
		if (h <= 0.0f) {
			return rt;
		}
		for (int i = 1; i < 7; i++) {
			if (h <= hk[i]) {
				return rt * Math::lerp(rk[i - 1], rk[i], (h - hk[i - 1]) / (hk[i] - hk[i - 1]));
			}
		}
		return 0.0f;
	}
	float blend(float p_h) const {
		return 0.3f * rt * Math::smoothstep(h0, h0 + 0.5f * rt, p_h) * (1.0f - Math::smoothstep(0.55f * h_top, h_top, p_h));
	}
	// Signed distance to each stem's surface (capsule chain), the nearest stem and the margin
	// to the second nearest.
	float stem_d(const Stem &p_s, const Vector3 &p_x, int *r_seg = nullptr, float *r_t = nullptr) const {
		float best = 1e9f;
		for (uint32_t i = 0; i + 1 < p_s.p.size(); i++) {
			const Vector3 ab = p_s.p[i + 1] - p_s.p[i];
			const float l2 = ab.length_squared();
			const float t = l2 > 1e-12f ? CLAMP((p_x - p_s.p[i]).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
			const float d = p_x.distance_to(p_s.p[i] + ab * t) - Math::lerp(p_s.r[i], p_s.r[i + 1], t);
			if (d < best) {
				best = d;
				if (r_seg) {
					*r_seg = int(i);
					*r_t = t;
				}
			}
		}
		return best;
	}
	float field(const Vector3 &p_x, int *r_near = nullptr, float *r_margin = nullptr) const {
		const Vector3 rel = p_x - c;
		const float h = rel.dot(up);
		const float br = bole_r(h);
		const float radial = (rel - up * h).length();
		float d[9];
		int n = 0;
		d[n++] = br > 0.0f ? radial - br : p_x.distance_to(c + up * (0.95f * rt));
		float m = d[0], m2 = 1e9f;
		int near = -1;
		for (uint32_t i = 0; i < stems.size() && n < 9; i++) {
			d[n] = stem_d(stems[i], p_x);
			if (d[n] < m) {
				m2 = m;
				m = d[n];
				near = int(i);
			} else if (d[n] < m2) {
				m2 = d[n];
			}
			n++;
		}
		if (r_near) {
			// The stem nearest, and its lead over every other stem.
			float best = 1e9f, second = 1e9f;
			int bi = -1;
			for (int i = 1; i < n; i++) {
				if (d[i] < best) {
					second = best;
					best = d[i];
					bi = i - 1;
				} else if (d[i] < second) {
					second = d[i];
				}
			}
			*r_near = bi;
			*r_margin = second - best;
		}
		(void)near;
		// The polynomial smooth minimum (Quilez): it blends only within k of a meeting, so the
		// bole keeps its own girth where the stems are still deep inside it (the exponential one
		// swelled it there, a ring where the crotch began).
		const float k = blend(h);
		if (k < 1e-5f) {
			return m;
		}
		float f = d[0];
		for (int i = 1; i < n; i++) {
			const float hh = MAX(k - Math::abs(f - d[i]), 0.0f) / k;
			f = MIN(f, d[i]) - hh * hh * k * 0.25f;
		}
		return f;
	}
};
static thread_local TubeCrotch tube_crotch;
// A split's stem under the swept crotch: drawn a little inside it (its rings, set square to the
// bend at its base, bulge past the straight segments the crotch wraps), full girth at its top.
struct TubeCrotchShrink {
	bool active = false;
	Vector3 c, up;
	float rt = 0.0f, h_top = 0.0f;
};
static thread_local TubeCrotchShrink tube_shrink;

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
	const float repeats = MAX(1.0f, Math::round(girth / tube_tile));
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
	LocalVector<float> side_v; // The swept crotch's v, per side.
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
		// A ring over a leader's split, on the bole's axis: it follows the swept crotch.
		const TubeCrotch &cx = tube_crotch;
		float c_h = 0.0f;
		bool cring = false;
		if (cx.active && !capped) {
			const Vector3 rel = p_points[r] - cx.c;
			c_h = rel.dot(cx.up);
			cring = c_h > cx.h0 + 1e-4f && c_h <= cx.h_top + 1e-4f && (rel - cx.up * c_h).length() < 1e-3f;
		}
		if (int(side_v.size()) != p_sides + 1) {
			side_v.resize(p_sides + 1);
		}
		for (int s = 0; s <= p_sides; s++) {
			const float ang = Math::TAU * s / p_sides;
			const Vector3 dir = nrm * Math::cos(ang) + bin * Math::sin(ang);
			float rr = p_radii[r];
			Vector3 c_pos, c_nrm, c_tan;
			Vector2 c_uv;
			if (!cring) {
				side_v[s] = v;
			}
			if (cring) {
				// The outermost crossing of the blended surface along this side's radius.
				const Vector3 o = p_points[r];
				const float far = 2.6f * cx.rt, step = 0.02f * cx.rt;
				float rho = 0.0f;
				for (float q = far; q > 0.0f; q -= step) {
					if (cx.field(o + dir * q) < 0.0f) {
						float lo = q, hi = q + step;
						for (int it = 0; it < 12; it++) {
							const float mid = 0.5f * (lo + hi);
							if (cx.field(o + dir * mid) < 0.0f) {
								lo = mid;
							} else {
								hi = mid;
							}
						}
						rho = 0.5f * (lo + hi);
						break;
					}
				}
				// Just outside the stems over the blend, just inside them at the top ring, where
				// the stems' own bark takes over.
				rho = MAX(0.0f, rho + Math::lerp(0.002f, -0.004f, Math::smoothstep(0.7f * cx.h_top, cx.h_top, c_h)));
				rr = rho;
				c_pos = o + dir * rho;
				const float e = 0.003f;
				Vector3 g(cx.field(c_pos + Vector3(e, 0, 0)) - cx.field(c_pos - Vector3(e, 0, 0)),
						cx.field(c_pos + Vector3(0, e, 0)) - cx.field(c_pos - Vector3(0, e, 0)),
						cx.field(c_pos + Vector3(0, 0, e)) - cx.field(c_pos - Vector3(0, 0, e)));
				c_nrm = g.length_squared() > 1e-14f ? g.normalized() : dir;
				// The bole's UVs, turning into the nearest stem's toward the top ring (not where
				// two stems meet: there both keep the bole's, down the crotch's valley).
				// v runs over the surface, side by side: by the axis, it smeared over the stems'
				// undersides, where the outline steps out between two rings.
				if (r > 0) {
					side_v[s] += c_pos.distance_to(r_b.verts[base + (r - 1) * (p_sides + 1) + s]) / circumference;
				}
				const Vector2 ub(float(s) / p_sides * repeats, side_v[s]);
				Vector3 tb = bin * Math::cos(ang) - nrm * Math::sin(ang);
				c_uv = ub;
				c_tan = tb;
				int near = -1;
				float margin = 0.0f;
				cx.field(c_pos, &near, &margin);
				if (near >= 0) {
					const TubeCrotch::Stem &st = cx.stems[near];
					int seg = 0;
					float tt = 0.0f;
					cx.stem_d(st, c_pos, &seg, &tt);
					const Vector3 ab = st.p[seg + 1] - st.p[seg];
					const Vector3 tj = ab.length_squared() > 1e-12f ? ab.normalized() : cx.up;
					Vector3 ns = st.n0 - tj * st.n0.dot(tj);
					ns = ns.length_squared() > 1e-10f ? ns.normalized() : nrm;
					const Vector3 bs = tj.cross(ns);
					const Vector3 rel = c_pos - (st.p[seg] + ab * tt);
					float a = Math::atan2(rel.dot(bs), rel.dot(ns));
					if (a < 0.0f) {
						a += float(Math::TAU);
					}
					const Vector2 us(a / float(Math::TAU) * st.repeats, (st.arc[seg] + tt * ab.length()) / st.circ);
					// The stem's UVs shifted by whole tiles to the bole's: shifting the bole's instead
					// squeezed whole tiles into the row under the crotch's first ring, a band.
					const Vector2 uss(us.x - Math::round(us.x - ub.x), us.y - Math::round(us.y - ub.y));
					const float w = Math::smoothstep(0.15f * cx.h_top, cx.h_top, c_h) * Math::smoothstep(0.0f, 0.25f * cx.rt, margin);
					c_uv = ub.lerp(uss, w);
					const Vector3 ts = rel.length_squared() > 1e-12f ? tj.cross(rel.normalized()) : tb;
					c_tan = tb.lerp(ts, w);
				}
				c_tan = c_tan - c_nrm * c_tan.dot(c_nrm);
				c_tan = c_tan.length_squared() > 1e-10f ? c_tan.normalized() : tb;
			}
			if (tube_shrink.active && !cring) {
				const float hs = (p_points[r] + dir * rr - tube_shrink.c).dot(tube_shrink.up);
				rr *= 1.0f - 0.15f * (1.0f - Math::smoothstep(tube_shrink.h_top - 0.3f * tube_shrink.rt, tube_shrink.h_top, hs));
			}
			if (hdata && !cring) {
				// Faded out at the soil line (the tree's ground is y = 0): plates and furrows cut
				// the ground plane in a ragged line, read as a rim around the root collar. Not on
				// fallen wood, which lies across that plane: its underside lost its furrows.
				const float gy = p_points[r].y + dir.y * rr;
				// None on a cap's end face: the furrows ran into its centre as pleats.
				const float face = 1.0f - CLAMP(Math::abs(slope) - 0.5f, 0.0f, 1.0f);
				rr += p_relief * relief_at(float(s) / p_sides * repeats, v) * (p_soil_fade ? Math::smoothstep(-0.02f, 0.2f, gy) : 1.0f) * face *
						(tube_fade_r > 0.0f ? Math::smoothstep(tube_fade_r0, tube_fade_r, p_points[r].distance_to(tube_fade_c)) : 1.0f);
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
				r_b.relief_off.push_back(Vector2(cring ? 0.0f : rr - p_radii[r], p_relief));
			}
			r_b.verts.push_back(cring ? c_pos : p_points[r] + dir * rr + tongue);
			r_b.normals.push_back(cring ? c_nrm : n_out);
			if (end_face) {
				const float rf = p_radii[r] / tube_tile;
				r_b.not_bark.push_back(uint32_t(r_b.verts.size() - 1));
				r_b.uvs.push_back(Vector2(0.5f + rf * Math::cos(ang), 0.5f + rf * Math::sin(ang)));
				r_b.colors.push_back(p_color.lerp(Color(0.3f, 0.24f, 0.17f), 0.7f));
			} else {
				r_b.uvs.push_back(cring ? c_uv : Vector2(float(s) / p_sides * repeats, v));
				r_b.colors.push_back(p_color);
			}
			if (r_b.use_tangents) {
				// Along u; the binormal (normal x tangent) then runs along the stem, with v. On the
				// end face u runs along the frame's normal axis.
				const Vector3 tan = cring ? c_tan : (end_face ? (nrm - n_out * nrm.dot(n_out)).normalized() : bin * Math::cos(ang) - nrm * Math::sin(ang));
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
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Gorse: spines.
	{ TreeGenerator::LEAF_DIGITATE, 3, 0.0f }, // Broom: trifoliate.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Gum rockrose: linear-lanceolate, opposite.
	{ TreeGenerator::LEAF_LOBED, 5, 0.2f }, // Kermes oak: shallow pointed lobes are its spiny teeth, 3-6 a side.
	{ TreeGenerator::LEAF_PINNATE, 8, 0.0f }, // Lentisk: paripinnate, 3-5 pairs, no end leaflet.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Dwarf fan palm: its fans are built by _palm_geometry.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Stone pine.
	{ TreeGenerator::LEAF_LOBED, 6, 0.1f }, // Cork oak: ovate, shallowly toothed.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Holm oak: adult leaves entire or nearly.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Wild olive: narrow lanceolate.
	{ TreeGenerator::LEAF_NEEDLE, 0, 0.0f }, // Maritime pine.
	{ TreeGenerator::LEAF_PINNATE, 8, 0.0f }, // Carob: paripinnate, 2-5 pairs of rounded leaflets.
	{ TreeGenerator::LEAF_SIMPLE, 0, 0.0f }, // Blackthorn: elliptic, finely toothed.
	{ TreeGenerator::LEAF_PALMATE, 5, 0.4f }, // Field maple: five rounded lobes.
	{ TreeGenerator::LEAF_PINNATE, 7, 0.0f }, // Dog rose: five to seven toothed leaflets.
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
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Gorse: evergreen spines.
	{ true, 140.0f, 90.0f, 160.0f, 40.0f, Color(0.22f, 0.2f, 0.07f) }, // Broom: early out, falls soon after turning.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Gum rockrose: evergreen (the summer fold is its own).
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Kermes oak.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Lentisk.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Dwarf fan palm.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Stone pine.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Cork oak.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Holm oak.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Wild olive.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Maritime pine.
	{ false, 0.0f, 0.0f, 1.0f, 0.0f, Color() }, // Carob.
	{ true, 105.0f, 235.0f, 200.0f, 100.0f, Color(0.3f, 0.18f, 0.06f) }, // Blackthorn: leafs after its flowers; dull yellow-brown.
	{ true, 115.0f, 225.0f, 200.0f, 100.0f, Color(0.5f, 0.38f, 0.04f) }, // Field maple: butter yellow.
	{ true, 100.0f, 245.0f, 200.0f, 90.0f, Color(0.35f, 0.2f, 0.06f) }, // Dog rose: early out, dull orange-yellow.
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
	// Heath shrubs (trees.md 5u): the open share of their flowers, and its colour.
	if (p_preset == PRESET_GORSE || p_preset == PRESET_BROOM || p_preset == PRESET_GUM_ROCKROSE) {
		d["flowering"] = get_bloom(p_preset, p_season.get("day_of_year", 182.0f));
		d["flower_color"] = p_preset == PRESET_GUM_ROCKROSE ? Color(0.95f, 0.94f, 0.9f) : Color(0.95f, 0.68f, 0.03f);
	}
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
			// Lentisk's are elliptic, about 2.5:1, on a paripinnate leaf (no end leaflet).
			const bool paripinnate = preset == PRESET_LENTISK || preset == PRESET_CAROB;
			const float hwr = preset == PRESET_ROWAN ? 0.15f : (preset == PRESET_ELDER ? 0.24f : (paripinnate ? 0.22f : 0.19f));
			// Three rows, a quarter wider than the leaflet: the leaflet texture cuts the
			// lanceolate outline and its teeth from it (_leaflet_texture).
			const int steps = 3;
			const float hull = hwr * LEAFLET_HULL;
			// Leaflets of 2 x hwr l x l x 0.656 each share the slots' area.
			const float leaflet = Math::sqrt(slot_area / (float(form.count) * 2.0f * 0.656f * hwr));
			// Lentisk's pairs sit close on a short rachis.
			const float rachis = leaflet * (paripinnate ? 0.42f : 0.6f) * float(pairs + 1);
			for (int i = 0; i < pairs; i++) {
				const float t = Math::lerp(0.25f, 0.9f, pairs > 1 ? float(i) / (pairs - 1) : 0.5f);
				const Vector3 at = start + along * (rachis * t) + n * (-0.15f * rachis * t * t);
				for (int sgn = -1; sgn <= 1; sgn += 2) {
					const Vector3 dir = (side * float(sgn) * 0.85f + along * 0.5f).normalized();
					_add_blade(r_b, at, dir, n.cross(dir).normalized(), n, leaflet, leaflet * hull, steps, 0, 0.0f, p_color, false, 1.0f);
				}
			}
			if (!paripinnate) {
				const Vector3 tip = start + along * rachis + n * (-0.15f * rachis);
				_add_blade(r_b, tip, along, side, n, leaflet, leaflet * hull, steps, 0, 0.0f, p_color, false, 1.0f);
			}
			{
				// The rachis: a stalk 1.5-3 mm across carrying the pairs (grooved above in rowan and
				// ash; Stace 2019). Without it near leaflets floated apart. A shallow V ribbon, so
				// it keeps some width seen edge-on, sampled on the leaflet texture's midrib (opaque).
				// Lentisk's rachis is narrowly winged (Flora iberica), a little wider and redder.
				const float hw = MAX(0.0012f, (paripinnate ? 0.03f : 0.025f) * leaflet);
				const Color rc = paripinnate ? Color(p_color.r * 0.9f, p_color.g * 0.6f, p_color.b * 0.5f, p_color.a) : Color(p_color.r * 0.85f, p_color.g * 0.8f, p_color.b * 0.75f, p_color.a);
				const int segs = 4;
				const int base = r_b.verts.size();
				for (int i = 0; i <= segs; i++) {
					const float t = float(i) / segs;
					const Vector3 mid = start + along * (rachis * t) + n * (-0.15f * rachis * t * t);
					const float w = hw * Math::lerp(1.0f, 0.6f, t);
					const Vector3 pts[3] = { mid + side * w + n * (0.7f * w), mid, mid - side * w + n * (0.7f * w) };
					const Vector3 nrm[3] = { (n + side * 0.6f).normalized(), n, (n - side * 0.6f).normalized() };
					for (int e = 0; e < 3; e++) {
						r_b.verts.push_back(pts[e]);
						r_b.normals.push_back(nrm[e]);
						r_b.uvs.push_back(Vector2(0.49f + 0.01f * e, 0.5f));
						r_b.colors.push_back(rc);
					}
				}
				for (int i = 0; i < segs; i++) {
					const int a = base + i * 3, b = a + 3;
					for (int e = 0; e < 2; e++) {
						r_b.indices.push_back(a + e);
						r_b.indices.push_back(b + e);
						r_b.indices.push_back(a + e + 1);
						r_b.indices.push_back(a + e + 1);
						r_b.indices.push_back(b + e);
						r_b.indices.push_back(b + e + 1);
					}
				}
			}
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

// A gorse spine (trees.md 5u): rigid, deeply furrowed, tapering to a point; three faces, so it
// reads as a furrowed prism from every side. Primary spines (p_branched) carry one or two
// secondary spines near their base (15-30 mm and to 12 mm; Webb et al. 1988).
void TreeGenerator::_add_spine(Builder &r_b, const Vector3 &p_base, const Vector3 &p_out, float p_size, const Color &p_color, uint32_t p_hash, bool p_branched) {
	auto prism = [&](const Vector3 &p_b, const Vector3 &p_d, float p_len, float p_r, float p_roll) {
		Vector3 s0 = p_d.cross(Math::abs(p_d.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
		s0 = s0.rotated(p_d, p_roll);
		const Vector3 s1 = p_d.cross(s0);
		const Vector3 tip = p_b + p_d * p_len;
		Vector3 c[3];
		for (int k = 0; k < 3; k++) {
			const float a = float(Math::TAU) * k / 3.0f;
			c[k] = p_b + (s0 * Math::cos(a) + s1 * Math::sin(a)) * p_r;
		}
		// Flat faces (their own vertices), darker toward the base where the furrows deepen.
		for (int k = 0; k < 3; k++) {
			const Vector3 a = c[k], b = c[(k + 1) % 3];
			Vector3 n = (b - a).cross(tip - a);
			n = n.length_squared() > 1e-20f ? n.normalized() : p_d;
			const int v = r_b.verts.size();
			r_b.verts.push_back(a);
			r_b.verts.push_back(b);
			r_b.verts.push_back(tip);
			const Color base_c = Color(p_color.r * 0.8f, p_color.g * 0.82f, p_color.b * 0.8f);
			// The tip yellows and browns, as gorse spines do.
			const Color tip_c = p_color.lerp(Color(0.32f, 0.26f, 0.1f), 0.35f);
			r_b.colors.push_back(base_c);
			r_b.colors.push_back(base_c);
			r_b.colors.push_back(tip_c);
			for (int q = 0; q < 3; q++) {
				r_b.normals.push_back(n);
			}
			r_b.uvs.push_back(Vector2(0, 0));
			r_b.uvs.push_back(Vector2(1, 0));
			r_b.uvs.push_back(Vector2(0.5f, 1));
			r_b.indices.push_back(v);
			r_b.indices.push_back(v + 2);
			r_b.indices.push_back(v + 1);
		}
	};
	const float r = p_size * 0.045f;
	prism(p_base, p_out, p_size, r, float(p_hash & 255u) / 255.0f * float(Math::TAU));
	if (!p_branched) {
		return;
	}
	const int sides = 1 + int((p_hash >> 8) & 1u);
	const Vector3 ref = Math::abs(p_out.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	const Vector3 across = p_out.cross(ref).normalized();
	for (int k = 0; k < sides; k++) {
		const float t = 0.15f + 0.12f * k + 0.1f * float((p_hash >> (12 + 4 * k)) & 15u) / 15.0f;
		const float roll = float((p_hash >> (20 + 4 * k)) & 15u) / 15.0f * float(Math::TAU);
		const Vector3 side = across.rotated(p_out, roll);
		const Vector3 d = (p_out * 0.64f + side * 0.77f).normalized();
		const float len = p_size * (0.3f + 0.15f * float((p_hash >> (16 + 3 * k)) & 7u) / 7.0f);
		prism(p_base + p_out * (p_size * t), d, len, r * 0.6f, roll);
	}
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
		case PRESET_KERMES_OAK: // Smooth grey, finely fissured only at the base of old stems.
		case PRESET_LENTISK:
		case PRESET_RED_MAPLE:
		case PRESET_WITCH_HAZEL:
			return BARK_SMOOTH;
		case PRESET_SCOTS_PINE:
		case PRESET_WHITE_PINE:
		case PRESET_LARCH:
			return BARK_PLATED;
		case PRESET_STONE_PINE:
			return BARK_PINEA;
		case PRESET_CORK_OAK:
			return BARK_CORK;
		case PRESET_HOLM_OAK: // Dark grey, cracked into small, thin, squarish plates.
		case PRESET_CAROB: // Rough grey-brown, finely cracked.
			return BARK_SCALY;
		case PRESET_MARITIME_PINE: // Very thick, deep furrows between long purplish plates.
			return BARK_PLATED;
		case PRESET_WILD_OLIVE: // Grey, smooth when young, fissured and gnarled when old.
			return BARK_FURROWED;
		case PRESET_BLACKTHORN: // Dark, almost black, finely scaly.
			return BARK_SCALY;
		case PRESET_DOG_ROSE: // Smooth green to brown canes.
			return BARK_SMOOTH;
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

BarkCell bark_voronoi(float p_x, float p_y, int p_cu, int p_cv, uint32_t p_salt, float p_merge = 0.0f, float p_scar = 0.0f, uint32_t p_msalt = 0, float p_vmerge = -1.0f, float p_ax = 1.0f) {
	const float x = p_x * p_cu, y = p_y * p_cv;
	const int ix = int(Math::floor(x)), iy = int(Math::floor(y));
	Vector2 sites[25];
	uint32_t ids[25];
	int n = 0;
	for (int j = -2; j <= 2; j++) {
		for (int i = -2; i <= 2; i++) {
			const int cx = ((ix + i) % p_cu + p_cu) % p_cu, cy = ((iy + j) % p_cv + p_cv) % p_cv;
			const float jx = bark_hash(cx, cy, p_salt), jy = bark_hash(cx, cy, p_salt + 7u);
			sites[n] = Vector2((ix + i + jx) * p_ax, iy + j + jy); // p_ax > 1 stretches the cells along y.
			ids[n] = uint32_t(cy * p_cu + cx);
			n++;
		}
	}
	const Vector2 p(x * p_ax, y);
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
		float e = (mid - p).dot(d / len);
		if (p_ax != 1.0f) {
			// Stretched cells: the fissures between side-by-side plates open wide, the breaks
			// across a plate stay narrow.
			e *= Math::lerp(1.6f, 0.3f, Math::abs(d.x) / len);
		}
		// p_vmerge >= 0: walls to a cell above or below split with their own share, so
		// plates run along the stem for one, two or three cells, offset from their neighbours.
		const float merge = (p_vmerge >= 0.0f && Math::abs(d.y) > Math::abs(d.x)) ? p_vmerge : p_merge;
		if (e < c.edge && bark_cracks(ids[a], ids[k], merge, p_scar, p_msalt)) {
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
// The bark albedo texture's scale: stored divided by it, the material's albedo multiplies it back.
float TreeGenerator::_bark_headroom(BarkKind p_kind) {
	// Stone pine's orange-red fissures run far warmer than its grey-brown bark colour.
	return p_kind == BARK_SMOOTH ? 1.35f : (p_kind == BARK_PINEA ? 3.0f : (p_kind == BARK_CORK ? 1.3f : 1.0f));
}

const TreeGenerator::BarkSet &TreeGenerator::_bark_set(BarkKind p_kind) {
	BarkSet &set = bark_sets[p_kind];
	if (set.albedo.is_valid()) {
		return set;
	}
	// Cork's ridges are several centimetres wide: on the common 0.4 m tile a few dozen of them
	// round a 2 m girth repeat five times and read as a lattice, so cork gets a 1.2 m tile at
	// twice the texels.
	const int N = p_kind == BARK_CORK ? 1024 : 512;
	const float texel = _bark_tile_of(p_kind) / N;
	struct Look {
		int cu, cv; // Plates around and along one tile.
		float crack; // Crack half-width, in cell units.
		float depth; // Metres from plate top to crack floor.
		float grain; // Fibre and surface noise, metres.
		float dark; // Albedo in the cracks.
		float mottle; // Albedo variation over plates.
	};
	static const Look LOOKS[BARK_MAX] = {
		{ 6, 6, 0.0f, 0.0006f, 0.0004f, 1.0f, 0.18f }, // Smooth: no plates, fine grain, lenticels, creases, crust lichen.
		{ 6, 3, 0.1f, 0.028f, 0.004f, 0.3f, 0.12f }, // Furrowed: long interlaced ridges.
		{ 4, 2, 0.16f, 0.03f, 0.0025f, 0.25f, 0.2f }, // Plated: wide flat plates, deep furrows.
		{ 12, 9, 0.06f, 0.006f, 0.0015f, 0.45f, 0.14f }, // Scaly: small overlapping scales.
		{ 5, 4, 0.03f, 0.003f, 0.001f, 0.6f, 0.0f }, // Flaking: shallow edges, colour mosaic.
		{ 3, 40, 0.0f, 0.0008f, 0.0004f, 1.0f, 0.04f }, // Papery: smooth, lenticel bands.
		{ 6, 6, 0.0f, 0.0006f, 0.0004f, 1.0f, 0.12f }, // Lenticelled: smooth, dense raised pale lenticels, peeling skin.
		{ 5, 3, 0.15f, 0.045f, 0.002f, 0.8f, 0.16f }, // Pinea: long flat vertical plates, orange-red fissures (5y).
		{ 9, 12, 0.2f, 0.08f, 0.003f, 0.3f, 0.14f }, // Cork (1.2 m tile): long, irregular, spongy ridges with rounded edges between deep fissures (5aa).
	};
	const Look &L = LOOKS[p_kind];
	const uint32_t salt = 101u + uint32_t(p_kind) * 31u;
	const int F = p_kind == BARK_CORK ? 3 : 1; // Noise cycles a tile, scaled with cork's larger tile.
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
			const float fibre = bark_fbm(u, v, 48 * F, 6 * F, 3, salt + 3u);
			float height = L.grain * (fibre + 0.5f * bark_fbm(u, v, 16 * F, 16 * F, 4, salt + 9u));
			Color a(1, 1, 1);
			float r = 0.88f + 0.08f * fibre;
			if (L.crack > 0.0f) {
				// Warp the lookup so ridges interlace and wander rather than sit on a grid.
				const float warp = (p_kind == BARK_PLATED || p_kind == BARK_PINEA) ? 2.0f : (p_kind == BARK_FLAKING ? 10.0f : (p_kind == BARK_CORK ? 0.9f : 1.0f)); // Cork: amplitude in its 3x tile.
				const float wu = u + 0.035f * warp * bark_fbm(u, v, 4 * F, 4 * F, 2, salt + 21u);
				const float wv = v + 0.05f * warp * bark_fbm(u, v, 4 * F, 4 * F, 2, salt + 27u);
				const float merge = p_kind == BARK_PLATED ? 0.35f : (p_kind == BARK_PINEA ? 0.05f : (p_kind == BARK_SCALY ? 0.1f : (p_kind == BARK_CORK ? 0.5f : 0.0f)));
				const float scar_cut = p_kind == BARK_FLAKING ? 0.62f : 0.0f;
				const BarkCell c = bark_voronoi(wu - Math::floor(wu), wv - Math::floor(wv), L.cu, L.cv, salt, merge, scar_cut, salt + 43u, p_kind == BARK_PINEA ? 0.5f : (p_kind == BARK_CORK ? 0.75f : -1.0f), p_kind == BARK_PINEA ? 1.8f : 1.0f);
				// Cracks open unevenly: wider where the layer split first. Some cell walls never
				// cracked, so plates join into larger irregular shapes, and the plate rims are
				// ragged rather than straight (what makes a Voronoi read as tiles).
				const float crack_w = L.crack * (0.5f + 2.0f * (bark_fbm(u, v, 6 * F, 3 * F, 2, salt + 33u) + 0.5f));
				const float ragged = c.edge + 0.12f * bark_fbm(u, v, 24 * F, 24 * F, 3, salt + 81u) + (p_kind == BARK_FLAKING ? 1.2f * bark_fbm(u, v, 8, 8, 2, salt + 83u) : 0.0f);
				float open = Math::smoothstep(0.0f, crack_w, ragged);
				// Flaking bark is mostly smooth: only a fallen flake leaves an edge, around a shallow
				// scar of fresh, warmer inner bark (sycamore, plane, horse chestnut).
				const bool scar = scar_cut > 0.0f && bark_hash(int(c.id), 5, salt + 43u) > scar_cut; // 0 in the crack, 1 on the plate.
				float plate = open;
				float cork_open = 1.0f;
				if (p_kind == BARK_CORK) {
					// Virgin cork (Natividade 1950; Pereira 2007): Europe's most deeply furrowed bark,
					// a few dozen thick, corky ridges round the trunk, mostly up and down, several
					// centimetres wide with rounded, spongy tops, between deep dark furrows; the ridges
					// merge and split and are broken across here and there.
					// Round 5 (docs, 18 Nov): a Voronoi network of pillows read as fish scales in a
					// lattice at every setting, so the ridges are now the furrowed family's: zero lines
					// of noise stretched along the stem, two families so they merge and split, and the
					// cross breaks dropped (they read as rows of dashes).
					// Round 7 (docs, 18 Nov: thin, stringy ridges like a juniper's): fewer, fatter
					// ridges (six noise cycles a tile, a dozen furrows), fissures twice as wide, and
					// broad cross breaks that cut the ridges into irregular blocks.
					// Round 8 (docs, 18 Nov: thin pencil-line fissures, smooth waxy ridges, pale
					// blotches): each fissure is cut to a width in metres, not a threshold on the
					// noise (which gave hairlines where the noise was steep): the distance to a zero
					// line is |n| / |grad n| (first-order, Taubin 1994). Fissures 2.5-5 cm across and
					// 3.5-6.5 cm deep with rounded shoulders, a second family that splits and merges
					// the ridges, and cross fissures that cut them into blocks, full width.
					const float tile = _bark_tile_of(p_kind);
					const float e = 1.0f / N;
					auto zero_dist = [&](float p_u, float p_v, int p_px, int p_py, uint32_t p_salt) {
						auto at = [&](float p_a, float p_b) {
							const float qu = p_a + 0.035f * warp * bark_fbm(p_a, p_b, 4 * F, 4 * F, 2, salt + 21u);
							const float qv = p_b + 0.05f * warp * bark_fbm(p_a, p_b, 4 * F, 4 * F, 2, salt + 27u);
							return bark_fbm(qu, qv, p_px, p_py, 3, p_salt);
						};
						const float n0 = at(p_u, p_v);
						const float gx = (at(p_u + e, p_v) - at(p_u - e, p_v)) / (2.0f * e * tile);
						const float gy = (at(p_u, p_v + e) - at(p_u, p_v - e)) / (2.0f * e * tile);
						return Math::abs(n0) / MAX(Math::sqrt(gx * gx + gy * gy), 1e-4f);
					};
					auto cut = [](float p_d, float p_w) {
						// 1 on the floor, 0 past the rounded shoulder.
						return Math::pow(1.0f - Math::smoothstep(0.0f, 1.15f, p_d / p_w), 1.6f);
					};
					const float wn = bark_fbm(u, v, 4, 6, 2, salt + 33u) + 0.25f; // 0..0.5
					const float w1 = 0.034f + 0.04f * wn; // Nominal: the first-order distance runs about twice the true one at this noise.
					const float dep = 0.045f + 0.05f * wn;
					const float d1 = zero_dist(u, v, 8, 1, salt + 51u);
					const float c1 = cut(d1, w1);
					// The second family only where it splits a ridge (half the stem).
					const float m2 = Math::smoothstep(-0.02f, 0.06f, bark_fbm(u, v, 3, 4, 2, salt + 63u));
					const float d2 = zero_dist(u, v, 6, 1, salt + 55u);
					const float c2 = m2 * cut(d2, 0.8f * w1);
					// Cross fissures, about every 15-25 cm, on most of the ridges.
					const float m3 = Math::smoothstep(-0.08f, 0.02f, bark_fbm(u, v, 5, 3, 2, salt + 67u));
					const float d3 = zero_dist(u, v, 3, 8, salt + 59u);
					const float c3 = m3 * cut(d3, 0.6f * w1);
					const float depth = dep * MAX(MAX(c1, 0.8f * c2), 0.65f * c3);
					cork_open = 1.0f - MAX(MAX(c1, c2), 0.85f * c3);
					// Lumpy, crumbly ridge tops, at the scale of a thumb.
					const float lumps = bark_fbm(u, v, 30, 20, 3, salt + 57u);
					// Each block swells from its fissures to a rounded crown (flat between grooves, it
					// read as carved plaster).
					const float dm = MIN(d1, MIN(m2 > 0.5f ? d2 : 1.0f, m3 > 0.5f ? d3 : 1.0f));
					const float crown = 0.03f * Math::smoothstep(0.3f * w1, 2.2f * w1, dm);
					plate = 1.0f - (depth + 0.03f - crown) / L.depth + 0.05f * lumps;
					open = cork_open;
				} else if (p_kind == BARK_FURROWED) {
					// Furrows are the zero lines of noise stretched along the stem: long, wavy, merging
					// and splitting like an oak's interlaced ridges. The cells only add the shallower
					// cross breaks that cut ridges into lengths.
					const float n = bark_fbm(wu, wv, L.cu, 1, 3, salt + 51u);
					const float furrow = Math::smoothstep(0.0f, 0.18f, Math::abs(n));
					const float cross = Math::lerp(1.0f, open, 0.55f);
					open = MIN(furrow, cross);
					plate = Math::sqrt(open); // Rounded ridge tops.
				} else if (p_kind == BARK_PLATED || p_kind == BARK_PINEA) {
					// Flat plate tops, steep walls, and each plate built of thin flaking layers.
					const float layers = Math::floor((bark_fbm(u, v, 12, 6, 3, salt + 57u) + 0.5f) * 4.0f) / 4.0f;
					plate = Math::smoothstep(0.0f, 0.5f, open) * (0.75f + 0.25f * layers);
				} else if (p_kind == BARK_SCALY) {
					// Shingles: each scale lifts toward its lower edge, over the next one down.
					const float below = (wv - Math::floor(wv)) * L.cv - c.site.y; // -1..1 cell units
					plate = open * (0.55f + 0.45f * CLAMP(0.5f + below, 0.0f, 1.0f));
				}
				// Plate tone drifts smoothly (a per-cell tone outlines joined plates).
				const float plate_noise = 0.5f + bark_fbm(u, v, 5 * F, 3 * F, 2, salt + 41u) * 1.6f;
				height += L.depth * (plate - 1.0f) + L.depth * 0.15f * (plate_noise - 0.5f) * plate;
				const float shade = Math::lerp(L.dark, 1.0f + L.mottle * (plate_noise - 0.5f), p_kind == BARK_CORK ? Math::smoothstep(0.0f, 0.3f, open) : plate);
				a = Color(shade, shade, shade);
				if (p_kind == BARK_CORK) {
					// Cork's fissures are warm red-brown under the grey-buff, weathered ridges.
					// The whole fissure darkens, its walls the fresher red-brown cork, its floor in
					// shadow-dark (a thin dark line in a pale field read as pencil, docs 18 Nov); the
					// ridge tops mottle at a few centimetres, never in broad patches.
					const float mott = 1.0f + 0.22f * bark_fbm(u, v, 40, 30, 3, salt + 75u);
					const Color wall(0.78f, 0.56f, 0.42f), floor_c(0.2f, 0.15f, 0.12f);
					const Color fis = floor_c.lerp(wall, Math::smoothstep(0.1f, 0.8f, cork_open));
					a = fis.lerp(Color(mott, mott, mott), Math::smoothstep(0.7f, 1.0f, cork_open));
				}
				if (p_kind == BARK_PINEA) {
					// Stone pine (Gymnosperm Database: "red-brown to orange with blackish edges to
					// the plates"): weathered grey-brown plate tops, the fresh orange-red inner
					// layers in the fissures. The bark colour is the plate tops'; the fissures' factor
					// (linear) turns it orange-red, inside the headroom (_bark_headroom).
					const Color top(1.0f, 1.0f, 1.0f), fissure(1.75f, 0.72f, 0.42f);
					a = fissure.lerp(top, Math::smoothstep(0.3f, 0.6f, open)) * shade;
					// Where the outer layers of a plate have flaked off, the warmer layer under them.
					const float flaked = Math::smoothstep(0.08f, 0.2f, bark_fbm(u, v, 6, 3, 3, salt + 61u));
					a = a * Color(1.0f, 1.0f, 1.0f).lerp(Color(1.45f, 0.9f, 0.7f), 0.4f * flaked * Math::smoothstep(0.6f, 0.9f, open));
					// Blackish plate rims, between the orange floor and the grey top.
					a = a * Math::lerp(0.55f, 1.0f, MAX(1.0f - Math::smoothstep(0.15f, 0.35f, open), Math::smoothstep(0.55f, 0.85f, open)));
				}
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
				const int ix = int(Math::floor(lx)), iy = int(Math::floor(ly));
				if (p_kind == BARK_PAPERY) {
					// Birch: each cell's dash anywhere in it, read from the neighbouring cells too so a
					// long one runs over its cell's edge; lengths from a few millimetres to a hand,
					// thick and thin, and denser in bands up the stem (a dash per cell at a jittered
					// spot still read as a grid, docs 17 Nov).
					const float band = bark_fbm(u, v, 2, 9, 2, salt + 69u);
					float m = 0.0f;
					for (int oy = -1; oy <= 1; oy++) {
						for (int ox = -1; ox <= 1; ox++) {
							const int jx = ((ix + ox) % cu + cu) % cu, jy = ((iy + oy) % cv + cv) % cv;
							const float on = bark_hash(jx, jy, salt + 61u);
							if (on < 0.45f - 1.6f * band) {
								continue;
							}
							const float cx = float(ix + ox) + bark_hash(jx, jy, salt + 63u);
							const float cy = float(iy + oy) + bark_hash(jx, jy, salt + 67u);
							const float h = bark_hash(jx, jy, salt + 65u);
							const float len = 0.05f + 0.75f * h * h;
							const float thick = 0.1f + 0.2f * bark_hash(jx, jy, salt + 64u) * (0.4f + 0.6f * h);
							// Ragged ends: a slight bow and taper.
							const float dx = (lx - cx) / len;
							const float dy = (ly - cy - 0.08f * dx * dx) / (thick * (1.0f - 0.4f * dx * dx));
							m = MAX(m, 1.0f - Math::smoothstep(0.6f, 1.0f, dx * dx + dy * dy));
						}
					}
					if (m > 0.0f) {
						a = a * Math::lerp(1.0f, 0.18f, m);
						height += m * 0.0004f;
						r = Math::lerp(r, 0.97f, m);
					}
				} else {
					const float on = bark_hash(ix, iy, salt + 61u);
					if (on > 0.9f) {
						const float cx = 0.5f + 0.3f * (bark_hash(ix, iy, salt + 63u) - 0.5f);
						const float len = 0.1f + 0.08f * on;
						// Off the cell's middle both ways, so they do not line up in rows.
						const float cy = 0.5f + 0.5f * (bark_hash(ix, iy, salt + 67u) - 0.5f);
						const float dx = (lx - ix - cx) / len, dy = (ly - iy - cy) / 0.1f;
						const float m = 1.0f - Math::smoothstep(0.6f, 1.0f, dx * dx + dy * dy);
						// Faint on beech: dashes darker than this read as a printed pattern up close;
						// flush (a raised rim lit as a ring of dew drops at 40 cm).
						a = a * Math::lerp(1.0f, 0.95f, m);
						height += m * 0.00005f;
						r = Math::lerp(r, 0.97f, m);
					}
				}
				if (p_kind == BARK_SMOOTH) {
					// Creases: short fine wrinkles across the stem where the thin bark stretched
					// as the stem thickened (beech, hornbeam, hazel; Vaucher, Tree Bark, 2003),
					// a few centimetres long, darker in their fold.
					const int wu_n = 9, wv_n = 70;
					const float qx = u * wu_n, qy = v * wv_n;
					const int jx = int(qx), jy = int(qy);
					const float pick = bark_hash(jx, jy, salt + 91u);
					// Sparse and soft: dense regular folds read as a tiling hatch at 40 cm.
					if (pick > 0.9f) {
						const float cx = 0.5f + 0.4f * (bark_hash(jx, jy, salt + 93u) - 0.5f);
						const float half = 0.25f + 0.35f * bark_hash(jx, jy, salt + 95u);
						const float cy = 0.5f + 0.4f * (bark_hash(jx, jy, salt + 97u) - 0.5f);
						// A gentle bow: the fold sags towards its ends.
						const float dxw = (qx - jx - cx) / half;
						const float dyw = (qy - jy - cy - 0.15f * dxw * dxw) / 0.09f;
						const float m = (1.0f - Math::smoothstep(0.5f, 1.0f, Math::abs(dxw))) * (1.0f - Math::smoothstep(0.3f, 1.0f, Math::abs(dyw)));
						a = a * Math::lerp(1.0f, 0.95f, m);
						height -= m * 0.00015f;
					}
				}
				if (p_kind == BARK_SMOOTH) {
					// Crust lichens, the beech's own (Barkman 1958; Gilbert 2000): thin pale
					// films a hand across with lobed, wandering edges, granular inside, a few
					// tiny dark fruiting dots, and here and there the black script of Graphis.
					// Round discs on a grid read as stamps up close (docs, 16 Nov): the colonies
					// are thresholds of noise at three scales instead, so no two edges repeat.
					const float c1 = bark_fbm(u, v, 5, 3, 4, salt + 111u);
					const float c2 = bark_fbm(u, v, 11, 8, 3, salt + 112u);
					const float field = c1 + 0.45f * c2;
					const float colony = Math::smoothstep(0.045f, 0.075f, field);
					// A darker margin line where a colony meets bare bark or its neighbour.
					const float margin = Math::exp(-Math::pow((field - 0.045f) / 0.008f, 2.0f));
					const float grain = bark_fbm(u, v, 96, 96, 2, salt + 114u);
					if (colony > 0.0f || margin > 0.01f) {
						// Whitish grey crusts, paler than the bark, granular. The weight is clamped: past 1
						// the lerp overshot to white that the texture clipped flat, a beech log's whole
						// side one tone at 40 cm (docs, 16 Nov).
						const float tone = 0.85f + 0.15f * Math::smoothstep(-0.05f, 0.15f, c2);
						a = a.lerp(Color(1.3f, 1.3f, 1.24f) * tone, CLAMP(0.75f * colony * (0.8f + 2.0f * grain), 0.0f, 0.9f));
						a = a * Math::lerp(1.0f, 0.82f, margin * 0.7f);
						height += 0.0002f * colony + 0.00015f * colony * grain;
						r = Math::lerp(r, 0.9f, colony);
						// Fruiting dots: under a millimetre, sparse, only inside a colony.
						const int du = 90, dv = 90;
						const float lx = u * du, ly = v * dv;
						const int ix = int(lx), iy = int(ly);
						if (bark_hash(ix, iy, salt + 113u) < 0.04f * colony) {
							const float cx = 0.25f + 0.5f * bark_hash(ix, iy, salt + 115u);
							const float cy = 0.25f + 0.5f * bark_hash(ix, iy, salt + 117u);
							const float rr = 0.2f + 0.15f * bark_hash(ix, iy, salt + 119u);
							const float d = Math::sqrt((lx - ix - cx) * (lx - ix - cx) + (ly - iy - cy) * (ly - iy - cy)) / rr;
							a = a * Math::lerp(1.0f, 0.62f, 1.0f - Math::smoothstep(0.6f, 1.0f, d));
						}
					}
					// Graphis: few, fine and grey-black, in their own patches.
					const float script = Math::smoothstep(0.06f, 0.1f, bark_fbm(u, v, 4, 3, 3, salt + 131u));
					if (script > 0.0f) {
						const int gu = 22, gv = 40;
						const float gx = u * gu, gy = v * gv;
						const int jx = int(gx), jy = int(gy);
						if (bark_hash(jx, jy, salt + 121u) < 0.15f) {
							const float cx = 0.3f + 0.4f * bark_hash(jx, jy, salt + 123u);
							const float cy = 0.3f + 0.4f * bark_hash(jx, jy, salt + 127u);
							const float ox = gx - jx - cx, oy = (gy - jy - cy);
							const float len = 0.15f + 0.25f * bark_hash(jx, jy, salt + 129u);
							const float wav = ox - 0.06f * Math::sin(oy * 11.0f + 6.0f * bark_hash(jx, jy, salt + 125u));
							const float line = (1.0f - Math::smoothstep(0.02f, 0.05f, Math::abs(wav))) * (1.0f - Math::smoothstep(len - 0.08f, len, Math::abs(oy)));
							a = a * Math::lerp(1.0f, 0.72f, line * script);
						}
					}
					// Green algal film (Desmococcus) in drifts, and silvery patches where the bark
					// is dry: beech is mottled grey, green-grey and silver, not one grey.
					// Drifts, not a cast: under a third of the bark (an even cover tinted it olive).
					const float alg = Math::smoothstep(0.06f, 0.18f, bark_fbm(u, v, 2, 3, 4, salt + 141u));
					a = Color(a.r * Math::lerp(1.0f, 0.84f, alg), a.g * Math::lerp(1.0f, 0.98f, alg), a.b * Math::lerp(1.0f, 0.66f, alg));
					// Darker damp patches where water stands longer.
					a = a * Math::lerp(1.0f, 0.78f, Math::smoothstep(0.04f, 0.16f, bark_fbm(u, v, 3, 2, 4, salt + 145u)));
					// Mottling a few centimetres across, grey on grey: without it a hand's width of
					// bark between colonies read as one flat wall at 40 cm (docs, 16 Nov). Softer
					// than before: big smooth blotches were what read as cloud noise (docs, 16 Nov).
					a = a * Math::lerp(0.76f, 1.1f, Math::smoothstep(-0.1f, 0.1f, bark_fbm(u, v, 10, 7, 3, salt + 147u)));
					a = a * Math::lerp(0.86f, 1.06f, Math::smoothstep(-0.08f, 0.08f, bark_fbm(u, v, 24, 16, 2, salt + 149u)));
					// Elephant-skin wrinkling (beech's own; Vaucher 2003): a net of fine creases a few
					// centimetres across, sharp in the fold and rounded between, as ridged noise
					// (1 - |n|) rather than soft value noise, so the relief has edges a low sun catches.
					{
						// Stretched round the stem, so the folds run mostly across it as on a beech
						// (isotropic noise drew wandering contour loops), and only in patches.
						const float n1 = bark_fbm(u, v, 5, 36, 3, salt + 143u);
						const float patch = Math::smoothstep(-0.02f, 0.1f, bark_fbm(u, v, 4, 6, 2, salt + 151u));
						const float fold = (1.0f - Math::smoothstep(0.0f, 0.03f, Math::abs(n1))) * patch;
						height += 0.0003f * bark_fbm(u, v, 12, 8, 3, salt + 165u) - 0.00025f * fold;
						a = a * Math::lerp(1.0f, 0.9f, fold);
					}
					// Horizontal striations: the lenticels' bands, faint lines across the stem a few
					// millimetres to a centimetre apart, broken into dashes (what makes beech read as
					// beech up close, with the creases).
					{
						const float band_v = v * 52.0f + 0.6f * bark_fbm(u, v, 3, 2, 2, salt + 153u) * 8.0f;
						const float row = Math::floor(band_v);
						const float in_row = band_v - row;
						const float lw = 0.06f + 0.05f * bark_hash(int(row), 3, salt + 155u);
						const float line = 1.0f - Math::smoothstep(lw * 0.5f, lw, Math::abs(in_row - 0.5f));
						const float dash = Math::smoothstep(0.0f, 0.08f, bark_fbm(u * 1.0f + row * 0.37f, v, 40, 1, 2, salt + 157u));
						const float sl = line * dash * (0.5f + 0.5f * bark_hash(int(row), 7, salt + 159u));
						a = a * Math::lerp(1.0f, 0.88f, sl);
						height -= 0.00012f * sl;
					}
					// Granular surface: the bark's millimetre grain, so a hand's distance shows a
					// crisp matt skin rather than smooth gradients.
					{
						const float gr = bark_hash(x, y, salt + 161u) - 0.5f;
						const float gr2 = bark_fbm(u, v, 160, 160, 1, salt + 163u);
						a = a * (1.0f + 0.07f * gr + 0.12f * gr2);
						height += 0.00004f * gr + 0.00008f * gr2;
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
	if (p_kind == BARK_CORK) {
		// A pillow shaped from the distance to its cell's straight walls has sharp creases along
		// the cell's medial axis: a faceted pyramid (docs, 18 Nov: faceted flakes, a reptile's
		// scales). Blurring the relief over about a centimetre rounds them into swollen ridges;
		// the spongy pitting goes back on top, unblurred.
		LocalVector<float> tmp;
		tmp.resize(N * N);
		const int R = 2;
		for (int pass = 0; pass < 1; pass++) {
			for (int y = 0; y < N; y++) {
				for (int x = 0; x < N; x++) {
					float sum = 0.0f;
					for (int k = -R; k <= R; k++) {
						sum += hw[y * N + (x + k + N) % N];
					}
					tmp[y * N + x] = sum / (2 * R + 1);
				}
			}
			for (int y = 0; y < N; y++) {
				for (int x = 0; x < N; x++) {
					float sum = 0.0f;
					for (int k = -R; k <= R; k++) {
						sum += tmp[((y + k + N) % N) * N + x];
					}
					hw[y * N + x] = sum / (2 * R + 1);
				}
			}
		}
		for (int y = 0; y < N; y++) {
			for (int x = 0; x < N; x++) {
				const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
				const int k = y * N + x;
				// Virgin cork is a spongy, porous mass (Pereira 2007): crumbly faces a few millimetres
				// across, open pores, and weathering from pale grey to ochre. Without them the ridges
				// read as melted wax (docs, 18 Nov).
				const float crumb = bark_fbm(u, v, 200, 160, 2, salt + 173u);
				const float pore = Math::smoothstep(0.08f, 0.16f, bark_fbm(u, v, 320, 240, 1, salt + 177u));
				hw[k] += 0.003f * bark_fbm(u, v, 72, 36, 3, salt + 171u) + 0.0025f * crumb - 0.003f * pore;
				const float ochre = Math::smoothstep(-0.1f, 0.15f, bark_fbm(u, v, 6, 4, 3, salt + 179u));
				// The ochre weathering is faint: strong, it read as pale and brown blotches across
				// the ridges, a leopard's coat (docs, 18 Nov).
				const Color tint = Color(1.0f, 1.0f, 1.0f).lerp(Color(1.08f, 0.98f, 0.84f), 0.3f * ochre) * ((1.0f + 0.3f * crumb) * (1.0f - 0.35f * pore));
				aw[k * 3 + 0] *= tint.r;
				aw[k * 3 + 1] *= tint.g;
				aw[k * 3 + 2] *= tint.b;
				rw[k] = 1.0f; // Matte throughout.
			}
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
				// Smooth bark's pale crust colonies stand above 1 and were clipped flat (no lichen
				// left on a beech log at 40 cm, docs 16 Nov): stored with headroom, which the
				// material's albedo gives back.
				const float f = CLAMP(aw[k * 3 + c] * 0.92f / _bark_headroom(p_kind), 0.0f, 1.0f);
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
	if (p_kind == BARK_CORK) {
		// Cork's pillows are wider than the relief tube's grid, but their sampled heights still
		// pick up the pits and the ridges' rims and fold the tube into sharp facets (docs, 18 Nov:
		// faceted flakes and knife-cut slits). The tube takes a prefiltered map, about the grid's
		// size; the normal map keeps the fine relief.
		float *H = reinterpret_cast<float *>(hbytes.ptrw());
		LocalVector<float> tmp;
		tmp.resize(N * N);
		const int R = 6;
		for (int pass = 0; pass < 1; pass++) {
			for (int y = 0; y < N; y++) {
				for (int x = 0; x < N; x++) {
					float sum = 0.0f;
					for (int k = -R; k <= R; k++) {
						sum += H[y * N + (x + k + N) % N];
					}
					tmp[y * N + x] = sum / (2 * R + 1);
				}
			}
			for (int y = 0; y < N; y++) {
				for (int x = 0; x < N; x++) {
					float sum = 0.0f;
					for (int k = -R; k <= R; k++) {
						sum += tmp[((y + k + N) % N) * N + x];
					}
					H[y * N + x] = sum / (2 * R + 1);
				}
			}
		}
	}
	set.height = Image::create_from_data(N, N, false, Image::FORMAT_RF, hbytes);
	if (OS::get_singleton()->has_environment("TREE_BARK_DUMP") && p_kind == BARK_CORK) {
		const String dir = OS::get_singleton()->get_environment("TREE_BARK_DUMP");
		Ref<Image> a2 = Image::create_from_data(N, N, false, Image::FORMAT_RGB8, abytes);
		a2->save_png(dir + "/cork_albedo.png");
		const float *H = reinterpret_cast<const float *>(hbytes.ptr());
		float lo = 1e9f, hi = -1e9f;
		for (int i = 0; i < N * N; i++) {
			lo = MIN(lo, H[i]);
			hi = MAX(hi, H[i]);
		}
		Vector<uint8_t> hb;
		hb.resize(N * N);
		for (int i = 0; i < N * N; i++) {
			hb.write[i] = uint8_t(CLAMP((H[i] - lo) / MAX(hi - lo, 1e-6f) * 255.0f, 0.0f, 255.0f));
		}
		Ref<Image> h2 = Image::create_from_data(N, N, false, Image::FORMAT_L8, hb);
		h2->save_png(dir + "/cork_height.png");
		print_line(vformat("cork height range %.4f .. %.4f", lo, hi));
	}
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
		const int g_around = int(0.025f / texel + 0.5f), g_along = int(0.05f / texel + 0.5f); // The relief tube's grid.
		const float grid = grid_error(g_around, g_along);
		set.relief_gain = MAX(0.0f, flat - grid);
		// Rings can be further apart along a stem when the pattern runs along it (furrowed bark's
		// long ridges): the longest spacing whose error stays within half a millimetre of 5 cm.
		set.relief_along = 0.05f;
		for (int kv : { 96, 128 }) {
			if (p_kind == BARK_CORK) {
				break; // Long rings over the cork's pillows read as diamond facets.
			}
			if (grid_error(g_around, kv) <= grid + 0.0005f) {
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
	0.35f, // Gorse.
	0.25f, // Broom.
	0.1f, // Gum rockrose: resin-varnished stems in dry sun.
	0.2f, // Kermes oak.
	0.2f, // Lentisk.
	0.05f, // Dwarf fan palm: fibre, sun.
	0.1f, // Stone pine: dry, sunny coasts.
	0.05f, // Cork oak: a dry climate; a little moss low on the shaded side.
	0.15f, // Holm oak: deep shade under the dome keeps the base damp.
	0.05f, // Wild olive: dry and sunny.
	0.1f, // Maritime pine: Atlantic dunes, damper than the stone pine's.
	0.05f, // Carob: dry coasts.
	0.4f, // Blackthorn.
	0.6f, // Field maple: corky bark holds moss.
	0.2f, // Dog rose: thin canes.
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
	if (fallen_branch && shed_radius_max <= 0.05f) {
		// Sticks dry out between showers (all surface, little wood to hold water): a few cushions,
		// not the coat a log carries.
		k *= 0.4f;
	} else if (fallen || fallen_branch) {
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
	{ 0.9f, 0.75f, 0.12f }, // Gorse: old coastal gorse is grey with Ramalina and Usnea.
	{ 0.4f, 0.4f, 0.05f }, // Broom.
	{ 0.2f, 0.3f, 0.05f }, // Gum rockrose: sticky, short-lived stems.
	{ 0.6f, 0.5f, 0.1f }, // Kermes oak: smooth grey bark, crusts and some Xanthoria near the sea.
	{ 0.6f, 0.5f, 0.1f }, // Lentisk.
	{ 0.1f, 0.3f, 0.05f }, // Dwarf fan palm.
	{ 0.08f, 0.15f, 0.0f }, // Stone pine: thick flaking plates hold little.
	{ 0.0f, 0.25f, 0.05f }, // Cork oak (none: docs 18 Nov, the crusts on cork read as hard-edged green decals): thin crusts on the ridges (worked trees are stripped every nine years; docs 18 Nov: big patches read as decals).
	{ 0.5f, 0.3f, 0.05f }, // Holm oak: grey crusts on the dark bark.
	{ 0.6f, 0.3f, 0.15f }, // Wild olive: crusts and Xanthoria on old trunks.
	{ 0.1f, 0.2f, 0.0f }, // Maritime pine: thick, flaking plates.
	{ 0.4f, 0.3f, 0.1f }, // Carob.
	{ 0.8f, 0.5f, 0.15f }, // Blackthorn: grey crusts and Usnea in old hedges.
	{ 0.7f, 0.5f, 0.1f }, // Field maple.
	{ 0.3f, 0.2f, 0.0f }, // Dog rose.
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
		// In the litter's damp and shade crusts give way to moss and algae; a log's full cover
		// read as one flat olive-grey wall at 40 cm (docs, 16 Nov).
		k *= 0.25f;
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
	const int sectors = 128;
	// The rim: torn ragged, lobed where the big roots held more soil.
	float rim[sectors];
	for (int k = 0; k < sectors; k++) {
		const float a = float(Math::TAU) * k / sectors;
		const Vector3 q(Math::cos(a) * 1.7f, 0.0f, Math::sin(a) * 1.7f);
		rim[k] = R * (0.82f + 0.3f * moss_noise(q + Vector3(3.1f, 0, 0), salt + 1u) + 0.06f * (moss_noise(q * 6.0f, salt + 18u) - 0.5f));
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
		// Torn, not smoothed: soil breaks away in blocks along root channels and cracks, a
		// fist-to-head-sized relief over the whole face.
		const float blocks = Math::abs(moss_noise(p_q * 4.5f, salt + 15u) - 0.5f);
		// Small blocks and crumbs down to a few centimetres: the grid is about 4 cm here.
		const float fine = Math::abs(moss_noise(p_q * 14.0f, salt + 16u) - 0.5f) + 0.5f * (moss_noise(p_q * 30.0f, salt + 17u) - 0.5f);
		return -T * (0.2f + 0.8f * Math::pow(MAX(1.0f - p_rho * p_rho, 0.0f), 0.6f)) * (1.0f - 0.25f * rotten) + T * 0.45f * lump + 0.05f * clod - 0.09f * (0.25f - blocks) * (1.0f - 0.4f * rotten) - 0.035f * (0.25f - fine);
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
	const int rings = 36;
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
	// Lumps on the underside: a sphere of p_seg rings, its vertices shared so it shades round,
	// pushed out by noise (p_jitter at frequency p_jitter_freq) and scaled by p_sc; p_angular
	// folds the noise into ridges so a stone shows edges and flat faces, not a blob.
	auto lump = [&](const Vector3 &p_c, const Basis &p_b, const Vector3 &p_sc, float p_jitter, const Color &p_col, float p_jitter_freq, int p_seg, float p_angular = 0.0f) {
		const int rings = MAX(p_seg, 3), segs = 2 * rings;
		const int first = w.verts.size();
		const uint32_t ls = salt + uint32_t(Math::abs(p_c.x * 1000.0f + p_c.z * 37.0f));
		for (int i = 0; i <= rings; i++) {
			const float th = float(Math::PI) * float(i) / rings;
			for (int j = 0; j < segs; j++) {
				const float ph = float(Math::TAU) * float(j) / segs;
				const Vector3 u(Math::sin(th) * Math::cos(ph), Math::cos(th), Math::sin(th) * Math::sin(ph));
				const float nz = moss_noise(u * p_jitter_freq + Vector3(3.1f, 7.7f, 1.3f), ls);
				const float nz2 = moss_noise(u * p_jitter_freq * 2.7f + Vector3(5.3f, 2.1f, 9.4f), ls + 7u);
				float j_amt = (nz - 0.5f) + 0.4f * (nz2 - 0.5f);
				if (p_angular > 0.0f) {
					// Facets: quantised toward a few planes.
					j_amt = Math::lerp(j_amt, Math::round(j_amt * 3.0f) / 3.0f, p_angular);
				}
				const Vector3 lp = p_c + p_b.xform(u * p_sc * (1.0f + p_jitter * j_amt));
				const float shade = (0.82f + 0.18f * u.y) * (0.85f + 0.3f * nz2);
				push(lp, p_b.xform(u), Color(p_col.r * shade, p_col.g * shade, p_col.b * shade), Vector2(lp.x, lp.z) * 4.0f);
			}
		}
		for (int i = 0; i < rings; i++) {
			for (int j = 0; j < segs; j++) {
				const int a0 = first + i * segs + j, a1 = first + i * segs + (j + 1) % segs;
				const int b0 = a0 + segs, b1 = a1 + segs;
				if (i > 0) {
					tri(a0, b0, a1);
				}
				if (i + 1 < rings) {
					tri(a1, b0, b1);
				}
			}
		}
	};
	// Stones in the underside, half out of the soil: glacial till and brash carry stones from
	// grit to small boulders, and a plate lifts them with it (Coutts 1986; the pit-and-mound
	// literature: Schaetzl et al. 1989). Mostly small, a few the size of a head; soil-stained.
	const int stones = 14 + int(pr.rand() % 16);
	for (int st = 0; st < stones; st++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::sqrt(pr.randf()) * 0.95f;
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 c = d * (rim_at(a) * rho);
		const float u = pr.randf();
		const float sz = Math::lerp(0.03f, 0.22f, u * u * u) * (0.6f + 0.4f * R / 2.0f);
		c.y = under(rho, c) + sz * Math::lerp(0.1f, 0.45f, pr.randf());
		const Basis b = Basis(Vector3(pr.randf(), pr.randf(), pr.randf()).normalized(), pr.randf() * 3.0f);
		const Vector3 sc(sz * Math::lerp(0.8f, 1.3f, pr.randf()), sz * Math::lerp(0.5f, 0.8f, pr.randf()), sz * Math::lerp(0.8f, 1.2f, pr.randf()));
		const float tone = 0.7f + 0.35f * pr.randf();
		const Color stained = stone_c.lerp(mineral, Math::lerp(0.25f, 0.6f, pr.randf()));
		lump(c, b, sc, 0.45f, Color(stained.r * tone, stained.g * tone, stained.b * tone), 1.8f, 6, 0.6f);
	}
	// Clods: soil held in the root mat, standing proud of the face round the laterals and along
	// the rim where the plate tore, lumps from a fist to a bucket across, crumbling with rot.
	const int clods = int((22.0f + 10.0f * pr.randf()) * R * (1.0f - 0.4f * rotten));
	for (int k = 0; k < clods; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::lerp(0.2f, 1.02f, Math::sqrt(pr.randf()));
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 c = d * (rim_at(a) * MIN(rho, 1.0f));
		const float u = pr.randf();
		const float sz = Math::lerp(0.05f, 0.24f, u * u) * (0.7f + 0.3f * R / 2.0f);
		c.y = under(MIN(rho, 1.0f), c) + sz * 0.3f;
		const Basis b = Basis(Vector3(pr.randf(), pr.randf(), pr.randf()).normalized(), pr.randf() * 3.0f);
		const Vector3 sc(sz * Math::lerp(0.9f, 1.4f, pr.randf()), sz * Math::lerp(0.45f, 0.75f, pr.randf()), sz * Math::lerp(0.8f, 1.2f, pr.randf()));
		lump(c, b, sc, 0.6f, soil_colour(c, top(c) - c.y + 0.05f), 2.2f, 7);
	}
	// Crumbs: the soil of a torn face is crumb and small-block structure, not a smooth skin
	// (soil aggregates; a humus-rich topsoil crumbles into peas to walnuts). Thousands are far
	// below the pixel; a few hundred of the larger ones per plate carry the texture as geometry.
	const int crumbs = int(130.0f * R * (1.0f - 0.3f * rotten));
	for (int k = 0; k < crumbs; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::lerp(0.1f, 1.0f, Math::sqrt(pr.randf()));
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 c = d * (rim_at(a) * rho);
		const float sz = Math::lerp(0.012f, 0.045f, pr.randf() * pr.randf());
		c.y = under(rho, c) + sz * 0.35f;
		const Basis b = Basis(Vector3(pr.randf(), pr.randf(), pr.randf()).normalized(), pr.randf() * 3.0f);
		const Vector3 sc(sz * Math::lerp(0.8f, 1.3f, pr.randf()), sz * Math::lerp(0.6f, 0.9f, pr.randf()), sz * Math::lerp(0.8f, 1.2f, pr.randf()));
		lump(c, b, sc, 0.5f, soil_colour(c, top(c) - c.y + 0.03f * pr.randf()), 2.0f, 3);
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
			// Roots swell and pinch along their length where they grew round stones and
			// through denser soil, not the even taper of a shoot.
			const float swell = i == 0 ? 1.0f : 0.78f + 0.44f * moss_noise(p_local[i] * 9.0f, salt + 31u);
			rad.push_back(p_r[i] * swell);
		}
		const int sides = p_r[0] < 0.004f ? 3 : CLAMP(int(p_r[0] * 160.0f), 5, 12);
		Vector4i rp;
		r_bark.bone = 0;
		r_bark.begin(rp);
		_tube(r_bark, pts, rad, sides, 0.0f, root_c, nullptr, 0.0f, -1.0f, false);
		r_bark.end(rp, p_r[0]);
		// Soil crusted on in patches, the bark darker where it is still damp, paler rubbed bare.
		for (int v = rp.z; v < rp.z + rp.w; v++) {
			const Vector3 q = r_bark.verts[v];
			const float tone = 0.72f + 0.5f * moss_noise(q * 14.0f, salt + 32u);
			const float crust = Math::smoothstep(0.55f, 0.7f, moss_noise(q * 6.0f, salt + 33u));
			Color c = r_bark.colors[v];
			const float a = c.a;
			c = Color(c.r * tone, c.g * tone, c.b * tone).lerp(mineral * 0.9f, 0.7f * crust);
			c.a = a;
			r_bark.colors.write[v] = c;
		}
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
		float turn_rate = 0.0f;
		for (int i = 0; i <= steps; i++) {
			const float f = float(i) / steps;
			// Wandering, not zigzag: the turn itself drifts.
			turn_rate = 0.7f * turn_rate + 0.12f * (pr.randf() - 0.5f);
			a += turn_rate;
			const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
			const float rho = Math::lerp(0.6f * tr / MAX(rim_at(a), 0.1f), reach, f);
			Vector3 q = d * (rim_at(a) * rho);
			// Thick at the root crown under the stem, tapering fast (Coutts 1983, the zone of rapid
			// taper), then slowly to the rim.
			const float rr = r0 * (0.3f + 0.7f * Math::pow(1.0f - f, 2.0f)) + tr * 0.25f * MAX(0.0f, 1.0f - f * 4.0f);
			// Inside the plate near the stem, then along the underside mostly out of the soil
			// (washed and shaken bare), and past the rim hanging under their own weight.
			// Washed and shaken bare: out of the soil by most of their girth.
			const float floor_y = under(MIN(rho, 1.0f), q) - rr * 0.35f;
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
	// A mat of them: most of a tree's root length is under 2 mm thick (fine roots; Coutts 1983).
	const int fines = int(110.0f * R);
	for (int k = 0; k < fines; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::lerp(0.25f, 1.02f, Math::sqrt(pr.randf()));
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 o = d * (rim_at(a) * rho);
		o.y = under(MIN(rho, 1.0f), o) + 0.01f;
		const float u = pr.randf();
		const float len = Math::lerp(0.06f, 0.4f, pr.randf());
		const float r0 = Math::lerp(0.0015f, 0.01f, u * u);
		lp.clear();
		lr.clear();
		const Vector3 go = (d * Math::lerp(0.3f, 1.0f, pr.randf()) + Vector3(0, -1, 0)).normalized();
		Vector3 at = o;
		Vector3 head = go;
		for (int i = 0; i <= 6; i++) {
			const float f = float(i) / 6.0f;
			lp.push_back(at);
			lr.push_back(r0 * Math::lerp(1.0f, 0.4f, f));
			// Wiry and kinked: each piece turns off a little, and hangs as it goes.
			head = (head + Vector3(pr.randf() - 0.5f, pr.randf() - 0.5f, pr.randf() - 0.5f) * 0.9f + Vector3(0, -0.15f, 0)).normalized();
			at += head * (len / 6.0f);
		}
		root(lp, lr, false, 0, len * 0.5f);
	}
	// Torn stubs: roots that ran down and out into the soil below the plate and snapped where
	// it held them, standing out of the underside a hand or two (the plate's face reads as a
	// bristle of broken ends; Coutts 1983).
	const int stubs = int(14.0f * R);
	for (int k = 0; k < stubs; k++) {
		const float a = pr.randf() * float(Math::TAU);
		const float rho = Math::lerp(0.15f, 0.95f, Math::sqrt(pr.randf()));
		const Vector3 d(Math::cos(a), 0.0f, Math::sin(a));
		Vector3 o = d * (rim_at(a) * rho);
		o.y = under(rho, o) + 0.03f;
		const float u = pr.randf();
		const float r0 = Math::lerp(0.008f, 0.04f, u * u) * (0.6f + 0.4f * tr / 0.3f);
		const float len = Math::lerp(0.08f, 0.35f, pr.randf());
		const Vector3 go = (Vector3(0, -1, 0) + d * Math::lerp(0.2f, 0.9f, pr.randf()) + Vector3(pr.randf() - 0.5f, 0.0f, pr.randf() - 0.5f) * 0.5f).normalized();
		lp.clear();
		lr.clear();
		for (int i = 0; i <= 3; i++) {
			const float f = float(i) / 3.0f;
			lp.push_back(o + go * (len * f) + Vector3(pr.randf() - 0.5f, pr.randf() - 0.5f, pr.randf() - 0.5f) * (0.02f * f));
			lr.push_back(r0 * Math::lerp(1.0f, 0.8f, f));
		}
		root(lp, lr, true);
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
void TreeGenerator::_fracture(const Vector3 &p_end, const Vector3 &p_dir, float p_r, float p_old, uint32_t p_seed, const Vector3 &p_tension) {
	Builder &w = wood_b;
	RandomPCG sr;
	sr.seed(uint64_t(p_seed) * 6364136223846793005ull + 0x66726163u);
	const Vector3 g = p_dir.normalized();
	const Vector3 world_up(0, 1, 0);
	// The tension side: up for a limb that bent down under its weight; for a near-vertical stem,
	// downwind (here a random bearing).
	Vector3 td = world_up - g * world_up.dot(g);
	if (p_tension != Vector3()) {
		// Given: a stem a storm bent, torn on its windward side (the storm kit, section 5t).
		td = p_tension - g * p_tension.dot(g);
	} else if (td.length_squared() < 0.04f) {
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

// Green wood's modulus of rupture (MPa; Wood Handbook FPL-GTR-282, 2021, tables 5-3a/b, green;
// GALES for the European conifers: Norway spruce 35.5, Scots pine 39) and the root anchorage per
// kilogram of stem (N m/kg; Nicoll et al. 2006, Can. J. For. Res. 36:1871, free-draining mineral
// soil; deep or tap-rooted species higher, shallow rooters lower; shrubs seldom throw). Section 5t.
static const struct {
	float mor;
	float creg;
} STORM_WOOD[] = {
	{ 57.0f, 190.0f }, // oak
	{ 62.0f, 150.0f }, // beech
	{ 46.0f, 140.0f }, // birch
	{ 39.0f, 170.0f }, // Scots pine
	{ 35.5f, 140.0f }, // Norway spruce
	{ 50.0f, 200.0f }, // hazel
	{ 55.0f, 200.0f }, // hawthorn
	{ 34.0f, 160.0f }, // lime
	{ 55.0f, 160.0f }, // sycamore maple
	{ 66.0f, 160.0f }, // ash
	{ 35.0f, 150.0f }, // horse chestnut
	{ 45.0f, 150.0f }, // black alder
	{ 33.0f, 140.0f }, // white willow
	{ 53.0f, 170.0f }, // European larch
	{ 38.0f, 160.0f }, // silver fir
	{ 50.0f, 170.0f }, // rowan
	{ 35.0f, 200.0f }, // elder
	{ 52.0f, 180.0f }, // holly
	{ 60.0f, 200.0f }, // yew
	{ 70.0f, 200.0f }, // box
	{ 65.0f, 170.0f }, // sugar maple
	{ 53.0f, 150.0f }, // red maple
	{ 57.0f, 170.0f }, // red oak
	{ 57.0f, 190.0f }, // white oak
	{ 34.0f, 160.0f }, // white pine
	{ 44.0f, 130.0f }, // hemlock
	{ 59.0f, 150.0f }, // American beech
	{ 44.0f, 140.0f }, // paper birch
	{ 57.0f, 150.0f }, // yellow birch
	{ 50.0f, 200.0f }, // witch-hazel
	{ 50.0f, 200.0f }, // mountain laurel
	{ 45.0f, 200.0f }, // gorse
	{ 40.0f, 200.0f }, // broom
	{ 35.0f, 200.0f }, // gum rockrose
	{ 60.0f, 200.0f }, // kermes oak (dense oak wood)
	{ 50.0f, 200.0f }, // lentisk
	{ 30.0f, 300.0f }, // dwarf fan palm (fibrous, flexible; it bends rather than breaks)
	{ 40.0f, 210.0f }, // stone pine (deep taproot on sand)
	{ 55.0f, 200.0f }, // cork oak (dense oak wood, deep roots)
	{ 60.0f, 200.0f }, // holm oak (very hard, dense wood)
	{ 60.0f, 220.0f }, // wild olive (very dense wood, deep roots)
	{ 40.0f, 190.0f }, // maritime pine (deep taproot, but tall and exposed)
	{ 50.0f, 200.0f }, // carob
	{ 55.0f, 200.0f }, // blackthorn
	{ 50.0f, 190.0f }, // field maple
	{ 55.0f, 200.0f }, // dog rose
};
static_assert(sizeof(STORM_WOOD) / sizeof(STORM_WOOD[0]) == TreeGenerator::PRESET_MAX, "one storm row per preset");

Ref<StandardMaterial3D> TreeGenerator::_soil_material() const {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_albedo(Color(1, 1, 1));
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
	m->set_snow_from_weather(true);
	m->set_roughness(0.95f);
	return m;
}

// Broken wood (trees.md 5j): fracture faces and splinters, vertex colour by how fresh the break
// is, a texture of fibres along v with their relief.
Ref<StandardMaterial3D> TreeGenerator::_wood_material() {
	Ref<StandardMaterial3D> m = _soil_material();
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _wood_texture(false));
	m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, _wood_texture(true));
	m->set_normal_scale(0.8f);
	m->set_roughness(0.85f);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	return m;
}

// A builder's triangles as a storm kit entry [name, surface arrays, material] (section 5t),
// relief tubes in place of the plain ones they cover (as LOD 0 draws them).
void TreeGenerator::_storm_pack(Builder &b, bool p_flip, const String &p_name, const Ref<Material> &p_m, Array &r_out) {
	if (b.indices.is_empty()) {
		return;
	}
	if (p_flip) {
		int32_t *iw = b.indices.ptrw();
		for (int64_t i = 0; i + 2 < b.indices.size(); i += 3) {
			SWAP(iw[i + 1], iw[i + 2]);
		}
	}
	PackedInt32Array idx;
	for (uint32_t i = 0; i < b.parts.size(); i++) {
		if (b.part_role[i] == Builder::ROLE_PLAIN) {
			continue;
		}
		const Vector4i &pt = b.parts[i];
		for (int k = pt.x; k < pt.x + pt.y; k++) {
			idx.push_back(b.indices[k]);
		}
	}
	if (idx.is_empty()) {
		return;
	}
	Array a;
	a.resize(Mesh::ARRAY_MAX);
	a[Mesh::ARRAY_VERTEX] = b.verts;
	a[Mesh::ARRAY_NORMAL] = b.normals;
	a[Mesh::ARRAY_TEX_UV] = b.uvs;
	a[Mesh::ARRAY_COLOR] = b.colors;
	a[Mesh::ARRAY_INDEX] = idx;
	if (b.use_tangents && b.tangents.size() == b.verts.size() * 4) {
		a[Mesh::ARRAY_TANGENT] = b.tangents;
	}
	Array entry;
	entry.push_back(p_name);
	entry.push_back(a);
	entry.push_back(p_m);
	r_out.push_back(entry);
}

// What TreeWind3D needs to break a standing rigged tree in a storm or throw it (section 5t): the
// wood's strength and the roots' anchorage (mesh meta "tree_storm": modulus of rupture in Pa,
// anchorage in N m per kg of stem, plate radius, plate thickness, stem radius, seed, bark colour)
// and a fracture cap of unit radius along +Y torn on its +X side ("tree_fracture_cap", as [name,
// surface arrays, material] triples). The root plate (some 80-100k triangles) is built only when a
// tree is thrown (build_storm_plate()).
void TreeGenerator::_storm_kit(const Ref<ArrayMesh> &p_mesh, const Array &p_static_materials) {
	if (nodes.is_empty() || p_mesh.is_null()) {
		return;
	}
	RandomPCG kr;
	kr.seed(uint64_t(seed) * 2862933555777941757ull + 0x73746f726dull);
	float trunk_r = 0.02f;
	for (const Node &n : nodes) {
		if (!n.removed && n.parent == 0) {
			trunk_r = MAX(trunk_r, n.radius);
		}
	}
	// The plate a standing tree throws (5k, Krisans et al. 2020): radius about 11 stem radii,
	// thickness about a fifth of that; a small tree's down to half a metre, an old one's up to 3.6 m (7 m across).
	const float plate_radius = CLAMP(11.0f * trunk_r * Math::lerp(0.9f, 1.2f, kr.randf()), 0.5f, 3.6f);
	const float plate_thick = CLAMP(0.22f * plate_radius, 0.15f, 0.6f);
	const Builder saved_wood = wood_b;
	wood_b = Builder();
	_fracture(Vector3(), Vector3(0, 1, 0), 1.0f, 0.0f, uint32_t(seed) * 2654435761u + 77u, Vector3(1, 0, 0));
	Builder cap = wood_b;
	wood_b = saved_wood;
	Array cap_kit;
	_storm_pack(cap, false, "wood", _wood_material(), cap_kit);
	const int p = CLAMP(int(preset), 0, int(PRESET_MAX) - 1);
	const Color bc = bark_color;
	p_mesh->set_meta(SNAME("tree_storm"), PackedFloat32Array({ STORM_WOOD[p].mor * 1.0e6f, STORM_WOOD[p].creg, plate_radius, plate_thick, trunk_r, float(seed % 65536), bc.r, bc.g, bc.b }));
	p_mesh->set_meta(SNAME("tree_fracture_cap"), cap_kit);
}

// The root plate a storm throws with a tree (5k, 5t), built upright about the tree's foot from its
// "tree_storm" meta: [name, surface arrays, material] triples for its roots ("bark", no material:
// the tree's own), soil and torn root ends ("wood").
Array TreeGenerator::build_storm_plate(const PackedFloat32Array &p_storm) {
	Array out;
	if (p_storm.size() < 9) {
		return out;
	}
	Ref<TreeGenerator> g;
	g.instantiate();
	g->seed = int(p_storm[5]);
	g->bark_color = Color(p_storm[6], p_storm[7], p_storm[8]);
	g->plate = RootPlate();
	g->plate.on = true;
	g->plate.radius = p_storm[2];
	g->plate.thick = p_storm[3];
	g->plate.trunk_r = p_storm[4];
	g->plate.base0 = Vector3();
	g->plate.hinge = Vector3();
	g->plate.rot = Basis();
	g->soil_b = Builder();
	g->wood_b = Builder();
	Builder roots;
	roots.use_tangents = true;
	g->_root_plate(roots);
	// Roots carry no lichen and no lost bark (vertex alpha 0.5 to the bark's lichen shader).
	for (int v = 0; v < roots.colors.size(); v++) {
		roots.colors.write[v].a = 0.5f;
	}
	_storm_pack(roots, true, "bark", Ref<Material>(), out);
	_storm_pack(g->soil_b, true, "soil", g->_soil_material(), out);
	_storm_pack(g->wood_b, false, "wood", g->_wood_material(), out);
	return out;
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
// Gorse: a flush from February to May, a few flowers all year (Trees and Shrubs Online: almost
// year-round, spring peak). Broom: May to June (Bean). Trees.md 5u.
float TreeGenerator::get_bloom(Preset p_preset, float p_day) {
	auto ramp = [&](float p_a, float p_b, float p_c, float p_d) {
		if (p_day <= p_a || p_day >= p_d) {
			return 0.0f;
		}
		return p_day < p_b ? (p_day - p_a) / (p_b - p_a) : (p_day <= p_c ? 1.0f : (p_d - p_day) / (p_d - p_c));
	};
	switch (p_preset) {
		case PRESET_GORSE:
			return MAX(ramp(20.0f, 60.0f, 120.0f, 160.0f), 0.04f);
		case PRESET_BROOM:
			return ramp(120.0f, 136.0f, 160.0f, 178.0f);
		case PRESET_GUM_ROCKROSE:
			// April to June, peaking late April and May (Flora iberica); each flower lasts a day,
			// so a shrub in flower shows a share of its buds open at once.
			return ramp(90.0f, 110.0f, 145.0f, 175.0f);
		case PRESET_DWARF_FAN_PALM:
			// March to June, peaking in April (Flora iberica; Herrera 1989).
			return ramp(65.0f, 90.0f, 130.0f, 170.0f);
		// Hedge shrubs (trees.md 5ad; lowland Britain and France): blackthorn on the bare wood in
		// March and April, hawthorn's May blossom, dog rose in June.
		case PRESET_BLACKTHORN:
			return ramp(70.0f, 85.0f, 105.0f, 120.0f);
		case PRESET_HAWTHORN:
			return ramp(118.0f, 128.0f, 145.0f, 158.0f);
		case PRESET_DOG_ROSE:
			return ramp(150.0f, 162.0f, 180.0f, 196.0f);
		default:
			return 0.0f;
	}
}

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
	// Face normals, averaged, for a finished part (all parts are seen from both sides). Godot's
	// front faces wind clockwise, so a face's normal is (c - a) x (b - a): the right-handed one
	// pointed to the back, and with double-sided shading every crown part was lit from behind
	// in raster (white rockrose flowers drew grey, gorse flowers flat). Trees.md 5v.
	auto close_part = [&](Vector4i &p_part, float p_size) {
		Vector3 *nw = r_b.normals.ptrw();
		const Vector3 *vw = r_b.verts.ptr();
		for (int64_t i = p_part.x; i + 2 < r_b.indices.size(); i += 3) {
			const int a = r_b.indices[i], b = r_b.indices[i + 1], c = r_b.indices[i + 2];
			const Vector3 fn = (vw[c] - vw[a]).cross(vw[b] - vw[a]);
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
		// Gorse and broom flowers and pods (trees.md 5u), kept by the coarser LODs: a heath in
		// flower is gold from far off.
		if (fruit_cover > 0.0f && (preset == PRESET_GORSE || preset == PRESET_BROOM) && !tips.is_empty()) {
			const bool gorse = preset == PRESET_GORSE;
			// How much of the crop is open on this day. Gorse: a flush from February to May, a few
			// flowers all year (TSO: almost year-round, spring peak). Broom: May to June (Bean).
			auto ramp = [&](float p_a, float p_b, float p_c, float p_d) {
				if (day <= p_a || day >= p_d) {
					return 0.0f;
				}
				return day < p_b ? (day - p_a) / (p_b - p_a) : (day <= p_c ? 1.0f : (p_d - day) / (p_d - p_c));
			};
			const float bloom = get_bloom(preset, day); // No season: 1 July.
			// Pods: gorse's from June, bursting in the July heat; broom's black by August, the
			// valves twisting open and hanging into autumn.
			const float pods = gorse ? ramp(140.0f, 170.0f, 215.0f, 250.0f) : ramp(165.0f, 190.0f, 240.0f, 290.0f);
			const float ripe = gorse ? CLAMP((day - 175.0f) / 25.0f, 0.0f, 1.0f) : CLAMP((day - 190.0f) / 20.0f, 0.0f, 1.0f);
			const float split = gorse ? 0.0f : CLAMP((day - 220.0f) / 25.0f, 0.0f, 1.0f);
			// Corolla 13-20 mm (gorse) and 16-25 mm (broom); calyx nearly as long (gorse, hairy,
			// yellow-green) or a quarter (broom, glabrous, green). Pods 10-20 mm, villous (gorse);
			// 15-60 mm, black, hairy on the margins (broom). Webb et al. 1988.
			const float L = gorse ? 0.017f : 0.02f;
			// Egg-yolk to golden yellow (gorse deeper, a little orange in the banner; broom a
			// clear golden yellow). Paler values read as a dull ochre once shaded.
			const Color petal_c = gorse ? lin(1.0f, 0.86f, 0.06f) : lin(1.0f, 0.88f, 0.1f);
			const Color banner_c = gorse ? lin(1.0f, 0.8f, 0.0f) : lin(1.0f, 0.84f, 0.04f);
			const Color keel_c = petal_c.lerp(lin(1.0f, 0.92f, 0.3f), 0.3f);
			// A petal or calyx lip: three stations of three points, folded along the midline by
			// p_fold (the edges rise off the midrib, so the petal is a V, not a flat card) and
			// curled back at the tip by p_curl (eight triangles).
			auto petal = [&](const Vector3 &p_at, const Vector3 &p_dir, const Vector3 &p_width, float p_len, float p_w, float p_fold, const Color &p_c0, const Color &p_c1, float p_curl = 0.0f) {
				const Vector3 nrm = p_dir.cross(p_width).normalized();
				static const float prof[3] = { 0.35f, 1.0f, 0.55f };
				static const float ts[3] = { 0.0f, 0.55f, 1.0f };
				const int v0 = r_b.verts.size();
				for (int k = 0; k < 3; k++) {
					const Vector3 c = p_at + p_dir * (p_len * ts[k]) + nrm * (p_curl * p_len * ts[k] * ts[k]);
					const float w = p_w * prof[k] * 0.5f;
					const Color col = p_c0.lerp(p_c1, ts[k]);
					vert(c - p_width * w + nrm * (p_fold * w), Vector2(0.0f, ts[k]), col);
					vert(c, Vector2(0.5f, ts[k]), col * 0.92f);
					vert(c + p_width * w + nrm * (p_fold * w), Vector2(1.0f, ts[k]), col);
				}
				for (int k = 0; k < 2; k++) {
					const int q = v0 + k * 3;
					for (int e = 0; e < 2; e++) {
						tri(q + e, q + 3 + e, q + e + 1);
						tri(q + e + 1, q + 3 + e, q + 4 + e);
					}
				}
			};
			const Color calyx_c = gorse ? lin(0.8f, 0.72f, 0.22f) : lin(0.32f, 0.44f, 0.16f);
			const Color stalk_c = gorse ? lin(0.38f, 0.42f, 0.24f) : lin(0.3f, 0.42f, 0.16f);
			const Color pod_green = gorse ? lin(0.4f, 0.42f, 0.28f) : lin(0.3f, 0.4f, 0.14f);
			const Color pod_black = gorse ? lin(0.13f, 0.1f, 0.08f) : lin(0.07f, 0.06f, 0.05f);
			// Flowers along the last shoots: in gorse's spine axils, in broom's leaf joints on
			// last year's switches. One internode stands for a few of the real nodes.
			const int per_node = gorse ? 28 : 5;
			const int depth = gorse ? 2 : 3;
			const float budget = (gorse ? 60000.0f : 8000.0f) * fruit_cover;
			const float want = float(tips.size() * per_node * depth);
			const float p_flower = want > 0.0f ? MIN(1.0f, budget / want) : 0.0f;
			static const float valve_prof[5] = { 0.3f, 0.9f, 1.0f, 0.9f, 0.3f };
			// One part per flower or pod, sized negative: the coarser LODs keep a stochastic subset,
			// each survivor grown about its base to keep the gold's area (as leaves, trees.md 5x.2).
			// A mature gorse carries some 25 000 of them; drawn whole at every distance they were
			// 1.7 million triangles.
			Vector4i part;
			uint32_t slot = 0;
			for (const Tip &t : tips) {
				int node = t.node;
				for (int lvl = 0; lvl < depth && node > 0; lvl++, node = nodes[node].parent) {
					const Node &n = nodes[node];
					if (n.removed || n.dead || n.radius > 0.012f) {
						break;
					}
					r_b.bone = bone_of(node);
					const Vector3 d = n.dir;
					const Vector3 s0 = perp(d);
					for (int q = 0; q < per_node; q++) {
						const uint32_t h = hash_murmur3_one_32(slot++, uint32_t(seed) * 2654435761u + 0x70656aU);
						const float hu = float(h & 0xffff) / 65535.0f;
						if (hu >= p_flower) {
							continue;
						}
						const float hv = float((h >> 16) & 0xffff) / 65535.0f;
						const bool is_flower = hv < bloom;
						const bool is_pod = !is_flower && hv < bloom + pods * 0.45f;
						if (!is_flower && !is_pod) {
							continue;
						}
						const float along = (q + 0.5f) / per_node;
						const float roll = float(q) * 2.39996f + hu * 40.0f;
						const Vector3 radial = s0.rotated(d, roll);
						const Vector3 at = n.start + d * (n.length * along) + radial * n.radius;
						const float jit = Math::lerp(0.85f, 1.15f, float((h >> 4) & 255u) / 255.0f);
						r_b.begin(part);
						if (is_flower) {
							// Out from the shoot and a little forward; the banner on its upper side.
							// Each turned its own way (a cluster of flowers along a shoot, not a row of
							// cards facing alike).
							const float tw = float((h >> 12) & 255u) / 255.0f - 0.5f;
							const Vector3 a = (radial * 0.8f + d * (0.45f + 0.3f * tw) + Vector3(0, 0.15f, 0)).normalized();
							Vector3 u = Vector3(0, 1, 0) - a * a.y;
							u = u.length_squared() > 1e-4f ? u.normalized().rotated(a, tw * 1.4f) : perp(a);
							const Vector3 sd = a.cross(u).normalized();
							const float l = L * jit;
							// Each flower its own brightness, and its throat shaded: the base of each
							// petal sits in the calyx and its neighbours' shadow, which a shadow map
							// cannot resolve at a centimetre (the path tracer finds it anyway).
							const float fb = Math::lerp(0.82f, 1.08f, float((h >> 20) & 255u) / 255.0f);
							const Color pc = petal_c * fb, bc = banner_c * fb, kc = keel_c * fb;
							const Vector3 c0 = at + a * (gorse ? 0.05f * l : 0.3f * l);
							if (!gorse) {
								stalk(at, c0, 0.0007f, stalk_c);
							}
							// Calyx: two lips, hairy and nearly the corolla's length in gorse, a short cup in
							// broom; each lip a small folded blade.
							const float cl = gorse ? 0.7f * l : 0.28f * l;
							petal(c0, (a + u * 0.06f).normalized(), sd, cl, 0.24f * l, 0.7f, calyx_c * 0.85f, calyx_c);
							petal(c0, (a - u * 0.06f).normalized(), sd, cl * 0.95f, 0.22f * l, -0.7f, calyx_c * 0.85f, calyx_c);
							const Vector3 k0 = c0 + a * (gorse ? 0.25f * l : 0.1f * l);
							// Wings and keel: a boat along the flower's axis, closed over the stamens.
							for (int side = -1; side <= 1; side += 2) {
								const Vector3 wd = (u * -0.75f + sd * (0.65f * side)).normalized();
								petal(k0 + sd * (0.04f * l * side), (a - u * 0.08f).normalized(), wd, 0.85f * l, 0.4f * l, -0.6f * side, kc * 0.6f, pc, 0.1f * side);
							}
							// The banner: folded along its midline and its tip turned back.
							petal(k0 + u * (0.08f * l), (u * 0.85f + a * 0.55f).normalized(), sd, 0.9f * l, 0.7f * l, 0.65f, bc * 0.6f, bc, -0.25f);
						} else if (split > 0.0f && float((h >> 8) & 255u) / 255.0f < split) {
							// A burst broom pod: two black valves twisted into spirals.
							const Vector3 a = (radial * 0.5f + d * 0.6f + Vector3(0, -0.2f, 0)).normalized();
							const float pl = 0.04f * jit;
							const Vector3 w = perp(a);
							blade(at, a, w, pl, 0.009f, valve_prof, 3.5f, 0.15f, pod_black, pod_black * 1.3f);
							blade(at, (a + w * 0.4f).normalized(), w.rotated(a, 1.2f), pl * 0.95f, 0.009f, valve_prof, -3.2f, 0.15f, pod_black, pod_black * 1.3f);
						} else {
							// A closed pod: a flattened lens, hairy, green then black.
							const Vector3 a = gorse ? (radial * 0.7f + d * 0.6f).normalized() : (radial * 0.45f + d * 0.75f + Vector3(0, -0.15f, 0)).normalized();
							const float pl = (gorse ? 0.016f : 0.04f) * jit;
							const float pw = gorse ? 0.0032f : 0.0045f, pt = gorse ? 0.0024f : 0.0014f;
							const Color pc = pod_green.lerp(pod_black, ripe);
							const Vector3 w = perp(a);
							const Vector3 th = a.cross(w);
							const int v0 = r_b.verts.size();
							const int rings = 6, sides = 6;
							for (int i = 0; i < rings; i++) {
								const float tt = float(i) / (rings - 1);
								const float pr = Math::pow(Math::sin(float(Math::PI) * (0.08f + 0.84f * tt)), 0.6f);
								const Vector3 c = at + a * (pl * tt);
								for (int k = 0; k < sides; k++) {
									const float an = float(Math::TAU) * k / sides;
									vert(c + w * (Math::cos(an) * pw * pr) + th * (Math::sin(an) * pt * pr), Vector2(float(k) / sides, tt), pc * Math::lerp(0.85f, 1.1f, float(k & 1)));
								}
							}
							for (int i = 0; i + 1 < rings; i++) {
								for (int k = 0; k < sides; k++) {
									const int k1 = (k + 1) % sides;
									tri(v0 + i * sides + k, v0 + i * sides + k1, v0 + (i + 1) * sides + k);
									tri(v0 + i * sides + k1, v0 + (i + 1) * sides + k1, v0 + (i + 1) * sides + k);
								}
							}
						}
						close_part(part, -0.02f);
					}
				}
			}
		}
		// Gum rockrose flowers and capsules (trees.md 5v), kept by the coarser LODs: white flowers
		// 5-10 cm across are seen from far off. One flower at the tip of a short side shoot, five
		// broad, crumpled petals (the crinkled silk is geometry: each petal a 4 x 5 grid lifted by
		// a hash), in most plants a maroon blotch at each petal's base, a boss of many yellow
		// stamens; three sepals behind. The capsule after: a woody, ten-valved globe 1-1.5 cm on the
		// tip, green, then brown, then grey and split, standing into winter (Flora iberica).
		if (fruit_cover > 0.0f && preset == PRESET_GUM_ROCKROSE && !tips.is_empty()) {
			const float bloom = get_bloom(preset, day);
			const float caps = day <= 120.0f ? CLAMP((60.0f - day) / 60.0f, 0.0f, 1.0f) * 0.4f : (day < 170.0f ? (day - 120.0f) / 50.0f : 1.0f);
			const float dry = CLAMP((day - 170.0f) / 60.0f, 0.0f, 1.0f);
			// Most plants carry the blotch (var. maculatus); some are plain white (var. albiflorus).
			const bool blotched = (uint32_t(seed) * 2654435761u >> 20) % 10 < 7;
			const Color white = lin(0.97f, 0.96f, 0.93f), shade = lin(0.88f, 0.87f, 0.84f);
			const Color blotch = lin(0.32f, 0.02f, 0.08f), base_y = lin(0.98f, 0.86f, 0.4f);
			const Color stamen_c = lin(0.98f, 0.78f, 0.12f), sepal_c = lin(0.42f, 0.48f, 0.22f);
			const Color cap_green = lin(0.38f, 0.42f, 0.2f), cap_brown = lin(0.42f, 0.3f, 0.18f), cap_grey = lin(0.5f, 0.47f, 0.42f);
			Vector4i part;
			r_b.begin(part);
			int bone_now = -1;
			uint32_t slot = 0;
			for (const Tip &t : tips) {
				const Node &n = nodes[t.node];
				if (n.removed || n.dead) {
					continue;
				}
				const uint32_t h = hash_murmur3_one_32(slot++, uint32_t(seed) * 2654435761u + 0x63697374u);
				const float hu = float(h & 0xffff) / 65535.0f;
				// Each flower opens for a day and drops its petals (Flora iberica), so a bush in
				// flower carries buds, open flowers and spent ones together: about a fifth of the
				// tips open at the peak, as many in bud (more early in the season), some spent.
				const float early = CLAMP((125.0f - day) / 35.0f, 0.0f, 1.0f);
				const float p_open = 0.22f * bloom * fruit_cover;
				const float p_bud = p_open + (0.12f + 0.18f * early) * bloom * fruit_cover;
				const float p_spent = p_bud + (0.18f - 0.12f * early) * bloom * fruit_cover;
				const bool is_flower = hu < p_open;
				const bool is_bud = !is_flower && hu < p_bud;
				const bool is_spent = !is_flower && !is_bud && hu < p_spent;
				const bool is_cap = !is_flower && !is_bud && !is_spent && hu < p_spent + 0.5f * caps * fruit_cover;
				if (!is_flower && !is_bud && !is_spent && !is_cap) {
					continue;
				}
				const int b = bone_of(t.node);
				if (b != bone_now) {
					if (bone_now >= 0) {
						close_part(part, 0.1f);
						r_b.begin(part);
					}
					r_b.bone = b;
					bone_now = b;
				}
				const Vector3 d = n.dir;
				const Vector3 at = n.end();
				const float jit = Math::lerp(0.85f, 1.15f, float((h >> 16) & 255u) / 255.0f);
				// Facing: up and out over the shoot's last leaves, each tilted its own way (up to
				// about 40 degrees), on a stalk of a few centimetres (the flowering side shoot).
				const float ta = float((h >> 24) & 255u) / 255.0f * float(Math::TAU);
				const float tt0 = float((h >> 8) & 255u) / 255.0f;
				const Vector3 tilt_axis = perp(Vector3(0, 1, 0)).rotated(Vector3(0, 1, 0), ta);
				const Vector3 ax = (d * 0.6f + Vector3(0, 0.8f, 0)).normalized().rotated(tilt_axis, 0.7f * tt0 - 0.15f).normalized();
				const Vector3 c0 = at + ax * 0.045f;
				const Vector3 s0 = perp(ax);
				auto sepals = [&](float p_r, float p_up) {
					for (int q = 0; q < 3; q++) {
						const Vector3 sd = s0.rotated(ax, q * 2.0944f + 0.5f);
						const Vector3 sdir = (sd * (0.8f - p_up) + ax * (p_up - 0.35f)).normalized();
						const Vector3 sw = ax.cross(sdir).normalized();
						const int v0 = r_b.verts.size();
						for (int i = 0; i < 3; i++) {
							const float tt = float(i) / 2.0f;
							const float w = p_r * 0.32f * Math::sin(float(Math::PI) * (0.15f + 0.7f * tt));
							const Vector3 c = c0 + sdir * (p_r * 0.45f * tt);
							vert(c - sw * w, Vector2(0.0f, tt), sepal_c);
							vert(c + sw * w, Vector2(1.0f, tt), sepal_c);
						}
						for (int k = 0; k < 2; k++) {
							tri(v0 + k * 2, v0 + k * 2 + 2, v0 + k * 2 + 1);
							tri(v0 + k * 2 + 1, v0 + k * 2 + 2, v0 + k * 2 + 3);
						}
					}
				};
				if (is_bud) {
					// A bud: an ovoid 1-1.5 cm, the three sepals closed round it, the white of the
					// folded petals showing at the tip just before it opens.
					stalk(at, c0, 0.0014f, sepal_c);
					const float br = 0.006f * jit, bl = 0.016f * jit;
					const bool late = tt0 > 0.6f;
					const Vector3 s1 = ax.cross(s0);
					const int rings = 5, sides = 9;
					const int v0 = r_b.verts.size();
					for (int i = 0; i <= rings; i++) {
						const float tt = float(i) / rings;
						const float rr = br * Math::pow(Math::sin(float(Math::PI) * (0.08f + 0.92f * tt)), 0.7f);
						const Color cc = late && tt > 0.7f ? white : sepal_c * (0.85f + 0.15f * tt);
						for (int k = 0; k < sides; k++) {
							const float ph = float(Math::TAU) * k / sides;
							vert(c0 + ax * (bl * tt) + (s0 * Math::cos(ph) + s1 * Math::sin(ph)) * MAX(rr, 0.0004f), Vector2(), cc * ((k % 3) == 0 ? 0.85f : 1.0f));
						}
					}
					for (int i = 0; i < rings; i++) {
						for (int k = 0; k < sides; k++) {
							const int k1 = (k + 1) % sides;
							tri(v0 + i * sides + k, v0 + (i + 1) * sides + k, v0 + i * sides + k1);
							tri(v0 + i * sides + k1, v0 + (i + 1) * sides + k, v0 + (i + 1) * sides + k1);
						}
					}
				} else if (is_spent) {
					// Spent: the petals fallen, the sepals clasp the swelling green ovary and a few
					// dry stamens.
					stalk(at, c0, 0.0014f, sepal_c);
					sepals(0.02f * jit, 0.55f);
					const Vector3 s1 = ax.cross(s0);
					const float cr = 0.0045f * jit;
					const int v0 = r_b.verts.size();
					for (int i = 0; i <= 4; i++) {
						const float th = float(Math::PI) * float(i) / 4.0f;
						for (int k = 0; k < 8; k++) {
							const float ph = float(Math::TAU) * k / 8.0f;
							vert(c0 + ax * (cr * (1.0f - Math::cos(th))) + (s0 * Math::cos(ph) + s1 * Math::sin(ph)) * (Math::sin(th) * cr), Vector2(), cap_green);
						}
					}
					for (int i = 0; i < 4; i++) {
						for (int k = 0; k < 8; k++) {
							const int k1 = (k + 1) % 8;
							tri(v0 + i * 8 + k, v0 + (i + 1) * 8 + k, v0 + i * 8 + k1);
							tri(v0 + i * 8 + k1, v0 + (i + 1) * 8 + k, v0 + (i + 1) * 8 + k1);
						}
					}
				} else if (is_flower) {
					stalk(at, c0, 0.0014f, sepal_c);
					// 5-10 cm across: petals 2.5-5 cm.
					const float R = 0.036f * Math::lerp(0.75f, 1.25f, float((h >> 4) & 255u) / 255.0f);
					sepals(R, 0.0f);
					const int RN = 6, CN = 7; // Rows out from the centre, columns across.
					for (int q = 0; q < 5; q++) {
						// Five separate petals: each on a narrow base, broad and rounded at the tip,
						// turned a little on its own axis so neighbours overlap unevenly.
						const Vector3 pd0 = s0.rotated(ax, q * 1.25664f + 0.25f * (float((h >> (q * 3)) & 7u) / 7.0f - 0.5f));
						const Vector3 pd = pd0.rotated(ax.cross(pd0).normalized(), 0.0f);
						const float twist = 0.25f * (float((h >> (q * 5 + 2)) & 7u) / 7.0f - 0.5f);
						const Vector3 pw0 = ax.cross(pd).normalized();
						const Vector3 pup = ax.rotated(pd, twist);
						const Vector3 pw = pw0.rotated(pd, twist);
						const float pl = R * Math::lerp(0.9f, 1.08f, float((h >> (q * 4 + 1)) & 15u) / 15.0f);
						const int v0 = r_b.verts.size();
						for (int i = 0; i < RN; i++) {
							const float tt = float(i) / (RN - 1);
							// Obovate on a short claw: narrow at the centre, widest at three quarters.
							const float half = pl * (0.07f + 0.5f * Math::pow(Math::sin(float(Math::PI) * 0.62f * tt), 1.4f));
							// Cupped: the petal rises from the centre and its outer half lifts.
							const Vector3 c = c0 + pd * (pl * tt) + pup * (pl * (0.06f + 0.32f * tt * tt));
							for (int k = 0; k < CN; k++) {
								const float u = float(k) / (CN - 1) * 2.0f - 1.0f;
								const uint32_t hk = hash_murmur3_one_32(uint32_t(i * 16 + k + q * 97), h);
								// Crumpled silk: lifts and folds growing to the edge, the margin waved.
								const float crumple = (float(hk & 255u) / 255.0f - 0.5f) * pl * 0.2f * (0.2f + tt) * (0.3f + Math::abs(u)) + pl * 0.05f * Math::sin(u * 7.0f + tt * 9.0f + q) * tt * Math::abs(u);
								// The edges of the petal curl up a little more than its middle.
								const float cup = pl * 0.1f * u * u * tt;
								Color col = white.lerp(shade, float((hk >> 8) & 255u) / 255.0f * 0.5f);
								if (tt < 0.1f) {
									col = base_y;
								} else if (blotched && tt > 0.15f && tt < 0.42f && Math::abs(u) < 0.5f) {
									// A spot apart on each petal: white margins and a pale band above
									// the yellow, so the five read as five at a metre, not a ring.
									col = blotch.lerp(blotch * 0.7f, float((hk >> 16) & 255u) / 255.0f);
								}
								vert(c + pw * (u * half) + pup * (crumple + cup), Vector2(0.5f + 0.5f * u, tt), col);
							}
						}
						for (int i = 0; i + 1 < RN; i++) {
							for (int k = 0; k + 1 < CN; k++) {
								const int a = v0 + i * CN + k;
								tri(a, a + CN, a + 1);
								tri(a + 1, a + CN, a + CN + 1);
							}
						}
					}
					// The stamens: a dense ring of yellow, as a low crown of twelve points round the
					// green ovary.
					const int v0 = r_b.verts.size();
					const float sr = R * 0.24f;
					const int ctr = vert(c0 + ax * (R * 0.1f), Vector2(0.5f, 0.5f), sepal_c);
					for (int k = 0; k < 24; k++) {
						const Vector3 rd = s0.rotated(ax, k * 0.2618f);
						const float r = (k & 1) ? sr : sr * 0.55f;
						vert(c0 + rd * r + ax * ((k & 1) ? R * 0.16f : R * 0.05f), Vector2(0.5f, 0.5f), stamen_c);
					}
					for (int k = 0; k < 24; k++) {
						tri(ctr, v0 + 1 + k, v0 + 1 + (k + 1) % 24);
					}
				} else {
					// The capsule: a globe of ten valves (ridged), on a short stalk; splitting from the
					// top once dry.
					const Vector3 ax = (d * 0.7f + Vector3(0, 0.6f, 0)).normalized();
					const float cr = 0.0065f * jit;
					const Vector3 c = at + ax * (0.01f + cr);
					stalk(at, c - ax * cr, 0.0011f, cap_green.lerp(cap_brown, dry));
					const Color cc = dry < 0.5f ? cap_green.lerp(cap_brown, dry * 2.0f) : cap_brown.lerp(cap_grey, dry * 2.0f - 1.0f);
					const Vector3 s0 = perp(ax), s1 = ax.cross(s0);
					const int rings = 6, sides = 20;
					const int v0 = r_b.verts.size();
					for (int i = 0; i <= rings; i++) {
						const float th = float(Math::PI) * float(i) / rings;
						for (int k = 0; k < sides; k++) {
							const float ph = float(Math::TAU) * k / sides;
							const float ridge = (k & 1) ? 1.0f : 0.9f;
							// Split valves spread at the top.
							const float open = dry > 0.7f && i < 2 ? 1.0f + 0.35f * (2 - i) * (k & 1) : 1.0f;
							const Vector3 rr = (s0 * Math::cos(ph) + s1 * Math::sin(ph)) * (Math::sin(th) * cr * ridge * open);
							vert(c + rr + ax * (Math::cos(th) * cr * 0.95f), Vector2(float(k) / sides, float(i) / rings), cc * ((k & 1) ? 1.0f : 0.8f));
						}
					}
					for (int i = 0; i < rings; i++) {
						for (int k = 0; k < sides; k++) {
							const int k1 = (k + 1) % sides;
							tri(v0 + i * sides + k, v0 + (i + 1) * sides + k, v0 + i * sides + k1);
							tri(v0 + i * sides + k1, v0 + (i + 1) * sides + k, v0 + (i + 1) * sides + k1);
						}
					}
				}
			}
			close_part(part, 0.1f);
		}
		// Dwarf fan palm flowers and dates (trees.md 5w; Flora iberica vol. 18; Herrera 1989).
		// Dioecious: short, dense, branched inflorescences come out between the leaf bases,
		// sheathed at the foot in a flattened bract, March to June: male plants' a crowd of
		// small bright yellow flowers, female plants' fewer, greener ones. Female plants then
		// set dates, globose to oblong drupes 1.5-4 cm in dense clusters on the short stalks
		// that bow under them: green through summer, yellow, then orange-brown to red-brown
		// when ripe, September to November, a few hanging on into winter.
		if (fruit_cover > 0.0f && preset == PRESET_DWARF_FAN_PALM && !palm_crowns.is_empty()) {
			const float bloom = get_bloom(preset, day);
			const bool male = (uint32_t(seed) * 2654435761u >> 18) % 2 == 0;
			// Fruit set from June, swelling to full size by August, ripe in autumn; the birds and
			// foxes take most by midwinter (Herrera 1989).
			const float fruit = male ? 0.0f : (in_days(160.0f, 40.0f) ? (in_days(330.0f, 40.0f) ? 0.35f : 1.0f) : 0.0f);
			const float swell = day >= 160.0f ? CLAMP((day - 160.0f) / 70.0f, 0.3f, 1.0f) : 1.0f;
			const float ripe = day >= 160.0f ? CLAMP((day - 220.0f) / 80.0f, 0.0f, 1.0f) : 1.0f;
			const Color fl_y = lin(0.95f, 0.78f, 0.12f), fl_f = lin(0.72f, 0.7f, 0.25f), axis_c = lin(0.62f, 0.55f, 0.22f);
			const Color bract_c = lin(0.5f, 0.42f, 0.26f);
			const Color d_green = lin(0.45f, 0.5f, 0.2f), d_yel = lin(0.8f, 0.62f, 0.16f), d_ripe = lin(0.5f, 0.24f, 0.08f);
			const Color date_c = ripe < 0.5f ? d_green.lerp(d_yel, ripe * 2.0f) : d_yel.lerp(d_ripe, ripe * 2.0f - 1.0f);
			const bool flowers = bloom > 0.0f;
			if (flowers || fruit > 0.0f) {
				Vector4i part;
				r_b.begin(part);
				uint32_t slot = 0;
				for (const int top : palm_crowns) {
					const Node &cn = nodes[top];
					const Vector3 ax = cn.dir;
					const Vector3 s0 = perp(ax);
					// Grown crowns flower; seedlings and cushions in their first years less.
					const int count = int(Math::round((flowers ? Math::lerp(1.0f, 4.0f, bloom) : 3.0f * fruit) * fruit_cover * (height > 0.6f ? 1.0f : 0.5f)));
					r_b.bone = bone_of(top);
					for (int q = 0; q < count; q++) {
						const uint32_t h = hash_murmur3_one_32(slot++, uint32_t(seed) * 2654435761u + 0x70616c6du);
						const float roll = float(h & 0xffff) / 65535.0f * float(Math::TAU);
						const Vector3 out = s0.rotated(ax, roll);
						// From between the leaf bases, a little below the crown's top.
						const Vector3 base = cn.end() - ax * Math::lerp(0.03f, 0.08f, float((h >> 16) & 255u) / 255.0f) + out * 0.04f;
						// Erect to spreading in flower; bowed out and down by the fruit.
						const float bow = flowers ? 0.15f : 0.55f + 0.3f * swell;
						const Vector3 d0 = (ax * (1.0f - bow) + out * 0.8f - Vector3(0, bow * 0.6f, 0)).normalized();
						// Short and stubby (8-15 cm), the branches packed close.
						const float len = Math::lerp(0.08f, 0.15f, float((h >> 8) & 255u) / 255.0f) * (flowers ? 0.8f : 1.0f);
						// The peduncle in its bract.
						LocalVector<Vector3> pts;
						LocalVector<float> rad;
						LocalVector<Color> col;
						pts.push_back(base);
						pts.push_back(base + d0 * (len * 0.45f));
						pts.push_back(base + (d0 - Vector3(0, bow * 0.3f, 0)).normalized() * len);
						rad.push_back(0.006f);
						rad.push_back(0.005f);
						rad.push_back(0.003f);
						col.push_back(axis_c);
						col.push_back(axis_c);
						col.push_back(axis_c);
						tube(pts, rad, 4, col);
						{
							// The bract: a flattened sheath round the foot, split at its end.
							LocalVector<Vector3> bp;
							LocalVector<float> br;
							LocalVector<Color> bc;
							bp.push_back(base - d0 * 0.01f);
							bp.push_back(base + d0 * (len * 0.3f));
							br.push_back(0.011f);
							br.push_back(0.007f);
							bc.push_back(bract_c);
							bc.push_back(bract_c * 1.1f);
							tube(bp, br, 5, bc);
						}
						// The branches: five to eight short rachillae along the outer half.
						const int nb = (flowers && male ? 8 : 6) + int((h >> 4) % 4u);
						for (int b = 0; b < nb; b++) {
							const float t = 0.45f + 0.55f * float(b) / MAX(nb - 1, 1);
							const Vector3 at = pts[1].lerp(pts[2], (t - 0.45f) / 0.55f);
							const Vector3 bd = (d0 + perp(d0).rotated(d0, roll + b * 2.4f) * 0.9f - Vector3(0, flowers ? 0.0f : 0.4f, 0)).normalized();
							const float bl = Math::lerp(0.018f, 0.035f, 1.0f - t * 0.6f);
							stalk(at, at + bd * bl, 0.0018f, axis_c);
							if (flowers) {
								// Packed all round each branch: male ones a golden lump of 4 mm flowers,
								// female ones fewer, larger and greener.
								const int nf = male ? 12 : 6;
								for (int k = 0; k < nf; k++) {
									const float u = (k + 0.5f) / nf;
									const uint32_t hk = hash_murmur3_one_32(uint32_t(k + b * 16), h);
									const Vector3 jit = perp(bd).rotated(bd, float(hk & 255u) / 255.0f * float(Math::TAU)) * (male ? 0.0045f : 0.004f);
									const float r = (male ? 0.0042f : 0.0045f) * (bloom > 0.4f ? 1.0f : 0.75f);
									// A few millimetres across: an octahedron (8 triangles) is round enough.
									const Vector3 fc = at + bd * (bl * u) + jit;
									const Color fcol = male ? fl_y * (0.9f + 0.2f * float((hk >> 8) & 255u) / 255.0f) : fl_f;
									const Vector3 ox[6] = { Vector3(r, 0, 0), Vector3(-r, 0, 0), Vector3(0, r, 0), Vector3(0, -r, 0), Vector3(0, 0, r), Vector3(0, 0, -r) };
									int oi[6];
									for (int o = 0; o < 6; o++) {
										oi[o] = vert(fc + ox[o], Vector2(), fcol);
									}
									const int of[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
									for (int o = 0; o < 8; o++) {
										tri(oi[of[o][0]], oi[of[o][1]], oi[of[o][2]]);
									}
								}
							} else {
								// Dates: one to three a branch, clustered at its end.
								const int nd = 1 + int((h >> (b % 16)) % 3u);
								for (int k = 0; k < nd; k++) {
									const uint32_t hk = hash_murmur3_one_32(uint32_t(k + b * 16 + 1000), h);
									const float hv = float(hk & 255u) / 255.0f;
									if (hv > fruit + 0.15f) {
										continue; // Taken.
									}
									const float r = Math::lerp(0.008f, 0.014f, float((hk >> 8) & 255u) / 255.0f) * swell;
									const Vector3 side = perp(bd).rotated(bd, float((hk >> 16) & 255u) / 255.0f * float(Math::TAU));
									const Vector3 c = at + bd * bl + side * (r * 1.1f * k) - Vector3(0, r * 0.9f, 0);
									stalk(at + bd * bl, c, 0.0012f, axis_c);
									// Globose to oblong, smooth and glossy: a UV sphere on shared vertices (the
									// part's averaged face normals then shade it round), stretched along the hang.
									const Color dc = date_c * (0.9f + 0.2f * float((hk >> 4) & 15u) / 15.0f);
									const float stretch = (hk & 1u) ? 1.35f : 1.05f;
									const int RS = 6, SS = 8;
									const int s0 = r_b.verts.size();
									for (int ri = 0; ri <= RS; ri++) {
										const float th = float(Math::PI) * ri / RS;
										for (int si = 0; si < SS; si++) {
											const float ph = float(Math::TAU) * si / SS;
											const Vector3 p(Math::sin(th) * Math::cos(ph) * r, -Math::cos(th) * r * stretch, Math::sin(th) * Math::sin(ph) * r);
											vert(c + p, Vector2(float(si) / SS, float(ri) / RS), dc);
										}
									}
									for (int ri = 0; ri < RS; ri++) {
										for (int si = 0; si < SS; si++) {
											const int a0 = s0 + ri * SS + si, a1 = s0 + ri * SS + (si + 1) % SS;
											const int b0 = a0 + SS, b1 = a1 + SS;
											// Outward faces under close_part's (c - a) x (b - a).
											tri(a0, a1, b0);
											tri(a1, b1, b0);
										}
									}
								}
							}
						}
					}
				}
				close_part(part, 0.15f);
			}
		}
		big_end = r_b.indices.size();
	}
	if (fruit_cover > 0.0f && params[PARAM_NEEDLES] <= 0.5f && !tips.is_empty() && preset != PRESET_DWARF_FAN_PALM) {
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
		ACORN_KERMES,
		DRUPE_LENTISK,
		CONE_STONE_PINE,
		DRUPE_SLOE,
		HIP_ROSE,
		BLOSSOM, // Hedge shrubs in flower (trees.md 5ad): the preset says whose.
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
		case PRESET_MARITIME_PINE:
			// Cones in whorls, kept for years (Gymnosperm Database).
			kind = CONE_PINE, share = 0.2f, low = 0.5f;
			break;
		case PRESET_STONE_PINE:
			// On the sunlit top of the umbrella, where the crown spends itself on cones.
			kind = CONE_STONE_PINE, share = 0.35f, low = 0.45f;
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
			kind = BERRY_HAW, share = 0.7f, low = 0.1f;
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
		case PRESET_KERMES_OAK:
			kind = ACORN_KERMES, share = 0.12f, low = 0.0f;
			break;
		case PRESET_LENTISK:
			kind = DRUPE_LENTISK, share = 0.25f, low = 0.0f;
			break;
		case PRESET_FIELD_MAPLE:
			kind = KEYS_MAPLE, share = 0.12f, low = 0.2f;
			break;
		case PRESET_BLACKTHORN:
			kind = DRUPE_SLOE, share = 0.6f, low = 0.0f;
			break;
		case PRESET_DOG_ROSE:
			kind = HIP_ROSE, share = 0.3f, low = 0.0f;
			break;
		default:
			break;
	}
	// Hedge shrubs flower from the same tips that later fruit (trees.md 5ad): hawthorn white over
	// the whole bush in May, blackthorn white along its bare twigs in March and April, dog rose
	// pale pink in June.
	const float blossom = (preset == PRESET_HAWTHORN || preset == PRESET_BLACKTHORN || preset == PRESET_DOG_ROSE) ? get_bloom(preset, day) : 0.0f;
	if (blossom > 0.0f) {
		kind = BLOSSOM;
		share = preset == PRESET_DOG_ROSE ? 0.4f : 1.0f;
		low = 0.0f;
	}
	// Dioecious species: only the female trees fruit (holly, yew), by seed.
	if ((kind == BERRY_HOLLY || kind == BERRY_YEW || kind == DRUPE_LENTISK) && (uint32_t(seed) * 2654435761u >> 16) % 2 == 1) {
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
		case ACORN_KERMES:
			// Biennial: the acorns swell through their second summer and fall from October to
			// December (Flora iberica).
			present = in_days(180.0f, 345.0f) ? 1.0f : 0.0f;
			break;
		case DRUPE_LENTISK:
			// Red from late summer, black when ripe from October; birds take them into winter
			// (Jordano 1989).
			present = in_days(210.0f, 45.0f) ? 1.0f : 0.0f;
			break;
		case DRUPE_SLOE:
			// September to November, many left on into winter.
			present = in_days(240.0f, 20.0f) ? 1.0f : 0.0f;
			break;
		case HIP_ROSE:
			present = in_days(240.0f, 45.0f) ? 1.0f : 0.0f;
			break;
		case BLOSSOM:
			present = blossom;
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
	// Hawthorn and blackthorn flower from short spurs all along the last years' wood, not only
	// at the shoot ends (Stace 2019): in flower the whole bush is white, and the haws and sloes
	// follow on the same spurs.
	LocalVector<Tip> spurs;
	if ((kind == BLOSSOM && preset != PRESET_DOG_ROSE) || kind == BERRY_HAW || kind == DRUPE_SLOE) {
		for (int i = 1; i < int(nodes.size()); i++) {
			const Node &n = nodes[i];
			if (!n.removed && !n.dead && n.leafy && n.order >= 1 && n.radius <= 0.012f) {
				spurs.push_back({ i, CLAMP((n.end().y - base_y) / tall, 0.0f, 1.0f) });
			}
		}
	}
	const LocalVector<Tip> &sites = spurs.is_empty() ? tips : spurs;
	// A budget of clusters, so a big crown carries a few hundred, not tens of thousands. Hedge fruit
	// (haws, sloes, hips) comes by the thousand: the autumn hedge is dotted red and blue-black.
	int eligible = 0;
	for (const Tip &t : sites) {
		eligible += t.f >= low ? 1 : 0;
	}
	// Corymbs carry a hundred berries or more each: a rowan bears some dozens of heads.
	// A stone pine's crown is spent on cones, three years' worth at once: about a thousand.
	const float budget = (kind == BERRY_ROWAN || kind == BERRY_ELDER ? 80.0f : (kind == CONE_STONE_PINE ? 900.0f : (kind == BLOSSOM ? 3000.0f : (kind == BERRY_HAW || kind == DRUPE_SLOE || kind == HIP_ROSE ? 900.0f : 360.0f)))) * fruit_cover;
	const float p = MIN(share * fruit_cover * present, eligible > 0 ? budget / eligible : 0.0f);
	// Catkins open before the leaves: hazel from late January, alder February, birch April.
	const bool open_catkin = (kind == CATKIN_HAZEL && in_days(20.0f, 100.0f)) || (kind == CATKIN_ALDER && in_days(40.0f, 100.0f)) || (kind == CATKIN_BIRCH && in_days(100.0f, 135.0f));
	for (const Tip &t : sites) {
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
			case CONE_STONE_PINE: {
				// Three cohorts at once, the longest ripening of any pine: ripe in April three
				// years after pollination (Gymnosperm Database). Conelets of the year 1-2 cm,
				// green second-year cones about half size, third-year cones 8-12 cm, broad ovoid,
				// green, then shiny chestnut-brown, open from spring into the next year. On short
				// stout stalks, held out and up on the shoot rather than hanging.
				const float yr = float((uint32_t(t.node) * 2654435761u) >> 24) / 255.0f;
				const int cohort = yr < 0.35f ? 0 : (yr < 0.65f ? 1 : 2);
				// Day of year moves each cohort on: third-year cones brown from late winter, open
				// from April, and the opened ones fall through the next winter.
				const float ripe = cohort == 2 ? CLAMP((day - 30.0f) / 60.0f, 0.0f, 1.0f) : 0.0f;
				const float op = cohort == 2 ? CLAMP((day - 100.0f) / 120.0f, 0.0f, 1.0f) * 0.7f : 0.0f;
				// Heavy cones come in twos and threes on the bare shoot behind the needle tuft (the
				// needles are 10-18 cm), held out and drooping under their weight: seen from below
				// and from the side, not buried in the needles.
				const int k = cohort == 2 ? 1 + int(cr.randf() * 2.6f) : 1;
				const Color green = lin(0.32f, 0.38f, 0.2f), chestnut = lin(0.42f, 0.24f, 0.12f);
				const Color c0 = green.lerp(chestnut, ripe);
				for (int q = 0; q < k; q++) {
					const float len = cohort == 0 ? Math::lerp(0.012f, 0.02f, cr.randf()) : (cohort == 1 ? Math::lerp(0.035f, 0.05f, cr.randf()) : Math::lerp(0.09f, 0.13f, cr.randf()));
					const Vector3 out = sd.rotated(d, q * 2.3f);
					const Vector3 ax = (d * 0.3f + out * 0.7f + Vector3(0, cohort == 2 ? -0.45f : 0.1f, 0)).normalized();
					const float back = cohort == 0 ? 0.02f : MIN(0.2f, n.length * 0.8f);
					cone(at - d * (back + 0.05f * q) + out * 0.015f, ax, len, len * 0.42f * (1.0f + 0.3f * op), cohort == 0 ? 24 : 72, false, op, c0 * 0.85f, c0.lerp(lin(0.55f, 0.42f, 0.28f), op));
					size = MAX(size, len);
				}
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
			case ACORN_KERMES: {
				// One or two acorns on the year-old wood behind the tip, each 1.5-3 cm, in a cup of
				// stiff, spreading, spine-tipped scales covering a third to a half of it.
				const float day_ripe = CLAMP((day - 250.0f) / 50.0f, 0.0f, 1.0f);
				const Color nut = lin(0.5f, 0.52f, 0.26f).lerp(lin(0.5f, 0.33f, 0.16f), day_ripe);
				const Color cup_c = lin(0.55f, 0.52f, 0.36f);
				const int k = 1 + int(cr.randf() * 1.6f);
				for (int q = 0; q < k; q++) {
					const Vector3 base = n.start + d * (n.length * Math::lerp(0.4f, 0.8f, cr.randf()));
					const Vector3 ax = (sd.rotated(d, cr.randf() * Math::TAU) * 0.7f + d * 0.5f + Vector3(0, -0.3f, 0)).normalized();
					const float L = Math::lerp(0.016f, 0.026f, cr.randf()), r0 = L * 0.36f;
					const Vector3 u0 = perp(ax), u1 = ax.cross(u0);
					const int rings = 7, sides = 10;
					const int v0 = r_b.verts.size();
					// The nut: an ovoid from inside the cup to a blunt point.
					for (int i = 0; i <= rings; i++) {
						const float tt = float(i) / rings;
						const float rr = r0 * Math::pow(Math::sin(float(Math::PI) * (0.5f + 0.5f * tt)), 0.7f) * (i == rings ? 0.0f : 1.0f) + (i == 0 ? 0.0f : 0.0f);
						const Vector3 c = base + ax * (0.004f + L * tt);
						for (int e = 0; e < sides; e++) {
							const float ph = float(Math::TAU) * e / sides;
							vert(c + (u0 * Math::cos(ph) + u1 * Math::sin(ph)) * MAX(rr, 0.0003f), Vector2(), nut);
						}
					}
					for (int i = 0; i < rings; i++) {
						for (int e = 0; e < sides; e++) {
							const int e1 = (e + 1) % sides;
							tri(v0 + i * sides + e, v0 + (i + 1) * sides + e, v0 + i * sides + e1);
							tri(v0 + i * sides + e1, v0 + (i + 1) * sides + e, v0 + (i + 1) * sides + e1);
						}
					}
					// The cup: a bowl to 40% of the nut, ringed by spreading spines.
					const int c0 = r_b.verts.size();
					for (int i = 0; i <= 3; i++) {
						const float tt = float(i) / 3.0f;
						const float rr = r0 * Math::lerp(0.5f, 1.12f, Math::sqrt(tt));
						const Vector3 c = base + ax * (0.004f + L * 0.4f * tt);
						for (int e = 0; e < sides; e++) {
							const float ph = float(Math::TAU) * (e + 0.5f) / sides;
							vert(c + (u0 * Math::cos(ph) + u1 * Math::sin(ph)) * rr, Vector2(), cup_c * (0.85f + 0.15f * float(i & 1)));
						}
					}
					for (int i = 0; i < 3; i++) {
						for (int e = 0; e < sides; e++) {
							const int e1 = (e + 1) % sides;
							tri(c0 + i * sides + e, c0 + (i + 1) * sides + e, c0 + i * sides + e1);
							tri(c0 + i * sides + e1, c0 + (i + 1) * sides + e, c0 + (i + 1) * sides + e1);
						}
					}
					for (int e = 0; e < 18; e++) {
						const float ph = float(Math::TAU) * e / 18.0f + cr.randf() * 0.2f;
						const float tt = 0.3f + 0.7f * float(e % 3) / 2.0f;
						const Vector3 rd = u0 * Math::cos(ph) + u1 * Math::sin(ph);
						const Vector3 sb = base + ax * (0.004f + L * 0.4f * tt) + rd * (r0 * Math::lerp(0.5f, 1.12f, Math::sqrt(tt)));
						const Vector3 tip = sb + (rd * 0.8f - ax * 0.4f).normalized() * (L * 0.18f);
						const Vector3 w = ax.cross(rd).normalized() * (L * 0.03f);
						tri(vert(sb - w, Vector2(), cup_c), vert(tip, Vector2(), cup_c * 0.7f), vert(sb + w, Vector2(), cup_c));
					}
					stalk(n.start + d * (n.length * 0.5f), base, 0.0012f, cup_c);
				}
				size = 0.04f;
			} break;
			case DRUPE_LENTISK: {
				// Short, dense clusters of 4-5 mm drupes in the leaf axils, red, then black from
				// October, not all together.
				const int k = 8 + int(cr.randf() * 14.0f);
				const float ripe = CLAMP((day - 265.0f) / 45.0f + (day < 60.0f ? 2.0f : 0.0f), 0.0f, 1.0f);
				const Vector3 hub = n.start + d * (n.length * Math::lerp(0.3f, 0.8f, cr.randf())) + Vector3(0, -0.006f, 0);
				const Color stalk_c = lin(0.42f, 0.2f, 0.12f);
				stalk(n.start + d * (n.length * 0.5f), hub, 0.0008f, stalk_c);
				for (int q = 0; q < k; q++) {
					const Vector3 o = Vector3(cr.randf() - 0.5f, (cr.randf() - 0.8f) * 0.8f, cr.randf() - 0.5f) * 0.03f;
					const float rb = 0.0022f * Math::lerp(0.85f, 1.15f, cr.randf());
					const Vector3 bc = hub + o;
					const Vector3 away = o.length_squared() > 1e-10f ? o.normalized() : Vector3(0, -1, 0);
					stalk(hub, bc - away * (rb * 0.8f), 0.0003f, stalk_c);
					const float rp = CLAMP(ripe * 1.6f - 0.6f * cr.randf(), 0.0f, 1.0f);
					ball(bc, rb, lin(0.72f, 0.1f, 0.08f).lerp(lin(0.09f, 0.06f, 0.07f), rp) * Math::lerp(0.85f, 1.1f, cr.randf()));
				}
				size = 0.04f;
			} break;
			case DRUPE_SLOE: {
				// One or two, 10-15 mm, blue-black under a dusty waxy bloom, on short stalks along
				// the twig behind its tip.
				const int k = 1 + int(cr.randf() * 1.8f);
				for (int q = 0; q < k; q++) {
					const Vector3 on = n.start + n.dir * (n.length * Math::lerp(0.3f, 0.8f, cr.randf()));
					const Vector3 out = sd.rotated(d, cr.randf() * float(Math::TAU));
					const float rb = Math::lerp(0.005f, 0.0075f, cr.randf());
					const Vector3 bc = on + out * (rb * 1.4f) + Vector3(0, -rb * 1.2f, 0);
					stalk(on, bc + Vector3(0, rb * 0.8f, 0), 0.0006f, lin(0.3f, 0.28f, 0.2f));
					ball(bc, rb, lin(0.33f, 0.36f, 0.5f) * Math::lerp(0.85f, 1.1f, cr.randf()));
				}
				size = 0.03f;
			} break;
			case HIP_ROSE: {
				// One to three, oval, 15-20 mm, scarlet and glossy, at the tips of the arching
				// canes (two balls make the oval), a dark scar where the sepals fell.
				const int k = 1 + int(cr.randf() * 2.5f);
				for (int q = 0; q < k; q++) {
					const Vector3 out = sd.rotated(d, q * 2.2f + cr.randf());
					const Vector3 ax = (d * 0.4f + out * 0.6f + Vector3(0, -0.3f, 0)).normalized();
					const float rb = Math::lerp(0.006f, 0.0075f, cr.randf());
					const Vector3 b0 = at + ax * 0.012f;
					stalk(at, b0, 0.0008f, lin(0.3f, 0.32f, 0.15f));
					const Color c = lin(0.78f, 0.1f, 0.05f) * Math::lerp(0.85f, 1.1f, cr.randf());
					ball(b0 + ax * rb, rb, c);
					ball(b0 + ax * (rb * 2.0f), rb * 0.92f, c);
					const Vector3 tip = b0 + ax * (rb * 2.9f);
					const Vector3 u1 = perp(ax), u2 = ax.cross(u1);
					const float cs = rb * 0.4f;
					const Color dk = lin(0.12f, 0.06f, 0.03f);
					const int s0 = vert(tip + u1 * cs, Vector2(), dk), s1 = vert(tip + (u1 * -0.5f + u2 * 0.87f) * cs, Vector2(), dk), s2 = vert(tip + (u1 * -0.5f - u2 * 0.87f) * cs, Vector2(), dk);
					tri(s0, s1, s2);
				}
				size = 0.05f;
			} break;
			case BLOSSOM: {
				// Five-petalled flowers facing out and up: hawthorn 8-15 mm in flat corymbs of
				// 5-25 with pink-purple anthers; blackthorn 10-15 mm, singly or in pairs crowded
				// along the bare twig, orange anthers; dog rose 4-6 cm, one to three, pale pink with
				// a paler eye and a yellow boss (Stace 2019).
				const bool haw = preset == PRESET_HAWTHORN, rose = preset == PRESET_DOG_ROSE;
				const int k = haw ? 16 + int(cr.randf() * 16.0f) : (rose ? 1 + int(cr.randf() * 2.5f) : 8 + int(cr.randf() * 9.0f));
				const float fr = haw ? Math::lerp(0.0055f, 0.0075f, cr.randf()) : (rose ? Math::lerp(0.022f, 0.028f, cr.randf()) : Math::lerp(0.006f, 0.0075f, cr.randf()));
				const Color petal_c = rose ? lin(0.97f, 0.8f, 0.84f) : (haw ? lin(0.97f, 0.94f, 0.84f) : lin(0.96f, 0.95f, 0.91f));
				const Color eye_c = rose ? lin(0.99f, 0.95f, 0.88f) : petal_c;
				const Color boss_c = haw ? lin(0.55f, 0.22f, 0.32f) : lin(0.95f, 0.75f, 0.2f);
				for (int q = 0; q < k; q++) {
					Vector3 fc;
					if (haw) {
						// Flat-topped corymbs, two to a shoot (its end and a spur half way along):
						// flowers spread over a disc above each.
						const float a = cr.randf() * float(Math::TAU), rr = 0.024f * Math::sqrt(cr.randf());
						fc = (q % 2 == 0 ? at : n.start + n.dir * (n.length * 0.5f) + sd.rotated(d, 2.0f) * 0.02f) + d * 0.01f + (sd * Math::cos(a) + d.cross(sd) * Math::sin(a)) * rr + Vector3(0, 0.008f, 0);
					} else if (rose) {
						fc = at + sd.rotated(d, q * 2.4f) * (0.02f * q) + d * 0.01f;
					} else {
						fc = n.start + n.dir * (n.length * Math::lerp(0.2f, 1.0f, cr.randf())) + sd.rotated(d, cr.randf() * float(Math::TAU)) * 0.006f;
					}
					// Facing out from the shoot and up toward the light.
					const Vector3 face = (sd.rotated(d, cr.randf() * float(Math::TAU)) * 0.7f + Vector3(0, 0.5f, 0) + d * 0.4f).normalized();
					const Vector3 u1 = perp(face), u2 = face.cross(u1);
					const float a0 = cr.randf() * float(Math::TAU);
					if (!rose) {
						// Small flowers by the thousand: five triangles, the boss coloured at the centre
						// and fading into the petals, a third of the full flower's cost.
						const int cc = vert(fc + face * (fr * 0.15f), Vector2(), petal_c.lerp(boss_c, haw ? 0.5f : 0.25f));
						int pv = -1, fv = -1;
						for (int pk = 0; pk < 5; pk++) {
							const float a = a0 + float(Math::TAU) * pk / 5.0f;
							const int v = vert(fc + (u1 * Math::cos(a) + u2 * Math::sin(a)) * fr + face * (fr * 0.2f), Vector2(), petal_c);
							if (pv >= 0) {
								tri(cc, pv, v);
							} else {
								fv = v;
							}
							pv = v;
						}
						tri(cc, pv, fv);
						continue;
					}
					const int c0 = vert(fc + face * (fr * 0.08f), Vector2(), eye_c);
					for (int pk = 0; pk < 5; pk++) {
						// A petal: a rounded kite from the centre, cupped a little.
						const float a = a0 + float(Math::TAU) * pk / 5.0f, hw = float(Math::TAU) / 12.0f;
						const Vector3 dl = u1 * Math::cos(a - hw) + u2 * Math::sin(a - hw), dm = u1 * Math::cos(a) + u2 * Math::sin(a), dr = u1 * Math::cos(a + hw) + u2 * Math::sin(a + hw);
						const int l = vert(fc + dl * (fr * 0.75f) + face * (fr * 0.12f), Vector2(), petal_c);
						const int m = vert(fc + dm * fr + face * (fr * 0.2f), Vector2(), petal_c);
						const int r = vert(fc + dr * (fr * 0.75f) + face * (fr * 0.12f), Vector2(), petal_c);
						tri(c0, l, m);
						tri(c0, m, r);
					}
					// The boss of stamens, a small raised ring.
					const float bs = fr * (rose ? 0.3f : 0.35f);
					const int b0 = vert(fc + face * (fr * 0.25f), Vector2(), boss_c);
					int prev = -1, first = -1;
					for (int pk = 0; pk < 5; pk++) {
						const float a = a0 + float(Math::TAU) * (pk + 0.5f) / 5.0f;
						const int v = vert(fc + (u1 * Math::cos(a) + u2 * Math::sin(a)) * bs + face * (fr * 0.15f), Vector2(), boss_c);
						if (prev >= 0) {
							tri(b0, prev, v);
						} else {
							first = v;
						}
						prev = v;
					}
					tri(b0, prev, first);
				}
				size = haw ? 0.06f : (rose ? 0.08f : 0.04f);
			} break;
			case BERRY_HOLLY:
			case BERRY_HAW:
			case BERRY_YEW: {
				const int k = kind == BERRY_ROWAN ? 15 + int(cr.randf() * 20.0f) : (kind == BERRY_ELDER ? 25 + int(cr.randf() * 30.0f) : (kind == BERRY_HAW ? 3 + int(cr.randf() * 8.0f) : 2 + int(cr.randf() * 5.0f)));
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

// Cork harvest (trees.md 5aa): years since the cork was last stripped from the bole and the
// limb bases, or -1 where it never was: other species, dead wood, trees under 25 (the first
// stripping waits for a girth of 70 cm), and cork_stripped_years 30 or more. -1 picks a stage
// of the nine-year cycle by seed, so a stand shows trees stripped in different summers; just
// stripped stays rare.
float TreeGenerator::_cork_strip_age() const {
	if (preset != PRESET_CORK_OAK || decay_class > 0 || fallen || fallen_branch || params[PARAM_YEARS] < 25.0f || cork_stripped_years >= 30.0f) {
		return -1.0f;
	}
	if (cork_stripped_years >= 0.0f) {
		return cork_stripped_years;
	}
	static const float STAGES[8] = { 0.02f, 0.25f, 1.0f, 2.5f, 3.5f, 5.0f, 6.5f, 8.0f };
	return STAGES[(uint32_t(seed) * 2654435761u >> 13) % 8];
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
	const float floor_k = (amount > 0.0f || _cork_strip_age() >= 0.0f) ? _bark_set(_bark_kind(preset)).relief_floor : 0.0f;
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
	HashSet<Vector3i> edge_cells;
	// A log's peel edge is cut as a step (trees.md 5o): bark the thickness it is, standing over
	// the wood, never a slope across the 2.5 cm grid that read as a decal's edge. Vertices sink
	// fully or not at all, and the triangles the edge crosses are split along it below.
	// A cork harvest is cut as a step too: the cork stands centimetres proud of the stripped
	// stem (trees.md 5aa).
	const float strip_age = _cork_strip_age();
	float strip_h = 0.0f;
	if (strip_age >= 0.0f) {
		// Stripped up to about twice the girth (Portuguese rules allow 2-3 times), and onto the
		// bases of the main limbs, no higher than a metre over the fork.
		const float girth = float(Math::TAU) * MAX(nodes[0].radius, 0.05f);
		strip_h = CLAMP(2.0f * girth, 1.2f, _fork_rule().fork_height + 1.0f);
	}
	const bool step_edge = ((fallen || fallen_branch) && amount > 0.0f) || strip_age >= 0.0f;
	LocalVector<Vector3> orig;
	LocalVector<float> bare_v, depth_v;
	if (step_edge) {
		orig.resize(nv);
		bare_v.resize(nv);
		depth_v.resize(nv);
		for (int v = 0; v < nv; v++) {
			orig[v] = vw[v];
			bare_v[v] = 0.0f;
			depth_v[v] = 0.0f;
		}
	}
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
		if (v < int(r_bark.wound.size()) && !skip[v]) {
			// A living stem's wounds (trees.md 5p), moss already held off in wound_apply.
			bare = MAX(bare, r_bark.wound[v]);
		}
		if ((fallen || fallen_branch) && !skip[v] && bare > 0.35f && bare < 0.65f) {
			// A peel edge (trees.md 5o): for deadwood_features, at most one a 6 cm cell.
			const Vector3i cell(int(Math::floor(vw[v].x / 0.06f)), int(Math::floor(vw[v].y / 0.06f)), int(Math::floor(vw[v].z / 0.06f)));
			if (!edge_cells.has(cell) && deadwood_features.size() < 8 * 384) {
				edge_cells.insert(cell);
				for (float f : { 0.0f, vw[v].x, vw[v].y, vw[v].z, nr[v].x, nr[v].y, nr[v].z, 0.0f }) {
					deadwood_features.push_back(f);
				}
			}
		}
		bool stripped = false;
		if (strip_age >= 0.0f && !skip[v] && radius[v] >= 0.06f) {
			// The cut runs round the stem with an axe, wavering a hand's breadth; the strippers
			// leave a few centimetres at the foot.
			const float h = vw[v].y - ground;
			const float edge = strip_h + 0.12f * (moss_noise(vw[v] * 2.5f, 0x636f726bu) - 0.5f);
			if (h < edge && h > 0.04f) {
				bare = 1.0f;
				stripped = true;
			}
		}
		if (bare > 0.0f) {
			const float r = radius[v];
			// Thin bark on smooth tubes (beech, birch, young stems): a few millimetres. A deeper
			// step stood the moss shell, built on the bark, out of the sunk wood as a ledge.
			// On logs the edge is a cut step, the moss held clear of it: a beech's bark is 5-10 mm.
			float depth = step_edge ? MIN(0.25f * r, 0.003f + 0.015f * r) : MIN(0.25f * r, 0.002f + 0.008f * r);
			if (v < int(r_bark.relief_off.size()) && r_bark.relief_off[v].y > 0.0f) {
				// Displaced bark: the wood lies at the furrow floor (what was bark above it is gone).
				depth = MAX(0.0f, r_bark.relief_off[v].x - 0.9f * floor_k * r_bark.relief_off[v].y) + 0.002f;
			}
			if (stripped) {
				// Virgin cork 3-5 cm thick over the stripped stem, which is smooth; the new cork
				// grows back 3-5 mm a year (Pereira 2007).
				const float regrown = MIN(0.004f * strip_age, 0.03f);
				if (v < int(r_bark.relief_off.size()) && r_bark.relief_off[v].y > 0.0f) {
					// Displaced bark: down past the deepest fissure, so the stem is smooth.
					depth = MAX(0.0f, r_bark.relief_off[v].x - floor_k * r_bark.relief_off[v].y) + MAX(0.004f, 0.012f - regrown);
				} else {
					depth = MIN(0.3f * r, MAX(0.006f, 0.04f - regrown));
				}
			}
			if (step_edge && !skip[v]) {
				bare_v[v] = bare;
				depth_v[v] = depth;
				vw[v] -= nr[v] * (bare >= 0.5f ? depth : 0.0f);
			} else {
				vw[v] -= nr[v] * (depth * Math::smoothstep(0.1f, 0.9f, bare));
			}
			cw[v].a = 0.5f - 0.5f * bare;
		} else {
			cw[v].a = 0.5f + 0.5f * lw;
		}
	}
	if (step_edge) {
		_split_peel_edge(r_bark, orig, bare_v, depth_v, skip);
	}
}

// The peel edge as geometry (trees.md 5o): each bark triangle the edge (bare 0.5) crosses is cut
// along it into a bark piece at the surface and a wood piece at the wood's depth, with a wall of
// the bark's thickness between them, inner bark (phloem, red-brown) under the darker cork. Cut
// points are interpolated from the two corners of their edge in a fixed order, so neighbouring
// triangles share them exactly. New vertices are appended; each part keeps its index range.
void TreeGenerator::_split_peel_edge(Builder &r_b, const LocalVector<Vector3> &p_orig, const LocalVector<float> &p_bare, const LocalVector<float> &p_depth, const LocalVector<uint8_t> &p_skip) {
	const int nv0 = r_b.verts.size();
	const bool tang = r_b.use_tangents && r_b.tangents.size() >= nv0 * 4;
	const bool has_relief = int(r_b.relief_off.size()) >= nv0;
	const bool has_wound = int(r_b.wound.size()) >= nv0;
	const bool has_bones = r_b.bones.size() >= nv0 * 4 && r_b.weights.size() >= nv0 * 4;
	// Which part each triangle and vertex is in.
	LocalVector<int> vert_part;
	vert_part.resize(nv0);
	for (int v = 0; v < nv0; v++) {
		vert_part[v] = -1;
	}
	for (uint32_t i = 0; i < r_b.parts.size(); i++) {
		const Vector4i &pt = r_b.parts[i];
		for (int v = pt.z; v < pt.z + pt.w && v < nv0; v++) {
			vert_part[v] = i;
		}
	}
	auto add_vert = [&](int p_a, int p_b, float p_t, bool p_sunk, int p_side, int p_part) -> int {
		// p_side: the corner whose colour and alpha the vertex takes (its side of the edge).
		const int at = r_b.verts.size();
		const Vector3 n = r_b.normals[p_a].lerp(r_b.normals[p_b], p_t).normalized();
		const float d = Math::lerp(p_depth[p_a], p_depth[p_b], p_t);
		r_b.verts.push_back(p_orig[p_a].lerp(p_orig[p_b], p_t) - n * (p_sunk ? d : 0.0f));
		r_b.normals.push_back(n);
		r_b.uvs.push_back(r_b.uvs[p_a].lerp(r_b.uvs[p_b], p_t));
		Color c = r_b.colors[p_a].lerp(r_b.colors[p_b], p_t);
		c.a = r_b.colors[p_side].a;
		r_b.colors.push_back(c);
		if (tang) {
			for (int k = 0; k < 4; k++) {
				r_b.tangents.push_back(Math::lerp(r_b.tangents[p_a * 4 + k], r_b.tangents[p_b * 4 + k], p_t));
			}
		}
		if (has_bones) {
			const int src = p_t < 0.5f ? p_a : p_b;
			for (int k = 0; k < 4; k++) {
				r_b.bones.push_back(r_b.bones[src * 4 + k]);
				r_b.weights.push_back(r_b.weights[src * 4 + k]);
			}
		}
		if (has_relief) {
			r_b.relief_off.push_back(r_b.relief_off[p_a].lerp(r_b.relief_off[p_b], p_t));
		}
		if (has_wound) {
			r_b.wound.push_back(Math::lerp(r_b.wound[p_a], r_b.wound[p_b], p_t));
		}
		if (p_part >= 0) {
			r_b.extra_part.push_back(Vector2i(at, p_part));
		}
		return at;
	};
	// One cut point per (edge, level): shared by the two triangles on that edge.
	HashMap<uint64_t, int> cuts;
	auto cut = [&](int p_a, int p_b, bool p_sunk, int p_part) -> int {
		const int a = MIN(p_a, p_b), b = MAX(p_a, p_b);
		const uint64_t key = (uint64_t(a) << 33) | (uint64_t(b) << 1) | (p_sunk ? 1u : 0u);
		if (const int *f = cuts.getptr(key)) {
			return *f;
		}
		const float t = CLAMP((0.5f - p_bare[a]) / (p_bare[b] - p_bare[a]), 0.0f, 1.0f);
		const int bark_corner = p_bare[a] < 0.5f ? a : b;
		const int wood_corner = p_bare[a] < 0.5f ? b : a;
		const int v = add_vert(a, b, t, p_sunk, p_sunk ? wood_corner : bark_corner, p_part);
		cuts.insert(key, v);
		return v;
	};
	const Color phloem = Color(0.36f, 0.2f, 0.12f).srgb_to_linear();
	PackedInt32Array out;
	out.resize(r_b.indices.size());
	int n_out = 0;
	auto emit = [&](int a, int b, int c) {
		if (n_out + 3 > out.size()) {
			out.resize(MAX(out.size() * 2, n_out + 3));
		}
		int32_t *ow = out.ptrw();
		ow[n_out++] = a;
		ow[n_out++] = b;
		ow[n_out++] = c;
	};
	// Triangles in index order; parts' index ranges are remapped as they grow.
	LocalVector<int> tri_part;
	tri_part.resize(r_b.indices.size() / 3);
	for (uint32_t i = 0; i < tri_part.size(); i++) {
		tri_part[i] = -1;
	}
	for (uint32_t i = 0; i < r_b.parts.size(); i++) {
		const Vector4i &pt = r_b.parts[i];
		for (int q = pt.x / 3; q < (pt.x + pt.y) / 3 && q < int(tri_part.size()); q++) {
			tri_part[q] = i;
		}
	}
	LocalVector<Vector2i> new_range;
	new_range.resize(r_b.parts.size());
	for (uint32_t i = 0; i < new_range.size(); i++) {
		new_range[i] = Vector2i(-1, 0);
	}
	const int32_t *iw = r_b.indices.ptr();
	const int nidx = r_b.indices.size();
	for (int q = 0; q + 2 < nidx; q += 3) {
		const int part = tri_part[q / 3];
		if (part >= 0 && new_range[part].x < 0) {
			new_range[part].x = n_out;
		}
		const int i0 = iw[q], i1 = iw[q + 1], i2 = iw[q + 2];
		const int c[3] = { i0, i1, i2 };
		bool ok = i0 < nv0 && i1 < nv0 && i2 < nv0 && !p_skip[i0] && !p_skip[i1] && !p_skip[i2];
		int nb = 0;
		if (ok) {
			for (int k = 0; k < 3; k++) {
				nb += p_bare[c[k]] >= 0.5f ? 1 : 0;
			}
		}
		if (!ok || nb == 0 || nb == 3) {
			emit(i0, i1, i2);
			if (part >= 0) {
				new_range[part].y = n_out - new_range[part].x;
			}
			continue;
		}
		// The lone corner (the one on its own side), kept in winding order: L, P, Q.
		int li = 0;
		for (int k = 0; k < 3; k++) {
			if ((p_bare[c[k]] >= 0.5f) == (nb == 1)) {
				li = k;
			}
		}
		const int L = c[li], P = c[(li + 1) % 3], Q = c[(li + 2) % 3];
		const int vp = vert_part[L];
		const bool lone_wood = nb == 1;
		// Cut points on L-P (X) and L-Q (Y), at the surface (bark side) and sunk (wood side).
		const int Xs = cut(L, P, false, vp), Ys = cut(L, Q, false, vp);
		const int Xw = cut(L, P, true, vp), Yw = cut(L, Q, true, vp);
		const int Xl = lone_wood ? Xw : Xs, Yl = lone_wood ? Yw : Ys;
		const int Xo = lone_wood ? Xs : Xw, Yo = lone_wood ? Ys : Yw;
		emit(L, Xl, Yl);
		emit(Xo, P, Q);
		emit(Xo, Q, Yo);
		// The wall: from the surface cut down to the sunk one, both ways round so either winding
		// shows (the bark material culls back faces). Its own vertices carry the phloem colour.
		auto wall_vert = [&](int p_src) -> int {
			const int at = r_b.verts.size();
			r_b.verts.push_back(r_b.verts[p_src]);
			r_b.normals.push_back(r_b.normals[p_src]);
			r_b.uvs.push_back(r_b.uvs[p_src]);
			Color cc = phloem;
			cc.a = 0.5f; // Bark, no lichen.
			r_b.colors.push_back(cc);
			if (tang) {
				for (int k = 0; k < 4; k++) {
					r_b.tangents.push_back(r_b.tangents[p_src * 4 + k]);
				}
			}
			if (has_bones) {
				for (int k = 0; k < 4; k++) {
					r_b.bones.push_back(r_b.bones[p_src * 4 + k]);
					r_b.weights.push_back(r_b.weights[p_src * 4 + k]);
				}
			}
			if (has_relief) {
				r_b.relief_off.push_back(r_b.relief_off[p_src]);
			}
			if (has_wound) {
				r_b.wound.push_back(0.0f);
			}
			if (vp >= 0) {
				r_b.extra_part.push_back(Vector2i(at, vp));
			}
			return at;
		};
		const int a = wall_vert(Xs), b = wall_vert(Ys), cw = wall_vert(Yw), d = wall_vert(Xw);
		// The wall faces the wood side: from the bark toward where the bark is gone.
		const Vector3 mid = (r_b.verts[a] + r_b.verts[b]) * 0.5f;
		const Vector3 wood_dir = (lone_wood ? p_orig[L] : (p_orig[P] + p_orig[Q]) * 0.5f) - mid;
		const Vector3 along = r_b.verts[b] - r_b.verts[a];
		const Vector3 sn = r_b.normals[a];
		Vector3 wn = along.cross(sn);
		if (wn.dot(wood_dir) < 0.0f) {
			wn = -wn;
		}
		wn = (wn.normalized() + sn * 0.2f).normalized();
		for (int v : { a, b, cw, d }) {
			r_b.normals.write[v] = wn;
			if (tang) {
				const Vector3 t = along.normalized();
				r_b.tangents.write[v * 4] = t.x;
				r_b.tangents.write[v * 4 + 1] = t.y;
				r_b.tangents.write[v * 4 + 2] = t.z;
				r_b.tangents.write[v * 4 + 3] = 1.0f;
			}
		}
		// The bottom edge darker: cut fibres in the step's shadow.
		r_b.colors.write[cw] = Color(phloem.r * 0.6f, phloem.g * 0.6f, phloem.b * 0.6f, 0.5f);
		r_b.colors.write[d] = r_b.colors[cw];
		// Godot's front faces wind clockwise: (v2 - v0) x (v1 - v0) along the facing.
		const Vector3 fn = (r_b.verts[cw] - r_b.verts[a]).cross(r_b.verts[b] - r_b.verts[a]);
		if (fn.dot(wn) >= 0.0f) {
			emit(a, b, cw);
			emit(a, cw, d);
		} else {
			emit(a, cw, b);
			emit(a, d, cw);
		}
		if (part >= 0) {
			new_range[part].y = n_out - new_range[part].x;
		}
	}
	if (cuts.is_empty()) {
		return;
	}
	out.resize(n_out);
	r_b.indices = out;
	for (uint32_t i = 0; i < r_b.parts.size(); i++) {
		if (new_range[i].x >= 0) {
			r_b.parts[i].x = new_range[i].x;
			r_b.parts[i].y = new_range[i].y;
		}
	}
}

// Bark stories (docs/ideas/trees.md 5p): what a living stem records of its life. Each tree is
// dealt its wounds from its seed, by species and stem size: lightning scars, frost cracks with
// their ribs, deer and squirrel stripping. All on the main stem, as fields over its length (s,
// metres from the foot) and its circumference (az), read per vertex when its tubes are built.
namespace {
bool wound_species(int p_preset, const int *p_list, int p_n) {
	for (int i = 0; i < p_n; i++) {
		if (p_list[i] == p_preset) {
			return true;
		}
	}
	return false;
}

// The main stem's frame at a point: the nearest point on its centreline, the distance along it,
// the angle round it from a reference that follows the stem (world x laid onto each segment's
// cross-section, z where the segment runs along x).
struct StemAt {
	float s = 0.0f;
	float az = 0.0f;
	float r = 0.0f; // The centreline's radius there.
	float dist = 0.0f; // The point's distance from the centreline.
	Vector3 axis;
	Vector3 radial;
};

Vector3 stem_ref(const Vector3 &p_axis) {
	Vector3 n = Vector3(1, 0, 0) - p_axis * p_axis.x;
	if (n.length_squared() < 0.01f) {
		n = Vector3(0, 0, 1) - p_axis * p_axis.z;
	}
	return n.normalized();
}

bool stem_at(const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_rad, const LocalVector<float> &p_arc, const Vector3 &p_p, StemAt &r_at) {
	float best = 1e30f;
	int bi = -1;
	float bt = 0.0f;
	for (int i = 0; i + 1 < int(p_pts.size()); i++) {
		const Vector3 d = p_pts[i + 1] - p_pts[i];
		const float l2 = d.length_squared();
		if (l2 < 1e-10f) {
			continue;
		}
		const float t = CLAMP((p_p - p_pts[i]).dot(d) / l2, 0.0f, 1.0f);
		const float dd = p_p.distance_squared_to(p_pts[i] + d * t);
		if (dd < best) {
			best = dd;
			bi = i;
			bt = t;
		}
	}
	if (bi < 0) {
		return false;
	}
	const Vector3 axis = (p_pts[bi + 1] - p_pts[bi]).normalized();
	const Vector3 c = p_pts[bi].lerp(p_pts[bi + 1], bt);
	Vector3 rv = p_p - c;
	rv -= axis * rv.dot(axis);
	r_at.dist = rv.length();
	if (r_at.dist < 1e-6f) {
		return false;
	}
	r_at.radial = rv / r_at.dist;
	r_at.axis = axis;
	const Vector3 n = stem_ref(axis);
	const Vector3 b = axis.cross(n);
	r_at.az = Math::atan2(r_at.radial.dot(b), r_at.radial.dot(n));
	r_at.s = Math::lerp(p_arc[bi], p_arc[bi + 1], bt);
	r_at.r = Math::lerp(p_rad[bi], p_rad[bi + 1], bt);
	return true;
}

// The point at s along the centreline and az round it, at p_r from it; its outward direction.
Vector3 stem_point(const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_arc, float p_s, float p_az, float p_r, Vector3 &r_out, Vector3 &r_axis) {
	int i = 0;
	while (i + 2 < int(p_pts.size()) && p_arc[i + 1] < p_s) {
		i++;
	}
	const float t = CLAMP((p_s - p_arc[i]) / MAX(p_arc[i + 1] - p_arc[i], 1e-6f), 0.0f, 1.0f);
	r_axis = (p_pts[i + 1] - p_pts[i]).normalized();
	const Vector3 n = stem_ref(r_axis);
	const Vector3 b = r_axis.cross(n);
	r_out = n * Math::cos(p_az) + b * Math::sin(p_az);
	return p_pts[i].lerp(p_pts[i + 1], t) + r_out * p_r;
}
} // namespace

void TreeGenerator::_plan_wounds() {
	wounds.clear();
	if ((fallen || fallen_branch) && deadwood_stem >= 0 && decay_class >= 1 && decay_class <= 4 && nodes[deadwood_stem].radius >= 0.06f) {
		// Drying checks (trees.md 5o): wood shrinks about twice as much round the stem as along
		// its radius as it dries, so a log splits in radial checks along the grain, a few
		// millimetres to a couple of centimetres open and some centimetres deep, wider and more
		// of them as it seasons; they close again as the wood softens (Panshin and de Zeeuw,
		// Textbook of Wood Technology, 1980; Harmon et al. 1986).
		float len = 0.0f;
		for (int k = deadwood_stem; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
			len += nodes[k].length;
		}
		const float r = nodes[deadwood_stem].radius;
		if (len < 0.8f) {
			return;
		}
		RandomPCG rng;
		rng.seed(uint64_t(seed) * 2654435761u + 0x63686b73u);
		static const float OPEN[5] = { 0.0f, 0.6f, 1.0f, 0.8f, 0.4f };
		const float open = OPEN[decay_class];
		const int n = 3 + int(rng.randf() * 3.0f * MIN(len / 4.0f, 2.0f));
		for (int i = 0; i < n; i++) {
			Wound w;
			w.kind = WOUND_CHECK;
			const float l = Math::lerp(0.3f, 1.6f, rng.randf()) * MIN(1.0f, len / 3.0f);
			w.s0 = Math::lerp(0.15f, MAX(0.2f, len - l - 0.15f), rng.randf());
			w.s1 = w.s0 + l;
			w.az0 = rng.randf() * float(Math::TAU);
			w.twist = (rng.randf() - 0.5f) * 0.25f;
			w.half_w = Math::lerp(0.003f, 0.012f, rng.randf()) * open;
			w.age = MIN(0.045f, Math::lerp(0.12f, 0.25f, rng.randf()) * r) * open; // Depth, metres.
			w.salt = rng.rand();
			wounds.push_back(w);
		}
		return;
	}
	if (bark_wounds <= 0.0f || fallen || fallen_branch || decay_class > 1 || nodes.size() < 3 || cut_height > 0.0f) {
		return;
	}
	const int ms = _main_stem();
	if (ms < 0) {
		return;
	}
	// The stem's length, its radius at breast height, and where its live crown starts (the first
	// living limb over 2 cm).
	float len = 0.0f, r13 = 0.0f, crown_base = -1.0f;
	for (int k = ms; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
		const Node &n = nodes[k];
		if (r13 <= 0.0f && len + n.length > 1.3f) {
			r13 = n.radius;
		}
		if (crown_base < 0.0f) {
			for (int l = n.first_lateral; l >= 0; l = nodes[l].next_sibling) {
				if (!nodes[l].removed && !nodes[l].dead && nodes[l].radius > 0.02f) {
					crown_base = len;
					break;
				}
			}
		}
		len += n.length;
	}
	if (len < 2.5f || r13 <= 0.0f) {
		return;
	}
	if (crown_base < 0.0f) {
		crown_base = 0.6f * len;
	}
	RandomPCG rng;
	rng.seed(uint64_t(seed) * 2654435761u + 0x776f756eu);
	auto chance = [&](float p_base) { return rng.randf() < MIN(1.0f, p_base * bark_wounds); };
	auto age = [&](float p_lo, float p_hi) { return Math::lerp(p_lo, p_hi, rng.randf() * rng.randf()); };
	// The grain's spiral: a turn every 8-25 m, either hand.
	const float twist = (rng.randf() < 0.5f ? -1.0f : 1.0f) * float(Math::TAU) / Math::lerp(8.0f, 25.0f, rng.randf());

	// Lightning strikes the tallest trees of a stand most, oaks and conifers above all (Taylor
	// 1974; Minko 1975). A scar from high in the crown to the foot, a few centimetres to a hand
	// wide.
	{
		static const int MORE[] = { PRESET_OAK, PRESET_RED_OAK, PRESET_WHITE_OAK, PRESET_SCOTS_PINE, PRESET_NORWAY_SPRUCE, PRESET_WHITE_PINE, PRESET_LARCH, PRESET_ASH };
		float p = 0.035f * CLAMP((len - 6.0f) / 14.0f, 0.0f, 1.5f);
		p *= wound_species(preset, MORE, 8) ? 2.0f : 1.0f;
		if (len > 8.0f && chance(p)) {
			Wound w;
			w.kind = WOUND_LIGHTNING;
			w.s0 = 0.0f;
			w.s1 = len * Math::lerp(0.55f, 0.85f, rng.randf());
			w.az0 = rng.randf() * float(Math::TAU);
			w.twist = twist * Math::lerp(0.8f, 1.3f, rng.randf());
			w.half_w = Math::lerp(0.025f, 0.07f, rng.randf());
			w.age = age(0.5f, 40.0f);
			w.salt = rng.rand();
			wounds.push_back(w);
		}
	}
	// Frost cracks split thin-barked broadleaves of some size, low on the stem, and their
	// callus builds into ribs over the winters (Kubler 1988).
	{
		static const int THIN[] = { PRESET_OAK, PRESET_RED_OAK, PRESET_WHITE_OAK, PRESET_LIME, PRESET_BEECH, PRESET_AMERICAN_BEECH, PRESET_ASH, PRESET_SYCAMORE_MAPLE, PRESET_SUGAR_MAPLE, PRESET_RED_MAPLE, PRESET_HORSE_CHESTNUT };
		if (r13 >= 0.12f && wound_species(preset, THIN, 11) && chance(0.18f * CLAMP(r13 / 0.25f, 0.5f, 1.5f))) {
			const int n = rng.randf() < 0.3f ? 2 : 1;
			for (int i = 0; i < n; i++) {
				Wound w;
				w.kind = WOUND_FROST;
				w.s0 = Math::lerp(0.2f, 0.6f, rng.randf());
				w.s1 = MIN(w.s0 + Math::lerp(1.2f, 3.5f, rng.randf()), MAX(crown_base, w.s0 + 1.0f));
				w.az0 = rng.randf() * float(Math::TAU);
				w.twist = twist;
				w.age = age(3.0f, 60.0f);
				w.salt = rng.rand();
				wounds.push_back(w);
			}
		}
	}
	// Deer strip smooth pole-stage stems at browse height, pulling the bark upward with their
	// incisors (Gill 1992; Welch and Scott 1998).
	{
		static const int DEER[] = { PRESET_ASH, PRESET_BEECH, PRESET_AMERICAN_BEECH, PRESET_NORWAY_SPRUCE, PRESET_ROWAN, PRESET_WHITE_WILLOW, PRESET_SYCAMORE_MAPLE, PRESET_LIME, PRESET_LARCH, PRESET_RED_MAPLE, PRESET_SUGAR_MAPLE, PRESET_SILVER_FIR };
		if (r13 >= 0.03f && r13 <= 0.15f && wound_species(preset, DEER, 12) && chance(0.22f)) {
			const int n = 1 + int(rng.randf() * 2.5f);
			const float a_age = age(0.3f, 15.0f);
			for (int i = 0; i < n; i++) {
				Wound w;
				w.kind = WOUND_DEER;
				w.s0 = Math::lerp(0.35f, 0.7f, rng.randf());
				w.s1 = MIN(w.s0 + Math::lerp(0.3f, 1.1f, rng.randf()), 1.85f);
				w.az0 = rng.randf() * float(Math::TAU);
				w.half_w = r13 * Math::lerp(0.3f, 1.1f, rng.randf());
				w.age = a_age + float(i) * rng.randf() * 3.0f;
				w.salt = rng.rand();
				wounds.push_back(w);
			}
		}
	}
	// Grey squirrels strip patches at the crown base of young broadleaves and larch (Mayle et
	// al. 2004).
	{
		static const int SQ[] = { PRESET_BEECH, PRESET_AMERICAN_BEECH, PRESET_SYCAMORE_MAPLE, PRESET_OAK, PRESET_RED_OAK, PRESET_BIRCH, PRESET_LARCH, PRESET_SUGAR_MAPLE, PRESET_RED_MAPLE, PRESET_HORSE_CHESTNUT };
		if (r13 >= 0.05f && r13 <= 0.22f && crown_base > 2.0f && wound_species(preset, SQ, 10) && chance(0.15f)) {
			const int n = 1 + int(rng.randf() * 4.0f);
			const float a_age = age(0.3f, 10.0f);
			for (int i = 0; i < n; i++) {
				Wound w;
				w.kind = WOUND_SQUIRREL;
				w.s0 = MIN(crown_base + Math::lerp(-1.0f, 2.5f, rng.randf()), len - 0.5f);
				w.s1 = Math::lerp(0.05f, 0.18f, rng.randf());
				w.az0 = rng.randf() * float(Math::TAU);
				w.half_w = Math::lerp(0.03f, 0.1f, rng.randf());
				w.age = a_age;
				w.salt = rng.rand();
				wounds.push_back(w);
			}
		}
	}
}

// Branch-scar chevrons (trees.md 5r): on smooth bark (beech, hornbeam) and on birch, a limb's
// junction is marked by a dark chevron, the "Chinese moustache" (Mattheck and Breloer, The Body
// Language of Trees, 1994): the branch bark ridge, the bark fold pushed up in the crotch, splits
// in two arms once the limb dies, and the stem's girth growth pulls them apart round it and down
// its sides. The longer since the limb was shed, the longer and flatter the arms; on birch the
// fold's bark is black against the white. A beech or birch bole grown in a stand carries one from
// every self-pruned limb up its clear length. Planned from the shed and dead laterals of the
// main stem, and filled to a density up the clear bole for the twigs shed before the record.
void TreeGenerator::_plan_chevrons() {
	if (bark_chevrons <= 0.0f || fallen || fallen_branch || decay_class > 1 || nodes.size() < 3) {
		return;
	}
	switch (preset) {
		case PRESET_BEECH:
		case PRESET_AMERICAN_BEECH:
		case PRESET_BIRCH:
		case PRESET_PAPER_BIRCH:
		case PRESET_RED_MAPLE:
		case PRESET_ROWAN:
			break;
		default:
			return;
	}
	const int ms = _main_stem();
	if (ms < 0) {
		return;
	}
	int last_year = 0;
	for (const Node &n : nodes) {
		last_year = MAX(last_year, n.born);
	}
	float len = 0.0f, r13 = 0.0f, crown_base = -1.0f;
	struct Seg {
		float s0, len, r;
	};
	LocalVector<Seg> segs;
	for (int k = ms; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
		const Node &n = nodes[k];
		if (r13 <= 0.0f && len + n.length > 1.3f) {
			r13 = n.radius;
		}
		segs.push_back({ len, n.length, n.radius });
		if (crown_base < 0.0f) {
			for (int l = n.first_lateral; l >= 0; l = nodes[l].next_sibling) {
				if (!nodes[l].removed && !nodes[l].dead && nodes[l].radius > 0.02f) {
					crown_base = len;
					break;
				}
			}
		}
		len += n.length;
	}
	if (len < 2.0f || r13 < 0.03f) {
		return;
	}
	if (crown_base < 0.0f) {
		crown_base = 0.6f * len;
	}
	auto stem_r = [&](float p_s) {
		for (const Seg &g : segs) {
			if (p_s < g.s0 + g.len) {
				return g.r;
			}
		}
		return segs[segs.size() - 1].r;
	};
	// The stem's age from its girth: about 3-4 mm of radius a year in a stand (beech and birch,
	// Pretzsch 2009).
	const float tree_age = CLAMP(r13 / 0.0035f, 8.0f, 250.0f);
	const bool papery = preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH;
	RandomPCG rng;
	rng.seed(uint64_t(seed) * 2654435761u + 0x63687672u);
	const int first = wounds.size();
	auto add = [&](float p_s, float p_az, float p_rb, float p_frac, bool p_scar) {
		const float r = stem_r(p_s);
		Wound w;
		w.kind = WOUND_CHEVRON;
		w.s0 = p_s;
		w.az0 = p_az;
		w.half_w = p_rb;
		w.age = p_frac * tree_age;
		// Arms: the scar's width and then the girth gained since, pulled round the stem; at most
		// a little under half way round.
		// A birch's are broader for the limb (its fold runs out wide in the thin bark).
		const float arm = (p_rb * (1.5f + 5.0f * p_frac) + 0.25f * r * p_frac) * Math::lerp(0.8f, 1.2f, rng.randf()) * (papery ? 1.5f : 1.0f);
		w.extra = MIN(arm, 0.42f * float(Math::PI) * r) * (p_scar ? 1.0f : -1.0f);
		// Flatter with age: from about 60 degrees below the horizontal to about 28 (a birch's
		// steeper, a black inverted V).
		w.twist = Math::deg_to_rad(papery ? Math::lerp(65.0f, 42.0f, p_frac) : Math::lerp(60.0f, 28.0f, p_frac)) * Math::lerp(0.85f, 1.15f, rng.randf());
		w.salt = rng.rand();
		wounds.push_back(w);
	};
	// The recorded limbs: shed (removed) leave a scar, dead ones still stand as stubs.
	len = 0.0f;
	for (int k = ms; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
		const Node &n = nodes[k];
		const Vector3 ref = stem_ref(n.dir);
		const Vector3 bin = n.dir.cross(ref);
		for (int l = n.first_lateral; l >= 0; l = nodes[l].next_sibling) {
			const Node &b = nodes[l];
			if ((!b.removed && !b.dead) || b.radius < 0.005f) {
				// Thinner shed twigs are drawn at a thumb's thickness below (a smaller mark is lost
				// in the bark's own texture).
				continue;
			}
			Vector3 rad = b.dir - n.dir * b.dir.dot(n.dir);
			if (rad.length_squared() < 1e-6f) {
				continue;
			}
			rad.normalize();
			const float s = len + CLAMP((b.start - n.start).dot(n.dir), 0.0f, n.length);
			if (s < 0.4f || s > len + n.length || s > 0.92f * crown_base + 0.08f * len) {
				continue;
			}
			const float frac = last_year > 0 ? CLAMP(float(last_year - b.born) / float(last_year), 0.15f, 1.0f) : 0.5f;
			add(s, Math::atan2(rad.dot(bin), rad.dot(ref)), MIN(MAX(b.radius, 0.012f), 0.45f * stem_r(s)), frac, b.removed);
		}
		len += n.length;
	}
	// The twigs shed before the record, up the clear bole: about one a metre and a half in the
	// open, two in a stand, more on birch; older and larger low down.
	const float clear = MAX(0.0f, crown_base - 0.5f);
	float per_m = (papery ? 4.0f : 1.8f) * Math::lerp(0.6f, 1.0f, CLAMP(r13 / 0.2f, 0.0f, 1.0f));
	if (preset == PRESET_RED_MAPLE || preset == PRESET_ROWAN) {
		per_m *= 0.5f;
	}
	const int want = int(clear * per_m * bark_chevrons);
	int have = int(wounds.size()) - first;
	for (int tries = 0; have < want && tries < want * 6; tries++) {
		const float s = Math::lerp(0.5f, crown_base, rng.randf());
		const float az = rng.randf() * float(Math::TAU) - float(Math::PI);
		const float r = stem_r(s);
		bool clash = false;
		for (int i = first; i < int(wounds.size()) && !clash; i++) {
			const Wound &o = wounds[i];
			const float ds = s - o.s0;
			const float dl = Math::abs(Math::angle_difference(az, o.az0)) * r;
			clash = Math::abs(ds) < 0.12f + 0.4f * Math::abs(o.extra) && dl < 0.08f + 1.1f * Math::abs(o.extra);
		}
		if (clash) {
			continue;
		}
		const float low = 1.0f - s / MAX(crown_base, 0.5f);
		const float frac = CLAMP(Math::lerp(0.2f, 0.95f, rng.randf()) * Math::lerp(0.7f, 1.1f, low), 0.1f, 1.0f);
		// Twigs thinner than a thumb leave no mark that reads; the scars are of limbs that were.
		const float rb = CLAMP(r13 * Math::lerp(0.04f, 0.14f, rng.randf()), 0.012f, 0.035f);
		add(s, az, rb, frac, true);
		have++;
	}
}

bool TreeGenerator::_wounds_resin() const {
	switch (preset) {
		case PRESET_SCOTS_PINE:
		case PRESET_NORWAY_SPRUCE:
		case PRESET_LARCH:
		case PRESET_SILVER_FIR:
		case PRESET_WHITE_PINE:
		case PRESET_STONE_PINE:
		case PRESET_MARITIME_PINE: // Tapped for resin in the Landes for centuries.
		case PRESET_HEMLOCK:
			return true;
		default:
			return false;
	}
}

// x: offset out from the stem surface (metres), y: bare wood 0..1 (0.5 at the edge, ramping over
// 2 cm so the interpolated value finds the edge between vertices), z: darkening.
Vector3 TreeGenerator::_wound_at(float p_s, float p_az, float p_r, int p_only) const {
	float off = 0.0f, bare = 0.0f, dark = 0.0f;
	for (int i = 0; i < int(wounds.size()); i++) {
		if (p_only >= 0 && i != p_only) {
			continue;
		}
		const Wound &w = wounds[i];
		if (w.kind == WOUND_CHEVRON) {
			continue; // Its own strips (_grow_chevrons).
		}
		const float salt = float(w.salt & 0xffff) * 0.01f;
		// Woundwood: rolls of callus grow in from every edge, about 3 mm a year wider and
		// standing out a little less (Shigo 1989; Neely 1988).
		const float rw = CLAMP(0.008f + 0.003f * w.age, 0.01f, 0.08f);
		const float rh = MIN(MIN(0.0015f * w.age, 0.035f), 0.18f * p_r) * Math::smoothstep(1.0f, 3.0f, w.age);
		float d = 1e9f; // Signed distance to the bare edge, negative inside.
		float groove = 0.0f;
		switch (w.kind) {
			case WOUND_LIGHTNING: {
				if (p_s > w.s1 + 0.2f) {
					continue;
				}
				const float l = Math::angle_difference(p_az, w.az0 + w.twist * (p_s - w.s0)) * p_r;
				float hw = w.half_w * (0.7f + 0.6f * moss_noise(Vector3(p_s * 2.5f, 0.3f, salt), w.salt));
				hw *= Math::smoothstep(w.s1, w.s1 - 2.0f, p_s);
				// Stretches where the bark held.
				hw *= 1.0f - Math::smoothstep(0.66f, 0.74f, moss_noise(Vector3(p_s * 0.6f, 3.1f, salt), w.salt + 7u));
				d = Math::abs(l) - hw;
				// The wood split down the strip's middle.
				groove = 0.012f * (1.0f - Math::smoothstep(0.002f, 0.012f, Math::abs(l))) * CLAMP(hw / 0.02f, 0.0f, 1.0f);
				dark = MAX(dark, 0.7f * (1.0f - Math::smoothstep(0.002f, 0.01f, Math::abs(l))) * CLAMP(hw / 0.02f, 0.0f, 1.0f));
			} break;
			case WOUND_FROST: {
				const float e = Math::smoothstep(w.s0, w.s0 + 0.35f, p_s) * Math::smoothstep(w.s1, w.s1 - 0.6f, p_s);
				if (e <= 0.0f) {
					continue;
				}
				const float l = Math::angle_difference(p_az, w.az0 + w.twist * (p_s - w.s0) + 0.03f * (moss_noise(Vector3(p_s * 3.0f, 1.7f, salt), w.salt) - 0.5f) / MAX(p_r, 0.05f)) * p_r;
				const float x = Math::abs(l);
				// Two lips of callus either side of the split, each winter's growth (frost ribs).
				const float wr = CLAMP(0.03f + 0.0012f * w.age, 0.03f, 0.08f);
				const float h = MIN(0.02f + 0.0015f * w.age, MIN(0.08f, 0.25f * p_r)) * e;
				const float u = (x - 0.65f * wr) / (0.7f * wr);
				// The split between them: a V a few centimetres deep (the bark's grid is 2 cm round a
				// mature stem, so a narrower one would fall between its vertices).
				off += h * Math::exp(-u * u) - (0.03f + 0.5f * h) * e * (1.0f - Math::smoothstep(0.0f, 0.03f, x));
				dark = MAX(dark, e * (1.0f - Math::smoothstep(0.01f, 0.035f, x)));
				continue;
			}
			case WOUND_CHECK: {
				const float e = Math::smoothstep(w.s0, w.s0 + 0.25f, p_s) * Math::smoothstep(w.s1, w.s1 - 0.25f, p_s);
				if (e <= 0.0f) {
					continue;
				}
				// Wandering a little with the grain.
				const float l = Math::angle_difference(p_az, w.az0 + w.twist * (p_s - w.s0) + 0.02f * (moss_noise(Vector3(p_s * 4.0f, 0.9f, salt), w.salt) - 0.5f) / MAX(p_r, 0.05f)) * p_r;
				const float x = Math::abs(l);
				const float hw = w.half_w * (0.5f + 0.5f * e);
				// A V as wide as the stem's 2.5 cm grid can carry, its floor the check's depth;
				// the slot itself dark.
				off -= w.age * e * (1.0f - Math::smoothstep(hw, hw + 0.022f, x));
				dark = MAX(dark, e * (1.0f - Math::smoothstep(hw, hw + 0.012f, x)));
				continue;
			}
			case WOUND_DEER: {
				const float l = Math::angle_difference(p_az, w.az0) * p_r;
				const float hw = w.half_w * (1.0f + 0.4f * (moss_noise(Vector3(p_s * 5.0f, 2.3f, salt), w.salt) - 0.5f));
				// The strip tears off upward and ends in tongues, a few centimetres apart.
				const float period = 0.05f;
				const float cell = Math::floor(l / period);
				const float tri = Math::abs(Math::fposmod(l / period, 1.0f) - 0.5f) * 2.0f;
				const float amp = 0.04f + 0.18f * bark_hash(int(cell), 11, w.salt);
				const float top = w.s1 - amp * tri - 0.1f * bark_hash(int(cell), 12, w.salt);
				const float bottom = w.s0 + 0.04f * moss_noise(Vector3(l * 20.0f, 0.5f, salt), w.salt + 3u);
				d = MAX(Math::abs(l) - hw, MAX(p_s - top, bottom - p_s));
			} break;
			case WOUND_SQUIRREL: {
				const float l = Math::angle_difference(p_az, w.az0) * p_r;
				const float a = l / w.half_w, b = (p_s - w.s0) / w.s1;
				float q = Math::sqrt(a * a + b * b);
				q *= 1.0f + 0.5f * (moss_noise(Vector3(l * 12.0f, p_s * 12.0f, salt), w.salt) - 0.5f);
				d = (q - 1.0f) * MIN(w.half_w, w.s1);
			} break;
		}
		bare = MAX(bare, CLAMP(0.5f - d / 0.02f, 0.0f, 1.0f));
		if (rh > 0.0f) {
			const float x = d / rw;
			const float u = (x - 0.35f) / 0.45f;
			off += rh * Math::exp(-u * u) * Math::smoothstep(-0.4f, 0.05f, x);
		}
		off -= groove;
	}
	return Vector3(off, bare, dark);
}

// Drying checks as geometry (trees.md 5o): the stem's 2.5 cm grid can only sink a soft trough
// along a check, which reads as a groove drawn on the wood, never a split. Each check gets its
// own walls: two faces of split wood from the lips at the check's half width down to a V floor
// below the trough, a centimetre along the grain, following the same wandering line as the
// trough (_wound_at). Split wood inside is less weathered than the face, darkening with depth
// (no light reaches the floor). Wood surface, "wood" material: fibres along the log.
void TreeGenerator::_grow_checks() {
	if (wounds.is_empty() || wound_pts.size() < 2 || wound_surf.is_empty() || !(fallen || fallen_branch)) {
		return;
	}
	const int na = 96;
	const int ns = int(wound_surf.size()) / na;
	const float ground = nodes.is_empty() ? 0.0f : nodes[0].start.y;
	auto radius_at = [&](float p_s) {
		int i = 0;
		while (i + 2 < int(wound_arc.size()) && wound_arc[i + 1] < p_s) {
			i++;
		}
		const float t = CLAMP((p_s - wound_arc[i]) / MAX(wound_arc[i + 1] - wound_arc[i], 1e-6f), 0.0f, 1.0f);
		return Math::lerp(wound_rad[i], wound_rad[i + 1], t);
	};
	auto surf_at = [&](float p_s, float p_az) {
		const int is = CLAMP(int(p_s / 0.02f), 0, ns - 1);
		const int ia = ((int(Math::floor((p_az + float(Math::PI)) / float(Math::TAU) * na)) % na) + na) % na;
		const float m = wound_surf[is * na + ia];
		return m > 0.0f ? m : radius_at(p_s);
	};
	static const float GREY[5] = { 0.0f, 0.3f, 0.6f, 0.85f, 1.0f };
	const float old = GREY[CLAMP(decay_class, 0, 4)];
	// Inside a check the wood is fresher than the weathered face: buff to grey-brown, then dark.
	const Color inner = Color(0.62f, 0.54f, 0.42f).srgb_to_linear().lerp(Color(0.44f, 0.4f, 0.35f).srgb_to_linear(), old);
	const Color deep = Color(0.07f, 0.06f, 0.05f);
	Builder &w = wood_b;
	for (int wi = 0; wi < int(wounds.size()); wi++) {
		const Wound &wd = wounds[wi];
		if (wd.kind != WOUND_CHECK || wd.half_w < 0.0015f) {
			continue;
		}
		const float salt = float(wd.salt & 0xffff) * 0.01f;
		Vector4i part;
		w.begin(part);
		const int first = w.verts.size();
		const float step = 0.01f;
		const int n = MAX(2, int((wd.s1 - wd.s0) / step) + 1);
		const int across = 4; // Lip, mid wall, floor, mid wall, lip: 5 columns.
		for (int i = 0; i < n; i++) {
			const float sv = Math::lerp(wd.s0, wd.s1, float(i) / (n - 1));
			const float r = radius_at(sv);
			const float e = Math::smoothstep(wd.s0, wd.s0 + 0.25f, sv) * Math::smoothstep(wd.s1, wd.s1 - 0.25f, sv);
			// The trough's own centre line (as _wound_at), so the walls sit in it.
			const float azc = wd.az0 + wd.twist * (sv - wd.s0) + 0.02f * (moss_noise(Vector3(sv * 4.0f, 0.9f, salt), wd.salt) - 0.5f) / MAX(r, 0.05f);
			const float hw = wd.half_w * (0.5f + 0.5f * e) * Math::smoothstep(0.0f, 0.35f, e);
			const float depth = wd.age * e + 0.008f * e;
			// The face either side, read just outside the trough's shoulder.
			const float da = (wd.half_w + 0.026f) / MAX(r, 0.02f);
			float top = MIN(surf_at(sv, azc - da), surf_at(sv, azc + da));
			{
				Vector3 out, ax;
				const Vector3 p = stem_point(wound_pts, wound_arc, sv, azc, top, out, ax);
				const float bare = _bare_at(p, out, ax, ground);
				if (bare >= 0.5f) {
					// Where the bark is off the wood face lies the bark's thickness in (_peel_bark).
					top -= MIN(0.25f * r, 0.003f + 0.015f * r);
				}
			}
			top -= 0.001f;
			for (int k = 0; k <= across; k++) {
				const float t = float(k) / across * 2.0f - 1.0f; // -1 lip, 0 floor, 1 lip.
				const float at = Math::abs(t);
				// A V a little rounded at the floor; the lips eased so they do not stand as fins.
				const float rr = top - depth * (1.0f - at * at * (0.7f + 0.3f * at));
				const float x = t * hw;
				Vector3 out, ax;
				const Vector3 p = stem_point(wound_pts, wound_arc, sv, azc + x / MAX(rr, 0.02f), rr, out, ax);
				w.verts.push_back(p);
				// Each wall faces across the check: the wall on the -x side faces +x (round the stem).
				const Vector3 tang = ax.cross(out).normalized();
				const float side = t < 0.0f ? 1.0f : (t > 0.0f ? -1.0f : 0.0f);
				w.normals.push_back((tang * side + out * 0.35f).normalized());
				w.uvs.push_back(Vector2(t * hw / 0.15f + 0.37f * float(wi), sv / 0.15f));
				const float dk = 1.0f - at; // 0 at the lips, 1 at the floor.
				Color c = inner.lerp(deep, Math::smoothstep(0.15f, 0.95f, dk));
				c = c * (0.85f + 0.3f * moss_noise(Vector3(sv * 9.0f, t, salt), wd.salt + 5u));
				c.a = 1.0f;
				w.colors.push_back(c);
			}
		}
		const int cols = across + 1;
		for (int i = 0; i + 1 < n; i++) {
			for (int k = 0; k < across; k++) {
				const int a = first + i * cols + k, b = a + 1, c2 = a + cols, d = c2 + 1;
				// Clockwise front (Godot) facing the opening: out of the stem.
				auto tri = [&](int p0, int p1, int p2) {
					const Vector3 fn = (w.verts[p2] - w.verts[p0]).cross(w.verts[p1] - w.verts[p0]);
					const Vector3 want = w.normals[p0] + w.normals[p1] + w.normals[p2];
					if (fn.dot(want) < 0.0f) {
						SWAP(p1, p2);
					}
					w.indices.push_back(p0);
					w.indices.push_back(p1);
					w.indices.push_back(p2);
				};
				tri(a, c2, b);
				tri(b, c2, d);
			}
		}
		w.end(part, wd.half_w);
	}
}

// Branch-scar chevrons as geometry (trees.md 5r): the stem's bark grid is 2.5 cm round and 5-10 cm
// along at best, so a chevron's arm a centimetre or two wide cannot be drawn in its vertex colour.
// Each scar is its own small mesh laid on the recorded stem surface: an oval eye with a callus rim,
// and two arms tapering out and down, a low fold of bark whose edges tuck under the stem's surface
// (no floating edge at a grazing view), creased across. Bark material, its UV continued from the
// stem under it and its colour and lichen weight taken from there, darkened.
void TreeGenerator::_grow_chevrons(Builder &r_bark) {
	if (wounds.is_empty() || wound_pts.size() < 2 || wound_surf.is_empty() || wound_vtx.size() != wound_surf.size() || fallen || fallen_branch) {
		return;
	}
	const int na = 96;
	const int ns = int(wound_surf.size()) / na;
	auto radius_at = [&](float p_s) {
		int i = 0;
		while (i + 2 < int(wound_arc.size()) && wound_arc[i + 1] < p_s) {
			i++;
		}
		const float t = CLAMP((p_s - wound_arc[i]) / MAX(wound_arc[i + 1] - wound_arc[i], 1e-6f), 0.0f, 1.0f);
		return Math::lerp(wound_rad[i], wound_rad[i + 1], t);
	};
	auto bin_of = [&](float p_s, float p_az) {
		const int is = CLAMP(int(p_s / 0.02f), 0, ns - 1);
		const int ia = ((int(Math::floor((p_az + float(Math::PI)) / float(Math::TAU) * na)) % na) + na) % na;
		return is * na + ia;
	};
	// The surface radius, bilinear over the bins (the tube's own between its vertices).
	auto surf_at = [&](float p_s, float p_az) {
		const float fs = CLAMP(p_s / 0.02f - 0.5f, 0.0f, float(ns - 1));
		const float fa = (p_az + float(Math::PI)) / float(Math::TAU) * na - 0.5f;
		const int s0 = int(fs), a0 = int(Math::floor(fa));
		const float ts = fs - s0, ta = fa - a0;
		float sum = 0.0f, wsum = 0.0f;
		for (int ds = 0; ds <= 1; ds++) {
			for (int da = 0; da <= 1; da++) {
				const float w = (ds ? ts : 1.0f - ts) * (da ? ta : 1.0f - ta);
				const float m = wound_surf[MIN(s0 + ds, ns - 1) * na + ((a0 + da) % na + na) % na];
				if (m > 0.0f && w > 0.0f) {
					sum += w * m;
					wsum += w;
				}
			}
		}
		return wsum > 0.0f ? sum / wsum : radius_at(p_s);
	};
	// The stem's own triangles, bucketed by (s, az), for the surface under each strip vertex: the
	// recorded bins are sparse where the rings are 10 cm apart, and a radius read between them sank
	// stretches of an arm under the bark.
	const float len = wound_arc[wound_arc.size() - 1];
	const int bs_n = int(len / 0.05f) + 1, ba_n = 32;
	LocalVector<LocalVector<int>> buckets;
	buckets.resize(bs_n * ba_n);
	{
		HashSet<int> stem_parts;
		for (const Vector2i &e : wound_vtx) {
			if (e.y >= 0) {
				stem_parts.insert(e.y);
			}
		}
		const int *ix = r_bark.indices.ptr();
		const Vector3 *vx = r_bark.verts.ptr();
		LocalVector<StemAt> v_at;
		for (int pi : stem_parts) {
			if (pi >= int(r_bark.parts.size())) {
				continue;
			}
			const Vector4i &pt = r_bark.parts[pi];
			v_at.resize(pt.w);
			LocalVector<uint8_t> ok;
			ok.resize(pt.w);
			for (int v = 0; v < pt.w; v++) {
				ok[v] = stem_at(wound_pts, wound_rad, wound_arc, vx[pt.z + v], v_at[v]) ? 1 : 0;
			}
			for (int q = pt.x; q + 2 < pt.x + pt.y; q += 3) {
				float s_lo = 1e9f, s_hi = -1e9f;
				float a_ref = 0.0f, a_lo = 0.0f, a_hi = 0.0f;
				bool good = true;
				for (int j = 0; j < 3 && good; j++) {
					const int lv = ix[q + j] - pt.z;
					if (lv < 0 || lv >= pt.w || !ok[lv]) {
						good = false;
						break;
					}
					const StemAt &a = v_at[lv];
					s_lo = MIN(s_lo, a.s);
					s_hi = MAX(s_hi, a.s);
					if (j == 0) {
						a_ref = a.az;
					} else {
						const float d = Math::angle_difference(a_ref, a.az);
						a_lo = MIN(a_lo, d);
						a_hi = MAX(a_hi, d);
					}
				}
				if (!good || a_hi - a_lo > 1.5f) {
					continue;
				}
				const int i0 = CLAMP(int(s_lo / 0.05f), 0, bs_n - 1), i1 = CLAMP(int(s_hi / 0.05f), 0, bs_n - 1);
				const int j0 = int(Math::floor((a_ref + a_lo + float(Math::PI)) / float(Math::TAU) * ba_n));
				const int j1 = int(Math::floor((a_ref + a_hi + float(Math::PI)) / float(Math::TAU) * ba_n));
				for (int i = i0; i <= i1; i++) {
					for (int j = j0; j <= j1; j++) {
						buckets[i * ba_n + ((j % ba_n) + ba_n) % ba_n].push_back(q);
					}
				}
			}
		}
	}
	// The stem's outermost surface along a ray from its centreline at (s, az); the bins' radius
	// where the ray finds none.
	auto surface = [&](float p_s, float p_az) {
		Vector3 out, ax;
		const Vector3 o = stem_point(wound_pts, wound_arc, p_s, p_az, 0.0f, out, ax);
		const int i = CLAMP(int(p_s / 0.05f), 0, bs_n - 1);
		const int j = ((int(Math::floor((p_az + float(Math::PI)) / float(Math::TAU) * ba_n)) % ba_n) + ba_n) % ba_n;
		float best = -1.0f;
		const int *ix = r_bark.indices.ptr();
		const Vector3 *vx = r_bark.verts.ptr();
		for (int q : buckets[i * ba_n + j]) {
			const Vector3 &a = vx[ix[q]], &b = vx[ix[q + 1]], &c = vx[ix[q + 2]];
			const Vector3 e1 = b - a, e2 = c - a;
			const Vector3 pv = out.cross(e2);
			const float det = e1.dot(pv);
			if (Math::abs(det) < 1e-12f) {
				continue;
			}
			const float inv = 1.0f / det;
			const Vector3 tv = o - a;
			const float u = tv.dot(pv) * inv;
			if (u < -1e-4f || u > 1.0001f) {
				continue;
			}
			const Vector3 qv = tv.cross(e1);
			const float v = out.dot(qv) * inv;
			if (v < -1e-4f || u + v > 1.0001f) {
				continue;
			}
			const float t = e2.dot(qv) * inv;
			best = MAX(best, t);
		}
		return best > 0.0f ? best : surf_at(p_s, p_az);
	};
	const bool papery = preset == PRESET_BIRCH || preset == PRESET_PAPER_BIRCH;
	const bool beech = preset == PRESET_BEECH || preset == PRESET_AMERICAN_BEECH;
	// The fold's darkness (a multiplier on the bark's colour) and height: beech's is a grey-black
	// fold about a millimetre proud, birch's a flat black mark on the white.
	const float k_dark = papery ? 0.1f : (beech ? 0.45f : 0.6f);
	const float fold = papery ? 0.0002f : 0.0004f;
	const float tuck = 0.0005f;
	const Vector4 keep_uv2 = r_bark.uv2_map;
	const uint8_t keep_role = r_bark.role;
	r_bark.uv2_map = Vector4();
	r_bark.role = Builder::ROLE_ALL;
	for (int wi = 0; wi < int(wounds.size()); wi++) {
		const Wound &w = wounds[wi];
		if (w.kind != WOUND_CHEVRON || w.s0 >= wound_arc[wound_arc.size() - 1] - 0.05f) {
			continue;
		}
		// The stem vertex under the scar: the UV, colour and skin the scar continues.
		Vector2i bv(-1, -1);
		{
			const int c = bin_of(w.s0, w.az0);
			for (int d = 0; d < 9 && bv.x < 0; d++) {
				const int is = c / na + (d / 3 - 1), ia = (c % na + (d % 3 - 1) + na) % na;
				if (is >= 0 && is < ns) {
					bv = wound_vtx[is * na + ia];
				}
			}
		}
		if (bv.x < 0 || bv.x >= r_bark.verts.size()) {
			continue;
		}
		StemAt at;
		if (!stem_at(wound_pts, wound_rad, wound_arc, r_bark.verts[bv.x], at)) {
			continue;
		}
		const Vector2 base_uv = r_bark.uvs[bv.x];
		const float repeats = MAX(1.0f, Math::round(float(Math::TAU) * wound_rad[0] / _bark_tile()));
		const float circ = float(Math::TAU) * wound_rad[0] / repeats;
		const float salt = float(w.salt & 0xffff) * 0.01f;
		const float frac = CLAMP(w.age / MAX(1.0f, CLAMP(radius_at(1.3f) / 0.0035f, 8.0f, 250.0f)), 0.0f, 1.0f);
		Vector4i part;
		r_bark.begin(part);
		// The skin (and the wind's bone) of the stem vertex under the scar.
		const bool skin = r_bark.bones.size() == r_bark.verts.size() * 4 && r_bark.weights.size() == r_bark.bones.size();
		// One vertex at (x round the stem, y along it) in metres from the scar's centre, lifted
		// by p_h off the surface, its height's gradient (gx, gy) for the normal, darkened by p_d.
		auto put = [&](float p_x, float p_y, float p_h, float p_gx, float p_gy, float p_d) {
			const float sv = w.s0 + p_y;
			const float rr = MAX(radius_at(sv), 0.01f);
			const float az = w.az0 + p_x / rr;
			Vector3 out, ax;
			const Vector3 p = stem_point(wound_pts, wound_arc, sv, az, surface(sv, az) + p_h, out, ax);
			const Vector3 tang = ax.cross(out).normalized();
			r_bark.verts.push_back(p);
			r_bark.normals.push_back((out - tang * p_gx - ax * p_gy).normalized());
			r_bark.uvs.push_back(base_uv + Vector2(Math::angle_difference(at.az, az) * repeats / float(Math::TAU), (sv - at.s) / circ));
			// The colour under the scar's middle for the mark's dark body (per-vertex reads took the
			// moss and film tints of the stem's grid in patches along an arm), the bark's own under
			// its soft edge, so the edge meets the bark without a halo.
			const Vector2i under = wound_vtx[bin_of(sv, az)];
			const Color cu = r_bark.colors[under.x >= 0 && under.x < r_bark.colors.size() ? under.x : bv.x];
			Color c = cu.lerp(r_bark.colors[bv.x], Math::smoothstep(0.1f, 0.5f, p_d));
			const float k = Math::lerp(1.0f, k_dark, CLAMP(p_d, 0.0f, 1.0f));
			// Lichen keeps off the fold a little (its weight in the alpha's upper half); held off
			// wholly, the bark texture's own pale crusts showed through the dark as blotches.
			const float a = c.a > 0.5f ? Math::lerp(c.a, 0.5f, 0.5f * p_d) : c.a;
			r_bark.colors.push_back(Color(c.r * k, c.g * k, c.b * k, a));
			if (r_bark.use_tangents) {
				r_bark.tangents.push_back(tang.x);
				r_bark.tangents.push_back(tang.y);
				r_bark.tangents.push_back(tang.z);
				r_bark.tangents.push_back(1.0f);
			}
			if (skin) {
				for (int j = 0; j < 4; j++) {
					r_bark.bones.push_back(r_bark.bones[bv.x * 4 + j]);
					r_bark.weights.push_back(r_bark.weights[bv.x * 4 + j]);
				}
			}
			r_bark.extra_part.push_back(Vector2i(r_bark.verts.size() - 1, bv.y));
		};
		// Counter-clockwise seen from outside, as _tube's (the bark is flipped in the mesh loop).
		auto quad_grid = [&](int p_first, int p_rows, int p_cols, bool p_wrap) {
			const int span = p_wrap ? p_cols : p_cols - 1;
			for (int i = 0; i + 1 < p_rows; i++) {
				for (int k = 0; k < span; k++) {
					const int k1 = (k + 1) % p_cols;
					const int a = p_first + i * p_cols + k, b = p_first + i * p_cols + k1;
					const int c = a + p_cols, d = b + p_cols;
					for (const Vector3i &t : { Vector3i(a, b, c), Vector3i(b, d, c) }) {
						int p0 = t.x, p1 = t.y, p2 = t.z;
						const Vector3 fn = (r_bark.verts[p2] - r_bark.verts[p0]).cross(r_bark.verts[p1] - r_bark.verts[p0]);
						if (fn.dot(r_bark.normals[p0] + r_bark.normals[p1] + r_bark.normals[p2]) > 0.0f) {
							SWAP(p1, p2);
						}
						r_bark.indices.push_back(p0);
						r_bark.indices.push_back(p1);
						r_bark.indices.push_back(p2);
					}
				}
			}
		};
		const bool scar = w.extra > 0.0f;
		const float rb = w.half_w;
		// The eye: the occluded stub, a knob of callus over it, an oval stretched round the stem as
		// it grows; the scar itself a small dark pit at its top (Shigo 1989: the stub is walled
		// off and overgrown from the collar).
		const float ex = MIN(MAX(rb * (1.2f + 0.6f * frac), 0.015f), 0.06f);
		const float ey = ex * Math::lerp(0.85f, 0.6f, frac);
		if (scar) {
			const int sides = CLAMP(int(float(Math::TAU) * ex / 0.004f), 14, 48);
			static const float RING[6] = { 0.0f, 0.18f, 0.35f, 0.6f, 0.85f, 1.0f };
			// A dome a fifth of its width high, a pit at its middle, its foot tucked under the bark.
			const float knob = (papery ? 0.12f : 0.22f) * ex;
			const float HGT[6] = { 0.55f * knob, 0.75f * knob, knob, 0.75f * knob, 0.3f * knob, -tuck };
			const float DRK[6] = { 1.0f, 0.95f, papery ? 0.95f : 0.6f, papery ? 0.95f : 0.8f, papery ? 0.85f : 0.55f, 0.1f };
			const int first = r_bark.verts.size();
			for (int ri = 0; ri < 6; ri++) {
				for (int j = 0; j < sides; j++) {
					const float ang = float(Math::TAU) * j / sides;
					const float wob = 1.0f + 0.16f * (moss_noise(Vector3(Math::cos(ang) * 1.3f, Math::sin(ang) * 1.3f, salt), w.salt) - 0.5f);
					const float rf = RING[ri] * wob;
					const float x = Math::cos(ang) * ex * rf, y = Math::sin(ang) * ey * rf;
					const int r0 = MAX(ri - 1, 0), r1 = MIN(ri + 1, 5);
					const float dh = (HGT[r1] - HGT[r0]) / MAX((RING[r1] - RING[r0]) * 0.5f * (ex + ey), 1e-4f);
					put(x, y, HGT[ri], Math::cos(ang) * dh * 0.7f, Math::sin(ang) * dh * 0.7f, DRK[ri]);
				}
			}
			quad_grid(first, 6, sides, true);
		}
		// The arms.
		const float arm = Math::abs(w.extra);
		for (int side = -1; side <= 1; side += 2) {
			RandomPCG rng;
			rng.seed(uint64_t(w.salt) * 2246822519u + uint64_t(side + 2) * 0x61726du);
			const float L = arm * Math::lerp(0.85f, 1.15f, rng.randf());
			const float w0 = papery ? CLAMP(1.4f * rb, 0.015f, 0.05f) : CLAMP(1.5f * rb, 0.018f, 0.045f);
			const float droop = Math::lerp(0.15f, 0.35f, rng.randf());
			float x = scar ? float(side) * 0.7f * ex : float(side) * 0.8f * rb;
			float y = scar ? -0.3f * ey : -0.9f * rb;
			const float step = 0.005f;
			const int n = CLAMP(int(L / step), 6, 120);
			const int first = r_bark.verts.size();
			const float crease = papery ? 0.0f : 1.0f;
			for (int i = 0; i <= n; i++) {
				const float t = float(i) / n;
				const float th = w.twist + droop * t + 0.12f * (moss_noise(Vector3(t * 3.0f, float(side), salt), w.salt) - 0.5f);
				const Vector2 d(float(side) * Math::cos(th), -Math::sin(th));
				const Vector2 pp(-d.y, d.x);
				// Tapering to a point; a birch's broad at the root, as a black triangle.
				const float hw = w0 * (1.0f - 0.88f * Math::pow(t, papery ? 0.8f : 1.6f)) * (0.85f + 0.3f * moss_noise(Vector3(t * 9.0f, float(side) * 2.0f, salt), w.salt + 3u));
				// Creases across the fold, a couple of centimetres apart: darker and lower in them.
				const float cr = crease * (0.5f + 0.5f * Math::sin(t * L / 0.018f * float(Math::TAU) + salt));
				const float h0 = fold * (1.0f - 0.35f * cr) * (1.0f - 0.5f * t);
				for (int k = 0; k < 7; k++) {
					const float c = float(k) / 3.0f - 1.0f;
					const float h = h0 * (1.0f - c * c) - tuck * c * c * c * c;
					const float dhdc = -2.0f * c * h0 - 4.0f * tuck * c * c * c;
					const Vector2 g = pp * (0.5f * dhdc / MAX(hw, 1e-3f));
					// Dark down the fold's middle, fading to the bark's own tone where it meets it;
					// darker in the creases.
					const float dk = (papery ? Math::smoothstep(0.95f, 0.4f, Math::abs(c)) * (1.0f - 0.25f * t) : Math::smoothstep(1.0f, 0.0f, Math::abs(c)) * (0.7f + 0.3f * cr) * (1.0f - 0.75f * Math::smoothstep(0.2f, 1.0f, t)));
					put(x + pp.x * c * hw, y + pp.y * c * hw, h, g.x, g.y, dk);
				}
				x += d.x * L / n;
				y += d.y * L / n;
			}
			quad_grid(first, n + 1, 7, false);
		}
		// Kept at the first LOD (a 2 cm part), dropped at the second.
		r_bark.end(part, 0.02f);
	}
	r_bark.uv2_map = keep_uv2;
	r_bark.role = keep_role;
}

// Resin (trees.md 5p): conifers flood a wound with resin, which wells from its edges and runs
// down the bark in drips that end in a drop, and sets in beads on the callus; amber while fresh,
// crusted whitish-yellow as its terpenes evaporate. Small tubes and beads laid on the stem's
// wounded surface (its highest point round each step, so they bridge the furrows).
void TreeGenerator::_grow_resin(Builder &r_resin) {
	if (wounds.is_empty() || !_wounds_resin() || wound_pts.size() < 2 || wound_surf.is_empty()) {
		return;
	}
	const int na = 96;
	const int ns = int(wound_surf.size()) / na;
	const float len = wound_arc[wound_arc.size() - 1];
	auto radius_at = [&](float p_s) {
		int i = 0;
		while (i + 2 < int(wound_arc.size()) && wound_arc[i + 1] < p_s) {
			i++;
		}
		const float t = CLAMP((p_s - wound_arc[i]) / MAX(wound_arc[i + 1] - wound_arc[i], 1e-6f), 0.0f, 1.0f);
		return Math::lerp(wound_rad[i], wound_rad[i + 1], t);
	};
	// The surface's radius at (s, az), bilinear over the bins: resin runs in the bark's furrows
	// and over its plates (the highest neighbour lifted runs off the furrows and kinked them
	// where the plates changed).
	auto surf_at = [&](float p_s, float p_az) {
		const float fs = CLAMP(p_s / 0.02f - 0.5f, 0.0f, float(ns - 1));
		const float fa = (p_az + float(Math::PI)) / float(Math::TAU) * na - 0.5f;
		const int s0 = int(fs), a0 = int(Math::floor(fa));
		const float ts = fs - s0, ta = fa - a0;
		float sum = 0.0f, wsum = 0.0f;
		for (int ds = 0; ds <= 1; ds++) {
			for (int da = 0; da <= 1; da++) {
				const float w = (ds ? ts : 1.0f - ts) * (da ? ta : 1.0f - ta);
				const float m = wound_surf[MIN(s0 + ds, ns - 1) * na + ((a0 + da) % na + na) % na];
				if (m > 0.0f && w > 0.0f) {
					sum += w * m;
					wsum += w;
				}
			}
		}
		return wsum > 0.0f ? sum / wsum : radius_at(p_s);
	};
	auto lin = [](float r, float g, float b) { return Color(r, g, b).srgb_to_linear(); };
	const Color fresh = lin(0.5f, 0.27f, 0.05f), old = lin(0.82f, 0.77f, 0.62f);
	// A drop: a squashed ball of rings, shared vertices, pointing down the stem.
	auto bead = [&](const Vector3 &p_c, const Vector3 &p_out, const Vector3 &p_axis, float p_r, const Color &p_col) {
		const int rings = 5, sides = 8;
		const int base = r_resin.verts.size();
		const Vector3 side = p_axis.cross(p_out).normalized();
		for (int i = 0; i <= rings; i++) {
			const float th = float(Math::PI) * float(i) / rings;
			for (int j = 0; j < sides; j++) {
				const float ph = float(Math::TAU) * float(j) / sides;
				// Flattened against the bark (half height outward), longer down the stem.
				const Vector3 n = p_axis * Math::cos(th) + (side * Math::cos(ph) + p_out * Math::sin(ph)) * Math::sin(th);
				const Vector3 q = p_axis * (Math::cos(th) * 1.3f * p_r) + side * (Math::cos(ph) * Math::sin(th) * p_r) + p_out * (Math::sin(ph) * Math::sin(th) * 0.6f * p_r);
				r_resin.verts.push_back(p_c + q);
				r_resin.normals.push_back(n.normalized());
				r_resin.uvs.push_back(Vector2(float(j) / sides, float(i) / rings));
				r_resin.colors.push_back(p_col);
			}
		}
		for (int i = 0; i < rings; i++) {
			for (int j = 0; j < sides; j++) {
				const int a = base + i * sides + j, b = base + i * sides + (j + 1) % sides;
				const int c = a + sides, d = b + sides;
				// Counter-clockwise from outside, as _tube's (the surface is flipped with the bark's).
				r_resin.indices.push_back(a);
				r_resin.indices.push_back(c);
				r_resin.indices.push_back(b);
				r_resin.indices.push_back(b);
				r_resin.indices.push_back(c);
				r_resin.indices.push_back(d);
			}
		}
	};
	for (int wi = 0; wi < int(wounds.size()); wi++) {
		const Wound &w = wounds[wi];
		if (w.kind == WOUND_FROST || w.kind == WOUND_CHECK || w.kind == WOUND_CHEVRON) {
			continue;
		}
		RandomPCG rng;
		rng.seed(uint64_t(w.salt) * 2246822519u + 0x7265736eu);
		const float flow = Math::lerp(0.4f, 1.0f, rng.randf());
		// Edge points: from points inside the wound, outward round the stem (a scar's sides) or
		// down it (a stripped patch's foot), to where the bark starts.
		const float lo = w.kind == WOUND_SQUIRREL ? w.s0 - w.s1 : w.s0;
		const float hi = MIN(w.kind == WOUND_SQUIRREL ? w.s0 + w.s1 : w.s1, len - 0.1f);
		const int tries = CLAMP(int((hi - lo) / 0.025f), 4, 320);
		for (int t = 0; t < tries; t++) {
			const float s = Math::lerp(lo, hi, rng.randf());
			const float r = radius_at(s);
			const float az_c = w.kind == WOUND_LIGHTNING ? w.az0 + w.twist * (s - w.s0) : w.az0;
			if (_wound_at(s, az_c, r, wi).y < 0.5f) {
				continue;
			}
			// March out: round the stem either way, or down the stem for a patch's foot.
			const int dir = rng.randf() < 0.5f ? -1 : 1;
			const bool down = w.kind != WOUND_LIGHTNING && rng.randf() < 0.45f;
			float es = s, ea = az_c;
			bool found = false;
			for (int k = 1; k < 120; k++) {
				const float ts = down ? s - 0.004f * k : s;
				const float ta = down ? az_c : az_c + dir * 0.004f * k / MAX(r, 0.01f);
				if (_wound_at(ts, ta, r, wi).y < 0.5f) {
					es = ts;
					ea = ta;
					found = true;
					break;
				}
			}
			if (!found || es < 0.15f) {
				continue;
			}
			const float a = Math::smoothstep(0.0f, 12.0f, w.age) * rng.randf();
			const Color col = fresh.lerp(old, CLAMP(0.25f + 0.75f * a, 0.0f, 1.0f));
			if (rng.randf() < 0.55f * flow) {
				// A run: from the edge down the bark, wandering a little, thickening to its drop.
				const float run = Math::lerp(0.03f, 0.3f, rng.randf() * rng.randf()) * flow;
				const float rm = Math::lerp(0.003f, 0.008f, rng.randf());
				LocalVector<Vector3> rp;
				LocalVector<float> rr;
				const int steps = MAX(3, int(run / 0.01f));
				Vector3 out, axis;
				const float wob = rng.randf() * 10.0f;
				// The surface under the run, smoothed along it: resin bridges the furrows it
				// crosses and fills them (following each bin, it kinked at every plate edge).
				LocalVector<float> sf, sa;
				for (int k = 0; k <= steps; k++) {
					const float f = float(k) / steps;
					const float ss = es - run * f;
					const float aa = ea + 0.002f * Math::sin(wob + f * 7.0f) / MAX(r, 0.01f);
					sf.push_back(surf_at(ss, aa));
					sa.push_back(aa);
				}
				for (int k = 0; k <= steps; k++) {
					float m = 0.0f, sum = 0.0f;
					int c = 0;
					for (int j = MAX(0, k - 3); j <= MIN(steps, k + 3); j++) {
						m = MAX(m, sf[j]);
						sum += sf[j];
						c++;
					}
					const float f = float(k) / steps;
					const float rk = rm * (0.55f + 0.45f * f);
					const float sr = Math::lerp(sum / c, m, 0.85f);
					// A little sunk at the plate tops it bridges: a run is a smear spread on the bark.
					rp.push_back(stem_point(wound_pts, wound_arc, es - run * f, sa[k], sr + 0.1f * rk, out, axis));
					rr.push_back(rk);
				}
				_tube(r_resin, rp, rr, 6, 0.0f, col, nullptr, 0.0f, -1.0f, false);
				// The drop at its end, on the run's last point (out and axis are that point's).
				bead(rp[rp.size() - 1] + out * (0.3f * rm), out, axis, rm * 1.5f, col);
			} else if (rng.randf() < 0.6f) {
				// A bead set on the callus at the edge.
				const float rb = Math::lerp(0.002f, 0.007f, rng.randf());
				Vector3 out, axis;
				const Vector3 c = stem_point(wound_pts, wound_arc, es, ea, surf_at(es, ea) + 0.3f * rb, out, axis);
				bead(c, out, axis, rb, col);
			}
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
		const float circ = Math::TAU * MAX(rad[0], 0.001f) / MAX(1.0f, Math::round(Math::TAU * MAX(rad[0], 0.001f) / _bark_tile()));
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
// Lentisk's leaflets are entire and elliptic with a short point (Flora iberica): its own cut.
Ref<Texture2D> TreeGenerator::_leaflet_texture() {
	const bool entire = preset == PRESET_LENTISK;
	Ref<Texture2D> &cached = entire ? leaflet_entire_texture : leaflet_texture;
	if (cached.is_valid()) {
		return cached;
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
			const float half = entire ? Math::pow(Math::sin(float(Math::PI) * Math::pow(v, 0.9f)), 0.75f) * (1.0f - 0.15f * Math::smoothstep(0.85f, 1.0f, v)) : Math::sin(float(Math::PI) * Math::pow(v, 0.85f)) * (1.0f - 0.25f * Math::smoothstep(0.6f, 1.0f, v));
			// Teeth on the upper three quarters, about 22 a side, a tenth of the half-width deep.
			const float s = v * 22.0f - Math::floor(v * 22.0f);
			const float teeth = entire ? 0.0f : Math::smoothstep(0.12f, 0.3f, v) * (1.0f - Math::smoothstep(0.93f, 1.0f, v));
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
	cached = ImageTexture::create_from_image(img);
	return cached;
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
	if (phyllotaxis == PHYLLOTAXIS_WHORLED || params[PARAM_NEEDLES] > 0.5f || preset == PRESET_DWARF_FAN_PALM) {
		return; // No spurs on a palm's petioles: its fans are _palm_geometry.
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
	// Fallen twigs and small branches lie in the litter's damp and stain brown as they rot,
	// where standing dead wood bleaches grey in the sun and wind.
	static const Color LITTER_STAINED[6] = { Color(), Color(0.31f, 0.25f, 0.2f), Color(0.33f, 0.27f, 0.21f), Color(0.35f, 0.29f, 0.22f),
		Color(0.35f, 0.28f, 0.2f), Color(0.3f, 0.24f, 0.17f) };
	if (fallen_branch) {
		bark_color = bark_color.lerp(LITTER_STAINED[decay_class], 0.7f);
	} else if (_bark_kind(preset) == BARK_SMOOTH && decay_class <= 3) {
		// Smooth bark (beech, hornbeam) stays silver-grey on dead wood until it sloughs; browned
		// toward the general weathered tone it read olive under warm light (docs, 16 Nov).
		bark_color = bark_color.lerp(Color(0.44f, 0.44f, 0.43f), 0.5f).lerp(WEATHERED[decay_class], 0.15f);
	} else {
		bark_color = bark_color.lerp(WEATHERED[decay_class], decay_class >= 3 && bark_peel <= 0.0f ? 0.85f : 0.55f);
	}
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

// The thickest stem off the root node: the bole.
int TreeGenerator::_main_stem() const {
	int stem = -1;
	for (int i = 1; i < int(nodes.size()); i++) {
		const Node &n = nodes[i];
		if (!n.removed && n.parent == 0 && (stem < 0 || n.radius > nodes[stem].radius)) {
			stem = i;
		}
	}
	return stem;
}

// A felled stem is not a rigid rod. Its natural sweep and the crown's upturn left the top of a
// log standing up off the ground, and a log thrown with its root plate bridged from the plate to
// its top. Wood bends and cracks under its own weight where it is unsupported, the crown's limbs
// break under it, and a rotting log sags into the soil until it lies along it (Harmon et al.
// 1986; Fraver et al. 2002, decay classes by contact and collapse). Walking out along the bole,
// each internode that ends off the ground (or in it) turns about its start, with all the wood
// beyond it, toward resting on the ground, by at most a few degrees per internode while sound
// (a gentle sag or a crack) and more once soft.
void TreeGenerator::_settle_log(float p_ground, float p_trunk_r) {
	static const float SINK[6] = { 0.15f, 0.15f, 0.15f, 0.25f, 0.35f, 0.45f };
	static const float BEND[6] = { 4.0f, 5.0f, 7.0f, 12.0f, 20.0f, 30.0f };
	const float sink = SINK[CLAMP(decay_class, 0, 5)];
	const float max_bend = Math::deg_to_rad(BEND[CLAMP(decay_class, 0, 5)]);
	LocalVector<bool> sub;
	sub.resize(nodes.size());
	bool touched = false;
	for (int k = _main_stem(); k >= 0 && !nodes[k].removed; k = nodes[k].main) {
		Node &n = nodes[k];
		const float r_end = (n.main >= 0 && !nodes[n.main].removed) ? nodes[n.main].radius : 0.6f * n.radius;
		const float want = p_ground + r_end * (1.0f - sink);
		const float h = n.end().y - want;
		// Off the plate: the butt end stays up where the plate holds it, the bole only bends
		// down once it has come to the ground, or where it already lies low.
		if (!touched) {
			touched = n.start.y - (p_ground + n.radius * (1.0f - sink)) < 0.5f * p_trunk_r + 0.05f;
			if (!touched) {
				continue;
			}
		}
		if (Math::abs(h) < 0.01f || n.length < 1e-3f) {
			continue;
		}
		Vector3 bend_axis = n.dir.cross(Vector3(0, 1, 0));
		if (bend_axis.length_squared() < 1e-6f) {
			continue;
		}
		bend_axis.normalize();
		// Turning about n x up by +a lowers a forward-pointing end; solve for the angle that
		// brings the end to the wanted height, within the wood's give.
		const float cur = Math::asin(CLAMP(n.dir.y, -1.0f, 1.0f));
		const float goal = Math::asin(CLAMP((want - n.start.y) / n.length, -1.0f, 1.0f));
		const float a = CLAMP(cur - goal, -max_bend, max_bend);
		// (A positive turn about dir x up lifts the end.)
		const Basis rb(bend_axis, -a);
		const Vector3 pivot = n.start;
		for (int i = 0; i < int(nodes.size()); i++) {
			sub[i] = !nodes[i].removed && (i == k || (i > k && nodes[i].parent >= 0 && sub[nodes[i].parent]));
			if (sub[i]) {
				nodes[i].start = pivot + rb.xform(nodes[i].start - pivot);
				nodes[i].dir = rb.xform(nodes[i].dir).normalized();
			}
		}
	}
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
		// Where the stem comes down: a long stem does not bridge from the plate to its top. The
		// crown takes the fall, its limbs and often the top snapping, and the bole touches down
		// about two fifths to three fifths of the way out, sooner once it has sagged and rotted (Harmon
		// et al. 1986: most logs lie in contact with the ground for much of their length). The
		// rest of the stem is laid along the ground below (_settle_log()).
		Vector3 tip = base;
		float tip_r = trunk_r;
		{
			float total = 0.0f;
			for (int k = _main_stem(); k >= 0 && !nodes[k].removed; k = nodes[k].main) {
				total += nodes[k].length;
			}
			const float touch = total * Math::lerp(0.42f, 0.6f, fr.randf()) * (decay_class >= 3 ? 0.8f : 1.0f);
			float run = 0.0f;
			for (int k = _main_stem(); k >= 0 && !nodes[k].removed; k = nodes[k].main) {
				run += nodes[k].length;
				tip = nodes[k].end();
				tip_r = nodes[k].radius;
				if (run >= touch) {
					break;
				}
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
	_settle_log(base.y, trunk_r);
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
	// Sticks: fine woody debris breaks into pieces when it falls and again as it lies, most of
	// them well under a metre and a half (Harmon et al. 1986; Woodall and Monleon 2008, FIA fine
	// woody debris tallies by length), so a stick is a piece of a twig or small branch, not the
	// whole shoot.
	const bool stick = shed_radius_max <= 0.05f;
	const float stick_u = br.randf();
	const float stick_len = 0.35f + 1.25f * stick_u * stick_u;
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
		const float d = Math::abs(Math::log(MAX(n.radius, 0.002f) / target)) - 0.15f * MIN(chain, stick ? stick_len : 4.0f) + 0.05f * br.randf();
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
	if (stick) {
		// Snapped to its length: the axis past it gone, a fracture at the break (trees.md 5j).
		float run = 0.0f;
		for (int k = pick; k >= 0 && !nodes[k].removed; k = nodes[k].main) {
			run += nodes[k].length;
			const int next = nodes[k].main;
			if (run >= stick_len && next >= 0) {
				LocalVector<bool> cut;
				cut.resize(nodes.size());
				for (int i = 0; i < int(nodes.size()); i++) {
					cut[i] = i == next || (i > next && nodes[i].parent >= 0 && cut[nodes[i].parent]);
					if (cut[i]) {
						keep[i] = false;
					}
				}
				broken_ends.insert(k);
				break;
			}
		}
		// Twigs grow in a zigzag from bud to bud and dead wood keeps it: kinked a few degrees at
		// every node, more on thin wood, instead of a smooth curve. Each kink turns the rest of
		// the axis and its side shoots about the node.
		for (int k = nodes[pick].main; k >= 0 && keep[k]; k = nodes[k].main) {
			const Vector3 pivot = nodes[k].start;
			Vector3 side = nodes[k].dir.cross(Vector3(0, 1, 0));
			if (side.length_squared() < 1e-6f) {
				side = nodes[k].dir.cross(Vector3(1, 0, 0));
			}
			side.normalize();
			const Vector3 kink_axis = Basis(nodes[k].dir.normalized(), br.randf() * float(Math::TAU)).xform(side).normalized();
			const float angle = Math::deg_to_rad(Math::lerp(6.0f, 16.0f, br.randf()) * CLAMP(0.012f / MAX(nodes[k].radius, 0.003f), 0.6f, 1.6f));
			const Basis kb(kink_axis, angle);
			LocalVector<bool> sub;
			sub.resize(nodes.size());
			for (int i = 0; i < int(nodes.size()); i++) {
				sub[i] = keep[i] && (i == k || (i > k && nodes[i].parent >= 0 && sub[nodes[i].parent]));
				if (sub[i]) {
					nodes[i].start = pivot + kb.xform(nodes[i].start - pivot);
					nodes[i].dir = kb.xform(nodes[i].dir);
				}
			}
		}
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
		// A bent or kinked axis also rolls until its bend lies flat: the spread of its points in
		// height (an axis bowed upward stood one end in the air even after tipping).
		float axis_len = 0.0f;
		for (int a = pick; a >= 0 && keep[a]; a = nodes[a].main) {
			axis_len += nodes[a].length;
		}
		for (int k = 0; k < 24; k++) {
			const float roll = roll0 + float(k) * float(Math::TAU) / 24.0f;
			const Basis t = Basis(level, roll) * turn;
			float rise = 0.0f;
			for (int i = pick + 1; i < int(nodes.size()); i++) {
				if (keep[i] && !nodes[i].removed) {
					rise += Math::abs(t.xform(nodes[i].dir).y) * nodes[i].length * nodes[i].radius;
				}
			}
			float lo = 1e30f, hi = -1e30f;
			for (int a = pick; a >= 0 && keep[a]; a = nodes[a].main) {
				const float y = t.xform(nodes[a].end() - base).y;
				lo = MIN(lo, y);
				hi = MAX(hi, y);
			}
			rise += 3.0f * (hi - lo) * axis_len * nodes[pick].radius;
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
	// Tip it about the level line to rest on two points: a bent limb laid along its mean
	// direction balances on its lowest point with an end in the air; it rocks until the hull of
	// its underside meets the ground, so the gap under the axis is least.
	{
		const Vector3 pitch_axis = level.cross(Vector3(0, 1, 0)).normalized();
		float best_pitch = 0.0f;
		float best_gap = 1e30f;
		for (int k = -30; k <= 30; k++) {
			const Basis pb(pitch_axis, Math::deg_to_rad(float(k)));
			float low = 1e30f;
			float gap = 0.0f;
			float total = 0.0f;
			for (int pass = 0; pass < 2; pass++) {
				for (int a = pick; a >= 0 && !nodes[a].removed; a = nodes[a].main) {
					const Node &n = nodes[a];
					const float y0 = pb.xform(n.start - ground).y - n.radius;
					const float y1 = pb.xform(n.end() - ground).y - ((n.main >= 0 && !nodes[n.main].removed) ? nodes[n.main].radius : 0.5f * n.radius);
					if (pass == 0) {
						low = MIN(low, MIN(y0, y1));
					} else {
						gap += (0.5f * (y0 + y1) - low) * n.length;
						total += n.length;
					}
				}
			}
			gap /= MAX(total, 1e-4f);
			if (gap < best_gap - 1e-5f) {
				best_gap = gap;
				best_pitch = float(k);
			}
		}
		if (best_pitch != 0.0f) {
			const Basis pb(pitch_axis, Math::deg_to_rad(best_pitch));
			for (int i = pick; i < int(nodes.size()); i++) {
				if (keep[i]) {
					nodes[i].start = ground + pb.xform(nodes[i].start - ground);
					nodes[i].dir = pb.xform(nodes[i].dir);
				}
			}
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
		// The tube tapers along the internode to the next one's radius (or to a tip).
		const float r_end = (n.main >= 0 && !nodes[n.main].removed) ? nodes[n.main].radius : 0.5f * n.radius;
		lift = MAX(lift, MAX(ground.y + n.radius * 0.85f - n.start.y, ground.y + r_end * 0.85f - n.end().y));
	}
	for (int i = pick; i < int(nodes.size()); i++) {
		if (keep[i]) {
			nodes[i].start.y += lift;
		}
	}
	// Side branches pointing into the ground broke off when it fell, and so did those standing
	// high: a dead lateral more than a few limb diameters up snaps on impact or soon after.
	const float stand_max = ground.y + MAX(stick ? 0.15f : 0.6f, 8.0f * nodes[pick].radius);
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
// The thinnest bark tube a far mesh keeps when its near mesh is split off (trees.md 5x.2): leaf
// twigs under this radius are near only.
static constexpr float FAR_TWIG_RADIUS = 0.001f;

// The stone (trees.md 5q): a RockGenerator boulder when the rocks module is built in (looked up
// by name, so vegetation does not depend on it), glacial family: subrounded, its faces and
// fractures real geometry. Fitted to the planned half extents and kept as vertices for the
// roots' drape: the top of the actual rock on a grid, so roots lie on its faces and never cut
// through a corner an ellipsoid would miss.
void TreeGenerator::_plan_root_stone_rock() {
	stone.verts.clear();
	stone.normals.clear();
	stone.cavity.clear();
	stone.indices.clear();
	stone.top.clear();
	stone.gn = 0;
	if (!ClassDB::class_exists(SNAME("RockGenerator"))) {
		return;
	}
	Object *obj = ClassDB::instantiate(SNAME("RockGenerator"));
	if (!obj) {
		return;
	}
	obj->set(SNAME("family"), 2); // FAMILY_GLACIAL
	obj->set(SNAME("seed"), int(uint32_t(seed) * 2654435761u >> 1));
	obj->set(SNAME("lods"), false);
	// The family's base grid (96 cells) at 128: fractures and grain as geometry at a root's
	// scale. Close-up (3 mm cells) cost 5 s and 230k triangles a stone, drawn at every distance.
	obj->set(SNAME("close_up"), false);
	obj->set(SNAME("resolution"), 128);
	Ref<ArrayMesh> rock = obj->call(SNAME("generate"));
	memdelete(obj);
	if (rock.is_null() || rock->get_surface_count() == 0) {
		return;
	}
	const Array a = rock->surface_get_arrays(0);
	const PackedVector3Array v = a[Mesh::ARRAY_VERTEX];
	const PackedVector3Array n = a[Mesh::ARRAY_NORMAL];
	const PackedColorArray col = a[Mesh::ARRAY_COLOR];
	const PackedInt32Array idx = a[Mesh::ARRAY_INDEX];
	if (v.is_empty() || idx.is_empty() || n.size() != v.size()) {
		return;
	}
	AABB box(v[0], Vector3());
	for (const Vector3 &p : v) {
		box.expand_to(p);
	}
	// The rock's longest axis along x, its shortest up, as the planned extents.
	Vector3 sz = box.size;
	int ax[3] = { 0, 1, 2 };
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2 - i; j++) {
			if (sz[ax[j]] < sz[ax[j + 1]]) {
				SWAP(ax[j], ax[j + 1]);
			}
		}
	}
	// Largest -> x, middle -> z, smallest -> y.
	const int from_x = ax[0], from_z = ax[1], from_y = ax[2];
	const Vector3 mid = box.get_center();
	const Vector3 s(stone.r.x / MAX(0.5f * sz[from_x], 1e-4f), stone.r.y / MAX(0.5f * sz[from_y], 1e-4f), stone.r.z / MAX(0.5f * sz[from_z], 1e-4f));
	// An odd permutation of the axes mirrors the rock: flip z then, so the winding holds.
	const bool odd = (from_x == 0 && from_y == 2) || (from_x == 2 && from_y == 1) || (from_x == 1 && from_y == 0);
	const float zf = odd ? -1.0f : 1.0f;
	const int nv = v.size();
	stone.verts.resize(nv);
	stone.normals.resize(nv);
	stone.cavity.resize(nv);
	for (int i = 0; i < nv; i++) {
		const Vector3 q = v[i] - mid;
		const Vector3 m(q[from_x], q[from_y], zf * q[from_z]);
		stone.verts.write[i] = stone.c + m * s;
		const Vector3 nn(n[i][from_x], n[i][from_y], zf * n[i][from_z]);
		stone.normals.write[i] = Vector3(nn.x / s.x, nn.y / s.y, nn.z / s.z).normalized();
		stone.cavity.write[i] = i < col.size() ? col[i].r : 1.0f;
	}
	// The soil builder winds counter-clockwise seen from outside (flipped for Godot later): by the
	// signed volume, whichever way the rock came wound.
	double vol = 0.0;
	for (int i = 0; i + 2 < idx.size(); i += 3) {
		const Vector3 a0 = stone.verts[idx[i]] - stone.c, a1 = stone.verts[idx[i + 1]] - stone.c, a2 = stone.verts[idx[i + 2]] - stone.c;
		vol += a0.dot(a1.cross(a2));
	}
	const bool swap = vol < 0.0;
	stone.indices.resize(idx.size());
	for (int i = 0; i + 2 < idx.size(); i += 3) {
		stone.indices.write[i] = idx[i];
		stone.indices.write[i + 1] = idx[swap ? i + 2 : i + 1];
		stone.indices.write[i + 2] = idx[swap ? i + 1 : i + 2];
	}
	// The top above the ground on a grid, then one cell of dilation so a root crossing a cell
	// edge never dips into a face.
	const int gn = 32;
	LocalVector<float> t;
	t.resize(gn * gn);
	for (float &x : t) {
		x = 0.0f;
	}
	for (int i = 0; i < nv; i++) {
		const Vector3 &p = stone.verts[i];
		const int gx = CLAMP(int((p.x - stone.c.x + stone.r.x) / (2.0f * stone.r.x) * gn), 0, gn - 1);
		const int gz = CLAMP(int((p.z - stone.c.z + stone.r.z) / (2.0f * stone.r.z) * gn), 0, gn - 1);
		t[gz * gn + gx] = MAX(t[gz * gn + gx], p.y - stone.ground);
	}
	stone.top.resize(gn * gn);
	for (int z = 0; z < gn; z++) {
		for (int x = 0; x < gn; x++) {
			float m = 0.0f;
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int xx = x + dx, zz = z + dz;
					if (xx >= 0 && zz >= 0 && xx < gn && zz < gn) {
						m = MAX(m, t[zz * gn + xx]);
					}
				}
			}
			stone.top[z * gn + x] = m;
		}
	}
	stone.gn = gn;
}

// The stone's top above the ground at (x, z): the rock's grid when there is one (bilinear on
// cell centres), else the planned ellipsoid.
float TreeGenerator::_root_stone_top(float p_x, float p_z) const {
	const float dx = (p_x - stone.c.x) / stone.r.x, dz = (p_z - stone.c.z) / stone.r.z;
	if (stone.gn > 0) {
		const int gn = stone.gn;
		const float fx = (dx * 0.5f + 0.5f) * gn - 0.5f, fz = (dz * 0.5f + 0.5f) * gn - 0.5f;
		if (fx < -1.0f || fz < -1.0f || fx > gn || fz > gn) {
			return 0.0f;
		}
		const int x0 = int(Math::floor(fx)), z0 = int(Math::floor(fz));
		const float ux = fx - x0, uz = fz - z0;
		auto at = [&](int x, int z) -> float {
			return (x < 0 || z < 0 || x >= gn || z >= gn) ? 0.0f : stone.top[z * gn + x];
		};
		const float a = Math::lerp(at(x0, z0), at(x0 + 1, z0), ux);
		const float b = Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), ux);
		return Math::lerp(a, b, uz);
	}
	const float q = dx * dx + dz * dz;
	if (q >= 1.0f) {
		return 0.0f;
	}
	return MAX(stone.c.y + stone.r.y * Math::sqrt(1.0f - q) - stone.ground, 0.0f);
}

void TreeGenerator::_build_root_stone(Builder &r_b) const {
	Vector4i part;
	r_b.begin(part);
	if (!stone.verts.is_empty()) {
		const int first = r_b.verts.size();
		const uint32_t ls = uint32_t(seed) * 2654435761u + 0x73746f6eu;
		// Weathered granite grey (darker than the bench's first pale ellipsoid), the hollows
		// darker, pale lichen crusts on the upper faces, soil stain at the foot.
		const Color base = Color(0.33f, 0.32f, 0.3f).srgb_to_linear();
		const Color soil_c = Color(0.27f, 0.22f, 0.17f).srgb_to_linear();
		const Color lichen_c = Color(0.62f, 0.63f, 0.55f).srgb_to_linear();
		for (int i = 0; i < stone.verts.size(); i++) {
			const Vector3 &p = stone.verts[i];
			const Vector3 &nn = stone.normals[i];
			const float mott = 0.8f + 0.4f * moss_noise(p * 3.0f, ls) + 0.15f * (moss_noise(p * 17.0f, ls + 5u) - 0.5f);
			const float cav = 0.55f + 0.45f * CLAMP(stone.cavity[i], 0.0f, 1.0f);
			Color c = base;
			c = Color(c.r * mott * cav, c.g * mott * cav, c.b * mott * cav);
			const float lich = Math::smoothstep(0.58f, 0.7f, moss_noise(p * 7.0f, ls + 21u)) * Math::smoothstep(0.0f, 0.5f, nn.y);
			c = c.lerp(lichen_c * cav, 0.75f * lich);
			c = c.lerp(soil_c, 1.0f - Math::smoothstep(stone.ground - 0.02f, stone.ground + 0.07f, p.y));
			r_b.verts.push_back(p);
			r_b.normals.push_back(nn);
			r_b.uvs.push_back(Vector2(p.x, p.z) * 4.0f);
			r_b.colors.push_back(c);
		}
		for (int i = 0; i < stone.indices.size(); i++) {
			r_b.indices.push_back(first + stone.indices[i]);
		}
		r_b.end(part, stone.r.x);
		return;
	}
	const int rings = 22, segs = 44;
	const int first = r_b.verts.size();
	const uint32_t ls = uint32_t(seed) * 2654435761u + 0x73746f6eu;
	const Color base = Color(0.4f, 0.39f, 0.36f).srgb_to_linear();
	for (int i = 0; i <= rings; i++) {
		const float th = float(Math::PI) * float(i) / rings;
		for (int j = 0; j < segs; j++) {
			const float ph = float(Math::TAU) * float(j) / segs;
			const Vector3 u(Math::sin(th) * Math::cos(ph), Math::cos(th), Math::sin(th) * Math::sin(ph));
			const float nz = moss_noise(u * 1.6f + Vector3(3.1f, 7.7f, 1.3f), ls);
			const float nz2 = moss_noise(u * 4.3f + Vector3(5.3f, 2.1f, 9.4f), ls + 7u);
			const float nz3 = moss_noise(u * 11.0f + Vector3(1.7f, 4.2f, 8.8f), ls + 13u);
			float j_amt = (nz - 0.5f) + 0.45f * (nz2 - 0.5f) + 0.15f * (nz3 - 0.5f);
			// Folded toward a few planes: a weathered block's faces and edges, not a pebble.
			j_amt = Math::lerp(j_amt, Math::round(j_amt * 3.0f) / 3.0f, 0.65f);
			const Vector3 lp = stone.c + u * stone.r * (1.0f + 0.5f * j_amt);
			// Grey with darker weathering and pale lichen crusts in patches.
			const float lich = Math::smoothstep(0.6f, 0.72f, moss_noise(lp * 9.0f, ls + 21u)) * Math::smoothstep(-0.2f, 0.4f, u.y);
			const float shade = (0.78f + 0.22f * u.y) * (0.7f + 0.45f * nz2) * (0.85f + 0.25f * nz3) * (1.0f + 0.35f * lich);
			// Lichen-grey above, soil-stained low down.
			Color c = base.lerp(Color(0.3f, 0.25f, 0.19f).srgb_to_linear(), 1.0f - Math::smoothstep(stone.ground - 0.02f, stone.ground + 0.08f, lp.y));
			c = Color(c.r * shade, c.g * shade, c.b * shade);
			r_b.verts.push_back(lp);
			r_b.normals.push_back(u);
			r_b.uvs.push_back(Vector2(lp.x, lp.z) * 4.0f);
			r_b.colors.push_back(c);
		}
	}
	for (int i = 0; i < rings; i++) {
		for (int j = 0; j < segs; j++) {
			const int a0 = first + i * segs + j, a1 = first + i * segs + (j + 1) % segs;
			const int b0 = a0 + segs, b1 = a1 + segs;
			// Counter-clockwise seen from outside, as the root plate (flipped for Godot with it).
			auto tri = [&](int a, int b, int c) {
				const Vector3 n = (r_b.verts[b] - r_b.verts[a]).cross(r_b.verts[c] - r_b.verts[a]);
				if (n.dot(r_b.verts[a] - stone.c) < 0.0f) {
					SWAP(b, c);
				}
				r_b.indices.push_back(a);
				r_b.indices.push_back(b);
				r_b.indices.push_back(c);
			};
			if (i > 0) {
				tri(a0, b0, a1);
			}
			if (i + 1 < rings) {
				tri(a1, b0, b1);
			}
		}
	}
	r_b.end(part, stone.r.x);
}

// A tree on a bank (trees.md 5q): the ground under it falls toward +Z at ground_slope. The root
// collar and roots are built for level ground and then sheared onto the slope near the soil
// (fading out over the lower stem), so the downhill roots run down the face and the uphill ones
// into it; the stem's foot swells downhill as on real slopes. bank_erosion lifts the downhill
// roots off the slope in an arch, fixed at the stem and at their tips: the soil washed out from
// under them (Gartner 2007, exposed roots on eroding banks). Normals and tangents follow the
// shear's Jacobian.
void TreeGenerator::_bank_shear(Builder &r_b, bool p_drape) const {
	const bool drape = p_drape && stone.on;
	if ((ground_slope <= 0.0f && !drape) || fallen || fallen_branch || nodes.is_empty() || r_b.verts.is_empty()) {
		return;
	}
	const float ts = Math::tan(ground_slope);
	const Vector3 ax = nodes[0].start;
	const float tr = MAX(foot_roots.trunk_r, 0.05f);
	float reach = tr * 3.0f;
	if (foot_roots.on) {
		for (const float l : foot_roots.len) {
			reach = MAX(reach, tr + l + 0.5f * tr);
		}
	}
	const float top = tr * 1.5f;
	// Roots over the stone (trees.md 5q): near the soil every surface is lifted by the stone's
	// height above the ground there, so roots and the stem's foot lie over it as roots grow over
	// rock (thickening against it, never through it).
	auto lift = [&](const Vector3 &p) -> float {
		if (!drape) {
			return 0.0f;
		}
		const float top = _root_stone_top(p.x, p.z);
		if (top <= 0.0f) {
			return 0.0f;
		}
		const float hh = p.y - stone.ground;
		const float w = 1.0f - Math::smoothstep(stone.root_top, stone.root_top * 2.5f + 0.1f, hh);
		return MAX(top, 0.0f) * w;
	};
	auto shear = [&](const Vector3 &p) -> float {
		if (ground_slope <= 0.0f) {
			return 0.0f;
		}
		const float hh = p.y - ax.y;
		const float w = 1.0f - Math::smoothstep(top, top + 3.0f * tr + 0.3f, hh);
		if (w <= 0.0f) {
			return 0.0f;
		}
		const float dz = p.z - ax.z;
		float dy = -ts * dz;
		if (dz > 0.0f && bank_erosion > 0.0f) {
			const float d = Vector2(p.x - ax.x, dz).length();
			const float a = CLAMP((d - tr) / MAX(reach - tr, 0.05f), 0.0f, 1.0f);
			const float arch = 4.0f * a * (1.0f - a);
			dy *= 1.0f - bank_erosion * 0.6f * arch * Math::smoothstep(0.0f, 0.5f, dz / MAX(d, 1e-3f));
		}
		return dy * w;
	};
	auto disp = [&](const Vector3 &p) -> float {
		const float l = lift(p);
		return l + shear(p + Vector3(0.0f, l, 0.0f));
	};
	const int nv = r_b.verts.size();
	Vector3 *vw = r_b.verts.ptrw();
	Vector3 *nw = r_b.normals.size() == nv ? r_b.normals.ptrw() : nullptr;
	float *tw = (r_b.use_tangents && r_b.tangents.size() >= nv * 4) ? r_b.tangents.ptrw() : nullptr;
	const float e = 0.005f;
	for (int v = 0; v < nv; v++) {
		const Vector3 p = vw[v];
		const float dy = disp(p);
		if (dy == 0.0f) {
			continue;
		}
		const float gx = (disp(p + Vector3(e, 0, 0)) - disp(p - Vector3(e, 0, 0))) / (2.0f * e);
		const float gy = (disp(p + Vector3(0, e, 0)) - disp(p - Vector3(0, e, 0))) / (2.0f * e);
		const float gz = (disp(p + Vector3(0, 0, e)) - disp(p - Vector3(0, 0, e))) / (2.0f * e);
		vw[v].y += dy;
		if (nw) {
			const Vector3 n = nw[v];
			const float my = n.y / MAX(1.0f + gy, 0.05f);
			const Vector3 m(n.x - gx * my, my, n.z - gz * my);
			if (m.length_squared() > 1e-12f) {
				nw[v] = m.normalized();
			}
		}
		if (tw) {
			tw[v * 4 + 1] += gx * tw[v * 4] + gy * tw[v * 4 + 1] + gz * tw[v * 4 + 2];
			Vector3 t(tw[v * 4], tw[v * 4 + 1], tw[v * 4 + 2]);
			if (nw) {
				t -= nw[v] * nw[v].dot(t);
			}
			if (t.length_squared() > 1e-12f) {
				t.normalize();
				tw[v * 4] = t.x;
				tw[v * 4 + 1] = t.y;
				tw[v * 4 + 2] = t.z;
			}
		}
	}
}

Ref<ArrayMesh> TreeGenerator::_build_mesh() {
	struct TileGuard {
		~TileGuard() {
			tube_tile = BARK_TILE;
			tube_fade_r = 0.0f;
			tube_fade_r0 = 0.0f;
			tube_crotch.active = false;
			tube_crotch.stems.clear();
			tube_shrink.active = false;
		}
	} tile_guard;
	tube_tile = _bark_tile();
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
		const float repeats = MAX(1.0f, Math::round(girth / _bark_tile()));
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
			float t = 0.85f * CLAMP((w - 0.15f) / 0.4f, 0.0f, 1.0f);
			if (t > 0.0f) {
				// Ahead of the cushions the film is algae and protonema in drifts a hand to a
				// forearm across, the bark's silver-grey between them; an even tint over the whole
				// moss weight read as an olive wall on a lying beech at 40 cm (docs, 16 Nov).
				const float drift = Math::smoothstep(0.42f, 0.62f, moss_noise(r_b.verts[v] * 6.0f, 0x66696c6du));
				// The shell shows from about 0.47: a few centimetres of full tint at its edge hide the
				// edge's grid (and moss creeps ahead of a cushion so); wider, it read as the wall.
				t *= Math::lerp(0.1f + 0.75f * drift, 1.0f, Math::smoothstep(0.41f, 0.47f, w));
				r_b.colors.write[v] = r_b.colors[v].lerp(moss_green, t);
			}
		}
	};
	// Bark stories (trees.md 5p): the main stem's wounds, applied to each of its tubes as they are
	// built. Pushed out (callus, frost ribs) or in (a split) along the stem's radius, the normal
	// tilted by the same field's slope; the bare wood goes to _peel_bark. Moss holds the bark under
	// it, as on dead wood.
	_plan_wounds();
	_plan_chevrons();
	wound_pts.clear();
	wound_vtx.clear();
	wound_rad.clear();
	wound_arc.clear();
	wound_surf.clear();
	const int wound_stem = wounds.is_empty() ? -1 : ((fallen || fallen_branch) && deadwood_stem >= 0 ? deadwood_stem : _main_stem());
	deadwood_features.clear();
	auto wound_apply = [&](Builder &r_b, const Vector4i &p_part, bool p_record) {
		if (wounds.is_empty() || wound_pts.size() < 2) {
			return;
		}
		const int nv = r_b.verts.size();
		if (int(r_b.wound.size()) < nv) {
			const int from = r_b.wound.size();
			r_b.wound.resize(nv);
			for (int v = from; v < nv; v++) {
				r_b.wound[v] = 0.0f;
			}
		}
		Vector3 *vw = r_b.verts.ptrw();
		Vector3 *nw = r_b.normals.ptrw();
		Color *cw = r_b.colors.ptrw();
		const bool tang = r_b.use_tangents && r_b.tangents.size() >= nv * 4;
		float *tw = tang ? r_b.tangents.ptrw() : nullptr;
		const int na = 96;
		const int ns = int(wound_surf.size()) / na;
		for (int v = p_part.z; v < p_part.z + p_part.w && v < nv; v++) {
			StemAt at;
			if (!stem_at(wound_pts, wound_rad, wound_arc, vw[v], at) || at.dist > 2.0f * at.r + 0.05f) {
				continue;
			}
			const Vector3 f = _wound_at(at.s, at.az, at.r);
			float keep = 1.0f;
			if (f.x != 0.0f || f.y > 0.0f || f.z > 0.0f) {
				if (moss_k > 0.0f) {
					// Only a cushion hides a wound: the faint green film at the moss weight's edge is on
					// the wood as well (a strict cut there hid every wound low on a stand's stems).
					keep = 1.0f - Math::smoothstep(0.3f, 0.5f, _moss_at(vw[v], nw[v], moss_ground, moss_k));
				}
				// The relief is wood under the bark and keeps its shape under moss; bare wood and
				// the crack's dark only show where the moss leaves the bark.
				{
					const float e = 0.004f;
					const float fl = _wound_at(at.s, at.az + e / MAX(at.r, 0.01f), at.r).x;
					const float fs = _wound_at(at.s + e, at.az, at.r).x;
					const Vector3 tl = at.axis.cross(at.radial);
					Vector3 n = nw[v] - (tl * ((fl - f.x) / e) + at.axis * ((fs - f.x) / e));
					if (n.length_squared() > 1e-8f) {
						nw[v] = n.normalized();
					}
					vw[v] += at.radial * f.x;
					if (tang) {
						Vector3 t(tw[v * 4], tw[v * 4 + 1], tw[v * 4 + 2]);
						t -= nw[v] * nw[v].dot(t);
						if (t.length_squared() > 1e-8f) {
							t.normalize();
							tw[v * 4] = t.x;
							tw[v * 4 + 1] = t.y;
							tw[v * 4 + 2] = t.z;
						}
					}
					r_b.wound[v] = MAX(r_b.wound[v], f.y * keep);
					const float k = 1.0f - 0.6f * f.z * keep;
					cw[v] = Color(cw[v].r * k, cw[v].g * k, cw[v].b * k, cw[v].a);
				}
			}
			if (p_record && ns > 0) {
				const int is = CLAMP(int(at.s / 0.02f), 0, ns - 1);
				const int ia = int((at.az + float(Math::PI)) / float(Math::TAU) * na) % na;
				float &m = wound_surf[is * na + ia];
				if (at.dist + f.x > m) {
					m = at.dist + f.x;
					if (is * na + ia < int(wound_vtx.size())) {
						wound_vtx[is * na + ia] = Vector2i(v, int(r_b.parts.size()) - 1);
					}
				}
			}
		}
	};
	// Moss shoots on lying wood (trees.md 5c): seen from a step away, the shell's texture blurs and
	// its cushions are smooth domes, where a real carpet of Hypnum and Thuidium is a pile of
	// leafy shoots a centimetre or two long, combed one way, lit at their tips and dark between.
	// Modelled as tufts of tapered blades over the shell's shown faces, more where it is thick;
	// LOD 0 and the path tracer only, as the shell.
	auto moss_shoots = [&](const Vector4i &p_part, float p_r, uint32_t p_salt) {
		RandomPCG mr;
		mr.seed(uint64_t(seed) * 0x9e3779b9u + p_salt * 2654435761u + 0x73686f6fu);
		LocalVector<int> tris;
		for (int q = p_part.x; q + 2 < p_part.x + p_part.y; q += 3) {
			tris.push_back(q);
		}
		if (tris.is_empty()) {
			return;
		}
		Vector4i part;
		moss.begin(part);
		// Shoots per square metre: about one tuft per 2 cm square, thinner on thin wood.
		const float density = 3000.0f * CLAMP(p_r / 0.12f, 0.4f, 1.0f);
		const Vector3 comb = Vector3(0.3f, -1.0f, 0.2f).normalized(); // Shoots lean downhill a little.
		for (int q : tris) {
			const int i0 = moss.indices[q], i1 = moss.indices[q + 1], i2 = moss.indices[q + 2];
			const Vector3 a = moss.verts[i0], b = moss.verts[i1], c = moss.verts[i2];
			const Vector3 fn = (b - a).cross(c - a);
			const float area = 0.5f * fn.length();
			// Only over cushions: where all three corners stand above the bark the shell is thick.
			const float tone = (moss.colors[i0].g + moss.colors[i1].g + moss.colors[i2].g) / 3.0f;
			float want = area * density;
			int n = int(want);
			if (mr.randf() < want - n) {
				n++;
			}
			for (int k = 0; k < n; k++) {
				float u = mr.randf(), v = mr.randf();
				if (u + v > 1.0f) {
					u = 1.0f - u;
					v = 1.0f - v;
				}
				const Vector3 p = a + (b - a) * u + (c - a) * v;
				Vector3 nn = (moss.normals[i0] * (1.0f - u - v) + moss.normals[i1] * u + moss.normals[i2] * v).normalized();
				// Only in the carpet: at its thin edge, over the bark, lone tufts read as stars.
				const float mw = _moss_at(p, nn, moss_ground, moss_k);
				const float carpet = Math::smoothstep(0.3f, 0.75f, mw);
				if (mr.randf() > carpet) {
					continue;
				}
				const Vector2 uv = moss.uvs[i0] * (1.0f - u - v) + moss.uvs[i1] * u + moss.uvs[i2] * v;
				const Color base = moss.colors[i0].lerp(moss.colors[i1], u / MAX(u + v, 1e-4f)).lerp(moss.colors[i2], v);
				Vector3 t1 = nn.cross(Math::abs(nn.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
				Vector3 t2 = nn.cross(t1);
				const int blades = 3 + int(mr.randf() * (1.0f + 4.0f * carpet));
				// Shorter, fewer-bladed and more upright towards the carpet's edge, so it thins out
				// into the shell rather than ending in lone stars.
				const float len = Math::lerp(0.006f, 0.016f, mr.randf()) * (0.8f + 3.0f * tone) * Math::lerp(0.35f, 1.0f, carpet);
				const float az0 = mr.randf() * float(Math::TAU);
				// Each tuft's own shade: lit yellow-green tips, dark olive at the base.
				const float sh = Math::lerp(0.8f, 1.25f, mr.randf());
				const Color tip_c = Color(base.r * 1.6f * sh + 0.02f, base.g * 1.45f * sh + 0.02f, base.b * 0.9f * sh);
				const Color root_c = Color(base.r * 0.55f, base.g * 0.6f, base.b * 0.55f);
				for (int bl = 0; bl < blades; bl++) {
					const float az = az0 + float(Math::TAU) * (float(bl) + 0.4f * mr.randf()) / blades;
					// Shoots rise from the carpet and arch over, not spread flat as a star.
					const float tilt = Math::lerp(0.15f, 0.75f, mr.randf()) * Math::lerp(0.6f, 1.0f, carpet);
					Vector3 side = t1 * Math::cos(az) + t2 * Math::sin(az);
					Vector3 dir = (nn * Math::cos(tilt) + side * Math::sin(tilt) + comb * 0.25f).normalized();
					const float l = len * Math::lerp(0.7f, 1.15f, mr.randf());
					const float w = Math::lerp(0.0012f, 0.0022f, mr.randf());
					const Vector3 across = dir.cross(nn).normalized() * w;
					// Bending over at the tip, away from the tuft.
					const Vector3 mid = p + dir * (0.55f * l);
					const Vector3 tip = mid + (dir * 0.6f + side * 0.5f - nn * 0.1f).normalized() * (0.45f * l);
					Vector3 face = across.cross(dir).normalized();
					if (face.dot(nn) < 0.0f) {
						face = -face;
					}
					// Shaded as the cushion's surface, bent a little toward the blade's own face.
					const Vector3 sn = (nn * 0.7f + face * 0.3f).normalized();
					const int base_i = moss.verts.size();
					// One triangle a blade (each face), its tip bent over: a blade with a joint
					// cost three times the triangles for a bend under a millimetre at 40 cm.
					const Vector3 vp[3] = { p - across, p + across, tip };
					const Color vc[3] = { root_c, root_c, tip_c };
					for (int j = 0; j < 3; j++) {
						moss.verts.push_back(vp[j]);
						moss.normals.push_back(sn);
						moss.uvs.push_back(uv);
						moss.colors.push_back(vc[j]);
						moss.tangents.push_back(across.x / w);
						moss.tangents.push_back(across.y / w);
						moss.tangents.push_back(across.z / w);
						moss.tangents.push_back(1.0f);
					}
					// Both faces (the moss material culls back faces).
					moss.indices.push_back(base_i);
					moss.indices.push_back(base_i + 1);
					moss.indices.push_back(base_i + 2);
					moss.indices.push_back(base_i);
					moss.indices.push_back(base_i + 2);
					moss.indices.push_back(base_i + 1);
				}
			}
		}
		moss.end(part, p_r);
	};
	// Stemflow and the films it feeds (trees.md 5o): smooth-barked trees funnel rain down their
	// stems, beech above all (stemflow up to a fifth of the rain on a beech stand; Levia and Frost
	// 2003), and the water runs in a few wandering tracks that stay dark and wet for hours and
	// carry green algae (Desmococcus, Trentepohlia) and cyanobacterial films; between the tracks
	// the bark is mottled by algae lower down and pale crust lichen (Barkman 1958). Rough bark
	// spreads the water through its furrows instead. Vertex colour, so it holds at every LOD and
	// under path tracing.
	const BarkKind film_kind = _bark_kind(preset);
	const bool film_bark = (film_kind == BARK_SMOOTH || film_kind == BARK_LENTICELLED) && !fallen && !fallen_branch;
	const float film_wet = 0.45f + 0.55f * CLAMP(forest_shade / 0.6f, 0.0f, 1.0f);
	auto film_tint = [&](Builder &r_b, const Vector4i &p_part, float p_radius) {
		if (!film_bark || p_radius < 0.04f || r_b.parts.is_empty() || r_b.parts[r_b.parts.size() - 1] != p_part) {
			return;
		}
		const uint32_t fs = uint32_t(seed) * 2654435761u;
		const Color algae_c(0.09f, 0.15f, 0.04f);
		const float big = Math::smoothstep(0.04f, 0.15f, p_radius);
		for (int v = p_part.z; v < p_part.z + p_part.w; v++) {
			const Vector3 p = r_b.verts[v];
			const float h = p.y - moss_ground;
			// Tracks: bands a hand or two wide, running down the stem for metres and wandering.
			const float wander = 0.6f * (moss_noise(p * Vector3(0.0f, 0.8f, 0.0f) + Vector3(0.3f, 0.0f, 0.7f), fs + 0x73666c31u) - 0.5f);
			const Vector3 q = p + Vector3(wander, 0.0f, -wander);
			const float track = big * Math::smoothstep(0.56f, 0.66f, moss_noise(q * Vector3(7.5f, 0.3f, 7.5f), fs + 0x73666c32u));
			// Films between them: algae low on the stem in patches, crust lichen higher up.
			const float low = 1.0f - Math::smoothstep(1.0f, 8.0f, h);
			// Colonies at two scales: blotches a hand to a forearm across, ragged at the edge.
			const float pn = 0.65f * moss_noise(p * Vector3(3.2f, 1.6f, 3.2f), fs + 0x73666c33u) + 0.35f * moss_noise(p * Vector3(11.0f, 7.0f, 11.0f), fs + 0x73666c35u);
			const float patch = Math::smoothstep(0.5f, 0.62f, pn);
			const float cn = 0.6f * moss_noise(p * Vector3(6.0f, 3.5f, 6.0f), fs + 0x73666c34u) + 0.4f * moss_noise(p * Vector3(16.0f, 10.0f, 16.0f), fs + 0x73666c36u);
			const float crust = Math::smoothstep(0.55f, 0.66f, cn) * (1.0f - track) * (1.0f - 0.7f * patch);
			Color c = r_b.colors[v];
			const float a = c.a;
			const float g = CLAMP(film_wet * (0.55f * track + 0.45f * patch * low), 0.0f, 0.75f);
			c = c.lerp(algae_c, 0.8f * g);
			const float dk = Math::lerp(1.0f, 0.5f, film_wet * track);
			c = Color(c.r * dk, c.g * dk, c.b * dk);
			c = c.lerp(Color(MIN(c.r * 1.35f + 0.03f, 1.0f), MIN(c.g * 1.35f + 0.03f, 1.0f), MIN(c.b * 1.3f + 0.025f, 1.0f)), 0.6f * big * crust);
			c.a = a;
			r_b.colors.write[v] = c;
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
		const float circumference = girth / MAX(1.0f, Math::round(girth / _bark_tile()));
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
		for (int pass_apply = 0; pass_apply < 2; pass_apply++) {
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
		if (ground_slope > 0.0f) {
			// On a bank the downhill roots run on down the face as anchors and stay thick, the
			// uphill ones short into the slope (trees.md 5q; Stokes 2002, root anchorage on slopes).
			const float down = Math::sin(az);
			len *= 1.0f + (0.4f + 0.5f * bank_erosion) * MIN(1.0f, ground_slope / 0.6f) * MAX(down, 0.0f) - 0.35f * MAX(-down, 0.0f);
		}
		root_az.push_back(az);
		root_len.push_back(roots_on ? len : 0.0f);
		// Roots wander round stones and older roots rather than run out as straight spokes.
		root_bend.push_back(Math::lerp(-0.45f, 0.45f, root_rng.randf()));
	}
	// The stone the roots drape over (trees.md 5q): on the stem's +Z side, its inner edge just
	// under the stem's foot; the root nearest it turned onto it and run on past it.
	stone = RootStone();
	if (root_stone > 0.0f && roots_on && !fallen && !fallen_branch && !nodes.is_empty()) {
		const float R = root_stone;
		stone.on = true;
		stone.r = Vector3(R, 0.6f * R, 0.85f * R);
		stone.ground = ground_y;
		stone.c = Vector3(nodes[0].start.x, ground_y - 0.2f * R, nodes[0].start.z + trunk_r + 0.7f * stone.r.z);
		int best = -1;
		float best_da = 1e9f;
		for (int k = 0; k < int(root_az.size()); k++) {
			const float da = Math::abs(Math::angle_difference(root_az[k], float(Math::PI) * 0.5f));
			if (da < best_da) {
				best_da = da;
				best = k;
			}
		}
		if (best >= 0) {
			root_az[best] = float(Math::PI) * 0.5f;
			root_len[best] = MAX(root_len[best], 0.7f * stone.r.z * 2.0f + 0.5f * trunk_r);
			root_bend[best] *= 0.3f;
		}
		_plan_root_stone_rock();
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
	stone.root_top = root_top;
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
					// The root flattens as it nears the soil, so it slides under it: full height, it met the
					// ground in a wall at its tip, a flap lying on the soil (docs, 18 Nov).
					const float a_w = rr * 1.15f, b_h = rr * 0.85f * (1.0f - 0.55f * Math::smoothstep(0.25f, 0.6f, t));
					const float d_c = r_t + root_push(root_top, root_az[best]) * (1.0f - t) + root_len[best] * Math::pow(t, 1.1f) + rr * Math::smoothstep(0.0f, 0.3f, t);
					const float s1 = Math::smoothstep(0.0f, 0.35f, t);
					const float yc = (root_top - b_h) * (1.0f - s1) * (1.0f - s1) - rr * (0.1f + 2.0f * Math::smoothstep(0.35f, 0.85f, t)) * s1;
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
				// Burrs a few centimetres across, hard-edged, many: the larger, merging knobs of the
				// first try read as a melted mass (scene's close-up, 16 Nov).
				const float kn = callus_knobs(hp, CLAMP(0.08f * head_r, 0.025f, 0.045f), 0.45f * head_r, 0.38f, 0x6b6e6234u, crease);
				hoff += win * (head_r * (0.04f * k1 - 0.1f * fis) + kn * (1.0f - fis));
				// The bark's own furrows half closed over by the callus: at full depth they crossed
				// the knobs as long pleats; fully closed the head lost its bark and read as wax.
				hoff -= 0.45f * win * (vv[v].distance_to(p_pts[ring]) - r);
				dark = MIN(dark, Math::lerp(1.0f, 0.42f, win * fis));
				dark = MIN(dark, Math::lerp(1.0f, 0.45f, win * crease));
				// Green algae (Desmococcus) and moss film where water lingers between the knobs
				// and on their tops; drier, paler grey on the bulges' flanks.
				algae = win * CLAMP(0.2f + 1.6f * (-k1) + 0.4f * n.y + 0.3f * fis + 0.3f * crease, 0.0f, 0.6f);
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
					// Lighter than the head's: under the rods a stool is shaded already, and darker
					// fissures turned it into a black clump (scene's close-up, 16 Nov).
					dark = MIN(dark, Math::lerp(1.0f, 0.62f, win * fis));
					dark = MIN(dark, Math::lerp(1.0f, 0.78f, win * crease));
					algae = MAX(algae, win * CLAMP(0.1f + 0.8f * (-k1) + 0.3f * crease, 0.0f, 0.4f));
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
	// A leader that splits into stems is built after them: its swept crotch wraps their bases
	// (TubeCrotch), recorded as each is built.
	LocalVector<int> chain_order;
	{
		LocalVector<int> later;
		for (int s = 1; s < int(nodes.size()); s++) {
			int e = s;
			while (nodes[e].main >= 0 && !nodes[nodes[e].main].removed) {
				e = nodes[e].main;
			}
			bool sp = false;
			for (int l = nodes[e].first_lateral; l >= 0 && !sp; l = nodes[l].next_sibling) {
				sp = !nodes[l].removed && nodes[l].stem_level > nodes[e].stem_level;
			}
			(sp ? later : chain_order).push_back(s);
		}
		for (int s : later) {
			chain_order.push_back(s);
		}
	}
	HashMap<int, LocalVector<TubeCrotch::Stem>> crotch_stems; // By the split node.
	for (int s : chain_order) {
		const Node &first = nodes[s];
		if (first.removed || (first.parent > 0 && nodes[first.parent].main == s)) {
			continue;
		}
		if (preset == PRESET_DWARF_FAN_PALM && first.order > 0) {
			continue; // Petioles are green, not bark: _palm_geometry draws them with the fans.
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
		// A leader that split into stems (trees.md 5z) keeps its girth to the split: tapered to
		// 0.6 there like a shoot's tip, it pinched into the neck under the stems (docs, 18 Nov).
		bool split_tip = false;
		for (int l = nodes[chain[chain.size() - 1]].first_lateral; l >= 0 && !split_tip; l = nodes[l].next_sibling) {
			split_tip = nodes[l].stem_level > nodes[chain[chain.size() - 1]].stem_level;
		}
		// The swept crotch carries no displaced relief (the normal map keeps the bark): the
		// bole's fades out over half a radius under it, each stem's from just past its top.
		tube_fade_r = 0.0f;
		tube_fade_r0 = 0.0f;
		tube_crotch.active = false;
		if (split_tip) {
			tube_fade_c = nodes[chain[chain.size() - 1]].end();
			tube_fade_r0 = 0.6f * nodes[chain[chain.size() - 1]].radius;
			tube_fade_r = 1.2f * nodes[chain[chain.size() - 1]].radius;
		} else if (first.parent >= 0 && first.stem_level > nodes[first.parent].stem_level) {
			tube_fade_c = nodes[first.parent].end();
			tube_fade_r0 = 1.3f * TREE_CROTCH_TOP * nodes[first.parent].radius;
			tube_fade_r = tube_fade_r0 + 0.6f * nodes[first.parent].radius;
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
			float r_end = c + 1 < int(chain.size()) ? nodes[chain[c + 1]].radius : n.radius * (bolling.has(chain[c]) || cut_stubs.has(chain[c]) || split_tip ? 1.0f : 0.6f);
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
		if (s == wound_stem && pts.size() >= 2) {
			wound_pts = pts;
			wound_rad = rad;
			wound_arc.resize(pts.size());
			wound_arc[0] = 0.0f;
			for (uint32_t i = 1; i < pts.size(); i++) {
				wound_arc[i] = wound_arc[i - 1] + pts[i].distance_to(pts[i - 1]);
			}
			wound_surf.resize((int(wound_arc[pts.size() - 1] / 0.02f) + 1) * 96);
			for (uint32_t i = 0; i < wound_surf.size(); i++) {
				wound_surf[i] = -1.0f;
			}
			wound_vtx.resize(wound_surf.size());
			for (uint32_t i = 0; i < wound_vtx.size(); i++) {
				wound_vtx[i] = Vector2i(-1, -1);
			}
		}
		// A hanger's base is a fracture hanging beside its stub, not a junction: no branch
		// collar, fork crotch or weld into the parent.
		// A shed limb or stick (its parent gone) snapped off the same way: no junction with wood
		// that is not there (the fork code pulled a stick's rings onto its old parent's axis).
		const bool hanger = hanger_bases.has(s) || (first.parent > 0 && nodes[first.parent].removed);
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
		// A leader's split into several stems (trees.md 5z) takes the same path whatever the
		// stems' share: four stems of a cork oak, each under 0.6 of the bole, went in as thin pipes
		// pushed into the bole's capped top, a narrow neck under the crown (docs, 18 Nov). Their
		// bases also swell a little, so together they fill the bole's top.
		const bool split = first.parent >= 0 && first.stem_level > nodes[first.parent].stem_level;
		const bool split_stem = split; // (`split` is a ring index in the near tube below.)
		if (!stem && !hanger && pts.size() >= 2 && (r0 > 0.6f * nodes[first.parent].radius || split)) {
			const Node &par = nodes[first.parent];
			const float rp = par.radius;
			Vector3 side = first.dir - par.dir * first.dir.dot(par.dir);
			side = side.length() > 1e-4f ? side.normalized() : Vector3();
			const float rc = MIN(split ? MAX(rad[0] * 1.1f, rp * 0.55f) : rad[0], rp * 0.9f);
			// A split's stems start on the bole's axis, wholly inside it (its relief too): the swept
			// crotch over the split wraps them.
			const float off = split ? 0.0f : MAX(0.0f, rp * 0.92f - rc);
			const Vector3 base = pts[0] + side * off;
			LocalVector<Vector3> cp;
			LocalVector<float> cr;
			const float ks[3] = { 1.2f, 0.7f, 0.25f };
			// A fork low on the stem must not run down out of the ground: its open start
			// showed beside the root flare.
			const float room = MAX(0.0f, (base.y - nodes[0].start.y - 0.1f) / MAX(par.dir.y, 0.2f));
			const float drop = MIN(rc * ks[0], room) / ks[0];
			// A split's stems start narrower, wholly inside the bole: level with its surface, their
			// open first ring stood out through the bole's relief as a pleated rim (docs, 18 Nov).
			const float kr[3] = { split ? 0.6f : 1.0f, split ? 0.7f : 1.0f, split ? 0.8f : 1.0f };
			for (int k = 0; k < 3; k++) {
				cp.push_back(base - par.dir * (drop * ks[k]));
				cr.push_back(rc * kr[k]);
			}
			// The collar's extra rings near the axis would pull the turn back to the axis.
			uint32_t i = 1;
			while (i + 1 < pts.size() && pts[i].distance_to(pts[0]) < rp * 0.8f) {
				i++;
			}
			for (; i < pts.size(); i++) {
				cp.push_back(pts[i]);
				// A split's stems keep their swollen base for about a bole radius and a half, so
				// the bole flows into them (the bole was twice as wide as any of its four stems).
				const float flare = split ? Math::exp(-pts[i].distance_to(pts[0]) / (1.5f * rp)) : 0.0f;
				cr.push_back(rad[i] + MAX(0.0f, rc - rad[i]) * flare);
			}
			pts = cp;
			rad = cr;
		}
		// The tube is open at its end. Thin tips close to under a millimetre on their own; an
		// axis that stops thick (a shed or snapped continuation, a pruned limb) would show its
		// hollow inside as a flat dark disc. It ends in a rounded callus instead (wound wood
		// rolls over a stub; Shigo 1985), or under the splinters of a break.
		// A leader that split into stems (trees.md 5z) ends inside their swollen bases (docs,
		// 18 Nov: a narrow neck under the split). The bole keeps its girth: narrowed, the stems'
		// bases stood out of it as a lip.
		const bool split_end = split_tip;
		if (split_end && pts.size() >= 2) {
			// The bole runs on past the split as the swept crotch (TubeCrotch), from 0.6 of its
			// radius under the split (its stems swell out of it from there, not in a step at the
			// split) up to where its outline lies on the stems, rings every 3 cm on a straight
			// axis; the radii here (a blunt dome) serve only the closing cap, inside the stems.
			const Vector3 top = pts[pts.size() - 1];
			const Vector3 up = (pts[pts.size() - 1] - pts[pts.size() - 2]).normalized();
			const float rt = rad[rad.size() - 1];
			TubeCrotch &cx = tube_crotch;
			cx.c = top;
			cx.up = up;
			cx.rt = rt;
			cx.h0 = -0.6f * rt;
			cx.h_top = TREE_CROTCH_TOP * rt;
			cx.stems.clear();
			if (crotch_stems.has(chain[chain.size() - 1])) {
				cx.stems = crotch_stems[chain[chain.size() - 1]];
			}
			cx.active = !cx.stems.is_empty();
			if (cx.active) {
				const Vector3 c0 = top + up * cx.h0;
				while (pts.size() > 2 && (pts[pts.size() - 1] - top).dot(up) > cx.h0 + 0.02f) {
					pts.resize(pts.size() - 1);
					rad.resize(rad.size() - 1);
				}
				pts.push_back(c0);
				rad.push_back(rt);
				const int n = MAX(4, int(Math::ceil((cx.h_top - cx.h0) / 0.03f)));
				for (int k = 1; k <= n; k++) {
					const float h = Math::lerp(cx.h0, cx.h_top, float(k) / n);
					pts.push_back(top + up * h);
					rad.push_back(MAX(cx.bole_r(h), 0.3f * rt));
				}
			}
		}
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
			if (tube_crotch.active) {
				// The swept crotch starts at the bole's girth there, flare and all.
				const float h = MAX(0.0f, (tube_crotch.c + tube_crotch.up * tube_crotch.h0).y - nodes[0].start.y);
				tube_crotch.rt *= (1.0f + flare * Math::exp(-h / MAX(0.05f, trunk_r * 2.5f))) * (1.0f + 0.15f * flare * Math::exp(-h / 0.12f));
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
		} else if (preset == PRESET_BROOM) {
			// Broom's switches stay green and photosynthesise for years, browning only on old wood
			// at the base (Bean); gorse's shoots are green only their first year or two.
			col = Color(0.28f, 0.42f, 0.16f).lerp(col, Math::smoothstep(0.006f, 0.016f, r0));
		} else if (preset == PRESET_GORSE) {
			col = Color(0.3f, 0.42f, 0.22f).lerp(col, Math::smoothstep(0.0025f, 0.006f, r0));
		}
		if (first.dead) {
			col = col.lerp(Color(0.42f, 0.4f, 0.37f), 0.6f);
		}
		col = col.srgb_to_linear();
		// A split's stems take the bole's tone: their own showed as a band of other bark above it.
		const float tint = (split_tip || (first.parent >= 0 && first.stem_level > nodes[first.parent].stem_level)) ? 1.0f : 0.92f + 0.16f * float((uint32_t(s) * 2654435761u) >> 24) / 255.0f;
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
		if ((fallen || fallen_branch) && r0 >= 0.03f) {
			// Lying wood is seen from a step away, along its length: its peeled patches, moss and
			// tint are per vertex, and rings an internode apart (30-50 cm on a log) smeared them
			// into long streaks. Rings every few centimetres instead.
			densify(pts, rad, [&](const Vector3 &, float p_r) { return CLAMP(0.25f * p_r, 0.03f, 0.06f); });
		}
		// A split's stem: recorded for the bole's swept crotch, its tubes framed from the side
		// facing the bole's axis (the crotch maps its bark into theirs by that frame).
		Vector3 crotch_n0;
		tube_shrink.active = false;
		if (split_stem && pts.size() >= 2) {
			tube_shrink.active = true;
			tube_shrink.c = first.start;
			tube_shrink.up = nodes[first.parent].dir;
			tube_shrink.rt = nodes[first.parent].radius;
			tube_shrink.h_top = TREE_CROTCH_TOP * tube_shrink.rt;
			TubeCrotch::Stem cs;
			const Vector3 axis_up = nodes[first.parent].dir;
			const Vector3 t0 = (pts[1] - pts[0]).normalized();
			Vector3 in = -(first.dir - axis_up * first.dir.dot(axis_up));
			in = in - t0 * in.dot(t0);
			crotch_n0 = in.length_squared() > 1e-8f ? in.normalized() : Vector3();
			const float reach = 4.0f * nodes[first.parent].radius;
			float arc = 0.0f;
			for (uint32_t i = 0; i < pts.size(); i++) {
				if (i > 0) {
					arc += pts[i].distance_to(pts[i - 1]);
				}
				cs.p.push_back(pts[i]);
				cs.r.push_back(rad[i]);
				cs.arc.push_back(arc);
				if (pts[i].distance_to(first.start) > reach && i >= 1) {
					break;
				}
			}
			cs.n0 = crotch_n0;
			const float girth = Math::TAU * MAX(girth_r > 0.0f ? girth_r : rad[0], 0.001f);
			cs.repeats = MAX(1.0f, Math::round(girth / _bark_tile()));
			cs.circ = girth / cs.repeats;
			if (crotch_n0 != Vector3()) {
				crotch_stems[first.parent].push_back(cs);
			}
		}
		if (!(near_tube && skip_lods)) {
			Vector4i part;
			bark.role = near_tube ? Builder::ROLE_PLAIN : Builder::ROLE_ALL;
			tube_carry_nrm = crotch_n0;
			bark.begin(part);
			_tube(bark, pts, rad, sides, 0.0f, col, nullptr, 0.0f, girth_r);
			tube_carry_nrm = Vector3();
			bark.end(part, r0);
			if (collar) {
				root_collar(bark, part, sides, pts, rad, false);
			}
			film_tint(bark, part, r0);
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
			if (!stem && !hanger && !split_stem && junction_weld) {
				weld(bark, part, s, pts, rad, sides);
			}
			if (s == wound_stem) {
				wound_apply(bark, part, !near_tube);
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
					if (crotch_n0 != Vector3()) {
						tube_carry_nrm = crotch_n0;
					}
					_tube(bark, bp, br, bsides, 0.0f, col, relief ? bs.height.ptr() : nullptr, relief ? scale : 0.0f, girth_r, !(fallen || fallen_branch));
					bark.end(part, r0);
					if (collar) {
						root_collar(bark, part, bsides, bp, br, false);
					}
					film_tint(bark, part, r0);
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
					if (!stem && !hanger && !split_stem && junction_weld) {
						weld(bark, part, s, bp, br, bsides);
					}
					if (s == wound_stem) {
						wound_apply(bark, part, true);
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
					if (crotch_n0 != Vector3()) {
						tube_carry_nrm = crotch_n0;
					}
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
						if (raw[i] > 0.15f && raw[i] < 0.85f) {
							// A ragged margin (trees.md 5c): a moss carpet's edge is lobed and torn, a
							// few centimetres in and out, never the weight's smooth contour (which drew
							// cushions as round discs).
							const Vector3 &mp = moss.verts[part.z + i];
							raw[i] += 0.17f * (moss_noise(mp * 13.0f, 0x6d6f7377u) - 0.5f) + 0.08f * (moss_noise(mp * 31.0f, 0x6d6f7378u) - 0.5f);
						}
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
						// Thin at the margin and thickening inward over a wide band of the weight: a
						// cushion's edge is a few millimetres of shoots creeping over the bark, its crown
						// well inside (a cliff of the full depth at the edge read as a cut-out shell).
						const float thick = Math::lerp(0.15f, 1.0f, Math::smoothstep(0.5f, 0.85f, raw[i]));
						float fill = Math::lerp(floor_d, depth * thick * (0.6f + 0.8f * hummock), ramp * ramp * (3.0f - 2.0f * ramp));
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
					if (kept > 0 && (fallen || fallen_branch) && !rigged && r0 >= 0.03f) {
						moss_shoots(Vector4i(part.x, kept, part.z, nv), r0, uint32_t(s));
					}
				}
			}
			cut_arc0 = 0.0f;
			tube_carry_nrm = Vector3();
		}
		tube_crotch.active = false;
		tube_shrink.active = false;
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
	const Color twig_col = (birch_twigs ? Color(0.3f, 0.18f, 0.14f) : (preset == PRESET_BROOM ? Color(0.28f, 0.42f, 0.16f) : (preset == PRESET_GORSE ? Color(0.3f, 0.42f, 0.22f) : bark_color))).srgb_to_linear();
	// A mat's dwarfed shoots carry their needles close: more to a shoot (trees.md 5s).
	const int per = MAX(0, int(params[PARAM_LEAVES_PER_INTERNODE] * (1.0f + 0.9f * krummholz)));
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
	// Gum rockrose is summer semi-deciduous (trees.md 5v; Correia and Ascensao 2017): from early
	// July the big spring leaves fold along the midrib, shrink, dull and partly fall, and only the
	// small summer leaves at the tips stay green; the autumn rains bring new leaves.
	float summer_fold = 0.0f;
	if (preset == PRESET_GUM_ROCKROSE && !season.is_empty()) {
		const float sday = season.get("day_of_year", 182.0f);
		summer_fold = sday <= 175.0f || sday >= 300.0f ? 0.0f : (sday < 205.0f ? (sday - 175.0f) / 30.0f : (sday <= 260.0f ? 1.0f : (300.0f - sday) / 40.0f));
	}
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
		if (summer_fold > 0.0f) {
			if (h > 1.0f - 0.35f * summer_fold) {
				return false;
			}
			r_size *= 1.0f - 0.4f * summer_fold;
			r_color = r_color.lerp(Color(0.1f, 0.1f, 0.06f), 0.6f * summer_fold);
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
					{
						Vector4i part;
						bark.begin(part);
						_tube(bark, pts, rad, 3, 0.0f, twig_col);
						bark.end(part, rad[0]);
					}
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
					{
						Vector4i part;
						leaves.begin(part);
						_add_spray(leaves, b0, bdir, blen + size * 0.5f, half, flat, c);
						leaves.end(part, 0.0f);
					}
					{
						Vector4i part;
						leaves.begin(part);
						_add_spray(leaves, b0, bdir, blen + size * 0.5f, half, cross, c);
						leaves.end(part, 0.0f);
					}
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
					{
						Vector4i part;
						leaves.begin(part);
						if (preset == PRESET_GORSE) {
							// Spines stand out wider than conifer needles (45-70 degrees).
							const Vector3 sout = (ar * 0.85f + bdir * 0.53f).normalized();
							const uint32_t sh_hash = hash_murmur3_one_32(uint32_t(i) * 7919u + uint32_t(sh * 257 + k), uint32_t(seed));
							_add_spine(leaves, base, sout, needle_size * 1.25f, c, sh_hash, (k % 3) == 0);
						} else {
							_add_needle(leaves, base, out, needle_size, c);
						}
						leaves.end(part, 0.0f);
					}
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
		const float droop = params[PARAM_TWIG_DROOP] * (1.0f - 0.85f * krummholz);
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
			{
				Vector4i part;
				bark.begin(part);
				_tube(bark, pts, rad, 3, 0.0f, twig_col);
				bark.end(part, rad[0]);
			}
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
				{
					Vector4i part;
					leaves.begin(part);
					_add_leaf(leaves, at, k == p_leaves - 1 ? tip_dir : out, up, leaf_size, c);
					leaves.end(part, 0.0f);
				}
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

	if (preset == PRESET_DWARF_FAN_PALM) {
		_palm_geometry(bark, leaves);
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
	_grow_checks();
	_grow_chevrons(bark);
	Builder resin;
	_grow_resin(resin);
	Builder *builders[10] = { &bark, &leaves, &moss, &climber_leaf_b, &fungi, &wood_b, &climber_adult_b, &soil_b, &crown_parts, &resin };
	if (stone.on) {
		_build_root_stone(soil_b);
	}
	for (int s = 0; s < 10; s++) {
		// The stone itself is not draped over itself; it is sheared with the ground.
		_bank_shear(*builders[s], s != 7);
	}
	// Rigged trees skin from MultiMesh instance bones; a part broken off by a storm or a body
	// (TreeWind3D) is drawn unskinned, with a twin of each surface's material (section 5t).
	Array static_materials;
	auto twin = [&](const Ref<StandardMaterial3D> &p_m) {
		Ref<StandardMaterial3D> t = p_m->duplicate();
		t->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, false);
		return t;
	};
	for (int s = 0; s < 10; s++) {
		Builder &b = *builders[s];
		if (b.indices.is_empty()) {
			continue;
		}
		if (s == 0 || s == 2 || s == 7 || s == 9) {
			// The bark, moss, soil and resin builders wind their triangles counter-clockwise seen from outside;
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
			bark_custom = _bare_amount() > 0.0f || !b.wound.is_empty() || _cork_strip_age() >= 0.0f;
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
				if (far_leaves && far_lod1) {
					_promote_lod1(b, lods);
				}
			} else if (s == 3 || s == 6) {
				lods = _leaf_lods(b, false);
			} else if (s == 8) {
				// Crown parts (5n): mistletoe balls and brooms are seen from far; cones, catkins
				// and keys are centimetres across, LOD 0 only.
				bool scatter = false;
				for (uint32_t i = 0; i < b.part_size.size() && !scatter; i++) {
					scatter = b.part_size[i] < 0.0f;
				}
				if (scatter) {
					// A shrub's flowers and pods (sized negative) thin stochastically as leaves do,
					// survivors grown about their base by 1/sqrt(keep) (Cook et al. 2007); the other
					// parts before big_end stay whole, those after it are LOD 0 only.
					const int full = b.indices.size();
					for (const auto &level : TREE_LODS) {
						const float grow = 1.0f / Math::sqrt(level.keep);
						PackedInt32Array idx;
						for (uint32_t i = 0; i < b.parts.size(); i++) {
							const Vector4i pt = b.parts[i];
							if (pt.x >= crown_big_end) {
								continue;
							}
							if (b.part_size[i] >= 0.0f) {
								for (int k = 0; k < pt.y; k++) {
									idx.push_back(b.indices[pt.x + k]);
								}
								continue;
							}
							const float u = float((uint32_t(i) * 2654435761u) >> 8) / 16777216.0f;
							if (u >= level.keep) {
								continue;
							}
							const int v0 = b.verts.size();
							const Vector3 pivot = b.verts[pt.z];
							for (int v = 0; v < pt.w; v++) {
								b.verts.push_back(pivot + (b.verts[pt.z + v] - pivot) * grow);
								b.normals.push_back(b.normals[pt.z + v]);
								b.uvs.push_back(b.uvs[pt.z + v]);
								b.colors.push_back(b.colors[pt.z + v]);
								for (int k = 0; k < 4; k++) {
									b.bones.push_back(b.bones[(pt.z + v) * 4 + k]);
									b.weights.push_back(b.weights[(pt.z + v) * 4 + k]);
								}
							}
							for (int k = 0; k < pt.y; k++) {
								idx.push_back(b.indices[pt.x + k] - pt.z + v0);
							}
						}
						if (idx.size() >= full) {
							continue;
						}
						if (idx.is_empty()) {
							idx.push_back(0);
							idx.push_back(0);
							idx.push_back(0);
						}
						lods[level.error] = idx;
					}
					if (far_leaves && far_lod1) {
						// A far mesh starts at the first thinned level, as its leaves do (8d).
						_promote_lod1(b, lods);
					}
				} else if (crown_big_end < b.indices.size()) {
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
			// A far mesh with its near mesh split off (trees.md 8d) leaves out the leaf twigs under
			// a millimetre (FAR_TWIG_RADIUS): a fraction of a pixel past the near range, and a
			// shrub's bulk (lentisk 700k of its 930k bark triangles). The near mesh keeps them.
			const bool far_twigs = far_leaves && far_lod1 && !skip_lods;
			auto skip_part = [&](uint32_t i) {
				return b.part_role[i] == Builder::ROLE_PLAIN || (far_twigs && b.part_role[i] == Builder::ROLE_ALL && b.part_size[i] < FAR_TWIG_RADIUS);
			};
			bool any_plain = false;
			for (uint32_t i = 0; i < b.parts.size(); i++) {
				any_plain = any_plain || skip_part(i);
			}
			if (any_plain) {
				PackedInt32Array full;
				full.resize(b.indices.size());
				int at = 0, from = 0;
				int32_t *fw = full.ptrw();
				for (uint32_t i = 0; i < b.parts.size(); i++) {
					const Vector4i &pt = b.parts[i];
					if (!skip_part(i)) {
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
			for (const Vector2i &ep : b.extra_part) {
				const Vector4 &m = b.part_uv2[ep.y];
				if (m.x > 0.0f && ep.x < uv2.size()) {
					u2[ep.x] = Vector2(b.uvs[ep.x].x * m.x + m.z, b.uvs[ep.x].y * m.y + m.w);
				}
			}
			arrays[Mesh::ARRAY_TEX_UV2] = uv2;
		}
		if (s == 0 && far_leaves && far_lod1 && !skip_lods) {
			// The far mesh's left-out twigs: drop their vertices too.
			PackedVector2Array uv2 = arrays[Mesh::ARRAY_TEX_UV2];
			_compact_vertices(b, lods, uv2.size() == b.verts.size() ? &uv2 : nullptr);
			if (!uv2.is_empty()) {
				arrays[Mesh::ARRAY_TEX_UV2] = uv2;
			}
		}
		{
			// A level no smaller than the base it simplifies (the base lost its plain tubes, far
			// twigs or LOD 0 leaves after the levels were cut) is rejected by the renderer with an
			// error; it would draw the same anyway.
			Dictionary kept;
			for (const KeyValue<Variant, Variant> &kv : lods) {
				if (PackedInt32Array(kv.value).size() < b.indices.size()) {
					kept[kv.key] = kv.value;
				}
			}
			lods = kept;
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
			const char *names[10] = { "bark", params[PARAM_NEEDLES] > 0.5f ? "needles" : "leaves", "moss", "climber_leaves", "fungi", "wood", "climber_leaves_adult", "soil", "crown_parts", "resin" };
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
				if (rigged && material_source->has_meta(SNAME("tree_static_materials"))) {
					const Array src = material_source->get_meta(SNAME("tree_static_materials"));
					static_materials.push_back(found < src.size() ? src[found] : Variant());
				}
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
			// The texture stores albedo / headroom (linear); the material's albedo colour is sRGB
			// (the shader converts it), so the headroom goes in encoded: given as a plain 1.35 it
			// came back as 2.05 and smooth bark rendered 1.5 times too bright.
			const float head = _bark_headroom(_bark_kind(preset));
			m->set_albedo(Color(head, head, head).linear_to_srgb());
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
				if (rigged) {
					static_materials.push_back(_lichen_material(twin(m)));
				}
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
			if (preset == PRESET_GORSE || preset == PRESET_BROOM) {
				// Mostly petals: thin, they glow golden against the light, and their satin
				// sheen is weak (a plain 0.5 specular laid the sky's grey-blue over the yellow
				// and turned it mustard).
				m->set_backlight(Color(0.3f, 0.26f, 0.02f));
				m->set_specular(0.25f);
			}
			mesh->surface_set_name(mesh->get_surface_count() - 1, "crown_parts");
		} else if (s == 9) {
			// Resin runs and beads on a conifer's wounds (trees.md 5p): vertex colour, glossy.
			m->set_roughness(0.22f);
			m->set_feature(BaseMaterial3D::FEATURE_SNOW, false);
			mesh->surface_set_name(mesh->get_surface_count() - 1, "resin");
		} else if (s == 7) {
			// A root plate's soil and stones (trees.md 5k): vertex colour, the relief is geometry.
			m = _soil_material();
			if (rigged) {
				m->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, true);
			}
			mesh->surface_set_name(mesh->get_surface_count() - 1, "soil");
		} else if (s == 5) {
			m = _wood_material();
			if (rigged) {
				m->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, true);
			}
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
			// The moss shell carries the bark's UVs; its texture is drawn over the common tile.
			const float mt = _bark_tile() / BARK_TILE;
			m->set_uv1_scale(Vector3(mt, mt, 1.0f));
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
			if (rigged) {
				static_materials.push_back(_flutter_material(twin(m), s == 3 ? 0.05f : params[PARAM_LEAF_SIZE]));
			}
		} else {
			mesh->surface_set_material(mesh->get_surface_count() - 1, m);
			if (rigged) {
				static_materials.push_back(twin(m));
			}
		}
	}
	if (rigged && static_materials.size() == mesh->get_surface_count()) {
		mesh->set_meta(SNAME("tree_static_materials"), static_materials);
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
		if (storm_kit && !fallen && decay_class == 0 && !fallen_branch) {
			_storm_kit(mesh, static_materials);
		}
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
			// The edge wanders with the bark's furrows and, on smooth bark with little relief,
			// with a soft noise (the relief alone tore it into a jagged cut).
			// At three scales, stretched along the grain (bark tears along it): a straight
			// contour between the vertices read as a cut (docs, 16 Nov).
			float e1 = texture(lichen_map, base_uv * vec2(1.1, 0.35) + vec2(0.31, 0.17)).g;
			float e2 = texture(lichen_map, base_uv * vec2(5.0, 1.1) + vec2(0.71, 0.43)).r;
			float e3 = texture(lichen_map, base_uv * vec2(17.0, 3.0) + vec2(0.13, 0.89)).g;
			float edge_n = 0.35 * bark_relief + 0.3 * e1 + 0.22 * e2 + 0.13 * e3;
			bare_field += 0.5 * (0.5 - edge_n);
			bark_bare = smoothstep(0.47, 0.53, bare_field);
		}
		// Lichen: a colony covers the texels whose growth order is under the weight.
		float lw = clamp(2.0 * COLOR.a - 1.0, 0.0, 1.0);
		if (lw > 0.004) {
			vec4 lm = texture(lichen_map, UV2);
			// A crust is a skin a fraction of a millimetre thick: the bark's own tone shows through
			// it (darker in the bark's cracks), as do its relief and normal map.
			float bark_tone = clamp(dot(albedo.rgb * albedo_tex.rgb, vec3(0.3, 0.5, 0.2)) / max(dot(COLOR.rgb, vec3(0.3, 0.5, 0.2)), 1e-3), 0.4, 1.4);
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
			vec3 cc = (lm.b < lichen_orange_share ? lichen_orange_color : crust_col) * mix(0.78, 1.05, lm.r) * mix(1.0, bark_tone, 0.7);
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
			// Fibres drawn, not sampled: a texel of the lichen layer covers a millimetre at best
			// and blurred into a stretched photo at 40 cm (docs, 16 Nov). Lines along the grain a
			// millimetre and a half apart (BARK_TILE 0.4 m: 266 a tile), wavering with a slow
			// noise, and coarser growth bands; faded out as they shrink below a pixel.
			float warp = texture(lichen_map, vec2(base_uv.x * 0.6, base_uv.y * 0.12)).r;
			// The grain meanders a centimetre or two either way over half a metre: straight
			// parallel bands read as sawn planks (docs, 16 Nov).
			float mea = (texture(lichen_map, vec2(base_uv.x * 0.35 + 0.61, base_uv.y * 0.09)).g - 0.5) * 0.06;
			// Knots: the grain parts round each overgrown branch base and closes again behind
			// it, the knot itself a dark end-grain eye with its own rings. Cells 16 cm round
			// and 48 cm along the stem, about half with a knot 1.5-4.5 cm across.
			vec2 kq = base_uv * vec2(2.5, 0.83);
			vec2 kc = floor(kq);
			float kx_shift = 0.0;
			float knot = 0.0;
			for (int ky = -1; ky <= 1; ky++) {
				vec2 cc = kc + vec2(0.0, float(ky));
				float kh = fract(sin(dot(cc, vec2(17.3, 91.7))) * 43758.5453);
				if (kh < 0.5) {
					vec2 ctr = (cc + vec2(0.2 + 0.6 * fract(kh * 13.1), 0.2 + 0.6 * fract(kh * 7.7))) / vec2(2.5, 0.83);
					float kr = (0.015 + 0.03 * fract(kh * 3.3)) / 0.4; // Tile units.
					vec2 dq = base_uv - ctr;
					dq.x = dq.x - round(dq.x * 1.0) * 1.0;
					vec2 de = vec2(dq.x, dq.y * 0.3);
					float f = kr * kr / (dot(de, de) + 0.3 * kr * kr);
					kx_shift += dq.x * f;
					float kd = length(vec2(dq.x, dq.y * 0.6)) / kr;
					knot = max(knot, (1.0 - smoothstep(0.55, 0.75, kd)) * (0.75 + 0.25 * sin(kd * 30.0)));
				}
			}
			float fx = (base_uv.x - kx_shift + mea) * 266.0 + warp * 9.0;
			float fw = fwidth(fx);
			float fib = 0.5 + 0.5 * sin(6.2832 * fx);
			fib = mix(fib, 0.5, smoothstep(0.25, 0.6, fw));
			// Weathering eats the soft earlywood and leaves the latewood standing in ridges
			// along the grain, uneven in width (no planed board's even bands).
			float bx = base_uv.x * 38.0 + warp * 3.0;
			float kux = base_uv.x - kx_shift + mea;
			// Ridges run a hand to a forearm and fade (sampled long, they ran the log's length).
			float band = 0.6 * texture(lichen_map, vec2(kux * 9.0, base_uv.y * 0.7)).r + 0.4 * texture(lichen_map, vec2(kux * 31.0 + 0.4, base_uv.y * 1.5)).r;
			// Rays: beech shows its broad rays on a split or peeled face as short dark spindles
			// along the grain, a few millimetres long.
			vec2 rq = vec2(base_uv.x * 140.0, base_uv.y * 26.0 + warp * 2.0);
			vec2 rc = floor(rq);
			vec2 rl = fract(rq) - 0.5;
			float rh = fract(sin(dot(rc, vec2(41.3, 289.1))) * 43758.5453);
			float ray = rh < 0.3 ? (1.0 - smoothstep(0.12, 0.3, abs(rl.x))) * (1.0 - smoothstep(0.25, 0.45, abs(rl.y))) : 0.0;
			ray *= 1.0 - smoothstep(0.3, 0.7, fwidth(rq.x));
			float grain = 0.55 * fib + 0.45 * band;
			// Checks: thin dark splits along the grain, wavering with it, broken into lengths.
			float wander = texture(lichen_map, vec2(base_uv.x * 0.7 + 0.53, base_uv.y * 0.02)).r;
			float split = abs(fract(kux * 5.0 + 1.2 * wander) - 0.5);
			float run = smoothstep(0.5, 0.65, texture(lichen_map, vec2(base_uv.x * 0.9 + 0.21, base_uv.y * 0.03)).r);
			float checks = mix(1.0, smoothstep(0.01, 0.07, split), run);
			vec3 wood = bark_wood_color * (0.8 + 0.32 * grain) * mix(0.3, 1.0, checks) * (1.0 - 0.3 * ray);
			// Silver-grey on the weathered ridges, brown in the grooves where it stays damp.
			wood = mix(wood * vec3(0.92, 0.86, 0.78), wood * vec3(1.08, 1.08, 1.06), smoothstep(0.35, 0.7, band));
			// Close up the long grain alone smeared into streaks (a texel stretched over a hand's
			// length). Finer fibres; rot staining in patches; the black zone lines where rival
			// fungi meet (spalting, beech above all); and bark beetles' galleries etched into the
			// sapwood under the lost bark, a mother gallery along the grain with the larvae's
			// tunnels running off it (Rayner and Boddy 1988; Gregoire et al., bark beetles, 2015).
			float fine = fib;
			float stain = texture(lichen_map, vec2(base_uv.x * 1.3 + 0.37, base_uv.y * 0.6 + 0.11)).r;
			// Blotches of fungal and water staining a hand to a forearm long, brown to near
			// black, and grey where the wood dried bleached.
			float blot = texture(lichen_map, vec2(base_uv.x * 0.55 + 0.13, base_uv.y * 0.22 + 0.61)).g;
			float bleach = texture(lichen_map, vec2(base_uv.x * 0.7 + 0.52, base_uv.y * 0.3 + 0.05)).r;
			float zn = texture(lichen_map, vec2(base_uv.x * 0.9 + 0.71, base_uv.y * 0.45 + 0.29)).r;
			float zone = 1.0 - smoothstep(0.004, 0.014, abs(zn - 0.5));
			wood *= mix(1.1, 0.8, smoothstep(0.35, 0.7, stain));
			wood = mix(wood, wood * vec3(0.5, 0.42, 0.36), 0.8 * smoothstep(0.5, 0.72, blot));
			wood = mix(wood, vec3(dot(wood, vec3(0.33))) * 1.25, 0.5 * smoothstep(0.55, 0.8, bleach));
			wood = mix(wood, wood * vec3(0.35, 0.28, 0.22), knot);
			vec2 gq = base_uv * vec2(4.0, 1.6);
			vec2 gc = floor(gq);
			vec2 gl = fract(gq);
			float gh = fract(sin(dot(gc, vec2(127.1, 311.7))) * 43758.5453);
			float gallery = 0.0;
			if (gh < 0.45) {
				float gx = 0.35 + 0.3 * fract(gh * 7.31);
				float along = smoothstep(0.1, 0.18, gl.y) * (1.0 - smoothstep(0.82, 0.9, gl.y));
				float mother = (1.0 - smoothstep(0.012, 0.03, abs(gl.x - gx))) * along;
				float side = abs(gl.x - gx);
				float wig = abs(fract(gl.y * 11.0 + 0.6 * sin(side * 25.0 + gh * 20.0)) - 0.5);
				float larva = (1.0 - smoothstep(0.03, 0.07, wig)) * (1.0 - smoothstep(0.12, 0.3, side)) * along;
				gallery = max(mother, 0.7 * larva);
			}
			wood *= 1.0 - 0.65 * gallery;
			wood = mix(wood, vec3(0.03, 0.025, 0.02), 0.85 * zone);
			if (bark_stripped_age >= 0.0) {
				// A cork oak's stripped stem (trees.md 5aa): the phellogen bright orange when
				// fresh, deep red-brown in weeks, brown-black within the year; then the new cork,
				// browner and smoother than virgin cork, finely and regularly cracked as it thickens.
				float t = bark_stripped_age;
				vec3 c = mix(vec3(0.69, 0.17, 0.02), vec3(0.17, 0.025, 0.008), smoothstep(0.0, 0.12, t));
				c = mix(c, vec3(0.033, 0.013, 0.008), smoothstep(0.12, 1.0, t));
				c = mix(c, vec3(0.15, 0.09, 0.055), smoothstep(1.0, 7.0, t)); // Linear: sRGB orange, red-brown, near black, cork brown.
				float tone = texture(lichen_map, base_uv * vec2(1.7, 0.6) + vec2(0.21, 0.4)).g;
				float crk = texture(lichen_map, base_uv * vec2(7.0, 2.2) + vec2(0.57, 0.12)).r;
				float lines = 1.0 - smoothstep(0.015, 0.05, abs(crk - 0.5));
				c *= mix(0.85, 1.12, tone);
				c *= 1.0 - 0.55 * smoothstep(0.5, 5.0, t) * lines;
				wood = c;
				bark_wood_n = vec3(0.5, 0.5, 1.0);
			}
			float rim = smoothstep(0.25, 0.47, bare_field) * (1.0 - bark_bare);
			ALBEDO *= 1.0 - 0.45 * rim;
			ALBEDO = mix(ALBEDO, wood, bark_bare);
			ROUGHNESS = mix(ROUGHNESS, 0.8, bark_bare);
			// The wood's own relief: fibre ridges across u and the checks as grooves, as a tangent
			// space normal from the pattern's slope (finite differences of the same samples).
			float du = 0.004;
			float split1 = abs(fract((kux + du) * 5.0 + 1.2 * wander) - 0.5);
			float c1 = mix(1.0, smoothstep(0.01, 0.07, split1), run);
			float bx1 = bx + du * 38.0;
			float band1 = 0.6 * texture(lichen_map, vec2((kux + du) * 9.0, base_uv.y * 0.7)).r + 0.4 * texture(lichen_map, vec2((kux + du) * 31.0 + 0.4, base_uv.y * 1.5)).r;
			float h0 = 0.6 * band + checks - 0.6 * gallery, h1 = 0.6 * band1 + c1 - 0.6 * gallery;
			// The fibres' own ridges from their analytic slope (they repeat faster than du).
			float fib_n = 0.1 * cos(6.2832 * fx) * (1.0 - smoothstep(0.25, 0.6, fw));
			bark_wood_n = vec3(clamp(0.5 - (h1 - h0) * 4.0 - fib_n, 0.0, 1.0), 0.5, 1.0);
		}
	}
	{
		// Fire (vegetation_fire.h): bark charred up to about the flame length that passed, with a
		// ragged top (the flame wraps the stem unevenly). Char takes the plates first, the
		// furrows keep a little of the bark's brown.
		vec3 fire_w = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec4 fire_s = fire_state_at(fire_w.xz);
		if (fire_s.x > 0.0) {
			float fire_edge = texture(lichen_map, base_uv * vec2(1.3, 0.4) + vec2(0.57, 0.21)).r;
			float reach = fire_s.z * (0.7 + 0.6 * fire_edge);
			float charred = fire_s.x * (1.0 - smoothstep(0.8, 1.0, (fire_w.y - MODEL_MATRIX[3].y) / max(reach, 0.02)));
			float plates = texture(texture_roughness, base_uv).b;
			ALBEDO = mix(ALBEDO, mix(ALBEDO * 0.25, vec3(0.03, 0.026, 0.023), smoothstep(0.3, 0.6, plates)), charred);
			ROUGHNESS = mix(ROUGHNESS, 0.9, charred);
			bark_bare *= 1.0 - charred;
			// Burning and smouldering wood near the ground (logs, fallen branches, a stem's foot): the glow
			// the fire map carries there shows in the char's cracks, the furrows between the plates.
			float fire_low = 1.0 - smoothstep(0.15, 0.5, fire_w.y - MODEL_MATRIX[3].y);
			float fire_cracks = 1.0 - smoothstep(0.2, 0.45, plates);
			fire_bark_emit = fire_glow_color(fire_s.y) * charred * fire_low * (0.15 + 0.85 * fire_cracks) * 3.0;
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
	{
		// The fire's glow, added at the end of fragment() after any emission the material sets.
		const int fr = code.find("void fragment() {");
		int depth = 0;
		int end = -1;
		for (int i = code.find("{", fr); fr >= 0 && i >= 0 && i < code.length(); i++) {
			if (code[i] == '{') {
				depth++;
			} else if (code[i] == '}' && --depth == 0) {
				end = i;
				break;
			}
		}
		if (end > 0) {
			code = code.insert(end, "	EMISSION += fire_bark_emit;\n");
			code = code.insert(code.find("{", fr) + 1, "\n	vec3 fire_bark_emit = vec3(0.0);");
		}
	}
	const String uniforms = R"(uniform sampler2D lichen_map : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform vec3 lichen_crust_color : source_color = vec3(0.5, 0.53, 0.47);
uniform vec3 lichen_leafy_color : source_color = vec3(0.6, 0.64, 0.55);
uniform vec3 lichen_orange_color : source_color = vec3(0.74, 0.58, 0.28);
uniform float lichen_leafy_share = 0.5;
uniform float lichen_orange_share = 0.0;
uniform vec3 bark_wood_color : source_color = vec3(0.55, 0.53, 0.49);
uniform float bark_stripped_age = -1.0;
)";
	code = code.insert(vx, uniforms + VegetationFire::SHADER_CODE + "\n");
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
	const Color wood = (fallen || fallen_branch || decay_class >= 5) ? Color(0.38f, 0.31f, 0.23f) : decay_class == 4 ? Color(0.42f, 0.38f, 0.32f)
																													 : Color(0.47f, 0.45f, 0.41f);
	m->set_shader_parameter("bark_wood_color", wood);
	m->set_shader_parameter("bark_stripped_age", _cork_strip_age());
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
		case PRESET_CORK_OAK: // Grey felted below.
		case PRESET_HOLM_OAK:
			leaf_underside = 0.6f;
			break;
		case PRESET_WILD_OLIVE: // Silvery scales below.
			leaf_underside = 0.85f;
			break;
		case PRESET_CAROB: // Paler below.
			leaf_underside = 0.3f;
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
	if (al >= 0) {
		// Fire (vegetation_fire.h): leaves under the flame charred, up to the scorch height killed brown.
		code = code.insert(al + albedo_line.length(), R"(
	vec3 fire_leaf_back = vec3(1.0);
	{
		vec3 fire_w = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec4 fire_s = fire_state_at(fire_w.xz);
		if (fire_s.x > 0.0) {
			float fire_rough = 0.0;
			vec3 fire_before = ALBEDO;
			fire_foliage_apply(fire_s, fire_w.y - MODEL_MATRIX[3].y, fract(dot(COLOR.rgb, vec3(53.17, 21.71, 7.13))), ALBEDO, fire_rough);
			// Light through a dead leaf takes its colour too (seen from below, a crown is mostly backlit).
			fire_leaf_back = clamp(ALBEDO / max(fire_before, vec3(1e-3)), 0.0, 1.5);
		}
	})");
		const String back_line = "	BACKLIGHT = (backlight.rgb + backlight_tex);\n";
		const int bl = code.find(back_line);
		if (bl >= 0) {
			code = code.insert(bl + back_line.length(), "	BACKLIGHT *= fire_leaf_back;\n");
		}
	}
	if (al >= 0 && leaf_underside > 0.0f) {
		code = code.insert(al + albedo_line.length(), R"(
	if (!FRONT_FACING) {
		float leaf_lum = dot(ALBEDO, vec3(0.299, 0.587, 0.114));
		ALBEDO = mix(ALBEDO, vec3(leaf_lum) * vec3(0.93, 1.04, 0.9) * 1.2, leaf_underside);
	})");
	}
	code = code.insert(vx, uniforms + "uniform float leaf_underside = 0.0;\n" + wind + VegetationFire::SHADER_CODE + "\n");
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

// Far meshes from LOD 1 (trees.md 8d): the leaves' LOD 1 (half the leaves, each grown to keep
// the crown's leaf area; Cook et al. 2007) becomes the surface's own triangles, the coarser
// levels stay, and vertices only LOD 0 used are dropped. Beyond the near range raster already
// drew LOD 1 or coarser; the path tracer, which draws LOD 0, now sees LOD 1 there too.
void TreeGenerator::_promote_lod1(Builder &r_b, Dictionary &r_lods) const {
	const Variant first = TREE_LODS[0].error;
	if (!r_lods.has(first)) {
		return;
	}
	r_b.indices = r_lods[first];
	r_lods.erase(first);
	_compact_vertices(r_b, r_lods, nullptr);
}

// Drops the vertices no index (base or LOD) uses, remapping them all; and the LODs no coarser than
// the base (a palm keeps its fans whole), which the renderer rejects. The parts index the old
// vertices, so they are cleared: nothing reads them past this point.
void TreeGenerator::_compact_vertices(Builder &r_b, Dictionary &r_lods, PackedVector2Array *r_uv2) const {
	const int nv = r_b.verts.size();
	LocalVector<int> remap;
	remap.resize(nv);
	for (int v = 0; v < nv; v++) {
		remap[v] = -1;
	}
	auto mark = [&](const PackedInt32Array &p_idx) {
		for (int i = 0; i < p_idx.size(); i++) {
			remap[p_idx[i]] = 0;
		}
	};
	mark(r_b.indices);
	for (const KeyValue<Variant, Variant> &kv : r_lods) {
		mark(kv.value);
	}
	int n = 0;
	for (int v = 0; v < nv; v++) {
		if (remap[v] == 0) {
			remap[v] = n++;
		}
	}
	const bool tang = r_b.use_tangents && r_b.tangents.size() >= nv * 4;
	const bool skin = r_b.bones.size() >= nv * 4 && r_b.weights.size() >= nv * 4;
	for (int v = 0; v < nv; v++) {
		const int to = remap[v];
		if (to < 0 || to == v) {
			continue;
		}
		r_b.verts.write[to] = r_b.verts[v];
		r_b.normals.write[to] = r_b.normals[v];
		r_b.uvs.write[to] = r_b.uvs[v];
		r_b.colors.write[to] = r_b.colors[v];
		if (r_uv2) {
			r_uv2->write[to] = (*r_uv2)[v];
		}
		for (int k = 0; k < 4; k++) {
			if (tang) {
				r_b.tangents.write[to * 4 + k] = r_b.tangents[v * 4 + k];
			}
			if (skin) {
				r_b.bones.write[to * 4 + k] = r_b.bones[v * 4 + k];
				r_b.weights.write[to * 4 + k] = r_b.weights[v * 4 + k];
			}
		}
	}
	r_b.verts.resize(n);
	r_b.normals.resize(n);
	r_b.uvs.resize(n);
	r_b.colors.resize(n);
	if (r_uv2) {
		r_uv2->resize(n);
	}
	if (tang) {
		r_b.tangents.resize(n * 4);
	}
	if (skin) {
		r_b.bones.resize(n * 4);
		r_b.weights.resize(n * 4);
	}
	auto apply = [&](PackedInt32Array &r_idx) {
		int32_t *w = r_idx.ptrw();
		for (int i = 0; i < r_idx.size(); i++) {
			w[i] = remap[w[i]];
		}
	};
	apply(r_b.indices);
	Dictionary out;
	for (const KeyValue<Variant, Variant> &kv : r_lods) {
		PackedInt32Array idx = kv.value;
		if (idx.size() >= r_b.indices.size()) {
			continue;
		}
		apply(idx);
		out[kv.key] = idx;
	}
	r_lods = out;
	r_b.parts.clear();
	r_b.part_size.clear();
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
			// A palm's fan (sized; a fan of a dozen and more segments) stays whole and at its size:
			// grown about its base it would reach metres out.
			// Its spines (sized negative) are LOD 0 only.
			const float psize = i < r_leaves.part_size.size() ? r_leaves.part_size[i] : 0.0f;
			const bool whole = psize > 0.0f;
			if (psize < 0.0f || (u >= level.keep && !whole)) {
				continue;
			}
			const float g = whole ? 1.0f : grow;
			const Vector4i pt = r_leaves.parts[i];
			const int v0 = r_leaves.verts.size();
			const Vector3 pivot = r_leaves.verts[pt.z];
			// A species leaf (lobed, palmate, compound) is drawn as its simple twin here.
			const Vector4i *twin = p_twins ? leaf_twin_of.getptr(int(i)) : nullptr;
			const Builder &src = twin ? leaf_twins : r_leaves;
			const Vector4i sp = twin ? *twin : pt;
			const bool skinned = r_leaves.bones.size() >= (pt.z + 1) * 4;
			for (int v = 0; v < sp.w; v++) {
				r_leaves.verts.push_back(pivot + (src.verts[sp.z + v] - pivot) * g);
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
	// Gorse and broom: light-demanding heath shrubs (Ellenberg L 7-8), gorse oceanic and cut back
	// by hard frost, broom a little hardier (Atlas of the British and Irish Flora, Preston et al.
	// 2002); vegetation places them (trees.md 5u).
	{ 6, 15, -3, 9, 600, 2500, 0.5f, 45, 0.1f, 0, 0.3f, 0.80f, 1.6f, 0.5f }, // Gorse
	{ 5, 15, -6, 9, 500, 2000, 0.5f, 45, 0.1f, 0, 0.3f, 0.45f, 1.6f, 0.4f }, // Broom
	// Mediterranean scrub (trees.md 5v; Blondel et al. 2010, Flora iberica): mild winters,
	// summer drought; vegetation places them with PlantGenerator.apply_scrub_rules.
	{ 12, 19, 3, 13, 300, 1100, 0.2f, 40, 0.1f, 0, 0.2f, 0.55f, 1.4f, 0.5f }, // Gum rockrose
	{ 12, 20, 3, 14, 250, 1000, 0.2f, 45, 0.05f, 0, 0.3f, 0.85f, 1.8f, 0.6f }, // Kermes oak
	{ 13, 20, 4, 14, 250, 1100, 0.2f, 45, 0.1f, 0, 0.35f, 0.85f, 2.2f, 0.7f }, // Lentisk
	{ 15, 20, 7, 14, 250, 900, 0.15f, 40, 0.05f, 0, 0.2f, 0.6f, 2.5f, 0.8f }, // Dwarf fan palm
	// Stone pine: Mediterranean coasts, sands and dunes, hardy to about -12 C (Gymnosperm
	// Database; Atlas 2016); light-demanding, open stands and parkland.
	{ 13, 19, 3, 13, 250, 1200, 0.2f, 30, 0.3f, 0, 0.3f, 0.6f, 9.0f, 1.2f }, // Stone pine
	// Cork oak: western Mediterranean, oceanic-influenced, on acid soils; needs 450 mm or more and
	// mild winters (Pereira 2007).
	{ 13, 19, 4, 12, 450, 1200, 0.3f, 35, 0.3f, 0, 0.4f, 0.6f, 10.0f, 1.2f }, // Cork oak
	// Holm oak: the whole Mediterranean, limestone too, dry summers, hardy to about -15 C; casts
	// deep shade (Rameau et al. 2008).
	{ 11, 19, 1, 12, 350, 1500, 0.25f, 45, 0.15f, 0, 0.6f, 0.8f, 7.0f, 1.0f }, // Holm oak
	// Wild olive: thermo-Mediterranean, frost-tender (about -8 C), the driest lowland sites.
	{ 14, 21, 5, 14, 250, 900, 0.15f, 45, 0.1f, 0, 0.3f, 0.6f, 6.0f, 0.8f }, // Wild olive
	// Maritime pine: western Mediterranean and Atlantic coasts, acid sands, needs some summer rain.
	{ 11, 18, 2, 11, 500, 1500, 0.35f, 40, 0.3f, 0, 0.2f, 0.5f, 6.0f, 1.0f }, // Maritime pine
	// Carob: the warmest coasts, frost-tender (about -4 C), limestone, dry summers.
	{ 16, 22, 8, 15, 250, 800, 0.15f, 35, 0.2f, 0, 0.35f, 0.75f, 9.0f, 1.0f }, // Carob
	// Blackthorn: hedges, scrub and wood edges of all lowland Europe; light-demanding.
	{ 4, 15, -15, 9, 400, 1400, 0.4f, 40, 0.15f, 0, 0.4f, 0.70f, 2.5f, 0.6f }, // Blackthorn
	// Field maple: lowland hedges and woods on base-rich soils, temperate Europe to the Caspian.
	{ 6, 14, -10, 7, 450, 1200, 0.45f, 35, 0.3f, 0, 0.6f, 0.80f, 6.0f, 0.9f }, // Field maple
	// Dog rose: hedges and scrub nearly everywhere in Europe below the mountains.
	{ 3, 16, -15, 10, 350, 1500, 0.35f, 40, 0.1f, 0, 0.4f, 0.55f, 2.0f, 0.5f }, // Dog rose
};

static bool is_shrub(TreeGenerator::Preset p_preset) {
	return p_preset == TreeGenerator::PRESET_HAZEL || p_preset == TreeGenerator::PRESET_HAWTHORN || p_preset == TreeGenerator::PRESET_ELDER || p_preset == TreeGenerator::PRESET_HOLLY || p_preset == TreeGenerator::PRESET_BOX ||
			p_preset == TreeGenerator::PRESET_WITCH_HAZEL || p_preset == TreeGenerator::PRESET_MOUNTAIN_LAUREL || p_preset == TreeGenerator::PRESET_GORSE ||
			p_preset == TreeGenerator::PRESET_BROOM || p_preset == TreeGenerator::PRESET_GUM_ROCKROSE || p_preset == TreeGenerator::PRESET_KERMES_OAK ||
			p_preset == TreeGenerator::PRESET_LENTISK || p_preset == TreeGenerator::PRESET_BLACKTHORN || p_preset == TreeGenerator::PRESET_DOG_ROSE;
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
	// The tree line: trees stop where the growing season's mean falls to 6.4 C, whatever the
	// annual mean (Paulsen and Korner 2014; an oceanic mountain's annual mean there is several
	// degrees above a continental one's). The same rule and fade as the alpine plants
	// (PlantGenerator::make_alpine, full to 6.4 C and gone at 7.2 C), mirrored, so the two
	// cross-fade over the same 0.8 C instead of overlapping or leaving a bare band.
	p_species->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(7.2f, 100.0f));
	p_species->set_rule_fade(PlantSpecies::RULE_SEASON_TEMPERATURE, 0.8f);
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
	} else if (p_form == FORM_FLAGGED || p_form == FORM_KRUMMHOLZ) {
		// Exposure varies over the variants: a flagged tree from half its limbs gone to a few
		// lee limbs on a short stem; krummholz mats with and without a flag stem.
		const uint32_t h = uint32_t(seed) * 2654435761u;
		const float t = float((h >> 8) & 255u) / 255.0f;
		wind_exposure = p_form == FORM_FLAGGED ? Math::lerp(0.6f, 0.95f, t) : 1.0f;
		krummholz = p_form == FORM_KRUMMHOLZ ? Math::lerp(0.8f, 1.0f, t) : 0.0f;
	} else if (p_form == FORM_BANK) {
		// Bank trees (trees.md 5q): the variants step through slopes so placement can pick the
		// nearest (0, 20, 34 and 49 degrees), a level one among them so gentle ground still gets
		// a sensible tree. The steeper the bank the more of the soil has washed from under the
		// downhill roots (Gartner 2007); one steep variant in eight stands over a boulder, as on
		// stony stream banks and gorge sides.
		// The 34 degree bank first: a single-variant stand (scouts, tests) gets that one; the
		// level tree is the stand's own open-grown form.
		static const float slopes[4] = { 0.6f, 0.0f, 0.85f, 0.35f };
		const uint32_t h = uint32_t(seed) * 2654435761u;
		const float t = float((h >> 8) & 255u) / 255.0f;
		ground_slope = slopes[p_variant % 4];
		bank_erosion = ground_slope > 0.0f ? Math::lerp(0.3f, 0.9f, t) * Math::lerp(0.6f, 1.0f, ground_slope / 0.85f) : 0.0f;
		root_stone = p_variant % 8 == 2 ? Math::lerp(0.35f, 0.6f, float((h >> 16) & 255u) / 255.0f) : 0.0f;
	} else if (p_form == FORM_HEDGE) {
		// A cut field hedge (trees.md 5ad): two metres of hedge, several stems planted along it,
		// flailed every winter to an A-shape (even variants) or a box, 1.8-2.5 m tall and 1.6-2.4 m
		// wide at the base. Each cut stub breaks into short shoots (the envelope stops a shoot
		// where the flail took it), so the hedge grows a dense twiggy shell, leafy to the ground.
		TreeGenerator look;
		look.set_preset(p_preset);
		const uint32_t h = uint32_t(seed) * 2654435761u;
		const float t = float((h >> 8) & 255u) / 255.0f, t2 = float((h >> 16) & 255u) / 255.0f;
		for (int k = 0; k < PARAM_MAX; k++) {
			params[k] = look.params[k];
		}
		const float top = Math::lerp(1.8f, 2.5f, t);
		const float base_w = Math::lerp(1.6f, 2.4f, t2);
		envelope = AABB(Vector3(-1.0f, -1.0f, -base_w * 0.5f), Vector3(2.0f, 1.0f + top, base_w));
		envelope_batter = p_variant % 2 == 0 ? 1.0f - Math::lerp(0.6f, 1.0f, t2) / base_w : 0.0f;
		envelope_sprouts = 2;
		// Planted in a staggered double row, five to the metre, and leaning out along the line:
		// stems from all along the segment's foot fill its length.
		params[PARAM_STEMS] = Math::round(MAX(look.params[PARAM_STEMS], 1.0f) + 6.0f);
		params[PARAM_STEM_SPREAD] = 55.0f;
		params[PARAM_BASAL_SPROUTS] = MAX(look.params[PARAM_BASAL_SPROUTS], 0.6f);
		params[PARAM_SHADOW_STRENGTH] = MIN(look.params[PARAM_SHADOW_STRENGTH], 0.05f);
		// Cut every year for decades: 15-30 years of growth, the stems no older.
		params[PARAM_YEARS] = Math::round(CLAMP(look.params[PARAM_YEARS] * 0.8f, 15.0f, 30.0f));
		params[PARAM_BUD_LIGHT_MIN] = 0.0f;
		// The flail takes the tops and the sides each year: buds all along the stems break, and the
		// side shoots, in full light on the hedge's face, keep their leaves (a bare-sided hedge of
		// upright stems under a leafy lid read wrong).
		params[PARAM_SHED_RATIO] = 0.0f;
		params[PARAM_APICAL_DOMINANCE] = look.params[PARAM_APICAL_DOMINANCE] * 0.6f;
		params[PARAM_BRANCH_ANGLE] = MAX(look.params[PARAM_BRANCH_ANGLE], 65.0f);
		params[PARAM_LEAF_YEARS] = look.params[PARAM_LEAF_YEARS] + 2.0f;
		params[PARAM_LEAVES_PER_INTERNODE] = look.params[PARAM_LEAVES_PER_INTERNODE] * 1.5f;
		// Cutting wakes the dormant buds all down the stems (hedge regrowth comes from them, not
		// only from the cut ends), so they wait longer to break than on an uncut bush.
		params[PARAM_BUD_IDLE_YEARS] = MAX(look.params[PARAM_BUD_IDLE_YEARS], 8.0f);
		// The shell is thousands of thin twigs: under da Vinci's rule they would sum into stems as
		// thick as an uncut tree's; a hedge's stems stay a few centimetres.
		params[PARAM_PIPE_EXPONENT] = MAX(look.params[PARAM_PIPE_EXPONENT], 2.5f);
		params[PARAM_GRAVITY_BENDING] = MIN(look.params[PARAM_GRAVITY_BENDING], 0.2f);
		// Regrowth after the flail is short and stubby, shattered ends with tufts of shoots, not
		// long single whips over a flat lid (docs, 18 Nov).
		params[PARAM_INTERNODE_LENGTH] = look.params[PARAM_INTERNODE_LENGTH] * 0.6f;
		// Leaves on cut hedges run small, many to a short shoot.
		params[PARAM_LEAF_SIZE] = look.params[PARAM_LEAF_SIZE] * (p_preset == PRESET_HAZEL || p_preset == PRESET_FIELD_MAPLE ? 0.65f : 0.85f);
	} else if (p_form == FORM_THICKET || p_form == FORM_OLD) {
		// Heath shrubs (trees.md 5u), from the preset's own numbers each variant.
		TreeGenerator look;
		look.set_preset(p_preset);
		const uint32_t h = uint32_t(seed) * 2654435761u;
		const float t = float((h >> 8) & 255u) / 255.0f;
		for (int k = 0; k < PARAM_MAX; k++) {
			params[k] = look.params[k];
		}
		if (p_form == FORM_THICKET) {
			// Several plants grown into one stand: stems from all round, splayed out low, leafy
			// to the ground at the edges, wider than tall.
			params[PARAM_STEMS] = Math::round(look.params[PARAM_STEMS] * 2.5f);
			params[PARAM_STEM_SPREAD] = MIN(look.params[PARAM_STEM_SPREAD] + 12.0f, 85.0f);
			params[PARAM_BASAL_SPROUTS] = 0.7f;
			params[PARAM_YEARS] = look.params[PARAM_YEARS] + 3.0f;
			params[PARAM_BUD_LIGHT_MIN] = 0.0f;
			params[PARAM_SHED_RATIO] = look.params[PARAM_SHED_RATIO] * 0.5f;
			params[PARAM_TRUNK_GRAVITROPISM] = look.params[PARAM_TRUNK_GRAVITROPISM] * 0.6f;
			// The wind and grazing take the tallest leaders: a low, broad top.
			// Thicket tops: gorse 1.3 m, broom 1.7, gum rockrose 1.8, kermes oak 1.4, lentisk 2.
			const float top = (p_preset == PRESET_BROOM ? 1.7f : (p_preset == PRESET_GUM_ROCKROSE ? 1.8f : (p_preset == PRESET_KERMES_OAK ? 1.4f : (p_preset == PRESET_LENTISK ? 2.0f : 1.3f)))) * Math::lerp(0.9f, 1.1f, t);
			envelope = AABB(Vector3(-4.0f, -1.0f, -4.0f), Vector3(8.0f, 1.0f + top, 8.0f));
		} else {
			// Old (BSBI: gorse lives 15-20 years, grows top-heavy and keels over; broom 10-20,
			// its branches leaning out and thinning, GOERT): older, the inside shaded out and
			// dead but standing, the stems splayed.
			// Gum rockrose lives 15-20 years but stops gaining height early: old plants are leggy
			// and sparse, not taller (Flora iberica).
			params[PARAM_YEARS] = Math::round(look.params[PARAM_YEARS] * Math::lerp(1.5f, 1.7f, t));
			if (p_preset == PRESET_GUM_ROCKROSE) {
				params[PARAM_INTERNODE_LENGTH_AGE] = 0.86f;
			}
			params[PARAM_BUD_LIGHT_MIN] = MAX(look.params[PARAM_BUD_LIGHT_MIN], 0.2f);
			params[PARAM_SHED_RATIO] = look.params[PARAM_SHED_RATIO] * 1.5f;
			params[PARAM_DEAD_BRANCH_KEEP] = 0.9f;
			params[PARAM_STEM_SPREAD] = MIN(look.params[PARAM_STEM_SPREAD] + 15.0f, 85.0f);
			params[PARAM_TRUNK_GRAVITROPISM] = look.params[PARAM_TRUNK_GRAVITROPISM] * 0.5f;
			params[PARAM_BASAL_SPROUTS] = look.params[PARAM_BASAL_SPROUTS] * 0.3f;
			params[PARAM_GRAVITY_BENDING] = 0.25f;
		}
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
		// 64 bits: presets run past 31 (a 32-bit shift by them was undefined).
		const uint64_t bit = 1ull << int(p_preset);
		const uint64_t ivy_hosts = (1ull << PRESET_OAK) | (1ull << PRESET_BEECH) | (1ull << PRESET_ASH) | (1ull << PRESET_SYCAMORE_MAPLE) | (1ull << PRESET_LIME) |
				(1ull << PRESET_BLACK_ALDER) | (1ull << PRESET_WHITE_WILLOW) | (1ull << PRESET_HORSE_CHESTNUT) | (1ull << PRESET_HAWTHORN) | (1ull << PRESET_ELDER) |
				(1ull << PRESET_SUGAR_MAPLE) | (1ull << PRESET_RED_MAPLE) | (1ull << PRESET_RED_OAK) | (1ull << PRESET_WHITE_OAK) | (1ull << PRESET_AMERICAN_BEECH) | (1ull << PRESET_YELLOW_BIRCH);
		const uint64_t liana_hosts = (1ull << PRESET_OAK) | (1ull << PRESET_ASH) | (1ull << PRESET_LIME) | (1ull << PRESET_SYCAMORE_MAPLE) | (1ull << PRESET_BLACK_ALDER) |
				(1ull << PRESET_WHITE_WILLOW) | (1ull << PRESET_SUGAR_MAPLE) | (1ull << PRESET_RED_MAPLE) | (1ull << PRESET_RED_OAK) | (1ull << PRESET_WHITE_OAK);
		const uint64_t twine_hosts = (1ull << PRESET_HAZEL) | (1ull << PRESET_HAWTHORN) | (1ull << PRESET_ROWAN) | (1ull << PRESET_ELDER) | (1ull << PRESET_WITCH_HAZEL) |
				(1ull << PRESET_BLACKTHORN) | (1ull << PRESET_FIELD_MAPLE);
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
	envelope_batter = p_from.envelope_batter;
	envelope_sprouts = p_from.envelope_sprouts;
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
	cork_stripped_years = p_from.cork_stripped_years;
	bark_wounds = p_from.bark_wounds;
	bark_chevrons = p_from.bark_chevrons;
	wind_exposure = p_from.wind_exposure;
	ground_slope = p_from.ground_slope;
	root_stone = p_from.root_stone;
	bank_erosion = p_from.bank_erosion;
	krummholz = p_from.krummholz;
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
	leaflet_entire_texture = p_from.leaflet_entire_texture;
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
	p_stand_grown = p_form != FORM_OPEN && p_form != FORM_POLLARD && p_form != FORM_FLAGGED && p_form != FORM_KRUMMHOLZ && p_form != FORM_BANK && p_form != FORM_THICKET && p_form != FORM_OLD && p_form != FORM_HEDGE &&
			!(p_form == FORM_SAPLING && scrub_shrub(p_preset));
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
		// Gorse's spines are short and its flowers sit among them: sprays (quads a needle long
		// either side of the shoot) hid the flowers, and its spines as geometry cost about what
		// its flowers do (trees.md 5u).
		needle_sprays = p_preset != PRESET_GORSE;
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
		const float saved_exposure = wind_exposure;
		const float saved_krummholz = krummholz;
		const float saved_slope = ground_slope;
		const float saved_erosion = bank_erosion;
		const float saved_stone = root_stone;
		const AABB saved_envelope = envelope;
		const float saved_batter = envelope_batter;
		const int saved_sprouts = envelope_sprouts;
		if (p_form == FORM_SAPLING) {
			// A young heath shrub is three or four years old, already flowering (BSBI, GOERT).
			const bool heath = scrub_shrub(p_preset);
			params[PARAM_YEARS] = heath ? MAX(3.0f, Math::round(params[PARAM_YEARS] * 0.33f)) : MAX(6.0f, Math::round(params[PARAM_YEARS] * 0.2f));
		}
		// Leaf detail near, on demand: lobed and compound leaves are simple lenses in the far
		// meshes, and a copy of this generator, as set up here, builds the full-leaf mesh of a
		// variant when the field asks for it (request_near_mesh()).
		const LeafKind leaf_kind = LEAF_FORMS[p_preset].kind;
		// With far_lod1 every broadleaf splits: its far mesh starts at LOD 1 (trees.md 8d).
		// Gorse's spines are needles but thin stochastically as leaves (trees.md 5x.2): it splits too.
		const bool near_split = near_leaf_range > 0.0f && p_variants > 0 && (params[PARAM_NEEDLES] <= 0.5f || scrub_shrub(Preset(p_preset))) &&
				(far_lod1 || leaf_kind == LEAF_LOBED || leaf_kind == LEAF_PALMATE || leaf_kind == LEAF_PINNATE || leaf_kind == LEAF_DIGITATE) &&
				(p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_UNDERSTORY || p_form == FORM_SAPLING || p_form == FORM_COPPICE || p_form == FORM_COPPICE_YOUNG || p_form == FORM_POLLARD || p_form == FORM_FLAGGED || p_form == FORM_KRUMMHOLZ || p_form == FORM_BANK || p_form == FORM_THICKET || p_form == FORM_OLD || p_form == FORM_HEDGE);
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
		wind_exposure = saved_exposure;
		krummholz = saved_krummholz;
		envelope = saved_envelope;
		envelope_batter = saved_batter;
		envelope_sprouts = saved_sprouts;
		ground_slope = saved_slope;
		bank_erosion = saved_erosion;
		root_stone = saved_stone;
		mesh_cache[key] = meshes;
		crown_cache[key] = crown / n;
		// One full-needle variant for the near range (the field repeats it over the variants):
		// about 30M vertices for a spruce, so one only, and no LOD copies since it is only drawn
		// close. GPU memory and path tracing cost of it are not yet measured.
		if (params[PARAM_NEEDLES] > 0.5f && near_needle_range > 0.0f && p_variants > 0 && (p_form == FORM_OPEN || p_form == FORM_STAND || p_form == FORM_UNDERSTORY || p_form == FORM_FLAGGED || p_form == FORM_KRUMMHOLZ || p_form == FORM_THICKET || p_form == FORM_OLD)) {
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
	if (p_form == FORM_FLAGGED || p_form == FORM_KRUMMHOLZ) {
		// Placement turns each instance's +X down the prevailing wind instead of a random yaw
		// (the meshes carry tree_wind_flag; trees.md 5q).
		sp->set_meta("wind_flag", true);
	}
	static const char *form_names[FORM_MAX] = { " (open)", " (stand)", " (sapling)", " (snag)", " (log)", " (fallen branch)", " (sticks)", " (understory)", " (coppice)", " (young coppice)", " (pollard)", " (flagged)", " (krummholz)", " (bank)", " (thicket)", " (old)", " (hedge)" };
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
			p_preset == PRESET_WHITE_PINE || p_preset == PRESET_HEMLOCK || p_preset == PRESET_STONE_PINE || p_preset == PRESET_MARITIME_PINE;
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
		if (p_preset == PRESET_GORSE || p_preset == PRESET_BROOM) {
			// Heath shrubs: their green stems and spines hide the ground whatever the season, and a
			// bush in full flower is gold from afar (about half its outside in flowers, trees.md 5u).
			const float bloom = get_bloom(p_preset, season.get("day_of_year", 182.0f)) * (p_preset == PRESET_GORSE ? 0.55f : 0.45f);
			const Color gold = Color(0.95f, 0.68f, 0.03f);
			const Color stems = p_preset == PRESET_BROOM ? Color(0.06f, 0.14f, 0.03f) : far;
			far = Color(stems.r, stems.g, stems.b).lerp(gold, bloom);
			far.a = p_form == FORM_SAPLING ? 0.6f : 0.85f;
		}
		if (p_preset == PRESET_HAWTHORN || p_preset == PRESET_BLACKTHORN || p_preset == PRESET_DOG_ROSE) {
			// A hedge in blossom is white from afar: hawthorn smothered in May, blackthorn white on
			// bare black wood, dog rose pink-flecked (trees.md 5ad).
			const float bloom = get_bloom(p_preset, season.get("day_of_year", 182.0f)) * (p_preset == PRESET_DOG_ROSE ? 0.15f : (p_preset == PRESET_HAWTHORN ? 0.5f : 0.6f));
			const float a = Math::lerp(far.a, MAX(far.a, 0.8f), bloom);
			far = far.lerp(Color(0.93f, 0.92f, 0.9f), bloom);
			far.a = a;
		}
		if (p_preset == PRESET_GUM_ROCKROSE) {
			// A jara in flower is speckled white from afar: a flower 7 cm across on about a third
			// of the shoot tips at the peak (trees.md 5v).
			const float bloom = get_bloom(p_preset, season.get("day_of_year", 182.0f)) * 0.3f;
			far = Color(far.r, far.g, far.b, far.a).lerp(Color(0.92f, 0.91f, 0.88f, far.a), bloom);
		}
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

// A heath shrub species for vegetation (trees.md 5u): the meshes of one form, its footprint,
// canopy radius and far colour on the season's day; placement and its climate and terrain rules
// are vegetation's (PlantGenerator.apply_scrub_rules and its own).
Ref<PlantSpecies> TreeGenerator::make_shrub_species(Preset p_preset, ShrubForm p_form, int p_variants, const Dictionary &p_season) {
	ERR_FAIL_COND_V_MSG(!scrub_shrub(p_preset) && !hedge_shrub(p_preset), Ref<PlantSpecies>(), "Not a heath, scrub or hedge shrub preset.");
	ERR_FAIL_INDEX_V(int(p_form), int(SHRUB_MAX), Ref<PlantSpecies>());
	static const Form forms[SHRUB_MAX] = { FORM_SAPLING, FORM_OPEN, FORM_OLD, FORM_THICKET, FORM_KRUMMHOLZ, FORM_HEDGE };
	static const char *names[SHRUB_MAX] = { " (young)", "", " (old)", " (thicket)", " (cushion)", " (hedge)" };
	Ref<TreeGenerator> g;
	g.instantiate();
	g->set_preset(p_preset);
	g->season = p_season.duplicate(true);
	// Gum rockrose is not wind-clipped into cushions (an erect, short-lived seeder): its low form
	// is the young plant.
	Form form = p_preset == PRESET_GUM_ROCKROSE && p_form == SHRUB_CUSHION ? FORM_SAPLING : forms[p_form];
	// Hedge shrubs grown wild have no heath thicket or wind cushion: their thicket is the open
	// shrub, their low form the young one.
	if (hedge_shrub(p_preset) && !scrub_shrub(p_preset) && (p_form == SHRUB_THICKET || p_form == SHRUB_CUSHION)) {
		form = p_form == SHRUB_THICKET ? FORM_OPEN : FORM_SAPLING;
	}
	Ref<PlantSpecies> sp = g->_stand_species(p_preset, false, MAX(p_variants, 1), form);
	ERR_FAIL_COND_V(sp.is_null(), sp);
	// No climate, terrain or shade rules: a fresh species' ranges.
	Ref<PlantSpecies> plain;
	plain.instantiate();
	for (int r = 0; r < PlantSpecies::RULE_MAX; r++) {
		sp->set_rule_range(PlantSpecies::Rule(r), plain->get_rule_range(PlantSpecies::Rule(r)));
		sp->set_rule_fade(PlantSpecies::Rule(r), plain->get_rule_fade(PlantSpecies::Rule(r)));
	}
	sp->set_name(String(preset_names[p_preset]) + names[p_form]);
	sp->set_meta("shrub_form", int(p_form));
	if (form == FORM_HEDGE) {
		// Two metres of hedge along local x, laid end to end along a field edge by the caller: no
		// random yaw, one segment every two metres (trees.md 5ad).
		sp->set_random_yaw(false);
		sp->set_spacing(2.0f);
		sp->set_meta("hedge_segment_length", 2.0f);
	}
	// The source keeps the near meshes alive through the species (_near_mesh_source meta).
	return sp;
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
			// Ravine woods on the gorge sides (Tilio-Acerion: sycamore, ash and lime on steep,
			// unstable, stony slopes; Ellenberg 1988): sycamore bank trees past about 25 degrees.
			Ref<PlantSpecies> ravine = _stand_species(PRESET_SYCAMORE_MAPLE, false, _bank_variants(p_variants), FORM_BANK);
			ravine->set_density(0.4f);
			ravine->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(25.0f, 90.0f));
			ravine->set_rule_fade(PlantSpecies::RULE_SLOPE, 6.0f);
			ravine->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
			ravine->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
			list.push_back(ravine);
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
			// Subalpine up to the tree line (Korner 2012), its cold end set by the growing
			// season's mean (apply_habitat), about -1.5 C annual mean in the Alps but warmer on
			// an oceanic mountain.
			const Vector2 belt(-100.0f, 2.5f);
			clip(canopy(PRESET_LARCH, 0.8f, 50.0f, 0.6f, false, true), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(canopy(PRESET_NORWAY_SPRUCE, 0.8f, 50.0f, 0.6f, true, false), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(open(PRESET_LARCH, 0.4f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			clip(open(PRESET_ROWAN, 0.4f), PlantSpecies::RULE_TEMPERATURE, belt, 1.0f);
			// Past the closed forest (the timberline) to the tree species line: wind-flagged
			// spruce in the open, then krummholz mats in the lee of rocks and hollows, where snow
			// lies (Holtmeier 2009; Korner 2012). Beyond the stand's habitat range on purpose:
			// this is where it fails.
			Ref<PlantSpecies> flagged = _stand_species(PRESET_NORWAY_SPRUCE, false, p_variants, FORM_FLAGGED);
			flagged->set_density(0.3f);
			// Between the closed forest and the tree line, by the growing season's mean (the
			// alpine plants' rule): 6.4-8.2 C.
			flagged->set_rule_range(PlantSpecies::RULE_TEMPERATURE, Vector2(-100.0f, 2.5f));
			flagged->set_rule_fade(PlantSpecies::RULE_TEMPERATURE, 1.0f);
			flagged->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(6.8f, 7.8f));
			flagged->set_rule_fade(PlantSpecies::RULE_SEASON_TEMPERATURE, 0.4f);
			list.push_back(flagged);
			Ref<PlantSpecies> mat = _stand_species(PRESET_NORWAY_SPRUCE, false, p_variants, FORM_KRUMMHOLZ);
			mat->set_density(0.35f);
			// Past the tree line among the alpine plants, a degree of season mean above it:
			// 5.2-7 C (the tree species line; Korner 2012).
			mat->set_rule_range(PlantSpecies::RULE_TEMPERATURE, Vector2(-100.0f, 2.5f));
			mat->set_rule_fade(PlantSpecies::RULE_TEMPERATURE, 1.0f);
			mat->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(5.6f, 6.6f));
			mat->set_rule_fade(PlantSpecies::RULE_SEASON_TEMPERATURE, 0.4f);
			list.push_back(mat);
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
			// On the banks themselves, alder and willow lean out over the water with their roots
			// washed bare (trees.md 5q). Placement picks each tree's slope variant.
			Ref<PlantSpecies> alder_bank = _stand_species(PRESET_BLACK_ALDER, false, _bank_variants(p_variants), FORM_BANK);
			alder_bank->set_density(0.5f);
			alder_bank->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.0f, 12.0f));
			alder_bank->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 6.0f);
			alder_bank->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
			alder_bank->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
			list.push_back(alder_bank);
			Ref<PlantSpecies> willow_bank = _stand_species(PRESET_WHITE_WILLOW, false, _bank_variants(p_variants), FORM_BANK);
			willow_bank->set_density(0.3f);
			willow_bank->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.0f, 8.0f));
			willow_bank->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 4.0f);
			willow_bank->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.55f));
			willow_bank->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
			list.push_back(willow_bank);
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
		case STAND_STONE_PINE: {
			lead = PRESET_STONE_PINE;
			// Stone pine on coastal sands (trees.md 5y): open woods and parkland of umbrella crowns
			// on clear boles, kept open for the cones and grazing, the crowns near touching
			// (pinhal manso; Mutke et al. 2012: 50-150 trees a hectare in productive stands). Open
			// grown, at about a crown's width apart; the scrub beneath (lentisk, kermes oak,
			// gum rockrose, dwarf fan palm) is PlantGenerator.make_scrub's on the same ground.
			Ref<PlantSpecies> pine = _stand_species(PRESET_STONE_PINE, false, p_variants);
			pine->set_density(0.8f);
			pine->set_spacing(MAX(pine->get_spacing(), 11.0f));
			list.push_back(pine);
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
	// Old oleasters are gnarled: fluted, twisted, burred and hollowing at the base (trees.md 5ac),
	// so a wild olive always grows at least part veteran. Restored on return.
	struct VeteranRestore {
		float &value;
		const float saved;
		~VeteranRestore() { value = saved; }
	} veteran_restore{ veteran, veteran };
	if (preset == PRESET_WILD_OLIVE && decay_class == 0) {
		veteran = MAX(veteran, 0.5f);
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	nodes.clear();
	buds.clear();
	sprout_roots.clear();
	cut_heads.clear();
	cut_stubs.clear();
	thinned_stubs.clear();
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
	const bool palm = preset == PRESET_DWARF_FAN_PALM;
	for (int s = 0; s < stems && !palm; s++) {
		const float tilt = stems == 1 ? _rand(0.0f, 0.04f) : spread * _rand(0.4f, 1.0f);
		add_stem_bud(Math::TAU * s / stems + _rand(-0.4f, 0.4f), tilt);
	}
	palm_fronds.clear();
	palm_trunks.clear();
	palm_crowns.clear();
	if (palm) {
		_grow_palm();
	}
	// Mats at the tree line are old, their growth a few centimetres a year (Holtmeier 2009): the
	// cushion fills over many more years than an upright tree takes to its height.
	const int years = palm ? 0 : CLAMP(int(params[PARAM_YEARS] * (1.0f + 0.9f * krummholz)), 1, 300);
	LocalVector<Vector3> layer_roots;
	int last_cut = -1;
	int first_cut = -1;
	int cuts = 0;
	last_cut_year = -1000;
	const int since = cut_since >= 0 ? cut_since : 2 + int(uint32_t(seed) * 2654435761u % uint32_t(MAX(cut_cycle - 2, 1)));
	for (int y = 1; y <= years; y++) {
		// A cut tree's sprouts are its poles, from the head or the stool: no more stems from the base.
		// A krummholz mat thickens from the base and from limbs that root where they touch the
		// ground (layering), so it stands on many stems.
		const float sprouts = MAX(params[PARAM_BASAL_SPROUTS], 0.06f * krummholz);
		if (sprouts > 0.0f && rng.randf() < sprouts && last_cut < 0) {
			add_stem_bud(_rand(0.0f, Math::TAU), spread * _rand(0.3f, 1.0f));
		}
		_grow_year(y);
		if (krummholz > 0.0f && y > 6 && int(layer_roots.size()) < 20 && rng.randf() < 0.5f * krummholz) {
			// Layering (Cooper 1911; Laberge et al. 2000): a mat limb pressed to the ground under
			// the snow roots where it lies, and its tip or a bud on it turns up as a new stem, so a
			// mat stands on many stems spread over its width, each with a skirt of its own.
			const float ground = nodes[0].start.y;
			LocalVector<int> cand;
			for (int i = 1; i < int(nodes.size()); i++) {
				const Node &n = nodes[i];
				if (n.removed || n.dead || n.order < 1 || n.layered || n.end().y - ground > 0.1f) {
					continue;
				}
				const Vector2 h(n.end().x - nodes[0].start.x, n.end().z - nodes[0].start.z);
				if (h.length() < 0.5f) {
					continue;
				}
				bool near = false;
				for (const Vector3 &lr : layer_roots) {
					near = near || lr.distance_to(n.end()) < 0.6f;
				}
				if (!near) {
					cand.push_back(i);
				}
			}
			if (!cand.is_empty()) {
				const int i = cand[MIN(int(rng.randf() * cand.size()), int(cand.size()) - 1)];
				layer_roots.push_back(nodes[i].end());
				Vector3 out(nodes[i].end().x - nodes[0].start.x, 0.0f, nodes[i].end().z - nodes[0].start.z);
				Bud b;
				b.node = i;
				b.dir = (Vector3(0, 1, 0) + out.normalized() * 0.35f).normalized();
				b.sprout = true;
				b.layer = true;
				buds.push_back(b);
			}
		}
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
				// Rotted back and snapped short over the decades since: a jagged stub, not a
				// pole's rounded end (that read as a dark ball on the head).
				nodes[r].length *= 0.45f;
				thinned_stubs.push_back(r);
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
	_wind_flag();
	if (!palm) {
		_prune_collisions();
	}
	for (const int r : thinned_stubs) {
		if (r < int(nodes.size()) && !nodes[r].removed) {
			broken_ends.insert(r);
		}
	}
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
	if (wind_exposure > 0.0f && mesh.is_valid()) {
		// Placement turns the mesh's +X down the prevailing wind (trees.md 5q).
		mesh->set_meta(SNAME("tree_wind_flag"), Vector2(wind_exposure, krummholz));
	}
	if (ground_slope > 0.0f && mesh.is_valid() && !fallen && !fallen_branch) {
		// Placement turns the mesh's +Z downhill and picks the variant nearest the ground's slope.
		mesh->set_meta(SNAME("tree_ground_slope"), Vector2(ground_slope, bank_erosion));
	}
	if (stone.on && mesh.is_valid()) {
		// The stone's centre and half extents (mesh space, before any bank shear), for rocks and
		// the floor to keep clear of it.
		mesh->set_meta(SNAME("tree_root_stone"), PackedFloat32Array({ stone.c.x, stone.c.y, stone.c.z, stone.r.x, stone.r.y, stone.r.z }));
	}
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
		// Features to root at (trees.md 5o): records of 8 floats, kind (0 a peel edge, where bark
		// meets bare wood; 1 a drying check), a point on the surface (x, y, z), its outward normal
		// (x, y, z), and the check's half width (0 for an edge). Edges one per 6 cm cell, checks a
		// point every 5 cm along them.
		for (const Wound &w : wounds) {
			if (w.kind != WOUND_CHECK || wound_pts.size() < 2) {
				continue;
			}
			for (float sv = w.s0 + 0.15f; sv < w.s1 - 0.15f; sv += 0.05f) {
				Vector3 out, ax;
				const float az = w.az0 + w.twist * (sv - w.s0);
				int i = 0;
				while (i + 2 < int(wound_arc.size()) && wound_arc[i + 1] < sv) {
					i++;
				}
				const float rr = Math::lerp(wound_rad[i], wound_rad[i + 1], CLAMP((sv - wound_arc[i]) / MAX(wound_arc[i + 1] - wound_arc[i], 1e-6f), 0.0f, 1.0f));
				const Vector3 p = stem_point(wound_pts, wound_arc, sv, az, rr - 0.5f * w.age, out, ax);
				for (float f : { 1.0f, p.x, p.y, p.z, out.x, out.y, out.z, w.half_w }) {
					deadwood_features.push_back(f);
				}
			}
		}
		if (!deadwood_features.is_empty()) {
			mesh->set_meta(SNAME("tree_deadwood_features"), deadwood_features);
		}
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
	ClassDB::bind_method(D_METHOD("set_cork_stripped_years", "years"), &TreeGenerator::set_cork_stripped_years);
	ClassDB::bind_method(D_METHOD("get_cork_stripped_years"), &TreeGenerator::get_cork_stripped_years);
	ClassDB::bind_method(D_METHOD("get_bark_peel"), &TreeGenerator::get_bark_peel);
	ClassDB::bind_method(D_METHOD("set_root_stone", "radius"), &TreeGenerator::set_root_stone);
	ClassDB::bind_method(D_METHOD("get_root_stone"), &TreeGenerator::get_root_stone);
	ClassDB::bind_method(D_METHOD("set_ground_slope", "slope"), &TreeGenerator::set_ground_slope);
	ClassDB::bind_method(D_METHOD("get_ground_slope"), &TreeGenerator::get_ground_slope);
	ClassDB::bind_method(D_METHOD("set_bank_erosion", "erosion"), &TreeGenerator::set_bank_erosion);
	ClassDB::bind_method(D_METHOD("get_bank_erosion"), &TreeGenerator::get_bank_erosion);
	ClassDB::bind_method(D_METHOD("set_wind_exposure", "exposure"), &TreeGenerator::set_wind_exposure);
	ClassDB::bind_method(D_METHOD("get_wind_exposure"), &TreeGenerator::get_wind_exposure);
	ClassDB::bind_method(D_METHOD("set_krummholz", "krummholz"), &TreeGenerator::set_krummholz);
	ClassDB::bind_method(D_METHOD("get_krummholz"), &TreeGenerator::get_krummholz);
	ClassDB::bind_method(D_METHOD("set_bark_wounds", "wounds"), &TreeGenerator::set_bark_wounds);
	ClassDB::bind_method(D_METHOD("get_bark_wounds"), &TreeGenerator::get_bark_wounds);
	ClassDB::bind_method(D_METHOD("set_bark_chevrons", "amount"), &TreeGenerator::set_bark_chevrons);
	ClassDB::bind_method(D_METHOD("set_storm_kit", "enabled"), &TreeGenerator::set_storm_kit);
	ClassDB::bind_method(D_METHOD("get_storm_kit"), &TreeGenerator::get_storm_kit);
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("build_storm_plate", "storm"), &TreeGenerator::build_storm_plate);
	ClassDB::bind_method(D_METHOD("get_bark_chevrons"), &TreeGenerator::get_bark_chevrons);
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
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("make_shrub_species", "preset", "form", "variants", "season"), &TreeGenerator::make_shrub_species, DEFVAL(SHRUB_MATURE), DEFVAL(3), DEFVAL(Dictionary()));
	ClassDB::bind_static_method("TreeGenerator", D_METHOD("get_bloom", "preset", "day_of_year"), &TreeGenerator::get_bloom);
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
	ClassDB::bind_method(D_METHOD("set_far_lod1", "enabled"), &TreeGenerator::set_far_lod1);
	ClassDB::bind_method(D_METHOD("get_far_lod1"), &TreeGenerator::get_far_lod1);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cork_stripped_years", PROPERTY_HINT_RANGE, "-1,30,0.01"), "set_cork_stripped_years", "get_cork_stripped_years");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bark_wounds", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_bark_wounds", "get_bark_wounds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bark_chevrons", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_bark_chevrons", "get_bark_chevrons");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "storm_kit"), "set_storm_kit", "get_storm_kit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_exposure", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wind_exposure", "get_wind_exposure");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "krummholz", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_krummholz", "get_krummholz");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_slope", PROPERTY_HINT_RANGE, "0,1.2,0.01,radians_as_degrees"), "set_ground_slope", "get_ground_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_erosion", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_bank_erosion", "get_bank_erosion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "root_stone", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_root_stone", "get_root_stone");
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_lod1"), "set_far_lod1", "get_far_lod1");
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
	BIND_ENUM_CONSTANT(PRESET_GORSE);
	BIND_ENUM_CONSTANT(PRESET_BROOM);
	BIND_ENUM_CONSTANT(PRESET_GUM_ROCKROSE);
	BIND_ENUM_CONSTANT(PRESET_KERMES_OAK);
	BIND_ENUM_CONSTANT(PRESET_LENTISK);
	BIND_ENUM_CONSTANT(PRESET_DWARF_FAN_PALM);
	BIND_ENUM_CONSTANT(PRESET_STONE_PINE);
	BIND_ENUM_CONSTANT(PRESET_CORK_OAK);
	BIND_ENUM_CONSTANT(PRESET_HOLM_OAK);
	BIND_ENUM_CONSTANT(PRESET_WILD_OLIVE);
	BIND_ENUM_CONSTANT(PRESET_MARITIME_PINE);
	BIND_ENUM_CONSTANT(PRESET_CAROB);
	BIND_ENUM_CONSTANT(PRESET_BLACKTHORN);
	BIND_ENUM_CONSTANT(PRESET_FIELD_MAPLE);
	BIND_ENUM_CONSTANT(PRESET_DOG_ROSE);
	BIND_ENUM_CONSTANT(SHRUB_YOUNG);
	BIND_ENUM_CONSTANT(SHRUB_MATURE);
	BIND_ENUM_CONSTANT(SHRUB_OLD);
	BIND_ENUM_CONSTANT(SHRUB_THICKET);
	BIND_ENUM_CONSTANT(SHRUB_CUSHION);
	BIND_ENUM_CONSTANT(SHRUB_HEDGE);
	BIND_ENUM_CONSTANT(SHRUB_MAX);
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
	BIND_ENUM_CONSTANT(STAND_STONE_PINE);
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

/**************************************************************************/
/*  plant_avalanche.cpp                                                   */
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

// Avalanche tracks (docs/ideas/vegetation.md, "Avalanche tracks"). Where a dense avalanche breaks the stand, WorldStand
// marks the ground DISTURBANCE_AVALANCHE (6, SnowAvalanche3D stand_damage) with the years since. Repeated avalanches keep
// the track out of forest: only plants that bend under the snow or grow back fast hold there. Vegetation and soil are
// torn away where it runs most often, and broken branches are left lying in the debris (ALPECOLe, "Avalanches",
// University of Zurich 2011). The track grows a tall-forb and fern community among bent shrubs: fireweed, lady fern,
// false hellebore and other broad-leaved forbs, salmonberry-like brambles, huckleberry (NRCS ecological site
// F003XN926WA, "Cryic/Udic Active Natural Disturbance", North Cascades; LANDFIRE BpS 10830, North Pacific Avalanche Chute
// Shrubland, 2020). The bent shrubs (green alder, willow) are the trees' (TreeGenerator); this is the ground layer:
// - first years: torn turf and scoured soil, broken twigs and shed needles from the snapped conifers;
// - from the second year: fireweed, then lady fern, raspberry, a broad-leaved tall forb (butterbur's form) and, on the
//   drier ribs, bilberry, holding while the track keeps being struck (its age stays young).

#include "plant_generator.h"
#include "plant_species.h"

TypedArray<Resource> PlantGenerator::make_avalanche_track(int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct Spec {
		Preset preset;
		Vector2 ages; // years since the avalanche
		float age_fade;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale, patch_cover;
		float visibility;
	};
	const Spec set[] = {
		{ P::PRESET_ROOTED_SOD, Vector2(0.0f, 2.0f), 1.0f, 2.2f, 0.4f, 0.35f, 0.8f, 1.4f, 7.0f, 0.35f, 40.0f },
		{ P::PRESET_TWIGS, Vector2(0.0f, 4.0f), 2.0f, 0.6f, 0.15f, 0.5f, 0.8f, 1.3f, 8.0f, 0.45f, 25.0f },
		{ P::PRESET_NEEDLE_LITTER, Vector2(0.0f, 3.0f), 1.5f, 0.5f, 0.2f, 0.4f, 0.8f, 1.2f, 8.0f, 0.4f, 25.0f },
		{ P::PRESET_FIREWEED, Vector2(1.0f, 25.0f), 2.0f, 0.6f, 0.2f, 0.7f, 0.8f, 1.2f, 7.0f, 0.55f, 60.0f },
		{ P::PRESET_FERN, Vector2(2.0f, 60.0f), 3.0f, 0.9f, 0.35f, 0.5f, 0.9f, 1.4f, 6.0f, 0.45f, 50.0f },
		{ P::PRESET_RASPBERRY, Vector2(2.0f, 40.0f), 3.0f, 1.0f, 0.4f, 0.4f, 0.8f, 1.2f, 10.0f, 0.4f, 50.0f },
		{ P::PRESET_BUTTERBUR, Vector2(3.0f, 60.0f), 3.0f, 1.0f, 0.4f, 0.45f, 0.9f, 1.3f, 9.0f, 0.45f, 50.0f },
		{ P::PRESET_BILBERRY, Vector2(4.0f, 60.0f), 3.0f, 0.5f, 0.2f, 0.5f, 0.8f, 1.2f, 12.0f, 0.5f, 40.0f },
	};
	TypedArray<Resource> out;
	int seed_base = 14000;
	for (const Spec &f : set) {
		Dictionary season = p_season.duplicate();
		if (f.preset == P::PRESET_ROOTED_SOD) {
			season["rooting_age"] = 0.3f; // torn last winter, the grass on the flung turf drying
		}
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(seed_base);
		gen->set_season(season);
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
		for (int i = 0; i < v.size(); i++) {
			meshes.push_back(v[i]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(f.footprint);
		sp->set_density(f.density * get_presence(f.preset, p_season));
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		sp->set_align_to_ground(0.3f);
		sp->set_rule_range(PlantSpecies::RULE_STAND_AGE, f.ages);
		sp->set_rule_fade(PlantSpecies::RULE_STAND_AGE, f.age_fade);
		sp->set_rule_range(PlantSpecies::RULE_DISTURBANCE, Vector2(DISTURBANCE_AVALANCHE_KIND, DISTURBANCE_AVALANCHE_KIND));
		sp->set_rule_fade(PlantSpecies::RULE_DISTURBANCE, 0.0f);
		// Tracks run down steep slopes; out of the remaining shade of standing trees at the edge.
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 45.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.6f));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.15f);
		sp->set_patch_scale(f.patch_scale);
		sp->set_patch_cover(f.patch_cover);
		sp->set_patch_seed(seed_base);
		sp->set_visibility_range(f.visibility);
		sp->set_meta("avalanche_track", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

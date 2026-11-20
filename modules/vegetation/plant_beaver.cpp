/**************************************************************************/
/*  plant_beaver.cpp                                                      */
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


// Beaver ponds and the meadows they leave (docs/ideas/vegetation.md, "Beaver ponds and meadows"). An active pond zones by
// water depth and distance from the shore like any still water (Spence 1982), with the dead trees standing in it from the
// trees' STAND_BEAVER_POND. When the dam fails the drained bed becomes a "beaver meadow": rushes and small sedges on the
// bare mud in the first years, then a dense sedge and tall-grass meadow that can hold for decades before willow and alder
// close it (Wright, Jones and Flecker 2003, Ecology 84; Naiman, Johnston and Kelley 1988; Ecoscience 19(3), 2012).
// The meadow is placed by RULE_STAND_AGE with RULE_DISTURBANCE (4, 4): years since the pond drained, on ground WorldStand
// marks with kind 4 (beaver drainage).

#include "plant_generator.h"
#include "plant_species.h"

TypedArray<Resource> PlantGenerator::make_beaver(BeaverStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct BeaverSpec {
		Preset preset;
		Vector2 depth; // Water depth, m; (0, 0) on land only.
		float depth_fade;
		Vector2 water; // Distance from the shore, m, negative in the water; x > y for no rule.
		float water_fade;
		Vector2 ages; // Years since the pond drained; x > y for no rule (the pond itself).
		float age_fade;
		float shade_max;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale, patch_cover;
		float visibility;
	};
	const Vector2 LAND(0.0f, 0.0f);
	const Vector2 ANY(1.0f, 0.0f);
	LocalVector<BeaverSpec> set;
	switch (p_stand) {
		case BEAVER_POND:
			// Floating lilies over the old channel, bulrush in the shallows, tussock sedge along the flooded margin,
			// then soft rush, marsh marigold and meadowsweet up the wet shore.
			set = LocalVector<BeaverSpec>({ { P::PRESET_WATER_LILY, Vector2(0.4f, 2.0f), 0.2f, ANY, 0.0f, ANY, 0.0f, 1.0f, 1.1f, 0.5f, 0.85f, 0.8f, 1.2f, 6.0f, 0.65f, 50.0f },
					{ P::PRESET_CATTAIL, Vector2(0.05f, 0.6f), 0.05f, ANY, 0.0f, ANY, 0.0f, 0.6f, 0.7f, 0.35f, 0.85f, 0.85f, 1.15f, 5.0f, 0.5f, 60.0f },
					{ P::PRESET_SEDGE, Vector2(0.0f, 0.25f), 0.05f, Vector2(-100.0f, 4.0f), 1.5f, ANY, 0.0f, 0.6f, 0.5f, 0.25f, 0.9f, 1.0f, 1.4f, 4.0f, 0.6f, 40.0f },
					{ P::PRESET_RUSH, LAND, 0.05f, Vector2(0.0f, 6.0f), 2.0f, ANY, 0.0f, 0.5f, 0.7f, 0.3f, 0.7f, 0.85f, 1.2f, 4.0f, 0.5f, 40.0f },
					{ P::PRESET_MARSH_MARIGOLD, LAND, 0.05f, Vector2(0.0f, 4.0f), 1.5f, ANY, 0.0f, 0.6f, 0.6f, 0.2f, 0.45f, 0.8f, 1.2f, 3.0f, 0.4f, 30.0f },
					{ P::PRESET_MEADOWSWEET, LAND, 0.05f, Vector2(1.0f, 10.0f), 3.0f, ANY, 0.0f, 0.5f, 0.8f, 0.3f, 0.5f, 0.85f, 1.2f, 6.0f, 0.5f, 50.0f } });
			break;
		case BEAVER_MEADOW:
			// The drained bed: rush and small sedge seed in on the mud at once, then sedge and tall grass close into a
			// dense meadow with meadowsweet, marsh marigold in the wet hollows and buttercup on the drier ground.
			set = LocalVector<BeaverSpec>({ { P::PRESET_RUSH, LAND, 0.05f, ANY, 0.0f, Vector2(0.0f, 4.0f), 2.0f, 0.4f, 0.6f, 0.3f, 0.75f, 0.7f, 1.1f, 5.0f, 0.55f, 40.0f },
					{ P::PRESET_SEDGE, LAND, 0.05f, ANY, 0.0f, Vector2(0.0f, 4.0f), 2.0f, 0.4f, 0.45f, 0.2f, 0.6f, 0.6f, 0.9f, 4.0f, 0.5f, 35.0f },
					{ P::PRESET_SEDGE, LAND, 0.05f, ANY, 0.0f, Vector2(3.0f, 38.0f), 3.0f, 0.4f, 0.35f, 0.2f, 0.95f, 1.0f, 1.4f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_COCKSFOOT, LAND, 0.05f, ANY, 0.0f, Vector2(3.0f, 38.0f), 3.0f, 0.4f, 0.4f, 0.2f, 0.7f, 1.3f, 1.7f, 7.0f, 0.6f, 45.0f },
					{ P::PRESET_MEADOWSWEET, LAND, 0.05f, ANY, 0.0f, Vector2(4.0f, 38.0f), 3.0f, 0.4f, 0.8f, 0.3f, 0.45f, 0.9f, 1.25f, 8.0f, 0.45f, 50.0f },
					{ P::PRESET_MARSH_MARIGOLD, LAND, 0.05f, Vector2(0.0f, 8.0f), 3.0f, Vector2(2.0f, 28.0f), 3.0f, 0.4f, 0.6f, 0.2f, 0.4f, 0.8f, 1.2f, 4.0f, 0.4f, 30.0f },
					{ P::PRESET_BUTTERCUP, LAND, 0.05f, ANY, 0.0f, Vector2(3.0f, 38.0f), 3.0f, 0.4f, 0.5f, 0.15f, 0.35f, 0.85f, 1.2f, 6.0f, 0.4f, 30.0f } });
			break;
		case BEAVER_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9100 + int(p_stand) * 100;
	for (const BeaverSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(seed_base);
		gen->set_season(p_season);
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
		if (f.preset == P::PRESET_WATER_LILY) {
			sp->set_floating(true);
		}
		sp->set_rule_range(PlantSpecies::RULE_WATER_DEPTH, f.depth);
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DEPTH, f.depth_fade);
		if (f.water.x <= f.water.y) {
			sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, f.water);
			sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, f.water_fade);
		}
		if (f.ages.x <= f.ages.y) {
			sp->set_rule_range(PlantSpecies::RULE_STAND_AGE, f.ages);
			sp->set_rule_fade(PlantSpecies::RULE_STAND_AGE, f.age_fade);
			sp->set_rule_range(PlantSpecies::RULE_DISTURBANCE, Vector2(DISTURBANCE_BEAVER_KIND, DISTURBANCE_BEAVER_KIND));
			sp->set_rule_fade(PlantSpecies::RULE_DISTURBANCE, 0.0f);
		}
		if (f.shade_max < 1.0f) {
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, f.shade_max));
			sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.15f);
		}
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 25.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("beaver", p_stand == BEAVER_POND ? "pond" : "meadow");
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

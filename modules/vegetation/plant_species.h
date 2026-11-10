/**************************************************************************/
/*  plant_species.h                                                       */
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
#include "core/variant/typed_array.h"
#include "scene/resources/mesh.h"

class TerrainDataMap3D;

// One kind of plant and where it grows (docs/ideas/vegetation-placement.md). Rules are ranges
// over the TerrainDataMap3D maps with a soft edge; a plant is kept with probability
// density times every rule's weight.
class PlantSpecies : public Resource {
	GDCLASS(PlantSpecies, Resource);

public:
	enum Rule {
		RULE_SLOPE, // Degrees.
		RULE_HEIGHT, // Metres.
		RULE_CURVATURE, // 1/m, positive in hollows.
		RULE_WETNESS, // Topographic wetness index.
		RULE_WATER_DEPTH, // Metres, 0 on land.
		RULE_WATER_DISTANCE, // Metres, negative under water.
		RULE_MAX,
	};

private:
	TypedArray<Mesh> meshes;
	float spacing = 2.0f;
	float footprint = 0.5f;
	float density = 1.0f;
	Vector2 scale_range = Vector2(0.8f, 1.2f);
	float align_to_ground = 0.0f;
	bool random_yaw = true;
	// Patches: clover beds, flower drifts. Value noise over the ground keeps the species to
	// patches about patch_scale metres across, covering patch_cover of the ground.
	float patch_scale = 0.0f; // 0: no patches.
	float patch_cover = 0.5f;
	int patch_seed = 0;
	bool patch_invert = false; // Grow between the patches instead (same scale, cover and seed: the exact complement).
	float visibility_range = 0.0f; // 0: always drawn.
	Vector2 ranges[RULE_MAX];
	float fades[RULE_MAX] = {};

protected:
	static void _bind_methods();

public:
	void set_meshes(const TypedArray<Mesh> &p_meshes);
	TypedArray<Mesh> get_meshes() const { return meshes; }
	void set_patch_scale(float p_scale) { patch_scale = MAX(p_scale, 0.0f); }
	float get_patch_scale() const { return patch_scale; }
	void set_patch_cover(float p_cover) { patch_cover = CLAMP(p_cover, 0.0f, 1.0f); }
	float get_patch_cover() const { return patch_cover; }
	void set_patch_seed(int p_seed) { patch_seed = p_seed; }
	int get_patch_seed() const { return patch_seed; }
	void set_patch_invert(bool p_invert) { patch_invert = p_invert; }
	bool is_patch_invert() const { return patch_invert; }
	float get_patch_weight(const Vector3 &p_pos) const;
	void set_visibility_range(float p_range) { visibility_range = MAX(p_range, 0.0f); }
	float get_visibility_range() const { return visibility_range; }
	void set_spacing(float p_spacing);
	float get_spacing() const { return spacing; }
	void set_footprint(float p_footprint);
	float get_footprint() const { return footprint; }
	void set_density(float p_density);
	float get_density() const { return density; }
	void set_scale_range(const Vector2 &p_range);
	Vector2 get_scale_range() const { return scale_range; }
	void set_align_to_ground(float p_align);
	float get_align_to_ground() const { return align_to_ground; }
	void set_random_yaw(bool p_enable);
	bool is_random_yaw() const { return random_yaw; }

	void set_rule_range(Rule p_rule, const Vector2 &p_range);
	Vector2 get_rule_range(Rule p_rule) const;
	void set_rule_fade(Rule p_rule, float p_fade);
	float get_rule_fade(Rule p_rule) const;

	// Weight of one value against one rule: 1 inside the range, smoothstep to 0 over the fade.
	float rule_weight(Rule p_rule, float p_value) const;
	// Product of every rule's weight at a point of the map.
	float get_suitability(const TerrainDataMap3D *p_map, const Vector3 &p_global_position) const;
	float get_suitability_at(Object *p_map, const Vector3 &p_global_position) const;

	PlantSpecies();
};

VARIANT_ENUM_CAST(PlantSpecies::Rule);

// Plant species in priority order: earlier species are placed first and later ones keep clear
// of their footprints (trees, then bushes, then ground cover).
class Biome : public Resource {
	GDCLASS(Biome, Resource);

	TypedArray<PlantSpecies> species;

protected:
	static void _bind_methods();

public:
	void set_species(const TypedArray<PlantSpecies> &p_species);
	TypedArray<PlantSpecies> get_species() const { return species; }
};

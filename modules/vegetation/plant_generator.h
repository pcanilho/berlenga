/**************************************************************************/
/*  plant_generator.h                                                     */
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
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

// Small plants for ground cover (docs/ideas/plant-generator.md), after Prusinkiewicz et al.,
// "The use of positional information in the modeling of plants" (2001): organs placed along
// axes by phyllotaxis, their size, angle and bend functions of their position; leaves, petals
// and stems are generalized cylinders swept along a turtle path with a parallel-transport frame.
class PlantGenerator : public Resource {
	GDCLASS(PlantGenerator, Resource);

public:
	static void free_shaders();
	enum Preset {
		PRESET_DANDELION,
		PRESET_PLANTAIN,
		PRESET_CLOVER,
		PRESET_FERN,
		PRESET_SEDGE,
		PRESET_DAISY,
		PRESET_BUTTERCUP,
		PRESET_NETTLE,
		PRESET_THISTLE,
		PRESET_YARROW,
		PRESET_DOCK,
		PRESET_COCKSFOOT,
		PRESET_TIMOTHY,
		PRESET_DRY_GRASS,
		PRESET_LEAF_LITTER,
		PRESET_MOSS,
		PRESET_BILBERRY,
		PRESET_BRACKEN,
		PRESET_BRAMBLE,
		PRESET_FOXGLOVE,
		PRESET_MAX,
	};

	enum LeafShape {
		LEAF_LANCEOLATE, // Narrow, widest below the middle.
		LEAF_OVATE, // Egg-shaped.
		LEAF_ROUND, // Clover leaflet, buttercup petal.
		LEAF_LINEAR, // Grass and sedge blades.
		LEAF_SPATULATE, // Widest near the tip (daisy basal leaves, petals).
		LEAF_PALMATE, // Round with deep lobes (buttercup).
	};

	struct Leaf {
		LeafShape shape = LEAF_LANCEOLATE;
		float length = 0.1f;
		float width = 0.03f;
		float petiole = 0.0f; // Fraction of the length that is stalk.
		float fold = 0.2f; // Rise of the edges against the midrib, as a fraction of the width.
		float bend = 0.5f; // Downward curl along the length, radians over the whole leaf.
		float twist = 0.0f; // Radians over the whole leaf.
		int lobes = 0; // Lobes on each side (dandelion teeth, buttercup lobes).
		float lobe_depth = 0.0f;
		int segments = 10;
	};

	enum Venation {
		VENATION_PINNATE, // A midrib with secondaries (dandelion, daisy, fern pinnae).
		VENATION_PALMATE, // Several primaries from the base (buttercup).
		VENATION_PARALLEL, // Veins along the length (plantain, sedge, petals).
	};

	// What the leaf texture is baked from: the main leaf's outline and venation, its colours.
	struct LeafTexture {
		Leaf leaf;
		float aspect = 0.25f; // Width over length of that leaf.
		Venation venation = VENATION_PINNATE;
		int parallel_veins = 5;
		Color lamina = Color(0.12f, 0.3f, 0.07f);
		Color vein = Color(0.3f, 0.42f, 0.15f);
		float yellowing = 0.3f; // Dry, yellowed margins.
		float spots = 0.3f; // Small blemishes.
		float chevron = 0.0f; // Clover's pale V mark.
		Color base_tint = Color(0, 0, 0, 0); // Petals: a different colour towards the base (alpha: amount).
	};
	struct TextureSet {
		Ref<ImageTexture> albedo;
		Ref<ImageTexture> normal;
		Ref<ImageTexture> backlight;
	};

private:
	struct Builder {
		PackedVector3Array verts;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedVector2Array uv2s; // x: wind weight (0 at the root, 1 at the tips), y: the organ's base, packed (generate()).
		PackedInt32Array organs; // Per vertex: the basal organ it belongs to (0: the plant as a whole).
		PackedColorArray colors; // Ambient occlusion guess, multiplies the albedo.
		PackedInt32Array indices;
	};
	enum Surface {
		SURFACE_LEAF,
		SURFACE_STEM,
		SURFACE_PETAL,
		SURFACE_CENTRE,
		SURFACE_MAX,
	};

	Preset preset = PRESET_DANDELION;
	int seed = 0;
	float size = 1.0f;
	bool lods = true;
	Color leaf_color = Color(0.12f, 0.3f, 0.07f);
	Color petal_color = Color(1.0f, 0.78f, 0.05f);
	Color centre_color = Color(0.95f, 0.65f, 0.05f);
	Color stem_color = Color(0.2f, 0.36f, 0.1f);

	Builder builders[SURFACE_MAX];
	RandomPCG rng;
	float plant_height = 0.0f;
	// Basal organs (a frond, a cane, a tiller, a rosette leaf): everything added after an organ that
	// starts on the ground belongs to it, until the next one. The shader bends each organ about its
	// own base (bracken fronds and bramble canes rise from a rhizome or crown, not the plant's origin).
	int organ = 0;
	LocalVector<Vector3> organ_bases;
	void _begin_organ(const Vector3 &p_origin);
	void _tag_organ(Builder &p_builder) const;

	float _rand(float p_from, float p_to) { return Math::lerp(p_from, p_to, rng.randf()); }
	static float _leaf_width(const Leaf &p_leaf, float p_t);
	void _add_leaf(Surface p_surface, const Transform3D &p_base, const Leaf &p_leaf, float p_wind_base, float p_ao = 1.0f, const Color &p_tint = Color(1, 1, 1));
	LocalVector<Transform3D> _stem_frames(const Transform3D &p_base, float p_length, float p_bend, int p_segments) const;
	Transform3D _frame_at(const LocalVector<Transform3D> &p_frames, float p_s) const;
	void _add_grass_tuft(int p_blades, float p_length_min, float p_length_max, float p_width_min, float p_width_max, float p_spread);
	Transform3D _add_stem(const Transform3D &p_base, float p_length, float p_r0, float p_r1, float p_bend, int p_segments, float p_wind_base, Surface p_surface = SURFACE_STEM);
	void _add_head(Surface p_surface, const Transform3D &p_base, float p_radius, float p_height);
	void _add_ray_flower(const Transform3D &p_top, float p_radius, int p_rays, const Leaf &p_petal);
	void _add_vogel_head(const Transform3D &p_top, float p_radius, int p_florets, const Leaf &p_floret);
	void _add_cup_flower(const Transform3D &p_top, float p_radius);

	void _build_dandelion();
	void _build_plantain();
	void _build_clover();
	void _build_fern();
	void _build_sedge();
	void _build_daisy();
	void _build_buttercup();
	void _build_nettle();
	void _build_thistle();
	void _build_yarrow();
	void _build_dock();
	void _build_cocksfoot();
	void _build_timothy();
	void _build_dry_grass();
	void _build_leaf_litter();
	void _build_moss();
	void _build_bilberry();
	void _build_bracken();
	void _build_bramble();
	void _build_foxglove();

	static Ref<Shader> shaders[2]; // Plain and textured, shared by every generated plant.
	static Ref<Shader> _plant_shader(bool p_textured);
	Ref<ShaderMaterial> _material(const Color &p_color, float p_roughness, bool p_backlight) const;
	Ref<ShaderMaterial> _textured_material(const TextureSet &p_set, float p_roughness) const;
	bool textures = true;
	int texture_size = 256;
	float leaf_wear = 0.5f;
	uint32_t leaf_serial = 0; // Counts the leaves of a plant: their wear draws from it, not from rng.
	HashMap<uint64_t, TextureSet> texture_cache;
	LeafTexture _leaf_texture_spec(bool p_petal) const;
	TextureSet _bake_leaf_texture(const LeafTexture &p_spec, uint32_t p_seed) const;
	TextureSet _bake_moss_texture(uint32_t p_seed) const;

protected:
	static void _bind_methods();

public:
	void set_preset(Preset p_preset) { preset = p_preset; }
	Preset get_preset() const { return preset; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }
	void set_size(float p_size) { size = MAX(p_size, 0.01f); }
	float get_size() const { return size; }
	void set_leaf_color(const Color &p_color) { leaf_color = p_color; }
	Color get_leaf_color() const { return leaf_color; }
	void set_petal_color(const Color &p_color) { petal_color = p_color; }
	Color get_petal_color() const { return petal_color; }
	void set_centre_color(const Color &p_color) { centre_color = p_color; }
	Color get_centre_color() const { return centre_color; }
	void set_stem_color(const Color &p_color) { stem_color = p_color; }
	Color get_stem_color() const { return stem_color; }

	void set_lods(bool p_lods) { lods = p_lods; }
	bool get_lods() const { return lods; }
	void set_textures(bool p_textures) { textures = p_textures; }
	bool get_textures() const { return textures; }
	void set_texture_size(int p_size) { texture_size = CLAMP(p_size, 32, 2048); }
	void set_leaf_wear(float p_wear) { leaf_wear = CLAMP(p_wear, 0.0f, 1.0f); }
	float get_leaf_wear() const { return leaf_wear; }
	int get_texture_size() const { return texture_size; }
	Ref<ArrayMesh> generate();
	TypedArray<ArrayMesh> generate_variants(int p_count);
	void apply_preset_colors();
};

VARIANT_ENUM_CAST(PlantGenerator::Preset);

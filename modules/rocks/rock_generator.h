/**************************************************************************/
/*  rock_generator.h                                                      */
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

#pragma once

#include "core/object/ref_counted.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class ImporterMesh;

// Rocks and stones as real geometry (docs/ideas/rocks.md). Each rock is a block broken along the
// joints of its rock type (Plato's cube: natural fragments average about six faces, Domokos et al.
// 2020), chipped, then rounded by transport as Bloore's (1977) abrasion flow, the surface moving in at
// 1 + c kappa, on a signed distance field: corners go first, the rock shrinks later (Domokos et al.
// 2014). Weathering by rock type (spheroidal granite, karst pits, sandstone laminae, basalt vesicles,
// slate cleavage, glacial striations) is carved into the same field, which is meshed by surface nets
// with distance LODs. A generated mesh rests on its flattest side: local Y is its shortest axis, X its
// longest, and its b axis (the intermediate one, sieve size) is 2 units long, so an instance scaled
// by s is a stone of b-axis diameter 2 s.
class RockGenerator : public RefCounted {
	GDCLASS(RockGenerator, RefCounted);

public:
	enum RockType {
		ROCK_GRANITE,
		ROCK_LIMESTONE,
		ROCK_SANDSTONE,
		ROCK_BASALT,
		ROCK_SLATE,
		ROCK_MAX,
	};

	enum Family {
		FAMILY_ROCKFALL, // fresh blocks off a cliff
		FAMILY_SCREE, // smaller fresh stones on talus, chipped on the way down
		FAMILY_GLACIAL, // subrounded boulders out of till, a striated facet
		FAMILY_COBBLE, // river stones rounded by transport_distance
		FAMILY_PEBBLE, // beach stones, rounded and flattened by the swash
		FAMILY_CLIFF_BLOCK, // large jointed blocks set into cliff faces
		FAMILY_CORESTONE, // tor blocks: joint blocks rounded in place by deep weathering, flat sheeting faces
		FAMILY_SEA_STACK, // a sea stack: one jointed column (x up when stood on end), bedded, notched at the waterline
		FAMILY_SEA_ARCH, // a sea arch: a bedded wall with the opening the waves cut through it (x the span, y up)
		FAMILY_SEA_PLATFORM, // a wave-cut platform: a flat bench of bedrock (x along the shore), jointed, with pools
		FAMILY_HOODOO, // a hoodoo: a beaded column of soft bedded rock (x up when stood on end) under a wider caprock
		FAMILY_PAVEMENT, // limestone pavement: a flat slab of bedrock cut into clints by deep grykes, with pans
		FAMILY_FIN, // sandstone fin: a narrow wall of bedrock between two joints, its top rounded, some holed by an arch
		FAMILY_POTHOLE, // a slab of river bedrock drilled with potholes in a line down the flow, polished and fluted
		FAMILY_GOBLIN, // a sandstone knob (Goblin Valley): a bulbous head on a narrow neck, rounded all over
		FAMILY_CAVE_MOUTH, // a limestone outcrop at a cliff foot with a passage opening into it (x along, z up, y out)
		FAMILY_LEDGE, // a ledge slab: the caprock bed standing out over the lip of a bench (x along the lip, y up, z out)
		FAMILY_CLIFF_PILLAR, // a cliff face's joint-bounded pillar: a column between vertical joints (x up when stood on end, y across, z into the wall)
		FAMILY_MAX,
	};

	struct Lithology {
		const char *name;
		// Fracture: block proportions (b = 1) and how jointed the rock is.
		float a_ratio; // long axis over b
		float c_ratio; // short axis over b
		float joint_jitter; // degrees joints wander off orthogonal
		bool columnar; // basalt columns: 5 to 7-sided prisms
		float extra_cuts; // mean secondary fracture planes
		// Abrasion: distance (m) of river transport that takes it about two thirds of the way to round.
		float abrasion_length;
		// Weathering, all 0..1 strengths.
		float spheroidal;
		float pits; // solution pits (karst) or vesicles
		float laminae; // bedding grooves
		float cleavage; // stepped plates
		float grain; // surface roughness at the grain scale
		// Colour: two mineral colours mixed in specks, a band colour, the weathered rind.
		Color base;
		Color speck;
		float speck_share;
		float speck_size; // m
		Color band;
		float band_contrast;
		Color rind;
		float roughness;
		float porosity;
	};

	static const Lithology &get_lithology(RockType p_type);
	static String get_rock_type_name(RockType p_type);
	static String get_family_name(Family p_family);

private:
	RockType rock_type = ROCK_GRANITE;
	Family family = FAMILY_ROCKFALL;
	int seed = 0;
	int resolution = 0; // 0: the family's own
	real_t transport_distance = -1.0; // m; < 0: the family's own
	real_t weathering = -1.0; // 0..1; < 0: the family's own
	real_t salt = -1.0; // sea-spray salt 0..1 (tafoni, honeycomb); < 0: the family's own
	real_t detail = 1.0;
	bool lods = true;
	bool simplify = true;
	bool close_up = false;
	int fracture_pieces = 0;
	real_t marks_scale = 0.0; // m per unit the close-up marks are carved at; 0: the family's

	double last_build_msec = 0.0;
	int last_triangles = 0;
	int last_faces = 0;

	static HashMap<int, Ref<ShaderMaterial>> material_cache;
	static Ref<Shader> shared_shader;

protected:
	static void _bind_methods();

public:
	void set_rock_type(RockType p_type);
	RockType get_rock_type() const { return rock_type; }
	void set_family(Family p_family);
	Family get_family() const { return family; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }
	void set_resolution(int p_resolution) { resolution = p_resolution <= 0 ? 0 : CLAMP(p_resolution, 16, 160); }
	int get_resolution() const { return resolution; }
	void set_transport_distance(real_t p_distance) { transport_distance = p_distance; }
	real_t get_transport_distance() const { return transport_distance; }
	void set_weathering(real_t p_weathering) { weathering = p_weathering < 0.0 ? -1.0 : MIN(p_weathering, real_t(1.0)); }
	real_t get_weathering() const { return weathering; }
	void set_salt(real_t p_salt) { salt = p_salt < 0.0 ? -1.0 : MIN(p_salt, real_t(1.0)); }
	real_t get_salt() const { return salt; }
	void set_detail(real_t p_detail) { detail = CLAMP(p_detail, real_t(0.0), real_t(2.0)); }
	real_t get_detail() const { return detail; }
	void set_lods(bool p_lods) { lods = p_lods; }
	bool get_lods() const { return lods; }
	void set_simplify(bool p_simplify) { simplify = p_simplify; }
	bool get_simplify() const { return simplify; }
	void set_close_up(bool p_close_up) { close_up = p_close_up; }
	bool get_close_up() const { return close_up; }
	void set_fracture_pieces(int p_pieces) { fracture_pieces = CLAMP(p_pieces, 0, 64); }
	int get_fracture_pieces() const { return fracture_pieces; }
	void set_marks_scale(real_t p_scale) { marks_scale = MAX(p_scale, real_t(0.0)); }
	real_t get_marks_scale() const { return marks_scale; }
	real_t get_marks_metres_per_unit() const;
	// Metres per mesh unit of a typical rock of a family (half its b axis), for the millimetre relief.
	static real_t get_family_metres_per_unit(Family p_family);

	int get_base_resolution() const;
	int get_effective_resolution() const;
	real_t get_effective_transport_distance() const;
	real_t get_effective_weathering() const;
	real_t get_effective_salt() const;
	// Roundness reached by the transport (0 fresh .. 1 fully rounded), from the rock type's abrasion length.
	real_t get_roundness() const;

	// One rock for the current seed. Its metadata: "rock_axes" (half lengths a, c, b along x, y, z),
	// "rock_bottom" (lowest y), "rock_hull" (up to 48 points for a convex collision shape), "rock_faces"
	// (planes it broke along), "rock_roundness".
	Ref<ArrayMesh> generate();
	// The same rock before it reaches the renderer (LODs made, no ArrayMesh yet), with the metadata in r_meta: safe
	// on a worker thread once get_material() has made this rock type's material.
	Ref<ImporterMesh> build_importer_mesh(Dictionary &r_meta);
	// count rocks, seeds seed .. seed + count - 1.
	Array generate_library(int p_count);

	double get_last_build_msec() const { return last_build_msec; }
	int get_last_triangle_count() const { return last_triangles; }

	// The shared rock shader (object-space mineral colour, weathered rind, lichen, moss, wetness and snow from the
	// engine's weather lines, contact with the ground) and one material per rock type.
	static String get_shader_code();
	static Ref<ShaderMaterial> get_material(RockType p_type);
	// Frees the shared shader and materials (module uninitialisation).
	static void finish();
};

VARIANT_ENUM_CAST(RockGenerator::RockType);
VARIANT_ENUM_CAST(RockGenerator::Family);

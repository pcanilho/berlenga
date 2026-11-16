/**************************************************************************/
/*  cave_network_3d.h                                                       */
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

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class TerrainGenerator3D;
class RockField3D;

// Cave networks under a heightfield (docs/ideas/rocks.md, "Real caves behind the mouths"): Paris, Peytavie, Guérin,
// Collon, Galin, "Synthesizing Geologically Coherent Cave Networks" (Computer Graphics Forum 40(7), 2021). Each cave
// is grown from an entrance at a cliff foot into the rock behind it: points sampled in the bedrock (denser on the
// bedding planes), a nearest-neighbour graph, least-cost paths under an anisotropic cost (bedding planes and joints
// cheap), a gamma-skeleton between the key points (the entrance, sinks under closed hollows above), side passages,
// tortuosity, and a passage type from the height over the water table.
class CaveNetwork3D : public Node3D {
	GDCLASS(CaveNetwork3D, Node3D);

public:
	enum PassageType {
		PASSAGE_ENTRANCE, // the first metres from the mouth in, a low arch
		PASSAGE_TUBE, // phreatic: round to elliptical, below or at the water table
		PASSAGE_BED, // phreatic along a bedding plane: wide and low
		PASSAGE_KEYHOLE, // a tube with a vadose trench cut in its floor, just above the water table
		PASSAGE_CANYON, // vadose: tall and narrow, well above the water table
		PASSAGE_SHAFT, // vertical
		PASSAGE_MAX,
	};

	struct Segment {
		Vector3 a, b;
		float ra = 1.0f, rb = 1.0f; // half widths at a and b, metres
		PassageType type = PASSAGE_TUBE;
	};
	struct Chamber {
		Vector3 center;
		Vector3 radii; // x, z across, y up
		float yaw = 0.0f;
	};
	struct Cave {
		Vector3 entrance, out;
		LocalVector<Segment> segments;
		LocalVector<Chamber> chambers;
		LocalVector<Vector3> keys;
		AABB bounds;
	};

private:
	// Meshes: the rock round the passages as one distance field (docs/ideas/rocks.md, "Real caves"), polygonised
	// per chunk by surface nets (Gibson 1998). Round each entrance a patch also carries the ground itself, so the
	// cliff, the opening and the passage are one surface; the landscape gets a hole there.
	struct Chunk {
		Vector3i key;
		Ref<ArrayMesh> mesh;
		Node3D *node = nullptr;
		int triangles = 0;
	};
	struct FieldSource {
		LocalVector<const Segment *> segments;
		LocalVector<const Chamber *> chambers;
		LocalVector<Vector3> patches; // entrances whose patch reaches this chunk
		LocalVector<Vector3> patch_out;
	};
	friend struct CaveMeshJob;
	HashMap<Vector3i, Chunk> chunks;
	real_t mesh_cell = 0.3;
	real_t chunk_size = 16.0;
	real_t patch_radius = 14.0;
	real_t roughness = 1.0;
	bool collision = true;
	Ref<Material> wall_material;
	Ref<Material> ground_material;
	Ref<ShaderMaterial> default_wall;
	uint64_t last_mesh_usec = 0;
	const TerrainGenerator3D *mesh_gen = nullptr; // resolved on the main thread for the mesh workers

	NodePath generator_path;
	NodePath rock_field_path;
	int seed = 0;
	real_t cave_length = 250.0; // m, how far behind the entrance the network may reach
	real_t sample_spacing = 7.0; // m, between bedrock samples (the bedding planes get 0.6 of it)
	real_t bedding_spacing = 12.0; // m, between the inception horizons
	real_t cover = 4.0; // m, the least rock over a passage
	real_t gamma = 2.0; // the gamma-skeleton: 2 branchwork (Gabriel graph), under 1 mazes
	real_t tortuosity = 0.4; // 0 straight .. 1 up to 4 m of wander
	int side_passages = 6;
	real_t chamber_chance = 0.35;
	LocalVector<Cave> caves;
	uint64_t last_build_usec = 0;
	int last_samples = 0;

	TerrainGenerator3D *_get_generator() const;
	bool _grow(Cave &r_cave, uint32_t p_seed) const;
	real_t _air(const FieldSource &p_src, const Vector3 &p_p) const;
	real_t _rock(const FieldSource &p_src, const TerrainGenerator3D *p_gen, const Vector3 &p_p, bool *r_ground = nullptr) const;
	void _gather(const AABB &p_box, FieldSource &r_src) const;
	void _mesh_chunk(const Vector3i &p_key, Chunk &r_chunk) const;
	static void _mesh_job(void *p_data, uint32_t p_index);
	Ref<Material> _wall_material();

protected:
	static void _bind_methods();

public:
	int add_cave(const Vector3 &p_entrance, const Vector3 &p_out);
	void clear_caves();
	int get_cave_count() const { return int(caves.size()); }
	const Cave *get_cave(int p_index) const { return p_index >= 0 && p_index < int(caves.size()) ? &caves[p_index] : nullptr; }
	Array get_segments(int p_cave) const;
	Array get_chambers(int p_cave) const;
	Array get_key_points(int p_cave) const;
	Array find_entrances(const Rect2 &p_rect, int p_max = 8) const;
	real_t get_last_build_msec() const { return real_t(last_build_usec) / 1000.0; }
	int build_meshes(const AABB &p_area);
	void clear_meshes();
	int get_mesh_chunk_count() const { return int(chunks.size()); }
	int get_mesh_triangle_count() const;
	real_t get_last_mesh_msec() const { return real_t(last_mesh_usec) / 1000.0; }
	real_t get_rock_distance(const Vector3 &p_position) const;
	void set_mesh_cell(real_t p_value) { mesh_cell = CLAMP(p_value, real_t(0.05), real_t(4.0)); }
	real_t get_mesh_cell() const { return mesh_cell; }
	void set_patch_radius(real_t p_value) { patch_radius = CLAMP(p_value, real_t(0.0), real_t(100.0)); }
	real_t get_patch_radius() const { return patch_radius; }
	void set_roughness(real_t p_value) { roughness = CLAMP(p_value, real_t(0.0), real_t(4.0)); }
	real_t get_roughness() const { return roughness; }
	void set_collision(bool p_enable) { collision = p_enable; }
	bool has_collision() const { return collision; }
	void set_wall_material(const Ref<Material> &p_material) { wall_material = p_material; }
	Ref<Material> get_wall_material() const { return wall_material; }
	void set_ground_material(const Ref<Material> &p_material) { ground_material = p_material; }
	Ref<Material> get_ground_material() const { return ground_material; }
	int get_last_sample_count() const { return last_samples; }

	void set_generator(const NodePath &p_path) { generator_path = p_path; }
	NodePath get_generator() const { return generator_path; }
	void set_rock_field(const NodePath &p_path) { rock_field_path = p_path; }
	NodePath get_rock_field() const { return rock_field_path; }
	void set_seed(int p_value) { seed = p_value; }
	int get_seed() const { return seed; }
	void set_cave_length(real_t p_value) { cave_length = CLAMP(p_value, real_t(20.0), real_t(2000.0)); }
	real_t get_cave_length() const { return cave_length; }
	void set_sample_spacing(real_t p_value) { sample_spacing = CLAMP(p_value, real_t(2.0), real_t(50.0)); }
	real_t get_sample_spacing() const { return sample_spacing; }
	void set_bedding_spacing(real_t p_value) { bedding_spacing = CLAMP(p_value, real_t(2.0), real_t(200.0)); }
	real_t get_bedding_spacing() const { return bedding_spacing; }
	void set_cover(real_t p_value) { cover = CLAMP(p_value, real_t(1.0), real_t(100.0)); }
	real_t get_cover() const { return cover; }
	void set_gamma(real_t p_value) { gamma = CLAMP(p_value, real_t(0.5), real_t(10.0)); }
	real_t get_gamma() const { return gamma; }
	void set_tortuosity(real_t p_value) { tortuosity = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_tortuosity() const { return tortuosity; }
	void set_side_passages(int p_value) { side_passages = CLAMP(p_value, 0, 64); }
	int get_side_passages() const { return side_passages; }
	void set_chamber_chance(real_t p_value) { chamber_chance = CLAMP(p_value, real_t(0.0), real_t(1.0)); }
	real_t get_chamber_chance() const { return chamber_chance; }
};

VARIANT_ENUM_CAST(CaveNetwork3D::PassageType);

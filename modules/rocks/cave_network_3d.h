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
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class TerrainGenerator3D;
class RockField3D;
struct CaveMeshJob;

// Cave networks under a heightfield (docs/ideas/rocks.md, "Real caves behind the mouths"): Paris, Peytavie, Guérin,
// Collon, Galin, "Synthesizing Geologically Coherent Cave Networks" (Computer Graphics Forum 40(7), 2021). Each cave
// is grown from an entrance at a cliff foot into the rock behind it: points sampled in the bedrock (denser on the
// bedding planes), a nearest-neighbour graph, least-cost paths under an anisotropic cost (bedding planes and joints
// cheap), a gamma-skeleton between the key points (the entrance, sinks under closed hollows above), side passages,
// tortuosity, and a passage type from the height over the water table.

class CaveNetwork3D : public Node3D, public WorldWaterProvider {
	GDCLASS(CaveNetwork3D, Node3D);

public:
	enum PassageType {
		PASSAGE_ENTRANCE, // the first metres from the mouth in, a low arch
		PASSAGE_TUBE, // phreatic: round to elliptical, below or at the water table
		PASSAGE_BED, // phreatic along a bedding plane: wide and low
		PASSAGE_KEYHOLE, // a tube with a vadose trench cut in its floor, just above the water table
		PASSAGE_CANYON, // vadose: tall and narrow, well above the water table
		PASSAGE_SHAFT, // vertical
		PASSAGE_SEA, // a sea cave: a tall arch quarried by waves along a joint, floored below low water
		PASSAGE_MAX,
	};

	struct Segment {
		Vector3 a, b;
		float ra = 1.0f, rb = 1.0f; // half widths at a and b, metres
		PassageType type = PASSAGE_TUBE;
		bool stream = false; // a vadose stream runs along its trench floor, in a channel cut into the fill
	};
	struct Chamber {
		Vector3 center;
		Vector3 radii; // x, z across, y up
		float yaw = 0.0f;
	};
	// A fallen block on a floor: breakdown in chambers, wave-tumbled boulders in sea caves.
	struct Block {
		Transform3D xform; // the box's half extents in its basis
		Transform3D inv;
		real_t round = 0.1;
	};
	struct Cave {
		Vector3 entrance, out;
		LocalVector<Block> blocks;
		LocalVector<Segment> segments;
		LocalVector<Chamber> chambers;
		LocalVector<Vector3> keys;
		AABB bounds;
		bool holed = false; // the landscape has its hole over this cave's patch
		bool sea = false;
		real_t mouth = 0.0; // a sea cave's mouth radius (its patch is sized to it)
		LocalVector<Vector3> blowholes; // where a sea cave's roof shaft opens at the ground
	};

private:
	// Meshes: the rock round the passages as one distance field (docs/ideas/rocks.md, "Real caves"), polygonised
	// per chunk by surface nets (Gibson 1998). Round each entrance a patch also carries the ground itself, so the
	// cliff, the opening and the passage are one surface; the landscape gets a hole there.
	// A rimstone pool: the still water behind a dam, at its level, inside its outline (xz round center).
	struct RimPool {
		Vector3 center; // y: the water level
		real_t depth = 0.05;
		PackedVector2Array outline;
	};
	struct Chunk {
		Vector3i key;
		Ref<ArrayMesh> mesh;
		Node3D *node = nullptr;
		int triangles = 0;
		real_t cell = 0.0; // its grid: mesh_cell, or near_mesh_cell round the focus
		bool pinned = false; // an entrance patch chunk: kept while the landscape's hole is open
		Vector<Transform3D> spel[16]; // speleothems by kind * SPEL_VARIANTS + variant (unit shapes placed)
		Ref<ArrayMesh> rim_mesh; // rimstone dams (world space)
		LocalVector<RimPool> pools;
		LocalVector<ObjectID> pool_water; // the live water of each pool (none when far)
	};
	enum {
		SPEL_STALACTITE,
		SPEL_STALAGMITE,
		SPEL_COLUMN,
		SPEL_CURTAIN,
		SPEL_KINDS,
		SPEL_VARIANTS = 4,
	};
	// A drip point: where seepage through the roof drips into air, and what grows there.
	struct Drip {
		Vector3 roof, foot; // the roof point and the floor under it
		Vector3 along; // a curtain's run (down the slope of an inclined roof), horizontal
		real_t gap = 0.0;
		real_t len = 0.0, rad = 0.0; // the hanging form
		real_t mite = 0.0, mrad = 0.0; // the standing form (0: none)
		real_t yaw = 0.0;
		int kind = SPEL_STALACTITE;
		int variant = 0;
	};
	struct FieldSource {
		LocalVector<const Segment *> segments;
		LocalVector<const Chamber *> chambers;
		LocalVector<const Block *> blocks;
		LocalVector<Vector3> patches; // entrances whose patch reaches this chunk
		LocalVector<Vector3> patch_out;
		LocalVector<real_t> patch_r; // each patch's half size (_patch_half)
	};
	friend struct CaveMeshJob;
	HashMap<Vector3i, Chunk> chunks;
	real_t mesh_cell = 0.3;
	bool cave_ambient_probes = true; // interior probes deep in the rock: no sky ambient there in raster
	// Near the follow target (or the point refine_meshes is given), chunks are meshed on a finer grid, so the walls
	// round the camera hold real relief down to a few centimetres; a refined chunk overlaps its coarse neighbours by
	// one coarse cell, so the seam between them has no crack.
	real_t near_mesh_cell = 0.1;
	real_t near_mesh_radius = 10.0;
	real_t _cell_for(const Vector3i &p_key, const Vector3 &p_focus, real_t p_slack = 0.0) const;
	void _replace_chunk(Chunk &p_chunk, const Ref<Material> &p_wall);
	real_t chunk_size = 16.0;
	real_t patch_radius = 14.0;
	// A cave's patch half size: patch_radius, a sea cave's no wider than its mouth needs (the beach's own sand surface
	// is cut away over the whole patch, and the bare ground there showed its heightfield's facets).
	real_t _patch_half(const Cave &p_cave) const { return p_cave.sea && p_cave.mouth > 0.0 ? MIN(patch_radius, p_cave.mouth * 1.3 + 3.0) : patch_radius; }
	real_t roughness = 1.0;
	real_t scallops = 1.0; // Worley dish hollows on the walls (0 off)
	real_t speleothems = 1.0; // drip formations: density (0 none)
	Ref<ArrayMesh> spel_mesh[16];
	Ref<ShaderMaterial> spel_material;
	real_t rimstone = 1.0; // rimstone dams and pools below the drips: density (0 none)
	bool rimstone_water = true;
	real_t rimstone_water_radius = 16.0; // m round the camera with live water
	int rimstone_water_max = 12; // the nearest pools with live water
	uint64_t pool_water_usec = 0;
	bool sea_mist = true;
	ObjectID mist_node;
	// The sea in sea caves (docs/ideas/world-links.md, "Sea caves"): a WorldWater producer along each sea cave. The
	// sea's level just outside the mouth (any other producer, read through the contract) runs in as a long wave:
	// lagged by the shallow-water speed sqrt(g h), grown by Green's law as the passage narrows and shoals, damped by
	// the rough floor. A wave that seals the passage traps the air over the back chamber, and the rising water drives
	// it up the blowhole like an oscillating water column's piston (air speed = rise speed x chamber / shaft area).
	struct SeaSurge {
		int cave = -1;
		Vector3 probe; // out in front of the mouth, where the sea answers
		Vector3 dir; // into the cave
		AABB bounds;
		LocalVector<Vector3> axis; // samples along the passage, then the chamber
		LocalVector<float> along, floor, roof, half_width, amp, lag;
		LocalVector<float> level, flow; // live: the surface (m) and the current into the cave (m/s)
		float sea = 0.0f; // the still sea at build time
		float mean = NAN; // the sea's level at the mouth, low-passed (tides pass, waves do not)
		LocalVector<float> history; // the wave at the mouth (m over the mean), one every HISTORY_STEP
		int head = 0;
		double since = 0.0;
		int chamber = -1; // the chamber's sample, or -1
		Vector3 blowhole;
		float chamber_area = 0.0f, shaft_area = 0.0f;
		float spout = 0.0f; // air speed up the blowhole, m/s
		bool sealed = false;
	};
	static constexpr float SURGE_HISTORY_STEP = 0.05f;
	LocalVector<SeaSurge> surges;
	bool sea_surge = true;
	real_t surge_damping = 40.0; // m, the e-folding of the wave along the passage
	bool surges_dirty = true;
	void _surge_build();
	void _surge_update(double p_delta);
	void _update_mist_node();
	// Cave streams (docs/ideas/rocks.md, "Cave streams"): a PhysXRiver3D along each run of stream segments.
	bool cave_streams = true;
	bool streams_dirty = true;
	LocalVector<ObjectID> stream_nodes;
	LocalVector<ObjectID> clast_nodes; // the streams' bed stones (MultiMeshes)
	LocalVector<Ref<ArrayMesh>> clast_library;
	void _scatter_clasts(const LocalVector<Vector3> &p_pts, const LocalVector<float> &p_wid, const LocalVector<float> &p_dep, uint32_t p_seed);
	void _mark_streams(Cave &r_cave, uint32_t p_seed) const;
	void _update_stream_nodes();
	Ref<ShaderMaterial> rim_material;
	bool collision = true;
	Ref<Material> wall_material;
	Ref<Material> ground_material;
	Ref<ShaderMaterial> default_wall;
	uint64_t last_mesh_usec = 0;
	const TerrainGenerator3D *mesh_gen = nullptr; // resolved on the main thread for the mesh workers

	// Streaming round a follow target: chunks within stream_radius are meshed on workers without waiting, those
	// past it (plus a chunk of slack) are freed; entrance patches stay.
	NodePath follow_target;
	real_t stream_radius = 64.0;
	CaveMeshJob *stream_job = nullptr;
	int64_t stream_group = -1;
	Vector3 stream_from;
	bool streamed = false;

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
	void _scatter_blocks(Cave &r_cave, uint32_t p_seed) const;
	real_t _air(const FieldSource &p_src, const Vector3 &p_p, real_t p_cell = -1.0) const;
	real_t _stream_wet(const FieldSource &p_src, const Vector3 &p_p) const;
	real_t _wall_foot(const FieldSource &p_src, const Vector3 &p_p) const;
	real_t _rock(const FieldSource &p_src, const TerrainGenerator3D *p_gen, const Vector3 &p_p, bool *r_ground = nullptr, real_t p_cell = -1.0) const;
	void _gather(const AABB &p_box, FieldSource &r_src) const;
	void _mesh_chunk(const Vector3i &p_key, Chunk &r_chunk, real_t p_cell) const;
	void _drips(const FieldSource &p_src, const TerrainGenerator3D *p_gen, const AABB &p_box, LocalVector<Drip> &r_drips) const;
	static real_t _flowstone(const LocalVector<Drip> &p_drips, const Vector3 &p_p, real_t p_rock, real_t *r_weight = nullptr);
	Ref<ArrayMesh> _spel_mesh(int p_kind, int p_variant);
	void _rimstone(const LocalVector<Drip> &p_drips, const Vector3 &p_own_from, real_t p_own_size, Chunk &r_chunk) const;
	Ref<Material> _rim_material();
	Node3D *_make_pool_water(const RimPool &p_pool, int p_index) const;
	void _pool_water_update();
	static void _mesh_job(void *p_data, uint32_t p_index);
	Ref<Material> _wall_material();
	void _wanted(const AABB &p_area, HashSet<Vector3i> &r_want, HashSet<Vector3i> &r_pinned, bool p_all_patches = false) const;
	void _add_chunk(Chunk &p_chunk, const Ref<Material> &p_wall);
	void _open_holes();
	void _stream_update();
	void _stream_finish(bool p_keep);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	int add_cave(const Vector3 &p_entrance, const Vector3 &p_out);
	int add_sea_cave(const Vector3 &p_entrance, const Vector3 &p_out);
	Array find_sea_entrances(const Rect2 &p_rect, int p_max = 4) const;
	bool is_sea_cave(int p_cave) const;
	PackedVector3Array get_blowholes(int p_cave) const;
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
	int get_mesh_chunk_count() const;
	int get_mesh_triangle_count() const;
	real_t get_last_mesh_msec() const { return real_t(last_mesh_usec) / 1000.0; }
	real_t get_rock_distance(const Vector3 &p_position) const;
	void set_mesh_cell(real_t p_value) { mesh_cell = CLAMP(p_value, real_t(0.05), real_t(4.0)); }
	real_t get_mesh_cell() const { return mesh_cell; }
	void set_near_mesh_cell(real_t p_value) { near_mesh_cell = CLAMP(p_value, real_t(0.0), real_t(4.0)); }
	real_t get_near_mesh_cell() const { return near_mesh_cell; }
	void set_near_mesh_radius(real_t p_value) { near_mesh_radius = MAX(p_value, real_t(0.0)); }
	real_t get_near_mesh_radius() const { return near_mesh_radius; }
	int refine_meshes(const Vector3 &p_center);
	void set_cave_ambient_probes(bool p_enable) { cave_ambient_probes = p_enable; }
	bool get_cave_ambient_probes() const { return cave_ambient_probes; }
	void set_patch_radius(real_t p_value) { patch_radius = CLAMP(p_value, real_t(0.0), real_t(100.0)); }
	real_t get_patch_radius() const { return patch_radius; }
	void set_roughness(real_t p_value) { roughness = CLAMP(p_value, real_t(0.0), real_t(4.0)); }
	real_t get_roughness() const { return roughness; }
	void set_collision(bool p_enable) { collision = p_enable; }
	void set_scallops(real_t p_amount) { scallops = MAX(p_amount, 0.0); }
	real_t get_scallops() const { return scallops; }
	void set_speleothems(real_t p_amount) { speleothems = MAX(p_amount, 0.0); }
	real_t get_speleothems() const { return speleothems; }
	int get_speleothem_count() const;
	void set_rimstone(real_t p_amount) { rimstone = MAX(p_amount, 0.0); }
	real_t get_rimstone() const { return rimstone; }
	void set_rimstone_water(bool p_enable) { rimstone_water = p_enable; }
	bool get_rimstone_water() const { return rimstone_water; }
	void set_rimstone_water_radius(real_t p_metres) { rimstone_water_radius = MAX(p_metres, 0.0); }
	real_t get_rimstone_water_radius() const { return rimstone_water_radius; }
	void set_rimstone_water_max(int p_count) { rimstone_water_max = MAX(p_count, 0); }
	int get_rimstone_water_max() const { return rimstone_water_max; }
	int get_rimstone_water_count() const;
	int get_rimstone_pool_count() const;
	void set_cave_streams(bool p_enable);
	bool get_cave_streams() const { return cave_streams; }
	int get_stream_count() const;
	void set_sea_surge(bool p_enable);
	bool get_sea_surge() const { return sea_surge; }
	void set_surge_damping(real_t p_metres) { surge_damping = MAX(p_metres, real_t(1.0)); surges_dirty = true; }
	real_t get_surge_damping() const { return surge_damping; }
	Dictionary get_sea_cave_water(int p_cave) const;
	Dictionary get_blowhole_spout(int p_cave) const;
	void step_sea_surge(double p_delta) { _surge_update(p_delta); }
	float contract_water_height_at(const Vector3 &p_position) const override;
	Vector3 contract_water_flow_at(const Vector3 &p_position) const override;
	void set_sea_mist(bool p_enable);
	bool get_sea_mist() const { return sea_mist; }
	Node *get_sea_mist_node() const { return ObjectDB::get_instance<Node>(mist_node); }
	Array get_rimstone_pools() const;
	void set_follow_target(const NodePath &p_path);
	NodePath get_follow_target() const { return follow_target; }
	void set_stream_radius(real_t p_metres) { stream_radius = MAX(p_metres, 8.0); }
	real_t get_stream_radius() const { return stream_radius; }
	bool is_streaming() const { return stream_job != nullptr; }
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

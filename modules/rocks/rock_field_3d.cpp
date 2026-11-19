/**************************************************************************/
/*  rock_field_3d.cpp                                                     */
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

#include "rock_field_3d.h"

#include "modules/terrain_deform/landscape_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"

#include "core/config/engine.h"
#include "core/math/convex_hull.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"

#include <cmath>
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

namespace {

inline uint32_t rf_hash(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7FEB352Du;
	p_x ^= p_x >> 15;
	p_x *= 0x846CA68Bu;
	p_x ^= p_x >> 16;
	return p_x;
}

inline uint32_t rf_hash3(int p_a, int p_b, int p_c, uint32_t p_seed) {
	return rf_hash(uint32_t(p_a) * 73856093u ^ rf_hash(uint32_t(p_b) * 19349663u ^ rf_hash(uint32_t(p_c) * 83492791u ^ p_seed)));
}

// Library keys: rock type, family, roundness bin.
inline RockGenerator::RockType key_type(uint32_t p_key) {
	return RockGenerator::RockType(p_key & 0xF);
}
inline RockGenerator::Family key_family(uint32_t p_key) {
	return RockGenerator::Family((p_key >> 4) & 0xF);
}
inline int key_bin(uint32_t p_key) {
	return int((p_key >> 8) & 0xF);
}

// Close-up shapes per variant: one per octave of stone size round the family's typical size (odd count).
constexpr int RF_MARK_BINS = 9;

// Ground detail libraries (docs/ideas/ground-detail.md): the same shapes, drawn from one of their own LODs, at bins
// RF_DETAIL_BIN + 0, 1, 2 for the pebble, gravel and fine classes.
constexpr int RF_DETAIL_BIN = 8;
// Triangles a ground detail stone is drawn with per class: a 1 cm grain a few pixels tall needs no more. (A MultiMesh
// takes one LOD for all its instances, chosen from its whole cell's bounds, so the grains carry their own.)
const int detail_triangles[3] = { 320, 140, 64 };

Ref<ArrayMesh> _detail_mesh(const Ref<ImporterMesh> &p_mesh, int p_class) {
	const int want = detail_triangles[CLAMP(p_class, 0, 2)];
	Array arrays = p_mesh->get_surface_arrays(0);
	const PackedInt32Array base = arrays[Mesh::ARRAY_INDEX];
	PackedInt32Array best = base;
	for (int l = 0; l < p_mesh->get_surface_lod_count(0); l++) {
		const PackedInt32Array idx = p_mesh->get_surface_lod_indices(0, l);
		// The coarsest LOD that still has the triangles wanted (LODs run fine to coarse).
		if (idx.size() / 3 >= want && idx.size() < best.size()) {
			best = idx;
		}
	}
	arrays[Mesh::ARRAY_INDEX] = best;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), p_mesh->get_surface_format(0) & ~Mesh::ARRAY_FLAG_COMPRESS_ATTRIBUTES);
	mesh->surface_set_material(0, p_mesh->get_surface_material(0));
	return mesh;
}

// River transport distances of the cobble roundness bins, in abrasion lengths.
const float cobble_bins[3] = { 0.25f, 1.0f, 3.0f };

// Close-up and fracture shapes are made per octave of stone size, so their marks are carved at the stone's
// own size: the slot of stone k of a group (variant and size bin).
int rf_mark_slot(uint32_t p_library, int p_variant, real_t p_scale) {
	const real_t fam_mpu = RockGenerator::get_family_metres_per_unit(key_family(p_library));
	const int bin = CLAMP(int(Math::round(Math::log2(MAX(p_scale, real_t(1e-4)) / fam_mpu))), -RF_MARK_BINS / 2, RF_MARK_BINS / 2);
	return p_variant * RF_MARK_BINS + bin + RF_MARK_BINS / 2;
}

struct LibraryJob {
	RockGenerator::RockType type;
	RockGenerator::Family family;
	float transport = -1.0f;
	int seed = 0;
	float detail = 1.0f;
	bool close_up = false;
	int fracture = 0; // pieces to split it into (break_stone)
	float marks_scale = 0.0f; // close-up: the size its marks are carved at
	Ref<ImporterMesh> mesh;
	Dictionary meta;
	Ref<Resource> asset; // fracture: the pieces as a PhysXBlastAsset, with their hulls
	float volume = 0.0f;
};

void library_job(void *p_userdata, uint32_t p_index) {
	LibraryJob &job = static_cast<LibraryJob *>(p_userdata)[p_index];
	Ref<RockGenerator> g;
	g.instantiate();
	g->set_rock_type(job.type);
	g->set_family(job.family);
	g->set_seed(job.seed);
	g->set_transport_distance(job.transport);
	g->set_detail(job.detail);
	g->set_close_up(job.close_up);
	g->set_lods(!job.close_up && job.fracture == 0); // a close-up shape only draws within close_up_radius
	g->set_fracture_pieces(job.fracture);
	g->set_marks_scale(job.marks_scale);
	job.mesh = g->build_importer_mesh(job.meta);
}

void close_job(void *p_userdata) {
	library_job(p_userdata, 0);
}

// A collision hull for a piece, few enough points that cooking it is cheap: the hull's own vertices, thinned by
// farthest-point sampling to at most p_max (a rough break face has thousands; PhysX takes 255 and cooks slowly).
PackedVector3Array rf_piece_hull(const PackedVector3Array &p_points, int p_max) {
	Geometry3D::MeshData md;
	if (ConvexHullComputer::convex_hull(Vector<Vector3>(p_points), md) != OK || md.vertices.size() < 4) {
		return p_points;
	}
	const LocalVector<Vector3> &v = md.vertices;
	PackedVector3Array out;
	if (int(v.size()) <= p_max) {
		for (const Vector3 &p : v) {
			out.push_back(p);
		}
		return out;
	}
	LocalVector<real_t> dist;
	dist.resize(v.size());
	for (uint32_t i = 0; i < v.size(); i++) {
		dist[i] = 1e30f;
	}
	uint32_t pick = 0;
	for (int k = 0; k < p_max; k++) {
		out.push_back(v[pick]);
		uint32_t far = 0;
		real_t far_d = -1.0f;
		for (uint32_t i = 0; i < v.size(); i++) {
			dist[i] = MIN(dist[i], v[i].distance_squared_to(v[pick]));
			if (dist[i] > far_d) {
				far_d = dist[i];
				far = i;
			}
		}
		pick = far;
	}
	return out;
}

// Breaking: the shape split into pieces, then (still on the worker) the Blast asset and each piece's hull, so a
// prepared stone breaks with no fracture, authoring or hull cooking on the main thread.
void fracture_job(void *p_userdata) {
	LibraryJob *job = static_cast<LibraryJob *>(p_userdata);
	library_job(p_userdata, 0);
	if (job->mesh.is_null() || job->mesh->get_surface_count() == 0 || !job->meta.has("rock_chunks")) {
		return;
	}
	const Array chunks = job->meta["rock_chunks"];
	const Array bonds = job->meta["rock_bonds"];
	Array pieces;
	LocalVector<int> remap;
	remap.resize(chunks.size());
	for (int c = 0; c < chunks.size(); c++) {
		const Array a = chunks[c];
		remap[c] = a.size() == Mesh::ARRAY_MAX ? pieces.size() : -1;
		if (remap[c] >= 0) {
			pieces.push_back(a);
		}
	}
	Array kept;
	for (int b = 0; b < bonds.size(); b++) {
		Dictionary d = Dictionary(bonds[b]).duplicate();
		const Vector2i pair = d["chunks"];
		if (pair.x < int(remap.size()) && pair.y < int(remap.size()) && remap[pair.x] >= 0 && remap[pair.y] >= 0) {
			d["chunks"] = Vector2i(remap[pair.x], remap[pair.y]);
			kept.push_back(d);
		}
	}
	Object *authoring = ClassDB::instantiate("PhysXBlastAuthoring");
	if (!authoring) {
		return;
	}
	Ref<RefCounted> holder(Object::cast_to<RefCounted>(authoring));
	if (holder.is_null()) {
		memdelete(authoring);
		return;
	}
	if (pieces.size() < 2) {
		return;
	}
	const Ref<Resource> asset = authoring->call("build_prefractured", job->mesh->get_surface_arrays(0), pieces, kept);
	if (asset.is_null()) {
		return;
	}
	const Array points = asset->call("get_chunk_points");
	Array hulls;
	for (int c = 0; c < points.size(); c++) {
		hulls.push_back(rf_piece_hull(points[c], 64));
	}
	asset->call("set_chunk_hulls", hulls);
	job->asset = asset;
	job->volume = float(job->meta.get("rock_volume", 0.0));
}

// Ground normal from the generator's height, by central differences.
Vector3 ground_normal(const TerrainGenerator3D *p_gen, real_t p_x, real_t p_z, real_t p_e) {
	const real_t hx = p_gen->get_height(Vector3(p_x + p_e, 0, p_z)) - p_gen->get_height(Vector3(p_x - p_e, 0, p_z));
	const real_t hz = p_gen->get_height(Vector3(p_x, 0, p_z + p_e)) - p_gen->get_height(Vector3(p_x, 0, p_z - p_e));
	const Vector3 n(-hx / (2.0f * p_e), 1.0f, -hz / (2.0f * p_e));
	return Math::is_finite(n.x) && Math::is_finite(n.z) ? n.normalized() : Vector3(0, 1, 0);
}

// Sequential deposition (Visscher and Bolsterli 1972): stones dropped one at a time, largest first, each coming to
// rest on the ground or on the stones already down (as spheres for the contact), so a cover packs without overlaps.
struct Deposit {
	float CELL = 0.75f; // m; the ground detail's grains use a finer grid
	struct Item {
		Vector2 xz;
		float top_centre; // height of the sphere's centre
		float r;
	};
	LocalVector<Item> items;
	HashMap<Vector2i, LocalVector<uint32_t>> grid;
	float max_r = 0.0f;

	// The rise above p_ground_centre (the centre's height resting on the ground) where a sphere of radius p_r at
	// p_xz rests; -1 if it would stand more than p_max_rise above the ground.
	float rest(const Vector2 &p_xz, float p_r, float p_ground_centre, float p_max_rise) const {
		float best = p_ground_centre;
		const int reach = int(Math::ceil((p_r + max_r) / CELL));
		const Vector2i c(int(Math::floor(p_xz.x / CELL)), int(Math::floor(p_xz.y / CELL)));
		for (int dz = -reach; dz <= reach; dz++) {
			for (int dx = -reach; dx <= reach; dx++) {
				const LocalVector<uint32_t> *b = grid.getptr(c + Vector2i(dx, dz));
				if (!b) {
					continue;
				}
				for (uint32_t i : *b) {
					const Item &it = items[i];
					const float rr = p_r + it.r;
					const float d2 = (it.xz - p_xz).length_squared();
					if (d2 >= rr * rr) {
						continue;
					}
					best = MAX(best, it.top_centre + Math::sqrt(rr * rr - d2));
				}
			}
		}
		const float rise = best - p_ground_centre;
		return rise > p_max_rise ? -1.0f : rise;
	}
	void add(const Vector2 &p_xz, float p_centre, float p_r) {
		items.push_back({ p_xz, p_centre, p_r });
		max_r = MAX(max_r, p_r);
		grid[Vector2i(int(Math::floor(p_xz.x / CELL)), int(Math::floor(p_xz.y / CELL)))].push_back(items.size() - 1);
	}
};

} // namespace

/* Setup */

// A ground detail job's generator, resolved on the main thread: the scene tree is not read from a worker.
static thread_local TerrainGenerator3D *rf_job_generator = nullptr;
// The coarse cell a worker is placing: shape libraries are looked up under library_lock, missing ones noted.
static thread_local void *rf_coarse_job = nullptr;

TerrainGenerator3D *RockField3D::_get_generator() const {
	if (rf_job_generator) {
		return rf_job_generator;
	}
	if (!is_inside_tree() || generator_path.is_empty()) {
		return nullptr;
	}
	return Object::cast_to<TerrainGenerator3D>(get_node_or_null(generator_path));
}

Vector3 RockField3D::_target_position() const {
	if (!follow_target_path.is_empty()) {
		if (const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target_path))) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		if (const Camera3D *cam = get_viewport()->get_camera_3d()) {
			return cam->get_global_position();
		}
	}
	return get_global_position();
}

// The ground detail is about what the camera sees up close, so it is centred on the camera (the follow target when
// there is none).
Vector3 RockField3D::_detail_center() const {
	if (is_inside_tree() && get_viewport()) {
		if (const Camera3D *cam = get_viewport()->get_camera_3d()) {
			return cam->get_global_position();
		}
	}
	return _target_position();
}

real_t RockField3D::_cell_size(Layer p_layer) const {
	switch (p_layer) {
		case LAYER_DETAIL_PEBBLE:
			return 8.0;
		case LAYER_DETAIL_GRAVEL:
			return 4.0;
		case LAYER_DETAIL_FINE:
			return 2.0;
		default:
			break;
	}
	return (p_layer == LAYER_SCREE || p_layer == LAYER_RIVER || p_layer == LAYER_SHORE) ? 32.0 : 128.0; // tors and coast 128 m too
}

// Shapes per library. Big rounded stones (river and beach cobbles and boulders, glacial erratics, rockfall blocks)
// lie by the hundred in one view, each one large enough to recognise, so they get twice the variants: two of the
// same shape side by side read as a copy, where two pebbles do not.
int RockField3D::_variant_count(uint32_t p_key) const {
	const RockGenerator::Family f = key_family(p_key);
	const bool big = key_bin(p_key) < RF_DETAIL_BIN && (f == RockGenerator::FAMILY_COBBLE || f == RockGenerator::FAMILY_GLACIAL || f == RockGenerator::FAMILY_ROCKFALL || f == RockGenerator::FAMILY_SCREE);
	return big ? MIN(variants * 2, 32) : variants;
}

uint32_t RockField3D::_library_key(RockGenerator::RockType p_type, RockGenerator::Family p_family, int p_bin) const {
	return uint32_t(p_type) | (uint32_t(p_family) << 4) | (uint32_t(p_bin) << 8);
}

// Shape libraries are built on workers: _ensure_libraries waits for them; while streaming, _ensure_library starts them
// and lets the cell wait instead (library_pending), so a first coast stack or hoodoo is no main-thread hitch.
struct RockField3D::LibraryBuild {
	LocalVector<uint32_t> keys;
	LocalVector<uint32_t> first; // each key's first job (and one past the last key's)
	LocalVector<LibraryJob> jobs;
	WorkerThreadPool::GroupID group = 0;
};

RockField3D::LibraryBuild *RockField3D::_start_libraries(const LocalVector<uint32_t> &p_missing) {
	LibraryBuild *b = memnew(LibraryBuild);
	b->keys = p_missing;
	LocalVector<uint32_t> &missing = b->keys;
	LocalVector<LibraryJob> &jobs = b->jobs;
	for (uint32_t k : missing) {
		RockGenerator::get_material(key_type(k)); // made here, read by the workers
		b->first.push_back(jobs.size());
		for (int v = 0; v < _variant_count(k); v++) {
			LibraryJob j;
			j.type = key_type(k);
			j.family = key_family(k);
			j.seed = seed * 1000 + v;
			j.detail = float(detail);
			if (j.family == RockGenerator::FAMILY_COBBLE) {
				j.transport = RockGenerator::get_lithology(j.type).abrasion_length * cobble_bins[CLAMP(key_bin(k), 0, 2)];
			} else if (j.family == RockGenerator::FAMILY_PEBBLE && key_bin(k) < RF_DETAIL_BIN) {
				// Shingle bins: from little-worn to long-worn (1.5 to 6 abrasion lengths), each with its own shapes.
				static const float pebble_bins[4] = { 1.5f, 2.5f, 4.0f, 6.0f };
				j.transport = RockGenerator::get_lithology(j.type).abrasion_length * pebble_bins[CLAMP(key_bin(k), 0, 3)];
				j.seed += 131 * key_bin(k);
			} else if (key_bin(k) >= RF_DETAIL_BIN) {
				// Ground detail classes (pebbles, gravel, fine gravel): each its own shapes, so a finer class is not
				// the coarser one shrunk, and no shape repeats across the scales.
				j.seed += 131 * key_bin(k);
			}
			jobs.push_back(j);
		}
	}
	b->first.push_back(jobs.size());
	b->group = WorkerThreadPool::get_singleton()->add_native_group_task(&library_job, jobs.ptr(), int(jobs.size()), -1, true, "RockField3D libraries");
	return b;
}

void RockField3D::_finish_libraries(LibraryBuild *p_build) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(p_build->group);
	LocalVector<uint32_t> &missing = p_build->keys;
	LocalVector<LibraryJob> &jobs = p_build->jobs;
	for (uint32_t m = 0; m < missing.size(); m++) {
		Library lib;
		for (uint32_t ji = p_build->first[m]; ji < p_build->first[m + 1]; ji++) {
			LibraryJob &j = jobs[ji];
			if (j.mesh.is_null()) {
				continue;
			}
			Ref<ArrayMesh> mesh = key_bin(missing[m]) >= RF_DETAIL_BIN ? _detail_mesh(j.mesh, key_bin(missing[m]) - RF_DETAIL_BIN) : j.mesh->get_mesh();
			lib.meshes.push_back(mesh);
			lib.axes.push_back(j.meta.get("rock_axes", Vector3(1, 0.5, 1)));
			lib.bottom.push_back(j.meta.get("rock_bottom", -0.5));
			lib.hulls.push_back(j.meta.get("rock_hull", PackedVector3Array()));
			lib.potholes.push_back(j.meta.get("rock_potholes", PackedFloat32Array()));
			lib.caves.push_back(j.meta.get("rock_cave", Transform3D(Basis().scaled(Vector3()), Vector3())));
			// Cap samples: the mesh's vertices thinned to one per 0.06-unit voxel (normals averaged).
			LocalVector<Vector3> cp, cn;
			{
				const Array arrays = mesh->surface_get_arrays(0);
				const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
				const PackedVector3Array norms = arrays[Mesh::ARRAY_NORMAL];
				HashMap<Vector3i, uint32_t> voxel;
				const float cell = 0.06f;
				for (int i = 0; i < verts.size(); i++) {
					const Vector3i key = Vector3i((verts[i] / cell).floor());
					const Vector3 nrm = i < norms.size() ? norms[i] : Vector3();
					uint32_t *at = voxel.getptr(key);
					if (at) {
						cn[*at] += nrm;
					} else {
						voxel.insert(key, cp.size());
						cp.push_back(verts[i]);
						cn.push_back(nrm);
					}
				}
				for (Vector3 &n : cn) {
					n = n.length_squared() > 1e-12f ? n.normalized() : Vector3(0, 1, 0);
				}
			}
			lib.cap_points.push_back(std::move(cp));
			lib.cap_normals.push_back(std::move(cn));
		}
		lib.close.resize(lib.meshes.size() * RF_MARK_BINS);
		lib.close_state.resize(lib.meshes.size() * RF_MARK_BINS);
		lib.close_used.resize(lib.meshes.size() * RF_MARK_BINS);
		for (uint32_t v = 0; v < lib.close.size(); v++) {
			lib.close_state[v] = 0;
			lib.close_used[v] = 0;
		}
		MutexLock lock(library_lock);
		libraries.insert(missing[m], lib);
	}
	last_library_msec += double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	memdelete(p_build);
}

void RockField3D::_ensure_libraries(const LocalVector<uint32_t> &p_keys) {
	LocalVector<uint32_t> missing;
	for (uint32_t k : p_keys) {
		if (!libraries.has(k) && missing.find(k) < 0) {
			missing.push_back(k);
		}
	}
	// Ones a streaming cell started: wait for them rather than build them twice.
	for (int i = int(missing.size()) - 1; i >= 0; i--) {
		if (LibraryBuild **b = library_builds.getptr(missing[i])) {
			LibraryBuild *build = *b;
			for (uint32_t k : build->keys) {
				library_builds.erase(k);
			}
			_finish_libraries(build);
			missing.remove_at(i);
		}
	}
	for (int i = int(missing.size()) - 1; i >= 0; i--) {
		if (libraries.has(missing[i])) {
			missing.remove_at(i);
		}
	}
	if (missing.is_empty()) {
		return;
	}
	_finish_libraries(_start_libraries(missing));
}

// Libraries the streaming started that are done; p_wait waits for all of them.
void RockField3D::_collect_libraries(bool p_wait) {
	LocalVector<LibraryBuild *> done;
	for (const KeyValue<uint32_t, LibraryBuild *> &kv : library_builds) {
		if ((p_wait || WorkerThreadPool::get_singleton()->is_group_task_completed(kv.value->group)) && done.find(kv.value) < 0) {
			done.push_back(kv.value);
		}
	}
	for (LibraryBuild *b : done) {
		for (uint32_t k : b->keys) {
			library_builds.erase(k);
		}
		_finish_libraries(b);
	}
}

RockField3D::Library *RockField3D::_ensure_library(uint32_t p_key) {
	if (rf_coarse_job) {
		CoarseJob *job = static_cast<CoarseJob *>(rf_coarse_job);
		MutexLock lock(library_lock);
		Library *lib = libraries.getptr(p_key);
		if (!lib) {
			if (job->missing.find(p_key) < 0) {
				job->missing.push_back(p_key);
			}
			return nullptr;
		}
		return lib->meshes.is_empty() ? nullptr : lib;
	}
	if (library_async && !libraries.has(p_key)) {
		if (!library_builds.has(p_key)) {
			LocalVector<uint32_t> k;
			k.push_back(p_key);
			RockGenerator::get_material(key_type(p_key));
			library_builds.insert(p_key, _start_libraries(k));
		}
		library_pending = true;
		return nullptr;
	}
	if (!libraries.has(p_key)) {
		LocalVector<uint32_t> k;
		k.push_back(p_key);
		_ensure_libraries(k);
	}
	Library *lib = libraries.getptr(p_key);
	return (lib && !lib->meshes.is_empty()) ? lib : nullptr;
}

RockGenerator::RockType RockField3D::_type_at(const TerrainGenerator3D *p_gen, const Vector3 &p_pos) const {
	if (use_strata && p_gen && p_gen->is_strata_enabled() && p_gen->get_rock_hardness(p_pos) < 0.5) {
		return soft_rock_type;
	}
	return rock_type;
}

void RockField3D::_prepare_rivers() {
	if (rivers_ready) {
		return;
	}
	rivers.clear();
	rivers_ready = true;
	const TerrainGenerator3D *gen = _get_generator();
	if (!gen) {
		return;
	}
	const Array rs = gen->get_rivers(river_min_area);
	for (int r = 0; r < rs.size(); r++) {
		const Dictionary d = rs[r];
		const PackedVector3Array pts = d.get("points", PackedVector3Array());
		const PackedFloat32Array widths = d.get("widths", PackedFloat32Array());
		const PackedFloat32Array areas = d.get("areas", PackedFloat32Array());
		if (pts.size() < 2) {
			continue;
		}
		River rv;
		float dist = 0.0f;
		rv.bounds = Rect2(pts[0].x, pts[0].z, 0, 0);
		for (int i = 0; i < pts.size(); i++) {
			if (i > 0) {
				dist += Vector2(pts[i].x - pts[i - 1].x, pts[i].z - pts[i - 1].z).length();
			}
			rv.points.push_back(pts[i]);
			rv.widths.push_back(i < widths.size() ? widths[i] : 2.0f);
			rv.distance.push_back(dist);
			rv.discharge.push_back(i < areas.size() ? float(gen->get_discharge(areas[i])) : 0.1f);
			rv.bounds.expand_to(Vector2(pts[i].x, pts[i].z));
		}
		rv.head_type = _type_at(gen, pts[0]);
		rivers.push_back(rv);
	}
}

/* Placement */

// A stone at rest: yawed, its up partly following the ground (p_follow_ground 0 level, 1 on the slope), tilted, sunk
// by a share of its height (and more on a slope, so the uphill side does not float).
void RockField3D::_rest(const TerrainGenerator3D *p_gen, Stone &r_stone, const Library &p_lib, const Vector3 &p_xz, real_t p_yaw, real_t p_sink, const Vector3 &p_tilt_axis, real_t p_tilt, real_t p_follow_ground) const {
	const int v = r_stone.variant;
	const Vector3 axes = p_lib.axes[v];
	const float s = r_stone.scale;
	const real_t ground = p_gen->get_height(Vector3(p_xz.x, 0, p_xz.z));
	const Vector3 n = ground_normal(p_gen, p_xz.x, p_xz.z, MAX(real_t(0.5), real_t(axes.x * s)));
	const Vector3 up = Vector3(0, 1, 0).lerp(n, p_follow_ground).normalized();
	Basis b(Vector3(0, 1, 0), p_yaw);
	const Vector3 axis = Vector3(0, 1, 0).cross(up);
	if (axis.length_squared() > 1e-8f) {
		b = Basis(axis.normalized(), Math::acos(CLAMP(up.y, real_t(-1.0), real_t(1.0)))) * b;
	}
	if (p_tilt != 0.0 && p_tilt_axis.length_squared() > 1e-8f) {
		b = Basis(p_tilt_axis.normalized(), p_tilt) * b;
	}
	const real_t slope = MIN(Math::sqrt(MAX(real_t(1.0) / (n.y * n.y) - real_t(1.0), real_t(0.0))), real_t(1.0));
	const real_t height = 2.0f * axes.y * s;
	real_t sink = p_sink * height + slope * MAX(axes.x, axes.z) * s * (1.0f - p_follow_ground) * 0.6f;
	// Convex ground (crests, ridges, knolls) falls away under a big block's edges, and a block seated on the centre
	// point alone looks perched. Seat it to the mean height round its footprint (the slope cancels in the mean, so
	// only the curvature counts), at most a third of its height deeper.
	const real_t reach = MAX(axes.x, axes.z) * s * 0.85f;
	if (reach > 0.25f) {
		real_t ring = 0.0;
		for (int k = 0; k < 6; k++) {
			const real_t a = p_yaw + real_t(k) * real_t(Math::TAU) / 6.0f;
			ring += p_gen->get_height(Vector3(p_xz.x + Math::cos(a) * reach, 0, p_xz.z + Math::sin(a) * reach));
		}
		sink += CLAMP(ground - ring / 6.0f, real_t(0.0), height / 3.0f);
	}
	r_stone.basis = b.scaled(Vector3(s, s, s));
	real_t y = ground - p_lib.bottom[v] * s - sink;
	// No daylight under the stone: the seat above uses the centre's ground and the unrotated bottom, so on a slope (or
	// with the stone tilted) its downhill underside can stand off the ground. Take the stone's underside (its
	// ellipsoid's 15 % nearest the ground, measured along the ground's normal: the face that bears on the slope, not the
	// lowest in height, which on a steep slope is only its downhill edge) and lower the stone until all of it meets
	// the ground there; it never rises. Seating to the lowest support point is the contact rule of SpeedTree's and Megascans'
	// scatter tools ("align to terrain, no gaps") and of rigid-body rest.
	if (reach > 0.08f) {
		static Vector3 dirs[48];
		static bool dirs_made = false;
		if (!dirs_made) { // a Fibonacci sphere, made once (the same on every thread)
			for (int i = 0; i < 48; i++) {
				const real_t yy = 1.0 - (real_t(i) + 0.5) * 2.0 / 48.0;
				const real_t rr = Math::sqrt(MAX(real_t(1.0) - yy * yy, real_t(0.0)));
				const real_t ph = real_t(i) * 2.39996323;
				dirs[i] = Vector3(Math::cos(ph) * rr, yy, Math::sin(ph) * rr);
			}
			dirs_made = true;
		}
		Vector3 pts[48];
		real_t dn[48];
		real_t lo = 1e9, hi = -1e9;
		for (int i = 0; i < 48; i++) {
			pts[i] = r_stone.basis.xform(dirs[i] * axes);
			dn[i] = pts[i].dot(n);
			lo = MIN(lo, dn[i]);
			hi = MAX(hi, dn[i]);
		}
		const real_t band = lo + 0.15 * (hi - lo);
		const real_t floor_y = y - 0.35 * height; // a ledge or step under one side: lowered by at most a third of its height
		for (int i = 0; i < 48; i++) {
			if (dn[i] <= band) {
				// The ellipsoid's centre over the mesh origin, so that its bottom is the mesh's (about 0 for most stones).
				const real_t centre = (p_lib.bottom[v] + axes.y) * s;
				y = MIN(y, p_gen->get_height(Vector3(p_xz.x + pts[i].x, 0, p_xz.z + pts[i].z)) - pts[i].y - centre);
			}
		}
		y = MAX(y, floor_y);
	}
	r_stone.position = Vector3(p_xz.x, y, p_xz.z);
	r_stone.ground = ground;
	r_stone.footprint = MAX(axes.x, axes.z) * s * 0.85f;
}

void RockField3D::_place(Layer p_layer, const Rect2 &p_rect, uint32_t p_seed, LocalVector<Stone> &r_stones, const HashMap<uint32_t, Library *> *p_libs, const LocalVector<DetailSeed> *p_seeds) {
	TerrainGenerator3D *gen = _get_generator();
	if (!gen || !_layer_on(p_layer) || layer_density[p_layer] <= 0.0) {
		return;
	}
	RandomPCG rng(p_seed);
	const real_t density = layer_density[p_layer];
	const real_t water_skip = 0.05;
	auto pick_variant = [&](const Library *p_lib) { return uint16_t(rng.rand() % p_lib->meshes.size()); };
	// Stones of a packed cover (scree, river beds, shore): placed by sequential deposition, largest first.
	struct Pending {
		Vector3 xz;
		float size; // b axis, m
		uint32_t key;
		float yaw, tilt, sink, follow, film;
		Vector3 tilt_axis;
	};
	LocalVector<Pending> pending;
	// A debris skirt round a big block: flakes and fragments it shed as it weathered (granular disintegration and
	// spalling) or broke off when it landed collect at its foot, more on the downhill side. A few to ten angular
	// chips of 4 to 15% of its size, a power law, in a ring just outside its footprint.
	auto skirt = [&](const Stone &p_big, RockGenerator::RockType p_type, float p_share) {
		const float b = 2.0f * p_big.scale;
		if (b < 0.8f) {
			return;
		}
		const uint32_t key = _library_key(p_type, RockGenerator::FAMILY_SCREE, 0);
		Library *lib = _ensure_library(key);
		if (!lib) {
			return;
		}
		const Vector3 n = ground_normal(gen, p_big.position.x, p_big.position.z, MAX(real_t(0.5), real_t(p_big.footprint)));
		const Vector3 down = Vector3(n.x, 0, n.z);
		const int count = int(float(3 + MIN(int(b * 3.0f), 7)) * p_share + rng.randf());
		for (int k = 0; k < count; k++) {
			const float d = b * MIN(0.04f * Math::pow(1.0f - rng.randf() * 0.97f, -1.0f / 1.8f), 0.15f);
			if (d < min_size) {
				continue;
			}
			const float a = rng.randf() * float(Math::TAU);
			const float r = float(p_big.footprint) * (0.8f + 0.7f * Math::pow(rng.randf(), 1.5f)) + d * 0.5f;
			Vector3 p = Vector3(p_big.position.x, 0, p_big.position.z) + Vector3(Math::cos(a), 0, Math::sin(a)) * r + down * (float(p_big.footprint) * 0.6f * rng.randf());
			if (gen->get_water_depth(p) > water_skip) {
				continue;
			}
			Stone st;
			st.layer = p_big.layer;
			st.library = key;
			st.variant = pick_variant(lib);
			st.scale = d * 0.5f;
			_rest(gen, st, *lib, p, rng.randf() * Math::TAU, 0.2 + 0.2 * rng.randf(), Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f), Math::deg_to_rad(25.0f) * rng.randf(), 0.8);
			r_stones.push_back(st);
		}
	};

	switch (p_layer) {
		case LAYER_ROCKFALL: {
			const PackedVector4Array bl = gen->get_boulders(p_rect, density);
			Library *lib = _ensure_library(_library_key(rock_type, RockGenerator::FAMILY_ROCKFALL, 0));
			if (!lib) {
				return;
			}
			for (int i = 0; i < bl.size(); i++) {
				const Vector4 b = bl[i];
				Stone st;
				st.layer = p_layer;
				st.library = _library_key(rock_type, RockGenerator::FAMILY_ROCKFALL, 0);
				st.variant = pick_variant(lib);
				st.scale = float(b.w) * 0.9f;
				if (2.0f * st.scale < min_size) {
					continue;
				}
				_rest(gen, st, *lib, Vector3(b.x, 0, b.z), rng.randf() * Math::TAU, 0.15 + 0.15 * rng.randf(), Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f), Math::deg_to_rad(12.0f) * rng.randf(), 0.5);
				r_stones.push_back(st);
				skirt(st, rock_type, 1.0f);
			}
		} break;
		case LAYER_GLACIAL: {
			// Boulders in the till: about one per 250 m2 of full till, sizes a power law (exponent 2.2) from 0.5 to 3 m;
			// a few erratics of the far rock, larger and rounder (carried furthest).
			const real_t sub = 8.0;
			for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + sub * 0.5f, 0, z + sub * 0.5f);
					const Color geo = gen->get_geology_at(c);
					// Till (moraines) holds the most; ground the last glaciation's ice covered keeps a scatter of the
					// blocks it dropped as it melted back (boulder fields, erratics on scoured rock).
					const float till = geo.b < 0.0f ? -geo.b : 0.0f;
					const float scoured = 0.35f * Math::smoothstep(5.0f, 30.0f, float(gen->get_glacier_depth(c)));
					const float cover = MAX(till, scoured);
					if (cover < 0.05f) {
						continue;
					}
					const float want = cover * float(sub * sub) / 250.0f * float(density);
					int count = int(want);
					if (rng.randf() < want - count) {
						count++;
					}
					for (int k = 0; k < count; k++) {
						const Vector3 p(x + rng.randf() * sub, 0, z + rng.randf() * sub);
						if (gen->get_water_depth(p) > water_skip || ground_normal(gen, p.x, p.z, 2.0).y < 0.82f) {
							continue; // in water, or too steep to hold a boulder (about 35 degrees)
						}
						const bool erratic = rng.randf() < 0.08f;
						const RockGenerator::RockType t = erratic ? erratic_rock_type : rock_type;
						const uint32_t key = _library_key(t, RockGenerator::FAMILY_GLACIAL, 0);
						Library *lib = _ensure_library(key);
						if (!lib) {
							continue;
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const float d = MIN(0.5f * Math::pow(1.0f - rng.randf() * 0.999f, -1.0f / 2.2f), 3.0f) * (erratic ? 1.6f : 1.0f);
						st.scale = d * 0.5f;
						if (d < min_size) {
							continue;
						}
						_rest(gen, st, *lib, p, rng.randf() * Math::TAU, 0.25 + 0.25 * rng.randf(), Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f), Math::deg_to_rad(15.0f) * rng.randf(), 0.6);
						r_stones.push_back(st);
						skirt(st, t, 0.5f); // a boulder long in the till has shed less, and grus has washed away

					}
				}
			}
		} break;
		case LAYER_SCREE: {
			// The talus cover (docs/ideas/rocks.md, "Scree"): clast-supported, the stones touching, some 60 to 70% of
			// the ground under them with gravel between (Statham 1976; Sass and Krautblatter 2007). Blocks a power law
			// (exponent 2.5) in size from the smallest kept, coarser where the apron thins at its toe (fall sorting:
			// the big blocks run furthest); laid largest first by sequential deposition (Visscher and Bolsterli 1972),
			// each where it touches but does not pass through those already down; then a tail of cobbles (5 to 15 cm)
			// in the gaps. A fifth to a third of each stone sunk in the fines.
			const real_t sub = 2.0;
			const float dmin = MAX(float(min_size), 0.15f);
			// The tail starts at 10 cm: ground_detail draws the 1 to 15 cm pebbles and chips between the stones on the GPU,
			// so the field keeps to the coarse end and the two do not double up.
			const float tail_min = MAX(float(min_size), 0.1f);
			struct Disc {
				float x, z, r;
			};
			HashMap<int64_t, LocalVector<Disc>> laid;
			const float grid = 0.5f;
			auto gkey = [&](int64_t p_x, int64_t p_z) { return (p_x << 32) ^ (p_z & 0xffffffff); };
			auto fits = [&](float p_x, float p_z, float p_r) {
				const int64_t gx = int64_t(Math::floor(p_x / grid)), gz = int64_t(Math::floor(p_z / grid));
				const int64_t span = 1 + int64_t(Math::ceil(2.0f * 0.9f / grid));
				for (int64_t i = -span; i <= span; i++) {
					for (int64_t k = -span; k <= span; k++) {
						const LocalVector<Disc> *cellv = laid.getptr(gkey(gx + i, gz + k));
						if (!cellv) {
							continue;
						}
						for (const Disc &o : *cellv) {
							const float dx = o.x - p_x, dz = o.z - p_z, m = 0.8f * (o.r + p_r);
							if (dx * dx + dz * dz < m * m) {
								return false;
							}
						}
					}
				}
				return true;
			};
			auto lay = [&](float p_x, float p_z, float p_r) {
				laid[gkey(int64_t(Math::floor(p_x / grid)), int64_t(Math::floor(p_z / grid)))].push_back({ p_x, p_z, p_r });
			};
			struct Try {
				Vector3 xz;
				float size;
				float t;
			};
			struct TrySizeGreater {
				bool operator()(const Try &p_a, const Try &p_b) const { return p_a.size > p_b.size; }
			};
			LocalVector<Try> tries;
			for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + sub * 0.5f, 0, z + sub * 0.5f);
					// Talus under a fifth is soil with a few stones (the ground material shows it); full scree from there.
					const float t = (float(gen->get_talus(c)) - 0.2f) / 0.8f;
					if (t < 0.05f) {
						continue;
					}
					// Blocks to cover 65% of full talus (the mean b axis squared of the power law cut at 0.9 m is about
					// 3.4 dmin^2; the footprint about 1.4 b by b), tried four times over: the spacing test turns many
					// away as the cover jams.
					// Fall sorting down the cone (the generator's get_scree_sorting: 0 at the apex, 1 at the toe): small stones
					// and fines high up, the big blocks run furthest.
					const float sort = CLAMP(float(gen->get_scree_sorting(c)), 0.0f, 1.0f);
					const float sort_k = 0.5f + 1.0f * sort;
					// (the count scaled so the cover stays the same at either end: more, smaller stones up the cone)
					const float want = 4.0f * 0.65f * t * float(sub * sub) * float(density) / (0.785f * 1.4f * 3.4f * dmin * dmin * sort_k * sort_k);
					int count = int(want);
					if (rng.randf() < want - count) {
						count++;
					}
					for (int k = 0; k < count; k++) {
						tries.push_back({ Vector3(x + rng.randf() * sub, 0, z + rng.randf() * sub), MAX(MIN(dmin * Math::pow(1.0f - rng.randf() * 0.999f, -1.0f / 2.5f), 0.9f) * (1.0f + 1.2f * (1.0f - t)) * sort_k, tail_min), t });
					}
					// The toe: the largest blocks roll furthest and stop on the thin apron below the cone, car- to
					// house-sized (2 to 6 m), a few per hundred metres of toe (fall sorting; Kirkby and Statham 1975).
					const float toe = 1.0f - t;
					if (toe > 0.4f && rng.randf() < 0.012f * toe * float(density)) {
						Pending pd;
						pd.xz = Vector3(x + rng.randf() * sub, 0, z + rng.randf() * sub);
						pd.size = 2.0f + 4.0f * rng.randf() * rng.randf();
						pd.key = _library_key(rock_type, RockGenerator::FAMILY_ROCKFALL, 0);
						pd.yaw = rng.randf() * Math::TAU;
						pd.tilt = Math::deg_to_rad(12.0f) * rng.randf();
						pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
						pd.sink = 0.12f + 0.12f * rng.randf();
						pd.follow = 0.5f;
						pd.film = 0.0f;
						pending.push_back(pd);
						lay(pd.xz.x, pd.xz.z, 0.45f * pd.size);
					}
					// The cobble tail in the gaps: some 6% more cover in stones of 10 to 15 cm, tried twice over.
					if (tail_min < dmin) {
						const float tm = 0.5f * (tail_min + dmin);
						const float want_t = 2.0f * 0.06f * t * float(sub * sub) * float(density) / (0.785f * 1.4f * tm * tm);
						const int nt = int(want_t) + (rng.randf() < want_t - int(want_t) ? 1 : 0);
						for (int k = 0; k < nt; k++) {
							tries.push_back({ Vector3(x + rng.randf() * sub, 0, z + rng.randf() * sub), Math::lerp(tail_min, dmin, rng.randf() * rng.randf()), t });
						}
					}
				}
			}
			// Largest first: each settles among those already down.
			tries.sort_custom<TrySizeGreater>();
			const RockGenerator::RockType type = rock_type;
			const uint32_t key = _library_key(type, RockGenerator::FAMILY_SCREE, 0);
			for (const Try &tr : tries) {
				// The footprint's mean radius (axes about 1.35, 0.7 and 1 times b over 2), nestled a little.
				const float r = 0.45f * tr.size;
				if (!fits(tr.xz.x, tr.xz.z, r)) {
					continue;
				}
				// None on the rock wall above the cone: a stone stays only below the angle of repose (talus stands at 32
				// to 38 degrees; past 42 a block would slide or the ground is the cliff itself, Statham 1976).
				{
					const real_t e = MAX(real_t(0.6), real_t(r));
					const real_t gx = (gen->get_height(tr.xz + Vector3(e, 0, 0)) - gen->get_height(tr.xz - Vector3(e, 0, 0))) / (2.0 * e);
					const real_t gz = (gen->get_height(tr.xz + Vector3(0, 0, e)) - gen->get_height(tr.xz - Vector3(0, 0, e))) / (2.0 * e);
					if (gx * gx + gz * gz > 0.9 * 0.9) {
						continue;
					}
				}
				lay(tr.xz.x, tr.xz.z, r);
				Pending pd;
				pd.xz = tr.xz;
				pd.size = tr.size;
				pd.key = key;
				pd.yaw = rng.randf() * Math::TAU;
				pd.tilt = Math::deg_to_rad(20.0f) * rng.randf();
				pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
				pd.sink = 0.2f + 0.13f * rng.randf();
				pd.follow = 0.8f;
				pd.film = 0.0f;
				pending.push_back(pd);
			}
		} break;
		case LAYER_RIVER: {
			_prepare_rivers();
			for (const River &rv : rivers) {
				if (!rv.bounds.grow(20.0).intersects(p_rect)) {
					continue;
				}
				const float labr = RockGenerator::get_lithology(rv.head_type).abrasion_length;
				for (uint32_t i = 0; i + 1 < rv.points.size(); i++) {
					const Vector3 a = rv.points[i], b = rv.points[i + 1];
					const float w = 0.5f * (rv.widths[i] + rv.widths[i + 1]);
					const Rect2 seg_rect = Rect2(Vector2(a.x, a.z), Vector2()).expand(Vector2(b.x, b.z)).grow(w);
					if (!seg_rect.intersects(p_rect)) {
						continue;
					}
					const Vector2 dir2 = Vector2(b.x - a.x, b.z - a.z);
					const float len = dir2.length();
					if (len < 1e-3f) {
						continue;
					}
					const Vector2 f = dir2 / len;
					const Vector2 side(-f.y, f.x);
					// Bed size: what the flow can move (Shields: tau* = 0.045 at threshold, so D = h S / (1.65 0.045);
					// depth from hydraulic geometry d = 0.27 Q^0.4, Leopold and Maddock 1953) and what still arrives
					// from upstream (Sternberg: D = D0 exp(-x / 20 km)).
					const float q = 0.5f * (rv.discharge[i] + rv.discharge[i + 1]);
					const float depth = 0.27f * Math::pow(MAX(q, 1e-3f), 0.4f);
					const float slope = CLAMP((a.y - b.y) / len, 1e-4f, 0.3f);
					const float dist = 0.5f * (rv.distance[i] + rv.distance[i + 1]);
					const float competent = depth * slope / (1.65f * 0.045f);
					const float supplied = 0.5f * Math::exp(-dist / 20000.0f);
					const float d50 = CLAMP(MIN(competent, supplied), 0.005f, 1.2f);
					if (d50 * 2.2f < min_size) {
						continue; // gravel and sand: the bed material shows it
					}
					const float round = 1.0f - Math::exp(-(dist + 300.0f) / labr);
					const int bin = round < 0.4f ? 0 : (round < 0.8f ? 1 : 2);
					const float area = len * w * 1.2f;
					const float want = 0.45f * area * float(density) / (float(Math::PI) * 0.25f * d50 * d50 * 1.4f);
					const int count = MIN(int(want), max_stones_per_cell);
					for (int k = 0; k < count; k++) {
						const float t = rng.randf();
						const float u = (rng.randf() - 0.5f) * 1.2f;
						const Vector2 p = Vector2(a.x, a.z).lerp(Vector2(b.x, b.z), t) + side * (u * w);
						if (!p_rect.has_point(p)) {
							continue;
						}
						const float d = d50 * Math::exp(rng.randfn(0.0f, 0.45f));
						if (d < min_size) {
							continue;
						}
						if (gen->get_water_depth(Vector3(p.x, 0, p.y)) > 0.3) {
							continue; // into a lake or the sea
						}
						Pending pd;
						pd.xz = Vector3(p.x, 0, p.y);
						pd.size = MIN(d, 1.5f);
						const RockGenerator::RockType type = rng.randf() < 0.7f ? rv.head_type : _type_at(gen, pd.xz);
						pd.key = _library_key(type, RockGenerator::FAMILY_COBBLE, bin);
						// Imbrication: long axis across the flow, the stone dipping upstream (15 to 30 degrees).
						pd.yaw = Math::atan2(-f.x, -f.y) + (rng.randf() - 0.5f) * 0.6f;
						pd.tilt_axis = Vector3(side.x, 0, side.y);
						pd.tilt = Math::deg_to_rad(15.0f + 15.0f * rng.randf());
						pd.sink = 0.25f + 0.1f * rng.randf();
						pd.follow = 0.6f;
						pd.film = Math::abs(u) < 0.5f ? 1.0f : 0.5f;
						pending.push_back(pd);
					}
				}
			}
		} break;
		case LAYER_SHORE: {
			// Shingle and boulders between the low water line and the storm ridge: coarser on steeper beaches (beach
			// slope rises with grain size: Bascom 1951, Bujan et al. 2019) and boulders under rock.
			// Grain size and band are taken at the corners of 2 m sub-cells and blended to each stone, so the
			// cover fades with the beach slope instead of stepping from sub-cell to sub-cell.
			const real_t sub = 2.0;
			LocalVector<Pending> shore_tries;
			HashMap<int64_t, LocalVector<Vector3>> shore_laid; // laid stones by grid cell: (x, radius, z)
			const float sgrid = 0.25f;
			float shore_max_r = 0.0f;
			struct ShorePendingGreater {
				bool operator()(const Pending &p_a, const Pending &p_b) const { return p_a.size > p_b.size; }
			};
			static const Vector2 probes[9] = { Vector2(0, 0), Vector2(4, 0), Vector2(-4, 0), Vector2(0, 4), Vector2(0, -4), Vector2(10, 0), Vector2(-10, 0), Vector2(0, 10), Vector2(0, -10) };
			const int nx = int(Math::ceil(p_rect.size.x / sub)), nz = int(Math::ceil(p_rect.size.y / sub));
			struct ShoreCorner {
				float d50 = 0.0f; // m
				float band = 0.0f; // 0..1, how far inside the shingle band
				float e = 0.0f; // height above the water line
				float rate = 0.0f; // stones above min_size per unit, before the packing factor
			};
			LocalVector<ShoreCorner> corners;
			corners.resize((nx + 1) * (nz + 1));
			for (int j = 0; j <= nz; j++) {
				for (int i = 0; i <= nx; i++) {
					ShoreCorner &sc = corners[j * (nx + 1) + i];
					const Vector3 c(p_rect.position.x + i * sub, 0, p_rect.position.y + j * sub);
					real_t level = -1e30;
					for (const Vector2 &o : probes) {
						const real_t l = gen->get_water_level(c + Vector3(o.x, 0, o.y));
						if (!Math::is_nan(l)) {
							level = MAX(level, l);
						}
					}
					if (level < -1e29) {
						continue;
					}
					sc.e = float(gen->get_height(c) - level);
					if (sc.e < -0.6f || sc.e > 2.5f) {
						continue;
					}
					const Vector3 n = ground_normal(gen, c.x, c.z, 1.0);
					const float tanb = Math::sqrt(MAX(1.0f / (n.y * n.y) - 1.0f, 0.0f));
					const float rock = float(gen->get_rock_exposure(c));
					float d50 = 0.002f * Math::exp(tanb / 0.045f);
					d50 = MAX(d50, 0.5f * Math::smoothstep(0.2f, 0.7f, rock));
					// Sorted across the beach: the swash throws the coarsest up to the storm berm, the finer stay low
					// (Bluck 1967; Carter and Orford 1993).
					d50 *= 0.75f + 0.55f * Math::smoothstep(0.2f, 2.2f, sc.e);
					sc.d50 = MIN(d50, 1.2f);
					sc.band = Math::smoothstep(-0.6f, 0.0f, sc.e) * (1.0f - Math::smoothstep(1.6f, 2.5f, sc.e));
					// Stones per square metre big enough to be geometry: the packed count of grains this size,
					// times the share of the log-normal sizes (sigma 0.35) above min_size.
					const float visible = 0.5f * std::erfc(Math::log(float(min_size) / sc.d50) / (0.35f * float(Math::SQRT2)));
					sc.rate = sc.band * visible / MAX(sc.d50 * sc.d50, 1e-4f);
				}
			}
			for (int j = 0; j < nz; j++) {
				for (int i = 0; i < nx; i++) {
					const ShoreCorner *cc[4] = { &corners[j * (nx + 1) + i], &corners[j * (nx + 1) + i + 1], &corners[(j + 1) * (nx + 1) + i], &corners[(j + 1) * (nx + 1) + i + 1] };
					float rate_max = 0.0f;
					for (const ShoreCorner *k : cc) {
						rate_max = MAX(rate_max, k->rate);
					}
					if (rate_max <= 0.0f) {
						continue; // sand, or off the shore
					}
					// Shingle is clast-supported: stones touching, sand showing only between them (some 80 to 90% cover;
					// Carter and Orford 1993). Candidates three times over at the densest corner's rate, each kept by its
					// own blended rate, then laid largest first where they do not pass through those already down.
					const float want = 3.0f * 0.85f * rate_max * float(sub * sub) * float(density) / (0.785f * 1.4f);
					const int count = MIN(int(want + rng.randf()), 1200);
					const real_t x = p_rect.position.x + i * sub, z = p_rect.position.y + j * sub;
					const RockGenerator::RockType type = _type_at(gen, Vector3(x + sub * 0.5f, 0, z + sub * 0.5f));
					for (int k = 0; k < count; k++) {
						const float fx = rng.randf(), fz = rng.randf();
						const float w[4] = { (1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz };
						float d50 = 0.0f, rate = 0.0f, e = 0.0f;
						for (int q = 0; q < 4; q++) {
							d50 += w[q] * cc[q]->d50;
							rate += w[q] * cc[q]->rate;
							e += w[q] * cc[q]->e;
						}
						if (rate <= 0.0f || d50 <= 0.0f || rng.randf() * rate_max > rate) {
							continue;
						}
						Pending pd;
						pd.xz = Vector3(x + fx * sub, 0, z + fz * sub);
						// A size from the log-normal above min_size (the smaller ones are left to the terrain's own detail).
						pd.size = 0.0f;
						for (int tries = 0; tries < 8 && pd.size < min_size; tries++) {
							pd.size = d50 * Math::exp(rng.randfn(0.0f, 0.35f));
						}
						pd.size = CLAMP(pd.size, float(min_size), 1.5f);
						// Four shape bins of pebbles (the swash's sorting by shape: discs and blades up the beach, spheres and
						// rods rolled down; Bluck 1967), each its own variants, so neighbours are rarely twins.
						// The bigger ones from the two most-worn cobble bins: rounder and less round, so no two read alike.
						pd.key = pd.size < 0.25f ? _library_key(type, RockGenerator::FAMILY_PEBBLE, int(rng.rand() % 4)) : _library_key(type, RockGenerator::FAMILY_COBBLE, 1 + int(rng.rand() % 2));
						pd.yaw = rng.randf() * Math::TAU;
						pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
						pd.tilt = Math::deg_to_rad(10.0f) * rng.randf();
						// Set into the shingle, a quarter to two fifths deep (the swash packs the gaps round them).
						pd.sink = 0.25f + 0.15f * rng.randf();
						pd.follow = 0.85f;
						pd.film = e < 0.8f ? 1.0f : CLAMP((2.5f - e) / 1.7f, 0.0f, 1.0f) * 0.5f;
						shore_tries.push_back(pd);
					}
				}
			}
			shore_tries.sort_custom<ShorePendingGreater>();
			for (const Pending &pd : shore_tries) {
				const float r = 0.5f * pd.size;
				const int64_t gx = int64_t(Math::floor(pd.xz.x / sgrid)), gz = int64_t(Math::floor(pd.xz.z / sgrid));
				const int64_t span = 1 + int64_t(Math::ceil(0.82f * (r + shore_max_r) / sgrid));
				bool clear = true;
				for (int64_t i = -span; i <= span && clear; i++) {
					for (int64_t k = -span; k <= span && clear; k++) {
						const LocalVector<Vector3> *cellv = shore_laid.getptr(((gx + i) << 32) ^ ((gz + k) & 0xffffffff));
						if (!cellv) {
							continue;
						}
						for (const Vector3 &o : *cellv) {
							const float dx = o.x - pd.xz.x, dz = o.z - pd.xz.z, m = 0.82f * (o.y + r);
							if (dx * dx + dz * dz < m * m) {
								clear = false;
								break;
							}
						}
					}
				}
				if (!clear) {
					continue;
				}
				shore_laid[(gx << 32) ^ (gz & 0xffffffff)].push_back(Vector3(pd.xz.x, r, pd.xz.z));
				shore_max_r = MAX(shore_max_r, r);
				pending.push_back(pd);
			}
		} break;
		case LAYER_CLIFF: {
			// The wall built of blocks in courses along the beds (Paris et al. 2020: blocks tiling the rock, set by its
			// fractures): every course a bed's thickness, stacked up the face, blocks end to end along it with staggered
			// joints, each standing a fifth to two fifths of its depth out of the face. A heightfield wall rises many
			// courses within one plan cell, so each cell walks the courses its face spans and finds where each one
			// crosses it along the fall line.
			const real_t sub = 3.0;
			Vector3 bed_normal(0, 1, 0);
			if (gen->is_strata_enabled()) {
				const real_t dip = Math::deg_to_rad(gen->get_strata_dip());
				const real_t dd = Math::deg_to_rad(gen->get_strata_dip_direction());
				// Dip direction clockwise from north (-z): the beds go down that way, so their normal leans the other.
				const Vector3 down_dip(Math::sin(dd), 0, -Math::cos(dd));
				bed_normal = (Vector3(0, 1, 0) * Math::cos(dip) - down_dip * Math::sin(dip)).normalized();
			}
			const float course = 1.4f; // m, course thickness
			const float min_tan = Math::tan(Math::deg_to_rad(50.0f));
			// Steeper than this a heightfield face is a stretched step, not a wall with courses in it.
			const float max_tan = Math::tan(Math::deg_to_rad(76.0f));
			const real_t gen_cell = gen->get_cell_size();
			for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + sub * 0.5f, 0, z + sub * 0.5f);
					const float rock = float(gen->get_rock_exposure(c));
					if (rock < 0.5f) {
						continue;
					}
					const real_t hc = gen->get_height(c);
					const real_t e = 1.0;
					const Vector2 grad((gen->get_height(c + Vector3(e, 0, 0)) - gen->get_height(c - Vector3(e, 0, 0))) / (2.0 * e), (gen->get_height(c + Vector3(0, 0, e)) - gen->get_height(c - Vector3(0, 0, e))) / (2.0 * e));
					const float gl = grad.length();
					if (gl < min_tan || gl > max_tan) {
						continue;
					}
					const Vector2 up_dir = grad / gl; // uphill, in plan
					// Only where the terrain mesh really has this face: across one terrain cell up and down the fall
					// line the face must stay near planar. Where the heightfield cannot hold the wall (a near-vertical
					// step between two vertices) the mesh and the bilinear height part by metres, and blocks set into
					// the one float in front of the other.
					{
						const Vector3 u(up_dir.x * gen_cell, 0, up_dir.y * gen_cell);
						const real_t bend = gen->get_height(c + u) + gen->get_height(c - u) - 2.0 * hc;
						if (Math::abs(bend) > course * 0.5f) {
							continue;
						}
					}
					// The span of heights the face covers inside this cell, along the fall line.
					const float half = float(sub) * 0.5f;
					const float y0 = float(hc) - gl * half, y1 = float(hc) + gl * half;
					const int k0 = int(Math::floor(y0 / course)), k1 = int(Math::floor(y1 / course));
					for (int k = k0; k <= k1; k++) {
						// The course's middle height, wobbling a little along the wall; where it crosses the face.
						const float yk = (k + 0.5f) * course;
						const float along = (yk - float(hc)) / gl;
						if (Math::abs(along) > half) {
							continue;
						}
						if (rng.randf() > (0.75f * rock - 0.1f) * float(density)) {
							continue; // a gap: the wall shows between blocks
						}
						const Vector2 pxz = Vector2(c.x, c.z) + up_dir * along;
						// Blocks of a course stagger against the course below: shift along the strike by half a block.
						const Vector2 strike(-up_dir.y, up_dir.x);
						const Vector2 bp = pxz + strike * ((k & 1) ? 0.9f : 0.0f) * float(sub) * 0.5f * (rng.randf() * 0.4f + 0.8f);
						if (!p_rect.has_point(bp)) {
							continue;
						}
						const Vector3 pos3(bp.x, 0, bp.y);
						const RockGenerator::RockType type = _type_at(gen, pos3 + Vector3(0, yk, 0));
						const uint32_t key = _library_key(type, RockGenerator::FAMILY_CLIFF_BLOCK, 0);
						Library *lib = _ensure_library(key);
						if (!lib) {
							continue;
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const Vector3 axes = lib->axes[st.variant];
						st.scale = course * (0.85f + 0.2f * rng.randf()) / MAX(2.0f * axes.y, 0.05f);
						// Long axis along the strike, short axis on the bed normal, b axis into the wall.
						const Vector3 y_axis = bed_normal;
						Vector3 x_axis = Vector3(strike.x, 0, strike.y);
						x_axis = (x_axis - y_axis * y_axis.dot(x_axis)).normalized();
						const Vector3 z_axis = x_axis.cross(y_axis).normalized();
						Basis b;
						b.set_column(0, x_axis);
						b.set_column(1, y_axis);
						b.set_column(2, z_axis);
						b = Basis(y_axis, (rng.randf() - 0.5f) * 0.25f) * b;
						// The face's outward normal at the crossing; the block's centre that far into the rock.
						const Vector3 n = Vector3(-grad.x, 1.0f, -grad.y).normalized();
						const Vector3 surface(bp.x, yk, bp.y);
						const Vector3 centre = surface - n * (axes.z * st.scale * (0.6f + 0.2f * rng.randf()));
						// The centre must lie inside the rock (under the terrain at its own plan position), or the block
						// stands out of a bend in the face rather than being set into it.
						if (gen->get_height(centre) < centre.y + 0.1f * course) {
							continue;
						}
						st.basis = b * Basis().scaled(Vector3(st.scale, st.scale, st.scale));
						st.position = centre;
						st.ground = yk - 50.0f; // no contact band on a wall
						st.footprint = 0.0f;
						r_stones.push_back(st);
						if (int(r_stones.size()) >= max_stones_per_cell) {
							return;
						}
					}
				}
			}
		} break;
		case LAYER_TOR: {
			// Tors and outcrops (Linton 1955; Twidale 1982): bedrock rotted along its joints under the soil, the grus
			// stripped off the crests, and the sounder joint blocks left standing in stacks, rounded in place, on the
			// highest convex ground. Granite most of all, gritstone and basalt less. One tor at most per cell, at the
			// most convex point of thin soil, plus a few low outcrops on the other crests.
			static const float tor_rate[RockGenerator::ROCK_MAX] = { 0.8f, 0.1f, 0.5f, 0.3f, 0.15f };
			const real_t sub = 16.0;
			struct Crest {
				Vector3 p;
				float score;
			};
			LocalVector<Crest> crests;
			for (real_t z = p_rect.position.y + sub * 0.5; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x + sub * 0.5; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + (rng.randf() - 0.5f) * sub * 0.6f, 0, z + (rng.randf() - 0.5f) * sub * 0.6f);
					if (gen->get_water_depth(c) > 0.0) {
						continue;
					}
					const real_t h = gen->get_height(c);
					real_t ring = 0.0;
					for (int k = 0; k < 6; k++) {
						const real_t a = real_t(k) * Math::TAU / 6.0;
						ring += gen->get_height(c + Vector3(Math::cos(a) * 15.0, 0, Math::sin(a) * 15.0));
					}
					const float convex = float(h - ring / 6.0);
					const Vector3 n = ground_normal(gen, c.x, c.z, 3.0);
					if (convex < 0.6f || n.y < 0.85f) {
						continue; // not a crest, or too steep (the grus would not be stripped evenly)
					}
					const float exposure = float(gen->get_rock_exposure(c));
					const float thin = 1.0f - Math::smoothstep(0.3f, 1.5f, float(gen->get_soil_depth(c)));
					crests.push_back({ c, convex * (0.4f + 0.6f * MAX(exposure, thin)) });
				}
			}
			if (crests.is_empty()) {
				break;
			}
			struct ByScore {
				_FORCE_INLINE_ bool operator()(const Crest &p_a, const Crest &p_b) const { return p_a.score > p_b.score; }
			};
			crests.sort_custom<ByScore>();
			// A stack of corestones: two to five tiers between sheeting joints, the blocks of a tier side by side,
			// tiers thinner and narrower upwards, each shifted a little on the one below.
			auto build_stack = [&](const Vector3 &p_at, RockGenerator::RockType p_type, uint32_t p_family_key, float p_width, int p_tiers, float p_shrink, real_t p_base, bool p_sea) {
				Library *lib = _ensure_library(p_family_key);
				if (!lib) {
					return real_t(0.0);
				}
				const real_t yaw = rng.randf() * Math::TAU;
				const Vector3 along(Math::cos(yaw), 0, Math::sin(yaw));
				real_t y = p_base;
				Vector3 centre = p_at;
				for (int t = 0; t < p_tiers; t++) {
					const float w = p_width * Math::pow(p_shrink, float(t));
					const int count = t == 0 ? 1 + int(rng.randf() * 2.4f) : (t < p_tiers - 1 && rng.randf() < 0.35f ? 2 : 1);
					real_t tier_top = y;
					for (int k = 0; k < count; k++) {
						Stone st;
						st.layer = p_layer;
						st.library = p_family_key;
						st.variant = pick_variant(lib);
						const Vector3 ax = lib->axes[st.variant];
						st.scale = w / float(count) * (0.85f + 0.3f * rng.randf()) * 0.5f;
						const real_t offset = (real_t(k) - (count - 1) * 0.5) * ax.x * st.scale * 1.9;
						const Vector3 xz = centre + along * offset;
						Basis b(Vector3(0, 1, 0), yaw + (rng.randf() - 0.5f) * 0.25f);
						b = Basis(Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f).normalized(), Math::deg_to_rad(4.0f) * (rng.randf() - 0.5f)) * b;
						st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
						const real_t bottom = -lib->bottom[st.variant] * st.scale;
						real_t base_y = y;
						if (t == 0 && !p_sea) {
							// A base block stands on the ground under its own footprint, not the crest's middle: on a
							// narrow crest a block set beside the top hung out over the falling slope. It sinks to the
							// lowest ground under its ends and sides (set a little into it, as the middle one is).
							const Vector3 side_dir(-along.z, 0, along.x);
							const real_t ha = ax.x * st.scale * 0.8, hs = ax.z * st.scale * 0.8;
							real_t gmin = gen->get_height(xz);
							gmin = MIN(gmin, gen->get_height(xz + along * ha));
							gmin = MIN(gmin, gen->get_height(xz - along * ha));
							gmin = MIN(gmin, gen->get_height(xz + side_dir * hs));
							gmin = MIN(gmin, gen->get_height(xz - side_dir * hs));
							base_y = MIN(y, gmin - 0.1 * ax.y * st.scale);
						}
						st.position = Vector3(xz.x, base_y + bottom, xz.z);
						st.ground = p_sea ? y - 50.0 : gen->get_height(xz);
						st.footprint = t == 0 && !p_sea ? MAX(ax.x, ax.z) * st.scale * 0.85f : 0.0f;
						tier_top = MAX(tier_top, st.position.y + ax.y * st.scale);
						r_stones.push_back(st);
					}
					// The next tier sits on this one, settled into its rounded top (the rounded edges leave the
					// joint open as a groove, not a gap).
					y = tier_top - 0.16 * (tier_top - y);
					centre += Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f) * w * 0.3f;
				}
				return y;
			};
			int outcrops = 0;
			for (uint32_t i = 0; i < crests.size() && outcrops < 4; i++) {
				const Crest &cr = crests[i];
				if (!p_rect.has_point(Vector2(cr.p.x, cr.p.z))) {
					continue;
				}
				const RockGenerator::RockType type = _type_at(gen, cr.p);
				const uint32_t key = _library_key(type, RockGenerator::FAMILY_CORESTONE, 0);
				const real_t ground = gen->get_height(cr.p);
				// A tor needs a summit broad enough for its base (a knife ridge sheds its blocks): the ground a few metres
				// out from the top falls no more than a third of the way down.
				real_t fall = 0.0;
				for (int k = 0; k < 6; k++) {
					const real_t a = real_t(k) * Math::TAU / 6.0;
					fall = MAX(fall, ground - gen->get_height(cr.p + Vector3(Math::cos(a) * 4.0, 0, Math::sin(a) * 4.0)));
				}
				if (i == 0 && rng.randf() < tor_rate[type] * float(density) * Math::smoothstep(0.6f, 3.0f, cr.score) && fall < 1.4) {
					// The tor: base blocks 3 to 7 m, set a quarter of their thickness into the bedrock below.
					const float width = 3.0f + 4.0f * rng.randf();
					const uint32_t first = r_stones.size();
					build_stack(cr.p, type, key, width, 2 + int(rng.randf() * 3.99f), 0.82f, ground - width * 0.12f, false);
					if (first < r_stones.size()) {
						const Stone base = r_stones[first]; // a copy: the skirt adds to r_stones
						skirt(base, type, 1.0f);
					}
				} else if (rng.randf() < 0.5f * tor_rate[type] * float(density)) {
					// A low outcrop: one tier of rounded joint blocks breaking the turf.
					const float width = 1.5f + 2.5f * rng.randf();
					build_stack(cr.p, type, key, width, 1, 1.0f, ground - width * 0.2f, false);
					outcrops++;
				}
			}
		} break;
		case LAYER_COAST: {
			// A cliffed coast retreats by undercutting at the waterline: the notch, then the cliff falls, and harder
			// joint-bounded remnants stand off it as sea stacks, arches where a headland's cave cut right through,
			// and the wave-cut platform the cliff stood on, a rock shelf near low water (Trenhaile 1987; Sunamura
			// 1992). Stacks and arches here are built of large wall blocks stacked tier on tier from the bed; the
			// platform of flat-topped blocks just above the water line at the cliff foot.
			const real_t sub = 16.0;
			const real_t level_any = gen->get_sea_level();
			struct Spot {
				Vector3 p;
				Vector3 to_land;
				float score;
			};
			LocalVector<Spot> spots;
			for (real_t z = p_rect.position.y + sub * 0.5; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x + sub * 0.5; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x, 0, z);
					const real_t depth = gen->get_water_depth(c);
					if (depth < 0.8 || depth > 12.0) {
						continue;
					}
					const real_t level = gen->get_water_level(c);
					if (Math::is_nan(level) || Math::abs(level - level_any) > 0.5) {
						continue; // the sea only, not lakes and rivers
					}
					// A cliff within 50 m: land rising 8 m or more above the water.
					Vector3 best_dir;
					float best_rise = 0.0f;
					for (int k = 0; k < 8; k++) {
						const real_t a = real_t(k) * Math::TAU / 8.0;
						const Vector3 d(Math::cos(a), 0, Math::sin(a));
						for (real_t r = 15.0; r <= 50.0; r += 17.5) {
							const float rise = float(gen->get_height(c + d * r) - level);
							if (rise > best_rise) {
								best_rise = rise;
								best_dir = d;
							}
						}
					}
					if (best_rise < 8.0f) {
						continue;
					}
					const float rock = float(gen->get_rock_exposure(c + best_dir * 30.0));
					spots.push_back({ c, best_dir, MIN(best_rise, 40.0f) * (0.3f + 0.7f * rock) / float(1.0 + depth * 0.1) });
				}
			}
			if (spots.is_empty()) {
				break;
			}
			struct ByScore {
				_FORCE_INLINE_ bool operator()(const Spot &p_a, const Spot &p_b) const { return p_a.score > p_b.score; }
			};
			spots.sort_custom<ByScore>();
			const Spot &sp = spots[0];
			const RockGenerator::RockType type = _type_at(gen, sp.p + sp.to_land * 30.0);
			// The stack's fallen blocks: fresh fragments of all forms (slabs, wedges, rods), not wall blocks.
			const uint32_t key = _library_key(type, RockGenerator::FAMILY_ROCKFALL, 0);
			Library *lib = _ensure_library(key);
			if (!lib) {
				break;
			}
			const real_t level = gen->get_water_level(sp.p);
			if (!p_rect.has_point(Vector2(sp.p.x, sp.p.z)) || rng.randf() > 0.6f * float(density)) {
				break;
			}
			const real_t yaw = Math::atan2(sp.to_land.z, sp.to_land.x);
			const Vector3 strike(-sp.to_land.z, 0, sp.to_land.x);
			// Each form is one shape of its own family (RockGenerator FAMILY_SEA_*): one mass of bedrock, not a pile.
			const real_t bed = gen->get_height(sp.p);
			if (rng.randf() < 0.3f) {
				// An arch, 14 to 26 m along the coast, its opening facing the sea, its feet sunk in the bed.
				const uint32_t ak = _library_key(type, RockGenerator::FAMILY_SEA_ARCH, 0);
				if (Library *al = _ensure_library(ak)) {
					Stone st;
					st.layer = p_layer;
					st.library = ak;
					st.variant = pick_variant(al);
					const Vector3 ax = al->axes[st.variant];
					st.scale = (14.0f + 12.0f * rng.randf()) / MAX(2.0f * ax.x, 0.5f);
					// x along the strike, z (through the opening) to the land.
					Basis b(Vector3(strike.x, 0, strike.z), Vector3(0, 1, 0), Vector3(-sp.to_land.x, 0, -sp.to_land.z)); // right-handed
					st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
					st.position = Vector3(sp.p.x, bed - 0.6 - al->bottom[st.variant] * st.scale, sp.p.z);
					st.ground = bed - 50.0;
					r_stones.push_back(st);
				}
			} else {
				// A stack: the notch (about a fifth up the column) at the waterline, so 15 to 30 m tall and at least
				// tall enough to stand on the bed with its notch at the sea; its fallen blocks at its foot.
				const uint32_t sk = _library_key(type, RockGenerator::FAMILY_SEA_STACK, 0);
				if (Library *sl = _ensure_library(sk)) {
					Stone st;
					st.layer = p_layer;
					st.library = sk;
					st.variant = pick_variant(sl);
					const Vector3 ax = sl->axes[st.variant];
					const AABB box = sl->meshes[st.variant]->get_aabb(); // its x: the column's length, foot to top
					const real_t depth = level - bed;
					const real_t height = MAX(real_t(15.0 + 15.0 * rng.randf() * rng.randf()), (depth + 0.8) / 0.22);
					st.scale = height / MAX(real_t(box.size.x), real_t(0.5));
					// Stood on end: the column's x up, its notch (a fifth up from its foot) at the sea.
					Basis b = Basis(Vector3(0, 1, 0), yaw + rng.randf() * Math::TAU) * Basis(Vector3(0, 0, 1), Math::PI * 0.5);
					st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
					st.position = Vector3(sp.p.x, level - 0.22 * height - box.position.x * st.scale, sp.p.z);
					st.ground = bed - 50.0;
					r_stones.push_back(st);
					const float w = 2.0f * ax.z * st.scale;
					for (int k = 0; k < 3; k++) {
						Stone fb;
						fb.layer = p_layer;
						fb.library = key;
						fb.variant = pick_variant(lib);
						fb.scale = w * (0.06f + 0.12f * rng.randf() * rng.randf());
						const Vector3 at = sp.p + Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f).normalized() * w * (0.8f + 0.5f * rng.randf());
						_rest(gen, fb, *lib, at, rng.randf() * Math::TAU, 0.2, Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f), Math::deg_to_rad(20.0f) * rng.randf(), 0.6);
						r_stones.push_back(fb);
					}
				}
			}
			// The wave-cut platform: one bench of bedrock cut flush at the cliff foot, its top a little above low
			// water, reaching seaward; the cliff's own rock, joints and pools carved in it. Found by walking from the
			// form toward the land until the ground rises out of the sea: the platform's landward edge sinks into it.
			const uint32_t pk = _library_key(type, RockGenerator::FAMILY_SEA_PLATFORM, 0);
			if (Library *pl = _ensure_library(pk)) {
				Vector3 foot = sp.p;
				for (real_t r = 0.0; r < 60.0; r += 1.0) {
					foot = sp.p + sp.to_land * r;
					if (gen->get_height(foot) > level + 1.0) {
						break;
					}
				}
				Stone st;
				st.layer = p_layer;
				st.library = pk;
				st.variant = pick_variant(pl);
				const Vector3 ax = pl->axes[st.variant];
				st.scale = (28.0f + 14.0f * rng.randf()) / MAX(2.0f * ax.x, 0.5f);
				Basis b(Vector3(strike.x, 0, strike.z), Vector3(0, 1, 0), Vector3(-sp.to_land.x, 0, -sp.to_land.z)); // right-handed
				st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
				const Vector3 at = foot - sp.to_land * (ax.z * st.scale * 0.75f);
				// Where a beach stands over the platform's level the sand buries it (a platform shows only where the waves
				// keep it bare): a slab there only pokes through the sand in jagged edges, so none is laid.
				const real_t over = MAX(gen->get_height(at), gen->get_height(foot - sp.to_land * 3.0)) - (level + 0.6);
				st.position = Vector3(at.x, level + 0.6 - ax.y * st.scale, at.z); // above mean sea level: bare at low tide
				st.ground = gen->get_height(at);
				st.film = 0.6f;
				if (over <= 0.2) {
					r_stones.push_back(st);
				}
			}
		} break;
		case LAYER_LANDFORM: {
			// Bedrock landforms where their rock and setting are (docs/ideas/rocks.md, landforms):
			// - limestone pavement on flat, bare limestone (ice-scoured, thin soil): slabs flush with the ground;
			// - hoodoos on steep, bare, soft bedded rock (badland slopes, amphitheatre heads): a row of columns
			//   along one joint set, a few metres apart.
			const real_t sub = 12.0;
			struct Site {
				Vector3 p;
				float score;
				int kind; // 0 pavement, 1 hoodoos, 2 fins, 3 goblins, 4 cave mouths
			};
			LocalVector<Site> sites;
			const real_t shore_top = gen->has_sea_result() ? gen->get_sea_level() + 6.0 : -1e9; // beaches and storm ridges are sand and shingle
			for (real_t z = p_rect.position.y + sub * 0.5; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x + sub * 0.5; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + (rng.randf() - 0.5f) * sub * 0.6f, 0, z + (rng.randf() - 0.5f) * sub * 0.6f);
					if (gen->get_water_depth(c) > 0.0 || gen->get_height(c) < shore_top) {
						continue;
					}
					const RockGenerator::RockType type = _type_at(gen, Vector3(c.x, gen->get_height(c), c.z));
					const Vector3 n = ground_normal(gen, c.x, c.z, 4.0);
					const float exposure = float(gen->get_rock_exposure(c));
					const float thin = 1.0f - Math::smoothstep(0.2f, 0.8f, float(gen->get_soil_depth(c)));
					const float bare = MAX(exposure, thin);
					if (type == RockGenerator::ROCK_LIMESTONE && n.y > 0.94f && bare > 0.35f) { // up to 20 degrees: stepped pavements (Malham, the Burren) follow gentle slopes
						// A bench, not a knoll or a col: the ground within 8 m stays on the plane through the centre.
						const real_t h0 = gen->get_height(c);
						const real_t sx = -n.x / MAX(n.y, real_t(0.1)), sz = -n.z / MAX(n.y, real_t(0.1));
						float off = 0.0f;
						for (int j = 0; j < 8; j++) {
							const Vector3 d = Vector3(Math::cos(j * Math::TAU / 8.0), 0, Math::sin(j * Math::TAU / 8.0)) * 8.0;
							off = MAX(off, float(Math::abs(gen->get_height(c + d) - (h0 + sx * d.x + sz * d.z))));
						}
						if (off < 0.8f) {
							sites.push_back({ c, bare * (1.0f + rng.randf() * 0.3f), 0 });
						}
					}
					// Cave mouths: bare limestone cliff (a wall the terrain holds, 50 to 76 degrees) whose foot, 10 m down
					// the fall line, flattens out: the passage opens at the foot.
					if (cave_mouths && type == RockGenerator::ROCK_LIMESTONE && bare > 0.5f && n.y < 0.64f && n.y > 0.23f) {
						const Vector2 down = Vector2(n.x, n.z).normalized(); // a ground normal leans downhill
						const Vector3 foot = c + Vector3(down.x, 0, down.y) * 10.0;
						// A real foot: level ground over 8 m in front, and the wall behind rising at least 8 m within 12 m.
						const real_t hf = gen->get_height(foot);
						const bool wall = gen->get_height(foot - Vector3(down.x, 0, down.y) * 12.0) > hf + 8.0;
						if (wall && ground_normal(gen, foot.x + down.x * 4.0, foot.z + down.y * 4.0, 8.0).y > 0.93f && gen->get_water_depth(foot) <= 0.0) {
							sites.push_back({ foot, bare * (0.7f + rng.randf() * 0.3f), 4 });
						}
					}
					// Goblins: flat, bare sandstone valley floors (Goblin Valley: Entrada between mesa walls).
					if (type == RockGenerator::ROCK_SANDSTONE && n.y > 0.97f && bare > 0.5f) {
						sites.push_back({ c, bare * (0.8f + rng.randf() * 0.3f), 3 });
					}
					// Fins: bare sandstone on gentle ground (Arches: under 15 degrees), the joints opened by weathering.
					static const float fin_rate[RockGenerator::ROCK_MAX] = { 0.0f, 0.1f, 1.0f, 0.0f, 0.0f };
					if (fin_rate[type] > 0.0f && n.y > 0.95f && bare > 0.5f) {
						sites.push_back({ c, bare * fin_rate[type] * (0.9f + rng.randf() * 0.3f), 2 });
					}
					static const float hoodoo_rate[RockGenerator::ROCK_MAX] = { 0.0f, 0.25f, 1.0f, 0.0f, 0.15f };
					if (hoodoo_rate[type] > 0.0f && n.y > 0.7f && n.y < 0.94f && bare > 0.5f) {
						sites.push_back({ c, bare * hoodoo_rate[type] * (1.0f + rng.randf() * 0.3f), 1 });
					}
				}
			}
			struct ByScore {
				_FORCE_INLINE_ bool operator()(const Site &p_a, const Site &p_b) const { return p_a.score > p_b.score; }
			};
			sites.sort_custom<ByScore>();
			// Potholes: river reaches steep enough to scour to bedrock (bed shear past what the supply can armour), one slab
			// of drilled bedrock down the thalweg every 50 to 90 m, its top flush with the bed, a grinder stone left in
			// some of its holes.
			_prepare_rivers();
			for (const River &rv : rivers) {
				if (!rv.bounds.grow(20.0).intersects(p_rect)) {
					continue;
				}
				real_t run = rng.randf() * 60.0;
				for (uint32_t i = 0; i + 1 < rv.points.size(); i++) {
					const Vector3 pa = rv.points[i], pb = rv.points[i + 1];
					const Vector2 d2(pb.x - pa.x, pb.z - pa.z);
					const real_t len = d2.length();
					if (len < 1e-3) {
						continue;
					}
					run += len;
					const float slope = float((pa.y - pb.y) / len);
					const float w = 0.5f * (rv.widths[i] + rv.widths[i + 1]);
					// Steep, not a fall; a channel wide enough to carry grinders (the width stands in for the flow).
					if (run < 50.0 || slope < 0.03f || slope > 0.4f || w < 0.6f) {
						continue;
					}
					const Vector2 at2 = Vector2(pa.x, pa.z).lerp(Vector2(pb.x, pb.z), rng.randf());
					if (!p_rect.has_point(at2)) {
						continue;
					}
					const Vector3 at(at2.x, 0, at2.y);
					if (gen->get_height(at) < shore_top || gen->get_water_depth(at) > 0.5) {
						continue; // the delta and the lake: sediment, not a scoured bed
					}
					if (gen->get_rock_exposure(at) < 0.3 && slope < 0.08f) {
						continue; // a gravel bed: the bedrock is under it
					}
					run = -40.0 * rng.randf();
					const RockGenerator::RockType type = _type_at(gen, Vector3(at.x, gen->get_height(at), at.z));
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_POTHOLE, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					Stone st;
					st.layer = p_layer;
					st.library = key;
					st.variant = pick_variant(lib);
					const AABB box = lib->meshes[st.variant]->get_aabb();
					st.scale = CLAMP(real_t(w) * 3.0, 3.0, 9.0) / MAX(real_t(box.size.z), real_t(0.5)); // across the bed and into its banks
					const Vector3 fx = Vector3(d2.x, 0, d2.y) / len;
					const Vector3 n = ground_normal(gen, at.x, at.z, 3.0);
					const Vector3 fz = fx.cross(n).normalized();
					const Vector3 fxx = n.cross(fz).normalized();
					st.basis = Basis(fxx, n, fz).scaled_local(Vector3(st.scale, st.scale, st.scale));
					// A bed wide and level enough across to hold the slab flush: a V-shaped gully (or a terrain grid too
					// coarse to carve the channel) would bury its sides or leave it standing proud.
					const real_t g = gen->get_height(at);
					const real_t half_l = real_t(box.size.x) * st.scale * 0.4, half_w = real_t(box.size.z) * st.scale * 0.4;
					real_t worst = 0.0, above = 0.0;
					for (int ia = -1; ia <= 1; ia++) {
						for (int ic = -1; ic <= 1; ic++) {
							const Vector3 o = fxx * (real_t(ia) * half_l) + fz * (real_t(ic) * half_w);
							const real_t r = gen->get_height(at + Vector3(o.x, 0, o.z)) - (g + o.y);
							worst = MAX(worst, Math::abs(r));
							above = MAX(above, r);
						}
					}
					if (worst > 0.5 || n.y < 0.92) {
						continue; // not on a channel wall either
					}
					const real_t top = g + above; // never under the ground: where the bed falls away it stands as a low step
					st.position = Vector3(at.x, top + 0.03 - (box.position.y + box.size.y) * st.scale, at.z);
					st.ground = g - 0.5; // it is the bed: the ground-blend band below its top, at the buried edges
					r_stones.push_back(st);
					// Grinders: a rounded cobble on the floor of most holes, a little smaller than the hole.
					const PackedFloat32Array holes = lib->potholes[st.variant]; // a copy: the cobble library may be added below
					const uint32_t ckey = _library_key(type, RockGenerator::FAMILY_COBBLE, 2);
					Library *clib = holes.is_empty() ? nullptr : _ensure_library(ckey);
					for (int h = 0; clib && h + 3 < holes.size(); h += 4) {
						if (rng.randf() > 0.6f) {
							continue;
						}
						const Vector3 hole_at = st.position + st.basis.xform(Vector3(holes[h], holes[h + 1], holes[h + 2]));
						const real_t r = real_t(holes[h + 3]) * st.scale;
						Stone gs;
						gs.layer = p_layer;
						gs.library = ckey;
						gs.variant = pick_variant(clib);
						const Vector3 cax = clib->axes[gs.variant];
						gs.scale = r * (0.35 + 0.25 * rng.randf()) / MAX(real_t(cax.z), real_t(0.3));
						gs.basis = Basis(st.basis.orthonormalized()) * Basis(Vector3(0, 1, 0), rng.randf() * Math::TAU);
						gs.basis = gs.basis.scaled_local(Vector3(gs.scale, gs.scale, gs.scale));
						gs.position = hole_at + n * real_t(-clib->bottom[gs.variant] * gs.scale);
						gs.ground = float(hole_at.y);
						r_stones.push_back(gs);
					}
				}
			}
			LocalVector<Vector3> used;
			int made = 0;
			// Hoodoos gather in badland heads and amphitheatres, not on every steep soft slope: one cell in four has them.
			const bool badland = rng.randf() < 0.25f;
			const bool fin_field = rng.randf() < 0.35f; // fins come in fields along one joint set
			const bool goblin_field = rng.randf() < 0.3f;
			bool cave_made = rng.randf() > 0.25f; // a cave mouth in one cell in four that has a limestone cliff foot
			for (const Site &s : sites) {
				if ((s.kind == 1 && !badland) || (s.kind == 2 && !fin_field) || (s.kind == 3 && !goblin_field)) {
					continue;
				}
				if (made >= 2 || rng.randf() > (s.kind == 0 ? 0.8f : (s.kind == 4 ? 1.0f : 0.3f)) * float(density)) { // flat bare benches are rare already
					continue;
				}
				bool near = false;
				for (const Vector3 &u : used) {
					near = near || Vector2(u.x - s.p.x, u.z - s.p.z).length() < 60.0f;
				}
				if (near) {
					continue;
				}
				used.push_back(s.p);
				made++;
				const RockGenerator::RockType type = _type_at(gen, Vector3(s.p.x, gen->get_height(s.p), s.p.z));
				if (s.kind == 4) {
					if (cave_made) {
						continue;
					}
					cave_made = true;
					// The portal block is set into the slope: its front stands at the foot and its back and top are
					// buried in the wall as it rises, the opening facing out. The terrain inside the passage is cut away (TerrainGenerator3D.add_hole; the
					// block's rock hides the hole's edge, as a static mesh cave entrance does over a landscape hole);
					// a few fallen blocks lie on the floor at the entrance.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_CAVE_MOUTH, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					Vector3 up_n = ground_normal(gen, s.p.x, s.p.z, 12.0);
					Vector2 o2 = Vector2(up_n.x, up_n.z);
					if (o2.length() < 0.05f) {
						continue;
					}
					o2.normalize(); // out, away from the cliff
					const Vector3 out(o2.x, 0, o2.y), up(0, 1, 0);
					const Vector3 strike(-o2.y, 0, o2.x); // so that strike x out = up
					// Where the wall rises from the foot: the first point back toward it 1 m above the foot.
					real_t g = gen->get_height(s.p);
					real_t back = -1.0;
					for (real_t t = 0.0; t <= 14.0; t += 0.5) {
						if (gen->get_height(s.p - out * t) > g + 1.0) {
							back = MAX(t - 1.0, real_t(0.0));
							break;
						}
					}
					if (back < 0.0) {
						continue;
					}
					const Vector3 base = s.p - out * back;
					for (int j = -2; j <= 2; j++) {
						g = MIN(g, gen->get_height(base + strike * real_t(j) * 1.5));
					}
					// The wall's own normal a few metres up; the block leans back with it (up to 25 degrees), so its front
					// lies along the face and its sides and top go into the hill, the passage running down into it.
					Vector3 wall_n = ground_normal(gen, base.x - out.x * 5.0, base.z - out.z * 5.0, 5.0);
					const real_t lean = MIN(Math::acos(CLAMP(Vector2(wall_n.x, wall_n.z).length(), real_t(0.0), real_t(1.0))), Math::deg_to_rad(real_t(25.0)));
					const Vector3 out_t = (out * Math::cos(lean) + up * Math::sin(lean)).normalized();
					const Vector3 up_t = strike.cross(out_t).normalized();
					Stone st;
					st.layer = p_layer;
					st.library = key;
					st.variant = pick_variant(lib);
					const AABB box = lib->meshes[st.variant]->get_aabb();
					const real_t height = 6.0 + 6.0 * rng.randf();
					st.scale = height / MAX(real_t(box.size.z), real_t(0.5));
					st.basis = Basis(strike, out_t, up_t).scaled_local(Vector3(st.scale, st.scale, st.scale));
					// The passage's floor at its mouth sits on the foot where the wall rises, the front 0.3 m out of it.
					const Transform3D cave = (st.variant < int(lib->caves.size())) ? lib->caves[st.variant] : Transform3D(Basis().scaled(Vector3()), Vector3());
					const bool has_cave = cave.basis.determinant() != 0.0f;
					const Vector3 mouth_local = has_cave ? cave.xform(Vector3(0, 1, -1)) : Vector3(0.5f * (box.position.x + box.get_end().x), box.get_end().y, box.position.z);
					// Far enough out that no ground stands in front of the opening: across its width and height the
					// wall must lie behind the front (the hole takes the ground inside the passage, not in front of it).
					Vector3 mouth = Vector3(base.x, g, base.z) + out * 0.3;
					if (has_cave) {
						const Basis hb = st.basis * cave.basis;
						const real_t hw = hb.get_column(0).length(), hh = 2.0 * hb.get_column(2).length();
						real_t push = 0.0;
						for (int iu = -2; iu <= 2; iu++) {
							for (int iv = 1; iv <= 4; iv++) {
								const Vector3 p = mouth + strike * (hw * real_t(iu) * 0.45) + up_t * (hh * real_t(iv) * 0.24);
								real_t t = 0.0;
								while (t < 12.0 && gen->get_height(p + out * t) > p.y) {
									t += 0.25;
								}
								push = MAX(push, t);
							}
						}
						mouth += out * push;
					}
					st.position = mouth - st.basis.xform(mouth_local) - up * (0.02 * height);
					st.ground = g;
					r_stones.push_back(st);
					if (has_cave) {
						const Transform3D hb = Transform3D(st.basis, st.position) * cave;
						if (gen->add_hole(hb)) {
							hole_rects.push_back(Rect2(hb.origin.x, hb.origin.z, 0, 0).grow(hb.basis.get_scale().length()));
							_terrain3d_hole(hb);
						}
					}
					const Vector3 front = mouth;
					// Breakdown: 3 to 7 blocks fallen from the roof and the hood, on the floor in front of the entrance.
					const uint32_t bkey = _library_key(type, RockGenerator::FAMILY_ROCKFALL, 0);
					Library *blib = _ensure_library(bkey);
					const int falls = blib ? 3 + int(rng.randf() * 4.99f) : 0;
					for (int k = 0; k < falls; k++) {
						const Vector3 at = front + out * real_t(0.5 + 4.0 * rng.randf()) + strike * real_t((rng.randf() - 0.5f) * 0.6f * float(box.size.x) * float(st.scale));
						Stone bs;
						bs.layer = p_layer;
						bs.library = bkey;
						bs.variant = pick_variant(blib);
						const real_t size = 0.5 + 1.2 * rng.randf() * rng.randf();
						bs.scale = size * 0.5;
						bs.basis = Basis(up, rng.randf() * Math::TAU).scaled(Vector3(bs.scale, bs.scale, bs.scale));
						const real_t gb = gen->get_height(at);
						bs.position = Vector3(at.x, gb - blib->bottom[bs.variant] * bs.scale - 0.12 * size, at.z);
						bs.ground = gb;
						r_stones.push_back(bs);
					}
					continue;
				}
				if (s.kind == 3) {
					// Goblins: 6 to 20 knobs scattered within 25 m, 1.5 to 5 m tall, the foot set into the floor.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_GOBLIN, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					const int count = 6 + int(rng.randf() * 14.99f);
					for (int k = 0; k < count; k++) {
						const real_t ang = rng.randf() * Math::TAU, rad = 25.0 * Math::sqrt(rng.randf());
						const Vector3 at = s.p + Vector3(Math::cos(ang), 0, Math::sin(ang)) * rad;
						if (gen->get_water_depth(at) > 0.0 || ground_normal(gen, at.x, at.z, 3.0).y < 0.9) {
							continue; // on the floor, not up the slopes round it
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const AABB box = lib->meshes[st.variant]->get_aabb();
						const real_t height = 1.5 + 3.5 * rng.randf() * rng.randf();
						st.scale = height / MAX(real_t(box.size.x), real_t(0.3));
						st.basis = (Basis(Vector3(0, 1, 0), rng.randf() * Math::TAU) * Basis(Vector3(0, 0, 1), Math::PI * 0.5)).scaled(Vector3(st.scale, st.scale, st.scale));
						const real_t r = real_t(box.size.y) * st.scale * 0.5;
						real_t g = gen->get_height(at);
						for (int j = 0; j < 6; j++) {
							g = MIN(g, gen->get_height(at + Vector3(Math::cos(j * Math::TAU / 6.0), 0, Math::sin(j * Math::TAU / 6.0)) * r));
						}
						st.position = Vector3(at.x, g - 0.06 * height - box.position.x * st.scale, at.z);
						st.ground = gen->get_height(at);
						r_stones.push_back(st);
					}
					continue;
				}
				if (s.kind == 2) {
					// Fins: 3 to 7 parallel walls along one joint set, 6 to 14 m apart, each 12 to 35 m long, standing on
					// the lowest ground along its foot.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_FIN, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					const real_t yaw = rng.randf() * Math::TAU;
					const Vector3 along(Math::cos(yaw), 0, Math::sin(yaw));
					const Vector3 up(0, 1, 0);
					const Vector3 across = up.cross(along);
					const int count = 3 + int(rng.randf() * 4.99f);
					real_t off = -real_t(count) * 5.0;
					for (int k = 0; k < count; k++) {
						off += 6.0 + 8.0 * rng.randf();
						const Vector3 at = s.p + across * off + along * real_t((rng.randf() - 0.5f) * 12.0f);
						if (gen->get_water_depth(at) > 0.0) {
							continue;
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const AABB box = lib->meshes[st.variant]->get_aabb();
						const real_t length = 12.0 + 23.0 * rng.randf() * rng.randf();
						st.scale = length / MAX(real_t(box.size.x), real_t(0.5));
						const real_t jy = (rng.randf() - 0.5f) * 0.15f; // the joints wander a little from fin to fin
						const Vector3 al = Basis(up, jy).xform(along), ac = Basis(up, jy).xform(across);
						st.basis = Basis(al, ac, up).scaled_local(Vector3(st.scale, st.scale, st.scale));
						real_t g = gen->get_height(at);
						for (int j = -2; j <= 2; j++) {
							for (int sd = -1; sd <= 1; sd += 2) {
								g = MIN(g, gen->get_height(at + al * (real_t(j) * 0.22 * length) + ac * (real_t(sd) * real_t(box.size.y) * st.scale * 0.5)));
							}
						}
						const real_t height = real_t(box.size.z) * st.scale;
						st.position = Vector3(at.x, g - 0.04 * height - box.position.z * st.scale, at.z);
						st.ground = gen->get_height(at);
						r_stones.push_back(st);
					}
					continue;
				}
				if (s.kind == 0) {
					// Pavement: a patch 8 to 16 m across, its top a little proud of the soil round it, its normal the
					// ground's, turned to a random joint direction.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_PAVEMENT, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					Stone st;
					st.layer = p_layer;
					st.library = key;
					st.variant = pick_variant(lib);
					const Vector3 ax = lib->axes[st.variant];
					st.scale = (8.0f + 8.0f * rng.randf()) / MAX(2.0f * ax.x, 0.5f); // within the flat 8 m checked round the site
					const Vector3 n = ground_normal(gen, s.p.x, s.p.z, real_t(ax.x * st.scale));
					Basis b(Vector3(0, 1, 0), rng.randf() * Math::TAU);
					const Vector3 axis = Vector3(0, 1, 0).cross(n);
					if (axis.length_squared() > 1e-8) {
						b = Basis(axis.normalized(), Math::acos(CLAMP(n.y, real_t(-1.0), real_t(1.0)))) * b;
					}
					st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
					const real_t g = gen->get_height(s.p);
					st.position = Vector3(s.p.x, g + 0.15 - ax.y * st.scale, s.p.z);
					st.ground = g - 0.4; // bedrock at the surface: the ground-blend band only at its buried edges
					r_stones.push_back(st);
				} else {
					// Hoodoos: 3 to 9 columns in a row along a joint, 3 to 8 m apart, 4 to 20 m tall, the foot sunk in
					// the debris apron.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_HOODOO, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					const real_t yaw = rng.randf() * Math::TAU;
					const Vector3 along(Math::cos(yaw), 0, Math::sin(yaw));
					const int count = 3 + int(rng.randf() * 6.99f);
					Vector3 at = s.p - along * real_t(count) * 2.5;
					for (int k = 0; k < count; k++) {
						at += along * real_t(3.0 + 5.0 * rng.randf()) + Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f) * 2.0f;
						if (gen->get_water_depth(at) > 0.0) {
							continue;
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const AABB box = lib->meshes[st.variant]->get_aabb();
						const real_t height = 4.0 + 16.0 * rng.randf() * rng.randf();
						st.scale = height / MAX(real_t(box.size.x), real_t(0.5));
						const Basis b = Basis(Vector3(0, 1, 0), yaw + rng.randf() * Math::TAU) * Basis(Vector3(0, 0, 1), Math::PI * 0.5);
						st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
						// The lowest ground under its foot, so no side hangs over the slope.
						const real_t r = real_t(box.size.y) * st.scale * 0.5;
						real_t g = gen->get_height(at);
						for (int j = 0; j < 6; j++) {
							g = MIN(g, gen->get_height(at + Vector3(Math::cos(j * Math::TAU / 6.0), 0, Math::sin(j * Math::TAU / 6.0)) * r));
						}
						st.position = Vector3(at.x, g - 0.12 * height - box.position.x * st.scale, at.z);
						st.ground = gen->get_height(at);
						r_stones.push_back(st);
					}
				}
			}
		} break;
		case LAYER_DETAIL_PEBBLE:
		case LAYER_DETAIL_GRAVEL:
		case LAYER_DETAIL_FINE: {
			// Ground detail (docs/ideas/ground-detail.md): the shore's and the talus's own size laws carried on below
			// min_size into this layer's size class, so the small stones keep the big ones' sorting, rock and family.
			const real_t sub = 1.0;
			const float scree_dmin = MAX(float(min_size), 0.15f);
			float sd1, sd2, pd1, pd2;
			_detail_sizes(p_layer, min_size, sd1, sd2); // shore: up to the smallest stone the shore layer draws
			_detail_sizes(p_layer, scree_dmin, pd1, pd2); // talus: up to the scree layer's own smallest
			const bool shore_on = layer_enabled[LAYER_SHORE] && sd2 > sd1 * 1.01f;
			const bool scree_on = layer_enabled[LAYER_SCREE] && pd2 > pd1 * 1.01f;
			if (!shore_on && !scree_on) {
				break;
			}
			// Each class's cover (count x mean footprint) is held under about half the ground: past that the
			// deposition would only reject.
			const float cap_shore = 0.45f / (0.785f * sd1 * sd2);
			const float cap_scree = 0.45f / (0.785f * pd1 * pd2);
			static const Vector2 probes[9] = { Vector2(0, 0), Vector2(4, 0), Vector2(-4, 0), Vector2(0, 4), Vector2(0, -4), Vector2(10, 0), Vector2(-10, 0), Vector2(0, 10), Vector2(0, -10) };
			const int nx = int(Math::ceil(p_rect.size.x / sub)), nz = int(Math::ceil(p_rect.size.y / sub));
			struct DetailCorner {
				float shore = 0.0f; // stones of the class per m2 from the shore's log-normal (sigma 0.35)
				float scree = 0.0f; // and from the talus's power law (exponent 2.5)
				float d50 = 0.0f; // the shore's median size, m
				float e = 0.0f; // height above the water line
			};
			LocalVector<DetailCorner> corners;
			corners.resize((nx + 1) * (nz + 1));
			for (int j = 0; j <= nz; j++) {
				for (int i = 0; i <= nx; i++) {
					DetailCorner &dc = corners[j * (nx + 1) + i];
					const Vector3 c(p_rect.position.x + i * sub, 0, p_rect.position.y + j * sub);
					if (shore_on) {
						real_t level = -1e30;
						for (const Vector2 &o : probes) {
							const real_t l = gen->get_water_level(c + Vector3(o.x, 0, o.y));
							if (!Math::is_nan(l)) {
								level = MAX(level, l);
							}
						}
						if (level > -1e29) {
							const float e = float(gen->get_height(c) - level);
							if (e >= -0.6f && e <= 2.5f) {
								// As the shore layer: the median size from the beach slope (Bujan et al. 2019), boulders under rock.
								const Vector3 n = ground_normal(gen, c.x, c.z, 1.0);
								const float tanb = Math::sqrt(MAX(1.0f / (n.y * n.y) - 1.0f, 0.0f));
								const float rock = float(gen->get_rock_exposure(c));
								float d50 = 0.002f * Math::exp(tanb / 0.045f);
								d50 = MIN(MAX(d50, 0.5f * Math::smoothstep(0.2f, 0.7f, rock)), 1.2f);
								const float band = Math::smoothstep(-0.6f, 0.0f, e) * (1.0f - Math::smoothstep(1.6f, 2.5f, e));
								// The tail below the shore layer's sizes is wider than its sigma 0.35: a shingle beach is poorly
								// sorted at its fine end (mixed sand and gravel beaches, Kirk 1980; Jennings and Shulmeister
								// 2002), which also grades the grains in with the pebbles instead of a cut where the beach slope
								// changes.
								const float sig = 0.6f * float(Math::SQRT2);
								const float share = 0.5f * (std::erfc(Math::log(sd1 / d50) / sig) - std::erfc(Math::log(sd2 / d50) / sig));
								// A grading curve is by weight, and a surface's cover by a size class follows its weight share
								// (not its count share: counting from the median grain put gravel all over fine sand). Packed
								// as a shingle surface, stones touching.
								dc.shore = MIN(0.85f * band * share / (0.785f * sd1 * sd2), cap_shore);
								dc.d50 = d50;
								dc.e = e;
							}
						}
					}
					if (scree_on) {
						const float t = (float(gen->get_talus(c)) - 0.2f) / 0.8f;
						if (t >= 0.05f) {
							// The scree layer keeps 0.3 t of the ground per (pi 1.25 dmin^2) stones from dmin up; the power
							// law's count between d1 and d2 follows from it.
							const float n_min = 0.3f * t / (float(Math::PI) * 1.25f * scree_dmin * scree_dmin);
							dc.scree = MIN(n_min * (Math::pow(pd1 / scree_dmin, -2.5f) - Math::pow(pd2 / scree_dmin, -2.5f)), cap_scree);
						}
					}
				}
			}
			const float pa = Math::pow(pd1, -2.5f), pb = Math::pow(pd2, -2.5f);
			for (int j = 0; j < nz; j++) {
				for (int i = 0; i < nx; i++) {
					const DetailCorner *cc[4] = { &corners[j * (nx + 1) + i], &corners[j * (nx + 1) + i + 1], &corners[(j + 1) * (nx + 1) + i], &corners[(j + 1) * (nx + 1) + i + 1] };
					float rate_max = 0.0f;
					for (const DetailCorner *k : cc) {
						rate_max = MAX(rate_max, k->shore + k->scree);
					}
					if (rate_max <= 0.0f) {
						continue;
					}
					const int count = MIN(int(rate_max * float(sub * sub) * float(density) + rng.randf()), ground_detail_max_per_cell);
					const real_t x = p_rect.position.x + i * sub, z = p_rect.position.y + j * sub;
					const RockGenerator::RockType type = _type_at(gen, Vector3(x + sub * 0.5f, 0, z + sub * 0.5f));
					for (int k = 0; k < count; k++) {
						const float fx = rng.randf(), fz = rng.randf();
						const float w[4] = { (1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz };
						float shore = 0.0f, scree = 0.0f, d50 = 0.0f, e = 0.0f, ws = 0.0f;
						for (int q = 0; q < 4; q++) {
							shore += w[q] * cc[q]->shore;
							scree += w[q] * cc[q]->scree;
							if (cc[q]->d50 > 0.0f) {
								d50 += w[q] * cc[q]->d50;
								e += w[q] * cc[q]->e;
								ws += w[q];
							}
						}
						const float total = shore + scree;
						if (total <= 0.0f || rng.randf() * rate_max > total) {
							continue;
						}
						Pending pd;
						pd.xz = Vector3(x + fx * sub, 0, z + fz * sub);
						pd.yaw = rng.randf() * Math::TAU;
						pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
						if (ws > 0.0f && rng.randf() * total < shore) {
							d50 /= ws;
							e /= ws;
							// The shore's log-normal, kept to this class (a log-uniform pick when the class is far in its tail).
							float d = 0.0f;
							for (int tries = 0; tries < 12; tries++) {
								const float t = d50 * Math::exp(rng.randfn(0.0f, 0.6f));
								if (t >= sd1 && t < sd2) {
									d = t;
									break;
								}
							}
							pd.size = d > 0.0f ? d : sd1 * Math::pow(sd2 / sd1, rng.randf());
							pd.key = _library_key(type, RockGenerator::FAMILY_PEBBLE, RF_DETAIL_BIN + (p_layer - LAYER_DETAIL_PEBBLE));
							pd.tilt = Math::deg_to_rad(8.0f) * rng.randf();
							pd.sink = 0.2f + 0.1f * rng.randf();
							pd.follow = 0.85f;
							pd.film = e < 0.8f ? 1.0f : CLAMP((2.5f - e) / 1.7f, 0.0f, 1.0f) * 0.5f;
						} else {
							// The talus's power law, kept to this class (inverse transform).
							pd.size = Math::pow(pa - rng.randf() * (pa - pb), -1.0f / 2.5f);
							pd.key = _library_key(type, RockGenerator::FAMILY_SCREE, RF_DETAIL_BIN + (p_layer - LAYER_DETAIL_PEBBLE));
							pd.tilt = Math::deg_to_rad(20.0f) * rng.randf();
							pd.sink = 0.15f + 0.15f * rng.randf();
							pd.follow = 0.8f;
							pd.film = 0.0f;
						}
						pending.push_back(pd);
					}
				}
			}
		} break;
		default:
			break;
	}

	if (pending.is_empty()) {
		return;
	}
	// Largest first, then dropped where they land.
	struct BySize {
		_FORCE_INLINE_ bool operator()(const Pending &p_a, const Pending &p_b) const { return p_a.size > p_b.size; }
	};
	pending.sort_custom<BySize>();
	// Over the cap, thin evenly over the cell rather than dropping the finest: where the grain size changes
	// between sub-cells, a size cut-off left bare rectangles in a shingle beach.
	const bool detail = _is_detail(p_layer);
	const int cap = detail ? ground_detail_max_per_cell : max_stones_per_cell;
	const float keep = int(pending.size()) > cap ? float(cap) / float(pending.size()) : 1.0f;
	Deposit dep;
	Deposit coarse; // the coarser layers' stones, on their own grid (they are far larger than the grains)
	if (detail) {
		// The grains come to rest on the ground and against the stones of the coarser layers already down, and are
		// left out where they would sit inside one.
		dep.CELL = MAX(0.05f, 3.0f * pending[0].size);
		LocalVector<DetailSeed> own_seeds;
		if (!p_seeds) {
			_detail_seeds(p_layer, p_rect.grow(0.5), own_seeds);
		}
		for (const DetailSeed &sd : (p_seeds ? *p_seeds : own_seeds)) {
			coarse.add(sd.xz, sd.centre, sd.r);
		}
	}
	for (const Pending &pd : pending) {
		if (int(r_stones.size()) >= cap) {
			break;
		}
		if (keep < 1.0f && rng.randf() > keep) {
			continue;
		}
		if (!p_rect.has_point(Vector2(pd.xz.x, pd.xz.z))) {
			continue;
		}
		if (p_layer != LAYER_RIVER && p_layer != LAYER_SHORE && gen->get_water_depth(pd.xz) > (detail ? real_t(0.6) : water_skip)) {
			continue;
		}
		Library *lib = nullptr;
		if (p_libs) {
			Library *const *found = p_libs->getptr(pd.key);
			lib = found ? *found : nullptr;
		} else {
			lib = _ensure_library(pd.key);
		}
		if (!lib) {
			continue;
		}
		Stone st;
		st.layer = p_layer;
		st.library = pd.key;
		st.variant = pick_variant(lib);
		st.scale = pd.size * 0.5f;
		st.film = pd.film;
		_rest(gen, st, *lib, pd.xz, pd.yaw, pd.sink, pd.tilt_axis, pd.tilt, pd.follow);
		const Vector3 axes = lib->axes[st.variant];
		const float r = st.scale * (axes.x + axes.y + axes.z) / 3.0f * 0.85f;
		const float ground_centre = float(st.position.y);
		float rise = dep.rest(Vector2(pd.xz.x, pd.xz.z), r, ground_centre, 1.3f * r);
		if (rise < 0.0f) {
			continue;
		}
		if (!coarse.items.is_empty()) {
			const float on_coarse = coarse.rest(Vector2(pd.xz.x, pd.xz.z), r, ground_centre, 1.3f * r);
			if (on_coarse < 0.0f) {
				continue;
			}
			rise = MAX(rise, on_coarse);
		}
		st.position.y += rise;
		dep.add(Vector2(pd.xz.x, pd.xz.z), float(st.position.y), r);
		r_stones.push_back(st);
	}
}

/* Cells */

void RockField3D::_free_cell(Cell &r_cell) {
	for (MultiMeshInstance3D *mmi : r_cell.instances) {
		if (mmi) {
			mmi->queue_free();
		}
	}
	r_cell.instances.clear();
	r_cell.groups.clear();
	close_dirty = true;
	if (r_cell.body) {
		r_cell.body->queue_free();
		r_cell.body = nullptr;
	}
	stone_count -= r_cell.stones;
	r_cell.stones = 0;
}

// How long a family's faces stand exposed between disturbances, for its lichen (0 just turned over, 1 for centuries).
static float rock_surface_age(RockGenerator::Family p_family) {
	switch (p_family) {
		case RockGenerator::FAMILY_SCREE:
			return 0.1f; // active talus: rockfall keeps turning the stones
		case RockGenerator::FAMILY_ROCKFALL:
		case RockGenerator::FAMILY_PEBBLE:
			return 0.2f;
		case RockGenerator::FAMILY_COBBLE:
		case RockGenerator::FAMILY_POTHOLE:
			return 0.3f; // scoured by floods
		case RockGenerator::FAMILY_GOBLIN:
		case RockGenerator::FAMILY_HOODOO:
			return 0.4f; // soft rock, its skin keeps flaking
		case RockGenerator::FAMILY_FIN:
			return 0.6f;
		default:
			return 1.0f;
	}
}

Ref<ShaderMaterial> RockField3D::_material(RockGenerator::RockType p_type, RockGenerator::Family p_family, bool p_detail) {
	const bool p_cliff = p_family == RockGenerator::FAMILY_CAVE_MOUTH;
	const float age = rock_surface_age(p_family);
	const int mkey = int(p_type) + (p_cliff ? 1000 : 0) + int(age * 10.0f + 0.5f) * 10000 + (p_detail ? 2000 : 0);
	if (Ref<ShaderMaterial> *m = materials.getptr(mkey)) {
		return *m;
	}
	Ref<ShaderMaterial> m = RockGenerator::get_material(p_type)->duplicate();
	m->set_shader_parameter("cliff_match", p_cliff);
	m->set_shader_parameter("lichen_cover", lichen_cover);
	m->set_shader_parameter("surface_age", age);
	m->set_shader_parameter("moss_cover", moss_cover);
	// Ground detail grains: a foot band in proportion to the grain (the field's stones keep theirs), and the ground's
	// dust coat the field's small stones carry, so a grain matches the pebbles round it.
	m->set_shader_parameter("small_linear", p_detail);
	_apply_ground(m);
	if (TerrainGenerator3D *gen = _get_generator(); gen && gen->has_sea_result()) {
		m->set_shader_parameter("sea_level", float(gen->get_sea_level())); // the shore zones of sea stones
	}
	materials.insert(mkey, m);
	return m;
}

// Holes opened by others (CaveNetwork3D's entrance patches): the built cells over them are built again without the
// stones that lay there. Holes this field opens itself are handled where it opens them (_build_cell).
void RockField3D::_watch_holes() {
	TerrainGenerator3D *gen = _get_generator();
	if (!gen || gen->get_instance_id() == holes_watched) {
		return;
	}
	const Callable cb = callable_mp(this, &RockField3D::_on_holes_changed);
	if (Object *old = ObjectDB::get_instance(holes_watched)) {
		if (old->is_connected(SNAME("holes_changed"), cb)) {
			old->disconnect(SNAME("holes_changed"), cb);
		}
	}
	gen->connect(SNAME("holes_changed"), cb);
	holes_watched = gen->get_instance_id();
}

void RockField3D::_on_holes_changed(const Rect2 &p_rect) {
	if (building > 0) {
		return;
	}
	LocalVector<Vector3i> again;
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		const real_t ks = _cell_size(Layer(kv.key.x));
		if (Rect2(kv.key.y * ks, kv.key.z * ks, ks, ks).intersects(p_rect.grow(2.0))) {
			again.push_back(kv.key);
		}
	}
	for (const Vector3i &k : again) {
		stone_count -= cells[k].stones;
		_free_cell(cells[k]);
		cells.erase(k);
		_build_cell(k);
	}
}

// The stone's MultiMesh custom data: ground height, film (+2 where the sea reaches it), scale, and the random and the
// ground's slope under it packed in a.
Color RockField3D::_stone_custom(const TerrainGenerator3D *p_gen, const Stone &p_stone, const Vector3 &p_axes, RandomPCG &r_rng) const {
	const bool has_sea = p_gen && p_gen->has_sea_result();
	const real_t sea = has_sea ? p_gen->get_sea_level() : 0.0;
	// Stones the sea reaches carry the shore zones (the shader's sea_level): the coast's forms, and shore
	// stones on the sea's own beach (not a lake's, not the land near sea level inland). Flagged as film + 2.
	bool at_sea = false;
	// (A coast form's ground sits far below it, so it is not tested against the sea.)
	if (has_sea && p_stone.layer == LAYER_COAST) {
		at_sea = true;
	} else if (has_sea && p_stone.layer == LAYER_SHORE && Math::abs(p_stone.ground - sea) < 6.0) {
		const real_t level = p_gen->get_water_level(p_stone.position);
		at_sea = !Math::is_nan(level) && Math::abs(level - sea) < 0.5;
	}
	// w: the random (6 bits) and the ground's slope under the stone (8 bits a component, -2..2), so the
	// shader's contact and ground-blend band follow a slope: ground(x) = x + slope . (p.xz - origin.xz).
	Vector2 slope;
	if (p_gen && p_stone.ground > p_stone.position.y - 30.0) {
		const real_t r = CLAMP(p_stone.scale * MAX(p_axes.x, p_axes.z) * 0.6, 0.2, 4.0);
		const Vector3 o = p_stone.position;
		slope.x = (p_gen->get_height(o + Vector3(r, 0, 0)) - p_gen->get_height(o - Vector3(r, 0, 0))) / (2.0 * r);
		slope.y = (p_gen->get_height(o + Vector3(0, 0, r)) - p_gen->get_height(o - Vector3(0, 0, r))) / (2.0 * r);
	}
	const int qx = CLAMP(int(Math::round((slope.x + 2.0f) / 4.0f * 255.0f)), 0, 255);
	const int qz = CLAMP(int(Math::round((slope.y + 2.0f) / 4.0f * 255.0f)), 0, 255);
	const int rq = MIN(int(r_rng.randf() * 64.0f), 63);
	return Color(p_stone.ground, p_stone.film + (at_sea ? 2.0f : 0.0f), p_stone.scale, float(rq + 64 * (qx + 256 * qz)));
}

MultiMeshInstance3D *RockField3D::_add_group_instance(const Ref<MultiMesh> &p_multimesh, uint32_t p_library, real_t p_visibility, real_t p_margin, bool p_detail) {
	MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
	mmi->set_multimesh(p_multimesh);
	mmi->set_material_override(_material(key_type(p_library), RockGenerator::Family(key_family(p_library)), p_detail));
	mmi->set_visibility_range_end(p_visibility);
	mmi->set_visibility_range_end_margin(p_margin);
	add_child(mmi, false, INTERNAL_MODE_BACK);
	mmi->set_as_top_level(true);
	mmi->set_global_transform(Transform3D());
	return mmi;
}

/* Ground detail cells on worker threads */

// Main thread: what the placement reads, so the worker touches neither the scene tree, the library map nor the cells.
void RockField3D::_detail_prepare(DetailJob &r_job) {
	r_job.field = this;
	r_job.gen = _get_generator();
	const Layer layer = Layer(r_job.key.x);
	const real_t cs = _cell_size(layer);
	const Rect2 rect(r_job.key.y * cs, r_job.key.z * cs, cs, cs);
	// The shapes the placement can pick: either rock (strata), pebble or scree family, this size class.
	const int bin = RF_DETAIL_BIN + (layer - LAYER_DETAIL_PEBBLE);
	const RockGenerator::RockType types[2] = { rock_type, soft_rock_type };
	const RockGenerator::Family families[2] = { RockGenerator::FAMILY_PEBBLE, RockGenerator::FAMILY_SCREE };
	LocalVector<uint32_t> keys, missing;
	for (int t = 0; t < (use_strata ? 2 : 1); t++) {
		for (RockGenerator::Family f : families) {
			const uint32_t k = _library_key(types[t], f, bin);
			keys.push_back(k);
			if (!libraries.has(k)) {
				missing.push_back(k);
			}
		}
	}
	if (!missing.is_empty()) {
		_ensure_libraries(missing);
	}
	for (uint32_t k : keys) {
		Library *lib = libraries.getptr(k);
		if (lib && !lib->meshes.is_empty()) {
			r_job.libs[k] = lib;
		}
	}
	_detail_seeds(layer, rect.grow(0.5), r_job.seeds);
}

// Any thread: places the cell's grains and packs each shape's records.
void RockField3D::_detail_compute(DetailJob &r_job) {
	const TerrainGenerator3D *gen = r_job.gen;
	if (!gen) {
		return;
	}
	const Vector3i key = r_job.key;
	const Layer layer = Layer(key.x);
	const real_t cs = _cell_size(layer);
	const Rect2 rect(key.y * cs, key.z * cs, cs, cs);
	LocalVector<Stone> stones;
	rf_job_generator = r_job.gen;
	_place(layer, rect, rf_hash3(key.x, key.y, key.z, uint32_t(seed) * 2654435761u), stones, &r_job.libs, &r_job.seeds);
	rf_job_generator = nullptr;
	if (gen->has_holes_in(rect.grow(8.0))) {
		for (int i = int(stones.size()) - 1; i >= 0; i--) {
			if (gen->is_hole(stones[i].position)) {
				stones.remove_at(i);
			}
		}
	}
	r_job.stones = int(stones.size());
	// As _build_cell: one MultiMesh per shape, the same randoms in the same order.
	HashMap<uint64_t, LocalVector<uint32_t>> groups;
	for (uint32_t i = 0; i < stones.size(); i++) {
		groups[(uint64_t(stones[i].library) << 16) | stones[i].variant].push_back(i);
	}
	RandomPCG rng(rf_hash3(key.y, key.z, key.x, 991u));
	for (const KeyValue<uint64_t, LocalVector<uint32_t>> &g : groups) {
		const uint32_t lk = uint32_t(g.key >> 16);
		const int v = int(g.key & 0xFFFF);
		Library *const *found = r_job.libs.getptr(lk);
		const Library *lib = found ? *found : nullptr;
		if (!lib || v >= int(lib->meshes.size())) {
			continue;
		}
		const Vector3 ax = lib->axes[v];
		DetailGroup dg;
		dg.library = lk;
		dg.variant = v;
		const uint32_t n = g.value.size();
		dg.buffer.resize(n * 16);
		float *w = dg.buffer.ptrw();
		dg.xforms.resize(n);
		dg.custom.resize(n);
		dg.reach.resize(n);
		for (uint32_t k = 0; k < n; k++) {
			const Stone &st = stones[g.value[k]];
			const Transform3D xf(st.basis, st.position);
			const Color cd = _stone_custom(gen, st, ax, rng);
			float *rec = w + k * 16;
			for (int row = 0; row < 3; row++) {
				rec[row * 4 + 0] = xf.basis.rows[row][0];
				rec[row * 4 + 1] = xf.basis.rows[row][1];
				rec[row * 4 + 2] = xf.basis.rows[row][2];
				rec[row * 4 + 3] = xf.origin[row];
			}
			rec[12] = cd.r;
			rec[13] = cd.g;
			rec[14] = cd.b;
			rec[15] = cd.a;
			dg.xforms[k] = xf;
			dg.custom[k] = cd;
			dg.reach[k] = st.scale * MAX(ax.x, MAX(ax.y, ax.z));
		}
		r_job.groups.push_back(dg);
	}
}

void RockField3D::_detail_job(void *p_job) {
	DetailJob *job = static_cast<DetailJob *>(p_job);
	job->field->_detail_compute(*job);
}

// Main thread: the cell's MultiMeshes, one buffer upload each.
void RockField3D::_detail_commit(DetailJob &r_job) {
	const Layer layer = Layer(r_job.key.x);
	const real_t cs = _cell_size(layer);
	const real_t vis = layer_radius[layer] + cs;
	Cell cell;
	cell.stones = r_job.stones;
	stone_count += cell.stones;
	for (DetailGroup &dg : r_job.groups) {
		const Library *lib = libraries.getptr(dg.library);
		if (!lib || dg.variant >= int(lib->meshes.size())) {
			continue;
		}
		const int n = int(dg.xforms.size());
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(lib->meshes[dg.variant]);
		mm->set_instance_count(n);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), dg.buffer);
		// Hundreds of grains of one shape sit under the merged BLAS cap, which would bake a copy of every grain's
		// triangles: give each its own TLAS entry over the shared shape instead.
		RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true);
		Group grp;
		grp.library = dg.library;
		grp.variant = dg.variant;
		grp.multimesh = mm;
		grp.xforms = std::move(dg.xforms);
		grp.custom = std::move(dg.custom);
		grp.reach = std::move(dg.reach);
		grp.near.resize(n);
		grp.broken.resize(n);
		grp.shapes.resize(n);
		for (int k = 0; k < n; k++) {
			grp.near[k] = false;
			grp.broken[k] = false;
			grp.shapes[k] = nullptr;
		}
		cell.groups.push_back(grp);
		cell.instances.push_back(_add_group_instance(mm, dg.library, vis, cs * 0.5, true));
	}
	cells.insert(r_job.key, cell);
}

// Adds the finished jobs still wanted (within p_budget_msec unless p_wait), drops the rest.
void RockField3D::_detail_finish(bool p_wait, real_t p_budget_msec) {
	if (detail_jobs.is_empty()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector3 dc = _detail_center();
	LocalVector<Vector3i> done;
	for (KeyValue<Vector3i, DetailJob *> &kv : detail_jobs) {
		DetailJob *job = kv.value;
		if (!p_wait && (!WorkerThreadPool::get_singleton()->is_task_completed(job->task) || double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0 > p_budget_msec)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		const Layer layer = Layer(kv.key.x);
		const real_t cs = _cell_size(layer);
		const Vector2 c((kv.key.y + 0.5f) * cs, (kv.key.z + 0.5f) * cs);
		if (_layer_on(layer) && !cells.has(kv.key) && c.distance_to(Vector2(dc.x, dc.z)) <= layer_radius[layer] + cs * 1.5f) {
			_detail_commit(*job);
		}
		memdelete(job);
		done.push_back(kv.key);
	}
	for (const Vector3i &k : done) {
		detail_jobs.erase(k);
	}
}

void RockField3D::_detail_cancel() {
	for (KeyValue<Vector3i, DetailJob *> &kv : detail_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
		memdelete(kv.value);
	}
	detail_jobs.clear();
}

// Adds the coarse cells placed on workers that are still wanted (within p_budget_msec unless p_wait). A cell that asked
// for a shape library not built yet starts it on workers and is placed again once it is in.
void RockField3D::_coarse_finish(bool p_wait, real_t p_budget_msec) {
	if (coarse_jobs.is_empty()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector3 tp = _target_position();
	LocalVector<Vector3i> done;
	LocalVector<uint32_t> start;
	for (KeyValue<Vector3i, CoarseJob *> &kv : coarse_jobs) {
		CoarseJob *job = kv.value;
		if (!p_wait && (!WorkerThreadPool::get_singleton()->is_task_completed(job->task) || double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0 > p_budget_msec)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		const Layer layer = Layer(kv.key.x);
		const real_t cs = _cell_size(layer);
		const Vector2 c((kv.key.y + 0.5f) * cs, (kv.key.z + 0.5f) * cs);
		if (!job->missing.is_empty()) {
			for (uint32_t k : job->missing) {
				if (!libraries.has(k) && !library_builds.has(k) && start.find(k) < 0) {
					start.push_back(k);
				}
			}
		} else if (_layer_on(layer) && !cells.has(kv.key) && c.distance_to(Vector2(tp.x, tp.z)) <= layer_radius[layer] + cs * 1.5f) {
			_coarse_commit(kv.key, job->stones, job->groups);
		}
		memdelete(job);
		done.push_back(kv.key);
	}
	for (const Vector3i &k : done) {
		coarse_jobs.erase(k);
	}
	if (!start.is_empty()) {
		LibraryBuild *b = _start_libraries(start);
		for (uint32_t k : start) {
			library_builds.insert(k, b);
		}
	}
}

void RockField3D::_coarse_cancel() {
	for (KeyValue<Vector3i, CoarseJob *> &kv : coarse_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
		memdelete(kv.value);
	}
	coarse_jobs.clear();
}

void RockField3D::set_cell_threads(bool p_enabled) {
	if (cell_threads == p_enabled) {
		return;
	}
	if (!p_enabled) {
		_coarse_finish(true, 0.0);
	}
	cell_threads = p_enabled;
}

void RockField3D::set_ground_detail_threads(bool p_enabled) {
	if (detail_threads == p_enabled) {
		return;
	}
	if (!p_enabled) {
		_detail_finish(true, 0.0);
	}
	detail_threads = p_enabled;
}

void RockField3D::_build_cell(const Vector3i &p_key) {
	building++;
	struct Done {
		int &n;
		~Done() { n--; }
	} done{ building };
	const Layer layer = Layer(p_key.x);
	if (_is_detail(layer)) {
		DetailJob job;
		job.key = p_key;
		_detail_prepare(job);
		_detail_compute(job);
		_detail_commit(job);
		return;
	}
	const real_t cs = _cell_size(layer);
	const Rect2 rect(p_key.y * cs, p_key.z * cs, cs, cs);
	LocalVector<Stone> stones;
	library_pending = false;
	_place(layer, rect, rf_hash3(p_key.x, p_key.y, p_key.z, uint32_t(seed) * 2654435761u), stones);
	if (library_pending) {
		// A shape library is still building on workers (streaming): the cell is placed again once it is in.
		library_pending = false;
		return;
	}
	LocalVector<CoarseGroup> groups;
	_coarse_commit(p_key, stones, groups);
}

// Any thread: one MultiMesh's records per shape, the same randoms in the same order wherever it runs.
void RockField3D::_coarse_pack(const Vector3i &p_key, const LocalVector<Stone> &p_stones, LocalVector<CoarseGroup> &r_groups) {
	const TerrainGenerator3D *gen = _get_generator();
	HashMap<uint64_t, LocalVector<uint32_t>> by_shape;
	for (uint32_t i = 0; i < p_stones.size(); i++) {
		by_shape[(uint64_t(p_stones[i].library) << 16) | p_stones[i].variant].push_back(i);
	}
	RandomPCG rng(rf_hash3(p_key.y, p_key.z, p_key.x, 991u));
	for (const KeyValue<uint64_t, LocalVector<uint32_t>> &g : by_shape) {
		const uint32_t lk = uint32_t(g.key >> 16);
		const int v = int(g.key & 0xFFFF);
		const Library *lib = _ensure_library(lk);
		if (!lib || v >= int(lib->meshes.size())) {
			continue;
		}
		const Vector3 ax = lib->axes[v];
		CoarseGroup cg;
		cg.library = lk;
		cg.variant = v;
		const uint32_t n = g.value.size();
		cg.members = g.value;
		cg.buffer.resize(n * 16);
		float *w = cg.buffer.ptrw();
		cg.xforms.resize(n);
		cg.custom.resize(n);
		cg.reach.resize(n);
		for (uint32_t k = 0; k < n; k++) {
			const Stone &st = p_stones[g.value[k]];
			const Transform3D xf(st.basis, st.position);
			const Color cd = _stone_custom(gen, st, ax, rng);
			float *rec = w + k * 16;
			for (int row = 0; row < 3; row++) {
				rec[row * 4 + 0] = xf.basis.rows[row][0];
				rec[row * 4 + 1] = xf.basis.rows[row][1];
				rec[row * 4 + 2] = xf.basis.rows[row][2];
				rec[row * 4 + 3] = xf.origin[row];
			}
			rec[12] = cd.r;
			rec[13] = cd.g;
			rec[14] = cd.b;
			rec[15] = cd.a;
			cg.xforms[k] = xf;
			cg.custom[k] = cd;
			cg.reach[k] = st.scale * MAX(ax.x, MAX(ax.y, ax.z));
		}
		r_groups.push_back(std::move(cg));
	}
}

// Worker: places a coarse cell and packs it; false when it asked for a shape library not built yet.
bool RockField3D::_coarse_place(CoarseJob &r_job) {
	const Vector3i key = r_job.key;
	const Layer layer = Layer(key.x);
	const real_t cs = _cell_size(layer);
	const Rect2 rect(key.y * cs, key.z * cs, cs, cs);
	rf_job_generator = r_job.gen;
	rf_coarse_job = &r_job;
	_place(layer, rect, rf_hash3(key.x, key.y, key.z, uint32_t(seed) * 2654435761u), r_job.stones);
	const bool ok = r_job.missing.is_empty();
	if (ok) {
		_coarse_pack(key, r_job.stones, r_job.groups);
	}
	rf_coarse_job = nullptr;
	rf_job_generator = nullptr;
	return ok;
}

void RockField3D::_coarse_job(void *p_job) {
	CoarseJob *job = static_cast<CoarseJob *>(p_job);
	job->field->_coarse_place(*job);
}

// Main thread: a placed coarse cell's stones off holes and broken ones, its MultiMeshes (packed already on a worker
// unless p_groups is empty or a stone was taken out) and collision.
void RockField3D::_coarse_commit(const Vector3i &p_key, LocalVector<Stone> &stones, LocalVector<CoarseGroup> &p_groups) {
	building++;
	struct Done {
		int &n;
		~Done() { n--; }
	} done{ building };
	const Layer layer = Layer(p_key.x);
	const real_t cs = _cell_size(layer);
	const Rect2 rect(p_key.y * cs, p_key.z * cs, cs, cs);
	const uint32_t placed = stones.size();
	// Nothing lies on ground a cave mouth opened (TerrainGenerator3D holes); the cave's own blocks are landforms.
	if (layer != LAYER_LANDFORM) {
		if (const TerrainGenerator3D *hg = _get_generator(); hg && hg->has_holes_in(rect.grow(8.0))) {
			for (int i = int(stones.size()) - 1; i >= 0; i--) {
				if (hg->is_hole(stones[i].position)) {
					stones.remove_at(i);
				}
			}
		}
	}
	if (!broken_stones.is_empty()) {
		for (int i = int(stones.size()) - 1; i >= 0; i--) {
			if (broken_stones.has(Vector3i((stones[i].position * 100.0f).round()))) {
				stones.remove_at(i);
			}
		}
	}
	if (p_groups.is_empty() || stones.size() != placed) {
		p_groups.clear();
		_coarse_pack(p_key, stones, p_groups);
	}
	Cell cell;
	cell.stones = int(stones.size());
	stone_count += cell.stones;
	// One MultiMesh per shape, its records in one upload.
	const real_t vis = layer_radius[layer] + cs;
	LocalVector<Vector2i> where; // stone -> (group, index in it)
	where.resize(stones.size());
	for (Vector2i &w : where) {
		w = Vector2i(-1, -1);
	}
	for (CoarseGroup &cg : p_groups) {
		const Library *lib = libraries.getptr(cg.library);
		if (!lib || cg.variant >= int(lib->meshes.size())) {
			continue;
		}
		const int n = int(cg.members.size());
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(lib->meshes[cg.variant]);
		mm->set_instance_count(n);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), cg.buffer);
		Group grp;
		grp.library = cg.library;
		grp.variant = cg.variant;
		grp.multimesh = mm;
		grp.xforms = std::move(cg.xforms);
		grp.custom = std::move(cg.custom);
		grp.reach = std::move(cg.reach);
		grp.near.resize(n);
		grp.broken.resize(n);
		grp.shapes.resize(n);
		for (int k = 0; k < n; k++) {
			grp.near[k] = false;
			grp.broken[k] = false;
			grp.shapes[k] = nullptr;
			where[cg.members[k]] = Vector2i(int(cell.groups.size()), k);
		}
		cell.groups.push_back(grp);
		cell.instances.push_back(_add_group_instance(mm, cg.library, vis, cs * 0.5));
	}
	// Collision for the stones big enough to stand on or trip over, near the target.
	const Vector3 tp = _target_position();
	const Vector2 cc = rect.get_center();
	// Coast forms at any distance (whatever collision_enabled says): the sea measures its floor by ray casts and turns
	// a cell whose floor stands out of the water into shore, so a stack gets its foam and run-up (water's
	// seabed_from_floor); remeasured once the cells are built.
	const bool near = collision_enabled && Vector2(tp.x, tp.z).distance_to(cc) < collision_radius + cs * 0.71;
	const bool coast = coast_collision && layer == LAYER_COAST;
	if (near || coast) {
		StaticBody3D *body = nullptr;
		for (uint32_t si = 0; si < stones.size(); si++) {
			const Stone &st = stones[si];
			if (2.0f * st.scale < collision_min_size) {
				continue;
			}
			if (!near && key_family(st.library) < RockGenerator::FAMILY_SEA_STACK) {
				continue;
			}
			const Library *lib = libraries.getptr(st.library);
			if (!lib || st.variant >= lib->hulls.size() || lib->hulls[st.variant].is_empty()) {
				continue;
			}
			if (!body) {
				body = memnew(StaticBody3D);
				body->set_collision_layer(collision_layer);
				body->set_collision_mask(0);
				add_child(body, false, INTERNAL_MODE_BACK);
				body->set_as_top_level(true);
				body->set_global_transform(Transform3D());
			}
			const Transform3D xf(st.basis, st.position);
			if (key_family(st.library) >= RockGenerator::FAMILY_SEA_STACK) {
				// A coast form is not convex (an arch's opening, a stack's notch, a platform's pools): its own
				// triangles. Not breakable (no shape registered for it).
				if (st.variant < lib->meshes.size() && lib->meshes[st.variant].is_valid()) {
					Library *wlib = libraries.getptr(st.library);
					if (wlib->trimesh.size() < wlib->meshes.size()) {
						wlib->trimesh.resize(wlib->meshes.size());
					}
					if (wlib->trimesh[st.variant].is_null()) {
						Ref<ConcavePolygonShape3D> shape;
						shape.instantiate();
						const Array arrays = lib->meshes[st.variant]->surface_get_arrays(0);
						const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
						const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
						Vector<Vector3> faces;
						faces.resize(idx.size());
						for (int k = 0; k < idx.size(); k++) {
							faces.write[k] = v[idx[k]];
						}
						shape->set_faces(faces);
						wlib->trimesh[st.variant] = shape;
					}
					CollisionShape3D *cs3 = memnew(CollisionShape3D);
					cs3->set_shape(wlib->trimesh[st.variant]);
					cs3->set_transform(xf);
					body->add_child(cs3);
					coast_seabed_dirty = coast_seabed_dirty || layer == LAYER_COAST;
				}
				continue;
			}
			const PackedVector3Array &hull = lib->hulls[st.variant];
			Vector<Vector3> pts;
			pts.resize(hull.size());
			for (int k = 0; k < hull.size(); k++) {
				pts.write[k] = xf.xform(hull[k]);
			}
			Ref<ConvexPolygonShape3D> shape;
			shape.instantiate();
			shape->set_points(pts);
			CollisionShape3D *cs3 = memnew(CollisionShape3D);
			cs3->set_shape(shape);
			body->add_child(cs3);
			if (where[si].x >= 0) {
				cell.groups[where[si].x].shapes[where[si].y] = cs3;
			}
		}
		cell.body = body;
	}
	cells.insert(p_key, cell);
	close_dirty = true;
	// Holes this cell opened: the cells of other layers already built over them are built again without the stones
	// that lay there.
	if (!hole_rects.is_empty()) {
		LocalVector<Rect2> rects;
		rects.resize(hole_rects.size());
		for (uint32_t i = 0; i < hole_rects.size(); i++) {
			rects[i] = hole_rects[i];
		}
		hole_rects.clear();
		LocalVector<Vector3i> again;
		for (const KeyValue<Vector3i, Cell> &kv : cells) {
			if (kv.key.x == LAYER_LANDFORM) {
				continue;
			}
			const real_t ks = _cell_size(Layer(kv.key.x));
			const Rect2 kr(kv.key.y * ks, kv.key.z * ks, ks, ks);
			for (const Rect2 &r : rects) {
				if (kr.intersects(r.grow(8.0))) {
					again.push_back(kv.key);
					break;
				}
			}
		}
		for (const Vector3i &k : again) {
			stone_count -= cells[k].stones;
			_free_cell(cells[k]);
			cells.erase(k);
			_build_cell(k);
		}
	}
}

void RockField3D::_update_streaming(bool p_all) {
	_watch_holes();
	TerrainGenerator3D *gen = _get_generator();
	const Vector3 tp = _target_position();
	if (!gen || !Math::is_finite(gen->get_height(tp))) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_update_detail_radii();
	// Drop cells out of reach (with a cell of slack, so a target on a border does not churn).
	const Vector3 dc = _detail_center();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		const Layer layer = Layer(kv.key.x);
		const real_t cs = _cell_size(layer);
		const Vector2 c((kv.key.y + 0.5f) * cs, (kv.key.z + 0.5f) * cs);
		const Vector3 lc = _is_detail(layer) ? dc : tp;
		if (!_layer_on(layer) || c.distance_to(Vector2(lc.x, lc.z)) > layer_radius[layer] + cs * 1.5f) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
	_collect_libraries(p_all);
	// Coarse cells placed on workers since the last frame.
	_coarse_finish(p_all, MAX(build_budget_msec * 0.5, real_t(1.0)));
	// Ground detail cells placed on workers since the last frame.
	uint64_t detail_usec = OS::get_singleton()->get_ticks_usec();
	_detail_finish(p_all, MAX(build_budget_msec * 0.5, real_t(1.0)));
	detail_usec = OS::get_singleton()->get_ticks_usec() - detail_usec;
	// Wanted cells, nearest first.
	struct Want {
		Vector3i key;
		real_t dist;
	};
	LocalVector<Want> want;
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!_layer_on(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const real_t r = layer_radius[l];
		const Vector3 lc = _is_detail(l) ? dc : tp;
		const int x0 = int(Math::floor((lc.x - r) / cs)), x1 = int(Math::floor((lc.x + r) / cs));
		const int z0 = int(Math::floor((lc.z - r) / cs)), z1 = int(Math::floor((lc.z + r) / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector3i key(l, x, z);
				if (cells.has(key)) {
					continue;
				}
				const Rect2 rect(x * cs, z * cs, cs, cs);
				const Vector2 nearest(CLAMP(lc.x, rect.position.x, rect.get_end().x), CLAMP(lc.z, rect.position.y, rect.get_end().y));
				const real_t d = nearest.distance_to(Vector2(lc.x, lc.z));
				if (d <= r) {
					want.push_back({ key, d });
				}
			}
		}
	}
	// Nearest first, the detail layers coarse before fine, so each grain cell finds the stones it rests against already
	// down (_detail_seeds).
	struct ByDist {
		_FORCE_INLINE_ bool operator()(const Want &p_a, const Want &p_b) const {
			const int da = _is_detail(p_a.key.x) ? p_a.key.x : 0, db = _is_detail(p_b.key.x) ? p_b.key.x : 0;
			return da != db ? da < db : p_a.dist < p_b.dist;
		}
	};
	want.sort_custom<ByDist>();
	// Ground detail first: its cells are a few metres from the camera, so they would wait behind the far cells of the
	// coarse layers otherwise. A detail cell waits for every coarser cell round it (the stones its grains rest on), and
	// goes to a worker unless ground_detail_threads is off.
	const bool threads = detail_threads && !p_all;
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count() - 1);
	LocalVector<Rect2> unbuilt; // wanted cells of the coarse layers not down yet
	for (const Want &w : want) {
		if (!_is_detail(w.key.x)) {
			const real_t cs = _cell_size(Layer(w.key.x));
			unbuilt.push_back(Rect2(w.key.y * cs, w.key.z * cs, cs, cs));
		}
	}
	bool over_budget = false;
	auto out_of_budget = [&]() {
		over_budget = over_budget || (!p_all && double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0 > build_budget_msec);
		return over_budget;
	};
	int detail_layer = -1;
	bool layer_waiting = false;
	for (const Want &w : want) {
		if (!_is_detail(w.key.x)) {
			continue;
		}
		if (w.key.x != detail_layer) {
			if (layer_waiting) {
				break;
			}
			detail_layer = w.key.x;
		}
		const real_t cs = _cell_size(Layer(w.key.x));
		const Rect2 near_rect = Rect2(w.key.y * cs, w.key.z * cs, cs, cs).grow(4.5);
		bool blocked = false;
		for (const Rect2 &r : unbuilt) {
			if (r.intersects(near_rect)) {
				blocked = true;
				break;
			}
		}
		if (blocked || detail_jobs.has(w.key)) {
			layer_waiting = true;
			continue;
		}
		if (threads) {
			layer_waiting = true;
			if (int(detail_jobs.size()) >= max_jobs) {
				continue;
			}
			const uint64_t tp0 = OS::get_singleton()->get_ticks_usec();
			DetailJob *job = memnew(DetailJob);
			job->key = w.key;
			_detail_prepare(*job);
			job->task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_detail_job, job, false, "RockField3D ground detail");
			detail_jobs.insert(w.key, job);
			detail_usec += OS::get_singleton()->get_ticks_usec() - tp0;
			continue;
		}
		if (out_of_budget()) {
			break;
		}
		const uint64_t tb0 = OS::get_singleton()->get_ticks_usec();
		_build_cell(w.key);
		detail_usec += OS::get_singleton()->get_ticks_usec() - tb0;
	}
	// Then the coarse layers, nearest first; a cell whose shapes are not built yet waits for them on workers (not
	// with cave mouths on: those open terrain holes as they are placed). Placed on workers too unless cell_threads is
	// off (or the landforms with cave mouths on).
	const bool cthreads = cell_threads && !p_all;
	for (const Want &w : want) {
		if (_is_detail(w.key.x)) {
			continue;
		}
		if (cthreads && !(cave_mouths && w.key.x == LAYER_LANDFORM)) {
			if (coarse_jobs.has(w.key) || int(coarse_jobs.size()) >= max_jobs) {
				continue;
			}
			_prepare_rivers(); // read by the workers
			CoarseJob *job = memnew(CoarseJob);
			job->field = this;
			job->key = w.key;
			job->gen = gen;
			job->task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_coarse_job, job, false, "RockField3D cells");
			coarse_jobs.insert(w.key, job);
			continue;
		}
		if (out_of_budget()) {
			break;
		}
		library_async = !p_all && !(cave_mouths && w.key.x == LAYER_LANDFORM);
		_build_cell(w.key);
		library_async = false;
	}
	last_detail_msec = double(detail_usec) / 1000.0;
	_remeasure_sea();
	last_build_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	_update_close_up(p_all);
	_update_breakable();
}

// Close-up detail: stones whose surface comes within close_up_radius of the target are drawn with a close-up
// version of their shape (RockGenerator.close_up: the grid fine enough to carry the rock's millimetre fabric as
// geometry), shared by every near stone of that shape; the base instance is collapsed meanwhile. Close-up shapes
// are built on worker threads as they are first wanted and dropped when no stone has wanted them for a while.
void RockField3D::_update_close_up(bool p_wait) {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	_finish_moss(p_wait);
	// Finished builds.
	for (int i = int(close_jobs.size()) - 1; i >= 0; i--) {
		CloseJob &cj = close_jobs[i];
		if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(cj.task)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(cj.task);
		LibraryJob *job = static_cast<LibraryJob *>(cj.job);
		if (Library *lib = libraries.getptr(cj.library)) {
			if (cj.variant < int(lib->close.size())) {
				if (job->mesh.is_valid()) {
					lib->close[cj.variant] = job->mesh->get_mesh();
					lib->close_state[cj.variant] = 2;
				} else {
					lib->close_state[cj.variant] = 0;
				}
			}
		}
		memdelete(job);
		close_jobs.remove_at(i);
		close_dirty = true;
	}
	if (!close_up_enabled) {
		return;
	}
	const Vector3 tp = _target_position();
	if (!close_dirty && tp.distance_to(close_target) < 0.25f) {
		return;
	}
	close_dirty = false;
	close_target = tp;
	// Which stones are near, by shape; start builds for shapes not ready.
	HashMap<uint64_t, LocalVector<int>> wanted_count;
	HashMap<uint64_t, LocalVector<Transform3D>> near_xf;
	HashMap<uint64_t, LocalVector<Color>> near_cd;
	int building = int(close_jobs.size());
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(kv.key.x));
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		const Vector2 nearest(CLAMP(tp.x, rect.position.x, rect.get_end().x), CLAMP(tp.z, rect.position.y, rect.get_end().y));
		const bool cell_near = nearest.distance_to(Vector2(tp.x, tp.z)) < close_up_radius + 6.0f;
		for (Group &grp : kv.value.groups) {
			Library *lib = libraries.getptr(grp.library);
			if (!lib || (grp.variant + 1) * RF_MARK_BINS > int(lib->close.size())) {
				continue;
			}
			// Cobbles and pebbles: the base grid already holds their fabric at millimetres.
			const RockGenerator::Family fam = key_family(grp.library);
			if (fam == RockGenerator::FAMILY_COBBLE || fam == RockGenerator::FAMILY_PEBBLE || fam >= RockGenerator::FAMILY_SEA_STACK) {
				continue; // coast forms: tens of metres of bedrock, their base grid is the detail they can hold
			}
			const real_t fam_mpu = RockGenerator::get_family_metres_per_unit(fam);
			bool changed = false;
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				// One close-up shape per octave of size: its marks are carved at the stone's own size.
				const int slot = rf_mark_slot(grp.library, grp.variant, grp.custom[k].b);
				const int bin = slot % RF_MARK_BINS - RF_MARK_BINS / 2;
				const uint64_t key = (uint64_t(grp.library) << 16) | uint64_t(slot);
				// Stones more than an octave off their family's size (scree toe blocks, the odd giant chip) keep the
				// base shape: their close-up would be a shape of its own for one or two stones.
				bool want = !grp.broken[k] && Math::abs(bin) <= 1 && cell_near && grp.xforms[k].origin.distance_to(tp) - grp.reach[k] < close_up_radius;
				if (want) {
					lib->close_used[slot] = now;
					if (lib->close_state[slot] == 0 && building < 2) {
						// Build it: one or two at a time, so the workers keep up with walking pace.
						LibraryJob *job = memnew(LibraryJob);
						job->type = key_type(grp.library);
						job->family = key_family(grp.library);
						job->seed = seed * 1000 + grp.variant;
						job->detail = float(detail);
						job->close_up = true;
						job->marks_scale = float(fam_mpu * Math::pow(real_t(2.0), real_t(bin)));
						if (job->family == RockGenerator::FAMILY_COBBLE) {
							job->transport = RockGenerator::get_lithology(job->type).abrasion_length * cobble_bins[CLAMP(key_bin(grp.library), 0, 2)];
						}
						CloseJob cj;
						cj.library = grp.library;
						cj.variant = slot;
						cj.job = job;
						cj.task = WorkerThreadPool::get_singleton()->add_native_task(&close_job, job, false, "RockField3D close-up");
						close_jobs.push_back(cj);
						lib->close_state[slot] = 1;
						building++;
					}
					want = lib->close_state[slot] == 2;
				}
				if (want != grp.near[k]) {
					grp.near[k] = want;
					grp.multimesh->set_instance_transform(int(k), (want || grp.broken[k]) ? Transform3D(Basis().scaled(Vector3(0, 0, 0)), grp.xforms[k].origin) : grp.xforms[k]);
					changed = true;
				}
				if (want) {
					near_xf[key].push_back(grp.xforms[k]);
					near_cd[key].push_back(grp.custom[k]);
				}
			}
			(void)changed;
		}
	}
	// Close-up instances: one MultiMesh per shape, rebuilt from the near list.
	LocalVector<uint64_t> drop;
	for (KeyValue<uint64_t, MultiMeshInstance3D *> &kv : close_instances) {
		if (!near_xf.has(kv.key)) {
			kv.value->queue_free();
			drop.push_back(kv.key);
		}
	}
	for (uint64_t k : drop) {
		close_instances.erase(k);
	}
	for (KeyValue<uint64_t, LocalVector<Transform3D>> &kv : near_xf) {
		const uint32_t lk = uint32_t(kv.key >> 16);
		const int v = int(kv.key & 0xFFFF);
		Library *lib = libraries.getptr(lk);
		MultiMeshInstance3D *mmi = nullptr;
		if (MultiMeshInstance3D **found = close_instances.getptr(kv.key)) {
			mmi = *found;
		} else {
			mmi = memnew(MultiMeshInstance3D);
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_custom_data(true);
			mm->set_mesh(lib->close[v]);
			mmi->set_multimesh(mm);
			mmi->set_material_override(_material(key_type(lk), RockGenerator::Family(key_family(lk))));
			mmi->set_meta("rock_close_up", true);
			add_child(mmi, false, INTERNAL_MODE_BACK);
			mmi->set_as_top_level(true);
			mmi->set_global_transform(Transform3D());
			close_instances.insert(kv.key, mmi);
		}
		Ref<MultiMesh> mm = mmi->get_multimesh();
		const LocalVector<Color> &cd = near_cd[kv.key];
		mm->set_instance_count(int(kv.value.size()));
		for (uint32_t k = 0; k < kv.value.size(); k++) {
			mm->set_instance_transform(int(k), kv.value[k]);
			mm->set_instance_custom_data(int(k), cd[k]);
		}
	}
	if (moss_cushions) {
		_update_moss(near_xf, near_cd, p_wait);
	}
	// Drop close-up shapes nothing has wanted for half a minute, oldest first, beyond the budget.
	int ready = 0;
	for (KeyValue<uint32_t, Library> &kv : libraries) {
		for (uint32_t v = 0; v < kv.value.close.size(); v++) {
			if (kv.value.close_state[v] == 2) {
				ready++;
				if (ready > close_up_max_shapes && now - kv.value.close_used[v] > 30000000u) {
					kv.value.close[v] = Ref<ArrayMesh>();
					kv.value.close_state[v] = 0;
					ready--;
				}
			}
		}
	}
}

void RockField3D::_clear_close_up() {
	for (CloseJob &cj : close_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(cj.task);
		memdelete(static_cast<LibraryJob *>(cj.job));
	}
	close_jobs.clear();
	for (KeyValue<uint64_t, MultiMeshInstance3D *> &kv : close_instances) {
		kv.value->queue_free();
	}
	close_instances.clear();
	close_dirty = true;
	_clear_moss();
}

// Moss shells (docs/ideas/rocks.md, "Moss cushions as real geometry"): each close-up stone 30 cm or bigger gets a
// shell built from its close-up shape where the rock shader paints moss; built two at a time on workers, kept
// while the stone stays close.
void RockField3D::_update_moss(const HashMap<uint64_t, LocalVector<Transform3D>> &p_near_xf, const HashMap<uint64_t, LocalVector<Color>> &p_near_cd, bool p_wait) {
	for (KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		kv.value.wanted = false;
	}
	int building = 0;
	for (const KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		building += kv.value.job ? 1 : 0;
	}
	for (const KeyValue<uint64_t, LocalVector<Transform3D>> &kv : p_near_xf) {
		const uint32_t lk = uint32_t(kv.key >> 16);
		const int slot = int(kv.key & 0xFFFF);
		Library *lib = libraries.getptr(lk);
		if (!lib || slot >= int(lib->close.size()) || lib->close[slot].is_null()) {
			continue;
		}
		const LocalVector<Color> *cds = p_near_cd.getptr(kv.key);
		if (!cds) {
			continue;
		}
		// Moss needs a stone that has lain still for years, as the shader's moss does (surface_age): none on active
		// scree or fresh rockfall, whose stones are turned over by each fall.
		const float age = float(rock_surface_age(RockGenerator::Family(key_family(lk))));
		const float age_cover = Math::smoothstep(0.15f, 0.6f, age);
		if (age_cover <= 0.0f) {
			continue;
		}
		for (uint32_t k = 0; k < kv.value.size(); k++) {
			const Transform3D &xf = kv.value[k];
			const Color &cd = (*cds)[k];
			if (cd.b < 0.15f) {
				continue; // stones under 30 cm: their moss stays the shader's
			}
			const Vector3i key = Vector3i((xf.origin * 100.0).round());
			if (MossShell *ms = moss_shells.getptr(key)) {
				ms->wanted = true;
				continue;
			}
			if (building >= (p_wait ? 16 : 2)) {
				continue; // two at a time at walking pace; more when a build waits for the finished picture
			}
			RockMoss::Source *src = moss_sources.getptr(kv.key);
			if (!src) {
				const Array arr = lib->close[slot]->surface_get_arrays(0);
				RockMoss::Source ns;
				ns.vertices = arr[Mesh::ARRAY_VERTEX];
				ns.normals = arr[Mesh::ARRAY_NORMAL];
				ns.uv = arr[Mesh::ARRAY_TEX_UV];
				ns.uv2 = arr[Mesh::ARRAY_TEX_UV2];
				ns.color = arr[Mesh::ARRAY_COLOR];
				ns.indices = arr[Mesh::ARRAY_INDEX];
				src = &moss_sources.insert(kv.key, ns)->value;
			}
			RockMoss::Job *job = memnew(RockMoss::Job);
			job->source = *src;
			job->params.xform = xf;
			job->params.custom = cd;
			job->params.moss_cover = float(moss_cover) * age_cover;
			const Ref<ShaderMaterial> mat = _material(key_type(lk), RockGenerator::Family(key_family(lk)));
			if (mat.is_valid()) {
				const Variant pd = mat->get_shader_parameter("pole_direction");
				if (pd.get_type() == Variant::VECTOR3) {
					job->params.pole_direction = pd;
				}
				const Variant as = mat->get_shader_parameter("aspect_strength");
				if (as.get_type() == Variant::FLOAT) {
					job->params.aspect_strength = float(as);
				}
				const Variant ch = mat->get_shader_parameter("contact_height");
				if (ch.get_type() == Variant::FLOAT) {
					job->params.contact_height = float(ch);
				}
			}
			MossShell ms;
			ms.job = job;
			ms.task = WorkerThreadPool::get_singleton()->add_native_task(&RockMoss::build, job, false, "RockField3D moss");
			moss_shells.insert(key, ms);
			building++;
		}
	}
	// Shells of stones no longer close go (a building one when it finishes).
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		if (!kv.value.wanted && !kv.value.job) {
			if (kv.value.node) {
				kv.value.node->queue_free();
			}
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		moss_shells.erase(k);
	}
}

void RockField3D::_finish_moss(bool p_wait) {
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		MossShell &ms = kv.value;
		if (!ms.job) {
			continue;
		}
		if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(ms.task)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(ms.task);
		RockMoss::Job *job = ms.job;
		ms.job = nullptr;
		ms.task = WorkerThreadPool::INVALID_TASK_ID;
		if (!ms.wanted) {
			drop.push_back(kv.key);
		} else if (job->triangles > 0 && is_inside_tree()) {
			Ref<ArrayMesh> mesh;
			mesh.instantiate();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, job->arrays);
			mesh->surface_set_material(0, RockMoss::get_material());
			if (!job->shoot_arrays.is_empty()) {
				mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, job->shoot_arrays);
				mesh->surface_set_material(1, RockMoss::get_shoot_material());
			}
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_mesh(mesh);
			mi->set_meta("rock_moss", true);
			add_child(mi, false, INTERNAL_MODE_BACK);
			mi->set_as_top_level(true);
			mi->set_global_transform(job->params.xform);
			ms.node = mi;
			ms.triangles = job->triangles;
		}
		memdelete(job);
	}
	for (const Vector3i &k : drop) {
		moss_shells.erase(k);
	}
}

void RockField3D::_clear_moss() {
	for (KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		if (kv.value.job) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value.task);
			memdelete(kv.value.job);
		}
		if (kv.value.node) {
			kv.value.node->queue_free();
		}
	}
	moss_shells.clear();
}

void RockField3D::set_moss_cushions(bool p_enabled) {
	moss_cushions = p_enabled;
	if (!moss_cushions) {
		_clear_moss();
	}
	close_dirty = true;
}

int RockField3D::get_moss_shell_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		n += kv.value.node ? 1 : 0;
	}
	return n;
}

int RockField3D::get_moss_triangle_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		n += kv.value.triangles;
	}
	return n;
}

int RockField3D::get_moss_pending() const {
	int n = 0;
	for (const KeyValue<Vector3i, MossShell> &kv : moss_shells) {
		n += kv.value.job ? 1 : 0;
	}
	return n;
}

int RockField3D::get_close_up_shape_count() const {
	int n = 0;
	for (const KeyValue<uint32_t, Library> &kv : libraries) {
		for (uint8_t st : kv.value.close_state) {
			n += st == 2 ? 1 : 0;
		}
	}
	return n;
}

int RockField3D::get_close_up_triangle_count() const {
	int t = 0;
	for (const KeyValue<uint32_t, Library> &kv : libraries) {
		for (const Ref<ArrayMesh> &m : kv.value.close) {
			if (m.is_valid() && m->get_surface_count() > 0) {
				t += m->surface_get_array_index_len(0) / 3;
			}
		}
	}
	return t;
}

void RockField3D::set_close_up_enabled(bool p_enabled) {
	close_up_enabled = p_enabled;
	if (!close_up_enabled) {
		for (KeyValue<Vector3i, Cell> &kv : cells) {
			for (Group &grp : kv.value.groups) {
				for (uint32_t k = 0; k < grp.near.size(); k++) {
					if (grp.near[k]) {
						grp.near[k] = false;
						grp.multimesh->set_instance_transform(int(k), grp.xforms[k]);
					}
				}
			}
		}
		_clear_close_up();
	}
	close_dirty = true;
}

/* Breaking */

bool RockField3D::is_breaking_supported() const {
	return ClassDB::class_exists("PhysXDestructible3D") && ClassDB::class_exists("PhysXBlastAuthoring");
}

void RockField3D::_start_fracture(uint64_t p_key) {
	Fracture &f = fractures[p_key];
	if (f.state != 0) {
		return;
	}
	const uint32_t lk = uint32_t(p_key >> 16);
	LibraryJob *job = memnew(LibraryJob);
	job->type = key_type(lk);
	job->family = key_family(lk);
	const int slot = int(p_key & 0xFFFF);
	job->seed = seed * 1000 + slot / RF_MARK_BINS;
	job->detail = float(detail);
	job->fracture = break_pieces;
	job->marks_scale = float(RockGenerator::get_family_metres_per_unit(job->family) * Math::pow(real_t(2.0), real_t(slot % RF_MARK_BINS - RF_MARK_BINS / 2)));
	if (job->family == RockGenerator::FAMILY_COBBLE) {
		job->transport = RockGenerator::get_lithology(job->type).abrasion_length * cobble_bins[CLAMP(key_bin(lk), 0, 2)];
	}
	f.job = job;
	f.task = WorkerThreadPool::get_singleton()->add_native_task(&fracture_job, job, false, "RockField3D fracture");
	f.state = 1;
}

// The finished pieces as a PhysXBlastAsset (pieces that came out empty are dropped with their bonds).
bool RockField3D::_finish_fracture(uint64_t p_key, bool p_wait) {
	Fracture *f = fractures.getptr(p_key);
	if (!f || f->state != 1) {
		return f && f->state == 2;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(f->task)) {
		return false;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(f->task);
	LibraryJob *job = static_cast<LibraryJob *>(f->job);
	f->job = nullptr;
	f->state = 3;
	if (job->asset.is_valid()) {
		f->asset = job->asset;
		f->volume = job->volume;
		f->state = 2;
	}
	memdelete(job);
	return f->state == 2;
}

// Shapes of the stones near the target are fractured ahead on a worker, so a hit breaks them at once.
void RockField3D::_update_breakable() {
	for (KeyValue<uint64_t, Fracture> &kv : fractures) {
		if (kv.value.state == 1) {
			_finish_fracture(kv.key, false);
		}
	}
	if (break_prepare_radius <= 0.0f || !is_breaking_supported()) {
		return;
	}
	const Vector3 tp = _target_position();
	int building = 0;
	for (const KeyValue<uint64_t, Fracture> &kv : fractures) {
		building += kv.value.state == 1 ? 1 : 0;
	}
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (building >= 1) {
			return; // one at a time: it shares the workers with the close-up shapes
		}
		if (_is_detail(kv.key.x)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(kv.key.x));
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		const Vector2 nearest(CLAMP(tp.x, rect.position.x, rect.get_end().x), CLAMP(tp.z, rect.position.y, rect.get_end().y));
		if (nearest.distance_to(Vector2(tp.x, tp.z)) > break_prepare_radius + 8.0f) {
			continue;
		}
		for (const Group &grp : kv.value.groups) {
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				const uint64_t key = (uint64_t(grp.library) << 16) | uint64_t(rf_mark_slot(grp.library, grp.variant, grp.custom[k].b));
				const Fracture *f = fractures.getptr(key);
				if (f && f->state != 0) {
					continue;
				}
				if (!grp.broken[k] && grp.shapes[k] && grp.xforms[k].origin.distance_to(tp) - grp.reach[k] < break_prepare_radius) {
					_start_fracture(key);
					building++;
					break;
				}
			}
			if (building >= 1) {
				break;
			}
		}
	}
}

Node *RockField3D::break_stone(const Vector3 &p_position, real_t p_damage, real_t p_radius) {
	ERR_FAIL_COND_V_MSG(!is_breaking_supported(), nullptr, "RockField3D: breaking stones needs the godot_physx module (PhysX Blast).");
	ERR_FAIL_COND_V(!is_inside_tree(), nullptr);
	// The stone whose surface comes nearest the point (reach as a sphere; inside it counts as zero).
	Cell *best_cell = nullptr;
	int best_group = -1, best_k = -1;
	real_t best = 1e30f;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(kv.key.x));
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		if (!rect.grow(cs).has_point(Vector2(p_position.x, p_position.z))) {
			continue;
		}
		for (uint32_t g = 0; g < kv.value.groups.size(); g++) {
			const Group &grp = kv.value.groups[g];
			if (key_family(grp.library) >= RockGenerator::FAMILY_SEA_STACK) {
				continue; // bedrock, not a stone to break
			}
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				if (grp.broken[k]) {
					continue;
				}
				const real_t d = MAX(grp.xforms[k].origin.distance_to(p_position) - grp.reach[k], real_t(0.0)) - grp.reach[k] * 0.001f;
				if (d < best) {
					best = d;
					best_cell = &kv.value;
					best_group = int(g);
					best_k = int(k);
				}
			}
		}
	}
	if (!best_cell || best > p_radius) {
		return nullptr;
	}
	Group &grp = best_cell->groups[best_group];
	const uint64_t key = (uint64_t(grp.library) << 16) | uint64_t(rf_mark_slot(grp.library, grp.variant, grp.custom[best_k].b));
	_start_fracture(key);
	if (!_finish_fracture(key, true)) {
		return nullptr;
	}
	Node3D *node = _spawn_breakable(grp, best_k, key, false);
	if (node) {
		node->call("apply_radial_damage", p_position, p_damage, p_radius * 0.5f, p_radius);
	}
	return node;
}

// Swap stone p_k of p_grp for a PhysXDestructible3D of its pieces (its fracture p_key must be ready). A struck stone
// keeps its bottom pieces where they sit; a fall comes loose whole, out of the wall, and breaks where it lands.
Node3D *RockField3D::_spawn_breakable(Group &p_grp, int p_k, uint64_t p_key, bool p_fall) {
	const Fracture *f = fractures.getptr(p_key);
	if (!f || f->state != 2) {
		return nullptr;
	}
	Object *obj = ClassDB::instantiate("PhysXDestructible3D");
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node) {
		if (obj) {
			memdelete(obj);
		}
		return nullptr;
	}
	Transform3D xf = p_grp.xforms[p_k];
	const Color cd = p_grp.custom[p_k];
	// Its own copy of the rock material, with the stone's instance data (ground, film, scale, seed), so the copy
	// shades exactly like the stone it replaces.
	Ref<ShaderMaterial> mat = _material(key_type(p_grp.library), RockGenerator::Family(key_family(p_grp.library)));
	if (mat.is_valid()) {
		mat = mat->duplicate();
		mat->set_shader_parameter("rock_instance", Vector4(cd.r, cd.g, cd.b, cd.a));
	}
	const real_t s = xf.basis.get_scale().x;
	const real_t mass = MAX(real_t(f->volume) * s * s * s * break_density, real_t(0.01));
	node->set("blast_asset", f->asset);
	node->set("material_override", mat);
	node->set("auto_mass", false);
	node->set("mass", mass);
	if (p_fall) {
		// A block loosed from the wall: a rigid body from the start, moved out of the face by its reach so it does
		// not start inside the heightfield. It breaks where it hits hard: one contact taking the impulse of a 3 m/s
		// change of its whole speed (an impact shares itself over several contacts; sliding on the face a contact
		// takes a few percent of that). In the bench a 21 t block slides about 11 m down a 60 degree face, breaks in
		// six at the first hard knock and in ten further down. Harder hits break it further (fragmentation grows with
		// impact energy, Ruiz-Carulla et al. 2015).
		Vector3 out(0, 1, 0);
		if (TerrainGenerator3D *gen = _get_generator()) {
			const Vector3 c = xf.origin;
			const real_t e = 1.0;
			const real_t gx = (gen->get_height(c + Vector3(e, 0, 0)) - gen->get_height(c - Vector3(e, 0, 0))) / (2.0 * e);
			const real_t gz = (gen->get_height(c + Vector3(0, 0, e)) - gen->get_height(c - Vector3(0, 0, e))) / (2.0 * e);
			out = Vector3(-gx, 1, -gz).normalized();
			node->set("kill_y", gen->get_height(c) - 400.0);
		}
		xf.origin += out * (p_grp.reach[p_k] * 1.05f + 0.05f); // clear of the face plane: reach is its farthest surface point
		node->set("dynamic", true);
		node->set("anchor", 2); // ANCHOR_NONE
		node->set("impact_strength", mass * 3.0);
		node->set("impact_damage_scale", 1.0 / mass);
		node->set("impact_radius", MAX(p_grp.reach[p_k] * 1.5f, real_t(0.5)));
		node->set("shatter_speed", 1.5);
		node->set("freeze_when_asleep", true);
	} else {
		// The bottom pieces stay where they are: a stone sits sunk in the ground (or in a wall or a tor), and a piece
		// that starts inside the terrain would be pushed down through it rather than out.
		node->set("anchor", 1); // ANCHOR_GROUNDED
		node->set("buried_below_y", cd.r); // the ground under the stone
		// Struck stone falls apart rather than exploding: pieces leave at a couple of metres a second at most.
		node->set("shatter_speed", 2.5);
	}
	// Placed before it enters the tree: it reads its scale (the stone's) when it spawns its intact piece.
	node->set_as_top_level(true);
	node->set_transform(xf);
	add_child(node, false, INTERNAL_MODE_BACK);
	breakables.push_back(node->get_instance_id());
	// Hide the stone and drop its collision.
	const Transform3D orig = p_grp.xforms[p_k];
	p_grp.broken[p_k] = true;
	p_grp.multimesh->set_instance_transform(p_k, Transform3D(Basis().scaled(Vector3(0, 0, 0)), orig.origin));
	if (CollisionShape3D *shape = Object::cast_to<CollisionShape3D>(p_grp.shapes[p_k])) {
		shape->set_disabled(true);
	}
	broken_stones.insert(Vector3i((orig.origin * 100.0f).round()));
	close_dirty = true;
	if (p_fall) {
		falls.push_back(node->get_instance_id());
		while (int(falls.size()) > rockfall_max) {
			if (Node *old = Object::cast_to<Node>(ObjectDB::get_instance(falls[0]))) {
				old->queue_free();
			}
			falls.remove_at(0);
		}
		emit_signal(SNAME("rockfall"), xf.origin, mass);
	}
	return node;
}

/* Terrain3D */

// A hole in a Terrain3D's control map over a cave's box (Terrain3D 1.x, called by name so the engine builds without
// it): every vertex whose ground lies in the box gets the hole bit (Terrain3DData.set_control_hole), then the control
// map goes to the GPU and the collision is updated. The ground tested is the generator's, which the Terrain3D's heights
// are taken from.
void RockField3D::_terrain3d_hole(const Transform3D &p_box) {
	if (terrain3d_path.is_empty() || !is_inside_tree()) {
		return;
	}
	Node *t3d = get_node_or_null(terrain3d_path);
	const TerrainGenerator3D *gen = _get_generator();
	if (!t3d || !gen || !t3d->has_method("get_data")) {
		return;
	}
	Object *data = Object::cast_to<Object>(t3d->call("get_data"));
	if (!data || !data->has_method("set_control_hole")) {
		return;
	}
	const real_t step = MAX(real_t(t3d->get("vertex_spacing")), real_t(0.05));
	Vector2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
	for (int k = 0; k < 8; k++) {
		const Vector3 c = p_box.xform(Vector3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1));
		lo = lo.min(Vector2(c.x, c.z));
		hi = hi.max(Vector2(c.x, c.z));
	}
	// Terrain3D's own heights where it has them (Terrain3DData.get_height), else the generator's. A vertex is a hole
	// where the ground round it (half a spacing each way, so a steep wall's span) passes through the box.
	const bool own = data->has_method("get_height");
	auto height = [&](real_t p_x, real_t p_z) -> real_t {
		if (own) {
			const real_t h = data->call("get_height", Vector3(p_x, 0, p_z));
			if (!Math::is_nan(h)) {
				return h;
			}
		}
		return gen->get_height(Vector3(p_x, 0, p_z));
	};
	int written = 0;
	const Transform3D inv = p_box.affine_inverse();
	for (real_t z = Math::floor(lo.y / step) * step; z <= hi.y; z += step) {
		for (real_t x = Math::floor(lo.x / step) * step; x <= hi.x; x += step) {
			real_t y0 = 1e30, y1 = -1e30;
			for (int k = 0; k < 5; k++) {
				const real_t hx = x + (k == 1 ? -0.5 : (k == 2 ? 0.5 : 0.0)) * step, hz = z + (k == 3 ? -0.5 : (k == 4 ? 0.5 : 0.0)) * step;
				const real_t hy = height(hx, hz);
				y0 = MIN(y0, hy);
				y1 = MAX(y1, hy);
			}
			// The column from y0 to y1 against the box, in the box's unit frame (slab test).
			const Vector3 a = inv.xform(Vector3(x, y0, z)), d = inv.xform(Vector3(x, y1, z)) - a;
			real_t t0 = 0.0, t1 = 1.0;
			for (int k = 0; k < 3 && t0 <= t1; k++) {
				if (Math::abs(d[k]) < 1e-9f) {
					if (Math::abs(a[k]) > 1.0f) {
						t0 = 2.0;
					}
					continue;
				}
				real_t ta = (-1.0f - a[k]) / d[k], tb = (1.0f - a[k]) / d[k];
				if (ta > tb) {
					SWAP(ta, tb);
				}
				t0 = MAX(t0, ta);
				t1 = MIN(t1, tb);
			}
			if (t0 <= t1) {
				data->call("set_control_hole", Vector3(x, 0.5 * (y0 + y1), z), true);
				written++;
			}
		}
	}
	if (written == 0) {
		return;
	}
	if (data->has_method("update_maps")) {
		data->call("update_maps", 1); // Terrain3DRegion.TYPE_CONTROL
	}
	Object *col = t3d->has_method("get_collision") ? Object::cast_to<Object>(t3d->call("get_collision")) : nullptr;
	if (col && col->has_method("update")) {
		col->call("update");
	}
}

// Writes every cave hole the generator holds into the Terrain3D (for one set after the caves were placed); returns how
// many holes.
int RockField3D::write_terrain3d_holes() {
	const TerrainGenerator3D *gen = _get_generator();
	if (!gen) {
		return 0;
	}
	const TypedArray<Transform3D> holes = gen->get_holes();
	for (int i = 0; i < holes.size(); i++) {
		_terrain3d_hole(holes[i]);
	}
	return holes.size();
}

/* Live rockfall */

// Falls per game hour now, as a multiple of rockfall_rate: 1 as the base, plus 3 in the frost-cracking window
// (-8 to -3 C), plus 6 for two game hours after the air thaws through 0 C, plus the rain rate over 5 mm/h.
real_t RockField3D::get_rockfall_trigger() const {
	const WorldWeather *w = WorldContracts::weather();
	if (!w->has_provider()) {
		return 1.0;
	}
	const Vector3 tp = _target_position();
	real_t t = 1.0;
	const real_t air = _rockfall_air(tp);
	if (air >= -8.0f && air <= -3.0f) {
		t += 3.0;
	}
	if (thaw_left > 0.0) {
		t += 6.0;
	}
	t += MAX(real_t(w->precipitation_at(tp)), real_t(0.0)) / 5.0;
	return t;
}

// The air at the target's height (the weather contract applies the lapse rate).
real_t RockField3D::_rockfall_air(const Vector3 &p_pos) const {
	const WorldWeather *w = WorldContracts::weather();
	if (!w->has_provider()) {
		return 10.0;
	}
	return w->air_temperature_at(p_pos);
}

bool RockField3D::_find_stone(const Vector3 &p_origin, Cell *&r_cell, int &r_group, int &r_k) {
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (kv.key.x != LAYER_CLIFF) {
			continue;
		}
		const real_t cs = _cell_size(LAYER_CLIFF);
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		if (!rect.grow(1.0).has_point(Vector2(p_origin.x, p_origin.z))) {
			continue;
		}
		for (uint32_t g = 0; g < kv.value.groups.size(); g++) {
			const Group &grp = kv.value.groups[g];
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				if (!grp.broken[k] && grp.xforms[k].origin.distance_squared_to(p_origin) < 1e-4f) {
					r_cell = &kv.value;
					r_group = int(g);
					r_k = int(k);
					return true;
				}
			}
		}
	}
	return false;
}

// A random intact cliff block between p_min and p_max metres from p_center (reservoir sampling); false if none.
bool RockField3D::_pick_fall(const Vector3 &p_center, real_t p_min, real_t p_max, Vector3 &r_origin, uint64_t &r_key) {
	int seen = 0;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (kv.key.x != LAYER_CLIFF) {
			continue;
		}
		const real_t cs = _cell_size(LAYER_CLIFF);
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		const Vector2 nearest(CLAMP(p_center.x, rect.position.x, rect.get_end().x), CLAMP(p_center.z, rect.position.y, rect.get_end().y));
		if (nearest.distance_to(Vector2(p_center.x, p_center.z)) > p_max) {
			continue;
		}
		for (const Group &grp : kv.value.groups) {
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				if (grp.broken[k]) {
					continue;
				}
				const real_t d = grp.xforms[k].origin.distance_to(p_center);
				if (d < p_min || d > p_max) {
					continue;
				}
				seen++;
				if (fall_rng.rand() % uint32_t(seen) == 0) {
					r_origin = grp.xforms[k].origin;
					r_key = (uint64_t(grp.library) << 16) | uint64_t(rf_mark_slot(grp.library, grp.variant, grp.custom[k].b));
				}
			}
		}
	}
	return seen > 0;
}

void RockField3D::_update_rockfall(double p_delta) {
	if (!is_breaking_supported()) {
		return;
	}
	const WeatherMap3D *w = WeatherMap3D::get_active();
	const double dt = p_delta * (w ? double(w->get_time_scale()) : 1.0); // game seconds
	const Vector3 tp = _target_position();
	const real_t air = _rockfall_air(tp);
	if (last_air < 0.0f && air >= 0.0f) {
		thaw_left = 7200.0;
	}
	last_air = air;
	thaw_left = MAX(thaw_left - dt, 0.0);
	// The falls whose fracture is ready leave the wall now (the block may have streamed out meanwhile).
	for (uint32_t i = 0; i < pending_falls.size();) {
		const PendingFall pf = pending_falls[i];
		if (!_finish_fracture(pf.key, false)) {
			const Fracture *f = fractures.getptr(pf.key);
			if (f && f->state == 1) {
				i++;
				continue;
			}
		} else {
			Cell *cell = nullptr;
			int g = -1, k = -1;
			if (_find_stone(pf.origin, cell, g, k)) {
				_spawn_breakable(cell->groups[g], k, pf.key, true);
			}
		}
		pending_falls.remove_at(i);
	}
	// A Poisson process: the chance of at least one fall in this step.
	const double rate = double(rockfall_rate) * double(get_rockfall_trigger()) / 3600.0;
	if (pending_falls.size() >= 2 || fall_rng.randf() >= float(1.0 - Math::exp(-rate * dt))) {
		return;
	}
	PendingFall pf;
	if (_pick_fall(tp, MIN(real_t(20.0), rockfall_radius * 0.5f), rockfall_radius, pf.origin, pf.key)) {
		_start_fracture(pf.key);
		pending_falls.push_back(pf);
	}
}

Node *RockField3D::trigger_rockfall(const Vector3 &p_position, real_t p_radius) {
	ERR_FAIL_COND_V_MSG(!is_breaking_supported(), nullptr, "RockField3D: rockfall needs the godot_physx module (PhysX Blast).");
	ERR_FAIL_COND_V(!is_inside_tree(), nullptr);
	Vector3 origin;
	uint64_t key = 0;
	if (!_pick_fall(p_position, 0.0, p_radius, origin, key)) {
		return nullptr;
	}
	_start_fracture(key);
	if (!_finish_fracture(key, true)) {
		return nullptr;
	}
	Cell *cell = nullptr;
	int g = -1, k = -1;
	if (!_find_stone(origin, cell, g, k)) {
		return nullptr;
	}
	return _spawn_breakable(cell->groups[g], k, key, true);
}

void RockField3D::set_live_rockfall(bool p_enabled) {
	live_rockfall = p_enabled;
	if (is_inside_tree()) {
		_update_process();
	}
}

void RockField3D::_update_process() {
	set_process_internal(!Engine::get_singleton()->is_editor_hint() && (streaming || live_rockfall || ground_contact));
	set_physics_process_internal(!Engine::get_singleton()->is_editor_hint() && rockfall_dust);
}

/* Rockfall dust */

void RockField3D::set_rockfall_dust(bool p_enabled) {
	if (rockfall_dust == p_enabled) {
		return;
	}
	rockfall_dust = p_enabled;
	if (!rockfall_dust) {
		dust_tracks.clear();
		dust_events.clear();
	}
	if (is_inside_tree()) {
		_update_dust_node();
		_update_process();
	}
}

Node *RockField3D::get_rockfall_dust_node() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(dust_node));
}

// The field's own PhysXRockDust3D (godot_physx, built with Flow), called by name so the module builds without it.
void RockField3D::_update_dust_node() {
	Node *n = get_rockfall_dust_node();
	if (!rockfall_dust || Engine::get_singleton()->is_editor_hint()) {
		if (n) {
			n->queue_free();
		}
		dust_node = ObjectID();
		return;
	}
	if (n || !ClassDB::class_exists("PhysXRockDust3D")) {
		return;
	}
	Object *obj = ClassDB::instantiate("PhysXRockDust3D");
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node) {
		if (obj) {
			memdelete(obj);
		}
		return;
	}
	node->set_name("RockfallDust");
	add_child(node, false, INTERNAL_MODE_BACK);
	node->set("rock_field", node->get_path_to(this));
	dust_node = node->get_instance_id();
}

void RockField3D::take_rockfall_dust(LocalVector<Dust> &r_events) {
	r_events = dust_events;
	dust_events.clear();
}

void RockField3D::_add_dust(const Vector3 &p_position, const Vector3 &p_velocity, float p_mass, float p_radius) {
	if (p_mass <= 0.0f) {
		return;
	}
	dust_total += p_mass;
	if (dust_events.size() >= 256) {
		return; // nobody is draining them
	}
	Dust d;
	d.position = p_position;
	d.velocity = p_velocity;
	d.mass = p_mass;
	d.radius = p_radius;
	dust_events.push_back(d);
}

// Every live piece of every broken stone, on the physics tick (its body's own velocity). An impact is a change of
// more than 2.5 m/s in one tick (gravity adds 0.16 m/s at 60 Hz); its fines are the kinetic energy it took out of
// the piece times dust_per_joule. A piece that appears in a stone already tracked broke off: its new faces shed
// 0.01% of its mass. A piece moving over 0.8 m/s with its underside on the ground abrades 2e-6 of its mass per metre.
// Rain on the rock damps it all (wet fines stick): 1 / (1 + rain / 2 mm/h).
void RockField3D::_update_dust(double p_delta) {
	if (!rockfall_dust || p_delta <= 0.0) {
		return;
	}
	if (!get_rockfall_dust_node()) {
		dust_events.clear(); // no Flow to take them: keep only the totals
	}
	dust_tick++;
	const float dt = float(p_delta);
	const WeatherMap3D *w = WeatherMap3D::get_active();
	const TerrainGenerator3D *gen = _get_generator();
	HashSet<uint64_t> live;
	for (const ObjectID &id : breakables) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(id));
		if (!node) {
			continue;
		}
		const uint64_t key = uint64_t(id);
		live.insert(key);
		DustTrack &tr = dust_tracks[key];
		const int n = node->call("get_piece_count");
		for (int i = 0; i < n; i++) {
			const uint32_t chunk = uint32_t(int64_t(node->call("get_piece_chunk", i)));
			const float m = node->call("get_piece_mass", i);
			if (chunk == UINT32_MAX || m <= 0.0f) {
				continue;
			}
			const Vector3 v = node->call("get_piece_linear_velocity", i);
			const Vector3 c = node->call("get_piece_center", i);
			const float r = Math::pow(3.0f * m / (4.0f * float(Math::PI) * float(break_density)), 1.0f / 3.0f);
			const float wet = w ? 1.0f / (1.0f + MAX(float(w->get_rain_at(c)), 0.0f) / 2.0f) : 1.0f;
			const Vector3 under = c - Vector3(0, r * 0.8f, 0);
			PieceTrack *pt = tr.pieces.getptr(chunk);
			if (!pt) {
				if (tr.ticks > 0) {
					_add_dust(c, v, m * 1e-4f * wet, r);
				}
				PieceTrack t;
				t.velocity = v;
				t.tick = dust_tick;
				t.seen = 1;
				tr.pieces.insert(chunk, t);
				continue;
			}
			if (pt->tick + 1 == dust_tick && pt->seen >= 2) {
				const Vector3 dv = v - pt->velocity;
				if (dv.length() > 2.5f) {
					const float lost = 0.5f * m * (pt->velocity.length_squared() - v.length_squared());
					if (lost > 0.0f) {
						_add_dust(under, pt->velocity, lost * float(dust_per_joule) * wet, r);
						dust_impacts++;
					}
				} else if (gen && v.length() > 0.8f && gen->get_height(c) > c.y - r * 1.4f) {
					_add_dust(under, v, m * 2e-6f * v.length() * dt * wet, r);
				}
			}
			pt->velocity = v;
			pt->tick = dust_tick;
			pt->seen++;
		}
		// Pieces gone (merged, killed, freed) leave the track.
		LocalVector<uint32_t> gone;
		for (const KeyValue<uint32_t, PieceTrack> &kv : tr.pieces) {
			if (kv.value.tick != dust_tick) {
				gone.push_back(kv.key);
			}
		}
		for (uint32_t g : gone) {
			tr.pieces.erase(g);
		}
		tr.ticks++;
	}
	LocalVector<uint64_t> dead;
	for (const KeyValue<uint64_t, DustTrack> &kv : dust_tracks) {
		if (!live.has(kv.key)) {
			dead.push_back(kv.key);
		}
	}
	for (uint64_t k : dead) {
		dust_tracks.erase(k);
	}
}

void RockField3D::restore_broken() {
	for (const ObjectID &id : breakables) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
			n->queue_free();
		}
	}
	breakables.clear();
	falls.clear();
	pending_falls.clear();
	if (!broken_stones.is_empty()) {
		broken_stones.clear();
		for (KeyValue<Vector3i, Cell> &kv : cells) {
			_free_cell(kv.value);
		}
		cells.clear();
		stone_count = 0;
	}
}

void RockField3D::rebuild() {
	clear();
	contact_dirty = true;
	auto_ground_found = false;
	if (ground_material.is_null() && is_inside_tree()) {
		for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
			_apply_ground(kv.value);
		}
	}
	restore_broken();
	fractures.clear();
	obstacle_cache.clear();
	rivers_ready = false;
	if (is_inside_tree() && (streaming || live_rockfall)) {
		_update_process();
	}
}

void RockField3D::clear() {
	_detail_cancel();
	_coarse_cancel();
	_collect_libraries(true);
	for (KeyValue<uint64_t, Fracture> &kv : fractures) {
		if (kv.value.state == 1) {
			_finish_fracture(kv.key, true);
		}
	}
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		_free_cell(kv.value);
	}
	cells.clear();
	stone_count = 0;
	_clear_close_up();
}

int RockField3D::build_area(const Rect2 &p_rect) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_watch_holes();
	int built = 0;
	_update_detail_radii();
	const Vector3 tp = _detail_center();
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!_layer_on(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
		const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector3i key(l, x, z);
				if (_is_detail(l)) {
					// Ground detail only within its reach of the target, as streaming keeps it.
					const Rect2 rect(x * cs, z * cs, cs, cs);
					const Vector2 nearest(CLAMP(tp.x, rect.position.x, rect.get_end().x), CLAMP(tp.z, rect.position.y, rect.get_end().y));
					if (nearest.distance_to(Vector2(tp.x, tp.z)) > layer_radius[l]) {
						continue;
					}
				}
				if (!cells.has(key)) {
					_build_cell(key);
					built++;
				}
			}
		}
	}
	last_build_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	// Close-up shapes too, waiting for them (stills and benches want the finished picture): two passes, the first
	// starts the builds, the second places them.
	if (close_up_enabled) {
		for (int pass = 0; pass < 64; pass++) {
			close_dirty = true;
			_update_close_up(true);
			if (close_jobs.is_empty()) {
				close_dirty = true;
				_update_close_up(true);
				if (close_jobs.is_empty() && get_moss_pending() == 0) {
					close_dirty = true;
					_update_close_up(true); // the moss of stones whose shapes the last pass placed
					if (get_moss_pending() == 0) {
						break;
					}
				}
			}
		}
	}
	_remeasure_sea();
	return built;
}

PackedVector4Array RockField3D::get_obstacles(const Rect2 &p_rect, real_t p_min_size) {
	PackedVector4Array out;
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!layer_enabled[l] || l == LAYER_CLIFF || _is_detail(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
		const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				// Placement is deterministic per cell, so each cell is placed once and its obstacles kept.
				const Vector3i key(l, x, z);
				ObstacleCell *oc = obstacle_cache.getptr(key);
				if (!oc) {
					LocalVector<Stone> stones;
					_place(Layer(l), Rect2(x * cs, z * cs, cs, cs), rf_hash3(l, x, z, uint32_t(seed) * 2654435761u), stones);
					ObstacleCell cell;
					for (const Stone &st : stones) {
						if (st.footprint > 0.0f) {
							cell.obstacles.push_back(Vector4(st.position.x, st.ground, st.position.z, st.footprint));
							cell.sizes.push_back(2.0f * st.scale);
						}
					}
					oc = &obstacle_cache.insert(key, std::move(cell))->value;
				}
				for (uint32_t k = 0; k < oc->obstacles.size(); k++) {
					const Vector4 &o = oc->obstacles[k];
					if (oc->sizes[k] >= p_min_size && p_rect.has_point(Vector2(o.x, o.z))) {
						out.push_back(o);
					}
				}
			}
		}
	}
	return out;
}

Array RockField3D::get_stones(int p_layer, const Rect2 &p_rect) {
	Array out;
	ERR_FAIL_INDEX_V(p_layer, LAYER_MAX, out);
	const real_t cs = _cell_size(Layer(p_layer));
	const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
	const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			LocalVector<Stone> stones;
			_place(Layer(p_layer), Rect2(x * cs, z * cs, cs, cs), rf_hash3(p_layer, x, z, uint32_t(seed) * 2654435761u), stones);
			for (const Stone &st : stones) {
				if (!p_rect.has_point(Vector2(st.position.x, st.position.z))) {
					continue;
				}
				Dictionary d;
				d["position"] = st.position;
				d["basis"] = st.basis;
				d["scale"] = st.scale;
				d["ground"] = st.ground;
				d["footprint"] = st.footprint;
				d["film"] = st.film;
				d["rock_type"] = int(key_type(st.library));
				d["family"] = int(key_family(st.library));
				d["bin"] = key_bin(st.library);
				d["variant"] = int(st.variant);
				d["layer"] = p_layer;
				// Highest point of the stone as placed: the support of its hull straight up (the hull holds the mesh's
				// extreme vertices, so this is the mesh's own top, not the box of its axes).
				real_t top = st.position.y + st.scale;
				const Library *lib = libraries.getptr(st.library);
				if (lib && st.variant < lib->hulls.size() && !lib->hulls[st.variant].is_empty()) {
					const PackedVector3Array &hull = lib->hulls[st.variant];
					real_t best = -1e20;
					for (int k = 0; k < hull.size(); k++) {
						best = MAX(best, st.basis.rows[1].dot(hull[k]));
					}
					top = st.position.y + best;
				} else if (lib && st.variant < lib->axes.size()) {
					const Vector3 ax = lib->axes[st.variant];
					top = st.position.y + Math::abs(st.basis.rows[1].x) * ax.x + Math::abs(st.basis.rows[1].y) * ax.y + Math::abs(st.basis.rows[1].z) * ax.z;
				}
				d["top"] = top;
				out.push_back(d);
			}
		}
	}
	return out;
}

// A stone's cap, where moss, snow and litter settle: from the library (base) mesh's surface samples placed by the
// stone's basis. The cap is the largest connected patch of up-facing surface (normals within about 45 degrees of
// up), not the region round the highest point, which on a tilted block is a corner or an edge. Its centre is the
// patch's mean, its normal the patch's mean normal, and its flat radius how far out from the centre the surface
// stays within `p_tolerance` metres of that plane.
Array RockField3D::get_stone_caps(int p_layer, const Rect2 &p_rect, real_t p_tolerance) {
	Array out = get_stones(p_layer, p_rect);
	LocalVector<Vector3> wp, wn;
	LocalVector<int> up, comp, queue;
	HashMap<Vector3i, LocalVector<int>> grid;
	for (int i = 0; i < out.size(); i++) {
		Dictionary d = out[i];
		const uint32_t key = _library_key(RockGenerator::RockType(int(d["rock_type"])), RockGenerator::Family(int(d["family"])), int(d["bin"]));
		const Library *lib = _ensure_library(key);
		const int v = d["variant"];
		const Vector3 pos = d["position"];
		const Basis b = d["basis"];
		const real_t scale = d["scale"];
		Vector3 cap_point = Vector3(pos.x, real_t(d["top"]), pos.z);
		Vector3 cap_normal(0, 1, 0);
		real_t cap_radius = 0.0;
		if (lib && v < int(lib->cap_points.size()) && !lib->cap_points[v].is_empty()) {
			const LocalVector<Vector3> &cp = lib->cap_points[v];
			const LocalVector<Vector3> &cn = lib->cap_normals[v];
			const Basis rot = b.orthonormalized();
			wp.resize(cp.size());
			wn.resize(cp.size());
			up.clear();
			for (uint32_t k = 0; k < cp.size(); k++) {
				wp[k] = pos + b.xform(cp[k]);
				wn[k] = rot.xform(cn[k]);
				if (wn[k].y > 0.7f) {
					up.push_back(int(k));
				}
			}
			if (!up.is_empty()) {
				// Connected patches: neighbours within one and a half sample spacings, found through a grid.
				const real_t link = 0.06f * 1.5f * scale;
				grid.clear();
				for (int k : up) {
					grid[Vector3i((wp[k] / link).floor())].push_back(k);
				}
				comp.resize(cp.size());
				for (int &c : comp) {
					c = -1;
				}
				int best = -1, best_count = 0;
				for (int seed_k : up) {
					if (comp[seed_k] >= 0) {
						continue;
					}
					int count = 0;
					queue.clear();
					queue.push_back(seed_k);
					comp[seed_k] = seed_k;
					for (uint32_t qi = 0; qi < queue.size(); qi++) {
						const int k = queue[qi];
						count++;
						const Vector3i g = Vector3i((wp[k] / link).floor());
						for (int dz = -1; dz <= 1; dz++) {
							for (int dy = -1; dy <= 1; dy++) {
								for (int dx = -1; dx <= 1; dx++) {
									const LocalVector<int> *cell = grid.getptr(g + Vector3i(dx, dy, dz));
									if (!cell) {
										continue;
									}
									for (int o : *cell) {
										if (comp[o] < 0 && wp[o].distance_to(wp[k]) < link) {
											comp[o] = seed_k;
											queue.push_back(o);
										}
									}
								}
							}
						}
					}
					if (count > best_count) {
						best_count = count;
						best = seed_k;
					}
				}
				Vector3 sum, nsum;
				for (int k : up) {
					if (comp[k] == best) {
						sum += wp[k];
						nsum += wn[k];
					}
				}
				cap_point = sum / real_t(best_count);
				cap_normal = nsum.normalized();
				// Onto the surface: the mean of a curved patch lies inside the stone, so lift it to the height (along
				// the normal) of the patch sample nearest it across the plane.
				real_t nearest = 1e20;
				real_t lift = 0.0;
				for (int k : up) {
					if (comp[k] != best) {
						continue;
					}
					const Vector3 rel = wp[k] - cap_point;
					const real_t off = rel.dot(cap_normal);
					const real_t across = (rel - cap_normal * off).length_squared();
					if (across < nearest) {
						nearest = across;
						lift = off;
					}
				}
				cap_point += cap_normal * lift;
				// The flat radius: the nearest up-facing sample off the plane by more than the tolerance, within
				// the reach of the patch.
				real_t r = 1e20;
				real_t reach = 0.0;
				for (uint32_t k = 0; k < wp.size(); k++) {
					if (wn[k].dot(cap_normal) <= 0.2f) {
						continue;
					}
					const Vector3 rel = wp[k] - cap_point;
					const real_t off = rel.dot(cap_normal);
					const real_t across = (rel - cap_normal * off).length();
					if (Math::abs(off) > p_tolerance) {
						r = MIN(r, across);
					} else if (comp[k] == best) {
						reach = MAX(reach, across);
					}
				}
				cap_radius = MIN(r, reach + 0.03f * scale);
			}
		}
		d["cap_point"] = cap_point;
		d["cap_normal"] = cap_normal;
		d["cap_radius"] = cap_radius;
	}
	return out;
}

int RockField3D::get_library_triangle_count() const {
	int t = 0;
	for (const KeyValue<uint32_t, Library> &kv : libraries) {
		for (const Ref<ArrayMesh> &m : kv.value.meshes) {
			if (m.is_valid() && m->get_surface_count() > 0) {
				t += m->surface_get_array_index_len(0) / 3;
			}
		}
	}
	return t;
}

/* Properties */

void RockField3D::set_generator(const NodePath &p_path) {
	generator_path = p_path;
	rebuild();
}

void RockField3D::set_seed(int p_seed) {
	seed = p_seed;
	clear(); // workers read the libraries
	libraries.clear();
	rebuild();
}

void RockField3D::set_rock_type(RockGenerator::RockType p_type) {
	rock_type = RockGenerator::RockType(CLAMP(int(p_type), 0, int(RockGenerator::ROCK_MAX) - 1));
	rebuild();
}

void RockField3D::set_soft_rock_type(RockGenerator::RockType p_type) {
	soft_rock_type = RockGenerator::RockType(CLAMP(int(p_type), 0, int(RockGenerator::ROCK_MAX) - 1));
	rebuild();
}

void RockField3D::set_erratic_rock_type(RockGenerator::RockType p_type) {
	erratic_rock_type = RockGenerator::RockType(CLAMP(int(p_type), 0, int(RockGenerator::ROCK_MAX) - 1));
	rebuild();
}

void RockField3D::set_use_strata(bool p_use) {
	use_strata = p_use;
	rebuild();
}

void RockField3D::set_variants(int p_variants) {
	variants = CLAMP(p_variants, 1, 32);
	clear(); // workers read the libraries
	libraries.clear();
	rebuild();
}

void RockField3D::set_detail(real_t p_detail) {
	detail = CLAMP(p_detail, real_t(0.0), real_t(2.0));
	clear(); // workers read the libraries
	libraries.clear();
	rebuild();
}

void RockField3D::set_lichen_cover(real_t p_cover) {
	lichen_cover = CLAMP(p_cover, real_t(0.0), real_t(1.0));
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		kv.value->set_shader_parameter("lichen_cover", lichen_cover);
	}
}

// The material the blend band takes its ground colours from: ground_material when set, else the TerrainMaterial3D
// of the Landscape3D that draws this field's generator (any landscape's when the field has no generator), so the
// blend matches the ground without any wiring. Looked up once per rebuild.
Ref<ShaderMaterial> RockField3D::_ground_source() const {
	if (ground_material.is_valid()) {
		return ground_material;
	}
	if (auto_ground_found || !is_inside_tree()) {
		return auto_ground;
	}
	auto_ground_found = true;
	auto_ground.unref();
	const Node *gen = get_node_or_null(generator_path);
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "Landscape3D", true, false);
	for (int i = 0; i < found.size(); i++) {
		const Landscape3D *land = Object::cast_to<Landscape3D>(found[i]);
		if (!land) {
			continue;
		}
		const Ref<ShaderMaterial> m = land->get_material();
		if (m.is_null() || !m->is_class("TerrainMaterial3D")) {
			continue;
		}
		const Node *src = land->get_node_or_null(land->get_source());
		if (gen && src == gen) {
			auto_ground = m;
			break;
		}
		if (auto_ground.is_null()) {
			auto_ground = m;
		}
	}
	return auto_ground;
}

// The ground's layer colours for the blend band: a TerrainMaterial3D's tm_<layer>_color times the mean of its albedo
// texture (the texture's detail averages out over the band), as linear albedo. Without a ground material, the
// terrain material's own defaults.
void RockField3D::_apply_ground(const Ref<ShaderMaterial> &p_material) const {
	_sync_scree_blocks();
	static const char *names[5] = { "rock", "scree", "soil", "alluvium", "sand" };
	static const Color defaults[5] = { Color(0.36, 0.35, 0.33), Color(0.41, 0.40, 0.37), Color(0.2, 0.22, 0.1), Color(0.28, 0.27, 0.16), Color(0.7, 0.62, 0.46) };
	p_material->set_shader_parameter("ground_blend", ground_blend);
	p_material->set_shader_parameter("mineral_glints", mineral_glints);
	p_material->set_shader_parameter("ground_blend_height", ground_blend_height);
	const Ref<ShaderMaterial> ground = _ground_source();
	for (int i = 0; i < 5; i++) {
		Color c = defaults[i];
		if (ground.is_valid()) {
			const Variant v = ground->get_shader_parameter(StringName(String("tm_") + names[i] + "_color"));
			if (v.get_type() == Variant::COLOR) {
				c = v;
			}
			const Ref<Texture2D> tex = ground->get_shader_parameter(StringName(String("tm_") + names[i] + "_albedo_tex"));
			if (tex.is_valid()) {
				Ref<Image> img = tex->get_image();
				if (img.is_valid() && !img->is_empty()) {
					img = img->duplicate();
					if (img->is_compressed()) {
						img->decompress();
					}
					if (!img->is_compressed()) {
						img->convert(Image::FORMAT_RGBA8);
						img->resize(8, 8, Image::INTERPOLATE_BILINEAR);
						Color sum(0, 0, 0, 0);
						for (int y = 0; y < 8; y++) {
							for (int x = 0; x < 8; x++) {
								sum += img->get_pixel(x, y).srgb_to_linear();
							}
						}
						const Color mean = (sum / 64.0f).linear_to_srgb();
						c = Color(c.r * mean.r, c.g * mean.g, c.b * mean.b);
					}
				}
			}
			const Variant aa = ground->get_shader_parameter("tm_alluvium_area");
			if (aa.get_type() == Variant::FLOAT) {
				p_material->set_shader_parameter("ground_alluvium_area", aa);
			}
		}
		const Color lin = c.srgb_to_linear();
		p_material->set_shader_parameter(StringName(String("ground_") + names[i]), Vector3(lin.r, lin.g, lin.b));
	}
}

void RockField3D::_remeasure_sea() {
	if (!coast_seabed_dirty || !is_inside_tree()) {
		return;
	}
	coast_seabed_dirty = false;
	TerrainGenerator3D *gen = _get_generator();
	Node *sea = gen ? gen->get_sea_node() : nullptr;
	if (sea && sea->has_method("remeasure_seabed") && bool(sea->get("seabed_from_floor"))) {
		sea->call("remeasure_seabed");
	}
}

void RockField3D::set_mineral_glints(bool p_enabled) {
	mineral_glints = p_enabled;
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		_apply_ground(kv.value);
	}
}

void RockField3D::set_coast_collision(bool p_enabled) {
	coast_collision = p_enabled;
	rebuild();
}

void RockField3D::set_cave_mouths(bool p_enabled) {
	cave_mouths = p_enabled;
	rebuild();
}

void RockField3D::set_ground_blend(bool p_enabled) {
	ground_blend = p_enabled;
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		_apply_ground(kv.value);
	}
}

void RockField3D::set_ground_blend_height(real_t p_height) {
	ground_blend_height = MAX(p_height, real_t(0.0));
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		_apply_ground(kv.value);
	}
}

// Inside the scree layer's radius the real stones lie on the talus, so the ground's own parallax scree blocks go
// (TerrainMaterial3D.scree_blocks_distance), leaving its gravel bed under them.
void RockField3D::_sync_scree_blocks() const {
	const Ref<ShaderMaterial> g = _ground_source();
	if (g.is_null()) {
		return;
	}
	bool has = false;
	g->get("scree_blocks_distance", &has);
	if (has) {
		g->set("scree_blocks_distance", layer_enabled[LAYER_SCREE] ? layer_radius[LAYER_SCREE] : 0.0);
	}
}

void RockField3D::set_ground_material(const Ref<ShaderMaterial> &p_material) {
	ground_material = p_material;
	_sync_scree_blocks();
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		_apply_ground(kv.value);
	}
}

void RockField3D::set_moss_cover(real_t p_cover) {
	moss_cover = CLAMP(p_cover, real_t(0.0), real_t(1.0));
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		kv.value->set_shader_parameter("moss_cover", moss_cover);
	}
}

void RockField3D::set_layer_enabled(int p_layer, bool p_enabled) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	layer_enabled[p_layer] = p_enabled;
	_sync_scree_blocks();
	rebuild();
}

bool RockField3D::is_layer_enabled(int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, LAYER_MAX, false);
	return layer_enabled[p_layer];
}

void RockField3D::set_layer_density(int p_layer, real_t p_density) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	layer_density[p_layer] = MAX(p_density, real_t(0.0));
	rebuild();
}

real_t RockField3D::get_layer_density(int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, LAYER_MAX, 0.0);
	return layer_density[p_layer];
}

void RockField3D::set_layer_radius(int p_layer, real_t p_radius) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	layer_radius[p_layer] = MAX(p_radius, real_t(0.0));
	_sync_scree_blocks();
}

real_t RockField3D::get_layer_radius(int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, LAYER_MAX, 0.0);
	return layer_radius[p_layer];
}

void RockField3D::set_min_size(real_t p_size) {
	min_size = MAX(p_size, real_t(0.01));
	rebuild();
}

void RockField3D::set_river_min_area(real_t p_area) {
	river_min_area = MAX(p_area, real_t(1.0));
	rebuild();
}

void RockField3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	rebuild();
}

void RockField3D::set_streaming(bool p_streaming) {
	streaming = p_streaming;
	if (is_inside_tree()) {
		_update_process();
	}
}

// The contact map (docs/ideas/rocks.md, "Stone and ground contact"). Rock fragments keep the soil beside them wetter
// (a mulch: Poesen and Lavee 1994) and trap the fines washed down against their uphill side; UE's "draw in virtual
// texture" and Frostbite's terrain masks put such marks into the terrain from the objects. Here RockField3D stamps
// each stone's footprint (get_obstacles) into an RG8 global around the target, CONTACT_SIZE metres a side, redrawn
// when the target crosses an eighth of it or the field changes; TerrainMaterial3D darkens and tints by it.
void RockField3D::_update_contact(bool p_force) {
	if (!is_inside_tree()) {
		return;
	}
	_finish_contact(p_force);
	if (contact_job) {
		return; // one at a time
	}
	const Vector3 t = _target_position();
	const real_t step = CONTACT_SIZE / 8.0;
	const Vector2 origin = Vector2(Math::round(t.x / step) * step, Math::round(t.z / step) * step) - Vector2(CONTACT_SIZE, CONTACT_SIZE) * 0.5;
	if (!p_force && !contact_dirty && origin == contact_origin) {
		return;
	}
	contact_dirty = false;
	contact_origin = origin;
	ContactJob *job = memnew(ContactJob);
	job->start = OS::get_singleton()->get_ticks_usec();
	job->origin = origin;
	const TerrainGenerator3D *gen = _get_generator();
	const PackedVector4Array obs = get_obstacles(Rect2(origin, Vector2(CONTACT_SIZE, CONTACT_SIZE)).grow(4.0), 0.1);
	job->obstacles.resize(obs.size());
	job->uphill.resize(obs.size());
	job->damp_share.resize(obs.size());
	for (int i = 0; i < obs.size(); i++) {
		const Vector4 o = obs[i];
		// Uphill from the ground's gradient round the foot.
		Vector2 up;
		real_t slope = 0.0;
		if (gen) {
			const real_t e = MAX(real_t(0.5), real_t(o.w));
			up = Vector2(gen->get_height(Vector3(o.x + e, 0, o.z)) - gen->get_height(Vector3(o.x - e, 0, o.z)), gen->get_height(Vector3(o.x, 0, o.z + e)) - gen->get_height(Vector3(o.x, 0, o.z - e))) / (2.0 * e);
			slope = up.length();
			up = slope > 1e-4 ? up / slope : Vector2();
		}
		job->obstacles[i] = Vector4(o.x, o.z, o.w, slope);
		// Scree drains: stones on talus keep no damp ring (it read as a dirty smudge on dry grey talus), only a
		// faint one; the banked fines stay.
		job->damp_share[i] = gen ? 1.0f - 0.85f * float(Math::smoothstep(real_t(0.25), real_t(0.5), real_t(gen->get_talus(Vector3(o.x, 0, o.z))))) : 1.0f;
		job->uphill[i] = up;
	}
	contact_job = job;
	// High priority: a job of about 0.1 s that the ground waits on.
	contact_task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_contact_job, job, true, "RockField3D contact map");
	if (p_force) {
		_finish_contact(true);
	}
}

void RockField3D::_contact_job(void *p_job) {
	ContactJob *job = static_cast<ContactJob *>(p_job);
	const int R = CONTACT_RES;
	const real_t texel = CONTACT_SIZE / real_t(R);
	const Vector2 origin = job->origin;
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	job->data.resize(R * R * 2);
	uint8_t *w = job->data.ptrw();
	memset(w, 0, job->data.size());
	for (uint32_t i = 0; i < job->obstacles.size(); i++) {
		const Vector4 o = job->obstacles[i];
		const real_t r = o.z;
		const Vector2 up = job->uphill[i];
		// The damp ring: about the radius wide (at least 25 cm, so small stones still show one), falling off smoothly.
		const real_t ring = MAX(real_t(0.25), real_t(1.0) * r);
		const real_t bank = ring * 2.5; // the banked fines reach further out uphill
		const real_t reach = r + MAX(ring, bank);
		const int x0 = MAX(0, int(Math::floor((o.x - reach - origin.x) / texel))), x1 = MIN(R - 1, int(Math::ceil((o.x + reach - origin.x) / texel)));
		const int z0 = MAX(0, int(Math::floor((o.y - reach - origin.y) / texel))), z1 = MIN(R - 1, int(Math::ceil((o.y + reach - origin.y) / texel)));
		const real_t uphill_w = CLAMP(real_t(o.w) * 2.5f, real_t(0.0), real_t(1.0));
		// Pebbles mark the ground less than boulders (a faint ring, not a dark halo round every chip).
		const real_t size_w = CLAMP(r / real_t(0.35), real_t(0.35), real_t(1.0));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 d = Vector2(origin.x + (x + 0.5) * texel - o.x, origin.y + (z + 0.5) * texel - o.y);
				const real_t dl = d.length();
				const real_t out = dl - r; // metres past the footprint's edge
				const real_t s = CLAMP(1.0 - out / ring, 0.0, 1.0);
				const real_t damp = s * s * (3.0 - 2.0 * s);
				// Fines: a thin skirt all round, a bank on the uphill side.
				const real_t u = dl > 1e-4 ? CLAMP((d.x * up.x + d.y * up.y) / dl, real_t(0.0), real_t(1.0)) : 0.0;
				const real_t span = Math::lerp(ring * 0.6, bank, u * uphill_w);
				const real_t sf = CLAMP(1.0 - out / span, 0.0, 1.0);
				const real_t fines = sf * sf * (3.0 - 2.0 * sf) * Math::lerp(real_t(0.45), real_t(1.0), u * uphill_w);
				uint8_t *px = w + (z * R + x) * 2;
				px[0] = MAX(px[0], uint8_t(CLAMP(damp * size_w * job->damp_share[i] * 255.0, 0.0, 255.0)));
				px[1] = MAX(px[1], uint8_t(CLAMP(fines * size_w * (0.5f + 0.5f * job->damp_share[i]) * 255.0, 0.0, 255.0)));
			}
		}
	}
	job->end = OS::get_singleton()->get_ticks_usec();
	job->start = t0;
}

void RockField3D::_finish_contact(bool p_wait) {
	if (!contact_job) {
		return;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(contact_task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(contact_task);
	ContactJob *job = contact_job;
	contact_job = nullptr;
	contact_task = WorkerThreadPool::INVALID_TASK_ID;
	if (is_inside_tree() && ground_contact) {
		const int R = CONTACT_RES;
		Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_RG8, job->data);
		if (contact_texture.is_valid() && contact_texture->get_width() == R) {
			contact_texture->update(img);
		} else {
			contact_texture = ImageTexture::create_from_image(img);
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->global_shader_parameter_set("rock_contact_map", contact_texture->get_rid());
		rs->global_shader_parameter_set("rock_contact_rect", Vector4(job->origin.x, job->origin.y, CONTACT_SIZE, CONTACT_SIZE));
		contact_usec = job->end - job->start; // the worker's own time
	}
	memdelete(job);
}

void RockField3D::_clear_contact() {
	_finish_contact(true);
	if (contact_texture.is_valid()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("rock_contact_rect", Vector4());
	}
	contact_origin = Vector2(1e30, 1e30);
	contact_dirty = true;
}

/* Ground detail (docs/ideas/ground-detail.md) */

void RockField3D::_detail_sizes(Layer p_layer, real_t p_upper, float &r_d1, float &r_d2) const {
	switch (p_layer) {
		case LAYER_DETAIL_PEBBLE:
			r_d1 = 0.04f;
			r_d2 = float(p_upper);
			break;
		case LAYER_DETAIL_GRAVEL:
			r_d1 = 0.02f;
			r_d2 = 0.04f;
			break;
		case LAYER_DETAIL_FINE:
			r_d1 = 0.01f;
			r_d2 = 0.02f;
			break;
		default:
			r_d1 = r_d2 = 0.0f;
			return;
	}
	r_d2 = MIN(r_d2, float(p_upper)); // a min_size under a class's top takes the class's top off it
}

// Each class is drawn out to where its mean stone (the geometric mean of its sizes) is ground_detail_pixels tall:
// d = s / (pixels x the angle of one pixel), the vertical field of view over the image height.
void RockField3D::_update_detail_radii() {
	if (!ground_detail) {
		return;
	}
	real_t fov = Math::deg_to_rad(real_t(75.0));
	real_t height = 1080.0;
	if (is_inside_tree()) {
		Viewport *vp = get_viewport();
		if (vp) {
			if (Camera3D *cam = vp->get_camera_3d()) {
				fov = Math::deg_to_rad(cam->get_fov());
				if (cam->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH) {
					const Size2 sz = vp->get_visible_rect().size;
					fov = 2.0 * Math::atan(Math::tan(fov * 0.5) * sz.y / MAX(sz.x, real_t(1.0)));
				}
			}
			height = vp->get_visible_rect().size.y;
		}
	}
	if (ground_detail_reference_height > 0) {
		height = ground_detail_reference_height;
	}
	const real_t pixel = fov / MAX(height, real_t(1.0));
	for (int l = LAYER_DETAIL_PEBBLE; l <= LAYER_DETAIL_FINE; l++) {
		float d1, d2;
		_detail_sizes(Layer(l), MAX(min_size, real_t(0.15)), d1, d2);
		const real_t mean = Math::sqrt(real_t(d1) * real_t(MAX(d2, d1)));
		layer_radius[l] = MIN(mean / (ground_detail_pixels * pixel), ground_detail_max_radius);
	}
}

// The stones of the coarser layers (the big ones' cells and the coarser detail classes) round p_rect, as spheres for
// the deposition.
void RockField3D::_detail_seeds(Layer p_layer, const Rect2 &p_rect, LocalVector<DetailSeed> &r_seeds) const {
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		const int l = kv.key.x;
		if (l == p_layer || (_is_detail(l) && l > p_layer) || l == LAYER_CLIFF || l == LAYER_COAST || l == LAYER_LANDFORM) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const Rect2 rect(kv.key.y * cs, kv.key.z * cs, cs, cs);
		if (!rect.grow(4.0).intersects(p_rect)) {
			continue;
		}
		for (const Group &grp : kv.value.groups) {
			const Library *lib = libraries.getptr(grp.library);
			if (!lib || grp.variant >= int(lib->axes.size())) {
				continue;
			}
			const Vector3 ax = lib->axes[grp.variant];
			const float axm = (ax.x + ax.y + ax.z) / 3.0f;
			for (uint32_t k = 0; k < grp.xforms.size(); k++) {
				if (grp.broken[k]) {
					continue;
				}
				const Vector3 o = grp.xforms[k].origin;
				const float r = float(grp.custom[k].b) * axm * 0.85f;
				if (p_rect.grow(r).has_point(Vector2(o.x, o.z))) {
					r_seeds.push_back({ Vector2(o.x, o.z), float(o.y), r });
				}
			}
		}
	}
}

void RockField3D::set_ground_detail(bool p_enabled) {
	if (ground_detail == p_enabled) {
		return;
	}
	ground_detail = p_enabled;
	if (!ground_detail) {
		// Drop the detail cells now; the rest of the field stays as it is.
		_detail_cancel();
		LocalVector<Vector3i> drop;
		for (KeyValue<Vector3i, Cell> &kv : cells) {
			if (_is_detail(kv.key.x)) {
				stone_count -= kv.value.stones;
				_free_cell(kv.value);
				drop.push_back(kv.key);
			}
		}
		for (const Vector3i &k : drop) {
			cells.erase(k);
		}
	}
	_update_detail_radii();
}

void RockField3D::set_ground_detail_pixels(real_t p_pixels) {
	ground_detail_pixels = CLAMP(p_pixels, real_t(0.5), real_t(64.0));
	_update_detail_radii();
}

void RockField3D::set_ground_detail_reference_height(int p_height) {
	ground_detail_reference_height = MAX(p_height, 0);
	_update_detail_radii();
}

void RockField3D::set_ground_detail_max_radius(real_t p_radius) {
	ground_detail_max_radius = MAX(p_radius, real_t(0.5));
	_update_detail_radii();
}

int RockField3D::get_ground_detail_count(int p_layer) const {
	int n = 0;
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x) && (p_layer < 0 || kv.key.x == p_layer)) {
			n += kv.value.stones;
		}
	}
	return n;
}

void RockField3D::set_ground_contact(bool p_enabled) {
	ground_contact = p_enabled;
	if (ground_contact) {
		contact_dirty = true;
		_update_contact(true);
	} else {
		_clear_contact();
	}
	if (is_inside_tree()) {
		_update_process();
	}
}

void RockField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			auto_ground_found = false;
			_update_process();
			_update_dust_node();
			fall_rng.seed(uint64_t(seed) * 2654435761u + 17);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (streaming) {
				_update_streaming(false);
			}
			if (live_rockfall) {
				_update_rockfall(get_process_delta_time());
			}
			if (ground_contact) {
				_update_contact(false);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_update_dust(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			clear();
			_clear_contact();
			dust_tracks.clear();
			dust_events.clear();
		} break;
	}
}

void RockField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_generator", "path"), &RockField3D::set_generator);
	ClassDB::bind_method(D_METHOD("get_generator"), &RockField3D::get_generator);
	ClassDB::bind_method(D_METHOD("set_terrain3d", "path"), &RockField3D::set_terrain3d);
	ClassDB::bind_method(D_METHOD("get_terrain3d"), &RockField3D::get_terrain3d);
	ClassDB::bind_method(D_METHOD("write_terrain3d_holes"), &RockField3D::write_terrain3d_holes);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &RockField3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &RockField3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &RockField3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &RockField3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_rock_type", "rock_type"), &RockField3D::set_rock_type);
	ClassDB::bind_method(D_METHOD("get_rock_type"), &RockField3D::get_rock_type);
	ClassDB::bind_method(D_METHOD("set_soft_rock_type", "rock_type"), &RockField3D::set_soft_rock_type);
	ClassDB::bind_method(D_METHOD("get_soft_rock_type"), &RockField3D::get_soft_rock_type);
	ClassDB::bind_method(D_METHOD("set_erratic_rock_type", "rock_type"), &RockField3D::set_erratic_rock_type);
	ClassDB::bind_method(D_METHOD("get_erratic_rock_type"), &RockField3D::get_erratic_rock_type);
	ClassDB::bind_method(D_METHOD("set_use_strata", "use"), &RockField3D::set_use_strata);
	ClassDB::bind_method(D_METHOD("get_use_strata"), &RockField3D::get_use_strata);
	ClassDB::bind_method(D_METHOD("set_variants", "variants"), &RockField3D::set_variants);
	ClassDB::bind_method(D_METHOD("get_variants"), &RockField3D::get_variants);
	ClassDB::bind_method(D_METHOD("set_detail", "detail"), &RockField3D::set_detail);
	ClassDB::bind_method(D_METHOD("get_detail"), &RockField3D::get_detail);
	ClassDB::bind_method(D_METHOD("set_mineral_glints", "enabled"), &RockField3D::set_mineral_glints);
	ClassDB::bind_method(D_METHOD("is_mineral_glints"), &RockField3D::is_mineral_glints);
	ClassDB::bind_method(D_METHOD("set_coast_collision", "enabled"), &RockField3D::set_coast_collision);
	ClassDB::bind_method(D_METHOD("is_coast_collision"), &RockField3D::is_coast_collision);
	ClassDB::bind_method(D_METHOD("set_cave_mouths", "enabled"), &RockField3D::set_cave_mouths);
	ClassDB::bind_method(D_METHOD("is_cave_mouths"), &RockField3D::is_cave_mouths);
	ClassDB::bind_method(D_METHOD("set_ground_blend", "enabled"), &RockField3D::set_ground_blend);
	ClassDB::bind_method(D_METHOD("is_ground_blend"), &RockField3D::is_ground_blend);
	ClassDB::bind_method(D_METHOD("set_ground_blend_height", "height"), &RockField3D::set_ground_blend_height);
	ClassDB::bind_method(D_METHOD("get_ground_blend_height"), &RockField3D::get_ground_blend_height);
	ClassDB::bind_method(D_METHOD("set_ground_material", "material"), &RockField3D::set_ground_material);
	ClassDB::bind_method(D_METHOD("get_ground_material"), &RockField3D::get_ground_material);
	ClassDB::bind_method(D_METHOD("set_lichen_cover", "cover"), &RockField3D::set_lichen_cover);
	ClassDB::bind_method(D_METHOD("get_lichen_cover"), &RockField3D::get_lichen_cover);
	ClassDB::bind_method(D_METHOD("set_moss_cover", "cover"), &RockField3D::set_moss_cover);
	ClassDB::bind_method(D_METHOD("get_moss_cover"), &RockField3D::get_moss_cover);
	ClassDB::bind_method(D_METHOD("set_layer_enabled", "layer", "enabled"), &RockField3D::set_layer_enabled);
	ClassDB::bind_method(D_METHOD("is_layer_enabled", "layer"), &RockField3D::is_layer_enabled);
	ClassDB::bind_method(D_METHOD("set_layer_density", "layer", "density"), &RockField3D::set_layer_density);
	ClassDB::bind_method(D_METHOD("get_layer_density", "layer"), &RockField3D::get_layer_density);
	ClassDB::bind_method(D_METHOD("set_layer_radius", "layer", "radius"), &RockField3D::set_layer_radius);
	ClassDB::bind_method(D_METHOD("get_layer_radius", "layer"), &RockField3D::get_layer_radius);
	ClassDB::bind_method(D_METHOD("set_min_size", "metres"), &RockField3D::set_min_size);
	ClassDB::bind_method(D_METHOD("get_min_size"), &RockField3D::get_min_size);
	ClassDB::bind_method(D_METHOD("set_river_min_area", "area"), &RockField3D::set_river_min_area);
	ClassDB::bind_method(D_METHOD("get_river_min_area"), &RockField3D::get_river_min_area);
	ClassDB::bind_method(D_METHOD("set_max_stones_per_cell", "count"), &RockField3D::set_max_stones_per_cell);
	ClassDB::bind_method(D_METHOD("get_max_stones_per_cell"), &RockField3D::get_max_stones_per_cell);
	ClassDB::bind_method(D_METHOD("set_break_pieces", "pieces"), &RockField3D::set_break_pieces);
	ClassDB::bind_method(D_METHOD("get_break_pieces"), &RockField3D::get_break_pieces);
	ClassDB::bind_method(D_METHOD("set_break_prepare_radius", "radius"), &RockField3D::set_break_prepare_radius);
	ClassDB::bind_method(D_METHOD("get_break_prepare_radius"), &RockField3D::get_break_prepare_radius);
	ClassDB::bind_method(D_METHOD("set_break_density", "density"), &RockField3D::set_break_density);
	ClassDB::bind_method(D_METHOD("get_break_density"), &RockField3D::get_break_density);
	ClassDB::bind_method(D_METHOD("break_stone", "position", "damage", "radius"), &RockField3D::break_stone, DEFVAL(10.0), DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("restore_broken"), &RockField3D::restore_broken);
	ClassDB::bind_method(D_METHOD("get_broken_count"), &RockField3D::get_broken_count);
	ClassDB::bind_method(D_METHOD("is_breaking_supported"), &RockField3D::is_breaking_supported);
	ClassDB::bind_method(D_METHOD("set_live_rockfall", "enabled"), &RockField3D::set_live_rockfall);
	ClassDB::bind_method(D_METHOD("is_live_rockfall"), &RockField3D::is_live_rockfall);
	ClassDB::bind_method(D_METHOD("set_live_rockfall_rate", "rate"), &RockField3D::set_live_rockfall_rate);
	ClassDB::bind_method(D_METHOD("get_live_rockfall_rate"), &RockField3D::get_live_rockfall_rate);
	ClassDB::bind_method(D_METHOD("set_live_rockfall_radius", "radius"), &RockField3D::set_live_rockfall_radius);
	ClassDB::bind_method(D_METHOD("get_live_rockfall_radius"), &RockField3D::get_live_rockfall_radius);
	ClassDB::bind_method(D_METHOD("set_live_rockfall_max", "count"), &RockField3D::set_live_rockfall_max);
	ClassDB::bind_method(D_METHOD("get_live_rockfall_max"), &RockField3D::get_live_rockfall_max);
	ClassDB::bind_method(D_METHOD("get_rockfall_trigger"), &RockField3D::get_rockfall_trigger);
	ClassDB::bind_method(D_METHOD("trigger_rockfall", "position", "radius"), &RockField3D::trigger_rockfall, DEFVAL(30.0));
	ClassDB::bind_method(D_METHOD("set_rockfall_dust", "enabled"), &RockField3D::set_rockfall_dust);
	ClassDB::bind_method(D_METHOD("is_rockfall_dust"), &RockField3D::is_rockfall_dust);
	ClassDB::bind_method(D_METHOD("set_rockfall_dust_per_joule", "mass"), &RockField3D::set_rockfall_dust_per_joule);
	ClassDB::bind_method(D_METHOD("get_rockfall_dust_per_joule"), &RockField3D::get_rockfall_dust_per_joule);
	ClassDB::bind_method(D_METHOD("get_rockfall_dust_total"), &RockField3D::get_rockfall_dust_total);
	ClassDB::bind_method(D_METHOD("get_rockfall_dust_impacts"), &RockField3D::get_rockfall_dust_impacts);
	ClassDB::bind_method(D_METHOD("get_rockfall_dust_node"), &RockField3D::get_rockfall_dust_node);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &RockField3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &RockField3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_min_size", "metres"), &RockField3D::set_collision_min_size);
	ClassDB::bind_method(D_METHOD("get_collision_min_size"), &RockField3D::get_collision_min_size);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &RockField3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &RockField3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &RockField3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &RockField3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_close_up_enabled", "enabled"), &RockField3D::set_close_up_enabled);
	ClassDB::bind_method(D_METHOD("is_close_up_enabled"), &RockField3D::is_close_up_enabled);
	ClassDB::bind_method(D_METHOD("set_close_up_radius", "radius"), &RockField3D::set_close_up_radius);
	ClassDB::bind_method(D_METHOD("get_close_up_radius"), &RockField3D::get_close_up_radius);
	ClassDB::bind_method(D_METHOD("set_close_up_max_shapes", "count"), &RockField3D::set_close_up_max_shapes);
	ClassDB::bind_method(D_METHOD("get_close_up_max_shapes"), &RockField3D::get_close_up_max_shapes);
	ClassDB::bind_method(D_METHOD("get_close_up_shape_count"), &RockField3D::get_close_up_shape_count);
	ClassDB::bind_method(D_METHOD("get_close_up_triangle_count"), &RockField3D::get_close_up_triangle_count);
	ClassDB::bind_method(D_METHOD("set_build_budget_msec", "msec"), &RockField3D::set_build_budget_msec);
	ClassDB::bind_method(D_METHOD("get_build_budget_msec"), &RockField3D::get_build_budget_msec);
	ClassDB::bind_method(D_METHOD("set_streaming", "streaming"), &RockField3D::set_streaming);
	ClassDB::bind_method(D_METHOD("is_streaming"), &RockField3D::is_streaming);
	ClassDB::bind_method(D_METHOD("rebuild"), &RockField3D::rebuild);
	ClassDB::bind_method(D_METHOD("build_area", "rect"), &RockField3D::build_area);
	ClassDB::bind_method(D_METHOD("clear"), &RockField3D::clear);
	ClassDB::bind_method(D_METHOD("get_obstacles", "rect", "min_size"), &RockField3D::get_obstacles, DEFVAL(0.25));
	ClassDB::bind_method(D_METHOD("set_ground_contact", "enabled"), &RockField3D::set_ground_contact);
	ClassDB::bind_method(D_METHOD("set_ground_detail", "enabled"), &RockField3D::set_ground_detail);
	ClassDB::bind_method(D_METHOD("is_ground_detail"), &RockField3D::is_ground_detail);
	ClassDB::bind_method(D_METHOD("set_ground_detail_pixels", "pixels"), &RockField3D::set_ground_detail_pixels);
	ClassDB::bind_method(D_METHOD("get_ground_detail_pixels"), &RockField3D::get_ground_detail_pixels);
	ClassDB::bind_method(D_METHOD("set_ground_detail_reference_height", "height"), &RockField3D::set_ground_detail_reference_height);
	ClassDB::bind_method(D_METHOD("get_ground_detail_reference_height"), &RockField3D::get_ground_detail_reference_height);
	ClassDB::bind_method(D_METHOD("set_ground_detail_max_radius", "radius"), &RockField3D::set_ground_detail_max_radius);
	ClassDB::bind_method(D_METHOD("get_ground_detail_max_radius"), &RockField3D::get_ground_detail_max_radius);
	ClassDB::bind_method(D_METHOD("set_ground_detail_max_per_cell", "count"), &RockField3D::set_ground_detail_max_per_cell);
	ClassDB::bind_method(D_METHOD("get_ground_detail_max_per_cell"), &RockField3D::get_ground_detail_max_per_cell);
	ClassDB::bind_method(D_METHOD("get_ground_detail_count", "layer"), &RockField3D::get_ground_detail_count, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_cell_threads", "enabled"), &RockField3D::set_cell_threads);
	ClassDB::bind_method(D_METHOD("is_cell_threads"), &RockField3D::is_cell_threads);
	ClassDB::bind_method(D_METHOD("set_ground_detail_threads", "enabled"), &RockField3D::set_ground_detail_threads);
	ClassDB::bind_method(D_METHOD("is_ground_detail_threads"), &RockField3D::is_ground_detail_threads);
	ClassDB::bind_method(D_METHOD("get_ground_detail_pending"), &RockField3D::get_ground_detail_pending);
	ClassDB::bind_method(D_METHOD("get_cells_pending"), &RockField3D::get_cells_pending);
	ClassDB::bind_method(D_METHOD("get_last_detail_msec"), &RockField3D::get_last_detail_msec);
	ClassDB::bind_method(D_METHOD("set_moss_cushions", "enabled"), &RockField3D::set_moss_cushions);
	ClassDB::bind_method(D_METHOD("is_moss_cushions_enabled"), &RockField3D::is_moss_cushions_enabled);
	ClassDB::bind_method(D_METHOD("get_moss_shell_count"), &RockField3D::get_moss_shell_count);
	ClassDB::bind_method(D_METHOD("get_moss_triangle_count"), &RockField3D::get_moss_triangle_count);
	ClassDB::bind_method(D_METHOD("get_moss_pending"), &RockField3D::get_moss_pending);
	ClassDB::bind_method(D_METHOD("is_ground_contact_enabled"), &RockField3D::is_ground_contact_enabled);
	ClassDB::bind_method(D_METHOD("get_contact_texture"), &RockField3D::get_contact_texture);
	ClassDB::bind_method(D_METHOD("get_last_contact_usec"), &RockField3D::get_last_contact_usec);
	ClassDB::bind_method(D_METHOD("get_stones", "layer", "rect"), &RockField3D::get_stones);
	ClassDB::bind_method(D_METHOD("get_stone_caps", "layer", "rect", "tolerance"), &RockField3D::get_stone_caps, DEFVAL(0.03));
	ClassDB::bind_method(D_METHOD("get_stone_count"), &RockField3D::get_stone_count);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &RockField3D::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &RockField3D::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_library_msec"), &RockField3D::get_last_library_msec);
	ClassDB::bind_method(D_METHOD("get_library_triangle_count"), &RockField3D::get_library_triangle_count);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "generator", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainGenerator3D"), "set_generator", "get_generator");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain3d"), "set_terrain3d", "get_terrain3d");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_GROUP("Geology", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rock_type", PROPERTY_HINT_ENUM, "Granite,Limestone,Sandstone,Basalt,Slate"), "set_rock_type", "get_rock_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "soft_rock_type", PROPERTY_HINT_ENUM, "Granite,Limestone,Sandstone,Basalt,Slate"), "set_soft_rock_type", "get_soft_rock_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "erratic_rock_type", PROPERTY_HINT_ENUM, "Granite,Limestone,Sandstone,Basalt,Slate"), "set_erratic_rock_type", "get_erratic_rock_type");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_strata"), "set_use_strata", "get_use_strata");
	ADD_GROUP("Shapes", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "variants", PROPERTY_HINT_RANGE, "1,32,1"), "set_variants", "get_variants");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_detail", "get_detail");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lichen_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_lichen_cover", "get_lichen_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moss_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_moss_cover", "get_moss_cover");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_blend"), "set_ground_blend", "is_ground_blend");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mineral_glints"), "set_mineral_glints", "is_mineral_glints");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_blend_height", PROPERTY_HINT_RANGE, "0,1,0.01,suffix:m"), "set_ground_blend_height", "get_ground_blend_height");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ground_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_ground_material", "get_ground_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_contact"), "set_ground_contact", "is_ground_contact_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "moss_cushions"), "set_moss_cushions", "is_moss_cushions_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_size", PROPERTY_HINT_RANGE, "0.01,1,0.01,suffix:m"), "set_min_size", "get_min_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "river_min_area", PROPERTY_HINT_RANGE, "1000,10000000,1000,or_greater,suffix:m²"), "set_river_min_area", "get_river_min_area");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_stones_per_cell", PROPERTY_HINT_RANGE, "1,100000,1"), "set_max_stones_per_cell", "get_max_stones_per_cell");
	static const char *layer_names[LAYER_MAX] = { "rockfall", "scree", "glacial", "river", "shore", "cliff", "tor", "coast", "landform", "detail_pebble", "detail_gravel", "detail_fine" };
	for (int l = 0; l < LAYER_MAX; l++) {
		ADD_GROUP(String(layer_names[l]).capitalize(), String(layer_names[l]) + "_");
		ADD_PROPERTYI(PropertyInfo(Variant::BOOL, String(layer_names[l]) + "_enabled"), "set_layer_enabled", "is_layer_enabled", l);
		ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, String(layer_names[l]) + "_density", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_layer_density", "get_layer_density", l);
		ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, String(layer_names[l]) + "_radius", PROPERTY_HINT_RANGE, "0,4000,1,suffix:m"), "set_layer_radius", "get_layer_radius", l);
	}
	ADD_GROUP("Ground Detail", "ground_detail_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail"), "set_ground_detail", "is_ground_detail");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_detail_pixels", PROPERTY_HINT_RANGE, "0.5,16,0.1,suffix:px"), "set_ground_detail_pixels", "get_ground_detail_pixels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_detail_reference_height", PROPERTY_HINT_RANGE, "0,4320,1,suffix:px"), "set_ground_detail_reference_height", "get_ground_detail_reference_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_detail_max_radius", PROPERTY_HINT_RANGE, "1,200,0.5,suffix:m"), "set_ground_detail_max_radius", "get_ground_detail_max_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_detail_max_per_cell", PROPERTY_HINT_RANGE, "1,1000000,1"), "set_ground_detail_max_per_cell", "get_ground_detail_max_per_cell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_threads"), "set_cell_threads", "is_cell_threads");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail_threads"), "set_ground_detail_threads", "is_ground_detail_threads");
	ADD_GROUP("Breaking", "break_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "break_pieces", PROPERTY_HINT_RANGE, "2,48,1"), "set_break_pieces", "get_break_pieces");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_prepare_radius", PROPERTY_HINT_RANGE, "0,50,0.5,suffix:m"), "set_break_prepare_radius", "get_break_prepare_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_density", PROPERTY_HINT_RANGE, "500,4000,10,suffix:kg/m³"), "set_break_density", "get_break_density");
	ADD_GROUP("Live Rockfall", "live_rockfall_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_rockfall"), "set_live_rockfall", "is_live_rockfall");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_rockfall_rate", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater"), "set_live_rockfall_rate", "get_live_rockfall_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_rockfall_radius", PROPERTY_HINT_RANGE, "10,1000,1,suffix:m"), "set_live_rockfall_radius", "get_live_rockfall_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "live_rockfall_max", PROPERTY_HINT_RANGE, "1,128,1"), "set_live_rockfall_max", "get_live_rockfall_max");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rockfall_dust"), "set_rockfall_dust", "is_rockfall_dust");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rockfall_dust_per_joule", PROPERTY_HINT_RANGE, "0,0.001,0.000001,suffix:kg/J"), "set_rockfall_dust_per_joule", "get_rockfall_dust_per_joule");
	ADD_SIGNAL(MethodInfo("rockfall", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "mass")));
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_min_size", PROPERTY_HINT_RANGE, "0,4,0.01,suffix:m"), "set_collision_min_size", "get_collision_min_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0,1000,1,suffix:m"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "coast_collision"), "set_coast_collision", "is_coast_collision");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cave_mouths"), "set_cave_mouths", "is_cave_mouths");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_GROUP("Streaming", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming", "is_streaming");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "build_budget_msec", PROPERTY_HINT_RANGE, "0.1,50,0.1,suffix:ms"), "set_build_budget_msec", "get_build_budget_msec");
	ADD_GROUP("Close-up", "close_up_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "close_up_enabled"), "set_close_up_enabled", "is_close_up_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "close_up_radius", PROPERTY_HINT_RANGE, "0,50,0.1,suffix:m"), "set_close_up_radius", "get_close_up_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "close_up_max_shapes", PROPERTY_HINT_RANGE, "1,64,1"), "set_close_up_max_shapes", "get_close_up_max_shapes");

	BIND_ENUM_CONSTANT(LAYER_ROCKFALL);
	BIND_ENUM_CONSTANT(LAYER_SCREE);
	BIND_ENUM_CONSTANT(LAYER_GLACIAL);
	BIND_ENUM_CONSTANT(LAYER_RIVER);
	BIND_ENUM_CONSTANT(LAYER_SHORE);
	BIND_ENUM_CONSTANT(LAYER_CLIFF);
	BIND_ENUM_CONSTANT(LAYER_TOR);
	BIND_ENUM_CONSTANT(LAYER_COAST);
	BIND_ENUM_CONSTANT(LAYER_LANDFORM);
	BIND_ENUM_CONSTANT(LAYER_DETAIL_PEBBLE);
	BIND_ENUM_CONSTANT(LAYER_DETAIL_GRAVEL);
	BIND_ENUM_CONSTANT(LAYER_DETAIL_FINE);
	BIND_ENUM_CONSTANT(LAYER_MAX);
}

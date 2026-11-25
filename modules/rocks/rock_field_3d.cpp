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
#include "modules/terrain_deform/world_cache.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"
#include "modules/world/world_walker_steps.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/convex_hull.h"
#include "core/io/marshalls.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

#include <cmath>
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/surface_tool.h"
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

// Two squares' grains alike enough to swap (tiles of piles): the same rocks and wetness within 0.15, and grain count,
// the rocks' shares
// and mean log size within what a square's own grains already vary by chance: 1.5 standard errors (count: Poisson,
// sqrt(n); mean log size: its spread over sqrt(n)), and never tighter than 15% and 0.12.
static bool rf_tile_like(uint32_t p_sig_a, int p_n_a, float p_ls_a, float p_sd_a, float p_film_a, float p_mix_a, uint32_t p_sig_b, int p_n_b, float p_ls_b, float p_sd_b, float p_film_b, float p_mix_b) {
	if (p_sig_a != p_sig_b || Math::abs(p_film_a - p_film_b) >= 0.15f) {
		return false;
	}
	const float n = float(MAX(MIN(p_n_a, p_n_b), 1));
	const float tn = MAX(0.15f, 1.5f / Math::sqrt(n));
	const float nr = float(p_n_a) / float(MAX(p_n_b, 1));
	if (nr <= 1.0f - tn || nr >= 1.0f / (1.0f - tn)) {
		return false;
	}
	// The rocks' shares (binomial: sqrt(p (1 - p) / n)), so a square keeps its mix of colours.
	const float pm = 0.5f * (p_mix_a + p_mix_b);
	if (Math::abs(p_mix_a - p_mix_b) >= MAX(0.08f, 1.5f * Math::sqrt(pm * (1.0f - pm) / n))) {
		return false;
	}
	const float tl = MAX(0.12f, 1.5f * MAX(p_sd_a, p_sd_b) / Math::sqrt(n));
	return Math::abs(p_ls_a - p_ls_b) < tl;
}

// Library keys: rock type (4 bits), family (5), roundness bin (4).
inline RockGenerator::RockType key_type(uint32_t p_key) {
	return RockGenerator::RockType(p_key & 0xF);
}
inline RockGenerator::Family key_family(uint32_t p_key) {
	return RockGenerator::Family((p_key >> 4) & 0x1F);
}
inline int key_bin(uint32_t p_key) {
	return int((p_key >> 9) & 0xF);
}

// Close-up shapes per variant: one per octave of stone size round the family's typical size (odd count).
constexpr int RF_MARK_BINS = 9;

// Ground detail libraries (docs/ideas/ground-detail.md): the same shapes, drawn from one of their own LODs, at bins
// RF_DETAIL_BIN + 0, 1, 2 for the pebble, gravel and fine classes.
constexpr int RF_DETAIL_BIN = 8;
// Detail grains off screen that look smaller than this many degrees stay out of the path tracer (multimesh_set_rt_offscreen_cull).
constexpr float RF_DETAIL_RT_CULL_DEG = 0.5f;
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
	// Close-up shapes get LODs too: they draw within close_up_radius of the follow target, which a far or high camera
	// sees at a few pixels a stone; full detail wherever a pixel can show it (the renderer's 1 px rule), the coarser
	// levels only below that, in the main and the shadow passes alike.
	g->set_lods(job.fracture == 0);
	g->set_fracture_pieces(job.fracture);
	g->set_marks_scale(job.marks_scale);
	// The world cache (world_cache.h): a library shape is the same bytes from the same inputs (and the same
	// executable, in the key), so a later launch loads it instead of building it again. Not close-ups or fracture
	// pieces, built on demand and carrying more than the mesh.
	const bool cacheable = !job.close_up && job.fracture == 0 && WorldCache::is_enabled();
	String name;
	if (cacheable) {
		WorldCacheKey key("rock_shape");
		key.value(int32_t(job.type));
		key.value(int32_t(job.family));
		key.value(int32_t(job.seed));
		key.value(job.transport);
		key.value(job.detail);
		key.value(job.marks_scale);
		name = key.finish();
		Vector<uint8_t> data;
		if (WorldCache::load_any("rock_shape", name, data)) {
			Variant v;
			int len = 0;
			if (decode_variant(v, data.ptr(), data.size(), &len, false) == OK && len == data.size() && v.get_type() == Variant::ARRAY) {
				const Array a = v;
				if (a.size() == 2 && a[0].get_type() == Variant::DICTIONARY && a[1].get_type() == Variant::DICTIONARY) {
					Ref<ImporterMesh> im;
					im.instantiate();
					im->set("_data", a[0]);
					if (im->get_surface_count() > 0) {
						for (int i = 0; i < im->get_surface_count(); i++) {
							im->set_surface_material(i, RockGenerator::get_material(job.type));
						}
						job.mesh = im;
						job.meta = a[1];
						return;
					}
				}
			}
		}
	}
	job.mesh = g->build_importer_mesh(job.meta);
	if (cacheable && job.mesh.is_valid()) {
		// The surfaces without their material (the rock type's, set again on load).
		Dictionary d = Dictionary(job.mesh->get("_data")).duplicate(true);
		Array surfaces = d.get("surfaces", Array());
		for (int i = 0; i < surfaces.size(); i++) {
			Dictionary sd = surfaces[i];
			sd.erase("material");
		}
		Array a;
		a.push_back(d);
		a.push_back(job.meta);
		int len = 0;
		if (encode_variant(a, nullptr, len, false) == OK && len > 0) {
			Vector<uint8_t> data;
			data.resize(len);
			if (encode_variant(a, data.ptrw(), len, false) == OK) {
				WorldCache::store("rock_shape", name, data.ptr(), data.size());
			}
		}
	}
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

// The generator's height with the point held inside its world: past the edge its heights are not numbers, and a
// stone near the edge looks round it for metres (its slope, fall line and run-out), which drew thousands of
// non-finite normals round a spot 30 m from the edge. The ground just past the edge reads as the edge's own.
static real_t rf_height(const TerrainGenerator3D *p_gen, const Vector3 &p) {
	const Rect2 w = p_gen->get_result_rect(); // also from the detail jobs' worker threads
	return p_gen->get_height(Vector3(CLAMP(p.x, w.position.x, w.get_end().x), p.y, CLAMP(p.z, w.position.y, w.get_end().y)));
}

// Ground normal from the generator's height, by central differences.
Vector3 ground_normal(const TerrainGenerator3D *p_gen, real_t p_x, real_t p_z, real_t p_e) {
	const real_t hx = rf_height(p_gen, Vector3(p_x + p_e, 0, p_z)) - rf_height(p_gen, Vector3(p_x - p_e, 0, p_z));
	const real_t hz = rf_height(p_gen, Vector3(p_x, 0, p_z + p_e)) - rf_height(p_gen, Vector3(p_x, 0, p_z - p_e));
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
static thread_local const void *rf_job_wear = nullptr; // the detail job's StandWear while _place runs for it
static thread_local const void *rf_job_frost = nullptr; // and its FrostClimate
static thread_local const void *rf_job_rain = nullptr; // and its RainClimate

// Frost-sorted patterned ground (docs/ideas/ground-detail.md, "Frost-sorted patterned ground"). Freeze-thaw sorting
// is a self-organising phase separation of stones from fines (Kessler and Werner 2003: stones move toward stone-rich
// ground, stone domains are squeezed along their length); a Cahn-Hilliard field shows the same selection: a stone
// share near 0.6 gives fine-centred circles in a stone net, a mobility that is larger across the slope than along it
// gives stripes down it. Two periodic maps, computed once (64 x 64 cells of 0.2 m, a 2 m wavelength): 0 circles,
// 1 stripes along z. Values about -1 (fines) to 1 (stones).
static const int RF_FROST_N = 64;
static constexpr float RF_FROST_CELL = 0.2f;
static LocalVector<float> rf_frost_map[2];
static BinaryMutex rf_frost_mutex;
static bool rf_frost_ready = false;

static void rf_frost_build() {
	MutexLock lock(rf_frost_mutex);
	if (rf_frost_ready) {
		return;
	}
	const int N = RF_FROST_N;
	// The fastest-growing wavelength is 2 pi sqrt(2 kappa) cells: 10 cells (2 m) at kappa 1.27.
	const float kappa = 1.27f, dt = 0.01f;
	for (int m = 0; m < 2; m++) {
		const float c0 = m == 0 ? 0.25f : 0.0f; // circles: stones a little over half; stripes: even
		const float mx = 1.0f, mz = m == 0 ? 1.0f : 0.2f;
		LocalVector<float> c, mu;
		c.resize(N * N);
		mu.resize(N * N);
		RandomPCG r(0x5f3a91u + m);
		for (float &v : c) {
			v = c0 + r.random(-0.05f, 0.05f);
		}
		auto at = [&](const LocalVector<float> &f, int x, int z) { return f[((z + N) % N) * N + (x + N) % N]; };
		for (int step = 0; step < 4000; step++) {
			for (int z = 0; z < N; z++) {
				for (int x = 0; x < N; x++) {
					const float v = c[z * N + x];
					const float lap = at(c, x + 1, z) + at(c, x - 1, z) + at(c, x, z + 1) + at(c, x, z - 1) - 4.0f * v;
					mu[z * N + x] = v * v * v - v - kappa * lap;
				}
			}
			for (int z = 0; z < N; z++) {
				for (int x = 0; x < N; x++) {
					const float u = mu[z * N + x];
					const float lx = at(mu, x + 1, z) + at(mu, x - 1, z) - 2.0f * u;
					const float lz = at(mu, x, z + 1) + at(mu, x, z - 1) - 2.0f * u;
					c[z * N + x] += dt * (mx * lx + mz * lz);
				}
			}
		}
		rf_frost_map[m] = c;
	}
	rf_frost_ready = true;
}

// The map's value at a point of its plane (m), bilinear and periodic.
static float rf_frost_sample(int p_map, float p_u, float p_v) {
	const int N = RF_FROST_N;
	const float fu = p_u / RF_FROST_CELL, fv = p_v / RF_FROST_CELL;
	const int x = int(Math::floor(fu)), z = int(Math::floor(fv));
	const float tu = fu - x, tv = fv - z;
	auto at = [&](int a, int b) { return rf_frost_map[p_map][((b % N + N) % N) * N + (a % N + N) % N]; };
	return Math::lerp(Math::lerp(at(x, z), at(x + 1, z), tu), Math::lerp(at(x, z + 1), at(x + 1, z + 1), tu), tv);
}

// The stone share (0 fines .. 1 stones) of the sorted ground at a point: circles on the flat, stripes down the fall
// line from about 3 degrees. Circles: the map in world axes with a slow warp (its 12.8 m period does not line up);
// stripes: each 48 m block's map turned to the block's fall line, blended with the next block's only near their edge.
// The share of the ground each detail class covers in the sorted ground's stone domains (the borders are
// clast-supported: Washburn 1956; Hallet 2013): the detail cells and the far cover maps.
static const float rf_frost_cover[3] = { 0.6f, 0.4f, 0.2f };
// A caller sampling many points (the frost map) keeps the blocks' fall lines here instead of recomputing them.
struct RFFrostFalls {
	HashMap<int64_t, Vector2> fall;
};
static float rf_frost_share(const TerrainGenerator3D *p_gen, float p_x, float p_z, float p_slope_deg, RFFrostFalls *r_cache = nullptr) {
	const float wx = 0.7f * Math::sin(p_x * 0.21f + 1.3f * Math::sin(p_z * 0.17f)), wz = 0.7f * Math::cos(p_z * 0.19f + 1.1f * Math::sin(p_x * 0.13f));
	const float circ = rf_frost_sample(0, p_x + wx, p_z + wz);
	const float t = Math::smoothstep(2.0f, 5.0f, p_slope_deg);
	float c = circ;
	if (t > 0.0f) {
		const float B = 48.0f;
		const float bx = p_x / B - 0.5f, bz = p_z / B - 0.5f;
		const int i0 = int(Math::floor(bx)), j0 = int(Math::floor(bz));
		const float sx = Math::smoothstep(0.4f, 0.6f, bx - i0), sz = Math::smoothstep(0.4f, 0.6f, bz - j0);
		float stripe = 0.0f;
		for (int q = 0; q < 4; q++) {
			const int bi = i0 + (q & 1), bj = j0 + (q >> 1);
			const float w = ((q & 1) ? sx : 1.0f - sx) * ((q >> 1) ? sz : 1.0f - sz);
			if (w <= 0.0f) {
				continue;
			}
			const float cx = (bi + 0.5f) * B, cz = (bj + 0.5f) * B;
			const int64_t key = (int64_t(bi) << 32) ^ int64_t(uint32_t(bj));
			Vector2 fall;
			if (r_cache && r_cache->fall.has(key)) {
				fall = r_cache->fall[key];
			} else {
				const Vector3 n = ground_normal(p_gen, cx, cz, 12.0);
				fall = Vector2(n.x, n.z);
				fall = fall.length() > 1e-4f ? fall.normalized() : Vector2(0, 1);
				if (r_cache) {
					r_cache->fall[key] = fall;
				}
			}
			// Map z along the fall line, x across it (the stripes run along map z).
			const Vector2 d(p_x - cx, p_z - cz);
			const float u = d.x * fall.y - d.y * fall.x, v = d.dot(fall);
			stripe += w * rf_frost_sample(1, u + 0.3f * wx, v);
		}
		c = Math::lerp(circ, stripe, t);
	}
	return Math::smoothstep(-0.25f, 0.35f, c);
}
static thread_local bool rf_ledges_only = false; // get_obstacles: of the cliff layer only the ledge slabs (plants stand on their benches)
static thread_local bool rf_far = false; // _far_place: only the stones the far tier keeps need placing

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
	if (const Camera3D *cam = WorldContracts::get_view_camera(this)) {
		return cam->get_global_position();
	}
	return get_global_position();
}

// The ground detail is about what the camera sees up close, so it is centred on the camera (the follow target when
// there is none).
Vector3 RockField3D::_detail_center() const {
	if (const Camera3D *cam = WorldContracts::get_view_camera(this)) {
		return cam->get_global_position();
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
	return uint32_t(p_type) | (uint32_t(p_family) << 4) | (uint32_t(p_bin) << 9);
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
				j.seed += 131 * key_bin(k); // each bin its own shapes, not the same stones worn more
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
	// Workers placing cells call this too (each of get_obstacles' batch): one builds the list under the lock while the
	// others wait, and the flag is set only once the list is whole, so no worker reads it half built.
	MutexLock lock(rivers_lock);
	if (rivers_ready) {
		return;
	}
	rivers.clear();
	const TerrainGenerator3D *gen = _get_generator();
	if (!gen) {
		rivers_ready = true;
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
	rivers_ready = true;
}

// The river layer's segments that reach into p_rect, with the bed's median size as the river layer sets it (what the
// flow can move and what still arrives from upstream). Needs _prepare_rivers.
void RockField3D::_river_segments(const Rect2 &p_rect, LocalVector<RiverSeg> &r_segs) const {
	for (uint32_t r = 0; r < rivers.size(); r++) {
		const River &rv = rivers[r];
		if (!rv.bounds.grow(20.0).intersects(p_rect)) {
			continue;
		}
		// Each river's pools and riffles start at their own place along it.
		const float phase = float(hash_murmur3_one_32(r * 7919u + 17u) & 0xffffu) / 65536.0f * float(Math::TAU);
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
			// As the river layer (_place, LAYER_RIVER): Shields threshold and Sternberg fining.
			const float q = 0.5f * (rv.discharge[i] + rv.discharge[i + 1]);
			const float depth = 0.27f * Math::pow(MAX(q, 1e-3f), 0.4f);
			const float slope = CLAMP((a.y - b.y) / len, 1e-4f, 0.3f);
			const float dist = rv.distance[i];
			const float competent = depth * slope / (1.65f * 0.045f);
			const float supplied = 0.5f * Math::exp(-(dist + 0.5f * len) / 20000.0f);
			RiverSeg sg;
			sg.a = Vector2(a.x, a.z);
			sg.f = dir2 / len;
			sg.side = Vector2(-sg.f.y, sg.f.x);
			sg.len = len;
			sg.w = w;
			sg.d50 = CLAMP(MIN(competent, supplied), 0.005f, 1.2f);
			sg.s0 = dist;
			sg.phase = phase;
			sg.type = rv.head_type;
			r_segs.push_back(sg);
		}
	}
}

// The bed one segment gives at p: inside the river layer's band (0.6 of the width either side of the line), with the
// median coarse in the riffles and fine in the pools between them, about six widths apart (Leopold, Wolman and Miller
// 1964; Keller and Melhorn 1978), coarser down the thalweg than on the bars at the sides, and the pools' slack water
// draping fines over their bed (silt). False outside. p_texel: a map's texel; a bed narrower than it is spread over it,
// thinner by as much.
bool RockField3D::_river_bed(const RiverSeg &p_seg, const Vector2 &p_xz, RiverBed &r_bed, float p_texel) {
	const Vector2 d = p_xz - p_seg.a;
	const float t = d.dot(p_seg.f);
	if (t < -0.5f * p_seg.w || t > p_seg.len + 0.5f * p_seg.w) {
		return false;
	}
	const float we = MAX(p_seg.w, p_texel);
	const float u = Math::abs(d.dot(p_seg.side)) / MAX(we, 0.5f);
	if (u >= 0.6f) {
		return false;
	}
	const float s = p_seg.s0 + CLAMP(t, 0.0f, p_seg.len);
	const float riffle = 0.5f + 0.5f * Math::cos(float(Math::TAU) * s / (6.0f * MAX(p_seg.w, 1.0f)) + p_seg.phase);
	r_bed.band = (1.0f - Math::smoothstep(0.42f, 0.6f, u)) * MIN(p_seg.w / we, 1.0f);
	r_bed.d50 = p_seg.d50 * (0.5f + 0.9f * riffle) * (1.0f - 0.35f * Math::smoothstep(0.1f, 0.6f, u));
	r_bed.silt = (1.0f - riffle) * (1.0f - riffle) * (1.0f - 0.5f * Math::smoothstep(0.2f, 0.6f, u));
	r_bed.f = p_seg.f;
	r_bed.side = p_seg.side;
	r_bed.type = p_seg.type;
	return true;
}

/* Placement */

// A stone at rest: yawed, its up partly following the ground (p_follow_ground 0 level, 1 on the slope), tilted, sunk
// by a share of its height (and more on a slope, so the uphill side does not float).
void RockField3D::_rest(const TerrainGenerator3D *p_gen, Stone &r_stone, const Library &p_lib, const Vector3 &p_xz, real_t p_yaw, real_t p_sink, const Vector3 &p_tilt_axis, real_t p_tilt, real_t p_follow_ground) const {
	const int v = r_stone.variant;
	const Vector3 axes = p_lib.axes[v];
	const float s = r_stone.scale;
	// The drawn ground: the heightfield plus the landscape's cliff and bank relief (WorldGround.relief_at), so a stone
	// on a relief-drawn bank or face rests on the surface drawn there, not on the heightfield under or over it.
	const WorldGround *wg = WorldContracts::ground();
	auto H = [&](real_t p_x, real_t p_z) -> real_t {
		real_t h = rf_height(p_gen, Vector3(p_x, 0, p_z));
		if (wg) {
			const Vector2 r = wg->relief_at(Vector3(p_x, h, p_z));
			if (r.y > 0.0f && Math::is_finite(r.x)) {
				h += r.x;
			}
		}
		return h;
	};
	const real_t ground = H(p_xz.x, p_xz.z);
	Vector3 n;
	{
		const real_t e = MAX(real_t(0.5), real_t(axes.x * s));
		const real_t hx = H(p_xz.x + e, p_xz.z) - H(p_xz.x - e, p_xz.z);
		const real_t hz = H(p_xz.x, p_xz.z + e) - H(p_xz.x, p_xz.z - e);
		n = Vector3(-hx / (2.0f * e), 1.0f, -hz / (2.0f * e));
		n = Math::is_finite(n.x) && Math::is_finite(n.z) ? n.normalized() : Vector3(0, 1, 0);
	}
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
			ring += H(p_xz.x + Math::cos(a) * reach, p_xz.z + Math::sin(a) * reach);
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
				y = MIN(y, H(p_xz.x + pts[i].x, p_xz.z + pts[i].z) - pts[i].y - centre);
			}
		}
		y = MAX(y, floor_y);
	}
	r_stone.position = Vector3(p_xz.x, y, p_xz.z);
	r_stone.ground = ground;
	r_stone.footprint = MAX(axes.x, axes.z) * s * 0.85f;
}

void RockField3D::_place(Layer p_layer, const Rect2 &p_cell_rect, uint32_t p_seed, LocalVector<Stone> &r_stones, const HashMap<uint32_t, Library *> *p_libs, const LocalVector<DetailSeed> *p_seeds) {
	TerrainGenerator3D *gen = _get_generator();
	if (!gen || !_layer_on(p_layer) || layer_density[p_layer] <= 0.0) {
		return;
	}
	// Stones only on the world (past its edge the generator has no ground; rf_height holds their neighbourhood in it).
	const Rect2 p_rect = gen->get_result_rect().intersection(p_cell_rect);
	if (p_rect.size.x <= 0.0 || p_rect.size.y <= 0.0) {
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
		float pedestal = 0.0f; // m of soil column under it (rain splash)
	};
	LocalVector<Pending> pending;
	// A debris skirt round a big block: flakes and fragments it shed as it weathered (granular disintegration and
	// spalling) or broke off when it landed collect at its foot, more on the downhill side. A few to ten angular
	// chips of 4 to 15% of its size, a power law, in a ring just outside its footprint.
	auto skirt = [&](const Stone &p_big, RockGenerator::RockType p_type, float p_share) {
		const float b = 2.0f * p_big.scale;
		if (b < 0.8f || rf_far) {
			return;
		}
		// Its own randoms (from the block's place), so the stones after it come out the same with or without it.
		const Vector3i bq = Vector3i((p_big.position * 10.0f).floor());
		RandomPCG rng(rf_hash3(bq.x, bq.y, bq.z, p_seed ^ 0x5c1u));
		auto pick_variant = [&](const Library *p_lib) { return uint16_t(rng.rand() % p_lib->meshes.size()); };
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
			_rest(gen, st, *lib, p, rng.randf() * Math::TAU, 0.15 + 0.15 * rng.randf(), Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f), Math::deg_to_rad(25.0f) * rng.randf(), 0.8);
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
				// (Drawn before the size test, so a far block's big stones lie as their cell's do.)
				const real_t yaw = rng.randf() * Math::TAU, sink = 0.15 + 0.15 * rng.randf();
				const float tx = rng.randf() - 0.5f, tz = rng.randf() - 0.5f;
				const real_t tilt = Math::deg_to_rad(12.0f) * rng.randf();
				if (2.0f * st.scale < (rf_far ? MAX(min_size, 0.6 * far_min_size) : min_size)) {
					continue;
				}
				// (A small fragment lies on the talus more than in it.)
				_rest(gen, st, *lib, Vector3(b.x, 0, b.z), yaw, sink * MIN(real_t(1.0), real_t(0.4) + real_t(st.scale)), Vector3(tx, 0, tz), tilt, 0.5);
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
						const real_t yaw = rng.randf() * Math::TAU, sink = 0.25 + 0.25 * rng.randf();
						const float tx = rng.randf() - 0.5f, tz = rng.randf() - 0.5f;
						const real_t tilt = Math::deg_to_rad(15.0f) * rng.randf();
						if (d < (rf_far ? MAX(min_size, 0.6 * far_min_size) : min_size)) {
							continue;
						}
						_rest(gen, st, *lib, p, yaw, sink, Vector3(tx, 0, tz), tilt, 0.6);
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
						// Only where it can have stopped: the apron under 30 degrees across the block's own footprint (on
						// the wall above a thin cone it stood out of the cliff as a slab in the air).
						if (ground_normal(gen, pd.xz.x, pd.xz.z, MAX(real_t(1.0), real_t(0.5f * pd.size))).y >= 0.87f) {
							pending.push_back(pd);
							lay(pd.xz.x, pd.xz.z, 0.45f * pd.size);
						}
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
					const real_t gx = (rf_height(gen, tr.xz + Vector3(e, 0, 0)) - rf_height(gen, tr.xz - Vector3(e, 0, 0))) / (2.0 * e);
					const real_t gz = (rf_height(gen, tr.xz + Vector3(0, 0, e)) - rf_height(gen, tr.xz - Vector3(0, 0, e))) / (2.0 * e);
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
			for (uint32_t r = 0; r < rivers.size(); r++) {
				const River &rv = rivers[r];
				if (!rv.bounds.grow(20.0).intersects(p_rect)) {
					continue;
				}
				// The riffles' phase along this river, as _river_segments gives the detail bed.
				const float phase = float(hash_murmur3_one_32(r * 7919u + 17u) & 0xffffu) / 65536.0f * float(Math::TAU);
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
					// The count over the bed's real width; laid over a bed at least three medians wide, so a rill
					// narrower than its stones does not string them out single file along its line (a bed's
					// coarse grains sit side by side across it).
					const float we = MAX(w, 3.0f * d50);
					// Two tries per stone kept: the riffles keep the coarse ones, the pools between them about six
					// widths apart hold few (Leopold, Wolman and Miller 1964; the detail bed's riffles, same phase),
					// so the stones come in clusters, not at an even spacing.
					const float want = 0.45f * area * float(density) / (float(Math::PI) * 0.25f * d50 * d50 * 1.4f);
					const int count = MIN(int(2.0f * want), 2 * max_stones_per_cell);
					const float riffle_len = 6.0f * MAX(w, 1.0f);
					for (int k = 0; k < count; k++) {
						const float t = rng.randf();
						const float s_at = rv.distance[i] + t * len;
						const float riffle = 0.5f + 0.5f * Math::cos(float(Math::TAU) * s_at / riffle_len + phase);
						if (rng.randf() > 0.1f + 0.9f * riffle * riffle) {
							continue;
						}
						// The thalweg swings from bank to bank between riffles (alternate bars, twice the riffle
						// spacing), and the coarse stones gather about it: no straight row along the line.
						const float swing = 0.3f * Math::sin(float(Math::TAU) * s_at / (2.0f * riffle_len) + 1.7f * phase);
						const float u = CLAMP(swing + (rng.randf() + rng.randf() - 1.0f) * 0.6f, -0.6f, 0.6f);
						const Vector2 p = Vector2(a.x, a.z).lerp(Vector2(b.x, b.z), t) + side * (u * we);
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
						// Imbrication: long axis across the flow, the stone dipping upstream (15 to 30 degrees). The a axis
						// is a line, so either end may point to the left bank (half are turned round), and its bearing
						// spreads some 25 degrees either way (imbricated gravels' a-axis fabrics are broad); neighbours of
						// one shape no longer lie as twins.
						pd.yaw = Math::atan2(-f.x, -f.y) + (rng.randf() - 0.5f) * 0.9f + (rng.randf() < 0.5f ? float(Math::PI) : 0.0f);
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
					sc.e = float(rf_height(gen, c) - level);
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
							pd.size = d50 * Math::exp(rng.randfn(0.0f, d50 > 0.3f ? 0.5f : 0.35f)); // (boulder beaches sort less)
						}
						pd.size = CLAMP(pd.size, float(min_size), 1.5f);
						// Four shape bins of pebbles (the swash's sorting by shape: discs and blades up the beach, spheres and
						// rods rolled down; Bluck 1967), each its own variants, so neighbours are rarely twins.
						// The bigger ones from the two most-worn cobble bins: rounder and less round, so no two read alike.
						// Boulders: the biggest moved least, so less worn (blocky, the joints' faces still there; Oak 1984);
						// all three cobble bins, each its own shapes.
						const int cbin = pd.size > 0.6f ? int(rng.rand() % 2) : 1 + int(rng.rand() % 2);
						pd.key = pd.size < 0.25f ? _library_key(type, RockGenerator::FAMILY_PEBBLE, int(rng.rand() % 4)) : _library_key(type, RockGenerator::FAMILY_COBBLE, cbin);
						pd.yaw = rng.randf() * Math::TAU;
						pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
						pd.tilt = Math::deg_to_rad(pd.size > 0.4f ? 25.0f : 10.0f) * rng.randf();
						// Set into the shingle, a quarter to two fifths deep (the swash packs the gaps round them); a
						// boulder anything from perched to half buried.
						pd.sink = pd.size > 0.4f ? 0.12f + 0.4f * rng.randf() : 0.25f + 0.15f * rng.randf();
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
			const float course = 1.4f; // m, the mean bed thickness (the face-fit tolerances scale by it)
			const float min_tan = Math::tan(Math::deg_to_rad(50.0f));
			// Up to near vertical: past about 50 to 60 degrees a heightfield face is a stretched step whose few vertices
			// draw long vertical bands (draped folds) that no texture hides, so the wall's own blocks cover it there
			// as real geometry (cliff kit pieces placed over steep heightfield faces: Guerrilla, GDC 2017, "GPU-Based
			// Run-Time Procedural Placement in Horizon Zero Dawn"; Carrier, GDC 2018, "Procedural World Generation of
			// Far Cry 5"). docs/ideas/rocks.md, "Wall blocks on steep faces".
			const float max_tan = Math::tan(Math::deg_to_rad(float(CLAMP(cliff_block_max_slope, real_t(50.0), real_t(89.0)))));
			const float sheer_tan = Math::tan(Math::deg_to_rad(50.0f)) * 0.999f; // every face this layer takes
			// Beds of uneven thickness (bed thickness in a sedimentary pile is near log-normal, not a fixed course): the
			// height is cut into packages of about 3.6 m with jittered tops, each split into one to three beds of 0.4 m
			// up. Every cell of every wall finds the same beds at the same heights, so a bed runs on along the cliff.
			const float bed_pkg = 3.6f;
			auto bed_hash = [](int64_t p_i, uint32_t p_salt) { return hash_fmix32(hash_murmur3_one_32(uint32_t(p_i) ^ uint32_t(p_i >> 32) * 0x9e3779b1u, p_salt)); };
			auto u01 = [](uint32_t p_h) { return float(p_h & 0xffffu) / 65535.0f; };
			auto pkg_base = [&](int64_t p_p) { return (float(p_p) + 0.38f * (u01(bed_hash(p_p, 0x6b3du)) - 0.5f)) * bed_pkg; };
			auto beds_in = [&](int64_t p_p, float *r_bounds) {
				const float b0 = pkg_base(p_p), b1 = pkg_base(p_p + 1);
				const uint32_t h = bed_hash(p_p, 0x51f3u) % 100u;
				const int nb = h < 20u ? 1 : (h < 65u ? 2 : 3);
				r_bounds[0] = b0;
				for (int i = 1; i < nb; i++) {
					r_bounds[i] = b0 + (b1 - b0) * (float(i) + 0.6f * (u01(bed_hash(p_p * 4 + i, 0x3e77u)) - 0.5f)) / float(nb);
				}
				r_bounds[nb] = b1;
				return nb;
			};
			const real_t gen_cell = gen->get_cell_size();
			if (ledge_slabs) {
				_place_ledges(gen, p_layer, p_rect, p_seed, r_stones);
			}
			if (rf_ledges_only) {
				break; // the wall blocks stand in faces too steep for plants: no obstacles
			}
			for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += sub) {
				for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += sub) {
					const Vector3 c(x + sub * 0.5f, 0, z + sub * 0.5f);
					const float rock = float(gen->get_rock_exposure(c));
					if (rock < 0.15f) {
						continue; // under either threshold below, before the heights (each a full ground evaluation)
					}
					const real_t hc = rf_height(gen, c);
					const real_t e = 1.0;
					const Vector2 grad((rf_height(gen, c + Vector3(e, 0, 0)) - rf_height(gen, c - Vector3(e, 0, 0))) / (2.0 * e), (rf_height(gen, c + Vector3(0, 0, e)) - rf_height(gen, c - Vector3(0, 0, e))) / (2.0 * e));
					const float gl = grad.length();
					if (!(gl >= min_tan && gl <= max_tan) || !Math::is_finite(hc)) {
						continue; // also off the generated world, where the heights are NaN
					}
					// Soil does not hold past about 45 degrees: a sheer face is bare rock whatever the exposure map's
					// blend says (it fades rock out over talus and flutes the walls carry).
					if (rock < (gl > sheer_tan ? 0.15f : 0.5f)) {
						continue;
					}
					const Vector2 up_dir = grad / gl; // uphill, in plan
					// Only where the terrain mesh really has this face: across one terrain cell up and down the fall
					// line the face must stay near planar. Where the heightfield cannot hold the wall (a near-vertical
					// step between two vertices) the mesh and the bilinear height part by metres, and blocks set into
					// the one float in front of the other.
					{
						// Measured square to the face: on a steep face metres of height between the bilinear surface and
						// the mesh's triangles are only centimetres across the face, which a block set into it covers.
						// Over the face's own extent: a near-vertical wall is only a metre or two across in plan, and a
						// whole terrain cell up and down the fall line lands on the bench above and the foot below.
						const real_t span = MIN(gen_cell, MAX(real_t(1.0), real_t(6.0f * course / gl)));
						const Vector3 u(up_dir.x * span, 0, up_dir.y * span);
						const real_t bend = rf_height(gen, c + u) + rf_height(gen, c - u) - 2.0 * hc;
						if (!(Math::abs(bend) / Math::sqrt(1.0f + gl * gl) <= course * (gl > sheer_tan ? 0.5f : 0.32f))) {
							continue;
						}
					}
					// The span of heights the face covers inside this cell, along the fall line.
					const float half = float(sub) * 0.5f;
					const float y0 = float(hc) - gl * half, y1 = float(hc) + gl * half;
					// Past 50 degrees the face is the stretched heightfield step (vertical flutes) the blocks are there to cover:
					// beds nearly closed there, and hard beds stand proud of soft ones.
					const bool sheer = gl > sheer_tan;
					const Vector2 strike(-up_dir.y, up_dir.x);
					// Joints are spaced along one world axis (the one nearer the strike), so neighbouring cells of the same
					// wall agree on where a bed's joints fall even as the strike turns a little between them.
					const bool along_x = Math::abs(strike.x) >= Math::abs(strike.y);
					const float strike_comp = along_x ? strike.x : strike.y;
					const Rect2 sub_rect(c.x - half, c.z - half, float(sub), float(sub));
					const int64_t p0 = int64_t(Math::floor(y0 / bed_pkg)) - 1, p1 = int64_t(Math::floor(y1 / bed_pkg)) + 1;
					for (int64_t p = p0; p <= p1; p++) {
						float bounds[4];
						const int nb = beds_in(p, bounds);
						for (int i = 0; i < nb; i++) {
							const float yb0 = bounds[i], yb1 = bounds[i + 1];
							const float bed = yb1 - yb0;
							const float yk = 0.5f * (yb0 + yb1);
							const float along = (yk - float(hc)) / gl;
							if (Math::abs(along) > half) {
								continue;
							}
							const uint32_t bed_id = bed_hash(p * 4 + i, 0x2c1bu);
							const float hard = u01(bed_id); // 1: a hard bed (stands out, few gaps), 0: a soft one
							// Joint spacing scales with the bed's thickness (Narr and Suppe 1991; Bai and Pollard 2000),
							// 1 to 2.6 times it, its own ratio per bed.
							const float spacing = bed * (1.0f + 1.6f * u01(bed_hash(p * 4 + i, 0x71a9u))) / MAX(Math::abs(strike_comp), 0.7f);
							const Vector2 pxz = Vector2(c.x, c.z) + up_dir * along;
							const float s_here = along_x ? pxz.x : pxz.y;
							const int64_t j0 = int64_t(Math::floor((s_here - float(sub)) / spacing)) - 1;
							const int64_t j1 = int64_t(Math::floor((s_here + float(sub)) / spacing)) + 1;
							for (int64_t j = j0; j <= j1; j++) {
								// Joints sit off the even spacing by up to 0.3 of it either way, and a quarter of them never
								// opened, so a block spans two or three spacings there: blocks along one bed differ in length.
								// The block fills the span between two open joints, less the open joint itself.
								auto joint_open = [&](int64_t p_j) { return (bed_hash(p_j, bed_id ^ 0x5bd1u) & 3u) != 0u; };
								if (!joint_open(j) && (joint_open(j - 1) || joint_open(j - 2))) {
									continue; // this stretch belongs to the block from the open joint before it
								}
								int64_t jn = j + 1;
								while (!joint_open(jn) && jn < j + 3) {
									jn++;
								}
								const float ja = (float(j) + 0.6f * (u01(bed_hash(j, bed_id)) - 0.5f)) * spacing;
								const float jb = (float(jn) + 0.6f * (u01(bed_hash(jn, bed_id)) - 0.5f)) * spacing;
								const Vector2 bp = pxz + strike * ((0.5f * (ja + jb) - s_here) / strike_comp);
								if (!sub_rect.has_point(bp) || !p_rect.has_point(bp)) {
									continue;
								}
								if (rng.randf() > (sheer ? 0.72f + 0.26f * hard : 0.75f * rock - 0.1f) * float(density)) {
									continue; // a gap: a block weathered out, the wall shows behind
								}
								const float span = (jb - ja) * Math::abs(strike_comp); // along the strike, metres
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
								// Of three shapes, the one whose length to height best matches the joint span to the bed.
								{
									const float want = span / MAX(bed, 0.05f);
									float best = 1e9f;
									for (int t = 0; t < 3; t++) {
										const uint16_t v = pick_variant(lib);
										const Vector3 ax = lib->axes[v];
										const float d = Math::abs(Math::log(MAX(ax.x, 0.01f) / MAX(ax.y, 0.01f) / want));
										if (d < best) {
											best = d;
											st.variant = v;
										}
									}
								}
								const Vector3 axes = lib->axes[st.variant];
								// Fits the bed and the joint span, less the open joints (shadowed lines between blocks).
								const float fill = 0.86f + 0.08f * rng.randf();
								st.scale = fill * MIN(bed / MAX(2.0f * axes.y, 0.05f), 1.2f * span / MAX(2.0f * axes.x, 0.05f)); // a block may lap a little past its joint
								// Long axis along the strike, short axis on the bed normal, b axis into the wall. The bed
								// normal is turned most of the way toward the face's own up-slope direction: a block kept
								// square to level beds on a 50 to 76 degree face pushed its top out of it as a pale shelf
								// (from afar a flake stuck on the slope); turned with the face it lies nearly flush.
								const Vector3 fn = Vector3(-grad.x, 1.0f, -grad.y).normalized();
								const Vector3 face_up = (Vector3(0, 1, 0) - fn * fn.y).normalized();
								const Vector3 y_axis = bed_normal.slerp(face_up, 0.75f).normalized();
								Vector3 x_axis = Vector3(strike.x, 0, strike.y);
								x_axis = (x_axis - y_axis * y_axis.dot(x_axis)).normalized();
								const Vector3 z_axis = x_axis.cross(y_axis).normalized();
								Basis b;
								b.set_column(0, x_axis);
								b.set_column(1, y_axis);
								b.set_column(2, z_axis);
								b = Basis(y_axis, (rng.randf() - 0.5f) * 0.25f) * b;
								if (sheer) {
									b = Basis(x_axis, (rng.randf() - 0.5f) * 0.1f) * b; // a block settled on its bed
								}
								// The face's outward normal at the crossing; the block's centre that far into the rock.
								const Vector3 n = fn;
								const Vector3 surface(bp.x, yk, bp.y);
								// Set deep on a sloping face: a block standing a third of its depth out of a 50 to 76 degree
								// face pushed its level top out as a pale shelf. On a sheer wall the beds weather apart
								// (differential erosion): a hard bed stands out half its depth, a soft one a fifth, sitting back
								// in the face, so the courses read as relief with shadowed bed lines, not tiles laid flat.
								// Blended from 50 to 65 degrees: on a face just past 50 a block lies nearly flush (proud, its top shows as a pale
								// shelf), on a sheer one the beds stand out.
								const float sheer_t = CLAMP((Math::rad_to_deg(Math::atan(gl)) - 50.0f) / 15.0f, 0.0f, 1.0f);
								const float set_in = Math::lerp(0.86f + 0.12f * rng.randf(), 0.5f + 0.3f * (1.0f - hard) + 0.08f * rng.randf(), sheer_t);
								const Vector3 centre = surface - n * (axes.z * st.scale * set_in);
								// The centre must lie inside the rock (under the terrain at its own plan position), or the
								// block stands out of a bend in the face rather than being set into it.
								if (rf_height(gen, centre) < centre.y + 0.1f * bed) {
									continue;
								}
								// And the face is where the cell's plane put it: at the crossing and at both ends of the
								// block (along the strike) the ground stands near the block's face, measured square to the
								// face. Where the real face bends away or steps back (a gully, a convex corner), the block
								// would stand out of it in the air, read from afar as a slab hung on the cliff. On a sheer
								// wall a block may stand proud of a fold (a joint block jutting from a wall).
								{
									bool out = false;
									const real_t ext = axes.x * st.scale * 0.9;
									for (int e = -1; e <= 1 && !out; e++) {
										const Vector3 q = surface + x_axis * (ext * e);
										const real_t gap = (q.y - rf_height(gen, q)) * n.y; // + where the face lies under the block's face
										out = gap > (sheer ? 0.45f : 0.2f) * course;
									}
									if (out) {
										continue;
									}
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
					const real_t h = rf_height(gen, c);
					real_t ring = 0.0;
					for (int k = 0; k < 6; k++) {
						const real_t a = real_t(k) * Math::TAU / 6.0;
						ring += rf_height(gen, c + Vector3(Math::cos(a) * 15.0, 0, Math::sin(a) * 15.0));
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
							real_t gmin = rf_height(gen, xz);
							gmin = MIN(gmin, rf_height(gen, xz + along * ha));
							gmin = MIN(gmin, rf_height(gen, xz - along * ha));
							gmin = MIN(gmin, rf_height(gen, xz + side_dir * hs));
							gmin = MIN(gmin, rf_height(gen, xz - side_dir * hs));
							base_y = MIN(y, gmin - 0.1 * ax.y * st.scale);
						}
						st.position = Vector3(xz.x, base_y + bottom, xz.z);
						st.ground = p_sea ? y - 50.0 : rf_height(gen, xz);
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
				const real_t ground = rf_height(gen, cr.p);
				// A tor needs a summit broad enough for its base (a knife ridge sheds its blocks): the ground a few metres
				// out from the top falls no more than a third of the way down.
				real_t fall = 0.0;
				for (int k = 0; k < 6; k++) {
					const real_t a = real_t(k) * Math::TAU / 6.0;
					fall = MAX(fall, ground - rf_height(gen, cr.p + Vector3(Math::cos(a) * 4.0, 0, Math::sin(a) * 4.0)));
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
							const float rise = float(rf_height(gen, c + d * r) - level);
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
			const real_t bed = rf_height(gen, sp.p);
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
					if (rf_height(gen, foot) > level + 1.0) {
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
				const real_t over = MAX(rf_height(gen, at), rf_height(gen, foot - sp.to_land * 3.0)) - (level + 0.6);
				st.position = Vector3(at.x, level + 0.6 - ax.y * st.scale, at.z); // above mean sea level: bare at low tide
				st.ground = rf_height(gen, at);
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
					if (gen->get_water_depth(c) > 0.0 || rf_height(gen, c) < shore_top) {
						continue;
					}
					const RockGenerator::RockType type = _type_at(gen, Vector3(c.x, rf_height(gen, c), c.z));
					const Vector3 n = ground_normal(gen, c.x, c.z, 4.0);
					const float exposure = float(gen->get_rock_exposure(c));
					const float thin = 1.0f - Math::smoothstep(0.2f, 0.8f, float(gen->get_soil_depth(c)));
					const float bare = MAX(exposure, thin);
					if (type == RockGenerator::ROCK_LIMESTONE && n.y > 0.94f && bare > 0.35f) { // up to 20 degrees: stepped pavements (Malham, the Burren) follow gentle slopes
						// A bench, not a knoll or a col: the ground within 8 m stays on the plane through the centre.
						const real_t h0 = rf_height(gen, c);
						const real_t sx = -n.x / MAX(n.y, real_t(0.1)), sz = -n.z / MAX(n.y, real_t(0.1));
						float off = 0.0f;
						for (int j = 0; j < 8; j++) {
							const Vector3 d = Vector3(Math::cos(j * Math::TAU / 8.0), 0, Math::sin(j * Math::TAU / 8.0)) * 8.0;
							off = MAX(off, float(Math::abs(rf_height(gen, c + d) - (h0 + sx * d.x + sz * d.z))));
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
						const real_t hf = rf_height(gen, foot);
						const bool wall = rf_height(gen, foot - Vector3(down.x, 0, down.y) * 12.0) > hf + 8.0;
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
					if (rf_height(gen, at) < shore_top || gen->get_water_depth(at) > 0.5) {
						continue; // the delta and the lake: sediment, not a scoured bed
					}
					if (gen->get_rock_exposure(at) < 0.3 && slope < 0.08f) {
						continue; // a gravel bed: the bedrock is under it
					}
					run = -40.0 * rng.randf();
					const RockGenerator::RockType type = _type_at(gen, Vector3(at.x, rf_height(gen, at), at.z));
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
					const real_t g = rf_height(gen, at);
					const real_t half_l = real_t(box.size.x) * st.scale * 0.4, half_w = real_t(box.size.z) * st.scale * 0.4;
					real_t worst = 0.0, above = 0.0;
					for (int ia = -1; ia <= 1; ia++) {
						for (int ic = -1; ic <= 1; ic++) {
							const Vector3 o = fxx * (real_t(ia) * half_l) + fz * (real_t(ic) * half_w);
							const real_t r = rf_height(gen, at + Vector3(o.x, 0, o.z)) - (g + o.y);
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
				const RockGenerator::RockType type = _type_at(gen, Vector3(s.p.x, rf_height(gen, s.p), s.p.z));
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
					real_t g = rf_height(gen, s.p);
					real_t back = -1.0;
					for (real_t t = 0.0; t <= 14.0; t += 0.5) {
						if (rf_height(gen, s.p - out * t) > g + 1.0) {
							back = MAX(t - 1.0, real_t(0.0));
							break;
						}
					}
					if (back < 0.0) {
						continue;
					}
					const Vector3 base = s.p - out * back;
					for (int j = -2; j <= 2; j++) {
						g = MIN(g, rf_height(gen, base + strike * real_t(j) * 1.5));
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
								while (t < 12.0 && rf_height(gen, p + out * t) > p.y) {
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
						if (rf_coarse_job) {
							// On a worker: the generator's holes, their signal and the Terrain3D are the main thread's;
							// the job carries the hole there (_apply_holes).
							static_cast<CoarseJob *>(rf_coarse_job)->holes.push_back(hb);
						} else {
							LocalVector<Transform3D> one;
							one.push_back(hb);
							_apply_holes(one);
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
						const real_t gb = rf_height(gen, at);
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
						real_t g = rf_height(gen, at);
						for (int j = 0; j < 6; j++) {
							g = MIN(g, rf_height(gen, at + Vector3(Math::cos(j * Math::TAU / 6.0), 0, Math::sin(j * Math::TAU / 6.0)) * r));
						}
						st.position = Vector3(at.x, g - 0.06 * height - box.position.x * st.scale, at.z);
						st.ground = rf_height(gen, at);
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
						real_t g = rf_height(gen, at);
						for (int j = -2; j <= 2; j++) {
							for (int sd = -1; sd <= 1; sd += 2) {
								g = MIN(g, rf_height(gen, at + al * (real_t(j) * 0.22 * length) + ac * (real_t(sd) * real_t(box.size.y) * st.scale * 0.5)));
							}
						}
						const real_t height = real_t(box.size.z) * st.scale;
						st.position = Vector3(at.x, g - 0.04 * height - box.position.z * st.scale, at.z);
						st.ground = rf_height(gen, at);
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
					const real_t g = rf_height(gen, s.p);
					st.position = Vector3(s.p.x, g + 0.15 - ax.y * st.scale, s.p.z);
					st.ground = g - 0.4; // bedrock at the surface: the ground-blend band only at its buried edges
					r_stones.push_back(st);
				} else {
					// Hoodoos: 3 to 9 columns in a row along a joint, 2 to 10 m apart, 3 to 20 m tall, the foot sunk in
					// the debris apron.
					const uint32_t key = _library_key(type, RockGenerator::FAMILY_HOODOO, 0);
					Library *lib = _ensure_library(key);
					if (!lib) {
						continue;
					}
					const real_t yaw = rng.randf() * Math::TAU;
					const Vector3 along(Math::cos(yaw), 0, Math::sin(yaw));
					// (A cluster: the soft bed the row stands on runs on, and often a second row on the bed above or
					// below, a few metres across, so they stand in groups rather than as lone posts.)
					const int count = 5 + int(rng.randf() * 9.99f);
					const Vector3 across(-along.z, 0, along.x);
					// No two alike in a row (Bryce, Goreme): the row's own height, each column well off it, a lean of a
					// few degrees, spacing from 2 to 10 m with gaps where one fell, and the line wandering across.
					const real_t row_h = 5.0 + 9.0 * rng.randf();
					Vector3 at = s.p - along * real_t(count) * 2.8;
					real_t drift = 0.0;
					for (int k = 0; k < count; k++) {
						const real_t u = rng.randf();
						at += along * real_t(2.0 + 6.0 * u * u);
						drift = CLAMP(drift + (rng.randf() - 0.5f) * 3.0f, real_t(-3.5), real_t(3.5));
						if (k % 2 == 1 && rng.randf() < 0.6f) {
							drift += (rng.randf() < 0.5f ? -1.0f : 1.0f) * (4.0f + 3.0f * rng.randf()); // the second row
						}
						const Vector3 p_at = at + across * drift + along * (rng.randf() - 0.5f) * 1.5f;
						const float hu = rng.randf(), tilt = rng.randf(), tilt_az = rng.randf() * float(Math::TAU), spin = rng.randf();
						if (gen->get_water_depth(p_at) > 0.0 || rng.randf() < 0.15f) {
							continue; // in water, or fallen
						}
						Stone st;
						st.layer = p_layer;
						st.library = key;
						st.variant = pick_variant(lib);
						const AABB box = lib->meshes[st.variant]->get_aabb();
						const real_t height = CLAMP(row_h * (0.45 + 1.1 * hu), real_t(3.0), real_t(20.0));
						st.scale = height / MAX(real_t(box.size.x), real_t(0.5));
						const Vector3 tilt_axis(Math::cos(tilt_az), 0, Math::sin(tilt_az));
						const Basis b = Basis(tilt_axis, Math::deg_to_rad(1.0f + 7.0f * tilt * tilt)) * Basis(Vector3(0, 1, 0), yaw + spin * Math::TAU) * Basis(Vector3(0, 0, 1), Math::PI * 0.5);
						st.basis = b.scaled(Vector3(st.scale, st.scale, st.scale));
						// The lowest ground under its foot, so no side hangs over the slope.
						const real_t r = real_t(box.size.y) * st.scale * 0.5;
						real_t g = rf_height(gen, p_at);
						for (int j = 0; j < 6; j++) {
							g = MIN(g, rf_height(gen, p_at + Vector3(Math::cos(j * Math::TAU / 6.0), 0, Math::sin(j * Math::TAU / 6.0)) * r));
						}
						st.position = Vector3(p_at.x, g - 0.12 * height, p_at.z) - b.xform(Vector3(box.position.x, 0, 0)) * st.scale;
						st.ground = rf_height(gen, p_at);
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
			// River beds: the river layer's own bed carried on below min_size, as the shore's (the rivers are read only
			// once prepared on the main thread: _detail_prepare, build_area).
			LocalVector<RiverSeg> rsegs;
			if (detail_rivers && layer_enabled[LAYER_RIVER] && sd2 > sd1 * 1.01f && rivers_ready) {
				_river_segments(p_rect.grow(1.0), rsegs);
			}
			// Worn ground (docs/ideas/ground-detail.md, "Game trails and boar rooting"): a trail's tread loses its fines and
			// small gravel and keeps an armour of angular stones standing proud, the kicked grains gather on its shoulders,
			// and boar rooting leaves stones turned up loose on the soil, settling over two years.
			const StandWear *wear = static_cast<const StandWear *>(rf_job_wear);
			const bool wear_on = wear && wear->any && sd2 > sd1 * 1.01f;
			const int cls = int(p_layer - LAYER_DETAIL_PEBBLE);
			// The share of the ground each class covers at full wear: the tread's armour (Marion and Leung: the soil goes,
			// the coarse fraction stays), the shoulders' kicked grains, and the stones a fresh rooting turns up; as counts
			// per m2 of the class's mean footprint, times the soil's stoniness (more where the bedrock is near).
			static const float armour_cover[3] = { 0.15f, 0.10f, 0.05f };
			static const float kick_cover[3] = { 0.01f, 0.08f, 0.12f };
			static const float root_cover[3] = { 0.06f, 0.05f, 0.06f };
			const float per_cover = 1.0f / (0.785f * sd1 * sd2);
			// Frost-sorted ground: stones gathered into the borders of circles and stripes on cold, wet, gentle soil.
			const FrostClimate *frost = static_cast<const FrostClimate *>(rf_job_frost);
			const bool frost_on = frost && frost->any && sd2 > sd1 * 1.01f;
			// The share of the ground each class covers in the stone domains (the borders are clast-supported: Washburn
			// 1956; Hallet 2013); stones of the coarse end, frost-heaved.
			const float *frost_cover = rf_frost_cover;
			// Storm runoff's gravel (docs/ideas/ground-detail.md, "Gravel sorted by storm runoff"): the rills and gullies
			// the terrain's droplet erosion cut (TerrainGenerator3D.get_gully_at, the rule its material draws their gravel
			// by) keep a lag of the coarse grains in their floors, the fines washed out (Poesen et al. 2003), and drop
			// their load as a spread of fine grains on the fans where the slope eases.
			const bool runoff_on = runoff_gravel && sd2 > sd1 * 1.01f;
			// The share of the ground each class covers on a channel floor (clast-supported where the flow ran
			// strongest) and on a fan.
			static const float lag_cover[3] = { 0.35f, 0.25f, 0.06f };
			static const float fan_cover[3] = { 0.0f, 0.04f, 0.12f };
			// Stones on bare soil under rain (docs/ideas/ground-detail.md, "Rain splash pedestals"): the soil's own coarse
			// fraction lying on it where plants leave it bare (a dry climate, a recent burn), each on the column of soil
			// it shielded from the splash. The share of the ground each class covers at full bareness.
			const RainClimate *rain = static_cast<const RainClimate *>(rf_job_rain);
			const bool rain_on = rain && rain->any && sd2 > sd1 * 1.01f;
			static const float soil_cover[3] = { 0.10f, 0.06f, 0.03f };
			if (!shore_on && !scree_on && rsegs.is_empty() && !wear_on && !frost_on && !runoff_on && !rain_on) {
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
				float river = 0.0f; // and from a river bed's log-normal
				RiverBed bed;
				float gully = 0.0f; // a rill or gully floor's share (as the terrain material's), 0..1
				float fan = 0.0f; // the fan below it, 0..1
				Vector2 flow; // downhill
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
							const float e = float(rf_height(gen, c) - level);
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
					if (runoff_on) {
						// As the terrain material: the channel floor where the slope-area index and the flow's concentration
						// are high (not on bare rock), the fan where the deposit lies on easing ground.
						const Vector3 n = ground_normal(gen, c.x, c.z, 1.0);
						const Vector2 gl = gen->get_gully_at(c, n.y);
						if (gl.x > 0.0f || gl.y > 0.0f) {
							const float rk = CLAMP(float(gen->get_rock_exposure(c)), 0.0f, 1.0f);
							dc.gully = CLAMP(float(gl.x) * (1.0f - rk), 0.0f, 1.0f);
							dc.fan = Math::smoothstep(0.15f, 0.8f, float(gl.y)) * Math::smoothstep(0.85f, 0.93f, float(n.y));
							const Vector2 f(n.x, n.z);
							dc.flow = f.length() > 1e-4f ? f.normalized() : Vector2(0, 1);
						}
					}
				}
			}
			// Each river segment over the corners it reaches; where two overlap (a bend, a junction) the one deeper inside.
			for (const RiverSeg &sg : rsegs) {
				const Rect2 r = Rect2(sg.a, Vector2()).expand(sg.a + sg.f * sg.len).grow(0.6f * sg.w + 0.5f);
				const int i0 = MAX(int(Math::floor((r.position.x - p_rect.position.x) / sub)), 0), i1 = MIN(int(Math::ceil((r.get_end().x - p_rect.position.x) / sub)), nx);
				const int j0 = MAX(int(Math::floor((r.position.y - p_rect.position.y) / sub)), 0), j1 = MIN(int(Math::ceil((r.get_end().y - p_rect.position.y) / sub)), nz);
				for (int j = j0; j <= j1; j++) {
					for (int i = i0; i <= i1; i++) {
						DetailCorner &dc = corners[j * (nx + 1) + i];
						RiverBed bed;
						if (!_river_bed(sg, Vector2(p_rect.position.x + i * sub, p_rect.position.y + j * sub), bed) || bed.band <= dc.bed.band) {
							continue;
						}
						dc.bed = bed;
						// A gravel bed is clast-supported like shingle; the pools' drape of fines hides most of it.
						const float sig = 0.6f * float(Math::SQRT2);
						const float share = 0.5f * (std::erfc(Math::log(sd1 / bed.d50) / sig) - std::erfc(Math::log(sd2 / bed.d50) / sig));
						dc.river = MIN(0.85f * bed.band * share * (1.0f - 0.75f * bed.silt) / (0.785f * sd1 * sd2), cap_shore);
					}
				}
			}
			const float pa = Math::pow(pd1, -2.5f), pb = Math::pow(pd2, -2.5f);
			const float ka = Math::pow(sd1, -2.5f), kb = Math::pow(sd2, -2.5f);
			for (int j = 0; j < nz; j++) {
				for (int i = 0; i < nx; i++) {
					const DetailCorner *cc[4] = { &corners[j * (nx + 1) + i], &corners[j * (nx + 1) + i + 1], &corners[(j + 1) * (nx + 1) + i], &corners[(j + 1) * (nx + 1) + i + 1] };
					float rate_max = 0.0f;
					for (const DetailCorner *k : cc) {
						rate_max = MAX(rate_max, k->shore + k->scree + k->river);
					}
					// The worn ground's own stones per m2 at its most over the sub-cell (it is sampled finer than the
					// corners: a trail's tread is narrower than a sub-cell), times the soil's stoniness (more where the
					// bedrock is near). Each stone reads it where it falls.
					// The frost gate over the sub-cell (climate, slope, soil), at its middle.
					float fgate = 0.0f, fslope = 0.0f;
					if (frost_on) {
						Vector3 fn;
						fgate = _frost_gate(gen, *frost, Vector3(p_rect.position.x + (i + 0.5f) * sub, 0, p_rect.position.y + (j + 0.5f) * sub), fn);
						// Circles or stripes by the hillside's slope over a few metres (hummocks do not turn circles into stripes).
						fslope = Math::rad_to_deg(Math::acos(CLAMP(float(fn.y), -1.0f, 1.0f)));
						rate_max += MIN(frost_cover[cls] * per_cover * fgate, cap_shore);
					}
					if (runoff_on) {
						float gm = 0.0f, fm = 0.0f;
						for (const DetailCorner *k : cc) {
							gm = MAX(gm, k->gully);
							fm = MAX(fm, k->fan);
						}
						rate_max += MIN(lag_cover[cls] * per_cover * gm, cap_shore) + MIN(fan_cover[cls] * per_cover * fm, cap_shore);
					}
					float rsoil = 0.0f, rdepth = 0.0f;
					if (rain_on) {
						rsoil = MIN(soil_cover[cls] * per_cover * _rain_soil(gen, *rain, Vector3(p_rect.position.x + (i + 0.5f) * sub, 0, p_rect.position.y + (j + 0.5f) * sub), rdepth), cap_shore);
						rate_max += rsoil;
					}
					Vector3 wmax;
					float stony = 0.0f;
					if (wear_on) {
						wmax = wear->max_in(Rect2(p_rect.position.x + i * sub, p_rect.position.y + j * sub, sub, sub));
						if (wmax != Vector3()) {
							const Vector3 mid(p_rect.position.x + (i + 0.5f) * sub, 0, p_rect.position.y + (j + 0.5f) * sub);
							stony = per_cover * (0.5f + 0.5f * CLAMP(2.0f * float(gen->get_rock_exposure(mid)), 0.0f, 1.0f));
							rate_max += MIN(armour_cover[cls] * stony * float(wmax.x), cap_shore) + MIN(kick_cover[cls] * stony * float(wmax.y), cap_shore) + MIN(root_cover[cls] * stony * float(wmax.z), cap_shore);
						}
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
						float shore = 0.0f, scree = 0.0f, river = 0.0f, d50 = 0.0f, e = 0.0f, ws = 0.0f;
						float rd50 = 0.0f, silt = 0.0f, wr = 0.0f;
						float worn = 0.0f, kick = 0.0f, root = 0.0f;
						float soil = rsoil, sdepth = rdepth;
						const RiverBed *bed = nullptr;
						for (int q = 0; q < 4; q++) {
							shore += w[q] * cc[q]->shore;
							scree += w[q] * cc[q]->scree;
							river += w[q] * cc[q]->river;
							if (cc[q]->bed.band > 0.0f) {
								rd50 += w[q] * cc[q]->bed.d50;
								silt += w[q] * cc[q]->bed.silt;
								wr += w[q];
								if (!bed || w[q] > 0.25f) {
									bed = &cc[q]->bed;
								}
							}
							if (cc[q]->d50 > 0.0f) {
								d50 += w[q] * cc[q]->d50;
								e += w[q] * cc[q]->e;
								ws += w[q];
							}
						}
						if (stony > 0.0f) {
							const Vector3 wv = wear->at(x + fx * sub, z + fz * sub);
							// The coarsest class stays where it lies; the gravel and the fines are kicked and washed off the tread.
							const float thin = cls > 0 ? 1.0f - 0.8f * float(wv.x) : 1.0f;
							shore *= thin;
							scree *= thin;
							river *= thin;
							soil *= thin;
							// Trampling and rooting break the pedestals down (the soil starts again from level).
							sdepth *= 1.0f - CLAMP(float(MAX(wv.x, wv.z)), 0.0f, 1.0f);
							worn = MIN(armour_cover[cls] * stony * float(wv.x), cap_shore);
							kick = MIN(kick_cover[cls] * stony * float(wv.y), cap_shore);
							root = MIN(root_cover[cls] * stony * float(wv.z), cap_shore);
						}
						float sorted = 0.0f;
						if (fgate > 0.0f) {
							// The fines' centres hold none of the other sources' stones either.
							const float share = rf_frost_share(gen, float(x + fx * sub), float(z + fz * sub), fslope);
							sorted = MIN(frost_cover[cls] * per_cover * fgate, cap_shore) * share;
							const float keep = 1.0f - fgate * (1.0f - share);
							shore *= keep;
							scree *= keep;
							soil *= keep;
							sdepth *= 1.0f - fgate; // frost churn levels them
						}
						float lag = 0.0f, spread = 0.0f;
						Vector2 flow;
						if (runoff_on) {
							float gully = 0.0f, fan = 0.0f;
							for (int q = 0; q < 4; q++) {
								gully += w[q] * cc[q]->gully;
								fan += w[q] * cc[q]->fan;
								flow += w[q] * cc[q]->flow;
							}
							flow = flow.length() > 1e-4f ? flow.normalized() : Vector2(0, 1);
							// Flowing water and fresh deposits take the place of the splash pedestals.
							soil *= (1.0f - gully) * (1.0f - fan);
							if (gully > 0.0f) {
								// The floor: its coarse lag; the gravel half washed out, the fines mostly.
								lag = MIN(lag_cover[cls] * per_cover * gully, cap_shore);
								static const float washed[3] = { 0.0f, 0.4f, 0.8f };
								shore *= 1.0f - washed[cls] * gully;
								scree *= 1.0f - washed[cls] * gully;
							}
							if (fan > 0.0f) {
								// The fan: fine grains spread over it, burying some of the coarser ground under it.
								spread = MIN(fan_cover[cls] * per_cover * fan, cap_shore);
								const float buried = cls == 0 ? 0.6f : 0.3f;
								shore *= 1.0f - buried * fan;
								scree *= 1.0f - buried * fan;
							}
						}
						const float base = shore + scree + river;
						const float total = base + worn + kick + root + sorted + lag + spread + soil;
						if (total <= 0.0f || rng.randf() * rate_max > total) {
							continue;
						}
						const float pick = rng.randf() * total;
						Pending pd;
						pd.xz = Vector3(x + fx * sub, 0, z + fz * sub);
						pd.yaw = rng.randf() * Math::TAU;
						pd.tilt_axis = Vector3(rng.randf() - 0.5f, 0, rng.randf() - 0.5f);
						if (pick >= base + worn + kick + root + sorted + lag + spread) {
							// A stone of the soil, angular or a little weathered round, of any size in the class; most stand on
							// their pedestals (a flat cap, the column under it), some have toppled off theirs and lie tilted on
							// the lowered ground beside it. Only where its own spot is bare (the bare patches are finer than
							// the square the rate was read for).
							float own_depth = 0.0f;
							if (_rain_soil(gen, *rain, pd.xz, own_depth) <= 0.0f) {
								continue;
							}
							sdepth = own_depth;
							pd.key = _library_key(type, rng.randf() < 0.3f ? RockGenerator::FAMILY_PEBBLE : RockGenerator::FAMILY_SCREE, RF_DETAIL_BIN + cls);
							pd.size = sd1 * Math::pow(sd2 / sd1, rng.randf());
							if (cls == 0 && rng.randf() < 0.04f) {
								// Now and then a cobble, 15 to 40 cm: the caps real tall pedestals stand under (a column is
								// capped at about a third of its cap's width, so the pebbles' columns stop at 3 cm however
								// much soil the rain took; a cobble's carries the 5 to 15 cm a fresh burn or a badly
								// eroding slope loses).
								pd.size = 0.15f * Math::pow(0.4f / 0.15f, rng.randf());
							}
							pd.film = 0.0f;
							if (sdepth > 0.002f && rng.randf() >= 0.25f) {
								pd.tilt = Math::deg_to_rad(8.0f) * rng.randf();
								pd.sink = 0.03f + 0.04f * rng.randf();
								pd.follow = 0.3f; // the cap lies as the column top, not the slope
								// Up to about a third of the cap's width (a taller, narrower column topples it).
								const float ph = MIN(sdepth, 0.35f * pd.size) * (0.6f + 0.4f * rng.randf());
								// (Off: the same stones lie on the soil without their columns.)
								if (rain_pedestals) {
									pd.pedestal = ph;
									pedestal_stones[0].increment();
								}
							} else {
								pd.tilt = Math::deg_to_rad(20.0f + 40.0f * rng.randf());
								pd.sink = 0.05f + 0.05f * rng.randf();
								pd.follow = 0.5f;
								// Its column stays beside it, bared and rounding off (negative: a column with no stone).
								const float ph = MIN(sdepth, 0.35f * pd.size) * (0.6f + 0.4f * rng.randf());
								if (rain_pedestals && sdepth > 0.002f) {
									pd.pedestal = -ph;
									pedestal_stones[1].increment();
								}
							}
						} else if (pick >= base + worn + kick + root + sorted) {
							if (pick < base + worn + kick + root + sorted + lag) {
								// The lag: the coarse end of the class, subangular to a little rounded by the short run, imbricated
								// across the flow (long axis across it, dipping upstream, as a stream's gravel: Rust 1972).
								pd.key = _library_key(type, rng.randf() < 0.35f ? RockGenerator::FAMILY_PEBBLE : RockGenerator::FAMILY_SCREE, RF_DETAIL_BIN + cls);
								pd.size = sd1 * Math::pow(sd2 / sd1, Math::sqrt(rng.randf()));
								pd.yaw = Math::atan2(-flow.x, -flow.y) + (rng.randf() - 0.5f) * 0.8f;
								pd.tilt_axis = Vector3(-flow.y, 0, flow.x);
								pd.tilt = Math::deg_to_rad(8.0f + 17.0f * rng.randf());
								pd.sink = 0.2f + 0.1f * rng.randf();
								pd.follow = 0.7f;
								pd.film = 0.0f;
								runoff_stones[0].increment();
							} else {
								// The fan's spread: the fine end, lying flat, half in the silt laid round it.
								pd.key = _library_key(type, rng.randf() < 0.5f ? RockGenerator::FAMILY_PEBBLE : RockGenerator::FAMILY_SCREE, RF_DETAIL_BIN + cls);
								pd.size = Math::pow(ka - rng.randf() * (ka - kb), -1.0f / 2.5f);
								pd.tilt = Math::deg_to_rad(10.0f) * rng.randf();
								pd.sink = 0.25f + 0.1f * rng.randf();
								pd.follow = 0.9f;
								pd.film = 0.0f;
								runoff_stones[1].increment();
							}
						} else if (pick >= base + worn + kick + root) {
							// Frost-sorted: blocky frost-shattered stones (the rockfall family's fresh joint blocks, not the
							// talus's chips), piled clast on clast into a border standing over the soil, some heaved up on
							// edge with the long axis tilted up (Washburn 1956; Hallet 2013).
							pd.key = _library_key(type, RockGenerator::FAMILY_ROCKFALL, RF_DETAIL_BIN + cls);
							pd.film = 0.0f;
							pd.size = sd1 * Math::pow(sd2 / sd1, Math::pow(rng.randf(), 0.35f));
							frost_stones.increment();
							if (rng.randf() < 0.25f) {
								pd.tilt = Math::deg_to_rad(35.0f + 30.0f * rng.randf());
								pd.sink = 0.15f + 0.1f * rng.randf();
								pd.follow = 0.5f;
							} else {
								pd.tilt = Math::deg_to_rad(25.0f) * rng.randf();
								pd.sink = 0.04f + 0.08f * rng.randf();
								pd.follow = 0.7f;
							}
						} else if (pick >= base) {
							const float q = pick - base;
							pd.key = _library_key(type, RockGenerator::FAMILY_SCREE, RF_DETAIL_BIN + cls);
							pd.film = 0.0f;
							if (q < worn) {
								// The tread's armour: angular, the coarse end of the class, standing proud of the worn soil.
								pd.size = sd1 * Math::pow(sd2 / sd1, Math::sqrt(rng.randf()));
								pd.tilt = Math::deg_to_rad(25.0f) * rng.randf();
								pd.sink = 0.05f + 0.1f * rng.randf();
								pd.follow = 0.75f;
								wear_stones[0].increment();
							} else if (q < worn + kick) {
								wear_stones[1].increment();
								// Kicked to the shoulders: the fine end (the class's power law), loose on the surface.
								pd.size = Math::pow(ka - rng.randf() * (ka - kb), -1.0f / 2.5f);
								pd.tilt = Math::deg_to_rad(15.0f) * rng.randf();
								pd.sink = 0.1f + 0.1f * rng.randf();
								pd.follow = 0.85f;
							} else {
								// Turned up by rooting: any size, lying on the loosened soil at any angle.
								pd.size = sd1 * Math::pow(sd2 / sd1, rng.randf());
								pd.tilt = Math::deg_to_rad(60.0f) * rng.randf();
								pd.sink = 0.08f * rng.randf();
								pd.follow = 0.3f;
								wear_stones[2].increment();
							}
						} else if (bed && wr > 0.0f && pick >= shore && pick < shore + river) {
							rd50 /= wr;
							silt /= wr;
							// The bed's log-normal kept to this class, as the shore's.
							float d = 0.0f;
							for (int tries = 0; tries < 12; tries++) {
								const float t = rd50 * Math::exp(rng.randfn(0.0f, 0.6f));
								if (t >= sd1 && t < sd2) {
									d = t;
									break;
								}
							}
							pd.size = d > 0.0f ? d : sd1 * Math::pow(sd2 / sd1, rng.randf());
							// Most of a river's stones come from its headwaters (as the river layer's).
							const RockGenerator::RockType rt = rng.randf() < 0.7f ? bed->type : type;
							pd.key = _library_key(rt, RockGenerator::FAMILY_PEBBLE, RF_DETAIL_BIN + (p_layer - LAYER_DETAIL_PEBBLE));
							// Imbrication (as the river layer's cobbles): the long axis across the flow, each grain dipping
							// upstream on the one before it like roof tiles, 10 to 30 degrees (Rust 1972; the a-b plane's
							// upstream dip of a flowing gravel bed).
							pd.yaw = Math::atan2(-bed->f.x, -bed->f.y) + (rng.randf() - 0.5f) * 0.7f;
							pd.tilt_axis = Vector3(bed->side.x, 0, bed->side.y);
							pd.tilt = Math::deg_to_rad(10.0f + 20.0f * rng.randf());
							// Set deeper in the pools, where the fines drape them.
							pd.sink = 0.25f + 0.1f * rng.randf() + 0.2f * silt;
							pd.follow = 0.6f;
							pd.film = 1.0f;
						} else if (ws > 0.0f && pick < shore) {
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
	// Neighbours of one shape are told apart: a variant that a stone of the same library within a few sizes already
	// has is drawn again (up to three times), so a packed bed shows no side-by-side twins (no visible repetition; the
	// greedy colouring tile games use so no two neighbours share a tile).
	struct Placed {
		Vector2 p;
		uint32_t key;
		int next; // the next stone in its grid bin, or -1
		uint16_t variant;
	};
	LocalVector<Placed> placed;
	// 1 m bins over the cell, each the head of a list through Placed::next (no allocation per bin).
	const real_t bin_m = 1.0;
	const int gw = MAX(1, int(Math::ceil(p_rect.size.x / bin_m))), gh = MAX(1, int(Math::ceil(p_rect.size.y / bin_m)));
	LocalVector<int> bin_head;
	auto bin_of = [&](const Vector2 &p_at) {
		const int bx = CLAMP(int(Math::floor((p_at.x - p_rect.position.x) / bin_m)), 0, gw - 1);
		const int bz = CLAMP(int(Math::floor((p_at.y - p_rect.position.y) / bin_m)), 0, gh - 1);
		return Vector2i(bx, bz);
	};
	auto shared_near = [&](const Vector2 &p_at, uint32_t p_key, uint16_t p_variant, real_t p_reach) {
		if (bin_head.is_empty()) {
			return false;
		}
		const int rc = int(Math::ceil(p_reach / bin_m));
		const Vector2i c = bin_of(p_at);
		for (int bz = MAX(c.y - rc, 0); bz <= MIN(c.y + rc, gh - 1); bz++) {
			for (int bx = MAX(c.x - rc, 0); bx <= MIN(c.x + rc, gw - 1); bx++) {
				for (int id = bin_head[bz * gw + bx]; id >= 0; id = placed[id].next) {
					const Placed &o = placed[id];
					if (o.key == p_key && o.variant == p_variant && o.p.distance_to(p_at) < p_reach) {
						return true;
					}
				}
			}
		}
		return false;
	};
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
		const Vector2 at2(pd.xz.x, pd.xz.z);
		const real_t reach = MAX(real_t(4.0 * pd.size), real_t(0.3));
		for (int t = 0; t < 3 && lib->meshes.size() > 1 && shared_near(at2, pd.key, st.variant, reach); t++) {
			st.variant = pick_variant(lib);
		}
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
		if (pd.pedestal > 0.0f && rise < 0.1f * r) {
			// On its soil column (resting on another stone it has none).
			st.position.y += pd.pedestal;
			st.pedestal = pd.pedestal;
		} else if (pd.pedestal < 0.0f && rise < 0.1f * r) {
			st.pedestal = pd.pedestal; // toppled: the empty column beside it
		}
		dep.add(Vector2(pd.xz.x, pd.xz.z), float(st.position.y), r);
		if (bin_head.is_empty()) {
			bin_head.resize(gw * gh);
			for (int &h : bin_head) {
				h = -1;
			}
		}
		const Vector2i bc = bin_of(at2);
		placed.push_back({ at2, pd.key, bin_head[bc.y * gw + bc.x], st.variant });
		bin_head[bc.y * gw + bc.x] = int(placed.size()) - 1;
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
	for (TilePlacement &tp : r_cell.tiles) {
		for (const RID &r : tp.instances) {
			RS::get_singleton()->free_rid(r);
		}
	}
	r_cell.tiles.clear();
	r_cell.tile_meshes.clear();
	if (r_cell.tile_set) {
		r_cell.tile_set = false;
		_tile_set_release(r_cell.tile_block);
	}
	r_cell.groups.clear();
	close_dirty = true;
	if (r_cell.body) {
		r_cell.body->queue_free();
		r_cell.body = nullptr;
	}
	stone_count -= r_cell.stones;
	r_cell.stones = 0;
}

// A tile placement under a contact goes back to single grains (one MultiMesh per shape, instanced per grain in the
// path tracer like the cell's own), so its stones can react one by one.
void RockField3D::_split_tile(Cell &r_cell, uint32_t p_index, bool p_force, real_t p_vis, real_t p_margin) {
	TilePlacement &tp = r_cell.tiles[p_index];
	if (tp.instances.is_empty() && !p_force) {
		return;
	}
	for (const RID &r : tp.instances) {
		RS::get_singleton()->free_rid(r);
	}
	tp.instances.clear();
	HashMap<int, LocalVector<int>> by_group;
	for (const Vector2i &m : tp.members) {
		by_group[m.x].push_back(m.y);
	}
	tp.members.clear();
	const real_t vis = p_vis > 0.0 ? p_vis : (r_cell.instances.is_empty() ? real_t(30.0) : r_cell.instances[0]->get_visibility_range_end());
	const real_t margin = p_margin > 0.0 ? p_margin : (r_cell.instances.is_empty() ? real_t(1.0) : r_cell.instances[0]->get_visibility_range_end_margin());
	for (const KeyValue<int, LocalVector<int>> &kv : by_group) {
		const Library *lib = libraries.getptr(r_cell.groups[kv.key].library);
		if (!lib || r_cell.groups[kv.key].variant >= int(lib->meshes.size())) {
			continue;
		}
		Group grp;
		grp.library = r_cell.groups[kv.key].library;
		grp.variant = r_cell.groups[kv.key].variant;
		const int n = int(kv.value.size());
		Vector<float> buf;
		buf.resize(n * 16);
		float *w = buf.ptrw();
		for (int k = 0; k < n; k++) {
			Group &src = r_cell.groups[kv.key];
			const int i = kv.value[k];
			src.broken[i] = true; // now drawn by the new group
			const Transform3D &xf = src.xforms[i];
			const Color &cd = src.custom[i];
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
			grp.xforms.push_back(xf);
			grp.custom.push_back(cd);
			grp.reach.push_back(src.reach[i]);
			grp.near.push_back(false);
			grp.broken.push_back(false);
			grp.shapes.push_back(nullptr);
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(lib->meshes[grp.variant]);
		mm->set_instance_count(n);
		RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true); // first: it keeps a CPU copy of the buffer
		RS::get_singleton()->multimesh_set_rt_offscreen_cull(mm->get_rid(), RF_DETAIL_RT_CULL_DEG);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), buf);
		grp.multimesh = mm;
		r_cell.groups.push_back(grp);
		r_cell.instances.push_back(_add_group_instance(mm, grp.library, vis, margin, true));
	}
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
	// Blocks of the wall itself take the landscape's rock colour (a cave mouth's, and the cliff's courses: in their own
	// stone colour the courses read as pale plates stuck on the face).
	// Hoodoos, goblins, stacks and arches are the bedrock left standing (cut from the slope or the cliff round them), so
	// they take it too: in their own stone colour they read as pale pieces set on the hill or in the sea.
	const bool p_group = p_family == RockGenerator::FAMILY_HOODOO || p_family == RockGenerator::FAMILY_GOBLIN;
	// Ledge slabs are the caprock bed of the bench itself, standing out over its lip: the wall's rock too.
	const bool p_cliff = p_family == RockGenerator::FAMILY_CAVE_MOUTH || p_family == RockGenerator::FAMILY_CLIFF_BLOCK || p_family == RockGenerator::FAMILY_LEDGE || p_family == RockGenerator::FAMILY_SEA_STACK || p_family == RockGenerator::FAMILY_SEA_ARCH || p_group;
	const float age = rock_surface_age(p_family);
	const int mkey = int(p_type) + (p_cliff ? 1000 : 0) + int(age * 10.0f + 0.5f) * 10000 + (p_detail ? 2000 : 0) + (p_group ? 4000 : 0);
	if (Ref<ShaderMaterial> *m = materials.getptr(mkey)) {
		return *m;
	}
	Ref<ShaderMaterial> m = RockGenerator::get_material(p_type)->duplicate();
	m->set_shader_parameter("cliff_match", p_cliff);
	m->set_shader_parameter("lichen_cover", lichen_cover);
	m->set_shader_parameter("surface_age", age);
	m->set_shader_parameter("instance_tint", p_group ? 0.45f : 0.0f);
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

void RockField3D::_on_exclusions_changed(const Rect2 &p_area) {
	exclusions.read(WorldExclusion3D::KIND_ROCKS);
	if (Engine::get_singleton()->is_editor_hint() && !editor_preview) {
		return;
	}
	_on_holes_changed(p_area); // the coarse cells there are placed again
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
		_free_cell(cells[k]);
		cells.erase(k);
		_build_cell(k);
	}
}

// Worn ground from WorldStand (docs/ideas/ground-detail.md, "Game trails and boar rooting").
Vector3 RockField3D::StandWear::at(real_t p_x, real_t p_z) const {
	if (!any) {
		return Vector3();
	}
	const real_t fx = CLAMP((p_x - rect.position.x) / step, real_t(0), real_t(nx)), fz = CLAMP((p_z - rect.position.y) / step, real_t(0), real_t(nz));
	const int i = MIN(int(fx), nx - 1), j = MIN(int(fz), nz - 1);
	const real_t u = fx - i, t = fz - j;
	const int w = nx + 1;
	return v[j * w + i].lerp(v[j * w + i + 1], u).lerp(v[(j + 1) * w + i].lerp(v[(j + 1) * w + i + 1], u), t);
}

Vector3 RockField3D::StandWear::max_in(const Rect2 &p_rect) const {
	if (!any) {
		return Vector3();
	}
	const int i0 = CLAMP(int(Math::floor((p_rect.position.x - rect.position.x) / step)), 0, nx), i1 = CLAMP(int(Math::ceil((p_rect.get_end().x - rect.position.x) / step)), 0, nx);
	const int j0 = CLAMP(int(Math::floor((p_rect.position.y - rect.position.y) / step)), 0, nz), j1 = CLAMP(int(Math::ceil((p_rect.get_end().y - rect.position.y) / step)), 0, nz);
	Vector3 m;
	for (int j = j0; j <= j1; j++) {
		for (int i = i0; i <= i1; i++) {
			m = m.max(v[j * (nx + 1) + i]);
		}
	}
	return m;
}

// Main thread. A trail's wear w = tread (1 - exp(-passes / 300)): the tread is worn bare within a few hundred passes a
// year and changes little after (Marion and Leung); its shoulders tread (1 - tread) 4 of that. A rooting is fresh at
// up to a season and settled by two years (the terrain material's fade).
void RockField3D::_sample_wear(const Rect2 &p_rect, StandWear &r_wear) const {
	r_wear = StandWear();
	r_wear.rect = p_rect;
	if (!ground_wear) {
		return;
	}
	const WorldStand *st = WorldContracts::stand();
	if (!st) {
		return;
	}
	r_wear.nx = MAX(1, int(Math::ceil(p_rect.size.x / r_wear.step)));
	r_wear.nz = MAX(1, int(Math::ceil(p_rect.size.y / r_wear.step)));
	r_wear.v.resize((r_wear.nx + 1) * (r_wear.nz + 1));
	for (int j = 0; j <= r_wear.nz; j++) {
		for (int i = 0; i <= r_wear.nx; i++) {
			const Vector3 p(p_rect.position.x + i * r_wear.step, 0, p_rect.position.y + j * r_wear.step);
			Vector3 c;
			const Vector2 t = st->trail_at(p);
			if (t.x > 0.0f && t.y > 0.0f) {
				const float worn = 1.0f - Math::exp(-float(t.x) / 300.0f);
				c.x = t.y * worn;
				c.y = 4.0f * t.y * (1.0f - t.y) * worn;
			}
			const float age = st->rooting_age_at(p);
			if (age >= 0.0f) {
				c.z = 1.0f - Math::smoothstep(0.25f, 2.0f, age); // the terrain material's fade (Landscape3D's stand map)
			}
			r_wear.v[j * (r_wear.nx + 1) + i] = c;
			r_wear.any = r_wear.any || c.x > 0.0f || c.y > 0.0f || c.z > 0.0f;
		}
	}
	if (!r_wear.any) {
		r_wear.v.clear();
	}
}

// Main thread: the climate for the detail job (WorldWeather's season climate), and whether its cell can sort at all.
void RockField3D::_frost_climate(const Rect2 &p_rect, FrostClimate &r_frost) const {
	r_frost = FrostClimate();
	const TerrainGenerator3D *gen = _get_generator();
	WeatherSeason::Climate cl;
	if (!frost_sorting || !gen || !WorldContracts::weather()->season_climate(cl)) {
		return;
	}
	r_frost.on = true;
	r_frost.maat = 0.5f * (cl.temperature_january + cl.temperature_july);
	r_frost.lapse = 0.5f * (cl.lapse_rate_january + cl.lapse_rate_july);
	r_frost.reference = cl.reference_height;
	// Sorting needs water in the active layer: wet, poorly drained ground (300 mm a year and up).
	r_frost.wet = Math::smoothstep(250.0f, 500.0f, cl.precipitation);
	if (r_frost.wet <= 0.0f) {
		return;
	}
	rf_frost_build();
	for (int k = 0; k < 9 && !r_frost.any; k++) {
		const Vector3 p(p_rect.position.x + (k % 3) * 0.5f * p_rect.size.x, 0, p_rect.position.y + (k / 3) * 0.5f * p_rect.size.y);
		Vector3 n;
		r_frost.any = _frost_gate(gen, r_frost, p, n) > 0.0f;
	}
}

// Main thread: the rain's climate over a detail cell (docs/ideas/ground-detail.md, "Rain splash pedestals"). Plant
// cover on soil follows the year's rain: in drylands a perennial cover of grass and shrubs of about a tenth at 200 mm
// to most of the ground by 700 mm (the rain-use curves of semi-arid rangelands; Le Houerou 1984), so the bare share
// falls from about 0.9 to 0 over 200 to 700 mm. A fire bares the soil for its first years (the regrowth closes it
// over about three: the burnt area erosion studies, Shakesby and Doerr 2006).
void RockField3D::_rain_climate(const Rect2 &p_rect, RainClimate &r_rain) const {
	r_rain = RainClimate();
	const TerrainGenerator3D *gen = _get_generator();
	WeatherSeason::Climate cl;
	if (!gen || !WorldContracts::weather()->season_climate(cl)) {
		return;
	}
	r_rain.on = true;
	r_rain.rect = p_rect;
	const Vector3 mid(p_rect.get_center().x, 0, p_rect.get_center().y);
	const float f = WorldContracts::climate_precipitation_factor_at(mid);
	r_rain.climate_precip = cl.precipitation;
	r_rain.precip = cl.precipitation * (f >= 0.0f ? f : 1.0f);
	r_rain.bare = WorldContracts::bare_share_for_precipitation(r_rain.precip); // (the shared rule: terrain colour reads it too)
	bool burnt = false;
	if (const WorldFire *fire = WorldContracts::fire()) {
		for (int k = 0; k < 9; k++) {
			const Vector3 p(p_rect.position.x + (k % 3) * 0.5f * p_rect.size.x, 0, p_rect.position.y + (k / 3) * 0.5f * p_rect.size.y);
			const float age = fire->burn_age_at(p);
			if (age >= 0.0f) {
				r_rain.burn[k] = CLAMP(fire->burnt_at(p), 0.0f, 1.0f) * (1.0f - Math::smoothstep(0.5f, 3.0f, age));
				burnt = burnt || r_rain.burn[k] > 0.0f;
			}
		}
	}
	r_rain.any = r_rain.bare > 0.0f || burnt;
}

float RockField3D::SnowPack::at(float p_x, float p_z) const {
	if (!any || rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
		return 0.0f;
	}
	const float u = CLAMP((p_x - rect.position.x) / rect.size.x * 4.0f, 0.0f, 4.0f);
	const float v = CLAMP((p_z - rect.position.y) / rect.size.y * 4.0f, 0.0f, 4.0f);
	const int i = MIN(int(u), 3), j = MIN(int(v), 3);
	const float fu = u - i, fv = v - j;
	const float a = Math::lerp(depth[j * 5 + i], depth[j * 5 + i + 1], fu);
	const float b = Math::lerp(depth[(j + 1) * 5 + i], depth[(j + 1) * 5 + i + 1], fu);
	return Math::lerp(a, b, fv);
}

// Main thread: the snowpack's depth over a detail cell (docs/ideas/ground-detail.md, "Stones through snow"), from the
// snow contract (SnowCover3D). A stone whose top is under it is left out: deep snow (SnowSurface3D's geometry) would
// hide it anyway, and thin snow (a material on the ground) would show it lying on top.
void RockField3D::_snow_pack(const Rect2 &p_rect, SnowPack &r_snow) const {
	r_snow = SnowPack();
	const WorldSnow *snow = WorldContracts::snow();
	if (!snow_burial || !snow || !snow->has_provider()) {
		return;
	}
	r_snow.rect = p_rect;
	float sum = 0.0f;
	for (int k = 0; k < 25; k++) {
		const Vector3 p(p_rect.position.x + (k % 5) * 0.25f * p_rect.size.x, 0, p_rect.position.y + (k / 5) * 0.25f * p_rect.size.y);
		r_snow.depth[k] = MAX(snow->depth_at(p), 0.0f);
		sum += r_snow.depth[k];
		r_snow.any = r_snow.any || r_snow.depth[k] > 0.002f;
	}
	r_snow.mean = sum / 25.0f;
}

void RockField3D::set_snow_burial(bool p_enabled) {
	if (snow_burial == p_enabled) {
		return;
	}
	snow_burial = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

// Main thread, every two seconds: the laid detail cells whose snowpack has changed by a centimetre or more (snowfall,
// melt, drift) are laid again (as for a changed relief).
void RockField3D::_update_snow_cells(double p_delta) {
	snow_check -= float(p_delta);
	if (snow_check > 0.0f || !snow_burial || !ground_detail) {
		return;
	}
	snow_check = 2.0f;
	const WorldSnow *snow = WorldContracts::snow();
	if (!snow || !snow->has_provider()) {
		return;
	}
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		if (!_is_detail(kv.key.x) || kv.value.snow < 0.0f) {
			continue;
		}
		const real_t ks = _cell_size(Layer(kv.key.x));
		const Rect2 r(kv.key.y * ks, kv.key.z * ks, ks, ks);
		float sum = 0.0f;
		for (int k = 0; k < 5; k++) {
			const Vector2 q = k == 4 ? r.get_center() : r.position + r.size * Vector2(0.25f + 0.5f * (k % 2), 0.25f + 0.5f * (k / 2));
			sum += MAX(snow->depth_at(Vector3(q.x, 0, q.y)), 0.0f);
		}
		// (Five points against the 25 it was laid with: a margin over the centimetre.)
		if (Math::abs(sum / 5.0f - kv.value.snow) > 0.015f) {
			_on_relief_changed(r);
		}
	}
}

float RockField3D::RainClimate::burn_at(float p_x, float p_z) const {
	if (rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
		return burn[4];
	}
	const float u = CLAMP((p_x - rect.position.x) / rect.size.x * 2.0f, 0.0f, 2.0f);
	const float v = CLAMP((p_z - rect.position.y) / rect.size.y * 2.0f, 0.0f, 2.0f);
	const int i = MIN(int(u), 1), j = MIN(int(v), 1);
	const float fu = u - i, fv = v - j;
	const float a = Math::lerp(burn[j * 3 + i], burn[j * 3 + i + 1], fu);
	const float b = Math::lerp(burn[(j + 1) * 3 + i], burn[(j + 1) * 3 + i + 1], fu);
	return Math::lerp(a, b, fv);
}

// Any thread: stones lie on bare soil (the soil's own coarse fraction, more where the bedrock is near), and each rain
// splashes the soil away round them while the stone shields its own spot: an erosion pedestal (Ellison 1944; field
// pedestals read soil loss directly: millimetres to centimetres over a wet season on bare soil, most on steeper, bare,
// silty ground). Not on water, talus, sand (it slumps), bare rock, thin soil or steep slopes.
float RockField3D::_rain_soil(const TerrainGenerator3D *p_gen, const RainClimate &p_rain, const Vector3 &p_at, float &r_depth) const {
	r_depth = 0.0f;
	if (!p_rain.on) {
		return 0.0f;
	}
	const float fire = p_rain.burn_at(float(p_at.x), float(p_at.z));
	// Where the dry climate's bare share falls: the shared patch pattern (WorldContracts::bare_patch) that the terrain
	// material's bare soil and every plant layer read, so stones stand on pedestals only where the ground shows bare
	// soil and no sward grows; the share's own value at the point, as theirs.
	const float h = float(rf_height(p_gen, p_at));
	const Vector3 at(p_at.x, h, p_at.z);
	// (The climate's precipitation was read on the main thread into p_rain: the weather provider is a node. The
	// orographic factor map is plain data, safe here.)
	const float f_here = WorldContracts::climate_precipitation_factor_at(at);
	const float share_here = WorldContracts::bare_share_for_precipitation(p_rain.climate_precip * (f_here >= 0.0f ? f_here : 1.0f));
	const float dry_bare = WorldContracts::bare_patch(at, share_here, 0.0f);
	const float bare = MAX(dry_bare, fire);
	if (bare <= 0.0f) {
		return 0.0f;
	}
	if (p_gen->get_water_depth(at) > 0.03 || p_gen->get_talus(at) > 0.25 || p_gen->get_sand_depth_at(at) > 0.05) {
		return 0.0f;
	}
	if (p_gen->has_sea_result() && h < float(p_gen->get_sea_level()) + 2.5f) {
		return 0.0f;
	}
	const float soil = Math::smoothstep(0.05f, 0.2f, float(p_gen->get_soil_depth(at)));
	const float rock = float(p_gen->get_rock_exposure(at));
	const Vector3 n = ground_normal(p_gen, p_at.x, p_at.z, 2.0);
	const float slope = Math::rad_to_deg(Math::acos(CLAMP(float(n.y), -1.0f, 1.0f)));
	const float gate = soil * (1.0f - Math::smoothstep(0.4f, 0.7f, rock)) * (1.0f - Math::smoothstep(28.0f, 35.0f, slope));
	if (gate <= 0.0f) {
		return 0.0f;
	}
	// The soil lost since the surface was last disturbed: a dryland's few millimetres a year over years, a burnt
	// slope's centimetres in its first wet seasons; more with the rain and on steeper ground (splash goes downhill).
	const float dry = 0.035f * dry_bare * CLAMP(p_rain.precip / 400.0f, 0.5f, 1.5f);
	const float burn = 0.05f * fire * CLAMP(p_rain.precip / 600.0f, 0.5f, 2.0f);
	r_depth = MAX(dry, burn) * (0.6f + 0.8f * Math::smoothstep(2.0f, 20.0f, slope)) * gate;
	// Stones on the surface: more where the bedrock is near.
	const float stony = 0.4f + 0.6f * Math::smoothstep(0.0f, 0.4f, rock);
	return bare * gate * stony;
}

// Any thread: how strongly the ground sorts at a point, 0..1. Active sorting where the mean annual air is below
// about -1 C (permafrost or deep seasonal frost; Washburn 1979, Ballantyne 2018), on soil (not bare rock, talus,
// water or the beach), flat to about 10 degrees (stripes on 3 to 10, none past 15).
float RockField3D::_frost_gate(const TerrainGenerator3D *p_gen, const FrostClimate &p_frost, const Vector3 &p_at, Vector3 &r_normal) const {
	r_normal = Vector3(0, 1, 0);
	if (!p_frost.on || p_frost.wet <= 0.0f) {
		return 0.0f;
	}
	const float h = float(rf_height(p_gen, p_at));
	const float maat = p_frost.maat - p_frost.lapse * (h - p_frost.reference) / 1000.0f;
	const float cold = Math::smoothstep(1.0f, -2.0f, maat);
	if (cold <= 0.0f) {
		return 0.0f;
	}
	const Vector3 at(p_at.x, h, p_at.z);
	// Frost-susceptible soil only: no water, talus, dune or beach sand (sand drains and does not heave), and a
	// soil cover deep enough to sort (an active layer of some tens of centimetres).
	if (p_gen->get_water_depth(at) > 0.03 || p_gen->get_talus(at) > 0.25 || p_gen->get_sand_depth_at(at) > 0.05) {
		return 0.0f;
	}
	if (p_gen->has_sea_result() && h < float(p_gen->get_sea_level()) + 2.5f) {
		return 0.0f;
	}
	const float deep = Math::smoothstep(0.1f, 0.4f, float(p_gen->get_soil_depth(at)));
	if (deep <= 0.0f) {
		return 0.0f;
	}
	r_normal = ground_normal(p_gen, p_at.x, p_at.z, 4.0); // the hillside's slope, not the hummocks'
	const float slope = Math::rad_to_deg(Math::acos(CLAMP(float(r_normal.y), -1.0f, 1.0f)));
	const float flat = 1.0f - Math::smoothstep(10.0f, 15.0f, slope);
	const float soil = 1.0f - Math::smoothstep(0.4f, 0.7f, float(p_gen->get_rock_exposure(at)));
	return cold * p_frost.wet * flat * soil * deep;
}

Vector2 RockField3D::get_frost_at(const Vector3 &p_position) const {
	const TerrainGenerator3D *gen = _get_generator();
	FrostClimate fc;
	_frost_climate(Rect2(p_position.x, p_position.z, 0, 0), fc);
	if (!gen || !fc.on) {
		return Vector2();
	}
	Vector3 n;
	const float g = _frost_gate(gen, fc, p_position, n);
	const float slope = Math::rad_to_deg(Math::acos(CLAMP(float(n.y), -1.0f, 1.0f)));
	return Vector2(g, g > 0.0f ? rf_frost_share(gen, float(p_position.x), float(p_position.z), slope) : 0.0f);
}

// Main thread: the relief over the rect, for the tiles (none is cut over relief: _rest seats each grain on it, a tile's
// plane cannot follow it), probed every 2 m first (most ground has none) and then every 0.25 m.
void RockField3D::_sample_relief(const Rect2 &p_rect, ReliefGrid &r_relief) const {
	r_relief = ReliefGrid();
	WorldGround *wg = WorldContracts::ground();
	if (!wg) {
		return;
	}
	auto probe = [&](real_t p_x, real_t p_z) {
		const Vector2 r = wg->relief_at(Vector3(p_x, 0, p_z));
		return Math::is_finite(r.x) ? float(r.x) : 0.0f;
	};
	bool any = false;
	for (real_t z = p_rect.position.y; z <= p_rect.get_end().y + 0.01 && !any; z += 2.0) {
		for (real_t x = p_rect.position.x; x <= p_rect.get_end().x + 0.01 && !any; x += 2.0) {
			any = Math::abs(probe(x, z)) > 0.002f;
		}
	}
	if (!any) {
		return;
	}
	r_relief.origin = p_rect.position;
	r_relief.nx = int(Math::ceil(p_rect.size.x / r_relief.step)) + 1;
	r_relief.nz = int(Math::ceil(p_rect.size.y / r_relief.step)) + 1;
	r_relief.h.resize(r_relief.nx * r_relief.nz);
	for (int j = 0; j < r_relief.nz; j++) {
		for (int i = 0; i < r_relief.nx; i++) {
			r_relief.h[j * r_relief.nx + i] = probe(p_rect.position.x + i * r_relief.step, p_rect.position.y + j * r_relief.step);
		}
	}
	r_relief.any = true;
}

float RockField3D::ReliefGrid::max_in(const Rect2 &p_rect) const {
	if (!any) {
		return 0.0f;
	}
	const int i0 = CLAMP(int(Math::floor((p_rect.position.x - origin.x) / step)), 0, nx - 1), i1 = CLAMP(int(Math::ceil((p_rect.get_end().x - origin.x) / step)), 0, nx - 1);
	const int j0 = CLAMP(int(Math::floor((p_rect.position.y - origin.y) / step)), 0, nz - 1), j1 = CLAMP(int(Math::ceil((p_rect.get_end().y - origin.y) / step)), 0, nz - 1);
	float m = 0.0f;
	for (int j = j0; j <= j1; j++) {
		for (int i = i0; i <= i1; i++) {
			m = MAX(m, Math::abs(h[j * nx + i]));
		}
	}
	return m;
}

void RockField3D::set_runoff_gravel(bool p_enabled) {
	if (runoff_gravel == p_enabled) {
		return;
	}
	runoff_gravel = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

// The unit soil column (1 high, radius 0.9 at the top, flaring to 1.3 where it meets the ground, its sides a little
// lumpy) and its material: fresh subsoil, paler than the surface's crusted, darker skin, with splash rills down it.
void RockField3D::_pedestal_resources() {
	if (pedestal_meshes[0].is_valid() && pedestal_material.is_valid()) {
		return;
	}
	// The column's profile in the field (Ellison 1944's photographs; the pedestals of soil loss surveys): flared at the
	// foot where the splashed grit gathers, narrowing up to a waist the splash undercut under the cap's rim, its sides
	// fluted by the drops' runnels and lumpy where clods and roots hold. A column whose stone fell off rounds into a
	// cone in the next storms (no cap shields it). Each shape takes its own lumps, flutes, waist and lean.
	const int SEG = 28;
	const int RINGS = 8;
	static const float ys_stand[RINGS] = { 0.0f, 0.1f, 0.22f, 0.38f, 0.55f, 0.72f, 0.88f, 1.0f };
	static const float rs_stand[RINGS] = { 1.6f, 1.3f, 1.08f, 0.94f, 0.84f, 0.76f, 0.7f, 0.64f };
	static const float ys_empty[RINGS] = { 0.0f, 0.15f, 0.32f, 0.5f, 0.66f, 0.8f, 0.91f, 0.98f };
	static const float rs_empty[RINGS] = { 1.6f, 1.25f, 1.0f, 0.84f, 0.68f, 0.5f, 0.32f, 0.14f };
	for (int k = 0; k < RF_PEDESTAL_SHAPES; k++) {
		const bool empty = k >= RF_PEDESTAL_STANDING;
		const float *ys = empty ? ys_empty : ys_stand;
		const float *rs = empty ? rs_empty : rs_stand;
		RandomPCG rng(0x9ed5u + uint64_t(k) * 7919u);
		const float ph[4] = { float(rng.randf() * Math::TAU), float(rng.randf() * Math::TAU), float(rng.randf() * Math::TAU), float(rng.randf() * Math::TAU) };
		const float lump_a = 0.06f + 0.06f * rng.randf();
		const float flute_a = 0.035f + 0.03f * rng.randf();
		const int flutes = 9 + int(rng.rand() % 6u);
		const float waist = 0.9f + 0.2f * rng.randf(); // the undercut: narrower or fuller under the cap
		const float lean = 0.1f * (rng.randf() - 0.5f);
		Ref<SurfaceTool> st;
		st.instantiate();
		st->begin(Mesh::PRIMITIVE_TRIANGLES);
		auto at = [&](int p_ring, int p_seg) {
			const float a = Math::TAU * float(p_seg % SEG) / SEG;
			const float y = ys[p_ring];
			const float lump = 1.0f + lump_a * (Math::sin(2.0f * a + ph[0] + 1.1f * p_ring) + 0.7f * Math::sin(3.0f * a + ph[1]) + 0.5f * Math::sin(5.0f * a + ph[2] + 0.8f * p_ring));
			// Flutes on the sides (not the foot's grit nor the cap's seat), wandering a little up the column.
			const float fl = flute_a * Math::smoothstep(0.08f, 0.25f, y) * (1.0f - Math::smoothstep(0.85f, 1.0f, y)) * Math::sin(float(flutes) * a + ph[3] + 0.9f * y);
			const float w = empty ? 1.0f : Math::lerp(1.0f, waist, Math::smoothstep(0.4f, 0.9f, y));
			const float r = rs[p_ring] * w * (lump + fl);
			return Vector3(Math::cos(a) * r + lean * y * y, y, Math::sin(a) * r);
		};
		for (int ring = 0; ring < RINGS - 1; ring++) {
			for (int g = 0; g < SEG; g++) {
				const Vector3 a = at(ring, g), b = at(ring, g + 1), c2 = at(ring + 1, g + 1), d = at(ring + 1, g);
				const float u0 = float(g) / SEG, u1 = float(g + 1) / SEG;
				// (Clockwise seen from outside: Godot's front faces, so the generated normals point out.)
				st->set_uv(Vector2(u0, ys[ring]));
				st->add_vertex(a);
				st->set_uv(Vector2(u1, ys[ring]));
				st->add_vertex(b);
				st->set_uv(Vector2(u1, ys[ring + 1]));
				st->add_vertex(c2);
				st->set_uv(Vector2(u0, ys[ring]));
				st->add_vertex(a);
				st->set_uv(Vector2(u1, ys[ring + 1]));
				st->add_vertex(c2);
				st->set_uv(Vector2(u0, ys[ring + 1]));
				st->add_vertex(d);
			}
		}
		if (empty) {
			// The rounded top (no cap seat to hide it).
			const Vector3 apex(lean, 1.0f, 0.0f);
			for (int g = 0; g < SEG; g++) {
				st->set_uv(Vector2(float(g) / SEG, ys[RINGS - 1]));
				st->add_vertex(at(RINGS - 1, g));
				st->set_uv(Vector2(float(g + 1) / SEG, ys[RINGS - 1]));
				st->add_vertex(at(RINGS - 1, g + 1));
				st->set_uv(Vector2(float(g) / SEG, 1.0f));
				st->add_vertex(apex);
			}
		}
		st->index();
		st->generate_normals();
		st->generate_tangents();
		pedestal_meshes[k] = st->commit();
	}
	Ref<Shader> sh;
	sh.instantiate();
	String code = R"(shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;
)" + BaseMaterial3D::get_camera_cutout_shader_code() +
			TerrainGenerator3D::get_geology_shader_code() + R"(
// Rain splash pedestals' soil columns (RockField3D.rain_pedestals; docs/ideas/ground-detail.md): the soil the stone
// shielded, its sides fresh subsoil (paler and warmer than the surface's humus-dark, crusted skin) cut by splash into
// fine vertical rills, grit standing out of them; the foot blends into the ground's own colour.
uniform vec3 ground_rock = vec3(0.107, 0.1, 0.089);
uniform vec3 ground_scree = vec3(0.141, 0.133, 0.114);
uniform vec3 ground_soil = vec3(0.033, 0.04, 0.01);
uniform vec3 ground_alluvium = vec3(0.064, 0.06, 0.022);
uniform vec3 ground_sand = vec3(0.448, 0.342, 0.181);
uniform float ground_alluvium_area = 5.5;
uniform float ground_blend = 1.0;
uniform float ground_blend_height = 0.3;
uniform bool mineral_glints = false;
varying vec3 v_world;
varying float v_h;

float pd_hash(vec3 p) {
	p = fract(p * 0.3183099 + 0.1);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float pd_noise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(pd_hash(i), pd_hash(i + vec3(1, 0, 0)), f.x), mix(pd_hash(i + vec3(0, 1, 0)), pd_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(pd_hash(i + vec3(0, 0, 1)), pd_hash(i + vec3(1, 0, 1)), f.x), mix(pd_hash(i + vec3(0, 1, 1)), pd_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_h = VERTEX.y;
}

void fragment() {
	if (!IN_SHADOW_PASS && camera_cutout.w > 0.0 && camera_cutout_discard((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz, INV_VIEW_MATRIX, PROJECTION_MATRIX, FRAGCOORD.xy)) {
		discard;
	}
	vec3 soil = ground_soil;
	if (terrain_geology_rect.z > 0.0) {
		vec4 g = terrain_geology(v_world);
		float alluv = smoothstep(ground_alluvium_area, ground_alluvium_area + 1.0, g.w);
		soil = mix(ground_soil, ground_alluvium, alluv);
		soil = mix(soil, ground_sand, clamp(-g.x, 0.0, 1.0));
	}
	// Fresh subsoil: less humus, so paler than the surface and iron brown (the B horizon's yellowish to reddish browns,
	// about 10YR 5/4), keeping the soil's own lightness.
	float lum = dot(soil, vec3(0.2126, 0.7152, 0.0722));
	vec3 sub = lum * vec3(2.9, 2.2, 1.5) + vec3(0.01, 0.007, 0.004);
	// Each column its own shade (patches of a few centimetres: horizons, clods, a paler crust drying on top).
	float patch = pd_noise(v_world * 16.0 + 3.0);
	sub *= 0.82 + 0.36 * patch;
	// Splash rills: fine vertical runnels (stretched noise), darker in their bottoms.
	float rill = pd_noise(vec3(v_world.x * 380.0, v_world.y * 120.0, v_world.z * 380.0));
	float fine = pd_noise(v_world * 900.0);
	vec3 col = sub * (0.88 + 0.2 * rill) * (0.9 + 0.2 * fine);
	// Grit and small chips standing out of the column (paler and darker grains), and a skirt of the splashed grit
	// gathered at the foot (coarser, more of it, a little darker and damp).
	float foot = 1.0 - smoothstep(0.08, 0.3, v_h);
	float grit = pd_noise(v_world * mix(1500.0, 700.0, foot) + 7.0);
	float hi = mix(0.8, 0.62, foot), lo = mix(0.15, 0.3, foot);
	col *= grit > hi ? 1.45 : (grit < lo ? 0.6 : 1.0);
	col *= 1.0 - 0.18 * foot;
	// The very foot: into the ground's own (crusted) colour.
	col = mix(soil, col, smoothstep(0.0, 0.08, v_h));
	ALBEDO = col;
	ROUGHNESS = 0.93 - 0.06 * rill;
	SPECULAR = 0.35;
	// (No screen derivatives: the path tracer's hit shaders have none. The grooves are under a millimetre, so their
	// shading stays in the albedo.)
}
)";
	sh->set_code(code);
	pedestal_material.instantiate();
	pedestal_material->set_shader(sh);
	_apply_ground(pedestal_material);
}

Vector2 RockField3D::get_pedestal_at(const Vector3 &p_position) const {
	const TerrainGenerator3D *gen = _get_generator();
	RainClimate rc;
	_rain_climate(Rect2(p_position.x - 1.0f, p_position.z - 1.0f, 2.0f, 2.0f), rc);
	if (!gen || !rc.any) {
		return Vector2();
	}
	float depth;
	const float share = _rain_soil(gen, rc, p_position, depth);
	return Vector2(share, rain_pedestals ? depth : 0.0f);
}

PackedVector4Array RockField3D::get_pedestal_spots(const Rect2 &p_rect, real_t p_step, int p_max_count) const {
	PackedVector4Array out;
	const TerrainGenerator3D *gen = _get_generator();
	RainClimate rc;
	_rain_climate(p_rect, rc);
	if (!gen || !rc.any || !rain_pedestals || p_step <= 0.0) {
		return out;
	}
	// Every step over the rect: the points on bare soil (the shared bare patches or a burn), deepest splash first.
	struct Spot {
		Vector4 v;
		bool operator<(const Spot &p_o) const { return v.w > p_o.v.w; }
	};
	LocalVector<Spot> spots;
	const int nx = MIN(1024, int(Math::ceil(p_rect.size.x / p_step))), nz = MIN(1024, int(Math::ceil(p_rect.size.y / p_step)));
	for (int j = 0; j < nz; j++) {
		for (int i = 0; i < nx; i++) {
			const Vector3 p(p_rect.position.x + (i + 0.5) * p_step, 0, p_rect.position.y + (j + 0.5) * p_step);
			float depth = 0.0f;
			const float share = _rain_soil(gen, rc, p, depth);
			if (share >= 0.5f && depth > 0.0f) {
				spots.push_back({ Vector4(p.x, rf_height(gen, p), p.z, depth) });
			}
		}
	}
	spots.sort();
	const int n = MIN(int(spots.size()), MAX(p_max_count, 0));
	out.resize(n);
	for (int k = 0; k < n; k++) {
		out.set(k, spots[k].v);
	}
	return out;
}

void RockField3D::set_rain_pedestals(bool p_enabled) {
	if (rain_pedestals == p_enabled) {
		return;
	}
	rain_pedestals = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

void RockField3D::set_frost_sorting(bool p_enabled) {
	if (frost_sorting == p_enabled) {
		return;
	}
	frost_sorting = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
	if (!frost_sorting) {
		_clear_frost_map();
	}
	if (is_inside_tree()) {
		_update_process();
	}
}

void RockField3D::_watch_stand() {
	WorldStand *st = WorldContracts::stand();
	if (!st || st->get_instance_id() == stand_watched) {
		return;
	}
	const Callable cb = callable_mp(this, &RockField3D::_on_stand_changed);
	if (Object *old = ObjectDB::get_instance(stand_watched)) {
		if (old->is_connected(SNAME("stand_changed"), cb)) {
			old->disconnect(SNAME("stand_changed"), cb);
		}
	}
	st->connect(SNAME("stand_changed"), cb);
	stand_watched = st->get_instance_id();
}

void RockField3D::_watch_ground() {
	WorldGround *wg = WorldContracts::ground();
	if (!wg || wg->get_instance_id() == ground_watched) {
		return;
	}
	const Callable cb = callable_mp(this, &RockField3D::_on_relief_changed);
	if (Object *old = ObjectDB::get_instance(ground_watched)) {
		if (old->is_connected(SNAME("relief_changed"), cb)) {
			old->disconnect(SNAME("relief_changed"), cb);
		}
	}
	wg->connect(SNAME("relief_changed"), cb);
	ground_watched = wg->get_instance_id();
}

// A trail laid or moved, a rooting added or aged: the detail cells over it are built again (streaming builds the
// freed ones again as it reaches them; a job already reading the old ground is dropped).
void RockField3D::_on_stand_changed(const Rect2 &p_rect) {
	if (building > 0 || !ground_wear) {
		return;
	}
	// Gathered and rebuilt once at the frame's end: a script adding many disturbances in one frame built the same
	// cells again for each, and every freed copy lived until the frame's end (the memory of the field many times).
	stand_dirty = stand_dirty.has_area() ? stand_dirty.merge(p_rect) : p_rect;
	if (!stand_queued) {
		stand_queued = true;
		callable_mp(this, &RockField3D::_apply_stand_changes).call_deferred();
	}
}

// The drawn relief over an area changed (WorldGround.relief_changed: Landscape3D's full heights streamed in, its relief
// settings): the detail cells there are laid again on it, gathered with the stand's changes.
void RockField3D::_on_relief_changed(const Rect2 &p_rect) {
	if (building > 0 || !ground_detail) {
		return;
	}
	relief_dirty = relief_dirty.has_area() ? relief_dirty.merge(p_rect) : p_rect;
	if (!stand_queued) {
		stand_queued = true;
		callable_mp(this, &RockField3D::_apply_stand_changes).call_deferred();
	}
}

void RockField3D::_apply_stand_changes() {
	stand_queued = false;
	Rect2 area;
	if (stand_dirty.has_area() && ground_wear) {
		area = stand_dirty;
	}
	if (relief_dirty.has_area() && ground_detail) {
		area = area.has_area() ? area.merge(relief_dirty) : relief_dirty;
	}
	stand_dirty = Rect2();
	relief_dirty = Rect2();
	if (building > 0 || !area.has_area() || !is_inside_tree()) {
		return;
	}
	area = area.grow(1.0);
	for (const KeyValue<Vector3i, DetailJob *> &kv : detail_jobs) {
		const real_t ks = _cell_size(Layer(kv.key.x));
		if (Rect2(kv.key.y * ks, kv.key.z * ks, ks, ks).intersects(area)) {
			_detail_cancel();
			break;
		}
	}
	LocalVector<Vector3i> again;
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		if (!_is_detail(kv.key.x)) {
			continue;
		}
		const real_t ks = _cell_size(Layer(kv.key.x));
		if (Rect2(kv.key.y * ks, kv.key.z * ks, ks, ks).intersects(area)) {
			again.push_back(kv.key);
		}
	}
	for (const Vector3i &k : again) {
		_free_cell(cells[k]);
		cells.erase(k);
		if (!streaming) {
			_build_cell(k);
		}
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
		slope.x = (rf_height(p_gen, o + Vector3(r, 0, 0)) - rf_height(p_gen, o - Vector3(r, 0, 0))) / (2.0 * r);
		slope.y = (rf_height(p_gen, o + Vector3(0, 0, r)) - rf_height(p_gen, o - Vector3(0, 0, r))) / (2.0 * r);
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
	_prepare_rivers(); // the river beds' grains read them on the worker
	const Layer layer = Layer(r_job.key.x);
	const real_t cs = _cell_size(layer);
	const Rect2 rect(r_job.key.y * cs, r_job.key.z * cs, cs, cs);
	// The shapes the placement can pick: either rock (strata), pebble or scree family, this size class.
	const int bin = RF_DETAIL_BIN + (layer - LAYER_DETAIL_PEBBLE);
	const RockGenerator::RockType types[2] = { rock_type, soft_rock_type };
	// Pebbles (shore, river beds), scree (talus, worn ground) and fresh joint blocks (frost-sorted borders).
	const RockGenerator::Family families[3] = { RockGenerator::FAMILY_PEBBLE, RockGenerator::FAMILY_SCREE, RockGenerator::FAMILY_ROCKFALL };
	_frost_climate(rect, r_job.frost);
	_rain_climate(rect, r_job.rain);
	_snow_pack(rect, r_job.snow);
	LocalVector<uint32_t> keys, missing;
	for (int t = 0; t < (use_strata ? 2 : 1); t++) {
		for (int fi = 0; fi < (r_job.frost.any ? 3 : 2); fi++) {
			const uint32_t k = _library_key(types[t], families[fi], bin);
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
	_sample_wear(rect.grow(1.0), r_job.wear);
	_sample_relief(rect.grow(1.0), r_job.relief);
	// The block's shared tiles: held while the job reads them, or cut by it (_detail_compute) when there are none yet.
	r_job.block = _tile_block(r_job.key);
	if (ground_detail_tiles) {
		TileSet **found = tile_sets.getptr(r_job.block);
		if (found) {
			(*found)->users++;
			r_job.set = *found;
		} else {
			r_job.new_set = memnew(TileSet);
			if (!tile_set_pending.has(r_job.block)) {
				tile_set_pending.insert(r_job.block);
				r_job.pending_owner = true;
			}
		}
	}
}

// Main thread: a job's newly cut set joins tile_sets (or, cut twice, gives way to the one there: the same tiles).
void RockField3D::_tile_set_adopt(DetailJob &r_job) {
	if (r_job.pending_owner) {
		tile_set_pending.erase(r_job.block);
		r_job.pending_owner = false;
	}
	if (!r_job.new_set) {
		return;
	}
	TileSet **found = tile_sets.getptr(r_job.block);
	if (found) {
		memdelete(r_job.new_set);
		(*found)->users++;
		r_job.set = *found;
	} else {
		r_job.new_set->users = 1;
		tile_sets.insert(r_job.block, r_job.new_set);
		r_job.set = r_job.new_set;
	}
	r_job.new_set = nullptr;
}

// Main thread: a job let go without a cell (its hold on the set released).
void RockField3D::_tile_set_drop(DetailJob &r_job) {
	if (r_job.pending_owner) {
		tile_set_pending.erase(r_job.block);
		r_job.pending_owner = false;
	}
	if (r_job.new_set) {
		memdelete(r_job.new_set);
		r_job.new_set = nullptr;
	}
	if (r_job.set) {
		r_job.set = nullptr;
		_tile_set_release(r_job.block);
	}
}

void RockField3D::_tile_set_release(const Vector3i &p_block) {
	TileSet **found = tile_sets.getptr(p_block);
	if (!found) {
		return;
	}
	if (--(*found)->users <= 0) {
		memdelete(*found);
		tile_sets.erase(p_block);
	}
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
	rf_job_wear = &r_job.wear;
	rf_job_frost = &r_job.frost;
	rf_job_rain = &r_job.rain;
	_place(layer, rect, rf_hash3(key.x, key.y, key.z, uint32_t(seed) * 2654435761u), stones, &r_job.libs, &r_job.seeds);
	rf_job_wear = nullptr; // (the shared tiles below are cut from unworn ground: they stand for the block's ground)
	rf_job_frost = nullptr;
	rf_job_rain = nullptr;
	rf_job_generator = nullptr;
	if (gen->has_holes_in(rect.grow(8.0))) {
		for (int i = int(stones.size()) - 1; i >= 0; i--) {
			if (gen->is_hole(stones[i].position)) {
				stones.remove_at(i);
			}
		}
	}
	if (r_job.snow.any) {
		// Under the snow: a stone whose top is under the pack's surface is left out (the finest first: what shows
		// through a thin pack is the coarse lag). Its top over the ground from its shape's bottom and half height,
		// tested stone by stone against the pack's depth at that stone. The pack read here is smooth over metres, but
		// a shallow pack is not even at the scale of the stones: its depth varies about the mean by a coefficient of
		// variation of 0.3 or more (Clark et al. 2011; Lopez-Moreno et al. 2011, shallower packs varying the most),
		// and less of it stays on a stone's convex, sun-warmed top than in the gaps. Each stone's own depth is the mean
		// scaled by 0.4 to 1.3 (a hash of its position, the mean 0.85, a CV of about 0.3), so through thin snow the
		// stones a little proud of the mean still show here and there instead of all going at one depth.
		uint64_t buried = 0;
		for (int i = int(stones.size()) - 1; i >= 0; i--) {
			const Stone &st = stones[i];
			const float mean = r_job.snow.at(float(st.position.x), float(st.position.z));
			const uint32_t hs = hash_murmur3_one_32(uint32_t(int32_t(Math::floor(st.position.z * 512.0))), hash_murmur3_one_32(uint32_t(int32_t(Math::floor(st.position.x * 512.0))), 0x5e0a7u));
			const float d = mean * (0.4f + 0.9f * float(hash_fmix32(hs) & 0xffffu) / 65535.0f);
			if (d <= 0.002f) {
				continue;
			}
			Library *const *lp = r_job.libs.getptr(st.library);
			if (!lp || !*lp || st.variant >= (*lp)->axes.size()) {
				continue;
			}
			const float top = float(st.position.y) + ((*lp)->bottom[st.variant] + 2.0f * (*lp)->axes[st.variant].y) * st.scale;
			if (top - st.ground < d) {
				stones.remove_at(i);
				buried++;
			}
		}
		snow_buried.add(buried);
	}
	// Tiles of piles: some squares of the cell drawn as placements of tiles cut from its own grains (their grains
	// replaced by the tile's, so what is drawn is what the finer layers and the contacts see).
	LocalVector<int> place;
	if (ground_detail_tiles) {
		if (r_job.new_set) {
			// The block's shared tiles: squares spread over the block (by their hash), each placed on its own without
			// the coarse stones round it (they shape only the grains next to them, which no tile square takes), so the
			// set samples all the block's ground and is the same whichever cell streams in first.
			const Vector3i b = r_job.block;
			const real_t ts = cs / 4;
			const LocalVector<DetailSeed> none;
			// Stratified: one square from each of TILE_SAMPLES equal parts of the block, the first of a few hashed tries
			// there that has grains to tile (the detail lies in bands: shores, talus, river beds).
			const int side = int(Math::round(Math::sqrt(real_t(TILE_SAMPLES))));
			const int span = TILE_BLOCK * 4 / side; // squares per part, across
			for (int c = 0; c < side * side; c++) {
				for (int a = 0; a < TILE_TRIES; a++) {
					const uint32_t h = rf_hash3(b.y * side + c % side, b.z * side + c / side, key.x, (uint32_t(seed) ^ 0x51edu) + uint32_t(a) * 0x9e3779b9u);
					const int sx = b.y * TILE_BLOCK * 4 + (c % side) * span + int(h % uint32_t(span));
					const int sz = b.z * TILE_BLOCK * 4 + (c / side) * span + int((h / uint32_t(span)) % uint32_t(span));
					const Rect2 srect(sx * ts, sz * ts, ts, ts);
					if (gen->has_holes_in(srect)) {
						continue;
					}
					LocalVector<Stone> grains;
					rf_job_generator = r_job.gen;
					_place(layer, srect, rf_hash3(sx, sz, key.x, uint32_t(seed) * 2654435761u ^ 0x5a5au), grains, &r_job.libs, &none);
					rf_job_generator = nullptr;
					if (grains.size() < 8) {
						continue;
					}
					TileSquare cand[1];
					_tile_squares(gen, key.x, srect, 1, grains, none, &cand[0]);
					bool any = false;
					for (int ci = 0; ci < 1; ci++) {
						const TileSquare &s = cand[ci];
						if (!s.ok) {
							continue;
						}
						any = true;
						TileCut cut;
						cut.level = s.level;
						cut.sig = s.sig;
						cut.lsize = s.lsize;
						cut.lsd = s.lsd;
						cut.film = s.film;
						cut.mix = s.mix;
						cut.count = int(s.idx.size());
						// Thinned to a cover: kept only while fewer than TILE_CHOICES + 1 kept ones of its size match it, so a
						// block has a few tiles for each kind of ground (fewer BLAS) and every square still picks from several.
						int like = 0;
						for (const TileCut &k : r_job.new_set->cuts) {
							like += (k.level == cut.level && rf_tile_like(k.sig, k.count, k.lsize, k.lsd, k.film, k.mix, cut.sig, cut.count, cut.lsize, cut.lsd, cut.film, cut.mix)) ? 1 : 0;
						}
						if (like >= TILE_CHOICES + 1) {
							continue;
						}
						const Transform3D inv = s.frame.affine_inverse();
						for (uint32_t i : s.idx) {
							Stone st = grains[i];
							const Transform3D xf = inv * Transform3D(st.basis, st.position);
							st.position = xf.origin;
							st.basis = xf.basis;
							cut.stones.push_back(st);
						}
						_tile_cut(gen, grains, s, r_job.libs, rf_hash3(sx, sz, key.x, 4441u + ci), cut.tile);
						r_job.new_set->cuts.push_back(std::move(cut));
					}
					if (!any) {
						continue;
					}
					break;
				}
			}
		}
		_detail_tiles(r_job, stones, place);
	}
	r_job.stones = int(stones.size());
	// The soil columns under the pedestalled stones: one of the unit columns (_pedestal_resources) scaled to the stone's
	// underside and the soil lost round it, turned, squashed and leaning a little each (no two alike), from just under
	// the lowered ground into the stone's base. A toppled stone's column stands beside it, empty and rounding off.
	r_job.standing.clear();
	for (const Stone &st : stones) {
		Library *const *lp = r_job.libs.getptr(st.library);
		_standing_append(st, lp ? *lp : nullptr, r_job.standing);
	}
	for (int k = 0; k < RF_PEDESTAL_SHAPES; k++) {
		r_job.pedestals[k].clear();
	}
	for (uint32_t i = 0; i < stones.size(); i++) {
		const Stone &st = stones[i];
		if (st.pedestal == 0.0f) {
			continue;
		}
		Library *const *lp = r_job.libs.getptr(st.library);
		if (!lp || !*lp || st.variant >= (*lp)->axes.size()) {
			continue;
		}
		const Vector3 ax = (*lp)->axes[st.variant];
		const uint32_t hsh = rf_hash3(int(Math::floor(st.position.x * 997.0)), int(Math::floor(st.position.z * 991.0)), 0x9edu, uint32_t(seed));
		const float u0 = float(hsh & 0xffffu) / 65535.0f, u1 = float(hsh >> 16) / 65535.0f;
		const uint32_t h2 = rf_hash3(int(hsh & 0x7fffffffu), 17, 0x51du, uint32_t(seed));
		const float u2 = float(h2 & 0xffffu) / 65535.0f, u3 = float(h2 >> 16) / 65535.0f;
		// Narrower than the cap at its top (the splash undercuts it), flaring to the foot: the unit column's waist is
		// about 0.75 of this, its foot 1.3.
		const float hw = st.scale * Math::sqrt(MAX(ax.x * ax.z, 1e-4f));
		const bool empty = st.pedestal < 0.0f;
		const float r = (empty ? 0.75f : 0.85f) * hw;
		const float rx = r * (0.8f + 0.4f * u0), rz = r * (1.2f - 0.4f * u0);
		Vector3 at = st.position;
		// (The ground keeps its height: the column is the soil it lost round it. Its flared foot goes in deep enough that
		// the downhill side never stands off a slope of 20 degrees or so.)
		float base = st.ground - 0.01f - 0.5f * r;
		float top;
		if (empty) {
			// Beside the stone that fell off it (it rolled a little over half its size away), lower as the rain rounds it.
			const float a = u3 * Math::TAU;
			const float d = 0.55f * hw + r;
			at += Vector3(Math::cos(a) * d, 0, Math::sin(a) * d);
			top = st.ground - st.pedestal * (0.85f + 0.15f * u2);
		} else {
			top = float(st.position.y) - 0.3f * st.scale * ax.y; // into the stone's lower part
		}
		const float hgt = MAX(top - base, 0.005f);
		// A little lean (columns stand off the vertical where the splash came more from one side).
		const float lean = 0.12f * (u2 - 0.5f);
		Basis b = Basis(Vector3(0, 1, 0), u1 * Math::TAU).scaled_local(Vector3(rx, hgt, rz));
		b = Basis(Vector3(1, 0, 0), lean) * b;
		const int shape = empty ? RF_PEDESTAL_STANDING + int(u3 * 7.0f) % (RF_PEDESTAL_SHAPES - RF_PEDESTAL_STANDING) : int(u2 * 13.0f) % RF_PEDESTAL_STANDING;
		LocalVector<float> &out = r_job.pedestals[shape];
		const uint32_t o = out.size();
		out.resize(o + 12);
		float *rec = out.ptr() + o;
		for (int row = 0; row < 3; row++) {
			rec[row * 4 + 0] = b.rows[row][0];
			rec[row * 4 + 1] = b.rows[row][1];
			rec[row * 4 + 2] = b.rows[row][2];
		}
		rec[3] = at.x;
		rec[7] = base;
		rec[11] = at.z;
	}
	// As _build_cell: one MultiMesh per shape (and its tiled grains apart), the same randoms in the same order.
	HashMap<uint64_t, LocalVector<uint32_t>> groups;
	for (uint32_t i = 0; i < stones.size(); i++) {
		const bool tiled = i < place.size() && place[i] >= 0;
		groups[(uint64_t(tiled) << 48) | (uint64_t(stones[i].library) << 16) | stones[i].variant].push_back(i);
	}
	RandomPCG rng(rf_hash3(key.y, key.z, key.x, 991u));
	for (const KeyValue<uint64_t, LocalVector<uint32_t>> &g : groups) {
		const uint32_t lk = uint32_t((g.key >> 16) & 0xFFFFFFFFu);
		const int v = int(g.key & 0xFFFF);
		const bool tiled = (g.key >> 48) != 0;
		Library *const *found = r_job.libs.getptr(lk);
		const Library *lib = found ? *found : nullptr;
		if (!lib || v >= int(lib->meshes.size())) {
			continue;
		}
		const Vector3 ax = lib->axes[v];
		DetailGroup dg;
		dg.library = lk;
		dg.variant = v;
		dg.tiled = tiled;
		const uint32_t n = g.value.size();
		if (!tiled) {
			dg.buffer.resize(n * 16);
		}
		float *w = tiled ? nullptr : dg.buffer.ptrw();
		dg.xforms.resize(n);
		dg.custom.resize(n);
		dg.reach.resize(n);
		if (tiled) {
			dg.place.resize(n);
		}
		for (uint32_t k = 0; k < n; k++) {
			const Stone &st = stones[g.value[k]];
			const Transform3D xf(st.basis, st.position);
			const Color cd = _stone_custom(gen, st, ax, rng);
			if (w) {
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
			}
			dg.xforms[k] = xf;
			dg.custom[k] = cd;
			dg.reach[k] = st.scale * MAX(ax.x, MAX(ax.y, ax.z));
			if (tiled) {
				dg.place[k] = place[g.value[k]];
			}
		}
		r_job.groups.push_back(dg);
	}
}

// Any thread: the 4 x 4 squares of a detail cell as tile squares: grains on the ground, its plane, size, wetness and
// rocks, and whether it can be drawn as a tile (enough grains, no hole, clear of the coarse stones, lies on a plane).
void RockField3D::_tile_squares(const TerrainGenerator3D *p_gen, int p_layer, const Rect2 &p_rect, int p_n, const LocalVector<Stone> &p_stones, const LocalVector<DetailSeed> &p_seeds, TileSquare *r_sq) const {
	const int N = p_n;
	const Rect2 rect = p_rect;
	const real_t ts = rect.size.x / N;
	const int level = MAX(0, int(Math::round(Math::log(_cell_size(Layer(p_layer)) / 4 / ts) / Math::log(2.0)))); // 0: a quarter of the cell, 1: a half square
	const int sx0 = int(Math::round(rect.position.x / ts)), sz0 = int(Math::round(rect.position.y / ts));
	for (uint32_t i = 0; i < p_stones.size(); i++) {
		const int ix = CLAMP(int(Math::floor((p_stones[i].position.x - rect.position.x) / ts)), 0, N - 1);
		const int iz = CLAMP(int(Math::floor((p_stones[i].position.z - rect.position.y) / ts)), 0, N - 1);
		r_sq[iz * N + ix].idx.push_back(i);
	}
	for (int q = 0; q < N * N; q++) {
		TileSquare &s = r_sq[q];
		const Rect2 r(rect.position + Vector2(q % N, q / N) * ts, Vector2(ts, ts));
		s.rect = r;
		s.level = level;
		s.order = rf_hash3(sx0 + q % N, sz0 + q / N, p_layer + 16 * level, uint32_t(seed) ^ 0x7111u);
		tile_skips[4].increment();
		if (s.idx.size() < 8 || p_gen->has_holes_in(r)) {
			s.fail = 1;
			tile_skips[0].increment();
			continue;
		}
		// The ground's plane over the square (least squares on a 5 x 5 grid, symmetric: the slopes are separable).
		const Vector2 c = r.get_center();
		real_t h[25];
		real_t mean = 0.0, sx = 0.0, sz = 0.0, s2 = 0.0;
		for (int j = 0; j < 5; j++) {
			for (int i = 0; i < 5; i++) {
				const real_t u = (i - 2) * ts * 0.25, v = (j - 2) * ts * 0.25;
				h[j * 5 + i] = rf_height(p_gen, Vector3(c.x + u, 0, c.y + v));
				mean += h[j * 5 + i];
				sx += h[j * 5 + i] * u;
				sz += h[j * 5 + i] * v;
				s2 += u * u;
			}
		}
		mean /= 25.0;
		const real_t gx = sx / s2, gz = sz / s2;
		float ls = 0.0f, film = 0.0f;
		HashMap<uint32_t, int> libs;
		for (uint32_t i : s.idx) {
			ls += Math::log(MAX(p_stones[i].scale, 1e-4f));
			film += p_stones[i].film;
			libs[p_stones[i].library]++;
		}
		s.lsize = ls / float(s.idx.size());
		s.film = film / float(s.idx.size());
		float var = 0.0f;
		for (uint32_t i : s.idx) {
			const float d = Math::log(MAX(p_stones[i].scale, 1e-4f)) - s.lsize;
			var += d * d;
		}
		s.lsd = Math::sqrt(var / float(s.idx.size()));
		// Clear of the coarse stones (the shore's, the talus's: over four times its grains) the grains pile against;
		// the coarser grains of the finer classes' own beds stay (a grain resting on one stays a single grain below).
		const float big = 4.0f * 2.0f * Math::exp(s.lsize);
		bool clear = true;
		for (const DetailSeed &sd : p_seeds) {
			if (sd.r < big) {
				continue;
			}
			const Vector2 nearest(CLAMP(sd.xz.x, r.position.x, r.get_end().x), CLAMP(sd.xz.y, r.position.y, r.get_end().y));
			if (nearest.distance_to(sd.xz) < sd.r + 0.02f) {
				clear = false;
				break;
			}
		}
		if (!clear) {
			s.fail = 2;
			tile_skips[1].increment();
			continue;
		}
		// Flat enough: the ground off the plane by under a sixth of a grain (half its size is the scale): its grains sit
		// as they settled within what their own sink into the ground (0.15 to 0.35 of the scale) already varies.
		const real_t tol = MAX(real_t(0.0015), real_t(0.35) * Math::exp(real_t(s.lsize)));
		bool flat = true;
		for (int j = 0; j < 5 && flat; j++) {
			for (int i = 0; i < 5; i++) {
				const real_t u = (i - 2) * ts * 0.25, v = (j - 2) * ts * 0.25;
				if (Math::abs(h[j * 5 + i] - (mean + gx * u + gz * v)) > tol) {
					flat = false;
					break;
				}
			}
		}
		if (!flat) {
			s.fail = 3;
			tile_skips[2].increment();
			continue;
		}
		// The rocks in it, those with a tenth or more of its grains.
		LocalVector<uint32_t> keys;
		for (const KeyValue<uint32_t, int> &kv : libs) {
			if (kv.value * 10 >= int(s.idx.size())) {
				keys.push_back(kv.key);
			}
		}
		keys.sort();
		s.sig = 7u;
		for (uint32_t k : keys) {
			s.sig = hash_murmur3_one_32(k, s.sig);
		}
		const Vector3 n = Vector3(-gx, 1, -gz).normalized();
		const Vector3 o(c.x, mean, c.y);
		Vector3 bx = Vector3(1, 0, 0) - n * n.x;
		bx.normalize();
		const Vector3 bz = bx.cross(n);
		s.frame = Transform3D(Basis(bx, n, bz), o);
		s.plane = Plane(n, o);
		// Grains lifted onto a coarser grain (their origin over 1.2 of their scale above the plane) stay single.
		LocalVector<uint32_t> on_ground;
		for (uint32_t i : s.idx) {
			if (s.plane.distance_to(p_stones[i].position) < 1.2f * p_stones[i].scale) {
				on_ground.push_back(i);
			}
		}
		s.idx = on_ground;
		s.ok = s.idx.size() >= 8;
		if (s.ok && !keys.is_empty()) {
			int first = 0;
			for (uint32_t i : s.idx) {
				first += p_stones[i].library == keys[0] ? 1 : 0;
			}
			s.mix = float(first) / float(s.idx.size());
		}
	}
}

// Any thread: a square's grains as a tile (one record buffer per shape, in tile space).
void RockField3D::_tile_cut(const TerrainGenerator3D *p_gen, const LocalVector<Stone> &p_stones, const TileSquare &p_sq, const HashMap<uint32_t, Library *> &p_libs, uint32_t p_seed, DetailTile &r_tile) const {
	const Transform3D inv = p_sq.frame.affine_inverse();
	HashMap<uint64_t, LocalVector<uint32_t>> shapes;
	for (uint32_t i : p_sq.idx) {
		shapes[(uint64_t(p_stones[i].library) << 16) | p_stones[i].variant].push_back(i);
	}
	RandomPCG rng(p_seed);
	for (const KeyValue<uint64_t, LocalVector<uint32_t>> &sh : shapes) {
		Library *const *found = p_libs.getptr(uint32_t(sh.key >> 16));
		const int v = int(sh.key & 0xFFFF);
		if (!found || v >= int((*found)->axes.size())) {
			continue;
		}
		DetailTileShape ts_shape;
		ts_shape.library = uint32_t(sh.key >> 16);
		ts_shape.variant = v;
		ts_shape.count = int(sh.value.size());
		ts_shape.buffer.resize(ts_shape.count * 16);
		float *w = ts_shape.buffer.ptrw();
		for (int k = 0; k < ts_shape.count; k++) {
			const Stone &st = p_stones[sh.value[k]];
			const Transform3D xf = inv * Transform3D(st.basis, st.position);
			// Its own record: the ground and slope are the tile placement's plane (the shader's rock_tile_plane).
			const Color cd = _stone_custom(p_gen, st, (*found)->axes[v], rng);
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
		}
		r_tile.shapes.push_back(ts_shape);
	}
}

// Any thread: tiles of piles (Peytavie et al. 2009; docs/ideas/ground-detail.md). The cell is cut into 4 x 4 squares
// (_tile_squares); a square that can be a tile takes a matching tile of its block's shared set (rf_tile_like; chosen by
// its own hash from its first TILE_CHOICES matches, never a neighbour's, at most six times in the cell), or else one of up to four tiles cut from the cell's own squares (at most six uses; a tile used once is dropped). Each is
// turned by a random quarter turn and its own grains are replaced by the tile's. Squares that match nothing keep their
// grains. p_place: the placement of each stone, or -1.
void RockField3D::_detail_tiles(DetailJob &r_job, LocalVector<Stone> &r_stones, LocalVector<int> &r_place) const {
	const TerrainGenerator3D *gen = r_job.gen;
	const Vector3i key = r_job.key;
	const real_t cs = _cell_size(Layer(key.x));
	const int N = 4;
	const Rect2 rect(key.y * cs, key.z * cs, cs, cs);
	// The squares that can be tiles. (Splitting bent or crowded squares in half was tried: the halves rarely repeat,
	// so their tiles cost more BLAS than the grains they replace; docs/ideas/ground-detail.md, "Tiles that follow
	// bumpy ground".) sq[i].idx index r_stones.
	LocalVector<TileSquare> sq;
	{
		TileSquare top[N * N];
		_tile_squares(gen, key.x, rect, N, r_stones, r_job.seeds, top);
		for (int q = 0; q < N * N; q++) {
			const Vector3 worn = r_job.wear.max_in(top[q].rect);
			bool channel = false;
			if (top[q].ok && runoff_gravel) {
				// No tile over a rill, gully or fan: its grains lie by the flow.
				for (int c = 0; c < 5 && !channel; c++) {
					const Vector2 p = c == 4 ? top[q].rect.get_center() : top[q].rect.position + top[q].rect.size * Vector2(c & 1, c >> 1);
					const Vector3 n = ground_normal(gen, p.x, p.y, 1.0);
					const Vector2 gl = gen->get_gully_at(Vector3(p.x, 0, p.y), n.y);
					channel = gl.x > 0.02f || gl.y > 0.15f;
				}
			}
			bool pedestalled = false;
			if (top[q].ok && r_job.rain.any) {
				// Nor over stones on their pedestals: a tile's grains lie on the plane.
				for (uint32_t si : top[q].idx) {
					if (r_stones[si].pedestal != 0.0f) {
						pedestalled = true;
						break;
					}
				}
			}
			if (top[q].ok && (channel || pedestalled || MAX(worn.x, MAX(worn.y, worn.z)) > 0.02f || r_job.frost.any || r_job.snow.any || (r_job.relief.any && r_job.relief.max_in(top[q].rect) > 0.01f))) {
				top[q].ok = false; // worn ground (a trail, a rooting) keeps its own grains: no tile is cut from it
				top[q].fail = 4;
			}
			if (top[q].ok) {
				sq.push_back(top[q]);
			}
		}
	}
	if (sq.is_empty()) {
		return;
	}
	const int units = int(sq.size());
	LocalVector<int> by_order;
	for (int q = 0; q < units; q++) {
		by_order.push_back(q);
	}
	for (uint32_t a = 1; a < by_order.size(); a++) { // at most 16: insertion
		for (uint32_t b = a; b > 0 && sq[by_order[b]].order < sq[by_order[b - 1]].order; b--) {
			SWAP(by_order[b], by_order[b - 1]);
		}
	}
	// The squares each touches (its neighbours, of either size).
	LocalVector<LocalVector<int>> touching;
	touching.resize(units);
	for (int a = 0; a < units; a++) {
		const Rect2 ga = sq[a].rect.grow(sq[a].rect.size.x * 0.01f);
		for (int b = 0; b < units; b++) {
			if (a != b && ga.intersects(sq[b].rect)) {
				touching[a].push_back(b);
			}
		}
	}
	auto matches = [](const TileSquare &a, int p_level, uint32_t p_sig, int p_count, float p_lsize, float p_lsd, float p_film, float p_mix) {
		return a.level == p_level && rf_tile_like(a.sig, int(a.idx.size()), a.lsize, a.lsd, a.film, a.mix, p_sig, p_count, p_lsize, p_lsd, p_film, p_mix);
	};
	const TileSet *set = r_job.set ? r_job.set : r_job.new_set;
	LocalVector<int> shared;
	shared.resize(units);
	LocalVector<int> use; // the own cut each square takes
	use.resize(units);
	for (int q = 0; q < units; q++) {
		shared[q] = -1;
		use[q] = -1;
	}
	LocalVector<int> shared_uses;
	if (set) {
		shared_uses.resize(set->cuts.size());
		for (uint32_t t = 0; t < set->cuts.size(); t++) {
			shared_uses[t] = 0;
		}
	}
	struct Cut {
		int source;
		int uses = 0;
	};
	LocalVector<Cut> cuts;
	for (int q : by_order) {
		const TileSquare &s = sq[q];
		if (set) {
			// The first few matching candidates of the set (TILE_CHOICES: its variety; fewer tiles, less BLAS), past
			// them only when the neighbours have taken those.
			LocalVector<int> cand;
			int matching = 0;
			for (uint32_t t = 0; t < set->cuts.size(); t++) {
				const TileCut &c = set->cuts[t];
				if (shared_uses[t] >= 6 || !matches(s, c.level, c.sig, c.count, c.lsize, c.lsd, c.film, c.mix)) {
					continue;
				}
				matching++;
				bool taken = false;
				for (int nb : touching[q]) {
					taken = taken || shared[nb] == int(t);
				}
				if (!taken) {
					cand.push_back(int(t));
				}
				if (matching >= TILE_CHOICES && !cand.is_empty()) {
					break;
				}
			}
			if (!cand.is_empty()) {
				const int t = cand[hash_murmur3_one_32(s.order ^ 0x5157u) % cand.size()];
				shared[q] = t;
				shared_uses[t]++;
				continue;
			}
		}
		int best = -1;
		for (uint32_t t = 0; t < cuts.size(); t++) {
			const TileSquare &src = sq[cuts[t].source];
			if (cuts[t].uses < 6 && matches(s, src.level, src.sig, int(src.idx.size()), src.lsize, src.lsd, src.film, src.mix)) {
				best = int(t);
				break;
			}
		}
		if (best < 0 && cuts.size() < 4) {
			cuts.push_back(Cut{ q, 0 });
			best = int(cuts.size()) - 1;
		}
		if (best >= 0) {
			cuts[best].uses++;
			use[q] = best;
		} else {
			tile_skips[3].increment();
		}
	}
	// A tile used by its own square alone saves nothing: those squares keep their grains.
	LocalVector<int> tile_of_cut;
	tile_of_cut.resize(cuts.size());
	for (uint32_t t = 0; t < cuts.size(); t++) {
		tile_of_cut[t] = -1;
		if (cuts[t].uses < 2) {
			continue;
		}
		DetailTile tile;
		_tile_cut(gen, r_stones, sq[cuts[t].source], r_job.libs, rf_hash3(key.y, key.z, key.x, 4441u + t), tile);
		tile_of_cut[t] = int(r_job.tiles.size());
		r_job.tiles.push_back(tile);
	}
	// The placements, and the stones: each placed square's own grains replaced by its tile's, turned.
	LocalVector<bool> gone;
	gone.resize(r_stones.size());
	for (uint32_t i = 0; i < r_stones.size(); i++) {
		gone[i] = false;
	}
	LocalVector<Stone> added;
	LocalVector<int> added_place;
	for (int q = 0; q < units; q++) {
		const bool is_shared = shared[q] >= 0;
		if (!is_shared && (use[q] < 0 || tile_of_cut[use[q]] < 0)) {
			continue;
		}
		const TileSquare &s = sq[q];
		const uint32_t hq = hash_murmur3_one_32(s.order);
		const bool own_source = !is_shared && q == cuts[use[q]].source;
		const real_t turn = own_source ? 0.0 : real_t(hq & 3u) * Math::PI * 0.5;
		DetailPlacement pl;
		pl.shared = is_shared;
		pl.tile = is_shared ? shared[q] : tile_of_cut[use[q]];
		pl.square = s.rect;
		pl.xform = s.frame * Transform3D(Basis(Vector3(0, 1, 0), turn), Vector3());
		pl.plane = s.plane;
		const int pi = int(r_job.placements.size());
		r_job.placements.push_back(pl);
		for (uint32_t i : s.idx) {
			gone[i] = true;
		}
		auto add = [&](Stone st, const Transform3D &p_to) {
			const Transform3D xf = p_to * Transform3D(st.basis, st.position);
			st.position = xf.origin;
			st.basis = xf.basis;
			st.ground = float((-s.plane.normal.x * st.position.x - s.plane.normal.z * st.position.z + s.plane.d) / s.plane.normal.y);
			added.push_back(st);
			added_place.push_back(pi);
		};
		if (is_shared) {
			for (const Stone &st : set->cuts[shared[q]].stones) {
				add(st, pl.xform);
			}
		} else {
			const TileSquare &src = sq[cuts[use[q]].source];
			const Transform3D to = pl.xform * src.frame.affine_inverse();
			for (uint32_t i : src.idx) {
				add(r_stones[i], to);
			}
		}
	}
	if (r_job.placements.is_empty()) {
		return;
	}
	LocalVector<Stone> out;
	r_place.clear();
	for (uint32_t i = 0; i < r_stones.size(); i++) {
		if (!gone[i]) {
			out.push_back(r_stones[i]);
			r_place.push_back(-1);
		}
	}
	for (uint32_t i = 0; i < added.size(); i++) {
		out.push_back(added[i]);
		r_place.push_back(added_place[i]);
	}
	r_stones = out;
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
	cell.standing = r_job.standing;
	cell.snow = snow_burial ? r_job.snow.mean : -1.0f; // (0 with no snow yet: a pack arriving later lays it again)
	stone_count += cell.stones;
	// The tiles: one MultiMesh per shape of each (not instanced per grain in the path tracer: the merged BLAS of its
	// grains, which every placement shares), and per placement one RenderingServer instance per shape.
	const Ref<World3D> world = get_world_3d();
	const RID scenario = world.is_valid() ? world->get_scenario() : RID();
	auto build = [&](const DetailTile &p_tile, LocalVector<Ref<MultiMesh>> &r_mms, LocalVector<RID> &r_mats) {
		for (const DetailTileShape &sh : p_tile.shapes) {
			const Library *lib = libraries.getptr(sh.library);
			if (!lib || sh.variant >= int(lib->meshes.size())) {
				r_mms.push_back(Ref<MultiMesh>());
				r_mats.push_back(RID());
				continue;
			}
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_custom_data(true);
			mm->set_mesh(lib->meshes[sh.variant]);
			mm->set_instance_count(sh.count);
			RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), sh.buffer);
			r_mms.push_back(mm);
			const Ref<Material> mat = _material(key_type(sh.library), RockGenerator::Family(key_family(sh.library)), true);
			r_mats.push_back(mat.is_valid() ? mat->get_rid() : RID());
		}
	};
	LocalVector<LocalVector<Ref<MultiMesh>>> tile_mm;
	LocalVector<LocalVector<RID>> tile_mat;
	for (const DetailTile &tile : r_job.tiles) {
		LocalVector<Ref<MultiMesh>> mms;
		LocalVector<RID> mats;
		build(tile, mms, mats);
		for (const Ref<MultiMesh> &mm : mms) {
			if (mm.is_valid()) {
				cell.tile_meshes.push_back(mm);
			}
		}
		tile_mm.push_back(std::move(mms));
		tile_mat.push_back(std::move(mats));
	}
	// The block's shared tiles: held by the cell, each built the first time a placement takes it.
	TileSet *set = r_job.set;
	if (set) {
		set->users++;
		cell.tile_set = true;
		cell.tile_block = r_job.block;
	}
	LocalVector<uint32_t> single; // placements drawn as single grains
	for (const DetailPlacement &pl : r_job.placements) {
		TilePlacement tp;
		tp.square = pl.square;
		const Vector3 n = pl.plane.normal;
		const Vector4 plane(n.x, n.y, n.z, -pl.plane.d);
		if (pl.shared && (!set || pl.tile >= int(set->cuts.size()))) {
			cell.tiles.push_back(tp);
			continue;
		}
		if (pl.shared && set->cuts[pl.tile].mms.is_empty()) {
			// A shared tile is built only once TILE_MIN_USES placements have asked for it (its BLAS is a copy of every
			// grain's triangles: a tile that is used once or twice costs more than the grains it saves). Until then
			// its placements are drawn as their single grains: the same stones, the same place.
			if (++set->cuts[pl.tile].requests < TILE_MIN_USES) {
				single.push_back(cell.tiles.size());
				cell.tiles.push_back(tp);
				continue;
			}
			build(set->cuts[pl.tile].tile, set->cuts[pl.tile].mms, set->cuts[pl.tile].mats);
		}
		const LocalVector<Ref<MultiMesh>> &mms = pl.shared ? set->cuts[pl.tile].mms : tile_mm[pl.tile];
		const LocalVector<RID> &mats = pl.shared ? set->cuts[pl.tile].mats : tile_mat[pl.tile];
		for (uint32_t k = 0; k < mms.size(); k++) {
			if (mms[k].is_null() || !scenario.is_valid()) {
				continue;
			}
			RID inst = RS::get_singleton()->instance_create2(mms[k]->get_rid(), scenario);
			RS::get_singleton()->instance_set_transform(inst, pl.xform);
			RS::get_singleton()->instance_geometry_set_material_override(inst, mats[k]);
			RS::get_singleton()->instance_geometry_set_visibility_range(inst, 0.0f, vis, 0.0f, cs * 0.5f, RSE::VISIBILITY_RANGE_FADE_DISABLED);
			RS::get_singleton()->instance_geometry_set_shader_parameter(inst, SNAME("rock_tile_plane"), plane);
			RS::get_singleton()->instance_set_visible(inst, is_visible_in_tree());
			tp.instances.push_back(inst);
		}
		cell.tiles.push_back(tp);
		tile_stats[1]++;
	}
	tile_stats[0] += r_job.tiles.size();
	for (DetailGroup &dg : r_job.groups) {
		const Library *lib = libraries.getptr(dg.library);
		if (!lib || dg.variant >= int(lib->meshes.size())) {
			continue;
		}
		const int n = int(dg.xforms.size());
		if (dg.tiled) {
			// Drawn by its placements: kept for the finer layers' seeds and the contacts (which split a placement).
			Group grp;
			grp.library = dg.library;
			grp.variant = dg.variant;
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
				if (dg.place[k] >= 0 && dg.place[k] < int(cell.tiles.size())) {
					cell.tiles[dg.place[k]].members.push_back(Vector2i(int(cell.groups.size()), k));
				}
			}
			tile_stats[2] += n;
			cell.groups.push_back(grp);
			continue;
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(lib->meshes[dg.variant]);
		mm->set_instance_count(n);
		// Hundreds of grains of one shape sit under the merged BLAS cap, which would bake a copy of every grain's
		// triangles: give each its own TLAS entry over the shared shape instead (set first: it keeps a CPU copy).
		RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true);
		// Grains off screen that look smaller than this leave the path tracer's set (their shadows and bounces are below a pixel).
		RS::get_singleton()->multimesh_set_rt_offscreen_cull(mm->get_rid(), RF_DETAIL_RT_CULL_DEG);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), dg.buffer);
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
	for (uint32_t ti : single) {
		_split_tile(cell, ti, true, vis, cs * 0.5);
	}
	for (int k = 0; k < RF_PEDESTAL_SHAPES; k++) {
		if (r_job.pedestals[k].is_empty()) {
			continue;
		}
		// The rain splash pedestals' soil columns: one MultiMesh per column shape for the cell, instanced per column in
		// the path tracer as the grains are.
		_pedestal_resources();
		const int n = int(r_job.pedestals[k].size() / 12);
		Vector<float> buf;
		buf.resize(n * 12);
		memcpy(buf.ptrw(), r_job.pedestals[k].ptr(), sizeof(float) * n * 12);
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(pedestal_meshes[k]);
		mm->set_instance_count(n);
		RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true);
		RS::get_singleton()->multimesh_set_rt_offscreen_cull(mm->get_rid(), RF_DETAIL_RT_CULL_DEG);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), buf);
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_multimesh(mm);
		mmi->set_material_override(pedestal_material);
		mmi->set_visibility_range_end(vis);
		mmi->set_visibility_range_end_margin(cs * 0.5);
		add_child(mmi, false, INTERNAL_MODE_BACK);
		mmi->set_as_top_level(true);
		mmi->set_global_transform(Transform3D());
		cell.instances.push_back(mmi);
		pedestal_stones[2].add(uint64_t(n));
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
		_tile_set_adopt(*job);
		if (_layer_on(layer) && !cells.has(kv.key) && c.distance_to(Vector2(dc.x, dc.z)) <= layer_radius[layer] + cs * 1.5f) {
			_detail_commit(*job);
		}
		_tile_set_drop(*job); // the cell holds its own
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
		_tile_set_drop(*kv.value);
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
			_apply_holes(job->holes);
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
		_tile_set_adopt(job);
		_detail_commit(job);
		_tile_set_drop(job); // the cell holds its own
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
	const bool far_big = _far_on() && _far_layer(p_key.x);
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
		// In rank order (far_thinning): a far cell draws only its first records, so the order is the order stones
		// leave as the camera goes away; a hash per stone, the big ones last to go.
		{
			struct Ranked {
				float r;
				uint32_t i;
				bool operator<(const Ranked &p_o) const { return r < p_o.r; }
			};
			LocalVector<Ranked> rk;
			rk.resize(n);
			for (uint32_t k = 0; k < n; k++) {
				const Stone &st = p_stones[g.value[k]];
				const Vector3i q = Vector3i((st.position * 10.0f).floor());
				const float u = float(rf_hash3(q.x, q.y, q.z, 0x7a11u) & 0xFFFFFFu) / 16777216.0f;
				const float size = 2.0f * float(st.scale) * MAX(ax.x, MAX(ax.y, ax.z));
				// (The far tier's stones lead: past the radius they are drawn from the far blocks, so a thinning
				// cell keeps them to the end and the two meet without a gap.)
				const bool big = far_big && size >= float(far_min_size);
				rk[k] = { u / (1.0f + 0.6f * size) - (big ? 2.0f : 0.0f), g.value[k] };
				cg.big += big ? 1 : 0;
			}
			rk.sort();
			cg.members.resize(n);
			for (uint32_t k = 0; k < n; k++) {
				cg.members[k] = rk[k].i;
			}
		}
		cg.buffer.resize(n * 16);
		float *w = cg.buffer.ptrw();
		cg.xforms.resize(n);
		cg.custom.resize(n);
		cg.reach.resize(n);
		for (uint32_t k = 0; k < n; k++) {
			const Stone &st = p_stones[cg.members[k]];
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

// Main thread: cave holes a placement made (on a worker they wait in its job), cut into the generator and a Terrain3D.
void RockField3D::_apply_holes(const LocalVector<Transform3D> &p_holes) {
	TerrainGenerator3D *gen = _get_generator();
	if (!gen) {
		return;
	}
	for (const Transform3D &hb : p_holes) {
		if (gen->add_hole(hb)) {
			hole_rects.push_back(Rect2(hb.origin.x, hb.origin.z, 0, 0).grow(hb.basis.get_scale().length()));
			_terrain3d_hole(hb);
		}
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
	// Keep-out areas (WorldExclusion3D, editor-tools.md C): stones thin by a hash over the fade and none lie inside.
	if (layer != LAYER_LANDFORM && !exclusions.is_empty() && exclusions.any_in(rect.grow(2.0))) {
		for (int i = int(stones.size()) - 1; i >= 0; i--) {
			const Vector3 &sp = stones[i].position;
			const float k = exclusions.keep_out(Vector2(sp.x, sp.z));
			if (k > 0.0f && (k >= 1.0f || float(rf_hash3(int(Math::floor(sp.x * 100.0f)), int(Math::floor(sp.z * 100.0f)), 77, 0x9e37u) & 0xFFFF) / 65535.0f < k)) {
				stones.remove_at(i);
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
	for (const Stone &st : stones) {
		const Library *sl = libraries.getptr(st.library);
		_standing_append(st, sl, cell.standing);
	}
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
		if (rt_instanced_cells) {
			// One shared BLAS per shape and a TLAS entry per stone in the path tracer, not a merged BLAS that bakes a
			// copy of every stone's triangles (set first: it keeps a CPU copy of the buffer).
			RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true);
		}
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), cg.buffer);
		Group grp;
		grp.library = cg.library;
		grp.variant = cg.variant;
		grp.multimesh = mm;
		grp.xforms = std::move(cg.xforms);
		grp.custom = std::move(cg.custom);
		grp.reach = std::move(cg.reach);
		grp.keep_min = cg.big;
		grp.near.resize(n);
		grp.broken.resize(n);
		grp.shapes.resize(n);
		for (int k = 0; k < n; k++) {
			grp.near[k] = false;
			grp.broken[k] = false;
			grp.shapes[k] = nullptr;
			where[cg.members[k]] = Vector2i(int(cell.groups.size()), k);
		}
		// The LOD by each stone's own size: the shapes are built a unit across and each stone's scale lives in its
		// instance record, which the renderer's LOD choice does not see (it judges the mesh at its built size). The
		// group's biggest stone as the model scale: it gets exactly the LOD its true size calls for, and no stone a
		// coarser one than that (the smaller ones in the group stay at least as fine as they need).
		real_t lod_scale = 0.0;
		for (const Transform3D &xf : grp.xforms) {
			lod_scale = MAX(lod_scale, xf.basis.get_scale_abs().x);
		}
		cell.groups.push_back(grp);
		MultiMeshInstance3D *gi = _add_group_instance(mm, cg.library, vis, cs * 0.5);
		gi->set_lod_bias(MAX(lod_scale, real_t(0.001)));
		cell.instances.push_back(gi);
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
			_free_cell(cells[k]);
			cells.erase(k);
			_build_cell(k);
		}
	}
}

// Far thinning (docs/ideas/rocks.md, "Far rocks"; Ghost of Tsushima's distance density): a scatter layer keeps its
// full count over the inner half of its radius, then each cell draws only its first records (rank order, the big
// stones last), (r/2 / d)^2 of them, in eighths so the shapes and the path tracer's structures change seldom, and
// none by the radius, so a field thins out instead of ending at a line. Landmarks (tors, coast forms, landforms) are
// few and each one counts: they stay whole.
void RockField3D::_update_thinning(const Vector3 &p_target) {
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		const Layer layer = Layer(kv.key.x);
		if (_is_detail(layer)) {
			continue;
		}
		real_t keep = 1.0;
		if (far_thinning && layer != LAYER_TOR && layer != LAYER_COAST && layer != LAYER_LANDFORM) {
			const real_t cs = _cell_size(layer);
			const real_t r = layer_radius[layer];
			const real_t d = Vector2((kv.key.y + 0.5f) * cs, (kv.key.z + 0.5f) * cs).distance_to(Vector2(p_target.x, p_target.z));
			if (d > 0.5 * r) {
				keep = (0.5 * r / d) * (0.5 * r / d) * (1.0 - Math::smoothstep(real_t(0.8 * r), real_t(r + cs), real_t(d)));
				keep = Math::ceil(keep * 8.0) / 8.0;
			}
		}
		for (Group &g : kv.value.groups) {
			if (g.multimesh.is_null()) {
				continue;
			}
			const int n = g.multimesh->get_instance_count();
			const int want = (keep >= 1.0 || g.keep_min >= n) ? -1 : MAX(int(Math::ceil(n * keep)), g.keep_min);
			if (g.multimesh->get_visible_instance_count() != want) {
				g.multimesh->set_visible_instance_count(want);
			}
			// Widened survivors (Ghost of Tsushima's grass; trees' far_widen): the stones still drawn grow so their plan
			// cover matches all of the cell's, up to 1.6x (the cover holds to about 0.8 of the radius, where the terrain's
			// own far stones come in). The far tier's big stones lead and are drawn unscaled, so they meet their far copies.
			const int widen_for = far_widen ? want : -1;
			if (widen_for != g.widen_want) {
				g.widen_want = widen_for;
				float s = 1.0f;
				if (widen_for >= 0 && widen_for < n && uint32_t(n) <= g.reach.size()) {
					double all = 0.0, kept = 0.0;
					for (int k = g.keep_min; k < n; k++) {
						const double a = double(g.reach[k]) * double(g.reach[k]);
						all += a;
						kept += k < widen_for ? a : 0.0;
					}
					s = kept > 0.0 ? float(MIN(Math::sqrt(all / kept), 1.6)) : 1.0f;
				}
				if (Math::abs(s - g.widen) > 0.01f) {
					g.widen = s;
					_upload_widened(g);
				}
			}
		}
	}
}

void RockField3D::_upload_widened(Group &r_group) {
	const uint32_t n = uint32_t(r_group.multimesh->get_instance_count());
	if (r_group.xforms.size() < n || r_group.custom.size() < n) {
		return;
	}
	PackedFloat32Array buf;
	buf.resize(n * 16);
	float *w = buf.ptrw();
	for (uint32_t k = 0; k < n; k++) {
		const bool hidden = (k < r_group.near.size() && r_group.near[k]) || (k < r_group.broken.size() && r_group.broken[k]);
		const Transform3D xf = hidden ? Transform3D(Basis().scaled(Vector3(0, 0, 0)), r_group.xforms[k].origin) : _drawn_xform(r_group, k);
		float *rec = w + k * 16;
		for (int row = 0; row < 3; row++) {
			rec[row * 4 + 0] = xf.basis.rows[row][0];
			rec[row * 4 + 1] = xf.basis.rows[row][1];
			rec[row * 4 + 2] = xf.basis.rows[row][2];
			rec[row * 4 + 3] = xf.origin[row];
		}
		const Color &cd = r_group.custom[k];
		rec[12] = cd.r;
		rec[13] = cd.g;
		rec[14] = cd.b;
		rec[15] = cd.a;
	}
	RS::get_singleton()->multimesh_set_buffer(r_group.multimesh->get_rid(), buf);
}

void RockField3D::_update_far_cover(const Vector3 &p_target, bool p_force) {
	RenderingServer *rs = RenderingServer::get_singleton();
	TerrainGenerator3D *gen = _get_generator();
	if (!_far_on() || !gen) {
		if (far_cover_texture.is_valid()) {
			rs->global_shader_parameter_set("rock_far_cover_rect", Vector4());
			far_cover_texture.unref();
			far_reach_texture.unref();
			far_cover_origin = Vector2(1e20, 1e20);
		}
		return;
	}
	const int R = FAR_COVER_RES;
	const real_t size = R * FAR_COVER_TEXEL;
	// Centred on the target, moved in 1 km steps.
	const Vector2 o = (Vector2(p_target.x, p_target.z) / 1024.0).floor() * 1024.0 + Vector2(512.0 - size * 0.5, 512.0 - size * 0.5);
	if (!p_force && o == far_cover_origin) {
		return;
	}
	far_cover_origin = o;
	LocalVector<float> area, river_area;
	area.resize(R * R);
	river_area.resize(R * R);
	for (uint32_t i = 0; i < area.size(); i++) {
		area[i] = 0.0f;
		river_area[i] = 0.0f;
	}
	// Rockfall blocks under the faces (the generator's own list, as the layer places them; their debris skirts add
	// about a third), and the glacial boulders' mean cover on till.
	if (_layer_on(LAYER_ROCKFALL) && layer_density[LAYER_ROCKFALL] > 0.0) {
		const PackedVector4Array bl = gen->get_boulders(Rect2(o, Vector2(size, size)), layer_density[LAYER_ROCKFALL]);
		for (int i = 0; i < bl.size(); i++) {
			const Vector4 b = bl[i];
			const int ix = int(Math::floor((b.x - o.x) / FAR_COVER_TEXEL)), iz = int(Math::floor((b.z - o.y) / FAR_COVER_TEXEL));
			if (ix < 0 || iz < 0 || ix >= R || iz >= R) {
				continue;
			}
			const float r = 0.9f * float(b.w);
			area[iz * R + ix] += 1.3f * float(Math::PI) * r * r;
		}
	}
	// River beds of cobbles and boulders (the river layer's own Shields and Sternberg sizes): 45% of the bed in stone
	// where the median is coarse enough to be drawn near, spread along each segment in steps under half a texel.
	if (_layer_on(LAYER_RIVER) && layer_density[LAYER_RIVER] > 0.0) {
		_prepare_rivers();
		LocalVector<RiverSeg> segs;
		_river_segments(Rect2(o, Vector2(size, size)), segs);
		for (const RiverSeg &sg : segs) {
			if (sg.d50 * 2.2f < float(min_size)) {
				continue; // gravel and sand: the bed material shows it
			}
			const int steps = MAX(1, int(Math::ceil(sg.len / (0.5f * float(FAR_COVER_TEXEL)))));
			const float step = sg.len / steps;
			for (int k = 0; k < steps; k++) {
				const Vector2 p = sg.a + sg.f * ((k + 0.5f) * step);
				const int ix = int(Math::floor((p.x - o.x) / FAR_COVER_TEXEL)), iz = int(Math::floor((p.y - o.y) / FAR_COVER_TEXEL));
				if (ix < 0 || iz < 0 || ix >= R || iz >= R) {
					continue;
				}
				area[iz * R + ix] += 0.45f * step * sg.w * 1.2f * float(layer_density[LAYER_RIVER]);
				river_area[iz * R + ix] += 0.45f * step * sg.w * 1.2f * float(layer_density[LAYER_RIVER]);
			}
		}
	}
	const bool glacial = _layer_on(LAYER_GLACIAL) && layer_density[LAYER_GLACIAL] > 0.0;
	Vector<uint8_t> data;
	data.resize(R * R * 4);
	uint8_t *w = data.ptrw();
	// The hand-over (docs/ideas/ground-detail.md, "The middle-distance hand-over"): a river bed's cobbles are drawn
	// whole to 0.8 of the layer's radius (thinned, their survivors widened), then gone by a cell past it; its tone
	// fades in over exactly that. The rockfall blocks and boulders go on as the far tier's stones, so theirs keeps the
	// 250 to 700 m fade it was set to.
	const real_t rr = layer_radius[LAYER_RIVER];
	const real_t river_start = far_thinning ? 0.8 * rr : 0.95 * rr, river_end = rr + _cell_size(LAYER_RIVER);
	Vector<uint8_t> reach;
	reach.resize(R * R * 2);
	uint8_t *rw = reach.ptrw();
	const uint8_t def_start = uint8_t(250.0 / FAR_REACH_UNIT), def_end = uint8_t(700.0 / FAR_REACH_UNIT);
	LocalVector<uint8_t> has; // 2: river, 1: other stones, 0: none
	has.resize(R * R);
	for (int z = 0; z < R; z++) {
		for (int x = 0; x < R; x++) {
			const Vector3 c(o.x + (x + 0.5) * FAR_COVER_TEXEL, 0, o.y + (z + 0.5) * FAR_COVER_TEXEL);
			float cover = area[z * R + x] / float(FAR_COVER_TEXEL * FAR_COVER_TEXEL);
			if (glacial) {
				const Color geo = gen->get_geology_at(c);
				const float till = geo.b < 0.0f ? -geo.b : 0.0f;
				cover += 0.004f * till * float(layer_density[LAYER_GLACIAL]); // one boulder per 250 m2, about 1 m2 each
			}
			const float a = CLAMP(cover / 0.6f, 0.0f, 1.0f); // 60% plan cover: all stone
			uint8_t *px = w + (z * R + x) * 4;
			const bool river = river_area[z * R + x] > 0.5f * area[z * R + x] && river_area[z * R + x] > 0.0f;
			has[z * R + x] = a <= 0.0f ? 0 : (river ? 2 : 1);
			rw[(z * R + x) * 2 + 0] = river ? uint8_t(CLAMP(river_start / FAR_REACH_UNIT, 0.0, 255.0)) : def_start;
			rw[(z * R + x) * 2 + 1] = river ? uint8_t(CLAMP(river_end / FAR_REACH_UNIT, 0.0, 255.0)) : def_end;
			if (a <= 0.0f) {
				px[0] = px[1] = px[2] = px[3] = 0;
				continue;
			}
			const Color rind = RockGenerator::get_lithology(_type_at(gen, c)).rind;
			px[0] = uint8_t(CLAMP(rind.r * 255.0f, 0.0f, 255.0f));
			px[1] = uint8_t(CLAMP(rind.g * 255.0f, 0.0f, 255.0f));
			px[2] = uint8_t(CLAMP(rind.b * 255.0f, 0.0f, 255.0f));
			px[3] = uint8_t(a * 255.0f);
		}
	}
	// Empty texels next to a bed take its reach (the nearer fade wins), so filtering does not blend it with the default.
	for (int z = 0; z < R; z++) {
		for (int x = 0; x < R; x++) {
			if (has[z * R + x] != 0) {
				continue;
			}
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int nx = x + dx, nz = z + dz;
					if (nx < 0 || nz < 0 || nx >= R || nz >= R || has[nz * R + nx] != 2) {
						continue;
					}
					rw[(z * R + x) * 2 + 0] = MIN(rw[(z * R + x) * 2 + 0], rw[(nz * R + nx) * 2 + 0]);
					rw[(z * R + x) * 2 + 1] = MIN(rw[(z * R + x) * 2 + 1], rw[(nz * R + nx) * 2 + 1]);
				}
			}
		}
	}
	Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_RGBA8, data);
	Ref<Image> rimg = Image::create_from_data(R, R, false, Image::FORMAT_RG8, reach);
	if (far_cover_texture.is_valid() && far_reach_texture.is_valid()) {
		far_cover_texture->update(img);
		far_reach_texture->update(rimg);
	} else {
		far_cover_texture = ImageTexture::create_from_image(img);
		far_reach_texture = ImageTexture::create_from_image(rimg);
	}
	rs->global_shader_parameter_set("rock_far_cover_map", far_cover_texture->get_rid());
	rs->global_shader_parameter_set("rock_far_reach_map", far_reach_texture->get_rid());
	rs->global_shader_parameter_set("rock_far_cover_rect", Vector4(o.x, o.y, size, size));
}

bool RockField3D::_far_layer(int p_layer) const {
	if (p_layer < 0 || p_layer >= LAYER_MAX || !_layer_on(p_layer) || layer_radius[p_layer] >= far_radius) {
		return false;
	}
	switch (p_layer) {
		case LAYER_ROCKFALL:
		case LAYER_GLACIAL:
		case LAYER_TOR:
		case LAYER_COAST:
			return true;
		case LAYER_LANDFORM:
			return !cave_mouths; // (a cave mouth opens the terrain as it is placed: not from a far block)
		default:
			return false;
	}
}

// Any thread: each of the block's cells placed and packed exactly as its near cell would be, keeping the records of
// the stones that lead (at least far_min_size), so a stone looks the same from the far block and from its cell.
void RockField3D::_far_place(FarJob &r_job) {
	CoarseJob probe; // collects the shape libraries asked for that are not built
	const bool worker = rf_job_generator != nullptr; // (set by _far_job)
	if (worker) {
		rf_job_generator = r_job.gen;
		rf_coarse_job = &probe;
	}
	rf_far = true;
	for (const Vector3i &key : r_job.cells) {
		const real_t cs = _cell_size(Layer(key.x));
		LocalVector<Stone> stones;
		_place(Layer(key.x), Rect2(key.y * cs, key.z * cs, cs, cs), rf_hash3(key.x, key.y, key.z, uint32_t(seed) * 2654435761u), stones);
		if (!probe.missing.is_empty()) {
			break;
		}
		LocalVector<CoarseGroup> groups;
		_coarse_pack(key, stones, groups);
		for (const CoarseGroup &cg : groups) {
			if (cg.big <= 0) {
				continue;
			}
			Vector<float> &rec = r_job.records[(uint64_t(cg.library) << 16) | uint64_t(cg.variant)];
			const int at = rec.size();
			rec.resize(at + cg.big * 16);
			memcpy(rec.ptrw() + at, cg.buffer.ptr(), sizeof(float) * cg.big * 16);
		}
	}
	rf_far = false;
	r_job.missing = probe.missing;
	r_job.holes = probe.holes;
	if (worker) {
		rf_coarse_job = nullptr;
		rf_job_generator = nullptr;
	}
}

void RockField3D::_far_job(void *p_job) {
	FarJob *job = static_cast<FarJob *>(p_job);
	rf_job_generator = job->gen; // (marks the thread a worker for _far_place)
	job->field->_far_place(*job);
	rf_job_generator = nullptr;
}

void RockField3D::_free_far(FarBlock &r_block) {
	for (MultiMeshInstance3D *mmi : r_block.instances) {
		if (mmi) {
			mmi->queue_free();
		}
	}
	r_block.instances.clear();
}

void RockField3D::_far_commit(FarJob &r_job) {
	if (FarBlock *old = far_blocks.getptr(r_job.key)) {
		_free_far(*old);
		far_blocks.erase(r_job.key);
	}
	_apply_holes(r_job.holes);
	FarBlock fb;
	fb.sig = r_job.sig;
	const real_t cs = _cell_size(Layer(r_job.key.x));
	for (const KeyValue<uint64_t, Vector<float>> &kv : r_job.records) {
		const uint32_t lk = uint32_t(kv.key >> 16);
		const int v = int(kv.key & 0xFFFF);
		const Library *lib = libraries.getptr(lk);
		const int n = kv.value.size() / 16;
		if (!lib || v >= int(lib->meshes.size()) || n <= 0) {
			continue;
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(lib->meshes[v]);
		mm->set_instance_count(n);
		RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), kv.value);
		fb.instances.push_back(_add_group_instance(mm, lk, far_radius + cs * FAR_BLOCK * 0.75, cs));
	}
	far_blocks.insert(r_job.key, fb);
	far_last_cells = -1; // look again: the cells may have moved on while it was placed
}

void RockField3D::_far_cancel() {
	for (KeyValue<Vector3i, FarJob *> &kv : far_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
		memdelete(kv.value);
	}
	far_jobs.clear();
	for (KeyValue<Vector3i, FarBlock> &kv : far_blocks) {
		_free_far(kv.value);
	}
	far_blocks.clear();
	far_last_cells = -1;
	if (far_cover_texture.is_valid()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("rock_far_cover_rect", Vector4());
		far_cover_texture.unref();
		far_reach_texture.unref();
	}
	far_cover_origin = Vector2(1e20, 1e20);
}

void RockField3D::build_far() {
	far_last_cells = -1;
	_update_far_cover(_target_position(), true);
	far_fill = true; // (no streaming: the cells inside the radius that are not built get their big stones too)
	_update_far(_target_position(), true);
	far_fill = false;
}

int RockField3D::get_far_stone_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, FarBlock> &kv : far_blocks) {
		for (const MultiMeshInstance3D *mmi : kv.value.instances) {
			if (mmi && mmi->get_multimesh().is_valid()) {
				n += mmi->get_multimesh()->get_instance_count();
			}
		}
	}
	return n;
}

void RockField3D::set_far_radius(real_t p_radius) {
	far_radius = MAX(p_radius, real_t(0.0));
	far_last_cells = -1;
}

void RockField3D::set_far_min_size(real_t p_size) {
	far_min_size = MAX(p_size, real_t(0.1));
	far_last_cells = -1;
}

// The far tier (docs/ideas/rocks.md, "Far rocks"; Unreal's HLOD, Horizon Zero Dawn's far placement): past each
// landmark and boulder layer's radius, to far_radius, blocks of FAR_BLOCK x FAR_BLOCK cells draw the stones of at
// least far_min_size (big boulders, erratics, cliff blocks, tors, stacks, hoodoos) with their shapes' own LODs. A block
// holds only the cells no near cell covers; it is placed again on a worker when that set changes, the old one drawn
// meanwhile.
void RockField3D::_update_far(const Vector3 &p_target, bool p_all) {
	LocalVector<Vector3i> done;
	LocalVector<uint32_t> start;
	for (KeyValue<Vector3i, FarJob *> &kv : far_jobs) {
		FarJob *job = kv.value;
		if (!p_all && !WorkerThreadPool::get_singleton()->is_task_completed(job->task)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		if (!job->missing.is_empty()) {
			for (uint32_t k : job->missing) {
				if (!libraries.has(k) && !library_builds.has(k) && start.find(k) < 0) {
					start.push_back(k);
				}
			}
			far_last_cells = -1;
		} else if (_far_on() && _far_layer(kv.key.x)) {
			_far_commit(*job);
		}
		memdelete(job);
		done.push_back(kv.key);
	}
	for (const Vector3i &k : done) {
		far_jobs.erase(k);
	}
	if (!start.is_empty()) {
		LibraryBuild *b = _start_libraries(start);
		for (uint32_t k : start) {
			library_builds.insert(k, b);
		}
	}
	_update_far_cover(p_target, false);
	if (!_far_on()) {
		if (!far_blocks.is_empty()) {
			for (KeyValue<Vector3i, FarBlock> &kv : far_blocks) {
				_free_far(kv.value);
			}
			far_blocks.clear();
		}
		return;
	}
	if (!p_all && int(cells.size()) == far_last_cells && Vector2(p_target.x, p_target.z).distance_to(Vector2(far_last_target.x, far_last_target.z)) < 8.0) {
		return;
	}
	far_last_target = p_target;
	far_last_cells = int(cells.size());
	struct Want {
		Vector3i key;
		real_t dist;
		uint64_t sig;
		LocalVector<Vector3i> in;
	};
	LocalVector<Want> want;
	HashSet<Vector3i> keep;
	const Vector2 t2(p_target.x, p_target.z);
	auto nearest_to = [&](const Rect2 &p_r) {
		return Vector2(CLAMP(t2.x, p_r.position.x, p_r.get_end().x), CLAMP(t2.y, p_r.position.y, p_r.get_end().y)).distance_to(t2);
	};
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!_far_layer(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const real_t bs = cs * FAR_BLOCK;
		const real_t r = layer_radius[l];
		const int bx0 = int(Math::floor((t2.x - far_radius) / bs)), bx1 = int(Math::floor((t2.x + far_radius) / bs));
		const int bz0 = int(Math::floor((t2.y - far_radius) / bs)), bz1 = int(Math::floor((t2.y + far_radius) / bs));
		for (int bz = bz0; bz <= bz1; bz++) {
			for (int bx = bx0; bx <= bx1; bx++) {
				const Rect2 br(bx * bs, bz * bs, bs, bs);
				const real_t bd = nearest_to(br);
				if (bd > far_radius) {
					continue;
				}
				Want w;
				w.key = Vector3i(l, bx, bz);
				w.dist = bd;
				w.sig = 0xcbf29ce484222325ull;
				for (int cz = bz * FAR_BLOCK; cz < (bz + 1) * FAR_BLOCK; cz++) {
					for (int cx = bx * FAR_BLOCK; cx < (bx + 1) * FAR_BLOCK; cx++) {
						const real_t d = nearest_to(Rect2(cx * cs, cz * cs, cs, cs));
						const Vector3i ck(l, cx, cz);
						if (d > far_radius || (d <= r && !far_fill) || cells.has(ck) || coarse_jobs.has(ck)) {
							continue; // out of reach, or a near cell's
						}
						w.in.push_back(ck);
						w.sig = (w.sig ^ uint64_t(rf_hash3(cx, cz, l, 0xfa4u))) * 1099511628211ull;
					}
				}
				if (w.in.is_empty()) {
					continue;
				}
				keep.insert(w.key);
				const FarBlock *fb = far_blocks.getptr(w.key);
				if ((fb && fb->sig == w.sig) || far_jobs.has(w.key)) {
					continue; // up to date, or being placed (looked at again once it lands)
				}
				want.push_back(std::move(w));
			}
		}
	}
	LocalVector<Vector3i> gone;
	for (KeyValue<Vector3i, FarBlock> &kv : far_blocks) {
		if (!keep.has(kv.key)) {
			_free_far(kv.value);
			gone.push_back(kv.key);
		}
	}
	for (const Vector3i &k : gone) {
		far_blocks.erase(k);
	}
	if (want.is_empty()) {
		return;
	}
	struct ByDist {
		_FORCE_INLINE_ bool operator()(const Want &p_a, const Want &p_b) const { return p_a.dist < p_b.dist; }
	};
	want.sort_custom<ByDist>();
	_prepare_rivers(); // read by the placement
	TerrainGenerator3D *gen = _get_generator();
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count() - 1);
	for (Want &w : want) {
		FarJob *job = memnew(FarJob);
		job->field = this;
		job->key = w.key;
		job->sig = w.sig;
		job->cells = std::move(w.in);
		job->gen = gen;
		if (!cell_threads || p_all) {
			_far_place(*job);
			if (job->missing.is_empty()) {
				_far_commit(*job);
			}
			memdelete(job);
			continue;
		}
		if (int(far_jobs.size()) >= max_jobs) {
			memdelete(job);
			far_last_cells = -1; // the rest next time
			break;
		}
		_prepare_rivers(); // read by the workers
		job->task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_far_job, job, false, "RockField3D far blocks");
		far_jobs.insert(w.key, job);
	}
}

void RockField3D::_update_streaming(bool p_all) {
	_watch_holes();
	_watch_stand();
	_watch_ground();
	TerrainGenerator3D *gen = _get_generator();
	const Vector3 tp = _target_position();
	if (!gen || !Math::is_finite(rf_height(gen, tp))) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_update_detail_radii();
	_update_cover(p_all);
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
	_update_thinning(tp);
	_update_far(tp, p_all);
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
		if (threads && ground_detail_tiles) {
			// Its block's shared tiles are being cut by another cell's job: wait for them rather than cut them twice.
			const Vector3i b = _tile_block(w.key);
			if (!tile_sets.has(b) && tile_set_pending.has(b)) {
				layer_waiting = true;
				continue;
			}
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
					grp.multimesh->set_instance_transform(int(k), (want || grp.broken[k]) ? Transform3D(Basis().scaled(Vector3(0, 0, 0)), grp.xforms[k].origin) : _drawn_xform(grp, k));
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
		real_t lod_scale = 0.0;
		for (uint32_t k = 0; k < kv.value.size(); k++) {
			mm->set_instance_transform(int(k), kv.value[k]);
			mm->set_instance_custom_data(int(k), cd[k]);
			lod_scale = MAX(lod_scale, kv.value[k].basis.get_scale_abs().x);
		}
		mmi->set_lod_bias(MAX(lod_scale, real_t(0.001))); // the LOD by the biggest stone's true size, as the cells
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
						grp.multimesh->set_instance_transform(int(k), _drawn_xform(grp, k));
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
	// A ledge slab that fails: a cantilever breaks at its root, where the bending moment is greatest, along the joint
	// or stress-release crack over the bench's edge (cantilever failure of undercut caprock and notched cliffs: Young
	// and Ashford 2008, Stock et al. 2012; toppling of the joint blocks, Lamb and Dietrich 2009). Its back stays held
	// under the bench (ANCHOR_BACK), the root is cut through, and the part out over the lip falls as one piece,
	// pivoting off the bench's edge, and breaks where it lands.
	const bool ledge_fall = p_fall && key_family(p_grp.library) == RockGenerator::FAMILY_LEDGE;
	Vector3 root;
	real_t root_thick = 0.0;
	Vector3 root_out;
	if (ledge_fall) {
		const Library *lib = libraries.getptr(p_grp.library);
		TerrainGenerator3D *gen = _get_generator();
		if (!lib || p_grp.variant >= lib->axes.size() || !gen) {
			node->queue_free();
			return nullptr;
		}
		const Vector3 ax = lib->axes[p_grp.variant];
		const Vector3 ya = xf.basis.get_column(1).normalized(), za = xf.basis.get_column(2).normalized();
		root_thick = 2.0 * ax.y * s;
		root_out = za;
		const Vector3 front = xf.origin + ya * (ax.y * s) + za * (ax.z * s);
		// The drawn ground, heightfield plus the landscape's relief: what the slab was seated on and the collision
		// follows.
		const WorldGround *wg = WorldContracts::ground();
		auto drawn = [&](const Vector3 &p_at) -> real_t {
			real_t g = rf_height(gen, p_at);
			if (wg) {
				const Vector2 rl = wg->relief_at(Vector3(p_at.x, g, p_at.z));
				if (rl.y > 0.0f && Math::is_finite(rl.x)) {
					g += rl.x;
				}
			}
			return g;
		};
		// The bench's edge: back from the front, just under the slab's top, the first point the ground stands at.
		real_t t = 0.0;
		const real_t depth = 2.0 * ax.z * s;
		for (; t < depth * 0.8; t += 0.05) {
			const Vector3 q = front - za * t - ya * (root_thick * 0.3);
			if (drawn(q) >= q.y) {
				break;
			}
		}
		if (t >= depth * 0.8) {
			node->queue_free();
			return nullptr; // no bench under it to break off from
		}
		// (A slab whose front stands on the bench all along still breaks a fifth of the way in: its joint.)
		t = CLAMP(t, depth * 0.2, depth * 0.6);
		root = front - za * t - ya * (root_thick * 0.5);
		// The part that falls must start out over the face below the lip, not in the ground: its middle in the air.
		// (A convex body whose middle starts under the collision's one-sided surface falls through it.)
		// (The drawn ground, heightfield plus the landscape's relief: what the collision follows.)
		const Vector3 free_mid = front - za * (t * 0.5) - ya * (root_thick * 0.5);
		// (Clear of the heightfield as well: the collision may follow it without the relief's undercut.)
		if (MAX(drawn(free_mid), rf_height(gen, free_mid)) > free_mid.y) {
			node->queue_free();
			return nullptr;
		}
		node->set("dynamic", false);
		node->set("anchor", 3); // ANCHOR_BACK
		node->set("kill_y", rf_height(gen, xf.origin) - 400.0);
		node->set("impact_strength", mass * 3.0);
		node->set("impact_damage_scale", 1.0 / mass);
		node->set("impact_radius", MAX(p_grp.reach[p_k] * 1.5f, real_t(0.5)));
		node->set("shatter_speed", 0.6); // the crack's own spall: a few chips, not a blast
		node->set("freeze_when_asleep", true);
		// Where its underside starts in the face below the lip (the heightfield has no undercut), the contact pushes
		// it out faster than gravity draws it in (at 1 m/s it sank through), yet without a leap.
		node->set("max_depenetration_velocity", 4.0);
	} else if (p_fall) {
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
			const real_t gx = (rf_height(gen, c + Vector3(e, 0, 0)) - rf_height(gen, c - Vector3(e, 0, 0))) / (2.0 * e);
			const real_t gz = (rf_height(gen, c + Vector3(0, 0, e)) - rf_height(gen, c - Vector3(0, 0, e))) / (2.0 * e);
			out = Vector3(-gx, 1, -gz).normalized();
			node->set("kill_y", rf_height(gen, c) - 400.0);
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
	if (ledge_fall) {
		// The crack through the root: a plane across the slab over the bench's edge, facing out.
		node->call("apply_plane_cut", Plane(root_out, root));
	}
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
		return rf_height(gen, Vector3(p_x, 0, p_z));
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
bool RockField3D::_pick_fall(const Vector3 &p_center, real_t p_min, real_t p_max, Vector3 &r_origin, uint64_t &r_key, bool p_ledges) {
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
			// Ledge slabs only by trigger_rockfall for now: in the bench the falling part still drops through the
			// collision ground (docs/ideas/rocks.md, "Falling ledges").
			if (!p_ledges && key_family(grp.library) == RockGenerator::FAMILY_LEDGE) {
				continue;
			}
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
	if (!_pick_fall(p_position, 0.0, p_radius, origin, key, true)) {
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
	if (Engine::get_singleton()->is_editor_hint()) {
		set_process_internal(editor_preview && streaming);
		set_physics_process_internal(false);
		return;
	}
	set_process_internal(streaming || live_rockfall || ground_contact || frost_sorting);
	set_physics_process_internal(rockfall_dust || stones_react);
}

void RockField3D::set_editor_preview(bool p_enable) {
	if (editor_preview == p_enable) {
		return;
	}
	editor_preview = p_enable;
	if (!is_inside_tree() || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	_update_process();
	if (!editor_preview) {
		clear();
	}
}

Callable RockField3D::get_rebuild_button() {
	return callable_mp(this, &RockField3D::rebuild);
}

/* Stones that react */

// The stones a contact body touches, each physics frame (docs/ideas/ground-detail.md, "Stones that react").
// Technique: an active zone round each contact, as in Servin et al.'s multiscale terrain (2021: the soil under a tool
// turns into discrete particles while it is disturbed and merges back once at rest). Here the zone is the body's
// footprint: the stones in it that have no collision of their own become rigid bodies (a convex hull from their
// shape's own hull) with the push of the body, up to react_max_bodies at a time, the rest (and the finest) are shoved
// out of the footprint. A loose stone stays an instance of its MultiMesh, moved each frame to its body, so the
// raster and the path tracer draw it as before; at rest it is a record again where it lies.
void RockField3D::_update_react(double p_delta) {
	if (!stones_react || !is_inside_tree()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const TerrainGenerator3D *gen = _get_generator();
	const float dt = float(p_delta);
	// The loose stones: follow their bodies, settle the ones at rest.
	for (int i = int(loose.size()) - 1; i >= 0; i--) {
		Loose &l = loose[i];
		Cell *c = cells.getptr(l.cell);
		if (!c || l.group >= c->groups.size() || l.k >= c->groups[l.group].xforms.size()) {
			_free_loose(i);
			continue;
		}
		Transform3D bt = ps->body_get_state(l.body, PhysicsServer3D::BODY_STATE_TRANSFORM);
		const Vector3 v = ps->body_get_state(l.body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY);
		const bool sleeping = ps->body_get_state(l.body, PhysicsServer3D::BODY_STATE_SLEEPING);
		l.age += dt;
		l.idle = v.length() < 0.05f ? l.idle + dt : 0.0f;
		bool done = sleeping || l.idle > 0.4f || l.age > 8.0f;
		if (gen) {
			// Through the ground (no collision there, or tunnelled): back on it, at rest.
			const float ground = float(rf_height(gen, bt.origin));
			if (bt.origin.y < ground - 0.5f * l.reach) {
				bt.origin.y = ground + 0.3f * l.reach;
				done = true;
			}
		}
		Group &g = c->groups[l.group];
		g.xforms[l.k] = Transform3D(bt.basis * Basis::from_scale(Vector3(l.scale, l.scale, l.scale)), bt.origin);
		g.multimesh->set_instance_transform(int(l.k), g.xforms[l.k]);
		if (done) {
			react_settled++;
			_free_loose(i);
		}
	}
	// The contact bodies near the ground.
	Vector<Node *> movers = get_tree()->get_nodes_in_group(SNAME("world_contact"));
	HashMap<ObjectID, Vector3> seen;
	const Ref<World3D> world = get_world_3d();
	if (world.is_null()) {
		return;
	}
	LocalVector<ReactPress> presses;
	react_walkers.begin();
	for (Node *n : movers) {
		Node3D *n3 = Object::cast_to<Node3D>(n);
		if (!n3 || !n3->is_visible_in_tree() || seen.has(n3->get_instance_id())) {
			continue;
		}
		const WorldContactBody wb = WorldContracts::describe_body(n3);
		const Vector3 *last = react_last.getptr(n3->get_instance_id());
		Vector3 vel;
		if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(n3)) {
			vel = rb->get_linear_velocity();
		} else if (const CharacterBody3D *cb = Object::cast_to<CharacterBody3D>(n3)) {
			vel = cb->get_velocity();
		} else if (last && dt > 0.0f) {
			vel = (wb.center - *last) / dt;
		}
		if (vel.length_squared() > 900.0f) {
			vel = Vector3(); // teleported
		}
		seen.insert(n3->get_instance_id(), wb.center);
		// A walker (a character) presses its soles, a step at a time (WorldWalkerSteps), never its body shape; one whose
		// feet are contact bodies of their own presses through them.
		if (wb.walker) {
			if (wb.feet) {
				continue;
			}
			LocalVector<WorldWalkerStep> steps;
			react_walkers.step(n3, wb, steps);
			for (const WorldWalkerStep &st : steps) {
				ReactPress pr;
				pr.center = st.position;
				pr.bottom = st.position.y;
				pr.oval = true;
				pr.fu = Vector2(Math::cos(st.yaw), -Math::sin(st.yaw));
				pr.fv = Vector2(-pr.fu.y, pr.fu.x);
				pr.hu = st.length * 0.5f;
				pr.hw = st.width * 0.5f;
				pr.foot = pr.hu;
				presses.push_back(pr); // a sole set down: at rest, it presses
			}
			continue;
		}
		ReactPress pr;
		pr.center = wb.center;
		pr.vel = vel;
		pr.wheel = wb.wheel;
		pr.bottom = wb.center.y - wb.half_height;
		pr.foot = wb.wheel ? MAX(wb.width * 0.5f, wb.half_height * 0.35f) : wb.radius;
		pr.hu = pr.hw = pr.foot;
		// A box (a boot, a sole): its footprint is its own oblong, not the circle round it.
		if (wb.box) {
			Vector2 fu, fv;
			float hu = pr.foot, hw = pr.foot;
			for (int c = 0; c < n3->get_child_count(); c++) {
				const CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(n3->get_child(c));
				const BoxShape3D *bx = cs && !cs->is_disabled() ? Object::cast_to<BoxShape3D>(cs->get_shape().ptr()) : nullptr;
				if (!bx) {
					continue;
				}
				const Basis cb = cs->get_global_basis();
				const Vector3 ex = cb.get_column(0), ez = cb.get_column(2);
				fu = Vector2(ex.x, ex.z);
				fv = Vector2(ez.x, ez.z);
				if (fu.length() > 1e-3f && fv.length() > 1e-3f) {
					hu = bx->get_size().x * 0.5f * fu.length();
					hw = bx->get_size().z * 0.5f * fv.length();
					fu.normalize();
					fv.normalize();
					pr.oval = true;
					pr.fu = fu;
					pr.fv = fv;
					pr.hu = hu;
					pr.hw = hw;
				}
				break;
			}
		}
		presses.push_back(pr);
	}
	react_walkers.end();
	for (const ReactPress &pr : presses) {
		const bool oval = pr.oval;
		const Vector2 fu = pr.fu, fv = pr.fv;
		const float hu = pr.hu, hw = pr.hw, foot = pr.foot, bottom = pr.bottom;
		const Vector3 vel = pr.vel;
		const Vector3 centre = pr.center;
		const bool wheel = pr.wheel;
		const float foot_min = oval ? MIN(hu, hw) : foot;
		if (gen && bottom > float(rf_height(gen, centre)) + 0.25f) {
			continue; // off the ground
		}
		WorldGround *wg = WorldContracts::ground();
		const bool dented = wg && wg->has_provider();
		const Vector2 c2(centre.x, centre.z);
		Vector2 hv(vel.x, vel.z);
		const float speed = hv.length();
		// At rest on them: what it pressed aside on arrival stays aside (a stone rolling back stays put), and the stones
		// under it only follow the print down as the ground gives (a sole's print deepens while it stands).
		const bool resting = vel.length() < 0.15f;
		// Every layer with loose stones: the ground detail classes and the small stones of the coarse layers that
		// have no collision of their own.
		static const Layer react_layers[6] = { LAYER_DETAIL_PEBBLE, LAYER_DETAIL_GRAVEL, LAYER_DETAIL_FINE, LAYER_SHORE, LAYER_RIVER, LAYER_SCREE };
		for (Layer layer : react_layers) {
			const real_t cs = _cell_size(layer);
			const int x0 = int(Math::floor((c2.x - foot - 0.5f) / cs)), x1 = int(Math::floor((c2.x + foot + 0.5f) / cs));
			const int z0 = int(Math::floor((c2.y - foot - 0.5f) / cs)), z1 = int(Math::floor((c2.y + foot + 0.5f) / cs));
			for (int cz = z0; cz <= z1; cz++) {
				for (int cx = x0; cx <= x1; cx++) {
					const Vector3i key(layer, cx, cz);
					Cell *cell = cells.getptr(key);
					if (!cell) {
						continue;
					}
					// A tile placement it reaches goes back to single grains first.
					for (uint32_t ti = 0; ti < cell->tiles.size(); ti++) {
						if (!cell->tiles[ti].instances.is_empty() && cell->tiles[ti].square.grow(foot + 0.3f).has_point(c2)) {
							_split_tile(*cell, ti);
						}
					}
					for (uint32_t gi = 0; gi < cell->groups.size(); gi++) {
						Group &g = cell->groups[gi];
						if (g.multimesh.is_null()) {
							continue; // grains a tile placement draws
						}
						const uint32_t fam = key_family(g.library);
						if (fam >= RockGenerator::FAMILY_SEA_STACK) {
							continue;
						}
						const Library *lib = libraries.getptr(g.library);
						for (uint32_t k = 0; k < g.xforms.size(); k++) {
							if (g.broken[k] || g.near[k] || g.shapes[k]) {
								continue;
							}
							const Vector3 o = g.xforms[k].origin;
							const float r = float(g.reach[k]);
							if (r > 0.3f) {
								continue; // a boulder: not shoved by a foot or a wheel
							}
							Vector2 d(o.x - c2.x, o.z - c2.y);
							const float dl = d.length();
							// How far out in the footprint, 1 at its edge.
							const float edge = oval ? Math::sqrt(Math::pow(float(d.dot(fu) / hu), 2.0f) + Math::pow(float(d.dot(fv) / hw), 2.0f)) : dl / MAX(foot, 1e-3f);
							if (edge > 1.0f + r / MAX(foot_min, 1e-3f) || o.y + r < bottom - 0.02f) {
								continue;
							}
							bool is_loose = false;
							for (const Loose &l : loose) {
								if (l.cell == key && l.group == gi && l.k == k) {
									is_loose = true;
									break;
								}
							}
							if (is_loose) {
								continue;
							}
							const Vector2 out = dl > 1e-4f ? d / dl : (speed > 1e-3f ? hv / speed : Vector2(1, 0));
							const float s = float(g.xforms[k].basis.get_scale().x);
							// What a rolling or stepping contact does to a grain bed: most grains are pressed into it, a share
							// squeezed out sideways into a low berm, and a few flicked (a stone caught by the edge of the tread
							// or the sole). Picked per stone (a hash, so a stone gets the same answer on every frame it is under).
							const uint32_t hs = hash_murmur3_one_32(uint32_t(k) * 2654435761u ^ hash_murmur3_one_32(uint32_t(key.y) * 73856093u ^ uint32_t(key.z) * 19349663u ^ gi));
							const float pick = float(hs & 0xffffu) / 65536.0f;
							const float ground_gen = gen ? float(rf_height(gen, o)) : o.y - r;
							float ground_here = ground_gen;
							if (dented) {
								// A deformable ground under the stones (GroundSurface3D's sand or mud): the print it took.
								const float h = wg->height_at(o);
								if (Math::is_finite(h) && h < ground_here) {
									ground_here = h;
								}
							}
							// Pressed in the middle of the print, pushed out at its edge (so the displaced stones heap in a rim).
							// For a body going along, the middle is its line: distance across it (else a stone first met at the
							// leading edge would always count as at the edge).
							// A sole sets down and presses its whole print, its edge pushing a modest rim; a wheel packs the
							// stones of its rut flat and closer, squeezing out only those at its edge.
							float across = edge;
							if (!oval && speed > 0.3f) {
								across = Math::abs(d.dot(Vector2(-hv.y, hv.x) / speed)) / MAX(foot, 1e-3f);
							}
							const float press = wheel ? 0.9f * (1.0f - Math::smoothstep(0.7f, 1.0f, across)) : 0.85f * (1.0f - Math::smoothstep(0.7f, 1.05f, across));
							// A disturbed stone shows its damp underside (the film darkens it, as the wet ones by the water) and
							// is no longer eased into the ground it sat on.
							// A pressed stone is set in damp soil from under the surface (0.75); one shoved or flicked onto the
							// dry top keeps only a light film (0.4: darker read as a hole on a pale boulder).
							const auto disturb = [&](uint32_t p_k, float p_ground, float p_film = 0.4f) {
								Color &cd = g.custom[p_k];
								cd.g = cd.g >= 2.0f ? MAX(cd.g, 2.0f + p_film) : MAX(cd.g, p_film);
								cd.r = p_ground;
								g.multimesh->set_instance_custom_data(int(p_k), cd);
							};
							if (resting && pick >= press) {
								continue;
							}
							if (pick < press) {
								// Pressed into the bed and laid flat (its own yaw kept; once: a stone already down stays as it is):
								// a print or a rut is a flatter, smoother patch of the same stones (most of each still showing over the
								// dent, so the stones cover it, not the sand), darker with the damp from under.
								// Bedded in the dent, but no deeper than 0.6 of its size under the undisturbed ground: the stones
								// fill the print as packed gravel does (and a ground drawn coarser under the path tracer, its dent
								// shallower, does not close over them).
								const float y = MIN(o.y, MAX(ground_here - 0.1f * r, ground_gen - 0.6f * r));
								if (y < o.y - 1e-4f) {
									const Vector3 zc = g.xforms[k].basis.get_column(2);
									const float yaw = Math::atan2(zc.x, zc.z);
									g.xforms[k] = Transform3D(Basis(Vector3(0, 1, 0), yaw) * Basis::from_scale(Vector3(s, s, s)), Vector3(o.x, y, o.z));
									g.multimesh->set_instance_transform(int(k), g.xforms[k]);
									disturb(k, ground_here - r, 0.75f);
									react_pressed++;
								}
								continue;
							}
							if (pick < press + 0.25f * (1.0f - press) && r >= 0.012f && int(loose.size()) < react_max_bodies && lib && g.variant < int(lib->hulls.size()) && !lib->hulls[g.variant].is_empty()) {
								// Promoted: a rigid body of its own hull, pushed on by the body (a share of its speed, out of the
								// footprint and a little up, as a tread or a sole flicks it).
								const PackedVector3Array &hull = lib->hulls[g.variant];
								Vector<Vector3> pts;
								pts.resize(hull.size());
								for (int q = 0; q < hull.size(); q++) {
									pts.write[q] = hull[q] * s;
								}
								Loose l;
								l.shape = ps->convex_polygon_shape_create();
								ps->shape_set_data(l.shape, pts);
								l.body = ps->body_create();
								ps->body_set_mode(l.body, PhysicsServer3D::BODY_MODE_RIGID);
								ps->body_add_shape(l.body, l.shape);
								ps->body_set_collision_layer(l.body, 0); // nothing collides into a pebble (a vehicle would shake)
								ps->body_set_collision_mask(l.body, collision_layer);
								const float vol = 4.18879f * r * r * r * 0.35f; // a pebble fills about a third of its sphere
								ps->body_set_param(l.body, PhysicsServer3D::BODY_PARAM_MASS, MAX(vol * 2650.0f, 0.002f));
								ps->body_set_param(l.body, PhysicsServer3D::BODY_PARAM_FRICTION, 0.8f);
								ps->body_set_param(l.body, PhysicsServer3D::BODY_PARAM_BOUNCE, 0.15f);
								ps->body_set_space(l.body, world->get_space());
								ps->body_set_state(l.body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(g.xforms[k].basis.orthonormalized(), o + Vector3(0, 0.25f * r, 0)));
								const float push = MAX(speed, 0.4f);
								const Vector2 h = hv * 0.6f + out * (0.5f * push);
								ps->body_set_state(l.body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, Vector3(h.x, 0.3f * push, h.y));
								ps->body_set_state(l.body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, Vector3(-out.y, 0, out.x) * (push / MAX(r, 0.01f)) * 0.5f);
								l.cell = key;
								l.group = gi;
								l.k = k;
								l.scale = s;
								l.reach = r;
								loose.push_back(l);
								react_promoted++;
								disturb(k, o.y - 10.0f);
							} else {
								// Squeezed out sideways to the footprint's edge (its place along the body's way kept), its height
								// above the ground kept, turned a little.
								Vector2 to = c2 + out * (foot + r * 1.1f);
								if (oval) {
									to = c2 + d * ((1.0f + 1.1f * r / MAX(foot_min, 1e-3f)) / MAX(edge, 1e-3f));
								} else if (speed > 1e-3f) {
									const Vector2 fw = hv / speed;
									const Vector2 lat(-fw.y, fw.x);
									const float along = d.dot(fw);
									const float across = d.dot(lat) >= 0.0f ? 1.0f : -1.0f;
									to = c2 + fw * along + lat * across * (Math::sqrt(MAX(foot * foot - along * along, 0.0f)) + r * 1.1f);
								}
								// A little further out for some (the rim is a band, not a line), lying on the stones already
								// there: higher by up to most of its size.
								const float extra = float((hs >> 16) & 0xffu) / 255.0f;
								to += (to - c2).normalized() * (extra * 2.0f * r);
								float y = o.y + (0.3f + 0.5f * (1.0f - extra)) * r;
								if (gen) {
									y += float(rf_height(gen, Vector3(to.x, 0, to.y)) - rf_height(gen, o));
								}
								if (dented) {
									// On top of the rim the ground itself pushed up (the stones cover it, the sand does not).
									const float h = wg->height_at(Vector3(to.x, 0, to.y));
									if (Math::is_finite(h)) {
										y = MAX(y, h + 0.45f * r);
									}
								}
								const float turn = (float(hash_murmur3_one_32(k * 2654435761u + uint32_t(react_shoved)) & 1023u) / 1023.0f - 0.5f) * 1.2f;
								g.xforms[k] = Transform3D(Basis(Vector3(0, 1, 0), turn) * g.xforms[k].basis, Vector3(to.x, y, to.y));
								g.multimesh->set_instance_transform(int(k), g.xforms[k]);
								disturb(k, y - 2.0f * r);
								react_shoved++;
							}
						}
					}
				}
			}
		}
	}
	react_last = seen;
}

// Frees loose stone p_index's body (its instance stays where the body left it).
void RockField3D::_free_loose(uint32_t p_index) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Loose &l = loose[p_index];
	if (l.body.is_valid()) {
		ps->free_rid(l.body);
	}
	if (l.shape.is_valid()) {
		ps->free_rid(l.shape);
	}
	loose.remove_at_unordered(p_index);
}

void RockField3D::_clear_react() {
	while (!loose.is_empty()) {
		_free_loose(loose.size() - 1);
	}
	react_last.clear();
}

void RockField3D::set_stones_react(bool p_enabled) {
	stones_react = p_enabled;
	if (!stones_react) {
		_clear_react();
	}
	if (is_inside_tree()) {
		_update_process();
	}
}

Dictionary RockField3D::get_react_stats() const {
	Dictionary d;
	d["loose"] = int(loose.size());
	d["promoted"] = int64_t(react_promoted);
	d["pressed"] = int64_t(react_pressed);
	d["shoved"] = int64_t(react_shoved);
	d["settled"] = int64_t(react_settled);
	return d;
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
		snow_dust.clear();
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
	// The coarse part of the fines (most of the mass: silt and sand) settles within a few metres of the hit, on snow
	// as dirt (dust on snow, Painter et al. 2007): black carbon equivalent at 0.013 of the dust's mass, as
	// GroundDust3D hands its own over. The fine part rides the Flow plume and is that system's to settle.
	// Gathered in 4 m cells and handed over once a second (a rolling piece sheds a little every tick).
	const Vector2i cell(int(Math::floor(p_position.x / 4.0f)), int(Math::floor(p_position.z / 4.0f)));
	SnowDust &sd = snow_dust[cell];
	sd.at += p_position * p_mass;
	sd.mass += p_mass;
	sd.radius = MAX(sd.radius, p_radius);
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
	const Node *dn = get_rockfall_dust_node();
	if (!dn || !dn->can_process()) {
		dust_events.clear(); // no Flow to take them (or it is paused): keep only the totals
	}
	dust_tick++;
	const float dt = float(p_delta);
	snow_dust_timer += dt;
	if (snow_dust_timer >= 1.0f && !snow_dust.is_empty()) {
		snow_dust_timer = 0.0f;
		if (WorldSnow *snow = WorldContracts::snow(); snow && snow->has_provider()) {
			for (const KeyValue<Vector2i, SnowDust> &kv : snow_dust) {
				const float r = 2.0f + 4.0f * kv.value.radius;
				snow->add_impurity(kv.value.at / MAX(kv.value.mass, 1e-12f), r, 0.7f * kv.value.mass * 1.0e6f * 0.013f / (float(Math::PI) * r * r));
			}
		}
		snow_dust.clear();
	}
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
				} else if (gen && v.length() > 0.8f && rf_height(gen, c) > c.y - r * 1.4f) {
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
		if (pedestal_material.is_valid()) {
			_apply_ground(pedestal_material);
		}
	}
	restore_broken();
	fractures.clear();
	obstacle_cache.clear();
	{
		MutexLock lock(rivers_lock);
		rivers_ready = false;
	}
	if (is_inside_tree() && (streaming || live_rockfall)) {
		_update_process();
	}
}

void RockField3D::clear() {
	_detail_cancel();
	_coarse_cancel();
	_far_cancel();
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
	_watch_stand();
	_watch_ground();
	int built = 0;
	_update_detail_radii();
	_prepare_rivers();
	_update_cover(true);
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
	// Stills thin as play does (the path-traced pits once seen with thinning here no longer reproduce, 22 Nov,
	// docs/ideas/rocks.md "Far rocks").
	_update_thinning(_target_position());
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

// Ledge slabs (docs/ideas/rocks.md, "Ledge slabs"): the hard caprock of each bench left standing out over the weaker
// bed under it (differential erosion), along the lips the terrain finds (TerrainGenerator3D.get_ledge_lips). One slab
// between two cross joints after another along each lip, its front at the lip's overhang, its back half under the
// bench (a cantilever holds only while as much of it is held as hangs out); now and then a gap where a block fell.
void RockField3D::_place_ledges(const TerrainGenerator3D *p_gen, Layer p_layer, const Rect2 &p_rect, uint32_t p_seed, LocalVector<Stone> &r_stones) {
	const real_t step = 2.0;
	LocalVector<TerrainGenerator3D::LedgeLip> lips;
	p_gen->get_ledge_lips(p_rect, step, lips);
	if (lips.is_empty()) {
		return;
	}
	RandomPCG rng(uint64_t(p_seed) * 0x9E3779B97F4A7C15ull + 0x1ed6eull); // its own: the cliff blocks' draws stay as they were
	// The drawn ground (as _rest): the heightfield plus the landscape's relief, which the lips' heights leave out.
	const WorldGround *wg = WorldContracts::ground();
	auto H = [&](real_t p_x, real_t p_z) -> real_t {
		real_t h = rf_height(p_gen, Vector3(p_x, 0, p_z));
		if (wg) {
			const Vector2 r = wg->relief_at(Vector3(p_x, h, p_z));
			if (r.y > 0.0f && Math::is_finite(r.x)) {
				h += r.x;
			}
		}
		return h;
	};
	// The lip points come in no order along the lips: each slab (or gap where one fell) claims the stretch of lip it
	// covers, and a point inside a claimed stretch places nothing.
	struct Claim {
		Vector3 c, x, z;
		real_t half_len, half_depth;
	};
	LocalVector<Claim> claims;
	auto claimed = [&](const Vector3 &p_at) {
		for (const Claim &cl : claims) {
			const Vector3 d = p_at - cl.c;
			if (Math::abs(d.dot(cl.x)) < cl.half_len && Math::abs(d.dot(cl.z)) < cl.half_depth && Math::abs(d.y) < 4.0f) {
				return true;
			}
		}
		return false;
	};
	for (uint32_t i = 0; i < lips.size(); i++) {
		const TerrainGenerator3D::LedgeLip &lp = lips[i];
		if (lp.overhang < 0.1f || lp.cap_thickness <= 0.0f || claimed(lp.position)) {
			continue;
		}
		if (int(r_stones.size()) >= max_stones_per_cell) {
			return;
		}
		Vector3 out0 = Vector3(lp.out.x, 0, lp.out.z);
		if (out0.length_squared() < 1e-6f) {
			continue;
		}
		out0.normalize();
		if (rng.randf() > lp.weight) {
			// A gap where a block fell: the lip shows the bench's own edge for a slab's length or so.
			const Vector3 x0(-out0.z, 0, out0.x);
			claims.push_back({ lp.position, x0, out0, real_t(lp.cap_thickness * (2.0f + 3.0f * rng.randf())), real_t(step) });
			continue;
		}
		const Vector3 pos = lp.position;
		const RockGenerator::RockType type = _type_at(p_gen, pos - Vector3(0, lp.cap_thickness * 0.5f, 0));
		const uint32_t key = _library_key(type, RockGenerator::FAMILY_LEDGE, 0);
		Library *lib = _ensure_library(key);
		if (!lib) {
			continue;
		}
		const Vector3 out = out0;
		// The variant: as thick as the cap, deep enough that at least as much lies under the bench as hangs out.
		Stone st;
		st.layer = p_layer;
		st.library = key;
		Vector3 centre, axes;
		Basis b;
		real_t top = 0.0;
		bool placed = false, overlap = false;
		for (int pass = 0; pass < 2 && !placed && !overlap; pass++) {
			real_t scale = 0.0, hang = 0.0;
			for (int t = 0; t < (pass == 0 ? 4 : 1); t++) {
				uint16_t v = uint16_t(rng.rand() % lib->meshes.size());
				if (pass == 1) {
					// The second try, where the bench bends or narrows under the first: the shortest joint block.
					for (int k = 0; k < int(lib->axes.size()); k++) {
						if (lib->axes[k].x < lib->axes[v].x) {
							v = uint16_t(k);
						}
					}
				}
				const Vector3 ax = lib->axes[v];
				const real_t sc = lp.cap_thickness * (0.92f + 0.16f * rng.randf()) / MAX(2.0f * ax.y, 0.02f);
				const real_t depth = 2.0 * ax.z * sc;
				const real_t h = MIN(real_t(lp.overhang), depth * 0.45);
				if (t == 0 || h > hang + 0.05) {
					st.variant = v;
					scale = sc;
					hang = h;
				}
				if (h >= lp.overhang - 0.05f) {
					break;
				}
			}
			axes = lib->axes[st.variant];
			st.scale = scale;
			// The slab follows the bench it caps: its top along the drawn bench behind the lip (the benches rise back a
			// share of a bed), its z out over the lip, x along it.
			const real_t back_d = MAX(real_t(1.0), axes.z * st.scale * 1.2);
			const real_t h_lip = H(pos.x, pos.z);
			const real_t h_back = H(pos.x - out.x * back_d, pos.z - out.z * back_d);
			const real_t rise = CLAMP(h_back - h_lip, real_t(-0.1) * back_d, real_t(0.5) * back_d);
			Vector3 z_axis = (out * back_d - Vector3(0, rise, 0)).normalized();
			Vector3 x_axis = Vector3(0, 1, 0).cross(z_axis).normalized();
			// And the bench's fall along the lip (the beds dip, and a lip on a dipping bed falls with it): measured a
			// little behind the lip, over the slab's length.
			{
				const real_t l = MAX(real_t(1.0), axes.x * st.scale);
				const Vector3 bk = pos - out * MIN(back_d * 0.5, real_t(axes.z * st.scale));
				const real_t fall = CLAMP((H(bk.x + x_axis.x * l, bk.z + x_axis.z * l) - H(bk.x - x_axis.x * l, bk.z - x_axis.z * l)) / (2.0 * l), real_t(-0.35), real_t(0.35));
				x_axis = (x_axis + Vector3(0, fall, 0)).normalized();
			}
			const Vector3 y_axis = z_axis.cross(x_axis).normalized();
			z_axis = x_axis.cross(y_axis).normalized();
			b = Basis();
			b.set_column(0, x_axis);
			b.set_column(1, y_axis);
			b.set_column(2, z_axis);
			b = Basis(y_axis, (rng.randf() - 0.5f) * 0.12f) * b; // the cross joints wander a little off square to the lip
			// The top a few centimetres under the drawn bench's edge (flush, never a shelf on it); the front at the
			// overhang.
			top = h_lip - lp.cap_thickness * 0.04;
			const Vector3 lip_top(pos.x, top, pos.z);
			// A real edge: in front of the lip, past the most a slab can hang, the ground has fallen a cap's thickness
			// (on a smooth face or where the lip's out runs along the slope there is no bench to cap).
			{
				const real_t ahead = MAX(real_t(1.0), hang + 0.5);
				if (H(pos.x + out.x * ahead, pos.z + out.z * ahead) > top - lp.cap_thickness) {
					break;
				}
			}
			// The back half must lie under the bench, and both ends of the slab on it: where the bench falls away behind
			// or beside (a spur, a gully's corner, a lip that bends) the slab would hang in the air. A slab that would
			// is drawn back from the lip, a third of its hang at a time, before it gives up.
			const real_t len = axes.x * st.scale * 0.9;
			// (From just behind the most a slab hangs out to near its back, across its length: the drawn bench stands
			// within a quarter of the cap under the slab's top, and the heightfield alone, which relief can sit over,
			// over its middle. Every quarter metre back: a riser behind can rise and fall within a metre.)
			const int depth_n = MAX(4, int(Math::ceil(0.8 * axes.z * st.scale / 0.25)));
			auto seats = [&](const Vector3 &p_centre) {
				for (int e = -2; e <= 2; e++) {
					for (int r = 0; r <= depth_n; r++) {
						const Vector3 mid = p_centre + x_axis * (len * 0.5 * e) - z_axis * (axes.z * st.scale * (0.1 + 0.8 * r / depth_n));
						const Vector3 q = mid + y_axis * (axes.y * st.scale);
						if (!(H(q.x, q.z) >= q.y - lp.cap_thickness * 0.25f && rf_height(p_gen, mid) >= mid.y)) {
							return false;
						}
					}
				}
				return true;
			};
			for (int k = 0; k < 3 && !placed; k++) {
				const real_t hk = hang * real_t(3 - k) / 3.0;
				if (k > 0 && hk < 0.1) {
					break;
				}
				// Slid along the lip to abut a neighbour where it would overlap one: a bed's joint blocks stand end to
				// end, a joint apart, so a lip carries a run of them rather than slabs spaced by the lip points' grid.
				const Vector3 c0 = lip_top - y_axis * (axes.y * st.scale) + z_axis * (hk - axes.z * st.scale);
				bool any_free = false;
				for (int sh = 0; sh < 9 && !placed; sh++) {
					const real_t off = (sh == 0 ? 0.0 : ((sh & 1) ? 1.0 : -1.0) * real_t((sh + 1) / 2) * 0.25) * len;
					centre = c0 + x_axis * off;
					if (claimed(centre + x_axis * len) || claimed(centre - x_axis * len) || claimed(centre)) {
						continue;
					}
					any_free = true;
					placed = seats(centre);
				}
				if (!any_free) {
					overlap = true;
					break; // no room between its neighbours
				}
			}
		}
		if (!placed) {
			continue;
		}
		st.basis = b * Basis().scaled(Vector3(st.scale, st.scale, st.scale));
		st.position = centre;
		st.ground = top - 50.0f; // no contact band: a bed of the bench, not a stone lying on it
		st.footprint = 0.0f;
		r_stones.push_back(st);
		// Its stretch of lip, give or take: joints open a little or close up.
		claims.push_back({ Vector3(centre.x, pos.y, centre.z), b.get_column(0), b.get_column(2), real_t(axes.x * st.scale * (0.9f + 0.1f * rng.randf())), real_t(axes.z * st.scale + step) });
	}
}

void RockField3D::_obstacle_place_job(void *p_job, uint32_t p_index) {
	CoarseJob &job = static_cast<CoarseJob *>(p_job)[p_index];
	const real_t cs = job.field->_cell_size(Layer(job.key.x));
	rf_job_generator = job.gen;
	rf_coarse_job = &job;
	rf_ledges_only = job.key.x == LAYER_CLIFF;
	job.field->_place(Layer(job.key.x), Rect2(job.key.y * cs, job.key.z * cs, cs, cs), rf_hash3(job.key.x, job.key.y, job.key.z, uint32_t(job.field->seed) * 2654435761u), job.stones);
	rf_ledges_only = false;
	rf_coarse_job = nullptr;
	rf_job_generator = nullptr;
}

static void rf_key_add(void *p_user, const void *p_data, size_t p_size) {
	static_cast<WorldCacheKey *>(p_user)->bytes(p_data, p_size);
}

bool RockField3D::_obstacle_cache_base(const TerrainGenerator3D *p_gen, String &r_base) const {
	const WorldGround *wg = WorldContracts::ground();
	if (!p_gen || (wg && wg->get_script_instance())) {
		return false; // a script stub answers relief_at: nothing to key it by
	}
	WorldCacheKey key("rocks_base");
	if (!key.object(this) || !key.object(p_gen)) {
		return false;
	}
	p_gen->hash_result([&](const void *p_data, size_t p_size) { key.bytes(p_data, p_size); });
	if (const LocalVector<WorldReliefProvider *> *rel = WorldContracts::relief_providers_list()) {
		key.value(uint64_t(rel->size()));
		for (const WorldReliefProvider *rp : *rel) {
			const Object *o = rp->contract_relief_object();
			if (!o || !key.object(o)) {
				return false;
			}
			rp->contract_relief_state(&rf_key_add, &key);
		}
	}
	r_base = key.finish();
	return true;
}

// A zone of obstacle cells in the world cache: 512 m square per layer.
static Vector3i rf_obstacle_zone(const Vector3i &p_cell, real_t p_cell_size) {
	const int per = MAX(1, int(Math::round(512.0 / p_cell_size)));
	auto fdiv = [per](int a) { return a >= 0 ? a / per : -((-a + per - 1) / per); };
	return Vector3i(p_cell.x, fdiv(p_cell.y), fdiv(p_cell.z));
}

PackedVector4Array RockField3D::get_obstacles(const Rect2 &p_rect, real_t p_min_size) {
	// The cells not cached yet: placed on the worker threads in batches (a whole-world query places thousands).
	LocalVector<Vector3i> missing;
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!layer_enabled[l] || (l == LAYER_CLIFF && !ledge_slabs) || _is_detail(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
		const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector3i key(l, x, z);
				if (!obstacle_cache.has(key)) {
					missing.push_back(key);
				}
			}
		}
	}
	const uint32_t BATCH = 512;
	TerrainGenerator3D *gen = _get_generator();
	// The world cache (world_cache.h): each 512 m zone's missing cells, loaded when the same cells were placed from
	// the same inputs before (the same bytes), else placed below and stored.
	String cache_base;
	HashMap<Vector3i, LocalVector<Vector3i>> zones; // zone: its missing cells, in order
	HashMap<Vector3i, String> zone_names;
	if (!missing.is_empty() && WorldCache::is_enabled() && _obstacle_cache_base(gen, cache_base)) {
		for (const Vector3i &k : missing) {
			zones[rf_obstacle_zone(k, _cell_size(Layer(k.x)))].push_back(k);
		}
		LocalVector<Vector3i> rest;
		for (const KeyValue<Vector3i, LocalVector<Vector3i>> &z : zones) {
			WorldCacheKey key("rocks");
			const CharString b = cache_base.utf8();
			key.bytes(b.get_data(), b.length());
			key.value(z.key);
			key.array(z.value.ptr(), z.value.size());
			const String name = key.finish();
			Vector<uint8_t> data;
			bool ok = WorldCache::load_any("rocks", name, data);
			LocalVector<ObstacleCell> got;
			if (ok) {
				// Per cell: count, then the obstacles, sizes and heights.
				const uint8_t *r = data.ptr();
				const uint8_t *end = r + data.size();
				for (uint32_t c = 0; c < z.value.size() && ok; c++) {
					uint32_t n = 0;
					if (end - r < int64_t(sizeof(n))) {
						ok = false;
						break;
					}
					memcpy(&n, r, sizeof(n));
					r += sizeof(n);
					const size_t need = size_t(n) * (sizeof(Vector4) + 2 * sizeof(float));
					if (size_t(end - r) < need) {
						ok = false;
						break;
					}
					ObstacleCell cell;
					cell.obstacles.resize(n);
					cell.sizes.resize(n);
					cell.heights.resize(n);
					memcpy(cell.obstacles.ptr(), r, sizeof(Vector4) * n);
					r += sizeof(Vector4) * n;
					memcpy(cell.sizes.ptr(), r, sizeof(float) * n);
					r += sizeof(float) * n;
					memcpy(cell.heights.ptr(), r, sizeof(float) * n);
					r += sizeof(float) * n;
					got.push_back(std::move(cell));
				}
				ok = ok && r == end;
			}
			if (ok) {
				for (uint32_t c = 0; c < z.value.size(); c++) {
					obstacle_cache.insert(z.value[c], std::move(got[c]));
				}
			} else {
				zone_names.insert(z.key, name);
				for (const Vector3i &k : z.value) {
					rest.push_back(k);
				}
			}
		}
		missing = std::move(rest);
	}
	if (!missing.is_empty()) {
		_prepare_rivers(); // read by the workers: built here, on the main thread, before any of them start
	}
	LocalVector<CoarseJob> placed;
	for (uint32_t b0 = 0; b0 < missing.size(); b0 += BATCH) {
		const uint32_t n = MIN(BATCH, missing.size() - b0);
		placed.clear();
		placed.resize(n);
		for (uint32_t i = 0; i < n; i++) {
			placed[i].field = this;
			placed[i].key = missing[b0 + i];
			placed[i].gen = gen;
		}
		if (gen && n > 1) {
			WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&RockField3D::_obstacle_place_job, placed.ptr(), int(n), -1, true, SNAME("RockField3D obstacles"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
		}
		for (uint32_t i = 0; i < n; i++) {
			CoarseJob &pj = placed[i];
			if (!gen || n == 1 || !pj.missing.is_empty()) {
				// Not placed on a worker, or it wanted a shape library not built yet: place it here, where the
				// library is built or asked for as before.
				pj.stones.clear();
				const real_t cs = _cell_size(Layer(pj.key.x));
				rf_ledges_only = pj.key.x == LAYER_CLIFF;
				_place(Layer(pj.key.x), Rect2(pj.key.y * cs, pj.key.z * cs, cs, cs), rf_hash3(pj.key.x, pj.key.y, pj.key.z, uint32_t(seed) * 2654435761u), pj.stones);
				rf_ledges_only = false;
			}
		}
		for (uint32_t i = 0; i < n; i++) {
			_apply_holes(placed[i].holes);
			ObstacleCell cell;
			for (const Stone &st : placed[i].stones) {
				if (key_family(st.library) == RockGenerator::FAMILY_LEDGE) {
					// A ledge slab: its plan (long along the lip) as a row of circles a depth apart, each as wide as
					// the slab is deep, at the bench's height; its top lies flush with the bench (height 0).
					const Library *lib = libraries.getptr(st.library);
					if (!lib || st.variant >= lib->axes.size()) {
						continue;
					}
					const Vector3 ax = lib->axes[st.variant];
					const Vector3 xa = st.basis.get_column(0).normalized(), ya = st.basis.get_column(1).normalized();
					const real_t half_len = ax.x * st.scale, half_depth = ax.z * st.scale;
					const Vector3 top_c = st.position + ya * (ax.y * st.scale);
					const int n_c = MAX(1, int(Math::ceil(half_len / half_depth)));
					for (int k = 0; k < n_c; k++) {
						const real_t t = n_c == 1 ? 0.0 : (real_t(k) / real_t(n_c - 1) * 2.0 - 1.0) * (half_len - half_depth);
						const Vector3 c = top_c + xa * t;
						cell.obstacles.push_back(Vector4(c.x, c.y, c.z, half_depth));
						cell.sizes.push_back(2.0f * half_depth);
						cell.heights.push_back(0.0f);
					}
					continue;
				}
				if (st.footprint > 0.0f) {
					cell.obstacles.push_back(Vector4(st.position.x, st.ground, st.position.z, st.footprint));
					cell.sizes.push_back(2.0f * st.scale);
					// Its top: the hull's support straight up when the shape is built (as get_stones), else
					// about 0.7 b (the c axis) over the origin, where the mesh's base sits.
					real_t top = st.position.y + 1.4f * st.scale;
					const Library *lib = libraries.getptr(st.library);
					if (lib && st.variant < lib->hulls.size() && !lib->hulls[st.variant].is_empty()) {
						const PackedVector3Array &hull = lib->hulls[st.variant];
						real_t best = -1e20;
						for (int h = 0; h < hull.size(); h++) {
							best = MAX(best, st.basis.rows[1].dot(hull[h]));
						}
						top = st.position.y + best;
					}
					cell.heights.push_back(MAX(float(top - st.ground), 0.0f));
				}
			}
			obstacle_cache.insert(missing[b0 + i], std::move(cell));
		}
	}
	for (const KeyValue<Vector3i, String> &zn : zone_names) {
		Vector<uint8_t> data;
		for (const Vector3i &k : zones[zn.key]) {
			const ObstacleCell &cell = obstacle_cache[k];
			const uint32_t n = cell.obstacles.size();
			const int64_t at = data.size();
			data.resize(at + sizeof(n) + size_t(n) * (sizeof(Vector4) + 2 * sizeof(float)));
			uint8_t *w = data.ptrw() + at;
			memcpy(w, &n, sizeof(n));
			w += sizeof(n);
			memcpy(w, cell.obstacles.ptr(), sizeof(Vector4) * n);
			w += sizeof(Vector4) * n;
			memcpy(w, cell.sizes.ptr(), sizeof(float) * n);
			w += sizeof(float) * n;
			memcpy(w, cell.heights.ptr(), sizeof(float) * n);
		}
		WorldCache::store("rocks", zn.value, data.ptr(), data.size());
	}
	PackedVector4Array out;
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!layer_enabled[l] || (l == LAYER_CLIFF && !ledge_slabs) || _is_detail(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
		const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const ObstacleCell *oc = obstacle_cache.getptr(Vector3i(l, x, z));
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

Array RockField3D::get_wind_obstacles(const Rect2 &p_rect) {
	Array out;
	if (!wind_obstacles || !_get_generator()) {
		return out; // no ground yet: nothing placed (and nothing cached empty)
	}
	const PackedVector4Array obs = get_obstacles(p_rect, 0.7);
	// get_obstacles fills the cache for the rect; read the heights back from it.
	for (int l = 0; l < LAYER_MAX; l++) {
		if (!layer_enabled[l] || (l == LAYER_CLIFF && !ledge_slabs) || _is_detail(l)) {
			continue;
		}
		const real_t cs = _cell_size(Layer(l));
		const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
		const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const ObstacleCell *oc = obstacle_cache.getptr(Vector3i(l, x, z));
				if (!oc) {
					continue;
				}
				for (uint32_t k = 0; k < oc->obstacles.size(); k++) {
					const Vector4 &o = oc->obstacles[k];
					const float h = k < oc->heights.size() ? oc->heights[k] : 0.0f;
					if (h < 0.5f || !p_rect.has_point(Vector2(o.x, o.z))) {
						continue;
					}
					// The footprint's square, a little narrower than its disc (the same area).
					const real_t r = o.w * 0.886;
					out.push_back(AABB(Vector3(o.x - r, o.y, o.z - r), Vector3(2.0 * r, h, 2.0 * r)));
				}
			}
		}
	}
	(void)obs;
	return out;
}

void RockField3D::set_wind_obstacles(bool p_enabled) {
	wind_obstacles = p_enabled;
	if (p_enabled) {
		add_to_group(SNAME("weather_shelter"));
	} else if (is_in_group(SNAME("weather_shelter"))) {
		remove_from_group(SNAME("weather_shelter"));
	}
}

Array RockField3D::get_ledge_slabs(const Rect2 &p_rect) {
	Array out;
	TerrainGenerator3D *gen = _get_generator();
	if (!gen || !ledge_slabs || !layer_enabled[LAYER_CLIFF]) {
		return out;
	}
	struct Slab {
		Vector3 top, along, out;
		real_t half_len = 0, half_depth = 0, thick = 0;
		Dictionary d;
	};
	LocalVector<Slab> slabs;
	const real_t cs = _cell_size(LAYER_CLIFF);
	const int x0 = int(Math::floor(p_rect.position.x / cs)), x1 = int(Math::floor(p_rect.get_end().x / cs));
	const int z0 = int(Math::floor(p_rect.position.y / cs)), z1 = int(Math::floor(p_rect.get_end().y / cs));
	_prepare_rivers();
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			// The cell's slabs as the cell draws them (the same seed); its blocks are skipped as for get_obstacles.
			LocalVector<Stone> stones;
			rf_ledges_only = true;
			_place(LAYER_CLIFF, Rect2(x * cs, z * cs, cs, cs), rf_hash3(LAYER_CLIFF, x, z, uint32_t(seed) * 2654435761u), stones);
			rf_ledges_only = false;
			for (const Stone &st : stones) {
				if (key_family(st.library) != RockGenerator::FAMILY_LEDGE || !p_rect.has_point(Vector2(st.position.x, st.position.z))) {
					continue;
				}
				const Library *lib = libraries.getptr(st.library);
				if (!lib || st.variant >= lib->axes.size()) {
					continue;
				}
				const Vector3 ax = lib->axes[st.variant];
				Slab s;
				s.along = st.basis.get_column(0).normalized();
				const Vector3 ya = st.basis.get_column(1).normalized();
				const Vector3 za = st.basis.get_column(2).normalized();
				s.out = Vector3(za.x, 0, za.z).normalized();
				s.half_len = ax.x * st.scale;
				s.half_depth = ax.z * st.scale;
				s.thick = 2.0 * ax.y * st.scale;
				s.top = st.position + ya * (ax.y * st.scale);
				// How far it hangs past the bench's edge: back from its front until the heightfield stands under its
				// base, a tenth of a metre at a time.
				const Vector3 front = s.top + za * s.half_depth;
				const Vector3 base_front = front - ya * s.thick;
				real_t hang = 0;
				for (real_t t = 0; t < 2.0 * s.half_depth; t += 0.1) {
					const Vector3 q = base_front - za * t;
					if (rf_height(gen, q) >= q.y - 0.05) {
						break;
					}
					hang = t + 0.1;
				}
				s.d["overhang"] = MIN(hang, real_t(2.0 * s.half_depth));
				s.d["position"] = st.position;
				s.d["basis"] = st.basis;
				s.d["top"] = s.top;
				s.d["front"] = front;
				s.d["along"] = s.along;
				s.d["out"] = s.out;
				s.d["length"] = 2.0 * s.half_len;
				s.d["depth"] = 2.0 * s.half_depth;
				s.d["thickness"] = s.thick;
				s.d["rock_type"] = int(key_type(st.library));
				s.d["variant"] = int(st.variant);
				slabs.push_back(s);
			}
		}
	}
	// Runs: slabs end to end along one lip (ends within a slab's depth, facing within 35 degrees, tops on one bed),
	// joined by union-find.
	LocalVector<uint32_t> parent;
	parent.resize(slabs.size());
	for (uint32_t i = 0; i < slabs.size(); i++) {
		parent[i] = i;
	}
	auto find = [&](uint32_t a) {
		while (parent[a] != a) {
			parent[a] = parent[parent[a]];
			a = parent[a];
		}
		return a;
	};
	for (uint32_t i = 0; i < slabs.size(); i++) {
		for (uint32_t j = i + 1; j < slabs.size(); j++) {
			const Slab &a = slabs[i], &b = slabs[j];
			const Vector3 d = Vector3(b.top.x - a.top.x, 0, b.top.z - a.top.z);
			// (Along a dipping bed the tops fall with the dip: up to 0.4 m a metre.)
			if (a.out.dot(b.out) < 0.82 || Math::abs(a.top.y - b.top.y) > 0.5 + 0.4 * d.length()) {
				continue;
			}
			const real_t gap = Math::abs(d.dot(a.along)) - a.half_len - b.half_len;
			const real_t side = Math::abs(d.dot(a.out));
			if (gap < MAX(a.half_depth, b.half_depth) * 2.0 + 1.0 && side < MAX(a.half_depth, b.half_depth) * 1.5) {
				parent[find(i)] = find(j);
			}
		}
	}
	HashMap<uint32_t, int> run_id;
	LocalVector<int> run_count;
	LocalVector<real_t> run_len;
	for (uint32_t i = 0; i < slabs.size(); i++) {
		const uint32_t r = find(i);
		if (!run_id.has(r)) {
			run_id[r] = int(run_count.size());
			run_count.push_back(0);
			run_len.push_back(0);
		}
		run_count[run_id[r]]++;
		run_len[run_id[r]] += 2.0 * slabs[i].half_len;
	}
	for (uint32_t i = 0; i < slabs.size(); i++) {
		const int r = run_id[find(i)];
		Dictionary d = slabs[i].d;
		d["run"] = r;
		d["run_slabs"] = run_count[r];
		d["run_length"] = run_len[r];
		out.push_back(d);
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
			const Rect2 crect(x * cs, z * cs, cs, cs);
			// The detail classes read what their jobs read (worn ground, frost sorting), so these are the stones the
			// cells draw.
			StandWear wear;
			FrostClimate frost;
			RainClimate rain;
			if (_is_detail(Layer(p_layer))) {
				_sample_wear(crect.grow(1.0), wear);
				_frost_climate(crect, frost);
				_rain_climate(crect, rain);
				rf_job_wear = &wear;
				rf_job_frost = &frost;
				rf_job_rain = &rain;
			}
			_place(Layer(p_layer), crect, rf_hash3(p_layer, x, z, uint32_t(seed) * 2654435761u), stones);
			rf_job_wear = nullptr;
			rf_job_frost = nullptr;
			rf_job_rain = nullptr;
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
				d["pedestal"] = st.pedestal;
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
// texture (the texture's detail averages out over the band; not for an detail_only layer, whose mean is its colour),
// as linear albedo. Without a ground material, the
// terrain material's own defaults.
void RockField3D::_apply_ground(const Ref<ShaderMaterial> &p_material) const {
	_sync_scree_blocks();
	static const char *names[5] = { "rock", "scree", "soil", "alluvium", "sand" };
	static const Color defaults[5] = { Color(0.36, 0.35, 0.33), Color(0.41, 0.40, 0.37), Color(0.2, 0.22, 0.1), Color(0.28, 0.27, 0.16), Color(0.7, 0.62, 0.46) };
	p_material->set_shader_parameter("ground_blend", ground_blend);
	p_material->set_shader_parameter("mineral_glints", mineral_glints);
	// The stone scans (load_scan_set); not on the ground detail grains (a scan's metre-scale detail on a 2 cm grain
	// is a flat tint).
	const bool scans = stone_scans[0].is_valid() && !bool(p_material->get_shader_parameter("small_linear"));
	p_material->set_shader_parameter("stone_scan", scans);
	if (scans) {
		static const char *sfx[2] = { "a", "b" };
		for (int i = 0; i < 2; i++) {
			const RockScan &sc = stone_scans[i];
			p_material->set_shader_parameter(StringName(String("stone_scan_albedo_") + sfx[i]), sc.albedo);
			p_material->set_shader_parameter(StringName(String("stone_scan_mean_") + sfx[i]), sc.mean);
			p_material->set_shader_parameter(StringName(String("stone_scan_scale_") + sfx[i]), 1.0f / sc.tile);
			p_material->set_shader_parameter(StringName(String("stone_scan_normal_") + sfx[i]), sc.normal);
			p_material->set_shader_parameter(StringName(String("stone_scan_normal_strength_") + sfx[i]), sc.normal.is_valid() ? sc.normal_strength : 0.0f);
		}
	}
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
			// A layer with detail_only divides the texture by its own mean (tm_<layer>_albedo_mean), so its average
			// is the layer colour already.
			const Variant am = ground->get_shader_parameter(StringName(String("tm_") + names[i] + "_albedo_mean"));
			const bool detail_only = am.get_type() == Variant::VECTOR3 && Vector3(am) != Vector3(1, 1, 1);
			if (tex.is_valid() && !detail_only) {
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
			if (i == 0) {
				// The rock layer's scan for the wall's own blocks (cliff_match): the same texture at the same tile
				// size in world space, so a ledge or a course carries the face's grain, divided by the mean the
				// ground colour above already holds (the layer's albedo_mean for a detail_only layer).
				// The face scan where the ground has one (the cliff overlay, rock faces past about 45 degrees: what the
				// wall round a ledge or a course actually shows), else the rock layer's.
				Ref<Texture2D> stex = tex;
				Ref<Texture2D> ntex = ground->get_shader_parameter("tm_rock_normal_tex");
				Variant sc = ground->get_shader_parameter("tm_rock_scale");
				Variant ns = ground->get_shader_parameter("tm_rock_normal");
				Variant smean = am;
				bool s_detail_only = detail_only;
				const Ref<Texture2D> ctex = ground->get_shader_parameter("tm_ov_cliff_albedo_tex");
				if (ctex.is_valid()) {
					stex = ctex;
					ntex = ground->get_shader_parameter("tm_ov_cliff_normal_tex");
					sc = ground->get_shader_parameter("tm_ov_cliff_scale");
					ns = ground->get_shader_parameter("tm_ov_cliff_normal");
					smean = ground->get_shader_parameter("tm_ov_cliff_albedo_mean");
					s_detail_only = smean.get_type() == Variant::VECTOR3 && Vector3(smean) != Vector3(1, 1, 1);
				}
				// The divisor that makes ground_rock * scan / divisor the face's own colour: the face draws its layer
				// colour times the scan (over its albedo_mean when detail_only), and ground_rock already holds the rock
				// layer texture's mean when that layer is not detail_only.
				auto linear_mean = [](const Ref<Texture2D> &p_tex) {
					Vector3 m(1, 1, 1);
					Ref<Image> img = p_tex.is_valid() ? p_tex->get_image() : Ref<Image>();
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
							m = Vector3(sum.r, sum.g, sum.b) / 64.0f;
						}
					}
					return m;
				};
				const Vector3 face_div = s_detail_only ? Vector3(smean) : Vector3(1, 1, 1);
				const Vector3 rock_mean = (!detail_only && tex.is_valid()) ? linear_mean(tex) : Vector3(1, 1, 1);
				const Vector3 lin_mean = face_div * rock_mean;
				p_material->set_shader_parameter("cliff_scan", stex.is_valid());
				if (stex.is_valid()) {
					p_material->set_shader_parameter("cliff_scan_albedo", stex);
					p_material->set_shader_parameter("cliff_scan_mean", lin_mean.max(Vector3(0.001, 0.001, 0.001)));
				}
				p_material->set_shader_parameter("cliff_scan_has_normal", ntex.is_valid());
				if (ntex.is_valid()) {
					p_material->set_shader_parameter("cliff_scan_normal", ntex);
				}
				if (sc.get_type() == Variant::FLOAT) {
					p_material->set_shader_parameter("cliff_scan_scale", sc);
				}
				if (ns.get_type() == Variant::FLOAT) {
					p_material->set_shader_parameter("cliff_scan_normal_strength", ns);
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

// Scanned detail on the stones (docs/ideas/scan-textures.md): the art folder's scans.cfg sections [stone] and
// [stone_variant] (system="rocks"), half the stones taking each, triplanar in the stone's own frame at the scan's real
// size, divided by the scan's mean so the rock type keeps its colour. Returns how many loaded.
int RockField3D::load_scan_set(const String &p_dir) {
	scan_dir = p_dir;
	int n = 0;
	n += rock_scan_load(p_dir, "stone", stone_scans[0]) ? 1 : 0;
	n += rock_scan_load(p_dir, "stone_variant", stone_scans[1]) ? 1 : 0;
	if (n == 0) {
		WARN_PRINT(vformat("RockField3D: no [stone] scans in '%s' (fetch the art first): the stones keep their procedural surface only.", p_dir.path_join("scans.cfg")));
	} else if (!stone_scans[1].is_valid()) {
		stone_scans[1] = stone_scans[0];
	} else if (!stone_scans[0].is_valid()) {
		stone_scans[0] = stone_scans[1];
	}
	for (KeyValue<int, Ref<ShaderMaterial>> &kv : materials) {
		_apply_ground(kv.value);
	}
	return n;
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
	if (pedestal_material.is_valid()) {
		_apply_ground(pedestal_material);
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
			up = Vector2(rf_height(gen, Vector3(o.x + e, 0, o.z)) - rf_height(gen, Vector3(o.x - e, 0, o.z)), rf_height(gen, Vector3(o.x, 0, o.z + e)) - rf_height(gen, Vector3(o.x, 0, o.z - e))) / (2.0 * e);
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
				const real_t span = Math::lerp(real_t(ring * 0.6), real_t(bank), real_t(u * uphill_w));
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

// The frost-sorted ground for the terrain material (terrain_frost_map, RG8: r the gate, g the stone share; over
// terrain_frost_rect, FROST_MAP_SIZE round the target): the same gate and share the frost stones are placed by, so
// the material can shade the fine centres (bare, wet silt) between the stone borders. The gate on a 2 m grid (it
// follows climate, soil and slope), the share per texel where the gate is open.
void RockField3D::_frost_map_job(void *p_job) {
	FrostMapJob *job = static_cast<FrostMapJob *>(p_job);
	const int R = FROST_MAP_RES;
	const real_t texel = FROST_MAP_SIZE / real_t(R);
	const int G = int(FROST_MAP_SIZE / 2.0) + 1;
	LocalVector<float> gate, slope;
	gate.resize(G * G);
	slope.resize(G * G);
	bool open = false;
	for (int z = 0; z < G; z++) {
		for (int x = 0; x < G; x++) {
			Vector3 n;
			const float g = job->field->_frost_gate(job->gen, job->fc, Vector3(job->origin.x + x * 2.0, 0.0, job->origin.y + z * 2.0), n);
			gate[z * G + x] = g;
			slope[z * G + x] = Math::rad_to_deg(Math::acos(CLAMP(float(n.y), -1.0f, 1.0f)));
			open = open || g > 0.0f;
		}
	}
	job->data.resize(R * R * 4);
	uint8_t *w = job->data.ptrw();
	memset(w, 0, job->data.size());
	if (!open) {
		return;
	}
	RFFrostFalls falls;
	LocalVector<float> fine;
	fine.resize(R * R);
	memset(fine.ptr(), 0, sizeof(float) * R * R);
	for (int z = 0; z < R; z++) {
		const real_t gz = ((z + 0.5) * texel) / 2.0;
		const int iz = MIN(int(gz), G - 2);
		const float fz = float(gz - iz);
		for (int x = 0; x < R; x++) {
			const real_t gx = ((x + 0.5) * texel) / 2.0;
			const int ix = MIN(int(gx), G - 2);
			const float fx = float(gx - ix);
			const float g = Math::lerp(Math::lerp(gate[iz * G + ix], gate[iz * G + ix + 1], fx), Math::lerp(gate[(iz + 1) * G + ix], gate[(iz + 1) * G + ix + 1], fx), fz);
			if (g <= 0.004f) {
				continue;
			}
			const float sl = Math::lerp(Math::lerp(slope[iz * G + ix], slope[iz * G + ix + 1], fx), Math::lerp(slope[(iz + 1) * G + ix], slope[(iz + 1) * G + ix + 1], fx), fz);
			const float share = rf_frost_share(job->gen, float(job->origin.x + (x + 0.5) * texel), float(job->origin.y + (z + 0.5) * texel), sl, &falls);
			w[(z * R + x) * 4] = uint8_t(CLAMP(g * 255.0f, 0.0f, 255.0f));
			w[(z * R + x) * 4 + 1] = uint8_t(CLAMP(share * 255.0f, 0.0f, 255.0f));
			fine[z * R + x] = g * (1.0f - share);
			job->any = true;
		}
	}
	if (!job->any) {
		return;
	}
	// b: the fine centres' dome. Frost heave lifts each fine domain some centimetres over its stony border, highest in
	// the middle (Washburn 1956; Hallet and Prestrud 1986: 5 to 30 cm), and the landscape's metre mesh cannot draw a
	// dome a metre or two across, so the material bends its normal by it. The fines' share times its own tent blur over
	// about 2.75 m (two box passes of 11 texels each way), square-rooted: wide domains stand highest, edges ease in.
	LocalVector<float> blur, tmp;
	blur.resize(R * R);
	memcpy(blur.ptr(), fine.ptr(), sizeof(float) * R * R);
	tmp.resize(R * R);
	const int K = 5;
	for (int pass = 0; pass < 2; pass++) {
		for (int z = 0; z < R; z++) {
			const float *row = blur.ptr() + z * R;
			float a = 0.0f;
			for (int d = -K; d <= K; d++) {
				a += row[CLAMP(d, 0, R - 1)];
			}
			for (int x = 0; x < R; x++) {
				tmp[z * R + x] = a / float(2 * K + 1);
				a += row[MIN(x + K + 1, R - 1)] - row[MAX(x - K, 0)];
			}
		}
		for (int x = 0; x < R; x++) {
			float a = 0.0f;
			for (int d = -K; d <= K; d++) {
				a += tmp[CLAMP(d, 0, R - 1) * R + x];
			}
			for (int z = 0; z < R; z++) {
				blur[z * R + x] = a / float(2 * K + 1);
				a += tmp[MIN(z + K + 1, R - 1) * R + x] - tmp[MAX(z - K, 0) * R + x];
			}
		}
	}
	for (int i = 0; i < R * R; i++) {
		w[i * 4 + 2] = uint8_t(CLAMP(Math::sqrt(MAX(fine[i] * blur[i], 0.0f)) * 255.0f, 0.0f, 255.0f));
	}
}

void RockField3D::_update_frost_map() {
	_finish_frost_map(false);
	if (frost_map_job || !is_inside_tree()) {
		return;
	}
	const TerrainGenerator3D *gen = _get_generator();
	if (!frost_sorting || !gen) {
		_clear_frost_map();
		return;
	}
	const Vector3 t = _target_position();
	const real_t step = FROST_MAP_SIZE / 8.0;
	const Vector2 origin = Vector2(Math::round(t.x / step) * step, Math::round(t.z / step) * step) - Vector2(FROST_MAP_SIZE, FROST_MAP_SIZE) * 0.5;
	if (origin == frost_map_origin) {
		return;
	}
	FrostClimate fc;
	_frost_climate(Rect2(origin, Vector2(FROST_MAP_SIZE, FROST_MAP_SIZE)), fc);
	frost_map_origin = origin;
	if (!fc.on) {
		_clear_frost_map();
		frost_map_origin = origin;
		return;
	}
	FrostMapJob *job = memnew(FrostMapJob);
	job->field = this;
	job->gen = gen;
	job->fc = fc;
	job->origin = origin;
	frost_map_job = job;
	frost_map_task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_frost_map_job, job, false, "RockField3D frost map");
}

void RockField3D::_finish_frost_map(bool p_wait) {
	if (!frost_map_job) {
		return;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(frost_map_task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(frost_map_task);
	FrostMapJob *job = frost_map_job;
	frost_map_job = nullptr;
	frost_map_task = WorkerThreadPool::INVALID_TASK_ID;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (is_inside_tree() && frost_sorting && job->any) {
		const int R = FROST_MAP_RES;
		Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_RGBA8, job->data);
		if (frost_map_texture.is_valid() && frost_map_texture->get_width() == R) {
			frost_map_texture->update(img);
		} else {
			frost_map_texture = ImageTexture::create_from_image(img);
		}
		rs->global_shader_parameter_set("terrain_frost_map", frost_map_texture->get_rid());
		rs->global_shader_parameter_set("terrain_frost_rect", Vector4(job->origin.x, job->origin.y, FROST_MAP_SIZE, FROST_MAP_SIZE));
		frost_map_published = true;
	} else if (frost_map_published) {
		rs->global_shader_parameter_set("terrain_frost_rect", Vector4());
		frost_map_published = false;
	}
	memdelete(job);
}

void RockField3D::_clear_frost_map() {
	_finish_frost_map(true);
	if (frost_map_published) {
		RenderingServer::get_singleton()->global_shader_parameter_set("terrain_frost_rect", Vector4());
		frost_map_published = false;
	}
	frost_map_origin = Vector2(1e30, 1e30);
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

// The far hand-over (docs/ideas/ground-detail.md, "Seamless far edge"). Each detail class is drawn out to its radius
// and no further; past it the ground showed plain sand where the grains stopped, a line across a beach. The ground
// material draws their aggregate look there instead (Meng et al. 2015: explicit grains near, their aggregate far),
// from these maps: per texel each class's cover of the shore, from the same size law the cells place by, and the rock
// type for the grains' colour. Two levels round the camera, 1 m and 8 m texels, rebuilt on a worker when it has moved
// an eighth of a map.
void RockField3D::_cover_fill(CoverJob *p_job) {
	const TerrainGenerator3D *gen = p_job->gen;
	const int n = COVER_N;
	p_job->data.resize(n * n * 4);
	uint8_t *w = p_job->data.ptrw();
	memset(w, 0, n * n * 4);
	static const Vector2 probes[9] = { Vector2(0, 0), Vector2(4, 0), Vector2(-4, 0), Vector2(0, 4), Vector2(0, -4), Vector2(10, 0), Vector2(-10, 0), Vector2(0, 10), Vector2(0, -10) };
	const float sig = 0.6f * float(Math::SQRT2);
	RFFrostFalls frost_falls;
	// The river beds first, each segment over the texels it reaches (the one deeper inside wins).
	LocalVector<RiverBed> beds;
	if (!p_job->river_segs.is_empty()) {
		beds.resize(n * n);
		for (const RiverSeg &sg : p_job->river_segs) {
			const Rect2 r = Rect2(sg.a, Vector2()).expand(sg.a + sg.f * sg.len).grow(0.6f * MAX(sg.w, p_job->texel) + p_job->texel);
			const int i0 = MAX(int((r.position.x - p_job->origin.x) / p_job->texel), 0), i1 = MIN(int((r.get_end().x - p_job->origin.x) / p_job->texel), n - 1);
			const int j0 = MAX(int((r.position.y - p_job->origin.y) / p_job->texel), 0), j1 = MIN(int((r.get_end().y - p_job->origin.y) / p_job->texel), n - 1);
			for (int j = j0; j <= j1; j++) {
				for (int i = i0; i <= i1; i++) {
					RiverBed bed;
					if (_river_bed(sg, p_job->origin + Vector2(i + 0.5f, j + 0.5f) * p_job->texel, bed, p_job->texel) && bed.band > beds[j * n + i].band) {
						beds[j * n + i] = bed;
					}
				}
			}
		}
	}
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const Vector3 c(p_job->origin.x + (i + 0.5f) * p_job->texel, 0, p_job->origin.y + (j + 0.5f) * p_job->texel);
			// As the detail cells (_build_detail): the shore's band above the water line and its median size from
			// the beach slope, the classes' shares of a poorly sorted tail.
			float cover[3] = {};
			bool any = false;
			if (p_job->shore) {
				real_t level = -1e30;
				for (const Vector2 &o : probes) {
					const real_t l = gen->get_water_level(c + Vector3(o.x, 0, o.y));
					if (!Math::is_nan(l)) {
						level = MAX(level, l);
					}
				}
				const float e = level > -1e29 ? float(rf_height(gen, c) - level) : 1e9f;
				if (e >= -0.6f && e <= 2.5f) {
					const Vector3 nn = ground_normal(gen, c.x, c.z, MAX(1.0f, 0.5f * p_job->texel));
					const float tanb = Math::sqrt(MAX(1.0f / (nn.y * nn.y) - 1.0f, 0.0f));
					const float rock = float(gen->get_rock_exposure(c));
					float d50 = 0.002f * Math::exp(MIN(tanb / 0.045f, 20.0f));
					d50 = MIN(MAX(d50, 0.5f * Math::smoothstep(0.2f, 0.7f, rock)), 1.2f);
					const float band = Math::smoothstep(-0.6f, 0.0f, e) * (1.0f - Math::smoothstep(1.6f, 2.5f, e));
					for (int k = 0; k < 3; k++) {
						const float d1 = p_job->sizes[k][0], d2 = p_job->sizes[k][1];
						if (d2 > d1 * 1.01f) {
							const float share = 0.5f * (std::erfc(Math::log(d1 / d50) / sig) - std::erfc(Math::log(d2 / d50) / sig));
							cover[k] += 0.85f * band * share;
							any = true;
						}
					}
				}
			}
			if (!beds.is_empty() && beds[j * n + i].band > 0.0f) {
				// The river bed's log-normal, as the detail cells (and the pools' drape of fines over it).
				const RiverBed &bed = beds[j * n + i];
				for (int k = 0; k < 3; k++) {
					const float d1 = p_job->sizes[k][0], d2 = p_job->sizes[k][1];
					if (d2 > d1 * 1.01f) {
						const float share = 0.5f * (std::erfc(Math::log(d1 / bed.d50) / sig) - std::erfc(Math::log(d2 / bed.d50) / sig));
						cover[k] += 0.85f * bed.band * share * (1.0f - 0.75f * bed.silt);
						any = true;
					}
				}
			}
			if (p_job->scree) {
				// And the talus's power law below the scree layer's smallest (the grains shed onto a beach under a cliff).
				const float t = (float(gen->get_talus(c)) - 0.2f) / 0.8f;
				if (t >= 0.05f) {
					const float dmin = p_job->scree_dmin;
					const float n_min = 0.3f * t / (float(Math::PI) * 1.25f * dmin * dmin);
					for (int k = 0; k < 3; k++) {
						const float d1 = p_job->scree_sizes[k][0], d2 = p_job->scree_sizes[k][1];
						if (d2 > d1 * 1.01f) {
							cover[k] += n_min * (Math::pow(d1 / dmin, -2.5f) - Math::pow(d2 / dmin, -2.5f)) * 0.785f * d1 * d2;
							any = true;
						}
					}
				}
			}
			if (p_job->frost.any) {
				// Frost-sorted ground (as the detail cells): the other sources thin out of the fine centres and the
				// borders' stones are added, by the pattern's mean stone share over the texel.
				Vector3 fnrm;
				const float fg = p_job->field->_frost_gate(gen, p_job->frost, c, fnrm);
				if (fg > 0.0f) {
					const float sl = Math::rad_to_deg(Math::acos(CLAMP(float(fnrm.y), -1.0f, 1.0f)));
					float share = 0.0f;
					for (int q = 0; q < 4; q++) {
						const float ox = ((q & 1) ? 0.25f : -0.25f) * p_job->texel, oz = ((q >> 1) ? 0.25f : -0.25f) * p_job->texel;
						share += 0.25f * rf_frost_share(gen, float(c.x + ox), float(c.z + oz), sl, &frost_falls);
					}
					const float keep = 1.0f - fg * (1.0f - share);
					for (int k = 0; k < 3; k++) {
						cover[k] *= keep;
						if (p_job->sizes[k][1] > p_job->sizes[k][0] * 1.01f) {
							cover[k] += MIN(rf_frost_cover[k] * fg, 0.45f) * share;
							any = true;
						}
					}
				}
			}
			if (p_job->rain.any) {
				// Stones on bare soil under rain (as the detail cells; their pedestals are below the far texels).
				float depth;
				const float soil = p_job->field->_rain_soil(gen, p_job->rain, c, depth);
				if (soil > 0.0f) {
					static const float soil_cover[3] = { 0.10f, 0.06f, 0.03f };
					for (int k = 0; k < 3; k++) {
						if (p_job->sizes[k][1] > p_job->sizes[k][0] * 1.01f) {
							cover[k] += MIN(soil_cover[k] * soil, 0.45f);
							any = true;
						}
					}
				}
			}
			if (!any) {
				continue;
			}
			uint8_t *px = w + (j * n + i) * 4;
			for (int k = 0; k < 3; k++) {
				px[k] = uint8_t(CLAMP(int(MIN(cover[k], 0.45f) / 0.5f * 255.0f + 0.5f), 0, 255));
			}
			px[3] = uint8_t(CLAMP(int(p_job->field->_type_at(gen, c)), 0, 7) * 32);
		}
	}
}

void RockField3D::_cover_task(void *p_job) {
	_cover_fill(static_cast<CoverJob *>(p_job));
}

void RockField3D::_cover_finish(bool p_wait) {
	if (!cover_job) {
		return;
	}
	if (cover_job->task != WorkerThreadPool::INVALID_TASK_ID) {
		if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(cover_job->task)) {
			return;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(cover_job->task);
	}
	const int l = cover_job->level;
	const Ref<Image> img = Image::create_from_data(COVER_N, COVER_N, false, Image::FORMAT_RGBA8, cover_job->data);
	if (cover_tex[l].is_valid() && cover_tex[l]->get_width() == COVER_N) {
		cover_tex[l]->update(img);
	} else {
		cover_tex[l] = ImageTexture::create_from_image(img);
	}
	{
		MutexLock lock(cover_data_mutex);
		cover_data[l] = cover_job->data;
		cover_origin[l] = cover_job->origin;
		cover_valid[l] = true;
	}
	memdelete(cover_job);
	cover_job = nullptr;
	_push_cover();
}

// One level at a time: the near map first, then the far one, each when the camera has moved an eighth of it from
// its middle (or the classes' sizes changed).
void RockField3D::_update_cover(bool p_wait) {
	_cover_finish(p_wait);
	if (cover_job) {
		return;
	}
	TerrainGenerator3D *gen = _get_generator();
	if (!ground_cover || !ground_detail || !(layer_enabled[LAYER_SHORE] || layer_enabled[LAYER_SCREE]) || !gen) {
		return;
	}
	const Vector3 dc = _detail_center();
	static const float texel[2] = { 1.0f, 8.0f };
	for (int l = 0; l < 2; l++) {
		const float span = COVER_N * texel[l];
		const Vector2 mid = cover_origin[l] + Vector2(span, span) * 0.5f;
		if (cover_valid[l] && mid.distance_to(Vector2(dc.x, dc.z)) < span / 8.0f) {
			continue;
		}
		CoverJob *job = memnew(CoverJob);
		job->field = this;
		job->gen = gen;
		job->level = l;
		job->texel = texel[l];
		// Snapped to whole texels, so a rebuilt map lines up with the last one.
		job->origin = Vector2(Math::floor(dc.x / texel[l]) * texel[l] - span * 0.5f, Math::floor(dc.z / texel[l]) * texel[l] - span * 0.5f);
		job->shore = layer_enabled[LAYER_SHORE];
		job->scree = layer_enabled[LAYER_SCREE];
		job->scree_dmin = MAX(float(min_size), 0.15f);
		job->river = detail_rivers && layer_enabled[LAYER_RIVER];
		if (job->river) {
			_prepare_rivers();
			_river_segments(Rect2(job->origin, Vector2(span, span)), job->river_segs);
		}
		for (int k = 0; k < 3; k++) {
			_detail_sizes(Layer(LAYER_DETAIL_PEBBLE + k), min_size, job->sizes[k][0], job->sizes[k][1]);
			_detail_sizes(Layer(LAYER_DETAIL_PEBBLE + k), job->scree_dmin, job->scree_sizes[k][0], job->scree_sizes[k][1]);
		}
		_frost_climate(Rect2(job->origin, Vector2(span, span)), job->frost);
		_rain_climate(Rect2(job->origin, Vector2(span, span)), job->rain);
		cover_job = job;
		if (p_wait || !detail_threads) {
			_cover_fill(job);
			_cover_finish(true);
			continue;
		}
		job->task = WorkerThreadPool::get_singleton()->add_native_task(&RockField3D::_cover_task, job, false, SNAME("RockField3D cover"));
		return;
	}
}

// The maps, where each class's real stones stop and the band they hand over in, their sizes and the rock types'
// colours, to the ground material (a TerrainMaterial3D; nothing for any other).
void RockField3D::_push_cover() {
	const Ref<ShaderMaterial> g = _ground_source();
	if (g.is_null() || !g->has_method(SNAME("set_ground_cover"))) {
		return;
	}
	if (!ground_cover || !ground_detail || !cover_valid[0] || !cover_valid[1]) {
		g->call(SNAME("set_ground_cover"), Variant(), Variant(), Rect2(), Rect2(), Vector3(), Vector3(), Vector3(), PackedColorArray());
		return;
	}
	const float span[2] = { COVER_N * 1.0f, COVER_N * 8.0f };
	Vector3 reach, band, size;
	for (int k = 0; k < 3; k++) {
		const int l = LAYER_DETAIL_PEBBLE + k;
		float d1, d2;
		_detail_sizes(Layer(l), min_size, d1, d2);
		reach[k] = layer_enabled[l] ? layer_radius[l] : 0.0f;
		band[k] = _cell_size(Layer(l)); // cells reach past the radius by up to a cell, raggedly
		size[k] = Math::sqrt(d1 * MAX(d2, d1));
	}
	PackedColorArray colors;
	for (int t = 0; t < 8; t++) {
		const Ref<ShaderMaterial> m = t < RockGenerator::ROCK_MAX ? RockGenerator::get_material(RockGenerator::RockType(t)) : Ref<ShaderMaterial>();
		Color c(0.35, 0.33, 0.3);
		if (m.is_valid()) {
			// As the stones' shader mixes it (linear albedo): the mineral and its specks, under the weathered rind.
			const Vector3 base = m->get_shader_parameter("base_color");
			const Vector3 speck = m->get_shader_parameter("speck_color");
			const Vector3 rind = m->get_shader_parameter("rind_color");
			const float share = m->get_shader_parameter("speck_share");
			const Vector3 v = base.lerp(speck, CLAMP(share, 0.0f, 1.0f) * 0.85f).lerp(rind, 0.55f);
			c = Color(v.x, v.y, v.z);
		}
		colors.push_back(c);
	}
	g->call(SNAME("set_ground_cover"), cover_tex[0], cover_tex[1], Rect2(cover_origin[0], Vector2(span[0], span[0])), Rect2(cover_origin[1], Vector2(span[1], span[1])), reach, band, size, colors);
}

void RockField3D::_clear_cover() {
	if (cover_job) {
		if (cover_job->task != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(cover_job->task);
		}
		memdelete(cover_job);
		cover_job = nullptr;
	}
	{
		MutexLock lock(cover_data_mutex);
		cover_valid[0] = cover_valid[1] = false;
		cover_data[0].clear();
		cover_data[1].clear();
	}
	_push_cover();
}

Vector3 RockField3D::_cover_sample(const Vector2 &p_xz) const {
	static const float texel[2] = { 1.0f, 8.0f };
	for (int l = 0; l < 2; l++) {
		if (!cover_valid[l] || cover_data[l].size() != COVER_N * COVER_N * 4) {
			continue;
		}
		// Texel centres sit at half a texel; stay a texel inside so the bilinear taps are all in the map.
		const float fx = (p_xz.x - cover_origin[l].x) / texel[l] - 0.5f;
		const float fz = (p_xz.y - cover_origin[l].y) / texel[l] - 0.5f;
		if (fx < 0.0f || fz < 0.0f || fx >= COVER_N - 1 || fz >= COVER_N - 1) {
			continue;
		}
		const int x = int(fx), z = int(fz);
		const float tx = fx - x, tz = fz - z;
		const uint8_t *d = cover_data[l].ptr();
		Vector3 r;
		for (int k = 0; k < 3; k++) {
			const float a = d[(z * COVER_N + x) * 4 + k], b = d[(z * COVER_N + x + 1) * 4 + k];
			const float c = d[((z + 1) * COVER_N + x) * 4 + k], e = d[((z + 1) * COVER_N + x + 1) * 4 + k];
			r[k] = Math::lerp(Math::lerp(a, b, tx), Math::lerp(c, e, tx), tz) * (0.5f / 255.0f);
		}
		return r;
	}
	return Vector3();
}

// A stone's record for WorldGround.stones_near: x, z, ground, top (its shape's bottom and twice its half height over the
// origin), its footprint's half axes x and z and its yaw, when its top stands a centimetre or more over its ground.
void RockField3D::_standing_append(const Stone &p_stone, const Library *p_lib, LocalVector<float> &r_out) {
	if (!p_lib || p_stone.variant >= p_lib->axes.size() || p_stone.ground < p_stone.position.y - 30.0) {
		return;
	}
	const Vector3 ax = p_lib->axes[p_stone.variant];
	const float top = float(p_stone.position.y) + (p_lib->bottom[p_stone.variant] + 2.0f * ax.y) * p_stone.scale;
	if (top - p_stone.ground < 0.01f) {
		return;
	}
	const Vector3 bx = p_stone.basis.get_column(0);
	const uint32_t o = r_out.size();
	r_out.resize(o + 7);
	r_out[o + 0] = float(p_stone.position.x);
	r_out[o + 1] = float(p_stone.position.z);
	r_out[o + 2] = p_stone.ground;
	r_out[o + 3] = top;
	r_out[o + 4] = p_stone.scale * ax.x;
	r_out[o + 5] = p_stone.scale * ax.z;
	r_out[o + 6] = float(Math::atan2(-bx.z, bx.x)); // (Basis(Y, yaw)'s x axis is (cos, 0, -sin))
}

// WorldGround.stones_near: the laid cells' standing stones in the rect (main thread).
void RockField3D::contract_stones_near(const Rect2 &p_rect, LocalVector<float> &r_out) const {
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		const real_t ks = _cell_size(Layer(kv.key.x));
		if (kv.value.standing.is_empty() || !Rect2(kv.key.y * ks, kv.key.z * ks, ks, ks).grow(4.0).intersects(p_rect)) {
			continue;
		}
		const LocalVector<float> &sv = kv.value.standing;
		for (uint32_t i = 0; i + 6 < sv.size(); i += 7) {
			if (p_rect.has_point(Vector2(sv[i], sv[i + 1]))) {
				const uint32_t o = r_out.size();
				r_out.resize(o + 7);
				memcpy(r_out.ptr() + o, sv.ptr() + i, sizeof(float) * 7);
			}
		}
	}
}

Vector3 RockField3D::contract_cover_at(const Vector3 &p_position) const {
	MutexLock lock(cover_data_mutex);
	return _cover_sample(Vector2(p_position.x, p_position.z));
}

// A grid of reads (sand's armour): the lock once for all of them.
void RockField3D::contract_cover_points(const Vector2 *p_points, int p_count, Vector3 *r_cover) const {
	MutexLock lock(cover_data_mutex);
	for (int i = 0; i < p_count; i++) {
		r_cover[i] = _cover_sample(p_points[i]);
	}
}

// Off: the ground detail ignores the game trails and the boar rooting WorldStand publishes (cells already built are
// built again).
void RockField3D::set_ground_wear(bool p_enabled) {
	if (ground_wear == p_enabled) {
		return;
	}
	ground_wear = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

// Off: the river beds keep only the river layer's stones (their grains and their far cover go; cells already built
// are built again).
void RockField3D::set_ground_detail_rivers(bool p_enabled) {
	if (detail_rivers == p_enabled) {
		return;
	}
	detail_rivers = p_enabled;
	_detail_cancel();
	_clear_cover();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

// Off: every grain its own instance (cells already built are built again).
void RockField3D::set_ground_detail_tiles(bool p_enabled) {
	if (ground_detail_tiles == p_enabled) {
		return;
	}
	ground_detail_tiles = p_enabled;
	_detail_cancel();
	LocalVector<Vector3i> drop;
	for (KeyValue<Vector3i, Cell> &kv : cells) {
		if (_is_detail(kv.key.x)) {
			_free_cell(kv.value);
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		cells.erase(k);
	}
}

void RockField3D::set_ledge_slabs(bool p_enabled) {
	if (ledge_slabs == p_enabled) {
		return;
	}
	ledge_slabs = p_enabled;
	if (is_inside_tree()) {
		rebuild();
	}
}

void RockField3D::set_rt_instanced_cells(bool p_enabled) {
	if (rt_instanced_cells == p_enabled) {
		return;
	}
	rt_instanced_cells = p_enabled;
	if (is_inside_tree()) {
		rebuild();
	}
}

// What the cell stones cost the path tracer: groups at or under the merged BLAS cap bake a copy of each stone's
// triangles (unless rt_instanced_cells), larger ones and instanced ones share their shape's BLAS.
Dictionary RockField3D::get_rt_stats() const {
	static const int64_t cap = int64_t(GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles"));
	int64_t merged_tris = 0, merged_groups = 0, merged_stones = 0, inst_stones = 0, inst_groups = 0, all_tris = 0;
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		for (const Group &g : kv.value.groups) {
			if (g.multimesh.is_null()) {
				continue;
			}
			const Ref<Mesh> m = g.multimesh->get_mesh();
			if (m.is_null() || m->get_surface_count() == 0) {
				continue;
			}
			const int64_t n = g.multimesh->get_instance_count();
			const int64_t t = n * int64_t(m->surface_get_array_index_len(0) / 3);
			all_tris += t;
			if (!RS::get_singleton()->multimesh_is_rt_instanced(g.multimesh->get_rid()) && t <= cap) {
				merged_tris += t;
				merged_groups++;
				merged_stones += n;
			} else {
				inst_groups++;
				inst_stones += n;
			}
		}
	}
	Dictionary d;
	d["merged_triangles"] = merged_tris;
	d["merged_groups"] = merged_groups;
	d["merged_stones"] = merged_stones;
	d["instanced_groups"] = inst_groups;
	d["instanced_stones"] = inst_stones;
	d["cell_triangles"] = all_tris; // every cell stone's triangles, as drawn
	return d;
}

Dictionary RockField3D::get_ground_detail_tile_stats() const {
	Dictionary d;
	d["tiles"] = int64_t(tile_stats[0]);
	d["placements"] = int64_t(tile_stats[1]);
	d["grains"] = int64_t(tile_stats[2]);
	int live = 0, grains = 0, instances = 0;
	int64_t triangles = 0, most = 0;
	for (const KeyValue<Vector3i, Cell> &kv : cells) {
		for (const TilePlacement &tp : kv.value.tiles) {
			live += tp.instances.is_empty() ? 0 : 1;
			grains += tp.instances.is_empty() ? 0 : int(tp.members.size());
			instances += int(tp.instances.size());
		}
		for (const Ref<MultiMesh> &mm : kv.value.tile_meshes) {
			const Ref<Mesh> m = mm->get_mesh();
			if (m.is_valid() && m->get_surface_count() > 0) {
				const int64_t t = int64_t(mm->get_instance_count()) * (m->surface_get_array_index_len(0) / 3);
				triangles += t;
				most = MAX(most, t);
			}
		}
	}
	int64_t shared_built = 0, shared_cands = 0, shared_triangles = 0;
	for (const KeyValue<Vector3i, TileSet *> &kv : tile_sets) {
		for (const TileCut &cut : kv.value->cuts) {
			shared_cands++;
			if (cut.mms.is_empty()) {
				continue;
			}
			shared_built++;
			for (const Ref<MultiMesh> &mm : cut.mms) {
				const Ref<Mesh> m = mm.is_valid() ? mm->get_mesh() : Ref<Mesh>();
				if (m.is_valid() && m->get_surface_count() > 0) {
					const int64_t t = int64_t(mm->get_instance_count()) * (m->surface_get_array_index_len(0) / 3);
					shared_triangles += t;
					most = MAX(most, t);
				}
			}
		}
	}
	triangles += shared_triangles;
	Array by_layer; // per detail class (pebble, gravel, fine): sets, candidates, built, triangles
	for (int l = LAYER_DETAIL_PEBBLE; l <= LAYER_DETAIL_FINE; l++) {
		int64_t n[4] = {};
		for (const KeyValue<Vector3i, TileSet *> &kv : tile_sets) {
			if (kv.key.x != l) {
				continue;
			}
			n[0]++;
			for (const TileCut &cut : kv.value->cuts) {
				n[1]++;
				n[2] += cut.mms.is_empty() ? 0 : 1;
				for (const Ref<MultiMesh> &mm : cut.mms) {
					const Ref<Mesh> m = mm.is_valid() ? mm->get_mesh() : Ref<Mesh>();
					if (m.is_valid() && m->get_surface_count() > 0) {
						n[3] += int64_t(mm->get_instance_count()) * (m->surface_get_array_index_len(0) / 3);
					}
				}
			}
		}
		Array a;
		for (int64_t v : n) {
			a.push_back(v);
		}
		by_layer.push_back(a);
	}
	d["shared_by_class"] = by_layer;
	d["shared_sets"] = int64_t(tile_sets.size());
	{
		int64_t widened = 0, thinned = 0;
		double widen_sum = 0.0;
		for (const KeyValue<Vector3i, Cell> &kv : cells) {
			for (const Group &g : kv.value.groups) {
				if (g.multimesh.is_valid() && g.multimesh->get_visible_instance_count() >= 0) {
					thinned++;
				}
				if (g.widen > 1.0f) {
					widened++;
					widen_sum += g.widen;
				}
			}
		}
		d["far_thinned_groups"] = thinned;
		d["far_widened_groups"] = widened;
		d["far_widen_mean"] = widened > 0 ? widen_sum / double(widened) : 1.0;
	}
	d["shared_candidates"] = shared_cands;
	d["shared_tiles"] = shared_built; // candidates some placement took (their MultiMeshes and BLAS built)
	d["shared_triangles"] = shared_triangles;
	d["live_placements"] = live;
	d["live_grains"] = grains;
	d["instances"] = instances; // RenderingServer instances (one per shape of a placement)
	d["tile_triangles"] = triangles; // baked into the tiles' BLAS
	d["largest_shape_triangles"] = most;
	d["squares"] = int64_t(tile_skips[4].get());
	d["skip_sparse"] = int64_t(tile_skips[0].get());
	d["skip_coarse"] = int64_t(tile_skips[1].get());
	d["skip_bent"] = int64_t(tile_skips[2].get());
	d["skip_unmatched"] = int64_t(tile_skips[3].get());
	d["frost_sorted"] = int64_t(frost_stones.get());
	d["runoff_lag"] = int64_t(runoff_stones[0].get());
	d["runoff_fan"] = int64_t(runoff_stones[1].get());
	d["pedestals"] = int64_t(pedestal_stones[0].get());
	d["pedestals_toppled"] = int64_t(pedestal_stones[1].get());
	d["pedestal_columns"] = int64_t(pedestal_stones[2].get());
	d["snow_buried"] = int64_t(snow_buried.get());
	d["wear_armour"] = int64_t(wear_stones[0].get());
	d["wear_shoulder"] = int64_t(wear_stones[1].get());
	d["wear_rooting"] = int64_t(wear_stones[2].get());
	return d;
}

void RockField3D::set_ground_detail_cover(bool p_enabled) {
	ground_cover = p_enabled;
	if (!ground_cover) {
		_clear_cover();
	}
}

Rect2 RockField3D::get_ground_detail_cover_rect(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, 2, Rect2());
	MutexLock lock(cover_data_mutex);
	if (!cover_valid[p_level]) {
		return Rect2();
	}
	const float span = COVER_N * (p_level == 0 ? 1.0f : 8.0f);
	return Rect2(cover_origin[p_level], Vector2(span, span));
}

Ref<Texture2D> RockField3D::get_ground_detail_cover_texture(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, 2, Ref<Texture2D>());
	return cover_tex[p_level];
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
		_clear_cover();
		LocalVector<Vector3i> drop;
		for (KeyValue<Vector3i, Cell> &kv : cells) {
			if (_is_detail(kv.key.x)) {
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
		case NOTIFICATION_ENTER_TREE: {
			WorldContracts::register_cover(this);
			// Before any node is ready, so the weather's first shelter bake finds the boulders.
			if (wind_obstacles) {
				add_to_group(SNAME("weather_shelter"));
			}
			if (WorldContracts *wc = WorldContracts::get_singleton()) {
				const Callable cb = callable_mp(this, &RockField3D::_on_exclusions_changed);
				if (!wc->is_connected(SNAME("exclusions_changed"), cb)) {
					wc->connect(SNAME("exclusions_changed"), cb);
				}
			}
			exclusions.read(WorldExclusion3D::KIND_ROCKS);
		} break;
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
			if (Engine::get_singleton()->is_editor_hint()) {
				break; // editor preview: the stones only, no rockfall, contact, frost or snow
			}
			if (live_rockfall) {
				_update_rockfall(get_process_delta_time());
			}
			if (ground_contact) {
				_update_contact(false);
			}
			_update_frost_map();
			_update_snow_cells(get_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (rockfall_dust) {
				_update_dust(get_physics_process_delta_time());
			}
			_update_react(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			const bool vis = is_visible_in_tree();
			for (KeyValue<Vector3i, Cell> &kv : cells) {
				for (TilePlacement &tp : kv.value.tiles) {
					for (const RID &r : tp.instances) {
						RS::get_singleton()->instance_set_visible(r, vis);
					}
				}
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (WorldContracts *wc = WorldContracts::get_singleton()) {
				const Callable cb = callable_mp(this, &RockField3D::_on_exclusions_changed);
				if (wc->is_connected(SNAME("exclusions_changed"), cb)) {
					wc->disconnect(SNAME("exclusions_changed"), cb);
				}
			}
			WorldContracts::unregister_cover(this);
			_clear_react();
			_clear_cover();
			clear();
			_clear_contact();
			_clear_frost_map();
			dust_tracks.clear();
			dust_events.clear();
		} break;
	}
}

void RockField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_coarse_cover_at", "position"), &RockField3D::get_coarse_cover_at);
	ClassDB::bind_method(D_METHOD("set_far_thinning", "enabled"), &RockField3D::set_far_thinning);
	ClassDB::bind_method(D_METHOD("set_ledge_slabs", "enabled"), &RockField3D::set_ledge_slabs);
	ClassDB::bind_method(D_METHOD("is_ledge_slabs"), &RockField3D::is_ledge_slabs);
	ClassDB::bind_method(D_METHOD("set_rt_instanced_cells", "enabled"), &RockField3D::set_rt_instanced_cells);
	ClassDB::bind_method(D_METHOD("is_rt_instanced_cells"), &RockField3D::is_rt_instanced_cells);
	ClassDB::bind_method(D_METHOD("get_rt_stats"), &RockField3D::get_rt_stats);
	ClassDB::bind_method(D_METHOD("set_far_widen", "enabled"), &RockField3D::set_far_widen);
	ClassDB::bind_method(D_METHOD("is_far_widen"), &RockField3D::is_far_widen);
	ClassDB::bind_method(D_METHOD("get_far_cover_texture"), &RockField3D::get_far_cover_texture);
	ClassDB::bind_method(D_METHOD("set_far_radius", "radius"), &RockField3D::set_far_radius);
	ClassDB::bind_method(D_METHOD("get_far_radius"), &RockField3D::get_far_radius);
	ClassDB::bind_method(D_METHOD("set_far_min_size", "size"), &RockField3D::set_far_min_size);
	ClassDB::bind_method(D_METHOD("get_far_min_size"), &RockField3D::get_far_min_size);
	ClassDB::bind_method(D_METHOD("get_far_stone_count"), &RockField3D::get_far_stone_count);
	ClassDB::bind_method(D_METHOD("build_far"), &RockField3D::build_far);
	ClassDB::bind_method(D_METHOD("is_far_thinning"), &RockField3D::is_far_thinning);
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
	ClassDB::bind_method(D_METHOD("load_scan_set", "dir"), &RockField3D::load_scan_set);
	ClassDB::bind_method(D_METHOD("set_cliff_block_max_slope", "degrees"), &RockField3D::set_cliff_block_max_slope);
	ClassDB::bind_method(D_METHOD("get_cliff_block_max_slope"), &RockField3D::get_cliff_block_max_slope);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cliff_block_max_slope", PROPERTY_HINT_RANGE, "50,89,0.5,suffix:°"), "set_cliff_block_max_slope", "get_cliff_block_max_slope");
	ClassDB::bind_method(D_METHOD("get_scan_dir"), &RockField3D::get_scan_dir);
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
	ClassDB::bind_method(D_METHOD("set_editor_preview", "enable"), &RockField3D::set_editor_preview);
	ClassDB::bind_method(D_METHOD("is_editor_preview"), &RockField3D::is_editor_preview);
	ClassDB::bind_method(D_METHOD("get_rebuild_button"), &RockField3D::get_rebuild_button);
	ClassDB::bind_method(D_METHOD("build_area", "rect"), &RockField3D::build_area);
	ClassDB::bind_method(D_METHOD("clear"), &RockField3D::clear);
	ClassDB::bind_method(D_METHOD("get_obstacles", "rect", "min_size"), &RockField3D::get_obstacles, DEFVAL(0.25));
	ClassDB::bind_method(D_METHOD("get_wind_obstacles", "rect"), &RockField3D::get_wind_obstacles);
	ClassDB::bind_method(D_METHOD("set_wind_obstacles", "enabled"), &RockField3D::set_wind_obstacles);
	ClassDB::bind_method(D_METHOD("is_wind_obstacles_enabled"), &RockField3D::is_wind_obstacles_enabled);
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
	ClassDB::bind_method(D_METHOD("set_ground_detail_rivers", "enabled"), &RockField3D::set_ground_detail_rivers);
	ClassDB::bind_method(D_METHOD("set_stones_react", "enabled"), &RockField3D::set_stones_react);
	ClassDB::bind_method(D_METHOD("is_stones_react"), &RockField3D::is_stones_react);
	ClassDB::bind_method(D_METHOD("set_react_max_bodies", "count"), &RockField3D::set_react_max_bodies);
	ClassDB::bind_method(D_METHOD("get_react_max_bodies"), &RockField3D::get_react_max_bodies);
	ClassDB::bind_method(D_METHOD("get_loose_count"), &RockField3D::get_loose_count);
	ClassDB::bind_method(D_METHOD("get_react_stats"), &RockField3D::get_react_stats);
	ClassDB::bind_method(D_METHOD("is_ground_detail_rivers"), &RockField3D::is_ground_detail_rivers);
	ClassDB::bind_method(D_METHOD("set_ground_wear", "enabled"), &RockField3D::set_ground_wear);
	ClassDB::bind_method(D_METHOD("set_runoff_gravel", "enabled"), &RockField3D::set_runoff_gravel);
	ClassDB::bind_method(D_METHOD("is_runoff_gravel"), &RockField3D::is_runoff_gravel);
	ClassDB::bind_method(D_METHOD("set_snow_burial", "enabled"), &RockField3D::set_snow_burial);
	ClassDB::bind_method(D_METHOD("is_snow_burial"), &RockField3D::is_snow_burial);
	ClassDB::bind_method(D_METHOD("set_rain_pedestals", "enabled"), &RockField3D::set_rain_pedestals);
	ClassDB::bind_method(D_METHOD("is_rain_pedestals"), &RockField3D::is_rain_pedestals);
	ClassDB::bind_method(D_METHOD("get_pedestal_at", "position"), &RockField3D::get_pedestal_at);
	ClassDB::bind_method(D_METHOD("get_pedestal_spots", "rect", "step", "max_count"), &RockField3D::get_pedestal_spots, DEFVAL(2.0), DEFVAL(64));
	ClassDB::bind_method(D_METHOD("set_frost_sorting", "enabled"), &RockField3D::set_frost_sorting);
	ClassDB::bind_method(D_METHOD("is_frost_sorting"), &RockField3D::is_frost_sorting);
	ClassDB::bind_method(D_METHOD("get_frost_at", "position"), &RockField3D::get_frost_at);
	ClassDB::bind_method(D_METHOD("is_ground_wear"), &RockField3D::is_ground_wear);
	ClassDB::bind_method(D_METHOD("set_ground_detail_tiles", "enabled"), &RockField3D::set_ground_detail_tiles);
	ClassDB::bind_method(D_METHOD("is_ground_detail_tiles"), &RockField3D::is_ground_detail_tiles);
	ClassDB::bind_method(D_METHOD("get_ground_detail_tile_stats"), &RockField3D::get_ground_detail_tile_stats);
	ClassDB::bind_method(D_METHOD("set_ground_detail_cover", "enabled"), &RockField3D::set_ground_detail_cover);
	ClassDB::bind_method(D_METHOD("is_ground_detail_cover"), &RockField3D::is_ground_detail_cover);
	ClassDB::bind_method(D_METHOD("get_ground_detail_cover_texture", "level"), &RockField3D::get_ground_detail_cover_texture);
	ClassDB::bind_method(D_METHOD("get_ground_detail_cover_rect", "level"), &RockField3D::get_ground_detail_cover_rect);
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
	ClassDB::bind_method(D_METHOD("get_ledge_slabs", "rect"), &RockField3D::get_ledge_slabs);
	ClassDB::bind_method(D_METHOD("get_stone_caps", "layer", "rect", "tolerance"), &RockField3D::get_stone_caps, DEFVAL(0.03));
	ClassDB::bind_method(D_METHOD("get_stone_count"), &RockField3D::get_stone_count);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &RockField3D::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &RockField3D::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_library_msec"), &RockField3D::get_last_library_msec);
	ClassDB::bind_method(D_METHOD("get_library_triangle_count"), &RockField3D::get_library_triangle_count);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview"), "set_editor_preview", "is_editor_preview");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "rebuild_button", PROPERTY_HINT_TOOL_BUTTON, "Rebuild,Reload", PROPERTY_USAGE_EDITOR), "", "get_rebuild_button");
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_obstacles"), "set_wind_obstacles", "is_wind_obstacles_enabled");
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

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_thinning"), "set_far_thinning", "is_far_thinning");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_widen"), "set_far_widen", "is_far_widen");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rt_instanced_cells"), "set_rt_instanced_cells", "is_rt_instanced_cells");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ledge_slabs"), "set_ledge_slabs", "is_ledge_slabs");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_radius", PROPERTY_HINT_RANGE, "0,8000,10,suffix:m"), "set_far_radius", "get_far_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_min_size", PROPERTY_HINT_RANGE, "0.5,10,0.1,suffix:m"), "set_far_min_size", "get_far_min_size");
	ADD_GROUP("Ground Detail", "ground_detail_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail"), "set_ground_detail", "is_ground_detail");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_detail_pixels", PROPERTY_HINT_RANGE, "0.5,16,0.1,suffix:px"), "set_ground_detail_pixels", "get_ground_detail_pixels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_detail_reference_height", PROPERTY_HINT_RANGE, "0,4320,1,suffix:px"), "set_ground_detail_reference_height", "get_ground_detail_reference_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_detail_max_radius", PROPERTY_HINT_RANGE, "1,200,0.5,suffix:m"), "set_ground_detail_max_radius", "get_ground_detail_max_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_detail_max_per_cell", PROPERTY_HINT_RANGE, "1,1000000,1"), "set_ground_detail_max_per_cell", "get_ground_detail_max_per_cell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_threads"), "set_cell_threads", "is_cell_threads");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail_threads"), "set_ground_detail_threads", "is_ground_detail_threads");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail_cover"), "set_ground_detail_cover", "is_ground_detail_cover");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail_rivers"), "set_ground_detail_rivers", "is_ground_detail_rivers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_wear"), "set_ground_wear", "is_ground_wear");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "frost_sorting"), "set_frost_sorting", "is_frost_sorting");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "runoff_gravel"), "set_runoff_gravel", "is_runoff_gravel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_pedestals"), "set_rain_pedestals", "is_rain_pedestals");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snow_burial"), "set_snow_burial", "is_snow_burial");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_detail_tiles"), "set_ground_detail_tiles", "is_ground_detail_tiles");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stones_react"), "set_stones_react", "is_stones_react");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "react_max_bodies", PROPERTY_HINT_RANGE, "0,256,1"), "set_react_max_bodies", "get_react_max_bodies");
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

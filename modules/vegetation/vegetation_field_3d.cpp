/**************************************************************************/
/*  vegetation_field_3d.cpp                                               */
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

#include "vegetation_field_3d.h"

#include "fairy_rings.h"

#include "grass_field_3d.h"
#include "plant_generator.h"
#include "vegetation_detail.h"
#include "vegetation_light.h"

// Rigged trees (TreeWind3D, from the trees work) when that class is in the module.
#include "tree_bark_detail.h"
#include "tree_generator.h"
#if __has_include("tree_wind_3d.h")
#include "tree_wind_3d.h"
#define VEGETATION_HAS_TREE_WIND
#endif

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/rw_lock.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/weather/weather_sky_material.h"
#include "modules/world/world_contracts.h"

namespace {

float wrap_delta(float p_d, float p_size) {
	p_d = Math::abs(p_d);
	return MIN(p_d, p_size - p_d);
}

// Bridson (2007) Poisson-disk sampling on a torus of side p_size: no two points closer than
// p_r, also across the wrap, so the tile repeats without seams.
void poisson_torus(float p_size, float p_r, RandomPCG &p_rng, LocalVector<Vector2> &r_points) {
	const float cell = p_r / Math::SQRT2;
	const int n = MAX(1, int(Math::floor(p_size / cell)));
	const float cs = p_size / n; // Cells tile the torus exactly; cs <= r / sqrt(2) holds.
	LocalVector<int> grid;
	grid.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		grid[i] = -1;
	}
	LocalVector<int> active;
	auto cell_of = [&](const Vector2 &p) -> Vector2i {
		return Vector2i(CLAMP(int(p.x / cs), 0, n - 1), CLAMP(int(p.y / cs), 0, n - 1));
	};
	auto fits = [&](const Vector2 &p) -> bool {
		const Vector2i c = cell_of(p);
		const int reach = int(Math::ceil(p_r / cs));
		for (int dz = -reach; dz <= reach; dz++) {
			for (int dx = -reach; dx <= reach; dx++) {
				const int gx = (c.x + dx + n * 4) % n;
				const int gz = (c.y + dz + n * 4) % n;
				const int idx = grid[gz * n + gx];
				if (idx < 0) {
					continue;
				}
				const Vector2 q = r_points[idx];
				const float ddx = wrap_delta(q.x - p.x, p_size);
				const float ddz = wrap_delta(q.y - p.y, p_size);
				if (ddx * ddx + ddz * ddz < p_r * p_r) {
					return false;
				}
			}
		}
		return true;
	};
	auto add = [&](const Vector2 &p) {
		const Vector2i c = cell_of(p);
		grid[c.y * n + c.x] = r_points.size();
		active.push_back(r_points.size());
		r_points.push_back(p);
	};
	add(Vector2(p_rng.randf() * p_size, p_rng.randf() * p_size));
	const int k = 30;
	while (!active.is_empty()) {
		const uint32_t ai = p_rng.rand(active.size());
		const Vector2 base = r_points[active[ai]];
		bool found = false;
		for (int t = 0; t < k; t++) {
			const float ang = p_rng.randf() * Math::TAU;
			const float rad = p_r * (1.0f + p_rng.randf());
			Vector2 p = base + Vector2(Math::cos(ang), Math::sin(ang)) * rad;
			p.x = Math::fposmod(float(p.x), p_size);
			p.y = Math::fposmod(float(p.y), p_size);
			if (grid[cell_of(p).y * n + cell_of(p).x] >= 0) {
				continue;
			}
			if (fits(p)) {
				add(p);
				found = true;
				break;
			}
		}
		if (!found) {
			active[ai] = active[active.size() - 1];
			active.resize(active.size() - 1);
		}
	}
}

uint32_t tile_hash(int p_x, int p_z, int p_seed, int p_species) {
	uint32_t h = hash_murmur3_one_32(uint32_t(p_x));
	h = hash_murmur3_one_32(uint32_t(p_z), h);
	h = hash_murmur3_one_32(uint32_t(p_seed), h);
	h = hash_murmur3_one_32(uint32_t(p_species), h);
	return hash_fmix32(h);
}

} // namespace

const VegetationField3D::Pattern &VegetationField3D::_get_pattern(float p_spacing, int p_species) {
	const uint64_t key = (uint64_t(hash_murmur3_one_float(p_spacing, hash_murmur3_one_float(tile_size, uint32_t(seed)))) << 32) | uint32_t(p_species);
	Pattern *existing = patterns.getptr(key);
	if (existing) {
		return *existing;
	}
	Pattern p;
	RandomPCG rng(uint64_t(seed) * 1000003ull + uint64_t(p_species) * 7919ull + 17ull);
	poisson_torus(tile_size, p_spacing, rng, p.points);
	const int count = p.points.size();
	// Random ranks: Bridson inserts points in a spatially coherent order, so the order is
	// shuffled before ranking or density would cut the pattern into patches.
	LocalVector<int> perm;
	perm.resize(count);
	for (int i = 0; i < count; i++) {
		perm[i] = i;
	}
	for (int i = count - 1; i > 0; i--) {
		SWAP(perm[i], perm[rng.rand(i + 1)]);
	}
	p.ranks.resize(count);
	p.randoms.resize(count);
	for (int i = 0; i < count; i++) {
		p.ranks[perm[i]] = (i + 0.5f) / count;
		p.randoms[i] = rng.randf();
	}
	patterns.insert(key, p);
	return patterns[key];
}

// Where streaming fields' placed plants end (vegetation_near_radius): past it only the ground
// shows them, through the terrain's far blend of the cover map, which a TerrainMaterial3D with
// far_start_from_vegetation starts before it. With several streaming fields (a forest out to
// 150 m, the floor and meadow plants to 80 m) the nearest end counts, so the blend has taken over
// before the first plants run out. A field takes its radius back (0) when it stops.
float VegetationField3D::near_radius_published = 0.0f;

// The drawn ground's height over the heightfield at p's xz (Landscape3D's bank and cliff relief, WorldGround.relief_at):
// 0 with no relief producer or out of its reach.
static float _relief_y(const Vector3 &p_at) {
	const float r = WorldContracts::ground()->relief_at(p_at).x;
	return Math::is_finite(r) ? r : 0.0f;
}

// A plant's mesh cut off at p_height (a mown meadow's stubble): every triangle clipped to the part below the plane
// (Sutherland-Hodgman against y = p_height), the attributes interpolated along the cut edges; the cut stalks keep
// their materials. LODs and the card ranks (CUSTOM0) are dropped: a few stalks need neither.
static Ref<ArrayMesh> _cut_mesh(const Ref<Mesh> &p_mesh, float p_height) {
	Ref<ArrayMesh> out;
	out.instantiate();
	if (p_mesh.is_null()) {
		return out;
	}
	for (int si = 0; si < p_mesh->get_surface_count(); si++) {
		const Array a = p_mesh->surface_get_arrays(si);
		const PackedVector3Array vin = a[Mesh::ARRAY_VERTEX];
		if (vin.is_empty()) {
			continue;
		}
		const PackedVector3Array nin = a[Mesh::ARRAY_NORMAL];
		const PackedFloat32Array tin = a[Mesh::ARRAY_TANGENT];
		const PackedColorArray cin = a[Mesh::ARRAY_COLOR];
		const PackedVector2Array uin = a[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array u2in = a[Mesh::ARRAY_TEX_UV2];
		PackedInt32Array idx = a[Mesh::ARRAY_INDEX];
		if (idx.is_empty()) {
			idx.resize(vin.size());
			for (int i = 0; i < vin.size(); i++) {
				idx.set(i, i);
			}
		}
		struct V {
			Vector3 p, n;
			float t[4] = { 1, 0, 0, 1 };
			Color c;
			Vector2 uv, uv2;
		};
		auto get = [&](int i) {
			V v;
			v.p = vin[i];
			v.n = i < nin.size() ? nin[i] : Vector3(0, 1, 0);
			if (tin.size() >= (i + 1) * 4) {
				for (int k = 0; k < 4; k++) {
					v.t[k] = tin[i * 4 + k];
				}
			}
			v.c = i < cin.size() ? cin[i] : Color(1, 1, 1);
			v.uv = i < uin.size() ? uin[i] : Vector2();
			v.uv2 = i < u2in.size() ? u2in[i] : Vector2();
			return v;
		};
		auto mix = [](const V &p_a, const V &p_b, float p_t) {
			V v;
			v.p = p_a.p.lerp(p_b.p, p_t);
			v.n = p_a.n.lerp(p_b.n, p_t).normalized();
			for (int k = 0; k < 4; k++) {
				v.t[k] = Math::lerp(p_a.t[k], p_b.t[k], p_t);
			}
			v.c = p_a.c.lerp(p_b.c, p_t);
			v.uv = p_a.uv.lerp(p_b.uv, p_t);
			v.uv2 = p_a.uv2.lerp(p_b.uv2, p_t);
			return v;
		};
		LocalVector<V> verts;
		for (int i = 0; i + 2 < idx.size(); i += 3) {
			const V tri[3] = { get(idx[i]), get(idx[i + 1]), get(idx[i + 2]) };
			V poly[4];
			int n = 0;
			for (int k = 0; k < 3; k++) {
				const V &cur = tri[k];
				const V &nxt = tri[(k + 1) % 3];
				const bool in_c = cur.p.y <= p_height;
				const bool in_n = nxt.p.y <= p_height;
				if (in_c) {
					poly[n++] = cur;
				}
				if (in_c != in_n) {
					poly[n++] = mix(cur, nxt, (p_height - cur.p.y) / (nxt.p.y - cur.p.y));
				}
			}
			for (int k = 1; k + 1 < n; k++) {
				verts.push_back(poly[0]);
				verts.push_back(poly[k]);
				verts.push_back(poly[k + 1]);
			}
		}
		if (verts.is_empty()) {
			continue;
		}
		PackedVector3Array vo, no;
		PackedFloat32Array to;
		PackedColorArray co;
		PackedVector2Array uo, u2o;
		for (const V &v : verts) {
			vo.push_back(v.p);
			no.push_back(v.n);
			for (int k = 0; k < 4; k++) {
				to.push_back(v.t[k]);
			}
			co.push_back(v.c);
			uo.push_back(v.uv);
			u2o.push_back(v.uv2);
		}
		Array arr;
		arr.resize(Mesh::ARRAY_MAX);
		arr[Mesh::ARRAY_VERTEX] = vo;
		arr[Mesh::ARRAY_NORMAL] = no;
		if (!tin.is_empty()) {
			arr[Mesh::ARRAY_TANGENT] = to;
		}
		if (!cin.is_empty()) {
			arr[Mesh::ARRAY_COLOR] = co;
		}
		if (!uin.is_empty()) {
			arr[Mesh::ARRAY_TEX_UV] = uo;
		}
		if (!u2in.is_empty()) {
			arr[Mesh::ARRAY_TEX_UV2] = u2o;
		}
		out->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
		out->surface_set_material(out->get_surface_count() - 1, p_mesh->surface_get_material(si));
	}
	return out;
}

// The top edge of a steep drop in the drawn ground (heightfield plus relief) within 1 m of p_xz (RULE_BANK_EDGE):
// along each of 16 directions from a point not on a face itself, the first 5 cm step steeper than 45 degrees, and how
// far the ground falls over the 0.6 m past it. Returns the largest fall (m, 0 with no edge near); r_dir the way it falls, r_ahead the edge's distance;
// r_face (if given) the face's angle that way (radians from level: its fall over its run while steeper than 35 degrees, up to 1.5 m).
static float _bank_edge(const TerrainDataMap3D *p_map, const Vector2 &p_xz, Vector2 &r_dir, float &r_ahead, float *r_face = nullptr) {
	auto h = [&](const Vector2 &q) {
		const Vector3 p(q.x, 0.0f, q.y);
		return p_map->get_height_at(p) + _relief_y(p);
	};
	const float step = 0.05f;
	float best = 0.0f;
	r_dir = Vector2();
	r_ahead = 0.0f;
	for (int k = 0; k < 16; k++) {
		const float a = Math::TAU * k / 16.0f;
		const Vector2 d(Math::cos(a), Math::sin(a));
		float prev = h(p_xz);
		if (h(p_xz - d * step) - prev > step * 0.7f) {
			continue; // On the face already (steeper than 35 degrees behind): not the top.
		}
		for (int i = 1; i <= 20; i++) {
			const Vector2 q = p_xz + d * (step * i);
			const float hq = h(q);
			if (prev - hq > step) { // Past 45 degrees: the edge.
				const float e = step * (i - 1);
				const float drop = h(p_xz + d * e) - h(p_xz + d * (e + 0.6f));
				if (drop > best) {
					best = drop;
					r_dir = d;
					r_ahead = e;
				}
				break;
			}
			prev = hq;
		}
	}
	if (r_face && best > 0.0f) {
		// The 16 directions are 22.5 degrees apart: the steepest of the found one and its two neighbours, so a face
		// met at an angle does not read as a gentler one.
		const Vector2 top = p_xz + r_dir * r_ahead;
		const float h0 = h(top);
		*r_face = float(Math::PI * 0.25);
		for (int k = -1; k <= 1; k++) {
			const Vector2 d = r_dir.rotated(float(k) * float(Math::TAU / 16.0));
			float hp = h0;
			int n = 0;
			for (int i = 1; i <= 30; i++) {
				const float hq = h(top + d * (step * i));
				if (hp - hq < step * 0.7f) {
					break;
				}
				hp = hq;
				n = i;
			}
			if (n > 0) {
				*r_face = MAX(*r_face, float(Math::atan2(h0 - hp, step * n)));
			}
		}
	}
	return best;
}

static HashMap<ObjectID, float> &_near_radii() {
	static HashMap<ObjectID, float> radii;
	return radii;
}

void VegetationField3D::_publish_near_radius(float p_radius) {
	HashMap<ObjectID, float> &radii = _near_radii();
	const ObjectID id = get_instance_id();
	if (p_radius > 0.0f) {
		const float *had = radii.getptr(id);
		if (had && *had == p_radius) {
			return;
		}
		radii[id] = p_radius;
	} else if (!radii.erase(id)) {
		return;
	}
	float r = 0.0f;
	for (const KeyValue<ObjectID, float> &kv : radii) {
		r = r > 0.0f ? MIN(r, kv.value) : kv.value;
	}
	print_verbose(vformat("VegetationField3D \"%s\": stream radius %.1f m %s; vegetation_near_radius %.1f m.", get_name(), p_radius, p_radius > 0.0f ? "published" : "withdrawn", r));
	if (r == near_radius_published) {
		return;
	}
	near_radius_published = r;
	RenderingServer::get_singleton()->global_shader_parameter_set("vegetation_near_radius", r);
}

void VegetationField3D::_clear_generated() {
	_publish_near_radius(0.0f);
	_stream_clear();
	{
		RWLockWrite guard(crown_lock);
		crowns.clear();
		crown_grid.clear();
		max_crown_radius = 0.0f;
		deadwood.clear();
		deadwood_grid.clear();
		deadwood_max_radius = 0.0f;
	}
	for (MultiMeshInstance3D *mmi : generated) {
		if (mmi->get_parent()) {
			mmi->get_parent()->remove_child(mmi);
		}
		memdelete(mmi);
	}
	generated.clear();
	for (KeyValue<ObjectID, LazyNear> &kv : lazy_near) {
		kv.value.far = nullptr;
		_lazy_near_free(kv.value);
	}
	lazy_near.clear();
	has_near_swap = false;
	impostor_cells.clear();
	impostor_meshes.clear();
	set_process_internal(false);
	placements.clear();
	shelter_static.clear();
	shelter_dirty = true;
	far_image.unref();
	far_view_state = 0;
	far_view_round = 0;
	plant_count = 0;
}

void VegetationField3D::clear() {
	_clear_generated();
	_report_stand_disturbances(true);
}

// A placed disturbed stand (TreeGenerator's windthrow and beetle stands: species meta "stand_disturbance" =
// Vector2(WorldStand kind, years ago)) reports its patches to the stand-age map (WorldStand.add_disturbance;
// docs/ideas/world-contracts.md Stand, trees.md 5aq), so the vegetation and successions placed there see them:
// each 30 m cell holding at least three of its legacy plants (Senf and Seidl 2021 grade disturbance on 30 m cells)
// becomes a disc centred on them. The previous report is taken back first. Streaming fields place no fixed stand
// and report nothing.
void VegetationField3D::_report_stand_disturbances(bool p_clear_only) {
	WorldContracts *wc = WorldContracts::get_singleton();
	Ref<WorldStand> stand = wc ? wc->get_stand() : Ref<WorldStand>();
	if (stand.is_null()) {
		stand_disturbances.clear();
		stand_disturbance_discs.clear();
		stand_disturbance_years.clear();
		return;
	}
	// Discs as (x, z, radius, kind) and years; a populate that places the same ones changes nothing on the map, so
	// two fields reading the stand rules do not wake each other with identical re-reports.
	LocalVector<Vector4> discs;
	LocalVector<float> years;
	if (!p_clear_only && !streaming && biome.is_valid()) {
		_stand_discs(discs, years);
	}
	if (discs.size() == stand_disturbance_discs.size() && discs.size() == stand_disturbances.size()) {
		bool same = true;
		for (uint32_t i = 0; i < discs.size() && same; i++) {
			same = discs[i].is_equal_approx(stand_disturbance_discs[i]) && Math::is_equal_approx(years[i], stand_disturbance_years[i]);
		}
		if (same) {
			return;
		}
	}
	for (int id : stand_disturbances) {
		stand->remove_disturbance(id);
	}
	stand_disturbances.clear();
	for (uint32_t i = 0; i < discs.size(); i++) {
		stand_disturbances.push_back(stand->add_disturbance(Vector3(discs[i].x, 0.0f, discs[i].y), discs[i].z, int(discs[i].w), years[i]));
	}
	stand_disturbance_discs = discs;
	stand_disturbance_years = years;
}

void VegetationField3D::_stand_discs(LocalVector<Vector4> &r_discs, LocalVector<float> &r_years) const {
	constexpr float CELL = 30.0f;
	struct Cell {
		int n = 0;
		Vector2 sum;
		float years = 1e9f; // The youngest kill in the cell, as the map's youngest event wins.
		LocalVector<Vector2> points;
	};
	HashMap<Vector2i, Cell> cells[8];
	const TypedArray<PlantSpecies> list = biome->get_species();
	for (int s = 0; s < list.size() && s < int(placements.size()); s++) {
		const Ref<PlantSpecies> sp = list[s];
		if (sp.is_null() || !sp->has_meta(SNAME("stand_disturbance"))) {
			continue;
		}
		const Vector2 kd = sp->get_meta(SNAME("stand_disturbance"));
		const int kind = int(kd.x);
		// Fire, wind, beetle, avalanche, ice storm: the stands' own kills. A beaver meadow is the dam's to report and
		// boar rooting the ground's.
		if (kind < 1 || kind > 7 || kind == 4 || kind == 5) {
			continue;
		}
		for (const Vector3 &p : placements[s]) {
			Cell &c = cells[kind][Vector2i(Math::floor(p.x / CELL), Math::floor(p.z / CELL))];
			c.n++;
			c.sum += Vector2(p.x, p.z);
			c.years = MIN(c.years, kd.y);
			c.points.push_back(Vector2(p.x, p.z));
		}
	}
	for (int kind = 1; kind < 8; kind++) {
		for (const KeyValue<Vector2i, Cell> &kv : cells[kind]) {
			if (kv.value.n < 3) {
				continue;
			}
			// The disc reaches the cell's farthest legacy plant (a crown's width more), no wider than the cell.
			const Vector2 c = kv.value.sum / float(kv.value.n);
			float reach = 0.0f;
			for (const Vector2 &q : kv.value.points) {
				reach = MAX(reach, c.distance_to(q));
			}
			r_discs.push_back(Vector4(c.x, c.y, MIN(reach + 3.0f, CELL * 0.75f), float(kind)));
			r_years.push_back(kv.value.years);
		}
	}
}

float VegetationLight::sun_mean(const TerrainDataMap3D *p_map) {
	if (!p_map || !p_map->has_climate()) {
		return 0.0f;
	}
	const Rect2 r = p_map->get_map_rect();
	double sum = 0.0;
	for (int z = 0; z < 16; z++) {
		for (int x = 0; x < 16; x++) {
			sum += p_map->get_shortwave_at(Vector3(r.position.x + (x + 0.5f) * r.size.x / 16.0f, 0.0f, r.position.y + (z + 0.5f) * r.size.y / 16.0f));
		}
	}
	return float(sum / 256.0);
}

float VegetationLight::exposure(const TerrainDataMap3D *p_map, float p_sun_mean, float p_x, float p_z) {
	if (p_sun_mean <= 0.0f) {
		return 1.0f;
	}
	return CLAMP(p_map->get_shortwave_at(Vector3(p_x, 0.0f, p_z)) / p_sun_mean, 0.3f, 1.5f);
}

// Everything placement needs, read on the main thread so tiles can be placed on workers.
// The geology's loose sand (TerrainGenerator3D::get_geology_image(), R below 0) on its grid over
// the generator's world rect.
void VegetationField3D::_read_sand(const TerrainGenerator3D *p_gen, PlaceSetup &r_setup) {
	r_setup.sand.clear();
	r_setup.sand_nx = 0;
	r_setup.sand_ny = 0;
	if (!p_gen) {
		return;
	}
	const Ref<Image> img = p_gen->get_geology_image();
	if (img.is_null() || img->get_width() < 2 || img->get_height() < 2 || img->get_format() != Image::FORMAT_RGBAF) {
		return;
	}
	const int w = img->get_width(), h = img->get_height();
	const float *src = reinterpret_cast<const float *>(img->get_data().ptr());
	bool any = false;
	r_setup.sand.resize(w * h);
	for (int i = 0; i < w * h; i++) {
		const float s = MAX(-src[i * 4], 0.0f);
		r_setup.sand[i] = s;
		any = any || s > 0.0f;
	}
	if (!any) {
		r_setup.sand.clear();
		return;
	}
	r_setup.sand_nx = w;
	r_setup.sand_ny = h;
	r_setup.sand_rect = p_gen->get_world_rect();
}

// How much the desert sand moves (docs/ideas/vegetation.md, "Arid ground"): crests and upper flanks every windy
// day, the plinths less, the interdune corridors under thin sand not at all (Tsoar 2005). From the sand sea's depth
// on its own grid: its place between the local low and high over about half a crest spacing (crests come some 25
// cells apart), and 0 under thin sand. Slip faces are raised at placement from the ground's slope.
void VegetationField3D::_read_sand_mobility(const TerrainGenerator3D *p_gen, PlaceSetup &r_setup) {
	r_setup.mobility.clear();
	r_setup.sea_depth.clear();
	r_setup.mobility_nx = 0;
	r_setup.mobility_ny = 0;
	if (!p_gen || !p_gen->get_sand_sea_rect().has_area()) {
		return;
	}
	const Rect2 rect = p_gen->get_sand_sea_rect();
	const float cell = MAX(float(p_gen->get_sand_sea_scale()), 1.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 2048);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 2048);
	LocalVector<float> depth;
	depth.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const float d = float(p_gen->get_sand_sea_height(Vector3(rect.position.x + rect.size.x * x / (nx - 1), 0.0f, rect.position.y + rect.size.y * z / (ny - 1))));
			depth[z * nx + x] = d;
			any = any || d > 0.05f;
		}
	}
	if (!any) {
		return;
	}
	// Local low and high: separable sliding windows.
	const int r = 12;
	LocalVector<float> lo_x, hi_x, lo, hi;
	lo_x.resize(nx * ny);
	hi_x.resize(nx * ny);
	lo.resize(nx * ny);
	hi.resize(nx * ny);
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			float a = 1e9f, b = -1e9f;
			for (int k = MAX(x - r, 0); k <= MIN(x + r, nx - 1); k++) {
				a = MIN(a, depth[z * nx + k]);
				b = MAX(b, depth[z * nx + k]);
			}
			lo_x[z * nx + x] = a;
			hi_x[z * nx + x] = b;
		}
	}
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			float a = 1e9f, b = -1e9f;
			for (int k = MAX(z - r, 0); k <= MIN(z + r, ny - 1); k++) {
				a = MIN(a, lo_x[k * nx + x]);
				b = MAX(b, hi_x[k * nx + x]);
			}
			lo[z * nx + x] = a;
			hi[z * nx + x] = b;
		}
	}
	r_setup.mobility.resize(nx * ny);
	for (int i = 0; i < nx * ny; i++) {
		const float d = depth[i];
		if (d < 0.05f) {
			r_setup.mobility[i] = -1.0f;
			continue;
		}
		const float place = (d - lo[i]) / (hi[i] - lo[i] + 0.3f);
		const float m = Math::smoothstep(0.3f, 0.85f, place) * Math::smoothstep(0.2f, 0.6f, d);
		r_setup.mobility[i] = m;
	}
	r_setup.mobility_nx = nx;
	r_setup.mobility_ny = ny;
	r_setup.mobility_rect = rect;
	r_setup.sea_depth = depth;
}

// A felled crown or snapped stem (TreeWind3D: storms, avalanches) keeps only p_keep of its leaf area: every crown
// centred within p_radius of p_xz. Leaf area scales, so opacity = 1 - (1 - opacity)^keep (Beer's law, as the
// canopy map's LAI = -2 ln(1 - opacity)).
void VegetationField3D::_apply_canopy_losses(LocalVector<Crown> &r_crowns, const LocalVector<Vector4> &p_losses, int p_from) {
	for (uint32_t i = p_from; i < p_losses.size(); i++) {
		const Vector4 &l = p_losses[i];
		const Vector2 c(l.x, l.y);
		const float keep = CLAMP(l.w, 0.0f, 1.0f);
		for (Crown &cr : r_crowns) {
			if (cr.xz.distance_squared_to(c) < l.z * l.z) {
				cr.opacity = keep <= 0.0f ? 0.0f : 1.0f - Math::pow(MAX(1.0f - cr.opacity, 1e-4f), keep);
			}
		}
	}
}

void VegetationField3D::add_canopy_loss(const Vector2 &p_xz, float p_radius, float p_keep) {
	ERR_FAIL_COND(p_radius <= 0.0f);
	canopy_losses.push_back(Vector4(p_xz.x, p_xz.y, p_radius, CLAMP(p_keep, 0.0f, 1.0f)));
	// The crowns already placed: get_shade_at() and the plants under them see the gap now; the canopy map at
	// its next repaint (the caller cuts the live map itself).
	RWLockWrite guard(crown_lock);
	_apply_canopy_losses(crowns, canopy_losses, canopy_losses.size() - 1);
}

void VegetationField3D::add_plant_removal(const Vector3 &p_position, float p_radius, int p_species) {
	ERR_FAIL_COND(p_radius <= 0.0f);
	plant_removals.push_back(Vector4(p_position.x, p_position.z, p_radius, float(MAX(p_species, -1))));
	const Rect2 area(p_position.x - p_radius, p_position.z - p_radius, 2.0f * p_radius, 2.0f * p_radius);
	if (streaming) {
		if (stream_setup_valid) {
			removal_dirty = removal_dirty.has_area() ? removal_dirty.merge(area) : area;
		}
	} else if (plant_count > 0) {
		queue_populate();
	}
}

PackedVector4Array VegetationField3D::get_plant_removals() const {
	PackedVector4Array out;
	for (const Vector4 &r : plant_removals) {
		out.push_back(r);
	}
	return out;
}

void VegetationField3D::clear_plant_removals() {
	if (plant_removals.is_empty()) {
		return;
	}
	plant_removals.clear();
	if (streaming) {
		if (stream_setup_valid) {
			removal_dirty = stream_setup.rect;
		}
	} else if (plant_count > 0) {
		queue_populate();
	}
}

Array VegetationField3D::get_instances_near(int p_species, const Vector3 &p_position, float p_radius) const {
	Array out;
	if (biome.is_null() || p_species < 0 || p_species >= biome->get_species().size()) {
		return out;
	}
	const Ref<PlantSpecies> sp = biome->get_species()[p_species];
	if (sp.is_null()) {
		return out;
	}
	const TypedArray<Mesh> meshes = sp->get_meshes();
	const Vector2 c(p_position.x, p_position.z);
	const float r2 = p_radius * p_radius;
	for (int i = 0; i < get_child_count(true); i++) {
		const MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(get_child(i, true));
		if (!mmi || !mmi->is_visible() || mmi->get_multimesh().is_null()) {
			continue;
		}
		const Ref<MultiMesh> mm = mmi->get_multimesh();
		const Ref<Mesh> mesh = mm->get_mesh();
		// A hay meadow's cut stalks (meta hay_cut_of: the mesh they were cut from) count as the species' too.
		if (mesh.is_null() || !(meshes.has(mesh) || (mesh->has_meta(SNAME("hay_cut_of")) && meshes.has(mesh->get_meta(SNAME("hay_cut_of")))))) {
			continue;
		}
		const Transform3D xf = mmi->get_global_transform();
		const int n = mm->get_visible_instance_count() >= 0 ? mm->get_visible_instance_count() : mm->get_instance_count();
		for (int k = 0; k < n; k++) {
			const Transform3D t = xf * mm->get_instance_transform(k);
			if (Vector2(t.origin.x, t.origin.z).distance_squared_to(c) > r2) {
				continue;
			}
			Dictionary d;
			d["transform"] = t;
			d["mesh"] = mesh;
			out.push_back(d);
		}
	}
	return out;
}

PackedVector4Array VegetationField3D::get_canopy_losses() const {
	PackedVector4Array out;
	for (const Vector4 &l : canopy_losses) {
		out.push_back(l);
	}
	return out;
}

void VegetationField3D::set_spring_pools(const NodePath &p_path) {
	if (spring_pools == p_path) {
		return;
	}
	spring_pools = p_path;
	if (streaming) {
		_stream_clear(); // Placed again round the new pools.
	}
}

// Signed distance (m) to the nearest pool shore, negative inside a pool; p_reach past every pool's bounds.
float VegetationField3D::PlaceSetup::pool_distance(const Vector2 &p_xz, float p_reach) const {
	float best = p_reach;
	for (uint32_t k = 0; k < pools.size(); k++) {
		if (!pool_bounds[k].grow(p_reach).has_point(p_xz)) {
			continue;
		}
		const PackedVector2Array &o = pools[k];
		const int n = o.size();
		float d2 = 1e30f;
		bool inside = false;
		for (int i = 0, j = n - 1; i < n; j = i++) {
			const Vector2 a = o[j];
			const Vector2 b = o[i];
			const Vector2 ab = b - a;
			const float t = CLAMP((p_xz - a).dot(ab) / MAX(ab.length_squared(), 1e-12f), 0.0f, 1.0f);
			d2 = MIN(d2, p_xz.distance_squared_to(a + ab * t));
			if ((a.y > p_xz.y) != (b.y > p_xz.y) && p_xz.x < a.x + (p_xz.y - a.y) * ab.x / ab.y) {
				inside = !inside;
			}
		}
		const float d = inside ? -Math::sqrt(d2) : Math::sqrt(d2);
		if (inside) {
			return d; // Pools do not overlap.
		}
		best = MIN(best, d);
	}
	return best;
}

PackedVector4Array VegetationField3D::get_springs() const {
	PackedVector4Array out;
	for (uint32_t i = 0; i < springs_seen.springs.size(); i++) {
		const Vector3 &p = springs_seen.springs[i];
		out.push_back(Vector4(p.x, p.y, p.z, springs_seen.discharge[i]));
	}
	return out;
}

float VegetationField3D::PlaceSetup::sea_depth_at(const Vector2 &p_xz) const {
	if (sea_depth.is_empty()) {
		return 0.0f;
	}
	const float gx = (p_xz.x - mobility_rect.position.x) / mobility_rect.size.x * (mobility_nx - 1);
	const float gz = (p_xz.y - mobility_rect.position.y) / mobility_rect.size.y * (mobility_ny - 1);
	if (gx < 0.0f || gz < 0.0f || gx > mobility_nx - 1 || gz > mobility_ny - 1) {
		return 0.0f;
	}
	const int ix = MIN(int(gx), mobility_nx - 2), iz = MIN(int(gz), mobility_ny - 2);
	const float fx = gx - ix, fz = gz - iz;
	const float *r0 = &sea_depth[iz * mobility_nx + ix];
	const float *r1 = r0 + mobility_nx;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

// Burnt ground (docs/ideas/vegetation.md, "Burnt ground and regrowth"): the fire contract's years since the ground last
// burnt, sampled once per setup on a grid of 2 m or more over the map (at most 512 a side). Regrowth takes seasons, so
// a fire burning now shows once the field places again.
void VegetationField3D::_read_burn_age(PlaceSetup &r_setup) const {
	r_setup.burn.clear();
	r_setup.burn_nx = 0;
	r_setup.burn_ny = 0;
	const WorldFire *fire = WorldContracts::fire();
	if (fire) {
		burn_seen = fire->get_burn_generation();
	}
	bool wanted = false;
	if (biome.is_valid()) {
		const TypedArray<PlantSpecies> list = biome->get_species();
		for (int i = 0; i < list.size() && !wanted; i++) {
			const Ref<PlantSpecies> sp = list[i];
			wanted = sp.is_valid() && (sp->uses_burn_age() || sp->get_burn_recovery() > 0.0f || (_deferred_kinds(sp) & 1));
			const TypedArray<Mesh> ms = sp.is_valid() ? sp->get_meshes() : TypedArray<Mesh>();
			for (int m = 0; m < ms.size() && !wanted; m++) {
				const Ref<Mesh> mesh = ms[m];
				wanted = mesh.is_valid() && mesh->has_meta(SNAME("tree_fire_years"));
			}
		}
	}
	burn_follow = wanted;
	if (!wanted || !fire || !fire->has_provider() || !r_setup.rect.has_area()) {
		return;
	}
	const Rect2 rect = r_setup.rect;
	const float cell = MAX(2.0f, MAX(rect.size.x, rect.size.y) / 511.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	LocalVector<float> ages;
	ages.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 xz = rect.position + rect.size * Vector2(float(x) / (nx - 1), float(z) / (ny - 1));
			const float a = fire->burn_age_at(Vector3(xz.x, 0.0f, xz.y));
			ages[z * nx + x] = a;
			any = any || a >= 0.0f;
		}
	}
	if (!any) {
		return;
	}
	r_setup.burn = ages;
	r_setup.burn_nx = nx;
	r_setup.burn_ny = ny;
	r_setup.burn_rect = rect;
}

// Stand age and disturbance (docs/ideas/vegetation.md, "Stand age"; WorldStand in world-contracts.md, Stand): the
// years since the last stand-replacing disturbance and its kind, on the burn grid's spacing over the map. Read when a
// species uses the stand rules or is a succession stage (TreeGenerator.make_succession).
void VegetationField3D::_read_stand(PlaceSetup &r_setup) const {
	r_setup.stand_rules.clear();
	r_setup.stand_age.clear();
	r_setup.stand_kind.clear();
	r_setup.browse_rules.clear();
	r_setup.stand_browse.clear();
	r_setup.rooting_rules.clear();
	r_setup.stand_rooting.clear();
	r_setup.stand_nx = 0;
	r_setup.stand_ny = 0;
	const WorldStand *stand = WorldContracts::stand();
	if (stand) {
		stand_seen = stand->get_stand_generation();
	}
	// The field's species, and the canopy field's trees when a species here reads the forest edge (the edge follows
	// their stand stages).
	LocalVector<Ref<Biome>> biomes;
	if (biome.is_valid()) {
		biomes.push_back(biome);
		const VegetationField3D *other = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
		if (other && other != this && other->biome.is_valid()) {
			const TypedArray<PlantSpecies> own = biome->get_species();
			for (int i = 0; i < own.size(); i++) {
				const Ref<PlantSpecies> sp = own[i];
				if (sp.is_valid() && sp->uses_forest_edge()) {
					biomes.push_back(other->biome);
					break;
				}
			}
		}
	}
	for (const Ref<Biome> &b : biomes) {
		const TypedArray<PlantSpecies> list = b->get_species();
		for (int i = 0; i < list.size(); i++) {
			const Ref<PlantSpecies> sp = list[i];
			if (sp.is_valid() && sp->uses_browse() && b == biome) {
				const Vector2 r = sp->get_rule_range(PlantSpecies::RULE_BROWSE);
				const Vector3 br(r.x, r.y, sp->get_rule_fade(PlantSpecies::RULE_BROWSE));
				if (r_setup.browse_rules.find(br) < 0) {
					r_setup.browse_rules.push_back(br);
				}
			}
			if (sp.is_valid() && sp->uses_rooting() && b == biome) {
				const Vector3 rr = sp->get_rooting_rule();
				if (r_setup.rooting_rules.find(rr) < 0) {
					r_setup.rooting_rules.push_back(rr);
				}
			}
			if (sp.is_null() || !sp->uses_stand()) {
				continue;
			}
			PlaceSetup::StandRule rule;
			sp->get_stand_rules(rule.age, rule.age_fade, rule.kind, rule.kind_fade);
			if (r_setup.stand_rules.find(rule) < 0) {
				r_setup.stand_rules.push_back(rule);
			}
		}
	}
	_read_strikes(r_setup);
	_read_browse(r_setup);
	stand_follow = !r_setup.stand_rules.is_empty() || !r_setup.browse_rules.is_empty() || !r_setup.rooting_rules.is_empty() || strike_follow || browse_follow;
	if ((r_setup.stand_rules.is_empty() && r_setup.browse_rules.is_empty() && r_setup.rooting_rules.is_empty()) || !stand || !r_setup.rect.has_area()) {
		return;
	}
	const Rect2 rect = r_setup.rect;
	const float cell = MAX(2.0f, MAX(rect.size.x, rect.size.y) / 511.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	r_setup.stand_age.resize(nx * ny);
	r_setup.stand_kind.resize(nx * ny);
	const bool browse = !r_setup.browse_rules.is_empty();
	if (browse) {
		r_setup.stand_browse.resize(nx * ny);
	}
	const bool rooting = !r_setup.rooting_rules.is_empty();
	if (rooting) {
		r_setup.stand_rooting.resize(nx * ny);
	}
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 xz = rect.position + rect.size * Vector2(float(x) / (nx - 1), float(z) / (ny - 1));
			const Vector3 p(xz.x, 0.0f, xz.y);
			r_setup.stand_age[z * nx + x] = stand->stand_age_at(p);
			r_setup.stand_kind[z * nx + x] = uint8_t(CLAMP(stand->disturbance_at(p), 0, 255));
			if (browse) {
				r_setup.stand_browse[z * nx + x] = stand->browse_at(p);
			}
			if (rooting) {
				r_setup.stand_rooting[z * nx + x] = stand->rooting_age_at(p);
			}
		}
	}
	r_setup.stand_nx = nx;
	r_setup.stand_ny = ny;
	r_setup.stand_rect = rect;
}

// Deferred tree variants (trees.md 5bc): the kinds the setup's area needs (any burn, strike or browse over it) are
// asked of each species' variant source; true when a species' meshes grew (its placements must be set up again).
bool VegetationField3D::_realize_variants(const PlaceSetup &p_setup, bool p_wait, LocalVector<int> *r_grown) {
	variants_waiting = false;
	if (biome.is_null()) {
		return false;
	}
	const int need = (p_setup.burn.is_empty() ? 0 : 1) | (p_setup.strikes.is_empty() ? 0 : 2) | (p_setup.browse.is_empty() ? 0 : 4);
	if (!need) {
		return false;
	}
	bool grew = false;
	const TypedArray<PlantSpecies> list = biome->get_species();
	for (int i = 0; i < list.size(); i++) {
		const Ref<PlantSpecies> sp = list[i];
		const int kinds = _deferred_kinds(sp) & need;
		if (!kinds) {
			continue;
		}
		Object *src = Object::cast_to<Object>(sp->get_meta(SNAME("_tree_variant_source"), Variant()));
		if (!src) {
			continue;
		}
		const int left = src->call(SNAME("request_variants"), sp, kinds, p_wait);
		if (left != kinds) {
			grew = true;
			if (r_grown) {
				r_grown->push_back(i);
			}
		}
		variants_waiting = variants_waiting || (left != 0 && !p_wait);
	}
	return grew;
}

// Struck trees (trees.md 5ax; WorldStand strikes in world-links.md): wanted when a species carries struck variants (mesh
// meta tree_lightning_years). The strikes over the setup's area are copied, so the placing threads read a fixed list.
void VegetationField3D::_read_strikes(PlaceSetup &r_setup) const {
	// Main thread only (setup and _stream_update): add_strike / advance_years change the list with no lock.
	r_setup.strikes.clear();
	r_setup.strike_cells.clear();
	bool wanted = false;
	if (biome.is_valid()) {
		const TypedArray<PlantSpecies> list = biome->get_species();
		for (int i = 0; i < list.size() && !wanted; i++) {
			const Ref<PlantSpecies> sp = list[i];
			wanted = (_deferred_kinds(sp) & 2) != 0;
			const TypedArray<Mesh> ms = sp.is_valid() ? sp->get_meshes() : TypedArray<Mesh>();
			for (int m = 0; m < ms.size() && !wanted; m++) {
				const Ref<Mesh> mesh = ms[m];
				wanted = mesh.is_valid() && mesh->has_meta(SNAME("tree_lightning_years"));
			}
		}
	}
	strike_follow = wanted;
	const WorldStand *stand = WorldContracts::stand();
	if (!wanted || !stand || !r_setup.rect.has_area()) {
		return;
	}
	stand->get_strikes_in(r_setup.rect, r_setup.strikes);
	const float R = WorldStand::STRIKE_RADIUS;
	for (uint32_t i = 0; i < r_setup.strikes.size(); i++) {
		const Vector3 &s = r_setup.strikes[i];
		for (int z = int(Math::floor((s.y - R) / 16.0f)); z <= int(Math::floor((s.y + R) / 16.0f)); z++) {
			for (int x = int(Math::floor((s.x - R) / 16.0f)); x <= int(Math::floor((s.x + R) / 16.0f)); x++) {
				r_setup.strike_cells[Vector2i(x, z)].push_back(i);
			}
		}
	}
}

// Years since the youngest strike whose reach holds the trunk at p_xz, -1 none (as WorldStand.strike_age_at).
float VegetationField3D::PlaceSetup::strike_at(const Vector2 &p_xz) const {
	if (strikes.is_empty()) {
		return -1.0f;
	}
	const LocalVector<uint32_t> *in = strike_cells.getptr(Vector2i(int(Math::floor(p_xz.x / 16.0f)), int(Math::floor(p_xz.y / 16.0f))));
	float r = -1.0f;
	if (in) {
		const float R = WorldStand::STRIKE_RADIUS;
		for (uint32_t i : *in) {
			const Vector3 &s = strikes[i];
			if (p_xz.distance_squared_to(Vector2(s.x, s.y)) <= R * R && (r < 0.0f || s.z < r)) {
				r = s.z;
			}
		}
	}
	return r;
}

// Browsed trees (trees.md 5az; WorldStand browse discs, world-contracts.md): wanted when a species carries browsed
// variants (mesh meta tree_browse). The pressure over the setup's area is sampled into a grid (at most 256 x 256, cells
// of 4 m or more) on the main thread, so the placing threads read a fixed copy.
void VegetationField3D::_read_browse(PlaceSetup &r_setup) const {
	r_setup.browse.clear();
	r_setup.browse_nx = 0;
	r_setup.browse_ny = 0;
	bool wanted = false;
	if (biome.is_valid()) {
		const TypedArray<PlantSpecies> list = biome->get_species();
		for (int i = 0; i < list.size() && !wanted; i++) {
			const Ref<PlantSpecies> sp = list[i];
			wanted = (_deferred_kinds(sp) & 4) != 0;
			const TypedArray<Mesh> ms = sp.is_valid() ? sp->get_meshes() : TypedArray<Mesh>();
			for (int m = 0; m < ms.size() && !wanted; m++) {
				const Ref<Mesh> mesh = ms[m];
				wanted = mesh.is_valid() && mesh->has_meta(SNAME("tree_browse"));
			}
		}
	}
	browse_follow = wanted;
	const WorldStand *stand = WorldContracts::stand();
	if (!wanted || !stand || !r_setup.rect.has_area()) {
		return;
	}
	const Rect2 rect = r_setup.rect;
	const float cell = MAX(4.0f, MAX(rect.size.x, rect.size.y) / 255.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 256);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 256);
	r_setup.browse.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 xz = rect.position + rect.size * Vector2(float(x) / (nx - 1), float(z) / (ny - 1));
			const float b = stand->browse_at(Vector3(xz.x, 0.0f, xz.y));
			r_setup.browse[z * nx + x] = b;
			any = any || b > 0.0f;
		}
	}
	if (!any) {
		r_setup.browse.clear(); // No deer here: every tree unbrowsed, nothing to look up.
		return;
	}
	r_setup.browse_nx = nx;
	r_setup.browse_ny = ny;
	r_setup.browse_rect = rect;
}

// Browsing pressure at p_xz, bilinear over the copied grid (0 off it or where none).
float VegetationField3D::PlaceSetup::browse_at(const Vector2 &p_xz) const {
	if (browse.is_empty()) {
		return 0.0f;
	}
	const float fx = (p_xz.x - browse_rect.position.x) / browse_rect.size.x * (browse_nx - 1);
	const float fz = (p_xz.y - browse_rect.position.y) / browse_rect.size.y * (browse_ny - 1);
	if (fx < 0.0f || fz < 0.0f || fx > browse_nx - 1 || fz > browse_ny - 1) {
		return 0.0f;
	}
	const int x0 = MIN(int(fx), browse_nx - 2), z0 = MIN(int(fz), browse_ny - 2);
	const float tx = fx - x0, tz = fz - z0;
	const float *g = browse.ptr() + z0 * browse_nx + x0;
	return Math::lerp(Math::lerp(g[0], g[1], tx), Math::lerp(g[browse_nx], g[browse_nx + 1], tx), tz);
}

// The stand changed over p_area: its grid cells are read again, and the area where a species' stand weight changed is
// returned (empty when every change stayed inside every species' stage, as a year passing mostly does).
Rect2 VegetationField3D::_update_stand(PlaceSetup &r_setup, const Rect2 &p_area) const {
	const WorldStand *stand = WorldContracts::stand();
	if (!stand || r_setup.stand_age.is_empty()) {
		_read_stand(r_setup);
		if (strike_follow || browse_follow) {
			// Struck trees: the strikes added or aged there re-place it.
			return r_setup.stand_age.is_empty() ? p_area : r_setup.rect;
		}
		return r_setup.stand_age.is_empty() ? Rect2() : r_setup.rect;
	}
	if (strike_follow) {
		_read_strikes(r_setup);
	}
	if (browse_follow) {
		_read_browse(r_setup);
	}
	stand_seen = stand->get_stand_generation();
	const Rect2 &sr = r_setup.stand_rect;
	const int nx = r_setup.stand_nx, ny = r_setup.stand_ny;
	const float cx = sr.size.x / (nx - 1);
	const float cz = sr.size.y / (ny - 1);
	const int x0 = CLAMP(int(Math::floor((p_area.position.x - sr.position.x) / cx)) - 1, 0, nx - 1);
	const int x1 = CLAMP(int(Math::ceil((p_area.get_end().x - sr.position.x) / cx)) + 1, 0, nx - 1);
	const int z0 = CLAMP(int(Math::floor((p_area.position.y - sr.position.y) / cz)) - 1, 0, ny - 1);
	const int z1 = CLAMP(int(Math::ceil((p_area.get_end().y - sr.position.y) / cz)) + 1, 0, ny - 1);
	Rect2 changed;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const Vector2 xz = sr.position + Vector2(x * cx, z * cz);
			const Vector3 p(xz.x, 0.0f, xz.y);
			const int i = z * nx + x;
			const float age = stand->stand_age_at(p);
			const int kind = CLAMP(stand->disturbance_at(p), 0, 255);
			const float br = r_setup.stand_browse.is_empty() ? 0.0f : stand->browse_at(p);
			const float ro = r_setup.stand_rooting.is_empty() ? -1.0f : stand->rooting_age_at(p);
			if (r_setup.stand_differs(r_setup.stand_age[i], r_setup.stand_kind[i], age, kind) || (!r_setup.stand_browse.is_empty() && r_setup.browse_differs(r_setup.stand_browse[i], br)) || (!r_setup.stand_rooting.is_empty() && r_setup.rooting_differs(r_setup.stand_rooting[i], ro))) {
				const Rect2 c(xz - Vector2(cx, cz) * 0.5f, Vector2(cx, cz));
				changed = changed.has_area() ? changed.merge(c) : c;
			}
			r_setup.stand_age[i] = age;
			r_setup.stand_kind[i] = uint8_t(kind);
			if (!r_setup.stand_browse.is_empty()) {
				r_setup.stand_browse[i] = br;
			}
			if (!r_setup.stand_rooting.is_empty()) {
				r_setup.stand_rooting[i] = ro;
			}
		}
	}
	if (strike_follow || browse_follow) {
		changed = changed.has_area() ? changed.merge(p_area) : p_area;
	}
	return changed;
}

int VegetationField3D::PlaceSetup::stand_cell(const Vector2 &p_xz) const {
	if (stand_age.is_empty()) {
		return -1;
	}
	const int x = int(Math::round((p_xz.x - stand_rect.position.x) / stand_rect.size.x * (stand_nx - 1)));
	const int z = int(Math::round((p_xz.y - stand_rect.position.y) / stand_rect.size.y * (stand_ny - 1)));
	if (x < 0 || z < 0 || x >= stand_nx || z >= stand_ny) {
		return -1;
	}
	return z * stand_nx + x;
}

bool VegetationField3D::PlaceSetup::rooting_differs(float p_a0, float p_a1) const {
	if (p_a0 == p_a1) {
		return false;
	}
	for (const Vector3 &r : rooting_rules) {
		if (PlantSpecies::rooting_weight(r, p_a0) != PlantSpecies::rooting_weight(r, p_a1)) {
			return true;
		}
	}
	return false;
}

bool VegetationField3D::PlaceSetup::browse_differs(float p_b0, float p_b1) const {
	if (p_b0 == p_b1) {
		return false;
	}
	for (const Vector3 &r : browse_rules) {
		auto w = [&r](float p_v) {
			return r.z <= 0.0f ? ((p_v >= r.x && p_v <= r.y) ? 1.0f : 0.0f) : Math::smoothstep(float(r.x - r.z), float(r.x), p_v) * (1.0f - Math::smoothstep(float(r.y), float(r.y + r.z), p_v));
		};
		if (w(p_b0) != w(p_b1)) {
			return true;
		}
	}
	return false;
}

bool VegetationField3D::PlaceSetup::stand_differs(float p_age0, int p_kind0, float p_age1, int p_kind1) const {
	if (p_age0 == p_age1 && p_kind0 == p_kind1) {
		return false;
	}
	for (const StandRule &r : stand_rules) {
		if (PlantSpecies::stand_weight(r.age, r.age_fade, r.kind, r.kind_fade, p_age0, p_kind0) != PlantSpecies::stand_weight(r.age, r.age_fade, r.kind, r.kind_fade, p_age1, p_kind1)) {
			return true;
		}
	}
	return false;
}

namespace {
float cover_mask_at(const Image *p_mask, const Rect2 &p_rect, const Vector2 &p_xz, int p_channel);

// Squared Euclidean distance transform of one line (Felzenszwalb and Huttenlocher 2012), in place.
void edt_1d(float *p_f, int p_n, int p_stride, LocalVector<float> &r_d, LocalVector<int> &r_v, LocalVector<float> &r_z) {
	const float inf = 1e20f;
	r_d.resize(p_n);
	r_v.resize(p_n);
	r_z.resize(p_n + 1);
	int k = 0;
	r_v[0] = 0;
	r_z[0] = -inf;
	r_z[1] = inf;
	for (int q = 1; q < p_n; q++) {
		const float fq = p_f[q * p_stride];
		float sq = 0.0f;
		while (true) {
			const int v = r_v[k];
			sq = ((fq + float(q) * q) - (p_f[v * p_stride] + float(v) * v)) / (2.0f * (q - v));
			if (sq <= r_z[k] && k > 0) {
				k--;
				continue;
			}
			break;
		}
		k++;
		r_v[k] = q;
		r_z[k] = sq;
		r_z[k + 1] = inf;
	}
	k = 0;
	for (int q = 0; q < p_n; q++) {
		while (r_z[k + 1] < q) {
			k++;
		}
		const int v = r_v[k];
		r_d[q] = float(q - v) * (q - v) + p_f[v * p_stride];
	}
	for (int q = 0; q < p_n; q++) {
		p_f[q * p_stride] = r_d[q];
	}
}

void edt_2d(LocalVector<float> &r_f, int p_nx, int p_ny) {
	LocalVector<float> d, z;
	LocalVector<int> v;
	for (int x = 0; x < p_nx; x++) {
		edt_1d(r_f.ptr() + x, p_ny, p_nx, d, v, z);
	}
	for (int y = 0; y < p_ny; y++) {
		edt_1d(r_f.ptr() + y * p_nx, p_nx, 1, d, v, z);
	}
}

struct EdgeJob {
	const void *setup = nullptr; // VegetationField3D::PlaceSetup
	LocalVector<int> canopy;
	LocalVector<const PlantSpecies *> other_canopy; // The canopy field's (kept alive by its biome).
	Rect2 rect;
	int nx = 0;
	int ny = 0;
	uint8_t *in = nullptr;
};
} // namespace

void VegetationField3D::_edge_row(void *p_job, uint32_t p_row) {
	EdgeJob &job = *static_cast<EdgeJob *>(p_job);
	const PlaceSetup &su = *static_cast<const PlaceSetup *>(job.setup);
	const Image *mask = su.mask_img.ptr();
	for (int x = 0; x < job.nx; x++) {
		const Vector2 xz = job.rect.position + job.rect.size * Vector2(float(x) / (job.nx - 1), float(p_row) / (job.ny - 1));
		const Vector3 p(xz.x, 0.0f, xz.y);
		float sum = 0.0f;
		for (int s : job.canopy) {
			const PlantSpecies *sp = su.species[s].sp.ptr();
			float suit = sp->get_suitability(su.map, p);
			if (suit > 0.0f && sp->get_mask_channel() >= 0) {
				const float m = CLAMP(cover_mask_at(mask, su.mask_rect, xz, sp->get_mask_channel()), 0.0f, 1.0f);
				suit *= sp->is_mask_invert() ? 1.0f - m : m;
			}
			if (suit > 0.0f && !su.stand_rules.is_empty() && sp->uses_stand()) {
				Vector2 a, k;
				float af, kf;
				sp->get_stand_rules(a, af, k, kf);
				const int sc = su.stand_cell(xz);
				suit *= PlantSpecies::stand_weight(a, af, k, kf, sc < 0 ? -1.0f : su.stand_age[sc], sc < 0 ? 0 : int(su.stand_kind[sc]));
			}
			sum += suit * sp->get_density();
			if (sum >= EDGE_CLOSED) {
				break;
			}
		}
		for (uint32_t i = 0; i < job.other_canopy.size() && sum < EDGE_CLOSED; i++) {
			const PlantSpecies *sp = job.other_canopy[i];
			float suit = sp->get_suitability(su.map, p);
			if (suit > 0.0f && !su.stand_rules.is_empty() && sp->uses_stand()) {
				Vector2 a, k;
				float af, kf;
				sp->get_stand_rules(a, af, k, kf);
				const int sc = su.stand_cell(xz);
				suit *= PlantSpecies::stand_weight(a, af, k, kf, sc < 0 ? -1.0f : su.stand_age[sc], sc < 0 ? 0 : int(su.stand_kind[sc]));
			}
			sum += suit * sp->get_density();
		}
		job.in[p_row * job.nx + x] = sum >= EDGE_CLOSED ? 1 : 0;
	}
}

// Forest edges (docs/ideas/vegetation.md, "Forest edges"): where the field's canopy species would close the canopy,
// from their rules, patches, masks, stand stages and density (not from the placed crowns, which stream round the
// camera: every tile must see the same edge), then the signed distance to that forest's edge by an exact Euclidean
// distance transform both ways (Felzenszwalb and Huttenlocher 2012). Edge species, understory trees (a shade rule)
// and the field's own forest-edge users are not counted as canopy.
void VegetationField3D::_read_forest_edge(PlaceSetup &r_setup) const {
	r_setup.edge.clear();
	r_setup.edge_nx = 0;
	r_setup.edge_ny = 0;
	EdgeJob job;
	bool wanted = false;
	for (uint32_t s = 0; s < r_setup.species.size(); s++) {
		const Ref<PlantSpecies> &sp = r_setup.species[s].sp;
		if (sp.is_null()) {
			continue;
		}
		if (sp->uses_forest_edge()) {
			wanted = true;
		} else if (sp->get_canopy_radius() > 0.0f && !sp->uses_shade() && sp->get_density() > 0.0f) {
			job.canopy.push_back(int(s));
		}
	}
	// A floor under another field's forest (canopy_field): its canopy species count too (without their masks).
	if (wanted && r_setup.other && r_setup.other->biome.is_valid()) {
		const TypedArray<PlantSpecies> list = r_setup.other->biome->get_species();
		for (int i = 0; i < list.size(); i++) {
			const Ref<PlantSpecies> sp = list[i];
			if (sp.is_valid() && !sp->uses_forest_edge() && sp->get_canopy_radius() > 0.0f && !sp->uses_shade() && sp->get_density() > 0.0f) {
				job.other_canopy.push_back(sp.ptr());
			}
		}
	}
	if (!wanted || (job.canopy.is_empty() && job.other_canopy.is_empty()) || !r_setup.map || !r_setup.rect.has_area()) {
		return;
	}
	const Rect2 rect = r_setup.rect;
	const float cell = MAX(2.0f, MAX(rect.size.x, rect.size.y) / 511.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	LocalVector<uint8_t> in;
	in.resize(nx * ny);
	job.setup = &r_setup;
	job.rect = rect;
	job.nx = nx;
	job.ny = ny;
	job.in = in.ptr();
	WorkerThreadPool *wtp = WorkerThreadPool::get_singleton();
	wtp->wait_for_group_task_completion(wtp->add_native_group_task(&VegetationField3D::_edge_row, &job, ny, -1, true, "VegetationField3D forest edge"));
	bool any_in = false;
	bool any_out = false;
	for (uint8_t v : in) {
		any_in = any_in || v;
		any_out = any_out || !v;
	}
	if (!any_in) {
		return;
	}
	const float inf = 1e20f;
	LocalVector<float> to_out, to_in;
	to_out.resize(nx * ny);
	to_in.resize(nx * ny);
	for (int i = 0; i < nx * ny; i++) {
		to_out[i] = in[i] ? inf : 0.0f;
		to_in[i] = in[i] ? 0.0f : inf;
	}
	if (any_out) {
		edt_2d(to_out, nx, ny);
	}
	edt_2d(to_in, nx, ny);
	const float cx = rect.size.x / (nx - 1);
	r_setup.edge.resize(nx * ny);
	for (int i = 0; i < nx * ny; i++) {
		// The edge lies half a cell past the last forest node.
		r_setup.edge[i] = in[i] ? (any_out ? MIN((Math::sqrt(to_out[i]) - 0.5f) * cx, 1000.0f) : 1000.0f) : -MIN((Math::sqrt(to_in[i]) - 0.5f) * cx, 1000.0f);
	}
	r_setup.edge_nx = nx;
	r_setup.edge_ny = ny;
	r_setup.edge_rect = rect;
}

float VegetationField3D::PlaceSetup::edge_at(const Vector2 &p_xz) const {
	if (edge.is_empty()) {
		return -1000.0f;
	}
	const float gx = CLAMP((p_xz.x - edge_rect.position.x) / edge_rect.size.x * (edge_nx - 1), 0.0f, float(edge_nx - 1));
	const float gz = CLAMP((p_xz.y - edge_rect.position.y) / edge_rect.size.y * (edge_ny - 1), 0.0f, float(edge_ny - 1));
	const int ix = MIN(int(gx), edge_nx - 2), iz = MIN(int(gz), edge_ny - 2);
	const float fx = gx - ix, fz = gz - iz;
	const float *r0 = &edge[iz * edge_nx + ix];
	const float *r1 = r0 + edge_nx;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

Vector2 VegetationField3D::PlaceSetup::edge_open(const Vector2 &p_xz) const {
	if (edge.is_empty()) {
		return Vector2();
	}
	const float h = edge_rect.size.x / (edge_nx - 1);
	const Vector2 g(edge_at(p_xz + Vector2(h, 0.0f)) - edge_at(p_xz - Vector2(h, 0.0f)), edge_at(p_xz + Vector2(0.0f, h)) - edge_at(p_xz - Vector2(0.0f, h)));
	return g.length_squared() > 1e-8f ? -g.normalized() : Vector2();
}

// A species that grows only in a passing context (burns, a stand stage after a disturbance, forest edges): the maps
// made from rules alone (far shell, cover map, sward shade) leave it out, as they would spread it over all its ground.
bool VegetationField3D::_context_only(const PlantSpecies *p_sp) {
	if (p_sp->uses_burn_age()) {
		return true;
	}
	if (p_sp->uses_stand()) {
		Vector2 a, k;
		float af, kf;
		p_sp->get_stand_rules(a, af, k, kf);
		if (PlantSpecies::stand_weight(a, af, k, kf, -1.0f, 0) <= 0.0f) {
			return true;
		}
	}
	if (p_sp->uses_forest_edge()) {
		Vector2 r;
		float f;
		p_sp->get_forest_edge_rule(r, f);
		if (r.y < 10000.0f) {
			return true;
		}
	}
	if (p_sp->uses_browse() && p_sp->rule_weight(PlantSpecies::RULE_BROWSE, 0.0f) <= 0.0f) {
		return true; // Only where deer browse.
	}
	if (p_sp->uses_rooting() && PlantSpecies::rooting_weight(p_sp->get_rooting_rule(), -1.0f) <= 0.0f) {
		return true; // Only where boar rooted.
	}
	return false;
}

template <typename T>
static void _copy_vector(LocalVector<T> &r_to, const LocalVector<T> &p_from) {
	r_to.resize(p_from.size());
	for (uint32_t i = 0; i < p_from.size(); i++) {
		r_to[i] = p_from[i];
	}
}

void VegetationField3D::_copy_context(const PlaceSetup &p_setup) {
	_copy_vector(context.burn, p_setup.burn);
	context.burn_nx = p_setup.burn_nx;
	context.burn_ny = p_setup.burn_ny;
	context.burn_rect = p_setup.burn_rect;
	_copy_vector(context.stand_rules, p_setup.stand_rules);
	_copy_vector(context.stand_age, p_setup.stand_age);
	_copy_vector(context.stand_kind, p_setup.stand_kind);
	_copy_vector(context.browse_rules, p_setup.browse_rules);
	_copy_vector(context.stand_browse, p_setup.stand_browse);
	_copy_vector(context.rooting_rules, p_setup.rooting_rules);
	_copy_vector(context.stand_rooting, p_setup.stand_rooting);
	context.stand_nx = p_setup.stand_nx;
	context.stand_ny = p_setup.stand_ny;
	context.stand_rect = p_setup.stand_rect;
	_copy_vector(context.edge, p_setup.edge);
	context.edge_nx = p_setup.edge_nx;
	context.edge_ny = p_setup.edge_ny;
	context.edge_rect = p_setup.edge_rect;
	context_valid = true;
}

// How much a context-only species (_context_only) grows at a point by the last setup's grids, as placement weighs it:
// burn age, stand age and kind, forest edge. 0 without a setup.
float VegetationField3D::_context_weight(const PlantSpecies *p_sp, const Vector2 &p_xz) const {
	if (!context_valid) {
		return 0.0f;
	}
	float w = 1.0f;
	if (p_sp->uses_burn_age()) {
		w *= p_sp->rule_weight(PlantSpecies::RULE_BURN_AGE, context.burn_at(p_xz));
	}
	if (w > 0.0f && p_sp->uses_stand()) {
		Vector2 a, k;
		float af, kf;
		p_sp->get_stand_rules(a, af, k, kf);
		const int sc = context.stand_cell(p_xz);
		w *= PlantSpecies::stand_weight(a, af, k, kf, sc < 0 ? -1.0f : context.stand_age[sc], sc < 0 ? 0 : int(context.stand_kind[sc]));
	}
	if (w > 0.0f && p_sp->uses_browse()) {
		w *= p_sp->rule_weight(PlantSpecies::RULE_BROWSE, context.browse_cell(context.stand_cell(p_xz)));
	}
	if (w > 0.0f && p_sp->uses_rooting()) {
		w *= PlantSpecies::rooting_weight(p_sp->get_rooting_rule(), context.rooting_cell(context.stand_cell(p_xz)));
	}
	if (w > 0.0f && p_sp->uses_forest_edge()) {
		Vector2 r;
		float f;
		p_sp->get_forest_edge_rule(r, f);
		const float e = context.edge.is_empty() && r.y >= 10000.0f ? r.y : context.edge_at(p_xz);
		w *= f <= 0.0f ? ((e >= r.x && e <= r.y) ? 1.0f : 0.0f) : Math::smoothstep(float(r.x) - f, float(r.x), e) * (1.0f - Math::smoothstep(float(r.y), float(r.y) + f, e));
	}
	return w;
}

float VegetationField3D::PlaceSetup::burn_at(const Vector2 &p_xz) const {
	if (burn.is_empty()) {
		return -1.0f;
	}
	const int x = int(Math::round((p_xz.x - burn_rect.position.x) / burn_rect.size.x * (burn_nx - 1)));
	const int z = int(Math::round((p_xz.y - burn_rect.position.y) / burn_rect.size.y * (burn_ny - 1)));
	if (x < 0 || z < 0 || x >= burn_nx || z >= burn_ny) {
		return -1.0f;
	}
	return burn[z * burn_nx + x];
}

float VegetationField3D::PlaceSetup::mobility_at(const Vector2 &p_xz) const {
	if (mobility.is_empty()) {
		return -1.0f;
	}
	const float gx = (p_xz.x - mobility_rect.position.x) / mobility_rect.size.x * (mobility_nx - 1);
	const float gz = (p_xz.y - mobility_rect.position.y) / mobility_rect.size.y * (mobility_ny - 1);
	if (gx < 0.0f || gz < 0.0f || gx > mobility_nx - 1 || gz > mobility_ny - 1) {
		return -1.0f;
	}
	const int ix = MIN(int(gx), mobility_nx - 2), iz = MIN(int(gz), mobility_ny - 2);
	const float fx = gx - ix, fz = gz - iz;
	const float *r0 = &mobility[iz * mobility_nx + ix];
	const float *r1 = r0 + mobility_nx;
	// The edge of the sand: nearest, so -1 does not blend into the sand's values.
	if (MIN(MIN(r0[0], r0[1]), MIN(r1[0], r1[1])) < 0.0f) {
		return (fx < 0.5f ? (fz < 0.5f ? r0[0] : r1[0]) : (fz < 0.5f ? r0[1] : r1[1]));
	}
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

float VegetationField3D::PlaceSetup::sand_at(const Vector2 &p_xz) const {
	if (sand.is_empty() || !sand_rect.has_area()) {
		return 0.0f;
	}
	const float gx = (p_xz.x - sand_rect.position.x) / sand_rect.size.x * (sand_nx - 1);
	const float gz = (p_xz.y - sand_rect.position.y) / sand_rect.size.y * (sand_ny - 1);
	if (gx < 0.0f || gz < 0.0f || gx > sand_nx - 1 || gz > sand_ny - 1) {
		return 0.0f;
	}
	const int ix = MIN(int(gx), sand_nx - 2), iz = MIN(int(gz), sand_ny - 2);
	const float fx = gx - ix, fz = gz - iz;
	const float *r0 = &sand[iz * sand_nx + ix];
	const float *r1 = r0 + sand_nx;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

// Small items as real geometry out to where they are still detail_pixels tall (vegetation_detail.h, the rule the
// rock field's ground detail uses), so a fallen leaf or a twig is drawn as far as it can be seen, and no further.
float VegetationField3D::_species_range(const Ref<PlantSpecies> &p_species, real_t p_pixel_angle) const {
	if (p_species.is_null()) {
		return 0.0f;
	}
	const float hand = p_species->get_visibility_range();
	if (!detail_reach || hand <= 0.0f || p_species->get_detail_size() <= 0.0f) {
		return hand;
	}
	const float reach = float(VegetationDetail::reach(p_species->get_detail_size(), detail_pixels, p_pixel_angle));
	return MAX(hand, MIN(reach, detail_max_range));
}

float VegetationField3D::get_species_range(int p_species) const {
	if (biome.is_null() || p_species < 0 || p_species >= biome->get_species().size()) {
		return 0.0f;
	}
	return _species_range(biome->get_species()[p_species], VegetationDetail::pixel_angle(this, detail_reference_height));
}

// One species' placement setup (part of _make_setup; also run again for a species whose meshes grew, trees.md 5bc).
void VegetationField3D::_setup_species(PlaceSetup &r_setup, int s, const Ref<PlantSpecies> &p_sp, real_t pixel) {
	PlaceSpecies &ps = r_setup.species[s];
	ps.sp = p_sp;
	if (ps.sp.is_null()) {
		return;
	}
	ps.range = _species_range(ps.sp, pixel);
	if (streaming && far_shell && !ps.sp->get_meshes().is_empty()) {
		ps.far_albedo = _mesh_albedo(ps.sp->get_meshes()[0]);
	}
	// The far tail streams only (each tile's cells hold the ranks its distance keeps); small items keep their
	// pixel reach.
	ps.reach = streaming && far_reach > 1.0f && ps.range > 0.0f && ps.sp->get_detail_size() <= 0.0f ? ps.range * far_reach : ps.range;
	const TypedArray<Mesh> meshes = ps.sp->get_meshes();
	for (int i = 0; i < meshes.size(); i++) {
		ps.meshes.push_back(meshes[i]);
	}
	const TypedArray<Mesh> near = ps.sp->get_near_meshes();
	for (int i = 0; i < near.size(); i++) {
		ps.near_meshes.push_back(near[i]);
	}
	ps.pattern = _get_pattern(ps.sp->get_spacing(), s); // A copy: the cache may grow.
	ps.footprint = ps.sp->get_footprint();
	r_setup.max_footprint = MAX(r_setup.max_footprint, ps.footprint);
	const float canopy_r = ps.sp->get_canopy_radius();
	if (canopy_r <= 0.0f) {
		for (const Ref<Mesh> &m : ps.meshes) {
			if (m.is_valid()) {
				const AABB bb = m->get_aabb();
				ps.spread = MAX(ps.spread, MAX(MAX(Math::abs(bb.position.x), Math::abs(bb.get_end().x)), MAX(Math::abs(bb.position.z), Math::abs(bb.get_end().z))));
			}
		}
	}
	if (canopy_r > 0.0f && ps.meshes.size() > 0 && ps.meshes[0].is_valid()) {
		ps.canopy_top = ps.meshes[0]->get_aabb().get_end().y;
		ps.stem_foot = _foot_radius(ps.meshes[0], ps.footprint > 0.0f ? ps.footprint : 1.0f);
		ps.canopy_base = _crown_base(ps.meshes[0]);
	}
	ps.crown_max = canopy_r * MAX(ps.sp->get_scale_range().x, ps.sp->get_scale_range().y);
	r_setup.crown_max = MAX(r_setup.crown_max, ps.crown_max);
	// Trees shaped by the place (the trees module): flagged by the wind, and bank forms whose
	// ground falls towards mesh +Z.
	ps.wind_flag = bool(ps.sp->get_meta(SNAME("wind_flag"), false));
	{
		const Vector2 fd = ps.sp->get_meta(SNAME("fall_dir"), Vector2());
		ps.fall_dir = fd.length_squared() > 1e-8f ? fd.normalized() : Vector2();
		ps.fall_downhill = bool(ps.sp->get_meta(SNAME("fall_downhill"), false));
		ps.fall_scatter = float(ps.sp->get_meta(SNAME("fall_scatter"), 0.0f));
		ps.fall_water = float(ps.sp->get_meta(SNAME("fall_to_water"), 0.0f));
	}
	// Forest edge trees (trees' make_edge): low branches on one side, turned to face the open.
	ps.open_side = Vector2();
	for (const Ref<Mesh> &m : ps.meshes) {
		if (m.is_valid() && m->has_meta(SNAME("tree_open_side"))) {
			const Vector3 o = m->get_meta(SNAME("tree_open_side"));
			const Vector2 o2(o.x, o.z);
			ps.open_side = o2.length_squared() > 1e-8f ? o2.normalized() : Vector2();
			break;
		}
	}
	ps.far_widen = bool(ps.sp->get_meta(SNAME("far_widen"), false));
	ps.far_hull_range = float(ps.sp->get_meta(SNAME("far_hull_range"), 0.0f));
	ps.clone_scale = float(ps.sp->get_meta(SNAME("clone_scale"), 0.0f));
	if (ps.far_widen && bool(ps.sp->get_meta(SNAME("far_skyline"), true)) && !ps.meshes.is_empty() && ps.meshes[0].is_valid()) {
		// Meta far_skyline = false: thinned on the skyline too.
		ps.far_top = ps.meshes[0]->get_aabb().get_end().y;
	}
	{
		bool any_bank = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_bank = any_bank || (m.is_valid() && m->has_meta(SNAME("tree_ground_slope")));
		}
		if (any_bank) {
			for (const Ref<Mesh> &m : ps.meshes) {
				const Vector2 gs = m.is_valid() ? Vector2(m->get_meta(SNAME("tree_ground_slope"), Vector2())) : Vector2();
				ps.bank_slopes.push_back(gs.x);
			}
		}
	}
	{
		// Cut when a hay meadow is mown: the plant generator's herbs that stand over the stubble (by preset, or by the
		// plant shader on a hand-made species' meshes; not trees or shrubs with a canopy, mounds, dung, wrack or bank sod).
		float top = 0.0f;
		for (const Ref<Mesh> &m : ps.meshes) {
			if (m.is_valid()) {
				top = MAX(top, m->get_aabb().get_end().y);
			}
		}
		const int preset = int(p_sp->get_meta(SNAME("plant_preset"), -1));
		const bool herb = preset >= 0 ? !PlantGenerator::is_ground_feature(preset) : (!ps.meshes.is_empty() && PlantGenerator::is_plant_mesh(ps.meshes[0]));
		bool mound = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			mound = mound || (m.is_valid() && m->has_meta(SNAME("sward_mound")));
		}
		ps.hay_cut = herb && !mound && p_sp->get_canopy_radius() <= 0.0f && !p_sp->uses_bank_edge() && top > 0.12f;
		// Bare-ground species (crusts, desert annuals, those tagged bare_ground) live between the clumps; the rest are
		// the clumps.
		ps.bare_cover = herb && !mound && p_sp->get_canopy_radius() <= 0.0f && preset != PlantGenerator::PRESET_SOIL_CRUST && preset != PlantGenerator::PRESET_DESERT_ANNUALS && !bool(p_sp->get_meta(SNAME("bare_ground"), false));
		ps.base_variants = int(ps.meshes.size());
		if (ps.hay_cut) {
			// Always, so a meadow mown later needs no new setup. The same plants cut at the stubble (7.5 to 10 cm; GrassField3D.hay_meadows): their stalks stand in the
			// mown meadow until they grow back.
			for (int i = 0; i < ps.base_variants; i++) {
				Ref<ArrayMesh> cut = _cut_mesh(ps.meshes[i], 0.085f);
				cut->set_meta(SNAME("hay_cut_of"), ps.meshes[i]);
				ps.meshes.push_back(cut);
			}
		}
	}
	{
		// Bank roots (PlantGenerator.make_bank_roots): per variant, the face angle it was grown on.
		bool any_face = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_face = any_face || (m.is_valid() && m->has_meta(SNAME("bank_face_slope")));
		}
		if (any_face) {
			for (const Ref<Mesh> &m : ps.meshes) {
				ps.face_slopes.push_back(m.is_valid() ? float(m->get_meta(SNAME("bank_face_slope"), 0.0f)) : 0.0f);
			}
		}
	}
	{
		// Stilt-rooted trees (trees' wet forests): the mesh origin is the stem's foot on its tussock, the ground at
		// the root tips lies tree_base_lift below it.
		bool any_lift = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_lift = any_lift || (m.is_valid() && m->has_meta(SNAME("tree_base_lift")));
		}
		if (any_lift) {
			for (const Ref<Mesh> &m : ps.meshes) {
				ps.base_lifts.push_back(m.is_valid() ? float(m->get_meta(SNAME("tree_base_lift"), 0.0f)) : 0.0f);
			}
		}
	}
	{
		bool any_fire = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_fire = any_fire || (m.is_valid() && m->has_meta(SNAME("tree_fire_years")));
		}
		if (any_fire) {
			bool any_band = false;
			for (const Ref<Mesh> &m : ps.meshes) {
				const float fy = (m.is_valid() && m->has_meta(SNAME("tree_fire_years"))) ? float(m->get_meta(SNAME("tree_fire_years"))) : -1.0f;
				ps.fire_years.push_back(fy);
				ps.fire_years_max = MAX(ps.fire_years_max, fy);
				any_band = any_band || (m.is_valid() && m->has_meta(SNAME("tree_fire_band")));
			}
			if (any_band) {
				for (const Ref<Mesh> &m : ps.meshes) {
					ps.fire_bands.push_back((m.is_valid() && m->has_meta(SNAME("tree_fire_band"))) ? Vector2(m->get_meta(SNAME("tree_fire_band"))) : Vector2(-1.0f, -1.0f));
				}
			}
		}
	}
	{
		// Struck variants (trees.md 5ax): their years since the strike, -1 for the others.
		bool any_strike = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_strike = any_strike || (m.is_valid() && m->has_meta(SNAME("tree_lightning_years")));
		}
		ps.strike_years.clear();
		ps.strike_years_max = 0.0f;
		if (any_strike) {
			for (const Ref<Mesh> &m : ps.meshes) {
				const float sy = (m.is_valid() && m->has_meta(SNAME("tree_lightning_years"))) ? float(m->get_meta(SNAME("tree_lightning_years"))) : -1.0f;
				ps.strike_years.push_back(sy);
				ps.strike_years_max = MAX(ps.strike_years_max, sy);
			}
		}
		// Flood-marked variants (trees.md 5av): their silt line's height and age, unflooded ones (0, -1).
		ps.flood_marks.clear();
		bool any_flood = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_flood = any_flood || (m.is_valid() && m->has_meta(SNAME("tree_flood")));
		}
		if (any_flood) {
			for (const Ref<Mesh> &m : ps.meshes) {
				ps.flood_marks.push_back((m.is_valid() && m->has_meta(SNAME("tree_flood"))) ? Vector2(m->get_meta(SNAME("tree_flood"))) : Vector2(0.0f, -1.0f));
			}
		}
		// Browsed variants (trees.md 5az): marked by mesh meta tree_browse.
		ps.browsed.clear();
		bool any_browse = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			any_browse = any_browse || (m.is_valid() && m->has_meta(SNAME("tree_browse")));
		}
		if (any_browse) {
			for (const Ref<Mesh> &m : ps.meshes) {
				ps.browsed.push_back((m.is_valid() && m->has_meta(SNAME("tree_browse"))) ? 1 : 0);
			}
		}
	}
	ps.frontal = ps.sp->get_wind_frontal_area();
	if (ps.frontal < 0.0f) {
		ps.frontal = 0.0f;
		if (ps.meshes.size() > 0 && ps.meshes[0].is_valid()) {
			// A tuft's leaves hide about 0.4 of its outline (optical porosity 0.6, grasses).
			const AABB bb = ps.meshes[0]->get_aabb();
			ps.frontal = 0.4f * MAX(bb.get_end().y, 0.0f) * 0.5f * (bb.size.x + bb.size.z);
		}
	}
	const Color far_srgb = ps.sp->get_far_color();
	ps.far_color = Color(far_srgb.r, far_srgb.g, far_srgb.b).srgb_to_linear() * Color(1, 1, 1, 0) + Color(0, 0, 0, far_srgb.a);
	// Fallen wood from the trees module: each mesh's axis (docs/ideas/forest-floor.md, 6.6).
	if (bool(ps.sp->get_meta(SNAME("deadwood"), false))) {
		const float smax = MAX(ps.sp->get_scale_range().x, ps.sp->get_scale_range().y);
		for (const Ref<Mesh> &m : ps.meshes) {
			PackedFloat32Array axis;
			if (m.is_valid() && m->has_meta(SNAME("tree_deadwood_axis"))) {
				axis = m->get_meta(SNAME("tree_deadwood_axis"));
			}
			if (axis.size() < 7) {
				axis.clear();
			} else {
				const float reach = MAX(Vector3(axis[0], axis[1], axis[2]).length(), Vector3(axis[3], axis[4], axis[5]).length()) + axis[6];
				ps.deadwood_reach = MAX(ps.deadwood_reach, reach * smax);
			}
			ps.deadwood_axes.push_back(axis);
			ps.deadwood_features.push_back(m.is_valid() && m->has_meta(SNAME("tree_deadwood_features")) ? PackedFloat32Array(m->get_meta(SNAME("tree_deadwood_features"))) : PackedFloat32Array());
			ps.deadwood_pits.push_back(m.is_valid() && m->has_meta(SNAME("tree_root_pit")) ? PackedFloat32Array(m->get_meta(SNAME("tree_root_pit"))) : PackedFloat32Array());
		}
	}
	// A stone under the roots (trees.md 5q): the floor keeps clear of it as of a root pit. The
	// bank shear only lifts the mesh, so its footprint across the ground holds.
	{
		const float smax = MAX(ps.sp->get_scale_range().x, ps.sp->get_scale_range().y);
		bool any_stone = false;
		for (const Ref<Mesh> &m : ps.meshes) {
			PackedFloat32Array st;
			if (m.is_valid() && m->has_meta(SNAME("tree_root_stone"))) {
				st = m->get_meta(SNAME("tree_root_stone"));
			}
			if (st.size() >= 6) {
				any_stone = true;
				ps.stone_reach = MAX(ps.stone_reach, (Vector2(st[0], st[2]).length() + MAX(st[3], st[5]) + ROOT_STONE_MARGIN) * smax);
			} else {
				st.clear();
			}
			ps.root_stones.push_back(st);
		}
		if (!any_stone) {
			ps.root_stones.clear();
		}
	}
	if (ps.sp->get_deadwood_rule() == PlantSpecies::DEADWOOD_BESIDE) {
		r_setup.deadwood_reach = MAX(r_setup.deadwood_reach, ps.sp->get_deadwood_band());
	}
}

bool VegetationField3D::_make_setup(PlaceSetup &r_setup) {
	hay_seen = GrassField3D::get_hay_generation(); // Placed with the meadows as they are now.
	_update_bark_detail(); // The species are final by now.
	r_setup = PlaceSetup();
	ERR_FAIL_COND_V_MSG(biome.is_null(), false, "VegetationField3D: no biome.");
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	ERR_FAIL_COND_V_MSG(!map || !map->is_baked(), false, "VegetationField3D: data_map must point to a baked TerrainDataMap3D.");
	r_setup.map = map;
	r_setup.rect = map->get_map_rect();
	r_setup.to_local = get_global_transform().affine_inverse();
	r_setup.global_xf = get_global_transform();
	r_setup.tile_size = tile_size;
	r_setup.seed = seed;
	// The density mask, read once on the CPU (bilinear, clamped to its rect; outside it a species
	// with a channel grows as if the mask were 1).
	if (density_mask.is_valid() && density_mask_rect.has_area()) {
		r_setup.mask_img = density_mask->get_image();
		if (r_setup.mask_img.is_valid() && r_setup.mask_img->is_compressed()) {
			r_setup.mask_img = r_setup.mask_img->duplicate();
			r_setup.mask_img->decompress();
		}
		r_setup.mask_rect = density_mask_rect;
	}
	r_setup.obstacles = obstacle_grid;
	r_setup.rocks_any = !obstacle_grid.is_empty() || _get_rock_field() != nullptr;
	r_setup.paths = path_grid;
	r_setup.crop_fields = GrassField3D::get_crop_field_mask();
	_follow_holes(map);
	r_setup.holes.read(VegetationHoles::source_of(map));
	r_setup.removals = plant_removals;
	{
		const TerrainGenerator3D *gen = VegetationHoles::source_of(map);
		r_setup.dunes = gen && gen->get_dune_rect().has_area() ? gen : nullptr;
		r_setup.coast = gen && gen->has_sea_result() ? gen : nullptr;
		r_setup.sea_y = gen ? float(gen->get_sea_level() + gen->get_height_offset()) : 0.0f;
		_read_sand(gen, r_setup);
		// Every field on a sand sea reads it: sand plants by their mobility rule, and the rest to keep off the sand.
		_read_sand_mobility(gen, r_setup);
		// Oases at desert springs, for the species that use them.
		bool spring_rule = false;
		const TypedArray<PlantSpecies> sl = biome->get_species();
		for (int i = 0; i < sl.size() && !spring_rule; i++) {
			const Ref<PlantSpecies> sp = sl[i];
			spring_rule = sp.is_valid() && sp->uses_spring();
		}
		if (spring_rule) {
			r_setup.springs.build(gen);
		}
		springs_seen = r_setup.springs;
		// Nebkhas: the sand mounds desert shrubs build (terrain-gen; docs/ideas/vegetation.md, "Nebkhas").
		if (gen && gen->get_nebkha_density() > 0.0f) {
			r_setup.nebkhas = gen->get_nebkhas(r_setup.rect);
			for (int i = 0; i < r_setup.nebkhas.size(); i++) {
				const Vector4 n = r_setup.nebkhas[i];
				r_setup.nebkha_grid[Vector2i(int(Math::floor(n.x / NEBKHA_CELL)), int(Math::floor(n.z / NEBKHA_CELL)))].push_back(i);
			}
		}
	}
	// Oasis spring pools (OasisSprings3D, the water thread's): their shores, for the species that grow round them.
	r_setup.pools.clear();
	r_setup.pool_bounds.clear();
	if (!spring_pools.is_empty() && is_inside_tree()) {
		Node *op = get_node_or_null(spring_pools);
		if (op && op->has_method("get_pool_count") && op->has_method("get_outline")) {
			const int count = op->call("get_pool_count");
			for (int i = 0; i < count; i++) {
				const PackedVector2Array outline = op->call("get_outline", i);
				if (outline.size() < 3) {
					continue;
				}
				Rect2 b(outline[0], Vector2());
				for (const Vector2 &q : outline) {
					b.expand_to(q);
				}
				r_setup.pools.push_back(outline);
				r_setup.pool_bounds.push_back(b);
			}
		}
	}
	_read_burn_age(r_setup);
	_read_stand(r_setup);
	_realize_variants(r_setup, true);
	r_setup.stone_tops = stone_top_grid;
	r_setup.stone_top_normals = stone_top_grid_normals;
	// Another field's crowns (canopy_field): a camera-local herb layer under a world forest.
	r_setup.other = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
	if (r_setup.other == this) {
		r_setup.other = nullptr;
	}
	// Leaf drift downwind: litter_wind, or else the weather's wind (autumn gales carry leaves
	// furthest; 10 m/s leans the drift by half).
	Vector2 skew = litter_wind;
	if (skew == Vector2() && !weather_map.is_empty()) {
		const WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
		if (wm && wm->get_wind_direction() != Vector2()) {
			skew = wm->get_wind_direction().normalized() * (0.05f * wm->get_wind_speed());
		}
	}
	r_setup.litter_skew = skew.limit_length(0.8f);
	r_setup.wind_dir = skew.length_squared() > 1e-8f ? skew.normalized() : Vector2();
	r_setup.litter_reach = 3.0f * LITTER_SPREAD * (1.0f + r_setup.litter_skew.length());
	litter_skew = r_setup.litter_skew;
	const TypedArray<PlantSpecies> species_list = biome->get_species();
	const int count = species_list.size();
	r_setup.species.resize(count);
	const real_t pixel = VegetationDetail::pixel_angle(this, detail_reference_height);
	for (int s = 0; s < count; s++) {
		_setup_species(r_setup, s, species_list[s], pixel);
	}
	// Halo: how far around a tile each species must be placed for the tile to come out exactly
	// as in one placement of the whole map. Only earlier species act on later ones (footprints
	// keep later plants out, crowns shade them), so walking back from the last species each
	// one's halo is the furthest any later species, itself placed over its own halo, reaches it.
	for (int s = count - 1; s >= 0; s--) {
		PlaceSpecies &ps = r_setup.species[s];
		if (ps.sp.is_null()) {
			continue;
		}
		float halo = 0.0f;
		for (int s2 = s + 1; s2 < count; s2++) {
			const PlaceSpecies &later = r_setup.species[s2];
			if (later.sp.is_null()) {
				continue;
			}
			if (ps.footprint + later.footprint > 0.0f) {
				halo = MAX(halo, later.halo + ps.footprint + later.footprint);
			}
			if (ps.crown_max > 0.0f && (later.sp->uses_shade() || later.sp->uses_conifer())) {
				halo = MAX(halo, later.halo + ps.crown_max * 1.4f);
			}
			if (ps.crown_max > 0.0f && later.sp->uses_litter()) {
				halo = MAX(halo, later.halo + ps.crown_max + r_setup.litter_reach);
			}
			if (ps.stone_reach > 0.0f) {
				halo = MAX(halo, later.halo + ps.stone_reach);
			}
			if (ps.deadwood_reach > 0.0f && later.sp->get_deadwood_rule() != PlantSpecies::DEADWOOD_IGNORE) {
				halo = MAX(halo, later.halo + ps.deadwood_reach + later.sp->get_deadwood_band());
			}
		}
		ps.halo = halo;
	}
	r_setup.grid_cell = MAX(1.0f, r_setup.max_footprint * 2.0f);
	r_setup.sun_mean = VegetationLight::sun_mean(map);
	// Nebkha shrubs: each mound is crowned by one of them, picked by the mound's hash.
	for (int s = 0; s < count; s++) {
		PlaceSpecies &ps = r_setup.species[s];
		ps.colonnade = ps.sp.is_valid() ? Vector2(ps.sp->get_meta("colonnade", Vector2())) : Vector2();
		if (ps.colonnade.x <= 0.0f || ps.colonnade.y <= 0.0f) {
			ps.colonnade = Vector2();
		}
		if (ps.sp.is_valid() && bool(ps.sp->get_meta("nebkha", false))) {
			ps.nebkha_slot = r_setup.nebkha_count++;
		} else if (ps.sp.is_valid() && bool(ps.sp->get_meta("desert", false)) && bool(ps.sp->get_meta("woody_shrub", false)) && !bool(ps.sp->get_meta("oasis", false))) {
			ps.desert_shrub = true;
		}
	}
	// How far past a tile's edge its placements read the rocks: the halo they are placed in, and the widest rock query
	// of _place() (a rock foot's shelter, the gap between stones, a stem or log's clearance, a ground plant's leaves,
	// the leaf drift at a drift cell's centre).
	float nebkha_h = 0.0f;
	for (int i = 0; i < r_setup.nebkhas.size(); i++) {
		nebkha_h = MAX(nebkha_h, float(r_setup.nebkhas[i].w));
	}
	for (const PlaceSpecies &ps : r_setup.species) {
		if (ps.sp.is_null()) {
			continue;
		}
		const float smax = MAX(ps.sp->get_scale_range().x, ps.sp->get_scale_range().y);
		float q = MAX(4.0f * PlantSpecies::ROCK_FOOT_REACH, 6.0f * PlantSpecies::STONE_GAP_REACH);
		q = MAX(q, DRIFT_CELL * float(Math::SQRT2) + 1.6f);
		q = MAX(q, ps.deadwood_reach + OBSTACLE_MARGIN);
		q = MAX(q, ps.stem_foot * smax + OBSTACLE_MARGIN);
		q = MAX(q, ps.spread * smax);
		if (ps.nebkha_slot >= 0) {
			q = MAX(q, NEBKHA_CROWN * NEBKHA_RADIUS * nebkha_h * 1.1f); // A mound's shrub, scaled to its crown.
		}
		r_setup.rock_reach = MAX(r_setup.rock_reach, ps.halo + q);
	}
	r_setup.rock_reach += 1.0f;
	_read_forest_edge(r_setup);
	return true;
}

bool VegetationField3D::PlaceSetup::near_nebkha(const Vector2 &p_xz) const {
	if (nebkhas.is_empty()) {
		return false;
	}
	const int cx = int(Math::floor(p_xz.x / NEBKHA_CELL)), cz = int(Math::floor(p_xz.y / NEBKHA_CELL));
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const LocalVector<int> *cell = nebkha_grid.getptr(Vector2i(cx + dx, cz + dz));
			if (!cell) {
				continue;
			}
			for (int i : *cell) {
				const Vector4 n = nebkhas[i];
				const float reach = NEBKHA_RADIUS * n.w + NEBKHA_CLEAR;
				if (Vector2(n.x - p_xz.x, n.z - p_xz.y).length_squared() < reach * reach) {
					return true;
				}
			}
		}
	}
	return false;
}

namespace {

// Shade from a list of crowns in a hash grid (see VegetationField3D::_shade_at()).
template <typename Grid, typename Crowns>
float crown_shade(const Grid &p_grid, const Crowns &p_crowns, float p_reach, float p_cell, const Vector2 &p_xz, float *r_conifer) {
	if (r_conifer) {
		*r_conifer = 0.0f;
	}
	if (p_crowns.is_empty()) {
		return 0.0f;
	}
	const int r = int(Math::ceil(p_reach / p_cell));
	const Vector2i gc(int(Math::floor(p_xz.x / p_cell)), int(Math::floor(p_xz.y / p_cell)));
	float transmit = 1.0f;
	float needles = 0.0f;
	float weight = 0.0f;
	for (int dz = -r; dz <= r; dz++) {
		for (int dx = -r; dx <= r; dx++) {
			const LocalVector<int> *bucket = p_grid.getptr(gc + Vector2i(dx, dz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const auto &c = p_crowns[idx];
				const float d = c.xz.distance_to(p_xz) / c.radius;
				if (d >= 1.4f) {
					continue;
				}
				const float block = c.opacity * (1.0f - Math::smoothstep(0.6f, 1.4f, d));
				transmit *= 1.0f - block;
				// The stand type: each crown's share of the cover overhead.
				needles += block * c.conifer;
				weight += block;
			}
		}
	}
	if (r_conifer && weight > 0.0f) {
		*r_conifer = needles / weight;
	}
	return 1.0f - transmit;
}

// Whether a point lies within a stem's foot (Crown::stem, plus a margin), from crowns in a hash
// grid of CROWN_CELL cells; feet are under 2 m across, so the 3 x 3 cells round the point hold them.
template <typename Grid, typename Crowns>
bool stem_in(const Grid &p_grid, const Crowns &p_crowns, const Vector2 &p_xz, float p_margin, float p_cell) {
	if (p_crowns.is_empty()) {
		return false;
	}
	const Vector2i gc(int(Math::floor(p_xz.x / p_cell)), int(Math::floor(p_xz.y / p_cell)));
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const LocalVector<int> *bucket = p_grid.getptr(gc + Vector2i(dx, dz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const auto &c = p_crowns[idx];
				const float r = c.stem + p_margin;
				if (c.stem > 0.0f && c.xz.distance_squared_to(p_xz) < r * r) {
					return true;
				}
			}
		}
	}
	return false;
}

// Leaf fall from the crowns (docs/ideas/forest-floor.md, step 6, "Where it lands"): every crown
// drops its leaves under itself and, with an exponential tail (Ferrari and Sugita 1996), past its
// edge over LITTER_SPREAD metres, so edges and clearings get a drift that thins outwards. Summed
// as a deposit and turned into cover: about 1 under closed crowns.
// p_skew leans the tail downwind (its direction, the way the wind blows toward; its length k makes
// the drift reach 1 + k times as far downwind and 1 - k times as far upwind).
template <typename Grid, typename Crowns>
float crown_litter(const Grid &p_grid, const Crowns &p_crowns, float p_reach, float p_cell, const Vector2 &p_xz, const Vector2 &p_skew) {
	if (p_crowns.is_empty()) {
		return 0.0f;
	}
	const int r = int(Math::ceil(p_reach / p_cell));
	const Vector2i gc(int(Math::floor(p_xz.x / p_cell)), int(Math::floor(p_xz.y / p_cell)));
	float deposit = 0.0f;
	for (int dz = -r; dz <= r; dz++) {
		for (int dx = -r; dx <= r; dx++) {
			const LocalVector<int> *bucket = p_grid.getptr(gc + Vector2i(dx, dz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const auto &c = p_crowns[idx];
				const Vector2 to = p_xz - c.xz;
				const float dist = to.length();
				const float beyond = dist - c.radius;
				if (beyond <= 0.0f) {
					deposit += c.opacity * (1.0f - c.conifer);
					continue;
				}
				const float spread = VegetationField3D::LITTER_SPREAD * (1.0f + to.dot(p_skew) / dist);
				if (beyond > 3.0f * spread) {
					continue;
				}
				deposit += c.opacity * (1.0f - c.conifer) * Math::exp(-beyond / spread);
			}
		}
	}
	return 1.0f - Math::exp(-3.0f * deposit);
}

} // namespace

// Places the species over the tiles of p_tiles (tile coordinates), in priority order. With
// p_halo each species is also placed around them as far as later species need it (its halo),
// so the tiles come out as they would in one placement of the whole map; plants outside
// p_tiles are not returned. Reads only p_setup and the data map: safe on a worker thread.
namespace {

// Logs in a hash grid of DEADWOOD_CELL cells: each in every cell its footprint overlaps.
template <typename Grid, typename Log>
void deadwood_insert(Grid &r_grid, const Log &p_log, int p_index, float p_cell) {
	const float x0 = MIN(p_log.a.x, p_log.b.x) - p_log.radius, x1 = MAX(p_log.a.x, p_log.b.x) + p_log.radius;
	const float z0 = MIN(p_log.a.z, p_log.b.z) - p_log.radius, z1 = MAX(p_log.a.z, p_log.b.z) + p_log.radius;
	for (int cz = int(Math::floor(z0 / p_cell)); cz <= int(Math::floor(z1 / p_cell)); cz++) {
		for (int cx = int(Math::floor(x0 / p_cell)); cx <= int(Math::floor(x1 / p_cell)); cx++) {
			r_grid[Vector2i(cx, cz)].push_back(p_index);
		}
	}
}

// The log whose side is nearest the point (within p_reach), seen from above: a log is a cylinder
// round its axis, so over its footprint the upper surface stands sqrt(r^2 - d^2) over the axis.
template <typename Grid, typename Logs, typename Hit>
// p_min_radius: only wood at least that thick (a plant beside logs, not the sticks round them).
bool deadwood_nearest(const Grid &p_grid, const Logs &p_logs, const Vector2 &p_xz, float p_reach, float p_cell, Hit &r_hit, float p_min_radius = 0.0f) {
	if (p_logs.is_empty()) {
		return false;
	}
	bool found = false;
	const int cx0 = int(Math::floor((p_xz.x - p_reach) / p_cell)), cx1 = int(Math::floor((p_xz.x + p_reach) / p_cell));
	const int cz0 = int(Math::floor((p_xz.y - p_reach) / p_cell)), cz1 = int(Math::floor((p_xz.y + p_reach) / p_cell));
	for (int cz = cz0; cz <= cz1; cz++) {
		for (int cx = cx0; cx <= cx1; cx++) {
			const auto *bucket = p_grid.getptr(Vector2i(cx, cz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const auto &d = p_logs[idx];
				if (d.pit || d.feature >= 0 || d.radius < p_min_radius) {
					continue;
				}
				const Vector2 a(d.a.x, d.a.z), ab = Vector2(d.b.x, d.b.z) - a;
				const float len2 = ab.length_squared();
				const float t = len2 > 1e-8f ? CLAMP((p_xz - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
				const Vector2 off = p_xz - (a + ab * t);
				const float dist = off.length();
				const float side = dist - d.radius;
				if (side > p_reach || side >= r_hit.side) {
					continue;
				}
				found = true;
				r_hit.side = side;
				r_hit.radius = d.radius;
				r_hit.across = dist > 1e-5f ? off / dist : Vector2();
				const Vector3 c = d.a.lerp(d.b, t);
				r_hit.axis_y = c.y;
				const float h = d.radius * d.radius - dist * dist;
				if (h > 0.0f) {
					const float up = Math::sqrt(h);
					r_hit.top = c.y + up;
					r_hit.normal = Vector3(off.x, up, off.y) / MAX(d.radius, 1e-4f);
				} else {
					r_hit.top = c.y;
					r_hit.normal = Vector3(0, 1, 0);
				}
			}
		}
	}
	return found;
}

// The nearest feature (Deadwood.feature: a crack or bark edge on a log's surface) to a point, across
// the ground within p_reach, at p_low to p_high over p_ground and facing out sideways (not the top);
// r_any: whether there are features in reach at all.
template <typename Grid, typename Logs>
bool deadwood_feature_nearest(const Grid &p_grid, const Logs &p_logs, const Vector3 &p_pos, float p_reach, float p_low, float p_high, float p_ground, float p_cell, Vector3 &r_point, Vector3 &r_normal, bool &r_any, float &r_best) {
	bool found = false;
	const int cx0 = int(Math::floor((p_pos.x - p_reach) / p_cell)), cx1 = int(Math::floor((p_pos.x + p_reach) / p_cell));
	const int cz0 = int(Math::floor((p_pos.z - p_reach) / p_cell)), cz1 = int(Math::floor((p_pos.z + p_reach) / p_cell));
	for (int cz = cz0; cz <= cz1; cz++) {
		for (int cx = cx0; cx <= cx1; cx++) {
			const auto *bucket = p_grid.getptr(Vector2i(cx, cz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const auto &d = p_logs[idx];
				if (d.feature < 0) {
					continue;
				}
				const float dist = Vector2(d.a.x - p_pos.x, d.a.z - p_pos.z).length();
				if (dist > p_reach) {
					continue;
				}
				r_any = true;
				const Vector3 n = (d.b - d.a).normalized();
				const float over = d.a.y - p_ground;
				if (over < p_low || over > p_high || n.y > 0.75f || Vector2(n.x, n.z).length_squared() < 0.04f) {
					continue;
				}
				// Nearest, and a check a little before a peel edge (a crack splits the wood open).
				const float score = dist + (d.feature == 1 ? 0.0f : 0.05f);
				if (score < r_best) {
					r_best = score;
					r_point = d.a;
					r_normal = n;
					found = true;
				}
			}
		}
	}
	return found;
}

// Whether the point lies in a root pit (Deadwood.pit) of the list.
template <typename Grid, typename Logs>
bool deadwood_pit(const Grid &p_grid, const Logs &p_logs, const Vector2 &p_xz, float p_cell) {
	const auto *bucket = p_grid.getptr(Vector2i(int(Math::floor(p_xz.x / p_cell)), int(Math::floor(p_xz.y / p_cell))));
	if (!bucket) {
		return false;
	}
	for (int idx : *bucket) {
		const auto &d = p_logs[idx];
		if (d.pit && Vector2(p_xz.x - d.a.x, p_xz.y - d.a.z).length_squared() < d.radius * d.radius) {
			return true;
		}
	}
	return false;
}

} // namespace

bool VegetationField3D::_pit_at(const Vector2 &p_xz) const {
	RWLockRead guard(crown_lock);
	return deadwood_pit(deadwood_grid, deadwood, p_xz, DEADWOOD_CELL);
}

int VegetationField3D::get_deadwood_count() const {
	RWLockRead guard(crown_lock);
	int n = 0;
	for (const Deadwood &d : deadwood) {
		n += d.feature < 0 && !d.stone ? 1 : 0; // Logs and pits, not the places on them nor root stones.
	}
	return n;
}

// The placed log whose side is nearest the point, within reach: side (m from its side across the
// ground, negative under it), radius, top (height of its upper surface there) and normal; empty
// when there is none.
Dictionary VegetationField3D::get_deadwood_at(const Vector3 &p_global_position, float p_reach, float p_min_radius) const {
	DeadwoodHit hit;
	Dictionary d;
	if (_deadwood_at(Vector2(p_global_position.x, p_global_position.z), MAX(p_reach, 0.0f), hit, MAX(p_min_radius, 0.0f))) {
		d["side"] = hit.side;
		d["radius"] = hit.radius;
		d["top"] = hit.top;
		d["normal"] = hit.normal;
		d["across"] = Vector3(hit.across.x, 0.0f, hit.across.y);
	}
	return d;
}

bool VegetationField3D::_deadwood_at(const Vector2 &p_xz, float p_reach, DeadwoodHit &r_hit, float p_min_radius) const {
	RWLockRead guard(crown_lock);
	return deadwood_nearest(deadwood_grid, deadwood, p_xz, p_reach, DEADWOOD_CELL, r_hit, p_min_radius);
}

bool VegetationField3D::_deadwood_feature_at(const Vector3 &p_pos, float p_reach, float p_low, float p_high, float p_ground, Vector3 &r_point, Vector3 &r_normal, bool &r_any) const {
	RWLockRead guard(crown_lock);
	float best = 1e9f;
	return deadwood_feature_nearest(deadwood_grid, deadwood, p_pos, p_reach, p_low, p_high, p_ground, DEADWOOD_CELL, r_point, r_normal, r_any, best);
}

// The placed logs, for other fields' plants (canopy_field) and get_deadwood_at().
void VegetationField3D::_publish_deadwood(const LocalVector<const LocalVector<Deadwood> *> &p_lists) {
	RWLockWrite guard(crown_lock);
	deadwood.clear();
	deadwood_grid.clear();
	deadwood_max_radius = 0.0f;
	for (const LocalVector<Deadwood> *list : p_lists) {
		for (const Deadwood &d : *list) {
			deadwood.push_back(d);
			deadwood_insert(deadwood_grid, d, deadwood.size() - 1, DEADWOOD_CELL);
			deadwood_max_radius = MAX(deadwood_max_radius, d.radius);
		}
	}
}

void VegetationField3D::_place(const PlaceSetup &p_setup, const Rect2i &p_tiles, bool p_halo, LocalVector<TileOut> &r_tiles, const VegetationObstacles *p_rocks) const {
	const VegetationObstacles &rocks = p_rocks ? *p_rocks : p_setup.obstacles;
	// The climate's bare share (WorldContracts::climate_bare_share_at) on 32 m cells: it follows the climate map's
	// slow change, the patches themselves come from bare_patch at each plant.
	HashMap<Vector2i, float> bare_cells;
	const int species_count = p_setup.species.size();
	const float ts = p_setup.tile_size;
	const Rect2 &rect = p_setup.rect;
	TerrainDataMap3D *map = p_setup.map;
	r_tiles.resize(p_tiles.size.x * p_tiles.size.y);
	for (int i = 0; i < int(r_tiles.size()); i++) {
		TileOut &t = r_tiles[i];
		t.tile = p_tiles.position + Vector2i(i % p_tiles.size.x, i / p_tiles.size.x);
		t.species.resize(species_count);
	}
	const Rect2 core(Vector2(p_tiles.position) * ts, Vector2(p_tiles.size) * ts);
	// Footprints of placed plants and the crowns placed so far, in hash grids.
	const float grid_cell = p_setup.grid_cell;
	HashMap<Vector2i, LocalVector<int>> grid;
	LocalVector<Placed> placed;
	LocalVector<Crown> local_crowns;
	HashMap<Vector2i, LocalVector<int>> local_crown_grid;
	LocalVector<Deadwood> local_deadwood; // Logs placed so far (and the canopy field's, through _deadwood_at()).
	HashMap<Vector2i, LocalVector<int>> local_deadwood_grid;
	const float crown_reach = p_setup.crown_max * 1.4f;
	// Leaf drift (_litter_drift), cached on DRIFT_CELL cells as the litter species ask for it (they come after the trees
	// and logs, so those are placed by then).
	HashMap<Vector2i, float> drift_cache;
	auto drift_at = [&](const Vector2 &p_xz) -> float {
		const Vector2i k(int(Math::floor(p_xz.x / DRIFT_CELL)), int(Math::floor(p_xz.y / DRIFT_CELL)));
		if (const float *c = drift_cache.getptr(k)) {
			return *c;
		}
		const Vector2 at((k.x + 0.5f) * DRIFT_CELL, (k.y + 0.5f) * DRIFT_CELL);
		const float d = _litter_drift(map, rocks, local_crown_grid, local_crowns, local_deadwood_grid, local_deadwood, p_setup.other, at, p_setup.wind_dir);
		drift_cache.insert(k, d);
		return d;
	};
	const Image *mask = p_setup.mask_img.ptr();
	auto mask_at = [&](const Vector2 &p_xz, int p_channel) -> float {
		if (!mask || mask->is_empty() || !p_setup.mask_rect.has_point(p_xz)) {
			return 1.0f;
		}
		const Vector2 uv = (p_xz - p_setup.mask_rect.position) / p_setup.mask_rect.size;
		const float fx = uv.x * mask->get_width() - 0.5f;
		const float fy = uv.y * mask->get_height() - 0.5f;
		const int x0 = CLAMP(int(Math::floor(fx)), 0, mask->get_width() - 1);
		const int y0 = CLAMP(int(Math::floor(fy)), 0, mask->get_height() - 1);
		const int x1 = MIN(x0 + 1, mask->get_width() - 1);
		const int y1 = MIN(y0 + 1, mask->get_height() - 1);
		const float ax = CLAMP(fx - Math::floor(fx), 0.0f, 1.0f);
		const float ay = CLAMP(fy - Math::floor(fy), 0.0f, 1.0f);
		const Color c = mask->get_pixel(x0, y0).lerp(mask->get_pixel(x1, y0), ax).lerp(mask->get_pixel(x0, y1).lerp(mask->get_pixel(x1, y1), ax), ay);
		return c[p_channel];
	};
	const int map_tx0 = int(Math::floor(rect.position.x / ts));
	const int map_tz0 = int(Math::floor(rect.position.y / ts));
	const int map_tx1 = int(Math::ceil((rect.position.x + rect.size.x) / ts));
	const int map_tz1 = int(Math::ceil((rect.position.y + rect.size.y) / ts));

	for (int s = 0; s < species_count; s++) {
		const PlaceSpecies &ps = p_setup.species[s];
		const Ref<PlantSpecies> &sp = ps.sp;
		if (sp.is_null()) {
			continue;
		}
		const int variants = MAX(1, ps.base_variants > 0 ? ps.base_variants : int(ps.meshes.size()));
		const Pattern &pat = ps.pattern;
		const float fp = ps.footprint;
		const Vector2 scale_range = sp->get_scale_range();
		const float align = sp->get_align_to_ground();
		const int reach = int(Math::ceil((fp + p_setup.max_footprint) / grid_cell));
		const bool shade_rule = sp->uses_shade();
		const bool conifer_rule = sp->uses_conifer();
		const bool litter_rule = sp->uses_litter();
		const bool drift_rule = sp->uses_litter_drift();
		const bool fairy_rule = sp->uses_fairy_ring();
		const bool high_water_rule = sp->uses_high_water();
		const bool bank_edge_rule = sp->uses_bank_edge();
		const bool dune_rule = sp->uses_dune() && p_setup.dunes;
		const bool coast_rule = sp->uses_coast();
		const bool field_rule = sp->uses_field_edge();
		const bool mobility_rule = sp->uses_sand_mobility();
		const bool burn_rule = sp->uses_burn_age();
		// Succession (WorldStand): the species' stage years and legacy kind; off the grid, old growth.
		const bool edge_rule = sp->uses_forest_edge();
		Vector2 edge_r;
		float edge_f = 0.0f;
		if (edge_rule) {
			sp->get_forest_edge_rule(edge_r, edge_f);
		}
		const bool stand_rule = !p_setup.stand_rules.is_empty() && sp->uses_stand();
		const bool browse_rule = sp->uses_browse();
		const bool rooting_rule = sp->uses_rooting();
		const Vector3 rooting_r = sp->get_rooting_rule();
		Vector2 stand_age_r, stand_kind_r;
		float stand_age_f = 0.0f, stand_kind_f = 0.0f;
		if (stand_rule) {
			sp->get_stand_rules(stand_age_r, stand_age_f, stand_kind_r, stand_kind_f);
		}
		const bool burn_recovery = !burn_rule && sp->get_burn_recovery() > 0.0f && !p_setup.burn.is_empty();
		const bool spring_rule = sp->uses_spring();
		const bool pool_rule = sp->uses_pool();
		// Nebkha shrubs grow on the sand they gathered; a pool's reeds on its wet sand.
		const bool sand_plant = sp->uses_dune() || coast_rule || mobility_rule || spring_rule || pool_rule || ps.nebkha_slot >= 0;
		if (coast_rule && !p_setup.coast) {
			continue; // Coast plants need a sea to measure from.
		}
		if (pool_rule && p_setup.pools.is_empty()) {
			continue; // Pool plants need pools to measure from.
		}
		const float pool_reach = pool_rule ? MAX(Math::abs(sp->get_rule_range(PlantSpecies::RULE_POOL_DISTANCE).x), Math::abs(sp->get_rule_range(PlantSpecies::RULE_POOL_DISTANCE).y)) + sp->get_rule_fade(PlantSpecies::RULE_POOL_DISTANCE) + 1.0f : 0.0f;
		const float canopy_r = sp->get_canopy_radius();
		const float range = ps.range;
		const bool is_log = ps.deadwood_reach > 0.0f;
		// The most a point can keep: the density, raised at rock feet (rock_foot).
		const float max_keep = sp->get_density() * (!p_setup.rocks_any ? 1.0f : sp->rock_foot_weight(0.0f));
		const PlantSpecies::DeadwoodRule dw_rule = is_log ? PlantSpecies::DEADWOOD_IGNORE : sp->get_deadwood_rule();
		const bool dw_near = dw_rule == PlantSpecies::DEADWOOD_ON || dw_rule == PlantSpecies::DEADWOOD_BESIDE;
		const float dw_band = sp->get_deadwood_band();
		const float halo = p_halo ? ps.halo : 0.0f;
		const int ht = int(Math::ceil(halo / ts));
		const int tx0 = MAX(p_tiles.position.x - ht, map_tx0);
		const int tz0 = MAX(p_tiles.position.y - ht, map_tz0);
		const int tx1 = MIN(p_tiles.position.x + p_tiles.size.x + ht, map_tx1);
		const int tz1 = MIN(p_tiles.position.y + p_tiles.size.y + ht, map_tz1);
		const Rect2 reach_rect = core.grow(halo);
		const bool nebkha_sp = ps.nebkha_slot >= 0;
		if (nebkha_sp && p_setup.nebkhas.is_empty()) {
			continue;
		}
		// A nebkha shrub's crown: its leaves' reach at unit scale (the mesh's footprint if it has none).
		float nebkha_unit = ps.spread;
		if (nebkha_sp && nebkha_unit <= 0.0f && !ps.meshes.is_empty() && ps.meshes[0].is_valid()) {
			const AABB ab = ps.meshes[0]->get_aabb();
			nebkha_unit = 0.5f * MAX(ab.size.x, ab.size.z);
		}
		LocalVector<Vector2> mound_xz;
		LocalVector<float> mound_h;
		// Colonnades (trees.md 5az): the tile's slots along its vanished logs, each with the log's direction, rank
		// (the whole row kept or dropped together by density) and key (one stilt height for the row).
		const bool row_sp = ps.colonnade.x > 0.0f && !nebkha_sp;
		LocalVector<Vector2> row_xz;
		LocalVector<Vector2> row_dir;
		LocalVector<float> row_rank;
		LocalVector<uint32_t> row_key;

		for (int tz = tz0; tz < tz1; tz++) {
			for (int tx = tx0; tx < tx1; tx++) {
				const Vector2i tile(tx, tz);
				const bool in_core = p_tiles.has_point(tile);
				TileOut *out = in_core ? &r_tiles[(tz - p_tiles.position.y) * p_tiles.size.x + (tx - p_tiles.position.x)] : nullptr;
				if (out) {
					out->species[s].xforms.resize(MAX(variants, int(ps.meshes.size())));
					out->species[s].customs.resize(MAX(variants, int(ps.meshes.size())));
				}
				const float shift = float(tile_hash(tx, tz, p_setup.seed, s) & 0xFFFFFF) / float(0x1000000);
				if (nebkha_sp) {
					// This species' mounds in the tile: their tops are its points.
					mound_xz.clear();
					mound_h.clear();
					const Rect2 tr(tx * ts, tz * ts, ts, ts);
					const int c0x = int(Math::floor(tr.position.x / NEBKHA_CELL)), c0z = int(Math::floor(tr.position.y / NEBKHA_CELL));
					const int c1x = int(Math::floor((tr.position.x + ts) / NEBKHA_CELL)), c1z = int(Math::floor((tr.position.y + ts) / NEBKHA_CELL));
					for (int cz = c0z; cz <= c1z; cz++) {
						for (int cx = c0x; cx <= c1x; cx++) {
							const LocalVector<int> *cell = p_setup.nebkha_grid.getptr(Vector2i(cx, cz));
							if (!cell) {
								continue;
							}
							for (int mi : *cell) {
								const Vector4 n = p_setup.nebkhas[mi];
								const Vector2 m(n.x, n.z);
								const uint32_t mh = hash_murmur3_one_float(n.x, hash_murmur3_one_float(n.z, uint32_t(p_setup.seed)));
								if (tr.has_point(m) && int(mh % uint32_t(p_setup.nebkha_count)) == ps.nebkha_slot) {
									mound_xz.push_back(m);
									mound_h.push_back(n.w);
								}
							}
						}
					}
				}
				if (row_sp) {
					// Colonnades: seedlings that came up in a row along a nurse log stand on stilts in that row once it
					// has rotted away (Harmon and Franklin 1989; Franklin et al. 1981). One candidate log a
					// COLONNADE_CELL cell: centre and bearing by the cell's hash, 45-100% of the longest log; slots every
					// spacing along it (a little jitter), a fifth left empty where a seedling died.
					row_xz.clear();
					row_dir.clear();
					row_rank.clear();
					row_key.clear();
					const Rect2 tr(tx * ts, tz * ts, ts, ts);
					const float half = 0.5f * ps.colonnade.y + ps.colonnade.x;
					const int c0x = int(Math::floor((tr.position.x - half) / COLONNADE_CELL)), c0z = int(Math::floor((tr.position.y - half) / COLONNADE_CELL));
					const int c1x = int(Math::floor((tr.position.x + ts + half) / COLONNADE_CELL)), c1z = int(Math::floor((tr.position.y + ts + half) / COLONNADE_CELL));
					for (int cz = c0z; cz <= c1z; cz++) {
						for (int cx = c0x; cx <= c1x; cx++) {
							const uint32_t key = tile_hash(cx, cz, p_setup.seed ^ 0x636f6c6e, s);
							auto u = [&](uint32_t p_salt) {
								return float(hash_murmur3_one_32(p_salt, key) & 0xFFFFFF) / float(0x1000000);
							};
							const Vector2 c((cx + u(1)) * COLONNADE_CELL, (cz + u(2)) * COLONNADE_CELL);
							const float ang = u(3) * Math::PI;
							const Vector2 d(Math::cos(ang), Math::sin(ang));
							const Vector2 side(-d.y, d.x);
							const float len = Math::lerp(0.45f, 1.0f, u(4)) * ps.colonnade.y;
							const int ns = MAX(2, int(len / ps.colonnade.x) + 1);
							const float rank = u(5);
							for (int k = 0; k < ns; k++) {
								const uint32_t kh = hash_murmur3_one_32(uint32_t(k), key);
								const float uk = float(kh & 0xFFFF) / 65535.0f;
								if (uk < 0.2f) {
									continue;
								}
								const float along = (k - 0.5f * (ns - 1)) * ps.colonnade.x + (float((kh >> 16) & 0xFF) / 255.0f - 0.5f) * 0.3f * ps.colonnade.x;
								const float lat = (float((kh >> 24) & 0xFF) / 255.0f - 0.5f) * 0.12f;
								const Vector2 q = c + d * along + side * lat;
								if (tr.has_point(q)) {
									row_xz.push_back(q);
									row_dir.push_back(d);
									row_rank.push_back(rank);
									row_key.push_back(key);
								}
							}
						}
					}
				}
				const uint32_t point_count = nebkha_sp ? mound_xz.size() : (row_sp ? row_xz.size() : pat.points.size());
				for (uint32_t i = 0; i < point_count; i++) {
					Vector2 xz = nebkha_sp ? mound_xz[i] : (row_sp ? row_xz[i] : Vector2(tx * ts, tz * ts) + pat.points[i]);
					if (!rect.has_point(xz) || (!in_core && !reach_rect.has_point(xz))) {
						continue;
					}
					// Ordered dither: a point survives where its (per-tile shifted) rank is below
					// the density there, so density changes smoothly and the pattern stays blue noise.
					const float rank = nebkha_sp ? 0.0f : (row_sp ? row_rank[i] : Math::fmod(pat.ranks[i] + shift, 1.0f));
					if (rank >= max_keep) {
						continue;
					}
					if (ps.desert_shrub && p_setup.near_nebkha(xz)) {
						continue; // A mound's own shrub crowns it; the others keep off its flanks.
					}
					Vector3 p3(xz.x, 0.0f, xz.y);
					float suit = sp->get_suitability(map, p3, false);
					Vector2 bank_dir; // RULE_BANK_EDGE: over the drop, and how far ahead the edge is.
					float bank_face = 0.0f; // RULE_BANK_EDGE: the face's angle below the edge (radians).
					float bank_ahead = 0.0f;
					// Patches (clonal stands), except that a rock's foot is a patch of its own
					// for species that gather there: the patch weight is lifted towards 1 there.
					float rock_boost = 1.0f;
					if (suit > 0.0f && sp->get_patch_scale() > 0.0f) {
						float pw = sp->get_patch_weight(p3);
						if (pw < 1.0f && sp->get_rock_foot() > 0.0f && !rocks.is_empty()) {
							const float reach = 4.0f * PlantSpecies::ROCK_FOOT_REACH;
							const float c = rocks.clearance(xz, reach);
							if (c < reach) {
								const float fall = Math::exp(-MAX(c, 0.0f) / PlantSpecies::ROCK_FOOT_REACH);
								pw = MAX(pw, 1.0f - Math::exp(-sp->get_rock_foot() * fall));
								rock_boost = sp->rock_foot_weight(c);
							}
						}
						suit *= pw;
					} else if (suit > 0.0f && sp->get_rock_foot() > 0.0f && !rocks.is_empty()) {
						const float reach = 4.0f * PlantSpecies::ROCK_FOOT_REACH;
						const float c = rocks.clearance(xz, reach);
						if (c < reach) {
							rock_boost = sp->rock_foot_weight(c);
						}
					}
					float shade = 0.0f;
					const bool damp_rule = sp->is_stone_top() && sp->get_stone_top_damp() != 0.0f;
					if ((shade_rule || conifer_rule || damp_rule || range > 0.0f) && suit > 0.0f) {
						float conifer = 0.0f;
						shade = crown_shade(local_crown_grid, local_crowns, crown_reach, CROWN_CELL, xz, &conifer);
						if (p_setup.other) {
							// Both canopies stop light: transmissions multiply; the stand type is
							// each one's share of the cover.
							const Vector3 g(xz.x, 0.0f, xz.y);
							const float os = p_setup.other->get_shade_at(g);
							const float oc = p_setup.other->get_conifer_at(g);
							conifer = (shade + os) > 1e-4f ? (conifer * shade + oc * os) / (shade + os) : 0.0f;
							shade = 1.0f - (1.0f - shade) * (1.0f - os);
						}
						if (shade_rule) {
							suit *= sp->rule_weight(PlantSpecies::RULE_SHADE, shade);
						}
						if (conifer_rule) {
							suit *= sp->rule_weight(PlantSpecies::RULE_CONIFER, conifer);
						}
					}
					if (dune_rule && suit > 0.0f) {
						// Coastal dunes (TerrainGenerator3D): the dune's stability under marram, -1
						// off the dune sand (less than DUNE_SAND_MIN of it).
						const Vector3 dp(xz.x, 0.0f, xz.y);
						const float dune = p_setup.dunes->get_dune_height(dp) >= DUNE_SAND_MIN ? float(p_setup.dunes->get_dune_stability(dp)) : -1.0f;
						suit *= sp->rule_weight(PlantSpecies::RULE_DUNE, dune);
					}
					if (mobility_rule && suit > 0.0f) {
						// Desert sand seas: slip faces past 20 to 30 degrees avalanche and stay bare.
						float m = p_setup.mobility_at(xz);
						if (m >= 0.0f) {
							m = MAX(m, Math::smoothstep(20.0f, 30.0f, float(Math::rad_to_deg(map->get_slope_at(p3)))));
						}
						suit *= sp->rule_weight(PlantSpecies::RULE_SAND_MOBILITY, m);
					}
					if (spring_rule && suit > 0.0f) {
						// Oases: wetness round a desert spring (DesertSprings); -1 off the sand seas.
						suit *= sp->rule_weight(PlantSpecies::RULE_SPRING, p_setup.springs.at(xz));
					}
					if (pool_rule && suit > 0.0f) {
						// Round an oasis spring's pool: by the distance from its shore, negative in the water.
						suit *= sp->rule_weight(PlantSpecies::RULE_POOL_DISTANCE, p_setup.pool_distance(xz, MIN(pool_reach, 1000.0f)));
					}
					if (burn_rule && suit > 0.0f) {
						// Burnt ground: the burn presets by the years since (charred stems, fire moss, fireweed); -1 never burnt.
						suit *= sp->rule_weight(PlantSpecies::RULE_BURN_AGE, p_setup.burn_at(xz));
					} else if (burn_recovery && suit > 0.0f) {
						// Everything else thins on a fresh burn and closes in again over its own years.
						suit *= sp->burn_cover(p_setup.burn_at(xz));
					}
					if (edge_rule && suit > 0.0f) {
						// Forest edges: metres from the canopy's edge, positive inside (the shrub mantle, edge trees).
						// No forest known (no canopy species here or in the canopy field): an interior rule (open-ended inside)
						// does nothing, an edge rule places nothing.
						const float e = p_setup.edge.is_empty() && edge_r.y >= 10000.0f ? edge_r.y : p_setup.edge_at(xz);
						suit *= edge_f <= 0.0f ? ((e >= edge_r.x && e <= edge_r.y) ? 1.0f : 0.0f) : Math::smoothstep(float(edge_r.x) - edge_f, float(edge_r.x), e) * (1.0f - Math::smoothstep(float(edge_r.y), float(edge_r.y) + edge_f, e));
					}
					if (stand_rule && suit > 0.0f) {
						const int sc = p_setup.stand_cell(xz);
						suit *= PlantSpecies::stand_weight(stand_age_r, stand_age_f, stand_kind_r, stand_kind_f, sc < 0 ? -1.0f : p_setup.stand_age[sc], sc < 0 ? 0 : int(p_setup.stand_kind[sc]));
					}
					if (browse_rule && suit > 0.0f) {
						// Deer browsing pressure (0 where none is known).
						suit *= sp->rule_weight(PlantSpecies::RULE_BROWSE, p_setup.browse_cell(p_setup.stand_cell(xz)));
					}
					if (rooting_rule && suit > 0.0f) {
						// Boar rooting: years since the soil was turned (-1 never).
						suit *= PlantSpecies::rooting_weight(rooting_r, p_setup.rooting_cell(p_setup.stand_cell(xz)));
					}
					if (!sand_plant && suit > 0.0f && !p_setup.sea_depth.is_empty()) {
						// Desert sand seas: only sand plants (RULE_SAND_MOBILITY) root in their loose sand; the rest
						// thin out as it deepens from 5 to 30 cm.
						suit *= 1.0f - Math::smoothstep(0.05f, 0.3f, p_setup.sea_depth_at(xz));
					}
					if (!sand_plant && suit > 0.0f && !p_setup.sand.is_empty()) {
						// Loose sand (beach, dune, sand sea): no soil to root in or to hold litter.
						suit *= 1.0f - CLAMP((p_setup.sand_at(xz) - 0.05f) / 0.25f, 0.0f, 1.0f);
					}
					if (coast_rule && suit > 0.0f) {
						// The tidal frame (salt marsh and shingle): height over the generator's sea, the
						// coast's exposure to waves and its beach mask, all from the geology source.
						const Vector3 cp(xz.x, 0.0f, xz.y);
						suit *= sp->rule_weight(PlantSpecies::RULE_TIDE_HEIGHT, float(p_setup.coast->get_height(cp)) - p_setup.sea_y);
						if (suit > 0.0f) {
							suit *= sp->rule_weight(PlantSpecies::RULE_COAST_EXPOSURE, float(p_setup.coast->get_coast_exposure_at(cp)));
						}
						if (suit > 0.0f) {
							suit *= sp->rule_weight(PlantSpecies::RULE_BEACH, float(p_setup.coast->get_beach_at(cp)));
						}
					}
					if (litter_rule && suit > 0.0f) {
						float litter = crown_litter(local_crown_grid, local_crowns, p_setup.crown_max + p_setup.litter_reach, CROWN_CELL, xz, p_setup.litter_skew);
						if (p_setup.other) {
							litter = 1.0f - (1.0f - litter) * (1.0f - p_setup.other->get_litter_at(Vector3(xz.x, 0.0f, xz.y)));
						}
						// Leaf drifts: the wind piles what fell into hollows and against obstacles and sweeps crests.
						if (litter > 0.0f) {
							litter = MIN(litter * drift_at(xz), 1.0f);
						}
						suit *= sp->rule_weight(PlantSpecies::RULE_LITTER, litter);
					}
					if (drift_rule && suit > 0.0f) {
						suit *= sp->rule_weight(PlantSpecies::RULE_LITTER_DRIFT, drift_at(xz));
					}
					if (fairy_rule && suit > 0.0f) {
						// Meadow fairy rings (fairy_rings.h): the same rings the grass shows.
						const FairyRings::Hit fh = FairyRings::at(xz);
						suit *= sp->rule_weight(PlantSpecies::RULE_FAIRY_RING, fh.arc > 0.5f ? fh.distance : 1000.0f);
					}
					if (high_water_rule && suit > 0.0f) {
						// The last flood's mark (WorldWater.high_water_at): wrack strands along it, the grass below combed flat.
						const float ground = map->get_height_at(p3);
						const Vector4 hw = WorldContracts::water()->high_water_at(Vector3(xz.x, ground, xz.y));
						const bool known = hw.x > -1e20f && hw.y >= 0.0f;
						suit *= sp->rule_weight(PlantSpecies::RULE_HIGH_WATER, known ? ground - hw.x : 1000.0f);
						suit *= sp->rule_weight(PlantSpecies::RULE_FLOOD_AGE, known ? hw.y / 365.25f : 10000.0f);
					}
					if (bank_edge_rule && suit > 0.0f) {
						// The top edge of a cut bank or scarp in the drawn ground: stood on it, facing the drop.
						const float drop = _bank_edge(map, xz, bank_dir, bank_ahead, ps.face_slopes.is_empty() ? nullptr : &bank_face);
						suit *= sp->rule_weight(PlantSpecies::RULE_BANK_EDGE, drop);
						if (suit > 0.0f && bank_dir != Vector2()) {
							xz += bank_dir * bank_ahead;
							p3 = Vector3(xz.x, 0.0f, xz.y);
						}
					}
					if (ps.bare_cover && climate_bare && suit > 0.0f) {
						// Drylands: the climate's bare soil patches (WorldContracts::bare_patch), as the ground and sward.
						const Vector2i bc(int(Math::floor(xz.x / 32.0f)), int(Math::floor(xz.y / 32.0f)));
						const float *bp = bare_cells.getptr(bc);
						const float bare_share = bp ? *bp : bare_cells.insert(bc, WorldContracts::climate_bare_share_at(Vector3((bc.x + 0.5f) * 32.0f, 0.0f, (bc.y + 0.5f) * 32.0f)))->value;
						if (bare_share > 0.0f && WorldContracts::bare_patch(p3, bare_share) > 0.5f) {
							continue;
						}
					}
					bool hay_stalk = false;
					if (ps.hay_cut && suit > 0.0f && GrassField3D::has_hay_meadows()) {
						// Mown with the hay meadow: its cut stalks until it grows back (or gone, with no cut meshes).
						const float md = GrassField3D::mown_days_at(Vector3(xz.x, 0.0f, xz.y));
						if (md >= 0.0f && md < HAY_REGROW) {
							if (int(ps.meshes.size()) < 2 * ps.base_variants) {
								continue;
							}
							hay_stalk = true;
						}
					}
					if (sp->get_mask_channel() >= 0 && suit > 0.0f) {
						const float m = CLAMP(mask_at(xz, sp->get_mask_channel()), 0.0f, 1.0f);
						suit *= sp->is_mask_invert() ? 1.0f - m : m;
					}
					// An uprooted tree's root pit stays bare soil: no plant but the log itself.
					if (suit > 0.0f && !is_log && (deadwood_pit(local_deadwood_grid, local_deadwood, xz, DEADWOOD_CELL) || (p_setup.other && p_setup.other->_pit_at(xz)))) {
						continue;
					}
					// Fallen wood (docs/ideas/forest-floor.md, 6.6): off its footprint, on its upper
					// surface or along its side, by the species' rule.
					bool on_log = false;
					DeadwoodHit hit;
					if (dw_rule != PlantSpecies::DEADWOOD_IGNORE && suit > 0.0f) {
						const float dw_reach = dw_rule == PlantSpecies::DEADWOOD_BESIDE ? dw_band : 0.0f;
						// Beside: the nearest wood thick enough, past any sticks nearer.
						const float dw_min = dw_rule == PlantSpecies::DEADWOOD_BESIDE ? sp->get_deadwood_min_radius() : 0.0f;
						bool found = deadwood_nearest(local_deadwood_grid, local_deadwood, xz, dw_reach, DEADWOOD_CELL, hit, dw_min);
						if (p_setup.other) {
							found = p_setup.other->_deadwood_at(xz, dw_reach, hit, dw_min) || found;
						}
						if (dw_rule == PlantSpecies::DEADWOOD_AVOID) {
							if (found && hit.side < 0.0f) {
								suit = 0.0f;
							}
						} else if (!found) {
							suit = 0.0f;
						} else if (dw_rule == PlantSpecies::DEADWOOD_ON) {
							// The upper two thirds of its width, on logs thick enough.
							if (hit.side > -hit.radius / 3.0f || hit.radius < sp->get_deadwood_min_radius()) {
								suit = 0.0f;
							}
							on_log = true;
						} else {
							// Beside it: outside the log, and only by wood thick enough (honey fungus
							// by stumps and trunks, not twigs).
							if (hit.side < 0.0f || hit.radius < sp->get_deadwood_min_radius()) {
								suit = 0.0f;
							}
							// Only by wood lying on the ground: a trunk held up by its root plate or
							// branches has open ground under it, not a foot to grow along.
							if (hit.axis_y - hit.radius - map->get_height_at(p3) > DEADWOOD_BESIDE_GAP) {
								suit = 0.0f;
							}
							suit *= 1.0f - CLAMP(hit.side / dw_band, 0.0f, 1.0f);
							if (sp->is_deadwood_uphill() && suit > 0.0f) {
								// Leaves creeping downhill stop against its uphill side.
								const float slope = map->get_slope_at(p3);
								const float aspect = map->get_aspect_at(p3);
								const Vector2 downhill(Math::sin(aspect), Math::cos(aspect));
								const float uphill = -hit.across.dot(downhill);
								suit *= Math::lerp(1.0f, CLAMP(uphill * 2.0f, 0.0f, 1.0f), CLAMP(slope / Math::deg_to_rad(4.0f), 0.0f, 1.0f));
							}
						}
					} else if (dw_near) {
						suit = 0.0f;
					}
					// Wood feeders (PlantSpecies.deadwood_from_wood, honey fungus) come up from the wood
					// itself: their foot moves onto the log's side where it meets the ground, the
					// plant turned to face out of it (+X away from the wood).
					// The foot goes up the side to a bark crack about 20 cm over the litter (or the
					// log's widest girth on a thinner one), so the clump visibly roots in the wood.
					Vector2 wood_out;
					float wood_y = -1e9f;
					if (dw_rule == PlantSpecies::DEADWOOD_BESIDE && suit > 0.0f && sp->is_deadwood_from_wood() && hit.across != Vector2()) {
						const float ground = map->get_height_at(p3);
						const float lift = CLAMP(hit.axis_y - ground, 0.0f, WOOD_FEEDER_HEIGHT);
						const float dy = hit.axis_y - (ground + lift);
						const float reach = Math::sqrt(MAX(hit.radius * hit.radius - dy * dy, 0.0f)); // Out from the axis at that height.
						xz -= hit.across * (hit.side + hit.radius - reach + WOOD_FEEDER_INSET);
						p3 = Vector3(xz.x, 0.0f, xz.y);
						wood_out = hit.across;
						wood_y = hit.axis_y - dy;
						// Where the log carries its cracks and bark edges (tree_deadwood_features), the
						// clump bursts from the nearest one low on its side instead: from a drying check
						// or where the bark has peeled from the wood. None there on a log that has them:
						// no clump.
						Vector3 fp, fn;
						bool any = false;
						float best = 1e9f;
						const Vector3 at(xz.x, wood_y, xz.y);
						const float lo = 0.02f, hi = WOOD_FEEDER_HEIGHT + 0.12f;
						bool got = deadwood_feature_nearest(local_deadwood_grid, local_deadwood, at, WOOD_FEEDER_REACH, lo, hi, ground, DEADWOOD_CELL, fp, fn, any, best);
						if (p_setup.other) {
							Vector3 op, on;
							bool oany = false;
							if (p_setup.other->_deadwood_feature_at(at, WOOD_FEEDER_REACH, lo, hi, ground, op, on, oany)) {
								const float od = Vector2(op.x - at.x, op.z - at.z).length();
								if (!got || od < best) {
									fp = op;
									fn = on;
									got = true;
								}
							}
							any = any || oany;
						}
						if (got) {
							const Vector2 out2 = Vector2(fn.x, fn.z).normalized();
							xz = Vector2(fp.x, fp.z) - out2 * WOOD_FEEDER_INSET;
							p3 = Vector3(xz.x, 0.0f, xz.y);
							wood_out = out2;
							wood_y = fp.y;
						} else if (any) {
							suit = 0.0f;
						}
					}
					// Rocks as nurse objects: shelter and the rain running off them make their foot
					// moister, so species that like it gather there (PlantSpecies.rock_foot).
					suit *= rock_boost;
					// On stone tops (PlantSpecies.stone_top): only inside a stone's cap.
					bool on_top = false;
					Vector4 top_cap;
					Vector3 top_normal;
					if (suit > 0.0f && sp->is_stone_top()) {
						int ti = -1;
						const float c = p_setup.stone_tops.clearance(xz, 0.0f, nullptr, &ti);
						if (ti >= 0 && c < 0.0f) {
							on_top = true;
							top_cap = p_setup.stone_tops.items[ti];
							top_normal = ti < p_setup.stone_top_normals.size() ? p_setup.stone_top_normals[ti] : Vector3();
							if (damp_rule) {
								// How damp the top stays: tilted to the pole (north) it is shaded, low
								// over the ground it keeps the ground's humidity, beside taller stones
								// (on the sun's side most) or under a canopy it is out of the sun. High,
								// open, sun-facing tops dry out: hoary, yellow-olive cushions there
								// (Racomitrium, Grimmia), deep green ones in the damp. The measure is
								// the cap's, not the plant's, so a top reads as one or the other.
								const float aspect = top_normal.y > 0.1f ? -top_normal.z : 0.0f;
								const float low = 1.0f - Math::smoothstep(STONE_TOP_LOW, STONE_TOP_HIGH, float(top_cap.y) - map->get_height_at(Vector3(top_cap.x, 0.0f, top_cap.z)));
								const float shelter = p_setup.stone_tops.shelter(Vector2(top_cap.x, top_cap.z), float(top_cap.y), STONE_TOP_SHELTER_REACH);
								const float wet = 1.0f * aspect + 0.5f * low + 0.7f * shelter + 0.6f * shade;
								// One or the other per stone (a little jitter per cap, so the split is not a
								// contour line across the field).
								const uint32_t ch = hash_murmur3_one_float(float(top_cap.x), hash_murmur3_one_float(float(top_cap.z)));
								const float jitter = 0.1f * (float(ch & 0xffff) / 65535.0f - 0.5f);
								const float damp = wet + jitter >= STONE_TOP_DAMP_SPLIT ? 1.0f : 0.0f;
								suit *= sp->stone_top_damp_weight(damp);
							}
							if (sp->has_stone_top_water()) {
								// The top's height over the water: in a stream the surface is the bed under
								// the stone plus the map's water depth there; on the bank, its height over the
								// ground plus a quarter of the way to the water (banks rise about 1 in 4), so
								// a stone at the edge sits in the splash and flood band and one up the bank
								// does not. Aquatic mosses keep under and just over the surface, dry-ground
								// mosses above the flood line (Scarlett and O'Hare 2006).
								const Vector3 cap3(top_cap.x, 0.0f, top_cap.z);
								const float bed = map->get_height_at(cap3);
								const float depth = map->get_water_depth_at(cap3);
								const float over = depth > 0.0f ? float(top_cap.y) - (bed + depth) : float(top_cap.y) - bed + 0.25f * MAX(map->get_water_distance_at(cap3), 0.0f);
								suit *= sp->stone_top_water_weight(over);
							}
						} else {
							suit = 0.0f;
						}
					}
					// Crevice plants: only in the fine soil caught between stones (PlantSpecies.stone_gap).
					if (suit > 0.0f && sp->get_stone_gap() > 0.0f) {
						const float gap_reach = 6.0f * PlantSpecies::STONE_GAP_REACH;
						float first = 1e6f;
						const float second = rocks.is_empty() ? 1e6f : rocks.second_clearance(xz, gap_reach, &first);
						suit *= sp->stone_gap_weight(first, second);
					}
					// Worn paths: the plants thin out towards the trodden line by their resistance
					// (PlantSpecies.trample_resistance), the ones left shorter. Trees, logs and the
					// plants on stones and logs stand clear of the feet.
					float trodden = 1.0f;
					if (suit > 0.0f && !p_setup.paths.is_empty() && canopy_r <= 0.0f && !is_log && !on_top && !on_log && sp->get_trample_resistance() > 0.0f) {
						const float passes = p_setup.paths.passes_at(xz);
						suit *= sp->trample_cover(passes);
						trodden = sp->trample_scale(passes);
					}
					// Field margins (PlantSpecies field_edge_range): arable weeds in the headland and the
					// uncropped strip, verge plants by the hedge; no fields, no margin plants.
					if (field_rule && suit > 0.0f) {
						const float reach = MAX(Math::abs(sp->get_rule_range(PlantSpecies::RULE_FIELD_EDGE).x), Math::abs(sp->get_rule_range(PlantSpecies::RULE_FIELD_EDGE).y)) + sp->get_rule_fade(PlantSpecies::RULE_FIELD_EDGE) + 1.0f;
						suit *= p_setup.crop_fields.is_empty() ? 0.0f : sp->rule_weight(PlantSpecies::RULE_FIELD_EDGE, p_setup.crop_fields.edge_distance(xz, MIN(reach, 1000.0f)));
					}
					// Crop fields: nothing else grows in them (the margins are outside), but for the
					// margin plants that keep to the field's edge.
					if (suit > 0.0f && !field_rule && !p_setup.crop_fields.is_empty() && !on_top && p_setup.crop_fields.contains(xz)) {
						suit = 0.0f;
					}
					const float keep = sp->get_density() * suit;
					if (rank >= keep) {
						continue;
					}
					// Rocks: no stem inside one. A tree's trunk foot clears it, a log its whole
					// length, a ground plant its stem; leaves may lean over the rock.
					if (!rocks.is_empty() && !on_top) {
						const float om = is_log ? ps.deadwood_reach : (canopy_r > 0.0f ? ps.stem_foot * MAX(scale_range.x, scale_range.y) : 0.0f) + OBSTACLE_MARGIN;
						if (rocks.blocks(xz, om)) {
							continue;
						}
					}
					// Openings in the ground (cave mouths): the terrain is cut away there, so no stem
					// stands in one or at its edge, and no log lies over it.
					if (!p_setup.holes.is_empty() && p_setup.holes.at(Vector3(xz.x, map->get_height_at(Vector3(xz.x, 0.0f, xz.y)), xz.y), is_log ? ps.deadwood_reach : OBSTACLE_MARGIN)) {
						continue;
					}
					// Stems: ground plants keep out of the trunks' feet (flare and root ridges), this
					// field's and the canopy field's; other woody species keep their footprints.
					if (canopy_r <= 0.0f && !is_log && (stem_in(local_crown_grid, local_crowns, xz, STEM_MARGIN, CROWN_CELL) || (p_setup.other && p_setup.other->_in_stem(xz, STEM_MARGIN)))) {
						continue;
					}
					if (!p_setup.removals.is_empty() && p_setup.removed_at(xz, s)) {
						continue; // Taken away (add_plant_removal).
					}
					// Footprints of earlier species.
					bool blocked = false;
					const Vector2i gc(int(Math::floor(xz.x / grid_cell)), int(Math::floor(xz.y / grid_cell)));
					for (int dz = -reach; dz <= reach && !blocked; dz++) {
						for (int dx = -reach; dx <= reach && !blocked; dx++) {
							const LocalVector<int> *bucket = grid.getptr(gc + Vector2i(dx, dz));
							if (!bucket) {
								continue;
							}
							for (int idx : *bucket) {
								const Placed &o = placed[idx];
								if (o.species == s || (o.deadwood && dw_near) || o.top != on_top) {
									continue;
								}
								const float min_d = fp + o.footprint;
								if (o.xz.distance_squared_to(xz) < min_d * min_d) {
									blocked = true;
									break;
								}
							}
						}
					}
					if (blocked) {
						continue;
					}
					// The plant's transform: on the ground, tilted towards its normal, random yaw and size.
					const float r = (nebkha_sp || row_sp) ? float(hash_murmur3_one_float(xz.x, hash_murmur3_one_float(xz.y)) & 0xFFFFFF) / float(0x1000000) : pat.randoms[i];
					float scale = Math::lerp(float(scale_range.x), float(scale_range.y), Math::fmod(r * 104729.0f, 1.0f)) * trodden;
					if (nebkha_sp && nebkha_unit > 0.0f) {
						// The shrub that built the mound: its crown about NEBKHA_CROWN of the mound's radius.
						scale = NEBKHA_CROWN * NEBKHA_RADIUS * mound_h[i] / nebkha_unit * Math::lerp(0.9f, 1.1f, r);
					}
					// At a rock's foot: leaves must not pass into the rock. A plant reaching in over it
					// grows smaller and leans away, as plants pressed against a boulder do.
					float rock_fit = 1.0f;
					Vector2 rock_away;
					if (canopy_r <= 0.0f && !is_log && !on_log && !on_top && ps.spread > 0.0f && !rocks.is_empty()) {
						const float reach = ps.spread * scale;
						const float c = rocks.clearance(xz, reach, &rock_away);
						// Crevice plants arch their leaves out over the stones they root between.
						const float overhang = ROCK_OVERHANG + CREVICE_OVERHANG * sp->get_stone_gap();
						if (c + overhang < reach) {
							rock_fit = (c + overhang) / reach;
							if (rock_fit < ROCK_FIT_MIN) {
								// Crevice plants live pressed between stones: the smallest fit, not none.
								if (sp->get_stone_gap() <= 0.0f) {
									continue;
								}
								rock_fit = ROCK_FIT_MIN;
							}
						}
					}
					// On a stone top: the plant stays on its cap, a cushion only a little over the rim
					// (STONE_TOP_OVERHANG); smaller to fit, or none where even ROCK_FIT_MIN of it would not.
					if (on_top && ps.spread > 0.0f) {
						const float room = float(top_cap.w) - Vector2(xz.x - float(top_cap.x), xz.y - float(top_cap.z)).length() + STONE_TOP_OVERHANG;
						const float reach = ps.spread * scale;
						if (room < reach) {
							rock_fit = room / reach;
							if (rock_fit < ROCK_FIT_MIN) {
								continue;
							}
						}
					}
					placed.push_back({ xz, fp, s, is_log, on_top });
					grid[gc].push_back(placed.size() - 1);
					scale *= rock_fit;
					// A stand's margin: smaller plants leaning out to the open (PlantSpecies.margin_*).
					float margin = 0.0f;
					Vector2 margin_out;
					if (sp->get_margin_width() > 0.0f) {
						margin = sp->margin_at(sp->get_patch_depth(p3, &margin_out));
						scale *= Math::lerp(1.0f, sp->get_margin_scale(), margin);
					}
					if (canopy_r > 0.0f) {
						Crown crown = { xz, canopy_r * scale, sp->get_canopy_opacity(), ps.canopy_top * scale, sp->is_canopy_conifer() ? 1.0f : 0.0f, ps.stem_foot * scale, ps.canopy_base * scale };
						crown.species = s;
						crown.scale = scale;
						crown.y = map->get_height_at(p3) + _relief_y(p3);
						local_crowns.push_back(crown);
						local_crown_grid[Vector2i(int(Math::floor(xz.x / CROWN_CELL)), int(Math::floor(xz.y / CROWN_CELL)))].push_back(local_crowns.size() - 1);
						if (out) {
							out->crowns.push_back(crown);
						}
					}
					if (!out && !is_log) {
						continue; // Placed only for the plants of the tiles asked for.
					}
					// Floating plants sit on the water surface, level, over the ground below it;
					// plants on a log on its upper surface, tilted with it.
					const bool floats = sp->is_floating();
					// On a log: on the moss that covers its upper side, not on the bare wood under it
					// (the trees' moss shell stands up to 1.1 to 2.5 cm proud of the bark, section 5c of
					// trees.md), so cushions and seedlings are not swallowed by it.
					// On the drawn ground: the heightfield plus Landscape3D's bank and cliff relief (WorldGround.relief_at).
					float y = on_log ? hit.top + DEADWOOD_MOSS_LIFT / MAX(hit.normal.y, 0.5f) : map->get_height_at(p3) + (floats ? map->get_water_depth_at(p3) : _relief_y(p3));
					if (wood_y > -1e8f) {
						y = wood_y; // On the wood's side (above).
					}
					const float slope = floats ? 0.0f : map->get_slope_at(p3);
					const float aspect = map->get_aspect_at(p3);
					Vector3 normal = on_log ? hit.normal : Vector3(Math::sin(aspect) * Math::sin(slope), Math::cos(slope), Math::cos(aspect) * Math::sin(slope));
					if (on_top) {
						const Vector2 off(xz.x - top_cap.x, xz.y - top_cap.z);
						if (top_normal.y > 0.1f) {
							// A measured cap (stone_top_normals): the plane through its point, as flat
							// as the caller found the stone's surface there.
							y = float(top_cap.y) - (top_normal.x * off.x + top_normal.z * off.y) / top_normal.y - STONE_TOP_SINK;
							normal = top_normal;
						} else {
							// Otherwise a dome of the cap's radius (a stone's top is rounded): set down
							// its curve from the highest point and tilted with it.
							const float rr = MAX(float(top_cap.w), 0.01f);
							const float t = MIN(off.length() / rr, 1.0f);
							y = float(top_cap.y) - 0.5f * rr * t * t - STONE_TOP_SINK;
							normal = Vector3(off.x / rr, 1.0f, off.y / rr).normalized();
						}
					}
					const Vector3 up = Vector3(0, 1, 0).lerp(normal, align).normalized();
					Basis b;
					const Vector3 axis = Vector3(0, 1, 0).cross(up);
					if (axis.length_squared() > 1e-10f) {
						b = Basis(axis.normalized(), Math::acos(CLAMP(up.y, -1.0f, 1.0f)));
					}
					float yaw = sp->is_random_yaw() ? Math::fmod(r * 7919.0f, 1.0f) * Math::TAU : 0.0f;
					const Vector2 downhill(Math::sin(aspect), Math::cos(aspect));
					int bank_variant = -1;
					if (!ps.bank_slopes.is_empty()) {
						// A bank tree: the variant grown on the slope nearest this ground's, ties
						// broken by the plant's own random number.
						float best = 1e9f;
						const int nv = ps.bank_slopes.size();
						const int start = MIN(int(r * nv), nv - 1);
						for (int k = 0; k < nv; k++) {
							const int vi = (start + k) % nv;
							const float d = Math::abs(ps.bank_slopes[vi] - slope);
							if (d < best - 1e-4f) {
								best = d;
								bank_variant = vi;
							}
						}
					}
					if (bank_dir != Vector2() && !ps.face_slopes.is_empty() && bank_face > 0.0f) {
						// Bank roots: the variant grown on the face angle nearest this face's, ties broken by the plant's random number.
						float best = 1e9f;
						const int nv = ps.face_slopes.size();
						const int start = MIN(int(r * nv), nv - 1);
						for (int k = 0; k < nv; k++) {
							const int vi = (start + k) % nv;
							const float d = Math::abs(ps.face_slopes[vi] - bank_face);
							if (d < best - 1e-4f) {
								best = d;
								bank_variant = vi;
							}
						}
					}
					int flood_variant = -1;
					Vector2 flood_flow;
					if (!ps.flood_marks.is_empty() && WorldContracts::water() && WorldContracts::water()->has_provider()) {
						// Flood-marked trees (TreeGenerator.flood_height, trees.md 5av): the variant whose silt line is
						// nearest the last flood's mark over this ground (WorldWater.high_water_at: its peak above the
						// ground, then its age), turned with mesh +X down the peak's flow so the line rides higher on the
						// upstream face; with no mark here (or one under 10 cm) an unflooded variant. With no water
						// producer at all the variants stay as drawn (their marks are then the generator's own).
						const float ground = map ? map->get_height_at(Vector3(xz.x, 0.0f, xz.y)) : y;
						const Vector4 hw = WorldContracts::water()->high_water_at(Vector3(xz.x, ground, xz.y));
						const bool known = hw.x > -1e20f && hw.y >= 0.0f && hw.x - ground > 0.1f;
						const float want_h = known ? hw.x - ground : 0.0f;
						const float want_y = known ? hw.y / 365.25f : -1.0f;
						const int nv = ps.flood_marks.size();
						const int start = MIN(int(Math::fmod(r * 2903.0f, 1.0f) * nv), nv - 1);
						float best = 1e9f;
						for (int k = 0; k < nv; k++) {
							const int vi = (start + k) % nv;
							const Vector2 fm = ps.flood_marks[vi];
							if ((fm.x > 0.0f) != known) {
								continue;
							}
							// 30 cm of line weighs as much as 1.5 years of fading.
							const float d = known ? Math::abs(fm.x - want_h) / 0.3f + Math::abs(fm.y - want_y) / 1.5f : 0.0f;
							if (d < best - 1e-4f) {
								best = d;
								flood_variant = vi;
							}
						}
						if (flood_variant >= 0 && known) {
							flood_flow = Vector2(hw.z, hw.w);
						}
					}
					if (bank_dir != Vector2()) {
						// A sod lip (RULE_BANK_EDGE): mesh +X out over the drop, a little scatter along the edge.
						yaw = Math::atan2(-bank_dir.y, bank_dir.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 0.2f;
					} else if (wood_out != Vector2()) {
						yaw = Math::atan2(-wood_out.y, wood_out.x);
					} else if (flood_flow.length_squared() > 1e-6f) {
						// Its silt line is higher on mesh -X (the upstream face, where the flood piled against it).
						const Vector2 fd = flood_flow.normalized();
						yaw = Math::atan2(-fd.y, fd.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 0.3f;
					} else if (bank_variant >= 0 && !ps.bank_slopes.is_empty() && ps.bank_slopes[bank_variant] > 1e-3f && slope > 1e-3f) {
						// Its ground falls towards mesh +Z: turn +Z downhill.
						yaw = Math::atan2(downhill.x, downhill.y);
					} else if (ps.fall_downhill && slope > 1e-3f) {
						// Carried down by an avalanche (the trees module, trees.md 5bh): mesh +X down the fall line.
						yaw = Math::atan2(-downhill.y, downhill.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 2.0f * ps.fall_scatter;
					} else if (ps.fall_dir != Vector2()) {
						// Thrown by one storm (the trees module, trees.md 5am): mesh +X along the storm's track, each
						// tree turned by the gusts and its neighbours within fall_scatter.
						yaw = Math::atan2(-ps.fall_dir.y, ps.fall_dir.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 2.0f * ps.fall_scatter;
					} else if (ps.fall_water > 0.0f && map && Math::fmod(r * 3571.0f, 1.0f) < ps.fall_water) {
						// Felled by beavers (the trees module, trees.md 5au): most fall toward the water, the beaver
						// having cut from the water side. +X down the water distance, about 30 degrees either way.
						const float e = 1.5f;
						const Vector2 grad(map->get_water_distance_at(Vector3(xz.x + e, 0.0f, xz.y)) - map->get_water_distance_at(Vector3(xz.x - e, 0.0f, xz.y)),
								map->get_water_distance_at(Vector3(xz.x, 0.0f, xz.y + e)) - map->get_water_distance_at(Vector3(xz.x, 0.0f, xz.y - e)));
						if (grad.length_squared() > 1e-8f) {
							const Vector2 to_water = -grad.normalized();
							yaw = Math::atan2(-to_water.y, to_water.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 1.05f;
						}
					} else if (ps.wind_flag && p_setup.wind_dir != Vector2()) {
						// Flagged by the prevailing wind: mesh +X downwind, a little scatter (eddies
						// round the land turn it locally).
						yaw = Math::atan2(-p_setup.wind_dir.y, p_setup.wind_dir.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 0.3f;
					} else if (sp->is_follow_flow() && slope > 1e-3f) {
						// Downstream: water runs downhill, so +X along the fall line (a little scatter,
						// as shoots splay in the eddies behind a stone).
						const Vector2 down(Math::sin(aspect), Math::cos(aspect));
						yaw = Math::atan2(-down.y, down.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 0.5f;
					}
					if (ps.open_side != Vector2() && wood_out == Vector2() && !p_setup.edge.is_empty()) {
						// An edge tree (mesh meta tree_open_side): that side turned down the edge distance, towards the
						// open, within about 30 degrees (light reaches an edge tree over a wide arc; within 15 degrees a run of
						// edge trees read as one tree repeated).
						const Vector2 open = p_setup.edge_open(xz);
						if (open != Vector2()) {
							yaw = Math::atan2(-open.y, open.x) - Math::atan2(-ps.open_side.y, ps.open_side.x) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 1.05f;
						}
					}
					if (row_sp) {
						// A colonnade tree: its stilts' tunnel (mesh +X) along the vanished log, either way round.
						const Vector2 d = row_dir[i];
						yaw = Math::atan2(-d.y, d.x) + ((hash_murmur3_one_32(row_key[i], uint32_t(i) * 2654435761u) & 1u) ? Math::PI : 0.0f) + (Math::fmod(r * 7919.0f, 1.0f) - 0.5f) * 0.12f;
					}
					b = b * Basis(Vector3(0, 1, 0), yaw);
					if (wood_out != Vector2()) {
						// Out of the bark: the clump's foot tips away from the wood, as it bursts
						// from a crack in the side and the stalks curve up to the light.
						const Vector3 lean_axis = Vector3(0, 1, 0).cross(Vector3(wood_out.x, 0.0f, wood_out.y)).normalized();
						b = Basis(lean_axis, Math::deg_to_rad(WOOD_FEEDER_LEAN_DEG)) * b;
					}
					if (margin > 0.0f && sp->get_margin_lean() > 0.0f && margin_out != Vector2()) {
						// Tip the plant's axis towards the open, about the horizontal axis across it.
						const Vector3 lean_axis = Vector3(0, 1, 0).cross(Vector3(margin_out.x, 0.0f, margin_out.y)).normalized();
						b = Basis(lean_axis, Math::deg_to_rad(sp->get_margin_lean()) * margin) * b;
					}
					if (rock_fit < 1.0f && !on_top) {
						const Vector3 lean_axis = Vector3(0, 1, 0).cross(Vector3(rock_away.x, 0.0f, rock_away.y)).normalized();
						b = Basis(lean_axis, Math::deg_to_rad(ROCK_LEAN_DEG) * (1.0f - rock_fit) / (1.0f - ROCK_FIT_MIN)) * b;
					}
					// Trodden: the plants left are pressed down and lie over, each its own way.
					const float trod = CLAMP((1.0f - trodden) / 0.55f, 0.0f, 1.0f);
					if (trod > 0.0f) {
						const float ta = Math::fmod(r * 6151.0f, 1.0f) * Math::TAU;
						b = Basis(Vector3(Math::cos(ta), 0.0f, Math::sin(ta)), Math::deg_to_rad(55.0f) * trod) * b;
					}
					b.scale(Vector3(scale, scale * (1.0f - 0.5f * trod), scale));
					int fire_variant = -1;
					if (!ps.fire_years.is_empty()) {
						// Trees grown for a fire (TreeGenerator.fire_years): on burnt ground the variant whose years
						// are nearest the ground's burn age (WorldFire.burn_age_at, the field's grid), off burns or
						// long after (past 1.5 times the oldest band) an unburnt one; ties by the tree's own number.
						// With bands (mesh meta "tree_fire_band", years from and to): the variant whose band holds the age,
						// an unburnt one past the last band.
						const float age = p_setup.burn.is_empty() ? -1.0f : p_setup.burn_at(xz);
						bool burnt = age >= 0.0f && age <= ps.fire_years_max * 1.5f;
						if (!ps.fire_bands.is_empty()) {
							burnt = false;
							for (const Vector2 &band : ps.fire_bands) {
								burnt = burnt || (band.y > band.x && age >= band.x && age < band.y);
							}
						}
						const int nv = ps.fire_years.size();
						const int start = MIN(int(r * nv), nv - 1);
						float best = 1e9f;
						for (int k = 0; k < nv; k++) {
							const int vi = (start + k) % nv;
							const float fy = ps.fire_years[vi];
							if ((fy >= 0.0f) != burnt) {
								continue;
							}
							if (burnt && !ps.fire_bands.is_empty() && !(age >= ps.fire_bands[vi].x && age < ps.fire_bands[vi].y)) {
								continue;
							}
							const float d = burnt ? Math::abs(fy - age) : 0.0f;
							if (d < best - 1e-4f) {
								best = d;
								fire_variant = vi;
							}
						}
					}
					if (!ps.strike_years.is_empty() && !(fire_variant >= 0 && ps.fire_years[fire_variant] >= 0.0f)) {
						// Struck trees (TreeGenerator.lightning_years, trees.md 5ax): under a recorded strike
						// (WorldStand.strike_age_at, copied in the setup) the struck variant nearest the strike's age, until
						// STRIKE_SHOWN years (8; then its scar has healed into the tree's own); otherwise never a struck one.
						const float sa = p_setup.strike_at(xz);
						const bool struck = sa >= 0.0f && sa <= 8.0f;
						const int nv = ps.strike_years.size();
						const int start = MIN(int(r * nv), nv - 1);
						float best = 1e9f;
						int pick = -1;
						for (int k = 0; k < nv; k++) {
							const int vi = (start + k) % nv;
							const float sy = ps.strike_years[vi];
							if ((sy >= 0.0f) != struck || (!ps.fire_years.is_empty() && vi < int(ps.fire_years.size()) && ps.fire_years[vi] >= 0.0f)) {
								continue;
							}
							const float d = struck ? Math::abs(sy - sa) : 0.0f;
							if (d < best - 1e-4f) {
								best = d;
								pick = vi;
							}
						}
						if (pick >= 0 && (struck || fire_variant < 0 || ps.strike_years[fire_variant] >= 0.0f)) {
							fire_variant = pick;
						}
					}
					if (flood_variant >= 0 && fire_variant < 0) {
						fire_variant = flood_variant;
					}
					if (!ps.browsed.is_empty()) {
						// Browsed trees (TreeGenerator.browse, trees.md 5az): where deer press (WorldStand.browse_at, the
						// setup's grid) a living tree is the browsed variant with the chance smoothstep(0.3, 0.8, pressure),
						// by its own number; elsewhere never one. Burnt and struck picks stay as they are.
						const int nv = ps.browsed.size();
						auto special = [&](int vi) {
							return (vi < int(ps.fire_years.size()) && ps.fire_years[vi] >= 0.0f) || (vi < int(ps.strike_years.size()) && ps.strike_years[vi] >= 0.0f);
						};
						const int cur = fire_variant >= 0 ? fire_variant : (bank_variant >= 0 ? bank_variant : MIN(int(r * variants), variants - 1));
						if (cur >= nv || !special(cur)) {
							const float pressure = p_setup.browse_at(xz);
							const bool want = pressure > 0.0f && Math::fmod(r * 4801.0f, 1.0f) < Math::smoothstep(0.3f, 0.8f, pressure);
							if (cur >= nv || bool(ps.browsed[cur]) != want) {
								const int start = MIN(int(Math::fmod(r * 3301.0f, 1.0f) * nv), nv - 1);
								for (int k = 0; k < nv; k++) {
									const int vi = (start + k) % nv;
									if (bool(ps.browsed[vi]) == want && !special(vi)) {
										fire_variant = vi;
										break;
									}
								}
							} else {
								fire_variant = cur;
							}
						}
					}
					if (row_sp && variants > 1) {
						// One log, one girth: every tree of a colonnade the same living variant (its stilt height), by the
						// log's key. Burnt, struck and browsed picks stay as they are.
						auto special = [&](int vi) {
							return (vi < int(ps.fire_years.size()) && ps.fire_years[vi] >= 0.0f) || (vi < int(ps.strike_years.size()) && ps.strike_years[vi] >= 0.0f) || (vi < int(ps.browsed.size()) && ps.browsed[vi]);
						};
						if (fire_variant < 0 || !special(fire_variant)) {
							const int start = int(hash_murmur3_one_32(0x726f77u, row_key[i]) % uint32_t(variants));
							for (int k = 0; k < variants; k++) {
								const int vi = (start + k) % variants;
								if (!special(vi)) {
									fire_variant = vi;
									break;
								}
							}
						}
					}
					int clone_variant = -1;
					if (ps.clone_scale > 0.0f && variants > 1) {
						// A clone (aspen, trees.md 5bf; Mitton and Grant 1996): every stem from one root system shares its
						// genotype, so its form, bark and leaf-out and autumn dates. The clones are jittered cells (a
						// Worley partition, so no straight edges show in autumn), each one variant.
						const Vector2 cq = xz / ps.clone_scale;
						const int cx = int(Math::floor(cq.x)), cz = int(Math::floor(cq.y));
						float best = 1e9f;
						uint32_t best_h = 0;
						for (int dz = -1; dz <= 1; dz++) {
							for (int dx = -1; dx <= 1; dx++) {
								const uint32_t h = hash_murmur3_one_32(uint32_t(cx + dx) * 73856093u ^ uint32_t(cz + dz) * 19349663u, uint32_t(ps.clone_scale * 16.0f) + 0x636c6eu);
								const Vector2 site(float(cx + dx) + 0.15f + 0.7f * float(h & 0xffff) / 65535.0f, float(cz + dz) + 0.15f + 0.7f * float(h >> 16) / 65535.0f);
								const float d2 = site.distance_squared_to(cq);
								if (d2 < best) {
									best = d2;
									best_h = h;
								}
							}
						}
						clone_variant = int(hash_murmur3_one_32(best_h, 0x766172u) % uint32_t(variants));
					}
					int variant = fire_variant >= 0 ? fire_variant : (bank_variant >= 0 ? bank_variant : (clone_variant >= 0 ? clone_variant : MIN(int(r * variants), variants - 1)));
					if (hay_stalk) {
						variant += ps.base_variants;
					}
					// Stilt roots: raised by the variant's lift along its own up (scaled and tilted with it).
					const Vector3 lift = variant < int(ps.base_lifts.size()) ? b.xform(Vector3(0.0f, ps.base_lifts[variant], 0.0f)) : Vector3();
					const Transform3D world(b, Vector3(xz.x, y, xz.y) + lift);
					if (is_log && variant < int(ps.deadwood_axes.size()) && ps.deadwood_axes[variant].size() >= 7) {
						const PackedFloat32Array &ax = ps.deadwood_axes[variant];
						const Vector3 la = world.xform(Vector3(ax[0], ax[1], ax[2]));
						const Vector3 lb = world.xform(Vector3(ax[3], ax[4], ax[5]));
						// The reach profile (float 7 the count N, then N reaches evenly from a to b,
						// measured to the outside of the moss): one piece per span, so plants beside
						// and on the log follow its real width and bends instead of the widest reach.
						// Older 7-float meshes are one piece of that radius.
						// Newer meshes add the stem's centreline: N points (x, y, z, girth to the outside
						// of the moss) along it, so a bent stick or branch is its own thin wood, not a
						// fat straight piece over its chord (whose reach takes the bend).
						const int n = ax.size() >= 8 ? int(ax[7]) : 0;
						const bool profile = n >= 2 && ax.size() >= 8 + n;
						const bool centreline = profile && ax.size() >= 8 + 5 * n;
						const int pieces = profile ? n - 1 : 1;
						for (int k = 0; k < pieces; k++) {
							Deadwood log = { la, lb, ax[6] * scale };
							if (centreline) {
								const int c = 8 + n + 4 * k;
								log.a = world.xform(Vector3(ax[c], ax[c + 1], ax[c + 2]));
								log.b = world.xform(Vector3(ax[c + 4], ax[c + 5], ax[c + 6]));
								log.radius = MAX(0.5f * (ax[c + 3] + ax[c + 7]) * scale - DEADWOOD_MOSS_LIFT, 0.01f);
							} else if (profile) {
								log.a = la.lerp(lb, float(k) / float(pieces));
								log.b = la.lerp(lb, float(k + 1) / float(pieces));
								log.radius = MAX(0.5f * (ax[8 + k] + ax[9 + k]) * scale - DEADWOOD_MOSS_LIFT, 0.01f);
							}
							local_deadwood.push_back(log);
							deadwood_insert(local_deadwood_grid, log, local_deadwood.size() - 1, DEADWOOD_CELL);
							if (out) {
								out->deadwood.push_back(log);
							}
						}
						// Its cracks and bark edges (tree_deadwood_features), where wood feeders root.
						if (variant < int(ps.deadwood_features.size())) {
							const PackedFloat32Array &fa = ps.deadwood_features[variant];
							const Basis nb = world.basis.inverse().transposed();
							for (int f = 0; f + 7 < fa.size(); f += 8) {
								Deadwood d;
								d.feature = int8_t(fa[f]);
								d.a = world.xform(Vector3(fa[f + 1], fa[f + 2], fa[f + 3]));
								d.b = d.a + nb.xform(Vector3(fa[f + 4], fa[f + 5], fa[f + 6])).normalized() * 0.01f; // Kept short: the grid takes the a-b span.
								d.radius = 0.0f;
								local_deadwood.push_back(d);
								deadwood_insert(local_deadwood_grid, d, local_deadwood.size() - 1, DEADWOOD_CELL);
								if (out) {
									out->deadwood.push_back(d);
								}
							}
						}
						// An uprooted tree's pit (cx, cy, cz, radius, depth in mesh space): bare soil.
						if (variant < int(ps.deadwood_pits.size()) && ps.deadwood_pits[variant].size() >= 4) {
							const PackedFloat32Array &pit = ps.deadwood_pits[variant];
							Deadwood d;
							d.a = d.b = world.xform(Vector3(pit[0], pit[1], pit[2]));
							d.radius = pit[3] * scale;
							d.pit = true;
							local_deadwood.push_back(d);
							deadwood_insert(local_deadwood_grid, d, local_deadwood.size() - 1, DEADWOOD_CELL);
							if (out) {
								out->deadwood.push_back(d);
							}
						}
					}
					// The stone its roots grow over: a disc of its widest half extent, kept clear.
					if (variant < int(ps.root_stones.size()) && ps.root_stones[variant].size() >= 6) {
						const PackedFloat32Array &st = ps.root_stones[variant];
						Deadwood d;
						d.a = d.b = world.xform(Vector3(st[0], st[1], st[2]));
						d.radius = (MAX(st[3], st[5]) + ROOT_STONE_MARGIN) * scale;
						d.pit = true;
						d.stone = true;
						local_deadwood.push_back(d);
						deadwood_insert(local_deadwood_grid, d, local_deadwood.size() - 1, DEADWOOD_CELL);
						if (out) {
							out->deadwood.push_back(d);
						}
					}
					if (!out) {
						continue;
					}
					out->species[s].xforms[variant].push_back(p_setup.to_local * world);
					// Custom data for the plant shader: the rank among the plants kept here (blue noise,
					// so thinning by distance keeps the field even), the species' range, size, species.
					// y: the range in whole metres, and in its fraction the light the plant grew in
					// (VegetationLight), which tints its leaves; species without a range get neither.
					const float light = (1.0f - shade) * VegetationLight::exposure(map, p_setup.sun_mean, xz.x, xz.y);
					const float range_light = range > 0.0f ? MAX(Math::floor(range), 1.0f) + VegetationLight::pack(light) / 101.0f : 0.0f;
					// w: the species, and in its fraction the regrowth flush on burnt ground (the plant shader's fresh
					// green): full on a fresh burn, gone by a year and a half.
					const float burn_age = p_setup.burn.is_empty() ? -1.0f : p_setup.burn_at(xz);
					const float flush = burn_age < 0.0f ? 0.0f : 1.0f - Math::smoothstep(0.0f, 1.5f, burn_age);
					// x: the rank in its fraction and the far tail's end in whole metres (vegetation_far_scale()).
					const float tail = ps.reach > range ? Math::floor(ps.reach) : 0.0f;
					out->species[s].customs[variant].push_back(Color(tail + CLAMP(rank / MAX(keep, 1e-4f), 0.0f, 0.999f), range_light, scale, float(s) + flush * 0.99f));
					if (ps.far_color.a > 0.0f && range > 0.0f) {
						// The ground a plant hides: a disc of half its spacing, scaled.
						const float rr = 0.5f * sp->get_spacing() * scale;
						out->far.push_back({ xz, ps.far_color, float(ps.far_color.a * Math::PI * rr * rr), range });
					}
					out->species[s].positions.push_back(world.origin);
					if (ps.frontal > 0.0f) {
						out->shelter.push_back(Vector3(world.origin.x, world.origin.z, ps.frontal * scale * scale));
					}
					out->count++;
				}
			}
		}
	}
}

// A cell's MultiMesh: one record per plant, or for a rigged tree one per bone, which TreeWind3D
// (target: this field) poses each frame.
void VegetationField3D::_fill_cell(const Ref<MultiMesh> &p_mm, const Ref<Mesh> &p_mesh, const LocalVector<Transform3D> &p_xforms, const LocalVector<Color> &p_customs) {
#ifdef VEGETATION_HAS_TREE_WIND
	if (TreeWind3D::get_mesh_bone_count(p_mesh) > 0) {
		TreeWind3D::fill_rest(p_mm, p_mesh, p_xforms, p_customs);
		return;
	}
#endif
	p_mm->set_instance_count(p_xforms.size());
	for (uint32_t i = 0; i < p_xforms.size(); i++) {
		p_mm->set_instance_transform(i, p_xforms[i]);
		p_mm->set_instance_custom_data(i, p_customs[i]);
	}
}

// Near meshes on demand. Within 1.2 x near_range a far cell asks its source for the near mesh
// every frame until it has it, then gets its near cell with the same crossfade as eager near
// meshes; beyond 1.5 x the near cell is freed (and the source may free the mesh).
// A cell's lod_bias: its biggest plant's scale. The renderer picks a MultiMesh's mesh LOD by the node's own scale
// only, not the instances' (a plant scaled 2.2 took the LOD of one at 1, coarser than its size on screen needs);
// by the biggest, no plant in the cell gets a coarser one than its size calls for (as the rock cells do).
real_t VegetationField3D::_lod_scale(const LocalVector<Transform3D> &p_xforms) {
	real_t s = 0.0;
	for (const Transform3D &x : p_xforms) {
		const Vector3 k = x.basis.get_scale_abs();
		s = MAX(s, MAX(k.x, MAX(k.y, k.z)));
	}
	return MAX(s, real_t(0.001));
}

void VegetationField3D::_lazy_near_update(const Vector3 &p_camera) {
	// Ice storms (trees.md 5be): the glaze over each near cell (WorldWeather.glaze_at, read twice a second), in bands; a
	// near mesh grows its band's ice as geometry, and a cell whose band changes gets a new near cell once it is built
	// (the old one draws meanwhile). The local season likewise (trees.md 5bi): WorldWeather.season_at over the cell,
	// banded by the source (TreeGenerator.near_season_band), so a late-frost hollow or a slope not yet in leaf grows
	// its own leaf state in the near mesh.
	WorldContracts *wc = WorldContracts::get_singleton();
	const Ref<WorldWeather> weather = wc ? wc->get_weather() : Ref<WorldWeather>();
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const bool glaze_check = weather.is_valid() && now - glaze_checked_usec > 500000;
	if (glaze_check) {
		glaze_checked_usec = now;
	}
	auto near_code = [](int p_variant, int p_glaze, int p_season) {
		return p_variant + TreeGenerator::NEAR_GLAZE_STRIDE * p_glaze + TreeGenerator::NEAR_SEASON_STRIDE * p_season;
	};
	auto make_near = [&](LazyNear &c, const Ref<Mesh> &p_mesh) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(p_mesh);
		_fill_cell(mm, p_mesh, c.xforms, c.customs);
		const float margin = impostors_traced ? 0.0f : MAX(c.range * 0.1f, 1.0f);
		c.near = memnew(MultiMeshInstance3D);
		c.near->set_lod_bias(_lod_scale(c.xforms));
		c.near->set_name(String(c.far->get_name()) + "_Near");
		c.near->set_multimesh(mm);
		c.near->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
		c.near->set_visibility_range_end(c.range);
		c.near->set_visibility_range_end_margin(margin);
		add_child(c.near, false, INTERNAL_MODE_BACK);
		c.far->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
		c.far->set_visibility_range_begin(c.range);
		c.far->set_visibility_range_begin_margin(margin);
	};
	for (KeyValue<ObjectID, LazyNear> &kv : lazy_near) {
		LazyNear &c = kv.value;
		const float d = c.centre.distance_to(p_camera);
		int glaze = c.glaze_band;
		int season = c.season_band;
		if (weather.is_null()) {
			glaze = 0;
			season = 0;
		} else if (glaze_check && d <= c.range * 1.2f) {
			glaze = TreeGenerator::glaze_band(weather->glaze_at(c.centre));
			const TreeGenerator *tg = Object::cast_to<TreeGenerator>(ObjectDB::get_instance(c.source));
			season = tg ? tg->near_season_band(weather->season_at(c.centre)) : 0;
		}
		const int want = near_code(c.variant, glaze, season);
		if (c.near) {
			if (d > c.range * 1.5f) {
				_lazy_near_free(c);
				continue;
			}
			if (want != near_code(c.variant, c.glaze_band, c.season_band)) {
				Object *src = ObjectDB::get_instance(c.source);
				const Ref<Mesh> mesh = src ? Ref<Mesh>(src->call(SNAME("request_near_mesh"), c.key, want)) : Ref<Mesh>();
				if (mesh.is_valid()) {
					_lazy_near_free(c);
					c.glaze_band = glaze;
					c.season_band = season;
					make_near(c, mesh);
				}
			}
			continue;
		}
		c.glaze_band = glaze;
		c.season_band = season;
		if (d > c.range * 1.2f) {
			continue;
		}
		Object *src = ObjectDB::get_instance(c.source);
		if (!src) {
			continue;
		}
		const Ref<Mesh> mesh = src->call(SNAME("request_near_mesh"), c.key, want);
		if (mesh.is_null()) {
			continue; // Still building.
		}
		make_near(c, mesh);
	}
}

void VegetationField3D::_lazy_near_free(LazyNear &p_cell) {
	if (!p_cell.near) {
		return;
	}
	if (p_cell.near->get_parent()) {
		p_cell.near->get_parent()->remove_child(p_cell.near);
	}
	memdelete(p_cell.near);
	p_cell.near = nullptr;
	if (p_cell.far) {
		p_cell.far->set_visibility_range_begin(0.0f);
		p_cell.far->set_visibility_range_begin_margin(0.0f);
	}
}

void VegetationField3D::_lazy_near_drop(MultiMeshInstance3D *p_far) {
	LazyNear *c = lazy_near.getptr(p_far->get_instance_id());
	if (!c) {
		return;
	}
	c->far = nullptr;
	_lazy_near_free(*c);
	lazy_near.erase(p_far->get_instance_id());
}

// Adds a placed tile's plants to the scene: one MultiMeshInstance3D per species mesh variant,
// split into cells where the species swaps meshes by distance. Main thread.
// The share of a species' plants (by rank) kept p_distance metres from the camera: the plant shader's
// vegetation_far_scale() without its 1.12 overlap. Past half the range with the square of the distance when the
// species has a far tail (p_reach > p_range), fading out over the tail's last quarter; else from 1 at 35% of the
// range to 0 at the range.
float VegetationField3D::_far_keep(float p_range, float p_reach, float p_distance) {
	if (p_range <= 0.0f) {
		return 1.0f;
	}
	if (p_reach <= p_range) {
		return CLAMP((p_range - p_distance) / (p_range * 0.65f), 0.0f, 1.0f);
	}
	const float d0 = 0.5f * p_range;
	const float tail = d0 * d0 / MAX(p_distance * p_distance, d0 * d0);
	return tail * CLAMP((p_reach - p_distance) / (p_reach * 0.25f), 0.0f, 1.0f);
}

void VegetationField3D::_commit_species(const PlaceSetup &p_setup, const TileOut &p_tile, int p_species, LocalVector<MultiMeshInstance3D *> &r_nodes, float p_max_rank) {
	const PlaceSpecies &ps = p_setup.species[p_species];
	if (ps.sp.is_null() || ps.meshes.is_empty()) {
		return;
	}
	const TileOut::Species &so = p_tile.species[p_species];
	const float range = ps.range;
	const float reach = MAX(ps.reach, range);
	const float near_range = ps.sp->get_near_range();
	// Cells for the near swap: a quarter of the swap distance, at least 4 m. A species with a range is cut too where
	// tiles are big against it, into cells of its range (at least 16 m: cells are draw calls), and every cell is kept
	// a half diagonal past the range, so a tile next to the camera is not cut at once.
	const float near_cell = MAX(near_range * 0.25f, 4.0f);
	const float range_cell = range > 0.0f ? MAX(range, 16.0f) : 0.0f;
	// Small items (PlantSpecies.detail_size): the path tracer instances their shared shapes (vegetation_detail.h).
	const bool detail = ps.sp->get_detail_size() > 0.0f;
	const int tx = p_tile.tile.x;
	const int tz = p_tile.tile.y;
	const int s = p_species;
	for (int v = 0; v < int(so.xforms.size()); v++) {
		if (so.xforms[v].is_empty()) {
			continue;
		}
		const Ref<Mesh> &mesh = ps.meshes[v % ps.meshes.size()];
		if (mesh.is_null()) {
			continue;
		}
		Ref<Mesh> near_mesh;
		if (near_range > 0.0f && ps.near_meshes.size() > 0) {
			near_mesh = ps.near_meshes[v % ps.near_meshes.size()];
		}
		// Near meshes on demand: cells as for a near mesh, the far pass only for now
		// (_lazy_near_update() adds the near cells as the camera comes close).
		Object *near_source = near_range > 0.0f && ps.near_meshes.is_empty() ? ps.sp->get_near_mesh_source() : nullptr;
		// Godot's visibility range measures to the centre of the whole MultiMesh, so a
		// species with a near mesh is cut into small cells: the swap then happens a few
		// plants at a time instead of a whole tile at once.
		const float ts = p_setup.tile_size;
		float cell_size = ts;
		if (near_mesh.is_valid() || near_source) {
			cell_size = near_cell;
		}
		if (range_cell > 0.0f) {
			cell_size = MIN(cell_size, range_cell);
		}
		const int cells = CLAMP(int(Math::ceil(ts / cell_size)), 1, 16);
		const Vector2 tile_origin = Vector2(tx * ts, tz * ts);
		// Trees (meta far_widen, trees.md 9a): the tail's survivors grown in their transforms instead of the plant
		// shader's vegetation_far_scale(), by the keep this tile was committed for (min(1/sqrt(keep), 1.6), the
		// cover held as the ranks thin; Cook et al. 2007): across by the full factor, up by a third of it so the
		// skyline does not rise. Per tile, rebuilt with the tile as the camera comes closer; a tree's surfaces then
		// need no camera-dependent vertex capture in the path tracer.
		Basis widen;
		if (ps.far_widen && p_max_rank < 1.0f) {
			// By the tail alone, as the shader's (vegetation review, 20 Nov): the committed keep includes the fade over
			// the reach's last quarter, so the distance it was committed for is found from it (keep falls with
			// distance) and the tail taken there.
			const float k = MAX((p_max_rank - 0.02f) / 1.12f, 1e-4f);
			float lo = 0.5f * range, hi = MAX(reach, lo);
			for (int it = 0; it < 24; it++) {
				const float mid = 0.5f * (lo + hi);
				if (_far_keep(range, reach, mid) > k) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			const float d0 = 0.5f * range, dk = MAX(0.5f * (lo + hi), d0);
			const float f = MIN(dk / d0, 1.6f); // min(1/sqrt(tail), 1.6), tail = (d0/d)^2.
			widen = Basis::from_scale(Vector3(f, Math::lerp(1.0f, f, 0.33f), f));
		}
		LocalVector<LocalVector<uint32_t>> buckets;
		buckets.resize(cells * cells);
		for (uint32_t i = 0; i < so.xforms[v].size(); i++) {
			bool extra = false;
			if (p_max_rank < 1.0f && Math::fmod(so.customs[v][i].r, 1.0f) >= p_max_rank) {
				// Thinned out at the tile's distance (the far tail), unless a tree's crown breaks the skyline from the
				// camera (trees.md 9c): ridgeline silhouettes against the sky are what reads of a far forest, and a
				// thinned ridge shows gaps there first.
				if (!(ps.far_top > 0.0f && stream_eye_valid && p_setup.map && _on_skyline(p_setup.map, stream_eye, p_setup.global_xf.xform(so.xforms[v][i].xform(Vector3(0.0f, ps.far_top, 0.0f)))))) {
					continue;
				}
				extra = true;
			}
			const Vector3 o = p_setup.global_xf.xform(so.xforms[v][i].origin);
			const int cx = CLAMP(int((o.x - tile_origin.x) / ts * cells), 0, cells - 1);
			const int cz = CLAMP(int((o.z - tile_origin.y) / ts * cells), 0, cells - 1);
			// A kept skyline tree is marked (top bit): it stands as it is, not widened for the tail it is not part of.
			buckets[cz * cells + cx].push_back(extra ? (i | 0x80000000u) : i);
		}
		for (int b = 0; b < cells * cells; b++) {
			if (buckets[b].is_empty()) {
				continue;
			}
			// Trees past far_hull_range: their crown hulls (trees.md 9b), a third pass.
			const Ref<Mesh> hull_mesh = ps.far_hull_range > 0.0f && mesh->has_meta(SNAME("tree_far_hull")) ? Ref<Mesh>(mesh->get_meta(SNAME("tree_far_hull"))) : Ref<Mesh>();
			const float hull_margin = impostors_traced ? 0.0f : ps.far_hull_range * 0.05f;
			for (int pass = 0; pass < 3; pass++) {
				const bool near_pass = pass == 1;
				const bool hull_pass = pass == 2;
				if ((near_pass && near_mesh.is_null()) || (hull_pass && hull_mesh.is_null())) {
					continue;
				}
				Ref<MultiMesh> mm;
				mm.instantiate();
				mm->set_transform_format(MultiMesh::TRANSFORM_3D);
				mm->set_use_custom_data(true);
				const Ref<Mesh> draw_mesh = hull_pass ? hull_mesh : (near_pass ? near_mesh : mesh);
				mm->set_mesh(draw_mesh);
				LocalVector<Transform3D> bx;
				LocalVector<Color> bc;
				for (uint32_t bi : buckets[b]) {
					const uint32_t i = bi & 0x7fffffffu;
					Transform3D x = so.xforms[v][i];
					if (!(bi & 0x80000000u)) {
						x.basis = x.basis * widen;
					}
					bx.push_back(x);
					bc.push_back(so.customs[v][i]);
				}
				_fill_cell(mm, draw_mesh, bx, bc);
				if (detail) {
					VegetationDetail::trace_instanced(mm);
				}
				MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
				mmi->set_lod_bias(_lod_scale(bx));
				mmi->set_name(cells > 1 || hull_pass ? vformat("Plants%s_%d_%d_%d_%d_%d", near_pass ? "Near" : (hull_pass ? "Hull" : ""), s, v, tx, tz, b) : vformat("Plants_%d_%d_%d_%d", s, v, tx, tz));
				mmi->set_multimesh(mm);
				if (near_mesh.is_valid() && !hull_pass) {
					// Crossfade: both draw over the margin, each dithered in or out
					// (GeometryInstance3D fade mode Self), so the swap never pops.
					// No margin under path tracing: the TLAS takes both cells inside it (no dither
					// there), so plants in the band would be traced twice (_set_swap_margins()).
					const float margin = impostors_traced ? 0.0f : MAX(near_range * 0.1f, 1.0f);
					has_near_swap = true;
					mmi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
					if (near_pass) {
						mmi->set_visibility_range_end(near_range);
						mmi->set_visibility_range_end_margin(margin);
					} else {
						mmi->set_visibility_range_begin(near_range);
						mmi->set_visibility_range_begin_margin(margin);
					}
				}
				if (range > 0.0f && !near_pass) {
					// Ground cover too small to see far away. The plant shader thins the instances
					// by rank and shrinks them out before the range (docs/ideas/plant-generator.md,
					// "Far distance"), so the tile is cut only once every plant in it is gone.
					// Godot measures to the cell's centre: a plant can be up to its half diagonal nearer.
					mmi->set_visibility_range_end(reach * 1.05f + 0.75f * ts / cells);
				}
				if (hull_mesh.is_valid() && !near_pass) {
					// The tree to far_hull_range, its hull past it (dithered over the margin; none under path tracing,
					// where the TLAS would take both).
					mmi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
					if (hull_pass) {
						mmi->set_visibility_range_begin(ps.far_hull_range);
						mmi->set_visibility_range_begin_margin(hull_margin);
					} else if (mmi->get_visibility_range_end() <= 0.0f || ps.far_hull_range < mmi->get_visibility_range_end()) {
						// Never later than the tile's own reach (a hull range past it would draw the trees further).
						mmi->set_visibility_range_end(ps.far_hull_range);
						mmi->set_visibility_range_end_margin(hull_margin);
					}
				}
				add_child(mmi, false, INTERNAL_MODE_BACK);
				r_nodes.push_back(mmi);
				if (hull_pass) {
					continue;
				}
				if (near_source) {
					has_near_swap = true;
					LazyNear cell;
					cell.far = mmi;
					cell.source = near_source->get_instance_id();
					cell.key = ps.sp->get_near_mesh_key();
					cell.variant = v;
					cell.range = near_range;
					for (const Transform3D &x : bx) {
						cell.centre += p_setup.global_xf.xform(x.origin);
					}
					cell.centre /= MAX(int(bx.size()), 1);
					cell.xforms = bx;
					cell.customs = bc;
					lazy_near.insert(mmi->get_instance_id(), cell);
				}
				if (!near_pass && draw_mesh->has_meta(SNAME("impostor_of"))) {
					impostor_cells.push_back(mmi);
					impostor_meshes.push_back(draw_mesh);
					if (impostors_traced) {
						mm->set_mesh(draw_mesh->get_meta(SNAME("impostor_of")));
					}
				}
			}
		}
	}
	// Past the range: the plants as a voxel shell, so tall cover keeps its volume and silhouette
	// instead of dropping to the ground's colour (far_volume, off by default).
	if (far_volume && range > 0.0f && far_volume_range > range) {
		const Ref<ArrayMesh> vol = _build_far_volume(p_setup, p_tile, p_species);
		if (vol.is_valid()) {
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_mesh(vol);
			mm->set_instance_count(1);
			mm->set_instance_transform(0, Transform3D());
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(vformat("FarVolume_%d_%d_%d", s, tx, tz));
			mmi->set_multimesh(mm);
			// Crossfades in while the plant shader thins the plants out, before their range.
			mmi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
			mmi->set_visibility_range_begin(range * 0.85f);
			mmi->set_visibility_range_begin_margin(range * 0.15f);
			mmi->set_visibility_range_end(far_volume_range);
			mmi->set_visibility_range_end_margin(far_volume_range * 0.1f);
			add_child(mmi, false, INTERNAL_MODE_BACK);
			r_nodes.push_back(mmi);
		}
	}
}

// Far plants as volume (docs/ideas/vegetation.md, "Far plants as volume"): the approach of Unreal
// 5.7's voxelised Nanite foliage and of hybrid mesh-volume LoDs (Loubet and Neyret 2017), with
// plain geometry so raster and path tracing see the same thing. Each plant's bounds fill an
// ellipsoid of leaf cover over its foot; the cover sums on a voxel grid; a voxel is solid with
// that probability (a fixed hash, so the shell is porous where the stand is thin and stable from
// frame to frame); the solid voxels' faces open to the air are the mesh, their corners jittered by
// a hash of their position (shared by every face meeting there) so the shell is lumpy, not boxy.
// Colour: the species' far colour, darker down inside the stand.
Ref<ArrayMesh> VegetationField3D::_build_far_volume(const PlaceSetup &p_setup, const TileOut &p_tile, int p_species) const {
	const PlaceSpecies &ps = p_setup.species[p_species];
	if (ps.far_color.a <= 0.0f) {
		return Ref<ArrayMesh>();
	}
	const TileOut::Species &so = p_tile.species[p_species];
	const float cell = far_volume_cell;
	const float inv = 1.0f / cell;
	HashMap<Vector3i, float> cover;
	for (int v = 0; v < int(so.xforms.size()); v++) {
		const Ref<Mesh> &mesh = ps.meshes[v % ps.meshes.size()];
		if (mesh.is_null()) {
			continue;
		}
		const AABB bb = mesh->get_aabb();
		const float height = bb.get_end().y;
		const float half = 0.25f * (bb.size.x + bb.size.z);
		if (height < cell * 0.5f) {
			continue; // Too low to stand out of the ground: the far colour carries it.
		}
		for (uint32_t i = 0; i < so.xforms[v].size(); i++) {
			const Vector3 o = p_setup.global_xf.xform(so.xforms[v][i].origin);
			const float sc = so.customs[v][i].b;
			const float ry = 0.5f * height * sc;
			const float rxz = 0.8f * half * sc;
			const Vector3 c = o + Vector3(0, ry, 0);
			const Vector3i lo(int(Math::floor((c.x - rxz) * inv)), int(Math::floor(o.y * inv)), int(Math::floor((c.z - rxz) * inv)));
			const Vector3i hi(int(Math::floor((c.x + rxz) * inv)), int(Math::floor((c.y + ry) * inv)), int(Math::floor((c.z + rxz) * inv)));
			for (int z = lo.z; z <= hi.z; z++) {
				for (int y = lo.y; y <= hi.y; y++) {
					for (int x = lo.x; x <= hi.x; x++) {
						const Vector3 p((x + 0.5f) * cell, (y + 0.5f) * cell, (z + 0.5f) * cell);
						const float dx = (p.x - c.x) / MAX(rxz, 1e-3f);
						const float dy = (p.y - c.y) / MAX(ry, 1e-3f);
						const float dz = (p.z - c.z) / MAX(rxz, 1e-3f);
						const float d2 = dx * dx + dy * dy + dz * dz;
						if (d2 < 1.0f) {
							cover[Vector3i(x, y, z)] += 0.45f * ps.far_color.a * (1.0f - d2);
						}
					}
				}
			}
		}
	}
	if (cover.is_empty()) {
		return Ref<ArrayMesh>();
	}
	auto hash01 = [](const Vector3i &p_k, uint32_t p_salt) {
		uint32_t h = hash_murmur3_one_32(uint32_t(p_k.x), p_salt);
		h = hash_murmur3_one_32(uint32_t(p_k.y), h);
		h = hash_murmur3_one_32(uint32_t(p_k.z), h);
		return float(hash_fmix32(h) & 0xffffff) / float(0x1000000);
	};
	HashSet<Vector3i> solid;
	int top = INT32_MIN;
	int bottom = INT32_MAX;
	for (const KeyValue<Vector3i, float> &kv : cover) {
		// Beer-Lambert: overlapping plants fill a voxel as their leaves would block a ray through it.
		if (hash01(kv.key, 0x9e37u) < 1.0f - Math::exp(-kv.value)) {
			solid.insert(kv.key);
			top = MAX(top, kv.key.y);
			bottom = MIN(bottom, kv.key.y);
		}
	}
	if (solid.is_empty()) {
		return Ref<ArrayMesh>();
	}
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedInt32Array indices;
	const Vector3i dirs[6] = { Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 1, 0), Vector3i(0, -1, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1) };
	const Color base = ps.far_color;
	auto corner = [&](const Vector3i &p_c) {
		const float j = 0.35f * cell;
		return Vector3(p_c.x * cell + (hash01(p_c, 1u) - 0.5f) * j * 2.0f, p_c.y * cell + (hash01(p_c, 2u) - 0.5f) * j, p_c.z * cell + (hash01(p_c, 3u) - 0.5f) * j * 2.0f);
	};
	for (const Vector3i &k : solid) {
		// Light falls off down into the stand (its own shade), as the plants' leaves do.
		int depth = 0;
		for (int up = 1; up <= 3 && solid.has(k + Vector3i(0, up, 0)); up++) {
			depth = up;
		}
		const float shade = 1.0f - 0.15f * depth;
		const Color col(base.r * shade, base.g * shade, base.b * shade, 1.0f);
		for (int f = 0; f < 6; f++) {
			const Vector3i d = dirs[f];
			if (solid.has(k + d) || (d.y < 0 && k.y <= bottom)) {
				continue; // Inside, or on the ground.
			}
			// The face's four corners, wound outward.
			Vector3i a, b;
			if (d.x != 0) {
				a = Vector3i(0, 1, 0);
				b = Vector3i(0, 0, 1);
			} else if (d.y != 0) {
				a = Vector3i(0, 0, 1);
				b = Vector3i(1, 0, 0);
			} else {
				a = Vector3i(1, 0, 0);
				b = Vector3i(0, 1, 0);
			}
			const Vector3i o = k + Vector3i(MAX(d.x, 0), MAX(d.y, 0), MAX(d.z, 0));
			Vector3i q[4] = { o, o + a, o + a + b, o + b };
			const bool flip = (d.x + d.y + d.z) < 0;
			const int first = verts.size();
			Vector3 pts[4];
			for (int c = 0; c < 4; c++) {
				pts[c] = corner(q[c]);
			}
			Vector3 n = (pts[1] - pts[0]).cross(pts[3] - pts[0]);
			if (flip) {
				n = -n;
			}
			n = n.normalized();
			for (int c = 0; c < 4; c++) {
				verts.push_back(pts[c]);
				normals.push_back(n);
				colors.push_back(col);
			}
			if (flip) {
				indices.append_array(PackedInt32Array({ first, first + 1, first + 2, first, first + 2, first + 3 }));
			} else {
				indices.append_array(PackedInt32Array({ first, first + 2, first + 1, first, first + 3, first + 2 }));
			}
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, _far_volume_mat());
	mesh->set_meta(SNAME("far_volume"), true);
	return mesh;
}

Ref<StandardMaterial3D> VegetationField3D::_far_volume_mat() const {
	Ref<StandardMaterial3D> mat = far_volume_material;
	if (mat.is_null()) {
		mat.instantiate();
		mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		mat->set_roughness(1.0f);
		// A leaf canopy's own sheen is broken up in its gaps: a smooth surface's sheen (specular 0.5) lit the far
		// layer grey with the sky at grazing angles, over every meadow (as GrassField3D's far shell found).
		mat->set_specular(0.1f);
		mat->set_albedo(Color(far_shade, far_shade, far_shade).linear_to_srgb()); // The far match's darkening (far_match_view).
		const_cast<VegetationField3D *>(this)->far_volume_material = mat;
	}
	return mat;
}

// The far shell (far_shell): the plant cover as one surface draped over the ground, from where the streamed plants
// thin out to far_shell_range, in tiles of 32 x 32 cells: 64 m tiles (2 m cells) to 400 m, four times wider (8 m) to
// 1600 m, and four times again (32 m) past it, a few pixels a cell at 1080p each. A tile is built for the centre it
// was seen from (its inner edge rises out of the ground over a band there) and built again once the centre has moved
// a cell while it holds an edge.
namespace {
constexpr float FAR_SHELL_TILE = 64.0f;
constexpr int FAR_SHELL_LEVELS = 3;
constexpr float FAR_SHELL_SPLIT[FAR_SHELL_LEVELS - 1] = { 400.0f, 1600.0f };
constexpr int FAR_SHELL_CELLS = 32;

uint32_t far_shell_hash(int p_x, int p_z, uint32_t p_salt) {
	return hash_murmur3_one_32(uint32_t(p_x), hash_murmur3_one_32(uint32_t(p_z), p_salt));
}

float far_shell_unit(uint32_t p_h, int p_shift) {
	return float((p_h >> p_shift) & 0xFFFFu) / 65535.0f;
}

// Value noise in [0, 1] on a lattice p_scale metres wide (smooth between its points), for the shell's patches.
float far_shell_noise(float p_x, float p_z, float p_scale, uint32_t p_salt) {
	const float gx = p_x / p_scale, gz = p_z / p_scale;
	const int ix = int(Math::floor(gx)), iz = int(Math::floor(gz));
	float fx = gx - ix, fz = gz - iz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float a = far_shell_unit(far_shell_hash(ix, iz, p_salt), 0), b = far_shell_unit(far_shell_hash(ix + 1, iz, p_salt), 0);
	const float c = far_shell_unit(far_shell_hash(ix, iz + 1, p_salt), 0), d = far_shell_unit(far_shell_hash(ix + 1, iz + 1, p_salt), 0);
	return Math::lerp(Math::lerp(a, b, fx), Math::lerp(c, d, fx), fz);
}

// The far shell's clump field (three value-noise octaves, 7, 19 and 53 m, weights 0.5, 0.3, 0.2, each scaled to
// [-1, 1]) is far from uniform: most of it sits near 0. Its quantile, from a table sampled once, so a threshold at
// 1 - cover leaves the cover's share of the ground under clumps.
float far_shell_clump_quantile(float p_v) {
	static float table[257];
	static bool built = false;
	if (!built) {
		LocalVector<float> v;
		v.resize(256 * 256);
		const float scales[3] = { 7.0f, 19.0f, 53.0f };
		const float amps[3] = { 0.5f, 0.3f, 0.2f };
		for (int j = 0; j < 256; j++) {
			for (int i = 0; i < 256; i++) {
				float d = 0.0f;
				for (int o = 0; o < 3; o++) {
					d += amps[o] * (far_shell_noise(i * 3.71f + 0.3f, j * 3.71f + 0.7f, scales[o], 0x68E31DA4u + uint32_t(o) * 0x9E3779B9u) - 0.5f) * 2.0f;
				}
				v[j * 256 + i] = d;
			}
		}
		v.sort();
		for (int k = 0; k <= 256; k++) {
			table[k] = v[MIN(k * 256, int(v.size()) - 1)];
		}
		built = true;
	}
	if (p_v <= table[0]) {
		return 0.0f;
	}
	if (p_v >= table[256]) {
		return 1.0f;
	}
	int lo = 0, hi = 256;
	while (hi - lo > 1) {
		const int mid = (lo + hi) / 2;
		if (table[mid] <= p_v) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float span = table[hi] - table[lo];
	return (lo + (span > 0.0f ? (p_v - table[lo]) / span : 0.0f)) / 256.0f;
}

// The distances a level's tiles cover.
void far_shell_band(int p_level, float p_start, float p_end, float &r_lo, float &r_hi) {
	r_lo = p_level > 0 ? MAX(p_start, FAR_SHELL_SPLIT[p_level - 1]) : p_start;
	r_hi = p_level < FAR_SHELL_LEVELS - 1 ? MIN(p_end, FAR_SHELL_SPLIT[p_level]) : p_end;
}
} // namespace

void VegetationField3D::set_far_shell(bool p_on) {
	far_shell = p_on;
	if (!far_shell) {
		_free_far_shell();
	}
}

void VegetationField3D::set_far_shell_clumps(bool p_enabled) {
	if (far_shell_clumps != p_enabled) {
		far_shell_clumps = p_enabled;
		_free_far_shell(); // Rebuilt on the next update.
	}
}

void VegetationField3D::_free_far_shell() {
	for (KeyValue<Vector3i, FarShellTile> &kv : far_shell_tiles) {
		if (kv.value.mi) {
			if (kv.value.mi->get_parent()) {
				kv.value.mi->get_parent()->remove_child(kv.value.mi);
			}
			memdelete(kv.value.mi);
		}
	}
	far_shell_tiles.clear();
	far_shell_columns = 0;
}

// A plant mesh's mean albedo as the plant shader draws it (linear): base_color times the vertex colour, the worn
// share (vertex alpha) yellowed and browned as the shader does, by triangle area over every surface. The far shell's
// colour, so the far stand has the near plants' own hue (a dry sward's gold, not its leaves' green).
Color VegetationField3D::_mesh_albedo(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return Color(0, 0, 0, 0);
	}
	double r = 0.0, g = 0.0, b = 0.0, area = 0.0;
	for (int si = 0; si < p_mesh->get_surface_count(); si++) {
		if (p_mesh->surface_get_primitive_type(si) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arr = p_mesh->surface_get_arrays(si);
		const PackedVector3Array v = arr[Mesh::ARRAY_VERTEX];
		const PackedColorArray c = arr[Mesh::ARRAY_COLOR];
		const PackedInt32Array ix = arr[Mesh::ARRAY_INDEX];
		Color base(1, 1, 1), dead_c(0.36f, 0.24f, 0.11f);
		const Ref<ShaderMaterial> sm = p_mesh->surface_get_material(si);
		if (sm.is_valid() && sm->get_shader_parameter(SNAME("albedo_tex")).get_type() == Variant::OBJECT) {
			// Textured cards: their colour is the texture's (its mean, where opaque), white without it.
			const Ref<Texture2D> tex = sm->get_shader_parameter(SNAME("albedo_tex"));
			Ref<Image> img = tex.is_valid() ? tex->get_image() : Ref<Image>();
			if (img.is_null() || img->is_empty()) {
				return Color(0, 0, 0, 0); // Unknown: the far colour stands.
			}
			img = img->duplicate();
			if (img->is_compressed()) {
				img->decompress();
			}
			img->convert(Image::FORMAT_RGBA8);
			const int step = MAX(1, MAX(img->get_width(), img->get_height()) / 64);
			double tr = 0.0, tg = 0.0, tb = 0.0, tw = 0.0;
			for (int y = 0; y < img->get_height(); y += step) {
				for (int x = 0; x < img->get_width(); x += step) {
					const Color px = img->get_pixel(x, y);
					if (px.a < 0.5f) {
						continue;
					}
					const Color l = px.srgb_to_linear();
					tr += l.r;
					tg += l.g;
					tb += l.b;
					tw += 1.0;
				}
			}
			if (tw <= 0.0) {
				return Color(0, 0, 0, 0);
			}
			base = Color(tr / tw, tg / tw, tb / tw);
		} else if (sm.is_valid()) {
			const Variant bc = sm->get_shader_parameter(SNAME("base_color"));
			if (bc.get_type() == Variant::COLOR) {
				base = Color(bc).srgb_to_linear();
			}
			const Variant dc = sm->get_shader_parameter(SNAME("dead_color"));
			if (dc.get_type() == Variant::COLOR) {
				dead_c = Color(dc).srgb_to_linear();
			}
		}
		const int tris = ix.is_empty() ? v.size() / 3 : ix.size() / 3;
		for (int t = 0; t < tris; t++) {
			const int i0 = ix.is_empty() ? t * 3 : ix[t * 3], i1 = ix.is_empty() ? t * 3 + 1 : ix[t * 3 + 1], i2 = ix.is_empty() ? t * 3 + 2 : ix[t * 3 + 2];
			const float a = 0.5f * (v[i1] - v[i0]).cross(v[i2] - v[i0]).length();
			if (a <= 0.0f) {
				continue;
			}
			Color vc = c.size() == v.size() ? (c[i0] + c[i1] + c[i2]) / 3.0f : Color(1, 1, 1, 1);
			Color al(base.r * vc.r, base.g * vc.g, base.b * vc.b);
			const float dead = 1.0f - vc.a;
			if (dead > 0.0f) {
				const float luma = al.r * 0.2126f + al.g * 0.7152f + al.b * 0.0722f;
				const float ky = CLAMP(luma * 8.0f, 0.6f, 1.4f), kb = CLAMP(luma * 6.0f, 0.55f, 1.5f);
				const Color yellow(0.30f * ky, 0.20f * ky, 0.025f * ky);
				const Color brown(dead_c.r * kb, dead_c.g * kb, dead_c.b * kb);
				const Color worn = yellow.lerp(brown, Math::smoothstep(0.35f, 0.85f, dead));
				al = al.lerp(worn, Math::smoothstep(0.0f, 0.5f, dead));
			}
			r += al.r * a;
			g += al.g * a;
			b += al.b * a;
			area += a;
		}
	}
	if (area <= 0.0) {
		return Color(0, 0, 0, 0);
	}
	return Color(r / area, g / area, b / area, 1.0f);
}

// Where the shell starts: inside the streamed plants' thinning, so it rises under them.
float VegetationField3D::_far_shell_start() const {
	float reach = 0.0f;
	for (const PlaceSpecies &ps : stream_setup.species) {
		if (ps.sp.is_valid()) {
			// An unranged species is drawn as far as the field streams.
			reach = MAX(reach, ps.range > 0.0f ? MAX(ps.reach, ps.range) : stream_radius);
		}
	}
	const float streamed = MIN(stream_radius, reach > 0.0f ? reach : stream_radius);
	return MAX(streamed * 0.6f, 4.0f);
}

// Returns true while tiles are still to build.
bool VegetationField3D::_update_far_shell(const Vector2 &p_centre, uint64_t p_t0, uint64_t p_budget) {
	if (!far_shell || !stream_setup_valid || !stream_setup.map) {
		if (!far_shell_tiles.is_empty()) {
			_free_far_shell();
		}
		return false;
	}
	const float start = _far_shell_start();
	const float end = far_shell_range;
	if (end <= start) {
		if (!far_shell_tiles.is_empty()) {
			_free_far_shell();
		}
		return false;
	}
	struct Want {
		Vector3i key;
		float d;
		bool operator<(const Want &o) const { return d < o.d; }
	};
	LocalVector<Want> want;
	HashSet<Vector3i> wanted;
	float size = FAR_SHELL_TILE;
	for (int level = 0; level < FAR_SHELL_LEVELS; level++, size *= 4.0f) {
		const float cell = size / FAR_SHELL_CELLS;
		float lo_d, hi_d;
		far_shell_band(level, start, end, lo_d, hi_d);
		if (hi_d <= lo_d) {
			continue;
		}
		const Vector2i lo(int(Math::floor((p_centre.x - hi_d) / size)), int(Math::floor((p_centre.y - hi_d) / size)));
		const Vector2i hi(int(Math::floor((p_centre.x + hi_d) / size)), int(Math::floor((p_centre.y + hi_d) / size)));
		for (int z = lo.y; z <= hi.y; z++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector2 p0(x * size, z * size);
				const Vector2 nearest(CLAMP(p_centre.x, p0.x, p0.x + size), CLAMP(p_centre.y, p0.y, p0.y + size));
				const float d_near = nearest.distance_to(p_centre);
				const float d_far = Vector2(MAX(Math::abs(p_centre.x - p0.x), Math::abs(p_centre.x - p0.x - size)), MAX(Math::abs(p_centre.y - p0.y), Math::abs(p_centre.y - p0.y - size))).length();
				if (d_near > hi_d || d_far < lo_d || !stream_setup.rect.intersects(Rect2(p0, Vector2(size, size)))) {
					continue;
				}
				const Vector3i k(x, z, level);
				wanted.insert(k);
				const FarShellTile *have = far_shell_tiles.getptr(k);
				// Holding an edge (or the inner rise): built again once the centre has moved a cell.
				const bool edge = d_near < lo_d + start * 0.5f + cell || d_far > hi_d;
				if (!have || (edge && have->built_at.distance_to(p_centre) > cell)) {
					want.push_back({ k, d_near });
				}
			}
		}
	}
	want.sort();
	bool busy = false;
	const bool first = far_shell_tiles.is_empty();
	for (const Want &w : want) {
		if (!first && OS::get_singleton()->get_ticks_usec() - p_t0 > p_budget) {
			busy = true;
			break;
		}
		FarShellTile tile;
		tile.built_at = p_centre;
		tile.mi = _build_far_shell_tile(w.key, p_centre, start, tile.columns);
		FarShellTile *have = far_shell_tiles.getptr(w.key);
		if (have) {
			far_shell_columns -= have->columns;
			if (have->mi) {
				remove_child(have->mi);
				memdelete(have->mi);
			}
			*have = tile;
		} else {
			far_shell_tiles.insert(w.key, tile);
		}
		far_shell_columns += tile.columns;
	}
	LocalVector<Vector3i> drop;
	for (const KeyValue<Vector3i, FarShellTile> &kv : far_shell_tiles) {
		if (!wanted.has(kv.key)) {
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		FarShellTile &t = far_shell_tiles[k];
		far_shell_columns -= t.columns;
		if (t.mi) {
			remove_child(t.mi);
			memdelete(t.mi);
		}
		far_shell_tiles.erase(k);
	}
	return busy;
}

// One tile of the far shell. Each cell reads every ranged species' cover there: its suitability (with its patches)
// times its density times the ground it hides (far_color alpha). The cell's canopy is their expected height (each
// species' mesh at its mean scale, by its cover), varied by a hash so the stand is lumpy, and their mean far colour
// by cover; the surface's corners take the mean of the cells round them, so it is one draped layer that thins down
// into the ground where the cover thins, with no boxes (the "canopy height model" of remote sensing, and the far
// grass shell's idea). It rises out of the ground over a band inside the streamed plants' thinning. Species that
// need the crowns' shade, sand, a coast or pools are left out: their rules cannot be read this far out.
MeshInstance3D *VegetationField3D::_build_far_shell_tile(const Vector3i &p_key, const Vector2 &p_centre, float p_start, int &r_columns) {
	r_columns = 0;
	const PlaceSetup &su = stream_setup;
	TerrainDataMap3D *map = su.map;
	struct Kind {
		const PlantSpecies *sp;
		float height;
		Color color;
		float cover; // At most.
		float reach; // m: where its own plants end (its range, else the field's stream radius).
	};
	LocalVector<Kind> kinds;
	for (const PlaceSpecies &ps : su.species) {
		// Unranged species too: they end where the field stops streaming (a heath's gorse and heather would leave the
		// ground bare past the stream radius otherwise).
		if (ps.sp.is_null() || ps.far_color.a <= 0.0f || ps.meshes.is_empty() || ps.meshes[0].is_null() || ps.nebkha_slot >= 0) {
			continue;
		}
		const PlantSpecies *sp = ps.sp.ptr();
		if (sp->get_rule_range(PlantSpecies::RULE_SHADE).x > 0.0f || sp->uses_dune() || sp->uses_coast() || sp->uses_spring() || sp->uses_pool() || ps.deadwood_reach > 0.0f || _context_only(sp)) {
			continue;
		}
		// A species whose own far form takes over past its plants (meta "far_shell" false: the trees module's crown
		// hulls and skyline trees) stays out of the shell.
		// Trees (a crown that shades the field, or a far tail of their own, meta far_widen) never: their own far forms
		// carry them (drawn tails, crown hulls, skyline trees), and the shell drew their mass from their habitat rules
		// alone, without the stand's patches, clearings or extent, as olive mounds metres tall over open hillsides
		// (landscape valley, 23 Nov: a 60 m forest raised a "forest" of shell over every slope to the skyline).
		if (!bool(sp->get_meta(SNAME("far_shell"), true)) || sp->get_canopy_radius() > 0.0f || bool(sp->get_meta(SNAME("far_widen"), false))) {
			continue;
		}
		const Vector2 sr = sp->get_scale_range();
		const float h = ps.meshes[0]->get_aabb().get_end().y * 0.5f * (sr.x + sr.y);
		// The share of ground its leaves hide where it grows at full suitability: its plants a square metre (a Poisson
		// disc pattern of its spacing holds about 0.7 / spacing^2, times its density), each a disc of its crown's
		// half width at its mean scale, half of it leaves, overlapping at random (1 - e^-x).
		const AABB ab = ps.meshes[0]->get_aabb();
		const float r = 0.25f * (ab.size.x + ab.size.z) * 0.5f * (sr.x + sr.y);
		const float per_m2 = 0.7f * sp->get_density() / MAX(sp->get_spacing() * sp->get_spacing(), 1e-4f);
		const float cover = 1.0f - Math::exp(-per_m2 * Math::PI * r * r * 0.5f) * 1.0f;
		if (h < 0.5f || cover <= 0.01f) {
			// Low herbs and grasses: the sward's own far blades and columns (GrassField3D) and the ground's far colour
			// carry them; a layer here would stand over the far blades and hide them.
			continue;
		}
		// The plant's own drawn hue when known (its far colour is its leaves' only: a dry sward is gold, not green),
		// no brighter than a little over its far colour: white umbels or petals are a small share of a plant seen
		// from afar, under and between its leaves.
		Color tone = ps.far_color;
		if (ps.far_albedo.a > 0.0f) {
			const float lf = ps.far_color.get_luminance();
			const float la = MAX(ps.far_albedo.get_luminance(), 1e-4f);
			const float k = MIN(la, 1.3f * lf) / la;
			tone = Color(ps.far_albedo.r * k, ps.far_albedo.g * k, ps.far_albedo.b * k, 1.0f);
		}
		kinds.push_back({ sp, h, tone, cover, ps.range > 0.0f ? ps.range : stream_radius });
		if (OS::get_singleton()->get_environment("VEG_FAR_SHELL_DEBUG") == "1" && p_key == Vector3i(0, 0, 0)) {
			print_line(vformat("far shell kind h %.2f cover %.2f spacing %.2f density %.2f", h, cover, sp->get_spacing(), sp->get_density()));
		}
	}
	if (kinds.is_empty()) {
		return nullptr;
	}
	const int level = p_key.z;
	float size = FAR_SHELL_TILE;
	for (int l = 0; l < level; l++) {
		size *= 4.0f;
	}
	const float cell = size / FAR_SHELL_CELLS;
	float lo_d, hi_d;
	far_shell_band(level, p_start, far_shell_range, lo_d, hi_d);
	const float rise = p_start * 0.5f; // The band it rises out of the ground over, past p_start.
	const int bx = p_key.x * FAR_SHELL_CELLS;
	const int bz = p_key.y * FAR_SHELL_CELLS;
	const int n = FAR_SHELL_CELLS + 2;
	const float gy = get_global_position().y;
	const uint32_t salt = 0x7F4A7C15u;
	// Cells with a ring of neighbours, so the tiles' edges agree.
	LocalVector<float> hgt;
	LocalVector<Color> col;
	hgt.resize(n * n);
	col.resize(n * n);
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const int cx = bx + i - 1, cz = bz + j - 1;
			const Vector2 q((cx + 0.5f) * cell, (cz + 0.5f) * cell);
			hgt[j * n + i] = 0.0f;
			col[j * n + i] = Color(0, 0, 0, 0);
			if (!su.rect.has_point(q)) {
				continue;
			}
			const Vector3 at(q.x, gy, q.y);
			float none = 1.0f;
			float sum = 0.0f;
			float hs = 0.0f;
			Color cs(0, 0, 0, 0);
			const float dq = q.distance_to(p_centre);
			for (const Kind &k : kinds) {
				// Each kind takes over from its own plants only where they end (the last 15% of their range): inside
				// it the solid shell showed between airy plants as a flat wall of peaks (landscape meadow view, 23 Nov,
				// a 2.3 m tall grass drawn to 60 m under a shell from 48 m).
				const float handover = Math::smoothstep(0.85f * k.reach, k.reach, dq);
				const float c = handover > 0.0f ? k.cover * handover * CLAMP(k.sp->get_suitability(map, at), 0.0f, 1.0f) : 0.0f;
				if (c <= 0.0f) {
					continue;
				}
				sum += c;
				hs += c * k.height;
				cs += k.color * c;
				none *= 1.0f - c;
			}
			if (sum <= 0.0f) {
				continue;
			}
			const float cover = 1.0f - none;
			const uint32_t h = far_shell_hash(cx, cz, salt ^ uint32_t(level));
			// Expected height where it grows, by the cover; clumped by the hash (more variation where it is thin).
			const float lump = 1.0f + (far_shell_unit(h, 0) - 0.5f) * (0.5f + 0.6f * (1.0f - cover));
			// Clumps and gaps (docs/ideas/vegetation.md, "Next round", 3): heath, gorse and scrub stand in clumps with
			// open ground between, which is what keeps a far hillside's grain (the canopy height models of remote
			// sensing show the same). Where the cells are fine enough to hold them, the stand stands at its full mass
			// where a clump field (three octaves, 7, 19 and 53 m) is over its cover's threshold and opens to the ground
			// elsewhere, so about its cover's share of the ground is under clumps; each octave fades out as the cells
			// grow past about a third of its size (prefiltered, as a mip), leaving the even mass by cover.
			float dev = 0.0f, act = 0.0f;
			const float scales[3] = { 7.0f, 19.0f, 53.0f };
			const float amps[3] = { 0.5f, 0.3f, 0.2f };
			for (int o = 0; o < (far_shell_clumps ? 3 : 0); o++) {
				const float a = 1.0f - Math::smoothstep(0.35f, 0.6f, cell / scales[o]);
				if (a > 0.0f) {
					dev += a * amps[o] * (far_shell_noise(q.x, q.y, scales[o], 0x68E31DA4u + uint32_t(o) * 0x9E3779B9u) - 0.5f) * 2.0f;
					act += a * amps[o];
				}
			}
			float mass = cover;
			if (act > 0.0f) {
				// The field's own distribution (far_shell_clump_quantile()) turns it uniform, so a threshold at 1 - cover
				// puts the clumps on the cover's share of the ground.
				const float u = far_shell_clump_quantile(dev / act);
				const float open = Math::smoothstep(1.0f - cover - 0.06f, 1.0f - cover + 0.06f, u);
				mass = Math::lerp(cover, open, act);
			}
			// Its mass, not its tips: the stand's tops are mostly air, and a sward's own blades (GrassField3D's far
			// rings and columns) stand through it.
			hgt[j * n + i] = 0.4f * hs / sum * mass * lump;
			col[j * n + i] = Color(cs.r / sum, cs.g / sum, cs.b / sum, cover);
		}
	}
	// Corners: the mean of the cells round them, on the ground (jittered by a hash shared with the neighbours).
	float hc_top = 0.0f; // The tallest stand's full mass, for the clumps' shade.
	for (const Kind &k : kinds) {
		hc_top = MAX(hc_top, 0.4f * k.height);
	}
	const int m = FAR_SHELL_CELLS + 1;
	LocalVector<Vector3> corner;
	LocalVector<float> ch;
	LocalVector<Color> cc;
	corner.resize(m * m);
	ch.resize(m * m);
	cc.resize(m * m);
	for (int j = 0; j < m; j++) {
		for (int i = 0; i < m; i++) {
			const uint32_t h = far_shell_hash(bx + i, bz + j, salt ^ 0x85EBCA6Bu ^ uint32_t(level));
			const float x = (bx + i + (far_shell_unit(h, 0) - 0.5f) * 0.5f) * cell;
			const float z = (bz + j + (far_shell_unit(h, 16) - 0.5f) * 0.5f) * cell;
			float hh = 0.0f;
			Color c(0, 0, 0, 0);
			float w = 0.0f;
			for (int dj = 0; dj < 2; dj++) {
				for (int di = 0; di < 2; di++) {
					const int k = (j + dj) * n + i + di;
					hh += hgt[k];
					if (col[k].a > 0.0f) {
						c += Color(col[k].r, col[k].g, col[k].b, 1.0f) * col[k].a;
						w += col[k].a;
					}
				}
			}
			const float d = Vector2(x, z).distance_to(p_centre);
			const float up = rise > 0.0f ? CLAMP((d - p_start) / rise, 0.0f, 1.0f) : 1.0f;
			ch[j * m + i] = 0.25f * hh * up * up * (3.0f - 2.0f * up);
			cc[j * m + i] = w > 0.0f ? Color(c.r / w, c.g / w, c.b / w, CLAMP(w * 0.25f, 0.0f, 1.0f)) : Color(0, 0, 0, 0);
			corner[j * m + i] = Vector3(x, map->get_height_at(Vector3(x, gy, z)), z);
		}
	}
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedInt32Array idx;
	const Transform3D to_local = get_global_transform().affine_inverse();
	// One vertex a corner (shared), normals from the surface's slope.
	LocalVector<int> vid;
	vid.resize(m * m);
	const float lift_min = 0.03f; // Under this the corner is ground: the surface sinks a little into it there.
	for (int j = 0; j < m; j++) {
		for (int i = 0; i < m; i++) {
			vid[j * m + i] = -1;
		}
	}
	for (int j = 0; j < FAR_SHELL_CELLS; j++) {
		for (int i = 0; i < FAR_SHELL_CELLS; i++) {
			const int k[4] = { j * m + i, j * m + i + 1, (j + 1) * m + i + 1, (j + 1) * m + i };
			float top = 0.0f;
			for (int c = 0; c < 4; c++) {
				top = MAX(top, ch[k[c]]);
			}
			// Each cell kept stands inside the band and has some cover above the ground.
			const Vector2 mid = (Vector2(corner[k[0]].x, corner[k[0]].z) + Vector2(corner[k[2]].x, corner[k[2]].z)) * 0.5f;
			const float dm = mid.distance_to(p_centre);
			if (top < lift_min || dm < lo_d || dm >= hi_d) {
				continue;
			}
			r_columns++;
			for (int c = 0; c < 4; c++) {
				if (vid[k[c]] >= 0) {
					continue;
				}
				const int ci = k[c] % m, cj = k[c] / m;
				// The surface's slope from the neighbouring corners.
				const int il = MAX(ci - 1, 0), ir = MIN(ci + 1, m - 1), jl = MAX(cj - 1, 0), jr = MIN(cj + 1, m - 1);
				const Vector3 pl = corner[cj * m + il] + Vector3(0, ch[cj * m + il], 0);
				const Vector3 pr = corner[cj * m + ir] + Vector3(0, ch[cj * m + ir], 0);
				const Vector3 pd = corner[jl * m + ci] + Vector3(0, ch[jl * m + ci], 0);
				const Vector3 pu = corner[jr * m + ci] + Vector3(0, ch[jr * m + ci], 0);
				Vector3 nn = (pu - pd).cross(pr - pl).normalized();
				if (nn.y < 0.0f) {
					nn = -nn;
				}
				const float hc = ch[k[c]];
				const Vector3 p = corner[k[c]] + Vector3(0, hc >= lift_min ? hc : -0.05f, 0);
				// Down into the stand where it is low (its own shade), the colour of what grows round it.
				const Color base = cc[k[c]].a > 0.0f ? cc[k[c]] : Color(0.1f, 0.1f, 0.08f, 0.0f);
				// Mottled at the clumps' scale and in larger patches. No further darkening for the shade between tops:
				// the plants' far colour is measured on screen with their own self-shade (docs/ideas/vegetation.md, "Far
				// colour matched to the plants on screen"), and a darker shell read as black blotches next to the
				// plants it continues (dry heath bench, 20 Nov: about half their brightness). The low corners round a
				// clump sit a little in its shade, its tops in light.
				const float mottle = (0.8f + 0.4f * far_shell_noise(corner[k[c]].x, corner[k[c]].z, 3.0f, 0x2545F491u)) * (0.75f + 0.5f * far_shell_noise(corner[k[c]].x, corner[k[c]].z, 23.0f, 0x9E3779B9u));
				const float rel = hc_top > 0.0f ? CLAMP(hc / hc_top, 0.0f, 1.0f) : 1.0f;
				const float shade = mottle * (0.85f + 0.15f * rel);
				vid[k[c]] = verts.size();
				verts.push_back(to_local.xform(p));
				normals.push_back(to_local.basis.xform(nn).normalized());
				colors.push_back(Color(base.r * shade, base.g * shade, base.b * shade, 1.0f));
			}
			// Godot's front faces wind clockwise seen from the front (up here).
			idx.append_array(PackedInt32Array({ vid[k[0]], vid[k[1]], vid[k[2]], vid[k[0]], vid[k[2]], vid[k[3]] }));
		}
	}
	if (verts.is_empty()) {
		return nullptr;
	}
	// Winding check on the first triangle: facing up, or flip all.
	{
		const Vector3 a = verts[idx[0]], b = verts[idx[1]], c = verts[idx[2]];
		if ((b - a).cross(c - a).dot(to_local.basis.xform(Vector3(0, 1, 0))) > 0.0f) {
			for (int t = 0; t < idx.size(); t += 3) {
				const int tmp = idx[t + 1];
				idx.write[t + 1] = idx[t + 2];
				idx.write[t + 2] = tmp;
			}
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, _far_volume_mat());
	mesh->set_meta(SNAME("far_volume"), true);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(mesh);
	// Under a pixel's shadow this far out; the path tracer traces them anyway.
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mi->set_name(vformat("FarShell_%d_%d_%d", p_key.x, p_key.y, level));
	add_child(mi, false, INTERNAL_MODE_BACK);
	return mi;
}

// The maps from placed tiles (canopy, stand shade, forest floor, far colour), computed from the
// tiles' crowns and far colour alone, so streaming runs it on a worker; _apply_maps() hands the
// result to the field and the renderer on the main thread.
void VegetationField3D::_compute_maps(Maps &r_maps, const LocalVector<const LocalVector<Crown> *> &p_crowns, const LocalVector<const LocalVector<FarAdd> *> &p_far) {
	// The crowns as the field's canopy, in a hash grid (get_shade_at()).
	for (const LocalVector<Crown> *list : p_crowns) {
		for (const Crown &c : *list) {
			r_maps.crowns.push_back(c);
			r_maps.grid[Vector2i(int(Math::floor(c.xz.x / CROWN_CELL)), int(Math::floor(c.xz.y / CROWN_CELL)))].push_back(r_maps.crowns.size() - 1);
			r_maps.max_radius = MAX(r_maps.max_radius, c.radius);
		}
	}
	_apply_canopy_losses(r_maps.crowns, r_maps.canopy_losses);
	const float reach = r_maps.max_radius * 1.4f;
	const Rect2 &rect = r_maps.maps_rect;
	if (!r_maps.crowns.is_empty() && rect.has_area()) {
		if (r_maps.want_canopy) {
			// The placed crowns as WeatherMap3D's canopy_map (height in red, leaf area / 4 in
			// green), so snow interception, wind in the stand and anything else reading
			// get_canopy_closure_at() sees the same forest. A crown's opacity is Beer's law with
			// the weather map's extinction of 0.5, opacity = 1 - exp(-0.5 LAI), so
			// LAI = -2 ln(1 - opacity) and overlapping crowns add their leaf area exactly as
			// get_shade_at() multiplies their transmission: closure there equals this shade.
			const int w = CLAMP(int(Math::ceil(rect.size.x)), 1, 1024); // About 1 m cells.
			const int h = CLAMP(int(Math::ceil(rect.size.y)), 1, 1024);
			r_maps.canopy = Image::create_empty(w, h, false, Image::FORMAT_RGF);
			const int r = int(Math::ceil(reach / CROWN_CELL));
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const Vector2 p = rect.position + Vector2((x + 0.5f) * rect.size.x / w, (y + 0.5f) * rect.size.y / h);
					const Vector2i gc(int(Math::floor(p.x / CROWN_CELL)), int(Math::floor(p.y / CROWN_CELL)));
					float lai = 0.0f;
					float top = 0.0f;
					float best = 0.0f;
					for (int dz = -r; dz <= r; dz++) {
						for (int dx = -r; dx <= r; dx++) {
							const LocalVector<int> *bucket = r_maps.grid.getptr(gc + Vector2i(dx, dz));
							if (!bucket) {
								continue;
							}
							for (int idx : *bucket) {
								const Crown &c = r_maps.crowns[idx];
								const float d = c.xz.distance_to(p) / c.radius;
								if (d >= 1.4f) {
									continue;
								}
								// The same crown term as _shade_at(), as leaf area.
								const float o = MIN(c.opacity * (1.0f - Math::smoothstep(0.6f, 1.4f, d)), 0.999f);
								const float l = -2.0f * Math::log(1.0f - o);
								lai += l;
								if (l > best) {
									best = l;
									top = c.top;
								}
							}
						}
					}
					r_maps.canopy->set_pixel(x, y, Color(lai > 0.0f ? MAX(top, 0.1f) : 0.0f, lai * 0.25f, 0.0f));
				}
			}
		}

		// The shade under the stand, on half-metre cells: the crowns' shade weighed by how closed
		// the stand is. Under one open-grown tree the ground keeps its sward and gets no litter
		// mat, as an open-grown tree sheds onto grass that keeps growing, so its leaves blow away
		// or rot in the sward; under a closed canopy it is the full shade. The forest floor map
		// and the grass's shade rule (get_stand_shade_at()) both read it.
		const int w = CLAMP(int(Math::ceil(rect.size.x * 2.0f)), 1, 2048);
		const int h = CLAMP(int(Math::ceil(rect.size.y * 2.0f)), 1, 2048);
		LocalVector<float> shade;
		LocalVector<float> &conifer = r_maps.stand_conifer;
		shade.resize(w * h);
		conifer.resize(w * h);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const Vector2 p = rect.position + Vector2((x + 0.5f) * rect.size.x / w, (y + 0.5f) * rect.size.y / h);
				shade[y * w + x] = crown_shade(r_maps.grid, r_maps.crowns, reach, CROWN_CELL, p, &conifer[y * w + x]);
			}
		}
		// The closure of the stand: the mean shade over about 5 m around (a separable box blur).
		const int rad_x = MAX(1, int(Math::round(5.0f * w / rect.size.x)));
		const int rad_y = MAX(1, int(Math::round(5.0f * h / rect.size.y)));
		LocalVector<float> tmp;
		tmp.resize(w * h);
		for (int y = 0; y < h; y++) {
			float sum = 0.0f;
			int n = 0;
			for (int x = -rad_x; x < w + rad_x; x++) {
				const int add = x + rad_x;
				if (add >= 0 && add < w) {
					sum += shade[y * w + add];
					n++;
				}
				const int rem = x - rad_x - 1;
				if (rem >= 0 && rem < w) {
					sum -= shade[y * w + rem];
					n--;
				}
				if (x >= 0 && x < w) {
					tmp[y * w + x] = n > 0 ? sum / n : 0.0f;
				}
			}
		}
		LocalVector<float> &stand = r_maps.stand_shade;
		stand.resize(w * h);
		for (int x = 0; x < w; x++) {
			float sum = 0.0f;
			int n = 0;
			for (int y = -rad_y; y < h + rad_y; y++) {
				const int add = y + rad_y;
				if (add >= 0 && add < h) {
					sum += tmp[add * w + x];
					n++;
				}
				const int rem = y - rad_y - 1;
				if (rem >= 0 && rem < h) {
					sum -= tmp[rem * w + x];
					n--;
				}
				if (y >= 0 && y < h) {
					stand[y * w + x] = n > 0 ? sum / n : 0.0f;
				}
			}
		}
		for (int i = 0; i < w * h; i++) {
			stand[i] = shade[i] * Math::smoothstep(0.25f, 0.65f, stand[i]);
		}
		r_maps.stand_size = Vector2i(w, h);

		if (r_maps.want_floor) {
			// The forest floor for the ground's shader (get_floor_shader_code()): litter cover in
			// red and the conifer share of the cover in green.
			// Needles and leaves fall under every crown, not only in a closed stand: an open stand
			// (stone pine, parkland oaks) keeps a mat under and just past each crown, thinning into
			// drifts between them (litterfall falls off within about a crown's height of the stem;
			// Ferrari and Sugita 1996). The shade blurred over about 2 m carries it past the crown.
			const int lr_x = MAX(1, int(Math::round(2.0f * w / rect.size.x)));
			const int lr_y = MAX(1, int(Math::round(2.0f * h / rect.size.y)));
			LocalVector<float> fall;
			fall.resize(w * h);
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					float sum = 0.0f;
					int n = 0;
					for (int dx = -lr_x; dx <= lr_x; dx++) {
						const int xx = x + dx;
						if (xx >= 0 && xx < w) {
							sum += shade[y * w + xx];
							n++;
						}
					}
					tmp[y * w + x] = n > 0 ? sum / n : 0.0f;
				}
			}
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					float sum = 0.0f;
					int n = 0;
					for (int dy = -lr_y; dy <= lr_y; dy++) {
						const int yy = y + dy;
						if (yy >= 0 && yy < h) {
							sum += tmp[yy * w + x];
							n++;
						}
					}
					fall[y * w + x] = n > 0 ? sum / n : 0.0f;
				}
			}
			Vector<uint8_t> data;
			data.resize(w * h * 2);
			uint8_t *d = data.ptrw();
			for (int i = 0; i < w * h; i++) {
				const float litter = MAX(stand[i], 0.9f * Math::smoothstep(0.04f, 0.3f, fall[i]));
				d[i * 2 + 0] = uint8_t(CLAMP(litter, 0.0f, 1.0f) * 255.0f + 0.5f);
				d[i * 2 + 1] = uint8_t(CLAMP(conifer[i], 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			r_maps.floor = Image::create_from_data(w, h, false, Image::FORMAT_RG8, data);
		}
	}

	// Plants beyond their range as ground colour (vegetation_far_map, read by
	// get_floor_shader_code()'s vegetation_far()): the mean colour of the plants on each cell of
	// about 1 m and the share of ground they hide, and a mean range over the rect, so the ground
	// takes over as the plant shader thins them.
	const Rect2 &far_rect = r_maps.far_rect;
	if (!r_maps.want_far || !far_rect.has_area()) {
		return;
	}
	const Vector2i fs(CLAMP(int(Math::ceil(far_rect.size.x)), 1, 2048), CLAMP(int(Math::ceil(far_rect.size.y)), 1, 2048));
	LocalVector<FarCell> cells;
	cells.resize(fs.x * fs.y);
	const float cell_area = (far_rect.size.x / fs.x) * (far_rect.size.y / fs.y);
	float range_sum = 0.0f;
	float cover_sum = 0.0f;
	for (const LocalVector<FarAdd> *list : p_far) {
		for (const FarAdd &f : *list) {
			if (!far_rect.has_point(f.xz)) {
				continue;
			}
			const int fx = CLAMP(int((f.xz.x - far_rect.position.x) / far_rect.size.x * fs.x), 0, fs.x - 1);
			const int fz = CLAMP(int((f.xz.y - far_rect.position.y) / far_rect.size.y * fs.y), 0, fs.y - 1);
			const float c = MIN(f.area / cell_area, 4.0f * f.color.a);
			FarCell &fc = cells[fz * fs.x + fx];
			fc.r += f.color.r * c;
			fc.g += f.color.g * c;
			fc.b += f.color.b * c;
			fc.cover += c;
			fc.range += f.range * c;
			range_sum += f.range * c;
			cover_sum += c;
		}
	}
	if (cover_sum <= 0.0f) {
		return; // Nothing to show far away; another field may own the map.
	}
	// Premultiplied linear colour, stored as its square root in 8 bits (dark plant colours keep
	// their precision; the shader squares it back), so filtering between a patch and bare cells
	// fades the cover without darkening the colour. 8-bit RGBA: the path tracer's material
	// evaluation reads it as the raster does (a half-float map came back white there).
	r_maps.far = Image::create_empty(fs.x, fs.y, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < fs.y; y++) {
		for (int x = 0; x < fs.x; x++) {
			const FarCell &c = cells[y * fs.x + x];
			const float inv = c.cover > 0.0f ? 1.0f / c.cover : 0.0f;
			// Overlapping plants: cover adds as 1 - exp(-sum), like overlapping crowns.
			const float a = 1.0f - Math::exp(-c.cover);
			r_maps.far->set_pixel(x, y, Color(Math::sqrt(c.r * inv * a), Math::sqrt(c.g * inv * a), Math::sqrt(c.b * inv * a), a));
		}
	}
	r_maps.far_range = range_sum / cover_sum;
}

void VegetationField3D::_maps_wants(Maps &r_maps) const {
	r_maps.canopy_losses = canopy_losses;
	r_maps.want_canopy = !weather_map.is_empty() && is_inside_tree();
	r_maps.want_floor = forest_floor_enabled && is_inside_tree();
	r_maps.want_far = true;
}

void VegetationField3D::_apply_maps(Maps &p_maps) {
	{
		RWLockWrite guard(crown_lock);
		crowns = p_maps.crowns;
		crown_grid = p_maps.grid;
		max_crown_radius = p_maps.max_radius;
		stand_rect = p_maps.maps_rect;
		stand_size = p_maps.stand_size;
		stand_shade = p_maps.stand_shade;
		stand_conifer = p_maps.stand_conifer;
	}
	if (!is_inside_tree()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!weather_map.is_empty()) {
		WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
		ERR_FAIL_NULL_MSG(wm, "VegetationField3D: weather_map must point to a WeatherMap3D.");
		if (p_maps.canopy.is_valid()) {
			wm->set_canopy_map_rect(p_maps.maps_rect);
			wm->set_canopy_map(ImageTexture::create_from_image(p_maps.canopy));
		} else {
			wm->set_canopy_map(Ref<Texture2D>());
		}
	}
	if (forest_floor_enabled && p_maps.floor.is_valid()) {
		// An empty field leaves the floor map alone: another field with crowns may own it.
		floor_texture = ImageTexture::create_from_image(p_maps.floor);
		rs->global_shader_parameter_set("forest_floor_map", floor_texture->get_rid());
		rs->global_shader_parameter_set("forest_floor_rect", Vector4(p_maps.maps_rect.position.x, p_maps.maps_rect.position.y, p_maps.maps_rect.size.x, p_maps.maps_rect.size.y));
	}
	if (p_maps.far.is_valid()) {
		far_texture = ImageTexture::create_from_image(p_maps.far);
		rs->global_shader_parameter_set("vegetation_far_map", far_texture->get_rid());
		rs->global_shader_parameter_set("vegetation_far_rect", Vector4(p_maps.far_rect.position.x, p_maps.far_rect.position.y, p_maps.far_rect.size.x, p_maps.far_rect.size.y));
		rs->global_shader_parameter_set("vegetation_far_range", p_maps.far_range);
		far_image = p_maps.far;
		far_image_rect = p_maps.far_rect;
		far_image_range = p_maps.far_range;
		if (!plant_color_registered && is_inside_tree()) {
			WorldContracts::register_plant_color(this); // The plants' colour over the ground for WorldGround.albedo_at.
			plant_color_registered = true;
		}
		_publish_far_shade();
		if (far_match_view && far_view_state == 0 && far_view_round == 0) {
			_far_view_start(30);
		}
		if (far_match_view && is_inside_tree()) {
			set_process_internal(true);
		}
	}
}

namespace {

// A density mask texel, bilinear, clamped to its rect; 1 outside it (as _place() reads it).
float cover_mask_at(const Image *p_mask, const Rect2 &p_rect, const Vector2 &p_xz, int p_channel) {
	if (!p_mask || p_mask->is_empty() || !p_rect.has_point(p_xz)) {
		return 1.0f;
	}
	const Vector2 uv = (p_xz - p_rect.position) / p_rect.size;
	const float fx = uv.x * p_mask->get_width() - 0.5f;
	const float fy = uv.y * p_mask->get_height() - 0.5f;
	const int x0 = CLAMP(int(Math::floor(fx)), 0, p_mask->get_width() - 1);
	const int y0 = CLAMP(int(Math::floor(fy)), 0, p_mask->get_height() - 1);
	const int x1 = MIN(x0 + 1, p_mask->get_width() - 1);
	const int y1 = MIN(y0 + 1, p_mask->get_height() - 1);
	const float ax = CLAMP(fx - Math::floor(fx), 0.0f, 1.0f);
	const float ay = CLAMP(fy - Math::floor(fy), 0.0f, 1.0f);
	const Color c = p_mask->get_pixel(x0, y0).lerp(p_mask->get_pixel(x1, y0), ax).lerp(p_mask->get_pixel(x0, y1).lerp(p_mask->get_pixel(x1, y1), ax), ay);
	return c[p_channel];
}

uint8_t cover_srgb8(float p_c) {
	p_c = CLAMP(p_c, 0.0f, 1.0f);
	const float s = p_c < 0.0031308f ? p_c * 12.92f : 1.055f * Math::pow(p_c, 1.0f / 2.4f) - 0.055f;
	return uint8_t(s * 255.0f + 0.5f);
}

} // namespace

// The plant cover per texel as seen from far above, from the rules rather than placed plants, so it
// reaches every kilometre of the map while only the camera's surroundings are placed (the far
// field colour maps of Frostbite, Andersson 2007, and Far Cry 5, McLaren 2018). Three layers:
// the crowns (expected crown area per square metre through Beer's law with their opacity, as the
// canopy map), the plants under them (their far colours, by the shade and stand type those crowns
// give, as vegetation_far() weighs placed plants) and the grass under those (its rules and far
// colour). The count per square metre is each species' Poisson pattern density times its density
// times its suitability; footprints between species are not modelled at this scale.
void VegetationField3D::_cover_row(void *p_job, uint32_t p_row) {
	const CoverJob &job = *static_cast<const CoverJob *>(p_job);
	const PlaceSetup &setup = *job.setup;
	const TerrainDataMap3D *map = setup.map;
	const Image *mask = setup.mask_img.ptr();
	const float ts2 = setup.tile_size * setup.tile_size;
	(void)ts2;
	// Crowns without a far colour: a sunlit broadleaf or needle crown from above (linear).
	const Vector3 broadleaf(0.045f, 0.075f, 0.025f);
	const Vector3 needles(0.025f, 0.045f, 0.022f);
	for (int x = 0; x < job.w; x++) {
		const Vector2 xz = job.rect.position + Vector2((x + 0.5f) * job.rect.size.x / job.w, (p_row + 0.5f) * job.rect.size.y / job.h);
		const Vector3 p(xz.x, 0.0f, xz.y);
		// Suitability averaged over four points of the texel, so patches smaller than a texel
		// (patch_scale) come out as their mean cover instead of speckle that shimmers far away.
		const Vector2 sub = Vector2(job.rect.size.x / job.w, job.rect.size.y / job.h) * 0.25f;
		const Vector3 subs[4] = { p + Vector3(-sub.x, 0, -sub.y), p + Vector3(sub.x, 0, -sub.y), p + Vector3(-sub.x, 0, sub.y), p + Vector3(sub.x, 0, sub.y) };
		auto suit_at = [&](const PlaceSpecies &p_ps) {
			float sum = 0.0f;
			for (const Vector3 &q : subs) {
				float suit = p_ps.sp->get_suitability(map, q);
				if (p_ps.sp->get_mask_channel() >= 0 && suit > 0.0f) {
					const float m = CLAMP(cover_mask_at(mask, setup.mask_rect, Vector2(q.x, q.z), p_ps.sp->get_mask_channel()), 0.0f, 1.0f);
					suit *= p_ps.sp->is_mask_invert() ? 1.0f - m : m;
				}
				sum += suit;
			}
			return sum * 0.25f * p_ps.sp->get_density();
		};
		float *cr = const_cast<float *>(job.crowns.ptr()) + (int64_t(p_row) * job.w + x) * 5;
		if (job.pass == 0) {
			// Crowns.
			float k_sum = 0.0f;
			float k_conifer = 0.0f;
			Vector3 crown;
			for (uint32_t s = 0; s < setup.species.size(); s++) {
				const PlaceSpecies &ps = setup.species[s];
				if (job.crown_k[s].is_empty()) {
					continue;
				}
				const float q = CLAMP(suit_at(ps), 0.0f, 1.0f) * 10.0f;
				if (q <= 0.0f) {
					continue;
				}
				const int i = MIN(int(q), 9);
				const float k = Math::lerp(job.crown_k[s][i], job.crown_k[s][i + 1], q - i);
				const bool con = ps.sp->is_canopy_conifer();
				const Vector3 c = ps.far_color.a > 0.0f ? Vector3(ps.far_color.r, ps.far_color.g, ps.far_color.b) : (con ? needles : broadleaf);
				crown += c * k;
				k_sum += k;
				k_conifer += con ? k : 0.0f;
			}
			cr[0] = k_sum;
			cr[1] = k_conifer;
			cr[2] = crown.x;
			cr[3] = crown.y;
			cr[4] = crown.z;
			continue;
		}
		const float k_sum = cr[0];
		const float closure = 1.0f - Math::exp(-k_sum);
		const float conifer = k_sum > 0.0f ? cr[1] / k_sum : 0.0f;
		const Vector3 crown = k_sum > 0.0f ? Vector3(cr[2], cr[3], cr[4]) / k_sum : Vector3();
		// The plants under the crowns.
		float c_sum = 0.0f;
		Vector3 plants;
		for (const PlaceSpecies &ps : setup.species) {
			if (ps.sp.is_null() || ps.far_color.a <= 0.0f || (ps.sp->get_canopy_radius() > 0.0f && !ps.sp->uses_shade() && !ps.sp->uses_conifer()) || _context_only(ps.sp.ptr())) {
				continue;
			}
			float suit = suit_at(ps);
			if (ps.sp->uses_shade()) {
				suit *= ps.sp->rule_weight(PlantSpecies::RULE_SHADE, closure);
			}
			if (ps.sp->uses_conifer()) {
				suit *= ps.sp->rule_weight(PlantSpecies::RULE_CONIFER, conifer);
			}
			if (suit <= 0.0f) {
				continue;
			}
			const Vector2 sr = ps.sp->get_scale_range();
			const float rr = 0.5f * ps.sp->get_spacing() * (sr.x + sr.y) * 0.5f;
			const float c = MIN(suit * ps.pattern.points.size() / ts2 * float(Math::PI) * rr * rr * ps.far_color.a, 4.0f * ps.far_color.a);
			plants += Vector3(ps.far_color.r, ps.far_color.g, ps.far_color.b) * c;
			c_sum += c;
		}
		const float a_plants = 1.0f - Math::exp(-c_sum);
		if (c_sum > 0.0f) {
			plants *= job.plant_shade / c_sum;
		}
		// The grass under them, layer by layer from the top: a tall sward in patches over an
		// undercoat that fills the gaps between them reads as the undercoat there, not bare soil.
		float bare = 1.0f;
		Vector3 grass;
		// The light the blades grew in (GrassField3D tints them by it, VegetationLight): the far
		// colours are measured on open flat ground, so they take the same tint here. Without it a
		// meadow in a valley's shade read lighter and yellower past its blades.
		const float light = (1.0f - closure) * VegetationLight::exposure(map, setup.sun_mean, xz.x, xz.y);
		for (uint32_t l = 0; l < job.grass.size(); l++) {
			float gl = 0.0f;
			for (const Vector3 &q : subs) {
				gl += CLAMP(job.grass[l]->get_suitability(map, q), 0.0f, 1.0f) * 0.25f;
			}
			if (job.grass[l]->uses_shade()) {
				gl *= job.grass[l]->rule_weight(PlantSpecies::RULE_SHADE, closure);
			}
			const Color &fc = job.grass_far[l];
			float fr = fc.r, fg = fc.g, fb = fc.b;
			VegetationLight::tint(fr, fg, fb, light);
			grass += Vector3(fr, fg, fb) * (gl * bare);
			bare *= 1.0f - gl;
		}
		float g = 1.0f - bare;
		if (g > 0.0f) {
			grass /= g;
		}
		// Drylands (climate_bare, WorldContracts::bare_patch): the sward and cover plants keep to their clumps, so from
		// afar the ground shows the bare share between them (the patches' mean, as the terrain material draws it).
		float a_cover = a_plants;
		if (job.climate_bare) {
			const float share = WorldContracts::climate_bare_share_at(p);
			if (share > 0.0f) {
				g *= 1.0f - share;
				a_cover *= 1.0f - share;
			}
			// The climate's dry-season cured share (WorldContracts::climate_cured_share_at, the terrain's cured soil
			// cover and GrassField3D's turf) in the dry season: the far sward straw, the cover plants partly (the evergreen
			// shrubs among them keep green). The grass fields' far colours carry the season's own dryness already.
			const float cured = WorldContracts::climate_cured_share_at(p) * job.dry_season;
			if (cured > 0.0f) {
				grass = grass.lerp(job.cured, 0.9f * cured);
				plants = plants.lerp(job.cured * job.plant_shade, 0.5f * cured);
			}
		}
		const float ground_a = 1.0f - (1.0f - g) * (1.0f - a_cover);
		Vector3 ground = ground_a > 0.0f ? (grass * (g * (1.0f - a_cover)) + plants * a_cover) / ground_a : Vector3();
		const float a = 1.0f - (1.0f - ground_a) * (1.0f - closure);
		const Vector3 col = a > 0.0f ? (ground * (ground_a * (1.0f - closure)) + crown * closure) / a : Vector3();
		uint8_t *o = job.out + (int64_t(p_row) * job.w + x) * 4;
		o[0] = cover_srgb8(col.x);
		o[1] = cover_srgb8(col.y);
		o[2] = cover_srgb8(col.z);
		o[3] = uint8_t(CLAMP(a, 0.0f, 1.0f) * 255.0f + 0.5f);
	}
}

void VegetationField3D::_build_cover(const PlaceSetup &p_setup) {
	if (!cover_map_enabled || !is_inside_tree()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	CoverJob job;
	job.setup = &p_setup;
	// The plants' share carries the far match's darkening (vegetation_far_shade), as vegetation_far()
	// gives it to the same plants nearer: the grass and crowns have their own far colours.
	job.plant_shade = far_shade;
	job.climate_bare = climate_bare;
	cover_shade = far_shade;
	job.rect = p_setup.rect;
	job.w = CLAMP(int(Math::ceil(job.rect.size.x / cover_cell)), 1, 4096);
	job.h = CLAMP(int(Math::ceil(job.rect.size.y / cover_cell)), 1, 4096);
	LocalVector<NodePath> grass_paths;
	if (!cover_grass.is_empty()) {
		grass_paths.push_back(cover_grass);
	}
	for (int i = 0; i < cover_grass_layers.size(); i++) {
		grass_paths.push_back(cover_grass_layers[i]);
	}
	for (const NodePath &gp : grass_paths) {
		GrassField3D *grass = Object::cast_to<GrassField3D>(get_node_or_null(gp));
		if (!grass) {
			continue;
		}
		Ref<PlantSpecies> rules = grass->get_rules();
		if (rules.is_null()) {
			rules.instantiate(); // The grass's default rules: dry land.
		}
		job.grass.push_back(rules);
		job.grass_far.push_back(grass->get_far_color());
		if (job.grass_far.size() == 1) {
			const Color d = grass->get_dry_color().srgb_to_linear() * 0.8f;
			job.cured = Vector3(d.r, d.g, d.b);
			// The dry season by the sward's own dryness (its turf's gate, get_ground_shader_code()).
			job.dry_season = Math::smoothstep(0.3f, 0.7f, grass->get_effective_dryness());
		}
		// The grass measures its far colour from a render a few frames in: the map follows it.
		const Callable on_far = callable_mp(this, &VegetationField3D::_on_grass_far_color);
		if (!grass->is_connected(SNAME("far_color_changed"), on_far)) {
			grass->connect(SNAME("far_color_changed"), on_far);
		}
	}
	// The crowns over each canopy species' own pattern (the tile, wrapped), as _shade_at() sees
	// them: exact for the spacing of a Poisson-disk stand, which covers more ground than crowns
	// dropped at random would (Beer's law alone would need a clumping index, Nilson 1971).
	job.crown_k.resize(p_setup.species.size());
	const float ts = p_setup.tile_size;
	for (uint32_t s = 0; s < p_setup.species.size(); s++) {
		const PlaceSpecies &ps = p_setup.species[s];
		if (ps.sp.is_null() || ps.sp->get_canopy_radius() <= 0.0f || ps.sp->uses_shade() || ps.sp->uses_conifer()) {
			continue;
		}
		const Vector2 sr = ps.sp->get_scale_range();
		const float r = ps.sp->get_canopy_radius() * (sr.x + sr.y) * 0.5f;
		const float opacity = ps.sp->get_canopy_opacity();
		const int grid = CLAMP(int(Math::ceil(ts)), 8, 64);
		job.crown_k[s].resize(11);
		for (int qi = 0; qi <= 10; qi++) {
			const float keep = qi / 10.0f;
			double shade = 0.0;
			for (int gy = 0; gy < grid; gy++) {
				for (int gx = 0; gx < grid; gx++) {
					const Vector2 at((gx + 0.5f) * ts / grid, (gy + 0.5f) * ts / grid);
					float transmit = 1.0f;
					for (uint32_t i = 0; i < ps.pattern.points.size(); i++) {
						if (ps.pattern.ranks[i] >= keep) {
							continue;
						}
						for (int oy = -1; oy <= 1; oy++) {
							for (int ox = -1; ox <= 1; ox++) {
								const float d = (ps.pattern.points[i] + Vector2(ox, oy) * ts).distance_to(at) / r;
								if (d < 1.4f) {
									transmit *= 1.0f - opacity * (1.0f - Math::smoothstep(0.6f, 1.4f, d));
								}
							}
						}
					}
					shade += 1.0f - transmit;
				}
			}
			job.crown_k[s][qi] = -Math::log(MAX(1.0f - float(shade / (grid * grid)), 1e-4f));
		}
	}
	Vector<uint8_t> data;
	data.resize(job.w * job.h * 4);
	job.out = data.ptrw();
	job.crowns.resize(job.w * job.h * 5);
	WorkerThreadPool *wtp = WorkerThreadPool::get_singleton();
	job.pass = 0;
	wtp->wait_for_group_task_completion(wtp->add_native_group_task(&VegetationField3D::_cover_row, &job, job.h, -1, true, "VegetationField3D cover"));
	// Crowns spread past the trunks they grow on (to the edge of a lake, past a stand's last
	// suitable cell): blur them by the crown radius, which keeps their mean.
	float crown_r = 0.0f;
	for (uint32_t s = 0; s < p_setup.species.size(); s++) {
		if (!job.crown_k[s].is_empty()) {
			const Vector2 sr = p_setup.species[s].sp->get_scale_range();
			crown_r = MAX(crown_r, p_setup.species[s].sp->get_canopy_radius() * (sr.x + sr.y) * 0.5f);
		}
	}
	const int rad = int(Math::round(crown_r / cover_cell));
	if (rad > 0) {
		LocalVector<float> tmp;
		tmp.resize(job.crowns.size());
		for (int pass = 0; pass < 2; pass++) {
			const LocalVector<float> &src = pass == 0 ? job.crowns : tmp;
			LocalVector<float> &dst = pass == 0 ? tmp : job.crowns;
			for (int y = 0; y < job.h; y++) {
				for (int x = 0; x < job.w; x++) {
					float acc[5] = {};
					int n = 0;
					for (int d = -rad; d <= rad; d++) {
						const int xx = pass == 0 ? x + d : x;
						const int yy = pass == 0 ? y : y + d;
						if (xx < 0 || yy < 0 || xx >= job.w || yy >= job.h) {
							continue;
						}
						const float *c = src.ptr() + (int64_t(yy) * job.w + xx) * 5;
						for (int k = 0; k < 5; k++) {
							acc[k] += c[k];
						}
						n++;
					}
					float *o = dst.ptr() + (int64_t(y) * job.w + x) * 5;
					for (int k = 0; k < 5; k++) {
						o[k] = acc[k] / n;
					}
				}
			}
		}
	}
	job.pass = 1;
	wtp->wait_for_group_task_completion(wtp->add_native_group_task(&VegetationField3D::_cover_row, &job, job.h, -1, true, "VegetationField3D cover"));
	// Patches a few metres across still speckle the 4 m texels; the far field only needs their
	// mean, and the terrain samples the map's top level: a 3 x 3 box over premultiplied cover.
	{
		auto lin = [](uint8_t p_v) { return Color(p_v / 255.0f, 0, 0).srgb_to_linear().r; };
		LocalVector<float> pre;
		pre.resize(job.w * job.h * 4);
		for (int i = 0; i < job.w * job.h; i++) {
			const uint8_t *o = job.out + int64_t(i) * 4;
			const float a = o[3] / 255.0f;
			pre[i * 4 + 0] = lin(o[0]) * a;
			pre[i * 4 + 1] = lin(o[1]) * a;
			pre[i * 4 + 2] = lin(o[2]) * a;
			pre[i * 4 + 3] = a;
		}
		for (int y = 0; y < job.h; y++) {
			for (int x = 0; x < job.w; x++) {
				float acc[4] = {};
				int n = 0;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx;
						const int yy = y + dy;
						if (xx < 0 || yy < 0 || xx >= job.w || yy >= job.h) {
							continue;
						}
						for (int k = 0; k < 4; k++) {
							acc[k] += pre[(yy * job.w + xx) * 4 + k];
						}
						n++;
					}
				}
				const float a = acc[3] / n;
				uint8_t *o = job.out + (int64_t(y) * job.w + x) * 4;
				for (int k = 0; k < 3; k++) {
					o[k] = cover_srgb8(a > 0.0f ? acc[k] / n / a : 0.0f);
				}
				o[3] = uint8_t(CLAMP(a, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
	}
	cover_texture = ImageTexture::create_from_image(Image::create_from_data(job.w, job.h, false, Image::FORMAT_RGBA8, data));
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("terrain_cover_map", cover_texture->get_rid());
	rs->global_shader_parameter_set("terrain_cover_rect", Vector4(job.rect.position.x, job.rect.position.y, job.rect.size.x, job.rect.size.y));
	print_verbose(vformat("VegetationField3D: cover map %dx%d in %.1f ms.", job.w, job.h, (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0));
}

// A streaming field builds its cover map when it starts streaming; changed after that, the map is
// built again at once (no populate(), which would place the whole world and stop the streaming).
void VegetationField3D::set_cover_map_enabled(bool p_enable) {
	if (cover_map_enabled == p_enable) {
		return;
	}
	cover_map_enabled = p_enable;
	if (stream_setup_valid) {
		_build_cover(stream_setup);
	}
}

void VegetationField3D::set_cover_grass(const NodePath &p_path) {
	cover_grass = p_path;
	if (stream_setup_valid) {
		_build_cover(stream_setup);
	}
}

void VegetationField3D::set_cover_grass_layers(const TypedArray<NodePath> &p_paths) {
	cover_grass_layers = p_paths;
	if (stream_setup_valid) {
		_build_cover(stream_setup);
	}
}

void VegetationField3D::_on_grass_far_color() {
	if (stream_setup_valid) {
		_build_cover(stream_setup);
		return;
	}
	PlaceSetup setup;
	if (_make_setup(setup)) {
		_build_cover(setup);
	}
}

Error VegetationField3D::populate() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_clear_generated();
	impostors_traced = is_inside_tree() && _path_traced(); // Cells start with the right mesh.
	PlaceSetup setup;
	if (!_make_setup(setup)) {
		return ERR_UNCONFIGURED;
	}
	_copy_context(setup);
	_build_cover(setup);
	const Rect2 rect = setup.rect;
	const Rect2i tiles(Vector2i(int(Math::floor(rect.position.x / tile_size)), int(Math::floor(rect.position.y / tile_size))),
			Vector2i(int(Math::ceil((rect.position.x + rect.size.x) / tile_size)) - int(Math::floor(rect.position.x / tile_size)),
					int(Math::ceil((rect.position.y + rect.size.y) / tile_size)) - int(Math::floor(rect.position.y / tile_size))));
	// A rock_field: its stones over the whole map (the field places it all at once).
	_rocks_for(setup, rect, setup.obstacles);
	LocalVector<TileOut> outs;
	_place(setup, tiles, false, outs);
	const int species_count = setup.species.size();
	placements.resize(species_count);
	LocalVector<const LocalVector<Crown> *> crown_lists;
	LocalVector<const LocalVector<FarAdd> *> far_lists;
	for (int s = 0; s < species_count; s++) {
		for (const TileOut &t : outs) {
			for (const Vector3 &p : t.species[s].positions) {
				placements[s].push_back(p);
			}
			_commit_species(setup, t, s, generated);
		}
	}
	LocalVector<const LocalVector<Deadwood> *> deadwood_lists;
	for (const TileOut &t : outs) {
		plant_count += t.count;
		for (const Vector3 &v : t.shelter) {
			shelter_static.push_back(v);
		}
		shelter_dirty = true;
		crown_lists.push_back(&t.crowns);
		far_lists.push_back(&t.far);
		deadwood_lists.push_back(&t.deadwood);
	}
	_publish_deadwood(deadwood_lists);
	Maps maps;
	maps.maps_rect = rect;
	maps.far_rect = rect;
	_maps_wants(maps);
	_compute_maps(maps, crown_lists, far_lists);
	_apply_maps(maps);
	set_process_internal(!impostor_cells.is_empty() || !lazy_near.is_empty() || has_near_swap || _far_view_active());
	_report_stand_disturbances(false);
	// The field's own reports are not a change to follow (else it would populate again for ever).
	if (const WorldStand *own = WorldContracts::stand()) {
		stand_seen = own->get_stand_generation();
	}
	populate_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("populated"));
	return OK;
}

// Canopy shade as the share of sky light the crowns stop. Each crown passes 1 - opacity of the
// light under its core (Beer-Lambert through the foliage), thinning to nothing a little past its
// rim, where diffuse sky light still reaches under the edge; overlapping crowns multiply their
// transmission. Plants then take it as a shade rule, the way field botany ranks them by light
// (Ellenberg's L values: bilberry, ferns and mosses in shade, meadow grasses in the open).
// The radius of a stem's foot at the soil line: the farthest point from the axis where the
// surface crosses 2 cm above the ground (the flare and root ridges reach out there; TreeGenerator, trees.md
// 5d), capped at p_cap (the species' footprint) so a shrub's low branches do not count. Measured
// once per mesh and kept on it.
// The crown base height (fire behaviour's CBH, the lowest foliage that carries a fire up into the crown): the 5th
// percentile height of the "leaves" or "needles" surfaces' vertices, so a stray low twig does not set it. A mesh
// without such surfaces: a third of its height.
float VegetationField3D::_crown_base(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return 0.0f;
	}
	static const StringName key = SNAME("_vegetation_crown_base");
	if (p_mesh->has_meta(key)) {
		return p_mesh->get_meta(key);
	}
	LocalVector<float> ys;
	const ArrayMesh *am = Object::cast_to<ArrayMesh>(p_mesh.ptr());
	for (int si = 0; am && si < am->get_surface_count(); si++) {
		const String name = am->surface_get_name(si);
		if (name != "leaves" && name != "needles") {
			continue;
		}
		const Array arrays = am->surface_get_arrays(si);
		if (arrays.size() <= Mesh::ARRAY_VERTEX) {
			continue;
		}
		const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
		const int step = MAX(1, v.size() / 4096);
		for (int i = 0; i < v.size(); i += step) {
			ys.push_back(v[i].y);
		}
	}
	float base;
	if (ys.is_empty()) {
		base = p_mesh->get_aabb().get_end().y / 3.0f;
	} else {
		ys.sort();
		base = MAX(ys[ys.size() / 20], 0.0f);
	}
	p_mesh->set_meta(key, base);
	return base;
}

PackedFloat32Array VegetationField3D::get_crowns_in(const Rect2 &p_rect) const {
	PackedFloat32Array res;
	RWLockRead guard(crown_lock);
	for (const Crown &c : crowns) {
		if (c.xz.x + c.radius < p_rect.position.x || c.xz.y + c.radius < p_rect.position.y || c.xz.x - c.radius > p_rect.get_end().x || c.xz.y - c.radius > p_rect.get_end().y) {
			continue;
		}
		const float rec[7] = { float(c.xz.x), float(c.xz.y), float(c.radius), float(c.top), float(c.base), float(c.conifer), float(c.opacity) };
		for (float f : rec) {
			res.push_back(f);
		}
	}
	return res;
}

float VegetationField3D::_foot_radius(const Ref<Mesh> &p_mesh, float p_cap) {
	if (p_mesh.is_null()) {
		return 0.0f;
	}
	static const StringName key = SNAME("_vegetation_foot_radius");
	if (p_mesh->has_meta(key)) {
		return MIN(float(p_mesh->get_meta(key)), p_cap);
	}
	// Where the surface crosses the soil line (2 cm up): every triangle edge that spans it.
	constexpr float soil = 0.02f;
	float r = 0.0f;
	for (int si = 0; si < p_mesh->get_surface_count(); si++) {
		const Array arrays = p_mesh->surface_get_arrays(si);
		if (arrays.size() <= Mesh::ARRAY_INDEX) {
			continue;
		}
		const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
		const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
		const int n = idx.is_empty() ? v.size() : idx.size();
		auto vert = [&](int i) { return v[idx.is_empty() ? i : idx[i]]; };
		for (int t = 0; t + 2 < n; t += 3) {
			for (int e = 0; e < 3; e++) {
				const Vector3 a = vert(t + e), b = vert(t + (e + 1) % 3);
				if ((a.y - soil) * (b.y - soil) > 0.0f || a.y == b.y) {
					continue;
				}
				const Vector3 c = a.lerp(b, (soil - a.y) / (b.y - a.y));
				r = MAX(r, Vector2(c.x, c.z).length());
			}
		}
	}
	p_mesh->set_meta(key, r);
	return MIN(r, p_cap);
}

bool VegetationField3D::_in_stem(const Vector2 &p_xz, float p_margin) const {
	RWLockRead guard(crown_lock);
	return stem_in(crown_grid, crowns, p_xz, p_margin, CROWN_CELL);
}

float VegetationField3D::_shade_at(const Vector2 &p_xz, float *r_conifer) const {
	return crown_shade(crown_grid, crowns, max_crown_radius * 1.4f, CROWN_CELL, p_xz, r_conifer);
}

// How much of the grass the field's tall undergrowth shades out at a point (docs/ideas/
// forest-floor.md, "Grass under bracken"): bracken and bramble stands cast a deep shade and bury
// the ground in their litter, so the sward under them thins out (Marrs and Watt 2006 for
// bracken). Not from the placed plants, which stream round the camera and change as it moves, but
// from where the species grow: each one with a sward_shade weighs it by its density there (its
// rules, patches and the canopy's shade, as placement sees them), so a grass tile reads the same
// value whenever it is built. Main thread (it looks up the data map and canopy field nodes).
// The plants' frontal area index for sand blown by the wind (docs/ideas/vegetation.md, "Dune grass"):
// each plant's frontal area spread bilinearly over the 1 m grid's nodes, per square metre: Raupach's
// (1993) lambda, which GroundSurface3D turns into the share of the wind's stress the sand keeps.
void VegetationField3D::_build_shelter() const {
	shelter_grid.clear();
	const float inv = 1.0f / SHELTER_CELL;
	const float per_m2 = inv * inv;
	auto splat = [&](const Vector3 &p_v) {
		const float fx = p_v.x * inv;
		const float fz = p_v.y * inv;
		const int x0 = int(Math::floor(fx));
		const int z0 = int(Math::floor(fz));
		const float tx = fx - x0;
		const float tz = fz - z0;
		const float a = p_v.z * per_m2;
		shelter_grid[Vector2i(x0, z0)] += a * (1.0f - tx) * (1.0f - tz);
		shelter_grid[Vector2i(x0 + 1, z0)] += a * tx * (1.0f - tz);
		shelter_grid[Vector2i(x0, z0 + 1)] += a * (1.0f - tx) * tz;
		shelter_grid[Vector2i(x0 + 1, z0 + 1)] += a * tx * tz;
	};
	for (const Vector3 &v : shelter_static) {
		splat(v);
	}
	for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		for (const Vector3 &v : kv.value->out.shelter) {
			splat(v);
		}
	}
	shelter_dirty = false;
}

float VegetationField3D::sample_lateral_cover(const Vector3 &p_position) const {
	if (shelter_dirty) {
		_build_shelter();
	}
	if (shelter_grid.is_empty()) {
		return 0.0f;
	}
	const float fx = p_position.x / SHELTER_CELL;
	const float fz = p_position.z / SHELTER_CELL;
	const int x0 = int(Math::floor(fx));
	const int z0 = int(Math::floor(fz));
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int p_x, int p_z) {
		const float *v = shelter_grid.getptr(Vector2i(p_x, p_z));
		return v ? *v : 0.0f;
	};
	const float l = Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
	return MIN(l, 0.5f); // Past 0.5 the drag partition no longer holds; the sand is still.
}

void VegetationField3D::set_obstacles(const PackedVector4Array &p_obstacles) {
	obstacles = p_obstacles;
	obstacle_grid.build(obstacles);
	RWLockWrite guard(rock_lock);
	rock_grid.build(obstacles);
	PackedVector4Array fetched;
	rock_cache.gather_all(fetched);
	rock_grid.add(fetched);
}

// The rock field's stones are fetched again from scratch (changed after streaming started: the tiles placed from
// now on read them).
void VegetationField3D::set_rock_field(const NodePath &p_path) {
	rock_field = p_path;
	rock_cache.clear();
	RWLockWrite guard(rock_lock);
	rock_grid.build(obstacles);
}

void VegetationField3D::set_rock_obstacle_min(float p_size) {
	rock_obstacle_min = MAX(p_size, 0.0f);
	set_rock_field(rock_field);
}

void VegetationField3D::set_rock_footprint_max(float p_radius) {
	rock_footprint_max = MAX(p_radius, 0.0f);
}

Object *VegetationField3D::_get_rock_field() const {
	if (rock_field.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	Node *n = get_node_or_null(rock_field);
	return n && n->has_method(SNAME("get_obstacles")) ? n : nullptr;
}

bool VegetationField3D::_rocks_for(const PlaceSetup &p_setup, const Rect2 &p_core, VegetationObstacles &r_grid) {
	Object *rf = _get_rock_field();
	if (!rf) {
		return false;
	}
	// Every stone whose footprint a placement in p_core can reach: its centre within the reach plus the widest footprint.
	const Rect2 want = p_core.grow(p_setup.rock_reach + rock_footprint_max);
	PackedVector4Array fresh;
	if (rock_cache.fetch(rf, want, rock_obstacle_min, &fresh)) {
		if (rock_cache.max_radius > rock_footprint_max) {
			WARN_PRINT_ONCE(vformat("VegetationField3D: a stone %.1f m wide at its foot, wider than rock_footprint_max (%.1f m): raise it.", rock_cache.max_radius, rock_footprint_max));
		}
		RWLockWrite guard(rock_lock);
		if (rock_grid.is_empty()) {
			rock_grid.build(obstacles);
		}
		rock_grid.add(fresh);
	}
	PackedVector4Array near;
	for (int i = 0; i < obstacles.size(); i++) {
		const Vector4 o = obstacles[i];
		if (want.has_point(Vector2(o.x, o.z))) {
			near.push_back(o);
		}
	}
	rock_cache.gather(want, near);
	r_grid.build(near);
	return true;
}

void VegetationField3D::set_paths(const Array &p_paths) {
	paths = p_paths;
	path_grid.build(VegetationPaths::merged(paths, world_trails));
}

// Game trails (WorldStand.set_trails, from make_game_trails): worn like the field's own paths.
void VegetationField3D::_read_world_trails() {
	const WorldStand *stand = WorldContracts::stand();
	if (!stand) {
		return;
	}
	trails_seen = stand->get_stand_generation();
	const Array now = stand->get_trails();
	if (now == world_trails) {
		return;
	}
	const Rect2 old_r = VegetationPaths::bounds(world_trails, 4.0f);
	const Rect2 new_r = VegetationPaths::bounds(now, 4.0f);
	world_trails = now.duplicate(true);
	path_grid.build(VegetationPaths::merged(paths, world_trails));
	const Rect2 area = old_r.has_area() ? (new_r.has_area() ? old_r.merge(new_r) : old_r) : new_r;
	if (area.has_area() && (streaming || plant_count > 0)) {
		_on_holes_changed(area); // Placed again where the trails came or went.
	}
}

void VegetationField3D::_follow_trails(double p_delta) {
	trails_check -= p_delta;
	if (trails_check > 0.0) {
		return;
	}
	trails_check = 0.5;
	const WorldStand *stand = WorldContracts::stand();
	if (stand && stand->get_stand_generation() != trails_seen) {
		_read_world_trails();
	}
}

void VegetationField3D::set_stone_tops(const PackedVector4Array &p_tops) {
	stone_tops = p_tops;
	_build_stone_tops();
}

void VegetationField3D::set_stone_top_normals(const PackedVector3Array &p_normals) {
	stone_top_normals = p_normals;
	_build_stone_tops();
}

void VegetationField3D::_build_stone_tops() {
	stone_top_grid.build(stone_tops);
	// The grid drops empty caps: keep the normals in step with the caps it kept.
	stone_top_grid_normals.clear();
	if (stone_top_normals.size() == stone_tops.size()) {
		for (int i = 0; i < stone_tops.size(); i++) {
			if (stone_tops[i].w > 0.0f) {
				const Vector3 n = stone_top_normals[i];
				stone_top_grid_normals.push_back(n.length_squared() > 1e-8f ? n.normalized() : Vector3());
			}
		}
	}
}

float VegetationField3D::get_sward_shade_at(const Vector3 &p_global_position) const {
	return sward_at(p_global_position, nullptr);
}

Color VegetationField3D::get_sward_litter_at(const Vector3 &p_global_position) const {
	Color litter;
	sward_at(p_global_position, &litter);
	return litter;
}

// The undergrowth's shade on the sward at a point and, if asked, its litter on the ground there:
// the species' litter colours weighed by how much each covers, the cover adding as transmissions.
float VegetationField3D::sward_at(const Vector3 &p_global_position, Color *r_litter) const {
	if (r_litter) {
		*r_litter = Color(0, 0, 0, 0);
	}
	if (biome.is_null() || !is_inside_tree()) {
		return 0.0f;
	}
	const TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	if (!map || !map->is_baked()) {
		return 0.0f;
	}
	// An uprooted tree's root pit is bare soil: no sward (GrassField3D.undergrowth_field).
	{
		const Vector2 xz(p_global_position.x, p_global_position.z);
		const VegetationField3D *other = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
		if (_pit_at(xz) || (other && other != this && other->_pit_at(xz))) {
			return 1.0f;
		}
	}
	const TypedArray<PlantSpecies> species = biome->get_species();
	float transmit = 1.0f;
	float bare = 1.0f;
	Color litter_sum(0, 0, 0, 0);
	float shade = -1.0f;
	for (int i = 0; i < species.size(); i++) {
		const Ref<PlantSpecies> sp = species[i];
		if (sp.is_null() || sp->get_density() <= 0.0f) {
			continue;
		}
		const float lit = r_litter ? sp->get_litter_color().a : 0.0f;
		if (sp->get_sward_shade() <= 0.0f && lit <= 0.0f) {
			continue;
		}
		float v = sp->get_density() * sp->get_suitability(map, p_global_position);
		if (v > 0.0f && _context_only(sp.ptr())) {
			// Edge brambles, clearing raspberry, burn regrowth: only where their context holds (the last setup's grids).
			v *= _context_weight(sp.ptr(), Vector2(p_global_position.x, p_global_position.z));
		}
		if (v > 0.0f && sp->uses_shade()) {
			if (shade < 0.0f) {
				const Vector2 xz(p_global_position.x, p_global_position.z);
				float own;
				{
					RWLockRead guard(crown_lock);
					own = _shade_at(xz);
				}
				const VegetationField3D *other = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
				const float os = (other && other != this) ? other->get_shade_at(p_global_position) : 0.0f;
				shade = 1.0f - (1.0f - own) * (1.0f - os);
			}
			v *= sp->rule_weight(PlantSpecies::RULE_SHADE, shade);
		}
		// The sward goes only where the fronds close over it: at a stand's thin margin, where the
		// plants stand apart, grass keeps growing between them and no litter mat forms, so the
		// shade and litter follow the stand's cover, not its density linearly.
		v = Math::smoothstep(0.25f, 0.85f, CLAMP(v, 0.0f, 1.0f));
		transmit *= 1.0f - sp->get_sward_shade() * v;
		if (lit > 0.0f && v > 0.0f) {
			const Color lc = sp->get_litter_color();
			const float c = lit * v;
			bare *= 1.0f - c;
			litter_sum += Color(lc.r * c, lc.g * c, lc.b * c, c);
		}
	}
	if (r_litter && litter_sum.a > 0.0f) {
		const float inv = 1.0f / litter_sum.a;
		*r_litter = Color(litter_sum.r * inv, litter_sum.g * inv, litter_sum.b * inv, 1.0f - bare);
	}
	return 1.0f - transmit;
}

float VegetationField3D::get_stand_shade_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock);
	if (stand_shade.is_empty()) {
		return _shade_at(Vector2(p_global_position.x, p_global_position.z));
	}
	const float fx = (p_global_position.x - stand_rect.position.x) / stand_rect.size.x * stand_size.x - 0.5f;
	const float fy = (p_global_position.z - stand_rect.position.y) / stand_rect.size.y * stand_size.y - 0.5f;
	if (fx < -0.5f || fy < -0.5f || fx > stand_size.x - 0.5f || fy > stand_size.y - 0.5f) {
		return _shade_at(Vector2(p_global_position.x, p_global_position.z));
	}
	const int x0 = CLAMP(int(Math::floor(fx)), 0, stand_size.x - 1);
	const int y0 = CLAMP(int(Math::floor(fy)), 0, stand_size.y - 1);
	const int x1 = MIN(x0 + 1, stand_size.x - 1);
	const int y1 = MIN(y0 + 1, stand_size.y - 1);
	const float tx = CLAMP(fx - x0, 0.0f, 1.0f);
	const float ty = CLAMP(fy - y0, 0.0f, 1.0f);
	const float a = Math::lerp(stand_shade[y0 * stand_size.x + x0], stand_shade[y0 * stand_size.x + x1], tx);
	const float b = Math::lerp(stand_shade[y1 * stand_size.x + x0], stand_shade[y1 * stand_size.x + x1], tx);
	return Math::lerp(a, b, ty);
}

void VegetationField3D::set_forest_floor_enabled(bool p_enabled) {
	forest_floor_enabled = p_enabled;
	if (!p_enabled && floor_texture.is_valid() && is_inside_tree()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("forest_floor_rect", Vector4());
		floor_texture.unref();
	}
}

String VegetationField3D::get_floor_shader_code() {
	return R"(
// VegetationField3D forest floor, for terrain shaders (docs/ideas/forest-floor.md). Under a closed
// canopy only 1 to 5% of daylight reaches the ground and the floor is a continuous litter layer
// 2 to 10 cm deep (Gilliam 2007): fallen leaves under broadleaves, needles over feather moss
// under conifers. Litter is the ground's own material here (the litter-material recipe of
// forest games: a floor material matching the canopy, 3D litter and plants on top near the
// camera), so no soil shows between plants. Leaves are a stochastic sprite layer: jittered
// cells, one leaf per cell at a random turn and size, overlapping their neighbours, two layers;
// needles the same with thin strokes. Each layer fades to its mean colour once a pixel covers
// its detail, so it neither aliases nor costs anything far away.
global uniform sampler2D forest_floor_map : hint_default_black, filter_linear;
global uniform vec4 forest_floor_rect; // xy: corner, zw: size; z 0 = no forest.

vec2 ff_hash2(vec2 p) {
	p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
	return fract(sin(p) * 43758.5453);
}

float ff_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(ff_hash2(i).x, ff_hash2(i + vec2(1.0, 0.0)).x, f.x), mix(ff_hash2(i + vec2(0.0, 1.0)).x, ff_hash2(i + vec2(1.0, 1.0)).x, f.x), f.y);
}

// Closure (x) and conifer share (y) at a point; zero outside the map.
vec2 forest_floor_at(vec3 world_pos) {
	if (forest_floor_rect.z <= 0.0) {
		return vec2(0.0);
	}
	vec2 uv = (world_pos.xz - forest_floor_rect.xy) / forest_floor_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec2(0.0);
	}
	return textureLod(forest_floor_map, uv, 0.0).rg;
}

// Fallen leaves of one layer, cell size s metres. rgb: colour, a: cover.
vec4 ff_leaf_layer(vec2 xz, float s, float seed) {
	vec2 p = xz / s + seed;
	vec2 cell = floor(p);
	float best = -1.0;
	vec4 col = vec4(0.0);
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec2 c = cell + vec2(float(i), float(j));
			vec2 h = ff_hash2(c);
			vec2 h2 = ff_hash2(c + 17.31);
			vec2 centre = c + 0.15 + 0.7 * h;
			float ang = h.y * 6.2832;
			vec2 dir = vec2(cos(ang), sin(ang));
			vec2 q = p - centre;
			float len = 0.55 + 0.4 * h2.x; // Half length, in cells.
			float u = dot(q, dir) / len;
			float v = dot(q, vec2(-dir.y, dir.x)) / (len * 0.6);
			// Ovate, pointed at the tip, curled leaves read narrower.
			float wdt = (1.0 - u * u) * (1.0 - 0.35 * u) * (0.7 + 0.3 * h2.y);
			float inside = wdt - v * v;
			if (inside > 0.0 && h2.y > best) {
				best = h2.y;
				// Beech and oak litter: orange-brown fresh, dark brown decaying, pale and grey-brown bleached.
				float k = fract(h.x * 7.13 + h2.x * 3.7);
				vec3 c0 = k < 0.55 ? vec3(0.24, 0.115, 0.045) : (k < 0.8 ? vec3(0.15, 0.08, 0.037) : (k < 0.92 ? vec3(0.3, 0.19, 0.1) : vec3(0.15, 0.12, 0.085)));
				c0 *= 0.85 + 0.3 * ff_noise(q * 3.0 + c);
				// Midrib and veins catch the light; the rim curls up and shades the leaf below.
				float vein = 1.0 + 0.25 * (1.0 - smoothstep(0.0, 0.06, abs(v))) * step(-0.9, u);
				float rim = 0.7 + 0.3 * smoothstep(0.0, 0.25, inside);
				col = vec4(c0 * vein * rim, 1.0);
			}
		}
	}
	return col;
}

// Fallen needles, cell size s: two strokes per cell over the duff.
vec4 ff_needle_layer(vec2 xz, float s, float seed) {
	vec2 p = xz / s + seed;
	vec2 cell = floor(p);
	vec4 col = vec4(0.0);
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec2 c = cell + vec2(float(i), float(j));
			for (int n = 0; n < 2; n++) {
				vec2 h = ff_hash2(c + float(n) * 31.7);
				vec2 h2 = ff_hash2(c + float(n) * 31.7 + 5.1);
				vec2 q = p - (c + h);
				float ang = h2.x * 6.2832;
				vec2 dir = vec2(cos(ang), sin(ang));
				float u = dot(q, dir);
				float v = dot(q, vec2(-dir.y, dir.x));
				float half_len = 0.8 + 0.6 * h2.y;
				if (abs(u) < half_len && abs(v) < 0.08) {
					float k = fract(h.x * 5.3 + h2.y * 2.1);
					vec3 c0 = k < 0.5 ? vec3(0.27, 0.12, 0.04) : (k < 0.8 ? vec3(0.16, 0.09, 0.045) : vec3(0.34, 0.25, 0.14));
					col = vec4(c0 * (0.8 + 0.2 * (1.0 - abs(v) / 0.08)), 1.0);
				}
			}
		}
	}
	return col;
}

// A litter layer, faded to its mean where a pixel covers its detail (the layer's own mip).
vec3 ff_layer_blend(vec3 under, vec4 layer, vec3 mean, float cover, float fine) {
	return mix(mix(under, layer.rgb, layer.a), mix(under, mean, cover), fine);
}

// The floor colour at a point under the canopy: conifer 0 broadleaf, 1 needle.
vec3 forest_litter(vec3 world_pos, float conifer, float footprint) {
	vec2 xz = world_pos.xz;
	// Humus showing through, dark and fibrous.
	vec3 c = vec3(0.05, 0.034, 0.02) * (0.8 + 0.4 * ff_noise(xz * 2.3));
	// Mixed stands are a patchwork of the two litters, not their average.
	float needle = smoothstep(-0.12, 0.12, conifer - (0.25 + 0.5 * ff_noise(xz * 0.35 + 3.0)));
	if (needle < 0.999) {
		vec3 leaves = c;
		vec3 mean = vec3(0.2, 0.1, 0.045);
		float f0 = smoothstep(0.03, 0.1, footprint);
		float f1 = smoothstep(0.022, 0.075, footprint);
		leaves = ff_layer_blend(leaves, f0 < 1.0 ? ff_leaf_layer(xz, 0.06, 0.0) : vec4(0.0), mean, 0.85, f0);
		leaves = ff_layer_blend(leaves, f1 < 1.0 ? ff_leaf_layer(xz, 0.045, 41.0) : vec4(0.0), mean, 0.6, f1);
		// Patches of darker, wetter, older litter.
		leaves *= 0.75 + 0.45 * ff_noise(xz * 0.8);
		c = mix(c, leaves, 1.0 - needle);
	}
	if (needle > 0.001) {
		// Older needles, half decayed, under the fresh ones.
		vec3 duff = vec3(0.11, 0.06, 0.028) * (0.8 + 0.4 * ff_noise(xz * 5.0 + 2.0));
		float f = smoothstep(0.01, 0.03, footprint);
		duff = ff_layer_blend(duff, f < 1.0 ? ff_needle_layer(xz, 0.025, 7.0) : vec4(0.0), vec3(0.17, 0.085, 0.035), 0.75, f);
		duff = ff_layer_blend(duff, f < 1.0 ? ff_needle_layer(xz, 0.019, 23.0) : vec4(0.0), vec3(0.17, 0.085, 0.035), 0.6, f);
		duff *= 0.8 + 0.35 * ff_noise(xz * 1.1 + 9.0);
		// Feather moss carpets under spruce and pine, in drifts.
		float moss = smoothstep(0.38, 0.62, ff_noise(xz * 0.45 + 17.0) * 0.7 + ff_noise(xz * 2.7) * 0.3);
		vec3 moss_c = vec3(0.13, 0.19, 0.035) * (0.7 + 0.5 * ff_noise(xz * 23.0)) * (0.85 + 0.3 * ff_noise(xz * 4.0 + 5.0));
		duff = mix(duff, moss_c, moss * 0.85);
		c = mix(c, duff, needle);
	}
	return c;
}

global uniform sampler2D vegetation_far_map : hint_default_black, filter_linear; // sqrt of premultiplied linear colour, cover.
global uniform vec4 vegetation_far_rect; // z 0: no plants published.
global uniform float vegetation_far_range;
global uniform float vegetation_far_shade;

// The share of plants the plant shader still draws at a distance from the camera, as a share of
// the range (PlantGenerator's thinning): 1 to 35% of the range, 0 at the range.
float vegetation_keep(float d, float range) {
	return clamp((range - d) / (range * 0.65), 0.0, 1.0);
}

// Ground albedo with the placed plants' colour over it where they have been thinned away. Call it
// after grass_ground() with CAMERA_POSITION_WORLD.
vec3 vegetation_far(vec3 world_pos, vec3 albedo, vec3 camera_pos) {
	if (vegetation_far_rect.z <= 0.0 || vegetation_far_range <= 0.0) {
		return albedo;
	}
	vec2 uv = (world_pos.xz - vegetation_far_rect.xy) / vegetation_far_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return albedo;
	}
	float gone = 1.0 - vegetation_keep(length(world_pos - camera_pos), vegetation_far_range);
	if (gone <= 0.0) {
		return albedo;
	}
	vec4 f = textureLod(vegetation_far_map, uv, 0.0);
	// vegetation_far_shade: the darkening plants give the ground they stand on within their range
	// (their shadows and shaded sides), measured on screen (VegetationField3D.far_match_view).
	float shade = mix(1.0, vegetation_far_shade, f.a * gone);
	return (albedo * (1.0 - f.a * gone) + f.rgb * f.rgb * gone) * shade;
}

// Ground albedo with the forest floor over it, where the canopy closes.
vec3 forest_floor(vec3 world_pos, vec3 albedo) {
	vec2 f = forest_floor_at(world_pos);
	// Litter blows a little past the crowns and thins out where light reaches the ground.
	// Ragged, patchy edge: two octaves of noise, so the mat breaks into drifts as it thins.
	float n = ff_noise(world_pos.xz * 0.6) * 0.65 + ff_noise(world_pos.xz * 2.3 + 5.1) * 0.35;
	float cover = smoothstep(0.15, 0.6, f.x + 0.3 * (n - 0.5));
	if (cover <= 0.0) {
		return albedo;
	}
	vec2 fw = fwidth(world_pos.xz);
	float footprint = max(fw.x, fw.y);
	return mix(albedo, forest_litter(world_pos, f.y, footprint), cover);
}
)";
}

Array VegetationField3D::get_crowns_near(const Vector3 &p_position, float p_radius) const {
	Array out;
	RWLockRead guard(crown_lock);
	const Vector2 c(p_position.x, p_position.z);
	const float reach = p_radius + max_crown_radius;
	const int x0 = int(Math::floor((c.x - reach) / CROWN_CELL)), x1 = int(Math::floor((c.x + reach) / CROWN_CELL));
	const int z0 = int(Math::floor((c.y - reach) / CROWN_CELL)), z1 = int(Math::floor((c.y + reach) / CROWN_CELL));
	const TypedArray<PlantSpecies> list = biome.is_valid() ? biome->get_species() : TypedArray<PlantSpecies>();
	for (int gz = z0; gz <= z1; gz++) {
		for (int gx = x0; gx <= x1; gx++) {
			const LocalVector<int> *cell = crown_grid.getptr(Vector2i(gx, gz));
			if (!cell) {
				continue;
			}
			for (int idx : *cell) {
				const Crown &cr = crowns[idx];
				if (cr.xz.distance_to(c) > p_radius + cr.radius) {
					continue;
				}
				Dictionary d;
				d["position"] = Vector3(cr.xz.x, cr.y, cr.xz.y);
				float height = cr.top;
				float base = cr.base > 0.0f ? cr.base : cr.top * 0.35f;
				Variant preset = -1;
				if (cr.species >= 0 && cr.species < list.size()) {
					const Ref<PlantSpecies> sp = list[cr.species];
					if (sp.is_valid()) {
						if (sp->has_meta("preset")) {
							preset = sp->get_meta("preset");
						}
						const TypedArray<Mesh> meshes = sp->get_meshes();
						const Ref<Mesh> m = meshes.is_empty() ? Ref<Mesh>() : Ref<Mesh>(meshes[0]);
						if (m.is_valid() && m->has_meta("crown")) {
							const Vector3 cm = m->get_meta("crown");
							height = cm.x * cr.scale;
							base = cm.y * cr.scale;
						}
					}
				}
				d["preset"] = preset;
				d["height"] = height;
				d["crown_base"] = base;
				d["crown_radius"] = cr.radius;
				d["conifer"] = cr.conifer > 0.5f;
				out.push_back(d);
			}
		}
	}
	return out;
}

float VegetationField3D::get_shade_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock); // Another field may read this one's crowns from its workers.
	return _shade_at(Vector2(p_global_position.x, p_global_position.z));
}

float VegetationField3D::get_light_at(const Vector3 &p_global_position) const {
	const TerrainDataMap3D *map = data_map.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	const float exposure = map && map->is_baked() ? VegetationLight::exposure(map, VegetationLight::sun_mean(map), p_global_position.x, p_global_position.z) : 1.0f;
	return (1.0f - get_shade_at(p_global_position)) * exposure;
}

float VegetationField3D::get_conifer_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock);
	float conifer = 0.0f;
	_shade_at(Vector2(p_global_position.x, p_global_position.z), &conifer);
	return conifer;
}

// The placed plants over the ground for WorldGround.albedo_at: the far map's colour (stored as the square root of the
// premultiplied linear colour) and cover (a), bilinear; -1 past the map.
Color VegetationField3D::contract_plant_albedo_at(const Vector3 &p_position) const {
	if (far_image.is_null() || far_image_rect.size.x <= 0.0f || far_image_rect.size.y <= 0.0f) {
		return Color(0, 0, 0, -1.0f);
	}
	const Vector2 uv = (Vector2(p_position.x, p_position.z) - far_image_rect.position) / far_image_rect.size;
	if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
		return Color(0, 0, 0, -1.0f);
	}
	const int w = far_image->get_width();
	const int h = far_image->get_height();
	const float fx = CLAMP(uv.x * w - 0.5f, 0.0f, float(w - 1));
	const float fz = CLAMP(uv.y * h - 0.5f, 0.0f, float(h - 1));
	const int x0 = int(fx);
	const int z0 = int(fz);
	const int x1 = MIN(x0 + 1, w - 1);
	const int z1 = MIN(z0 + 1, h - 1);
	const float tx = fx - x0;
	const float tz = fz - z0;
	// Premultiplied colour and cover interpolate; the square roots are undone per texel first.
	auto texel = [&](int p_x, int p_z) {
		const Color t = far_image->get_pixel(p_x, p_z);
		return Color(t.r * t.r, t.g * t.g, t.b * t.b, t.a);
	};
	const Color c = texel(x0, z0).lerp(texel(x1, z0), tx).lerp(texel(x0, z1).lerp(texel(x1, z1), tx), tz);
	if (c.a <= 1e-4f) {
		return Color(0, 0, 0, 0);
	}
	return Color(c.r / c.a, c.g / c.a, c.b / c.a, MIN(c.a, 1.0f));
}

// Leaf drifts (docs/ideas/vegetation.md, "Leaf drifts"). Wind moves fallen leaves along the ground until they lodge:
// in hollows and furrows, and against anything that breaks the flow, trunks, logs and stones, deepest in their lee and
// along their windward foot; ridges, knolls and steep slopes are swept to the soil (Orndorff and Lang 1981, Am. Midl.
// Nat.; Boerner and Kooser 1989, Am. Midl. Nat., litter up to several times deeper in hollows than on ridges; the lee
// of obstacles as for snowdrifts). The terrain term is the ground's depth under its ring at 2.5 and 6 m (a
// relative relief, so it reads the same at any map resolution); the obstacles' term decays over half a metre from
// their side, stronger downwind of them.
float VegetationField3D::_litter_drift(const TerrainDataMap3D *p_map, const VegetationObstacles &p_rocks, const HashMap<Vector2i, LocalVector<int>> &p_crown_grid, const LocalVector<Crown> &p_crowns, const HashMap<Vector2i, LocalVector<int>> &p_log_grid, const LocalVector<Deadwood> &p_logs, const VegetationField3D *p_other, const Vector2 &p_xz, const Vector2 &p_wind) const {
	float d = 1.0f;
	if (p_map) {
		auto hgt = [&](float p_dx, float p_dz) { return p_map->get_height_at(Vector3(p_xz.x + p_dx, 0.0f, p_xz.y + p_dz)); };
		const float h0 = hgt(0.0f, 0.0f);
		const float ring1 = 0.25f * (hgt(2.5f, 0.0f) + hgt(-2.5f, 0.0f) + hgt(0.0f, 2.5f) + hgt(0.0f, -2.5f));
		const float ring2 = 0.25f * (hgt(4.2f, 4.2f) + hgt(-4.2f, 4.2f) + hgt(4.2f, -4.2f) + hgt(-4.2f, -4.2f));
		const float hollow = 0.6f * (ring1 - h0) + 0.4f * (ring2 - h0); // m, positive in a hollow.
		d += 3.5f * CLAMP(hollow, -0.24f, 0.3f);
		// Steep slopes shed their leaves downhill.
		const float slope = p_map->get_slope_at(Vector3(p_xz.x, 0.0f, p_xz.y)); // Radians.
		d *= 1.0f - 0.6f * Math::smoothstep(0.45f, 0.8f, slope);
	}
	// The feet of obstacles: a side factor from the direction away from them (downwind: the lee eddy, deepest).
	float lee = 0.0f;
	auto foot = [&](float p_gap, const Vector2 &p_away, float p_scale) {
		if (p_gap > 1.6f) {
			return;
		}
		const float side = p_wind == Vector2() ? 0.75f : 0.55f + 0.45f * p_away.dot(p_wind);
		lee = MAX(lee, p_scale * side * Math::exp(-MAX(p_gap, 0.0f) / 0.5f));
	};
	if (!p_rocks.is_empty()) {
		Vector2 away;
		const float gap = p_rocks.clearance(p_xz, 1.6f, &away);
		foot(gap, away, 1.0f);
	}
	DeadwoodHit hit;
	bool found = deadwood_nearest(p_log_grid, p_logs, p_xz, 1.6f, DEADWOOD_CELL, hit, 0.05f);
	if (p_other) {
		found = p_other->_deadwood_at(p_xz, 1.6f, hit, 0.05f) || found;
	}
	if (found && hit.side >= 0.0f) {
		foot(hit.side, hit.across, MIN(1.0f, hit.radius / 0.15f));
	}
	if (!p_crowns.is_empty()) {
		const Vector2i gc(int(Math::floor(p_xz.x / CROWN_CELL)), int(Math::floor(p_xz.y / CROWN_CELL)));
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				const LocalVector<int> *bucket = p_crown_grid.getptr(gc + Vector2i(dx, dz));
				if (!bucket) {
					continue;
				}
				for (int idx : *bucket) {
					const Crown &cr = p_crowns[idx];
					if (cr.stem <= 0.0f) {
						continue;
					}
					const Vector2 to = p_xz - cr.xz;
					const float len = to.length();
					if (len < cr.stem + 1.6f && len > 1e-4f) {
						foot(len - cr.stem, to / len, MIN(1.0f, cr.stem / 0.25f));
					}
				}
			}
		}
	}
	d += 1.3f * lee;
	return CLAMP(d, 0.15f, 2.5f);
}

float VegetationField3D::get_litter_drift_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock);
	const Vector2 xz(p_global_position.x, p_global_position.z);
	const VegetationField3D *other = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
	if (other == this) {
		other = nullptr;
	}
	const TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	const Vector2 wind = litter_skew.length_squared() > 1e-8f ? litter_skew.normalized() : Vector2();
	if (!rock_field.is_empty()) {
		RWLockRead rocks_guard(rock_lock); // The rocks fetched so far, round the placed tiles.
		return _litter_drift(map, rock_grid, crown_grid, crowns, deadwood_grid, deadwood, other, xz, wind);
	}
	return _litter_drift(map, obstacle_grid, crown_grid, crowns, deadwood_grid, deadwood, other, xz, wind);
}

float VegetationField3D::get_litter_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock);
	return crown_litter(crown_grid, crowns, max_crown_radius + 3.0f * LITTER_SPREAD * (1.0f + litter_skew.length()), CROWN_CELL, Vector2(p_global_position.x, p_global_position.z), litter_skew);
}

PackedVector3Array VegetationField3D::get_placements(int p_species) const {
	PackedVector3Array res;
	if (streaming) {
		// The placed tiles around the centre, committed to the scene or not.
		for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
			if (p_species >= 0 && p_species < int(kv.value->out.species.size())) {
				for (const Vector3 &p : kv.value->out.species[p_species].positions) {
					res.push_back(p);
				}
			}
		}
		return res;
	}
	ERR_FAIL_INDEX_V(p_species, int(placements.size()), res);
	for (const Vector3 &p : placements[p_species]) {
		res.push_back(p);
	}
	return res;
}

// The trunks of the placed trees within p_radius of p_center, for close-up bark (tree_bark_detail): read back from
// the drawn cells, whose meshes carry TreeGenerator's tree_colliders (capsules a, b, radius, bone, mesh-local). Each
// tree once, however many cells or passes draw it. The trunk is the chain of capsules from the first, up to
// p_height metres above its base.
Array VegetationField3D::get_trunks_near(const Vector3 &p_center, float p_radius, float p_height) const {
	Array res;
	HashMap<const Mesh *, Vector2i> species_of; // mesh: species, variant.
	if (biome.is_valid()) {
		for (int s = 0; s < biome->get_species().size(); s++) {
			const Ref<PlantSpecies> sp = biome->get_species()[s];
			if (sp.is_null()) {
				continue;
			}
			const TypedArray<Mesh> ms = sp->get_meshes();
			for (int v = 0; v < ms.size(); v++) {
				const Ref<Mesh> m = ms[v];
				if (m.is_valid()) {
					species_of.insert(m.ptr(), Vector2i(s, v));
				}
			}
		}
	}
	HashSet<Vector3i> seen;
	const float r2 = p_radius * p_radius;
	for (int c = 0; c < get_child_count(true); c++) {
		const MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(get_child(c, true));
		if (!mmi || mmi->get_multimesh().is_null()) {
			continue;
		}
		const Ref<MultiMesh> mm = mmi->get_multimesh();
		const Ref<Mesh> mesh = mm->get_mesh();
		if (mesh.is_null() || !mesh->has_meta(SNAME("tree_colliders"))) {
			continue;
		}
		const PackedFloat32Array col = mesh->get_meta(SNAME("tree_colliders"));
		if (col.size() < 8) {
			continue;
		}
		const Vector2i *sv = species_of.getptr(mesh.ptr());
		int preset = -1;
		if (sv && biome.is_valid()) {
			const Ref<PlantSpecies> sp = biome->get_species()[sv->x];
			preset = sp.is_valid() ? int(sp->get_meta(SNAME("preset"), -1)) : -1;
		}
		const Transform3D gx = mmi->get_global_transform();
		const bool custom = mm->is_using_custom_data();
		const int count = mm->get_visible_instance_count() < 0 ? mm->get_instance_count() : mm->get_visible_instance_count();
		for (int i = 0; i < count; i++) {
			const Transform3D t = gx * mm->get_instance_transform(i);
			if (t.origin.distance_squared_to(p_center) > r2) {
				continue;
			}
			if (t.basis.get_scale().y < 1e-3f) {
				continue; // A tree taken out of the stand (felled, thrown, burnt down): its instance is collapsed.
			}
			const Vector3i key(int(Math::round(t.origin.x * 100.0f)), int(Math::round(t.origin.y * 100.0f)), int(Math::round(t.origin.z * 100.0f)));
			if (seen.has(key)) {
				continue;
			}
			seen.insert(key);
			const float scale = t.basis.get_scale().y;
			Array segments;
			const Vector3 a0(col[0], col[1], col[2]);
			Vector3 prev = a0;
			float r_top = col[6];
			float height = 0.0f;
			for (int k = 0; k + 7 < col.size(); k += 8) {
				const Vector3 a(col[k], col[k + 1], col[k + 2]);
				const Vector3 b(col[k + 3], col[k + 4], col[k + 5]);
				if (k > 0 && a.distance_to(prev) > 1e-3f) {
					break; // The trunk's chain ends where the next capsule starts elsewhere (a branch).
				}
				Array seg;
				seg.push_back(t.xform(a));
				seg.push_back(t.xform(b));
				seg.push_back(col[k + 6] * scale);
				segments.push_back(seg);
				height += a.distance_to(b) * scale;
				r_top = col[k + 6];
				prev = b;
				if ((b.y - a0.y) * scale >= p_height) {
					break;
				}
			}
			const Vector3 b0(col[3], col[4], col[5]);
			Dictionary d;
			d["base"] = t.xform(a0);
			d["up"] = t.basis.xform(b0 - a0).normalized();
			d["height"] = height;
			d["radius_base"] = col[6] * scale;
			d["radius_top"] = r_top * scale;
			d["segments"] = segments;
			d["preset"] = preset;
			d["species"] = sv ? sv->x : -1;
			d["variant"] = sv ? sv->y : -1;
			d["basis"] = t.basis;
			d["custom"] = custom ? mm->get_instance_custom_data(i) : Color();
			// A seed from where it stands: the same tree gives the same number whichever cell draws it.
			d["seed"] = int(hash_murmur3_one_32(uint32_t(key.x), hash_murmur3_one_32(uint32_t(key.z))) & 0x7fffffff);
			res.push_back(d);
		}
	}
	return res;
}

// Streaming (docs/ideas/vegetation-lod-streaming.md, "Built (2)"): tiles around a centre are placed
// on WorkerThreadPool, nearest first, each over its halo so it comes out exactly as in populate().
// The main thread adds a placed tile's species to the scene within stream_budget_msec a frame,
// only out to each species' visibility range, and frees tiles once they are past the radius.

void VegetationField3D::_stream_job(void *p_job) {
	StreamJob *job = static_cast<StreamJob *>(p_job);
	job->field->_place(job->field->stream_setup, Rect2i(job->tile, Vector2i(1, 1)), true, job->out, job->own_rocks ? &job->rocks : nullptr);
	job->done.set();
}

// Skyline trees (trees.md 9c; silhouettes kept in simplification as Luebke and Erikson 1997 and Sander et al. 2000
// keep them): a crown top is on the skyline from the eye when no ground beyond it, along the line of sight, rises
// above that line, and no canopy in front hides it (sampled at doubling distances to 2.5 km on the data map; past its edge the map holds its border).
bool VegetationField3D::_on_skyline(const TerrainDataMap3D *p_map, const Vector3 &p_eye, const Vector3 &p_top) {
	const Vector2 dh(p_top.x - p_eye.x, p_top.z - p_eye.z);
	const float dist = dh.length();
	if (dist < 1.0f) {
		return false;
	}
	const Vector2 dir = dh / dist;
	const float slope = (p_top.y - p_eye.y) / dist;
	// Behind first and from far to near: seen from above (the common case) the falling line meets the ground first
	// far out, so most trees leave after one sample; from low on level ground the front test leaves after one.
	for (float s = 2560.0f; s >= 10.0f; s *= 0.5f) {
		const Vector3 q(p_top.x + dir.x * s, 0.0f, p_top.z + dir.y * s);
		if (p_map->get_height_at(q) > p_top.y + slope * s) {
			return false;
		}
	}
	// In front, a canopy as tall as this tree on the ground between hides it (the nearer crowns are the skyline
	// then): from inside or below a forest on level ground no far tree is kept, so the tail still thins there.
	// Where the ground between is open this keeps fewer than it could, never more.
	const float tall = p_top.y - p_map->get_height_at(Vector3(p_top.x, 0.0f, p_top.z));
	for (float t : { 0.5f, 0.7f, 0.85f, 0.95f }) {
		const Vector3 q(p_eye.x + dh.x * t, 0.0f, p_eye.z + dh.y * t);
		if (p_map->get_height_at(q) + tall > p_eye.y + (p_top.y - p_eye.y) * t + 0.5f) {
			return false;
		}
	}
	return true;
}

Vector3 VegetationField3D::_stream_centre() const {
	if (!stream_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(stream_target));
		if (n) {
			return n->get_global_position();
		}
	}
	Viewport *vp = get_viewport();
	const Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : get_global_position();
}

void VegetationField3D::_stream_free_species(StreamTile *p_tile, int p_species) {
	for (MultiMeshInstance3D *mmi : p_tile->nodes[p_species]) {
		_lazy_near_drop(mmi);
		const int64_t idx = impostor_cells.find(mmi);
		if (idx >= 0) {
			impostor_cells.remove_at_unordered(idx);
			impostor_meshes.remove_at_unordered(idx); // Same swap: the lists stay paired.
		}
		if (mmi->get_parent()) {
			mmi->get_parent()->remove_child(mmi);
		}
		memdelete(mmi);
	}
	p_tile->nodes[p_species].clear();
	p_tile->committed[p_species] = false;
}

void VegetationField3D::_stream_clear() {
	_free_far_shell();
	stream_setup_valid = false; // The maps job's result is dropped.
	_stream_maps_finish();
	for (StreamJob *job : stream_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		memdelete(job);
	}
	stream_jobs.clear();
	for (KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		for (uint32_t s = 0; s < kv.value->nodes.size(); s++) {
			_stream_free_species(kv.value, s);
		}
		memdelete(kv.value);
	}
	stream_tiles.clear();
	stream_setup = PlaceSetup();
	stream_setup_valid = false;
	stream_maps_dirty = false;
	stream_maps_wait = 0.0;
	stream_maps_centre = Vector2(1e30f, 1e30f);
	stream_idle = false;
	plant_count = 0;
}

// The ranks a streamed tile's cells hold for a species with a far tail, a tile nearer than it is (so the camera can
// come a tile closer before they are built again), with the plant shader's overlap; 1: all.
static float _stream_rank(float p_range, float p_reach, float p_distance, float p_tile) {
	const float k = VegetationField3D::_far_keep(p_range, p_reach, MAX(p_distance - p_tile, 0.0f)) * 1.12f + 0.02f;
	return k >= 1.0f ? 1.0f : k;
}

static float _tile_distance(const Vector2i &p_tile, float p_size, const Vector2 &p_xz) {
	const Vector2 lo = Vector2(p_tile) * p_size;
	const Vector2 d(MAX(MAX(lo.x - p_xz.x, p_xz.x - (lo.x + p_size)), 0.0f), MAX(MAX(lo.y - p_xz.y, p_xz.y - (lo.y + p_size)), 0.0f));
	return d.length();
}

void VegetationField3D::_stream_update(double p_delta) {
	if (!stream_setup_valid) {
		TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
		if (biome.is_null() || !map || !map->is_baked()) {
			return; // Waits for the map; baked restarts it.
		}
		if (!_make_setup(stream_setup)) {
			return;
		}
		stream_setup_valid = true;
		impostors_traced = _path_traced();
		_copy_context(stream_setup);
		_build_cover(stream_setup);
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const uint64_t budget = uint64_t(stream_budget_msec * 1000.0f);
	if (stream_maps_job && stream_maps_job->done.is_set()) {
		_stream_maps_finish();
	}
	const Vector3 c3 = _stream_centre();
	stream_eye = c3;
	stream_eye_valid = true;
	const Vector2 centre(c3.x, c3.z);
	const float ts = stream_setup.tile_size;
	const Rect2 &rect = stream_setup.rect;
	const int species_count = stream_setup.species.size();
	bool busy = false;

	// Placed tiles back from the workers.
	for (uint32_t i = 0; i < stream_jobs.size();) {
		StreamJob *job = stream_jobs[i];
		if (!job->done.is_set()) {
			i++;
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		StreamTile **existing = stream_tiles.getptr(job->tile);
		StreamTile *old_tile = (existing && (*existing)->stale) ? *existing : nullptr;
		if (_tile_distance(job->tile, ts, centre) <= stream_radius + ts * 1.5f && (!existing || old_tile)) {
			StreamTile *t = memnew(StreamTile);
			t->out = job->out[0];
			t->nodes.resize(species_count);
			t->committed.resize(species_count);
			t->committed_keep.resize(species_count);
			t->committed_eye.resize(species_count);
			for (int s = 0; s < species_count; s++) {
				t->committed[s] = false;
				t->committed_keep[s] = 1.0f;
				t->committed_eye[s] = c3;
			}
			if (old_tile) {
				// A replacement (holes or burn ages changed): what the old tile shows is committed at once and
				// the old instances go in the same frame, so no plant pops out and back in.
				for (int s = 0; s < species_count; s++) {
					if (old_tile->committed[s]) {
						_commit_species(stream_setup, t->out, s, t->nodes[s], old_tile->committed_keep[s]);
						t->committed[s] = true;
						t->committed_keep[s] = old_tile->committed_keep[s];
					}
					_stream_free_species(old_tile, s);
				}
				plant_count -= old_tile->out.count;
				if (stream_maps_job) {
					stream_graveyard.push_back(old_tile);
				} else {
					memdelete(old_tile);
				}
				*existing = t;
			} else {
				stream_tiles.insert(job->tile, t);
			}
			t->flood_key = flood_follow ? _flood_key(job->tile, ts) : 0;
			plant_count += t->out.count;
			stream_maps_dirty = true;
			shelter_dirty = true;
		}
		memdelete(job);
		stream_jobs.remove_at_unordered(i);
	}

	// Tiles past the radius go; species past their range leave the scene, those within it join,
	// nearest first, while the frame's budget lasts.
	LocalVector<Vector2i> gone;
	// Ground opened or closed (cave mouths): once no tile is being placed (the workers read the
	// setup), the setup takes the new holes and the tiles near them are placed again.
	// Plants taken away (add_plant_removal): once no tile is being placed, the setup takes the new list and the tiles
	// there are placed again without them.
	if (removal_dirty.has_area() && stream_jobs.is_empty()) {
		stream_setup.removals = plant_removals;
		const Rect2 near = removal_dirty.grow(stream_setup.max_footprint + 0.5f);
		for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
			if (Rect2(Vector2(kv.key) * ts, Vector2(ts, ts)).intersects(near)) {
				kv.value->stale = true; // Placed again; the old one shows until the new one is in.
			}
		}
		removal_dirty = Rect2();
	}
	if (holes_dirty.has_area() && stream_jobs.is_empty()) {
		stream_setup.holes.read(VegetationHoles::source_of(stream_setup.map));
		stream_setup.crop_fields = GrassField3D::get_crop_field_mask();
		const Rect2 near = holes_dirty.grow(stream_setup.deadwood_reach + stream_setup.max_footprint + OBSTACLE_MARGIN);
		for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
			if (Rect2(Vector2(kv.key) * ts, Vector2(ts, ts)).intersects(near)) {
				kv.value->stale = true; // Placed again; the old one shows until the new one is in.
			}
		}
		holes_dirty = Rect2();
	}
	// Burn ages changed: the grid samples the area again (all of it when it held no burn) and the tiles there are
	// placed again with the new ages, each swapped in when its replacement is ready.
	if (burn_dirty.has_area() && stream_jobs.is_empty()) {
		PlaceSetup &su = stream_setup;
		if (su.burn.is_empty() || burn_dirty.encloses(su.rect)) {
			_read_burn_age(su);
		} else {
			const WorldFire *fire = WorldContracts::fire();
			const float cx = su.burn_rect.size.x / (su.burn_nx - 1);
			const float cz = su.burn_rect.size.y / (su.burn_ny - 1);
			const int x0 = CLAMP(int(Math::floor((burn_dirty.position.x - su.burn_rect.position.x) / cx)) - 1, 0, su.burn_nx - 1);
			const int x1 = CLAMP(int(Math::ceil((burn_dirty.get_end().x - su.burn_rect.position.x) / cx)) + 1, 0, su.burn_nx - 1);
			const int z0 = CLAMP(int(Math::floor((burn_dirty.position.y - su.burn_rect.position.y) / cz)) - 1, 0, su.burn_ny - 1);
			const int z1 = CLAMP(int(Math::ceil((burn_dirty.get_end().y - su.burn_rect.position.y) / cz)) + 1, 0, su.burn_ny - 1);
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 xz = su.burn_rect.position + Vector2(x * cx, z * cz);
					su.burn[z * su.burn_nx + x] = fire ? fire->burn_age_at(Vector3(xz.x, 0.0f, xz.y)) : -1.0f;
				}
			}
		}
		const float cell = su.burn.is_empty() ? 2.0f : su.burn_rect.size.x / (su.burn_nx - 1);
		const Rect2 near = burn_dirty.grow(cell + su.max_footprint);
		for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
			if (Rect2(Vector2(kv.key) * ts, Vector2(ts, ts)).intersects(near)) {
				kv.value->stale = true; // Placed again; the old one shows until the new one is in.
			}
		}
		burn_dirty = Rect2();
		variants_waiting = true; // A new burn may need deferred fire variants.
		_copy_context(su);
	}
	// The stand changed: only where a species crossed its stand range are the tiles placed again.
	if (stand_dirty.has_area() && stream_jobs.is_empty()) {
		PlaceSetup &su = stream_setup;
		Rect2 changed = _update_stand(su, stand_dirty);
		variants_waiting = true; // A new strike or browse disc may need deferred variants (checked in process).
		if (changed.has_area() && !su.edge.is_empty()) {
			// The canopy's stages moved: the forest edge moves with them (a windthrow opens a new edge).
			LocalVector<float> old_edge;
			old_edge.resize(su.edge.size());
			for (uint32_t i = 0; i < su.edge.size(); i++) {
				old_edge[i] = su.edge[i];
			}
			_read_forest_edge(su);
			if (su.edge.size() != old_edge.size()) {
				changed = su.rect;
			} else {
				const float cx = su.edge_rect.size.x / (su.edge_nx - 1);
				const float cz = su.edge_rect.size.y / (su.edge_ny - 1);
				for (int i = 0; i < int(old_edge.size()); i++) {
					if (Math::abs(old_edge[i] - su.edge[i]) > 0.25f) {
						const Vector2 xz = su.edge_rect.position + Vector2((i % su.edge_nx) * cx, (i / su.edge_nx) * cz);
						changed = changed.merge(Rect2(xz - Vector2(cx, cz) * 0.5f, Vector2(cx, cz)));
					}
				}
			}
		}
		if (changed.has_area()) {
			const Rect2 near = changed.grow(su.max_footprint);
			for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
				if (Rect2(Vector2(kv.key) * ts, Vector2(ts, ts)).intersects(near)) {
					kv.value->stale = true;
				}
			}
		}
		stand_dirty = Rect2();
		_copy_context(su);
	}
	struct Pending {
		float d;
		Vector2i tile;
		bool operator<(const Pending &p_o) const { return d < p_o.d; }
	};
	LocalVector<Pending> pending;
	for (KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		const float d = _tile_distance(kv.key, ts, centre);
		if (d > stream_radius + ts * 1.5f) {
			if (!gone.has(kv.key)) {
				gone.push_back(kv.key);
			}
			continue;
		}
		for (int s = 0; s < species_count; s++) {
			const PlaceSpecies &ps = stream_setup.species[s];
			if (ps.sp.is_null()) {
				continue;
			}
			const float range = ps.range;
			const float reach = MAX(ps.reach, range);
			const bool wanted = range <= 0.0f || d <= reach * 1.05f + ts * 0.5f;
			const bool keep = range <= 0.0f || d <= reach * 1.05f + ts * 1.5f;
			if (kv.value->committed[s] && !keep) {
				_stream_free_species(kv.value, s);
			} else if (!kv.value->committed[s] && wanted) {
				pending.push_back({ d, kv.key });
			} else if (kv.value->committed[s] && reach > range && kv.value->committed_keep[s] < 1.0f && _stream_rank(range, reach, d, ts) > kv.value->committed_keep[s] * 1.3f) {
				pending.push_back({ d, kv.key }); // Closer: its cells need more of the tail's plants.
			} else if (kv.value->committed[s] && ps.far_top > 0.0f && kv.value->committed_keep[s] < 1.0f && c3.distance_to(kv.value->committed_eye[s]) > 0.1f * d) {
				pending.push_back({ d, kv.key }); // The eye moved a tenth of the way: the skyline from it is another.
			}
		}
	}
	for (const Vector2i &key : gone) {
		StreamTile *t = stream_tiles[key];
		for (int s = 0; s < species_count; s++) {
			_stream_free_species(t, s);
		}
		plant_count -= t->out.count;
		if (stream_maps_job) {
			stream_graveyard.push_back(t);
		} else {
			memdelete(t);
		}
		stream_tiles.erase(key);
		stream_maps_dirty = true;
		shelter_dirty = true;
	}
	pending.sort();
	for (const Pending &p : pending) {
		StreamTile *t = stream_tiles[p.tile];
		for (int s = 0; s < species_count; s++) {
			const float range = stream_setup.species[s].range;
			const float reach = MAX(stream_setup.species[s].reach, range);
			if (stream_setup.species[s].sp.is_null() || (range > 0.0f && p.d > reach * 1.05f + ts * 0.5f)) {
				continue;
			}
			const float rank = reach > range ? _stream_rank(range, reach, p.d, ts) : 1.0f;
			if (t->committed[s]) {
				const bool skyline_moved = stream_setup.species[s].far_top > 0.0f && t->committed_keep[s] < 1.0f && c3.distance_to(t->committed_eye[s]) > 0.1f * p.d;
				if (!skyline_moved && (rank <= t->committed_keep[s] * 1.3f || t->committed_keep[s] >= 1.0f)) {
					continue;
				}
				// The tail's plants for the nearer distance: the new cells first, then the old go, in one frame.
				LocalVector<MultiMeshInstance3D *> fresh;
				_commit_species(stream_setup, t->out, s, fresh, rank);
				_stream_free_species(t, s);
				t->nodes[s] = fresh;
			} else {
				_commit_species(stream_setup, t->out, s, t->nodes[s], rank);
			}
			t->committed[s] = true;
			t->committed_keep[s] = rank;
			t->committed_eye[s] = c3;
		}
		if (OS::get_singleton()->get_ticks_usec() - t0 > budget) {
			busy = true;
			break;
		}
	}

	// Missing tiles within the radius, nearest first, as many at once as there are workers.
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count());
	if (int(stream_jobs.size()) < max_jobs && !holes_dirty.has_area() && !burn_dirty.has_area() && !stand_dirty.has_area() && !removal_dirty.has_area()) {
		const int r = int(Math::ceil(stream_radius / ts)) + 2;
		const Vector2i ct(int(Math::floor(centre.x / ts)), int(Math::floor(centre.y / ts)));
		const Rect2i map_tiles(Vector2i(int(Math::floor(rect.position.x / ts)), int(Math::floor(rect.position.y / ts))),
				Vector2i(int(Math::ceil((rect.position.x + rect.size.x) / ts)) - int(Math::floor(rect.position.x / ts)),
						int(Math::ceil((rect.position.y + rect.size.y) / ts)) - int(Math::floor(rect.position.y / ts))));
		LocalVector<Pending> missing;
		for (int dz = -r; dz <= r; dz++) {
			for (int dx = -r; dx <= r; dx++) {
				const Vector2i tile = ct + Vector2i(dx, dz);
				const StreamTile *const *have = stream_tiles.getptr(tile);
				if (!map_tiles.has_point(tile) || (have && !(*have)->stale)) {
					continue;
				}
				const float d = _tile_distance(tile, ts, centre);
				if (d > stream_radius + (have ? ts * 1.5f : 0.0f)) {
					continue; // A stale tile is replaced as far out as tiles are kept.
				}
				bool queued = false;
				for (const StreamJob *job : stream_jobs) {
					queued = queued || job->tile == tile;
				}
				if (!queued) {
					missing.push_back({ d, tile });
				}
			}
		}
		missing.sort();
		busy = busy || !missing.is_empty();
		for (uint32_t i = 0; i < missing.size() && int(stream_jobs.size()) < max_jobs; i++) {
			StreamJob *job = memnew(StreamJob);
			job->field = this;
			job->tile = missing[i].tile;
			// A rock_field's stones round the tile, fetched here on the main thread (RockField3D places its cells here).
			job->own_rocks = _rocks_for(stream_setup, Rect2(Vector2(job->tile) * ts, Vector2(ts, ts)), job->rocks);
			job->task = WorkerThreadPool::get_singleton()->add_native_task(&VegetationField3D::_stream_job, job, false, "VegetationField3D tile");
			stream_jobs.push_back(job);
		}
	}
	busy = busy || !stream_jobs.is_empty();
	busy = _update_far_shell(centre, t0, budget) || busy;

	// The maps (canopy, stand, floor, far colour), a couple of times a second at most.
	stream_maps_wait -= p_delta;
	if (centre.distance_to(stream_maps_centre) > stream_maps_radius * 0.25f) {
		stream_maps_dirty = true;
	}
	if (stream_maps_dirty && stream_maps_wait <= 0.0 && !stream_maps_job) {
		_stream_publish(centre);
		stream_maps_dirty = false;
		stream_maps_wait = 0.5;
	}
	stream_frame_max_msec = MAX(stream_frame_max_msec, (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0);
	const bool was_idle = stream_idle;
	stream_idle = !busy && !stream_maps_dirty && !stream_maps_job;
	if (stream_idle && !was_idle) {
		emit_signal(SNAME("populated"));
	}
}

// The maps over the tiles placed so far, computed on a worker (about 20 ms for a 200 m radius of
// oak wood); the tiles it reads stay alive until it is done.
void VegetationField3D::_stream_publish(const Vector2 &p_centre) {
	stream_maps_centre = p_centre;
	const Rect2 &rect = stream_setup.rect;
	StreamMapsJob *job = memnew(StreamMapsJob);
	job->field = this;
	LocalVector<const LocalVector<Deadwood> *> deadwood_lists;
	for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		job->crowns.push_back(&kv.value->out.crowns);
		job->far.push_back(&kv.value->out.far);
		deadwood_lists.push_back(&kv.value->out.deadwood);
	}
	_publish_deadwood(deadwood_lists);
	// Shade maps near the centre on fine cells; far colour out to the radius.
	const float mr = MIN(stream_maps_radius, stream_radius);
	job->maps.maps_rect = Rect2(p_centre - Vector2(mr, mr), Vector2(mr, mr) * 2.0f).intersection(rect);
	job->maps.far_rect = Rect2(p_centre - Vector2(stream_radius, stream_radius), Vector2(stream_radius, stream_radius) * 2.0f).intersection(rect);
	_maps_wants(job->maps);
	job->task = WorkerThreadPool::get_singleton()->add_native_task(&VegetationField3D::_stream_maps_job, job, false, "VegetationField3D maps");
	stream_maps_job = job;
}

void VegetationField3D::_stream_maps_job(void *p_job) {
	StreamMapsJob *job = static_cast<StreamMapsJob *>(p_job);
	_compute_maps(job->maps, job->crowns, job->far);
	job->done.set();
}

// Waits for the maps job, applies its maps (unless only clearing) and frees the tiles it held.
void VegetationField3D::_stream_maps_finish() {
	if (!stream_maps_job) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(stream_maps_job->task);
	if (stream_setup_valid) {
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		_apply_maps(stream_maps_job->maps);
		_publish_near_radius(stream_radius);
		stream_publish_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	}
	memdelete(stream_maps_job);
	stream_maps_job = nullptr;
	for (StreamTile *t : stream_graveyard) {
		memdelete(t);
	}
	stream_graveyard.clear();
}

// Main-thread cost of streaming: the slowest frame since the last call (reset here) and the last
// map publish, both in milliseconds.
Dictionary VegetationField3D::get_stream_stats() {
	Dictionary d;
	d["tiles"] = stream_tiles.size();
	d["jobs"] = stream_jobs.size();
	d["frame_max_msec"] = stream_frame_max_msec;
	d["publish_msec"] = stream_publish_msec;
	stream_frame_max_msec = 0.0;
	return d;
}

void VegetationField3D::set_streaming(bool p_enable) {
	if (streaming == p_enable) {
		return;
	}
	streaming = p_enable;
	if (!is_inside_tree() || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	_clear_generated();
	if (streaming) {
		set_process_internal(true);
	} else if (populate_on_ready) {
		queue_populate();
	}
}

void VegetationField3D::queue_populate() {
	if (populate_queued) {
		return;
	}
	populate_queued = true;
	callable_mp(this, &VegetationField3D::_queued_populate).call_deferred();
}

void VegetationField3D::_queued_populate() {
	populate_queued = false;
	if (is_inside_tree()) {
		populate();
	}
}

bool VegetationField3D::_path_traced() const {
	Viewport *vp = get_viewport();
	if (!vp || !vp->is_using_pathtracing()) {
		return false;
	}
	const Ref<Environment> env = WeatherSkyMaterial::find_environment(const_cast<VegetationField3D *>(this));
	return env.is_valid() && env->is_pathtracing_enabled();
}

// The near swap's crossfade margins: both cells draw inside them, dithered in and out, which the
// path tracer does not do; it would trace the plants there twice. Margins off while traced.
void VegetationField3D::_set_swap_margins(bool p_traced) {
	for (int i = 0; i < get_child_count(true); i++) {
		GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(get_child(i, true));
		if (!gi || gi->get_visibility_range_fade_mode() != GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF) {
			continue;
		}
		const float begin = gi->get_visibility_range_begin();
		if (begin > 0.0f) {
			gi->set_visibility_range_begin_margin(p_traced ? 0.0f : MAX(begin * 0.1f, 1.0f));
		}
		if (String(gi->get_name()).contains("Near")) {
			const float end = gi->get_visibility_range_end();
			gi->set_visibility_range_end_margin(p_traced ? 0.0f : MAX(end * 0.1f, 1.0f));
		}
	}
}

void VegetationField3D::_update_impostors(bool p_traced) {
	impostors_traced = p_traced;
	if (has_near_swap) {
		_set_swap_margins(p_traced);
	}
	for (uint32_t i = 0; i < impostor_cells.size(); i++) {
		Ref<MultiMesh> mm = impostor_cells[i]->get_multimesh();
		if (mm.is_valid()) {
			mm->set_mesh(p_traced ? Ref<Mesh>(impostor_meshes[i]->get_meta(SNAME("impostor_of"))) : impostor_meshes[i]);
		}
	}
}

void VegetationField3D::_on_map_baked() {
	if (streaming) {
		_clear_generated(); // Placed again from the new map, around the centre.
		// Out to the same radius again in a moment: keep it published meanwhile, so the terrain's
		// far blend does not jump back to its own far_start for the frames the tiles take.
		_publish_near_radius(stream_radius);
		set_process_internal(true);
	} else if (populate_on_ready) {
		queue_populate();
	}
}

// The generator's holes (TerrainGenerator3D.add_hole()), followed once found through the data map.
void VegetationField3D::_follow_holes(TerrainDataMap3D *p_map) {
	TerrainGenerator3D *gen = VegetationHoles::source_of(p_map);
	if (!gen || gen->get_instance_id() == hole_source) {
		return;
	}
	const Callable on_holes = callable_mp(this, &VegetationField3D::_on_holes_changed);
	if (Object *old = ObjectDB::get_instance(hole_source)) {
		if (old->is_connected(SNAME("holes_changed"), on_holes)) {
			old->disconnect(SNAME("holes_changed"), on_holes);
		}
	}
	hole_source = gen->get_instance_id();
	gen->connect(SNAME("holes_changed"), on_holes);
}

// Burn ages changed (WorldFire.burn_changed, a live fire or a scar placed, moved or aged): the streamed tiles over
// the area are placed again (_stream_update, once no tile is being placed); a populated field populates again.
void VegetationField3D::_follow_burns(double p_delta) {
	burn_check -= p_delta;
	if (!burn_follow || burn_check > 0.0) {
		return;
	}
	burn_check = 0.5;
	const WorldFire *fire = WorldContracts::fire();
	if (!fire || fire->get_burn_generation() == burn_seen) {
		return;
	}
	Rect2 area;
	if (!fire->burn_changes_since(burn_seen, area)) {
		area = Rect2(-1e6f, -1e6f, 2e6f, 2e6f);
	}
	burn_seen = fire->get_burn_generation();
	if (!area.has_area()) {
		return;
	}
	if (streaming) {
		if (stream_setup_valid && area.intersects(stream_setup.rect)) {
			burn_dirty = burn_dirty.has_area() ? burn_dirty.merge(area) : area;
		}
	} else if (plant_count > 0) {
		queue_populate();
	}
}

// The flood marks over a tile: high_water_at on a 3 x 3 lattice (corners, edge middles, centre), each peak to 5 cm
// and age to 30 days (RULE_FLOOD_AGE fades over months), hashed; no mark hashes apart from any mark.
uint64_t VegetationField3D::_flood_key(const Vector2i &p_tile, float p_ts) const {
	const WorldWater *water = WorldContracts::water();
	uint64_t h = 0x9E3779B97F4A7C15ull;
	for (int j = 0; j < 3; j++) {
		for (int i = 0; i < 3; i++) {
			const Vector3 at((p_tile.x + 0.5f * i) * p_ts, 0.0f, (p_tile.y + 0.5f * j) * p_ts);
			const Vector4 hw = water ? water->high_water_at(at) : Vector4(-Math::INF, -1.0f, 0.0f, 0.0f);
			const bool known = hw.x > -1e20f && hw.y >= 0.0f;
			const int64_t peak = known ? int64_t(Math::floor(hw.x * 20.0f)) : INT64_MIN;
			const int64_t age = known ? int64_t(Math::floor(hw.y / 30.0f)) : -1;
			h = hash_murmur3_one_64(uint64_t(peak), hash_murmur3_one_64(uint64_t(age), h));
		}
	}
	return h == 0 ? 1 : h;
}

// A flood peaked, fell or aged (WorldWater.high_water_at: a river's storm, a lake's flood, the sea's storm run-up):
// streamed tiles whose marks changed are placed again, each swapped in when its replacement is ready (the wrack moves
// to the new line; the old one goes). Twice the burn check's interval: marks move over hours, not seconds.
void VegetationField3D::_follow_floods(double p_delta) {
	flood_check -= p_delta;
	if (flood_check > 0.0 || !streaming || !stream_setup_valid) {
		return;
	}
	flood_check = 2.0;
	bool wanted = false;
	for (const PlaceSpecies &ps : stream_setup.species) {
		wanted = wanted || (ps.sp.is_valid() && ps.sp->uses_high_water()) || !ps.flood_marks.is_empty();
	}
	if (!wanted) {
		flood_follow = false;
		return;
	}
	const float ts = stream_setup.tile_size;
	for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		StreamTile *t = kv.value;
		const uint64_t key = _flood_key(kv.key, ts);
		if (!flood_follow || t->flood_key == 0) {
			t->flood_key = key; // First look (or the species came in): the tile was placed with these marks.
			continue;
		}
		if (key != t->flood_key && !t->stale) {
			t->stale = true; // Placed again; the old one shows until the new one is in.
			t->flood_key = key;
			flood_tiles++;
		}
	}
	flood_follow = true;
}

// A hay meadow mown, or its days moved on (GrassField3D.get_hay_generation): streamed tiles with a herb that is cut
// are placed again; a populated field populates again.
void VegetationField3D::_follow_hay(double p_delta) {
	hay_check -= p_delta;
	if (hay_check > 0.0) {
		return;
	}
	hay_check = 1.0;
	const uint64_t g = GrassField3D::get_hay_generation();
	if (g == hay_seen) {
		return;
	}
	if (hay_seen == 0) {
		hay_seen = g; // Nothing placed yet.
		return;
	}
	hay_seen = g;
	if (streaming && stream_setup_valid) {
		bool wanted = false;
		for (const PlaceSpecies &ps : stream_setup.species) {
			wanted = wanted || ps.hay_cut;
		}
		if (wanted) {
			for (const KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
				kv.value->stale = true; // Placed again; the old one shows until the new one is in.
			}
		}
	} else if (!streaming && plant_count > 0) {
		queue_populate();
	}
}

// Stand ages or disturbances changed (WorldStand.stand_changed: a disturbance added, aged or removed, years passing,
// fire's burns): streamed tiles are placed again where a species crossed its stand range (_stream_update); a
// populated field populates again.
void VegetationField3D::_follow_stand(double p_delta) {
	stand_check -= p_delta;
	if (!stand_follow || stand_check > 0.0) {
		return;
	}
	stand_check = 0.5;
	const WorldStand *stand = WorldContracts::stand();
	if (!stand || stand->get_stand_generation() == stand_seen) {
		return;
	}
	Rect2 area;
	if (!stand->stand_changes_since(stand_seen, area)) {
		area = Rect2(-1e6f, -1e6f, 2e6f, 2e6f);
	}
	stand_seen = stand->get_stand_generation();
	if (!area.has_area()) {
		return;
	}
	if (streaming) {
		if (stream_setup_valid && area.intersects(stream_setup.rect)) {
			stand_dirty = stand_dirty.has_area() ? stand_dirty.merge(area) : area;
		}
	} else if (plant_count > 0) {
		queue_populate();
	}
}

// Crop fields came, went or moved (GrassField3D.fields): rebuild clear of them.
void VegetationField3D::_on_crop_fields_changed() {
	_on_holes_changed(Rect2(-1e6f, -1e6f, 2e6f, 2e6f));
}

void VegetationField3D::_on_holes_changed(const Rect2 &p_rect) {
	if (streaming) {
		if (stream_setup_valid) {
			holes_dirty = holes_dirty.has_area() ? holes_dirty.merge(p_rect) : p_rect;
		}
	} else if (plant_count > 0 || populate_on_ready) {
		queue_populate();
	}
}

void VegetationField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			add_to_group(SNAME("vegetation_crop_aware"));
			_update_bark_detail();
			_read_world_trails();
			TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
			if (map) {
				// Repopulate whenever the map is baked again (terrain edits).
				map->connect(SNAME("baked"), callable_mp(this, &VegetationField3D::_on_map_baked));
				_follow_holes(map);
				if (map->is_baked() && populate_on_ready && !streaming) {
					queue_populate();
				}
			}
			// Always: the contact trample field steps here (and streaming, when on).
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (plant_color_registered) {
				WorldContracts::unregister_plant_color(this);
				plant_color_registered = false;
			}
			if (contact_trample_owner == this) {
				contact_trample.release();
				contact_trample_owner = nullptr;
			}
		} break;
		case NOTIFICATION_PREDELETE: {
			_clear_generated();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_contact_trample_step(get_process_delta_time());
			// A few times a second: has path tracing been switched on or off?
			impostor_check -= get_process_delta_time();
			if (impostor_check <= 0.0) {
				impostor_check = 0.25;
				const bool traced = _path_traced();
				if (traced != impostors_traced) {
					_update_impostors(traced);
				}
			}
			_follow_burns(get_process_delta_time());
			_follow_floods(get_process_delta_time());
			_follow_hay(get_process_delta_time());
			_follow_stand(get_process_delta_time());
			_follow_trails(get_process_delta_time());
			if (variants_waiting && streaming && stream_setup_valid && stream_jobs.is_empty()) {
				// A new burn, strike or browse disc (trees.md 5bc): deferred variants it needs grow on a worker; once in,
				// the field streams again with them.
				variants_check -= get_process_delta_time();
				if (variants_check <= 0.0) {
					variants_check = 0.25;
					LocalVector<int> grown;
					const bool grew_now = _realize_variants(stream_setup, false, &grown);
					if (grew_now) {
						// Only the grown species are set up again (their meshes and variant tables; their place in
						// the order and their halos stay), and every tile is placed again, the old one showing until
						// its replacement is in, so nothing blinks out and the frame is not held for a whole setup.
						const real_t pixel = VegetationDetail::pixel_angle(this, detail_reference_height);
						for (int s : grown) {
							if (s >= int(stream_setup.species.size())) {
								continue;
							}
							PlaceSpecies &old = stream_setup.species[s];
							const int nebkha_slot = old.nebkha_slot;
							const bool desert_shrub = old.desert_shrub;
							const Vector2 colonnade = old.colonnade;
							const float halo = old.halo;
							const Ref<PlantSpecies> sp = old.sp;
							stream_setup.species[s] = PlaceSpecies();
							_setup_species(stream_setup, s, sp, pixel);
							PlaceSpecies &ps = stream_setup.species[s];
							ps.nebkha_slot = nebkha_slot;
							ps.desert_shrub = desert_shrub;
							ps.colonnade = colonnade;
							ps.halo = halo;
						}
						_copy_context(stream_setup);
						for (KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
							kv.value->stale = true;
						}
					}
				}
			}
			if (streaming) {
				if (still_shot && still_frozen && _stream_centre().distance_to(still_at) >= 0.5f) {
					still_frozen = false;
				}
				if (!(still_shot && still_frozen)) {
					_stream_update(get_process_delta_time());
					if (still_shot && stream_idle && stream_setup_valid) {
						// Settled: freeze, and drop the placement patterns (rebuilt if it moves on).
						still_frozen = true;
						still_at = _stream_centre();
						patterns.clear();
					}
				}
			}
			if (!lazy_near.is_empty()) {
				_lazy_near_update(_stream_centre());
			}
			if (_far_view_active()) {
				_far_view_step();
			}
		} break;
	}
}

VegetationField3D *VegetationField3D::contact_trample_owner = nullptr;

// One VegetationField3D keeps the ground cover's trample field when no GrassField3D does (grass keeps its own, with
// its wear and recovery tuned to the sward): bodies then lay herbs, ferns and heath down on bare ground too.
void VegetationField3D::_contact_trample_step(double p_delta) {
	bool grass = false;
	for (Node *n : get_tree()->get_nodes_in_group(SNAME("grass_trample_field"))) {
		if (GrassField3D *g = Object::cast_to<GrassField3D>(n)) {
			grass = grass || g->is_trample_enabled();
		}
	}
	if (grass) {
		if (contact_trample_owner == this) {
			contact_trample.release();
			contact_trample_owner = nullptr;
		}
		return;
	}
	if (contact_trample_owner == nullptr) {
		contact_trample_owner = this;
	}
	if (contact_trample_owner != this) {
		return;
	}
	Viewport *vp = get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	const Vector3 center = cam ? cam->get_global_position() : get_global_position();
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	if (map && !map->is_baked()) {
		map = nullptr;
	}
	const float base = get_global_position().y;
	// Plants up to about 1.5 m (the vegetation's band; taller shrubs are rigged and bend through TreeWind3D).
	contact_trample.step(get_tree(), center, p_delta, 1.5f, [&](const Vector3 &p) { return map ? map->get_height_at(p) : base; });
}

void VegetationField3D::set_biome(const Ref<Biome> &p_biome) {
	biome = p_biome;
	_update_bark_detail();
}

void VegetationField3D::set_bark_detail(bool p_enabled) {
	bark_detail = p_enabled;
	_update_bark_detail();
}

Node *VegetationField3D::get_bark_detail_node() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(bark_detail_node));
}

// The close-up bark on this field's trunks: a BarkDetail3D made once a species has trunks (tree_colliders), as an
// internal child (not saved with the scene) following this field and the camera; gone when switched off. A scene
// that adds its own BarkDetail3D under the field keeps it, and none is made.
void VegetationField3D::_update_bark_detail() {
	if (!is_inside_tree() || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	Node *existing = get_bark_detail_node();
	bool want = bark_detail && biome.is_valid();
	if (want) {
		for (int i = 0; i < get_child_count(false); i++) {
			if (Object::cast_to<BarkDetail3D>(get_child(i, false))) {
				want = false; // The scene's own.
			}
		}
	}
	if (want) {
		bool trunks = false;
		const TypedArray<PlantSpecies> list = biome->get_species();
		for (int i = 0; i < list.size() && !trunks; i++) {
			const Ref<PlantSpecies> sp = list[i];
			if (sp.is_null()) {
				continue;
			}
			const TypedArray<Mesh> meshes = sp->get_meshes();
			for (int m = 0; m < meshes.size() && !trunks; m++) {
				const Ref<Mesh> mesh = meshes[m];
				trunks = mesh.is_valid() && mesh->has_meta(SNAME("tree_colliders"));
			}
		}
		want = trunks;
	}
	if (want && !existing) {
		BarkDetail3D *bd = memnew(BarkDetail3D);
		bd->set_name("BarkDetail");
		add_child(bd, false, INTERNAL_MODE_BACK);
		bd->set_vegetation(bd->get_path_to(this));
		bark_detail_node = bd->get_instance_id();
	} else if (!want && existing && !bark_detail) {
		existing->queue_free();
		bark_detail_node = ObjectID();
	}
}

void VegetationField3D::set_data_map(const NodePath &p_path) {
	data_map = p_path;
}

void VegetationField3D::set_tile_size(float p_size) {
	tile_size = MAX(p_size, 1.0f);
	patterns.clear();
}

void VegetationField3D::set_seed(int p_seed) {
	seed = p_seed;
	patterns.clear();
}

void VegetationField3D::set_populate_on_ready(bool p_enable) {
	populate_on_ready = p_enable;
}

void VegetationField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_crop_fields_changed"), &VegetationField3D::_on_crop_fields_changed);
	ClassDB::bind_method(D_METHOD("get_contact_trample_at", "position"), &VegetationField3D::get_contact_trample_at);
	ClassDB::bind_method(D_METHOD("set_biome", "biome"), &VegetationField3D::set_biome);
	ClassDB::bind_method(D_METHOD("get_biome"), &VegetationField3D::get_biome);
	ClassDB::bind_method(D_METHOD("set_data_map", "path"), &VegetationField3D::set_data_map);
	ClassDB::bind_method(D_METHOD("get_data_map"), &VegetationField3D::get_data_map);
	ClassDB::bind_method(D_METHOD("set_density_mask", "mask"), &VegetationField3D::set_density_mask);
	ClassDB::bind_method(D_METHOD("get_density_mask"), &VegetationField3D::get_density_mask);
	ClassDB::bind_method(D_METHOD("set_density_mask_rect", "rect"), &VegetationField3D::set_density_mask_rect);
	ClassDB::bind_method(D_METHOD("get_density_mask_rect"), &VegetationField3D::get_density_mask_rect);
	ClassDB::bind_method(D_METHOD("set_obstacles", "obstacles"), &VegetationField3D::set_obstacles);
	ClassDB::bind_method(D_METHOD("set_rock_field", "path"), &VegetationField3D::set_rock_field);
	ClassDB::bind_method(D_METHOD("get_rock_field"), &VegetationField3D::get_rock_field);
	ClassDB::bind_method(D_METHOD("set_rock_obstacle_min", "size"), &VegetationField3D::set_rock_obstacle_min);
	ClassDB::bind_method(D_METHOD("set_climate_bare", "enabled"), &VegetationField3D::set_climate_bare);
	ClassDB::bind_method(D_METHOD("is_climate_bare"), &VegetationField3D::is_climate_bare);
	ClassDB::bind_method(D_METHOD("get_rock_obstacle_min"), &VegetationField3D::get_rock_obstacle_min);
	ClassDB::bind_method(D_METHOD("set_rock_footprint_max", "radius"), &VegetationField3D::set_rock_footprint_max);
	ClassDB::bind_method(D_METHOD("get_rock_footprint_max"), &VegetationField3D::get_rock_footprint_max);
	ClassDB::bind_method(D_METHOD("get_rock_chunk_count"), &VegetationField3D::get_rock_chunk_count);
	ClassDB::bind_method(D_METHOD("set_paths", "paths"), &VegetationField3D::set_paths);
	ClassDB::bind_method(D_METHOD("get_paths"), &VegetationField3D::get_paths);
	ClassDB::bind_method(D_METHOD("get_path_passes_at", "position"), &VegetationField3D::get_path_passes_at);
	ClassDB::bind_method(D_METHOD("sample_lateral_cover", "position"), &VegetationField3D::sample_lateral_cover);
	ClassDB::bind_method(D_METHOD("get_obstacles"), &VegetationField3D::get_obstacles);
	ClassDB::bind_method(D_METHOD("set_stone_tops", "tops"), &VegetationField3D::set_stone_tops);
	ClassDB::bind_method(D_METHOD("get_stone_tops"), &VegetationField3D::get_stone_tops);
	ClassDB::bind_method(D_METHOD("set_stone_top_normals", "normals"), &VegetationField3D::set_stone_top_normals);
	ClassDB::bind_method(D_METHOD("get_stone_top_normals"), &VegetationField3D::get_stone_top_normals);
	ClassDB::bind_method(D_METHOD("set_canopy_field", "path"), &VegetationField3D::set_canopy_field);
	ClassDB::bind_method(D_METHOD("get_canopy_field"), &VegetationField3D::get_canopy_field);
	ClassDB::bind_method(D_METHOD("set_litter_wind", "wind"), &VegetationField3D::set_litter_wind);
	ClassDB::bind_method(D_METHOD("get_litter_wind"), &VegetationField3D::get_litter_wind);
	ClassDB::bind_method(D_METHOD("set_spring_pools", "path"), &VegetationField3D::set_spring_pools);
	ClassDB::bind_method(D_METHOD("get_spring_pools"), &VegetationField3D::get_spring_pools);
	ClassDB::bind_method(D_METHOD("set_weather_map", "path"), &VegetationField3D::set_weather_map);
	ClassDB::bind_method(D_METHOD("get_weather_map"), &VegetationField3D::get_weather_map);
	ClassDB::bind_method(D_METHOD("set_tile_size", "size"), &VegetationField3D::set_tile_size);
	ClassDB::bind_method(D_METHOD("get_tile_size"), &VegetationField3D::get_tile_size);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &VegetationField3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &VegetationField3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_populate_on_ready", "enable"), &VegetationField3D::set_populate_on_ready);
	ClassDB::bind_method(D_METHOD("is_populate_on_ready"), &VegetationField3D::is_populate_on_ready);
	ClassDB::bind_method(D_METHOD("populate"), &VegetationField3D::populate);
	ClassDB::bind_method(D_METHOD("queue_populate"), &VegetationField3D::queue_populate);
	ClassDB::bind_method(D_METHOD("clear"), &VegetationField3D::clear);
	ClassDB::bind_method(D_METHOD("get_plant_count"), &VegetationField3D::get_plant_count);
	ClassDB::bind_method(D_METHOD("get_populate_time_msec"), &VegetationField3D::get_populate_time_msec);
	ClassDB::bind_method(D_METHOD("get_placements", "species"), &VegetationField3D::get_placements);
	ClassDB::bind_method(D_METHOD("get_flood_replaced_tiles"), &VegetationField3D::get_flood_replaced_tiles);
	ClassDB::bind_method(D_METHOD("get_trunks_near", "center", "radius", "height"), &VegetationField3D::get_trunks_near, DEFVAL(4.0f));
	ClassDB::bind_method(D_METHOD("get_deadwood_count"), &VegetationField3D::get_deadwood_count);
	ClassDB::bind_method(D_METHOD("get_crowns_in", "rect"), &VegetationField3D::get_crowns_in);
	ClassDB::bind_method(D_METHOD("get_deadwood_at", "global_position", "reach", "min_radius"), &VegetationField3D::get_deadwood_at, DEFVAL(0.0f), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_sward_shade_at", "global_position"), &VegetationField3D::get_sward_shade_at);
	ClassDB::bind_method(D_METHOD("get_sward_litter_at", "global_position"), &VegetationField3D::get_sward_litter_at);
	ClassDB::bind_static_method("VegetationField3D", D_METHOD("get_near_radius"), &VegetationField3D::get_near_radius);
	ClassDB::bind_method(D_METHOD("get_crowns_near", "position", "radius"), &VegetationField3D::get_crowns_near);
	ClassDB::bind_method(D_METHOD("get_shade_at", "global_position"), &VegetationField3D::get_shade_at);
	ClassDB::bind_method(D_METHOD("get_light_at", "global_position"), &VegetationField3D::get_light_at);
	ClassDB::bind_method(D_METHOD("get_stand_shade_at", "global_position"), &VegetationField3D::get_stand_shade_at);
	ClassDB::bind_static_method("VegetationField3D", D_METHOD("get_floor_shader_code"), &VegetationField3D::get_floor_shader_code);
	ClassDB::bind_static_method("VegetationField3D", D_METHOD("make_game_trails", "data_map", "area", "goals", "options"), &VegetationField3D::make_game_trails, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("set_forest_floor_enabled", "enabled"), &VegetationField3D::set_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("is_forest_floor_enabled"), &VegetationField3D::is_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("get_conifer_at", "global_position"), &VegetationField3D::get_conifer_at);
	ClassDB::bind_method(D_METHOD("get_litter_at", "global_position"), &VegetationField3D::get_litter_at);
	ClassDB::bind_method(D_METHOD("get_litter_drift_at", "global_position"), &VegetationField3D::get_litter_drift_at);
	ClassDB::bind_method(D_METHOD("get_spring_wetness_at", "global_position"), &VegetationField3D::get_spring_wetness_at);
	ClassDB::bind_method(D_METHOD("get_springs"), &VegetationField3D::get_springs);
	ClassDB::bind_method(D_METHOD("add_canopy_loss", "xz", "radius", "keep"), &VegetationField3D::add_canopy_loss, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_canopy_losses"), &VegetationField3D::get_canopy_losses);
	ClassDB::bind_method(D_METHOD("clear_canopy_losses"), &VegetationField3D::clear_canopy_losses);
	ClassDB::bind_method(D_METHOD("add_plant_removal", "position", "radius", "species"), &VegetationField3D::add_plant_removal, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_plant_removals"), &VegetationField3D::get_plant_removals);
	ClassDB::bind_method(D_METHOD("clear_plant_removals"), &VegetationField3D::clear_plant_removals);
	ClassDB::bind_method(D_METHOD("get_instances_near", "species", "position", "radius"), &VegetationField3D::get_instances_near);
	ClassDB::bind_method(D_METHOD("set_cover_map_enabled", "enable"), &VegetationField3D::set_cover_map_enabled);
	ClassDB::bind_method(D_METHOD("is_cover_map_enabled"), &VegetationField3D::is_cover_map_enabled);
	ClassDB::bind_method(D_METHOD("set_cover_grass", "path"), &VegetationField3D::set_cover_grass);
	ClassDB::bind_method(D_METHOD("get_cover_grass"), &VegetationField3D::get_cover_grass);
	ClassDB::bind_method(D_METHOD("set_cover_grass_layers", "paths"), &VegetationField3D::set_cover_grass_layers);
	ClassDB::bind_method(D_METHOD("get_cover_grass_layers"), &VegetationField3D::get_cover_grass_layers);
	ClassDB::bind_method(D_METHOD("set_cover_cell", "cell"), &VegetationField3D::set_cover_cell);
	ClassDB::bind_method(D_METHOD("set_far_volume", "enabled"), &VegetationField3D::set_far_volume);
	ClassDB::bind_method(D_METHOD("set_bark_detail", "enabled"), &VegetationField3D::set_bark_detail);
	ClassDB::bind_method(D_METHOD("is_bark_detail"), &VegetationField3D::is_bark_detail);
	ClassDB::bind_method(D_METHOD("get_bark_detail_node"), &VegetationField3D::get_bark_detail_node);
	ClassDB::bind_method(D_METHOD("set_far_match_view", "enabled"), &VegetationField3D::set_far_match_view);
	ClassDB::bind_method(D_METHOD("is_far_match_view"), &VegetationField3D::is_far_match_view);
	ClassDB::bind_method(D_METHOD("get_far_shade"), &VegetationField3D::get_far_shade);
	ClassDB::bind_method(D_METHOD("get_far_match_stats"), &VegetationField3D::get_far_match_stats);
	ClassDB::bind_method(D_METHOD("is_far_volume"), &VegetationField3D::is_far_volume);
	ClassDB::bind_method(D_METHOD("set_far_volume_range", "range"), &VegetationField3D::set_far_volume_range);
	ClassDB::bind_method(D_METHOD("get_far_volume_range"), &VegetationField3D::get_far_volume_range);
	ClassDB::bind_method(D_METHOD("set_far_shell", "enabled"), &VegetationField3D::set_far_shell);
	ClassDB::bind_method(D_METHOD("is_far_shell"), &VegetationField3D::is_far_shell);
	ClassDB::bind_method(D_METHOD("set_far_shell_range", "range"), &VegetationField3D::set_far_shell_range);
	ClassDB::bind_method(D_METHOD("get_far_shell_range"), &VegetationField3D::get_far_shell_range);
	ClassDB::bind_method(D_METHOD("set_far_shell_clumps", "enabled"), &VegetationField3D::set_far_shell_clumps);
	ClassDB::bind_method(D_METHOD("is_far_shell_clumps"), &VegetationField3D::is_far_shell_clumps);
	ClassDB::bind_method(D_METHOD("get_far_shell_column_count"), &VegetationField3D::get_far_shell_column_count);
	ClassDB::bind_method(D_METHOD("set_far_reach", "reach"), &VegetationField3D::set_far_reach);
	ClassDB::bind_method(D_METHOD("get_far_reach"), &VegetationField3D::get_far_reach);
	ClassDB::bind_method(D_METHOD("set_detail_reach", "enabled"), &VegetationField3D::set_detail_reach);
	ClassDB::bind_method(D_METHOD("get_detail_reach"), &VegetationField3D::get_detail_reach);
	ClassDB::bind_method(D_METHOD("set_detail_pixels", "pixels"), &VegetationField3D::set_detail_pixels);
	ClassDB::bind_method(D_METHOD("get_detail_pixels"), &VegetationField3D::get_detail_pixels);
	ClassDB::bind_method(D_METHOD("set_detail_reference_height", "height"), &VegetationField3D::set_detail_reference_height);
	ClassDB::bind_method(D_METHOD("get_detail_reference_height"), &VegetationField3D::get_detail_reference_height);
	ClassDB::bind_method(D_METHOD("set_detail_max_range", "range"), &VegetationField3D::set_detail_max_range);
	ClassDB::bind_method(D_METHOD("get_detail_max_range"), &VegetationField3D::get_detail_max_range);
	ClassDB::bind_method(D_METHOD("get_species_range", "species"), &VegetationField3D::get_species_range);
	ClassDB::bind_method(D_METHOD("set_far_volume_cell", "cell"), &VegetationField3D::set_far_volume_cell);
	ClassDB::bind_method(D_METHOD("get_far_volume_cell"), &VegetationField3D::get_far_volume_cell);
	ClassDB::bind_method(D_METHOD("get_cover_cell"), &VegetationField3D::get_cover_cell);
	ClassDB::bind_method(D_METHOD("get_cover_texture"), &VegetationField3D::get_cover_texture);
	ClassDB::bind_method(D_METHOD("set_streaming", "enable"), &VegetationField3D::set_streaming);
	ClassDB::bind_method(D_METHOD("is_streaming"), &VegetationField3D::is_streaming);
	ClassDB::bind_method(D_METHOD("set_stream_target", "path"), &VegetationField3D::set_stream_target);
	ClassDB::bind_method(D_METHOD("get_stream_target"), &VegetationField3D::get_stream_target);
	ClassDB::bind_method(D_METHOD("set_stream_radius", "radius"), &VegetationField3D::set_stream_radius);
	ClassDB::bind_method(D_METHOD("get_stream_radius"), &VegetationField3D::get_stream_radius);
	ClassDB::bind_method(D_METHOD("set_stream_budget_msec", "msec"), &VegetationField3D::set_stream_budget_msec);
	ClassDB::bind_method(D_METHOD("get_stream_budget_msec"), &VegetationField3D::get_stream_budget_msec);
	ClassDB::bind_method(D_METHOD("set_stream_maps_radius", "radius"), &VegetationField3D::set_stream_maps_radius);
	ClassDB::bind_method(D_METHOD("get_stream_maps_radius"), &VegetationField3D::get_stream_maps_radius);
	ClassDB::bind_method(D_METHOD("get_stream_tile_count"), &VegetationField3D::get_stream_tile_count);
	ClassDB::bind_method(D_METHOD("is_stream_idle"), &VegetationField3D::is_stream_idle);
	ClassDB::bind_method(D_METHOD("set_still_shot", "enabled"), &VegetationField3D::set_still_shot);
	ClassDB::bind_method(D_METHOD("is_still_shot"), &VegetationField3D::is_still_shot);
	ClassDB::bind_method(D_METHOD("is_still_frozen"), &VegetationField3D::is_still_frozen);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "still_shot"), "set_still_shot", "is_still_shot");
	ClassDB::bind_method(D_METHOD("get_stream_stats"), &VegetationField3D::get_stream_stats);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "biome", PROPERTY_HINT_RESOURCE_TYPE, "Biome"), "set_biome", "get_biome");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "litter_wind"), "set_litter_wind", "get_litter_wind");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "spring_pools", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "OasisSprings3D"), "set_spring_pools", "get_spring_pools");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "canopy_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_canopy_field", "get_canopy_field");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forest_floor_enabled"), "set_forest_floor_enabled", "is_forest_floor_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "density_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_density_mask", "get_density_mask");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "density_mask_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_density_mask_rect", "get_density_mask_rect");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "obstacles"), "set_obstacles", "get_obstacles");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "rock_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "RockField3D"), "set_rock_field", "get_rock_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_obstacle_min", PROPERTY_HINT_RANGE, "0,4,0.05,suffix:m"), "set_rock_obstacle_min", "get_rock_obstacle_min");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "climate_bare"), "set_climate_bare", "is_climate_bare");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_footprint_max", PROPERTY_HINT_RANGE, "0,128,0.5,suffix:m"), "set_rock_footprint_max", "get_rock_footprint_max");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "paths", PROPERTY_HINT_ARRAY_TYPE, "Dictionary"), "set_paths", "get_paths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "stone_tops"), "set_stone_tops", "get_stone_tops");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "stone_top_normals"), "set_stone_top_normals", "get_stone_top_normals");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "1,256,1,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "populate_on_ready"), "set_populate_on_ready", "is_populate_on_ready");

	ADD_GROUP("Cover map", "cover_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cover_map_enabled"), "set_cover_map_enabled", "is_cover_map_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "cover_grass", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GrassField3D"), "set_cover_grass", "get_cover_grass");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "cover_grass_layers", PROPERTY_HINT_ARRAY_TYPE, vformat("%s/%s:%s", Variant::NODE_PATH, PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GrassField3D")), "set_cover_grass_layers", "get_cover_grass_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cover_cell", PROPERTY_HINT_RANGE, "0.5,64,0.5,suffix:m"), "set_cover_cell", "get_cover_cell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_match_view"), "set_far_match_view", "is_far_match_view");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bark_detail"), "set_bark_detail", "is_bark_detail");
	ADD_GROUP("Far volume", "far_volume");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_volume"), "set_far_volume", "is_far_volume");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_volume_range", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:m"), "set_far_volume_range", "get_far_volume_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_shell"), "set_far_shell", "is_far_shell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_shell_range", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m"), "set_far_shell_range", "get_far_shell_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_shell_clumps"), "set_far_shell_clumps", "is_far_shell_clumps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_reach", PROPERTY_HINT_RANGE, "1,8,0.1"), "set_far_reach", "get_far_reach");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "detail_reach"), "set_detail_reach", "get_detail_reach");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_pixels", PROPERTY_HINT_RANGE, "0.25,16,0.25,suffix:px"), "set_detail_pixels", "get_detail_pixels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_reference_height", PROPERTY_HINT_RANGE, "0,4320,1,suffix:px"), "set_detail_reference_height", "get_detail_reference_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_max_range", PROPERTY_HINT_RANGE, "0,200,0.5,or_greater,suffix:m"), "set_detail_max_range", "get_detail_max_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_volume_cell", PROPERTY_HINT_RANGE, "0.05,4,0.01,suffix:m"), "set_far_volume_cell", "get_far_volume_cell");
	ADD_GROUP("Streaming", "stream");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming", "is_streaming");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "stream_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_stream_target", "get_stream_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_radius", PROPERTY_HINT_RANGE, "16,4096,1,or_greater,suffix:m"), "set_stream_radius", "get_stream_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_budget_msec", PROPERTY_HINT_RANGE, "0.1,16,0.1,suffix:ms"), "set_stream_budget_msec", "get_stream_budget_msec");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_maps_radius", PROPERTY_HINT_RANGE, "8,1024,1,or_greater,suffix:m"), "set_stream_maps_radius", "get_stream_maps_radius");

	ADD_SIGNAL(MethodInfo("populated"));
}

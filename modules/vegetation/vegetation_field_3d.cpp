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

#include "grass_field_3d.h"
#include "vegetation_light.h"

// Rigged trees (TreeWind3D, from the trees work) when that class is in the module.
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
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/weather/weather_sky_material.h"
#include "scene/main/viewport.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"

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
			p.x = Math::fposmod(p.x, p_size);
			p.y = Math::fposmod(p.y, p_size);
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

void VegetationField3D::_clear_generated() {
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
	plant_count = 0;
}

void VegetationField3D::clear() {
	_clear_generated();
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
bool VegetationField3D::_make_setup(PlaceSetup &r_setup) {
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
	r_setup.litter_reach = 3.0f * LITTER_SPREAD * (1.0f + r_setup.litter_skew.length());
	litter_skew = r_setup.litter_skew;
	const TypedArray<PlantSpecies> species_list = biome->get_species();
	const int count = species_list.size();
	r_setup.species.resize(count);
	for (int s = 0; s < count; s++) {
		PlaceSpecies &ps = r_setup.species[s];
		ps.sp = species_list[s];
		if (ps.sp.is_null()) {
			continue;
		}
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
		if (canopy_r > 0.0f && ps.meshes.size() > 0 && ps.meshes[0].is_valid()) {
			ps.canopy_top = ps.meshes[0]->get_aabb().get_end().y;
			ps.stem_foot = _foot_radius(ps.meshes[0], ps.footprint > 0.0f ? ps.footprint : 1.0f);
		}
		ps.crown_max = canopy_r * MAX(ps.sp->get_scale_range().x, ps.sp->get_scale_range().y);
		r_setup.crown_max = MAX(r_setup.crown_max, ps.crown_max);
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
			}
		}
		if (ps.sp->get_deadwood_rule() == PlantSpecies::DEADWOOD_BESIDE) {
			r_setup.deadwood_reach = MAX(r_setup.deadwood_reach, ps.sp->get_deadwood_band());
		}
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
			if (ps.deadwood_reach > 0.0f && later.sp->get_deadwood_rule() != PlantSpecies::DEADWOOD_IGNORE) {
				halo = MAX(halo, later.halo + ps.deadwood_reach + later.sp->get_deadwood_band());
			}
		}
		ps.halo = halo;
	}
	r_setup.grid_cell = MAX(1.0f, r_setup.max_footprint * 2.0f);
	r_setup.sun_mean = VegetationLight::sun_mean(map);
	return true;
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
bool deadwood_nearest(const Grid &p_grid, const Logs &p_logs, const Vector2 &p_xz, float p_reach, float p_cell, Hit &r_hit) {
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

} // namespace

int VegetationField3D::get_deadwood_count() const {
	RWLockRead guard(crown_lock);
	return deadwood.size();
}

// The placed log whose side is nearest the point, within reach: side (m from its side across the
// ground, negative under it), radius, top (height of its upper surface there) and normal; empty
// when there is none.
Dictionary VegetationField3D::get_deadwood_at(const Vector3 &p_global_position, float p_reach) const {
	DeadwoodHit hit;
	Dictionary d;
	if (_deadwood_at(Vector2(p_global_position.x, p_global_position.z), MAX(p_reach, 0.0f), hit)) {
		d["side"] = hit.side;
		d["radius"] = hit.radius;
		d["top"] = hit.top;
		d["normal"] = hit.normal;
	}
	return d;
}

bool VegetationField3D::_deadwood_at(const Vector2 &p_xz, float p_reach, DeadwoodHit &r_hit) const {
	RWLockRead guard(crown_lock);
	return deadwood_nearest(deadwood_grid, deadwood, p_xz, p_reach, DEADWOOD_CELL, r_hit);
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

void VegetationField3D::_place(const PlaceSetup &p_setup, const Rect2i &p_tiles, bool p_halo, LocalVector<TileOut> &r_tiles) const {
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
		const int variants = MAX(1, int(ps.meshes.size()));
		const Pattern &pat = ps.pattern;
		const float fp = ps.footprint;
		const Vector2 scale_range = sp->get_scale_range();
		const float align = sp->get_align_to_ground();
		const int reach = int(Math::ceil((fp + p_setup.max_footprint) / grid_cell));
		const bool shade_rule = sp->uses_shade();
		const bool conifer_rule = sp->uses_conifer();
		const bool litter_rule = sp->uses_litter();
		const float canopy_r = sp->get_canopy_radius();
		const float range = sp->get_visibility_range();
		const bool is_log = ps.deadwood_reach > 0.0f;
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

		for (int tz = tz0; tz < tz1; tz++) {
			for (int tx = tx0; tx < tx1; tx++) {
				const Vector2i tile(tx, tz);
				const bool in_core = p_tiles.has_point(tile);
				TileOut *out = in_core ? &r_tiles[(tz - p_tiles.position.y) * p_tiles.size.x + (tx - p_tiles.position.x)] : nullptr;
				if (out) {
					out->species[s].xforms.resize(variants);
					out->species[s].customs.resize(variants);
				}
				const float shift = float(tile_hash(tx, tz, p_setup.seed, s) & 0xFFFFFF) / float(0x1000000);
				for (uint32_t i = 0; i < pat.points.size(); i++) {
					const Vector2 xz = Vector2(tx * ts, tz * ts) + pat.points[i];
					if (!rect.has_point(xz) || (!in_core && !reach_rect.has_point(xz))) {
						continue;
					}
					// Ordered dither: a point survives where its (per-tile shifted) rank is below
					// the density there, so density changes smoothly and the pattern stays blue noise.
					const float rank = Math::fmod(pat.ranks[i] + shift, 1.0f);
					if (rank >= sp->get_density()) {
						continue;
					}
					const Vector3 p3(xz.x, 0.0f, xz.y);
					float suit = sp->get_suitability(map, p3);
					float shade = 0.0f;
					if ((shade_rule || conifer_rule || range > 0.0f) && suit > 0.0f) {
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
					if (litter_rule && suit > 0.0f) {
						float litter = crown_litter(local_crown_grid, local_crowns, p_setup.crown_max + p_setup.litter_reach, CROWN_CELL, xz, p_setup.litter_skew);
						if (p_setup.other) {
							litter = 1.0f - (1.0f - litter) * (1.0f - p_setup.other->get_litter_at(Vector3(xz.x, 0.0f, xz.y)));
						}
						suit *= sp->rule_weight(PlantSpecies::RULE_LITTER, litter);
					}
					if (sp->get_mask_channel() >= 0 && suit > 0.0f) {
						const float m = CLAMP(mask_at(xz, sp->get_mask_channel()), 0.0f, 1.0f);
						suit *= sp->is_mask_invert() ? 1.0f - m : m;
					}
					// Fallen wood (docs/ideas/forest-floor.md, 6.6): off its footprint, on its upper
					// surface or along its side, by the species' rule.
					bool on_log = false;
					DeadwoodHit hit;
					if (dw_rule != PlantSpecies::DEADWOOD_IGNORE && suit > 0.0f) {
						const float dw_reach = dw_rule == PlantSpecies::DEADWOOD_BESIDE ? dw_band : 0.0f;
						bool found = deadwood_nearest(local_deadwood_grid, local_deadwood, xz, dw_reach, DEADWOOD_CELL, hit);
						if (p_setup.other) {
							found = p_setup.other->_deadwood_at(xz, dw_reach, hit) || found;
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
							if (hit.side < 0.0f) {
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
					const float keep = sp->get_density() * suit;
					if (rank >= keep) {
						continue;
					}
					// Rocks: no stem inside one. A tree's trunk foot clears it, a log its whole
					// length, a ground plant its stem; leaves may lean over the rock.
					if (!p_setup.obstacles.is_empty()) {
						const float om = is_log ? ps.deadwood_reach : (canopy_r > 0.0f ? ps.stem_foot * MAX(scale_range.x, scale_range.y) : 0.0f) + OBSTACLE_MARGIN;
						if (p_setup.obstacles.blocks(xz, om)) {
							continue;
						}
					}
					// Stems: ground plants keep out of the trunks' feet (flare and root ridges), this
					// field's and the canopy field's; other woody species keep their footprints.
					if (canopy_r <= 0.0f && !is_log && (stem_in(local_crown_grid, local_crowns, xz, STEM_MARGIN, CROWN_CELL) || (p_setup.other && p_setup.other->_in_stem(xz, STEM_MARGIN)))) {
						continue;
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
								if (o.species == s || (o.deadwood && dw_near)) {
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
					placed.push_back({ xz, fp, s, is_log });
					grid[gc].push_back(placed.size() - 1);

					// The plant's transform: on the ground, tilted towards its normal, random yaw and size.
					const float r = pat.randoms[i];
					const float scale = Math::lerp(scale_range.x, scale_range.y, Math::fmod(r * 104729.0f, 1.0f));
					if (canopy_r > 0.0f) {
						const Crown crown = { xz, canopy_r * scale, sp->get_canopy_opacity(), ps.canopy_top * scale, sp->is_canopy_conifer() ? 1.0f : 0.0f, ps.stem_foot * scale };
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
					const float y = on_log ? hit.top + DEADWOOD_MOSS_LIFT / MAX(hit.normal.y, 0.5f) : map->get_height_at(p3) + (floats ? map->get_water_depth_at(p3) : 0.0f);
					const float slope = floats ? 0.0f : map->get_slope_at(p3);
					const float aspect = map->get_aspect_at(p3);
					const Vector3 normal = on_log ? hit.normal : Vector3(Math::sin(aspect) * Math::sin(slope), Math::cos(slope), Math::cos(aspect) * Math::sin(slope));
					const Vector3 up = Vector3(0, 1, 0).lerp(normal, align).normalized();
					Basis b;
					const Vector3 axis = Vector3(0, 1, 0).cross(up);
					if (axis.length_squared() > 1e-10f) {
						b = Basis(axis.normalized(), Math::acos(CLAMP(up.y, -1.0f, 1.0f)));
					}
					const float yaw = sp->is_random_yaw() ? Math::fmod(r * 7919.0f, 1.0f) * Math::TAU : 0.0f;
					b = b * Basis(Vector3(0, 1, 0), yaw);
					b.scale(Vector3(scale, scale, scale));
					const int variant = MIN(int(r * variants), variants - 1);
					const Transform3D world(b, Vector3(xz.x, y, xz.y));
					if (is_log && variant < int(ps.deadwood_axes.size()) && ps.deadwood_axes[variant].size() >= 7) {
						const PackedFloat32Array &ax = ps.deadwood_axes[variant];
						const Deadwood log = { world.xform(Vector3(ax[0], ax[1], ax[2])), world.xform(Vector3(ax[3], ax[4], ax[5])), ax[6] * scale };
						local_deadwood.push_back(log);
						deadwood_insert(local_deadwood_grid, log, local_deadwood.size() - 1, DEADWOOD_CELL);
						if (out) {
							out->deadwood.push_back(log);
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
					out->species[s].customs[variant].push_back(Color(CLAMP(rank / MAX(keep, 1e-4f), 0.0f, 1.0f), range_light, scale, float(s)));
					if (ps.far_color.a > 0.0f && range > 0.0f) {
						// The ground a plant hides: a disc of half its spacing, scaled.
						const float rr = 0.5f * sp->get_spacing() * scale;
						out->far.push_back({ xz, ps.far_color, float(ps.far_color.a * Math::PI * rr * rr), range });
					}
					out->species[s].positions.push_back(world.origin);
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
void VegetationField3D::_lazy_near_update(const Vector3 &p_camera) {
	for (KeyValue<ObjectID, LazyNear> &kv : lazy_near) {
		LazyNear &c = kv.value;
		const float d = c.centre.distance_to(p_camera);
		if (c.near) {
			if (d > c.range * 1.5f) {
				_lazy_near_free(c);
			}
			continue;
		}
		if (d > c.range * 1.2f) {
			continue;
		}
		Object *src = ObjectDB::get_instance(c.source);
		if (!src) {
			continue;
		}
		const Ref<Mesh> mesh = src->call(SNAME("request_near_mesh"), c.key, c.variant);
		if (mesh.is_null()) {
			continue; // Still building.
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(mesh);
		_fill_cell(mm, mesh, c.xforms, c.customs);
		const float margin = impostors_traced ? 0.0f : MAX(c.range * 0.1f, 1.0f);
		c.near = memnew(MultiMeshInstance3D);
		c.near->set_name(String(c.far->get_name()) + "_Near");
		c.near->set_multimesh(mm);
		c.near->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
		c.near->set_visibility_range_end(c.range);
		c.near->set_visibility_range_end_margin(margin);
		add_child(c.near, false, INTERNAL_MODE_BACK);
		c.far->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
		c.far->set_visibility_range_begin(c.range);
		c.far->set_visibility_range_begin_margin(margin);
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
void VegetationField3D::_commit_species(const PlaceSetup &p_setup, const TileOut &p_tile, int p_species, LocalVector<MultiMeshInstance3D *> &r_nodes) {
	const PlaceSpecies &ps = p_setup.species[p_species];
	if (ps.sp.is_null() || ps.meshes.is_empty()) {
		return;
	}
	const TileOut::Species &so = p_tile.species[p_species];
	const float range = ps.sp->get_visibility_range();
	const float near_range = ps.sp->get_near_range();
	// Cells for the near swap: a quarter of the swap distance, at least 4 m.
	const float near_cell = MAX(near_range * 0.25f, 4.0f);
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
		const int cells = near_mesh.is_valid() || near_source ? CLAMP(int(Math::ceil(ts / near_cell)), 1, 16) : 1;
		const Vector2 tile_origin = Vector2(tx * ts, tz * ts);
		LocalVector<LocalVector<uint32_t>> buckets;
		buckets.resize(cells * cells);
		for (uint32_t i = 0; i < so.xforms[v].size(); i++) {
			const Vector3 o = p_setup.global_xf.xform(so.xforms[v][i].origin);
			const int cx = CLAMP(int((o.x - tile_origin.x) / ts * cells), 0, cells - 1);
			const int cz = CLAMP(int((o.z - tile_origin.y) / ts * cells), 0, cells - 1);
			buckets[cz * cells + cx].push_back(i);
		}
		for (int b = 0; b < cells * cells; b++) {
			if (buckets[b].is_empty()) {
				continue;
			}
			for (int pass = 0; pass < (near_mesh.is_valid() ? 2 : 1); pass++) {
				const bool near_pass = pass == 1;
				Ref<MultiMesh> mm;
				mm.instantiate();
				mm->set_transform_format(MultiMesh::TRANSFORM_3D);
				mm->set_use_custom_data(true);
				mm->set_mesh(near_pass ? near_mesh : mesh);
				const Ref<Mesh> draw_mesh = near_pass ? near_mesh : mesh;
				LocalVector<Transform3D> bx;
				LocalVector<Color> bc;
				for (uint32_t i : buckets[b]) {
					bx.push_back(so.xforms[v][i]);
					bc.push_back(so.customs[v][i]);
				}
				_fill_cell(mm, draw_mesh, bx, bc);
				MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
				mmi->set_name(cells > 1 ? vformat("Plants%s_%d_%d_%d_%d_%d", near_pass ? "Near" : "", s, v, tx, tz, b) : vformat("Plants_%d_%d_%d_%d", s, v, tx, tz));
				mmi->set_multimesh(mm);
				if (near_mesh.is_valid()) {
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
					mmi->set_visibility_range_end(range * 1.05f);
				}
				add_child(mmi, false, INTERNAL_MODE_BACK);
				r_nodes.push_back(mmi);
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
			Vector<uint8_t> data;
			data.resize(w * h * 2);
			uint8_t *d = data.ptrw();
			for (int i = 0; i < w * h; i++) {
				d[i * 2 + 0] = uint8_t(CLAMP(stand[i], 0.0f, 1.0f) * 255.0f + 0.5f);
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
			if (ps.sp.is_null() || ps.far_color.a <= 0.0f || (ps.sp->get_canopy_radius() > 0.0f && !ps.sp->uses_shade() && !ps.sp->uses_conifer())) {
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
			plants /= c_sum;
		}
		// The grass under them, layer by layer from the top: a tall sward in patches over an
		// undercoat that fills the gaps between them reads as the undercoat there, not bare soil.
		float bare = 1.0f;
		Vector3 grass;
		for (uint32_t l = 0; l < job.grass.size(); l++) {
			float gl = 0.0f;
			for (const Vector3 &q : subs) {
				gl += CLAMP(job.grass[l]->get_suitability(map, q), 0.0f, 1.0f) * 0.25f;
			}
			if (job.grass[l]->uses_shade()) {
				gl *= job.grass[l]->rule_weight(PlantSpecies::RULE_SHADE, closure);
			}
			const Color &fc = job.grass_far[l];
			grass += Vector3(fc.r, fc.g, fc.b) * (gl * bare);
			bare *= 1.0f - gl;
		}
		const float g = 1.0f - bare;
		if (g > 0.0f) {
			grass /= g;
		}
		const float ground_a = 1.0f - (1.0f - g) * (1.0f - a_plants);
		Vector3 ground = ground_a > 0.0f ? (grass * (g * (1.0f - a_plants)) + plants * a_plants) / ground_a : Vector3();
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
	_build_cover(setup);
	const Rect2 rect = setup.rect;
	const Rect2i tiles(Vector2i(int(Math::floor(rect.position.x / tile_size)), int(Math::floor(rect.position.y / tile_size))),
			Vector2i(int(Math::ceil((rect.position.x + rect.size.x) / tile_size)) - int(Math::floor(rect.position.x / tile_size)),
					int(Math::ceil((rect.position.y + rect.size.y) / tile_size)) - int(Math::floor(rect.position.y / tile_size))));
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
	set_process_internal(!impostor_cells.is_empty() || !lazy_near.is_empty() || has_near_swap);
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
void VegetationField3D::set_obstacles(const PackedVector4Array &p_obstacles) {
	obstacles = p_obstacles;
	obstacle_grid.build(obstacles);
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
	return albedo * (1.0 - f.a * gone) + f.rgb * f.rgb * gone;
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

float VegetationField3D::get_shade_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock); // Another field may read this one's crowns from its workers.
	return _shade_at(Vector2(p_global_position.x, p_global_position.z));
}

float VegetationField3D::get_conifer_at(const Vector3 &p_global_position) const {
	RWLockRead guard(crown_lock);
	float conifer = 0.0f;
	_shade_at(Vector2(p_global_position.x, p_global_position.z), &conifer);
	return conifer;
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

// Streaming (docs/ideas/vegetation-lod-streaming.md, "Built (2)"): tiles around a centre are placed
// on WorkerThreadPool, nearest first, each over its halo so it comes out exactly as in populate().
// The main thread adds a placed tile's species to the scene within stream_budget_msec a frame,
// only out to each species' visibility range, and frees tiles once they are past the radius.

void VegetationField3D::_stream_job(void *p_job) {
	StreamJob *job = static_cast<StreamJob *>(p_job);
	job->field->_place(job->field->stream_setup, Rect2i(job->tile, Vector2i(1, 1)), true, job->out);
	job->done.set();
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
		_build_cover(stream_setup);
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const uint64_t budget = uint64_t(stream_budget_msec * 1000.0f);
	if (stream_maps_job && stream_maps_job->done.is_set()) {
		_stream_maps_finish();
	}
	const Vector3 c3 = _stream_centre();
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
		if (_tile_distance(job->tile, ts, centre) <= stream_radius + ts * 1.5f && !stream_tiles.has(job->tile)) {
			StreamTile *t = memnew(StreamTile);
			t->out = job->out[0];
			t->nodes.resize(species_count);
			t->committed.resize(species_count);
			for (int s = 0; s < species_count; s++) {
				t->committed[s] = false;
			}
			stream_tiles.insert(job->tile, t);
			plant_count += t->out.count;
			stream_maps_dirty = true;
		}
		memdelete(job);
		stream_jobs.remove_at_unordered(i);
	}

	// Tiles past the radius go; species past their range leave the scene, those within it join,
	// nearest first, while the frame's budget lasts.
	LocalVector<Vector2i> gone;
	struct Pending {
		float d;
		Vector2i tile;
		bool operator<(const Pending &p_o) const { return d < p_o.d; }
	};
	LocalVector<Pending> pending;
	for (KeyValue<Vector2i, StreamTile *> &kv : stream_tiles) {
		const float d = _tile_distance(kv.key, ts, centre);
		if (d > stream_radius + ts * 1.5f) {
			gone.push_back(kv.key);
			continue;
		}
		for (int s = 0; s < species_count; s++) {
			const PlaceSpecies &ps = stream_setup.species[s];
			if (ps.sp.is_null()) {
				continue;
			}
			const float range = ps.sp->get_visibility_range();
			const bool wanted = range <= 0.0f || d <= range * 1.05f + ts * 0.5f;
			const bool keep = range <= 0.0f || d <= range * 1.05f + ts * 1.5f;
			if (kv.value->committed[s] && !keep) {
				_stream_free_species(kv.value, s);
			} else if (!kv.value->committed[s] && wanted) {
				pending.push_back({ d, kv.key });
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
	}
	pending.sort();
	for (const Pending &p : pending) {
		StreamTile *t = stream_tiles[p.tile];
		for (int s = 0; s < species_count; s++) {
			if (t->committed[s]) {
				continue;
			}
			const float range = stream_setup.species[s].sp.is_valid() ? stream_setup.species[s].sp->get_visibility_range() : 0.0f;
			if (stream_setup.species[s].sp.is_null() || (range > 0.0f && p.d > range * 1.05f + ts * 0.5f)) {
				continue;
			}
			_commit_species(stream_setup, t->out, s, t->nodes[s]);
			t->committed[s] = true;
		}
		if (OS::get_singleton()->get_ticks_usec() - t0 > budget) {
			busy = true;
			break;
		}
	}

	// Missing tiles within the radius, nearest first, as many at once as there are workers.
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count());
	if (int(stream_jobs.size()) < max_jobs) {
		const int r = int(Math::ceil(stream_radius / ts)) + 1;
		const Vector2i ct(int(Math::floor(centre.x / ts)), int(Math::floor(centre.y / ts)));
		const Rect2i map_tiles(Vector2i(int(Math::floor(rect.position.x / ts)), int(Math::floor(rect.position.y / ts))),
				Vector2i(int(Math::ceil((rect.position.x + rect.size.x) / ts)) - int(Math::floor(rect.position.x / ts)),
						int(Math::ceil((rect.position.y + rect.size.y) / ts)) - int(Math::floor(rect.position.y / ts))));
		LocalVector<Pending> missing;
		for (int dz = -r; dz <= r; dz++) {
			for (int dx = -r; dx <= r; dx++) {
				const Vector2i tile = ct + Vector2i(dx, dz);
				if (!map_tiles.has_point(tile) || stream_tiles.has(tile)) {
					continue;
				}
				const float d = _tile_distance(tile, ts, centre);
				if (d > stream_radius) {
					continue;
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
			job->task = WorkerThreadPool::get_singleton()->add_native_task(&VegetationField3D::_stream_job, job, false, "VegetationField3D tile");
			stream_jobs.push_back(job);
		}
	}
	busy = busy || !stream_jobs.is_empty();

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
		set_process_internal(true);
	} else if (populate_on_ready) {
		queue_populate();
	}
}

void VegetationField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
			if (map) {
				// Repopulate whenever the map is baked again (terrain edits).
				map->connect(SNAME("baked"), callable_mp(this, &VegetationField3D::_on_map_baked));
				if (map->is_baked() && populate_on_ready && !streaming) {
					queue_populate();
				}
			}
			if (streaming) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_PREDELETE: {
			_clear_generated();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// A few times a second: has path tracing been switched on or off?
			impostor_check -= get_process_delta_time();
			if (impostor_check <= 0.0) {
				impostor_check = 0.25;
				const bool traced = _path_traced();
				if (traced != impostors_traced) {
					_update_impostors(traced);
				}
			}
			if (streaming) {
				_stream_update(get_process_delta_time());
			}
			if (!lazy_near.is_empty()) {
				_lazy_near_update(_stream_centre());
			}
		} break;
	}
}

void VegetationField3D::set_biome(const Ref<Biome> &p_biome) {
	biome = p_biome;
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
	ClassDB::bind_method(D_METHOD("set_biome", "biome"), &VegetationField3D::set_biome);
	ClassDB::bind_method(D_METHOD("get_biome"), &VegetationField3D::get_biome);
	ClassDB::bind_method(D_METHOD("set_data_map", "path"), &VegetationField3D::set_data_map);
	ClassDB::bind_method(D_METHOD("get_data_map"), &VegetationField3D::get_data_map);
	ClassDB::bind_method(D_METHOD("set_density_mask", "mask"), &VegetationField3D::set_density_mask);
	ClassDB::bind_method(D_METHOD("get_density_mask"), &VegetationField3D::get_density_mask);
	ClassDB::bind_method(D_METHOD("set_density_mask_rect", "rect"), &VegetationField3D::set_density_mask_rect);
	ClassDB::bind_method(D_METHOD("get_density_mask_rect"), &VegetationField3D::get_density_mask_rect);
	ClassDB::bind_method(D_METHOD("set_obstacles", "obstacles"), &VegetationField3D::set_obstacles);
	ClassDB::bind_method(D_METHOD("get_obstacles"), &VegetationField3D::get_obstacles);
	ClassDB::bind_method(D_METHOD("set_canopy_field", "path"), &VegetationField3D::set_canopy_field);
	ClassDB::bind_method(D_METHOD("get_canopy_field"), &VegetationField3D::get_canopy_field);
	ClassDB::bind_method(D_METHOD("set_litter_wind", "wind"), &VegetationField3D::set_litter_wind);
	ClassDB::bind_method(D_METHOD("get_litter_wind"), &VegetationField3D::get_litter_wind);
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
	ClassDB::bind_method(D_METHOD("get_deadwood_count"), &VegetationField3D::get_deadwood_count);
	ClassDB::bind_method(D_METHOD("get_deadwood_at", "global_position", "reach"), &VegetationField3D::get_deadwood_at, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_sward_shade_at", "global_position"), &VegetationField3D::get_sward_shade_at);
	ClassDB::bind_method(D_METHOD("get_sward_litter_at", "global_position"), &VegetationField3D::get_sward_litter_at);
	ClassDB::bind_method(D_METHOD("get_shade_at", "global_position"), &VegetationField3D::get_shade_at);
	ClassDB::bind_method(D_METHOD("get_stand_shade_at", "global_position"), &VegetationField3D::get_stand_shade_at);
	ClassDB::bind_static_method("VegetationField3D", D_METHOD("get_floor_shader_code"), &VegetationField3D::get_floor_shader_code);
	ClassDB::bind_method(D_METHOD("set_forest_floor_enabled", "enabled"), &VegetationField3D::set_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("is_forest_floor_enabled"), &VegetationField3D::is_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("get_conifer_at", "global_position"), &VegetationField3D::get_conifer_at);
	ClassDB::bind_method(D_METHOD("get_litter_at", "global_position"), &VegetationField3D::get_litter_at);
	ClassDB::bind_method(D_METHOD("set_cover_map_enabled", "enable"), &VegetationField3D::set_cover_map_enabled);
	ClassDB::bind_method(D_METHOD("is_cover_map_enabled"), &VegetationField3D::is_cover_map_enabled);
	ClassDB::bind_method(D_METHOD("set_cover_grass", "path"), &VegetationField3D::set_cover_grass);
	ClassDB::bind_method(D_METHOD("get_cover_grass"), &VegetationField3D::get_cover_grass);
	ClassDB::bind_method(D_METHOD("set_cover_grass_layers", "paths"), &VegetationField3D::set_cover_grass_layers);
	ClassDB::bind_method(D_METHOD("get_cover_grass_layers"), &VegetationField3D::get_cover_grass_layers);
	ClassDB::bind_method(D_METHOD("set_cover_cell", "cell"), &VegetationField3D::set_cover_cell);
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
	ClassDB::bind_method(D_METHOD("get_stream_stats"), &VegetationField3D::get_stream_stats);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "biome", PROPERTY_HINT_RESOURCE_TYPE, "Biome"), "set_biome", "get_biome");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "litter_wind"), "set_litter_wind", "get_litter_wind");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "canopy_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_canopy_field", "get_canopy_field");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forest_floor_enabled"), "set_forest_floor_enabled", "is_forest_floor_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "density_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_density_mask", "get_density_mask");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "density_mask_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_density_mask_rect", "get_density_mask_rect");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "obstacles"), "set_obstacles", "get_obstacles");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "1,256,1,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "populate_on_ready"), "set_populate_on_ready", "is_populate_on_ready");

	ADD_GROUP("Cover map", "cover_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cover_map_enabled"), "set_cover_map_enabled", "is_cover_map_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "cover_grass", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GrassField3D"), "set_cover_grass", "get_cover_grass");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "cover_grass_layers", PROPERTY_HINT_ARRAY_TYPE, vformat("%s/%s:%s", Variant::NODE_PATH, PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GrassField3D")), "set_cover_grass_layers", "get_cover_grass_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cover_cell", PROPERTY_HINT_RANGE, "0.5,64,0.5,suffix:m"), "set_cover_cell", "get_cover_cell");
	ADD_GROUP("Streaming", "stream");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming", "is_streaming");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "stream_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_stream_target", "get_stream_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_radius", PROPERTY_HINT_RANGE, "16,4096,1,or_greater,suffix:m"), "set_stream_radius", "get_stream_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_budget_msec", PROPERTY_HINT_RANGE, "0.1,16,0.1,suffix:ms"), "set_stream_budget_msec", "get_stream_budget_msec");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_maps_radius", PROPERTY_HINT_RANGE, "8,1024,1,or_greater,suffix:m"), "set_stream_maps_radius", "get_stream_maps_radius");

	ADD_SIGNAL(MethodInfo("populated"));
}

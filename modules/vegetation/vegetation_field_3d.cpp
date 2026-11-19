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
#include "vegetation_detail.h"
#include "vegetation_light.h"

// Rigged trees (TreeWind3D, from the trees work) when that class is in the module.
#include "tree_bark_detail.h"
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

// Where streaming fields' placed plants end (vegetation_near_radius): past it only the ground
// shows them, through the terrain's far blend of the cover map, which a TerrainMaterial3D with
// far_start_from_vegetation starts before it. With several streaming fields (a forest out to
// 150 m, the floor and meadow plants to 80 m) the nearest end counts, so the blend has taken over
// before the first plants run out. A field takes its radius back (0) when it stops.
float VegetationField3D::near_radius_published = 0.0f;

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
			wanted = sp.is_valid() && (sp->uses_burn_age() || sp->get_burn_recovery() > 0.0f);
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

bool VegetationField3D::_make_setup(PlaceSetup &r_setup) {
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
	r_setup.paths = path_grid;
	r_setup.crop_fields = GrassField3D::get_crop_field_mask();
	_follow_holes(map);
	r_setup.holes.read(VegetationHoles::source_of(map));
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
		PlaceSpecies &ps = r_setup.species[s];
		ps.sp = species_list[s];
		if (ps.sp.is_null()) {
			continue;
		}
		ps.range = _species_range(ps.sp, pixel);
		if (streaming && far_shell && ps.range > 0.0f && !ps.sp->get_meshes().is_empty()) {
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
		if (ps.sp.is_valid() && bool(ps.sp->get_meta("nebkha", false))) {
			ps.nebkha_slot = r_setup.nebkha_count++;
		} else if (ps.sp.is_valid() && bool(ps.sp->get_meta("desert", false)) && bool(ps.sp->get_meta("woody_shrub", false)) && !bool(ps.sp->get_meta("oasis", false))) {
			ps.desert_shrub = true;
		}
	}
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
		const bool dune_rule = sp->uses_dune() && p_setup.dunes;
		const bool coast_rule = sp->uses_coast();
		const bool field_rule = sp->uses_field_edge();
		const bool mobility_rule = sp->uses_sand_mobility();
		const bool burn_rule = sp->uses_burn_age();
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
		const float max_keep = sp->get_density() * (p_setup.obstacles.is_empty() ? 1.0f : sp->rock_foot_weight(0.0f));
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
				const uint32_t point_count = nebkha_sp ? mound_xz.size() : pat.points.size();
				for (uint32_t i = 0; i < point_count; i++) {
					Vector2 xz = nebkha_sp ? mound_xz[i] : Vector2(tx * ts, tz * ts) + pat.points[i];
					if (!rect.has_point(xz) || (!in_core && !reach_rect.has_point(xz))) {
						continue;
					}
					// Ordered dither: a point survives where its (per-tile shifted) rank is below
					// the density there, so density changes smoothly and the pattern stays blue noise.
					const float rank = nebkha_sp ? 0.0f : Math::fmod(pat.ranks[i] + shift, 1.0f);
					if (rank >= max_keep) {
						continue;
					}
					if (ps.desert_shrub && p_setup.near_nebkha(xz)) {
						continue; // A mound's own shrub crowns it; the others keep off its flanks.
					}
					Vector3 p3(xz.x, 0.0f, xz.y);
					float suit = sp->get_suitability(map, p3, false);
					// Patches (clonal stands), except that a rock's foot is a patch of its own
					// for species that gather there: the patch weight is lifted towards 1 there.
					float rock_boost = 1.0f;
					if (suit > 0.0f && sp->get_patch_scale() > 0.0f) {
						float pw = sp->get_patch_weight(p3);
						if (pw < 1.0f && sp->get_rock_foot() > 0.0f && !p_setup.obstacles.is_empty()) {
							const float reach = 4.0f * PlantSpecies::ROCK_FOOT_REACH;
							const float c = p_setup.obstacles.clearance(xz, reach);
							if (c < reach) {
								const float fall = Math::exp(-MAX(c, 0.0f) / PlantSpecies::ROCK_FOOT_REACH);
								pw = MAX(pw, 1.0f - Math::exp(-sp->get_rock_foot() * fall));
								rock_boost = sp->rock_foot_weight(c);
							}
						}
						suit *= pw;
					} else if (suit > 0.0f && sp->get_rock_foot() > 0.0f && !p_setup.obstacles.is_empty()) {
						const float reach = 4.0f * PlantSpecies::ROCK_FOOT_REACH;
						const float c = p_setup.obstacles.clearance(xz, reach);
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
						suit *= sp->rule_weight(PlantSpecies::RULE_LITTER, litter);
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
						const float second = p_setup.obstacles.is_empty() ? 1e6f : p_setup.obstacles.second_clearance(xz, gap_reach, &first);
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
					if (!p_setup.obstacles.is_empty() && !on_top) {
						const float om = is_log ? ps.deadwood_reach : (canopy_r > 0.0f ? ps.stem_foot * MAX(scale_range.x, scale_range.y) : 0.0f) + OBSTACLE_MARGIN;
						if (p_setup.obstacles.blocks(xz, om)) {
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
					const float r = nebkha_sp ? float(hash_murmur3_one_float(xz.x, hash_murmur3_one_float(xz.y)) & 0xFFFFFF) / float(0x1000000) : pat.randoms[i];
					float scale = Math::lerp(scale_range.x, scale_range.y, Math::fmod(r * 104729.0f, 1.0f)) * trodden;
					if (nebkha_sp && nebkha_unit > 0.0f) {
						// The shrub that built the mound: its crown about NEBKHA_CROWN of the mound's radius.
						scale = NEBKHA_CROWN * NEBKHA_RADIUS * mound_h[i] / nebkha_unit * Math::lerp(0.9f, 1.1f, r);
					}
					// At a rock's foot: leaves must not pass into the rock. A plant reaching in over it
					// grows smaller and leans away, as plants pressed against a boulder do.
					float rock_fit = 1.0f;
					Vector2 rock_away;
					if (canopy_r <= 0.0f && !is_log && !on_log && !on_top && ps.spread > 0.0f && !p_setup.obstacles.is_empty()) {
						const float reach = ps.spread * scale;
						const float c = p_setup.obstacles.clearance(xz, reach, &rock_away);
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
						crown.y = map->get_height_at(p3);
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
					float y = on_log ? hit.top + DEADWOOD_MOSS_LIFT / MAX(hit.normal.y, 0.5f) : map->get_height_at(p3) + (floats ? map->get_water_depth_at(p3) : 0.0f);
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
					if (wood_out != Vector2()) {
						yaw = Math::atan2(-wood_out.y, wood_out.x);
					} else if (bank_variant >= 0 && ps.bank_slopes[bank_variant] > 1e-3f && slope > 1e-3f) {
						// Its ground falls towards mesh +Z: turn +Z downhill.
						yaw = Math::atan2(downhill.x, downhill.y);
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
					const int variant = fire_variant >= 0 ? fire_variant : (bank_variant >= 0 ? bank_variant : MIN(int(r * variants), variants - 1));
					const Transform3D world(b, Vector3(xz.x, y, xz.y));
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
		LocalVector<LocalVector<uint32_t>> buckets;
		buckets.resize(cells * cells);
		for (uint32_t i = 0; i < so.xforms[v].size(); i++) {
			if (p_max_rank < 1.0f && Math::fmod(so.customs[v][i].r, 1.0f) >= p_max_rank) {
				continue; // Thinned out at the tile's distance (the far tail).
			}
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
				if (detail) {
					VegetationDetail::trace_instanced(mm);
				}
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
					// Godot measures to the cell's centre: a plant can be up to its half diagonal nearer.
					mmi->set_visibility_range_end(reach * 1.05f + 0.75f * ts / cells);
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
		if (ps.sp.is_valid() && ps.range > 0.0f) {
			reach = MAX(reach, MAX(ps.reach, ps.range));
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
	};
	LocalVector<Kind> kinds;
	for (const PlaceSpecies &ps : su.species) {
		if (ps.sp.is_null() || ps.range <= 0.0f || ps.far_color.a <= 0.0f || ps.meshes.is_empty() || ps.meshes[0].is_null() || ps.nebkha_slot >= 0) {
			continue;
		}
		const PlantSpecies *sp = ps.sp.ptr();
		if (sp->get_rule_range(PlantSpecies::RULE_SHADE).x > 0.0f || sp->uses_dune() || sp->uses_coast() || sp->uses_spring() || sp->uses_pool() || ps.deadwood_reach > 0.0f) {
			continue;
		}
		// A species whose own far form takes over past its plants (meta "far_shell" false: the trees module's crown
		// hulls and skyline trees) stays out of the shell.
		if (!bool(sp->get_meta(SNAME("far_shell"), true))) {
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
		kinds.push_back({ sp, h, tone, cover });
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
			for (const Kind &k : kinds) {
				const float c = k.cover * CLAMP(k.sp->get_suitability(map, at), 0.0f, 1.0f);
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
			// grow past a quarter of its size (prefiltered, as a mip), leaving the even mass by cover.
			float dev = 0.0f, act = 0.0f;
			const float scales[3] = { 7.0f, 19.0f, 53.0f };
			const float amps[3] = { 0.5f, 0.3f, 0.2f };
			for (int o = 0; o < (far_shell_clumps ? 3 : 0); o++) {
				const float a = 1.0f - Math::smoothstep(0.25f, 0.5f, cell / scales[o]);
				if (a > 0.0f) {
					dev += a * amps[o] * (far_shell_noise(q.x, q.y, scales[o], 0x68E31DA4u + uint32_t(o) * 0x9E3779B9u) - 0.5f) * 2.0f;
					act += a * amps[o];
				}
			}
			float mass = cover;
			if (act > 0.0f) {
				// The value noise's spread is about a third of its range: its threshold for this cover.
				const float open = Math::smoothstep(-0.12f, 0.12f, dev / act * 0.35f + (cover - 0.5f) * 0.6f);
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
				// A sward or scrub seen from afar is its lit tops over the shade between them (darker than the leaves'
				// own colour, the more so where it is thin), mottled at the clumps' scale and in larger patches.
				const float mottle = (0.8f + 0.4f * far_shell_noise(corner[k[c]].x, corner[k[c]].z, 3.0f, 0x2545F491u)) * (0.75f + 0.5f * far_shell_noise(corner[k[c]].x, corner[k[c]].z, 23.0f, 0x9E3779B9u));
				// The low corners round a clump sit in its shade (ambient occlusion at the clumps' foot), its tops in light.
				const float rel = hc_top > 0.0f ? CLAMP(hc / hc_top, 0.0f, 1.0f) : 1.0f;
				const float shade = (0.55f + 0.2f * CLAMP(cc[k[c]].a, 0.0f, 1.0f)) * mottle * (0.8f + 0.2f * rel);
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
	// The plants' share carries the far match's darkening (vegetation_far_shade), as vegetation_far()
	// gives it to the same plants nearer: the grass and crowns have their own far colours.
	job.plant_shade = far_shade;
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
		const float rec[7] = { c.xz.x, c.xz.y, c.radius, c.top, c.base, c.conifer, c.opacity };
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
}

void VegetationField3D::set_paths(const Array &p_paths) {
	paths = p_paths;
	path_grid.build(paths);
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
		StreamTile **existing = stream_tiles.getptr(job->tile);
		StreamTile *old_tile = (existing && (*existing)->stale) ? *existing : nullptr;
		if (_tile_distance(job->tile, ts, centre) <= stream_radius + ts * 1.5f && (!existing || old_tile)) {
			StreamTile *t = memnew(StreamTile);
			t->out = job->out[0];
			t->nodes.resize(species_count);
			t->committed.resize(species_count);
			t->committed_keep.resize(species_count);
			for (int s = 0; s < species_count; s++) {
				t->committed[s] = false;
				t->committed_keep[s] = 1.0f;
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
				if (rank <= t->committed_keep[s] * 1.3f || t->committed_keep[s] >= 1.0f) {
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
		}
		if (OS::get_singleton()->get_ticks_usec() - t0 > budget) {
			busy = true;
			break;
		}
	}

	// Missing tiles within the radius, nearest first, as many at once as there are workers.
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count());
	if (int(stream_jobs.size()) < max_jobs && !holes_dirty.has_area() && !burn_dirty.has_area()) {
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
	ClassDB::bind_method(D_METHOD("set_forest_floor_enabled", "enabled"), &VegetationField3D::set_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("is_forest_floor_enabled"), &VegetationField3D::is_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("get_conifer_at", "global_position"), &VegetationField3D::get_conifer_at);
	ClassDB::bind_method(D_METHOD("get_litter_at", "global_position"), &VegetationField3D::get_litter_at);
	ClassDB::bind_method(D_METHOD("get_spring_wetness_at", "global_position"), &VegetationField3D::get_spring_wetness_at);
	ClassDB::bind_method(D_METHOD("get_springs"), &VegetationField3D::get_springs);
	ClassDB::bind_method(D_METHOD("add_canopy_loss", "xz", "radius", "keep"), &VegetationField3D::add_canopy_loss, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_canopy_losses"), &VegetationField3D::get_canopy_losses);
	ClassDB::bind_method(D_METHOD("clear_canopy_losses"), &VegetationField3D::clear_canopy_losses);
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

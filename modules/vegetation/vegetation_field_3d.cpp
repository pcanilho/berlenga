/**************************************************************************/
/*  vegetation_field_3d.cpp                                               */
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

#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/weather/weather_map_3d.h"
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
	crowns.clear();
	crown_grid.clear();
	max_crown_radius = 0.0f;
	for (MultiMeshInstance3D *mmi : generated) {
		if (mmi->get_parent()) {
			mmi->get_parent()->remove_child(mmi);
		}
		memdelete(mmi);
	}
	generated.clear();
	placements.clear();
	plant_count = 0;
}

void VegetationField3D::clear() {
	_clear_generated();
}

Error VegetationField3D::populate() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_clear_generated();
	ERR_FAIL_COND_V_MSG(biome.is_null(), ERR_UNCONFIGURED, "VegetationField3D: no biome.");
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	ERR_FAIL_COND_V_MSG(!map || !map->is_baked(), ERR_UNCONFIGURED, "VegetationField3D: data_map must point to a baked TerrainDataMap3D.");

	const TypedArray<PlantSpecies> species_list = biome->get_species();
	const int species_count = species_list.size();
	placements.resize(species_count);
	float max_footprint = 0.0f;
	for (int s = 0; s < species_count; s++) {
		Ref<PlantSpecies> sp = species_list[s];
		if (sp.is_valid()) {
			max_footprint = MAX(max_footprint, sp->get_footprint());
		}
	}
	crowns.clear();
	crown_grid.clear();
	max_crown_radius = 0.0f;
	// Footprints of placed plants, in a hash grid.
	const float grid_cell = MAX(1.0f, max_footprint * 2.0f);
	HashMap<Vector2i, LocalVector<int>> grid;
	LocalVector<Placed> placed;

	const Rect2 rect = map->get_map_rect();
	// The density mask, read once on the CPU (bilinear, clamped to its rect; outside it a species
	// with a channel grows as if the mask were 1).
	Ref<Image> mask_img;
	if (density_mask.is_valid() && density_mask_rect.has_area()) {
		mask_img = density_mask->get_image();
		if (mask_img.is_valid() && mask_img->is_compressed()) {
			mask_img = mask_img->duplicate();
			mask_img->decompress();
		}
	}
	auto mask_at = [&](const Vector2 &p_xz, int p_channel) -> float {
		if (mask_img.is_null() || mask_img->is_empty() || !density_mask_rect.has_point(p_xz)) {
			return 1.0f;
		}
		const Vector2 uv = (p_xz - density_mask_rect.position) / density_mask_rect.size;
		const float fx = uv.x * mask_img->get_width() - 0.5f;
		const float fy = uv.y * mask_img->get_height() - 0.5f;
		const int x0 = CLAMP(int(Math::floor(fx)), 0, mask_img->get_width() - 1);
		const int y0 = CLAMP(int(Math::floor(fy)), 0, mask_img->get_height() - 1);
		const int x1 = MIN(x0 + 1, mask_img->get_width() - 1);
		const int y1 = MIN(y0 + 1, mask_img->get_height() - 1);
		const float ax = CLAMP(fx - Math::floor(fx), 0.0f, 1.0f);
		const float ay = CLAMP(fy - Math::floor(fy), 0.0f, 1.0f);
		const Color c = mask_img->get_pixel(x0, y0).lerp(mask_img->get_pixel(x1, y0), ax).lerp(mask_img->get_pixel(x0, y1).lerp(mask_img->get_pixel(x1, y1), ax), ay);
		return c[p_channel];
	};
	const int tx0 = int(Math::floor(rect.position.x / tile_size));
	const int tz0 = int(Math::floor(rect.position.y / tile_size));
	const int tx1 = int(Math::ceil((rect.position.x + rect.size.x) / tile_size));
	const int tz1 = int(Math::ceil((rect.position.y + rect.size.y) / tile_size));
	const Transform3D to_local = get_global_transform().affine_inverse();

	for (int s = 0; s < species_count; s++) {
		Ref<PlantSpecies> sp = species_list[s];
		if (sp.is_null()) {
			continue;
		}
		const TypedArray<Mesh> meshes = sp->get_meshes();
		const int variants = MAX(1, meshes.size());
		const Pattern &pat = _get_pattern(sp->get_spacing(), s);
		const float fp = sp->get_footprint();
		const Vector2 scale_range = sp->get_scale_range();
		const float align = sp->get_align_to_ground();
		const int reach = int(Math::ceil((fp + max_footprint) / grid_cell));
		const bool shade_rule = sp->uses_shade();
		const float canopy_r = sp->get_canopy_radius();
		float canopy_top = 0.0f;
		if (canopy_r > 0.0f && meshes.size() > 0) {
			const Ref<Mesh> m0 = meshes[0];
			if (m0.is_valid()) {
				canopy_top = m0->get_aabb().get_end().y;
			}
		}

		for (int tz = tz0; tz < tz1; tz++) {
			for (int tx = tx0; tx < tx1; tx++) {
				const float shift = float(tile_hash(tx, tz, seed, s) & 0xFFFFFF) / float(0x1000000);
				LocalVector<LocalVector<Transform3D>> xforms;
				LocalVector<LocalVector<Color>> customs;
				xforms.resize(variants);
				customs.resize(variants);
				for (uint32_t i = 0; i < pat.points.size(); i++) {
					const Vector2 xz = Vector2(tx * tile_size, tz * tile_size) + pat.points[i];
					if (!rect.has_point(xz)) {
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
					if (shade_rule && suit > 0.0f) {
						suit *= sp->rule_weight(PlantSpecies::RULE_SHADE, _shade_at(xz));
					}
					if (sp->get_mask_channel() >= 0 && suit > 0.0f) {
						const float m = CLAMP(mask_at(xz, sp->get_mask_channel()), 0.0f, 1.0f);
						suit *= sp->is_mask_invert() ? 1.0f - m : m;
					}
					if (rank >= sp->get_density() * suit) {
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
								if (o.species == s) {
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
					placed.push_back({ xz, fp, s });
					grid[gc].push_back(placed.size() - 1);

					// The plant's transform: on the ground, tilted towards its normal, random yaw and size.
					const float r = pat.randoms[i];
					const float y = map->get_height_at(p3);
					const float slope = map->get_slope_at(p3);
					const float aspect = map->get_aspect_at(p3);
					const Vector3 normal(Math::sin(aspect) * Math::sin(slope), Math::cos(slope), Math::cos(aspect) * Math::sin(slope));
					const Vector3 up = Vector3(0, 1, 0).lerp(normal, align).normalized();
					Basis b;
					const Vector3 axis = Vector3(0, 1, 0).cross(up);
					if (axis.length_squared() > 1e-10f) {
						b = Basis(axis.normalized(), Math::acos(CLAMP(up.y, -1.0f, 1.0f)));
					}
					const float yaw = sp->is_random_yaw() ? Math::fmod(r * 7919.0f, 1.0f) * Math::TAU : 0.0f;
					const float scale = Math::lerp(scale_range.x, scale_range.y, Math::fmod(r * 104729.0f, 1.0f));
					b = b * Basis(Vector3(0, 1, 0), yaw);
					b.scale(Vector3(scale, scale, scale));
					const int variant = MIN(int(r * variants), variants - 1);
					const Transform3D world(b, Vector3(xz.x, y, xz.y));
					if (canopy_r > 0.0f) {
						const float cr = canopy_r * scale;
						crowns.push_back({ xz, cr, sp->get_canopy_opacity(), canopy_top * scale });
						crown_grid[Vector2i(int(Math::floor(xz.x / CROWN_CELL)), int(Math::floor(xz.y / CROWN_CELL)))].push_back(crowns.size() - 1);
						max_crown_radius = MAX(max_crown_radius, cr);
					}
					xforms[variant].push_back(to_local * world);
					customs[variant].push_back(Color(float(variant), scale, r, float(s)));
					placements[s].push_back(world.origin);
					plant_count++;
				}
				for (int v = 0; v < variants; v++) {
					if (xforms[v].is_empty() || meshes.size() == 0) {
						continue;
					}
					Ref<Mesh> mesh = meshes[v];
					if (mesh.is_null()) {
						continue;
					}
					Ref<MultiMesh> mm;
					mm.instantiate();
					mm->set_transform_format(MultiMesh::TRANSFORM_3D);
					mm->set_use_custom_data(true);
					mm->set_mesh(mesh);
					mm->set_instance_count(xforms[v].size());
					for (uint32_t i = 0; i < xforms[v].size(); i++) {
						mm->set_instance_transform(i, xforms[v][i]);
						mm->set_instance_custom_data(i, customs[v][i]);
					}
					MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
					mmi->set_name(vformat("Plants_%d_%d_%d_%d", s, v, tx, tz));
					mmi->set_multimesh(mm);
					if (sp->get_visibility_range() > 0.0f) {
						// Ground cover too small to see far away: dropped by distance, faded out.
						mmi->set_visibility_range_end(sp->get_visibility_range());
						mmi->set_visibility_range_end_margin(sp->get_visibility_range() * 0.15f);
						mmi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
					}
					add_child(mmi, false, INTERNAL_MODE_BACK);
					generated.push_back(mmi);
				}
			}
		}
	}
	_publish_canopy(rect);
	populate_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("populated"));
	return OK;
}

// Canopy shade as the share of sky light the crowns stop. Each crown passes 1 - opacity of the
// light under its core (Beer-Lambert through the foliage), thinning to nothing a little past its
// rim, where diffuse sky light still reaches under the edge; overlapping crowns multiply their
// transmission. Plants then take it as a shade rule, the way field botany ranks them by light
// (Ellenberg's L values: bilberry, ferns and mosses in shade, meadow grasses in the open).
float VegetationField3D::_shade_at(const Vector2 &p_xz) const {
	if (crowns.is_empty()) {
		return 0.0f;
	}
	const float reach_m = max_crown_radius * 1.4f;
	const int r = int(Math::ceil(reach_m / CROWN_CELL));
	const Vector2i gc(int(Math::floor(p_xz.x / CROWN_CELL)), int(Math::floor(p_xz.y / CROWN_CELL)));
	float transmit = 1.0f;
	for (int dz = -r; dz <= r; dz++) {
		for (int dx = -r; dx <= r; dx++) {
			const LocalVector<int> *bucket = crown_grid.getptr(gc + Vector2i(dx, dz));
			if (!bucket) {
				continue;
			}
			for (int idx : *bucket) {
				const Crown &c = crowns[idx];
				const float d = c.xz.distance_to(p_xz) / c.radius;
				if (d >= 1.4f) {
					continue;
				}
				transmit *= 1.0f - c.opacity * (1.0f - Math::smoothstep(0.6f, 1.4f, d));
			}
		}
	}
	return 1.0f - transmit;
}

// The placed crowns as WeatherMap3D's canopy_map (height in red, leaf area / 4 in green), so snow
// interception, wind in the stand and anything else reading get_canopy_closure_at() sees the same
// forest. A crown's opacity is Beer's law with the weather map's extinction of 0.5,
// opacity = 1 - exp(-0.5 LAI), so LAI = -2 ln(1 - opacity) and overlapping crowns add their leaf
// area exactly as get_shade_at() multiplies their transmission: closure there equals this shade.
void VegetationField3D::_publish_canopy(const Rect2 &p_rect) {
	if (weather_map.is_empty() || !is_inside_tree()) {
		return;
	}
	WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
	ERR_FAIL_NULL_MSG(wm, "VegetationField3D: weather_map must point to a WeatherMap3D.");
	if (crowns.is_empty()) {
		wm->set_canopy_map(Ref<Texture2D>());
		return;
	}
	const int w = CLAMP(int(Math::ceil(p_rect.size.x)), 1, 1024); // About 1 m cells.
	const int h = CLAMP(int(Math::ceil(p_rect.size.y)), 1, 1024);
	Ref<Image> img = Image::create_empty(w, h, false, Image::FORMAT_RGF);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const Vector2 p = p_rect.position + Vector2((x + 0.5f) * p_rect.size.x / w, (y + 0.5f) * p_rect.size.y / h);
			const Vector2i gc(int(Math::floor(p.x / CROWN_CELL)), int(Math::floor(p.y / CROWN_CELL)));
			const int r = int(Math::ceil(max_crown_radius * 1.4f / CROWN_CELL));
			float lai = 0.0f;
			float top = 0.0f;
			float best = 0.0f;
			for (int dz = -r; dz <= r; dz++) {
				for (int dx = -r; dx <= r; dx++) {
					const LocalVector<int> *bucket = crown_grid.getptr(gc + Vector2i(dx, dz));
					if (!bucket) {
						continue;
					}
					for (int idx : *bucket) {
						const Crown &c = crowns[idx];
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
			img->set_pixel(x, y, Color(lai > 0.0f ? MAX(top, 0.1f) : 0.0f, lai * 0.25f, 0.0f));
		}
	}
	wm->set_canopy_map_rect(p_rect);
	wm->set_canopy_map(ImageTexture::create_from_image(img));
}

float VegetationField3D::get_shade_at(const Vector3 &p_global_position) const {
	return _shade_at(Vector2(p_global_position.x, p_global_position.z));
}

PackedVector3Array VegetationField3D::get_placements(int p_species) const {
	PackedVector3Array res;
	ERR_FAIL_INDEX_V(p_species, int(placements.size()), res);
	for (const Vector3 &p : placements[p_species]) {
		res.push_back(p);
	}
	return res;
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

void VegetationField3D::_on_map_baked() {
	if (populate_on_ready) {
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
				if (map->is_baked() && populate_on_ready) {
					queue_populate();
				}
			}
		} break;
		case NOTIFICATION_PREDELETE: {
			_clear_generated();
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
	ClassDB::bind_method(D_METHOD("get_shade_at", "global_position"), &VegetationField3D::get_shade_at);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "biome", PROPERTY_HINT_RESOURCE_TYPE, "Biome"), "set_biome", "get_biome");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "density_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_density_mask", "get_density_mask");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "density_mask_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_density_mask_rect", "get_density_mask_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "1,256,1,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "populate_on_ready"), "set_populate_on_ready", "is_populate_on_ready");

	ADD_SIGNAL(MethodInfo("populated"));
}

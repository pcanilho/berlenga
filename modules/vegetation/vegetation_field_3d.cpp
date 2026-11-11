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
#include "servers/rendering/rendering_server.h"

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

	// Far ground colour cells, about 1 m.
	far_size = Vector2i(CLAMP(int(Math::ceil(rect.size.x)), 1, 2048), CLAMP(int(Math::ceil(rect.size.y)), 1, 2048));
	far_cells.clear();
	far_cells.resize(far_size.x * far_size.y);

	for (int s = 0; s < species_count; s++) {
		Ref<PlantSpecies> sp = species_list[s];
		if (sp.is_null()) {
			continue;
		}
		const TypedArray<Mesh> meshes = sp->get_meshes();
		const Color far_srgb = sp->get_far_color();
		const Color far_color = Color(far_srgb.r, far_srgb.g, far_srgb.b).srgb_to_linear() * Color(1, 1, 1, 0) + Color(0, 0, 0, far_srgb.a);
		const float range = sp->get_visibility_range();
		const int variants = MAX(1, meshes.size());
		const Pattern &pat = _get_pattern(sp->get_spacing(), s);
		const float fp = sp->get_footprint();
		const Vector2 scale_range = sp->get_scale_range();
		const float align = sp->get_align_to_ground();
		const int reach = int(Math::ceil((fp + max_footprint) / grid_cell));
		const bool shade_rule = sp->uses_shade();
		const bool conifer_rule = sp->uses_conifer();
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
					if ((shade_rule || conifer_rule) && suit > 0.0f) {
						float conifer = 0.0f;
						const float shade = _shade_at(xz, &conifer);
						if (shade_rule) {
							suit *= sp->rule_weight(PlantSpecies::RULE_SHADE, shade);
						}
						if (conifer_rule) {
							suit *= sp->rule_weight(PlantSpecies::RULE_CONIFER, conifer);
						}
					}
					if (sp->get_mask_channel() >= 0 && suit > 0.0f) {
						const float m = CLAMP(mask_at(xz, sp->get_mask_channel()), 0.0f, 1.0f);
						suit *= sp->is_mask_invert() ? 1.0f - m : m;
					}
					const float keep = sp->get_density() * suit;
					if (rank >= keep) {
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
					// Floating plants sit on the water surface, level, over the ground below it.
					const bool floats = sp->is_floating();
					const float y = map->get_height_at(p3) + (floats ? map->get_water_depth_at(p3) : 0.0f);
					const float slope = floats ? 0.0f : map->get_slope_at(p3);
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
						crowns.push_back({ xz, cr, sp->get_canopy_opacity(), canopy_top * scale, sp->is_canopy_conifer() ? 1.0f : 0.0f });
						crown_grid[Vector2i(int(Math::floor(xz.x / CROWN_CELL)), int(Math::floor(xz.y / CROWN_CELL)))].push_back(crowns.size() - 1);
						max_crown_radius = MAX(max_crown_radius, cr);
					}
					xforms[variant].push_back(to_local * world);
					// Custom data for the plant shader: the rank among the plants kept here (blue noise,
					// so thinning by distance keeps the field even), the species' range, size, species.
					customs[variant].push_back(Color(CLAMP(rank / MAX(keep, 1e-4f), 0.0f, 1.0f), range, scale, float(s)));
					if (far_color.a > 0.0f && range > 0.0f) {
						const int fx = CLAMP(int((xz.x - rect.position.x) / rect.size.x * far_size.x), 0, far_size.x - 1);
						const int fz = CLAMP(int((xz.y - rect.position.y) / rect.size.y * far_size.y), 0, far_size.y - 1);
						const float cell_area = (rect.size.x / far_size.x) * (rect.size.y / far_size.y);
						// The ground a plant hides: a disc of half its spacing, scaled.
						const float rr = 0.5f * sp->get_spacing() * scale;
						const float c = far_color.a * MIN(Math::PI * rr * rr / cell_area, 4.0f);
						FarCell &fc = far_cells[fz * far_size.x + fx];
						fc.r += far_color.r * c;
						fc.g += far_color.g * c;
						fc.b += far_color.b * c;
						fc.cover += c;
						fc.range += range * c;
					}
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
					if (range > 0.0f) {
						// Ground cover too small to see far away. The plant shader thins the instances
						// by rank and shrinks them out before the range (docs/ideas/plant-generator.md,
						// "Far distance"), so the tile is cut only once every plant in it is gone.
						mmi->set_visibility_range_end(range * 1.05f);
					}
					add_child(mmi, false, INTERNAL_MODE_BACK);
					generated.push_back(mmi);
				}
			}
		}
	}
	_publish_canopy(rect);
	_publish_floor(rect);
	_publish_far(rect);
	populate_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("populated"));
	return OK;
}

// Canopy shade as the share of sky light the crowns stop. Each crown passes 1 - opacity of the
// light under its core (Beer-Lambert through the foliage), thinning to nothing a little past its
// rim, where diffuse sky light still reaches under the edge; overlapping crowns multiply their
// transmission. Plants then take it as a shade rule, the way field botany ranks them by light
// (Ellenberg's L values: bilberry, ferns and mosses in shade, meadow grasses in the open).
float VegetationField3D::_shade_at(const Vector2 &p_xz, float *r_conifer) const {
	if (r_conifer) {
		*r_conifer = 0.0f;
	}
	if (crowns.is_empty()) {
		return 0.0f;
	}
	const float reach_m = max_crown_radius * 1.4f;
	const int r = int(Math::ceil(reach_m / CROWN_CELL));
	const Vector2i gc(int(Math::floor(p_xz.x / CROWN_CELL)), int(Math::floor(p_xz.y / CROWN_CELL)));
	float transmit = 1.0f;
	float needles = 0.0f;
	float weight = 0.0f;
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

// The forest floor under the placed crowns, for the ground's shader (get_floor_shader_code()):
// canopy closure (the shade, 1 - transmitted sky light) in red and the conifer share of the
// cover in green, on half-metre cells. Under a closed canopy the floor is litter, not soil
// (docs/ideas/forest-floor.md), and its kind follows the trees: leaves under broadleaves,
// needles and moss under conifers.
void VegetationField3D::_publish_floor(const Rect2 &p_rect) {
	if (!forest_floor_enabled || !is_inside_tree() || crowns.is_empty()) {
		return; // Another field with crowns may own the floor map; an empty one leaves it alone.
	}
	const int w = CLAMP(int(Math::ceil(p_rect.size.x * 2.0f)), 1, 2048);
	const int h = CLAMP(int(Math::ceil(p_rect.size.y * 2.0f)), 1, 2048);
	Vector<uint8_t> data;
	data.resize(w * h * 2);
	uint8_t *d = data.ptrw();
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const Vector2 p = p_rect.position + Vector2((x + 0.5f) * p_rect.size.x / w, (y + 0.5f) * p_rect.size.y / h);
			float conifer = 0.0f;
			const float shade = _shade_at(p, &conifer);
			d[(y * w + x) * 2 + 0] = uint8_t(CLAMP(shade, 0.0f, 1.0f) * 255.0f + 0.5f);
			d[(y * w + x) * 2 + 1] = uint8_t(CLAMP(conifer, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
	}
	floor_texture = ImageTexture::create_from_image(Image::create_from_data(w, h, false, Image::FORMAT_RG8, data));
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("forest_floor_map", floor_texture->get_rid());
	rs->global_shader_parameter_set("forest_floor_rect", Vector4(p_rect.position.x, p_rect.position.y, p_rect.size.x, p_rect.size.y));
}

// Plants beyond their range as ground colour (vegetation_far_map, read by get_floor_shader_code()'s
// vegetation_far()): the mean colour of the plants on each cell and the share of ground they hide,
// and in the map's rect a mean range, so the ground takes over as the plant shader thins them.
void VegetationField3D::_publish_far(const Rect2 &p_rect) {
	if (!is_inside_tree()) {
		return;
	}
	float range_sum = 0.0f;
	float cover_sum = 0.0f;
	for (const FarCell &c : far_cells) {
		range_sum += c.range;
		cover_sum += c.cover;
	}
	if (cover_sum <= 0.0f) {
		far_cells.clear();
		return; // Nothing to show far away; another field may own the map.
	}
	// Premultiplied linear colour, stored as its square root in 8 bits (dark plant colours keep
	// their precision; the shader squares it back), so filtering between a patch and bare cells
	// fades the cover without darkening the colour. 8-bit RGBA: the path tracer's material
	// evaluation reads it as the raster does (a half-float map came back white there).
	Ref<Image> img = Image::create_empty(far_size.x, far_size.y, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < far_size.y; y++) {
		for (int x = 0; x < far_size.x; x++) {
			const FarCell &c = far_cells[y * far_size.x + x];
			const float inv = c.cover > 0.0f ? 1.0f / c.cover : 0.0f;
			// Overlapping plants: cover adds as 1 - exp(-sum), like overlapping crowns.
			const float a = 1.0f - Math::exp(-c.cover);
			img->set_pixel(x, y, Color(Math::sqrt(c.r * inv * a), Math::sqrt(c.g * inv * a), Math::sqrt(c.b * inv * a), a));
		}
	}
	far_texture = ImageTexture::create_from_image(img);
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("vegetation_far_map", far_texture->get_rid());
	rs->global_shader_parameter_set("vegetation_far_rect", Vector4(p_rect.position.x, p_rect.position.y, p_rect.size.x, p_rect.size.y));
	rs->global_shader_parameter_set("vegetation_far_range", range_sum / cover_sum);
	far_cells.clear();
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
	float cover = smoothstep(0.12, 0.5, f.x + 0.12 * (ff_noise(world_pos.xz * 0.6) - 0.5));
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
	return _shade_at(Vector2(p_global_position.x, p_global_position.z));
}

float VegetationField3D::get_conifer_at(const Vector3 &p_global_position) const {
	float conifer = 0.0f;
	_shade_at(Vector2(p_global_position.x, p_global_position.z), &conifer);
	return conifer;
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
	ClassDB::bind_static_method("VegetationField3D", D_METHOD("get_floor_shader_code"), &VegetationField3D::get_floor_shader_code);
	ClassDB::bind_method(D_METHOD("set_forest_floor_enabled", "enabled"), &VegetationField3D::set_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("is_forest_floor_enabled"), &VegetationField3D::is_forest_floor_enabled);
	ClassDB::bind_method(D_METHOD("get_conifer_at", "global_position"), &VegetationField3D::get_conifer_at);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "biome", PROPERTY_HINT_RESOURCE_TYPE, "Biome"), "set_biome", "get_biome");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forest_floor_enabled"), "set_forest_floor_enabled", "is_forest_floor_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "density_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_density_mask", "get_density_mask");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "density_mask_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_density_mask_rect", "get_density_mask_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "1,256,1,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "populate_on_ready"), "set_populate_on_ready", "is_populate_on_ready");

	ADD_SIGNAL(MethodInfo("populated"));
}

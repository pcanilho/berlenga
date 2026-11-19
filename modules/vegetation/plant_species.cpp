/**************************************************************************/
/*  plant_species.cpp                                                     */
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

#include "plant_species.h"

#include "core/object/class_db.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"

PlantSpecies::PlantSpecies() {
	ranges[RULE_SLOPE] = Vector2(0.0f, 90.0f);
	ranges[RULE_HEIGHT] = Vector2(-100000.0f, 100000.0f);
	ranges[RULE_CURVATURE] = Vector2(-1000.0f, 1000.0f);
	ranges[RULE_WETNESS] = Vector2(-1000.0f, 1000.0f);
	// Dry land by default: no plant grows under water unless it says so.
	ranges[RULE_WATER_DEPTH] = Vector2(0.0f, 0.0f);
	fades[RULE_WATER_DEPTH] = 0.05f;
	ranges[RULE_WATER_DISTANCE] = Vector2(-1000000.0f, 1000000.0f);
	ranges[RULE_SOIL_DEPTH] = Vector2(0.0f, 100000.0f);
	ranges[RULE_ROCK] = Vector2(0.0f, 1.0f);
	ranges[RULE_TALUS] = Vector2(0.0f, 1.0f);
	ranges[RULE_SHADE] = Vector2(0.0f, 1.0f);
	ranges[RULE_TEMPERATURE] = Vector2(-100.0f, 100.0f);
	ranges[RULE_JANUARY_TEMPERATURE] = Vector2(-100.0f, 100.0f);
	ranges[RULE_PRECIPITATION] = Vector2(0.0f, 100000.0f);
	ranges[RULE_ARIDITY] = Vector2(0.0f, 1000.0f);
	ranges[RULE_SUNLIGHT] = Vector2(0.0f, 2000.0f);
	ranges[RULE_CONIFER] = Vector2(0.0f, 1.0f);
	// Out of a waterfall's spray by default: in the curtain's wet zone only mosses and wet rock
	// (spray-zone vegetation; a species that wants it widens the range).
	ranges[RULE_SPRAY] = Vector2(0.0f, 0.5f);
	fades[RULE_SPRAY] = 0.15f;
	ranges[RULE_LITTER] = Vector2(0.0f, 1.0f);
	ranges[RULE_DUNE] = Vector2(-1.0f, 1.0f);
	ranges[RULE_SEASON_TEMPERATURE] = Vector2(-100.0f, 100.0f);
	ranges[RULE_TIDE_HEIGHT] = Vector2(-10000.0f, 10000.0f);
	ranges[RULE_COAST_EXPOSURE] = Vector2(0.0f, 1.0f);
	ranges[RULE_BEACH] = Vector2(0.0f, 1.0f);
	ranges[RULE_FIELD_EDGE] = Vector2(-100000.0f, 100000.0f);
	ranges[RULE_SAND_MOBILITY] = Vector2(-1.0f, 1.0f);
	ranges[RULE_BURN_AGE] = Vector2(-1.0f, 1000.0f);
}

float PlantSpecies::season_temperature(float p_january, float p_july) {
	// T(theta) = a - b cos(theta), theta = 0 mid-January. The season is where T >= 0.9 C:
	// cos(theta) <= c, theta in [t0, 2 pi - t0]; its mean is a + b sin(t0) / (pi - t0).
	const float a = 0.5f * (p_january + p_july);
	const float b = 0.5f * (p_july - p_january);
	if (Math::abs(b) < 1e-4f) {
		return a >= 0.9f ? a : -10.0f;
	}
	const float c = (a - 0.9f) / b;
	if (c >= 1.0f) {
		return a; // Every day counts.
	}
	if (c <= -1.0f) {
		return -10.0f;
	}
	const float t0 = Math::acos(c);
	if (365.0f * (1.0f - t0 / float(Math::PI)) < 94.0f) {
		return -10.0f;
	}
	return a + b * Math::sin(t0) / (float(Math::PI) - t0);
}

void PlantSpecies::set_meshes(const TypedArray<Mesh> &p_meshes) {
	meshes = p_meshes;
	emit_changed();
}

void PlantSpecies::set_spacing(float p_spacing) {
	spacing = MAX(p_spacing, 0.05f);
	emit_changed();
}

void PlantSpecies::set_footprint(float p_footprint) {
	footprint = MAX(p_footprint, 0.0f);
	emit_changed();
}

void PlantSpecies::set_sward_shade(float p_shade) {
	sward_shade = CLAMP(p_shade, 0.0f, 1.0f);
	emit_changed();
}

void PlantSpecies::set_litter_color(const Color &p_color) {
	litter_color = p_color;
	litter_color.a = CLAMP(litter_color.a, 0.0f, 1.0f);
	emit_changed();
}

void PlantSpecies::set_density(float p_density) {
	density = CLAMP(p_density, 0.0f, 1.0f);
	emit_changed();
}

void PlantSpecies::set_scale_range(const Vector2 &p_range) {
	scale_range = Vector2(MAX(p_range.x, 0.001f), MAX(p_range.y, 0.001f));
	emit_changed();
}

void PlantSpecies::set_align_to_ground(float p_align) {
	align_to_ground = CLAMP(p_align, 0.0f, 1.0f);
	emit_changed();
}

void PlantSpecies::set_random_yaw(bool p_enable) {
	random_yaw = p_enable;
	emit_changed();
}

void PlantSpecies::set_canopy_conifer(bool p_conifer) {
	canopy_conifer = p_conifer;
	emit_changed();
}

void PlantSpecies::set_floating(bool p_floating) {
	floating = p_floating;
	emit_changed();
}

void PlantSpecies::set_rule_range(Rule p_rule, const Vector2 &p_range) {
	ERR_FAIL_INDEX(p_rule, RULE_MAX);
	ranges[p_rule] = p_range;
	emit_changed();
}

Vector2 PlantSpecies::get_rule_range(Rule p_rule) const {
	ERR_FAIL_INDEX_V(p_rule, RULE_MAX, Vector2());
	return ranges[p_rule];
}

void PlantSpecies::set_rule_fade(Rule p_rule, float p_fade) {
	ERR_FAIL_INDEX(p_rule, RULE_MAX);
	fades[p_rule] = MAX(p_fade, 0.0f);
	emit_changed();
}

float PlantSpecies::get_rule_fade(Rule p_rule) const {
	ERR_FAIL_INDEX_V(p_rule, RULE_MAX, 0.0f);
	return fades[p_rule];
}

float PlantSpecies::rule_weight(Rule p_rule, float p_value) const {
	const Vector2 r = ranges[p_rule];
	const float f = fades[p_rule];
	if (f <= 0.0f) {
		return (p_value >= r.x && p_value <= r.y) ? 1.0f : 0.0f;
	}
	return Math::smoothstep(r.x - f, r.x, p_value) * (1.0f - Math::smoothstep(r.y, r.y + f, p_value));
}

float PlantSpecies::get_suitability(const TerrainDataMap3D *p_map, const Vector3 &p_pos, bool p_patches) const {
	float w = rule_weight(RULE_SLOPE, Math::rad_to_deg(p_map->get_slope_at(p_pos)));
	if (w <= 0.0f) {
		return 0.0f;
	}
	w *= rule_weight(RULE_WATER_DEPTH, p_map->get_water_depth_at(p_pos));
	w *= rule_weight(RULE_WATER_DISTANCE, p_map->get_water_distance_at(p_pos));
	w *= rule_weight(RULE_WETNESS, p_map->get_wetness_at(p_pos));
	w *= rule_weight(RULE_CURVATURE, p_map->get_curvature_at(p_pos));
	w *= rule_weight(RULE_HEIGHT, p_map->get_height_at(p_pos));
	if (p_map->has_geology()) {
		// Soil, bare rock and scree from the terrain generator: trees need soil to root in,
		// lichens and cushion plants take the rock, a few specialists the scree.
		w *= rule_weight(RULE_SOIL_DEPTH, p_map->get_soil_depth_at(p_pos));
		w *= rule_weight(RULE_ROCK, p_map->get_rock_at(p_pos));
		w *= rule_weight(RULE_TALUS, p_map->get_talus_at(p_pos));
		w *= rule_weight(RULE_SPRAY, p_map->get_spray_at(p_pos));
	}
	if (p_map->has_climate() && w > 0.0f) {
		// Climate from TerrainDataMap3D.bake_climate(): where a species can live at all
		// (temperature, frost, rain, drought), and sunny against shaded slopes.
		w *= rule_weight(RULE_TEMPERATURE, p_map->get_temperature_at(p_pos));
		w *= rule_weight(RULE_JANUARY_TEMPERATURE, p_map->get_temperature_at(p_pos, 15.0f));
		w *= rule_weight(RULE_PRECIPITATION, p_map->get_precipitation_at(p_pos));
		w *= rule_weight(RULE_ARIDITY, p_map->get_aridity_at(p_pos));
		w *= rule_weight(RULE_SUNLIGHT, p_map->get_shortwave_at(p_pos));
		const Vector2 season_range = ranges[RULE_SEASON_TEMPERATURE];
		if (w > 0.0f && (season_range.x > -100.0f || season_range.y < 100.0f)) {
			const float jan = p_map->get_temperature_at(p_pos, 15.0f);
			const float jul = p_map->get_temperature_at(p_pos, 15.0f + 365.0f * 0.5f);
			w *= rule_weight(RULE_SEASON_TEMPERATURE, season_temperature(jan, jul));
		}
	}
	if (p_patches && patch_scale > 0.0f && w > 0.0f) {
		w *= get_patch_weight(p_pos);
	}
	return w;
}

static float patch_hash(int p_x, int p_z, int p_seed) {
	uint32_t h = uint32_t(p_x) * 0x8DA6B343u ^ uint32_t(p_z) * 0xD8163841u ^ uint32_t(p_seed) * 0xCB1AB31Fu;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return (h & 0xFFFFFF) / float(0xFFFFFF);
}

static float patch_value_noise(float p_x, float p_z, int p_seed) {
	const int ix = int(Math::floor(p_x));
	const int iz = int(Math::floor(p_z));
	float fx = p_x - ix;
	float fz = p_z - iz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float a = patch_hash(ix, iz, p_seed);
	const float b = patch_hash(ix + 1, iz, p_seed);
	const float c = patch_hash(ix, iz + 1, p_seed);
	const float d = patch_hash(ix + 1, iz + 1, p_seed);
	return Math::lerp(Math::lerp(a, b, fx), Math::lerp(c, d, fx), fz);
}

// Coupes: a Voronoi diagram over a jittered grid of patch_scale cells (straight edges, as coupes
// are laid out along banks and rides), the distance to the cell's edge as the distance to the
// nearest bisector (Quilez, Voronoi edges, 2012).
Vector3 PlantSpecies::get_coupe_at(const Vector3 &p_pos, Vector2 *r_out) const {
	if (coupe_rotation <= 0 || patch_scale <= 0.0f) {
		if (r_out) {
			*r_out = Vector2();
		}
		return Vector3(-1.0f, 1e6f, 0.0f);
	}
	const Vector2 p(p_pos.x / patch_scale, p_pos.z / patch_scale);
	const int ix = int(Math::floor(p.x)), iz = int(Math::floor(p.y));
	auto site = [&](int p_x, int p_z) {
		return Vector2(float(p_x) + 0.15f + 0.7f * patch_hash(p_x, p_z, patch_seed + 31), float(p_z) + 0.15f + 0.7f * patch_hash(p_x, p_z, patch_seed + 37));
	};
	Vector2i best;
	Vector2 a;
	float bd = 1e30f;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Vector2 s = site(ix + dx, iz + dz);
			const float d = p.distance_squared_to(s);
			if (d < bd) {
				bd = d;
				best = Vector2i(ix + dx, iz + dz);
				a = s;
			}
		}
	}
	float edge = 1e30f;
	Vector2 out;
	bool ride = false;
	for (int dz = -2; dz <= 2; dz++) {
		for (int dx = -2; dx <= 2; dx++) {
			const Vector2i c(best.x + dx, best.y + dz);
			if (c == best) {
				continue;
			}
			const Vector2 b = site(c.x, c.y);
			const Vector2 n = (b - a).normalized();
			const float d = (0.5f * (a + b) - p).dot(n);
			if (d < edge) {
				edge = d;
				out = n;
				// A ride along about half the boundaries, the same either side of it.
				const Vector2i lo = best.x < c.x || (best.x == c.x && best.y < c.y) ? best : c;
				const Vector2i hi = lo == best ? c : best;
				ride = patch_hash(lo.x * 7919 + hi.x, lo.y * 104729 + hi.y, patch_seed + 41) < 0.5f;
			}
		}
	}
	edge *= patch_scale;
	if (ride) {
		edge -= 0.5f * ride_width;
	}
	if (r_out) {
		*r_out = out;
	}
	// The cutting order: the year steps by about 5 along x and 8 along z (mod the rotation), so
	// a coupe and its neighbours differ by four years or more, fresh stools next to tall poles
	// (docs, 18 Nov: a random age per coupe often put like beside like and the edges barely
	// read), with a year's jitter.
	const int step = best.x * 5 + best.y * 8 + int(patch_hash(best.x, best.y, patch_seed + 43) * 2.0f);
	const float age = float(((step % coupe_rotation) + coupe_rotation) % coupe_rotation);
	return Vector3(age, edge, ride && edge < 0.0f ? 1.0f : 0.0f);
}

// The patch noise is a smooth field: its value over the threshold divided by its gradient is the
// distance to the edge to first order (the edge is where it crosses the threshold).
float PlantSpecies::get_patch_depth(const Vector3 &p_pos, Vector2 *r_out) const {
	if (patch_scale <= 0.0f) {
		if (r_out) {
			*r_out = Vector2();
		}
		return 1e6f;
	}
	if (coupe_rotation > 0) {
		Vector2 out;
		const Vector3 c = get_coupe_at(p_pos, &out);
		const bool in = int(c.x) >= coupe_ages.x && int(c.x) <= coupe_ages.y;
		// Inside a coupe of its ages: the way out is to the nearest edge. Outside: approximate
		// (the distance to this coupe's edge; a neighbour of the right age may be nearer).
		float d = in ? c.y : -MAX(c.y, 0.0f);
		if (patch_invert) {
			d = -d;
		}
		if (r_out) {
			*r_out = d >= 0.0f ? out : -out;
		}
		return d;
	}
	const auto noise = [&](float p_x, float p_z) {
		const float x = p_x / patch_scale;
		const float z = p_z / patch_scale;
		return 0.7f * patch_value_noise(x, z, patch_seed) + 0.3f * patch_value_noise(x * 2.7f + 17.3f, z * 2.7f - 5.1f, patch_seed + 1);
	};
	const float threshold = 0.5f + 0.42f * (0.5f - patch_cover) * 2.0f;
	const float e = MAX(patch_scale * 0.02f, 0.05f);
	const float n = noise(p_pos.x, p_pos.z);
	Vector2 g((noise(p_pos.x + e, p_pos.z) - noise(p_pos.x - e, p_pos.z)) / (2.0f * e), (noise(p_pos.x, p_pos.z + e) - noise(p_pos.x, p_pos.z - e)) / (2.0f * e));
	float inside = n - threshold;
	if (patch_invert) {
		inside = -inside;
		g = -g;
	}
	const float gl = g.length();
	if (r_out) {
		*r_out = gl > 1e-6f ? -g / gl : Vector2();
	}
	// Flat noise (no gradient): deep inside or far outside.
	return gl > 1e-6f ? inside / gl : (inside > 0.0f ? 1e6f : -1e6f);
}

float PlantSpecies::get_patch_weight(const Vector3 &p_pos) const {
	if (patch_scale <= 0.0f) {
		return 1.0f;
	}
	if (coupe_rotation > 0) {
		const Vector3 c = get_coupe_at(p_pos);
		const bool in = int(c.x) >= coupe_ages.x && int(c.x) <= coupe_ages.y;
		const float w = in ? Math::smoothstep(0.0f, 0.6f, c.y) : 0.0f;
		return patch_invert ? 1.0f - w : w;
	}
	if (patch_ring > 0.0f) {
		// Fairy rings: the mycelium spreads outward and dies behind, fruiting along its advancing
		// front (Dowson, Rayner and Boddy 1989): a band along the patches' edges.
		const float d = get_patch_depth(p_pos) / patch_ring;
		return Math::exp(-d * d);
	}
	// Two octaves of value noise; the threshold keeps patch_cover of the ground, with soft edges.
	const float x = p_pos.x / patch_scale;
	const float z = p_pos.z / patch_scale;
	const float n = 0.7f * patch_value_noise(x, z, patch_seed) + 0.3f * patch_value_noise(x * 2.7f + 17.3f, z * 2.7f - 5.1f, patch_seed + 1);
	// Two octaves of value noise are close to a bell around 0.5: map the cover through it.
	const float threshold = 0.5f + 0.42f * (0.5f - patch_cover) * 2.0f;
	const float w = Math::smoothstep(threshold - 0.06f, threshold + 0.06f, n);
	return patch_invert ? 1.0f - w : w;
}

float PlantSpecies::get_suitability_at(Object *p_map, const Vector3 &p_pos) const {
	const TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(p_map);
	ERR_FAIL_COND_V_MSG(!map || !map->is_baked(), 0.0f, "PlantSpecies: needs a baked TerrainDataMap3D.");
	return get_suitability(map, p_pos);
}

void PlantSpecies::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_meshes", "meshes"), &PlantSpecies::set_meshes);
	ClassDB::bind_method(D_METHOD("get_meshes"), &PlantSpecies::get_meshes);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &PlantSpecies::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &PlantSpecies::get_spacing);
	ClassDB::bind_method(D_METHOD("set_footprint", "footprint"), &PlantSpecies::set_footprint);
	ClassDB::bind_method(D_METHOD("set_sward_shade", "shade"), &PlantSpecies::set_sward_shade);
	ClassDB::bind_method(D_METHOD("get_sward_shade"), &PlantSpecies::get_sward_shade);
	ClassDB::bind_method(D_METHOD("set_trample_resistance", "passes"), &PlantSpecies::set_trample_resistance);
	ClassDB::bind_method(D_METHOD("get_trample_resistance"), &PlantSpecies::get_trample_resistance);
	ClassDB::bind_method(D_METHOD("set_burn_recovery", "years"), &PlantSpecies::set_burn_recovery);
	ClassDB::bind_method(D_METHOD("get_burn_recovery"), &PlantSpecies::get_burn_recovery);
	ClassDB::bind_method(D_METHOD("burn_cover", "age"), &PlantSpecies::burn_cover);
	ClassDB::bind_method(D_METHOD("set_detail_size", "size"), &PlantSpecies::set_detail_size);
	ClassDB::bind_method(D_METHOD("get_detail_size"), &PlantSpecies::get_detail_size);
	ClassDB::bind_method(D_METHOD("trample_cover", "passes"), &PlantSpecies::trample_cover);
	ClassDB::bind_method(D_METHOD("trample_scale", "passes"), &PlantSpecies::trample_scale);
	ClassDB::bind_method(D_METHOD("get_patch_depth", "position"), &PlantSpecies::get_patch_depth_at);
	ClassDB::bind_method(D_METHOD("set_rock_foot", "amount"), &PlantSpecies::set_rock_foot);
	ClassDB::bind_method(D_METHOD("get_rock_foot"), &PlantSpecies::get_rock_foot);
	ClassDB::bind_method(D_METHOD("set_stone_gap", "amount"), &PlantSpecies::set_stone_gap);
	ClassDB::bind_method(D_METHOD("get_stone_gap"), &PlantSpecies::get_stone_gap);
	ClassDB::bind_method(D_METHOD("set_margin_width", "width"), &PlantSpecies::set_margin_width);
	ClassDB::bind_method(D_METHOD("get_margin_width"), &PlantSpecies::get_margin_width);
	ClassDB::bind_method(D_METHOD("set_margin_scale", "scale"), &PlantSpecies::set_margin_scale);
	ClassDB::bind_method(D_METHOD("get_margin_scale"), &PlantSpecies::get_margin_scale);
	ClassDB::bind_method(D_METHOD("set_margin_lean", "degrees"), &PlantSpecies::set_margin_lean);
	ClassDB::bind_method(D_METHOD("get_margin_lean"), &PlantSpecies::get_margin_lean);
	ClassDB::bind_method(D_METHOD("set_litter_color", "color"), &PlantSpecies::set_litter_color);
	ClassDB::bind_method(D_METHOD("get_litter_color"), &PlantSpecies::get_litter_color);
	ClassDB::bind_method(D_METHOD("get_footprint"), &PlantSpecies::get_footprint);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &PlantSpecies::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &PlantSpecies::get_density);
	ClassDB::bind_method(D_METHOD("set_scale_range", "range"), &PlantSpecies::set_scale_range);
	ClassDB::bind_method(D_METHOD("get_scale_range"), &PlantSpecies::get_scale_range);
	ClassDB::bind_method(D_METHOD("set_align_to_ground", "align"), &PlantSpecies::set_align_to_ground);
	ClassDB::bind_method(D_METHOD("get_align_to_ground"), &PlantSpecies::get_align_to_ground);
	ClassDB::bind_method(D_METHOD("set_random_yaw", "enable"), &PlantSpecies::set_random_yaw);
	ClassDB::bind_method(D_METHOD("is_random_yaw"), &PlantSpecies::is_random_yaw);
	ClassDB::bind_method(D_METHOD("set_floating", "floating"), &PlantSpecies::set_floating);
	ClassDB::bind_method(D_METHOD("is_floating"), &PlantSpecies::is_floating);
	ClassDB::bind_method(D_METHOD("set_stone_top", "on"), &PlantSpecies::set_stone_top);
	ClassDB::bind_method(D_METHOD("is_stone_top"), &PlantSpecies::is_stone_top);
	ClassDB::bind_method(D_METHOD("set_stone_top_damp", "amount"), &PlantSpecies::set_stone_top_damp);
	ClassDB::bind_method(D_METHOD("get_stone_top_damp"), &PlantSpecies::get_stone_top_damp);
	ClassDB::bind_method(D_METHOD("set_stone_top_water", "band"), &PlantSpecies::set_stone_top_water);
	ClassDB::bind_method(D_METHOD("get_stone_top_water"), &PlantSpecies::get_stone_top_water);
	ClassDB::bind_method(D_METHOD("set_follow_flow", "on"), &PlantSpecies::set_follow_flow);
	ClassDB::bind_method(D_METHOD("is_follow_flow"), &PlantSpecies::is_follow_flow);
	ClassDB::bind_method(D_METHOD("set_wind_frontal_area", "area"), &PlantSpecies::set_wind_frontal_area);
	ClassDB::bind_method(D_METHOD("get_wind_frontal_area"), &PlantSpecies::get_wind_frontal_area);
	ClassDB::bind_method(D_METHOD("set_rule_range", "rule", "range"), &PlantSpecies::set_rule_range);
	ClassDB::bind_method(D_METHOD("get_rule_range", "rule"), &PlantSpecies::get_rule_range);
	ClassDB::bind_method(D_METHOD("set_rule_fade", "rule", "fade"), &PlantSpecies::set_rule_fade);
	ClassDB::bind_method(D_METHOD("get_rule_fade", "rule"), &PlantSpecies::get_rule_fade);
	ClassDB::bind_method(D_METHOD("rule_weight", "rule", "value"), &PlantSpecies::rule_weight);
	ClassDB::bind_static_method("PlantSpecies", D_METHOD("season_temperature", "january", "july"), &PlantSpecies::season_temperature);
	ClassDB::bind_method(D_METHOD("set_patch_scale", "scale"), &PlantSpecies::set_patch_scale);
	ClassDB::bind_method(D_METHOD("get_patch_scale"), &PlantSpecies::get_patch_scale);
	ClassDB::bind_method(D_METHOD("set_patch_cover", "cover"), &PlantSpecies::set_patch_cover);
	ClassDB::bind_method(D_METHOD("get_patch_cover"), &PlantSpecies::get_patch_cover);
	ClassDB::bind_method(D_METHOD("set_patch_seed", "seed"), &PlantSpecies::set_patch_seed);
	ClassDB::bind_method(D_METHOD("get_patch_seed"), &PlantSpecies::get_patch_seed);
	ClassDB::bind_method(D_METHOD("set_mask_channel", "channel"), &PlantSpecies::set_mask_channel);
	ClassDB::bind_method(D_METHOD("get_mask_channel"), &PlantSpecies::get_mask_channel);
	ClassDB::bind_method(D_METHOD("set_mask_invert", "invert"), &PlantSpecies::set_mask_invert);
	ClassDB::bind_method(D_METHOD("is_mask_invert"), &PlantSpecies::is_mask_invert);
	ClassDB::bind_method(D_METHOD("set_patch_invert", "invert"), &PlantSpecies::set_patch_invert);
	ClassDB::bind_method(D_METHOD("is_patch_invert"), &PlantSpecies::is_patch_invert);
	ClassDB::bind_method(D_METHOD("set_patch_ring", "width"), &PlantSpecies::set_patch_ring);
	ClassDB::bind_method(D_METHOD("set_coupe_rotation", "years"), &PlantSpecies::set_coupe_rotation);
	ClassDB::bind_method(D_METHOD("get_coupe_rotation"), &PlantSpecies::get_coupe_rotation);
	ClassDB::bind_method(D_METHOD("set_coupe_ages", "ages"), &PlantSpecies::set_coupe_ages);
	ClassDB::bind_method(D_METHOD("get_coupe_ages"), &PlantSpecies::get_coupe_ages);
	ClassDB::bind_method(D_METHOD("set_ride_width", "width"), &PlantSpecies::set_ride_width);
	ClassDB::bind_method(D_METHOD("get_ride_width"), &PlantSpecies::get_ride_width);
	ClassDB::bind_method(D_METHOD("get_coupe_at", "global_position"), &PlantSpecies::get_coupe_at_bind);
	ClassDB::bind_method(D_METHOD("get_patch_ring"), &PlantSpecies::get_patch_ring);
	ClassDB::bind_method(D_METHOD("get_patch_weight", "global_position"), &PlantSpecies::get_patch_weight);
	ClassDB::bind_method(D_METHOD("set_near_meshes", "meshes"), &PlantSpecies::set_near_meshes);
	ClassDB::bind_method(D_METHOD("get_near_meshes"), &PlantSpecies::get_near_meshes);
	ClassDB::bind_method(D_METHOD("set_near_range", "range"), &PlantSpecies::set_near_range);
	ClassDB::bind_method(D_METHOD("get_near_range"), &PlantSpecies::get_near_range);
	ClassDB::bind_method(D_METHOD("set_near_mesh_source", "source"), &PlantSpecies::set_near_mesh_source);
	ClassDB::bind_method(D_METHOD("get_near_mesh_source"), &PlantSpecies::get_near_mesh_source);
	ClassDB::bind_method(D_METHOD("set_near_mesh_key", "key"), &PlantSpecies::set_near_mesh_key);
	ClassDB::bind_method(D_METHOD("get_near_mesh_key"), &PlantSpecies::get_near_mesh_key);
	ClassDB::bind_method(D_METHOD("set_visibility_range", "range"), &PlantSpecies::set_visibility_range);
	ClassDB::bind_method(D_METHOD("get_visibility_range"), &PlantSpecies::get_visibility_range);
	ClassDB::bind_method(D_METHOD("set_far_color", "color"), &PlantSpecies::set_far_color);
	ClassDB::bind_method(D_METHOD("get_far_color"), &PlantSpecies::get_far_color);
	ClassDB::bind_method(D_METHOD("set_canopy_radius", "radius"), &PlantSpecies::set_canopy_radius);
	ClassDB::bind_method(D_METHOD("get_canopy_radius"), &PlantSpecies::get_canopy_radius);
	ClassDB::bind_method(D_METHOD("set_canopy_opacity", "opacity"), &PlantSpecies::set_canopy_opacity);
	ClassDB::bind_method(D_METHOD("get_canopy_opacity"), &PlantSpecies::get_canopy_opacity);
	ClassDB::bind_method(D_METHOD("set_canopy_conifer", "conifer"), &PlantSpecies::set_canopy_conifer);
	ClassDB::bind_method(D_METHOD("is_canopy_conifer"), &PlantSpecies::is_canopy_conifer);
	ClassDB::bind_method(D_METHOD("get_suitability_at", "data_map", "global_position"), &PlantSpecies::get_suitability_at);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "meshes", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("Mesh")), "set_meshes", "get_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,50,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "footprint", PROPERTY_HINT_RANGE, "0,50,0.01,or_greater,suffix:m"), "set_footprint", "get_footprint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sward_shade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_sward_shade", "get_sward_shade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trample_resistance", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:passes/yr"), "set_trample_resistance", "get_trample_resistance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "burn_recovery", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:yr"), "set_burn_recovery", "get_burn_recovery");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_size", PROPERTY_HINT_RANGE, "0,1,0.005,or_greater,suffix:m"), "set_detail_size", "get_detail_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "litter_color"), "set_litter_color", "get_litter_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_foot", PROPERTY_HINT_RANGE, "0,8,0.1"), "set_rock_foot", "get_rock_foot");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stone_gap", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_stone_gap", "get_stone_gap");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "margin_width", PROPERTY_HINT_RANGE, "0,20,0.1,or_greater,suffix:m"), "set_margin_width", "get_margin_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "margin_scale", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_margin_scale", "get_margin_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "margin_lean", PROPERTY_HINT_RANGE, "0,60,0.5,suffix:°"), "set_margin_lean", "get_margin_lean");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "scale_range"), "set_scale_range", "get_scale_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_ground", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_ground", "get_align_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_yaw"), "set_random_yaw", "is_random_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "floating"), "set_floating", "is_floating");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stone_top"), "set_stone_top", "is_stone_top");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stone_top_damp", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_stone_top_damp", "get_stone_top_damp");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "stone_top_water", PROPERTY_HINT_NONE, "suffix:m"), "set_stone_top_water", "get_stone_top_water");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_flow"), "set_follow_flow", "is_follow_flow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_frontal_area", PROPERTY_HINT_RANGE, "-1,10,0.001,or_greater"), "set_wind_frontal_area", "get_wind_frontal_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,1000,0.5,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "far_color"), "set_far_color", "get_far_color");
	ADD_GROUP("Near", "near_");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "near_meshes", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("Mesh")), "set_near_meshes", "get_near_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_range", PROPERTY_HINT_RANGE, "0,500,0.5,or_greater,suffix:m"), "set_near_range", "get_near_range");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "near_mesh_source", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_near_mesh_source", "get_near_mesh_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "near_mesh_key"), "set_near_mesh_key", "get_near_mesh_key");
	ADD_GROUP("Canopy", "canopy_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_radius", PROPERTY_HINT_RANGE, "0,30,0.1,or_greater,suffix:m"), "set_canopy_radius", "get_canopy_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_opacity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_canopy_opacity", "get_canopy_opacity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "canopy_conifer"), "set_canopy_conifer", "is_canopy_conifer");
	ClassDB::bind_method(D_METHOD("set_deadwood_rule", "rule"), &PlantSpecies::set_deadwood_rule);
	ClassDB::bind_method(D_METHOD("get_deadwood_rule"), &PlantSpecies::get_deadwood_rule);
	ClassDB::bind_method(D_METHOD("set_deadwood_band", "band"), &PlantSpecies::set_deadwood_band);
	ClassDB::bind_method(D_METHOD("get_deadwood_band"), &PlantSpecies::get_deadwood_band);
	ClassDB::bind_method(D_METHOD("set_deadwood_min_radius", "radius"), &PlantSpecies::set_deadwood_min_radius);
	ClassDB::bind_method(D_METHOD("get_deadwood_min_radius"), &PlantSpecies::get_deadwood_min_radius);
	ClassDB::bind_method(D_METHOD("set_deadwood_uphill", "uphill"), &PlantSpecies::set_deadwood_uphill);
	ClassDB::bind_method(D_METHOD("is_deadwood_uphill"), &PlantSpecies::is_deadwood_uphill);
	ClassDB::bind_method(D_METHOD("set_deadwood_from_wood", "from_wood"), &PlantSpecies::set_deadwood_from_wood);
	ClassDB::bind_method(D_METHOD("is_deadwood_from_wood"), &PlantSpecies::is_deadwood_from_wood);
	ADD_GROUP("Deadwood", "deadwood_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deadwood_rule", PROPERTY_HINT_ENUM, "Avoid,On,Beside,Ignore"), "set_deadwood_rule", "get_deadwood_rule");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deadwood_band", PROPERTY_HINT_RANGE, "0.01,5,0.01,suffix:m"), "set_deadwood_band", "get_deadwood_band");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deadwood_min_radius", PROPERTY_HINT_RANGE, "0,1,0.005,suffix:m"), "set_deadwood_min_radius", "get_deadwood_min_radius");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deadwood_uphill"), "set_deadwood_uphill", "is_deadwood_uphill");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deadwood_from_wood"), "set_deadwood_from_wood", "is_deadwood_from_wood");
	ADD_GROUP("Patches", "patch_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_scale", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_patch_scale", "get_patch_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_patch_cover", "get_patch_cover");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patch_seed"), "set_patch_seed", "get_patch_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "patch_invert"), "set_patch_invert", "is_patch_invert");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_ring", PROPERTY_HINT_RANGE, "0,5,0.05,suffix:m"), "set_patch_ring", "get_patch_ring");
	ADD_GROUP("Coupes", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "coupe_rotation", PROPERTY_HINT_RANGE, "0,40,1,suffix:years"), "set_coupe_rotation", "get_coupe_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "coupe_ages"), "set_coupe_ages", "get_coupe_ages");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ride_width", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:m"), "set_ride_width", "get_ride_width");
	ADD_GROUP("Patches", "patch_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask_channel", PROPERTY_HINT_ENUM, "None:-1,Red:0,Green:1,Blue:2,Alpha:3"), "set_mask_channel", "get_mask_channel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mask_invert"), "set_mask_invert", "is_mask_invert");

	ADD_GROUP("Rules", "");
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "slope_range", PROPERTY_HINT_NONE, "suffix:°"), "set_rule_range", "get_rule_range", RULE_SLOPE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "slope_fade", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_rule_fade", "get_rule_fade", RULE_SLOPE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "height_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_HEIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "height_fade", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_HEIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "curvature_range"), "set_rule_range", "get_rule_range", RULE_CURVATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "curvature_fade", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater"), "set_rule_fade", "get_rule_fade", RULE_CURVATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "wetness_range"), "set_rule_range", "get_rule_range", RULE_WETNESS);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "wetness_fade", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_rule_fade", "get_rule_fade", RULE_WETNESS);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "water_depth_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_WATER_DEPTH);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "water_depth_fade", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_WATER_DEPTH);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "water_distance_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_WATER_DISTANCE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "water_distance_fade", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_WATER_DISTANCE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "soil_depth_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_SOIL_DEPTH);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "soil_depth_fade", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_SOIL_DEPTH);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "rock_range"), "set_rule_range", "get_rule_range", RULE_ROCK);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "rock_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_ROCK);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "talus_range"), "set_rule_range", "get_rule_range", RULE_TALUS);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "talus_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_TALUS);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "shade_range"), "set_rule_range", "get_rule_range", RULE_SHADE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "shade_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_SHADE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "temperature_range", PROPERTY_HINT_NONE, "suffix:°C"), "set_rule_range", "get_rule_range", RULE_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "temperature_fade", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:°C"), "set_rule_fade", "get_rule_fade", RULE_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "january_temperature_range", PROPERTY_HINT_NONE, "suffix:°C"), "set_rule_range", "get_rule_range", RULE_JANUARY_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "january_temperature_fade", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:°C"), "set_rule_fade", "get_rule_fade", RULE_JANUARY_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "precipitation_range", PROPERTY_HINT_NONE, "suffix:mm"), "set_rule_range", "get_rule_range", RULE_PRECIPITATION);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "precipitation_fade", PROPERTY_HINT_RANGE, "0,2000,1,suffix:mm"), "set_rule_fade", "get_rule_fade", RULE_PRECIPITATION);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "aridity_range"), "set_rule_range", "get_rule_range", RULE_ARIDITY);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "aridity_fade", PROPERTY_HINT_RANGE, "0,5,0.01"), "set_rule_fade", "get_rule_fade", RULE_ARIDITY);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "sunlight_range", PROPERTY_HINT_NONE, "suffix:W/m²"), "set_rule_range", "get_rule_range", RULE_SUNLIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "sunlight_fade", PROPERTY_HINT_RANGE, "0,200,1,suffix:W/m²"), "set_rule_fade", "get_rule_fade", RULE_SUNLIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "conifer_range"), "set_rule_range", "get_rule_range", RULE_CONIFER);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "conifer_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_CONIFER);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "spray_range"), "set_rule_range", "get_rule_range", RULE_SPRAY);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "spray_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_SPRAY);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "litter_range"), "set_rule_range", "get_rule_range", RULE_LITTER);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "litter_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_LITTER);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "dune_range"), "set_rule_range", "get_rule_range", RULE_DUNE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "dune_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_DUNE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "season_temperature_range", PROPERTY_HINT_NONE, "suffix:°C"), "set_rule_range", "get_rule_range", RULE_SEASON_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "season_temperature_fade", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:°C"), "set_rule_fade", "get_rule_fade", RULE_SEASON_TEMPERATURE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "tide_height_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_TIDE_HEIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "tide_height_fade", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_TIDE_HEIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "coast_exposure_range"), "set_rule_range", "get_rule_range", RULE_COAST_EXPOSURE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "coast_exposure_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_COAST_EXPOSURE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "beach_range"), "set_rule_range", "get_rule_range", RULE_BEACH);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "beach_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_BEACH);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "field_edge_range", PROPERTY_HINT_NONE, "suffix:m"), "set_rule_range", "get_rule_range", RULE_FIELD_EDGE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "field_edge_fade", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:m"), "set_rule_fade", "get_rule_fade", RULE_FIELD_EDGE);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "sand_mobility_range"), "set_rule_range", "get_rule_range", RULE_SAND_MOBILITY);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "sand_mobility_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rule_fade", "get_rule_fade", RULE_SAND_MOBILITY);
	ADD_PROPERTYI(PropertyInfo(Variant::VECTOR2, "burn_age_range", PROPERTY_HINT_NONE, "suffix:yr"), "set_rule_range", "get_rule_range", RULE_BURN_AGE);
	ADD_PROPERTYI(PropertyInfo(Variant::FLOAT, "burn_age_fade", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:yr"), "set_rule_fade", "get_rule_fade", RULE_BURN_AGE);

	BIND_ENUM_CONSTANT(RULE_SLOPE);
	BIND_ENUM_CONSTANT(RULE_HEIGHT);
	BIND_ENUM_CONSTANT(RULE_CURVATURE);
	BIND_ENUM_CONSTANT(RULE_WETNESS);
	BIND_ENUM_CONSTANT(RULE_WATER_DEPTH);
	BIND_ENUM_CONSTANT(RULE_WATER_DISTANCE);
	BIND_ENUM_CONSTANT(RULE_SOIL_DEPTH);
	BIND_ENUM_CONSTANT(RULE_ROCK);
	BIND_ENUM_CONSTANT(RULE_TALUS);
	BIND_ENUM_CONSTANT(RULE_SHADE);
	BIND_ENUM_CONSTANT(RULE_TEMPERATURE);
	BIND_ENUM_CONSTANT(RULE_JANUARY_TEMPERATURE);
	BIND_ENUM_CONSTANT(RULE_PRECIPITATION);
	BIND_ENUM_CONSTANT(RULE_ARIDITY);
	BIND_ENUM_CONSTANT(RULE_SUNLIGHT);
	BIND_ENUM_CONSTANT(RULE_CONIFER);
	BIND_ENUM_CONSTANT(RULE_SPRAY);
	BIND_ENUM_CONSTANT(RULE_LITTER);
	BIND_ENUM_CONSTANT(RULE_DUNE);
	BIND_ENUM_CONSTANT(RULE_SEASON_TEMPERATURE);
	BIND_ENUM_CONSTANT(RULE_TIDE_HEIGHT);
	BIND_ENUM_CONSTANT(RULE_COAST_EXPOSURE);
	BIND_ENUM_CONSTANT(RULE_BEACH);
	BIND_ENUM_CONSTANT(RULE_FIELD_EDGE);
	BIND_ENUM_CONSTANT(RULE_SAND_MOBILITY);
	BIND_ENUM_CONSTANT(RULE_BURN_AGE);
	BIND_ENUM_CONSTANT(RULE_MAX);
	BIND_ENUM_CONSTANT(DEADWOOD_AVOID);
	BIND_ENUM_CONSTANT(DEADWOOD_ON);
	BIND_ENUM_CONSTANT(DEADWOOD_BESIDE);
	BIND_ENUM_CONSTANT(DEADWOOD_IGNORE);
}

void Biome::set_species(const TypedArray<PlantSpecies> &p_species) {
	species = p_species;
	emit_changed();
}

void Biome::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_species", "species"), &Biome::set_species);
	ClassDB::bind_method(D_METHOD("get_species"), &Biome::get_species);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "species", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("PlantSpecies")), "set_species", "get_species");
}

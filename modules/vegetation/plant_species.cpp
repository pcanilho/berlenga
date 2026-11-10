/**************************************************************************/
/*  plant_species.cpp                                                     */
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

float PlantSpecies::get_suitability(const TerrainDataMap3D *p_map, const Vector3 &p_pos) const {
	float w = rule_weight(RULE_SLOPE, Math::rad_to_deg(p_map->get_slope_at(p_pos)));
	if (w <= 0.0f) {
		return 0.0f;
	}
	w *= rule_weight(RULE_WATER_DEPTH, p_map->get_water_depth_at(p_pos));
	w *= rule_weight(RULE_WATER_DISTANCE, p_map->get_water_distance_at(p_pos));
	w *= rule_weight(RULE_WETNESS, p_map->get_wetness_at(p_pos));
	w *= rule_weight(RULE_CURVATURE, p_map->get_curvature_at(p_pos));
	w *= rule_weight(RULE_HEIGHT, p_map->get_height_at(p_pos));
	if (patch_scale > 0.0f) {
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

float PlantSpecies::get_patch_weight(const Vector3 &p_pos) const {
	if (patch_scale <= 0.0f) {
		return 1.0f;
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
	ClassDB::bind_method(D_METHOD("get_footprint"), &PlantSpecies::get_footprint);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &PlantSpecies::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &PlantSpecies::get_density);
	ClassDB::bind_method(D_METHOD("set_scale_range", "range"), &PlantSpecies::set_scale_range);
	ClassDB::bind_method(D_METHOD("get_scale_range"), &PlantSpecies::get_scale_range);
	ClassDB::bind_method(D_METHOD("set_align_to_ground", "align"), &PlantSpecies::set_align_to_ground);
	ClassDB::bind_method(D_METHOD("get_align_to_ground"), &PlantSpecies::get_align_to_ground);
	ClassDB::bind_method(D_METHOD("set_random_yaw", "enable"), &PlantSpecies::set_random_yaw);
	ClassDB::bind_method(D_METHOD("is_random_yaw"), &PlantSpecies::is_random_yaw);
	ClassDB::bind_method(D_METHOD("set_rule_range", "rule", "range"), &PlantSpecies::set_rule_range);
	ClassDB::bind_method(D_METHOD("get_rule_range", "rule"), &PlantSpecies::get_rule_range);
	ClassDB::bind_method(D_METHOD("set_rule_fade", "rule", "fade"), &PlantSpecies::set_rule_fade);
	ClassDB::bind_method(D_METHOD("get_rule_fade", "rule"), &PlantSpecies::get_rule_fade);
	ClassDB::bind_method(D_METHOD("rule_weight", "rule", "value"), &PlantSpecies::rule_weight);
	ClassDB::bind_method(D_METHOD("set_patch_scale", "scale"), &PlantSpecies::set_patch_scale);
	ClassDB::bind_method(D_METHOD("get_patch_scale"), &PlantSpecies::get_patch_scale);
	ClassDB::bind_method(D_METHOD("set_patch_cover", "cover"), &PlantSpecies::set_patch_cover);
	ClassDB::bind_method(D_METHOD("get_patch_cover"), &PlantSpecies::get_patch_cover);
	ClassDB::bind_method(D_METHOD("set_patch_seed", "seed"), &PlantSpecies::set_patch_seed);
	ClassDB::bind_method(D_METHOD("get_patch_seed"), &PlantSpecies::get_patch_seed);
	ClassDB::bind_method(D_METHOD("set_patch_invert", "invert"), &PlantSpecies::set_patch_invert);
	ClassDB::bind_method(D_METHOD("is_patch_invert"), &PlantSpecies::is_patch_invert);
	ClassDB::bind_method(D_METHOD("get_patch_weight", "global_position"), &PlantSpecies::get_patch_weight);
	ClassDB::bind_method(D_METHOD("set_visibility_range", "range"), &PlantSpecies::set_visibility_range);
	ClassDB::bind_method(D_METHOD("get_visibility_range"), &PlantSpecies::get_visibility_range);
	ClassDB::bind_method(D_METHOD("get_suitability_at", "data_map", "global_position"), &PlantSpecies::get_suitability_at);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "meshes", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("Mesh")), "set_meshes", "get_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,50,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "footprint", PROPERTY_HINT_RANGE, "0,50,0.01,or_greater,suffix:m"), "set_footprint", "get_footprint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "scale_range"), "set_scale_range", "get_scale_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_ground", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_ground", "get_align_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_yaw"), "set_random_yaw", "is_random_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,1000,0.5,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_GROUP("Patches", "patch_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_scale", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_patch_scale", "get_patch_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_patch_cover", "get_patch_cover");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patch_seed"), "set_patch_seed", "get_patch_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "patch_invert"), "set_patch_invert", "is_patch_invert");

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

	BIND_ENUM_CONSTANT(RULE_SLOPE);
	BIND_ENUM_CONSTANT(RULE_HEIGHT);
	BIND_ENUM_CONSTANT(RULE_CURVATURE);
	BIND_ENUM_CONSTANT(RULE_WETNESS);
	BIND_ENUM_CONSTANT(RULE_WATER_DEPTH);
	BIND_ENUM_CONSTANT(RULE_WATER_DISTANCE);
	BIND_ENUM_CONSTANT(RULE_MAX);
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

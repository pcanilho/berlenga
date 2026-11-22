/**************************************************************************/
/*  plant_species.h                                                       */
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

#pragma once

#include "core/io/resource.h"
#include "core/variant/typed_array.h"
#include "scene/resources/mesh.h"

class TerrainDataMap3D;

// One kind of plant and where it grows (docs/ideas/vegetation-placement.md). Rules are ranges
// over the TerrainDataMap3D maps with a soft edge; a plant is kept with probability
// density times every rule's weight.
class PlantSpecies : public Resource {
	GDCLASS(PlantSpecies, Resource);

public:
	enum Rule {
		RULE_SLOPE, // Degrees.
		RULE_HEIGHT, // Metres.
		RULE_CURVATURE, // 1/m, positive in hollows.
		RULE_WETNESS, // Topographic wetness index.
		RULE_WATER_DEPTH, // Metres, 0 on land.
		RULE_WATER_DISTANCE, // Metres, negative under water.
		RULE_SOIL_DEPTH, // Metres of soil over bedrock (needs the map's geology_source).
		RULE_ROCK, // Bare rock exposure, 0 to 1 (geology).
		RULE_TALUS, // Scree cover, 0 to 1 (geology).
		RULE_SHADE, // Canopy shade, 0 open sky to 1 dense crown (VegetationField3D, from placed canopy).
		RULE_TEMPERATURE, // Mean annual temperature, C (needs the map's climate).
		RULE_JANUARY_TEMPERATURE, // January mean, C: frost hardiness.
		RULE_PRECIPITATION, // mm a year.
		RULE_ARIDITY, // Precipitation over potential evapotranspiration.
		RULE_SUNLIGHT, // Annual mean shortwave, W/m2: sunny slopes against shaded ones.
		RULE_CONIFER, // Share of the canopy overhead that is needle-leaved, 0 broadleaf to 1 conifer (VegetationField3D).
		RULE_SPRAY, // Waterfall spray potential, 0 to 1 (geology from a TerrainGenerator3D).
		RULE_LITTER, // Leaf fall from the placed canopy, 0 none to 1 under closed crowns, drifting past the crowns (VegetationField3D).
		RULE_DUNE, // Coastal dune stability (TerrainGenerator3D marram cover), 0 bare to 1 fixed; -1 off the dune sand. Only where the map's geology_source has dunes.
		RULE_SEASON_TEMPERATURE, // Growing season mean, C (Paulsen and Koerner 2014: days over 0.9 C; under 94 of them counts as -10). Trees stop at 6.4 C.
		RULE_TIDE_HEIGHT, // Metres above the sea (the geology source's sea). Coast rules need a geology source with a sea; without one a species using them is not placed.
		RULE_COAST_EXPOSURE, // 0 sheltered to 1 open to the sea's waves (TerrainGenerator3D.get_coast_exposure_at); 0 far from the sea.
		RULE_BEACH, // Beach mask, 1 on the beach face fading over the backshore, 0 inland (TerrainGenerator3D.get_beach_at).
		RULE_FIELD_EDGE, // Metres from the nearest crop field's edge (GrassField3D.fields), negative inside the field. A species using it may grow in the fields; without fields it is not placed.
		RULE_SAND_MOBILITY, // Desert sand seas (TerrainGenerator3D.sand_sea_rect): 0 stable interdune ground to 1 moving crest or slip face; -1 off the sand seas. A species using it grows on their loose sand.
		RULE_BURN_AGE, // Years since the ground last burnt (WorldFire.burn_age_at); -1 never. A species using it grows only on ground that burnt (the burn presets).
		RULE_SPRING, // Desert springs (DesertSprings): 0 dry to 1 at an oasis spring's water; -1 off the sand seas. A species using it grows on their sand.
		RULE_POOL_DISTANCE, // Metres from the nearest oasis spring pool's shore (VegetationField3D.spring_pools), negative in the pool. A species using it grows only round those pools, on their sand too.
		RULE_STAND_AGE, // Years since the last stand-replacing disturbance (WorldStand.stand_age_at); -1 old growth, which only ranges reaching -1 or 10000 years take.
		RULE_DISTURBANCE, // The kind behind the stand age (WorldStand.disturbance_at): 0 none, 1 fire, 2 wind, 3 beetle. Pick one kind with (k, k).
		RULE_FOREST_EDGE, // Metres from the edge of the field's forest canopy, positive inside, negative in the open; -1000 with no canopy (VegetationField3D).
		RULE_BROWSE, // Deer browsing pressure, 0 none to 1 heavy (WorldStand.browse_at); 0 where no browsing is known.
		RULE_ROOTING, // Years since boar last rooted the ground (WorldStand.rooting_age_at); -1 never, which only ranges reaching -1 or 10000 years take.
		RULE_LITTER_DRIFT, // How the wind gathers fallen leaves (VegetationField3D): 1 as they fell, up to 2.5 in hollows and against trunks, logs and rocks, down to 0.15 on crests.
		RULE_FAIRY_RING, // Metres from the nearest meadow fairy ring's line (fairy_rings.h), + outside; 1000 with none or in a broken ring's gap.
		RULE_HIGH_WATER, // Metres of the ground over the last flood's peak surface (WorldWater.high_water_at), - below it; 1000 where no flood is known.
		RULE_FLOOD_AGE, // Years since that flood's peak (WorldWater.high_water_at); 10000 where no flood is known.
		RULE_BANK_EDGE, // Metres the drawn ground (heightfield plus WorldGround.relief_at) drops past a steep (45 degree) edge within 1 m of the point, the steepest way; 0 with no edge near (VegetationField3D: a species using it stands on the edge, mesh +X over the drop).
		RULE_MAX,
	};
	// How the species meets fallen wood placed in the field (meshes with tree_deadwood_axis).
	enum DeadwoodRule {
		DEADWOOD_AVOID, // Kept off a log's footprint.
		DEADWOOD_ON, // Only on the upper surface of logs at least deadwood_min_radius thick.
		DEADWOOD_BESIDE, // Only within deadwood_band of a log's side.
		DEADWOOD_IGNORE,
	};

private:
	TypedArray<Mesh> meshes;
	float spacing = 2.0f;
	float footprint = 0.5f;
	float sward_shade = 0.0f; // How much of the grass a stand of it shades out under it (bracken, bramble).
	float trample_resistance = 0.0f; // Passes a year on a worn path that halve its cover (Cole 1995); 0: not trampled.
	float burn_recovery = 0.0f; // Years to full cover after a fire; 0: fire does not thin it.
	float detail_size = 0.0f; // m; > 0: a small item whose range the field takes from its pixel reach (vegetation_detail.h).
public:
	// Ground litter colours for undergrowth (linear): dead bracken fronds, sRGB about (150, 100, 60);
	// bramble's leaf litter over soil, sRGB about (115, 92, 64), not covering all of it.
	static inline const Color BRACKEN_LITTER = Color(0.30f, 0.13f, 0.045f, 0.95f);
	static inline const Color BRAMBLE_LITTER = Color(0.17f, 0.105f, 0.05f, 0.7f);

private:
	Color litter_color = Color(0, 0, 0, 0); // Its own litter on the ground under its stands (rgb) and how much it covers (a).
	float density = 1.0f;
	Vector2 scale_range = Vector2(0.8f, 1.2f);
	float align_to_ground = 0.0f;
	bool random_yaw = true;
	bool floating = false; // Rides the water surface (water lilies), not the ground under it.
	bool stone_top = false; // Grows only on the tops of stones (VegetationField3D.stone_tops): moss cushions.
	// Patches: clover beds, flower drifts. Value noise over the ground keeps the species to
	// patches about patch_scale metres across, covering patch_cover of the ground.
	float patch_scale = 0.0f; // 0: no patches.
	float patch_cover = 0.5f;
	int patch_seed = 0;
	int mask_channel = -1; // VegetationField3D.density_mask channel that scales this species (0 R .. 3 A), -1 none.
	bool mask_invert = false; // Grow where the mask is low instead.
	// A stand's margin (patch species): plants within margin_width of the patch edge grow smaller,
	// down to margin_scale of their size at the very edge, and lean out towards the open by up to
	// margin_lean degrees. Bracken's advancing front (Watt 1947).
	float rock_foot = 0.0f; // How much more it grows at a rock's foot (obstacles): sheltered, moist ground.
	float stone_gap = 0.0f; // 0..1: how strictly it keeps to the gaps between stones (obstacles); 1: nowhere else.
	float stone_top_damp = 0.0f; // -1..1, stone_top species: +1 keeps to damp, shaded tops, -1 to dry, sunlit ones.
	// stone_top species: the band of the top's height over the water surface (m) it keeps to; off
	// while x >= y. Aquatic mosses under and just over the water, dry mosses above the flood line.
	Vector2 stone_top_water = Vector2();
	bool follow_flow = false; // Turned so its +X points downstream (downhill), not a random yaw.
	float wind_frontal_area = -1.0f; // m2 a plant shows the wind at scale 1 (sand shelter); negative: from its mesh bounds.
	float margin_width = 0.0f;
	float margin_scale = 1.0f;
	float margin_lean = 0.0f;
	bool patch_invert = false; // Grow between the patches instead (same scale, cover and seed: the exact complement).
	float patch_ring = 0.0f; // > 0: grow only along the patches' edges, in a band this wide (m): fairy rings.
	// Coupes (coppice with standards): with coupe_rotation > 0 the patches are straight-edged
	// compartments about patch_scale across (a Voronoi cell each), each cut in its own year of a
	// coupe_rotation-year cycle, and the species grows only in coupes whose years since the cut lie
	// in coupe_ages (inclusive). About half the coupe boundaries are rides ride_width wide, kept
	// clear. Rackham 2006; Peterken 1993; Buckley 1992.
	int coupe_rotation = 0;
	Vector2i coupe_ages;
	float ride_width = 0.0f;
	float visibility_range = 0.0f; // 0: always drawn.
	// Near detail: heavier meshes (same variant order as meshes) drawn within near_range,
	// meshes beyond it, with a dithered crossfade over the last tenth.
	TypedArray<Mesh> near_meshes;
	float near_range = 0.0f; // 0: no near swap.
	// Near meshes on demand instead (heavy meshes built only for plants that come near): an object
	// with request_near_mesh(key: int, variant: int) -> Mesh, null while it is still building.
	// Used when near_meshes is empty and near_range > 0; VegetationField3D asks per cell within
	// 1.2 x near_range and drops the cell beyond 1.5 x, so the source can free unused meshes.
	ObjectID near_mesh_source;
	int near_mesh_key = 0;
	Color far_color = Color(0, 0, 0, 0); // The plant seen from far above (rgb) and how much ground it hides (a).
	// A canopy species shades what grows under it: crown radius (times the plant's scale) and
	// the share of light its crown stops.
	float canopy_radius = 0.0f; // 0: casts no shade.
	float canopy_opacity = 0.85f;
	bool canopy_conifer = false; // The crown drops needles, not leaves: picks the floor under it.
	Vector2 ranges[RULE_MAX];
	float fades[RULE_MAX] = {};
	DeadwoodRule deadwood_rule = DEADWOOD_AVOID;
	float deadwood_band = 0.6f; // Metres from the log's side (DEADWOOD_BESIDE).
	float deadwood_min_radius = 0.08f; // The thinnest log it grows on (DEADWOOD_ON), metres.
	bool deadwood_uphill = false; // DEADWOOD_BESIDE on the uphill side only (trapped leaves).
	bool deadwood_from_wood = false; // DEADWOOD_BESIDE: comes up out of the wood's side (wood-decay fungi), facing out.

protected:
	static void _bind_methods();

public:
	void set_meshes(const TypedArray<Mesh> &p_meshes);
	TypedArray<Mesh> get_meshes() const { return meshes; }
	void set_patch_scale(float p_scale) { patch_scale = MAX(p_scale, 0.0f); }
	float get_patch_scale() const { return patch_scale; }
	void set_patch_cover(float p_cover) { patch_cover = CLAMP(p_cover, 0.0f, 1.0f); }
	float get_patch_cover() const { return patch_cover; }
	void set_patch_seed(int p_seed) { patch_seed = p_seed; }
	int get_patch_seed() const { return patch_seed; }
	void set_mask_channel(int p_channel) { mask_channel = CLAMP(p_channel, -1, 3); }
	int get_mask_channel() const { return mask_channel; }
	void set_mask_invert(bool p_invert) { mask_invert = p_invert; }
	bool is_mask_invert() const { return mask_invert; }
	void set_patch_invert(bool p_invert) { patch_invert = p_invert; }
	bool is_patch_invert() const { return patch_invert; }
	void set_patch_ring(float p_width) { patch_ring = CLAMP(p_width, 0.0f, 5.0f); emit_changed(); }
	float get_patch_ring() const { return patch_ring; }
	void set_coupe_rotation(int p_years) { coupe_rotation = MAX(p_years, 0); emit_changed(); }
	int get_coupe_rotation() const { return coupe_rotation; }
	void set_coupe_ages(const Vector2i &p_ages) { coupe_ages = p_ages; emit_changed(); }
	Vector2i get_coupe_ages() const { return coupe_ages; }
	void set_ride_width(float p_width) { ride_width = MAX(p_width, 0.0f); emit_changed(); }
	float get_ride_width() const { return ride_width; }
	// The coupe at a point: x years since its cut (-1 without coupes), y the distance to its edge
	// in metres (to the ride's edge where the edge is a ride; negative on the ride), z 1 on a ride.
	Vector3 get_coupe_at(const Vector3 &p_pos, Vector2 *r_out = nullptr) const;
	Vector3 get_coupe_at_bind(const Vector3 &p_pos) const { return get_coupe_at(p_pos); }
	float get_patch_weight(const Vector3 &p_pos) const;
	// How far inside its patch a point is, in metres (negative outside), and the way out of it on
	// the ground (unit xz) when asked. 1e6 for species without patches.
	float get_patch_depth(const Vector3 &p_pos, Vector2 *r_out = nullptr) const;
	float get_patch_depth_at(const Vector3 &p_pos) const { return get_patch_depth(p_pos); }
	void set_rock_foot(float p_amount) { rock_foot = CLAMP(p_amount, 0.0f, 8.0f); emit_changed(); }
	float get_rock_foot() const { return rock_foot; }
	void set_stone_gap(float p_amount) { stone_gap = CLAMP(p_amount, 0.0f, 1.0f); emit_changed(); }
	float get_stone_gap() const { return stone_gap; }
	static constexpr float STONE_GAP_REACH = 0.12f; // Metres: the e-folding distance to a second stone's edge.
	static constexpr float STONE_EDGE_HUG = 0.08f; // Metres from the nearest stone's edge a crevice plant roots at most in full.
	static constexpr float STONE_EDGE_REACH = 0.2f; // Metres from it where it no longer roots.
	// The gap's weight at a point whose nearest stone edge is p_first away and second-nearest
	// p_second: a crevice plant roots against a stone (full within STONE_EDGE_HUG, none past
	// STONE_EDGE_REACH) in a gap with a second stone close by (falling off exponentially with that
	// stone's distance, reaching 0 at four reaches), scaled in by stone_gap.
	float stone_gap_weight(float p_first, float p_second) const {
		const float tail = 0.018315639f; // exp(-4)
		const float g = MAX(Math::exp(-MAX(p_second, 0.0f) / STONE_GAP_REACH) - tail, 0.0f) / (1.0f - tail);
		const float hug = CLAMP((STONE_EDGE_REACH - p_first) / (STONE_EDGE_REACH - STONE_EDGE_HUG), 0.0f, 1.0f);
		return Math::lerp(1.0f, g * hug, stone_gap);
	}
	static constexpr float ROCK_FOOT_REACH = 0.5f; // Metres: the e-folding distance of the rock foot's shelter.
	// The rock foot's weight at a distance past a rock's edge (clearance, metres).
	float rock_foot_weight(float p_clearance) const { return 1.0f + rock_foot * Math::exp(-MAX(p_clearance, 0.0f) / ROCK_FOOT_REACH); }
	void set_margin_width(float p_width) { margin_width = MAX(p_width, 0.0f); emit_changed(); }
	float get_margin_width() const { return margin_width; }
	void set_margin_scale(float p_scale) { margin_scale = CLAMP(p_scale, 0.05f, 1.0f); emit_changed(); }
	float get_margin_scale() const { return margin_scale; }
	void set_margin_lean(float p_degrees) { margin_lean = CLAMP(p_degrees, 0.0f, 60.0f); emit_changed(); }
	float get_margin_lean() const { return margin_lean; }
	// The margin's effect at a depth: 0 deep inside, 1 at the edge.
	float margin_at(float p_depth) const { return margin_width > 0.0f ? 1.0f - Math::smoothstep(0.0f, 1.0f, CLAMP(p_depth / margin_width, 0.0f, 1.0f)) : 0.0f; }
	void set_near_meshes(const TypedArray<Mesh> &p_meshes) { near_meshes = p_meshes; }
	TypedArray<Mesh> get_near_meshes() const { return near_meshes; }
	void set_near_range(float p_range) { near_range = MAX(p_range, 0.0f); }
	float get_near_range() const { return near_range; }
	void set_near_mesh_source(Object *p_source) { near_mesh_source = p_source ? p_source->get_instance_id() : ObjectID(); }
	Object *get_near_mesh_source() const { return ObjectDB::get_instance(near_mesh_source); }
	void set_near_mesh_key(int p_key) { near_mesh_key = p_key; }
	int get_near_mesh_key() const { return near_mesh_key; }
	void set_visibility_range(float p_range) { visibility_range = MAX(p_range, 0.0f); }
	float get_visibility_range() const { return visibility_range; }
	void set_far_color(const Color &p_color) { far_color = p_color; }
	Color get_far_color() const { return far_color; }
	void set_canopy_radius(float p_radius) { canopy_radius = MAX(p_radius, 0.0f); }
	float get_canopy_radius() const { return canopy_radius; }
	void set_canopy_opacity(float p_opacity) { canopy_opacity = CLAMP(p_opacity, 0.0f, 1.0f); }
	float get_canopy_opacity() const { return canopy_opacity; }
	void set_canopy_conifer(bool p_conifer);
	bool is_canopy_conifer() const { return canopy_conifer; }
	bool uses_conifer() const { return ranges[RULE_CONIFER].x > 0.0f || ranges[RULE_CONIFER].y < 1.0f; }
	// True when the shade rule is narrower than "anywhere", so placement must compute shade.
	bool uses_shade() const { return ranges[RULE_SHADE].x > 0.0f || ranges[RULE_SHADE].y < 1.0f; }
	bool uses_litter() const { return ranges[RULE_LITTER].x > 0.0f || ranges[RULE_LITTER].y < 1.0f; }
	// The growing season's mean temperature from a January and July mean on the annual harmonic:
	// the days with a mean of at least 0.9 C, -10 when there are fewer than 94 of them (Paulsen and
	// Koerner 2014, the treeline model: trees need 6.4 C over at least 94 days).
	static float season_temperature(float p_january, float p_july);
	bool uses_field_edge() const { return ranges[RULE_FIELD_EDGE].x > -100000.0f || ranges[RULE_FIELD_EDGE].y < 100000.0f; }
	bool uses_dune() const { return ranges[RULE_DUNE].x > -1.0f || ranges[RULE_DUNE].y < 1.0f; }
	bool uses_burn_age() const { return ranges[RULE_BURN_AGE].x > -1.0f || ranges[RULE_BURN_AGE].y < 1000.0f; }
	// The stand rules, or a succession species' own stage (TreeGenerator.make_succession meta "succession_age" and
	// "succession_kind", taken where the rules are left at their defaults).
	bool uses_stand() const;
	void get_stand_rules(Vector2 &r_age, float &r_age_fade, Vector2 &r_kind, float &r_kind_fade) const;
	// Weight of a stand (years, -1 old growth; disturbance kind) under those rules.
	static float stand_weight(const Vector2 &p_age, float p_age_fade, const Vector2 &p_kinds, float p_kind_fade, float p_years, int p_kind);
	// The forest edge rule, or meta "edge_distance" (Vector2 m; TreeGenerator.make_edge) where it is left at its default.
	bool uses_forest_edge() const;
	bool uses_browse() const { return ranges[RULE_BROWSE].x > 0.0f || ranges[RULE_BROWSE].y < 1.0f; }
	bool uses_bank_edge() const { return ranges[RULE_BANK_EDGE].x > -10000.0f || ranges[RULE_BANK_EDGE].y < 10000.0f; }
	bool uses_high_water() const { return ranges[RULE_HIGH_WATER].x > -10000.0f || ranges[RULE_HIGH_WATER].y < 10000.0f || ranges[RULE_FLOOD_AGE].x > 0.0f || ranges[RULE_FLOOD_AGE].y < 10000.0f; }
	bool uses_fairy_ring() const { return ranges[RULE_FAIRY_RING].x > -10000.0f || ranges[RULE_FAIRY_RING].y < 10000.0f; }
	bool uses_litter_drift() const { return ranges[RULE_LITTER_DRIFT].x > 0.0f || ranges[RULE_LITTER_DRIFT].y < 100.0f; }
	bool uses_rooting() const { return ranges[RULE_ROOTING].x > -1.0f || ranges[RULE_ROOTING].y < 10000.0f; }
	// Weight of a rooting age (years, -1 never rooted) under a rooting rule (range x, y; fade z).
	static float rooting_weight(const Vector3 &p_rule, float p_years) {
		if (p_years < 0.0f) {
			return (p_rule.x <= -1.0f || p_rule.y >= 10000.0f) ? 1.0f : 0.0f;
		}
		return p_rule.z <= 0.0f ? ((p_years >= p_rule.x && p_years <= p_rule.y) ? 1.0f : 0.0f) : Math::smoothstep(float(p_rule.x - p_rule.z), float(p_rule.x), p_years) * (1.0f - Math::smoothstep(float(p_rule.y), float(p_rule.y + p_rule.z), p_years));
	}
	Vector3 get_rooting_rule() const { return Vector3(ranges[RULE_ROOTING].x, ranges[RULE_ROOTING].y, fades[RULE_ROOTING]); }
	void get_forest_edge_rule(Vector2 &r_range, float &r_fade) const;
	bool uses_pool() const { return ranges[RULE_POOL_DISTANCE].x > -100000.0f || ranges[RULE_POOL_DISTANCE].y < 100000.0f; }
	bool uses_spring() const { return ranges[RULE_SPRING].x > -1.0f || ranges[RULE_SPRING].y < 1.0f; }
	bool uses_sand_mobility() const { return ranges[RULE_SAND_MOBILITY].x > -1.0f || ranges[RULE_SAND_MOBILITY].y < 1.0f; }
	// True when a coast rule (tide height, exposure, beach) is narrower than "anywhere".
	bool uses_coast() const {
		return ranges[RULE_TIDE_HEIGHT].x > -10000.0f || ranges[RULE_TIDE_HEIGHT].y < 10000.0f ||
				ranges[RULE_COAST_EXPOSURE].x > 0.0f || ranges[RULE_COAST_EXPOSURE].y < 1.0f ||
				ranges[RULE_BEACH].x > 0.0f || ranges[RULE_BEACH].y < 1.0f;
	}
	void set_spacing(float p_spacing);
	float get_spacing() const { return spacing; }
	void set_footprint(float p_footprint);
	float get_footprint() const { return footprint; }
	void set_sward_shade(float p_shade);
	float get_sward_shade() const { return sward_shade; }
	void set_trample_resistance(float p_passes) { trample_resistance = MAX(p_passes, 0.0f); }
	float get_trample_resistance() const { return trample_resistance; }
	void set_burn_recovery(float p_years) { burn_recovery = MAX(p_years, 0.0f); }
	float get_burn_recovery() const { return burn_recovery; }
	// The share of its cover back on ground that burnt p_age years ago (-1: never; 1): none on a fresh burn, rising to
	// full over burn_recovery years (smoothstep: slow at first, then closing in).
	float burn_cover(float p_age) const {
		if (burn_recovery <= 0.0f || p_age < 0.0f) {
			return 1.0f;
		}
		return Math::smoothstep(0.0f, burn_recovery, p_age);
	}
	void set_detail_size(float p_size) { detail_size = MAX(p_size, 0.0f); }
	float get_detail_size() const { return detail_size; }
	// Trampling (Cole 1995): cover falls along a log-logistic curve of the passes (slope 2), halved
	// at trample_resistance passes a year and near bare at ten times that; the plants left are shorter and flatter, halfway to their
	// trodden height (0.45) at the same passes.
	float trample_cover(float p_passes) const {
		if (trample_resistance <= 0.0f || p_passes <= 0.0f) {
			return 1.0f;
		}
		const float x = p_passes / trample_resistance;
		// Past about four times the resistance nothing stands: the tread is bare.
		return (1.0f - Math::smoothstep(2.5f, 5.0f, x)) / (1.0f + x * x);
	}
	float trample_scale(float p_passes) const {
		if (trample_resistance <= 0.0f || p_passes <= 0.0f) {
			return 1.0f;
		}
		return Math::lerp(1.0f, 0.45f, p_passes / (p_passes + trample_resistance));
	}
	void set_litter_color(const Color &p_color);
	Color get_litter_color() const { return litter_color; }
	void set_density(float p_density);
	float get_density() const { return density; }
	void set_scale_range(const Vector2 &p_range);
	Vector2 get_scale_range() const { return scale_range; }
	void set_align_to_ground(float p_align);
	float get_align_to_ground() const { return align_to_ground; }
	void set_random_yaw(bool p_enable);
	bool is_random_yaw() const { return random_yaw; }
	void set_floating(bool p_floating);
	bool is_floating() const { return floating; }
	void set_stone_top(bool p_on) { stone_top = p_on; emit_changed(); }
	bool is_stone_top() const { return stone_top; }
	void set_stone_top_damp(float p_amount) { stone_top_damp = CLAMP(p_amount, -1.0f, 1.0f); emit_changed(); }
	float get_stone_top_damp() const { return stone_top_damp; }
	void set_stone_top_water(const Vector2 &p_band) {
		stone_top_water = p_band;
		emit_changed();
	}
	Vector2 get_stone_top_water() const { return stone_top_water; }
	bool has_stone_top_water() const { return stone_top_water.x < stone_top_water.y; }
	// The weight of a stone top p_h metres over the water: 1 inside the band, fading over 4 cm.
	float stone_top_water_weight(float p_h) const {
		if (!has_stone_top_water()) {
			return 1.0f;
		}
		const float fade = 0.04f;
		return Math::smoothstep(float(stone_top_water.x) - fade, float(stone_top_water.x), p_h) * (1.0f - Math::smoothstep(float(stone_top_water.y), float(stone_top_water.y) + fade, p_h));
	}
	void set_follow_flow(bool p_on) {
		follow_flow = p_on;
		emit_changed();
	}
	bool is_follow_flow() const { return follow_flow; }
	void set_wind_frontal_area(float p_area) {
		wind_frontal_area = p_area;
		emit_changed();
	}
	float get_wind_frontal_area() const { return wind_frontal_area; }
	// The weight of a stone top whose dampness is p_damp (0 dry and sunlit .. 1 damp and shaded).
	float stone_top_damp_weight(float p_damp) const {
		return stone_top_damp >= 0.0f ? Math::lerp(1.0f, p_damp, stone_top_damp) : Math::lerp(1.0f, 1.0f - p_damp, -stone_top_damp);
	}

	void set_deadwood_rule(DeadwoodRule p_rule) { deadwood_rule = p_rule; }
	DeadwoodRule get_deadwood_rule() const { return deadwood_rule; }
	void set_deadwood_band(float p_band) { deadwood_band = MAX(p_band, 0.01f); }
	float get_deadwood_band() const { return deadwood_band; }
	void set_deadwood_min_radius(float p_radius) { deadwood_min_radius = MAX(p_radius, 0.0f); }
	float get_deadwood_min_radius() const { return deadwood_min_radius; }
	void set_deadwood_uphill(bool p_uphill) { deadwood_uphill = p_uphill; }
	bool is_deadwood_uphill() const { return deadwood_uphill; }
	void set_deadwood_from_wood(bool p_from) { deadwood_from_wood = p_from; }
	bool is_deadwood_from_wood() const { return deadwood_from_wood; }

	void set_rule_range(Rule p_rule, const Vector2 &p_range);
	Vector2 get_rule_range(Rule p_rule) const;
	void set_rule_fade(Rule p_rule, float p_fade);
	float get_rule_fade(Rule p_rule) const;

	// Weight of one value against one rule: 1 inside the range, smoothstep to 0 over the fade.
	float rule_weight(Rule p_rule, float p_value) const;
	// Product of every rule's weight at a point of the map.
	float get_suitability(const TerrainDataMap3D *p_map, const Vector3 &p_global_position, bool p_patches = true) const;
	float get_suitability_at(Object *p_map, const Vector3 &p_global_position) const;

	PlantSpecies();
};

VARIANT_ENUM_CAST(PlantSpecies::Rule);
VARIANT_ENUM_CAST(PlantSpecies::DeadwoodRule);

// Plant species in priority order: earlier species are placed first and later ones keep clear
// of their footprints (trees, then bushes, then ground cover).
class Biome : public Resource {
	GDCLASS(Biome, Resource);

	TypedArray<PlantSpecies> species;

protected:
	static void _bind_methods();

public:
	void set_species(const TypedArray<PlantSpecies> &p_species);
	TypedArray<PlantSpecies> get_species() const { return species; }
};

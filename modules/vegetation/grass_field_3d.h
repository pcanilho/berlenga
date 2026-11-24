/**************************************************************************/
/*  grass_field_3d.h                                                      */
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

#include "desert_springs.h"
#include "plant_species.h"
#include "vegetation_holes.h"
#include "vegetation_obstacles.h"
#include "vegetation_paths.h"
#include "water_current_bend.h"

#include "modules/world/world_contracts.h"

#include "core/io/image.h"
#include "core/math/geometry_2d.h"
#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MultiMeshInstance3D;
class MeshInstance3D;
class TerrainDataMap3D;

// Grass blades around the camera (docs/ideas/grass.md), after Ghost of Tsushima (Wohllaib,
// GDC 2021): blades on a jittered grid seeded by world position, clumps that share height,
// lean and colour, a quadratic Bezier blade bent by the wind in the vertex shader, density
// thinned by rank with distance and blades widened to keep coverage. Tiles around the camera
// are built on the CPU when they come into range, one MultiMesh each.
class GrassField3D : public Node3D, public WorldPlantColorProvider {
	GDCLASS(GrassField3D, Node3D);

public:
	// WorldPlantColorProvider: the sward over the ground, its far colour by the ground map's cover.
	Color contract_plant_albedo_at(const Vector3 &p_position) const override;

	// A cereal grown instead of grass (vegetation.md, farmland stage 1): each record a tiller in drill
	// rows, its look set by the growth stage.
	enum Crop {
		CROP_NONE,
		CROP_WHEAT,
		CROP_BARLEY,
		CROP_OATS,
		CROP_RYE,
	};
	// Every crop node's fields, for the meadow grass and plants to keep out of (a copy, safe to read
	// on worker threads).
	struct CropFieldMask {
		struct Item {
			Rect2 bounds;
			PackedVector2Array polygon;
		};
		LocalVector<Item> items;
		// The hedges round them (GrassField3D.hedges): runs of segments, a straight line each, whose
		// core nothing else grows in.
		struct Hedge {
			Vector2 a, b;
			float half = 0.7f;
			Rect2 bounds;
		};
		LocalVector<Hedge> hedges;
		bool is_empty() const { return items.is_empty(); }
		bool any_in(const Rect2 &p_rect) const {
			for (const Item &it : items) {
				if (it.bounds.intersects(p_rect)) {
					return true;
				}
			}
			for (const Hedge &h : hedges) {
				if (h.bounds.intersects(p_rect)) {
					return true;
				}
			}
			return false;
		}
		bool in_hedge(const Vector2 &p_q) const {
			for (const Hedge &h : hedges) {
				if (!h.bounds.has_point(p_q)) {
					continue;
				}
				const Vector2 ab = h.b - h.a;
				const float t = CLAMP((p_q - h.a).dot(ab) / MAX(ab.length_squared(), 1e-8f), 0.0f, 1.0f);
				if (p_q.distance_to(h.a + ab * t) < h.half) {
					return true;
				}
			}
			return false;
		}
		// In a field, or in a hedge's core.
		bool contains(const Vector2 &p_q) const {
			for (const Item &it : items) {
				if (it.bounds.has_point(p_q) && Geometry2D::is_point_in_polygon(p_q, it.polygon)) {
					return true;
				}
			}
			return in_hedge(p_q);
		}		// Metres to the nearest field's edge, negative inside a field; p_reach where none is nearer.
		float edge_distance(const Vector2 &p_q, float p_reach) const {
			float best = p_reach;
			bool inside = false;
			for (const Item &it : items) {
				if (!it.bounds.grow(p_reach).has_point(p_q)) {
					continue;
				}
				const int n = it.polygon.size();
				const Vector2 *pts = it.polygon.ptr();
				for (int i = 0; i < n; i++) {
					const Vector2 a = pts[i];
					const Vector2 b = pts[(i + 1) % n];
					const Vector2 ab = b - a;
					const float t = CLAMP((p_q - a).dot(ab) / MAX(ab.length_squared(), 1e-8f), 0.0f, 1.0f);
					best = MIN(best, p_q.distance_to(a + ab * t));
				}
				inside = inside || (it.bounds.has_point(p_q) && Geometry2D::is_point_in_polygon(p_q, it.polygon));
			}
			return inside ? -best : best;
		}
	};

private:

	struct Tile {
		MultiMeshInstance3D *mmi = nullptr;
		int lod = 0;
		uint32_t generation = 0; // Built under this rebuild generation; older ones are replaced, nearest first.
	};
	uint32_t generation = 0; // Bumped by rebuild(): the tiles stay drawn until their successors are built.

	NodePath data_map;
	NodePath canopy_field; // A VegetationField3D whose crowns shade the grass (rules.shade_range).
	PackedVector4Array obstacles; // Rocks, (x, ground y, z, radius at the soil): no blade grows inside one.
	VegetationObstacles obstacle_grid; // obstacles, the mounds and the rock_field's stones fetched so far.
	// A RockField3D whose stones the blades keep out of, fetched round each tile and far ring tile before it is built
	// (get_obstacles), in place of (or as well as) the obstacles array.
	NodePath rock_field;
	float rock_obstacle_min = 0.25f;
	float rock_footprint_max = 32.0f;
	VegetationRockCache rock_cache;
	void _rebuild_obstacle_grid();
	void _rocks_for(const Rect2 &p_rect); // Main thread: fetches the rock_field's stones that reach into p_rect.
	Array paths; // Worn paths (VegetationPaths): the sward thins and shortens towards the trodden line.
	VegetationPaths path_grid;
	float trample_resistance = 300.0f; // Passes a year that halve the sward's cover (Cole 1995: grasses resist).
	float _trample_cover(float p_passes) const;
	float _trample_scale(float p_passes) const;
	VegetationHoles holes; // Openings in the ground (cave mouths, the data map's generator): no blade there.
	ObjectID hole_source;
	void _follow_holes(TerrainDataMap3D *p_map);
	void _on_holes_changed(const Rect2 &p_rect);
	bool _blade_in_hole(const TerrainDataMap3D *p_map, float p_x, float p_z, float p_ground_y) const;
	NodePath undergrowth_field; // A VegetationField3D whose bracken and bramble stands shade the sward out (PlantSpecies.sward_shade).
	Ref<PlantSpecies> rules;
	float density = 220.0f; // Blades per square metre, near the camera (fine verge blades: denser).
	Vector2 blade_height = Vector2(0.2f, 0.5f);
	float blade_width = 0.018f; // Cocksfoot and false oat-grass leaves are 5 to 10 mm: wider read as cards.
	float clump_size = 1.2f;
	float lean = 0.35f;
	Color base_color = Color(0.1f, 0.2f, 0.05f);
	Color tip_color = Color(0.36f, 0.44f, 0.2f); // Summer grey-green, not spring lime.
	Color dry_color = Color(0.55f, 0.48f, 0.28f);
	float dryness = 0.2f;
	// The year (docs/ideas/vegetation-seasons.md): a WeatherMap3D whose season browns the sward in
	// drought and hard cold. season_green 1: as dryness says; 0: all thatch.
	NodePath season_source;
	float season_response = 1.0f;
	float season_green = 1.0f;
	float season_baked_dryness = -1.0f; // The effective dryness of the last far colour bake.
	int season_wait = 0;
	bool ground_published = false; // This field's _bake_ground() set the grass_ground_* globals.
	// The ground map as published (R: the sward's cover) and its rect, for WorldGround.albedo_at (the cover's colour).
	Ref<Image> ground_image;
	Vector4 ground_image_rect;
	bool plant_color_registered = false;
	float _dryness() const { return CLAMP(dryness + (1.0f - dryness) * (1.0f - season_green) * season_response, 0.0f, 1.0f); }
	// Summer dormancy and winter kill take whole blades down to the crown (Volaire and Norton 2006),
	// not only their tips: the share of blades dead from the root.
	float _dormant() const { return CLAMP((1.0f - season_green) * season_response, 0.0f, 1.0f); }
	// The world's weather turns the sward through the year (WorldContracts::weather), with a season_source or, by
	// default, without one (follow_weather): one weather for every system.
	bool follow_weather = true;
	bool _follows_season() const { return !season_source.is_empty() || follow_weather; }
	void _season_step();
	float sun_mean = 0.0f; // VegetationLight::sun_mean() of the data map.
	float color_variation = 0.5f;
	float radius = 40.0f;
	float tile_size = 8.0f;
	Vector2 fallback_wind = Vector2(2.0f, 0.5f);
	float wind_response = 0.12f;
	int max_tiles_per_frame = 12;
	int blade_segments = 4;
	float view_facing = 0.6f;
	float fibre_shading = 0.8f;
	// Still shots (scene benches on the cloud's memory): once the field round the camera is complete it stops
	// updating (no tile, ring or shell rebuilds, no trample) until the camera moves still_shot_move metres.
	bool still_shot = false;
	int still_quiet = 0; // Frames in a row with nothing left to build.
	Vector3 still_at;
	bool still_frozen = false;
	uint32_t still_generation = 0;
	int last_built = 0; // Near tiles built by the last _update_tiles(). // ANISOTROPY of the blades' specular, spread across the strands (docs/ideas/grass.md, "Fibre shading").
	// Ground under the grass takes its colour (alpha: strength), through the grass_ground_* globals.
	Color ground_tint = Color(0.05f, 0.09f, 0.025f, 0.85f);
	bool ground_tint_enabled = true;
	Vector4 fade_band; // Last centre and band sent to the shaders.
	static GrassField3D *fade_owner; // The field whose band the ground shader reads.
	// The widest other field with a ground tint (an undercoat under a patchy tall sward): past its
	// own band the ground takes its far colour, and past the fade owner's band the two far colours
	// mix by the tall sward's cover share.
	static GrassField3D *under_owner;
	void _publish_under();
	void _drop_under();
	bool ground_baked = false;
	class VegetationField3D *_get_canopy(const PlantSpecies *p_rules) const;
	class VegetationField3D *_get_undergrowth() const;
	bool _tile_has_obstacles(float p_x0, float p_z0) const;
	Ref<ImageTexture> ground_texture;
	Ref<Shader> blade_shader; // Kept across rebuilds while its code is the same.
	Ref<ImageTexture> ground_litter_texture;

	// Trample field (docs/ideas/grass.md, contact): bend per texel around the camera, pressed by
	// movers and relaxing back, so blades stay stateless and trails persist.
	bool trample_enabled = true;
	StringName trample_group = "grass_trample";
	float trample_size = 32.0f;
	int trample_resolution = 128;
	float trample_radius = 0.4f;
	float trample_recovery = 6.0f; // Half-life in seconds.
	LocalVector<Vector2> trample; // World xz bend per texel, length 0..1.
	Vector2i trample_origin; // World texel of the map's first texel.
	bool trample_active = false;
	Ref<Image> trample_image;
	Ref<ImageTexture> trample_texture;
	HashMap<ObjectID, Vector3> trample_last;
	WaterCurrentBend water_current; // the current in water over the sward

	Crop crop = CROP_NONE;
	float crop_stage = 75.0f; // BBCH (Zadoks) decimal code: 10 first leaf, 30 stem elongation, 51 heading, 75 milk, 87 hard dough, 99 harvested (stubble).
	float row_spacing = 0.125f;
	float drill_angle = 0.0f; // The rows' direction, radians from +x towards +z.
	Ref<ArrayMesh> crop_mesh_far; // Fewer parts for the coarse levels.
	// Fields (set_fields()): polygons each with its own drill angle, stage or sowing calendar,
	// tramlines and headland.
	struct CropPlot {
		PackedVector2Array polygon;
		Rect2 bounds;
		bool clockwise = false;
		float drill_angle = 0.0f;
		float row_spacing = 0.125f;
		float stage = -1.0f; // Fixed stage; below 0 the node's crop_stage.
		int sown_day = -1; // Day of the year sown; with a season_source the stage follows the calendar.
		int harvest_day = -1;
		float tramline_spacing = 24.0f;
		float tramline_gauge = 1.8f;
		float headland = 12.0f;
		float current = 75.0f; // The stage now.
	};
	static bool _crop_swath(const CropPlot &p_pl, float &r_pitch, float &r_half);
	static float crop_canopy_height(Crop p_crop, float p_stage);
	Vector4 crop_view_published;
	Vector4 crop_sun_published;
	Color crop_lobe1_published;
	Color crop_lobe2_published;
	void _publish_crop_sun();
	mutable Ref<ImageTexture> crop_stage_texture; // A texel a plot: its stage (shader crop_plot_stage).
	Array fields;
	// Hedgerows round the fields (crop nodes): TreeGenerator's hedge segments laid end to end along
	// each field's edge, hedge_offset metres out (past the verge), one gateway gap per field.
	bool hedges = true;
	float hedge_offset = 3.5f;
	bool hedge_voxel_lods = true; // Voxel shells past 30 m (VegetationVoxelLod).
	static inline HashMap<String, Ref<Mesh>> hedge_shell_cache;
	LocalVector<Node3D *> hedge_nodes;
	LocalVector<CropFieldMask::Hedge> hedge_runs; // World xz, for the mask.
	int hedge_day = -1000;
	bool hedges_queued = false;
	void _queue_hedges();
	void _build_hedges();
	void _free_hedges();
	LocalVector<CropPlot> plots;
	int crop_day = -1; // The season's day the plot stages were set for.
	void _update_plot_stages(int p_day);
	static LocalVector<GrassField3D *> crop_nodes; // Crop nodes in the tree, sharing the fields' ground.
	static bool crop_ground_dirty;
	static bool crop_plots_dirty;
	static Ref<ImageTexture> crop_ground_texture;
	static Ref<ImageTexture> crop_plots_texture;
	void _crop_ground_step();
	void _crop_fields_changed_notify();
	CropFieldMask crop_mask; // The other nodes' fields, as this meadow last built against.
	struct CropShape {
		float height; // Stem to the ear's base, ripe (m).
		float ear_length;
		float ear_width; // Half width.
		float awn_length;
		int awns;
		int branches; // Panicle branches (oats); 0: a spike.
		float leaf_length;
		float leaf_width; // Half width.
		float nod; // How far the ripe ear bends over (0 upright, 1 hanging).
		Color green;
		Color ripe;
		Color straw;
	};
	static CropShape _crop_shape(Crop p_crop);
	Ref<ArrayMesh> _make_crop_mesh(bool p_far) const;
	void _crop_uniforms(const Ref<ShaderMaterial> &p_material) const;
	template <typename F>
	void _crop_rows(const Rect2 &p_rect, float p_keep, F &&p_place, bool p_fields = true) const;

	bool gpu_fill = true; // Fill tiles with the compute shader (stage 3a) when a RenderingDevice exists.
	Ref<Image> height_image; // The data map's height channel, for the GPU fill.
	Rect2 height_rect;

	HashMap<Vector2i, Tile> tiles;
	// Far rings (docs/ideas/grass.md, "Far grass"): past the near tiles, one set of blades out to far_ring_range,
	// thinned with the square of the distance in the shader and widened to keep their cover, on a quadtree of
	// tiles (key: x, z in tiles of RING_TILE << level, level) so each holds about the same number of blades. Only
	// the widest meadow field (the fade owner) has them.
	bool far_rings = true;
	float far_ring_range = 250.0f;
	struct RingTile {
		MultiMeshInstance3D *mmi = nullptr;
		uint32_t generation = 0;
		float built_near = 0.0f; // Holds every blade kept at this distance or farther.
	};
	HashMap<Vector3i, RingTile> ring_tiles;
	Ref<ArrayMesh> ring_mesh_near; // Two segments.
	Ref<ArrayMesh> ring_mesh_far; // One: a triangle.
	Vector4 ring_band; // Shader: start (all kept), end, end band, widest widening.
	Ref<Texture2D> ground_data_tex; // The data map's texture, for the far blades' ground normal.
	Vector4 ground_data_rect;
	int ring_blade_count = 0;
	double ring_build_msec = 0.0; // The last frame's ring building.
	bool _rings_active() const;
	void _update_rings(const Vector2 &p_center, TerrainDataMap3D *p_map);
	void _build_ring_tile(const Vector3i &p_key, float p_near, TerrainDataMap3D *p_map, RingTile &r_tile);
	void _fill_ring_tile_gpu(const Vector3i &p_key, const Rect2 &p_rect, TerrainDataMap3D *p_map, float p_cell, float p_width, float p_step, int p_n, const LocalVector<float> &p_suit, const LocalVector<float> &p_light, const LocalVector<float> &p_cand, RingTile &r_tile);
	void _free_ring_tile(RingTile &p_tile);
	void _free_rings();
	Ref<ArrayMesh> _make_ring_mesh(int p_segments) const;
	// Far shell (docs/ideas/grass.md, "Far grass"): past the rings, the sward as voxel columns standing on the
	// ground (VegetationVoxelLod's idea, the hedges' and VegetationField3D.far_volume's), rising out of it over the
	// rings' end band and sinking back by far_shell_range. Tiles of SHELL_TILE metres, keyed by tile.
	bool far_shell = true;
	float far_shell_range = 500.0f;

	// Burnt ground (WorldFire.burn_age_at): the sward is gone on a fresh burn and grows back, short
	// blades first, over burn_recovery years (the rules' own burn_recovery when they set one).
	// Sampled once a bake on a grid over the map (cell at least 2 m), like VegetationField3D.
	float burn_recovery = 0.7f;
	LocalVector<float> burn_ages;
	Rect2 burn_rect;
	int burn_nx = 0;
	int burn_ny = 0;
	// A live fire (or a scar placed, moved or aged) reports where the ages changed (WorldFire.burn_changed): the
	// grid is sampled again there and only the tiles, rings, shell and ground tint over it rebuild, at most every
	// BURN_FOLLOW_MSEC.
	uint64_t burn_seen = 0; // WorldFire.get_burn_generation() the grid holds
	bool burn_read = false; // The grid was read since the last rebuild().
	uint64_t burn_follow_usec = 0;
	// Desert sand seas (TerrainGenerator3D sand sea): the sward keeps off loose sand as it deepens from 5 to 30 cm,
	// like VegetationField3D's non-sand species, unless the rules are a sand plant's (dune, coast, sand mobility).
	LocalVector<float> sea_depth;
	Rect2 sea_rect;
	int sea_nx = 0;
	int sea_ny = 0;
	// Oases at desert springs (DesertSprings, as VegetationField3D's RULE_SPRING): the sward comes back onto the sand
	// where the wetness passes 0.5 to 0.85.
	DesertSprings springs;
	void _read_sand_sea(TerrainDataMap3D *p_map);
	void _read_burn_age(TerrainDataMap3D *p_map);
	void _follow_burns(TerrainDataMap3D *p_map);
	void _burn_dirty(const Rect2 &p_area);
	Ref<ImageTexture> burn_flush_texture;
	void _publish_burn_flush();
	float _burn_cover(const PlantSpecies *p_rules, float p_x, float p_z) const;
	// Deer lawns (WorldStand.browse_at; docs/ideas/vegetation.md, "Deer browsing"): where deer browse hard the sward is
	// grazed to graze_height (0: off), in the shader from a grid of the pressure over the map (graze_map), so near
	// tiles, rings and the path tracer agree without a rebuild. Followed when the stand changes.
	float graze_height = 0.04f;
	// Drylands (WorldContracts::bare_patch): no sward on the climate's bare soil patches, which the terrain material
	// draws from the same pattern. In the shaders, so it costs no rebuild.
	bool climate_bare = true;
	Ref<ImageTexture> graze_texture;
	Rect2 graze_rect;
	int graze_nx = 0;
	int graze_ny = 0;
	uint64_t graze_seen = 0; // WorldStand.get_stand_generation() the texture holds.
	Array world_trails; // WorldStand.get_trails() as last read: worn with paths.
	uint64_t trails_seen = 0;
	uint64_t trails_follow_usec = 0;
	bool _read_world_trails();
	// Mounds (PlantGenerator.make_mounds: ant hills, molehills, wood ant nests) placed by the undergrowth or canopy
	// field: their species' "sward_mound" radius keeps blades from under them (the mound carries its own turf).
	PackedVector4Array mound_obstacles;
	uint64_t mounds_follow_usec = 0;
	bool _follow_mounds();
	void _stale_tiles_over(const PackedVector4Array &p_discs);
	// Storm lodging (docs/ideas/vegetation.md, "Storm lodging"): where gusts on wet, tall sward pass the stems' base
	// strength the blades go down in swirled patches along the wind and stand again over days. A grid of cells round
	// the camera (lodge_cell m, LODGE_N cells a side) reads WorldWeather wind, rain and wetness a few rows a frame;
	// the shader reads it as lodge_map (r, g: direction * 0.5 + 0.5, b: amount).
	static constexpr int LODGE_N = 96;
	static constexpr float LODGE_CELL = 4.0f;
	static constexpr int LODGE_ROWS_PER_FRAME = 8;
	bool lodging = true;
	bool fairy_rings = true; // Meadow fairy rings (fairy_rings.h) in the blades; crops never.
	bool deer_beds = true; // Deer beds (deer_beds.h) in the blades; crops never.
	bool terracettes = true; // Worn treads across steep slopes in the blades; crops never.
	float deer_bed_day = 180.0f; // The season's day the beds' ages count from (set by the season step).
	bool snowmelt = true; // The sward after a long snow lie: matted, pale, snow mould patches (the season's snowpack by height).
	int melt_day = -2; // The day the band table was made for.
	float melt_h0 = 0.0f; // m, the first band's height
	void _update_snowmelt(const Dictionary &p_season);
	// Under the last flood's mark (WorldWater.high_water_at): blades combed flat downstream and silted, sharp at the
	// crest. A FLOOD_N grid of FLOOD_CELL m round the camera, as flood_map (r peak m, g days since, b flow angle, a known).
	static constexpr int FLOOD_N = 64;
	static constexpr float FLOOD_CELL = 6.0f;
	bool flood_wash = true;
	bool flood_any = false;
	Vector2i flood_origin;
	uint64_t flood_usec = 0;
	Ref<ImageTexture> flood_texture;
	void _flood_step(const Vector3 &p_center, bool p_force);
	Vector2i lodge_origin; // Cell of the grid's first corner.
	bool lodge_placed = false;
	LocalVector<float> lodge_amt;
	LocalVector<Vector2> lodge_dir;
	LocalVector<double> lodge_row_time; // lodge_clock the row was last stepped at.
	double lodge_clock = 0.0; // Weather seconds since the grid was placed.
	int lodge_row = 0;
	int lodge_any = 0; // Cells lodged at the last sweep.
	int lodge_counting = 0; // Cells lodged so far in this sweep.
	bool lodge_calm = false; // The last sweep's probe found no wind near lodging.
	bool lodge_dirty = false;
	Ref<ImageTexture> lodge_texture;
	float _lodge_height() const;
	void _lodge_place(const Vector3 &p_center);
	void _lodge_step_cell(int p_i, const Vector3 &p_pos, float p_dt, const WorldWeather *p_weather, float p_ucrit, bool p_crop);
	void _lodge_rows(int p_rows, const Vector3 &p_center);
	void _lodge_upload();
	// Hay meadows (hay_meadows; docs/ideas/vegetation.md, "Mowing and hay rows"): polygons mown on a day of the year
	// (or a fixed number of days ago). In the shader: stubble regrowing, the cut grass lying in swaths, tedded, raked
	// into windrows and carted, from a map of which meadow each 0.5 m texel is in (mow_map) and a texel a meadow of its
	// state (mow_state: days since mown, row angle), so the days pass without a rebuild.
	struct HayMeadow {
		PackedVector2Array polygon;
		Rect2 bounds;
		float row_angle = 0.0f; // Radians: the mower's passes and the rows along world (cos, sin) in xz.
		int mow_day = -1; // Day of the year mown; with a season_source the days since follow the calendar.
		float days = -1.0f; // Fixed days since mown (>= 0); below 0 the calendar.
		float current = -1.0f; // Days since mown now; below 0 not mown (or long grown back).
		float rain = 0.0f; // mm of rain on the cut grass while it lay (from WorldWeather.rainfall_at; a fixed "rain" key)
		float rain_fixed = -1.0f; // The meadow's own "rain" (mm, >= 0) instead of the weather's
		float rain_sent = 0.0f; // rain as last written to mow_state
	};
	uint64_t hay_rain_usec = 0; // When the rain on the hay was last added up
	void _hay_rain_step();
	static constexpr int MAX_HAY_MEADOWS = 64;
	static constexpr float HAY_REACH = 3.2f; // m a cut blade can be gathered sideways into its row (half the widest pitch).
	Array hay_meadows;
	LocalVector<HayMeadow> meadows;
	int hay_day = -1;
	Ref<ImageTexture> mow_texture;
	Ref<ImageTexture> mow_state_texture;
	Rect2 mow_rect;
	void _build_mow_map();
	void _update_mow_state(int p_day);
	void _publish_mow();
	bool mow_published = false; // The ground field handed grass_mow_map to the ground shader.
	float _hay_reach() const { return meadows.is_empty() ? 0.0f : HAY_REACH; }
	static LocalVector<GrassField3D *> hay_nodes; // In the tree with hay meadows (mown_days_at).
	static uint64_t hay_generation;
	void _hay_register();
	// Round bales (PlantGenerator.make_hay_bale) along the windrows once baled (days 3.5 to 5), one MultiMesh a variant;
	// a meadow set on several layers of a sward gets its bales from the first of them.
	static constexpr float HAY_YIELD = 0.5f; // kg of hay a square metre (an estimate for a meadow's first cut).
	static constexpr float HAY_ROW_PITCH = 6.0f; // m between windrows (the shader's).
	LocalVector<MultiMeshInstance3D *> bale_nodes;
	static inline LocalVector<Ref<ArrayMesh>> bale_meshes;
	bool bales_queued = false;
	int bale_count = 0;
	void _queue_bales();
	void _build_bales();
	void _free_bales();
	bool graze_any = false;
	bool graze_published = false; // The ground field handed grass_graze_map to the ground shader.
	void _read_graze(TerrainDataMap3D *p_map);
	void _publish_graze();
	struct ShellTile {
		MeshInstance3D *mi = nullptr;
		uint32_t generation = 0;
		int cells = 0; // Columns a side.
	};
	HashMap<Vector2i, ShellTile> shell_tiles;
	Ref<ShaderMaterial> shell_material;
	int shell_column_count = 0;
	void _update_shell(const Vector2 &p_center, TerrainDataMap3D *p_map);
	void _build_shell_tile(const Vector2i &p_key, int p_cells, TerrainDataMap3D *p_map, ShellTile &r_tile);
	void _free_shell_tile(ShellTile &p_tile);
	void _free_shell();
	float _shell_column(int p_x, int p_z, float p_cell, const PlantSpecies *p_rules, TerrainDataMap3D *p_map) const;
	Ref<ArrayMesh> blade_mesh;
	Ref<ShaderMaterial> material;
	int blade_count = 0;
	double build_msec = 0.0;

	// Far colour bake (docs/ideas/vegetation-lod-streaming.md, section 4): a patch of these blades
	// rendered in a private world, then a white ground under the same light; their ratio is the
	// colour the far ground takes. Runs once each time the blades' look changes.
	class SubViewport *far_bake_vp = nullptr;
	class DirectionalLight3D *far_bake_sun = nullptr;
	MultiMeshInstance3D *far_bake_blades = nullptr;
	Ref<StandardMaterial3D> far_bake_ground_mat;
	int far_bake_state = -1;
	int far_bake_wait = 0;
	float far_bake_sun_energy = 1.0f; // The scene sun's energy the far colour was baked under.
	bool far_bake_queued = false;
	bool far_baked = false;
	bool far_bake = true; // Off: no bake, the far ground keeps the hand-matched colour (slow software-rendered scouts).
	// Mean of each view, per sun elevation: sward at the four sun azimuths, white, black at the four.
	static constexpr int FAR_ELEVATIONS = 4;
	static constexpr int FAR_REF_ELEVATION = 1; // 30 degrees: get_far_color() and the cover map's colour.
	Color far_bake_view[9 * FAR_ELEVATIONS];
	Color far_color_baked;
	// The far colour's change with the sun's azimuth from the view (phi): factor 1 + lobe1 cos(phi)
	// + lobe2 cos(2 phi) per channel, fitted to the four baked azimuths (canopy BRDF hot spot).
	Color far_lobe1;
	Color far_lobe2;
	// The same at each baked sun elevation: a sward of upright blades takes less of a high sun than
	// flat ground does, so its matching albedo falls as the sun climbs.
	Color far_elev_color[FAR_ELEVATIONS];
	Color far_elev_lobe1[FAR_ELEVATIONS];
	Color far_elev_lobe2[FAR_ELEVATIONS];
	int far_elev_count = 0; // How many elevations baked (0: only the reference, as before).
	float sun_elevation_deg = 30.0f; // The sun's elevation as last published.
	float published_far_elevation = -1000.0f;
	void _far_at_elevation(float p_deg, Color &r_color, Color &r_lobe1, Color &r_lobe2) const;
	NodePath sun_light; // The DirectionalLight3D whose azimuth turns the far colour; empty: the first in the scene.
	ObjectID sun_cache;
	int sun_search_wait = 0;
	Vector4 published_sun;
	Vector4 published_view;
	class DirectionalLight3D *_get_sun();
	void _publish_sun_view(const Vector3 &p_camera);
	void _publish_far();
	float _cover_share() const;
	void _queue_far_bake();
	void _far_bake_start();
	void _far_bake_step();
	void _far_bake_free();

	// Live-view match (far_match_view): reads the frame back (async, no stall) at pairs of ground
	// points straddling the fade band in the same direction from the camera, and scales the far
	// colour until the full blades and the far ground beyond agree on screen.
	bool far_match_view = true;
	Color far_view_scale = Color(1, 1, 1, 1);
	int far_view_state = 0; // 0 idle, 1 waiting for frames, 2 readback in flight.
	int far_view_wait = 0;
	int far_view_round = 0;
	int far_view_misses = 0;
	float far_view_elevation = -1000.0f; // The sun elevation it last matched under.
	Vector3 far_view_camera; // Where it last matched from.
	Vector2 far_view_size; // The viewport's visible size when the pixels were picked.
	PackedVector2Array far_view_px; // Pairs: inside the band, past it.
	LocalVector<int> far_view_pair_slot; // Each pair's field slot (crop_ground_plots), crops only.
	HashMap<int, Color> far_view_slot_ratio; // Per field slot: blades / far ground this round.
	// Per field slot: its far canopy's own scale on top of crop_ground_view (fields of different
	// crops and stages differ; one global scale left a barley field's far half dark).
	static inline HashMap<int, Color> crop_slot_view;
	double far_view_msec = 0.0; // The last readback's sampling cost on the thread it lands on.
	Color far_view_ratio = Color(1, 1, 1, 1);
	int far_view_pairs = 0;
	// The same match for a crop's fields (the first crop node): its tillers just inside their band
	// against the far canopy past it, in the same field; per channel, as crop_ground_view.
	Color crop_view_scale = Color(1, 1, 1, 1);
	bool _is_crop_view() const { return crop != CROP_NONE && !crop_nodes.is_empty() && crop_nodes[0] == this; }
	void _far_view_step(class Camera3D *p_cam);
	void _far_view_start(int p_wait);
	void _far_view_issue(RID p_viewport_texture);
	void _far_view_on_data(const PackedByteArray &p_data, int p_width, int p_height, int p_format);
	void _far_view_apply(const Color &p_ratio, int p_pairs);
	Color _far_color_hand() const;

	void _ensure_resources();
	void _build_material();
	int _lod_for(float p_distance) const;
	void _build_tile(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map);
	void _build_tile_gpu(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map, const PlantSpecies *p_rules, const class VegetationField3D *p_canopy, const LocalVector<float> &p_light, const LocalVector<float> &p_sward, const LocalVector<float> &p_wear, int p_lg, float p_width);
	void _free_tile(Tile &p_tile);
	void _update_fade(const Vector2 &p_center);
	void _update_tiles(const Vector3 &p_center);
	void _bake_ground(TerrainDataMap3D *p_map);
	// The ground tint's map is baked a few rows a frame after a change (a 1024 map is a million
	// suitability samples), the old one shown until the new one is done; the first bake is whole.
	struct GroundJob {
		bool active = false;
		int res = 0;
		int row = 0;
		Vector4 rect;
		Vector<uint8_t> data;
		Vector<uint8_t> ldata;
		bool any_litter = false;
	} ground_job;
	static constexpr uint64_t GROUND_BAKE_BUDGET_USEC = 1500;
	void _ground_bake_rows(TerrainDataMap3D *p_map, uint64_t p_budget_usec);
	void _ground_bake_publish(const Vector4 &p_rect, const Ref<Image> &p_img, const Ref<Image> &p_litter);
	void _trample_recenter(const Vector3 &p_center);
	void _trample_step(double p_delta);
	void _trample_upload();
	Vector2 _mover_extent(Node3D *p_node) const; // Radius and half height.

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_gpu_fill(bool p_enabled);
	bool is_gpu_fill() const { return gpu_fill; }
	static bool is_gpu_fill_available();
	static void free_gpu_fill();
	void set_data_map(const NodePath &p_path);
	NodePath get_data_map() const { return data_map; }
	void set_canopy_field(const NodePath &p_path);
	NodePath get_canopy_field() const { return canopy_field; }
	void set_undergrowth_field(const NodePath &p_path);
	NodePath get_undergrowth_field() const { return undergrowth_field; }
	void set_obstacles(const PackedVector4Array &p_obstacles);
	void set_paths(const Array &p_paths);
	Array get_paths() const { return paths; }
	void set_trample_resistance(float p_passes);
	float get_trample_resistance() const { return trample_resistance; }
	static constexpr float OBSTACLE_MARGIN = 0.02f; // Blades root this far off a rock's foot.
	PackedVector4Array get_obstacles() const { return obstacles; }
	void set_rock_field(const NodePath &p_path);
	NodePath get_rock_field() const { return rock_field; }
	void set_rock_obstacle_min(float p_size);
	float get_rock_obstacle_min() const { return rock_obstacle_min; }
	void set_rock_footprint_max(float p_radius);
	float get_rock_footprint_max() const { return rock_footprint_max; }
	int get_rock_chunk_count() const { return rock_cache.chunks.size(); }
	void set_rules(const Ref<PlantSpecies> &p_rules);
	Ref<PlantSpecies> get_rules() const { return rules; }
	void set_density(float p_density);
	float get_density() const { return density; }
	void set_blade_height(const Vector2 &p_height);
	Vector2 get_blade_height() const { return blade_height; }
	void set_blade_width(float p_width);
	float get_blade_width() const { return blade_width; }
	void set_clump_size(float p_size);
	float get_clump_size() const { return clump_size; }
	void set_lean(float p_lean);
	float get_lean() const { return lean; }
	Color get_far_color() const;
	bool is_far_color_baked() const { return far_baked; }
	void set_far_bake(bool p_enabled);
	bool is_far_bake() const { return far_bake; }
	PackedColorArray get_far_color_lobes() const;
	Dictionary get_far_published() const;
	void set_far_match_view(bool p_enabled);
	bool is_far_match_view() const { return far_match_view; }
	void set_sun_light(const NodePath &p_path);
	NodePath get_sun_light() const { return sun_light; }
	void set_base_color(const Color &p_color);
	Color get_base_color() const { return base_color; }
	void set_tip_color(const Color &p_color);
	Color get_tip_color() const { return tip_color; }
	void set_dry_color(const Color &p_color);
	Color get_dry_color() const { return dry_color; }
	void set_dryness(float p_dryness);
	float get_dryness() const { return dryness; }
	void set_season_source(const NodePath &p_path);
	NodePath get_season_source() const { return season_source; }
	void set_follow_weather(bool p_on);
	bool is_follow_weather() const { return follow_weather; }
	void set_season_response(float p_response);
	float get_season_response() const { return season_response; }
	float get_season_green() const { return season_green; }
	float get_effective_dryness() const { return _dryness(); }
	static float season_greenness(const Dictionary &p_season);
	void set_color_variation(float p_amount);
	float get_color_variation() const { return color_variation; }
	void set_radius(float p_radius);
	float get_radius() const { return radius; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_fallback_wind(const Vector2 &p_wind);
	Vector2 get_fallback_wind() const { return fallback_wind; }
	void set_wind_response(float p_response);
	float get_wind_response() const { return wind_response; }

	void set_blade_segments(int p_segments);
	int get_blade_segments() const { return blade_segments; }
	void set_view_facing(float p_facing);
	float get_view_facing() const { return view_facing; }
	void set_still_shot(bool p_enabled);
	bool is_still_shot() const { return still_shot; }
	bool is_still_frozen() const { return still_frozen; }
	void set_fibre_shading(float p_amount);
	float get_fibre_shading() const { return fibre_shading; }
	void set_ground_tint(const Color &p_color);
	Color get_ground_tint() const { return ground_tint; }
	void set_ground_tint_enabled(bool p_enabled);
	bool is_ground_tint_enabled() const { return ground_tint_enabled; }
	static String get_ground_shader_code();
	// grass_ground_opened(xz): what the ground's ruts, digs and puddles do to plants there (mud round 5): x lays them
	// flat, y takes them away, z the ground's drop (m). Shared with PlantGenerator's plant shader.
	static String get_ground_opened_shader_code();
	// The camera cut-out round the player (BaseMaterial3D.get_camera_cutout_shader_code()) put into a vegetation shader:
	// its functions before the first function and, first in every fragment(), the discard outside the shadow pass.
	static String add_camera_cutout(const String &p_code);

	void set_trample_enabled(bool p_enabled);
	bool is_trample_enabled() const { return trample_enabled; }
	void set_trample_group(const StringName &p_group) { trample_group = p_group; }
	StringName get_trample_group() const { return trample_group; }
	void set_trample_size(float p_size);
	float get_trample_size() const { return trample_size; }
	void set_trample_resolution(int p_resolution);
	int get_trample_resolution() const { return trample_resolution; }
	void set_trample_radius(float p_radius) { trample_radius = MAX(p_radius, 0.01f); }
	float get_trample_radius() const { return trample_radius; }
	void set_trample_recovery(float p_seconds) { trample_recovery = MAX(p_seconds, 0.0f); }
	float get_trample_recovery() const { return trample_recovery; }

	void add_trample(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity = Vector3(), float p_strength = 1.0f);
	Vector2 get_trample_at(const Vector3 &p_position) const;
	void clear_trample();

	void set_crop(Crop p_crop);
	Crop get_crop() const { return crop; }
	void set_crop_stage(float p_stage);
	float get_crop_stage() const { return crop_stage; }
	void set_row_spacing(float p_spacing);
	float get_row_spacing() const { return row_spacing; }
	static constexpr int MAX_PLOTS = 63;
	void set_fields(const Array &p_fields);
	void set_hay_meadows(const Array &p_meadows);
	Array get_hay_meadows() const { return hay_meadows; }
	float get_mown_days_at(const Vector3 &p_position) const;
	// Days since the ground was mown, over every GrassField3D in the tree with hay meadows (-1: not in a mown meadow);
	// the generation changes whenever a meadow, or its days, does (VegetationField3D follows it).
	static float mown_days_at(const Vector3 &p_position);
	static uint64_t get_hay_generation() { return hay_generation; }
	static bool has_hay_meadows() { return !hay_nodes.is_empty(); }
	int get_hay_bale_count() const { return bale_count; }
	void set_hedges(bool p_enabled);
	bool get_hedges() const { return hedges; }
	void set_hedge_offset(float p_offset);
	float get_hedge_offset() const { return hedge_offset; }
	void set_hedge_voxel_lods(bool p_enabled);
	bool get_hedge_voxel_lods() const { return hedge_voxel_lods; }
	int get_hedge_segment_count() const;
	static Ref<ArrayMesh> make_voxel_shell(const Ref<Mesh> &p_mesh, float p_voxel);
	Array get_fields() const { return fields; }
	float get_field_stage(int p_index) const;
	static float crop_calendar_stage(int p_sown_day, int p_harvest_day, int p_day);
	static Color crop_canopy_look(Crop p_crop, float p_stage);
	static CropFieldMask get_crop_field_mask();
	void _on_crop_fields_changed();
	void set_drill_angle(float p_angle);
	float get_drill_angle() const { return drill_angle; }

	void rebuild();
	void update_at(const Vector3 &p_center);
	int get_blade_count() const { return blade_count; }
	int get_tile_count() const { return tiles.size(); }
	void set_far_rings(bool p_enabled);
	bool is_far_rings() const { return far_rings; }
	void set_far_ring_range(float p_range);
	float get_far_ring_range() const { return far_ring_range; }
	int get_ring_blade_count() const { return ring_blade_count; }
	int get_ring_tile_count() const { return ring_tiles.size(); }
	double get_ring_build_msec() const { return ring_build_msec; }
	void set_far_shell(bool p_enabled);
	bool is_far_shell() const { return far_shell; }
	void set_far_shell_range(float p_range);
	float get_far_shell_range() const { return far_shell_range; }
	void set_burn_recovery(float p_years);
	void set_graze_height(float p_m);
	void set_climate_bare(bool p_on);
	bool is_climate_bare() const { return climate_bare; }
	void set_lodging(bool p_on);
	void set_snowmelt(bool p_on);
	bool is_snowmelt() const { return snowmelt; }
	// The last lasting snow lie at a height on a day (the season's degree-day snowpack, WeatherSeason): x the days it
	// lay, y the days since it melted out (-1 snow still lies, or none lasted this season). Needs the weather's climate.
	static Vector2 snow_lie(const WeatherSeason::Climate &p_climate, float p_height, int p_day);
	// At a point: x days the snow lay, y days since it melted, z the sward's matting (0..1) there, as the blades show it
	// before their own patchiness; p_day -1: today.
	Vector3 get_snowmelt_at(const Vector3 &p_position, int p_day = -1) const;
	void set_flood_wash(bool p_on);
	bool is_flood_wash() const { return flood_wash; }
	// Under the last flood's mark at the point: x combed flat (0-1), y silted (0-1), z days since the peak (-1 none).
	Vector3 get_flood_wash_at(const Vector3 &p_position) const;
	void update_flood_map(const Vector3 &p_center) { _flood_step(p_center, true); } // Read the mark again now.
	void set_fairy_rings(bool p_on);
	bool is_fairy_rings() const { return fairy_rings; }
	void set_terracettes(bool p_on);
	bool is_terracettes() const { return terracettes; }
	void set_deer_beds(bool p_on);
	bool is_deer_beds() const { return deer_beds; }
	void set_deer_bed_day(float p_day);
	float get_deer_bed_day() const { return deer_bed_day; }
	// The deer bed at a point on day p_day (deer_beds.h): x how flat the grass lies (0 none), yz the way it lies, w the
	// bed's length (m).
	static Vector4 get_deer_bed_at(const Vector3 &p_global_position, float p_day);
	// The fairy ring bands at a point (fairy_rings.h): x the dark front, y the dead band, z the inner dark band.
	static Vector3 get_fairy_ring_at(const Vector3 &p_global_position);
	bool is_lodging() const { return lodging; }
	// Lodged share (0..1) and the direction the blades lie, as Vector3(amount, dir.x, dir.z); zero outside the grid.
	Vector3 get_lodging_at(const Vector3 &p_global_position) const;
	// Steps the whole grid by this many weather seconds at once (time skips, tests), reading the weather now.
	void advance_lodging(double p_weather_seconds);
	float get_graze_height() const { return graze_height; }
	float get_burn_recovery() const { return burn_recovery; }
	float get_burn_cover_at(const Vector3 &p_global_position) const;
	int get_shell_column_count() const { return shell_column_count; }
	int get_shell_tile_count() const { return shell_tiles.size(); }
	double get_build_time_msec() const { return build_msec; }
	Ref<ShaderMaterial> get_material() const { return material; }

	~GrassField3D();
};

VARIANT_ENUM_CAST(GrassField3D::Crop);

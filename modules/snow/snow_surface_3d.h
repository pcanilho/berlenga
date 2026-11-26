/**************************************************************************/
/*  snow_surface_3d.h                                                     */
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


#pragma once

#include "core/templates/hash_map.h"
#include "modules/ground/ground_deform_field.h"
#include "modules/ground/ground_deform_gpu.h"
#include "scene/resources/texture_rd.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class CollisionShape3D;
class MeshInstance3D;
class SnowCover3D;
class SnowClumps3D;
class StaticBody3D;

// Deep snow as geometry (docs/ideas/snow.md, stage 2). A camera-following grid, dense near the
// middle and coarser outward, is lifted in its vertex shader to the ground plus the SnowCover3D
// pack depth, so the snow has real volume in raster and under path tracing (displaced surfaces
// are traced from their capture, docs/ideas/displaced-hit-varyings.md). Thin snow (under
// min_depth) stays a material effect and the surface tucks under the ground there.
//
// Deformation, after Batman: Arkham Origins (GDC 2014), Rise of the Tomb Raider (SIGGRAPH 2015)
// and God of War Ragnarok (GDC 2023): a camera-following field of dent and rim heights. Unlike
// those, the field is mass conserving: a pressed column keeps (1 - rim_share) of what it loses
// as compaction, up to compacted_density, and pushes rim_share sideways into a rim around the
// contact. Bodies sink by Bekker's pressure-sinkage relation z = (p / (kc / b + kphi))^(1/n)
// (Bekker 1969, snow values from Wong, Theory of Ground Vehicles), and a heightfield collision
// patch follows the deformed surface so they rest in their own tracks. Trails refill as new
// snow settles into hollows and wind drifts over them, and shrink as the pack around them
// settles toward the compacted density. Tiles leaving the field are kept in a bounded cache and
// come back when the camera returns.
class SnowSurface3D : public Node3D {
	GDCLASS(SnowSurface3D, Node3D);

	static SnowSurface3D *active; // the last to enter the tree: the snow contract reads its surface

	static constexpr int TILE = GroundDeformField::TILE; // deformation texels per cached tile side

	NodePath snow_cover;
	NodePath follow_target;
	float extent = 96.0f;
	float detail_spacing = 0.125f;
	float max_spacing = 2.0f;
	float ground_spacing = 0.25f;
	float min_depth = 0.03f;
	float edge_fade = 8.0f;
	float micro_relief = 1.0f;
	Color tint = Color(1, 1, 1);

	bool deform_enabled = true;
	float deform_extent = 32.0f;
	int deform_resolution = 512;
	PackedStringArray deform_groups;
	float compacted_density = 450.0f; // kg/m3 under a boot or a wheel
	float rim_share = 0.3f;
	float contact_tolerance = 0.03f;
	float bekker_n = 1.6f;
	float bekker_kc = 4.37f; // kN/m^(n+1)
	float bekker_kphi = 196.72f; // kN/m^(n+2)
	float character_mass = 75.0f;
	float character_contact_area = 0.035f; // m2, one boot
	float refill_share = 0.5f;
	float wind_fill_rate = 0.5f; // per weather hour at full drift
	int max_cached_tiles = 1024;

	bool collision_enabled = true;
	float collision_extent = 16.0f;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	float collision_rate = 15.0f;

	// Ground, sampled from the cover's height source on a lattice.
	int ground_n = 0;
	Vector2i ground_origin; // lattice index of the first sample
	bool ground_valid = false;
	LocalVector<float> ground;
	Ref<Image> ground_image;
	Ref<ImageTexture> ground_texture;
	float ground_min = 0.0f;
	float ground_max = 0.0f;

	// Deformation field (shared with GroundSurface3D, modules/ground). On the GPU path it is the CPU copy read
	// back from the GPU field (for collision fallbacks, queries and scripted presses).
	GroundDeformField field;

	// The print field on the GPU (docs/ideas/snow.md, follow-up 13).
	bool gpu_simulation = true;
	struct GPUSink : public GroundDeformField::StampSink {
		SnowSurface3D *surface = nullptr;
		void stamp(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance, Vector2 p_velocity) override;
	};
	GPUSink gpu_sink;
	GroundDeformGPUHost gpu_host;
	bool gpu_ground_dirty = true;
	bool gpu_cover_dirty = true;
	int gpu_cover_n = 0;
	bool gpu_cover_valid = false;
	float gpu_relax_keep = 1.0f;
	bool _gpu_on() const;
	void _gpu_shift(Vector2i p_origin);
	void _gpu_submit(double p_delta, bool p_collision, bool p_sync);
	void _gpu_take();
	void _gpu_release();
	PackedByteArray _gpu_params(double p_delta, int p_cover_n, bool p_cover_valid) const;
	PackedByteArray _gpu_cover_cells(int &r_n, bool &r_valid) const;
	int _gpu_collision_cells() const;
	struct SnowColumns : public GroundDeformField::Columns {
		const SnowSurface3D *surface = nullptr;
		const SnowCover3D *cover = nullptr;
		// Set by sinkage() for the body about to press: held by the crust (only the snow over it
		// compacts), or through it (the rim heaps in plates).
		mutable bool held = false;
		mutable bool broke = false;
		GroundDeformField::Column column(float p_x, float p_z) const override;
		float sinkage(const GroundDeformField::Body &p_body) const override;
	};
	bool collision_dirty = true;
	Ref<Image> deform_image;
	Ref<ImageTexture> deform_texture;
	uint64_t seen_cover_update = 0;
	double collision_accum = 0.0;

	MeshInstance3D *mesh_instance = nullptr;
	Ref<ArrayMesh> mesh;
	Ref<ShaderMaterial> material;
	Ref<ImageTexture> cup_tile_texture;
	Ref<ImageTexture> sas_tile_texture;
	Ref<Shader> shader;
	StaticBody3D *body = nullptr;
	bool clumps = true; // SnowClumps3D: the lumps a print breaks out of cohesive snow (ground detail)
	SnowClumps3D *clumps_node = nullptr;
	// Plates of a broken crust as geometry (docs/ideas/snow.md, follow-up 38): the field's 6 cm texels round
	// each sector's ramp off, so the pieces are slabs over it with crisp edges and a lip.
	struct CrustBreak {
		Vector3 position;
		float radius = 0.0f; // the footprint's, at the crust
		float thickness = 0.0f;
		float crack = 0.0f;
		uint32_t seed = 0;
		float age = 0.0f;
		bool built = false;
		float surface = 0.0f; // the snow surface at the middle when built, to see new snow bury it
	};
	mutable LocalVector<CrustBreak> crust_breaks;
	MeshInstance3D *plates_instance = nullptr;
	Ref<StandardMaterial3D> plates_material;
	bool crust_plates = true;
	bool plates_dirty = false;
	float plates_check = 0.0f;
	void _note_break(const Vector3 &p_center, float p_radius, float p_thickness) const;
	void _crust_plates_step(double p_delta);
	void _build_plates();
	CollisionShape3D *collision_node = nullptr;
	Ref<HeightMapShape3D> collision_shape;
	Vector2 mesh_center;
	bool mesh_dirty = true;
	double step_msec = 0.0;

	SnowCover3D *_cover() const;
	Vector3 _follow_position() const;
	float _texel() const { return deform_extent / deform_resolution; }
	void _build_mesh();
	void _ensure_children();
	void _follow(Vector3 p_center);
	void _shift_ground(Vector2i p_origin);
	void _shift_deform(Vector2i p_origin);
	float _ground_at(float p_x, float p_z) const;
	float _dent_max(float p_depth, float p_density, float p_wet = 0.0f) const;
	static float rotten_share(float p_wet); // the share of the dry pack's bearing a wet one keeps
	float _press(Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, Vector2 p_velocity = Vector2());
	void _press_movers(double p_delta);
	void _relax();
	// Stones through snow (follow-up 57): melt moats round the stones standing out of the pack, windward scour and
	// lee tails, as a signed height (m) over a square round the follow target (see the .cpp).
	bool stone_relief = true;
	float stone_relief_extent = 32.0f;
	struct StoneRelief {
		bool valid = false;
		Vector2 origin; // the square's corner (x, z)
		float texel = 0.0625f;
		int n = 0;
		LocalVector<float> h; // m, n * n
		Ref<Image> image;
		Ref<ImageTexture> texture;
		Rect2 query; // the rect the stones were read for
		LocalVector<Vector4> stones; // x, z, ground, top
		LocalVector<Vector3> shape; // the footprint: half axes (m) along its own x and z, and its yaw (rad)
		uint64_t cover_update = UINT64_MAX;
		double wait = 0.0;
		int drawn = 0; // stones standing out of the pack in the last build
		PackedFloat32Array given; // set_stone_list: x, z, ground, top, half axis x, half axis z, yaw per stone (7 floats)
	} stone;
	void _stone_relief_step(double p_delta);
	void _read_stones(const Rect2 &p_rect);
	void _build_stone_relief();
	float _stone_relief_at(float p_x, float p_z) const;
	// Round 9 (follow-up 54): rut and trench walls that give way (see the .cpp).
	bool wall_collapse = true;
	double crumble_accum = 0.0;
	double crumble_hold = 0.0; // s: after a GPU upload, until the read back shows it
	uint32_t crumble_seed = 1;
	int walls_collapsed = 0;
	float collapsed_volume = 0.0f; // m3
	LocalVector<uint8_t> crumble_mark;
	LocalVector<Vector2i> crumble_pending; // world tiles seeded for the GPU field, not pulled yet
	int _crumble(double p_delta, bool p_gpu);
	void _update_collision();
	void _upload_deform();
	void _upload_ground();
	void _update_material();
	float _cover_depth_at(float p_x, float p_z, float p_ground, const SnowCover3D *p_cover) const;
	float _surface_at(float p_x, float p_z, const SnowCover3D *p_cover) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_snow_cover(const NodePath &p_path);
	NodePath get_snow_cover() const { return snow_cover; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_extent(float p_extent);
	float get_extent() const { return extent; }
	void set_detail_spacing(float p_spacing);
	float get_detail_spacing() const { return detail_spacing; }
	void set_max_spacing(float p_spacing);
	float get_max_spacing() const { return max_spacing; }
	void set_ground_spacing(float p_spacing);
	float get_ground_spacing() const { return ground_spacing; }
	void set_min_depth(float p_depth);
	float get_min_depth() const { return min_depth; }
	void set_edge_fade(float p_m);
	float get_edge_fade() const { return edge_fade; }
	void set_micro_relief(float p_scale);
	float get_micro_relief() const { return micro_relief; }
	void set_tint(const Color &p_tint);
	Color get_tint() const { return tint; }

	void set_deform_enabled(bool p_enabled);
	bool is_deform_enabled() const { return deform_enabled; }
	const GroundDeformField &get_deform_field() const { return field; } // CPU copy (read back), for GroundLumpScan
	void set_clumps(bool p_enabled);
	bool is_clumps() const { return clumps; }
	SnowClumps3D *get_clumps_node() const { return clumps_node; }
	void set_deform_extent(float p_extent);
	float get_deform_extent() const { return deform_extent; }
	void set_deform_resolution(int p_resolution);
	int get_deform_resolution() const { return deform_resolution; }
	void set_deform_groups(const PackedStringArray &p_groups) { deform_groups = p_groups; }
	PackedStringArray get_deform_groups() const { return deform_groups; }
	void set_compacted_density(float p_density) { compacted_density = CLAMP(p_density, 100.0f, 917.0f); }
	float get_compacted_density() const { return compacted_density; }
	void set_rim_share(float p_share) { rim_share = CLAMP(p_share, 0.0f, 0.9f); }
	float get_rim_share() const { return rim_share; }
	void set_contact_tolerance(float p_m) { contact_tolerance = MAX(p_m, 0.0f); }
	float get_contact_tolerance() const { return contact_tolerance; }
	void set_bekker_n(float p_n) { bekker_n = CLAMP(p_n, 0.3f, 3.0f); }
	float get_bekker_n() const { return bekker_n; }
	void set_bekker_kc(float p_k) { bekker_kc = MAX(p_k, 0.0f); }
	float get_bekker_kc() const { return bekker_kc; }
	void set_bekker_kphi(float p_k) { bekker_kphi = MAX(p_k, 0.001f); }
	float get_bekker_kphi() const { return bekker_kphi; }
	void set_character_mass(float p_kg) { character_mass = MAX(p_kg, 0.0f); }
	float get_character_mass() const { return character_mass; }
	void set_character_contact_area(float p_m2) { character_contact_area = MAX(p_m2, 0.001f); }
	float get_character_contact_area() const { return character_contact_area; }
	void set_refill_share(float p_share) { refill_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_refill_share() const { return refill_share; }
	void set_wind_fill_rate(float p_rate) { wind_fill_rate = MAX(p_rate, 0.0f); }
	float get_wind_fill_rate() const { return wind_fill_rate; }
	void set_max_cached_tiles(int p_tiles) { max_cached_tiles = MAX(p_tiles, 0); }
	int get_max_cached_tiles() const { return max_cached_tiles; }

	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_extent(float p_extent);
	float get_collision_extent() const { return collision_extent; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_rate(float p_rate) { collision_rate = MAX(p_rate, 0.5f); }
	float get_collision_rate() const { return collision_rate; }

	// Presses the snow down to p_bottom (world height of the body's underside) within p_radius of
	// p_position (a bowl for round bodies, flat for boxes), plus p_sink of Bekker sinkage below the
	// undisturbed surface. Returns the snow volume moved, m3.
	float press(Vector3 p_position, float p_radius, float p_bottom, float p_sink = 0.0f, bool p_round = true, Vector3 p_velocity = Vector3());
	// The same with a flat rectangular print p_size wide (x) and long (z), turned by p_yaw about up:
	// a boot, a sled runner, a crate.
	float press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink = 0.0f, Vector3 p_velocity = Vector3());
	static float bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi); // m
	void clear_deformation();
	// The print field runs in compute shaders where there is a RenderingDevice (Forward+ and Mobile), with
	// a copy read back for collision and queries; otherwise, or with this off, on the CPU.
	void set_gpu_simulation(bool p_enabled);
	bool is_gpu_simulation() const { return gpu_simulation; }
	bool is_gpu_active() const { return _gpu_on(); }
	// Waits for the GPU field and reads it back now (tests and tools; it stalls the GPU).
	void sync_deformation();
	void refresh_ground();

	float get_surface_height_at(Vector3 p_position) const;
	float get_dent_at(Vector3 p_position) const;
	float get_rim_at(Vector3 p_position) const;
	Vector2 get_moved_volume() const; // m3: (dent, rim) over the live field
	int get_cached_tile_count() const { return _gpu_on() ? gpu_host.cache.size() : field.cached_tile_count(); }
	Rect2 get_deform_rect() const;
	static SnowSurface3D *get_active() { return active; }
	static Ref<Image> suncup_tile(); // the periodic suncup relief (R) and its gradient (GB), made once
	static Ref<Image> sastrugi_tile(); // the periodic sastrugi relief along +x (R) and its gradient (GB), made once
	double get_step_time_msec() const { return step_msec; }
	void set_crust_plates(bool p_enabled);
	void set_stone_relief(bool p_enabled);
	bool get_stone_relief() const { return stone_relief; }
	void set_stone_relief_extent(float p_extent);
	float get_stone_relief_extent() const { return stone_relief_extent; }
	int get_stone_relief_count() const { return stone.drawn; }
	float get_stone_relief_at(Vector3 p_position) const { return _stone_relief_at(p_position.x, p_position.z); }
	void rebuild_stone_relief();
	void set_stone_list(const PackedFloat32Array &p_stones);
	PackedFloat32Array get_stone_list() const { return stone.given; }
	void set_wall_collapse(bool p_enabled) { wall_collapse = p_enabled; }
	bool is_wall_collapse() const { return wall_collapse; }
	int get_walls_collapsed() const { return walls_collapsed; }
	float get_collapsed_volume() const { return collapsed_volume; }
	int crumble(float p_delta); // one wall pass now (tests); returns the texels that gave way
	bool get_crust_plates() const { return crust_plates; }
	int get_crust_plate_count() const; // breaks drawn as plates
	Ref<ShaderMaterial> get_surface_material() const { return material; }

	SnowSurface3D();
	~SnowSurface3D();
};
